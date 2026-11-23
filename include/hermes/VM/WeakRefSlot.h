/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_WEAKREFSLOT_H
#define HERMES_VM_WEAKREFSLOT_H

#include "hermes/VM/CompressedPointer.h"
#include "hermes/VM/GCCell.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/RootAcceptor.h"
#include "hermes/VM/WeakRoot.h"

#include <new>

namespace hermes {
namespace vm {

/// This is a single slot in the weak reference table. It manages a
/// WeakSmallHermesValue that can weakly hold Pointer (i.e., String, Object,
/// BoxedDouble, BigInt) and Symbol values, or any other primitive values that a
/// SmallHermesValue can hold (e.g., Bool, small integer, etc.). The GC will
/// make sure it is updated when the object is moved; if the object/symbol is
/// garbage-collected, it will be invalidated.
class WeakRefSlot {
 public:
  // Mutator methods.

  /// Return true if this slot stores a valid Object pointer or Symbol.
  bool hasValue() const {
    assert(!isFree() && "Should never query a free WeakRef");
    return !!value_.root;
  }

  bool isObject() const {
    return value_.root.isObject();
  }
  bool isSymbol() const {
    return value_.root.isSymbol();
  }

  /// Return the underlying value, with a read barrier if it's a pointer or
  /// symbol.
  inline HermesValue getValue(PointerBase &base, GC &gc) const;

  /// Return the underlying value, without a read barrier. This should only be
  /// used in cases where it is known that no read barrier is necessary (e.g.,
  /// in the GC itself).
  inline HermesValue getValueNoBarrierUnsafe(PointerBase &base) const;

  /// Return the object as a GCCell *, with a read barrier
  inline GCCell *getObject(PointerBase &base, GC &gc) const;

  /// Same as getObject, but without a read barrier
  GCCell *getObjectNoBarrierUnsafe(PointerBase &base) const {
    // Cannot check state() here because it can race with marking code.
    assert(hasValue() && "tried to access collected referent");
    return value_.root.getObjectNoBarrierUnsafe(base);
  }
  CompressedPointer getObjectNoBarrierUnsafe() const {
    assert(hasValue() && "tried to access collected referent");
    return value_.root.getObjectNoBarrierUnsafe();
  }

  /// Return the SymbolID, with a read barrier.
  inline SymbolID getSymbol(GC &gc) const;

  /// Return the symbol, without a read barrier.
  SymbolID getSymbolNoBarrierUnsafe() const {
    return value_.root.getSymbolNoBarrierUnsafe();
  }

  void markWeakRoots(WeakRootAcceptor &acceptor) {
    assert(!isFree() && "Cannot mark the weak root of a freed slot.");
    acceptor.acceptWeak(value_.root);
  }

  // GC methods to update slot when referent moves/dies.

#ifdef UNIT_TEST
  /// Update the stored pointer. Only for tests: once a slot is allocated, only
  /// the GC changes its referent, through markWeakRoots(). This must not make
  /// a slot of GCBase::weakSlots_ point into the YG, since YG collections may
  /// not visit it (see GCBase::weakSlots_).
  inline void setPointerForTest(CompressedPointer ptr);
#endif

  /// Clear the pointer (because the object died).
  void clearPointer() {
    value_.root.invalidate();
  }

  /// GC methods to recycle slots.

  /// Set the slot to free, this can be called from either the mutator (when
  /// destroying a WeakRef) or the background thread (in finalizer).
  void free() {
    /// There are three operations related to this atomic variable:
    /// 1. Freeing the slot on background thread.
    /// 2. Checking if the slot is free on the mutator.
    /// 3. Freeing and reusing the slot on the mutator.
    /// Since the only operation that background thread can do with the slot is
    /// freeing it, we don't need a barrier to order the store.
    free_.store(true, std::memory_order_relaxed);
  }

  /// Methods required by ManagedChunkedList.

  WeakRefSlot() = default;

