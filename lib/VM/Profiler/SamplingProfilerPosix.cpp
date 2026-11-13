/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SamplingProfilerSampler.h"

#if defined(HERMESVM_SAMPLING_PROFILER_POSIX)

#include "hermes/Support/ErrorHandling.h"
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

struct SamplingProfilerPosix;

/// Shared state between a SamplingProfilerPosix and any per-thread guards
/// that reference it. Outlives whichever of the two is destroyed first, so
/// thread-death handlers can safely observe "profiler is gone" without a
/// use-after-free.
struct ProfilerHandle {
  std::mutex mu;
  /// Back-pointer to the owning profiler. Cleared by the profiler's
  /// destructor, at which point thread-death handlers skip this entry.
  SamplingProfilerPosix *profiler{nullptr};
};

struct SamplingProfilerPosix : SamplingProfiler {
  SamplingProfilerPosix(Runtime &rt);
  ~SamplingProfilerPosix() override;

  /// Thread that this profiler instance represents. A value of 0 means the
  /// registered thread has exited or no thread is currently registered.
  /// Must only be accessed while holding the runtimeDataLock_.
  pthread_t currentThread_;

  /// Shared state with per-thread death guards referencing this profiler.
  /// Set once at construction, never reassigned. The underlying object
  /// outlives this profiler if any thread guard still references it.
  std::shared_ptr<ProfilerHandle> handle_;
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

/// Per-thread set of profiler handles registered to this thread. On thread
/// exit, destroyThreadDeathGuard iterates the handles and invalidates each
/// profiler's registered thread identity (if it still matches) before the
/// pthread_t becomes invalid and bionic's pthread_kill would abort.
struct ThreadDeathGuard {
  std::vector<std::shared_ptr<ProfilerHandle>> handles;
};

/// Pthread key holding the thread-local ThreadDeathGuard. Initialized
/// lazily via std::call_once; never deleted (process-lifetime, matching
/// the SamplerPosix singleton).
pthread_key_t g_threadDeathGuardKey;
std::once_flag g_threadDeathGuardKeyInit;

void destroyThreadDeathGuard(void *ptr) {
  std::unique_ptr<ThreadDeathGuard> guard{static_cast<ThreadDeathGuard *>(ptr)};
  // For each handle, take handle->mu first (serializes with profiler
  // destruction), then invoke the Sampler-internal hook which takes
  // runtimeDataLock_. Lock order handle->mu -> runtimeDataLock_ does not
  // overlap with the sampler path (profilerLock_ -> runtimeDataLock_), so
  // there is no deadlock risk.
  for (auto &h : guard->handles) {
    std::lock_guard<std::mutex> lock(h->mu);
    if (h->profiler) {
      Sampler::onRegisteredThreadExit(h->profiler);
    }
  }
}

void ensureThreadDeathGuardKey() {
  std::call_once(g_threadDeathGuardKeyInit, [] {
    int ret =
        pthread_key_create(&g_threadDeathGuardKey, destroyThreadDeathGuard);
    if (ret != 0) {
      hermes_fatal("pthread_key_create failed for sampling profiler");
    }
  });
}

/// Attach \p handle to the calling thread's death guard. Must be called on
/// the thread that will host the profiler.
void attachHandleToCurrentThread(std::shared_ptr<ProfilerHandle> handle) {
  ensureThreadDeathGuardKey();
  auto *guard = static_cast<ThreadDeathGuard *>(
      pthread_getspecific(g_threadDeathGuardKey));
  if (!guard) {
    guard = new ThreadDeathGuard();
    pthread_setspecific(g_threadDeathGuardKey, guard);
  }
  // Before adding the new handle, drop entries this thread's death guard no
  // longer needs to keep around:
  //  - handles whose profiler has been destroyed (its back-pointer was
  //    cleared under handle->mu by ~SamplingProfilerPosix), and
  //  - an existing entry for the same profiler, which happens when a profiler
  //    is moved back onto this thread via setRuntimeThread().
  // Without this, a thread that hosts many profilers over its lifetime -- or
  // a single profiler that repeatedly moves onto it -- would accumulate
  // entries until the thread exits.
  auto &handles = guard->handles;
  ProfilerHandle *incoming = handle.get();
  handles.erase(
      std::remove_if(
          handles.begin(),
          handles.end(),
          [incoming](const std::shared_ptr<ProfilerHandle> &h) {
            if (h.get() == incoming) {
              return true;
            }
            std::lock_guard<std::mutex> lock(h->mu);
            return h->profiler == nullptr;
          }),
      handles.end());
  handles.push_back(std::move(handle));
}
} // namespace

