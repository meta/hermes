# arm64 Port of the Polymorphic GetById Tier — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the arm64 JIT the same polymorphic GetById tier x86-64
has: the recording call through the per-site helper slot, recording of
legacy single-shape specializations, and the polymorphic chain.

**Architecture:** Emitter-only port in lib/VM/JIT/arm64/. The records,
`_jit_get_by_id`, progress, demotion, carry-forward and GC tracing are
shared code already built and tested on arm64. Tests that pin only
banners, comments, counters and output move from test/jit/x86-64/ to
test/jit/ so both backends run them.

**Tech Stack:** C++17, asmjit a64, lit/FileCheck under qemu-user.

**Plan review:** Codex, 4 rounds (addendum and plan together); final
verdict READY TO EXECUTE.

**Spec:** doc/superpowers/specs/2026-10-14-jit-getbyid-polymorphic-design.md
— the whole spec is binding; its "arm64 addendum (2026-10-14)" section
states every arm64-specific decision. Read both "Emission (x86-64)"
and the addendum before Task 1.

## Global Constraints

- No changes under lib/VM/JIT/x86-64/, lib/VM/JIT/JitCompiler.cpp,
  lib/VM/JIT/JitHandlers.{h,cpp}, include/hermes/VM/JIT/ or
  lib/VM/CodeBlock.cpp. If the port seems to need one, stop and report.
- Recording call: x0 runtime, x1 &source, w2 symID, x3 cache entry,
  x4 version record, x5 `JitByIdSiteRecord *`; IP saved first, then
  `ldr x16, [x5, #helperOfs]`, `blr x16`. `helperOfs` from
  `&site_->helper - site_`.
- Chain registers: xTemp1 object, xTemp2 compressed receiver class
  (restored at the join), xTemp3 receiver lazyJITId, xTemp4 parent in
  the prototype block / large-offset temporary for own loads; ID
  compares via `emit_cmp_imm32(a, reg.w(), id, xScratch.w())`.
- Comment strings identical to x86: `// Get polymorphic chain (%u
  entries)`, `// Get from object specialization`, `// Get from parent
  specialization` (tests pin them on both backends).
- arm64 builds are Debug `-O1`, no ASan (aarch64/README.md). Rebuild
  `cmake-build-host` (hermesc shermes) BEFORE any arm64 tree, every
  time, and rebuild a tree before running its suite.
- Trees (already configured): `cmake-build-host`, `cmake-build-arm64`,
  `cmake-build-arm64-hv32`, `cmake-build-arm64-boxed`,
  `cmake-build-x86jit`, `cmake-build-x86jit-hsan`.
- arm64 suite: `LIT_FILTER='jit/' cmake --build <tree> --target
  check-hermes`; sanity: `./aarch64/qemu-sanity.sh <tree>`.
- Never bare `cd`. 80 columns, 2-space indent, doc comment on every
  declaration. Invoke `gc-safe-coding` before editing the emitter
  (it calls initHCLazyIDMayAlloc, a GC safepoint, and reads WeakRoots).
