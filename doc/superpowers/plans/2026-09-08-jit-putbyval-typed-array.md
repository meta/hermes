# JIT PutByVal Shape Feedback + Typed-Array Store Tier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Per-site shape feedback for PutByVal (JSArray / typed-array
kind / other), evidence-driven tier selection at recompile, and an
inline typed-array store tier — the first consumer of
`JitVersionData::consumerRecords`.

**Architecture:** A new `_jit_put_by_val_{loose,strict}` recording
helper records each site's observed target shape into per-version
records; prior-based JSArray tiers set a sticky flag on inline hits so
records are complete; recompiles emit only evidence-supported tiers,
including an exact-kind typed-array store tier; `considerRecompile`
gains a ByVal progress term.

**Tech Stack:** C++17, asmjit (x86-64), lit/FileCheck tests.

**Spec:** doc/superpowers/specs/2026-09-08-jit-putbyval-typed-array-design.md
(Codex-reviewed, all findings CLOSED). The spec is the authority; this
plan argues from it. Context: 2026-09-04-jit-recompilation-design.md,
2026-09-07-jit-version-data-design.md.

## Global Constraints

- x86-64 backend only. NO arm64 emitter changes; arm64 keeps calling
  `_sh_ljs_put_by_val_{loose,strict}_rjs` and stays dormant. The arm64
  BUILD must stay green (shared driver/headers compile there).
- Supported tier kinds, exactly: Uint8, Int8, Uint16, Int16, Uint32,
  Int32 (truncating-int path), Float32, Float64. Uint8Clamped,
  Float16, BigUint64, BigInt64 record as `otherSeen` and never
  specialize.
- Tiers are dropped only on positive evidence, never on absence of
  evidence. Sites with no evidence keep the (instrumented) JSArray
  tier where `HERMES_JIT_INLINE_SAFE_STORE` permits one.
- Helper identity travels by argument (`vd` + `siteId`); never by
  return address. Record BEFORE the trigger tail; scalars only.
- No new command-line flags; no BYTECODE_VERSION change; no
  interpreter changes.
- Runtime C++ (JitHandlers.cpp, JitCompiler.cpp) follows GC-safe
  rules: implementers touching those files invoke the
  `gc-safe-coding` skill first.
- 80-column limit, 2-space indent, doc comments on every declaration.
- Build: ASan HV64 at `cmake-build-x86jit`
  (`cmake --build cmake-build-x86jit --target hermes`). JIT lit suite:
  `LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes`.
  arm64 build check: `cmake --build cmake-build-arm64 --target hermes`.
  All commands run from the repo root
  `/home/tmikov/work/hermes-x86-jit`; never `cd`.
- Every new lit test: copyright header, `// REQUIRES: jit`,
  `// UNSUPPORTED: handle_san` unless the test is cheap, threshold
  pinned with `-Xjit-recompile-threshold=64`, version checks scoped to
  the target function, prove-can-fail for headline pins (mutate, show
  the named check fail, restore).
- Commit messages end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01ExvqpAhy7pehcdZf34z3dB`.

## File Structure

- `include/hermes/VM/JIT/JitFunctionData.h` — record types
  (`JitByValSiteRecord`, `JitConsumerRecords`), typed `consumerRecords`
  ownership, supported-kind predicate.
- `lib/VM/JIT/JitHandlers.h/.cpp` — `_jit_put_by_val_{loose,strict}`.
- `lib/VM/JIT/JitCompiler.cpp` — carry-forward copy, ByVal progress
  term, diagnostics line.
- `lib/VM/JIT/RuntimeOffsets.h` — typed-array/buffer offsets; flags
  helpers moved out of the Hades-only section.
- `lib/VM/JIT/x86-64/JitEmitter.h/-property.cpp` — helper-call swap,
  sticky flag, tier selection, `emitPutByValTypedArrayTier`.
- `test/jit/x86-64/` — new tests; `putbyval-inline-emitted.js` update.

---

### Task 1: Consumer record types and carry-forward

**Files:**
- Modify: `include/hermes/VM/JIT/JitFunctionData.h`
- Modify: `lib/VM/JIT/JitCompiler.cpp` (Compiler ctor, ~line 101)

**Interfaces:**
- Produces: `JitByValSiteRecord` (fields below, exact names),
  `JitConsumerRecords` with `std::deque<JitByValSiteRecord> byValSites`
  and `JitByValSiteRecord &findOrCreateByValSite(uint32_t siteId)`;
  `JitVersionData::consumerRecords` becomes
  `std::unique_ptr<JitConsumerRecords>`;
  `bool isJitSupportedTypedArrayStoreKind(CellKind)`.

- [ ] **Step 1: Add the record types to JitFunctionData.h**

Add `#include "hermes/VM/CellKind.h"` and `#include <deque>`. Before
`JitVersionData`, add:

```cpp
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

/// One PutByVal site's observed shape and what the owning body emitted
/// for it. Observed fields are written by the recording helper and (for
/// jsArraySeen) by the sticky-flag store embedded in prior-based
/// JSArray tiers; emitted fields are filled during emission. See the
/// 2026-09-08 PutByVal spec.
struct JitByValSiteRecord {
  /// Sentinel for taKind/specializedTAKind: no kind recorded/emitted.
  static constexpr uint8_t kTAKindNone = 0xff;
  /// Sentinel for taKind: a second, different typed-array kind was
  /// seen; the site never specializes and never counts as progress.
  static constexpr uint8_t kTAKindPoly = 0xfe;

  /// Bytecode offset of the PutByVal instruction: unique within the
  /// function, stable across versions.
  uint32_t siteId;
  /// JSArray traffic observed here. uint8_t (not bool) because emitted
  /// code stores the constant 1 into it through an embedded address.
  uint8_t jsArraySeen = 0;
  /// A non-JSArray, non-supported-typed-array target declined here.
  uint8_t otherSeen = 0;
  /// Observed typed-array kind: a CellKind value, or a sentinel.
  uint8_t taKind = kTAKindNone;
  /// What the owning body emitted at this site.
  uint8_t jsArrayTierEmitted = 0;
  uint8_t specializedTAKind = kTAKindNone;
};

/// The version-local consumer feedback records, owned by (and destroyed
/// with) the JitVersionData. byValSites is a deque, NOT a vector:
/// emitted code embeds the address of an entry's jsArraySeen byte, so
/// entries must never relocate once created. Lookup is a linear scan;
/// functions have a handful of ByVal sites.
struct JitConsumerRecords {
  std::deque<JitByValSiteRecord> byValSites;

  /// \return the entry for \p siteId, creating it if absent.
  JitByValSiteRecord &findOrCreateByValSite(uint32_t siteId) {
    for (auto &s : byValSites)
      if (s.siteId == siteId)
        return s;
    byValSites.emplace_back();
    byValSites.back().siteId = siteId;
    return byValSites.back();
  }
};
```

