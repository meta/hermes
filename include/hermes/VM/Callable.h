/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_CALLABLE_H
#define HERMES_VM_CALLABLE_H

#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/Handle.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/NativeArgs.h"
#include "hermes/VM/Runtime-inline.h"

extern "C" {
struct SHRuntime;
struct SHNativeFuncInfo;
}

namespace hermes {
namespace vm {

/// Environment storing all escaping variables for a function.
/// We need the `HERMES_EMPTY_BASES` attribute here because without it, MSVC
/// or Clang on Windows will add 4 padding bytes before parentEnvironment_ under
/// HV32 release mode (TrailingObjects<Environment, GCHermesValue> has
/// an alignas(8) specifier), which causes different layout than SHEnvironment.
/// Memory layout without HERMES_EMPTY_BASES, with HV32 release mode:
/// 0:  base class VariableSizeRuntimeCell
/// 4:  padding
/// 8:  parentEnvironment_
/// 12: size_
/// 16: trailing objects of GCHermesValue
/// Memory layout with HERMES_EMPTY_BASES, with HV32 release mode:
/// 0: base class VariableSizeRuntimeCell
/// 4: parentEnvironment_
/// 8: size_
/// 12: padding
/// 16: trailing objects of GChermesValue
/// Once we upgrade to c++20, we should use [[no_unique_address]] here.
class HERMES_EMPTY_BASES Environment final
    : public VariableSizeRuntimeCell,
      private llvh::TrailingObjects<Environment, GCHermesValue> {
  friend TrailingObjects;
  friend void EnvironmentBuildMeta(const GCCell *cell, Metadata::Builder &mb);

  /// The parent lexical environment. This value will be nullptr if the
  /// parent is the global scope.
  GCPointer<Environment> parentEnvironment_{};

  /// Number of entries in the environment.
  AtomicIfConcurrentGC<uint32_t> size_;

 public:
  static const VTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::EnvironmentKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::EnvironmentKind;
  }

  /// Create a new Environment.
  static HermesValue create(
      Runtime &runtime,
      Handle<Environment> parentEnvironment,
      uint32_t size) {
    auto *cell = runtime.makeAVariable<Environment>(
        allocationSize(size), runtime, parentEnvironment, size);
    return HermesValue::encodeObjectValue(cell);
  }

  static Environment *
  create(Runtime &runtime, Handle<Callable> parentFn, uint32_t size) {
    return runtime.makeAVariable<Environment>(
        allocationSize(size), runtime, parentFn, size);
  }

  /// \return the parent lexical environment. This value will be nullptr if the
  /// parent is the global scope.
  Environment *getParentEnvironment(PointerBase &runtime) const {
    return parentEnvironment_.get(runtime);
  }

  /// \return the number of HermesValue slots in this environment.
  uint32_t getSize() const {
    return size_.load(std::memory_order_relaxed);
  }

  GCHermesValue &slot(uint32_t index) {
    assert(index < getSize() && "invalid Environment slot index");
    return getSlots()[index];
  }

  /// Gets the amount of memory required by this object for a given size.
  static uint32_t allocationSize(uint32_t size) {
    return totalSizeToAlloc<GCHermesValue>(size);
  }

 public:
  /// \param parentEnvironment the parent lexical environment, or nullptr if the
  ///   parent is the global scope.
  /// \param size the number of entries in the environment.
  Environment(
      Runtime &runtime,
      Handle<Environment> parentEnvironment,
      uint32_t size)
      : parentEnvironment_(runtime, parentEnvironment.get(), runtime.getHeap()),
        size_(size) {
    // Initialize all slots to 'undefined'.
    GCHermesValue::uninitialized_fill(
        getSlots(),
        getSlots() + size,
        HermesValue::encodeUndefinedValue(),
        runtime.getHeap());
  }

  /// Create an environment using the given function to retrieve the parent
  /// environment. \param parentFn the function whose environment is the parent
  /// of this one. \param size the number of entries in the environment.
  inline Environment(
      Runtime &runtime,
      Handle<Callable> parentFn,
      uint32_t size);

 private:
  /// \return a pointer to the array of HermesValue.
  GCHermesValue *getSlots() {
    return getTrailingObjects<GCHermesValue>();
  }
  const GCHermesValue *getSlots() const {
    return getTrailingObjects<GCHermesValue>();
  }

  /// Dummy function for static asserts that may need private fields.
  static inline void staticAsserts() {
    static_assert(sizeof(Environment) == sizeof(SHEnvironment));
    static_assert(
        offsetof(Environment, parentEnvironment_) ==
        offsetof(SHEnvironment, parentEnvironment));
    static_assert(
        offsetof(Environment, size_) == offsetof(SHEnvironment, size));
    llvm_unreachable("staticAsserts must never be called.");
  }
};

struct CallableVTable : public ObjectVTable {
  /// Call the callable with arguments already on the stack.
  CallResult<PseudoHandle<>> (
      *call)(Handle<Callable> selfHandle, Runtime &runtime);
};

/// The abstract base for callable entities, specifically NativeFunction and
/// Function. It presents the ability to call a function with arguments already
/// on the stack. Subclasses implement this for native functions and
/// interpreted functions.
class Callable : public JSObject {
  using Super = JSObject;
  friend void CallableBuildMeta(const GCCell *cell, Metadata::Builder &mb);

