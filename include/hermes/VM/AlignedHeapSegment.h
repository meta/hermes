/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_SEGMENT_H
#define HERMES_VM_SEGMENT_H

#include "hermes/ADT/BitArray.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/VM/AdviseUnused.h"
#include "hermes/VM/AllocResult.h"
#include "hermes/VM/CardBoundaryTable.h"
#include "hermes/VM/HeapAlign.h"

#include "llvh/Support/MathExtras.h"

#include <cstdint>
#include <cstring>

namespace hermes {
namespace vm {

class GCCell;
class StorageProvider;

#ifndef HERMESVM_LOG_HEAP_SEGMENT_SIZE
#error Heap segment size must be defined.
#endif

// In this class:
// TODO (T25527350): Debug Dump
// TODO (T25527350): Heap Moving

/// An \c AlignedHeapSegment manages a contiguous chunk of memory aligned to
/// kSegmentUnitSize. The storage is further split up according to the diagram
/// below:
///
/// +----------------------------------------+
/// | (1) Card Status Array                  |
/// +----------------------------------------+
/// | (2) Card Boundary Table                |
/// +----------------------------------------+
/// | (3) Mark Bit Array                     |
/// +----------------------------------------+
/// | (4) Allocation Space                   |
/// |                                        |
/// | ...                                    |
/// |                                        |
/// | (End)                                  |
/// +----------------------------------------+
///
/// The tables in (1), (2) and (3) cover the contiguous allocation space (4)
/// into which GCCells are bump allocated. They have fixed size computed from
/// kSegmentUnitSize. For segments whose size is some non-unit multiple of
/// kSegmentUnitSize, card status arrays are allocated separately instead. Only
/// one GCCell is allowed in each such segment, so card boundary table is unused
/// and the inline Mark Bit Array is large enough. Any segment size smaller than
/// kSegmentUnitSize is not supported. The headers of all GCCells, in any
/// segment type, must reside in the first region of kSegmentUnitSize. This
/// invariant ensures that we can always get the card status and boundary table
/// from a valid GCCell pointer.
class AlignedHeapSegment {
  friend struct RuntimeOffsets;

 protected:
  /// The provider that created this segment. It will be used to properly
  /// destroy this.
  StorageProvider *provider_{nullptr};

  /// The start of the aligned segment.
  char *lowLim_{nullptr};

  /// The current address in this segment to allocate new object. This must be
  /// positioned after lowLim_ to be correctly initialized.
  char *level_{start()};

 public:
  /// Base 2 log of the heap segment size.
  static constexpr size_t kLogSize = HERMESVM_LOG_HEAP_SEGMENT_SIZE;
  /// The unit segment size, in bytes. Any valid heap segment's size must be a
  /// multiple of this.
  static constexpr size_t kSegmentUnitSize = (1 << kLogSize);

  /// Contents of the memory region managed by this segment.
  class Contents {
   public:
    /// The number of bits representing the total number of heap-aligned
    /// addresses in the segment storage.
    static constexpr size_t kMarkBitArraySize =
        kSegmentUnitSize >> LogHeapAlign;
    /// BitArray for marking allocation region of a segment.
    using MarkBitArray = BitArray<kMarkBitArraySize>;
    /// The size (and base-two log of the size) of cards used in the card table.
    static constexpr size_t kLogCardSize =
        CardBoundaryTable::kLogCardSize; // ==> 512-byte cards.
    static constexpr size_t kCardSize = 1
        << kLogCardSize; // ==> 512-byte cards.
    /// The size of the maximum inline card table. CardStatus array larger
    /// segment has larger size and is stored separately.
    static constexpr size_t kInlineCardTableSize =
        kSegmentUnitSize >> kLogCardSize;

    /// Get the segment size from SHSegmentInfo. This is only used in debug code
    /// or when clearing the entire card table.
    size_t getSegmentSize() const {
      return (size_t)prefixHeader_.segmentInfo_.shiftedSegmentSize
          << HERMESVM_LOG_HEAP_SEGMENT_SIZE;
    }

   private:
    friend class FixedSizeHeapSegment;
    friend class AlignedHeapSegment;
    /// The JIT emits the card-dirty store inline; RuntimeOffsets derives the
    /// offsets and constants it bakes in from the members below.
    friend struct RuntimeOffsets;

    enum class CardStatus : char { Clean = 0, Dirty = 1 };

