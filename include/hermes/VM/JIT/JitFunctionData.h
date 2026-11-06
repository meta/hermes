/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JIT_JITFUNCTIONDATA_H
#define HERMES_VM_JIT_JITFUNCTIONDATA_H

#include "hermes/VM/JIT/Config.h"

#if HERMESVM_JIT

#include "hermes/VM/CellKind.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/WeakRoot.h"
#include "llvh/ADT/SmallVector.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace hermes {
namespace vm {

class Runtime;
class CodeBlock;
class HiddenClass;
typedef HermesValue (*JITCompiledFunctionPtr)(Runtime *runtime);

/// True for the typed-array kinds the inline PutByVal store tier can
/// specialize on. Uint8Clamped (round-half-to-even), Float16, and the
/// BigInt kinds are excluded and record as "other".
inline bool isJitSupportedTypedArrayStoreKind(CellKind kind) {
  switch (kind) {
    case CellKind::Uint8ArrayKind:
    case CellKind::Int8ArrayKind:
    case CellKind::Uint16ArrayKind:
    case CellKind::Int16ArrayKind:
    case CellKind::Uint32ArrayKind:
    case CellKind::Int32ArrayKind:
    case CellKind::Float32ArrayKind:
    case CellKind::Float64ArrayKind:
      return true;
    default:
      return false;
  }
}

/// True for the typed-array kinds the inline GetByVal load tier can
/// specialize on: the store set plus Uint8Clamped. The two sets differ
/// because the specialization each direction needs to skip is
/// different: clamping (round-half-to-even on write) is store-side
/// only, so a load can read a Uint8Clamped element with the same plain
/// tier as Uint8Array; Float16 is excluded on both sides for now, and
/// the BigInt kinds are excluded on both sides because a load would
/// have to allocate a BigInt result, which the inline tiers don't do.
inline bool isJitSupportedTypedArrayLoadKind(CellKind kind) {
  return kind == CellKind::Uint8ClampedArrayKind ||
      isJitSupportedTypedArrayStoreKind(kind);
}

/// One PutByVal site's observed shape and what the owning body emitted
/// for it. Observed fields are written by the recording helper alone --
/// no emitted tier instruments itself, so a hit is silent; emitted
/// fields are filled during emission. See the 2026-09-09 monotone
/// emission spec.
struct JitByValSiteRecord {
  /// Sentinel for taKind/specializedTAKind: no kind recorded/emitted.
  static constexpr uint8_t kTAKindNone = 0xff;

  /// Bytecode offset of the PutByVal instruction: unique within the
  /// function, stable across versions.
  uint32_t siteId;
  /// JSArray traffic observed here. Selects no tier -- the JSArray tier
  /// is a static prior emitted everywhere it is possible at all -- but
  /// it feeds diagnostics and the demotion stability rule.
  uint8_t jsArraySeen = 0;
  /// A non-JSArray, non-supported-typed-array target declined here.
  uint8_t otherSeen = 0;
  /// The FIRST supported typed-array kind observed here, as a CellKind
  /// value, or kTAKindNone. Never un-learned and never replaced: a
  /// second kind sets taPoisoned instead.
  uint8_t taKind = kTAKindNone;
  /// The typed-array kind the owning body emitted a tier for.
  uint8_t specializedTAKind = kTAKindNone;
  /// A supported kind DIFFERENT from taKind was observed. taKind keeps
  /// the first kind permanently; a poisoned site still gets its first
  /// kind's tier, and poison marks that no further kind can be added.
  uint8_t taPoisoned = 0;
  /// Set by recordByValObservation when a field actually transitions;
  /// cleared by the demotion pass each threshold crossing.
  uint8_t changed = 0;
  /// Consecutive crossings with no transition and nothing actionable;
  /// at kDemotionStableCrossings the site's helper slot is flipped.
  uint8_t unchangedCrossings = 0;
  /// The helper this site's emitted call goes through, as per-site
  /// MUTABLE state: initialized by the emitter to the recording
  /// _jit_put_by_val_{loose,strict}, flipped to the matching plain
  /// _sh_ljs helper on demotion. The emitted call is
  /// `movabs xScratch, &helper; call [xScratch]`. Never flipped back.
  void *helper = nullptr;
};

/// What a JitByIdShapeEntry describes. Set when the entry is recorded and
/// never changed, so a cleared weak root never changes what an entry
/// means (DESIGN-PRINCIPLES.md: encode kinds explicitly, not by nullness).
enum class JitByIdEntryKind : uint8_t {
  /// A data property of the receiver itself.
  Own,
  /// A data property of the receiver's immediate prototype, whose class
  /// is protoClass.
  Proto,
  /// A data property `depth` prototypes up (2..kMaxChainDepth), valid
  /// while the runtime's full parent cache epoch equals `epoch`. See the
  /// 2026-10-24 GetById chain spec.
  Chain,
};

/// One observed GetById shape: a receiver class, what kind of lookup
/// answers it, and the slot to load. See the 2026-10-14 polymorphic
/// GetById spec, "Records", and the 2026-10-24 chain spec, "Records".
struct JitByIdShapeEntry {
  /// The deepest prototype, counted from the receiver, that a chain entry
  /// may reach.
  static constexpr uint8_t kMaxChainDepth = 8;

