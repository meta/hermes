/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/HadesGC.h"

#include "hermes/Support/Compiler.h"
#include "hermes/Support/ErrorHandling.h"
#include "hermes/Support/Statistic.h"
#include "hermes/VM/AllocResult.h"
#include "hermes/VM/CheckHeapWellFormedAcceptor.h"
#include "hermes/VM/FillerCell.h"
#include "hermes/VM/GCBase-inline.h"
#include "hermes/VM/GCCell.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/HermesValue-inline.h"
#include "hermes/VM/SmallHermesValue-inline.h"
#include "hermes/VM/WeakRoot-inline.h"

#include <array>
#include <functional>
#include <stack>

#define DEBUG_TYPE "hadesgc"

HERMES_SLOW_STATISTIC(
    NumOldGenAlloc,
    "NumOldGenAlloc: Number of old gen allocations");
HERMES_SLOW_STATISTIC(
    NumOldGenAllocSlow,
    "NumOldGenAllocSlow: Number of old gen allocation slow paths");

// In ASAN builds, poison the memory outside of the FreelistCell so that
// accesses are flagged as illegal while it is in the freelist.
// Use +1 on the pointer to skip towards the memory region directly after the
// FreelistCell header of a cell. This way the header is always intact and
// readable, and only the contents of the cell are poisoned.
#define ASAN_POISON_FREE_CELL(cell) \
  __asan_poison_memory_region(      \
      cell + 1, cell->getAllocatedSize() - sizeof(FreelistCell));

namespace hermes {
namespace vm {

static const char *kGCName =
    kConcurrentGC ? "hades (concurrent)" : "hades (incremental)";

static const char *kCompacteeNameForCrashMgr = "COMPACT";
static const char *kYGNameForCrashMgr = "YG";

// We have a target max pause time of 50ms.
static constexpr size_t kTargetMaxPauseMs = 50;

// Assert that it is always safe to construct a cell that is as large as the
// entire segment. This lets us always assume that contiguous regions in a
// segment can be safely turned into a single FreelistCell.
static_assert(
    FixedSizeHeapSegment::maxSize() <= HadesGC::maxNormalAllocationSize());

// A free list cell is always variable-sized.
const VTable HadesGC::OldGen::FreelistCell::vt{
    CellKind::FreelistKind,
    /*variableSize*/ 0};

void FreelistBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.setVTable(&HadesGC::OldGen::FreelistCell::vt);
}

GCCell *HadesGC::OldGen::finishAlloc(GCCell *cell, uint32_t sz) {
  // Track the number of allocated bytes in a segment.
  incrementAllocatedBytes(sz);
  // Write a mark bit so this entry doesn't get free'd by the sweeper.
  AlignedHeapSegment::setCellMarkBit(cell);
  // Could overwrite the VTable, but the allocator will write a new one in
  // anyway.
  return cell;
}

void HadesGC::OldGen::SegmentBucket::addToFreelist(SegmentBucket *dummyHead) {
  auto *oldHead = dummyHead->next;
  if (oldHead)
    oldHead->prev = this;
  prev = dummyHead;
  next = oldHead;
  dummyHead->next = this;
}

void HadesGC::OldGen::SegmentBucket::removeFromFreelist() const {
  if (next)
    next->prev = prev;
  prev->next = next;
}

void HadesGC::OldGen::addCellToFreelist(
    void *addr,
    uint32_t sz,
    SegmentBucket *segBucket) {
  assert(
      sz >= sizeof(FreelistCell) &&
      "Cannot construct a FreelistCell into an allocation in the OG");
  FreelistCell *newFreeCell = constructCell<FreelistCell>(addr, sz);
  FixedSizeHeapSegment::setCellHead(static_cast<GCCell *>(addr), sz);
  addCellToFreelist(newFreeCell, segBucket);
}

void HadesGC::OldGen::addCellToFreelist(
    FreelistCell *cell,
    SegmentBucket *segBucket) {
  const size_t sz = cell->getAllocatedSize();
  // Push onto the size-specific free list for this bucket and segment.
  CompressedPointer oldHead = segBucket->head;
  cell->next_ = oldHead;
  segBucket->head =
      CompressedPointer::encodeNonNull(cell, gc_.getPointerBase());

#ifndef NDEBUG
  if (oldHead) {
    // If there is an existing cell in the list, check that it belongs to the
    // same bucket.
    auto *ofc = vmcast<FreelistCell>(oldHead.getNonNull(gc_.pointerBase_));
    assert(getFreelistBucket(sz) == getFreelistBucket(ofc->getAllocatedSize()));
  }
#endif

  // If this SegmentBucket was not already in the freelist, add it.
  if (!oldHead) {
    uint32_t bucket = getFreelistBucket(sz);
    auto *dummyHead = &buckets_[bucket];
    segBucket->addToFreelist(dummyHead);

    // Set a bit indicating that there are now available blocks for this bucket.
    freelistBucketBitArray_.set(bucket, true);
  }

  ASAN_POISON_FREE_CELL(cell);
}

void HadesGC::OldGen::addCellToFreelistFromSweep(
    char *freeRangeStart,
    char *freeRangeEnd,
    SegmentBuckets &segBuckets,
    bool setHead) {
  assert(
      gc_.concurrentPhase_ == Phase::Sweep &&
      "addCellToFreelistFromSweep should only be called during sweeping.");
  size_t newCellSize = freeRangeEnd - freeRangeStart;
  // While coalescing, sweeping may generate new cells, so make sure the cell
  // head is updated.
  if (setHead)
    FixedSizeHeapSegment::setCellHead(
        reinterpret_cast<GCCell *>(freeRangeStart), newCellSize);
  auto *newCell = constructCell<FreelistCell>(freeRangeStart, newCellSize);
  // Get the size bucket for the cell being added;
  const uint32_t bucket = getFreelistBucket(newCellSize);
  // Push onto the size-specific free list for this bucket and segment.
  auto *segBucket = &segBuckets[bucket];
  newCell->next_ = segBucket->head;
  segBucket->head =
      CompressedPointer::encodeNonNull(newCell, gc_.getPointerBase());
  ASAN_POISON_FREE_CELL(newCell);
}

HadesGC::OldGen::FreelistCell *HadesGC::OldGen::removeCellFromFreelist(
    size_t bucket,
    SegmentBucket *segBucket) {
  return removeCellFromFreelist(&segBucket->head, bucket, segBucket);
}

HadesGC::OldGen::FreelistCell *HadesGC::OldGen::removeCellFromFreelist(
    AssignableCompressedPointer *prevLoc,
    size_t bucket,
    SegmentBucket *segBucket) {
  FreelistCell *cell =
      vmcast<FreelistCell>(prevLoc->getNonNull(gc_.getPointerBase()));
  assert(cell && "Cannot get a null cell from freelist");

  // Update whatever was pointing to the cell we are removing.
  *prevLoc = cell->next_;
  // Update the bit arrays if the given freelist is now empty.
  if (!segBucket->head) {
    segBucket->removeFromFreelist();

    // If setting the bit to 0 above made this bucket empty for all segments,
    // set the bucket bit to 0 as well.
    freelistBucketBitArray_.set(bucket, buckets_[bucket].next);
  }

  // Unpoison the memory so that the mutator can use it.
  __asan_unpoison_memory_region(
      cell + 1, cell->getAllocatedSize() - sizeof(FreelistCell));
  return cell;
}

/* static */
template <typename CallbackFunction>
void HadesGC::forAllObjsInSegment(
    FixedSizeHeapSegment &seg,
    CallbackFunction callback) {
  for (GCCell *cell = reinterpret_cast<GCCell *>(seg.start()),
              *end = reinterpret_cast<GCCell *>(seg.level());
       cell < end;
       cell = cell->nextCell()) {
    // Skip free-list entries.
    if (!vmisa<OldGen::FreelistCell>(cell)) {
      callback(cell);
    }
  }
}

/* static */
template <typename CallbackFunction>
void HadesGC::forCompactedObjsInSegment(
    FixedSizeHeapSegment &seg,
    CallbackFunction callback,
    PointerBase &base) {
  void *const stop = seg.level();
  GCCell *cell = reinterpret_cast<GCCell *>(seg.start());
  while (cell < stop) {
    if (cell->hasMarkedForwardingPointer()) {
      // This cell has been evacuated, do nothing.
      cell = reinterpret_cast<GCCell *>(
          reinterpret_cast<char *>(cell) +
          cell->getMarkedForwardingPointer()
              .getNonNull(base)
              ->getAllocatedSize());
    } else {
      // This cell is being compacted away, call the callback on it.
      // NOTE: We do not check if it is a FreelistCell here in order to avoid
      // the extra overhead of that check in YG. The callback should add that
      // check if required.
      callback(cell);
      cell = cell->nextCell();
    }
  }
}

GCCell *HadesGC::OldGen::FreelistCell::carve(uint32_t sz) {
  const auto origSize = getAllocatedSize();
  assert(
      origSize >= sz + minAllocationSize() &&
      "Can't split if it would leave too small of a second cell");
  const auto finalSize = origSize - sz;
  char *newCellAddress = reinterpret_cast<char *>(this) + finalSize;
  GCCell *const newCell = reinterpret_cast<GCCell *>(newCellAddress);
  setSizeFromGC(finalSize);
  FixedSizeHeapSegment::setCellHead(newCell, sz);
  return newCell;
}

class HadesGC::CollectionStats final {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = std::chrono::time_point<Clock>;
  using Duration = std::chrono::microseconds;

  CollectionStats(HadesGC &gc, std::string cause, std::string collectionType)
      : gc_{gc},
        cause_{std::move(cause)},
        collectionType_{std::move(collectionType)} {}
  ~CollectionStats() {
    assert(usedDbg_ && "Stats not submitted.");
  }

  void addCollectionType(std::string collectionType) {
    if (std::find(tags_.begin(), tags_.end(), collectionType) == tags_.end())
      tags_.emplace_back(std::move(collectionType));
  }

  /// Record the allocated bytes in the heap and its size before a collection
  /// begins.
  void setBeforeSizes(uint64_t allocated, uint64_t external, uint64_t sz) {
    allocatedBefore_ = allocated;
    externalBefore_ = external;
    sizeBefore_ = sz;
  }

  /// Record how many bytes were swept during the collection.
  void setSweptBytes(uint64_t sweptBytes) {
    sweptBytes_ = sweptBytes;
  }

  void setSweptExternalBytes(uint64_t externalBytes) {
    sweptExternalBytes_ = externalBytes;
  }

  void setAfterSize(uint64_t sz) {
    sizeAfter_ = sz;
  }

  /// Record that a collection is beginning right now.
  void setBeginTime() {
    assert(beginTime_ == Clock::time_point{} && "Begin time already set");
    beginTime_ = Clock::now();
  }

  /// Record that a collection is ending right now.
  void setEndTime() {
    assert(endTime_ == Clock::time_point{} && "End time already set");
    endTime_ = Clock::now();
  }

  std::chrono::milliseconds getElapsedTime() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - beginTime_);
  }

  /// Record this amount of CPU time was taken.
  /// Call begin/end in each thread that does work to correctly count CPU time.
  /// NOTE: Can only be used by one thread at a time.
  void beginCPUTimeSection() {
    assert(
        cpuTimeSectionStart_ == Duration{} &&
        "Must end cpu time section before starting another");
    cpuTimeSectionStart_ = oscompat::thread_cpu_time();
  }
  void endCPUTimeSection() {
    cpuDuration_ += oscompat::thread_cpu_time() - cpuTimeSectionStart_;
    cpuTimeSectionStart_ = {};
  }

  uint64_t beforeAllocatedBytes() const {
    return allocatedBefore_;
  }

  /// Since Hades allows allocations during an old gen collection, use the
  /// initially allocated bytes and the swept bytes to determine the actual
  /// impact of the GC.
  uint64_t afterAllocatedBytes() const {
    assert(sweptBytes_ <= allocatedBefore_);
    return allocatedBefore_ - sweptBytes_;
  }

  uint64_t afterExternalBytes() const {
    assert(sweptExternalBytes_ <= externalBefore_);
    return externalBefore_ - sweptExternalBytes_;
  }

  double survivalRatio() const {
    return allocatedBefore_
        ? static_cast<double>(afterAllocatedBytes()) / allocatedBefore_
        : 0;
  }

  void markUsed() {
    assert(!std::exchange(usedDbg_, true) && "markUsed called twice");
  }

  InternalAnalyticsEvent getEvent() && {
    markUsed();
    auto wallTime = endTime_ - beginTime_;
    return InternalAnalyticsEvent{
        GCAnalyticsEvent{
            gc_.getName(),
            gc_.getKindAsStr(),
            collectionType_,
            std::move(cause_),
            std::chrono::duration_cast<std::chrono::milliseconds>(wallTime),
            std::chrono::duration_cast<std::chrono::milliseconds>(cpuDuration_),
            /*allocated*/
            BeforeAndAfter{allocatedBefore_, afterAllocatedBytes()},
            /*size*/ BeforeAndAfter{sizeBefore_, sizeAfter_},
            /*external*/ BeforeAndAfter{externalBefore_, afterExternalBytes()},
            /*survivalRatio*/ survivalRatio(),
            /*tags*/ std::move(tags_)},
        /*durationSecs*/ std::chrono::duration<double>(wallTime).count(),
        /*cpuDurationSecs*/
        std::chrono::duration<double>(cpuDuration_).count()};
  }

 private:
  HadesGC &gc_;
  std::string cause_;
  std::string collectionType_;
  std::vector<std::string> tags_;
  TimePoint beginTime_{};
  TimePoint endTime_{};
  Duration cpuTimeSectionStart_{};
  Duration cpuDuration_{};
  uint64_t allocatedBefore_{0};
  uint64_t externalBefore_{0};
  uint64_t sizeBefore_{0};
  uint64_t sizeAfter_{0};
  uint64_t sweptBytes_{0};
  uint64_t sweptExternalBytes_{0};

#ifndef NDEBUG
  bool usedDbg_{false};
#endif
};

template <typename T>
static T convertPtr(PointerBase &, CompressedPointer cp) {
  return cp;
}
template <>
/* static */ GCCell *convertPtr(PointerBase &base, CompressedPointer cp) {
  return cp.get(base);
}
template <typename T>
static T convertPtr(PointerBase &, GCCell *p) {
  return p;
}
template <>
/* static */ CompressedPointer convertPtr(PointerBase &base, GCCell *a) {
  return CompressedPointer::encodeNonNull(a, base);
}

