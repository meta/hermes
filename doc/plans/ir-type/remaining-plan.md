# IR Type System: Remaining Work Plan (finish Phase 4, then Phase 5)

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close out Phase 4 (Array + Function) and implement Phase 5 (Tuple +
ExactObject) of the IR type system v2, so that every refined Flow type lowers
to a live refined IR type, recursive types lower without crashing, and every
consumer treats refined kinds soundly.

**Supersedes:** Tasks 7–10 of [phase4-plan.md](phase4-plan.md), which were
written before `flow::ArrayType` was deleted (D112825921) and before arrays
became `Array<T>` class specializations. Their `isLooping`-based routing does
not catch array cycles (see Task A1). Tasks 1–6 of phase4-plan.md are done.
Governing decisions D1–D6 are in [phase3-5-design.md](phase3-5-design.md).

**Note (2026-11-08):** the commit subjects named in the status lines below
are from the development history, which was squashed into nine commits (see
"Commit stack" in HANDOFF.md).

**Status (2026-11-07):** all tasks are done; each task below has a status
line naming its commits. See the Phase 4 and Phase 5 notes in
[progress.md](progress.md).

**State at time of writing** (stack top D112825915, rebased onto master
2026-11-06; all line numbers below are at that revision):

- Done: `TypeContext` Array/Function factories, interning, reserve/fill,
  accessors, algebra and printing; `Array<T>` lowers to `array(T)`;
  `TypedFunction` and `ClassConstructor` (of a class with an explicit
  constructor signature; otherwise `object`) lower to `function(...)`.
- Full lit (4503) and `HermesIRTests` (99) pass, but no test exercises a
  recursive array or function type.
- Verified failures (ASan stack overflow in `flowTypeToIRType`, reproduced on
  the rebased build):
  - `type Json = number | string | Json[]` (cycle through an array)
  - `type F = (x: number) => F` and `(x: number) => F | null` (cycle through a
    function)
  - `type FA = (x: FA[]) => number` (cycle through both)
- Verified to work: recursive classes, a class with a `C[]` field, recursive
  object-literal and tuple aliases (these still lower to `object`).

---

## Conventions

- **Build dir** (outside the EdenFS checkout, ASan+Debug, `-O1`):
  `B=/data/users/tmikov/scratch/hermes-irtype-asan`. Configure once with the
  flags in `CLAUDE.md` if it does not exist. One build at a time.
- **Build:** `ninja -C $B shermes hermes HermesIRTests`
- **Unit tests:** `$B/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.*'`
- **Lit (Linux runs multi-process):**
  `ASAN_OPTIONS=detect_leaks=0 ninja -C $B check-hermes`; filter with
  `LIT_FILTER=<regex>`. Regenerate `%FileCheckOrRegen` baselines with
  `ninja -C $B update-lit`, only after reading why each diff changed.
- **Recursive-type tests must run under a timeout** (`timeout 60 ...`): the
  failure mode before a fix is unbounded recursion.
- **Format:** `arc f` on touched `.cpp`/`.h`.
- **Commits:** one commit per task, Meta format (`[SH]: subject`, Summary,
  Test Plan), explicit paths only. Fold fixes into the commit they fix when
  that is where they belong; never `jf submit` unprompted.
- **Every test states what it fails on, and that failure is checked**
  (DESIGN-PRINCIPLES.md §6): run each new test once against the unfixed code
  and record the failure in the commit's test plan.

---

## Part A — Finish Phase 4

### Task A1: Break cycles through arrays and functions in `flowTypeToIRType`

**Status (2026-11-07): done** in `[SH]: Lower recursive array and function
types`. The identity check failed, so the co-inductive dedup was added.

**Why the old Task 7 design does not work.** Flow's `FindLoopingTypes`
stops at nominal types (`isLooping(Type *, TypeWithId *)` returns false,
`lib/Sema/FlowChecker-scopetypes.cpp:163-166`). `Array<T>` is now a
`ClassType` specialization, so in `type Json = number | string | Json[]` the
cycle runs through a class and Flow never sets `isLooping` on it. In IRGen,
though, the array intercept in `flowTypeToIRType(flow::Type *)`
(`lib/IRGen/ESTreeIRGen-typed-class.cpp:660-670`) lowers the class
*structurally* (`createArray(elem)`), so the cycle reappears. Function cycles
(`type F = (x: number) => F`) are flagged by Flow (`isLooping(Type *,
TypedFunctionType *)`, `:208-218`) but IRGen ignores the flag:
`lowerTypedFunction` (`:641-658`) recurses into its return type
unconditionally.

