/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT
#include "JitHandlers.h"

#include "../JSLib/JSLibInternal.h"
#include "hermes/Support/ErrorHandling.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/Interpreter.h"
#include "hermes/VM/JIT/JitFunctionData.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSObject-inline.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/PropertyCache.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/RuntimeModule.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StackFrame.h"
#include "hermes/VM/StaticHUtils.h"
#include "hermes/VM/StringPrimitiveValueDenseMapInfo-inline.h"

#include "llvh/Support/raw_ostream.h"

#include <algorithm>

#define DEBUG_TYPE "jit"

namespace hermes::vm {
SHLegacyValue _sh_ljs_create_bytecode_closure(
    SHRuntime *shr,
    const SHLegacyValue *env,
    SHRuntimeModule *shRuntimeModule,
    uint32_t functionID) {
  Runtime &runtime = getRuntime(shr);
  auto *runtimeModule = (RuntimeModule *)shRuntimeModule;
  GCScopeMarkerRAII marker{runtime};
  return JSFunction::createWithInferredParent(
             runtime,
             runtimeModule->getDomain(runtime),
             _sh_ljs_is_undefined(*env)
                 ? Runtime::makeNullHandle<Environment>()
                 : Handle<Environment>::vmcast(toPHV(env)),
             runtimeModule->getCodeBlockMayAllocate(functionID))
      .getHermesValue();
}

SHLegacyValue _interpreter_create_generator(
    SHRuntime *shr,
    SHLegacyValue *frame,
    const SHLegacyValue *env,
    SHRuntimeModule *shRuntimeModule,
    uint32_t functionID) {
  Runtime &runtime = getRuntime(shr);
  StackFramePtr framePtr{toPHV(frame)};
  auto *runtimeModule = (RuntimeModule *)shRuntimeModule;
  CallResult<PseudoHandle<JSGeneratorObject>> res{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    res = Interpreter::createGenerator_RJS(
        runtime,
        runtimeModule,
        functionID,
        env ? Handle<Environment>::vmcast(toPHV(env))
            : Runtime::makeNullHandle<Environment>(),
        framePtr.getNativeArgs());
  }
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return res->getHermesValue();
}

SHLegacyValue _sh_ljs_get_bytecode_string(
    SHRuntime *shr,
    SHRuntimeModule *runtimeModule,
    uint32_t stringID) {
  return HermesValue::encodeStringValue(
      ((RuntimeModule *)runtimeModule)
          ->getStringPrimFromStringIDMayAllocate(stringID));
}

SHLegacyValue _sh_ljs_get_bytecode_bigint(
    SHRuntime *shr,
    SHRuntimeModule *runtimeModule,
    uint32_t bigintID) {
  Runtime &runtime = getRuntime(shr);
  CallResult<HermesValue> res{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    res = BigIntPrimitive::fromBytes(
        runtime,
        ((RuntimeModule *)runtimeModule)->getBigIntBytesFromBigIntId(bigintID));
  }
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return *res;
}

SHLegacyValue _interpreter_create_object_from_buffer(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  Runtime &runtime = getRuntime(shr);
  CallResult<PseudoHandle<>> res = Interpreter::createObjectFromBuffer(
      runtime,
      (CodeBlock *)codeBlock,
      runtime.objectPrototype,
      shapeTableIndex,
      valBufferOffset,
      ObjectAllocKind::Untyped);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return res->getHermesValue();
}

SHLegacyValue _interpreter_create_object_from_buffer_with_parent(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  Runtime &runtime = getRuntime(shr);
  auto *parentPHV = toPHV(parent);
  Handle<JSObject> parentHandle = parentPHV->isObject()
      ? Handle<JSObject>::vmcast(parentPHV)
      : parentPHV->isNull()
      ? Runtime::makeNullHandle<JSObject>()
      : Handle<JSObject>::vmcast(&runtime.objectPrototype);
  CallResult<PseudoHandle<>> res = Interpreter::createObjectFromBuffer(
      runtime,
      (CodeBlock *)codeBlock,
      parentHandle,
      shapeTableIndex,
      valBufferOffset,
      /* isTyped */ ObjectAllocKind::Untyped);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return res->getHermesValue();
}

SHLegacyValue _interpreter_create_typed_object_from_buffer(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  Runtime &runtime = getRuntime(shr);
  CallResult<PseudoHandle<>> res = Interpreter::createObjectFromBuffer(
      runtime,
      (CodeBlock *)codeBlock,
      Handle<JSObject>::dyn_vmcast(Handle<>(toPHV(parent))),
      shapeTableIndex,
      valBufferOffset,
      ObjectAllocKind::TypedEnumerable);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return res->getHermesValue();
}

SHLegacyValue _interpreter_create_typed_non_enum_object_from_buffer(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  Runtime &runtime = getRuntime(shr);
  CallResult<PseudoHandle<>> res = Interpreter::createObjectFromBuffer(
      runtime,
      (CodeBlock *)codeBlock,
      Handle<JSObject>::dyn_vmcast(Handle<>(toPHV(parent))),
      shapeTableIndex,
      valBufferOffset,
      ObjectAllocKind::TypedNonEnumerable);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return res->getHermesValue();
}

SHLegacyValue _interpreter_create_object_with_parent(
    Runtime &runtime,
    CodeBlock *codeBlock,
    PinnedHermesValue *parent,
    uint32_t shapeTableIndex) {
  auto *parentPHV = toPHV(parent);
  Handle<JSObject> parentHandle = parentPHV->isObject()
      ? Handle<JSObject>::vmcast(parentPHV)
      : parentPHV->isNull()
      ? Runtime::makeNullHandle<JSObject>()
      : Handle<JSObject>::vmcast(&runtime.objectPrototype);
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<JSObject> res;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.clazz = runtime.ignoreAllocationFailure(
      Interpreter::getHiddenClassForBuffer(
          runtime,
          codeBlock,
          parentHandle,
          shapeTableIndex,
          ObjectAllocKind::Untyped));
  return JSObject::create(runtime, parentHandle, lv.clazz).getHermesValue();
}

/// Wrapper around Interpreter::createArrayFromBuffer.
SHLegacyValue _interpreter_create_array_from_buffer(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    unsigned numElements,
    unsigned numLiterals,
    unsigned bufferIndex) {
  Runtime &runtime = getRuntime(shr);
  CallResult<PseudoHandle<>> res = [&] {
    GCScopeMarkerRAII marker{runtime};
    return Interpreter::createArrayFromBuffer(
        runtime, (CodeBlock *)codeBlock, numElements, numLiterals, bufferIndex);
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
  return res->getHermesValue();
}

/// Counterpart of _sh_ljs_create_regexp that takes the precompiled regexp
/// bytecode from the RuntimeModule cache of \p codeBlock.
SHLegacyValue _interpreter_create_regexp(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    SHSymbolID patternID,
    SHSymbolID flagsID,
    uint32_t regexpID) {
  Runtime &runtime = getRuntime(shr);
  return Interpreter::createRegExp(
             runtime,
             (CodeBlock *)codeBlock,
             SymbolID::unsafeCreate(patternID),
             SymbolID::unsafeCreate(flagsID),
             regexpID)
      .getHermesValue();
}

void _interpreter_create_class(SHRuntime *shr, SHLegacyValue *frameRegs) {
  Runtime &runtime = getRuntime(shr);
  if (LLVM_UNLIKELY(
          Interpreter::caseCreateClass(
              runtime, (PinnedHermesValue *)frameRegs) ==
          ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
}

/// Implementation of createFunctionEnvironment that takes the closure to get
/// the parentEnvironment from.
/// The native backend doesn't use createFunctionEnvironment.
SHLegacyValue _sh_ljs_create_function_environment(
    SHRuntime *shr,
    SHLegacyValue *frame,
    uint32_t size) {
  Runtime &runtime = getRuntime(shr);

  StackFramePtr framePtr{toPHV(frame)};
  struct : public Locals {
    PinnedValue<Environment> parent;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.parent = framePtr.getCalleeClosureUnsafe()->getEnvironment(runtime);
  return Environment::create(runtime, lv.parent, size);
}

void _sh_print_function_entry_exit(bool enter, const char *msg) {
  static unsigned level = 0;
  if (enter) {
    printf("%*s*** Enter FunctionID ", level * 4, "");
    ++level;
  } else {
    --level;
    printf("%*s*** Leave FunctionID ", level * 4, "");
  }
  printf("%s\n", msg);
  fflush(stdout);
}

SHLegacyValue
_sh_ljs_string_add(SHRuntime *shr, SHLegacyValue *left, SHLegacyValue *right) {
  Runtime &runtime = getRuntime(shr);

  // StringPrimitive::concat has special handling for two arguments,
  auto lhsHandle = Handle<StringPrimitive>::vmcast(toPHV(left));
  auto rhsHandle = Handle<StringPrimitive>::vmcast(toPHV(right));
  CallResult<HermesValue> result{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    result = StringPrimitive::concat(runtime, lhsHandle, rhsHandle);
  }
  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *result;
}

JSObject *_jit_new_empty_object_for_buffer(
    Runtime &runtime,
    CodeBlock *codeBlock,
    uint32_t shapeTableIndex,
    PinnedHermesValue *tmp) {
  // Get or create the HiddenClass.
  // TODO: Inline the fast path completely into the JIT emitted code once we can
  // also inline the allocation.
  CallResult<HiddenClass *> clazzRes = Interpreter::getHiddenClassForBuffer(
      runtime,
      codeBlock,
      Handle<JSObject>::vmcast(&runtime.objectPrototype),
      shapeTableIndex,
      /* isTyped */ ObjectAllocKind::Untyped);
  if (LLVM_UNLIKELY(clazzRes == ExecutionStatus::EXCEPTION))
    _sh_throw_current(&runtime);
  HiddenClass *clazz = *clazzRes;

  // Construct the object.
  *tmp = HermesValue::encodeObjectValue(clazz);
  PseudoHandle<JSObject> result = JSObject::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime.objectPrototype),
      Handle<HiddenClass>::vmcast(toPHV(tmp)));
  assert(
      runtime.getHeap().inYoungGen(result.get()) &&
      "New object is not in young gen");

  return result.get();
}

void _jit_put_by_id(
    SHRuntime *shr,
    SHJitVersionData *versionData,
    SHLegacyValue *shBase,
    SHLegacyValue *shValue,
    uint8_t cacheIdx,
    SHSymbolID symID,
    bool strictMode,
    bool tryProp) {
  // Recompilation trigger: every call to this helper is a decline of
  // the inline PutById tier of the CALLING BODY, whose version record
  // identifies it. Threshold crossings hand off to the JITContext;
  // retired bodies' events land in their own frozen records and spend
  // nothing. Runs before any raw object pointer is derived:
  // recompilation may allocate.
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold)) {
    getRuntime(shr).getJITContext().considerRecompile(getRuntime(shr), vd);
  }

  Runtime &runtime = getRuntime(shr);
  CodeBlock *curCodeBlock = vd->codeBlock;
  Handle<> value{toPHV(shValue)};
  SmallHermesValue shv = SmallHermesValue::encodeHermesValue(*value, runtime);

  if (HermesValue base = *toPHV(shBase); LLVM_LIKELY(base.isObject())) {
    auto *obj = vmcast<JSObject>(base);
    auto *cacheEntry = curCodeBlock->getWriteCacheEntry(cacheIdx);

    CompressedPointer clazzPtr{obj->getClassGCPtr()};
    // If we have a cache hit, reuse the cached offset and immediately
    // return the property.
    if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {
      JSObject::setNamedSlotValueUnsafe(
          obj, runtime, cacheEntry->getSlot(), shv);
      return;
    }

    // Now check against the AddPropertyCacheEntry to ensure we can still
    // use the cached information.
    // NOTE: Need to check resultClazz in all cases because it's a
    // weak reference that may have been freed even if the add cache is
    // valid.
    const auto &addCacheEntry =
        curCodeBlock->getRuntimeModule()->getAddCacheEntry(
            cacheEntry->getAddCacheIndex());
    if (LLVM_LIKELY(addCacheEntry.startClazz == clazzPtr) &&
        LLVM_LIKELY(addCacheEntry.resultClazz) &&
        LLVM_LIKELY(
            addCacheEntry.getParentEpoch() == runtime.getParentCacheEpoch()) &&
        LLVM_LIKELY(addCacheEntry.parent == obj->getParentGCPtr(runtime))) {
      HiddenClass *resultClazz =
          addCacheEntry.resultClazz.getNonNull(runtime, runtime.getHeap());
      JSObject::addNewOwnPropertyInSlot(
          obj, runtime, resultClazz, addCacheEntry.getSlot(), shv);
      return;
    }
  }

  ExecutionStatus status;
  {
    GCScopeMarkerRAII marker{runtime};
    status = Interpreter::putByIdSlowPath_RJS(
        runtime,
        curCodeBlock,
        toPHV(shBase),
        toPHV(shValue),
        cacheIdx,
        SymbolID::unsafeCreate(symID),
        strictMode,
        tryProp);
  }
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    _sh_throw_current(shr);
  }
}

