# Phase 4 audit: what remains of GCScope and handle allocation

Read-only audit of the working copy after Phases 1-3 (HEAD stack
`cce78a6e216f::.`). Scope: `lib/ include/ API/ tools/`, excluding
`unittests/` and the generated `API/hermes_sandbox/external/*` blobs.
Numbers come from the greps quoted in each section.

## Headline

- **Allocating handle constructs left in production code: 12 sites,
  all in `tools/`** (`tools/test-runner/Executor.cpp` x2,
  `tools/hermes/repl.cpp` x8 incl. the 2 MutableHandle ctors,
  `tools/hvm-bench/interp-dispatch-bench.cpp` x2; see section 1 for the
  exact list). `lib/`, `include/` (outside the definitions of the APIs
  themselves) and `API/` have **zero**.
- Every API that still returns a `Handle<T>`/`CallResult<Handle<T>>`/
  `MutableHandle<T>` in production code returns a *view* (of a
  parameter, a runtime root, or a pinned slot), not an allocation.
  Rule 4 is not violated anywhere I could find.
- Consequence: **every `GCScope`, `GCScopeMarkerRAII`,
  `flushToMarker`/`flushToSmallCount`/`marker.flush()` in `lib/`,
  `include/` and `API/` is vestigial** (class (a)). Counts (by regex on
  non-comment lines; `/tmp` script, see section 2): 301 `GCScope`
  declarations, 189 `GCScopeMarkerRAII` declarations, ~248 flush/marker
  operations, 5 `NoLeakHandleScope`, 11 `NoHandleScope`. Only `tools/`
  (4 `GCScope`, 2 marker, 1 flush) is class (b). API/hermes and
  API/hermes_abi (class (c) in the brief) turn out to be (a):
  JSI/ABI value lifetime is *not* built on GCScope.
- No rule-10 (longjmp past LocalsRAII) violation found. Two
  `GCScopeMarkerRAII` are live across `_sh_throw_current` (harmless
  leaks of already-vestigial markers, see section 4).
- Not deletable mechanically, though: a few debug-only checks and the
  root-marking section depend on GCScope (section 3).

## 1. Remaining allocating handle constructs

### 1.1 API surface that allocates a slot in the topmost GCScope

From `Handle.h`, `Handle-inline.h`, `HandleRootOwner.h`,
`HandleRootOwner-inline.h`, `Runtime.h`:

| API | file:line |
|---|---|
| `HandleRootOwner::makeHandle(HermesValue)` / `<T>(T*)` / `<T>(HermesValue)` / `(SymbolID)` / `(PseudoHandle<T>&&)` / `<T>(PseudoHandle<HermesValue>&&)` | HandleRootOwner.h:51-71, -inline.h:17-45 |
| `HandleRootOwner::makeMutableHandle(HermesValue)` / `(T*)` | HandleRootOwner.h:74-77, -inline.h:47-54 |
| `Runtime::makeHandle(const GCPointer<T>&)` / `makeMutableHandle(const GCPointer<T>&)` | Runtime.h:422-426, 2296-2304 |
| `HandleBase(HandleRootOwner&, HermesValue)` (public) | Handle.h:364, Handle-inline.h:36 |
| `Handle<T>(HandleRootOwner&, value_type)` (protected; friends `HandleRootOwner`, `Runtime`) and `Handle<T>(GCScope*, value_type)` | Handle.h:453-456 |
| `Handle<T>(HandleRootOwner&, HermesValue, bool)` (protected) and public `Handle<T>::vmcast(HandleRootOwner&, HermesValue)` | Handle.h:442, 529 |
| `MutableHandle<T>(HandleRootOwner&, value = default)` (public) | Handle.h:602 |
| `HandleRootOwner::newPinnedHermesValue(...)` / `GCScope::newPinnedHermesValue` | HandleRootOwner.h:150-157, -inline.h:93-104 |

Not allocating (checked): `makeNullHandle`, `getUndefinedValue` and the
other `get*Value`, `Handle<T>::vmcast(const PinnedHermesValue*)`,
`vmcast(const PinnedValue<U>*)`, `vmcast(const Handle<U>&)`,
`Handle<T>::dyn_vmcast(Handle<U>)`, `Handle<T>{runtime.someRoot}` (view
on a `PinnedHermesValue`), `Handle<>(&stackPinnedHermesValue)`.

### 1.2 Callers of the allocating APIs

Greps run (all over `lib include API tools`, `*.cpp *.h`):

- `\.makeHandle(` `->makeHandle` bare `makeHandle(` `makeHandle<`
  `makeMutableHandle` -> only `tools/` plus the definitions.
- `(Mutable)?Handle<..> name{|(runtime|*this|rt|<anything>runtime<..>`
  -> only `tools/` plus definitions. A second sweep listing the first
  argument token of **every** `Handle<..> x(` / `{` construction in
  production code showed only PinnedHermesValue*/Handle/PinnedValue
  sources (toPHV, `&O2REG`, `lv.x`, `runtime.<root>`), i.e. views.
