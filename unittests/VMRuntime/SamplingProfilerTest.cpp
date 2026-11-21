/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/Profiler/SamplingProfiler.h"

#if HERMESVM_SAMPLING_PROFILER_AVAILABLE

#include "TestHelpers1.h"
#include "hermes/VM/Runtime.h"

#include "llvh/ADT/ScopeExit.h"
#include "llvh/Support/Compiler.h"
#include "llvh/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>

namespace {
using namespace hermes::vm;

static constexpr bool withSamplingProfilerEnabled = true;
static constexpr bool withSamplingProfilerDisabled = false;

std::shared_ptr<Runtime> makeRuntime(bool withEnableSampleProfiling) {
  auto cfg = RuntimeConfig::Builder()
                 .withEnableSampleProfiling(withEnableSampleProfiling)
                 .build();
  return Runtime::create(cfg);
}

/// Return the number of recorded samples without clearing them.
int getSampleCount(SamplingProfiler &profiler) {
  std::string dump;
  llvh::raw_string_ostream os(dump);
  profiler.dumpSampledStack(os);
  os.flush();

  const std::string kPrefix = "Total ";
  auto pos = dump.find(kPrefix);
  EXPECT_NE(pos, std::string::npos) << dump;
  if (pos == std::string::npos) {
    return -1;
  }
  return std::atoi(dump.c_str() + pos + kPrefix.size());
}

TEST(SamplingProfilerTest, Invariants) {
  // No sample profiler registration by default
  EXPECT_TRUE(Runtime::create(RuntimeConfig{})->samplingProfiler == nullptr);

  EXPECT_TRUE(
      makeRuntime(withSamplingProfilerDisabled)->samplingProfiler == nullptr);

  auto rt = makeRuntime(withSamplingProfilerEnabled);
  ASSERT_FALSE(rt->samplingProfiler == nullptr);

  // The sample profiler belongs to this thread.
  EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
}

#ifndef __APPLE__
TEST(SamplingProfilerTest, MultipleRuntimes) {
  auto rt0 = makeRuntime(withSamplingProfilerEnabled);
  auto rt1 = makeRuntime(withSamplingProfilerEnabled);
  auto rt2 = makeRuntime(withSamplingProfilerEnabled);

  EXPECT_TRUE(rt0->samplingProfiler->belongsToCurrentThread());
  EXPECT_TRUE(rt1->samplingProfiler->belongsToCurrentThread());
  EXPECT_TRUE(rt2->samplingProfiler->belongsToCurrentThread());
}

TEST(SamplingProfilerTest, MultipleProfilers) {
  auto rt = makeRuntime(withSamplingProfilerEnabled);
  auto sp0 = SamplingProfiler::create(*rt);
  auto sp1 = SamplingProfiler::create(*rt);
  auto sp2 = SamplingProfiler::create(*rt);
  EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
  EXPECT_TRUE(sp0->belongsToCurrentThread());
  EXPECT_TRUE(sp1->belongsToCurrentThread());
  EXPECT_TRUE(sp2->belongsToCurrentThread());
}
#endif

TEST(SamplingProfilerTest, RegisterDifferentThread) {
  constexpr uint32_t kThreadCount = 3;

  auto rt = makeRuntime(withSamplingProfilerEnabled);

  for (uint32_t threadNumber = 0; threadNumber < kThreadCount; ++threadNumber) {
    std::thread([&]() {
      rt->samplingProfiler->setRuntimeThread();
      EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
    }).join();
  }
}

TEST(SamplingProfilerTest, RegisterIdenticalThread) {
  auto rt = makeRuntime(withSamplingProfilerEnabled);

  rt->samplingProfiler->setRuntimeThread();
  EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
  rt->samplingProfiler->setRuntimeThread();
  EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
}

#if defined(HERMESVM_SAMPLING_PROFILER_POSIX)
// Skipped under TSan, where it deadlocks. TSan defers an asynchronous signal
// until the target thread passes through a TSan interceptor, and libstdc++'s
// std::future::wait() blocks in a raw futex syscall that TSan does not
// intercept. So while the main thread waits on registeredFuture and still owns
// rt, the timer thread can send it SIGPROF and wait for the handler, holding
// profilerLock_; the worker then blocks on profilerLock_ in setRuntimeThread()
// before it can fulfil the future.
#if !LLVM_THREAD_SANITIZER_BUILD
TEST(
    SamplingProfilerTest,
    MigrationBeforePreviousThreadExitPreservesOwnership) {
  auto rt = makeRuntime(withSamplingProfilerEnabled);
  auto siblingRt = makeRuntime(withSamplingProfilerEnabled);

  // Sample during migration, so setRuntimeThread() and thread exit contend
  // with the timer loop for profilerLock_.
  ASSERT_TRUE(SamplingProfiler::enable(1000));
  for (unsigned i = 0; i < 8; ++i) {
    std::promise<void> registered;
    auto registeredFuture = registered.get_future();
    std::promise<void> migratedBack;
    auto migratedBackFuture = migratedBack.get_future();
    std::thread worker([&] {
      siblingRt->samplingProfiler->setRuntimeThread();
      for (unsigned j = 0; j < 3; ++j) {
        rt->samplingProfiler->setRuntimeThread();
        EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
        EXPECT_TRUE(siblingRt->samplingProfiler->belongsToCurrentThread());
      }
      registered.set_value();

      migratedBackFuture.wait();
      EXPECT_FALSE(rt->samplingProfiler->belongsToCurrentThread());
      EXPECT_TRUE(siblingRt->samplingProfiler->belongsToCurrentThread());
    });

    registeredFuture.wait();
    for (unsigned j = 0; j < 3; ++j) {
      rt->samplingProfiler->setRuntimeThread();
      EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
    }
    migratedBack.set_value();
    worker.join();

    EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
    EXPECT_FALSE(siblingRt->samplingProfiler->belongsToCurrentThread());
  }
  EXPECT_TRUE(SamplingProfiler::disable());
}
#endif // !LLVM_THREAD_SANITIZER_BUILD

TEST(SamplingProfilerTest, RemoteDestructionAllowsThreadGuardReuse) {
  auto rt = makeRuntime(withSamplingProfilerDisabled);
  std::unique_ptr<SamplingProfiler> profiler;
  std::promise<void> registered;
  auto registeredFuture = registered.get_future();
  std::promise<void> destroyed;
  auto destroyedFuture = destroyed.get_future();
  std::thread worker([&] {
    profiler = SamplingProfiler::create(*rt);
    EXPECT_TRUE(profiler->belongsToCurrentThread());
    registered.set_value();

    destroyedFuture.wait();
    profiler = SamplingProfiler::create(*rt);
    EXPECT_TRUE(profiler->belongsToCurrentThread());
    profiler->setRuntimeThread();
    EXPECT_TRUE(profiler->belongsToCurrentThread());
  });

  registeredFuture.wait();
  profiler.reset();
  destroyed.set_value();
  worker.join();

  ASSERT_NE(profiler, nullptr);
  EXPECT_FALSE(profiler->belongsToCurrentThread());
  profiler->setRuntimeThread();
  EXPECT_TRUE(profiler->belongsToCurrentThread());
}

TEST(SamplingProfilerTest, ProfilerDestructionCanRaceWithThreadExit) {
  auto rt = makeRuntime(withSamplingProfilerDisabled);

  // Sample during the race, so thread exit contends with the timer loop for
  // profilerLock_ while the timer loop may be signalling the exiting thread.
  ASSERT_TRUE(SamplingProfiler::enable(1000));
  for (unsigned i = 0; i < 32; ++i) {
    std::unique_ptr<SamplingProfiler> profiler;
    std::promise<void> registered;
    auto registeredFuture = registered.get_future();
    std::promise<void> exit;
    auto exitFuture = exit.get_future();
    std::thread worker([&] {
      profiler = SamplingProfiler::create(*rt);
      EXPECT_TRUE(profiler->belongsToCurrentThread());
      registered.set_value();
      exitFuture.wait();
    });

    registeredFuture.wait();
    exit.set_value();
    profiler.reset();
    worker.join();
  }
  EXPECT_TRUE(SamplingProfiler::disable());
}

TEST(SamplingProfilerTest, RegistrationDuringThreadTeardownIsRejected) {
  auto rt = makeRuntime(withSamplingProfilerEnabled);
  auto workerRt = makeRuntime(withSamplingProfilerEnabled);
  std::unique_ptr<SamplingProfiler> lateProfiler;
  bool lateCallbackRan = false;

  std::thread([&] {
    // Constructed before the worker's guard, this callback runs after it.
    thread_local auto lateRegistration = llvh::make_scope_exit(
        [runtime = rt.get(),
         profiler = rt->samplingProfiler.get(),
         workerProfiler = workerRt->samplingProfiler.get(),
         &lateProfiler,
         &lateCallbackRan] {
          EXPECT_FALSE(workerProfiler->belongsToCurrentThread());

          profiler->setRuntimeThread();
          EXPECT_FALSE(profiler->belongsToCurrentThread());
          workerProfiler->setRuntimeThread();
          EXPECT_FALSE(workerProfiler->belongsToCurrentThread());

          lateProfiler = SamplingProfiler::create(*runtime);
          EXPECT_FALSE(lateProfiler->belongsToCurrentThread());
          lateCallbackRan = true;
        });
    (void)lateRegistration;
    workerRt->samplingProfiler->setRuntimeThread();
    EXPECT_TRUE(workerRt->samplingProfiler->belongsToCurrentThread());
  }).join();

  EXPECT_TRUE(lateCallbackRan);
  ASSERT_NE(lateProfiler, nullptr);
  // Failed migration must also clear the previous, still-live thread identity.
  EXPECT_FALSE(rt->samplingProfiler->belongsToCurrentThread());
  EXPECT_FALSE(workerRt->samplingProfiler->belongsToCurrentThread());
  EXPECT_FALSE(lateProfiler->belongsToCurrentThread());
}
#endif

// Regression test for https://github.com/facebook/hermes/issues/1853 and
// https://github.com/getsentry/sentry-react-native/issues/5441: if a thread
// that registered a profiler exits while the profiler instance lives on, the
// sampler must not call pthread_kill on the dead thread. On Android bionic
// this would abort the process; on other POSIX platforms it would be an
// ESRCH. With the thread-death guard, the sampler skips the profiler.
TEST(SamplingProfilerTest, SamplingAfterRegisteredThreadExitDoesNotCrash) {
  auto rt = makeRuntime(withSamplingProfilerEnabled);

  std::thread worker([&]() {
    rt->samplingProfiler->setRuntimeThread();
    EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
  });
  // std::thread::join() waits for the OS thread to fully terminate. Per
  // POSIX, pthread_join does not return until after the thread's C++
  // thread_local destructors and all pthread_key_create destructors have
  // run. That means our ThreadDeathGuard destructor -- and therefore the
  // call to Sampler::onRegisteredThreadExit that invalidates the
  // registered thread -- has completed by the time join() returns. Do not
  // relax this ordering assumption when editing the test.
  worker.join();

  // After the registered thread has exited, the profiler must no longer
  // report ownership by any live thread.
  EXPECT_FALSE(rt->samplingProfiler->belongsToCurrentThread());

  SamplingProfiler::enable();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  SamplingProfiler::disable();

#if defined(HERMESVM_SAMPLING_PROFILER_POSIX)
  EXPECT_EQ(getSampleCount(*rt->samplingProfiler), 0);
#endif

  // Re-register on the main thread so subsequent use works normally.
  rt->samplingProfiler->setRuntimeThread();
  EXPECT_TRUE(rt->samplingProfiler->belongsToCurrentThread());
}

// A profiler whose registered thread has exited must be skipped without
// stopping the sampling loop: the other, still-live profilers must keep being
// sampled. This exercises the tri-state SampleResult path where a per-profiler
// skip (SampleResult::ThreadExited) is distinguished from a fatal error
// (SampleResult::Failed) that would tear down the whole timer loop. See the
// discussion on https://github.com/facebook/hermes/pull/1995.
TEST(SamplingProfilerTest, SamplingSkipsExitedThreadButSamplesOthers) {
  // This runtime stays registered on the main (this) thread for the whole
  // test, so it remains sampleable.
  auto liveRt = makeRuntime(withSamplingProfilerEnabled);
  ASSERT_FALSE(liveRt->samplingProfiler == nullptr);

  // This runtime is registered on a worker thread that then exits, leaving a
  // profiler whose registered thread is gone.
  auto deadRt = makeRuntime(withSamplingProfilerEnabled);
  ASSERT_FALSE(deadRt->samplingProfiler == nullptr);

  std::thread worker([&]() {
    deadRt->samplingProfiler->setRuntimeThread();
    EXPECT_TRUE(deadRt->samplingProfiler->belongsToCurrentThread());
  });
  // As documented in SamplingAfterRegisteredThreadExitDoesNotCrash, join()
  // does not return until the worker's ThreadDeathGuard destructor -- and thus
  // Sampler::onRegisteredThreadExit -- has run, so deadRt's registered thread
  // is invalidated once join() returns.
  worker.join();

  // Preconditions: the dead profiler no longer belongs to any live thread,
  // while the live profiler still belongs to this thread.
  EXPECT_FALSE(deadRt->samplingProfiler->belongsToCurrentThread());
  EXPECT_TRUE(liveRt->samplingProfiler->belongsToCurrentThread());

  ASSERT_TRUE(SamplingProfiler::enable());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (getSampleCount(*liveRt->samplingProfiler) < 2 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(SamplingProfiler::disable());

  // The live profiler must have been sampled repeatedly. If skipping the
  // exited-thread profiler had been treated as a fatal error, the timer loop
  // would have torn down after a single iteration, so the live profiler would
  // have at most one sample. Requiring several samples makes the test
  // independent of the iteration order of the two profilers.
  int sampleCount = getSampleCount(*liveRt->samplingProfiler);
  EXPECT_GE(sampleCount, 2)
      << "live profiler collected " << sampleCount
      << " sample(s); the sampling loop likely stopped after skipping the "
         "exited-thread profiler instead of continuing";

#if defined(HERMESVM_SAMPLING_PROFILER_POSIX)
  // A joined worker's pthread_t can be reused by the sampler thread.
  EXPECT_EQ(getSampleCount(*deadRt->samplingProfiler), 0);
#endif
}

} // namespace

#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
