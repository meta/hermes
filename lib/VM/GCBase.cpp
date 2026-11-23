/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "gc"
#include "hermes/VM/GC.h"

#include "hermes/Platform/Logging.h"
#include "hermes/Public/JSOutOfMemoryError.h"
#include "hermes/Support/ErrorHandling.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/JSWeakMapImpl.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/SmallHermesValue-inline.h"
#include "hermes/VM/VTable.h"

#include "llvh/Support/Debug.h"
#include "llvh/Support/FileSystem.h"
#include "llvh/Support/Format.h"
#include "llvh/Support/raw_os_ostream.h"
#include "llvh/Support/raw_ostream.h"

#include <inttypes.h>
#include <clocale>
#include <stdexcept>
#include <system_error>

using llvh::format;

namespace hermes {
namespace vm {

const char GCBase::kNaturalCauseForAnalytics[] = "natural";
const char GCBase::kHandleSanCauseForAnalytics[] = "handle-san";

GCBase::GCBase(
    GCCallbacks &gcCallbacks,
    PointerBase &pointerBase,
    const GCConfig &gcConfig,
    std::shared_ptr<CrashManager> crashMgr,
    HeapKind kind)
    : gcCallbacks_(gcCallbacks),
      pointerBase_(pointerBase),
      crashMgr_(crashMgr),
      heapKind_(kind),
      analyticsCallback_(gcConfig.getAnalyticsCallback()),
      recordGcStats_(gcConfig.getShouldRecordStats()),
      // Start off not in GC.
      inGC_(false),
      name_(gcConfig.getName()),
      weakSlots_(gcConfig.getOccupancyTarget(), 0.5 /* sizingWeight */),
      weakMapEntrySlots_(gcConfig.getOccupancyTarget(), 0.5 /* sizingWeight */),
#ifdef HERMES_MEMORY_INSTRUMENTATION
      allocationLocationTracker_(this),
      samplingAllocationTracker_(this),
#endif
#if HERMESVM_SANITIZE_HANDLES != 0
      sanitizeRate_(gcConfig.getSanitizeConfig().getSanitizeRate()),
#endif
      tripwireCallback_(gcConfig.getTripwireConfig().getCallback()),
      tripwireLimit_(gcConfig.getTripwireConfig().getLimit()) {
  for (unsigned i = 0; i < (unsigned)XorPtrKeyID::_NumKeys; ++i) {
    pointerEncryptionKey_[i] = std::random_device()();
    if constexpr (sizeof(uintptr_t) >= 8) {
      // std::random_device() yields an unsigned int, so combine two.
      pointerEncryptionKey_[i] =
          (pointerEncryptionKey_[i] << 32) | std::random_device()();
    }
  }
  buildMetadataTable();
#ifdef HERMESVM_PLATFORM_LOGGING
  hermesLog(
      "HermesGC",
      "Initialisation (Init: %dMB, Max: %dMB, Tripwire: %dMB)",
      gcConfig.getInitHeapSize() >> 20,
      gcConfig.getMaxHeapSize() >> 20,
      gcConfig.getTripwireConfig().getLimit() >> 20);
#endif // HERMESVM_PLATFORM_LOGGING
#if HERMESVM_SANITIZE_HANDLES != 0
  const std::minstd_rand::result_type seed =
      gcConfig.getSanitizeConfig().getRandomSeed() >= 0
      ? gcConfig.getSanitizeConfig().getRandomSeed()
      : std::random_device()();
  if (sanitizeRate_ > 0.0 && sanitizeRate_ < 1.0) {
    llvh::errs()
        << "Warning: you are using handle sanitization with random sampling.\n"
        << "Sanitize Rate: ";
    llvh::write_double(llvh::errs(), sanitizeRate_, llvh::FloatStyle::Percent);
    llvh::errs() << "\n"
                 << "Sanitize Rate Seed: " << seed << "\n"
                 << "Re-run with -gc-sanitize-handles-random-seed=" << seed
                 << " for deterministic crashes.\n";
  }
  randomEngine_.seed(seed);
#endif
}

GCBase::GCCycle::GCCycle(GCBase &gc, std::string extraInfo)
    : gc_(gc), extraInfo_(std::move(extraInfo)), previousInGC_(gc_.inGC_) {
  if (!previousInGC_) {
    gc_.getCallbacks().onGCEvent(GCEventKind::CollectionStart, extraInfo_);
    gc_.inGC_ = true;
  }
}

GCBase::GCCycle::~GCCycle() {
  if (!previousInGC_) {
    gc_.inGC_ = false;
    gc_.getCallbacks().onGCEvent(GCEventKind::CollectionEnd, extraInfo_);
  }
}

void GCBase::runtimeWillExecute() {
  if (recordGcStats_ && !execStartTimeRecorded_) {
    execStartTime_ = std::chrono::steady_clock::now();
    execStartCPUTime_ = oscompat::thread_cpu_time();
    oscompat::num_context_switches(
        startNumVoluntaryContextSwitches_, startNumInvoluntaryContextSwitches_);
    execStartTimeRecorded_ = true;
  }
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
namespace {

constexpr HeapSnapshot::NodeID objectIDForRootSection(
    RootAcceptor::Section section) {
  // Since root sections start at zero, and in IDTracker the root sections
  // start one past the reserved GC root, this number can be added to
  // do conversions.
  return GCBase::IDTracker::reserved(
      static_cast<GCBase::IDTracker::ReservedObjectID>(
          static_cast<HeapSnapshot::NodeID>(
              GCBase::IDTracker::ReservedObjectID::GCRoots) +
          1 + static_cast<HeapSnapshot::NodeID>(section)));
}

// Abstract base class for all snapshot acceptors.
struct SnapshotAcceptor : public RootAcceptorWithNames {
  SnapshotAcceptor(PointerBase &base, HeapSnapshot &snap)
      : pointerBase_(base), snap_(snap) {}

  using RootAcceptorWithNames::accept;

  virtual void acceptHV(HermesValue &hv, const char *name) {
    if (hv.isPointer()) {
      GCCell *ptr = static_cast<GCCell *>(hv.getPointer());
      accept(ptr, name);
    }
  }
  virtual void acceptSHV(SmallHermesValue &hv, const char *name) {
    if (hv.isPointer()) {
      GCCell *ptr = static_cast<GCCell *>(hv.getPointer(pointerBase_));
      accept(ptr, name);
    }
  }

  virtual void acceptSym(SymbolID, const char *) {}

  void accept(PinnedHermesValue &hv, const char *name) override {
    assert((!hv.isPointer() || hv.getPointer()) && "Value is not nullable.");
    acceptHV(hv, name);
  }
  void acceptNullable(PinnedHermesValue &hv, const char *name) override {
    acceptHV(hv, name);
  }
  void accept(PinnedSmallHermesValue &shv, const char *name) override {
    acceptSHV(shv, name);
  }
  void accept(const RootSymbolID &sym, const char *name) override {
    acceptSym(sym, name);
  }

  void accept(GCPointerBase &ptr, const char *name) {
    auto *p = ptr.get(pointerBase_);
    accept(p, name);
  }
  void accept(GCHermesValueBase &hv, const char *name) {
    acceptHV(hv, name);
  }
  void accept(GCSmallHermesValueBase &hv, const char *name) {
    acceptSHV(hv, name);
  }
  void accept(const GCSymbolID &sym, const char *name) {}

 protected:
  PointerBase &pointerBase_;
  HeapSnapshot &snap_;
};

struct PrimitiveNodeAcceptor : public SnapshotAcceptor {
  using SnapshotAcceptor::accept;

  PrimitiveNodeAcceptor(
      PointerBase &base,
      HeapSnapshot &snap,
      GCBase::IDTracker &tracker)
      : SnapshotAcceptor(base, snap), tracker_(tracker) {}

  // Do nothing for any value except a number.
  void accept(GCCell *&ptr, const char *name) override {}

  void acceptHV(HermesValue &hv, const char *) override {
    if (tracker_.isTrackingNumberIDs() && hv.isNumber()) {
      seenNumbers_.insert(hv.getNumber());
    }
  }

  void acceptSHV(SmallHermesValue &hv, const char *) override {
    if (tracker_.isTrackingNumberIDs() && hv.isNumber()) {
      seenNumbers_.insert(hv.getNumber(pointerBase_));
    }
  }

  void writeAllNodes() {
    // Always write out the nodes for singletons.
    snap_.beginNode();
    snap_.endNode(
        HeapSnapshot::NodeType::Object,
        "undefined",
        GCBase::IDTracker::reserved(
            GCBase::IDTracker::ReservedObjectID::Undefined),
        0,
        0);
    snap_.beginNode();
    snap_.endNode(
        HeapSnapshot::NodeType::Object,
        "null",
        GCBase::IDTracker::reserved(GCBase::IDTracker::ReservedObjectID::Null),
        0,
        0);
    snap_.beginNode();
    snap_.endNode(
        HeapSnapshot::NodeType::Object,
        "true",
        GCBase::IDTracker::reserved(GCBase::IDTracker::ReservedObjectID::True),
        0,
        0);
    snap_.beginNode();
    snap_.endNode(
        HeapSnapshot::NodeType::Object,
        "false",
        GCBase::IDTracker::reserved(GCBase::IDTracker::ReservedObjectID::False),
        0,
        0);
    if (tracker_.isTrackingNumberIDs()) {
      for (double num : seenNumbers_) {
        // A number never has any edges, so just make a node for it.
        snap_.beginNode();
        // Convert the number value to a string, according to the JS conversion
        // routines.
        char buf[hermes::NUMBER_TO_STRING_BUF_SIZE];
        snap_.endNode(
            HeapSnapshot::NodeType::Number,
            hermes::numberToString(num, buf, sizeof(buf)),
            tracker_.getNumberID(num),
            // Numbers are zero-sized in the heap because they're stored inline.
            0,
            0);
      }
    } else {
      snap_.beginNode();
      snap_.endNode(
          HeapSnapshot::NodeType::Object,
          "number",
          GCBase::IDTracker::reserved(
              GCBase::IDTracker::ReservedObjectID::Number),
          0,
          0);
    }
  }

 private:
  GCBase::IDTracker &tracker_;
  // Track all numbers that are seen in a heap pass, and only emit one node for
  // each of them.
  llvh::DenseSet<double, GCBase::IDTracker::DoubleComparator> seenNumbers_;
};

struct EdgeAddingAcceptor : public SnapshotAcceptor {
  using SnapshotAcceptor::accept;

  EdgeAddingAcceptor(GCBase &gc, HeapSnapshot &snap)
      : SnapshotAcceptor(gc.getPointerBase(), snap), gc_(gc) {}

  void accept(GCCell *&ptr, const char *name) override {
    if (!ptr) {
      return;
    }
    snap_.addNamedEdge(
        HeapSnapshot::EdgeType::Internal,
        llvh::StringRef::withNullAsEmpty(name),
        gc_.getObjectID(ptr));
  }

  void acceptHV(HermesValue &hv, const char *name) override {
    if (auto id = gc_.getSnapshotID(hv)) {
      snap_.addNamedEdge(
          HeapSnapshot::EdgeType::Internal,
          llvh::StringRef::withNullAsEmpty(name),
          id.getValue());
    }
  }

  void acceptSHV(SmallHermesValue &shv, const char *name) override {
    HermesValue hv = shv.toHV(pointerBase_);
    acceptHV(hv, name);
  }

  void acceptSym(SymbolID sym, const char *name) override {
    if (sym.isInvalid()) {
      return;
    }
    snap_.addNamedEdge(
        HeapSnapshot::EdgeType::Internal,
        llvh::StringRef::withNullAsEmpty(name),
        gc_.getObjectID(sym));
  }

 private:
  GCBase &gc_;
};

struct SnapshotRootSectionAcceptor : public SnapshotAcceptor,
                                     public WeakRootAcceptor {
  using SnapshotAcceptor::accept;
  using WeakRootAcceptor::acceptWeak;

  SnapshotRootSectionAcceptor(PointerBase &base, HeapSnapshot &snap)
      : SnapshotAcceptor(base, snap) {}

  void accept(GCCell *&, const char *) override {
    // While adding edges to root sections, there's no need to do anything for
    // pointers.
  }

  void acceptWeak(WeakRootBase &ptr) override {
    // Same goes for weak pointers.
  }

  void acceptWeakSym(WeakRootSymbolID &ws) override {
    // Same goes for weak symbols.
  }

  void acceptWeak(WeakSmallHermesValue &wshv) override {
    // Same goes for weak pointers/symbols.
  }

  void beginRootSection(Section section) override {
    // Make an element edge from the super root to each root section.
    snap_.addIndexedEdge(
        HeapSnapshot::EdgeType::Element,
        rootSectionNum_++,
        objectIDForRootSection(section));
  }

  void endRootSection() override {
    // Do nothing for the end of the root section.
  }

 private:
  // v8's roots start numbering at 1.
  int rootSectionNum_{1};
};

struct SnapshotRootAcceptor : public SnapshotAcceptor, public WeakRootAcceptor {
  using SnapshotAcceptor::accept;
  using WeakRootAcceptor::acceptWeak;

  SnapshotRootAcceptor(
      GCBase &gc,
      HeapSnapshot &snap,
      GCBase::SavedNumRootEdges &numRootEdges)
      : SnapshotAcceptor(gc.getPointerBase(), snap),
        gc_(gc),
        numRootEdges_(numRootEdges) {}

  void accept(GCCell *&ptr, const char *name) override {
    pointerAccept(ptr, name, false);
  }

  void acceptWeak(WeakRootBase &wr) override {
    auto *ptr = wr.getNoBarrierUnsafe(gc_.getPointerBase());
    pointerAccept(ptr, nullptr, true);
  }

  void acceptWeak(WeakSmallHermesValue &wshv) override {
    if (wshv.isPointer()) {
      GCCell *ptr = wshv.getPointerNoBarrierUnsafe(gc_.getPointerBase());
      pointerAccept(ptr, nullptr, true);
    }
    if (wshv.isSymbol()) {
      acceptSym(wshv.getSymbolNoBarrierUnsafe(), nullptr, true);
    }
  }

  void acceptWeakSym(WeakRootSymbolID &ws) override {
    acceptSym(ws.getNoBarrierUnsafe(), nullptr, true);
  }

  void acceptSym(SymbolID sym, const char *name) override {
    acceptSym(sym, name, false);
  }

  void acceptSym(SymbolID sym, const char *name, bool weak) {
    if (sym.isInvalid()) {
      return;
    }
    auto nameRef = llvh::StringRef::withNullAsEmpty(name);
    const auto id = gc_.getObjectID(sym);
    if (!nameRef.empty()) {
      snap_.addNamedEdge(
          weak ? HeapSnapshot::EdgeType::Weak
               : HeapSnapshot::EdgeType::Internal,
          nameRef,
          id);
    } else if (weak) {
      // TODO: Figure out if we can just add these weak edges as indexed instead
      // of named
      std::string numericName = std::to_string(nextEdge_++);
      snap_.addNamedEdge(HeapSnapshot::EdgeType::Weak, numericName.c_str(), id);
    } else {
      // Unnamed edges get indices.
      snap_.addIndexedEdge(HeapSnapshot::EdgeType::Element, nextEdge_++, id);
    }
  }

  void provideSnapshot(
      const std::function<void(HeapSnapshot &)> &func) override {
    func(snap_);
  }

  void beginRootSection(Section section) override {
    assert(
        currentSection_ == Section::InvalidSection &&
        "beginRootSection called while previous section is open");
    snap_.beginNode();
    currentSection_ = section;
  }

  void endRootSection() override {
    // A root section creates a synthetic node with that name and makes edges
    // come from that root.
    static const char *rootNames[] = {
// Parentheses around the name is adopted from V8's roots.
#define ROOT_SECTION(name) "(" #name ")",
#include "hermes/VM/RootSections.def"
    };

    // If we haven't visited this section before, save its current edge count.
    auto sectionIdx = static_cast<unsigned>(currentSection_);
    if (!numRootEdges_[sectionIdx].hasValue()) {
      numRootEdges_[sectionIdx] = snap_.getCurEdgeCount();
    } else {
      // Compare the edge count of this scan with the first scan, if some roots
      // are newly dropped, we will add dummy edges to make sure all following
      // scans have the same edge count as the first scan in a single call of
      // createSnapshot().
      auto savedEdgeCount = numRootEdges_[sectionIdx].getValue();
      assert(
          savedEdgeCount >= snap_.getCurEdgeCount() &&
          "Unexpected new edges added");
      const auto id = GCBase::IDTracker::reserved(
          GCBase::IDTracker::ReservedObjectID::Undefined);
      for (auto i = snap_.getCurEdgeCount(); i < savedEdgeCount; ++i) {
        snap_.addIndexedEdge(HeapSnapshot::EdgeType::Element, nextEdge_++, id);
      }
    }

    snap_.endNode(
        HeapSnapshot::NodeType::Synthetic,
        rootNames[static_cast<unsigned>(currentSection_)],
        objectIDForRootSection(currentSection_),
        // The heap visualizer doesn't like it when these synthetic nodes have a
        // size (it describes them as living in the heap).
        0,
        0);
    currentSection_ = Section::InvalidSection;
    // Reset the edge counter, so each root section's unnamed edges start at
    // zero.
    nextEdge_ = 0;
  }

 private:
  GCBase &gc_;
  llvh::DenseSet<HeapSnapshot::NodeID> seenIDs_;
  /// For unnamed edges, use indices instead.
  unsigned nextEdge_{0};
  Section currentSection_{Section::InvalidSection};
  /// Number of edges for each root section.
  GCBase::SavedNumRootEdges &numRootEdges_;

  void pointerAccept(GCCell *ptr, const char *name, bool weak) {
    assert(
        currentSection_ != Section::InvalidSection &&
        "accept called outside of begin/end root section pair");
    if (!ptr) {
      return;
    }

    const auto id = gc_.getObjectID(ptr);
    if (!seenIDs_.insert(id).second) {
      // Already seen this node, don't add another edge.
      return;
    }
    auto nameRef = llvh::StringRef::withNullAsEmpty(name);
    if (!nameRef.empty()) {
      snap_.addNamedEdge(
          weak ? HeapSnapshot::EdgeType::Weak
               : HeapSnapshot::EdgeType::Internal,
          nameRef,
          id);
    } else if (weak) {
      // TODO: Figure out if we can just add these weak edges as indexed instead
      // of named
      std::string numericName = std::to_string(nextEdge_++);
      snap_.addNamedEdge(HeapSnapshot::EdgeType::Weak, numericName.c_str(), id);
    } else {
      // Unnamed edges get indices.
      snap_.addIndexedEdge(HeapSnapshot::EdgeType::Element, nextEdge_++, id);
    }
  }
};

} // namespace

void GCBase::createSnapshotImpl(
    GC &gc,
    HeapSnapshot &snap,
    SavedNumRootEdges &numRootEdges) {
  const auto rootScan = [&gc, &snap, &numRootEdges, this]() {
    {
      // Make the super root node and add edges to each root section.
      SnapshotRootSectionAcceptor rootSectionAcceptor(getPointerBase(), snap);
      // The super root has a single element pointing to the "(GC roots)"
      // synthetic node. v8 also has some "shortcut" edges to things like the
      // global object, but those don't seem necessary for correctness.
      snap.beginNode();
      snap.addIndexedEdge(
          HeapSnapshot::EdgeType::Element,
          1,
          IDTracker::reserved(IDTracker::ReservedObjectID::GCRoots));
      snap.endNode(
          HeapSnapshot::NodeType::Synthetic,
          "",
          IDTracker::reserved(IDTracker::ReservedObjectID::SuperRoot),
          0,
          0);
      snapshotAddGCNativeNodes(snap);
      snap.beginNode();
      markRoots(rootSectionAcceptor, true);
      markWeakRoots(rootSectionAcceptor, /*markLongLived*/ true);
      snapshotAddGCNativeEdges(snap);
      snap.endNode(
          HeapSnapshot::NodeType::Synthetic,
          "(GC roots)",
          static_cast<HeapSnapshot::NodeID>(
              IDTracker::reserved(IDTracker::ReservedObjectID::GCRoots)),
          0,
          0);
    }
    {
      // Make a node for each root section and add edges into the actual heap.
      // Within a root section, there might be duplicates. The root acceptor
      // filters out duplicate edges because there cannot be duplicate edges to
      // nodes reachable from the super root.
      SnapshotRootAcceptor rootAcceptor(gc, snap, numRootEdges);
      markRoots(rootAcceptor, true);
      markWeakRoots(rootAcceptor, /*markLongLived*/ true);
    }
    gcCallbacks_.visitIdentifiers([&snap, this](
                                      SymbolID sym,
                                      const StringPrimitive *str) {
      snap.beginNode();
      if (str) {
        snap.addNamedEdge(
            HeapSnapshot::EdgeType::Internal, "description", getObjectID(str));
      }
      snap.endNode(
          HeapSnapshot::NodeType::Symbol,
          convertSymbolToUTF8(sym),
          idTracker_.getObjectID(sym),
          sizeof(SymbolID),
          0);
    });
  };

  snap.beginSection(HeapSnapshot::Section::Nodes);
  rootScan();
  // Add all primitive values as nodes if they weren't added before.
  // This must be done as a step before adding any edges to these nodes.
  // In particular, custom edge adders might try to add edges to primitives that
  // haven't been recorded yet.
  // The acceptor is recording some state between objects, so define it outside
  // the loop.
  PrimitiveNodeAcceptor primitiveAcceptor(
      getPointerBase(), snap, getIDTracker());
  // Add a node for each object in the heap.
  const auto snapshotForObject =
      [&snap, &primitiveAcceptor, &gc, this](GCCell *cell) {
        auto &allocationLocationTracker = getAllocationLocationTracker();
        // First add primitive nodes.
        markCellWithNames(primitiveAcceptor, cell);
        EdgeAddingAcceptor acceptor(gc, snap);
        // Allow nodes to add extra nodes not in the JS heap.
        cell->getVT()->snapshotMetaData.addNodes(cell, gc, snap);
        snap.beginNode();
        // Add all internal edges first.
        markCellWithNames(acceptor, cell);
        // Allow nodes to add custom edges not represented by metadata.
        cell->getVT()->snapshotMetaData.addEdges(cell, gc, snap);
        auto stackTracesTreeNode =
            allocationLocationTracker.getStackTracesTreeNodeForAlloc(
                gc.getObjectID(cell));
        snap.endNode(
            cell->getVT()->snapshotMetaData.nodeType(),
            cell->getVT()->snapshotMetaData.nameForNode(cell, gc),
            gc.getObjectID(cell),
            cell->getAllocatedSizeSlow(),
            stackTracesTreeNode ? stackTracesTreeNode->id : 0);
      };
  gc.forAllObjs(snapshotForObject);
  // Scan all WeakMapEntrySlot so that PrimitiveNodeAcceptor won't miss
  // primitives stored as WeakMap values.
  weakMapEntrySlots_.forEach([&primitiveAcceptor](WeakMapEntrySlot &slot) {
    primitiveAcceptor.accept(slot.mappedValue);
  });
  // Write the singleton number nodes into the snapshot.
  primitiveAcceptor.writeAllNodes();
  snap.endSection(HeapSnapshot::Section::Nodes);

  snap.beginSection(HeapSnapshot::Section::Edges);
  rootScan();
  // No need to run the primitive scan again, as it only adds nodes, not edges.
  // Add edges between objects in the heap.
  gc.forAllObjs(snapshotForObject);
  snap.endSection(HeapSnapshot::Section::Edges);

  snap.emitAllocationTraceInfo();

  snap.beginSection(HeapSnapshot::Section::Samples);
  getAllocationLocationTracker().addSamplesToSnapshot(snap);
  snap.endSection(HeapSnapshot::Section::Samples);

  snap.beginSection(HeapSnapshot::Section::Locations);
  forAllObjs([&snap, &gc](GCCell *cell) {
    cell->getVT()->snapshotMetaData.addLocations(cell, gc, snap);
  });
  snap.endSection(HeapSnapshot::Section::Locations);
}

void GCBase::createSnapshot(
    GC &gc,
    llvh::raw_ostream &os,
    bool captureNumericValue) {
  if (!captureNumericValue) {
    idTracker_.stopTrackingNumberIDs();
  }
  // Chrome 125 requires correct node count and edge count in the "snapshot"
  // field, which is at the beginning of the heap snapshot. We do two passes to
  // populate the correct node/edge count. First, we create a dummy HeapSnapshot
  // instance with a no-op JSON emitter, and invoke createSnapshotImpl() with
  // it. From that instance we can get the node count and edge count, and use
  // them to create a HeapSnapShot instance in the second pass.
  JSONEmitter dummyJSON{llvh::nulls()};
  HeapSnapshot dummySnap{dummyJSON, 0, 0, 0, gcCallbacks_.getStackTracesTree()};
  // Array for saving the number of edges for each root section. We set the
  // value the first time we visit a root section, and make sure the same number
  // of edges are added in a single call of this function.
  SavedNumRootEdges numRootEdges;
  createSnapshotImpl(gc, dummySnap, numRootEdges);

  // Second pass, write out the real snapshot with the correct node_count and
  // edge_count.
  JSONEmitter json{os};
  HeapSnapshot snap{
      json,
      dummySnap.getNodeCount(),
      dummySnap.getEdgeCount(),
      dummySnap.getTraceFunctionCount(),
      gcCallbacks_.getStackTracesTree()};
  createSnapshotImpl(gc, snap, numRootEdges);
  // Check if the node/edge counts of the two passes are equal.
  assert(
      dummySnap.getNodeCount() == snap.getNodeCount() &&
      "Node count of two passes of createSnapshotImpl are not equal");
  assert(
      dummySnap.getEdgeCount() == snap.getEdgeCount() &&
      "Edge count of two passes of createSnapshotImpl are not equal");
  idTracker_.startTrackingNumberIDs();
}

void GCBase::snapshotAddGCNativeNodes(HeapSnapshot &snap) {
  snap.beginNode();
  snap.endNode(
      HeapSnapshot::NodeType::Native,
      "hermes::ManagedChunkedList<WeakRefSlot>",
      IDTracker::reserved(IDTracker::ReservedObjectID::WeakRefSlotStorage),
      weakSlots_.capacity() * sizeof(decltype(weakSlots_)::value_type),
      0);
}

void GCBase::snapshotAddGCNativeEdges(HeapSnapshot &snap) {
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Internal,
      "weakRefSlots",
      IDTracker::reserved(IDTracker::ReservedObjectID::WeakRefSlotStorage));
}

void GCBase::enableHeapProfiler(
    std::function<void(
        uint64_t,
        std::chrono::microseconds,
        std::vector<GCBase::AllocationLocationTracker::HeapStatsUpdate>)>
        fragmentCallback) {
  getAllocationLocationTracker().enable(std::move(fragmentCallback));
}

void GCBase::disableHeapProfiler() {
  getAllocationLocationTracker().disable();
}

void GCBase::enableSamplingHeapProfiler(size_t samplingInterval, int64_t seed) {
  getSamplingAllocationTracker().enable(samplingInterval, seed);
}

void GCBase::disableSamplingHeapProfiler(llvh::raw_ostream &os) {
  getSamplingAllocationTracker().disable(os);
}
#endif // HERMES_MEMORY_INSTRUMENTATION

void GCBase::checkTripwire(size_t dataSize) {
  if (LLVM_LIKELY(!tripwireCallback_) ||
      LLVM_LIKELY(dataSize < tripwireLimit_) || tripwireCalled_) {
    return;
  }

#ifdef HERMES_MEMORY_INSTRUMENTATION
  class Ctx : public GCTripwireContext {
   public:
    Ctx(GCBase *gc) : gc_(gc) {}

    std::error_code createSnapshotToFile(const std::string &path) override {
      std::error_code code;
      llvh::raw_fd_ostream os(path, code, llvh::sys::fs::FileAccess::FA_Write);
      if (code) {
        return code;
      }
      // We currently cannot call collect() to perform a full collection here
      // because this is typically called while gcMutex_ is already held. While
      // we could refactor it to work, it is unlikely to provide much value
      // since the tripwire is invoked shortly after a collection anyway.
      gc_->createSnapshot(os, true);
      return std::error_code{};
    }

    std::error_code createSnapshot(std::ostream &os, bool captureNumericValue)
        override {
      llvh::raw_os_ostream ros(os);
      gc_->createSnapshot(ros, captureNumericValue);
      return std::error_code{};
    }

   private:
    GCBase *gc_;
  } ctx(this);
#else // !defined(HERMES_MEMORY_INSTRUMENTATION)
  class Ctx : public GCTripwireContext {
   public:
    std::error_code createSnapshotToFile(const std::string &path) override {
      return std::error_code(ENOSYS, std::system_category());
    }

    std::error_code createSnapshot(std::ostream &os, bool captureNumericValue)
        override {
      return std::error_code(ENOSYS, std::system_category());
    }
  } ctx;
#endif // !defined(HERMES_MEMORY_INSTRUMENTATION)

  tripwireCalled_ = true;
  tripwireCallback_(ctx);
}

void GCBase::printAllCollectedStats(llvh::raw_ostream &os) {
  if (!recordGcStats_)
    return;

  dump(os);
  os << "GC stats:\n";
  JSONEmitter json{os, /*pretty*/ true};
  json.openDict();
  printStats(json);
  json.closeDict();
  os << "\n";
}

void GCBase::getHeapInfo(HeapInfo &info) {
  info.numCollections = cumStats_.numCollections;
  info.generalStats = cumStats_;
}

void GCBase::getHeapInfoWithMallocSize(HeapInfo &info) {
  // Assign to overwrite anything previously in the heap info.
  info.mallocSizeEstimate =
      weakSlots_.capacity() * sizeof(decltype(weakSlots_)::value_type);
}

#ifndef NDEBUG
void GCBase::getDebugHeapInfo(DebugHeapInfo &info) {
  recordNumAllocatedObjects();
  info.numAllocatedObjects = numAllocatedObjects_;
  info.numReachableObjects = numReachableObjects_;
  info.numCollectedObjects = numCollectedObjects_;
  info.numFinalizedObjects = numFinalizedObjects_;
  info.numMarkedSymbols = numMarkedSymbols_;
  info.numHiddenClasses = numHiddenClasses_;
  info.numLeafHiddenClasses = numLeafHiddenClasses_;
}

size_t GCBase::countUsedWeakRefs() const {
  return weakSlots_.sizeForTests();
}
#endif

#ifndef NDEBUG
void GCBase::DebugHeapInfo::assertInvariants() const {
  // The number of allocated objects at any time is at least the number
  // found reachable in the last collection.
  assert(numAllocatedObjects >= numReachableObjects);
  // The number of objects finalized in the last collection is at most the
  // number of objects collected.
  assert(numCollectedObjects >= numFinalizedObjects);
}
#endif

void GCBase::dump(llvh::raw_ostream &, bool) { /* nop */ }

void GCBase::printStats(JSONEmitter &json) {
  json.emitKeyValue("type", "hermes");
  json.emitKeyValue("version", 0);
  gcCallbacks_.printRuntimeGCStats(json);

  std::chrono::duration<double> elapsedTime =
      std::chrono::steady_clock::now() - execStartTime_;
  auto elapsedCPUSeconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          oscompat::thread_cpu_time())
          .count() -
      std::chrono::duration_cast<std::chrono::duration<double>>(
          execStartCPUTime_)
          .count();