template <bool CompactionEnabled>
class HadesGC::EvacAcceptor final : public RootAcceptor,
                                    public WeakRootAcceptor {
 public:
  EvacAcceptor(HadesGC &gc, bool doCompaction)
      : gc{gc},
        pointerBase_{gc.getPointerBase()},
        isTrackingIDs_{gc.isTrackingIDs()} {
    if constexpr (CompactionEnabled) {
      // If we are compacting, populate the compactee start pointer so we
      // evacuate pointers we find in the compactee segment.
      if (doCompaction) {
        compacteeEvacStart = gc.compactee_.start;
        compacteeEvacStartCP = gc.compactee_.startCP;
      }
    } else {
      assert(!doCompaction && "Compaction is not enabled");
    }
  }

  ~EvacAcceptor() override {}

  inline bool shouldForward(const void *ptr) const {
    return gc.inYoungGen(ptr) ||
        (CompactionEnabled &&
         compacteeEvacStart == FixedSizeHeapSegment::storageStart(ptr));
  }
  inline bool shouldForward(CompressedPointer ptr) const {
    return gc.inYoungGen(ptr) ||
        (CompactionEnabled && compacteeEvacStartCP == ptr.getSegmentStart());
  }

  LLVM_NODISCARD GCCell *acceptRoot(GCCell *ptr) {
    if (shouldForward(ptr))
      return forwardCell<GCCell *>(ptr);
    return ptr;
  }

  LLVM_NODISCARD GCCell *acceptHeap(GCCell *ptr, void *heapLoc) {
    if (shouldForward(ptr)) {
      assert(
          AlignedHeapSegment::getCellMarkBit(ptr) &&
          "Should only evacuate marked objects.");
      return forwardCell<GCCell *>(ptr);
    }
    if (CompactionEnabled && gc.compactee_.contains(ptr)) {
      assert(
          currentCell_ < heapLoc &&
          heapLoc < (reinterpret_cast<const char *>(currentCell_) +
                     currentCell_->getAllocatedSizeSlow()) &&
          "currentCell_ must be set for compaction");
      // If a compaction is about to take place, dirty the card for any newly
      // evacuated cells, since the marker may miss them.
      AlignedHeapSegment::dirtyCardForAddressInLargeObj(currentCell_, heapLoc);
    }
    return ptr;
  }

  LLVM_NODISCARD CompressedPointer
  acceptHeap(CompressedPointer cptr, void *heapLoc) {
    if (shouldForward(cptr)) {
      GCCell *ptr = cptr.getNonNull(pointerBase_);
      assert(
          AlignedHeapSegment::getCellMarkBit(ptr) &&
          "Should only evacuate marked objects.");
      return forwardCell<CompressedPointer>(ptr);
    }
    if (CompactionEnabled && gc.compactee_.contains(cptr)) {
      assert(
          currentCell_ < heapLoc &&
          heapLoc < (reinterpret_cast<const char *>(currentCell_) +
                     currentCell_->getAllocatedSizeSlow()) &&
          "currentCell_ must be set for compaction");
      // If a compaction is about to take place, dirty the card for any newly
      // evacuated cells, since the marker may miss them.
      AlignedHeapSegment::dirtyCardForAddressInLargeObj(currentCell_, heapLoc);
    }
    return cptr;
  }

  template <typename T>
  LLVM_NODISCARD T forwardCell(GCCell *const cell) {
    assert(
        AlignedHeapSegment::getCellMarkBit(cell) &&
        "Cannot forward unmarked object");
    if (cell->hasMarkedForwardingPointer()) {
      // Get the forwarding pointer from the header of the object.
      CompressedPointer forwardedCell = cell->getMarkedForwardingPointer();
      assert(
          forwardedCell.getNonNull(pointerBase_)->isValid() &&
          "Cell was forwarded incorrectly");
      return convertPtr<T>(pointerBase_, forwardedCell);
    }
    assert(cell->isValid() && "Encountered an invalid cell");
    const auto cellSize = cell->getAllocatedSize();
    // Newly discovered cell, first forward into the old gen.
    GCCell *const newCell = gc.oldGen_.alloc(cellSize);
    HERMES_SLOW_ASSERT(
        gc.inOldGen(newCell) && "Evacuated cell not in the old gen");
    assert(
        AlignedHeapSegment::getCellMarkBit(newCell) &&
        "Cell must be marked when it is allocated into the old gen");
    // Copy the contents of the existing cell over before modifying it.
    std::memcpy((void *)newCell, cell, cellSize);
    assert(newCell->isValid() && "Cell was copied incorrectly");
    evacuatedBytes_ += cellSize;
    CopyListCell *const copyCell = static_cast<CopyListCell *>(cell);
    // Set the forwarding pointer in the old spot
    copyCell->setMarkedForwardingPointer(
        CompressedPointer::encodeNonNull(newCell, pointerBase_));
    if (isTrackingIDs_) {
      gc.moveObject(cell, cellSize, newCell, cellSize);
    }
    // Push onto the copied list.
    push(copyCell);
    return convertPtr<T>(pointerBase_, newCell);
  }

  void accept(GCCell *&ptr) override {
    ptr = acceptRoot(ptr);
  }

  void accept(GCPointerBase &ptr) {
    ptr.setInGC(acceptHeap(ptr, &ptr));
  }

  void accept(PinnedHermesValue &hv) override {
    assert((!hv.isPointer() || hv.getPointer()) && "Value is not nullable.");
    acceptNullable(hv);
  }

  void acceptNullable(PinnedHermesValue &hv) override {
    if (hv.isPointer()) {
      GCCell *forwardedPtr = acceptRoot(static_cast<GCCell *>(hv.getPointer()));
      hv.setInGC(hv.updatePointer(forwardedPtr), gc);
    }
  }

  void accept(PinnedSmallHermesValue &shv) override {
    if (shv.isPointer()) {
      GCCell *ptr = shv.getPointer(pointerBase_);
      GCCell *forwardedPtr = acceptRoot(ptr);
      shv.setInGC(shv.updatePointer(forwardedPtr, pointerBase_), gc);
    }
  }

  void accept(GCHermesValueBase &hv) {
    if (hv.isPointer()) {
      GCCell *forwardedPtr =
          acceptHeap(static_cast<GCCell *>(hv.getPointer()), &hv);
      hv.setInGC(hv.updatePointer(forwardedPtr), gc);
    }
  }

  void accept(GCSmallHermesValueBase &hv) {
    if (hv.isPointer()) {
      CompressedPointer forwardedPtr = acceptHeap(hv.getPointer(), &hv);
      hv.setInGC(hv.updatePointer(forwardedPtr), gc);
    }
  }

  void acceptWeak(WeakRootBase &wr) override {
    // It's safe to not do a read barrier here since this is happening in the GC
    // and does not extend the lifetime of the referent.
    GCCell *const ptr = wr.getNoBarrierUnsafe(pointerBase_);

    if (!shouldForward(ptr))
      return;

    if (ptr->hasMarkedForwardingPointer()) {
      // Get the forwarding pointer from the header of the object.
      CompressedPointer forwardedCell = ptr->getMarkedForwardingPointer();
      assert(
          forwardedCell.getNonNull(pointerBase_)->isValid() &&
          "Cell was forwarded incorrectly");
      // Assign back to the input pointer location.
      wr = forwardedCell;
    } else {
      wr = nullptr;
    }
  }

  void acceptWeak(WeakSmallHermesValue &wshv) override {
    if (wshv.isPointer()) {
      GCCell *const ptr = wshv.getPointerNoBarrierUnsafe(pointerBase_);
      if (!shouldForward(ptr))
        return;

      if (ptr->hasMarkedForwardingPointer()) {
        CompressedPointer forwardedCell = ptr->getMarkedForwardingPointer();
        assert(
            forwardedCell.getNonNull(pointerBase_)->isValid() &&
            "Cell was forwarded incorrectly");
        wshv.set(wshv.updatePointer(forwardedCell));
      } else {
        // The target GCCell is dead, invalidate it.
        wshv.invalidate();
      }
    }
  }

  // Nothing to do for symbols, since they are not collected during a YG
  // collection.
  void acceptWeakSym(WeakRootSymbolID &ws) override {}
  void accept(const RootSymbolID &sym) override {}
  void accept(const GCSymbolID &sym) {}

  uint64_t evacuatedBytes() const {
    return evacuatedBytes_;
  }

  /// Set the current cell being visited.
  void setCurrentCell(const GCCell *cell) {
    if constexpr (CompactionEnabled)
      currentCell_ = cell;
  }

  /// The copy list is a linked list managed implicitly in the body of each
  /// copied YG object. This points to the first node in the list.
  AssignableCompressedPointer copyListHead{nullptr};

 private:
  HadesGC &gc;
  PointerBase &pointerBase_;
  const bool isTrackingIDs_;
  uint64_t evacuatedBytes_{0};

  /// If we are currently evacuating the compactee, then these are set to
  /// gc.compactee_.start and gc.compactee_.startCP. This allows us to
  /// efficiently check for whether we need to evacuate a cell.
  void *compacteeEvacStart{
      reinterpret_cast<void *>(CompacteeState::kInvalidCompacteeStart)};
  AssignableCompressedPointer compacteeEvacStartCP{
      CompressedPointer::fromRaw(CompacteeState::kInvalidCompacteeStart)};

  /// Current GCCell being visited. For heap locations that could be from a
  /// large object, we need this pointer to get the correct card table.
  const GCCell *currentCell_{nullptr};

  void push(CopyListCell *cell) {
    cell->next_ = copyListHead;
    copyListHead = CompressedPointer::encodeNonNull(cell, pointerBase_);
  }
};

void HadesGC::barrierEnqueue(GCCell *cell) {
  if (LLVM_UNLIKELY(
          markState_->barrierChunkIndex_ ==
          markState_->barrierPushChunk_.size())) {
    // barrierPushChunk_ is full, we need to flush it.
    handleFullBarrierChunk();
  }
  markState_->barrierPushChunk_[markState_->barrierChunkIndex_++] = cell;
}

void HadesGC::handleFullBarrierChunk() {
  // We introduce a scope here to make sure we release the barrier lock before
  // calling incremental mark. Otherwise, we might cause a deadlock with the
  // background thread since it obtains the locks in the opposite order.
  {
    std::lock_guard lk{markState_->barrierWorklistMtx_};
    markState_->barrierWorklist_.insert(
        markState_->barrierWorklist_.end(),
        markState_->barrierPushChunk_.begin(),
        markState_->barrierPushChunk_.end());
    markState_->barrierChunkIndex_ = 0;

    if (LLVM_LIKELY(
            markState_->barrierWorklist_.size() <
            MarkState::kMaxBarrierWorklistSize))
      return;
  }

  // The barrier queue is full, call incrementalMark to drain it, but pass a
  // limit of 0 so it doesn't do any actual marking. We could also consider
  // doing some marking here to prevent the mark worklist from growing, but that
  // is less of a concern because (unlike the write barrier) its size is bounded
  // by the number of objects in the heap.
  // Note that doing "complete marking" or sweeping here would be risky.
  // Completing marking involves clearing WeakRoots, and sweeping will perform
  // trimming since it is running on the mutator. Neither of these are expected
  // unless there is an allocation.
  auto lk = ensureBackgroundTaskPaused();
  incrementalMark(0);
  assert(markState_->barrierWorklist_.empty() && "Worklist was not emptied");
}

llvh::SmallVector<GCCell *, 0> HadesGC::drainBarrierWorklist() {
  llvh::SmallVector<GCCell *, 0> cells;
  // Move the list (in O(1) time) to a local variable, and then release the
  // lock. This unblocks the mutator faster.
  std::lock_guard<Mutex> lk{markState_->barrierWorklistMtx_};
  std::swap(cells, markState_->barrierWorklist_);
  assert(markState_->barrierWorklist_.empty() && "worklist isn't cleared out");
  assert(cells.size() <= MarkState::kMaxBarrierWorklistSize && "overflow");
  // Keep the previously allocated size to minimise allocations
  markState_->barrierWorklist_.reserve(cells.size());
  return cells;
}

void HadesGC::flushPendingBarrierPushChunk() {
  // NOTE: We do not acquire the lock here, because there must be no running
  // background thread when this is called.
  assert(!calledByBackgroundThread());
  markState_->barrierWorklist_.insert(
      markState_->barrierWorklist_.end(),
      markState_->barrierPushChunk_.data(),
      markState_->barrierPushChunk_.data() + markState_->barrierChunkIndex_);
  markState_->barrierChunkIndex_ = 0;
}

#ifndef NDEBUG
bool HadesGC::isBarrierWorklistEmpty() {
  std::lock_guard<Mutex> lk{markState_->barrierWorklistMtx_};
  return markState_->barrierChunkIndex_ == 0 &&
      markState_->barrierWorklist_.empty();
}
#endif

/// Ideally, MarkAcceptor would be declared in an anonymous namespace, but it
/// needs to be forward declared in HadesGC.h in order to access private members
/// of HadesGC. Forcing it to have internal linkage reflects the fact that it is
/// only actually used in this file, and the compiler should inline methods on
/// it aggressively.
class HERMES_ATTRIBUTE_INTERNAL_LINKAGE HadesGC::MarkAcceptor final
    : public RootAcceptor {
  HadesGC &gc;
  PointerBase &pointerBase_;
  /// Current GCCell being visited. For heap locations that could be from a
  /// large object, we need this pointer to get the correct card table.
  const GCCell *currentCell_{nullptr};

 public:
  MarkAcceptor(HadesGC &gc) : gc{gc}, pointerBase_{gc.getPointerBase()} {}

  void acceptHeap(GCCell *cell, const void *heapLoc) {
    assert(cell && "Cannot pass null pointer to acceptHeap");
    assert(!gc.inYoungGen(heapLoc) && "YG slot found in OG marking");
    assert(
        currentCell_ < heapLoc &&
        heapLoc < (reinterpret_cast<const char *>(currentCell_) +
                   currentCell_->getAllocatedSizeSlow()) &&
        "currentCell_ must be set for compaction");
    if (gc.compactee_.contains(cell) && !gc.compactee_.contains(heapLoc)) {
      // This is a pointer in the heap pointing into the compactee, dirty the
      // corresponding card.
      AlignedHeapSegment::dirtyCardForAddressInLargeObj(currentCell_, heapLoc);
    }
    if (AlignedHeapSegment::getCellMarkBit(cell)) {
      // Points to an already marked object, do nothing.
      return;
    }
    // This must be done after checking the cell mark bit, to avoid reading the
    // metadata of cells in the YG, which we do not have the lock for.
    assert(cell->isValid() && "Encountered an invalid cell");
    push(cell);
  }

  void acceptRoot(GCCell *cell) {
    assert(cell->isValid() && "Encountered an invalid cell");
    if (!AlignedHeapSegment::getCellMarkBit(cell))
      push(cell);
  }

  void accept(GCCell *&ptr) override {
    if (ptr)
      acceptRoot(ptr);
  }

  void accept(GCPointerBase &ptr) {
    if (auto cp = concurrentRead<CompressedPointer>(ptr))
      acceptHeap(cp.getNonNull(pointerBase_), &ptr);
  }

  void accept(GCHermesValueBase &hvRef) {
    HermesValue hv = concurrentRead<HermesValue>(hvRef);
    if (hv.isPointer()) {
      acceptHeap(static_cast<GCCell *>(hv.getPointer()), &hvRef);
    } else if (hv.isSymbol()) {
      acceptSym(hv.getSymbol());
    }
  }

  void accept(PinnedHermesValue &hv) override {
    // PinnedHermesValue is a root type and cannot live in the heap. Therefore
    // there's no risk of a concurrent access.
    if (hv.isPointer()) {
      acceptRoot(static_cast<GCCell *>(hv.getPointer()));
    } else if (hv.isSymbol()) {
      acceptSym(hv.getSymbol());
    }
  }
  void acceptNullable(PinnedHermesValue &hv) override {
    if (hv.isPointer()) {
      if (void *ptr = hv.getPointer())
        acceptRoot(static_cast<GCCell *>(ptr));
    } else if (hv.isSymbol()) {
      acceptSym(hv.getSymbol());
    }
  }

  void accept(PinnedSmallHermesValue &shv) override {
    // PinnedSmallHermesValue is a root type and cannot live in the heap.
    // Therefore there's no risk of a concurrent access.
    if (shv.isPointer()) {
      GCCell *ptr = shv.getPointer(pointerBase_);
      acceptRoot(ptr);
    } else if (shv.isSymbol()) {
      acceptSym(shv.getSymbol());
    }
  }

  void accept(GCSmallHermesValueBase &hvRef) {
    const SmallHermesValue hv = concurrentRead<SmallHermesValue>(hvRef);
    if (hv.isPointer()) {
      acceptHeap(hv.getPointer(pointerBase_), &hvRef);
    } else if (hv.isSymbol()) {
      acceptSym(hv.getSymbol());
    }
  }

  void acceptSym(SymbolID sym) {
    const uint32_t idx = sym.unsafeGetIndex();
    if (sym.isInvalid() || idx >= gc.markState_->markedSymbols.size()) {
      // Ignore symbols that aren't valid or are pointing outside of the range
      // when the collection began.
      return;
    }
    gc.markState_->markedSymbols[idx] = true;
  }

  void accept(const RootSymbolID &sym) override {
    acceptSym(sym);
  }
  void accept(const GCSymbolID &sym) {
    acceptSym(concurrentRead<SymbolID>(sym));
  }

  void push(GCCell *cell) {
    assert(
        !AlignedHeapSegment::getCellMarkBit(cell) &&
        "A marked object should never be pushed onto a worklist");
    assert(
        !gc.inYoungGen(cell) &&
        "Shouldn't ever push a YG object onto the worklist");
    AlignedHeapSegment::setCellMarkBit(cell);
    gc.markState_->localWorklist.push(cell);
  }

  /// Set the current cell being visited.
  void setCurrentCell(const GCCell *cell) {
    currentCell_ = cell;
  }

 private:
  template <typename T>
  T concurrentReadImpl(const T &valRef) {
    // There is a benign data race here, as the GC can read a pointer while
    // it's being modified by the mutator; however, the following rules we
    // obey prevent it from being a problem:
    // * The only things being modified that the GC reads are the GCPointers
    //    and GCHermesValue in an object. All other fields are ignored.
    // * Those fields are fewer than 64 bits.
    // * Therefore, on 64-bit platforms, those fields are atomic
    //    automatically.
    // * On 32-bit platforms, we don't run this code concurrently, and
    //    instead yield cooperatively with the mutator.
    // * Thanks to the write barrier, if something is modified its old value
    //    is placed in the globalWorklist, so we don't miss marking it.
    // * Since the global worklist is pushed onto *before* the write
    //    happens, we know that there's no way the loop will exit unless it
    //    reads the old value.
    // * If it observes the old value (pre-write-barrier value) here, the
    //    new value will still be live, either by being pre-marked by the
    //    allocator, or because something else's write barrier will catch
    //    the modification.
    TsanIgnoreReadsBegin();

    // Read valRef and copy into result.
    T result = valRef;

    // This fence prevents the compiler from optimizing away the returned value,
    // guaranteeing that we will not observe changes to the underlying value
    // past this point. Not having a fence here could lead to a TOCTOU bug,
    // because the underlying value may change after a pointer check (in the
    // case of HermesValue) or a null check (for pointers).
    // We have to make sure that the read we did above is not reordered later.
    std::atomic_signal_fence(std::memory_order_seq_cst);

    TsanIgnoreReadsEnd();
    return result;
  }

  template <typename T>
  T concurrentRead(const T &ref) {
    return kConcurrentGC ? concurrentReadImpl<T>(ref) : ref;
  }
};

bool HadesGC::incrementalMark(size_t markLimit) {
  assert(gcMutex_ && "Must hold the GC lock while accessing mark bits.");
  MarkAcceptor acceptor(*this);

  // Pull any new items off the global worklist.
  auto cells = drainBarrierWorklist();
  for (GCCell *cell : cells) {
    assert(cell->isValid() && "Invalid cell received off the global worklist");
    assert(
        !inYoungGen(cell) &&
        "Shouldn't ever traverse a YG object in this loop");
    HERMES_SLOW_ASSERT(
        dbgContains(cell) && "Non-heap cell found in global worklist");
    if (!AlignedHeapSegment::getCellMarkBit(cell)) {
      // Cell has not yet been marked.
      acceptor.push(cell);
    }
  }

  size_t numMarkedBytes = 0;
  while (!markState_->localWorklist.empty() && numMarkedBytes < markLimit) {
    GCCell *const cell = markState_->localWorklist.top();
    markState_->localWorklist.pop();
    assert(cell->isValid() && "Invalid cell in marking");
    assert(
        AlignedHeapSegment::getCellMarkBit(cell) &&
        "Discovered unmarked object");
    assert(
        !inYoungGen(cell) &&
        "Shouldn't ever traverse a YG object in this loop");
    HERMES_SLOW_ASSERT(
        dbgContains(cell) && "Non-heap object discovered during marking");
    const auto sz = cell->getAllocatedSizeSlow();
    numMarkedBytes += sz;
    acceptor.setCurrentCell(cell);
    markCell(acceptor, cell);
  }
  markState_->markedBytes += numMarkedBytes;
  return !markState_->localWorklist.empty();
}

/// Mark weak roots separately from the MarkAcceptor since this is done while
/// the world is stopped.
/// Don't use the default weak root acceptor because fine-grained control of
/// writes of compressed pointers is important.
class HadesGC::MarkWeakRootsAcceptor final : public WeakRootAcceptor {
 public:
  MarkWeakRootsAcceptor(HadesGC &gc, const llvh::BitVector &markedSymbols)
      : gc_{gc}, markedSymbols_(markedSymbols) {}

  void acceptWeak(WeakRootBase &wr) override {
    if (!wr) {
      return;
    }
    GCCell *const cell = wr.getNoBarrierUnsafe(gc_.getPointerBase());
    HERMES_SLOW_ASSERT(gc_.dbgContains(cell) && "ptr not in heap");
    if (AlignedHeapSegment::getCellMarkBit(cell)) {
      // If the cell is marked, no need to do any writes.
      return;
    }
    // Reset weak root if target GCCell is dead.
    wr = nullptr;
  }

  void acceptWeakSym(WeakRootSymbolID &ws) override {
    if (ws.isInvalid()) {
      return;
    }
    const uint32_t idx = ws.getNoBarrierUnsafe().unsafeGetIndex();
    // Symbols pointing outside of the markedSymbols range have been allocated
    // after this collection has started and should be treated as live.
    if (idx < markedSymbols_.size() && !markedSymbols_[idx]) {
      ws = SymbolID::empty();
    }
  }