SamplingProfilerPosix::SamplingProfilerPosix(Runtime &rt)
    : SamplingProfiler(rt),
      currentThread_{pthread_self()},
      handle_{std::make_shared<ProfilerHandle>()} {
  // Set the back-pointer before publishing to any other thread. No lock
  // needed: this profiler is not reachable from any other thread yet.
  handle_->profiler = this;
  attachHandleToCurrentThread(handle_);

  // Note that we cannot register this in the base class constructor, because
  // all fields must be initialized before we register with the profiling
  // thread.
  sampling_profiler::Sampler::get()->registerRuntime(this);
}

SamplingProfilerPosix::~SamplingProfilerPosix() {
  // TODO(T125910634): re-introduce the requirement for destroying the sampling
  // profiler on the same thread in which it was created.
  Sampler::get()->unregisterRuntime(this);
  // After unregisterRuntime returns, profilerLock_ has been taken and
  // released, which guarantees no sampler iteration is in-flight for this
  // profiler. Clear the back-pointer so any thread-death handler still
  // holding a shared_ptr to handle_ observes profiler==nullptr and skips.
  std::lock_guard<std::mutex> lock(handle_->mu);
  handle_->profiler = nullptr;
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

void Sampler::platformRegisterRuntime(SamplingProfiler *profiler) {}

void Sampler::platformUnregisterRuntime(SamplingProfiler *profiler) {}

void Sampler::platformPostSampleStack(SamplingProfiler *localProfiler) {}

SampleResult Sampler::platformSuspendVMAndWalkStack(
    SamplingProfiler *profiler) {
  auto *self = static_cast<SamplerPosix *>(this);
  auto *posixProfiler = static_cast<SamplingProfilerPosix *>(profiler);

  // If the registered thread has exited, skip this sample. The caller
  // holds runtimeDataLock_, which serializes with the thread-death handler
  // (see Sampler::onRegisteredThreadExit), so currentThread_ will not be
  // concurrently invalidated between this check and the pthread_kill
  // below. This prevents bionic's pthread_kill from aborting on a recycled
  // pthread_t after a registered JS thread has exited.
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
  auto profiler = static_cast<sampling_profiler::SamplingProfilerPosix *>(this);
  {
    std::lock_guard<std::mutex> lock(profiler->runtimeDataLock_);
    profiler->currentThread_ = pthread_self();
    threadID_ = oscompat::global_thread_id();
    threadNames_[threadID_] = oscompat::thread_name();
  }
  // Register with the new thread's death guard. If an older thread's guard
  // still references handle_, it will find currentThread_ no longer matches
  // itself and skip on exit.
  sampling_profiler::attachHandleToCurrentThread(profiler->handle_);
}

namespace sampling_profiler {
void Sampler::onRegisteredThreadExit(SamplingProfiler *profiler) {
  auto *posix = static_cast<SamplingProfilerPosix *>(profiler);
  std::lock_guard<std::mutex> lock(posix->runtimeDataLock_);
  // Only invalidate if currentThread_ still points at the caller. This
  // correctly handles the setRuntimeThread() case where the profiler has
  // moved to a different thread before the original thread exits. The
  // explicit zero check avoids invoking pthread_equal with an invalid
  // thread ID, which POSIX leaves implementation-defined.
  if (posix->currentThread_ != 0 &&
      pthread_equal(posix->currentThread_, pthread_self())) {
    posix->currentThread_ = 0;
  }
}
} // namespace sampling_profiler

} // namespace vm
} // namespace hermes

#endif // defined(HERMESVM_SAMPLING_PROFILER_POSIX)
