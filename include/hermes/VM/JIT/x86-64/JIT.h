/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JIT_X86_64_JIT_H
#define HERMES_VM_JIT_X86_64_JIT_H

#include "hermes/ADT/TransparentOwningPtr.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/JIT/JitCounters.h"
#include "hermes/VM/JIT/PerfJitDump.h"

namespace hermes {
namespace vm {
struct RuntimeOffsets;

namespace x86_64 {

namespace DumpJitCode {
enum : unsigned {
  Code = 0x01,
  CompileStatus = 0x02,
  InstErr = 0x04,
  BRK = 0x40,
  EntryExit = 0x80,
};
}

/// All state related to JIT compilation.
class JITContext {
  class Compiler;
  friend RuntimeOffsets;

 public:
  class Impl;

  /// Construct a JIT context. No executable memory is allocated before it is
  /// needed.
  /// \param enable whether JIT is enabled.
  /// On x86-64 the JIT additionally requires AVX; without it the
  /// context is constructed disabled.
  JITContext(bool enable);
  ~JITContext();

  JITContext(const JITContext &) = delete;
  void operator=(const JITContext &) = delete;

  /// \return whether \p codeBlock should be JIT compiled.
  /// \pre codeBlock does not already have a JITCompiledFunctionPtr.
  /// Does not allocate.
  inline bool shouldCompile(CodeBlock *codeBlock);

  /// Compile a function to native code and return the native pointer.
  /// \pre codeBlock does not already have a JITCompiledFunctionPtr.
  /// \pre shouldCompile() must be true.
  /// \return the native pointer, nullptr if compilation failed.
  inline JITCompiledFunctionPtr compile(Runtime &runtime, CodeBlock *codeBlock);

  /// \return true if JIT compilation is enabled.
  bool isEnabled() const {
    return enabled_;
  }

  /// Enable or disable JIT compilation. Enabling has no effect if the
  /// constructor left the JIT off (disabled, or no AVX), since there is no
  /// Impl to compile with.
  void setEnabled(bool enabled) {
    enabled_ = enabled && impl_;
  }

  /// Set the default threshold for function execution count before a function
  /// is compiled. On a per-function basis, the count may be altered based on
  /// internal heuristics.
  /// Can be overridden by setForceJIT(true).
  void setDefaultExecThreshold(uint32_t threshold) {
    defaultExecThreshold_ = threshold;
  }

  /// Enable or disable dumping JIT'ed Code.
  void setDumpJITCode(unsigned dump) {
    dumpJITCode_ = dump;
  }

  /// \return true if dumping JIT'ed Code is enabled.
  unsigned getDumpJITCode() {
    return dumpJITCode_;
  }

  /// Construct data structure used for perf profiling support. This should be
  /// called only when PerfProf is enabled and perf JITContext.
  /// \param jitdumpFd The file descriptor of the opened jitdump file.
  /// \param commentFd The file descriptor of the opended \p commentFile.
  /// \param commentFile The path of the file to store the comments.
  void initPerfProfData(
      int jitdumpFd,
      int commentFd,
      const std::string &commentFile) {
    assert(
        !perfJitDump_ &&
        "perfJitDump_ should be constructed once per JITContext");
    perfJitDump_ =
        std::make_unique<PerfJitDump>(jitdumpFd, commentFd, commentFile);
  }

  /// Set the flag to fatally crash on JIT compilation errors.
  void setCrashOnError(bool crash) {
    crashOnError_ = crash;
  }

  /// \return true if we should fatally crash on JIT compilation errors.
  bool getCrashOnError() {
    return crashOnError_;
  }

  /// Set the flag to force jitting of all functions.
  void setForceJIT(bool force) {
    forceJIT_ = force;
  }

  /// Set the memory limit for JIT'ed code in bytes.
  void setMemoryLimit(uint32_t memoryLimit) {
    memoryLimit_ = memoryLimit;
  }

  /// Set the largest lazy JIT id assignable to a HiddenClass. Exposed only so
  /// that tests can reach the exhaustion path without interning 65535 hidden
  /// classes; production code should leave this at the default.
  void setHCIdLimit(uint32_t hcIdLimit);

  /// Set the flag to emit asserts in the JIT'ed code.
  void setEmitAsserts(bool emitAsserts) {
    emitAsserts_ = emitAsserts;
  }

  /// Set the flag to verify FR type assumptions in the JIT'ed code.
  void setEmitTypeAsserts(bool emitTypeAsserts) {
    emitTypeAsserts_ = emitTypeAsserts;
  }