  HeapInfo info;
  getHeapInfoWithMallocSize(info);
  getHeapInfo(info);
#ifndef NDEBUG
  DebugHeapInfo debugInfo;
  getDebugHeapInfo(debugInfo);
#endif

  json.emitKey("heapInfo");
  json.openDict();
#ifndef NDEBUG
  json.emitKeyValue("Num allocated cells", debugInfo.numAllocatedObjects);
  json.emitKeyValue("Num reachable cells", debugInfo.numReachableObjects);
  json.emitKeyValue("Num collected cells", debugInfo.numCollectedObjects);
  json.emitKeyValue("Num finalized cells", debugInfo.numFinalizedObjects);
  json.emitKeyValue("Num marked symbols", debugInfo.numMarkedSymbols);
  json.emitKeyValue("Num hidden classes", debugInfo.numHiddenClasses);
  json.emitKeyValue("Num leaf classes", debugInfo.numLeafHiddenClasses);
  json.emitKeyValue("Num weak references", ((GC *)this)->countUsedWeakRefs());
#endif
  json.emitKeyValue("Peak RSS", oscompat::peak_rss());
  json.emitKeyValue("Current RSS", oscompat::current_rss());
  json.emitKeyValue("Current Dirty", oscompat::current_private_dirty());
  json.emitKeyValue("Heap size", info.heapSize);
  json.emitKeyValue("Allocated bytes", info.allocatedBytes);
  json.emitKeyValue("Num collections", info.numCollections);
  json.emitKeyValue("External bytes", info.externalBytes);
  json.emitKeyValue("Malloc size", info.mallocSizeEstimate);
  json.closeDict();

