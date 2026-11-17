# IR Type System v2 — Phases 3-5 Design (Refined Types, Wired Live)

## References

- [ir-type-system-v2-design.md](ir-type-system-v2-design.md) — conceptual type
  hierarchy, type algebra, interning algorithm, the original 8-phase plan.
- [ir-type-v2-implementation.md](ir-type-v2-implementation.md) — concrete C++
  data structures (`TypeEntry`, `TypeContext`, side arrays, well-known IDs).
- [flow-type-system.md](flow-type-system.md) — the Flow type system that feeds
  IR types during lowering.
- [progress.md](progress.md) — step tracking (extended by this work).

Phases 1-2 (the bitmask→indexed `TypeContext` refactor and explicit-context
migration) are complete. The refined `TypeKind`s (`ClassInstance`, `Array`,
`Tuple`, `Function`, `ExactObject`) and their `TypeEntry` payloads already
exist in `include/hermes/IR/TypeContext.h`, but there are no factories,
accessors, or type-algebra support for them, and `flowTypeToIRType`
(`lib/IRGen/ESTreeIRGen-typed-class.cpp`) collapses every refined Flow type to
`Object`/`Number`. This document covers Phases 3-5: making those refined types
real and flowing them live through the IR.

## Goal and Scope

Enrich the IR type system so Flow's nominal classes, arrays, function
signatures, tuples, and exact objects survive lowering and flow **live**
through the IR into codegen — instructions and values carry refined types, not
just `Object`.

**In scope (Phases 3-5):**
- `TypeContext` construction (factories), accessors, and the type algebra
  (`unionTy`/`intersectTy`/`subtractTy`/`isSubsetOf`/`areDisjoint`) for all
  five refined kinds.
- The Flow→IR interning lowerer, including accurate representation of recursive
  (looping) types.
- Switching `flowTypeToIRType` to emit refined types, and auditing all
  consumers (verifier, printer, TypeInference, InstSimplify, SH + HBC codegen)
  so they remain correct.

**Out of scope (later phases):**
- Runtime type descriptors / precise class-id runtime checks (Phase 7).
- Deep structural subtyping: function variance, exact-object width/depth
  subtyping (Phase 8). The design keeps the door open for these. D10 makes it
  a prerequisite of precise runtime checks for structural kinds, so this part
  of Phase 8 must come before the structural part of Phase 7.
- Optimizer *exploitation* of refined types beyond what falls out naturally.
- `bits32`, and real `Int32`/`Uint32` lowering (still aliased to `Number`).
- `UntypedFunction`, `NativeFunction`, `CPtr` lowering — intentionally left at
  `Object`/`Number`/`Number`.

## Key Design Decisions

These were settled during brainstorming and govern the whole effort.

### D1. Wire refined types live, end-to-end

`flowTypeToIRType` emits refined types; instructions/values carry them; codegen
must tolerate them. (As opposed to building the machinery but keeping lowering
collapsed.)

### D2. The "codegen treats refined as Object" soundness invariant

This is what makes wiring live safe. Today `flowTypeToIRType` already lowers
`Class → Object`, so a Flow cast like `x as Dog` already becomes
`CheckedTypeCast(Object)` and codegen already emits only an *object-ness*
check — class precision is **not** enforced at runtime today; it rests on
Flow's static guarantees. Therefore:

> **Codegen and lowering treat every refined kind exactly as they treat
> `Object`.** Carrying `ClassInstance(Dog)` instead of `Object` changes only
> what the *optimizer* can prove. The emitted code stays identical until
> Phase 7 adds runtime descriptors.

When this may be relaxed, and for which kinds, is governed by D10: precise
checks of structural kinds require deep structural subtyping first.

Concretely, the consumer audit for each phase is: **find every site that
switches on the exact `Object` identity (`isObjectType()`, `id == kObjectId`,
or a `TypeKind::Object` switch) in the verifier, codegen (SH + HBC), and
lowering, and make it accept refined kinds too.** Sites that use
`canBeObject()` already work (refined `<:` Object ⇒ true). Optimizer passes may
*exploit* the extra precision, but only conservatively (subtyping may answer
"no"; `areDisjoint` stays conservative), so they can never become unsound.

### D3. Recursive types are represented accurately

