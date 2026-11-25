/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_DICTPROPERTYMAP_H
#define HERMES_VM_DICTPROPERTYMAP_H

#include "hermes/VM/IdentifierTable.h"
#include "hermes/VM/PropertyDescriptor.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/SymbolID.h"

#include "llvh/Support/TrailingObjects.h"

namespace hermes {
namespace vm {

/// DictPropertyMap provides simple property metadata storage for JavaScript
/// objects. It maps from SymbolID to PropertyDescriptor and provides
/// iteration in insertion order.
///
/// The object contains two data structures:
/// - an open addressing hash table mapping from SymbolID to an integer index.
/// - a descriptor array containing pairs of SymbolID and PropertyDescriptor.
/// The layout in memory is actually the opposite order (because it simplifies
/// iteration over the symbols).
///
/// Fast property lookup is supported by the hash table - it conceptually maps
/// from a SymbolID to an index in the descriptor array. To save memory, only
/// part of the SymbolID is stored in the hash table itself, so the SymbolID
/// in the descriptor array entry is also checked on a match.
///
/// New properties are inserted in the hash table and appended sequentially to
/// the end of the descriptor array, thus encoding the original insertion order.
///
/// Deleted properties are removed from the hash table and marked by a "deleted"
/// SymbolID in the descriptor array. Then the desctiptor is added to a list of
/// deleted property slots - PropertyDescriptor::flags is overloaded to serve as
/// the "next deleted" link. We remove an entry from the "deleted" list when we
/// need to allocate a slot for a new property - then finally the entry is
/// marked with an "invalid" SymbolID.
///
/// Iteration simply walks the descriptor array from start to end, skipping
/// deleted and invalid properties, preserving the original insertion order.
///
/// The object has to be reallocated when any of these conditions occur:
/// - the descriptor array is full (it never shrinks, even after deletions)
/// - the hash table occupancy is above a certain threshold (note that deletions
///   don't decrease the hash table occupancy).
///
/// Reallocation first scans the descriptor array and inserts valid (and not
/// deleted) properties in the new hash table and descriptor array. We must also
/// preserve the list of deleted properties, so then it walks the deleted list
/// and appends the descriptors to the new desctiptor array.
///
/// A property descriptor is always in one of these states:
///  - "uninitialized". It is beyond \c numDescriptors.
///  - "valid". It contains a valid SymbolID and descriptor.
///  - "deleted". It contains ReservedSymbolId::deleted and is part of the
///  "deleted" list.
///  - "invalid". It contains SymbolID::empty(). It used to be "deleted"
///  but its slot was re-used by a new property.
///
namespace detail {

/// A valid entry in the hash table holds an index into the descriptor array
/// and part of the SymbolID (for filtering). Entries transition as follows:
/// empty -> valid -> deleted -> valid -> ...
class DPMHashPair {
 public:
  DPMHashPair() : idpart_(0), desc_(0) {}
  bool isEmpty() const {
    return desc_ == EMPTY;
  }
  bool isDeleted() const {
    return desc_ == DELETED;
  }
  bool isValid() const {
    return desc_ >= FIRST_VALID;
  }
  uint32_t getDescIndex() const {
    assert(isValid() && "asked for descriptor of invalid pair");
    return desc_ - FIRST_VALID;
  }
  /// Returns false if this entry does not match \p id.
  bool mayBe(SymbolID id) const {
    assert(isValid() && "tried to match invalid pair");
    return idpart_ == (id.unsafeGetRaw() & ID_MASK);
  }
  /// (Re)initialize an empty or deleted hash table entry.
  /// Returns true iff the previous state was deleted.
  bool setDescIndex(uint32_t idx, SymbolID id) {
    assert(!isValid() && "overwriting a valid entry");
    assert(canStore(idx) && "impossibly large descriptor index");
    bool ret = isDeleted();
    desc_ = idx + FIRST_VALID;
    idpart_ = (id.unsafeGetRaw() & ID_MASK);
    assert(isValid() && "failed to make a valid entry");
    return ret;
  }
  /// Mark a valid hash table position as deleted.
  /// Returns the descriptor index it held.
  uint32_t setDeleted() {
    assert(isValid() && "tried to delete an empty/deleted entry");
    uint32_t ret = getDescIndex();
    desc_ = DELETED;
    return ret;
  }
  /// Returns true if idx is small enough to be stored as a descriptor index
  /// in this class.
  static constexpr bool canStore(uint32_t idx) {
    return idx <= maxDescIndex();
  }

