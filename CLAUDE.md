# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## CRITICAL: Never Change the Working Directory

**NEVER use `cd` to change the current directory from the project root.** Almost all operations can be performed by passing the correct path to commands and tools. Changing directories causes confusion and errors in subsequent operations.

In the rare cases where changing directory is absolutely unavoidable, use a subshell so the directory change does not persist:
```bash
(cd other-dir; command;)
```

## Overview

Hermes is a JavaScript engine optimized for fast start-up of React Native apps. It features ahead-of-time static optimization and compact bytecode.

## Build Commands

### Default Build: ASan+Debug with -O1

Always build and test with AddressSanitizer enabled unless the user explicitly asks otherwise or there is a specific reason not to (e.g., performance benchmarking, testing release behavior). The ASan build catches memory bugs that are otherwise silent or non-deterministic.

```bash
# Configure ASan+Debug build (the default for development)
cmake -B cmake-build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=ON \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1"
```

### Other Build Configurations

```bash
# Plain Debug build (no sanitizer, for debugging with full symbols)
cmake -B cmake-build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Release build
cmake -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
```

#### Common CMake Options

Pass these with `-D` when configuring, e.g., `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DHERMES_ENABLE_DEBUGGER=ON`

**Build Type & Core Options:**
- `CMAKE_BUILD_TYPE` - Debug or Release (required)
- `HERMES_ENABLE_DEBUGGER` - Build with debugger support (default: OFF)
- `HERMES_FACEBOOK_BUILD` - Build Facebook internal version (default: OFF)
- `HERMES_ENABLE_CONTRIB_EXTENSIONS` - Include community-contributed extensions (default: ON)
- `HERMES_ENABLE_WERROR` - Treat warnings as errors (default: OFF)

**Sanitizers:**
- `HERMES_ENABLE_ADDRESS_SANITIZER` - Enable ASan (default: OFF)
- `HERMES_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER` - Enable UBSan (default: OFF)
- `HERMES_ENABLE_THREAD_SANITIZER` - Enable TSan (default: OFF)

**GC & Memory:**
- `HERMESVM_GCKIND` - GC type: MALLOC or HADES (default: HADES)
- `HERMESVM_HEAP_HV_MODE` - Heap HermesValue encoding mode (default: HEAP_HV_64). See "Heap HermesValue Modes" section below.
- `HERMESVM_SANITIZE_HANDLES` - Move heap after every alloc to catch stale handles (default: OFF)

**Performance/Debug Tradeoffs:**
- `HERMES_SLOW_DEBUG` - Enable slow checks in Debug builds (default: ON)
- `HERMESVM_ALLOW_JIT` - JIT mode: 0 (off), 1 (auto), 2 (force on) (default: 0)

**Compilation Modes:**
- `HERMESVM_INTERNAL_JAVASCRIPT_NATIVE` - Use natively compiled internal JS instead of bytecode (default: OFF)
- `HERMES_UNICODE_LITE` - Use internal no-op unicode instead of system libraries (default: OFF)

**External Dependencies:**
- `HERMES_ALLOW_BOOST_CONTEXT` - Use Boost.Context fibers: 0 (off), 1 (auto), 2 (force on) (default: 1)
- `JSI_UNSTABLE` - Enable JSI unstable APIs (default: OFF)
- `IMPORT_HOST_COMPILERS` - Import shermes/hermesc from another build for cross-compilation

### Building

```bash
# Build
cmake --build cmake-build-asan --target hermes

# Run all tests
cmake --build cmake-build-asan --target check-hermes

# Run single test
cmake-build-asan/bin/hermes path/to/test.js

# Run test262 testsuite (ONLY run it when user asks for it)
python3 utils/test_runner.py path/to/test262/test -b cmake-build-asan/bin

# Generate the preprocessed JS file from a single test262 test.
# Then you can run the <output_file> with hermes.
python3 utils/test_runner.py <path_to_single_test262_test> -b cmake-build-asan/bin -d > <output_file>
```

### Running Tests in Claude Code on macOS

Due to a sandbox limitation in Claude Code on macOS, Python's multiprocessing module cannot create semaphores, causing the lit test runner to fail. This is a bug in the Claude Code sandbox, not in Hermes. To work around this, run tests in single-process mode:

```bash
LIT_OPTS="-j1" cmake --build cmake-build-asan --target check-hermes
```

Since single-process mode is slow (~5 minutes for all tests), use the `LIT_FILTER` environment variable to run only specific tests matching a regex:

```bash
# Run only tests matching "Array" in their path
LIT_OPTS="-j1" LIT_FILTER="Array" cmake --build cmake-build-asan --target check-hermes

# Run only tests in a specific directory
LIT_OPTS="-j1" LIT_FILTER="BCGen" cmake --build cmake-build-asan --target check-hermes
```