- [ ] **Step 2: Retype consumerRecords**

Replace the `void *consumerRecords = nullptr;` member (and its
comment) in `JitVersionData` with:

```cpp
  /// Version-local consumer feedback (PutByVal per-site shape
  /// records). Null until the first entry is created; destroyed with
  /// this record, which outlives every possible execution of the body.
  std::unique_ptr<JitConsumerRecords> consumerRecords{};
```

- [ ] **Step 3: Carry-forward copy in the Compiler ctor**

In `lib/VM/JIT/JitCompiler.cpp`, add a body to the `Compiler`
constructor (currently `{}`, ~line 120):

```cpp
    // Carry forward the observed shape feedback from the version this
    // candidate is compiled to replace: what any version learned, the
    // next version knows. Emitted-tier fields start empty and are
    // filled by this compile. First compiles start with no records.
    if (JitFunctionData *jd = codeBlock->getJitData()) {
      if (jd->current && jd->current->consumerRecords) {
        auto copy = std::make_unique<JitConsumerRecords>();
        for (const auto &s : jd->current->consumerRecords->byValSites) {
          JitByValSiteRecord r = s;
          r.jsArrayTierEmitted = 0;
          r.specializedTAKind = JitByValSiteRecord::kTAKindNone;
          copy->byValSites.push_back(r);
        }
        candidateVD_->consumerRecords = std::move(copy);
      }
    }
```

- [ ] **Step 4: Build both backends**

Run: `cmake --build cmake-build-x86jit --target hermes` and
`cmake --build cmake-build-arm64 --target hermes`. Expected: clean.

- [ ] **Step 5: Run the JIT suite (no behavior change expected)**

Run: `LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes`
Expected: all pass.

- [ ] **Step 6: Commit** — `JIT: add per-version PutByVal consumer records`

---

### Task 2: The recording helpers and the emitter call swap

**Files:**
- Modify: `lib/VM/JIT/JitHandlers.h` (after `_jit_put_by_id`)
- Modify: `lib/VM/JIT/JitHandlers.cpp`
- Modify: `lib/VM/JIT/x86-64/JitEmitter.h` (DECL_PUT_BY_VAL ~line 973,
  `putByValImpl` decl ~line 1559)
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp`
  (`putByValImpl`, ~line 196)
- Modify: `test/jit/x86-64/putbyval-inline-emitted.js`

**Interfaces:**
- Consumes: Task 1's `JitConsumerRecords::findOrCreateByValSite`,
  `isJitSupportedTypedArrayStoreKind`.
- Produces: `void _jit_put_by_val_loose(SHRuntime *, SHLegacyValue
  *target, SHLegacyValue *key, SHLegacyValue *value, SHJitVersionData
  *versionData, uint32_t siteId)` and `_jit_put_by_val_strict` (same
  signature); `putByValImpl(FR, FR, FR, const char *name, bool
  strict)`.

- [ ] **Step 1: Declare the helpers in JitHandlers.h**

```cpp
/// Slow path of PutByVal (loose), and the recording site for ByVal
/// tier declines: records the observed target shape into
/// \p versionData's entry for \p siteId, counts the decline (possibly
/// triggering a recompile), then forwards to the plain SH helper.
/// \param siteId the PutByVal instruction's bytecode offset.
void _jit_put_by_val_loose(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    SHJitVersionData *versionData,
    uint32_t siteId);
/// Strict-mode variant of _jit_put_by_val_loose.
void _jit_put_by_val_strict(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    SHJitVersionData *versionData,
    uint32_t siteId);
```

- [ ] **Step 2: Implement in JitHandlers.cpp**

Model placement on `_jit_put_by_id`. Record BEFORE the trigger: the
observation that crosses the threshold must participate in the
feedback the recompile reads (spec, "The recording helper"). The
classification extracts scalars only; no raw pointer survives into the
trigger, which may move the heap.

```cpp
/// Record one PutByVal decline's observed target shape into \p vd's
/// entry for \p siteId. Reads the target's kind as a scalar and
/// retains no raw pointer; allocates only native memory.
static void recordByValObservation(
    JitVersionData *vd,
    uint32_t siteId,
    SHLegacyValue *target) {
  if (!vd->consumerRecords)
    vd->consumerRecords = std::make_unique<JitConsumerRecords>();
  JitByValSiteRecord &site =
      vd->consumerRecords->findOrCreateByValSite(siteId);
  HermesValue t = *toPHV(target);
  if (!t.isObject()) {
    site.otherSeen = 1;
    return;
  }
  CellKind kind = static_cast<GCCell *>(t.getObject())->getKind();
  if (kind == CellKind::JSArrayKind) {
    site.jsArraySeen = 1;
  } else if (isJitSupportedTypedArrayStoreKind(kind)) {
    uint8_t k8 = (uint8_t)kind;
    if (site.taKind == JitByValSiteRecord::kTAKindNone)
      site.taKind = k8;
    else if (site.taKind != k8)
      site.taKind = JitByValSiteRecord::kTAKindPoly;
  } else {
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
  recordByValObservation(vd, siteId, target);
  // Recompilation trigger, as in _jit_put_by_id: every call is a
  // decline of the calling body's ByVal tiers; the shared counter and
  // threshold pool ById and ByVal declines. Runs after recording (see
  // above) but before the store logic derives raw pointers.
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold)) {
    getRuntime(shr).getJITContext().considerRecompile(getRuntime(shr), vd);
  }
  _sh_ljs_put_by_val_loose_rjs(shr, target, key, value);
}
```

`_jit_put_by_val_strict` is identical except the forward is
`_sh_ljs_put_by_val_strict_rjs`. (These extern-C wrappers are the
forwarding boundary; `putByValWithReceiver_RJS` is TU-local to
StaticH.cpp.) Add any missing includes (`hermes/VM/static_h.h` for the
`_sh_ljs` declarations).

- [ ] **Step 3: Swap the emitter call**

In `JitEmitter.h`, change `putByValImpl`'s declaration to:

```cpp
  void putByValImpl(
      FR frTarget,
      FR frKey,
      FR frValue,
      const char *name,
      bool strict);
