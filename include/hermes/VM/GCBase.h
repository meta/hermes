/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_GCBASE_H
#define HERMES_VM_GCBASE_H

#include "hermes/ADT/ManagedChunkedList.h"
#include "hermes/Inst/Inst.h"
#include "hermes/Platform/Logging.h"
#include "hermes/Public/CrashManager.h"
#include "hermes/Public/GCConfig.h"
#include "hermes/Public/GCTripwireContext.h"
#include "hermes/Support/CheckedMalloc.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/Support/StatsAccumulator.h"
#include "hermes/VM/AlignedHeapSegment.h"
#include "hermes/VM/AllocOptions.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/CompressedPointer.h"
#include "hermes/VM/GCDecl.h"
#include "hermes/VM/GCExecTrace.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/HeapAlign.h"
#include "hermes/VM/HeapSnapshot.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/RootAcceptor.h"
#include "hermes/VM/SlotVisitor.h"
#include "hermes/VM/SmallHermesValue.h"
#include "hermes/VM/StackTracesTree-NoRuntime.h"
#include "hermes/VM/StorageProvider.h"
#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/VTable.h"
#include "hermes/VM/WeakRefSlot.h"

#include "llvh/ADT/ArrayRef.h"
#include "llvh/ADT/BitVector.h"
#include "llvh/ADT/DenseMap.h"
#include "llvh/Support/ErrorHandling.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <random>
#include <system_error>
#include <vector>

namespace hermes {
namespace vm {

/// Forward declarations;
class GCCell;
class JSObject;
class JSWeakMapImplBase;

/// Used by XorPtr to separate encryption keys between uses.
enum XorPtrKeyID {
  ArrayBufferData,
  JSFunctionCodeBlock,
  DummyObjectFinalizerCallback,
  _NumKeys
};

// A specific GC class extend GCBase, and override its virtual functions.
// In addition, it must implement the following methods:

/// Allocate a new cell of type \p T and size \p size. Instantiate an object of
/// type \p T in the newly allocated cell, using \p args as the arguments to its
/// constructor. If necessary perform a GC cycle, which may potentially move
/// allocated objects.
/// \p hasFinalizer must be \p HasFinalizer::Yes if cells of
/// the given type require a finalizer to be called.
/// \pre size must be heap-aligned.
///
///   template <
///     typename T,
///     HasFinalizer hasFinalizer,
///     LongLived longLived,
///     class... Args>
/// inline T *makeAImpl(uint32_t size, Args &&... args);
///
/// Allocate two young gen objects of the size \p size1 + \p size2.
/// Calls the constructors of types T1 and T2 with the arguments \p t1Args and
/// \p t2Args respectively.
/// \pre the TOTAL size must be able to fit in the young gen
///  (can be checked with canAllocateInYoungGen(size)).
/// \post both result pointers are in the young gen.
/// \return a pointer to size1 size T1, and a pointer to size2 size T2.
///
/// template <typename T1, typename T2, typename... T1Args, typename... T2Args>
/// inline std::pair<T1 *, T2 *> make2YoungGenUnsafeImpl(
///     uint32_t size1,
///     std::tuple<T1Args...> t1Args,
///     uint32_t size2,
///     std::tuple<T2Args...> t2Args);
///
/// In some GCs, objects can have associated memory allocated outside the heap,
/// and this memory can influence GC initiation and heap sizing heuristics.
/// This method tests whether an external memory allocation is too large (e.g.,
/// larger than the max size of the heap):
///
///   bool canAllocExternalMemory(uint32_t size);
///
/// These APIs inform the GC of this external memory.
///
///   void creditExternalMemory(GCCell *alloc, uint32_t size);
///   void debitExternalMemory(GCCell *alloc, uint32_t size);
///
/// Force a garbage collection cycle. The provided cause will be used in
/// logging.
///   void collect(std::string cause);
///
/// The maximum size of an normal allocation (compared to large allocation)
/// allowable by the GC in any state.
///   static constexpr uint32_t maxNormalAllocationSize();
/// The minimum size of an allocation allowable by the GC in any state.
///   static constexpr uint32_t minAllocationSize();
///
/// Mark a pointer to a GCCell.
///   template <class T> void mark(T *&ptr);
///
/// Returns true if \p p points into the heap.
///   bool contains(const void *p) const;
///
/// Return the lower bound of the heap's virtual address range (inclusive).
///   char *lowLim() const;
///
/// Return the upper bound of the heap's virtual address range (exclusive).
///   char *hiLim() const;
///
/// Iterate over all objects in the heap.
///   void forAllObjs(const std::function<void(GCCell *)> &callback);
///
/// In the "mark" functions below, Name is one of const char*, int,
/// unsigned, or const StringPrimitive*:
///
/// Mark a HermesValue which may or may not be a pointer.
///   void mark(HermesValue &hv, Name name);
///   void mark(HermesValue &hv);
///
/// Mark a T* location. This location must be outside the heap.
///   void mark(T *&ptr, Name name);
///   void mark(T *&ptr);
///
/// Mark a GCPointer<T>, which must be within the heap.
///   void mark(GCPointer<T> &ptr, Name name);
///
/// Return true if the GC is active and is calling into the VM.
/// If true, some objects in the heap may have an invalid VTable pointer.
///   bool calledByGC() const;
///
/// Various forms of write barriers: these can have empty implementations
/// for GCs that don't require them:
///
///   The given value is being written at the given loc (required to
///   be in the heap).  If value is a pointer, execute a write barrier.
///     void writeBarrier(GCHermesValue *loc, HermesValue value);
///     void writeBarrier(
///         const GCSmallHermesValue *loc,
///         SmallHermesValue value);
///
///   The given pointer value is being written at the given loc (required to
///   be in the heap).  The value is may be null.  Execute a write barrier.
///     void writeBarrier(const GCPointerBase *loc, const GCCell *value);
///
///   The given value/pointer is being written at a previously uninitialized loc
///   (required to be in the heap).
///     void constructorWriteBarrier(
///         const GCHermesValue *loc,
///         HermesValue value);
///     void constructorWriteBarrier(
///         const GCSmallHermesValue *loc,
///         SmallHermesValue value);
///     void constructorWriteBarrier(
///         const GCPointerBase *loc,
///         const GCCell *value);
///
///   A weak ref is about to be read. Executes a read barrier so the GC can
///   take action such as extending the lifetime of the reference.
///     void weakRefReadBarrier(GCCell *value);
///   A mapped value in WeakMap/WeakSet is about to be read. Executes a read
///   barrier so the GC can take action such as extending the lifetime of the
///   mapped value.
///     void weakRefReadBarrier(HermesValue value);
///
///   The given symbol has just been allocated, ensure that it is treated as
///   live. Symbols are special because they are allocated by the
///   IdentifierTable, but are marked by the GC.
///     void symbolAllocationBarrier(SymbolID sym);
///
///   We copied HermesValues into the given region.  Note that \p numHVs is
///   the number of HermesValues in the the range, not the char length.
///   Do any necessary barriers.
///     void writeBarrierRange(GCHermesValue* start, uint32_t numHVs);
///     void writeBarrierRange(
///         const GCSmallHermesValue *start, uint32_t
///         numHVs);
///     void constructorWriteBarrierRange(
///         const GCHermesValueBase *start,
///         uint32_t numHVs);
///     void constructorWriteBarrierRange(
///         const GCSmallHermesValueBase *start,
///         uint32_t numHVs);
///
///   The given loc or region is about to be overwritten, but the new value is
///   not important. Perform any necessary barriers.
///     void snapshotWriteBarrier(const GCHermesValueBase *loc);
///     void snapshotWriteBarrier(const GCSmallHermesValueBase *loc);
///     void snapshotWriteBarrier(const GCPointerBase *loc);
///     void snapshotWriteBarrier(const GCSymboldID *symbol);
///     void snapshotWriteBarrierRange(
///         const GCHermesValueBase *start,
///         uint32_t numHVs);
///     void snapshotWriteBarrierRange(
///         const GCSmallHermesValueBase *start,
///         uint32_t numHVs);
///
///   The above barriers may have a variant with "ForLargeObj" suffix, which is
///   used when the heap location may be from a GCCell of a kind that supports
///   large allocation. This variant is less efficient since it has to load the
///   cards array through pointer in SHSegmentInfo, instead of the inline array
///   field in CardTable structure. Because of this, these variant write
///   barriers need a pointer to the start of the object in order to locate the
///   card table for an object that is larger than the unit segment size.
///
///   In debug builds: is a write barrier necessary for a write of the given
///   GC pointer \p value to the given \p loc?
///      bool needsWriteBarrier(void *loc, void *value);
///
/// This is intended to be called only from within object or root mark
/// functions and indicates whether the \c mark() operation, called within
/// the current GC phase, is the first such call that guarantees that the
/// location passed to mark will contain the final, correct, pointer value
/// after the mark call.
///   bool isUpdatingPointers() const;
///
class GCBase {
  friend struct RuntimeOffsets;

