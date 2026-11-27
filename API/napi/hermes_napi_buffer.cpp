/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSTypedArray.h"

#include <cstring>

//===========================================================================
// Buffer operations (Buffer as Uint8Array)
//===========================================================================

/// Helper: create a Uint8Array backed by a new ArrayBuffer of \p length
/// bytes. On success, the Uint8Array is stored in \p result and the data
/// pointer is optionally stored in \p data.
static napi_status createBufferUint8Array(
    napi_env env,
    size_t length,
    void **data,
    napi_value *result) {
  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  struct : public Locals {
    PinnedValue<JSArrayBuffer> ab;
    PinnedValue<JSTypedArrayBase> ta;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Create the ArrayBuffer.
  lv.ab = JSArrayBuffer::create(runtime);

  // Allocate the data block.
  auto status = JSArrayBuffer::createDataBlock(runtime, lv.ab, length);
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  // Create a Uint8Array view over the entire ArrayBuffer.
  lv.ta = Uint8Array::create(
      runtime,
      Uint8Array::getPrototype(runtime),
      Uint8Array::getRootHiddenClass(runtime));
  JSTypedArrayBase::setBuffer(
      runtime,
      vmcast<JSTypedArrayBase>(lv.ta.getHermesValue()),
      vmcast<JSArrayBuffer>(lv.ab.getHermesValue()),
      0,
      static_cast<JSTypedArrayBase::size_type>(length),
      1);

  if (data != nullptr) {
    *data = vmcast<JSArrayBuffer>(lv.ab.getHermesValue())->getDataBlock();
  }

  *result = env->addToCurrentScope(lv.ta.getHermesValue());
  return napi_ok;
}

napi_status NAPI_CDECL napi_create_buffer(
    napi_env env,
    size_t length,
    void **data,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  auto status = createBufferUint8Array(env, length, data, result);
  if (status != napi_ok) {
    return status;
  }
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_buffer_copy(
    napi_env env,
    size_t length,
    const void *data,
    void **result_data,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  CHECK_ARG(env, data);

  using namespace hermes::vm;

  void *bufData = nullptr;
  auto status = createBufferUint8Array(env, length, &bufData, result);
  if (status != napi_ok) {
    return status;
  }

  // Copy the data into the buffer.
  std::memcpy(bufData, data, length);

  if (result_data != nullptr) {
    *result_data = bufData;
  }
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_is_buffer(napi_env env, napi_value value, bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  // Buffer is implemented as Uint8Array.
  *result = phv->isObject() && hermes::vm::vmisa<hermes::vm::Uint8Array>(*phv);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_buffer_info(
    napi_env env,
    napi_value value,
    void **data,
    size_t *length) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);

  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(
      env, phv->isObject() && vmisa<Uint8Array>(*phv), napi_invalid_arg);

  auto *ta = vmcast<JSTypedArrayBase>(*phv);
  Runtime &runtime = env->runtime;

  if (data != nullptr) {
    if (ta->attached(runtime)) {
      *data = ta->data(runtime);
    } else {
      *data = nullptr;
    }
  }
  if (length != nullptr) {
    *length = ta->getLength();
  }

  return napi_clear_last_error(env);
}

#ifndef NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED
napi_status NAPI_CDECL napi_create_external_buffer(
    napi_env env,
    size_t length,
    void *data,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  struct : public Locals {
    PinnedValue<JSArrayBuffer> ab;
    PinnedValue<JSTypedArrayBase> ta;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Create an ArrayBuffer.
  lv.ab = JSArrayBuffer::create(runtime);

  // Build a shared_ptr whose custom deleter invokes the NAPI
  // finalizer.
  std::shared_ptr<void> ctx;
  if (finalize_cb) {
    // The env is owned by the Runtime and outlives every GC cycle,
    // so capturing it by raw pointer is safe.
    ctx =
        std::shared_ptr<void>(data, [env, finalize_cb, finalize_hint](void *d) {
          env->queuePendingFinalizer(finalize_cb, d, finalize_hint);
        });
  }

  JSArrayBuffer::setExternalDataBlock(
      runtime, lv.ab, static_cast<uint8_t *>(data), length, ctx);

  // Create a Uint8Array view over the entire ArrayBuffer.
  lv.ta = Uint8Array::create(
      runtime,
      Uint8Array::getPrototype(runtime),
      Uint8Array::getRootHiddenClass(runtime));
  JSTypedArrayBase::setBuffer(
      runtime,
      vmcast<JSTypedArrayBase>(lv.ta.getHermesValue()),
      vmcast<JSArrayBuffer>(lv.ab.getHermesValue()),
      0,
      static_cast<JSTypedArrayBase::size_type>(length),
      1);

  *result = env->addToCurrentScope(lv.ta.getHermesValue());
  return napi_clear_last_error(env);
}
#endif // NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED

napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(
    napi_env env,
    napi_value arraybuffer,
    size_t byte_offset,
    size_t byte_length,
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

  // Validate bounds without overflow: byte_offset and byte_length are
  // caller-supplied size_t values that get narrowed to the view's uint32
  // size_type below, so compare with subtraction rather than an addition that
  // could wrap past the end for a large byte_offset.
  size_t bufferSize = vmcast<JSArrayBuffer>(*abPhv)->size();
  RETURN_STATUS_IF_FALSE(
      env,
      byte_offset <= bufferSize && byte_length <= bufferSize - byte_offset,
      napi_invalid_arg);

  struct : public Locals {
    PinnedValue<JSTypedArrayBase> ta;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Create a Uint8Array view at the specified offset and length.
  // Re-deref *abPhv after the allocation: it is a NAPI pinned handle, so it
  // survives GC and yields the post-relocation pointer.
  lv.ta = Uint8Array::create(
      runtime,
      Uint8Array::getPrototype(runtime),
      Uint8Array::getRootHiddenClass(runtime));
  JSTypedArrayBase::setBuffer(
      runtime,
      vmcast<JSTypedArrayBase>(lv.ta.getHermesValue()),
      vmcast<JSArrayBuffer>(*abPhv),
      static_cast<JSTypedArrayBase::size_type>(byte_offset),
      static_cast<JSTypedArrayBase::size_type>(byte_length),
      1);

  *result = env->addToCurrentScope(lv.ta.getHermesValue());
  return napi_clear_last_error(env);
}