**Design: lazy reservation keyed by `const flow::TypeInfo *`.** This works
for both kinds without relying on `isLooping`, and keeps interning for the
common non-recursive case:

- `loweringInProgress_`: `DenseMap<const flow::TypeInfo *, Type>`. When
  lowering of an Array class or a `TypedFunctionType` starts, insert the
  `TypeInfo` with value `NoType` ("in progress, nothing reserved").
- Re-entry while in progress means there is a cycle. If the value is still
  `NoType`, call `reserveArray()`/`reserveFunction()`, store the reserved Type,
  and return it. Otherwise return the stored reserved Type.
- When lowering finishes: if a Type was reserved, `fillArray`/`fillFunction`
  it with the computed children and return it. Otherwise call the interning
  `createArray`/`createFunction`. In both cases erase the in-progress entry
  and record the result in `loweredStructuralTypes_`
  (`DenseMap<const flow::TypeInfo *, Type>`), checked first on entry.
- Key arrays by the Array `ClassType` `TypeInfo`. It is canonical per
  specialization because `isArrayClassType` is derived from the `ClassType`'s
  generic-origin Decl (`include/hermes/Sema/FlowContext.h`,
  `isArrayClassType`). Key functions by the `TypedFunctionType`.

**Invariant to restate.** `reserveArray`'s doc comment
(`include/hermes/IR/TypeContext.h:681-685`) says a reserved entry "must not be
inspected by any type operation". The lazy scheme does inspect it:
`unionTy(number|string, reservedArray)` runs while the array is unfilled.
That is safe because the algebra reads only the kind and the id of
Array/Function entries (`isLeafSubtypeById`, `lib/IR/TypeContext.cpp:457-476`;
`areLeafTypesDisjointById`, `:478-500`), and `reserveArray`/`reserveFunction`
set the kind (`:317-324`, `:348-361`). Rewrite both comments to say exactly
that: the kind and id are valid immediately, and the payload is valid only
after fill. Add a unit test that runs `unionTy`, `isSubsetOf` and `areDisjoint`
on a reserved, unfilled Array, so a future payload-reading change breaks a
test instead of silently reading `kNoTypeId`.

**Files:**
- Modify: `lib/IRGen/ESTreeIRGen.h` (two maps, next to `loweredClassTypes_` at
  `:622`), `lib/IRGen/ESTreeIRGen-typed-class.cpp` (array intercept,
  `lowerTypedFunction`), `include/hermes/IR/TypeContext.h` (comments),
  `unittests/IR/TypeContextTest.cpp`.
- Test: new `test/IRGen/flow/recursive-type.js` (`%FileCheckOrRegen`,
  `-Xdump-functions` to keep the dump small) and
  `test/shermes/recursive-type-exec.js` (`-typed -exec`).

**Steps:**
- [ ] Write the lit tests with one function per shape: `Json` (array cycle),
  `F` and `F | null` (function cycle), `FA` (cycle through both), mutual
  recursion `type A = B[]; type B = (a: A) => B`, and a class whose field is
  an array of a function type returning the class. Run under `timeout`;
  confirm each crashes with a stack overflow before the fix.
- [ ] Implement the lazy reservation for arrays and functions. Route
  `ClassConstructor` (`:741-752`) through the same function-lowering helper so
  constructor signatures share the scheme.
- [ ] Route the one caller that bypasses the array intercept,
  `flowTypeToIRType(arm->info)` in `getDefaultInitValue` (`:610`), through the
  `flow::Type *` overload. Today it lowers an `Array<T>` arm as
  `classInstance(Array)`; harmless for `isIDZType`, but it is the only place
  that can disagree with the rest of IRGen.
