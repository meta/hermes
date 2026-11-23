/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSWEAKMAPIMPL_H
#define HERMES_VM_JSWEAKMAPIMPL_H

#include "hermes/VM/CallResult.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/WeakRef.h"

namespace hermes {
namespace vm {

namespace detail {

/// Used as the key to the DenseSet in JSWeakMapImpl.
/// Packages the hash with the WeakRef itself.
/// Uses invalid locations as slots for empty and tombstone keys.
class WeakRefKey {
  /// Used to construct Empty/Tombstone WeakRefKey.
  static constexpr uintptr_t kEmptyKey = 0;
  static constexpr uintptr_t kTombstoneKey = 1;

  /// Slot to the key object (packaged with its mapped value and owning map).
  WeakMapEntrySlot *slot_;

  /// GC-stable hash value of the JSObject pointed to by ref, while it's alive.
  uint32_t hash_;

  explicit WeakRefKey(WeakMapEntrySlot *slot, uint32_t hash)
      : slot_(slot), hash_(hash) {}

 public:
  WeakRefKey(
      Runtime &runtime,
      SmallHermesValue keyObjOrSym,
      uint32_t keyHash,
      HermesValue value,
      JSWeakMapImplBase *ownerMapPtr)
      : slot_(runtime.getHeap()
                  .allocWeakMapEntrySlot(keyObjOrSym, value, ownerMapPtr)),
        hash_(keyHash) {}

  /// \return The heap snapshot NodeID for this key. Note that heap snapshot
  /// are created on mutator thread with no background thread running, key
  /// objects/symbols are accessed here with no barriers.
  HeapSnapshot::NodeID getKeyObjectID(GC &gc) const {
    if (slot_->key.isObject()) {
      return gc.getObjectID(slot_->key.getPointerNoBarrierUnsafe());
    }
    assert(slot_->key.isSymbol());
    return gc.getObjectID(slot_->key.getSymbolNoBarrierUnsafe());
  }

  /// \return The mapped value by the the key object.
  HermesValue getMappedValue(GC &gc) const {
    // During marking phase in Hades, mutator thread may read a mapped value A
    // and store it to a marked object B, then deletes the entry. In
    // completeMarking(), A may become unreachable and gets swept, leaving a
    // dangling reference in B. To address it, we add a read barrier on every
    // access to mapped value. Adding writer barrier should also work but this
    // is cleaner.
    gc.weakRefReadBarrier(slot_->mappedValue);
    return slot_->mappedValue;
  }

  /// Set mapped value in slot_ to \p value.
  void setMappedValue(GC &gc, HermesValue value) {
    gc.setWeakMapEntryValue(*slot_, value);
  }

  /// Create an empty key to be used in DenseMap.
  static WeakRefKey createEmptyKey() {
    return WeakRefKey{
        reinterpret_cast<WeakMapEntrySlot *>(kEmptyKey), kEmptyKey};
  }

  /// Create a tombstone key to be used in DenseMap.
  static WeakRefKey createTombstoneKey() {
    return WeakRefKey{
        reinterpret_cast<WeakMapEntrySlot *>(kTombstoneKey), kTombstoneKey};
  }

  /// \return true if the WeakRoot to the key object is still alive.
  bool isKeyValid() const {
    return !!slot_->key;
  }

  /// \return true if the underlying slots are equal, or point to the same key
  /// object.
  bool isKeyEqual(const WeakRefKey &other) const {
    // Comparing when the slots are equal should always succeed. This also
    // handles comparison with tombstone and empty.
    if (slot_ == other.slot_)
      return true;
    // If the slots don't match and either one is tombstone or empty,
    // the comparison has failed (so we won't dereference them below).
    if (reinterpret_cast<uintptr_t>(slot_) <= kTombstoneKey ||
        reinterpret_cast<uintptr_t>(other.slot_) <= kTombstoneKey)
      return false;

    // If either key has been garbage collected, it is impossible for them to
    // compare equal. Otherwise, compare their raw bits directly since they can
    // only be Objects or Symbols.
    return slot_->key && other.slot_->key &&
        slot_->key.getRaw() == other.slot_->key.getRaw();
  }

  /// \return true if the underlying slot points to the same key object as
  /// \p keyObject. This is only used by WeakRefLookupKey, so \p keyObject
  /// should never be null.
  bool isKeyEqual(SmallHermesValue keyObjOrSym) const {
    if (reinterpret_cast<uintptr_t>(slot_) <= kTombstoneKey)
      return false;
    return slot_->key.getRaw() == keyObjOrSym.getRaw();
  }

  /// Free the WeakMapEntrySlot held by this reference.
  void releaseSlot() {
    slot_->free();
  }