  void acceptWeak(WeakSmallHermesValue &wshv) override {
    if (wshv.isPointer()) {
      GCCell *const cell = wshv.getPointerNoBarrierUnsafe(gc_.getPointerBase());
      HERMES_SLOW_ASSERT(gc_.dbgContains(cell) && "ptr not in heap");
      if (AlignedHeapSegment::getCellMarkBit(cell)) {
        // If the cell is marked, do nothing.
        return;
      }
      // The target GCCell is dead, invalidate it.
      wshv.invalidate();
    } else if (wshv.isSymbol()) {
      SymbolID sym = wshv.getSymbolNoBarrierUnsafe();
      if (sym.isInvalid())
        return;
      const uint32_t idx = sym.unsafeGetIndex();
      // Symbols pointing outside the markedSymbols range have been allocated
      // after this collection has started and should be treated as live.
      if (idx < markedSymbols_.size() && !markedSymbols_[idx]) {
        wshv.invalidate();
      }
    }
  }

 private:
  HadesGC &gc_;
  const llvh::BitVector &markedSymbols_;
};

class HadesGC::Executor {
 public:
  Executor() : thread_([this] { worker(); }) {}
  ~Executor() {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      shutdown_ = true;
      cv_.notify_one();
    }
    thread_.join();
  }

  /// Enqueue the function \p fn for execution on the executor thread.
  template <typename Fn>
  void add(Fn &&fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.emplace_back(std::forward<Fn>(fn));
    cv_.notify_one();
  }

  /// Get the ID of the executor thread.
  std::thread::id getThreadId() const {
    return thread_.get_id();
  }

 private:
  /// Drain enqueued tasks from the queue and run them.
  void worker() {
    oscompat::set_thread_name("hades");
    std::unique_lock<std::mutex> lk(mtx_);
    while (!shutdown_) {
      cv_.wait(lk, [this]() { return !queue_.empty() || shutdown_; });
      while (!queue_.empty()) {
        auto fn = std::move(queue_.front());
        queue_.pop_front();
        lk.unlock();
        fn();
        lk.lock();
      }
    }
  }

  std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool shutdown_{false};
  std::thread thread_;
};

bool HadesGC::OldGen::sweepNext(bool backgroundThread) {
  // Check if there are any more segments to sweep. This could be triggered when
  // OG has zero FixedSizeHeapSegment (but could have JumboHeapSegment).
  if (!sweepIterator_.segNumber) {
    endSweep();
    return false;
  }
  assert(gc_.gcMutex_ && "gcMutex_ must be held while sweeping.");

  sweepIterator_.segNumber--;

  const bool isTracking = gc_.isTrackingIDs();
  // Re-evaluate this start point each time, as releasing the gcMutex_ allows
  // allocations into the old gen, which might boost the credited memory.
  const uint64_t externalBytesBefore = externalBytes();

  auto &segBuckets = segmentBuckets_[sweepIterator_.segNumber];

  // Clear the head pointers and remove this segment from the segment level
  // freelists, so that we can construct a new freelist. The
  // freelistBucketBitArray_ will be updated after the segment is swept. The
  // bits will be inconsistent with the actual freelist for the duration of
  // sweeping, but this is fine because gcMutex_ is held during the entire
  // period.
  for (size_t bucket = 0; bucket < kNumFreelistBuckets; bucket++) {
    auto *segBucket = &segBuckets[bucket];
    if (segBucket->head) {
      segBucket->removeFromFreelist();
      segBucket->head = nullptr;
    }
  }

  // If allocChunk is in this segment, it may get coalesced, so delete it first.
  if (segments_[sweepIterator_.segNumber].contains(allocChunk_))
    allocChunk_ = nullptr;

  char *freeRangeStart = nullptr, *freeRangeEnd = nullptr;
  size_t mergedCells = 0;
  int32_t segmentSweptBytes = 0;
  auto &seg = segments_[sweepIterator_.segNumber];
  for (GCCell *cell = reinterpret_cast<GCCell *>(seg.start()),
              *end = reinterpret_cast<GCCell *>(seg.level());
       cell < end;
       cell = cell->nextCell()) {
    assert(cell->isValid() && "Invalid cell in sweeping");
    if (AlignedHeapSegment::getCellMarkBit(cell)) {
      // Cannot concurrently trim storage. Technically just checking
      // backgroundThread would suffice, but the kConcurrentGC lets us compile
      // away this check in incremental mode.
      // Also don't trim during a YG collection: it may complete an OG
      // collection while evacuating (see OldGen::allocSlow), and its heap walks
      // (e.g. scanDirtyCardsForSegment, finalizeCompactee) rely on the sizes of
      // cells not changing. In incremental mode, this also skips trimming in
      // the sweeps done by yieldToOldGen.
      if ((kConcurrentGC && backgroundThread) || gc_.ygCollectionStats_)
        continue;
      const uint32_t cellSize = cell->getAllocatedSize();
      const uint32_t trimmedSize =
          cell->getVT()->getTrimmedSize(cell, cellSize);
      assert(cellSize >= trimmedSize && "Growing objects is not supported.");
      assert(
          trimmedSize >= minAllocationSize() &&
          "Trimmed object must still meet minimum size.");
      assert(
          isSizeHeapAligned(trimmedSize) &&
          "Trimmed size must also be heap aligned");
      const uint32_t trimmableBytes = cellSize - trimmedSize;

      // If this cell has extra space we can trim, trim it.
      if (LLVM_UNLIKELY(trimmableBytes >= minAllocationSize())) {
        static_cast<VariableSizeRuntimeCell *>(cell)->setSizeFromGC(
            trimmedSize);
        GCCell *newCell = cell->nextCell();
        // Just create a FillerCell, the next iteration will free it.
        constructCell<FillerCell>(newCell, trimmableBytes);
        assert(
            !AlignedHeapSegment::getCellMarkBit(newCell) &&
            "Trimmed space cannot be marked");
        FixedSizeHeapSegment::setCellHead(newCell, trimmableBytes);
#ifndef NDEBUG
        sweepIterator_.trimmedBytes += trimmableBytes;
#endif
      }
      continue;
    }

    const auto sz = cell->getAllocatedSize();
    char *const cellCharPtr = reinterpret_cast<char *>(cell);

    if (freeRangeEnd != cellCharPtr) {
      assert(
          freeRangeEnd < cellCharPtr &&
          "Should not overshoot the start of an object");
      // We are starting a new free range, flush the previous one.
      if (LLVM_LIKELY(freeRangeStart))
        addCellToFreelistFromSweep(
            freeRangeStart, freeRangeEnd, segBuckets, mergedCells > 1);

      mergedCells = 0;
      freeRangeEnd = freeRangeStart = cellCharPtr;
    }
    // Expand the current free range to include the current cell.
    freeRangeEnd += sz;
    mergedCells++;

    if (vmisa<FreelistCell>(cell))
      continue;

    segmentSweptBytes += sz;
    // Cell is dead, run its finalizer first if it has one.
    cell->getVT()->finalizeIfExists(cell, gc_);
    if (isTracking && !vmisa<FillerCell>(cell)) {
      gc_.untrackObject(cell, sz);
    }
  }

  // Flush any free range that was left over.
  if (freeRangeStart)
    addCellToFreelistFromSweep(
        freeRangeStart, freeRangeEnd, segBuckets, mergedCells > 1);

  // Update the segment level freelists for any buckets that this segment has
  // free cells for.
  for (size_t bucket = 0; bucket < kNumFreelistBuckets; ++bucket) {
    auto *segBucket = &segBuckets[bucket];
    if (segBucket->head)
      segBucket->addToFreelist(&buckets_[bucket]);

    // In case sweeping has changed the availability of a bucket, update the
    // overall bit array. Note that this is necessary even if segBucket->head is
    // null, as the bits were not updated when the freelist for this segment was
    // erased prior to sweeping.
    freelistBucketBitArray_.set(bucket, buckets_[bucket].next);
  }

  // Correct the allocated byte count.
  decrementAllocatedBytes(segmentSweptBytes);
  sweepIterator_.sweptBytes += segmentSweptBytes;
  sweepIterator_.sweptExternalBytes += externalBytesBefore - externalBytes();

  // There are more iterations to go.
  if (sweepIterator_.segNumber)
    return true;

  endSweep();
  return false;
}

void HadesGC::OldGen::endSweep() {
  assert(!sweepIterator_.segNumber && "All segments should have been swept.");
  // In the end of sweeping, free any dead jumbo segments. Note that this does
  // not necessarily happen here, just a convenient place to do it.
  freeUnusedJumboSegments();

  // This was the last sweep iteration, finish the collection.
  auto &stats = *gc_.ogCollectionStats_;

  auto sweptBytes = sweepIterator_.sweptBytes;
  auto preAllocated = stats.beforeAllocatedBytes();
  if (sweptBytes > preAllocated) {
    // Only trimming can result in freeing more memory than was allocated at the
    // start of the collection, since we may trim cells that were allocated
    // after the collection started.
    assert(sweepIterator_.trimmedBytes >= (sweptBytes - preAllocated));
    // We can't precisely calculate how much of the trimmed memory came from
    // cells allocated during the collection, so just cap the swept bytes at the
    // number of initially allocated bytes.
    sweptBytes = preAllocated;
  }
  stats.setSweptBytes(sweptBytes);
  stats.setSweptExternalBytes(sweepIterator_.sweptExternalBytes);
  const uint64_t targetSizeBytes =
      (stats.afterAllocatedBytes() + stats.afterExternalBytes()) /
      gc_.occupancyTarget_;

  // In a very large heap, use the configured max heap size as a backstop to
  // prevent the target size crossing it (which would delay collection and cause
  // an OOM). This is just an approximation, a precise accounting would subtract
  // segment metadata and YG memory.
  uint64_t clampedSizeBytes = std::min(targetSizeBytes, gc_.maxHeapSize_);
  targetSizeBytes_.update(clampedSizeBytes);
  sweepIterator_ = {};
}

void HadesGC::OldGen::initializeSweep() {
  assert(
      !sweepIterator_.segNumber && !sweepIterator_.sweptBytes &&
      !sweepIterator_.sweptExternalBytes && "Sweep is already in progress.");
  sweepIterator_.segNumber = segments_.size();
}

size_t HadesGC::OldGen::sweepSegmentsRemaining() const {
  return sweepIterator_.segNumber;
}

size_t HadesGC::OldGen::getMemorySize() const {
  size_t memorySize = segments_.size() * sizeof(FixedSizeHeapSegment);
  memorySize += segmentBuckets_.size() * sizeof(SegmentBuckets);
  return memorySize;
}

// Assume about 30% of the YG will survive initially.
constexpr double kYGInitialSurvivalRatio = 0.3;

HadesGC::OldGen::OldGen(HadesGC &gc) : gc_(gc) {}

HadesGC::HadesGC(
    GCCallbacks &gcCallbacks,
    PointerBase &pointerBase,
    const GCConfig &gcConfig,
    std::shared_ptr<CrashManager> crashMgr,
    std::shared_ptr<StorageProvider> provider,
    experiments::VMExperimentFlags vmExperimentFlags)
    : GCBase(
          gcCallbacks,
          pointerBase,
          gcConfig,
          std::move(crashMgr),
          HeapKind::HadesGC),
      maxHeapSize_{clampMaxHeapSize(gcConfig.getMaxHeapSize())},
      provider_(std::move(provider)),
      oldGen_{*this},
      backgroundExecutor_{
          kConcurrentGC ? std::make_unique<Executor>() : nullptr},
      promoteYGToOG_{!gcConfig.getAllocInYoung()},
      revertToYGAtTTI_{gcConfig.getRevertToYGAtTTI()},
      overwriteDeadYGObjects_{gcConfig.getOverwriteDeadYGObjects()},
      occupancyTarget_(gcConfig.getOccupancyTarget()),
      ygAverageSurvivalBytes_{/*weight*/ 0.5,
                              /*init*/ kYGInitialSizeFactor *
                                  FixedSizeHeapSegment::maxSize() *
                                  kYGInitialSurvivalRatio} {
  (void)vmExperimentFlags;
  std::lock_guard<Mutex> lk(gcMutex_);
  crashMgr_->setCustomData("HermesGC", getKindAsStr().c_str());
  // createSegment relies on member variables and should not be called until
  // they are initialised.
  llvh::ErrorOr<FixedSizeHeapSegment> newYoungGen = createSegment();
  if (!newYoungGen)
    hermes_fatal("Failed to initialize the young gen", newYoungGen.getError());
  setYoungGen(std::move(newYoungGen.get()));
  const size_t initHeapSize = std::max<uint64_t>(
      gcConfig.getInitHeapSize(), FixedSizeHeapSegment::maxSize());
  oldGen_.setTargetSizeBytes(initHeapSize - FixedSizeHeapSegment::maxSize());
}

HadesGC::~HadesGC() {
  crashMgr_->removeCustomData("HermesGC");
  assert(
      concurrentPhase_ == Phase::None &&
      "Must call finalizeAll before destructor.");
}

void HadesGC::getHeapInfo(HeapInfo &info) {
  std::lock_guard<Mutex> lk{gcMutex_};
  GCBase::getHeapInfo(info);
  info.allocatedBytes = allocatedBytes();
  info.heapSize = segmentFootprint();
  info.numCompactions = numCompactions_;
  // If YG isn't empty, its bytes haven't been accounted for yet, add them here.
  info.totalAllocatedBytes = totalAllocatedBytes_ + youngGen().used();
  info.va = info.heapSize;
  info.externalBytes = oldGen_.externalBytes() + getYoungGenExternalBytes();
  info.numLargeAllocations = oldGen_.numLargeAllocations();
  info.allocatedLargeObjectBytes = oldGen_.allocatedLargeObjectBytes();
  info.youngGenStats = ygCumulativeStats_;
  info.fullStats = ogCumulativeStats_;
}

void HadesGC::getHeapInfoWithMallocSize(HeapInfo &info) {
  // Get the usual heap info.
  getHeapInfo(info);
  // Get GCBase's malloc size estimate.
  GCBase::getHeapInfoWithMallocSize(info);
  std::lock_guard<Mutex> lk{gcMutex_};
  // First add the usage by the runtime's roots.
  info.mallocSizeEstimate += gcCallbacks_.mallocSize();
  // Scan all objects for their malloc size. This operation is what makes
  // getHeapInfoWithMallocSize O(heap size).
  forAllObjs([&info](GCCell *cell) {
    info.mallocSizeEstimate += cell->getVT()->getMallocSize(cell);
  });
}

void HadesGC::getCrashManagerHeapInfo(
    CrashManager::HeapInformation &crashInfo) {
  HeapInfo info;
  getHeapInfo(info);
  crashInfo.size_ = info.heapSize;
  crashInfo.used_ = info.allocatedBytes;
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
void HadesGC::createSnapshot(llvh::raw_ostream &os, bool captureNumericValue) {
  std::lock_guard<Mutex> lk{gcMutex_};
  // No allocations are allowed throughout the entire heap snapshot process.
  NoAllocScope scope{*this};
  // Let any existing collections complete before taking the snapshot.
  waitForCollectionToFinish("snapshot");
  {
    GCCycle cycle{*this, "GC Heap Snapshot"};
    GCBase::createSnapshot(*this, os, captureNumericValue);
  }
}

void HadesGC::snapshotAddGCNativeNodes(HeapSnapshot &snap) {
  GCBase::snapshotAddGCNativeNodes(snap);
  if (nativeIDs_.ygFinalizables == IDTracker::kInvalidNode) {
    nativeIDs_.ygFinalizables = getIDTracker().nextNativeID();
  }
  if (nativeIDs_.og == IDTracker::kInvalidNode) {
    nativeIDs_.og = getIDTracker().nextNativeID();
  }
  snap.beginNode();
  snap.endNode(
      HeapSnapshot::NodeType::Native,
      "std::vector<GCCell *>",
      nativeIDs_.ygFinalizables,
      youngGenFinalizables_.capacity() * sizeof(GCCell *),
      0);
  snap.beginNode();
  snap.endNode(
      HeapSnapshot::NodeType::Native,
      "OldGen",
      nativeIDs_.og,
      sizeof(oldGen_) + oldGen_.getMemorySize(),
      0);
}

void HadesGC::snapshotAddGCNativeEdges(HeapSnapshot &snap) {
  GCBase::snapshotAddGCNativeEdges(snap);
  // All native ids should be lazily initialized in snapshotAddGCNativeNodes,
  // because that is called first.
  assert(nativeIDs_.ygFinalizables != IDTracker::kInvalidNode);
  assert(nativeIDs_.og != IDTracker::kInvalidNode);
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Internal,
      "youngGenFinalizables",
      nativeIDs_.ygFinalizables);
  snap.addNamedEdge(HeapSnapshot::EdgeType::Internal, "oldGen", nativeIDs_.og);
}

void HadesGC::enableHeapProfiler(
    std::function<void(
        uint64_t,
        std::chrono::microseconds,
        std::vector<GCBase::AllocationLocationTracker::HeapStatsUpdate>)>
        fragmentCallback) {
  std::lock_guard<Mutex> lk{gcMutex_};
  // Let any existing collections complete before enabling the profiler.
  waitForCollectionToFinish("heap profiler enable");
  GCBase::enableHeapProfiler(std::move(fragmentCallback));
}

void HadesGC::disableHeapProfiler() {
  std::lock_guard<Mutex> lk{gcMutex_};
  // Let any existing collections complete before disabling the profiler.
  waitForCollectionToFinish("heap profiler disable");
  GCBase::disableHeapProfiler();
}

void HadesGC::enableSamplingHeapProfiler(
    size_t samplingInterval,
    int64_t seed) {
  std::lock_guard<Mutex> lk{gcMutex_};
  // Let any existing collections complete before enabling the profiler.
  waitForCollectionToFinish("sampling heap profiler enable");
  GCBase::enableSamplingHeapProfiler(samplingInterval, seed);
}

void HadesGC::disableSamplingHeapProfiler(llvh::raw_ostream &os) {
  std::lock_guard<Mutex> lk{gcMutex_};
  // Let any existing collections complete before disabling the profiler.
  waitForCollectionToFinish("sampling heap profiler disable");
  GCBase::disableSamplingHeapProfiler(os);
}
#endif // HERMES_MEMORY_INSTRUMENTATION

void HadesGC::printStats(JSONEmitter &json) {
  // Ensure we can safely access any internal stats.
  std::lock_guard<Mutex> lk{gcMutex_};
  GCBase::printStats(json);
  json.emitKey("specific");
  json.openDict();
  json.emitKeyValue("collector", getKindAsStr());
  json.emitKey("stats");
  json.openDict();
  json.emitKeyValue("Num compactions", numCompactions_);
  json.emitKeyValue("Num large allocations", oldGen_.numLargeAllocations());
  json.emitKeyValue(
      "Current heap bytes of large allocation",
      oldGen_.allocatedLargeObjectBytes());
  json.emitKeyValue("Num young gen collections", numYoungCollections_);
  json.emitKeyValue("Num old gen collections", numOldCollections_);
  json.closeDict();
  json.closeDict();
}

