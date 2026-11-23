/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_GCCONCURRENCY_H
#define HERMES_VM_GCCONCURRENCY_H

#include "hermes/VM/sh_config.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace hermes {
namespace vm {

/// If true, the Hades will run with a concurrent background thread. If false,
/// Hades will run with a single thread that interleaves work with the YG and
/// OG. Has no effect on non-Hades GCs.
static constexpr bool kConcurrentGC =
#if defined(HERMESVM_ALLOW_CONCURRENT_GC) && \
    (HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES)
    // Only use Hades concurrently if on a 64-bit platform.
    sizeof(void *) == 8
#else
    false
#endif
    ;

namespace impl {

/// FakeAtomic has the same API as std::atomic, but ignores the memory order
/// argument and always accesses data non-atomically.
/// Used when the GC doesn't require atomicity.
/// In the JS VM, there is currently only one mutator thread and at most one GC
/// thread. The GC thread will not do any modifications to these atomics, and
/// will only read them. Therefore it is typically safe for the mutator to use
/// relaxed reads. Writes will typically require std::memory_order_release or
/// stricter to make sure the GC sees the writes which occur before the atomic
/// write.
/// NOTE: This differs from std::atomic where it doesn't have default memory
/// orders, since we want all atomic operations to be very explicit with their
/// requirements. Also don't define operator T for the same reason.
template <typename T>
class FakeAtomic final {
 public:
  constexpr FakeAtomic() : data_{} {}
  constexpr FakeAtomic(T desired) : data_{desired} {}

  T load(std::memory_order order) const {
    (void)order;
    return data_;
  }

  void store(T desired, std::memory_order order) {
    (void)order;
    data_ = desired;
  }

  T fetch_add(T arg, std::memory_order order) {
    (void)order;
    const T oldData = data_;
    data_ += arg;
    return oldData;
  }

  T exchange(T arg, std::memory_order order) {
    (void)order;
    const T oldData = data_;
    data_ = arg;
    return oldData;
  }

  T fetch_sub(T arg, std::memory_order order) {
    (void)order;
    const T oldData = data_;
    data_ -= arg;
    return oldData;
  }

  /// Use store explicitly instead.
  FakeAtomic &operator=(const FakeAtomic &) = delete;

 private:
  T data_;
};

/// A DebugMutex wraps a std::recursive_mutex and also tracks which thread
/// currently has the mutex locked. Only available in debug modes.
class DebugMutex {
 public:
  DebugMutex() : tid_() {}
  ~DebugMutex() = default;

  operator bool() const {
    // Check that this thread owns the mutex.
    // The mutex must be held in order to check this condition safely.
    return tid_.load(std::memory_order_relaxed) == std::this_thread::get_id();
  }

  void lock() {
    inner_.lock();
    depth_++;
    tid_.store(std::this_thread::get_id(), std::memory_order_relaxed);
  }

  /// Try to lock the mutex without blocking.
  /// \return true if the mutex was locked by this call.
  bool try_lock() {
    if (!inner_.try_lock())
      return false;
    depth_++;
    tid_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    return true;
  }

  void unlock() {
    assert(depth_ && "Should not unlock an unlocked mutex");
    assert(
        tid_.load(std::memory_order_relaxed) == std::this_thread::get_id() &&
        "Mutex should be acquired and unlocked on the same thread");
    depth_--;
    if (!depth_) {
      tid_.store(std::thread::id{}, std::memory_order_relaxed);
    }
    inner_.unlock();
  }

  uint32_t depth() const {
    assert(*this && "Must hold the inner mutex to call depth");
    return depth_;
  }

 private:
  std::recursive_mutex inner_;
  // Sometimes we want to assert that the the current thread does not hold the
  // mutex. Since the mutex is not held, TSAN complains that the access to tid_
  // is not thread safe. It is safe to use this atomic with any memory ordering
  // because all we care about is the last value assigned to tid_ by the
  // *current* thread.
  std::atomic<std::thread::id> tid_;
  uint32_t depth_{0};
};

/// A FakeMutex has the same API as a std::mutex but does nothing.
/// It pretends to always be locked for convenience of asserts that need to work
/// in both concurrent code and non-concurrent code.
class FakeMutex {
 public:
  explicit FakeMutex() = default;

  operator bool() const {
    return true;
  }

  uint32_t depth() const {
    return 1;
  }

  void lock() {}
  bool try_lock() {
    return true;
  }
  void unlock() {}
};

} // namespace impl

// Only these typedefs should be used by the rest of the VM.
template <typename T>
using AtomicIfConcurrentGC = typename std::
    conditional<kConcurrentGC, std::atomic<T>, impl::FakeAtomic<T>>::type;

using Mutex = std::conditional<
    kConcurrentGC,
#ifndef NDEBUG
    impl::DebugMutex
#else
    std::recursive_mutex
#endif
    ,
    impl::FakeMutex>::type;

} // namespace vm
} // namespace hermes

#endif
