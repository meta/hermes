# arm64 JIT: functional parity with x86-64 — design

Date: 2026-09-18. Status: draft, pre-review.
Scope: the arm64 backend (lib/VM/JIT/arm64/) gains every emitter-side
capability the x86-64 backend acquired since the two diverged, so that
both backends implement the same policy and pass the same behavioral
test corpus. The shared driver (JitCompiler.cpp, JitHandlers.cpp,
JitFunctionData.h) already implements everything arch-independently
and is NOT changed. x86-64 emission is NOT changed (mirrored-file
discipline: only arm64 files, shared tests, and docs move).

Binding companions (shipped designs the port implements on arm64):
- 2026-09-04 recompilation + 2026-09-07 version-data (ById trigger)
- 2026-09-08 putbyval-typed-array + 2026-09-09 monotone-demotion
- 2026-09-11 getbyval (+ its perf diagnosis)
- 2026-09-12 getbyindex (incl. the shared element-load tail)
Their policy, records, and semantics sections apply verbatim; this
document specifies only what is arm64-specific.

## The gap (measured from the code, 2026-09-18)

| capability | x86-64 | arm64 today |
|---|---|---|
| ById recompile TRIGGER (cold-site reporting) | live: PutById appends the cold cacheIdx (JitEmitter-property.cpp ~2163) AND GetById appends to `coldReadCacheIdxs_` (~914) | DORMANT: both lists exist, neither appended (doc/JIT.md ~717); `_jit_put_by_id` IS called with version data |
| indirect (demotable) helper call | `callRuntimeIndirect` / `WithSavedIPIndirect` | absent |
| per-site ByVal records (`byValSiteRecord`) | Emitter member | absent |
| PutByVal recording slow path + demotion | via slot | direct `callRuntimeWithSavedIP` |
| PutByVal typed-array STORE tier | 8 kinds | absent (fast-array store tier present, under HERMES_JIT_INLINE_SAFE_STORE) |
| GetByVal JSArray + TA load tiers | shipped | bare helper |
| GetByIndex JSArray + TA tiers | shipped | bare helper |
| shared `emitTypedArrayElementLoad` tail | shipped | absent |
| tier dump comments | "// Inline fast array load", "// Inline typed array load (kind N)", "// Inline typed array store (kind N)" | only "// Inline fast array store" / "// Put to object specialization" |
| policy/semantics tests | 37 recompile-* + taval/getval/getidx in test/jit/x86-64/ (x86-gated by DIRECTORY only) | none run |

The shared driver already: pools declines, recompiles, selects tiers
from records via each emitter's own predicate check, demotes through
`isRecordingHelper`/`demoteSite`, sweeps on retirement, counts
`recordingByValSites` at install, and prints the "JIT ByVal sites: N
observed, M specialized" banner (JitCompiler.cpp ~632). None of it is
arch-gated. arm64 gets all of it the moment its emitter installs
recording helpers in site slots and emits the tiers.

## What "parity" means here

Functional parity = same policy, same semantics, same observable
recompile/demotion behavior, same test corpus. NOT byte-level or
performance parity: arm64 runs only under qemu-user in this tree, where
timings are meaningless (aarch64/README.md "Limitations"), so the port
is correctness-only, as the inline-writes arm64 port was; the x86-64
measured numbers stand as the expectation, and emission is verified by
inspection (dump pins) rather than A/B.

## Design, per capability

### 0. ById recompile trigger

Mirror the single x86 append site: when the PutById emitter finds the
property cache entry cold at compile time (the same condition under
which x86 pushes `cacheIdx` at JitEmitter-property.cpp ~2163), push it
onto arm64's existing `coldWriteCacheIdxs_`. The driver already copies
the list into `JitVersionData::coldByIdSites` at install. This is the
change that makes `considerRecompile`'s gate fire on arm64 at all.
CORRECTION (found by the Task 5 review, 2026-09-18): this section
originally claimed "GetById stays non-reporting on both backends" —
FALSE. x86-64's GetById emitter also appends cold read-cache indices
(`coldReadCacheIdxs_.push_back`, x86-64/JitEmitter-property.cpp
~914, landed with the recompilation mechanism). The gap-assessment
grep had only looked at the write list. Parity therefore requires
the same one-hunk mirror in arm64's GetById emitter, which the Task 5
fix round adds as its own commit.

### 1. Indirect helper call

