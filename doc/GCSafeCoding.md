# C++ Coding With GC

Hermes is a JavaScript engine with a garbage collector (GC). Various entities like
JS objects, hidden classes, property maps, and so on are allocated in the GC heap
and their lifetime is managed by the garbage collector — they can be freed when the
GC discovers that they are no longer referenced. Additionally, the GC can
automatically move GC-managed entities for heap compaction or other reasons.

Writing C++ code that uses GC-managed objects can be difficult because of this —
objects that the C++ code is accessing can be freed or moved. This document explains
the rules for writing correct and performant GC-safe code.


## The fundamental rule of GC-safe native code

The GC can only determine that objects are unreachable or move objects at
**GC safepoints**. Without that guarantee, it would be impossible to write GC-safe
C++ code. So, the fundamental requirement for GC-safe code is that all "live"
pointers to GC-managed entities used by the C++ code **must** be stored in locations
known to the GC before a GC safepoint, and must be reloaded from those locations
after the GC safepoint.

Storing the pointers in a location known to the GC ensures that the GC will consider
those objects reachable and will not free them. Reloading the pointers after the safe
point guarantees that if the GC moved the objects, the native code will use the
updated pointer value.

This is all very abstract. In the next sections we will talk about more concrete
patterns.

## GC safepoints

In practice, a GC safepoint is either an allocation, or a function call that might
transitively reach a GC safepoint. Not all C++ calls can reach a GC safepoint.

In the Hermes code base, we use naming conventions, parameter types, and explicit
doc-comments to communicate this. The rules are:

- A function that takes a `Runtime &` or `PointerBase &` parameter (both of which
  provide access to the GC) is assumed to be able to reach GC safepoints, unless
  explicitly documented otherwise, or named with a suffix like `_noalloc` or
  `_nogc`.
- A function with an **`_RJS`** suffix may invoke JavaScript code recursively
  ("Recursive JavaScript"), so it definitely reaches GC safepoints.
  For example: `JSObject::getNamed_RJS()`, `toString_RJS()`, `toNumber_RJS()`.
- A function that does **not** take `Runtime &` or `PointerBase &`, and is not
  `_RJS`, is generally safe to call without protecting pointers.

These rules must be followed absolutely by C++ code — all pointers to heap values
must be stored before and reloaded after. (In practice, the values simply "live" in
locations known to the GC and the C++ code loads them every time. The C++ compiler is
smart enough to optimize out consecutive loads if the underlying value couldn't have
changed.)


## GC roots vs heap values

The GC needs a starting point to discover which objects are alive. That starting
point is the set of **roots** — pointers to GC-managed objects that live *outside*
the GC heap, in locations the GC is explicitly told about. Roots include:

- The **register stack** — the VM's operand stack used by the bytecode interpreter.
- **Locals** — stack-allocated `PinnedValue<>` fields registered via `LocalsRAII`.
- **Runtime fields** — `PinnedHermesValue` fields inside the `Runtime` object
  itself, such as well-known prototypes (`arrayPrototype`, `objectPrototype`, etc.).

None of these live in the GC heap. They live on the C++ stack or in fixed locations
in the `Runtime`. The GC knows about each category and walks them all during
collection (see `Runtime::markRoots()` in `Runtime.cpp`).

Starting from these roots, the GC traces through every pointer it finds. An object
in the GC heap is alive if and only if it is **transitively reachable** from at least
one root. Any object not reachable from any root can be freed. If the GC moves an
object (for compaction), it updates every pointer it knows about — both roots and
pointers stored inside other GC heap objects.

The key insight for C++ code: **if you hold a raw pointer to a GC object and that
pointer is not stored in a root, the GC does not know about it.** The pointed-to
object could be freed (use-after-free) or moved (dangling pointer). This is why all
live pointers must be stored in roots before any GC safepoint.


## HermesValue

`HermesValue` is the fundamental value type exposed to the GC — it is a NaN-boxed
64-bit value that can contain JS primitives (numbers, booleans, undefined, null,
symbols) and, importantly, pointers to GC-managed objects.

The GC works on `HermesValue`s, using the NaN-boxing tag bits to determine whether
each value is a pointer or a primitive. When a value is a pointer, the GC traces
through it, and updates it if the pointed-to object moves.


## PinnedHermesValue

`PinnedHermesValue` is a subclass of `HermesValue`. The only purpose of the subclass
is to indicate that this `HermesValue` instance is a **GC root** — it lives in
memory that is not in the GC heap (and is not movable by the GC), and is known to
the GC as a root.

Note that declaring something as `PinnedHermesValue` does not *magically* register
it as a root. The declaration is a semantic marker: if a value is stored outside
the GC heap and is registered with the GC as a root (through `Locals` or
`Runtime` fields), it should be typed as `PinnedHermesValue` to indicate its
"root-ness".

`PinnedHermesValue` is essential for writing GC-safe code. The "locations known to
the GC" mentioned in the first section are all `PinnedHermesValue` roots.
Conceptually, C++ code stores all pointers to GC objects it needs into
`PinnedHermesValue` instances before a safepoint and reloads them after. In
practice, there are convenient C++ abstractions built on top of
`PinnedHermesValue`.