- [ ] **Identity check (counterexample first, DESIGN-PRINCIPLES.md §3).**
  Shallow subtyping (D4) compares function and array types by id, so if one
  recursive alias reaches IRGen through two distinct `TypeInfo`s, its values
  get two unrelated ids. Then `StoreFrameInst`'s strict
  `isSubsetOf(value, variable)` check would fail, and so would
  `narrowToFlowType` for anything outside the pass-through added during the
  rebase (`lib/IRGen/ESTreeIRGen-expr.cpp:18-50`). Write a test that passes a
  `Json` value and an `F` value across two functions, stores them into typed
  locals and returns them; run with `-verify-ir`.
  - If it passes, note in the commit that Flow shares the `TypeInfo`.
  - If it fails, add co-inductive dedup: before reserving, look the
    `TypeInfo` up in a table keyed by Flow's cycle-safe `TypeInfo::equals`/
    `hash` (`TypesDenseMapInfo`, `lib/Sema/FlowTypesDumper.h`), and reuse the
    hit's IR type.
- [ ] Unit tests: a self-referential array (`a = reserveArray();
  fillArray(a, a)`) and function; the algebra queries above on a reserved,
  unfilled entry; `isSubsetOf(recursive, Object)`.
- [ ] Run full lit and `HermesIRTests`; commit as
  `[SH]: Lower recursive array and function types`.

### Task A2: Cycle-safe, lossless type printing

**Status (2026-11-07): done** in `[SH]: Make IR type printing cycle-safe`.
Back-references print as `^N` (depth-counted), not `array(...)`, which is
ambiguous.

`printLeaf` (`lib/IR/TypeContext.cpp:557-580`) recurses into an array's
element and a function's return type with no cycle guard, so printing
`type A = A[]` or `type F = (x: number) => F` never terminates once A1 lands.
It also prints a union element by its kind name: `(number|string)[]` dumps as
`array(union)` (verified with `-Xdump-functions`).

- [ ] Write the failing case first: a unit test printing a self-referential
  array and function (with a timeout guard in the test command), and a lit
  check expecting `array(number|string)`.
- [ ] Thread a small visited set (the ids currently being printed) through
  `printLeaf`. On re-entry print a back-reference such as `array(...)`, the
  bare kind name with an elided payload. Print a union element with the same
  arm-joining code `print` uses for top-level unions (`:582-618`).
- [ ] Keep `function(<ret>)`. Printing full signatures would churn every
  baseline for little diagnostic gain; the function header already shows the
  parameters.
- [ ] `update-lit`, read the diffs (they should be only `array(union)` →
  `array(a|b)`), commit as `[SH]: Make IR type printing cycle-safe`.

### Task A3: Consumer audit for Array/Function (replaces old Task 8)

**Status (2026-11-07): done**, one commit per item: `[SH]: Check accepted
arms when casting objects` (reached from typed JS; real miscompile),
`[SH]: Type typed array allocations as array(T)`, `[SH]: Drop narrow of
fast array prototype to object`, `[SH]: Count refined object types in
Auditor stats`, `[SH]: Check IR of function-type pass-through`.
Follow-up "A3b", not in this plan: `[SH]: Document D9: intersecting
unrelated types`, `[SH]: intersectTy of unrelated types keeps 1st (D9)`,
`[SH]: Narrow compatible function values on store`.

A search for exact-`Object` checks (`isObjectType()`, `TypeKind::Object`,
`== Type::createObject()`) in `lib/IR`, `lib/IRGen`, `lib/Optimizer`,
`lib/BCGen` and `include/hermes/IR` finds three sites outside `TypeContext`.
Most consumers already go through `canBeObject`/`isSubsetOf`, which are
refinement-aware.

- [ ] **SH negative type checks (possible miscompile; build the
  counterexample first).** `_typeCastHelper` (`lib/BCGen/SH/SH.cpp:2448-2490`)
  checks the *bad* arms when there are fewer of them than good arms
  (`:2459-2462`). Every refined kind maps to `_sh_ljs_is_object`
  (`:2430-2440`). So a bad arm that is a refined kind, next to a good arm that
  is also an object, makes the negative check reject valid values. Example: a
  cast from `classInstance(A)|classInstance(B)|number|string` to
  `classInstance(A)|number|string` emits `if (_sh_ljs_is_object(x)) throw`,
  which rejects every `A`.
  - Try to reach this from typed JS, e.g. by narrowing a
    `A | B | number | string` union so that only `B` is excluded.
  - Fix: when any bad arm is a refined kind and the result can be an object,
    force the positive check. Add a unit-style or exec lit test that throws
    without the fix.
  - If no JS input reaches it, still fix it (it is a latent soundness bug
    under D2), and say in the commit that the test is IR-level.