- Read DESIGN-PRINCIPLES.md (repo root) before starting.
- Commits end with exactly:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MxvtDWnnS16sfyuVjoNLwC
  ```
  Message per the tightening-commit-messages skill. Never push. Do not
  touch the untracked `2026-09-08-putbyval-typed-array-measurements.md`
  and `dz/issues/01a063df-0527-71dc-89c2-2464bff4a1e4.md`.

## Review Focus

1. A class ID that `a64::Utils::isAddSubImm` rejects (above 4095 and
   not a multiple of 4096 below 2^24): must compare correctly through
   xScratch (Task 1 pin test forces such IDs with many prior classes).
2. hwRes aliasing xTemp2 or xTemp4 on a prototype hit: the load must
   still read the parent's slot (Task 2's relocated proto/edge tests
   on all three arm64 heap modes).
3. Indirect (out-of-line) slots whose byte offset reaches 32 KiB, where
   arm64's emit_load_from_base_offset needs its temporary register, own
   and prototype holder (Task 1's getbyid-poly-bigslot.js, all three
   arm64 heap modes).
4. A demoted slot on arm64: the 4-argument helper called with x4/x5
   set (relocated demotion tests on arm64).
5. HV32 compressed-pointer decode of the receiver class into xTemp3
   while xTemp2 keeps the compressed value for the generic tier
   (relocated generic-join test on cmake-build-arm64-hv32).

---

### Task 1: arm64 emitter — recording call, records, chain

**Files:**
- Modify: `lib/VM/JIT/arm64/JitEmitter.h` (`byIdSiteRecord`,
  `DECL_GET_BY_ID`, `getByIdImpl` declaration,
  `callRuntimeWithSavedIPIndirect(const a64::Mem &, const char *)`)
- Modify: `lib/VM/JIT/arm64/JitEmitter.cpp` (the new overload, next to
  the uint64_t one ~608)
- Modify: `lib/VM/JIT/arm64/JitEmitter-property.cpp` (`GetByIdImpl`
  ~798-1180, `Emitter::getByIdImpl`)
- Create: `test/jit/getbyid-poly-emitted-arm64.js`,
  `test/jit/getbyid-poly-bigslot.js`
- Modify: `test/jit/count.js`, `test/jit/recompile-phased-warming.js`
  (their `X86-NEXT:` ById dump lines become `CHECK-NEXT:` in this task,
  because arm64 starts printing the line here)

**Interfaces:**
- Consumes (shared, unchanged): `JitByIdSiteRecord`,
  `JitConsumerRecords::findOrCreateByIdSite(uint32_t)`,
  `isEligibleByIdShape`, `addByIdShape`, `_jit_get_by_id(SHRuntime *,
  const SHLegacyValue *, SHSymbolID, SHReadPropertyCacheEntry *,
  SHJitVersionData *, SHJitByIdSiteRecord *)`.
- Reference implementation to mirror: x86-64 `GetByIdImpl`
  (lib/VM/JIT/x86-64/JitEmitter-property.cpp: `run()` site setup and
  call ~780-855, `emitFastPath()` tier selection ~940-985,
  `addCacheShape`/`preparePolyChain`/`emitPolyChain` ~1049-1170,
  legacy recording at the end of both specializations).

- [ ] **Step 1: Rebuild host compilers and the HV64 arm64 tree**

```bash
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes
```

- [ ] **Step 2: Write the failing pin test**

`test/jit/getbyid-poly-emitted-arm64.js` (standard copyright header):

```js
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// REQUIRES: jit, jit-arch-arm64
// UNSUPPORTED: handle_san

// arm64 emission of the polymorphic GetById tier: the recording call
// goes through the site record in x5, and the chain compares lazy JIT
// IDs. Lazy IDs are assigned only when emitted code bakes a class in,
// so 4200 tiny functions are compiled warm first, each specializing
// its single GetById on its own hidden class: that assigns ~4200 IDs,
// and the chain in get() then compares IDs above 4095 that are not
// multiples of 4096, which a64::Utils::isAddSubImm rejects, so each
// compare materializes the ID in w16 first.
function main() {
  var N = 4200;
  for (var i = 0; i < N; ++i) {
    var o = {};
    o['k' + i] = i;
    o.x = 1;
    var f = new Function('o', 'return o.x;');
    // Three calls: interpreted warm-up fills the cache, the threshold
    // compile (-Xjit-threshold=2) specializes on o's class.
    f(o); f(o); f(o);
  }
  function get(o) {
    return o.v;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 1]);
    return s;
  }
  function P() {}
  P.prototype.v = 2;
  print(drive(get, [{v: 1}, new P()], 1000));
}
main();