    /// Pass segment size to CardBoundaryTable constructor to allocate its data
    /// separately if \p sz > kSegmentUnitSize.
    Contents(size_t segmentSize) {
      assert(
          segmentSize && segmentSize % kSegmentUnitSize == 0 &&
          "segmentSize must be a multiple of kSegmentUnitSize");

      prefixHeader_.segmentInfo_.shiftedSegmentSize =
          segmentSize >> HERMESVM_LOG_HEAP_SEGMENT_SIZE;
      if (segmentSize == kSegmentUnitSize) {
        prefixHeader_.cards_ = inlineCardsArray_;
      } else {
        size_t cardTableSize = segmentSize >> kLogCardSize;
        // The card table must be initially all clean. CardStatus is clean by
        // default, so AtomicIfConcurrentGC<CardStatus> is as well. Thus, the
        // new array's entries are all clean with aggregate initialization.
        prefixHeader_.cards_ =
            (new AtomicIfConcurrentGC<CardStatus>[cardTableSize] {});
      }
    }

    ~Contents() {
      if (prefixHeader_.cards_ != inlineCardsArray_) {
        delete[] prefixHeader_.cards_;
      }
    }

    /// Note that because of the Contents object, the first few bytes of the
    /// card table are unused, we instead use them to store a small
    /// SHSegmentInfo struct and the outline cards_ pointer. Note that we can't
    /// use an anonymous struct here because we need to get its size to compute
    /// kFirstUsedIndex.
    struct PrefixHeader {
      SHSegmentInfo segmentInfo_;
      AtomicIfConcurrentGC<CardStatus> *cards_{nullptr};
    };

    union {
      PrefixHeader prefixHeader_;

      /// The card table optimizes young gen collections by restricting the
      /// amount of heap belonging to the old gen that must be scanned. The card
      /// table expects to be constructed at the start of a segment's storage,
      /// and covers the extent of that storage's memory. There are two cases:
      /// 1. For FixedSizeHeapSegment, this inline array is large enough.
      /// 2. For JumboHeapSegment, the CardStatus array is allocated separately.
      /// In either case, the pointers to the CardStatus array is stored in the
      /// above cards_ field. Any fast path should use a specialization for
      /// FixedSizeHeapSegment, which uses this inline array directly.
      AtomicIfConcurrentGC<CardStatus> inlineCardsArray_[kInlineCardTableSize];
    };

    /// Card boundary table, only used for FixedSizeHeapSegment.
    /// JumboHeapSegment only has one GCCell and it always lives at the start of
    /// allocation region.
    CardBoundaryTable boundaryTable_;

    MarkBitArray markBitArray_;

    static constexpr size_t kMetadataSize = sizeof(inlineCardsArray_) +
        sizeof(boundaryTable_) + sizeof(MarkBitArray);
    /// The minimum prefix space size (space before allocRegion_) to ensure that
    /// there is enough mapped unused space to store PrefixHeader. See the
    /// comment of kFirstUsedIndex for more details.
    static constexpr size_t kMinimumPrefixSize =
        sizeof(PrefixHeader) * Contents::kCardSize;
    /// If kMetadataSize <= kMinimumPrefixSize, add necessary padding bytes.
    /// Otherwise, make sure that it's aligned to kCardSize because allocRegion_
    /// needs to be aligned to that. Note that we use kMetadataSize+1 because
    /// kMetadataSize may already be aligned to kCardSize and cause kPaddingSize
    /// to be zero.
    static constexpr size_t kPaddingSize = kMetadataSize < kMinimumPrefixSize
        ? (llvh::alignTo<kMinimumPrefixSize>(kMetadataSize) - kMetadataSize)
        : (llvh::alignTo<kCardSize>(kMetadataSize + 1) - kMetadataSize);
    [[maybe_unused]] char padding_[kPaddingSize];

    /// The first byte of the allocation region, which extends past the "end" of
    /// the struct, to the end of the memory region that contains it.
    char allocRegion_[1];

    /// Ensure that each index corresponds to a single byte, so that one card
    /// byte is mapped to kCardSize heap bytes.
    static_assert(
        sizeof(inlineCardsArray_[0]) == 1,
        "Validate assumption that card table entries are one byte");

   public:
    /// A prefix of every segment is occupied by auxiliary data structures:
    /// 1. Card status array.
    /// 2. Card boundary array.
    /// 3. Mark bit array.
    /// 4. Padding pages and guard pages.
    /// Only the suffix of the card table that maps to the suffix of entire
    /// segment that is used for allocation is ever used; the prefix that maps
    /// to these auxiliary structures is not used. We repurpose this small space
    /// to store other data, such as SHSegmentInfo and outline cards array
    /// pointer. The actual first used index should take into account all these
    /// structures. It's used as starting index for clearing/dirtying range of
    /// bits. And this index must be larger than the size of prefix space that
    /// we repurposed, to avoid corrupting it when clearing/dirtying bits.
    static constexpr size_t kFirstUsedIndex = sizeof(PrefixHeader);
  };

