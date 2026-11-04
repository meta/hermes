/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_X86_64
#include "hermes/VM/JIT/x86-64/JIT.h"

#include "JitImpl.h"

#define DEBUG_TYPE "jit"

namespace hermes {
namespace vm {
namespace x86_64 {

JITContext::JITContext(bool enable) : enabled_(enable) {
  // x86-64: the backend emits VEX encodings unconditionally; without AVX
  // the JIT stays disabled and the interpreter carries on.
  if (enabled_ && !__builtin_cpu_supports("avx"))
    enabled_ = false;
  if (!enabled_)
    return;
  impl_ = std::make_unique<Impl>();
}

JITContext::~JITContext() = default;

void JITContext::setHCIdLimit(uint32_t hcIdLimit) {
  if (impl_)
    impl_->hcIdLimit = std::min<uint32_t>(hcIdLimit, Impl::kHCIdOverflow);
}

void JITContext::dumpCounters(llvh::raw_ostream &os) {
  static constexpr const char *kCounterNames[] = {
#define COUNTER_NAME(name) #name,
      JIT_COUNTERS(COUNTER_NAME)
#undef COUNTER_NAME
  };
  for (unsigned i = 0; i < (unsigned)JitCounter::_Last; ++i)
    os << kCounterNames[i] << ": " << counters_[i] << "\n";

  // Slow-call-by-callee-kind histogram, see the counters_ layout comment
  // in JIT.h. Only print the kinds that were actually observed.
  for (unsigned k = 0; k < kNumCellKinds; ++k) {
    uint64_t count = counters_[(unsigned)JitCounter::_Last + k];
    if (count != 0)
      os << "NumCallSlow[" << cellKindStr((CellKind)k) << "]: " << count
         << "\n";
  }
}

void JITContext::markRoots(
    RootAcceptorWithNames &acceptor,
    bool markLongLived) {
  if (!impl_)
    return;
  acceptor.accept(impl_->usedHCs);
}

} // namespace x86_64
} // namespace vm
} // namespace hermes
#endif // HERMESVM_JIT_X86_64