- [ ] **Allocation typing for array literals.** `AllocFastArrayInst`'s
  inherent type is `object` (`include/hermes/IR/Instrs.h:2966-2968`). As a
  result, a typed array literal returned as `number[]` gets
  `CheckedTypeCastInst (:array(number)) %alloc: object` (verified at `-O0`),
  which becomes a runtime object check.
  - Mirror what `emitTypedClassAllocation` does for classes
    (`lib/IRGen/ESTreeIRGen-typed-class.cpp:573`): trusted-narrow the
    allocation in `genFastArrayFromElements` (`lib/IRGen/ESTreeIRGen-expr.cpp:465`)
    and `genSHBuiltinObjectProps` (`:861`) to the lowered `array(T)`.
  - Check the `-O` output of a non-inlined function (call it from two sites,
    or mark it so the inliner skips it) before and after; the cast must
    disappear.
- [ ] **The prototype narrow-back-to-object wart.**
  `visitAllocFastArrayInst` (`lib/IR/IRVerifier.cpp:1127-1132`) does not
  constrain the prototype's type, and `generateAllocFastArrayInst`
  (`lib/BCGen/SH/SH.cpp:1740`) only emits its register. Remove the
  `UnionNarrowTrustedInst(..., Object)` at `ESTreeIRGen-expr.cpp:476` and the
  comment that justifies it. Run the full suite with both backends; if
  something does require `object`, find it, cite it in the commit, and keep
  the narrow.
- [ ] `lib/Optimizer/Scalar/Auditor.cpp:125` counts refined kinds as "other".
  It is statistics only; switch to `isSubsetOf(t, Object)` for accurate
  counts.
- [ ] Add a lit test for the shallow-function-subtyping pass-through in
  `narrowToFlowType` (`bar = foo` with contravariant parameters; it exists as
  `test/shermes/function-type-flow.js` but only checks execution). Add an IR
  dump check that no cast is emitted, so a future change to that path is
  visible.
- [ ] Commit as `[SH]: Treat Array/Function as Object in consumers`.

### Task A4: Full regression and baselines (old Task 9)

**Status (2026-11-07): done** together with B7 in `[SH]: Record IR type
Phase 4 and 5 progress`. No baseline changed (`update-lit` was a no-op).

- [ ] Build everything; run `HermesIRTests`, `HermesVMRuntimeTests` and the
  full lit suite.
- [ ] `update-lit` and read every changed baseline. Each change must be
  explained by A1–A3: refined types where `object` was, removed casts,
  printing changes. Stop on anything else.
- [ ] Commit only regenerated baselines not already in A1–A3.

### Task A5: Record Phase 4 progress (old Task 10)

**Status (2026-11-07): done** together with B7 in `[SH]: Record IR type
Phase 4 and 5 progress`.

- [ ] `progress.md`: add Phase 4 rows (tasks 1–6 of phase4-plan.md, A1–A4)
  and a "Phase 4" context note mirroring the Phase 3 one. Include the
  rebase-era changes: the `BuiltinReceiverKind::Number` adaptation in
  D112825921, the `registerArrayClassType` removal in D112825909, and the
  `narrowToFlowType` pass-through in D112825915.
- [ ] `HANDOFF.md`: it is dated 2026-06-09 and says Phase 4 has not started.
  Rewrite TL;DR, "What actually landed", "What remains" and the build
  conventions (devserver scratch build dir, multi-process lit).
- [ ] Commit as `[SH]: Record IR type Phase 4 progress`.

---

## Part B — Phase 5: Tuple + ExactObject

The `TypeEntry` payloads already exist (`include/hermes/IR/TypeContext.h`,
`tuple` and `exactObject`), as does the shared `fieldArrays_` side array.
Today both kinds lower to `object` (`lib/IRGen/ESTreeIRGen-typed-class.cpp:753-755`).

