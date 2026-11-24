# Computed Property Cache Implementation Plan

**Status, 2026-11-14.** This is the plan the first form of the feature
was built from, kept as the record of that build. It was executed in
full. Afterwards the get and `hasOwnProperty` parts (Task 2: their
fast paths, fills and unit tests) were replaced by a lookup in the
class's own property map, and the table now serves puts only. The spec
describes the code as it is and says what changed under "History".

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `obj[key]` gets and puts and `hasOwnProperty`/`Object.hasOwn`,
with a uniqued string key, are answered from one runtime-wide cache keyed
on (hidden class, property name), in the interpreter, the JIT and native
code alike.

**Architecture:** A `ComputedPropertyCache` owned by the Runtime: 1024
sets of two entries, each entry a `Slot` fact (class has the data
property in this slot) or an `Add` fact (adding the name to this class
gives that class and slot, valid at this parent epoch). Fast paths at the
top of `JSObject::getComputedWithReceiver_RJS`,
`JSObject::putComputedWithReceiver_RJS`, `objectPrototypeHasOwnProperty`
and `objectHasOwn`; fills at the points where the generic code has just
established the fact. Class pointers are weak roots.

**Tech Stack:** C++17 (Hermes VM), gtest unit tests, lit/FileCheck.

**Spec:** doc/superpowers/specs/2026-11-11-computed-property-cache-design.md

Read the spec in full before your task, and `DESIGN-PRINCIPLES.md` (repo
root). Where this plan settles something the spec left open it says
"Spec resolution"; the list is repeated in the last section.

## Global Constraints

- Scope and non-goals are the spec's. No bytecode, compiler or JIT
  emitter change. No runtime switch, no counters in the product.
- A class is cacheable when none of `dictionaryMode`,
  `hasIndexLikeProperties`, `hostObject`, `lazyObject`, `proxyObject`,
  `indexedStorage` is set. A key has a name when it is a string with the
  uniqued bit; the name is `getUniqueID()`.
- Table: 1024 sets, 2 ways, allocated zeroed on the C++ heap in the
  cache's constructor. Entry at most 32 bytes (`static_assert`).
- Put fast path order, exactly: test "key is a string and receiver is the
  object"; encode the SmallHermesValue; only then test the uniqued bit,
  take the name, read the object and look up. Nothing allocates between
  the lookup and the store.
- The result class of an `Add` hit is read with
  `WeakRoot::getNonNull(runtime, runtime.getHeap())`, never a
  `...NoBarrierUnsafe` accessor.
- Every C++ change under `lib/VM/` or `include/hermes/VM/` is written
  after invoking the `gc-safe-coding` skill.
- 80 columns, 2-space indent, a doc comment on every declaration. Before
  each commit: `git clang-format HEAD -- <the task's C++ files>`.
- Never `cd`; use absolute paths, `git -C`, or `( cd dir; cmd; )`. The
  working directory is the repo root, `/home/tmikov/work/hermes-x86-jit`;
  paths below are relative to it.
- Never push or fetch. Never `git add -A`, `git add .` or
  `git add dz/issues`: stage exact paths. Never touch the untracked
  `2026-09-08-putbyval-typed-array-measurements.md`, `HANDOFF.md` and
  `dz/issues/01a063df-0527-71dc-89c2-2464bff4a1e4.md`.
- Build trees (configured, in the repo root): `cmake-build-x86jit` (ASan
  Debug, HV64, Hades, JIT forced, `HERMES_SLOW_DEBUG`; the reference
  tree), `cmake-build-x86jit-hv32`, `cmake-build-x86jit-boxed`,
  `cmake-build-x86jit-malloc` (MallocGC), `cmake-build-x86jit-hsan`
  (MallocGC, Handle-SAN, HV64); Release `cmake-build-jit2` (HV64) and
  `cmake-build-jit2-hv32`; arm64 cross trees `cmake-build-arm64`,
  `cmake-build-arm64-hv32` (run under qemu-user, `aarch64/README.md`);
  `cmake-build-host` (host compilers for the arm64 trees).
- Unit tests: `cmake --build <tree> --target HermesVMRuntimeTests`, then
  `<tree>/unittests/VMRuntime/HermesVMRuntimeTests
  --gtest_filter='ComputedPropertyCache*'`. Lit:
  `LIT_FILTER='computed-property-cache' cmake --build <tree> --target
  check-hermes`; the full suite without `LIT_FILTER`.
- Prove-can-fail: every new test is shown to FAIL with the mutation the
  task names, then the code is restored, the tree REBUILT and the test
  re-run green. Report each mutation and the failure it produced.
- Unit-test rules (spec "Tests"): nothing allocates between a fill and
  the `lookup` that observes it, or between planting an entry and the
  operation that should follow it; stored values are existing objects; a
  planted entry's name is held by a SymbolID handle and its classes by
  pinned values; tests of allocating paths assert results only.
- Measurements only from precompiled bytecode: each build's own
  `hermesc -O -emit-binary`.
- `S=/tmp/claude-1000/react-bench/cpc`; every temporary file goes there,
  never in the repo.