 public:
  static const char kNaturalCauseForAnalytics[];
  static const char kHandleSanCauseForAnalytics[];

  /// The type of GC to perform.
  enum class GCType : uint8_t {
    /// Minor GC work. In the context of HadesGC, this means YG collection. For
    /// MallocGC, this would be the same as Major, which is to perform full
    /// collection.
    Minor,
    /// Major GC work that may block the JS execution. In HadesGC, this means OG
    /// collection.
    Major,
  };

  /// Execution policy for a GC operation. MallocGC does not support this since
  /// it's always synchronous.
  enum class GCExecutionPolicy : uint8_t {
    /// Run the GC work synchronously in the JS execution thread.
    Sync,
    /// Run the GC work asynchronously either in background thread or in the JS
    /// execution thread as a queued job. Note that for HadesGC, this may still
    /// start with a synchronous YG collection (which is a short pause) and run
    /// the OG work in background thread.
    Async,
  };

  /// Options for a GC request, defaulting to synchronous minor collection.
  struct GCOptions {
    GCType type = GCType::Minor;
    GCExecutionPolicy executionPolicy = GCExecutionPolicy::Sync;
  };

  /// An interface enabling the garbage collector to mark roots and free
  /// symbols.
  struct GCCallbacks {
    /// Virtual destructor to avoid warnings.
    virtual ~GCCallbacks() = 0;

    /// Callback that will be invoked by the GC to mark all roots in the
    /// beginning of every GC by calling "gc->mark()".
    /// The \p markLongLived argument indicates whether root data structures
    /// that contain only references to long-lived objects (allocated
    /// via allocLongLived) are required to be scanned.  A generational
    /// collector, for example, might take advantage of this.
    virtual void markRoots(
        RootAcceptorWithNames &acceptor,
        bool markLongLived = true) = 0;

    /// Callback that will be invoked by the GC to mark all weak roots in the
    /// beginning of every GC.
    virtual void markWeakRoots(
        WeakRootAcceptor &weakAcceptor,
        bool markLongLived = true) = 0;

    /// Callback that might be invoked by the GC before it completes marking.
    /// Not all GCs will call this. It should be used to mark any roots that
    /// might change without executing proper read and write barriers on the GC.
    /// An example would be something that skipped weak ref read barriers in a
    /// signal handler. If they aren't marked, they would be collected as
    /// garbage.
    /// While it is possible to implement this just as forwarding to
    /// \c markRoots, to be faster it should try to mark only things that would
    /// not have been properly doing barriers.
    virtual void markRootsForCompleteMarking(
        RootAcceptorWithNames &acceptor) = 0;

    /// \return one higher than the largest symbol in the identifier table. This
    /// enables the GC to size its internal structures for symbol marking.
    /// Optionally invoked at the beginning of a garbage collection.
    virtual unsigned getSymbolsEnd() const = 0;

    /// Free all symbols which are not marked as \c true in \p markedSymbols.
    /// Optionally invoked at the end of a garbage collection. The function may
    /// set additional bits in \p markedSymbols to reflect the fact that some
    /// symbols were not freed.
    virtual void freeSymbols(llvh::BitVector &markedSymbols) = 0;

    /// Prints any statistics maintained in the Runtime about GC to \p
    /// os.  At present, this means the breakdown of markRoots time by
    /// "phase" within markRoots.
    virtual void printRuntimeGCStats(JSONEmitter &json) const = 0;

    /// \returns the approximate usage of memory external to the GC such as
    /// malloc by the roots of the object graph.
    virtual size_t mallocSize() const = 0;

    /// Visits every entry in the identifier table and calls acceptor with
    /// the entry as argument. This is intended to be used only for Snapshots,
    /// as it is slow. The function passed as acceptor shouldn't perform any
    /// heap operations.
    virtual void visitIdentifiers(
        const std::function<void(SymbolID, const StringPrimitive *)>
            &acceptor) = 0;

    /// Convert the given symbol into its UTF-8 string representation.
    /// \post The implementation of this function must not perform any GC
    ///   operations, such as allocations, mutating values in the heap, or
    ///   making handles.
    virtual std::string convertSymbolToUTF8(SymbolID id) = 0;

    /// Returns the current stack as a string. This function will not cause
    /// any allocs in the GC.
    virtual std::string getCallStackNoAlloc() = 0;

    /// This is called with CollectionStart at the start of each GC, and with
    /// CollectionEnd at the end.
    /// \param extraInfo contains more detailed extra info for specific GC.
    virtual void onGCEvent(GCEventKind kind, const std::string &extraInfo) = 0;

    /// Return the current VM instruction pointer which can be used to derive
    /// the current VM stack-trace. It's "slow" because it's virtual.
    virtual const inst::Inst *getCurrentIPSlow() const = 0;

    /// Register a new heap segment with the VM. This will populate the
    /// SegmentInfo, as well as any information needed for compressed pointers.
    /// \param idx represents the index to use for this segment.
    /// \param lowLim represents the start of the newly added segment.
    virtual void registerHeapSegment(unsigned idx, void *lowLim) = 0;

#ifdef HERMES_MEMORY_INSTRUMENTATION
    /// Return a \c StackTracesTreeNode representing the current VM stack-trace
    /// at this point.
    virtual StackTracesTreeNode *getCurrentStackTracesTreeNode(
        const inst::Inst *ip) = 0;

    /// Get a StackTraceTree which can be used to recover stack-traces from \c
    /// StackTraceTreeNode() as returned by \c getCurrentStackTracesTreeNode() .
    virtual StackTracesTree *getStackTracesTree() = 0;
#endif

#ifdef HERMES_SLOW_DEBUG
    /// \return true if the given symbol is a live entry in the identifier
    /// table.
    /// NOTE: Used by CheckHeapWellFormedAcceptor to make sure a symbol
    /// that is discovered live is marked as live.
    virtual bool isSymbolLive(SymbolID id) = 0;

    /// \return An associated heap cell for the symbol if one exists, null
    /// otherwise.
    virtual const void *getStringForSymbol(SymbolID id) = 0;
#endif
  };

  /// Wrapper class that takes some \p RT which implements all the methods
  /// specified in \c GCCallbacks as non-virtual functions, and creates a new
  /// subclass of \c GCCallbacks that calls through to those non-virtual
  /// methods.
  template <typename RT>
  class GCCallbacksWrapper final : public GCCallbacks {
    RT &runtime_;

