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

#include "llvh/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
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

  // Sample for long enough that the loop iterates many times over both
  // profilers. The dead one is skipped every iteration; the live one must
  // accumulate samples.
  SamplingProfiler::enable();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  SamplingProfiler::disable();

  std::string dump;
  llvh::raw_string_ostream os(dump);
  liveRt->samplingProfiler->dumpSampledStack(os);
  os.flush();

  // dumpSampledStack emits a "Total N samples" header; extract N.
  const std::string kPrefix = "Total ";
  auto pos = dump.find(kPrefix);
  ASSERT_NE(pos, std::string::npos) << dump;
  int sampleCount = std::atoi(dump.c_str() + pos + kPrefix.size());

  // The live profiler must have been sampled repeatedly. If skipping the
  // exited-thread profiler had been treated as a fatal error, the timer loop
  // would have torn down after a single iteration, so the live profiler would
  // have at most one sample. Requiring several samples makes the test
  // independent of the (pointer-ordered) iteration order of the two profilers.
  EXPECT_GE(sampleCount, 2)
      << "live profiler collected " << sampleCount
      << " sample(s); the sampling loop likely stopped after skipping the "
         "exited-thread profiler instead of continuing";
}

} // namespace

#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