  /// Environment containing all captured variables for the function.
  GCPointer<Environment> environment_{};

  /// Dummy function for static asserts that may need private fields.
  static inline void staticAsserts();

 public:
  static bool classof(const GCCell *cell) {
    return kindInRange(
        cell->getKind(),
        CellKind::CallableKind_first,
        CellKind::CallableKind_last);
  }

  const CallableVTable *getVT() const {
    return static_cast<const CallableVTable *>(GCCell::getVT());
  }

  /// \return the environment associated with this callable.
  Environment *getEnvironment(PointerBase &runtime) const {
    return environment_.get(runtime);
  }

  enum class WritablePrototype : uint8_t { No = 0, Yes = 1 };

  /// Initialize the name, length and prototype property. This method factors
  /// out common code for initialization of callable-s.
  /// First, defines the ".name" property. It is set to the empty string if
  /// \p name is \c INVALID_IDENTIFIER_ID.
  /// Then it defines the ".length" property to \p paramCount.
  /// If \p prototypeObjectHandle is not null, sets the ".prototype" property
  /// to that value, and also defines ".prototype.constructor" to point back
  /// to the callable object.
  /// \param name identifier id of the function name, or
  ///   \c INVALID_IDENTIFIER_ID.
  /// \param paramCount number of declared named parameters
  /// \param prototypeObjectHandle optional value for .prototype
  /// \param writablePrototype determines whether the prototype property will
  ///   be created as writable or read-only.
  static ExecutionStatus defineNameLengthAndPrototype(
      Handle<Callable> selfHandle,
      Runtime &runtime,
      SymbolID name,
      unsigned paramCount,
      Handle<JSObject> prototypeObjectHandle,
      WritablePrototype writablePrototype);

  /// \return true if \p fn is a generator function.
  static bool isGeneratorFunction(Callable *fn);

  /// \return true if \p fn is an async function.
  static bool isAsyncFunction(Callable *fn);

  /// Execute this function with no arguments. This is just a convenience
  /// helper method; it actually invokes the interpreter recursively.
  static CallResult<PseudoHandle<>> executeCall0(
      Handle<Callable> selfHandle,
      Runtime &runtime,
      Handle<> thisArgHandle,
      bool construct = false);

  /// Execute this function with one argument. This is just a convenience
  /// helper method; it actually invokes the interpreter recursively.
  static CallResult<PseudoHandle<>> executeCall1(
      Handle<Callable> selfHandle,
      Runtime &runtime,
      Handle<> thisArgHandle,
      HermesValue param1,
      bool construct = false);

  /// Execute this function with two arguments. This is just a convenience
  /// helper method; it actually invokes the interpreter recursively.
  static CallResult<PseudoHandle<>> executeCall2(
      Handle<Callable> selfHandle,
      Runtime &runtime,
      Handle<> thisArgHandle,
      HermesValue param1,
      HermesValue param2,
      bool construct = false);

  /// Execute this function with three arguments. This is just a convenience
  /// helper method; it actually invokes the interpreter recursively.
  static CallResult<PseudoHandle<>> executeCall3(
      Handle<Callable> selfHandle,
      Runtime &runtime,
      Handle<> thisArgHandle,
      HermesValue param1,
      HermesValue param2,
      HermesValue param3,
      bool construct = false);

  /// Execute this function with four arguments. This is just a convenience
  /// helper method; it actually invokes the interpreter recursively.
  static CallResult<PseudoHandle<>> executeCall4(
      Handle<Callable> selfHandle,
      Runtime &runtime,
      Handle<> thisArgHandle,
      HermesValue param1,
      HermesValue param2,
      HermesValue param3,
      HermesValue param4,
      bool construct = false);

  /// Execute this function with given this and newTarget, and a
  /// variable number of arguments taken from arrayLike.  This invokes
  /// the interpreter recursively.
  static CallResult<PseudoHandle<>> executeCall(
      Handle<Callable> selfHandle,
      Runtime &runtime,
      Handle<> newTarget,
      Handle<> thisArgument,
      Handle<JSObject> arrayLike);

  /// Calls CallableVTable::call.
  static CallResult<PseudoHandle<>> call(
      Handle<Callable> selfHandle,
      Runtime &runtime) {
    // Any call to a native or JS function could potentially allocate.
    // Move the heap to force raw pointer errors to come out whenever a call is
    // made.
    runtime.potentiallyMoveHeap();
    return selfHandle->getVT()->call(selfHandle, runtime);
  }

  /// Call the callable in construct mode with arguments already on the stack.
  /// Checks the return value of the called function. If it is an object, then
  /// it is returned, else the `this` value is returned.
  static CallResult<PseudoHandle<>>
  construct(Handle<Callable> selfHandle, Runtime &runtime, Handle<> thisVal) {
    auto result = call(selfHandle, runtime);
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return (*result)->isObject()
        ? std::move(result)
        : CallResult<PseudoHandle<>>{PseudoHandle<>(thisVal)};
  }