```

and the macro block to:

```cpp
#define DECL_PUT_BY_VAL(methodName, commentStr, strict)      \
  void methodName(FR frTarget, FR frKey, FR frValue) {       \
    putByValImpl(frTarget, frKey, frValue, commentStr, strict); \
  }

  DECL_PUT_BY_VAL(putByValLoose, "PutByValLoose", false);
  DECL_PUT_BY_VAL(putByValStrict, "PutByValStrict", true);
```

In `JitEmitter-property.cpp`, `putByValImpl`'s helper-call tail
becomes (the tier structure around it is unchanged in this task):

```cpp
  uint32_t siteId =
      (uint32_t)((const char *)emittingIP - (const char *)codeBlock_->begin());
  ...
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  loadFrameAddr(x86::rdx, frKey);
  loadFrameAddr(x86::rcx, frValue);
  loadBits64InGp(x86::r8, (uint64_t)versionData_, "JitVersionData");
  a.mov(x86::r9d, asmjit::Imm(siteId));
  if (strict) {
    EMIT_RUNTIME_CALL(
        *this,
        void (*)(
            SHRuntime *,
            SHLegacyValue *,
            SHLegacyValue *,
            SHLegacyValue *,
            SHJitVersionData *,
            uint32_t),
        _jit_put_by_val_strict);
  } else {
    EMIT_RUNTIME_CALL(
        *this,
        void (*)(
            SHRuntime *,
            SHLegacyValue *,
            SHLegacyValue *,
            SHLegacyValue *,
            SHJitVersionData *,
            uint32_t),
        _jit_put_by_val_loose);
  }
```

Compute `siteId` at the TOP of `putByValImpl` (before any emission).
The `EMIT_RUNTIME_CALL` macro stringifies its function argument, so
passing the actual symbols (not a variable) is what keeps the dump
lines meaningful — this is why the `shImpl` function-pointer parameter
is removed rather than kept. Six arguments fit in registers (rdi, rsi,
rdx, rcx, r8, r9); no stack args. Add `#include
"hermes/VM/JIT/JitHandlers.h"` to JitEmitter-property.cpp if missing.
`SHJitVersionData` may need a forward typedef visible there; use the
one from JitHandlers.h.

- [ ] **Step 4: Build x86-64 and arm64**

Expected: clean. arm64 does not reference `putByValImpl` (its own
emitter has its own); JitHandlers.cpp is shared and must compile for
both.

- [ ] **Step 5: Update putbyval-inline-emitted.js and run the suite**

The test pins `call _sh_ljs_put_by_val_loose_rjs` (~line 121); change
to `call _jit_put_by_val_loose`. The two new argument-setup lines
(`r8`/`r9d`) appear between the `rcx` load and the call — add SPEC
lines for them if the surrounding pins are CHECK-NEXT-tight; keep them
unpinned otherwise. Run:
`LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes`
Expected: all pass. NOTE the precise neutrality claim: ByVal feedback
cannot yet qualify as PROGRESS (the gate still requires a warmed cold
ById site), but the shared counter now counts ByVal declines too, so
a function that ALSO has a warmed cold-ById site can newly recompile,
or recompile sooner (a mixed stream crosses 64 in ~32 calls). That is
a behavior change in triggering timing; if any existing test's version
pins shift, inspect the dump and adjust the test's bounded windows —
do not paper over an unexplained diff.

- [ ] **Step 6: Commit** — `JIT: record PutByVal shape observations per site`

---

### Task 3: Sticky-flag instrumentation on prior-based JSArray tiers

**Files:**
- Modify: `lib/VM/JIT/x86-64/JitEmitter.h` (Emitter members,
  `emitPutByValFastArrayTier` decl ~line 1741)
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp`
- Modify: `test/jit/x86-64/putbyval-inline-emitted.js` (if its SPEC
  window covers the tier tail)

**Interfaces:**
- Consumes: Task 1 records; Task 2's siteId computation.
- Produces: `JitByValSiteRecord &Emitter::byValSiteRecord(uint32_t
  siteId)`; `emitPutByValFastArrayTier(FR, FR, FR, const
  asmjit::Label &helperLab, uint8_t *stickyFlag)`.

- [ ] **Step 1: Give the Emitter access to the candidate's records**

Add to Emitter (near `coldWriteCacheIdxs_`); include
`hermes/VM/JIT/JitFunctionData.h` in JitEmitter.h if not already
transitively available (versionData_ is a `JitVersionData *`):

```cpp
  /// \return the candidate version record's ByVal site entry for
  /// \p siteId, creating the records and/or entry if absent. The
  /// entry's address is stable (deque) and may be embedded in code.
  JitByValSiteRecord &byValSiteRecord(uint32_t siteId) {
    if (!versionData_->consumerRecords)
      versionData_->consumerRecords = std::make_unique<JitConsumerRecords>();
    return versionData_->consumerRecords->findOrCreateByValSite(siteId);
  }
```

- [ ] **Step 2: Add the flag parameter to the fast-array tier**

`emitPutByValFastArrayTier` gains a final `uint8_t *stickyFlag`
parameter. At the END of the tier (after `emitSafeStoreOrSlow`, i.e.
on the fall-through success path only), emit:

```cpp
  if (stickyFlag) {
    comment("// Record JSArray traffic (sticky flag)");
    loadBits64InGp(xScratch, (uint64_t)stickyFlag, "jsArraySeen");
    a.mov(x86::byte_ptr(xScratch), 1);
  }
```

`xScratch` (r11) is never handed out by TempRegAlloc and nothing
relies on it across this boundary; a blind byte store of an immediate
needs no other register.

- [ ] **Step 3: Wire it in putByValImpl**

```cpp
  JitByValSiteRecord &site = byValSiteRecord(siteId);
  ...
#if HERMES_JIT_INLINE_SAFE_STORE
  // Sticky flag on prior-based tiers only: a tier emitted on positive
  // jsArraySeen evidence needs no instrumentation.
  uint8_t *stickyFlag = site.jsArraySeen ? nullptr : &site.jsArraySeen;
  emitPutByValFastArrayTier(frTarget, frKey, frValue, helperLab, stickyFlag);
  ...
#endif
```

(Until Task 4 adds tier selection, every site emits the JSArray tier
and — since `jsArraySeen` can only be set at runtime after v1 exists —
v1 tiers all get the flag, which is exactly the spec's v1 behavior.
A recompiled body whose site has `jsArraySeen` already set emits no
flag.)

- [ ] **Step 4: Build, run the JIT suite, fix the emitted test**

The tier now ends with two extra instructions; update
`putbyval-inline-emitted.js` SPEC pins if they break (the flag store
is a pinnable `mov byte ptr` + the materialization; prefer adding a
`SPEC: // Record JSArray traffic` line over pinning registers).
Expected: suite green.

