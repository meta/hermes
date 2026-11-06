/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_RUNTIMEMODULE_H
#define HERMES_VM_RUNTIMEMODULE_H

#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/Public/DebuggerTypes.h"
#include "hermes/Support/HashString.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/IdentifierTable.h"
#include "hermes/VM/RegExpBytecode.h"

#include "hermes/VM/StringPrimitiveValueDenseMapInfo.h"
#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/WeakRoot.h"

#include "llvh/ADT/simple_ilist.h"

namespace hermes {
namespace vm {

template <typename HVType>
class ArrayStorageBase;
class CodeBlock;
class Runtime;

using BigIntID = uint32_t;
using StringID = uint32_t;

namespace detail {
/// Unit tests need to call into this function. We cannot expose the
/// templated version as its definition is in the cpp file, and will
/// cause a link error.
StringID mapStringMayAllocate(RuntimeModule &module, const char *str);
} // namespace detail

/// Flags supporting RuntimeModule.
union RuntimeModuleFlags {
  struct {
    /// Whether this runtime module should persist in memory (i.e. never get
    /// freed even when refCount_ goes to 0.) This is needed when we want to
    /// have lazy identifiers whose string content is a pointer to the string
    /// storage in the bytecode module. We should only make the first (biggest)
    /// module persistent.
    /// This flag must not be set when creating a RuntimeModule for lazy
    /// compiled Modules, because the BytecodeModule will change underneath the
    /// provider, so the VM can't hold pointers into the storage.
    bool persistent : 1;

    /// Whether this runtime module's epilogue should be hidden in
    /// runtime.getEpilogues().
    bool hidesEpilogue : 1;

    /// Whether functions in this module should be treated as builtins, meaning
    /// Function.prototype.toString() returns "[native code]" instead of the
    /// actual source. This is used for InternalJavaScript and extensions.
    bool funcsAreBuiltins : 1;
  };
  uint8_t flags;
  RuntimeModuleFlags() : flags(0) {}
};

struct SwitchTargets {
  // The offset of the basic block target corresponding to this switch case.
  // The offset is relative to the address of the StringSwitchImm instruction.
  int32_t bytecodeOffset = 0;
  // The index of this case in the switch: dense in [0, table size), assigned
  // in the order the cases are inserted by initializeStringSwitchImmTable().
  // This is what the JIT lookup helper returns. It is a property of the
  // bytecode, not of any compiled body, so the table remains valid across
  // recompilations; each compiled body carries its own table mapping case
  // index to a code address inside that body.
  uint32_t caseIndex = 0;
};

/// This DenseMap specialization is used at runtime to map string values
/// used in switch statements to the proper branch target.  Note that
/// it cannot use pointer equality -- the StringPrimitiveValueDenseMapInfo
/// trait compares StringPrimitives by their values.
using StringSwitchDenseMap = llvh::DenseMap<
    StringPrimitive *,
    SwitchTargets,
    StringPrimitiveValueDenseMapInfo>;

/// This class is used to store the non-instruction information needed to
/// execute code. The RuntimeModule owns a BytecodeModule, from which it copies
/// the string ID map and function map. Every CodeBlock contains a reference to
/// the RuntimeModule that contains its relevant information. Whenever a
/// JSFunction is created/destroyed, it will update the reference count of the
/// runtime module following through the code block.
/// CodeBlock's bytecode buffers live in a BytecodeFunction, which is owned by
/// BytecodeModule, which is stored in this RuntimeModule.
///
/// If executing a CodeBlock, construct a RuntimeModule with
/// RuntimeModule::create(runtime) first. If the string ID map and function map
/// are needed, then use RuntimeModule::create(runtime, bytecodeModule).
///
/// All RuntimeModule-s associated with a \c Runtime are kept together in a
/// linked list which can be walked to perform memory management tasks.
class RuntimeModule final : public llvh::ilist_node<RuntimeModule> {
 private:
  friend struct RuntimeOffsets;

  friend StringID detail::mapStringMayAllocate(
      RuntimeModule &module,
      const char *str);

  /// The runtime this module is associated with.
  Runtime &runtime_;

  /// The table maps from a sequential string id in the bytecode to an
  /// SymbolID.
  std::vector<RootSymbolID> stringIDMap_{};