  /// The maximum index that can be hold.
  static constexpr uint32_t maxDescIndex() {
    return (1 << DESC_BITS) - FIRST_VALID - 1;
  }

 private:
  /// Encoding of desc_. Empty is 0 so that zeroed memory means all empty.
  enum { EMPTY = 0, DELETED, FIRST_VALID };

  /// Number of bits of SymbolID to store. A static_assert checks that
  /// the max possible descriptor index can be stored in the other bits.
  /// Could be slightly higher, but single byte is efficient to access.
  static constexpr size_t ID_BITS = 8;
  static constexpr size_t ID_MASK = (1 << ID_BITS) - 1;

  /// Bits that can hold (max possible descriptor index + FIRST_VALID).
  static constexpr size_t DESC_BITS = 32 - ID_BITS;

  struct {
    /// Part of a SymbolID.
    uint32_t idpart_ : ID_BITS;
    /// Encoded descriptor index (or EMPTY/DELETED).
    uint32_t desc_ : DESC_BITS;
  };
};

} // namespace detail

class DictPropertyMap final
    : public VariableSizeRuntimeCell,
      private llvh::TrailingObjects<
          DictPropertyMap,
          std::pair<GCSymbolID, NamedPropertyDescriptor>,
          detail::DPMHashPair> {
  friend TrailingObjects;
  friend void DictPropertyMapBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);

 public:
  using size_type = uint32_t;

  using DescriptorPair = std::pair<GCSymbolID, NamedPropertyDescriptor>;

  static const size_type DEFAULT_CAPACITY = 2;

  /// An opaque class representing a reference to a valid property in the
  /// property map.
  class PropertyPos {
    friend class DictPropertyMap;
    size_type hashPairIndex;

    PropertyPos(size_type hashPairIndex) : hashPairIndex(hashPairIndex) {}
    /// Default constructor so this class can be used in OptValue.
    PropertyPos() = default;
    friend class OptValue<PropertyPos>;
  };

  static const VTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::DictPropertyMapKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::DictPropertyMapKind;
  }

  /// Return the maximum possible capacity of DictPropertyMap.
  static constexpr DictPropertyMap::size_type getMaxCapacity() {
    return hermes::vm::detail::DPMHashPair::maxDescIndex();
  }

  /// Create an instance of DictPropertyMap with the specified capacity.
  static CallResult<PseudoHandle<DictPropertyMap>> create(
      Runtime &runtime,
      size_type capacity = DEFAULT_CAPACITY);

  /// Return the number of non-deleted properties in the map.
  size_type size() const {
    return numProperties_;
  }

  /// Call the supplied callback pass each property's \c SymbolID and \c
  /// NamedPropertyDescriptor as parameters.
  /// Obviously the callback shouldn't be doing naughty things like modifying
  /// the property map or creating new hidden classes (even implicitly).
  template <typename CallbackFunction>
  static void forEachProperty(
      Handle<DictPropertyMap> selfHandle,
      Runtime &runtime,
      const CallbackFunction &callback);

  /// Same as \p forEachProperty, but the callback cannot do anything that
  /// affects heap state such as allocate objects or handles.
  /// If your callback doesn't need to do any allocations or modify heap state
  /// this is slightly faster than \p forEachProperty.
  template <typename CallbackFunction>
  static void forEachPropertyNoAlloc(
      DictPropertyMap *self,
      const CallbackFunction &callback);

