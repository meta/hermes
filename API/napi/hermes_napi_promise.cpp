/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/Callable.h"
#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/StringRefUtils.h"

//===========================================================================
// napi_env__ deferred list management
//===========================================================================

void napi_env__::addDeferred(napi_deferred__ *def) {
  def->prev_ = nullptr;
  def->next_ = deferredListHead_;
  if (deferredListHead_) {
    deferredListHead_->prev_ = def;
  }
  deferredListHead_ = def;
}

void napi_env__::removeDeferred(napi_deferred__ *def) {
  if (def->prev_) {
    def->prev_->next_ = def->next_;
  } else {
    deferredListHead_ = def->next_;
  }
  if (def->next_) {
    def->next_->prev_ = def->prev_;
  }
  def->prev_ = nullptr;
  def->next_ = nullptr;
}

void napi_env__::markDeferreds(hermes::vm::RootAcceptor &acceptor) {
  for (auto *def = deferredListHead_; def; def = def->next_) {
    acceptor.accept(def->resolve);
    acceptor.accept(def->reject);
  }
}

//===========================================================================
// Promise executor trampoline
//===========================================================================

/// Native function trampoline used as the executor passed to
/// `new Promise(executor)`. When called by the Promise constructor,
/// it receives (resolve, reject) as arguments and stores them in the
/// napi_deferred__ that was passed as the NativeFunction's context.
static hermes::vm::CallResult<hermes::vm::HermesValue>
promiseExecutorTrampoline(void *context, hermes::vm::Runtime &runtime) {
  using namespace hermes::vm;

  auto *deferred = static_cast<napi_deferred__ *>(context);
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // The Promise constructor calls executor(resolve, reject).
  deferred->resolve = args.getArg(0);
  deferred->reject = args.getArg(1);

  return HermesValue::encodeUndefinedValue();
}

//===========================================================================
// napi_create_promise
//===========================================================================