  /// Create a new object and construct the new object of the given type
  /// by invoking \p selfHandle with construct=true.
  /// \param selfHandle the Callable from which to construct the new object.
  static CallResult<PseudoHandle<>> executeConstruct0(
      Handle<Callable> selfHandle,
      Runtime &runtime);

  /// Create a new object and construct the new object of the given type
  /// by invoking \p selfHandle with construct=true.
  /// \param selfHandle the Callable from which to construct the new object.
  /// \param param1 the first argument to the constructor.
  static CallResult<PseudoHandle<>> executeConstruct1(
      Handle<Callable> selfHandle,
      Runtime &runtime,
      Handle<> param1);

  /// If the own property ".length" is present and is a number, convert it to
  /// integer and return it. Otherwise return 0. Note that it is not guaranteed
  /// to be non-negative.
  /// Follows ES2018 19.2.3.2 5 and 6.
  static CallResult<double> extractOwnLengthProperty_RJS(
      Handle<Callable> selfHandle,
      Runtime &runtime);

  /// Define the length, name, prototype of this function, used when the
  /// creation has been delayed by lazy objects.
  static void defineLazyProperties(Handle<Callable> fn, Runtime &runtime);

  /// \return the `this` to be used for a construct call on \p callee, with \p
  /// newTarget as the new.target. We need take special care when \p callee is a
  /// NativeConstructor, ES6 function, or JSCallableProxy. In these cases, those
  /// functions will create their own `this`, so this function will \return
  /// undefined.
  /// \param callee the function to be called as constructor.
  /// \param newTarget is the new.target of the construct call.
  static CallResult<PseudoHandle<>> createThisForConstruct_RJS(
      Handle<> callee,
      Runtime &runtime,
      Handle<> newTarget);

  /// \return true if \p F is responsible for making its own `this` parameter
  /// when called as a constructor.
  static inline bool makesOwnThis(Callable *F) {
    return kindInRange(
        F->getKind(),
        CellKind::CallableMakesThisKind_first,
        CellKind::CallableMakesThisKind_last);
  }

 protected:
  Callable(
      Runtime &runtime,
      JSObject *parent,
      HiddenClass *clazz,
      Handle<Environment> env)
      : JSObject(runtime, parent, clazz),
        environment_(runtime, *env, runtime.getHeap()) {}

#ifdef HERMES_MEMORY_INSTRUMENTATION
  static std::string _snapshotNameImpl(GCCell *cell, GC &gc);
#endif
};

void Callable::staticAsserts() {
  static_assert(sizeof(Callable) == sizeof(SHCallable));
  static_assert(
      offsetof(Callable, environment_) == offsetof(SHCallable, environment));
}

Environment::Environment(
    Runtime &runtime,
    Handle<Callable> parentFn,
    uint32_t size)
    : parentEnvironment_(
          runtime,
          // TODO: Consider keeping the parent as a compressed pointer.
          parentFn->getEnvironment(runtime),
          runtime.getHeap()),
      size_(size) {
  // Initialize all slots to 'undefined'.
  GCHermesValue::uninitialized_fill(
      getSlots(),
      getSlots() + size,
      HermesValue::encodeUndefinedValue(),
      runtime.getHeap());
}

/// A function produced by Function.prototype.bind(). It packages a function
/// with values for some of its parameters.
class BoundFunction final : public Callable {
  friend void BoundFunctionBuildMeta(const GCCell *cell, Metadata::Builder &mb);

  /// The target function to call.
  GCPointer<Callable> target_;

  /// Storage for the arguments.
  GCPointer<ArrayStorage> argStorage_;

 public:
  using Super = Callable;
  static const CallableVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::BoundFunctionKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::BoundFunctionKind;
  }

  /// \return the target function.
  Callable *getTarget(PointerBase &runtime) const {
    return target_.get(runtime);
  }

  /// Creates a BoundFunction instance bound to \p target. It can throw if
  /// obtaining \c target.length throws.
  /// \param argCountWithThis number of arguments, including \c this. It can be
  ///     0 in which case there is no \c this.
  /// \param argsWithThis the arguments, \c this at index 0.
  static CallResult<HermesValue> create(
      Runtime &runtime,
      Handle<Callable> target,
      unsigned argCountWithThis,
      ConstArgIterator argsWithThis);

  /// Perform the actual call. This is a light-weight handler which is part of
  /// the private API - it is only used internally and by the interpreter.
  /// Other users of this class must use \c Callable::call().
  static CallResult<PseudoHandle<>> _boundCall(
      BoundFunction *self,
      Runtime &runtime);

  /// Initialize the length and name and property of a lazily created bound
  /// function.
  static ExecutionStatus initializeLengthAndName_RJS(
      Handle<Callable> selfHandle,
      Runtime &runtime,
      Handle<Callable> target,
      unsigned argCount);

