/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/Callable.h"

#include "hermes/BCGen/FunctionInfo.h"
#include "hermes/VM/ArrayLike.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/JSCallableProxy.h"
#include "hermes/VM/JSDataView.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSMapImpl.h"
#include "hermes/VM/JSNativeFunctions.h"
#include "hermes/VM/JSProxy.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/JSWeakMapImpl.h"
#include "hermes/VM/JSWeakRef.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/SmallXString.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StaticHUtils.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/static_h.h"

#include "llvh/ADT/ArrayRef.h"
#include "llvh/ADT/ScopeExit.h"

namespace hermes {
namespace vm {

namespace {

/// A helper to convert a SH-style function into a CallResult-style function.
template <typename Res, typename FnPtr, typename ProfileFn>
inline CallResult<Res>
_callWrapper(FnPtr functionPtr, Runtime &runtime, const ProfileFn &profileFn) {
  // ScopedNativeDepthTracker depthTracker{runtime};
  // if (LLVM_UNLIKELY(depthTracker.overflowed())) {
  //   return runtime.raiseStackOverflow(
  //       Runtime::StackOverflowKind::NativeStack);
  // }

  StackFramePtr newFrame{runtime.getStackPointer()};

  // Ensure the IP was correctly saved before we create the new frame.
  runtime.validateSavedIPBeforeCall();

  // If we call into the JIT (either directly or transitively), it may modify
  // the saved IP. Make sure the IP is restored before we return to the caller.
  auto restoreIP = llvh::make_scope_exit(
      [&newFrame, &runtime]() { runtime.setCurrentIP(newFrame.getSavedIP()); });

  SHJmpBuf jBuf;
  SHLocals *locals = runtime.shLocals;
  if (LLVM_LIKELY(_sh_try(getSHRuntime(runtime), &jBuf) == 0)) {
#ifdef HERMESVM_PROFILER_NATIVECALL
    auto t1 = HERMESVM_RDTSC();
#endif
    SHLegacyValue res = functionPtr(&runtime);
#ifdef HERMESVM_PROFILER_NATIVECALL
    profileFn(HERMESVM_RDTSC() - t1);
#endif
    _sh_end_try(getSHRuntime(runtime), &jBuf);

    if constexpr (std::is_same_v<Res, HermesValue>)
      return HermesValue::fromRaw(res.raw);
    else
      return createPseudoHandle(HermesValue::fromRaw(res.raw));
  } else {
    auto oldFrame = newFrame.getPreviousFramePointer();
    SHLegacyValue exc = _sh_catch(
        getSHRuntime(runtime), locals, oldFrame, newFrame.ptr() - oldFrame);
    runtime.setThrownValue(HermesValue::fromRaw(exc.raw));
    return ExecutionStatus::EXCEPTION;
  }
}

} // unnamed namespace

//===----------------------------------------------------------------------===//
// class Environment

const VTable Environment::vt{CellKind::EnvironmentKind, 0};

void EnvironmentBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const Environment *>(cell);
  mb.setVTable(&Environment::vt);
  mb.addField("parentEnvironment", &self->parentEnvironment_);
  mb.addArray(self->getSlots(), &self->size_, sizeof(GCHermesValue));
}

//===----------------------------------------------------------------------===//
// class Callable

void CallableBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<Callable>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const Callable *>(cell);
  mb.setVTable(&Callable::vt);
  mb.addField("environment", &self->environment_);
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
std::string Callable::_snapshotNameImpl(GCCell *cell, GC &gc) {
  auto *const self = reinterpret_cast<Callable *>(cell);
  return self->getNameIfExists(gc.getPointerBase());
}
#endif

/// \return the inferred parent of a Callable based on its \p kind.
static Handle<JSObject> inferredParent(Runtime &runtime, FuncKind kind) {
  if (kind == FuncKind::Generator) {
    return runtime.generatorFunctionPrototype;
  } else if (kind == FuncKind::Async) {
    return runtime.asyncFunctionPrototype;
  } else {
    assert(kind == FuncKind::Normal && "Unsupported function kind");
    return runtime.functionPrototype;
  }
}

void Callable::defineLazyProperties(Handle<Callable> fn, Runtime &runtime) {
  struct : public Locals {
    PinnedValue<JSObject> prototypeObject;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // lazy functions can be Bound or JS Functions.
  if (auto jsFun = Handle<JSFunction>::dyn_vmcast(fn)) {
    const CodeBlock *codeBlock = jsFun->getCodeBlock();

    // Set the actual non-lazy hidden class.
    // Preserve the object's parent and CellKind.
    Handle<HiddenClass> newClass = runtime.getHiddenClassForPrototype(
        jsFun->getParent(runtime),
        vmisa<JSClass>(*jsFun) ? runtime.classJSClass
                               : runtime.classJSFunction);
    jsFun->updateClassNoAllocPropStorageUnsafe(runtime, *newClass);

    // Create empty object for prototype.
    auto prototypeParent = Callable::isGeneratorFunction(*jsFun)
        ? Handle<JSObject>::vmcast(&runtime.generatorPrototype)
        : Handle<JSObject>::vmcast(&runtime.objectPrototype);

    // According to ES12 26.7.4, AsyncFunction instances do not have a
    // 'prototype' property, hence we need to set an null handle here.
    // Functions that cannot be used with `new` should also not define a
    // 'prototype' property, such as arrow functions per 15.3.4, except for
    // generator functions.
    if (!(codeBlock->getHeaderFlags().isCallProhibited(/* construct */ true) &&
          !Callable::isGeneratorFunction(*jsFun))) {
      lv.prototypeObject = JSObject::create(runtime, prototypeParent);
    }

    auto cr = Callable::defineNameLengthAndPrototype(
        fn,
        runtime,
        codeBlock->getNameMayAllocate(),
        codeBlock->getParamCount() - 1,
        lv.prototypeObject,
        Callable::WritablePrototype::Yes);
    assert(
        cr != ExecutionStatus::EXCEPTION && "failed to define length and name");
    (void)cr;
  } else if (auto nativeFun = Handle<NativeJSFunction>::dyn_vmcast(fn)) {
    // Set the actual non-lazy hidden class.
    Handle<HiddenClass> newClass = runtime.getHiddenClassForPrototype(
        nativeFun->getParent(runtime),
        vmisa<NativeJSClass>(*nativeFun) ? runtime.classNativeJSClass
                                         : runtime.classNativeJSFunction);
    nativeFun->updateClassNoAllocPropStorageUnsafe(runtime, *newClass);

    auto prototypeParent = Callable::isGeneratorFunction(*nativeFun)
        ? Handle<JSObject>::vmcast(&runtime.generatorPrototype)
        : Handle<JSObject>::vmcast(&runtime.objectPrototype);

    // According to ES12 26.7.4, AsyncFunction instances do not have a
    // 'prototype' property, hence we need to set an null handle here.
    // Functions that cannot be used with `new` should also not define a
    // 'prototype' property, such as arrow functions per 15.3.4, except for
    // generator functions.
    if (!(nativeFun->getFunctionInfo()->prohibit_invoke ==
              ProhibitInvoke::Construct &&
          !Callable::isGeneratorFunction(*nativeFun))) {
      lv.prototypeObject = JSObject::create(runtime, prototypeParent);
    }

    const SHNativeFuncInfo *funcInfo = nativeFun->getFunctionInfo();
    assert(funcInfo && "funcInfo cannot be null for a lazy NativeJSFunction");
    const SHUnit *unit = nativeFun->getUnit();
    SymbolID name = SymbolID::unsafeCreate(unit->symbols[funcInfo->name_index]);
    uint32_t argCount = funcInfo->arg_count;
    auto cr = Callable::defineNameLengthAndPrototype(
        fn,
        runtime,
        name,
        argCount,
        lv.prototypeObject,
        Callable::WritablePrototype::Yes);
    assert(
        cr != ExecutionStatus::EXCEPTION && "failed to define length and name");
    (void)cr;
  } else {
    // no other kind of function can be lazy currently
    assert(false && "invalid lazy function");
  }
}

ExecutionStatus Callable::defineNameLengthAndPrototype(
    Handle<Callable> selfHandle,
    Runtime &runtime,
    SymbolID name,
    unsigned paramCount,
    Handle<JSObject> prototypeObjectHandle,
    WritablePrototype writablePrototype) {
  PropertyFlags pf;
  pf.clear();
  pf.enumerable = 0;
  pf.writable = 0;
  pf.configurable = 1;

  GCScope scope{runtime, "defineNameLengthAndPrototype"};

  struct : public Locals {
    PinnedValue<> length;
    PinnedValue<StringPrimitive> name;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  namespace P = Predefined;
/// Adds a property to the object in \p OBJ_HANDLE.  \p SYMBOL provides its name
/// as a \c Predefined enum value, and its value is  rooted in \p HANDLE.  If
/// property definition fails, the exceptional execution status will be
/// propagated to the outer function.
#define DEFINE_PROP(OBJ_HANDLE, SYMBOL, HANDLE)                            \
  do {                                                                     \
    auto status = JSObject::defineNewOwnProperty(                          \
        OBJ_HANDLE, runtime, Predefined::getSymbolID(SYMBOL), pf, HANDLE); \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {             \
      return ExecutionStatus::EXCEPTION;                                   \
    }                                                                      \
  } while (false)

  assert(name.isValid() && "A name must always be provided");

  // Length is the number of formal arguments.
  // 10.2.9 SetFunctionLength is performed during 10.2.3 OrdinaryFunctionCreate.
  lv.length = HermesValue::encodeTrustedNumberValue(paramCount);
  DEFINE_PROP(selfHandle, P::length, lv.length);

  // Define the name.
  // 10.2.8 SetFunctionName is performed after 10.2.3 OrdinaryFunctionCreate.
  lv.name = runtime.getStringPrimFromSymbolID(name);
  DEFINE_PROP(selfHandle, P::name, lv.name);

  if (prototypeObjectHandle) {
    // Set its 'prototype' property.
    pf.clear();
    pf.enumerable = 0;
    /// System constructors have read-only prototypes.
    pf.writable = (uint8_t)writablePrototype;
    pf.configurable = 0;
    DEFINE_PROP(selfHandle, P::prototype, prototypeObjectHandle);

    if (LLVM_LIKELY(!Callable::isGeneratorFunction(*selfHandle))) {
      // Set the 'constructor' property in the prototype object.
      // This must not be set for GeneratorFunctions, because
      // prototypes must not point back to their constructors.
      // See the diagram: ES9.0 25.2 (GeneratorFunction objects).
      pf.clear();
      pf.enumerable = 0;
      pf.writable = 1;
      pf.configurable = 1;
      DEFINE_PROP(prototypeObjectHandle, P::constructor, selfHandle);
    }
  }

  return ExecutionStatus::RETURNED;

#undef DEFINE_PROP
}

bool Callable::isGeneratorFunction(Callable *fn) {
  if (auto *jsFun = dyn_vmcast<JSFunction>(fn)) {
    return jsFun->getCodeBlock()->getHeaderFlags().getKind() ==
        FuncKind::Generator;
  } else if (auto *nativeFun = dyn_vmcast<NativeJSFunction>(fn)) {
    return nativeFun->getFunctionInfo()->kind == FuncKind::Generator;
  }
  return false;
}

bool Callable::isAsyncFunction(Callable *fn) {
  if (auto *jsFun = dyn_vmcast<JSFunction>(fn)) {
    return jsFun->getCodeBlock()->getHeaderFlags().getKind() == FuncKind::Async;
  } else if (auto *nativeFun = dyn_vmcast<NativeJSFunction>(fn)) {
    return nativeFun->getFunctionInfo()->kind == FuncKind::Async;
  }
  return false;
}

/// Execute this function with no arguments. This is just a convenience
/// helper method; it actually invokes the interpreter recursively.
CallResult<PseudoHandle<>> Callable::executeCall0(
    Handle<Callable> selfHandle,
    Runtime &runtime,
    Handle<> thisArgHandle,
    bool construct) {
  ScopedNativeCallFrame newFrame{
      runtime,
      0,
      selfHandle.getHermesValue(),
      construct ? selfHandle.getHermesValue()
                : HermesValue::encodeUndefinedValue(),
      *thisArgHandle};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  return call(selfHandle, runtime);
}

/// Execute this function with one argument. This is just a convenience
/// helper method; it actually invokes the interpreter recursively.
CallResult<PseudoHandle<>> Callable::executeCall1(
    Handle<Callable> selfHandle,
    Runtime &runtime,
    Handle<> thisArgHandle,
    HermesValue param1,
    bool construct) {
  ScopedNativeCallFrame newFrame{
      runtime,
      1,
      selfHandle.getHermesValue(),
      construct ? selfHandle.getHermesValue()
                : HermesValue::encodeUndefinedValue(),
      *thisArgHandle};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  newFrame->getArgRef(0) = param1;
  return call(selfHandle, runtime);
}

/// Execute this function with two arguments. This is just a convenience
/// helper method; it actually invokes the interpreter recursively.
CallResult<PseudoHandle<>> Callable::executeCall2(
    Handle<Callable> selfHandle,
    Runtime &runtime,
    Handle<> thisArgHandle,
    HermesValue param1,
    HermesValue param2,
    bool construct) {
  ScopedNativeCallFrame newFrame{
      runtime,
      2,
      selfHandle.getHermesValue(),
      construct ? selfHandle.getHermesValue()
                : HermesValue::encodeUndefinedValue(),
      *thisArgHandle};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  newFrame->getArgRef(0) = param1;
  newFrame->getArgRef(1) = param2;
  return call(selfHandle, runtime);
}

/// Execute this function with three arguments. This is just a convenience
/// helper method; it actually invokes the interpreter recursively.
CallResult<PseudoHandle<>> Callable::executeCall3(
    Handle<Callable> selfHandle,
    Runtime &runtime,
    Handle<> thisArgHandle,
    HermesValue param1,
    HermesValue param2,
    HermesValue param3,
    bool construct) {
  ScopedNativeCallFrame newFrame{
      runtime,
      3,
      selfHandle.getHermesValue(),
      construct ? selfHandle.getHermesValue()
                : HermesValue::encodeUndefinedValue(),
      *thisArgHandle};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  newFrame->getArgRef(0) = param1;
  newFrame->getArgRef(1) = param2;
  newFrame->getArgRef(2) = param3;
  return call(selfHandle, runtime);
}

/// Execute this function with four arguments. This is just a convenience
/// helper method; it actually invokes the interpreter recursively.
CallResult<PseudoHandle<>> Callable::executeCall4(
    Handle<Callable> selfHandle,
    Runtime &runtime,
    Handle<> thisArgHandle,
    HermesValue param1,
    HermesValue param2,
    HermesValue param3,
    HermesValue param4,
    bool construct) {
  ScopedNativeCallFrame newFrame{
      runtime,
      4,
      selfHandle.getHermesValue(),
      construct ? selfHandle.getHermesValue()
                : HermesValue::encodeUndefinedValue(),
      *thisArgHandle};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  newFrame->getArgRef(0) = param1;
  newFrame->getArgRef(1) = param2;
  newFrame->getArgRef(2) = param3;
  newFrame->getArgRef(3) = param4;
  return call(selfHandle, runtime);
}

CallResult<PseudoHandle<>> Callable::executeCall(
    Handle<Callable> selfHandle,
    Runtime &runtime,
    Handle<> newTarget,
    Handle<> thisArgument,
    Handle<JSObject> arrayLike) {
  CallResult<uint64_t> nRes = getArrayLikeLength_RJS(arrayLike, runtime);
  if (LLVM_UNLIKELY(nRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (*nRes > UINT32_MAX) {
    return runtime.raiseRangeError("Too many arguments for apply");
  }
  uint32_t n = static_cast<uint32_t>(*nRes);
  ScopedNativeCallFrame newFrame{
      runtime, n, selfHandle.getHermesValue(), *newTarget, *thisArgument};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);

  // Initialize the arguments to undefined because we might allocate and cause
  // a gc while populating them.
  // TODO: look into doing this lazily.
  newFrame.fillArguments(n, HermesValue::encodeUndefinedValue());

  if (LLVM_UNLIKELY(
          createListFromArrayLike_RJS(
              arrayLike,
              runtime,
              n,
              [&newFrame](Runtime &, uint64_t index, PseudoHandle<> value) {
                newFrame->getArgRef(index) = value.getHermesValue();
                return ExecutionStatus::RETURNED;
              }) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return Callable::call(selfHandle, runtime);
}

CallResult<PseudoHandle<>> Callable::executeConstruct0(
    Handle<Callable> selfHandle,
    Runtime &runtime) {
  CallResult<PseudoHandle<HermesValue>> thisArgRes =
      Callable::createThisForConstruct_RJS(selfHandle, runtime, selfHandle);
  if (LLVM_UNLIKELY(thisArgRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<> thisArg;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.thisArg = std::move(*thisArgRes);
  auto result = executeCall0(selfHandle, runtime, lv.thisArg, true);
  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return (*result)->isObject() ? std::move(result)
                               : CallResult<PseudoHandle<>>(createPseudoHandle(
                                     lv.thisArg.getHermesValue()));
}

CallResult<PseudoHandle<>> Callable::executeConstruct1(
    Handle<Callable> selfHandle,
    Runtime &runtime,
    Handle<> param1) {
  CallResult<PseudoHandle<>> thisArgRes =
      Callable::createThisForConstruct_RJS(selfHandle, runtime, selfHandle);
  if (LLVM_UNLIKELY(thisArgRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<> thisArg;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.thisArg = std::move(*thisArgRes);
  CallResult<PseudoHandle<>> result =
      executeCall1(selfHandle, runtime, lv.thisArg, *param1, true);
  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return (*result)->isObject() ? std::move(result)
                               : CallResult<PseudoHandle<>>(createPseudoHandle(
                                     lv.thisArg.getHermesValue()));
}

CallResult<PseudoHandle<>> Callable::createThisForConstruct_RJS(
    Handle<> callee,
    Runtime &runtime,
    Handle<> newTarget) {
  if (LLVM_UNLIKELY(!isConstructor(runtime, *callee))) {
    return runtime.raiseTypeError(
        "This function cannot be used as a constructor.");
  }
  // Don't verify newTarget if it's the same as callee.
  if (callee->getRaw() != newTarget->getRaw()) {
    if (LLVM_UNLIKELY(!isConstructor(runtime, *newTarget))) {
      return runtime.raiseTypeError(
          "new.target cannot be used as a constructor.");
    }
  }

  // Advance past bound functions to see what we will actually be invoking.
  auto *calleeTargetFunc = vmcast<Callable>(*callee);
  auto *newTargetPtr = vmcast<Callable>(*newTarget);
  while (auto *bound = dyn_vmcast<BoundFunction>(calleeTargetFunc)) {
    NoAllocScope scope(runtime);
    calleeTargetFunc = bound->getTarget(runtime);
    // From ES15 10.4.1.2 [[Construct]] Step 5: If SameValue(F, newTarget) is
    // true, set newTarget to target.
    if (bound == newTargetPtr) {
      newTargetPtr = calleeTargetFunc;
    }
  }

  // Callables that make their own this should be given undefined in a construct
  // call.
  if (Callable::makesOwnThis(calleeTargetFunc)) {
    return createPseudoHandle(HermesValue::encodeUndefinedValue());
  }

  struct : public Locals {
    PinnedValue<Callable> newTarget;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.newTarget = newTargetPtr;

  auto thisArgRes = ordinaryCreateFromConstructor_RJS(
      runtime, lv.newTarget, runtime.objectPrototype);
  if (LLVM_UNLIKELY(thisArgRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return createPseudoHandle(thisArgRes->getHermesValue());
}

CallResult<double> Callable::extractOwnLengthProperty_RJS(
    Handle<Callable> selfHandle,
    Runtime &runtime) {
  struct : public Locals {
    PinnedValue<> propValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  CallResult<PseudoHandle<>> propRes{
      createPseudoHandle(HermesValue::encodeUndefinedValue())};
  NamedPropertyDescriptor desc;
  if (JSObject::getOwnNamedDescriptor(
          selfHandle,
          runtime,
          Predefined::getSymbolID(Predefined::length),
          desc)) {
    propRes = JSObject::getNamedPropertyValue_RJS(
        selfHandle, runtime, selfHandle, desc);
  } else if (selfHandle->isProxyObject(runtime)) {
    ComputedPropertyDescriptor desc;
    CallResult<bool> hasLength = JSProxy::getOwnProperty(
        selfHandle,
        runtime,
        runtime.getPredefinedStringHandle(Predefined::length),
        desc,
        nullptr);
    if (LLVM_UNLIKELY(hasLength == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*hasLength) {
      propRes = JSProxy::getNamed(
          selfHandle,
          runtime,
          Predefined::getSymbolID(Predefined::length),
          selfHandle);
    }
  }

  {
    NoAllocScope naScope{runtime};

    if (propRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }

    if (!(*propRes)->isNumber()) {
      return 0.0;
    }
  }

  lv.propValue = std::move(*propRes);
  auto intRes = toIntegerOrInfinity(runtime, lv.propValue);
  if (intRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  return intRes->getNumber();
}

//===----------------------------------------------------------------------===//
// class BoundFunction

const CallableVTable BoundFunction::vt{
    {
        VTable(
            CellKind::BoundFunctionKind,
            cellSize<BoundFunction>(),
            /* allowLargeAlloc */ false,
            nullptr,
            nullptr,
            nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
            ,
            VTable::HeapSnapshotMetadata{
                HeapSnapshot::NodeType::Closure,
                BoundFunction::_snapshotNameImpl,
                BoundFunction::_snapshotAddEdgesImpl,
                nullptr,
                nullptr}
#endif
            ),
        BoundFunction::_getOwnIndexedRangeImpl,
        BoundFunction::_haveOwnIndexedImpl,
        BoundFunction::_getOwnIndexedPropertyFlagsImpl,
        BoundFunction::_getOwnIndexedImpl,
        BoundFunction::_setOwnIndexedImpl,
        BoundFunction::_deleteOwnIndexedImpl,
        BoundFunction::_checkAllOwnIndexedImpl,
    },
    BoundFunction::_callImpl};

void BoundFunctionBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<BoundFunction>());
  CallableBuildMeta(cell, mb);
  const auto *self = static_cast<const BoundFunction *>(cell);
  mb.setJitCall(BoundFunction::_jitCallImpl);
  mb.setVTable(&BoundFunction::vt);
  mb.addField("target", &self->target_);
  mb.addField("argStorage", &self->argStorage_);
}

CallResult<HermesValue> BoundFunction::create(
    Runtime &runtime,
    Handle<Callable> target,
    unsigned argCountWithThis,
    ConstArgIterator argsWithThis) {
  unsigned argCount = argCountWithThis > 0 ? argCountWithThis - 1 : 0;

  struct : public Locals {
    PinnedValue<ArrayStorage> arrStorage;
    PinnedValue<BoundFunction> self;
    PinnedValue<JSObject> proto;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // Copy the arguments. If we don't have any, we must at least initialize
  // 'this' to 'undefined'.
  auto arrRes = ArrayStorage::create(runtime, argCount + 1);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.arrStorage.castAndSetHermesValue<ArrayStorage>(*arrRes);
  if (argCountWithThis) {
    for (unsigned i = 0; i != argCountWithThis; ++i) {
      ArrayStorage::push_back(
          lv.arrStorage, runtime, Handle<>(&argsWithThis[i]));
    }
  } else {
    // Don't need to worry about resizing since it was created with a capacity
    // of at least 1.
    ArrayStorage::push_back(
        lv.arrStorage, runtime, Runtime::getUndefinedValue());
  }
  CallResult<PseudoHandle<JSObject>> protoRes =
      getPrototypeOf(Handle<JSObject>::vmcast(target), runtime);
  if (LLVM_UNLIKELY(protoRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.proto = std::move(*protoRes);

  auto *cell = runtime.makeAFixed<BoundFunction>(
      runtime,
      lv.proto,
      runtime.getHiddenClassForPrototype(*lv.proto, runtime.classBoundFunction),
      target,
      lv.arrStorage);
  lv.self = JSObjectInit::initToPointer(runtime, cell);
  if (initializeLengthAndName_RJS(lv.self, runtime, target, argCount) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return lv.self.getHermesValue();
}

ExecutionStatus BoundFunction::initializeLengthAndName_RJS(
    Handle<Callable> selfHandle,
    Runtime &runtime,
    Handle<Callable> target,
    unsigned argCount) {
  struct : public Locals {
    PinnedValue<> length;
    PinnedValue<StringPrimitive> name;
    PinnedValue<StringPrimitive> boundName;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  if (LLVM_UNLIKELY(target->isLazy(runtime))) {
    Callable::initializeLazyObject(runtime, target);
  }

  // Extract target.length.
  auto targetLength = Callable::extractOwnLengthProperty_RJS(target, runtime);
  if (targetLength == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  // Define .length
  PropertyFlags pf{};
  pf.enumerable = 0;
  pf.writable = 0;
  pf.configurable = 1;

  // Length is the number of formal arguments.
  lv.length = HermesValue::encodeTrustedNumberValue(
      argCount >= *targetLength ? 0.0 : *targetLength - argCount);
  if (LLVM_UNLIKELY(
          JSObject::defineNewOwnProperty(
              selfHandle,
              runtime,
              Predefined::getSymbolID(Predefined::length),
              pf,
              lv.length) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Set the name by prepending "bound ".
  auto propRes = JSObject::getNamed_RJS(
      target, runtime, Predefined::getSymbolID(Predefined::name));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if ((*propRes)->isString()) {
    lv.name.castAndSetHermesValue<StringPrimitive>(propRes->getHermesValue());
  } else {
    lv.name = runtime.getPredefinedString(Predefined::emptyString);
  }
  auto nameView = StringPrimitive::createStringView(runtime, lv.name);
  llvh::SmallU16String<32> boundName{"bound "};
  boundName.append(nameView.begin(), nameView.end());
  // Share name strings for repeatedly bound functions by using the
  // identifier table. If a new symbol is created, it will disappear
  // after the name string dies, since nothing else refers to it.
  auto &identifierTable = runtime.getIdentifierTable();
  auto boundNameSym = identifierTable.getSymbolHandle(runtime, boundName);
  if (LLVM_UNLIKELY(boundNameSym == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.boundName = identifierTable.getStringPrim(runtime, **boundNameSym);
  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;

  if (LLVM_UNLIKELY(
          JSObject::defineOwnProperty(
              selfHandle,
              runtime,
              Predefined::getSymbolID(Predefined::name),
              dpf,
              lv.boundName) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return ExecutionStatus::RETURNED;
}

CallResult<PseudoHandle<>> BoundFunction::_boundCall(
    BoundFunction *self,
    Runtime &runtime) {
  ScopedNativeDepthTracker depthTracker{runtime};
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }

  CallResult<PseudoHandle<>> res{ExecutionStatus::EXCEPTION};
  StackFramePtr originalCalleeFrame = StackFramePtr(runtime.getStackPointer());
  // Save the original newTarget since we will overwrite it.
  HermesValue originalNewTarget = originalCalleeFrame.getNewTargetRef();
  Callable *originalNewTargetPtr = originalNewTarget.isUndefined()
      ? nullptr
      : vmcast<Callable>(originalNewTarget);
  // From ES15 10.4.1.2 [[Construct]] Step 5: If SameValue(F, newTarget) is
  // true, set newTarget to target. Target in this context means the bound
  // function target. If this boolean is true, then we will set new.target to
  // target as the spec says.
  bool forwardNewTarget = false;
  // Save the original arg count since we will lose it.
  auto originalArgCount = originalCalleeFrame.getArgCount();
  // Keep track of the total arg count.
  auto totalArgCount = originalArgCount;

#ifndef NDEBUG
  auto callerFrame = runtime.getCurrentFrame();
#endif
  struct : Locals {
    PinnedValue<> savedThisArg;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // We must preserve the "thisArg" passed to us by the caller because it is in
  // a register that is not supposed to be modified by a call. Copy it to the
  // scratch register in the caller's frame.
  // Note that since there is only one scratch reg, we must process all chained
  // bound calls in one go (which is more efficient anyway).
  lv.savedThisArg = originalCalleeFrame.getThisArgRef();

  // Pop the stack down to the first argument, erasing the call frame - we don't
  // need the call frame since we will build a new one.
  runtime.popToSavedStackPointer(&originalCalleeFrame.getThisArgRef());

  // Loop, copying the bound arguments of all chained bound functions.
  for (;;) {
    auto boundArgCount = self->getArgCountWithThis(runtime) - 1;
    totalArgCount += boundArgCount;

    // Check if we have enough stack for the arguments and the frame metadata.
    if (LLVM_UNLIKELY(!runtime.checkAvailableStack(
            StackFrameLayout::callerOutgoingRegisters(boundArgCount)))) {
      // Oops, we ran out of stack in the middle of calling a bound function.
      // Restore everything and bail.

      // We can't "pop" the stack pointer to an arbitrary value, which may be
      // higher than the current pointer. So, first we pop everything that we
      // may have pushed, then allocate the correct amount to get back to the
      // initial state.
      runtime.popToSavedStackPointer(&originalCalleeFrame.getThisArgRef());
      runtime.allocUninitializedStack(
          StackFrameLayout::CallerExtraRegistersAtEnd + 1);
      assert(
          runtime.getStackPointer() == originalCalleeFrame.ptr() &&
          "Stack wasn't restored properly");

      res = runtime.raiseStackOverflow(
          Runtime::StackOverflowKind::JSRegisterStack);
      goto bail;
    }

    // Allocate space only for the arguments for now.
    auto *stack = runtime.allocUninitializedStack(boundArgCount);

    // Copy the bound arguments (but not the bound "this").
    std::uninitialized_copy_n(
        self->getArgsWithThis(runtime) + 1, boundArgCount, ArgIterator(stack));

    // We need to do this check at each layer of bound functions, since in the
    // spec, each bound function would be a invoked separately, and each
    // invocation compares the callee of the construct to the original
    // new.target.
    if (self == originalNewTargetPtr) {
      forwardNewTarget = true;
    }

    // Loop while the target is another bound function.
    auto *targetAsBound = dyn_vmcast<BoundFunction>(self->getTarget(runtime));
    if (!targetAsBound)
      break;
    self = targetAsBound;
  }

  // Block scope for non-trivial variables to avoid complaints from "goto".
  {
    // Allocate space for "thisArg" and the frame metdata following the outgoing
    // registers. Note that we already checked earlier that we have enough
    // stack.
    auto *stack = runtime.allocUninitializedStack(
        StackFrameLayout::CallerExtraRegistersAtEnd + 1);
    auto callee = HermesValue::encodeObjectValue(self->getTarget(runtime));

    // Initialize the new frame metadata.
    auto newCalleeFrame = StackFramePtr::initFrame(
        stack,
        runtime.getCurrentFrame(),
        runtime.getCurrentIP(),
        nullptr,
        nullptr,
        totalArgCount,
        callee,
        forwardNewTarget ? callee : originalNewTarget);
    // Initialize "thisArg". When constructing we must use the original 'this',
    // not the bound one.
    newCalleeFrame.getThisArgRef() = !originalNewTarget.isUndefined()
        ? *lv.savedThisArg
        : self->getArgsWithThis(runtime)[0];

    res =
        Callable::call(newCalleeFrame.getCalleeClosureHandleUnsafe(), runtime);

    assert(
        runtime.getCurrentFrame() == callerFrame &&
        "caller frame not restored");

    // Restore the original stack level.
    runtime.popToSavedStackPointer(originalCalleeFrame.ptr());
  }

bail:
  // We must restore the original call frame. There is no need to restore
  // all the fields to their previous values, just the registers which are not
  // supposed to be modified by a call.
  StackFramePtr::initFrame(
      originalCalleeFrame.ptr(),
      StackFramePtr{},
      runtime.getCurrentIP(),
      nullptr,
      nullptr,
      0,
      HermesValue::encodeEmptyValue(),
      HermesValue::encodeEmptyValue());

  // Restore "thisArg" and clear the scratch register to avoid a leak.
  originalCalleeFrame.getThisArgRef() = *lv.savedThisArg;

  return res;
}

CallResult<PseudoHandle<>> BoundFunction::_callImpl(
    Handle<Callable> selfHandle,
    Runtime &runtime) {
  // Pass `nullptr` as the IP because this function is never called
  // from the interpreter, which should use `_boundCall` directly.
  return _boundCall(vmcast<BoundFunction>(selfHandle.get()), runtime);
}

HermesValue BoundFunction::_jitCallImpl(Runtime *runtime, JSObject *self) {
  auto res = [&] {
    GCScopeMarkerRAII marker{*runtime};
    return _boundCall(vmcast<BoundFunction>(self), *runtime);
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(runtime);
  return res->get();
}

//===----------------------------------------------------------------------===//
// class NativeJSFunction

const CallableVTable NativeJSFunction::vt{
    {
        VTable(
            CellKind::NativeJSFunctionKind,
            cellSize<NativeJSFunction>(),
            /* allowLargeAlloc */ false,
            nullptr,
            nullptr,
            nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
            ,
            VTable::HeapSnapshotMetadata{
                HeapSnapshot::NodeType::Closure,
                NativeJSFunction::_snapshotNameImpl,
                NativeJSFunction::_snapshotAddEdgesImpl,
                nullptr,
                nullptr}
#endif
            ),
        NativeJSFunction::_getOwnIndexedRangeImpl,
        NativeJSFunction::_haveOwnIndexedImpl,
        NativeJSFunction::_getOwnIndexedPropertyFlagsImpl,
        NativeJSFunction::_getOwnIndexedImpl,
        NativeJSFunction::_setOwnIndexedImpl,
        NativeJSFunction::_deleteOwnIndexedImpl,
        NativeJSFunction::_checkAllOwnIndexedImpl,
    },
    NativeJSFunction::_callImpl};

void NativeJSFunctionBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<NativeJSFunction>());
  CallableBuildMeta(cell, mb);
  mb.setJitCall(NativeJSFunction::_jitCallImpl);
  mb.setVTable(&NativeJSFunction::vt);
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
std::string NativeJSFunction::_snapshotNameImpl(GCCell *cell, GC &gc) {
  return "NativeJSFunction";
}
#endif

Handle<NativeJSFunction> NativeJSFunction::create(
    Runtime &runtime,
    Handle<JSObject> parentHandle,
    Handle<Environment> parentEnvHandle,
    NativeJSFunctionPtr functionPtr,
    const SHNativeFuncInfo *funcInfo,
    const SHUnit *unit) {
  auto *cell = runtime.makeAFixed<NativeJSFunction>(
      runtime,
      parentHandle,
      runtime.getLazyHiddenClassForPrototype(*parentHandle),
      parentEnvHandle,
      functionPtr,
      funcInfo,
      unit);
  auto selfHandle = JSObjectInit::initToHandle(runtime, cell);
  assert(selfHandle->isLazy(runtime) && "NativeJSFunction must be lazy");
  return selfHandle;
}

Handle<NativeJSFunction> NativeJSFunction::createWithInferredParent(
    Runtime &runtime,
    Handle<Environment> parentEnvHandle,
    NativeJSFunctionPtr functionPtr,
    const SHNativeFuncInfo *funcInfo,
    const SHUnit *unit) {
  return NativeJSFunction::create(
      runtime,
      inferredParent(runtime, (FuncKind)funcInfo->kind),
      parentEnvHandle,
      functionPtr,
      funcInfo,
      unit);
}

/// This is a lightweight and unsafe wrapper intended to be used only by the
/// interpreter. Its purpose is to avoid needlessly exposing the private
/// fields.
CallResult<PseudoHandle<>> NativeJSFunction::_nativeCall(
    NativeJSFunction *self,
    Runtime &runtime) {
  return _callWrapper<PseudoHandle<>>(
      self->functionPtr_, runtime, [=](uint64_t duration) {
#ifdef HERMESVM_PROFILER_NATIVECALL
        self->callDuration_ = duration;
        ++self->callCount_;
#endif
      });
}

CallResult<PseudoHandle<>> NativeJSFunction::_callImpl(
    Handle<Callable> selfHandle,
    Runtime &runtime) {
  // SHLJS code needs to run in the context of an existing GCScope:
  // - When it is invoked from the interpreter, it uses the interpreter's
  // GCScope.
  // - When invoked directly from native as root, the caller must ensure a
  // GCScope.
  // - When invoked from an unknown context using this virtual call, we need
  // a GCScope to ensure safety.
  GCScope gcScope{runtime};
  return _nativeCall(vmcast<NativeJSFunction>(selfHandle.get()), runtime);
}

HermesValue NativeJSFunction::_jitCallImpl(Runtime *runtime, JSObject *self) {
  SHLegacyValue res = vmcast<NativeJSFunction>(self)->functionPtr_(runtime);
  return *toPHV(&res);
}

//===----------------------------------------------------------------------===//
// class NativeJSClass

const CallableVTable NativeJSClass::vt{
    {
        VTable(
            CellKind::NativeJSClassKind,
            cellSize<NativeJSClass>(),
            /*allowLargeAlloc*/ false,
            nullptr,
            nullptr,
            nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
            ,
            VTable::HeapSnapshotMetadata{
                HeapSnapshot::NodeType::Closure,
                NativeJSClass::_snapshotNameImpl,
                NativeJSClass::_snapshotAddEdgesImpl,
                nullptr,
                nullptr}
#endif
            ),
        NativeJSClass::_getOwnIndexedRangeImpl,
        NativeJSClass::_haveOwnIndexedImpl,
        NativeJSClass::_getOwnIndexedPropertyFlagsImpl,
        NativeJSClass::_getOwnIndexedImpl,
        NativeJSClass::_setOwnIndexedImpl,
        NativeJSClass::_deleteOwnIndexedImpl,
        NativeJSClass::_checkAllOwnIndexedImpl,
    },
    NativeJSClass::_callImpl};

void NativeJSClassBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<NativeJSClass>());
  NativeJSFunctionBuildMeta(cell, mb);
  mb.setVTable(&NativeJSClass::vt);
}

Handle<NativeJSClass> NativeJSClass::create(
    Runtime &runtime,
    Handle<JSObject> parentHandle,
    Handle<Environment> parentEnvHandle,
    NativeJSFunctionPtr functionPtr,
    const SHNativeFuncInfo *funcInfo,
    const SHUnit *unit) {
  auto *cell = runtime.makeAFixed<NativeJSClass>(
      runtime,
      parentHandle,
      runtime.getLazyHiddenClassForPrototype(*parentHandle),
      parentEnvHandle,
      functionPtr,
      funcInfo,
      unit);
  auto selfHandle = JSObjectInit::initToHandle(runtime, cell);
  assert(selfHandle->isLazy(runtime) && "NativeJSClass must be lazy");
  return selfHandle;
}

//===----------------------------------------------------------------------===//
// class NativeFunction

const CallableVTable NativeFunction::vt{
    {
        VTable(
            CellKind::NativeFunctionKind,
            cellSize<NativeFunction>(),
            /* allowLargeAlloc */ false,
            nullptr,
            nullptr,
            nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
            ,
            VTable::HeapSnapshotMetadata{
                HeapSnapshot::NodeType::Closure,
                NativeFunction::_snapshotNameImpl,
                NativeFunction::_snapshotAddEdgesImpl,
                nullptr,
                nullptr}
#endif
            ),
        NativeFunction::_getOwnIndexedRangeImpl,
        NativeFunction::_haveOwnIndexedImpl,
        NativeFunction::_getOwnIndexedPropertyFlagsImpl,
        NativeFunction::_getOwnIndexedImpl,
        NativeFunction::_setOwnIndexedImpl,
        NativeFunction::_deleteOwnIndexedImpl,
        NativeFunction::_checkAllOwnIndexedImpl,
    },
    NativeFunction::_callImpl};

void NativeFunctionBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<NativeFunction>());
  CallableBuildMeta(cell, mb);
  mb.setJitCall(&NativeFunction::_jitCallImpl);
  mb.setVTable(&NativeFunction::vt);
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
std::string NativeFunction::_snapshotNameImpl(GCCell *cell, GC &gc) {
  auto *const self = reinterpret_cast<NativeFunction *>(cell);
  return getFunctionName(self->functionPtr_);
}
#endif

Handle<NativeFunction> NativeFunction::create(
    Runtime &runtime,
    Handle<JSObject> parentHandle,
    Handle<HiddenClass> clazz,
    Handle<Environment> parentEnvHandle,
    void *context,
    NativeFunctionPtr functionPtr,
    SymbolID name,
    unsigned paramCount,
    Handle<JSObject> prototypeObjectHandle) {
  auto *cell = runtime.makeAFixed<NativeFunction>(
      runtime, parentHandle, clazz, parentEnvHandle, context, functionPtr);
  auto selfHandle = JSObjectInit::initToHandle(runtime, cell);

  size_t reservedSlots = clazz->getNumProperties();

  // Allocate a propStorage if the number of additional slots requires it.
  runtime.ignoreAllocationFailure(
      JSObject::allocatePropStorage(selfHandle, runtime, reservedSlots));

  auto st = defineNameLengthAndPrototype(
      selfHandle,
      runtime,
      name,
      paramCount,
      prototypeObjectHandle,
      Callable::WritablePrototype::Yes);
  (void)st;
  assert(
      st != ExecutionStatus::EXCEPTION && "defineLengthAndPrototype() failed");

  return selfHandle;
}

CallResult<PseudoHandle<>> NativeFunction::_callImpl(
    Handle<Callable> selfHandle,
    Runtime &runtime) {
  return _nativeCall(vmcast<NativeFunction>(selfHandle.get()), runtime);
}

HermesValue NativeFunction::_jitCallImpl(Runtime *rt, JSObject *self) {
  Runtime &runtime = *rt;
  auto newFrame = runtime.setCurrentFrameToTopOfStack();
  auto res = [&]() -> CallResult<HermesValue> {
    ScopedNativeDepthTracker depthTracker{runtime};
    if (LLVM_UNLIKELY(depthTracker.overflowed())) {
      return runtime.raiseStackOverflow(
          Runtime::StackOverflowKind::NativeStack);
    }

    // We don't need to allocate CalleeExtraRegistersAtStart here because the
    // only register is DebugEnvironment, which is not useful in a
    // NativeFunction.
    static_assert(
        StackFrameLayout::FirstLocal - 1 ==
            StackFrameLayout::DebugEnvironment &&
        StackFrameLayout::CalleeExtraRegistersAtStart == 1);

    GCScopeMarkerRAII marker{runtime};
    auto *nfSelf = vmcast<NativeFunction>(self);
    return nfSelf->functionPtr_(nfSelf->context_, runtime);
  }();

  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(rt);

  runtime.restoreStackAndPreviousFrame(newFrame);
  return *res;
}

//===----------------------------------------------------------------------===//
// class NativeConstructor

template <class From>
static CallResult<PseudoHandle<JSObject>> toCallResultPseudoHandleJSObject(
    PseudoHandle<From> &&other) {
  return PseudoHandle<JSObject>{std::move(other)};
}

template <class From>
static CallResult<PseudoHandle<JSObject>> toCallResultPseudoHandleJSObject(
    CallResult<From *> other) {
  if (LLVM_UNLIKELY(other == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return PseudoHandle<JSObject>::create(*other);
}

template <class From>
static CallResult<PseudoHandle<JSObject>> toCallResultPseudoHandleJSObject(
    CallResult<PseudoHandle<From>> &&other) {
  return std::move(other);
}

template <class From>
static CallResult<PseudoHandle<JSObject>> toCallResultPseudoHandleJSObject(
    CallResult<Handle<From>> other) {
  if (LLVM_UNLIKELY(other == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return PseudoHandle<JSObject>{*other};
}

template <class From>
static CallResult<PseudoHandle<JSObject>> toCallResultPseudoHandleJSObject(
    Handle<From> other) {
  return PseudoHandle<JSObject>{other};
}

CallResult<PseudoHandle<JSObject>> NativeConstructor::parentForNewThis_RJS(
    Runtime &runtime,
    Handle<Callable> newTarget,
    Handle<JSObject> nativeCtorProto) {
  // Slow path, only taken when called via super (or Reflect.construct.)
  CallResult<PseudoHandle<>> prototypeProp = JSObject::getNamed_RJS(
      newTarget, runtime, Predefined::getSymbolID(Predefined::prototype));
  if (LLVM_UNLIKELY(prototypeProp == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (vmisa<JSObject>(prototypeProp->get()))
    return PseudoHandle<JSObject>::vmcast(std::move(prototypeProp.getValue()));
  return PseudoHandle<JSObject>{nativeCtorProto};
}

#define NATIVE_CONSTRUCTOR(fun)                    \
  template CallResult<PseudoHandle<JSObject>> fun( \
      Runtime &, Handle<JSObject>, void *);
#include "hermes/VM/NativeFunctions.def"
#undef NATIVE_CONSTRUCTOR

const CallableVTable NativeConstructor::vt{
    {
        VTable(
            CellKind::NativeConstructorKind,
            cellSize<NativeConstructor>(),
            /* allowLargeAlloc */ false,
            nullptr,
            nullptr,
            nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
            ,
            VTable::HeapSnapshotMetadata{
                HeapSnapshot::NodeType::Closure,
                NativeConstructor::_snapshotNameImpl,
                NativeConstructor::_snapshotAddEdgesImpl,
                nullptr,
                nullptr}
#endif
            ),
        NativeConstructor::_getOwnIndexedRangeImpl,
        NativeConstructor::_haveOwnIndexedImpl,
        NativeConstructor::_getOwnIndexedPropertyFlagsImpl,
        NativeConstructor::_getOwnIndexedImpl,
        NativeConstructor::_setOwnIndexedImpl,
        NativeConstructor::_deleteOwnIndexedImpl,
        NativeConstructor::_checkAllOwnIndexedImpl,
    },
    NativeConstructor::_callImpl};

void NativeConstructorBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<NativeConstructor>());
  NativeFunctionBuildMeta(cell, mb);
  mb.setVTable(&NativeConstructor::vt);
}

#ifndef NDEBUG
CallResult<PseudoHandle<>> NativeConstructor::_callImpl(
    Handle<Callable> selfHandle,
    Runtime &runtime) {
  StackFramePtr newFrame{runtime.getStackPointer()};
  auto newTarget = newFrame.getNewTargetRef();
  assert(
      (newTarget.isUndefined() || vmisa<Callable>(newTarget)) &&
      "new.target for a NativeConstructor should either be undefined or a callable");
  auto res = NativeFunction::_callImpl(selfHandle, runtime);
  if (LLVM_LIKELY(res != ExecutionStatus::EXCEPTION)) {
    assert(
        (newTarget.isUndefined() || vmisa<JSObject>(res->getHermesValue())) &&
        "NativeConstructor needs to return an object when called as constructor.");
  }
  return res;
}
#endif

//===----------------------------------------------------------------------===//
// class JSFunction

const CallableVTable JSFunction::vt{
    {
        VTable(
            CellKind::JSFunctionKind,
            cellSize<JSFunction>(),
            /* allowLargeAlloc */ false,
            nullptr,
            nullptr,
            nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
            ,
            VTable::HeapSnapshotMetadata{
                HeapSnapshot::NodeType::Closure,
                JSFunction::_snapshotNameImpl,
                JSFunction::_snapshotAddEdgesImpl,
                nullptr,
                JSFunction::_snapshotAddLocationsImpl}
#endif
            ),
        JSFunction::_getOwnIndexedRangeImpl,
        JSFunction::_haveOwnIndexedImpl,
        JSFunction::_getOwnIndexedPropertyFlagsImpl,
        JSFunction::_getOwnIndexedImpl,
        JSFunction::_setOwnIndexedImpl,
        JSFunction::_deleteOwnIndexedImpl,
        JSFunction::_checkAllOwnIndexedImpl,
    },
    JSFunction::_callImpl};

void JSFunctionBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSFunction>());
  CallableBuildMeta(cell, mb);
  const auto *self = static_cast<const JSFunction *>(cell);
  mb.addField("domain", &self->domain_);
  mb.setJitCall(JSFunction::_jitCallImpl);
  mb.setVTable(&JSFunction::vt);
}

PseudoHandle<JSFunction> JSFunction::create(
    Runtime &runtime,
    Handle<Domain> domain,
    Handle<JSObject> parentHandle,
    Handle<Environment> envHandle,
    CodeBlock *codeBlock) {
  auto *cell = runtime.makeAFixed<JSFunction, kHasFinalizer>(
      runtime,
      domain,
      parentHandle,
      runtime.getLazyHiddenClassForPrototype(*parentHandle),
      envHandle,
      codeBlock);
  auto self = JSObjectInit::initToPseudoHandle(runtime, cell);
  assert(cell->isLazy(runtime) && "JSFunction must be lazy");
  return self;
}

PseudoHandle<JSFunction> JSFunction::createWithInferredParent(
    Runtime &runtime,
    Handle<Domain> domain,
    Handle<Environment> envHandle,
    CodeBlock *codeBlock) {
  auto parentHandle =
      inferredParent(runtime, (FuncKind)codeBlock->getHeaderFlags().getKind());
  return create(runtime, domain, parentHandle, envHandle, codeBlock);
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
void JSFunction::addLocationToSnapshot(
    HeapSnapshot &snap,
    HeapSnapshot::NodeID id,
    GC &gc) const {
  if (auto location = codeBlock_->getSourceLocationForFunction()) {
    snap.addLocation(
        id,
        codeBlock_->getRuntimeModule()->getScriptID(),
        location->line,
        location->column);
  }
}
#endif

CallResult<HermesValue> JSFunction::_jittedCall(
    JITCompiledFunctionPtr functionPtr,
    Runtime &runtime) {
  // We should not need a GCScope here because all calls to _jittedCall
  // originate either in the interpreter, NativeJSFunctions, or other JIT code.
  // Deep recursion purely between these 3 should not produce excess handles.
  return _callWrapper<HermesValue>(functionPtr, runtime, [](uint64_t) {});
}

CallResult<PseudoHandle<>> JSFunction::_callImpl(
    Handle<Callable> selfHandle,
    Runtime &runtime) {
  auto *self = vmcast<JSFunction>(selfHandle.get());

  if (auto *jitPtr = self->getCodeBlock()->getJITCompiled()) {
    GCScope scope{runtime};
    return _callWrapper<PseudoHandle<>>(jitPtr, runtime, [](uint64_t) {});
  }

  CallResult<HermesValue> result = self->_interpret(runtime);
  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  return createPseudoHandle(*result);
}

HermesValue JSFunction::_jitCallImpl(Runtime *runtime, JSObject *self) {
  auto *jsSelf = vmcast<JSFunction>(self);

  if (auto *jitPtr = jsSelf->getCodeBlock()->getJITCompiled())
    return jitPtr(runtime);

  CallResult<HermesValue> result = jsSelf->_interpret(*runtime);
  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
    _sh_throw_current(runtime);

  return *result;
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
std::string JSFunction::_snapshotNameImpl(GCCell *cell, GC &gc) {
  auto *const self = vmcast<JSFunction>(cell);
  std::string funcName = Callable::_snapshotNameImpl(self, gc);
  if (!funcName.empty()) {
    return funcName;
  }
  return self->codeBlock_->getNameString();
}

void JSFunction::_snapshotAddEdgesImpl(
    GCCell *cell,
    GC &gc,
    HeapSnapshot &snap) {
  auto *const self = vmcast<JSFunction>(cell);
  // Add the super type's edges too.
  Callable::_snapshotAddEdgesImpl(self, gc, snap);

  // A node for the code block will be added as part of the RuntimeModule.
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Shortcut,
      "codeBlock",
      gc.getIDTracker().getNativeID(self->codeBlock_));
}

void JSFunction::_snapshotAddLocationsImpl(
    GCCell *cell,
    GC &gc,
    HeapSnapshot &snap) {
  auto *const self = vmcast<JSFunction>(cell);
  self->addLocationToSnapshot(snap, gc.getObjectID(self), gc);
}
#endif

//===----------------------------------------------------------------------===//
// class JSClass

const CallableVTable JSClass::vt{
    {
        VTable(
            CellKind::JSClassKind,
            cellSize<JSClass>(),
            /*allowLargeAlloc*/ false,
            nullptr,
            nullptr,
            nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
            ,
            VTable::HeapSnapshotMetadata{
                HeapSnapshot::NodeType::Closure,
                JSClass::_snapshotNameImpl,
                JSClass::_snapshotAddEdgesImpl,
                nullptr,
                JSClass::_snapshotAddLocationsImpl}
#endif
            ),
        JSClass::_getOwnIndexedRangeImpl,
        JSClass::_haveOwnIndexedImpl,
        JSClass::_getOwnIndexedPropertyFlagsImpl,
        JSClass::_getOwnIndexedImpl,
        JSClass::_setOwnIndexedImpl,
        JSClass::_deleteOwnIndexedImpl,
        JSClass::_checkAllOwnIndexedImpl,
    },
    JSClass::_callImpl};

void JSClassBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSClass>());
  JSFunctionBuildMeta(cell, mb);
  mb.setVTable(&JSClass::vt);
}

PseudoHandle<JSClass> JSClass::create(
    Runtime &runtime,
    Handle<Domain> domain,
    Handle<JSObject> parentHandle,
    Handle<Environment> envHandle,
    CodeBlock *codeBlock) {
  auto *cell = runtime.makeAFixed<JSClass, kHasFinalizer>(
      runtime,
      domain,
      parentHandle,
      runtime.getLazyHiddenClassForPrototype(*parentHandle),
      envHandle,
      codeBlock);
  auto self = JSObjectInit::initToPseudoHandle(runtime, cell);
  assert(cell->isLazy(runtime) && "JSClass must be lazy");
  return self;
}

} // namespace vm
} // namespace hermes