- `Handle<T>::vmcast(runtime, ...)`, `dyn_vmcast(runtime, ...)`: zero
  production callers (7 in `unittests/`).
- `newPinnedHermesValue`, `Handle(GCScope*, ..)`: only inside
  `Handle-inline.h`/`HandleRootOwner*`.
- Declarations returning handles with a `Runtime&`/`PointerBase&`
  parameter (multi-line signatures included via a second grep):

| function | file:line | verdict |
|---|---|---|
| `getPrimitivePrototype` (`CallResult<Handle<JSObject>>`) | Operations.cpp:760, .h:157 | views of runtime roots; no allocation |
| `speciesConstructor` (`CallResult<Handle<Callable>>`) | Operations.cpp:1951, .h:498 | returns the parameter `defaultConstructor`; view |
| `generatorValidate` | JSLib/GeneratorPrototype.cpp:76 | returns `dyn_vmcast` view of param |
| `validateUint8Array` | JSLib/TypedArray.cpp:2470 | `vmcast` view of param |
| `inferredParent` | Callable.cpp:118 | runtime roots |
| `createDebuggerInternalObject` (`Handle<JSObject>`) | JSLib/DebuggerInternal.cpp:17 | view of `runtime.debuggerInternalObject_` (only in `HERMES_ENABLE_DEBUGGER`) |
| `defineSystemConstructor` x2 | JSLib/JSLibInternal.cpp:22,102 | returns the caller's `constructorOut` (`MutableHandle` param) |
| `getErrorPrototype` | SerializedValue.cpp:413 | runtime roots |
| `StringPrimitive::ensureFlat` | StringPrimitive.h:314 | returns param |
| `Runtime::getHiddenClassForPrototype`, `Runtime::getGlobal` | Runtime-inline.h:50, Runtime.cpp:1451 | views of root slots |
| `JSTypedArrayBase::getPrototype/getConstructor` | JSTypedArray.h:447 | root views |
| `Handle<Environment>/...` in `StackFrame-inline.h:45`, `NativeArgs.h` | | stack/root views |
| `Handle<HiddenClass>` `Runtime-inline.h:58` | | root view |

All of the above are safe to keep as `Handle` returning views, but each
one is a trap if someone later "fixes" it to allocate. Their declared
return type can stay when `Handle<T>` survives as a view type.

### 1.3 Remaining allocating sites (all `tools/`)

| file:line | construct | class | suggested conversion |
|---|---|---|---|
| tools/test-runner/Executor.cpp:183 | `runtime.makeHandle(runtime.getThrownValue())` | still-to-convert (tool) | `PinnedValue<>` in a Locals, or pass `runtime.getThrownValue()` into a `PinnedHermesValue`-backed Locals |
| tools/test-runner/Executor.cpp:273-275 | `GCScope`+`GCScopeMarkerRAII`+`MutableHandle<Callable> task{runtime}` | still-to-convert | `PinnedValue<Callable> task` in Locals; drop both scopes |
| tools/test-runner/Executor.cpp:327 | `GCScope scope(*env.runtime)` | protects nothing in this extent unless the call at 340 allocates (it calls `makeNullHandle`, a view) -> vestigial | delete with the rest |
| tools/hermes/repl.cpp:335 | `GCScope` | protects 347/350/369/373/386 | delete after conversion |
| tools/hermes/repl.cpp:347, 369, 446, 470 | `runtime->makeHandle(runtime->getThrownValue())` (x4) | still-to-convert | `PinnedValue<>` |
| tools/hermes/repl.cpp:350, 373 | `makeHandle<Callable/JSFunction>(std::move(*res))` | still-to-convert | `PinnedValue<Callable>`/`PinnedValue<JSFunction>` = `std::move(*res)` |
| tools/hermes/repl.cpp:386, 459 | `MutableHandle<> resHandle{*runtime}`, `MutableHandle<Callable> task{*runtime}` | still-to-convert | `PinnedValue<>`, `PinnedValue<Callable>` |
| tools/hermes/repl.cpp:431 | `GCScopeMarkerRAII gcMarker` | protects 446-470 | delete after conversion |
| tools/hvm-bench/interp-dispatch-bench.cpp:60 | `runtime.makeHandle(Domain::create(runtime))` | still-to-convert | `PinnedValue<Domain>` |
| tools/hvm-bench/interp-dispatch-bench.cpp:164 | `return runtime.makeHandle<StringPrimitive>(*status)` | still-to-convert, **returns Handle into the topmost GCScope** (rule 4 in reverse: the benchmark relies on caller scope) | return `PseudoHandle`/`HermesValue` and let the caller pin |
| tools/hvm-bench/interp-dispatch-bench.cpp:197 | `GCScope scope(*runtime)` | protects 60/164 | delete after conversion |

