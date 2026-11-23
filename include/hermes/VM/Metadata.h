/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_METADATA_H
#define HERMES_VM_METADATA_H

#include "hermes/Support/OptValue.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/HermesValue.h"

#include <cassert>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace hermes {
namespace vm {

// Forward declare to avoid pulling in a runtime dependency.
class GCSymbolID;

/// Metadata is a the information about a class's pointers locations.
/// This is used by the Garbage collector to know where potential pointers
/// to other objects are.
struct Metadata final {
  using offset_t = std::uint8_t;

  /// The information about an array for an object.
  struct ArrayData {
    /// Which type of element the array holds.
    enum class ArrayType : uint8_t {
#define SLOT_TYPE(type) type,
#include "hermes/VM/SlotKinds.def"
#undef SLOT_TYPE
    };
    ArrayType type;
    /// The offset of where the array starts.
    offset_t startOffset;
    /// The offset of the field that knows how many elements are in the array.
    offset_t lengthOffset;
    /// The width of each element. For example, a pointer has a stride of
    /// sizeof(void *).
    uint8_t stride;

    explicit ArrayData() = default;
    explicit constexpr ArrayData(
        ArrayType type,
        offset_t startOffset,
        offset_t lengthOffset,
        uint8_t stride)
        : type(type),
          startOffset(startOffset),
          lengthOffset(lengthOffset),
          stride(stride) {}
    ArrayData(const ArrayData &data) = default;
    ArrayData &operator=(const ArrayData &data) = default;
  };

  /// A Builder is a way to build a metadata structure by incrementally
  /// adding fields to it.
  /// After adding the fields, call \c build in order to create the \c Metadata
  /// object.
  class Builder final {
   public:
    /// Creates a Builder for an object, from a base to be used to
    /// calculate the offsets of fields from the base.
    Builder(const void *base);

    /// @name Field adders
    /// @{
    /// Adds a field to the metadata.
    /// The version without a \p name parameter means that field will not appear
    /// in snapshots.

    /// Add a field for a certain slot type at the given \p fieldLocation.
#define SLOT_TYPE(type)                     \
  void addField(const type *fieldLocation); \
  void addField(const char *name, const type *fieldLocation);
#include "hermes/VM/SlotKinds.def"
#undef SLOT_TYPE

    /// @}

    /// Adds an array to this class's metadata.
    /// \p startLocation The location of the first element in the array.
    /// \p lengthLocation The location of the size of the array.
#define SLOT_TYPE(type)                                     \
  void addArray(                                            \
      const type *startLocation,                            \
      const AtomicIfConcurrentGC<uint32_t> *lengthLocation, \
      std::size_t stride) {                                 \
    addArray(                                               \
        nullptr,                                            \
        ArrayData::ArrayType::type,                         \
        startLocation,                                      \
        lengthLocation,                                     \
        stride);                                            \
  }                                                         \
                                                            \
  void addArray(                                            \
      const char *name,                                     \
      const type *startLocation,                            \
      const AtomicIfConcurrentGC<uint32_t> *lengthLocation, \
      std::size_t stride) {                                 \
    addArray(                                               \
        name,                                               \
        ArrayData::ArrayType::type,                         \
        startLocation,                                      \
        lengthLocation,                                     \
        stride);                                            \
  }
#include "hermes/VM/SlotKinds.def"
#undef SLOT_TYPE

    void addArray(
        const char *name,
        ArrayData::ArrayType type,
        const void *startLocation,
        const AtomicIfConcurrentGC<uint32_t> *lengthLocation,
        std::size_t stride);

    /// Should be called first when building metadata for a JSObject subclass.
    /// Records how many property slots are unused due to overlap.
    void addJSObjectOverlapSlots(unsigned num) {
      if (!jsObjectOverlapSlots_) {
        jsObjectOverlapSlots_ = num;
      } else {
        // Subsequent calls do nothing but assert that order of calls is sane.
        assert(
            num <= *jsObjectOverlapSlots_ &&
            "most derived class should call this method first");
      }
    }

