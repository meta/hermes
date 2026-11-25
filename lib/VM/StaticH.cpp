/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SHUnitExt.h"
#include "hermes/BCGen/SerializedLiteralParser.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/FastArray.h"
#include "hermes/VM/HiddenClass-inline.h"
#include "hermes/VM/Interpreter.h"
#include "hermes/VM/JIT/Config.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSCallableProxy.h"
#include "hermes/VM/JSGeneratorObject.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSObject-inline.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/ModuleExportsCache-inline.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/SerializedLiteralOperations.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StaticHUtils.h"
#include "hermes/VM/StringBuilder.h"

#include "JSLib/JSLibInternal.h"

#include <cstdarg>

using namespace hermes;
using namespace hermes::vm;

extern "C" void _SH_MODEL(void) {}

namespace {
/// Convert the given \p cr to a \c CallResult<HermesValue>.
template <typename T>
CallResult<HermesValue> toCallResultHermesValue(CallResult<Handle<T>> cr) {
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return cr->getHermesValue();
}

template <typename T>
CallResult<HermesValue> toCallResultHermesValue(
    CallResult<PseudoHandle<T>> cr) {
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return cr->getHermesValue();
}
} // namespace

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_throw_register_stack_overflow(SHRuntime *shr) {
  Runtime &runtime = getRuntime(shr);
  (void)runtime.raiseStackOverflow(Runtime::StackOverflowKind::JSRegisterStack);
  _sh_throw_current(shr);
}