- Commits: one per task; fix rounds are folded into the task's commit
  (`git commit --amend`). Messages: subject at most 72 columns, body
  wrapped at 72, ``` fences for code, last line
  `Co-Authored-By: Claude <noreply@anthropic.com>`.

## Review Focus

Inputs the spec implies but none of its tests names. Each has a check in
the lit test of Task 5.

1. Keys that are not strings (a number, a Symbol, a String object, an
   object with `toString`) pass through the new first-thing tests and
   behave as before.
2. A primitive receiver (`(5)[k]`, `"abc"[k]`, `true[k]`): the lookup
   runs on the prototype object and may hit the cache for it; the result
   is the method, as before.
3. More live (class, name) pairs than the table holds (3000 in a loop):
   eviction never produces a wrong value.
4. Adds and stores of non-integer doubles in a loop long enough to hit:
   under HV32 and BOXED the encode allocates on the hit path.
5. `"__proto__"` as a computed key: on an ordinary object it reaches the
   accessor on `Object.prototype` and is never cached as an add; on an
   object with a null prototype it is a plain property.

---

### Task 1: The cache and its GC hook

**Files:**
- Create: `include/hermes/VM/ComputedPropertyCache.h`
- Create: `include/hermes/VM/ComputedPropertyCache-inline.h`
- Create: `lib/VM/ComputedPropertyCache.cpp`
- Create: `unittests/VMRuntime/ComputedPropertyCacheTest.cpp`
- Modify: `lib/VM/CMakeLists.txt` (source list, after `CodeBlock.cpp`)
- Modify: `unittests/VMRuntime/CMakeLists.txt` (after `CodeCoverageProfilerTest.cpp`)
- Modify: `include/hermes/VM/Runtime.h` (member next to
  `fixedReadPropCache_`, `:1434`; accessor next to
  `getParentCacheEpoch`, `:697`)
- Modify: `lib/VM/Runtime.cpp:780-790` (`markWeakRoots`)
- Modify: `include/hermes/VM/StringPrimitive.h:424-426`
  (`getUniqueID` moves to the public section)

**Interfaces:**
- Produces (`ComputedPropertyCache.h`; it may include only what
  `PropertyCache.h` includes plus `<memory>`: `HiddenClass` is
  forward-declared, because `Runtime.h` includes this header):

```cpp
class ComputedPropertyCache {
 public:
  enum class Kind : uint8_t { Slot, Add };
  struct Entry {
    WeakRoot<HiddenClass> clazz{nullptr}; // null: the entry is empty
    SymbolID name{};
    uint32_t slot : 30;    // the three bit-fields are zeroed by the
    uint32_t kind : 1;     // constructor; kind holds a Kind
    uint32_t writable : 1; // Slot only
    WeakRoot<HiddenClass> resultClazz{nullptr}; // Add only
    uint64_t parentEpoch{0};                    // Add only
    Kind getKind() const;
  };
  static constexpr unsigned kNumSets = 1024;
  static constexpr unsigned kNumWays = 2;
  static constexpr SlotIndex kMaxSlot = (1u << 30) - 1;

  ComputedPropertyCache();
  static unsigned setIndex(CompressedPointer clazz, SymbolID name);
  const Entry *lookup(CompressedPointer clazz, SymbolID name) const;
  void insertSlot(PointerBase &base, HiddenClass *clazz, SymbolID name,
                  SlotIndex slot, bool writable);
  void insertAdd(PointerBase &base, HiddenClass *startClazz, SymbolID name,
                 HiddenClass *resultClazz, SlotIndex slot,
                 uint64_t parentEpoch);
  void recordSlot(Runtime &runtime, HiddenClass *clazz, HermesValue key,
                  PropertyFlags flags, SlotIndex slot);
  void markWeakRoots(WeakRootAcceptor &acceptor);
  unsigned numEntries() const;
  void clear();
  // -inline.h:
  static bool isCacheable(const HiddenClass *clazz);
  static OptValue<SymbolID> nameOfKey(Runtime &runtime, HermesValue key);
};
```

  `Runtime::getComputedPropertyCache()` returns a reference to the
  Runtime's instance.
- `setIndex` is
  `((uint32_t)(clazz.getRaw() >> 3) * 0x9E3779B1u +
  name.unsafeGetRaw() * 0x85EBCA6Bu) >> 22`.
- `lookup` compares `clazz` and `name` of both ways, way 0 first, and
  returns the match or null; it is inline in the header. A live object's
  class is never null, so an empty entry never matches.
- `Entry` has no usable copy assignment: `CompressedPointer` deletes
  it (`CompressedPointer.h:78-89`), and so do the weak roots. Give
  `Entry` a default constructor that zeroes the three bit-fields, and
  two members used for every copy and reset: `void copyFrom(const Entry
  &other)`, field by field, the weak roots through
  `root = other.root.getNoBarrierUnsafe()` (`WeakRoot.h:47-55`), and
  `void reset()`, which sets `clazz` and `resultClazz` to `nullptr` and
  every other field to zero.
- `insertSlot`/`insertAdd` insert unconditionally (tests plant with
  them): if way 0 holds a different key, `way1.copyFrom(way0)`; then
  write every field of way 0. `insertSlot` asserts `slot <= kMaxSlot`.
- `recordSlot` does nothing unless `nameOfKey(key)` has a value,
  `isCacheable(clazz)`, `flags.indexed`, `flags.accessor`,
  `flags.hostObject` and `flags.proxyObject` are all clear, and
  `slot <= kMaxSlot`; then `insertSlot` with
  `writable = flags.writable && !flags.internalSetter`. No allocation
  (`NoAllocScope`).
- `nameOfKey`: `key.isString()` and `key.getString()->isUniqued()`, then
  `getUniqueID(runtime)`; otherwise `llvh::None`.
- `markWeakRoots`: for each entry with a non-null `clazz`, remember the
  raw `clazz`, `acceptWeak(clazz)`, for `Add` `acceptWeak(resultClazz)`;
  then `reset()` the entry if `clazz` is null or differs from the
  remembered value, or it is `Add` and `resultClazz` is null.
- `Runtime::markWeakRoots` calls it inside the existing
  `if (markLongLived)` block.

- [ ] **Step 1: Write the tests** in
  `unittests/VMRuntime/ComputedPropertyCacheTest.cpp`. The fixture is
  the file's own `ComputedPropertyCacheTest`, derived from
  `RuntimeTestFixtureBase` with `kTestGCConfigBuilder` and a sanitize
  rate of 1.0 (`GCSanitizeConfig::Builder().withSanitizeRate(1.0)`):
  `RuntimeTestFixture` uses 0.01 (`VMRuntimeTestHelpers.h:41-43`), which
  makes the Handle-SAN trees move the heap on one allocation in a
  hundred, and the tests need it on every one. Each test opens its own
  `GCScope scope{runtime, "<name>", 128}`.

  Rules for every test in this file, in all tasks:
  - Classes are never held as raw pointers across an allocation. A class
    is read from its object (`obj->getClassGCPtr()`, `obj->getClass(
    runtime)`) or from a pinned value at the point of use.
  - Before an entry is planted or its presence asserted, every object,
    key and value the following operation needs already exists.
  - A test that asserts the presence of an `Add` entry written by a
    fill (Task 4) first calls `premark` (below) on the prototype of its
    receivers: the add fill allocates while marking an unmarked chain,
    which under a moving collector empties the entry it just wrote.
    Tests that expect no entry, and all tests of Tasks 1-3, do not
    premark.

  File-local helpers that later tasks reuse:

```cpp
/// \return the SymbolID of \p s; its handle stays in the current GCScope.
SymbolID sym(Runtime &runtime, const char *s);
/// \return a handle to the uniqued string of \p id
/// (runtime.getStringPrimFromSymbolID).
Handle<> keyOf(Runtime &runtime, SymbolID id);
/// \return a new object whose parent is \p parent, with the named
/// properties added in order by JSObject::putNamed_RJS, each holding
/// \p value.
Handle<JSObject> makeObject(Runtime &runtime, Handle<JSObject> parent,
                            std::initializer_list<SymbolID> names,
                            Handle<> value);
