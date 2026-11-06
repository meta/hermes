/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JIT_JIT_H
#define HERMES_VM_JIT_JIT_H

#include "hermes/VM/JIT/Config.h"

#if HERMESVM_JIT

#if HERMESVM_JIT_ARM64
#include "hermes/VM/JIT/arm64/JIT.h"
#elif HERMESVM_JIT_X86_64
#include "hermes/VM/JIT/x86-64/JIT.h"
#endif

namespace hermes {
namespace vm {

#if HERMESVM_JIT_ARM64
using arm64::JITContext;
namespace arm64 {
class Emitter;
}
#define FRIEND_JIT                \
  friend class arm64::JITContext; \
  friend class arm64::Emitter;
#elif HERMESVM_JIT_X86_64
using x86_64::JITContext;
namespace x86_64 {
class Emitter;
}
#define FRIEND_JIT                 \
  friend class x86_64::JITContext; \
  friend class x86_64::Emitter;
#else
#error \
    "JIT enabled but no backend selects this architecture \
(HERMESVM_ALLOW_JIT=2 on an unsupported target?)"
#endif

} // namespace vm
} // namespace hermes

#else

#include "hermes/VM/CodeBlock.h"

#define FRIEND_JIT

namespace hermes {
namespace vm {

/// All state related to JIT compilation.
class JITContext {
 public:
  /// Construct a JIT context. No executable memory is allocated before it is
  /// needed.
  /// \param enable whether JIT is enabled.
  JITContext(bool enable) {}
  ~JITContext() = default;

  JITContext(const JITContext &) = delete;
  void operator=(const JITContext &) = delete;

  /// \return whether \p codeBlock should be JIT compiled.
  /// \pre codeBlock does not already have a JITCompiledFunctionPtr.
  /// Does not allocate.
  inline bool shouldCompile(CodeBlock *codeBlock) {
    return false;
  }

  /// Compile a function to native code and return the native pointer.
  /// \pre shouldCompile() must be true.
  /// \return the native pointer, nullptr if compilation failed.
  inline JITCompiledFunctionPtr compile(
      Runtime &runtime,
      CodeBlock *codeBlock) {
    return codeBlock->getJITCompiled();
  }

  /// \return true if JIT compilation is enabled.
  bool isEnabled() const {
    return false;
  }

  /// Enable or disable JIT compilation.
  void setEnabled(bool enabled) {}

  /// Enable or disable dumping JIT'ed Code.
  void setDumpJITCode(unsigned dump) {}

  /// \return true if dumping JIT'ed Code is enabled.
  unsigned getDumpJITCode() {
    return 0;
  }

  /// Construct data structure used for perf profiling support. This should be
  /// called only when PerfProf is enabled and perf JITContext.
  /// \param jitdumpFd The file descriptor of the opened jitdump file.
  /// \param commentFd The file descriptor of the opended \p commentFile.
  /// \param commentFile The path of the file to store the comments.
  void initPerfProfData(
      int jitdumpFd,
      int commentFd,
      const std::string &commentFile) {}

  /// Set the flag to fatally crash on JIT compilation errors.
  void setCrashOnError(bool crash) {}

  /// \return true if we should fatally crash on JIT compilation errors.
  bool getCrashOnError() {
    return false;
  }

  /// Set the flag to force jitting of all functions.
  void setForceJIT(bool force) {}

  /// Set the memory limit for JIT'ed code in bytes.
  void setMemoryLimit(uint32_t memoryLimit) {}

  /// Set the maximum number of recompiles per function (0 disables).
  void setMaxRecompiles(uint8_t maxRecompiles) {}

  /// \return the maximum number of recompiles per function.
  uint8_t getMaxRecompiles() const {
    return 0;
  }

  /// Set the ById declines within one body before considering a
  /// recompile (no-op without the JIT).
  void setRecompileDeclineThreshold(uint32_t threshold) {}

  /// \return the declines before a recompile is considered.
  uint32_t getRecompileDeclineThreshold() const {
    return 0;
  }

  /// Set the largest lazy JIT id assignable to a HiddenClass (testing only).
  void setHCIdLimit(uint32_t hcIdLimit) {}

  /// Set the default threshold for function execution count before a function
  /// is compiled. On a per-function basis, the count may be altered based on
  /// internal heuristics.
  /// Can be overridden by setForceJIT(true).
  void setDefaultExecThreshold(uint32_t threshold) {}

  /// Set the flag to emit asserts in the JIT'ed code.
  void setEmitAsserts(bool emitAsserts) {}

  /// Set the flag to verify FR type assumptions in the JIT'ed code.
  void setEmitTypeAsserts(bool emitTypeAsserts) {}

  /// Set whether we should emit counters in the JIT'ed code.
  void setEmitCounters(bool emitCounters) {}

  /// Dump the counters to the given stream. Counters must be enabled.
  void dumpCounters(llvh::raw_ostream &) {}

  /// \return true if we should emit asserts in the JIT'ed code.
  bool getEmitAsserts() {
    return false;
  }

  /// \return true if we should verify FR type assumptions in JIT'ed code.
  bool getEmitTypeAsserts() {
    return false;
  }

  /// Called by the GC at the beginning of a collection. This method informs the
  /// GC of all runtime roots.  The \p markLongLived argument
  /// indicates whether root data structures that contain only
  /// references to long-lived objects (allocated directly as long lived)
  /// are required to be scanned.
  void markRoots(RootAcceptorWithNames &acceptor, bool markLongLived) {}
};

} // namespace vm
} // namespace hermes

#endif // HERMESVM_JIT

#endif // HERMES_VM_JIT_JIT_H
