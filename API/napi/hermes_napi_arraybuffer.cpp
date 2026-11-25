/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/JSArrayBuffer.h"

//===========================================================================
// ArrayBuffer operations
//===========================================================================

napi_status NAPI_CDECL napi_create_arraybuffer(
    napi_env env,
    size_t byte_length,
    void **data,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  // Create the ArrayBuffer object.
  GCScope gcScope(runtime);

  auto proto = Handle<JSObject>::vmcast(&runtime.arrayBufferPrototype);
  auto abPH = JSArrayBuffer::create(runtime, proto);

  // Root the ArrayBuffer before allocating the data block (which is
  // a GC safepoint since it adjusts external memory accounting).
  struct : public Locals {
    PinnedValue<JSArrayBuffer> ab;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.ab = std::move(abPH);

  // Allocate the data block.
  auto status = JSArrayBuffer::createDataBlock(runtime, lv.ab, byte_length);
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  // Optionally return the data pointer.
  if (data != nullptr) {
    *data = lv.ab->getDataBlock();
  }

  *result = env->addToCurrentScope(lv.ab.getHermesValue());
  return napi_clear_last_error(env);
}

#ifndef NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED
napi_status NAPI_CDECL napi_create_external_arraybuffer(
    napi_env env,
    void *external_data,
    size_t byte_length,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  Runtime &runtime = env->runtime;

  // Create the ArrayBuffer object.
  GCScope gcScope(runtime);

  auto proto = Handle<JSObject>::vmcast(&runtime.arrayBufferPrototype);
  auto abPH = JSArrayBuffer::create(runtime, proto);

  // Root the ArrayBuffer before calling setExternalDataBlock (which
  // is a GC safepoint because it allocates a NativeState internally).
  struct : public Locals {
    PinnedValue<JSArrayBuffer> ab;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.ab = std::move(abPH);

  // Build a shared_ptr whose custom deleter invokes the NAPI
  // finalizer. The shared_ptr itself is stored inside the
  // JSArrayBuffer's NativeState by setExternalDataBlock.
  std::shared_ptr<void> ctx;
  if (finalize_cb) {
    // The env is owned by the Runtime and outlives every GC cycle,
    // so capturing it by raw pointer is safe.
    ctx = std::shared_ptr<void>(
        external_data, [env, finalize_cb, finalize_hint](void *data) {
          // Queue for deferred execution outside GC.
          env->queuePendingFinalizer(finalize_cb, data, finalize_hint);
        });
  }

  JSArrayBuffer::setExternalDataBlock(
      runtime, lv.ab, static_cast<uint8_t *>(external_data), byte_length, ctx);

  *result = env->addToCurrentScope(lv.ab.getHermesValue());
  return napi_clear_last_error(env);
}
#endif // NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED

napi_status NAPI_CDECL napi_get_arraybuffer_info(
    napi_env env,
    napi_value arraybuffer,
    void **data,
    size_t *byte_length) {
  CHECK_ENV(env);
  CHECK_ARG(env, arraybuffer);

  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(arraybuffer);
  RETURN_STATUS_IF_FALSE(
      env, phv->isObject() && vmisa<JSArrayBuffer>(*phv), napi_invalid_arg);

  auto *ab = vmcast<JSArrayBuffer>(*phv);

  if (data != nullptr) {
    *data = ab->attached() ? ab->getDataBlock() : nullptr;
  }
  if (byte_length != nullptr) {
    *byte_length = ab->size();
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_is_arraybuffer(napi_env env, napi_value value, bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  *result =
      phv->isObject() && hermes::vm::vmisa<hermes::vm::JSArrayBuffer>(*phv);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_detach_arraybuffer(napi_env env, napi_value arraybuffer) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, arraybuffer);

  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(arraybuffer);
  RETURN_STATUS_IF_FALSE(
      env,
      phv->isObject() && vmisa<JSArrayBuffer>(*phv),
      napi_arraybuffer_expected);

  auto *ab = vmcast<JSArrayBuffer>(*phv);
  // An already-detached buffer is not detachable.
  RETURN_STATUS_IF_FALSE(
      env, ab->attached(), napi_detachable_arraybuffer_expected);

  Runtime &runtime = env->runtime;

  // detach() needs a Handle, which requires a GCScope.
  // setExternalFinalizer (called inside detach for external buffers)
  // may allocate handles internally.
  GCScope gcScope(runtime);
  struct : public Locals {
    PinnedValue<JSArrayBuffer> ab;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.ab = ab;
  JSArrayBuffer::detach(runtime, lv.ab);

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_is_detached_arraybuffer(napi_env env, napi_value value, bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);

  // If the value is an ArrayBuffer, report whether it is detached.
  // If it's not an ArrayBuffer at all, return false (matches V8 behavior).
  *result = phv->isObject() &&
      hermes::vm::vmisa<hermes::vm::JSArrayBuffer>(*phv) &&
      !hermes::vm::vmcast<hermes::vm::JSArrayBuffer>(*phv)->attached();

  return napi_clear_last_error(env);
}
