/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Predefined.h"

using namespace hermes::vm;

//===========================================================================
// napi_type_tag_object
//===========================================================================

napi_status NAPI_CDECL napi_type_tag_object(
    napi_env env,
    napi_value value,
    const napi_type_tag *type_tag) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, type_tag);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(value);
  HermesValue hv = *phv;

  // Handle externals (DecoratedObject).
  if (isNapiExternal(hv)) {
    return napiExternalSetTypeTag(env, hv, type_tag);
  }

  // Must be a regular object.
  RETURN_STATUS_IF_FALSE(env, hv.isObject(), napi_object_expected);

  // Check if the object already has a type tag (re-tag is an error).
  {
    NapiObjectData *existing = nullptr;
    STATUS_CALL(getNapiObjectData(env, value, &existing));
    RETURN_STATUS_IF_FALSE(
        env, !existing || !existing->hasTypeTag, napi_invalid_arg);
  }

  auto objHandle = Handle<JSObject>::vmcast(phv);

  NapiObjectData *napiData = nullptr;
  STATUS_CALL(getOrCreateNapiObjectData(env, objHandle, &napiData));

  napiData->hasTypeTag = true;
  napiData->typeTagLower = type_tag->lower;
  napiData->typeTagUpper = type_tag->upper;

  return napi_clear_last_error(env);
}

//===========================================================================
// napi_check_object_type_tag
//===========================================================================

napi_status NAPI_CDECL napi_check_object_type_tag(
    napi_env env,
    napi_value value,
    const napi_type_tag *type_tag,
    bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, type_tag);
  CHECK_ARG(env, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(value);
  HermesValue hv = *phv;

  // Handle externals (DecoratedObject).
  if (isNapiExternal(hv)) {
    return napiExternalCheckTypeTag(env, hv, type_tag, result);
  }

  // Must be a regular object.
  if (!hv.isObject()) {
    *result = false;
    return napi_clear_last_error(env);
  }

  NapiObjectData *napiData = nullptr;
  STATUS_CALL(getNapiObjectData(env, value, &napiData));

  if (!napiData || !napiData->hasTypeTag) {
    *result = false;
    return napi_clear_last_error(env);
  }

  *result = napiData->typeTagLower == type_tag->lower &&
      napiData->typeTagUpper == type_tag->upper;

  return napi_clear_last_error(env);
}
