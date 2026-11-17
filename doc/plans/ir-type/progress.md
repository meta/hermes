# Implementation Progress

Tracks progress of `doc/plans/ir-type/phase1-steps.md` for
`doc/plans/ir-type/ir-type-system-v2-design.md` and
`doc/plans/ir-type/ir-type-v2-implementation.md`.

The file has two sections: "Status" and "Context Notes".

**Status Section**:

Each row contains the step label from the detailed, plan, a brief description, list of
dependency labels, Status (initially empty), optional brief note (initially empty).

The status of a row is one of:
- "" (empty) initially, before work has started
- "wip" as soon as work on that raw has started.
- "done" when work has completed successfully. Rarely "Brief Note" may contain very brief
explanation. More details in "Context Notes".
- "blocked" when work cannot proceed for some reason. "Brief Note" must contain a brief
explanation. More details in "Context Notes".

Example on start:

| Step 11 | Port util binding | 5 |  |  |

**Context Notes**:

After completing work on a step, either successfully or by blocking, a section for that
step is added. It needs to have the format from the following example (empty bullets can
be omitted):

```
### Step 11: Port util binding
- **Files**: created `foo.c`, modified `bar.c`.
- **Decisions**:
-- Decision 1 concise explanation
-- Decision 2 concise explanation
- **What was done**: ...
- **Issues**: ...
- **Notes for next step**: ...
```

## Status

| Step | Description | Depends On | Status | Brief Note (optional) |
|------|-------------|------------|--------|-----------------------|
| P1-S1 | TypeContext skeleton with well-known types | — | done | |
| P1-S2 | Type queries on TypeContext | P1-S1 | done | |
| P1-S3 | Type operations with interning | P1-S2 | done | |
| P1-S4 | Utility methods | P1-S3 | done | |
| P1-S5 | Thread-local context and RAII guard | P1-S1 | done | |
| P1-S6 | Wire TypeContext into Module | P1-S1 | done | |
| P1-S7 | Install RAII guards at compilation entry points | P1-S5, P1-S6 | done | |
| P1-S7.5 | Add RAII guards to unit tests | P1-S7 | done | |
| P1-S8 | Rewrite Type class | P1-S4, P1-S7.5 | done | |
| P2-S1 | Foundation: accessors and conventions | P1-S8 | done | |
| P2-S2 | Migrate IRVerifier to explicit context | P2-S1 | done | |
| P2-S3 | Migrate Instrs.{h,cpp} and IR.cpp | P2-S1 | done | |
| P2-S4 | Migrate IRGen (ESTreeIRGen-*.cpp) | P2-S1 | done | |
| P2-S5 | Migrate InstSimplify pass | P2-S1 | done | |
| P2-S6 | Migrate TypeInference pass (heaviest) | P2-S1 | done | |
| P2-S7 | Migrate remaining optimizer + BCGen | P2-S1 | done | |
| P2-S8 | Migrate test fixtures | P2-S1 | done | |
| P2-S8.5 | Migrate IRPrinter (Dumper) off TLS Type printing | P2-S1 | done | |
| P2-S9 | Remove TLS infrastructure | P2-S8.5 | done | |
| P3-S1 | ClassInstance factory, side array, accessors | P1-S8 | done | |
| P3-S2 | Id-aware nominal subtyping (isSubclass) | P3-S1 | done | |
| P3-S3 | Id-aware disjoint/intersect/union | P3-S2 | done | |
| P3-S4 | Pretty-print ClassInstance | P3-S1 | done | |
| P3-S5 | Lower Flow ClassType to ClassInstance (live) | P3-S3 | done | merged with P3-S6 |
| P3-S6 | Consumer audit: codegen/verifier treat refined as object | P3-S5 | done | landed with P3-S5 |
| P3-S7 | Full regression + lit baselines | P3-S6 | done | |
| P3-S8 | Update progress tracking | P3-S7 | done | |
| P4-T1 | Array factory, interning, reserve/fill, accessor | P3-S8 | done | |
| P4-T2 | Function factory, `paramArrays_`, interning, reserve/fill, accessors | P4-T1 | done | |
| P4-T3 | Type algebra for Array/Function | P4-T2 | done | tests only; algebra already id-aware |
| P4-T4 | Pretty-print Array and Function | P4-T2 | done | |
| P4-T5 | Lower `Array<T>` to `array(T)` (non-recursive) | P4-T3 | done | needed `flow::ArrayType` deletion + ClassType provenance |
| P4-T6 | Lower TypedFunction and ClassConstructor to `function(...)` | P4-T5 | done | via `Function::functionType_` |
| P4-T7..T10 | Looping path, audit, regression, progress | P4-T6 | done | superseded by remaining-plan A1–A5 |
| P4-A1 | Lower recursive array and function types | P4-T6 | done | lazy reservation + Flow-equality dedup |
| P4-A2 | Cycle-safe, lossless type printing | P4-A1 | done | `^N` back-references |
| P4-A3 | Consumer audit for Array/Function | P4-A1 | done | 5 commits; SH negative-check miscompile fixed |
| P4-A3b | D9: intersecting unrelated types; narrow compatible functions on store | P4-A3 | done | added during execution |
| P4-A4 | Full regression and baselines | P4-A3b | done | combined with P5-B7; no baseline changes |
| P4-A5 | Record Phase 4 progress | P4-A4 | done | combined with P5-B7 |
| P5-B1 | Tuple factory, interning, reserve/fill, accessors | P4-A1 | done | |
| P5-B2 | ExactObject factory, interning, reserve/fill, accessors; D7/D8 | P5-B1 | done | plus fix: copy aliasing inputs in class/function fill |
| P5-B3 | Algebra and printing for Tuple/ExactObject | P5-B2 | done | no algebra code change |
| P5-B4 | Lower Tuple and ExactObject | P5-B3 | done | |
| P5-B5 | Allocation typing for Tuple/ExactObject literals | P5-B4 | done | plus ObjectStackPromotion looks through narrows |
| P5-B6 | Totality pass | P5-B5 | done | found and fixed one union-intersection bug |
| P5-B7 | Regression, baselines, progress | P5-B6 | done | |
| R1 | Codex review: fix sets 1–3 | P5-B7 | done | folded into the squashed commits; see "Codex review" |

## Context Notes