/// Record one ByVal (Put or Get) decline's observed target shape into
/// \p vd's entry for \p siteId. \p isSupportedKind is the caller's own
/// operation's typed-array support predicate -- store or load -- so
/// taKind is only ever set to a kind the RECORDING operation itself
/// validated. Reads the target's kind as a scalar and retains no raw
/// pointer; allocates only native memory.
static void recordByValObservation(
    JitVersionData *vd,
    uint32_t siteId,
    SHLegacyValue *target,
    bool (*isSupportedKind)(CellKind)) {
  if (!vd->consumerRecords)
    vd->consumerRecords = std::make_unique<JitConsumerRecords>();
  JitByValSiteRecord &site = vd->consumerRecords->findOrCreateByValSite(siteId);
  HermesValue t = *toPHV(target);
  if (!t.isObject()) {
    if (!site.otherSeen)
      site.changed = 1;
    site.otherSeen = 1;
    return;
  }
  CellKind kind = static_cast<GCCell *>(t.getObject())->getKind();
  if (kind == CellKind::JSArrayKind) {
    if (!site.jsArraySeen)
      site.changed = 1;
    site.jsArraySeen = 1;
  } else if (isSupportedKind(kind)) {
    uint8_t k8 = (uint8_t)kind;
    if (site.taKind == JitByValSiteRecord::kTAKindNone) {
      site.taKind = k8;
      site.changed = 1;
    } else if (site.taKind != k8) {
      // Poison keeps the first kind: the site still gets (or keeps) its
      // taKind tier, and the poison flag records that no further kind
      // can ever be added, i.e. that the site cannot progress again.
      if (!site.taPoisoned)
        site.changed = 1;
      site.taPoisoned = 1;
    }
  } else {
    if (!site.otherSeen)
      site.changed = 1;
    site.otherSeen = 1;
  }
}