    /// The number of initial direct property slots to exclude in the metadata
    /// for a JSObject subclass.
    unsigned getJSObjectOverlapSlots() const {
      assert(
          jsObjectOverlapSlots_ && "missing call to addJSObjectOverlapSlots");
      return *jsObjectOverlapSlots_;
    }

    void setVTable(const VTable *vtp) {
      vtp_ = vtp;
    }

    void setJitCall(ObjectJitCallPtr jitCall) {
      jitCall_ = jitCall;
    }

    /// Build creates a Metadata, and destroys this builder.
    Metadata build();

   private:
    /// Calculate the offset of a field at \p fieldLocation relative to base_.
    offset_t getOffset(const void *fieldLocation);

    /// The base of the object, used to calculate offsets.
    const char *base_;
    /// A list of offsets and within the object to its field type and name.
#define SLOT_TYPE(type) std::map<offset_t, const char *> map##type##_;
#include "hermes/VM/SlotKinds.def"
#undef SLOT_TYPE

#ifndef NDEBUG
    /// True if [offset, offset + size) overlaps any previously added field.
    bool fieldConflicts(offset_t offset, size_t size);
    std::vector<bool> coveredOffsets_;
#endif
    /// An optional array for an object to contain.
    OptValue<ArrayData> array_;

    /// For subclasses of JSObject, the number of unused direct property slots.
    OptValue<unsigned> jsObjectOverlapSlots_;

    /// The VTable pointer for the cell that this metadata describes.
    const VTable *vtp_{nullptr};

    /// The jitCall pointer for the cell that this metadata describes. This is
    /// only non-null for JSObject subtypes.
    ObjectJitCallPtr jitCall_{nullptr};

    friend Metadata;
  };

  constexpr Metadata() = default;
  /// Construct from a builder.
  Metadata(Builder &&mb);

  /// The maximum number of offsets we can store for a cell. Bump this if
  /// asserts start failing and more fields are needed.
  static constexpr size_t kMaxNumFields = 10;

  struct SlotOffsets {
    /// The offset of each field for a given cell type is stored contiguously in
    /// fields. It is grouped by the slot type, and slots for a given type are
    /// in ascending order.

    /// Record one past the last value in offsets that corresponds to a given
    /// slot type.
#define SLOT_TYPE(type) uint8_t end##type{};
#include "hermes/VM/SlotKinds.def"
#undef SLOT_TYPE

    std::array<offset_t, kMaxNumFields> fields{};

    /// The optional array for this object to hold.
    /// NOTE: this format currently does not support multiple arrays.
    OptValue<ArrayData> array{};
  } offsets;

  /// Whether any field or the array can hold a pointer to a GCCell, that is,
  /// whether the cell has slots other than GCSymbolIDs. Set by
  /// buildMetadataTable. It is true until then, which is always safe to assume.
  bool hasPointerSlots{true};

  using SlotNames = std::array<const char *, kMaxNumFields>;

  /// The names of the fields, only used in snapshots. This is placed after the
  /// ArrayData so that all the hot fields above are adjacent in memory.
  SlotNames names{};

  /// The VTable pointer for the cell that this metadata describes.
  const VTable *vtp{};

  /// The jitCall function for this cell.
  ObjectJitCallPtr jitCall{};

  /// Static array storing the Metadata corresponding to each CellKind. This is
  /// initialized by buildMetadataTable.
  static std::array<Metadata, kNumCellKinds> metadataTable;
};

static_assert(
    std::is_trivially_destructible<Metadata>::value,
    "Metadata must not have a destructor.");

/// @name Formatters
/// @{

llvh::raw_ostream &operator<<(llvh::raw_ostream &os, const Metadata &meta);
llvh::raw_ostream &operator<<(llvh::raw_ostream &os, Metadata::ArrayData array);
llvh::raw_ostream &operator<<(
    llvh::raw_ostream &os,
    Metadata::ArrayData::ArrayType arraytype);

/// @}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_METADATA_H
