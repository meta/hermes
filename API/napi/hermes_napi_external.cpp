/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/DecoratedObject.h"
#include "hermes/VM/HandleRootOwner.h"

using namespace hermes::vm;

//===========================================================================
// NapiExternalDecoration
//===========================================================================

/// Decoration for NAPI external values. Stores the user's native data
/// pointer and an optional destructor callback. The destructor is
/// called during GC finalization when the external object is collected.
struct NapiExternalDecoration : public DecoratedObject::Decoration {
  napi_env env;
  void *data;
  napi_finalize finalize_cb;
  void *finalize_hint;

  /// Type tag fields (128-bit tag set via napi_type_tag_object).
  bool hasTypeTag = false;
  uint64_t typeTagLower = 0;
  uint64_t typeTagUpper = 0;

  NapiExternalDecoration(
      napi_env env,
      void *data,
      napi_finalize finalize_cb,
      void *finalize_hint)
      : env(env),
        data(data),
        finalize_cb(finalize_cb),
        finalize_hint(finalize_hint) {}

  ~NapiExternalDecoration() override {
    if (finalize_cb) {
      // Queue for deferred execution outside GC — calling back into
      // JS during GC sweep causes reentrancy. The env is owned by
      // the Runtime and outlives every GC cycle, so dereferencing it
      // here is always safe.
      env->queuePendingFinalizer(finalize_cb, data, finalize_hint);
    }
  }
};

//===========================================================================
// napi_create_external
//===========================================================================

napi_status NAPI_CDECL napi_create_external(
    napi_env env,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  Runtime &runtime = env->runtime;

  // Create a DecoratedObject with an NapiExternalDecoration.

  auto decoration = std::make_unique<NapiExternalDecoration>(
      env, data, reinterpret_cast<napi_finalize>(finalize_cb), finalize_hint);

  auto obj = DecoratedObject::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime.objectPrototype),
      Handle<HiddenClass>::vmcast(&runtime.classDecoratedObject),
      std::move(decoration));

  *result = env->addToCurrentScope(obj.getHermesValue());
  return napi_clear_last_error(env);
}

//===========================================================================
// napi_get_value_external
//===========================================================================

napi_status NAPI_CDECL
napi_get_value_external(napi_env env, napi_value value, void **result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(value);
  HermesValue hv = *phv;

  // Must be a DecoratedObject (but not a HostObject subclass).
  RETURN_STATUS_IF_FALSE(
      env,
      hv.isObject() &&
          static_cast<GCCell *>(hv.getObject())->getKind() ==
              CellKind::DecoratedObjectKind,
      napi_invalid_arg);

  auto *decoratedObj = vmcast<DecoratedObject>(hv);
  auto *decoration =
      static_cast<NapiExternalDecoration *>(decoratedObj->getDecoration());
  *result = decoration ? decoration->data : nullptr;

  return napi_clear_last_error(env);
}

//===========================================================================
// Type tag helpers for externals
//===========================================================================

bool isNapiExternal(HermesValue hv) {
  return hv.isObject() &&
      static_cast<GCCell *>(hv.getObject())->getKind() ==
      CellKind::DecoratedObjectKind;
}

napi_status napiExternalSetTypeTag(
    napi_env env,
    HermesValue hv,
    const napi_type_tag *type_tag) {
  auto *decoratedObj = vmcast<DecoratedObject>(hv);
  auto *decoration =
      static_cast<NapiExternalDecoration *>(decoratedObj->getDecoration());
  if (!decoration) {
    return napi_set_last_error(env, napi_invalid_arg);
  }

  // Tag can only be set once.
  RETURN_STATUS_IF_FALSE(env, !decoration->hasTypeTag, napi_invalid_arg);

  decoration->hasTypeTag = true;
  decoration->typeTagLower = type_tag->lower;
  decoration->typeTagUpper = type_tag->upper;

  return napi_clear_last_error(env);
}

napi_status napiExternalCheckTypeTag(
    napi_env env,
    HermesValue hv,
    const napi_type_tag *type_tag,
    bool *result) {
  auto *decoratedObj = vmcast<DecoratedObject>(hv);
  auto *decoration =
      static_cast<NapiExternalDecoration *>(decoratedObj->getDecoration());
  if (!decoration) {
    *result = false;
    return napi_clear_last_error(env);
  }

  *result = decoration->hasTypeTag &&
      decoration->typeTagLower == type_tag->lower &&
      decoration->typeTagUpper == type_tag->upper;

  return napi_clear_last_error(env);
}