- [ ] **Step 5: Commit** — `JIT: observe inline JSArray PutByVal hits via a sticky flag`

---

### Task 4: Typed-array tier, evidence-driven tier selection, diagnostics

**Files:**
- Modify: `lib/VM/JIT/RuntimeOffsets.h`
- Modify: `include/hermes/VM/JSTypedArray.h` (friend declaration)
- Modify: `include/hermes/VM/JSArrayBuffer.h` (friend declaration)
- Modify: `lib/VM/JIT/x86-64/JitEmitter.h`
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp`
- Modify: `lib/VM/JIT/JitCompiler.cpp` (dump block, ~line 449)
- Create: `test/jit/x86-64/recompile-taval-byid-trigger.js`

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: `emitPutByValTypedArrayTier(FR frTarget, FR frKey, FR
  frValue, CellKind kind, const asmjit::Label &kindMissLab, const
  asmjit::Label &helperLab)`; `emitPutByValFastArrayTier` gains `bool
  targetKnownObject` (before `stickyFlag`); RuntimeOffsets
  `jsTypedArrayBaseBuffer/Length/Offset`, `jsArrayBufferData`;
  the `JIT ByVal sites: N observed, M specialized` dump line.

- [ ] **Step 1: RuntimeOffsets**

Move `objectFlagsFastArrayMask()` and `objectFlagsFastArrayValue()`
(lines ~296-323) OUT of the `#if HERMESVM_GCKIND ==
_HERMESVM_GCVALUE_HADES` block, placing them right after its `#endif`
with their comment intact — they depend only on SHObjectFlags, and the
typed-array tier (which has no write barrier and exists under every
GC) uses them. Add the four new offsets in the SAME unconditional
place, next to the relocated helpers — NOT next to the ArrayImpl
offsets, which live inside the Hades-only block and must stay there
(under MallocGC the unconditional typed-array tier still compiles and
must see these members):

```cpp
  static constexpr uint32_t jsTypedArrayBaseBuffer =
      offsetof(JSTypedArrayBase, buffer_);
  static constexpr uint32_t jsTypedArrayBaseLength =
      offsetof(JSTypedArrayBase, length_);
  static constexpr uint32_t jsTypedArrayBaseOffset =
      offsetof(JSTypedArrayBase, offset_);
  static constexpr uint32_t jsArrayBufferData =
      offsetof(JSArrayBuffer, data_);
```

with `#include "hermes/VM/JSTypedArray.h"` (brings JSArrayBuffer).
The pragma in the file only silences the layout warning — it does NOT
grant access, and these members are protected/private with no
friendship (unlike ArrayImpl, which already friends RuntimeOffsets at
JSArray.h:35 for exactly this purpose). Add to JSTypedArrayBase and to
JSArrayBuffer, next to their members, following ArrayImpl's precedent
and comment style:

```cpp
  /// The x86-64 JIT emits an inline typed-array element store that
  /// reads the fields above directly; RuntimeOffsets is where their
  /// offsets are derived with offsetof().
  friend struct RuntimeOffsets;
```

- [ ] **Step 2: The typed-array tier emitter**

Declare in JitEmitter.h (next to `emitPutByValFastArrayTier`, but NOT
under `HERMES_JIT_INLINE_SAFE_STORE` — this tier has no barrier and is
unconditional on x86-64):

```cpp
  /// Emit the inline typed-array store tier specialized for exactly
  /// \p kind. A kind mismatch on an object target branches to
  /// \p kindMissLab (the JSArray tier at duo sites, else the helper);
  /// every other guard declines to \p helperLab, whose helper call
  /// preserves exact JS semantics and keeps recording. Emits no write
  /// barrier: typed-array storage holds no GC pointers.
  void emitPutByValTypedArrayTier(
      FR frTarget,
      FR frKey,
      FR frValue,
      CellKind kind,
      const asmjit::Label &kindMissLab,
      const asmjit::Label &helperLab);
```

Implementation in JitEmitter-property.cpp, mirroring the register
discipline of `emitPutByValFastArrayTier` (alloc temps, record, free
all before code):

