/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_COMPUTEDPROPERTYCACHE_H
#define HERMES_VM_COMPUTEDPROPERTYCACHE_H

#include "hermes/VM/GCPointer.h"
#include "hermes/VM/PropertyCache.h" // SlotIndex
#include "hermes/VM/SymbolID.h"
#include "hermes/VM/WeakRoot.h"
#include "hermes/VM/sh_mirror.h"

#include <memory>

namespace hermes {

template <typename T>
class OptValue;

namespace vm {

class HiddenClass;
class JSObject;
class Runtime;
struct PropertyFlags;
struct WeakRootAcceptor;

/// A runtime-wide cache for computed puts with a uniqued string key, keyed
/// on (hidden class, property name), so that one probe tells a store to an
/// existing property from an add. A `Slot` entry says that every object
/// with the class has the name as an own data property in the slot; an
/// `Add` entry says that adding the name with the default flags to an
/// object with the class yields the result class and the slot. The table
/// has kNumSets sets of kNumWays entries, held weakly: the GC empties an
/// entry whose class dies or moves.
class ComputedPropertyCache {
 public:
  /// What an entry records.
  enum class Kind : uint8_t {
    /// The class has the name as a data property in the slot.
    Slot,
    /// Adding the name to the class gives resultClazz and the slot.
    Add,
  };

  /// One entry of the table. It has no usable copy assignment, because the
  /// weak roots delete it; use copyFrom() and reset().
  struct Entry {
    /// The class of the object. Null: the entry is empty.
    WeakRoot<HiddenClass> clazz{nullptr};
    /// The name of the property.
    SymbolID name{};
    /// The slot index of the property.
    uint32_t slot : 30;
    /// A Kind.
    uint32_t kind : 1;
    /// Slot only: a plain store to the slot is allowed.
    uint32_t writable : 1;
    /// Add only: the class after the add.
    WeakRoot<HiddenClass> resultClazz{nullptr};
    /// Add only: the full parent cache epoch when the entry was filled.
    uint64_t parentEpoch{0};

    /// Create an empty entry.
    Entry() : slot(0), kind(0), writable(0) {}

    /// \return the kind of the entry.
    Kind getKind() const {
      return static_cast<Kind>(kind);
    }

    /// Copy every field of \p other into this entry.
    void copyFrom(const Entry &other);

    /// Empty the entry: both classes null, the name SymbolID{} (as
    /// constructed), every other field zero.
    void reset();
  };

  /// The number of sets in the table.
  static constexpr unsigned kNumSets = 1024;
  /// The number of entries in a set.
  static constexpr unsigned kNumWays = 2;
  /// The largest slot index an entry can hold.
  static constexpr SlotIndex kMaxSlot = (1u << 30) - 1;

  /// Allocate the table, every entry empty.
  ComputedPropertyCache();

  /// \return the index of the set of the key (\p clazz, \p name).
  static unsigned setIndex(CompressedPointer clazz, SymbolID name) {
    return ((uint32_t)(clazz.getRaw() >> 3) * 0x9E3779B1u +
            name.unsafeGetRaw() * 0x85EBCA6Bu) >>
        22;
  }

  /// \return the entry for (\p clazz, \p name), or null if there is none.
  /// Writes nothing. \p clazz is the class of a live object, so it is
  /// never null and never matches an empty entry.
  const Entry *lookup(CompressedPointer clazz, SymbolID name) const {
    assert(clazz && "lookup needs the class of a live object");
    const Entry *set = &table_[setIndex(clazz, name) * kNumWays];
    for (unsigned way = 0; way < kNumWays; ++way) {
      if (set[way].clazz == clazz && set[way].name == name)
        return &set[way];
    }
    return nullptr;
  }

  /// Insert a `Slot` entry for (\p clazz, \p name) in way 0 of its set.
  /// A different key in way 0 moves to way 1; the key never ends up in
  /// both ways.
  /// \pre slot <= kMaxSlot.
  void insertSlot(
      PointerBase &base,
      HiddenClass *clazz,
      SymbolID name,
      SlotIndex slot,
      bool writable);

  /// Insert an `Add` entry for (\p startClazz, \p name) in way 0 of its
  /// set. A different key in way 0 moves to way 1; the key never ends up
  /// in both ways.
  /// \pre slot <= kMaxSlot.
  void insertAdd(
      PointerBase &base,
      HiddenClass *startClazz,
      SymbolID name,
      HiddenClass *resultClazz,
      SlotIndex slot,
      uint64_t parentEpoch);

  /// The probe of the put fast path. \return the entry for (the class of
  /// \p obj, the name of \p key), or null if the key has no name, the
  /// class is not cacheable or there is no entry. Does not allocate.
  inline const Entry *
  probe(Runtime &runtime, const JSObject *obj, HermesValue key) const;

  /// Record that objects with class \p clazz have the property \p name,
  /// with \p flags, in \p slot, if the class is cacheable, the property is
  /// a named data property and the slot fits. Otherwise do nothing. Does
  /// not allocate.
  void recordSlot(
      Runtime &runtime,
      HiddenClass *clazz,
      SymbolID name,
      PropertyFlags flags,
      SlotIndex slot);

  /// The fill after a put to an existing property: if \p clazz is cacheable
  /// and \p key has a name, recordSlot() under that name. The class is
  /// tested first and inline, so that an excluded class costs neither a
  /// call nor the key's name. Does not allocate.
  inline void fillSlot(
      Runtime &runtime,
      HiddenClass *clazz,
      HermesValue key,
      PropertyFlags flags,
      SlotIndex slot);

  /// Accept the classes of every non-empty entry as weak roots, and empty
  /// the entries whose class died or moved, and the `Add` entries whose
  /// result class died.
  void markWeakRoots(WeakRootAcceptor &acceptor);

  /// \return the number of non-empty entries.
  unsigned numEntries() const;

  /// Empty every entry.
  void clear();

  /// \return true if entries may be recorded for objects of \p clazz: it
  /// is not a dictionary, has no index-like named property and is not a
  /// host object, lazy object, proxy or object with indexed storage.
  static inline bool isCacheable(const HiddenClass *clazz);

  /// \return the name of \p key: its SymbolID if it is a uniqued string,
  /// otherwise none.
  static inline OptValue<SymbolID> nameOfKey(Runtime &runtime, HermesValue key);

 private:
  /// \return way 0 of the set of (\p clazz, \p name), ready to be
  /// written: if it holds a different key, that entry moves to way 1; if
  /// it is empty, way 1 is emptied only when it holds this key.
  Entry &prepareWay0(CompressedPointer clazz, SymbolID name);

  /// The table: kNumSets sets of kNumWays consecutive entries.
  std::unique_ptr<Entry[]> table_;
};

static_assert(
    sizeof(ComputedPropertyCache::Entry) <= 32,
    "ComputedPropertyCache::Entry must fit in 32 bytes");

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_COMPUTEDPROPERTYCACHE_H