   public:
    GCCallbacksWrapper(RT &runtime) : runtime_(runtime) {}
    void markRoots(RootAcceptorWithNames &acceptor, bool markLongLived)
        override {
      runtime_.markRoots(acceptor, markLongLived);
    }
    void markWeakRoots(WeakRootAcceptor &weakAcceptor, bool markLongLived)
        override {
      runtime_.markWeakRoots(weakAcceptor, markLongLived);
    }
    void markRootsForCompleteMarking(RootAcceptorWithNames &acceptor) override {
      runtime_.markRootsForCompleteMarking(acceptor);
    }
    unsigned getSymbolsEnd() const override {
      return runtime_.getSymbolsEnd();
    }
    void freeSymbols(llvh::BitVector &markedSymbols) override {
      runtime_.freeSymbols(markedSymbols);
    }
    void printRuntimeGCStats(JSONEmitter &json) const override {
      runtime_.printRuntimeGCStats(json);
    }
    size_t mallocSize() const override {
      return runtime_.mallocSize();
    }
    void visitIdentifiers(
        const std::function<void(SymbolID, const StringPrimitive *)> &acceptor)
        override {
      runtime_.visitIdentifiers(acceptor);
    }
    std::string convertSymbolToUTF8(SymbolID id) override {
      return runtime_.convertSymbolToUTF8(id);
    }
    std::string getCallStackNoAlloc() override {
      return runtime_.getCallStackNoAlloc();
    }
    void onGCEvent(GCEventKind kind, const std::string &extraInfo) override {
      runtime_.onGCEvent(kind, extraInfo);
    }
    const inst::Inst *getCurrentIPSlow() const override {
      return runtime_.getCurrentIPSlow();
    }
    void registerHeapSegment(unsigned idx, void *lowLim) override {
      runtime_.registerHeapSegment(idx, lowLim);
    }

#ifdef HERMES_MEMORY_INSTRUMENTATION
    StackTracesTreeNode *getCurrentStackTracesTreeNode(
        const inst::Inst *ip) override {
      return runtime_.getCurrentStackTracesTreeNode(ip);
    }
    StackTracesTree *getStackTracesTree() override {
      return runtime_.getStackTracesTree();
    }
#endif

#ifdef HERMES_SLOW_DEBUG
    bool isSymbolLive(SymbolID id) override {
      return runtime_.isSymbolLive(id);
    }
    const void *getStringForSymbol(SymbolID id) override {
      return runtime_.getStringForSymbol(id);
    }
#endif
  };

  /// Stats for collections. Time unit, where applicable, is seconds.
  struct CumulativeHeapStats {
    unsigned numCollections{0};

    /// Summary statistics for GC wall times. For Hades, this should only track
    /// time spent on the mutator.
    StatsAccumulator<double> gcWallTime;

    /// Summary statistics for GC CPU times.
    StatsAccumulator<double> gcCPUTime;

    gcheapsize_t finalHeapSize{0};

    /// Bytes allocated just before a collection.
    StatsAccumulator<gcheapsize_t, uint64_t> usedBefore;

    /// Bytes alive after a collection.
    StatsAccumulator<gcheapsize_t, uint64_t> usedAfter;
  };

  struct HeapInfo {
    /// Number of garbage collections (of any kind) since creation.
    unsigned numCollections{0};
    /// Number of compaction since creation (zero if non-generational GC).
    unsigned numCompactions{0};
    /// Total (cumulative) bytes allocated within the JS heap since creation.
    uint64_t totalAllocatedBytes{0};
    /// Number of currently allocated bytes within the JS heap. Some may be
    /// in unreachable objects (unless a full collection just occurred).
    uint64_t allocatedBytes{0};
    /// Current capacity of the JS heap, in bytes.
    uint64_t heapSize{0};
    /// Estimate of amount of current malloc space used by the runtime and any
    /// auxiliary allocations owned by heap objects. (Calculated by querying
    /// each finalizable object to report its malloc usage.)
    unsigned mallocSizeEstimate{0};
    /// The total amount of Virtual Address space (VA) that the GC is using.
    uint64_t va{0};
    /// Number of bytes retained by objects as external memory on the C++ heap.
    /// This is typically associated with allocations that are modified
    /// infrequently, and can therefore be stored in a counter in the GC, making
    /// them cheaper to query. This is a subset of mallocSizeEstimate.
    uint64_t externalBytes{0};
    /// Number of large allocations that have occurred (zero if non-generational
    /// GC).
    uint64_t numLargeAllocations{0};
    /// Bytes of alive large objects (zero if non-generational  GC).
    uint64_t allocatedLargeObjectBytes{0};
    /// Stats for general collection (including both YG and OG).
    CumulativeHeapStats generalStats;
    /// Stats for full collections (zeroes if non-generational GC).
    CumulativeHeapStats fullStats;
    /// Stats for collections in the young generation (zeroes if
    /// non-generational GC).
    CumulativeHeapStats youngGenStats;
  };

#ifndef NDEBUG
  struct DebugHeapInfo {
    /// Number of currently allocated objects present in the heap. Some may be
    /// unreachable.
    unsigned numAllocatedObjects{0};
    /// Number of reachable objects in the last collection.
    unsigned numReachableObjects{0};
    /// Number of collected objects in the last collection.
    unsigned numCollectedObjects{0};
    /// Number of finalized objects in the last collection.
    unsigned numFinalizedObjects{0};
    /// Number of marked symbols.
    unsigned numMarkedSymbols{0};
    /// Number of hidden classes alive after the last collection.
    unsigned numHiddenClasses{0};
    /// Number of "leaf" hidden classes alive after the last collection.
    unsigned numLeafHiddenClasses{0};

    // Assert any invariants that should hold among the fields of the
    // DebugHeapInfo.
    void assertInvariants() const;
  };
#endif

#ifdef HERMES_MEMORY_INSTRUMENTATION
  /// When enabled, every allocation gets an attached stack-trace and an
  /// object ID. When disabled old allocations continue to be tracked but
  /// no new allocations get a stack-trace.
  struct AllocationLocationTracker final {
    friend struct RuntimeOffsets;

    explicit AllocationLocationTracker(GCBase *gc);

    /// Returns true if tracking is enabled for new allocations.
    bool isEnabled() const;
    /// Must be called by GC implementations whenever a new allocation is made.
    void newAlloc(const GCCell *ptr, uint32_t sz);

    /// If an object's size changes, update the entry here.
    void updateSize(const GCCell *ptr, uint32_t oldSize, uint32_t newSize);

    /// Must be called by GC implementations whenever an allocation is freed.
    void freeAlloc(const GCCell *ptr, uint32_t sz);
    /// Returns data needed to reconstruct the JS stack used to create the
    /// specified allocation.
    StackTracesTreeNode *getStackTracesTreeNodeForAlloc(
        HeapSnapshot::NodeID id) const;

    /// A Fragment is a time bound for when objects are allocated. Any
    /// allocations that occur before the lastSeenObjectID_ are in this
    /// fragment. Allocations increment the numObjects_ and numBytes_. Free'd
    /// cells from this fragment decrement numObjects_ and numBytes_.
    struct Fragment {
      HeapSnapshot::NodeID lastSeenObjectID_;
      std::chrono::microseconds timestamp_;
      /// Number of objects still alive in this fragment. Incremented when
      /// objects are created, decremented when objects are destroyed.
      uint64_t numObjects_;
      /// Total size of objects still alive in this fragment.
      uint64_t numBytes_;
      /// If true, one of numObjects or numBytes changed since the last flush.
      bool touchedSinceLastFlush_;
    };

    /// This must match the definition in jsi::Instrumentation to avoid
    /// unnecessary copying.
    using HeapStatsUpdate = std::tuple<uint64_t, uint64_t, uint64_t>;

    /// Enable location tracking.
    void enable(
        std::function<void(
            uint64_t,
            std::chrono::microseconds,
            std::vector<HeapStatsUpdate>)> callback);

    /// Disable location tracking - turns \c newAlloc() into a no-op. Existing
    /// allocations continue to be tracked.
    void disable();

    /// Flush the current fragment and write all flushed fragments to \p snap.
    void addSamplesToSnapshot(HeapSnapshot &snap);

   private:
    /// Flush out heap profiler data to the callback after a new kFlushThreshold
    /// bytes are allocated.
    static constexpr uint64_t kFlushThreshold = 128 * (1 << 10);
    /// This mutex protects stackMap_ and fragments_. Specifically does not
    /// protect enabled_, because enabled_ should only be changed while the GC
    /// isn't running anyway.
    Mutex mtx_;
    /// Associates allocations at their current location with their stack trace
    /// data.
    llvh::DenseMap<HeapSnapshot::NodeID, StackTracesTreeNode *> stackMap_;
    /// We need access to the GCBase to collect the current stack when nodes are
    /// allocated.
    GCBase *gc_;
    /// Indicates if tracking of new allocations is enabled.
    bool enabled_{false};
    /// Time when the profiler was started.
    std::chrono::steady_clock::time_point startTime_;
    /// This should be called periodically whenever the last seen object ID is
    /// updated.
    std::function<
        void(uint64_t, std::chrono::microseconds, std::vector<HeapStatsUpdate>)>
        fragmentCallback_;
    /// All samples that have been flushed. Only needs the last object ID to be
    /// written to the file.
    std::vector<Fragment> fragments_;

