/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/NativeState.h"
#include "hermes/VM/Predefined.h"

using namespace hermes::vm;

//===========================================================================
// NapiObjectData helpers
//===========================================================================

/// The SymbolID for the consolidated NAPI internal property.
static SymbolID napiDataSymbolID() {
  return Predefined::getSymbolID(Predefined::InternalPropertyNapiData);
}

/// NativeState destructor callback for the consolidated NapiObjectData.
/// This runs during GC sweep, so it must NOT call back into JS (GC
/// reentrancy). Instead, it queues finalizer callbacks for deferred
/// execution outside GC via the env's pending finalizer queue.
///
/// The env is owned by the Runtime and outlives every GC cycle,
/// including the one driven by ~Runtime::getHeap().finalizeAll(),
/// so dereferencing it here is always safe.
static void finalizeNapiObjectData(GC &, NativeState *ns) {
  auto *data = static_cast<NapiObjectData *>(ns->context());
  if (!data)
    return;

  napi_env env = data->env;

  if (data->wrapFinalizeCb) {
    env->queuePendingFinalizer(
        data->wrapFinalizeCb, data->wrapData, data->wrapFinalizeHint);
  }

  auto *entry = data->finalizerChain;
  while (entry) {
    if (entry->finalize_cb) {
      env->queuePendingFinalizer(
          entry->finalize_cb, entry->data, entry->finalize_hint);
    }
    auto *next = entry->next;
    delete entry;
    entry = next;
  }

  delete data;
}

//===========================================================================
// getNapiObjectData
//===========================================================================

napi_status getNapiObjectData(
    napi_env env,
    napi_value js_object,
    NapiObjectData **outData) {
  auto *phv = reinterpret_cast<PinnedHermesValue *>(js_object);
  HermesValue hv = *phv;

  RETURN_STATUS_IF_FALSE(env, hv.isObject(), napi_invalid_arg);

  Runtime &runtime = env->runtime;
  GCScope gcScope(runtime);

  auto objHandle = Handle<JSObject>::vmcast(phv);

  NamedPropertyDescriptor desc;
  bool exists = JSObject::getOwnNamedDescriptor(
      objHandle, runtime, napiDataSymbolID(), desc);
  if (!exists) {
    *outData = nullptr;
    return napi_ok;
  }

  // Read the NativeState from the property slot.
  SmallHermesValue shv =
      JSObject::getNamedSlotValueUnsafe(*objHandle, runtime, desc);
  auto nsHV = shv.unboxToHV(runtime);
  if (!nsHV.isObject()) {
    *outData = nullptr;
    return napi_ok;
  }

  auto *ns = dyn_vmcast<NativeState>(nsHV);
  if (!ns) {
    *outData = nullptr;
    return napi_ok;
  }

  *outData = static_cast<NapiObjectData *>(ns->context());
  return napi_ok;
}

//===========================================================================
// getOrCreateNapiObjectData
//===========================================================================

napi_status getOrCreateNapiObjectData(
    napi_env env,
    Handle<JSObject> objHandle,
    NapiObjectData **outData) {
  Runtime &runtime = env->runtime;

  NamedPropertyDescriptor desc;
  bool exists = JSObject::getOwnNamedDescriptor(
      objHandle, runtime, napiDataSymbolID(), desc);

  if (exists) {
    // Read the existing NativeState from the property slot.
    SmallHermesValue shv =
        JSObject::getNamedSlotValueUnsafe(*objHandle, runtime, desc);
    auto nsHV = shv.unboxToHV(runtime);
    auto *ns = vmcast<NativeState>(nsHV);
    *outData = static_cast<NapiObjectData *>(ns->context());
    return napi_ok;
  }

  // Create a new NapiObjectData and define the internal property.
  auto *data = new NapiObjectData{env};

  struct : public Locals {
    PinnedValue<NativeState> ns;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.ns = NativeState::create(runtime, data, finalizeNapiObjectData);

  auto defRes = JSObject::defineOwnProperty(
      objHandle,
      runtime,
      napiDataSymbolID(),
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      lv.ns);

  if (LLVM_UNLIKELY(defRes == ExecutionStatus::EXCEPTION)) {
    // The NativeState's finalizer will eventually clean up data.
    return captureRuntimeException(env, napi_pending_exception);
  }

  if (!*defRes) {
    // defineOwnProperty returned false (e.g., object not extensible).
    // Prevent the NativeState finalizer from calling user code.
    auto *nsAfter = *lv.ns;
    nsAfter->setContext(nullptr);
    delete data;
    return napi_set_last_error(env, napi_generic_failure);
  }

  *outData = data;
  return napi_ok;
}

//===========================================================================
// napi_wrap
//===========================================================================

napi_status NAPI_CDECL napi_wrap(
    napi_env env,
    napi_value js_object,
    void *native_object,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint,
    napi_ref *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, js_object);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(js_object);
  HermesValue hv = *phv;

  RETURN_STATUS_IF_FALSE(env, hv.isObject(), napi_invalid_arg);

  // Check if the object is already wrapped.
  {
    NapiObjectData *existing = nullptr;
    STATUS_CALL(getNapiObjectData(env, js_object, &existing));
    RETURN_STATUS_IF_FALSE(
        env, !existing || !existing->hasWrap, napi_invalid_arg);
  }

  // If result is non-null, a finalize_cb is required (per spec).
  if (result != nullptr) {
    CHECK_ARG(env, finalize_cb);
  }

  Runtime &runtime = env->runtime;
  GCScope gcScope(runtime);

  auto objHandle = Handle<JSObject>::vmcast(phv);

  NapiObjectData *napiData = nullptr;
  STATUS_CALL(getOrCreateNapiObjectData(env, objHandle, &napiData));

  // Set wrap fields.
  // When a ref is returned to the user, the finalizer is stored in
  // the ref (not here), to avoid double-invocation.
  napiData->hasWrap = true;
  napiData->wrapData = native_object;
  napiData->wrapFinalizeCb = result != nullptr
      ? nullptr
      : reinterpret_cast<napi_finalize>(finalize_cb);
  napiData->wrapFinalizeHint = result != nullptr ? nullptr : finalize_hint;

  // Optionally create a weak reference to the object.
  if (result != nullptr) {
    auto *ref = new napi_ref__;
    ref->env = env;
    ref->refcount = 0; // weak reference
    // Re-read from the GC-tracked PinnedHermesValue — the raw copy
    // `hv` captured earlier may be stale after allocations above.
    ref->value = *phv;
    ref->finalize_cb = reinterpret_cast<napi_finalize>(finalize_cb);
    ref->finalize_data = native_object;
    ref->finalize_hint = finalize_hint;

    // Move the value into a weak slot.
    ref->createWeakSlot(runtime);

    env->addReference(ref);
    *result = ref;
  }

  return napi_clear_last_error(env);
}

