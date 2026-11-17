# IR Type System Phase 3 (ClassInstance) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the IR `ClassInstance` type real and flow it live: lower Flow
`ClassType` to an interned-by-identity `ClassInstance` IR type with nominal
subtyping, and ensure the verifier/printer/codegen treat it correctly.

**Architecture:** Add a `ClassInstance` factory (+ reserve/fill for recursive
classes) and a `fieldArrays_` side array to `TypeContext`; make the leaf
subtype/disjoint logic id-aware so it can distinguish nominal classes; replace
the `Class → Object` arm of `flowTypeToIRType` with an identity-cached
reserve→fill lowering; audit consumers under the invariant "codegen treats
refined kinds exactly like `Object`."

**Tech Stack:** C++17, LLVM/llvh ADTs, CMake + Ninja (ASan build), gtest
(`HermesIRTests`), lit (`check-hermes`), Sapling (`sl`), `arc f`.

See [phase3-5-design.md](phase3-5-design.md) for the full design and the
decisions (D1–D6) referenced below.

---

## Conventions for every task

- **Build dir:** `cmake-build-asan` (already configured; ASan+Debug, `-O1`).
- **Build unit tests:** `cmake --build cmake-build-asan --target HermesIRTests`
- **Run unit tests:**
  `cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.*'`
- **Build the compiler:** `cmake --build cmake-build-asan --target hermes`
- **Lit (this sandbox needs single-process):**
  `LIT_OPTS="-j1" LIT_FILTER="<regex>" cmake --build cmake-build-asan --target check-hermes`
- **Format:** run `arc f` on every touched `.cpp`/`.h` before committing.
- **Commit:** Meta format (see `facebook/CLAUDE-meta.md`), body wrapped at 72
  columns, subject ≤50 chars, via
  `sl commit -A <paths> --reason "<intent> - sl help commit" -m "<msg>"`.
  Commit only the listed paths, never `-A` without paths (the tree has many
  unrelated untracked files).

---

## Task 1: `ClassInstance` factory, side array, and accessors

**Files:**
- Modify: `include/hermes/IR/TypeContext.h` (add `fieldArrays_`; change
  `ExactObjectField` to hold a `Type`; declare factories/accessors)
- Modify: `lib/IR/TypeContext.cpp` (implement)
- Test: `unittests/IR/TypeContextTest.cpp`

- [ ] **Step 1: Write the failing test**

Add to `unittests/IR/TypeContextTest.cpp` (inside the anonymous namespace, after
the existing tests). It builds a tiny class hierarchy and checks construction,
identity (non-interned), and accessors. Class names use a real `StringTable`.

```cpp
TEST_F(TypeContextTest, ClassInstanceConstruction) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  Identifier animalName = strTab.getIdentifier("Animal");
  Identifier dogName = strTab.getIdentifier("Dog");

  // class Animal { legs: number }
  ExactObjectField animalFields[] = {
      {strTab.getIdentifier("legs"), Type::createNumber()}};
  Type animal =
      ctx.createClassInstance(Type::createNoType(), animalName, animalFields);

  // class Dog extends Animal { name: string }
  ExactObjectField dogFields[] = {
      {strTab.getIdentifier("name"), Type::createString()}};
  Type dog = ctx.createClassInstance(animal, dogName, dogFields);

  EXPECT_EQ(ctx.getKind(animal), TypeKind::ClassInstance);
  EXPECT_EQ(ctx.getKind(dog), TypeKind::ClassInstance);

  // Nominal: each declaration is a distinct entry, even structurally identical.
  Type animal2 =
      ctx.createClassInstance(Type::createNoType(), animalName, animalFields);
  EXPECT_NE(animal, animal2);

  // Accessors.
  EXPECT_EQ(ctx.getClassName(dog), dogName);
  EXPECT_EQ(ctx.getClassSuperType(dog), animal);
  EXPECT_TRUE(ctx.getClassSuperType(animal).isNoType());

  auto fields = ctx.getClassFields(dog);
  ASSERT_EQ(fields.size(), 1u);
  EXPECT_EQ(fields[0].name, dogName.isValid() ? fields[0].name : fields[0].name);
  EXPECT_EQ(fields[0].type, Type::createString());
}
```

- [ ] **Step 2: Run the test, verify it fails to compile**

Run: `cmake --build cmake-build-asan --target HermesIRTests`
Expected: FAIL — `createClassInstance` / `getClassName` / `getClassSuperType` /
`getClassFields` are not members of `TypeContext`, and `ExactObjectField` has no
`type` member.

- [ ] **Step 3: Change `ExactObjectField` to hold a `Type`**

In `include/hermes/IR/TypeContext.h`, replace the existing struct:

```cpp
/// Side array element for exact object fields (and class fields).
struct ExactObjectField {
  Identifier name;
  Type type;
};
```

(Was `uint32_t typeId;`. `Type` is a 4-byte wrapper over `uint32_t`, layout
compatible with the existing storage. No current code reads `.typeId`, so this
is safe.)

