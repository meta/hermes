/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_DOMAIN_H
#define HERMES_VM_DOMAIN_H

#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/CopyableVector.h"
#include "hermes/VM/GCCell.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

/// A Domain is a GC-managed proxy for a set of RuntimeModules which were
/// compiled at the same time, and thus are intended to work together.
/// For example, they can provide a set of CommonJS modules.
///
/// A Domain is constructed empty, and then populated with RuntimeModules,
/// which it then keeps alive for its entire lifetime. All JSFunctions which are
/// constructed in the given Domain then keep strong GC pointers to it.
///
/// When a Domain is garbage collected, all functions that can run in the domain
/// must be unreachable. We can therefore free all RuntimeModules within the
/// domain, knowing there are no other ways to run any code dependent on the
/// Domain.
///
/// Each RuntimeModule keeps a weak reference to its owning Domain,
/// and those weak references are marked when the Domain marks its own WeakRefs.
///
/// A Domain owns a CommonJS module table which can be resolved either by slow
/// dynamic requires (based on strings to file names) xor by statically resolved
/// fast requires. The actual data for the CJS modules is stored in cjsModules_
/// and can be retrieved quickly using an index, while the string -> index table
/// used for dynamic requires provides an index into the cjsModules_ storage.
class Domain final : public GCCell {
  using Super = GCCell;
  friend void DomainBuildMeta(const GCCell *cell, Metadata::Builder &mb);

  static const VTable vt;

  /// Offsets for fields in the cjsModules_ ArrayStorage which contain
  /// information about each individual module.
  enum Offsets : uint32_t {
    /// Cache of the module.exports property, populated after require()
    /// completes. We store both module and cachedExports in order to correctly
    /// handle the case when the CJS module is still initializing and the
    /// exports value may change during module initialization.
    CachedExportsOffset,

    /// Encapsulating JSObject of the given module.
    /// Created when initialization of the CJS module begins.
    /// Contains the `exports` property, which contains the exported values.
    ModuleOffset,

    /// Index of the function in the RuntimeModule.
    /// Encoded as a HermesValue NativeUInt32.
    FunctionIndexOffset,

    /// Number of fields used by a CJS module in the ArrayStorage.
    CJSModuleSize,
  };

  // TODO(T83098051): Consider optimizing cjsModules_ for non-contiguous IDs.
  /// CJS Modules used when modules have been resolved ahead of time.
  /// Used during requireFast modules by index.
  /// Stores information on module i at entries (i * CJSModuleSize) through
  /// ((i+1) * CJSModuleSize). In this way, we avoid allocating a new heap
  /// object and keep information about a given CJS module accessible without an
  /// extra indirection.
  /// For example, requireFast(2) requires the (2 * CJSModuleSize)th element of
  /// this vector.
  /// Lazily allocated: field is nullptr until importCJSModuleTable() is called.
  GCPointer<ArrayStorage> cjsModules_;

  /// Contains the RuntimeModule corresponding to each of the CJS modules above.
  /// The n-th element in this array corresponds to the CJS module with offset
  /// (n * CJSModuleSize).
  CopyableVector<RuntimeModule *> cjsRuntimeModules_{};

  /// Map of { StringID => CJS module index }.
  /// Used when doing a slow require() call that needs to resolve a filename.
  /// The index is used to look up the actual CJSModule in cjsModules_.
  llvh::DenseMap<SymbolID, uint32_t> cjsModuleTable_{};

  /// RuntimeModules owned by this Domain.
  /// These will be freed by Runtime when marking weak roots, if the Domain is
  /// dead.
  CopyableVector<RuntimeModule *> runtimeModules_{};

  /// The require() function stub that is used when using requireFast() calls
  /// within this domain. Holds the require.context property which users can use
  /// to load new segments.
  /// Lazily allocated upon loading the first CJS modules into this domain.
  GCPointer<NativeFunction> throwingRequire_{};

  /// The ID of the CJS module that should be evaluated immediately after the
  /// first RuntimeModule has been loaded. This is set to the first CJS module
  /// of the first RuntimeModule.
  OptValue<uint32_t> cjsEntryModuleID_;

 public:
  static constexpr CellKind getCellKind() {
    return CellKind::DomainKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::DomainKind;
  }

  /// Create a Domain with no associated RuntimeModules.
  static PseudoHandle<Domain> create(Runtime &runtime);

  /// Add \p runtimeModule to the list of RuntimeModules owned by this domain.
  static void addRuntimeModule(
      Handle<Domain> self,
      Runtime &runtime,
      RuntimeModule *runtimeModule) {
    self->runtimeModules_.push_back(runtimeModule, runtime.getHeap());
  }

  /// Import the CommonJS module table from the given \p runtimeModule,
  /// ignoring modules with IDs / paths that have already been imported.
  LLVM_NODISCARD static ExecutionStatus importCJSModuleTable(
      Handle<Domain> self,
      Runtime &runtime,
      RuntimeModule *runtimeModule);

  /// \return the ID of the entry CJS module.
  /// \pre at least one RuntimeModule has been imported with
  /// importCJSModuleTable().
  uint32_t getCJSEntryModuleID() const {
    return *cjsEntryModuleID_;
  }

