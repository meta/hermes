# IR Type System v2 — Design Proposal

## Problem

The current IR `Type` is a 16-bit bitmask of 13 primitive kinds. It can express
"this value might be a number or a string" but cannot express "this value is an
instance of class Foo" or "this is an array of numbers". The Flow type system
(FlowContext) has full nominal classes, typed arrays, function signatures, tuples,
and exact objects — but none of that information survives lowering into IR.

We want to enrich the IR type system so it can carry Flow-level type information
through optimization and into codegen. The type system will also generate runtime
type representations for soundness enforcement, checked casts, and type guards.

## Design Goals

1. **Expressive**: can represent everything the Flow type system expresses —
   nominal classes, typed arrays, function signatures, tuples, exact objects,
   and arbitrary unions thereof.

2. **Correct**: every type operation (union, intersect, subset, etc.) is
   well-defined and sound for all type combinations. Every operation
   produces a valid result for every input — no "returns garbage" fallback
   paths. Some operations (notably `subtract`) may return a conservative
   approximation when the exact result is not representable.

3. **Orthogonal**: Union is the single, uniform composition mechanism for all
   types. The type hierarchy has clean, independent dimensions with no ad-hoc
   encoding distinctions.

4. **Backwards-compatible**: the existing IR type system is a proper subset.
   All current types and operations continue to work. Flow types lower to IR
   types.

5. **Suitable for runtime**: types can be lowered to compact runtime type
   descriptors for checked casts and type guards.

6. **Efficient** (deferred): performance optimizations (bitmask fast paths,
   compact encoding) can be layered on without changing the type algebra.

## Type Hierarchy

A Type represents a **set of possible runtime values**. Every runtime value
belongs to exactly one *base type*. A Type is either a base type, a union of
types, or the empty type (NoType).

```
Type
 ├── NoType                          -- empty set (bottom, unreachable)
 ├── Primitives
 │    ├── Undefined
 │    ├── Null
 │    ├── Boolean
 │    ├── Number (fp64)
 │    │    ├── Int32                  -- fp64, value in int32 range
 │    │    └── Uint32                 -- fp64, value in uint32 range
 │    ├── BigInt
 │    ├── String
 │    └── Symbol
 ├── Object                          -- any object, no refinement
 │    ├── ClassInstance(classId)      -- nominal, with inheritance
 │    ├── Array(elemType)            -- known element type
 │    ├── Tuple(elemTypes...)        -- fixed-length, positional
 │    ├── Function(signature)        -- known param/return types
 │    └── ExactObject(fields...)     -- known exact field set
 ├── Internal
 │    ├── Empty                      -- TDZ
 │    ├── Uninit                     -- declared, not initialized
 │    ├── Environment
 │    ├── PrivateName
 │    ├── FunctionCode
 │    └── bits32                     -- physical 32-bit integer, signless
 └── Union(Type, Type, ...)          -- union of two or more types
```

### Primitive Types

Primitives correspond to JS primitive values. Each is a distinct kind — a
runtime value is exactly one primitive kind.

| Type | Description |
|------|-------------|
| Undefined | The JS `undefined` value |
| Null | The JS `null` value |
| Boolean | `true` or `false` |
| Number | IEEE 754 double-precision float |
| BigInt | Arbitrary-precision integer |
| String | JS string |
| Symbol | ES2015 symbol |

**Number subtypes**: `Int32` and `Uint32` are subtypes of `Number`. They
represent values that are still fp64 at runtime but are known to be integers
within a specific range. They are not separate runtime kinds — they are
refinements that enable integer-specific optimizations.

- `Int32`: integer value in [-2³¹, 2³¹ - 1].
- `Uint32`: integer value in [0, 2³² - 1].
- `Int32` and `Uint32` overlap (both contain [0, 2³¹ - 1]) but neither is a
  subtype of the other.

### Object Types

`Object` is the base type for all JS objects. It represents "some object, no
further information." Object refinements narrow this to specific categories:

| Refinement | Description |
|------------|-------------|
| ClassInstance(classId) | Instance of a specific nominal class. Subtyping follows the class inheritance chain. |
| Array(elemType) | Array with a known element type. Legacy arrays use `Array(any)`. Invariant (not covariant) because arrays are mutable. |
| Tuple(elemTypes...) | Fixed-length array with known positional types. Invariant. |
| Function(signature) | Callable with known parameter and return types. Signature is immutable (derived from FunctionCode). Legacy functions use `Function(any, ...) => any`. |
| ExactObject(fields...) | Object with a known exact set of typed fields. No extra properties. |

