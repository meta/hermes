# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.21.0)

# Set LTO policy
if(POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
endif()

# Set the VERSION variables based on the project command
if (POLICY CMP0048)
  cmake_policy(SET CMP0048 NEW)
endif()

# find_package uses <PackageName>_ROOT variables.
if (POLICY CMP0074)
  cmake_policy(SET CMP0074 NEW)
endif()
# Include file check macros honor CMAKE_REQUIRED_LIBRARIES.
if (POLICY CMP0075)
  cmake_policy(SET CMP0075 NEW)
endif()

# Only interpret if() arguments as variables or keywords when unquoted.
# CMake emits a warning if this is not set.
if (POLICY CMP0054)
  cmake_policy(SET CMP0054 NEW)
endif()

# Pick up a workaround for a CMake problem from LLVM r282552.
if(POLICY CMP0057)
  cmake_policy(SET CMP0057 NEW)
endif()

# Enable transitive library dependencies
if(POLICY CMP0022)
  cmake_policy(SET CMP0022 NEW)
endif()

# Require a build type
get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if ((NOT IS_MULTI_CONFIG) AND (NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL ""))
    message(FATAL_ERROR "Please set CMAKE_BUILD_TYPE")
endif()

# Has to be set before `project` as per documentation
# https://cmake.org/cmake/help/latest/variable/CMAKE_OSX_SYSROOT.html
set(CMAKE_OSX_SYSROOT ${HERMES_APPLE_TARGET_PLATFORM})

if(HERMES_APPLE_TARGET_PLATFORM MATCHES "catalyst")
  set(CMAKE_OSX_SYSROOT "macosx")
  set(CMAKE_THREAD_LIBS_INIT "-lpthread")
  set(CMAKE_HAVE_THREADS_LIBRARY 1)
  set(CMAKE_USE_WIN32_THREADS_INIT 0)
  set(CMAKE_USE_PTHREADS_INIT 1)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
endif()

if (WIN32)
add_definitions(-D_WINDOWS)
endif()

# This must be consistent with the release_version in:
# - android/build.gradle
# - npm/package.json
# - hermes-engine.podspec
project(Hermes
        VERSION 1.0.0
        LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/")

set(LLVH_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/llvh)

include(Hermes)
include(Lit)

# This is not a cache variable so that it is recomputed when the project
# version is updated.
if(NOT DEFINED HERMES_RELEASE_VERSION)
  set(HERMES_RELEASE_VERSION ${PROJECT_VERSION})
endif()

find_package(Python COMPONENTS Interpreter)
if (NOT Python_Interpreter_FOUND)
  message(FATAL_ERROR "Unable to find Python interpreter, required for builds and testing.
Please install Python or specify the PYTHON_EXECUTABLE CMake variable.")
endif()

# Project options.

set(HERMES_ENABLE_NAPI ON CACHE BOOL
  "Build Node-API support")

set(HERMES_IS_ANDROID OFF CACHE BOOL
  "Building for Android")

if(HERMES_IS_ANDROID OR HERMES_APPLE_TARGET_PLATFORM MATCHES "iphone|appletv")
  set(_HERMES_IS_MOBILE_BUILD_DEFAULT ON)
else()
  set(_HERMES_IS_MOBILE_BUILD_DEFAULT OFF)
endif()

set(HERMES_IS_MOBILE_BUILD ${_HERMES_IS_MOBILE_BUILD_DEFAULT} CACHE BOOL
  "Building for a mobile device")

set(HERMESVM_GCKIND HADES
  CACHE STRING
  "HermesVM GC type: MALLOC, HADES")

# Hermes VM opcode stats profiling
set(HERMESVM_PROFILER_OPCODE OFF CACHE BOOL
  "Enable opcode stats profiling in hermes VM")

# Hermes VM basic block profiling
set(HERMESVM_PROFILER_BB OFF CACHE BOOL
  "Enable basic block profiling in hermes VM")

# Hermes VM JS Function profiling
set(HERMESVM_PROFILER_JSFUNCTION OFF CACHE BOOL
  "Enable JS Function profiling in hermes VM")

# Hermes VM native call profiling
set(HERMESVM_PROFILER_NATIVECALL OFF CACHE BOOL
  "Enable native call profiling in hermes VM")

CHECK_CXX_SOURCE_COMPILES(
        "int main() { void *p = &&label; goto *p; label: return 0; }"
        HAVE_COMPUTED_GOTO)
if(HAVE_COMPUTED_GOTO)
    set(DEFAULT_INTERPRETER_THREADING ON)
else()
    set(DEFAULT_INTERPRETER_THREADING OFF)
endif()

set(HERMESVM_INDIRECT_THREADING ${DEFAULT_INTERPRETER_THREADING} CACHE BOOL
  "Enable the indirect threaded interpreter")

# Determine how HermesValue-s are encoded in the heap.
# - HEAP_HV_64: the default 64-bit encoding
# - HEAP_HV_PREFER32: on 32-bit systems use 32-bit encoding and boxed doubles.
#       On 64-bit systems, if the GC supports it, use 32-bit encoding with
#       compressed pointers and boxed doubles. Otherwise fall back to HEAP_HV_64.
# - HEAP_HV_BOXED: the encoding is either 32 or 64-bit depending on the system,
#       but always box doubles. This is used for testing that all paths can deal
#       with potential allocation of boxed doubles.
set(HERMESVM_HEAP_HV_MODE HEAP_HV_64 CACHE STRING
  "Heap HV mode: HEAP_HV_64, HEAP_HV_PREFER32, HEAP_HV_BOXED")

if(APPLE)
    set(DEFAULT_CONTIGUOUS_HEAP OFF)
else()
    set(DEFAULT_CONTIGUOUS_HEAP ON)
endif()

set(HERMESVM_ALLOW_CONTIGUOUS_HEAP ${DEFAULT_CONTIGUOUS_HEAP} CACHE BOOL
  "If this is on and compressed pointers are used, the heap will be allocated in a contiguous 4GB region.")

set(HERMESVM_ALLOW_HUGE_PAGES OFF CACHE BOOL
        "Enable huge pages to back the GC managed heap. Only useful on Linux.")

# Note that smaller heap segments will lower the maximum number of properties
# that can be stored in an object.
set(HERMESVM_LOG_HEAP_SEGMENT_SIZE 22
        CACHE STRING
        "Base 2 log of the size of segments in the GC managed heap in bytes.")

set(HERMESVM_ALLOW_CONCURRENT_GC ON CACHE BOOL
        "Enable concurrency in the GC for 64-bit builds.")

set(HERMESVM_ALLOW_INLINE_ASM ON CACHE BOOL
        "Allow the use of inline assembly in VM code.")

set(HERMESVM_API_TRACE_ANDROID_REPLAY OFF CACHE BOOL
  "Simulate Android config on Linux in API tracing.")

# Hermes VM Handle sanitization (moving the heap after every alloc)
set(HERMESVM_SANITIZE_HANDLES OFF CACHE BOOL
  "Enable Handle sanitization")

set(HERMESVM_CRASH_TRACE OFF CACHE BOOL
  "Enable recording of instructions for crash debugging depending on VMExperiments")

# Enable Address Sanitizer
set(HERMES_ENABLE_ADDRESS_SANITIZER OFF CACHE BOOL
  "Enable -fsanitize=address")

# Enable Undefined Behavior Sanitizer
set(HERMES_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER OFF CACHE BOOL
  "Enable -fsanitize=undefined")

# Enable Thread Sanitizer
set(HERMES_ENABLE_THREAD_SANITIZER OFF CACHE BOOL
  "Enable -fsanitize=thread")

# Enable Trace PC Guard
set(HERMES_ENABLE_TRACE_PC_GUARD OFF CACHE BOOL
  "Enable -fsanitize-coverage=trace-pc-guard")

set(HERMES_ENABLE_CODE_COVERAGE OFF CACHE BOOL
  "Enables code coverage to be collected from binaries. Coverage output will be placed in a subdirectory called \"coverage\" of the build directory.")

set(HERMES_ENABLE_LIBFUZZER OFF CACHE BOOL
  "Enable libfuzzer")

set(HERMES_ENABLE_FUZZILLI OFF CACHE BOOL
  "Enable fuzzilli")

set(HERMES_ENABLE_TOOLS ON CACHE BOOL
  "Enable CLI tools")

# Set linker flag for building the fuzzer
set(HERMES_FUZZING_FLAG "-fsanitize=fuzzer" CACHE STRING
  "Linker argument to link fuzz targets against a given fuzzer.")

# Build with -DHERMES_SLOW_DEBUG for debug builds
# This does not affect release builds
set(HERMES_SLOW_DEBUG ON CACHE BOOL
  "Enable slow checks in Debug builds")

# Build with -DHERMES_HARDENED for hardened builds
set(HERMES_HARDENED OFF CACHE BOOL
  "Enable compile-time security mitigations")

# On CentOS:
#   sudo yum install zlib-static glibc-static ncurses-static readline-static
set(HERMES_STATIC_LINK OFF CACHE BOOL
  "Link Hermes statically. May only work on GNU/Linux.")

set(HERMES_UNICODE_LITE OFF CACHE BOOL
  "Enable to use internal no-op unicode functionality instead of relying on underlying system libraries")

set(HERMES_CHECK_NATIVE_STACK ON CACHE BOOL
  "Check the native stack for stack overflow")

set(HERMES_ENABLE_DEBUGGER OFF CACHE BOOL
  "Build with debugger support")

set(HERMES_MEMORY_INSTRUMENTATION ${HERMES_ENABLE_DEBUGGER} CACHE BOOL
  "Build with memory instrumentation support")

set(HERMES_FACEBOOK_BUILD OFF CACHE BOOL
    "Build Facebook (rather than open-source) version of Hermes")

set(HERMES_ENABLE_CORE_EXTENSIONS ON CACHE BOOL
    "Include core JSI extensions (TextEncoder, etc.)")

set(HERMES_ENABLE_CONTRIB_EXTENSIONS ON CACHE BOOL
    "Include community-contributed extensions (can be disabled for critical deployments)")

# Contrib extensions require core extensions.
if(NOT HERMES_ENABLE_CORE_EXTENSIONS AND HERMES_ENABLE_CONTRIB_EXTENSIONS)
  message(STATUS "HERMES_ENABLE_CONTRIB_EXTENSIONS forced OFF because HERMES_ENABLE_CORE_EXTENSIONS is OFF")
  set(HERMES_ENABLE_CONTRIB_EXTENSIONS OFF)
endif()

set(HERMESVM_EXCEPTION_ON_OOM OFF CACHE BOOL
    "GC Out-of-memory raises an exception, rather than causing a crash")

set(HERMESVM_PLATFORM_LOGGING OFF CACHE BOOL
    "hermesLog(...) is enabled, using the platform's logging mechanism")

set(HERMES_ENABLE_WERROR OFF CACHE BOOL
  "Whether the build should have -Werror enabled")

set(HERMES_THREAD_SAFETY_ANALYSIS OFF CACHE BOOL
  "Whether to compile with clang's -Wthread-safety")

set(HERMES_GITHUB_RESOURCE_DIR "" CACHE STRING
  "A directory with additional files to bundle in the GitHub release")

set(ANDROID_LINUX_PERF_PATH ""
  CACHE STRING
  "If buildling for Android, full path to <linux/perf_events.h>")

set(HERMES_MSVC_MP ON CACHE STRING
  "Enable /MP in MSVC for parallel builds")

set(HERMES_ENABLE_INTL OFF CACHE BOOL
  "Enable JS Intl support (WIP)")

set(HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES ON CACHE BOOL
  "Enable RegExp Unicode Property Escapes support")

set(HERMES_ENABLE_TEST_SUITE ON CACHE BOOL
  "Enable the test suite")

set(HERMES_BUILD_APPLE_FRAMEWORK OFF CACHE BOOL
  "Whether to build the libhermes target as a framework bundle or dylib on Apple platforms")

set(HERMES_BUILD_LEAN_LIBHERMES OFF CACHE BOOL "Exclude the Hermes compiler from libhermes.")

set(HERMES_BUILD_SHARED_JSI OFF CACHE BOOL "Build JSI as a shared library.")

set(IMPORT_HOST_COMPILERS "" CACHE FILEPATH
  "Import the shermes and hermesc compiler from another build using the given CMake file.")

set(HERMESVM_INTERNAL_JAVASCRIPT_NATIVE OFF CACHE BOOL
  "Use natively compiled InternalJavaScript. Set OFF to use bytecode version")

# 0: JIT is disabled
# 1: JIT is enabled if supported by the platform
# 2: JIT is enabled
set(HERMESVM_ALLOW_JIT 0 CACHE STRING "JIT mode: 0 (off), 1 (auto) or 2 (force on)")

set(HERMES_ALLOW_BOOST_CONTEXT 1 CACHE STRING
   "Use Boost.Context fibers: 0 (off), 1 (auto) or 2 (force on)")

# Used in the compiler, for various compilation statistics.
set(LLVM_FORCE_ENABLE_STATS OFF CACHE BOOL
  "Enable gathering, printing of Statistics.")

set(QEMU_RUN_PREFIX "" CACHE STRING
  "Command prefix for invoking cross-compiled binaries with QEMU.")

set(JSI_UNSTABLE OFF CACHE BOOL "Enable the JSI unstable APIs.")

# Build the VM for the wasm2c-based sandbox in API/hermes_sandbox. This compiles
# out the setjmp/longjmp that shermes-compiled code and the JIT use for
# exceptions, since neither can run in the sandbox.
set(HERMES_SANDBOX_WASM OFF CACHE BOOL
  "Build the VM for the sandbox in API/hermes_sandbox.")

if (NOT QEMU_RUN_PREFIX STREQUAL "")
  add_definitions(-DQEMU_MODE)
endif()

# Used to force enable slow statistics even in release builds.
set(HERMES_FORCE_SLOW_STATS OFF CACHE BOOL
  "Force enable HERMES_SLOW_STATISTIC even when HERMES_SLOW_DEBUG is off.")

if(HERMES_FORCE_SLOW_STATS)
  # Also enable regular statistics when slow stats are forced on
  set(LLVM_FORCE_ENABLE_STATS ON)
endif()

# This definition is used by source that must be kept identical between Hermes and
# Static Hermes, but needs to behave differently in each case.
add_definitions(-DSTATIC_HERMES)

if (HERMES_UNICODE_LITE)
  add_definitions(-DHERMES_PLATFORM_UNICODE=HERMES_PLATFORM_UNICODE_LITE)
endif()

if(HERMES_CHECK_NATIVE_STACK)
  add_definitions(-DHERMES_CHECK_NATIVE_STACK)
endif()

if (HERMES_IS_MOBILE_BUILD)
  add_definitions(-DHERMES_IS_MOBILE_BUILD)
endif()

if (HERMESVM_INTERNAL_JAVASCRIPT_NATIVE)
  add_definitions(-DHERMESVM_INTERNAL_JAVASCRIPT_NATIVE)
endif()

if (HERMES_SANDBOX_WASM)
  if (NOT EMSCRIPTEN OR HERMESVM_INTERNAL_JAVASCRIPT_NATIVE OR
      NOT HERMESVM_ALLOW_JIT EQUAL 0)
    message(FATAL_ERROR "HERMES_SANDBOX_WASM requires Emscripten, "
      "HERMESVM_INTERNAL_JAVASCRIPT_NATIVE=OFF and HERMESVM_ALLOW_JIT=0")
  endif()
  add_definitions(-DHERMES_SANDBOX_WASM)
endif()

if (HERMES_STATIC_LINK)
  append("-static" CMAKE_EXE_LINKER_FLAGS)
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()

# Check if the linker supports deleting unused sections and ICF.
# These are GNU ld / lld flags, so skip them for MSVC.
if(NOT MSVC)
  check_linker_flag(CXX "LINKER:--gc-sections" HAVE_GC_SECTIONS)
  # Only check for -dead_strip on Apple because some linkers may interpret it
  # as "-d -e ad_strip".
  if(APPLE)
    check_linker_flag(CXX "LINKER:-dead_strip" HAVE_DEAD_STRIP)
  endif()
  check_linker_flag(CXX "LINKER:--icf=safe" HAVE_ICF)

  if(HAVE_GC_SECTIONS)
    add_flag_if_supported("-ffunction-sections" FUNCTION_SECTIONS)
    add_flag_if_supported("-fdata-sections" DATA_SECTIONS)
    list(APPEND HERMES_EXTRA_LINKER_FLAGS "LINKER:--gc-sections")
  elseif(HAVE_DEAD_STRIP)
    # This is similar to the above, but for macOS.
    list(APPEND HERMES_EXTRA_LINKER_FLAGS "LINKER:-dead_strip")
  endif()

  if(HAVE_ICF)
    add_flag_if_supported("-faddrsig" ADDRSIG)
    list(APPEND HERMES_EXTRA_LINKER_FLAGS "LINKER:--icf=safe")
  endif()
endif()

# Make the HERMES_RELEASE_VERSION accessible for version printing in C++.
if(HERMES_RELEASE_VERSION)
    add_definitions(-DHERMES_RELEASE_VERSION="${HERMES_RELEASE_VERSION}")
endif()

set(HERMES_PROFILER_MODE_IN_LIT_TEST "NONE")
if(HERMESVM_PROFILER_OPCODE)
    add_definitions(-DHERMESVM_PROFILER_OPCODE)
    set(HERMES_PROFILER_MODE_IN_LIT_TEST "OPCODE")
endif()
if(HERMESVM_PROFILER_BB)
    add_definitions(-DHERMESVM_PROFILER_BB)
    set(HERMES_PROFILER_MODE_IN_LIT_TEST "BB")
endif()
if(HERMESVM_PROFILER_JSFUNCTION)
    add_definitions(-DHERMESVM_PROFILER_JSFUNCTION)
    set(HERMES_PROFILER_MODE_IN_LIT_TEST "SAMPLING")
endif()
if(HERMESVM_PROFILER_NATIVECALL)
    add_definitions(-DHERMESVM_PROFILER_NATIVECALL)
    set(HERMES_PROFILER_MODE_IN_LIT_TEST "EXTERN")
endif()
if(HERMESVM_INDIRECT_THREADING)
    add_definitions(-DHERMESVM_INDIRECT_THREADING)
endif()

# Configure HERMESVM_COMPRESSED_POINTERS and HERMESVM_BOXED_DOUBLES. There are
# only three valid states:
#   COMPRESSED=false BOXED=false  => Heap uses 64-bit HV
#   COMPRESSED=true  BOXED=true   => 64-bit native ptr, heap uses 32-bit HV with
#                                    compressed pointers
#   COMPRESSED=false BOXED=true   => Heap uses native-ptr size HV
#
# Additionally, only with COMPRESSED=true, if allowed, HERMESVM_CONTIGUOUS_HEAP
# is set.
set(HERMESVM_COMPRESSED_POINTERS OFF)
set(HERMESVM_BOXED_DOUBLES OFF)
set(HERMESVM_CONTIGUOUS_HEAP OFF)
if(HERMESVM_HEAP_HV_MODE STREQUAL "HEAP_HV_64")
  # Do nothing.
elseif(HERMESVM_HEAP_HV_MODE STREQUAL "HEAP_HV_PREFER32")
  if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    # Box doubles on 32-bit systems.
    set(HERMESVM_BOXED_DOUBLES ON)
  else()
    # Compressed pointers are only enabled on 64 bit systems with Hades GC.
    if (HERMESVM_GCKIND MATCHES HADES)
      set(HERMESVM_COMPRESSED_POINTERS ON)
      set(HERMESVM_BOXED_DOUBLES ON)
      if(HERMESVM_ALLOW_CONTIGUOUS_HEAP)
        set(HERMESVM_CONTIGUOUS_HEAP ON)
      endif()
    endif()
  endif()
elseif(HERMESVM_HEAP_HV_MODE STREQUAL "HEAP_HV_BOXED")
  set(HERMESVM_BOXED_DOUBLES ON)
else()
  message(SEND_ERROR "Invalid HERMESVM_HEAP_HV_MODE: ${HERMESVM_HEAP_HV_MODE}")
endif()
if (HERMESVM_COMPRESSED_POINTERS)
  add_definitions(-DHERMESVM_COMPRESSED_POINTERS)
endif()
if (HERMESVM_BOXED_DOUBLES)
  add_definitions(-DHERMESVM_BOXED_DOUBLES)
endif()
if (HERMESVM_CONTIGUOUS_HEAP)
  add_definitions(-DHERMESVM_CONTIGUOUS_HEAP)
endif()

if(HERMESVM_ALLOW_HUGE_PAGES)
    add_definitions(-DHERMESVM_ALLOW_HUGE_PAGES)
endif()
add_definitions(-DHERMESVM_LOG_HEAP_SEGMENT_SIZE=${HERMESVM_LOG_HEAP_SEGMENT_SIZE})
if(HERMESVM_ALLOW_CONCURRENT_GC)
    add_definitions(-DHERMESVM_ALLOW_CONCURRENT_GC)
endif()
if(HERMESVM_ALLOW_INLINE_ASM)
    add_definitions(-DHERMESVM_ALLOW_INLINE_ASM)
endif()
if(HERMESVM_API_TRACE_ANDROID_REPLAY)
    add_definitions(-DHERMESVM_API_TRACE_ANDROID_REPLAY)
endif()
if(HERMESVM_CRASH_TRACE)
    add_definitions(-DHERMESVM_CRASH_TRACE=1)
endif()
if (HERMES_ENABLE_ADDRESS_SANITIZER)
    # Leave frame pointers. Allows the fast unwinder to function properly.
    append("-fsanitize=address -fno-omit-frame-pointer" CMAKE_CXX_FLAGS CMAKE_C_FLAGS)
    # MSVC handles ASAN runtime linking automatically when objects compiled with
    # /fsanitize=address are linked. Don't pass the flag to the linker on MSVC.
    if (MSVC)
        # MSVC needs these debug information for ASAN to report errors correctly.
        append("/Zi" CMAKE_CXX_FLAGS CMAKE_C_FLAGS)
        append("/DEBUG" CMAKE_EXE_LINKER_FLAGS)
        # Disable asan sanitization on contaienrs, which causes redefinition error
        # on VS2022 when building llvh.
        add_compile_definitions(_DISABLE_VECTOR_ANNOTATION)

        get_filename_component(MSVC_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
        # Define the paths to the ASAN Release and Debug DLLs.
        set(ASAN_RELEASE_DLL "${MSVC_BIN_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        set(ASAN_DEBUG_DLL   "${MSVC_BIN_DIR}/clang_rt.asan_dbg_dynamic-x86_64.dll")
    else()
        append("-fsanitize=address" CMAKE_EXE_LINKER_FLAGS)
        # GCC does not automatically link libpthread when using ASAN
        append("-lpthread" CMAKE_EXE_LINKER_FLAGS)
    endif()
endif()
if (HERMES_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER)
    # MSVC does not support UBSan
    if (MSVC)
        message(WARNING "Undefined Behavior Sanitizer is not supported on MSVC, ignoring HERMES_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER")
    else()
        add_definitions(-DHERMES_UBSAN)
        # Do not enable the vptr sanitizer, as it requires RTTI.
        append("-fsanitize=undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined" CMAKE_CXX_FLAGS CMAKE_C_FLAGS CMAKE_EXE_LINKER_FLAGS)
        # Function signature sanitization produces false positives on Apple platforms.
        # https://github.com/llvm/llvm-project/issues/109074
        if (APPLE)
            add_flag_if_supported("-fno-sanitize=function" APPLE_NO_SANITIZE_FUNCTION)
        endif()
    endif()
endif()
if (HERMES_ENABLE_THREAD_SANITIZER)
    # MSVC does not support TSan.
    if (MSVC)
        message(WARNING "Thread Sanitizer is not supported on MSVC, ignoring HERMES_ENABLE_THREAD_SANITIZER")
    else()
        append("-fsanitize=thread" CMAKE_CXX_FLAGS CMAKE_C_FLAGS CMAKE_EXE_LINKER_FLAGS)
    endif()
endif()
if (HERMES_ENABLE_TRACE_PC_GUARD)
    append("-fsanitize-coverage=trace-pc-guard" CMAKE_CXX_FLAGS CMAKE_C_FLAGS CMAKE_EXE_LINKER_FLAGS)
endif()
if (HERMES_ENABLE_CODE_COVERAGE)
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        append("-fprofile-instr-generate -fcoverage-mapping" CMAKE_CXX_FLAGS CMAKE_C_FLAGS)
    else()
        message(FATAL_ERROR "Code coverage flags not defined for this compiler: ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()
if(HERMES_FACEBOOK_BUILD)
    add_definitions(-DHERMES_FACEBOOK_BUILD)
endif()
if(HERMESVM_EXCEPTION_ON_OOM)
    set(HERMES_ENABLE_EH ON)
    add_definitions(-DHERMESVM_EXCEPTION_ON_OOM)
endif()
if(HERMESVM_PLATFORM_LOGGING)
    add_definitions(-DHERMESVM_PLATFORM_LOGGING)
endif()
if (NOT (ANDROID_LINUX_PERF_PATH STREQUAL ""))
  add_definitions(-DANDROID_LINUX_PERF_PATH="${ANDROID_LINUX_PERF_PATH}")
endif()

if (HERMES_ENABLE_INTL)
  add_definitions(-DHERMES_ENABLE_INTL)
endif()

if (HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES)
  add_definitions(-DHERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES)
endif()

if (HERMES_ENABLE_WERROR)
  # Turn all warnings into errors on GCC-compatible compilers.
  if (GCC_COMPATIBLE)
    append("-Werror" CMAKE_CXX_FLAGS CMAKE_C_FLAGS)
  endif()
endif()

# Collect all header files and add them to the IDE.
file(GLOB_RECURSE ALL_HEADER_FILES "*.h")

if(HERMES_SLOW_DEBUG)
  # Enable HERMES_SLOW_DEBUG in Debug mode
  set_property(DIRECTORY APPEND PROPERTY
      COMPILE_DEFINITIONS $<$<CONFIG:Debug>:HERMES_SLOW_DEBUG>)
endif()

if (HERMES_HARDENED)
  add_definitions(-DHERMES_HARDENED)
endif()

if (HERMES_ALLOW_BOOST_CONTEXT EQUAL 0)
    set(HERMES_USE_BOOST_CONTEXT 0)
elseif (HERMES_ALLOW_BOOST_CONTEXT EQUAL 2)
    set(HERMES_USE_BOOST_CONTEXT 1)
elseif (HERMES_ALLOW_BOOST_CONTEXT EQUAL 1)
    if (HERMES_ENABLE_ADDRESS_SANITIZER OR EMSCRIPTEN)
        set(HERMES_USE_BOOST_CONTEXT 0)
    else()
        set(HERMES_USE_BOOST_CONTEXT 1)
    endif()
else ()
    message(FATAL_ERROR "Invalid HERMES_ALLOW_BOOST_CONTEXT value ${HERMES_ALLOW_BOOST_CONTEXT}")
endif ()
# Use BOOST_CONTEXT_LIB to rever to boost_context as a dependency, regardless of
# whether it is enabled.
if (HERMES_USE_BOOST_CONTEXT)
    set(BOOST_CONTEXT_LIB boost_context)
else()
    set(BOOST_CONTEXT_LIB )
endif()
add_definitions(-DHERMES_USE_BOOST_CONTEXT=${HERMES_USE_BOOST_CONTEXT})

if(HERMES_FORCE_SLOW_STATS)
  add_definitions(-DHERMES_FORCE_SLOW_STATS)
endif()


if (GCC_COMPATIBLE)
  # Don't export symbols unless we explicitly say so
  append("-fvisibility=hidden" CMAKE_CXX_FLAGS CMAKE_C_FLAGS)
  # We depend on reading/writing certain properties in C++ objects in generated
  # C code through C structs that mirror those C++ objects. This means that
  # unrelated types may alias in our code, and we must disable strict aliasing.
  # We would also like to avoid arbitrary UB on signed integer and pointer
  # overflow, so disable strict overflow as well.
  append("-fno-strict-aliasing -fno-strict-overflow" CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
elseif ("${CMAKE_CXX_COMPILER_ID}" MATCHES "MSVC")
  # C4068 unknown pragma
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -wd4068")
  # C4200 nonstandard extension used: zero-sized array in struct/union
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -wd4200")
  # C4201 nonstandard extension used: nameless struct/union
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -wd4201")
  # C4530 C++ exception handler used, but unwind semantics are not enabled
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -wd4530")
  # Parallelize build
  if (HERMES_MSVC_MP)
    add_definitions( /MP )
  endif()
  # MSVC does not have strict aliasing so there is no need to support it.
  # Similar to GCC/Clang above, disable strict signed integer overflow.
  # TODO: Investigate disabling strict signed integer overflow for MSVC
  # append("/d2UndefIntOverflow-" CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
endif()

# Export a JSON file with the compilation commands that external tools can use
# to analyze the source code of the project.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


if (APPLE)
  find_library(CORE_FOUNDATION CoreFoundation)
  find_library(FOUNDATION Foundation)
else()
  set(CORE_FOUNDATION "")
  set(FOUNDATION "")
endif()

if (HERMES_ENABLE_DEBUGGER)
  add_definitions(-DHERMES_ENABLE_DEBUGGER)
endif()

if (HERMES_MEMORY_INSTRUMENTATION)
  add_definitions(-DHERMES_MEMORY_INSTRUMENTATION)
endif()

set(CMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++17")

if (JSI_UNSTABLE)
    add_definitions(-DJSI_UNSTABLE)
endif()
# JSI_DIR has always priority over other folders.
# It's used by React Native when building Hermes from source so
# the copy of JSI inside `react-native/ReactCommon/jsi` can be used.
if(JSI_DIR)
  if(EXISTS ${JSI_DIR})
    set(HERMES_JSI_DIR ${JSI_DIR})
  else()
    message(FATAL_ERROR "You specified a JSI directory with -DJSI_DIR but JSI can't be found there.")
  endif()
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/API/jsi)
  set(HERMES_JSI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/API/jsi)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../jsi)
  set(HERMES_JSI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../jsi)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../jsi)
  set(HERMES_JSI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../jsi)
else()
  message(FATAL_ERROR "Unable to find jsi.")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../hermes_common)
  set(HERMES_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../hermes_common)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../hermes_common)
  set(HERMES_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../hermes_common)
else()
  set(HERMES_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

include_directories(
  external/llvh/include
  external/llvh/gen/include
  ${CMAKE_CURRENT_BINARY_DIR}/external/llvh/include
)

include_directories(BEFORE
  ${CMAKE_CURRENT_BINARY_DIR}/lib/config
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/external
  )

if(HERMES_IS_ANDROID)
  find_package(fbjni REQUIRED CONFIG)
endif()

if("${CMAKE_CXX_COMPILER_ID}" MATCHES "MSVC")
  # MSVC requires c++20, because of the use of designated initializers in the codebase.
  set(CMAKE_CXX_STANDARD 20)
else()
  set(CMAKE_CXX_STANDARD 17)
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(external)
add_subdirectory(utils/hermes-lit)
add_subdirectory(lib)
add_subdirectory(public)
add_subdirectory(API)
add_subdirectory(android/intltest/java/com/facebook/hermes/test)

if(HERMES_ENABLE_TOOLS)
  add_subdirectory(tools)
endif()

# Make sure JSI is compiled with PIC
set(save_CMAKE_POSITION_INDEPENDENT_CODE ${CMAKE_POSITION_INDEPENDENT_CODE})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(save_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS ${HERMES_BUILD_SHARED_JSI})
add_subdirectory(${HERMES_JSI_DIR}/jsi ${CMAKE_CURRENT_BINARY_DIR}/jsi)
set(BUILD_SHARED_LIBS ${save_BUILD_SHARED_LIBS})
set(CMAKE_POSITION_INDEPENDENT_CODE ${save_CMAKE_POSITION_INDEPENDENT_CODE})

# Configure the test suites
#
if(HERMES_ENABLE_TEST_SUITE)
  if(NOT HERMES_ENABLE_TOOLS)
    message(FATAL_ERROR, "Running the test-suite requires the CLI tools to be built.")
  endif()

  add_subdirectory(unittests)
  add_subdirectory(test/static_asserts)

  list(APPEND HERMES_TEST_DEPS
    HermesUnitTests
    hermes
    hermesc
    hvm
    interp-dispatch-bench
    hdb
    hbcdump
    hbc-attribute
    hbc-deltaprep
    hbc-diff
    dependency-extractor
    shermes-dep
    compileJS
    emhermesc
    hermes-jsi
    hermes-jsi-lean
    hermes-jsi-shared
    hermes-jsi-lean-shared
    )

  if(HERMES_ENABLE_NAPI)
    list(APPEND HERMES_TEST_DEPS
      napi-runner
    )
    if(NOT WIN32)
      list(APPEND HERMES_TEST_DEPS
        napi_test_addon
        napi_ref_addon
        napi_text_encoder_addon
        napi_async_work_addon
      )
    endif()
  endif()

  set(coverage_directory "")
  if (HERMES_ENABLE_CODE_COVERAGE)
    set(coverage_directory ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/coverage)
  endif()

  set(HERMES_LIT_TEST_PARAMS_BASE
    test_exec_root=${CMAKE_CURRENT_BINARY_DIR}/test
    unittests_dir=${CMAKE_CURRENT_BINARY_DIR}/unittests
    debugger_enabled=${HERMES_ENABLE_DEBUGGER}
    jit_enabled=${HERMESVM_ALLOW_JIT}
    jit_arch=${CMAKE_SYSTEM_PROCESSOR}
    intl_enabled=${HERMES_ENABLE_INTL}
    regexp_unicode_properties_enabled=${HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES}
    hbc_deltaprep=${HERMES_TOOLS_OUTPUT_DIR}/hbc-deltaprep
    dependency_extractor=${HERMES_TOOLS_OUTPUT_DIR}/dependency-extractor
    FileCheck=${HERMES_TOOLS_OUTPUT_DIR}/FileCheck
    hermes=${HERMES_TOOLS_OUTPUT_DIR}/hermes
    hermesc=${HERMES_TOOLS_OUTPUT_DIR}/hermesc
    hdb=${HERMES_TOOLS_OUTPUT_DIR}/hdb
    hbcdump=${HERMES_TOOLS_OUTPUT_DIR}/hbcdump
    hbc-deltaprep=${HERMES_TOOLS_OUTPUT_DIR}/hbc-deltaprep
    hbc_diff=${HERMES_TOOLS_OUTPUT_DIR}/hbc-diff
    slow_debug=$<IF:$<CONFIG:Debug>,${HERMES_SLOW_DEBUG},OFF>
    debug_options=$<IF:$<CONFIG:Debug>,ON,OFF>
    exception_on_oom_enabled=${HERMESVM_EXCEPTION_ON_OOM}
    profiler=${HERMES_PROFILER_MODE_IN_LIT_TEST}
    gc=${HERMESVM_GCKIND}
    heap_hv_mode=${HERMESVM_HEAP_HV_MODE}
    ubsan=${HERMES_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER}
    asan=${HERMES_ENABLE_ADDRESS_SANITIZER}
    handle_san=${HERMESVM_SANITIZE_HANDLES}
    coverage=${coverage_directory}
    shermes=${HERMES_TOOLS_OUTPUT_DIR}/shermes
    check_native_stack=${HERMES_CHECK_NATIVE_STACK}
    unicode_lite=${HERMES_UNICODE_LITE}
    core_extensions_enabled=${HERMES_ENABLE_CORE_EXTENSIONS}
    contrib_extensions_enabled=${HERMES_ENABLE_CONTRIB_EXTENSIONS}
    jsi_unstable=${JSI_UNSTABLE}
    qemu_run_prefix=${QEMU_RUN_PREFIX}
    napi_enabled=${HERMES_ENABLE_NAPI}
    python=${Python_EXECUTABLE}
    )

  if(HERMES_ENABLE_NAPI)
    list(APPEND HERMES_LIT_TEST_PARAMS_BASE
      napi_runner=${HERMES_TOOLS_OUTPUT_DIR}/napi-runner
      napi_addon_dir=${CMAKE_CURRENT_BINARY_DIR}/unittests/napi
    )
  endif()

  set(HERMES_LIT_TEST_PARAMS
    ${HERMES_LIT_TEST_PARAMS_BASE}
    FileCheckOrRegen=${HERMES_TOOLS_OUTPUT_DIR}/FileCheck
    )

  set(HERMES_LIT_UPDATE_PARAMS
    ${HERMES_LIT_TEST_PARAMS_BASE}
    "FileCheckOrRegen=${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/utils/lit-expect-gen/generate.py"
    )

  set(LLVH_LIT_ARGS "-sv")

  add_lit_testsuite(check-hermes "Running the Hermes regression tests"
    ${CMAKE_CURRENT_SOURCE_DIR}/test
    ${CMAKE_CURRENT_SOURCE_DIR}/unittests
    PARAMS ${HERMES_LIT_TEST_PARAMS}
    DEPENDS ${HERMES_TEST_DEPS}
    ARGS ${HERMES_TEST_EXTRA_ARGS}
    )
  set_target_properties(check-hermes PROPERTIES FOLDER "Hermes regression tests")
  if(HERMES_ENABLE_NAPI)
    add_dependencies(check-hermes check-napi)
  endif()

  list(APPEND SHERMES_TEST_DEPS shermes-dep)
  add_lit_testsuite(check-shermes "Running the Static Hermes regression tests"
    ${CMAKE_CURRENT_SOURCE_DIR}/test/shermes
    PARAMS ${HERMES_LIT_TEST_PARAMS}
    DEPENDS ${SHERMES_TEST_DEPS}
    ARGS ${HERMES_TEST_EXTRA_ARGS}
    )
  set_target_properties(check-shermes PROPERTIES FOLDER "Static Hermes regression tests")

  # update-lit will regenerate the expectations for all tests that are verified with FileCheckOrRegen.
  # All other tests are run normally.
  add_lit_testsuite(update-lit "Running the Hermes regression tests, and updating auto-generated expectations."
    ${CMAKE_CURRENT_SOURCE_DIR}/test
    ${CMAKE_CURRENT_SOURCE_DIR}/unittests
    PARAMS ${HERMES_LIT_UPDATE_PARAMS}
    DEPENDS ${HERMES_TEST_DEPS}
    ARGS ${HERMES_TEST_EXTRA_ARGS}
    )
  set_target_properties(update-lit PROPERTIES FOLDER "Hermes regression tests")
endif()
