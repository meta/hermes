/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SamplingProfilerSampler.h"

#if defined(HERMESVM_SAMPLING_PROFILER_POSIX)

#include "hermes/Support/Semaphore.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/Profiler/SamplingProfiler.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/StackFrame-inline.h"

#include "llvh/Support/Compiler.h"

#include "TraceSerializer.h"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <csignal>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace hermes {
namespace vm {
namespace sampling_profiler {
namespace {

/// Name of the semaphore.
constexpr char kSamplingDoneSemaphoreName[] = "/samplingDoneSem";

class ThreadDeathGuard;

struct SamplingProfilerPosix : SamplingProfiler {
  SamplingProfilerPosix(Runtime &rt);
  ~SamplingProfilerPosix() override;

  /// Thread that this profiler instance represents. A value of 0 means the
  /// registered thread has exited or no thread is currently registered.
  /// Must only be accessed while holding the runtimeDataLock_.
  pthread_t currentThread_{0};

  /// Current thread membership, protected by Sampler::profilerLock_.
  /// Non-owning: the guard lives until its thread's TLS teardown.
  ThreadDeathGuard *guard_{nullptr};
};

struct SamplerPosix : Sampler {
  SamplerPosix();
  ~SamplerPosix() override;
  /// Pointing to the singleton SamplingProfiler instance.
  /// We need this field because accessing local static variable from
  /// signal handler is unsafe.
  static std::atomic<SamplerPosix *> instance_;

  /// Used to synchronise data writes between the timer thread and the signal
  /// handler in the runtime thread. Also used to send the target
  /// SamplingProfiler to be used during the stack walk.
  static std::atomic<SamplingProfiler *> profilerForSig_;

  /// Whether signal handler is registered or not. Protected by profilerLock_.
  bool isSigHandlerRegistered_{false};

  /// Semaphore to indicate all signal handlers have finished the sampling.
  Semaphore samplingDoneSem_;

  /// Register sampling signal handler if not done yet.
  /// \return true to indicate success.
  bool registerSignalHandlers();

  /// Unregister sampling signal handler.
  bool unregisterSignalHandler();

  /// Signal handler to walk the stack frames.
  static void profilingSignalHandler(int signo);
};

/// Current registrations on this thread. Membership and guard lifetime
/// transitions are synchronized by Sampler::profilerLock_.
class ThreadDeathGuard {
 public:
  /// Invalidate registrations before the owning thread finishes exiting.
  ~ThreadDeathGuard();

