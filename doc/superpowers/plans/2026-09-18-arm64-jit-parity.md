# arm64 JIT Functional Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the five emitter-side capabilities the x86-64 JIT gained
(ById recompile trigger, PutByVal recording + typed-array store tier,
shared element-load tail, GetByVal tiers, GetByIndex tiers) to the arm64
backend, and make the policy/semantics test corpus architecture-neutral.

**Architecture:** arm64-only emitter changes mirroring the x86-64 file
pair (lib/VM/JIT/arm64/JitEmitter*.{h,cpp}); the shared driver and the
x86-64 backend are untouched; tests relocate from test/jit/x86-64/ to
test/jit/ with content unchanged wherever they pin only banners,
comments, counters, and bytecode; new arm64 instruction-pin files follow
the *-emitted-arm64.js pattern.

**Tech Stack:** C++17, asmjit a64, lit/FileCheck, qemu-user (arm64
execution).

**Spec:** doc/superpowers/specs/2026-09-18-arm64-jit-parity-design.md
(Codex-reviewed, 3 rounds, all findings CLOSED; binding). The four
companion specs it names are binding for policy and semantics.

## Global Constraints

- NO changes under lib/VM/JIT/x86-64/, lib/VM/JIT/JitCompiler.cpp,
  JitHandlers.cpp, or include/hermes/VM/JIT/ — `git diff --stat` per
  task must show only lib/VM/JIT/arm64/, test/, utils/jit/, doc/.
- Tier dump comments EXACTLY as x86 emits them: "// Inline fast array
  load", "// Inline typed array load (kind %u)", "// Inline typed array
  store (kind %u)" (the relocated corpus pins them).
- Site helper slots initialized ONLY WHEN NULL (put and get families);
  indirect calls via `callRuntimeWithSavedIPIndirect`.
- INTEGER typed-array STORE conversion (spec §2): `fcmp d,d; b.vs
  helper` → `fabs dT, d; fcmp dT, d2p63; b.ge helper` (d2p63 = double
  2^63, bits 0x43E0000000000000) → `fcvtzs xT, d` (truncating, exact
  for the admitted domain) → store low bits. NO round-trip proof, NO
  sentinel. Domain must equal x86's: finite doubles in (−2^63, +2^63).
- Array-load hole check (spec §4): load raw bits INTO xRes via
  `emit_load_shv`, test emptiness through a DISTINCT temp
  (`emit_sh_ljs_is_empty(a, xTemp1, xRes)` on 8-byte slots; the
  non-writing `cmp` on 4-byte), decode xRes in place. NEVER
  `emit_shv_load_is_empty` in a load tier.
- xScratch is used by arm64's `emit_cmp_imm32` and mask
  materialization: tiers keep long-lived values (offset_, encoded
  values) in ALLOCATED temps, never in xScratch.
- Relocated tests: `git mv`, content unchanged; a pin that proves
  x86-specific is GENERALIZED (never duplicated per arch) and reported.
- Gates: arm64 jit suite on cmake-build-arm64 (HV64) per task, with
  cmake-build-host rebuilt FIRST (stale-host trap, dz 01a088bf);
  HV32/BOXED arm64 trees + `aarch64/qemu-sanity.sh` in the validation
  task; x86-64 jit suite on cmake-build-x86jit after every relocation
  (counts must not drop — relocated tests still run on x86).
  Pre-port baseline (measured 2026-09-18 at cf6b53a91): arm64 HV64 jit
  suite 60 pass / 69 unsupported / 0 fail.
- Prove-can-fail: restore AND REBUILD (arm64 tree) before the green
  re-run.