All object refinements are subtypes of `Object`. Different refinement categories
are disjoint — a value cannot be both a ClassInstance and an Array.

**Legacy vs typed objects**: the type system does NOT distinguish between
"legacy" (dynamic shape) and "typed" (fixed shape) objects. Legacy JS objects
can be typed with their known type information (e.g., a legacy function's
signature comes from its FunctionCode). The distinction between dynamic and
fixed shapes is encoded in the instructions that access them (PrLoad/PrStore
for typed access, LoadProperty/StoreProperty for legacy), not in the type
system.

### Internal Types

Internal types represent values that exist in the IR but are not JS-visible:

| Type | Description |
|------|-------------|
| Empty | Temporal Dead Zone — variable before its declaration |
| Uninit | After declaration, before initialization (maps to `undefined` at runtime) |
| Environment | Lexical scope |
| PrivateName | Private class field name |
| FunctionCode | IR function reference (the code, not a closure). Will carry the precise function signature in the future; currently untyped for backwards compatibility. |
| bits32 | Physical 32-bit integer, signless. Sign is a property of the consuming operation. Fits in a NaN-boxed register. |

`bits32` is fundamentally different from `Number` and its subtypes. `Number`
(including `Int32` and `Uint32`) is a JS value with fp64 representation.
`bits32` is a physical 32-bit integer with integer representation.
Specialized instructions handle conversion between `bits32` and `Number`.

### Union Types

Union is the **single, uniform composition mechanism**. `Union(A, B, ...)`
represents a value that could be any one of its member types.

```
Union(Number, String)          -- number or string
Union(ClassA, ClassB)          -- instance of ClassA or ClassB
Union(Number, ClassA, Null)    -- number, ClassA instance, or null
```

There is no conceptual distinction between unions of primitives and unions of
refined types. The bitmask encoding in the current IR is an implementation
optimization of primitive unions, not a separate concept.

**Canonical form**: unions are maintained in canonical form:
- Nested unions are flattened: `Union(A, Union(B, C))` = `Union(A, B, C)`.
- Single-element unions collapse: `Union(A)` = `A`.
- Duplicate arms are removed.
- Subsumed arms are removed: `Union(Int32, Number)` = `Number`.
- Arms are sorted for deterministic comparison.

### Special Types

- **NoType**: the empty set. No possible values. Bottom of the type lattice.
  Represents unreachable code.
- **AnyType**: the union of all JS-observable types (all primitives + Object).
  Top of the JS type lattice. Equivalent to today's `TYPE_ANY_MASK`.

`any` is not a distinct type kind — it is the maximal union. When Flow's `any`
type requires checked casts on output, that is an instruction-level decision
made during IRGen, not a property of the IR type.

## Subtype Relationships

The subtype relation `A <: B` means every value of type A is also a value of
type B (set inclusion).

### Rules

**Primitives:**
- `Int32 <: Number`.
- `Uint32 <: Number`.
- All other primitive kinds are pairwise disjoint.

**Objects:**
- Every object refinement `<: Object`.
- `ClassB <: ClassA` if ClassB extends ClassA (nominal inheritance chain).
- Two ClassInstances with no inheritance relationship are disjoint.
- Different refinement categories are disjoint (ClassInstance ∩ Array = ∅).

**Unions:**
- `A <: Union(..., A, ...)` — every type is a subtype of any union containing it.
- `Union(A₁, ..., Aₙ) <: B` iff `Aᵢ <: B` for all i.

**General:**
- `NoType <: T` for all T.
- `T <: AnyType` for all JS-observable T.
- Reflexive: `T <: T`.
- Transitive: `A <: B` and `B <: C` implies `A <: C`.

## Type Operations

### union(A, B)

Returns the smallest type containing all values of both A and B.

```
union(A, B):
  if A == B: return A
  if A == NoType: return B
  if B == NoType: return A
  if A <: B: return B
  if B <: A: return A
  return canonical Union(A, B)
```