napi_status NAPI_CDECL napi_create_promise(
    napi_env env,
    napi_deferred *deferred,
    napi_value *promise) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, deferred);
  CHECK_ARG(env, promise);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  struct : public Locals {
    PinnedValue<Callable> promiseCtor;
    PinnedValue<Callable> executor;
    PinnedValue<SymbolID> promiseSym;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // GCScope is needed for internal Handle allocations by
  // getNamed_RJS, NativeFunction::create, and construct calls.
  GCScope gcScope(runtime);

  // Look up the global Promise constructor.
  auto promiseSymRes =
      runtime.getIdentifierTable().getSymbolID(runtime, ASCIIRef("Promise", 7));
  if (LLVM_UNLIKELY(promiseSymRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }
  lv.promiseSym = *promiseSymRes;

  auto promiseCtorRes = JSObject::getNamed_RJS(
      Handle<JSObject>::vmcast(toPHV(&runtime.global_)),
      runtime,
      *lv.promiseSym);
  if (LLVM_UNLIKELY(promiseCtorRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  RETURN_STATUS_IF_FALSE(
      env,
      (*promiseCtorRes)->isObject() && vmisa<Callable>(promiseCtorRes->get()),
      napi_generic_failure);

  lv.promiseCtor.castAndSetHermesValue<Callable>(promiseCtorRes->get());

  // Allocate the deferred and add it to the env's list so its
  // resolve/reject values are GC-rooted.
  auto *def = new napi_deferred__;
  env->addDeferred(def);

  // Create a NativeFunction executor that will capture resolve/reject
  // into the deferred when called by the Promise constructor.
  lv.executor = NativeFunction::create(
      runtime,
      def, // context: pointer to the deferred
      promiseExecutorTrampoline,
      Predefined::getSymbolID(Predefined::emptyString),
      2 /* paramCount: resolve, reject */);

  // Call `new Promise(executor)` using the standard construct helper.
  auto constructRes =
      Callable::executeConstruct1(lv.promiseCtor, runtime, lv.executor);
  if (LLVM_UNLIKELY(constructRes == ExecutionStatus::EXCEPTION)) {
    env->removeDeferred(def);
    delete def;
    return captureRuntimeException(env, napi_pending_exception);
  }

  *deferred = def;
  *promise = env->addToCurrentScope(constructRes->get());
  return napi_clear_last_error(env);
}

//===========================================================================
// napi_resolve_deferred / napi_reject_deferred
//===========================================================================

/// Shared implementation for resolve/reject.
static napi_status concludeDeferred(
    napi_env env,
    napi_deferred deferred,
    napi_value resolution,
    bool isResolve) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, deferred);
  CHECK_ARG(env, resolution);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;
  GCScope gcScope(runtime);

  auto *resPhv = reinterpret_cast<const PinnedHermesValue *>(resolution);

  // Get the resolve or reject function from the deferred.
  PinnedHermesValue &funcPhv = isResolve ? deferred->resolve : deferred->reject;

  if (!funcPhv.isObject() || !vmisa<Callable>(funcPhv)) {
    env->removeDeferred(deferred);
    delete deferred;
    return napi_set_last_error(env, napi_generic_failure);
  }

  ScopedNativeCallFrame newFrame{
      runtime,
      1, // argc: the resolution/rejection value
      funcPhv,
      HermesValue::encodeUndefinedValue(), // newTarget
      HermesValue::encodeUndefinedValue()}; // this
  if (LLVM_UNLIKELY(newFrame.overflowed())) {
    env->removeDeferred(deferred);
    delete deferred;
    (void)runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
    return captureRuntimeException(env, napi_pending_exception);
  }

  newFrame->getArgRef(0) = *resPhv;

  auto callRes = Callable::call(Handle<Callable>::vmcast(&funcPhv), runtime);

  // Remove and delete the deferred regardless of success/failure.
  // The deferred is consumed (one-shot).
  env->removeDeferred(deferred);
  delete deferred;

  if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_resolve_deferred(
    napi_env env,
    napi_deferred deferred,
    napi_value resolution) {
  return concludeDeferred(env, deferred, resolution, true);
}

napi_status NAPI_CDECL napi_reject_deferred(
    napi_env env,
    napi_deferred deferred,
    napi_value rejection) {
  return concludeDeferred(env, deferred, rejection, false);
}

//===========================================================================
// napi_is_promise
//===========================================================================

napi_status NAPI_CDECL
napi_is_promise(napi_env env, napi_value value, bool *is_promise) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, is_promise);

  using namespace hermes::vm;

  *is_promise = false;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(value);

  // Non-objects cannot be promises.
  if (!phv->isObject()) {
    return napi_clear_last_error(env);
  }

  Runtime &runtime = env->runtime;
  struct : public Locals {
    PinnedValue<Callable> promiseCtor;
    PinnedValue<SymbolID> promiseSym;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  GCScope gcScope(runtime);

  // Look up the global Promise constructor.
  auto promiseSymRes =
      runtime.getIdentifierTable().getSymbolID(runtime, ASCIIRef("Promise", 7));
  if (LLVM_UNLIKELY(promiseSymRes == ExecutionStatus::EXCEPTION)) {
    // If we can't look up Promise, just return false.
    runtime.clearThrownValue();
    return napi_clear_last_error(env);
  }
  lv.promiseSym = *promiseSymRes;

  auto promiseCtorRes = JSObject::getNamed_RJS(
      Handle<JSObject>::vmcast(toPHV(&runtime.global_)),
      runtime,
      *lv.promiseSym);
  if (LLVM_UNLIKELY(promiseCtorRes == ExecutionStatus::EXCEPTION)) {
    runtime.clearThrownValue();
    return napi_clear_last_error(env);
  }

  if (!(*promiseCtorRes)->isObject() ||
      !vmisa<Callable>(promiseCtorRes->get())) {
    return napi_clear_last_error(env);
  }

  // Use the instanceof operator: value instanceof Promise.
  auto valHandle = Handle<>(phv);
  lv.promiseCtor.castAndSetHermesValue<Callable>(promiseCtorRes->get());

  auto instRes = instanceOfOperator_RJS(runtime, valHandle, lv.promiseCtor);
  if (LLVM_UNLIKELY(instRes == ExecutionStatus::EXCEPTION)) {
    runtime.clearThrownValue();
    return napi_clear_last_error(env);
  }

  *is_promise = *instRes;
  return napi_clear_last_error(env);
}