  long vol = -1;
  long invol = -1;
  if (oscompat::num_context_switches(vol, invol)) {
    vol -= startNumVoluntaryContextSwitches_;
    invol -= startNumInvoluntaryContextSwitches_;
  }

  json.emitKey("general");
  json.openDict();
  json.emitKeyValue("numCollections", cumStats_.numCollections);
  json.emitKeyValue("totalTime", elapsedTime.count());
  json.emitKeyValue("totalCPUTime", elapsedCPUSeconds);
  json.emitKeyValue("totalGCTime", formatSecs(cumStats_.gcWallTime.sum()).secs);
  json.emitKeyValue("volCtxSwitch", vol);
  json.emitKeyValue("involCtxSwitch", invol);
  json.emitKeyValue(
      "avgGCPause", formatSecs(cumStats_.gcWallTime.average()).secs);
  json.emitKeyValue("maxGCPause", formatSecs(cumStats_.gcWallTime.max()).secs);
  json.emitKeyValue(
      "totalGCCPUTime", formatSecs(cumStats_.gcCPUTime.sum()).secs);
  json.emitKeyValue(
      "avgGCCPUPause", formatSecs(cumStats_.gcCPUTime.average()).secs);
  json.emitKeyValue(
      "maxGCCPUPause", formatSecs(cumStats_.gcCPUTime.max()).secs);
  json.emitKeyValue("finalHeapSize", formatSize(cumStats_.finalHeapSize).bytes);
  json.emitKeyValue(
      "peakAllocatedBytes", formatSize(getPeakAllocatedBytes()).bytes);
  json.emitKeyValue("peakLiveAfterGC", formatSize(getPeakLiveAfterGC()).bytes);
  json.emitKeyValue(
      "totalAllocatedBytes", formatSize(info.totalAllocatedBytes).bytes);
  json.closeDict();
}

void GCBase::recordGCStats(
    const InternalAnalyticsEvent &event,
    CumulativeHeapStats *stats,
    bool onMutator,
    bool fromNewCollection) {
  // Hades OG collections do not block the mutator, and so do not contribute to
  // the max pause time or the total execution time.
  if (onMutator)
    stats->gcWallTime.record(event.durationSecs);
  stats->gcCPUTime.record(event.cpuDurationSecs);
  stats->finalHeapSize = event.size.after;
  stats->usedBefore.record(event.allocated.before);
  stats->usedAfter.record(event.allocated.after);
  stats->numCollections += fromNewCollection ? 1 : 0;
}

void GCBase::recordGCStats(
    const InternalAnalyticsEvent &event,
    bool onMutator,
    bool fromNewCollection) {
  if (analyticsCallback_) {
    analyticsCallback_(event);
  }
  recordGCStats(event, &cumStats_, onMutator, fromNewCollection);
}

void GCBase::oom(std::error_code reason) {
  hasOOMed_ = true;
  char detailBuffer[400];
  oomDetail(detailBuffer, reason);
#ifdef HERMESVM_EXCEPTION_ON_OOM
  // No need to run finalizeAll, the exception will propagate and eventually run
  // ~Runtime.
  throw JSOutOfMemoryError(
      std::string(detailBuffer) + "\ncall stack:\n" +
      gcCallbacks_.getCallStackNoAlloc());
#else
  hermesLog("HermesGC", "OOM: %s.", detailBuffer);
  // Record the OOM custom data with the crash manager.
  crashMgr_->setCustomData("HermesGCOOMDetailBasic", detailBuffer);
  hermes_fatal("OOM", reason);
#endif
}

void GCBase::oomDetail(
    llvh::MutableArrayRef<char> detailBuffer,
    std::error_code reason) {
  HeapInfo heapInfo;
  getHeapInfo(heapInfo);
  snprintf(
      detailBuffer.data(),
      detailBuffer.size(),
      "[%.20s] reason = %.150s (%d from category: %.50s), numCollections = %u, heapSize = %" PRIu64
      ", allocated = %" PRIu64 ", va = %" PRIu64 ", external = %" PRIu64,
      name_.c_str(),
      reason.message().c_str(),
      reason.value(),
      reason.category().name(),
      heapInfo.numCollections,
      heapInfo.heapSize,
      heapInfo.allocatedBytes,
      heapInfo.va,
      heapInfo.externalBytes);
}

#if HERMESVM_SANITIZE_HANDLES != 0
bool GCBase::shouldSanitizeHandles() {
  static std::uniform_real_distribution<> dist(0.0, 1.0);
  return dist(randomEngine_) < sanitizeRate_;
}
#endif

WeakRefSlot *GCBase::allocWeakSlot(SmallHermesValue target) {
  WeakRefSlot &slot = weakSlots_.add(target);
  // Record the slot if it points into the YG (see weakSlots_).
  if (inYoungGen(target))
    weakSlots_.record(slot);
  return &slot;
}

WeakMapEntrySlot *GCBase::allocWeakMapEntrySlot(
    SmallHermesValue key,
    HermesValue value,
    JSWeakMapImplBase *owner) {
  WeakMapEntrySlot &slot = weakMapEntrySlots_.add(
      key, value, CompressedPointer::encode(owner, getPointerBase()));
  // Record the slot if its key, mapped value or owner is in the YG (see
  // weakMapEntrySlots_).
  if (inYoungGen(key) || inYoungGen(value) || inYoungGen(owner))
    weakMapEntrySlots_.record(slot);
  return &slot;
}

HeapSnapshot::NodeID GCBase::getObjectID(const GCCell *cell) {
  assert(cell && "Called getObjectID on a null pointer");
  return getObjectID(
      CompressedPointer::encodeNonNull(
          const_cast<GCCell *>(cell), pointerBase_));
}

HeapSnapshot::NodeID GCBase::getObjectIDMustExist(const GCCell *cell) {
  assert(cell && "Called getObjectID on a null pointer");
  return idTracker_.getObjectIDMustExist(
      CompressedPointer::encodeNonNull(
          const_cast<GCCell *>(cell), pointerBase_));
}

HeapSnapshot::NodeID GCBase::getObjectID(CompressedPointer cell) {
  assert(cell && "Called getObjectID on a null pointer");
  return idTracker_.getObjectID(cell);
}

HeapSnapshot::NodeID GCBase::getObjectID(SymbolID sym) {
  return idTracker_.getObjectID(sym);
}

HeapSnapshot::NodeID GCBase::getNativeID(const void *mem) {
  assert(mem && "Called getNativeID on a null pointer");
  return idTracker_.getNativeID(mem);
}

bool GCBase::hasObjectID(const GCCell *cell) {
  assert(cell && "Called hasObjectID on a null pointer");
  return idTracker_.hasObjectID(
      CompressedPointer::encodeNonNull(
          const_cast<GCCell *>(cell), pointerBase_));
}

void GCBase::newAlloc(const GCCell *ptr, uint32_t sz) {
#ifdef HERMES_MEMORY_INSTRUMENTATION
  allocationLocationTracker_.newAlloc(ptr, sz);
  samplingAllocationTracker_.newAlloc(ptr, sz);
#endif
}

void GCBase::moveObject(
    const GCCell *oldPtr,
    uint32_t oldSize,
    const GCCell *newPtr,
    uint32_t newSize) {
  idTracker_.moveObject(
      CompressedPointer::encodeNonNull(
          const_cast<GCCell *>(oldPtr), pointerBase_),
      CompressedPointer::encodeNonNull(
          const_cast<GCCell *>(newPtr), pointerBase_));
#ifdef HERMES_MEMORY_INSTRUMENTATION
  // Use newPtr here because the idTracker_ just moved it.
  allocationLocationTracker_.updateSize(newPtr, oldSize, newSize);
  samplingAllocationTracker_.updateSize(newPtr, oldSize, newSize);
#endif
}

void GCBase::untrackObject(const GCCell *cell, uint32_t sz) {
  assert(cell && "Called untrackObject on a null pointer");
  // The allocation tracker needs to use the ID, so this needs to come
  // before untrackObject.
#ifdef HERMES_MEMORY_INSTRUMENTATION
  getAllocationLocationTracker().freeAlloc(cell, sz);
  getSamplingAllocationTracker().freeAlloc(cell, sz);
#endif
  idTracker_.untrackObject(
      CompressedPointer::encodeNonNull(
          const_cast<GCCell *>(cell), pointerBase_));
}

#ifndef NDEBUG
uint64_t GCBase::nextObjectID() {
  return debugAllocationCounter_++;
}
#endif

const GCExecTrace &GCBase::getGCExecTrace() const {
  return execTrace_;
}

llvh::raw_ostream &operator<<(
    llvh::raw_ostream &os,
    const DurationFormatObj &dfo) {
  if (dfo.secs >= 1.0) {
    os << format("%5.3f", dfo.secs) << " s";
  } else if (dfo.secs >= 0.001) {
    os << format("%5.3f", dfo.secs * 1000.0) << " ms";
  } else {
    os << format("%5.3f", dfo.secs * 1000000.0) << " us";
  }
  return os;
}

llvh::raw_ostream &operator<<(llvh::raw_ostream &os, const SizeFormatObj &sfo) {
  double dblsize = static_cast<double>(sfo.bytes);
  if (sfo.bytes >= (1024 * 1024 * 1024)) {
    double gbs = dblsize / (1024.0 * 1024.0 * 1024.0);
    os << format("%0.3f GiB", gbs);
  } else if (sfo.bytes >= (1024 * 1024)) {
    double mbs = dblsize / (1024.0 * 1024.0);
    os << format("%0.3f MiB", mbs);
  } else if (sfo.bytes >= 1024) {
    double kbs = dblsize / 1024.0;
    os << format("%0.3f KiB", kbs);
  } else {
    os << sfo.bytes << " B";
  }
  return os;
}

GCBase::GCCallbacks::~GCCallbacks() {}

GCBase::IDTracker::IDTracker() {
  assert(lastID_ % 2 == 1 && "First JS object ID isn't odd");
}

void GCBase::IDTracker::moveObject(
    CompressedPointer oldLocation,
    CompressedPointer newLocation) {
  if (oldLocation == newLocation) {
    // Don't need to do anything if the object isn't moving anywhere. This can
    // happen in old generations where it is compacted to the same location.
    return;
  }
  std::lock_guard<Mutex> lk{mtx_};
  auto old = objectIDMap_.find(oldLocation.getRaw());
  if (old == objectIDMap_.end()) {
    // Avoid making new keys for objects that don't need to be tracked.
    return;
  }
  const auto oldID = old->second;
  assert(
      objectIDMap_.count(newLocation.getRaw()) == 0 &&
      "Moving to a location that is already tracked");
  // Have to erase first, because any other access can invalidate the iterator.
  objectIDMap_.erase(old);
  objectIDMap_[newLocation.getRaw()] = oldID;
  // Update the reverse map entry if it exists.
  auto reverseMappingIt = idObjectMap_.find(oldID);
  if (reverseMappingIt != idObjectMap_.end()) {
    assert(
        reverseMappingIt->second == oldLocation.getRaw() &&
        "The reverse mapping should have the old address");
    reverseMappingIt->second = newLocation.getRaw();
  }
}

llvh::SmallVector<HeapSnapshot::NodeID, 1> &
GCBase::IDTracker::getExtraNativeIDs(HeapSnapshot::NodeID node) {
  std::lock_guard<Mutex> lk{mtx_};
  // The operator[] will default construct the vector to be empty if it doesn't
  // exist.
  return extraNativeIDs_[node];
}

HeapSnapshot::NodeID GCBase::IDTracker::getNumberID(double num) {
  std::lock_guard<Mutex> lk{mtx_};
  if (isTrackingNumberIDs_) {
    auto &numberRef = numberIDMap_[num];
    // If the entry didn't exist, the value was initialized to 0.
    if (numberRef != 0) {
      return numberRef;
    }
    // Else, it is a number that hasn't been seen before.
    return numberRef = nextNumberID();
  } else {
    return GCBase::IDTracker::reserved(
        GCBase::IDTracker::ReservedObjectID::Number);
  }
}

llvh::Optional<CompressedPointer> GCBase::IDTracker::getObjectForID(
    HeapSnapshot::NodeID id) {
  std::lock_guard<Mutex> lk{mtx_};
  auto it = idObjectMap_.find(id);
  if (it != idObjectMap_.end()) {
    return CompressedPointer::fromRaw(it->second);
  }
  // Do an O(N) search through the map, then cache the result.
  // This trades time for memory, since this is a rare operation.
  for (const auto &p : objectIDMap_) {
    if (p.second == id) {
      // Cache the result so repeated lookups are fast.
      // This cache is unlikely to grow that large, unless someone hovers over
      // every single object in a snapshot in Chrome.
      auto itAndDidInsert = idObjectMap_.try_emplace(p.second, p.first);
      assert(itAndDidInsert.second);
      return CompressedPointer::fromRaw(itAndDidInsert.first->second);
    }
  }
  // ID not found in the map, wasn't an object to begin with.
  return llvh::None;
}

bool GCBase::IDTracker::hasNativeIDs() {
  std::lock_guard<Mutex> lk{mtx_};
  return !nativeIDMap_.empty();
}

bool GCBase::IDTracker::hasTrackedObjectIDs() {
  std::lock_guard<Mutex> lk{mtx_};
  return !objectIDMap_.empty();
}

bool GCBase::IDTracker::isTrackingNumberIDs() {
  std::lock_guard<Mutex> lk{mtx_};
  return isTrackingNumberIDs_;
}

void GCBase::IDTracker::startTrackingNumberIDs() {
  std::lock_guard<Mutex> lk{mtx_};
  isTrackingNumberIDs_ = true;
}

void GCBase::IDTracker::stopTrackingNumberIDs() {
  std::lock_guard<Mutex> lk{mtx_};
  isTrackingNumberIDs_ = false;
}

HeapSnapshot::NodeID GCBase::IDTracker::getObjectID(CompressedPointer cell) {
  std::lock_guard<Mutex> lk{mtx_};
  auto iter = objectIDMap_.find(cell.getRaw());
  if (iter != objectIDMap_.end()) {
    return iter->second;
  }
  // Else, assume it is an object that needs to be tracked and give it a new ID.
  const auto objID = nextObjectID();
  objectIDMap_[cell.getRaw()] = objID;
  return objID;
}

bool GCBase::IDTracker::hasObjectID(CompressedPointer cell) {
  std::lock_guard<Mutex> lk{mtx_};
  return objectIDMap_.count(cell.getRaw());
}

HeapSnapshot::NodeID GCBase::IDTracker::getObjectIDMustExist(
    CompressedPointer cell) {
  std::lock_guard<Mutex> lk{mtx_};
  auto iter = objectIDMap_.find(cell.getRaw());
  assert(iter != objectIDMap_.end() && "cell must already have an ID");
  return iter->second;
}

HeapSnapshot::NodeID GCBase::IDTracker::getObjectID(SymbolID sym) {
  std::lock_guard<Mutex> lk{mtx_};
  auto iter = symbolIDMap_.find(sym.unsafeGetIndex());
  if (iter != symbolIDMap_.end()) {
    return iter->second;
  }
  // Else, assume it is a symbol that needs to be tracked and give it a new ID.
  const auto symID = nextObjectID();
  symbolIDMap_[sym.unsafeGetIndex()] = symID;
  return symID;
}

HeapSnapshot::NodeID GCBase::IDTracker::getNativeID(const void *mem) {
  std::lock_guard<Mutex> lk{mtx_};
  auto iter = nativeIDMap_.find(mem);
  if (iter != nativeIDMap_.end()) {
    return iter->second;
  }
  // Else, assume it is a piece of native memory that needs to be tracked and
  // give it a new ID.
  const auto objID = nextNativeID();
  nativeIDMap_[mem] = objID;
  return objID;
}

void GCBase::IDTracker::untrackObject(CompressedPointer cell) {
  std::lock_guard<Mutex> lk{mtx_};
  // It's ok if this didn't exist before, since erase will remove it anyway, and
  // the default constructed zero ID won't be present in extraNativeIDs_.
  const auto id = objectIDMap_[cell.getRaw()];
  objectIDMap_.erase(cell.getRaw());
  extraNativeIDs_.erase(id);
  // Erase the reverse mapping entry if it exists.
  idObjectMap_.erase(id);
}

void GCBase::IDTracker::untrackNative(const void *mem) {
  std::lock_guard<Mutex> lk{mtx_};
  nativeIDMap_.erase(mem);
}

void GCBase::IDTracker::untrackSymbol(uint32_t symIdx) {
  std::lock_guard<Mutex> lk{mtx_};
  symbolIDMap_.erase(symIdx);
}

HeapSnapshot::NodeID GCBase::IDTracker::lastID() const {
  return lastID_;
}

HeapSnapshot::NodeID GCBase::IDTracker::nextObjectID() {
  // This must be unique for most features that rely on it, check for overflow.
  if (LLVM_UNLIKELY(
          lastID_ >=
          std::numeric_limits<HeapSnapshot::NodeID>::max() - kIDStep)) {
    hermes_fatal("Ran out of object IDs");
  }
  return lastID_ += kIDStep;
}

HeapSnapshot::NodeID GCBase::IDTracker::nextNativeID() {
  // Calling nextObjectID effectively allocates two new IDs, one even
  // and one odd, returning the latter. For native objects, we want the former.
  HeapSnapshot::NodeID id = nextObjectID();
  assert(id > 0 && "nextObjectID should check for overflow");
  return id - 1;
}

HeapSnapshot::NodeID GCBase::IDTracker::nextNumberID() {
  // Numbers will all be considered JS memory, not native memory.
  return nextObjectID();
}

#ifdef HERMES_MEMORY_INSTRUMENTATION

GCBase::AllocationLocationTracker::AllocationLocationTracker(GCBase *gc)
    : gc_(gc) {}

bool GCBase::AllocationLocationTracker::isEnabled() const {
  return enabled_;
}

StackTracesTreeNode *
GCBase::AllocationLocationTracker::getStackTracesTreeNodeForAlloc(
    HeapSnapshot::NodeID id) const {
  auto mapIt = stackMap_.find(id);
  return mapIt == stackMap_.end() ? nullptr : mapIt->second;
}

void GCBase::AllocationLocationTracker::enable(
    std::function<
        void(uint64_t, std::chrono::microseconds, std::vector<HeapStatsUpdate>)>
        callback) {
  assert(!enabled_ && "Shouldn't enable twice");
  enabled_ = true;
  std::lock_guard<Mutex> lk{mtx_};
  // For correct visualization of the allocation timeline, it's necessary that
  // objects in the heap snapshot that existed before sampling was enabled have
  // numerically lower IDs than those allocated during sampling. We ensure this
  // by assigning IDs to everything here.
  uint64_t numObjects = 0;
  uint64_t numBytes = 0;
  gc_->forAllObjs([&numObjects, &numBytes, this](GCCell *cell) {
    numObjects++;
    numBytes += cell->getAllocatedSizeSlow();
    gc_->getObjectID(cell);
  });
  fragmentCallback_ = std::move(callback);
  startTime_ = std::chrono::steady_clock::now();
  fragments_.clear();
  // The first fragment has all objects that were live before the profiler was
  // enabled.
  // The ID and timestamp will be filled out via flushCallback.
  fragments_.emplace_back(
      Fragment{
          IDTracker::kInvalidNode,
          std::chrono::microseconds(),
          numObjects,
          numBytes,
          // Say the fragment is touched here so it is written out
          // automatically by flushCallback.
          true});
  // Immediately flush the first fragment.
  flushCallback();
}

void GCBase::AllocationLocationTracker::disable() {
  std::lock_guard<Mutex> lk{mtx_};
  flushCallback();
  enabled_ = false;
  fragmentCallback_ = nullptr;
}

void GCBase::AllocationLocationTracker::newAlloc(
    const GCCell *ptr,
    uint32_t sz) {
  // Note we always get the current IP even if allocation tracking is not
  // enabled as it allows us to assert this feature works across many tests.
  // Note it's not very slow, it's slower than the non-virtual version
  // in Runtime though.
  const auto *ip = gc_->gcCallbacks_.getCurrentIPSlow();
  if (!enabled_) {
    return;
  }
  std::lock_guard<Mutex> lk{mtx_};
  // This is stateful and causes the object to have an ID assigned.
  const auto id = gc_->getObjectID(ptr);
  HERMES_SLOW_ASSERT(
      &findFragmentForID(id) == &fragments_.back() &&
      "Should only ever be allocating into the newest fragment");
  Fragment &lastFrag = fragments_.back();
  assert(
      lastFrag.lastSeenObjectID_ == IDTracker::kInvalidNode &&
      "Last fragment should not have an ID assigned yet");
  lastFrag.numObjects_++;
  lastFrag.numBytes_ += sz;
  lastFrag.touchedSinceLastFlush_ = true;
  if (lastFrag.numBytes_ >= kFlushThreshold) {
    flushCallback();
  }
  if (auto node = gc_->gcCallbacks_.getCurrentStackTracesTreeNode(ip)) {
    auto itAndDidInsert = stackMap_.try_emplace(id, node);
    assert(itAndDidInsert.second && "Failed to create a new node");
    (void)itAndDidInsert;
  }
}

void GCBase::AllocationLocationTracker::updateSize(
    const GCCell *ptr,
    uint32_t oldSize,
    uint32_t newSize) {
  int32_t delta = static_cast<int32_t>(newSize) - static_cast<int32_t>(oldSize);
  if (!delta || !enabled_) {
    // Nothing to update.
    return;
  }
  std::lock_guard<Mutex> lk{mtx_};
  const auto id = gc_->getObjectIDMustExist(ptr);
  Fragment &frag = findFragmentForID(id);
  frag.numBytes_ += delta;
  frag.touchedSinceLastFlush_ = true;
}

void GCBase::AllocationLocationTracker::freeAlloc(
    const GCCell *ptr,
    uint32_t sz) {
  if (!enabled_) {
    // Fragments won't exist if the heap profiler isn't enabled.
    return;
  }
  // Hold a lock during freeAlloc because concurrent Hades might be creating an
  // alloc (newAlloc) at the same time.
  std::lock_guard<Mutex> lk{mtx_};
  // The ID must exist here since the memory profiler guarantees everything has
  // an ID (it does a heap pass at the beginning to assign them all).
  const auto id = gc_->getObjectIDMustExist(ptr);
  stackMap_.erase(id);
  Fragment &frag = findFragmentForID(id);
  assert(
      frag.numObjects_ >= 1 && "Num objects decremented too much for fragment");
  frag.numObjects_--;
  assert(frag.numBytes_ >= sz && "Num bytes decremented too much for fragment");
  frag.numBytes_ -= sz;
  frag.touchedSinceLastFlush_ = true;
}

GCBase::AllocationLocationTracker::Fragment &
GCBase::AllocationLocationTracker::findFragmentForID(HeapSnapshot::NodeID id) {
  assert(fragments_.size() >= 1 && "Must have at least one fragment available");
  for (auto it = fragments_.begin(); it != fragments_.end() - 1; ++it) {
    if (it->lastSeenObjectID_ >= id) {
      return *it;
    }
  }
  // Since no previous fragments matched, it must be the last fragment.
  return fragments_.back();
}

void GCBase::AllocationLocationTracker::flushCallback() {
  Fragment &lastFrag = fragments_.back();
  const auto lastID = gc_->getIDTracker().lastID();
  const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - startTime_);
  assert(
      lastFrag.lastSeenObjectID_ == IDTracker::kInvalidNode &&
      "Last fragment should not have an ID assigned yet");
  // In case a flush happens without any allocations occurring, don't add a new
  // fragment.
  if (lastFrag.touchedSinceLastFlush_) {
    lastFrag.lastSeenObjectID_ = lastID;
    lastFrag.timestamp_ = duration;
    // Place an empty fragment at the end, for any new allocs.
    fragments_.emplace_back(
        Fragment{
            IDTracker::kInvalidNode, std::chrono::microseconds(), 0, 0, false});
  }
  if (fragmentCallback_) {
    std::vector<HeapStatsUpdate> updatedFragments;
    // Don't include the last fragment, which is newly created (or has no
    // objects in it).
    for (size_t i = 0; i < fragments_.size() - 1; ++i) {
      auto &fragment = fragments_[i];
      if (fragment.touchedSinceLastFlush_) {
        updatedFragments.emplace_back(
            i, fragment.numObjects_, fragment.numBytes_);
        fragment.touchedSinceLastFlush_ = false;
      }
    }
    fragmentCallback_(lastID, duration, std::move(updatedFragments));
  }
}

