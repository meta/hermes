/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/Support/Conversions.h"
#include "hermes/Support/sh_tryfast_fp_cvt.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/DecoratedObject.h"
#include "hermes/VM/IdentifierTable.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/SymbolRegistry.h"

#include <cmath>
#include <cstring>
#include <limits>

//===========================================================================
// Singleton value getters
//===========================================================================

napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result =
      env->addToCurrentScope(hermes::vm::HermesValue::encodeUndefinedValue());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result = env->addToCurrentScope(hermes::vm::HermesValue::encodeNullValue());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_boolean(napi_env env, bool value, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result =
      env->addToCurrentScope(hermes::vm::HermesValue::encodeBoolValue(value));
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result = env->addToCurrentScope(*hermes::vm::toPHV(&env->runtime.global_));
  return napi_clear_last_error(env);
}

//===========================================================================
// Number creation
//===========================================================================

napi_status NAPI_CDECL
napi_create_double(napi_env env, double value, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  // Use encodeUntrustedNumberValue because the value comes from user
  // code and may be a signaling NaN that needs to be canonicalized.
  *result = env->addToCurrentScope(
      hermes::vm::HermesValue::encodeUntrustedNumberValue(value));
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_create_int32(napi_env env, int32_t value, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result = env->addToCurrentScope(
      hermes::vm::HermesValue::encodeTrustedNumberValue(value));
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_create_uint32(napi_env env, uint32_t value, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result = env->addToCurrentScope(
      hermes::vm::HermesValue::encodeTrustedNumberValue(value));
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_create_int64(napi_env env, int64_t value, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  // int64 is cast to double, which may lose precision for large
  // values. This matches the V8 reference implementation behavior.
  *result = env->addToCurrentScope(
      hermes::vm::HermesValue::encodeTrustedNumberValue(
          static_cast<double>(value)));
  return napi_clear_last_error(env);
}

//===========================================================================
// Number/boolean extraction
//===========================================================================

napi_status NAPI_CDECL
napi_get_value_double(napi_env env, napi_value value, double *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isNumber(), napi_number_expected);

  *result = phv->getNumber();
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_value_int32(napi_env env, napi_value value, int32_t *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isNumber(), napi_number_expected);

  // Apply ES5.1 section 9.5 ToInt32 conversion: NaN and Infinity
  // become 0, other values are truncated to the low 32 bits.
  *result = hermes::truncateToInt32(phv->getNumber());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_value_uint32(napi_env env, napi_value value, uint32_t *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isNumber(), napi_number_expected);

  // Apply ES5.1 section 9.6 ToUint32 conversion: same bit pattern as
  // ToInt32 but interpreted as unsigned.
  *result = hermes::truncateToUInt32(phv->getNumber());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_value_int64(napi_env env, napi_value value, int64_t *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isNumber(), napi_number_expected);

  double d = phv->getNumber();
  // Fast path: use hardware truncation for the common case of small integers.
  int64_t i;
  if (sh_tryfast_f64_to_i64(d, i)) {
    *result = i;
  } else if (std::isfinite(d)) {
    // Out of range or tryfast false negative — clamp to INT64_MIN/MAX,
    // matching V8's NumberToInt64() in deps/v8/src/numbers/conversions-inl.h.
    if (d >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
      *result = std::numeric_limits<int64_t>::max();
    } else if (d <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
      *result = std::numeric_limits<int64_t>::min();
    } else {
      *result = static_cast<int64_t>(d);
    }
  } else {
    // NaN, +Inf, -Inf all convert to 0.
    *result = 0;
  }
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_value_bool(napi_env env, napi_value value, bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isBool(), napi_boolean_expected);

  *result = phv->getBool();
  return napi_clear_last_error(env);
}

//===========================================================================
// Type checking
//===========================================================================

napi_status NAPI_CDECL
napi_typeof(napi_env env, napi_value value, napi_valuetype *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  hermes::vm::HermesValue hv = *phv;

  if (hv.isNumber()) {
    *result = napi_number;
  } else if (hv.isBigInt()) {
    *result = napi_bigint;
  } else if (hv.isString()) {
    *result = napi_string;
  } else if (hv.isObject()) {
    // Function check must come before plain object, because callable
    // objects are also objects.
    if (hermes::vm::vmisa<hermes::vm::Callable>(hv)) {
      *result = napi_function;
    } else if (
        static_cast<hermes::vm::GCCell *>(hv.getObject())->getKind() ==
        hermes::vm::CellKind::DecoratedObjectKind) {
      // A plain DecoratedObject (not HostObject subclass) is a NAPI
      // external value created by napi_create_external.
      *result = napi_external;
    } else {
      *result = napi_object;
    }
  } else if (hv.isBool()) {
    *result = napi_boolean;
  } else if (hv.isUndefined()) {
    *result = napi_undefined;
  } else if (hv.isSymbol()) {
    *result = napi_symbol;
  } else if (hv.isNull()) {
    *result = napi_null;
  } else {
    return napi_set_last_error(env, napi_invalid_arg);
  }

  return napi_clear_last_error(env);
}

//===========================================================================
// Object creation
//===========================================================================

napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  // Create a plain empty object with the standard Object.prototype.
  auto obj = hermes::vm::JSObject::create(env->runtime);
  *result = env->addToCurrentScope(obj.getHermesValue());
  return napi_clear_last_error(env);
}

//===========================================================================
// Array creation and operations
//===========================================================================

napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  auto arrRes = hermes::vm::JSArray::create(env->runtime, 0, 0);
  if (LLVM_UNLIKELY(arrRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(arrRes->getHermesValue());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_create_array_with_length(napi_env env, size_t length, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  auto arrRes = hermes::vm::JSArray::create(env->runtime, length, length);
  if (LLVM_UNLIKELY(arrRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(arrRes->getHermesValue());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_is_array(napi_env env, napi_value value, bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  *result = phv->isObject() && hermes::vm::vmisa<hermes::vm::JSArray>(*phv);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_array_length(napi_env env, napi_value value, uint32_t *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(
      env,
      phv->isObject() && hermes::vm::vmisa<hermes::vm::JSArray>(*phv),
      napi_array_expected);

  auto *arr = hermes::vm::vmcast<hermes::vm::JSArray>(*phv);
  *result = hermes::vm::JSArray::getLength(arr, env->runtime);
  return napi_clear_last_error(env);
}

//===========================================================================
// Type coercion
//===========================================================================

napi_status NAPI_CDECL
napi_coerce_to_bool(napi_env env, napi_value value, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  bool boolResult = hermes::vm::toBoolean(*phv);
  *result = env->addToCurrentScope(
      hermes::vm::HermesValue::encodeBoolValue(boolResult));
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_coerce_to_number(napi_env env, napi_value value, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto valHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(value));

  auto numRes = hermes::vm::toNumber_RJS(env->runtime, valHandle);
  if (LLVM_UNLIKELY(numRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = env->addToCurrentScope(*numRes);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_coerce_to_object(napi_env env, napi_value value, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto valHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(value));

  auto objRes = hermes::vm::toObject(env->runtime, valHandle);
  if (LLVM_UNLIKELY(objRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = env->addToCurrentScope(*objRes);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_coerce_to_string(napi_env env, napi_value value, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto valHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(value));

  auto strRes = hermes::vm::toString_RJS(env->runtime, valHandle);
  if (LLVM_UNLIKELY(strRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = env->addToCurrentScope(strRes->getHermesValue());
  return napi_clear_last_error(env);
}

//===========================================================================
// Comparison
//===========================================================================

napi_status NAPI_CDECL
napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs, bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, lhs);
  CHECK_ARG(env, rhs);
  CHECK_ARG(env, result);

  auto *lhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(lhs);
  auto *rhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(rhs);

  *result = hermes::vm::strictEqualityTest(*lhv, *rhv);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_instanceof(
    napi_env env,
    napi_value object,
    napi_value constructor,
    bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, object);
  CHECK_ARG(env, constructor);
  CHECK_ARG(env, result);

  *result = false;

  auto *ctorPhv =
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(constructor);

  // Match V8: when constructor is not callable, throw a TypeError and
  // return napi_function_expected.
  if (!(ctorPhv->isObject() &&
        hermes::vm::vmisa<hermes::vm::Callable>(*ctorPhv))) {
    napi_throw_type_error(
        env, "ERR_NAPI_CONS_FUNCTION", "Constructor must be a function");
    return napi_set_last_error(env, napi_function_expected);
  }

  auto objHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(object));
  auto ctorHandle = hermes::vm::Handle<>(ctorPhv);

  auto instRes =
      hermes::vm::instanceOfOperator_RJS(env->runtime, objHandle, ctorHandle);
  if (LLVM_UNLIKELY(instRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = *instRes;
  return napi_clear_last_error(env);
}

//===========================================================================
// Symbol operations
//===========================================================================

napi_status NAPI_CDECL
napi_create_symbol(napi_env env, napi_value description, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  // Use Locals for rooting the description string across GC points.
  struct : public Locals {
    PinnedValue<StringPrimitive> descStr;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (description == nullptr) {
    // No description provided — use empty string, matching
    // the Symbol() constructor behavior.
    lv.descStr = runtime.getPredefinedString(Predefined::emptyString);
  } else {
    auto *phv = reinterpret_cast<PinnedHermesValue *>(description);
    RETURN_STATUS_IF_FALSE(env, phv->isString(), napi_string_expected);
    lv.descStr.castAndSetHermesValue<StringPrimitive>(*phv);
  }

  auto symbolRes =
      runtime.getIdentifierTable().createNotUniquedSymbol(runtime, lv.descStr);
  if (LLVM_UNLIKELY(symbolRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(HermesValue::encodeSymbolValue(*symbolRes));
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_symbol_for(
    napi_env env,
    const char *utf8description,
    size_t length,
    napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, utf8description);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  if (length == NAPI_AUTO_LENGTH) {
    length = std::strlen(utf8description);
  }

  // Use Locals for rooting the key string across GC points.
  struct : public Locals {
    PinnedValue<StringPrimitive> key;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto strRes = StringPrimitive::createEfficient(
      runtime,
      UTF8Ref(reinterpret_cast<const uint8_t *>(utf8description), length));
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }
  lv.key.castAndSetHermesValue<StringPrimitive>(*strRes);

  auto symbolRes = runtime.getSymbolRegistry().getSymbolForKey(runtime, lv.key);
  if (LLVM_UNLIKELY(symbolRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(HermesValue::encodeSymbolValue(*symbolRes));
  return napi_clear_last_error(env);
}

//===========================================================================
// Date operations
//===========================================================================

napi_status NAPI_CDECL
napi_create_date(napi_env env, double time, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  auto dateObj = JSDate::create(
      runtime, time, Handle<JSObject>::vmcast(&runtime.datePrototype));

  *result = env->addToCurrentScope(dateObj.getHermesValue());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_is_date(napi_env env, napi_value value, bool *is_date) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, is_date);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  *is_date = phv->isObject() && hermes::vm::vmisa<hermes::vm::JSDate>(*phv);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_date_value(napi_env env, napi_value value, double *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(
      env,
      phv->isObject() && hermes::vm::vmisa<hermes::vm::JSDate>(*phv),
      napi_date_expected);

  auto *date = hermes::vm::vmcast<hermes::vm::JSDate>(*phv);
  *result = date->getPrimitiveValue();
  return napi_clear_last_error(env);
}