  uint32_t getHash() const {
    return hash_;
  }
};

/// Used as a lookup key to the DenseSet in JSWeakMapImpl, so that we don't
/// need to allocate a new WeakMapEntrySlot for query operations. With this,
/// the only case that we will allocate a new slot is when inserting new values.
struct WeakRefLookupKey {
  /// Pointer of the key object or the key symbol.
  SmallHermesValue keyObjOrSym;

  /// GC-stable hash value of the JSObject pointed to by ref, while it's alive.
  uint32_t hash;
};

/// Enable using WeakRefKey in DenseMap.
struct WeakRefInfo {
  /// \return Empty key which is simply null.
  static inline WeakRefKey getEmptyKey() {
    return WeakRefKey::createEmptyKey();
  }
  /// \return Empty key which is simply a pointer to 0x1 - don't dereference.
  static inline WeakRefKey getTombstoneKey() {
    return WeakRefKey::createTombstoneKey();
  }
  /// \return the hash in \p key.
  static inline unsigned getHashValue(const WeakRefKey &key) {
    return key.getHash();
  }
  /// \return the hash in lookup key.
  static inline unsigned getHashValue(const WeakRefLookupKey &key) {
    return key.hash;
  }
  /// \return true both arguments are empty, both are tombstone,
  /// or both point to the same JSObject.
  static inline bool isEqual(const WeakRefKey &a, const WeakRefKey &b) {
    return a.isKeyEqual(b);
  }
  /// \return true if \p b is neither empty nor tombstone, and points to the
  /// same JSObject as refCellPtr of \p a (which should never be null pointer).
  /// In case that the key object in \p b is already garbage collected, but the
  /// WeakMapEntrySlot owned by \p b is not freed yet, this function is still
  /// correct. The hash stored in \p b does not change after the key object is
  /// collected, and the equality check in isKeyEqual() always fails since no
  /// object will ever compare equal to an object that has been freed.
  static inline bool isEqual(const WeakRefLookupKey &a, const WeakRefKey &b) {
    assert(
        ((a.keyObjOrSym.isObject() && a.keyObjOrSym.getObject()) ||
         (a.keyObjOrSym.isSymbol() && a.keyObjOrSym.getSymbol().isValid())) &&
        "LookupKey should not use a null object pointer or invalid symbol");
    return b.isKeyEqual(a.keyObjOrSym);
  }
};

} // namespace detail

/// Base implementation of JSWeakMapImpl methods,
/// used by both WeakMap and WeakSet, with no templating.
class JSWeakMapImplBase : public JSObject {
  using Super = JSObject;
  using WeakRefKey = detail::WeakRefKey;
  using DenseSetT = llvh::DenseSet<WeakRefKey, detail::WeakRefInfo>;

 protected:
  JSWeakMapImplBase(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz),
        targetSize_(/* sizingWeight */ 0.5, /* initSize */ 8) {}

 public:
  static const ObjectVTable vt;

  static bool classof(const GCCell *cell) {
    return kindInRange(
        cell->getKind(),
        CellKind::WeakMapImplBaseKind_first,
        CellKind::WeakMapImplBaseKind_last);
  }

  static void WeakMapImplBaseBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);

  /// Set a key/value, overwriting the previous value at that key,
  /// or add a new key/value if the key doesn't exist.
  /// \pre \p key must be an Object or non-registered Symbol.
  static void setValue(
      Handle<JSWeakMapImplBase> self,
      Runtime &runtime,
      Handle<> key,
      Handle<> value);

  /// Delete a key/value in the map.
  /// \return true if the key/value existed and was removed.
  /// \pre \p key must be an Object or non-registered Symbol.
  static bool
  deleteValue(Handle<JSWeakMapImplBase> self, Runtime &runtime, Handle<> key);

  /// \return true if the \p key exists in the map.
  /// \pre \p key must be an Object or non-registered Symbol.
  static bool
  hasValue(Handle<JSWeakMapImplBase> self, Runtime &runtime, Handle<> key);

  /// \return the value at \p key, if it exists. Else, return undefined.
  /// \pre \p key must be an Object or non-registered Symbol.
  static HermesValue
  getValue(Handle<JSWeakMapImplBase> self, Runtime &runtime, Handle<> key);

  /// If \p key already exists, \return its current value without overwriting
  /// it. Otherwise insert \p key -> \p value and \return \p value.
  /// \pre \p key must be an Object or non-registered Symbol.
  static HermesValue getOrInsert(
      Handle<JSWeakMapImplBase> self,
      Runtime &runtime,
      Handle<> key,
      Handle<> value);