void GCBase::AllocationLocationTracker::addSamplesToSnapshot(
    HeapSnapshot &snap) {
  std::lock_guard<Mutex> lk{mtx_};
  if (enabled_) {
    flushCallback();
  }
  // There might not be fragments if tracking has never been enabled. If there
  // are, the last one is always invalid.
  assert(
      (fragments_.empty() ||
       fragments_.back().lastSeenObjectID_ == IDTracker::kInvalidNode) &&
      "Last fragment should not have an ID assigned yet");
  // Loop over the fragments if we have any, and always skip the last one.
  for (size_t i = 0, e = fragments_.size(); i + 1 < e; ++i) {
    const auto &fragment = fragments_[i];
    snap.addSample(fragment.timestamp_, fragment.lastSeenObjectID_);
  }
}

void GCBase::SamplingAllocationLocationTracker::enable(
    size_t samplingInterval,
    int64_t seed) {
  if (seed < 0) {
    seed = std::random_device()();
  }
  randomEngine_.seed(seed);
  dist_ = llvh::make_unique<std::poisson_distribution<>>(samplingInterval);
  limit_ = nextSample();
}

void GCBase::SamplingAllocationLocationTracker::disable(llvh::raw_ostream &os) {
  JSONEmitter json{os};
  ChromeSamplingMemoryProfile profile{json};
  std::lock_guard<Mutex> lk{mtx_};
  // Track a map of size -> count for each stack tree node.
  llvh::DenseMap<StackTracesTreeNode *, llvh::DenseMap<size_t, size_t>>
      sizesToCounts;
  // Do a pre-pass to compute sizesToCounts.
  for (const auto &s : samples_) {
    const Sample &sample = s.second;
    sizesToCounts[sample.node][sample.size]++;
  }

  // Have to emit the tree of stack frames before emitting samples, Chrome
  // requires the tree emitted first.
  profile.emitTree(gc_->gcCallbacks_.getStackTracesTree(), sizesToCounts);
  profile.beginSamples();
  for (const auto &s : samples_) {
    const Sample &sample = s.second;
    profile.emitSample(sample.size, sample.node, sample.id);
  }
  profile.endSamples();
  dist_.reset();
  samples_.clear();
  limit_ = 0;
}

