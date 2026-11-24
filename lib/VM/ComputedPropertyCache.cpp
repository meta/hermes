/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/ComputedPropertyCache-inline.h"

#include "hermes/VM/PropertyDescriptor.h"
#include "hermes/VM/RootAcceptor.h"

namespace hermes {
namespace vm {

void ComputedPropertyCache::Entry::copyFrom(const Entry &other) {
  clazz = other.clazz.getNoBarrierUnsafe();
  name = other.name;
  slot = other.slot;
  kind = other.kind;
  writable = other.writable;
  resultClazz = other.resultClazz.getNoBarrierUnsafe();
  parentEpoch = other.parentEpoch;
}

void ComputedPropertyCache::Entry::reset() {
  clazz = CompressedPointer(nullptr);
  name = SymbolID{};
  slot = 0;
  kind = 0;
  writable = 0;
  resultClazz = CompressedPointer(nullptr);
  parentEpoch = 0;
}

ComputedPropertyCache::ComputedPropertyCache()
    : table_(new Entry[kNumSets * kNumWays]()) {}

ComputedPropertyCache::Entry &ComputedPropertyCache::prepareWay0(
    CompressedPointer clazz,
    SymbolID name) {
  static_assert(kNumWays == 2, "insertion moves way 0 to way 1");
  Entry *set = &table_[setIndex(clazz, name) * kNumWays];
  // Way 0 holds the key: overwrite it.
  if (set[0].clazz == clazz && set[0].name == name)
    return set[0];
  if (!set[0].clazz) {
    // Way 0 is empty and holds no key to move. Way 1 keeps its entry
    // unless that is the key, which must not occupy both ways.
    if (set[1].clazz == clazz && set[1].name == name)
      set[1].reset();
    return set[0];
  }
  // Way 0 holds another key: it moves to way 1.
  set[1].copyFrom(set[0]);
  return set[0];
}

void ComputedPropertyCache::insertSlot(
    PointerBase &base,
    HiddenClass *clazz,
    SymbolID name,
    SlotIndex slot,
    bool writable) {
  assert(slot <= kMaxSlot && "slot too large");
  CompressedPointer cp = CompressedPointer::encodeNonNull(clazz, base);
  Entry &e = prepareWay0(cp, name);
  e.clazz = cp;
  e.name = name;
  e.slot = slot;
  e.kind = (uint32_t)Kind::Slot;
  e.writable = writable;
  e.resultClazz = CompressedPointer(nullptr);
  e.parentEpoch = 0;
}

void ComputedPropertyCache::insertAdd(
    PointerBase &base,
    HiddenClass *startClazz,
    SymbolID name,
    HiddenClass *resultClazz,
    SlotIndex slot,
    uint64_t parentEpoch) {
  assert(slot <= kMaxSlot && "slot too large");
  CompressedPointer cp = CompressedPointer::encodeNonNull(startClazz, base);
  Entry &e = prepareWay0(cp, name);
  e.clazz = cp;
  e.name = name;
  e.slot = slot;
  e.kind = (uint32_t)Kind::Add;
  e.writable = 0;
  e.resultClazz = CompressedPointer::encodeNonNull(resultClazz, base);
  e.parentEpoch = parentEpoch;
}

void ComputedPropertyCache::recordSlot(
    Runtime &runtime,
    HiddenClass *clazz,
    SymbolID name,
    PropertyFlags flags,
    SlotIndex slot) {
  NoAllocScope noAlloc{runtime};
  if (!isCacheable(clazz))
    return;
  if (flags.indexed || flags.accessor || flags.hostObject || flags.proxyObject)
    return;
  if (slot > kMaxSlot)
    return;
  insertSlot(
      runtime, clazz, name, slot, flags.writable && !flags.internalSetter);
}

void ComputedPropertyCache::markWeakRoots(WeakRootAcceptor &acceptor) {
  for (unsigned i = 0; i < kNumSets * kNumWays; ++i) {
    Entry &e = table_[i];
    if (!e.clazz)
      continue;
    CompressedPointer before = e.clazz.getNoBarrierUnsafe();
    acceptor.acceptWeak(e.clazz);
    bool isAdd = e.getKind() == Kind::Add;
    if (isAdd)
      acceptor.acceptWeak(e.resultClazz);
    if (!e.clazz || e.clazz != before || (isAdd && !e.resultClazz))
      e.reset();
  }
}

unsigned ComputedPropertyCache::numEntries() const {
  unsigned count = 0;
  for (unsigned i = 0; i < kNumSets * kNumWays; ++i)
    count += (bool)table_[i].clazz;
  return count;
}

void ComputedPropertyCache::clear() {
  for (unsigned i = 0; i < kNumSets * kNumWays; ++i)
    table_[i].reset();
}

} // namespace vm
} // namespace hermes