// OUT: 1500
// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'get'{{$}}
// CHECK: ldr x16, [x5, {{[0-9]+}}]
// CHECK-NEXT: blr x16
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'get'{{$}}
// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// CHECK: // Get polymorphic chain (2 entries)
// CHECK: ldrh w{{[0-9]+}}
// The own entry's receiver-ID compare, then the prototype entry's
// receiver-ID and prototype-ID compares, each through w16.
// CHECK: mov w16, 0x{{[0-9A-F]+}}
// CHECK-NEXT: cmp w{{[0-9]+}}, w16
// CHECK: mov w16, 0x{{[0-9A-F]+}}
// CHECK-NEXT: cmp w{{[0-9]+}}, w16
// CHECK: mov w16, 0x{{[0-9A-F]+}}
// CHECK-NEXT: cmp w{{[0-9]+}}, w16
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
```

asmjit's arm64 logger prints immediates without `#` and in hex above 9
(arm64/JitEmitter.cpp ~83 enables hex). Before relying on the `mov w16`
lines, confirm from a real dump that the 4200 warm compiles assigned
IDs (their `// Get from object specialization` comments) and that
`get`'s chain IDs are rejected by isAddSubImm; confirm the exact
operand spelling against that dump, keeping every check bound between
the `'get'` banners. If the run is too slow under
qemu (target: under ~60 s per RUN line on cmake-build-arm64), say so in
the report rather than lowering N below 4096.

Also create the arch-neutral `test/jit/getbyid-poly-bigslot.js`:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=DUMP,DUMP%jit_arch_prefix %s
// REQUIRES: jit

// Out-of-line slots far enough out that the byte offset reaches
// 32 KiB, where arm64's emit_load_from_base_offset needs its temporary
// register (HV64: slot > 4096; HV32: slot > 8192). 9000 properties
// precede `x`, so both modes qualify. The big object is a cacheable
// dictionary (no deletes), which Eligibility allows as a property
// holder, own or prototype; the prototype case's receiver,
// Object.create(holder), is not a dictionary.
function main() {
  function mk(prefix, last) {
    var o = {};
    for (var i = 0; i < 9000; ++i)
      o[prefix + i] = i;
    o.x = last;
    return o;
  }
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var r = '';
    for (var i = 0; i < n; ++i)
      r = f(arr[i % 3]);
    return r;
  }
  var bigOwn = mk('a', 'own');
  var viaBig = Object.create(mk('b', 'proto'));
  var small = {x: 'small'};
  drive(get, [bigOwn, viaBig, small], 300);
  print(get(bigOwn), get(viaBig), get(small));
}
main();

// CHECK: own proto small
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (3 entries)
// The two big-slot chain entries (own, prototype) each load through a
// register offset; both must appear before the generic tier, whose
// comment closes the window.
// DUMPARM64: ldr {{[wx][0-9]+}}, [x{{[0-9]+}}, x{{[0-9]+}} lsl {{[23]}}]
// DUMPARM64: ldr {{[wx][0-9]+}}, [x{{[0-9]+}}, x{{[0-9]+}} lsl {{[23]}}]
// DUMP: // Read property cache
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
```

Confirm on a real arm64 dump that the register-offset load belongs to
a chain entry (not the generic tier) and on x86 that the 3-entry chain
appears; if the dictionary objects turn out ineligible (e.g. no-cache),
say why and restructure with plain objects of the same size, never
dropping below the 32 KiB offset.

- [ ] **Step 3: See it fail**

```bash
LIT_FILTER='getbyid-poly-emitted-arm64' cmake --build cmake-build-arm64 --target check-hermes
```

Expected: FAIL (no `x5` call, no chain on arm64). Run the bigslot test
on cmake-build-arm64 too: its DUMP check fails (no chain).

- [ ] **Step 4: Invoke the gc-safe-coding skill**

- [ ] **Step 5: Emitter header and the indirect-call overload**

In `lib/VM/JIT/arm64/JitEmitter.h`, next to `byValSiteRecord`:

```cpp
  /// \return the candidate version record's ById site entry for
  /// \p siteId, creating the records and/or entry if absent. The
  /// entry's address is stable and may be embedded in code.
  JitByIdSiteRecord &byIdSiteRecord(uint32_t siteId) {
    if (!versionData_->consumerRecords)
      versionData_->consumerRecords = std::make_unique<JitConsumerRecords>();
    return versionData_->consumerRecords->findOrCreateByIdSite(siteId);
  }
```

Next to the existing `callRuntimeWithSavedIPIndirect(uint64_t, const
char *)` declaration:

```cpp
  /// Like the uint64_t overload, but the helper pointer is loaded from
  /// \p slot, a memory operand whose base register is already set up
  /// (e.g. a site record passed as an argument). The base must not be
  /// xScratch, which saving the IP clobbers.
  void callRuntimeWithSavedIPIndirect(const a64::Mem &slot, const char *name);
