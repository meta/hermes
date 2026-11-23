/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_MALLOCGC_H
#define HERMES_VM_MALLOCGC_H

#include "hermes/Public/GCConfig.h"
#include "hermes/VM/CompressedPointer.h"
#include "hermes/VM/GCBase.h"
#include "hermes/VM/GCCell.h"
#include "hermes/VM/VMExperiments.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/DenseSet.h"
#include "llvh/Support/raw_ostream.h"

#include <deque>
#include <limits>
#include <vector>

namespace hermes {
namespace vm {

class MallocGC final : public GCBase {
  struct CellHeader {
    /// If true, then this cell is in the "young generation". Note that MallocGC
    /// is not actually a generational collector, this just tracks some objects
    /// that could plausibly be in the young gen to exercise codepaths that
    /// depend on inYoungGen being true.
    bool inYoungGen;

    /// If true, then this cell is live. If this is false at the end of a
    /// collection, then this cell can be freed. Defaults to false when not in
    /// the middle of a collection.
    bool marked_ = false;

    /// The size of this cell. On 32bits platform, we use 24bits for size in
    /// KindAndSize, which is not large enough for any allocation with larger
    /// size. So we store the actual size here and use when calling
    /// GCCell::getAllocatedSizeSlow().
    uint32_t cellSize;

#if HERMESVM_SANITIZE_HANDLES != 0
    /// If non-null, contains a pointer to the new location where this cell will
    /// be after the collection is over.
    /// Only used in handle sanitization when pointers are moved.
    CellHeader *forwardingPtr_ = nullptr;
#endif

    /// Storage of the GCCell data is past this point. Needs to be aligned to
    /// the alignment of the heap.
    static_assert(
        alignof(GCCell) <= HeapAlign,
        "GCCell's alignment exceeds the alignment requirement of the heap");
    alignas(HeapAlign) uint8_t data_[0];

    bool isMarked() const {
      return marked_;
    }

    /// Mark a cell as live. Set its new location to \p newLocation.
    void mark() {
      assert(!isMarked() && "Marking an already marked pointer");
      marked_ = true;
    }

    void markWithForwardingPointer(CellHeader *newLocation) {
      mark();
#if HERMESVM_SANITIZE_HANDLES != 0
      forwardingPtr_ = newLocation;
#else
      llvm_unreachable(
          "Trying to add a forwarding pointer outside of handle-san mode");
#endif
    }

    void unmark() {
      assert(isMarked() && "Unmarking an already unmarked pointer");
      marked_ = false;
#if HERMESVM_SANITIZE_HANDLES != 0
      forwardingPtr_ = nullptr;
#endif
    }

    GCCell *data() {
      return reinterpret_cast<GCCell *>(data_);
    }

    const GCCell *data() const {
      return reinterpret_cast<const GCCell *>(data_);
    }

    CellHeader *getForwardingPointer() {
      assert(isMarked() && "Getting forwarding pointer from unmarked pointer");
#if HERMESVM_SANITIZE_HANDLES != 0
      assert(forwardingPtr_ && "Accessing a null forwarding pointer");
      return forwardingPtr_;
#else
      llvm_unreachable(
          "Can't get a forwarding pointer outside of handle-san mode");
#endif
    }

    static CellHeader *from(GCCell *cell) {
      return reinterpret_cast<CellHeader *>(
          reinterpret_cast<char *>(cell) - offsetof(CellHeader, data_));
    }

    static const CellHeader *from(const GCCell *cell) {
      return reinterpret_cast<const CellHeader *>(
          reinterpret_cast<const char *>(cell) - offsetof(CellHeader, data_));
    }
  };

  /// pointers_ stores all of the objects managed by the GC. If a pointer is not
  /// in this set, then it is not a GC pointer, and thus invalid to be collected
  /// or marked.
  llvh::DenseSet<CellHeader *> pointers_;
  /// newPointers_ is the set of live objects at the end of a collection.
  /// Pointers are moved from pointers_ to this as they are discovered to be
  /// alive.
  /// This should be empty between collections.
  llvh::DenseSet<CellHeader *> newPointers_;
  /// Whether to temporarily suppress collections. We must verify that we have
  /// enough space to satisfy all allocations while collections are suppressed,
  /// so in practice, this just prevents Handle-San from running.
  bool suppressCollections_ = false;
  /// maxSize_ is the absolute highest amount of memory that MallocGC is allowed
  /// to allocate.
  const gcheapsize_t maxSize_;
  /// sizeLimit_ is the point at which a collection should be run.
  gcheapsize_t sizeLimit_;
  /// allocatedBytes_ is the current amount of memory stored in the heap.
  gcheapsize_t allocatedBytes_{0};
  /// externalBytes_ is the external memory retained by cells on the heap.
  uint64_t externalBytes_{0};