std::string HadesGC::getKindAsStr() const {
  return kGCName;
}

void HadesGC::collect(std::string cause, bool /*canEffectiveOOM*/) {
  // Wait for any existing collections to finish before starting a new one.
  std::lock_guard<Mutex> lk{gcMutex_};
  // Disable the YG promotion mode. A forced GC via collect will do a full
  // collection immediately anyway, so there's no need to avoid collecting YG.
  // This is especially important when the forced GC is a memory warning.
  promoteYGToOG_ = false;
  waitForCollectionToFinish(cause);
  // This function should block until a collection finishes.
  // YG needs to be empty in order to do an OG collection.
  youngGenCollection(cause, /*forceOldGenCollection*/ true);
  // Wait for the collection to complete.
  waitForCollectionToFinish(cause);
  // Start a second YG collection to complete any pending compaction.
  // Since YG is empty, this will only be evacuating the compactee.
  // Note that it's possible for this call to start another OG collection if the
  // occupancy target is >= 75%. That doesn't break the contract of this
  // function though, and we don't want to bother with waiting for that
  // collection to complete because it won't find any garbage anyway.
  youngGenCollection(std::move(cause), /*forceOldGenCollection*/ false);
}

void HadesGC::collectWithOptions(
    std::string &&cause,
    const GCOptions &options) {
  if (options.type == GCType::Major &&
      options.executionPolicy == GCExecutionPolicy::Sync) {
    // Run full collection on the mutator thread.
    collect(std::move(cause));
    return;
  }

  std::lock_guard<Mutex> lk = ensureBackgroundTaskPaused();
  // Major collections should reclaim YG garbage even in promotion mode.
  if (options.type == GCType::Major) {
    promoteYGToOG_ = false;
  }
  // For minor GC, we do YG collection and let it determine if we should start
  // a new OG collection cycle. For async Major GC, we force a new OG
  // collection.
  youngGenCollection(
      std::move(cause),
      /*forceOldGenCollection*/ options.type == GCType::Major);
}

void HadesGC::waitForCollectionToFinish(std::string cause) {
  assert(
      gcMutex_ &&
      "gcMutex_ must be held before calling waitForCollectionToFinish");
  if (concurrentPhase_ == Phase::None) {
    return;
  }
  GCCycle cycle{*this, "GC Old Gen (Direct)"};

  llvh::Optional<CollectionStats> waitingStats;
  if (ygCollectionStats_) {
    ygCollectionStats_->addCollectionType("waiting");
  } else {
    waitingStats.emplace(*this, std::move(cause), "waiting");
    waitingStats->beginCPUTimeSection();
    waitingStats->setBeginTime();
  }

  while (concurrentPhase_ != Phase::None)
    incrementalCollect(false);

  if (waitingStats) {
    waitingStats->endCPUTimeSection();
    waitingStats->setEndTime();
    recordGCStats(
        std::move(*waitingStats).getEvent(),
        /* onMutator */ true,
        /* fromNewCollection */ false);
  }
}

void HadesGC::oldGenCollection(std::string cause, bool forceCompaction) {
  // Full collection:
  //  * Mark all live objects by iterating through a worklist.
  //  * Sweep dead objects onto the free lists.
  // This function must be called while the gcMutex_ is held.
  assert(gcMutex_ && "gcMutex_ must be held when starting an OG collection");
  assert(
      gcMutex_.depth() == 1 &&
      "Need ability to release mutex in oldGenCollection.");
  assert(
      concurrentPhase_ == Phase::None &&
      "Starting a second old gen collection");
  // Wait for any lingering background task to finish.
  if (kConcurrentGC) {
    // This is just making sure that any leftover work is completed before
    // starting a new collection. Since concurrentPhase_ == None here, there is
    // no collection ongoing. However, the background task may need to acquire
    // the lock in order to observe the value of concurrentPhase_.
    std::unique_lock<Mutex> lk{gcMutex_, std::adopt_lock};
    ogPauseCondVar_.wait(lk, [this] { return !backgroundTaskActive_; });
    lk.release();
  }
  // We know ygCollectionStats_ exists because oldGenCollection is only called
  // by youngGenCollection.
  ygCollectionStats_->addCollectionType("old gen start");
#ifdef HERMES_SLOW_DEBUG
  checkWellFormed();
#endif
  ogCollectionStats_ =
      std::make_unique<CollectionStats>(*this, std::move(cause), "old");
  // NOTE: Leave CPU time as zero if the collection isn't concurrent, as the
  // times aren't useful.
  if (kConcurrentGC)
    ogCollectionStats_->beginCPUTimeSection();
  ogCollectionStats_->setBeginTime();
  ogCollectionStats_->setBeforeSizes(
      oldGen_.allocatedBytes(), oldGen_.externalBytes(), segmentFootprint());

  // If we've reached the first OG collection, switch back to YG mode.
  promoteYGToOG_ = false;

  // First, clear any mark bits that were set by a previous collection or
  // direct-to-OG allocation, they aren't needed anymore.
  oldGen_.forAllSegments(
      [](auto &&seg) { seg.markBitArray().reset(); },
      [](auto &&seg) { seg.markBitArray().reset(); });

  // Mark phase: discover all pointers that are live.
  // Initialize the marking state.
  markState_.emplace(gcCallbacks_.getSymbolsEnd());
  {
    MarkAcceptor acceptor(*this);
    // Roots are marked before a marking thread is spun up, so that the root
    // marking is atomic.
    DroppingAcceptor<MarkAcceptor> nameAcceptor{acceptor};
    markRoots(nameAcceptor, /*markLongLived*/ true);
    // Do not call markWeakRoots here, as weak roots can only be cleared
    // after liveness is known.
  }

  concurrentPhase_ = Phase::Mark;
  // Before the thread starts up, make sure that any write barriers are aware
  // that concurrent marking is happening.
  ogMarkingBarriers_ = true;
  // prepareCompactee must be called before the new thread is spawned, in order
  // to ensure that write barriers start operating immediately, and that any
  // objects promoted during an intervening YG collection are correctly scanned.
  prepareCompactee(forceCompaction);

  // Setup the sweep iterator when collection begins, because the number of
  // segments can change if a YG collection interleaves. There's no need to
  // sweep those extra segments since they are full of newly promoted
  // objects from YG (which have all their mark bits set), thus the sweep
  // iterator doesn't need to be updated. We also don't need to sweep any
  // segments made since the start of the collection, since they won't have
  // any unmarked objects anyway.
  // NOTE: this must be called after prepareCompactee, which may remove segments
  // from the heap.
  oldGen_.initializeSweep();

  if (!kConcurrentGC) {
    // 32-bit system: 64-bit HermesValues cannot be updated in one atomic
    // instruction. Have YG collections interleave marking work.
    // In this version of the system, the mark stack will be drained in batches
    // during each YG collection. Once the mark stack is fully drained, the rest
    // of the collection finishes while blocking a YG GC. This allows
    // incremental work without actually using multiple threads.

    // Initialize the drain rate.
    updateDrainRate();
    return;
  }
  ogCollectionStats_->endCPUTimeSection();
  // 64-bit system: 64-bit HermesValues can be updated in one atomic
  // instruction. Start up a separate thread for doing marking work.
  // NOTE: Since the "this" value (the HadesGC instance) is copied to the
  // executor, the task must not outlive the GC. finalizeAll() ends any ongoing
  // collection, so a task that is still queued, or paused on ogPauseCondVar_,
  // at that point does no more work. When ~HadesGC destroys
  // backgroundExecutor_, ~Executor runs any queued task and joins the thread.
  collectOGInBackground();
  // Use concurrentPhase_ to be able to tell when the collection finishes.
}

void HadesGC::collectOGInBackground() {
  assert(gcMutex_ && "Must hold GC mutex when scheduling background work.");
  assert(
      !backgroundTaskActive_ && "Should only have one active task at a time");
  assert(kConcurrentGC && "Background work can only be done in concurrent GC");
  // Only Mark and Sweep phases can actually run in the background task.
  assert(concurrentPhase_ == Phase::Mark || concurrentPhase_ == Phase::Sweep);

  backgroundTaskActive_ = true;

  backgroundExecutor_->add([this]() {
    std::unique_lock<Mutex> lk(gcMutex_);
    while (true) {
      // If the mutator has requested the background task to stop and yield
      // gcMutex_, wait on ogPauseCondVar_ until the mutator acquires the mutex
      // and signals that we may resume.
      ogPauseCondVar_.wait(
          lk, [this] { return !ogPaused_.load(std::memory_order_relaxed); });
      assert(
          backgroundTaskActive_ &&
          "backgroundTaskActive_ must be true when the background task is in the loop.");
      incrementalCollect(true);
      if (concurrentPhase_ == Phase::None ||
          concurrentPhase_ == Phase::CompleteMarking) {
        backgroundTaskActive_ = false;
        // Signal the mutator to let it know that this task is complete, in case
        // it is waiting.
        ogPauseCondVar_.notify_one();
        break;
      }
    }
  });
}

std::lock_guard<Mutex> HadesGC::pauseBackgroundTask() {
  assert(kConcurrentGC && "Should not be called in incremental mode");
  assert(!calledByBackgroundThread() && "Must be called from mutator");
  // Signal to the background thread that it should stop and wait on
  // ogPauseCondVar_.
  ogPaused_.store(true, std::memory_order_relaxed);
  // Acquire gcMutex_ as soon as it is released by the background thread.
  gcMutex_.lock();
  // Signal to the background thread that it may resume. Note that it will just
  // go to wait on gcMutex_, since it is currently held by this thread.
  ogPaused_.store(false, std::memory_order_relaxed);
  ogPauseCondVar_.notify_one();
  return std::lock_guard(gcMutex_, std::adopt_lock);
}

void HadesGC::incrementalCollect(bool backgroundThread) {
  assert(gcMutex_ && "Must hold the GC mutex when calling incrementalCollect");
  switch (concurrentPhase_) {
    case Phase::None:
      break;
    case Phase::Mark: {
      if (!kConcurrentGC && ygCollectionStats_)
        ygCollectionStats_->addCollectionType("marking");
      // Drain some work from the mark worklist. If the work has finished
      // completely, move on to CompleteMarking.
      constexpr size_t kConcurrentMarkLimit = 8192;
      size_t markLimit =
          kConcurrentGC ? kConcurrentMarkLimit : markState_->byteDrainRate;
      if (!incrementalMark(markLimit))
        concurrentPhase_ = Phase::CompleteMarking;
      break;
    }
    case Phase::CompleteMarking:
      // Background task should exit, the mutator will restart it after the STW
      // pause.
      if (!backgroundThread) {
        if (ygCollectionStats_)
          ygCollectionStats_->addCollectionType("complete marking");
        completeMarking();
        concurrentPhase_ = Phase::Sweep;
      }
      break;
    case Phase::Sweep:
      if (!kConcurrentGC && ygCollectionStats_)
        ygCollectionStats_->addCollectionType("sweeping");
      // Calling oldGen_.sweepNext() will sweep the next segment.
      if (!oldGen_.sweepNext(backgroundThread)) {
        // Finish any collection bookkeeping.
        ogCollectionStats_->setEndTime();
        ogCollectionStats_->setAfterSize(segmentFootprint());
        concurrentPhase_ = Phase::None;
        ++numOldCollections_;
      }
      break;
    default:
      llvm_unreachable("No other possible state between iterations");
  }
}

void HadesGC::prepareCompactee(bool forceCompaction) {
  assert(gcMutex_);
  assert(
      compactee_.empty() &&
      "Ongoing compaction at the start of an OG collection.");
  if (promoteYGToOG_)
    return;

#if HERMESVM_SANITIZE_HANDLES != 0
  // Handle-SAN forces a compaction to move some OG objects.
  if (sanitizeRate_)
    forceCompaction = true;
#endif

  // To avoid compacting too often, keep a buffer of one segment or 5% of the
  // heap (whichever is greater). Since the selected segment will be removed
  // from the heap, we only want to compact if there are at least 2 segments in
  // the OG.
  uint64_t buffer = std::max<uint64_t>(
      oldGen_.targetSizeBytes() / 20, FixedSizeHeapSegment::maxSize());
  uint64_t threshold = oldGen_.targetSizeBytes() + buffer;
  uint64_t totalBytes = oldGen_.size() + oldGen_.externalBytes();
  if ((forceCompaction || totalBytes > threshold) &&
      oldGen_.numSegments() > 1) {
    compactee_.segment =
        std::make_shared<FixedSizeHeapSegment>(oldGen_.popSegment());
    addSegmentExtentToCrashManager(
        *compactee_.segment,
        FixedSizeHeapSegment::storageSize(),
        kCompacteeNameForCrashMgr);
    compactee_.start = compactee_.segment->lowLim();
    compactee_.startCP = CompressedPointer::encodeNonNull(
        reinterpret_cast<GCCell *>(compactee_.segment->lowLim()),
        getPointerBase());
  }
}

void HadesGC::finalizeCompactee() {
  char *stop = compactee_.segment->level();
  char *cur = compactee_.segment->start();
  PointerBase &base = getPointerBase();
  // Calculate the total number of bytes that were allocated in the compactee at
  // the start of compaction.
  int32_t preAllocated = 0;
  while (cur < stop) {
    auto *cell = reinterpret_cast<GCCell *>(cur);
    if (cell->hasMarkedForwardingPointer()) {
      auto size = cell->getMarkedForwardingPointer()
                      .getNonNull(base)
                      ->getAllocatedSize();
      preAllocated += size;
      cur += size;
    } else {
      auto size = cell->getAllocatedSize();
      if (!vmisa<OldGen::FreelistCell>(cell)) {
        cell->getVT()->finalizeIfExists(cell, *this);
        preAllocated += size;
      }
      cur += size;
    }
  }
  // At this point, any cells that survived compaction are already accounted for
  // separately in the counter, so we just need to subtract the number of bytes
  // allocated in the compactee.
  oldGen_.decrementAllocatedBytes(preAllocated);

#ifndef NDEBUG
  unitSegmentAddrMap_.erase(compactee_.segment->lowLim());
#endif
  const size_t segIdx = AlignedHeapSegment::getSegmentIndexFromStart(
      compactee_.segment->lowLim());
  segmentIndices_.push_back(segIdx);
  removeSegmentExtentFromCrashManager(std::to_string(segIdx));
  removeSegmentExtentFromCrashManager(kCompacteeNameForCrashMgr);
  compactee_ = {};
}

void HadesGC::updateOldGenThreshold() {
  const double markedBytes = markState_->markedBytes;
  const double preAllocated = ogCollectionStats_->beforeAllocatedBytes();
  assert(markedBytes <= preAllocated && "Cannot mark more than was allocated");
  const double postAllocated = oldGen_.allocatedBytes();
  assert(postAllocated >= preAllocated && "Cannot free memory during marking");

  // Calculate the number of bytes marked for each byte allocated into the old
  // generation. Note that this is skewed heavily by the number of young gen
  // collections that occur during marking, and therefore the size of the heap.
  // 1. In small heaps, this underestimates the true mark rate, because marking
  // may finish shortly after it starts, but we have to wait until the next YG
  // collection is complete. This is desirable, because we need a more
  // conservative margin in small heaps to avoid running over the heap limit.
  // 2. In large heaps, this approaches the true mark rate, because there will
  // be several young gen collections, giving us more accurate and finer grained
  // information on the allocation rate.
  const auto concurrentMarkRate =
      markedBytes / std::max(postAllocated - preAllocated, 1.0);

  // If the heap is completely full and we are running into blocking
  // collections, then it is possible that almost nothing is allocated into the
  // OG during the mark phase. Without correction, can become a self-reinforcing
  // cycle because it will cause the mark rate to be overestimated, making
  // collections start later, further increasing the odds of a blocking
  // collection. Empirically, the mark rate can be much higher than the below
  // limit, but we get diminishing returns with increasing mark rate, since the
  // threshold just asymptotically approaches 1.
  const auto clampedRate = std::min(concurrentMarkRate, 20.0);

  // Update the collection threshold using the newly computed mark rate. To add
  // a margin of safety, we assume everything in the heap at the time we hit the
  // threshold needs to be marked. This margin allows for variance in the
  // marking rate, and gives time for sweeping to start. The update is
  // calculated by viewing the threshold as the bytes to mark and the remainder
  // after the threshold as the bytes to fill. We can then solve for it using:
  // MarkRate = Threshold / (1 - Threshold)
  // This has some nice properties:
  // 1. As the threshold increases, the safety margin also increases (since the
  // safety margin is just the difference between the threshold and the
  // occupancy ratio).
  // 2. It neatly fits the range [0, 1) for a mark rate in [0, infinity). There
  // is no risk of division by zero.
  ogThreshold_.update(clampedRate / (clampedRate + 1));
}

void HadesGC::markWeakMapEntrySlots() {
  bool newlyMarkedValue;
  MarkAcceptor acceptor{*this};
  do {
    newlyMarkedValue = false;
    weakMapEntrySlots_.forEach([this, &acceptor](WeakMapEntrySlot &slot) {
      if (!slot.key || !slot.owner)
        return;
      GCCell *ownerMapCell = slot.owner.getNoBarrierUnsafe(getPointerBase());
      // If the owner structure isn't reachable, no need to mark the values.
      if (!AlignedHeapSegment::getCellMarkBit(ownerMapCell))
        return;
      if (slot.key.isPointer()) {
        GCCell *cell = slot.key.getPointerNoBarrierUnsafe(getPointerBase());
        // The WeakRef object must be marked for the mapped value to
        // be marked (unless there are other strong refs to the value).
        if (!AlignedHeapSegment::getCellMarkBit(cell))
          return;
      }
      acceptor.accept(slot.mappedValue);
    });
    newlyMarkedValue = !markState_->localWorklist.empty();
    // Drain everything there is to mark.
    incrementalMark(std::numeric_limits<size_t>::max());
  } while (newlyMarkedValue);

  // If either a key or its owning map is dead, set the mapped value to Empty.
  weakMapEntrySlots_.forEach([this](WeakMapEntrySlot &slot) {
    if (!slot.key || !slot.owner) {
      slot.mappedValue = HermesValue::encodeEmptyValue();
      return;
    }
    GCCell *ownerMapCell = slot.owner.getNoBarrierUnsafe(getPointerBase());
    // We don't need to handle the case of Symbol key here since if the key is
    // valid, its symbol must be alive.
    if (slot.key.isPointer()) {
      GCCell *cell = slot.key.getPointerNoBarrierUnsafe(getPointerBase());
      if (!AlignedHeapSegment::getCellMarkBit(cell)) {
        slot.mappedValue = HermesValue::encodeEmptyValue();
        return;
      }
    }
    if (!AlignedHeapSegment::getCellMarkBit(ownerMapCell)) {
      slot.mappedValue = HermesValue::encodeEmptyValue();
    }
  });
}

