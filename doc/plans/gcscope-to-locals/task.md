# Migrating handle allocations from GCScope to Locals

Supersedes `task.md`, `addendum.md`, `handle_migration_todo.md`,
`scan_handles.py` and `check_locals_usage.py` in the repo root (those
can be deleted once this lands). Read `doc/GCSafeCoding.md` and the
`gc-safe-coding` skill first; this document only adds what is specific
to the migration.

## Goal

Remove every handle *allocation* from production runtime code, so that
`GCScope` can eventually be deleted. A handle allocation is any of:

- `runtime.makeHandle(...)`, `runtime.makeMutableHandle(...)`
- `Handle<T>{runtime, ...}` / `MutableHandle<T>{runtime, ...}` (any
  constructor taking `Runtime &`)
- a call to a function taking `Runtime &`/`PointerBase &` that returns
  `Handle<T>`, `MutableHandle<T>` or `CallResult<Handle<T>>`
  (these allocate in the topmost `GCScope`)

Replace them with one `Locals` struct per function holding
`PinnedValue<T>` fields. Not allocations, leave alone: `vmcast<>(handle)`,
`args.getArgHandle(n)`, and `Handle<>` parameters.

## Current state (measured on the working copy)

Allocation sites, by the regex in "Finding work" below:

| Area | Sites |
|---|---|
| `lib/VM/JSLib` | 9 (essentially done; the earlier JSLib pass landed) |
| `lib/VM` (excluding JSLib) | ~259 |
| `include/hermes/VM` | 26 |
| `API/napi` | 20 |
| `lib/ConsoleHost` | 5 |
| `API/hermes`, `API/hermes_abi` | 0 by this regex, but they use `GCScope` heavily (61 / 27 mentions); audit separately |
| `unittests` | ~263; **out of scope** for now |

Biggest production files: `JSObject.cpp` (54), `JSProxy.cpp` (53),
`Operations.cpp` (50), `Interpreter-slowpaths.cpp` (19), `JSError.cpp`
(12), `StaticH.cpp`, `HiddenClass.cpp`, `hermes_napi_object.cpp` (11 each).

The hard part is not the bodies, it is the API surface: ~31 declarations
in `include/hermes/VM` and ~64 uses overall of `CallResult<Handle<T>>`
(`JSObject::getOwnPropertyKeys/Names/Symbols`, `toPropertyKey`,
`toPropertyKeyIfObject`, `getPrimitivePrototype`, `valueToSymbolID`,
`stringToSymbolID`, `iteratorStep`, `iterableToArray`,
`speciesConstructor`, `symbolDescriptiveString`, `JSError::toString`,
`JSString::create`, `JSBigInt::create`, `getForInPropertyNames`,
`IdentifierTable::getSymbolHandle*`, `Interpreter::reifyArgumentsSlowPath`,
...). They cannot be converted in place (see Rule 4), so they dictate
the order of work.

Existing `GCScope`/`GCScopeMarkerRAII`/`flushToMarker` instances stay
until the allocation sites they protect are gone; removing them is a
later task, not part of this one.

## Rules

1. Never add `GCScope`, `GCScopeMarkerRAII`, or any handle allocation.
   Never move or delete existing `GCScope`s in this task.
2. One `Locals` per function, declared at function scope (never in a
   loop). Fields are reused per iteration; that is the point.
   A lambda that needs its own rooted values may have its own `Locals`.
3. Assign with the cheapest form (see `gc-safe-coding`):
   `lv.x = std::move(pseudo)`, `lv.x = ptr`,
   `lv.x.castAndSetHermesValue<T>(hv)` (use `.template` in templates).
   Never `lv.x = vmcast<T>(hv)` for a `PinnedValue<T>`.
4. **A function must never return a `Handle<T>`/`MutableHandle<T>`/
   `CallResult<Handle<T>>` that points into its own `Locals`.** Once the
   allocation is removed the handle would dangle. Signature changes are
   required for such functions; see "Changing handle-returning APIs".
5. Pass `PinnedValue<T>` straight to `Handle<T>` parameters; do not wrap
   in `Handle<T>{lv.x}`. Compare/inspect through
   `lv.x.getHermesValue()` rather than constructing temporaries.
6. When a callee takes `MutableHandle<T>` or `MutableHandle<T> &`, pass
   a `MutableHandle<T>{&lv.x}`-style view of the pinned slot as the
   existing code does; do not allocate a new one.
7. Check every `CallResult`. Don't hold `PseudoHandle`/raw pointers
   across any `Runtime &` call; pin first.
8. Variable-length handle storage (vectors of `MutableHandle`) must be
   replaced with a GC-managed `ArrayStorage` held in a `PinnedValue`
   (precedent: `objectDefinePropertiesInternal` in `JSLib/Object.cpp`).
9. Locals must be initialised/clean at `LocalsRAII` construction
   (`HERMES_SLOW_DEBUG` asserts it); `PinnedValue` default-constructs
   to null, so just don't pre-assign.
10. **Native backend (`StaticH.cpp`, `StaticHUnit.cpp`): never call
    `_sh_throw_current` while a `LocalsRAII` is in scope.** It
    `longjmp`s, skipping destructors, and `_sh_catch_no_pop` restores
    only `shLocals`, not `vmLocals`, which would be left dangling. Record
    the failure in a `bool`, end the block, then throw.