This workaround is only needed for Claude Code on macOS. Normal users and CI systems do not need these flags.

### Building a Single File

For faster iteration when modifying a single file `dir1/dir2/file.cpp`:

```bash
# Find the target the file belongs to
find cmake-build-asan/ -name file.cpp.o

# Build just that file (example for VM files)
cmake --build cmake-build-asan --target lib/VM/CMakeFiles/hermesVMRuntime_obj.dir/file.cpp.o
```

### Inspecting C++ File Structure

```bash
# List all functions in a file sorted by line number (requires ctags)
utils/dump-cpp-funcs.sh file.cpp
```

## Code Architecture

The build produces two VM variants: "regular" (full VM with compiler) and "lean" (excludes parser and compiler for smaller binary size).

### Core Components

- **lib/VM/**: Virtual machine core - runtime, interpreter, garbage collector, object model
- **lib/VM/JSLib/**: JavaScript standard library implemented in C++ (Array, Object, String, etc.)
- **lib/InternalJavaScript/**: JavaScript polyfills compiled into the VM (e.g., Math.sumPrecise, Promise)
- **lib/Parser/**: JavaScript parser
- **lib/AST/**: Abstract syntax tree definitions
- **lib/IR/**: Intermediate representation for optimization
- **lib/IRGen/**: Generates IR from AST
- **lib/BCGen/**: Bytecode generation from IR
- **lib/Sema/**: Semantic analysis
- **lib/Support/**: Shared utilities and data structures
- **include/hermes/**: Public header files organized by component
- **API/hermes/extensions/**: JSI-based runtime extensions (see Extensions section below)

### Key VM Files

- `lib/VM/Runtime.cpp`: Main runtime implementation
- `lib/VM/Interpreter.cpp`: Bytecode interpreter
- `lib/VM/Callable.cpp`: Function and callable object implementation
- `lib/VM/JSObject.cpp`: JavaScript object model
- `lib/VM/Operations.cpp`: Core JS operations (typeof, instanceof, etc.)
- `lib/VM/gcs/`: Garbage collector implementations

### Testing

- `test/`: Lit-based integration tests organized by component
- `unittests/`: Google Test unit tests (VMRuntime, Support, API, Parser, etc.)

### Auto-Updating Tests

Some lit tests use `%FileCheckOrRegen` instead of `%FileCheck`. These are auto-updating tests whose expected output can be regenerated automatically.

If such a test fails due to intentional changes (e.g., changed output format, added runtime modules affecting IDs), and you understand why the failure occurred:

```bash
# Regenerate expected output for all auto-updating tests
cmake --build cmake-build-asan --target update-lit
```

**Important:** Only use `update-lit` when you understand the cause of the failure. Review the changes to verify they match your expectations. Do not blindly regenerate tests to make them pass.

## Code Style

Key conventions:

- **C++17**, no exceptions or RTTI
- **Naming**: Classes (`PascalCase`), functions/methods (`camelCase`), variables (`camelCase`), member vars (`_suffix`), constants (`SNAKE_CASE` or `kCamelCase`)
- **structs**: PODs only; use `class` for anything with constructors/destructors
- **Line limit**: 80 characters, 2-space indent
- **Doc comments**: Required for every declaration
- **Inlining**: Only trivial one-line methods in class body

## Native Function Development

Standard signature for VM native functions:
```cpp
CallResult<HermesValue> funcName(void *context, Runtime &runtime)
```

Access arguments:
```cpp
NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
```

## Macro-Generated Code

When making systematic changes, check for macro-generated code:
- `NATIVE_ERROR_TYPE` macro in `lib/VM/JSLib/Error.cpp` - generates error constructors
- `TYPED_ARRAY` macro in `lib/VM/JSLib/TypedArray.cpp` - generates typed array constructors
- `NATIVE_FUNCTION` macro in `include/hermes/VM/JSNativeFunctions.h` - generates function declarations

## GC-Safe Coding (Runtime Code)

When writing, modifying, or reviewing C++ code in the VM runtime (`lib/VM/`, `include/hermes/VM/`, `API/hermes/`), always invoke the `gc-safe-coding` skill first. Runtime code must follow strict GC-safety rules around handles, locals, and heap-allocated objects. The skill covers all the rules and common pitfalls.

## GC Handle Patterns

### Locals Pattern (preferred for new code)
```cpp
struct : public Locals {
  PinnedValue<JSObject> objHandle;
  PinnedValue<PropertyAccessor> accessor;
  PinnedValue<> tempValue;  // untyped
} lv;
LocalsRAII lraii(runtime, &lv);

// Assignment from PseudoHandle
lv.value = std::move(pseudoHandle);

// Assignment from CallResult with known type
lv.obj.castAndSetHermesValue<JSObject>(callResult.getValue());
```

### Critical: Null Prototype Handling
```cpp
// When traversing prototype chains, always check for null:
if (!*protoRes) {
  lv.O = nullptr;
} else {
  lv.O.castAndSetHermesValue<JSObject>(protoRes->getHermesValue());
}
```

### PinnedHermesValue
`PinnedHermesValue` publicly inherits from `HermesValue` — do not cast to `HermesValue` to access its methods.

## Copyright Header

```cpp
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
```

## Heap HermesValue Modes

The `HERMESVM_HEAP_HV_MODE` CMake option controls how JS values are encoded in memory. This significantly affects memory usage and performance.

### HEAP_HV_64 (default)

All JS values, pointers, and numbers are encoded as 64-bit values using NaN-boxing. This is the simplest mode and works on all platforms.

### HEAP_HV_PREFER32

Enables 32-bit HermesValue encoding when possible, saving memory at the cost of some overhead:

**On 32-bit devices:** JS values and pointers are 32-bit. Numbers that don't fit in 32 bits are boxed in the heap (typically a small percentage in real apps).

**On 64-bit platforms (except iOS):** JS values and pointers are 32-bit offsets into a contiguous (up to) 4GB reserved memory block. All heap memory is allocated within this block.

**On iOS (64-bit):** iOS doesn't allow large virtual address space reservations by default. Hermes allocates memory in 4MB segments anywhere in the 64-bit address space. 32-bit pointers are encoded as a combination of segment table index and offset within the segment. This has more overhead than other platforms but still saves significant memory.

### HEAP_HV_BOXED

Forces boxed doubles on all platforms. Used for testing that all code paths handle boxed doubles correctly.

### Production Defaults

By default, Hermes ships with HV32 (HEAP_HV_PREFER32) on Android for memory savings, and HV64 on iOS for best performance on faster devices.

## Extensions System

Hermes supports JSI-based extensions that add runtime functionality. Extensions use the stable JSI API rather than internal Hermes APIs, making them easier to maintain.

### Directory Structure

- `API/hermes/extensions/` - Core extensions maintained by the Hermes team
- `API/hermes/extensions/contrib/` - Community-contributed extensions

### Adding Extensions

Each extension consists of:
1. A JavaScript file (`NN-ExtensionName.js`) with a setup function
2. C++ files (`ExtensionName.h/cpp`) that call the JS setup and optionally provide native helpers

See `API/hermes/extensions/README.md` for detailed instructions.

### Contrib Extensions

Community contributions go in `extensions/contrib/`. These are:
- Maintained by contributors, not the Hermes team
- Enabled by default, but can be disabled with `-DHERMES_ENABLE_CONTRIB_EXTENSIONS=OFF`
- May be promoted to core if widely adopted

See `API/hermes/extensions/contrib/README.md` for contributor guidelines.

## Specs and Plans

Before writing or reviewing a design spec or implementation plan, read
`DESIGN-PRINCIPLES.md` and apply it.

## Issue Tracking

Bugs and follow-ups that outlive a session live in `dz/`, a
[ditz2](https://github.com/tmikov/ditz2) project: one markdown file per
issue, committed alongside the code. Use it for what a commit message or
progress note cannot hold — a defect nobody is working on yet, a
limitation recorded rather than fixed, a follow-up that would otherwise
be forgotten.

Run the `dz` command from `PATH`. **If `dz` is not available, ask the
user how to proceed** — do not install it, build it, or substitute
another tool.

- `dz help agents` is the contract for programmatic use: every command
  takes `--json`, exit 0 means stdout parses, errors go to stderr as one
  object.
- The component list starts empty and `--component` rejects every value
  until it is populated. Add one when an issue needs it:
  `dz component add <name>`.
- `dz/config.local.yaml` (author identity) and `dz/.lock` are gitignored
  (by `dz/.gitignore`); `dz/config.yaml` and `dz/issues/*.md` are
  committed.

## Environment-Specific Instructions

This project exists in two environments. The presence of the `facebook/` directory indicates which environment you are in. **Only read the file corresponding to the current environment:**

- If `facebook/` directory exists: Internal Meta repository using Mercurial (hg) and BUCK. Read `facebook/CLAUDE-meta.md` for Meta-specific instructions. Do not read `CLAUDE-github.md`.
- If `facebook/` directory does not exist: Public GitHub repository using Git. Read `CLAUDE-github.md` for GitHub-specific instructions.