  /// Lazy compilation: Next string ID to allocate when mapping strings.
  StringID nextStringID_ = 0;

  /// Lazy compilation: offset in stringKinds to begin at when importing the
  /// stringIDMap_.
  uint32_t stringKindsOffset_ = 0;

  /// Lazy compilation: offset in identifierHashes to begin at when importing
  /// the stringIDMap_.
  /// Updated when importStringIDMapMayAllocate runs.
  uint32_t identifierHashesOffset_ = 0;

  /// Weak pointer to a GC-managed Domain that owns this RuntimeModule.
  /// We use WeakRoot<Domain> here to express that the RuntimeModule does not
  /// own the Domain.
  /// We avoid using a raw pointer to Domain because we must be able to update
  /// it when the GC moves the Domain.
  WeakRoot<Domain> domain_;

  /// The table maps from a function index to a CodeBlock.
  std::vector<std::unique_ptr<CodeBlock>> functionMap_{};

  /// Each StringSwitchImm instructions is assigned a small, dense
  /// integer index at compile time.  This table has an entry for each
  /// such index.  The corresponding table will be populated on first
  /// use, and will map the case labels of the switch to the
  /// corresponding branch offset.
  std::vector<StringSwitchDenseMap> stringSwitchImmTables_;

  /// The byte-code provider for this RuntimeModule. The RuntimeModule is
  /// designed to own the provider exclusively, especially because in some
  /// cases the bytecode can be modified (e.g. for breakpoints). This however
  /// is a shared_ptr<> instead of unique_ptr<> for a pragmatic reason - when
  /// we run performance tests, we want to re-use a BCProvider between runtimes
  /// in order to minimize the noise.
  std::shared_ptr<hbc::BCProvider> bcProvider_{};

  /// Flags associated with the module.
  RuntimeModuleFlags flags_{};

  /// The sourceURL set explicitly for the module, or empty if none.
  std::string sourceURL_{};

  /// The scriptID assigned to this RuntimeModule.
  /// If this RuntimeModule is lazy, its scriptID matches its lazy root's
  /// scriptID.
  facebook::hermes::debugger::ScriptID scriptID_;

  /// A vector of cached hidden classes.
  TransparentConservativeVector<WeakRoot<HiddenClass>>
      objectLiteralHiddenClasses_;

  /// Vector of AddPropertyCacheEntry, where each element is lazily allocated
  /// whenever a CodeBlock needs a new entry.
  /// Stored in a central location here for easier marking of roots.
  TransparentConservativeVector<AddPropertyCacheEntry> addCacheEntries_;

  /// A map from template object ids to template objects.
  llvh::DenseMap<uint32_t, JSObject *> templateMap_;

  /// Indexed by module id: the exports of each module.  isEmpty means
  /// the module has not yet been initialized.
  ArrayStorageBase<HermesValue> *moduleExports_{nullptr};

  /// The shared bytecode of each regexp literal, indexed by regexp ID and
  /// created on first use. Grows with the regexp table under lazy
  /// compilation. Mutator thread only.
  std::vector<llvh::IntrusiveRefCntPtr<RegExpBytecode>> regExpCache_{};

  /// Registers the created RuntimeModule with \p domain, resulting in
  /// \p domain owning it. The RuntimeModule will be freed when the
  /// domain is collected..
  explicit RuntimeModule(
      Runtime &runtime,
      Handle<Domain> domain,
      RuntimeModuleFlags flags,
      llvh::StringRef sourceURL,
      facebook::hermes::debugger::ScriptID scriptID);

  CodeBlock *getCodeBlockSlowPath(unsigned index);

 public:
  ~RuntimeModule();

  /// Creates a new RuntimeModule under \p runtime and imports the CJS
  /// module table into \p domain.
  /// \param runtime the runtime to use for the identifier table.
  /// \param bytecode the bytecode to import strings and functions from.
  /// \param sourceURL the filename to report in exception backtraces.
  /// \return a raw pointer to the runtime module.
  static CallResult<RuntimeModule *> create(
      Runtime &runtime,
      Handle<Domain> domain,
      facebook::hermes::debugger::ScriptID scriptID,
      std::shared_ptr<hbc::BCProvider> &&bytecode = nullptr,
      RuntimeModuleFlags flags = {},
      llvh::StringRef sourceURL = {});