11. Keep each commit mechanical and reviewable: no unrelated refactors.

## Changing handle-returning APIs

Decision (recommended, confirm before starting Phase 2): for each
`CallResult<Handle<T>>` function, pick by how callers use the result:

- **Default: return `CallResult<PseudoHandle<T>>`** (or
  `CallResult<HermesValue>` for untyped). Callers pin immediately:
  `lv.x = std::move(*res);`. This matches checklist item 4 in the
  skill and the existing `_RJS` convention.
- **Out-parameter `MutableHandle<T> result`** (by value, it is just a
  pointer to a pinned slot) when the callee already has the object
  pinned in a local and returning it would force a re-wrap, or when
  callers want to write directly into their own `Locals`.
- **`HermesValue` return** only for the single-call-site
  `GlobalObject.cpp` creator functions (precedent in `JSLib`).

Change the declaration, definition and **all** call sites in one commit
(grep the whole tree including `API/`, `unittests/`, `lib/ConsoleHost`).
Callers in unittests are updated minimally to keep them compiling; do
not migrate unittests' own `makeHandle`s.

## Order of work

1. **Phase 1: leaf allocations in functions that return no handles**
   (`JSError`, `JSRegExpStringIterator`, `JSCallableProxy`,
   `JSTypedArray`, `HiddenClass`, `Callable`, `IdentifierTable`,
   `StaticH*`, `Debugger`, `Interpreter*`, `ConsoleHost`, `API/napi`,
   remaining JSLib). Lowest risk; shrinks the problem.
2. **Phase 2: handle-returning APIs**, one API (or one tight family)
   per commit, bottom-up so that callees change before callers:
   `IdentifierTable::getSymbolHandle*` -> `Operations.h` -> `JSObject.h`
   -> `PrimitiveBox`/`JSError`/`Interpreter`.
3. **Phase 3: the big files** (`JSObject.cpp`, `JSProxy.cpp`,
   `Operations.cpp`), after their dependencies' signatures are final.
   These are large enough to split by function groups across commits.
4. **Phase 4: audit** `API/hermes` and `API/hermes_abi` (they use
   `GCScope` as the napi/JSI value-lifetime mechanism, so many of these
   are legitimate and out of scope; list what remains and why in the
   final report), and `JIT/arm64/JitHandlers.cpp`.

Stop and ask the user if a conversion needs a semantic change beyond
rooting (e.g. an API whose callers rely on handle lifetime being the
caller's `GCScope`).

## Finding work

Allocation sites (single line; multi-line constructions are caught by
the compiler errors when you convert, not by this):

```bash
grep -rnE '\.makeHandle\(|\.makeMutableHandle\(|(Mutable)?Handle<[^>]*>[[:space:]]*[A-Za-z_0-9]*[[:space:]]*[{(][[:space:]]*runtime' \
  lib include API --include=*.cpp --include=*.h
```

Handle-returning declarations:

```bash
grep -rnE 'CallResult<Handle<' include lib API --include=*.cpp --include=*.h
```

The old `check_locals_usage.py` is not needed: it required universal
ctags (macOS ships BSD ctags and the script silently reported success),
and the "one Locals per function" rule is checked in review and by the
compiler/ASan. Do not recreate it.

## Per-unit procedure

Work in **a fresh subagent per file (or per API family)**; the
controller only tracks the list below. For each unit:

1. Invoke `gc-safe-coding`. Read the whole function, not just the
   allocation line; list every value live across a safepoint.
2. Convert. Keep names and behaviour; no drive-by cleanups.
3. Build: `cmake --build cmake-build-asan --target hermes` (add
   `-DJSI_UNSTABLE=ON` to a fresh configure if Worker/unittest code is
   being compiled; a stale dir producing a `-Wundef` failure in
   `PlatformUnicodeCF` means recreate the build dir).
4. Run `arc f` on edited `.cpp`/`.h`.
5. Test with `LIT_OPTS="-j1" LIT_FILTER=<area> cmake --build
   cmake-build-asan --target check-hermes`, plus an ad-hoc `tmp.js`
   exercising the touched path (read `tmp.js` first, overwrite it).
   Phase 2 and 3 units additionally run the full `check-hermes` and the
   VMRuntime unittests; consider a `HERMESVM_SANITIZE_HANDLES=ON` build
   for Phase 2/3, since moving the heap on every allocation is what
   exposes stale pointers introduced by the conversion.
6. Commit using the Meta commit format (see
   `facebook/CLAUDE-meta.md`; keep descriptions short per project
   convention). Never amend or submit unless asked.

## Progress

- [x] Phase 1: leaf allocations (batches 1-4).
- [x] Phase 2: handle-returning APIs, see `phase2-inventory.md`.
- [x] Phase 3: Operations.cpp, JSProxy.cpp, JSObject.cpp.
- [x] Phase 4: audit, see `phase4-audit.md`. 12 allocating sites remain,
  all in `tools/`; every remaining GCScope use in lib/ include/ API/ is
  vestigial. Deleting GCScope is a separate follow-up task.