A recursive type is fully representable in the interned-ID scheme: a cycle is
just a cycle in the ID-reference graph (e.g. a `Union` entry whose arm is an
`ExactObject` entry whose field references the `Union` entry's id). Nothing is
lost. We do **not** collapse recursive types to `Object`.

The only consequence is that **lowering** (Flow→IR) needs cycle-breaking:
an entry is reserved, its id is used by the types built while its children
are lowered, and the entry is filled afterwards (see the Phase 3 and Phase 4
lowering sections). The type **operations** do not need cycle handling,
because their structural parts reduce to interned-ID comparison (see D4):
`isSubsetOf(Tree, Tree)` is `id == id` and terminates.

### D4. Shallow subtyping now; deep subtyping later

- **ClassInstance**: nominal. `ClassA <: ClassB` iff B is an ancestor of A via
  the `superClassTypeId` chain. Every refinement `<: Object`.
- **Array, Tuple**: invariant — `Array(S) <: Array(T)` iff `S == T` (id
  equality). (Accurate, not an approximation, because arrays/tuples are
  mutable.)
- **Function**: compared by whole interned signature id (no variance yet).
- **ExactObject**: compared by entry id (no width/depth yet).

This keeps the operations non-recursive and trivially cycle-safe — the cycle
handling stays confined to the interner. The subtype/disjoint hooks are
factored so deep structural subtyping (function variance, object width/depth)
can replace the "equal id only" branches later **without touching call sites**.

Rationale: refined-vs-refined subtyping is only exercised by (1) redundant
`CheckedTypeCast` elimination, (2) narrowing via `intersectTy`, (3)
`UnionNarrowTrusted` no-op detection, (4) soundness asserts. In all four,
nominal class subtyping carries essentially all the practical value; arrays
are invariant anyway; function/exact-object casts are rare and, when present,
were already proven sound by the Flow checker, so weaker IR subtyping costs at
most an extra (sound) cast — never miscompilation. This holds only while casts
to structural kinds check object-ness alone (D2); a precise runtime check on
the shallow relation would reject valid values, which is why D10 makes deep
subtyping a prerequisite for such checks.

### D5. `areDisjoint` stays conservative for structural kinds

Declaring two types disjoint is a positive claim that can drive unsound folding
(e.g. "these can never be equal"). Two unrelated *classes* are genuinely
disjoint (nominal — one class per value). But two distinct *structural* refined
types are **not** safely disjoint (e.g. one permissive function value can
satisfy two different signatures; two `Array<…>` SSA values can alias the same
runtime array). So:

```
areDisjoint(refined A, refined B):
  both ClassInstance        -> disjoint iff neither is a subclass of the other
  refined vs primitive      -> disjoint
  refined vs Object/refined-superset -> not disjoint
  otherwise (any two distinct structural refined types) -> NOT disjoint
```

Cross-category disjointness (e.g. `Array` vs `Function`) is a sound future
optimization but is deliberately left out now for simplicity and safety.

Because such pairs may overlap, the algebra cannot treat their intersection as
empty, but the shallow model cannot express it either; D9 defines what
`intersectTy` returns for them.

### D6. ClassInstance stores fields

The `ClassInstance` payload stores `superClassTypeId`, `className`, and the
field list (`name` and type per field) — even though IRGen still reads field
layout/slots from the Flow `ClassType` during class codegen. This keeps the IR
type self-contained and lets later optimizations reason about field types from
IR alone. ClassInstance entries are **not interned** — one entry per class
declaration (nominal identity).

### D7. Exact objects with an indexer lower to `Object`

Approved by the owner on 2026-11-07.

**Decision.** A Flow `ExactObjectType` that has an index signature
(`getIndexer()`, e.g. `{[k: string]: number}`) lowers to `Object`, not to an IR
`ExactObject`. Only indexer-free exact objects become `ExactObject`.

**Rationale.** An IR `ExactObject` is a closed, ordered list of named fields;
its intern key is exactly that list. An indexer describes an open key set,
which such a list cannot express:

- An object with an indexer has no named fields at all
  (`include/hermes/Sema/FlowContext.h`, `ExactObjectType::Indexer` and
  `indexer_`), so there is no field list to lower.
- Flow lets an object with named fields flow into a string-keyed indexer
  (`FlowChecker::canAFlowIntoB(ExactObjectType *, ExactObjectType *)` in
  `lib/Sema/FlowChecker.cpp`), so the values of an indexer type have
  arbitrary, unordered key sets.

Lowering to `Object` is sound and loses nothing that is used today: under D2
codegen treats every refined kind as `Object` anyway, and IRGen types an
indexer read from the Flow indexer's value type through a `CheckedTypeCast`,
not from the IR type of the object (`ESTreeIRGen::emitMemberLoad` in
`lib/IRGen/ESTreeIRGen-expr.cpp`). Because the indexer's key and value types
are never lowered as part of the object, a recursive type that cycles through
an indexer is cut at the indexer and needs no cycle handling.

**Revisit when** the optimizer would benefit from knowing the key or value type
of an indexed object (e.g. to drop the cast on an indexer read). That needs a
new refined kind (a dictionary of `key → value`, with its own subtyping and
disjointness rules), not an extension of `ExactObject`.

### D8. Field variance is not part of the IR type

Approved by the owner on 2026-11-07.

**Decision.** Flow's `ExactObjectType::Field` carries a `FieldVariance`
(`None`, `ReadOnly` for `+x`, `WriteOnly` for `-x`). The IR drops it:
`ExactObjectField` holds only a name and a type, and the intern key of an
`ExactObject` is the ordered list of `(name, type id)` pairs. `{+x: number}`,
`{-x: number}` and `{x: number}` therefore lower to the same IR type id.

**Rationale.** Variance is a static permission on how code may access a field,
which the Flow checker enforces at every type-checked member access
(`checkFieldVariance` in `lib/Sema/FlowChecker-expr.cpp`; optional member
access `o?.x` is not type-checked yet, only warned about as unimplemented) and
at every assignment between exact object types (`canAFlowIntoB`). The IR has
nothing that uses it:

- Under D4, `ExactObject` types are compared by id only; there is no width,
  depth or variance subtyping that variance could feed.
- The IR never checks writes against a field's type, and codegen (D2) checks
  only object-ness.
- IRGen types a field read from the Flow field type of the member expression
  (`ESTreeIRGen::emitMemberLoad`), not from the IR `ExactObject` payload.

Keeping variance out of the key keeps one IR id per field shape and keeps
`ExactObjectField` shared with `ClassInstance`, whose fields have no variance.

**Constraint this places on consumers.** For a `WriteOnly` field the Flow type
is a lower bound of what the field may hold, not an upper bound: Flow lets
`{x: number | string}` flow into `{-x: number}` (`canAFlowIntoB`, the
`WriteOnly` case checks the destination type flows into the source type). So
the field types stored in an IR `ExactObject` are not a sound upper bound on
the values held by an object of that type, and no consumer may type a loaded
value from them while variance is absent from the IR type.

**Revisit when** deep structural subtyping for exact objects (width, depth or
variance; Phase 8, deferred by D4) is implemented, or when any consumer starts
reading field types from an IR `ExactObject` payload. Either one needs
variance in the payload and in the intern key.

### D9. Intersecting overlapping, unrelated types returns the first operand

Approved by the owner on 2026-11-07.

**Decision.** When two leaf types overlap (D5 does not declare them disjoint)
but neither is a subtype of the other, `intersectTy(a, b)` returns `a`. Under
D4 and D5 this happens for exactly two kinds of pair: two distinct structural
refined types (two different function, array, tuple or exact-object types, in
any combination), and a class and a structural refined type. Every other case
is unchanged and exact: a subtype relation in either direction returns the
smaller type, the number family's one non-subset overlap returns
`Int32 ∩ Uint32 = UInt31`, and disjoint pairs return `NoType`. When one
operand is a subset of the other, the result is the smaller operand, unions
included. Otherwise unions intersect arm by arm (`TypeContext::intersectTy` in
`lib/IR/TypeContext.cpp`), always with the arm of `a` first, so for unions too
the result is a subset of `a`.

**Rationale.** Types are sets of values, ordered by inclusion, and the
intersection of `A` and `B` is their greatest lower bound (meet) `A ∧ B`: the
largest type contained in both. For two distinct function types `F1` and
`F2` the meet is non-empty (one permissive function value can satisfy both
signatures; this is why D5 does not call them disjoint), and it is not
expressible: it would be an intersection type `F1 & F2`, which the shallow
model (D4) has no kind for. Under D4 the only expressible type below both
`F1` and `F2` is `NoType`; the expressible types above `F1 ∧ F2` include
`F1`, `F2`, `F1 | F2` and `Object`. So `intersectTy` must approximate, and the
direction of the approximation decides soundness:

- Returning `NoType` approximates the meet **from below**. It claims that no
  value has both types, which is false, and the optimizer treats a `NoType`
  value as unreachable or a cast to it as always failing. That miscompiles:
  in `test/shermes/intersect-unrelated-function-types.js` a value typed
  `F1 | null` is cast to `F2 | null`, the cast was typed `null`, and the
  `-O` build threw "Checked cast failed" on a valid function.
- Returning `a` approximates the meet **from above**: `a ⊇ a ∧ b`. Every value
  the true answer admits is still admitted, so it is sound wherever the
  result is used as an upper bound on a value's type, which is how every
  caller uses it. The cost is precision (an extra, sound cast may survive)
  and asymmetry: the result is a subset of `a` but not necessarily of `b`, so
  `intersectTy(a, b)` and `intersectTy(b, a)` can differ.

Returning `a` rather than, say, `Object` keeps the most useful bound: callers
intersect a type they mean to keep (the target type of a `CheckedTypeCast` or
`UnionNarrowTrusted`, or a declared type that bounds an inferred one) with
one they know less about, and they pass the kept type first. That is the
argument-order contract documented on `TypeContext::intersectTy`. Whether
the result is `NoType` does not depend on the order, so emptiness checks may
pass either order.

**Consequences.** The law `isSubsetOf(intersectTy(a, b), b)` does not hold
for these pairs; `isSubsetOf(intersectTy(a, b), a)` and
`areDisjoint(a, b) ⇒ intersectTy(a, b) is NoType` still do. A consumer that
mutates an existing value's type to an intersection (rather than typing a new
value with it) must make sure the result is still a subset of the value's old
type, since other users of the value rely on it.