void HadesGC::completeMarking() {
  assert(inGC() && "inGC_ must be set during the STW pause");
  // Update the collection threshold before marking anything more, so that only
  // the concurrently marked bytes are part of the calculation.
  updateOldGenThreshold();
  ogMarkingBarriers_ = false;
  // No locks are needed here because the world is stopped and there is only 1
  // active thread.
  flushPendingBarrierPushChunk();
  {
    MarkAcceptor acceptor{*this};
    // Remark any roots that may have changed without executing barriers.
    DroppingAcceptor<MarkAcceptor> nameAcceptor{acceptor};
    gcCallbacks_.markRootsForCompleteMarking(nameAcceptor);
  }
  // Drain the marking queue.
  incrementalMark(std::numeric_limits<size_t>::max());
  assert(isBarrierWorklistEmpty() && "Marking worklist wasn't drained");
  markWeakMapEntrySlots();
  assert(isBarrierWorklistEmpty() && "Marking worklist wasn't drained");
  // Reset weak roots to null after full reachability has been
  // determined.
  markState_->markedSymbols |= markState_->writeBarrierMarkedSymbols;

  // Now free symbols. Note that:
  // 1. We must do this before marking weak symbols, because the mark bits
  //    will be updated by this call to reflect symbols that were not actually
  //    freed.
  // 2. We call this after marking WeakMaps. This is safe even if WeakMaps
  //    have symbol keys, because the symbol keys for WeakMap are always
  //    non-lazy, so its mark bits won't be updated in below call (which
  //    calls IdentifierTable::freeUnmarkedSymbols()).
  gcCallbacks_.freeSymbols(markState_->markedSymbols);

  MarkWeakRootsAcceptor acceptor{*this, markState_->markedSymbols};
  markWeakRoots(acceptor, /*markLongLived*/ true);

  // Nothing needs markState_ from this point onward.
  markState_.reset();
}

void HadesGC::OldGen::freeUnusedJumboSegments() {
  assert(
      gc_.gcMutex_ && "GC mutex must be held when freeing dead jumbo segments");
  for (auto it = jumboSegments_.begin(); it != jumboSegments_.end();) {
    auto *cell = reinterpret_cast<GCCell *>(it->start());
    if (AlignedHeapSegment::getCellMarkBit(cell)) {
      ++it;
      continue;
    }
    // Cell must be dead now.

    auto sz = cell->getAllocatedSizeSlow();
    // Run its finalizer first if it has one.
    cell->getVT()->finalizeIfExists(cell, gc_);
    if (gc_.isTrackingIDs()) {
      gc_.untrackObject(cell, sz);
    }
    // Reflect the allocation bytes change.
    decrementAllocatedBytes(sz);
    allocatedLargeObjectBytes_ -= sz;
    sweepIterator_.sweptBytes += sz;

#ifndef NDEBUG
    // Remove the segment address mapping.
    for (auto *alignedAddr = it->lowLim(), *end = it->hiLim();
         alignedAddr != end;
         alignedAddr += AlignedHeapSegment::kSegmentUnitSize) {
      bool success = gc_.unitSegmentAddrMap_.erase(alignedAddr);
      assert(
          success &&
          "Existing JumboHeapSegment must have mapping entries in unitSegmentAddrMap_");
    }
#endif

    // Remove the entire segment.
    const size_t segIdx =
        AlignedHeapSegment::getSegmentIndexFromStart(it->lowLim());
    it = jumboSegments_.erase(it);
    gc_.segmentIndices_.push_back(segIdx);
    gc_.removeSegmentExtentFromCrashManager(std::to_string(segIdx));
  }
}

void HadesGC::finalizeAll() {
  std::lock_guard<Mutex> lk{gcMutex_};
  // Terminate any existing OG collection.
  concurrentPhase_ = Phase::None;
  if (ogCollectionStats_)
    ogCollectionStats_->markUsed();
  // In case of an OOM, we may be in the middle of a YG collection.
  if (ygCollectionStats_)
    ygCollectionStats_->markUsed();
  // Now finalize the heap.
  // We might be in the middle of a YG collection, with some objects promoted to
  // the OG, and some not. Only finalize objects that have not been promoted to
  // OG, and let the OG finalize the promoted objects.
  finalizeYoungGenObjects();
  // Remove contextual custom data entries for YG.
  removeSegmentExtentFromCrashManager(kYGNameForCrashMgr);
  removeSegmentExtentFromCrashManager(
      std::to_string(
          AlignedHeapSegment::getSegmentIndexFromStart(youngGen_.lowLim())));

  // If we are in the middle of a YG collection, some objects may have already
  // been promoted to the OG. Assume that any remaining external memory in the
  // YG belongs to those objects.
  transferExternalMemoryToOldGen();

  const auto finalizeCallback = [this](GCCell *cell) {
    assert(cell->isValid() && "Invalid cell in finalizeAll");
    cell->getVT()->finalizeIfExists(cell, *this);
  };
  if (compactee_.segment) {
    forCompactedObjsInSegment(
        *compactee_.segment, finalizeCallback, getPointerBase());
    auto segIndex = AlignedHeapSegment::getSegmentIndexFromStart(
        compactee_.segment->lowLim());
    removeSegmentExtentFromCrashManager(std::to_string(segIndex));
    removeSegmentExtentFromCrashManager(kCompacteeNameForCrashMgr);
  }

  oldGen_.forAllSegments(
      [this, &finalizeCallback](auto &&seg) {
        forAllObjsInSegment(seg, finalizeCallback);
        auto segIndex =
            AlignedHeapSegment::getSegmentIndexFromStart(seg.lowLim());
        removeSegmentExtentFromCrashManager(std::to_string(segIndex));
      },
      [this, &finalizeCallback](auto &&seg) {
        finalizeCallback(reinterpret_cast<GCCell *>(seg.start()));
        auto segIndex =
            AlignedHeapSegment::getSegmentIndexFromStart(seg.lowLim());
        removeSegmentExtentFromCrashManager(std::to_string(segIndex));
      });
}

void HadesGC::creditExternalMemory(GCCell *cell, uint32_t sz) {
  assert(canAllocExternalMemory(sz) && "Precondition");
  if (inYoungGen(cell)) {
    size_t newYGExtBytes = getYoungGenExternalBytes() + sz;
    setYoungGenExternalBytes(newYGExtBytes);
    youngGen_.chargeExternalMemory(sz);
  } else {
    std::lock_guard<Mutex> lk{gcMutex_};
    oldGen_.creditExternalMemory(sz);
    uint64_t totalBytes = oldGen_.allocatedBytes() + oldGen_.externalBytes();
    if (totalBytes > oldGen_.targetSizeBytes())
      youngGen_.setEffectiveEnd(youngGen_.level());
  }
}

void HadesGC::debitExternalMemory(GCCell *cell, uint32_t sz) {
  if (inYoungGen(cell)) {
    size_t oldYGExtBytes = getYoungGenExternalBytes();
    assert(
        oldYGExtBytes >= sz && "Debiting more native memory than was credited");
    size_t newYGExtBytes = oldYGExtBytes - sz;
    setYoungGenExternalBytes(newYGExtBytes);
    // Give back the room that the YG's remaining external memory no longer
    // accounts for, so that memory released before the next YG collection
    // doesn't bring that collection forward. A debit from a finalizer runs
    // during a YG collection or finalizeAll, each of which sets the effective
    // end afterwards.
    youngGen_.capExternalMemoryCharge(newYGExtBytes);
  } else {
    std::lock_guard<Mutex> lk{gcMutex_};
    oldGen_.debitExternalMemory(sz);
  }
}

void HadesGC::constructorWriteBarrierRangeSlow(
    const GCCell *owningObj,
    const GCHermesValueBase *start,
    uint32_t numHVs) {
  assert(
      reinterpret_cast<const char *>(owningObj) <=
          reinterpret_cast<const char *>(start) &&
      reinterpret_cast<const char *>(start + numHVs) <=
          (reinterpret_cast<const char *>(owningObj) +
           owningObj->getAllocatedSizeSlow()) &&
      "Range must start and end within the owning object.");

  // Most constructors should be running in the YG, so in the common case, we
  // can avoid doing anything for the whole range. If the range is in the OG,
  // then just dirty all the cards corresponding to it, and we can scan them for
  // pointers later. This is less precise but makes the write barrier faster.

  AlignedHeapSegment::dirtyCardsForAddressRange(
      owningObj, start, start + numHVs);
}

void HadesGC::constructorWriteBarrierRangeSlow(
    const GCCell *owningObj,
    const GCSmallHermesValueBase *start,
    uint32_t numHVs) {
  assert(
      reinterpret_cast<const char *>(owningObj) <=
          reinterpret_cast<const char *>(start) &&
      reinterpret_cast<const char *>(start + numHVs) <=
          (reinterpret_cast<const char *>(owningObj) +
           owningObj->getAllocatedSizeSlow()) &&
      "Range must start and end within the owning object.");

  AlignedHeapSegment::dirtyCardsForAddressRange(
      owningObj, start, start + numHVs);
}

void HadesGC::snapshotWriteBarrierRangeSlow(
    const GCHermesValueBase *start,
    uint32_t numHVs) {
  for (uint32_t i = 0; i < numHVs; ++i) {
    snapshotWriteBarrierInternal(start[i]);
  }
}

void HadesGC::snapshotWriteBarrierRangeSlow(
    const GCSmallHermesValueBase *start,
    uint32_t numHVs) {
  for (uint32_t i = 0; i < numHVs; ++i) {
    snapshotWriteBarrierInternal(start[i]);
  }
}

void HadesGC::snapshotWriteBarrierInternal(GCCell *oldValue) {
  assert(
      (oldValue->isValid()) &&
      "Invalid cell encountered in snapshotWriteBarrier");
  if (!inYoungGen(oldValue)) {
    HERMES_SLOW_ASSERT(
        dbgContains(oldValue) &&
        "Non-heap pointer encountered in snapshotWriteBarrier");
    barrierEnqueue(oldValue);
  }
}

void HadesGC::snapshotWriteBarrierInternal(CompressedPointer oldValue) {
  assert(
      (oldValue.get(getPointerBase())->isValid()) &&
      "Invalid cell encountered in snapshotWriteBarrier");
  if (!inYoungGen(oldValue)) {
    GCCell *ptr = oldValue.get(getPointerBase());
    HERMES_SLOW_ASSERT(
        dbgContains(ptr) &&
        "Non-heap pointer encountered in snapshotWriteBarrier");
    barrierEnqueue(ptr);
  }
}

void HadesGC::snapshotWriteBarrierInternal(HermesValue oldValue) {
  if (oldValue.isPointer()) {
    snapshotWriteBarrierInternal(static_cast<GCCell *>(oldValue.getPointer()));
  } else if (oldValue.isSymbol()) {
    // Symbols need snapshot write barriers.
    snapshotWriteBarrierInternal(oldValue.getSymbol());
  }
}

void HadesGC::snapshotWriteBarrierInternal(SmallHermesValue oldValue) {
  if (oldValue.isPointer()) {
    snapshotWriteBarrierInternal(oldValue.getPointer());
  } else if (oldValue.isSymbol()) {
    // Symbols need snapshot write barriers.
    snapshotWriteBarrierInternal(oldValue.getSymbol());
  }
}

void HadesGC::snapshotWriteBarrierInternal(SymbolID symbol) {
  HERMES_SLOW_ASSERT(
      ogMarkingBarriers_ &&
      "snapshotWriteBarrier should only be called while the OG is marking");
  const uint32_t idx = symbol.unsafeGetIndex();
  // Ignore symbols that are invalid or were allocated after marking started.
  if (symbol.isInvalid() || idx >= markState_->writeBarrierMarkedSymbols.size())
    return;
  markState_->writeBarrierMarkedSymbols[idx] = true;
}

bool HadesGC::canAllocExternalMemory(uint32_t size) {
  return size <= maxHeapSize_;
}

template <typename FixedSizeSegmentCallBack, typename JumboSegmentCallBack>
void HadesGC::OldGen::forAllSegments(
    FixedSizeSegmentCallBack fixedSizeSegCallback,
    JumboSegmentCallBack jumboSegCallback) {
  for (FixedSizeHeapSegment &seg : getSegments())
    fixedSizeSegCallback(seg);
  for (JumboHeapSegment &seg : getJumboSegments())
    jumboSegCallback(seg);
}

void HadesGC::forAllObjs(const std::function<void(GCCell *)> &callback) {
  std::lock_guard<Mutex> lk{gcMutex_};
  forAllObjsInSegment(youngGen(), callback);

  const auto skipGarbageCallback = [callback](GCCell *cell) {
    // If we're doing this check during sweeping, or between sweeping and
    // compaction, there might be some objects that are dead, and could
    // potentially have garbage in them. There's no need to check the
    // pointers of those objects.
    if (AlignedHeapSegment::getCellMarkBit(cell)) {
      callback(cell);
    }
  };
  oldGen_.forAllSegments(
      [this, &skipGarbageCallback, &callback](auto &&seg) {
        if (concurrentPhase_ != Phase::Sweep)
          forAllObjsInSegment(seg, callback);
        else
          forAllObjsInSegment(seg, skipGarbageCallback);
      },
      [this, &skipGarbageCallback, &callback](auto &&seg) {
        if (concurrentPhase_ != Phase::Sweep)
          callback(reinterpret_cast<GCCell *>(seg.start()));
        else
          skipGarbageCallback(reinterpret_cast<GCCell *>(seg.start()));
      });
  if (compactee_.segment) {
    // We have to skip unmarked objects in the compactee if marking has ended,
    // as described in skipGarbageCallback above.
    if (concurrentPhase_ != Phase::Sweep && concurrentPhase_ != Phase::None)
      forAllObjsInSegment(*compactee_.segment, callback);
    else
      forAllObjsInSegment(*compactee_.segment, skipGarbageCallback);
  }
}

void HadesGC::ttiReached() {
  if (revertToYGAtTTI_)
    promoteYGToOG_ = false;
}

#ifndef NDEBUG

bool HadesGC::calledByBackgroundThread() const {
  // If the background thread is active, check if this thread matches the
  // background thread.
  return kConcurrentGC &&
      backgroundExecutor_->getThreadId() == std::this_thread::get_id();
}

bool HadesGC::validPointer(const void *p) const {
  return dbgContains(p) && static_cast<const GCCell *>(p)->isValid();
}

bool HadesGC::dbgContains(const void *p) const {
  return inYoungGen(p) || inOldGen(p);
}

bool HadesGC::inOldGen(const void *p) const {
  auto alignedAddr = reinterpret_cast<const char *>(llvh::alignDown(
      reinterpret_cast<uintptr_t>(p), AlignedHeapSegment::kSegmentUnitSize));
  // If it isn't in any OG segment or the compactee, or in a JumboHeapSegment,
  // then this pointer is not considered in the OG.
  return alignedAddr != youngGen_.lowLim() &&
      unitSegmentAddrMap_.find(alignedAddr) != unitSegmentAddrMap_.end();
}

void HadesGC::trackReachable(CellKind kind, unsigned sz) {}

bool HadesGC::needsWriteBarrier(const GCHermesValueBase *loc, HermesValue value)
    const {
  // Values in the YG never need a barrier.
  if (inYoungGen(loc))
    return false;
  // If the old value is a pointer or a symbol, a snapshot barrier is needed.
  if (loc->isPointer() || loc->isSymbol())
    return true;
  // If the new value is a pointer, a relocation barrier is needed.
  if (value.isPointer())
    return true;
  return false;
}
bool HadesGC::needsWriteBarrierInCtor(
    const GCHermesValueBase *loc,
    HermesValue value) const {
  // Values in the YG never need a barrier.
  if (inYoungGen(loc))
    return false;
  // If the new value is a pointer, a relocation barrier is needed.
  if (value.isPointer())
    return true;
  return false;
}
bool HadesGC::needsWriteBarrier(
    const GCSmallHermesValueBase *loc,
    SmallHermesValue value) const {
  // Values in the YG never need a barrier.
  if (inYoungGen(loc))
    return false;
  // If the old value is a pointer or a symbol, a snapshot barrier is needed.
  if (loc->isPointer() || loc->isSymbol())
    return true;
  // If the new value is a pointer, a relocation barrier is needed.
  if (value.isPointer())
    return true;
  return false;
}
bool HadesGC::needsWriteBarrierInCtor(
    const GCSmallHermesValueBase *loc,
    SmallHermesValue value) const {
  // Values in the YG never need a barrier.
  if (inYoungGen(loc))
    return false;
  // If the new value is a pointer, a relocation barrier is needed.
  if (value.isPointer())
    return true;
  return false;
}
bool HadesGC::needsWriteBarrier(const GCPointerBase *loc, GCCell *value) const {
  return !inYoungGen(loc);
}
#endif

template void *HadesGC::allocSlow<CanBeLarge::Yes, MayFail::Yes>(uint32_t sz);
template void *HadesGC::allocSlow<CanBeLarge::Yes, MayFail::No>(uint32_t sz);
template void *HadesGC::allocSlow<CanBeLarge::No, MayFail::No>(uint32_t sz);
// We intentionally don't instantiate it with CanBeLarge::No and MayFail::Yes,
// as we currently don't allow normal allocation to fail instead of OOM.