void _jit_put_by_val_loose(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    SHJitVersionData *versionData,
    uint32_t siteId) {
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  recordByValObservation(vd, siteId, target, isJitSupportedTypedArrayStoreKind);
  // Recompilation trigger, as in _jit_put_by_id: every call is a
  // decline of the calling body's ByVal tiers; the shared counter and
  // threshold pool ById and ByVal declines. Runs after recording (see
  // above) but before the store logic derives raw pointers.
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold)) {
    getRuntime(shr).getJITContext().considerRecompile(getRuntime(shr), vd);
  }
  _sh_ljs_put_by_val_loose_rjs(shr, target, key, value);
}

/// Strict-mode variant of _jit_put_by_val_loose; see its documentation.
void _jit_put_by_val_strict(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    SHJitVersionData *versionData,
    uint32_t siteId) {
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  recordByValObservation(vd, siteId, target, isJitSupportedTypedArrayStoreKind);
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold)) {
    getRuntime(shr).getJITContext().considerRecompile(getRuntime(shr), vd);
  }
  _sh_ljs_put_by_val_strict_rjs(shr, target, key, value);
}

/// Slow path of GetByVal, and the recording site for the ByVal load
/// tier's declines: records the observed source shape into
/// \p versionData's entry for \p siteId using the LOAD predicate, then
/// forwards to the plain SH helper and returns its value. Installed by
/// getByValImpl's indirect helper slot (JitEmitter-property.cpp) as the
/// shared slow path for the JSArray and typed-array load tiers.
SHLegacyValue _jit_get_by_val(
    SHRuntime *shr,
    SHLegacyValue *source,
    SHLegacyValue *key,
    SHJitVersionData *versionData,
    uint32_t siteId) {
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  recordByValObservation(vd, siteId, source, isJitSupportedTypedArrayLoadKind);
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold)) {
    getRuntime(shr).getJITContext().considerRecompile(getRuntime(shr), vd);
  }
  return _sh_ljs_get_by_val_rjs(shr, source, key);
}

/// Slow path of GetByIndex, and the recording site for the ByIndex load
/// tier's declines: records the observed source shape into
/// \p versionData's entry for \p siteId using the LOAD predicate, then
/// forwards to the plain SH helper and returns its value. Installed by
/// getByIndexImpl() into the site's own mutable helper slot.
SHLegacyValue _jit_get_by_index(
    SHRuntime *shr,
    SHLegacyValue *source,
    uint32_t key,
    SHJitVersionData *versionData,
    uint32_t siteId) {
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  recordByValObservation(vd, siteId, source, isJitSupportedTypedArrayLoadKind);
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold)) {
    getRuntime(shr).getJITContext().considerRecompile(getRuntime(shr), vd);
  }
  return _sh_ljs_get_by_index_rjs(shr, source, key);
}