  /// \return the offset of the CJS module corresponding to \p filename, None on
  /// failure.
  OptValue<uint32_t> getCJSModuleOffset(SymbolID filename) const {
    assert(cjsModules_ && "CJS Modules not initialized");
    auto it = cjsModuleTable_.find(filename);
    return it != cjsModuleTable_.end() ? OptValue<uint32_t>{it->second}
                                       : llvh::None;
  }

  /// \return the offset of the CJS module with ID \p index, None if a CJS
  /// module with the given ID has not been loaded.
  OptValue<uint32_t> getCJSModuleOffset(Runtime &runtime, uint32_t id) const {
    assert(cjsModules_ && "CJS Modules not initialized");
    if (LLVM_UNLIKELY(
            id >= cjsModules_.getNonNull(runtime)->size() / CJSModuleSize)) {
      // Out of bounds.
      return llvh::None;
    }
    uint32_t offset = id * CJSModuleSize;
    if (LLVM_UNLIKELY(cjsModules_.getNonNull(runtime)
                          ->at(offset + FunctionIndexOffset)
                          .isEmpty())) {
      // The entry has not been populated yet.
      return llvh::None;
    }
    return offset;
  }

  /// \return the cached exports object for the given cjsModuleOffset.
  PseudoHandle<> getCachedExports(Runtime &runtime, uint32_t cjsModuleOffset)
      const {
    return createPseudoHandle(cjsModules_.getNonNull(runtime)->at(
        cjsModuleOffset + CachedExportsOffset));
  }

  /// \return the module object for the given cjsModuleOffset.
  PseudoHandle<JSObject> getModule(Runtime &runtime, uint32_t cjsModuleOffset)
      const {
    return createPseudoHandle(
        dyn_vmcast<JSObject>(cjsModules_.getNonNull(runtime)->at(
            cjsModuleOffset + ModuleOffset)));
  }

  /// \return the function index for the given cjsModuleOffset.
  uint32_t getFunctionIndex(Runtime &runtime, uint32_t cjsModuleOffset) const {
    return cjsModules_.getNonNull(runtime)
        ->at(cjsModuleOffset + FunctionIndexOffset)
        .getNativeUInt32();
  }

  /// \return the runtime module for the given cjsModuleOffset.
  RuntimeModule *getRuntimeModule(Runtime &runtime, uint32_t cjsModuleOffset)
      const {
    assert(cjsModuleOffset % CJSModuleSize == 0 && "Invalid cjsModuleOffset");
    return cjsRuntimeModules_[cjsModuleOffset / CJSModuleSize];
  }

  /// Set the module object for the given cjsModuleOffset.
  void setCachedExports(
      uint32_t cjsModuleOffset,
      Runtime &runtime,
      HermesValue cachedExports) {
    cjsModules_.getNonNull(runtime)->set(
        cjsModuleOffset + CachedExportsOffset,
        cachedExports,
        runtime.getHeap());
  }

  /// Set the module object for the given cjsModuleOffset.
  void setModule(uint32_t cjsModuleOffset, Runtime &runtime, Handle<> module) {
    cjsModules_.getNonNull(runtime)->set(
        cjsModuleOffset + ModuleOffset,
        module.getHermesValue(),
        runtime.getHeap());
  }

  /// \return the throwing require function with require.context bound to a
  /// context for this domain.
  PseudoHandle<NativeFunction> getThrowingRequire(Runtime &runtime) const;

 private:
  ~Domain() = default;

  /// Free all non-GC managed resources associated with the object.
  static void _finalizeImpl(GCCell *cell, GC &gc);

  /// \return the amount of non-GC memory being used by the given \p cell.
  static size_t _mallocSizeImpl(GCCell *cell);

#ifdef HERMES_MEMORY_INSTRUMENTATION
  /// Heap snapshot callbacks.
  static void _snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
  static void _snapshotAddNodesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
#endif
};

/// The context used as the "this" value for require() calls, to allow the
/// require() calls to find the Domain they must use to resolve requires, as
/// well as to find the current directory name for path resolution.
/// This must inherit from JSObject because even though the property storage is
/// pointless, it is exposed to JS, and we assume that HermesValues which are
/// objects are JSObjects.
class RequireContext final : public JSObject {
  using Super = JSObject;

  static const ObjectVTable vt;
  friend void RequireContextBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);
  friend void RequireContextSerialize(Serializer &, const GCCell *);

 public:
  static constexpr CellKind getCellKind() {
    return CellKind::RequireContextKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::RequireContextKind;
  }

  /// Create a RequireContext with domain \p domain and dirname \p dirname.
  static PseudoHandle<RequireContext> create(
      Runtime &runtime,
      Handle<Domain> domain,
      Handle<StringPrimitive> dirname);

  /// \return the domain for this require context.
  static Domain *getDomain(Runtime &runtime, RequireContext *self) {
    return self->domain_.get(runtime);
  }

  /// \return the current dirname for this require context.
  static StringPrimitive *getDirname(Runtime &runtime, RequireContext *self) {
    return self->dirname_.get(runtime);
  }

  RequireContext(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz) {}

 private:
  GCPointer<Domain> domain_;
  GCPointer<StringPrimitive> dirname_;
};

} // namespace vm
} // namespace hermes

#endif
