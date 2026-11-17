# IR Type System v2 — Session Handoff

Last updated: 2026-11-08. Purpose: let a fresh session continue the IR type
work without context loss. Read this first, then the docs it points to.

## TL;DR

- **Phases 1–5 are done and green.** Every refined Flow type now lowers to a
  live refined IR type: classes to `classInstance(C)`, `Array<T>` to
  `array(T)`, typed functions and the constructors of classes with an
  explicit constructor signature to `function(...)` (other class
  constructors stay `object`), tuples to `tuple(...)`, indexer-free exact
  objects to `exactObject(...)`.
  Recursive types through any of these lower, print and run through the
  algebra without diverging.
- The work is a stack of nine local draft commits (see "Commit stack"),
  squashed from the development history on 2026-11-08 and not resubmitted;
  the owner reviews and submits.
- **What remains** is out of scope for Phases 3–5: deep structural subtyping,
  runtime type descriptors, an `Intersection` kind, and the small open issues
  listed at the end of this file.
- An external review (Codex) of every commit was addressed; the fixes are
  folded into the squashed commits. See "Codex review" below.

## Commit stack (squashed 2026-11-08)

The development history (about 60 commits: task commits, review rounds and
fix sets) was squashed into nine commits, bottom first. Fixes were folded
into the commit that introduced the problem, except where a fix's test needs
a later kind (then it sits in the later commit). The stack was published
on 2026-11-10; the other diffs of the development history were abandoned.

1. `[SH]: Lower Flow classes to IR ClassInstance types` (D112825925):
   `ClassInstance` in `TypeContext` (reserve with superclass, fill,
   id-aware algebra, printing), live lowering of Flow classes, the D2
   consumer audit (verifier, SH, InstSimplify, IREval, Auditor), the SH
   negative cast check fix, lit baselines.
2. `[SH]: Delete dead flow::ArrayType; add BuiltinReceiverKind`
   (D112825921).
3. `[SH]: ClassType provenance; drop array side table` (D112825909).
4. `[SH]: Lower Flow arrays and functions to IR types` (D112825918):
   `Array`/`Function` in `TypeContext`, lowering of `Array<T>`, typed
   functions and class constructors, recursive structural lowering with
   Flow-equality dedup, cycle-safe printing (`^N`), array allocation typing.
5. `[SH]: Lower Flow tuples and exact objects to IR types` (D130912970):
   `Tuple` and
   `ExactObject` in `TypeContext`, their lowering (D7, D8), literal
   allocation typing, the factory/invalidation tests.
6. `[SH]: D9: intersectTy of unrelated types keeps 1st` (D130913111): D9,
   the subset
   rule, the type-algebra law (totality) tests.
7. `[SH]: Narrow compatible function values on store` (D130913112).
8. `[SH]: Look through trusted narrows in stack promotion` (D130913113).
9. `[SH]: Document IR type system v2 Phases 3-5` (D112825922): everything
   under `doc/plans/ir-type/`.

## Document map (all in `doc/plans/ir-type/`)

- `phase3-5-design.md` — the approved design for Phases 3–5 with the binding
  decisions **D1–D10**. Read the decisions first.
- `remaining-plan.md` — the plan that finished Phase 4 (Tasks A1–A5) and
  implemented Phase 5 (B1–B7). Supersedes tasks 7–10 of `phase4-plan.md`.
  Each task has a status line naming the development commit that
  implemented it; those commits were squashed into the stack above.
- `progress.md` — status table and context notes; the "Phase 4" and
  "Phase 5" notes are the detailed record of what landed and why.
- `phase3-plan.md`, `phase4-plan.md` — the earlier task plans (Phase 4 tasks
  1–6 were executed from `phase4-plan.md`).
- `ir-type-system-v2-design.md`, `ir-type-v2-implementation.md` — the
  original 8-phase design and C++ data structures. Their interning algorithm
  (`isLooping` routing, batch reserve/fill) was not used; see D3 below and
  the Phase 4 note in `progress.md`.
- `memory.md` — quick reference for the Phase 1–2 `TypeContext` internals.

## Decisions (one line each; full text in `phase3-5-design.md`)

- **D1**: refined types flow live end-to-end (IRGen → optimizer → codegen),
  not behind a flag.
- **D2**: codegen and the verifier treat every refined object kind exactly
  like `Object` (sound because refined ⊂ object) until runtime type
  descriptors exist.
- **D3**: recursive types are represented accurately as cycles in the
  type-id graph; only lowering needs cycle handling (reserve → fill), the
  algebra compares ids.