  /// \return the number of arguments, including the 'this' param.
  unsigned getArgCountWithThis(PointerBase &runtime) const {
    return argStorage_.getNonNull(runtime)->size();
  }

 public:
  BoundFunction(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<Callable> target,
      Handle<ArrayStorage> argStorage)
      : Callable(
            runtime,
            *parent,
            *clazz,
            Runtime::makeNullHandle<Environment>()),
        target_(runtime, *target, runtime.getHeap()),
        argStorage_(runtime, *argStorage, runtime.getHeap()) {}

 private:
  /// Return a pointer to the stored arguments, including \c this. \c this is
  /// at index 0, followed by the rest.
  ArrayStorage::iterator getArgsWithThis(PointerBase &runtime) {
    return argStorage_.getNonNull(runtime)->begin();
  }

  /// Call the callable with arguments already on the stack.
  static CallResult<PseudoHandle<>> _callImpl(
      Handle<Callable> selfHandle,
      Runtime &runtime);

  /// Call a BoundFunction with arguments already on the stack, using the native
  /// calling convention.
  static HermesValue _jitCallImpl(Runtime *runtime, JSObject *self);
};

/// This class represents a native function callable from JavaScript with
/// context and the JavaScript arguments.
class NativeJSFunction : public Callable {
  friend void NativeJSFunctionBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);

 protected:
  /// Pointer to the actual code.
  const NativeJSFunctionPtr functionPtr_;
  /// Pointer to the information describing this function.
  const SHNativeFuncInfo *functionInfo_;
  /// Pointer to the SHUnit that created this function.
  const SHUnit *unit_;

#ifdef HERMESVM_PROFILER_NATIVECALL
  /// How many times the function was called.
  uint32_t callCount_{0};
  /// Total duration of all calls.
  uint64_t callDuration_{0};
#endif

 public:
  using Super = Callable;
  static const CallableVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::NativeJSFunctionKind;
  }
  static bool classof(const GCCell *cell) {
    switch (cell->getKind()) {
      case CellKind::NativeJSFunctionKind:
      case CellKind::NativeJSClassKind:
        return true;
      default:
        return false;
    }
  }

  NativeJSFunctionPtr getFunctionPtr() const {
    return functionPtr_;
  }

  const SHNativeFuncInfo *getFunctionInfo() const {
    return functionInfo_;
  }

  const SHUnit *getUnit() const {
    return unit_;
  }

#ifdef HERMESVM_PROFILER_NATIVECALL
  uint32_t getCallCount() const {
    return callCount_;
  }
  uint32_t getCallDuration() const {
    return callDuration_;
  }
#else
  uint32_t getCallCount() const {
    return 0;
  }
  uint32_t getCallDuration() const {
    return 0;
  }
#endif

  /// This is a lightweight and unsafe wrapper intended to be used only by the
  /// interpreter. Its purpose is to avoid needlessly exposing the private
  /// fields.
  static CallResult<PseudoHandle<>> _nativeCall(
      NativeJSFunction *self,
      Runtime &runtime);

  /// Call originating from LegacyJS.
  static SHLegacyValue _legacyCall(SHRuntime *shr, NativeJSFunction *self) {
    return self->functionPtr_(shr);
  }

  /// Create an instance of NativeJSFunction.
  /// \param parentHandle object to use as [[Prototype]].
  /// \param parentEnvHandle the parent environment
  /// \param context the context to be passed to the function
  /// \param functionPtr the native function
  /// \param funcInfo pointer to the information describing the function.
  static PseudoHandle<NativeJSFunction> create(
      Runtime &runtime,
      Handle<JSObject> parentHandle,
      Handle<Environment> parentEnvHandle,
      NativeJSFunctionPtr functionPtr,
      const SHNativeFuncInfo *funcInfo,
      const SHUnit *unit);

  /// Create a Function with the prototype property set to new Object(). The
  /// parent is inferred by the kind of the function.
  /// \param parentEnvHandle the parent environment
  /// \param context the context to be passed to the function
  /// \param functionPtr the native function
  /// \param funcInfo pointer to the information describing the function.
  static PseudoHandle<NativeJSFunction> createWithInferredParent(
      Runtime &runtime,
      Handle<Environment> parentEnvHandle,
      NativeJSFunctionPtr functionPtr,
      const SHNativeFuncInfo *funcInfo,
      const SHUnit *unit);

 public:
  NativeJSFunction(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<Environment> environment,
      NativeJSFunctionPtr functionPtr,
      const SHNativeFuncInfo *funcInfo,
      const SHUnit *unit)
      : Callable(runtime, *parent, *clazz, environment),
        functionPtr_(functionPtr),
        functionInfo_(funcInfo),
        unit_(unit) {}

 protected:
#ifdef HERMES_MEMORY_INSTRUMENTATION
  static std::string _snapshotNameImpl(GCCell *cell, GC &gc);
