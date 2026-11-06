# Building and testing the arm64 JIT on an x86-64 host

The JIT only exists for arm64 (`include/hermes/VM/JIT/Config.h`), so on an
x86-64 development machine it is normally neither compiled nor executed —
`HERMESVM_ALLOW_JIT=1` silently yields `HERMESVM_JIT=0`, and the tests under
`test/jit` are skipped as unsupported. This directory holds the pieces
needed to cross-compile Hermes for aarch64 Linux and run it under
`qemu-user`, which is enough to compile *and execute* JIT'ed code locally.

It is a smoke-testing facility, not a replacement for real hardware — see
"Limitations" at the bottom.

## Prerequisites (Debian/Ubuntu)

```bash
sudo apt install qemu-user-static gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

clang (any recent version) provides the cross compiler itself; the
`gcc-aarch64-linux-gnu` packages supply the aarch64 glibc, libstdc++ and
binutils that clang drives. `qemu-user-static` provides `qemu-aarch64-static`.

## 1. Build the host compilers

Cross-compiling needs `hermesc`/`shermes` that run on the *host*, to compile
`InternalJavaScript` during the build. Build them from the same revision as
the target build — a stale host `hermesc` produces bytecode that does not
match what the target expects.

```bash
cmake -B cmake-build-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build cmake-build-host --target hermesc shermes -j "$(nproc)"
```

This also generates `cmake-build-host/ImportHostCompilers.cmake`, which the
cross build imports.

## 2. Configure and build for aarch64

```bash
cmake -B cmake-build-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/aarch64/aarch64-linux-gnu.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" \
  -DHERMESVM_ALLOW_JIT=2 \
  -DHERMES_UNICODE_LITE=ON \
  -DIMPORT_HOST_COMPILERS="$PWD/cmake-build-host/ImportHostCompilers.cmake" \
  -DQEMU_RUN_PREFIX="qemu-aarch64-static -L /usr/aarch64-linux-gnu"
cmake --build cmake-build-arm64 --target hermes -j "$(nproc)"
```

Why each of the non-obvious flags:

- **`HERMESVM_ALLOW_JIT=2`** — force the JIT on. `=1` (auto) would also work
  for aarch64, but `=2` is what sets the lit parameter `jit_enabled=2` that
  gates `test/jit`; with `=1` the whole JIT suite silently skips.
- **`HERMES_UNICODE_LITE=ON`** — uses the internal no-op unicode
  implementation, so no cross-compiled ICU is needed.
- **`IMPORT_HOST_COMPILERS`** — see step 1.
- **`QEMU_RUN_PREFIX`** — already supported by the build system: it is passed
  to lit, which prefixes every target-binary substitution (`%hermes`,
  `%hermesc`, `%FileCheck`, …) with it, and it defines `QEMU_MODE`, which
  drops two unit-test expectations that do not hold under emulation
  (physical-page accounting in `AlignedHeapSegmentTest`, and a
  `vm_commit(nullptr)` case that segfaults under qemu-user).
- **`-O1` Debug** — Debug keeps the assertions (including
  `-Xjit-emit-asserts`, which defaults on in Debug builds), `-O1` keeps
  emulation tolerably fast. **Do not add ASan**: the cross toolchain would
  need a matching aarch64 sanitizer runtime, and ASan's shadow-memory
  mapping does not survive qemu-user. This is the one place where the
  project's usual ASan-by-default rule cannot apply.

## 3. Run the sanity checks

```bash
./aarch64/qemu-sanity.sh              # defaults to cmake-build-arm64
```

Nine checks: the binary is aarch64; it runs under qemu; the JIT actually
compiles functions; emitted code contains arm64 mnemonics; JIT'ed arithmetic
executes correctly; a broad workload (`aarch64/jit-stress.js`) produces
byte-identical output with and without `-Xjit=force`, and again with
`-Xjit-emit-asserts`; the threshold tier-up path works; and the JIT's inline
counters increment (proving JIT'ed code really ran, rather than merely being
emitted).

`jit-stress.js` is a differential workload, not a unit test: it exercises
arithmetic and NaN/-0 corners, comparisons in both branch polarities,
strings, inline caches and shape transitions, literal buffers, arrays,
closures and environments, exceptions in loops, both switch forms, classes
and `super`, generators/iterators/destructuring, `for-in`, `arguments`, and
builtins that call back into JIT'ed code. Any JIT/interpreter divergence
shows up as a diff.

## 4. Run the JIT test suite

```bash
LIT_FILTER="jit/" cmake --build cmake-build-arm64 --target check-hermes -j "$(nproc)"
```

Expected: 54 pass and 27 are reported unsupported. Twenty-six of those are
the `test/jit/x86-64/` bring-up tests, which the directory's own
`lit.local.cfg` gates to that backend; the twenty-seventh is
`large_literal_obj.js`, which requires `!slow_debug` and so is skipped in
this (slow-debug) build exactly as it would be on real hardware. Runtime is
about 40s wall-clock with 16 threads.

Drop `LIT_FILTER` to run everything (much slower under emulation). Note that
lit runs `FileCheck` under qemu too, since it is built as a target binary,
and that `check-hermes` also builds and runs the NAPI suites — those pass
under emulation as well (41 of 64, the rest skipped by their own skip list).

Running `hermes-lit` directly needs `python` on `PATH`; Ubuntu only provides
`python3`, so prefer the `check-hermes` target.

## 5. The other two heap-value modes

Anything that touches heap value widths or the inline write barrier has to
be built and run in all three heap-value modes, exactly as on x86-64 (see
doc/JIT.md, "The heap-value-mode build matrix"). The two extra trees are
`cmake-build-arm64` with one flag added:

```bash
cmake -B cmake-build-arm64-hv32 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/aarch64/aarch64-linux-gnu.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" \
  -DHERMESVM_ALLOW_JIT=2 \
  -DHERMES_UNICODE_LITE=ON \
  -DHERMESVM_HEAP_HV_MODE=HEAP_HV_PREFER32 \
  -DIMPORT_HOST_COMPILERS="$PWD/cmake-build-host/ImportHostCompilers.cmake" \
  -DQEMU_RUN_PREFIX="qemu-aarch64-static -L /usr/aarch64-linux-gnu"
```

and the same again as `cmake-build-arm64-boxed` with
`-DHERMESVM_HEAP_HV_MODE=HEAP_HV_BOXED`. `HEAP_HV_PREFER32` turns on
compressed pointers and boxed doubles together, so a heap slot is four
bytes there; `HEAP_HV_BOXED` turns on boxed doubles alone, so a slot stays
eight bytes but holds a `HermesValue32` rather than a `HermesValue`. Both
`qemu-sanity.sh` and the `LIT_FILTER="jit/"` run above take a build
directory, so run each of them against all three trees.

## Limitations

- **Timings are meaningless.** qemu-user interprets guest instructions;
  anything performance-related must be measured on real hardware.
- **No ASan/TSan.** See above. Memory bugs that ASan would catch on macOS
  arm64 stay invisible here; the `HERMES_SLOW_DEBUG` assertions in a Debug
  build are the available substitute.
- **Not an ISA conformance oracle.** qemu implements the architecture
  faithfully enough for this JIT, but a subtle encoding or flag-semantics
  divergence between qemu and real silicon would not be caught here. Treat a
  green run as "the JIT is not obviously broken", and confirm anything
  load-bearing on hardware.
- **The system-level environment differs** (thread stack bounds, signals,
  `mmap` layout). The JIT's native-stack overflow check and executable-memory
  allocation do work under qemu, but they are exercising qemu's emulation of
  those facilities, not the real ones.
