# No-allocation compile experiment

Throwaway experiment (changes reverted, nothing committed). Goal: prove
that no code outside `tools/` and `unittests/` still calls an API that
allocates a slot in the topmost GCScope.

## Result

**Zero production hits.** Every compile error is in `tools/` (the 12
sites already listed in `phase4-audit.md`) or `unittests/`.

## APIs poisoned (`= delete`, working tree only, since reverted)

`include/hermes/VM/HandleRootOwner.h` (definitions removed from
`HandleRootOwner-inline.h`):
- `HandleRootOwner::makeHandle(HermesValue)`, `<T>(T*)`,
  `<T>(HermesValue)`, `(SymbolID)`, `<T>(PseudoHandle<T>&&)`,
  `<T>(PseudoHandle<HermesValue>&&)`
- `HandleRootOwner::makeMutableHandle(HermesValue)`, `<T>(T*)`
- `HandleRootOwner::newPinnedHermesValue(GCScope*, HermesValue)` (static)
  and `newPinnedHermesValue(HermesValue)`

`include/hermes/VM/Runtime.h` (definitions removed):
- `Runtime::makeHandle(const GCPointer<T>&)`,
  `Runtime::makeMutableHandle(const GCPointer<T>&)`

`include/hermes/VM/Handle.h` (inline def of the HandleBase ctor removed
from `Handle-inline.h`):
- `HandleBase(HandleRootOwner&, HermesValue = undefined)`
- `Handle<T>(HandleRootOwner&, HermesValue, bool)` (protected)
- `Handle<T>(HandleRootOwner&, value_type)` (protected)
- `Handle<T>(GCScope*, value_type)` (protected)
- `Handle<T>::vmcast(HandleRootOwner&, HermesValue)`
- `MutableHandle<T>(HandleRootOwner&, value_type = default)`

Not poisoned: `GCScope::newPinnedHermesValue` (member of GCScope; its
only callers were the removed `HandleRootOwner::newPinnedHermesValue`
overloads, and `Handle(GCScope*, ..)` is deleted, so it is unreachable).
`Handle<T>::dyn_vmcast` has no `Runtime&` overload in the current tree.
Non-allocating APIs (`makeNullHandle`, `get*Value`, view constructors)
untouched. Functions that return `Handle<T>` by calling one of the above
are covered transitively because their bodies would not compile.

## Build configuration

```
cmake -B /tmp/cmake-build-noalloc -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=OFF -DJSI_UNSTABLE=ON \
  -DHERMES_ENABLE_DEBUGGER=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1"
cmake --build /tmp/cmake-build-noalloc -- -k 0
```
938 build steps (all targets incl. lean variants, tools, unittests,
napi, API/hermes, hermes_abi); 57 failed compile steps: 3 in `tools/`, 54
in `unittests/`. Every production object compiled.

Extra `-fsyntax-only` checks (flags borrowed from `lib/VM/JSObject.cpp`
in compile_commands.json), all clean unless noted:
- `lib/VM/JSLib/Intl.cpp` with `-DHERMES_ENABLE_INTL`: OK.
- `lib/VM/JIT/arm64/*.cpp` (14 files) with `-DHERMESVM_ALLOW_JIT=2`
  `-Iexternal/asmjit/asmjit/src -DASMJIT_STATIC`; also
  `JIT/DiscoverBB.cpp`, `JIT/PerfJitDump.cpp`: OK.
- `Runtime.cpp`, `Interpreter.cpp`, `Runtime-profilers.cpp`,
  `JSLib/GlobalObject.cpp` with `-DHERMESVM_INTERNAL_JAVASCRIPT_NATIVE=1`: OK.
- same four with `-DHERMESVM_SANITIZE_HANDLES -DHERMESVM_PROFILER_MEM
  -DHERMESVM_DEBUG_TRACK_GCSCOPE_HANDLES -DHERMES_ENABLE_INTL`: OK.