//===========================================================================
// napi_unwrap
//===========================================================================

napi_status NAPI_CDECL
napi_unwrap(napi_env env, napi_value js_object, void **result) {
  CHECK_ENV(env);
  CHECK_ARG(env, js_object);
  CHECK_ARG(env, result);

  NapiObjectData *napiData = nullptr;
  STATUS_CALL(getNapiObjectData(env, js_object, &napiData));

  RETURN_STATUS_IF_FALSE(
      env, napiData != nullptr && napiData->hasWrap, napi_invalid_arg);

  *result = napiData->wrapData;
  return napi_clear_last_error(env);
}

//===========================================================================
// napi_remove_wrap
//===========================================================================

napi_status NAPI_CDECL
napi_remove_wrap(napi_env env, napi_value js_object, void **result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, js_object);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(js_object);
  HermesValue hv = *phv;

  RETURN_STATUS_IF_FALSE(env, hv.isObject(), napi_invalid_arg);

  NapiObjectData *napiData = nullptr;
  STATUS_CALL(getNapiObjectData(env, js_object, &napiData));

  RETURN_STATUS_IF_FALSE(
      env, napiData != nullptr && napiData->hasWrap, napi_invalid_arg);

  if (result) {
    *result = napiData->wrapData;
  }

  // Clear wrap fields. Do NOT delete the internal property —
  // other data (type tag, finalizers) may still be present.
  napiData->hasWrap = false;
  napiData->wrapData = nullptr;
  napiData->wrapFinalizeCb = nullptr;
  napiData->wrapFinalizeHint = nullptr;

  return napi_clear_last_error(env);
}

//===========================================================================
// napi_add_finalizer
//===========================================================================

napi_status NAPI_CDECL napi_add_finalizer(
    napi_env env,
    napi_value js_object,
    void *finalize_data,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint,
    napi_ref *result) {
  // V8 reference: "Omit NAPI_PREAMBLE ... because V8 calls here
  // cannot throw JS exceptions." We still need NAPI_PREAMBLE because
  // defineOwnProperty can throw in Hermes.
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, js_object);
  CHECK_ARG(env, finalize_cb);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(js_object);
  HermesValue hv = *phv;

  RETURN_STATUS_IF_FALSE(env, hv.isObject(), napi_invalid_arg);

  Runtime &runtime = env->runtime;
  GCScope gcScope(runtime);

  auto objHandle = Handle<JSObject>::vmcast(phv);

  NapiObjectData *napiData = nullptr;
  STATUS_CALL(getOrCreateNapiObjectData(env, objHandle, &napiData));

  // Create the new finalizer entry and prepend to chain.
  auto *entry = new NapiFinalizerEntry{
      finalize_data,
      reinterpret_cast<napi_finalize>(finalize_cb),
      finalize_hint,
      napiData->finalizerChain};
  napiData->finalizerChain = entry;

  // Optionally create a weak reference to the object.
  if (result != nullptr) {
    auto *ref = new napi_ref__;
    ref->env = env;
    ref->refcount = 0; // weak reference
    // Re-read from the GC-tracked PinnedHermesValue — the raw copy
    // `hv` captured earlier may be stale after allocations above.
    ref->value = *phv;

    // Move the value into a weak slot.
    ref->createWeakSlot(runtime);

    env->addReference(ref);
    *result = ref;
  }

  return napi_clear_last_error(env);
}