  /// The offset from the beginning of a segment of the allocatable region.
  static constexpr size_t kOffsetOfAllocRegion{
      offsetof(Contents, allocRegion_)};

  /// Currently kCardSize % kHeapAlign is 0, so this check is a bit redundant to
  /// the next check, but let's keep it to prevent a bad change in either
  /// kHeapAlign or kCardSize.
  static_assert(
      isSizeHeapAligned(kOffsetOfAllocRegion),
      "Allocation region must start at a heap aligned offset");
  static_assert(
      (kOffsetOfAllocRegion & (Contents::kCardSize - 1)) == 0,
      "Allocation region needs to be card aligned");

  ~AlignedHeapSegment();

  /// Returns the address that is the lower bound of the segment.
  /// \post The returned pointer is guaranteed to be aligned to
  /// kSegmentUnitSize.
  char *lowLim() const {
    return lowLim_;
  }

  /// Gets the segment size from SHSegmentInfo. This should only be used when
  /// we only have a GCCell pointer and don't know its owning segment.
  static size_t getSegmentSize(const GCCell *cell) {
    return contents(alignedStorageStart(cell))->getSegmentSize();
  }

  /// Return the maximum allocation size for the heap segment that contains
  /// \p cell.
  static size_t maxSize(const GCCell *cell) {
    return getSegmentSize(cell) - kOffsetOfAllocRegion;
  }

  /// Returns the address at which the first allocation in this segment would
  /// occur.
  /// Disable UB sanitization because 'this' may be null during the tests.
  char *start() const LLVM_NO_SANITIZE("undefined") {
    return contents()->allocRegion_;
  }

  /// Returns the address at which the next allocation, if any, will occur.
  char *level() const {
    return level_;
  }

  /// Returns the end card table index for this segment.
  size_t getEndCardIndex() const {
    return contents()->getSegmentSize() >> Contents::kLogCardSize;
  }

  /// Returns the card table index corresponding to a byte at the given address.
  size_t addressToCardIndex(const void *addr) {
    return addressToCardIndex(contents(), addr);
  }

  /// Returns the address corresponding to the given card table index.
  const char *cardIndexToAddress(size_t index) {
    return cardIndexToAddress(contents(), index);
  }

  /// Make the card table entry for the given address dirty. his uses the cards_
  /// pointer instead of the inline cards array, so technically works for both
  /// small and large objects. But we should only use this for large objects
  /// since it's less efficient. This specialization is important because it's
  /// used in write barriers, which are performance critical functions.
  /// \pre \p addr is required to be an address covered by the card table.
  static void dirtyCardForAddressInLargeObj(
      const GCCell *owningObj,
      const void *addr) {
    auto *segContents = contents(alignedStorageStart(owningObj));
    size_t index = addressToCardIndex(segContents, addr);
    segContents->prefixHeader_.cards_[index].store(
        Contents::CardStatus::Dirty, std::memory_order_relaxed);
  }
  void dirtyCardForAddressInLargeObj(const void *addr) {
    size_t index = addressToCardIndex(contents(), addr);
    contents()->prefixHeader_.cards_[index].store(
        Contents::CardStatus::Dirty, std::memory_order_relaxed);
  }

  /// Make the card table entries for cards that intersect the given address
  /// range dirty. The range is a closed interval [low, high]. This works for
  /// both small and large objects.
  /// \pre \p low and \p high are required to be addresses covered by the card
  /// table.
  static void dirtyCardsForAddressRange(
      const GCCell *owningObj,
      const void *low,
      const void *high) {
    auto *segContents = contents(alignedStorageStart(owningObj));
    high = reinterpret_cast<const char *>(high) + Contents::kCardSize - 1;
    cleanOrDirtyRange(
        segContents,
        addressToCardIndex(segContents, low),
        addressToCardIndex(segContents, high),
        Contents::CardStatus::Dirty);
  }
  void dirtyCardsForAddressRange(const void *low, const void *high) {
    high = reinterpret_cast<const char *>(high) + Contents::kCardSize - 1;
    cleanOrDirtyRange(
        contents(),
        addressToCardIndex(low),
        addressToCardIndex(high),
        Contents::CardStatus::Dirty);
  }

