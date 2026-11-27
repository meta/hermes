/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/JSTypedArray.h"

//===========================================================================
// TypedArray operations
//===========================================================================

/// Map a napi_typedarray_type to the element byte width.
static uint8_t typedArrayElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 0;
  }
}

/// Map a CellKind to a napi_typedarray_type. Returns false if the kind
/// is not a TypedArray.
static bool cellKindToTypedArrayType(
    hermes::vm::CellKind kind,
    napi_typedarray_type *type) {
  using hermes::vm::CellKind;
  switch (kind) {
    case CellKind::Int8ArrayKind:
      *type = napi_int8_array;
      return true;
    case CellKind::Uint8ArrayKind:
      *type = napi_uint8_array;
      return true;
    case CellKind::Uint8ClampedArrayKind:
      *type = napi_uint8_clamped_array;
      return true;
    case CellKind::Int16ArrayKind:
      *type = napi_int16_array;
      return true;
    case CellKind::Uint16ArrayKind:
      *type = napi_uint16_array;
      return true;
    case CellKind::Int32ArrayKind:
      *type = napi_int32_array;
      return true;
    case CellKind::Uint32ArrayKind:
      *type = napi_uint32_array;
      return true;
    case CellKind::Float32ArrayKind:
      *type = napi_float32_array;
      return true;
    case CellKind::Float64ArrayKind:
      *type = napi_float64_array;
      return true;
    case CellKind::BigInt64ArrayKind:
      *type = napi_bigint64_array;
      return true;
    case CellKind::BigUint64ArrayKind:
      *type = napi_biguint64_array;
      return true;
    default:
      return false;
  }
}

/// Create a TypedArray of the given \p type, then call setBuffer to attach
/// it to \p buf at \p byteOffset for \p length elements. The TypedArray
/// object is stored in \p taOut. \p buf must
/// be pinned, since the TypedArray allocation below is a GC point.
/// Returns ExecutionStatus.
static hermes::vm::ExecutionStatus createTypedArrayForType(
    napi_typedarray_type type,
    hermes::vm::Runtime &runtime,
    hermes::vm::PinnedValue<hermes::vm::JSArrayBuffer> &buf,
    size_t byteOffset,
    size_t length,
    hermes::vm::PinnedValue<hermes::vm::JSTypedArrayBase> &taOut) {
  using namespace hermes::vm;

  // Create the TypedArray object with the correct prototype.
  // Each case creates a PseudoHandle, assigns it to taOut, then calls
  // setBuffer. We use a macro to avoid repeating the pattern 11 times.
#define CREATE_TA_CASE(napiType, HermesType)      \
  case napiType: {                                \
    auto ph = HermesType::create(                 \
        runtime,                                  \
        HermesType::getPrototype(runtime),        \
        HermesType::getRootHiddenClass(runtime)); \
    taOut = std::move(ph);                        \
    break;                                        \
  }

  switch (type) {
    CREATE_TA_CASE(napi_int8_array, Int8Array)
    CREATE_TA_CASE(napi_uint8_array, Uint8Array)
    CREATE_TA_CASE(napi_uint8_clamped_array, Uint8ClampedArray)
    CREATE_TA_CASE(napi_int16_array, Int16Array)
    CREATE_TA_CASE(napi_uint16_array, Uint16Array)
    CREATE_TA_CASE(napi_int32_array, Int32Array)
    CREATE_TA_CASE(napi_uint32_array, Uint32Array)
    CREATE_TA_CASE(napi_float32_array, Float32Array)
    CREATE_TA_CASE(napi_float64_array, Float64Array)
    CREATE_TA_CASE(napi_bigint64_array, BigInt64Array)
    CREATE_TA_CASE(napi_biguint64_array, BigUint64Array)
    default:
      return ExecutionStatus::EXCEPTION;
  }
#undef CREATE_TA_CASE

  // Attach the buffer. setBuffer takes byte offset and byte size.
  uint8_t elemSize = typedArrayElementSize(type);
  JSTypedArrayBase::setBuffer(
      runtime,
      vmcast<JSTypedArrayBase>(taOut.getHermesValue()),
      buf.get(),
      static_cast<JSTypedArrayBase::size_type>(byteOffset),
      static_cast<JSTypedArrayBase::size_type>(length * elemSize),
      elemSize);

  return ExecutionStatus::RETURNED;
}