- **D4**: shallow subtyping: classes nominal through the superclass chain;
  arrays, tuples, functions and exact objects equal only to themselves (by
  id) and subtypes of `Object`.
- **D5**: `areDisjoint` is conservative: only unrelated classes and
  refined-vs-primitive are disjoint; distinct structural types overlap.
- **D6**: `ClassInstance` stores its fields and is not interned (one entry per
  class declaration).
- **D7**: an exact object type with an indexer lowers to `object`.
- **D8**: field variance is not part of the IR exact-object type or its key.
- **D9**: `intersectTy(a, b)` of two overlapping leaves with no subtype
  relation returns `a` (approximating the meet from above).
- **D10**: precise runtime checks of structural kinds require deep
  structural subtyping (Phase 7 depends on it, except for classes).

## What landed in Phases 4–5 (file level)

Trust this section and `progress.md` over the plans where they differ.

### `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`
- Interned factories, each with `reserve*`/`fill*` for recursive shapes and
  accessors: `createArray(elem)` (key: element id), `createFunction(...)`
  (key: full shallow signature; params in `paramArrays_`),
  `createTuple(elems)` (key: ordered element ids, not sorted or deduped),
  `createExactObject(fields)` (key: ordered `(name, type id)` pairs; fields
  share `fieldArrays_` with classes). Every `fill*` copies its input first,
  so inputs may point into the context's own side arrays.
- Algebra: no new rules were needed for the new kinds (the id-aware
  `isLeafSubtypeById`/`areLeafTypesDisjointById` cover every refined kind).
  `intersectLeafTy` implements D9; `intersectTy` keeps `a`'s arm first for
  unions and returns `b` exactly when `isSubsetOf(b, a)`.
- Printing: `print` → `printType` → `printLeaf`, with
  `printStructuralPayload` shared by the four structural kinds. Union
  payloads print their arms. A structural type reached again while its
  payload is printed prints `^N` (N counts enclosing structural types, 1 =
  innermost): `type F = (x: number) => F` is `function(^1)`. Functions print
  only their return type. Forms: `array(T)`, `function(R)`, `tuple(a, b)`,
  `exactObject(x: a, y: b)`.

### `lib/IRGen/`
- `ESTreeIRGen.h`: `FlowTypeStructuralInfo` (DenseMap traits: Flow's
  co-inductive `TypeInfo::equals` plus a hash that mixes in children),
  `loweringInProgress_`, `loweredStructuralTypes_`.
- `ESTreeIRGen-typed-class.cpp`: `lowerArrayClassType`, `lowerTypedFunction`
  (also used for class constructors), `lowerTupleType`,
  `lowerExactObjectType` (D7/D8 comments), all through
  `beginStructuralLowering`/`finishStructuralLowering`/
  `recordStructuralLowering` (lazy reservation + Flow-equality dedup).
- `ESTreeIRGen-expr.cpp`: allocations are trusted-narrowed to their lowered
  type right after creation: typed array literals and
  `$SHBuiltin.objectKeys/Values/Entries` (`array(T)`), tuple literals,
  typed object literals and object rest bindings. The narrow of the
  `Array<T>` prototype back to `object` is gone. `narrowToFlowType` emits a
  `UnionNarrowTrustedInst` for structurally compatible refined types that
  are neither subset of the other.
- `ESTreeIRGen-func.cpp` + `include/hermes/IR/IR.h`, `Instrs.h`:
  `Function::functionType_` holds a typed function's IR type;
  `CreateFunctionInst` and TypeInference take the closure type from it.

### Flow (`include/hermes/Sema/FlowContext.h`, `lib/Sema/`)
- `flow::ArrayType` deleted; builtin methods keyed by
  `BuiltinReceiverKind {Array, String, Number}`.
- `ClassType::genericClassDecl_`; `isArrayClassType` derives from it; the
  wrapper-keyed array side table is gone.

### Optimizer and backends
- `lib/Optimizer/Scalar/ObjectStackPromotion.cpp`: `collectObjectAliases`
  treats an allocation and the trusted narrows reachable from it as one
  object.
- `lib/Optimizer/Scalar/InstSimplify.cpp`: `===`/`!==` fold uses
  `areDisjoint`; `simplifyThrowIf` passes the invalid types first to
  `intersectTy`.
- `lib/Optimizer/Scalar/Auditor.cpp`: counts every subset of `object` as
  object.
- `lib/BCGen/SH/SH.cpp`: `_typeCastHelper` checks the accepted arms when a
  rejected and an accepted arm can both be objects.