```cpp
void Emitter::emitPutByValTypedArrayTier(
    FR frTarget,
    FR frKey,
    FR frValue,
    CellKind kind,
    const asmjit::Label &kindMissLab,
    const asmjit::Label &helperLab) {
  comment("// Inline typed array store (kind %u)", (unsigned)kind);
  bool isFloat64 = kind == CellKind::Float64ArrayKind;
  bool isFloat32 = kind == CellKind::Float32ArrayKind;
  uint32_t logWidth;   // element width: 0/1/2/3 (log2 bytes)
  switch (kind) {
    case CellKind::Uint8ArrayKind:
    case CellKind::Int8ArrayKind:  logWidth = 0; break;
    case CellKind::Uint16ArrayKind:
    case CellKind::Int16ArrayKind: logWidth = 1; break;
    case CellKind::Uint32ArrayKind:
    case CellKind::Int32ArrayKind:
    case CellKind::Float32ArrayKind: logWidth = 2; break;
    case CellKind::Float64ArrayKind: logWidth = 3; break;
    default: llvm_unreachable("unsupported typed-array tier kind");
  }

  freeAllFRTempExcept(frTarget);
  HWReg hwTarget = getOrAllocFRInGpX(frTarget, true);
  HWReg hwValue = getOrAllocFRInVecD(frValue, true);
  HWReg hwKey = getOrAllocFRInVecD(frKey, true);
  HWReg hwLoc = allocTempGpX();
  HWReg hwIdx = allocTempGpX();
  HWReg hwTemp1 = allocTempGpX();
  HWReg hwTemp2 = allocTempGpX();
  HWReg hwKeyTmp = allocTempVecD();
  HWReg hwValTmp = allocTempVecD();
  const x86::Gp target = hwTarget.gpq();
  const x86::Xmm value = hwValue.xmm();
  const x86::Xmm key = hwKey.xmm();
  const x86::Gp loc = hwLoc.gpq();
  const x86::Gp idx = hwIdx.gpq();
  const x86::Gp temp1 = hwTemp1.gpq();
  const x86::Gp temp2 = hwTemp2.gpq();
  const x86::Xmm keyTmp = hwKeyTmp.xmm();
  const x86::Xmm valTmp = hwValTmp.xmm();
  freeReg(hwLoc);
  freeReg(hwIdx);
  freeReg(hwTemp1);
  freeReg(hwTemp2);
  freeReg(hwKeyTmp);
  freeReg(hwValTmp);
  freeFRTemp(frTarget);
  freeFRTemp(frKey);
  freeFRTemp(frValue);

  // Object and exact-kind checks come FIRST (before any value-based
  // decline): at a duo site a kind miss must reach the JSArray tier,
  // which handles non-number values the checks below would decline.
  emit_sh_ljs_is_object(a, temp1, target);
  a.jne(helperLab);
  emit_sh_ljs_get_pointer(a, loc, target);
  a.cmp(
      x86::byte_ptr(
          loc,
          (int32_t)(offsetof(SHGCCell, kindAndSize) +
                    RuntimeOffsets::kindAndSizeKind)),
      asmjit::Imm((uint8_t)kind));
  a.jne(kindMissLab);

  // Object flags: fastIndexProperties set, frozen clear -- the same
  // masked compare the fast-array tier emits, and for the same reason:
  // an out-of-range defineProperty clears fastIndexProperties, and a
  // frozen typed array must throw on strict stores, not be written.
  a.mov(temp1.r32(), x86::dword_ptr(loc, offsetof(SHJSObject, flags)));
  a.and_(temp1.r32(), asmjit::Imm(RuntimeOffsets::objectFlagsFastArrayMask()));
  a.cmp(temp1.r32(), asmjit::Imm(RuntimeOffsets::objectFlagsFastArrayValue()));
  a.jne(helperLab);

  // Value conversion. Non-numbers are NaN-encoded, so:
  // - int kinds: 64-bit cvttsd2si yields the 0x8000... sentinel for
  //   NaN (any non-number), +-Inf, and |x| >= 2^63; one compare
  //   declines them all, and for everything else truncate-then-take-
  //   low-bits is exactly truncateToInt32's modular semantics.
  // - float kinds: a self-compare's parity flag declines every NaN
  //   pattern (real NaN included -- the helper stores canonical NaN).
  if (isFloat64 || isFloat32) {
    a.vucomisd(value, value);
    a.jp(helperLab);
    if (isFloat32)
      a.vcvtsd2ss(valTmp, valTmp, value);
  } else {
    a.vcvttsd2si(temp2, value);
    loadBits64InGp(temp1, (uint64_t)1 << 63, "int conversion sentinel");
    a.cmp(temp2, temp1);
    a.je(helperLab);
  }

  // Key: a double that survives a round trip through uint32. The
  // parity exit covers NaN-encoded non-number keys, as in the
  // fast-array tier. No 0xFFFFFFFF exclusion: bounds covers it.
  emit_double_is_uint32(a, idx, keyTmp, key);
  a.jne(helperLab);
  a.jp(helperLab);

  // Bounds: idx < length_. (No resizable ArrayBuffers in this tree;
  // length_ is fixed for the object's lifetime.)
  a.cmp(idx.r32(), x86::dword_ptr(loc, RuntimeOffsets::jsTypedArrayBaseLength));
  a.jae(helperLab);

  // The data pointer: buffer_ (compressed pointer), its data_ (null
  // iff detached -- the attached check is free with this load), plus
  // the view's byte offset. xScratch carries the offset so no fifth
  // allocatable temp is needed.
  a.mov(
      xScratch.r32(),
      x86::dword_ptr(loc, RuntimeOffsets::jsTypedArrayBaseOffset));
  emit_load_cp(a, loc, x86::ptr(loc, RuntimeOffsets::jsTypedArrayBaseBuffer));
  emit_sh_cp_decode_non_null(a, loc);
  a.mov(loc, x86::qword_ptr(loc, RuntimeOffsets::jsArrayBufferData));
  a.test(loc, loc);
  a.jz(helperLab);
  a.add(loc, xScratch);

  // The store.
  if (isFloat64) {
    a.vmovsd(x86::qword_ptr(loc, idx, 3), value);
  } else if (isFloat32) {
    a.vmovss(x86::dword_ptr(loc, idx, 2), valTmp);
  } else if (logWidth == 0) {
    a.mov(x86::byte_ptr(loc, idx, 0), temp2.r8());
  } else if (logWidth == 1) {
    a.mov(x86::word_ptr(loc, idx, 1), temp2.r16());
  } else {
    a.mov(x86::dword_ptr(loc, idx, 2), temp2.r32());
  }
}
```

Adjust asmjit spellings to the neighboring code's (e.g. if the file
uses `a.cvttsd2si` rather than `a.vcvttsd2si`, match it; the fast-array
tier and `emit_double_is_uint32` are the reference). If
`emit_sh_ljs_get_pointer`/`emit_load_cp`/`emit_sh_cp_decode_non_null`
have different arities than shown, follow their uses in
`emitPutByValFastArrayTier` exactly.

- [ ] **Step 3: Tier selection in putByValImpl**

Replace the tier block with evidence-driven selection. Full new shape
of the function body between the syncs and the helper tail:

```cpp
  // Evidence-driven tier selection (spec: "Tier selection at
  // recompile"). A first compile has no records, so every site is
  // prior-based: JSArray tier plus sticky flag, exactly the old
  // behavior. Recompiles emit what the observations support; a tier
  // is dropped only on positive evidence of other traffic.
  bool haveEvidence = site.jsArraySeen || site.otherSeen ||
      site.taKind != JitByValSiteRecord::kTAKindNone;
#if HERMES_JIT_INLINE_SAFE_STORE
  bool emitJSArray = site.jsArraySeen || !haveEvidence;
#else
  bool emitJSArray = false;
#endif
  bool emitTA = site.taKind != JitByValSiteRecord::kTAKindNone &&
      site.taKind != JitByValSiteRecord::kTAKindPoly &&
      isJitSupportedTypedArrayStoreKind((CellKind)site.taKind);
  site.jsArrayTierEmitted = emitJSArray;
  site.specializedTAKind =
      emitTA ? site.taKind : JitByValSiteRecord::kTAKindNone;

  asmjit::Label helperLab{};
  asmjit::Label contLab{};
  if (emitJSArray || emitTA) {
    helperLab = a.newLabel();
    contLab = a.newLabel();
  }
  if (emitTA) {
    // The recorded kind is compared first: it is what the site was
    // observed declining on. At a duo site its kind miss chains to
    // the JSArray tier rather than the helper.
    asmjit::Label kindMissLab = emitJSArray ? a.newLabel() : helperLab;
    emitPutByValTypedArrayTier(
        frTarget, frKey, frValue, (CellKind)site.taKind, kindMissLab,
        helperLab);
    a.jmp(contLab);
    if (emitJSArray)
      a.bind(kindMissLab);
  }
#if HERMES_JIT_INLINE_SAFE_STORE
  if (emitJSArray) {
    uint8_t *stickyFlag = site.jsArraySeen ? nullptr : &site.jsArraySeen;
    emitPutByValFastArrayTier(
        frTarget, frKey, frValue, helperLab,
        /*targetKnownObject*/ emitTA, stickyFlag);
    a.jmp(contLab);
  }
#endif
  if (emitJSArray || emitTA)
    a.bind(helperLab);
```