  /// Returns whether the card table entry for the given address is dirty. This
  /// works for both small and large objects (and is only used in debug code).
  /// \pre \p addr is required to be an address covered by the card table.
  static bool isCardForAddressDirty(const GCCell *owningObj, const void *addr) {
    auto *segContents = contents(alignedStorageStart(owningObj));
    auto index = addressToCardIndex(segContents, addr);
    return segContents->prefixHeader_.cards_[index].load(
               std::memory_order_relaxed) == Contents::CardStatus::Dirty;
  }
  bool isCardForAddressDirty(const void *addr) {
    return isCardForIndexDirty(addressToCardIndex(addr));
  }

  /// Returns whether the card table entry for the given index is dirty. This
  /// works for both small and large objects (and is only used in debug code).
  /// \pre \p index is required to be a valid card table index.
  bool isCardForIndexDirty(size_t index) {
    return contents()->prefixHeader_.cards_[index].load(
               std::memory_order_relaxed) == Contents::CardStatus::Dirty;
  }

  /// If there is a dirty card at or after \p fromIndex, at an index less than
  /// \p endIndex, returns the index of the dirty card, else returns none.
  OptValue<size_t> findNextDirtyCard(size_t fromIndex, size_t endIndex) const {
    return findNextCardWithStatus(
        Contents::CardStatus::Dirty, fromIndex, endIndex);
  }

  /// If there is a card at or after \p fromIndex, at an index less than
  /// \p endIndex, returns the index of the clean card, else returns none.
  OptValue<size_t> findNextCleanCard(size_t fromIndex, size_t endIndex) const {
    return findNextCardWithStatus(
        Contents::CardStatus::Clean, fromIndex, endIndex);
  }

  /// Clears the card table.
  void clearAllCards() const {
    // Use memset for efficiency. This is safe because we only clear cards
    // after scanning them, which only happens during YG collection. When
    // YG collection finishes, the background marking thread may continue but
    // will not access card tables unless preparing for compaction (in which
    // case this function won't be called).
    char *cards = reinterpret_cast<char *>(contents()->prefixHeader_.cards_);
    std::memset(
        cards + Contents::kFirstUsedIndex,
        (int)Contents::CardStatus::Clean,
        getEndCardIndex() - Contents::kFirstUsedIndex);
  }

  /// \return The card boundary table.
  CardBoundaryTable &cardBoundaryTable() const {
    return contents()->boundaryTable_;
  }

  /// Return a reference to the mark bit array covering the memory region
  /// managed by this segment.
  Contents::MarkBitArray &markBitArray() const {
    return contents()->markBitArray_;
  }

  /// Mark the given \p cell. Assumes the given address is a valid heap object.
  static void setCellMarkBit(const GCCell *cell) {
    auto *markBits = markBitArrayCovering(cell);
    size_t ind = addressToMarkBitArrayIndex(cell);
    markBits->set(ind, true);
  }

  /// Return whether the given \p cell is marked. Assumes the given address is
  /// a valid heap object.
  static bool getCellMarkBit(const GCCell *cell) {
    auto *markBits = markBitArrayCovering(cell);
    size_t ind = addressToMarkBitArrayIndex(cell);
    return markBits->at(ind);
  }

  /// Translate the given address to a 0-based index in the MarkBitArray of its
  /// segment. The base address is the start of the storage of this segment. For
  /// JumboSegment, this should always return a constant index
  /// kOffsetOfAllocRegion >> LogHeapAlign.
  static size_t addressToMarkBitArrayIndex(const GCCell *cell) {
    auto *cp = reinterpret_cast<const char *>(cell);
    auto *base = reinterpret_cast<const char *>(alignedStorageStart(cell));
    return (cp - base) >> LogHeapAlign;
  }

  /// Return true if objects \p a and \p b live in the same segment. This is
  /// used to check if a pointer field in an object points to another object in
  /// the same segment (so that we don't need to dirty the cards -- we only need
  /// to dirty cards that might contain old-to-young pointers, which must cross
  /// segments). This also works for large segments, since there is only one
  /// cell in those segments (i.e., \p a and \p b would be the same).
  static bool containedInSameSegment(const GCCell *a, const GCCell *b) {
    return (reinterpret_cast<uintptr_t>(a) ^ reinterpret_cast<uintptr_t>(b)) <
        kSegmentUnitSize;
  }

  /// Returns the index of the segment containing \p lowLim, which is required
  /// to be the start of its containing segment.  (This can allow extra
  /// efficiency, in cases where the segment start has already been computed.)
  /// Note: we can't assert that \p lowLim is really the start of a segment, if
  /// it's from a large object, so the caller must ensure it.
  static unsigned getSegmentIndexFromStart(const void *lowLim) {
    auto *segInfo = reinterpret_cast<const SHSegmentInfo *>(lowLim);
    return segInfo->index;
  }