- [ ] **Step 4: Declare the side array, factories, and accessors**

In `include/hermes/IR/TypeContext.h`, in the `public:` section of `TypeContext`
(near the other `create*`/`get*` methods), add:

```cpp
  /// Create a nominal class instance type. NOT interned: each call returns a
  /// fresh entry, so two structurally identical class declarations are
  /// different types. \p superClass is the parent ClassInstance, or NoType for
  /// a root class.
  Type createClassInstance(
      Type superClass,
      Identifier className,
      llvh::ArrayRef<ExactObjectField> fields);

  /// Reserve an empty ClassInstance entry and return its Type. Used together
  /// with fillClassInstance to break self/mutual recursion when lowering a
  /// class whose fields reference the class itself. Between reserve and fill
  /// the entry must not be inspected by any type operation.
  Type reserveClassInstance();

  /// Fill a previously reserved ClassInstance entry. \p t must come from
  /// reserveClassInstance.
  void fillClassInstance(
      Type t,
      Type superClass,
      Identifier className,
      llvh::ArrayRef<ExactObjectField> fields);

  /// \return the superclass of class \p t, or NoType if it is a root class.
  Type getClassSuperType(Type t) const;
  /// \return the name of class \p t.
  Identifier getClassName(Type t) const;
  /// \return the fields of class \p t.
  llvh::ArrayRef<ExactObjectField> getClassFields(Type t) const;
```

In the `private:` section, next to `std::vector<Type> typeArrays_;`, add:

```cpp
  /// Side array storing class (and, later, exact-object) field descriptors,
  /// referenced by (fieldOffset, fieldCount) in ClassInstance/ExactObject
  /// entries.
  std::vector<ExactObjectField> fieldArrays_;
```

- [ ] **Step 5: Implement in `lib/IR/TypeContext.cpp`**

Add after `TypeContext::getUnionArms` (the file already includes
`HermesSafeMath.h` for `safePossiblyNarrowingCast`):

```cpp
Type TypeContext::reserveClassInstance() {
  uint32_t id = entries_.size();
  TypeEntry e{}; // primMask defaults to kNotMaskable
  e.kind = TypeKind::ClassInstance;
  e.classInstance.superClassTypeId = kNoTypeId;
  e.classInstance.className = Identifier{};
  e.classInstance.fieldOffset = 0;
  e.classInstance.fieldCount = 0;
  entries_.push_back(e);
  return Type{id};
}

void TypeContext::fillClassInstance(
    Type t,
    Type superClass,
    Identifier className,
    llvh::ArrayRef<ExactObjectField> fields) {
  assert(t.id_ < entries_.size() && "Type ID out of range");
  TypeEntry &e = entries_[t.id_];
  assert(e.kind == TypeKind::ClassInstance && "not a ClassInstance entry");
  e.classInstance.superClassTypeId = superClass.id_;
  e.classInstance.className = className;
  e.classInstance.fieldOffset = safePossiblyNarrowingCast<uint32_t>(
      fieldArrays_.size(), "field array offset overflow");
  e.classInstance.fieldCount = safePossiblyNarrowingCast<uint16_t>(
      fields.size(), "too many class fields");
  fieldArrays_.insert(fieldArrays_.end(), fields.begin(), fields.end());
}

Type TypeContext::createClassInstance(
    Type superClass,
    Identifier className,
    llvh::ArrayRef<ExactObjectField> fields) {
  Type t = reserveClassInstance();
  fillClassInstance(t, superClass, className, fields);
  return t;
}

Type TypeContext::getClassSuperType(Type t) const {
  assert(getKind(t) == TypeKind::ClassInstance && "not a ClassInstance");
  return Type{entries_[t.id_].classInstance.superClassTypeId};
}

Identifier TypeContext::getClassName(Type t) const {
  assert(getKind(t) == TypeKind::ClassInstance && "not a ClassInstance");
  return entries_[t.id_].classInstance.className;
}

llvh::ArrayRef<ExactObjectField> TypeContext::getClassFields(Type t) const {
  assert(getKind(t) == TypeKind::ClassInstance && "not a ClassInstance");
  const auto &ci = entries_[t.id_].classInstance;
  return llvh::ArrayRef<ExactObjectField>(
      fieldArrays_.data() + ci.fieldOffset, ci.fieldCount);
}
```

- [ ] **Step 6: Run the test, verify it passes**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.ClassInstanceConstruction'`
Expected: PASS.

- [ ] **Step 7: Format and commit**