template <CanBeLarge canBeLarge, MayFail mayFail>
void *HadesGC::allocSlow(uint32_t sz) {
  // Check if it's a large allocation before YG collection.
  if (LLVM_UNLIKELY(sz > FixedSizeHeapSegment::maxSize())) {
    if constexpr (canBeLarge == CanBeLarge::Yes)
      return oldGen_.allocLarge<mayFail>(sz);

    // We can't allocate an object with size > FixedSizeHeapSegment::maxSize()
    // in the YG. If this happens, it would be an error in VM code to ever allow
    // such a size to be allocated without enabling large allocation. This case
    // is for production, if we miss a test case.
    oom(make_error_code(OOMError::SuperSegmentAlloc));
  }

  AllocResult res;
  // On slow path for CanBeLarge::Yes, we should have already held the lock.
  if constexpr (canBeLarge == CanBeLarge::No) {
    auto lk = ensureBackgroundTaskPaused();
    // Failed to alloc in young gen, do a young gen collection.
    youngGenCollection(
        kNaturalCauseForAnalytics, /*forceOldGenCollection*/ false);
  } else {
    // Failed to alloc in young gen, do a young gen collection.
    youngGenCollection(
        kNaturalCauseForAnalytics, /*forceOldGenCollection*/ false);
  }
  res = youngGen().alloc(sz);
  if (res.success)
    return res.ptr;

  // Still fails after YG collection, perhaps the alloc size is large, try
  // growing the YG to full size.
  youngGen().resetEffectiveEnd();
  res = youngGen().alloc(sz);
  assert(
      res.success &&
      "Allocation of size smaller than or equal to FixedSizeHeapSegment::maxSize() should never fail");
  return res.ptr;
}

template <MayFail mayFail>
GCCell *HadesGC::OldGen::allocLarge(uint32_t sz) {
  assert(
      sz > FixedSizeHeapSegment::maxSize() &&
      "Large allocation must have size larger than FixedSizeHeapSegment::maxSize()");
  assert(gc_.gcMutex_ && "gcMutex_ must be held before calling allocLarge()");
  size_t segmentSize = JumboHeapSegment::computeSegmentSize(sz);
  llvh::ErrorOr<JumboHeapSegment> seg = gc_.createJumboSegment(segmentSize);
  if (!seg) {
    // Can't create any new segments, wait for an OG collection to finish and
    // possibly free some segments.
    gc_.waitForCollectionToFinish("full heap");
    // Try the allocation again.
    seg = gc_.createJumboSegment(segmentSize);
    if (!seg) {
      // Return nullptr and let the caller handle it if Fail is allowed.
      if constexpr (mayFail == MayFail::Yes) {
        return nullptr;
      }
      // The GC didn't recover enough memory, OOM.
      // Re-use the error code from the earlier heap segment allocation, because
      // it's either that the max heap size was reached, or that segment failed
      // to allocate.
      gc_.oom(seg.getError());
    }
  }

  GCCell *newObj = static_cast<GCCell *>(seg->alloc());
  auto maxSize = seg->maxSize();
  jumboSegments_.emplace_back(std::move(seg.get()));
  gc_.totalAllocatedBytes_ += maxSize;
  numLargeAllocations_++;
  allocatedLargeObjectBytes_ += maxSize;
  return finishAlloc(newObj, maxSize);
}

template void *HadesGC::allocLongLived<CanBeLarge::Yes, MayFail::Yes>(
    uint32_t sz);
template void *HadesGC::allocLongLived<CanBeLarge::Yes, MayFail::No>(
    uint32_t sz);
template void *HadesGC::allocLongLived<CanBeLarge::No, MayFail::No>(
    uint32_t sz);
// We intentionally don't instantiate it with CanBeLarge::No and MayFail::Yes,
// as we currently don't allow normal allocation to fail instead of OOM.

template <CanBeLarge canBeLarge, MayFail mayFail>
void *HadesGC::allocLongLived(uint32_t sz) {
  assert(
      isSizeHeapAligned(sz) &&
      "Call to allocLongLived must use a size aligned to HeapAlign");
  assert(gcMutex_ && "GC mutex must be held when calling allocLongLived");
  // Alloc directly into the old gen when canBeLarge is No or it can fit in a
  // FixedSizeHeapSegment.
  if ((canBeLarge == CanBeLarge::No) ||
      LLVM_LIKELY(sz <= FixedSizeHeapSegment::maxSize())) {
    // OldGen::alloc does not internally increment totalAllocatedBytes, so do it
    // here.
    totalAllocatedBytes_ += sz;
    return oldGen_.alloc(sz);
  }
  return oldGen_.allocLarge<mayFail>(sz);
}

GCCell *HadesGC::OldGen::alloc(uint32_t sz) {
  ++NumOldGenAlloc;
  // First try to allocate out of the allocChunk_.
  if (LLVM_LIKELY(
          allocChunk_ &&
          allocChunk_->getAllocatedSize() >= sz + minAllocationSize())) {
    auto *newCell = allocChunk_->carve(sz);
    __asan_unpoison_memory_region(newCell, sz);
    return finishAlloc(newCell, sz);
  }
  ++NumOldGenAllocSlow;
  return allocSlow(sz);
}

GCCell *HadesGC::OldGen::allocSlow(uint32_t sz) {
  assert(
      isSizeHeapAligned(sz) &&
      "Should be aligned before entering this function");
  assert(sz >= minAllocationSize() && "Allocating too small of an object");
  assert(
      sz <= maxNormalAllocationSize() && "Allocating too large of an object");
  assert(gc_.gcMutex_ && "gcMutex_ must be held before calling oldGenAlloc");
  if (GCCell *cell = search(sz)) {
    return cell;
  }

  /// Allocate from this newly created segment.
  auto allocFromNewSegment = [this, sz](FixedSizeHeapSegment seg) {
    // Complete this allocation using a bump alloc.
    AllocResult res = seg.alloc(sz);
    assert(
        res.success &&
        "A newly created segment should always be able to allocate");
    // Set the cell head for any successful alloc, so that write barriers can
    // move from dirty cards to the head of the object.
    FixedSizeHeapSegment::setCellHead(static_cast<GCCell *>(res.ptr), sz);
    // Add the segment to segments_ and add the remainder of the segment to
    // the free list.
    addSegment(std::move(seg));
    GCCell *newObj = static_cast<GCCell *>(res.ptr);
    AlignedHeapSegment::setCellMarkBit(newObj);
    return newObj;
  };

  // Before waiting for a collection to finish, check if we're below the max
  // heap size and can simply allocate another segment. This will prevent
  // blocking the YG unnecessarily.
  llvh::ErrorOr<FixedSizeHeapSegment> seg = gc_.createSegment();
  if (seg) {
    return allocFromNewSegment(std::move(seg.get()));
  }

  // Can't expand to any more segments, wait for an old gen collection to
  // finish and possibly free up memory.
  gc_.waitForCollectionToFinish("full heap");

  // Repeat the search in case the collection did free memory.
  if (GCCell *cell = search(sz)) {
    return cell;
  }
  // It's possible that the collection freed some JumboHeapSegments, so try
  // to create a new segment again.
  seg = gc_.createSegment();
  if (seg) {
    return allocFromNewSegment(std::move(seg.get()));
  }

  // The GC didn't recover enough memory, OOM.
  // Re-use the error code from the earlier heap segment allocation, because
  // it's either that the max heap size was reached, or that segment failed to
  // allocate.
  gc_.oom(seg.getError());
}

uint32_t HadesGC::OldGen::getFreelistBucket(uint32_t size) {
  // If the size corresponds to the "small" portion of the freelist, then the
  // bucket is just (size) / (heap alignment)
  if (size < kMinSizeForLargeBlock) {
    auto bucket = size >> LogHeapAlign;
    assert(
        bucket < kNumSmallFreelistBuckets &&
        "Small blocks must be within the small free list range");
    return bucket;
  }
  // Otherwise, index into the large portion of the freelist, which is based on
  // powers of 2

  auto bucket =
      kNumSmallFreelistBuckets + llvh::Log2_32(size) - kLogMinSizeForLargeBlock;
  assert(
      bucket < kNumFreelistBuckets &&
      "Block size must be within the freelist range!");
  return bucket;
}

GCCell *HadesGC::OldGen::search(uint32_t sz) {
  // Once we're examining the rest of the free list, it's a first-fit algorithm.
  // This approach approximates finding the smallest possible fit.
  //
  // A free cell serves sz either as an exact fit or by splitting off a
  // remainder of at least minAllocationSize(); a cell sized strictly between
  // the two is dead, being neither takeable nor splittable. So when the exact
  // bucket is empty, resume at bucket(sz + minAllocationSize()) rather than
  // examine every cell of a bucket that cannot possibly satisfy the request.
  //
  // That skips a bucket only where minAllocationSize() > HeapAlign, which
  // today means HV64 alone: there minAllocationSize() is 2 * HeapAlign, so
  // exactly one small bucket is passed over and it holds cells of size
  // sz + HeapAlign, dead by the above. Under HV32 minAllocationSize() equals
  // HeapAlign and no bucket lies strictly between the two. Large buckets are
  // never passed over in either mode: sz + minAllocationSize() <= 2 * sz
  // bounds bucket(sz + minAllocationSize()) at bucket(sz) + 1, and bucket(sz)
  // is empty whenever this resume point is used.
  size_t bucket = getFreelistBucket(sz);
  if (!freelistBucketBitArray_.at(bucket))
    bucket = freelistBucketBitArray_.findNextSetBitFrom(
        getFreelistBucket(sz + minAllocationSize()));
  for (; bucket < kNumFreelistBuckets;
       bucket = freelistBucketBitArray_.findNextSetBitFrom(bucket + 1)) {
    auto *segBucket = buckets_[bucket].next;
    do {
      // Need to track the previous entry in order to change the next pointer.
      AssignableCompressedPointer *prevLoc = &segBucket->head;
      AssignableCompressedPointer cellCP = segBucket->head;

      do {
        auto *cell =
            vmcast<FreelistCell>(cellCP.getNonNull(gc_.getPointerBase()));
        assert(
            cellCP == *prevLoc &&
            "prevLoc should be updated in each iteration");
        assert(
            (!cell->next_ ||
             cell->next_.getNonNull(gc_.getPointerBase())->isValid()) &&
            "Next pointer points to an invalid cell");
        const auto cellSize = cell->getAllocatedSize();
        assert(
            getFreelistBucket(cellSize) == bucket &&
            "Found an incorrectly sized block in this bucket");
        // Check if the size is large enough that the cell could be split.
        if (cellSize >= sz + minAllocationSize()) {
          // Remove the free cell from the freelist.
          removeCellFromFreelist(prevLoc, bucket, segBucket);
          // Split the free cell. In order to avoid initializing
          // soon-to-be-unused values like the size and the next pointer, copy
          // the return path here.
          auto newCell = cell->carve(sz);

          // We are going to populate a new allocChunk_, so if one already
          // exists, return it to the freelist. It may make sense to only
          // selectively replace the existing allocChunk_ based on some
          // heuristic in the future.
          if (allocChunk_) {
            addCellToFreelist(
                allocChunk_,
                allocChunkBaseBucket_ +
                    getFreelistBucket(allocChunk_->getAllocatedSize()));
          }
          // The remainder of the free cell will serve as the new allocChunk_.
          allocChunk_ = cell;
          ASAN_POISON_FREE_CELL(allocChunk_);
          // Since the buckets for each segment are stored contiguously in
          // memory, we can compute the base bucket relative to the current one.
          allocChunkBaseBucket_ = segBucket - bucket;
          return finishAlloc(newCell, sz);
        } else if (cellSize == sz) {
          // Exact match, take it.
          removeCellFromFreelist(prevLoc, bucket, segBucket);
          return finishAlloc(cell, sz);
        }
        // Non-exact matches, or anything just barely too small to fit, will
        // need to find another block.
        // NOTE: This is due to restrictions on the minimum cell size to keep
        // the heap parseable, especially in debug mode. If this minimum size
        // becomes smaller (smaller header, size becomes part of it
        // automatically, debug magic field is handled differently), this
        // decisions can be re-examined. An example alternative is to make a
        // special fixed-size cell that is only as big as an empty GCCell. That
        // alternative only works if the empty is small enough to fit in any gap
        // in the heap. That's not true in debug modes currently.
        prevLoc = &cell->next_;
        cellCP = cell->next_;
      } while (cellCP);
      segBucket = segBucket->next;
    } while (segBucket);
  }
  return nullptr;
}

template <bool CompactionEnabled>
uint64_t HadesGC::youngGenEvacuateImpl(bool doCompaction) {
  assert((!doCompaction || CompactionEnabled) && "Compaction is disabled");
  EvacAcceptor<CompactionEnabled> acceptor{*this, doCompaction};
  // Marking each object puts it onto an embedded free list.
  {
    DroppingAcceptor nameAcceptor{acceptor};
    markRoots(nameAcceptor, /*markLongLived*/ doCompaction);
  }

  // Mark the values in WeakMap entries as roots for the purposes of young gen
  // collection. This is slightly suboptimal since some of the keys or maps
  // may be dead, but we still end up evacuating their corresponding value.
  // This is done for simplicity, since it lets us avoid needing to iterate to
  // a fixed point as is done during a full collection.
  weakMapEntrySlots_.forEach([&acceptor](WeakMapEntrySlot &slot) {
    acceptor.accept(slot.mappedValue);
  });

  // Find old-to-young pointers, as they are considered roots for YG
  // collection.
  scanDirtyCards(acceptor, doCompaction);
  // Iterate through the copy list to find new pointers.
  auto &pb = getPointerBase();
  while (acceptor.copyListHead) {
    // Pop copyCell from the copy list.
    auto *copyCell = (CopyListCell *)acceptor.copyListHead.getNonNull(pb);
    assert(
        AlignedHeapSegment::getCellMarkBit(copyCell) &&
        "Discovered unmarked object");
    acceptor.copyListHead = copyCell->next_;

    assert(
        copyCell->hasMarkedForwardingPointer() && "Discovered unmarked object");
    assert(
        (inYoungGen(copyCell) ||
         (doCompaction && compactee_.contains(copyCell))) &&
        "Unexpected object in YG collection");
    // Update the pointers inside the forwarded object, since the old
    // object is only there for the forwarding pointer.
    GCCell *const cell = copyCell->getMarkedForwardingPointer().getNonNull(pb);
    acceptor.setCurrentCell(cell);
    markCell(acceptor, cell);
  }

  // Mark weak roots. We only need to update the long lived weak roots if we are
  // evacuating part of the OG.
  markWeakRoots(acceptor, /*markLongLived*/ doCompaction);

  return acceptor.evacuatedBytes();
}

void HadesGC::youngGenCollection(
    std::string cause,
    bool forceOldGenCollection) {
  assert(gcMutex_ && "gcMutex_ must be held during YG collection");
  assert(
      gcMutex_.depth() == 1 &&
      "gcMutex should be not reentered in YG collection");
  // The YG is not parseable while a collection is occurring.
  assert(!inGC() && "Cannot be in GC at the start of YG!");
  ygCollectionStats_ = std::make_unique<CollectionStats>(*this, cause, "young");
  ygCollectionStats_->beginCPUTimeSection();
  ygCollectionStats_->setBeginTime();
  ++numYoungCollections_;
  GCCycle cycle{*this, "GC Young Gen"};
#ifdef HERMES_SLOW_DEBUG
  checkWellFormed();
  // Check that the card tables are well-formed before the collection.
  verifyCardTable();
#endif
  assert(
      youngGen().markBitArray().findNextZeroBitFrom(0) ==
          youngGen().markBitArray().size() &&
      "Young gen segment must have all mark bits set");
  struct {
    uint64_t before{0};
    uint64_t after{0};
  } heapBytes, externalBytes;
  heapBytes.before = youngGen().used();
  externalBytes.before = getYoungGenExternalBytes();
  // YG is about to be emptied, add all of the allocations.
  totalAllocatedBytes_ += youngGen().used();

  const bool doCompaction = compactee_.segment &&
      (concurrentPhase_ == Phase::Sweep || concurrentPhase_ == Phase::None);

  // Attempt to promote the YG segment to OG if the flag is set. If this call
  // fails for any reason, proceed with a GC.
  if (promoteYoungGenToOldGen()) {
    // Leave sweptBytes and sweptExternalBytes as defaults (which are 0).
    // Don't update the average YG survival ratio since no liveness was
    // calculated for the promotion case.
    ygCollectionStats_->setBeforeSizes(
        heapBytes.before, externalBytes.before, segmentFootprint());
    ygCollectionStats_->addCollectionType("promotion");
    assert(!doCompaction && "Cannot do compactions during YG promotions.");
  } else {
    auto &yg = youngGen();

    if (compactee_.segment) {
      // The remaining bytes after the collection is just the number of bytes
      // that were evacuated.
      heapBytes.after = youngGenEvacuateImpl<true>(doCompaction);
    } else {
      heapBytes.after = youngGenEvacuateImpl<false>(false);
    }
    // Inform trackers about objects that died during this YG collection.
    if (isTrackingIDs()) {
      auto trackerCallback = [this](GCCell *cell) {
        // The compactee might have free list cells, which are not tracked.
        // untrackObject requires the object to have been tracked previously.
        // So skip free list cells here.
        if (!vmisa<OldGen::FreelistCell>(cell)) {
          untrackObject(cell, cell->getAllocatedSize());
        }
      };
      forCompactedObjsInSegment(yg, trackerCallback, getPointerBase());
      if (doCompaction) {
        forCompactedObjsInSegment(
            *compactee_.segment, trackerCallback, getPointerBase());
      }
    }
    // Run finalizers for young gen objects.
    finalizeYoungGenObjects();
    // This was modified by debitExternalMemoryFromFinalizer, called by
    // finalizers. The difference in the value before to now was the swept bytes
    externalBytes.after = getYoungGenExternalBytes();

    if (overwriteDeadYGObjects_)
      memset(yg.start(), kInvalidHeapValue, yg.used());

    // Now the copy list is drained, and all references point to the old
    // gen. Clear the level of the young gen.
    yg.resetLevel();
    assert(
        yg.markBitArray().findNextZeroBitFrom(0) == yg.markBitArray().size() &&
        "Young gen segment must have all mark bits set");

    if (doCompaction) {
      numCompactions_++;
      ygCollectionStats_->addCollectionType("compact");
      // We can use the total amount of external memory in the OG before and
      // after running finalizers to measure how much external memory has been
      // released.
      uint64_t ogExternalBefore = oldGen_.externalBytes();
      // Similarly, finalizeCompactee will update the allocated bytes counter to
      // remove bytes allocated in the compactee.
      uint64_t ogAllocatedBefore = oldGen_.allocatedBytes();
      finalizeCompactee();
      heapBytes.before += ogAllocatedBefore - oldGen_.allocatedBytes();
      const uint64_t externalCompactedBytes =
          ogExternalBefore - oldGen_.externalBytes();
      // Since we can't track the actual number of external bytes that were in
      // this segment, just use the swept external byte count.
      externalBytes.before += externalCompactedBytes;
    }

    // Move external memory accounting from YG to OG as well.
    transferExternalMemoryToOldGen();

    // Potentially resize the YG if this collection did not meet our pause time
    // goals. Exclude compacting collections and the portion of YG time spent on
    // incremental OG collections, since they distort pause times and are
    // unaffected by YG size.
    if (!doCompaction)
      updateYoungGenSizeFactor();

    // The effective end of our YG is no longer accurate for multiple reasons:
    // 1. transferExternalMemoryToOldGen resets the effectiveEnd to be the end.
    // 2. Creating a large alloc in the YG can increase the effectiveEnd.
    // 3. The duration of this collection may not have met our pause time goals.
    youngGen().setEffectiveEnd(
        youngGen().start() +
        static_cast<size_t>(ygSizeFactor_ * FixedSizeHeapSegment::maxSize()));

    // We have to set these after the collection, in case a compaction took
    // place and updated these metrics.
    ygCollectionStats_->setBeforeSizes(
        heapBytes.before, externalBytes.before, segmentFootprint());

    // The swept bytes are just the bytes that were not evacuated.
    ygCollectionStats_->setSweptBytes(heapBytes.before - heapBytes.after);
    ygCollectionStats_->setSweptExternalBytes(
        externalBytes.before - externalBytes.after);
    ygCollectionStats_->setAfterSize(segmentFootprint());
    // If this is not a compacting YG, update the average survival bytes.
    // In a compacting YG, since the evacuatedBytes counter tracks both
    // segments, this value is not a useful predictor of future collections.
    if (!doCompaction)
      ygAverageSurvivalBytes_.update(
          ygCollectionStats_->afterAllocatedBytes() +
          ygCollectionStats_->afterExternalBytes());
  }
#ifdef HERMES_SLOW_DEBUG
  // Check that the card tables are well-formed after the collection.
  verifyCardTable();
#endif
  // Perform any pending work for an ongoing OG collection.
  // Do this before starting a new collection in case we need collections
  // back-to-back. Also, don't check this after starting a collection to avoid
  // waiting for something that is both unlikely, and will increase the pause
  // time if it does happen.
  yieldToOldGen();
  // We can only consider starting a new OG collection if any previous OG
  // collection is fully completed and has not left a pending compaction. This
  // handles the rare case where an OG collection was completed during this YG
  // collection, and the compaction will therefore only be completed in the next
  // collection.
  if (concurrentPhase_ == Phase::None && !compactee_.segment) {
    // There is no OG collection running, check the tripwire in case this is the
    // first YG after an OG completed.
    checkTripwireAndSubmitStats();
    if (forceOldGenCollection) {
      // If an OG collection is being forced, it's because something called
      // collect directly, most likely from a memory warning. In order to
      // respond to memory pressure effectively, the OG should compact.
      oldGenCollection(std::move(cause), /*forceCompaction*/ true);
    } else {
      // If the OG is sufficiently full after the collection finishes, begin
      // an OG collection.
      // External bytes are part of the numerator and denominator, because they
      // should not be included as part of determining the heap's occupancy, but
      // instead just influence when collections begin.
      const uint64_t totalAllocated =
          oldGen_.allocatedBytes() + oldGen_.externalBytes();
      const uint64_t totalBytes = oldGen_.targetSizeBytes();
      double allocatedRatio = static_cast<double>(totalAllocated) / totalBytes;
      if (allocatedRatio >= ogThreshold_) {
        oldGenCollection(kNaturalCauseForAnalytics, /*forceCompaction*/ false);
      }
    }
  }
#ifdef HERMES_SLOW_DEBUG
  // Run a well-formed check before exiting.
  checkWellFormed();
#endif
  ygCollectionStats_->setEndTime();
  ygCollectionStats_->endCPUTimeSection();
  auto statsEvent = std::move(*ygCollectionStats_).getEvent();
  recordGCStats(statsEvent, /* onMutator */ true, /* fromNewCollection */ true);
  recordGCStats(
      statsEvent,
      &ygCumulativeStats_,
      /* onMutator */ true,
      /* fromNewCollection */ true);
  ygCollectionStats_.reset();
}