For object refinements:
- `union(ClassA, ClassB)` where A extends B → B (the superclass).
- `union(ClassA, ClassB)` unrelated → `Union(ClassA, ClassB)`.
- `union(Array(T), Array(T))` → `Array(T)`.
- `union(Array(S), Array(T))` where S ≠ T → `Union(Array(S), Array(T))`.
  NOT `Array(union(S, T))` — arrays are invariant because they are mutable.

### intersect(A, B)

Returns the largest type whose values are in both A and B.

```
intersect(A, B):
  if A == B: return A
  if A <: B: return A
  if B <: A: return B
  if A and B are disjoint: return NoType
  distribute over unions:
    intersect(Union(A₁,...,Aₙ), B) = union(intersect(A₁,B), ..., intersect(Aₙ,B))
```

Key cases:
- `intersect(Number, Int32)` = Int32.
- `intersect(Object, ClassA)` = ClassA.
- `intersect(ClassA, ClassB)` unrelated = NoType.
- `intersect(Number, String)` = NoType.

### subtract(A, B)

Removes from A all values that are in B. Returns a conservative approximation
(superset of the true result) when the exact difference is not representable.

```
subtract(A, B):
  if A <: B: return NoType
  if A and B are disjoint: return A
  distribute over unions:
    subtract(Union(A₁,...,Aₙ), B) = union(subtract(A₁,B), ..., subtract(Aₙ,B))
```