  /// Requires that \p lowLim is the start address of a segment, and sets
  /// that segment's index to \p index.
  static void setSegmentIndexFromStart(void *lowLim, unsigned index) {
    auto *segInfo = reinterpret_cast<SHSegmentInfo *>(lowLim);
    segInfo->index = index;
  }

 protected:
  /// Return a pointer to the contents of the memory region managed by this
  /// segment.
  Contents *contents() const {
    return reinterpret_cast<Contents *>(lowLim_);
  }

  /// Given the \p lowLim of some valid segment's memory region, returns a
  /// pointer to the Contents laid out in the storage, assuming it exists.
  static Contents *contents(void *lowLim) {
    return reinterpret_cast<Contents *>(lowLim);
  }

  AlignedHeapSegment() = default;

  /// Construct Contents() at the address of \p lowLim.
  AlignedHeapSegment(
      StorageProvider *provider,
      void *lowLim,
      size_t segmentSize);

  AlignedHeapSegment(AlignedHeapSegment &&);
  AlignedHeapSegment &operator=(AlignedHeapSegment &&);

  /// Returns the card table index corresponding to a byte at the given
  /// address.
  /// \pre \p addr must be within the bounds of the segment owning this card
  /// table or at most 1 card after it, that is to say
  ///
  ///    segment.lowLim() <= addr < segment.hiLim() + kCardSize
  ///
  /// Note that we allow the extra card after the segment in order to simplify
  /// the logic for callers that are using this function to generate an open
  /// interval of card indices. See \c dirtyCardsForAddressRange for an
  /// example of how this is used.
  static size_t addressToCardIndex(Contents *segContents, const void *addr)
      LLVM_NO_SANITIZE("null") {
    auto addrPtr = reinterpret_cast<const char *>(addr);
    auto *base = reinterpret_cast<char *>(segContents);
    assert(
        base <= addrPtr &&
        addrPtr <
            (base + segContents->getSegmentSize() + Contents::kCardSize) &&
        "address is required to be within range.");
    return (addrPtr - base) >> Contents::kLogCardSize;
  }

  /// Returns the address corresponding to the given card table index.
  ///
  /// \pre \p index is bounded:
  ///
  ///     0 <= index <= getEndCardIndex()
  static const char *cardIndexToAddress(Contents *segContents, size_t index) {
    assert(
        index <= (segContents->getSegmentSize() >> Contents::kLogCardSize) &&
        "index must be within the index range");
    auto *base = reinterpret_cast<char *>(segContents);
    const char *res = base + (index << Contents::kLogCardSize);
    assert(
        base <= res && res <= (base + segContents->getSegmentSize()) &&
        "result must be within the covered range");
    return res;
  }

  /// Clean or dirty the cards within the given range.
  static void cleanOrDirtyRange(
      Contents *segContents,
      size_t from,
      size_t to,
      Contents::CardStatus cleanOrDirty) {
    for (size_t index = from; index < to; index++) {
      segContents->prefixHeader_.cards_[index].store(
          cleanOrDirty, std::memory_order_relaxed);
    }
  }

  /// \return the index of the first card in the range [fromIndex, endIndex)
  /// with value \p status, or none if one does not exist.
  OptValue<size_t> findNextCardWithStatus(
      Contents::CardStatus status,
      size_t fromIndex,
      size_t endIndex) const {
    for (size_t idx = fromIndex; idx < endIndex; idx++)
      if (contents()->prefixHeader_.cards_[idx].load(
              std::memory_order_relaxed) == status)
        return idx;

    return llvh::None;
  }

 private:
  /// Used in move constructor and move assignment operator following the copy
  /// and swap idiom.
  friend void swap(AlignedHeapSegment &a, AlignedHeapSegment &b);

  /// Return the starting address for aligned region of size kSegmentUnitSize
  /// that \p cell resides in. If \c cell resides in a JumboSegment, it's the
  /// only cell there, this essentially returns its segment starting address.
  static char *alignedStorageStart(const GCCell *cell) {
    return reinterpret_cast<char *>(
        reinterpret_cast<uintptr_t>(cell) & ~(kSegmentUnitSize - 1));
  }

  /// Given a \p cell, returns a pointer to the MarkBitArray covering the
  /// segment that \p cell resides in.
  ///
  /// \pre There exists a currently alive heap that claims to contain \c cell.
  static Contents::MarkBitArray *markBitArrayCovering(const GCCell *cell) {
    auto *segStart = alignedStorageStart(cell);
    return &contents(segStart)->markBitArray_;
  }
};

/// JumboHeapSegment has custom storage size that must be a multiple of
/// kSegmentUnitSize. Each such segment can only allocate a single object that
/// may occupy the entire allocation space at most. Therefore, the inline
/// MarkBitArray is large enough, while CardStatus array needs to be allocated
/// separately. The card boundary table is not needed for JumboHeapSegment.
class JumboHeapSegment : public AlignedHeapSegment {
  /// Size of this segment.
  size_t segmentSize_{0};