napi_status NAPI_CDECL napi_create_typedarray(
    napi_env env,
    napi_typedarray_type type,
    size_t length,
    napi_value arraybuffer,
    size_t byte_offset,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, arraybuffer);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  // Validate that arraybuffer is actually a JSArrayBuffer.
  auto *abPhv = reinterpret_cast<PinnedHermesValue *>(arraybuffer);
  RETURN_STATUS_IF_FALSE(
      env, abPhv->isObject() && vmisa<JSArrayBuffer>(*abPhv), napi_invalid_arg);

  // Validate the type.
  uint8_t elemSize = typedArrayElementSize(type);
  RETURN_STATUS_IF_FALSE(env, elemSize > 0, napi_invalid_arg);

  // Validate alignment: byte_offset must be a multiple of element size.
  if (elemSize > 1) {
    RETURN_STATUS_IF_FALSE(env, byte_offset % elemSize == 0, napi_invalid_arg);
  }

  // Validate bounds without overflow: byte_offset and length * elemSize are
  // caller-supplied size_t values that get narrowed to the view's uint32
  // size_type below, so reject anything that does not fit in the buffer using
  // division and subtraction rather than additions/multiplications that could
  // wrap. Matches the getTypedArrayBuffer helper in extensions/JSIUtils.cpp.
  size_t bufferSize = vmcast<JSArrayBuffer>(*abPhv)->size();
  RETURN_STATUS_IF_FALSE(
      env,
      length <= bufferSize / elemSize && byte_offset <= bufferSize &&
          length * elemSize <= bufferSize - byte_offset,
      napi_invalid_arg);

  struct : public Locals {
    PinnedValue<JSArrayBuffer> ab;
    PinnedValue<JSTypedArrayBase> ta;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.ab.castAndSetHermesValue<JSArrayBuffer>(*abPhv);

  auto status =
      createTypedArrayForType(type, runtime, lv.ab, byte_offset, length, lv.ta);
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = env->addToCurrentScope(lv.ta.getHermesValue());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_typedarray_info(
    napi_env env,
    napi_value typedarray,
    napi_typedarray_type *type,
    size_t *length,
    void **data,
    napi_value *arraybuffer,
    size_t *byte_offset) {
  CHECK_ENV(env);
  CHECK_ARG(env, typedarray);

  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(typedarray);
  RETURN_STATUS_IF_FALSE(
      env, phv->isObject() && vmisa<JSTypedArrayBase>(*phv), napi_invalid_arg);

  auto *ta = vmcast<JSTypedArrayBase>(*phv);

  if (type != nullptr) {
    napi_typedarray_type taType;
    if (!cellKindToTypedArrayType(
            static_cast<GCCell *>(phv->getObject())->getKind(), &taType)) {
      return napi_set_last_error(env, napi_invalid_arg);
    }
    *type = taType;
  }

  if (length != nullptr) {
    *length = ta->getLength();
  }

  Runtime &runtime = env->runtime;

  if (data != nullptr) {
    if (ta->attached(runtime)) {
      *data = ta->data(runtime);
    } else {
      *data = nullptr;
    }
  }

  if (arraybuffer != nullptr) {
    if (ta->attached(runtime)) {
      *arraybuffer = env->addToCurrentScope(
          HermesValue::encodeObjectValue(ta->getBuffer(runtime)));
    } else {
      *arraybuffer =
          env->addToCurrentScope(HermesValue::encodeUndefinedValue());
    }
  }

  if (byte_offset != nullptr) {
    *byte_offset = ta->getByteOffset();
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_is_typedarray(napi_env env, napi_value value, bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  *result =
      phv->isObject() && hermes::vm::vmisa<hermes::vm::JSTypedArrayBase>(*phv);
  return napi_clear_last_error(env);
}
