/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/Domain.h"

#include "hermes/VM/Callable.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/Profiler/SamplingProfiler.h"

namespace hermes {
namespace vm {

const VTable Domain::vt{
    CellKind::DomainKind,
    cellSize<Domain>(),
    /* allowLargeAlloc */ false,
    _finalizeImpl,
    _mallocSizeImpl,
    nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
    ,
    VTable::HeapSnapshotMetadata{
        HeapSnapshot::NodeType::Code,
        nullptr,
        Domain::_snapshotAddEdgesImpl,
        Domain::_snapshotAddNodesImpl,
        nullptr}
#endif
};

void DomainBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const Domain *>(cell);
  mb.setVTable(&Domain::vt);
  mb.addField("cjsModules", &self->cjsModules_);
  mb.addField("throwingRequire", &self->throwingRequire_);
}

PseudoHandle<Domain> Domain::create(Runtime &runtime) {
  auto *cell = runtime.makeAFixed<Domain, HasFinalizer::Yes>();
  auto self = createPseudoHandle(cell);
  return self;
}

void Domain::_finalizeImpl(GCCell *cell, GC &gc) {
  auto *self = vmcast<Domain>(cell);
  for (RuntimeModule *rm : self->runtimeModules_) {
    gc.getIDTracker().untrackNative(rm);
  }
  self->~Domain();
}

PseudoHandle<NativeFunction> Domain::getThrowingRequire(
    Runtime &runtime) const {
  return createPseudoHandle(throwingRequire_.get(runtime));
}

size_t Domain::_mallocSizeImpl(GCCell *cell) {
  auto *self = vmcast<Domain>(cell);
  size_t rmSize = 0;
  for (auto *rm : self->runtimeModules_)
    rmSize += sizeof(RuntimeModule) + rm->additionalMemorySize() +
        rm->regExpCacheMemorySize();

  return self->cjsRuntimeModules_.capacity_in_bytes() +
      self->cjsModuleTable_.getMemorySize() +
      self->runtimeModules_.capacity_in_bytes() + rmSize;
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
void Domain::_snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap) {
  auto *const self = vmcast<Domain>(cell);
  for (RuntimeModule *rm : self->runtimeModules_)
    snap.addNamedEdge(
        HeapSnapshot::EdgeType::Internal, "RuntimeModule", gc.getNativeID(rm));
}

void Domain::_snapshotAddNodesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap) {
  auto *const self = vmcast<Domain>(cell);
  for (RuntimeModule *rm : self->runtimeModules_) {
    // Create a native node for each RuntimeModule owned by this domain.
    rm->snapshotAddNodes(gc, snap);
    snap.beginNode();
    rm->snapshotAddEdges(gc, snap);
    snap.endNode(
        HeapSnapshot::NodeType::Native,
        "RuntimeModule",
        gc.getNativeID(rm),
        sizeof(RuntimeModule) + rm->additionalMemorySize(),
        0);
  }
}
#endif

