---
name: gc-safe-coding
description: >
  Rules for writing and reviewing GC-safe C++ code in the Hermes VM runtime.
  Use when writing, modifying, or reviewing C++ runtime VM code that uses
  internal Hermes VM APIs (as opposed to code using JSI). This includes working
  with GC-managed types (HermesValue, Handle, PinnedValue, JSObject,
  StringPrimitive, etc.), Locals, PseudoHandle, CallResult, or any
  function with _RJS suffix. Typically in lib/VM/, include/hermes/VM/,
  API/hermes/, or API/napi/.
---

For the full explanation and rationale, see [doc/GCSafeCoding.md](doc/GCSafeCoding.md).

## GC safepoints

A GC safepoint is either a GC heap allocation or a function call that might
transitively reach one (regular C heap allocations like `malloc` are not
safepoints). Any function that takes `Runtime &` or `PointerBase &`
may trigger GC, unless documented otherwise or named with `_noalloc`/`_nogc`.
Functions with `_RJS` suffix invoke JavaScript recursively and always trigger
GC.

**All raw pointers and PseudoHandles to GC objects must be rooted before any
GC safepoint.** `PseudoHandle<T>` is *not* a root — it is just as dangerous as
a raw pointer across a safepoint. The same applies to bare `SymbolID` values
extracted from a non-uniqued source (e.g., the `SymbolID` pulled out of the
slot filled by `getSymbolFromPrimitive` for a freshly-allocated
`StringPrimitive`): once nothing roots it, the lookup-table slot is reclaimed
by `freeUnmarkedSymbols` during sweep. Pin via `PinnedValue<SymbolID>`.

## Rooting local values: use Locals + PinnedValue (required for new code)

All code uses `Locals` + `PinnedValue<T>`. `GCScope`, `makeHandle()`,
`makeMutableHandle()` and the `Handle<>`/`MutableHandle<>` constructors that
allocated a slot no longer exist. `Handle<T>` is only a view of a pinned slot.
One `Locals` per function, declared at function scope (a lambda may have its
own; this is production code guidance, a block-scoped Locals inside a test
macro expansion is fine); put it after cheap fast-path returns, or in an
out-of-line `NOINLINE` slow helper, so hot paths do not pay for it.

```cpp
struct : public Locals {
  PinnedValue<JSObject> obj;
  PinnedValue<StringPrimitive> str;
  PinnedValue<SymbolID> sym;
  PinnedValue<> genericValue;
} lv;
LocalsRAII lraii(runtime, &lv);
```

### Assignment patterns

- **From PseudoHandle:** `lv.obj = std::move(*callResult);`
- **From HermesValue with known type:** `lv.obj.castAndSetHermesValue<JSObject>(hv);`
- **From raw pointer:** `lv.obj = somePtr;`
- **Clear:** `lv.obj = nullptr;`
- **In template context:** `lv.obj.template castAndSetHermesValue<T>(hv);`

### Passing to functions

`PinnedValue<T>` implicitly converts to `Handle<T>` and to `MutableHandle<T>`.
Pass directly to functions that accept them. Where an overload is ambiguous
or for `vmcast`/`dyn_vmcast`, wrap it: `Handle<T>(lv.x)` or
`Handle<T>::vmcast(&lv.x)`. `PinnedValue` has no `operator bool`; use `*lv.x`.

### Functions that produce a GC value

A function must never return a `Handle<T>` that points into its own `Locals`.
Return `CallResult<PseudoHandle<T>>` (or `PseudoHandle<T>` if it cannot fail)
and let the caller pin it immediately (`lv.x = std::move(*res);`), or take a
`MutableHandle<T> out` parameter that the caller points at a `PinnedValue` in
its own `Locals`. If `out` may alias an input handle, write it only at the end
of each path.

## Error handling with CallResult

Always check for exceptions before using the value:

```cpp
auto result = someOperation_RJS(runtime, args);
if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
  return ExecutionStatus::EXCEPTION;
lv.obj = std::move(*result);
```

## When Handle usage is fine (do not flag)

`Handle<>` is a view of an existing rooted slot. The following are fine:

- **`vmcast<>(handle)`** — casts an existing handle to a different type. The
  result points to the same `PinnedHermesValue` as the input.
- **`args.getArgHandle(n)`** — returns a handle pointing into the register
  stack, which is already a root.
- **Passing or receiving a `Handle<>` parameter** — the caller owns the slot.
- **`Handle<T>{runtime.someRoot}` / `Handle<>(&pinnedHV)`** — views of a
  runtime field or an already pinned value.

A `Handle` must never outlive the slot it views.

## Checklist for writing / reviewing GC-safe code

1. **No raw pointers or PseudoHandles across GC safepoints.** Every pointer to
   a GC object — including values held in `PseudoHandle<T>` — must be stored in
   a `PinnedValue` before any call that takes `Runtime &` or is `_RJS`.
   Watch for multi-step creation patterns: if `Foo::create()` returns a
   `PseudoHandle` and the next line calls `Bar::create(runtime)`, the first
   `PseudoHandle` is stale after the second allocation.
   Equally watch for capture-via-deref: `auto *x = vmcast<T>(*pinned)` extracts
   a raw pointer from a pinned location (e.g., a `PinnedHermesValue *` such as
   a `napi_value`). The pinned slot stays GC-safe, but the local raw pointer
   does not. Re-deref `*pinned` at each use site, or pin via `PinnedValue<T>`.
2. **Root with Locals.** Declare a `struct : public Locals` with `PinnedValue`
   fields and a `LocalsRAII`. `GCScope` and `makeHandle()` do not exist.
3. **Check every CallResult.** Never dereference a `CallResult` without first
   checking `== ExecutionStatus::EXCEPTION`.
4. **Never return Handle from local roots.** Do not return `Handle<T>` pointing
   into a `PinnedValue` that is about to be destroyed. Return
   `CallResult<PseudoHandle<T>>`, `CallResult<HermesValue>`, or write into a
   caller-supplied `MutableHandle<T>`.
5. **Null prototype checks.** When traversing prototype chains, check for null
   before calling `castAndSetHermesValue`.
6. **Loops are safe with Locals.** `PinnedValue` fields are reused each
   iteration — no unbounded growth.
7. **Native backend (`StaticH.cpp`, `StaticHUnit.cpp`): no `LocalsRAII` may
   be live across ANY call that can reach `_sh_throw_current`, including
   transitively through helpers (e.g. `getById_RJS`, `putById_RJS`, the
   getByVal helpers).** It `longjmp`s, skipping destructors, and the catch
   side restores only `shLocals`, so `runtime.vmLocals` would be left
   dangling. Split such a helper into a non-throwing core returning a status
   plus a throwing wrapper; call the core inside the Locals scope, record the
   failure in a `bool`, end the block, then throw (or return from inside the
   block on success and fall through to the throw after it).
8. **A `PseudoHandle` or raw pointer extracted from a pinned slot is stale
   after the next safepoint.** Re-read the slot, or pin the new value.

## Debugging tips

- If `IdentifierTable::materializeLazyIdentifier` asserts
  `(entry.isLazyASCII() || entry.isLazyUTF16()) && "identifier is not lazy"`,
  the entry is most often a free-list slot — look up the call stack for an
  unrooted `SymbolID` held across an allocation.