  /// If \p key already exists, \return its current value (without invoking
  /// \p callback). Otherwise call \p callback with the key to produce the
  /// value, then store key -> value and \return that value.
  /// \pre \p key must be an Object or non-registered Symbol.
  static CallResult<HermesValue> getOrInsertComputed(
      Handle<JSWeakMapImplBase> self,
      Runtime &runtime,
      Handle<> key,
      Handle<Callable> callback);

  /// \return the size of the internal set, after freeing any freeable slots
  /// and erasing their owning keys. Used for testing purposes.
  static uint32_t debugFreeSlotsAndGetSize(
      Runtime &runtime,
      JSWeakMapImplBase *self);

  /// Iterate every slot owned by entries in `set_`, free invalid ones, and
  /// erase the owning entry in `set_` immediately. In the end, recompute
  /// `targetSize_` using the new size of `set_`.
  void clearFreeableEntries();

  /// An iterator over the keys of the map.
  using KeyIterator = DenseSetT::iterator;

 protected:
  static void _finalizeImpl(GCCell *cell, GC &gc) {
    auto *self = vmcast<JSWeakMapImplBase>(cell);
    for (auto &element : self->set_) {
      // No need to explicitly erase the owning entry of this slot since the
      // whole map/set is to be deleted.
      element.releaseSlot();
    }
    self->~JSWeakMapImplBase();
  }

  static size_t _mallocSizeImpl(GCCell *cell) {
    auto *self = vmcast<JSWeakMapImplBase>(cell);
    return self->getMallocSize();
  }

#ifdef HERMES_MEMORY_INSTRUMENTATION
  static void _snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
  static void _snapshotAddNodesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
#endif

 public:
  // Public for tests.

  /// Lazily fetches the ID for the DenseMap used in this class. After it has
  /// been assigned once it'll stay constant.
  HeapSnapshot::NodeID getMapID(GC &gc);

  /// \return the number of bytes allocated by this object on the heap.
  size_t getMallocSize() const {
    return set_.getMemorySize();
  }

 private:
  /// The underlying weak value map.
  DenseSetT set_;

  /// When the size of `set_` reaches targetSize_, clearFreeableEntries() will
  /// be called to remove freeable entries and recompute targetSize_.
  ExponentialMovingAverage targetSize_;

  /// Target occupancy ration of the map storage.
  static constexpr double kOccupancyTarget = 0.5;
};

/// Underlying representation of the WeakMap and WeakSet objects.
///
/// The key/value pairs are stored in a `DenseSet<WeakRefKey>`. Each
/// `WeakRefKey` includes the underlying `WeakMapEntryRef` and hash of the key
/// object, which allows the DenseSet to compute the hash without relying on
/// the Runtime, and keep the set internally consistent even when the referenced
/// object is freed. Each `WeakMapEntryRef` uniquely owns a `WeakMapEntrySlot`,
/// which stores three things: the WeakRoot to key object, the mapped value,
/// and WeakRoot to the owning map.
/// The size of `set_` keeps growing when new key/value pairs are inserted,
/// until reaching `targetSize_`, at which point all freeable entries are
/// removed from `set_`, the WeakMapEntrySlots owned by them are freed, and
/// `targetSize_` is recomputed.
/// Note that when an entry is removed from `set_`, its underlying slot must be
/// freed together, otherwise, that slot could be used by another key and
/// causing insertion failure.
/// Currently, we only free slots at three places:
/// 1. In deleteValue(), if the key exists.
/// 2. In clearFreeableEntries(), which is called only in setValue() and
/// debugFreeSlotsAndGetSize(). All invalid slots are freed and owning entries
/// are erased from `set_`.
/// 3. In finalizer. All slots owned by entries in `set_` are freed and the
/// entire map/set is destructed.
template <CellKind C>
class JSWeakMapImpl final : public JSWeakMapImplBase {
  using Super = JSWeakMapImplBase;

 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return C;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == C;
  }

  /// Create a new WeakMap with prototype property \p parentHandle.
  /// Create a JSWeakMapImpl using the appropriate prototype from runtime.
  static CallResult<PseudoHandle<JSWeakMapImpl<C>>> create(Runtime &runtime);

  static CallResult<PseudoHandle<JSWeakMapImpl<C>>> create(
      Runtime &runtime,
      Handle<JSObject> parentHandle);

  static void WeakMapOrSetBuildMeta(const GCCell *cell, Metadata::Builder &mb);

  JSWeakMapImpl(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSWeakMapImplBase(runtime, parent, clazz) {}
};

using JSWeakMap = JSWeakMapImpl<CellKind::JSWeakMapKind>;
using JSWeakSet = JSWeakMapImpl<CellKind::JSWeakSetKind>;

} // namespace vm
} // namespace hermes

#endif
