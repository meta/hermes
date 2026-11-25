/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/Callable.h"
#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PropertyAccessor.h"

#include "llvh/ADT/DenseSet.h"

#include <cstring>

//===========================================================================
// Property operations by value key
//===========================================================================

napi_status NAPI_CDECL napi_set_property(
    napi_env env,
    napi_value object,
    napi_value key,
    napi_value value) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, key);
  CHECK_ARG(env, value);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);
  auto keyHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(key));
  auto valHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(value));

  auto putRes = hermes::vm::JSObject::putComputed_RJS(
      objHandle, env->runtime, keyHandle, valHandle);
  if (LLVM_UNLIKELY(putRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_property(
    napi_env env,
    napi_value object,
    napi_value key,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, key);
  CHECK_ARG(env, result);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);
  auto keyHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(key));

  auto getRes =
      hermes::vm::JSObject::getComputed_RJS(objHandle, env->runtime, keyHandle);
  if (LLVM_UNLIKELY(getRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = env->addToCurrentScope(getRes->get());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_has_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  CHECK_ARG(env, key);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);
  auto keyHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(key));

  auto hasRes =
      hermes::vm::JSObject::hasComputed(objHandle, env->runtime, keyHandle);
  if (LLVM_UNLIKELY(hasRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = *hasRes;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_delete_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, key);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);
  auto keyHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(key));

  auto delRes =
      hermes::vm::JSObject::deleteComputed(objHandle, env->runtime, keyHandle);
  if (LLVM_UNLIKELY(delRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  if (result != nullptr) {
    *result = *delRes;
  }

  return napi_clear_last_error(env);
}

//===========================================================================
// Named property operations
//===========================================================================

napi_status NAPI_CDECL napi_set_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    napi_value value) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, utf8name);
  CHECK_ARG(env, value);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);
  auto valHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(value));

  struct : public hermes::vm::Locals {
    hermes::vm::PinnedValue<hermes::vm::SymbolID> sym;
  } lv;
  hermes::vm::LocalsRAII lraii(env->runtime, &lv);

  // Intern the property name.
  auto internStatus = internUtf8AsSymbol(env, utf8name, lv.sym);
  if (internStatus != napi_ok) {
    return internStatus;
  }

  // Use putNamedOrIndexed because the property name could be
  // index-like (e.g. "0", "1").
  auto putRes = hermes::vm::JSObject::putNamedOrIndexed(
      objHandle, env->runtime, *lv.sym, valHandle);
  if (LLVM_UNLIKELY(putRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, utf8name);
  CHECK_ARG(env, result);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);

  struct : public hermes::vm::Locals {
    hermes::vm::PinnedValue<hermes::vm::SymbolID> sym;
  } lv;
  hermes::vm::LocalsRAII lraii(env->runtime, &lv);

  // Intern the property name.
  auto internStatus = internUtf8AsSymbol(env, utf8name, lv.sym);
  if (internStatus != napi_ok) {
    return internStatus;
  }

  // Use getNamedOrIndexed because the property name could be
  // index-like (e.g. "0", "1").
  auto getRes =
      hermes::vm::JSObject::getNamedOrIndexed(objHandle, env->runtime, *lv.sym);
  if (LLVM_UNLIKELY(getRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = env->addToCurrentScope(getRes->get());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_has_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, utf8name);
  CHECK_ARG(env, result);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);

  struct : public hermes::vm::Locals {
    hermes::vm::PinnedValue<hermes::vm::SymbolID> sym;
  } lv;
  hermes::vm::LocalsRAII lraii(env->runtime, &lv);

  // Intern the property name.
  auto internStatus = internUtf8AsSymbol(env, utf8name, lv.sym);
  if (internStatus != napi_ok) {
    return internStatus;
  }

  // Use hasNamedOrIndexed because the property name could be
  // index-like (e.g. "0", "1").
  auto hasRes =
      hermes::vm::JSObject::hasNamedOrIndexed(objHandle, env->runtime, *lv.sym);
  if (LLVM_UNLIKELY(hasRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = *hasRes;
  return napi_clear_last_error(env);
}

//===========================================================================
// Element (indexed) property operations
//===========================================================================

napi_status NAPI_CDECL napi_set_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    napi_value value) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);
  auto valHandle = hermes::vm::Handle<>(
      reinterpret_cast<hermes::vm::PinnedHermesValue *>(value));

  // Convert the uint32_t index to a HermesValue number for use as a
  // computed property key.
  struct : public hermes::vm::Locals {
    hermes::vm::PinnedValue<> idx;
  } lv;
  hermes::vm::LocalsRAII lraii(env->runtime, &lv);
  lv.idx = hermes::vm::HermesValue::encodeTrustedNumberValue(index);
  auto putRes = hermes::vm::JSObject::putComputed_RJS(
      objHandle, env->runtime, lv.idx, valHandle);
  if (LLVM_UNLIKELY(putRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);

  struct : public hermes::vm::Locals {
    hermes::vm::PinnedValue<> idx;
  } lv;
  hermes::vm::LocalsRAII lraii(env->runtime, &lv);
  lv.idx = hermes::vm::HermesValue::encodeTrustedNumberValue(index);
  auto getRes =
      hermes::vm::JSObject::getComputed_RJS(objHandle, env->runtime, lv.idx);
  if (LLVM_UNLIKELY(getRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = env->addToCurrentScope(getRes->get());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_has_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);

  struct : public hermes::vm::Locals {
    hermes::vm::PinnedValue<> idx;
  } lv;
  hermes::vm::LocalsRAII lraii(env->runtime, &lv);
  lv.idx = hermes::vm::HermesValue::encodeTrustedNumberValue(index);
  auto hasRes =
      hermes::vm::JSObject::hasComputed(objHandle, env->runtime, lv.idx);
  if (LLVM_UNLIKELY(hasRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = *hasRes;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_delete_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    bool *result) {
  NAPI_PREAMBLE(env);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);

  struct : public hermes::vm::Locals {
    hermes::vm::PinnedValue<> idx;
  } lv;
  hermes::vm::LocalsRAII lraii(env->runtime, &lv);
  lv.idx = hermes::vm::HermesValue::encodeTrustedNumberValue(index);
  auto delRes =
      hermes::vm::JSObject::deleteComputed(objHandle, env->runtime, lv.idx);
  if (LLVM_UNLIKELY(delRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  if (result != nullptr) {
    *result = *delRes;
  }

  return napi_clear_last_error(env);
}

//===========================================================================
// Property enumeration and prototype
//===========================================================================

napi_status NAPI_CDECL napi_get_all_property_names(
    napi_env env,
    napi_value object,
    napi_key_collection_mode key_mode,
    napi_key_filter key_filter,
    napi_key_conversion key_conversion,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  // Validate enum parameters.
  RETURN_STATUS_IF_FALSE(
      env,
      key_mode == napi_key_include_prototypes || key_mode == napi_key_own_only,
      napi_invalid_arg);
  RETURN_STATUS_IF_FALSE(
      env,
      key_conversion == napi_key_keep_numbers ||
          key_conversion == napi_key_numbers_to_strings,
      napi_invalid_arg);

  // Build OwnKeysFlags from the filter.
  hermes::vm::OwnKeysFlags okFlags;
  if (!(key_filter & napi_key_skip_strings))
    okFlags = okFlags.plusIncludeNonSymbols();
  if (!(key_filter & napi_key_skip_symbols))
    okFlags = okFlags.plusIncludeSymbols();
  // At least one of IncludeSymbols or IncludeNonSymbols must be set.
  if ((key_filter & napi_key_skip_strings) &&
      (key_filter & napi_key_skip_symbols))
    okFlags = okFlags.plusIncludeNonSymbols();

  // Always include non-enumerable so we can post-filter uniformly
  // for all attribute filters (writable, enumerable, configurable).
  okFlags = okFlags.plusIncludeNonEnumerable();

  // Always request KeepSymbols: string-named properties of ordinary objects
  // come back as uniqued SymbolIDs and indices as numbers, which are
  // GC-stable identities that can be used for deduplication. (Proxy and
  // HostObject results can still contain plain strings; those are uniqued
  // below.) With IncludeSymbols not set, only string-named properties are
  // returned; real Symbols are never uniqued.
  okFlags = okFlags.plusKeepSymbols();

  bool needAttrFilter =
      (key_filter &
       (napi_key_writable | napi_key_enumerable | napi_key_configurable)) != 0;

  struct : public hermes::vm::Locals {
    hermes::vm::PinnedValue<hermes::vm::JSArray> resultArray;
    hermes::vm::PinnedValue<hermes::vm::JSObject> curObj;
    hermes::vm::PinnedValue<hermes::vm::JSArray> keysArray;
    hermes::vm::PinnedValue<hermes::vm::JSArray> seenArray;
    hermes::vm::PinnedValue<hermes::vm::SymbolID> sym;
    hermes::vm::PinnedValue<> tmp;
  } lv;
  hermes::vm::LocalsRAII lraii(env->runtime, &lv);

  // Create the result array.
  auto arrRes = hermes::vm::JSArray::create(
      env->runtime,
      hermes::vm::Handle<hermes::vm::JSObject>::vmcast(
          &env->runtime.arrayPrototype),
      0,
      0);
  if (LLVM_UNLIKELY(arrRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }
  lv.resultArray = std::move(*arrRes);
  uint32_t resultIndex = 0;

  // For prototype chain deduplication, track seen keys by GC-stable
  // identity. The set key is 64 bits wide with disjoint domains: array
  // indices have kIndexTag set, and all other keys are the raw SymbolID
  // (uniqued or not) with the tag clear. Raw GC pointers must not be used,
  // since strings move.
  constexpr uint64_t kIndexTag = uint64_t(1) << 63;
  const bool useDedup = key_mode == napi_key_include_prototypes;
  llvh::SmallDenseSet<uint64_t, 16> seenKeys;
  bool needDedup = false;
  // Nothing else roots the symbols recorded in seenKeys once the object they
  // came from goes out of scope, and an unrooted SymbolID can be freed and
  // reused for a different name. Keep every recorded symbol alive in seenArray.
  // It is created when the first symbol is recorded.
  uint32_t seenCount = 0;

  lv.curObj = hermes::vm::vmcast<hermes::vm::JSObject>(*objPhv);

  // Walk the object (and optionally its prototype chain).
  for (;;) {
    auto keysRes = hermes::vm::JSObject::getOwnPropertyKeys(
        lv.curObj, env->runtime, okFlags);
    if (LLVM_UNLIKELY(keysRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
      return captureRuntimeException(env, napi_pending_exception);
    }

    lv.keysArray = std::move(*keysRes);
    uint32_t len =
        hermes::vm::JSArray::getLength(lv.keysArray.get(), env->runtime);

    for (uint32_t i = 0; i < len; ++i) {
      // Every element is read from the rooted keys array at its point of use,
      // because the calls below can move the key.
      auto keyAt = [&lv, env, i]() {
        return lv.keysArray->at(env->runtime, i).unboxToHV(env->runtime);
      };

      if (useDedup) {
        // Compute the dedup key. Numbers are indices; Symbol values are
        // uniqued string names or real Symbols; strings (from Proxy or
        // HostObject results) are uniqued to a SymbolID.
        uint64_t dedupKey;
        bool isSym = true;
        hermes::vm::HermesValue elem = keyAt();
        if (elem.isNumber()) {
          dedupKey = kIndexTag | static_cast<uint32_t>(elem.getNumber());
          isSym = false;
        } else if (elem.isSymbol()) {
          lv.sym = elem.getSymbol();
          dedupKey = lv.sym.get().unsafeGetRaw();
        } else {
          if (LLVM_UNLIKELY(
                  env->runtime.getIdentifierTable().getSymbolFromPrimitive(
                      env->runtime,
                      hermes::vm::createPseudoHandle(elem.getString()),
                      lv.sym) == hermes::vm::ExecutionStatus::EXCEPTION)) {
            return captureRuntimeException(env, napi_pending_exception);
          }
          // An ordinary object reports an index-like name as a number, so map
          // an index-like string into the index domain to dedup against it.
          if (auto idx = env->runtime.getIdentifierTable().symbolToArrayIndex(
                  lv.sym.get())) {
            dedupKey = kIndexTag | *idx;
            isSym = false;
          } else {
            dedupKey = lv.sym.get().unsafeGetRaw();
          }
        }

        bool inserted = seenKeys.insert(dedupKey).second;
        if (needDedup && !inserted)
          continue;
        if (inserted && isSym) {
          if (!lv.seenArray.get()) {
            auto seenRes = hermes::vm::JSArray::create(env->runtime, 0, 0);
            if (LLVM_UNLIKELY(
                    seenRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
              return captureRuntimeException(env, napi_pending_exception);
            }
            lv.seenArray = std::move(*seenRes);
          }
          lv.tmp = hermes::vm::HermesValue::encodeSymbolValue(lv.sym.get());
          if (LLVM_UNLIKELY(
                  hermes::vm::JSArray::setElementAt(
                      lv.seenArray, env->runtime, seenCount++, lv.tmp) ==
                  hermes::vm::ExecutionStatus::EXCEPTION)) {
            return captureRuntimeException(env, napi_pending_exception);
          }
        }
      }

      // Apply attribute filters if needed.
      if (needAttrFilter) {
        hermes::vm::ComputedPropertyDescriptor desc;
        lv.tmp = keyAt();
        auto found = hermes::vm::JSObject::getOwnComputedDescriptor(
            lv.curObj, env->runtime, lv.tmp, desc);
        if (LLVM_UNLIKELY(found == hermes::vm::ExecutionStatus::EXCEPTION)) {
          return captureRuntimeException(env, napi_pending_exception);
        }
        if (!*found)
          continue;

        // All specified attribute filters must match.
        if ((key_filter & napi_key_enumerable) && !desc.flags.enumerable)
          continue;
        if ((key_filter & napi_key_writable) && !desc.flags.writable)
          continue;
        if ((key_filter & napi_key_configurable) && !desc.flags.configurable)
          continue;
      }

      // The calls above may have run Proxy traps and triggered a GC that
      // moved the key. Re-read it from the rooted keys array.
      hermes::vm::HermesValue key = keyAt();

      // Because KeepSymbols is set, getOwnPropertyKeys returns string-named
      // properties as uniqued Hermes SymbolValues. Convert them back to
      // StringPrimitive values so NAPI consumers see them as JS strings, not
      // JS Symbols. Real Symbols are not uniqued and are kept as is.
      lv.tmp = key;
      if (key.isSymbol()) {
        hermes::vm::SymbolID symId = key.getSymbol();
        if (symId.isUniqued()) {
          lv.tmp = hermes::vm::HermesValue::encodeStringValue(
              env->runtime.getStringPrimFromSymbolID(symId));
        }
      }

      // Convert number to string if requested.
      if (key_conversion == napi_key_numbers_to_strings && key.isNumber()) {
        auto strRes = hermes::vm::toString_RJS(env->runtime, lv.tmp);
        if (LLVM_UNLIKELY(strRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
          return captureRuntimeException(env, napi_pending_exception);
        }
        lv.tmp = strRes->getHermesValue();
      }

      // Append to result array.
      if (LLVM_UNLIKELY(
              hermes::vm::JSArray::setElementAt(
                  lv.resultArray, env->runtime, resultIndex, lv.tmp) ==
              hermes::vm::ExecutionStatus::EXCEPTION)) {
        return captureRuntimeException(env, napi_pending_exception);
      }
      ++resultIndex;
    }

    // If own-only, stop after the first object.
    if (key_mode == napi_key_own_only)
      break;

    // Move to the prototype.
    auto protoRes = hermes::vm::JSObject::getPrototypeOf(
        hermes::vm::createPseudoHandle(*lv.curObj), env->runtime);
    if (LLVM_UNLIKELY(protoRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
      return captureRuntimeException(env, napi_pending_exception);
    }
    if (!protoRes->get())
      break;
    lv.curObj = std::move(*protoRes);
    needDedup = true;
  }

  // Set the final length of the result array.
  if (resultIndex > 0) {
    auto setLenRes = hermes::vm::JSArray::setLengthProperty(
        lv.resultArray, env->runtime, resultIndex);
    if (LLVM_UNLIKELY(setLenRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
      return captureRuntimeException(env, napi_pending_exception);
    }
  }

  *result = env->addToCurrentScope(lv.resultArray.getHermesValue());
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_property_names(napi_env env, napi_value object, napi_value *result) {
  // napi_get_property_names is equivalent to
  // napi_get_all_property_names with: include prototypes,
  // enumerable + skip_symbols, numbers_to_strings.
  // This matches the V8 reference implementation.
  return napi_get_all_property_names(
      env,
      object,
      napi_key_include_prototypes,
      static_cast<napi_key_filter>(napi_key_enumerable | napi_key_skip_symbols),
      napi_key_numbers_to_strings,
      result);
}

napi_status NAPI_CDECL napi_has_own_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, key);
  CHECK_ARG(env, result);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  // The key must be a string or symbol (a "name"), matching V8 which
  // requires k->IsName().
  auto *keyPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(key);
  RETURN_STATUS_IF_FALSE(
      env, keyPhv->isString() || keyPhv->isSymbol(), napi_name_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);
  auto keyHandle = hermes::vm::Handle<>(keyPhv);

  // Use getOwnComputedDescriptor to check if the property exists on
  // the object itself (no prototype chain traversal).
  hermes::vm::ComputedPropertyDescriptor desc;
  auto hasRes = hermes::vm::JSObject::getOwnComputedDescriptor(
      objHandle, env->runtime, keyHandle, desc);
  if (LLVM_UNLIKELY(hasRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = *hasRes;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_prototype(napi_env env, napi_value object, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  auto objHandle = hermes::vm::Handle<hermes::vm::JSObject>::vmcast(objPhv);

  // Use getPrototypeOf which is the proxy-aware version.
  // It returns a null JSObject pointer (not a null JS value) if
  // there is no prototype.
  auto protoRes = hermes::vm::JSObject::getPrototypeOf(
      hermes::vm::createPseudoHandle(*objHandle), env->runtime);
  if (LLVM_UNLIKELY(protoRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  hermes::vm::JSObject *proto = protoRes->get();
  if (proto) {
    *result = env->addToCurrentScope(
        hermes::vm::HermesValue::encodeObjectValue(proto));
  } else {
    *result =
        env->addToCurrentScope(hermes::vm::HermesValue::encodeNullValue());
  }

  return napi_clear_last_error(env);
}

//===========================================================================
// Define properties
//===========================================================================

/// Helper: create a NativeFunction wrapping a napi_callback for use as
/// a method, getter, or setter in napi_define_properties and
/// napi_define_class. The callback bundle is stored in the env's deque
/// for lifetime management.
napi_status createNapiNativeFunction(
    napi_env env,
    napi_callback cb,
    void *data,
    hermes::vm::SymbolID name,
    unsigned paramCount,
    hermes::vm::PseudoHandle<hermes::vm::NativeFunction> &out) {
  using namespace hermes::vm;

  Runtime &runtime = env->runtime;
  struct : public Locals {
    PinnedValue<NativeFunction> ctor;
    PinnedValue<JSObject> proto;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Allocate a callback bundle owned by the env.
  env->callbackBundles_.push_back({env, cb, data});
  CallbackBundle *bundle = &env->callbackBundles_.back();

  // Use NativeConstructor so functions are callable with 'new',
  // matching V8/Node behavior where all NAPI functions are
  // constructable. napiConstructorTrampoline handles both regular
  // calls and constructor calls.
  auto ctorPH = NativeConstructor::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime.functionPrototype),
      bundle,
      napiConstructorTrampoline,
      paramCount);

  // Root the constructor before creating the prototype, since
  // JSObject::create is a GC safepoint that can invalidate the
  // PseudoHandle.
  lv.ctor.castAndSetHermesValue<NativeFunction>(ctorPH.getHermesValue());

  // Create a prototype object so the function can be used as a
  // base class with extends.
  auto protoObj = JSObject::create(runtime);
  lv.proto = std::move(protoObj);

  // Set .name, .length, and .prototype on the function.
  auto defRes = Callable::defineNameLengthAndPrototype(
      Handle<Callable>::vmcast(&lv.ctor),
      runtime,
      name,
      paramCount,
      lv.proto,
      Callable::WritablePrototype::Yes);
  if (LLVM_UNLIKELY(defRes == ExecutionStatus::EXCEPTION)) {
    env->callbackBundles_.pop_back();
    return napi_generic_failure;
  }

  out = PseudoHandle<NativeFunction>::create(*lv.ctor);

  return napi_ok;
}

napi_status NAPI_CDECL napi_define_properties(
    napi_env env,
    napi_value object,
    size_t property_count,
    const napi_property_descriptor *properties) {
  NAPI_PREAMBLE(env);
  if (property_count > 0) {
    CHECK_ARG(env, properties);
  }

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(
      env, objPhv != nullptr && objPhv->isObject(), napi_object_expected);

  using namespace hermes::vm;
  Runtime &runtime = env->runtime;
  struct : public Locals {
    PinnedValue<Callable> getter;
    PinnedValue<Callable> setter;
    PinnedValue<PropertyAccessor> accessor;
    PinnedValue<NativeFunction> method;
    PinnedValue<SymbolID> propSym;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto objHandle = Handle<JSObject>::vmcast(objPhv);

  for (size_t i = 0; i < property_count; ++i) {
    const napi_property_descriptor *p = &properties[i];

    // Resolve the property key. Either utf8name (C string) or name
    // (napi_value that must be a string or symbol).
    if (p->utf8name != nullptr) {
      auto internStatus = internUtf8AsSymbol(env, p->utf8name, lv.propSym);
      if (internStatus != napi_ok) {
        return internStatus;
      }
    } else {
      auto *namePhv = reinterpret_cast<PinnedHermesValue *>(p->name);
      RETURN_STATUS_IF_FALSE(
          env,
          namePhv != nullptr && (namePhv->isString() || namePhv->isSymbol()),
          napi_name_expected);

      if (namePhv->isString()) {
        // Intern the string to get a SymbolID.
        if (LLVM_UNLIKELY(
                runtime.getIdentifierTable().getSymbolFromPrimitive(
                    runtime,
                    PseudoHandle<StringPrimitive>::create(
                        vmcast<StringPrimitive>(*namePhv)),
                    lv.propSym) == ExecutionStatus::EXCEPTION)) {
          return captureRuntimeException(env, napi_pending_exception);
        }
      } else {
        // Symbol value — extract the SymbolID directly.
        lv.propSym = namePhv->getSymbol();
      }
    }

    if (p->getter != nullptr || p->setter != nullptr) {
      // Case 1: Accessor property (getter and/or setter).
      Handle<Callable> getterHandle = Runtime::makeNullHandle<Callable>();
      Handle<Callable> setterHandle = Runtime::makeNullHandle<Callable>();

      if (p->getter != nullptr) {
        PseudoHandle<NativeFunction> getterFunc{};
        STATUS_CALL(createNapiNativeFunction(
            env, p->getter, p->data, *lv.propSym, 0, getterFunc));
        lv.getter.castAndSetHermesValue<Callable>(getterFunc.getHermesValue());
        getterHandle = lv.getter;
      }
      if (p->setter != nullptr) {
        PseudoHandle<NativeFunction> setterFunc{};
        STATUS_CALL(createNapiNativeFunction(
            env, p->setter, p->data, *lv.propSym, 1, setterFunc));
        lv.setter.castAndSetHermesValue<Callable>(setterFunc.getHermesValue());
        setterHandle = lv.setter;
      }

      auto accessor =
          PropertyAccessor::create(runtime, getterHandle, setterHandle);
      lv.accessor = std::move(accessor);

      DefinePropertyFlags dpf{};
      dpf.setEnumerable = 1;
      dpf.enumerable = (p->attributes & napi_enumerable) ? 1 : 0;
      dpf.setConfigurable = 1;
      dpf.configurable = (p->attributes & napi_configurable) ? 1 : 0;
      dpf.setGetter = 1;
      dpf.setSetter = 1;

      auto defRes = JSObject::defineOwnProperty(
          objHandle,
          runtime,
          *lv.propSym,
          dpf,
          lv.accessor,
          PropOpFlags().plusThrowOnError());
      if (LLVM_UNLIKELY(defRes == ExecutionStatus::EXCEPTION)) {
        return captureRuntimeException(env, napi_pending_exception);
      }
      RETURN_STATUS_IF_FALSE(env, *defRes, napi_invalid_arg);
    } else if (p->method != nullptr) {
      // Case 2: Method property.
      PseudoHandle<NativeFunction> methodFunc{};
      STATUS_CALL(createNapiNativeFunction(
          env, p->method, p->data, *lv.propSym, 0, methodFunc));
      lv.method = std::move(methodFunc);

      DefinePropertyFlags dpf{};
      dpf.setEnumerable = 1;
      dpf.enumerable = (p->attributes & napi_enumerable) ? 1 : 0;
      dpf.setWritable = 1;
      dpf.writable = (p->attributes & napi_writable) ? 1 : 0;
      dpf.setConfigurable = 1;
      dpf.configurable = (p->attributes & napi_configurable) ? 1 : 0;
      dpf.setValue = 1;

      auto defRes = JSObject::defineOwnProperty(
          objHandle,
          runtime,
          *lv.propSym,
          dpf,
          lv.method,
          PropOpFlags().plusThrowOnError());
      if (LLVM_UNLIKELY(defRes == ExecutionStatus::EXCEPTION)) {
        return captureRuntimeException(env, napi_pending_exception);
      }
      RETURN_STATUS_IF_FALSE(env, *defRes, napi_generic_failure);
    } else {
      // Case 3: Data property (plain value).
      auto *valPhv = reinterpret_cast<PinnedHermesValue *>(p->value);
      auto valHandle = Handle<>(valPhv);

      DefinePropertyFlags dpf{};
      dpf.setEnumerable = 1;
      dpf.enumerable = (p->attributes & napi_enumerable) ? 1 : 0;
      dpf.setWritable = 1;
      dpf.writable = (p->attributes & napi_writable) ? 1 : 0;
      dpf.setConfigurable = 1;
      dpf.configurable = (p->attributes & napi_configurable) ? 1 : 0;
      dpf.setValue = 1;

      auto defRes = JSObject::defineOwnProperty(
          objHandle,
          runtime,
          *lv.propSym,
          dpf,
          valHandle,
          PropOpFlags().plusThrowOnError());
      if (LLVM_UNLIKELY(defRes == ExecutionStatus::EXCEPTION)) {
        return captureRuntimeException(env, napi_pending_exception);
      }
      RETURN_STATUS_IF_FALSE(env, *defRes, napi_invalid_arg);
    }
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_object_freeze(napi_env env, napi_value object) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, object);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(env, objPhv->isObject(), napi_object_expected);

  using namespace hermes::vm;
  Runtime &runtime = env->runtime;

  auto objHandle = Handle<JSObject>::vmcast(objPhv);
  auto status = JSObject::freeze(objHandle, runtime);
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, object);

  auto *objPhv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(object);
  RETURN_STATUS_IF_FALSE(env, objPhv->isObject(), napi_object_expected);

  using namespace hermes::vm;
  Runtime &runtime = env->runtime;

  auto objHandle = Handle<JSObject>::vmcast(objPhv);
  auto status = JSObject::seal(objHandle, runtime);
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  return napi_clear_last_error(env);
}