  /// The receiver's hidden class.
  WeakRoot<HiddenClass> objClass{nullptr};
  /// Proto only: the immediate prototype's hidden class.
  WeakRoot<HiddenClass> protoClass{nullptr};
  /// Chain only: the full parent cache epoch at which a walk last
  /// validated this entry. Emitted code reads it through its address,
  /// and the helpers refresh it in place.
  uint64_t epoch = 0;
  /// Chain only: the epoch at which revalidating this entry last failed
  /// for a semantic reason; refreshes skip the entry until the epoch
  /// moves. Reset on carry-forward.
  uint64_t refreshFailedAt = 0;
  /// Slot in the receiver (Own), the prototype (Proto) or the object
  /// `depth` levels up (Chain).
  uint16_t slot = 0;
  /// Chain only: how many prototypes up the property lives,
  /// 2..kMaxChainDepth.
  uint8_t depth = 0;
  /// Set when recorded, never changed.
  JitByIdEntryKind kind = JitByIdEntryKind::Own;
  /// The owning body emitted an inline check for this entry. Carried
  /// forward: emitted classes are pinned by their lazy JIT IDs and every
  /// later version emits the entry again.
  uint8_t emitted = 0;
  /// A compile could not emit this entry (no lazy JIT ID). Never
  /// progress; reset on carry-forward.
  uint8_t unemittable = 0;

  /// Default: an empty, dead entry.
  JitByIdShapeEntry() = default;
  /// Copy every field. Spelled out because CompressedPointer deletes its
  /// copy assignment (CompressedPointer.h), so WeakRoot members make the
  /// implicit operations ill-formed; copying a weak root needs no
  /// barrier (it is "just moved around", WeakRoot.h).
  JitByIdShapeEntry(const JitByIdShapeEntry &other) {
    *this = other;
  }
  /// See the copy constructor.
  JitByIdShapeEntry &operator=(const JitByIdShapeEntry &other) {
    objClass = other.objClass.getNoBarrierUnsafe();
    protoClass = other.protoClass.getNoBarrierUnsafe();
    epoch = other.epoch;
    refreshFailedAt = other.refreshFailedAt;
    slot = other.slot;
    depth = other.depth;
    kind = other.kind;
    emitted = other.emitted;
    unemittable = other.unemittable;
    return *this;
  }

  /// \return an own entry for receiver class \p objClass.
  static JitByIdShapeEntry
  makeOwn(CompressedPointer objClass, uint16_t slot, bool emitted);
  /// \return a prototype entry for receiver class \p objClass whose
  /// immediate prototype has class \p protoClass.
  static JitByIdShapeEntry makeProto(
      CompressedPointer objClass,
      CompressedPointer protoClass,
      uint16_t slot,
      bool emitted);
  /// \return an unemitted chain entry for receiver class \p objClass,
  /// validated at \p epoch.
  static JitByIdShapeEntry makeChain(
      CompressedPointer objClass,
      uint8_t depth,
      uint16_t slot,
      uint64_t epoch);