- Runtime/emitter C++ implementers invoke `gc-safe-coding` first.
- Spec and this plan stay UNCOMMITTED (fold at squash). Commits end
  with:
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01ExvqpAhy7pehcdZf34z3dB`.
- Builds: host `cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-host`;
  arm64 `cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-arm64 --target hermes`;
  suite `(cd /home/tmikov/work/hermes-x86-jit && LIT_FILTER='jit/' cmake --build cmake-build-arm64 --target check-hermes)`.
  Never bare `cd`; foreground commands with generous timeouts.

## File Structure

Task 1 (ById trigger + deterministic canon + ById corpus):
lib/VM/JIT/arm64/JitEmitter-property.cpp (putById cold append);
utils/jit/jit-canon.sh (new, the canonicalize() body) +
utils/jit/jit-dump.sh (calls it); test/jit/recompile-deterministic.js
(adopts it); git mv of the 8 ById recompile tests.
Task 2 (indirect call + records + PutByVal port): arm64
JitEmitter.{h,cpp} (callRuntimeIndirect/WithSavedIPIndirect,
byValSiteRecord), JitEmitter-property.cpp (putByValImpl restructure,
emitPutByValTypedArrayTier); git mv taval-conversions/guards +
recompile-taval-*; new test/jit/recompile-taval-fractional.js;
test/jit/putbyval-inline-emitted-arm64.js gains TA-store + indirect pins.
Task 3 (shared tail + GetByVal): JitEmitter-property.cpp
(emitTypedArrayElementLoad, emitGetByValFastArrayTier,
emitGetByValTypedArrayTier, getByValImpl), JitEmitter.h (GetByValRegs
etc.); git mv recompile-getval-*; new test/jit/
getbyval-inline-emitted-arm64.js + getval-conversions-emitted-arm64.js.
Task 4 (GetByIndex): JitEmitter-property.cpp (the ByIndex trio),
JitEmitter.h; git mv recompile-getidx-*; new
test/jit/getbyindex-inline-emitted-arm64.js.
Task 5 (validation + docs): arm64 HV32/BOXED suites, qemu-sanity, x86
unchanged proof, doc/JIT.md, doc/JITTesting.md, companion-spec pointers.

---

### Task 1: ById recompile trigger, deterministic-test canon, ById corpus

- [ ] **Step 1:** In arm64 `Emitter::putById` (JitEmitter-property.cpp
~1100-1120), inside the `cacheIdx != PROPERTY_CACHING_DISABLED` branch:
arm64 currently passes `cacheEntry->clazz.get(...)` STRAIGHT into
`initHCLazyIDMayAlloc`. Restructure it exactly as x86 does at
x86-64/JitEmitter-property.cpp ~2153-2163: hoist the class into a
local `HiddenClass *cachedClazz`, and `if (!cachedClazz)
coldWriteCacheIdxs_.push_back(cacheIdx);` BEFORE the
`initHCLazyIDMayAlloc(cachedClazz)` call. Cold means the cache names
no class — do NOT derive it from `clazzID == 0`, which is also what an
ID-space-exhausted WARM class returns; advertising such a site as
cold burns recompile budget on identical bodies (that is exactly what
recompile-hcid-exhausted.js pins). Copy x86's comment. The driver
already moves the vector at install (JitCompiler.cpp ~550).
EXECUTION NOTE: this step originally said "GetById stays
non-reporting on both backends" — false; x86's GetById emitter also
appends (`coldReadCacheIdxs_.push_back`, ~914). The Task 5 review
caught it and the Task 5 fix round ported that hunk too (its own
commit, `JIT: arm64 GetById cold-site reporting`).
- [ ] **Step 2:** Extract `canonicalize()`'s sed+awk body from
utils/jit/jit-dump.sh (~lines 156-171) into a new executable
utils/jit/jit-canon.sh (stdin→stdout; header comment: one source of
truth for dump canonicalization, consumed by jit-dump.sh and
recompile-deterministic.js). jit-dump.sh's canonicalize() becomes a
call to it (RAW path unchanged). Verify jit-dump.sh output is
byte-identical before/after on one corpus file.
- [ ] **Step 3:** `git mv` test/jit/x86-64/recompile-deterministic.js
to test/jit/ and change its two RUN pipelines from `sed 's/0x…//g'` to
`%S/../../utils/jit/jit-canon.sh` (verify the relative path resolves
from test/jit/ under lit — `%S` is the test's directory). Prove-can-
fail on BOTH backends: with a scratch emitter change that alters
version-2 emission between the two runs (e.g., an extra nop under an
env var is too invasive — instead run once with `-Xjit-emit-counters`
off and once on if that changes dump text; otherwise pass a different
`-Xjit-recompile-threshold` to the second run so it never recompiles
and confirm `diff` fails) — the point is that canonicalization does
not hide a real structural difference; state exactly which lever you
used.
- [ ] **Step 4:** `git mv` the other EIGHT ById tests
(recompile-byid-warm, -cold-sites, -hcid-exhausted, -mid-recursion,
-phased-warming, -staleness-budget, -switch-old-frames,
-threshold-flag — nine total with deterministic) to test/jit/.
Relocation inventory for the whole plan: 9 ById + 15 taval + 10
getval + 5 getidx = 39 files; eight of them carry `UNSUPPORTED:
gc_malloc` and several `UNSUPPORTED: handle_san` — all preserved.
Content unchanged; they pin only "// Put to object specialization",
version banners, counters, and printed values (verified at plan time).
- [ ] **Step 5:** Gates: host rebuild → arm64 build → arm64 jit suite:
all nine relocated tests, deterministic included, PASS on arm64 (the trigger now fires —
recompile-byid-warm's version-2 "Put to object specialization" pin is
the direct proof); x86 jit suite on cmake-build-x86jit unchanged
counts. Commit — `JIT: arm64 ById recompile trigger; arch-neutral
recompile corpus`

### Task 2: Indirect call, site records, PutByVal recording + typed-array store tier

- [ ] **Step 1:** arm64 JitEmitter.{h,cpp}: `callRuntimeIndirect(uint64_t
slotAddr, const char *name)` = `loadBits64InGp(xScratch, slotAddr,
"helper slot address"); a.ldr(xScratch, a64::Mem(xScratch));
a.blr(xScratch)`; `callRuntimeWithSavedIPIndirect` wraps it exactly as
`callRuntimeWithSavedIP` wraps `callRuntime` (IP save, emitAsserts_
invalidation). `byValSiteRecord(uint32_t siteId)` member identical to
x86's (JitEmitter.h ~586).
- [ ] **Step 2:** `putByValImpl` restructured to x86's shape
(x86-64/JitEmitter-property.cpp ~386-520, read it fully): siteId =
bytecode offset; record consult with `isJitSupportedTypedArrayStoreKind`;
`emitPutByValTypedArrayTier` FIRST when the record holds a kind (kind
miss → the fast-array tier's label), `specializedTAKind` recorded;
fast-array tier under HERMES_JIT_INLINE_SAFE_STORE unchanged; slow
path = 6-arg recording call (x0 runtime, x1 target, x2 key, x3 value,
x4 versionData_, w5 siteId) through `&site.helper` initialized ONLY
WHEN NULL to `_jit_put_by_val_{loose,strict}` by `strict`, via
`callRuntimeWithSavedIPIndirect`; the `_FnT` signature typedef check
as x86 does. The TA tier is NOT gated by HERMES_JIT_INLINE_SAFE_STORE
(typed-array storage holds no GC pointers — same as x86).
- [ ] **Step 3:** `emitPutByValTypedArrayTier` per spec §2, modeled on
x86's (~201-385) with arm64 idioms: guard order object/exact-kind
(miss→kindMissLab)/flags-mask(as the arm64 fast-array tier does it,
xScratch for the mask only)/VALUE CONVERSION/key/bounds/base/store.
Integer kinds per Global Constraints (fcmp-b.vs, fabs-fcmp-b.ge vs a
d2p63 loaded once via loadBits64InGp+fmov into an allocated VecD
temp, fcvtzs). Float kinds: `fcmp d,d; b.vs helper`; Float32 `fcvt
sT, d`. Key `emit_double_is_uint32` + `b.ne` only. Bounds `ldr
wT,[loc,#length]; cmp wIdx, wT; b.hs helper`. Base: offset_ into an
ALLOCATED wTemp, `emit_load_cp` buffer, decode, `ldr loc,[loc,#data]`,
`cbz loc, helper`, `add loc, loc, xOff` (uxtw). Stores `strb/strh/str
wT` with `[loc, xIdx, uxtw #logWidth]`; `str sT`/`str d`. Comment
EXACTLY "// Inline typed array store (kind %u)". Check every
RuntimeOffsets field the tier loads against `maxNaturalBaseOffset`
with static_asserts as the fast-array tier does.
- [ ] **Step 4:** Tests: `git mv` taval-conversions.js, taval-guards.js,
and all recompile-taval-*.js to test/jit/ (content unchanged). NEW
test/jit/recompile-taval-fractional.js (arch-neutral). Recipe
(counters count threshold CROSSINGS, not calls — `NumRecompileChecks`
increments in considerRecompile when declines reach the threshold,
JitCompiler.cpp ~338): two INDEPENDENT store functions (Int32Array,
Uint8Array), each in its own function so no other decline source
shares its counter, warmed with the warm-prefix/variable-suffix
technique of recompile-taval-demote-carried.js (~19) at
`-Xjit-recompile-threshold=64` until each specializes (dump pins
version 2 + "// Inline typed array store" per function; exactly TWO
checks expected at that point); then a suffix of SEVERAL HUNDRED
fractional / negative-fractional stores per function — long enough
that a defective conversion declining every store would cross the
threshold several more times. Pins: `NumRecompileChecks: 2` (unchanged
by the suffix), `NumByValDemotions: 0`, and the truncated stored
values vs the interpreter. Verify it passes on x86 FIRST (it
documents x86's domain), then arm64.
test/jit/putbyval-inline-emitted-arm64.js: REPLACE its existing
direct-helper assertion (`// SPEC: call _sh_ljs_put_by_val_loose_rjs`
at ~line 165, with its "unchanged helper call" comment at ~163 — both
become false once the slow path records) with the indirect recording
call pin (`ldr x16, [x16]` / `blr x16` — pin the register asmjit
actually prints for xScratch; the printed comment names
`_jit_put_by_val_loose [indirect]` as x86's does), and ADD a
specialized-site section
pinning the TA store tier's integer NaN guard (`fcmp`/`b.vs`),
magnitude guard (`fabs`/`fcmp`/`b.ge`), truncating `fcvtzs`, a scaled
store, and the float-kind exit; anchor per the getval-conversions-
emitted.js hazards note (positional anchors, no floating patterns).
- [ ] **Step 5:** Prove-can-fail (arm64 tree, restore + REBUILD):
(1a) drop the integer NaN guard — taval-conversions' `I32 nonnum
valueOf` case fails by name ("valueOf called" missing); (1b) drop the
magnitude guard — the Infinity / 2^63 store cases fail by name.
- [ ] **Step 6:** Gates: arm64 jit suite (all taval tests + fractional
+ pins green); x86 jit suite unchanged counts + the fractional test
green there. Commit — `JIT: arm64 PutByVal shape feedback and
typed-array store tier`

### Task 3: Shared element-load tail + GetByVal tiers

- [ ] **Step 1:** `emitTypedArrayElementLoad(CellKind, const a64::Mem &,
xRes, xTemp1, dValTmp, doneLab)` per spec §3, with x86's contract
(allocates nothing; seam after base formation, before the undefined
block): per-kind `ldrsb/ldrb/ldrsh/ldrh/ldr w` → `scvtf`; Uint32 `ldr
w` → `ucvtf`; Float32 `ldr s` → `fcvt d,s`; Float64 `ldr d`; float
kinds `fcmp dT,dT; b.vc L; loadBits64InGp(xTemp1, canonicalNaN);
fmov dT, xTemp1; L:`; `fmov xRes, dT`. The Mem carries a register
index with `uxtw #shift` (ByVal) or an immediate (ByIndex); assert
the immediate form fits the scaled unsigned offset for the width.
asmjit a64 facts (verified against the vendored assembler during
plan review): one `a64::Mem` carries both shapes (armoperand.h ~207);
the memory encoder accepts a W index with UXTW including shift 0 for
byte accesses (a64assembler.cpp ~483/~2352); but `add` with an
extended register requires X-typed operands (~1230), so the base
formation keeps `add xLoc, xLoc, xOff` in X form. For ByVal pass
`a64::Mem(xBase, wIdx, a64::uxtw(logWidth))` and for ByIndex
`a64::Mem(xBase, K * width)`; do NOT also apply the JSArray tier's
separate scaled `add` (its ~line 219) — the scaling lives in the Mem.
- [ ] **Step 2:** `emitGetByValFastArrayTier` per spec §4 from the
arm64 fast-array STORE tier's guard sequence minus flags/jumbo/encode,
with the load-into-xRes + distinct-temp hole check (Global
Constraints) and `Emit_sh_shv_decode`. `emitGetByValTypedArrayTier`:
kind guard, key, bounds `b.hs undef`, base with `cbz loc, undef`,
shared tail, undefined path `loadBits64InGp(xRes, undefinedBits)`.
`getByValImpl` mirroring x86's ~1519-1665: result register allocated
in the impl (prefer x0), tier order TA-then-JSArray, JSArray
unconditional (no gate), 5-arg recording slow path (`_jit_get_by_val`,
x3 versionData_, w4 siteId) through the null-only slot,
`callRuntimeWithSavedIPIndirect`, result from x0. Comments EXACTLY
"// Inline fast array load" / "// Inline typed array load (kind %u)".
- [ ] **Step 3:** Tests: `git mv` all recompile-getval-*.js to
test/jit/ (getval-conversions/guards/getbyval-inline already there).
NEW test/jit/getbyval-inline-emitted-arm64.js (JSArray tier: kind
guard, the distinct-temp `asr`/`cmn` empty check, NO flags compare;
indirect-call pin) and test/jit/getval-conversions-emitted-arm64.js
(nine version-2 kind pins: tier line + the distinguishing load/convert
— `ldrsb`+`scvtf`, `ldrb`, `ldrsh`, `ldrh`, `ldr w`+`scvtf`, `ldr
w`+`ucvtf`, `ldr s`+`fcvt`, `ldr d`+`fcmp`/`b.vc` — per-kind warmed
functions, positional anchors).
- [ ] **Step 4:** Prove-can-fail (arm64): drop the tail's NaN
canonicalization — getval-conversions' Object.is oracle fails by name
(getidx's joins in Task 4 as the sharing proof); mutate the hole check
to use the same register for temp and input — the flags survive, so
actual holes STILL decline correctly; what breaks is every NON-hole
element, whose bits are now the shifted ETag: getval-guards' `dense
0` / `dense 3` cases (values 10 / 40) fail by name. This is an
8-byte-slot mutation, so it fails on the HV64 gate tree (and BOXED);
HV32's non-writing `cmp` is unaffected — run it on HV64.
- [ ] **Step 5:** Gates: arm64 jit suite; x86 unchanged. Commit —
`JIT: arm64 GetByVal tiers and the shared element-load tail`

### Task 4: GetByIndex tiers

- [ ] **Step 1:** The ByIndex trio per spec §5, mirroring x86's
~1671-2016: `emitGetByIndexFastArrayTier` (`mov wIdx, #K; ldr
wT,[loc,#beginIndex_]; sub wIdx,wIdx,wT; ldr wT,[loc,#elemCount_]; cmp
wIdx,wT; b.hs helper`; storage; load-into-xRes + distinct-temp hole
check; decode), `emitGetByIndexTypedArrayTier` (kind guard; `ldr
wT,[loc,#length]; cmp wT, #K; b.ls undef`; base with `cbz`; shared
tail with the IMMEDIATE Mem `[loc, #K*width]`), `getByIndexImpl`
(5-arg `_jit_get_by_index`, key immediate in w2, x3 versionData_, w4
siteId; null-only slot; TA-first selection with the LOAD predicate).
- [ ] **Step 2:** Tests: `git mv` all recompile-getidx-*.js to
test/jit/. NEW test/jit/getbyindex-inline-emitted-arm64.js (immediate
forms `mov w, #K` / `sub` / `cmp w, #K` / `b.ls`, the constant-
displacement element load `[x, #K*width]`, indirect-call pin, all
nine kind pins per the getval pattern).
- [ ] **Step 3:** Prove-can-fail (arm64): (1) drop the shared tail's
NaN canonicalization — BOTH getval-conversions AND getidx-conversions
fail by name (sharing proof on arm64); (2) TA bounds `b.ls` → `b.lo`
(inclusive) — getidx-guards' planted K==length cases fail.
- [ ] **Step 4:** Gates: arm64 jit suite; x86 unchanged. Commit —
`JIT: arm64 GetByIndex tiers`

### Task 5: Cross-mode validation and docs

- [ ] **Step 1:** Rebuild + jit suite green on cmake-build-arm64-hv32
and cmake-build-arm64-boxed; `aarch64/qemu-sanity.sh <dir>` on all
three arm64 trees; record the new arm64 counts vs the pre-port
baseline (60/69/0 on HV64) — the unsupported count must drop by
exactly the relocated-file count, and NO new failures.
- [ ] **Step 2:** x86 proof: full x86 ASan suite + jit suite on
cmake-build-x86jit green with the expected count delta: every
relocated test still runs (moved, not removed), PLUS ONE pass for the
new arch-neutral fractional test, PLUS THREE unsupported for the new
arm64-only pin files; `git diff --stat <pre-port-tip>` shows
nothing under lib/VM/JIT/x86-64/ or the shared driver.
- [ ] **Step 3:** Docs: doc/JIT.md — delete the recompilation dormancy
paragraph (~717) and the ByVal/GetByVal/GetByIndex "arm64 stays on
the helper" notes; describe both backends as implementing the policy;
extend the emitted-pin test table with the three new arm64 files.
doc/JITTesting.md — arm64 gate counts, and that the recompile/ByVal
corpus now runs on every JIT architecture; note utils/jit/jit-canon.sh.
The four companion specs' Delivered sections each gain one line:
"Ported to arm64 2026-09-18: see 2026-09-18-arm64-jit-parity-design.md".
Spec+plan uncommitted.
- [ ] **Step 4:** Commit — `docs: arm64 JIT parity delivered`

## Self-Review Notes (plan time)

- Spec coverage: §0 (T1), §1-2 (T2), §3-4 (T3), §5 (T4), §6 needs no
  code (verified: driver is shared), Tests incl. deterministic canon +
  fractional test + arm64 pins (T1-T4), Docs (T5). Non-goals honored.
- Every task ends with the arm64 suite AND an x86 unchanged check;
  the x86-untouched invariant is a per-task `git diff --stat` gate.
- The three spec-critical hazards (integer-store domain; hole-check
  clobber; xScratch discipline) are in Global Constraints AND the
  owning task steps with named prove-can-fail checks.
- Delegated with instructions: the exact cold-cache branch in arm64
  putById, the jit-canon.sh path substitution under lit, the
  deterministic test's prove-can-fail lever, register choices within
  the arm64 temp pools — each "match the sibling and state which".