bool isEligibleByIdShape(HiddenClass *objClass, HiddenClass *protoClass) {
  if (!protoClass)
    return !objClass->isDictionaryNoCache();
  return !objClass->isDictionary() && !protoClass->isDictionaryNoCache();
}

bool addByIdShape(JitByIdSiteRecord &site, const JitByIdShapeEntry &ne) {
  JitByIdShapeEntry *dead = nullptr;
  for (auto &e : site.entries) {
    if (!e.isLive()) {
      // A body may embed an emitted entry's address (a chain entry's
      // epoch), so only entries that were never emitted are reused.
      if (!dead && !e.emitted)
        dead = &e;
      continue;
    }
    if (e.sameIdentity(ne)) {
      if (ne.emitted)
        e.emitted = 1;
      return false;
    }
  }
  if (dead) {
    // Overwrites every field, including emitted and unemittable.
    *dead = ne;
    return true;
  }
  if (site.entries.size() < JitByIdSiteRecord::kMaxEntries) {
    site.entries.push_back(ne);
    assert(
        site.entries.capacity() == JitByIdSiteRecord::kMaxEntries &&
        "ById entries must stay in the record's inline storage");
    return true;
  }
  if (!site.megamorphic) {
    site.megamorphic = 1;
    return true;
  }
  return false;
}

/// Fold the shape the single-shape tier would specialize for
/// \p cacheEntry into \p site, if eligible and absent (subject to the
/// cap).
static void addCacheShape(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    ReadPropertyCacheEntry *cacheEntry) {
  HiddenClass *clazz = cacheEntry->clazz.get(runtime, runtime.getHeap());
  HiddenClass *neg = cacheEntry->negMatchClazz.get(runtime, runtime.getHeap());
  if (!clazz)
    return;
  if (!neg) {
    if (isEligibleByIdShape(clazz, nullptr))
      addByIdShape(
          site,
          JitByIdShapeEntry::makeOwn(
              cacheEntry->clazz.getNoBarrierUnsafe(),
              cacheEntry->getSlot(),
              false));
  } else if (isEligibleByIdShape(neg, clazz)) {
    addByIdShape(
        site,
        JitByIdShapeEntry::makeProto(
            cacheEntry->negMatchClazz.getNoBarrierUnsafe(),
            cacheEntry->clazz.getNoBarrierUnsafe(),
            cacheEntry->getSlot(),
            false));
  }
}

/// \return the position of \p kind in the poly chain's layout.
static unsigned polyEmitRank(JitByIdEntryKind kind) {
  return kind == JitByIdEntryKind::Own  ? 0
      : kind == JitByIdEntryKind::Chain ? 1
                                        : 2;
}

bool prepareByIdPolyChain(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    ReadPropertyCacheEntry *cacheEntry,
    llvh::function_ref<uint16_t(HiddenClass *)> lazyId,
    llvh::SmallVectorImpl<JitByIdPolyEntry> &out) {
  assert(out.empty() && "prepareByIdPolyChain() appends to an empty list");
  bool anyLive = false;
  for (const auto &e : site.entries)
    anyLive |= e.isLive();
  if (!anyLive)
    return false;
  if (cacheEntry->numGoodChanges == 1)
    addCacheShape(runtime, site, cacheEntry);
  bool anyChain = false;
  for (auto &e : site.entries) {
    if (!e.isLive())
      continue;
    bool isProto = e.kind == JitByIdEntryKind::Proto;
    bool isChain = e.kind == JitByIdEntryKind::Chain;
    // A chain entry needs a lazy ID for its receiver class only.
    uint16_t objId = lazyId(e.objClass.get(runtime, runtime.getHeap()));
    uint16_t protoId = 0;
    if (objId && isProto)
      protoId = lazyId(e.protoClass.get(runtime, runtime.getHeap()));
    if (!objId || (isProto && !protoId)) {
      e.unemittable = 1;
      continue;
    }
    e.emitted = 1;
    anyChain |= isChain;
    out.push_back(
        {objId,
         protoId,
         e.slot,
         e.kind,
         e.depth,
         isChain ? &e.epoch : nullptr});
  }
  if (out.empty())
    return false;
  // Own entries, then chain entries, then the prototype block: chain
  // entries leave the prototype block's state untouched.
  std::stable_sort(
      out.begin(),
      out.end(),
      [](const JitByIdPolyEntry &a, const JitByIdPolyEntry &b) {
        return polyEmitRank(a.kind) < polyEmitRank(b.kind);
      });
  site.chainEmitted = 1;
  site.chainEntryEmitted = anyChain ? 1 : 0;
  return true;
}

/// \return true if lookups through an object of class \p clazz are not
/// described by a class that only updateClass changes (spec: "The
/// validity argument", 6 and 7), so a chain path must not contain it.
static bool isChainExcluded(const HiddenClass *clazz) {
  return clazz->isDictionary() || clazz->isLazyObject() ||
      clazz->isHostObject() || clazz->isProxyObject();
}

/// Look \p name up in \p clazz without allocating: in the property map if
/// the class has one, else by findPropertyNoAlloc's walk of the
/// transition chain. \return true and fill \p desc if found.
static bool chainClassLookup(
    Runtime &runtime,
    HiddenClass *clazz,
    SymbolID name,
    NamedPropertyDescriptor &desc) {
  OptValue<bool> fast =
      HiddenClass::tryFindPropertyFast(clazz, runtime, name, desc);
  if (fast.hasValue())
    return *fast;
  llvh::Optional<NamedPropertyDescriptor> slow =
      HiddenClass::findPropertyNoAlloc(clazz, runtime, name);
  if (!slow)
    return false;
  desc = *slow;
  return true;
}