**Decisions this part adds** (extending D1–D6). Both were **approved by the
owner on 2026-11-07**. Each must be documented clearly in two places: a
"D7"/"D8" section in phase3-5-design.md (written in Task B2, before any code),
and a comment at the code that implements it (the lowering in
`flowTypeToIRType` and the `createExactObject` declaration in
`TypeContext.h`), naming the decision and its rationale:

- **D7 — indexers stay `object`.** A Flow `ExactObjectType` with an indexer
  (`getIndexer()`, used in `FlowChecker-scopetypes.cpp:202-205`) lowers to
  `object`. An indexer is an open key set, so the IR `ExactObject` (a closed,
  ordered field list) cannot represent it.
- **D8 — field variance is not part of the IR type.** Flow's
  `ExactObjectType::Field` carries `FieldVariance`. Under D4, object types
  are compared by id and the IR never checks writes, so `{+x: number}` and
  `{x: number}` share one IR id. Revisit only together with deep subtyping.

### Task B1: Tuple factory, interning, reserve/fill, accessors

**Status (2026-11-07): done** in `[SH]: Add interned Tuple factory to
TypeContext`.

- [ ] Unit tests first, mirroring the Array tests: identical element lists
  intern to one id, order matters, a reserved-then-filled self-referential
  tuple works.
- [ ] `createTuple(ArrayRef<Type>)` interned by an ordered element-id key
  (extend the intern-key machinery alongside `UnionInternKey`,
  `include/hermes/IR/TypeContext.h:280-310`, without its sort/dedup);
  `reserveTuple`/`fillTuple`; `getTupleElements` with the same
  invalidation warning as `getClassFields`.
- [ ] Commit.

### Task B2: ExactObject factory, interning, reserve/fill, accessors

**Status (2026-11-07): done** in `[SH]: Record IR type decisions D7 and
D8`, `[SH]: Copy aliasing inputs in class/function fill` (a fix found on
the way) and `[SH]: Add interned ExactObject factory to TypeContext`.

- [ ] Write the D7 and D8 sections in phase3-5-design.md (decision,
  rationale, what would make us revisit it).
- [ ] Unit tests first: `{x, y}` ≠ `{y, x}`, identical fields intern, a
  self-referential object (`type Tree = null | {left: Tree, right: Tree}`
  built by hand).
- [ ] `createExactObject(ArrayRef<ExactObjectField>)` interned by ordered
  `(name, type id)` pairs; `reserveExactObject`/`fillExactObject`;
  `getExactObjectFields`.
- [ ] Commit.

### Task B3: Algebra and printing for Tuple/ExactObject

**Status (2026-11-07): done** in `[SH]: Print tuples and exact objects;
test algebra` (no algebra code change).

The refined-kind rules in `isLeafSubtypeById`/`areLeafTypesDisjointById`
are written for any refined kind, so no new algebra code is expected.

- [ ] Unit tests: subtype of `Object`, not a subtype of a different tuple or
  object, not disjoint from another structural kind (D5), disjoint from
  primitives; union subsumption with `Object`.
- [ ] Printing: `tuple(a, b)` and `exactObject(x: a, y: b)`, using A2's cycle
  guard.
- [ ] Commit.

### Task B4: Lower Tuple and ExactObject

**Status (2026-11-07): done** in `[SH]: Lower Tuple and ExactObject`.

- [ ] Lit tests first: tuple parameters and returns, object-literal types,
  the recursive `Tree` alias and a recursive tuple alias. Confirm the
  recursive ones dump `object` today, so they will start exercising the cycle
  path once lowered.
- [ ] Extend A1's lazy reservation to `TupleType` and `ExactObjectType`
  (indexer → `object` per D7). The `Tree` alias is where a cycle passes
  through an `ExactObject` (design §Phase 5). Comment the indexer fallback
  (D7) and the dropped `FieldVariance` (D8) at the lowering site, and add lit
  checks that an indexed object type dumps `object` and that `{+x: number}`
  and `{x: number}` lower to the same IR type.
- [ ] Commit.