Dead code: none found with `#if 0` that contains allocating constructs
(the only hits for `makeHandle` outside tools are in comments, e.g.
`Handle.h:71` doc example, `StaticH.cpp:1236` commented-out call, and
the stale `lib/VM/JSObject.cpp.orig`, which is an on-disk leftover
ignored by Sapling; delete it).

Doc/comment mentions to refresh when GCScope goes:
`API/napi/hermes_napi_internal.h:99,125` ("Caller must provide an
active GCScope") and `API/napi/hermes_napi_object.cpp:~44` ("A GCScope is
needed because putComputed_RJS internally allocates temporary
handles") are stale today.

## 2. GCScope-family uses in production code

Counting rules (script, non-comment lines): `GCScope` = a declaration
`GCScope x{..}`/`(..)`; `MarkerRAII` = `GCScopeMarkerRAII x{..}`;
`flush/marker` = `flushToMarker`, `flushToSmallCount`, `createMarker`,
`marker.flush()`; the last column lists other mentions
(infra/comment). Total 882 raw mentions.

Subtotals by area (GCScope decl / MarkerRAII / flush ops):

| area | GCScope | MarkerRAII | flush ops | class |
|---|---|---|---|---|
| lib/VM/JSLib | 95 | 57 | 139 | (a) |
| lib/VM (rest, incl. StaticH, Interpreter, Operations, JSProxy, JSObject, Runtime, ...) | 44 | 103 | 83 | (a) |
| include/ (ArrayLike.h, DictPropertyMap.h, SerializedLiteralOperations.h, JSMapImpl.h, JSObject*.h, Callable.h) | 1 | 9 | 20 | (a) |
| lib/VM/Debugger | 5 | 6 | 2 | (a) |
| lib/VM/JIT/arm64/JitHandlers.cpp | 0 | 8 | 0 | (a) |
| lib/ConsoleHost | 1 | 5 | 0 | (a) |
| API/hermes (hermes.cpp 63 + TracingRuntime 1) | 64 | 1 | 0 | (a), not (c) |
| API/hermes_abi (hermes_vtable.cpp) | 27 | 0 | 0 | (a), not (c) |
| API/napi | 64 | 0 | 11 | (a), not (c) |
| tools/ (repl, Executor, hvm-bench) | 4 | 2 | 1 | (b) |

Totals: (a) 301/189/248, (b) 4/2/1, (c) 0. Also 5 `NoLeakHandleScope`
and 11 `NoHandleScope` (debug-only checks, see section 3).

### Why (a) holds everywhere

A GCScope matters only if a handle is allocated in its dynamic extent.
Section 1 shows no code in `lib/`, `include/` or `API/` allocates any
handle, including through callees (every handle-returning callee is a
view), so no extent contains an allocation. The only dynamic extents
that could still contain one are those of tool code (class (b)) and of
test code calling into the runtime (see section 3).

### Class (c) areas in detail

**API/hermes/hermes.cpp (JSI).** JSI `Value`/`Object`/`String`
lifetime is carried by `HermesPointerValue` entries in
`HermesRuntimeImpl::hermesValues_`
(`ManagedValues<vm::PinnedHermesValue>`, hermes.cpp:1407, 421), marked
by `runtime_.addCustomRootsFunction` (hermes.cpp:339-342). Helpers such
as `phv()`, `handle()`, `stringHandle()`, `vmHandleFromValue()`
(hermes.cpp:631-720) build `Handle` views over those slots (and over
stack `PinnedHermesValue numberStorage`, which only ever holds numbers).
None of the 63 `GCScope`s (typically one per JSI method, e.g.
hermes.cpp:1712, 2370, 3697) is the value-lifetime mechanism; they date
from when the method used `makeHandle`. Deleting them needs no
replacement. (hermes.cpp:1080 and :2092 and `throwPendingError`:3753
have a `GCScope` next to a `Locals`; same conclusion.)

**API/hermes/TracingRuntime.cpp:160,182.** `GCScope` + a per-iteration
marker around a loop; no handles. Vestigial.

**API/hermes_abi/hermes_vtable.cpp.** Same design: `ManagedValue`
(L45) objects owned by the ABI client, tracked by the custom-roots
function at L227; `makeVMHandle`/`toHandle` (L146-190, 308) return
views over `ManagedValue::value`. The 27 `GCScope`s (L461-1494, one per
vtable entry) protect nothing.

**API/napi.** Has its own handle-scope implementation:
`napi_env__::addToCurrentScope` (hermes_napi.cpp:264) writes into
`HandleBlock::slots[]` (`PinnedHermesValue` outside the GC heap), marked
by `markHandleScopes` (hermes_napi.cpp:308-324) through a custom roots
function (L79-92). It does not use `GCScope`. The 64 `GCScope`s and 11
flushes in `API/napi/*` (hermes_napi_object.cpp 25 mentions, _function
14, _string 10, _value 9, ...) are vestigial; their comments claiming
"putComputed_RJS allocates temporary handles" are stale.
`unittests/napi/NapiHandleScopeTest.cpp` has 6 GCScope mentions to check.

**lib/VM/JIT/arm64/JitHandlers.cpp.** 8 `GCScopeMarkerRAII`
(L35, 62, 93, 198, 277, 366, 398, 511); every one wraps a call whose
result is already a `PseudoHandle`/`CallResult<HermesValue>`. Vestigial.
**lib/ConsoleHost/ConsoleHost.cpp:425,552,578,666,979,1048** and
**lib/VM/Debugger/Debugger.cpp** (332, 344, 384 `flushToSmallCount(0)`
with a comment "this loop doesn't actually allocate any handles
presently", 591-661, 1205-1515): vestigial.

### Per-file table

Counts per file (every file with any mention):

| file | GCScope | MarkerRAII | flush/marker | NoLeakHandleScope | NoHandleScope | infra/comment mentions | class |
|---|---|---|---|---|---|---|---|
| API/hermes/TracingRuntime.cpp | 1 | 1 | 0 | 0 | 0 | 0/0 | (a) JSI/ABI values in custom-root list |
| API/hermes/hermes.cpp | 63 | 0 | 0 | 0 | 0 | 0/0 | (a) JSI/ABI values in custom-root list |
| API/hermes_abi/hermes_vtable.cpp | 27 | 0 | 0 | 0 | 0 | 0/0 | (a) JSI/ABI values in custom-root list |
| API/napi/hermes_napi.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (a) napi has own slot blocks |
| API/napi/hermes_napi_arraybuffer.cpp | 3 | 0 | 0 | 0 | 0 | 0/1 | (a) napi has own slot blocks |
| API/napi/hermes_napi_async_context.cpp | 1 | 0 | 0 | 0 | 0 | 0/1 | (a) napi has own slot blocks |
| API/napi/hermes_napi_bigint.cpp | 4 | 0 | 0 | 0 | 0 | 0/0 | (a) napi has own slot blocks |
| API/napi/hermes_napi_buffer.cpp | 4 | 0 | 0 | 0 | 0 | 0/1 | (a) napi has own slot blocks |
| API/napi/hermes_napi_dataview.cpp | 2 | 0 | 0 | 0 | 0 | 0/0 | (a) napi has own slot blocks |
| API/napi/hermes_napi_error.cpp | 3 | 0 | 0 | 0 | 0 | 0/1 | (a) napi has own slot blocks |
| API/napi/hermes_napi_external.cpp | 1 | 0 | 0 | 0 | 0 | 0/1 | (a) napi has own slot blocks |
| API/napi/hermes_napi_function.cpp | 5 | 0 | 5 | 0 | 0 | 0/4 | (a) napi has own slot blocks |
| API/napi/hermes_napi_internal.h | 0 | 0 | 0 | 0 | 0 | 0/2 | (a) napi has own slot blocks |
| API/napi/hermes_napi_object.cpp | 17 | 0 | 6 | 0 | 0 | 0/2 | (a) napi has own slot blocks |
| API/napi/hermes_napi_promise.cpp | 3 | 0 | 0 | 0 | 0 | 0/1 | (a) napi has own slot blocks |
| API/napi/hermes_napi_string.cpp | 9 | 0 | 0 | 0 | 0 | 0/1 | (a) napi has own slot blocks |
| API/napi/hermes_napi_typedarray.cpp | 1 | 0 | 0 | 0 | 0 | 0/1 | (a) napi has own slot blocks |
| API/napi/hermes_napi_typetag.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (a) napi has own slot blocks |
| API/napi/hermes_napi_value.cpp | 6 | 0 | 0 | 0 | 0 | 0/3 | (a) napi has own slot blocks |
| API/napi/hermes_napi_wrap.cpp | 3 | 0 | 0 | 0 | 0 | 0/0 | (a) napi has own slot blocks |
| include/hermes/VM/ArrayLike.h | 1 | 0 | 3 | 0 | 0 | 0/0 | (a) vestigial |
| include/hermes/VM/Callable.h | 0 | 1 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| include/hermes/VM/DictPropertyMap.h | 0 | 2 | 2 | 0 | 0 | 0/0 | (a) vestigial |
| include/hermes/VM/Handle-inline.h | 0 | 0 | 0 | 0 | 0 | 1/1 | infra: the machinery to delete |
| include/hermes/VM/Handle.h | 0 | 0 | 0 | 0 | 0 | 3/8 | infra: the machinery to delete |
| include/hermes/VM/HandleRootOwner-inline.h | 0 | 0 | 0 | 0 | 0 | 6/1 | infra: the machinery to delete |
| include/hermes/VM/HandleRootOwner.h | 0 | 0 | 7 | 0 | 0 | 35/17 | infra: the machinery to delete |
| include/hermes/VM/JSMapImpl.h | 0 | 2 | 2 | 0 | 0 | 0/0 | (a) vestigial |
| include/hermes/VM/JSObject-inline.h | 0 | 1 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| include/hermes/VM/JSObject.h | 0 | 1 | 1 | 0 | 0 | 0/0 | (a) vestigial |
| include/hermes/VM/JSTypedArray.h | 0 | 0 | 0 | 0 | 2 | 0/0 | (a) vestigial |
| include/hermes/VM/RootSections.def | 0 | 0 | 0 | 0 | 0 | 1/0 | infra: the machinery to delete |
| include/hermes/VM/Runtime.h | 0 | 0 | 0 | 0 | 0 | 19/1 | infra: the machinery to delete |
| include/hermes/VM/SerializedLiteralOperations.h | 0 | 2 | 5 | 0 | 0 | 2/0 | (a) vestigial |
| lib/ConsoleHost/ConsoleHost.cpp | 1 | 5 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/Callable.cpp | 3 | 2 | 0 | 0 | 0 | 0/5 | (a) vestigial |
| lib/VM/Debugger/Debugger.cpp | 5 | 6 | 2 | 0 | 0 | 2/0 | (a) vestigial |
| lib/VM/FastArray.cpp | 0 | 2 | 0 | 0 | 1 | 0/0 | (a) vestigial |
| lib/VM/HandleRootOwner.cpp | 0 | 0 | 0 | 0 | 0 | 11/1 | infra: the machinery to delete |
| lib/VM/IdentifierTable.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/Interpreter-slowpaths.cpp | 0 | 8 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/Interpreter.cpp | 1 | 1 | 58 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JIT/arm64/JitHandlers.cpp | 0 | 8 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSArray.cpp | 1 | 2 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSCallableProxy.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSError.cpp | 4 | 1 | 6 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSFinalizationRegistry.cpp | 1 | 1 | 0 | 0 | 0 | 0/2 | (a) vestigial |
| lib/VM/JSLib/Array.cpp | 28 | 10 | 60 | 3 | 0 | 7/3 | (a) vestigial |
| lib/VM/JSLib/Base64.cpp | 2 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/Date.cpp | 0 | 1 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/Error.cpp | 1 | 1 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/Function.cpp | 2 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/GlobalObject.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/HermesBuiltin.cpp | 2 | 8 | 4 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/HermesInternal.cpp | 4 | 1 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/Intl.cpp | 0 | 5 | 5 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/JSLibInternal.cpp | 3 | 0 | 2 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/JSLibInternal.h | 1 | 0 | 2 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/Map.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/Math.cpp | 1 | 2 | 4 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/Object.cpp | 6 | 3 | 6 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/Reflect.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/RegExp.cpp | 6 | 2 | 6 | 0 | 0 | 2/1 | (a) vestigial |
| lib/VM/JSLib/RuntimeJSONParse.cpp | 2 | 2 | 6 | 0 | 0 | 0/1 | (a) vestigial |
| lib/VM/JSLib/RuntimeJSONStringify.cpp | 2 | 4 | 8 | 0 | 0 | 2/0 | (a) vestigial |
| lib/VM/JSLib/Set.cpp | 1 | 9 | 2 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/String.cpp | 7 | 5 | 8 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/TypedArray.cpp | 18 | 4 | 20 | 0 | 0 | 2/0 | (a) vestigial |
| lib/VM/JSLib/WeakMap.cpp | 1 | 0 | 2 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/WeakSet.cpp | 1 | 0 | 2 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/eval.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/print.cpp | 1 | 0 | 2 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSLib/require.cpp | 2 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSObject.cpp | 1 | 5 | 6 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSProxy.cpp | 15 | 2 | 2 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/JSRegExp.cpp | 1 | 1 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/Operations.cpp | 2 | 25 | 4 | 0 | 1 | 0/1 | (a) vestigial |
| lib/VM/OrderedHashMap.cpp | 0 | 0 | 0 | 0 | 2 | 0/0 | (a) vestigial |
| lib/VM/Runtime-profilers.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/Runtime.cpp | 8 | 5 | 2 | 0 | 0 | 5/2 | (a) vestigial; plus infra (root marking L736-738, asserts L1719/L1772) |
| lib/VM/RuntimeModule.cpp | 1 | 1 | 0 | 0 | 0 | 0/0 | (a) vestigial |
| lib/VM/StaticH.cpp | 0 | 47 | 2 | 2 | 5 | 0/0 | (a) vestigial |
| lib/VM/StaticHUnit.cpp | 3 | 0 | 3 | 0 | 0 | 0/0 | (a) vestigial |
| tools/hermes/repl.cpp | 1 | 1 | 0 | 0 | 0 | 0/0 | (b) tool allocates handles itself |
| tools/hvm-bench/interp-dispatch-bench.cpp | 1 | 0 | 0 | 0 | 0 | 0/0 | (b) tool allocates handles itself |
| tools/test-runner/Executor.cpp | 2 | 1 | 1 | 0 | 0 | 0/0 | (b) tool allocates handles itself |


Notes for the notable rows:

- `lib/VM/StaticH.cpp`: 47 `GCScopeMarkerRAII` (all wrap a call whose
  result is a CallResult; none contains an allocation), 5 `NoHandleScope`
  (L1767, 1837, 1849, 1860, 2761), 2 `NoLeakHandleScope` (L1898, 2174),
  2 flush. `StaticHUnit.cpp:58` `GCScope` in `_sh_unit_init_guarded` is
  declared before the `_sh_try`, so it is not skipped by the longjmp.
- `lib/VM/Runtime.cpp`: 8 `GCScope`/5 marker, vestigial, but also
  L736-738 (root marking section, below) and the asserts at
  L1719/L1772 (`!getTopGCScope()`), L1777-1781 (a `createMarker` loop
  in `initCharacterStrings` that protects nothing).
- `lib/VM/Runtime-profilers.cpp:171`: vestigial `GCScope`.
- `lib/VM/JSLib/Array.cpp` (105 mentions) and `JSLib/TypedArray.cpp`
  (43) are the biggest single clean-ups; each has `NoLeakHandleScope`
  (Array.cpp:1336, 1424, 3134).
- The `infra` rows are the machinery itself (class definitions,
  `markGCScopes`, `GCScopeHandleTracker`).

## 3. Proposed sequence for deleting GCScope

Because nothing allocates any more, the deletion is mostly mechanical
and can be done in mostly-independent steps; the order below keeps the
tree building and tests green at each commit.

1. **Convert the `tools/` sites** (section 1.3; 12 allocating sites,
   3 files) and the unit tests (below). After this, a compile-time
   switch is possible: make the allocating `Handle`/`MutableHandle`
   constructors, `makeHandle`, `makeMutableHandle`,
   `Handle::vmcast(Runtime&, ..)` and `Runtime::makeHandle(GCPointer)`
   private/deleted first (a separate commit). This proves the audit by
   compiling `lib/ API/ tools/` and `unittests/`; anything my greps
   missed becomes a build error. Run this experiment *before*
   committing to the plan.
2. **Delete the debug-only users of GCScope state** that would break
   when `topGCScope_` is null: the 5 `NoLeakHandleScope`
   (`getTopGCScope()->getHandleCountDbg()`), the 11 `NoHandleScope`
   (`noHandleLevel_`, only checked in `newPinnedHermesValue`; in release
   they are already aliases of `NoAllocScope`, Runtime.h:2037-2062), and
   the asserts at Runtime.cpp:1719/1772.
3. **Delete all `GCScopeMarkerRAII` and `flushToMarker`/`flushToSmallCount`/
   `marker.flush()`** in `lib/`, `include/`, `API/`, then the
   `GCScope` declarations. Do it by directory in separate commits:
   `API/napi`, `API/hermes*` + `TracingRuntime`, `lib/ConsoleHost`,
   `lib/VM/Debugger`, `lib/VM/JIT`, `lib/VM/StaticH*`/`Interpreter*`,
   `lib/VM/JSLib`, rest of `lib/VM`, headers. Deleting a
   `GCScopeMarkerRAII` around a lambda/block sometimes leaves a
   now-redundant `{ }` block; leave blocks that scope a `Locals` (they
   implement rule 10).
   After this nothing but the class definitions mentions GCScope.
4. **Remove the machinery**: `GCScope`, `GCScopeMarkerRAII`,
   `GCScopeDebugBase`, `GCScopeHandleTracker` and its stats output,
   `HandleRootOwner::topGCScope_`, `newPinnedHermesValue`,
   `getTopGCScope`, the `gcScope_`/`numActiveHandles_` debug fields in
   `HandleBase`, and `markGCScopes`. Redesign needed here:
   - **Root section.** `RootSections.def` `ROOT_SECTION(GCScopes)`
     (and `Runtime.cpp:736-738`) feed snapshot/profiler output:
     `doc/MemoryProfilers.md:220` and
     `benchmarks/bench-runner/derived_metrics.py:125` mention the
     section name; heap-snapshot tests under `test/` should be grepped
     for it before removing (my grep of `test/` found no expectation).
   - **HandleRootOwner** keeps its non-GCScope duties (static constant
     roots `undefinedValue_` etc., `makeNullHandle`, `get*Value`).
     Probably fold into `Runtime` and drop the base class, or leave as a
     thin class.
   - **`Handle` stays as a view type** (`Handle<T>`, `MutableHandle<T>`
     over `PinnedHermesValue*`/`PinnedValue<T>`); only the allocating
     constructors go. `HandleBase::gcScope_` (debug) goes with it.
   - **JSI/ABI**: nothing to redesign (they own their roots). The
     `numberStorage`/`valueStorage` stack `PinnedHermesValue`s in
     hermes.cpp/hermes_vtable.cpp are number-only views and stay fine.
     (Optional: replace by `PinnedValue<>` in a Locals; not required.)
   - **napi**: nothing to redesign (own slots).
5. **Regenerate** `API/hermes_sandbox/external/*` (wasm2c blobs mention
   `markGCScopes`, `GCScope`). Refresh stale comments listed in section 1.3
   and `doc/GCSafeCoding.md`, `doc/VM.md`, `doc/MemoryProfilers.md`,
   `.claude/skills/gc-safe-coding/SKILL.md`, `dz/` items.
6. **Unit tests** (not part of this audit's scope but gate step 4):
   - `.makeHandle/.makeMutableHandle` mentions in `unittests/`: 341
     (lines containing `makeHandle|makeMutableHandle`, which also
     counts a few helper names). Per file, largest first:
     VMRuntime/JSLibTest.cpp 81, ObjectModelTest.cpp 68,
     OperationsTest.cpp 43, HeapSnapshotTest.cpp 20, ArrayTest.cpp 17,
     CodeCoverageProfilerTest.cpp 15, HiddenClassTest.cpp 13,
     HandleTest.cpp 13, napi/NapiExceptionTest.cpp 12,
     VMRuntimeTestHelpers.h 8, StringPrimitiveTest.cpp 8,
     InterpreterTest.cpp 7, ArrayStorageTest.cpp 6,
     InternalPropertiesTest.cpp 4, DecoratedObjectTest.cpp 4,
     OrderedHashMapTest.cpp 3, GCOOMTest.cpp 3, and 1-2 each in
     IdentifierTableTest, GCFinalizerTest, VMRuntimeTestHelpers.cpp,
     StackTracesTreeTest, SamplingHeapProfilerTest, NativeStateTest,
     NativeFunctionTest, JSTypedArrayTest, CallResultTest,
     napi/NapiObjectTest, NapiModuleLoadTest, NapiCreateFunctionTest.
   - allocating `Handle`/`MutableHandle` constructors with a runtime in
     `unittests/`: ~37 sites (ArrayStorageTest 2, Base64UtilTest 4,
     DictPropertyMapTest 2, GCOOMTest 1, HandleTest 5, HiddenClassTest 7,
     IdentifierTableTest 2, StaticBuiltinsTest 1, StringBuilderTest 3,
     SymbolIDTest 5, VMRuntimeTestHelpers.h 2, WeakValueMapTest 3), plus
     7 `Handle<T>::vmcast(runtime, ..)`.
   - `GCScope`/`GCScopeMarkerRAII`/`flushToMarker` in `unittests/`:
     92 + 8 + 6 (HandleTest 9, VMRuntimeTestHelpers.h 10 -- the
     fixtures, NapiExceptionTest 16, NapiCreateFunctionTest 10,
     NapiHandleScopeTest 6, ...). `HandleTest.cpp` tests the GCScope
     machinery itself and should be deleted with it.
   Convert the test fixtures (`VMRuntimeTestHelpers.h`) first: they
   hold a test-wide `GCScope` that every test's handles live in.
   Natural order inside tests: fixtures, then the large files above, as
   one subagent per file, same `PinnedValue`/Locals style.

Suggested first steps for the follow-up task: (1) the compile-time
experiment (privatise allocating APIs) on a scratch build to confirm
zero hits outside `tools/` and `unittests/`; (2) convert the three
tool files; (3) drop NoLeakHandleScope/NoHandleScope/asserts; (4)
bulk-delete scopes directory by directory.

## 4. Suspicious items

1. **`GCScopeMarkerRAII` live across `_sh_throw_current`.**
   `lib/VM/StaticH.cpp:987-991` (`putByValWithReceiver_RJS`, fast path
   `setOwnIndexed`) throws with `marker` still live and without
   `marker.flush()`; `lib/VM/StaticH.cpp:1072-1077` (`putByIndex_RJS`)
   calls `marker.flush()` first, then throws. The longjmp skips the
   destructor; the effect is only leaked slots in the enclosing
   GCScope (not dangling, GCScope itself is not skipped). Related
   regression tests: `test/hermes/regress-put-by-index-handle-leak.js`,
   `regress-create-class-handle-leak.js`. Moot once markers are
   deleted, but do not "fix" by adding more flushes.
2. **Rule 10 scan (LocalsRAII vs `_sh_throw_current`/longjmp).**
   Method: brace-depth script over `lib/VM/StaticH.cpp`,
   `StaticHUnit.cpp`, `Operations.cpp`, `Callable.cpp`, `JSObject.cpp`,
   `JIT/arm64/JitHandlers.cpp`, `JIT/arm64/JitEmitter.cpp`,
   `Interpreter.cpp`: flag any `_sh_throw_current`/`_sh_longjmp` or
   call to a throwing helper (`doCall`, `putById_RJS`,
   `putByValWithReceiver_RJS`, `putByIndex_RJS`, `getByIdWithReceiver_RJS`,
   `delByVal`, `getArgumentsPropByVal_RJS`, `reifyArguments`,
   `createObjectFromBuffer`, any `_sh_*` entry) reachable while a
   `LocalsRAII` is in an enclosing open block. Result: **no hits**
   (the one tool output at JitHandlers.cpp:44 is a false positive: the
   call is `_sh_ljs_is_undefined`, a pure predicate). All throwing
   sites that previously had a Locals now follow the
   "record failure, close block, then throw" pattern (e.g.
   StaticH.cpp:1085-1119, `_sh_ljs_callRequire` lambda at L490-522,
   StaticHUnit.cpp:~380). Frames between a throw and its catch are only
   `_sh_*` helpers, `doCall`, and compiled frames; C++->SH boundaries
   (`_callWrapper`, Callable.cpp:60; `_sh_unit_init_guarded`) install
   their own `SHJmpBuf`. The JIT compiler's own
   `_sh_longjmp(errorJmpBuf_)` (JIT.cpp:130, 388) crosses
   `Emitter::...` only; the single Locals there
   (JitEmitter.cpp:639-641, `getOrCreateHCId`-like function) calls only
   `ArrayStorageSmall::create/push_back`, never the emitter error
   callback. Caveat: static analysis only; callee transitivity was
   checked by name for helpers defined in these files and for
   `_jitCallImpl` implementations (Callable.cpp:942, 1061, 1219, 1474;
   JSObject.cpp:2827), none of which hold a Locals across the throw.
3. **Stack `PinnedHermesValue` locals are not roots.** ~25 uses
   (`numberStorage`/`valueStorage` in hermes.cpp and hermes_vtable.cpp,
   `lengthValue` PrimitiveBox.cpp:60, `indexValue` PrimitiveBox.cpp:155,
   `newValueHV` JSArray.h:410) only ever hold numbers (comment at
   JSArray.h:409 states the invariant). But `hermes.cpp:3760`
   (`throwPendingError`) stores whatever `vmHandleFromValue(jsiVal)`
   needs; `jsiVal` keeps pointer values alive via `hermesValues_`, so it
   is fine today. Fragile if someone writes a pointer into one of them.
4. **Handle-returning functions that return aliases of parameters**
   (`speciesConstructor`, `generatorValidate`, `validateUint8Array`,
   `ensureFlat`, `defineSystemConstructor`): safe, but their
   signatures look like rule-4 violations to a reviewer and would be
   one if anyone replaced the parameter with a local. Candidates for a
   doc comment or a return of `PseudoHandle`/`HermesValue` when
   `Handle` becomes view-only.
5. **`tools/hvm-bench/interp-dispatch-bench.cpp:164`** returns a
   `Handle` allocated in the topmost scope: the only known function in
   the tree that still returns a Handle into a caller scope allocation.
6. **Stale `NoLeakHandleScope`** in `StaticH.cpp:1898, 2174` and
   `JSLib/Array.cpp:1336, 1424, 3134`: they dereference
   `getTopGCScope()` (debug only). Will crash in debug builds if
   GCScope removal precedes their removal; also, they are the only
   consumers of `GCScope::getHandleCountDbg()`.
7. **Stale artifacts.** `lib/VM/JSObject.cpp.orig` on disk (ignored by
   Sapling, 5+ `makeHandle` mentions that confuse grep). Stale
   comments listed in section 1.3.
8. **`Runtime.cpp:407`** `strForSymbolNoDescription =
   vmcast<StringPrimitive>(ignoreAllocationFailure(createLongLived(..)))`
   stores a raw pointer from a `CallResult<HermesValue>`; long-lived
   objects are never moved/collected, so this is intended and safe;
   noted only because a PseudoHandle-style scan flags it.
9. Bare `SymbolID`/`PseudoHandle` held across safepoints: none found in
   the sites read (all `valueToSymbolID`/`stringToSymbolID` callers use
   `lv.sym`; `hermes.cpp:2375-2430` passes
   `StringPrimitive::createNoThrow(..)` straight into
   `stringToSymbolID`, which pins it).

## Reproducing

```bash
grep -rnE '\.makeHandle|->makeHandle|makeMutableHandle' lib include API tools --include=*.cpp --include=*.h
grep -rnE 'Handle<[^>]*>[[:space:]]*[A-Za-z_0-9]*[[:space:]]*[{(][[:space:]]*(\*?[A-Za-z_>.-]*[rR]untime[A-Za-z_]*|rt|\*this)[[:space:]]*[,)}]' lib include API tools --include=*.cpp --include=*.h
grep -rnE 'GCScope|flushToMarker|flushToSmallCount|createMarker|NoHandleScope|NoLeakHandleScope' lib include API tools --include=*.cpp --include=*.h
```
