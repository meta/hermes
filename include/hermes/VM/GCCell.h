/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_GCCELL_H
#define HERMES_VM_GCCELL_H

#include "hermes/Support/Algorithms.h"
#include "hermes/VM/AlignedHeapSegment.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/CompressedPointer.h"
#include "hermes/VM/HeapAlign.h"
#include "hermes/VM/VTable.h"
#include "hermes/VM/sh_mirror.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace hermes {
namespace vm {

template <typename T>
class ExternalStringPrimitive;
class VariableSizeRuntimeCell;

/// Return the allocation size for a fixed-size cell corresponding to class C.
/// This must be used instead of the builtin sizeof operator, since classes
/// further down the GCCell hierarchy may add (fixed-size) trailing objects and
/// redefine this method.
template <class C>
static constexpr uint32_t cellSize() {
  static_assert(HeapAlign % alignof(C) == 0, "insufficient heap alignment");
  return heapAlignSize(C::template cellSizeImpl<C>());
}

/// This class stores a CellKind and the size of a cell in 32 bits.
class KindAndSize {
  friend struct RuntimeOffsets;
  using RawType = CompressedPointer::RawType;
  static constexpr size_t kNumBits = sizeof(RawType) * 8;
  static constexpr size_t kNumKindBits = 8;
  // On 64 bit platforms without compressed pointers, just make the size 32 bits
  // so that it can be accessed without any masking or shifting.
  static constexpr size_t kNumSizeBits =
      std::min<size_t>(kNumBits - kNumKindBits, 32);
  static_assert(
      sizeof(CellKind) * 8 <= kNumKindBits,
      "CellKind size wider than available kind bits.");

  static_assert(
      sizeof(SHKindAndSize) == sizeof(RawType),
      "type is the wrong size");
  static_assert(SH_KINDANDSIZE_NUM_BITS == kNumBits, "num bits is incorrect");
  static_assert(
      SH_KINDANDSIZE_NUM_KIND_BITS == kNumKindBits,
      "num kind bits is incorrect");
  static_assert(
      SH_KINDANDSIZE_NUM_SIZE_BITS == kNumSizeBits,
      "num size bits is incorrect");

  /// The size of the cell. Due to heap alignment, we are guaranteed that the
  /// least significant bit will always be zero, so it can be used for the
  /// mark bit. In order for that to work, this has to come first.
  /// If this is 0, it means the cell has an actual size larger than
  /// maxNormalSize() (in HadesGC, it must be allocated in a JumboHeapSegment).
  RawType size_ : kNumSizeBits;
  /// The CellKind of the cell.
  RawType kind_ : kNumKindBits;

 public:
  size_t getSize() const {
    return size_;
  }
  CellKind getKind() const {
    return static_cast<CellKind>(kind_);
  }
  const VTable *getVT() const {
    return VTable::vtableArray[kind_];
  }
  KindAndSize() = default;
  KindAndSize(CellKind kind, size_t sz)
      : size_(sz), kind_(static_cast<uint8_t>(kind)) {
    assert((sz & 1) == 0 && "LSB of size must always be zero.");
    assert(sz <= maxSize() && "Size is too large.");
  }

  static constexpr uint32_t maxSize() {
    return (1ULL << kNumSizeBits) - 1;
  }
};

static_assert(
    std::is_trivial<KindAndSize>::value,
    "KindAndSize must be trivial");

/// This include file defines a GCCell that allows forward heap
/// traversal in a contiguous space: given a pointer to the head, you
/// can get the size, and thus get to the head of the next cell.
class GCCell {
  friend struct RuntimeOffsets;

  /// Either contains the CellKind and size of this cell, or a forwarding
  /// pointer.
  union {
    KindAndSize kindAndSize_;
    AssignableCompressedPointer forwardingPointer_;
  };

#ifndef NDEBUG
  static constexpr uint16_t kMagic{0xce11};
  // This is used to ensure a cell is valid and is not some other value.
  const uint16_t magic_{kMagic};
  /// Semi-unique (until the global counter overflows) id associated with every
  /// allocation.
  uint32_t _debugAllocationId_;
#endif

 public:
  GCCell() = default;