JitChainWalk jitWalkChain(Runtime &runtime, JSObject *obj, SymbolID name) {
  NoAllocScope noAlloc(runtime);
  JitChainWalk res{JitChainWalk::Status::Rejected, nullptr, 0, 0, false};
  NamedPropertyDescriptor desc;
  HiddenClass *clazz = obj->getClass(runtime);
  // The receiver must be plain too: a dictionary receiver could gain the
  // property without a class change.
  if (isChainExcluded(clazz))
    return res;
  if (chainClassLookup(runtime, clazz, name, desc)) {
    // An own property: the receiver's class alone decides, and entries
    // are keyed on it.
    res.allFlagged = true;
    return res;
  }
  bool allFlagged = true;
  JSObject *cur = obj;
  for (unsigned depth = 1; depth <= JitByIdShapeEntry::kMaxChainDepth;
       ++depth) {
    cur = cur->getParent(runtime);
    if (!cur)
      break;
    clazz = cur->getClass(runtime);
    if (isChainExcluded(clazz)) {
      allFlagged = false;
      break;
    }
    // Typed objects never change class or parent, so they count as
    // flagged (setCachedUsingEpoch skips them).
    if (!clazz->getIsCachedUsingEpoch() && !clazz->isTyped())
      allFlagged = false;
    if (!chainClassLookup(runtime, clazz, name, desc))
      continue;
    // Found here; depth 1 is the prototype entry's job.
    if (depth >= 2 && !desc.flags.accessor && !desc.flags.hostObject &&
        !desc.flags.proxyObject &&
        desc.slot <= ReadPropertyCacheEntry::kMaxSlot) {
      res.status = JitChainWalk::Status::Found;
      res.holder = cur;
      res.slot = desc.slot;
      res.depth = (uint8_t)depth;
    }
    break;
  }
  res.allFlagged = allFlagged;
  return res;
}

/// \return String.prototype's current hidden class. A raw pointer, valid
/// only until the next GC point. Never allocates.
static HiddenClass *stringProtoClass(Runtime &runtime) {
  return vmcast<JSObject>(runtime.stringPrototype.getHermesValue())
      ->getClass(runtime);
}

JitByIdStringTier prepareByIdString(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    SymbolID name,
    llvh::function_ref<uint16_t(HiddenClass *)> lazyId,
    SlotIndex &slot) {
  site.stringProtoId = 0;
  site.stringProtoCount = 0;
  // 1. The only own property of a primitive string a GetById can name
  // (Lowering.cpp:240-256 turns index names into numbers).
  if (name == Predefined::getSymbolID(Predefined::length)) {
    site.stringTier = JitByIdStringTier::Length;
    return site.stringTier;
  }
  // 2. The only GC point: the class is read immediately before it, and
  // no raw pointer is held across it.
  uint16_t id = lazyId(stringProtoClass(runtime));
  // 3. Re-read the class; nothing below allocates.
  NoAllocScope noAlloc(runtime);
  HiddenClass *clazz = stringProtoClass(runtime);
  site.stringProtoId = id;
  NamedPropertyDescriptor desc;
  if (id == 0 || !isEligibleByIdShape(clazz, nullptr)) {
    site.stringTier = JitByIdStringTier::Ineligible;
  } else if (!chainClassLookup(runtime, clazz, name, desc)) {
    site.stringTier = JitByIdStringTier::Absent;
    site.stringProtoCount = clazz->getNumProperties();
  } else if (desc.flags.accessor) {
    site.stringTier = JitByIdStringTier::Ineligible;
  } else {
    site.stringTier = JitByIdStringTier::Method;
    slot = desc.slot;
  }
  return site.stringTier;
}

bool byIdStringCanProgress(Runtime &runtime, const JitByIdSiteRecord &site) {
  if (!site.sawString)
    return false;
  if (site.stringTier == JitByIdStringTier::NotPrepared)
    return true;
  if (site.stringTier == JitByIdStringTier::Length)
    return false;
  NoAllocScope noAlloc(runtime);
  const HiddenClass *clazz = stringProtoClass(runtime);
  // Reads the class's ID without assigning one: 0 if it has none.
  if (clazz->getLazyJITId() != site.stringProtoId)
    return true;
  // A cacheable dictionary adds properties in place, keeping its class
  // (HiddenClass.cpp:455-482); while it stays current its count only
  // grows, so a different count means an addition.
  return site.stringTier == JitByIdStringTier::Absent &&
      clazz->isDictionary() && !clazz->isDictionaryNoCache() &&
      clazz->getNumProperties() != site.stringProtoCount;
}

/// Step 3 of the recording helper: record what \p shEntry now describes
/// for the object in \p source. Allocates nothing on the GC heap.
/// \return true if the read cache described the receiver as an own or
///   prototype hit whose shape was offered to addByIdShape, or if this is
///   the site's first string receiver (nothing to walk either way).
static bool recordByIdObservation(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    const SHLegacyValue *source,
    SHReadPropertyCacheEntry *shEntry) {
  NoAllocScope noAlloc(runtime);
  if (!site.seen) {
    site.seen = 1;
    site.changed = 1;
  }
  HermesValue src = *toPHV(source);
  // A string receiver marks the site once; later ones change nothing
  // (spec: "Recording"; flags mark transitions, not repeats).
  if (src.isString() && !site.sawString) {
    site.sawString = 1;
    site.changed = 1;
    return true;
  }
  if (!src.isObject())
    return false;
  auto *entry = reinterpret_cast<ReadPropertyCacheEntry *>(shEntry);
  JSObject *obj = vmcast<JSObject>(src);
  CompressedPointer now = obj->getClassGCPtr();
  CompressedPointer clazz = entry->clazz.getNoBarrierUnsafe();
  bool changed;
  if (clazz && clazz == now) {
    // Own hit. Takes precedence over negMatchClazz, which the own fill
    // does not clear (StaticH.cpp, _sh_ljs_get_by_id_rjs).
    if (!isEligibleByIdShape(obj->getClass(runtime), nullptr))
      return false;
    changed = addByIdShape(
        site, JitByIdShapeEntry::makeOwn(now, entry->getSlot(), false));
  } else if (now && entry->negMatchClazz.getNoBarrierUnsafe() == now) {
    JSObject *parent = obj->getParent(runtime);
    if (!clazz || !parent ||
        CompressedPointer(parent->getClassGCPtr()) != clazz)
      return false;
    if (!isEligibleByIdShape(obj->getClass(runtime), parent->getClass(runtime)))
      return false;
    changed = addByIdShape(
        site,
        JitByIdShapeEntry::makeProto(now, clazz, entry->getSlot(), false));
  } else {
    return false;
  }
  if (changed)
    site.changed = 1;
  return true;
}