#endif

  /// Call the native function with arguments already on the stack.
  static CallResult<PseudoHandle<>> _callImpl(
      Handle<Callable> selfHandle,
      Runtime &runtime);

  /// Call a NativeJSFunction with arguments already on the stack, using the
  /// native calling convention.
  static HermesValue _jitCallImpl(Runtime *runtime, JSObject *self);

 private:
  /// Dummy function for static asserts that may need private fields.
  static inline void staticAsserts() {
    static_assert(sizeof(NativeJSFunction) == sizeof(SHNativeJSFunction));
    static_assert(
        offsetof(NativeJSFunction, functionPtr_) ==
        offsetof(SHNativeJSFunction, functionPtr));
  }
};

/// A NativeJSClass is a special kind of NativeJSFunction that is used to
/// implement class constructors.
class NativeJSClass : public NativeJSFunction {
  static constexpr auto kHasFinalizer = HasFinalizer::No;

 public:
  NativeJSClass(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<Environment> environment,
      NativeJSFunctionPtr functionPtr,
      const SHNativeFuncInfo *funcInfo,
      const SHUnit *unit)
      : NativeJSFunction(
            runtime,
            parent,
            clazz,
            environment,
            functionPtr,
            funcInfo,
            unit) {}
  static const CallableVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::NativeJSClassKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::NativeJSClassKind;
  }

  /// Create an instance of NativeJSClass.
  static PseudoHandle<NativeJSClass> create(
      Runtime &runtime,
      Handle<JSObject> parentHandle,
      Handle<Environment> parentEnvHandle,
      NativeJSFunctionPtr functionPtr,
      const SHNativeFuncInfo *funcInfo,
      const SHUnit *unit);
};

/// A pointer to native function.
typedef CallResult<HermesValue> (
    *NativeFunctionPtr)(void *context, Runtime &runtime);

/// This class represents a native function callable from JavaScript with
/// context and the JavaScript arguments.
class NativeFunction : public Callable {
  friend void NativeFunctionBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);

 protected:
  /// Context to be passed to the native function.
  void *const context_;
  /// Pointer to the actual code.
  const NativeFunctionPtr functionPtr_;

#ifdef HERMESVM_PROFILER_NATIVECALL
  /// How many times the function was called.
  uint32_t callCount_{0};
  /// Total duration of all calls.
  uint64_t callDuration_{0};
#endif

 public:
  using Super = Callable;
  static const CallableVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::NativeFunctionKind;
  }
  static bool classof(const GCCell *cell) {
    return kindInRange(
        cell->getKind(),
        CellKind::NativeFunctionKind_first,
        CellKind::NativeFunctionKind_last);
  }

  void *getContext() const {
    return context_;
  }

  NativeFunctionPtr getFunctionPtr() const {
    return functionPtr_;
  }

#ifdef HERMESVM_PROFILER_NATIVECALL
  uint32_t getCallCount() const {
    return callCount_;
  }
  uint32_t getCallDuration() const {
    return callDuration_;
  }
#else
  uint32_t getCallCount() const {
    return 0;
  }
  uint32_t getCallDuration() const {
    return 0;
  }
#endif

  /// Run \p body, which takes no arguments and returns
  /// CallResult<HermesValue>, as the body of a call of \p self: inside the
  /// native depth check and the frame setup and teardown that every call of
  /// a NativeFunction performs. The caller has pushed the frame (callee
  /// \p self, this and arguments), as for _nativeCall.
  template <typename Body>
  static inline CallResult<PseudoHandle<>>
  runNativeCallBody(NativeFunction *self, Runtime &runtime, const Body &body);

  /// This is a lightweight and unsafe wrapper intended to be used only by the
  /// interpreter. Its purpose is to avoid needlessly exposing the private
  /// fields.
  static CallResult<PseudoHandle<>> _nativeCall(
      NativeFunction *self,
      Runtime &runtime) {
    NativeFunctionPtr functionPtr = self->functionPtr_;
    void *context = self->context_;
    return runNativeCallBody(self, runtime, [functionPtr, context, &runtime]() {
      return functionPtr(context, runtime);
    });
  }

  /// Create an instance of NativeFunction with Function.prototype as the parent
  /// and the default HiddenClass.
  ///
  /// \param context the context to be passed to the function
  /// \param functionPtr the native function
  /// \param name the name property of the function.
  /// \param paramCount number of parameters (excluding `this`)
  static PseudoHandle<NativeFunction> create(
      Runtime &runtime,
      void *context,
      NativeFunctionPtr functionPtr,
      SymbolID name,
      unsigned paramCount) {
    return create(
        runtime,
        runtime.functionPrototype,
        runtime.classNativeFunction,
        Runtime::makeNullHandle<Environment>(),
        context,
        functionPtr,
        name,
        paramCount,
        Runtime::makeNullHandle<JSObject>());
  }

  /// Create an instance of NativeFunction.
  /// \param parentHandle object to use as [[Prototype]].
  /// \param clazz the HiddenClass for the result object.
  /// \param parentEnvHandle the parent environment
  /// \param context the context to be passed to the function
  /// \param functionPtr the native function
  /// \param name the name property of the function.
  /// \param paramCount number of parameters (excluding `this`)
  /// \param prototypeObjectHandle if non-null, set as prototype property.
  static PseudoHandle<NativeFunction> create(
      Runtime &runtime,
      Handle<JSObject> parentHandle,
      Handle<HiddenClass> clazz,
      Handle<Environment> parentEnvHandle,
      void *context,
      NativeFunctionPtr functionPtr,
      SymbolID name,
      unsigned paramCount,
      Handle<JSObject> prototypeObjectHandle);

  /// \return the value in an additional slot.
  /// \param index must be less than the \c additionalSlotCount passed to
  /// the create method.
  static SmallHermesValue getAdditionalSlotValue(
      NativeFunction *self,
      Runtime &runtime,
      unsigned index) {
    return JSObject::getInternalProperty(
        self, runtime, numOverlapSlots<NativeFunction>() + index);
  }

  /// Set the value in an additional slot.
  /// \param index must be less than the \c additionalSlotCount passed to
  /// the create method.
  static void setAdditionalSlotValue(
      NativeFunction *self,
      Runtime &runtime,
      unsigned index,
      SmallHermesValue value) {
    JSObject::setInternalProperty(
        self, runtime, numOverlapSlots<NativeFunction>() + index, value);
  }

  NativeFunction(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<Environment> environment,
      void *context,
      NativeFunctionPtr functionPtr)
      : Callable(runtime, *parent, *clazz, environment),
        context_(context),
        functionPtr_(functionPtr) {}

 protected:
#ifdef HERMES_MEMORY_INSTRUMENTATION
  static std::string _snapshotNameImpl(GCCell *cell, GC &gc);
#endif

  /// Call the native function with arguments already on the stack.
  static CallResult<PseudoHandle<>> _callImpl(
      Handle<Callable> selfHandle,
      Runtime &runtime);

  /// Call a NativeFunction with arguments already on the stack, using the
  /// native calling convention.
  static HermesValue _jitCallImpl(Runtime *runtime, JSObject *self);
};

template <typename Body>
inline CallResult<PseudoHandle<>> NativeFunction::runNativeCallBody(
    NativeFunction *self,
    Runtime &runtime,
    const Body &body) {
  ScopedNativeDepthTracker depthTracker{runtime};
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }

  // Ensure the IP was correctly saved before we create the new frame.
  runtime.validateSavedIPBeforeCall();
  auto newFrame = runtime.setCurrentFrameToTopOfStack();
  // We don't need to allocate CalleeExtraRegistersAtStart here because the
  // only register is DebugEnvironment, which is not useful in a
  // NativeFunction.
  static_assert(
      StackFrameLayout::FirstLocal - 1 == StackFrameLayout::DebugEnvironment &&
      StackFrameLayout::CalleeExtraRegistersAtStart == 1);

#ifdef HERMESVM_PROFILER_NATIVECALL
  auto t1 = HERMESVM_RDTSC();
#endif

  CallResult<HermesValue> res = body();

#ifdef HERMESVM_PROFILER_NATIVECALL
  // body() may have moved the heap, so \p self may be stale. Reload the
  // callee from the frame's callee slot, which the GC updates.
  self = vmcast<NativeFunction>(newFrame.getCalleeClosureOrCBRef());
  self->callDuration_ = HERMESVM_RDTSC() - t1;
  ++self->callCount_;
#else
  (void)self;
#endif
  runtime.restoreStackAndPreviousFrame(newFrame);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return createPseudoHandle(*res);
}

/// A NativeFunction to be used as a constructor for native objects other than
/// Object.
class NativeConstructor final : public NativeFunction {
 public:
  static const CallableVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::NativeConstructorKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::NativeConstructorKind;
  }

  /// Create an instance of NativeConstructor.
  /// \param parentHandle the __proto__ of the resulting constructor. Note this
  /// is NOT .prototype; use Callable::defineNameLengthAndPrototype to set that.
  /// \param context the context pointer to be passed to the native function
  /// \param functionPtr the native function
  /// \param paramCount number of parameters (excluding `this`)
  /// passed to the constructor
  /// \param targetKind the expected CellKind of objects produced by the
  /// constructor
  static PseudoHandle<NativeConstructor> create(
      Runtime &runtime,
      Handle<JSObject> parentHandle,
      void *context,
      NativeFunctionPtr functionPtr,
      unsigned paramCount) {
    auto *cell = runtime.makeAFixed<NativeConstructor>(
        runtime,
        parentHandle,
        runtime.getHiddenClassForPrototype(
            *parentHandle, runtime.classNativeConstructor),
        Runtime::makeNullHandle<Environment>(),
        context,
        functionPtr);
    return JSObjectInit::initToPseudoHandle(runtime, cell);
  }

  /// Obtain the correct parent for the `this` to be created for the current
  /// constructor call. First, fetch the .prototype of new.target. If it's an
  /// object, that is used as the parent. If not, then we use \p
  /// nativeCtorProto.
  /// \param newTarget is the new.target of the current construct call.
  /// \param nativeCtorProto is the .prototype of the native constructor that
  /// was called as a constructor.
  static CallResult<PseudoHandle<JSObject>> parentForNewThis_RJS(
      Runtime &runtime,
      Handle<Callable> newTarget,
      Handle<JSObject> nativeCtorProto);

  NativeConstructor(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<Environment> parentEnvHandle,
      void *context,
      NativeFunctionPtr functionPtr)
      : NativeFunction(
            runtime,
            parent,
            clazz,
            parentEnvHandle,
            context,
            functionPtr) {}

 private:
#ifndef NDEBUG
  /// If construct=true, check that the constructor was called with a "this"
  /// of the correct type.
  static CallResult<PseudoHandle<>> _callImpl(
      Handle<Callable> selfHandle,
      Runtime &runtime);
#endif
};

/// An interpreted callable function with environment.
class JSFunction : public Callable {
  using Super = Callable;
  friend void JSFunctionBuildMeta(const GCCell *cell, Metadata::Builder &mb);
  friend struct RuntimeOffsets;

  /// CodeBlock to execute when called.
  CodeBlock *codeBlock_;

  static constexpr auto kHasFinalizer = HasFinalizer::No;

  /// JSFunctions must keep their domains alive.
  GCPointer<Domain> domain_;

 public:
  JSFunction(
      Runtime &runtime,
      Handle<Domain> domain,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<Environment> environment,
      CodeBlock *codeBlock)
      : Callable(runtime, *parent, *clazz, environment),
        codeBlock_(codeBlock),
        domain_(runtime, *domain, runtime.getHeap()) {
    assert(
        !vt.finalize_ == (kHasFinalizer != HasFinalizer::Yes) &&
        "kHasFinalizer invalid value");
  }

 public:
  static const CallableVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSFunctionKind;
  }
  static bool classof(const GCCell *cell) {
    return kindInRange(
        cell->getKind(),
        CellKind::CodeBlockFunctionKind_first,
        CellKind::CodeBlockFunctionKind_last);
  }

  /// Create a Function with the prototype property set to new Object().
  static PseudoHandle<JSFunction> create(
      Runtime &runtime,
      Handle<Domain> domain,
      Handle<JSObject> parentHandle,
      Handle<Environment> envHandle,
      CodeBlock *codeBlock);

  /// Create a Function with the prototype property set to new Object(). The
  /// parent is inferred by the CodeBlock's function header information.
  static PseudoHandle<JSFunction> createWithInferredParent(
      Runtime &runtime,
      Handle<Domain> domain,
      Handle<Environment> envHandle,
      CodeBlock *codeBlock);

  /// Create a Function with no environment and a CodeBlock simply returning
  /// undefined, with the prototype property auto-initialized to new Object().
  static PseudoHandle<JSFunction> create(
      Runtime &runtime,
      Handle<Domain> domain,
      Handle<JSObject> parentHandle) {
    return create(
        runtime,
        domain,
        parentHandle,
        Runtime::makeNullHandle<Environment>(),
        runtime.getEmptyCodeBlock());
  }

  /// A wrapper to convert a SH-style function to CallResult style.
  static CallResult<HermesValue> _jittedCall(
      JITCompiledFunctionPtr functionPtr,
      Runtime &runtime);

  /// Create a Function with no environment and a CodeBlock simply returning
  /// undefined, with the prototype property auto-initialized to new Object().
  static PseudoHandle<JSFunction> create(
      Runtime &runtime,
      Handle<JSObject> parentHandle) {
    struct : public Locals {
      PinnedValue<Domain> domain;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    lv.domain = Domain::create(runtime);
    return create(runtime, lv.domain, parentHandle);
  }

  /// \return the code block containing the function code.
  CodeBlock *getCodeBlock() const {
    return codeBlock_;
  }

  RuntimeModule *getRuntimeModule() const {
    return getCodeBlock()->getRuntimeModule();
  }

  /// Add a source location for a function in a heap snapshot.
  /// \param snap The snapshot to add a location to.
  /// \param id The object id to annotate with the location.
  void addLocationToSnapshot(
      HeapSnapshot &snap,
      HeapSnapshot::NodeID id,
      GC &gc) const;

  /// A helper to call the interpreter on this code block.
  CallResult<HermesValue> _interpret(Runtime &runtime) {
    return runtime.interpretFunction(codeBlock_);
  }

 protected:
  /// Call the JavaScript function with arguments already on the stack.
  static CallResult<PseudoHandle<>> _callImpl(
      Handle<Callable> selfHandle,
      Runtime &runtime);

  /// Call a JSFunction with arguments already on the stack, using the native
  /// calling convention.
  static HermesValue _jitCallImpl(Runtime *runtime, JSObject *self);

#ifdef HERMES_MEMORY_INSTRUMENTATION
  static std::string _snapshotNameImpl(GCCell *cell, GC &gc);
  static void
  _snapshotAddLocationsImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
  static void _snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
#endif
};

/// A JSClass is a special kind of JSFunction that is used to implement class
/// constructors.
class JSClass : public JSFunction {
  static constexpr auto kHasFinalizer = HasFinalizer::No;