 public:
  /// Construct a null JumboHeapSegment (one that does not own memory).
  JumboHeapSegment() = default;
  /// \c JumboHeapSegment is movable and assignable, but not copyable.
  JumboHeapSegment(JumboHeapSegment &&) = default;
  JumboHeapSegment &operator=(JumboHeapSegment &&) = default;
  JumboHeapSegment(const JumboHeapSegment &) = delete;
  JumboHeapSegment &operator=(const JumboHeapSegment &) = delete;

  /// Create a JumboHeapSegment by allocating memory with \p provider.
  static llvh::ErrorOr<JumboHeapSegment>
  create(StorageProvider *provider, const char *name, size_t segmentSize);

  /// Allocate memory of maxSize() from this segment.
  void *alloc() {
    assert(
        level() == start() &&
        "Only one GCCell may be allocated in this segment, at the start.");
    auto *cell = reinterpret_cast<GCCell *>(start());
    // After allocation, level_ should point to the end.
    level_ = hiLim();
#if LLVM_ADDRESS_SANITIZER_BUILD
    // Unpoison the entire allocation region.
    __asan_unpoison_memory_region(cell, maxSize());
#endif
    return cell;
  }

  /// Compute a suitable segment size to hold the object with size
  /// \p targetObjSize. It adds the size of the metadata and aligns to
  /// kSegmentUnitSize, which is required when creating a segment storage.
  /// Note: On a 64bit platform, \p targetObjSize could be as large as 2^32-1,
  /// hence the segment size could be larger than 4GB.
  static constexpr size_t computeSegmentSize(uint32_t targetObjSize) {
    return llvh::alignTo<kSegmentUnitSize>(
        targetObjSize + kOffsetOfAllocRegion);
  }

  /// Compute the actual allocation size for \p targetObjSize. This is
  /// essentially the `maxSize()` of the JumboHeapSegment with size derived from
  /// computeSegmentSize().
  static constexpr size_t computeActualCellSize(uint32_t targetObjSize) {
    return computeSegmentSize(targetObjSize) - kOffsetOfAllocRegion;
  }

  /// The maximum size of allocation region for this segment.
  size_t maxSize() const {
    return segmentSize_ - kOffsetOfAllocRegion;
  }

  /// Returns the storage size, in bytes, of a \c FixedSizeHeapSegment.
  size_t storageSize() const {
    return segmentSize_;
  }

  /// Returns the address that is the upper bound of the segment.
  char *hiLim() const {
    return lowLim() + storageSize();
  }

  /// \return \c true if and only if \p ptr is within the memory range owned by
  /// this segment.
  bool contains(const void *ptr) const {
    return start() <= ptr && ptr < hiLim();
  }

 private:
  JumboHeapSegment(StorageProvider *provider, void *lowLim, size_t segmentSize)
      : AlignedHeapSegment(provider, lowLim, segmentSize),
        segmentSize_(segmentSize) {}
};

/// FixedSizeHeapSegment has fixed storage size kSegmentUnitSize. Its
/// card status and boundary arrays and MarkBitArray are stored inline right
/// before the allocation region. This is used for all allocations in YoungGen
/// and normal object allocations in OldGen.
class FixedSizeHeapSegment : public AlignedHeapSegment {
  friend struct RuntimeOffsets;

  /// The upper limit of the space that we can currently allocated into;
  /// this may be decreased when externally allocated memory is credited to
  /// the generation owning this space.
  char *effectiveEnd_{end()};

 public:
  /// @name Constants and utility functions for the aligned storage of \c
  /// FixedSizeHeapSegment.
  ///
  /// @{
  /// The size and the alignment of the storage, in bytes.
  static constexpr size_t kSize = kSegmentUnitSize;
  /// Mask for isolating the offset into a storage for a pointer.
  static constexpr size_t kLowMask{kSize - 1};
  /// Mask for isolating the storage being pointed into by a pointer.
  static constexpr size_t kHighMask{~kLowMask};

  /// Returns the storage size, in bytes, of a \c FixedSizeHeapSegment.
  static constexpr size_t storageSize() {
    return kSize;
  }

