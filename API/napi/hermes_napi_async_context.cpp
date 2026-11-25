/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

using namespace hermes::vm;

//===========================================================================
// napi_async_context__ definition
//===========================================================================

/// An async context for Hermes. Since Hermes does not have Node.js's
/// async hooks system, this is a minimal struct that simply exists to
/// satisfy the API contract. The resource and resource_name parameters
/// from napi_async_init are accepted but not stored — they are only
/// meaningful for Node.js's async_hooks tracing.
struct napi_async_context__ {};

//===========================================================================
// NAPI async context API
//===========================================================================

napi_status NAPI_CDECL napi_async_init(
    napi_env env,
    napi_value async_resource,
    napi_value async_resource_name,
    napi_async_context *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, async_resource_name);
  CHECK_ARG(env, result);

  // async_resource and async_resource_name are for Node.js async hooks
  // tracing. Hermes does not have async hooks, so we accept and ignore
  // these parameters.
  (void)async_resource;
  (void)async_resource_name;

  *result = new napi_async_context__;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_async_destroy(napi_env env, napi_async_context async_context) {
  CHECK_ENV(env);
  CHECK_ARG(env, async_context);

  delete async_context;
  return napi_clear_last_error(env);
}

//===========================================================================
// napi_make_callback
//===========================================================================

napi_status NAPI_CDECL napi_make_callback(
    napi_env env,
    napi_async_context async_context,
    napi_value recv,
    napi_value func,
    size_t argc,
    const napi_value *argv,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, recv);
  if (argc > 0) {
    CHECK_ARG(env, argv);
  }

  // async_context is for Node.js async hooks tracing. Hermes does not
  // have async hooks, so it is accepted and ignored.
  (void)async_context;

  Runtime &runtime = env->runtime;

  // Validate that func is a callable.
  auto *phvFunc = reinterpret_cast<const PinnedHermesValue *>(func);
  CHECK_ARG(env, phvFunc);
  RETURN_STATUS_IF_FALSE(
      env, phvFunc->isObject() && vmisa<Callable>(*phvFunc), napi_invalid_arg);

  auto *callable = vmcast<Callable>(*phvFunc);

  auto *phvRecv = reinterpret_cast<const PinnedHermesValue *>(recv);

  // Build the call frame.
  ScopedNativeCallFrame newFrame{
      runtime,
      static_cast<uint32_t>(argc),
      HermesValue::encodeObjectValue(callable),
      HermesValue::encodeUndefinedValue(), // newTarget (not a construct)
      *phvRecv};
  if (LLVM_UNLIKELY(newFrame.overflowed())) {
    (void)runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
    return captureRuntimeException(env, napi_pending_exception);
  }

  // Copy arguments into the call frame.
  for (size_t i = 0; i < argc; ++i) {
    auto *phvArg = reinterpret_cast<const PinnedHermesValue *>(argv[i]);
    newFrame->getArgRef(i) = *phvArg;
  }

  // Execute the call.
  auto callRes = Callable::call(Handle<Callable>::vmcast(phvFunc), runtime);
  if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  // Store the result if requested.
  if (result != nullptr) {
    *result = env->addToCurrentScope(callRes->get());
  }

  return napi_clear_last_error(env);
}

//===========================================================================
// Callback scopes
//===========================================================================

/// A callback scope for Hermes. Since Hermes does not have Node.js's
/// async hooks system, this is a minimal struct that serves as a token
/// for the open/close pairing.
struct napi_callback_scope__ {};

napi_status NAPI_CDECL napi_open_callback_scope(
    napi_env env,
    napi_value resource_object,
    napi_async_context context,
    napi_callback_scope *result) {
  // Omit NAPI_PREAMBLE because V8/Node does the same — this function
  // cannot throw JS exceptions.
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  // resource_object and context are for Node.js async hooks tracing.
  // Hermes does not have async hooks, so they are accepted and ignored.
  (void)resource_object;
  (void)context;

  *result = new napi_callback_scope__;
  env->open_callback_scopes++;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_close_callback_scope(napi_env env, napi_callback_scope scope) {
  // Omit NAPI_PREAMBLE because V8/Node does the same — this function
  // cannot throw JS exceptions.
  CHECK_ENV(env);
  CHECK_ARG(env, scope);
  if (env->open_callback_scopes == 0) {
    return napi_set_last_error(env, napi_callback_scope_mismatch);
  }

  env->open_callback_scopes--;
  delete scope;
  return napi_clear_last_error(env);
}