Key cases:
- `subtract(Union(Number, String), Number)` = String (exact).
- `subtract(Union(ClassA, ClassB), ClassA)` = ClassB (exact).
- `subtract(Object, ClassA)` = Object (conservative — "all objects except
  ClassA" is not representable).
- `subtract(Number, Int32)` = Number (conservative).

### isSubsetOf(A, B)

Returns true iff `A <: B`. See Subtype Relationships above.

## Function Signatures

```
FunctionSignature:
  returnType   : Type
  thisType     : Type              -- NoType if no `this` parameter
  restType     : Type              -- NoType if no rest parameter
  params       : [(Type, bool)]    -- (type, isOptional) per parameter
  isAsync      : bool
  isGenerator  : bool
```

Note: `restType` represents the element type of a trailing rest parameter
(e.g., `...args: string[]` has `restType = String`). Flow's
`TypedFunctionType` does not currently represent rest parameters; this field
is forward-looking.

**Subtyping** uses standard variance:
- Contravariant in parameters (callee accepts at least what caller provides).
- Covariant in return type (callee returns at most what caller expects).
- Async/generator flags must match exactly.

Two `Function` types with different signatures are disjoint unless related
by the variance rules above.

## Class Types and Inheritance

Classes are nominal — identity is the type table index, not structure. Two
classes with identical fields are different types if they come from different
class declarations.

A `ClassInstance` type entry stores the superclass relationship, class name,
and field layout directly in its `TypeEntry` payload:

```
ClassInstance payload:
  superClassTypeId : uint32       -- type table ID of parent ClassInstance
                                     (kNoTypeId if no parent)
  className        : Identifier   -- for runtime type descriptors and
                                     error messages
  fields           : [(name, Type)]  -- direct fields
```

There is no separate class registry or class ID namespace. The type table
entry's own `id_` serves as the class identity. The `createClassInstance()`
method on `TypeContext` creates a new (non-interned) entry for each
class declaration.

**Subtyping**: `ClassInstance(A) <: ClassInstance(B)` iff B is an ancestor
of A in the inheritance chain. The chain is walked via the
`superClassTypeId` field in the type table entries.

**Disjointness**: two classes with no inheritance relationship are disjoint.
This enables precise intersection (`intersect(ClassA, ClassB)` = NoType for
unrelated classes).

## Relationship to Flow Types

| Flow Type | IR Type |
|-----------|---------|
| `void` | Undefined |
| `null` | Null |
| `number` | Number |
| `string` | String |
| `boolean` | Boolean |
| `bigint` | BigInt |
| `symbol` | Symbol |
| `any` | AnyType |
| `mixed` | AnyType (`mixed` is a compilation-level concept, not present in IR) |
| `ClassFoo` | ClassInstance(classFooId) |
| `Array<number>` | Array(Number) |
| `[number, string]` | Tuple(Number, String) |
| `(x: number) => string` | Function({params: [Number], return: String}) |
| `{x: number, y: string}` | ExactObject({x: Number, y: String}) |
| `ClassA \| null` | Union(ClassInstance(A), Null) |
| `ClassA \| ClassB` | Union(ClassInstance(A), ClassInstance(B)) |
| `number \| string` | Union(Number, String) |

**Not lowered to IR** (resolved before IR):
- Generics (monomorphized or erased).
- Type aliases (resolved to concrete types).
- `InferencePlaceholder` (resolved during checking).

**Lowered indirectly**:
- `ClassConstructorType` → `Function(constructorSignature)`. A class
  constructor is a callable with a known signature; until now lowered to
  unrefined `Object` due to limitations of the old type system.

## Relationship to Current IR Type System

Every type in the current 16-bit bitmask system maps directly:

| Current IR | New IR |
|------------|--------|
| `Number` bit | Number |
| `String` bit | String |
| `Boolean` bit | Boolean |
| `Object` bit | Object |
| `Null` bit | Null |
| `Undefined` bit | Undefined |
| `BigInt` bit | BigInt |
| `Symbol` bit | Symbol |
| `Empty` bit | Empty |
| `Uninit` bit | Uninit |
| `Environment` bit | Environment |
| `PrivateName` bit | PrivateName |
| `FunctionCode` bit | FunctionCode |
| Multiple bits set | Union of corresponding types |
| `notype` (0) | NoType |
| `any` mask | AnyType |

The new system is a strict superset. All existing type queries (`canBeNumber()`,
`isPrimitive()`, `isSubsetOf()`, etc.) have direct equivalents with identical
semantics for the primitive-only case.

## Runtime Type Representation

The type system must produce compact runtime type descriptors for:

1. **Checked casts**: `CheckedTypeCastInst` carries a target type. The runtime
   verifies the value matches.
2. **Type guards**: conditional branches narrow types; the narrowed type may
   need runtime verification.
3. **Soundness enforcement**: typed function parameters are checked at call
   boundaries when called from untyped code.

Runtime type checks by category:

| Check | Mechanism |
|-------|-----------|
| Primitive kind (number, string, ...) | Runtime type tag comparison |
| ClassInstance(id) | Class ID lookup in instance's class chain |
| Array(elemType) | Tag check (is array) + element type from array metadata |
| Function(sig) | Tag check (is callable) + signature from FunctionCode metadata |
| Union | Check each arm in sequence |

The specific encoding of runtime type descriptors is an implementation detail
to be determined when codegen support is added.

**Dependency on subtyping (added 2026-11-11, decision D10 in
`phase3-5-design.md`).** A precise runtime check must accept every value the
language accepts, so for each kind the runtime check and the IR subtype
relation must be the language's assignability relation. Phases 3–5 implemented
shallow subtyping (functions and exact objects equal only to themselves), so:
- `ClassInstance` checks can be precise as soon as descriptors exist: the
  relation is nominal and already exact.
- `Function` checks must apply the variance rules above to signature
  descriptors; an identity check would reject a closure of a compatible
  signature. `ExactObject` checks need field variance, which the IR currently
  drops. Both require deep structural subtyping first.
- `Array` checks need an element type the runtime does not have today, and
  untyped functions have no signature metadata at all. Values without
  descriptors need a strategy (a checking wrapper, per-use checks, or
  rejection) decided together with deep subtyping.
- The compiler's `isSubsetOf` must imply that the runtime check passes, or
  redundant-cast elimination becomes unsound.

## Representation (Implementation Sketch)

The following is a sketch — specific encoding decisions are deferred until
performance requirements are clearer.

### Type Value

A `Type` is a small, copyable value (ideally 4 bytes). It acts as an opaque
handle into a type table owned by the Module.

### TypeContext

Owned by each `Module`. Stores all interned types and the class inheritance
information needed for subtype checks. Type operations that involve refined
types (union, intersect, isSubsetOf on non-primitive types) access the context
to consult the type table.

### Primitive Optimization (deferred)

As an optimization, the common case of primitive-only types (unions of
primitives with no object refinement) can be encoded directly in the handle
without a table lookup — e.g., via a bitmask embedded in the handle. This is
purely an implementation optimization and does not affect the type algebra.

## Interning Algorithm: Flow Types to IR Types

When lowering Flow types to IR types, each unique Flow type must be assigned
a unique `IRTypeID`. Structurally identical Flow types (even with different
pointers) must receive the same ID, so that IR type equality is a simple
integer comparison.

### Challenge: Looping Types

Flow types can be recursive (looping). For example:

```
type Tree = null | {val: number, left: Tree, right: Tree}
```

This creates a cycle: the union contains an ExactObject whose fields refer
back to the union. Flow's `FindLoopingTypes` pass marks every type node on
a cycle with `isLooping = true` — not just union arms, but every intermediate
node (the ExactObject, Array, etc.).

Looping types cannot be deeply hashed or structurally compared by normal
recursion — the traversal would never terminate. Flow already solves this:
- `TypeInfo::hash()` uses shallow hashing for looping types (kind + arity).
- `TypeInfo::equals()` uses co-inductive comparison with a visited-pairs set.

Flow also provides `TypesDenseMapInfo` (in `FlowTypesDumper.h`), a
`DenseMapInfo<const TypeInfo *>` specialization that delegates to `hash()` and
`equals()`. This can be reused as the basis for interning tables.

### Data Structures

Three tables:

1. **Identity table**: `DenseMap<const TypeInfo *, IRTypeID>` — keyed by
   pointer identity. This is the fast path: once a Flow type has been
   processed, its pointer maps directly to an IR ID with no hashing or
   structural comparison.

2. **Non-looping dedup table**: `DenseMap<const TypeInfo *, IRTypeID,
   TypesDenseMapInfo>` — keyed by deep structural hash + structural equals.
   Ensures that two different `TypeInfo *` pointers representing structurally
   identical non-looping types receive the same IR ID.

3. **Looping dedup table**: `DenseMap<const TypeInfo *, IRTypeID,
   LoopingTypesDenseMapInfo>` — keyed by shallow hash + co-inductive equals.
   Ensures that structurally identical cycles receive the same IR IDs.

The non-looping and looping dedup tables are separate because their
hash/equality semantics differ: deep structural comparison terminates for
non-looping types but would recurse infinitely for looping types. Mixing them
in one table would require every lookup to handle both cases, complicating the
`DenseMapInfo` and risking subtle bugs.

### Algorithm

```
intern(type) → IRTypeID:

  // 1. Fast path: already processed this exact pointer?
  Look up type in identity table (pointer equality).
  If found → return existing ID.

  // 2. Non-looping type.
  if !type->isLooping:
    Look up type in non-looping dedup table (deep structural hash + equals).
    If found → record in identity table, return existing ID.
    Recursively intern all sub-types.
    Allocate new IRTypeID.
    Insert into both identity table and non-looping dedup table.
    Return new ID.

  // 3. Looping type.
  Walk the type graph from type, collecting all reachable looping types
  not already in the identity table — the "cycle cluster."
  Intern any non-looping sub-types encountered along the way (step 2).
  Deduplicate the cluster against the looping dedup table
  (shallow hash + co-inductive equals).
  If equivalent cycle found → map all nodes to existing IDs.
  Otherwise → batch-allocate IRTypeIDs for all nodes in the cluster.
  Insert all cluster nodes into both identity table and looping dedup table.
  Return the ID for type.
```

### Motivation and Design Rationale

**Why three tables?**

The identity table is an optimization: most lookups during recursive interning
revisit types that have already been processed (e.g., `Number` appears in many
field types). A pointer-keyed lookup is O(1) with no hashing and avoids the
cost of structural comparison entirely.

The two dedup tables handle the case where different `TypeInfo *` pointers
represent structurally identical types. Flow's own interning is partial —
it deduplicates within unions but does not globally deduplicate all types.
Without dedup tables, structurally identical types from different parts of
the Flow type graph would receive different IR IDs, breaking the invariant
that IR type equality is an integer comparison.

**Why separate dedup tables for looping vs non-looping?**

Non-looping types use deep structural hashing: the hash incorporates the
full structure of the type, including all sub-types recursively. This gives
excellent hash distribution and precise equality. It terminates because
there are no cycles.

Looping types cannot use deep hashing — the recursion would not terminate.
They use shallow hashing (kind + arity) which produces more collisions, and
co-inductive equality (with a visited-pairs set to detect cycles). Mixing
these in one table would mean either: (a) using shallow hashing for all types
(poor distribution for non-looping), or (b) checking `isLooping` inside the
`DenseMapInfo` methods (fragile, since the hash strategy must be consistent
across all entries in the table).

**Why batch-allocate looping types?**

A cycle like `Tree → ExactObject → Array(Tree)` contains multiple type nodes
that reference each other. You cannot fully intern any one without the others
having IDs. Batch allocation assigns IDs to all nodes in the cycle
simultaneously, then fills in cross-references using those IDs.

Every node in the cycle needs its own ID — not just the "root." Intermediate
nodes (like `Array(Tree)`) are real types that can appear independently in
other contexts and must be internable.

**Why is the identity table sufficient as a fast path?**

During recursive interning, the same `TypeInfo *` pointer is encountered
repeatedly — once when first processing a type, and again each time it appears
as a sub-type of another type. The identity table caches the result of the
first processing, making subsequent encounters O(1). The dedup tables are
only consulted once per unique pointer, during initial processing.

**Leveraging Flow's infrastructure.**

Flow already provides `TypeInfo::hash()`, `TypeInfo::equals()`, the
`isLooping` flag on every cycle node, and `TypesDenseMapInfo`. The interning
algorithm builds on these rather than reimplementing structural comparison.
This ensures consistency with Flow's type identity semantics and avoids
duplicating complex cycle-handling logic.

## Migration Plan

### Phase 1: Type representation and context (API-compatible)

- Define `Type` as a handle into `TypeContext`.
- Implement `TypeContext` with interning. Owned by `Module`.
- Use a **thread-local pointer** to maintain the "current" `TypeContext`.
  An RAII guard (`TypeContextRAII`) sets the thread-local at pass entry
  points. All type operations (`unionTy`, `intersectTy`, `isSubsetOf`, etc.)
  use `TypeContext::current()` when they need the table.
- Register all existing primitive types as interned entries.
- All existing type operations delegate to the new implementation but produce
  identical results. **No call-site changes required.**
- All existing tests pass unchanged.
- **Risk**: low. Semantics are unchanged; the thread-local is invisible to
  callers.

### Phase 2: Explicit context parameter (mechanical)

- Mechanically transform all call sites of `unionTy`, `intersectTy`,
  `subtractTy`, `isSubsetOf`, etc. to accept an explicit `TypeContext &`
  parameter.
- Remove the thread-local from `TypeContext`.
- This is a large but purely mechanical change with no semantic effect.
  The thread-local from Phase 1 provides a working system to migrate from
  incrementally.

### Phase 3: Class types

- Add `ClassInstance` to the type system.
- Lower Flow `ClassType` to `ClassInstance` during IRGen.
- `CheckedTypeCastInst` carries class types.
- Implement class subtype checking via inheritance chain.

### Phase 4: Function signatures and arrays

- Add `Function(signature)` and `Array(elemType)`.
- Lower Flow function and array types during IRGen.
- Enable signature-aware call-site analysis.

### Phase 5: Tuple, ExactObject

- Add remaining object refinements.
- Complete union/intersect/subset logic for all type combinations.

### Phase 6: Number subtypes and bits32

- Add `Int32`, `Uint32` subtypes of Number.
- Add `bits32` internal type.

### Phase 7: Runtime type descriptors

- Generate compact runtime type descriptors from IR types.
- Codegen emits type checks using descriptors.
- Ordering constraint (D10 in `phase3-5-design.md`): only nominal class
  checks can land before deep structural subtyping. Precise checks of
  functions, exact objects, arrays and tuples require deep structural
  subtyping (and a strategy for values without descriptors) first; on the
  shallow relation they would reject valid values.

### Phase 8: Exploitation in optimization

- Type inference produces and propagates refined types.
- InstSimplify uses refined types for more aggressive simplification.
- Codegen emits specialized code paths for known types.

## Open Questions

1. **Number subtype precision**: `Int32 ∩ Uint32` = non-negative int32 values.
   This is not directly representable. Is conservative approximation sufficient,
   or do we need finer number subtypes?

3. **Per-Module vs shared type context**: per-Module is simpler. Shared enables
   cross-module type identity but requires index translation.

4. **Class inheritance storage**: should the type context store the full
   superclass chain per class, or just the parent with a separate lookup?
   Tradeoff: faster subtype checks vs. compact storage.

5. **FunctionCode typing**: FunctionCode will carry the precise function
   signature in the future. Currently untyped for backwards compatibility.
   The migration path is to add an optional signature to FunctionCode's type
   representation when ready.
