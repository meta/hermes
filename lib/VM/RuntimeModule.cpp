/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/RuntimeModule.h"

#include "hermes/Support/PerfSection.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/HiddenClass-inline.h"
#include "hermes/VM/ModuleExportsCache.h"
#include "hermes/VM/Predefined.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringPrimitiveValueDenseMapInfo-inline.h"
#include "hermes/VM/WeakRoot-inline.h"

namespace hermes {
namespace vm {

RuntimeModule::RuntimeModule(
    Runtime &runtime,
    Handle<Domain> domain,
    RuntimeModuleFlags flags,
    llvh::StringRef sourceURL,
    facebook::hermes::debugger::ScriptID scriptID)
    : runtime_(runtime),
      domain_(*domain, runtime),
      flags_(flags),
      sourceURL_(sourceURL),
      scriptID_(scriptID) {
  // Reserve add cache entry 0 as an "always miss" entry.
  addCacheEntries_.emplace_back();

  runtime_.addRuntimeModule(this);
  Domain::addRuntimeModule(domain, runtime, this);
}

SymbolID RuntimeModule::createSymbolFromStringIDMayAllocate(
    StringID stringID,
    const StringTableEntry &entry,
    OptValue<uint32_t> mhash) {
  // Use manual pointer arithmetic to avoid out of bounds errors on empty
  // string accesses.
  auto strStorage = bcProvider_->getStringStorage();
  if (entry.isUTF16()) {
    const char16_t *s =
        (const char16_t *)(strStorage.begin() + entry.getOffset());
    UTF16Ref str{s, entry.getLength()};
    uint32_t hash = mhash ? *mhash : hashString(str);
    return mapStringMayAllocate(str, stringID, hash);
  } else {
    // ASCII.
    const char *s = (const char *)strStorage.begin() + entry.getOffset();
    ASCIIRef str{s, entry.getLength()};
    uint32_t hash = mhash ? *mhash : hashString(str);
    return mapStringMayAllocate(str, stringID, hash);
  }
}

RuntimeModule::~RuntimeModule() {
  if (bcProvider_ && !bcProvider_->getRawBuffer().empty())
    runtime_.getCrashManager().unregisterMemory(bcProvider_.get());
  runtime_.getCrashManager().unregisterMemory(this);
  runtime_.removeRuntimeModule(this);

  for (const auto &block : functionMap_) {
    runtime_.getHeap().getIDTracker().untrackNative(block.get());
  }
  runtime_.getHeap().getIDTracker().untrackNative(&functionMap_);

  // Entries may outlive the module, held by JSRegExps, but only the module
  // reports them.
  for (const auto &entry : regExpCache_) {
    if (entry)
      runtime_.getHeap().getIDTracker().untrackNative(entry.get());
  }
  runtime_.getHeap().getIDTracker().untrackNative(&regExpCache_);
}

void RuntimeModule::prepareForDestruction() {
  for (int i = 0, e = functionMap_.size(); i < e; i++) {
    if (functionMap_[i] != nullptr &&
        functionMap_[i]->getRuntimeModule() != this) {
      functionMap_[i] = nullptr;
    }
  }
}

CallResult<RuntimeModule *> RuntimeModule::create(
    Runtime &runtime,
    Handle<Domain> domain,
    facebook::hermes::debugger::ScriptID scriptID,
    std::shared_ptr<hbc::BCProvider> &&bytecode,
    RuntimeModuleFlags flags,
    llvh::StringRef sourceURL) {
  RuntimeModule *result;
  result = new RuntimeModule(runtime, domain, flags, sourceURL, scriptID);
  runtime.getCrashManager().registerMemory(result, sizeof(*result));
  if (bytecode) {
    if (result->initializeMayAllocate(std::move(bytecode)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // If the BC provider is backed by a buffer, register the BC provider struct
    // (but not the buffer contents, since that might be too large).
    if (result->bcProvider_ && !result->bcProvider_->getRawBuffer().empty())
      runtime.getCrashManager().registerMemory(
          result->bcProvider_.get(), sizeof(hbc::BCProviderFromBuffer));
  }
  return result;
}

RuntimeModule *RuntimeModule::createUninitialized(
    Runtime &runtime,
    Handle<Domain> domain,
    RuntimeModuleFlags flags,
    facebook::hermes::debugger::ScriptID scriptID) {
  return new RuntimeModule(runtime, domain, flags, "", scriptID);
}

void RuntimeModule::initializeWithoutCJSModulesMayAllocate(
    std::shared_ptr<hbc::BCProvider> &&bytecode) {
  assert(!bcProvider_ && "RuntimeModule already initialized");
  bcProvider_ = std::move(bytecode);
  importStringIDMapMayAllocate();
  initializeFunctionMap();
  // Initialize the object literal hidden class cache.
  auto numObjShapes = bcProvider_->getObjectShapeTable().size();
  objectLiteralHiddenClasses_.resize(numObjShapes);
  stringSwitchImmTables_.resize(bcProvider_->getNumStringSwitchImmInstrs());
}

ExecutionStatus RuntimeModule::initializeMayAllocate(
    std::shared_ptr<hbc::BCProvider> &&bytecode) {
  initializeWithoutCJSModulesMayAllocate(std::move(bytecode));
  if (LLVM_UNLIKELY(importCJSModuleTable() == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

void RuntimeModule::initAfterLazyCompilation() {
  importStringIDMapMayAllocate();
  initializeFunctionMap();
  // Initialize the object literal hidden class cache.
  auto numObjShapes = bcProvider_->getObjectShapeTable().size();
  objectLiteralHiddenClasses_.resize(numObjShapes);
  assert(
      bcProvider_->getNumStringSwitchImmInstrs() >=
          stringSwitchImmTables_.size() &&
      "Number of string switches detected should only grow.");
  stringSwitchImmTables_.resize(bcProvider_->getNumStringSwitchImmInstrs());
}

CodeBlock *RuntimeModule::getCodeBlockSlowPath(unsigned index) {
  if (bcProvider_->isFunctionLazy(index)) {
    functionMap_[index] = CodeBlock::createCodeBlock(
        this, bcProvider_->getFunctionHeader(index), nullptr, index);
    return functionMap_[index].get();
  }
  functionMap_[index] = CodeBlock::createCodeBlock(
      this,
      bcProvider_->getFunctionHeader(index),
      bcProvider_->getBytecode(index),
      index);
  return functionMap_[index].get();
}

void RuntimeModule::importStringIDMapMayAllocate() {
  assert(bcProvider_ && "Uninitialized RuntimeModule");
  PerfSection perf("Import String ID Map");
  GCScope scope(runtime_);

  auto strTableSize = bcProvider_->getStringCount();

  // Populate the string ID map with empty identifiers.
  stringIDMap_.resize(strTableSize, RootSymbolID(SymbolID::empty()));

  if (runtime_.getVMExperimentFlags() & experiments::MAdviseStringsSequential) {
    bcProvider_->adviseStringTableSequential();
  }

  if (runtime_.getVMExperimentFlags() & experiments::MAdviseStringsWillNeed) {
    bcProvider_->willNeedStringTable();
  }

  // Get the array of pre-computed hashes from identifiers in the bytecode
  // to their runtime representation as SymbolIDs.
  auto kinds = bcProvider_->getStringKinds();
  auto hashes = bcProvider_->getIdentifierHashes();
  assert(
      hashes.size() <= strTableSize &&
      "Should not have more strings than identifiers");

  // Preallocate enough space to store all identifiers to prevent
  // unnecessary allocations. NOTE: If this module is not the first module,
  // then this is an underestimate.
  runtime_.getIdentifierTable().reserve(hashes.size());
  {
    uint32_t hashID = identifierHashesOffset_;

    for (auto entry : kinds.drop_front(stringKindsOffset_)) {
      switch (entry.kind()) {
        case StringKind::String:
          nextStringID_ += entry.count();
          break;

        case StringKind::Identifier:
          for (uint32_t i = 0; i < entry.count();
               ++i, ++nextStringID_, ++hashID) {
            createSymbolFromStringIDMayAllocate(
                nextStringID_,
                bcProvider_->getStringTableEntry(nextStringID_),
                hashes[hashID]);
          }
          break;
      }
    }

    assert(
        nextStringID_ == strTableSize &&
        "Should map every string in the bytecode.");
    assert(hashID == hashes.size() && "Should hash all identifiers.");
  }

  if (runtime_.getVMExperimentFlags() & experiments::MAdviseStringsRandom) {
    bcProvider_->adviseStringTableRandom();
  }

  stringKindsOffset_ = bcProvider_->getStringKinds().size();
  identifierHashesOffset_ = bcProvider_->getIdentifierHashes().size();

  if (strTableSize == 0) {
    // If the string table turns out to be empty,
    // we always add one empty string to it.
    // Note that this can only happen when we are creating the RuntimeModule
    // in a non-standard way, either in unit tests or the special
    // emptyCodeBlockRuntimeModule_ in Runtime where the creation happens
    // manually instead of going through bytecode module generation.
    // In those cases, functions will be created with a default nameID=0
    // without adding the name string into the string table. Hence here
    // we need to add it manually and it will have index 0.
    ASCIIRef s;
    stringIDMap_.push_back({});
    mapStringMayAllocate(s, 0, hashString(s));
  }
}

void RuntimeModule::initializeFunctionMap() {
  assert(bcProvider_ && "Uninitialized RuntimeModule");
  assert(
      bcProvider_->getFunctionCount() >= functionMap_.size() &&
      "Unexpected size reduction. Lazy module missing functions?");
  functionMap_.resize(bcProvider_->getFunctionCount());
}

ExecutionStatus RuntimeModule::importCJSModuleTable() {
  PerfSection perf("Import CJS Module Table");
  return Domain::importCJSModuleTable(getDomain(runtime_), runtime_, this);
}

StringPrimitive *RuntimeModule::getStringPrimFromStringIDMayAllocate(
    StringID stringID) {
  return runtime_.getStringPrimFromSymbolID(
      getSymbolIDFromStringIDMayAllocate(stringID));
}

std::string RuntimeModule::getStringFromStringID(StringID stringID) {
  auto entry = bcProvider_->getStringTableEntry(stringID);
  auto strStorage = bcProvider_->getStringStorage();
  if (entry.isUTF16()) {
    const char16_t *s =
        (const char16_t *)(strStorage.begin() + entry.getOffset());
    std::string out;
    convertUTF16ToUTF8WithReplacements(out, UTF16Ref{s, entry.getLength()});
    return out;
  } else {
    // ASCII.
    const char *s = (const char *)strStorage.begin() + entry.getOffset();
    return std::string{s, entry.getLength()};
  }
}

llvh::ArrayRef<uint8_t> RuntimeModule::getBigIntBytesFromBigIntId(
    BigIntID bigIntId) const {
  assert(
      bigIntId < bcProvider_->getBigIntTable().size() && "Invalid bigint id");
  bigint::BigIntTableEntry entry = bcProvider_->getBigIntTable()[bigIntId];
  return bcProvider_->getBigIntStorage().slice(entry.offset, entry.length);
}

llvh::ArrayRef<uint8_t> RuntimeModule::getRegExpBytecodeFromRegExpID(
    uint32_t regExpId) const {
  assert(
      regExpId < bcProvider_->getRegExpTable().size() && "Invalid regexp id");
  RegExpTableEntry entry = bcProvider_->getRegExpTable()[regExpId];
  return bcProvider_->getRegExpStorage().slice(entry.offset, entry.length);
}

llvh::IntrusiveRefCntPtr<RegExpBytecode> RuntimeModule::getRegExpBytecode(
    uint32_t regExpID) {
  if (LLVM_UNLIKELY(regExpID >= regExpCache_.size())) {
    // Lazy compilation adds regexps to the table as functions compile.
    size_t tableSize = bcProvider_->getRegExpTable().size();
    assert(regExpID < tableSize && "Invalid regexp id");
    regExpCache_.resize(tableSize);
  }
  llvh::IntrusiveRefCntPtr<RegExpBytecode> &entry = regExpCache_[regExpID];
  if (LLVM_UNLIKELY(!entry)) {
    entry = RegExpBytecode::create(
        getRegExpBytecodeFromRegExpID(regExpID),
        RegExpBytecode::Ownership::Cached);
  }
  return entry;
}

template <typename T>
SymbolID RuntimeModule::mapStringMayAllocate(
    llvh::ArrayRef<T> str,
    StringID stringID,
    uint32_t hash) {
  // Create a SymbolID for a given string. In general a SymbolID holds onto an
  // intern'd StringPrimitive. As an optimization, if this RuntimeModule is
  // persistent, then it will not be deallocated before the Runtime, and we can
  // have the SymbolID hold a raw pointer into the storage and produce the
  // StringPrimitive when it is first required.
  SymbolID id;
  if (flags_.persistent) {
    // Registering a lazy identifier does not allocate, so we do not need a
    // GC scope.
    id = runtime_.getIdentifierTable().registerLazyIdentifier(
        runtime_, str, hash);
  } else {
    // Accessing a symbol non-lazily may allocate in the GC heap, so add a scope
    // marker.
    GCScopeMarkerRAII scopeMarker{runtime_};
    id = *runtime_.ignoreAllocationFailure(
        runtime_.getIdentifierTable().getSymbolHandle(runtime_, str, hash));
  }
  stringIDMap_[stringID] = RootSymbolID(id);
  return id;
}

OptValue<uint32_t> RuntimeModule::allocateAddCacheEntry() {
  if (addCacheEntries_.size() < WritePropertyCacheEntry::kMaxAddCacheIndex) {
    uint32_t index = addCacheEntries_.size();
    addCacheEntries_.emplace_back();
    return index;
  }
  return llvh::None;
}

void RuntimeModule::markRoots(RootAcceptor &acceptor, bool markLongLived) {
  for (auto &it : templateMap_) {
    acceptor.acceptPtr(it.second);
  }

  if (markLongLived) {
    for (auto symbol : stringIDMap_) {
      if (symbol.isValid()) {
        acceptor.accept(symbol);
      }
    }
    for (auto &table : stringSwitchImmTables_) {
      for (auto &[str, _] : table) {
        acceptor.acceptPtr(str);
      }
    }
  }

  acceptor.acceptPtr(moduleExports_);
}

void RuntimeModule::markWeakRoots(
    WeakRootAcceptor &acceptor,
    bool markLongLived) {
  if (markLongLived) {
    for (auto &cbPtr : functionMap_) {
      // Only mark a CodeBlock is its non-null, and has not been scanned
      // previously in this top-level markRoots invocation.
      if (cbPtr != nullptr && cbPtr->getRuntimeModule() == this) {
        cbPtr->markWeakElementsInCaches(runtime_, acceptor);
      }
    }
    for (auto &entry : objectLiteralHiddenClasses_) {
      acceptor.acceptWeak(entry);
    }
  }

  if (markLongLived) {
    for (auto &entry : addCacheEntries_) {
      acceptor.acceptWeak(entry.startClazz);
      acceptor.acceptWeak(entry.resultClazz);
      acceptor.acceptWeak(entry.parent);
    }
  } else {
    for (auto &entry : addCacheEntries_) {
      acceptor.acceptWeak(entry.parent);
    }
  }
}

HiddenClass *RuntimeModule::findCachedLiteralHiddenClass(
    Runtime &runtime,
    JSObject *parent,
    uint32_t shapeTableIndex) const {
  assert(
      shapeTableIndex < objectLiteralHiddenClasses_.size() &&
      "Invalid shape table index");
  auto *cached = objectLiteralHiddenClasses_[shapeTableIndex].get(
      runtime, runtime.getHeap());
  if (cached && cached->getObjectParent(runtime) == parent) {
    return cached;
  }
  return nullptr;
}

void RuntimeModule::setCachedLiteralHiddenClass(
    Runtime &runtime,
    unsigned shapeTableIndex,
    HiddenClass *clazz) {
  if (shapeTableIndex == hbc::SHAPE_TABLE_CACHING_DISABLED)
    return;
  assert(
      !findCachedLiteralHiddenClass(
          runtime, clazz->getObjectParent(runtime), shapeTableIndex) &&
      "Why are we caching an item already cached?");
  objectLiteralHiddenClasses_[shapeTableIndex].set(runtime, clazz);
}

size_t RuntimeModule::additionalMemorySize() const {
  return stringIDMap_.capacity() * sizeof(SymbolID) +
      addCacheEntries_.capacity_in_bytes() +
      objectLiteralHiddenClasses_.size() * sizeof(WeakRoot<HiddenClass>) +
      templateMap_.getMemorySize();
}

size_t RuntimeModule::regExpCacheMemorySize() const {
  size_t size = regExpCache_.capacity() * sizeof(regExpCache_[0]);
  for (const auto &entry : regExpCache_) {
    if (entry)
      size += entry->mallocSize();
  }
  return size;
}

/// Set the module export for module \p modIndex to \p modExport.
void RuntimeModule::setModuleExport(
    Runtime &runtime,
    uint32_t modIndex,
    Handle<> modExport) {
  module_export_cache::set(runtime, moduleExports_, modIndex, modExport);
}

void RuntimeModule::initializeStringSwitchImmTable(
    StringSwitchDenseMap &table,
    const hbc::StringSwitchTableCase *cases,
    uint32_t size) {
  assert(
      table.empty() &&
      "precondition -- should only be called with empty table.");
  // The index handed to the next case string seen for the first time. Cases
  // are numbered densely in [0, table.size()), which is what lets a compiled
  // body use the index to select a slot in its own jump table.
  uint32_t nextCaseIndex = 0;
  for (unsigned i = 0; i < size; i++) {
    const hbc::StringSwitchTableCase &switchCase = cases[i];
    // May run a GC, which visits this table as a root and updates the keys
    // already in it. strPrim is used below without an intervening allocation.
    StringPrimitive *strPrim =
        getStringPrimFromStringIDMayAllocate(switchCase.caseLabelStringID);
    auto [it, inserted] = table.try_emplace(strPrim);
    // A repeated case label cannot occur in a well-formed switch, but if it
    // did, the last one would win -- as it did when this was a plain
    // table[strPrim] assignment -- and both would share one case index.
    it->second.bytecodeOffset = switchCase.target;
    if (inserted)
      it->second.caseIndex = nextCaseIndex++;
  }
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
void RuntimeModule::snapshotAddNodes(GC &gc, HeapSnapshot &snap) const {
  // Create a native node for each CodeBlock owned by this module.
  for (const auto &cb : functionMap_) {
    // Skip the null code blocks, they are lazily inserted the first time
    // they are used.
    if (cb && cb->getRuntimeModule() == this) {
      // Only add a CodeBlock if this runtime module is the owner.
      snap.beginNode();
      snap.endNode(
          HeapSnapshot::NodeType::Native,
          "CodeBlock",
          gc.getNativeID(cb.get()),
          sizeof(CodeBlock) + cb->additionalMemorySize(),
          0);
    }
  }

  // Create a node for functionMap_.
  snap.beginNode();
  // Create an edge to each CodeBlock owned by this module.
  for (int i = 0, e = functionMap_.size(); i < e; i++) {
    const CodeBlock *cb = functionMap_[i].get();
    // Skip the null code blocks, they are lazily inserted the first time
    // they are used.
    if (cb && cb->getRuntimeModule() == this) {
      // Only add a CodeBlock if this runtime module is the owner.
      snap.addIndexedEdge(
          HeapSnapshot::EdgeType::Element, i, gc.getNativeID(cb));
    }
  }
  snap.endNode(
      HeapSnapshot::NodeType::Native,
      "std::vector<CodeBlock *>",
      gc.getNativeID(&functionMap_),
      functionMap_.capacity() * sizeof(CodeBlock *),
      0);

  // Create a native node for each cached regexp bytecode entry.
  for (const auto &entry : regExpCache_) {
    if (entry) {
      snap.beginNode();
      snap.endNode(
          HeapSnapshot::NodeType::Native,
          "RegExpBytecode",
          gc.getNativeID(entry.get()),
          entry->mallocSize(),
          0);
    }
  }

  // Create a node for regExpCache_, with an edge to each entry.
  snap.beginNode();
  for (int i = 0, e = regExpCache_.size(); i < e; i++) {
    if (const RegExpBytecode *entry = regExpCache_[i].get()) {
      snap.addIndexedEdge(
          HeapSnapshot::EdgeType::Element, i, gc.getNativeID(entry));
    }
  }
  snap.endNode(
      HeapSnapshot::NodeType::Native,
      "std::vector<RegExpBytecode *>",
      gc.getNativeID(&regExpCache_),
      regExpCache_.capacity() * sizeof(regExpCache_[0]),
      0);
}

void RuntimeModule::snapshotAddEdges(GC &gc, HeapSnapshot &snap) const {
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Internal,
      "functionMap",
      gc.getNativeID(&functionMap_));
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Internal,
      "regExpCache",
      gc.getNativeID(&regExpCache_));
}
#endif // HERMES_MEMORY_INSTRUMENTATION

namespace detail {

StringID mapStringMayAllocate(RuntimeModule &module, const char *str) {
  module.stringIDMap_.push_back({});
  module.mapStringMayAllocate(
      createASCIIRef(str), module.stringIDMap_.size() - 1);
  return module.stringIDMap_.size() - 1;
}

} // namespace detail

} // namespace vm

} // namespace hermes
