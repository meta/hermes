# Native JSON parsing

`JsonParse.cpp` compares the raw ASCII and UTF-16 APIs with
`Value::createFromJsonUtf8` and a cached `JSON.parse` function. It includes
primitives, long strings, and arrays of nested objects in both encodings.

Build Hermes in Release mode. On macOS, run these commands from the repository
root:

```sh
cmake -S . -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target hermesvm
c++ -std=c++17 -O3 -DNDEBUG -I API -I API/jsi -I public \
  benchmarks/jsi-json/JsonParse.cpp -L cmake-build-release/lib -lhermesvm \
  -Wl,-rpath,"$PWD/cmake-build-release/lib" \
  -o cmake-build-release/bin/jsi-json-benchmark
cmake-build-release/bin/jsi-json-benchmark plain 0 > plain-0.csv
cmake-build-release/bin/jsi-json-benchmark thread-safe 0 > thread-safe-0.csv
```

Repeat each command in fresh processes, incrementing the final argument to
rotate the starting method. Keep the CSV output outside the source tree.
The linker command above is for macOS; other platforms may need different
library and runtime search flags.

The `cached-parse-ascii` and `cached-parse-utf16` rows include creating the JS
input string on each call. The `cached-parse-prebuilt` row reuses a JS string
created before timing. All native input buffers, the function lookup, and
encoding conversions are outside timing. Each result handle is destroyed
inside timing. There is no JNI overhead in this benchmark.

For each input, all methods use the same iteration count. The harness runs
three warmup batches and eight measured batches, rotating method order and
requesting GC before each batch, outside timing. Report medians and the spread
across process runs with the machine, build options, and heap value mode.