### Codex review (2026-11-07)
An external reviewer (Codex) reviewed every commit of the stack
(`/data/users/tmikov/irtype/work/codex/findings-r1.md`). The fixes were
made as three fix sets (tests and accessor docs; lowering tests and one fix;
comments and docs) and were folded into the squashed commits on 2026-11-08
(see "Commit stack" in HANDOFF.md).
- **Outcomes**: the BLOCKER (exec test passes without the lowering) was
  refuted: the same commit's `IRGen/flow/tuple-object-type.js` baseline
  fails without it. Class lowering now lowers a class's superclass
  before reserving the class and passes it to
  `reserveClassInstance(superClass)`; before, the reserved class had no
  superclass while its fields were lowered, so class unions inside cached
  structural types were not normalized and depended on lowering order.
  Accepted limitation: the
  fill-from-own-entries tests cannot fail on libstdc++ when the defensive
  copy is removed (it tolerates the self-range `vector::insert`); only a
  `_GLIBCXX_DEBUG` build detects it. The commit-message findings were
  corrected in the squash; the `phase4-plan.md` header path is left as is.
- **Green bar after fix set 3**: `HermesIRTests` 125; full lit 4544
  expected passes, 6 expected failures, 345 unsupported, 0 unexpected.

### Phase 5: Tuple + ExactObject, wired live
See [remaining-plan.md](remaining-plan.md) Part B and
[phase3-5-design.md](phase3-5-design.md) (D7, D8, D9).
- **What was done**: `TypeContext` gained interned `createTuple` (key: the
  ordered element ids, neither sorted nor deduplicated) and
  `createExactObject` (key: the ordered `(name, type id)` pairs; field order
  is significant), each with `reserve*`/`fill*` for recursive shapes and an
  accessor (`getTupleElements`, `getExactObjectFields`). Exact-object fields
  share `fieldArrays_` with class fields. Printing: `tuple(a, b)` and
  `exactObject(x: a, y: b)` (empty forms `tuple()`, `exactObject()`), with
  the payload printing of all four structural kinds factored into
  `printStructuralPayload` and covered by the `^N` cycle guard. Flow
  `TupleType` lowers to `tuple(...)` (`lowerTupleType`) and an indexer-free
  `ExactObjectType` to `exactObject(...)` (`lowerExactObjectType`), both
  through Phase 4's lazy reservation, so `type Tree = null | {val: number,
  left: Tree, right: Tree}` and recursive tuple aliases terminate. Typed tuple
  literals, object literals and object rest bindings are trusted-narrowed
  to their lowered type right after allocation (`genTupleFromElements`,
  `genTypedObjectExpr`, `emitDestructuringTypedObject`), so no runtime cast
  is emitted where they meet their Flow type.
- **Decisions**:
-- D7: an exact object with an indexer lowers to `object` (an open key set
   is not a closed field list). Its key/value types are not lowered, so a
   cycle through an indexer is cut there. Documented at the lowering site.
-- D8: field variance is not part of the IR type or its intern key;
   `{+x: number}`, `{-x: number}` and `{x: number}` share one id. Usage
   constraint: for a `WriteOnly` field the Flow type is a lower bound, so no
   consumer may type a loaded value from an IR `ExactObject` payload.
   Documented at `createExactObject` and the lowering site.
-- D9 (shared with Phase 4, recorded there) governs every intersection of
   two distinct structural types.
-- `FlowTypeStructuralInfo::getHashValue` mixes in tuple elements and object
   field names/types: Flow's own `hash()` uses only the tuple length or the
   field count. Equality stays Flow's co-inductive `equals`, which also
   distinguishes variance; that is finer than IR identity and harmless (a
   non-recursive pair still interns to one id; a recursive pair gets two
   ids, which is sound under D4).
-- `ObjectStackPromotion` treats an allocation and every
   `UnionNarrowTrustedInst` reachable from it through chains of narrows as
   one object (`collectObjectAliases`). This restores scalar replacement
   of non-escaping typed object/tuple literals and of class instances,
   which had regressed in Phase 3 when class allocations were narrowed.
- **Fixes found on the way**:
-- `fillClassInstance`/`fillFunction` appended inputs that may point into
   their own side array (`std::vector::insert` self-range, undefined
   behavior that libstdc++ 11 happens to tolerate); they now copy first,
   as `fillTuple`/`fillExactObject` do.
-- The totality test found that `intersectTy` of a union and one of its
   own subsets returned the whole union (D9 kept the unrelated arms).
   `intersectTy` now returns `b` when `isSubsetOf(b, a)`; D9's text says
   a subset operand is returned exactly.
- **Tests**: `TypeContextTest` construction/interning, reserve/fill
  self-reference, fill-from-own-fields, printing (incl. recursive) and
  algebra tests for both kinds; `TypeAlgebraTotalityTest` (8 laws over all
  1764 ordered pairs of a 42-type sample (37 at B6, extended in the review),
  including a reserved-then-filled
  self-referential entry of each structural kind); lit
  `IRGen/flow/tuple-object-type.js`,
  `IRGen/flow/exact-object-variance-indexer.js` (pins D7 and D8),
  `shermes/tuple-object-type-exec.js`,
  `Optimizer/flow/tuple-object-literal-type.js`,
  `Optimizer/flow/object-stack-promotion-narrow.js`. Baselines regenerated
  in B4/B5 (`object` → `tuple(...)`/`exactObject(...)`, allocation narrows)
  and in the stack-promotion commit (three class tests scalar-replaced
  again).
- **Green bar (end of Phase 5, combined A4/B7 run)**: full build of every
  target except `mermes` (see Issues); `HermesIRTests` 121,
  `HermesVMRuntimeTests` 405, the other unittest binaries all pass; full
  lit 4537 expected passes, 6 expected failures, 345 unsupported,
  0 unexpected; `update-lit` changes nothing.
- **Deviations from the plan**: B5 shipped only allocation typing; its
  failing-test-first `StoreFrameInst` test was already covered by B4's
  `tuple-object-type-exec.js` (typed static fields, captured typed locals).
  B5's re-run of A3's exact-`Object` search was done in B7: the only
  exact-`Object` kind switch outside `TypeContext` is
  `nameOfFunctionCheckingForType` in `lib/BCGen/SH/SH.cpp`, which already
  lists all five refined kinds. B6 sampled more types than the plan listed
  (empty/uninit, the number family, reserved recursive entries, `AnyType`).
- **Issues**: the program wrapper's `exports` argument `{}` is now typed
  `exactObject()`, so it is behind a narrow in every typed IRGen dump. A
  rest binding whose Flow type equals a recursive alias is narrowed twice
  (allocation, then to the alias's reserved id; sound, redundant; fixed in
  the publication review, 2026-11-10).
  `mermes` (`tools/facebook/mermes`) does not link: it lacks
  `hermesHBCBackend` (unresolved `LowerArgumentsArray`,
  `ConsecutiveStringStorage`); its CMake file and those sources are not
  touched by this work.

### Phase 4: Array + Function (+ ClassConstructor), wired live
See [phase4-plan.md](phase4-plan.md) (tasks 1–6) and
[remaining-plan.md](remaining-plan.md) Part A, which superseded its tasks
7–10: their `isLooping`-based routing cannot see cycles through `Array<T>`,
a nominal class specialization.
- **What was done**:
-- `TypeContext`: interned `createArray` (key: element id) and
   `createFunction` (key: the full shallow signature; params in a new
   `paramArrays_`), `reserve*`/`fill*`, accessors. The existing id-aware
   algebra already handled both kinds (tests only).
-- Flow: the dead `flow::ArrayType` was deleted; builtin methods are keyed
   by a new `BuiltinReceiverKind`. `ClassType` records the generic class it
   was specialized from (`genericClassDecl_`), `isArrayClassType` is derived
   from it, and the wrapper-keyed array side table was dropped.
-- Lowering: `Array<T>` specializations lower to `array(T)`
   (`lowerArrayClassType`); `TypedFunction` and the `ClassConstructor` of a
   class with an explicit constructor signature to `function(...)`
   (`lowerTypedFunction`; other class constructors stay `object`).
   `Function::functionType_` carries a typed function's IR type, and
   `CreateFunctionInst` and TypeInference take the closure type from it, so
   closures type-check into function-typed slots without narrows.
-- A1, recursive types: lazy reservation. `beginStructuralLowering` marks a
   Flow type in progress (`loweringInProgress_`); reaching it again while
   its children are lowered reserves an IR entry; when the children are
   done the entry is filled, or, if nothing was reserved, the interning
   factory is used. Results go to `loweredStructuralTypes_`. Both maps are
   keyed by `FlowTypeStructuralInfo` (Flow's co-inductive `TypeInfo::equals`
   plus a hash that mixes in the children), because the identity check
   showed that a Flow type that is a different object equal to a recursive
   alias otherwise gets an unrelated id and fails `StoreFrameInst`
   verification. Equal mutually recursive aliases share one entry.
-- A2, printing: `printType`/`printLeaf` print union payloads by their
   arms (`array(string|number)`, not `array(union)`) and carry a stack of
   the arrays and functions being printed (Phase 5 adds tuples and exact
   objects); reaching one again prints `^N`, N counting enclosing
   structural types from the innermost (1). `type F =
   (x: number) => F` prints `function(^1)`; `type Json = number | string |
   Json[]` prints `string|number|array(string|number|^1)`. A bare
   `array(...)` was rejected as ambiguous (`M = M[][]` vs `N[]` with
   `N = N[]`). Functions still print only their return type.
-- A3, consumer audit: the SH `_typeCastHelper` emitted a negative check
   (throw if any rejected arm matches) even when a rejected and an accepted
   arm can both be objects, so `-O` threw on every valid object (a real
   miscompile, reproduced from typed JS); it now checks the accepted arms in
   that case. Typed array literals and `$SHBuiltin.objectKeys/Values/
   Entries` are trusted-narrowed to `array(T)` at allocation. The narrow of
   the `Array<T>` prototype back to `object` was removed (nothing constrains
   that operand). The Auditor counts every subset of `object` as object.
   `function-type-flow.js` gained an IR check of the pass-through.
-- A3b (added during execution): D9, then `narrowToFlowType` emits a
   trusted narrow for structurally compatible refined types instead of
   passing the value through unchanged, which fixes storing a
   contravariantly compatible function into a typed static field (found by
   A1).
- **Decisions**:
-- D9 (approved 2026-11-07): `intersectTy(a, b)` of two overlapping leaves
   with no subtype relation (two distinct structural types, or a class and
   a structural type) returns `a`, approximating the meet from above;
   NoType approximated it from below and miscompiled (a valid function
   failed a `-O` cast). The result is a subset of `a` only, so callers pass
   the type they keep first; union operands keep `a`'s arm first.
-- Reserved entries: kind and id are valid immediately, the payload only
   after fill; the algebra reads only kind and id of the structural refined
   kinds, which is what makes passing unfilled structural entries to
   `unionTy` safe. A class is reserved with its superclass
   (`reserveClassInstance(superClass)`), the one class payload the algebra
   reads (`isSubclass`); its name and fields are valid only after fill.
-- The `InstSimplify` `===`/`!==` fold uses `areDisjoint` instead of an
   empty intersection, so two distinct function types (which can alias at
   runtime) are not proved unequal (D2).
- **Rebase-era changes** (made while rebasing onto master on 2026-11-06,
  inside the existing diffs): D112825921 adds `BuiltinReceiverKind::Number`,
  which master's number-receiver builtins needed (the diff originally had
  only `Array` and `String`); D112825909 removes `registerArrayClassType`
  together with the side table it fed; the function-lowering diff (now in
  D112825918) added a `narrowToFlowType` pass-through for structurally
  compatible refined types (replaced in A3b by a trusted narrow).
- **Tests**: `TypeContextTest` Array/Function construction, algebra and
  printing, `ReservedUnfilledEntriesInAlgebra`, `RecursiveTypesAreObjects`,
  `RecursiveTypePrint`, `IntersectOverlappingUnrelatedReturnsFirst`; lit
  `IRGen/flow/function-type.js`, `IRGen/flow/recursive-type.js`,
  `shermes/recursive-type-exec.js`, `shermes/checked-cast-refined.js`,
  `Optimizer/flow/array-literal-type.js`,
  `shermes/intersect-unrelated-function-types.js`,
  `shermes/function-type-static-field.js` and its nullable variant.
  Baselines regenerated for `object` → `array(...)`/`function(...)`,
  union-payload printing, and allocation narrows replacing casts.
- **Green bar**: see Phase 5 (the A4 and B7 regression runs were combined).
- **Issues**: at `-O`, after a class instance is scalar-replaced, the load
  of its prototype from the frame stays behind unused. Home objects
  (`?P.prototype`, `?Array.prototype`) print as `classInstance()` with an
  empty name (`-dump-ir` and `-dump-lir`). Unproven doubt: whether
  co-inductive dedup can give different IR ids depending on which of two
  equal Flow types is lowered first.

### Phase 3: ClassInstance, wired live
See [phase3-5-design.md](phase3-5-design.md) (decisions D1–D6) and
[phase3-plan.md](phase3-plan.md).
- **What was done**: Flow `class` types now lower to a live, nominal
  `ClassInstance` IR type instead of collapsing to `Object`. `TypeContext`
  gained `createClassInstance`/`reserveClassInstance`/`fillClassInstance`, a
  `fieldArrays_` side array, and class accessors; `ExactObjectField` now holds a
  `Type`. Subtyping/disjoint/intersect/union became id-aware (so nominal
  classes that share the `ClassInstance` kind are distinguished) via
  `isLeafSubtypeById`/`areLeafTypesDisjointById`/`isSubclass`. `flowTypeToIRType`
  lowers `ClassType` with an identity-cached reserve→fill (handles self- and
  mutually-recursive classes); `emitTypedClassAllocation` trusted-narrows its
  allocation to the class instance type so the home object, static object, and
  instances are fully typed.
- **Decisions**:
-- Shallow subtyping (D4): classes nominal via the superclass chain; Array/Tuple
   invariant; Function/ExactObject by id. `areDisjoint` stays conservative for
   structural refined kinds (D5). Factored so deep structural subtyping can be
   added later without touching call sites.
-- ClassInstance stores its fields (D6); entries are non-interned (one per
   declaration).
-- Soundness invariant (D2): codegen/verifier treat every refined object kind
   exactly like `Object` (sound since `refined <: object`); precise runtime
   class checks are deferred to a later phase. The consumer audit relaxed
   exact-`isObjectType()` checks to `isSubsetOf(t, Object)` in the IRVerifier,
   SH backend (type-check emission + PrStore fast path), InstSimplify, and
   IREval, weakened the `LoadParentNoTrapsInst` ctor assert (no `TypeContext`
   there; the verifier does the real check), and mapped refined kinds to
   `_sh_ljs_is_object` in SH type-check codegen. These relaxations are
   behavior-preserving for pre-existing non-refined types.
- **Issues**: P3-S5 and P3-S6 are coupled — the live lowering can't leave a
  green tree without the consumer audit — so they landed in one commit.
- **Tests**: `TypeContextTest` unit tests for construction, nominal
  subtyping/algebra, recursive (self-referential) classes, and printing; a new
  `test/IRGen/flow/class-instance-type.js` lit test; ~26 `%FileCheckOrRegen`
  baselines regenerated (object → `classInstance(...)`). Full suite green:
  HermesIRTests (92), HermesVMRuntimeTests (350), full lit (4286, 0 unexpected
  failures).
- **Notes for next phase**: Phase 4 (Array + Function, + ClassConstructor →
  Function) adds the structural interner (non-looping dedup + the looping
  reserve/fill batch path). Candidate cleanups surfaced here: let
  `AllocFastArrayInst` (and similar) accept refined object kinds directly to
  drop the trusted-narrow-back-to-object workaround.

### P2-S1: Foundation — accessors and conventions
- **Files**: modified `include/hermes/IR/IRBuilder.h`, `lib/IRGen/ESTreeIRGen.h`, `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`, `unittests/IR/TypeContextTest.cpp`.
- **What was done**: Added `IRBuilder::getTypeContext()` and `ESTreeIRGen::getTypeContext()` accessors. Renamed `TypeContext::format` → `TypeContext::print` (matches `Type::print`). Added missing operations on TypeContext to reach API parity with TLS-using `Type` methods: `canBeAny`, `canBeType`, `isProperSubsetOf`, `isKnownPrimitiveType`, plus the `arms()` range adapter (returns `ArmRange` over a Type — yields the type itself for non-unions, each arm for unions, nothing for NoType). 4 new unit tests (42 total `TypeContextTest` cases). Phase 2 row added to status table for tracking.
- **Decisions**:
  - `ArmRange` and its iterator store the raw `uint32_t typeId_` instead of `Type` because `Type` is forward-declared in `TypeContext.h` (definition is in `IR.h`, which includes `TypeContext.h`). Methods that touch `Type` (`arms()`, `iterator::operator*`, `ArmRange::end`) are out-of-line in `TypeContext.cpp` where `Type` is complete. Same pattern as the existing `getUnionArms`.
  - The new helpers (`canBeAny`, `canBeType`, etc.) are also out-of-line for the same incomplete-type reason. Once Phase 2 is complete and Type loses its TLS-using methods, the layering can be revisited but it's not necessary.
  - `print` chosen over `format` for naming consistency with `Type::print` and `llvh::raw_ostream` conventions; `format` would have implied "build a string".
  - No callers migrated yet — the TLS-using methods on `Type` remain intact so the build stays green throughout P2-S2…P2-S8.

### P2-S8.5: Migrate IRPrinter (Dumper) off TLS Type printing
- **Files**: modified `include/hermes/Utils/Dumper.h`, `lib/Utils/Dumper.cpp`.
- **What was done**: Added a `Module *currentModule_` member to `IRPrinter`, set in `visitFunction` from `F.getParent()`. Migrated the three sites that previously relied on `Type::print(OS)`'s TLS lookup to format pretty type names: `printTypeLabel` (value types), `printFunctionHeader` (function return types), and the `LiteralIRType` operand branch in `printValueLabel` (the `type(...)` syntax in IR dumps). Each new path uses `currentModule_->getTypeContext().print(os_, t)`; falls back to the old `os_ << t` path when `currentModule_` is null.
- **Decisions**:
  - This is a missed migration step — should have happened during P2-S2…P2-S8 alongside the rest of the subsystem migrations. The greps used to find TLS callers searched for `Type::xxx` and `t.canBeX()` patterns and missed the implicit `os << type` pattern through `operator<<(raw_ostream &, Type)`. Adding it as P2-S8.5 keeps the cleanup commit (P2-S9) a pure dead-code deletion.
  - No behavior change: while TLS is still installed (which it is up through this commit), both the new `currentModule_` path and the old `operator<<` path produce the same pretty-printed names.

### P2-S9: Remove TLS infrastructure
- **Files**: modified `include/hermes/IR/IR.h`, `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`, `unittests/IR/TypeContextTest.cpp`. Deleted `TypeContextRAII typeContextGuard(...)` lines from 4 production files (`lib/BCGen/HBC/HBC.cpp`, `lib/BCGen/HBC/BCProviderFromSrc.cpp`, `lib/CompilerDriver/CompilerDriver.cpp`, `tools/shermes/shermes.cpp`) and 17 unittests/API files via sed.
- **What was deleted**:
  - `TypeContextRAII` class entirely.
  - `TypeContext::current_`, `current()`, `hasCurrent()`.
  - All TLS-using `Type::xxx` delegation methods: `unionTy`, `intersectTy`, `subtractTy`, `getFirstTypeKind`, `countTypes`, `isPrimitive`, `canBePrimitive`, `isNonPtr`, `isSubsetOf`, `canBeString/BigInt/Symbol/Number/Object/Boolean/Empty/Uninit/Undefined/Null/Any`. Convenience inline methods on Type that built on these (`isKnownPrimitiveType`, `canBeType`, `isProperSubsetOf`) also removed.
  - `Type::iterator` class entirely.
  - Two TLS-specific tests in `TypeContextTest.cpp` (`CurrentWithGuard`, `NestedGuards`); the `PrintWithoutGuardFallback` test renamed to `PrintFallback` and rewritten to assert the unconditional placeholder format.
- **What was kept on Type**:
  - All `is*Type()` (id-equality), `operator==/!=`, `hash`, `Profile`, all `createXxx()` constexpr factories.
  - `Type::print(OS)` retained as a minimal placeholder (`type#<id>`) for diagnostic streams that have no `TypeContext` available.