```

`getByIdImpl` gains `bool recording` (last parameter, doc: "\param
recording true for the GetById family: record shapes through the
site's helper slot"), and the macro passes it:

```cpp
#define DECL_GET_BY_ID(methodName, commentStr, shFn, recording)         \
  void methodName(FR frRes, SHSymbolID symID, FR frSource, uint8_t cacheIdx) { \
    getByIdImpl(                                                        \
        frRes, symID, frSource, cacheIdx, commentStr, shFn, #shFn,      \
        recording);                                                     \
  }
  DECL_GET_BY_ID(getById, "getById", _sh_ljs_get_by_id_rjs, true)
  DECL_GET_BY_ID(tryGetById, "tryGetById", _sh_ljs_try_get_by_id_rjs, false)
```

(keep the existing comment and `#undef` around the macro).

In `lib/VM/JIT/arm64/JitEmitter.cpp`, after the uint64_t overload:

```cpp
void Emitter::callRuntimeWithSavedIPIndirect(
    const a64::Mem &slot,
    const char *name) {
  assert(
      !(slot.hasBaseReg() && slot.baseId() == xScratch.id()) &&
      "saving the IP clobbers xScratch");
  // Save the current IP in the runtime.
  getBytecodeIP(xScratch);
  a.str(xScratch, a64::Mem(xRuntime, RuntimeOffsets::currentIP));

  comment("// call %s", name);
  a.ldr(xScratch, slot);
  a.blr(xScratch);

  if (emitAsserts_) {
    // Invalidate the current IP to make sure it is set before the next call.
    a.mov(xScratch, Runtime::kInvalidCurrentIP);
    a.str(xScratch, a64::Mem(xRuntime, RuntimeOffsets::currentIP));
  }
}
```