**Revisit when** either an `Intersection` type kind is added (the exact meet,
which would replace this decision), or deep structural subtyping (Phase 8)
makes more pairs comparable. With function variance, for example, `F1 <: F2`
may hold for the pair above, and the exact subtype branch then applies.

### D10. Precise runtime checks require deep structural subtyping

Approved by the owner on 2026-11-11.

**Decision.** D2 may be relaxed for a refined kind (codegen emitting a
precise runtime check instead of an object-ness check) only when the IR's
subtype relation for that kind agrees with the language's assignability
(Flow's `canAFlowIntoB`). For `Function` and `ExactObject` that requires deep
structural subtyping (function variance; field depth subtyping with variance,
which reverses D8), evaluated on runtime descriptors. Until then those kinds
keep the object-ness check. No kind gets a precise check before the runtime
type system exists (Phase 7, runtime type descriptors): a precise check needs
a runtime representation of the type it checks against. Phase 7 therefore
depends on deep structural subtyping for every structural kind; within it,
only `ClassInstance` can get precise checks first.

**Rationale.** Today a checked cast to a refined kind enforces nothing
beyond "is an object": any object passes a cast to `classInstance(Foo)` or
`function(F2)`, and the code after it trusts an unchecked type. That is only
tolerable because nothing in the emitted code depends on the refinement yet.
A precise check built on the shallow relation (D4) fails the other way: it
rejects valid values. With `F1 = (x: number | string) => number` and
`F2 = (x: number) => number | string`, Flow lets an `F1` closure flow into
`F2`, but `F1` and `F2` are distinct ids, so a check "the closure's
signature is `F2`" rejects a correct program. Per kind:

- **ClassInstance**: nominal, so the shallow relation is already the
  language's: "the object's class is `Foo` or a subclass" (a class id and the
  superclass chain) is exact. No dependency.