- **Decisions**:
  - The `operator<<(raw_ostream &, Type)` overload (in `IR.cpp`) was kept and now always prints the `type#<id>` placeholder. Pretty-printed type names require an explicit `TypeContext::print(os, t)` call (or for IR dumps, the `IRPrinter::currentModule_` plumbing added in P2-S8.5).
  - `TypeInference`'s `LLVM_DEBUG(dbgs() << inst->getType())` calls were left as-is. They now print `type#<id>` instead of pretty names in `--debug-only=typeinference` output, which is acceptable for debug-only diagnostics.

### P2-S8: Migrate test fixtures (and a stray IREval helper)
- **Files**: modified `unittests/IR/BuilderTest.cpp`, `lib/IR/IREval.cpp`.
- **What was done**: Migrated the only two TLS-using `Type` calls left in test code: `X->getType().isPrimitive()` in `BuilderTest::TestValueTypes` (now `M.getTypeContext().isPrimitive(...)`) and the body of `BuilderTest::Types` (now uses a local `TypeContext tc;` and calls `tc.unionTy(...)` / `tc.isPrimitive(...)` directly, dropping the now-unnecessary RAII guard for that test). All other test-fixture call sites already used the new explicit-context API; they only retain `TypeContextRAII` install lines, which P2-S9 deletes.
- **Bonus**: `lib/IR/IREval.cpp::disjointComparisonTypes` was a free helper missed by earlier passes (it is in `lib/IR`, not `lib/Optimizer` or `lib/BCGen`). Threaded `TypeContext &tc` as the first parameter; the single caller in `evalBinaryOperator` passes `builder.getTypeContext()`.
- **Decisions**:
  - Left the per-fixture `TypeContextRAII typeContextGuard(M.getTypeContext())` lines untouched. They still keep TLS valid for the unmigrated `Type` delegation methods that remain in P2-S8 state. P2-S9 deletes both the RAII class and these install sites in one motion.
  - In `BuilderTest::Types` the test never wraps a Module — it constructed a standalone `TypeContext` and installed the RAII guard purely to support `Type::unionTy`. With the explicit API the RAII guard is redundant; it is removed in this step (the only test fixture in P2-S8 that drops a guard early).