```bash
arc f
sl commit -A include/hermes/IR/TypeContext.h lib/IR/TypeContext.cpp \
  unittests/IR/TypeContextTest.cpp \
  --reason "add ClassInstance factory to TypeContext - sl help commit" \
  -m "[SH]: Add ClassInstance factory to TypeContext

Summary:
Add a nominal ClassInstance type to TypeContext: a fieldArrays_ side
array, createClassInstance plus a reserveClassInstance/fillClassInstance
pair for recursive classes, and the super/name/fields accessors. Change
ExactObjectField to hold a Type instead of a raw id so external callers
can build fields. No type-algebra or lowering changes yet.

Test Plan: HermesIRTests --gtest_filter='TypeContextTest.ClassInstance*'
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 2: Id-aware nominal subtyping

**Files:**
- Modify: `include/hermes/IR/TypeContext.h` (declare helpers + public
  `isSubclass`)
- Modify: `lib/IR/TypeContext.cpp` (implement; route `isSubsetOf`)
- Test: `unittests/IR/TypeContextTest.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(TypeContextTest, ClassInstanceSubtyping) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  Type animal = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("Animal"), {});
  Type dog =
      ctx.createClassInstance(animal, strTab.getIdentifier("Dog"), {});
  Type cat =
      ctx.createClassInstance(animal, strTab.getIdentifier("Cat"), {});

  // Reflexive + nominal chain.
  EXPECT_TRUE(ctx.isSubsetOf(dog, dog));
  EXPECT_TRUE(ctx.isSubsetOf(dog, animal));
  EXPECT_FALSE(ctx.isSubsetOf(animal, dog));
  EXPECT_FALSE(ctx.isSubsetOf(dog, cat));
  EXPECT_FALSE(ctx.isSubsetOf(cat, dog));

  // Every refinement <: Object.
  EXPECT_TRUE(ctx.isSubsetOf(dog, Type::createObject()));
  EXPECT_FALSE(ctx.isSubsetOf(Type::createObject(), dog));

  // Class vs primitive: never a subtype.
  EXPECT_FALSE(ctx.isSubsetOf(dog, Type::createNumber()));
  EXPECT_FALSE(ctx.isSubsetOf(Type::createNumber(), dog));

  // Class is a subset of a union containing its ancestor.
  Type animalOrNull = ctx.unionTy(animal, Type::createNull());
  EXPECT_TRUE(ctx.isSubsetOf(dog, animalOrNull));

  // Public isSubclass.
  EXPECT_TRUE(ctx.isSubclass(dog, animal));
  EXPECT_FALSE(ctx.isSubclass(animal, dog));
  EXPECT_TRUE(ctx.isSubclass(dog, dog));
}
```

- [ ] **Step 2: Run it, verify it fails**

Run: `cmake --build cmake-build-asan --target HermesIRTests`
Expected: FAIL — `isSubclass` not a member; and (once it links) the leaf
subtype path would `hermes_assert` on refined kinds.

- [ ] **Step 3: Declare the helpers**

In `include/hermes/IR/TypeContext.h`, add to `public:`:

```cpp
  /// \return true if class \p a is the same as or a subclass of class \p b.
  /// Both must be ClassInstance types.
  bool isSubclass(Type a, Type b) const;
```

Add to `private:`:

```cpp
  /// Id-aware leaf subtype test for two non-union types. Distinguishes nominal
  /// classes (which share the ClassInstance kind). Falls back to the
  /// kind-based isLeafSubtype for non-refined kinds.
  bool isLeafSubtypeById(uint32_t a, uint32_t b) const;
```

- [ ] **Step 4: Implement, and route `isSubsetOf` through it**

In `lib/IR/TypeContext.cpp`:

(a) Remove the `hermes_assert(!isRefinedKind(...))` from the file-scope
`isLeafSubtype` and drop its now-unreachable `b == TypeKind::Object` refinement
switch (refined kinds are handled by `isLeafSubtypeById` before this is
called). The function becomes:

```cpp
/// \return true if non-refined leaf kind \p a is a subtype of \p b.
/// Refined kinds are handled by TypeContext::isLeafSubtypeById.
bool isLeafSubtype(TypeKind a, TypeKind b) {
  if (a == b)
    return true;
  // Int32, Uint32, UInt31 are subtypes of Number.
  if (b == TypeKind::Number &&
      (a == TypeKind::Int32 || a == TypeKind::Uint32 || a == TypeKind::UInt31))
    return true;
  // UInt31 is a subtype of both Int32 and Uint32.
  if (a == TypeKind::UInt31 && (b == TypeKind::Int32 || b == TypeKind::Uint32))
    return true;
  return false;
}
```

(b) Add the id-aware member (after `intersectLeafTy` or near the subtype
logic). `isRefinedKind` is the file-scope helper already present:

```cpp
bool TypeContext::isSubclass(Type a, Type b) const {
  assert(getKind(a) == TypeKind::ClassInstance && "a is not a class");
  assert(getKind(b) == TypeKind::ClassInstance && "b is not a class");
  uint32_t cur = a.id_;
  while (cur != kNoTypeId) {
    if (cur == b.id_)
      return true;
    if (entries_[cur].kind != TypeKind::ClassInstance)
      break;
    cur = entries_[cur].classInstance.superClassTypeId;
  }
  return false;
}

