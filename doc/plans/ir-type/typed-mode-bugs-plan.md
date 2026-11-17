# Plan: fix three typed-mode bugs found during the IR type v2 review

Bugs (filed in `dz/issues/`): `01a1c623-6e9a` (calls to typed async and
generator functions), `01a1c623-71c5` (typed parameters of async and
generator functions), `01a1c623-7516` (`new Array<T>()`). All three
reproduce on the master commit the IR type v2 stack is based on
(3f124a87e23c), so they are not caused by it.

## Base and order

Work on top of the IR type v2 stack (D112825925 ... D112825922), not on
bare master: the stack changed the code two of the fixes touch
(`genAsyncFunction`/`genGeneratorFunction` now set the closure's function
type; `IRGen/flow/array-class-allocation.js` pins the current, wrong
`new Array<number>()` lowering). Fixing on master would conflict with the
stack and change that test twice.

One commit per bug, each with its tests and with its dz issue closed
(`dz close <id> --as fixed`, committed with the fix). Order: bug 2, bug 1,
bug 3. Bugs 1 and 2 are independent but both touch async/generator
functions, and bug 2 is the smallest.

Verification bar for each commit: ASan Debug build (clang, build dir
outside the checkout), `HermesIRTests`, full `check-hermes` with 0
unexpected failures, `update-lit` leaves no diff, `arc lint` clean. Every
new test is shown to fail with its fix reverted (the mutations are named
below).

## Bug 2: typed parameters of async and generator functions

**Symptom.** `-verify-ir` fails after `LowerGeneratorFunction`:
"StoreFrameInst: Value type mismatch".

```js
async function af(x: number): number { const y: number = x + 1; return y; }
function *g(x: number): number { yield x; return x + 1; }
```

Both fail at `-O0` and `-O`; any typed parameter triggers it.

**Cause.** For a typed function, IRGen gives each parameter its Flow type
(`jsParam->setType(irType)`, `lib/IRGen/ESTreeIRGen-func.cpp:1166`). The
body of an async or generator function lives in the inner function, so
the inner function owns the typed parameters.
`LowerToStateMachine::moveLocalsToOuter`
(`lib/Optimizer/Scalar/LowerGeneratorFunction.cpp:270`) moves them to the
outer function. For each inner parameter it:
- creates a `Variable` with the parameter's type (`jsParam->getType()`,
  line 281);
- creates a new outer `JSDynamicParam` without a type (line 293), so its
  `LoadParamInst` is `any`;
- stores that `any` into the `number` variable (line 295). The verifier
  rejects the store.

`this` (index 0, line 285) is not affected: the outer function runs
`emitFunctionPrologue` on the same function node, which types `this` the
same way (`ESTreeIRGen-func.cpp:797`).

**Fix.** Give the new outer parameter the inner parameter's type:
`outerJSParam->setType(jsParam->getType())`. For index 0, assert that the
outer `this` type equals the inner one.

**Soundness.** This is the trust model a plain typed function already
has: its `LoadParamInst`s carry the Flow types, justified by the
FlowChecker checking typed callers. Since the IR type stack, the outer
closure carries the function's signature (`setFunctionTypeFromFlow`), so
typed callers see the same parameter types. An untyped caller passing a
string can mistype the value here exactly as it can with a plain typed
function; the fix adds no new trust.

**Tests.** New `test/shermes/typed-async-generator-params.js`:
- an async function and a generator with typed parameters, run with
  `-verify-ir -exec` at `-O0` and `-O` on `shermes`, and with
  `hermes -typed -verify-ir`. Each prints its parameters from inside the
  body (an async function runs synchronously until its first `await`).
- an IR check after lowering that the outer `LoadParamInst` of `x` is
  `(:number)`.

Fails before: every run fails verification. Mutation: remove the
`setType` call; the runs fail again.

## Bug 1: calls to typed async and generator functions

**Symptom.** The call is typed with the annotated return type, and the
compiled code trusts it.

```js
async function af(): number { return 1; }
const r = af();
print(typeof r);   // -O0: "object"; -O: "number" (typeof folded)
```

A generator call (`function *g(): number`, then `g()`) fails verification
at `-O` ("CallInst: NoType instruction has output", after TypeInference):
the call is declared `number` and TypeInference intersects that with the
callee's inferred result, a generator object.

**Cause.** The FlowChecker types a call with `ftype->getReturnType()` at
four sites in `lib/Sema/FlowChecker-expr.cpp`:
- `:3013`, static `SH_call` dispatch;
- `:3111`, the main call path;
- `:3335`, `$SHBuiltin.call`;
- `:3410`, `fn.call(...)`.