### P2-S7: Migrate remaining optimizer + BCGen
- **Files**: modified `lib/Optimizer/Scalar/{ObjectMergeNewStores,ObjectStackPromotion,TDZDedup,Inlining,FunctionAnalysis,Auditor}.cpp`, `lib/BCGen/{Lowering,RegAlloc}.cpp`, `lib/BCGen/HBC/{ISel,Passes}.cpp`, `lib/BCGen/HBC/Passes/PeepholeLowering.cpp`, `lib/BCGen/SH/{LowerNanBoxedUnionNarrowT,SHRegAlloc,RecreateCheapValues,SH}.cpp`, `lib/BCGen/facebook/Mins/{LowerNanBoxedUnionNarrowT,RecreateCheapValues,Mins}.cpp`.
- **What was done**: Migrated all remaining TLS-using `Type` calls in the optimizer and BCGen subsystems. Three patterns:
  1. Per-call lookup via an existing carrier: `builder.getTypeContext().foo(t)` (Lowering, ObjectMergeNewStores, RecreateCheapValues, LowerNanBoxedUnionNarrowT, ObjectStackPromotion, TDZDedup, Inlining), or `inst->getModule()->getTypeContext().foo(t)` (RegAlloc, ISel, SHRegAlloc, Auditor).
  2. Stored member: SH.cpp's `InstrGen` got a `TypeContext &typeCtx_` initialized inline from `F_.getParent()->getTypeContext()`; PeepholeLowering threaded `tc` through `irTypeToTypeOfIsTypes` and used `builder_.getTypeContext()` in `lowerCheckedTypeCast`.
  3. Threaded parameter: free helpers (`isAnalyzableVariable` in FunctionAnalysis, `canCompareStrictEqualityRaw` in SH.cpp and Mins.cpp) gained a leading `TypeContext &tc` parameter.
  Replaced two more inline `Type::unionTy(Object, Undefined)` and `Type::unionTy(Empty, Uninit)` sites with the well-known constexpr factories from P2-S3.