  /// \return true if the slot has been freed. As noted in free(), since
  /// background thread can only free a slot, we don't need a stricter order.
  bool isFree() const {
    return free_.load(std::memory_order_relaxed);
  }

  WeakRefSlot *getNextFree() {
    assert(isFree() && "Can only get nextFree on a free slot");
    return value_.nextFree;
  }

  void setNextFree(WeakRefSlot *nextFree) {
    assert(isFree() && "can only set nextFree on a free slot");
    value_.nextFree = nextFree;
  }

  /// Emplace new value to this slot.
  void emplace(SmallHermesValue target) {
    assert(isFree() && "Slot must be free.");
    // value_.root is inactive on a freed slot (nextFree is set), so calling
    // `set()` on it is an UB. Use placement new instead.
    ::new (&value_.root) WeakSmallHermesValue(target);
    free_.store(false, std::memory_order_relaxed);
  }

 private:
  /// When the slot is not free, root points to the referenced object, and is
  /// updated by GC when it moves/dies (which happens either on the mutator or
  /// background thread at the STW phase). When it's freed, nextFree is set to
  /// another freed slot and added to a freelist for reuse (currently this is
  /// delegated to ManagedChunkedList).
  union WeakRootOrIndex {
    WeakSmallHermesValue root;
    WeakRefSlot *nextFree;
    WeakRootOrIndex() {}
  } value_;
  /// Atomic state represents whether the slot is free. It can be written by
  /// background thread (in finalizer) and read/written by mutator.
  std::atomic<bool> free_{true};
};

class JSObject;
class JSWeakMapImplBase;

/// Slot used by each entry in a WeakMap or WeakSet. The mapped value is stored
/// here so that GC can directly mark it if both its key and owner are alive,
/// eliminating the complexity of managing a separate values array. And the
/// WeakRoot to the owner is needed to correctly collect cycles, e.g.,
/// ```
/// var m = WeakMap();
/// var o = {};
/// m.set(o, m);
/// m = undefined;
/// ```
/// Note that all these fields must only be directly accessed by the GC, or
/// through the corresponding WeakMapEntryRef.
class WeakMapEntrySlot {
  /// The state of the slot.
  std::atomic_bool freed_{true};

 public:
  /// The owner (WeakMap/WeakSet) of this entry.
  WeakRoot<GCCell> owner;
  /// The WeakRoot to the key (or next free slot).
  union {
    WeakSmallHermesValue key;
    WeakMapEntrySlot *nextFree;
  };
  /// The value mapped by the WeakRef key.
  /// NOTE: It's set to Empty only if either the key or the owner is null.
  /// Once the slot is allocated, the mutator must only set it with
  /// GCBase::setWeakMapEntryValue.
  PinnedHermesValue mappedValue;

  void markWeakRoots(WeakRootAcceptor &acceptor) {
    acceptor.acceptWeak(key);
    acceptor.acceptWeak(owner);
  }

  void free() {
    freed_.store(true, std::memory_order_relaxed);
  }

  /// Methods required by ManagedChunkedList

  WeakMapEntrySlot() {}

  bool isFree() const {
    return freed_.load(std::memory_order_relaxed);
  }

  WeakMapEntrySlot *getNextFree() const {
    assert(isFree() && "Can only get nextFree on a free slot");
    return nextFree;
  }

  void setNextFree(WeakMapEntrySlot *nextFree) {
    assert(isFree() && "can only set nextFree on a free slot");
    this->nextFree = nextFree;
  }

  /// Emplace new value to this slot.
  void emplace(
      SmallHermesValue keyPtr,
      HermesValue value,
      CompressedPointer ownerPtr) {
    assert(isFree() && "Slot must be free");
    // key is inactive on a freed slot (nextFree is set), so calling `set()` on
    // it is an UB. Use placement new instead.
    ::new (&key) WeakSmallHermesValue(keyPtr);
    mappedValue = value;
    owner = ownerPtr;
    freed_.store(false, std::memory_order_relaxed);
  }
};

} // namespace vm
} // namespace hermes

#endif