- **Function**: the check must accept every compatible signature
  (contravariant parameters, covariant return), so it needs a signature
  descriptor on every typed closure and the deep relation on descriptors.
- **ExactObject**: field names and order can be checked against the
  object's shape, but depth subtyping (a `{+x: number | string}` slot
  accepting `{x: number}`) needs field variance in the IR type (D8 dropped
  it) and a check of field values, which for recursive types walks the
  object graph.
- **Array, Tuple**: invariance is already right statically, but the runtime
  does not know an array's element type; a check needs an element-type tag
  on allocation or an O(n) scan, and a scan holds only until the array is
  written through another reference.

**Why it shapes the IR type system.**
- *What a runtime value must carry.* Precise checks fix what the runtime
  needs: signature descriptors on closures, shapes plus field types and
  variance for objects, element types for arrays. The IR type payloads
  become the source of those descriptors, so their content (variance,
  optional parameters, rest types) is part of the runtime contract, not
  only an optimizer detail.
- *Values without descriptors.* Untyped functions and arrays created by
  untyped code carry no signature or element type, so they cannot be checked
  up front. A strategy must be chosen before precise checks exist: wrap the
  value in a checking proxy (a new identity, which affects `===`, alias
  reasoning and D5), check on each use instead of at the cast, or reject
  such values. Each choice changes what a cast instruction means in the IR.