  /// Set whether we should emit counters in the JIT'ed code.
  void setEmitCounters(bool emitCounters) {
    assert(
        (emitCounters || !counters_.get()) && "Can't disable enabled counters");
    if (emitCounters && !counters_.get()) {
      counters_.reset((uint64_t *)checkedCalloc(
          (unsigned)JitCounter::_Last + kNumCellKinds, sizeof(uint64_t)));
    }
  }

  /// Dump the counters to the given stream. Counters must be enabled.
  void dumpCounters(llvh::raw_ostream &os);

  /// \return true if we should emit asserts in the JIT'ed code.
  bool getEmitAsserts() {
    return emitAsserts_;
  }

  /// \return true if we should verify FR type assumptions in JIT'ed code.
  bool getEmitTypeAsserts() {
    return emitTypeAsserts_;
  }

  /// Called by the GC at the beginning of a collection. This method informs the
  /// GC of all runtime roots.  The \p markLongLived argument
  /// indicates whether root data structures that contain only
  /// references to long-lived objects (allocated directly as long lived)
  /// are required to be scanned.
  void markRoots(RootAcceptorWithNames &acceptor, bool markLongLived);

 private:
  /// Slow path that actually performs the compilation of the specified
  /// CodeBlock.
  JITCompiledFunctionPtr compileImpl(Runtime &runtime, CodeBlock *codeBlock);

 private:
  /// Only initialized if JIT is enabled.
  std::unique_ptr<Impl> impl_{};

  /// Whether JIT compilation is enabled.
  bool enabled_{false};
  /// The memory limit for JIT'ed code in bytes.
  /// Once the limit is reached, no more code will be JIT'ed.
  uint32_t memoryLimit_{32u << 20};
  /// whether to dump JIT'ed code
  unsigned dumpJITCode_{0};
  /// whether to fatally crash on JIT compilation errors
  bool crashOnError_{false};
  /// Whether to emit asserts in the JIT'ed code.
  bool emitAsserts_{false};
  /// Whether to verify FR type assumptions in the JIT'ed code.
  bool emitTypeAsserts_{false};
  /// Whether to force jitting of all functions.
  /// If true, ignores the default exec threshold completely.
  bool forceJIT_{false};

  /// Generate jitdump for all jitted functions.
  std::unique_ptr<PerfJitDump> perfJitDump_{};

  /// The JIT threshold for function execution count.
  /// Lowered based on the loop depth before deciding whether to JIT.
  uint32_t defaultExecThreshold_ = 1 << 5;

  /// Array of counters for use by the emitted code. Laid out as the named
  /// counters from JIT_COUNTERS (indexed by JitCounter) first, immediately
  /// followed by kNumCellKinds slots holding the slow-call-by-callee-kind
  /// histogram: slot `(unsigned)JitCounter::_Last + (unsigned)kind` counts
  /// how many slow calls (see JitCounter::NumCallSlow) had a callee of that
  /// CellKind.
  TransparentOwningPtr<uint64_t, llvh::FreeDeleter> counters_;
};

LLVM_ATTRIBUTE_ALWAYS_INLINE
inline bool JITContext::shouldCompile(CodeBlock *codeBlock) {
  assert(!codeBlock->getJITCompiled() && "already compiled");

  if (LLVM_LIKELY(!enabled_))
    return false;
  if (LLVM_LIKELY(codeBlock->getDontJIT()))
    return false;

  uint32_t loopDepth = codeBlock->getFunctionHeader().getLoopDepth();
  // It's possible that if the loop depth is too high, we will set the
  // execThreshold to 0 for this function, but that's OK because we want to JIT
  // it immediately.
  assert(loopDepth <= 3 && "loopDepth is larger than expected");
  uint32_t execThreshold =
      forceJIT_ ? 0 : (defaultExecThreshold_ >> (loopDepth * 2));

  if (LLVM_LIKELY(codeBlock->getExecutionCount() < execThreshold))
    return false;

  return true;
}

LLVM_ATTRIBUTE_ALWAYS_INLINE
inline JITCompiledFunctionPtr JITContext::compile(
    Runtime &runtime,
    CodeBlock *codeBlock) {
  assert(!codeBlock->getJITCompiled() && "already compiled");
  assert(shouldCompile(codeBlock) && "should not be compiled");
  return compileImpl(runtime, codeBlock);
}

} // namespace x86_64
} // namespace vm
} // namespace hermes
#endif // HERMES_VM_JIT_X86_64_JIT_H