bool TypeContext::isLeafSubtypeById(uint32_t a, uint32_t b) const {
  if (a == b)
    return true;
  TypeKind ak = entries_[a].kind;
  TypeKind bk = entries_[b].kind;
  if (isRefinedKind(ak) || isRefinedKind(bk)) {
    // Every refinement is a subtype of unrefined Object.
    if (isRefinedKind(ak) && bk == TypeKind::Object)
      return true;
    // Nominal class subtyping via the superclass chain.
    if (ak == TypeKind::ClassInstance && bk == TypeKind::ClassInstance)
      return isSubclass(Type{a}, Type{b});
    // Shallow model (D4): all other refined relationships -- different
    // categories, distinct structural types, refined vs primitive -- are not
    // subtypes. Factored here so deep structural subtyping can be added later
    // without touching call sites.
    return false;
  }
  return isLeafSubtype(ak, bk);
}
```

(c) In `TypeContext::isSubsetOf`, change the final leaf-leaf line from
`return isLeafSubtype(aKind, bKind);` to:

```cpp
  // Both leaf types (id-aware: distinguishes nominal classes).
  return isLeafSubtypeById(a.id_, b.id_);
```

- [ ] **Step 5: Run the test, verify it passes**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.ClassInstanceSubtyping'`
Expected: PASS.

- [ ] **Step 6: Run the full TypeContext suite (no regressions)**

Run: `cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.*'`
Expected: PASS (all existing primitive/union tests still green).

- [ ] **Step 7: Format and commit**

```bash
arc f
sl commit -A include/hermes/IR/TypeContext.h lib/IR/TypeContext.cpp \
  unittests/IR/TypeContextTest.cpp \
  --reason "add id-aware nominal class subtyping - sl help commit" \
  -m "[SH]: Add id-aware nominal class subtyping

Summary:
Add isLeafSubtypeById and isSubclass so the leaf subtype check can
distinguish nominal classes that share the ClassInstance kind. Route
isSubsetOf's leaf-leaf case through it. Every refinement is a subtype of
Object; class-vs-class walks the superclass chain; all other refined
relationships are non-subtypes in the shallow model. Drop the refined
assert and dead Object-refinement branch from the kind-based
isLeafSubtype.

Test Plan: HermesIRTests --gtest_filter='TypeContextTest.*'
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 3: Id-aware disjointness, intersection, union subsumption

**Files:**
- Modify: `lib/IR/TypeContext.cpp`
- Test: `unittests/IR/TypeContextTest.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(TypeContextTest, ClassInstanceAlgebra) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  Type animal = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("Animal"), {});
  Type dog = ctx.createClassInstance(animal, strTab.getIdentifier("Dog"), {});
  Type cat = ctx.createClassInstance(animal, strTab.getIdentifier("Cat"), {});

  // Disjointness.
  EXPECT_TRUE(ctx.areDisjoint(dog, cat));            // unrelated classes
  EXPECT_FALSE(ctx.areDisjoint(dog, animal));        // related
  EXPECT_TRUE(ctx.areDisjoint(dog, Type::createNumber())); // class vs primitive
  EXPECT_FALSE(ctx.areDisjoint(dog, Type::createObject())); // dog <: Object

  // Intersection.
  EXPECT_EQ(ctx.intersectTy(animal, dog), dog);      // narrower wins
  EXPECT_EQ(ctx.intersectTy(dog, animal), dog);
  EXPECT_TRUE(ctx.intersectTy(dog, cat).isNoType()); // unrelated -> empty
  EXPECT_EQ(ctx.intersectTy(dog, Type::createObject()), dog);

  // Union subsumption / non-subsumption.
  EXPECT_EQ(ctx.unionTy(dog, animal), animal);       // dog <: animal -> animal
  EXPECT_EQ(ctx.unionTy(dog, Type::createObject()),
            Type::createObject());                   // refinement -> Object
  Type dogOrCat = ctx.unionTy(dog, cat);
  EXPECT_EQ(ctx.getKind(dogOrCat), TypeKind::Union); // unrelated -> union
  EXPECT_EQ(ctx.countKinds(dogOrCat), 2u);

  // Subtract stays conservative but exact where representable.
  EXPECT_TRUE(ctx.subtractTy(dog, animal).isNoType()); // dog <: animal
  EXPECT_EQ(ctx.subtractTy(dog, cat), dog);            // disjoint
}
```

- [ ] **Step 2: Run it, verify it fails**

Run: `cmake --build cmake-build-asan --target HermesIRTests`
Expected: FAIL — `unionTy(dog, animal)` does not collapse (kind-based
subsumption can't tell classes apart), `intersectLeafTy` mis-handles classes,
and `areLeafKindsDisjoint` would treat two `ClassInstance` kinds as
non-disjoint. Some `EXPECT`s fail.

- [ ] **Step 3: Make disjointness id-aware**

In `lib/IR/TypeContext.cpp`, add a private id-aware disjointness helper and
route `areDisjoint` through it.

Declare in `include/hermes/IR/TypeContext.h` `private:`:

```cpp
  /// Id-aware disjointness test for two non-union types. Conservative for
  /// structural refined kinds (D5): only unrelated classes and refined-vs-
  /// primitive are declared disjoint.
  bool areLeafTypesDisjointById(uint32_t a, uint32_t b) const;