  /// Creates a new RuntimeModule that is not yet initialized. It may be
  /// initialized later through lazy compilation.
  /// \param runtime the runtime to use for the identifier table.
  /// \return a raw pointer to the runtime module.
  static RuntimeModule *createUninitialized(
      Runtime &runtime,
      Handle<Domain> domain,
      RuntimeModuleFlags flags = {},
      facebook::hermes::debugger::ScriptID scriptID =
          facebook::hermes::debugger::kInvalidLocation);

  /// Initialize modules created with \p createUninitialized,
  /// but do not import the CJS module table, allowing us to always succeed.
  /// \param bytecode the bytecode data to initialize it with.
  void initializeWithoutCJSModulesMayAllocate(
      std::shared_ptr<hbc::BCProvider> &&bytecode);

  /// Initialize modules created with \p createUninitialized and import the CJS
  /// module table from the provided bytecode file.
  /// \param bytecode the bytecode data to initialize it with.
  LLVM_NODISCARD ExecutionStatus
  initializeMayAllocate(std::shared_ptr<hbc::BCProvider> &&bytecode);

  /// Prepares this RuntimeModule for the systematic destruction of modules (
  /// either in Runtime destructor or Runtime::markDomainRefInRuntimeModules()).
  /// One RuntimeModule may refer CodeBlocks from another RuntimeMoudle, which
  /// could be destroyed first, so we must set pointers of these CodeBlocks to
  /// nullptr before destroying them.
  void prepareForDestruction();

  /// For opcodes that use a stringID as identifier explicitly, we know that
  /// the compiler would have marked the stringID as identifier, and hence
  /// we should have created the symbol during identifier table initialization.
  /// The symbol must already exist in the map. This is a fast path.
  SymbolID getSymbolIDMustExist(StringID stringID) {
    assert(
        stringIDMap_[stringID].isValid() &&
        "Symbol must exist for this string ID");
    return stringIDMap_[stringID];
  }

  /// \return the \c SymbolID for a string by string index. The symbol may not
  /// already exist for this given string ID. Hence we may need to create it
  /// on the fly.
  SymbolID getSymbolIDFromStringIDMayAllocate(StringID stringID) {
    SymbolID id = stringIDMap_[stringID];
    if (LLVM_UNLIKELY(!id.isValid())) {
      // Materialize this lazily created symbol.
      auto entry = bcProvider_->getStringTableEntry(stringID);
      id = createSymbolFromStringIDMayAllocate(stringID, entry, llvh::None);
    }
    assert(id.isValid() && "Failed to create symbol for stringID");
    return id;
  }

  /// Gets the SymbolID and looks it up in the runtime's identifier table.
  /// \return the StringPrimitive for a string by string index.
  StringPrimitive *getStringPrimFromStringIDMayAllocate(StringID stringID);

  /// \return the UTF-8 encoded string represented by stringID.
  /// Does no JS heap allocation.
  std::string getStringFromStringID(StringID stringID);

  /// \return the bigint bytes for a given \p bigIntId.
  llvh::ArrayRef<uint8_t> getBigIntBytesFromBigIntId(BigIntID bigIntId) const;

  /// \return the RegExp bytecode for a given regexp ID.
  llvh::ArrayRef<uint8_t> getRegExpBytecodeFromRegExpID(
      uint32_t regExpId) const;

  /// \return the shared bytecode of regexp literal \p regExpID, creating
  /// it from the module's bytecode on first use. Mutator thread only.
  llvh::IntrusiveRefCntPtr<RegExpBytecode> getRegExpBytecode(uint32_t regExpID);

  /// \return the number of functions in the function map.
  uint32_t getNumCodeBlocks() const {
    return functionMap_.size();
  }

  /// \return the CodeBlock for a function by function index.
  inline CodeBlock *getCodeBlockMayAllocate(unsigned index) {
    assert(index < functionMap_.size() && "Index out of bounds");
    if (LLVM_LIKELY(functionMap_[index])) {
      return functionMap_[index].get();
    }
    return getCodeBlockSlowPath(index);
  }

  const hbc::BCProvider *getBytecode() const {
    return bcProvider_.get();
  }

  hbc::BCProvider *getBytecode() {
    return bcProvider_.get();
  }