`callRuntimeIndirect(slotAddr, name)`: `loadBits64InGp(xScratch,
slotAddr)`; `ldr xScratch, [xScratch]`; `blr xScratch`.
`callRuntimeWithSavedIPIndirect` wraps it with the same IP save (and
`emitAsserts_` invalidation) as `callRuntimeWithSavedIP`. Both are
straight-line and clobber only xScratch, matching the direct forms'
contract.

### 2. Site records and the PutByVal port

- `byValSiteRecord(siteId)` member on the arm64 Emitter, identical to
  x86's (find-or-create in `versionData_->consumerRecords`).
- `putByValImpl` gains the x86 structure: site record consultation
  (bytecode offset as siteId), TA tier emitted first when the record
  holds a store-supported kind (kind miss chains into the fast-array
  tier), fast-array tier under `HERMES_JIT_INLINE_SAFE_STORE` exactly
  as today, then the slow path as the 6-argument recording call through
  the per-site slot initialized ONLY WHEN NULL to
  `_jit_put_by_val_{loose,strict}`, via `callRuntimeWithSavedIPIndirect`.
  Argument registers x0-x5 (runtime, target, key, value, versionData,
  siteId); the demoted 4-argument plain helper ignores x4/x5 (AAPCS64:
  extra integer arguments are simply unread, the same compatibility the
  x86 slots rely on for SysV).