    /// Updates the last fragment to have the current last ID and timestamp,
    /// then calls fragmentCallback_ with both the new fragment and any changed
    /// fragments from freeAlloc.
    void flushCallback();

    /// Find the fragment corresponding to the given id.
    /// \return fragments_.back() if none exists (it's the currently active
    ///   fragment).
    Fragment &findFragmentForID(HeapSnapshot::NodeID id);
  };

  class SamplingAllocationLocationTracker final {
    friend struct RuntimeOffsets;

   public:
    explicit inline SamplingAllocationLocationTracker(GCBase *gc) : gc_(gc) {}

    /// Returns true if tracking is enabled for new allocations.
    bool isEnabled() const {
      return !!dist_;
    }

    /// Must be called by GC implementations whenever a new allocation is made.
    void newAlloc(const GCCell *ptr, uint32_t sz);

    /// Must be called by GC implementations whenever an allocation is freed.
    void freeAlloc(const GCCell *ptr, uint32_t sz);

    /// If an object's size changes, update the entry here.
    void updateSize(const GCCell *ptr, uint32_t oldSize, uint32_t newSize);

    /// Turn the sampling memory profiler on. About once every
    /// \p samplingInterval bytes are allocated, sample the allocation by
    /// recording its stack.
    /// \param seed If non-negative, use as the seed for the random sampling
    ///   mechanism, giving deterministic output.
    void enable(size_t samplingInterval, int64_t seed);

    void disable(llvh::raw_ostream &os);

   private:
    struct Sample final {
      size_t size;
      StackTracesTreeNode *node;
      /// This is the auto-incremented sample ID, not the ID of the object
      /// associated with the sample.
      uint64_t id;
    };

    /// This mutex protects stackMap_ and samples_. Not needed for enabling and
    /// disabling because those only happen while the world is stopped.
    Mutex mtx_;

    GCBase *gc_;

    /// Subtract from this each allocation. If it would underflow below zero,
    /// take a sample.
    size_t limit_{0};

    /// Track all samples that have been taken.
    llvh::DenseMap<HeapSnapshot::NodeID, Sample> samples_;

    /// Use a poisson distribution to decide when to take the next sample.
    std::minstd_rand randomEngine_;
    std::unique_ptr<std::poisson_distribution<>> dist_;

    /// An auto-incrementing integer representing a unique ID for a sample.
    /// Used for ordering samples.
    uint64_t nextSampleID_{1};

    /// \return How many bytes should be waited until the next sample.
    size_t nextSample();
  };
#endif

  class IDTracker final {
   public:
    /// These are IDs that are reserved for special objects.
    enum class ReservedObjectID : HeapSnapshot::NodeID {
      // The ID for the super root object.
      SuperRoot,
      // The ID for the (GC roots) object.
      GCRoots,
#define ROOT_SECTION(name) name,
#include "hermes/VM/RootSections.def"
      IdentifierTableLookupVector,
      IdentifierTableHashTable,
      JSIHermesValueList,
      JSIWeakHermesValueList,
      WeakRefSlotStorage,
      Undefined,
      Null,
      True,
      False,
      Number,
      FirstNonReservedID,
    };

    // 0 is guaranteed to never be a valid node ID.
    static constexpr HeapSnapshot::NodeID kInvalidNode = 0;

    static constexpr HeapSnapshot::NodeID reserved(ReservedObjectID id) {
      // All reserved IDs should be odd numbers to signify that they're JS
      // objects and not native objects. This follows v8's output.
      return static_cast<std::underlying_type<ReservedObjectID>::type>(id) * 2 +
          1;
    }

    /// A comparator for doubles that allows NaN.
    struct DoubleComparator {
      static double getEmptyKey() {
        // Use a non-canonical NaN value as an empty value, which should never
        // occur naturally.
        // NOTE: HermesValue uses NaN tagging internally so we can use that to
        // get the encoding.
        return llvh::BitsToDouble(HermesValue::encodeUndefinedValue().getRaw());
      }
      static double getTombstoneKey() {
        // Use a non-canonical NaN value as the tombstone, which should never
        // occur naturally.
        // NOTE: HermesValue uses NaN tagging internally so we can use that to
        // get the encoding.
        return llvh::BitsToDouble(HermesValue::encodeNullValue().getRaw());
      }
      static unsigned getHashValue(double val) {
        uint64_t bits = llvh::DoubleToBits(val);
        // Representation of small floating values may have many lower bits as
        // 0's. Hash functions that generate 64bits hashes identical to the
        // input integer values (e.g., std::hash) may cause a lot of collisions,
        // since the hash values are truncated to 32bits. The LLVM hash_value
        // function has the nice property that each input bit affects each
        // output bit with close probability, so the hash value after truncating
        // is still good. On a random number set of uniform distribution (with
        // 1/16 being random doubles and the rest integers) in the range of
        // [-1000000, 1000000], with size 80M, this performs pretty well. In
        // practice, most numbers in the heap would be small integers, so use
        // this for now until we see other extreme cases.
        return llvh::hash_value(bits);
      }
      static bool isEqual(double LHS, double RHS) {
        return llvh::DoubleToBits(LHS) == llvh::DoubleToBits(RHS);
      }
    };

    explicit IDTracker();

    /// Return true if Object IDs have been tracked.
    bool hasTrackedObjectIDs();

    /// Return true if Number IDs are being tracked.
    bool isTrackingNumberIDs();

    /// Start assigning unique IDs to numbers passed to \p getNumberID
    void startTrackingNumberIDs();

    /// Stop assigning unique IDs to numbers passed to \p getNumberID
    void stopTrackingNumberIDs();

    /// Get the unique object id of the given object.
    /// If one does not yet exist, start tracking it.
    HeapSnapshot::NodeID getObjectID(CompressedPointer cell);

    /// \return true if the cell has an object ID associated with it, false if
    ///   there is none.
    bool hasObjectID(CompressedPointer cell);

    /// Same as \c getObjectID, except it asserts if the cell doesn't have an
    /// ID.
    HeapSnapshot::NodeID getObjectIDMustExist(CompressedPointer cell);

    /// Get the unique object id of the symbol with the given symbol \p sym. If
    /// one does not yet exist, start tracking it.
    HeapSnapshot::NodeID getObjectID(SymbolID sym);

    /// Get the unique object id of the given native memory (non-JS-heap).
    /// If one does not yet exist, start tracking it.
    HeapSnapshot::NodeID getNativeID(const void *mem);

    /// Get a list of IDs for native memory attached to the given node.
    /// List will be empty if nothing is attached yet. Then push onto the end
    /// with nextNativeID().
    /// When the NodeID that these are attached to is untracked, so are the
    /// attached native NodeIDs.
    llvh::SmallVector<HeapSnapshot::NodeID, 1> &getExtraNativeIDs(
        HeapSnapshot::NodeID node);

    /// If \p isTrackingNumberIDs_ is true, then assign a unique ID to a literal
    /// number value that occurs in the heap. Can be used to make fake nodes
    /// that will display their numeric value. Otherwise if \p
    /// isTrackingNumberIDs_ is false, then simply returns the reserved ID
    /// representing Number.
    HeapSnapshot::NodeID getNumberID(double num);

    /// Get the object pointer for the given ID. This is the inverse of \c
    /// getObjectID.
    /// Returns none if there is no object for that ID.
    llvh::Optional<CompressedPointer> getObjectForID(HeapSnapshot::NodeID id);

