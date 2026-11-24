/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_COMPUTEDPROPERTYCACHE_INLINE_H
#define HERMES_VM_COMPUTEDPROPERTYCACHE_INLINE_H

#include "hermes/VM/ComputedPropertyCache.h"

#include "hermes/Support/OptValue.h"
#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

inline bool ComputedPropertyCache::isCacheable(const HiddenClass *clazz) {
  ClassFlags flags = clazz->getFlags();
  return !flags.dictionaryMode && !flags.hasIndexLikeProperties &&
      !flags.hostObject && !flags.lazyObject && !flags.proxyObject &&
      !flags.indexedStorage;
}

inline OptValue<SymbolID> ComputedPropertyCache::nameOfKey(
    Runtime &runtime,
    HermesValue key) {
  if (!key.isString())
    return llvh::None;
  const StringPrimitive *str = key.getString();
  if (!str->isUniqued())
    return llvh::None;
  return str->getUniqueID(runtime);
}

inline const ComputedPropertyCache::Entry *ComputedPropertyCache::probe(
    Runtime &runtime,
    const JSObject *obj,
    HermesValue key) const {
  // A class that is not cacheable has no entries: skip the name (and its GC
  // barrier), the hash and the load from the table, which for a key that
  // never hits is likely a miss in the data cache.
  if (!isCacheable(obj->getClass(runtime)))
    return nullptr;
  OptValue<SymbolID> name = nameOfKey(runtime, key);
  if (!name)
    return nullptr;
  return lookup(obj->getClassGCPtr(), *name);
}

inline void ComputedPropertyCache::fillSlot(
    Runtime &runtime,
    HiddenClass *clazz,
    HermesValue key,
    PropertyFlags flags,
    SlotIndex slot) {
  if (!isCacheable(clazz))
    return;
  if (OptValue<SymbolID> name = nameOfKey(runtime, key))
    recordSlot(runtime, clazz, *name, flags, slot);
}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_COMPUTEDPROPERTYCACHE_INLINE_H