  // GCCell-s are not copyable (in the C++ sense).
  GCCell(const GCCell &) = delete;
  void operator=(const GCCell &) = delete;

  /// Return the allocated size of the object in bytes. This is fast but does
  /// not work for GCCell with size larger than maxNormalSize(), for which we
  /// set size_ to 0. For that case, getAllocatedSizeSlow() should be used
  /// instead.
  uint32_t getAllocatedSize() const {
    uint32_t sz = kindAndSize_.getSize();
    assert(
        sz &&
        "GCCell with potentially zero size must call getAllocatedSizeSlow()");
#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
    assert(
        AlignedHeapSegment::getSegmentSize(this) ==
            FixedSizeHeapSegment::storageSize() &&
        "getAllocatedSize() can only be called on GCCells that live in FixedSizeHeapSegment");
#endif
    return sz;
  }

  /// Get the allocated size for any GCCell. This is slower than above version
  /// but works for any GCCell.
  inline size_t getAllocatedSizeSlow() const;

  /// Implementation of cellSize. Do not use this directly.
  template <class C>
  static constexpr uint32_t cellSizeImpl() {
    static_assert(std::is_convertible<C *, GCCell *>::value, "must be a cell");
    // ExternalStringPrimitive<T> is special: it extends VariableSizeRuntimeCell
    // but it's actually fixed-size.
    static_assert(
        !std::is_convertible<C *, VariableSizeRuntimeCell *>::value,
        "must be fixed-size");
    return sizeof(C);
  }

  /// Is true if the cell has a variable size, false if it has a fixed size.
  bool isVariableSize() const {
    return isVariableSize(getVT());
  }

  /// Is true if the cell has a variable size, false if it has a fixed size,
  /// assuming \p vtp is the proper VTable for this cell.
  static bool isVariableSize(const VTable *vtp) {
    return vtp->size == 0;
  }

  /// \return the kind of this cell.
  CellKind getKind() const {
    return kindAndSize_.getKind();
  }

  /// \return the vtable for this cell.
  /// \pre The cell must be valid.
  const VTable *getVT() const {
    assert(isValid() && "Called getVT() on an invalid cell");
    return kindAndSize_.getVT();
  }

  /// \return the KindAndSize for this cell.
  /// NOTE: this should only be used by the GC.
  KindAndSize getKindAndSize() const {
    return kindAndSize_;
  }

  /// Set the KindAndSize of this GCCell to \p kindAndSize.
  /// NOTE: this should only be used by the GC.
  void setKindAndSize(KindAndSize kindAndSize) {
    kindAndSize_ = kindAndSize;
  }

  /// These three functions implement marked forwarding pointers.

  /// Sets this cell to contain a forwarding pointer to another cell. Note that
  /// the heap is not well-formed in phases that set mark bits in live objects;
  /// the mark bits must be removed, or the objects declared dead, for
  /// well-formedness to be restored.
  /// NOTE: this should only be used by the GC.
  void setMarkedForwardingPointer(CompressedPointer cell) {
    forwardingPointer_ = CompressedPointer::fromRaw(cell.getRaw() | 0x1);
  }

  /// Assumes (and asserts) that a forwarding pointer has been set in
  /// this cell via setMarkedForwardingPointer.
  /// \return the forwarding pointer that was set.
  /// NOTE: this should only be used by the GC.
  CompressedPointer getMarkedForwardingPointer() const {
    assert(isMarked());
    return CompressedPointer::fromRaw(forwardingPointer_.getRaw() - 0x1);
  }

  /// \return whether a forwarding pointer has been set in this cell via
  /// setMarkedForwardingPointer.
  bool hasMarkedForwardingPointer() const {
    return isMarked();
  }

  const GCCell *nextCell() const {
    return reinterpret_cast<const GCCell *>(
        reinterpret_cast<const char *>(this) + getAllocatedSize());
  }
  GCCell *nextCell() {
    return reinterpret_cast<GCCell *>(
        reinterpret_cast<char *>(this) + getAllocatedSize());
  }