/// A WeakRootAcceptor whose acceptWeak(WeakRootBase &) applies \p fn to
/// the root; the other two overloads do nothing.
struct TestWeakAcceptor;
/// Call JSObject::setCachedUsingEpoch on \p proto and on every object
/// above it on its prototype chain (Task 4 adds this helper).
void premark(Runtime &runtime, Handle<JSObject> proto);
```

  Tests (spec numbers in brackets):
  - `InsertLookup`: after `clear()`, `insertSlot(C, a, 3, true)` then
    `lookup(C, a)` has `getKind() == Slot`, `slot == 3`, `writable`;
    `lookup(C, b) == nullptr`; `numEntries() == 1`. `insertAdd(C, b, R,
    1, 77)` then `lookup(C, b)` has `Add`, `slot == 1`,
    `parentEpoch == 77`, `resultClazz == R`. Re-inserting `(C, a)` with
    slot 4 leaves `numEntries() == 2` and `slot == 4`.
  - `TwoWays` [13]: with one pinned class and no allocation from here
    to the end of the test, search `SymbolID::unsafeCreate(i)` for
    i = 1..2049 for three with equal `setIndex` for the class (2049
    names in 1024 sets guarantee a triple). The names are fabricated on
    purpose: the cache only compares names, and nothing is allocated, so
    the class's address, which is part of the hash, cannot change.
    Insert the first two, both `lookup` non-null; insert the third: the
    first is gone, the second and third remain.
  - `Predicate` [7]: `isCacheable` is true for the class of
    `JSObject::create(runtime)`; false for the class of a HostObject
    (`HostObject::createWithoutPrototype` with a trivial
    `HostObjectProxy`), of an array (`JSArray::create`), and of a class
    made by `HiddenClass::createRoot` with `lazyObject` set in its
    `ClassFlags`.
  - `RecordSlot` [7]: on a plain object's class with a uniqued key,
    default flags record `writable` set; flags with `internalSetter`
    record `writable` clear; with `accessor`, `indexed`, `hostObject` or
    `proxyObject` nothing; slot `1u << 30` nothing; a key that is a
    number, or a string from
    `StringPrimitive::create(runtime, createASCIIRef("a"))` (asserted
    not uniqued), nothing; default flags and a uniqued key with the
    class of an array: nothing.
  - `WeakRoots` [11]: entries `Slot(C1, a)`, `Add(C2, a) -> R`,
    `Slot(C3, a)`, classes pinned. Acceptor nulling only the root equal
    to `R`: `lookup(C2, a) == nullptr`, the other two remain,
    `numEntries()` down by one. Acceptor rewriting the root equal to `C1`
    to `C3`'s pointer: `lookup(C1, a) == nullptr` and `numEntries()` down
    by one. Acceptor nulling `C3`: `lookup(C3, a) == nullptr` (semantic,
    no mutation). Acceptor doing nothing: counts unchanged.
  - `CollectEmpties` [12, first half]: `clear()`; in an inner GCScope
    create 8 objects with distinct property names and
    `insertSlot(theirClass, name, 0, true)`; leave the scope;
    `runtime.collect("test")`; `numEntries() == 0`.
- [ ] **Step 2: Build and see the tests fail to compile** (no header).
- [ ] **Step 3: Implement** the cache, the Runtime member and accessor,
  the `markWeakRoots` call, the CMake entries, and make `getUniqueID`
  public.
- [ ] **Step 4: Build `HermesVMRuntimeTests` and `hermes` in
  `cmake-build-x86jit`; run the new tests: PASS.**
- [ ] **Step 5: Prove each test can fail.** Mutations: insertion without
  the way-0-to-way-1 copy (`TwoWays`); each predicate flag removed in
  turn that `Predicate` covers (`hostObject`, `indexedStorage`,
  `lazyObject`); each `recordSlot` condition removed in turn, the
  `isCacheable` call included; the
  result-class condition and the address comparison removed from
  `markWeakRoots` (`WeakRoots`); the call removed from
  `Runtime::markWeakRoots` (`CollectEmpties`).
- [ ] **Step 6: Run the full suite in `cmake-build-x86jit`**
  (`cmake --build cmake-build-x86jit --target check-hermes`): no new
  failures against the same suite at the parent commit.
- [ ] **Step 7: Commit** (`VM: add the computed property cache`).

### Task 2: Get and has

**Files:**
- Modify: `lib/VM/JSObject.cpp:1279-1325`
  (`getComputedWithReceiver_RJS`)
- Modify: `lib/VM/JSLib/Object.cpp:1753-1825`
- Test: `unittests/VMRuntime/ComputedPropertyCacheTest.cpp`

**Interfaces:**
- Consumes: Task 1's `nameOfKey`, `lookup`, `recordSlot`,
  `Runtime::getComputedPropertyCache()`.
- Produces: nothing new for later tasks; a file-local helper in the test
  file that Tasks 3 and 4 reuse:

```cpp
/// Compile \p source, an expression evaluating to a function, and
/// \return the function. Call it before planting anything.
Handle<Callable> compileFn(Runtime &runtime, const char *source);
```

- Get fast path, the first statement of `getComputedWithReceiver_RJS`:
  if `nameOfKey(*nameValHandle)` has a value and `lookup(selfHandle->
  getClassGCPtr(), name)` is a `Slot` entry, return
  `getNamedSlotValueUnsafe(*selfHandle, runtime, slot).unboxToHV(runtime)`
  as a pseudo-handle.
- Get fill, in the data-property branch at `:1320-1325` before the
  return: if `propObj.get() == selfHandle.get()`,
  `recordSlot(runtime, selfHandle->getClass(runtime), *nameValHandle,
  desc.flags, desc.slot)`. `nameValHandle`, not the converted handle: an
  object key is then not a string and records nothing.
- Has: a file-static
  `bool hasOwnCached(Runtime &runtime, HermesValue obj, HermesValue key)`
  in `Object.cpp` (object, name, `Slot` entry for its class), called
  first in `objectPrototypeHasOwnProperty` with
  `(args.getThisArg(), args.getArg(0))` and in `objectHasOwn` with
  `(args.getArg(0), args.getArg(1))`; true returns
  `HermesValue::encodeBoolValue(true)`.
- Has fill, in `objectHasOwnHelper` when `*hasProp`:
  `recordSlot(runtime, O->getClass(runtime), *P, desc.flags, desc.slot)`.

- [ ] **Step 1: Write the tests** (v1, v2 are two existing objects;
  `o` is `makeObject(proto, {a, b}, v1)` with `b` then set to `v2` by
  `putNamed_RJS`):
  - `GetFillsAndHits` [1]: `getComputed_RJS(o, keyOf(a))` returns `v1`;
    `lookup(o's class, a)` is `Slot`, slot 0, writable. Then
    `insertSlot(class, a, 1, true)`; get `a` returns `v2`.
  - `GetNotRecorded` [6], one sub-case each, asserting the generic
    result and `lookup(the object's class, name) == nullptr`: a property
    on the prototype; an own property of a dictionary object (made by
    adding and deleting a property); `length` of an array; `a` on an
    object that also has a property named `"0"`.
  - `GetNonUniquedKey` [9]: a string from
    `StringPrimitive::create(runtime, createASCIIRef("a"))`
    (`createASCIIRef` leaves out the terminator; `ASCIIRef("a")` would
    not), asserted `!isUniqued()` before each probe, reads `v1`; with
    `(class, a)` planted at slot 1 it still reads `v1`.
  - `HasOwn` [14]: `hasOwnProperty.call(o, k)` and `Object.hasOwn(o, k)`
    through two functions from `compileFn`, called with
    `Callable::executeCall2`: true, and a `Slot` entry afterwards (after
    `clear()` before each). Planted: `insertSlot(class, zz, 0, true)` for
    a name `zz` the object lacks; if `lookup(class, zz)` is still
    non-null after the call, the call must have returned true.
  - `HasNotRecorded` [6]: `Object.hasOwn` on an object whose `a` is an
    accessor (defined in JS) returns true and leaves no entry;
    `Object.hasOwn` on `new Proxy(target, {})` (no trap) whose target has
    `a` returns true and leaves no entry for the proxy's class.
  - `TypedObjectGet` [15, get half]: a typed class from
    `HiddenClass::createForTypedObject(runtime, nullParent, 2)` with two
    public properties `a` and `b` added by `addNewTypedPublicProperty`
    before the object is created
    (`JitChainWalkTest.cpp:66-79` shows the calls), the object from
    `JSObject::create(runtime, nullParent, clazz)`, and its two slots
    initialized with `v1` and `v2` by `JSObject::setNamedSlotValueUnsafe`
    (typed properties are read-only to `putNamed_RJS`). Get `a` returns
    `v1` and leaves an entry with `writable` clear; with `(class, a)`
    planted at `b`'s slot the get returns `v2`.
- [ ] **Step 2: Run: the new tests FAIL** (no entry after the get).
- [ ] **Step 3: Implement** the two fast paths and the two fills.
- [ ] **Step 4: Run the new tests and Task 1's: PASS.**
- [ ] **Step 5: Prove each test can fail.** Mutations: get fill removed;
  get fast path removed; the "found on the object itself" test removed;
  `dictionaryMode`, `indexedStorage`, `hasIndexLikeProperties`,
  `proxyObject` removed from the predicate in turn; the `accessor` test
  removed from `recordSlot`; the uniqued test removed from `nameOfKey`
  (assertion in `getUniqueID`); has fill removed; each has fast-path
  call removed.
- [ ] **Step 6: Full suite in `cmake-build-x86jit`: no new failures.**
- [ ] **Step 7: Commit** (`VM: cache computed gets and hasOwnProperty`).

### Task 3: Put to an existing property

**Files:**
- Modify: `lib/VM/JSObject.cpp:1691-1768`
  (`putComputedWithReceiver_RJS`)
- Test: `unittests/VMRuntime/ComputedPropertyCacheTest.cpp`

**Interfaces:**
- Consumes: Task 1's API.
- Produces: the put fast-path block that Task 4 extends with the `Add`
  kind.
- Fast path, after the `mustExist` assert and before the numeric fast
  path: the order in Global Constraints. For a `Slot` entry with
  `writable`: `setNamedSlotValueUnsafe(self, runtime, slot, shv)`,
  return true. A `Slot` entry without `writable` falls through to the
  generic code.
- Fill, after the in-place store succeeds at `:1755-1768`:
  `recordSlot(runtime, selfHandle->getClass(runtime), *nameValHandle,
  desc.flags, desc.slot)`.

- [ ] **Step 1: Write the tests:**
  - `PutFillsAndHits` [2]: `putComputed_RJS(o, keyOf(a), v2)` stores and
    leaves a `Slot` entry (slot 0, writable). Planted
    `insertSlot(class, a, 1, true)`: a put of `a` changes `b` and leaves
    `a`. Planted `insertSlot(class, a, 1, false)`: a put of `a` changes
    `a` and leaves `b`.
  - `PutReceiver` [8]: with the redirecting entry planted,
    `putComputedWithReceiver_RJS(o, keyOf(a), v2, otherObject)` leaves
    `o`'s `b` alone.
  - `PutNonUniquedKey` [9]: a non-uniqued `"a"` string as key stores to
    `a` and does not follow a planted redirect.
  - `TypedObjectPut` [15, put half]: on the typed object of Task 2, a
    put of the public property returns false (no `throwOnError`) and the
    value is unchanged, before and after the get has filled the entry.
  - `ReadOnlyPut`: an object whose `a` was made read-only by
    `Object.defineProperty` in JS: get fills an entry with `writable`
    clear; a put returns false and leaves the value.
- [ ] **Step 2: Run: FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run all `ComputedPropertyCache*` tests: PASS.**
- [ ] **Step 5: Prove each test can fail.** Mutations: fill removed;
  fast path removed; `writable` ignored; receiver test removed; the
  uniqued test taken before the encode is NOT a mutation here (Task 4).
- [ ] **Step 6: Full suite in `cmake-build-x86jit`: no new failures.**
- [ ] **Step 7: Commit** (`VM: cache computed puts to existing
  properties`).

### Task 4: Put that adds

**Files:**
- Modify: `lib/VM/JSObject.cpp` (`putComputedWithReceiver_RJS` fast path
  and `:1975-1985`; `tryCacheAddProperty` `:3032-3114`)
- Modify: `include/hermes/VM/JSObject.h` (declarations next to
  `tryCacheAddProperty`, `:1528-1550`)
- Test: `unittests/VMRuntime/ComputedPropertyCacheTest.cpp`

**Interfaces:**
- Consumes: Task 3's fast-path block; Task 1's `insertAdd`.
- Produces (private static members of `JSObject`):

```cpp
/// \return false if an object on \p self's prototype chain is a proxy,
/// a host object or a dictionary: the parent epoch cannot track those.
static bool parentsAllowEpochCaching(JSObject *self, PointerBase &base);
/// Mark every object on \p self's prototype chain with
/// setCachedUsingEpoch. May allocate.
static void markParentsCachedUsingEpoch(Handle<JSObject> self,
                                        Runtime &runtime);
/// addOwnProperty with the default new-property flags, then record the
/// add in the computed property cache.
static CallResult<bool> addOwnPropertyAndCacheComputed(
    Handle<JSObject> selfHandle, Runtime &runtime, SymbolID name,
    Handle<> nameValHandle, Handle<> valueHandle, PropOpFlags opFlags);
/// Record that adding \p nameValHandle's name to \p startClazz gave
/// \p self its current class. Does nothing if the add is not cacheable.
static void tryCacheComputedAdd(Handle<JSObject> self, Runtime &runtime,
                                Handle<> nameValHandle,
                                Handle<HiddenClass> startClazz);
```

- `tryCacheAddProperty` keeps its behavior and now calls the first two
  helpers in place of its two loops (`:3064-3071`, `:3104-3113`).
- At `:1975-1985`: if `selfHandle`'s raw value equals `receiver`'s and
  `nameValHandle->isString()`, return `addOwnPropertyAndCacheComputed`;
  otherwise `addOwnProperty` as today.
- `addOwnPropertyAndCacheComputed` pins the receiver's class in a Locals
  `PinnedValue<HiddenClass>` before `addOwnProperty`; on a true result
  calls `tryCacheComputedAdd`.
- `tryCacheComputedAdd`, under `NoAllocScope`: name from the key (none:
  return); result = `self`'s class; return if result equals the start,
  or either is not cacheable, or `!parentsAllowEpochCaching`; slot =
  `startClazz->getNumProperties()`; assert the result has one more
  property, `HiddenClass::debugIsPropertyDefined(result, runtime,
  name)` and not `debugIsPropertyDefined(start, runtime, name)`;
  `insertAdd` with
  `runtime.getFullParentCacheEpoch()`. After the scope:
  `markParentsCachedUsingEpoch`.
- Fast path, `Add` kind: if
  `entry->parentEpoch == runtime.getFullParentCacheEpoch()`,
  `addNewOwnPropertyInSlot(self, runtime,
  entry->resultClazz.getNonNull(runtime, runtime.getHeap()),
  entry->slot, shv)`, return true.

- [ ] **Step 1: Write the tests** (`proto` is a plain object used as the
  prototype of the test objects; `fresh()` is
  `JSObject::create(runtime, proto)`):
  - `AddFillsAndHits` [3]: objects #1, #2, #3 from `fresh()` and `ob`
    (a `fresh()` object given `b` by `putNamed_RJS`) exist first, and
    `premark(proto)` has run.
    Put `a` on #1: `lookup(#2's class, a)` is `Add`, `resultClazz`
    equals #1's class, slot 0,
    `parentEpoch == getFullParentCacheEpoch()`. Put `a` on #2: its class
    equals #1's, the value reads back. Planted:
    `insertAdd(#3's class, a, ob's class, 0, current epoch)`; a put of
    `a` on #3 gives #3 `ob`'s class.
  - `StaleEpochNotFollowed` [5]: these exist first: #4 from `fresh()`,
    `ob` (a `fresh()` object given `b` by `putNamed_RJS`) and `oa` (one
    given `a` the same way). Planted: `insertAdd(#4's class, a, ob's
    class, 0, current epoch - 1)`; assert `lookup` returns it and its
    epoch differs from the current one; a put of `a` on #4 gives #4
    `oa`'s class, not `ob`'s. This is the test
    the epoch mutation must fail: it has no allocation between the plant
    and the probe, so a moving collector cannot hide the mutation.
  - `AddAcrossStorage` [4]: `premark(proto)` first. Objects A and B from
    `fresh()`, names `p0`..`p19`, value an existing object. For each i:
    put on A; then, with no allocation in between, assert
    `lookup(B's class, p_i)` is `Add` with `resultClazz` equal to A's
    class and the current epoch; put on B; assert B's class equals A's
    and `getComputed_RJS(B, p_i)` is the value.
  - `AddEpoch` [5], results only: after `a` is cached as an add for
    `fresh()` objects: (i) a setter for `a` defined on `proto` in JS is
    called by the next put on a `fresh()` object and the object has no
    own `a`; (ii) with a second prototype, a read-only `a`: the put
    returns false and adds nothing, and with
    `PropOpFlags().plusThrowOnError()` it returns an exception
    (TypeError); (iii) `proto`'s own prototype replaced
    (`JSObject::setParent`) by an object with a setter for `a`: as (i).
    Each on its own prototype object so that the cases do not disturb
    each other. Under Hades, where nothing moves, these also fail the
    epoch mutation; under the moving collectors only
    `StaleEpochNotFollowed` is required to.
  - `AddNotRecorded` [6]: an object with 64 properties adding the 65th:
    no entry for (its start class, name), and the object's class is a
    dictionary; with 63 adding the 64th, after `premark`: an `Add`
    entry. A put that adds
    to an object whose prototype is a dictionary object: no entry. The
    same with a HostObject as prototype: the add happens
    (`JSObject.cpp:1137-1142` reports a writable property, so the
    generic put falls through to the add), no entry.
  - `SymbolDiesWhileStringLives`: the assumption `AddNameCollected`
    rests on. In an inner GCScope get the SymbolID of a name no object
    uses and its string; keep only the string in a pinned value and
    leave the scope; `runtime.collect("test")`; the string is not
    uniqued.
  - `AddNameCollected` [10]: the receiver from `fresh()` and the number
    value (`encodeTrustedNumberValue(0.5)`) exist first. Then, as above,
    a uniqued string whose SymbolID nothing else holds, kept in a pinned
    value, with no allocation between leaving the inner scope and the
    put. The put adds the property and it reads back through a get with
    the same string.
  - `CollectRefills` [12, second half]: after `CollectEmpties`'s
    sequence and `premark(proto)`, puts of the same names on objects
    created beforehand succeed and leave entries.
- [ ] **Step 2: Run: the new tests FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run all `ComputedPropertyCache*` tests in
  `cmake-build-x86jit`: PASS.**
- [ ] **Step 5: Configure and build the Handle-SAN BOXED tree** and run
  the new tests in it and in `cmake-build-x86jit-hsan`,
  `cmake-build-x86jit-malloc`, `-hv32`, `-boxed`: PASS.

```bash
cmake -S . -B cmake-build-x86jit-hsan-boxed -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-O1 -DCMAKE_C_FLAGS=-O1 \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=ON -DHERMESVM_ALLOW_JIT=2 \
  -DHERMESVM_GCKIND=MALLOC -DHERMESVM_SANITIZE_HANDLES=ON \
  -DHERMESVM_HEAP_HV_MODE=HEAP_HV_BOXED
cmake --build cmake-build-x86jit-hsan-boxed --target HermesVMRuntimeTests hermes
```

  If a test other than this plan's fails in a tree only because of that
  tree's configuration, show that it fails the same way at the parent
  commit and report it; do not change it.
- [ ] **Step 6: Prove each test can fail.** Mutations: add fill removed;
  `Add` fast path removed; the hit stores to `slot + 1`; the class
  update skipped (store without `addNewOwnPropertyInSlot`'s class
  change, e.g. `setNamedSlotValueUnsafe`); epoch comparison removed
  (`StaleEpochNotFollowed`);
  `dictionaryMode` removed from the predicate (65th property);
  the dictionary test and the host-object test removed from
  `parentsAllowEpochCaching` in turn; in the Handle-SAN BOXED tree, the
  uniqued-bit test moved before the encode with `getUniqueID` left
  after it (`AddNameCollected` must fail there on the `getUniqueID`
  assertion; in the other trees the encode does not collect and the
  mutation is not expected to show).
- [ ] **Step 7: Full suite in `cmake-build-x86jit`: no new failures**
  (`test/hermes/add-property-cache.js` and
  `prototype-property-cache.js` cover the refactored
  `tryCacheAddProperty`).
- [ ] **Step 8: Commit** (`VM: cache computed puts that add a
  property`).

### Task 5: Lit test and the test matrix

**Files:**
- Create: `test/hermes/computed-property-cache.js`
- Create: `test/jit/computed-property-cache.js`

**Interfaces:**
- Consumes: the finished feature.
- `test/hermes/computed-property-cache.js` RUN lines, as
  `test/hermes/add-property-cache.js:8-13`: `%hermes %s`, `%hermes -O0
  %s`, `%shermes -exec %s`, each piped to
  `%FileCheck --match-full-lines %s`.
- `test/jit/computed-property-cache.js` is a runner like
  `test/jit/loose-eq-null.js:10`:
  `// RUN: %hermes -O -Xjit=force -Xjit-crash-on-error
  %S/../hermes/computed-property-cache.js | %FileCheck
  --match-full-lines %S/../hermes/computed-property-cache.js`, followed
  by `// REQUIRES: jit` (`test/jit/loose-eq-null.js:10-11`).

- [ ] **Step 1: Write the test.** `'use strict'` file; each section
  prints a label (`CHECK-LABEL`) and then results (`CHECK-NEXT`). Every
  section runs its operation in a loop of at least 200 iterations over
  fresh objects, so that the cache is filled and hit under `-Xjit=force`
  too, and prints a summary that would differ on a wrong hit (a sum, a
  count, `JSON.stringify` of the last object).

  Keys must be uniqued strings or nothing hits: a string built at run
  time (`'p' + i`) is not uniqued by a property access
  (`IdentifierTable.cpp:329-345`). Every section therefore takes its keys
  from `Object.keys(seed)` of a seed object kept alive for the whole
  section, or from `for-in`, and reuses that array across the fresh
  receivers by index. Built strings appear only where the section says
  so, as generic-path coverage. Sections:
  1. the `jsxProd` loop itself (for-in copy with
     `hasOwnProperty.call` and the `key`/`ref` exclusion) over objects of
     three shapes;
  2. get, put, `hasOwnProperty.call`, `Object.hasOwn` on own data
     properties, with keys from `Object.keys(seed)`, and again with the
     same names built at run time from parts the compiler cannot fold:
     `k.slice(0, 1) + k.slice(1)` for each seed key `k` (a literal
     `'a' + 'b'` is folded, `InstSimplify.cpp:1012-1054`);
  3. put that adds: a seed object with 70 properties; fresh receivers
     each receive all 70 from the seed's key array (crossing the
     dictionary threshold), and every value is read back;
  4. after hits, each of these changes the outcome and must be seen: a
     setter for the name defined on the prototype (called, nothing
     added); a read-only property defined on the prototype (TypeError);
     the prototype's prototype replaced by one with a setter;
     `Object.preventExtensions` on the receiver (a store to an existing
     property still succeeds, an add throws TypeError); `Object.freeze`
     (both throw); `Object.defineProperty` making an
     own property read-only, and turning it into an accessor (getter
     called on get); `delete` followed by re-adding;
  5. prototypes: a proxy (set trap called), an array with an index-like
     and a plain name, a function not yet used as an object, an object
     with a null prototype;
  6. a proxy as receiver (get, set, `hasOwnProperty.call`): traps called
     every time;
  7. a target whose own property has first been stored to in a loop
     with the same key (so that its entry exists), then
     `Reflect.set(target, key, v, receiver)` with a different receiver:
     the target's property is unchanged and the receiver gets the
     property; `Reflect.get` with a receiver;
  8. Review Focus 1-5: non-string keys (number, Symbol, `new
     String('a')`, an object with `toString`); primitive receivers
     (`(5)[k]`, `'abc'[k]`, `true[k]` with `k` a method name and
     `'length'`); 3000 objects each with a distinct property name, then
     all read back; adds and stores of `i + 0.1`; `'__proto__'` as key
     on an ordinary object and on `Object.create(null)`.
