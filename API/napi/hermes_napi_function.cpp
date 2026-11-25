/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/Callable.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/NativeArgs.h"
#include "hermes/VM/PropertyAccessor.h"

#include <algorithm>
#include <cstring>
#include <vector>

//===========================================================================
// Callback info extraction
//===========================================================================

napi_status NAPI_CDECL napi_get_cb_info(
    napi_env env,
    napi_callback_info cbinfo,
    size_t *argc,
    napi_value *argv,
    napi_value *this_arg,
    void **data) {
  CHECK_ENV(env);
  CHECK_ARG(env, cbinfo);

  // Copy arguments into the caller's argv array, adding each to the
  // current handle scope so they remain valid for the scope's lifetime.
  if (argv != nullptr) {
    CHECK_ARG(env, argc);

    size_t actualCount = cbinfo->argc;
    size_t requestedCount = *argc;
    size_t copyCount = std::min(requestedCount, actualCount);

    // Copy actual arguments.
    for (size_t i = 0; i < copyCount; ++i) {
      argv[i] = env->addToCurrentScope(cbinfo->getArg(i));
    }
    // Fill remaining slots with undefined.
    for (size_t i = copyCount; i < requestedCount; ++i) {
      argv[i] = env->addToCurrentScope(
          hermes::vm::HermesValue::encodeUndefinedValue());
    }
  }

  // Set the actual argument count.
  if (argc != nullptr) {
    *argc = cbinfo->argc;
  }

  // Set the 'this' value, adding it to the handle scope.
  if (this_arg != nullptr) {
    *this_arg = env->addToCurrentScope(*cbinfo->thisArg);
  }

  // Set the user data pointer.
  if (data != nullptr) {
    *data = cbinfo->data;
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_new_target(
    napi_env env,
    napi_callback_info cbinfo,
    napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, cbinfo);
  CHECK_ARG(env, result);

  // new.target is undefined for regular function calls, or the
  // constructor callable when invoked via 'new'.
  if (cbinfo->newTarget->isUndefined()) {
    *result = nullptr;
  } else {
    *result = env->addToCurrentScope(*cbinfo->newTarget);
  }

  return napi_clear_last_error(env);
}

//===========================================================================
// Native function trampoline
//===========================================================================

/// The trampoline function that bridges Hermes's NativeFunctionPtr
/// signature with NAPI's napi_callback signature. This is the C++
/// function pointer stored in every NativeFunction created via
/// napi_create_function or napi_define_properties.
///
/// It:
///   1. Opens a handle scope for the callback's lifetime.
///   2. Constructs a napi_callback_info__ from NativeArgs.
///   3. Calls the user's napi_callback.
///   4. Handles the return value and pending exceptions.
///   5. Closes the handle scope.
hermes::vm::CallResult<hermes::vm::HermesValue> napiFunctionTrampoline(
    void *context,
    hermes::vm::Runtime &runtime) {
  using namespace hermes::vm;

  auto *bundle = static_cast<CallbackBundle *>(context);
  napi_env env = bundle->env;

  // Get the native args from the current stack frame.
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // Open a handle scope for the callback. All napi_values created
  // by the callback will live in this scope.
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) {
    return runtime.raiseTypeError("Failed to open handle scope");
  }

  // Build the callback info from the stack frame.
  napi_callback_info__ cbinfo;
  cbinfo.env = env;
  cbinfo.thisArg = &args.getThisArg();
  cbinfo.argsBase = &args.getThisArg();
  cbinfo.argc = args.getArgCount();
  cbinfo.data = bundle->data;
  cbinfo.newTarget = &args.getNewTarget();

  // Call the user's NAPI callback.
  napi_value result = bundle->cb(env, &cbinfo);

  // Handle the result. There are three cases:
  // 1. The callback set a pending exception → propagate to runtime.
  // 2. The callback returned non-null → use as return value.
  // 3. The callback returned null → return undefined.

  if (env->hasPendingException) {
    // Transfer the NAPI pending exception to the runtime's thrown
    // value. The caller (Hermes interpreter) will see the exception.
    runtime.setThrownValue(env->pendingException);
    env->pendingException = HermesValue::encodeUndefinedValue();
    env->hasPendingException = false;
    napi_close_handle_scope(env, scope);
    return ExecutionStatus::EXCEPTION;
  }

  if (result != nullptr) {
    // Read the return value from the handle scope slot before
    // closing the scope (closing invalidates the slot).
    auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
    // Use the PinnedHermesValue to construct a return value.
    // PinnedHermesValue inherits from HermesValue so the implicit
    // conversion to HermesValue creates a temporary rvalue.
    HermesValue ret = HermesValue::encodeUndefinedValue();
    // Copy the raw bits. PinnedHermesValue -> HermesValue static_cast
    // creates an rvalue that CallResult can accept.
    std::memcpy(&ret, phv, sizeof(HermesValue));
    napi_close_handle_scope(env, scope);
    return ret;
  }

  napi_close_handle_scope(env, scope);
  return HermesValue::encodeUndefinedValue();
}