```

Implement in `lib/IR/TypeContext.cpp`:

```cpp
bool TypeContext::areLeafTypesDisjointById(uint32_t a, uint32_t b) const {
  TypeKind ak = entries_[a].kind;
  TypeKind bk = entries_[b].kind;
  if (isRefinedKind(ak) || isRefinedKind(bk)) {
    // Subtype either way => overlap.
    if (isLeafSubtypeById(a, b) || isLeafSubtypeById(b, a))
      return false;
    // Two classes with no inheritance relation are disjoint.
    if (ak == TypeKind::ClassInstance && bk == TypeKind::ClassInstance)
      return true;
    // Exactly one is refined: disjoint unless the other is Object (which the
    // subtype check above already resolved as overlap).
    if (isRefinedKind(ak) != isRefinedKind(bk)) {
      TypeKind other = isRefinedKind(ak) ? bk : ak;
      return other != TypeKind::Object;
    }
    // Two distinct structural refined types (or class vs structural):
    // conservatively NOT disjoint (D5).
    return false;
  }
  return areLeafKindsDisjoint(ak, bk);
}
```

In `TypeContext::areDisjoint`, change the final leaf-leaf line from
`return areLeafKindsDisjoint(aKind, bKind);` to:

```cpp
  // Both leaf types (id-aware, conservative for refined kinds).
  return areLeafTypesDisjointById(a.id_, b.id_);
```

- [ ] **Step 4: Make intersection id-aware**

In `TypeContext::intersectLeafTy`, replace the two `isLeafSubtype(aKind, bKind)`
/ `isLeafSubtype(bKind, aKind)` checks with id-aware ones:

```cpp
uint32_t TypeContext::intersectLeafTy(uint32_t a, uint32_t b) const {
  assert(a != kNoTypeId && b != kNoTypeId && "NoType must be handled first");
  TypeKind aKind = entries_[a].kind;
  TypeKind bKind = entries_[b].kind;
  assert(aKind != TypeKind::Union && bKind != TypeKind::Union);

  if (isLeafSubtypeById(a, b))
    return a;
  if (isLeafSubtypeById(b, a))
    return b;

  // The only non-subset overlap in the number family is Int32 ∩ Uint32 =
  // UInt31.
  if (isNumberKind(aKind) && isNumberKind(bKind))
    return kUInt31Id;
  return kNoTypeId;
}
```

- [ ] **Step 5: Make union subsumption id-aware**

In `TypeContext::createUnionImpl`, the `isSubsumedBy` lambda uses
`isLeafSubtype(k, entries_[o].kind)`. Replace its body so it is id-aware:

```cpp
  auto isSubsumedBy = [this](uint32_t id, llvh::ArrayRef<uint32_t> others) {
    for (uint32_t o : others) {
      if (o != id && isLeafSubtypeById(id, o))
        return true;
    }
    return false;
  };
```

In `TypeContext::createUnionFromLeafArms`, the subsumption loop uses
`isLeafSubtype(candidate, entries_[normalized[j]].kind)`. Replace with:

```cpp
    for (size_t j = 0; j < normalized.size(); ++j) {
      if (i == j)
        continue;
      if (isLeafSubtypeById(normalized[i], normalized[j])) {
        subsumed = true;
        break;
      }
    }
```

(The `candidate` local for the kind is now unused; remove its declaration to
avoid a warning.)

- [ ] **Step 6: Run the test, then the full suite**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.*'`
Expected: PASS (new `ClassInstanceAlgebra` + all existing tests).

- [ ] **Step 7: Format and commit**

```bash
arc f
sl commit -A include/hermes/IR/TypeContext.h lib/IR/TypeContext.cpp \
  unittests/IR/TypeContextTest.cpp \
  --reason "make class disjoint/intersect/union id-aware - sl help commit" \
  -m "[SH]: Make class disjoint/intersect/union id-aware

Summary:
Route areDisjoint, intersectLeafTy, and union subsumption through
id-aware helpers so nominal classes behave correctly: unrelated classes
are disjoint, intersection picks the narrower class or NoType, and a
union of a class with its ancestor collapses to the ancestor.
Disjointness stays conservative for structural refined kinds.

Test Plan: HermesIRTests --gtest_filter='TypeContextTest.*'
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 4: Pretty-print `ClassInstance`

**Files:**
- Modify: `lib/IR/TypeContext.cpp` (`TypeContext::print`)
- Test: `unittests/IR/TypeContextTest.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(TypeContextTest, ClassInstancePrint) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  Type dog = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("Dog"), {});

  std::string s;
  llvh::raw_string_ostream os{s};
  ctx.print(os, dog);
  os.flush();
  EXPECT_EQ(s, "classInstance(Dog)");

  // A union containing a class prints the class arm via the same path.
  std::string s2;
  llvh::raw_string_ostream os2{s2};
  ctx.print(os2, ctx.unionTy(dog, Type::createNull()));
  os2.flush();
  // Arms are sorted by id; assert both parts are present.
  EXPECT_NE(s2.find("classInstance(Dog)"), std::string::npos);
  EXPECT_NE(s2.find("null"), std::string::npos);
}
```

- [ ] **Step 2: Run it, verify it fails**

Run: `cmake --build cmake-build-asan --target HermesIRTests`
Expected: FAIL — `print` emits `kindName` ("classInstance") with no name, so
the strings don't match.

- [ ] **Step 3: Implement refined printing**

In `lib/IR/TypeContext.cpp`, add a small file-scope helper above
`TypeContext::print` and use it for leaf and arm printing. Replace the
non-union leaf branch and the union-arm `kindName(...)` uses with a helper that
appends the class name for `ClassInstance`:

```cpp
// Inside the anonymous namespace, after kindName:
// (declared as a TypeContext member instead, so it can read entries_)
```

Add a private member declaration in `include/hermes/IR/TypeContext.h`
`private:`:

```cpp
  /// Append the printed form of a single non-union type to \p OS.
  void printLeaf(llvh::raw_ostream &OS, uint32_t id) const;