IRGen sets the `CallInst` type the same way when the callee's Flow type is
a `TypedFunctionType` (`lib/IRGen/ESTreeIRGen-expr.cpp:1093-1094`, in
`emitCall`). For an async function the value is a promise, and for a
generator a generator object. Typed mode has no promise or generator
types: `await` is already typed `any` with a warning
(`FlowChecker-expr.cpp:1840`).

**What type the call should have.** The exact type is a promise of the
annotated type (`Promise<number>`) or a generator object, but typed mode
has neither. The closest correct type is "some object". The two levels
can express different amounts of that:
- **IR:** `object` exists, and it is true: an async function always
  returns a promise (`spawnAsync` reports errors from parameter
  initialization and the body alike, `ESTreeIRGen-func.cpp:402-405`),
  and a generator call that returns at all returns a generator
  object. So IRGen types the call `object`.
- **Flow:** there is no general object type. The kinds are listed in
  `include/hermes/Sema/FlowContext.h:26-55`; exact objects are fixed
  shapes and classes are nominal with field layouts, so typing a promise as
  either would be false and would license typed property access on it.
  `mixed` is true but unusable (no property access without refinement).
  So the FlowChecker types the call `any`, like `await` today. Adding an
  object kind to Flow, or real promise and generator types, is the
  follow-up below.

**Fix.**
- Add one predicate, on `TypedFunctionType` or in `FlowContext`, for "the
  Flow type of a call to this function": `any` when the function is async
  or a generator, otherwise the return type. Use it at the four
  FlowChecker sites.
- IRGen, in `emitCall`: when the typed callee is async or a generator,
  type the `CallInst` `object` instead of the lowered return type. The
  store of the call into a binding typed `any` needs no cast.
- Keep the annotation as the signature's return type. It types the
  function's `return` statements (`FlowChecker.cpp:1787`), and Flow's
  assignability already requires matching async and generator flags
  (`FlowChecker.cpp:3419`). The IR function type keeps recording it too
  (`lowerTypedFunction`, `ESTreeIRGen-typed-class.cpp:720`).

