/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/Support/ErrorHandling.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/StringPrimitive.h"

#include <cstring>

//===========================================================================
// Error creation helpers
//===========================================================================

/// Helper: create an Error/TypeError/RangeError with a message string
/// (given as a napi_value that must be a JS string) and optionally set
/// a "code" property (given as a napi_value that must be a JS string
/// or nullptr). Returns the error object via \p result without
/// throwing it. \p prototype selects the error type.
static napi_status createErrorWithCode(
    napi_env env,
    napi_value code,
    napi_value msg,
    hermes::vm::Handle<hermes::vm::JSObject> prototype,
    napi_value *result) {
  using namespace hermes::vm;

  Runtime &runtime = env->runtime;
  struct : public Locals {
    PinnedValue<JSError> error;
    PinnedValue<SymbolID> codeSym;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  GCScope gcScope(runtime);

  // Get the message string from the napi_value.
  auto *msgPhv = reinterpret_cast<PinnedHermesValue *>(msg);
  auto msgHandle = Handle<StringPrimitive>::vmcast(msgPhv);

  // Create the error object with the appropriate prototype.
  auto errorObj = JSError::create(runtime, prototype);

  // Record the stack trace while we have the relevant frames.
  lv.error = std::move(errorObj);
  auto recordRes = JSError::recordStackTrace(lv.error, runtime);
  if (LLVM_UNLIKELY(recordRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  // Set the message property.
  auto setMsgRes = JSError::setMessage(lv.error, runtime, msgHandle);
  if (LLVM_UNLIKELY(setMsgRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  // Optionally set the "code" property.
  if (code != nullptr) {
    auto *codePhv = reinterpret_cast<PinnedHermesValue *>(code);

    // Look up the "code" symbol.
    auto codeSymRes =
        runtime.getIdentifierTable().getSymbolID(runtime, ASCIIRef{"code", 4});
    if (LLVM_UNLIKELY(codeSymRes == ExecutionStatus::EXCEPTION)) {
      return captureRuntimeException(env, napi_generic_failure);
    }
    lv.codeSym = *codeSymRes;

    // Define "code" as a writable, enumerable, configurable property.
    auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
    auto codeValHandle = Handle<>::vmcast(codePhv);
    auto defineRes = JSObject::defineOwnProperty(
        lv.error, runtime, *lv.codeSym, dpf, codeValHandle);
    if (LLVM_UNLIKELY(defineRes == ExecutionStatus::EXCEPTION)) {
      return captureRuntimeException(env, napi_generic_failure);
    }
  }

  // Return the error in the current handle scope without throwing.
  *result = env->addToCurrentScope(lv.error.getHermesValue());
  return napi_clear_last_error(env);
}

//===========================================================================
// Exception throwing
//===========================================================================

napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, error);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(error);
  env->pendingException = *phv;
  env->hasPendingException = true;
  return napi_clear_last_error(env);
}

/// Helper: create an Error/TypeError/RangeError with a message string,
/// optionally set a "code" property, and store it as the pending
/// exception. \p prototype selects the error type.
static napi_status throwErrorWithMessage(
    napi_env env,
    const char *code,
    const char *msg,
    hermes::vm::Handle<hermes::vm::JSObject> prototype) {
  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  struct : public Locals {
    PinnedValue<StringPrimitive> msgStr;
    PinnedValue<StringPrimitive> codeStr;
    PinnedValue<JSError> error;
    PinnedValue<SymbolID> codeSym;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // GCScope is needed for internal Handle allocations by
  // JSError::create, recordStackTrace, setMessage, getSymbolID,
  // and defineOwnProperty.
  GCScope gcScope(runtime);

  // Create the message string and root it immediately (before
  // JSError::create, which is a GC safepoint).
  auto msgStrRes = StringPrimitive::createEfficient(
      runtime,
      UTF8Ref(reinterpret_cast<const uint8_t *>(msg), std::strlen(msg)));
  if (msgStrRes == ExecutionStatus::EXCEPTION) {
    return captureRuntimeException(env, napi_generic_failure);
  }
  lv.msgStr.castAndSetHermesValue<StringPrimitive>(*msgStrRes);

  // Create the error object with the appropriate prototype.
  auto errorObj = JSError::create(runtime, prototype);

  // Record the stack trace while we have the relevant frames.
  lv.error = std::move(errorObj);
  auto recordRes = JSError::recordStackTrace(lv.error, runtime);
  if (LLVM_UNLIKELY(recordRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  // Set the message property.
  auto setMsgRes = JSError::setMessage(lv.error, runtime, lv.msgStr);
  if (LLVM_UNLIKELY(setMsgRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  // Optionally set the "code" property.
  if (code != nullptr) {
    // Create the code string and root it immediately (before
    // getSymbolID, which is a GC safepoint).
    auto codeStrRes = StringPrimitive::createEfficient(
        runtime,
        UTF8Ref(reinterpret_cast<const uint8_t *>(code), std::strlen(code)));
    if (LLVM_UNLIKELY(codeStrRes == ExecutionStatus::EXCEPTION)) {
      return captureRuntimeException(env, napi_generic_failure);
    }
    lv.codeStr.castAndSetHermesValue<StringPrimitive>(*codeStrRes);

    // Look up the "code" symbol.
    auto codeSymRes =
        runtime.getIdentifierTable().getSymbolID(runtime, ASCIIRef{"code", 4});
    if (LLVM_UNLIKELY(codeSymRes == ExecutionStatus::EXCEPTION)) {
      return captureRuntimeException(env, napi_generic_failure);
    }
    lv.codeSym = *codeSymRes;

    // Define "code" as writable, enumerable, configurable.
    auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
    auto defineRes = JSObject::defineOwnProperty(
        lv.error, runtime, *lv.codeSym, dpf, lv.codeStr);
    if (LLVM_UNLIKELY(defineRes == ExecutionStatus::EXCEPTION)) {
      return captureRuntimeException(env, napi_generic_failure);
    }
  }

  // Store the error as the NAPI pending exception.
  env->pendingException = lv.error.getHermesValue();
  env->hasPendingException = true;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_throw_error(napi_env env, const char *code, const char *msg) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, msg);

  return throwErrorWithMessage(
      env,
      code,
      msg,
      hermes::vm::Handle<hermes::vm::JSObject>::vmcast(
          &env->runtime.ErrorPrototype));
}

napi_status NAPI_CDECL
napi_throw_type_error(napi_env env, const char *code, const char *msg) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, msg);

  return throwErrorWithMessage(
      env,
      code,
      msg,
      hermes::vm::Handle<hermes::vm::JSObject>::vmcast(
          &env->runtime.TypeErrorPrototype));
}

napi_status NAPI_CDECL
napi_throw_range_error(napi_env env, const char *code, const char *msg) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, msg);

  return throwErrorWithMessage(
      env,
      code,
      msg,
      hermes::vm::Handle<hermes::vm::JSObject>::vmcast(
          &env->runtime.RangeErrorPrototype));
}

napi_status NAPI_CDECL
node_api_throw_syntax_error(napi_env env, const char *code, const char *msg) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, msg);

  return throwErrorWithMessage(
      env,
      code,
      msg,
      hermes::vm::Handle<hermes::vm::JSObject>::vmcast(
          &env->runtime.SyntaxErrorPrototype));
}

//===========================================================================
// Exception query/clear
//===========================================================================

napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool *result) {
  // This function is exception-safe — it must work even when there is
  // a pending exception. Use CHECK_ENV, not NAPI_PREAMBLE.
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  *result = env->hasPendingException;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_and_clear_last_exception(napi_env env, napi_value *result) {
  // This function is exception-safe — it must work even when there is
  // a pending exception. Use CHECK_ENV, not NAPI_PREAMBLE.
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  if (!env->hasPendingException) {
    // No pending exception — return undefined.
    return napi_get_undefined(env, result);
  }

  // Return the pending exception as a napi_value in the current
  // scope, then clear the pending state.
  *result = env->addToCurrentScope(env->pendingException);
  env->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();
  env->hasPendingException = false;

  return napi_clear_last_error(env);
}

//===========================================================================
// Error object creation (without throwing)
//===========================================================================

napi_status NAPI_CDECL napi_create_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, msg);
  CHECK_ARG(env, result);

  auto *msgPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(msg);
  RETURN_STATUS_IF_FALSE(env, msgPhv->isString(), napi_string_expected);

  if (code != nullptr) {
    auto *codePhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(code);
    RETURN_STATUS_IF_FALSE(env, codePhv->isString(), napi_string_expected);
  }

  return createErrorWithCode(
      env,
      code,
      msg,
      hermes::vm::Handle<hermes::vm::JSObject>::vmcast(
          &env->runtime.ErrorPrototype),
      result);
}