  /// Same as forEachProperty() but the callback returns true to continue or
  /// false to stop immediately.
  /// \return false if the callback returned false, true otherwise.
  template <typename CallbackFunction>
  static bool forEachPropertyWhile(
      Handle<DictPropertyMap> selfHandle,
      Runtime &runtime,
      const CallbackFunction &callback);

  /// Iterate over all the valid properties in the map, supplying a reference of
  /// the property descriptor to the \p callback. The callback is allowed to
  /// mutate the property descriptor.
  template <typename CallbackFunction>
  static void forEachMutablePropertyDescriptor(
      Handle<DictPropertyMap> selfHandle,
      Runtime &runtime,
      const CallbackFunction &callback);

  static DescriptorPair *getDescriptorPair(
      DictPropertyMap *self,
      PropertyPos pos);

  /// Find a property by \p id. On success return a reference to the found
  /// property.
  static OptValue<PropertyPos> find(const DictPropertyMap *self, SymbolID id);

  /// Find a property, or add it (with a random PropertyDescriptor) if it
  /// doesn't exist.
  /// \return a pair consisting of pointer to the property descriptor and a pool
  ///   denoting whether a new property was added.
  static CallResult<std::pair<NamedPropertyDescriptor *, bool>> findOrAdd(
      MutableHandle<DictPropertyMap> selfHandleRef,
      Runtime &runtime,
      SymbolID id);

  /// Add a new property with id \p id and descriptor \p desc, which must not
  /// already exist. This method may cause reallocation, in which case the new
  /// address will be updated in \p selfHandleRef.
  /// \p selfHandleRef pointer to the self handle, which may be updated if the
  ///     object is re-allocated.
  static ExecutionStatus add(
      MutableHandle<DictPropertyMap> selfHandleRef,
      Runtime &runtime,
      SymbolID id,
      NamedPropertyDescriptor desc);

  /// Remove the property at the specified position. This invalidates all
  /// positions.
  static void erase(DictPropertyMap *self, Runtime &runtime, PropertyPos pos);

  /// Allocate a new property slot. Either pop the first entry in the deleted
  /// list, or, if the deleted list is empty, return slot \c numProperties_,
  /// which is the next slot at the end of the currently allocated storage.
  static SlotIndex allocatePropertySlot(
      DictPropertyMap *self,
      Runtime &runtime);

  void dump();

 private:
  using HashPair = detail::DPMHashPair;

  /// Total size of the descriptor array.
  const size_type descriptorCapacity_;
  /// Total size of the hash table. It will always be a power of 2.
  const size_type hashCapacity_;

  /// How many entries have been added to the descriptor array (including
  /// deleted).
  AtomicIfConcurrentGC<size_type> numDescriptors_{0};

  /// Number of valid properties in the map.
  size_type numProperties_{0};

  /// A constant used to signify end of deleted list.
  static constexpr size_type END_OF_LIST =
      std::numeric_limits<size_type>::max();

  // Ensure that we can overload NamedPropertyDescriptor::_altFlags to store
  // the next deleted index.
  static_assert(
      std::is_same<decltype(NamedPropertyDescriptor::_altFlags), size_type>::
          value,
      "size_type must correspond to NamedPropertyDescriptor::_altFlags");

  /// Index of the most recently deleted NamedPropertyDescriptor. Its
  /// _altFlags field contains the index of the next deleted and so on.
  size_type deletedListHead_{END_OF_LIST};

  /// Number of entries in the deleted list.
  size_type deletedListSize_{0};

  /// Derive the size of the hash table so it can hold \p cap elements without
  /// many collisions. The result must also be a power of 2.
  static size_type calcHashCapacity(size_type cap) {
    assert(
        (cap <= std::numeric_limits<size_type>::max() / 4) &&
        "size will cause integer overflow in calcHashCapacity");

    return llvh::PowerOf2Ceil(cap * 4 / 3 + 1);
  }