  /// Returns the pointer to the beginning of the storage containing \p ptr
  /// (inclusive). Assuming such a storage exists. Note that
  ///
  ///     storageStart(seg.hiLim()) != seg.lowLim()
  ///
  /// as \c seg.hiLim() is not contained in the bounds of \c seg -- it
  /// is the first address not in the bounds.
  static void *storageStart(const void *ptr) {
    return reinterpret_cast<char *>(
        reinterpret_cast<uintptr_t>(ptr) & kHighMask);
  }

  /// Returns the pointer to the end of the storage containing \p ptr
  /// (exclusive). Assuming such a storage exists. Note that
  ///
  ///     storageEnd(seg.hiLim()) != seg.hiLim()
  ///
  /// as \c seg.hiLim() is not contained in the bounds of \c seg -- it
  /// is the first address not in the bounds.
  static void *storageEnd(const void *ptr) {
    return reinterpret_cast<char *>(storageStart(ptr)) + kSize;
  }

  /// Returns the offset in bytes to \p ptr from the start of its containing
  /// storage. Assuming such a storage exists. Note that
  ///
  ///     offset(seg.hiLim()) != seg.size()
  ///
  /// as \c seg.hiLim() is not contained in the bounds of \c seg -- it
  /// is the first address not in the bounds.
  static size_t offset(const char *ptr) {
    return reinterpret_cast<size_t>(ptr) & kLowMask;
  }
  /// @}

  /// Construct a null FixedSizeHeapSegment (one that does not own memory).
  FixedSizeHeapSegment() = default;
  /// \c FixedSizeHeapSegment is movable and assignable, but not copyable.
  /// Default to the base type move constructor and assignment operator. All
  /// fields that need to be invalidated are handled in them and the only
  /// field in this class (effectiveEnd_) doesn't need to be invalidated.
  FixedSizeHeapSegment(FixedSizeHeapSegment &&) = default;
  FixedSizeHeapSegment &operator=(FixedSizeHeapSegment &&) = default;
  FixedSizeHeapSegment(const FixedSizeHeapSegment &) = delete;
  FixedSizeHeapSegment &operator=(const FixedSizeHeapSegment &) = delete;

  /// Create a FixedSizeHeapSegment by allocating memory with \p provider.
  static llvh::ErrorOr<FixedSizeHeapSegment> create(
      StorageProvider *provider,
      const char *name = nullptr);

  /// Attempt an allocation of the given size in the segment.  If there is
  /// sufficent space, cast the space as a GCCell, and returns an uninitialized
  /// pointer to that cell (with success = true).  If there is not sufficient
  /// space, returns {nullptr, false}.
  inline AllocResult alloc(uint32_t size);

  /// Make the card table entry for the given address dirty. This uses the
  /// inline cards array and hence more efficient that the version in the base
  /// class. This specialization is important because it's used in write
  /// barriers, which are performance critical functions.
  /// \pre \p addr is required to be an address covered by the card table.
  static void dirtyCardForAddress(const void *addr) {
    auto *segContents = contents(storageStart(addr));
    size_t index = addressToCardIndex(segContents, addr);
    segContents->inlineCardsArray_[index].store(
        Contents::CardStatus::Dirty, std::memory_order_relaxed);
  }

  /// Find the head of the first cell that extends into the card at index
  /// \p cardIdx.
  /// \return A cell such that
  /// cell <= cardIndexToAddress(cardIdx) < cell->nextCell().
  GCCell *getFirstCellHead(size_t cardIdx) {
    GCCell *cell = contents()->boundaryTable_.firstObjForCard(
        lowLim(), lowLim() + contents()->getSegmentSize(), cardIdx);
    return cell;
  }

  /// Record the head of this cell so it can be found by the card scanner.
  static void setCellHead(const GCCell *cellStart, const size_t sz) {
    const char *start = reinterpret_cast<const char *>(cellStart);
    const char *end = start + sz;
    auto boundary = CardBoundaryTable::nextBoundary(start);
    // If this object crosses a card boundary, then update boundaries
    // appropriately.
    if (boundary.address() < end) {
      auto *segContents = contents(storageStart(cellStart));
      segContents->boundaryTable_.updateBoundaries(&boundary, start, end);
    }
  }

  /// The largest size the allocation region of an aligned heap segment could
  /// be.
  inline static constexpr size_t maxSize();

  /// The size of the allocation region in this aligned heap segment.
  inline size_t size() const;

  /// The number of bytes in the segment that are currently allocated.
  inline size_t used() const;

  /// The number of bytes in the segment that are available for allocation.
  inline size_t available() const;

  /// Returns the address that is the upper bound of the segment.
  char *hiLim() const {
    return lowLim() + storageSize();
  }

  /// Returns the first address after the region in which allocations can occur,
  /// taking external memory credits into a account (they decrease the effective
  /// end).
  inline char *effectiveEnd() const;