  /// \return true if every class this entry needs is still alive.
  bool isLive() const {
    return objClass && (kind != JitByIdEntryKind::Proto || protoClass);
  }

  /// \return true if \p other describes the same shape: same kind,
  /// receiver class and slot, plus the prototype class (Proto) or the
  /// depth (Chain). Epochs are not part of identity: receiver class,
  /// depth and slot, validated at an epoch, determine the answer.
  bool sameIdentity(const JitByIdShapeEntry &other) const;
};

inline JitByIdShapeEntry JitByIdShapeEntry::makeOwn(
    CompressedPointer objClass,
    uint16_t slot,
    bool emitted) {
  JitByIdShapeEntry e;
  e.objClass = objClass;
  e.slot = slot;
  e.kind = JitByIdEntryKind::Own;
  e.emitted = emitted;
  return e;
}

inline JitByIdShapeEntry JitByIdShapeEntry::makeProto(
    CompressedPointer objClass,
    CompressedPointer protoClass,
    uint16_t slot,
    bool emitted) {
  JitByIdShapeEntry e;
  e.objClass = objClass;
  e.protoClass = protoClass;
  e.slot = slot;
  e.kind = JitByIdEntryKind::Proto;
  e.emitted = emitted;
  return e;
}

inline JitByIdShapeEntry JitByIdShapeEntry::makeChain(
    CompressedPointer objClass,
    uint8_t depth,
    uint16_t slot,
    uint64_t epoch) {
  JitByIdShapeEntry e;
  e.objClass = objClass;
  e.epoch = epoch;
  e.slot = slot;
  e.depth = depth;
  e.kind = JitByIdEntryKind::Chain;
  return e;
}

inline bool JitByIdShapeEntry::sameIdentity(
    const JitByIdShapeEntry &other) const {
  if (kind != other.kind || objClass != other.objClass.getNoBarrierUnsafe() ||
      slot != other.slot)
    return false;
  if (kind == JitByIdEntryKind::Proto)
    return protoClass == other.protoClass.getNoBarrierUnsafe();
  if (kind == JitByIdEntryKind::Chain)
    return depth == other.depth;
  return true;
}

/// What a body prepared for string receivers at a GetById site.
enum class JitByIdStringTier : uint8_t {
  /// The body did not prepare the site: it was compiled before the site
  /// saw a string.
  NotPrepared,
  /// Prepared, nothing emitted: the name is not an own property of
  /// String.prototype's class, which is eligible and has a lazy ID.
  Absent,
  /// Prepared, nothing emitted: the name is an accessor, the class is not
  /// eligible, or no lazy ID could be assigned.
  Ineligible,
  /// The length block.
  Length,
  /// A method block guarded by stringProtoId.
  Method,
};

/// One GetById site's observed shapes and what the owning body emitted
/// for it. _jit_get_by_id records observed shapes and sets seen, changed
/// and sawString. It and, once the site is demoted, _jit_get_by_id_refresh
/// also write chain entries' epoch and refreshFailedAt. The emitter
/// also adds entries: the read cache's current shape, and the shapes
/// it emits, marked emitted. Both add through addByIdShape, so either
/// can set megamorphic.
struct JitByIdSiteRecord {
  /// Maximum number of shapes kept per site.
  static constexpr unsigned kMaxEntries = 4;