### Tests
- `unittests/IR/TypeContextTest.cpp`: construction, interning,
  reserve/fill, fill-from-own-entries, algebra and printing for every
  kind; `TypeAlgebraTotalityTest` checks 8 laws over all 1764 ordered pairs
  of a 42-type sample (every leaf kind), self-referential entries included.
- Lit: `IRGen/flow/{function-type,function-signature-type,recursive-type,
  tuple-object-type,exact-object-variance-indexer,class-mutual-recursion,
  class-reserved-superclass}.js`,
  `shermes/{recursive-type-exec,checked-cast-refined,
  intersect-unrelated-function-types,function-type-static-field,
  function-type-static-field-nullable,tuple-object-type-exec}.js`,
  `Optimizer/flow/{array-literal-type,tuple-object-literal-type,
  object-stack-promotion-narrow}.js`.
- **Green bar (2026-11-07, end of Phase 5)**: all targets build except
  `mermes` (see What remains); `HermesIRTests` 121, `HermesVMRuntimeTests`
  405, every other unittest binary passes; full lit 4537 expected passes, 6
  expected failures, 345 unsupported, 0 unexpected; `update-lit` changes
  nothing. **After review fix set 3**: `HermesIRTests` 125; full lit 4544
  expected passes, 6 expected failures, 345 unsupported, 0 unexpected.
  **After the publication review (2026-11-10)**: `HermesIRTests` 126; full
  lit 4548 expected passes, 6 expected failures, 345 unsupported, 0
  unexpected; every commit builds and passes on its own.

## Publication review (2026-11-10)

Before publishing, an independent reviewer read each of the nine commits.
No design or code problem was found. Commit-message and doc corrections
were made, and every reviewer nit and follow-up was fixed and folded into
the commit it belongs to:
- `isLeafSubtype`/`areLeafKindsDisjoint` assert that neither kind is
  refined; `IRBuilder::createLoadParentNoTrapsInst` asserts that the
  operand is a subset of `object`.
- One copy of the `Array` generic Decl (in `FlowContext`).
- Both `flowTypeToIRType` overloads recognize `Array<T>` in the Class arm;
  typed async and generator closures carry their function type;
  `getRawIdForTest` is gone (`getRawId` is public).
- Object rest bindings are narrowed once (see the open issues below).
- Exact-object field names that are not identifiers print quoted.
- The store narrow in `narrowToFlowType` has a source location.
- New tests: storing a narrowed object as a value keeps its allocation;
  the narrow chain and the exact-object store narrow are tested without
  rest bindings (readonly-field covariance, `{+x: number | string}`).
Skipped: making `isSubsetOf(t, object)` exclude `NoType` at its six
consumer sites. `NoType` values only occur in unreachable code there, so
any answer is sound, and excluding it in the verifier would reject valid
unreachable IR.

## Codex review (2026-11-07)

An external reviewer (Codex) reviewed every commit of the stack (its
report lived on a machine that has since been released). The fixes
(new tests, comment and doc corrections, and one real fix) were made as three
fix sets and are now folded into the squashed commits they belong to.

Outcomes:
- The one BLOCKER (the tuple/object exec test passed without the lowering)
  was refuted: the same commit's `IRGen/flow/tuple-object-type.js` baseline
  fails without it, and the exec test now also checks the IR.
- Class superclass reservation: the class arm of
  `flowTypeToIRType` used to reserve a class with no superclass and lower
  its fields before filling it, so unions inside cached structural types
  could keep a subclass arm beside its superclass, and their IR type
  depended on lowering order. The superclass is now lowered first and
  passed to `reserveClassInstance(superClass)`.
- Accepted limitation: every `fill*`
  that takes a list copies it before appending it to a side array, so the
  list may point into the context's own entries. The tests of that
  (`ClassInstanceFillFromOwnFields`, `ExactObjectFillFromOwnFields`,
  `FunctionFillFromOwnParams`; `fillTuple` has none) cannot fail on
  libstdc++ when the copy is removed, because libstdc++ tolerates the
  self-range `vector::insert`; only a `_GLIBCXX_DEBUG` build detects it.
- The commit-message findings were corrected in the squash. The wrong header
  path in `phase4-plan.md` is left as is: the plans are records.

## Working conventions (this environment)

- **Machine**: a Linux devserver; the checkout is `/data/repos/fbsource`
  (Sapling on EdenFS). Project: `xplat/static_h`.
- **Build dir outside the checkout**: never build inside EdenFS. The current
  build is `B=/data/users/tmikov/irtype/build-asan` (ASan+Debug, `-O1`,
  configured with the flags in `CLAUDE.md`). Scratch files go under
  `/data/users/tmikov/irtype/work/`.