### Task B5: Consumer audit and allocation typing for Tuple/ExactObject

**Status (2026-11-07): done** in `[SH]: Type tuple/object literal
allocations` and `[SH]: Look through trusted narrows in stack promotion`.
The `StoreFrameInst` test is covered by B4's `tuple-object-type-exec.js`;
the exact-`Object` search was re-run in B7 and found no new site.

Tuple and object literals are allocated as `object`
(`genTupleFromElements`, `lib/IRGen/ESTreeIRGen-expr.cpp:487-495`; object
literals at `:2517-2542`). Once their Flow types lower to refined types,
a store into a refined-typed slot hits `StoreFrameInst`'s strict subset check
(HANDOFF "Gotchas"), and a returned literal gets a runtime cast.

- [ ] Write the failing test first: a typed tuple literal and a typed object
  literal, each stored into a typed `let` captured by a closure (that forces
  `StoreFrameInst`) and returned.
- [ ] Trusted-narrow the allocations to the lowered type at creation, as in
  A3.
- [ ] Re-run A3's exact-`Object` search; add the new kinds to any switch that
  lists refined kinds (`SH.cpp:2430-2440` already lists them).
- [ ] Commit.

### Task B6: Totality pass

**Status (2026-11-07): done** in `[SH]: Intersect to the subset operand
exactly` (the one law that failed) and `[SH]: Check type algebra laws
over all sample pairs`.

- [ ] One parameterized unit test over all ordered pairs drawn from
  {NoType, each primitive, Object, Int32, ClassInstance(×2, related and
  unrelated), Array(×2), Function(×2), Tuple(×2), ExactObject(×2), a union of
  two refined kinds, a union of refined and primitive}. For every pair, check
  the expected laws rather than hand-written tables:
  - `isSubsetOf(a, unionTy(a, b))`
  - `isSubsetOf(intersectTy(a, b), a)`. Not `isSubsetOf(intersectTy(a, b),
    b)`: for two overlapping leaves where neither is a subtype of the other
    (two distinct structural refined types, or a class and a structural
    type), `intersectTy` returns `a` (D9 in phase3-5-design.md), which is not
    a subset of `b`.
  - `areDisjoint(a, b)` ⇒ `intersectTy(a, b)` is NoType
  - `isSubsetOf(a, b) && isSubsetOf(b, a)` ⇒ `a == b`
  - `isSubsetOf(subtractTy(a, b), a)`
  - termination on self-referential entries
- [ ] Fix whatever fails; commit.

### Task B7: Regression, baselines, progress

**Status (2026-11-07): done** in `[SH]: Record IR type Phase 4 and 5
progress`.

- [ ] As A4, then as A5 for Phase 5 (`progress.md` rows, `HANDOFF.md`).

---

## Out of scope (recorded so it is not lost)

- **Deep structural subtyping** (function variance, object width/depth;
  deferred by D4). When it lands, delete the `narrowToFlowType` pass-through
  for structurally compatible refined types.
- **Runtime type descriptors**, which would let codegen check refined kinds
  precisely instead of as `object` (relaxing D2).
- **An `Intersection` type kind** (the exact meet of two overlapping,
  unrelated types; would replace D9, under which `intersectTy` returns its
  first operand for such pairs).
- **Structural dedup of independently declared, identical recursive
  types.** Distinct ids are sound. A1 adds dedup only if its identity check
  shows the same alias reaching IRGen through distinct `TypeInfo`s.

## Self-review

- A1 depends on Flow caching `Array<T>` specializations so that one element
  type yields one `ClassType`. `getSpecializedArrayClassType` goes through
  `specializeGenericWithParsedTypes`, which reuses a cached specialization
  (`generic.getSpecialization(typeArgs)`, `lib/Sema/FlowChecker.cpp:3748`). If that ever
  stops holding, A1's identity check catches it.
- A1's lazy reservation fills an entry only after its children are lowered.
  Types built in between that reference the reserved id (unions, interned
  arrays/functions keyed by it) stay valid, because ids are stable and fill
  changes only the payload.
- A3's SH negative-check item is the only possible miscompile here. Its
  reachability from JS is unproven, so the plan requires trying to build the
  counterexample before deciding how to test it.