- [ ] **Step 2: Generate the CHECK lines from the interpreter at the
  commit BEFORE Task 1**, so that the expected output is the generic
  path's, not the cache's: extract `git archive fc6322e0e` to
  `$S/base-src`, configure `$S/base-hv64` from it with the options of
  `cmake-build-jit2` (read them from its `CMakeCache.txt`: Release,
  clang, `HERMESVM_ALLOW_JIT=2`), build `hermes hermesc`, and run
  `$S/base-hv64/bin/hermes` on the test. Task 6 reuses this tree. Then
  run the test in `cmake-build-x86jit`: PASS in all four RUN
  configurations.
- [ ] **Step 3: Prove the test can fail:** with the epoch comparison
  removed, section 4 fails; with the receiver test removed, section 7
  fails; with the `Add` hit storing to `slot + 1`, section 3 fails.
  Restore, rebuild, re-run green. (No JS-visible counterexample is known
  for the `hasIndexLikeProperties` exclusion; `GetNotRecorded` in Task 2
  is its test.)
- [ ] **Step 4: The matrix.** Full `check-hermes` in
  `cmake-build-x86jit`, `-hv32`, `-boxed`. The new unit tests and
  `LIT_FILTER='computed-property-cache'` in `cmake-build-x86jit-malloc`,
  `-hsan`, `-hsan-boxed`. Then arm64: rebuild the host compilers
  (`cmake --build cmake-build-host --target hermesc shermes`), build
  `hermes` and `HermesVMRuntimeTests` in `cmake-build-arm64` and
  `cmake-build-arm64-hv32`, and in both run
  `qemu-aarch64-static -L /usr/aarch64-linux-gnu
  <tree>/unittests/VMRuntime/HermesVMRuntimeTests
  --gtest_filter='ComputedPropertyCache*'`, the JIT runner
  (`LIT_FILTER='jit/computed-property-cache'`), and the interpreter by
  hand (`qemu-aarch64-static -L /usr/aarch64-linux-gnu <tree>/bin/hermes
  test/hermes/computed-property-cache.js` piped to the host
  `cmake-build-x86jit/bin/FileCheck --match-full-lines` with the test
  file). The `%shermes -exec` line is not run on arm64: the cross trees'
  shermes drives the host C compiler.
  Record every count. A failure outside this plan's tests is compared
  with the parent commit and reported.