void GCBase::SamplingAllocationLocationTracker::newAlloc(
    const GCCell *ptr,
    uint32_t sz) {
  // If the sampling profiler isn't enabled, don't check anything else.
  if (!isEnabled()) {
    return;
  }
  if (sz <= limit_) {
    // Exit if it's not time for a sample yet.
    limit_ -= sz;
    return;
  }
  const auto *ip = gc_->gcCallbacks_.getCurrentIPSlow();
  // This is stateful and causes the object to have an ID assigned.
  const auto id = gc_->getObjectID(ptr);
  if (StackTracesTreeNode *node =
          gc_->gcCallbacks_.getCurrentStackTracesTreeNode(ip)) {
    // Hold a lock while modifying samples_.
    std::lock_guard<Mutex> lk{mtx_};
    auto sampleItAndDidInsert =
        samples_.try_emplace(id, Sample{sz, node, nextSampleID_++});
    assert(sampleItAndDidInsert.second && "Failed to create a sample");
    (void)sampleItAndDidInsert;
  }
  // Reset the limit.
  limit_ = nextSample();
}

void GCBase::SamplingAllocationLocationTracker::freeAlloc(
    const GCCell *ptr,
    uint32_t sz) {
  // If the sampling profiler isn't enabled, don't check anything else.
  if (!isEnabled()) {
    return;
  }
  if (!gc_->hasObjectID(ptr)) {
    // This object's lifetime isn't being tracked.
    return;
  }
  const auto id = gc_->getObjectIDMustExist(ptr);
  // Hold a lock while modifying samples_.
  std::lock_guard<Mutex> lk{mtx_};
  samples_.erase(id);
}