    /// Tell the tracker that an object has moved locations.
    /// This must be called in a safe order, if A moves to B, and C moves to A,
    /// the first move must be recorded before the second.
    void moveObject(
        CompressedPointer oldLocation,
        CompressedPointer newLocation);

    /// Remove the object from being tracked. This should be done to keep the
    /// tracking working set small.
    void untrackObject(CompressedPointer cell);

    /// Remove the symbol from being tracked. This needs to be done to allow
    /// symbols to be re-used.
    void untrackSymbol(uint32_t symIdx);

    /// Remove the native memory from being tracked. This should be done to keep
    /// the tracking working set small. It is also required to be done when
    /// malloc'ed memory is freed, since addresses can be re-used by future
    /// allocations.
    void untrackNative(const void *mem);

    /// \return True if this is tracking any native IDs, false if there are no
    ///   native IDs being tracked.
    bool hasNativeIDs();

    /// Get the current last ID. All other existing IDs are less than or equal
    /// to this one.
    HeapSnapshot::NodeID lastID() const;

    /// Get the next unique native ID for a chunk of native memory.
    /// NOTE: public to get assigned native ids without needing to reserve in
    /// advance.
    HeapSnapshot::NodeID nextNativeID();

   private:
    /// Get the next unique object ID for a newly created object.
    HeapSnapshot::NodeID nextObjectID();
    /// Get the next unique number ID for a number.
    HeapSnapshot::NodeID nextNumberID();

    /// JS heap nodes are represented by odd-numbered IDs, while native nodes
    /// are represented with even-numbered IDs. This requirement is enforced by
    /// the Chrome snapshot viewer.
    static constexpr HeapSnapshot::NodeID kIDStep = 2;

    /// This mutex protects objectIDMap_, symbolIDMap_, and numberIDMap_.
    /// Specifically does not protect lastID_, since there's only one allocator
    /// at a time, and only new allocations affect lastID_.
    Mutex mtx_;

    /// The last ID assigned to a non-native object. Object IDs are not
    /// recycled so that snapshots don't confuse two objects with each other.
    /// NOTE: Reserved guarantees that this is an odd number.
    HeapSnapshot::NodeID lastID_{
        reserved(ReservedObjectID::FirstNonReservedID)};

    /// Map of object pointers to IDs. Only populated once the first heap
    /// snapshot is requested, or the first time the memory profiler is turned
    /// on, or if JSI tracing is in effect.
    /// This map's size is O(number of cells in the heap), which can grow quite
    /// large. Using compressed pointers keeps the size small.
    llvh::DenseMap<CompressedPointer::RawType, HeapSnapshot::NodeID>
        objectIDMap_;

    /// The inverse of \c objectIDMap_. Only used for debugging views on heap
    /// snapshots. To avoid wasting memory in the case where the debugger hasn't
    /// requested any, it is populated lazily as each entry is requested. We
    /// expect the vast majority of objects aren't inspected in the snapshot.
    llvh::DenseMap<HeapSnapshot::NodeID, CompressedPointer::RawType>
        idObjectMap_;

    /// Map of native pointers to IDs. Populated according to
    /// the same rules as the objectIDMap_.
    llvh::DenseMap<const void *, HeapSnapshot::NodeID> nativeIDMap_;

    /// Map from a JS heap object ID to additional lazily created IDs for
    /// objects. Most useful for native IDs that are attached to a heap object
    /// but don't have a stable pointer to use (such as std::vector and
    /// llvh::DenseMap).
    llvh::DenseMap<
        HeapSnapshot::NodeID,
        llvh::SmallVector<HeapSnapshot::NodeID, 1>>
        extraNativeIDs_;

    /// Map from symbol indices to unique IDs.  Populated according to
    /// the same rules as the objectIDMap_.
    llvh::DenseMap<uint32_t, HeapSnapshot::NodeID> symbolIDMap_;

    /// Map of numeric values to IDs. Used to give numbers in the heap a unique
    /// node.
    llvh::DenseMap<double, HeapSnapshot::NodeID, DoubleComparator> numberIDMap_;

    /// Whether to assign an unique ID to the number given to \p getNumberID
    bool isTrackingNumberIDs_ = true;
  };

  enum class HeapKind { HadesGC, MallocGC };

  GCBase(
      GCCallbacks &gcCallbacks,
      PointerBase &pointerBase,
      const GCConfig &gcConfig,
      std::shared_ptr<CrashManager> crashMgr,
      HeapKind kind);

  virtual ~GCBase() {
    assert(
        weakMapEntrySlots_.sizeForTests() == 0 &&
        "weakMapEntrySlots_ must all be freed");
    assert(weakSlots_.sizeForTests() == 0 && "weakSlots_ must all be freed");
  }

  /// Create a fixed size object of type T.
  /// \return a pointer to the newly created object in the GC heap.
  template <
      typename T,
      HasFinalizer hasFinalizer = HasFinalizer::No,
      LongLived longLived = LongLived::No,
      class... Args>
  T *makeAFixed(Args &&...args);

  /// Create a variable size object of type T and size \p size.
  /// \return a pointer to the newly created object in the GC heap.
  template <
      typename T,
      HasFinalizer hasFinalizer = HasFinalizer::No,
      LongLived longLived = LongLived::No,
      CanBeLarge canBeLarge = CanBeLarge::No,
      MayFail mayFail = MayFail::No,
      class... Args>
  T *makeAVariable(uint32_t size, Args &&...args);

  template <
      typename T,
      HasFinalizer hasFinalizer = HasFinalizer::No,
      LongLived longLived = LongLived::No,
      CanBeLarge canBeLarge = CanBeLarge::No,
      MayFail mayFail = MayFail::No,
      class... Args>
  T *makeA(uint32_t size, Args &&...args);

  /// Allocate two young gen objects of the size \p size1 + \p size2.
  /// Calls the constructors of types T1 and T2 with the arguments \p t1Args and
  /// \p t2Args respectively.
  /// \pre the TOTAL size must be able to fit in the young gen
  ///  (can be checked with canAllocateInYoungGen(size)).
  /// \post both result pointers are in the young gen.
  /// \return a pointer to size1 size T1, and a pointer to size2 size T2.
  template <typename T1, typename T2, typename... T1Args, typename... T2Args>
  inline std::pair<T1 *, T2 *> make2YoungGenUnsafe(
      uint32_t size1,
      std::tuple<T1Args...> t1Args,
      uint32_t size2,
      std::tuple<T2Args...> t2Args);

  /// Name to identify this heap in logs.
  const std::string &getName() const {
    return name_;
  }

  /// \return the base of pointers in the heap.
  /// NOTE: This normally should not be needed, Runtime provides it.
  /// However in some scenarios there is only a GC available, not a
  /// Runtime. In those cases use this function.
  PointerBase &getPointerBase() const {
    return pointerBase_;
  }

  GCCallbacks &getCallbacks() const {
    return gcCallbacks_;
  }

  /// Forwards to the GC callback \p convertSymbolToUTF8, see documentation
  /// for that function.
  std::string convertSymbolToUTF8(SymbolID id) {
    return gcCallbacks_.convertSymbolToUTF8(id);
  }

  /// Called by the Runtime to inform the GC that it is about to execute JS for
  /// the first time.
  void runtimeWillExecute();

  /// Inform the GC that TTI has been reached.  (In case, for example,
  /// behavior should change at that point.  Default behavior is to do
  /// nothing.)
  virtual void ttiReached() {}

  /// Do anything necessary to record the current number of allocated
  /// objects in numAllocatedObjects_.  Default is to do nothing.
  virtual void recordNumAllocatedObjects() {}

  /// Print any and all collected statistics to the give output stream, \p os.
  void printAllCollectedStats(llvh::raw_ostream &os);

  /// Whether to output GC statistics at the end of execution.
  bool shouldRecordGCStats() const {
    return recordGcStats_;
  }

  /// Total number of collections of any kind.
  unsigned getNumGCs() const {
    return cumStats_.numCollections;
  }

  /// Total wall time in seconds of all pauses due to collections so far.
  double getGCTime() const {
    return cumStats_.gcWallTime.sum();
  }