 public:
  JSClass(
      Runtime &runtime,
      Handle<Domain> domain,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<Environment> environment,
      CodeBlock *codeBlock)
      : JSFunction(runtime, domain, parent, clazz, environment, codeBlock) {}
  static const CallableVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSClassKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSClassKind;
  }
  /// Create an instance of JSClass.
  static PseudoHandle<JSClass> create(
      Runtime &runtime,
      Handle<Domain> domain,
      Handle<JSObject> parentHandle,
      Handle<Environment> envHandle,
      CodeBlock *codeBlock);
};

/// ES2023 15.7.14 ClassDefinitionEvaluation
/// \p superHandle contains the value of the super class. If this is a base
///    class, it is empty.
/// \p makeClassCons is a function that will be invoked to actually create the
///    class constructor. It is given the parent of the class constructor and
///    should use it to set the parent of the created callable. This is factored
///    out so that this function can be shared between the native and BC
///    backends, which create different callable types.
/// \return <class callable, home object>. The home object is the class'
///    .prototype property.
template <typename MakeClassConstructor>
static CallResult<std::pair<Callable *, JSObject *>> createClass(
    Runtime &runtime,
    Handle<> superHandle,
    MakeClassConstructor makeClassCons) {
  struct : public Locals {
    PinnedValue<JSObject> protoParent;
    PinnedValue<JSObject> ctorParent;
    PinnedValue<Callable> F;
    PinnedValue<JSObject> homeObject;
    PinnedValue<> badProtoParent;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  GCScopeMarkerRAII marker{runtime};

  // ES2023 15.7.14 RS: ClassDefinitionEvaluation
  // 7. If ClassHeritageopt is not present, then
  if (superHandle->isEmpty()) {
    //   a. Let protoParent be %Object.prototype%.
    lv.protoParent = runtime.objectPrototype;
    //   b. Let constructorParent be %Function.prototype%.
    lv.ctorParent = runtime.functionPrototype;
  } else {
    // 8. Else,
    //   f. If superclass is null, then
    if (superHandle->isNull()) {
      //     i. Let protoParent be null.
      lv.protoParent = Runtime::makeNullHandle<JSObject>();
      //     ii. Let constructorParent be %Function.prototype%.
      lv.ctorParent = runtime.functionPrototype;
    } else {
      //   g. Else if IsConstructor(superclass) is false, throw a TypeError
      //      exception.
      CallResult<bool> isCtorRes = isConstructor(runtime, superHandle.get());
      if (LLVM_UNLIKELY(isCtorRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (!*isCtorRes) {
        return runtime.raiseTypeErrorForValue(
            superHandle, " is not a constructor");
      }
      //   h. Else,
      auto superObj = Handle<JSObject>::vmcast(superHandle);
      //     i. Let protoParent be ? Get(superclass, "prototype").
      CallResult<PseudoHandle<>> protoParentRes = JSObject::getNamed_RJS(
          superObj, runtime, Predefined::getSymbolID(Predefined::prototype));
      if (LLVM_UNLIKELY(protoParentRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      PseudoHandle<> protoParentPH = std::move(*protoParentRes);
      //     ii. If protoParent is not an Object and protoParent is not
      //         null, throw a TypeError exception.
      if (protoParentPH->isNull()) {
        lv.protoParent = Runtime::makeNullHandle<JSObject>();
      } else if (
          auto *protoParentObj = dyn_vmcast<JSObject>(protoParentPH.get())) {
        lv.protoParent = protoParentObj;
      } else {
        lv.badProtoParent = std::move(protoParentPH);
        return runtime.raiseTypeErrorForValue(
            "Base class .prototype ",
            lv.badProtoParent,
            " is neither null nor an object");
      }
      //     iii. Let constructorParent be superclass.
      lv.ctorParent = superObj.get();
    }
  }

  lv.homeObject = JSObject::create(runtime, lv.protoParent);
  lv.F = makeClassCons(lv.ctorParent);

  // 16. Perform MakeConstructor(F, false, proto).
  auto ctorProp = DefinePropertyFlags::getNewNonEnumerableFlags();
  ctorProp.writable = 0;
  ctorProp.configurable = 0;
  CallResult<bool> makeConstructorRes = JSObject::defineOwnProperty(
      lv.F,
      runtime,
      Predefined::getSymbolID(Predefined::prototype),
      ctorProp,
      lv.homeObject);
  if (LLVM_UNLIKELY(makeConstructorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(*makeConstructorRes && "failed to add F.protoype");

  // 18. Perform CreateMethodProperty(proto, "constructor", F).
  CallResult<bool> createMethodRes = JSObject::defineOwnProperty(
      lv.homeObject,
      runtime,
      Predefined::getSymbolID(Predefined::constructor),
      DefinePropertyFlags::getNewNonEnumerableFlags(),
      lv.F);
  if (LLVM_UNLIKELY(createMethodRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(*createMethodRes && "failed to add proto.constructor");
  return std::make_pair(lv.F.get(), *lv.homeObject);
}
} // namespace vm
} // namespace hermes

#endif // HERMES_VM_CALLABLE_H