void GCBase::SamplingAllocationLocationTracker::updateSize(
    const GCCell *ptr,
    uint32_t oldSize,
    uint32_t newSize) {
  int32_t delta = static_cast<int32_t>(newSize) - static_cast<int32_t>(oldSize);
  if (!delta || !isEnabled() || !gc_->hasObjectID(ptr)) {
    // Nothing to update.
    return;
  }
  const auto id = gc_->getObjectIDMustExist(ptr);
  // Hold a lock while modifying samples_.
  std::lock_guard<Mutex> lk{mtx_};
  const auto it = samples_.find(id);
  if (it == samples_.end()) {
    return;
  }
  Sample &sample = it->second;
  // Update the size stored in the sample.
  sample.size = newSize;
}

size_t GCBase::SamplingAllocationLocationTracker::nextSample() {
  return (*dist_)(randomEngine_);
}
#endif // HERMES_MEMORY_INSTRUMENTATION

llvh::Optional<HeapSnapshot::NodeID> GCBase::getSnapshotID(HermesValue val) {
  if (val.isPointer() && val.getPointer()) {
    // Make nullptr HermesValue look like a JS null.
    // This should be rare, but is occasionally used by some parts of the VM.
    return val.getPointer()
        ? getObjectID(static_cast<GCCell *>(val.getPointer()))
        : IDTracker::reserved(IDTracker::ReservedObjectID::Null);
  } else if (val.isNumber()) {
    return idTracker_.getNumberID(val.getNumber());
  } else if (val.isSymbol() && val.getSymbol().isValid()) {
    return idTracker_.getObjectID(val.getSymbol());
  } else if (val.isUndefined()) {
    return IDTracker::reserved(IDTracker::ReservedObjectID::Undefined);
  } else if (val.isNull()) {
    return static_cast<HeapSnapshot::NodeID>(
        IDTracker::reserved(IDTracker::ReservedObjectID::Null));
  } else if (val.isBool()) {
    return static_cast<HeapSnapshot::NodeID>(
        val.getBool()
            ? IDTracker::reserved(IDTracker::ReservedObjectID::True)
            : IDTracker::reserved(IDTracker::ReservedObjectID::False));
  } else {
    return llvh::None;
  }
}