  /// A const-expr version of \c calcHashCapacity() using 64-bit arithmetic.
  /// NOTE: it must not be used at runtime since it might be slow.
  static constexpr uint64_t constCalcHashCapacity64(uint64_t cap) {
    return constPowerOf2Ceil(cap * 4 / 3 + 1);
  }

  /// A constexpr compatible version of llvh::PowerOf2Ceil().
  /// NOTE: it must not be used at runtime since it might be slow.
  static constexpr uint64_t constPowerOf2Ceil(uint64_t A, uint64_t ceil = 1) {
    return ceil >= A ? ceil : constPowerOf2Ceil(A, ceil << 1);
  }

  /// Hash a symbol ID. For now it is the identity hash.
  static unsigned hash(SymbolID symbolID) {
    return symbolID.unsafeGetRaw();
  }

 public:
  DictPropertyMap(size_type descriptorCapacity, size_type hashCapacity)
      : descriptorCapacity_(descriptorCapacity), hashCapacity_(hashCapacity) {
    // Clear the hash table.
    std::fill_n(getHashPairs(), hashCapacity_, HashPair{});
  }

 private:
  DescriptorPair *getDescriptorPairs() {
    return getTrailingObjects<DescriptorPair>();
  }
  const DescriptorPair *getDescriptorPairs() const {
    return getTrailingObjects<DescriptorPair>();
  }
  HashPair *getHashPairs() {
    return getTrailingObjects<HashPair>();
  }

  /// Store the next deleted index in a deleted descriptor pair. The index
  /// is stored in the PropertyFlags field.
  static void setNextDeletedIndex(
      DescriptorPair *descPair,
      size_type nextIndex) {
    assert(
        descPair->first == SymbolID::deleted() &&
        "Descriptor pair is not deleted");
    descPair->second._altFlags = nextIndex;
  }

  /// Obtain the previous deleted index from a deleted descriptor pair. The
  /// index is kept in the PropertyFlags field.
  static size_type getNextDeletedIndex(const DescriptorPair *descPair) {
    assert(
        descPair->first == SymbolID::deleted() &&
        "Descriptor pair is not deleted");
    return descPair->second._altFlags;
  }

  /// A placeholder function to keep track of the number of deleted entries
  /// in the hash table. We don't actually need to do it for now.
  LLVM_ATTRIBUTE_ALWAYS_INLINE void incDeletedHashCount() {}
  /// A placeholder function to keep track of the number of deleted entries
  /// in the hash table. We don't actually need to do it for now.
  LLVM_ATTRIBUTE_ALWAYS_INLINE void decDeletedHashCount() {}

  /// Search the hash table for \p symbolID. If found, return true and the
  /// and a pointer to the hash pair. If not found, return false and a pointer
  /// to the hash pair where it ought to be inserted.
  static inline std::pair<bool, HashPair *> lookupEntryFor(
      DictPropertyMap *self,
      SymbolID symbolID);

  /// Given a valid HashPair, return whether it's an entry for the given ID.
  bool isMatch(const HashPair *entry, SymbolID symbolID) const {
    return entry->mayBe(symbolID) &&
        getDescriptorPairs()[entry->getDescIndex()].first == symbolID;
  }

  /// Allocate a new property map with the specified capacity, copy the existing
  /// valid entries into it. If the specified capacity exceeds the maximum
  /// possible capacity, an exception will be raised.
  /// \param[in,out] selfHandleRef the original object handle on input, the new
  ///   object handle on output.
  /// \param newCapacity the capacity of the new object's descriptor array.
  static ExecutionStatus grow(
      MutableHandle<DictPropertyMap> selfHandleRef,
      Runtime &runtime,
      size_type newCapacity);