and after the helper call: `if (emitJSArray || emitTA) a.bind(contLab);`.
`emitPutByValFastArrayTier` gains `bool targetKnownObject` (before
`stickyFlag`): when true, skip its `emit_sh_ljs_is_object` + `jne`
(the TA tier already proved the target an object); it still derives
its own object pointer. Ruling recorded here, as an ACCEPTED
performance/code-size tradeoff (not an equivalence): on a duo site's
JSArray branch, the TA tier compares the kind byte and the JSArray
tier then compares it again from memory, re-deriving its own object
pointer and reloading operands the first tier's register bookkeeping
released. The spec's "one shared kind-load" ideal is deliberately not
implemented; the spec's shared-prefix sentence has been aligned to
say so. Also ruled: the
TA tier's value guards run AFTER the kind dispatch (the spec's guard
list is satisfied, reordered) so that a duo site's non-number JSArray
stores reach the JSArray tier instead of declining to the helper.

- [ ] **Step 4: The diagnostics line**

In JitCompiler.cpp, after the `JIT cold ById sites` block (~line 452):

```cpp
    if (auto *cr = jitData->current->consumerRecords.get()) {
      size_t observed = 0, specialized = 0;
      for (const auto &s : cr->byValSites) {
        if (s.jsArraySeen || s.otherSeen ||
            s.taKind != JitByValSiteRecord::kTAKindNone)
          ++observed;
        if (s.specializedTAKind != JitByValSiteRecord::kTAKindNone)
          ++specialized;
      }
      if (observed) {
        llvh::outs() << "JIT ByVal sites: " << observed << " observed, "
                     << specialized << " specialized\n";
      }
    }
```

- [ ] **Step 5: Build both backends, run the JIT suite**

Expected: green. Version-1 behavior is bit-for-bit the Task 3 shape
(no records at first compile), so existing pins hold.

- [ ] **Step 6: End-to-end smoke test (ById-triggered)**

Create `test/jit/x86-64/recompile-taval-byid-trigger.js`. The ByVal
progress term does not exist yet, so the recompile is triggered by a
warmed ById site; the point pinned is that version 2 SELECTS the
typed-array tier from the recorded evidence.

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// f's ById site (o.p) is cold under force and warms through helper
// calls; its ByVal site sees only Int32Array stores, recorded by the
// same helper stream. The ById progress source triggers the recompile;
// version 2 must select the typed-array tier for the ByVal site and
// drop the JSArray tier there (no JSArray evidence).
function f(a, o, v) {
  a[0] = v;
  o.p = v;
}
var ta = new Int32Array(4);
var o = {p: 0};
for (var i = 0; i < 100; ++i)
  f(ta, o, i);
print(ta[0], o.p);

// OUT: 99 99

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: // Inline typed array store
// SPEC-NOT: // Inline fast array store
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
```

(If 100 iterations do not cross the threshold before the loop ends —
declines only come from the ByVal site AND the ById site until each
warms — raise the iteration count; the ById site declines on every
call until the recompile, the ByVal site likewise, so the shared
counter grows by ~2 per call and 100 is ample.) Prove-can-fail: flip
`SPEC-NOT` to a real line by temporarily forcing `emitJSArray = true`
in the selection, confirm the named check fails, revert.

- [ ] **Step 7: Commit** — `JIT: inline typed-array PutByVal tier with evidence-driven selection`

---

### Task 5: ByVal progress term in considerRecompile

**Files:**
- Modify: `lib/VM/JIT/JitCompiler.cpp` (`considerRecompile`, ~line 227)
- Create: `test/jit/x86-64/recompile-taval-trigger.js`

**Interfaces:**
- Consumes: record fields from Task 1; selection semantics from Task 4.
- Produces: recompiles trigger on ByVal shape progress with no ById
  involvement.

- [ ] **Step 1: Extend the progress check**

Add a file-local helper above `considerRecompile`:

```cpp
/// \return true if some ByVal site's observed shape is not covered by
/// the current body's emitted tiers and a recompile could cover it.
/// A poisoned kind never counts; a jsArraySeen mismatch counts only
/// where the JSArray tier is emittable at all.
static bool byValShapeProgress(const JitVersionData *vd) {
  const JitConsumerRecords *cr = vd->consumerRecords.get();
  if (!cr)
    return false;
  for (const auto &s : cr->byValSites) {
    if (s.taKind != JitByValSiteRecord::kTAKindNone &&
        s.taKind != JitByValSiteRecord::kTAKindPoly &&
        isJitSupportedTypedArrayStoreKind((CellKind)s.taKind) &&
        s.taKind != s.specializedTAKind)
      return true;
#if HERMES_JIT_INLINE_SAFE_STORE
    if (s.jsArraySeen && !s.jsArrayTierEmitted)
      return true;
#endif
  }
  return false;
}
```

In `considerRecompile`, replace the gate and warm scan tail:

```cpp
  if (!enabled_ || !data->recompileBudget)
    return;
  // Progress check, two sources, either qualifies: a specific cold
  // ById site whose cache has since warmed, or a ByVal site whose
  // observed shape the current body's tiers do not cover. The
  // recompile emits everything both sources know.
  bool progress = false;
  if (vd->coldByIdSites != 0) {
    ... existing warmed scan, setting progress ...
  }
  if (!progress)
    progress = byValShapeProgress(vd);
  if (!progress)
    return;
  recompile(runtime, codeBlock);
```

(Keep the existing warm-scan code verbatim; only the variable name and
the surrounding structure change. `HERMES_JIT_INLINE_SAFE_STORE` comes
from `hermes/VM/JIT/Config.h`, already included transitively; verify.)

- [ ] **Step 2: Build both backends; run the JIT suite**

Expected: green — in particular the existing recompile suite, whose
functions' ByVal-free decline streams see `byValShapeProgress` return
false on empty records. arm64: records are never created (its emitter
never calls the recording helper), so the term is dormant.

- [ ] **Step 3: The pure-ByVal trigger test**

`test/jit/x86-64/recompile-taval-trigger.js` — same RUN lines as Task
4's test plus an OFF line
(`-Xjit-max-recompiles=0 ... | %FileCheck --check-prefix=OFF`):

```js
// f has no ById site at all: the recompile must trigger from the
// ByVal decline stream and its recorded Int32Array monomorphism.
function f(a, v) {
  a[0] = v;
}
var ta = new Int32Array(4);
for (var i = 0; i < 100; ++i)
  f(ta, i);