void *GCBase::getObjectForID(HeapSnapshot::NodeID id) {
  if (llvh::Optional<CompressedPointer> ptr = idTracker_.getObjectForID(id)) {
    return ptr->get(pointerBase_);
  }
  return nullptr;
}

void GCBase::sizeDiagnosticCensus(size_t allocatedBytes) {
  struct DiagnosticStat {
    uint64_t count{0};
    uint64_t size{0};
    std::map<std::string, DiagnosticStat> breakdown;

    static constexpr double getPercent(double numer, double denom) {
      return denom != 0 ? 100 * numer / denom : 0.0;
    }
    void printBreakdown(size_t depth) const {
      if (breakdown.empty())
        return;

      static const char *fmtBase =
          "%-25s : %'10" PRIu64 " [%'10" PRIu64 " B | %4.1f%%]";
      const std::string fmtStr = std::string(depth, '\t') + fmtBase;
      size_t totalSize = 0;
      size_t totalCount = 0;
      for (const auto &stat : breakdown) {
        hermesLog(
            "HermesGC",
            fmtStr.c_str(),
            stat.first.c_str(),
            stat.second.count,
            stat.second.size,
            getPercent(stat.second.size, size));
        stat.second.printBreakdown(depth + 1);
        totalSize += stat.second.size;
        totalCount += stat.second.count;
      }
      if (size_t other = size - totalSize)
        hermesLog(
            "HermesGC",
            fmtStr.c_str(),
            "Other",
            count - totalCount,
            other,
            getPercent(other, size));
    }
  };

  struct HeapSizeDiagnostic {
    uint64_t numCell = 0;
    uint64_t numVariableSizedObject = 0;
    DiagnosticStat stats;

    void rootsDiagnosticFrame() const {
      // Use this to print commas on large numbers
      char *currentLocale = std::setlocale(LC_NUMERIC, nullptr);
      std::setlocale(LC_NUMERIC, "");
      hermesLog("HermesGC", "Root size: %'7" PRIu64 " B", stats.size);
      stats.printBreakdown(1);
      std::setlocale(LC_NUMERIC, currentLocale);
    }

    void sizeDiagnosticFrame() const {
      // Use this to print commas on large numbers
      char *currentLocale = std::setlocale(LC_NUMERIC, nullptr);
      std::setlocale(LC_NUMERIC, "");

      hermesLog("HermesGC", "Heap size: %'7" PRIu64 " B", stats.size);
      hermesLog("HermesGC", "\tTotal cells: %'7" PRIu64, numCell);
      hermesLog(
          "HermesGC",
          "\tNum variable size cells: %'7" PRIu64,
          numVariableSizedObject);

      stats.printBreakdown(1);

      std::setlocale(LC_NUMERIC, currentLocale);
    }
  };

  struct HeapSizeDiagnosticAcceptor final : public RootAcceptor {
    // Can't be static in a local class.
    const int64_t HINT8_MIN = -(1 << 7);
    const int64_t HINT8_MAX = (1 << 7) - 1;
    const int64_t HINT16_MIN = -(1 << 15);
    const int64_t HINT16_MAX = (1 << 15) - 1;
    const int64_t HINT24_MIN = -(1 << 23);
    const int64_t HINT24_MAX = (1 << 23) - 1;
    const int64_t HINT32_MIN = -(1LL << 31);
    const int64_t HINT32_MAX = (1LL << 31) - 1;

    HeapSizeDiagnostic diagnostic;
    PointerBase &pointerBase_;

    HeapSizeDiagnosticAcceptor(PointerBase &pb) : pointerBase_{pb} {}

    void accept(GCCell *&ptr) override {
      diagnostic.stats.breakdown["Pointer"].count++;
      diagnostic.stats.breakdown["Pointer"].size += sizeof(GCCell *);
    }

    void accept(GCPointerBase &ptr) {
      diagnostic.stats.breakdown["GCPointer"].count++;
      diagnostic.stats.breakdown["GCPointer"].size += sizeof(GCPointerBase);
    }

    void accept(PinnedHermesValue &hv) override {
      acceptNullable(hv);
    }
    void acceptNullable(PinnedHermesValue &hv) override {
      acceptHV(
          hv,
          diagnostic.stats.breakdown["HermesValue"],
          sizeof(PinnedHermesValue));
    }
    void accept(PinnedSmallHermesValue &shv) override {
      acceptHV(
          shv.toHV(pointerBase_),
          diagnostic.stats.breakdown["SmallHermesValue"],
          sizeof(PinnedSmallHermesValue));
    }
    void accept(GCHermesValueBase &hv) {
      acceptHV(
          hv, diagnostic.stats.breakdown["HermesValue"], sizeof(GCHermesValue));
    }
    void accept(GCSmallHermesValueBase &shv) {
      acceptHV(
          shv.toHV(pointerBase_),
          diagnostic.stats.breakdown["SmallHermesValue"],
          sizeof(GCSmallHermesValue));
    }
    void acceptHV(
        const HermesValue &hv,
        DiagnosticStat &diag,
        const size_t hvBytes) {
      diag.count++;
      diag.size += hvBytes;
      llvh::StringRef hvType;
      if (hv.isBool()) {
        hvType = "Bool";
      } else if (hv.isNumber()) {
        hvType = "Number";
        double val = hv.getNumber();
        double intpart;
        llvh::StringRef numType = "Doubles";
        if (std::modf(val, &intpart) == 0.0) {
          if (val >= static_cast<double>(HINT8_MIN) &&
              val <= static_cast<double>(HINT8_MAX)) {
            numType = "Int8";
          } else if (
              val >= static_cast<double>(HINT16_MIN) &&
              val <= static_cast<double>(HINT16_MAX)) {
            numType = "Int16";
          } else if (
              val >= static_cast<double>(HINT24_MIN) &&
              val <= static_cast<double>(HINT24_MAX)) {
            numType = "Int24";
          } else if (
              val >= static_cast<double>(HINT32_MIN) &&
              val <= static_cast<double>(HINT32_MAX)) {
            numType = "Int32";
          }
        }
        diag.breakdown["Number"].breakdown[numType].count++;
        diag.breakdown["Number"].breakdown[numType].size += hvBytes;
      } else if (hv.isString()) {
        hvType = "StringPointer";
      } else if (hv.isSymbol()) {
        hvType = "Symbol";
      } else if (hv.isObject()) {
        hvType = "ObjectPointer";
      } else if (hv.isNull()) {
        hvType = "Null";
      } else if (hv.isUndefined()) {
        hvType = "Undefined";
      } else if (hv.isEmpty()) {
        hvType = "Empty";
      } else {
        assert(false && "Should be no other hermes values");
      }
      diag.breakdown[hvType].count++;
      diag.breakdown[hvType].size += hvBytes;
    }

    void accept(const RootSymbolID &sym) override {
      acceptSym(sym);
    }
    void accept(const GCSymbolID &sym) {
      acceptSym(sym);
    }
    void acceptSym(SymbolID sym) {
      diagnostic.stats.breakdown["Symbol"].count++;
      diagnostic.stats.breakdown["Symbol"].size += sizeof(SymbolID);
    }
  };

  hermesLog("HermesGC", "%s:", "Roots");
  HeapSizeDiagnosticAcceptor rootAcceptor{getPointerBase()};
  DroppingAcceptor<HeapSizeDiagnosticAcceptor> namedRootAcceptor{rootAcceptor};
  markRoots(namedRootAcceptor, /* markLongLived */ true);
  // For roots, compute the overall size and counts from the breakdown.
  for (const auto &substat : rootAcceptor.diagnostic.stats.breakdown) {
    rootAcceptor.diagnostic.stats.count += substat.second.count;
    rootAcceptor.diagnostic.stats.size += substat.second.size;
  }
  rootAcceptor.diagnostic.rootsDiagnosticFrame();

  hermesLog("HermesGC", "%s:", "Heap contents");
  HeapSizeDiagnosticAcceptor acceptor{getPointerBase()};
  forAllObjs([&acceptor, this](GCCell *cell) {
    markCell(acceptor, cell);
    acceptor.diagnostic.numCell++;
    if (cell->isVariableSize()) {
      acceptor.diagnostic.numVariableSizedObject++;
      // In theory should use sizeof(VariableSizeRuntimeCell), but that includes
      // padding sometimes. To be conservative, use the field it contains
      // directly instead.
      acceptor.diagnostic.stats.breakdown["Cell headers"].size +=
          (sizeof(GCCell) + sizeof(uint32_t));
    } else {
      acceptor.diagnostic.stats.breakdown["Cell headers"].size +=
          sizeof(GCCell);
    }

    // We include ExternalStringPrimitives because we're including external
    // memory in the overall heap size. We do not include
    // BufferedStringPrimitives because they just store a pointer to an
    // ExternalStringPrimitive (which is already tracked).
    auto *strprim = dyn_vmcast<StringPrimitive>(cell);
    if (strprim && !isBufferedStringPrimitive(cell)) {
      auto &stat = strprim->isASCII()
          ? acceptor.diagnostic.stats.breakdown["StringPrimitive (ASCII)"]
          : acceptor.diagnostic.stats.breakdown["StringPrimitive (UTF-16)"];
      stat.count++;
      const size_t len = strprim->getStringLength();
      // If the string is UTF-16 then the length is in terms of 16 bit
      // characters.
      const size_t sz = strprim->isASCII() ? len : len * 2;
      stat.size += sz;
      if (len < 8) {
        auto &subStat =
            stat.breakdown
                ["StringPrimitive (size " + std::to_string(len) + ")"];
        subStat.count++;
        subStat.size += sz;
      }
    }
  });

  assert(
      acceptor.diagnostic.stats.size == 0 &&
      acceptor.diagnostic.stats.count == 0 &&
      "Should not be setting overall stats during heap scan.");
  for (const auto &substat : acceptor.diagnostic.stats.breakdown)
    acceptor.diagnostic.stats.count += substat.second.count;
  acceptor.diagnostic.stats.size = allocatedBytes;
  acceptor.diagnostic.sizeDiagnosticFrame();
}

} // namespace vm
} // namespace hermes

#undef DEBUG_TYPE