- *One relation, two places.* Redundant-cast elimination is sound only if
  the compiler's `isSubsetOf` implies that the runtime check passes. With
  precise checks, the compiler's subtype relation and the runtime's must be
  the same relation (co-inductive for recursive types), or the optimizer
  removes casts the runtime would fail, or keeps casts it should not.
- *The shallow-model workarounds.* The trusted narrow `narrowToFlowType`
  emits for structurally compatible refined types (and D9's approximation
  for overlapping, unrelated pairs) are justified by Flow's proof and by
  D2. They cannot coexist with precise checks of the same kinds; deep
  subtyping replaces them.

**Consequences.** Precise checks of any kind come only with the runtime type
system (Phase 7, runtime type descriptors). Within that work the order is
constrained:
1. Precise nominal checks for `ClassInstance`, from its runtime descriptor
   (class id plus superclass chain). Independent of deep subtyping.
2. Deep structural subtyping (D4's deferred part, with field variance back
   in the IR, D8), together with the strategy for values without
   descriptors.
3. Precise checks for `Function`, `ExactObject`, `Array` and `Tuple`.

Until step 3, D2 holds for the structural kinds, and casts to them remain
object-ness checks backed only by Flow's static guarantees.

## Phase Plan (Approach A: phase-by-phase, each wired live)

Ordered by difficulty. The lowering sections below describe what was
implemented (updated 2026-11-10); the original plan used Flow's `isLooping`
flag and a batch reserve/fill interner, which turned out not to fit (see
Phase 4).

Classes are nominal, so a class is identified by its Flow `ClassType` and
needs only an identity cache, no structural dedup. Structural cycles first
become possible with arrays (Phase 4).

### Phase 3 — ClassInstance

**Factories/accessors:** `createClassInstance(Type super, Identifier name,
ArrayRef<ExactObjectField> fields)` (non-interned); `getClassSuperType`,
`getClassName`, `getClassFields`; `isSubclass(a, b)`.

**Lowering (superclass first, then reserve):** classes can reference
themselves (`class Node { next: Node }`) or siblings (`class A { b: B }` /
`class B { a: A }`), so the lowerer (the `Class` arm of
`ESTreeIRGen::flowTypeToIRType`) must:
1. Look up the Flow `ClassType` in the identity cache (`loweredClassTypes_`);
   return if present.
2. Lower the superclass (finite nominal chain). This can lower the class
   itself through a field of the superclass, so look the class up again.
3. **Reserve** the entry with its superclass,
   `reserveClassInstance(superClass)`, and record it in the cache
   immediately (breaks self- and mutual recursion through fields).
4. Lower each data field type (self- and sibling references resolve via the
   cache).
5. **Fill** the name and fields, `fillClassInstance(t, name, fields)`.

The superclass is set at reservation because it is the only class payload
the type algebra reads (`isSubclass`): unions built while the fields are
lowered (e.g. a field `D | Base` in `class D extends Base`) are then
normalized exactly. No structural dedup and no co-inductive comparison:
classes are non-interned, so the identity cache plus reserve/fill is
sufficient.

**`flowTypeToIRType`:** `Class → ClassInstance(...)`. `ClassConstructor` stays
`Object` (becomes `Function` in Phase 4). Everything else unchanged.