  /// Gets the amount of memory required by this object for a given capacity.
  static size_type allocationSize(
      size_type descriptorCapacity,
      size_type hashCapacity) {
    assert(
        descriptorCapacity <= getMaxCapacity() &&
        "descriptorCapacity exceeds the maximum allowed.");
    assert(
        hashCapacity <= calcHashCapacity(getMaxCapacity()) &&
        "hashCapacity exceeds the maximum allowed.");
    return totalSizeToAlloc<DescriptorPair, HashPair>(
        descriptorCapacity, hashCapacity);
  }

  /// Calculate the maximum allocation size of DictPropertyMap at compile time
  /// using approximated padding and getMaxCapacity().
  /// @{

  /// The maximum alignment padding a compiler might insert before a field or at
  /// the end of a struct. We use this for a conservative (but reasonable)
  /// estimate of the allocation size of the object.
  static constexpr uint64_t kAlignPadding = alignof(std::max_align_t) - 1;

  /// Calculate a conservative approximate size of DictPropertyMap in bytes,
  /// given a capacity. The calculation is performed using 64-bit arithmetic to
  /// avoid overflow.
  /// NOTE: it must not be used at runtime since it might be slow.
  static constexpr uint64_t constApproxAllocSize64(size_type cap) {
    static_assert(
        alignof(DictPropertyMap) <= kAlignPadding + 1,
        "DictPropertyMap exceeds supported alignment");
    static_assert(
        alignof(DictPropertyMap::DescriptorPair) <= kAlignPadding + 1,
        "DictPropertyMap::DescriptorPair exceeds supported alignment");
    static_assert(
        alignof(DictPropertyMap::HashPair) <= kAlignPadding + 1,
        "DictPropertyMap::HashPair exceeds supported alignment");

    return sizeof(DictPropertyMap) + kAlignPadding +
        sizeof(DictPropertyMap::DescriptorPair) * (uint64_t)cap +
        kAlignPadding +
        sizeof(DictPropertyMap::HashPair) * constCalcHashCapacity64(cap) +
        kAlignPadding;
  }

  /// A place to put things in order to avoid restrictions on using constexpr
  /// functions declared in the same class.
  struct detail;
  friend struct detail;

  /// @}

 protected:
  size_t numTrailingObjects(OverloadToken<DescriptorPair>) const {
    return descriptorCapacity_;
  }
};

//===----------------------------------------------------------------------===//
// DictPropertyMap inline methods.

template <typename CallbackFunction>
void DictPropertyMap::forEachProperty(
    Handle<DictPropertyMap> selfHandle,
    Runtime &runtime,
    const CallbackFunction &callback) {
  // Re-entering JS execution in this context can cause the property
  // descriptors to change (e.g. T138148986)
  NoRJSScope scope{runtime};
  for (size_type
           i = 0,
           e = selfHandle->numDescriptors_.load(std::memory_order_relaxed);
       i != e;
       ++i) {
    auto const *descPair = selfHandle->getDescriptorPairs() + i;
    if (descPair->first.isValid()) {
      callback(descPair->first, descPair->second);
    }
  }
}

template <typename CallbackFunction>
void DictPropertyMap::forEachPropertyNoAlloc(
    DictPropertyMap *self,
    const CallbackFunction &callback) {
  for (size_type i = 0,
                 e = self->numDescriptors_.load(std::memory_order_relaxed);
       i != e;
       ++i) {
    auto const *descPair = self->getDescriptorPairs() + i;
    if (descPair->first.isValid()) {
      callback(descPair->first, descPair->second);
    }
  }
}

template <typename CallbackFunction>
bool DictPropertyMap::forEachPropertyWhile(
    Handle<DictPropertyMap> selfHandle,
    Runtime &runtime,
    const CallbackFunction &callback) {
  // Re-entering JS execution in this context can cause the property
  // descriptors to change (e.g. T138148986)
  NoRJSScope scope{runtime};
  for (size_type
           i = 0,
           e = selfHandle->numDescriptors_.load(std::memory_order_relaxed);
       i != e;
       ++i) {
    auto const *descPair = selfHandle->getDescriptorPairs() + i;
    if (descPair->first.isValid()) {
      if (!callback(runtime, descPair->first, descPair->second))
        return false;
    }
  }
  return true;
}