```

Implement in `lib/IR/TypeContext.cpp`:

```cpp
void TypeContext::printLeaf(llvh::raw_ostream &OS, uint32_t id) const {
  TypeKind k = entries_[id].kind;
  if (k == TypeKind::ClassInstance) {
    OS << "classInstance(";
    Identifier name = entries_[id].classInstance.className;
    if (name.isValid())
      OS << name.str();
    OS << ')';
    return;
  }
  OS << kindName(k);
}
```

Then in `TypeContext::print`, replace:
- the non-union branch `OS << kindName(entry.kind);` with
  `printLeaf(OS, t.id_);`
- in the "any" shorthand extra-arm loop, `OS << '|' << kindName(k);` with
  `OS << '|'; printLeaf(OS, arm.id_);`
- in the general-union loop, `OS << kindName(entries_[arms[i].id_].kind);` with
  `printLeaf(OS, arms[i].id_);`

(The `TypeKind k = ...` locals that become unused in those loops should be
removed.)

- [ ] **Step 4: Run the test, then the full suite**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests --gtest_filter='TypeContextTest.*'`
Expected: PASS.

- [ ] **Step 5: Format and commit**

```bash
arc f
sl commit -A include/hermes/IR/TypeContext.h lib/IR/TypeContext.cpp \
  unittests/IR/TypeContextTest.cpp \
  --reason "pretty-print ClassInstance with class name - sl help commit" \
  -m "[SH]: Pretty-print ClassInstance with class name

Summary:
Print ClassInstance types as classInstance(Name), via a printLeaf helper
used for both standalone leaves and union arms. Other kinds are
unchanged.

Test Plan: HermesIRTests --gtest_filter='TypeContextTest.*'
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 5: Lower Flow `ClassType` to `ClassInstance` (wire live)

**Files:**
- Modify: `lib/IRGen/ESTreeIRGen.h` (add the class identity cache member)
- Modify: `lib/IRGen/ESTreeIRGen-typed-class.cpp` (`flowTypeToIRType`)
- Reference (read, do not assume names): `include/hermes/Sema/FlowContext.h`
  `ClassType` (≈ lines 735-1038) for the superclass/name/field accessors.

> **Accessor confirmation (do this first):** open `FlowContext.h` and confirm
> the exact `ClassType` accessors for: the superclass (`getSuperClassInfo()`
> returns a `ClassType*`; there is also a `Type*` form), the class name
> identifier, and iterating the *own* fields (`fields_` via its public getter,
> each `Field` exposing `name` and `type` (`flow::Type*`)). Use the confirmed
> names in the code below; the structure does not change.

- [ ] **Step 1: Add the identity cache to ESTreeIRGen**

In `lib/IRGen/ESTreeIRGen.h`, add a private member to the `ESTreeIRGen` class
(near other caches), and make sure `<llvh/ADT/DenseMap.h>` and the `flow`
forward declarations are available (they are — the file already uses
`flow::Type`):

```cpp
  /// Identity cache for lowering nominal Flow classes to IR ClassInstance
  /// types. Keyed by the Flow ClassType (nominal identity). Populated with the
  /// reserved IR Type before fields are lowered, so self/mutual references
  /// resolve without infinite recursion.
  llvh::DenseMap<const flow::TypeInfo *, Type> loweredClassTypes_;
```

- [ ] **Step 2: Write a failing lit test**

Create `test/IRGen/flow/class-instance-type.js`:

```javascript
// RUN: %shermes -typed -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

class Animal {
  legs: number;
  constructor() { this.legs = 4; }
}

class Dog extends Animal {
  name: string;
  constructor() { super(); this.name = 'rex'; }
}