  /// The external memory charge of the generation owning this segment may have
  /// changed; set the effective end of the segment to the given \p
  /// effectiveEnd, which is required to be a valid effective end for the
  /// segment.
  void setEffectiveEnd(char *effectiveEnd);

  /// Clear any external memory charge for this segment.  This has the effect of
  /// equating the effective end to the real end.
  void clearExternalMemoryCharge();

  /// Returns the first address after the region in which allocations may occur,
  /// ignoring external memory credits.
  inline char *end() const;

  /// Returns whether \p a and \p b are contained in the same
  /// FixedSizeHeapSegment.
  inline static bool containedInSame(const void *a, const void *b);

  explicit operator bool() const {
    return lowLim();
  }

  /// \return \c true if and only if \p ptr is within the memory range owned by
  ///     this \c FixedSizeHeapSegment.
  bool contains(const void *ptr) const {
    return storageStart(ptr) == lowLim();
  }

  /// Mark a set of pages as unused.
  ///
  /// \pre start and end must be aligned to the page boundary.
  void markUnused(char *start, char *end);

  /// Clear allocations after \p level in this segment.
  ///
  /// \p MU Indicate whether the newly freed pages should be returned to the OS.
  ///
  /// \post this->level() == level.
  template <AdviseUnused MU = AdviseUnused::No>
  void setLevel(char *level);

  /// Clear allocations in this segment.
  ///
  /// \p MU Indicate whether the newly freed pages should be returned to the OS.
  ///
  /// \post this->level() == this->start();
  template <AdviseUnused MU = AdviseUnused::No>
  void resetLevel();

#ifndef NDEBUG
  /// Returns true iff \p lvl could refer to a level within this segment.
  bool dbgContainsLevel(const void *lvl) const;

  /// Returns true iff \p p is located within a valid section of the segment,
  /// and not at dead memory.
  bool validPointer(const void *p) const;

  /// Checks that dead values are present in the [start, end) range.
  static void checkUnwritten(char *start, char *end);
#endif

#ifdef HERMES_SLOW_DEBUG
  /// Find the object containing \p loc.
  static GCCell *findObjectContaining(const void *loc) {
    auto *lowLim = static_cast<char *>(storageStart(loc));
    auto *hiLim = lowLim + kSize;
    return contents(lowLim)->boundaryTable_.findObjectContaining(
        lowLim, hiLim, loc);
  }
#endif

 private:
  FixedSizeHeapSegment(StorageProvider *provider, void *lowLim);
};

AllocResult FixedSizeHeapSegment::alloc(uint32_t size) {
  assert(lowLim() != nullptr && "Cannot allocate in a null segment");
  assert(isSizeHeapAligned(size) && "size must be heap aligned");

  char *cellPtr; // Initialized in the if below.

  // On 64-bit systems, we know that we can't allocate a size large enough to
  // cause a pointer value to overflow. This is not true on 32-bit systems. This
  // test should be decided statically.
  // TODO: Is there a portable way of expressing this in the preprocessor?
  if (sizeof(void *) == 8) {
    // Calculate the new level_ once.
    char *newLevel = level_ + size;
    if (LLVM_UNLIKELY(newLevel > effectiveEnd())) {
      return {nullptr, false};
    }
    cellPtr = level_;
    level_ = newLevel;
  } else {
    if (LLVM_UNLIKELY(available() < size)) {
      return {nullptr, false};
    }
    cellPtr = level_;
    level_ += size;
  }

  __asan_unpoison_memory_region(cellPtr, size);
#ifndef NDEBUG
  checkUnwritten(cellPtr, cellPtr + size);
#endif

  auto *cell = reinterpret_cast<GCCell *>(cellPtr);
  return {cell, true};
}

/* static */ constexpr size_t FixedSizeHeapSegment::maxSize() {
  return storageSize() - kOffsetOfAllocRegion;
}

size_t FixedSizeHeapSegment::size() const {
  return end() - start();
}

size_t FixedSizeHeapSegment::used() const {
  return level() - start();
}

size_t FixedSizeHeapSegment::available() const {
  return effectiveEnd() - level();
}

char *FixedSizeHeapSegment::effectiveEnd() const {
  return effectiveEnd_;
}

char *FixedSizeHeapSegment::end() const {
  return start() + maxSize();
}

/* static */
bool FixedSizeHeapSegment::containedInSame(const void *a, const void *b) {
  return (reinterpret_cast<uintptr_t>(a) ^ reinterpret_cast<uintptr_t>(b)) <
      storageSize();
}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_SEGMENT_H
