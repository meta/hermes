/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_HADESGC_H
#define HERMES_VM_HADESGC_H

#include "hermes/ADT/BitArray.h"
#include "hermes/ADT/ExponentialMovingAverage.h"
#include "hermes/Support/Algorithms.h"
#include "hermes/Support/SlowAssert.h"
#include "hermes/VM/AlignedHeapSegment.h"
#include "hermes/VM/GCBase.h"
#include "hermes/VM/VMExperiments.h"

#include "llvh/ADT/SparseBitVector.h"
#include "llvh/Support/ErrorOr.h"
#include "llvh/Support/PointerLikeTypeTraits.h"
#include "llvh/Support/SaveAndRestore.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stack>
#include <thread>
#include <utility>
#include <vector>

namespace hermes {
namespace vm {

/// A GC with a young and old generation, that does concurrent marking and
/// sweeping of the old generation.
///
/// The young gen is a single contiguous-allocation space.  A
/// young-gen collection completely evacuates live objects into the
/// older generation.
///
/// The old generation is a collection of heap segments, and allocations in the
/// old gen are done with a freelist (not a bump-pointer). When the old
/// collection is nearly full, it starts a background thread that will mark all
/// objects in the old gen, and then sweep the dead ones onto freelists.
///
/// Compaction is done in the old gen on a per-segment basis.
///
/// NOTE: Currently HadesGC is only a stub, meant to get things to compile.
/// It will be filled out to actually work later.
class HadesGC final : public GCBase {
  friend struct RuntimeOffsets;

 public:
  /// Initialize the GC with the give \p gcCallbacks and \p gcConfig.
  /// maximum size.
  /// \param gcCallbacks A callback interface enabling the garbage collector to
  ///   mark roots and free symbols.
  /// \param gcConfig A struct giving, e.g., minimum, initial, and maximum heap
  /// sizes.
  /// \param provider A provider of storage to be used by segments.
  HadesGC(
      GCCallbacks &gcCallbacks,
      PointerBase &pointerBase,
      const GCConfig &gcConfig,
      std::shared_ptr<CrashManager> crashMgr,
      std::shared_ptr<StorageProvider> provider,
      experiments::VMExperimentFlags vmExperimentFlags);

  ~HadesGC() override;

  static bool classof(const GCBase *gc) {
    return gc->getKind() == HeapKind::HadesGC;
  }

  /// When HERMESVM_COMPRESSED_POINTERS is ON, clamp \p maxHeapSize to
  /// [2 * kSegmentUnitSize, 4GB - kSegmentUnitSize], since we can't go beyond
  /// 4GB with compressed pointers. Otherwise, clamp it to
  /// [2 * kSegmentUnitSize, maxHeapSize].
  gcheapsize_t clampMaxHeapSize(gcheapsize_t maxHeapSize) const {
#ifdef HERMESVM_COMPRESSED_POINTERS
    return std::max<uint64_t>(
        std::min<uint64_t>(
            // Subtract the size of a single FixedSizeHeapSegment as a safe
            // buffer.
            (1ULL << 32) - FixedSizeHeapSegment::storageSize(),
            maxHeapSize),
        // At least one YG segment and one OG segment.
        2 * FixedSizeHeapSegment::storageSize());
#else
    return std::max<uint64_t>(
        maxHeapSize,
        // At least one YG segment and one OG segment.
        2 * FixedSizeHeapSegment::storageSize());
#endif
  }

  /// The maximum size of a normal allocation. All GCCell types that do not
  /// support large allocation must be smaller than or equal to this.
  static constexpr uint32_t maxNormalAllocationSize() {
    return FixedSizeHeapSegment::maxSize();
  }

  /// The minimum allocation size (aligned to HeapAlign).
  static constexpr uint32_t minAllocationSize() {
    return heapAlignSize(
        std::max(sizeof(OldGen::FreelistCell), sizeof(CopyListCell)));
  }

  /// Get the size of \p cell that are allocated by large allocation.
  static uint32_t getLargeCellSize(const GCCell *cell) {
    assert(
        VTable::getVTable(cell->getKind())->allowLargeAlloc &&
        "Can only be called on large object");
    // For HadesGC, we query the SHSegmentInfo and returns the size of the
    // entire allocation area in that segment, which must be a JumboHeapSegment.
    // Note that we forbid getAllocatedSize() calls on objects that live in
    // JumboHeapSegments, so this method should only be necessary in places such
    // as assertions or the memory profiler or outside of GC.
    auto sz = AlignedHeapSegment::maxSize(cell);
    assert(
        sz > GCCell::maxNormalSize() &&
        "This should only be reached when size > GCCell::maxNormalSize()");
    return sz;
  }

  /// \name GCBase overrides
  /// \{

  void getHeapInfo(HeapInfo &info) override;
  void getHeapInfoWithMallocSize(HeapInfo &info) override;
  void getCrashManagerHeapInfo(CrashManager::HeapInformation &info) override;
#ifdef HERMES_MEMORY_INSTRUMENTATION
  void createSnapshot(llvh::raw_ostream &os, bool captureNumericValue) override;
  void snapshotAddGCNativeNodes(HeapSnapshot &snap) override;
  void snapshotAddGCNativeEdges(HeapSnapshot &snap) override;
  void enableHeapProfiler(
      std::function<void(
          uint64_t,
          std::chrono::microseconds,
          std::vector<GCBase::AllocationLocationTracker::HeapStatsUpdate>)>
          fragmentCallback) override;
  void disableHeapProfiler() override;
  void enableSamplingHeapProfiler(size_t samplingInterval, int64_t seed)
      override;
  void disableSamplingHeapProfiler(llvh::raw_ostream &os) override;
#endif // HERMES_MEMORY_INSTRUMENTATION
  void printStats(JSONEmitter &json) override;
  std::string getKindAsStr() const override;

  /// \}

  /// \name GC non-virtual API
  /// \{

  template <typename T, class... Args>
  static T *constructCellCanBeLarge(void *ptr, uint32_t size, Args &&...args) {
    assert(ptr && "constructCellCanBeLarge() can't be called on null ptr");
    constexpr auto kind = T::getCellKind();
    assert(
        VTable::getVTable(kind)->allowLargeAlloc &&
        "constructLargeCell() should only be used for constructing object that supports large allocation");
    auto *cell = new (ptr) T(std::forward<Args>(args)...);
    // If this cell lives in a JumboHeapSegment, its size is the segment's max
    // allocation size.
    auto cellSize = size > FixedSizeHeapSegment::maxSize()
        ? JumboHeapSegment::computeActualCellSize(size)
        : size;
    // If cellSize is larger than GCCell::maxNormalSize(), set the size bits to
    // 0.
    cell->setKindAndSize(
        {kind, cellSize > GCCell::maxNormalSize() ? 0 : cellSize});
    return cell;
  }

  /// Allocate a new cell of the specified size \p size by calling alloc.
  /// Instantiate an object of type T with constructor arguments \p args in the
  /// newly allocated cell.
  /// \return a pointer to the newly created object in the GC heap.
  template <
      typename T,
      HasFinalizer hasFinalizer,
      LongLived longLived,
      CanBeLarge canBeLarge,
      MayFail mayFail,
      class... Args>
  inline T *makeAImpl(uint32_t size, Args &&...args);

  /// Slow path of makeA() above when canBeLarge is Yes. It acquires gcMutex_
  /// (background thread needs to be paused) and calls into allocSlow(). Mark it
  /// with noinline to prevent it from getting inlined together with the fast
  /// path.
  template <
      typename T,
      HasFinalizer hasFinalizer,
      MayFail mayFail,
      class... Args>
  LLVM_ATTRIBUTE_NOINLINE T *makeASlowCanBeLarge(uint32_t size, Args &&...args);

  /// \return the maximum number of bytes that can be allocated at once in the
  /// young gen.
  static constexpr inline uint32_t maxYoungGenAllocationSize() {
    return FixedSizeHeapSegment::maxSize();
  }

  /// Allocate two young gen objects of the size \p size1 + \p size2.
  /// Calls the constructors of types T1 and T2 with the arguments \p t1Args and
  /// \p t2Args respectively.
  /// \pre the TOTAL size must be able to fit in the young gen
  ///  (can be checked with canAllocateInYoungGen(size)).
  /// \post both result pointers are in the young gen.
  /// \return a pointer to size1 size T1, and a pointer to size2 size T2.
  template <typename T1, typename T2, typename... T1Args, typename... T2Args>
  inline std::pair<T1 *, T2 *> make2YoungGenUnsafeImpl(
      uint32_t size1,
      std::tuple<T1Args...> t1Args,
      uint32_t size2,
      std::tuple<T2Args...> t2Args);