- [ ] **Step 5: Commit** (`VM: test the computed property cache from
  JS`).

### Task 6: Measure and record

**Files:**
- Modify: `doc/superpowers/specs/2026-11-11-computed-property-cache-design.md`
  (append `## Delivered`)
- Modify: `dz/issues/01a1cdf5-7332-72b5-8fca-1a5efa5088c1.md` (via `dz`)
- Modify: `dz/issues/01a13c62-a0aa-7028-8a45-782974de9273.md` (tracker)

**Interfaces:**
- Consumes: the finished feature at HEAD; the base is `fc6322e0e`.

- [ ] **Step 1: Build before and after, Release.** Before:
  `$S/base-hv64` from Task 5, and `$S/base-hv32` configured from the
  same `$S/base-src` with the options of `cmake-build-jit2-hv32`
  (`HEAP_HV_PREFER32`); build `hermes hermesc`. After:
  `cmake --build cmake-build-jit2 --target hermes hermesc`, the same for
  `cmake-build-jit2-hv32`.
- [ ] **Step 2: React.** A script in `$S` modelled on
  `/tmp/claude-1000/react-bench/bench.sh`: benchmarks
  `pixel-grid-small`, `pixel-grid.react-native-forget`,
  `draw.react-native-forget`,
  `relay-feed-wrapperized.react-native-forget` from
  `/home/tmikov/work/react-benchmarks`; configurations base and head, HV64
  and HV32, each with `-Xjit` and with `-Xjit=off`; bytecode from each
  build's own `hermesc -O -emit-binary`; `taskset -c 2`; 7 repetitions
  (3 for `pixel-grid.react-native-forget`) with the configuration order
  rotated by the repetition; medians and ratios head/base.