  /// Bytecode offset of the GetById instruction.
  uint32_t siteId;
  /// Observed shapes, at most kMaxEntries. They live in the inline
  /// storage, which never reallocates because addByIdShape never exceeds
  /// kMaxEntries, and are never erased, reordered or (once emitted)
  /// overwritten: emitted code embeds the address of a chain entry's
  /// epoch. Dead entries that were never emitted are reused.
  llvh::SmallVector<JitByIdShapeEntry, kMaxEntries> entries;
  /// The recording helper completed at least one call here. The
  /// demotion rule's "observed" test.
  uint8_t seen = 0;
  /// A distinct live shape arrived while entries was full.
  uint8_t megamorphic = 0;
  /// Same meaning as JitByValSiteRecord::changed.
  uint8_t changed = 0;
  /// Same meaning as JitByValSiteRecord::unchangedCrossings.
  uint8_t unchangedCrossings = 0;
  /// The owning body emitted a polymorphic chain here. Emitted-tier
  /// field: reset on carry-forward; feeds the dump line only.
  uint8_t chainEmitted = 0;
  /// The owning body emitted at least one chain entry here. Emitted-tier
  /// field: reset on carry-forward; feeds the dump line only.
  uint8_t chainEntryEmitted = 0;
  /// At least one string receiver completed a recording call here.
  /// Observed-tier: carried forward, never cleared.
  uint8_t sawString = 0;
  /// What the owning body prepared for string receivers at this site.
  /// Emitted-tier: reset to NotPrepared on carry-forward.
  JitByIdStringTier stringTier = JitByIdStringTier::NotPrepared;
  /// Method, Absent and Ineligible only: lazy JIT ID of String.prototype's
  /// class when the owning body prepared the site; 0 if none could be
  /// assigned. Emitted-tier: reset to 0 on carry-forward.
  uint16_t stringProtoId = 0;
  /// Absent only: that class's property count when the owning body
  /// prepared the site. Emitted-tier: reset to 0 on carry-forward.
  uint32_t stringProtoCount = 0;
  /// Per-site mutable helper slot: _jit_get_by_id while recording; after
  /// demotion, _jit_get_by_id_refresh if the owning body emitted a chain
  /// entry here, else _sh_ljs_get_by_id_rjs. Retirement sets
  /// _sh_ljs_get_by_id_rjs. Never flipped back to _jit_get_by_id.
  void *helper = nullptr;
};

/// The version-local consumer feedback records, owned by (and destroyed
/// with) the JitVersionData. Each site record is its own allocation:
/// emitted code embeds the address of a record (GetById) or of its
/// helper slot (ByVal), so a record must never move once created, and
/// a vector of owning pointers keeps that while costing no heap memory
/// when empty. Lookup is a linear scan, done at emission and by the
/// ByVal recording helpers; functions have a handful of sites.
struct JitConsumerRecords {
  /// ByVal site records.
  std::vector<std::unique_ptr<JitByValSiteRecord>> byValSites;

  /// GetById site records.
  std::vector<std::unique_ptr<JitByIdSiteRecord>> byIdSites;

  /// Number of ByVal and ById sites whose helper slot still holds a
  /// recording helper. Gates the demotion pass constant-time once zero.
  /// 32-bit: site counts are bounded by bytecode size, which is 32-bit.
  uint32_t recordingSites = 0;

  /// \return the entry for \p siteId, creating it if absent.
  JitByValSiteRecord &findOrCreateByValSite(uint32_t siteId) {
    for (auto &s : byValSites)
      if (s->siteId == siteId)
        return *s;
    byValSites.push_back(std::make_unique<JitByValSiteRecord>());
    byValSites.back()->siteId = siteId;
    return *byValSites.back();
  }