**Documentation and follow-up issue** (part of this commit):
- Document, in the `TypedFunctionType` comment
  (`include/hermes/Sema/FlowContext.h`), the `Function::functionType_`
  comment (`include/hermes/IR/IR.h`) and the `createFunction` comment
  (`include/hermes/IR/TypeContext.h`): for async and generator signatures
  the return type is the annotation (the promise's value type, or the
  generator's return value), not the type of a call; a call is `any` in
  Flow and `object` in the IR until real types exist. Deep structural
  subtyping (D10) compares these annotations, so the definition matters.
- Add a short note to `phase3-5-design.md` next to D10 and to
  `HANDOFF.md`.
- File a dz issue: support real promise and generator types in typed
  mode, so that a call to `async function f(): T` is `Promise<T>`, `await`
  of it is `T` (today `any`, `FlowChecker-expr.cpp:1840`), and a generator
  call is a generator of its yield, return and next types. Note the
  interim types (`any` in Flow, `object` in the IR) and the places that
  set them, so the follow-up replaces them in one go.

**Consequence.** `const r: number = af()` becomes an `any`-to-`number`
flow. The FlowChecker accepts it with a checked cast, which throws at run
time: correct behaviour for a promise. No test or TypedLib code relies on
typed async call results; the only typed async test,
`IRGen/flow/function-type-async-generator.js`, checks closures, not calls.

**Tests.** New `test/shermes/typed-async-generator-call.js`:
- `typeof af()` prints `object` at `-O0` and `-O`. Fails before at `-O`
  (prints `number`).
- A generator call at `-O` with `-verify-ir -exec` prints `object` and the
  first yielded value. Fails before: verification fails.
- `const r: number = af()` in a `try` prints the caught `TypeError`. It
  documents the checked cast.
- An IRGen check that the `CallInst` of `af()` is `(:object)`. Fails
  before (`(:number)`).

Mutations:
- revert only the IRGen part: the `-O` `typeof` run prints `number`
  again;
- revert only the FlowChecker part: `const r = af()` is typed `number`,
  so the store gets a checked cast and the `typeof` run throws "Checked
  cast failed".

Each part has a failing observable.

## Bug 3: `new Array<T>()`

**Symptom.**

```js
const c: number[] = new Array<number>();
c.push(1);   // aborts: cast<FastArray> fails
```

**Cause.** The typed `new` path in `genNewExpr`
(`lib/IRGen/ESTreeIRGen-expr.cpp:3393-3446`) always calls
`emitTypedClassAllocation`
(`ESTreeIRGen-typed-class.cpp:474`). That emits `AllocTypedObjectInst`, a
plain typed object with `Array.prototype` as its parent, while the
FlowChecker types it `Array<number>`, and the fast-array builtins cast it
to `FastArray`. On top of the IR type stack, the allocation is also
trusted-narrowed to `array(number)`, which is false.

`Array<T>` has no explicit constructor, so the FlowChecker already
rejects arguments (`new Array<number>(3)`: "ft: class Array does not have
an explicit constructor"). Only `new Array<T>()` reaches IRGen.

**Subclasses (decided: allocate fast arrays, reject stored fields).**
`class Foo extends Array<number> {}` compiles, and `new Foo()` takes the
same path, producing a typed object again.
`test/IRGen/flow/class-super-computed-error.js` relies on such a subclass
compiling. Decision (owner, 2026-11-11): a class that is the
`@Hermes.array` class or derives from it is allocated as a fast array, and
the FlowChecker rejects such a class if it has stored instance fields,
which a `FastArray` cannot hold. Methods live on the prototype (the home
object), not on the instance, so field-less subclasses with methods keep
working.

**Fix.**
- `FlowContext`: add `derivesFromArrayClass(const ClassType *)`, true when
  the class or an ancestor (via `getSuperClassInfo()`) satisfies
  `isArrayClassType`.
- FlowChecker: where a class's layout is computed (`FlowChecker.cpp:437-451`
  assigns layout slots after the superclass's), report an error when the
  class derives from the array class and has stored instance fields
  (`getNumLayoutSlots() > 0`; `Array` itself has none). Because the
  check is on the class's own layout count, which includes inherited
  slots, a subclass of a rejected subclass is rejected too.
- IRGen, typed `new` path (`genNewExpr`): when
  `derivesFromArrayClass(classType)` holds, emit
  `AllocFastArrayInst(0, proto)` instead of `emitTypedClassAllocation`,
  where `proto` is the class's home object as today, narrowed to
  `flowTypeToIRType(classType)`: `array(T)` for `Array<T>` itself,
  `classInstance(Foo)` for a subclass. This mirrors
  `genFastArrayFromElements` (`ESTreeIRGen-expr.cpp:472-490`).
- The constructor call stays as it is (the condition at `:3431`): false
  for `Array<T>` itself (no constructor type, superclass or field
  initializers; assert it), true for a subclass, whose implicit or
  explicit constructor runs with the fast array as `this`. Check that
  the `super()` forwarding to `Array`'s implicit constructor
  (`ESTreeIRGen-typed-class.cpp:420`) touches no layout slot.

**IR typing note.** A subclass's IR superclass is `array(number)` (the
Class arm lowers `Array<T>` to `array(T)`), and `TypeContext::isSubclass`
stops at a non-class entry (`TypeContext.cpp:565-577`), so
`classInstance(Foo)` is not a subset of `array(number)` in the IR.
Assigning a `Foo` to a `number[]` binding gets the trusted narrow from
`narrowToFlowType` (checked: `let a: number[] = f` verifies and runs at
`-O0` and `-O`). That is sound; making the class chain reach the array
type would be a precision follow-up, not part of this fix.

**Tests.**
- New `test/shermes/typed-new-array.js`, at `-O0` and `-O` with
  `-verify-ir -exec`:
  - `new Array<number>()`, then `push`, `length` and indexing;
  - a field-less subclass with a method: `new Foo()`, call the method,
    assign it to `number[]`, `push` and read `length` through that.

  Fails before: both abort in `cast<FastArray>`. Mutation: restore
  `emitTypedClassAllocation` for these classes; they abort again.
- `test/IRGen/flow/array-class-allocation.js` (from the IR type stack):
  expect `AllocFastArrayInst` narrowed once to `array(number)`, and add a
  subclass allocation narrowed to `classInstance(Foo)`.
- New Sema error test: a subclass of `Array<number>` with a stored field,
  and a field-less subclass of that subclass, each report the error; a
  field-less subclass with methods does not. Mutation: remove the check;
  the errors disappear.
- `test/IRGen/flow/class-super-computed-error.js` is unchanged (its
  subclass has no fields).

## Docs

Close the three dz issues in their fixing commits, and file the new
promise/generator-types issue with bug 1's fix. Remove them from the
"Bugs found while testing" list in `HANDOFF.md`, and add one line each
under the publication-review section saying where they were fixed.