- [ ] **Step 3: Octane.** The same for `benchmarks/octane/*.js` without
  `regexp`, `mandreel_latency` and `splay_latency` (the 13 of
  `/tmp/claude-1000/-home-tmikov-work-hermes-x86-jit/d7daed21-e94e-4375-aaac-240004bf2ed9/scratchpad/hv32impl/bench.sh`),
  `-Xjit`, 5 repetitions. Any benchmark more than 1.5% slower is
  re-measured with 9 repetitions; if it holds, stop and report it with
  a profile instead of continuing.
- [ ] **Step 4: Profile** pixel-grid-small at HEAD as in dz
  01a1cdf5-7332 ("What is left per operation"): a Release tree with
  `-DCMAKE_CXX_FLAGS="-DHERMES_ENABLE_PERF_PROF
  -fno-omit-frame-pointer"`, `perf record -k 1 -F 4000 -g` with
  `-Xperf-prof`, two runs, samples under the `putByValLoose`, `Call2`
  and `getByVal` sites of `jsxProd`
  (`/tmp/claude-1000/react-bench/protoprof.sh` and `attr3.py` are the
  scripts used for the prototype).
- [ ] **Step 5: Check the criteria** (spec "Goal and success criteria"
  1, 2, 4). If criterion 1 is missed, stop and report the numbers and
  the profile; do not tune.