- **Decisions**:
  - SH.cpp's `_typeCastHelper` now iterates `typeCtx_.arms(checkTypes)` instead of relying on `Type::iterator`'s TLS-using dereference. This was the single user of that iterator outside of the Type class itself.
  - Marked the unused `canCompareStrictEqualityRaw` in `lib/BCGen/facebook/Mins/Mins.cpp` as `[[maybe_unused]]`. It was already dead but still compiled (no static-unused warning was firing); migrating preserves it for whenever the Mins backend wires it back up.
  - Dropped the redundant `assert` in `setInvalidTypes` in P2-S3 already; the corresponding asserts in `_typeCastHelper`/`generateThrowIfInst` (which still exist) now use the explicit context.

### P2-S6: Migrate TypeInference pass
- **Files**: modified `lib/Optimizer/Scalar/TypeInference.cpp`.
- **What was done**: Added `TypeContext &typeCtx_` member to `TypeInferenceImpl` (initialized via `M->getTypeContext()` in a new explicit constructor) and updated `TypeInference::runOnModule` to pass the Module. Threaded `TypeContext &tc` as the first parameter through eight file-scope helpers: `isBigIntOrObject`, `inferUnaryArith`, `inferUnaryArithDefault`, `inferTilde`, `inferMemoryLocationType`, `inferBinaryArith`, `inferBinaryBitwise`, `inferBinaryInst`. Migrated the remaining inline call sites in member methods (`inferPhi` union, `inferToPropertyKeyInst`, `inferThrowIfInst`, `inferUnionNarrowTrustedInst`, `inferCheckedTypeCastInst`, parameter type union, return type union, and the post-pass intersection in the prePassTypes narrowing).
- **Decisions**:
  - Threaded `TypeContext &tc` as a parameter to the file-scope helpers rather than moving them into the impl class. The helpers are already self-contained and just gained one extra leading parameter; relocating would have meant a larger restructure for marginal benefit.
  - Replaced one inline `Type::unionTy(Type::createObject(), Type::createUndefined())` with the well-known `Type::createObjectOrUndef()` factory added in P2-S3, and one `Type::unionTy(String, Symbol)` with `Type::createStringOrSymbol()` — both eliminate context lookups for fixed types.