- **Compiler**: on on-demand devservers configure with clang
  (`-DCMAKE_C_COMPILER=/usr/local/bin/clang.par
  -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++.par`); gcc configure fails
  there.
- **Build**: `ninja -C $B -j64 shermes hermes HermesIRTests` while
  iterating; `ninja -C $B -j64` (no target) for a full build.
- **Unit tests**: `ASAN_OPTIONS=detect_leaks=0
  $B/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.*'`.
- **Lit** runs multi-process (~30 s for the full suite on a large box):
  `ASAN_OPTIONS=detect_leaks=0 ninja -C $B check-hermes`; filter with
  `LIT_FILTER=<regex>`. `check-hermes` also runs the gtest binaries, so new
  unit tests raise the lit pass count. Known flake:
  `shermes/regress-bigint-type-inference.js` may abort with "Resource
  temporarily unavailable" while starting a thread; rerun it.
- **`update-lit` workflow**: `ASAN_OPTIONS=detect_leaks=0 ninja -C $B
  update-lit` regenerates `%FileCheckOrRegen` baselines. Run it only after
  you know why each test fails, then read `sl diff` of every changed test
  and revert anything you cannot explain. A clean run leaves `sl status`
  empty.
- **Recursive-type programs** run under `timeout 60`: the failure mode before
  a fix is unbounded recursion.
- **Format**: `arc f` on touched `.cpp`/`.h`.
- **Source control**: commit explicit paths only. Fold a fix into the commit
  it fixes (`sl amend`, `sl amend --to`, `sl histedit --plan`) when that is
  where it belongs; do not `jf submit` without the owner asking. Commit
  messages use the Meta format (`[SH]: subject` ≤ 50 chars, Summary, Test
  Plan with how each new test failed before the change, `Reviewers:
  #hermes`).
- **Evidence**: follow `DESIGN-PRINCIPLES.md`: cite code for every claim,
  build the counterexample before calling something unreachable, and show
  every new test failing without its change (or under a named mutation).

## Gotchas learned

- **`StoreFrameInst` is strict**: the verifier requires `isSubsetOf(value,
  variable)`. A slot typed with a refined type rejects a value typed
  `object` or an unrelated refined type. Type the value (narrow the
  allocation, or narrow in `narrowToFlowType`) rather than widening slots.
- **D9 argument order**: `intersectTy(a, b)` is a subset of `a`, not
  necessarily of `b`. Callers pass the type they keep first (cast/narrow
  target, pre-pass type, invalid-types set). Emptiness (`isNoType()`) does
  not depend on order.
- **Passes that rewrite a value's type in place** (rather than typing a new
  value) must keep the new type a subset of the old one, because other users
  rely on it. With D9 an intersection need not be, so the old type must be
  the first operand. TypeInference's `checkAndSetPrePassType` does this: it
  writes `intersectTy(prePassType, inferred)` onto the value, a subset of
  the type it had before the pass. `LowerNanBoxedUnionNarrowT` (SH and
  `facebook/Mins` copies) also rewrites in place but has no caller, so no
  commit in the stack changes it.