**Type algebra:** implement `ClassInstance` in the id-aware leaf helpers
`isLeafSubtypeById` / `areLeafTypesDisjointById` (classes share one
`TypeKind`, so the kind-only helpers cannot tell them apart):
`ClassInstance <: Object`; `ClassA <: ClassB` via `isSubclass`; unrelated
classes disjoint; class vs primitive disjoint. `intersectTy(Animal, Dog) → Dog`;
`intersectTy(Dog, Cat) → NoType`; `subtractTy` conservative.

**Printing:** extend `TypeContext::print` to render refined kinds (e.g.
`classInstance(Dog)`; exact spelling settled via `update-lit`).

**Consumer audit:** IRVerifier (exact-Object assertions); IRPrinter /
`TypeContext::print`; TypeInference (`intersect`/`union` now see
`ClassInstance`; confirm AllocTypedObject/PrLoad result types propagate);
InstSimplify (redundant `CheckedTypeCast` elimination now fires for class
casts — verify it stays sound); SH + HBC codegen (treat `ClassInstance` as
`Object` at every `TypeKind::Object`/`isObjectType()` switch).

**Tests:** unit — `createClassInstance`, `isSubclass`, subtype/disjoint/
intersect over hierarchies, reserve-fill on self- and mutually-recursive
classes. Lit — typed-class programs whose IR dumps now show `classInstance(...)`;
`update-lit` review. Full `-j1` lit + `HermesIRTests` + VMRuntime unit tests
green; `arc f`.

### Phase 4 — Array + Function (+ ClassConstructor); cycle-breaking interner

**Factories/accessors:** `createArray(Type elem)` and `createFunction(ret,
thisTy, restTy, ArrayRef<FunctionParam>, isAsync, isGenerator)`, both
**interned** by a shallow key over child IDs (extend the intern machinery with
per-kind keys alongside `UnionInternKey`). Accessors: `getArrayElementType`;
`getFunctionReturn/This/Rest/Params/flags`.

