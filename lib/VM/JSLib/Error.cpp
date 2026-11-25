/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES5.1 15.11 Initialize the Error constructor.
//===----------------------------------------------------------------------===//

#include "JSLibInternal.h"

#include "hermes/VM/CallResult.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
/// ErrorObject.

HermesValue createErrorConstructor(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<> nameValue;
    PinnedValue<> messageValue;
    PinnedValue<NativeConstructor> cons;
    PinnedValue<NativeFunction> getter;
    PinnedValue<NativeFunction> setter;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto errorPrototype = Handle<JSObject>::vmcast(&runtime.ErrorPrototype);

  // Error.prototype.xxx methods.
  defineMethod(
      runtime,
      errorPrototype,
      Predefined::getSymbolID(Predefined::toString),
      nullptr,
      errorPrototypeToString,
      0);

  // Error.prototype.xxx properties.
  // Error.prototype has three own properties: name, message, and stack.
  auto defaultName = runtime.getPredefinedString(Predefined::Error);
  lv.nameValue = HermesValue::encodeStringValue(defaultName);
  defineProperty(
      runtime,
      errorPrototype,
      Predefined::getSymbolID(Predefined::name),
      lv.nameValue);

  auto defaultMessage = runtime.getPredefinedString(Predefined::emptyString);
  lv.messageValue = HermesValue::encodeStringValue(defaultMessage);
  defineProperty(
      runtime,
      errorPrototype,
      Predefined::getSymbolID(Predefined::message),
      lv.messageValue);

  lv.getter = NativeFunction::create(
      runtime,
      nullptr,
      errorStackGetter,
      Predefined::getSymbolID(Predefined::emptyString),
      0);

  lv.setter = NativeFunction::create(
      runtime,
      nullptr,
      errorStackSetter,
      Predefined::getSymbolID(Predefined::emptyString),
      1);

  // Save the accessors on the runtime so we can use them for captureStackTrace.
  runtime.jsErrorStackAccessor =
      PropertyAccessor::create(runtime, lv.getter, lv.setter);

  auto accessor =
      Handle<PropertyAccessor>::vmcast(&runtime.jsErrorStackAccessor);

  DefinePropertyFlags dpf{};
  dpf.setEnumerable = 1;
  dpf.setConfigurable = 1;
  dpf.setGetter = 1;
  dpf.setSetter = 1;
  dpf.enumerable = 0;
  dpf.configurable = 1;

  auto stackRes = JSObject::defineOwnProperty(
      errorPrototype,
      runtime,
      Predefined::getSymbolID(Predefined::stack),
      dpf,
      accessor);
  (void)stackRes;

  assert(*stackRes && "Failed to define stack accessor");

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Error),
      ErrorConstructor,
      errorPrototype,
      1,
      lv.cons);

  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::captureStackTrace),
      nullptr,
      errorCaptureStackTrace,
      2);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isError),
      nullptr,
      errorIsError,
      1);

  return lv.cons.getHermesValue();
}

// The constructor creation functions have to be expanded from macros because
// the constructor functions are expanded from macros.
#define ERR_HELPER(error_name, argCount)                                    \
  HermesValue create##error_name##Constructor(Runtime &runtime) {           \
    struct : public Locals {                                                \
      PinnedValue<> nameValue;                                              \
      PinnedValue<NativeConstructor> cons;                                  \
    } lv;                                                                   \
    LocalsRAII lraii(runtime, &lv);                                         \
    auto errorPrototype =                                                   \
        Handle<JSObject>::vmcast(&runtime.error_name##Prototype);           \
    auto defaultName = runtime.getPredefinedString(Predefined::error_name); \
    lv.nameValue = HermesValue::encodeStringValue(defaultName);             \
    defineProperty(                                                         \
        runtime,                                                            \
        errorPrototype,                                                     \
        Predefined::getSymbolID(Predefined::name),                          \
        lv.nameValue);                                                      \
    lv.nameValue = HermesValue::encodeStringValue(                          \
        runtime.getPredefinedString(Predefined::emptyString));              \
    defineProperty(                                                         \
        runtime,                                                            \
        errorPrototype,                                                     \
        Predefined::getSymbolID(Predefined::message),                       \
        lv.nameValue);                                                      \
    defineSystemConstructor(                                                \
        runtime,                                                            \
        Predefined::getSymbolID(Predefined::error_name),                    \
        error_name##Constructor,                                            \
        errorPrototype,                                                     \
        Handle<JSObject>::vmcast(&runtime.ErrorConstructor),                \
        argCount,                                                           \
        lv.cons);                                                           \
    return lv.cons.getHermesValue();                                        \
  }