  std::shared_ptr<hbc::BCProvider> getBytecodeSharedPtr() {
    return bcProvider_;
  }

  /// \return true if the RuntimeModule has CJS modules that have not been
  /// statically resolved.
  bool hasCJSModules() const {
    return !getBytecode()->getCJSModuleTable().empty();
  }

  /// \return true if the RuntimeModule has CJS modules that have been resolved
  /// statically.
  bool hasCJSModulesStatic() const {
    return !getBytecode()->getCJSModuleTableStatic().empty();
  }

  /// \return the domain which owns this RuntimeModule.
  inline Handle<Domain> getDomain(Runtime &);

  /// \return a raw pointer to the domain which owns this RuntimeModule.
  inline Domain *getDomainUnsafe(Runtime &);

  /// \return a raw pointer to the domain which owns this RuntimeModule.
  /// Does not execute any read or write barriers on the GC. Should only be
  /// used during a signal handler or from a non-mutator thread.
  inline Domain *getDomainForSamplingProfiler(PointerBase &base);

  /// \return the Runtime of this module.
  Runtime &getRuntime() {
    return runtime_;
  }

  /// \return a constant reference to the function map.
  const std::vector<std::unique_ptr<CodeBlock>> &getFunctionMap() {
    return functionMap_;
  }

  StringSwitchDenseMap *getStringSwitchImmTables() {
    return stringSwitchImmTables_.data();
  }
#ifndef NDEBUG
  unsigned numStringSwitchImmTables() const {
    return stringSwitchImmTables_.size();
  }
#endif

  /// \return the sourceURL, or an empty string if none.
  llvh::StringRef getSourceURL() const {
    return sourceURL_;
  }

  /// \return whether this module hides its epilogue from
  /// Runtime.getEpilogues().
  bool hidesEpilogue() const {
    return flags_.hidesEpilogue;
  }

  /// \return whether functions in this module should be treated as builtins,
  /// i.e., their toString() returns "[native code]".
  bool funcsAreBuiltins() const {
    return flags_.funcsAreBuiltins;
  }

  /// \return any trailing data after the real bytecode.
  llvh::ArrayRef<uint8_t> getEpilogue() const {
    return bcProvider_->getEpilogue();
  }

  facebook::hermes::debugger::ScriptID getScriptID() const {
    return scriptID_;
  }

  /// Allocate a new AddPropertyCacheEntry.
  /// \return the index of the new entry, but if there's already too many to
  /// allocate a new entry, return llvh::None.
  /// Never returns 0.
  /// The entry index will be at most
  /// WritePropertyCacheEntry::kMaxAddCacheIndex.
  OptValue<uint32_t> allocateAddCacheEntry();

  /// \return the number of add cache entries.
  size_t numAddCacheEntries() const {
    return addCacheEntries_.size();
  }

  /// \return the add cache entry at \p index. Reference is invalidated upon
  /// calling allocateAddCacheEntry.
  AddPropertyCacheEntry &getAddCacheEntry(uint32_t index) {
    return addCacheEntries_[index];
  }

  /// Mark the non-weak roots owned by this RuntimeModule.
  void markRoots(RootAcceptor &acceptor, bool markLongLived);

  /// Mark the weak roots owned by this RuntimeModule.
  void markWeakRoots(WeakRootAcceptor &acceptor, bool markLongLived);

  /// Mark the weak reference to the Domain which owns this RuntimeModule.
  void markDomainRef(WeakRootAcceptor &acceptor) {
    acceptor.acceptWeak(domain_);
  }

  /// \return True if the weak root to the owning Domain is empty.
  bool isOwningDomainDead() const {
    return !domain_;
  }

  /// \return an estimate of the size of additional memory used by this
  /// RuntimeModule.
  size_t additionalMemorySize() const;

  /// \return the malloc'd bytes held by the regexp literal cache: its
  /// vector and the entries in it. Reported by the Domain separately from
  /// additionalMemorySize(), because the entries have their own snapshot
  /// nodes.
  size_t regExpCacheMemorySize() const;

#ifdef HERMES_MEMORY_INSTRUMENTATION
  /// Add native nodes and edges to heap snapshots.
  void snapshotAddNodes(GC &gc, HeapSnapshot &snap) const;
  void snapshotAddEdges(GC &gc, HeapSnapshot &snap) const;
#endif

