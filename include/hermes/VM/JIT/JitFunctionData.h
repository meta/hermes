/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JIT_JITFUNCTIONDATA_H
#define HERMES_VM_JIT_JITFUNCTIONDATA_H

#include "hermes/VM/JIT/Config.h"

#if HERMESVM_JIT

#include "hermes/VM/HermesValue.h"
#include "llvh/ADT/SmallVector.h"

#include <cstdint>
#include <memory>

namespace hermes {
namespace vm {

class Runtime;
class CodeBlock;
typedef HermesValue (*JITCompiledFunctionPtr)(Runtime *runtime);

/// Everything known about ONE compiled body: its code, and the feedback
/// recorded about that body's own execution. A version IS {its code, its
/// observations} -- observations made against one body do not describe
/// another, because a kind that reaches version 1's slow-path helper may
/// be handled inline by version 2 and never reach its helper at all. The
/// record is the recording helpers' sole identity argument: everything
/// function-level is reached through \c codeBlock. See
/// doc/superpowers/specs/2026-09-07-jit-version-data-design.md.
struct JitVersionData {
  /// The function this body was compiled from. Back-pointer used by the
  /// recording helpers to reach everything function-level.
  CodeBlock *codeBlock;
  /// This version's entry point, set at install. Null while the body is
  /// still a compilation candidate.
  JITCompiledFunctionPtr body = nullptr;
  /// This body's own helper-side decline counter. A retired body keeps
  /// counting into its own frozen record, where it influences nothing.
  uint32_t declineCount = 0;
  /// Declines this body must accumulate before its helper tail hands
  /// off to JITContext::considerRecompile. Snapshotted from the
  /// JITContext (i.e. from -Xjit-recompile-threshold) when this version
  /// is compiled, so the tail compares two fields of the one record it
  /// already holds and never walks to the CodeBlock or the JITContext.
  /// Never 0: the setter's caller clamps it up to 1.
  uint32_t declineThreshold;
  /// Number of Get/PutById sites whose inline tier this compile skipped
  /// because the property cache was cold. Derived at install as the
  /// clamped sum of coldWriteCacheIdxs and coldReadCacheIdxs below;
  /// feeds the cheap recompile trigger gate and the dump line.
  uint16_t coldByIdSites = 0;
  /// Write-cache indices whose specialization this compile skipped
  /// because the cache was cold. Written at install; consumed by
  /// considerRecompile's progress check to require that one of these
  /// specific sites -- not just some unrelated cache -- has since warmed.
  llvh::SmallVector<uint8_t, 4> coldWriteCacheIdxs;
  /// Read-cache indices whose specialization this compile skipped
  /// because the cache was cold. Same role as coldWriteCacheIdxs, for
  /// GetById sites.
  llvh::SmallVector<uint8_t, 4> coldReadCacheIdxs;
  /// Reserved for consumer-specific feedback records (e.g. the future
  /// PutByVal per-site observed-kind records), which are version-local
  /// by construction. Always null in v1; typed and owned by the consumer
  /// that allocates it. JitVersionData has no destructor for this field,
  /// so the consumer that allocates through this pointer must also own
  /// releasing it -- otherwise the records leak once per version.
  void *consumerRecords = nullptr;

  /// \param cb the function this body is compiled from.
  /// \param declineThreshold declines before considering a recompile.
  JitVersionData(CodeBlock *cb, uint32_t declineThreshold)
      : codeBlock(cb), declineThreshold(declineThreshold) {}
};

/// Per-function JIT metadata supporting recompilation: the control state
/// that spans versions, plus the per-version records. Allocated lazily
/// when a function is first JIT-compiled; owned by the CodeBlock; see
/// doc/superpowers/specs/2026-09-04-jit-recompilation-design.md and
/// doc/superpowers/specs/2026-09-07-jit-version-data-design.md.
struct JitFunctionData {
  /// Default declines of the ById helpers before the recompile check
  /// runs; the effective value is per-version and lives in
  /// JitVersionData::declineThreshold. Duplicated as a literal by the
  /// -Xjit-recompile-threshold flag and the RuntimeConfig field, which
  /// cannot include this header.
  static constexpr uint32_t kDefaultRecompileDeclineThreshold = 64;

  /// Remaining recompiles for this function. 0 disables all triggering.
  uint8_t recompileBudget;
  /// 1 after the first compile; incremented on each installed recompile.
  uint8_t version = 1;
  /// The record of the body future calls enter. Set at install, after
  /// the previous current has been retired.
  std::unique_ptr<JitVersionData> current{};
  /// Records of the previous bodies, retired by recompiles. Kept alive
  /// because a frame may still be executing a retired body: a record
  /// must outlive every possible execution of its body, and a retired
  /// body's helper calls still write into its record. The machine code
  /// itself is owned by JITContext::Impl and is never freed (see the
  /// reclamation dz issue).
  llvh::SmallVector<std::unique_ptr<JitVersionData>, 1> retired{};

  /// \param budget initial recompile budget (from -Xjit-max-recompiles).
  explicit JitFunctionData(uint8_t budget) : recompileBudget(budget) {}
};

} // namespace vm
} // namespace hermes

#endif // HERMESVM_JIT
#endif // HERMES_VM_JIT_JITFUNCTIONDATA_H