ExecutionStatus Domain::importCJSModuleTable(
    Handle<Domain> self,
    Runtime &runtime,
    RuntimeModule *runtimeModule) {
  if (runtimeModule->getBytecode()->getCJSModuleTable().empty() &&
      runtimeModule->getBytecode()->getCJSModuleTableStatic().empty()) {
    // Nothing to do, avoid allocating and simply return.
    return ExecutionStatus::RETURNED;
  }

  static_assert(
      CJSModuleSize < 10, "CJSModuleSize must be small to avoid overflow");

  struct : public Locals {
    PinnedValue<ArrayStorage> cjsModules;
    PinnedValue<StringPrimitive> str;
    PinnedValue<NativeFunction> requireFn;
    PinnedValue<RequireContext> context;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (!self->cjsModules_) {
    // Create the module table on first import.
    // If module IDs are contiguous and start from 0, we won't need to resize
    // for this RuntimeModule.

    const uint64_t firstSegmentModules =
        runtimeModule->getBytecode()->getCJSModuleTable().size() +
        runtimeModule->getBytecode()->getCJSModuleTableStatic().size();

    assert(
        firstSegmentModules <= std::numeric_limits<uint32_t>::max() &&
        "number of modules is 32 bits due to the bytecode format");

    // Use uint64_t to allow us to check for overflow.
    const uint64_t requiredSize = firstSegmentModules * CJSModuleSize;
    if (requiredSize > std::numeric_limits<uint32_t>::max()) {
      return runtime.raiseRangeError("Loaded module count exceeded limit");
    }

    auto cjsModulesRes = ArrayStorage::create(runtime, requiredSize);
    if (LLVM_UNLIKELY(cjsModulesRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.cjsModules = vmcast<ArrayStorage>(*cjsModulesRes);
    self->cjsRuntimeModules_.reserve(firstSegmentModules);
    for (size_t i = self->cjsRuntimeModules_.size(); i < firstSegmentModules;
         i++) {
      self->cjsRuntimeModules_.push_back(nullptr, runtime.getHeap());
    }

    lv.requireFn = NativeFunction::create(
        runtime,
        (void *)TypeErrorKind::InvalidDynamicRequire,
        throwTypeError,
        Predefined::getSymbolID(Predefined::emptyString),
        0);

    lv.str = runtime.getPredefinedString(Predefined::emptyString);
    lv.context = RequireContext::create(runtime, self, lv.str);

    // Set the require.context property.
    PropertyFlags pf = PropertyFlags::defaultNewNamedPropertyFlags();
    pf.writable = 0;
    pf.configurable = 0;
    if (LLVM_UNLIKELY(
            JSObject::defineNewOwnProperty(
                lv.requireFn,
                runtime,
                Predefined::getSymbolID(Predefined::context),
                pf,
                lv.context) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    self->throwingRequire_.set(runtime, *lv.requireFn, runtime.getHeap());
  } else {
    lv.cjsModules = self->cjsModules_.get(runtime);
  }

  assert(lv.cjsModules.get() && "cjsModules not set");

  // Find the maximum module ID so we can resize cjsModules at most once per
  // RuntimeModule.
  uint64_t maxModuleID = lv.cjsModules->size() / CJSModuleSize;

  // The non-static module table does not store module IDs. They are assigned
  // during registration by counting insertions to cjsModuleTable_. Count the
  // insertions up front.
  for (const auto &pair : runtimeModule->getBytecode()->getCJSModuleTable()) {
    SymbolID symbolId =
        runtimeModule->getSymbolIDFromStringIDMayAllocate(pair.first);
    if (self->cjsModuleTable_.find(symbolId) == self->cjsModuleTable_.end()) {
      ++maxModuleID;
    }
  }

  // The static module table stores module IDs in an arbitrary order. Scan for
  // the maximum ID.
  for (const auto &pair :
       runtimeModule->getBytecode()->getCJSModuleTableStatic()) {
    const auto &moduleID = pair.first;
    if (moduleID > maxModuleID) {
      maxModuleID = moduleID;
    }
  }

  assert(
      maxModuleID <= std::numeric_limits<uint32_t>::max() &&
      "number of modules is 32 bits due to the bytecode format");

  // Use uint64_t to allow us to check for overflow.
  const uint64_t requiredSize = (maxModuleID + 1) * CJSModuleSize;
  if (requiredSize > std::numeric_limits<uint32_t>::max()) {
    return runtime.raiseRangeError("Loaded module count exceeded limit");
  }

  // Resize the array to allow for the new modules, if necessary.
  if (requiredSize > lv.cjsModules->size()) {
    if (LLVM_UNLIKELY(
            ArrayStorage::resize(lv.cjsModules, runtime, requiredSize) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    self->cjsRuntimeModules_.reserve(maxModuleID + 1);
    for (size_t i = self->cjsRuntimeModules_.size(); i <= maxModuleID; i++) {
      self->cjsRuntimeModules_.push_back(nullptr, runtime.getHeap());
    }
  }

  /// \return Whether the module with ID \param moduleID has been registered in
  /// cjsModules. \pre Space has been allocated for this module's record in
  /// cjsModules.
  const auto isModuleRegistered = [&lv,
                                   maxModuleID](uint32_t moduleID) -> bool {
    assert(
        moduleID <= maxModuleID &&
        "CJS module ID exceeds maximum known module ID");
    (void)maxModuleID;
    uint32_t index = moduleID * CJSModuleSize;
    uint32_t requiredSize = index + CJSModuleSize;
    assert(
        lv.cjsModules->size() >= requiredSize &&
        "CJS module ID exceeds allocated storage");
    (void)requiredSize;
    return !lv.cjsModules->at(index + FunctionIndexOffset).isEmpty();
  };

  /// Register CJS module \param moduleID in the runtime module table.
  /// \return The index into cjsModules where this module's record begins.
  /// \pre Space has been allocated for this module's record in cjsModules.
  /// \pre Space has been allocated for this module's RuntimeModule* in
  /// cjsRuntimeModules_.
  /// \pre There is no module already registered under moduleID.
  auto &cjsEntryModuleID = self->cjsEntryModuleID_;
  auto &cjsRuntimeModules = self->cjsRuntimeModules_;
  const auto registerModule =
      [&runtime,
       &lv,
       &cjsRuntimeModules,
       runtimeModule,
       &isModuleRegistered,
       &cjsEntryModuleID](uint32_t moduleID, uint32_t functionID) -> uint32_t {
    assert(!isModuleRegistered(moduleID) && "CJS module ID collision occurred");
    (void)isModuleRegistered;
    if (LLVM_UNLIKELY(!cjsEntryModuleID.hasValue())) {
      cjsEntryModuleID = moduleID;
    }
    uint32_t index = moduleID * CJSModuleSize;
    lv.cjsModules->set(
        index + CachedExportsOffset,
        HermesValue::encodeEmptyValue(),
        runtime.getHeap());
    lv.cjsModules->set(
        index + ModuleOffset,
        HermesValue::encodeNullValue(),
        runtime.getHeap());
    lv.cjsModules->set(
        index + FunctionIndexOffset,
        HermesValue::encodeNativeUInt32(functionID),
        runtime.getHeap());
    cjsRuntimeModules[moduleID] = runtimeModule;
    assert(isModuleRegistered(moduleID) && "CJS module was not registered");
    return index;
  };

  // Import full table that allows dynamic requires.
  for (const auto &pair : runtimeModule->getBytecode()->getCJSModuleTable()) {
    SymbolID symbolId =
        runtimeModule->getSymbolIDFromStringIDMayAllocate(pair.first);
    auto emplaceRes = self->cjsModuleTable_.try_emplace(symbolId, 0xffffffff);
    if (emplaceRes.second) {
      // This module has not been registered before.
      // Assign it an arbitrary unused module ID, because nothing will be
      // referencing that ID from outside Domain.
      // Counting insertions to cjsModuleTable_ is a valid source of unique IDs
      // since a given Domain uses either dynamic requires or statically
      // resolved requires.
      uint32_t moduleID = self->cjsModuleTable_.size() - 1;
      const auto functionID = pair.second;
      auto index = registerModule(moduleID, functionID);
      // Update the mapping from symbolId to an index into cjsModules.
      emplaceRes.first->second = index;
    }
  }

  // Import table to be used for requireFast.
  for (const auto &pair :
       runtimeModule->getBytecode()->getCJSModuleTableStatic()) {
    const auto &moduleID = pair.first;
    const auto &functionID = pair.second;
    if (!isModuleRegistered(moduleID)) {
      registerModule(moduleID, functionID);
    }
  }

  self->cjsModules_.set(runtime, lv.cjsModules.get(), runtime.getHeap());
  return ExecutionStatus::RETURNED;
}

const ObjectVTable RequireContext::vt{
    VTable(CellKind::RequireContextKind, cellSize<RequireContext>()),
    RequireContext::_getOwnIndexedRangeImpl,
    RequireContext::_haveOwnIndexedImpl,
    RequireContext::_getOwnIndexedPropertyFlagsImpl,
    RequireContext::_getOwnIndexedImpl,
    RequireContext::_setOwnIndexedImpl,
    RequireContext::_deleteOwnIndexedImpl,
    RequireContext::_checkAllOwnIndexedImpl,
};

void RequireContextBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<RequireContext>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const RequireContext *>(cell);
  mb.setVTable(&RequireContext::vt);
  mb.addField(&self->domain_);
  mb.addField(&self->dirname_);
}

PseudoHandle<RequireContext> RequireContext::create(
    Runtime &runtime,
    Handle<Domain> domain,
    Handle<StringPrimitive> dirname) {
  auto *cell = runtime.makeAFixed<RequireContext>(
      runtime, runtime.objectPrototype, runtime.classRequireContext);
  auto self = JSObjectInit::initToPseudoHandle(runtime, cell);
  self->domain_.set(runtime, *domain, runtime.getHeap());
  self->dirname_.set(runtime, *dirname, runtime.getHeap());
  return self;
}

} // namespace vm
} // namespace hermes
