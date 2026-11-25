# No-allocation compile experiment, round 2

Re-run of `noalloc-experiment.md` after step 2 (tools/ and unittests/
converted). Throwaway edits, reverted, nothing committed.

## Result

Only `unittests/VMRuntime/HandleTest.cpp` (17 errors) and the two legacy
allocating `createNoThrowForTest` overloads in
`unittests/VMRuntime/VMRuntimeTestHelpers.h` (lines ~574-586, kept for
HandleTest) fail. Everything else, including lib/, API/, tools/, napi,
ABI and all other unit tests, compiles. The legacy overloads cascade
into 50 test TUs when present; with them temporarily removed the only
remaining errors are in HandleTest.cpp.

## APIs poisoned (`= delete`, inline definitions removed)

- `HandleRootOwner::makeHandle` (all 6 overloads), `makeMutableHandle`
  (2 overloads), both `newPinnedHermesValue` overloads
- `Runtime::makeHandle/makeMutableHandle(const GCPointer<T>&)`
- `HandleBase(HandleRootOwner&, HermesValue)`
- `Handle<T>(HandleRootOwner&, HermesValue, bool)`,
  `Handle<T>(HandleRootOwner&, value_type)`, `Handle<T>(GCScope*, value_type)`
- `Handle<T>::vmcast(HandleRootOwner&, HermesValue)`
- `MutableHandle<T>(HandleRootOwner&, value_type)`

## Configuration

```
cmake -B /tmp/cmake-build-noalloc2 -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=OFF -DJSI_UNSTABLE=ON \
  -DHERMES_ENABLE_DEBUGGER=ON -DCMAKE_CXX_FLAGS=-O1 -DCMAKE_C_FLAGS=-O1 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build /tmp/cmake-build-noalloc2 -- -k 0
```
938 steps; 50 failed (all via the legacy helpers or HandleTest). Same
extra-variant caveats as round 1 were not re-checked.