  /// Non-owning pointers, removed before the profilers can be destroyed.
  /// Every access requires Sampler::profilerLock_.
  std::vector<SamplingProfilerPosix *> profilers;
};

/// Later TLS or static destructors must not reuse the destroyed guard.
thread_local bool threadDeathGuardDestroyed = false;

ThreadDeathGuard::~ThreadDeathGuard() {
  threadDeathGuardDestroyed = true;
  std::lock_guard<std::mutex> lock(Sampler::get()->profilerLock_);
  for (auto *profiler : profilers) {
    assert(profiler->guard_ == this && "Inconsistent thread membership");
    Sampler::onRegisteredThreadExit(profiler);
  }
}

/// Return this thread's guard, or nullptr after TLS teardown. Caller holds
/// Sampler::profilerLock_.
ThreadDeathGuard *getCurrentThreadGuard() {
  if (threadDeathGuardDestroyed) {
    return nullptr;
  }
  thread_local ThreadDeathGuard guard;
  return &guard;
}

/// Remove the profiler's membership. Caller holds Sampler::profilerLock_.
void removeThreadMembership(SamplingProfilerPosix *profiler) {
  auto *guard = profiler->guard_;
  if (!guard) {
    return;
  }
  auto &profilers = guard->profilers;
  auto it = std::find(profilers.begin(), profilers.end(), profiler);
  assert(it != profilers.end() && "Missing thread membership");
  profilers.erase(it);
  profiler->guard_ = nullptr;
}
} // namespace

SamplingProfilerPosix::SamplingProfilerPosix(Runtime &rt)
    : SamplingProfiler(rt) {
  // Note that we cannot register this in the base class constructor, because
  // all fields must be initialized before we register with the profiling
  // thread.
  sampling_profiler::Sampler::get()->registerRuntime(this);
}

SamplingProfilerPosix::~SamplingProfilerPosix() {
  // TODO(T125910634): re-introduce the requirement for destroying the sampling
  // profiler on the same thread in which it was created.
  Sampler::get()->unregisterRuntime(this);
}

std::atomic<SamplerPosix *> SamplerPosix::instance_{nullptr};

std::atomic<SamplingProfiler *> SamplerPosix::profilerForSig_{nullptr};

namespace {
/// invoke sigaction() posix API to register \p handler.
/// \return what sigaction() returns: 0 to indicate success.
static int invokeSignalAction(void (*handler)(int)) {
  struct sigaction actions;
  memset(&actions, 0, sizeof(actions));
  sigemptyset(&actions.sa_mask);
  // Allows interrupted IO primitives to restart.
  actions.sa_flags = SA_RESTART;
  actions.sa_handler = handler;
  return sigaction(SIGPROF, &actions, nullptr);
}
} // namespace

bool SamplerPosix::registerSignalHandlers() {
  if (isSigHandlerRegistered_) {
    return true;
  }
  if (invokeSignalAction(profilingSignalHandler) != 0) {
    perror("signal handler registration failed");
    return false;
  }
  isSigHandlerRegistered_ = true;
  return true;
}

bool SamplerPosix::unregisterSignalHandler() {
  if (!isSigHandlerRegistered_) {
    return true;
  }
  // Restore to default.
  if (invokeSignalAction(SIG_DFL) != 0) {
    perror("signal handler unregistration failed");
    return false;
  }
  isSigHandlerRegistered_ = false;
  return true;
}

void SamplerPosix::profilingSignalHandler(int signo) {
  // Ensure that writes made on the timer thread before setting the current
  // profiler are correctly acquired.
  SamplingProfiler *localProfiler;
  while (!(localProfiler = profilerForSig_.load(std::memory_order_acquire))) {
  }

  // Avoid spoiling errno in a signal handler by storing the old version and
  // re-assigning it.
  auto oldErrno = errno;

  auto *profilerInstance = static_cast<SamplerPosix *>(instance_.load());
  assert(
      profilerInstance != nullptr &&
      "Why is SamplerPosix::instance_ not initialized yet?");

  profilerInstance->walkRuntimeStack(
      localProfiler, SamplingProfiler::MayAllocate::No);

  // Ensure that writes made in the handler are visible to the timer thread.
  profilerForSig_.store(nullptr);

  if (!profilerInstance->samplingDoneSem_.notifyOne()) {
    errno = oldErrno;
    abort(); // Something is wrong.
  }
  errno = oldErrno;
}

/*static*/ Sampler *Sampler::get() {
  // We intentionally leak this memory to avoid a case where instance is
  // accessed after it is destroyed during shutdown.
  static SamplerPosix *instance = new SamplerPosix{};
  return instance;
}

SamplerPosix::~SamplerPosix() = default;

SamplerPosix::SamplerPosix() {
  instance_.store(this);
}

bool Sampler::platformEnable() {
  auto *self = static_cast<SamplerPosix *>(this);
  if (!self->samplingDoneSem_.open(kSamplingDoneSemaphoreName)) {
    return false;
  }
  if (!self->registerSignalHandlers()) {
    return false;
  }

  return true;
}

bool Sampler::platformDisable() {
  auto *self = static_cast<SamplerPosix *>(this);
  if (!self->samplingDoneSem_.close()) {
    return false;
  }
  // Unregister handlers before shutdown.
  if (!self->unregisterSignalHandler()) {
    return false;
  }

  return true;
}

void Sampler::platformRegisterRuntime(SamplingProfiler *profiler) {
  auto *posix = static_cast<SamplingProfilerPosix *>(profiler);
  std::lock_guard<std::mutex> lock(posix->runtimeDataLock_);
  auto *guard = getCurrentThreadGuard();
  if (posix->guard_ != guard) {
    removeThreadMembership(posix);
    posix->currentThread_ = 0;
    if (guard) {
      guard->profilers.push_back(posix);
      posix->guard_ = guard;
      posix->currentThread_ = pthread_self();
    }
  }
  posix->threadID_ = oscompat::global_thread_id();
  posix->threadNames_[posix->threadID_] = oscompat::thread_name();
}

void Sampler::platformUnregisterRuntime(SamplingProfiler *profiler) {
  auto *posix = static_cast<SamplingProfilerPosix *>(profiler);
  std::lock_guard<std::mutex> lock(posix->runtimeDataLock_);
  removeThreadMembership(posix);
  posix->currentThread_ = 0;
}

void Sampler::platformPostSampleStack(SamplingProfiler *localProfiler) {}

SampleResult Sampler::platformSuspendVMAndWalkStack(
    SamplingProfiler *profiler) {
  auto *self = static_cast<SamplerPosix *>(this);
  auto *posixProfiler = static_cast<SamplingProfilerPosix *>(profiler);

  // If the registered thread has exited, skip this sample. The caller
  // holds profilerLock_ and runtimeDataLock_, so the thread-death callback
  // cannot finish before an in-flight sample completes.
  if (posixProfiler->currentThread_ == 0) {
    return SampleResult::ThreadExited;
  }

  // Guarantee that the runtime thread will not proceed until it has
  // acquired the updates to domains_.
  self->profilerForSig_.store(profiler, std::memory_order_release);

  // Signal target runtime thread to sample stack. The runtimeDataLock is
  // held by the caller, ensuring the runtime won't start to be used on
  // another thread before sampling begins.
  int result = pthread_kill(posixProfiler->currentThread_, SIGPROF);
  if (result != 0) {
    // On non-Android POSIX, pthread_kill may return ESRCH if the target
    // terminated in the narrow window where its pthread_t is still in the
    // thread list but the thread has exited. Treat it as a thread-exit skip
    // rather than a fatal error so the timer loop keeps sampling the other
    // registered profilers.
    self->profilerForSig_.store(nullptr, std::memory_order_release);
    return SampleResult::ThreadExited;
  }

  // Threading: samplingDoneSem_ will synchronise this thread with the
  // signal handler, so that we only have one active signal at a time.
  if (!self->samplingDoneSem_.wait()) {
    return SampleResult::Failed;
  }

  // Guarantee that this thread will observe all changes made to data
  // structures in the signal handler.
  while (self->profilerForSig_.load(std::memory_order_acquire) != nullptr) {
  }

  return SampleResult::Success;
}

} // namespace sampling_profiler

std::unique_ptr<SamplingProfiler> SamplingProfiler::create(Runtime &rt) {
  return std::make_unique<sampling_profiler::SamplingProfilerPosix>(rt);
}

bool SamplingProfiler::belongsToCurrentThread() {
  auto profiler = static_cast<sampling_profiler::SamplingProfilerPosix *>(this);
  std::lock_guard<std::mutex> lock(profiler->runtimeDataLock_);
  return profiler->currentThread_ == pthread_self();
}

void SamplingProfiler::setRuntimeThread() {
  sampling_profiler::Sampler::get()->registerRuntime(this);
}

namespace sampling_profiler {
void Sampler::onRegisteredThreadExit(SamplingProfiler *profiler) {
  auto *posix = static_cast<SamplingProfilerPosix *>(profiler);
  std::lock_guard<std::mutex> lock(posix->runtimeDataLock_);
  posix->guard_ = nullptr;
  posix->currentThread_ = 0;
}
} // namespace sampling_profiler

} // namespace vm
} // namespace hermes

#endif // defined(HERMESVM_SAMPLING_PROFILER_POSIX)