  /// \return true iff the cell is valid.
  ///
  /// Validity is defined by:
  ///   * The cell has a correct magic header
  ///   * The cell has a non-null vtable pointer that points to a VTable.
  bool isValid() const {
#ifndef NDEBUG
    // We only do the cell magic number check in debug builds.
    return magic_ == kMagic && kindAndSize_.getVT()->isValid();
#else
    return kindAndSize_.getVT()->isValid();
#endif
  }

  /// \return true iff the cell is valid (i.e., its vtable is valid)
  // and has the given \p expectedKind.
  bool isValid(CellKind expectedKind) const {
    return kindAndSize_.getVT()->isValid(expectedKind);
  }

  /// Placement new
  static void *operator new(size_t, void *p) {
    return p;
  }

  // Make new/delete illegal for GC cells.
  static void *operator new(size_t) = delete;
  static void operator delete(void *) = delete;

  /// \return a semi-unique (until the global counter overflows) id of this
  /// memory allocation.
  uint64_t getDebugAllocationId() const {
#ifndef NDEBUG
    return _debugAllocationId_;
#else
    return 0;
#endif
  }

#ifndef NDEBUG
  void setDebugAllocationIdInGC(uint32_t id) {
    _debugAllocationId_ = id;
  }
#endif

  /// Returns whether the cell's mark bit is set.
  bool isMarked() const {
    return forwardingPointer_.getRaw() & 0x1;
  }

  /// The maximum size for all normal GCCells (i.e., no large allocation
  /// support). With large allocation, any object with size larger than this
  /// will store 0 in its KindAndSize, and getAllocatedSizeSlow() must be used
  /// to get its actual size.
  static constexpr uint32_t maxNormalSize() {
    return KindAndSize::maxSize();
  }
};

static_assert(sizeof(GCCell) == sizeof(SHGCCell));
static_assert(sizeof(KindAndSize) == sizeof(SHKindAndSize));
// SHCellKind must match CellKind, since inline native code compares kinds.
#define CELL_KIND(name)                                    \
  static_assert(                                           \
      (int)CellKind::name##Kind == (int)SHCellKind_##name, \
      "SHCellKind_" #name " does not match CellKind");
#define SHARED_CELL_KIND(name) CELL_KIND(name)
#define CELL_RANGE(rangeName, first, last)           \
  static_assert(                                     \
      (int)CellKind::rangeName##Kind_first ==        \
              (int)SHCellKind_##rangeName##_first && \
          (int)CellKind::rangeName##Kind_last ==     \
              (int)SHCellKind_##rangeName##_last,    \
      "SHCellKind range " #rangeName " does not match CellKind");
#include "hermes/VM/CellKinds.def"
/// We should be able to assume that any allocation in a FixedSizeHeapSegment
/// has its size stored inline.
static_assert(GCCell::maxNormalSize() >= FixedSizeHeapSegment::maxSize());

/// A VariableSizeRuntimeCell is a GCCell with a variable size only known
/// at runtime, whereas GCCell is for fixed-size objects.
/// \see ArrayStorage for how to inherit from this class correctly.
class VariableSizeRuntimeCell : public GCCell {
 public:
  /// Sets the size of the current cell to be \p sz.
  /// NOTE: This should only be used by the GC, and only to shrink objects.
  /// \pre sz is already heap-aligned.
  void setSizeFromGC(uint32_t sz) {
    assert(
        isVariableSize() &&
        "Cannot call setSizeFromGC on a non-variable size cell");
    assert(
        sz >= sizeof(VariableSizeRuntimeCell) &&
        "Should not allocate a VariableSizeRuntimeCell of size less than "
        "the size of a cell");
    assert(
        sz <= maxNormalSize() && "Setting size of large object is not allowed");
    assert(sz < getAllocatedSize() && "Can only shrink objects");
    setKindAndSize(KindAndSize{getKind(), sz});
  }
};

static_assert(
    alignof(GCCell) <= HeapAlign,
    "GCCell's alignment exceeds the alignment requirement of the heap");

#ifdef NDEBUG
static_assert(
    sizeof(GCCell) == sizeof(CompressedPointer) &&
        sizeof(VariableSizeRuntimeCell) == sizeof(CompressedPointer),
    "Cell metadata should only be the size of a single pointer.");
#endif

static const char kInvalidHeapValue = (char)0xcc;

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_GCCELL_H