  /// Total CPU time in seconds of all pauses due to collections so far.
  double getGCCPUTime() const {
    return cumStats_.gcCPUTime.sum();
  }

  GCCallbacks &getGCCallbacks() const {
    return gcCallbacks_;
  }

  /// Cumulative stats over time so far.
  virtual size_t getPeakAllocatedBytes() const {
    return cumStats_.usedBefore.max();
  }
  virtual size_t getPeakLiveAfterGC() const {
    return cumStats_.usedAfter.max();
  }

  /// Populate \p info with information about the heap.
  virtual void getHeapInfo(HeapInfo &info);
  /// Same as \c getHeapInfo, and it adds the amount of malloc memory in use.
  virtual void getHeapInfoWithMallocSize(HeapInfo &info);

  /// Return a reference to the GCExecTrace object, which is used if
  /// we're keeping track of information about GCs, for tracing, for example.
  const GCExecTrace &getGCExecTrace() const;

  /// Populate \p info with crash manager information about the heap
  virtual void getCrashManagerHeapInfo(CrashManager::HeapInformation &info) = 0;

#ifndef NDEBUG
  /// Populate \p info with more detailed information about the heap that is
  /// too expensive to know during production builds.
  virtual void getDebugHeapInfo(DebugHeapInfo &info);

  /// \return Number of weak ref slots currently in use.
  /// Inefficient. For testing/debugging.
  size_t countUsedWeakRefs() const;

  /// Return true if \p ptr is currently pointing at valid accessable memory,
  /// allocated to an object.
  virtual bool validPointer(const void *ptr) const = 0;
#endif

  /// Dump detailed heap contents to the given output stream, \p os.
  virtual void dump(llvh::raw_ostream &os, bool verbose = false);

  /// Run the finalizers for all heap objects.
  virtual void finalizeAll() = 0;

  /// Force a garbage collection cycle. The provided cause will be used in
  /// logging.
  virtual void collect(std::string cause, bool canEffectiveOOM = false) = 0;

  /// Collect with the requested type and execution policy.
  virtual void collectWithOptions(
      std::string &&cause,
      const GCOptions &options) = 0;

  /// Iterate over all objects in the heap, and call \p callback on them.
  /// \param callback A function to call on each found object.
  virtual void forAllObjs(const std::function<void(GCCell *)> &callback) = 0;

  /// \return true if the pointer lives in the young generation.
  virtual bool inYoungGen(const GCCell *) const = 0;
  /// \return true if \p value is a pointer to a cell that lives in the young
  /// generation.
  bool inYoungGen(SmallHermesValue value) const {
    return value.isPointer() && inYoungGen(value.getPointer(pointerBase_));
  }
  inline bool inYoungGen(HermesValue value) const;

  /// Returns whether an external allocation of the given \p size fits
  /// within the maximum heap size. (Note that this does not guarantee that the
  /// allocation will "succeed" -- the size plus the used() of the heap may
  /// still exceed the max heap size. But if it fails, the allocation can never
  /// succeed.)
  virtual bool canAllocExternalMemory(uint32_t size) {
    return true;
  }

  /// Allocate a slot to use in WeakRef.
  /// \pre \p target must hold an Object or non-registered Symbol.
  WeakRefSlot *allocWeakSlot(SmallHermesValue target);

  /// Allocate a slot to use in WeakMap/WeakSet when inserting a new entry.
  /// \param key Pointer to the key object.
  /// \param value The mapped value by the key \p key.
  /// \param owner Pointer to the owning WeakMap/WeakSet.
  WeakMapEntrySlot *allocWeakMapEntrySlot(
      SmallHermesValue key,
      HermesValue value,
      JSWeakMapImplBase *owner);

  /// Set the mapped value of the WeakMap/WeakSet entry \p slot to \p value.
  /// The mutator must use this to update the mapped value of an entry.
  /// \pre \p slot must not be free.
  inline void setWeakMapEntryValue(WeakMapEntrySlot &slot, HermesValue value);

#ifndef NDEBUG
  /// \name Debug APIs
  /// \{
  virtual bool calledByBackgroundThread() const {
    return false;
  }
  virtual bool calledByGC() const {
    return true;
  }
  virtual bool dbgContains(const void *ptr) const = 0;
  virtual void trackReachable(CellKind kind, unsigned sz) {}
  virtual bool needsWriteBarrier(
      const GCHermesValueBase *loc,
      HermesValue value) const = 0;
  virtual bool needsWriteBarrier(
      const GCSmallHermesValueBase *loc,
      SmallHermesValue value) const = 0;
  virtual bool needsWriteBarrier(const GCPointerBase *loc, GCCell *value)
      const = 0;
  virtual bool needsWriteBarrierInCtor(
      const GCHermesValueBase *loc,
      HermesValue value) const = 0;
  virtual bool needsWriteBarrierInCtor(
      const GCSmallHermesValueBase *loc,
      SmallHermesValue value) const = 0;
  /// \}
#endif

  /// Do any logging of info about the heap that is useful, then dies with a
  /// fatal out-of-memory error.
  LLVM_ATTRIBUTE_NORETURN void oom(std::error_code reason);

#ifdef HERMES_MEMORY_INSTRUMENTATION
  /// An edges counter array for each root section. The counter is uninitialized
  /// if a root section is not visited yet.
  using SavedNumRootEdges = std::array<
      llvh::Optional<HeapSizeType>,
      static_cast<unsigned>(RootSectionAcceptor::Section::NumSections)>;

  /// Creates a snapshot of the heap, which includes information about what
  /// objects exist, their sizes, and what they point to.
  virtual void createSnapshot(
      llvh::raw_ostream &os,
      bool captureNumericValue) = 0;
  void createSnapshot(GC &gc, llvh::raw_ostream &os, bool captureNumericValue);
  /// Actual implementation of writing the snapshot. If \p numRootEdges is
  /// uninitialized, it will be populated with the edge counts for each root
  /// section. Otherwise, it will be used to pad the output with additional
  /// edges if necessary so they match the recorded count.
  void createSnapshotImpl(
      GC &gc,
      HeapSnapshot &snap,
      SavedNumRootEdges &numRootEdges);

  /// Subclasses can override and add more specific native memory usage.
  virtual void snapshotAddGCNativeNodes(HeapSnapshot &snap);

  /// Subclasses can override and add more specific edges.
  virtual void snapshotAddGCNativeEdges(HeapSnapshot &snap);

  /// Turn on the heap profiler, which will track when allocations are made and
  /// the stack trace of when they were created.
  virtual void enableHeapProfiler(
      std::function<void(
          uint64_t,
          std::chrono::microseconds,
          std::vector<GCBase::AllocationLocationTracker::HeapStatsUpdate>)>
          fragmentCallback);

  /// Turn off the heap profiler, which will stop tracking new allocations and
  /// not record any stack traces.
  /// Disabling will not forget any objects that are still alive. This way
  /// re-enabling later will still remember earlier objects.
  virtual void disableHeapProfiler();

  /// Turn the sampling memory profiler on. About once every
  /// \p samplingInterval bytes are allocated, sample the allocation by
  /// recording its stack.
  /// \param seed If non-negative, use as the seed for the random sampling
  ///   mechanism, giving deterministic output.
  virtual void enableSamplingHeapProfiler(
      size_t samplingInterval,
      int64_t seed);

  /// Turn off the sampling heap profiler, which will stop tracking new
  /// allocations and not record any stack traces. Write out the results of the
  /// trace to \p os. After this call, any remembered data about sampled objects
  /// will be gone.
  virtual void disableSamplingHeapProfiler(llvh::raw_ostream &os);
#endif // HERMES_MEMORY_INSTRUMENTATION

  /// Inform the GC about external memory retained by objects.
  virtual void creditExternalMemory(GCCell *alloc, uint32_t size) = 0;
  virtual void debitExternalMemory(GCCell *alloc, uint32_t size) = 0;

  /// @name Marking APIs
  /// @{

  /// Marks a cell by its metadata.
  /// \p cell The heap object to mark.
  /// \p acceptor The action to perform on each slot in the cell.
  template <typename Acceptor>
  inline void markCell(Acceptor &acceptor, GCCell *cell);