print(ta[0]);

// OUT: 99

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: // Inline typed array store
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized

// OFF: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// OFF-NOT: (version 2)
```

Scope the OFF-NOT window to f as recompile-byid-warm.js does (bound it
before global's lines). Prove-can-fail: comment out the
`byValShapeProgress` call, confirm the `(version 2)` SPEC line fails,
restore.

- [ ] **Step 4: Commit** — `JIT: trigger recompilation on uncovered PutByVal shapes`

---

### Task 6: Policy tests (shape selection end to end)

**Files:**
- Create: `test/jit/x86-64/recompile-taval-mono-jsarray.js`
- Create: `test/jit/x86-64/recompile-taval-duo.js`
- Create: `test/jit/x86-64/recompile-taval-poly-budget.js`
- Create: `test/jit/x86-64/recompile-taval-object-drop.js`
- Create: `test/jit/x86-64/recompile-taval-selfcorrect.js`
- Create: `test/jit/x86-64/recompile-taval-delayed-site.js`

All use the Task 4/5 RUN-line shape; versions scoped to the target
function; every headline pin gets a prove-can-fail pass (temporarily
break the behavior or inject the forbidden line, watch the named check
fail, restore). Iteration counts assume threshold 64 pinned on the RUN
line; adjust upward if a phase's decline arithmetic falls short, and
note the arithmetic in the test comment as
recompile-staleness-budget.js does.

- [ ] **Step 1: mono-JSArray** — `f(arr, o, v) { arr[0] = v; o.p = v; }`
called 100x with a dense `[0]` array and one object. The arr site's
stores are all inline tier hits (sticky flag), o.p declines warm the
ById cache and trigger v2. Pins: v2 window contains
`// Inline fast array store`, does NOT contain
`// Inline typed array store`, and `JIT ByVal sites: 1 observed, 0
specialized` (the flag made the site observed).

- [ ] **Step 2: duo** — `f(x, v) { x[0] = v; }` called alternately with
a dense array and an Int32Array, 70 each interleaved (the Int32Array
declines alone must cross the threshold of 64). Array stores hit
inline (flag), Int32Array stores decline (mono kind + counter);
crossing triggers v2. Pins: v2 contains BOTH tier comments;
`1 observed, 1 specialized`; OUT prints both containers' values.

- [ ] **Step 3: poly + budget preserved** — budget 2
(`-Xjit-max-recompiles=2`). `f(x, o, phase, v) { x[0] = v; o.p = v;
if (phase) o.q = v; }`. Phase 1: alternate Int32Array/Float64Array
targets (poisons the site) 100x with phase=0; o.p warms and triggers
v2. Pins: v2 `1 observed, 0 specialized`... wait — the record also has
taKind poisoned AND otherSeen unset; observed = 1 site. Phase 2 (after
a marker print): 100 more calls with phase=1; o.q's cold cache (cold
at both compiles) warms and its ById declines cross the threshold →
v3 installs. Pins: `(version 3)` appears AFTER the phase marker, still
`0 specialized` — proving the poisoned site burned no budget in
phase 1 (with budget 2, a phase-1 burn would leave v3 possible too,
so ALSO pin that no `(version 3)` appears BEFORE the marker).

- [ ] **Step 4: plain-object drop** — `f(o, t, v) { o[0] = v; t.p = v; }`
with `o = {}` (numeric key on a plain object → otherSeen) and t
warming its ById cache, 100x. Pins: v2 window has NO
`// Inline fast array store` and NO `// Inline typed array store`
(bounded -NOTs), `1 observed, 0 specialized`; OUT correct.