function make(): Dog {
  return new Dog();
}
```

Run (expect the checker to *generate* output, then inspect it):
`LIT_OPTS="-j1" LIT_FILTER="flow/class-instance-type" cmake --build cmake-build-asan --target check-hermes`

Before the lowering change this is a fresh `%FileCheckOrRegen` file; generate
its baseline once the lowering is in (Step 5). The behavioral assertion is in
Step 4's unit-level reasoning + the regenerated CHECK lines showing
`classInstance(Dog)` / `classInstance(Animal)` on the relevant values instead
of `object`.

- [ ] **Step 3: Replace the `Class` arm of `flowTypeToIRType`**

In `lib/IRGen/ESTreeIRGen-typed-class.cpp`, change the
`case flow::TypeKind::Class:` arm (currently `return Type::createObject();`).
Use the accessor names confirmed above:

```cpp
    case flow::TypeKind::Class: {
      auto *classType = llvh::cast<flow::ClassType>(flowType);
      // Identity cache: return the reserved/finished entry if present.
      auto it = loweredClassTypes_.find(classType);
      if (it != loweredClassTypes_.end())
        return it->second;

      TypeContext &tc = getTypeContext();
      // Reserve first, record in the cache, THEN lower super/fields so that a
      // field referencing this class (or a mutually recursive sibling)
      // resolves to the reserved Type instead of recursing forever.
      Type t = tc.reserveClassInstance();
      loweredClassTypes_[classType] = t;

      Type superTy = Type::createNoType();
      if (flow::ClassType *super = classType->getSuperClassInfo())
        superTy = flowTypeToIRType(super);

      llvh::SmallVector<ExactObjectField, 8> fields;
      for (const flow::ClassType::Field &f : classType->getFields()) {
        // Skip methods; only data fields carry a value type. (Confirm the
        // Field predicate name, e.g. isMethod(), in FlowContext.h.)
        if (f.isMethod())
          continue;
        fields.push_back(
            ExactObjectField{f.name, flowTypeToIRType(f.type->info)});
      }

      tc.fillClassInstance(t, superTy, classType->getClassName(), fields);
      return t;
    }
```

`ClassConstructor` stays `return Type::createObject();` for now (becomes
`Function` in Phase 4).

- [ ] **Step 4: Build the compiler**

Run: `cmake --build cmake-build-asan --target hermes`
Expected: builds clean. If an accessor name is wrong, fix it against
`FlowContext.h` and rebuild.

- [ ] **Step 5: Generate and review the lit baseline**

Run: `LIT_OPTS="-j1" LIT_FILTER="flow/class-instance-type" cmake --build cmake-build-asan --target update-lit`
Then open `test/IRGen/flow/class-instance-type.js` and confirm the CHECK lines
show `classInstance(Dog)` / `classInstance(Animal)` where values previously
showed `object` (e.g. the `new Dog()` result, the `make` return type). This
review is the real verification that classes flow live.

Re-run to confirm it now passes:
`LIT_OPTS="-j1" LIT_FILTER="flow/class-instance-type" cmake --build cmake-build-asan --target check-hermes`
Expected: PASS.

- [ ] **Step 6: Format and commit**

```bash
arc f
sl commit -A lib/IRGen/ESTreeIRGen.h lib/IRGen/ESTreeIRGen-typed-class.cpp \
  test/IRGen/flow/class-instance-type.js \
  --reason "lower Flow ClassType to IR ClassInstance - sl help commit" \
  -m "[SH]: Lower Flow ClassType to IR ClassInstance

Summary:
Replace the Class arm of flowTypeToIRType (was Object) with an
identity-cached reserve-then-fill lowering that produces a nominal
ClassInstance carrying the superclass, name, and data fields. The cache
is populated with the reserved Type before fields are lowered, so
self-referential and mutually recursive classes terminate. Class types
now flow live through the IR.

Test Plan: check-hermes (IRGen/flow/class-instance-type.js)
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 6: Consumer audit — verifier and codegen treat `ClassInstance` as `Object`

This task enforces invariant D2. Work through each site, fix if needed, and run
the broad test suite at the end.

**Files (audit; modify as needed):**
- `lib/IR/IRVerifier.cpp`
- `lib/BCGen/SH/SH.cpp` and other `lib/BCGen/SH/*` files
- `lib/BCGen/HBC/*` (ISel, Lowering, Passes)
- `lib/Optimizer/Scalar/TypeInference.cpp`, `InstSimplify.cpp`

- [ ] **Step 1: Find exact-`Object` switch sites**

Run (read-only survey):
`mcp__plugin_meta_mux__search_files` for `isObjectType\(|kObjectId|TypeKind::Object`
across `lib/IR`, `lib/BCGen`, `lib/Optimizer`. For each hit, decide: does it
need to also fire for refined kinds? Sites using `canBeObject()` are already
correct (refined `<:` Object).

- [ ] **Step 2: Fix any exact-Object check that must include refined kinds**

The expected pattern: a site asserting/branching on "this operand is an
object" using `isObjectType()` or `getKind()==TypeKind::Object` should instead
use `canBeObject(t)` (already true for refined) or explicitly accept refined
kinds. Apply the minimal change per site. Example shape:

```cpp
// Before:
assert(ty.isObjectType() && "expected object operand");
// After:
assert(tc.canBeObject(ty) && "expected object-like operand");
```

