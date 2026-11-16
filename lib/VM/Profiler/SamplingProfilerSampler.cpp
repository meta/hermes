/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SamplingProfilerSampler.h"

#if HERMESVM_SAMPLING_PROFILER_AVAILABLE

#include "hermes/VM/Callable.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/StackFrame-inline.h"

#include "llvh/Support/Compiler.h"

#include "TraceSerializer.h"

#include <fcntl.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <csignal>
#include <random>
#include <thread>

#if defined(_WINDOWS)
#include <windows.h>
// Must be included after windows.h
#include <mmsystem.h>
#include "llvh/ADT/ScopeExit.h"
#endif

namespace hermes {
namespace vm {
namespace sampling_profiler {

Sampler::~Sampler() = default;

Sampler::Sampler() {
  // Reserve some initial space for samples
  sampleStorage_.stack.reserve(50);
}

void Sampler::registerRuntime(SamplingProfiler *profiler) {
  std::lock_guard<std::mutex> lockGuard(profilerLock_);
  profilers_.insert(profiler);
  platformRegisterRuntime(profiler);
}

void Sampler::unregisterRuntime(SamplingProfiler *profiler) {
  std::lock_guard<std::mutex> lockGuard(profilerLock_);
  bool succeed = profilers_.erase(profiler);
  // TODO: should we allow recursive style
  // register/register -> unregister/unregister call?
  assert(succeed && "How can runtime not registered yet?");
  (void)succeed;
  platformUnregisterRuntime(profiler);
}

bool Sampler::sampleStacks() {
  for (SamplingProfiler *localProfiler : profilers_) {
    std::lock_guard<std::mutex> lk(localProfiler->runtimeDataLock_);
    switch (sampleStack(localProfiler)) {
      case SampleResult::Success:
        platformPostSampleStack(localProfiler);
        break;
      case SampleResult::ThreadExited:
        // The registered thread is gone; skip this profiler but keep
        // sampling the others.
        continue;
      case SampleResult::Failed:
        return false;
    }
  }
  return true;
}

SampleResult Sampler::sampleStack(SamplingProfiler *localProfiler) {
  if (localProfiler->suspendCount_ > 0) {
    // Sampling profiler is suspended. Copy pre-captured stack instead without
    // interrupting the VM thread.
    localProfiler->sampledStacks_.emplace_back(
        localProfiler->preSuspendStackStorage_.tid,
        localProfiler->preSuspendStackStorage_.timeStamp,
        localProfiler->preSuspendStackStorage_.stack.begin(),
        localProfiler->preSuspendStackStorage_.stack.end());
  } else {
    size_t currCapacity = sampleStorage_.stack.capacity();
    // Ensure there are no allocations in the signal handler by keeping ample
    // reserved space.
    localProfiler->domains_.reserve(
        localProfiler->domains_.size() + currCapacity);
    size_t domainCapacityBefore = localProfiler->domains_.capacity();
    (void)domainCapacityBefore;

    // Ditto for native functions.
    localProfiler->nativeFunctions_.reserve(
        localProfiler->nativeFunctions_.size() + currCapacity);
    size_t nativeFunctionsCapacityBefore =
        localProfiler->nativeFunctions_.capacity();
    (void)nativeFunctionsCapacityBefore;

    SampleResult result = platformSuspendVMAndWalkStack(localProfiler);
    if (result != SampleResult::Success) {
      // Either the registered thread has exited (skip this profiler) or an
      // unrecoverable error occurred (stop the loop). Nothing was walked, so
      // there is no sample to record; propagate the outcome to the caller.
      return result;
    }

    // The stack trace was truncated due to insufficient pre-allocated size in
    // the buffer. Grow the buffer and discard current sample.
    if (numSkippedFrames_ > 0) {
      sampleStorage_.stack.reserve((numSkippedFrames_ + currCapacity) * 3 / 2);
    } else {
      localProfiler->sampledStacks_.emplace_back(
          sampleStorage_.tid,
          sampleStorage_.timeStamp,
          sampleStorage_.stack.begin(),
          sampleStorage_.stack.end());
    }

    assert(
        localProfiler->domains_.capacity() == domainCapacityBefore &&
        "Must not dynamically allocate in signal handler");

    assert(
        localProfiler->nativeFunctions_.capacity() ==
            nativeFunctionsCapacityBefore &&
        "Must not dynamically allocate in signal handler");
  }

  sampleStorage_.stack.clear();
  numSkippedFrames_ = 0;
  return SampleResult::Success;
}

void Sampler::walkRuntimeStack(
    SamplingProfiler *profiler,
    SamplingProfiler::MayAllocate mayAllocate) {
  assert(
      profiler->suspendCount_ == 0 &&
      "Shouldn't interrupt the VM thread when the sampling profiler is "
      "suspended.");

  // Sampling stack will touch GC objects(like closure) so only do so if heap
  // is valid.
  auto &curThreadRuntime = profiler->runtime_;
  assert(
      !curThreadRuntime.getHeap().inGC() &&
      "sampling profiler should be suspended before GC");
  (void)curThreadRuntime;
  numSkippedFrames_ = profiler->walkRuntimeStack(sampleStorage_, mayAllocate);
}

void Sampler::timerLoop(double meanHzFreq) {
  oscompat::set_thread_name("hermes-sampling-profiler");

  std::random_device rd{};
  std::mt19937 gen{rd()};
  // The amount of time that is spent sleeping comes from a normal distribution,
  // to avoid the case where the timer thread samples a stack at a predictable
  // period.
  double interval = 1.0 / meanHzFreq;
  std::normal_distribution<> distribution{interval, interval / 2};
  std::unique_lock<std::mutex> uniqueLock(profilerLock_);

#if defined(_WINDOWS)
  // By default, timer resolution is approximately 64Hz on Windows, so if the
  // meanHzFreq parameter is greater than 64, sampling will occur at a lower
  // frequency than desired. Setting the period to 1 is the minimum useful
  // value, resulting in timer resolution of roughly 1 millsecond.
  timeBeginPeriod(1);
  auto restorePeriod = llvh::make_scope_exit([] { timeEndPeriod(1); });
#endif
  while (enabled_) {
    if (!sampleStacks()) {
      return;
    }

    double dur = std::fabs(distribution(gen));
    enabledCondVar_.wait_for(
        uniqueLock, std::chrono::duration<double>(dur), [this]() {
          return !enabled_;
        });
  }
}

bool Sampler::enabled() {
  std::lock_guard<std::mutex> lockGuard(profilerLock_);
  return enabled_;
}

bool Sampler::enable(double meanHzFreq) {
  std::lock_guard<std::mutex> lockGuard(profilerLock_);
  if (enabled_) {
    return true;
  }
  if (!platformEnable()) {
    return false;
  }
  enabled_ = true;
  // Start timer thread.
  timerThread_ = std::thread(&Sampler::timerLoop, this, meanHzFreq);
  return true;
}

bool Sampler::disable() {
  {
    std::lock_guard<std::mutex> lockGuard(profilerLock_);
    if (!enabled_) {
      // Already disabled.
      return true;
    }
    if (!platformDisable()) {
      return false;
    }
    // Telling timer thread to exit.
    enabled_ = false;
  }
  // Notify the timer thread that it has been disabled.
  enabledCondVar_.notify_all();
  // Wait for timer thread to exit. This avoids the timer thread reading from
  // memory that is freed after a main thread exits. This is outside the lock
  // on profilerLock_ since the timer thread needs to acquire that lock.
  timerThread_.join();
  return true;
}

} // namespace sampling_profiler
} // namespace vm
} // namespace hermes

#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