  /// Marks a cell by its metadata, but only for the slots that point between
  /// [begin, end).
  template <typename Acceptor>
  inline void markCellWithinRange(
      Acceptor &acceptor,
      GCCell *cell,
      const char *begin,
      const char *end);

  /// Marks a cell by its metadata, and outputs the names of the slots.
  /// Meant to be used by heap snapshots.
  template <typename Acceptor>
  inline void markCellWithNames(Acceptor &acceptor, GCCell *cell);

  /// @}

  /// If false, all reachable objects in the heap will have a dereference-able
  /// VTable pointer which describes its type and size. If true, both reachable
  /// objects and unreachable objects may not have dereference-able VTable
  /// pointers, and any reads from JS heap memory may give strange results.
  bool inGC() const {
    return inGC_;
  }

  HeapKind getKind() const {
    return heapKind_;
  }

  /// \return A string representation of the kind of GC.
  virtual std::string getKindAsStr() const = 0;

  bool isTrackingIDs() {
#ifdef HERMES_MEMORY_INSTRUMENTATION
    return getIDTracker().hasTrackedObjectIDs() ||
        getAllocationLocationTracker().isEnabled() ||
        getSamplingAllocationTracker().isEnabled();
#else
    return getIDTracker().hasTrackedObjectIDs();
#endif
  }

  IDTracker &getIDTracker() {
    return idTracker_;
  }

#ifdef HERMES_MEMORY_INSTRUMENTATION
  AllocationLocationTracker &getAllocationLocationTracker() {
    return allocationLocationTracker_;
  }

  SamplingAllocationLocationTracker &getSamplingAllocationTracker() {
    return samplingAllocationTracker_;
  }
#endif

  /// \name Snapshot ID methods
  /// \{
  // This set of methods are all mirrors of IDTracker, except with pointer
  // compression done automatically.
  HeapSnapshot::NodeID getObjectID(const GCCell *cell);
  HeapSnapshot::NodeID getObjectIDMustExist(const GCCell *cell);
  HeapSnapshot::NodeID getObjectID(CompressedPointer cell);
  HeapSnapshot::NodeID getObjectID(SymbolID sym);
  HeapSnapshot::NodeID getNativeID(const void *mem);
  /// \return The ID for the given value. If the value cannot be represented
  ///   with an ID, returns None.
  llvh::Optional<HeapSnapshot::NodeID> getSnapshotID(HermesValue val);

  /// \return The object pointer for the given \p id. This is quite slow.
  ///   If there is no such object this returns nullptr.
  void *getObjectForID(HeapSnapshot::NodeID id);

  /// \return True if the given cell has an ID associated with it.
  bool hasObjectID(const GCCell *cell);
  /// Records that a new allocation has occurred.
  void newAlloc(const GCCell *ptr, uint32_t sz);
  /// Moves an object to a new address and a new size for all trackers.
  void moveObject(
      const GCCell *oldPtr,
      uint32_t oldSize,
      const GCCell *newPtr,
      uint32_t newSize);
  /// Untracks a freed object from all trackers.
  void untrackObject(const GCCell *cell, uint32_t sz);
  /// \}

#ifndef NDEBUG
  /// \return The next debug allocation ID for embedding directly into a GCCell.
  /// NOTE: This is not the same ID as is used for stable lifetime tracking, use
  /// \p getObjectID for that.
  uint64_t nextObjectID();
#endif

// Mangling scheme used by MSVC encode public/private into the name.
// As a result, vanilla "ifdef public" trick leads to link errors.
#if defined(UNIT_TEST) || defined(_MSC_VER)
 public:
#else
 protected:
#endif

  /// dataSize is the live data in bytes, now is the current time point. The
  /// function checks these parameters against the limits set at initialisation.
  /// If the conditions are met, the tripwire is triggered and tripwireCallback_
  /// is called.
  void checkTripwire(size_t dataSize);

  // Visibility here is public for unit_tests and protected otherwise

 protected:
  /// An RAII-style object used to denote regions when a GC cycle is considered
  /// active.
  class GCCycle final {
   public:
    explicit GCCycle(GCBase &gc, std::string extraInfo = "");
    ~GCCycle();

    const std::string &extraInfo() {
      return extraInfo_;
    }

   private:
    GCBase &gc_;
    std::string extraInfo_;
    bool previousInGC_;
  };

  /// Returns the number of bytes allocated allocated since the last GC.
  /// TODO: Implement this for heaps other than GenGC
  /// (at which point this can become an abstract function).
  virtual gcheapsize_t bytesAllocatedSinceLastGC() const {
    return 0;
  }

  /// Convenience method to invoke the mark roots function provided at
  /// initialization, using the context provided then (on this heap).
  /// The \p markLongLived argument indicates whether root data structures
  /// containing only pointers to objects allocated via allocLongLived
  /// are required to be marked.  In this collector, such objects will
  /// be allocated in the old gen, and references to them need not be
  /// marked during young-gen collection.
  void markRoots(RootAcceptorWithNames &acceptor, bool markLongLived) {
    gcCallbacks_.markRoots(acceptor, markLongLived);
  }

  /// Convenience method to invoke the mark weak roots function provided at
  /// initialization, using the context provided then (on this heap), and to
  /// mark the weak slots. If \p onlyRecordedSlots is true, only mark the slots
  /// recorded in weakSlots_ and weakMapEntrySlots_ (see
  /// ManagedChunkedList::forEachRecorded).
  void markWeakRoots(
      WeakRootAcceptor &acceptor,
      bool markLongLived,
      bool onlyRecordedSlots = false) {
    gcCallbacks_.markWeakRoots(acceptor, markLongLived);
    acceptor.beginRootSection(RootAcceptor::Section::WeakRefSlots);
    auto markWeakRefSlot = [&acceptor](WeakRefSlot &slot) {
      slot.markWeakRoots(acceptor);
    };
    auto markWeakMapEntrySlot = [&acceptor](WeakMapEntrySlot &slot) {
      slot.markWeakRoots(acceptor);
    };
    if (onlyRecordedSlots) {
      weakSlots_.forEachRecorded(markWeakRefSlot);
      weakMapEntrySlots_.forEachRecorded(markWeakMapEntrySlot);
    } else {
      weakSlots_.forEach(markWeakRefSlot);
      weakMapEntrySlots_.forEach(markWeakMapEntrySlot);
    }
    acceptor.endRootSection();
  }

  /// Print the cumulative statistics.
  virtual void printStats(JSONEmitter &json);

  /// Extended GCAnalyticsEvent with additional fields for internal use.
  struct InternalAnalyticsEvent : public GCAnalyticsEvent {
    /// Higher resolution execution time stats. These should be preferred for
    /// internal use, but the old millisecond resolution fields are kept in the
    /// public base class for backward compatibility.
    double durationSecs;
    double cpuDurationSecs;
  };

  /// Record statistics from a single GC, which are specified in the given
  /// \p event, in the overall cumulative stats struct.
  /// \param fromNewCollection If true, it means this is called from a new
  /// collection cycle, otherwise, it's called while waiting for an existing
  /// collection to finish.
  void recordGCStats(
      const InternalAnalyticsEvent &event,
      bool onMutator,
      bool fromNewCollection);

  /// Record statistics from a single GC, which are specified in the given
  /// \p event, in the given cumulative stats struct.
  /// \param fromNewCollection If true, it means this is called from a new
  /// collection cycle, otherwise, it's called while waiting for an existing
  /// collection to finish.
  void recordGCStats(
      const InternalAnalyticsEvent &event,
      CumulativeHeapStats *stats,
      bool onMutator,
      bool fromNewCollection);

  /// Print detailed stats of the breakdown of the roots and heap in terms of
  /// the number of pointers, symbols, HermesValues, etc.
  void sizeDiagnosticCensus(size_t allocatedBytes);

  /// Do any additional GC-specific logging that is useful before dying with
  /// out-of-memory. Takes a char buffer to avoid dynamic allocations.
  virtual void oomDetail(
      llvh::MutableArrayRef<char> detailBuffer,
      std::error_code reason);