- [ ] **Step 5: self-correction** — budget 2. Phase 1:
`f(x, v) { x[0] = v; }` with Int32Array only, 100x → v2 is TA-only
(no JSArray evidence: pins as in Task 5's test). Phase 2: dense-array
calls, 100x → JSArray stores now decline (no tier), set jsArraySeen,
cross the threshold → v3 with BOTH tiers. Pins: v3 window has both
tier comments; `1 observed, 1 specialized`; phase marker separates the
version pins.

- [ ] **Step 6: delayed first execution** — budget 2.
`f(x, o, phase, v) { o.p = v; if (phase) x[0] = v; }`. Phase 1:
100 calls with phase=0 (ByVal site never runs; o.p warms → v2; the
site's empty record keeps the prior JSArray tier WITH the flag in v2).
Phase 2: 40 dense-array calls with phase=1 (inline hits set the flag
in v2's records) then 100 Int32Array calls (declines: mono kind →
progress → v3). Pins: v3 has BOTH tier comments and
`1 observed, 1 specialized` — the flag in v2 is what makes the JSArray
tier survive into v3; without Task 3's any-version instrumentation v3
would be TA-only, which is exactly the prove-can-fail lever
(temporarily restrict the flag to first compiles — pass
`stickyFlag = nullptr` when records were carried forward — and watch
the v3 both-tiers pin fail; restore).

- [ ] **Step 7: run the JIT suite; commit** —
`JIT: PutByVal shape-selection policy tests`

---

### Task 7: Semantics tests (conversions, guards, fallbacks)

**Files:**
- Create: `test/jit/x86-64/taval-conversions.js`
- Create: `test/jit/x86-64/taval-guards.js`

- [ ] **Step 1: conversions** — every supported kind through its ACTIVE
tier. Structural requirements, each closing a hole a lazier recipe
leaves open:

- **Eight DISTINCT source-level store functions** (`storeI8`,
  `storeU8`, ... `storeF64`), each `function storeX(a, i, v) { a[i] =
  v; }` at top level. A factory closure would share one CodeBlock and
  one site across kinds, destroying monomorphism — do not use one.
- **The DRIVER warms, then probes, via separate invocations**: call
  `storeX(ta, 0, 1)` 70 times in a loop at top level, THEN call it
  once per probe. Recompilation publishes the new entry point for
  SUBSEQUENT invocations only — a warm loop and probes inside one
  activation would run every probe in version 1 while the banner
  still claims version 2.
- **Interpreter/JIT equality by actual stdout diff**, following
  test/jit/putbyval-inline.js:

```js
// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=SPEC %s
```

  (A shared FileCheck OUT prefix would NOT enforce equality —
  FileCheck matches substrings in order, so `0` matches inside `300`.)
  The SPEC run pins, per store function, `(version 2)` and the exact
  line `JIT ByVal sites: 1 observed, 1 specialized` (each store
  function has one site; a numeric wildcard would accept 0 specialized
  and let the whole diff pass vacuously through the helper).
- **Nonzero offset and index**: probe through a nonzero-offset view —
  `var buf = new ArrayBuffer(64); var v8 = new Uint8Array(buf); var
  vI32 = new Int32Array(buf, 8, 4);` — storing at index 2 (not 0) for
  each width, then print the neighboring BYTES via `v8` to prove the
  scaled address `data_ + offset_ + idx*width` is exact (an
  implementation dropping `offset_` or mis-scaling passes index-0
  probes).
- **Value probes** per int kind: 300, -129, 65536, -1.5, -0, NaN,
  Infinity, -Infinity, 2147483648, 4294967301, 9007199254740992,
  Math.pow(2, 63), -Math.pow(2, 63), 1e300 — print the element after
  each. Float32: 1.1, 16777217, NaN, Infinity. Float64: 1.1, -0
  (print 1/element for the sign), NaN.
- **Key probes** (these exercise the key parity/roundtrip exits,
  which value probes do not): key NaN, key 1.5, key -1, key "1"
  (string), each through a warmed site, printing the array state and
  any named-property fallout after each.

Fill the printed expectations by running the interpreter RUN line; the
diff line then IS the equality test.

- [ ] **Step 2: guards** — in `taval-guards.js`, the Step 1 diff-RUN
structure plus SPEC v2 pins, all through a specialized strict-mode
site (wrap the store in a `"use strict"` function; driver warms with
70 separate calls first). Every RUN line that reaches the detach case
carries `-Xhermes-internal-test-methods` (the hook exists only under
that flag — see test/hermes/typedarray-detached.js's RUN line):
  - flags-guard regression: warm f with a normal Int32Array; then
    `var b = new Int32Array(1);
    Object.defineProperty(b, "2", {value: 0}); Object.freeze(b);
    try { f(b, 0, 7); } catch (e) { print("caught", e.name); }` —
    expect the throw (this is the spec's P1 counterexample).
  - observable fallback: `var c = new Int32Array(4);
    Object.preventExtensions(c);` store to index 10 → expect the
    strict throw.
  - genuine no-op: out-of-bounds store on a plain extensible
    Int32Array (silent; print the unchanged length and element).
  - detached: use a SEPARATE, extensible Int32Array `d`;
    `HermesInternal.detachArrayBuffer(d.buffer);` then an in-bounds
    store through the warmed site, printing whatever the interpreter
    does — do NOT label it a no-op in advance: a detached array has
    no indexed properties, so the strict store takes generic
    assignment and its outcome (throw or not) is pinned by the
    interpreter diff, not assumed. (On the non-extensible `c` it
    would definitely throw — that is why `c` is not reused here.)
  Prove-can-fail for the flags-guard pin: comment out the tier's
  flags compare, confirm the "caught TypeError" line vanishes from
  the JIT run (diff fails), restore.

- [ ] **Step 3: run the JIT suite; commit** —
`JIT: typed-array store conversion and guard tests`

---

### Task 8: Cross-mode validation, perf, docs, bookkeeping

**Files:**
- Modify: `doc/JIT.md` (Recompilation section)
- Modify: `dz/issues/` (close via `dz` CLI)

- [ ] **Step 1: All heap modes**

Run the JIT suite in `cmake-build-x86jit` (HV64 ASan),
`cmake-build-x86jit-hv32`, and `cmake-build-x86jit-boxed`:
`LIT_FILTER='jit/' cmake --build <build> --target check-hermes`.
Expected: green in all three (the tier's only heap-sensitive steps are
the two compressed-pointer decodes). Build `cmake-build-arm64` once
more. Also build-check MallocGC — the heap-value modes do NOT cover
the `HERMES_JIT_INLINE_SAFE_STORE=0` configuration that the capability
gating and the unconditional-offsets placement exist for:

```bash
cmake -B cmake-build-x86jit-malloc -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DHERMESVM_GCKIND=MALLOC -DHERMESVM_ALLOW_JIT=2
cmake --build cmake-build-x86jit-malloc --target hermes
```

and run the Task 5 trigger test manually against that binary (the TA
tier is unconditional; the JSArray tier and its progress term are
compiled out).

- [ ] **Step 2: Perf acceptance**

Rebuild `cmake-build-x86jit-rel` (Release/clang). Recreate the
ta-store microbenchmark from dz 01a07f0a-def8 (Int32Array fill,
1000 x 20000, precompiled with `-O -emit-binary`), run 3x interp vs
`-Xjit=force`. Expected: JIT >= 2.5x vs interpreter (was ~1.03x).
Record the numbers in the dz issue on closing. Also re-run the
arr-store variant to confirm no dense-array regression.

- [ ] **Step 3: Docs**

Add a paragraph to doc/JIT.md's Recompilation section: PutByVal shape
records (what is recorded, by whom), evidence-driven tier selection,
the sticky flag, the typed-array tier's guards, and the two dump
lines. Point at the spec.

- [ ] **Step 4: Close the dz issue**

`dz close 01a07f0a-def8 --as fixed -m "<numbers from step 2>"` (from
the repo root). Leave the GetByVal issue (01a07f0a-11f4) open; add a
comment there noting the records and tier now exist to build on.

- [ ] **Step 5: Full non-JIT sanity**

Run the full suite once on the ASan build:
`cmake --build cmake-build-x86jit --target check-hermes`.
Expected: green.

- [ ] **Step 6: Commit** — `JIT: document PutByVal shape feedback; close dz issue`

---

## Self-Review Notes (done at plan time)

- Spec coverage: records/ownership/carry-forward (T1), helper +
  record-before-trigger + forwarding boundary + typed binding (T2),
  sticky flag on prior-based tiers any-version (T3), tier body with
  flags guard + conversions + selection rules + shared-prefix ruling +
  capability gating + RuntimeOffsets move + diagnostics (T4), progress
  term with poly/capability exclusions (T5), every spec-listed test
  (T4-T7), heap modes + perf acceptance (T8). Non-goals honored: no
  GetByVal, no receiver variant, no budget-default change, no arm64
  emission.
- Two plan-level rulings are recorded inline in Task 4 (guard order
  vs the spec's numbered list; the shared-prefix implementation) —
  reviewers should treat them as the controller's rulings, not
  implementer deviations.
- Type consistency: `site.taKind`/`specializedTAKind` are uint8_t
  CellKind values with 0xff/0xfe sentinels everywhere;
  `byValSiteRecord()` is the single accessor; helper signature is
  identical in T2's decl, impl, and EMIT_RUNTIME_CALL binding.