(If `a64::Mem` spells the base accessors differently, use the asmjit
equivalents; the assertion's meaning must not change.)

- [ ] **Step 6: Site setup and the recording call in `run()`**

`GetByIdImpl` gains members (doc comments as in x86):

```cpp
  /// Whether this instruction belongs to the GetById family, which
  /// records shapes through its site's helper slot.
  bool recording_;
  /// The candidate version's record for this site; null unless
  /// recording_ and the instruction has a property cache entry.
  JitByIdSiteRecord *site_ = nullptr;
```

and the constructor takes `bool recording` last, initializing
`recording_(recording)`; `Emitter::getByIdImpl` passes it through.

At the top of `run()`, after the comment:

```cpp
    if (recording_ && cacheIdx != hbc::PROPERTY_CACHING_DISABLED) {
      uint32_t siteId = (uint32_t)((const char *)_.emittingIP -
                                   (const char *)_.codeBlock_->begin());
      site_ = &_.byIdSiteRecord(siteId);
      // Only a never-flipped slot is initialized; a demoted value
      // carried forward stays demoted.
      if (!site_->helper)
        site_->helper = (void *)_jit_get_by_id;
    }
```

Replace `_.callRuntimeWithSavedIP((void *)shImpl, shImplName);` (after
the x3 setup) with:

```cpp
    if (site_) {
      _.loadBits64InGp(a64::x4, (uint64_t)_.versionData_, "JitVersionData");
      // x5 is both the helper's record argument and the base of the
      // helper slot the call goes through.
      _.loadBits64InGp(a64::x5, (uint64_t)site_, "JitByIdSiteRecord");
      // Type-check the recording callee against the call sequence; the
      // demoted _sh_ljs_get_by_id_rjs ignores x4/x5 under AAPCS64.
      using _FnT = SHLegacyValue (*)(
          SHRuntime *,
          const SHLegacyValue *,
          SHSymbolID,
          SHReadPropertyCacheEntry *,
          SHJitVersionData *,
          SHJitByIdSiteRecord *);
      _FnT _fn = _jit_get_by_id;
      (void)_fn;
      // JitByIdSiteRecord is not standard-layout, so the slot's offset
      // comes from the record, not offsetof. It is 8-aligned and fits
      // the scaled 12-bit ldr immediate.
      int32_t helperOfs =
          (int32_t)((const char *)&site_->helper - (const char *)site_);
      _.callRuntimeWithSavedIPIndirect(
          a64::Mem(a64::x5, helperOfs), "_jit_get_by_id [indirect]");
    } else {
      _.callRuntimeWithSavedIP((void *)shImpl, shImplName);
    }
```

- [ ] **Step 7: Legacy recording**

At the end of `emitObjectSpecialization`, before `return true;`:

```cpp
    if (site_) {
      addByIdShape(
          *site_,
          cacheEntry->clazz.getNoBarrierUnsafe(),
          CompressedPointer(nullptr),
          cacheEntry->getSlot(),
          false,
          true);
    }
```

At the end of `emitParentSpecialization`, before `return true;`:

```cpp
    if (site_) {
      addByIdShape(
          *site_,
          cacheEntry->negMatchClazz.getNoBarrierUnsafe(),
          cacheEntry->clazz.getNoBarrierUnsafe(),
          cacheEntry->getSlot(),
          true,
          true);
    }
```

- [ ] **Step 8: The chain**

Add to `GetByIdImpl`, mirroring x86 (same `PolyEntry`, `poly_`,
`addCacheShape`, `preparePolyChain` — copy those three verbatim from
x86; they emit no code):

```cpp
  /// Emit the chain. Entry state: xTemp1 = object, xTemp2 = compressed
  /// receiver class. Fallthrough (the generic tier's join): the same.
  /// hwRes may alias a temp; it is written only on hit paths, which
  /// branch to contLab. ID compares use xScratch for IDs that are not
  /// encodable as a compare immediate.
  void emitPolyChain(Emit_sh_shv_decode &shvDecode) {
    _.comment("// Get polymorphic chain (%u entries)", (unsigned)poly_.size());
    // xTemp3 = receiver lazyJITId, decoded without clobbering xTemp2.
    // Loaded even if every entry is a prototype entry.
    const auto &xReg =
        emit_sh_cp_decode_non_null_preserve_input(a, xTemp3, xTemp2);
    a.ldrh(xTemp3.w(), a64::Mem(xReg, RuntimeOffsets::hiddenClassLazyJITId));
    bool anyProto = false;
    for (const PolyEntry &p : poly_) {
      if (p.isProto) {
        anyProto = true;
        continue;
      }
      asmjit::Label next = a.newLabel();
      emit_cmp_imm32(a, xTemp3.w(), p.objId, xScratch.w());
      a.b_ne(next);
      // May clobber xTemp1 (indirect slot) and xTemp4; this path ends
      // at contLab.
      emitLoadFromSlot(hwRes.a64GpX(), xTemp1, xTemp4, p.slot);
      shvDecode.emitFirstCase(a);
      a.b(contLab);
      a.bind(next);
    }
    if (!anyProto)
      return;
    asmjit::Label join = a.newLabel();
    // xTemp4 = parent; xTemp2 = parent's lazyJITId (scratch from here on).
    emit_load_cp(a, xTemp4, a64::Mem(xTemp1, offsetof(SHJSObject, parent)));
    a.cbz(xTemp4, join);
    emit_sh_cp_decode_non_null(a, xTemp4);
    emit_load_cp(a, xTemp2, a64::Mem(xTemp4, offsetof(SHJSObject, clazz)));
    emit_sh_cp_decode_non_null(a, xTemp2);
    a.ldrh(xTemp2.w(), a64::Mem(xTemp2, RuntimeOffsets::hiddenClassLazyJITId));
    for (const PolyEntry &p : poly_) {
      if (!p.isProto)
        continue;
      asmjit::Label next = a.newLabel();
      emit_cmp_imm32(a, xTemp3.w(), p.objId, xScratch.w());
      a.b_ne(next);
      emit_cmp_imm32(a, xTemp2.w(), p.protoId, xScratch.w());
      a.b_ne(next);
      // Clobbers xTemp4/xTemp2 only after both guards succeeded.
      emitLoadFromSlot(hwRes.a64GpX(), xTemp4, xTemp2, p.slot);
      shvDecode.emitFirstCase(a);
      a.b(contLab);
      a.bind(next);
    }
    a.bind(join);
    // Restore the generic tier's operand.
    emit_load_cp(a, xTemp2, a64::Mem(xTemp1, offsetof(SHJSObject, clazz)));
  }
```

Check `emitLoadFromSlot`'s contract before relying on it
(arm64/JitEmitter-property.cpp ~902): `tmpReg` must differ from
`objReg`, may equal `resReg`. Both calls above satisfy it.

In `emitFastPath`, replace the single-shape selection statement with
the x86 shape:

```cpp
    ReadPropertyCacheEntry *cacheEntry =
        _.codeBlock_->getReadCacheEntry(cacheIdx);
    if (site_ && preparePolyChain(cacheEntry)) {
      // Replaces the single-shape choice; always followed by the
      // generic tier below. Not a cold site.
      emitPolyChain(shvDecode);
    } else if (cacheEntry->numGoodChanges == 1) {
      // ... the existing object/parent specialization code, unchanged,
      // including the terminal `return` after a parent specialization ...
    } else if (!cacheEntry->clazz.getNoBarrierUnsafe()) {
      _.coldReadCacheIdxs_.push_back(cacheIdx);
    }
```

The generic tier after it is unchanged; its indirect path's
`shvDecode.emitAll(a)` stays the only rest-case emission.

- [ ] **Step 8b: Restore the shared dump-line checks**

In `test/jit/count.js` and `test/jit/recompile-phased-warming.js`, the
`X86-NEXT:` ById lines become `CHECK-NEXT:` (arm64 now prints the
line too) and their comments say both backends print it. Drop
`%jit_arch_prefix` from those RUN lines if nothing else there uses it;
keep the lit.local.cfg substitution (the rooted and bigslot tests use
it). Run both on cmake-build-x86jit: green.

- [ ] **Step 9: Build all three arm64 trees and run**

```bash
cmake --build cmake-build-host --target hermesc shermes
for t in cmake-build-arm64 cmake-build-arm64-hv32 cmake-build-arm64-boxed; do
  cmake --build $t --target hermes &&
  ./aarch64/qemu-sanity.sh $t &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: no `FAIL`; both new tests pass on every tree; 107 pass per
arm64 tree (the previous 105 plus the two new tests).

- [ ] **Step 10: Prove the pin can fail**

Mutation A: in `run()`, force `site_ = nullptr` after setup → rebuild
cmake-build-arm64 → the pin test FAILS (no `ldr x16, [x5`). Mutation
B: in `emitPolyChain`'s prototype loop, compare `xTemp2` against
`wzr` instead of `emit_cmp_imm32(..., p.protoId, xScratch.w())` → the
pin test FAILS (the third `cmp ..., w16` is missing). Restore and
rebuild after each; green.

- [ ] **Step 11: x86 unchanged**

`git diff --stat HEAD` shows only lib/VM/JIT/arm64/ files, the two new
tests, count.js and recompile-phased-warming.js. Then `LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target
check-hermes`: one more pass than before (bigslot; the arm64 pin test
is unsupported there).

- [ ] **Step 12: Commit**

```bash
git add lib/VM/JIT/arm64 test/jit/getbyid-poly-emitted-arm64.js \
  test/jit/getbyid-poly-bigslot.js test/jit/count.js \
  test/jit/recompile-phased-warming.js
git commit   # "JIT: arm64 polymorphic GetById tier"
```

---

### Task 2: Run the GetById tests on both backends

**Files:**
- Move (`git mv`): every `test/jit/x86-64/getbyid-record-*.js` and
  `test/jit/x86-64/getbyid-poly-*.js` → `test/jit/`
- Modify: the moved `getbyid-poly-rooted-obj.js`,
  `getbyid-poly-rooted-str.js`

**Interfaces:**
- Consumes: Task 1's arm64 tier (identical comment strings).
- Produces: nothing new.

- [ ] **Step 1: Relocate**

```bash
for f in test/jit/x86-64/getbyid-record-*.js test/jit/x86-64/getbyid-poly-*.js; do
  git mv "$f" test/jit/
done
```

Leave `test/jit/x86-64/exceptions.js` in place.

- [ ] **Step 2: Generalize the rooted tests' instruction pin**

In both rooted tests, the DUMP run's x86 instruction line
`// DUMP: cmp {{[a-z0-9]+}}, 1{{$}}` becomes per-backend lines. Change
that RUN line to
`... | %FileCheck --check-prefixes=DUMP,DUMP%jit_arch_prefix %s` and
replace the line with:

```
// DUMPX86: cmp {{[a-z0-9]+}}, 1{{$}}
// DUMPARM64: cmp w{{[0-9]+}}, 1{{$}}
```

keeping the surrounding `DUMP:`/`DUMP-NOT: cmp` lines (the DUMP-NOT
still forbids any earlier compare after the chain comment on both
backends). asmjit's arm64 logger prints immediates without `#`;
verify the spelling against a real arm64 dump before committing.

- [ ] **Step 3: Run on every tree**

```bash
cmake --build cmake-build-host --target hermesc shermes
for t in cmake-build-arm64 cmake-build-arm64-hv32 cmake-build-arm64-boxed cmake-build-x86jit; do
  cmake --build $t --target hermes &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
cmake --build cmake-build-x86jit-hsan --target hermes
LIT_FILTER='getbyid-' cmake --build cmake-build-x86jit-hsan --target check-hermes
```

Expected: no `FAIL`. x86jit: same pass count as after Task 1 (the
tests moved, none lost). arm64: the relocated tests now PASS there (not
unsupported). hsan: the relocated GC/rooting tests still run and pass.
Any relocated test that fails only on arm64 is a finding about the
port (debug with superpowers:systematic-debugging; fix in the arm64
emitter, never by gating the test), unless it pins something genuinely
x86-specific — then generalize the pin as in Step 2 and say why.

- [ ] **Step 4: Prove-can-fail on arm64**

On cmake-build-arm64 (rebuild host first each time; restore and
rebuild after each):
- Mutation A: delete the `addByIdShape(..., true)` call in arm64
  `emitObjectSpecialization` AND the `addCacheShape` call in arm64
  `preparePolyChain` → getbyid-poly-monotone.js FAILS.
- Mutation B: delete the join reload of xTemp2 in arm64
  `emitPolyChain` → getbyid-poly-generic-join.js FAILS.
- Mutation C: compare xTemp3 against `p.protoId` in the prototype loop
  (swap the operand) → getbyid-poly-proto.js FAILS.
- Mutation D: call `shvDecode.emitAll(a)` instead of `emitFirstCase`
  at an own-entry load → a Debug assertion (rest cases emitted twice)
  fires on cmake-build-arm64-boxed in getbyid-poly-generic-join.js.

- [ ] **Step 5: Commit**

```bash
git add -A test/jit
git commit   # "JIT: run the GetById tier tests on both backends"
```

(`git add -A test/jit` stages the moves; check `git status` first that
nothing else under test/jit is unexpected.)

---

### Task 3: Docs and close the issue

**Files:**
- Modify: `doc/JIT.md` (the GetById tier is no longer x86-only)
- Modify: the spec — append "arm64 delivered (date)" under the addendum
- Close: dz `01a13d77`

- [ ] **Step 1: Docs**

doc/JIT.md: where it says the polymorphic GetById tier, the recording
call or the chain is x86-64 only, say both backends, and add the arm64
call sequence (`x5` record, `ldr x16, [x5, #ofs]`, `blr x16`) and the
xScratch compare note next to the x86 description. Append to the spec
under the addendum an "arm64 delivered" paragraph: trees, sanity and
JIT suite counts per tree, the mutations that failed, and any
deviation from the addendum.

- [ ] **Step 2: Close the issue**

```bash
dz close 01a13d77 --as fixed -m - --json <<'EOF'
Ported: arm64 now emits the recording call, legacy-shape recording and
the polymorphic chain; the GetById tests run on both backends. See the
spec's "arm64 addendum" and "arm64 delivered" sections.
EOF
```

- [ ] **Step 3: Commit**

```bash
git add doc/JIT.md doc/superpowers/specs/2026-10-14-jit-getbyid-polymorphic-design.md dz/issues/01a13d77*.md
git commit   # "JIT: document the arm64 GetById tier"
```