/// \return true if \p e is a chain entry for receiver class \p clazz that
/// a walk at epoch \p now may refresh: live, its epoch stale, and not
/// marked as failing at \p now.
static bool isChainCandidate(
    const JitByIdShapeEntry &e,
    CompressedPointer clazz,
    uint64_t now) {
  return e.kind == JitByIdEntryKind::Chain && e.isLive() &&
      e.objClass == clazz && e.epoch != now && e.refreshFailedAt != now;
}

/// \return true if \p site holds a chain candidate for \p clazz at \p now.
static bool hasChainCandidate(
    const JitByIdSiteRecord &site,
    CompressedPointer clazz,
    uint64_t now) {
  for (const auto &e : site.entries)
    if (isChainCandidate(e, clazz, now))
      return true;
  return false;
}

/// \return true if a new entry would be stored rather than overflow:
/// fewer than kMaxEntries entries, or a dead one that was never emitted.
static bool byIdSiteHasRoom(const JitByIdSiteRecord &site) {
  if (site.entries.size() < JitByIdSiteRecord::kMaxEntries)
    return true;
  for (const auto &e : site.entries)
    if (!e.isLive() && !e.emitted)
      return true;
  return false;
}

/// Flag every unflagged, untyped object from \p recv's prototype up
/// \p depth levels, as JSObject::tryCacheAddProperty does.
/// Flagging changes only class flags (HiddenClass::markAsCachedUsingEpoch),
/// cannot run JS and cannot throw: the objects on a walked path are not
/// lazy, so setCachedUsingEpoch never materializes. It can allocate and
/// collect.
static void
flagChainPath(Runtime &runtime, Handle<JSObject> recv, uint8_t depth) {
  struct : public Locals {
    PinnedValue<JSObject> cur;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.cur = recv->getParent(runtime);
  for (uint8_t i = 0; i < depth && lv.cur.get() != nullptr; ++i) {
    if (!lv.cur->isCachedUsingEpoch(runtime))
      JSObject::setCachedUsingEpoch(lv.cur, runtime);
    lv.cur = lv.cur->getParent(runtime);
  }
}

/// The fast refresh (spec: "Recording and refresh"), before the forwarded
/// lookup. Allocates nothing. If a stale chain entry for the receiver's
/// class names the (depth, slot) a fully flagged walk finds, set its
/// epoch. \return true, with the holder's value in \p out, only if that
/// entry is emitted: the call is then complete. An unemitted entry is
/// refreshed but the call continues, so its miss still counts toward
/// the recompile that emits it.
static bool chainFastRefresh(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    const SHLegacyValue *source,
    SymbolID name,
    SHLegacyValue &out) {
  NoAllocScope noAlloc(runtime);
  HermesValue src = *toPHV(source);
  if (!src.isObject())
    return false;
  JSObject *obj = vmcast<JSObject>(src);
  CompressedPointer clazz = obj->getClassGCPtr();
  uint64_t now = runtime.getFullParentCacheEpoch();
  if (!hasChainCandidate(site, clazz, now))
    return false;
  JitChainWalk w = jitWalkChain(runtime, obj, name);
  if (w.status != JitChainWalk::Status::Found || !w.allFlagged)
    return false;
  for (auto &e : site.entries) {
    if (!isChainCandidate(e, clazz, now) || e.depth != w.depth ||
        e.slot != w.slot)
      continue;
    e.epoch = now;
    runtime.getJITContext().incrementCounter(JitCounter::NumByIdChainRefreshes);
    if (!e.emitted)
      return false;
    out = JSObject::getNamedSlotValueUnsafe(w.holder, runtime, w.slot)
              .unboxToHV(runtime);
    return true;
  }
  return false;
}

/// After the forwarded lookup: walk from the rooted receiver \p recv and
/// update \p site's chain entries (spec: "Recording and refresh",
/// revalidation and step 4). Flagging the path can allocate and collect;
/// only the walk's scalars cross it, and the receiver's class is re-read
/// afterwards. Never runs JS and never throws. Revalidation never sets
/// `changed`.
/// \param record whether a new (depth, slot) may be recorded; false for
///   _jit_get_by_id_refresh.
static void chainAfterForward(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    Handle<JSObject> recv,
    SymbolID name,
    bool record) {
  JitChainWalk w = jitWalkChain(runtime, *recv, name);
  bool found = w.status == JitChainWalk::Status::Found;
  uint64_t now = runtime.getFullParentCacheEpoch();
  CompressedPointer clazz = recv->getClassGCPtr();
  // The entry with the walk's identity, if any. Entries never move.
  JitByIdShapeEntry *same = nullptr;
  for (auto &e : site.entries) {
    if (e.kind != JitByIdEntryKind::Chain || !e.isLive() || e.objClass != clazz)
      continue;
    if (found && e.depth == w.depth && e.slot == w.slot) {
      same = &e;
      continue;
    }
    // A different verdict that depends only on flagged objects cannot
    // change without a bump: skip this entry until the epoch moves.
    if (w.allFlagged && isChainCandidate(e, clazz, now))
      e.refreshFailedAt = now;
  }
  if (!found)
    return;
  uint8_t depth = w.depth;
  uint16_t slot = (uint16_t)w.slot;
  if (same) {
    if (same->epoch == now)
      return;
    // Revalidation: flag the path so that any change to it bumps the
    // epoch, then adopt the epoch the walk ran at if nothing bumped it.
    // `same` stays valid: entries never move, and its class is the
    // rooted receiver's, so the GC cannot clear it.
    flagChainPath(runtime, recv, depth);
    if (runtime.getFullParentCacheEpoch() == now) {
      same->epoch = now;
      runtime.getJITContext().incrementCounter(
          JitCounter::NumByIdChainRefreshes);
    }
    return;
  }
  if (!record)
    return;
  if (!byIdSiteHasRoom(site)) {
    // Sets megamorphic (and changed) once; nothing is stored, so nothing
    // needs flagging.
    if (addByIdShape(
            site, JitByIdShapeEntry::makeChain(clazz, depth, slot, now)))
      site.changed = 1;
    return;
  }
  flagChainPath(runtime, recv, depth);
  if (runtime.getFullParentCacheEpoch() != now)
    return;
  if (addByIdShape(
          site,
          JitByIdShapeEntry::makeChain(
              recv->getClassGCPtr(), depth, slot, now)))
    site.changed = 1;
}

LLVM_ATTRIBUTE_NOINLINE
SHLegacyValue _jit_get_by_id(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry,
    SHJitVersionData *versionData,
    SHJitByIdSiteRecord *siteRecord) {
  Runtime &runtime = getRuntime(shr);
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  // vd owns the record, and a version record is never freed while its
  // body can run (retirement keeps it), so the pointer stays valid
  // across step 2 even if a getter retires this body.
  auto *site = reinterpret_cast<JitByIdSiteRecord *>(siteRecord);
  SymbolID name = SymbolID::unsafeCreate(symID);
  JitFunctionData *jd = vd->codeBlock->getJitData();

  // Step 0: staleness. A retired body or a demoted slot records and
  // refreshes nothing.
  // Step 1: fast refresh; may complete the call.
  if (vd == jd->current.get() && site->helper == (void *)_jit_get_by_id) {
    SHLegacyValue hv;
    if (chainFastRefresh(runtime, *site, source, name, hv))
      return hv;
  }

  // Step 2: forward. A throw longjmps past this frame, skipping
  // destructors, so no LocalsRAII may be registered across the call: a
  // stale one would leave runtime.vmLocals pointing at a dead frame.
  SHLegacyValue res = _sh_ljs_get_by_id_rjs(shr, source, symID, propCacheEntry);
  // Root the result at once (nothing allocates between the return and
  // here); steps 4 and 5 can flag, compile and collect.
  struct : public Locals {
    PinnedValue<> result;
    PinnedValue<JSObject> recv;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.result = HermesValue::fromRaw(res.raw);

  // Step 3: staleness again (a getter run above may have retired this
  // body, or a demotion pass flipped the site), then harvest.
  if (vd == jd->current.get() && site->helper == (void *)_jit_get_by_id) {
    bool harvested =
        recordByIdObservation(runtime, *site, source, propCacheEntry);
    // Step 4: nothing harvested: walk for a chain entry.
    HermesValue src = *toPHV(source);
    if (!harvested && src.isObject()) {
      lv.recv = vmcast<JSObject>(src);
      chainAfterForward(runtime, *site, lv.recv, name, true);
    }
  }

  // Step 5: count; a crossing may recompile (and collect).
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold))
    runtime.getJITContext().considerRecompile(runtime, vd);
  return lv.result.getHermesValue();
}