- `emitPutByValTypedArrayTier`, 8 kinds as x86 (Int8/16/32, Uint8/16/32,
  Float32/64; Uint8Clamped/Float16/BigInt decline and record "other"),
  with the arm64 conversion idioms in place of x86's:
  - Guard order as x86: object, exact kind (miss → kindMissLab), flags
    mask (fastIndexProperties set, frozen clear), THEN the value
    conversion, then key, bounds, base.
  - INTEGER kinds — the accepted domain must EQUAL x86's, or the two
    backends' demotion behavior diverges (x86 stores a fractional
    value like 1.5 inline as 1; a round-trip proof would decline it,
    and a site fed fractions forever would keep declining, counting,
    and eventually demoting on arm64 while staying silent on x86).
    x86 accepts every double except NaN (all non-numbers), ±Infinity,
    |x| ≥ 2^63, and −2^63 exactly (its sentinel). arm64 reproduces
    exactly that set with two exits and a truncation: `fcmp dValue,
    dValue; b.vs helper` (unordered = NaN = every non-number); `fabs
    dTmp, dValue; fcmp dTmp, d2p63; b.ge helper` where d2p63 holds
    2^63 as a double (bits 0x43E0000000000000, materialized once per
    tier via loadBits64InGp + fmov — GE rejects ±Infinity, |x| > 2^63,
    and both ±2^63 exactly, matching x86, whose vcvttsd2si yields the
    integer-indefinite sentinel for +2^63 and whose sentinel compare
    rejects the validly converted −2^63); then `fcvtzs xT, dValue`,
    which for every admitted |x| < 2^63 is a correct truncation toward
    zero with no saturation (fractions are discarded, as ToInt32
    requires), so the LOW 32 BITS are truncateToInt32()'s result. NO round-trip proof here (that is the
    key/index idiom, where exactness IS the requirement) and no
    x86-style sentinel compare (saturation makes a sentinel unsound
    on arm64).
  - FLOAT kinds: `fcmp dValue, dValue; b.vs helper` (V set = unordered
    = any NaN pattern, which every non-number is, and which the helper
    must canonicalize — the mirror of x86's parity exit); Float32 then
    `fcvt sTmp, dValue` and `str sTmp`.
  - Key: `emit_double_is_uint32` + `b.ne` (one exit, as the arm64
    fast-array tier documents); no 0xFFFFFFFF exclusion (bounds
    rejects it).
  - Base: `ldr wOff, [loc, #offset_]` into a REAL allocated temp (NOT
    xScratch — unlike x86, arm64's `emit_cmp_imm32`/mask sequences use
    xScratch; keep it out of long live ranges), `emit_load_cp` of the
    buffer, `emit_sh_cp_decode_non_null`, `ldr loc, [loc, #data_]`;
    `cbz loc, helper` (detached); `add loc, loc, wOff (uxtw)`.
  - Store: `str{b,h,} wT, [loc, wIdx, uxtw #logWidth]` / `str sTmp` /
    `str dValue` — the arm64 scaled register-offset forms.
  - Comment string EXACTLY "// Inline typed array store (kind %u)".

### 3. The shared element-load tail on arm64

`emitTypedArrayElementLoad(kind, a64::Mem, xRes, xTemp1, dValTmp,
doneLab)` with the x86 contract (allocates nothing; caller-owned
scratches; seam after the base is formed, before the undefined block;
branches to the caller's doneLab). Per-kind arm64 body:
- Int8 `ldrsb wT` / Uint8+Uint8Clamped `ldrb wT` / Int16 `ldrsh wT` /
  Uint16 `ldrh wT` / Int32 `ldr wT` → `scvtf dTmp, wT`; Uint32 `ldr
  wT` → `ucvtf dTmp, wT` (arm64 has the unsigned convert directly —
  no 64-bit detour); Float32 `ldr sTmp` → `fcvt dTmp, sTmp`; Float64
  `ldr dTmp`.
- Float kinds canonicalize: `fcmp dTmp, dTmp; b.vc encode;
  loadBits64InGp(xT, canonicalQuietNaNBits); fmov dTmp, xT` (the
  b.vc skips the replace on ordered). Integer kinds skip it.
- Encode as a number HV into xRes (`fmov xRes, dTmp` — HV64 identity
  encoding, number HVs are raw doubles in every heap mode of the frame).
The Mem parameter carries either a register index (ByVal: `[base,
wIdx, uxtw #shift]`) or an immediate (ByIndex: `[base, #K*width]`);
both are natural AArch64 addressing modes for every width (K*width ≤
2040 fits the unsigned scaled immediate).

### 4. GetByVal tiers

`getByValImpl` mirroring x86's: allocate the result register in the
impl (rax-hint analogue: prefer x0 so the helper's return needs no
move); TA tier first when specialized (load predicate), kind miss
chains to the JSArray tier; JSArray tier UNCONDITIONAL (no
HERMES_JIT_INLINE_SAFE_STORE gate — loads have no barrier); slow path =
5-argument recording call through the slot (`_jit_get_by_val`),
`callRuntimeWithSavedIPIndirect`, result from x0.

`emitGetByValFastArrayTier`: the arm64 fast-array STORE tier's guard
sequence (object, exact JSArrayKind, key uint32 round-trip + cmn
0xFFFFFFFF, begin-relative range check, storage load) MINUS the flags
check (per the C++ authority: empty subsumes it), MINUS the jumbo-cell
check (a read needs no card), MINUS the encode; then the hole check
done the way the x86 load tier does it and NOT via
`emit_shv_load_is_empty`: load the element's raw bits INTO xRes
(`emit_load_shv`), then test emptiness through a DISTINCT temp —
8-byte slots: `emit_sh_ljs_is_empty(a, xTemp1, xRes)` (its input is
preserved when the temp differs; with temp == input it overwrites the
value with the shifted ETag, which is exactly what
`emit_shv_load_is_empty`'s 8-byte branch does, so reusing it would
decode garbage under HV64/BOXED); 4-byte slots: the `cmp` against the
encoded empty value writes nothing — `b.eq helper` (hole), then
`Emit_sh_shv_decode` (arm64 has it) on xRes in place. The same rule
binds the GetByIndex fast-array tier. Comment EXACTLY
"// Inline fast array load".

`emitGetByValTypedArrayTier`: exact-kind guard; key; `ldr wT,
[loc,#length]; cmp wIdx, wT; b.hs undef`; base as in the store tier
with `cbz loc, undef` (detached → undefined, NOT helper, NO
recording); shared tail with the register-index Mem; undefined path:
`loadBits64InGp(xRes, undefinedBits)` and fall to doneLab. Comment
EXACTLY "// Inline typed array load (kind %u)".

### 5. GetByIndex tiers

`getByIndexImpl` as x86's (key immediate into w2 of the 5-argument
`_jit_get_by_index` call). Fast-array tier: `mov wIdx, #K; ldr wT,
[loc,#beginIndex_]; sub wIdx, wIdx, wT; ldr wT,[loc,#elemCount_]; cmp
wIdx, wT; b.hs helper` (begin-relative; a constant K does not make the
storage offset constant); TA tier bounds `ldr wT,[loc,#length]; cmp
wT, #K; b.ls undef` (K ≤ 255 is always an add/sub immediate); shared
tail with the immediate Mem.

### 6. Demotion, retirement, accounting

No arm64 code: the driver's demotion pass flips the slots the arm64
emitter installed, the retirement sweep flips still-recording slots,
and `recordingByValSites` is derived at install by counting
`isRecordingHelper` slots. The only emitter obligations are the
null-only slot initialization (both put and get families) and
emitting the indirect call through `&site.helper` — the address is
stable because `byValSites` is a deque.

## Tests

The decisive property, verified from the files: every policy and
semantics test in test/jit/x86-64/ for the ById-recompile, PutByVal,
GetByVal, and GetByIndex families pins ONLY shared-driver banners
("JIT compilation of FunctionID … (version 2)", "JIT ByVal sites: …"),
tier COMMENT strings, `-Xjit-emit-counters` counters, and bytecode
dumps — never instruction text. With arm64 emitting identical comment
strings, these tests are architecture-neutral by construction.

- RELOCATE (git mv, content unchanged unless a pin proves x86-specific,
  which must be reported and fixed by generalizing the pin, never by
  duplicating the file): recompile-byid-warm, recompile-cold-sites,
  recompile-deterministic, recompile-hcid-exhausted,
  recompile-mid-recursion, recompile-phased-warming,
  recompile-staleness-budget, recompile-switch-old-frames,
  recompile-threshold-flag, all recompile-taval-*, all
  recompile-getval-*, all recompile-getidx-*, taval-conversions,
  taval-guards → test/jit/. (getval-*/getidx-* semantics files already
  live there.) Any `UNSUPPORTED: gc_malloc` lines ride along
  unchanged (arm64 has no MallocGC tree; the feature is simply absent
  there).
- ONE relocated test needs its normalization generalized first:
  `recompile-deterministic.js` diffs two full `-Xdump-jitcode=3` runs
  after stripping hex literals, which is sufficient on x86-64
  (loadBits64InGp there is always `mov reg, imm64`) but NOT on arm64,
  where `isCheapConst()` (arm64/JitEmitter.h ~1787) picks `mov`/`movk`
  versus an RO-data `ldr` by the VALUE of the constant — and ASLR moves
  the embedded runtime addresses between the two processes, so the
  instruction SHAPE differs run to run and hex stripping cannot hide
  it. utils/jit/jit-dump.sh already canonicalizes exactly this
  (constant materializations → a CONST token, RO_DATA contents
  dropped, wide hex → ADDR). Before relocation the test adopts that
  canonicalization (share the sed pipeline — extract it into a small
  utils/jit/ helper both consumers invoke, or replicate it verbatim
  with a pointer), and its prove-can-fail (a deliberate emission
  change between the two runs must still diff) is re-run on BOTH
  backends so the x86 comparison keeps its strength.
- NEW arch-neutral test for the store-domain parity property
  (Codex finding): `recompile-taval-fractional.js` — a specialized
  Int32Array (and a Uint8Array) store site fed ONLY fractional and
  negative-fractional values after specialization pins, via
  `-Xjit-emit-counters`, NumByValDemotions: 0 and NumRecompileChecks
  unchanged from the warm-up count, plus the stored (truncated)
  values against the interpreter. It runs on x86 today and is the
  named check the arm64 integer-store domain must pass.
- STAY x86-only: the *-emitted.js instruction pins
  (putbyid/putbyval/getbyval/getbyindex-inline-emitted,
  getval-conversions-emitted, imul-emitted) and the bring-up tests.
- NEW arm64 pins, following the existing *-emitted-arm64.js pattern
  (`REQUIRES: jit-arch-arm64`, `UNSUPPORTED: handle_san`): the
  indirect recording call (`ldr x16, [x16]` + `blr x16` — pin the
  actual scratch register asmjit prints), the TA store tier (the
  integer NaN guard `fcmp`/`b.vs`, the magnitude guard `fabs`/`fcmp`/
  `b.ge`, the truncating `fcvtzs`, the float-kind `fcmp`/`b.vs` exit,
  a scaled store), the JSArray load tier (kind guard, empty check, no flags
  compare), a TA load tier per kind (nine `Inline typed array load`
  pins with the distinguishing load/convert — Uint32's `ucvtf`,
  Float32's `fcvt`, Float64's canonicalization `fcmp`/`b.vc`), and
  the GetByIndex immediate forms (`sub`/`cmp` with `#K`).
- Prove-can-fail on arm64 (restore + REBUILD each time; the qemu suite
  is the runner): (1a) drop the TA store tier's integer NaN guard —
  the named check must store a NON-NUMBER that needs conversion
  (taval-conversions' existing `I32 nonnum valueOf` case, whose
  "valueOf called" print an inline store would skip, plus its
  null/undefined cases; a numeric NaN alone converts to 0 correctly
  even without the guard, so it cannot be the oracle); (1b) drop the magnitude guard — the
  Infinity / 2^63 store cases fail by name; (2) drop
  the load tail's NaN canonicalization — getval-conversions' AND
  getidx-conversions' Object.is oracles fail (the sharing proof on
  arm64); (3) change a TA bounds condition to inclusive — the planted
  K==length cases fail.