// The AggregateError constructor takes in two parameters, while all the other
// Error types take in one.
#define NATIVE_ERROR_TYPE(name) ERR_HELPER(name, 1)
#define AGGREGATE_ERROR_TYPE(name) ERR_HELPER(name, 2)
#include "hermes/VM/NativeErrorTypes.def"

static CallResult<HermesValue> constructErrorObject(
    Runtime &runtime,
    NativeArgs args,
    Handle<> message,
    Handle<> opts,
    const PinnedValue<NativeConstructor> *errConstructor,
    const PinnedValue<JSObject> *errConstructorProto) {
  // If called constructor or called regularly, we want to always create a new
  // error.
  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<JSError> self;
    PinnedValue<> cause;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // If this is not a construct call, or it is a construct call and new.target
  // is the error constructor, then we know what parent to use to create the new
  // JSArray.
  if (LLVM_LIKELY(
          !args.isConstructorCall() ||
          (args.getNewTarget().getRaw() ==
           errConstructor->getHermesValue().getRaw()))) {
    lv.self = JSError::create(runtime, *errConstructorProto);
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            *errConstructorProto);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
    lv.self = JSError::create(runtime, lv.selfParent);
  }

  // Record the stack trace, skipping this entry.
  JSError::recordStackTrace(lv.self, runtime, true);

  // new Error(message).
  // Only proceed when 'typeof message' isn't undefined.
  if (!message->isUndefined()) {
    if (LLVM_UNLIKELY(
            JSError::setMessage(lv.self, runtime, message) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }

  // https://tc39.es/proposal-error-cause/
  // InstallErrorCause(O, options).
  // If Type(options) is Object and ? HasProperty(options, "cause") is true
  if (auto options = Handle<JSObject>::dyn_vmcast(opts)) {
    GCScopeMarkerRAII marker{runtime};

    auto causeName = Predefined::getSymbolID(Predefined::cause);

    // Use the below to ensure we trigger the `[[HasProperty]]` proxy trap.
    auto hasRes = JSObject::hasNamed(options, runtime, causeName);
    if (LLVM_UNLIKELY(hasRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    if (LLVM_UNLIKELY(*hasRes)) {
      // a. Let cause be ? Get(options, "cause").
      auto causeRes = JSObject::getNamed_RJS(
          options, runtime, causeName, PropOpFlags().plusThrowOnError());
      if (LLVM_UNLIKELY(causeRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.cause = std::move(*causeRes);

      // b. Perform ! CreateNonEnumerableDataPropertyOrThrow(O, "cause", cause).
      if (LLVM_UNLIKELY(
              JSObject::defineOwnProperty(
                  lv.self,
                  runtime,
                  Predefined::getSymbolID(Predefined::cause),
                  DefinePropertyFlags::getNewNonEnumerableFlags(),
                  lv.cause,
                  PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }

  return lv.self.getHermesValue();
}

// ES2023 20.5.7.1.1
static CallResult<HermesValue> constructAggregateErrorObject(
    Runtime &runtime,
    NativeArgs args,
    Handle<JSObject> prototype) {
  struct : public Locals {
    PinnedValue<JSObject> errorObjHandle;
    PinnedValue<JSArray> errorsList;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1-4 handled in constructErrorObject
  CallResult<HermesValue> errorObj = constructErrorObject(
      runtime,
      args,
      args.getArgHandle(1),
      args.getArgHandle(2),
      &runtime.AggregateErrorConstructor,
      &runtime.AggregateErrorPrototype);
  if (LLVM_UNLIKELY(errorObj == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.errorObjHandle.castAndSetHermesValue<JSObject>(*errorObj);

  // 5. Let errorsList be ? IterableToList(errors).
  CallResult<PseudoHandle<JSArray>> errorsList =
      iterableToArray(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(errorsList == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.errorsList = std::move(*errorsList);
  // 6. Perform ! DefinePropertyOrThrow(O, "errors", PropertyDescriptor {
  // [[Configurable]]: true, [[Enumerable]]: false, [[Writable]]: true,
  // [[Value]]: CreateArrayFromList(errorsList) }).

  CallResult<bool> res = JSObject::defineOwnProperty(
      lv.errorObjHandle,
      runtime,
      Predefined::getSymbolID(Predefined::errors),
      DefinePropertyFlags::getNewNonEnumerableFlags(),
      lv.errorsList);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. Return O.
  return lv.errorObjHandle.getHermesValue();
}

// Note- the following names for the error and aggregate functions are spelled
// to exactly match the names in NativeErrorTypes.def. They are used in the
// ERR_HELPER macro defined above.
CallResult<HermesValue> ErrorConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return constructErrorObject(
      runtime,
      args,
      args.getArgHandle(0),
      args.getArgHandle(1),
      &runtime.ErrorConstructor,
      &runtime.ErrorPrototype);
}

// AggregateError has a different constructor body than the other errors.
CallResult<HermesValue> AggregateErrorConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return constructAggregateErrorObject(
      runtime, args, runtime.AggregateErrorPrototype);
}

// Constructor functions have to be expanded from macro because they are
// native calls, and their interface are restricted. No extra parameters
// can be passed in. We can't use the #ALL_ERROR_TYPE macro since AggregateError
// requires a different constructor.
#define NATIVE_ERROR_TYPE(name)                                         \
  CallResult<HermesValue> name##Constructor(void *, Runtime &runtime) { \
    NativeArgs args = runtime.getCurrentFrame().getNativeArgs();        \
    return constructErrorObject(                                        \
        runtime,                                                        \
        args,                                                           \
        args.getArgHandle(0),                                           \
        args.getArgHandle(1),                                           \
        &runtime.name##Constructor,                                     \
        &runtime.name##Prototype);                                      \
  }
#include "hermes/VM/NativeErrorTypes.def"

/// ES2023 20.5.3.4
CallResult<HermesValue> errorPrototypeToString(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  // 1. Let O be the this value.
  auto O = args.dyncastThis<JSObject>();

  // 2. If Type(O) is not Object, throw a TypeError exception.
  if (LLVM_UNLIKELY(!O)) {
    return runtime.raiseTypeErrorForValue(
        "Error.prototype.toString called on incompatible receiver ",
        args.getThisHandle(),
        "");
  }

  // Steps 3. through 9 are implemented in JSError::toString.
  CallResult<PseudoHandle<StringPrimitive>> toStringRes =
      JSError::toString(O, runtime);
  if (LLVM_UNLIKELY(toStringRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return toStringRes->getHermesValue();
}

/// captureStackTrace(target, sentinelOpt?) { ... }
///
/// Adds a `stack` getter to the given `target` object that yields the current
/// stack trace. This is the exact same getter as the one available on Error
/// instances.
///
/// If the second argument to captureStackTrace is a function, when collecting
/// the stack trace all frames above the topmost call to the provided function,
/// including that call, are left out of the stack trace.
CallResult<HermesValue> errorCaptureStackTrace(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSError> errorHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Get the target object.
  auto targetHandle = args.dyncastArg<JSObject>(0);
  if (!targetHandle || targetHandle->isHostObject(runtime) ||
      targetHandle->isProxyObject(runtime)) {
    return runtime.raiseTypeError("Invalid argument");
  }

  // Construct a temporary Error instance.
  auto errorPrototype = Handle<JSObject>::vmcast(&runtime.ErrorPrototype);
  lv.errorHandle = JSError::create(runtime, errorPrototype);

  // Record the stack trace, skipping the entry for captureStackTrace itself.
  const bool skipTopFrame = true;
  JSError::recordStackTrace(lv.errorHandle, runtime, skipTopFrame);

  // Skip frames until and including the sentinel function, if any.
  if (auto sentinel = args.dyncastArg<Callable>(1)) {
    JSError::popFramesUntilInclusive(runtime, lv.errorHandle, sentinel);
  }

  // Initialize the target's [[CapturedError]] slot with the error instance.
  auto res = JSObject::defineOwnProperty(
      targetHandle,
      runtime,
      Predefined::getSymbolID(Predefined::InternalPropertyCapturedError),
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      lv.errorHandle);

  // Even though highly unlikely, something could have happened that caused
  // defineOwnProperty to throw an exception.
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!*res)) {
    return runtime.raiseTypeError(
        TwineChar16("Cannot add new properties to object"));
  }

  // Initialize the `stack` accessor on the target object.
  DefinePropertyFlags dpf{};
  dpf.setEnumerable = 1;
  dpf.setConfigurable = 1;
  dpf.setGetter = 1;
  dpf.setSetter = 1;
  dpf.enumerable = 0;
  dpf.configurable = 1;

  auto stackRes = JSObject::defineOwnProperty(
      targetHandle,
      runtime,
      Predefined::getSymbolID(Predefined::stack),
      dpf,
      runtime.jsErrorStackAccessor);

  // Ignore failures to set the "stack" property as other engines do.
  if (LLVM_UNLIKELY(stackRes == ExecutionStatus::EXCEPTION)) {
    runtime.clearThrownValue();
  }

  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> errorIsError(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // NOTE: Error.isError does NOT appear to check for Proxy in the way that
  // IsArray does, so we just can use vmisa here.

  // 1. If arg is not an Object, return false.
  // 2. If arg does not have an [[ErrorData]] internal slot, return false.
  // 3. Return true.
  return HermesValue::encodeBoolValue(vmisa<JSError>(args.getArg(0)));
}

} // namespace vm
} // namespace hermes