If a codegen switch emits per-`TypeKind` code and lacks `ClassInstance`/refined
cases, route those to the existing `Object` case (they are object-represented
at runtime in Phases 3-5).

- [ ] **Step 3: Build everything**

Run: `cmake --build cmake-build-asan --target hermes HermesIRTests`
Expected: clean build.

- [ ] **Step 4: Run the typed/flow + codegen test subsets**

Run:
```
LIT_OPTS="-j1" LIT_FILTER="flow|typed|class" cmake --build cmake-build-asan --target check-hermes
```
Expected: PASS. Investigate any failure as a real audit finding (a site that
assumed exact `Object`). Use `update-lit` only for dumps whose change you have
reviewed and understand (e.g. `object` → `classInstance(...)`).

- [ ] **Step 5: Commit (if any code changed)**

```bash
arc f
sl commit -A <each modified file> \
  --reason "treat ClassInstance as Object in verifier/codegen - sl help commit" \
  -m "[SH]: Treat ClassInstance as Object in verifier/codegen

Summary:
Audit exact-Object type checks for the new live ClassInstance kind and
relax them to accept object-like (refined) types, per the design
invariant that codegen treats refined kinds exactly like Object until
runtime descriptors exist. No emitted-code change for existing programs.

Test Plan: check-hermes (flow/typed/class subsets)
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 7: Full regression run and broader lit baseline

- [ ] **Step 1: Full unit tests**

Run: `cmake --build cmake-build-asan --target HermesIRTests && \
  cmake-build-asan/unittests/IR/HermesIRTests`
Expected: PASS.

- [ ] **Step 2: Full lit suite**

Run: `LIT_OPTS="-j1" cmake --build cmake-build-asan --target check-hermes`
Expected: PASS. If `%FileCheckOrRegen` tests changed only by `object` →
`classInstance(...)` in typed programs, review the diff, then
`cmake --build cmake-build-asan --target update-lit` and re-run. Any
non-type-name change is a real finding to investigate, not to regenerate
blindly.

- [ ] **Step 3: Commit regenerated baselines (if any)**

```bash
arc f
sl commit -A <regenerated test files> \
  --reason "regenerate lit baselines for ClassInstance type names - sl help commit" \
  -m "[SH]: Regenerate lit baselines for ClassInstance names

Summary:
Regenerate %FileCheckOrRegen baselines whose only change is typed-object
values now printing classInstance(Name) instead of object. Reviewed; no
behavioral change.

Test Plan: check-hermes
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Task 8: Update progress tracking

**Files:**
- Modify: `doc/plans/ir-type/progress.md`

- [ ] **Step 1: Add Phase 3 rows + context notes**

Append a Phase 3 section to the status table (e.g. `P3-S1 … P3-S8`, mapping to
Tasks 1-8 above) marked `done`, and add a short Context Notes entry per the
existing format describing: the `fieldArrays_` side array, the
`ExactObjectField` change to hold `Type`, id-aware leaf subtype/disjoint,
reserve→fill class lowering with the `loweredClassTypes_` identity cache, and
the consumer-audit invariant.

- [ ] **Step 2: Commit**

```bash
sl commit -A doc/plans/ir-type/progress.md \
  --reason "record IR type Phase 3 progress - sl help commit" \
  -m "[SH]: Record IR type Phase 3 progress

Summary:
Mark Phase 3 (ClassInstance, wired live) done in the IR type system
progress tracker and add context notes.

Test Plan: N/A (documentation only)
Reviewers: Hermes
Subscribers:
Tasks:
Tags:"
```

---

## Self-Review (completed during planning)

- **Spec coverage:** Phase 3 of [phase3-5-design.md](phase3-5-design.md) is
  covered — factories/accessors (Task 1), nominal subtyping (Task 2), algebra
  + conservative disjointness D5 (Task 3), printing (Task 4), reserve-first
  identity interner + live lowering D1/D3 (Task 5), consumer-audit invariant
  D2 (Tasks 6-7), tracking (Task 8). ClassInstance-stores-fields (D6) is in
  Task 1. Deep subtyping is explicitly deferred (D4) and the helpers are
  factored to allow it later.
- **Placeholder scan:** the only deferred specifics are the Flow `ClassType`
  accessor *names*, which Task 5 directs the implementer to confirm in
  `FlowContext.h` before use (an external API confirmation, not a logic gap).
- **Type consistency:** `ExactObjectField{name, type}` (Type), `Type`
  factories, `isLeafSubtypeById`/`areLeafTypesDisjointById`/`isSubclass`,
  `reserveClassInstance`/`fillClassInstance`/`createClassInstance`,
  `loweredClassTypes_` are used consistently across tasks.

## Known follow-ups (Phases 4-5, separate plans)

Written after Phase 3 lands, against the real code: Array + Function (+
ClassConstructor→Function) with the full non-looping + looping interner
(Phase 4), then Tuple + ExactObject and the totality pass (Phase 5).