napi_status NAPI_CDECL napi_create_type_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, msg);
  CHECK_ARG(env, result);

  auto *msgPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(msg);
  RETURN_STATUS_IF_FALSE(env, msgPhv->isString(), napi_string_expected);

  if (code != nullptr) {
    auto *codePhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(code);
    RETURN_STATUS_IF_FALSE(env, codePhv->isString(), napi_string_expected);
  }

  return createErrorWithCode(
      env,
      code,
      msg,
      hermes::vm::Handle<hermes::vm::JSObject>::vmcast(
          &env->runtime.TypeErrorPrototype),
      result);
}

napi_status NAPI_CDECL napi_create_range_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, msg);
  CHECK_ARG(env, result);

  auto *msgPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(msg);
  RETURN_STATUS_IF_FALSE(env, msgPhv->isString(), napi_string_expected);

  if (code != nullptr) {
    auto *codePhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(code);
    RETURN_STATUS_IF_FALSE(env, codePhv->isString(), napi_string_expected);
  }

  return createErrorWithCode(
      env,
      code,
      msg,
      hermes::vm::Handle<hermes::vm::JSObject>::vmcast(
          &env->runtime.RangeErrorPrototype),
      result);
}

napi_status NAPI_CDECL node_api_create_syntax_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, msg);
  CHECK_ARG(env, result);

  auto *msgPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(msg);
  RETURN_STATUS_IF_FALSE(env, msgPhv->isString(), napi_string_expected);

  if (code != nullptr) {
    auto *codePhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(code);
    RETURN_STATUS_IF_FALSE(env, codePhv->isString(), napi_string_expected);
  }

  return createErrorWithCode(
      env,
      code,
      msg,
      hermes::vm::Handle<hermes::vm::JSObject>::vmcast(
          &env->runtime.SyntaxErrorPrototype),
      result);
}