LLVM_ATTRIBUTE_NOINLINE
SHLegacyValue _jit_get_by_id_refresh(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry,
    SHJitVersionData *versionData,
    SHJitByIdSiteRecord *siteRecord) {
  Runtime &runtime = getRuntime(shr);
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  auto *site = reinterpret_cast<JitByIdSiteRecord *>(siteRecord);
  SymbolID name = SymbolID::unsafeCreate(symID);
  JitFunctionData *jd = vd->codeBlock->getJitData();
  // Retirement flips this slot to the plain helper, so a retired body
  // gets here only for a call already in flight; it only forwards.
  if (vd != jd->current.get())
    return _sh_ljs_get_by_id_rjs(shr, source, symID, propCacheEntry);
  {
    SHLegacyValue hv;
    if (chainFastRefresh(runtime, *site, source, name, hv))
      return hv;
  }
  // Same longjmp rule as _jit_get_by_id.
  SHLegacyValue res = _sh_ljs_get_by_id_rjs(shr, source, symID, propCacheEntry);
  struct : public Locals {
    PinnedValue<> result;
    PinnedValue<JSObject> recv;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.result = HermesValue::fromRaw(res.raw);
  HermesValue src = *toPHV(source);
  if (vd == jd->current.get() && src.isObject()) {
    lv.recv = vmcast<JSObject>(src);
    if (hasChainCandidate(
            *site, lv.recv->getClassGCPtr(), runtime.getFullParentCacheEpoch()))
      chainAfterForward(runtime, *site, lv.recv, name, false);
  }
  return lv.result.getHermesValue();
}

#ifdef HERMESVM_PROFILER_BB
void _interpreter_register_bb_execution(SHRuntime *shr, uint16_t pointIndex) {
  Runtime &runtime = getRuntime(shr);
  CodeBlock *codeBlock = runtime.getCurrentFrame().getCalleeCodeBlock();
  runtime.getBasicBlockExecutionInfo().executeBlock(codeBlock, pointIndex);
}
#endif

HermesValue
_jit_direct_eval(Runtime &runtime, PinnedHermesValue *text, bool strictCaller) {
  if (!text->isString()) {
    return *text;
  }

  CallResult<HermesValue> cr{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII gcMarker{runtime};
    cr = vm::directEval(
        runtime,
        Handle<StringPrimitive>::vmcast(text),
        strictCaller,
        nullptr,
        false);
  }
  if (cr == ExecutionStatus::EXCEPTION)
    _sh_throw_current(&runtime);

  return *cr;
}

void _sh_throw_invalid_construct(SHRuntime *shr) {
  Runtime &runtime = getRuntime(shr);
  (void)runtime.raiseTypeError("Function is not a constructor");
  _sh_throw_current(shr);
}
void _sh_throw_invalid_call(SHRuntime *shr) {
  Runtime &runtime = getRuntime(shr);
  (void)runtime.raiseTypeError("Class constructor invoked without new");
  _sh_throw_current(shr);
}

void *_jit_find_catch_target(
    SHRuntime *shr,
    SHCodeBlock *shCodeBlock,
    SHLegacyValue *frame,
    SHJmpBuf *jmpBuf,
    SHLocals *savedLocals,
    int32_t *addressTable) {
  Runtime &runtime = getRuntime(shr);
  CodeBlock *codeBlock = (CodeBlock *)shCodeBlock;
  if (isUncatchableError(runtime.getThrownValue())) {
    _sh_end_try(shr, jmpBuf);
    _sh_throw_current(shr);
  }
  // Find the IP. Either the exception was thrown from the current JS frame,
  // in which case it's in the Runtime currentIP slot, or it was thrown by a
  // callee, in which case it's in the register stack's SavedIP slot.
  const inst::Inst *ip;
  if (frame == runtime.getCurrentFrame().ptr()) {
    ip = runtime.getCurrentIP();
  } else {
    // Not the same frame. Load from the SavedIP StackFrameLayout slot.
    // This is valid because the register stack hasn't been reset by
    // _sh_catch_no_pop yet.
    uint32_t nextFrameOffset =
        (codeBlock->getFrameSize() +
         StackFrameLayout::CalleeExtraRegistersAtStart);
    StackFramePtr nextFrame{toPHV(frame) + nextFrameOffset};
    ip = nextFrame.getSavedIP();
  }
  // Look up the offset in the exception table.
  auto offset = codeBlock->getOffsetOf(ip);
  auto excTable =
      codeBlock->getRuntimeModule()->getBytecode()->getExceptionTable(
          codeBlock->getFunctionID());
  for (unsigned i = 0, e = excTable.size(); i < e; ++i) {
    if (excTable[i].start <= offset && offset < excTable[i].end) {
      _sh_catch_no_pop(
          shr,
          savedLocals,
          frame,
          codeBlock->getFrameSize() + hbc::StackFrameLayout::FirstLocal);
      return (void *)((char *)addressTable + addressTable[i]);
    }
  }
  _sh_end_try(shr, jmpBuf);
  _sh_throw_current(shr);
}

void _jit_throw_non_object_call(SHRuntime *shr) {
  {
    Runtime &runtime = getRuntime(shr);
    // The call target is in the register stack's callee slot.
    auto *callTarget =
        &StackFramePtr(runtime.getStackPointer()).getCalleeClosureOrCBRef();
    assert(!callTarget->isObject() && "Must be non-object");
    (void)runtime.raiseTypeErrorForValue(
        Handle<>(callTarget), " is not a function");
  }
  _sh_throw_current(shr);
}

SHLegacyValue _jit_call_builtin(
    SHRuntime *shr,
    SHLegacyValue *frame,
    uint32_t argCount,
    uint32_t builtinMethodID) {
  auto res = [&]() {
    Runtime &runtime = getRuntime(shr);
    // Non-allocating, so it is safe to resolve the callee before the frame
    // exists.
    auto callee =
        vmcast<NativeFunction>(runtime.getBuiltinCallable(builtinMethodID));
    auto newFrame = StackFramePtr::initFrame(
        runtime.getStackPointer(),
        StackFramePtr(toPHV(frame)),
        runtime.getCurrentIP(),
        /* savedCodeBlock */ nullptr,
        /* locals */ nullptr,
        argCount,
        HermesValue::encodeObjectValue(callee),
        HermesValue::encodeUndefinedValue());
    // "thisArg" is implicitly assumed to be "undefined": the bytecode never
    // populates the slot (HBCISel::verifyCall asserts CallBuiltin's `this` is
    // a non-register-allocated LiteralUndefined), and initFrame writes only
    // the seven metadata slots, so it must be set here, the same way the
    // interpreter's implCallBuiltin does.
    newFrame.getThisArgRef() = HermesValue::encodeUndefinedValue();

    GCScopeMarkerRAII marker{runtime};
    return NativeFunction::_nativeCall(callee, runtime);
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return res->getHermesValue();
}

int64_t _jit_string_switch_imm_table_lookup(
    RuntimeModule *runtimeModule,
    uint32_t tableIndex,
    SHLegacyValue *switchValueLegacy) {
  PinnedHermesValue *switchValue = toPHV(switchValueLegacy);
  if (!switchValue->isString()) {
    // Not a string; should branch to the default case.
    return -1;
  }

  assert(
      tableIndex < runtimeModule->numStringSwitchImmTables() &&
      "String Switch index out of range.");
  StringSwitchDenseMap *table =
      &runtimeModule->getStringSwitchImmTables()[tableIndex];

  auto iter = table->find(switchValue->getString());
  if (iter == table->end()) {
    // Not found; branch to the default case.
    return -1;
  }
  return iter->second.caseIndex;
}

const char *typePredName(TypePred pred) {
  switch (pred) {
    case TypePred::IsNumber:
      return "number";
    case TypePred::IsBool:
      return "bool";
    case TypePred::NotPointer:
      return "non-pointer";
    case TypePred::BitComparable:
      return "non-pointer non-number";
    case TypePred::IsObject:
      return "object";
  }
  return "<invalid>";
}

void _jit_type_assert_failed(
    uint32_t siteIdx,
    const std::vector<TypeAssertSite> *sites) {
  const TypeAssertSite &site = (*sites)[siteIdx];
  std::string message;
  llvh::raw_string_ostream os(message);
  os << "JIT type assert failed: function " << site.codeBlock->getFunctionID()
     << "(" << site.codeBlock->getNameString() << "), bytecode offset "
     << site.bytecodeOfs << ", r" << site.frIndex << ", expected "
     << typePredName(site.pred);
  os.flush();
  hermes_fatal(message);
}

} // namespace hermes::vm

#endif // HERMESVM_JIT