extern "C" void _sh_check_native_stack_overflow(SHRuntime *shr) {
  Runtime &runtime = getRuntime(shr);
  bool overflowing = runtime.isStackOverflowing();
  if (LLVM_UNLIKELY(overflowing)) {
    (void)runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
    _sh_throw_current(shr);
  }
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_coerce_this_ns(
    SHRuntime *shr,
    SHLegacyValue value) {
  if (LLVM_LIKELY(_sh_ljs_is_object(value))) {
    return value;
  } else if (_sh_ljs_is_null(value) || _sh_ljs_is_undefined(value)) {
    return getRuntime(shr).getGlobal().getHermesValue();
  } else {
    CallResult<HermesValue> res{HermesValue::encodeUndefinedValue()};
    {
      Runtime &runtime = getRuntime(shr);
      GCScopeMarkerRAII marker{runtime};
      struct : public Locals {
        PinnedValue<> value;
      } lv;
      LocalsRAII lraii{runtime, &lv};
      lv.value = HermesValue::fromRaw(value.raw);
      res = toObject(runtime, lv.value);
    }
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
      _sh_throw_current(shr);
    return *res;
  }
}

static SHLegacyValue getArgumentsPropByVal_RJS(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *idx,
    SHLegacyValue *lazyReg,
    bool strictMode) {
  Runtime &runtime = getRuntime(shr);
  StackFramePtr framePtr(toPHV(frame));
  // If the arguments object hasn't been created yet and we have a
  // valid integer index, we use the fast path.
  if (toPHV(lazyReg)->isUndefined()) {
    if (auto index = toArrayIndexFastPath(*toPHV(idx))) {
      // Is this an existing argument?
      if (*index < framePtr.getArgCount())
        return framePtr.getArgRef(*index);
    }
  }
  // Slow path.
  CallResult<PseudoHandle<>> res{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    res = Interpreter::getArgumentsPropByValSlowPath_RJS(
        runtime,
        toPHV(lazyReg),
        toPHV(idx),
        framePtr.getCalleeClosureHandleUnsafe(),
        strictMode);
  }
  if (res == ExecutionStatus::EXCEPTION)
    _sh_throw_current(shr);
  return res->getHermesValue();
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_arguments_prop_by_val_loose(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *idx,
    SHLegacyValue *lazyReg) {
  return getArgumentsPropByVal_RJS(shr, frame, idx, lazyReg, false);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_arguments_prop_by_val_strict(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *idx,
    SHLegacyValue *lazyReg) {
  return getArgumentsPropByVal_RJS(shr, frame, idx, lazyReg, true);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_arguments_length(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *lazyReg) {
  Runtime &runtime = getRuntime(shr);
  StackFramePtr framePtr(toPHV(frame));
  // If the arguments object hasn't been created yet, use the fast path.
  if (toPHV(lazyReg)->isUndefined())
    return HermesValue::encodeTrustedNumberValue(framePtr.getArgCount());

  CallResult<PseudoHandle<>> res{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    // The arguments object has been created, so this is a regular property
    // get.
    auto obj = Handle<JSObject>::vmcast(toPHV(lazyReg));
    res = JSObject::getNamed_RJS(
        obj, runtime, Predefined::getSymbolID(Predefined::length));
  }
  if (res == ExecutionStatus::EXCEPTION)
    _sh_throw_current(shr);
  return res->getHermesValue();
}

static void reifyArguments(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *lazyReg,
    bool strictMode) {
  Runtime &runtime = getRuntime(shr);
  StackFramePtr framePtr(toPHV(frame));
  // If the arguments object was already created, do nothing.
  if (!toPHV(lazyReg)->isUndefined()) {
    assert(
        toPHV(lazyReg)->isObject() &&
        "arguments lazy register is not an object");
    return;
  }

  CallResult<HermesValue> res{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    res = toCallResultHermesValue(
        Interpreter::reifyArgumentsSlowPath(
            runtime, framePtr.getCalleeClosureHandleUnsafe(), strictMode));
  }
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  *lazyReg = *res;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_reify_arguments_loose(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *lazyReg) {
  reifyArguments(shr, frame, lazyReg, false);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_reify_arguments_strict(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *lazyReg) {
  reifyArguments(shr, frame, lazyReg, true);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_by_val_with_receiver_rjs(
    SHRuntime *shr,
    SHLegacyValue *source,
    SHLegacyValue *key,
    SHLegacyValue *receiver) {
  Runtime &runtime = getRuntime(shr);
  Handle<> sourceHandle{toPHV(source)};
  Handle<> keyHandle{toPHV(key)};
  Handle<> receiverHandle{(toPHV(receiver))};
  CallResult<PseudoHandle<>> res{ExecutionStatus::EXCEPTION};

  if (LLVM_UNLIKELY(!sourceHandle->isObject())) {
    // Transient object.
    {
      GCScopeMarkerRAII marker{runtime};
      res = Interpreter::getByValTransientWithReceiver_RJS(
          runtime, sourceHandle, keyHandle, receiverHandle);
    }
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
      _sh_throw_current(shr);
    return res->getHermesValue();
  }

  // Fast path for arrays and typed arrays with numeric indices
  static_assert(
      HERMESVALUE_VERSION == 2,
      "HermesValue must use NaN-encoding for non-numbers");
  uint32_t index;
  if (sh_tryfast_f64_to_u32(key->f64, index)) {
    if (auto optRes = tryFastGetComputedMayAlloc(
            runtime, vmcast<JSObject>(*toPHV(source)), index))
      return *optRes;
  }

  // Slow path for regular objects.
  {
    GCScopeMarkerRAII marker{runtime};
    res = JSObject::getComputedWithReceiver_RJS(
        Handle<JSObject>::vmcast(sourceHandle),
        runtime,
        keyHandle,
        receiverHandle);
  }
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return res->getHermesValue();
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_ljs_get_by_index_rjs(SHRuntime *shr, SHLegacyValue *source, uint32_t key) {
  Handle<> sourceHandle{toPHV(source)};
  Runtime &runtime = getRuntime(shr);
  if (LLVM_LIKELY(sourceHandle->isObject())) {
    if (auto optRes = tryFastGetComputedMayAlloc(
            runtime, vmcast<JSObject>(*sourceHandle), key))
      return *optRes;
  }

  // Otherwise...
  // This is the "slow path".
  auto res = [&]() {
    GCScopeMarkerRAII marker{runtime};
    struct : public Locals {
      PinnedValue<> key;
    } lv;
    LocalsRAII lraii{runtime, &lv};

    lv.key = HermesValue::encodeTrustedNumberValue(key);
    return Interpreter::getByValTransient_RJS(runtime, sourceHandle, lv.key);
  }();

  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return res->getHermesValue();
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_own_private_by_sym(
    SHRuntime *shr,
    const SHLegacyValue *source,
    const SHLegacyValue *privateNameKey,
    SHPrivateNameCacheEntry *privateNameCacheEntry) {
  Runtime &runtime = getRuntime(shr);
  auto res = [&]() -> CallResult<HermesValue> {
    GCScopeMarkerRAII marker{runtime};
    auto *sourcePHV = toPHV(source);
    if (LLVM_LIKELY(sourcePHV->isObject())) {
      auto *privateNameKeyPHV = toPHV(privateNameKey);
      auto *cacheEntry =
          reinterpret_cast<PrivateNameCacheEntry *>(privateNameCacheEntry);
      auto *obj = vmcast<JSObject>(*sourcePHV);
      CompressedPointer clazzPtr{obj->getClassGCPtr()};
      if (LLVM_LIKELY(
              cacheEntry && cacheEntry->clazz == clazzPtr &&
              cacheEntry->nameVal == privateNameKeyPHV->getSymbol())) {
        return JSObject::getNamedSlotValueUnsafe(obj, runtime, cacheEntry->slot)
            .unboxToHV(runtime);
      }
      return JSObject::getPrivateField(
          Handle<JSObject>::vmcast(sourcePHV),
          runtime,
          Handle<SymbolID>::vmcast(privateNameKeyPHV),
          cacheEntry);
    } else {
      return runtime.raiseTypeError(
          "cannot read private property of a non-object");
    }
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return *res;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_catch(
    SHRuntime *shr,
    SHLocals *locals,
    SHLegacyValue *frame,
    uint32_t stackSize) {
  Runtime &runtime = getRuntime(shr);
  runtime.shCurJmpBuf = runtime.shCurJmpBuf->prev;
  _sh_catch_no_pop(shr, locals, frame, stackSize);
  return _sh_get_clear_thrown_value(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_catch_no_pop(
    SHRuntime *shr,
    SHLocals *locals,
    SHLegacyValue *frame,
    uint32_t stackSize) {
  Runtime &runtime = getRuntime(shr);

  runtime.shLocals = locals;
  runtime.popToSavedStackPointer(toPHV(frame + stackSize));
  runtime.setCurrentFrame(StackFramePtr(toPHV(frame)));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_get_clear_thrown_value(SHRuntime *shr) {
  Runtime &runtime = getRuntime(shr);
  SHLegacyValue res = runtime.getThrownValue();
  runtime.clearThrownValue();
  return res;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_throw_current(SHRuntime *shr) {
  Runtime &runtime = getRuntime(shr);
  assert(runtime.shCurJmpBuf && "No SH exception handler installed");
  if (!runtime.shCurJmpBuf) {
    fprintf(stderr, "SH: uncaught exception");
    abort();
  }
  _sh_longjmp(runtime.shCurJmpBuf->buf, 1);
  // longjmp is not marked as noreturn, so use llvm_unreachable to avoid a
  // compiler warning that this function doesn't actually seem to be noreturn.
  llvm_unreachable("longjmp cannot return");
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_throw(SHRuntime *shr, SHLegacyValue value) {
  Runtime &runtime = getRuntime(shr);
  runtime.setThrownValue(HermesValue::fromRaw(value.raw));
  _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_throw_type_error(SHRuntime *shr, SHLegacyValue *message) {
  (void)getRuntime(shr).raiseTypeError(Handle<>(toPHV(message)));
  _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_throw_type_error_ascii(
    SHRuntime *shr,
    const char *message) {
  (void)getRuntime(shr).raiseTypeError(TwineChar16(message));
  _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_throw_reference_error_ascii(
    SHRuntime *shr,
    const char *message) {
  Runtime &runtime = getRuntime(shr);
  (void)runtime.raiseReferenceError(message);
  _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_throw_empty(SHRuntime *shr) {
  Runtime &runtime = getRuntime(shr);
  (void)runtime.raiseReferenceError("accessing an uninitialized variable");
  _sh_throw_current(shr);
}

static SHLegacyValue doCall(Runtime &runtime, PinnedHermesValue *callTarget) {
  if (vmisa<NativeJSFunction>(*callTarget)) {
    return NativeJSFunction::_legacyCall(
        getSHRuntime(runtime), vmcast<NativeJSFunction>(*callTarget));
  }

  if (vmisa<JSFunction>(*callTarget)) {
    JSFunction *jsFunc = vmcast<JSFunction>(*callTarget);
#if HERMESVM_JIT
    if (auto *fnPtr = jsFunc->getCodeBlock()->getJITCompiled())
      return fnPtr(&runtime);
#elif !defined(HERMESVM_JIT)
#error JIT/Config.h has not been included
#endif
    CallResult<HermesValue> result = jsFunc->_interpret(runtime);
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
      _sh_throw_current(getSHRuntime(runtime));

    return result.getValue();
  }

  // FIXME: check for register stack overflow.
  CallResult<PseudoHandle<>> res{ExecutionStatus::EXCEPTION};

  {
    GCScopeMarkerRAII marker{runtime};
    if (LLVM_LIKELY(vmisa<Callable>(*callTarget))) {
      res = Callable::call(Handle<Callable>::vmcast(callTarget), runtime);
    } else {
      res = runtime.raiseTypeErrorForValue(
          Handle<>(callTarget), " is not a function");
    }
  }

  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(getSHRuntime(runtime));
  }

  return res->getHermesValue();
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_ljs_call(SHRuntime *shr, SHLegacyValue *frame, uint32_t argCount) {
  Runtime &runtime = getRuntime(shr);
  StackFramePtr newFrame(runtime.getStackPointer());
  newFrame.getPreviousFrameRef() = HermesValue::encodeNativePointer(frame);
  newFrame.getSavedIPRef() = HermesValue::encodeNativePointer(nullptr);
  newFrame.getSavedCodeBlockRef() = HermesValue::encodeNativePointer(nullptr);
  newFrame.getSHLocalsRef() = HermesValue::encodeNativePointer(nullptr);
  newFrame.getArgCountRef() = HermesValue::encodeNativeUInt32(argCount);
  return doCall(runtime, &newFrame.getCalleeClosureOrCBRef());
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_callRequire(
    SHRuntime *shr,
    SHArrayStorage **exportCache,
    SHLegacyValue *requireFunc,
    uint32_t modIndex) {
  // Check the cache.
  SHLegacyValue res = module_export_cache::get(
      reinterpret_cast<ArrayStorage *>(*exportCache), modIndex);
  if (!_sh_ljs_is_empty(res)) {
    return res;
  }

  Runtime &runtime = getRuntime(shr);
  auto slowRes = [&]() -> CallResult<SHLegacyValue> {
    struct : public Locals {
      PinnedValue<> modExport;
      PinnedValue<Callable> reqFuncPV;
    } lv;
    LocalsRAII lraii{runtime, &lv};

    // The value should be a Callable, or else we raise an error (as
    // Interpreter::handleCallSlowPath does).
    auto *reqPHV = toPHV(requireFunc);
    Callable *reqFuncCallable = dyn_vmcast_or_null<Callable>(*reqPHV);
    if (!reqFuncCallable) {
      (void)runtime.raiseTypeErrorForValue(
          Handle<>(reqPHV), " is not a function");
      return ExecutionStatus::EXCEPTION;
    }

    lv.reqFuncPV = reqFuncCallable;
    CallResult<PseudoHandle<>> modExport = Callable::executeCall1(
        lv.reqFuncPV,
        runtime,
        HandleRootOwner::getUndefinedValue(),
        HermesValue::encodeTrustedNumberValue(modIndex));
    if (LLVM_UNLIKELY(modExport == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    lv.modExport = modExport->get();

    // Populate the cache.
    module_export_cache::set(
        getRuntime(shr),
        reinterpret_cast<ArrayStorage *&>(*exportCache),
        modIndex,
        lv.modExport);

    // Whether or not the caching above succeeded, return the module export.
    return lv.modExport.get();
  }();
  if (LLVM_UNLIKELY(slowRes == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *slowRes;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_call_builtin(
    SHRuntime *shr,
    SHLegacyValue *frame,
    uint32_t argCount,
    uint32_t builtinMethodID) {
  auto res = [&]() {
    Runtime &runtime = getRuntime(shr);
    // Non-allocating, so it is safe to resolve the callee before the frame
    // exists.
    auto callee =
        vmcast<NativeFunction>(runtime.getBuiltinCallable(builtinMethodID));
    auto newFrame = StackFramePtr::initFrame(
        runtime.getStackPointer(),
        StackFramePtr(toPHV(frame)),
        /* savedIP */ nullptr,
        /* savedCodeBlock */ nullptr,
        /* locals */ nullptr,
        argCount,
        HermesValue::encodeObjectValue(callee),
        HermesValue::encodeUndefinedValue());
    // "thisArg" is implicitly assumed to be "undefined": the compiler never
    // populates the slot (CallBuiltin's `this` is lowered to an
    // ImplicitMovInst, which emits no code), and initFrame writes only the
    // seven metadata slots, so it must be set here, the same way the
    // interpreter's implCallBuiltin does.
    newFrame.getThisArgRef() = HermesValue::encodeUndefinedValue();

    GCScopeMarkerRAII marker{runtime};
    return NativeFunction::_nativeCall(callee, runtime);
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return res->getHermesValue();
}

extern "C" SHLegacyValue _sh_ljs_get_builtin_closure(
    SHRuntime *shr,
    uint32_t builtinMethodID) {
  return HermesValue::encodeObjectValue(
      getRuntime(shr).getBuiltinCallable(builtinMethodID));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_create_environment(
    SHRuntime *shr,
    const SHLegacyValue *parentEnv,
    uint32_t size) {
  Runtime &runtime = getRuntime(shr);

  auto parentHandle = parentEnv
      ? Handle<Environment>::vmcast(toPHV(parentEnv))
      : HandleRootOwner::makeNullHandle<Environment>();

  GCScopeMarkerRAII marker{runtime};
  return Environment::create(runtime, parentHandle, size);
  // #ifdef HERMES_ENABLE_DEBUGGER
  //   framePtr.getDebugEnvironmentRef() = *res;
  // #endif
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_ljs_get_env(SHRuntime *shr, SHLegacyValue startEnv, uint32_t level) {
  Runtime &runtime = getRuntime(shr);
  Environment *curEnv = vmcast<Environment>(*toPHV(&startEnv));
  while (level--) {
    assert(curEnv && "invalid environment relative level");
    curEnv = curEnv->getParentEnvironment(runtime);
  }

  return HermesValue::encodeObjectValue(curEnv);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_store_to_env(
    SHRuntime *shr,
    SHLegacyValue env,
    SHLegacyValue val,
    uint32_t index) {
  vmcast<Environment>(HermesValue::fromRaw(env.raw))
      ->slot(index)
      .set(HermesValue::fromRaw(val.raw), getRuntime(shr).getHeap());
}

extern "C" void _sh_ljs_store_np_to_env(
    SHRuntime *shr,
    SHLegacyValue env,
    SHLegacyValue val,
    uint32_t index) {
  vmcast<Environment>(HermesValue::fromRaw(env.raw))
      ->slot(index)
      .setNonPtr(HermesValue::fromRaw(val.raw), getRuntime(shr).getHeap());
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_create_generator_object(
    SHRuntime *shr,
    const SHLegacyValue *env,
    SHLegacyValue (*func)(SHRuntime *),
    const SHNativeFuncInfo *funcInfo,
    const SHUnit *unit) {
  assert(
      !_sh_ljs_is_null(*env) && "inner generator cannot have null environment");
  Runtime &runtime = getRuntime(shr);

  auto genObjRes =
      [&runtime, env, func, funcInfo, unit]() -> CallResult<SHLegacyValue> {
    GCScopeMarkerRAII marker{runtime};
    struct : public Locals {
      PinnedValue<NativeJSFunction> generatorFunction;
      PinnedValue<JSObject> prototype;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    Handle<NativeJSFunction> innerFunc = NativeJSFunction::create(
        runtime,
        Handle<JSObject>::vmcast(&runtime.functionPrototype),
        env ? Handle<Environment>::vmcast(toPHV(env))
            : Runtime::makeNullHandle<Environment>(),
        func,
        funcInfo,
        unit);
    lv.generatorFunction = vmcast<NativeJSFunction>(
        runtime.getCurrentFrame().getCalleeClosureUnsafe());
    uint8_t kind = lv.generatorFunction->getFunctionInfo()->kind;
    assert(
        (kind == FuncKind::Generator || kind == FuncKind::Async) &&
        "should be called from a generator or async function");

    // The generator created by an async function is internal to it, and the
    // async function's "prototype" property, if any, belongs to user code.
    if (kind == FuncKind::Async) {
      return JSGeneratorObject::create(
                 runtime,
                 innerFunc,
                 Handle<JSObject>::vmcast(&runtime.generatorPrototype))
          ->getHermesValue();
    }

    auto prototypeProp = JSObject::getNamed_RJS(
        lv.generatorFunction,
        runtime,
        Predefined::getSymbolID(Predefined::prototype));
    if (LLVM_UNLIKELY(prototypeProp == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    Handle<JSObject> prototype =
        Handle<JSObject>::vmcast(&runtime.generatorPrototype);
    if (vmisa<JSObject>(prototypeProp->get())) {
      lv.prototype.castAndSetHermesValue<JSObject>(prototypeProp->get());
      prototype = lv.prototype;
    }
    return JSGeneratorObject::create(runtime, innerFunc, prototype)
        ->getHermesValue();
  }();

  if (genObjRes == ExecutionStatus::EXCEPTION)
    _sh_throw_current(shr);

  return *genObjRes;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_create_class(
    SHRuntime *shr,
    const SHLegacyValue *env,
    SHLegacyValue (*func)(SHRuntime *),
    const SHNativeFuncInfo *funcInfo,
    const SHUnit *unit,
    SHLegacyValue *homeObjectOut,
    SHLegacyValue *superClass) {
  Runtime &runtime = getRuntime(shr);
  auto classRes = createClass(
      runtime,
      superClass ? Handle{toPHV(superClass)} : Runtime::getEmptyValue(),
      [&runtime, env, func, funcInfo, unit](Handle<JSObject> ctorParent) {
        auto envHandle = env ? Handle<Environment>::vmcast(toPHV(env))
                             : HandleRootOwner::makeNullHandle<Environment>();
        return *NativeJSClass::create(
            runtime, ctorParent, envHandle, func, funcInfo, unit);
      });
  if (classRes == ExecutionStatus::EXCEPTION)
    _sh_throw_current(shr);
  *homeObjectOut = HermesValue::encodeObjectValue(std::get<1>(*classRes));
  return HermesValue::encodeObjectValue(std::get<0>(*classRes));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_create_closure(
    SHRuntime *shr,
    const SHLegacyValue *env,
    SHLegacyValue (*func)(SHRuntime *),
    const SHNativeFuncInfo *funcInfo,
    const SHUnit *unit) {
  Runtime &runtime = getRuntime(shr);
  GCScopeMarkerRAII marker{runtime};
  return NativeJSFunction::createWithInferredParent(
             runtime,
             env ? Handle<Environment>::vmcast(toPHV(env))
                 : Runtime::makeNullHandle<Environment>(),
             func,
             funcInfo,
             unit)
      .getHermesValue();
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_declare_global_var(SHRuntime *shr, SHSymbolID name) {
  Runtime &runtime = getRuntime(shr);
  {
    GCScopeMarkerRAII mark{runtime};

    DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
    dpf.configurable = 0;
    // Do not overwrite existing globals with undefined.
    dpf.setValue = 0;

    auto res = JSObject::defineOwnProperty(
        runtime.getGlobal(),
        runtime,
        SymbolID::unsafeCreate(name),
        dpf,
        Runtime::getUndefinedValue(),
        PropOpFlags().plusThrowOnError());
    if (res != ExecutionStatus::EXCEPTION)
      return;
    assert(
        !runtime.getGlobal()->isProxyObject(runtime) &&
        "global can't be a proxy object");
    // If the property already exists, this should be a noop.
    // Instead of incurring the cost to check every time, do it
    // only if an exception is thrown, and swallow the exception
    // if it exists, since we didn't want to make the call,
    // anyway.  This most likely means the property is
    // non-configurable.
    NamedPropertyDescriptor desc;
    auto res1 = JSObject::getOwnNamedDescriptor(
        runtime.getGlobal(), runtime, SymbolID::unsafeCreate(name), desc);
    if (res1) {
      runtime.clearThrownValue();
      return;
    }
    // fall through for exception
  }
  _sh_throw_current(shr);
}

/// Calculate the default property access flags depending on the mode of the
/// \c CodeBlock.
#define DEFAULT_PROP_OP_FLAGS(strictMode) \
  (strictMode ? PropOpFlags().plusThrowOnError() : PropOpFlags())

template <bool tryProp, bool strictMode>
static inline void putById_RJS(
    Runtime &runtime,
    SHUnit *unit,
    const PinnedHermesValue *target,
    SymbolID symID,
    const PinnedHermesValue *value,
    WritePropertyCacheEntry *cacheEntry) {
  //++NumPutById;
  if (LLVM_LIKELY(target->isObject())) {
    SmallHermesValue shv = SmallHermesValue::encodeHermesValue(*value, runtime);
    auto *obj = vmcast<JSObject>(*target);

    // #ifdef HERMESVM_PROFILER_BB
#if 0
    {
      HERMES_SLOW_ASSERT(
          gcScope.getHandleCountDbg() == KEEP_HANDLES &&
          "unaccounted handles were created");
      auto shvHandle = runtime.makeHandle(shv.toHV(runtime));
      auto cacheHCPtr = vmcast_or_null<HiddenClass>(static_cast<GCCell *>(
          cacheEntry->clazz.get(runtime, runtime.getHeap())));
      CAPTURE_IP(runtime.recordHiddenClass(
          curCodeBlock, ip, ID(idVal), obj->getClass(runtime), cacheHCPtr));
      // shv/obj may be invalidated by recordHiddenClass
      if (shv.isPointer())
        shv.unsafeUpdatePointer(
            static_cast<GCCell *>(shvHandle->getPointer()), runtime);
      obj = vmcast<JSObject>(*target);
    }
    gcScope.flushToSmallCount(KEEP_HANDLES);
#endif
    CompressedPointer clazzPtr{obj->getClassGCPtr()};
    // If we have a cache hit, reuse the cached offset and immediately
    // return the property.
    if (LLVM_LIKELY(cacheEntry && cacheEntry->clazz == clazzPtr)) {
      //++NumPutByIdCacheHits;
      JSObject::setNamedSlotValueUnsafe(
          obj, runtime, cacheEntry->getSlot(), shv);
      return;
    }

    // Now check against the AddPropertyCacheEntry to ensure we can still
    // use the cached information.
    // NOTE: Need to check resultClazz in all cases because it's a
    // weak reference that may have been freed even if the add cache is
    // valid.
    const auto &addCacheEntry =
        sh_unit_get_add_cache_entry(unit, cacheEntry->getAddCacheIndex());
    if (LLVM_LIKELY(addCacheEntry.startClazz == clazzPtr) &&
        LLVM_LIKELY(addCacheEntry.resultClazz) &&
        LLVM_LIKELY(
            addCacheEntry.getParentEpoch() == runtime.getParentCacheEpoch()) &&
        LLVM_LIKELY(addCacheEntry.parent == obj->getParentGCPtr(runtime))) {
      HiddenClass *resultClazz =
          addCacheEntry.resultClazz.getNonNull(runtime, runtime.getHeap());
      JSObject::addNewOwnPropertyInSlot(
          obj, runtime, resultClazz, addCacheEntry.getSlot(), shv);
      return;
    }

    NamedPropertyDescriptor desc;
    OptValue<bool> hasOwnProp =
        JSObject::tryGetOwnNamedDescriptorFast(obj, runtime, symID, desc);
    if (LLVM_LIKELY(hasOwnProp.hasValue() && hasOwnProp.getValue()) &&
        !desc.flags.accessor && desc.flags.writable &&
        !desc.flags.internalSetter) {
      //++NumPutByIdFastPaths;

      // cacheIdx == 0 indicates no caching so don't update the cache in
      // those cases.
      HiddenClass *clazz = vmcast<HiddenClass>(clazzPtr.getNonNull(runtime));
      if (LLVM_LIKELY(!clazz->isDictionaryNoCache()) &&
          LLVM_LIKELY(cacheEntry) &&
          LLVM_LIKELY(desc.slot <= WritePropertyCacheEntry::kMaxSlot)) {
#ifdef HERMES_SLOW_DEBUG
        // if (cacheEntry->clazz && cacheEntry->clazz != clazzPtr)
        //   ++NumPutByIdCacheEvicts;
#else
        //(void)NumPutByIdCacheEvicts;
#endif
        // Cache the class and property slot.
        cacheEntry->clazz = clazzPtr;
        cacheEntry->setSlot(desc.slot);
      }

      // This must be valid because an own property was already found.
      JSObject::setNamedSlotValueUnsafe(obj, runtime, desc.slot, shv);
      return;
    }

    CallResult<bool> putRes{ExecutionStatus::EXCEPTION};
    {
      GCScopeMarkerRAII marker{runtime};
      const PropOpFlags defaultPropOpFlags = DEFAULT_PROP_OP_FLAGS(strictMode);
      putRes = JSObject::putNamed_RJS(
          Handle<JSObject>::vmcast(target),
          runtime,
          symID,
          Handle<>(value),
          !tryProp ? defaultPropOpFlags : defaultPropOpFlags.plusMustExist(),
          /* runtimeModule */ nullptr,
          unit,
          cacheEntry);
    }
    if (LLVM_UNLIKELY(putRes == ExecutionStatus::EXCEPTION))
      _sh_throw_current(getSHRuntime(runtime));
  } else {
    //++NumPutByIdTransient;
    assert(!tryProp && "TryPutById can only be used on the global object");
    ExecutionStatus retStatus;
    {
      GCScopeMarkerRAII marker{runtime};
      retStatus = Interpreter::putByIdTransient_RJS(
          runtime, Handle<>(target), symID, Handle<>(value), strictMode);
    }
    if (retStatus == ExecutionStatus::EXCEPTION)
      _sh_throw_current(getSHRuntime(runtime));
  }
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_put_by_id_loose_rjs(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *target,
    SHSymbolID symID,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *propCacheEntry) {
  putById_RJS<false, false>(
      getRuntime(shr),
      unit,
      toPHV(target),
      SymbolID::unsafeCreate(symID),
      toPHV(value),
      reinterpret_cast<WritePropertyCacheEntry *>(propCacheEntry));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_put_by_id_strict_rjs(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *target,
    SHSymbolID symID,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *propCacheEntry) {
  putById_RJS<false, true>(
      getRuntime(shr),
      unit,
      toPHV(target),
      SymbolID::unsafeCreate(symID),
      toPHV(value),
      reinterpret_cast<WritePropertyCacheEntry *>(propCacheEntry));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_try_put_by_id_loose_rjs(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *target,
    SHSymbolID symID,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *propCacheEntry) {
  putById_RJS<true, true>(
      getRuntime(shr),
      unit,
      toPHV(target),
      SymbolID::unsafeCreate(symID),
      toPHV(value),
      reinterpret_cast<WritePropertyCacheEntry *>(propCacheEntry));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_try_put_by_id_strict_rjs(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *target,
    SHSymbolID symID,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *propCacheEntry) {
  putById_RJS<true, true>(
      getRuntime(shr),
      unit,
      toPHV(target),
      SymbolID::unsafeCreate(symID),
      toPHV(value),
      reinterpret_cast<WritePropertyCacheEntry *>(propCacheEntry));
}

static inline void putByValWithReceiver_RJS(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    SHLegacyValue *receiver,
    bool strictMode) {
  Handle<> targetHandle{toPHV(target)}, keyHandle{toPHV(key)},
      valueHandle{toPHV(value)}, receiverHandle{toPHV(receiver)};
  Runtime &runtime = getRuntime(shr);
  if (LLVM_LIKELY(targetHandle->isObject())) {
    // Fast path for array writes with numeric index when self == receiver.
    // Uses hasFastIndexProperties + haveOwnIndexed + setOwnIndexed, matching
    // the canonical write fast path in JSObject::putComputedWithReceiver_RJS.
    // `haveOwnIndexed` checks whether an indexed element exists without
    // relying on read-side value semantics like `undefined` vs `empty`.
    static_assert(
        HERMESVALUE_VERSION == 2,
        "HermesValue must use NaN-encoding for non-numbers");
    uint32_t index;
    if (sh_tryfast_f64_to_u32(key->f64, index) && index != 0xFFFFFFFFu &&
        targetHandle.getHermesValue().getRaw() ==
            receiverHandle.getHermesValue().getRaw()) {
      auto objHandle = Handle<JSObject>::vmcast(targetHandle);
      if (objHandle->hasFastIndexProperties(runtime)) {
        if (LLVM_LIKELY(JSObject::haveOwnIndexed(objHandle, runtime, index))) {
          GCScopeMarkerRAII marker{runtime};
          auto result =
              JSObject::setOwnIndexed(objHandle, runtime, index, valueHandle);
          if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
            _sh_throw_current(shr);
          if (LLVM_LIKELY(*result))
            return;
          // Read-only property, fall through to generic path.
        }
      }
    }

    const PropOpFlags defaultPropOpFlags = DEFAULT_PROP_OP_FLAGS(strictMode);
    CallResult<bool> res{false};
    {
      GCScopeMarkerRAII marker{runtime};
      res = JSObject::putComputedWithReceiver_RJS(
          Handle<JSObject>::vmcast(targetHandle),
          runtime,
          keyHandle,
          valueHandle,
          receiverHandle,
          defaultPropOpFlags);
    }
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
      _sh_throw_current(shr);
    return;
  }

  // This is the "slow path".
  ExecutionStatus res;
  {
    GCScopeMarkerRAII marker{runtime};
    res = Interpreter::putByValTransient_RJS(
        runtime, targetHandle, keyHandle, valueHandle, strictMode);
  }
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
}

static inline void putByVal_RJS(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    bool strictMode) {
  putByValWithReceiver_RJS(shr, target, key, value, target, strictMode);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_put_by_val_loose_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value) {
  putByVal_RJS(shr, target, key, value, false);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_put_by_val_strict_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value) {
  putByVal_RJS(shr, target, key, value, true);
}

static inline void putByIndex_RJS(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t index,
    SHLegacyValue *value,
    bool strictMode) {
  Runtime &runtime = getRuntime(shr);
  Handle<> targetHandle{toPHV(target)};
  Handle<> valueHandle{toPHV(value)};

  if (LLVM_LIKELY(targetHandle->isObject())) {
    // Fast path: if the object has fast indexed properties and the element
    // exists in own indexed storage, write directly via setOwnIndexed,
    // bypassing the full putComputed property resolution. Uses haveOwnIndexed
    // (not getOwnIndexed) so the existence check does not depend on read-side
    // value semantics like `undefined` vs `empty`.
    auto objHandle = Handle<JSObject>::vmcast(targetHandle);
    if (objHandle->hasFastIndexProperties(runtime) && index != 0xFFFFFFFFu) {
      if (LLVM_LIKELY(JSObject::haveOwnIndexed(objHandle, runtime, index))) {
        GCScopeMarkerRAII marker{runtime};
        auto result =
            JSObject::setOwnIndexed(objHandle, runtime, index, valueHandle);
        if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
          marker.flush();
          _sh_throw_current(shr);
        }
        if (LLVM_LIKELY(*result))
          return;
        // Read-only property, fall through to generic path.
      }
    }
    // Generic path.
    // The throw must happen after the Locals scope ends: longjmp skips
    // ~LocalsRAII and would leave runtime.vmLocals dangling.
    bool failed;
    {
      GCScopeMarkerRAII marker{runtime};
      struct : public Locals {
        PinnedValue<> indexValue;
      } lv;
      LocalsRAII lraii{runtime, &lv};
      lv.indexValue = HermesValue::encodeTrustedNumberValue(index);
      const PropOpFlags defaultPropOpFlags = DEFAULT_PROP_OP_FLAGS(strictMode);
      auto putRes = JSObject::putComputed_RJS(
          objHandle, runtime, lv.indexValue, valueHandle, defaultPropOpFlags);
      failed = putRes == ExecutionStatus::EXCEPTION;
    }
    if (LLVM_UNLIKELY(failed))
      _sh_throw_current(shr);
    return;
  }

  // Non-object (transient).
  bool failed;
  {
    GCScopeMarkerRAII marker{runtime};
    struct : public Locals {
      PinnedValue<> indexValue;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    lv.indexValue = HermesValue::encodeTrustedNumberValue(index);
    auto retStatus = Interpreter::putByValTransient_RJS(
        runtime, targetHandle, lv.indexValue, valueHandle, strictMode);
    failed = retStatus == ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(failed))
    _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_put_by_index_loose_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t index,
    SHLegacyValue *value) {
  putByIndex_RJS(shr, target, index, value, false);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_put_by_index_strict_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t index,
    SHLegacyValue *value) {
  putByIndex_RJS(shr, target, index, value, true);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_put_by_val_with_receiver_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    SHLegacyValue *receiver,
    bool isStrict) {
  putByValWithReceiver_RJS(shr, target, key, value, receiver, isStrict);
}

template <bool tryProp>
static inline HermesValue getByIdWithReceiver_RJS(
    Runtime &runtime,
    Handle<> source,
    SymbolID symID,
    Handle<> receiver,
    ReadPropertyCacheEntry *cacheEntry) {
  //++NumGetById;
  // NOTE: it is safe to use OnREG(GetById) here because all instructions
  // have the same layout: opcode, registers, non-register operands, i.e.
  // they only differ in the width of the last "identifier" field.
  if (LLVM_LIKELY(source->isObject())) {
    auto *obj = vmcast<JSObject>(*source);

    CompressedPointer clazzPtr{obj->getClassGCPtr()};
#ifndef NDEBUG
    // if (vmcast<HiddenClass>(clazzPtr.getNonNull(runtime))->isDictionary())
    //   ++NumGetByIdDict;
#else
    //(void)NumGetByIdDict;
#endif

    if (LLVM_LIKELY(cacheEntry)) {
      // If we have a cache hit, reuse the cached offset and immediately
      // return the property.
      if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {
        //++NumGetByIdCacheHits;
        return JSObject::getNamedSlotValueUnsafe(
                   obj, runtime, cacheEntry->getSlot())
            .unboxToHV(runtime);
      }

      // See if it's a proto cache hit.
      if (LLVM_LIKELY(cacheEntry->negMatchClazz == clazzPtr)) {
        // Proxy, HostObject and lazy objects have special hidden classes, so
        // they should never match the cached class.
        assert(!obj->isProxyObject(runtime));
        assert(!obj->isHostObject(runtime));
        assert(!obj->isLazy(runtime));
        const GCPointer<JSObject> &parentGCPtr = obj->getParentGCPtr(runtime);
        if (LLVM_LIKELY(parentGCPtr)) {
          JSObject *parent = parentGCPtr.getNonNull(runtime);
          if (LLVM_LIKELY(cacheEntry->clazz == parent->getClassGCPtr())) {
            return JSObject::getNamedSlotValueUnsafe(
                       parent, runtime, cacheEntry->getSlot())
                .unboxToHV(runtime);
          }
        }
      }
    }

    NamedPropertyDescriptor desc;
    OptValue<bool> fastPathResult =
        JSObject::tryGetOwnNamedDescriptorFast(obj, runtime, symID, desc);
    if (LLVM_LIKELY(fastPathResult.hasValue() && fastPathResult.getValue()) &&
        !desc.flags.accessor) {
      //++NumGetByIdFastPaths;

      // cacheIdx == 0 indicates no caching so don't update the cache in
      // those cases.
      HiddenClass *clazz = vmcast<HiddenClass>(clazzPtr.getNonNull(runtime));
      if (LLVM_LIKELY(!clazz->isDictionaryNoCache()) &&
          LLVM_LIKELY(cacheEntry) &&
          LLVM_LIKELY(desc.slot <= ReadPropertyCacheEntry::kMaxSlot)) {
#ifdef HERMES_SLOW_DEBUG
        // if (cacheEntry->clazz && cacheEntry->clazz != clazzPtr)
        //   ++NumGetByIdCacheEvicts;
#else
        //(void)NumGetByIdCacheEvicts;
#endif
        // Cache the class, id and property slot.
        cacheEntry->clazz = clazzPtr;
        cacheEntry->setSlot(desc.slot);
      }

      assert(
          !obj->isProxyObject(runtime) &&
          "tryGetOwnNamedDescriptorFast returned true on Proxy");
      return JSObject::getNamedSlotValueUnsafe(obj, runtime, desc)
          .unboxToHV(runtime);
    }

#ifdef HERMES_SLOW_DEBUG
    // Call to getNamedDescriptorUnsafe is safe because `id` is kept alive
    // by the IdentifierTable.
    // JSObject *propObj = JSObject::getNamedDescriptorUnsafe(
    //    Handle<JSObject>::vmcast(&O2REG(GetById)), runtime, id, desc);
    // if (propObj) {
    //  if (desc.flags.accessor)
    //    ++NumGetByIdAccessor;
    //  else if (propObj != vmcast<JSObject>(O2REG(GetById)))
    //    ++NumGetByIdProto;
    //} else {
    //  ++NumGetByIdNotFound;
    //}
#else
    //(void)NumGetByIdAccessor;
    //(void)NumGetByIdProto;
    //(void)NumGetByIdNotFound;
#endif
#ifdef HERMES_SLOW_DEBUG
    // auto *savedClass = true /*cacheEntry*/
    //     ? cacheEntry->clazz.get(runtime, runtime.getHeap())
    //     : nullptr;
#endif
    //++NumGetByIdSlow;
    CallResult<PseudoHandle<>> resPH{ExecutionStatus::EXCEPTION};
    {
      GCScopeMarkerRAII marker(runtime);
      const PropOpFlags defaultPropOpFlags = DEFAULT_PROP_OP_FLAGS(false);
      resPH = JSObject::getNamedWithReceiver_RJS(
          Handle<JSObject>::vmcast(source),
          runtime,
          symID,
          receiver,
          !tryProp ? defaultPropOpFlags : defaultPropOpFlags.plusMustExist(),
          cacheEntry);
    }
    if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION))
      _sh_throw_current(getSHRuntime(runtime));
#ifdef HERMES_SLOW_DEBUG
    // if (cacheEntry && savedClass &&
    //     cacheEntry->clazz.get(runtime, runtime.getHeap()) != savedClass) {
    //   ++NumGetByIdCacheEvicts;
    // }
#endif
    return resPH->get();
  } else {
    //++NumGetByIdTransient;
    assert(!tryProp && "TryGetById can only be used on the global object");
    /* Slow path. */
    CallResult<PseudoHandle<>> resPH{ExecutionStatus::EXCEPTION};
    {
      GCScopeMarkerRAII marker{runtime};
      resPH = Interpreter::getByIdTransientWithReceiver_RJS(
          runtime, source, symID, receiver);
    }
    if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION))
      _sh_throw_current(getSHRuntime(runtime));
    return resPH->get();
  }
}

/// Assume the receiver is the same as the source object.
template <bool tryProp>
static inline HermesValue getById_RJS(
    Runtime &runtime,
    Handle<> source,
    SymbolID symID,
    ReadPropertyCacheEntry *cacheEntry) {
  return getByIdWithReceiver_RJS<tryProp>(
      runtime, source, symID, source, cacheEntry);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_create_this(
    SHRuntime *shr,
    SHLegacyValue *callee,
    SHLegacyValue *newTarget,
    SHReadPropertyCacheEntry *propCacheEntry,
    SHCompressedPointer *classCacheEntry) {
  Runtime &runtime = getRuntime(shr);
  auto *calleePHV = toPHV(callee);
  auto *newTargetPHV = toPHV(newTarget);
  {
    // TODO(T168592126) standardize on where we perform function call
    // validation for the native backend.
    if (LLVM_UNLIKELY(!vmisa<Callable>(*calleePHV))) {
      (void)runtime.raiseTypeErrorForValue(
          Handle<>(calleePHV), " cannot be used as a constructor.");
      goto throwCurrent;
    }
    if (LLVM_UNLIKELY(!vmisa<Callable>(*newTargetPHV))) {
      (void)runtime.raiseTypeErrorForValue(
          Handle<>(newTargetPHV), " invalid new.target.");
      goto throwCurrent;
    }

    auto *calleeFunc = vmcast<JSObject>(*calleePHV);
    auto cellKind = calleeFunc->getKind();
    Callable *correctNewTarget;
    // If this is a callable that expects a `this`, then skip ahead and start
    // making the object.
    if (cellKind >= CellKind::CallableExpectsThisKind_first) {
      correctNewTarget = vmcast<Callable>(*toPHV(newTarget));
    } else if (cellKind >= CellKind::CallableMakesThisKind_first) {
      // Callables that make their own this should be given undefined in a
      // construct call.
      return HermesValue::encodeUndefinedValue();
    } else if (cellKind >= CellKind::CallableUnknownMakesThisKind_first) {
      correctNewTarget = vmcast<Callable>(*toPHV(newTarget));
      while (auto *bound = dyn_vmcast<BoundFunction>(calleeFunc)) {
        calleeFunc = bound->getTarget(runtime);
        // From ES15 10.4.1.2 [[Construct]] Step 5: If SameValue(F, newTarget)
        // is true, set newTarget to target.
        if (bound == vmcast<Callable>(correctNewTarget)) {
          correctNewTarget = vmcast<Callable>(calleeFunc);
        }
      }
      cellKind = calleeFunc->getKind();
      // Repeat the checks, now against the target.
      if (cellKind >= CellKind::CallableExpectsThisKind_first) {
        // Do nothing, correctNewTarget is already set up correctly.
      } else if (cellKind >= CellKind::CallableMakesThisKind_first) {
        return HermesValue::encodeUndefinedValue();
      } else {
        // If we still can't recognize what to do after advancing through bound
        // functions (if any), error out. This code path is hit when invoking a
        // NativeFunction as a constructor.
        (void)runtime.raiseTypeError(
            "This function cannot be used as a constructor.");
        goto throwCurrent;
      }
    } else {
      // Not a Callable.
      (void)runtime.raiseTypeErrorForValue(
          Handle<>(calleePHV), " cannot be used as a constructor.");
      goto throwCurrent;
    }

    // We shouldn't need to check that new.target is a constructor explicitly.
    // There are 2 cases where this instruction is emitted.
    // 1. new expressions. In this case, new.target == callee. We always verify
    // that a function call is performed correctly, so don't need to
    // double-verify new.target.
    // 2. super() calls in derived constructors. In this case, new.target will
    // be set to the original callee for the new expression which triggered the
    // constructor now invoking super. This means that new.target will be
    // checked. Unfortunately, this check for constructors is done *after* this
    // instruction, so we can't add an assert that newTarget is a constructor
    // here. For example, using `new` on an arrow function, newTarget is not a
    // constructor, but we don't find that out until after this instruction.

    struct : public Locals {
      PinnedValue<Callable> newTarget;
      // This is the .prototype of new.target
      PinnedValue<> newTargetPrototype;
      PinnedValue<HiddenClass> clazz;
    } lv;
    LocalsRAII lraii(runtime, &lv);
    lv.newTarget = correctNewTarget;
    lv.newTargetPrototype = getById_RJS<false>(
        getRuntime(shr),
        lv.newTarget,
        Predefined::getSymbolID(Predefined::prototype),
        reinterpret_cast<ReadPropertyCacheEntry *>(propCacheEntry));
    Handle<JSObject> parentHandle = lv.newTargetPrototype->isObject()
        ? Handle<JSObject>::vmcast(&lv.newTargetPrototype)
        : runtime.objectPrototype;

    // Cache the class per call site, so the common case of a stable prototype
    // skips the parent transition lookup.
    auto *cacheEntry =
        reinterpret_cast<WeakRoot<HiddenClass> *>(classCacheEntry);
    HiddenClass *cached =
        cacheEntry ? cacheEntry->get(runtime, runtime.getHeap()) : nullptr;
    if (cached && cached->getObjectParent(runtime) == *parentHandle) {
      lv.clazz = cached;
    } else {
      lv.clazz = *runtime.getHiddenClassForPrototype(
          *parentHandle, runtime.classJSObject);
      assert(!lv.clazz->isDictionary() && "Prototype must be a normal object");
      if (cacheEntry)
        cacheEntry->set(runtime, *lv.clazz);
    }

    return JSObject::create(runtime, parentHandle, lv.clazz).getHermesValue();
  }
throwCurrent:
  _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_try_get_by_id_rjs(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry) {
  return getById_RJS<true>(
      getRuntime(shr),
      Handle<>{toPHV(source)},
      SymbolID::unsafeCreate(symID),
      reinterpret_cast<ReadPropertyCacheEntry *>(propCacheEntry));
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_by_id_rjs(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry) {
  return getById_RJS<false>(
      getRuntime(shr),
      Handle<>{toPHV(source)},
      SymbolID::unsafeCreate(symID),
      reinterpret_cast<ReadPropertyCacheEntry *>(propCacheEntry));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_by_id_with_receiver_rjs(
    SHRuntime *shr,
    const SHLegacyValue *source,
    const SHLegacyValue *receiver,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry) {
  return getByIdWithReceiver_RJS<false>(
      getRuntime(shr),
      Handle<>{toPHV(source)},
      SymbolID::unsafeCreate(symID),
      Handle<>{toPHV(receiver)},
      reinterpret_cast<ReadPropertyCacheEntry *>(propCacheEntry));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_define_own_by_id(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHSymbolID key,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *propCacheEntry) {
  Runtime &runtime = getRuntime(shr);
  SymbolID symID = SymbolID::unsafeCreate(key);
  auto *targetPHV = toPHV(target);
  auto *valuePHV = toPHV(value);
  auto *cacheEntry =
      reinterpret_cast<WritePropertyCacheEntry *>(propCacheEntry);
  assert(targetPHV->isObject() && "expected object operand");
  SmallHermesValue shv =
      SmallHermesValue::encodeHermesValue(*valuePHV, runtime);
  auto *obj = vmcast<JSObject>(*targetPHV);
  CompressedPointer clazzPtr{obj->getClassGCPtr()};
  // If we have a cache hit, reuse the cached offset and immediately write to
  // the property.
  if (LLVM_LIKELY(cacheEntry && cacheEntry->clazz == clazzPtr)) {
    JSObject::setNamedSlotValueUnsafe(obj, runtime, cacheEntry->getSlot(), shv);
    return;
  }
  NamedPropertyDescriptor desc;
  OptValue<bool> hasOwnProp =
      JSObject::tryGetOwnNamedDescriptorFast(obj, runtime, symID, desc);
  if (LLVM_LIKELY(hasOwnProp.hasValue() && hasOwnProp.getValue()) &&
      !desc.flags.accessor && desc.flags.writable &&
      !desc.flags.internalSetter && desc.flags.configurable &&
      desc.flags.enumerable) {
    // cacheIdx == 0 indicates no caching so don't update the cache in
    // those cases.
    HiddenClass *clazz = vmcast<HiddenClass>(clazzPtr.getNonNull(runtime));
    if (LLVM_LIKELY(!clazz->isDictionaryNoCache()) && LLVM_LIKELY(cacheEntry) &&
        LLVM_LIKELY(desc.slot <= WritePropertyCacheEntry::kMaxSlot)) {
      // Cache the class and property slot.
      cacheEntry->clazz = clazzPtr;
      cacheEntry->setSlot(desc.slot);
    }

    // This must be valid because an own property was already found.
    JSObject::setNamedSlotValueUnsafe(obj, runtime, desc.slot, shv);
    return;
  }
  CallResult<bool> putRes{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    putRes = JSObject::defineOwnProperty(
        Handle<JSObject>::vmcast(targetPHV),
        runtime,
        symID,
        DefinePropertyFlags::getDefaultNewPropertyFlags(),
        Handle<>(valuePHV),
        PropOpFlags().plusThrowOnError());
  }
  if (LLVM_UNLIKELY(putRes == ExecutionStatus::EXCEPTION))
    _sh_throw_current(getSHRuntime(runtime));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_define_own_by_val(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value) {
  Runtime &runtime = getRuntime(shr);
  CallResult<bool> cr{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    cr = JSObject::defineOwnComputed(
        Handle<JSObject>::vmcast(toPHV(target)),
        runtime,
        Handle<>(toPHV(key)),
        DefinePropertyFlags::getDefaultNewPropertyFlags(),
        Handle<>(toPHV(value)),
        PropOpFlags().plusThrowOnError());
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_define_own_ne_by_val(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value) {
  Runtime &runtime = getRuntime(shr);
  CallResult<bool> cr{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    cr = JSObject::defineOwnComputed(
        Handle<JSObject>::vmcast(toPHV(target)),
        runtime,
        Handle<>(toPHV(key)),
        DefinePropertyFlags::getNewNonEnumerableFlags(),
        Handle<>(toPHV(value)),
        PropOpFlags().plusThrowOnError());
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_define_own_by_index(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t key,
    SHLegacyValue *value) {
  Runtime &runtime = getRuntime(shr);
  CallResult<bool> cr{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    struct : public Locals {
      PinnedValue<> indexValue;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    lv.indexValue = HermesValue::encodeTrustedNumberValue(key);
    cr = JSObject::defineOwnComputedPrimitive(
        Handle<JSObject>::vmcast(toPHV(target)),
        runtime,
        lv.indexValue,
        DefinePropertyFlags::getDefaultNewPropertyFlags(),
        Handle<>(toPHV(value)),
        PropOpFlags().plusThrowOnError());
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
}

/// Put a non-enumerable property.
LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_define_own_getter_setter_by_val(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *getter,
    SHLegacyValue *setter,
    bool isEnumerable) {
  Runtime &runtime{getRuntime(shr)};

  ExecutionStatus cr = [&runtime, target, key, getter, setter, isEnumerable]() {
    GCScopeMarkerRAII marker{runtime};
    struct : public Locals {
      PinnedValue<PropertyAccessor> accessor;
    } lv;
    LocalsRAII lraii{runtime, &lv};

    DefinePropertyFlags dpFlags{};
    dpFlags.setConfigurable = 1;
    dpFlags.configurable = 1;
    dpFlags.setEnumerable = 1;
    dpFlags.enumerable = isEnumerable;

    Handle<Callable> getterCallable = Runtime::makeNullHandle<Callable>();
    Handle<Callable> setterCallable = Runtime::makeNullHandle<Callable>();
    if (LLVM_LIKELY(!toPHV(getter)->isUndefined())) {
      dpFlags.setGetter = 1;
      getterCallable = Handle<Callable>::vmcast(toPHV(getter));
    }
    if (LLVM_LIKELY(!toPHV(setter)->isUndefined())) {
      dpFlags.setSetter = 1;
      setterCallable = Handle<Callable>::vmcast(toPHV(setter));
    }
    assert(
        (dpFlags.setSetter || dpFlags.setGetter) &&
        "No accessor set in DefineOwnGetterSetterByVal");

    auto res =
        PropertyAccessor::create(runtime, getterCallable, setterCallable);
    lv.accessor = std::move(res);

    return JSObject::defineOwnComputed(
               Handle<JSObject>::vmcast(toPHV(target)),
               runtime,
               Handle<>(toPHV(key)),
               dpFlags,
               lv.accessor,
               PropOpFlags().plusThrowOnError())
        .getStatus();
  }();

  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_add_own_private_by_sym(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value) {
  Runtime &runtime{getRuntime(shr)};
  ExecutionStatus cr = [&runtime, target, key, value]() {
    GCScopeMarkerRAII marker{runtime};
    return JSObject::defineNewOwnProperty(
        Handle<JSObject>::vmcast(toPHV(target)),
        runtime,
        toPHV(key)->getSymbol(),
        PropertyFlags::privateFieldPropertyFlags(),
        Handle<>(toPHV(value)));
  }();
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_put_own_private_by_sym(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *privateNameKey,
    SHLegacyValue *value,
    SHPrivateNameCacheEntry *privateNameCacheEntry) {
  Runtime &runtime = getRuntime(shr);
  auto res = [&]() -> ExecutionStatus {
    GCScopeMarkerRAII marker{runtime};
    auto *targetPHV = toPHV(target);
    if (LLVM_LIKELY(targetPHV->isObject())) {
      auto *valuePHV = toPHV(value);
      SmallHermesValue shv =
          SmallHermesValue::encodeHermesValue(*valuePHV, runtime);
      auto *privateNameKeyPHV = toPHV(privateNameKey);
      auto *cacheEntry =
          reinterpret_cast<PrivateNameCacheEntry *>(privateNameCacheEntry);
      auto *obj = vmcast<JSObject>(*targetPHV);
      CompressedPointer clazzPtr{obj->getClassGCPtr()};
      if (LLVM_LIKELY(
              cacheEntry && cacheEntry->clazz == clazzPtr &&
              cacheEntry->nameVal == privateNameKeyPHV->getSymbol())) {
        // Fast path, use the cached slot
        JSObject::setNamedSlotValueUnsafe(obj, runtime, cacheEntry->slot, shv);
        return ExecutionStatus::RETURNED;
      }
      // Slow path, call setPrivateField
      return JSObject::setPrivateField(
          Handle<JSObject>::vmcast(targetPHV),
          runtime,
          Handle<SymbolID>::vmcast(privateNameKeyPHV),
          Handle<>(valuePHV),
          cacheEntry);
    } else {
      return runtime.raiseTypeError(
          "cannot write private property of a non-object");
    }
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
}

static HermesValue delByVal(
    Runtime &runtime,
    PinnedHermesValue *target,
    PinnedHermesValue *key,
    bool strictMode) {
  const PropOpFlags defaultPropOpFlags = DEFAULT_PROP_OP_FLAGS(strictMode);
  CallResult<bool> status{ExecutionStatus::EXCEPTION};
  if (LLVM_LIKELY(target->isObject())) {
    {
      GCScopeMarkerRAII marker{runtime};
      status = JSObject::deleteComputed(
          Handle<JSObject>::vmcast(toPHV(target)),
          runtime,
          Handle<>(toPHV(key)),
          defaultPropOpFlags);
    }
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
      _sh_throw_current(getSHRuntime(runtime));
    }
    return HermesValue::encodeBoolValue(status.getValue());
  } else {
    // This is the "slow path".
    CallResult<HermesValue> res{ExecutionStatus::EXCEPTION};
    {
      GCScopeMarkerRAII marker{runtime};
      res = toObject(runtime, Handle<>(target));
    }
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      // If an exception is thrown, likely we are trying to convert
      // undefined/null to an object. Passing over the name of the property
      // so that we could emit more meaningful error messages.
      // CAPTURE_IP(amendPropAccessErrorMsgWithPropName(
      //     runtime, Handle<>(&O2REG(DelById)), "delete", ID(idVal)));
      _sh_throw_current(getSHRuntime(runtime));
    }
    {
      GCScopeMarkerRAII marker{runtime};
      struct : public Locals {
        PinnedValue<> tmpValue;
      } lv;
      LocalsRAII lraii{runtime, &lv};
      lv.tmpValue = res.getValue();
      status = JSObject::deleteComputed(
          Handle<JSObject>::vmcast(&lv.tmpValue),
          runtime,
          Handle<>(toPHV(key)),
          defaultPropOpFlags);
    }
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
      _sh_throw_current(getSHRuntime(runtime));
    }
    return HermesValue::encodeBoolValue(status.getValue());
  }
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_del_by_val_strict(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key) {
  return delByVal(getRuntime(shr), toPHV(target), toPHV(key), true);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_del_by_val_loose(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key) {
  return delByVal(getRuntime(shr), toPHV(target), toPHV(key), false);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_string(SHRuntime *shr, SHSymbolID symID) {
  NoHandleScope noHandles{getRuntime(shr)};
  return HermesValue::encodeStringValue(
      getRuntime(shr).getStringPrimFromSymbolID(SymbolID::unsafeCreate(symID)));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_create_private_name(
    SHRuntime *shr,
    SHSymbolID descStrID) {
  Runtime &runtime = getRuntime(shr);
  CallResult<HermesValue> cr = [&runtime,
                                descStrID]() -> CallResult<HermesValue> {
    GCScopeMarkerRAII marker{runtime};
    struct : public Locals {
      PinnedValue<StringPrimitive> desc;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    auto *descStr = runtime.getIdentifierTable().getStringPrim(
        runtime, SymbolID::unsafeCreate(descStrID));
    lv.desc = descStr;
    auto res =
        runtime.getIdentifierTable().createNotUniquedSymbol(runtime, lv.desc);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return HermesValue::encodeSymbolValue(*res);
  }();
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *cr;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_create_regexp(
    SHRuntime *shr,
    SHUnit *unit,
    SHSymbolID pattern,
    SHSymbolID flags,
    uint32_t regexp_id) {
  Runtime &runtime = getRuntime(shr);
  GCScopeMarkerRAII marker{runtime};
  struct : public Locals {
    PinnedValue<JSRegExp> re;
    PinnedValue<StringPrimitive> pattern;
    PinnedValue<StringPrimitive> flags;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.re = JSRegExp::create(runtime);
  lv.pattern =
      runtime.getStringPrimFromSymbolID(SymbolID::unsafeCreate(pattern));
  lv.flags = runtime.getStringPrimFromSymbolID(SymbolID::unsafeCreate(flags));
  // The bytecode was compiled ahead of time; nothing is parsed here.
  JSRegExp::initialize(
      lv.re,
      runtime,
      lv.pattern,
      lv.flags,
      sh_unit_get_regexp_bytecode(unit, regexp_id));
  return lv.re.getHermesValue();
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_ljs_create_bigint(SHRuntime *shr, const uint8_t *value, uint32_t size) {
  Runtime &runtime = getRuntime(shr);
  CallResult<HermesValue> res{ExecutionStatus::EXCEPTION};
  {
    // This function won't allocate handles because the `fromBytes` function
    // is also used in the interpreter.
    NoHandleScope noHandle{runtime};
    res = BigIntPrimitive::fromBytes(runtime, llvh::ArrayRef{value, size});
  }
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_new_object(SHRuntime *shr) {
  PseudoHandle<JSObject> result;
  {
    NoHandleScope noHandle{getRuntime(shr)};
    result = JSObject::create(getRuntime(shr));
  }
  return result.getHermesValue();
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_new_object_with_parent(
    SHRuntime *shr,
    SHUnit *unit,
    const SHLegacyValue *parent,
    uint32_t shapeTableIndex) {
  auto &runtime = getRuntime(shr);
  auto *parentPHV = toPHV(parent);
  Handle<JSObject> parentHandle = parentPHV->isObject()
      ? Handle<JSObject>::vmcast(parentPHV)
      : parentPHV->isNull()
      ? Runtime::makeNullHandle<JSObject>()
      : Handle<JSObject>::vmcast(&runtime.objectPrototype);

  HiddenClass *clazz;
  auto *cacheEntry = reinterpret_cast<WeakRoot<HiddenClass> *>(
      &unit->object_literal_class_cache[shapeTableIndex]);
  if (*cacheEntry &&
      cacheEntry->getNonNull(runtime, runtime.getHeap())
              ->getObjectParent(runtime) == *parentHandle) {
    // There is a already a cached entry for this shape, we can just use that.
    clazz = cacheEntry->getNonNull(runtime, runtime.getHeap());
  } else {
    clazz = *runtime.getHiddenClassForPrototype(
        *parentHandle, runtime.classJSObject);
    cacheEntry->set(runtime, clazz);
  }

  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.clazz = clazz;

  PseudoHandle<JSObject> result;
  {
    NoHandleScope noHandle{runtime};
    result = JSObject::create(runtime, parentHandle, lv.clazz);
  }
  return result.getHermesValue();
}

template <ObjectAllocKind AllocKind>
static SHLegacyValue createObjectFromBuffer(
    Runtime &runtime,
    SHUnit *unit,
    Handle<JSObject> parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  HiddenClass *clazz = nullptr;
  auto *cacheEntry = reinterpret_cast<WeakRoot<HiddenClass> *>(
      &unit->object_literal_class_cache[shapeTableIndex]);
  if (*cacheEntry &&
      cacheEntry->getNonNull(runtime, runtime.getHeap())
              ->getObjectParent(runtime) == *parent) {
    // There is a already a cached entry for this shape, we can just use that.
    clazz = cacheEntry->getNonNull(runtime, runtime.getHeap());
  } else {
    // There is no cached entry, construct the class from scratch and try to
    // cache it.
    const SHShapeTableEntry *shapeInfo =
        &unit->obj_shape_table[shapeTableIndex];

    if (shapeInfo->num_props > HiddenClass::maxNumProperties()) {
      (void)runtime.raiseRangeError(
          TwineChar16("Object has more than ") +
          HiddenClass::maxNumProperties() + " properties");
      _sh_throw_current(&runtime);
    }
  }

  NoLeakHandleScope marker{runtime};
  struct : Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.clazz = clazz;
  if (!clazz) {
    const SHShapeTableEntry *shapeInfo =
        &unit->obj_shape_table[shapeTableIndex];
    llvh::ArrayRef keyBuffer{unit->obj_key_buffer, unit->obj_key_buffer_size};
    keyBuffer = keyBuffer.slice(shapeInfo->key_buffer_offset);

    if (isTypedAllocKind(AllocKind)) {
      bool propertiesEnumerable =
          AllocKind != ObjectAllocKind::TypedNonEnumerable;
      lv.clazz = HiddenClass::createForTypedObject(
          runtime, parent, shapeInfo->num_props);
      addTypedBufferPropertiesToHiddenClass(
          runtime,
          keyBuffer,
          shapeInfo->num_props,
          lv.clazz,
          propertiesEnumerable,
          [unit](StringID id) {
            return SymbolID::unsafeCreate(unit->symbols[id]);
          });
    } else {
      lv.clazz = addBufferPropertiesToHiddenClass(
          runtime,
          keyBuffer,
          shapeInfo->num_props,
          *runtime.getHiddenClassForPrototype(*parent, runtime.classJSObject),
          [unit](StringID id) {
            return SymbolID::unsafeCreate(unit->symbols[id]);
          });
    }
    assert(
        shapeInfo->num_props == lv.clazz->getNumProperties() &&
        "numLiterals should match hidden class property count.");
    // Dictionary mode classes cannot be cached since they can change as the
    // resulting object is modified.
    if (LLVM_LIKELY(!lv.clazz->isDictionary()) || isTypedAllocKind(AllocKind))
      cacheEntry->set(runtime, *lv.clazz);
  }

  // Create a new object using the built-in constructor or cached hidden class.
  // Note that the built-in constructor is empty, so we don't actually need to
  // call it.
  auto numProps = lv.clazz->getNumProperties();
  lv.obj = JSObject::create(runtime, parent, lv.clazz);

  struct {
    void visitStringID(StringID id) {
      auto shv = SmallHermesValue::encodeStringValue(
          runtime.getStringPrimFromSymbolID(
              SymbolID::unsafeCreate(unit->symbols[id])),
          runtime);
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, i++, shv);
    }
    void visitNumber(double d) {
      auto shv = SmallHermesValue::encodeNumberValue(d, runtime);
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, i++, shv);
    }
    void visitNull() {
      constexpr auto shv = SmallHermesValue::encodeNullValue();
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, i++, shv);
    }
    void visitUndefined() {
      constexpr auto shv = SmallHermesValue::encodeUndefinedValue();
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, i++, shv);
    }
    void visitBool(bool b) {
      auto shv = SmallHermesValue::encodeBoolValue(b);
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, i++, shv);
    }

    PinnedValue<JSObject> &obj;
    Runtime &runtime;
    SHUnit *unit;
    size_t i;
  } v{lv.obj, runtime, unit, 0};

  llvh::ArrayRef literalValBuffer{
      unit->literal_val_buffer, unit->literal_val_buffer_size};
  SerializedLiteralParser::parseValueBuffer(
      literalValBuffer.slice(valBufferOffset), numProps, v);

  return lv.obj.getHermesValue();
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_new_object_with_buffer(
    SHRuntime *shr,
    SHUnit *unit,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  auto &runtime = getRuntime(shr);
  return createObjectFromBuffer<ObjectAllocKind::Untyped>(
      runtime,
      unit,
      Handle<JSObject>::vmcast(&runtime.objectPrototype),
      shapeTableIndex,
      valBufferOffset);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_new_object_with_buffer_and_parent(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  auto &runtime = getRuntime(shr);
  auto *parentPHV = toPHV(parent);
  Handle<JSObject> parentHandle = parentPHV->isObject()
      ? Handle<JSObject>::vmcast(parentPHV)
      : parentPHV->isNull()
      ? Runtime::makeNullHandle<JSObject>()
      : Handle<JSObject>::vmcast(&runtime.objectPrototype);
  return createObjectFromBuffer<ObjectAllocKind::Untyped>(
      runtime, unit, parentHandle, shapeTableIndex, valBufferOffset);
}

extern "C" SHLegacyValue _sh_new_typed_object_with_buffer(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  auto &runtime = getRuntime(shr);
  auto *parentPHV = toPHV(parent);
  Handle<JSObject> parentHandle = parentPHV->isObject()
      ? Handle<JSObject>::vmcast(parentPHV)
      : parentPHV->isNull()
      ? Runtime::makeNullHandle<JSObject>()
      : Handle<JSObject>::vmcast(&runtime.objectPrototype);
  return createObjectFromBuffer<ObjectAllocKind::TypedEnumerable>(
      runtime, unit, parentHandle, shapeTableIndex, valBufferOffset);
}

extern "C" SHLegacyValue _sh_new_typed_non_enum_object_with_buffer(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  auto &runtime = getRuntime(shr);
  auto *parentPHV = toPHV(parent);
  Handle<JSObject> parentHandle = parentPHV->isObject()
      ? Handle<JSObject>::vmcast(parentPHV)
      : parentPHV->isNull()
      ? Runtime::makeNullHandle<JSObject>()
      : Handle<JSObject>::vmcast(&runtime.objectPrototype);
  return createObjectFromBuffer<ObjectAllocKind::TypedNonEnumerable>(
      runtime, unit, parentHandle, shapeTableIndex, valBufferOffset);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_new_array(SHRuntime *shr, uint32_t size) {
  Runtime &runtime = getRuntime(shr);

  CallResult<HermesValue> arrayRes = [&runtime, size]() {
    GCScopeMarkerRAII marker{runtime};
    return toCallResultHermesValue(JSArray::create(runtime, size, size));
  }();
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);

  return *arrayRes;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_new_array_with_buffer(
    SHRuntime *shr,
    SHUnit *unit,
    uint32_t numElements,
    uint32_t numLiterals,
    uint32_t arrayBufferIndex) {
  // Create a new array using the built-in constructor, and initialize
  // the elements from a literal array buffer.
  Runtime &runtime = getRuntime(shr);
  llvh::ArrayRef arrayBuffer{
      unit->literal_val_buffer, unit->literal_val_buffer_size};

  CallResult<HermesValue> arrayRes = [&runtime, numElements]() {
    GCScopeMarkerRAII marker{runtime};
    return toCallResultHermesValue(
        JSArray::create(runtime, numElements, numElements));
  }();
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);

  HermesValue arr = [&runtime,
                     unit,
                     &arrayRes,
                     numElements,
                     numLiterals,
                     arrayBuffer,
                     arrayBufferIndex]() {
    GCScopeMarkerRAII marker{runtime};
    struct : public Locals {
      PinnedValue<JSArray> arr;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    // Resize the array storage in advance.
    lv.arr = vmcast<JSArray>(*arrayRes);
    Handle<JSArray> arr = lv.arr;
    JSArray::setStorageEndIndex(arr, runtime, numElements);

    struct {
      void visitStringID(StringID id) {
        auto shv = SmallHermesValue::encodeStringValue(
            runtime.getStringPrimFromSymbolID(
                SymbolID::unsafeCreate(unit->symbols[id])),
            runtime);
        JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
      }
      void visitNumber(double d) {
        auto shv = SmallHermesValue::encodeNumberValue(d, runtime);
        JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
      }
      void visitNull() {
        constexpr auto shv = SmallHermesValue::encodeNullValue();
        JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
      }
      void visitUndefined() {
        constexpr auto shv = SmallHermesValue::encodeUndefinedValue();
        JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
      }
      void visitBool(bool b) {
        auto shv = SmallHermesValue::encodeBoolValue(b);
        JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
      }

      Handle<JSArray> arr;
      Runtime &runtime;
      SHUnit *unit;
      size_t i;
    } v{arr, runtime, unit, 0};

    SerializedLiteralParser::parseValueBuffer(
        arrayBuffer.slice(arrayBufferIndex), numLiterals, v);

    return arr.getHermesValue();
  }();

  return arr;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_define_own_in_dense_array(
    SHRuntime *shr,
    SHLegacyValue *array,
    SHLegacyValue *prop,
    uint32_t idx) {
  assert(array && prop);
  Runtime &runtime = getRuntime(shr);
  SmallHermesValue shv =
      SmallHermesValue::encodeHermesValue(*toPHV(prop), runtime);
  JSArray::unsafeSetExistingElementAt(
      vmcast<JSArray>(*toPHV(array)), runtime, idx, shv);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_new_fastarray(SHRuntime *shr, uint32_t sizeHint) {
  Runtime &runtime = getRuntime(shr);

  CallResult<HermesValue> arrayRes = [&runtime, sizeHint]() {
    NoLeakHandleScope noLeakHandles{runtime};
    return FastArray::create(runtime, sizeHint);
  }();
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);

  return *arrayRes;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_new_fastarray_with_proto(
    SHRuntime *shr,
    SHLegacyValue *prototype,
    uint32_t sizeHint) {
  Runtime &runtime = getRuntime(shr);

  CallResult<HermesValue> arrayRes{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    auto protoHandle = Handle<JSObject>::vmcast(toPHV(prototype));
    arrayRes = FastArray::create(runtime, protoHandle, sizeHint);
  }
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);

  return *arrayRes;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_ljs_is_in_rjs(SHRuntime *shr, SHLegacyValue *name, SHLegacyValue *obj) {
  Runtime &runtime = getRuntime(shr);
  CallResult<bool> cr{false};
  {
    GCScopeMarkerRAII marker{runtime};
    if (LLVM_UNLIKELY(!_sh_ljs_is_object(*obj))) {
      (void)runtime.raiseTypeError("right operand of 'in' is not an object");
      cr = ExecutionStatus::EXCEPTION;
    } else {
      cr = JSObject::hasComputed(
          Handle<JSObject>::vmcast(toPHV(obj)), runtime, Handle<>(toPHV(name)));
    }
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return _sh_ljs_bool(*cr);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_private_is_in_rjs(
    SHRuntime *shr,
    SHLegacyValue *privateName,
    SHLegacyValue *target,
    SHPrivateNameCacheEntry *privateNameCacheEntry) {
  Runtime &runtime = getRuntime(shr);
  CallResult<bool> cr{false};
  {
    GCScopeMarkerRAII marker{runtime};
    if (LLVM_UNLIKELY(!_sh_ljs_is_object(*target))) {
      (void)runtime.raiseTypeError("right operand of 'in' is not an object");
      cr = ExecutionStatus::EXCEPTION;
    } else {
      auto *privateNamePHV = toPHV(privateName);
      auto *obj = vmcast<JSObject>(*toPHV(target));
      CompressedPointer clazzPtr{obj->getClassGCPtr()};
      auto *cacheEntry =
          reinterpret_cast<PrivateNameCacheEntry *>(privateNameCacheEntry);
      if (LLVM_LIKELY(
              cacheEntry && cacheEntry->clazz == clazzPtr &&
              cacheEntry->nameVal == privateNamePHV->getSymbol())) {
        // Fast path, reuse the cached result.
        cr = cacheEntry->slot;
      } else {
        // Slow path, look up the symbol on the object's own properties.
        auto *targetPHV = toPHV(target);
        NamedPropertyDescriptor desc;
        auto res = JSObject::getOwnNamedDescriptor(
            Handle<JSObject>::vmcast(targetPHV),
            runtime,
            toPHV(privateName)->getSymbol(),
            desc);
        assert(
            (!res || desc.flags.privateName) &&
            "if a property exists here it should be a private property.");
        // We want to be able to cache negative results here, as in cache the
        // answer that an object with a given HC does not contain a property.
        // However, in dictionary mode the HC wouldn't change even if the object
        // subsequently gets the private property added to it. So we must
        // disable caching when the HC is a dictionary.
        if (privateNameCacheEntry &&
            !vmcast<JSObject>(*targetPHV)->getClass(runtime)->isDictionary()) {
          auto *cacheEntry =
              reinterpret_cast<PrivateNameCacheEntry *>(privateNameCacheEntry);
          cacheEntry->clazz = vmcast<JSObject>(*targetPHV)->getClassGCPtr();
          SymbolID nameSym = privateNamePHV->getSymbol();
          cacheEntry->nameVal = nameSym;
          cacheEntry->slot = res;
        }
        cr = res;
      }
    }
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return _sh_ljs_bool(*cr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_pname_list_rjs(
    SHRuntime *shr,
    SHLegacyValue *base,
    SHLegacyValue *index,
    SHLegacyValue *size) {
  Runtime &runtime = getRuntime(shr);
  if (toPHV(base)->isUndefined() || toPHV(base)->isNull()) {
    // Set the iterator to be undefined value.
    return HermesValue::encodeUndefinedValue();
  }

  auto cr = [&runtime, base, index, size]() -> CallResult<HermesValue> {
    GCScopeMarkerRAII marker{runtime};
    if (LLVM_UNLIKELY(!vmisa<JSObject>(*toPHV(base)))) {
      Handle<> baseHandle{toPHV(base)};
      // Convert to object and store it back to the register.
      auto res = toObject(runtime, baseHandle);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      *base = res.getValue();
    }

    struct : public Locals {
      PinnedValue<JSObject> obj;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    lv.obj.castAndSetHermesValue<JSObject>(*toPHV(base));
    uint32_t beginIndex;
    uint32_t endIndex;
    MutableHandle<JSObject> obj{lv.obj};
    auto cr = getForInPropertyNames(runtime, obj, beginIndex, endIndex);
    if (cr == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    auto arr = *cr;
    *index = HermesValue::encodeTrustedNumberValue(beginIndex);
    *size = HermesValue::encodeTrustedNumberValue(endIndex);
    return arr.getHermesValue();
  }();

  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);

  return *cr;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_get_next_pname_rjs(
    SHRuntime *shr,
    SHLegacyValue *props,
    SHLegacyValue *base,
    SHLegacyValue *indexVal,
    SHLegacyValue *sizeVal) {
  Runtime &runtime = getRuntime(shr);
  assert(
      vmisa<ArrayStorageSmall>(*toPHV(props)) &&
      "GetNextPName's props must be ArrayStorageSmall");
  auto arr = Handle<ArrayStorageSmall>::vmcast(toPHV(props));
  Handle obj = Handle<JSObject>::vmcast(toPHV(base));
  auto result =
      [&runtime, arr, obj, indexVal, sizeVal]() -> CallResult<HermesValue> {
    struct : public Locals {
      PinnedValue<> tmp;
      PinnedValue<JSObject> propObj;
      PinnedValue<SymbolID> tmpPropNameStorage;
      PinnedValue<HiddenClass> cachedClass;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    GCScopeMarkerRAII marker{runtime};
    uint32_t idx = toPHV(indexVal)->getNumber();
    uint32_t size = toPHV(sizeVal)->getNumber();
    // If there's a class at index 2, it means we have a cached class.
    uint32_t startIdx = 0;
    uint32_t numObjProps = 0;
    if (LLVM_LIKELY(size > 2)) {
      auto clazzSHV = arr->at(2);
      if (clazzSHV.isObject()) {
        lv.cachedClass = vmcast<HiddenClass>(clazzSHV.getObject(runtime));
        startIdx = arr->at(0).getNumberAs<uint32_t>(runtime);
        numObjProps = arr->at(1).getNumberAs<uint32_t>(runtime);
      }
    }

    // Loop until we find a property which is present.
    while (idx < size) {
      // If there's no caching, lv.cachedClass is nullptr and the comparison
      // will fail.
      if (LLVM_LIKELY(size > 0) && idx - startIdx < numObjProps &&
          LLVM_LIKELY(lv.cachedClass.get() == obj->getClass(runtime))) {
        // Cached.
        lv.propObj = obj;
        break;
      }
      auto tmpSHV = arr->at(idx);
      if (tmpSHV.isSymbol()) {
        // NOTE: This call is safe because we immediately discard desc,
        // so it can't outlive the SymbolID.
        NamedPropertyDescriptor desc;
        lv.propObj = JSObject::getNamedDescriptorUnsafe(
            obj, runtime, tmpSHV.getSymbol(), desc);
      } else {
        assert(
            (tmpSHV.isNumber() || tmpSHV.isString()) &&
            "GetNextPName must be symbol, string, number");
        ComputedPropertyDescWithSymStorage desc{lv.tmpPropNameStorage};
        lv.tmp = tmpSHV.unboxToHV(runtime);
        ExecutionStatus status = JSObject::getComputedPrimitiveDescriptor(
            obj, runtime, lv.tmp, lv.propObj, desc);
        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }
      if (LLVM_LIKELY(lv.propObj.get()))
        break;
      ++idx;
    }
    if (idx < size) {
      auto tmpSHV = arr->at(idx);
      // We must return the property as a string
      if (tmpSHV.isNumber()) {
        auto strRes =
            numberToStringPrimitive(runtime, tmpSHV.getNumber(runtime));
        if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.tmp = strRes->getHermesValue();
      } else if (tmpSHV.isSymbol()) {
        // for-in enumeration only returns numbers and strings.
        // In most cases (i.e. non-Proxy), we keep the symbol around instead
        // and convert here, so that the above getNamedDescriptor call is
        // faster. Proxy has a filter so that it only returns Strings here. So
        // we don't have to check isUniqued and can convert to string
        // unconditionally.
        assert(
            tmpSHV.getSymbol().isUniqued() &&
            "Symbol primitives (non-uniqued) can't be used in for-in, "
            "not even by Proxy");
        lv.tmp = HermesValue::encodeStringValue(
            runtime.getStringPrimFromSymbolID(tmpSHV.getSymbol()));
      } else {
        assert(
            tmpSHV.isString() && "GetNextPName must be symbol, string, number");
        lv.tmp = vm::HermesValue::encodeStringValue(tmpSHV.getString(runtime));
      }
      *indexVal = HermesValue::encodeTrustedNumberValue(idx + 1);
      return lv.tmp.get();
    } else {
      return HermesValue::encodeUndefinedValue();
    }
  }();

  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);

  return *result;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_direct_eval(
    SHRuntime *shr,
    SHLegacyValue *evalText,
    bool strictCaller) {
  if (!_sh_ljs_is_string(*evalText))
    return *evalText;
  auto result = [&runtime = getRuntime(shr),
                 evalText,
                 strictCaller]() -> CallResult<HermesValue> {
    GCScopeMarkerRAII marker{runtime};

    return vm::directEval(
        runtime,
        Handle<StringPrimitive>::vmcast(toPHV(evalText)),
        strictCaller,
        nullptr,
        false);
  }();

  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);

  return *result;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" int32_t _sh_to_int32_double_slow_path(double d) {
  return truncateToInt32SlowPath(d);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_prload_direct(SHRuntime *shr, SHLegacyValue source, uint32_t propIndex) {
  Runtime &runtime = getRuntime(shr);
  NoAllocScope noAlloc(runtime);
  return JSObject::getNamedSlotValueDirectUnsafe(
             vmcast<JSObject>(*toPHV(&source)), runtime, propIndex)
      .unboxToHV(runtime);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_prload_indirect(SHRuntime *shr, SHLegacyValue source, uint32_t propIndex) {
  Runtime &runtime = getRuntime(shr);
  NoAllocScope noAlloc(runtime);
  return JSObject::getNamedSlotValueIndirectUnsafe(
             vmcast<JSObject>(*toPHV(&source)), runtime, propIndex)
      .unboxToHV(runtime);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_prstore_direct(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  Runtime &runtime = getRuntime(shr);
  SmallHermesValue shv =
      SmallHermesValue::encodeHermesValue(*toPHV(value), runtime);
  JSObject::setNamedSlotValueDirectUnsafe(
      vmcast<JSObject>(*toPHV(target)), runtime, propIndex, shv);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_prstore_direct_bool(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_bool(*value));
  Runtime &runtime = getRuntime(shr);
  auto shv = SmallHermesValue::encodeBoolValue(_sh_ljs_get_bool(*value));
  JSObject::setNamedSlotValueDirectUnsafe<std::false_type>(
      vmcast<JSObject>(*toPHV(target)), runtime, propIndex, shv);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_prstore_direct_number(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_double(*value));
  Runtime &runtime = getRuntime(shr);
  auto shv =
      SmallHermesValue::encodeNumberValue(_sh_ljs_get_double(*value), runtime);
  JSObject::setNamedSlotValueDirectUnsafe(
      vmcast<JSObject>(*toPHV(target)), runtime, propIndex, shv);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_prstore_direct_object(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_object(*value));
  Runtime &runtime = getRuntime(shr);
  auto shv = SmallHermesValue::encodeObjectValue(
      vmcast<JSObject>(*toPHV(value)), runtime);
  JSObject::setNamedSlotValueDirectUnsafe(
      vmcast<JSObject>(*toPHV(target)), runtime, propIndex, shv);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_prstore_direct_string(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_string(*value));
  Runtime &runtime = getRuntime(shr);
  auto shv = SmallHermesValue::encodeStringValue(
      vmcast<StringPrimitive>(*toPHV(value)), runtime);
  JSObject::setNamedSlotValueDirectUnsafe(
      vmcast<JSObject>(*toPHV(target)), runtime, propIndex, shv);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_prstore_indirect(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  Runtime &runtime = getRuntime(shr);
  SmallHermesValue shv =
      SmallHermesValue::encodeHermesValue(*toPHV(value), runtime);
  JSObject::setNamedSlotValueIndirectUnsafe(
      vmcast<JSObject>(*toPHV(target)), runtime, propIndex, shv);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_unreachable() {
  hermes_fatal("Unreachable code reached");
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_throw_array_oob(SHRuntime *shr) {
  (void)getRuntime(shr).raiseRangeError("array load index out of range");
  _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_fastarray_load(SHRuntime *shr, SHLegacyValue *array, double index) {
  Runtime &runtime = getRuntime(shr);
  auto *arr = vmcast<FastArray>(*toPHV(array));
  ArrayStorageSmall *storage = arr->unsafeGetIndexedStorage(runtime);

  uint32_t intIndex = _sh_tryfast_f64_to_u32_cvt(index);
  // Check that the index is an unsigned integer that is within range.
  if (LLVM_UNLIKELY(intIndex >= storage->size() || intIndex != index))
    _sh_throw_array_oob(shr);

  return storage->at(intIndex).unboxToHV(runtime);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_fastarray_store(
    SHRuntime *shr,
    const SHLegacyValue *storedValue,
    SHLegacyValue *array,
    double index) {
  Runtime &runtime = getRuntime(shr);
  auto shv = SmallHermesValue::encodeHermesValue(*toPHV(storedValue), runtime);
  auto *arr = vmcast<FastArray>(*toPHV(array));
  ArrayStorageSmall *storage = arr->unsafeGetIndexedStorage(runtime);

  uint32_t intIndex = _sh_tryfast_f64_to_u32_cvt(index);
  // Check that the index is an unsigned integer that is within range.
  if (LLVM_UNLIKELY(intIndex >= storage->size() || intIndex != index))
    _sh_throw_array_oob(shr);

  return storage->set(intIndex, shv, runtime.getHeap());
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_fastarray_push(
    SHRuntime *shr,
    SHLegacyValue *pushedValue,
    SHLegacyValue *array) {
  Runtime &runtime = getRuntime(shr);
  auto arr = Handle<FastArray>::vmcast(toPHV(array));
  auto val = Handle<>(toPHV(pushedValue));
  auto res = FastArray::push(arr, runtime, val);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_fastarray_append(
    SHRuntime *shr,
    SHLegacyValue *other,
    SHLegacyValue *array) {
  Runtime &runtime = getRuntime(shr);
  auto arr = Handle<FastArray>::vmcast(toPHV(array));
  auto source = Handle<FastArray>::vmcast(toPHV(other));
  auto res = FastArray::append(arr, runtime, source);

  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
}

static SHLegacyValue objectPropsFastArray(
    SHRuntime *shr,
    SHLegacyValue obj,
    SHLegacyValue prototype,
    CallResult<HermesValue> (
        *impl)(Runtime &, Handle<JSObject>, Handle<JSObject>)) {
  Runtime &runtime = getRuntime(shr);
  // The Locals must be destroyed before throwing, since _sh_throw_current
  // doesn't return.
  auto res = [&runtime, obj, prototype, impl]() -> CallResult<HermesValue> {
    struct : public Locals {
      PinnedValue<JSObject> obj;
      PinnedValue<JSObject> prototype;
    } lv;
    LocalsRAII lraii(runtime, &lv);
    lv.obj.castAndSetHermesValue<JSObject>(HermesValue::fromRaw(obj.raw));
    lv.prototype.castAndSetHermesValue<JSObject>(
        HermesValue::fromRaw(prototype.raw));
    return impl(runtime, lv.obj, lv.prototype);
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_object_keys(SHRuntime *shr, SHLegacyValue obj, SHLegacyValue prototype) {
  return objectPropsFastArray(shr, obj, prototype, objectKeysFastArray);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_object_values(SHRuntime *shr, SHLegacyValue obj, SHLegacyValue prototype) {
  return objectPropsFastArray(shr, obj, prototype, objectValuesFastArray);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_object_entries(SHRuntime *shr, SHLegacyValue obj, SHLegacyValue prototype) {
  return objectPropsFastArray(shr, obj, prototype, objectEntriesFastArray);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_string_concat(SHRuntime *shr, uint32_t argCount, ...) {
  Runtime &runtime = getRuntime(shr);

  // StringPrimitive::concat has special handling for two arguments,
  // so use that fast path when possible.
  if (argCount == 2) {
    va_list args;
    va_start(args, argCount);
    auto lhsHandle = Handle<StringPrimitive>::vmcast(
        toPHV(va_arg(args, const SHLegacyValue *)));
    auto rhsHandle = Handle<StringPrimitive>::vmcast(
        toPHV(va_arg(args, const SHLegacyValue *)));
    va_end(args);
    CallResult<HermesValue> result{ExecutionStatus::EXCEPTION};
    {
      GCScopeMarkerRAII marker{runtime};
      result = StringPrimitive::concat(runtime, lhsHandle, rhsHandle);
    }
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
      _sh_throw_current(shr);
    }
    return *result;
  }

  // Otherwise, we concatenate all the strings ourselves.

  // Whether the first string is Buffered, and we should use
  // StringPrimitive::concat instead of StringBuilder.
  bool firstIsBuffered = false;
  // Information used for StringBuilder, if it's used instead of
  // StringPrimitive::concat.
  SafeUInt32 resultSize{0};
  bool isASCII = true;

  va_list args;

  // Compute the size, check for non-ASCII strings.
  va_start(args, argCount);
  for (size_t i = 0; i < argCount; ++i) {
    auto argHandle = Handle<StringPrimitive>::vmcast(
        toPHV(va_arg(args, const SHLegacyValue *)));
    if (i == 0 && isBufferedStringPrimitive(*argHandle)) {
      firstIsBuffered = true;
      // Don't need the other information any more.
      break;
    }
    resultSize.add(argHandle->getStringLength());
    if (!argHandle->isASCII()) {
      isASCII = false;
    }
  }
  va_end(args);

  // Perform the concatenation using a StringBuilder.
  va_start(args, argCount);
  CallResult<HermesValue> result = [&]() -> CallResult<HermesValue> {
    GCScopeMarkerRAII marker{runtime};
    if (firstIsBuffered) {
      // The first argument is a BufferedStringPrimitive, so we can use it and
      // concatenate the rest using StringPrimitive::concat.
      // Begin with the first argument in a MutableHandle.
      MutableHandle<StringPrimitive> outputStrHandle{
          runtime,
          vmcast<StringPrimitive>(*toPHV(va_arg(args, const SHLegacyValue *)))};
      // Start with 1 to account for the first arg already being in the handle.
      for (size_t i = 1; i < argCount; ++i) {
        auto argHandle = Handle<StringPrimitive>::vmcast(
            toPHV(va_arg(args, const SHLegacyValue *)));
        // Use concat repeatedly to use a BufferedStringPrimitive for faster
        // concatenation.
        auto res = StringPrimitive::concat(runtime, outputStrHandle, argHandle);
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        outputStrHandle = vmcast<StringPrimitive>(*res);
      }
      return outputStrHandle.getHermesValue();
    } else {
      // Otherwise, avoid allocating intermediate strings by using a
      // StringBuilder.
      auto builderRes =
          StringBuilder::createStringBuilder(runtime, resultSize, isASCII);
      if (LLVM_UNLIKELY(builderRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      auto builder = std::move(*builderRes);

      for (size_t i = 0; i < argCount; ++i) {
        auto argHandle = Handle<StringPrimitive>::vmcast(
            toPHV(va_arg(args, const SHLegacyValue *)));
        assert((!isASCII || argHandle->isASCII()) && "ASCII flag incorrect");
        builder.appendStringPrim(argHandle);
      }
      return builder.getStringPrimitive().getHermesValue();
    }
  }();
  va_end(args);

  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return *result;
}

extern "C" bool _sh_ljs_is_proxy(SHRuntime *shr, SHLegacyValue object) {
  Runtime &runtime = getRuntime(shr);
  HermesValue val = *toPHV(&object);
  JSObject *obj = vmcast<JSObject>(val);
  return obj->isProxyObject(runtime);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" int _sh_errno(void) {
  return errno;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue
_sh_asciiz_to_string(SHRuntime *shr, const char *str, ptrdiff_t len) {
  Runtime &runtime = getRuntime(shr);
  NoHandleScope noHandle{runtime};

  auto res = StringPrimitive::createEfficient(
      runtime,
      UTF8Ref((const uint8_t *)str, len < 0 ? strlen(str) : len),
      false);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}