  template <typename T, class... Args>
  static T *constructCell(void *ptr, uint32_t size, Args &&...args) {
    auto *cell = new (ptr) T(std::forward<Args>(args)...);
    constexpr auto kind = T::getCellKind();
    cell->setKindAndSize({kind, size});
    return cell;
  }

  /// Number of finalized objects in the last collection.
  unsigned numFinalizedObjects_{0};

  /// The total number of bytes allocated in the execution.
  uint64_t totalAllocatedBytes_{0};

  /// A trace of GC execution.
  GCExecTrace execTrace_;

/// These fields are not available in optimized builds.
#ifndef NDEBUG
  /// Number of currently allocated objects present in the heap before the start
  /// of the last collection.  Some may be unreachable.
  unsigned numAllocatedObjects_{0};
  /// Number of reachable objects in the last collection.  (More properly, this
  /// is the number not known to be unreachable: if a GC does not consider
  /// determine the reachability of some subset of objects, for example, an old
  /// generation in a generational collection, those objects should be included
  /// in this count.)
  unsigned numReachableObjects_{0};
  /// Number of collected objects in the last collection.  Equal to
  /// numAllocatedObjects_ (at the start of the last collection),
  /// minus numReachableObjects_ found in that collection.
  unsigned numCollectedObjects_{0};
  /// Number of marked symbols.
  unsigned numMarkedSymbols_{0};
  /// Number of hidden classes alive after the last collection.
  unsigned numHiddenClasses_{0};
  /// Number of "leaf" hidden classes alive after the last collection.
  unsigned numLeafHiddenClasses_{0};

  /// Associate a semi-unique (until it overflows) id with every allocation
  /// for easier identification when debugging.
  uint64_t debugAllocationCounter_{0};
#endif

  /// User-supplied callbacks invoked by the GC to query information or perform
  /// tasks.
  GCCallbacks &gcCallbacks_;

  /// Base of all pointers in compressed pointers implementation.
  PointerBase &pointerBase_;

  /// A place to log crash data if a crash is about to occur.
  std::shared_ptr<CrashManager> crashMgr_;

  HeapKind heapKind_;

  /// Callback called once for each GC event that wants to be logged. Can be
  /// null if no analytics are requested.
  std::function<void(const GCAnalyticsEvent &)> analyticsCallback_;

  /// Whether to output GC statistics at the end of execution.
  bool recordGcStats_{false};

  /// Whether or not a GC cycle is currently occurring.
  bool inGC_{false};

  /// Whether the GC has OOMed. Currently only useful for understanding crash
  /// dumps, particularly when HERMESVM_EXCEPTION_ON_OOM is set.
  bool hasOOMed_{false};

  /// The block of fields below records values of various metrics at
  /// the start of execution, so that we can get the values at the end
  /// and subtract.  The "runtimeWillExecute" method is called at
  /// first bytecode execution, but also when executing the bodies of
  /// other bundles.  We want to record these at the first such call.
  /// This field tells whether these values have been recorded yet.
  bool execStartTimeRecorded_{false};

  /// Time at which execution of the Hermes VM began.
  std::chrono::time_point<std::chrono::steady_clock> execStartTime_;
  std::chrono::microseconds execStartCPUTime_;
  /// Number of context switches before execution of the Hermes VM began.
  long startNumVoluntaryContextSwitches_{0};
  long startNumInvoluntaryContextSwitches_{0};

  // The cumulative GC stats.
  CumulativeHeapStats cumStats_;

  /// Name to identify this heap in logs.
  std::string name_;

  /// weakSlots_ is a list of all the weak pointers in the system. They are
  /// invalidated if they point to an object that is dead, and do not count
  /// towards whether an object is live or dead.
  /// The referent of a slot is set when the slot is allocated, and afterwards
  /// only the GC updates it. Since objects never move into the YG, in HadesGC
  /// only the slots allocated with a YG referent since the last YG collection
  /// can point into the YG. allocWeakSlot records them, so that non-compacting
  /// YG collections only need to visit the recorded slots. MallocGC does not
  /// use or clear the records, so the list ends up dropping them, and then
  /// stops recording (see ManagedChunkedList::record).
  ManagedChunkedList<WeakRefSlot> weakSlots_;

  /// A list of all slots used by WeakMap/WeakSet. They are freed by the mutator
  /// when operating on a WeakMap/WeakSet, or in the finalizer during sweeping.
  /// In collection phase, GC visits each non-free slot to update their values.
  /// As for weakSlots_, the slots whose key, owner or mapped value was set to a
  /// YG object since the last YG collection are recorded (by
  /// allocWeakMapEntrySlot and setWeakMapEntryValue), so that non-compacting
  /// YG collections of HadesGC only need to visit the recorded slots.
  ManagedChunkedList<WeakMapEntrySlot> weakMapEntrySlots_;

  /// Tracks what objects need a stable identity for features such as heap
  /// snapshots and the memory profiler.
  IDTracker idTracker_;

#ifdef HERMES_MEMORY_INSTRUMENTATION
  /// Attaches stack-traces to objects when enabled.
  AllocationLocationTracker allocationLocationTracker_;

  /// Attaches stack-traces to objects when enabled.
  SamplingAllocationLocationTracker samplingAllocationTracker_;
#endif

#ifndef NDEBUG
  /// The number of reasons why no allocation is allowed in this heap right
  /// now.
  uint32_t noAllocLevel_{0};

  friend class NoAllocScope;
#endif

#if HERMESVM_SANITIZE_HANDLES != 0
  /// \return true if we should run handle sanitization and the coin flip with
  /// probability sanitizeRate_ has passed.
  bool shouldSanitizeHandles();

  /// Whether to keep moving the heap around to detect unsanitary GC handles.
  double sanitizeRate_{1.0};

  /// PRNG for sanitizing at a less than 1.0 rate.
  std::minstd_rand randomEngine_;
#else
  /// Sanitize handles is completely disabled (and ignored at runtime) without
  /// a special build mode.
  static constexpr double sanitizeRate_{0.0};

  static constexpr bool shouldSanitizeHandles() {
    return false;
  }
#endif

 private:
  template <typename T, XorPtrKeyID K>
  friend class XorPtr;

  /// Randomly generated key used to obfuscate pointers in XorPtr.
  uintptr_t pointerEncryptionKey_[XorPtrKeyID::_NumKeys];

  /// Callback called if it's not null when the Live Data Tripwire is
  /// triggered.
  std::function<void(GCTripwireContext &)> tripwireCallback_;

  /// Maximum size limit before the heap size tripwire will trigger.
  gcheapsize_t tripwireLimit_;

  /// True if the tripwire has already been called on this heap.
  bool tripwireCalled_{false};
};

inline bool GCBase::inYoungGen(HermesValue value) const {
  return value.isPointer() &&
      inYoungGen(static_cast<const GCCell *>(value.getPointer()));
}

inline void GCBase::setWeakMapEntryValue(
    WeakMapEntrySlot &slot,
    HermesValue value) {
  assert(!slot.isFree() && "Setting the mapped value of a free slot");
  slot.mappedValue = value;
  // Record the slot if its new mapped value is in the YG (see
  // weakMapEntrySlots_).
  if (inYoungGen(value))
    weakMapEntrySlots_.record(slot);
}

// Utilities for formatting time durations and memory sizes.

/// An object that, written to an ostream, formats the given # of
/// secs in appropriate units (down to microseconds).
struct DurationFormatObj {
  double secs;
};
llvh::raw_ostream &operator<<(
    llvh::raw_ostream &os,
    const DurationFormatObj &dfo);
inline DurationFormatObj formatSecs(double secs) {
  return {secs};
}

/// An object that, written to an ostream, formats the given # of
/// bytes in appropriate units (bytes to GiB).
struct SizeFormatObj {
  uint64_t bytes;
};
llvh::raw_ostream &operator<<(llvh::raw_ostream &os, const SizeFormatObj &sfo);
inline SizeFormatObj formatSize(uint64_t size) {
  return {size};
}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_GCBASE_H