- Gates: arm64 jit suite green on all three arm64 trees (HV64, HV32,
  BOXED — cmake-build-arm64{,-hv32,-boxed}), rebuilding
  cmake-build-host FIRST (the stale-host-compilers trap, dz
  01a088bf); `aarch64/qemu-sanity.sh` on each; x86-64 jit suite green
  and its counts unchanged on cmake-build-x86jit (the relocation must
  not lose any x86 coverage: the relocated tests still run there);
  `git diff --stat` shows NO changes under lib/VM/JIT/x86-64/ or the
  shared driver.

## Non-goals

- Performance measurement on arm64 (qemu); no benchmark changes.
- Extending anything beyond x86 parity (no BigInt tiers, no MallocGC
  arm64 tree). GetById cold-site reporting turned out to BE x86 parity,
  not an extension — see the correction at §0 above; it is in scope
  and landed in the Task 5 fix round.
- Backend unification / engine extraction: the mirrored-file pairs stay
  mirrored (user decision on record: rule of three, drift managed by
  diffing).
- The x86-only bring-up tests stay where they are.

## Docs

doc/JIT.md: delete the three dormancy statements (recompilation ~717,
and the ByVal/GetByVal/GetByIndex "arm64 stays on the helper" notes)
and describe both backends as implementing the policy; the emitted-pin
test table gains the new arm64 files. doc/JITTesting.md: the arm64
gates' expected counts, and a note that the relocated corpus now runs
on every JIT architecture. The dormancy sentence in each of the four
companion specs' Delivered sections gets a one-line "ported to arm64
2026-09-18" pointer to this document.

