# IR Type System Phase 4 (Array + Function) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the IR `Array` and `Function` types real and flow them live:
lower Flow `ArrayType`, `TypedFunctionType`, and `ClassConstructor` to
structurally-interned `Array`/`Function` IR types, handle recursive (looping)
types via reserve/fill, and ensure the verifier/printer/codegen treat the new
kinds correctly.

**Architecture:** Add interned `Array` and `Function` factories (each with a
per-kind intern table mirroring `UnionInternKey`, plus a reserve/fill pair for
recursive types) and a `paramArrays_` side array to `TypeContext`. The existing
id-aware leaf algebra already treats distinct structural refined kinds as
"subtype only of `Object` / equal-id"; verify and test it for the new kinds.
Wire `flowTypeToIRType`: the non-looping common path lowers children first and
calls the interning factory (zero wasted entries); the looping path
(routed on the Flow `Type` wrapper's `isLooping` flag) generalizes the proven
Phase 3 reserve→record-in-identity-cache→recurse→fill recursion. Audit
consumers under the invariant "codegen treats refined kinds exactly like
`Object`."

**Tech Stack:** C++17, LLVM/llvh ADTs, CMake + Ninja (ASan build), gtest
(`HermesIRTests`), lit (`check-hermes`), Sapling (`sl`), `arc f`.

See [phase3-5-design.md](phase3-5-design.md) for the full design and the
decisions (D1–D6) referenced below. This plan mirrors the structure of
[phase3-plan.md](phase3-plan.md).

## Global Constraints

- **C++17**, no exceptions/RTTI; 80-col lines; 2-space indent; doc comment on
  every declaration.
- **D2 soundness invariant:** codegen and the verifier treat every refined kind
  (`Array`, `Function`, and the existing `ClassInstance`) exactly like `Object`.
  The audit rule: any site switching on *exact* `Object`
  (`isObjectType()`, `id == kObjectId`, `TypeKind::Object`) must also accept the
  new refined kinds.
- **D4 shallow subtyping:** `Array`/`Function <: Object`; within-kind is
  invariant / interned-id equality (no variance yet).
- **D5 conservative disjointness:** refined-vs-primitive disjoint; two distinct
  structural refined types are NOT disjoint.
- Refined kinds are `kNotMaskable` (default) → always take the general algebra
  path; no primMask changes.
- Commit messages: Meta format (`facebook/CLAUDE-meta.md`), subject ≤50 chars,
  body wrapped at 72 columns.

---

## Conventions for every task

- **Build dir:** `cmake-build-asan` (already configured; ASan+Debug, `-O1`).
- **Build unit tests:** `cmake --build cmake-build-asan --target HermesIRTests`
- **Run unit tests:**
  `cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.*'`
- **Build the compiler:** `cmake --build cmake-build-asan --target shermes`
  (the typed driver used by lit) and `--target hermes`.
- **Lit (this sandbox needs single-process):**
  `LIT_OPTS="-j1" LIT_FILTER="<regex>" cmake --build cmake-build-asan --target check-hermes`
- **Regenerate auto-updating baselines** (only after confirming diffs are
  cosmetic): `cmake --build cmake-build-asan --target update-lit`.
- **Format:** run `arc f` on every touched `.cpp`/`.h` before committing.
- **Commit:** `sl commit <paths> --reason "<intent> - sl help commit" -m "<msg>"`.
  Commit only the listed paths, never `-A` without paths (the tree has many
  unrelated untracked files). NEVER `sl amend`/`fold`/`metaedit`/`rebase`/
  `jf submit` unprompted.

---

## Task 1: `Array` factory, interning, reserve/fill, and accessor

**Files:**
- Modify: `include/hermes/IR/TypeContext.h` (declare factories/accessor +
  `arrayInternTable_`)
- Modify: `lib/IR/TypeContext.cpp` (implement)
- Test: `unittests/IR/TypeContextTest.cpp`

**Interfaces:**
- Consumes: existing `TypeEntry.array { uint32_t elemTypeId; }`, `Type`,
  `entries_`, `getKind`.
- Produces: `Type createArray(Type elem)`; `Type reserveArray()`;
  `void fillArray(Type t, Type elem)`; `Type getArrayElementType(Type t) const`.

- [ ] **Step 1: Write the failing test**

Add to `unittests/IR/TypeContextTest.cpp` (inside the anonymous namespace, after
the existing `ClassInstance*` tests):

```cpp
TEST_F(TypeContextTest, ArrayConstructionAndInterning) {
  TypeContext ctx;

  // Array<number> and Array<string>.
  Type arrNum = ctx.createArray(Type::createNumber());
  Type arrStr = ctx.createArray(Type::createString());

  EXPECT_EQ(ctx.getKind(arrNum), TypeKind::Array);
  EXPECT_EQ(ctx.getArrayElementType(arrNum), Type::createNumber());
  EXPECT_EQ(ctx.getArrayElementType(arrStr), Type::createString());

  // Interned: same element type => same id.
  Type arrNum2 = ctx.createArray(Type::createNumber());
  EXPECT_EQ(arrNum, arrNum2);
  // Different element type => different id.
  EXPECT_NE(arrNum, arrStr);
}

TEST_F(TypeContextTest, ArrayReserveFillSelfReference) {
  TypeContext ctx;
  // Simulate a recursive array whose element is the array itself:
  //   type A = Array<A>
  Type a = ctx.reserveArray();
  ctx.fillArray(a, a);
  EXPECT_EQ(ctx.getKind(a), TypeKind::Array);
  EXPECT_EQ(ctx.getArrayElementType(a), a);
}
```

- [ ] **Step 2: Run the test, verify it fails to compile**

Run: `cmake --build cmake-build-asan --target HermesIRTests`
Expected: FAIL — `createArray`/`getArrayElementType`/`reserveArray`/`fillArray`
are not members of `TypeContext`.

- [ ] **Step 3: Declare the factories, accessor, and intern table**

In `include/hermes/IR/TypeContext.h`, in the `public:` section of `TypeContext`
(after the `ClassInstance` accessors), add:

```cpp
  /// Create an Array type with element type \p elem. Interned: two calls with
  /// the same element type return the same Type. Allocates an entry only on a
  /// cache miss.
  Type createArray(Type elem);

  /// Reserve an empty Array entry and return its Type. Used with fillArray to
  /// break cycles when lowering a recursive array type. Between reserve and
  /// fill the entry must not be inspected by any type operation, and it is NOT
  /// interned (looping types dedup separately).
  Type reserveArray();

  /// Fill a previously reserved Array entry. \p t must come from reserveArray.
  void fillArray(Type t, Type elem);

  /// \return the element type of Array \p t.
  Type getArrayElementType(Type t) const;
```

In the `private:` section, after `internTable_`, add:

```cpp
  /// Intern table mapping an element type id to the canonical Array type id.
  /// Non-looping arrays only; looping arrays are built via reserve/fill.
  llvh::DenseMap<uint32_t, uint32_t> arrayInternTable_;
```

- [ ] **Step 4: Implement in `lib/IR/TypeContext.cpp`**

Add after `TypeContext::getClassFields` (before `isSubclass`):

```cpp
Type TypeContext::reserveArray() {
  uint32_t id = entries_.size();
  TypeEntry e{}; // primMask defaults to kNotMaskable
  e.kind = TypeKind::Array;
  e.array.elemTypeId = kNoTypeId;
  entries_.push_back(e);
  return Type{id};
}

void TypeContext::fillArray(Type t, Type elem) {
  assert(t.id_ < entries_.size() && "Type ID out of range");
  TypeEntry &e = entries_[t.id_];
  assert(e.kind == TypeKind::Array && "not an Array entry");
  e.array.elemTypeId = elem.id_;
}

Type TypeContext::createArray(Type elem) {
  auto it = arrayInternTable_.find(elem.id_);
  if (it != arrayInternTable_.end())
    return Type{it->second};
  Type t = reserveArray();
  fillArray(t, elem);
  arrayInternTable_[elem.id_] = t.id_;
  return t;
}

Type TypeContext::getArrayElementType(Type t) const {
  assert(getKind(t) == TypeKind::Array && "not an Array");
  return Type{entries_[t.id_].array.elemTypeId};
}
```

- [ ] **Step 5: Run the tests, verify they pass**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.Array*'`
Expected: PASS (2 tests).

- [ ] **Step 6: Format and commit**

```bash
arc f
sl commit include/hermes/IR/TypeContext.h lib/IR/TypeContext.cpp \
  unittests/IR/TypeContextTest.cpp \
  --reason "add Array factory to TypeContext - sl help commit" \
  -m "[SH]: Add interned Array factory to TypeContext

Summary:
Add an interned Array type to TypeContext: createArray (interned by
element type id via arrayInternTable_), a reserveArray/fillArray pair for
recursive arrays, and the element-type accessor. No type-algebra or
lowering changes yet.

Test Plan: HermesIRTests --gtest_filter='TypeContextTest.Array*'
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 2: `Function` factory, `paramArrays_`, interning, reserve/fill, accessors

**Files:**
- Modify: `include/hermes/IR/TypeContext.h` (declare factories/accessors +
  `paramArrays_` + `FunctionInternKey`/`FunctionInternKeyInfo` +
  `functionInternTable_`)
- Modify: `lib/IR/TypeContext.cpp` (implement)
- Test: `unittests/IR/TypeContextTest.cpp`

**Interfaces:**
- Consumes: existing `TypeEntry.function` payload; existing
  `struct FunctionParam { uint32_t typeId; bool isOptional; };`.
- Produces: `Type createFunction(Type ret, Type thisTy, Type restTy,
  llvh::ArrayRef<FunctionParam> params, bool isAsync, bool isGenerator)`;
  `Type reserveFunction()`; `void fillFunction(Type t, Type ret, Type thisTy,
  Type restTy, llvh::ArrayRef<FunctionParam> params, bool isAsync, bool
  isGenerator)`; accessors `getFunctionReturn/This/Rest(Type) const`,
  `llvh::ArrayRef<FunctionParam> getFunctionParams(Type) const`,
  `bool getFunctionIsAsync/IsGenerator(Type) const`.

- [ ] **Step 1: Write the failing test**

Add to `unittests/IR/TypeContextTest.cpp` after the Array tests:

```cpp
TEST_F(TypeContextTest, FunctionConstructionAndInterning) {
  TypeContext ctx;

  FunctionParam params[] = {
      {Type::createNumber().getRawIdForTest(), /*isOptional*/ false},
      {Type::createString().getRawIdForTest(), /*isOptional*/ true}};

  // (a: number, b?: string) => boolean, this: object, no rest.
  Type fn = ctx.createFunction(
      Type::createBoolean(),
      Type::createObject(),
      Type::createNoType(),
      params,
      /*isAsync*/ false,
      /*isGenerator*/ false);

  EXPECT_EQ(ctx.getKind(fn), TypeKind::Function);
  EXPECT_EQ(ctx.getFunctionReturn(fn), Type::createBoolean());
  EXPECT_EQ(ctx.getFunctionThis(fn), Type::createObject());
  EXPECT_TRUE(ctx.getFunctionRest(fn).isNoType());
  EXPECT_FALSE(ctx.getFunctionIsAsync(fn));
  EXPECT_FALSE(ctx.getFunctionIsGenerator(fn));
  auto gotParams = ctx.getFunctionParams(fn);
  ASSERT_EQ(gotParams.size(), 2u);
  EXPECT_EQ(gotParams[0].typeId, Type::createNumber().getRawIdForTest());
  EXPECT_FALSE(gotParams[0].isOptional);
  EXPECT_EQ(gotParams[1].typeId, Type::createString().getRawIdForTest());
  EXPECT_TRUE(gotParams[1].isOptional);

  // Interned: identical signature => same id.
  Type fn2 = ctx.createFunction(
      Type::createBoolean(), Type::createObject(), Type::createNoType(),
      params, false, false);
  EXPECT_EQ(fn, fn2);

  // Differ by isAsync => different id.
  Type fnAsync = ctx.createFunction(
      Type::createBoolean(), Type::createObject(), Type::createNoType(),
      params, /*isAsync*/ true, false);
  EXPECT_NE(fn, fnAsync);

  // Differ by a param optional flag => different id.
  FunctionParam params2[] = {
      {Type::createNumber().getRawIdForTest(), false},
      {Type::createString().getRawIdForTest(), /*isOptional*/ false}};
  Type fn3 = ctx.createFunction(
      Type::createBoolean(), Type::createObject(), Type::createNoType(),
      params2, false, false);
  EXPECT_NE(fn, fn3);
}

TEST_F(TypeContextTest, FunctionReserveFillSelfReference) {
  TypeContext ctx;
  // type F = (cb: F) => void  — a function whose parameter is itself.
  Type f = ctx.reserveFunction();
  FunctionParam params[] = {{f.getRawIdForTest(), false}};
  ctx.fillFunction(
      f, Type::createUndefined(), Type::createNoType(), Type::createNoType(),
      params, false, false);
  EXPECT_EQ(ctx.getKind(f), TypeKind::Function);
  ASSERT_EQ(ctx.getFunctionParams(f).size(), 1u);
  EXPECT_EQ(ctx.getFunctionParams(f)[0].typeId, f.getRawIdForTest());
}
```

Note: `getRawIdForTest()` is a tiny test-only accessor added in Step 3 so tests
can read a `Type`'s raw id (mirrors how the fixture is already a `friend`).

- [ ] **Step 2: Run the test, verify it fails to compile**

Run: `cmake --build cmake-build-asan --target HermesIRTests`
Expected: FAIL — `createFunction`/`fillFunction`/`reserveFunction`/the function
accessors/`getRawIdForTest` are undefined.

- [ ] **Step 3: Declare the test id accessor, side array, intern key, and API**

In `include/hermes/IR/TypeContext.h`:

(a) In `class Type`, inside the existing `#ifdef UNIT_TEST` region if present, or
add one in the `public:` section:

```cpp
#ifdef UNIT_TEST
  /// Test-only: read the raw type id.
  uint32_t getRawIdForTest() const {
    return id_;
  }
#endif
```

(b) After the `UnionInternKey`/`UnionInternKeyInfo` definitions, add a function
intern key (mirrors `UnionInternKey`):

```cpp
/// Hashable key for interning function types. Captures the whole shallow
/// signature: return/this/rest ids, the ordered params (id + optional), and
/// the async/generator flags.
struct FunctionInternKey {
  uint32_t returnTypeId;
  uint32_t thisTypeId;
  uint32_t restTypeId;
  bool isAsync;
  bool isGenerator;
  llvh::SmallVector<FunctionParam, 4> params;
};

/// DenseMap info for FunctionInternKey.
struct FunctionInternKeyInfo {
  static FunctionInternKey getEmptyKey() {
    FunctionInternKey k;
    k.returnTypeId = UINT32_MAX;
    return k;
  }
  static FunctionInternKey getTombstoneKey() {
    FunctionInternKey k;
    k.returnTypeId = UINT32_MAX - 1;
    return k;
  }
  static unsigned getHashValue(const FunctionInternKey &k) {
    llvh::hash_code h = llvh::hash_combine(
        k.returnTypeId, k.thisTypeId, k.restTypeId, k.isAsync, k.isGenerator);
    for (const FunctionParam &p : k.params)
      h = llvh::hash_combine(h, p.typeId, p.isOptional);
    return (unsigned)h;
  }
  static bool isEqual(
      const FunctionInternKey &a,
      const FunctionInternKey &b) {
    if (a.returnTypeId != b.returnTypeId || a.thisTypeId != b.thisTypeId ||
        a.restTypeId != b.restTypeId || a.isAsync != b.isAsync ||
        a.isGenerator != b.isGenerator || a.params.size() != b.params.size())
      return false;
    for (size_t i = 0, e = a.params.size(); i < e; ++i) {
      if (a.params[i].typeId != b.params[i].typeId ||
          a.params[i].isOptional != b.params[i].isOptional)
        return false;
    }
    return true;
  }
};
```

Note: `FunctionParam` is declared above `class Type` already; `FunctionInternKey`
must appear after it. If ordering requires, move the `FunctionParam` struct
above the intern-key definitions (it has no dependency on `Type`).

(c) In `TypeContext`'s `public:` section add:

```cpp
  /// Create a Function type. Interned: two calls with an identical signature
  /// return the same Type. Allocates an entry only on a cache miss. \p thisTy
  /// and \p restTy may be NoType when absent.
  Type createFunction(
      Type ret,
      Type thisTy,
      Type restTy,
      llvh::ArrayRef<FunctionParam> params,
      bool isAsync,
      bool isGenerator);

  /// Reserve an empty Function entry. Used with fillFunction to break cycles
  /// when lowering a recursive function type. NOT interned.
  Type reserveFunction();

  /// Fill a previously reserved Function entry. \p t must come from
  /// reserveFunction.
  void fillFunction(
      Type t,
      Type ret,
      Type thisTy,
      Type restTy,
      llvh::ArrayRef<FunctionParam> params,
      bool isAsync,
      bool isGenerator);

  /// \return the return type of Function \p t.
  Type getFunctionReturn(Type t) const;
  /// \return the "this" param type of Function \p t (NoType if absent).
  Type getFunctionThis(Type t) const;
  /// \return the rest param type of Function \p t (NoType if absent).
  Type getFunctionRest(Type t) const;
  /// \return the params of Function \p t, in order.
  llvh::ArrayRef<FunctionParam> getFunctionParams(Type t) const;
  /// \return whether Function \p t is async.
  bool getFunctionIsAsync(Type t) const;
  /// \return whether Function \p t is a generator.
  bool getFunctionIsGenerator(Type t) const;
```

(d) In `TypeContext`'s `private:` section add, next to `fieldArrays_`:

```cpp
  /// Side array storing function parameters, referenced by
  /// (paramOffset, paramCount) in Function entries.
  std::vector<FunctionParam> paramArrays_;

  /// Intern table mapping a function signature to the canonical Function id.
  /// Non-looping functions only; looping functions are built via reserve/fill.
  llvh::DenseMap<FunctionInternKey, uint32_t, FunctionInternKeyInfo>
      functionInternTable_;
```

- [ ] **Step 4: Implement in `lib/IR/TypeContext.cpp`**

Add after the Array implementation:

```cpp
Type TypeContext::reserveFunction() {
  uint32_t id = entries_.size();
  TypeEntry e{}; // primMask defaults to kNotMaskable
  e.kind = TypeKind::Function;
  e.function.returnTypeId = kNoTypeId;
  e.function.thisTypeId = kNoTypeId;
  e.function.restTypeId = kNoTypeId;
  e.function.paramOffset = 0;
  e.function.paramCount = 0;
  e.function.isAsync = false;
  e.function.isGenerator = false;
  entries_.push_back(e);
  return Type{id};
}

void TypeContext::fillFunction(
    Type t,
    Type ret,
    Type thisTy,
    Type restTy,
    llvh::ArrayRef<FunctionParam> params,
    bool isAsync,
    bool isGenerator) {
  assert(t.id_ < entries_.size() && "Type ID out of range");
  TypeEntry &e = entries_[t.id_];
  assert(e.kind == TypeKind::Function && "not a Function entry");
  e.function.returnTypeId = ret.id_;
  e.function.thisTypeId = thisTy.id_;
  e.function.restTypeId = restTy.id_;
  e.function.paramOffset = safePossiblyNarrowingCast<uint32_t>(
      paramArrays_.size(), "param array offset overflow");
  e.function.paramCount =
      safePossiblyNarrowingCast<uint16_t>(params.size(), "too many params");
  e.function.isAsync = isAsync;
  e.function.isGenerator = isGenerator;
  paramArrays_.insert(paramArrays_.end(), params.begin(), params.end());
}

Type TypeContext::createFunction(
    Type ret,
    Type thisTy,
    Type restTy,
    llvh::ArrayRef<FunctionParam> params,
    bool isAsync,
    bool isGenerator) {
  FunctionInternKey key;
  key.returnTypeId = ret.id_;
  key.thisTypeId = thisTy.id_;
  key.restTypeId = restTy.id_;
  key.isAsync = isAsync;
  key.isGenerator = isGenerator;
  key.params.assign(params.begin(), params.end());

  auto it = functionInternTable_.find(key);
  if (it != functionInternTable_.end())
    return Type{it->second};

  Type t = reserveFunction();
  fillFunction(t, ret, thisTy, restTy, params, isAsync, isGenerator);
  functionInternTable_[std::move(key)] = t.id_;
  return t;
}

Type TypeContext::getFunctionReturn(Type t) const {
  assert(getKind(t) == TypeKind::Function && "not a Function");
  return Type{entries_[t.id_].function.returnTypeId};
}

Type TypeContext::getFunctionThis(Type t) const {
  assert(getKind(t) == TypeKind::Function && "not a Function");
  return Type{entries_[t.id_].function.thisTypeId};
}

Type TypeContext::getFunctionRest(Type t) const {
  assert(getKind(t) == TypeKind::Function && "not a Function");
  return Type{entries_[t.id_].function.restTypeId};
}

llvh::ArrayRef<FunctionParam> TypeContext::getFunctionParams(Type t) const {
  assert(getKind(t) == TypeKind::Function && "not a Function");
  const auto &fn = entries_[t.id_].function;
  return llvh::ArrayRef<FunctionParam>(
      paramArrays_.data() + fn.paramOffset, fn.paramCount);
}

bool TypeContext::getFunctionIsAsync(Type t) const {
  assert(getKind(t) == TypeKind::Function && "not a Function");
  return entries_[t.id_].function.isAsync;
}

bool TypeContext::getFunctionIsGenerator(Type t) const {
  assert(getKind(t) == TypeKind::Function && "not a Function");
  return entries_[t.id_].function.isGenerator;
}
```

- [ ] **Step 5: Run the tests, verify they pass**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.Function*'`
Expected: PASS (2 tests).

- [ ] **Step 6: Format and commit**

```bash
arc f
sl commit include/hermes/IR/TypeContext.h lib/IR/TypeContext.cpp \
  unittests/IR/TypeContextTest.cpp \
  --reason "add Function factory to TypeContext - sl help commit" \
  -m "[SH]: Add interned Function factory to TypeContext

Summary:
Add an interned Function type to TypeContext: a paramArrays_ side array,
createFunction (interned by full shallow signature via a FunctionInternKey
and functionInternTable_), a reserveFunction/fillFunction pair for
recursive functions, and the signature accessors. Add a test-only
Type::getRawIdForTest accessor. No type-algebra or lowering changes yet.

Test Plan: HermesIRTests --gtest_filter='TypeContextTest.Function*'
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 3: Type algebra for `Array`/`Function` (subtype, disjoint, intersect, union)

The id-aware leaf algebra (`isLeafSubtypeById`, `areLeafTypesDisjointById`,
`intersectLeafTy`, the union subsumption in `createUnionImpl` /
`createUnionFromLeafArms`) already routes all refined kinds through the "refined"
branch: every refinement `<: Object`; two distinct structural refined types are
non-subtypes and non-disjoint (D4/D5). Since interning gives structurally
identical arrays/functions the *same id*, id-equality *is* structural equality —
so the existing code already yields the correct answers for the new kinds. This
task **verifies** that with tests and fixes any gap found (expected: none, or a
small clarifying comment).

**Files:**
- Test: `unittests/IR/TypeContextTest.cpp`
- Modify (only if a test fails): `lib/IR/TypeContext.cpp`

**Interfaces:**
- Consumes: `createArray`, `createFunction` (Tasks 1–2), `isSubsetOf`,
  `areDisjoint`, `intersectTy`, `unionTy`.

- [ ] **Step 1: Write the failing/verifying test**

Add to `unittests/IR/TypeContextTest.cpp`:

```cpp
TEST_F(TypeContextTest, ArrayFunctionAlgebra) {
  TypeContext ctx;

  Type arrNum = ctx.createArray(Type::createNumber());
  Type arrStr = ctx.createArray(Type::createString());
  FunctionParam noParams[] = {};
  Type fnVoid = ctx.createFunction(
      Type::createUndefined(), Type::createNoType(), Type::createNoType(),
      noParams, false, false);
  Type obj = Type::createObject();

  // Every refinement is a subtype of Object.
  EXPECT_TRUE(ctx.isSubsetOf(arrNum, obj));
  EXPECT_TRUE(ctx.isSubsetOf(fnVoid, obj));
  EXPECT_FALSE(ctx.isSubsetOf(obj, arrNum));

  // Invariant / id-equality within kind: Array<number> is only a subtype of
  // itself, not of Array<string>.
  EXPECT_TRUE(ctx.isSubsetOf(arrNum, arrNum));
  EXPECT_FALSE(ctx.isSubsetOf(arrNum, arrStr));

  // Conservative disjointness (D5): distinct structural refined types are NOT
  // declared disjoint; refined-vs-primitive IS disjoint.
  EXPECT_FALSE(ctx.areDisjoint(arrNum, arrStr));
  EXPECT_FALSE(ctx.areDisjoint(arrNum, fnVoid));
  EXPECT_TRUE(ctx.areDisjoint(arrNum, Type::createNumber()));

  // intersect: refined ∩ Object = refined; refined ∩ same = same; refined ∩
  // distinct-refined is conservative (returns NoType only when truly disjoint,
  // else one operand — assert it does not crash and stays sound).
  EXPECT_EQ(ctx.intersectTy(arrNum, obj), arrNum);
  EXPECT_EQ(ctx.intersectTy(arrNum, arrNum), arrNum);
  EXPECT_TRUE(ctx.intersectTy(arrNum, Type::createNumber()).isNoType());

  // union: refined | Object collapses to Object (subsumption); refined |
  // distinct-refined keeps both arms.
  EXPECT_EQ(ctx.unionTy(arrNum, obj), obj);
  Type u = ctx.unionTy(arrNum, arrStr);
  EXPECT_EQ(ctx.countKinds(u), 2u);
  EXPECT_TRUE(ctx.isSubsetOf(arrNum, u));
  EXPECT_TRUE(ctx.isSubsetOf(arrStr, u));
}
```

- [ ] **Step 2: Run the test**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.ArrayFunctionAlgebra'`
Expected: PASS with no production change (the existing refined branch handles it).
If any assertion fails, fix the corresponding branch in
`isLeafSubtypeById`/`areLeafTypesDisjointById`/`intersectLeafTy` in
`lib/IR/TypeContext.cpp`, keeping D4/D5 semantics, then re-run.

- [ ] **Step 3: Format and commit**

```bash
arc f
sl commit unittests/IR/TypeContextTest.cpp lib/IR/TypeContext.cpp \
  --reason "test array/function type algebra - sl help commit" \
  -m "[SH]: Test Array/Function type algebra

Summary:
Add algebra coverage for the new Array/Function refined kinds: subtype vs
Object, invariant within-kind subtyping, conservative disjointness (D5),
intersection, and union subsumption. The existing id-aware leaf algebra
already handles these; this locks the behavior in with tests.

Test Plan: HermesIRTests --gtest_filter='TypeContextTest.ArrayFunctionAlgebra'
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 4: Pretty-print `Array` and `Function`

**Files:**
- Modify: `lib/IR/TypeContext.cpp` (`printLeaf`)
- Test: `unittests/IR/TypeContextTest.cpp`

**Interfaces:**
- Consumes: `printLeaf(OS, id)`, `print(OS, t)`.

- [ ] **Step 1: Write the failing test**

Add to `unittests/IR/TypeContextTest.cpp` a helper-based test (mirror the
existing `ClassInstancePrint` style — build a `std::string`/`raw_string_ostream`
and compare):

```cpp
TEST_F(TypeContextTest, ArrayFunctionPrint) {
  TypeContext ctx;

  Type arrNum = ctx.createArray(Type::createNumber());
  FunctionParam params[] = {
      {Type::createNumber().getRawIdForTest(), false}};
  Type fn = ctx.createFunction(
      Type::createString(), Type::createNoType(), Type::createNoType(),
      params, false, false);

  auto str = [&](Type t) {
    std::string s;
    llvh::raw_string_ostream os{s};
    ctx.print(os, t);
    return os.str();
  };

  EXPECT_EQ(str(arrNum), "array(number)");
  EXPECT_EQ(str(fn), "function(string)");
}
```

- [ ] **Step 2: Run the test, verify it fails**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.ArrayFunctionPrint'`
Expected: FAIL — `printLeaf` currently prints the bare kind name (`array` /
`function`) for these kinds.

- [ ] **Step 3: Extend `printLeaf`**

In `lib/IR/TypeContext.cpp`, in `TypeContext::printLeaf`, before the final
`OS << kindName(k);`, add:

```cpp
  if (k == TypeKind::Array) {
    OS << "array(";
    printLeaf(OS, entries_[id].array.elemTypeId);
    OS << ')';
    return;
  }
  if (k == TypeKind::Function) {
    OS << "function(";
    printLeaf(OS, entries_[id].function.returnTypeId);
    OS << ')';
    return;
  }
```

Note: `printLeaf` recurses on the element/return id. Those ids may themselves be
unions (e.g. `array(number|string)`); `printLeaf` only handles non-union leaves,
so for a union child it would print the first-arm name only. Keep the spelling
minimal here (element/return printed via `printLeaf`); the full union spelling is
not needed for the diagnostic and matches how `classInstance(Name)` stays
shallow. The exact chosen spelling is confirmed against lit in Task 9 via
`update-lit`.

- [ ] **Step 4: Run the test, verify it passes**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.ArrayFunctionPrint'`
Expected: PASS.

- [ ] **Step 5: Format and commit**

```bash
arc f
sl commit lib/IR/TypeContext.cpp unittests/IR/TypeContextTest.cpp \
  --reason "pretty-print Array/Function types - sl help commit" \
  -m "[SH]: Pretty-print Array and Function types

Summary:
Extend TypeContext::printLeaf to render array(<elem>) and
function(<ret>) for the new refined kinds, mirroring the existing
classInstance(Name) shallow spelling. Exact spelling is confirmed
against lit baselines when lowering is wired.

Test Plan: HermesIRTests --gtest_filter='TypeContextTest.ArrayFunctionPrint'
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 5: Wire `Array` in `flowTypeToIRType` (non-looping path)

Lower non-recursive `flow::ArrayType` to `createArray`. Recursive arrays are
handled in Task 7; here the Array arm assumes the element is non-looping (Flow's
`isLooping` routing is added in Task 7 — until then a recursive array would
infinite-loop, but no such program is lowered by this task's tests).

**Files:**
- Modify: `lib/IRGen/ESTreeIRGen-typed-class.cpp` (`flowTypeToIRType` Array arm)
- Test: `test/IRGen/flow/array-type.js` (new lit test)

**Interfaces:**
- Consumes: `flow::ArrayType::getElement()` (`Type*`); `TypeContext::createArray`.

- [ ] **Step 1: Write the failing lit test**

Create `test/IRGen/flow/array-type.js`:

```javascript
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

function foo(a: number[]): number {
  return a[0];
}

// Auto-generated content below. Please do not modify manually.
```

- [ ] **Step 2: Run the test, verify it fails/regens with `object`**

Run: `LIT_OPTS="-j1" LIT_FILTER="array-type" cmake --build cmake-build-asan --target check-hermes`
Expected: FAIL (empty baseline) — before regen the param `a` still dumps as
`object`, not `array(number)`.

- [ ] **Step 3: Wire the Array arm**

In `lib/IRGen/ESTreeIRGen-typed-class.cpp`, in
`ESTreeIRGen::flowTypeToIRType`, replace the `Array` arm of the
`case flow::TypeKind::Array: case flow::TypeKind::Tuple: case
flow::TypeKind::ExactObject:` group. Split `Array` out:

```cpp
    case flow::TypeKind::Array: {
      auto *arrayType = llvh::cast<flow::ArrayType>(flowType);
      Type elem = flowTypeToIRType(arrayType->getElement());
      return getTypeContext().createArray(elem);
    }
    case flow::TypeKind::Tuple:
    case flow::TypeKind::ExactObject:
      return Type::createObject();
```

(`flowTypeToIRType(flow::Type*)` forwards to the `TypeInfo*` overload, per the
existing overload set; `getElement()` returns a `flow::Type*`.)

- [ ] **Step 4: Regenerate and inspect the baseline**

Run: `cmake --build cmake-build-asan --target update-lit`
Then inspect: `sl diff test/IRGen/flow/array-type.js`
Expected: the param type for `a` now dumps as `array(number)` (the exact
spelling from Task 4). Confirm the change is only `object → array(number)`.

- [ ] **Step 5: Run the test, verify it passes**

Run: `LIT_OPTS="-j1" LIT_FILTER="array-type" cmake --build cmake-build-asan --target check-hermes`
Expected: PASS.

- [ ] **Step 6: Format and commit**

```bash
arc f
sl commit lib/IRGen/ESTreeIRGen-typed-class.cpp test/IRGen/flow/array-type.js \
  --reason "lower Flow arrays to IR Array - sl help commit" \
  -m "[SH]: Lower non-recursive Flow arrays to IR Array

Summary:
Lower flow::ArrayType to an interned IR Array carrying its element type,
replacing the Array->Object collapse. Recursive arrays are handled in a
later change; this covers the non-looping common case. Tuple/ExactObject
still lower to Object.

Test Plan: LIT_FILTER=array-type check-hermes
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 6: Wire `TypedFunction` and `ClassConstructor` in `flowTypeToIRType`

Lower non-recursive `flow::TypedFunctionType` and `flow::ClassConstructor` to
`createFunction`. `UntypedFunction`/`NativeFunction`/`CPtr` stay as-is.

**Files:**
- Modify: `lib/IRGen/ESTreeIRGen-typed-class.cpp` (`flowTypeToIRType`)
- Test: `test/IRGen/flow/function-type.js` (new lit test)

**Interfaces:**
- Consumes: `flow::TypedFunctionType` (`getReturnType`, `getThisParam`,
  `getParams` → `Param{name, type, optional, rest}`, `isAsync`, `isGenerator`);
  `flow::ClassType::getConstructorTypeInfo()` (`TypedFunctionType*`, nullable);
  `TypeContext::createFunction`.
- Produces: a private helper `Type lowerTypedFunction(flow::TypedFunctionType
  *ft)` on `ESTreeIRGen`, reused by the `TypedFunction` and `ClassConstructor`
  arms.

- [ ] **Step 1: Write the failing lit test**

Create `test/IRGen/flow/function-type.js`:

```javascript
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

function takesFn(cb: (x: number) => string): void {
  cb(1);
}

// Auto-generated content below. Please do not modify manually.
```

- [ ] **Step 2: Run the test, verify it fails/regens with `object`**

Run: `LIT_OPTS="-j1" LIT_FILTER="function-type" cmake --build cmake-build-asan --target check-hermes`
Expected: FAIL (empty baseline) — `cb` still dumps as `object`.

- [ ] **Step 3: Add the helper and declare it**

In `include/hermes/IRGen/ESTreeIRGen.h` (near `flowTypeToIRType`'s declaration),
add:

```cpp
  /// Lower a Flow typed-function signature to an interned IR Function type.
  /// Used for both TypedFunction values and class constructors.
  Type lowerTypedFunction(flow::TypedFunctionType *ft);
```

In `lib/IRGen/ESTreeIRGen-typed-class.cpp`, add the definition (above
`flowTypeToIRType`):

```cpp
Type ESTreeIRGen::lowerTypedFunction(flow::TypedFunctionType *ft) {
  TypeContext &tc = getTypeContext();
  Type ret = flowTypeToIRType(ft->getReturnType());
  Type thisTy = ft->getThisParam() ? flowTypeToIRType(ft->getThisParam())
                                   : Type::createNoType();
  Type restTy = Type::createNoType();
  llvh::SmallVector<FunctionParam, 4> params;
  for (const flow::TypedFunctionType::Param &p : ft->getParams()) {
    Type pty = flowTypeToIRType(p.type);
    if (p.rest) {
      // The rest parameter is the trailing element type; record it separately.
      restTy = pty;
      continue;
    }
    params.push_back(FunctionParam{pty.getRawId(), p.optional});
  }
  return tc.createFunction(
      ret, thisTy, restTy, params, ft->isAsync(), ft->isGenerator());
}
```

Note: `FunctionParam.typeId` is a raw `uint32_t`. Add a non-test raw-id getter
to `Type` used by lowering:

```cpp
  /// \return the raw type id. Used by lowering to populate side-array entries
  /// that store raw ids (e.g. FunctionParam).
  uint32_t getRawId() const {
    return id_;
  }
```

Place it in `class Type`'s `public:` section (not under `UNIT_TEST`).

- [ ] **Step 4: Wire the two arms**

In `flowTypeToIRType`, replace:

```cpp
    case flow::TypeKind::TypedFunction:
    case flow::TypeKind::UntypedFunction:
      return Type::createObject();
```

with:

```cpp
    case flow::TypeKind::TypedFunction:
      return lowerTypedFunction(llvh::cast<flow::TypedFunctionType>(flowType));
    case flow::TypeKind::UntypedFunction:
      return Type::createObject();
```

and replace:

```cpp
    case flow::TypeKind::ClassConstructor:
      return Type::createObject();
```

with:

```cpp
    case flow::TypeKind::ClassConstructor: {
      auto *cc = llvh::cast<flow::ClassConstructorType>(flowType);
      flow::TypedFunctionType *ctorFt =
          cc->getClassTypeInfo()->getConstructorTypeInfo();
      // A class with no explicit constructor has no constructor signature;
      // fall back to Object (matches the pre-refinement behavior).
      if (!ctorFt)
        return Type::createObject();
      return lowerTypedFunction(ctorFt);
    }
```

Confirm `flow::ClassConstructorType::getClassTypeInfo()` exists (it is used in
`genClassDeclaration` at the top of this file). Verify `getConstructorTypeInfo()`
may return null and the fallback is correct.

- [ ] **Step 5: Regenerate and inspect the baseline**

Run: `cmake --build cmake-build-asan --target update-lit`
Then: `sl diff test/IRGen/flow/function-type.js`
Expected: `cb` now dumps as `function(string)` (return-type spelling from
Task 4). Confirm the diff is only `object → function(...)`.

- [ ] **Step 6: Run the test, verify it passes**

Run: `LIT_OPTS="-j1" LIT_FILTER="function-type" cmake --build cmake-build-asan --target check-hermes`
Expected: PASS.

- [ ] **Step 7: Format and commit**

```bash
arc f
sl commit include/hermes/IRGen/ESTreeIRGen.h \
  lib/IRGen/ESTreeIRGen-typed-class.cpp test/IRGen/flow/function-type.js \
  --reason "lower Flow functions/constructors to IR Function - sl help commit" \
  -m "[SH]: Lower Flow functions and constructors to IR Function

Summary:
Lower flow::TypedFunctionType and flow::ClassConstructor to an interned
IR Function via a shared lowerTypedFunction helper, replacing the
->Object collapse. Rest params are recorded as the Function's restTy;
optional params keep their flag. UntypedFunction/NativeFunction stay as
before. Classes without an explicit constructor fall back to Object.

Test Plan: LIT_FILTER=function-type check-hermes
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 7: Looping path — recursive types via `isLooping` routing

Handle recursive structural types (`type Json = number | string | Array<Json>`)
by generalizing the Phase 3 class reserve/fill recursion to Array and Function,
routed on the Flow `Type` wrapper's `isLooping` flag. **Write the tests first**
against real recursive-type programs — this is the riskiest component.

**Files:**
- Modify: `include/hermes/IRGen/ESTreeIRGen.h` (identity cache generalization)
- Modify: `lib/IRGen/ESTreeIRGen-typed-class.cpp` (routing + reserve/fill arms)
- Test: `test/IRGen/flow/recursive-type.js` (new lit test)

**Interfaces:**
- Consumes: `flow::Type::isLooping` (on the wrapper); the `flowTypeToIRType`
  overload taking `flow::Type*`; `reserveArray`/`fillArray`,
  `reserveFunction`/`fillFunction`; `flow::TypeInfo::equals` (cycle-safe).
- Produces: an identity cache `loweredLoopingTypes_` keyed by `const
  flow::TypeInfo *` mapping to the reserved/finished `Type`.

- [ ] **Step 1: Write the failing lit test**

Create `test/IRGen/flow/recursive-type.js`:

```javascript
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

type Json = number | string | Json[];

function useJson(j: Json): Json {
  return j;
}

// Auto-generated content below. Please do not modify manually.
```

- [ ] **Step 2: Run the test, verify it currently hangs or fails**

Run (with a timeout guard — a recursive array would infinite-loop before the
routing exists):
`timeout 60 bash -c 'LIT_OPTS="-j1" LIT_FILTER="recursive-type" cmake --build cmake-build-asan --target check-hermes'`
Expected: FAIL/timeout — the non-looping Array arm from Task 5 recurses forever
on `Json[]` because `Json` contains the array.

- [ ] **Step 3: Generalize the identity cache**

In `include/hermes/IRGen/ESTreeIRGen.h`, next to `loweredClassTypes_`, add:

```cpp
  /// Identity cache for looping (recursive) structural Flow types, keyed by the
  /// TypeInfo pointer. Populated at reserve time (before children are lowered)
  /// so a recursive reference back to the same type resolves to the reserved
  /// Type instead of recursing forever. Mirrors loweredClassTypes_ but for
  /// Array/Function/(later Tuple/ExactObject).
  llvh::DenseMap<const flow::TypeInfo *, Type> loweredLoopingTypes_;
```

- [ ] **Step 4: Add the `flow::Type*` routing overload**

In `lib/IRGen/ESTreeIRGen-typed-class.cpp`, add a `flow::Type*` overload of
`flowTypeToIRType` that inspects `isLooping` before delegating. If the current
`flow::Type*` overload merely forwards to `->info`, replace it with:

```cpp
Type ESTreeIRGen::flowTypeToIRType(flow::Type *flowType) {
  // Non-looping types take the acyclic path (children are lowered first, then
  // the interning factory is called).
  if (!flowType->isLooping)
    return flowTypeToIRType(flowType->info);

  // Looping type: reserve an entry, record it BEFORE lowering children so a
  // recursive reference resolves to the reserved id, then fill.
  flow::TypeInfo *info = flowType->info;
  auto it = loweredLoopingTypes_.find(info);
  if (it != loweredLoopingTypes_.end())
    return it->second;

  TypeContext &tc = getTypeContext();
  switch (info->getKind()) {
    case flow::TypeKind::Array: {
      auto *arrayType = llvh::cast<flow::ArrayType>(info);
      Type t = tc.reserveArray();
      loweredLoopingTypes_.insert({info, t});
      Type elem = flowTypeToIRType(arrayType->getElement());
      tc.fillArray(t, elem);
      return t;
    }
    case flow::TypeKind::TypedFunction: {
      auto *ft = llvh::cast<flow::TypedFunctionType>(info);
      Type t = tc.reserveFunction();
      loweredLoopingTypes_.insert({info, t});
      Type ret = flowTypeToIRType(ft->getReturnType());
      Type thisTy = ft->getThisParam()
          ? flowTypeToIRType(ft->getThisParam())
          : Type::createNoType();
      Type restTy = Type::createNoType();
      llvh::SmallVector<FunctionParam, 4> params;
      for (const flow::TypedFunctionType::Param &p : ft->getParams()) {
        Type pty = flowTypeToIRType(p.type);
        if (p.rest) {
          restTy = pty;
          continue;
        }
        params.push_back(FunctionParam{pty.getRawId(), p.optional});
      }
      tc.fillFunction(
          t, ret, thisTy, restTy, params, ft->isAsync(), ft->isGenerator());
      return t;
    }
    case flow::TypeKind::Union: {
      // A looping union: build arms via the looping-aware overload so recursive
      // arms resolve to reserved ids. Arms are deduped+sorted by id but
      // subsumption is skipped (arms may be placeholders); safe because Flow
      // already canonicalized the union. Unions are not reserved (they hold no
      // self-slot to fill); the recursion is broken by the reserved arm(s).
      Type res = Type::createNoType();
      for (flow::Type *arm : llvh::cast<flow::UnionType>(info)->getTypes())
        res = tc.unionTy(res, flowTypeToIRType(arm));
      return res;
    }
    default:
      // Tuple/ExactObject looping arrive in Phase 5; other kinds cannot loop.
      return flowTypeToIRType(info);
  }
}
```

Note: the `flow::TypeInfo*` overload (the acyclic path from Tasks 5–6) must call
the `flow::Type*` overload for its *children* (`getElement()`, `p.type`,
`getReturnType()`, union arms), so recursion is always routed through the
`isLooping` check. Audit the `Array`, `TypedFunction`, `Union` arms and
`lowerTypedFunction` added in Tasks 5–6: change child calls that currently pass
`flow::Type*` to go through the `flow::Type*` overload (they already do —
`flowTypeToIRType(p.type)` where `p.type` is `flow::Type*`). Confirm the overload
resolution picks the `flow::Type*` version for `Type*` arguments.

Also confirm the existing `Union` arm of the `flow::TypeInfo*` overload
(non-looping) already iterates `getTypes()` and calls `flowTypeToIRType(elemType)`
where `elemType` is `flow::Type*` — so non-looping unions route correctly with no
change.

- [ ] **Step 5: Regenerate and inspect the baseline; verify termination**

Run: `timeout 120 cmake --build cmake-build-asan --target update-lit`
Then: `sl diff test/IRGen/flow/recursive-type.js`
Expected: lowering **terminates**; `j` dumps with a refined recursive type
(e.g. `any` superset or `number|string|array(...)` — confirm the spelling is
sound and cycle-free, no infinite print). If `printLeaf` recurses infinitely on
the cyclic element, cap it: in Task 4's `array(...)`/`function(...)` printing,
print the element/return via `printLeaf` only for non-recursive ids; for an id
whose entry transitively references itself, print the bare kind name
(`array`/`function`). Add that guard here if the dump loops.

- [ ] **Step 6: Add a unit test for looping reserve/fill dedup (optional structural)**

Add to `unittests/IR/TypeContextTest.cpp` a direct check that two independently
reserve/filled recursive arrays with identical structure are usable (they may be
distinct ids — looping types are not structurally interned by the factory):

```cpp
TEST_F(TypeContextTest, LoopingArrayReserveFillUsable) {
  TypeContext ctx;
  // type A = A[]  built twice by hand.
  Type a1 = ctx.reserveArray();
  ctx.fillArray(a1, a1);
  Type a2 = ctx.reserveArray();
  ctx.fillArray(a2, a2);
  // Both are well-formed self-referential arrays; algebra terminates.
  EXPECT_TRUE(ctx.isSubsetOf(a1, Type::createObject()));
  EXPECT_TRUE(ctx.isSubsetOf(a2, Type::createObject()));
  EXPECT_FALSE(ctx.areDisjoint(a1, a2));
}
```

- [ ] **Step 7: Run all new tests, verify they pass**

Run:
```bash
cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.*Looping*' && \
  LIT_OPTS="-j1" LIT_FILTER="recursive-type" cmake --build cmake-build-asan --target check-hermes
```
Expected: PASS.

- [ ] **Step 8: Format and commit**

```bash
arc f
sl commit include/hermes/IRGen/ESTreeIRGen.h \
  lib/IRGen/ESTreeIRGen-typed-class.cpp lib/IR/TypeContext.cpp \
  unittests/IR/TypeContextTest.cpp test/IRGen/flow/recursive-type.js \
  --reason "lower recursive Flow types via reserve/fill - sl help commit" \
  -m "[SH]: Lower recursive Flow types via reserve/fill

Summary:
Route lowering on the Flow Type wrapper's isLooping flag: non-looping
types take the acyclic interning path, looping types reserve an entry and
record it in a TypeInfo-keyed identity cache before lowering children,
then fill -- generalizing the Phase 3 class reserve/fill to Array and
Function. Cap cyclic pretty-printing to avoid infinite recursion.

Test Plan:
HermesIRTests --gtest_filter='TypeContextTest.*Looping*';
LIT_FILTER=recursive-type check-hermes
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 8: Consumer audit — verifier and codegen treat `Array`/`Function` as `Object`

Under D2, find every site that switches on *exact* `Object` that arrays/functions
now flow into, and relax it to accept refined kinds. The Phase 3 audit already
relaxed the shared object sites (verifier `LoadParent*`, SH `SH.cpp`
object fast paths, InstSimplify, IREval) via `isSubsetOf(t, Object)` /
`isObjectKind` — those already cover Array/Function. This task finds the
**Array/Function-specific** new sites, chiefly `AllocFastArrayInst` and any
function-typed value site.

**Files:**
- Modify (as the audit finds): `lib/IR/IRVerifier.cpp`,
  `lib/IRGen/ESTreeIRGen-expr.cpp` (the `AllocFastArrayInst` prototype wart),
  `lib/BCGen/SH/SH.cpp`, `include/hermes/IR/Instrs.h`.
- Test: existing lit + a targeted regression program.

**Interfaces:**
- Consumes: `TypeContext::isSubsetOf`, the `isObjectKind` helper.

- [ ] **Step 1: Find candidate sites**

Run a search for exact-Object checks that a newly-typed array/function value can
reach:

```bash
```
Use the codebase search tool for `isObjectType`, `kObjectId`, and
`TypeKind::Object` across `lib/IR`, `lib/BCGen`, `lib/Optimizer`, `lib/IRGen`.
For each hit, determine whether an `Array`/`Function`-typed SSA value can flow
into it (e.g. `AllocFastArrayInst` operands, prototype loads, `CheckedTypeCast`
of a function value). List the sites that need relaxing.

- [ ] **Step 2: Write a regression lit test that would miscompile/assert**

Create `test/IRGen/flow/array-function-codegen.js`:

```javascript
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

function mk(): number[] {
  return [1, 2, 3];
}

function apply(cb: (x: number) => number, x: number): number {
  return cb(x);
}

// Auto-generated content below. Please do not modify manually.
```

- [ ] **Step 3: Run it — expect an assert or verifier failure if a site is unrelaxed**

Run: `LIT_OPTS="-j1" LIT_FILTER="array-function-codegen" cmake --build cmake-build-asan --target check-hermes`
Expected: FAIL — either an IRVerifier assert on a refined operand where `object`
is required (e.g. the `AllocFastArrayInst` prototype), or an empty baseline.
If it passes cleanly (all sites already covered by the Phase 3 relaxations),
note that and skip to Step 5.

- [ ] **Step 4: Relax the identified sites**

For the `AllocFastArrayInst` prototype wart (flagged in the handoff): in
`lib/IRGen/ESTreeIRGen-expr.cpp`, the `Array<T>` literal path currently narrows
the prototype **back** to `object` for `AllocFastArrayInst`. If Task 5/7 now
produce an `array(...)`-typed home object, keep the trusted-narrow-to-object
where the instruction's operand type requires it, OR relax the instruction's
operand constraint. Prefer the minimal change: keep the narrow, and only relax
verifier/codegen sites that assert exact `object` on values that are now
`array(...)`/`function(...)`. Concretely, for each site from Step 1 that a
refined value reaches, change:

```cpp
// before
if (t.isObjectType()) { ... }
// after
if (getTypeContext().isSubsetOf(t, Type::createObject())) { ... }
```

and for kind switches, add the refined kinds alongside `TypeKind::Object` (use
the existing `isObjectKind` helper in `lib/IR/TypeContext.cpp` where a
context-free kind test is needed). Make these edits directly (do not delegate
judgment-heavy verifier/codegen edits), following the Phase 3 pattern.

- [ ] **Step 5: Regenerate, inspect, and run**

Run: `cmake --build cmake-build-asan --target update-lit`
Then: `sl diff test/IRGen/flow/array-function-codegen.js` — confirm the IR is
sound (array/function values carry refined types; no lost casts).
Run: `LIT_OPTS="-j1" LIT_FILTER="array-function-codegen" cmake --build cmake-build-asan --target check-hermes`
Expected: PASS.

- [ ] **Step 6: Format and commit**

```bash
arc f
sl commit <the exact files you modified> \
  test/IRGen/flow/array-function-codegen.js \
  --reason "audit consumers for Array/Function refined types - sl help commit" \
  -m "[SH]: Treat Array/Function refined types as Object in consumers

Summary:
Audit verifier and codegen for sites that switch on exact Object which
Array/Function-typed values now reach, and relax them to accept refined
object kinds (isSubsetOf(t, Object) / isObjectKind), per the D2 invariant.
Behavior-preserving for pre-existing non-refined types.

Test Plan: LIT_FILTER=array-function-codegen check-hermes; full lit in a
follow-up.
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 9: Full regression run and broader lit baseline

**Files:**
- Modify (only baselines that change): various `%FileCheckOrRegen` tests.

- [ ] **Step 1: Build everything**

Run: `cmake --build cmake-build-asan --target shermes hermes HermesIRTests`
Expected: clean build.

- [ ] **Step 2: Run the IR + VM unit tests**

Run:
```bash
cmake-build-asan/unittests/IR/HermesIRTests && \
cmake --build cmake-build-asan --target HermesVMRuntimeTests && \
cmake-build-asan/unittests/VMRuntime/HermesVMRuntimeTests
```
Expected: all PASS (IR: 92 + the new TypeContext tests; VMRuntime: 350).

- [ ] **Step 3: Run the full lit suite (single-process)**

Run: `LIT_OPTS="-j1" cmake --build cmake-build-asan --target check-hermes`
Expected: any failures are `%FileCheckOrRegen` baselines whose only diff is
`object → array(...)`/`function(...)` where a typed array/function/constructor
now flows. Inspect a sample of the failures to confirm the diffs are cosmetic
and expected.

- [ ] **Step 4: Regenerate baselines and review**

Run: `cmake --build cmake-build-asan --target update-lit`
Then review: `sl diff --stat` and spot-check several changed baselines. Confirm
every change is the expected `object → refined` (or trusted-narrow/cast
adjustment). If any diff is NOT explained by the refinement, stop and
investigate before committing.

- [ ] **Step 5: Re-run the full lit suite to confirm green**

Run: `LIT_OPTS="-j1" cmake --build cmake-build-asan --target check-hermes`
Expected: 0 unexpected failures.

- [ ] **Step 6: Format and commit the baseline regeneration**

```bash
arc f
sl commit <the exact regenerated baseline files> \
  --reason "regenerate lit baselines for live array/function types - sl help commit" \
  -m "[SH]: Regenerate lit baselines for live Array/Function types

Summary:
Regenerate %FileCheckOrRegen baselines whose IR dumps now show refined
array(...)/function(...) types where typed arrays, function values, and
class constructors previously dumped as object. All diffs are cosmetic
refinements per the D2 invariant.

Test Plan: LIT_OPTS=-j1 check-hermes (0 unexpected failures);
HermesIRTests; HermesVMRuntimeTests
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 10: Update progress tracking

**Files:**
- Modify: `doc/plans/ir-type/progress.md`
- Modify: `doc/plans/ir-type/HANDOFF.md`

- [ ] **Step 1: Update `progress.md`**

Add Phase 4 rows to the status table and a "Phase 4" context note mirroring the
existing "Phase 3" note: what landed (Array + Function factories/interning,
paramArrays_, per-kind intern tables, reserve/fill looping path routed on
isLooping, flowTypeToIRType flips for Array/TypedFunction/ClassConstructor,
consumer audit), the green bar, and the commit stack.

- [ ] **Step 2: Update `HANDOFF.md`**

Move Phase 4 from "What remains" to done; leave Phase 5 (Tuple + ExactObject) as
the remaining work, noting the looping interner now exists and Phase 5 only
teaches the fill step to build Tuple/ExactObject entries.

- [ ] **Step 3: Commit**

```bash
sl commit doc/plans/ir-type/progress.md doc/plans/ir-type/HANDOFF.md \
  --reason "record IR type Phase 4 progress - sl help commit" \
  -m "[SH]: Record IR type Phase 4 progress

Summary:
Update progress.md and HANDOFF.md: Phase 4 (Array + Function + looping
interner) is complete and green; Phase 5 (Tuple + ExactObject) remains
and now only needs the fill step extended for the new kinds.

Test Plan: docs only
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Self-Review (completed during planning)

**Spec coverage** (against `phase3-5-design.md` Phase 4 section):
- Factories/accessors `createArray`/`createFunction` + accessors → Tasks 1, 2. ✓
- Interned by shallow key over child ids, per-kind (chosen) → Tasks 1, 2. ✓
- `flowTypeToIRType` flips (Array, TypedFunction, ClassConstructor; Untyped/
  Native/CPtr unchanged) → Tasks 5, 6. ✓
- Cycle-breaking interner (looping path, isLooping routing, reserve/fill) →
  Task 7. ✓ Dedicated tests before/with wiring → Task 7 Steps 1, 6. ✓
- Type algebra (Array/Function <: Object; invariant/id-equality; conservative
  disjoint) → Task 3. ✓
- Consumer audit → Task 8. ✓
- Printing → Task 4. ✓
- Unit + lit tests; `-j1` lit + unit green; `arc f` → each task + Task 9. ✓
- `progress.md` updated → Task 10. ✓

**Decisions honored:** per-kind intern keys (Tasks 1–2); generalize Phase 3
reserve/fill recursion for looping (Task 7); no separate design doc (this plan is
the increment record). The non-looping path allocates an entry only on a genuine
intern miss (no reserve/discard waste) — Task 1/2 `create*` check the table
first.

**Placeholder scan:** no TBD/TODO; every code step shows code; commands have
expected output. The two judgment-driven tasks (7 looping print-cap, 8 audit
site list) describe the exact rule and the exact edit shape, with the discovery
step explicit — this is inherent to an audit, not a placeholder.

**Type consistency:** `FunctionParam{typeId, isOptional}` used consistently;
`Type::getRawId()` (lowering) vs `getRawIdForTest()` (tests) both defined in
Task 2 / Task 6; `createArray(Type)`, `createFunction(Type,Type,Type,ArrayRef<
FunctionParam>,bool,bool)`, `lowerTypedFunction(TypedFunctionType*)` signatures
match across tasks.

## Known follow-ups (Phase 5, separate plan)

- Tuple + ExactObject factories/accessors/algebra; teach the Task 7 looping fill
  step to build Tuple/ExactObject entries; totality pass across all five refined
  kinds.
- Optional: a structural dedup table keyed by `flow::TypeInfo*` (co-inductive
  `equals`) to collapse two independently-declared identical *recursive* types to
  one id (currently they get distinct ids; sound, just not maximally shared).
- Revisit the `AllocFastArrayInst` prototype narrow-back-to-object wart now that
  arrays are typed (let the instruction accept refined object operands).