bool HadesGC::promoteYoungGenToOldGen() {
  if (!promoteYGToOG_) {
    return false;
  }

  // Attempt to create a new segment, if that fails, turn off the flag to
  // disable GC and return false so we proceed with a GC.
  // TODO: Add more stringent criteria for turning off this flag, for instance,
  // once the heap reaches a certain size. That would avoid growing the heap to
  // the maximum possible size before stopping promotions.
  llvh::ErrorOr<FixedSizeHeapSegment> newYoungGen = createSegment();
  if (!newYoungGen) {
    promoteYGToOG_ = false;
    return false;
  }

  // Move the external memory costs to the OG. Needs to happen here so that the
  // YG segment moved to OG is not left with an effective end.
  transferExternalMemoryToOldGen();
  // The flag is on to prevent GC until TTI. Promote the whole YG segment
  // directly into OG.
  // Before promoting it, set the cell heads correctly for the segment
  // going into OG. This could be done at allocation time, but at a cost
  // to YG alloc times for a case that might not come up.
  forAllObjsInSegment(youngGen_, [](GCCell *cell) {
    FixedSizeHeapSegment::setCellHead(cell, cell->getAllocatedSize());
  });
  // It is important that this operation is just a move of pointers to
  // segments. The addresses have to stay the same or else it would
  // require a marking pass through all objects.
  // This will also rename the segment in the crash data.
  oldGen_.addSegment(setYoungGen(std::move(newYoungGen.get())));

  return true;
}

FixedSizeHeapSegment HadesGC::setYoungGen(FixedSizeHeapSegment seg) {
  addSegmentExtentToCrashManager(
      seg, FixedSizeHeapSegment::storageSize(), kYGNameForCrashMgr);
  youngGenFinalizables_.clear();
  std::swap(youngGen_, seg);
  youngGenCP_ = CompressedPointer::encodeNonNull(
      reinterpret_cast<GCCell *>(youngGen_.lowLim()), getPointerBase());
  return seg;
}

void HadesGC::checkTripwireAndSubmitStats() {
  assert(
      gcMutex_ &&
      "gcMutex must be held before calling checkTripwireAndSubmitStats");
  assert(
      concurrentPhase_ == Phase::None &&
      "Cannot check stats while OG collection is ongoing.");
  if (!ogCollectionStats_) {
    return;
  }
  const auto usedBytes = ogCollectionStats_->afterAllocatedBytes() +
      ogCollectionStats_->afterExternalBytes();
  // We use the amount of live data from after a GC completed as the minimum
  // bound of what is live.
  checkTripwire(usedBytes);
  auto event = std::move(*ogCollectionStats_).getEvent();
  recordGCStats(event, /* onMutator */ false, /* fromNewCollection */ true);
  recordGCStats(
      event,
      &ogCumulativeStats_,
      /* onMutator */ false,
      /* fromNewCollection */ true);
  ogCollectionStats_.reset();
}

void HadesGC::transferExternalMemoryToOldGen() {
  oldGen_.creditExternalMemory(getYoungGenExternalBytes());
  setYoungGenExternalBytes(0);
  youngGen_.resetEffectiveEnd();
}

void HadesGC::updateYoungGenSizeFactor() {
  assert(
      ygSizeFactor_ <= 1.0 && ygSizeFactor_ >= 0.25 && "YG size out of range.");
  const auto ygDuration = ygCollectionStats_->getElapsedTime().count();
  // If the YG collection has taken less than 20% of our budgeted time, increase
  // the size of the YG by 10%.
  if (ygDuration < kTargetMaxPauseMs * 0.2)
    ygSizeFactor_ = std::min(ygSizeFactor_ * 1.1, 1.0);
  // If the YG collection has taken more than 40% of our budgeted time, decrease
  // the size of the YG by 10%. This is meant to leave some time for OG work.
  // However, don't let the YG size drop below 25% of the segment size.
  else if (ygDuration > kTargetMaxPauseMs * 0.4)
    ygSizeFactor_ = std::max(ygSizeFactor_ * 0.9, 0.25);
}

template <bool CompactionEnabled>
void HadesGC::scanDirtyCardsForSegment(
    EvacAcceptor<CompactionEnabled> &acceptor,
    FixedSizeHeapSegment &seg) {
  // Use level instead of end in case the OG segment is still in bump alloc
  // mode.
  const char *const origSegLevel = seg.level();
  size_t from = seg.addressToCardIndex(seg.start());
  const size_t to = seg.addressToCardIndex(origSegLevel - 1) + 1;

  // Do not scan unmarked objects in the OG if we are currently sweeping. We do
  // this for three reasons:
  // 1. If an object is unmarked during sweeping, that means it is dead, so it
  //    is wasteful to scan it and promote objects it refers to.
  // 2. During compaction, these unmarked objects may point to dead objects in
  //    the compactee. Promoting these objects is dangerous, since they may
  //    contain references to other dead objects that are or will be freed.
  // 3. If a compaction was completed during this cycle, unmarked objects may
  //    contain references into the now freed compactee. These pointers are not
  //    safe to visit.
  const bool visitUnmarked = concurrentPhase_ != Phase::Sweep;

  while (const auto oiBegin = seg.findNextDirtyCard(from, to)) {
    const auto iBegin = *oiBegin;

    const auto oiEnd = seg.findNextCleanCard(iBegin, to);
    const auto iEnd = oiEnd ? *oiEnd : to;

    assert(
        (iEnd == to || !seg.isCardForIndexDirty(iEnd)) &&
        seg.isCardForIndexDirty(iEnd - 1) &&
        "end should either be the end of the card table, or the first "
        "non-dirty card after a sequence of dirty cards");
    assert(iBegin < iEnd && "Indices must be apart by at least one");

    const char *const begin = seg.cardIndexToAddress(iBegin);
    const char *const end = seg.cardIndexToAddress(iEnd);
    // Don't try to mark any cell past the original boundary of the segment.
    const void *const boundary = std::min(end, origSegLevel);

    // Use the object heads rather than the card table to discover the head
    // of the object.
    GCCell *const firstObj = seg.getFirstCellHead(iBegin);
    assert(
        firstObj->isValid() && "Object head doesn't point to a valid object");
    GCCell *obj = firstObj;
    // Throughout this loop, objects are being marked which could promote
    // other objects into the OG. Such objects might be promoted onto a dirty
    // card, and be visited a second time. This is only a problem if the
    // acceptor isn't idempotent. Luckily, EvacAcceptor happens to be
    // idempotent, and so there's no correctness issue with visiting an object
    // multiple times. If EvacAcceptor wasn't idempotent, we'd have to be able
    // to identify objects promoted from YG in this loop, which would be
    // expensive.

    // Mark the first object with respect to the dirty card boundaries.
    if (visitUnmarked || AlignedHeapSegment::getCellMarkBit(obj)) {
      acceptor.setCurrentCell(obj);
      markCellWithinRange(acceptor, obj, begin, end);
    }

    obj = obj->nextCell();
    // If there are additional objects in this card, scan them.
    if (LLVM_LIKELY(obj < boundary)) {
      // Mark the objects that are entirely contained within the dirty card
      // boundaries. In a given iteration, obj is the start of a given object,
      // and next is the start of the next object. Iterate until the last
      // object where next is within the card.
      for (GCCell *next = obj->nextCell(); next < boundary;
           next = next->nextCell()) {
        if (visitUnmarked || AlignedHeapSegment::getCellMarkBit(obj)) {
          acceptor.setCurrentCell(obj);
          markCell(acceptor, obj);
        }
        obj = next;
      }

      // Mark the final object in the range with respect to the dirty card
      // boundaries.
      assert(
          obj < boundary && obj->nextCell() >= boundary &&
          "Last object in card must touch or cross cross the card boundary");
      if (visitUnmarked || AlignedHeapSegment::getCellMarkBit(obj)) {
        acceptor.setCurrentCell(obj);
        markCellWithinRange(acceptor, obj, begin, end);
      }
    }

    from = iEnd;
  }
}

template <bool CompactionEnabled>
void HadesGC::scanDirtyCardsForSegment(
    EvacAcceptor<CompactionEnabled> &acceptor,
    JumboHeapSegment &seg) {
  auto *cell = reinterpret_cast<GCCell *>(seg.start());

  // If it's Sweep phase and the object is dead, don't scan it. For more
  // detailed description, please see the comments in above function.
  if (concurrentPhase_ == Phase::Sweep &&
      !AlignedHeapSegment::getCellMarkBit(cell)) {
    return;
  }

  const char *const segEnd = seg.hiLim();
  size_t from = seg.addressToCardIndex(seg.start());
  const size_t to = seg.addressToCardIndex(segEnd - 1) + 1;

  acceptor.setCurrentCell(cell);
  while (const auto oiBegin = seg.findNextDirtyCard(from, to)) {
    const auto iBegin = *oiBegin;

    const auto oiEnd = seg.findNextCleanCard(iBegin, to);
    const auto iEnd = oiEnd ? *oiEnd : to;

    assert(
        (iEnd == to || !seg.isCardForIndexDirty(iEnd)) &&
        seg.isCardForIndexDirty(iEnd - 1) &&
        "end should either be the end of the card table, or the first "
        "non-dirty card after a sequence of dirty cards");
    assert(iBegin < iEnd && "Indices must be apart by at least one");

    const char *const begin = seg.cardIndexToAddress(iBegin);
    const char *const end = seg.cardIndexToAddress(iEnd);

    markCellWithinRange(acceptor, cell, begin, end);

    from = iEnd;
  }
}

template <bool CompactionEnabled>
void HadesGC::scanDirtyCards(
    EvacAcceptor<CompactionEnabled> &acceptor,
    bool doCompaction) {
  const bool preparingCompaction = CompactionEnabled && !doCompaction;
  // The acceptors in this loop can grow the old gen by adding another
  // segment, if there's not enough room to evac the YG objects discovered.
  // Since segments are always placed at the end, we can use indices instead
  // of iterators, which aren't invalidated. It's ok to not scan newly added
  // segments, since they are going to be handled from the rest of YG
  // collection.
  const auto segEnd = oldGen_.numSegments();
  for (size_t i = 0; i < segEnd; ++i) {
    // It is safe to hold this reference across a push_back into
    // oldGen_.segments_ since references into a deque are not invalidated.
    FixedSizeHeapSegment &seg = oldGen_.getSegments()[i];
    scanDirtyCardsForSegment(acceptor, seg);
    // Do not clear the card table if the OG thread is currently marking to
    // prepare for a compaction. Note that we should clear the card tables if
    // the compaction is currently ongoing.
    if (!preparingCompaction)
      seg.clearAllCards();
  }

  for (JumboHeapSegment &seg : oldGen_.getJumboSegments()) {
    // Unlike FixedSizeHeapSegment, we cannot allocate additional jumbo segments
    // during a YG collection, so we don't need to worry about that. But in
    // principle, it should still be safe to use reference in that case because
    // jumbo segments are stored in a list instead of vector.
    scanDirtyCardsForSegment(acceptor, seg);
    if (!preparingCompaction)
      seg.clearAllCards();
  }

  // No need to search dirty cards in the compactee segment if it is
  // currently being evacuated, since it will be scanned fully.
  if (preparingCompaction)
    scanDirtyCardsForSegment(acceptor, *compactee_.segment);
}

void HadesGC::finalizeYoungGenObjects() {
  for (GCCell *cell : youngGenFinalizables_) {
    if (!cell->hasMarkedForwardingPointer()) {
      cell->getVT()->finalize(cell, *this);
    }
  }
  youngGenFinalizables_.clear();
}

uint64_t HadesGC::allocatedBytes() const {
  // This can be called very early in initialization, before YG is initialized.
  return (youngGen_ ? youngGen_.used() : 0) + oldGen_.allocatedBytes();
}

uint64_t HadesGC::externalBytes() const {
  return getYoungGenExternalBytes() + oldGen_.externalBytes();
}

uint64_t HadesGC::segmentFootprint() const {
  size_t totalNormalSegments = oldGen_.numSegments() + (youngGen_ ? 1 : 0) +
      (compactee_.segment ? 1 : 0);
  size_t totalSegmentBytes =
      totalNormalSegments * FixedSizeHeapSegment::storageSize();
  for (const JumboHeapSegment &seg : oldGen_.getJumboSegments()) {
    totalSegmentBytes += seg.storageSize();
  }
  return totalSegmentBytes;
}

uint64_t HadesGC::heapFootprint() const {
  return segmentFootprint() + externalBytes();
}

uint64_t HadesGC::OldGen::allocatedBytes() const {
  assert(gc_.gcMutex_ && "gcMutex must be held when accessing allocatedBytes_");
  return allocatedBytes_;
}

uint64_t HadesGC::OldGen::allocatedLargeObjectBytes() const {
  assert(
      gc_.gcMutex_ &&
      "gcMutex must be held when accessing allocatedLargeObjectBytes_");
  return allocatedLargeObjectBytes_;
}

unsigned HadesGC::OldGen::numLargeAllocations() const {
  assert(
      gc_.gcMutex_ &&
      "gcMutex must be held when accessing numLargeAllocations_");
  return numLargeAllocations_;
}

void HadesGC::OldGen::incrementAllocatedBytes(size_t incr) {
  allocatedBytes_ += incr;
  assert(allocatedBytes_ <= size() && "Invalid increment");
}

void HadesGC::OldGen::decrementAllocatedBytes(size_t decr) {
  assert(allocatedBytes_ >= decr && "Invalid decrement");
  allocatedBytes_ -= decr;
}

uint64_t HadesGC::OldGen::externalBytes() const {
  assert(gc_.gcMutex_ && "OG external bytes must be accessed under gcMutex_.");
  return externalBytes_;
}

uint64_t HadesGC::OldGen::size() const {
  size_t totalSegments = numSegments() + (gc_.compactee_.segment ? 1 : 0);
  size_t jumboSegBytes = 0;
  for (auto &seg : jumboSegments_) {
    jumboSegBytes += seg.storageSize();
  }
  return totalSegments * FixedSizeHeapSegment::storageSize() + jumboSegBytes;
}

uint64_t HadesGC::OldGen::targetSizeBytes() const {
  assert(gc_.gcMutex_ && "Must hold gcMutex_ when accessing targetSizeBytes_.");
  return targetSizeBytes_;
}

size_t HadesGC::getYoungGenExternalBytes() const {
  assert(
      !calledByBackgroundThread() &&
      "ygExternalBytes_ is only accessible from the mutator.");
  return ygExternalBytes_;
}
void HadesGC::setYoungGenExternalBytes(size_t sz) {
  assert(
      !calledByBackgroundThread() &&
      "ygExternalBytes_ is only accessible from the mutator.");
  ygExternalBytes_ = sz;
}