 public:
  MallocGC(
      GCCallbacks &gcCallbacks,
      PointerBase &pointerBase,
      const GCConfig &gcConfig,
      std::shared_ptr<CrashManager> crashMgr,
      std::shared_ptr<StorageProvider> provider,
      experiments::VMExperimentFlags vmExperimentFlags);

  ~MallocGC() override;

  /// Perform a collection and resize the heap as necessary to ensure that an
  /// object of \p size bytes can be allocated. If this is not possible, we OOM.
  void collectBeforeAlloc(std::string cause, uint32_t size);

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

  /// Returns whether an external allocation of the given \p size fits
  /// within the maximum heap size.  (Note that this does not guarantee that the
  /// allocation will "succeed" -- the size plus the used() of the heap may
  /// still exceed the max heap size.  But if it fails, the allocation can never
  /// succeed.)
  bool canAllocExternalMemory(uint32_t size) override;

  /// \return the maximum number of bytes that can be allocated at once in the
  /// young gen.
  static constexpr inline uint32_t maxYoungGenAllocationSize() {
    return std::numeric_limits<uint32_t>::max();
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

  /// Collect all of the dead objects and symbols in the heap. Also invalidate
  /// weak pointers that point to dead objects.
  void collect(std::string cause, bool canEffectiveOOM = false) override;
  void collectWithOptions(std::string &&cause, const GCOptions &options)
      override;

  /// Total number of collections; MallocGC has no separate young generation.
  /// Requires exclusive runtime access.
  unsigned getNumYGGCs() const {
    return getNumGCs();
  }

  /// The minimum allocation size.
  static constexpr uint32_t minAllocationSize() {
    // MallocGC imposes no limit on individual allocations.
    return 0;
  }

  /// The maximum allocation size.
  static constexpr uint32_t maxNormalAllocationSize() {
    // MallocGC imposes no limit on individual allocations.
    return std::numeric_limits<uint32_t>::max();
  }

  /// Run the finalizers for all heap objects.
  void finalizeAll() override;

  bool inYoungGen(const GCCell *p) const override {
    return CellHeader::from(p)->inYoungGen;
  }
  /// Also allow checking SmallHermesValues and HermesValues.
  using GCBase::inYoungGen;

  /// Get the cell size. This is used when the size of \p cell is larger than
  /// GCCell::maxNormalSize().
  static uint32_t getLargeCellSize(const GCCell *cell) {
    auto *header = CellHeader::from(cell);
    return header->cellSize;
  }

#ifndef NDEBUG
  /// See comment in GCBase.
  bool calledByGC() const override {
    return inGC();
  }

  /// \return true iff the pointer \p p is controlled by this GC.
  bool validPointer(const void *p) const override;
  bool dbgContains(const void *p) const override;

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
#endif

#ifdef HERMES_MEMORY_INSTRUMENTATION
  /// Same as in superclass GCBase.
  virtual void createSnapshot(llvh::raw_ostream &os, bool captureNumericValue)
      override;
#endif

  virtual void creditExternalMemory(GCCell *alloc, uint32_t size) override;
  virtual void debitExternalMemory(GCCell *alloc, uint32_t size) override;

  class FreelistCell final : public GCCell {
    friend void FreelistBuildMeta(const GCCell *, Metadata::Builder &);

   private:
    static const VTable vt;

   public:
    static constexpr CellKind getCellKind() {
      return CellKind::FreelistKind;
    }
    static bool classof(const GCCell *cell) {
      return cell->getKind() == CellKind::FreelistKind;
    }
  };

  void writeBarrier(const GCHermesValue *, HermesValue) {}
  void writeBarrier(const GCSmallHermesValue *, SmallHermesValue) {}
  void writeBarrierForLargeObj(
      const GCCell *,
      const GCHermesValueInLargeObj *,
      HermesValue) {}
  void writeBarrierForLargeObj(
      const GCCell *,
      const GCSmallHermesValueInLargeObj *,
      SmallHermesValue) {}
  void writeBarrier(const GCPointerBase *, const GCCell *) {}
  void writeBarrierForLargeObj(
      const GCCell *,
      const GCPointerBase *,
      const GCCell *) {}
  void constructorWriteBarrier(const GCHermesValue *, HermesValue) {}
  void constructorWriteBarrier(const GCSmallHermesValue *, SmallHermesValue) {}
  void constructorWriteBarrierForLargeObj(
      const GCCell *,
      const GCSmallHermesValueInLargeObj *,
      SmallHermesValue) {}
  void constructorWriteBarrierForLargeObj(
      const GCCell *,
      const GCHermesValueInLargeObj *,
      HermesValue) {}
  void constructorWriteBarrier(const GCPointerBase *, const GCCell *) {}
  void constructorWriteBarrierForLargeObj(
      const GCCell *,
      const GCPointerBase *,
      const GCCell *) {}
  void writeBarrierRange(const GCHermesValue *, uint32_t) {}
  void writeBarrierRange(const GCSmallHermesValue *, uint32_t) {}
  void constructorWriteBarrierRange(
      const GCCell *,
      const GCHermesValueBase *,
      uint32_t) {}
  void constructorWriteBarrierRange(
      const GCCell *,
      const GCSmallHermesValueBase *,
      uint32_t) {}
  void snapshotWriteBarrier(const GCHermesValueBase *) {}
  void snapshotWriteBarrier(const GCSmallHermesValueBase *) {}
  void snapshotWriteBarrier(const GCPointerBase *) {}
  void snapshotWriteBarrier(const GCSymbolID *) {}
  void snapshotWriteBarrierRange(const GCHermesValueBase *, uint32_t) {}
  void snapshotWriteBarrierRange(const GCSmallHermesValueBase *, uint32_t) {}
  void weakRefReadBarrier(HermesValue) {}
  void weakRefReadBarrier(GCCell *) {}
  void weakRefReadBarrier(SymbolID) {}
  void symbolAllocationBarrier(SymbolID) {}

  void getHeapInfo(HeapInfo &info) override;
  void getHeapInfoWithMallocSize(HeapInfo &info) override;
  void getCrashManagerHeapInfo(CrashManager::HeapInformation &info) override;
  std::string getKindAsStr() const override;

  /// The largest the size of this heap could ever grow to.
  size_t maxSize() const {
    return maxSize_;
  }

  /// Iterate over all objects in the heap, and call \p callback on them.
  /// \param callback A function to call on each found object.
  void forAllObjs(const std::function<void(GCCell *)> &callback) override;

  static bool classof(const GCBase *gc) {
    return gc->getKind() == HeapKind::MallocGC;
  }

 private:
#ifdef HERMES_SLOW_DEBUG
  void checkWellFormed();
  void clearUnmarkedPropertyMaps();
#endif

  /// Allocate an object in the GC controlled heap with the size to allocate
  /// given by \p size.
  GCCell *alloc(uint32_t size);

  /// Initialize a cell with the required basic data for any cell.
  inline void initCell(GCCell *cell, uint32_t size);

  /// See \c GCBase::printStats.
  void printStats(JSONEmitter &json) override;

  /// Reset the statistics used for reporting GC information.
  void resetStats();

  struct MarkingAcceptor;
  class SkipWeakRefsMarkingAcceptor;
  struct FullMSCUpdateWeakRootsAcceptor;

  /// Continually pops elements from the mark stack of \p acceptor and
  /// scans their pointer fields.  If such a field points to an
  /// unmarked object, mark it and push it on the mark stack.
  void drainMarkStack(MarkingAcceptor &acceptor);

  /// Iterate the list of `weakMapEntrySlots_`, for each non-free slot, if
  /// both the key and the owner are marked, mark the mapped value with
  /// \p acceptor. This may further cause other values to be marked, so we need
  /// to keep iterating until no update. After the iteration, set each
  /// unreachable mapped value to Empty.
  void markWeakMapEntrySlots(MarkingAcceptor &acceptor);

  /// Update all of the weak references and invalidate the ones that point to
  /// dead objects.
  void updateWeakReferences();
};

/// @name Inline implementations
/// @{

inline bool MallocGC::canAllocExternalMemory(uint32_t size) {
  return size <= maxSize_;
}

template <
    typename T,
    HasFinalizer hasFinalizer,
    LongLived longLived,
    CanBeLarge canBeLarge,
    MayFail mayFail,
    class... Args>
inline T *MallocGC::makeAImpl(uint32_t size, Args &&...args) {
  // For now, when mayFail == MayFail::Yes, we must have canBeLarge ==
  // CanBeLarge::Yes.
  static_assert(
      (mayFail == MayFail::No) || (canBeLarge == CanBeLarge::Yes),
      "Only large allocation can actually fail");
  assert(
      isSizeHeapAligned(size) &&
      "Call to makeA must use a size aligned to HeapAlign");
  // Since there is no old generation in this collector, always forward to the
  // normal allocation.
  // Note that if checkedMalloc() fails and returns nullptr, it will fatal out
  // immediately.
  GCCell *mem = alloc(size);
  if (LLVM_UNLIKELY(!mem)) {
    if constexpr (mayFail == MayFail::Yes) {
      return nullptr;
    }
    oom(make_error_code(OOMError::MaxHeapReached));
  }
  auto *header = CellHeader::from(mem);
  header->inYoungGen = longLived == LongLived::No;
  header->cellSize = size;
  // On 32bits platform, the allocation size could be larger than
  // GCCell::maxNormalSize(), so we store 0 in the cell's KindAndSize, and call
  // GCCell::getAllocatedSizeSlow() to get the actual size (which reads the
  // cellSize field in CellHeader). Note that on 64bits platform, the size is
  // always smaller because MallocGC is not compatible with compressed pointer.
  return constructCell<T>(
      mem,
      size > GCCell::maxNormalSize() ? 0 : size,
      std::forward<Args>(args)...);
}

inline void MallocGC::initCell(GCCell *cell, uint32_t size) {
#ifndef NDEBUG
  // For debugging, fill with a dead value.
  std::fill_n(reinterpret_cast<char *>(cell), size, kInvalidHeapValue);
#endif
}

template <typename T1, typename T2, typename... T1Args, typename... T2Args>
std::pair<T1 *, T2 *> MallocGC::make2YoungGenUnsafeImpl(
    uint32_t size1,
    std::tuple<T1Args...> t1Args,
    uint32_t size2,
    std::tuple<T2Args...> t2Args) {
  uint32_t totalSize = size1 + size2;

  assert(!suppressCollections_ && "Collections are already suppressed");
  if (totalSize > sizeLimit_ - allocatedBytes_) {
    // If we need to collect to free up additional space, do so now.
    collectBeforeAlloc(kNaturalCauseForAnalytics, totalSize);
  } else if (shouldSanitizeHandles()) {
    // If Handle-San is enabled, then we want to run before the allocations
    // below. Once we have checked that there is enough space, we cannot
    // collect, as that may change the size.
    collectBeforeAlloc(kHandleSanCauseForAnalytics, totalSize);
  }
  if (totalSize > sizeLimit_ - allocatedBytes_) {
    oom(make_error_code(OOMError::MaxHeapReached));
  }

  // NOTE: We use makeAImpl directly here instead of makeA, because we want to
  // keep the common logic of assigning memory instrumentation IDs (e.g.) in
  // GCBase, so we don't want to duplicate that logic here.

  // We have verified that there is enough space to satisfy the allocations, so
  // suppress collections from Handle-San. This ensures that both objects are in
  // the "YG", and the size of the heap does not change past this point.
  suppressCollections_ = true;

  // Need to capture and explicitly use 'this'.
  // If it's not captured, compiler errors because 'makeAImpl' needs 'this'.
  // If it is captured but not used explicitly, compiler warns due to
  //   -Wunused-lambda-capture.
  // Might be due to apply_tuple.
  T1 *t1 = llvh::apply_tuple(
      [this, size1](auto &&...args) -> T1 * {
        return this->makeAImpl<
            T1,
            HasFinalizer::No,
            LongLived::No,
            CanBeLarge::No,
            MayFail::No>(size1, args...);
      },
      t1Args);

  T2 *t2 = llvh::apply_tuple(
      [this, size2](auto &&...args) -> T2 * {
        return this->makeAImpl<
            T2,
            HasFinalizer::No,
            LongLived::No,
            CanBeLarge::No,
            MayFail::No>(size2, args...);
      },
      t2Args);
  suppressCollections_ = false;

  assert(CellHeader::from(t1)->inYoungGen && "allocation must be in young gen");
  assert(CellHeader::from(t2)->inYoungGen && "allocation must be in young gen");

  return {t1, t2};
}

/// @}

} // namespace vm
} // namespace hermes

#endif