//===========================================================================
// Error type checking
//===========================================================================

napi_status NAPI_CDECL
napi_is_error(napi_env env, napi_value value, bool *result) {
  // Exception-safe — does not call into JavaScript.
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  hermes::vm::HermesValue hv = *phv;

  // In Hermes, JSError is the native error type (corresponds to
  // V8's IsNativeError). Check if the value is an object and a
  // JSError instance.
  *result = hv.isObject() && hermes::vm::vmisa<hermes::vm::JSError>(hv);
  return napi_clear_last_error(env);
}

//===========================================================================
// Fatal error
//===========================================================================

napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, err);

  // If the host provides a fatal_exception handler, delegate to it.
  if (env->host_ && env->host_->fatal_exception) {
    env->host_->fatal_exception(env->host_->data, env, err);
    return napi_clear_last_error(env);
  }

  // In Node.js, this triggers the 'uncaughtException' event. Hermes has
  // no event loop or process object, so we treat this as a fatal error:
  // print the error value and abort.
  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(err);
  HermesValue hv = *phv;

  std::string msg = "Fatal N-API exception: ";
  if (hv.isObject() && vmisa<JSError>(hv)) {
    // Use JSError::toString to get the "name: message" representation.
    GCScope gcScope(env->runtime);
    struct : public Locals {
      PinnedValue<JSObject> obj;
    } lv;
    LocalsRAII lraii(env->runtime, &lv);
    lv.obj.castAndSetHermesValue<JSObject>(hv);
    auto strRes = JSError::toString(lv.obj, env->runtime);
    if (strRes != ExecutionStatus::EXCEPTION) {
      auto *str = (*strRes).get();
      if (str->isASCII()) {
        auto ref = str->getStringRef<char>();
        msg.append(ref.begin(), ref.end());
      } else {
        auto ref = str->getStringRef<char16_t>();
        hermes::convertUTF16ToUTF8WithReplacements(msg, ref);
      }
    } else {
      env->runtime.clearThrownValue();
      msg += "[error extracting message]";
    }
  } else if (hv.isString()) {
    auto *str = vmcast<StringPrimitive>(hv);
    if (str->isASCII()) {
      auto ref = str->getStringRef<char>();
      msg.append(ref.begin(), ref.end());
    } else {
      auto ref = str->getStringRef<char16_t>();
      hermes::convertUTF16ToUTF8WithReplacements(msg, ref);
    }
  } else {
    msg += "[non-string error value]";
  }

  hermes::hermes_fatal(msg);
}

NAPI_NO_RETURN void NAPI_CDECL napi_fatal_error(
    const char *location,
    size_t location_len,
    const char *message,
    size_t message_len) {
  std::string msg;

  if (location != nullptr) {
    std::string loc;
    if (location_len == NAPI_AUTO_LENGTH) {
      loc.assign(location);
    } else {
      loc.assign(location, location_len);
    }
    msg = "FATAL ERROR: " + loc + " ";
  } else {
    msg = "FATAL ERROR: ";
  }

  if (message != nullptr) {
    if (message_len == NAPI_AUTO_LENGTH) {
      msg.append(message);
    } else {
      msg.append(message, message_len);
    }
  }

  hermes::hermes_fatal(msg);
}