//===========================================================================
// Function creation
//===========================================================================

napi_status NAPI_CDECL napi_create_function(
    napi_env env,
    const char *utf8name,
    size_t length,
    napi_callback cb,
    void *data,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  CHECK_ARG(env, cb);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;
  GCScope gcScope(runtime);
  struct : public Locals {
    PinnedValue<Callable> func;
    PinnedValue<JSObject> proto;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Allocate a callback bundle owned by the env. The bundle's
  // address is stable because we use a deque.
  env->callbackBundles_.push_back({env, cb, data});
  CallbackBundle *bundle = &env->callbackBundles_.back();

  // Determine the function name symbol.
  SymbolID nameSym = Predefined::getSymbolID(Predefined::emptyString);
  if (utf8name != nullptr) {
    size_t nameLen =
        (length == NAPI_AUTO_LENGTH) ? std::strlen(utf8name) : length;
    if (nameLen > 0) {
      auto strRes = StringPrimitive::createEfficient(
          runtime,
          UTF8Ref(reinterpret_cast<const uint8_t *>(utf8name), nameLen));
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        // Remove the bundle we just added since we're failing.
        env->callbackBundles_.pop_back();
        return captureRuntimeException(env, napi_generic_failure);
      }
      auto symRes = runtime.getIdentifierTable().getSymbolHandleFromPrimitive(
          runtime,
          PseudoHandle<StringPrimitive>::create(
              vmcast<StringPrimitive>(*strRes)));
      if (LLVM_UNLIKELY(symRes == ExecutionStatus::EXCEPTION)) {
        env->callbackBundles_.pop_back();
        return captureRuntimeException(env, napi_generic_failure);
      }
      nameSym = **symRes;
    }
  }

  // Create a NativeConstructor so the function is callable with 'new'.
  // All NAPI functions should be constructable, matching V8/Node behavior.
  // napiConstructorTrampoline handles both regular calls and constructor
  // calls (creating a 'this' object when called via 'new').
  auto funcHandle = NativeConstructor::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime.functionPrototype),
      bundle,
      napiConstructorTrampoline,
      0);

  // Root the function handle before creating the prototype, since
  // JSObject::create is a GC safepoint that can invalidate the
  // PseudoHandle.
  lv.func.castAndSetHermesValue<Callable>(funcHandle.getHermesValue());

  // Create a prototype object so the function can be used as a base class
  // (class Foo extends napiFunc {}). Without .prototype, JS would see
  // undefined and throw "Base class .prototype is neither null nor object".
  auto protoObj = JSObject::create(runtime);
  lv.proto = std::move(protoObj);

  // Set .name, .length, and .prototype properties.
  auto defRes = Callable::defineNameLengthAndPrototype(
      lv.func,
      runtime,
      nameSym,
      0, // paramCount (.length)
      lv.proto,
      Callable::WritablePrototype::Yes);
  if (LLVM_UNLIKELY(defRes == ExecutionStatus::EXCEPTION)) {
    env->callbackBundles_.pop_back();
    return captureRuntimeException(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(lv.func.getHermesValue());
  return napi_clear_last_error(env);
}

//===========================================================================
// Function invocation
//===========================================================================

napi_status NAPI_CDECL napi_call_function(
    napi_env env,
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

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  // Validate that func is a callable.
  auto *phvFunc = reinterpret_cast<const PinnedHermesValue *>(func);
  CHECK_ARG(env, phvFunc);
  RETURN_STATUS_IF_FALSE(
      env, phvFunc->isObject() && vmisa<Callable>(*phvFunc), napi_invalid_arg);

  auto *callable = vmcast<Callable>(*phvFunc);

  // A GCScope is needed because ScopedNativeCallFrame and
  // Callable::call may allocate internal handles.
  GCScope gcScope(runtime);

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
// Constructor invocation
//===========================================================================

napi_status NAPI_CDECL napi_new_instance(
    napi_env env,
    napi_value constructor,
    size_t argc,
    const napi_value *argv,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, constructor);
  if (argc > 0) {
    CHECK_ARG(env, argv);
  }
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  // Validate that the constructor is a callable.
  auto *phvCtor = reinterpret_cast<const PinnedHermesValue *>(constructor);
  RETURN_STATUS_IF_FALSE(
      env, phvCtor->isObject() && vmisa<Callable>(*phvCtor), napi_invalid_arg);

  struct : public Locals {
    PinnedValue<> thisArg;
    PinnedValue<> proto;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // A GCScope is needed because getNamed_RJS and JSObject::create
  // may internally allocate temporary Handle objects.
  GCScope gcScope(runtime);

  // Get the constructor's .prototype property. If it's an
  // object, use it as the new object's prototype; otherwise
  // fall back to Object.prototype.
  // We do this manually instead of using
  // Callable::createThisForConstruct_RJS because NativeFunction
  // is not considered a constructor by isConstructor(), but NAPI
  // requires napi_new_instance to work with any callable.
  auto protoRes = JSObject::getNamed_RJS(
      Handle<JSObject>::vmcast(phvCtor),
      runtime,
      Predefined::getSymbolID(Predefined::prototype));
  if (LLVM_UNLIKELY(protoRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }
  lv.proto = std::move(*protoRes);

  // Create the 'this' object with the resolved prototype.
  auto thisObj = JSObject::create(
      runtime,
      lv.proto->isObject()
          ? Handle<JSObject>::vmcast(&lv.proto)
          : Handle<JSObject>::vmcast(&runtime.objectPrototype));
  lv.thisArg = thisObj.getHermesValue();

  // Build the call frame with newTarget = constructor.
  ScopedNativeCallFrame newFrame{
      runtime,
      static_cast<uint32_t>(argc),
      *phvCtor, // callee
      *phvCtor, // newTarget = constructor
      *lv.thisArg};
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
  auto callRes = Callable::call(Handle<Callable>::vmcast(phvCtor), runtime);
  if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  // Constructor semantics: if the function returned an object,
  // use that; otherwise use the 'this' object.
  if ((*callRes)->isObject()) {
    *result = env->addToCurrentScope(callRes->get());
  } else {
    *result = env->addToCurrentScope(lv.thisArg.getHermesValue());
  }

  return napi_clear_last_error(env);
}

//===========================================================================
// Constructor trampoline for napi_define_class
//===========================================================================

/// A trampoline for NativeConstructor-based constructors created by
/// napi_define_class. Unlike napiFunctionTrampoline (used for regular
/// NativeFunction), this handles the "makes own this" semantics:
///
/// When called as a constructor (new.target is set):
///   1. Gets the prototype from new.target's .prototype property
///   2. Creates a new JSObject with that prototype as 'this'
///   3. Passes this new object as 'this' to the NAPI callback
///   4. Applies constructor return semantics (use returned object if
///      any, otherwise use the constructed 'this')
///
/// When called as a regular function (new.target is undefined):
///   Behaves identically to napiFunctionTrampoline.
hermes::vm::CallResult<hermes::vm::HermesValue> napiConstructorTrampoline(
    void *context,
    hermes::vm::Runtime &runtime) {
  using namespace hermes::vm;

  auto *bundle = static_cast<CallbackBundle *>(context);
  napi_env env = bundle->env;

  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSObject> parent;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Open a handle scope for the callback.
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) {
    return runtime.raiseTypeError("Failed to open handle scope");
  }

  napi_callback_info__ cbinfo;
  cbinfo.env = env;
  cbinfo.argsBase = &args.getThisArg();
  cbinfo.argc = args.getArgCount();
  cbinfo.data = bundle->data;
  cbinfo.newTarget = &args.getNewTarget();

  if (!args.getNewTarget().isUndefined()) {
    // Constructor call: create a new 'this' object.
    GCScope gcScope(runtime);

    auto newTarget = Handle<Callable>::vmcast(&args.getNewTarget());

    // Get the prototype object for the new 'this'. Uses
    // new.target's .prototype if it's an object, otherwise falls
    // back to Object.prototype.
    auto protoRes = NativeConstructor::parentForNewThis_RJS(
        runtime, newTarget, Handle<JSObject>::vmcast(&runtime.objectPrototype));
    if (LLVM_UNLIKELY(protoRes == ExecutionStatus::EXCEPTION)) {
      napi_close_handle_scope(env, scope);
      return ExecutionStatus::EXCEPTION;
    }

    lv.parent = std::move(*protoRes);
    auto thisObj = JSObject::create(runtime, lv.parent);

    // Store the 'this' object in the handle scope so it's rooted
    // across the user's callback (which may trigger GC).
    napi_value thisSlot = env->addToCurrentScope(thisObj.getHermesValue());
    cbinfo.thisArg = reinterpret_cast<PinnedHermesValue *>(thisSlot);
  } else {
    // Regular function call.
    cbinfo.thisArg = &args.getThisArg();
  }

  // Call the user's NAPI callback.
  napi_value result = bundle->cb(env, &cbinfo);

  // Handle the result.
  if (env->hasPendingException) {
    runtime.setThrownValue(env->pendingException);
    env->pendingException = HermesValue::encodeUndefinedValue();
    env->hasPendingException = false;
    napi_close_handle_scope(env, scope);
    return ExecutionStatus::EXCEPTION;
  }

  if (!args.getNewTarget().isUndefined()) {
    // Constructor call: apply constructor return semantics.
    // If the callback returned a non-null value that is an object,
    // use that; otherwise use the 'this' object.
    if (result != nullptr) {
      auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
      HermesValue ret = HermesValue::encodeUndefinedValue();
      std::memcpy(&ret, phv, sizeof(HermesValue));
      if (ret.isObject()) {
        napi_close_handle_scope(env, scope);
        return ret;
      }
    }
    // Return the constructed 'this' object.
    HermesValue thisVal = HermesValue::encodeUndefinedValue();
    std::memcpy(&thisVal, cbinfo.thisArg, sizeof(HermesValue));
    napi_close_handle_scope(env, scope);
    return thisVal;
  }

  // Regular function call: same as napiFunctionTrampoline.
  if (result != nullptr) {
    auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
    HermesValue ret = HermesValue::encodeUndefinedValue();
    std::memcpy(&ret, phv, sizeof(HermesValue));
    napi_close_handle_scope(env, scope);
    return ret;
  }

  napi_close_handle_scope(env, scope);
  return HermesValue::encodeUndefinedValue();
}

//===========================================================================
// Class definition
//===========================================================================

napi_status NAPI_CDECL napi_define_class(
    napi_env env,
    const char *utf8name,
    size_t length,
    napi_callback constructor,
    void *data,
    size_t property_count,
    const napi_property_descriptor *properties,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  CHECK_ARG(env, constructor);
  CHECK_ARG(env, utf8name);

  if (property_count > 0) {
    CHECK_ARG(env, properties);
  }

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  // Use Locals for values that must survive across GCScope flushes.
  struct : public Locals {
    PinnedValue<Callable> ctor;
    PinnedValue<JSObject> proto;
    PinnedValue<SymbolID> name;
    PinnedValue<Callable> getter;
    PinnedValue<Callable> setter;
    PinnedValue<PropertyAccessor> accessor;
    PinnedValue<NativeFunction> method;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // GCScope is needed because NativeConstructor::create,
  // defineNameLengthAndPrototype, and property definition APIs
  // allocate temporary Handle objects internally.
  GCScope gcScope(runtime);
  auto marker = gcScope.createMarker();

  // Allocate a callback bundle for the constructor.
  env->callbackBundles_.push_back({env, constructor, data});
  CallbackBundle *bundle = &env->callbackBundles_.back();

  // Resolve the class name to a SymbolID.
  lv.name = Predefined::getSymbolID(Predefined::emptyString);
  if (utf8name != nullptr) {
    size_t nameLen =
        (length == NAPI_AUTO_LENGTH) ? std::strlen(utf8name) : length;
    if (nameLen > 0) {
      auto strRes = StringPrimitive::createEfficient(
          runtime,
          UTF8Ref(reinterpret_cast<const uint8_t *>(utf8name), nameLen));
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        env->callbackBundles_.pop_back();
        return captureRuntimeException(env, napi_generic_failure);
      }
      auto symRes = runtime.getIdentifierTable().getSymbolHandleFromPrimitive(
          runtime,
          PseudoHandle<StringPrimitive>::create(
              vmcast<StringPrimitive>(*strRes)));
      if (LLVM_UNLIKELY(symRes == ExecutionStatus::EXCEPTION)) {
        env->callbackBundles_.pop_back();
        return captureRuntimeException(env, napi_generic_failure);
      }
      lv.name = **symRes;
    }
  }

  gcScope.flushToMarker(marker);

  // Create the NativeConstructor. Using NativeConstructor (not
  // NativeFunction) ensures isConstructor() returns true, so
  // 'new MyClass()' works from JavaScript.
  auto ctorPH = NativeConstructor::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime.functionPrototype),
      bundle,
      napiConstructorTrampoline,
      0 /* paramCount */);
  lv.ctor.castAndSetHermesValue<Callable>(ctorPH.getHermesValue());

  gcScope.flushToMarker(marker);

  // Create the prototype object.
  auto protoObj = JSObject::create(runtime);
  lv.proto.castAndSetHermesValue<JSObject>(protoObj.getHermesValue());

  // Set up constructor.prototype = protoObj and
  // protoObj.constructor = constructor, plus .name and .length.
  // defineNameLengthAndPrototype handles all of this.
  auto defRes = Callable::defineNameLengthAndPrototype(
      lv.ctor,
      runtime,
      *lv.name,
      0, // paramCount for .length
      lv.proto,
      Callable::WritablePrototype::Yes);
  if (LLVM_UNLIKELY(defRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  gcScope.flushToMarker(marker);

  // Separate instance properties from static properties.
  // Instance properties go on the prototype; static properties
  // go on the constructor.
  std::vector<napi_property_descriptor> staticDescs;

  for (size_t i = 0; i < property_count; ++i) {
    gcScope.flushToMarker(marker);

    const napi_property_descriptor *p = &properties[i];

    if ((p->attributes & napi_static) != 0) {
      staticDescs.push_back(*p);
      continue;
    }

    // Instance property: define on the prototype.
    // Resolve the property key.
    SymbolID propertySym{};
    if (p->utf8name != nullptr) {
      CallResult<Handle<SymbolID>> symRes = ExecutionStatus::EXCEPTION;
      auto internStatus = internUtf8AsSymbol(env, p->utf8name, symRes);
      if (internStatus != napi_ok) {
        return internStatus;
      }
      propertySym = **symRes;
    } else {
      auto *namePhv = reinterpret_cast<PinnedHermesValue *>(p->name);
      RETURN_STATUS_IF_FALSE(
          env,
          namePhv != nullptr && (namePhv->isString() || namePhv->isSymbol()),
          napi_name_expected);

      if (namePhv->isString()) {
        auto symRes = runtime.getIdentifierTable().getSymbolHandleFromPrimitive(
            runtime,
            PseudoHandle<StringPrimitive>::create(
                vmcast<StringPrimitive>(*namePhv)));
        if (LLVM_UNLIKELY(symRes == ExecutionStatus::EXCEPTION)) {
          return captureRuntimeException(env, napi_pending_exception);
        }
        propertySym = **symRes;
      } else {
        propertySym = namePhv->getSymbol();
      }
    }

    if (p->getter != nullptr || p->setter != nullptr) {
      // Accessor property on prototype.
      Handle<Callable> getterHandle = Runtime::makeNullHandle<Callable>();
      Handle<Callable> setterHandle = Runtime::makeNullHandle<Callable>();

      if (p->getter != nullptr) {
        PseudoHandle<NativeFunction> getterFunc{};
        STATUS_CALL(createNapiNativeFunction(
            env, p->getter, p->data, propertySym, 0, getterFunc));
        lv.getter.castAndSetHermesValue<Callable>(getterFunc.getHermesValue());
        getterHandle = lv.getter;
      }
      if (p->setter != nullptr) {
        PseudoHandle<NativeFunction> setterFunc{};
        STATUS_CALL(createNapiNativeFunction(
            env, p->setter, p->data, propertySym, 1, setterFunc));
        lv.setter.castAndSetHermesValue<Callable>(setterFunc.getHermesValue());
        setterHandle = lv.setter;
      }

      auto accessor =
          PropertyAccessor::create(runtime, getterHandle, setterHandle);
      lv.accessor = std::move(accessor);

      DefinePropertyFlags dpf{};
      dpf.setEnumerable = 1;
      dpf.enumerable = (p->attributes & napi_enumerable) ? 1 : 0;
      dpf.setConfigurable = 1;
      dpf.configurable = (p->attributes & napi_configurable) ? 1 : 0;
      dpf.setGetter = 1;
      dpf.setSetter = 1;

      auto propRes = JSObject::defineOwnProperty(
          lv.proto,
          runtime,
          propertySym,
          dpf,
          lv.accessor,
          PropOpFlags().plusThrowOnError());
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return captureRuntimeException(env, napi_pending_exception);
      }
      RETURN_STATUS_IF_FALSE(env, *propRes, napi_invalid_arg);
    } else if (p->method != nullptr) {
      // Method property on prototype.
      PseudoHandle<NativeFunction> methodFunc{};
      STATUS_CALL(createNapiNativeFunction(
          env, p->method, p->data, propertySym, 0, methodFunc));
      lv.method = std::move(methodFunc);

      DefinePropertyFlags dpf{};
      dpf.setEnumerable = 1;
      dpf.enumerable = (p->attributes & napi_enumerable) ? 1 : 0;
      dpf.setWritable = 1;
      dpf.writable = (p->attributes & napi_writable) ? 1 : 0;
      dpf.setConfigurable = 1;
      dpf.configurable = (p->attributes & napi_configurable) ? 1 : 0;
      dpf.setValue = 1;

      auto propRes = JSObject::defineOwnProperty(
          lv.proto,
          runtime,
          propertySym,
          dpf,
          lv.method,
          PropOpFlags().plusThrowOnError());
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return captureRuntimeException(env, napi_pending_exception);
      }
      RETURN_STATUS_IF_FALSE(env, *propRes, napi_generic_failure);
    } else {
      // Data property on prototype.
      auto *valPhv = reinterpret_cast<PinnedHermesValue *>(p->value);
      auto valHandle = Handle<>(valPhv);

      DefinePropertyFlags dpf{};
      dpf.setEnumerable = 1;
      dpf.enumerable = (p->attributes & napi_enumerable) ? 1 : 0;
      dpf.setWritable = 1;
      dpf.writable = (p->attributes & napi_writable) ? 1 : 0;
      dpf.setConfigurable = 1;
      dpf.configurable = (p->attributes & napi_configurable) ? 1 : 0;
      dpf.setValue = 1;

      auto propRes = JSObject::defineOwnProperty(
          lv.proto,
          runtime,
          propertySym,
          dpf,
          valHandle,
          PropOpFlags().plusThrowOnError());
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return captureRuntimeException(env, napi_pending_exception);
      }
      RETURN_STATUS_IF_FALSE(env, *propRes, napi_invalid_arg);
    }
  }

  // Store the constructor in the result handle scope.
  *result = env->addToCurrentScope(lv.ctor.getHermesValue());

  // Define static properties on the constructor using
  // napi_define_properties. This reuses the existing property
  // definition logic.
  if (!staticDescs.empty()) {
    STATUS_CALL(napi_define_properties(
        env, *result, staticDescs.size(), staticDescs.data()));
  }

  return napi_clear_last_error(env);
}