- [ ] **Step 6: Record.** Append `## Delivered (<date>)` to the spec:
  the commit, the React table (base, head, ratio, per mode, JIT and
  interpreter), the Octane table, the profile's three sample counts
  against the prototype's 401/346/136, the test matrix counts from Task
  5, and any difference from the spec. Add a log comment to dz
  01a1cdf5-7332 with the React ratios and close it
  (`dz close 01a1cdf5 --as fixed -m -`). In the tracker's "Status"
  section add one line under a heading "React benchmarks" with the
  ratios and the issue id.
- [ ] **Step 7: Commit** the spec and the two issue files by exact path
  (`Record the computed property cache results`).

## Spec resolutions

- The header is split in two (`ComputedPropertyCache.h` and
  `-inline.h`) because `Runtime.h` cannot include `HiddenClass.h`.
- `recordSlot` takes the class, the key, the property flags and the
  slot, not the object and the descriptor.
- The insertion functions that tests plant with are `insertSlot` and
  `insertAdd`.
- Test 12 is split: emptying by a collection in Task 1, refilling in
  Task 4. Test 15 is split between Tasks 2 and 3. Test 13 inserts
  directly instead of going through gets.
- Unit test 5 defines its setters in JS (`compileFn`), since a
  `PropertyAccessor` built by hand adds nothing. Its mutation promise is
  carried by `StaleEpochNotFollowed`, which plants a stale entry, so
  that it holds under the moving collectors too.
- The unit-test fixture sets the sanitize rate to 1.0.
- `TwoWays` uses fabricated SymbolIDs: it tests the table alone.
- `SymbolDiesWhileStringLives` is added to pin the assumption of test
  10.
- On arm64 the lit test runs through the JIT runner and the interpreter
  only.
- The Handle-SAN BOXED tree is `cmake-build-x86jit-hsan-boxed`, created
  in Task 4.