- **`UnionNarrowTrustedInst` must be looked through by escape analyses.**
  Typed allocations are now followed by a trusted narrow; a pass that treats
  the narrow as an escape loses the allocation (see
  `ObjectStackPromotion`'s `collectObjectAliases`).
- **Reserved entries**: for the structural kinds (array, function, tuple,
  exact object) kind and id are valid immediately, the payload only after
  `fill*`; the algebra may run on them unfilled because it reads only their
  kind and id. A class is reserved with its superclass
  (`reserveClassInstance(superClass)`): kind, id and superclass are valid at
  reserve, and they are all the algebra reads (`isSubclass`); name and
  fields only after `fillClassInstance`. Any change that makes the algebra
  read another payload breaks `ReservedUnfilledEntriesInAlgebra`,
  `ClassInstanceReservedWithSuperclass` and the lazy lowering.
- **D8 usage constraint**: never type a loaded value from an IR
  `ExactObject` field type; for a write-only field it is a lower bound.
- **Accessor invalidation**: `getClassFields`, `getExactObjectFields`,
  `getTupleElements`, `getFunctionParams` return views into side arrays that
  creation/fill and union creation can reallocate. Copy before creating
  types.
- **Flow hashes are shallow**: `TypeInfo::hash` reads only the node, so
  every function of one arity (and every tuple of one length) collides;
  `FlowTypeStructuralInfo` mixes in the children.
- **The SH backend checks every refined kind as `_sh_ljs_is_object`**, so a
  cast whose rejected and accepted arms can both be objects must check the
  accepted arms.
- **`UnionNarrowTrustedInst`/`CheckedTypeCastInst` verifiers are
  permissive**, so a wrong narrow does not fail verification; it shows up as
  a miscompile at `-O`.
- **Stale clangd diagnostics** after header edits: a real build is the
  source of truth.

## What remains

Out of scope (from `remaining-plan.md`):
- **Deep structural subtyping** (function variance, object width/depth;
  deferred by D4). When it lands, delete the `narrowToFlowType` trusted
  narrow for structurally compatible refined types, and revisit D8 and D9.
- **Runtime type descriptors**, so codegen can check refined kinds precisely
  instead of as `object` (relaxing D2). Constrained by **D10**: precise checks
  of structural kinds (functions, exact objects, arrays, tuples) need deep
  structural subtyping and a strategy for values without descriptors first;
  on the shallow relation they would reject valid values. Within that work,
  only class checks (nominal) can come before deep subtyping. Order: class
  checks; deep subtyping plus the untyped-value strategy; structural checks.
- **An `Intersection` type kind** (the exact meet of two overlapping,
  unrelated types); it would replace D9.
- **Structural dedup of independently declared, identical recursive types.**
  Distinct ids are sound. (Flow-equal types are already deduplicated.)

Open issues, filed in `dz/` (`dz show <id>`):
- `01a1c623-785b`: at `-O`, after a class instance is scalar-replaced, the
  `LoadFrameInst` of its prototype (`?P.prototype`) stays behind unused.
- `01a1c623-7bb6`: the program wrapper's `exports` argument `{}` is typed
  `exactObject()`, so every typed IRGen dump has a narrow on it.
- `01a1c623-7eed`: doubt, unproven: recursive-type dedup may be
  order-dependent, i.e. which of two Flow-equal types is lowered first
  could decide whether they share a reserved entry or get different ids.
  (A different order dependence, of class unions inside cached structural
  types, was real and is fixed in "Lower Flow classes to IR ClassInstance
  types".)
- `01a1c623-822f`: home objects (`?P.prototype`, `?Array.prototype`) print as
  `classInstance()` with an empty name, in `-dump-ir` and `-dump-lir`.
- `01a1c623-8582`: `mermes` (`tools/facebook/mermes`) does not link (missing
  `hermesHBCBackend`). Not caused by this work; it makes a target-less
  `ninja` exit non-zero.
- `01a1cb5b-f083`: typed mode has no promise or generator types. For an
  async function or a generator, the return type of the Flow
  `TypedFunctionType` and of the IR `Function` type is the annotation (the
  promise's value type, or the generator's return value), not the type of a
  call. A call is `mixed` in Flow (`FlowContext::getCallResultType`) and
  `object` in the IR (`emitCall`) until real types exist; deep structural
  subtyping must compare these return types as annotations (see the note
  after D10 in `phase3-5-design.md`).

Bugs found while testing the review follow-ups, all also present on the
base master commit, before this stack:
- `01a1c623-7516`: `new Array<number>()` allocates a typed object, not an
  array; `push` on it aborts.

Fixed after the review, following `typed-mode-bugs-plan.md`:
- `01a1c623-71c5` (typed parameters of async and generator functions
  failed IR verification after `LowerGeneratorFunction`): fixed by "Keep
  param types when lowering generators".
- `01a1c623-6e9a` (a call to a typed async or generator function was typed
  with the annotated return type: a promise held in a `number` binding, and
  a generator call failed verification at `-O`): fixed by "Type
  async/generator calls as mixed/object".

The redundant double narrow of an object rest binding whose type equals a
recursive alias is fixed: FlowChecker records the rest type on the
`RestElementNode`, and IRGen narrows the allocation once, to its lowered
Flow type.

## How to resume

1. Read `phase3-5-design.md` (D1–D10), this file, and the Phase 4/5 notes in
   `progress.md`.
2. Check the stack: `sl log -r 'draft() & ::.' -T '{node|short}
   {desc|firstline}\n'` from the checkout that holds the stack.
3. Check the green bar: `ninja -C $B -j64 shermes hermes HermesIRTests`,
   `HermesIRTests` (126), `ninja -C $B check-hermes` (4548 expected passes,
   0 unexpected).
4. New work starts with a design decision or plan of its own; the next
   natural phase is deep structural subtyping (D4's deferred part).