**`flowTypeToIRType`:** `Array → createArray(elem)`;
`TypedFunction → createFunction(signature)`;
`ClassConstructor → createFunction(constructorSignature)` (signature from the
Flow `ClassType`'s `constructorType_`). `UntypedFunction → Object`,
`NativeFunction → Number`, `CPtr → Number` (unchanged).

**Recursive-type lowering (lands here):** arrays and functions are the first
kinds that can form cycles (e.g. `type Json = number | string | Json[]`,
`type F = (x: number) => F`). Flow's `isLooping` flag cannot drive this:
`FindLoopingTypes` stops at nominal types, and `Array<T>` is a nominal
`ClassType`, so array cycles are never flagged. The lowerer detects cycles
itself, with **lazy reservation** (`beginStructuralLowering`,
`finishStructuralLowering`, `recordStructuralLowering`, used by
`lowerArrayClassType` and `lowerTypedFunction`):
- When lowering of a structural Flow type starts, it is marked in progress
  (`loweringInProgress_`), with nothing reserved.
- If lowering its children reaches the same type again, the type is
  recursive: an entry is reserved (`reserveArray`/`reserveFunction`) and
  its id is returned to the inner reference.
- When the children are done, the reserved entry is filled; if nothing was
  reserved, the interning factory (`createArray`/`createFunction`) is used,
  so non-recursive types stay interned. The result is recorded in
  `loweredStructuralTypes_`, which is checked first.

Recursive entries are not interned by `TypeContext`, so two Flow types that
Flow considers equal (an alias and a function declared as returning it) would
get unrelated ids and fail the strict `StoreFrameInst` check. Both maps are
therefore keyed by Flow's co-inductive `TypeInfo::equals`
(`FlowTypeStructuralInfo`, whose hash also mixes in the children's shallow
hashes, since Flow's own `hash()` reads only the node). A type equal to one
in progress shares its reserved entry, so equal types also share an id
inside a cycle.

A reserved entry's kind and id are valid at once, and the type algebra reads
only the kind and id of a structural entry, so unions and other types can be
built from it before it is filled; no union subsumption is skipped.

**Type algebra:** `Array`/`Function <: Object`; within-kind subtyping is
invariant / id-equality (per D4); disjointness conservative (per D5).

**Consumer audit & tests:** same playbook (codegen/verifier treat
`Array`/`Function` as `Object`). Unit — array/function factories + interning
dedup; recursive-type interning (`type Json = …`): assert termination, correct
cyclic id structure, and that two structurally-equal recursive types get one
id. Lit — typed arrays/functions/`new`-able classes; `update-lit` review.
`-j1` lit + unit tests green; `arc f`.

### Phase 5 — Tuple + ExactObject

**Factories/accessors:** `createTuple(ArrayRef<Type> elems)` interned by
ordered element ids; `createExactObject(ArrayRef<ExactObjectField> fields)`
interned by ordered `(name, typeId)` pairs (**field order significant**:
`{x,y}` ≠ `{y,x}`). Accessors: `getTupleElements`, `getExactObjectFields`.

**`flowTypeToIRType`:** `Tuple → createTuple`, `ExactObject → createExactObject`
(an exact object with an indexer stays `Object`, D7; field variance is dropped,
D8). After this phase the only Flow kinds still collapsing are the intentional
ones (`UntypedFunction → Object`, `NativeFunction → Number`, `CPtr → Number`,
`Any/Empty/Mixed → AnyType`), and the constructor of a class without an
explicit constructor signature, which has no signature to lower and stays
`Object`.

**Lowering:** no new machinery: `lowerTupleType` and `lowerExactObjectType`
use Phase 4's lazy reservation and Flow-equality maps, with reserve/fill
pairs for the new kinds (this is where `type Tree = null | {val, left: Tree,
right: Tree}` exercises an `ExactObject` inside a cycle).
`FlowTypeStructuralInfo` also mixes in the children of tuples and objects.

**Type algebra — totality pass:** with all five refined kinds present, do an
explicit review that `isSubsetOf`/`areDisjoint`/`intersectTy`/`subtractTy`/
`unionTy` return a sound, well-defined result for **every** pair of kinds
(refined×refined, refined×primitive, refined×union, union×union). Tuple/
ExactObject use the same invariant/id-equality + conservative-disjoint rules.

**Consumer audit & tests:** codegen/verifier treat `Tuple`/`ExactObject` as
`Object`. Unit — tuple/exactobject factories, field-order significance, the
recursive `Tree` interning, and a matrix test asserting totality across all
kind pairs. Lit — typed tuple/object-literal programs; `update-lit` review.
`-j1` lit + unit tests green; `arc f`.

## Cross-Phase Concerns

### Deliverables per phase

TypeContext factories/accessors + algebra for that phase's kinds; the
`flowTypeToIRType` flip for those kinds; the interner increment; the consumer
audit; unit + lit tests; `progress.md` updated.

### primMask interaction

Refined kinds are `kNotMaskable` (already the default), so they always take the
general path. No primitive-mask fast-path changes are needed; the existing
maskable check already routes refined operands to the general path.

### Error handling

Unreachable Flow kinds keep `hermes_fatal`. Type-table access keeps its bounds
asserts. A reserved-but-unfilled entry's kind and id are valid at once (and,
for a class, its superclass), and the type algebra may use it before fill;
only payload accessors and printing of the payload require the fill. Each
reserved entry is filled exactly once, and the lowerer asserts that a type
is not lowered twice.

### Verification bar (every phase)

- `HermesIRTests` unit tests for new ops/factories/interner.
- Lit run with `LIT_OPTS="-j1"` (Claude Code macOS sandbox needs
  single-process; use `LIT_FILTER` to scope during iteration) and `update-lit`
  for IR-dump changes, reviewed before regenerating.
- VMRuntime unit tests.
- `arc f` on all touched C++.
- test262 only on explicit request.
- Each phase leaves the tree green before the next begins.

### Primary risk & mitigation

Recursive-type lowering (Phase 4) is the riskiest component. Mitigations:
unit tests on hand-built recursive IR entries (reserve/fill, the algebra on
reserved entries, printing), lit and `-verify-ir` exec tests on recursive
aliases of every structural kind, and the D2
"codegen treats refined as Object" invariant, which keeps the live blast radius
to optimizer precision rather than correctness.

## Open Questions

None blocking. Deferred by design: deep structural subtyping (Phase 8),
runtime descriptors (Phase 7), cross-category disjointness, `Int32`/`Uint32`/
`bits32`.

Open for the deferred work (D10): how values that carry no descriptor
(untyped functions, arrays from untyped code) are handled once structural
kinds get precise runtime checks: wrapping in a checking proxy, checking on
each use, or rejecting them. The choice changes what a cast means in the IR
and must be made together with deep structural subtyping.