  /// Find the cached hidden class for an object literal, if one exists.
  /// \param parent is the parent object that we expect to find.
  /// \param shapeTableIndex is the ID of an object literal shape.
  /// \return the cached hidden class.
  HiddenClass *findCachedLiteralHiddenClass(
      Runtime &runtime,
      JSObject *parent,
      uint32_t shapeTableIndex) const;

  /// Set the cached hidden class for a given shape.
  /// If shapeTableIndex == SHAPE_TABLE_CACHING_DISABLED, do nothing.
  /// \param shapeTableIndex is the ID of an object literal shape.
  /// \param clazz the hidden class to cache.
  void setCachedLiteralHiddenClass(
      Runtime &runtime,
      unsigned shapeTableIndex,
      HiddenClass *clazz);

  /// Given \p templateObjectID, retrieve the cached template object.
  /// if it doesn't exist, return a nullptr.
  JSObject *findCachedTemplateObject(uint32_t templateObjID) {
    return templateMap_.lookup(templateObjID);
  }

  /// Cache a template object in the template map using a template object ID as
  /// key.
  /// \p templateObjID is the template object ID, and it should not already
  /// exist in the map.
  /// \p templateObj is the template object that we are caching.
  void cacheTemplateObject(
      uint32_t templateObjID,
      Handle<JSObject> templateObj) {
    assert(
        templateMap_.count(templateObjID) == 0 &&
        "The template object already exists.");
    templateMap_[templateObjID] = templateObj.get();
  }

  /// After a new lazy function has been compiled, update internal RuntimeModule
  /// state with new data from the BCProvider.
  void initAfterLazyCompilation();

  /// Returns the module export for module \p modIndex.  This will be
  /// empty if that module has not yet been initialized.
  inline HermesValue getModuleExport(uint32_t modIndex) const;

  /// Attempts to set the module export for module \p modIndex to \p modExport.
  /// This may fail if \p modIndex is outside current capacity
  /// of the cache, and attempts to reallocate it fail.  If that occurs,
  /// the array size will remain unchanged.
  void setModuleExport(Runtime &runtime, uint32_t modIndex, Handle<> modExport);

  /// The \p cases pointer points the the start of the string switch
  /// table for a StringSwitchImm instruction; \p size is the size of that
  /// table. Initializes \p table, which must be the runtime table dedicated to
  /// this instruction, to map the case labels to the right (bytecode) branch
  /// offsets and to a dense case index, assigned in insertion order, which the
  /// JIT uses to index the per-body jump table of the code that is running.
  void initializeStringSwitchImmTable(
      StringSwitchDenseMap &table,
      const hbc::StringSwitchTableCase *cases,
      uint32_t size);

 private:
  /// Import the string table from the supplied module.
  void importStringIDMapMayAllocate();

  /// Initialize functionMap_, without actually creating the code blocks.
  /// They will be created lazily when needed.
  void initializeFunctionMap();

  /// Import the CommonJS module table.
  /// Set every module to uninitialized, except for the first module.
  LLVM_NODISCARD ExecutionStatus importCJSModuleTable();

  /// Map the supplied string to a given \p stringID, register it in the
  /// identifier table, and \return the symbol ID.
  /// Computes the hash of the string when it's not supplied.
  template <typename T>
  SymbolID mapStringMayAllocate(llvh::ArrayRef<T> str, StringID stringID) {
    return mapStringMayAllocate(str, stringID, hermes::hashString(str));
  }

  /// Map the supplied string to a given \p stringID, register it in the
  /// identifier table, and \return the symbol ID.
  template <typename T>
  SymbolID
  mapStringMayAllocate(llvh::ArrayRef<T> str, StringID stringID, uint32_t hash);

  /// Create a symbol from a given \p stringID, which is an index to the
  /// string table, corresponding to the entry \p entry. If \p mhash is not
  /// None, use it as the hash; otherwise compute the hash from the string
  /// contents. \return the created symbol ID.
  SymbolID createSymbolFromStringIDMayAllocate(
      StringID stringID,
      const StringTableEntry &entry,
      OptValue<uint32_t> mhash);
};

using RuntimeModuleList = llvh::simple_ilist<RuntimeModule>;

} // namespace vm
} // namespace hermes

#endif