template <typename CallbackFunction>
void DictPropertyMap::forEachMutablePropertyDescriptor(
    Handle<DictPropertyMap> selfHandle,
    Runtime &runtime,
    const CallbackFunction &callback) {
  for (size_type
           i = 0,
           e = selfHandle->numDescriptors_.load(std::memory_order_relaxed);
       i != e;
       ++i) {
    auto *descPair = selfHandle->getDescriptorPairs() + i;
    if (descPair->first.isValid()) {
      callback(descPair->second);
    }
  }
}

inline DictPropertyMap::DescriptorPair *DictPropertyMap::getDescriptorPair(
    DictPropertyMap *self,
    PropertyPos pos) {
  assert(
      pos.hashPairIndex < self->hashCapacity_ && "property pos out of range");

  auto *hashPair = self->getHashPairs() + pos.hashPairIndex;
  auto descIndex = hashPair->getDescIndex();
  assert(
      descIndex < self->numDescriptors_.load(std::memory_order_relaxed) &&
      "descriptor index out of range");

  auto *res = self->getDescriptorPairs() + descIndex;
  assert(hashPair->mayBe(res->first) && "accessing incorrect descriptor pair");
  return res;
}

inline std::pair<bool, DictPropertyMap::HashPair *>
DictPropertyMap::lookupEntryFor(DictPropertyMap *self, SymbolID symbolID) {
  size_type const mask = self->hashCapacity_ - 1;
  size_type index = hash(symbolID) & mask;

  // Probing step.
  size_type step = 1;
  // Save the address of the start of the table to avoid recalculating it.
  HashPair *const tableStart = self->getHashPairs();
  // The first deleted entry we found.
  HashPair *deleted = nullptr;

  assert(symbolID.isValid() && "looking for an invalid SymbolID");

  for (;;) {
    HashPair *curEntry = tableStart + index;

    if (curEntry->isValid()) {
      if (self->isMatch(curEntry, symbolID))
        return {true, curEntry};
    } else if (curEntry->isEmpty()) {
      // If we encountered an empty pair, the search is over - we failed.
      // Return either this entry or a deleted one, if we encountered one.

      return {false, deleted ? deleted : curEntry};
    } else {
      assert(curEntry->isDeleted() && "unexpected HashPair state");
      // The first time we encounter a deleted entry, record it so we can
      // potentially reuse it for insertion.
      if (!deleted)
        deleted = curEntry;
    }

    index = (index + step) & mask;
    ++step;
  }
}

inline OptValue<DictPropertyMap::PropertyPos> DictPropertyMap::find(
    const DictPropertyMap *self,
    SymbolID id) {
  // We want to keep the public interface of find() clean, so it accepts a
  // const self pointer, but internally we don't want to duplicate the code
  // only to propagate the "constness". So we cast the const away and promise
  // not to mutate it.
  auto *mutableSelf = const_cast<DictPropertyMap *>(self);
  auto found = lookupEntryFor(mutableSelf, id);
  if (!found.first)
    return llvh::None;
  return PropertyPos{(size_type)(found.second - mutableSelf->getHashPairs())};
}

inline ExecutionStatus DictPropertyMap::add(
    MutableHandle<DictPropertyMap> selfHandleRef,
    Runtime &runtime,
    SymbolID id,
    NamedPropertyDescriptor desc) {
  auto found = findOrAdd(selfHandleRef, runtime, id);
  if (LLVM_UNLIKELY(found == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(found->second && "trying to add an existing property");
  *found->first = desc;
  return ExecutionStatus::RETURNED;
}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_DICTPROPERTYMAP_H