  /// \return the ById record for \p siteId, creating it if absent.
  JitByIdSiteRecord &findOrCreateByIdSite(uint32_t siteId) {
    for (auto &s : byIdSites)
      if (s->siteId == siteId)
        return *s;
    byIdSites.push_back(std::make_unique<JitByIdSiteRecord>());
    byIdSites.back()->siteId = siteId;
    return *byIdSites.back();
  }
};

/// Everything known about ONE compiled body: its code, and the feedback
/// recorded about that body's own execution. A version IS {its code, its
/// observations} -- observations made against one body do not describe
/// another, because a kind that reaches version 1's slow-path helper may
/// be handled inline by version 2 and never reach its helper at all. The
/// record is the recording helpers' sole identity argument: everything
/// function-level is reached through \c codeBlock. See
/// doc/superpowers/specs/2026-09-07-jit-version-data-design.md.
struct JitVersionData {
  /// The function this body was compiled from. Back-pointer used by the
  /// recording helpers to reach everything function-level.
  CodeBlock *codeBlock;
  /// This version's entry point, set at install. Null while the body is
  /// still a compilation candidate.
  JITCompiledFunctionPtr body = nullptr;
  /// This body's own helper-side decline counter. A retired body keeps
  /// counting into its own frozen record, where it influences nothing.
  uint32_t declineCount = 0;
  /// Declines this body must accumulate before its helper tail hands
  /// off to JITContext::considerRecompile. Snapshotted from the
  /// JITContext (i.e. from -Xjit-recompile-threshold) when this version
  /// is compiled, so the tail compares two fields of the one record it
  /// already holds and never walks to the CodeBlock or the JITContext.
  /// Never 0: the setter's caller clamps it up to 1.
  uint32_t declineThreshold;
  /// Number of Get/PutById sites whose inline tier this compile skipped
  /// because the property cache was cold. Derived at install as the
  /// clamped sum of coldWriteCacheIdxs and coldReadCacheIdxs below;
  /// feeds the cheap recompile trigger gate and the dump line.
  uint16_t coldByIdSites = 0;
  /// Write-cache indices whose specialization this compile skipped
  /// because the cache was cold. Written at install; consumed by
  /// considerRecompile's progress check to require that one of these
  /// specific sites -- not just some unrelated cache -- has since warmed.
  llvh::SmallVector<uint8_t, 4> coldWriteCacheIdxs;
  /// Read-cache indices whose specialization this compile skipped
  /// because the cache was cold. Same role as coldWriteCacheIdxs, for
  /// GetById sites.
  llvh::SmallVector<uint8_t, 4> coldReadCacheIdxs;
  /// Version-local consumer feedback (ByVal and GetById per-site
  /// records). Null until the first entry is created; destroyed with
  /// this record, which outlives every possible execution of the body.
  std::unique_ptr<JitConsumerRecords> consumerRecords{};

  /// \param cb the function this body is compiled from.
  /// \param declineThreshold declines before considering a recompile.
  JitVersionData(CodeBlock *cb, uint32_t declineThreshold)
      : codeBlock(cb), declineThreshold(declineThreshold) {}
};

/// Per-function JIT metadata supporting recompilation: the control state
/// that spans versions, plus the per-version records. Owned by the
/// CodeBlock. The Compiler constructor allocates it when a compile of the
/// function first starts; if that compile fails, it stays, with no
/// `current`. See
/// doc/superpowers/specs/2026-09-04-jit-recompilation-design.md and
/// doc/superpowers/specs/2026-09-07-jit-version-data-design.md.
struct JitFunctionData {
  /// Default declines of the ById helpers before the recompile check
  /// runs; the effective value is per-version and lives in
  /// JitVersionData::declineThreshold. Duplicated as a literal by the
  /// -Xjit-recompile-threshold flag and the RuntimeConfig field, which
  /// cannot include this header.
  static constexpr uint32_t kDefaultRecompileDeclineThreshold = 64;

  /// Remaining recompiles for this function. 0 disables all triggering.
  uint8_t recompileBudget;
  /// 1 after the first compile; incremented on each installed recompile.
  uint8_t version = 1;
  /// The record of the body future calls enter. Set at install, after
  /// the previous current has been retired.
  std::unique_ptr<JitVersionData> current{};
  /// Records of the previous bodies, retired by recompiles. Kept alive
  /// because a frame may still be executing a retired body: a record
  /// must outlive every possible execution of its body, and a retired
  /// body's helper calls still write into its record. The machine code
  /// itself is owned by JITContext::Impl and is never freed (see the
  /// reclamation dz issue).
  llvh::SmallVector<std::unique_ptr<JitVersionData>, 1> retired{};
  /// The candidate record of a compile in progress for this function,
  /// or null. Set by the Compiler constructor, cleared at install and
  /// by the Compiler destructor. CodeBlock::markWeakElementsInCaches
  /// traces its ById records, because a compile can collect
  /// (initHCLazyIDMayAlloc) while the candidate is not yet `current`.
  JitVersionData *compiling = nullptr;

  /// \param budget initial recompile budget (from -Xjit-max-recompiles).
  explicit JitFunctionData(uint8_t budget) : recompileBudget(budget) {}
};

} // namespace vm
} // namespace hermes

#endif // HERMESVM_JIT
#endif // HERMES_VM_JIT_JITFUNCTIONDATA_H