  /// Force a garbage collection cycle.
  /// (Part of general GC API defined in GCBase.h).
  void collect(std::string cause, bool canEffectiveOOM = false) override;
  void collectWithOptions(std::string &&cause, const GCOptions &options)
      override;

  /// Total number of completed YG collections.
  /// Requires exclusive runtime access.
  unsigned getNumYGGCs() const {
    return ygCumulativeStats_.numCollections;
  }

  /// Run the finalizers for all heap objects.
  void finalizeAll() override;

  /// Add some external memory cost to a cell.
  /// (Part of general GC API defined in GCBase.h).
  /// \pre canAllocExternalMemory(size) is true.
  void creditExternalMemory(GCCell *alloc, uint32_t size) override;

  /// Remove some external memory cost from a cell.
  /// (Part of general GC API defined in GCBase.h).
  void debitExternalMemory(GCCell *alloc, uint32_t size) override;

  /// \name Write Barriers
  /// \{

  /// NOTE: For all write barriers and read barriers:
  /// The call to writeBarrier/readBarrier must happen *before* the write/read
  /// to memory occurs.

  /// The given value is being written at the given loc (required to
  /// be in the heap). If value is a pointer, execute a write barrier.
  /// NOTE: The write barrier call must be placed *before* the write to the
  /// pointer, so that the current value can be fetched.
  void writeBarrier(const GCHermesValue *loc, HermesValue value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForNormalObj(loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    if (LLVM_UNLIKELY(ogMarkingBarriers_))
      snapshotWriteBarrierInternal(*loc);
    if (value.isPointer())
      relocationWriteBarrier(loc, value.getPointer());
  }
  void writeBarrierForLargeObj(
      const GCCell *owningObj,
      const GCHermesValueInLargeObj *loc,
      HermesValue value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForLargeObj(owningObj, loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    if (LLVM_UNLIKELY(ogMarkingBarriers_))
      snapshotWriteBarrierInternal(*loc);
    if (value.isPointer()) {
      relocationWriteBarrierForLargeObj(
          owningObj, loc, static_cast<GCCell *>(value.getPointer()));
    }
  }

  void writeBarrier(const GCSmallHermesValue *loc, SmallHermesValue value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForNormalObj(loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    if (LLVM_UNLIKELY(ogMarkingBarriers_))
      snapshotWriteBarrierInternal(*loc);
    if (value.isPointer())
      relocationWriteBarrier(loc, value.getPointer(getPointerBase()));
  }
  void writeBarrierForLargeObj(
      const GCCell *owningObj,
      const GCSmallHermesValueInLargeObj *loc,
      SmallHermesValue value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForLargeObj(owningObj, loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    if (LLVM_UNLIKELY(ogMarkingBarriers_))
      snapshotWriteBarrierInternal(*loc);
    if (value.isPointer()) {
      relocationWriteBarrierForLargeObj(
          owningObj, loc, value.getPointer(getPointerBase()));
    }
  }

  /// The given pointer value is being written at the given loc (required to
  /// be in the heap). The value may be null. Execute a write barrier.
  /// NOTE: The write barrier call must be placed *before* the write to the
  /// pointer, so that the current value can be fetched.
  void writeBarrier(const GCPointerBase *loc, const GCCell *value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForNormalObj(loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    if (*loc && ogMarkingBarriers_)
      snapshotWriteBarrierInternal(*loc);

    // Always do the non-snapshot write barrier in order for YG to be able to
    // scan cards.
    relocationWriteBarrier(loc, value);
  }
  void writeBarrierForLargeObj(
      const GCCell *owningObj,
      const GCPointerBase *loc,
      const GCCell *value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForLargeObj(owningObj, loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    if (LLVM_UNLIKELY(*loc && ogMarkingBarriers_))
      snapshotWriteBarrierInternal(*loc);
    // Always do the non-snapshot write barrier in order for YG to be able to
    // scan cards.
    relocationWriteBarrierForLargeObj(owningObj, loc, value);
  }

  /// Special versions of \p writeBarrier for when there was no previous value
  /// initialized into the space.
  void constructorWriteBarrier(const GCHermesValue *loc, HermesValue value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForNormalObj(loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    // A constructor never needs to execute a SATB write barrier, since its
    // previous value was definitely not live.
    if (!value.isPointer())
      return;

    relocationWriteBarrier(loc, value.getPointer(getPointerBase()));
  }

  void constructorWriteBarrierForLargeObj(
      const GCCell *owningObj,
      const GCHermesValueInLargeObj *loc,
      HermesValue value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForLargeObj(owningObj, loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    // A constructor never needs to execute a SATB write barrier, since its
    // previous value was definitely not live.
    if (!value.isPointer())
      return;

    relocationWriteBarrierForLargeObj(
        owningObj, loc, value.getPointer(getPointerBase()));
  }

  void constructorWriteBarrier(
      const GCSmallHermesValue *loc,
      SmallHermesValue value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForNormalObj(loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    // A constructor never needs to execute a SATB write barrier, since its
    // previous value was definitely not live.
    if (!value.isPointer())
      return;

    relocationWriteBarrier(loc, value.getPointer(getPointerBase()));
  }
  void constructorWriteBarrierForLargeObj(
      const GCCell *owningObj,
      const GCSmallHermesValueInLargeObj *loc,
      SmallHermesValue value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForLargeObj(owningObj, loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_LIKELY(inYoungGen(loc)))
      return;

    // A constructor never needs to execute a SATB write barrier, since its
    // previous value was definitely not live.
    if (!value.isPointer())
      return;

    relocationWriteBarrierForLargeObj(
        owningObj, loc, value.getPointer(getPointerBase()));
  }

  void constructorWriteBarrier(const GCPointerBase *loc, const GCCell *value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForNormalObj(loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_UNLIKELY(!inYoungGen(loc)))
      relocationWriteBarrier(loc, value);
  }
  void constructorWriteBarrierForLargeObj(
      const GCCell *owningObj,
      const GCPointerBase *loc,
      const GCCell *value) {
    assert(
        !calledByBackgroundThread() &&
        "Write barrier invoked by background thread.");
#ifdef HERMES_SLOW_DEBUG
    assertWriteBarrierForLargeObj(owningObj, loc);
#endif

    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_UNLIKELY(!inYoungGen(loc)))
      relocationWriteBarrierForLargeObj(owningObj, loc, value);
  }

  void constructorWriteBarrierRange(
      const GCCell *owningObj,
      const GCHermesValueBase *start,
      uint32_t numHVs) {
    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_UNLIKELY(!inYoungGen(start)))
      constructorWriteBarrierRangeSlow(owningObj, start, numHVs);
  }
  void constructorWriteBarrierRangeSlow(
      const GCCell *owningObj,
      const GCHermesValueBase *start,
      uint32_t numHVs);

  void constructorWriteBarrierRange(
      const GCCell *owningObj,
      const GCSmallHermesValueBase *start,
      uint32_t numHVs) {
    // A pointer that lives in YG never needs any write barriers.
    if (LLVM_UNLIKELY(!inYoungGen(start)))
      constructorWriteBarrierRangeSlow(owningObj, start, numHVs);
  }
  void constructorWriteBarrierRangeSlow(
      const GCCell *owningObj,
      const GCSmallHermesValueBase *start,
      uint32_t numHVs);

  void snapshotWriteBarrier(const GCHermesValueBase *loc) {
    if (LLVM_UNLIKELY(!inYoungGen(loc) && ogMarkingBarriers_))
      snapshotWriteBarrierInternal(*loc);
  }
  void snapshotWriteBarrier(const GCSmallHermesValueBase *loc) {
    if (LLVM_UNLIKELY(!inYoungGen(loc) && ogMarkingBarriers_))
      snapshotWriteBarrierInternal(*loc);
  }
  void snapshotWriteBarrier(const GCPointerBase *loc) {
    if (LLVM_UNLIKELY(!inYoungGen(loc) && ogMarkingBarriers_))
      if (CompressedPointer cp = *loc)
        snapshotWriteBarrierInternal(cp);
  }
  void snapshotWriteBarrier(const GCSymbolID *loc) {
    if (LLVM_UNLIKELY(!inYoungGen(loc) && ogMarkingBarriers_))
      snapshotWriteBarrierInternal(*loc);
  }

  void snapshotWriteBarrierRange(
      const GCHermesValueBase *start,
      uint32_t numHVs) {
    if (LLVM_UNLIKELY(!inYoungGen(start) && ogMarkingBarriers_))
      snapshotWriteBarrierRangeSlow(start, numHVs);
  }
  void snapshotWriteBarrierRangeSlow(
      const GCHermesValueBase *start,
      uint32_t numHVs);

  void snapshotWriteBarrierRange(
      const GCSmallHermesValueBase *start,
      uint32_t numHVs) {
    if (LLVM_UNLIKELY(!inYoungGen(start) && ogMarkingBarriers_))
      snapshotWriteBarrierRangeSlow(start, numHVs);
  }
  void snapshotWriteBarrierRangeSlow(
      const GCSmallHermesValueBase *start,
      uint32_t numHVs);

  /// Add read barrier for \p value. This is only used when reading entry
  /// value from WeakMap/WeakSet.
  void weakRefReadBarrier(HermesValue value) {
    assert(
        !calledByBackgroundThread() &&
        "Read barrier invoked by background thread.");
    if (ogMarkingBarriers_)
      snapshotWriteBarrierInternal(value);
  }

  void weakRefReadBarrier(GCCell *value) {
    assert(
        !calledByBackgroundThread() &&
        "Read barrier invoked by background thread.");
    // If the GC is marking, conservatively mark the value as live.
    if (ogMarkingBarriers_)
      snapshotWriteBarrierInternal(value);

    // Otherwise, if no GC is active at all, the weak ref must be alive.
    // During sweeping there's no special handling either.
  }

  void weakRefReadBarrier(SymbolID value) {
    assert(
        !calledByBackgroundThread() &&
        "Read barrier invoked by background thread.");
    if (ogMarkingBarriers_)
      snapshotWriteBarrierInternal(value);
  }

  void symbolAllocationBarrier(SymbolID sym) {
    assert(
        !calledByBackgroundThread() &&
        "Read barrier invoked by background thread.");
    // If marking is in progress, ensure the symbol is treated as live.
    if (ogMarkingBarriers_)
      snapshotWriteBarrierInternal(sym);
  }

  /// \}

  /// Returns whether an external allocation of the given \p size fits
  /// within the maximum heap size. (Note that this does not guarantee that the
  /// allocation will "succeed" -- the size plus the used() of the heap may
  /// still exceed the max heap size. But if it fails, the allocation can never
  /// succeed.)
  bool canAllocExternalMemory(uint32_t size) override;

  /// Iterate over all objects in the heap, and call \p callback on them.
  /// \param callback A function to call on each found object.
  void forAllObjs(const std::function<void(GCCell *)> &callback) override;

  /// Inform the GC that TTI has been reached. This will transition the GC mode,
  /// if the GC was currently allocating directly into OG.
  void ttiReached() override;

  /// \}

  /// \return true if the pointer lives in the young generation.
  bool inYoungGen(const GCCell *ptr) const override {
    return youngGen_.contains(ptr);
  }
  bool inYoungGen(const void *p) const {
    return youngGen_.contains(p);
  }
  bool inYoungGen(CompressedPointer p) const {
    return p.getSegmentStart() == youngGenCP_;
  }

  /// Approximate the dirty memory footprint of the GC's heap. Note that this
  /// does not return the number of dirty pages in the heap, but instead returns
  /// a number that goes up if pages are dirtied, and goes down if pages are
  /// cleaned.
  llvh::ErrorOr<size_t> getVMFootprintForTest() const;

#ifndef NDEBUG
  /// \name Debug APIs
  /// \{

  bool calledByBackgroundThread() const override;

  /// See comment in GCBase.
  bool calledByGC() const override {
    // Check if this is called by the background thread or the inGC flag is
    // set.
    return calledByBackgroundThread() || inGC();
  }

  /// Return true if \p ptr is currently pointing at valid accessable memory,
  /// allocated to an object.
  bool validPointer(const void *ptr) const override;

  /// Return true if \p ptr is within one of the virtual address ranges
  /// allocated for the heap. Not intended for use in normal production GC
  /// operation, debug mode only.
  bool dbgContains(const void *ptr) const override;

  /// Searches the old gen for this pointer. This is O(number of OG segments).
  /// NOTE: In any non-debug case, \c inYoungGen should be used instead, because
  /// it is O(1).
  /// \return true if the pointer is in the old gen.
  bool inOldGen(const void *p) const;

  /// Record that a cell of the given \p kind and size \p sz has been
  /// found reachable in a full GC.
  void trackReachable(CellKind kind, unsigned sz) override;

  bool needsWriteBarrier(const GCHermesValueBase *loc, HermesValue value)
      const override;
  bool needsWriteBarrier(
      const GCSmallHermesValueBase *loc,
      SmallHermesValue value) const override;
  bool needsWriteBarrier(const GCPointerBase *loc, GCCell *value)
      const override;
  bool needsWriteBarrierInCtor(const GCHermesValueBase *loc, HermesValue value)
      const override;
  bool needsWriteBarrierInCtor(
      const GCSmallHermesValueBase *loc,
      SmallHermesValue value) const override;
  /// \}
#endif

  class CollectionStats;
  class HeapMarkingAcceptor;
  template <bool CompactionEnabled>
  class EvacAcceptor;
  class MarkAcceptor;
  class MarkWeakRootsAcceptor;
  class OldGen;
  class Executor;

  struct CopyListCell final : public GCCell {
    // Linked list of cells pointing to the next cell that was copied.
    AssignableCompressedPointer next_;
  };

  /// Call \p callback on every non-freelist cell allocated in this segment.
  template <typename CallbackFunction>
  static void forAllObjsInSegment(
      FixedSizeHeapSegment &seg,
      CallbackFunction callback);
  /// Only call the callback on cells without forwarding pointers.
  template <typename CallbackFunction>
  static void forCompactedObjsInSegment(
      FixedSizeHeapSegment &seg,
      CallbackFunction callback,
      PointerBase &base);

  class OldGen final {
   public:
    explicit OldGen(HadesGC &gc);

    size_t numSegments() const;

    /// \return The deque of AlignedHeapSegments in the OG.
    std::deque<FixedSizeHeapSegment> &getSegments() {
      return segments_;
    }
    const std::deque<FixedSizeHeapSegment> &getSegments() const {
      return segments_;
    }

    /// \return The list of JumboHeapSegments in the OG.
    std::list<JumboHeapSegment> &getJumboSegments() {
      return jumboSegments_;
    }
    const std::list<JumboHeapSegment> &getJumboSegments() const {
      return jumboSegments_;
    }

    /// Iterate over all heap segments in the OG, and call \p
    /// fixedSizeSegCallback and \p jumboSegCallback on each segment with
    /// corresponding type. This does not support callback with return value or
    /// early return.
    template <typename FixedSizeSegmentCallBack, typename JumboSegmentCallBack>
    void forAllSegments(
        FixedSizeSegmentCallBack fixedSizeSegCallback,
        JumboSegmentCallBack jumboSegCallback);

    /// Take ownership of the given segment.
    void addSegment(FixedSizeHeapSegment seg);

    /// Remove the last segment from the OG.
    /// \return the segment that was removed.
    FixedSizeHeapSegment popSegment();

    /// Indicate that OG should target having a size of \p targetSizeBytes.
    void setTargetSizeBytes(size_t targetSizeBytes);

    /// Allocate into OG. Returns a pointer to the newly allocated space. That
    /// space must be filled before releasing the gcMutex_. Note that \p sz
    /// must be smaller than FixedSizeHeapSegment::maxSize().
    /// \return A non-null pointer to memory in the old gen that should have a
    ///   constructor run in immediately.
    /// \pre gcMutex_ must be held before calling this function.
    /// \post This function either successfully allocates, or reports OOM.
    GCCell *alloc(uint32_t sz);
    GCCell *allocSlow(uint32_t sz);

    /// Allocate objects that are larger than FixedSizeHeapSegment::maxSize().
    /// \tparam If Yes, failed allocation returns nullptr.
    template <MayFail mayFail>
    GCCell *allocLarge(uint32_t sz);

    /// \return the total number of bytes that are in use by the OG section of
    /// the JS heap, including any bytes allocated in a pending compactee, and
    /// excluding free list entries.
    uint64_t allocatedBytes() const;

    /// \return the number of bytes currently in the heap that are allocated by
    /// large allocation.
    uint64_t allocatedLargeObjectBytes() const;

    /// \return the total number of large allocations.
    unsigned numLargeAllocations() const;

    /// Increase the allocated bytes tracker by \p incr.
    void incrementAllocatedBytes(size_t incr);

    /// Decrease the allocated bytes tracker by \p decr.
    void decrementAllocatedBytes(size_t decr);

    /// \return the total number of bytes that are held in external memory, kept
    /// alive by objects in the OG.
    uint64_t externalBytes() const;

    /// \return the total number of bytes that are in use by the OG section of
    /// the JS heap, including any pending compactee and free list entries.
    uint64_t size() const;

    /// \return the total number of bytes that we aim to use in the OG
    /// section of the heap, including free list entries and external memory.
    /// This may be smaller or greater than size() + externalBytes().
    uint64_t targetSizeBytes() const;

    /// Add some external memory cost to the OG.
    void creditExternalMemory(uint32_t size) {
      assert(
          gc_.gcMutex_ && "OG external bytes must be accessed under gcMutex_.");
      externalBytes_ += size;
    }

    /// Remove some external memory cost from the OG.
    void debitExternalMemory(uint32_t size) {
      assert(
          externalBytes_ >= size && "Debiting more memory than was credited");
      assert(
          gc_.gcMutex_ && "OG external bytes must be accessed under gcMutex_.");
      externalBytes_ -= size;
    }

    class FreelistCell final : public VariableSizeRuntimeCell {
      friend void FreelistBuildMeta(const GCCell *, Metadata::Builder &);

     private:
      static const VTable vt;

     public:
      // If null, this is the tail of the free list.
      AssignableCompressedPointer next_{nullptr};

      /// Shrink this cell by carving out a region of size \p sz bytes. Unpoison
      /// the carved out region if necessary and return it (without any
      /// initialisation).
      /// \param sz The size that the newly-split cell should be.
      /// \pre getAllocatedSize() >= sz + minAllocationSize()
      GCCell *carve(uint32_t sz);

      static constexpr CellKind getCellKind() {
        return CellKind::FreelistKind;
      }
      static bool classof(const GCCell *cell) {
        return cell->getKind() == CellKind::FreelistKind;
      }
    };

    /// Sweep the next segment and advance the internal sweep iterator. If there
    /// are no more segments left to sweep, calls endSweep(). \p
    /// backgroundThread indicates whether this call was made from the
    /// background thread.
    bool sweepNext(bool backgroundThread);

    /// When no more segments to sweep, update OG collection stats with numbers
    /// from the sweep. Note that freeUnusedJumboSegments() is also called here.
    void endSweep();

    /// Initialize the internal sweep iterator. This will reset the internal
    /// sweep stats to 0, and set the sweep iterator to the last segment in the
    /// OG. The sweep iterator then works backwards from there, to avoid
    /// sweeping newly added segments.
    void initializeSweep();

    /// Number of segments left to sweep. Useful for determining how many YG
    /// collections it will take to complete an incremental OG collection.
    size_t sweepSegmentsRemaining() const;

    /// \return The number of bytes of native memory in use by this OldGen.
    size_t getMemorySize() const;

    /// Iterate through the jumbo segments list, and free those with dead
    /// objects. This is run in the Sweep phase, allowing mutator to proceed
    /// and allocate in YG.
    void freeUnusedJumboSegments();

#ifdef HERMES_SLOW_DEBUG
    /// Check that the freelists are well-formed.
    void verifyFreelists();
#endif

   private:
    /// The freelist buckets are split into two sections. In the "small"
    /// section, there is one bucket for each size, in multiples of heapAlign.
    /// In the "large" section, there is a bucket for each power of 2. The
    /// bucket for a large block is obtained by rounding down to the nearest
    /// power of 2.

    /// So for instance, with a heap alignment of 8 bytes, 32 small buckets,
    /// and a maximum allocation size of 2^21, we would get:

    /// |    Small section      |  Large section   |
    /// +----+----+----+   +--------------+   +----+
    /// | 0  | 8  | 16 |...| 248| 256| 512|...|2^21|
    /// +----+----+----+   +--------------+   +----+

    // We use 32 small buckets, so the total number of buckets is less than 64,
    // and can be scanned quickly.
    static constexpr size_t kLogNumSmallFreelistBuckets = 5;
    static constexpr size_t kNumSmallFreelistBuckets = 1
        << kLogNumSmallFreelistBuckets;
    static constexpr size_t kLogMinSizeForLargeBlock =
        kLogNumSmallFreelistBuckets + LogHeapAlign;
    static constexpr size_t kMinSizeForLargeBlock = 1
        << kLogMinSizeForLargeBlock;
    static constexpr size_t kNumLargeFreelistBuckets =
        llvh::detail::ConstantLog2<FixedSizeHeapSegment::maxSize()>::value -
        kLogMinSizeForLargeBlock + 1;
    static constexpr size_t kNumFreelistBuckets =
        kNumSmallFreelistBuckets + kNumLargeFreelistBuckets;

    /// \return the index of the bucket in freelistBuckets_ corresponding to
    /// \p size.
    /// \post The returned index is less than kNumFreelistBuckets.
    static uint32_t getFreelistBucket(uint32_t size);

    /// A node in the segment level freelist for a given bucket. It is inserted
    /// into a doubly linked list for a given bucket when the segment contains
    /// free cells for that bucket, and contains a pointer to a linked list of
    /// those cells.
    struct SegmentBucket {
      /// Pointers for maintaining a doubly linked list.
      SegmentBucket *next{nullptr};
      SegmentBucket *prev{nullptr};

      /// The head of the freelist of cells for this segment and bucket. Can be
      /// null if and only if this SegmentBucket is not in a segment level
      /// freelist.
      AssignableCompressedPointer head{nullptr};

      /// Adds this SegmentBucket to the freelist starting with \p dummyHead.
      void addToFreelist(SegmentBucket *dummyHead);

      /// Removes this SegmentBucket from its freelist, by updating the buckets
      /// before and after it.
      void removeFromFreelist() const;
    };

    /// Array containing the nodes for each bucket in a given segment.
    using SegmentBuckets = std::array<SegmentBucket, kNumFreelistBuckets>;

    /// Adds the given region of memory to the free list held in \p segBucket.
    void addCellToFreelist(void *addr, uint32_t sz, SegmentBucket *segBucket);

    /// Adds the given cell to the free list held in \p segBucket.
    /// \pre this->contains(cell) is true.
    void addCellToFreelist(FreelistCell *cell, SegmentBucket *segBucket);

    /// Remove the cell pointed to by the pointer at \p prevLoc from
    /// the given \p segBucket and \p bucket in the freelist.
    /// \return a pointer to the removed cell.
    FreelistCell *removeCellFromFreelist(
        AssignableCompressedPointer *prevLoc,
        size_t bucket,
        SegmentBucket *segBucket);

    /// Remove the first cell from the given \p segBucket and \p bucket in the
    /// freelist.
    /// \return a pointer to the removed cell.
    FreelistCell *removeCellFromFreelist(
        size_t bucket,
        SegmentBucket *segBucket);

    /// Adds the given region of memory to the free list for the segment that is
    /// currently being swept. \p buckets is a pointer to the array of
    /// SegmentBuckets for the segment. This does not update the Freelist bits,
    /// those should all be updated in a single pass at the end of sweeping.
    void addCellToFreelistFromSweep(
        char *freeRangeStart,
        char *freeRangeEnd,
        SegmentBuckets &segBuckets,
        bool setHead);

    HadesGC &gc_;

    /// Use a std::deque instead of a std::vector so that references into it
    /// remain valid across a push_back.
    std::deque<FixedSizeHeapSegment> segments_;

    /// List of large heap segments, each contains a single GCCell.
    /// Use std::list so that we can erase a segment once its GCCell is
    /// dead.
    std::list<JumboHeapSegment> jumboSegments_;

    /// See \c targetSizeBytes() above.
    ExponentialMovingAverage targetSizeBytes_{0, 0};

    /// Number of large allocations that have occurred in this execution.
    unsigned numLargeAllocations_{0};

    /// Sum of all bytes currently allocated in the heap by large allocation.
    /// This is a subset of allocatedBytes_ below.
    uint64_t allocatedLargeObjectBytes_{0};

    /// This is the sum of all bytes currently allocated in the heap, excluding
    /// bump-allocated segments. Use \c allocatedBytes() to include
    /// bump-allocated segments.
    uint64_t allocatedBytes_{0};

    /// The amount of bytes of external memory credited to objects in the OG.
    uint64_t externalBytes_{0};

    /// The below data structures should be used as follows.
    /// 1. When looking for a given size, first find the smallest appropriate
    ///    bucket that has any elements at all, by scanning
    ///    freelistBucketBitArray_.
    /// 2. Once a bucket is identified, index into buckets_ to obtain the
    ///    segment level freelist, which is a linked list of SegmentBuckets
    ///    containing free cells for that bucket.
    /// 3. Finally, obtain the cell level freelist from the selected
    ///    SegmentBucket, and select a cell.

    /// Keep track of which freelist buckets have valid elements to make search
    /// fast. This includes all segments.
    BitArray<kNumFreelistBuckets> freelistBucketBitArray_;

    /// Contains all of the nodes for the segment level freelists. For each
    /// segment in the OG, this holds kNumFreelistBuckets SegmentBucket nodes.
    /// These can be added or removed from the freelists pointed to by buckets_
    /// as needed.
    std::deque<SegmentBuckets> segmentBuckets_;

    /// Contains dummy heads for the segment level freelists for each bucket.
    std::array<SegmentBucket, kNumFreelistBuckets> buckets_{};

    /// A FreelistCell that has been set aside as an arena to try allocating
    /// from before we fall back to a full freelist search. This may be null if
    /// no such chunk is set.
    FreelistCell *allocChunk_ = nullptr;

    /// A pointer to the first SegmentBucket for the segment that allocChunk_ is
    /// in. This is necessary so we can return any leftover portion of
    /// allocChunk to the freelist_. Only valid if allocChunk_ is non-null.
    SegmentBucket *allocChunkBaseBucket_;

    /// Tracks the current progress of sweeping.
    struct SweepIterator {
      /// The current segment being swept, this should start at the end and move
      /// to the front of the segment list, to avoid sweeping newly added
      /// segments.
      size_t segNumber{0};

      /// The total number of GC-managed and external bytes swept in the current
      /// sweep.
      uint64_t sweptBytes{0};
      uint64_t sweptExternalBytes{0};

#ifndef NDEBUG
      /// The total number of bytes trimmed from GC-managed cells.
      uint64_t trimmedBytes{0};
#endif
    } sweepIterator_;

    /// Searches the OG for a space to allocate memory into.
    /// \return A pointer to uninitialized memory that can be written into, null
    ///   if no such space exists.
    GCCell *search(uint32_t sz);

    /// Common path for when an allocation has succeeded.
    /// \param cell The free memory that will soon have an object allocated into
    ///   it.
    /// \param sz The number of bytes associated with the free memory.
    GCCell *finishAlloc(GCCell *cell, uint32_t sz);
  };

 private:
  /// Enqueue the cell \p cell for the marker to process.
  /// This can only be called by the mutator.
  void barrierEnqueue(GCCell *cell);

  /// Empty and return the current worklist
  llvh::SmallVector<GCCell *, 0> drainBarrierWorklist();

  /// Flush the full barrier push chunk to the worklist.
  void handleFullBarrierChunk();

  /// When we are preparing to finish marking, move any elements in the push
  /// chunk into the worklist to make sure they are marked. This must only be
  /// called once per collection. The push chunk buffer and index are invalid
  /// after this operation.
  /// WARN: This can only be called by the mutator.
  void flushPendingBarrierPushChunk();

  /// WARN: This can only be called from the mutator.
  bool isBarrierWorklistEmpty();
  /// The maximum number of bytes that the heap can hold. Once this amount has
  /// been filled up, OOM will occur. When creating new segment, we allow an
  /// extra buffer with size AlignedHeapSegment::kSegmentUnitSize.
  const uint64_t maxHeapSize_;

  /// This needs to be placed before youngGen_ and oldGen_, because those
  /// members use numSegments_ as part of being constructed.
  uint64_t numSegments_{0};

  /// Stores previously allocated segment indices that have since
  /// been freed. We can reuse them when another segment is allocated.
  std::vector<size_t> segmentIndices_;

  /// Keeps the storage provider alive until after the GC is fully destructed.
  std::shared_ptr<StorageProvider> provider_;

  /// youngGen is a bump-pointer space, so it can re-use FixedSizeHeapSegment.
  /// Protected by gcMutex_.
  FixedSizeHeapSegment youngGen_;
  AssignableCompressedPointer youngGenCP_;

  /// List of cells in YG that have finalizers. Iterate through this to clean
  /// them out.
  /// Protected by gcMutex_.
  std::vector<GCCell *> youngGenFinalizables_;

  /// Since YG collection times are the primary driver of pause times, it is
  /// useful to have a knob to reduce the effective size of the YG. This number
  /// is the fraction of FixedSizeHeapSegment::maxSize() that we should use for
  /// the YG.. Note that we only set the YG size using this at the end of the
  /// first real YG, since doing it for direct promotions would waste OG memory
  /// without a pause time benefit.
  static constexpr double kYGInitialSizeFactor = 0.5;
  double ygSizeFactor_{kYGInitialSizeFactor};

  /// Flag used to signal to the background thread that it should stop and yield
  /// the gcMutex_ to the mutator as soon as possible.
  AtomicIfConcurrentGC<bool> ogPaused_{false};

  enum class Phase : uint8_t {
    None,
    Mark,
    CompleteMarking,
    Sweep,
  };

  /// Represents the current phase the concurrent GC is in. The main difference
  /// between phases is their effect on read and write barriers. Should only be
  /// accessed if gcMutex_ is acquired.
  Phase concurrentPhase_{Phase::None};

  /// Represents whether the background thread is currently marking. Should only
  /// be accessed by the mutator thread or during a STW pause.
  /// ogMarkingBarriers_ is true from the start of marking the OG heap until the
  /// start of the STW pause but is kept separate from concurrentPhase_ in
  /// order to reduce synchronisation requirements for write barriers.
  bool ogMarkingBarriers_{false};

  /// oldGen_ is a free list space, so it needs a different segment
  /// representation.
  /// Protected by gcMutex_.
  OldGen oldGen_;

#ifndef NDEBUG
  /// Map an address aligned to AlignedHeapSegment::kSegmentUnitSize to the
  /// start and end address of its owning segment.
  llvh::DenseMap<const char *, std::pair<const char *, const char *>>
      unitSegmentAddrMap_;
#endif

  /// Whoever holds this lock is permitted to modify data structures around the
  /// GC. This includes mark bits, free lists, etc.
  Mutex gcMutex_;

#ifdef HERMES_SLOW_DEBUG
  /// CellKind and size of the GCCell being constructed. It is set in makeA()
  /// before constructing the cell and reset in the end. Note that we don't
  /// use KindAndSize here since the actual size could be larger than
  /// GCCell::maxNormalSize().
  llvh::Optional<std::pair<CellKind, uint32_t>> currentCellKindAndSize_;
#endif

  /// Condition variable used to block either the mutator or background thread
  /// until the other has completed.
  ///   1. The background thread should wait on this when ogPaused_ is set to
  ///   true, until the mutator has acquired gcMutex_.
  ///   2. The mutator should wait on this if it is waiting for the background
  ///   thread to finish its current task.
  std::condition_variable_any ogPauseCondVar_;

  /// State that needs to be maintained while we are marking the old gen.
  struct MarkState {
    /// A worklist local to the marking thread, that is only pushed onto by the
    /// marking thread. If this is empty, the global worklist must be consulted
    /// to ensure that pointers modified in write barriers are handled.
    std::stack<GCCell *, std::vector<GCCell *>> localWorklist;

    /// A fixed size local buffer that the mutator can push elements onto
    /// without needing to acquire the lock. This allows us to batch writes
    /// before acquiring the lock and pushing them onto worklist_.
    static constexpr size_t kBarrierChunkSize = 128;
    std::array<GCCell *, kBarrierChunkSize> barrierPushChunk_;

    /// The index in pushChunk_ at which the next element will be written.
    unsigned barrierChunkIndex_{0};

    /// Mutex protecting barrierWorklist_, allowing it to be accessed from the
    /// GC thread.
    Mutex barrierWorklistMtx_;

    /// The maximum allowed size of the barrier worklist. If this is exceeded,
    /// the mutator needs to block and ensure it is drained.
    static constexpr auto kMaxBarrierWorklistSize = 16 * kBarrierChunkSize;

    /// A worklist that other threads may add to as objects to be marked and
    /// considered alive. These objects will *not* have their mark bits set,
    /// because the mutator can't be modifying mark bits at the same time as the
    /// marker thread.
    /// Use a SmallVector of size 0 since it is more aggressive with PODs
    /// Protected by barrierWorklistMtx_.
    llvh::SmallVector<GCCell *, 0> barrierWorklist_;

    /// markedSymbols_ represents which symbols have been proven live so far in
    /// a collection. True means that it is live, false means that it could
    /// possibly be garbage. The SymbolID's internal value is used as the index
    /// into this vector. Once the collection is finished, this vector is passed
    /// to IdentifierTable so that it can free symbols. If any new symbols are
    /// allocated after the collection began, assume they are live.
    llvh::BitVector markedSymbols;

    /// A vector the same size as markedSymbols_ that will collect all symbols
    /// marked by write barriers. Merge this with markedSymbols_ to have
    /// complete information about marked symbols. Kept separate to avoid
    /// synchronization.
    llvh::BitVector writeBarrierMarkedSymbols;

    /// The number of bytes to drain per call to drainSomeWork. A higher rate
    /// means more objects will be marked.
    /// Only used by incremental collections.
    size_t byteDrainRate{0};

    /// The number of bytes that have been marked so far.
    uint64_t markedBytes{0};

    explicit MarkState(size_t numSymbols)
        : markedSymbols(numSymbols), writeBarrierMarkedSymbols(numSymbols) {}
  };

  /// Store the mark state as an optional so it is convenient to create and
  /// destroy it at the start and end of marking. This field and all of its
  /// contents should only be accessed or modified when gcMutex_ is held.
  llvh::Optional<MarkState> markState_;

  /// This provides the background thread for doing marking and sweeping
  /// concurrently with the mutator.
  std::unique_ptr<Executor> backgroundExecutor_;

  /// True from the time the background task is created, to the time it exits
  /// the collection loop. False otherwise. Protected by gcMutex_.
  bool backgroundTaskActive_{false};

  /// If true, whenever YG fills up immediately put it into the OG.
  bool promoteYGToOG_;

  /// If true, turn off promoteYGToOG_ as soon as ttiReached() is called.
  bool revertToYGAtTTI_;

  /// If true, overwrite the allocation region in the YG with kInvalidHeapValue
  /// at the end of each YG collection.
  bool overwriteDeadYGObjects_;

  /// Target OG occupancy ratio at the end of an OG collection.
  const double occupancyTarget_;

  /// The threshold, expressed as the occupied fraction of the target OG size,
  /// at which we should start an OG collection.
  ExponentialMovingAverage ogThreshold_{0.5, 0.75};

  /// A collection section used to track the size of YG before and after a YG
  /// collection, as well as the time a YG collection takes.
  std::unique_ptr<CollectionStats> ygCollectionStats_;

  /// A collection section used to track the size of OG before and after an OG
  /// collection, as well as the time an OG collection takes.
  std::unique_ptr<CollectionStats> ogCollectionStats_;

  /// Cumulative stats for each type of collection.
  CumulativeHeapStats ogCumulativeStats_;
  CumulativeHeapStats ygCumulativeStats_;

  /// The weighted average of the number of bytes that are promoted to the OG in
  /// each YG collection.
  ExponentialMovingAverage ygAverageSurvivalBytes_;

  /// The amount of bytes of external memory credited to objects in the YG.
  /// Only accessible to the mutator.
  uint64_t ygExternalBytes_{0};

  struct CompacteeState {
    /// \return true if the pointer lives in the segment that is being marked or
    /// evacuated for compaction.
    bool contains(const void *p) const {
      return start == FixedSizeHeapSegment::storageStart(p);
    }
    bool contains(CompressedPointer p) const {
      return p.getSegmentStart() == startCP;
    }

#ifndef NDEBUG
    /// \return true if the compactee has not been assigned.
    bool empty() const {
      void *const invalid = reinterpret_cast<void *>(kInvalidCompacteeStart);
      return start == invalid && !segment;
    }
#endif

    /// The value to set the start pointers below to when there is no compactee.
    /// In order to keep the "contains" check cheap, this can be any non-null
    /// value that cannot correspond to the start of a segment.
    static constexpr uintptr_t kInvalidCompacteeStart = 0x1;

    /// The start address of the segment that will be compacted next. This is
    /// used during marking and by write barriers to determine whether a pointer
    /// is in the compactee segment.
    void *start{reinterpret_cast<void *>(kInvalidCompacteeStart)};
    AssignableCompressedPointer startCP{
        CompressedPointer::fromRaw(kInvalidCompacteeStart)};

    /// The segment being compacted. This should be removed from the OG right
    /// after it is identified, and freed entirely once the compaction is
    /// complete.
    std::shared_ptr<FixedSizeHeapSegment> segment;
  } compactee_;

  /// The number of compactions this GC has performed.
  size_t numCompactions_{0};

  /// The number of collections this GC has performed in each generation.
  size_t numYoungCollections_{0};
  size_t numOldCollections_{0};

  struct NativeIDs {
    HeapSnapshot::NodeID ygFinalizables{IDTracker::kInvalidNode};
    HeapSnapshot::NodeID og{IDTracker::kInvalidNode};
  } nativeIDs_;

  /// Slow path for allocations. This returns nullptr iff \p sz is larger than
  /// FixedSizeHeapSegment::maxSize().
  /// \tparam canBeLarge If Yes, the object being allocated supports large
  /// allocations, and \p sz may be > FixedHeapSegment::maxSize().
  /// \param mayFail If Yes, the large allocation path may fail and return a
  /// null pointer.
  template <CanBeLarge canBeLarge, MayFail mayFail>
  void *allocSlow(uint32_t sz);

  /// Like alloc, but the resulting object is expected to be long-lived.
  /// Allocate directly in the old generation (doing a full collection if
  /// necessary to create room).
  /// \tparam canBeLarge If Yes, the object being allocated supports large
  /// allocations, and \p sz may be > FixedHeapSegment::maxSize().
  /// \param mayFail If Yes, the large allocation path may fail and return a
  /// null pointer.
  template <CanBeLarge canBeLarge, MayFail mayFail>
  void *allocLongLived(uint32_t sz);

  /// Perform a YG garbage collection. All live objects in YG will be evacuated
  /// to the OG.
  /// \param cause The cause of the GC, used for logging.
  /// \param forceOldGenCollection If true, always start an old gen collection
  ///   if one is not already active.
  /// \post The YG is completely empty, and all bytes are available for new
  ///   allocations.
  void youngGenCollection(std::string cause, bool forceOldGenCollection);

  /// Implementation to actually do the YG collection. Mark roots, scan dirty
  /// cards, and evacuate reachable objects.
  /// \tparam CompactionEnabled Whether there is a compaction candidate.
  /// \param doCompaction Whether to actually do a compaction. This can only be
  ///   true if CompactionEnabled is true.
  /// \return the number of bytes evacuated.
  template <bool CompactionEnabled>
  uint64_t youngGenEvacuateImpl(bool doCompaction);

  /// In the "no GC before TTI" mode, move the Young Gen heap segment to the
  /// Old Gen without scanning for garbage.
  /// \return true if a promotion occurred, false if it did not.
  bool promoteYoungGenToOldGen();

  /// This function checks if the live bytes after the last OG GC is greater
  /// than the tripwire limit. If the conditions are met, the tripwire is
  /// triggered and tripwireCallback_ is called.
  /// Also submits pending OG collection stats, and calls the analytics
  /// callback.
  /// WARNING: Do not call this while there is an ongoing collection. It can
  /// cause a race condition and a deadlock.
  void checkTripwireAndSubmitStats();

  /// Credit the YG's external memory to the OG and reset the YG's effective
  /// end, which drops its external memory charge. Used by YG collections and
  /// by finalizeAll.
  void transferExternalMemoryToOldGen();

  /// Update the scaling factor for the size of the young gen to meet our pause
  /// time goals, based on the duration of the most recently completed YG.
  void updateYoungGenSizeFactor();

  /// Perform an OG garbage collection. All live objects in OG will be left
  /// untouched, all unreachable objects will be placed into a free list that
  /// can be used by \c oldGenAlloc.
  /// \param forceCompaction If true, ensure compaction is performed as part of
  ///   the collection regardless of heap conditions.
  void oldGenCollection(std::string cause, bool forceCompaction);

  /// If there's an OG collection going on, wait for it to complete. This
  /// function is synchronous and will block the caller if the GC background
  /// thread is still running.
  /// \pre The gcMutex_ must be held before entering this function.
  /// \post The gcMutex_ will be held when the function exits, but it might
  ///   have been unlocked and then re-locked.
  void waitForCollectionToFinish(std::string cause);

  /// Worker function that schedules the bulk of the GC work on the background
  /// thread, to perform it concurrently with the mutator.
  void collectOGInBackground();

  /// Ensures that work on the background thread to be suspended when
  /// concurrent GC is enabled.
  LLVM_NODISCARD std::lock_guard<Mutex> ensureBackgroundTaskPaused() {
    if constexpr (kConcurrentGC) {
      return pauseBackgroundTask();
    }
    return std::lock_guard(gcMutex_);
  }

  /// Forces work on the background thread to be suspended and returns a lock
  /// holding gcMutex_. This is used to ensure that the mutator receives
  /// priority in acquiring gcMutex_, and does not remain blocked on the
  /// background thread for an extended period of time. The background thread
  /// will resume once the lock is released.
  LLVM_NODISCARD std::lock_guard<Mutex> pauseBackgroundTask();

  /// Perform a single step of an OG collection. \p backgroundThread indicates
  /// whether this call was made from the background thread.
  void incrementalCollect(bool backgroundThread);

  /// Drain some of the work to be done for marking.
  /// \param markLimit Only mark up to this many bytes from the local
  /// worklist.
  ///   NOTE: This does *not* guarantee that the marker thread
  ///   has upper bounds on the amount of work it does before reading from the
  ///   global worklist. Any individual cell can be quite large (such as an
  ///   ArrayStorage).
  /// \return true if there is any remaining work in the local worklist.
  bool incrementalMark(size_t markLimit);

  /// Iterate the list of `weakMapEntrySlots_`, for each non-free slot, if
  /// both the key and the owner are marked, mark the mapped value.
  /// Note that this may further cause other values to be marked, so we need to
  /// keep iterating until no update. After the iteration, set each unreachable
  /// mapped value to Empty.
  void markWeakMapEntrySlots();

  /// Finish the marking process. This requires a STW pause in order to do a
  /// final marking worklist drain, and to update weak roots. It must be invoked
  /// from the mutator.
  void completeMarking();

  /// Update the OG collection threshold by estimating the mark rate and using
  /// that to estimate how late we can start a collection without going over the
  /// heap limit. Should be called at the start of completeMarking.
  void updateOldGenThreshold();

  /// Prepare the last segment in the OG for compaction and initialise any
  /// necessary state.
  /// \param forceCompaction If true, a compactee will be prepared regardless of
  ///   heap conditions. Note that if there are no OG heap segments, a
  ///   compaction cannot occur no matter what.
  void prepareCompactee(bool forceCompaction);

  /// Run finalizers on the compactee and clear any compaction state.
  void finalizeCompactee();

  /// Search a single segment for pointers that may need to be updated as the
  /// YG/compactee are evacuated.
  template <bool CompactionEnabled>
  void scanDirtyCardsForSegment(
      EvacAcceptor<CompactionEnabled> &acceptor,
      FixedSizeHeapSegment &segment);
  template <bool CompactionEnabled>
  void scanDirtyCardsForSegment(
      EvacAcceptor<CompactionEnabled> &acceptor,
      JumboHeapSegment &segment);

  /// Find all pointers from OG into the YG/compactee during a YG collection.
  /// This is done quickly through use of write barriers that detect the
  /// creation of such pointers. By default cards are cleared after they are
  /// scanned, but if \p CompactionEnabled is true, and \p doCompaction is
  /// false, the cards will be preserved, as they may represent pointers into
  /// the compactee which is not being evacuated yet.
  template <bool CompactionEnabled>
  void scanDirtyCards(
      EvacAcceptor<CompactionEnabled> &acceptor,
      bool doCompaction);

  /// Common logic for doing the Snapshot At The Beginning (SATB) write barrier.
  void snapshotWriteBarrierInternal(GCCell *oldValue);
  void snapshotWriteBarrierInternal(CompressedPointer oldValue);

  /// Common logic for doing the Snapshot At The Beginning (SATB) write barrier.
  /// Forwards to \c snapshotWriteBarrierInternal(GCCell*) if oldValue is a
  /// pointer. Forwards to \c snapshotWriteBarrierInternal(SymbolID) is oldValue
  /// is a symbol.
  void snapshotWriteBarrierInternal(HermesValue oldValue);
  void snapshotWriteBarrierInternal(SmallHermesValue oldValue);

  /// Performs a Snapshot At The Beginning (SATB) write barrier for a symbol,
  /// which assumes the old symbol was reachable at the start of the collection.
  void snapshotWriteBarrierInternal(SymbolID symbol);

  /// Common logic for doing the relocation write barrier for detecting
  /// pointers into YG and for tracking newly created pointers into the
  /// compactee.
  void relocationWriteBarrier(const void *loc, const void *value) {
    assert(!inYoungGen(loc) && "Pre-condition from other callers");
    // Do not dirty cards for compactee->compactee, yg->yg, or yg->compactee
    // pointers. But do dirty cards for compactee->yg pointers, since compaction
    // may not happen in the next YG.
    if (inYoungGen(value) ||
        (compactee_.contains(value) && !compactee_.contains(loc))) {
      // Only dirty a card if it's an old-to-young or old-to-compactee pointer.
      // This is fine to do since the GC never modifies card tables outside of
      // allocation.
      // Note that this *only* applies since the boundaries are updated
      // separately from the card table being marked itself.
      FixedSizeHeapSegment::dirtyCardForAddress(loc);
    }
  }

  void relocationWriteBarrierForLargeObj(
      const GCCell *owningObj,
      const void *loc,
      const GCCell *value) {
    assert(!inYoungGen(loc) && "Pre-condition from other callers");
    // Do not dirty cards for compactee->compactee, yg->yg, or yg->compactee
    // pointers. But do dirty cards for compactee->yg pointers, since compaction
    // may not happen in the next YG.
    if (inYoungGen(value) ||
        (compactee_.contains(value) && !compactee_.contains(loc))) {
      // Only dirty a card if it's an old-to-young or old-to-compactee pointer.
      // This is fine to do since the GC never modifies card tables outside of
      // allocation.
      // Note that this *only* applies since the boundaries are updated
      // separately from the card table being marked itself.
      AlignedHeapSegment::dirtyCardForAddressInLargeObj(owningObj, loc);
    }
  }

  /// Finalize all objects in YG that have finalizers.
  void finalizeYoungGenObjects();

  /// Return the total number of bytes that are in use by the JS heap.
  uint64_t allocatedBytes() const;

  /// \return the total number of bytes that are in use by objects on the JS
  ///   heap, but is not in the heap itself.
  uint64_t externalBytes() const;

  /// \return the total number of bytes used by heap segments, including segment
  /// metadata.
  uint64_t segmentFootprint() const;

  /// \return the total number of bytes used by the heap, including segment
  /// metadata and external memory.
  uint64_t heapFootprint() const;

  /// Accessor for the YG.
  FixedSizeHeapSegment &youngGen() {
    return youngGen_;
  }
  const FixedSizeHeapSegment &youngGen() const {
    return youngGen_;
  }

  /// Create a new segment (to be used by either YG or OG).
  llvh::ErrorOr<FixedSizeHeapSegment> createSegment();

  /// Create a jumbo segment (for large allocation).
  llvh::ErrorOr<JumboHeapSegment> createJumboSegment(size_t segmentSize);

  /// Actual implementation for creating a heap segment.
  /// \tparam createSegFunc takes StorageProvider pointer, segment name and size
  /// to create a segment and return it.
  template <typename T, typename CreateSegFunc>
  llvh::ErrorOr<T> createSegmentImpl(
      size_t segmentSize,
      CreateSegFunc createSegFunc);

  /// Set a given segment as the YG segment.
  /// \return the previous YG segment.
  FixedSizeHeapSegment setYoungGen(FixedSizeHeapSegment seg);

  /// Get/set the current number of external bytes used by the YG.
  size_t getYoungGenExternalBytes() const;
  void setYoungGenExternalBytes(size_t sz);

  /// Give the background GC a chance to complete marking and finish the OG
  /// collection.
  void yieldToOldGen();

  /// Set the drain rate in markState to the number of bytes that should be
  /// drained on a per-YG basis to help ensure an incremental collection will
  /// finish before the next one is needed.
  void updateDrainRate();

  /// Adds the start address of the segment to the CrashManager's custom data.
  /// \param extraName append this to the name of the segment. Must be
  ///   non-empty.
  void addSegmentExtentToCrashManager(
      const AlignedHeapSegment &seg,
      size_t segSize,
      const std::string &extraName);

  /// Deletes a segment from the CrashManager's custom data.
  /// \param extraName that was used to initially add this segment to the crash
  /// manager.
  void removeSegmentExtentFromCrashManager(const std::string &extraName);

#ifdef HERMES_SLOW_DEBUG
  /// Return the start/end address of the segment that contains \p addr.
  std::pair<const char *, const char *> getSegmentAddrRange(const void *addr);

  /// Assert that \p loc does not belong to an object that supports large
  /// allocation.
  void assertWriteBarrierForNormalObj(const void *loc);

  /// Assert that \p owningObj is an object that supports large allocation and
  /// it contains \p loc.
  void assertWriteBarrierForLargeObj(const GCCell *owningObj, const void *loc);

  /// Checks the heap to make sure all cells are valid.
  void checkWellFormed();

  /// Verify that the card table used to find pointers from OG into YG has the
  /// correct cards dirtied, given the contents of the OG currently.
  void verifyCardTable();
#endif
};

/// \name Inline implementations
/// \{

template <
    typename T,
    HasFinalizer hasFinalizer,
    LongLived longLived,
    CanBeLarge canBeLarge,
    MayFail mayFail,
    class... Args>
inline T *HadesGC::makeAImpl(uint32_t size, Args &&...args) {
  // When mayFail == MayFail::Yes, we must have canBeLarge == CanBeLarge::Yes.
  static_assert(
      (mayFail == MayFail::No) || (canBeLarge == CanBeLarge::Yes),
      "Only large allocation is allowed to fail");
  assert(
      ((canBeLarge == CanBeLarge::No) ||
       VTable::getVTable(T::getCellKind())->allowLargeAlloc) &&
      "T must support large allocation when canBeLarge is Yes");
  assert(
      isSizeHeapAligned(size) &&
      "Call to makeA must use a size aligned to HeapAlign");
  assert(size >= minAllocationSize() && "Allocating too small of an object");
  assert(noAllocLevel_ == 0 && "No allocs allowed right now.");

  if (shouldSanitizeHandles()) {
    auto lk = ensureBackgroundTaskPaused();
    // The best way to sanitize uses of raw pointers outside handles is to force
    // the entire heap to move, and ASAN poison the old heap. That is too
    // expensive to do, even with sampling, for Hades. It also doesn't test the
    // more interesting aspect of Hades which is concurrent background
    // collections. So instead, do a youngGenCollection which force-starts an
    // oldGenCollection if one is not already running.
    youngGenCollection(
        kHandleSanCauseForAnalytics, /*forceOldGenCollection*/ true);
  }

#ifdef HERMES_SLOW_DEBUG
  assert(!currentCellKindAndSize_ && "makeA() calls can not be interleaved");
  llvh::SaveAndRestore<llvh::Optional<std::pair<CellKind, uint32_t>>>
      saveCurrentCellKindAndSize(
          currentCellKindAndSize_, std::make_pair(T::getCellKind(), size));
#endif
  if constexpr (longLived == LongLived::Yes) {
    auto lk = ensureBackgroundTaskPaused();
    auto *ptr = allocLongLived<canBeLarge, mayFail>(size);
    if constexpr (canBeLarge == CanBeLarge::Yes) {
      if constexpr (mayFail == MayFail::Yes) {
        // If it fails, a nullptr is allowed and simply return it to the caller.
        if (LLVM_UNLIKELY(!ptr))
          return nullptr;
      }
      return constructCellCanBeLarge<T>(ptr, size, std::forward<Args>(args)...);
    }
    return constructCell<T>(ptr, size, std::forward<Args>(args)...);
  }

  // Try allocating in YG.
  AllocResult res = youngGen().alloc(size);
  if (LLVM_LIKELY(res.success)) {
    if (hasFinalizer == HasFinalizer::Yes)
      youngGenFinalizables_.emplace_back(static_cast<GCCell *>(res.ptr));
    return constructCell<T>(res.ptr, size, std::forward<Args>(args)...);
  }
  // Slow path for types that don't support large allocation.
  if constexpr (canBeLarge == CanBeLarge::No) {
    auto *ptr = allocSlow<CanBeLarge::No, MayFail::No>(size);
    if (hasFinalizer == HasFinalizer::Yes)
      youngGenFinalizables_.emplace_back(static_cast<GCCell *>(ptr));
    return constructCell<T>(ptr, size, std::forward<Args>(args)...);
  }
  // Slow path for types that support large allocation.
  return makeASlowCanBeLarge<T, hasFinalizer, mayFail>(
      size, std::forward<Args>(args)...);
}

template <typename T, HasFinalizer hasFinalizer, MayFail mayFail, class... Args>
T *HadesGC::makeASlowCanBeLarge(uint32_t size, Args &&...args) {
  // Since CanBeLarge is Yes, this could be a large allocation. We need to hold
  // the lock until constructCellCanBeLarge() returns.
  auto lk = ensureBackgroundTaskPaused();
  auto *ptr = allocSlow<CanBeLarge::Yes, mayFail>(size);
  if constexpr (mayFail == MayFail::Yes) {
    // If it fails, a nullptr is allowed and simply return it to the caller.
    if (LLVM_UNLIKELY(!ptr))
      return nullptr;
  }
  return constructCellCanBeLarge<T>(ptr, size, std::forward<Args>(args)...);
}

template <typename T1, typename T2, typename... T1Args, typename... T2Args>
std::pair<T1 *, T2 *> HadesGC::make2YoungGenUnsafeImpl(
    uint32_t size1,
    std::tuple<T1Args...> t1Args,
    uint32_t size2,
    std::tuple<T2Args...> t2Args) {
  assert(noAllocLevel_ == 0 && "No allocs allowed right now.");

  if (shouldSanitizeHandles()) {
    auto lk = ensureBackgroundTaskPaused();
    // The best way to sanitize uses of raw pointers outside handles is to force
    // the entire heap to move, and ASAN poison the old heap. That is too
    // expensive to do, even with sampling, for Hades. It also doesn't test the
    // more interesting aspect of Hades which is concurrent background
    // collections. So instead, do a youngGenCollection which force-starts an
    // oldGenCollection if one is not already running.
    youngGenCollection(
        kHandleSanCauseForAnalytics, /*forceOldGenCollection*/ true);
  }

  uint32_t totalSize = size1 + size2;

  assert(
      ((uint64_t)size1 + (uint64_t)size2) <=
          (uint64_t)maxYoungGenAllocationSize() &&
      "must be able to allocate in young gen");

  AllocResult res = youngGen().alloc(totalSize);
  void *t1Void = LLVM_UNLIKELY(!res.success)
      ? allocSlow<CanBeLarge::No, MayFail::No>(totalSize)
      : res.ptr;
  void *t2Void = ((uint8_t *)t1Void + size1);

  T1 *t1Res;
  T2 *t2Res;
  {
#ifdef HERMES_SLOW_DEBUG
    assert(!currentCellKindAndSize_ && "makeA() calls can not be interleaved");
    llvh::SaveAndRestore<llvh::Optional<std::pair<CellKind, uint32_t>>>
        saveCurrentCellKindAndSize(
            currentCellKindAndSize_, std::make_pair(T1::getCellKind(), size1));
#endif
    t1Res = llvh::apply_tuple(
        [t1Void, size1](auto &&...args) -> T1 * {
          return constructCell<T1>(t1Void, size1, args...);
        },
        t1Args);
  }
  {
#ifdef HERMES_SLOW_DEBUG
    assert(!currentCellKindAndSize_ && "makeA() calls can not be interleaved");
    llvh::SaveAndRestore<llvh::Optional<std::pair<CellKind, uint32_t>>>
        saveCurrentCellKindAndSize(
            currentCellKindAndSize_, std::make_pair(T2::getCellKind(), size2));
#endif
    t2Res = llvh::apply_tuple(
        [t2Void, size2](auto &&...args) -> T2 * {
          return constructCell<T2>(t2Void, size2, args...);
        },
        t2Args);
  }

  return {t1Res, t2Res};
}

/// \}

} // namespace vm
} // namespace hermes
#endif
