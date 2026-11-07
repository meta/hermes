/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SamplingProfilerSampler.h"

#if defined(HERMESVM_SAMPLING_PROFILER_WINDOWS)

#include "hermes/VM/Profiler/SamplingProfiler.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Windows.h"

namespace hermes {
namespace vm {

/// Open the current thread and \return a handle to it. The handle must later
/// be closed with a call to the Win32 CloseHandle API.
static HANDLE openCurrentThread() {
  return OpenThread(
      THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
      false,
      GetCurrentThreadId());
}

namespace sampling_profiler {
namespace {
struct SamplingProfilerWindows : SamplingProfiler {
  SamplingProfilerWindows(Runtime &rt) : SamplingProfiler(rt) {
    currentThread_ = openCurrentThread();

    // Note that we cannot register this in the base class constructor, because
    // all fields must be initialized before we register with the profiling
    // thread.
    sampling_profiler::Sampler::get()->registerRuntime(this);
  }

  ~SamplingProfilerWindows() override {
    // TODO(T125910634): re-introduce the requirement for destroying the
    // sampling profiler on the same thread in which it was created.
    Sampler::get()->unregisterRuntime(this);

    CloseHandle(currentThread_);
  }

  /// Thread that this profiler instance represents. This can be updated
  /// by later calls to SetRuntimeThread.
  HANDLE currentThread_;
};
} // namespace

namespace {
struct SamplerWindows : Sampler {
  SamplerWindows();
  ~SamplerWindows() override;
};
} // namespace

Sampler *Sampler::get() {
  // We intentionally leak this memory to avoid a case where instance is
  // accessed after it is destroyed during shutdown.
  static SamplerWindows *instance = new SamplerWindows{};
  return instance;
}

SamplerWindows::~SamplerWindows() = default;

SamplerWindows::SamplerWindows() = default;

bool Sampler::platformEnable() {
  return true;
}

bool Sampler::platformDisable() {
  return true;
}

void Sampler::platformRegisterRuntime(SamplingProfiler *profiler) {}

void Sampler::platformUnregisterRuntime(SamplingProfiler *profiler) {}

void Sampler::platformPostSampleStack(SamplingProfiler *localProfiler) {}

SampleResult Sampler::platformSuspendVMAndWalkStack(
    SamplingProfiler *profiler) {
  auto *winProfiler = static_cast<SamplingProfilerWindows *>(profiler);

  // Suspend the JS thread. The runtimeDataLock is held by the caller, ensuring
  // the runtime won't start to be used on another thread before sampling
  // begins.
  DWORD prevSuspendCount = SuspendThread(winProfiler->currentThread_);
  if (prevSuspendCount == static_cast<DWORD>(-1)) {
    return SampleResult::Success;
  }

  // Get the JS thread context. This ensures that the thread suspension is
  // completed.
  CONTEXT context;
  GetThreadContext(winProfiler->currentThread_, &context);

  // Walk the stack.
  walkRuntimeStack(profiler, SamplingProfiler::MayAllocate::No);

  // Resume the thread.
  prevSuspendCount = ResumeThread(winProfiler->currentThread_);
  assert(
      prevSuspendCount != static_cast<DWORD>(-1) &&
      "couldn't resume js thread");
  (void)prevSuspendCount;

  return SampleResult::Success;
}
} // namespace sampling_profiler

std::unique_ptr<SamplingProfiler> SamplingProfiler::create(Runtime &rt) {
  return std::make_unique<sampling_profiler::SamplingProfilerWindows>(rt);
}

bool SamplingProfiler::belongsToCurrentThread() {
  auto profiler =
      static_cast<sampling_profiler::SamplingProfilerWindows *>(this);
  std::lock_guard<std::mutex> lock(profiler->runtimeDataLock_);
  return GetThreadId(profiler->currentThread_) == GetCurrentThreadId();
}

void SamplingProfiler::setRuntimeThread() {
  auto profiler =
      static_cast<sampling_profiler::SamplingProfilerWindows *>(this);
  std::lock_guard<std::mutex> lock(profiler->runtimeDataLock_);
  CloseHandle(profiler->currentThread_);
  profiler->currentThread_ = openCurrentThread();
  threadID_ = oscompat::global_thread_id();
  threadNames_[threadID_] = oscompat::thread_name();
}

namespace sampling_profiler {
void Sampler::onRegisteredThreadExit(SamplingProfiler *) {
  // Not applicable on Windows: the sampling path uses HANDLE +
  // SuspendThread / ResumeThread rather than pthread_kill, and does not
  // abort on a stale HANDLE.
}
} // namespace sampling_profiler

} // namespace vm
} // namespace hermes
#endif // !defined(HERMESVM_SAMPLING_PROFILER_WINDOWS)