- same four with `-DHERMESVM_PROFILER_{OPCODE,BB,JSFUNCTION,NATIVECALL,EXTERN}`:
  fails, but NOT because of the poisoning: `Callable.h:617` static_assert
  (`sizeof(NativeJSFunction) == sizeof(SHNativeJSFunction)`) and
  `Runtime-profilers.cpp:171` `GCScope gcScope{this};` ("no matching
  constructor": `this` is a pointer, GCScope takes a reference).
  Pre-existing bit-rot in that configuration; it also means the
  profiler-only code in `Runtime-profilers.cpp` was never checked by this
  experiment. It has no handle allocation by inspection (only the GCScope
  and `gcScope.clearAllHandles()`).

## Errors

### Production (lib/ include/ API/): 0

### tools/: 10 diagnosed + 2 undiagnosed = the 12 known sites

Clang did not report two calls (they follow an earlier error in the
same function); I confirmed `interp-dispatch-bench.cpp:164` by removing
the line-60 error and recompiling. Both are in the audit's list.

| file:line | construct |
|---|---|
| tools/hermes/repl.cpp:347 | `makeHandle(getThrownValue())` |
| tools/hermes/repl.cpp:350 | `makeHandle<Callable>(std::move(*propRes))` |
| tools/hermes/repl.cpp:369 | `makeHandle(getThrownValue())` |
| tools/hermes/repl.cpp:372-373 (not reported) | `makeHandle<JSFunction>(std::move(*callRes))` |
| tools/hermes/repl.cpp:386 | `MutableHandle<>{*runtime}` |
| tools/hermes/repl.cpp:446 | `makeHandle(getThrownValue())` |
| tools/hermes/repl.cpp:459 | `MutableHandle<Callable>{*runtime}` |
| tools/hermes/repl.cpp:470 | `makeHandle(getThrownValue())` |
| tools/hvm-bench/interp-dispatch-bench.cpp:60 | `makeHandle(Domain::create(runtime))` |
| tools/hvm-bench/interp-dispatch-bench.cpp:164 (not reported) | `return makeHandle<StringPrimitive>(*status)` |
| tools/test-runner/Executor.cpp:183 | `makeHandle(getThrownValue())` |
| tools/test-runner/Executor.cpp:275 | `MutableHandle<Callable> task{runtime}` |

### unittests/: 54 failed TUs (188 distinct diagnostics, a lower bound)

Clang stops at `-ferror-limit=20`-style caps in some TUs (12 shown for
the biggest files), so per-file counts below are lower bounds; the
audit's ~341 `makeHandle` mentions and ~37 ctor sites are the fuller
picture. Includes `VMRuntimeTestHelpers.h` (9 lines, cascades into every
fixture user) and 12 hits in `include/hermes/VM/TypedArrays.def`
reached only from `unittests/VMRuntime/JSTypedArrayTest.cpp`
(macro-expanded `makeHandle`, test-only include).

- unittests/VMRuntime/ObjectModelTest.cpp: 112 120 126 142 160 186 44 46 55 69 80 95 (12)
- unittests/VMRuntime/JSLibTest.cpp: 103 111 120 126 132 141 155 164 51 55 80 93 (12)
- unittests/VMRuntime/HiddenClassTest.cpp: 206 207 259 260 263 341 376 422 46 47 48 51 (12)
- unittests/VMRuntime/HeapSnapshotTest.cpp: 1038 1045 1054 1061 1115 1116 847 890 934 960 961 962 (12)
- unittests/VMRuntime/HandleTest.cpp: 119 128 130 136 30 33 34 69 76 77 78 98 (12)
- unittests/VMRuntime/CodeCoverageProfilerTest.cpp: 103 104 106 153 155 157 176 189 190 192 222 223 (12)
- unittests/napi/NapiExceptionTest.cpp: 1029 1220 1438 1469 495 533 583 613 665 695 743 787 (12)
- unittests/VMRuntime/VMRuntimeTestHelpers.h: 118 481 494 511 521 534 539 548 557 (9)
- unittests/VMRuntime/StringPrimitiveTest.cpp: 138 149 256 267 282 299 312 327 57 (9)
- unittests/VMRuntime/ArrayStorageTest.cpp: 108 111 112 113 114 115 191 198 (8)
- unittests/VMRuntime/OperationsTest.cpp: 101 102 167 171 175 179 183 (7)
- unittests/VMRuntime/ArrayTest.cpp: 42 46 74 78 82 86 87 (7)
- unittests/VMRuntime/JSONParseTest.cpp: 102 134 172 36 60 79 (6)
- unittests/VMRuntime/InterpreterTest.cpp: 120 149 203 360 363 98 (6)
- unittests/VMRuntime/InternalPropertiesTest.cpp: 133 174 37 43 57 80 (6)
- unittests/VMRuntime/Base64UtilTest.cpp: 83 87 88 89 90 91 (6)
- unittests/VMRuntime/SymbolIDTest.cpp: 216 41 46 49 50 (5)
- unittests/VMRuntime/IdentifierTableTest.cpp: 179 182 185 70 (4)
- unittests/VMRuntime/GCOOMTest.cpp: 76 77 81 83 (4)
- unittests/VMRuntime/DecoratedObjectTest.cpp: 35 57 76 89 (4)
- unittests/VMRuntime/WeakValueMapTest.cpp: 42 43 44 (3)
- unittests/VMRuntime/StringBuilderTest.cpp: 23 38 56 (3)
- unittests/VMRuntime/OrderedHashMapTest.cpp: 50 62 72 (3)
- unittests/VMRuntime/GCFinalizerTest.cpp: 125 128 (2)
- unittests/VMRuntime/DictPropertyMapTest.cpp: 30 34 (2)
- unittests/VMRuntime/VMRuntimeTestHelpers.cpp: 22 (1)
- unittests/VMRuntime/StaticBuiltinsTest.cpp: 59 (1)
- unittests/VMRuntime/StackTracesTreeTest.cpp: 205 (1)
- unittests/VMRuntime/SamplingHeapProfilerTest.cpp: 168 (1)
- unittests/VMRuntime/NativeStateTest.cpp: 27 (1)
- unittests/VMRuntime/NativeFunctionTest.cpp: 24 (1)
- unittests/VMRuntime/CallResultTest.cpp: 62 (1)
- unittests/napi/NapiObjectTest.cpp: 2979 (1)
- unittests/napi/NapiModuleLoadTest.cpp: 111 (1)
- unittests/napi/NapiCreateFunctionTest.cpp: 286 (1)

Plus `unittests/VMRuntime/JSTypedArrayTest.cpp` via `TypedArrays.def`
lines 21-24, 28-36.

## Conclusion

The audit's claim holds: with every handle-allocating entry point
deleted, `lib/`, `include/` (outside the deleted definitions) and `API/`
build in the default configuration plus the Intl, arm64 JIT,
internal-JS-native, sanitize-handles and memory-profiler variants. The
only breakage is the 12 known `tools/` sites and the test suite. The
opcode/time-profiler configuration is already broken independently of
this change. Step 1 of the follow-up sequence can proceed:
convert `tools/`, then the unit tests, then make the APIs private for
real.
