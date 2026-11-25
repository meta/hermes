/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSDataView.h"

//===========================================================================
// DataView operations
//===========================================================================

napi_status NAPI_CDECL napi_create_dataview(
    napi_env env,
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

  Handle<JSArrayBuffer> ab = Handle<JSArrayBuffer>::vmcast(abPhv);

  // Validate bounds: byte_offset + length <= buffer size.
  RETURN_STATUS_IF_FALSE(
      env, byte_offset + length <= ab->size(), napi_invalid_arg);

  GCScope gcScope(runtime);

  struct : public Locals {
    PinnedValue<JSDataView> dv;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto proto = Handle<JSObject>::vmcast(&runtime.dataViewPrototype);
  lv.dv = JSDataView::create(runtime, proto);

  // Attach the buffer. setBuffer is a member function that takes
  // raw pointers — safe because the DataView is already rooted.
  lv.dv->setBuffer(
      runtime,
      *ab,
      static_cast<JSDataView::size_type>(byte_offset),
      static_cast<JSDataView::size_type>(length));

  *result = env->addToCurrentScope(lv.dv.getHermesValue());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_dataview_info(
    napi_env env,
    napi_value dataview,
    size_t *bytelength,
    void **data,
    napi_value *arraybuffer,
    size_t *byte_offset) {
  CHECK_ENV(env);
  CHECK_ARG(env, dataview);

  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(dataview);
  RETURN_STATUS_IF_FALSE(
      env, phv->isObject() && vmisa<JSDataView>(*phv), napi_invalid_arg);

  auto *dv = vmcast<JSDataView>(*phv);
  Runtime &runtime = env->runtime;

  if (bytelength != nullptr) {
    *bytelength = dv->byteLength();
  }

  if (byte_offset != nullptr) {
    *byte_offset = dv->byteOffset();
  }

  if (data != nullptr || arraybuffer != nullptr) {
    GCScope gcScope(runtime);

    if (dv->attached(runtime)) {
      JSArrayBuffer *buf = dv->getBuffer(runtime);
      if (data != nullptr) {
        *data = buf->getDataBlock() + dv->byteOffset();
      }
      if (arraybuffer != nullptr) {
        *arraybuffer =
            env->addToCurrentScope(HermesValue::encodeObjectValue(buf));
      }
    } else {
      if (data != nullptr) {
        *data = nullptr;
      }
      if (arraybuffer != nullptr) {
        *arraybuffer =
            env->addToCurrentScope(HermesValue::encodeUndefinedValue());
      }
    }
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_is_dataview(napi_env env, napi_value value, bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  *result = phv->isObject() && hermes::vm::vmisa<hermes::vm::JSDataView>(*phv);
  return napi_clear_last_error(env);
}