### P2-S5: Migrate InstSimplify pass
- **Files**: modified `lib/Optimizer/Scalar/InstSimplify.cpp`.
- **What was done**: Added `TypeContext &typeCtx_` member to `InstSimplifyImpl`, initialized from `builder_.getTypeContext()` in the constructor. Migrated the 12 TLS-using call sites: `canBeNumber`, `isPrimitive` (×2), `canBeUndefined`, `canBeObject`, `isSubsetOf` (×6 — equality/inequality fast paths, CoerceThisNS short-circuit, UnionNarrowTrusted no-op, CheckedTypeCast widening), `intersectTy` (×6 — strict equality, ThrowIf invalidation, CheckedTypeCast result computation), `isKnownPrimitiveType` (×2 — equality promotion). The free `notNaN` helper grew a `TypeContext &` parameter (only one caller, threaded `typeCtx_`).
- **Decisions**:
  - Stored `TypeContext &` directly even though `builder_.getTypeContext()` is one indirection: 12 use sites and the InstSimplifyImpl is constructed once per function, so caching reduces repetition without measurable cost.
  - The `kNullOrUndef` constexpr at file scope is unchanged — it is just a cheap constant Type tag and works fine as a constexpr.

### P2-S4: Migrate IRGen (ESTreeIRGen-*.cpp)
- **Files**: modified `lib/IRGen/ESTreeIRGen.h`, `lib/IRGen/ESTreeIRGen.cpp`, `lib/IRGen/ESTreeIRGen-func.cpp`, `lib/IRGen/ESTreeIRGen-expr.cpp`, `lib/IRGen/ESTreeIRGen-typed-class.cpp`.
- **What was done**: Migrated every TLS-using `Type` call in IRGen to the explicit `getTypeContext().foo(t, ...)` form (or to a local `TypeContext &tc` reference where the same context is used multiple times in a small scope). Affected operations: `unionTy` (×7 sites in func and expr), `subtractTy` (×2 in ESTreeIRGen.cpp), `isSubsetOf` (×3 in expr), `canBePrimitive` (×2), `isNonPtr` (×4), and the assertion-flow comparison in `enforceExprType`.
- **Decisions**:
  - Made `ESTreeIRGen::flowTypeToIRType` non-static (was `static`). It needs `getTypeContext()` for the `Union` arm, and `static` would have meant either threading TypeContext through every caller or special-casing the union case. All callers were already on instances — no caller change required.
  - Did not add new well-known unions for the TDZ shapes (`AnyType | Empty`, `irType | Undefined`, etc.) even though some appear three times. The dynamic forms (`irType | Undefined`) need real interning anyway, and the constant ones (`AnyType | Empty`) are infrequent enough that one extra well-known per shape is not worth the bloat.
  - For methods that use the context just once, used `getTypeContext().foo(...)` inline. For the few sites that use it multiple times in short scope (`enforceExprType`, the union loop in `flowTypeToIRType`, the `Optional` field flow in expr.cpp), introduced a local `TypeContext &tc` reference.

### P2-S3: Migrate Instrs.{h,cpp} and IR.cpp
- **Files**: modified `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`, `include/hermes/IR/IR.h`, `include/hermes/IR/Instrs.h`, `lib/IR/IR.cpp`, `lib/IR/Instrs.cpp`, `lib/IR/IRBuilder.cpp`, `unittests/IR/TypeContextTest.cpp`.
- **What was done**:
  - Added 4 new well-known union IDs to `TypeContext`: `kStringOrSymbolId`, `kEmptyOrUninitId`, `kObjectOrNullId`, `kObjectOrUndefId`. Each is pre-allocated in the constructor and added to the intern table. Added matching `Type::createXxx()` constexpr factories.
  - Replaced inline header `Type::unionTy(constant, constant)` calls with the new constexpr factories: `ToPropertyKeyInst`, `LoadParentNoTrapsInst::getInherentTypeImpl`, `functionNewTargetType` (in IR.cpp).
  - `ThrowIfInst`: removed the constructor-side `Type::subtractTy` call and the assertion that uses TLS-based `.isSubsetOf`. Added a `resultType` parameter to the constructor; `IRBuilder::createThrowIfInst` now computes the result type via `tc.subtractTy` and asserts the precondition. Dropped the now-redundant assert in `setInvalidTypes` (only one external caller, InstSimplify, and it derives `invalidSubset` via `intersectTy` so the invariant is preserved).
  - `BinaryOperatorInst::getBinarySideEffect`: added `TypeContext &tc` as the first parameter; updated the two callers (in `Instrs.h`) to pass `getModule()->getTypeContext()`.
  - `UnaryOperatorInst::getSideEffectImpl`, `PhiInst::addEntry`, `PhiInst::recalculateResultType`: switched from TLS-using calls to `getModule()->getTypeContext()`.
  - `PhiInst::PhiInst` constructor: removed the implicit `recalculateResultType()` call (it would crash because the inst has no parent yet). The builder now calls `recalculateResultType()` after `insert()`. Made the method public for that.
  - Updated `ReservedSlots` test to scan from `kObjectOrUndefId + 1`.