llvh::ErrorOr<size_t> HadesGC::getVMFootprintForTest() const {
  // Start by adding the YG.
  size_t footprint = 0;
  auto ygFootprint =
      hermes::oscompat::vm_footprint(youngGen_.start(), youngGen_.hiLim());
  // Check if the call failed.
  if (!ygFootprint)
    return ygFootprint;

  // Add each OG segment.
  for (const FixedSizeHeapSegment &seg : oldGen_.getSegments()) {
    auto segFootprint =
        hermes::oscompat::vm_footprint(seg.start(), seg.hiLim());
    if (!segFootprint)
      return segFootprint;
    footprint += *segFootprint;
  }
  for (const JumboHeapSegment &seg : oldGen_.getJumboSegments()) {
    auto segFootprint =
        hermes::oscompat::vm_footprint(seg.start(), seg.hiLim());
    if (!segFootprint)
      return segFootprint;
    footprint += *segFootprint;
  }
  return footprint;
}

size_t HadesGC::OldGen::numSegments() const {
  return segments_.size();
}

llvh::ErrorOr<FixedSizeHeapSegment> HadesGC::createSegment() {
  return createSegmentImpl<FixedSizeHeapSegment>(
      FixedSizeHeapSegment::storageSize(),
      [](StorageProvider *provider, const char *name, size_t segmentSize) {
        return FixedSizeHeapSegment::create(provider, name);
      });
}

llvh::ErrorOr<JumboHeapSegment> HadesGC::createJumboSegment(
    size_t segmentSize) {
  return createSegmentImpl<JumboHeapSegment>(
      segmentSize,
      [](StorageProvider *provider, const char *name, size_t segmentSize) {
        return JumboHeapSegment::create(provider, name, segmentSize);
      });
}

template <typename T, typename CreateSegFunc>
llvh::ErrorOr<T> HadesGC::createSegmentImpl(
    size_t segmentSize,
    CreateSegFunc createSegFunc) {
  // No heap size limit when Handle-SAN is on, to allow the heap enough room to
  // keep moving things around. Otherwise, if allocating this segment will
  // make the heap footprint larger than the max heap size plus one unit segment
  // size, return OOM error.
  if (!sanitizeRate_ &&
      heapFootprint() >=
          (maxHeapSize_ - segmentSize + AlignedHeapSegment::kSegmentUnitSize))
    return make_error_code(OOMError::MaxHeapReached);

  auto res = createSegFunc(provider_.get(), "hades-segment", segmentSize);
  if (!res) {
    return res.getError();
  }
  T seg(std::move(res.get()));
  // Even if compressed pointers are off, we still use the segment index for
  // crash manager indices.
  size_t segIdx;
  if (segmentIndices_.size()) {
    segIdx = segmentIndices_.back();
    segmentIndices_.pop_back();
  } else {
    segIdx = ++numSegments_;
  }
  gcCallbacks_.registerHeapSegment(segIdx, seg.lowLim());
  addSegmentExtentToCrashManager(
      seg, seg.storageSize(), std::to_string(segIdx));
#ifndef NDEBUG
  // Add this segment's address mapping.
  for (char *alignedAddr = seg.lowLim(), *end = seg.hiLim(); alignedAddr != end;
       alignedAddr += AlignedHeapSegment::kSegmentUnitSize) {
    auto inserted =
        unitSegmentAddrMap_.try_emplace(alignedAddr, seg.lowLim(), seg.hiLim());
    assert(
        inserted.second &&
        "One segment containing the same aligned address exists in unitSegmentAddrMap_");
  }
#endif
  seg.markBitArray().set();
  return llvh::ErrorOr<T>(std::move(seg));
}

void HadesGC::OldGen::addSegment(FixedSizeHeapSegment seg) {
  segments_.emplace_back(std::move(seg));
  FixedSizeHeapSegment &newSeg = segments_.back();
  incrementAllocatedBytes(newSeg.used());
  // Add a set of freelist buckets for this segment.
  segmentBuckets_.emplace_back();

  assert(
      segmentBuckets_.size() == segments_.size() &&
      "Must have as many freelists as segments.");

  // Add the remainder of the segment to the freelist.
  uint32_t sz = newSeg.available();
  if (sz >= minAllocationSize()) {
    auto res = newSeg.alloc(sz);
    assert(res.success);
    auto bucket = getFreelistBucket(sz);
    addCellToFreelist(res.ptr, sz, &segmentBuckets_.back()[bucket]);
  }
}

FixedSizeHeapSegment HadesGC::OldGen::popSegment() {
  const auto &segBuckets = segmentBuckets_.back();
  for (size_t bucket = 0; bucket < kNumFreelistBuckets; ++bucket) {
    if (segBuckets[bucket].head) {
      segBuckets[bucket].removeFromFreelist();
      freelistBucketBitArray_.set(bucket, buckets_[bucket].next);
    }
  }
  segmentBuckets_.pop_back();

  auto oldSeg = std::move(segments_.back());
  segments_.pop_back();

  // If the allocChunk_ is in the segment we are removing, clear it.
  if (oldSeg.contains(allocChunk_))
    allocChunk_ = nullptr;
  return oldSeg;
}

void HadesGC::OldGen::setTargetSizeBytes(size_t targetSizeBytes) {
  assert(gc_.gcMutex_ && "Must hold gcMutex_ when accessing targetSizeBytes_.");
  assert(!targetSizeBytes_ && "Should only initialise targetSizeBytes_ once.");
  targetSizeBytes_ = ExponentialMovingAverage(0.5, targetSizeBytes);
}

void HadesGC::yieldToOldGen() {
  assert(inGC() && "Must be in GC when yielding to old gen");

  // While we are both on the mutator and holding gcMutex_, check invariants.
#ifndef NDEBUG
  if (concurrentPhase_ == Phase::Mark) {
    assert(ogMarkingBarriers_ && "Barriers are not running");
    assert(markState_ && "Mark state is not set");
    assert((!kConcurrentGC || backgroundTaskActive_) && "BG thread is dead");
  } else if (concurrentPhase_ == Phase::CompleteMarking) {
    assert(ogMarkingBarriers_ && "Barriers are not running");
    assert(markState_ && "Mark state is not set");
  } else if (concurrentPhase_ == Phase::Sweep) {
    assert(!ogMarkingBarriers_ && "Barriers are running");
    assert(!markState_ && "Mark state is set");
    assert((!kConcurrentGC || backgroundTaskActive_) && "BG thread is dead");
  } else {
    assert(concurrentPhase_ == Phase::None && "No other phase");
    assert(!ogMarkingBarriers_ && "Barriers are running");
    assert(!markState_ && "Mark state is set");
  }
#endif

  if (!kConcurrentGC && concurrentPhase_ != Phase::None) {
    // If there is an ongoing collection, update the drain rate befor
    // collecting.
    if (concurrentPhase_ == Phase::Mark)
      updateDrainRate();

    constexpr uint32_t kYGIncrementalCollectBudget = kTargetMaxPauseMs / 2;
    const auto initialPhase = concurrentPhase_;
    // If the phase hasn't changed and we are still under 25ms after the first
    // iteration, then we can be reasonably sure that the next iteration will
    // also be <25ms, keeping us within 50ms even in the worst case.
    do {
      incrementalCollect(false);
    } while (concurrentPhase_ == initialPhase &&
             ygCollectionStats_->getElapsedTime().count() <
                 kYGIncrementalCollectBudget);

  } else if (concurrentPhase_ == Phase::CompleteMarking) {
    incrementalCollect(false);
    collectOGInBackground();
  }
}

void HadesGC::updateDrainRate() {
  // Concurrent collections don't need to use the drain rate because they
  // only yield the lock periodically to be interrupted, but otherwise will
  // continuously churn through work regardless of the rate.
  // Non-concurrent collections, on the other hand, can only make progress
  // at YG collection intervals, so they need to be configured to mark the
  // OG faster than it fills up.
  assert(!kConcurrentGC);

  // Set a fixed floor on the mark rate, regardless of the pause time budget.
  // yieldToOldGen may operate in multiples of this drain rate if it fits in
  // the budget. Pinning the mark rate in this way helps us keep the
  // dynamically computed OG collection threshold in a reasonable range. On a
  // slow device, where we can only do one iteration of this drain rate, the
  // OG threshold will be ~75%. And by not increasing the drain rate when the
  // threshold is high, we avoid having a one-way ratchet effect that hurts
  // pause times.
  constexpr size_t baseMarkRate = 3;
  uint64_t drainRate = baseMarkRate * ygAverageSurvivalBytes_;
  // In case the allocation rate is extremely low, set a lower bound to ensure
  // the collection eventually ends.
  constexpr uint64_t byteDrainRateMin = 8192;
  markState_->byteDrainRate = std::max(drainRate, byteDrainRateMin);
}

void HadesGC::addSegmentExtentToCrashManager(
    const AlignedHeapSegment &seg,
    size_t segSize,
    const std::string &extraName) {
  assert(!extraName.empty() && "extraName can't be empty");
  if (!crashMgr_) {
    return;
  }
  const std::string segmentName = name_ + ":HeapSegment:" + extraName;
  // Pointers need at most 18 characters for 0x + 16 digits for a 64-bit
  // pointer. Same for the size digits. Add an extra char for the terminator.
  constexpr unsigned kBufferMaxSize = 18 + 18 + 2;
  char segmentExtentBuffer[kBufferMaxSize];
  snprintf(
      segmentExtentBuffer,
      kBufferMaxSize,
      "%p:%p",
      seg.lowLim(),
      seg.lowLim() + segSize);
  crashMgr_->setCustomData(segmentName.c_str(), segmentExtentBuffer);

#ifdef HERMESVM_PLATFORM_LOGGING
  hermesLog(
      "HermesGC",
      "Added segment extent: %s = %s",
      segmentName.c_str(),
      segmentAddressBuffer);
#endif
}

void HadesGC::removeSegmentExtentFromCrashManager(
    const std::string &extraName) {
  assert(!extraName.empty() && "extraName can't be empty");
  if (!crashMgr_) {
    return;
  }
  const std::string segmentName = name_ + ":HeapSegment:" + extraName;
  crashMgr_->removeCustomData(segmentName.c_str());
}

#ifdef HERMES_SLOW_DEBUG
std::pair<const char *, const char *> HadesGC::getSegmentAddrRange(
    const void *addr) {
  auto *alignedAddr = reinterpret_cast<const char *>(llvh::alignDown(
      reinterpret_cast<uintptr_t>(addr), AlignedHeapSegment::kSegmentUnitSize));
  auto iter = unitSegmentAddrMap_.find(alignedAddr);
  assert(
      iter != unitSegmentAddrMap_.end() &&
      "Given addr is not in any valid segment.");
  return iter->getSecond();
}

void HadesGC::assertWriteBarrierForNormalObj(const void *loc) {
  if (currentCellKindAndSize_) {
    // If currentCellKindAndSize_ is set, the owning object is still being
    // constructed, use currentCellKindAndSize_ instead of trying to find its
    // owning object.
    assert(
        !VTable::getVTable(currentCellKindAndSize_->first)->allowLargeAlloc &&
        "This write barrier should not be called on GCCells that support large allocation");
  } else {
    // Normal objects should always be allocated in a FixedSizeHeapSegment.
    auto [start, end] = getSegmentAddrRange(loc);
    assert(
        ((end - start) == FixedSizeHeapSegment::kSize) &&
        "This write barrier does not work for GCCells larger than the size of FixedSizeHeapSegment");

    // Don't run the check with concurrent GC. Because when the background
    // thread is sweeping, it may merge freed cells and poison memory outside of
    // FreeListCell, which may be accessed by findObjectContaining() when
    // visiting next cell.
    // `findObjectContaining` relies on card boundary table, which is only valid
    // for OG, so we need to skip below check if \c loc lives in YG.
    if (!kConcurrentGC && !inYoungGen(loc)) {
      // The above check ensures that this loc must be within a
      // FixedSizeHeapSegment, so it's safe to access its card table.
      auto *obj = FixedSizeHeapSegment::findObjectContaining(loc);
      assert(
          !VTable::getVTable(obj->getKind())->allowLargeAlloc &&
          "This write barrier only works for GCCells that do not support large allocation");
    }
  }
}

void HadesGC::assertWriteBarrierForLargeObj(
    const GCCell *owningObj,
    const void *loc) {
  if (currentCellKindAndSize_) {
    // If currentCellKindAndSize_ is set, the owning object is still being
    // constructed, use currentCellKindAndSize_ instead of the kind and size of
    // owningObj.
    assert(
        VTable::getVTable(currentCellKindAndSize_->first)->allowLargeAlloc &&
        "This write barrier should only be called on GCCells that support large allocation");
    assert(
        owningObj <= loc &&
        loc < (reinterpret_cast<const char *>(owningObj) +
               currentCellKindAndSize_->second) &&
        "The owning object must contain the given heap location");
  } else {
    assert(
        VTable::getVTable(owningObj->getKind())->allowLargeAlloc &&
        "This write barrier should only be called on GCCells that support large allocation");
    assert(
        owningObj <= loc &&
        loc < (reinterpret_cast<const char *>(owningObj) +
               owningObj->getAllocatedSizeSlow()) &&
        "The owning object must contain the given heap location");
  }
}

void HadesGC::checkWellFormed() {
  CheckHeapWellFormedAcceptor acceptor(*this);
  {
    DroppingAcceptor<CheckHeapWellFormedAcceptor> nameAcceptor{acceptor};
    markRoots(nameAcceptor, true);
  }
  markWeakRoots(acceptor, /*markLongLived*/ true);
  forAllObjs([this, &acceptor](GCCell *cell) {
    assert(cell->isValid() && "Invalid cell encountered in heap");
    markCell(acceptor, cell);
  });

  weakMapEntrySlots_.forEach([](WeakMapEntrySlot &slot) {
    if (slot.mappedValue.isEmpty()) {
      assert(
          (!slot.key || !slot.owner) &&
          "Reachable entry should have not have Empty value.");
    }
  });

  oldGen_.verifyFreelists();
}

void HadesGC::OldGen::verifyFreelists() {
  assert(gc_.gcMutex_ && "gcMutex_ must be held to traverse freelists");
  // Records every SegmentBucket that has a valid freelist. This is used to
  // ensure that a SegmentBucket is chained into the freelist for a bucket if
  // and only if it is valid.
  llvh::DenseSet<const SegmentBucket *> occupiedSegBuckets;

  for (size_t seg = 0; seg < segments_.size(); seg++) {
    // Records every free cell encountered while traversing the freelists. This
    // is used to ensure that a FreelistCell is always found in a freelist.
    llvh::DenseSet<const FreelistCell *> freeCells;

    // The allocChunk_ will not be encountered in the freelists, so make sure we
    // record it.
    if (segments_[seg].contains(allocChunk_))
      freeCells.insert(allocChunk_);

    const auto &segBuckets = segmentBuckets_[seg];
    for (size_t bucket = 0; bucket < kNumFreelistBuckets; bucket++) {
      if (!segBuckets[bucket].head)
        continue;
      // Record that this segment has cells for this bucket.
      occupiedSegBuckets.insert(&segBuckets[bucket]);
      auto *cell = vmcast<FreelistCell>(
          segBuckets[bucket].head.getNonNull(gc_.pointerBase_));
      while (cell) {
        // Record each cell in the freelist.
        assert(freeCells.insert(cell).second && "Duplicate free cell");
        // Check that the cell is in the right bucket.
        assert(getFreelistBucket(cell->getAllocatedSize()) == bucket);
        cell = vmcast_or_null<FreelistCell>(cell->next_.get(gc_.pointerBase_));
      }
    }

    auto *cell = (GCCell *)segments_[seg].start();
    auto *end = (GCCell *)segments_[seg].level();
    while (cell < end) {
      if (FreelistCell *fcell = dyn_vmcast<FreelistCell>(cell))
        assert(freeCells.erase(fcell) && "Free cell not in list");
      cell = cell->nextCell();
    }
    assert(freeCells.empty() && "Free cells in list not in segment");
  }
  for (size_t bucket = 0; bucket < kNumFreelistBuckets; bucket++) {
    auto *prevBucketPtr = &buckets_[bucket];
    auto *bucketPtr = buckets_[bucket].next;
    // Element in freelistBucketBitArray_ must be set if and only if the
    // corresponding bucket is non-empty.
    assert(freelistBucketBitArray_.at(bucket) == !!bucketPtr);

    while (bucketPtr) {
      // Erase the found bucket from the set so we can assert that all occupied
      // buckets were found in the freelist.
      bool isErased = occupiedSegBuckets.erase(bucketPtr);
      assert(isErased && "Found unoccupied bucket in freelist");
      assert(bucketPtr->prev == prevBucketPtr && "Prev ptr mismatch");
      prevBucketPtr = bucketPtr;
      bucketPtr = bucketPtr->next;
    }
  }
  assert(occupiedSegBuckets.empty() && "Occupied bucket not in freelists");
}

void HadesGC::verifyCardTable() {
  assert(inGC() && "Must be in GC to call verifyCardTable");
  struct VerifyCardDirtyAcceptor final {
    HadesGC &gc;
    const GCCell *currentCell{nullptr};

    explicit VerifyCardDirtyAcceptor(HadesGC &gc) : gc(gc) {}

    void acceptHelper(void *valuePtr, void *locPtr) {
      // If marking is done, then any pointer from outside the compactee into
      // the compactee should have a dirty card.
      const bool crossRegionCompacteePtr =
          (gc.concurrentPhase_ == Phase::None ||
           gc.concurrentPhase_ == Phase::Sweep) &&
          !gc.compactee_.contains(locPtr) && gc.compactee_.contains(valuePtr);
      if (!gc.inYoungGen(locPtr) &&
          (gc.inYoungGen(valuePtr) || crossRegionCompacteePtr)) {
        assert(AlignedHeapSegment::isCardForAddressDirty(currentCell, locPtr));
      }
    }

    void accept(GCPointerBase &ptr) {
      acceptHelper(ptr.get(gc.getPointerBase()), &ptr);
    }

    void accept(GCHermesValueBase &hv) {
      if (hv.isPointer())
        acceptHelper(hv.getPointer(), &hv);
    }
    void accept(GCSmallHermesValueBase &hv) {
      if (hv.isPointer())
        acceptHelper(hv.getPointer(gc.getPointerBase()), &hv);
    }

    void accept(const GCSymbolID &hv) {}
  };

  VerifyCardDirtyAcceptor acceptor{*this};
  forAllObjs([this, &acceptor](GCCell *cell) {
    acceptor.currentCell = cell;
    markCell(acceptor, cell);
  });

  for (const FixedSizeHeapSegment &seg : oldGen_.getSegments()) {
    seg.cardBoundaryTable().verifyBoundaries(seg.start(), seg.level());
  }
  // JumboHeapSegment does not use card boundary table, so don't verify them.
}

#endif

} // namespace vm
} // namespace hermes