## Handle and MutableHandle

A `Handle<T>` is a wrapper around a pointer to an immutable `PinnedHermesValue`
(`const PinnedHermesValue *`), where the `PinnedHermesValue` is known to contain a
value of type `T` (a number, bool, `JSObject`, etc.). Most internal APIs accept
`Handle`s instead of direct pointers.

A `Handle<>` (i.e. `Handle<HermesValue>`) is an untyped handle that can hold any
`HermesValue`.

A `MutableHandle<T>` is similar to a `Handle<T>`, but it points to a *mutable*
`PinnedHermesValue` and allows updating the stored value.

`Handle<T>` is trivially copyable (it is just a pointer) and is designed to be
passed by value.

An important property: a `PinnedValue<T>` implicitly converts to `Handle<T>`. This
means that when a function accepts `Handle<T>`, you can pass a `PinnedValue<T>`
directly.


## PseudoHandle

`PseudoHandle<T>` holds a GC-managed value *without* protecting it via a root. It
is a move-only type — once moved from, the original is invalidated (in debug mode,
accessing an invalidated `PseudoHandle` asserts).

`PseudoHandle` exists for performance: in many cases a function produces a value and
the caller immediately stores it in a root. Wrapping the value in a `PseudoHandle`
encodes in the type system that the value is *unrooted* and must be stored somewhere
safe before any GC safepoint. Many internal APIs return
`CallResult<PseudoHandle<T>>` for this reason.

Common patterns with `PseudoHandle`:
```cpp
// Storing into a PinnedValue (moves and invalidates the PseudoHandle):
lv.obj = std::move(*result);
```


## CallResult and error handling

`CallResult<T>` is the standard return type for operations that can throw a JS
exception. It is either a value of type `T` or `ExecutionStatus::EXCEPTION`. The
typical usage pattern is:

```cpp
auto result = someOperation_RJS(runtime, args);
if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
  return ExecutionStatus::EXCEPTION;
}
// Use *result or result.getValue() to get the value.
lv.obj = std::move(*result);
```

Most functions that can trigger GC return `CallResult<PseudoHandle<T>>` or
`CallResult<HermesValue>`. The caller must check for exceptions before using the
value, and must store the value into a root before the next GC safepoint.


## Locals and PinnedValue\<T\>

**All code must use `Locals` + `PinnedValue<T>` for rooting local GC values.**
`GCScope` and `makeHandle()` no longer exist.

`Locals` with `PinnedValue<T>` is the way to create rooted storage for
local GC values. `Handle<T>` remains the standard type for passing GC values between
functions — `PinnedValue<T>` implicitly converts to `Handle<T>`, so the two work
together naturally.

### How it works

You declare an anonymous struct inheriting from `Locals`, with `PinnedValue<T>`
fields for each GC value you need to keep alive. Then create a `LocalsRAII` to
register the struct with the runtime. `LocalsRAII` pushes the struct onto a linked
list (`runtime.vmLocals`). During GC, `Runtime::markRoots()` walks this list and
marks every `PinnedHermesValue` in every `Locals` struct as a root.

```cpp
// Locals struct lives on the C++ stack. The PinnedValue fields are
// PinnedHermesValues that the GC will mark as roots.
struct : public Locals {
  PinnedValue<JSObject> obj;
  PinnedValue<StringPrimitive> str;
  PinnedValue<> genericValue;  // untyped — can hold any HermesValue
} lv;
LocalsRAII lraii(runtime, &lv);
```

### Assignment patterns

```cpp
// From a PseudoHandle (the typical case after a CallResult):
lv.obj = std::move(*callResult);

// From a CallResult with a HermesValue that you know is a specific type:
lv.obj.castAndSetHermesValue<JSObject>(callResult.getValue());

// From a raw pointer:
lv.obj = someJSObjectPtr;

// From a GCPointer:
lv.obj = someGCPointer.get(runtime);

// Clearing (so it doesn't keep an object alive unnecessarily):
lv.obj = nullptr;
```

### Passing PinnedValue where Handle is expected

`PinnedValue<T>` implicitly converts to `Handle<T>`, so you can pass it directly
to functions that accept handles:

```cpp
struct : public Locals {
  PinnedValue<JSObject> O;
} lv;
LocalsRAII lraii(runtime, &lv);
lv.O.castAndSetHermesValue<JSObject>(*objRes);

// getPrototypeOf takes Handle<JSObject>, but PinnedValue<JSObject> converts:
return getPrototypeOf(runtime, lv.O);
```

### Reading from PinnedValue

```cpp
// Get the underlying typed value:
JSObject *rawPtr = lv.obj.get();   // or *lv.obj
JSObject *rawPtr = *lv.obj;

// Dereference to access members:
lv.obj->someMethod();

// Get as HermesValue:
HermesValue hv = lv.obj.getHermesValue();
```

### Template functions

When calling `PinnedValue` member function templates like `castAndSetHermesValue<T>`
from a template context where the `PinnedValue` type is dependent, C++ requires the
`template` keyword before the member name:

```cpp
template <typename T>
void doSomething(Runtime &runtime, HermesValue value) {
  struct : public Locals {
    PinnedValue<T> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // "template" is required because PinnedValue<T> is a dependent type:
  lv.obj.template castAndSetHermesValue<T>(value);
}
```

Without the `template` keyword, the compiler parses the `<` as a less-than operator
instead of the start of a template argument list, causing a compile error. This
applies to any template member function called on a `PinnedValue` with a dependent
type parameter.

### Real-world example

From `arrayConstructor` in `Array.cpp`:

```cpp
CallResult<HermesValue> arrayConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<JSArray> self;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (LLVM_LIKELY(!args.isConstructorCall() || ...)) {
    CallResult<PseudoHandle<JSArray>> selfRes =
        JSArray::create(runtime, runtime.arrayPrototype);
    if (LLVM_UNLIKELY(selfRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    lv.self = std::move(*selfRes);
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(runtime, ...);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    lv.selfParent = std::move(*thisParentRes);
    auto arrRes = JSArray::create(runtime, lv.selfParent);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    lv.self = std::move(*arrRes);
  }

  // ...use lv.self throughout the rest of the function...
  return lv.self.getHermesValue();
}
```

### Locals and loops

Locals solves the loop problem inherently — the `PinnedValue` fields are a fixed
set, so you simply reuse them each iteration. Long-lived values stored in `lv.*`
persist across iterations.


## Common mistakes

### Holding raw pointers across GC safepoints

```cpp
// WRONG — rawPtr may dangle after the allocation:
JSObject *rawPtr = someHandle->getObject();
auto newObj = JSObject::create(runtime);  // GC safepoint!
rawPtr->doSomething();  // rawPtr may be invalid!

// CORRECT — store in a PinnedValue, reload after:
struct : public Locals {
  PinnedValue<JSObject> obj;
} lv;
LocalsRAII lraii(runtime, &lv);
lv.obj = someHandle->getObject();
auto newObj = JSObject::create(runtime);  // GC safepoint
lv.obj->doSomething();  // Safe: PinnedValue is a root
```

### Holding PseudoHandles across GC safepoints

`PseudoHandle<T>` is **not** a GC root — it holds an unrooted value just like a
raw pointer. It is equally dangerous to use a `PseudoHandle` after a GC
safepoint. This is easy to miss because `PseudoHandle` looks like a "safe" smart
pointer type, but it is not rooted.

Watch especially for **multi-step creation patterns** where two `::create()`
calls are made in sequence:

```cpp
// WRONG — ctor PseudoHandle is stale after the second create():
auto ctor = NativeConstructor::create(runtime, ...);
auto proto = JSObject::create(runtime);  // GC safepoint — ctor is stale!
lv.ctor = std::move(ctor);              // Too late — ctor is already stale!

// CORRECT — root the PseudoHandle before the next allocation:
lv.ctor = NativeConstructor::create(runtime, ...);  // Root immediately.
lv.proto = JSObject::create(runtime);   // GC safepoint — lv.ctor is safe.
```

This bug often goes undetected because the stale value is still valid most of
the time — the GC only moves objects during compaction, which is rare. Enable
`HERMESVM_SANITIZE_HANDLES` (enabled by default in ASAN builds) to catch these
bugs deterministically by moving the heap after every allocation.

### Returning a Handle from a destroyed Locals

```cpp
// WRONG — returning a Handle from a PinnedValue that is about to be destroyed:
Handle<JSObject> createThing(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.obj = ...;
  return lv.obj;  // lraii destroyed here — Handle dangles!
}
```

Instead, return a `PseudoHandle` (which copies the value out, not a pointer to the
root):

```cpp
// CORRECT — return a PseudoHandle:
CallResult<PseudoHandle<JSObject>> createThing(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.obj = ...;
  return PseudoHandle<JSObject>::create(*lv.obj);
}

// CORRECT — write into a caller-provided MutableHandle:
void createThing(Runtime &runtime, MutableHandle<JSObject> result) {
  // No local rooting needed — we write directly into the caller's root.
  auto res = JSObject::create(runtime);
  result = res.get();
}

// CORRECT — return a HermesValue (caller roots it, so risky):
CallResult<HermesValue> createThing(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.obj = ...;
  return lv.obj.getHermesValue();
}
```

### Forgetting to check for exceptions

```cpp
// WRONG — using the value without checking for exception:
auto result = someOperation_RJS(runtime, args);
lv.obj = std::move(*result);  // May dereference an exception!

// CORRECT:
auto result = someOperation_RJS(runtime, args);
if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
  return ExecutionStatus::EXCEPTION;
lv.obj = std::move(*result);
```

### Null prototype handling

When traversing prototype chains, always check for null:

```cpp
auto protoRes = JSObject::getPrototypeOf(obj, runtime);
if (LLVM_UNLIKELY(protoRes == ExecutionStatus::EXCEPTION))
  return ExecutionStatus::EXCEPTION;
if (!*protoRes) {
  lv.O = nullptr;  // End of prototype chain
} else {
  lv.O.castAndSetHermesValue<JSObject>(protoRes->getHermesValue());
}
```