## Delivered (2026-09-18, Task 5)

All five tasks landed on branch `x86-jit`, tip `98c6370b8` going into
Task 5 (Tasks 1-4: `90fe1fc22`, `06a1cfd70`, `bc2f98c03`, `98c6370b8`).
Task 5 (this document's own task) added no new emitter code -- scope
was cross-mode validation, the comment fix deferred from Task 2's
review, the jit-dump.sh corpus fix, baseline rerolls, and docs.

**Cross-mode validation (Step 1).** `cmake-build-host` rebuilt first
(stale-host trap), then each of the three arm64 trees rebuilt and
gated:

| tree | jit suite | qemu-sanity |
|---|---|---|
| `cmake-build-arm64` (HV64) | 103 pass / 30 unsupported / 0 fail | 9/9 |
| `cmake-build-arm64-hv32` | 103 pass / 30 unsupported / 0 fail | 9/9 |
| `cmake-build-arm64-boxed` | 103 pass / 30 unsupported / 0 fail | 9/9 |

Matches the brief's authoritative expectation exactly: up from the
pre-port 60 pass / 69 unsupported baseline (60 -> 103 = +43 net after
new arm64-only pin files; 69 -> 30 = -39, exactly the relocated-file
count), zero failures on any tree in any mode.

**x86 proof (Step 2).** `cmake-build-x86jit` rebuilt; full ASan suite
(`check-hermes`, no filter): 4248 pass / 6 expected fail / 157
unsupported / 0 unexpected failures. Jit suite (`LIT_FILTER='jit/'`):
126 pass / 7 unsupported / 0 fail -- the expected delta from the
pre-port 125/4 (+1 pass: `recompile-taval-fractional.js`; +3
unsupported: the three new arm64-only pin files), with every relocated
test confirmed still running on x86 (`recompile-` filter: 38 pass;
the taval/getval/getidx/inline-emission filter: 41 pass / 4
unsupported). Scope diff:
`git diff --stat cf6b53a91 HEAD -- lib/VM/JIT/x86-64 lib/VM/JIT/JitCompiler.cpp lib/VM/JIT/JitHandlers.cpp include/hermes/VM/JIT`
is EMPTY.

**Baseline reroll and corpus fix (Step 3).** `utils/jit/jit-dump.sh`'s
default-corpus loop now skips any file whose RUN lines all pass
`-Xhermes-internal-test-methods` (`getidx-guards.js`,
`getval-guards.js`, and `taval-guards.js` -- all three call
`HermesInternal.detachArrayBuffer`, which throws without that flag
before any JIT code is emitted, so the file previously aborted the
capture). Both stored baselines (`cmake-build-arm64/jit-baseline.dump`,
`cmake-build-x86jit/jit-baseline.dump`) rerolled at this tip and
verified deterministic (a second immediate capture matches byte for
byte): arm64's 352589 lines / 92% canonicalized, x86-64's 366910
lines / 100% verbatim (the corpus fix alone changed nothing else on
x86-64, which has no `isCheapConst()` split to canonicalize).

**Comment fix (deferred from Task 2 review).**
`lib/VM/JIT/arm64/JitEmitter.h`'s `coldWriteCacheIdxs_`/
`coldReadCacheIdxs_` doc comments, which said the recompilation
mechanism was dormant on arm64, now mirror x86-64's wording for the
same two members verbatim. Rebuilt and re-verified green on all three
arm64 trees after the change (comment-only; no emission difference
expected or found).

**Docs (Step 4).** doc/JIT.md: the recompilation dormancy paragraph
replaced with a both-backends-implement-the-policy statement; the
PutByVal typed-array section gained the arm64 idiom paragraph (fcmp/
b.vs + fabs/fcmp-2^63/b.ge vs. x86's sentinel, same accepted domain;
single-exit key check; `offset_` in a real temp); the GetByVal/
GetByIndex Tests paragraphs' stale `test/jit/x86-64/recompile-*`
paths corrected to their current arch-neutral `test/jit/` location;
the emitted-pin test bullet list extended with the three new arm64
files and the un-gated `handle_san` argument (only
`getval-conversions-emitted-arm64.js` carries the gate;
`getbyval-inline-emitted-arm64.js` and
`getbyindex-inline-emitted-arm64.js` do not, matching Task 3's
fix-round finding that loads have no encode step to decline).
doc/JITTesting.md: a dated update block recording the current arm64
(all three trees) and x86-64-HV64 G1/G2/G3 counts, the 39-file
corpus now running on every JIT architecture, `utils/jit/jit-canon.sh`
as the canonicalizer shared between `jit-dump.sh` and
`recompile-deterministic.js`, the corpus-skip fix, and the baseline
reroll; older per-config-table entries not re-measured this task are
left as their historical values with a pointer to the new block.
Each of the four companion specs' Delivered sections gained the
one-line "Ported to arm64 2026-09-18" pointer to this document.

**Self-review.** Scope diff for x86-64/shared-driver: empty (see
above). All doc claims cross-checked against the landed arm64 code
(`isCheapConst`, the `coldWriteCacheIdxs_` append site, the TA-tier
`fcmp`/`fabs`/`fcvtzs` sequence, `offset_`'s real-temp placement, and
each new pin file's actual `UNSUPPORTED` line) rather than assumed
from the design text. No test was gated away to reach green; no
failure was found to root-cause.

**Fix round (Controller review, 2026-09-18).** The review found the
gap-assessment above had only grepped the WRITE cold-cache list: x86-64
also reports cold GetById sites (`coldReadCacheIdxs_.push_back`,
`JitEmitter-property.cpp` ~914), which arm64 never mirrored — a real
functional-parity gap, not just wrong prose. See §0's correction above
for the finding and `JIT: arm64 GetById cold-site reporting`
(`4d6e3a747`) for the one-hunk fix, mirroring x86's `else if
(!cacheEntry->clazz.getNoBarrierUnsafe())` branch verbatim. Re-verified
after the fix: arm64 103/30/0 on all three trees (unchanged), x86
126/7/0 (unchanged), the ById-recompile test subset's counter pins
unaffected (9 pass / 0 fail), scope diff still empty. Three doc
corrections folded into the existing docs commit via a non-interactive
rebase (`4dca3382f` -> `8407e45a7`, content amended, message
unchanged): the recompilation paragraph now states both backends
report both PutById and GetById; the PutByVal typed-array section's
invented "arm64 has no MallocGC tree" explanation for unconditional
typed-array-tier availability was replaced with the verified
code-supported reason (`emitPutByValTypedArrayTier` is simply never
wrapped in `#if HERMES_JIT_INLINE_SAFE_STORE` on either backend — the
macro itself is arch-agnostic, driven by `HERMESVM_GCKIND`). Full
detail: task-5-report.md §8.