- **Decisions**:
  - Added well-known IDs for fixed-shape unions used in inline headers. Avoids both threading TypeContext through dozens of constructors and creating fresh union entries at runtime; result is constexpr.
  - Moved the type computation for `ThrowIfInst` into the builder rather than threading TypeContext through its constructor. The constructor's only computed type was `subtractTy(operand, invalidTypes)` — easier to do once at construction time in the builder than to thread context everywhere.
  - Made `PhiInst::recalculateResultType` public rather than friending IRBuilder. It is logically a public operation (already exposed via `addEntry`/`removeEntry` side effects).
  - For static helpers like `getBinarySideEffect`, threading `TypeContext &` as the first parameter is the natural fit — alternative (lookup via `getModule()` on a passed-in inst) would require passing the inst too.

### P2-S2: Migrate IRVerifier to explicit context
- **Files**: modified `include/hermes/IR/IR.h`, `lib/IR/IRVerifier.cpp`.
- **What was done**: Added `TypeContext &ctx_` member to the `Verifier` class, initialized from `M.getTypeContext()` in the constructor. Migrated 9 TLS-using call sites to the explicit `ctx_.foo(t, ...)` form (`canBeType`, `canBeEmpty` ×2, `canBeUninit`, `isSubsetOf` ×4, `unionTy`, `intersectTy`). Static factory calls (`Type::createNumber()` etc.) and `operator==` checks remain unchanged because they don't need a context.
- **Decisions**:
  - Made `Module::typeContext_` `mutable` and `Module::getTypeContext()` `const`. The Verifier holds `const Module &` (it doesn't mutate IR), but TypeContext is a side-cache whose intern table grows with each `unionTy`/`intersectTy` call — the standard "logical const" pattern. This unblocks any future const-Module consumer that needs type operations.
  - Stored `TypeContext &` directly rather than calling `M.getTypeContext()` at each use site: it's used in many methods, the reference is cheap to store, and it documents the dependency.

### P1-S2: Type queries on TypeContext
- **Files**: modified `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`, `unittests/IR/TypeContextTest.cpp`.
- **What was done**: Added `canBeNumber`, `canBeString`, `canBeObject`, `canBeNull`, `canBeUndefined`, `canBeEmpty`, `canBeUninit`, `canBeBigInt`, `canBeBoolean`, `canBeSymbol`, `isNoType`, `isPrimitive`, `canBePrimitive`, `isNonPtr`. Private helpers `containsMatchingKind` and `allMatchKind` take predicate functions for leaf/union dispatch. File-scoped kind helpers (`isNumberKind`, `isObjectKind`, `isPrimitiveKind`, `isNonPtrKind`) encode subtype relationships. 7 new unit tests (13 total).
- **Decisions**:
  - Well-known ID fast paths in each `canBeX` method, per the plan spec. Avoids table lookups for common types.
  - Kind helpers are subtype-aware: `isNumberKind` includes Int32/Uint32, `isObjectKind` includes ClassInstance/Array/Tuple/Function/ExactObject, `isPrimitiveKind` and `isNonPtrKind` include number subtypes. Refined kinds are dead code in Phase 1 but the predicates are correct when they activate.
  - `isPrimitive`/`isNonPtr` return false for NoType, matching the old bitmask semantics (`bitmask_ && !(bitmask_ & ~BITS)`).

### P1-S3: Type operations with interning
- **Files**: modified `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`, `unittests/IR/TypeContextTest.cpp`.
- **What was done**: Added `isSubsetOf`, `areDisjoint`, `unionTy`, `intersectTy`, `subtractTy` public methods. Added `UnionInternKey`/`UnionInternKeyInfo` structs for DenseMap-based union interning. Private `createUnionImpl` handles full canonicalization (flatten, sort, dedup, subsume, intern). Static helpers `isLeafSubtype` and `areLeafKindsDisjoint` handle leaf-level type relationships including future-proof Number/Object family rules. Intern table pre-populated with well-known unions in constructor. 10 new unit tests (23 total TypeContext tests).
- **Decisions**:
  - Used `DenseMap<UnionInternKey, uint32_t, UnionInternKeyInfo>` with SmallVector<uint32_t, 8> key and custom DenseMapInfo (sentinels use size-1 vectors with UINT32_MAX/UINT32_MAX-1, real keys always size >= 2).
  - `isLeafSubtype` handles Int32/Uint32 <: Number and object refinements <: Object even though unused in Phase 1 — keeps algorithms correct when refined types arrive.
  - `areLeafKindsDisjoint` uses the subtype check plus Number-family overlap rule; all other distinct kinds are disjoint.
  - `subtractTy` returns conservative approximation (returns `a` unchanged) when a is a leaf that's not subset of b and not disjoint from b.
  - `intersectTy` distributes over unions recursively; base case for two non-subset number-family leafs returns `kUInt31Id` (the only such overlap); other non-subset leafs return NoType.
  - Added `TypeKind::UInt31` (Int32 ∩ Uint32, integers in [0, 2^31-1]) to close the lattice gap. UInt31 <: Int32, UInt31 <: Uint32, UInt31 <: Number. Pre-allocated as well-known IDs: kInt32Id=15, kUint32Id=16, kUInt31Id=17 (shifted union IDs up by 3). 5 new unit tests (28 total).

### P1-S4: Utility methods
- **Files**: modified `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`, `unittests/IR/TypeContextTest.cpp`.
- **What was done**: Added `countKinds`, `getFirstKind`, `format` methods. Added `kindName` file-scoped helper for TypeKind→string mapping. Forward-declared `llvh::raw_ostream` in header. 5 new unit tests (28 total).
- **Decisions**:
  - `format` uses "any" shorthand when the union is a superset of AnyType (matching old `Type::print()`). Extra arms beyond AnyType (e.g. empty, uninit) are appended with `|`.
  - `kindName` covers all TypeKind values including deferred refined types and Bits32, for completeness.
  - `getFirstKind` returns `TypeKind::NoType` for NoType (matching old behavior where `getFirstTypeKind` returns `LAST_TYPE` for empty bitmask — callers check `isNoType()` first).

### P1-S5: Thread-local context and RAII guard
- **Files**: modified `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`, `unittests/IR/TypeContextTest.cpp`.
- **What was done**: Added `static thread_local TypeContext *current_` and `static TypeContext &current()` to `TypeContext`. Added `TypeContextRAII` class with save/restore semantics. 2 new unit tests (35 total).
- **Decisions**:
  - `TypeContextRAII` is a friend of `TypeContext` to access `current_` directly, keeping the static member private.
  - `current()` returns a reference (not pointer) and asserts non-null — callers never need to check.

### P1-S1: TypeContext skeleton with well-known types
- **Files**: created `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`, `unittests/IR/TypeContextTest.cpp`; modified `lib/CMakeLists.txt`, `unittests/IR/CMakeLists.txt`.
- **Decisions**:
  - TypeEntry uses raw `uint32_t` for all type references (not `Type`) since `Type` is still a bitmask. Changed to `Type` in P1-S8.
  - Added `kNullOrUndefId` (id=18) as a well-known union, needed for InstSimplify.cpp migration in P1-S8.
  - Reserved IDs 19-31 padded with NoType placeholders.
  - Union arm arrays stored in sorted order by ID for AnyType and AnyEmptyUninit.
- **What was done**: Created TypeContext with TypeKind enum (all kinds including deferred refined types), TypeEntry tagged-union struct, well-known ID constants, constructor that pre-allocates 15 leaf types and 4 well-known unions, getKind() and getUnionArms() accessors. Added to hermesFrontend build target. 6 unit tests.

### P1-S6: Wire TypeContext into Module
- **Files**: modified `include/hermes/IR/IR.h`.
- **What was done**: Added `#include "hermes/IR/TypeContext.h"` to IR.h. Added `TypeContext typeContext_` private member and `getTypeContext()` public accessor to `Module`. The context is default-constructed when `Module` is constructed. No semantic change — nothing uses `typeContext_` yet.
- **Decisions**:
  - Included `TypeContext.h` at the top with other hermes includes (alphabetically after `hermes/FrontEndDefs/Typeof.h`) rather than mid-file, since TypeContext.h has no dependency on `Type` or other IR.h definitions.

### P1-S7: Install RAII guards at compilation entry points
- **Files**: modified `lib/BCGen/HBC/BCProviderFromSrc.cpp`, `lib/CompilerDriver/CompilerDriver.cpp`, `tools/shermes/shermes.cpp`.
- **What was done**: Added `TypeContextRAII typeContextGuard(M->getTypeContext())` (or `M.getTypeContext()` for stack-allocated Module) immediately after Module creation in all three production compilation entry points. No new includes needed — all files already include `IR.h` which includes `TypeContext.h`.
- **Decisions**:
  - Test files that create `Module` (BasicBlockTest, LoopAnalysisTest, BCGen/TestHelpers, BCGen/HBC, VMRuntime/TestHelpers1, API/SegmentTestCompile) are deferred to P1-S7.5.
  - No additional production `Module` creation sites found beyond the three in the plan.

### P1-S7.5: Add RAII guards to unit tests
- **Files**: modified `unittests/IR/BuilderTest.cpp`, `unittests/IR/VariableTest.cpp`, `unittests/IR/IRUtilsTest.cpp`, `unittests/IR/IRVerifierTest.cpp`.
- **What was done**: Added `TypeContextRAII typeContextGuard(M.getTypeContext())` after Module creation in every test that uses `Type::` operations. For `BuilderTest::Types` (which uses `Type::unionTy` without a Module), added a standalone `TypeContext` + RAII guard.
- **Decisions**:
  - Only added guards to tests that directly use `Type::` operations (grep-verified). Test files that create Module but don't use `Type::` (BasicBlockTest, LoopAnalysisTest, etc.) were not modified — they don't need guards since P1-S8's static constructors (`createAnyType()`, etc.) remain constexpr and don't require the thread-local context. **(P1-S8 retroactively required guards in ALL Module-creating tests.)**
  - No new includes needed — all four files already include `IR.h` which includes `TypeContext.h`.

### P1-S8: Rewrite Type class
- **Files**: modified `include/hermes/IR/IR.h`, `include/hermes/IR/TypeContext.h`, `lib/IR/TypeContext.cpp`, `lib/IR/IR.cpp`, `lib/BCGen/SH/SH.cpp`, `lib/Optimizer/Scalar/InstSimplify.cpp`, `lib/BCGen/HBC/HBC.cpp`, and many test files (BasicBlockTest, LoopAnalysisTest, IRHashTest, BuilderTest, IRVerifierTest, IRUtilsTest, BCGen/TestHelpers, BCGen/HBC, VMRuntime/TestHelpers1, API/SegmentTestCompile, Optimizer/InstructionEscapeAnalysisTest, Optimizer/PassManagerTest).
- **What was done**: Replaced `Type`'s `uint16_t bitmask_` with `uint32_t id_` (index into TypeContext). Removed old nested `enum TypeKind` and bitmask infrastructure. All type operations (`unionTy`, `intersectTy`, `subtractTy`, predicates, `print`, `iterator`) delegate to `TypeContext::current()`. Well-known constructors and identity checks remain `constexpr`. Changed `TypeContext::typeArrays_` from `vector<uint32_t>` to `vector<Type>`. Moved `getUnionArms` out-of-line to resolve incomplete-type issue. Fixed SH.cpp to use `TypeKind::Empty` etc. instead of `Type::Empty`. Replaced `constexpr Type kNullOrUndef` in InstSimplify.cpp with `Type::createNullOrUndef()`. Added RAII guards at lazy compilation (`compileLazyFunctionWorker`) and eval compilation (`compileEvalWorker`) entry points in HBC.cpp. Added RAII guards to all remaining Module-creating test files.
- **Decisions**:
  - `Type` methods that need the context (queries, operations, `print`, `iterator`) are defined out-of-line in `TypeContext.cpp` to avoid include-order coupling. Only `constexpr` methods (identity checks, constructors, `operator==`) remain inline in `IR.h`.
  - `TypeContext::getUnionArms` moved out-of-line because `ArrayRef<Type>` pointer arithmetic requires `Type` to be complete, but `Type` is defined after `TypeContext.h` is included.
  - Fixed latent iterator-invalidation bug in `intersectTy`/`subtractTy`: arms are now copied to a `SmallVector` before calling `unionTy` which may reallocate `typeArrays_`.
  - Added `createNullOrUndef()` static constructor to Type (returns well-known `kNullOrUndefId`).
  - `sizeof(Type)` changed from 2 to 4 bytes. Updated `static_assert`.
  - `Type::iterator` reimplemented: for non-union types yields the type itself; for unions iterates arms via `getUnionArms`.
- **Issues**: P1-S7/P1-S7.5 missed two production compilation paths (`compileLazyFunctionWorker`, `compileEvalWorker` in HBC.cpp) and many test files that create Module but don't directly use `Type::`. These all needed RAII guards once Type methods became non-trivial.

