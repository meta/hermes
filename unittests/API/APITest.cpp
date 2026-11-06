/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <hermes/BCGen/HBC/BytecodeFileFormat.h>
#include <hermes/CompileJS.h>
#include <hermes/Public/JSOutOfMemoryError.h>
#include <hermes/VM/TimeLimitMonitor.h>
#include <hermes/VM/VMExperiments.h>
#include <hermes/VM/sh_config.h>
#include <hermes/hermes.h>
#include <hermes_sandbox/HermesSandboxRuntime.h>
#include <jsi/decorator.h>
#include <jsi/instrumentation.h>
#include <jsi/test/testlib.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <tuple>

using namespace facebook::jsi;
using namespace facebook::hermes;

struct HermesTestHelper {
  static size_t rootsListLength(const IHermesTestHelpers &rt) {
    return rt.rootsListLengthForTests();
  }

  static int64_t calculateRootsListChange(
      const IHermesTestHelpers &rt,
      std::function<void(void)> f) {
    auto before = rootsListLength(rt);
    f();
    return rootsListLength(rt) - before;
  }
};

namespace {

class HermesRuntimeTestBase {
 public:
  HermesRuntimeTestBase(std::shared_ptr<Runtime> rt) : rt(std::move(rt)) {}

 protected:
  Value eval(const char *code) {
    return rt->global().getPropertyAsFunction(*rt, "eval").call(*rt, code);
  }

  std::shared_ptr<Runtime> rt;
};

/// TODO: Run these tests against all jsi::Runtimes implemented on top of
///       Hermes, similar to HermesRuntimeTest below.
class HermesRuntimeCustomConfigTest : public ::testing::Test,
                                      public HermesRuntimeTestBase {
 public:
  HermesRuntimeCustomConfigTest(::hermes::vm::RuntimeConfig runtimeConfig)
      : HermesRuntimeTestBase(makeHermesRuntime(runtimeConfig)) {}
};

TEST(HermesRuntimeConfigTest, FinalizerThreadRunner) {
  struct RunnerState {
    std::atomic<bool> active{false};
    std::atomic<std::thread::id> thread{std::thread::id{}};
    std::atomic<bool> finalizedUnderRunner{false};
    std::atomic<bool> finalizedOnRunnerThread{false};
  } state;

  class TestHostObject final : public HostObject {
   public:
    explicit TestHostObject(RunnerState &state) : state_(state) {}

    ~TestHostObject() override {
      state_.finalizedUnderRunner = state_.active.load();
      state_.finalizedOnRunnerThread =
          state_.thread.load() == std::this_thread::get_id();
    }

   private:
    RunnerState &state_;
  };

  auto runtime = makeHermesRuntime(
      ::hermes::vm::RuntimeConfig::Builder()
          .withFinalizerThreadRunner([&state](std::function<void()> run) {
            state.thread = std::this_thread::get_id();
            state.active = true;
            run();
            state.active = false;
          })
          .build());
  runtime->global().setProperty(
      *runtime,
      "hostObject",
      Object::createFromHostObject(
          *runtime, std::make_shared<TestHostObject>(state)));

  runtime.reset();

  EXPECT_TRUE(state.finalizedUnderRunner);
  // Without this the test would also pass if the finalizer ran on the thread
  // destroying the runtime while the worker happened to be alive.
  EXPECT_TRUE(state.finalizedOnRunnerThread);
  EXPECT_FALSE(state.active);
}

TEST(HermesRuntimeConfigTest, EmptyFinalizerThreadRunner) {
  // An empty runner is not the same as an unset one: it suppresses the
  // platform default instead of selecting it. Nothing platform independent
  // observes the difference, so this only checks that the runtime still
  // finalizes; the Android behavior it controls is covered by
  // HostObjectJniTest.
  std::atomic<bool> finalized{false};

  class TestHostObject final : public HostObject {
   public:
    explicit TestHostObject(std::atomic<bool> &finalized)
        : finalized_(finalized) {}

    ~TestHostObject() override {
      finalized_ = true;
    }

   private:
    std::atomic<bool> &finalized_;
  };

  auto runtime = makeHermesRuntime(
      ::hermes::vm::RuntimeConfig::Builder()
          .withFinalizerThreadRunner(::hermes::vm::ThreadRunner{})
          .build());
  runtime->global().setProperty(
      *runtime,
      "hostObject",
      Object::createFromHostObject(
          *runtime, std::make_shared<TestHostObject>(finalized)));

  runtime.reset();

  EXPECT_TRUE(finalized);
}

#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
/// Event-loop control whose queued work runs only when requested by a test.
class IdleTimeGCEventLoopControl final : public IEventLoopControl {
 public:
  /// Copy the callback unless this controller is configured to drop tasks.
  void scheduleTask(const std::function<void()> &task) override;

  /// These tests do not create external task sources.
  uint64_t registerTaskQueueSource() override {
    return 0;
  }

  /// These tests do not create external task sources.
  void unregisterTaskQueueSource(uint64_t) override {}

  /// Configure whether subsequent scheduling attempts retain their callbacks.
  void dropTasks(bool drop);

  /// Return the number of scheduling attempts, including dropped tasks.
  size_t scheduledTaskCount() const;

  /// Return the number of retained callbacks.
  size_t queuedTaskCount() const;

  /// Execute one queued callback while the runtime is alive and exclusively
  /// owned by the calling thread.
  void runNext();

 private:
  mutable std::mutex mutex_;
  std::queue<std::function<void()>> tasks_;
  size_t scheduledTaskCount_{0};
  bool dropTasks_{false};
};

void IdleTimeGCEventLoopControl::scheduleTask(
    const std::function<void()> &task) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++scheduledTaskCount_;
  if (!dropTasks_) {
    tasks_.push(task);
  }
}

void IdleTimeGCEventLoopControl::dropTasks(bool drop) {
  std::lock_guard<std::mutex> lock(mutex_);
  dropTasks_ = drop;
}

size_t IdleTimeGCEventLoopControl::scheduledTaskCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return scheduledTaskCount_;
}

size_t IdleTimeGCEventLoopControl::queuedTaskCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

void IdleTimeGCEventLoopControl::runNext() {
  std::function<void()> task;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ASSERT_FALSE(tasks_.empty());
    task = std::move(tasks_.front());
    tasks_.pop();
  }
  task();
}

/// Exercise idle GC through public JSI calls and collection statistics.
class IdleTimeGCTest : public ::testing::Test {
 protected:
  /// The experiment's scope-entry budget between scheduling attempts.
  static constexpr size_t kEntryBudget = 10000;

  /// Create a runtime with independently configurable experiment and scheduler.
  void createRuntime(bool enabled = true, bool attachEventLoop = true);

  /// Enter execution scopes without allocating JavaScript objects.
  void enterScopes(size_t count);

  /// Advance until one task is submitted without assuming a bootstrap count.
  void enterUntilTaskScheduled();

  /// Allocate through JavaScript until a natural young collection occurs.
  void collectNaturally();

  /// Read the collection count without entering an execution scope.
  int64_t youngCollections();

  // The controller must outlive the runtime, including its finalizers.
  IdleTimeGCEventLoopControl eventLoop_;
  std::unique_ptr<HermesRuntime> runtime_;
};

void IdleTimeGCTest::createRuntime(bool enabled, bool attachEventLoop) {
  runtime_ = makeHermesRuntime(
      ::hermes::vm::RuntimeConfig::Builder()
          .withVMExperimentFlags(
              enabled ? ::hermes::vm::experiments::IdleTimeGC : 0)
          .withGCConfig(
              ::hermes::vm::GCConfig::Builder()
                  .withInitHeapSize(8 << 20)
                  .withMaxHeapSize(32 << 20)
                  .withShouldRecordStats(false)
                  .build())
          .build());
  if (attachEventLoop) {
    auto *control = castInterface<ISetEventLoopControl>(runtime_.get());
    ASSERT_NE(control, nullptr);
    control->setEventLoopControl(&eventLoop_);
  }
}

void IdleTimeGCTest::enterScopes(size_t count) {
  for (size_t i = 0; i < count; ++i) {
    runtime_->global();
  }
}

void IdleTimeGCTest::enterUntilTaskScheduled() {
  const auto before = eventLoop_.scheduledTaskCount();
  for (size_t i = 0;
       i <= kEntryBudget && eventLoop_.scheduledTaskCount() == before;
       ++i) {
    enterScopes(1);
  }
  ASSERT_EQ(eventLoop_.scheduledTaskCount(), before + 1);
}

void IdleTimeGCTest::collectNaturally() {
  const auto before = youngCollections();
  auto script = std::make_shared<StringBuffer>(R"(
    for (let i = 0; i < 1024; ++i) {
      globalThis.idleTimeGCGarbage = new Array(128).fill(i);
    }
  )");
  for (size_t i = 0; i < 64 && youngCollections() == before; ++i) {
    runtime_->evaluateJavaScript(script, "idle-time-gc.js");
  }
  ASSERT_GT(youngCollections(), before);
}

int64_t IdleTimeGCTest::youngCollections() {
  return runtime_->instrumentation().getHeapInfo(false).at(
      "hermes_yg_numCollections");
}

TEST_F(IdleTimeGCTest, Disabled) {
  createRuntime(false);
  const auto before = youngCollections();
  enterScopes(3 * kEntryBudget);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), 0);
  EXPECT_EQ(youngCollections(), before);
}

TEST_F(IdleTimeGCTest, NoEventLoopControl) {
  createRuntime(true, false);
  const auto before = youngCollections();
  enterScopes(3 * kEntryBudget);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), 0);
  EXPECT_EQ(youngCollections(), before);
}

TEST_F(IdleTimeGCTest, DeferredCollection) {
  createRuntime();
  const auto before = youngCollections();
  enterUntilTaskScheduled();
  ASSERT_EQ(eventLoop_.queuedTaskCount(), 1);
  EXPECT_EQ(youngCollections(), before);

  eventLoop_.runNext();
  EXPECT_EQ(youngCollections(), before + 1);
  const auto scheduled = eventLoop_.scheduledTaskCount();
  enterScopes(kEntryBudget - 1);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), scheduled);
  enterScopes(1);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), scheduled + 1);
}

TEST_F(IdleTimeGCTest, NaturalCollectionPostponesTask) {
  createRuntime();
  enterUntilTaskScheduled();
  eventLoop_.runNext();
  const auto scheduled = eventLoop_.scheduledTaskCount();
  enterScopes(kEntryBudget / 2);
  collectNaturally();
  const auto afterNatural = youngCollections();

  enterScopes(kEntryBudget / 2);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), scheduled);
  EXPECT_EQ(eventLoop_.queuedTaskCount(), 0);
  enterUntilTaskScheduled();
  eventLoop_.runNext();
  EXPECT_EQ(youngCollections(), afterNatural + 1);
}

TEST_F(IdleTimeGCTest, NaturalCollectionSkipsQueuedTask) {
  createRuntime();
  enterUntilTaskScheduled();
  collectNaturally();
  const auto afterNatural = youngCollections();
  ASSERT_EQ(eventLoop_.queuedTaskCount(), 1);
  eventLoop_.runNext();
  EXPECT_EQ(youngCollections(), afterNatural);

  const auto scheduled = eventLoop_.scheduledTaskCount();
  enterScopes(kEntryBudget - 1);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), scheduled);
  enterUntilTaskScheduled();
  eventLoop_.runNext();
  EXPECT_EQ(youngCollections(), afterNatural + 1);
}

TEST_F(IdleTimeGCTest, DroppedTasksRemainRateLimited) {
  eventLoop_.dropTasks(true);
  createRuntime();
  const auto before = youngCollections();
  enterUntilTaskScheduled();
  const auto scheduled = eventLoop_.scheduledTaskCount();
  ASSERT_EQ(eventLoop_.queuedTaskCount(), 0);

  enterScopes(kEntryBudget - 1);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), scheduled);
  enterScopes(1);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), scheduled + 1);
  EXPECT_EQ(eventLoop_.queuedTaskCount(), 0);
  EXPECT_EQ(youngCollections(), before);

  eventLoop_.dropTasks(false);
  enterScopes(kEntryBudget - 1);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), scheduled + 1);
  enterScopes(1);
  EXPECT_EQ(eventLoop_.scheduledTaskCount(), scheduled + 2);
  ASSERT_EQ(eventLoop_.queuedTaskCount(), 1);
  eventLoop_.runNext();
  EXPECT_EQ(youngCollections(), before + 1);
}

TEST_F(IdleTimeGCTest, QueuedTasksCollectOnlyOnce) {
  createRuntime();
  const auto before = youngCollections();
  enterUntilTaskScheduled();
  enterScopes(2 * kEntryBudget);
  ASSERT_EQ(eventLoop_.queuedTaskCount(), 3);
  EXPECT_EQ(youngCollections(), before);

  eventLoop_.runNext();
  EXPECT_EQ(youngCollections(), before + 1);
  eventLoop_.runNext();
  eventLoop_.runNext();
  EXPECT_EQ(youngCollections(), before + 1);
  EXPECT_EQ(eventLoop_.queuedTaskCount(), 0);
}

TEST_F(IdleTimeGCTest, QueuedTaskDoesNotRetainRuntime) {
  createRuntime();
  UUID key{0xe67ab3d6, 0x09a0, 0x11f0, 0xa641, 0x325096b39f47};
  auto data = std::make_shared<int>(0);
  std::weak_ptr<int> dataLifetime = data;
  runtime_->setRuntimeData(key, data);
  data.reset();

  enterUntilTaskScheduled();
  ASSERT_EQ(eventLoop_.queuedTaskCount(), 1);
  ASSERT_FALSE(dataLifetime.expired());

  runtime_.reset();
  ASSERT_TRUE(dataLifetime.expired());
  ASSERT_EQ(eventLoop_.queuedTaskCount(), 1);
  // The controller must discard queued callbacks without running them now.
}
#endif

class HermesRuntimeTest : public ::testing::TestWithParam<RuntimeFactory>,
                          public HermesRuntimeTestBase {
 public:
  HermesRuntimeTest() : HermesRuntimeTestBase(GetParam()()) {}
};

/// Like HermesRuntimeTest, but for tests that run bytecode from
/// hermes::compileJS, so it leaves out HermesSandboxRuntime. The sandbox's VM
/// comes from the revision the sandbox was last generated at, and may misread
/// bytecode from the compiler linked into this test even when the bytecode
/// version matches.
class HermesRuntimeBytecodeTest : public HermesRuntimeTest {};

// In JSC there's a bug where host functions are always ran with a this in
// nonstrict mode so this must be a hermes only test. See
// https://es5.github.io/#x10.4.3 for more info.
TEST_P(HermesRuntimeTest, StrictHostFunctionBindTest) {
  Function coolify = Function::createFromHostFunction(
      *rt,
      PropNameID::forAscii(*rt, "coolify"),
      0,
      [](Runtime &, const Value &thisVal, const Value *args, size_t count) {
        EXPECT_TRUE(thisVal.isUndefined());
        return thisVal.isUndefined();
      });
  rt->global().setProperty(*rt, "coolify", coolify);
  EXPECT_TRUE(eval(
                  "(function() {"
                  "  \"use strict\";"
                  "  return coolify.bind(undefined)();"
                  "})()")
                  .getBool());
}

TEST_P(HermesRuntimeTest, CreateValueFromJsonUtf8IgnoresGlobalJSON) {
  // Hermes parses JSON natively, and the sandbox uses the JSON.parse it saved
  // at startup, so replacing the global JSON.parse must not change the result,
  // also when the runtime is decorated (e.g. thread-safe).
  eval("JSON.parse = function() { return 'hijacked'; }");
  uint8_t json[] = "{\"a\": 1}";
  Value v = Value::createFromJsonUtf8(*rt, json, sizeof(json) - 1);
  ASSERT_TRUE(v.isObject());
  EXPECT_EQ(v.getObject(*rt).getProperty(*rt, "a").getNumber(), 1);
}

TEST_P(HermesRuntimeTest, DrainMicrotasksThrowsFinalizationRegistryError) {
  eval(R"(
    var target = {};
    globalThis.cleanupCount = 0;
    globalThis.registry = new FinalizationRegistry(() => {
      cleanupCount += 1;
      throw new Error('cleanup boom');
    });
    registry.register(target, 'held');
    target = null;
  )");
  eval("gc()");

  EXPECT_THROW(rt->drainMicrotasks(), JSError);
  EXPECT_EQ(eval("cleanupCount").asNumber(), 1);
}

TEST_P(HermesRuntimeTest, ResetTimezoneCache) {
  if (auto *hrt = castInterface<IHermes>(rt.get())) {
    EXPECT_NO_THROW({ hrt->resetTimezoneCache(); });
  }
}

/// Source for the string writer tests; \c written tracks how much of \c src
/// the writer has filled, to check the invocation contract.
template <typename S>
struct WriterSource {
  const S &src;
  size_t written = 0;
};

/// Copies the requested range of the WriterSource passed as \p ctx, checking
/// that ranges arrive in order and contiguous.
template <typename S>
static bool writeRange(
    void *ctx,
    typename S::value_type *dst,
    size_t offset,
    size_t count) noexcept {
  auto &source = *static_cast<WriterSource<S> *>(ctx);
  EXPECT_EQ(offset, source.written);
  EXPECT_LE(offset + count, source.src.size());
  for (size_t i = 0; i < count; ++i)
    dst[i] = source.src[offset + i];
  source.written = offset + count;
  return true;
}

TEST_P(HermesRuntimeTest, StringWriterTest) {
  auto *writer = castInterface<IStringWriter>(rt.get());
  if (!castInterface<IHermes>(rt.get())) {
    // Only runtimes backed by HermesRuntime provide the writer.
    return;
  }
  ASSERT_NE(writer, nullptr);

  // Around the interned (0/1 chars) and external (64K chars) thresholds.
  const size_t lengths[] = {0, 1, 2, 65535, 65536, 100000};
  // ASCII, Cyrillic and a surrogate pair, which a cut can leave unpaired.
  const char16_t pattern[] = {u'a', u'ж', 0xD83D, 0xDE42};
  for (size_t length : lengths) {
    std::string chars(length, '\0');
    std::u16string units(length, u'\0');
    for (size_t i = 0; i < length; ++i) {
      chars[i] = static_cast<char>('a' + i % 26);
      units[i] = pattern[i % 4];
    }
    WriterSource<std::string> asciiSource{chars};
    String fromAscii = writer->createStringFromAsciiWriter(
        length, &asciiSource, writeRange<std::string>);
    EXPECT_EQ(asciiSource.written, length);
    EXPECT_TRUE(
        String::strictEquals(
            *rt, fromAscii, String::createFromAscii(*rt, chars)));
    WriterSource<std::u16string> utf16Source{units};
    String fromUtf16 = writer->createStringFromUtf16Writer(
        length, &utf16Source, writeRange<std::u16string>);
    EXPECT_EQ(utf16Source.written, length);
    EXPECT_TRUE(
        String::strictEquals(
            *rt, fromUtf16, String::createFromUtf16(*rt, units)));
    EXPECT_EQ(fromUtf16.utf16(*rt), units);
  }

  // ASCII text through the UTF-16 writer still equals and keys like ASCII.
  std::u16string keyUnits = u"writerKey";
  WriterSource<std::u16string> keySource{keyUnits};
  String key = writer->createStringFromUtf16Writer(
      keyUnits.size(), &keySource, writeRange<std::u16string>);
  EXPECT_TRUE(
      String::strictEquals(
          *rt, key, String::createFromAscii(*rt, "writerKey")));
  Object obj(*rt);
  obj.setProperty(*rt, PropNameID::forString(*rt, key), 42);
  EXPECT_EQ(obj.getProperty(*rt, "writerKey").getNumber(), 42);

  // A writer that rejects its output.
  auto rejectAscii = [](void *, char *, size_t, size_t) noexcept {
    return false;
  };
  auto rejectUtf16 = [](void *, char16_t *, size_t, size_t) noexcept {
    return false;
  };
  EXPECT_THROW(
      writer->createStringFromAsciiWriter(1, nullptr, rejectAscii), JSError);
  EXPECT_THROW(
      writer->createStringFromAsciiWriter(100, nullptr, rejectAscii), JSError);
  EXPECT_THROW(
      writer->createStringFromUtf16Writer(100, nullptr, rejectUtf16), JSError);

  // Too long: a RangeError, and the writer is never called.
  auto unreachable = [](void *, char *, size_t, size_t) noexcept {
    ADD_FAILURE() << "writer called";
    return false;
  };
  const size_t tooLong = size_t(256) * 1024 * 1024 + 1;
  try {
    writer->createStringFromAsciiWriter(tooLong, nullptr, unreachable);
    ADD_FAILURE() << "expected a RangeError";
  } catch (const JSError &e) {
    EXPECT_TRUE(e.value().asObject(*rt).instanceOf(
        *rt, rt->global().getPropertyAsFunction(*rt, "RangeError")));
  }
}

TEST_P(HermesRuntimeTest, DescriptionTest) {
  // Minimally, if the description doesn't include "Hermes", something
  // is wrong.
  EXPECT_NE(rt->description().find("Hermes"), std::string::npos);

  std::shared_ptr<facebook::jsi::Runtime> rt2 = makeHermesRuntime(
      ::hermes::vm::RuntimeConfig::Builder()
          .withGCConfig(
              ::hermes::vm::GCConfig::Builder().withName("ForTesting").build())
          .build());
  EXPECT_NE(rt2->description().find("Hermes"), std::string::npos);
}

TEST_P(HermesRuntimeTest, ArrayBufferTest) {
  eval(
      "var buffer = new ArrayBuffer(16);\
        var int32View = new Int32Array(buffer);\
        int32View[0] = 1234;\
        int32View[1] = 5678;");

  Object object = rt->global().getPropertyAsObject(*rt, "buffer");
  EXPECT_TRUE(object.isArrayBuffer(*rt));

  auto arrayBuffer = object.getArrayBuffer(*rt);
  EXPECT_EQ(arrayBuffer.size(*rt), 16);

  // HermesSandboxRuntime does not support getting the ArrayBuffer's data.
  if (!dynamic_cast<HermesSandboxRuntime *>(rt.get())) {
    int32_t *buffer = reinterpret_cast<int32_t *>(arrayBuffer.data(*rt));
    EXPECT_EQ(buffer[0], 1234);
    EXPECT_EQ(buffer[1], 5678);
  }
}

class HermesRuntimeTestMethodsTest : public HermesRuntimeCustomConfigTest {
 public:
  HermesRuntimeTestMethodsTest()
      : HermesRuntimeCustomConfigTest(
            ::hermes::vm::RuntimeConfig::Builder()
                .withEnableHermesInternalTestMethods(true)
                .build()) {}
};

TEST_F(HermesRuntimeTestMethodsTest, ExternalArrayBufferTest) {
  struct FixedBuffer : MutableBuffer {
    size_t size() const override {
      return sizeof(arr);
    }
    uint8_t *data() override {
      return reinterpret_cast<uint8_t *>(arr.data());
    }

    std::array<uint32_t, 256> arr;
  };

  {
    auto buf = std::make_shared<FixedBuffer>();
    for (uint32_t i = 0; i < buf->arr.size(); i++)
      buf->arr[i] = i;
    auto arrayBuffer = ArrayBuffer(*rt, buf);
    auto square = eval(
        R"#(
(function (buf) {
  var view = new Uint32Array(buf);
  for(var i = 0; i < view.length; i++) view[i] = view[i] * view[i];
})
)#");
    square.asObject(*rt).asFunction(*rt).call(*rt, arrayBuffer);
    for (uint32_t i = 0; i < 256; i++)
      EXPECT_EQ(buf->arr[i], i * i);
  }

  {
    auto buf = std::make_shared<FixedBuffer>();
    std::weak_ptr<FixedBuffer> weakBuf(buf);
    auto arrayBuffer = ArrayBuffer(*rt, std::move(buf));
    EXPECT_EQ(weakBuf.use_count(), 1);
    auto detach = eval(
        R"#(
(function (buf) {
  var view = new Uint32Array(buf);
  HermesInternal.detachArrayBuffer(buf);
  view[0] = 5;
  view[0]
})
)#");
    EXPECT_TRUE(detach.asObject(*rt)
                    .asFunction(*rt)
                    .call(*rt, arrayBuffer)
                    .isUndefined());
    rt->instrumentation().collectGarbage("");
    EXPECT_TRUE(weakBuf.expired());
  }
}

TEST_F(HermesRuntimeTestMethodsTest, DetachedArrayBuffer) {
  auto ab = eval(
                R"(
  var x  = new ArrayBuffer(10);
  HermesInternal.detachArrayBuffer(x);
  x
)")
                .getObject(*rt)
                .getArrayBuffer(*rt);
  EXPECT_THROW(ab.data(*rt), JSINativeException);
}

TEST_F(HermesRuntimeTestMethodsTest, ArrayBufferDetached) {
  auto ab = eval("new ArrayBuffer(10)").getObject(*rt).getArrayBuffer(*rt);
  EXPECT_FALSE(ab.detached(*rt));

  auto detachedAb = eval(
                        R"(
  var x = new ArrayBuffer(10);
  HermesInternal.detachArrayBuffer(x);
  x
)")
                        .getObject(*rt)
                        .getArrayBuffer(*rt);
  EXPECT_TRUE(detachedAb.detached(*rt));
}

TEST(HermesRuntimeDecoratorTest, CreateArrayBufferCallsWith) {
  // Counts calls into a WithRuntimeDecorator.
  struct Count {
    void before() {
      ++entered;
    }
    void after() {
      ++exited;
    }

    int entered = 0;
    int exited = 0;
  };

  class CountRuntime final : public WithRuntimeDecorator<Count> {
   public:
    explicit CountRuntime(std::unique_ptr<Runtime> rt)
        : WithRuntimeDecorator<Count>(*rt, count), rt_(std::move(rt)) {}

    Count count;

   private:
    std::unique_ptr<Runtime> rt_;
  };

  struct FixedBuffer : MutableBuffer {
    size_t size() const override {
      return sizeof(arr);
    }
    uint8_t *data() override {
      return arr.data();
    }

    std::array<uint8_t, 8> arr{};
  };

  CountRuntime crt(makeHermesRuntime());
  auto arrayBuffer = ArrayBuffer(crt, std::make_shared<FixedBuffer>());

  // Creating the ArrayBuffer must go through the With hooks exactly once, like
  // every other call into the decorated runtime.
  EXPECT_EQ(crt.count.entered, 1);
  EXPECT_EQ(crt.count.exited, 1);
}

TEST_P(HermesRuntimeBytecodeTest, BytecodeTest) {
  auto *api = castInterface<IHermesRootAPI>(makeHermesRootAPI());
  const uint8_t shortBytes[] = {1, 2, 3};
  EXPECT_FALSE(api->isHermesBytecode(shortBytes, 0));
  EXPECT_FALSE(api->isHermesBytecode(shortBytes, sizeof(shortBytes)));
  uint8_t longBytes[1024];
  memset(longBytes, 'H', sizeof(longBytes));
  EXPECT_FALSE(api->isHermesBytecode(longBytes, sizeof(longBytes)));

  std::string bytecode;
  ASSERT_TRUE(hermes::compileJS("x = 1", bytecode));
  EXPECT_TRUE(api->isHermesBytecode(
      reinterpret_cast<const uint8_t *>(bytecode.data()), bytecode.size()));
  rt->evaluateJavaScript(
      std::unique_ptr<StringBuffer>(new StringBuffer(bytecode)), "");
  EXPECT_EQ(rt->global().getProperty(*rt, "x").getNumber(), 1);

  EXPECT_EQ(api->getBytecodeVersion(), hermes::hbc::BYTECODE_VERSION);
}

TEST(HermesRuntimePreparedJavaScriptTest, BytecodeTest) {
  auto rt = makeHermesRuntime();
  rt->evaluateJavaScript(std::make_unique<StringBuffer>("var q = 0;"), "");
  std::string bytecode;
  ASSERT_TRUE(hermes::compileJS("q++", bytecode));
  auto prep =
      rt->prepareJavaScript(std::make_unique<StringBuffer>(bytecode), "");
  EXPECT_EQ(rt->global().getProperty(*rt, "q").getNumber(), 0);
  rt->evaluatePreparedJavaScript(prep);
  EXPECT_EQ(rt->global().getProperty(*rt, "q").getNumber(), 1);
  rt->evaluatePreparedJavaScript(prep);
  EXPECT_EQ(rt->global().getProperty(*rt, "q").getNumber(), 2);
}

TEST_P(HermesRuntimeBytecodeTest, CompileWithSourceMapTest) {
  /* original source:
  const a: number = 12;
  class MyClass {
    val: number;
    constructor(val: number) {
      this.val = val;
    }
    doSomething(a: number, b: number) {
      invalidRef.sum = a + b + this.val;
    }
  }
  const c = new MyClass(3);
  c.doSomething(a, 15);*/
  const char *TestSource = R"#('use strict';
var a = 12;
var MyClass = /** @class */ (function () {
    function MyClass(val) {
        this.val = val;
    }
    MyClass.prototype.doSomething = function (a, b) {
        invalidRef.sum = a + b + this.val;
    };
    return MyClass;
}());
var c = new MyClass(3);
c.doSomething(a, 15);
//# sourceMappingURL=script.js.map)#";
  const char *TestSourceMap = R"#({
    "version":3,
    "file":"script.js",
    "sourceRoot":"",
    "sources":["script.ts"],
    "names":[],
    "mappings": ";AAAA,IAAM,CAAC,GAAW,EAAE,CAAC;AACrB;IAEI,iBAAY,GAAW;QACnB,IAAI,CAAC,GAAG,GAAG,GAAG,CAAC;IACnB,CAAC;IACD,6BAAW,GAAX,UAAY,CAAS,EAAE,CAAS;QAC5B,UAAU,CAAC,GAAG,GAAG,CAAC,GAAG,CAAC,GAAG,IAAI,CAAC,GAAG,CAAC;IACtC,CAAC;IACL,cAAC;AAAD,CAAC,AARD,IAQC;AACD,IAAM,CAAC,GAAG,IAAI,OAAO,CAAC,CAAC,CAAC,CAAC;AACzB,CAAC,CAAC,WAAW,CAAC,CAAC,EAAE,EAAE,CAAC,CAAC"
  })#";

  auto *api = castInterface<IHermesRootAPI>(makeHermesRootAPI());
  std::string bytecode;
  ASSERT_TRUE(
      hermes::compileJS(
          TestSource,
          "script.js",
          bytecode,
          true,
          true,
          nullptr,
          std::optional<std::string_view>(TestSourceMap)));
  EXPECT_TRUE(api->isHermesBytecode(
      reinterpret_cast<const uint8_t *>(bytecode.data()), bytecode.size()));
  try {
    rt->evaluateJavaScript(
        std::unique_ptr<StringBuffer>(new StringBuffer(bytecode)), "");
    FAIL() << "Expected JSIException";
  } catch (const facebook::jsi::JSIException &err) {
    EXPECT_STREQ(
        "Property 'invalidRef' doesn't exist\n\nReferenceError: Property 'invalidRef' doesn't exist\n    at anonymous (script.ts:8:9)\n    at global (script.ts:12:14)",
        err.what());
  }
}

TEST_P(HermesRuntimeBytecodeTest, JumpTableBytecodeTest) {
  std::string code = R"xyz(
    (function(){
var i = 0;
    switch (i) {
      case 0:
        return 5;
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
      case 6:
      case 7:
      case 8:
      case 9:
    }
})();
)xyz";
  std::string bytecode;
  ASSERT_TRUE(hermes::compileJS(code, bytecode));
  auto ret =
      rt->evaluateJavaScript(std::make_unique<StringBuffer>(bytecode), "");
  ASSERT_EQ(ret.asNumber(), 5.0);
}

TEST(HermesRuntimePreparedJavaScriptTest, InvalidSourceThrows) {
  auto rt = makeHermesRuntime();
  const char *badSource = "this is definitely not valid javascript";
  EXPECT_THROW(
      rt->prepareJavaScript(std::make_unique<StringBuffer>(badSource), ""),
      facebook::jsi::JSIException)
      << "prepareJavaScript should have thrown an exception";
}

TEST_P(HermesRuntimeTest, NoCorruptionOnJSError) {
  // If the test crashes or infinite loops, the likely cause is that
  // Hermes API library is not built with proper compiler flags
  // (-fexception in GCC/CLANG, /EHsc in MSVC)
  try {
    rt->evaluateJavaScript(std::make_unique<StringBuffer>("foo.bar = 1"), "");
    FAIL() << "Expected JSIException";
  } catch (const facebook::jsi::JSIException &) {
    // expected exception, ignore
  }
  try {
    rt->evaluateJavaScript(std::make_unique<StringBuffer>("foo.baz = 1"), "");
    FAIL() << "Expected JSIException";
  } catch (const facebook::jsi::JSIException &) {
    // expected exception, ignore
  }
  rt->evaluateJavaScript(std::make_unique<StringBuffer>("gc()"), "");
}

// In JSC we use multiple threads in our implementation of JSI so we can't
// use the ASSERT_DEATH macros when testing that implementation.
// Asserts are compiled out of opt builds
#if !defined(NDEBUG) && defined(ASSERT_DEATH)
TEST(HermesRuntimeDeathTest, ValueTest) {
  auto eval = [](Runtime &rt, const char *code) {
    return rt.global().getPropertyAsFunction(rt, "eval").call(rt, code);
  };

  ASSERT_DEATH(
      eval(*makeHermesRuntime(), "'slay'").getNumber(), "Assertion.*isNumber");
  ASSERT_DEATH(
      {
        auto rt = makeHermesRuntime();
        eval(*rt, "123").getString(*rt);
      },
      "Assertion.*isString");
}
#endif

TEST(HermesRootsTest, DontGrowWhenMoveObjectOutOfValue) {
  std::shared_ptr<HermesRuntime> rt = makeHermesRuntime();
  Value val = Object(*rt);
  // Keep the object alive during measurement.
  std::unique_ptr<Object> obj;
  auto helperRt = dynamicInterfaceCast<IHermesTestHelpers>(rt);
  auto rootsDelta = HermesTestHelper::calculateRootsListChange(
      *helperRt,
      [&]() { obj = std::make_unique<Object>(std::move(val).getObject(*rt)); });
  EXPECT_EQ(rootsDelta, 0);
}

TEST(HermesRootsTest, DontGrowWhenCloneObject) {
  std::shared_ptr<HermesRuntime> rt = makeHermesRuntime();
  Value val = Object(*rt);
  constexpr int kCloneCount = 1000;
  // Keep the objects alive during measurement.
  std::vector<Object> objects;
  objects.reserve(kCloneCount);
  auto helperRt = dynamicInterfaceCast<IHermesTestHelpers>(rt);
  auto rootsDelta =
      HermesTestHelper::calculateRootsListChange(*helperRt, [&]() {
        for (size_t i = 0; i < kCloneCount; i++) {
          objects.push_back(val.getObject(*rt));
        }
      });
  EXPECT_EQ(rootsDelta, 0);
}

TEST(HermesWatchTimeLimitTest, WatchTimeLimit) {
  // Some code that exercies the async break checks.
  const char *forABit = "var t = Date.now(); while (Date.now() < t + 100) {}";
  const char *forEver = "for (;;){}";
  uint32_t Around20MinsMS = 1234567;
  uint32_t ShortTimeoutMS = 123;
  {
    // Single runtime with ~20 minute limit that will not be reached.
    auto rt = makeHermesRuntime();
    rt->watchTimeLimit(Around20MinsMS);
    rt->evaluateJavaScript(std::make_unique<StringBuffer>(forABit), "");
  }
  {
    // Single runtime with timeout reset -- first a very long time out, then a
    // short one.
    auto rt = makeHermesRuntime();
    rt->watchTimeLimit(Around20MinsMS);
    rt->watchTimeLimit(ShortTimeoutMS);
    ASSERT_THROW(
        rt->evaluateJavaScript(std::make_unique<StringBuffer>(forEver), ""),
        JSIException);
  }
  {
    // Multiple runtimes, but neither will time out.
    auto rt1 = makeHermesRuntime();
    rt1->watchTimeLimit(Around20MinsMS);
    auto rt2 = makeHermesRuntime();
    rt2->watchTimeLimit(Around20MinsMS / 2);
    rt1->evaluateJavaScript(std::make_unique<StringBuffer>(forABit), "");
    rt2->evaluateJavaScript(std::make_unique<StringBuffer>(forABit), "");
  }
  {
    // Timeout in one of the runtimes. Make sure the first watchTimeLimit is the
    // "infinite one" as time TimeLimitMonitor could sleep for the given timeout
    // without properly handle the next timeouts.
    auto rt1 = makeHermesRuntime();
    rt1->watchTimeLimit(Around20MinsMS);
    auto rt2 = makeHermesRuntime();
    rt2->watchTimeLimit(ShortTimeoutMS);
    ASSERT_THROW(
        rt2->evaluateJavaScript(std::make_unique<StringBuffer>(forEver), ""),
        JSIException);
  }
  {
    auto timeLimitMonitor = hermes::vm::TimeLimitMonitor::getOrCreate();
    const auto &watchedRuntimes = timeLimitMonitor->getWatchedRuntimes();

    auto rt1 = makeHermesRuntime();
    rt1->watchTimeLimit(Around20MinsMS);
    auto rt2 = makeHermesRuntime();
    rt2->watchTimeLimit(Around20MinsMS);
    auto rt3 = makeHermesRuntime();
    rt3->watchTimeLimit(Around20MinsMS);
    EXPECT_EQ(watchedRuntimes.size(), 3);

    rt2 = nullptr;
    EXPECT_EQ(watchedRuntimes.size(), 2);
  }
}

#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
TEST_P(HermesRuntimeTest, GetHeapInfo) {
  // HermesSandboxRuntime uses the default JSI instrumentation, which reports no
  // heap information and does not collect garbage.
  if (dynamic_cast<HermesSandboxRuntime *>(rt.get()))
    return;

  auto &instrumentation = rt->instrumentation();
  // Make sure we do run some collections.
  instrumentation.collectGarbage("test");
  auto heapInfo = instrumentation.getHeapInfo(false);
  // Let's not assert the exact number of collections, which could be changed
  // in concrete Hades implementation.
  EXPECT_NE(heapInfo["hermes_full_numCollections"], 0);
  EXPECT_NE(heapInfo["hermes_yg_numCollections"], 0);
}
#endif

TEST_P(HermesRuntimeTest, TriggerAsyncTimeout) {
  auto runTest = [](auto *rt) {
    // Some code that loops forever to exercise the async interrupt.
    const char *forEver = "for (;;){}";
    uint32_t ShortTimeoutMS = 123;
    {
      std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(ShortTimeoutMS));
        rt->asyncTriggerTimeout();
      });
      ASSERT_THROW(
          rt->evaluateJavaScript(std::make_unique<StringBuffer>(forEver), ""),
          JSIException);
      t.join();
    }
  };

  // Only these runtimes support asyncTriggerTimeout.
  if (auto *hrt = dynamic_cast<HermesRuntime *>(rt.get()))
    runTest(hrt);
  else if (auto *hsrt = dynamic_cast<HermesSandboxRuntime *>(rt.get()))
    runTest(hsrt);
}

TEST(HermesRuntimeCrashManagerTest, CrashGetStackTrace) {
  class CrashManagerImpl : public hermes::vm::CrashManager {
   public:
    void registerMemory(void *, size_t) override {}
    void unregisterMemory(void *) override {}
    void setCustomData(const char *, const char *) override {}
    void removeCustomData(const char *) override {}
    void setContextualCustomData(const char *, const char *) override {}
    void removeContextualCustomData(const char *) override {}
    CallbackKey registerCallback(CallbackFunc cb) override {
      auto key = callbacks.size();
      callbacks.push_back(std::move(cb));
      return key;
    }
    void unregisterCallback(CallbackKey /*key*/) override {}
    void setHeapInfo(const HeapInformation & /*heapInfo*/) override {}

    std::vector<CallbackFunc> callbacks;
  };

  auto cm = std::make_shared<CrashManagerImpl>();
  auto rt = makeHermesRuntime(
      hermes::vm::RuntimeConfig::Builder().withCrashMgr(cm).build());
  Function runCrashCallbacks = Function::createFromHostFunction(
      *rt,
      PropNameID::forAscii(*rt, "runCrashCallbacks"),
      0,
      [&](Runtime &rt, const Value &, const Value *, size_t) {
        // Create a temporary file to write the crash manager data to.
        FILE *f = tmpfile();
        for (const auto &cb : cm->callbacks)
          cb(fileno(f));
        const auto sz = ftell(f);
        rewind(f);
        // Dump the crash manager data to a std::string.
        std::string out;
        out.resize(sz);
        const auto readsz = fread(&out[0], 1, sz, f);
        EXPECT_EQ(readsz, sz);
        fclose(f);
        return String::createFromUtf8(rt, std::move(out));
      });
  rt->global().setProperty(
      *rt, "runCrashCallbacks", std::move(runCrashCallbacks));
  std::string jsCode = R"(
function baz(){ return runCrashCallbacks(); }
function bar(){ return baz(); }
function foo(){ return bar(); }
var out = foo();
// Extract just the source locations of the call stack.
JSON.stringify(JSON.parse(out).callstack.map(x => x.SourceLocation));
)";
  auto buf = std::make_shared<StringBuffer>(std::move(jsCode));
  std::string callstack =
      rt->evaluateJavaScript(buf, "crashCode.js").asString(*rt).utf8(*rt);
  const char *expected =
      "[\"crashCode.js:2:41\",\"crashCode.js:3:27\",\"crashCode.js:4:27\",\"crashCode.js:5:14\",null]";
  EXPECT_EQ(callstack, expected);
}

TEST_P(HermesRuntimeTest, SpreadHostObjectWithOwnProperties) {
  // TODO(T174477667): Understand why this test fails for the sandbox under
  // MSVC.
#ifdef _MSC_VER
  if (dynamic_cast<HermesSandboxRuntime *>(rt.get()))
    return;
#endif

  class HostObjectWithPropertyNames : public HostObject {
    std::vector<PropNameID> getPropertyNames(Runtime &rt) override {
      return PropNameID::names(rt, "prop1", "1", "2", "prop2", "3");
    }
    Value get(Runtime &runtime, const PropNameID &name) override {
      return Value();
    }
  };

  Object ho = Object::createFromHostObject(
      *rt, std::make_shared<HostObjectWithPropertyNames>());
  rt->global().setProperty(*rt, "ho", ho);

  auto res = eval(R"###(
var spreaded = {...ho};
var props = Object.getOwnPropertyNames(spreaded);
props.toString();
)###")
                 .getString(*rt)
                 .utf8(*rt);
  EXPECT_EQ(res, "1,2,3,prop1,prop2");
}

TEST_P(HermesRuntimeTest, HostObjectWithOwnProperties) {
  class HostObjectWithPropertyNames : public HostObject {
    std::vector<PropNameID> getPropertyNames(Runtime &rt) override {
      return PropNameID::names(rt, "prop1", "1", "2", "prop2", "3");
    }
    Value get(Runtime &runtime, const PropNameID &name) override {
      if (PropNameID::compare(
              runtime, name, PropNameID::forAscii(runtime, "prop1")))
        return 10;
      return Value();
    }
  };

  Object ho = Object::createFromHostObject(
      *rt, std::make_shared<HostObjectWithPropertyNames>());
  rt->global().setProperty(*rt, "ho", ho);

  EXPECT_TRUE(eval("\"prop1\" in ho").getBool());
  EXPECT_TRUE(eval("1 in ho").getBool());
  EXPECT_TRUE(eval("2 in ho").getBool());
  EXPECT_TRUE(eval("\"prop2\" in ho").getBool());
  EXPECT_TRUE(eval("3 in ho").getBool());
  // HostObjects say they own any property, even if it's not in their property
  // names list.
  // This is an explicit design choice, to avoid the runtime and API costs of
  // handling checking for property existence.
  EXPECT_TRUE(eval("\"foo\" in ho").getBool());

  EXPECT_TRUE(eval(
                  "var properties = Object.getOwnPropertyNames(ho);"
                  "properties[0] === '1' && "
                  "properties[1] === '2' && "
                  "properties[2] === '3' && "
                  "properties[3] === 'prop1' && "
                  "properties[4] === 'prop2' && "
                  "properties.length === 5")
                  .getBool());
  EXPECT_TRUE(eval("ho[2] === undefined").getBool());
  EXPECT_TRUE(eval("ho.prop2 === undefined").getBool());

  eval("Object.defineProperty(ho, '0', {value: 'hi there'})");
  eval("Object.defineProperty(ho, '2', {value: 'hi there'})");
  eval("Object.defineProperty(ho, '4', {value: 'hi there'})");
  eval("Object.defineProperty(ho, 'prop2', {value: 'hi there'})");

  EXPECT_TRUE(eval(
                  "var properties = Object.getOwnPropertyNames(ho);"
                  "properties[0] === '0' && "
                  "properties[1] === '1' && "
                  "properties[2] === '2' && "
                  "properties[3] === '3' && "
                  "properties[4] === '4' && "
                  "properties[5] === 'prop2' && "
                  "properties[6] === 'prop1' && "
                  "properties.length === 7")
                  .getBool());
  EXPECT_TRUE(eval("ho[2] === 'hi there'").getBool());
  EXPECT_TRUE(eval("ho.prop2 === 'hi there'").getBool());

  // hasOwnProperty() always succeeds on HostObject
  EXPECT_TRUE(
      eval("Object.prototype.hasOwnProperty.call(ho, 'prop1')").getBool());
  EXPECT_TRUE(
      eval("Object.prototype.hasOwnProperty.call(ho, 'any-string')").getBool());

  // getOwnPropertyDescriptor() always succeeds on HostObject
  EXPECT_TRUE(eval(
                  "var d = Object.getOwnPropertyDescriptor(ho, 'prop1');"
                  "d != undefined && "
                  "d.value == 10 && "
                  "d.enumerable && "
                  "d.writable ")
                  .getBool());
  EXPECT_TRUE(eval(
                  "var d = Object.getOwnPropertyDescriptor(ho, 'any-string');"
                  "d != undefined && "
                  "d.value == undefined && "
                  "d.enumerable && "
                  "d.writable")
                  .getBool());
}

// TODO mhorowitz: move this to jsi/testlib.cpp once we have impls for all VMs
TEST_P(HermesRuntimeTest, WeakReferences) {
  Object o = eval("({one: 1})").getObject(*rt);
  WeakObject wo = WeakObject(*rt, o);
  rt->global().setProperty(*rt, "obj", o);

  eval("gc()");

  Value v = wo.lock(*rt);

  // At this point, the object has three strong refs (C++ o, v; JS global.obj).

  EXPECT_TRUE(v.isObject());
  EXPECT_EQ(v.getObject(*rt).getProperty(*rt, "one").asNumber(), 1);

  // Now start removing references.

  v = nullptr;

  // Two left

  eval("gc()");
  EXPECT_EQ(wo.lock(*rt).getObject(*rt).getProperty(*rt, "one").asNumber(), 1);

  o = Object(*rt);

  // Now one, only JS

  eval("gc()");
  EXPECT_EQ(wo.lock(*rt).getObject(*rt).getProperty(*rt, "one").asNumber(), 1);

  eval("obj = null");

  // Now none.

  eval("gc()");
  EXPECT_TRUE(wo.lock(*rt).isUndefined());

  // test where the last ref is C++

  o = eval("({two: 2})").getObject(*rt);
  wo = WeakObject(*rt, o);
  v = Value(*rt, o);

  eval("gc()");
  EXPECT_EQ(wo.lock(*rt).getObject(*rt).getProperty(*rt, "two").asNumber(), 2);

  v = nullptr;

  eval("gc()");
  EXPECT_EQ(wo.lock(*rt).getObject(*rt).getProperty(*rt, "two").asNumber(), 2);

  o = Object(*rt);

  eval("gc()");
  EXPECT_TRUE(wo.lock(*rt).isUndefined());
}

/// The source URL and the throwing source of SourceURLAppearsInBacktraceTest.
constexpr const char *kBacktraceTestURL =
    "//SourceURLAppearsInBacktraceTest/Test/URL";
constexpr const char *kBacktraceTestSource = R"(
function thrower() { throw new Error('Test Error Message')}
function throws1() { thrower(); }
throws1();
)";

/// Evaluate \p code, which must throw, in \p rt with kBacktraceTestURL as the
/// source URL, and check that the URL appears in the backtrace.
void expectSourceURLInBacktrace(Runtime &rt, const std::string &code) {
  bool caught = false;
  try {
    rt.evaluateJavaScript(
        std::make_unique<StringBuffer>(code), kBacktraceTestURL);
  } catch (facebook::jsi::JSError &err) {
    caught = true;
    EXPECT_TRUE(err.getStack().find(kBacktraceTestURL) != std::string::npos)
        << "Backtrace should contain source URL";
  }
  EXPECT_TRUE(caught) << "JS should have thrown an exception";
}

TEST_P(HermesRuntimeTest, SourceURLAppearsInBacktraceTest) {
  expectSourceURLInBacktrace(*rt, kBacktraceTestSource);
}

TEST_P(HermesRuntimeBytecodeTest, SourceURLAppearsInBacktraceTest) {
  std::string bytecode;
  bool compiled =
      hermes::compileJS(kBacktraceTestSource, kBacktraceTestURL, bytecode);
  ASSERT_TRUE(compiled) << "JS source should have compiled";
  expectSourceURLInBacktrace(*rt, bytecode);
}

TEST_P(HermesRuntimeTest, HostObjectAsParentTest) {
  class HostObjectWithProp : public HostObject {
    Value get(Runtime &runtime, const PropNameID &name) override {
      if (PropNameID::compare(
              runtime, name, PropNameID::forAscii(runtime, "prop1")))
        return 10;
      return Value();
    }
  };

  Object ho =
      Object::createFromHostObject(*rt, std::make_shared<HostObjectWithProp>());
  rt->global().setProperty(*rt, "ho", ho);

  EXPECT_TRUE(
      eval("var subClass = {__proto__: ho}; subClass.prop1 == 10;").getBool());
}

TEST_P(HermesRuntimeTest, NativeStateTest) {
  class C : public facebook::jsi::NativeState {
   public:
    std::atomic<int> *dtors;
    C(std::atomic<int> *_dtors) : dtors(_dtors) {}
    virtual ~C() override {
      ++*dtors;
    }
  };
  // Destructors run on the finalizer thread, so use atomics.
  std::atomic<int> dtors1 = 0;
  std::atomic<int> dtors2 = 0;
  auto waitForDtors = [](std::atomic<int> &dtor, int expected) {
    for (int i = 0; i < 200; ++i) {
      if (dtor.load() == expected)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };
  {
    Object obj = eval("({one: 1})").getObject(*rt);
    ASSERT_FALSE(obj.hasNativeState<C>(*rt));
    {
      // Set some state.
      obj.setNativeState(*rt, std::make_shared<C>(&dtors1));
      ASSERT_TRUE(obj.hasNativeState<C>(*rt));
      auto ptr = obj.getNativeState<C>(*rt);
      EXPECT_EQ(ptr->dtors, &dtors1);
    }
    {
      // Overwrite the state.
      obj.setNativeState(*rt, std::make_shared<C>(&dtors2));
      ASSERT_TRUE(obj.hasNativeState<C>(*rt));
      auto ptr = obj.getNativeState<C>(*rt);
      EXPECT_EQ(ptr->dtors, &dtors2);
    }
  } // closing scope -> obj unreachable
  // should finalize both. NativeState destructors are cleaned up by the
  // finalizer thread, not the GC thread, so we need to wait.
  eval("gc()");
  waitForDtors(dtors1, 1);
  waitForDtors(dtors2, 1);
  EXPECT_EQ(1, dtors1.load());
  EXPECT_EQ(1, dtors2.load());

  // Trying to set native state on frozen object should throw.
  {
    Object frozen = eval("Object.freeze({one: 1})").getObject(*rt);
    ASSERT_THROW(
        frozen.setNativeState(*rt, std::make_shared<C>(&dtors1)), JSIException);
  }
  // Make sure any NativeState cells are finalized before leaving, since they
  // point to local variables. Otherwise ASAN will complain.
  eval("gc()");
  waitForDtors(dtors1, 2);
}

TEST(HermesRuntimeConfigTest, NativeStateWeakObjectFinalization) {
  struct FinalizationState {
    const std::thread::id mutatorThread = std::this_thread::get_id();
    std::atomic<size_t> finalized{0};
    std::atomic<bool> finalizedOnMutator{false};
  } state;

  class WeakNativeState final : public NativeState {
   public:
    WeakNativeState(
        Runtime &runtime,
        const Object &object,
        FinalizationState &state)
        : weak_(std::in_place, runtime, object), state_(state) {}

    ~WeakNativeState() override {
      if (std::this_thread::get_id() == state_.mutatorThread) {
        state_.finalizedOnMutator = true;
      }
      weak_.reset();
      ++state_.finalized;
    }

   private:
    std::optional<WeakObject> weak_;
    FinalizationState &state_;
  };

  // Keep the counters alive until runtime teardown has joined the finalizer.
  auto runtime = makeHermesRuntime();
  constexpr size_t kNumBatches = 32;
  constexpr size_t kStatesPerBatch = 16384;
  for (size_t batch = 0; batch < kNumBatches; ++batch) {
    {
      Array owners(*runtime, kStatesPerBatch);
      for (size_t i = 0; i < kStatesPerBatch; ++i) {
        Object object(*runtime);
        object.setNativeState(
            *runtime,
            std::make_shared<WeakNativeState>(*runtime, object, state));
        owners.setValueAtIndex(*runtime, i, object);
      }
    }

    // Finalization releases the last reference to each weak-root slot while
    // collections traverse the same list. This stresses the interval between
    // checking whether a slot is occupied and visiting its value.
    const size_t expected = (batch + 1) * kStatesPerBatch;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
      runtime->instrumentation().collectGarbage("NativeStateWeakObject");
      std::this_thread::yield();
    } while (state.finalized < expected &&
             std::chrono::steady_clock::now() < deadline);
    ASSERT_EQ(state.finalized.load(), expected);
  }

  runtime.reset();
  EXPECT_EQ(state.finalized.load(), kNumBatches * kStatesPerBatch);
  EXPECT_FALSE(state.finalizedOnMutator.load());
}

TEST_P(HermesRuntimeTest, ExternalMemoryTest) {
  // Track the number of Counter destructor calls to verify the GC is freeing
  // objects under external memory pressure. This needs to be atomic because the
  // destructor of NativeState may be invoked on any thread.
  std::atomic<size_t> numFinalized = 0;

  class Counter : public HostObject, public NativeState {
   public:
    std::atomic<size_t> *numFinalized;
    Counter(std::atomic<size_t> *nf) : numFinalized(nf) {}
    ~Counter() {
      ++*numFinalized;
    }
  };

  auto waitForFinalized = [&](size_t expected) {
    for (int i = 0; i < 200; ++i) {
      if (numFinalized.load() >= expected)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  constexpr size_t kNumIter = 200;

  for (size_t i = 0; i < kNumIter; i++) {
    auto o = Object::createFromHostObject(
        *rt, std::make_shared<Counter>(&numFinalized));
    o.setExternalMemoryPressure(*rt, 1024 * 1024);
  }

  // Test that we can adjust memory pressure even on a frozen object.
  auto freeze = eval("Object.freeze").getObject(*rt).getFunction(*rt);
  for (size_t i = 0; i < kNumIter; i++) {
    Object o{*rt};
    o.setNativeState(*rt, std::make_shared<Counter>(&numFinalized));
    freeze.call(*rt, o);
    o.setExternalMemoryPressure(*rt, 1024 * 1024);
  }

  // Try setting a series of values on the same object.
  Object o{*rt};
  o.setExternalMemoryPressure(*rt, 5);
  o.setExternalMemoryPressure(*rt, 5);
  o.setExternalMemoryPressure(*rt, 0);
  o.setExternalMemoryPressure(*rt, 1024 * 1024);

  // Verify the GC collected a reasonable number of objects due to external
  // memory pressure. MallocGC does not handle external memory correctly, so
  // skip this check.
#if HERMESVM_GCKIND != _HERMESVM_GCVALUE_MALLOC
  // We created 2 * kNumIter Counter objects across both loops. Expect at least
  // half to have been finalized, proving the GC responded to memory pressure.
  waitForFinalized(kNumIter);
  EXPECT_GE(numFinalized.load(), kNumIter);
#endif

  // Make sure all Counter destructor calls (which runs on the finalizer thread)
  // complete before leaving, since they point to local variables.
  eval("gc()");
  waitForFinalized(2 * kNumIter);
}

TEST_P(HermesRuntimeTest, ExternalMemoryNoOpTest) {
  // HermesSandboxRuntime uses the default JSI instrumentation, which reports no
  // heap information, so the external memory cannot be measured.
  if (dynamic_cast<HermesSandboxRuntime *>(rt.get()))
    return;

  auto heapInfo = [&](const char *key) {
    return rt->instrumentation().getHeapInfo(false)[key];
  };
  auto externalBytes = [&] { return heapInfo("hermes_externalBytes"); };
  auto allocatedBytes = [&] { return heapInfo("hermes_totalAllocatedBytes"); };

  // The first property lookup on a fresh object may lazily allocate the
  // property map of its hidden class, so do one lookup up front to keep that
  // out of the measurements below.
  Object{*rt}.setExternalMemoryPressure(*rt, 0);

  Object o{*rt};
  const auto baseExternal = externalBytes();

  // Setting 0 on an object without external memory is a no-op, and must not
  // allocate anything on the JS heap.
  auto allocBefore = allocatedBytes();
  o.setExternalMemoryPressure(*rt, 0);
  o.setExternalMemoryPressure(*rt, 0);
  EXPECT_EQ(allocatedBytes(), allocBefore);
  EXPECT_EQ(externalBytes(), baseExternal);

  // Setting a real amount afterwards still works.
  o.setExternalMemoryPressure(*rt, 1024);
  EXPECT_EQ(externalBytes(), baseExternal + 1024);

  // Setting the same amount again is a no-op.
  allocBefore = allocatedBytes();
  o.setExternalMemoryPressure(*rt, 1024);
  EXPECT_EQ(allocatedBytes(), allocBefore);
  EXPECT_EQ(externalBytes(), baseExternal + 1024);

  // Going back to 0 and up again is accounted for correctly.
  o.setExternalMemoryPressure(*rt, 0);
  EXPECT_EQ(externalBytes(), baseExternal);
  o.setExternalMemoryPressure(*rt, 0);
  EXPECT_EQ(externalBytes(), baseExternal);
  o.setExternalMemoryPressure(*rt, 1024);
  EXPECT_EQ(externalBytes(), baseExternal + 1024);
  o.setExternalMemoryPressure(*rt, 512);
  EXPECT_EQ(externalBytes(), baseExternal + 512);

  // Proxies are rejected, even when the amount is 0.
  auto proxy = eval("new Proxy({}, {})").getObject(*rt);
  EXPECT_THROW(proxy.setExternalMemoryPressure(*rt, 0), JSINativeException);
  EXPECT_THROW(proxy.setExternalMemoryPressure(*rt, 1024), JSINativeException);
}

TEST_P(HermesRuntimeTest, PropNameIDFromSymbol) {
  auto strProp = PropNameID::forAscii(*rt, "a");
  auto secretProp = PropNameID::forSymbol(
      *rt, eval("var secret = Symbol('a'); secret;").getSymbol(*rt));
  auto globalProp =
      PropNameID::forSymbol(*rt, eval("Symbol.for('a');").getSymbol(*rt));
  auto x =
      eval("({a : 'str', [secret] : 'secret', [Symbol.for('a')] : 'global'});")
          .getObject(*rt);

  EXPECT_EQ(x.getProperty(*rt, strProp).getString(*rt).utf8(*rt), "str");
  EXPECT_EQ(x.getProperty(*rt, secretProp).getString(*rt).utf8(*rt), "secret");
  EXPECT_EQ(x.getProperty(*rt, globalProp).getString(*rt).utf8(*rt), "global");
}

TEST_P(HermesRuntimeTest, HostObjectSymbolTest) {
  class DummyHO : public HostObject {
   public:
    Value get(Runtime &, const PropNameID &name) {
      return Value();
    }
    void set(Runtime &rt, const PropNameID &name, const Value &value) {
      props.emplace_back(rt, name);
    }
    std::vector<PropNameID> getPropertyNames(Runtime &rt) {
      std::vector<PropNameID> propsRet;
      for (const auto &prop : props)
        propsRet.emplace_back(rt, prop);
      return propsRet;
    }

    std::vector<PropNameID> props;
  };
  auto hoObj = Object::createFromHostObject(*rt, std::make_shared<DummyHO>());
  rt->global().setProperty(*rt, "ho", hoObj);
  eval(R"(
var abcSym = Symbol("abc");
ho[abcSym] = 1;
var defSym = Symbol("def");
ho[defSym] = 2;
// Add a number symbol to test that we don't treat it as integer index.
var numberSym = Symbol("5");
ho[numberSym] = 55;
ho.xyz = 5;
ho.qwerty = 6;
// Create some duplicate properties to test deduplication.
Object.defineProperty(ho, "xyz", {
  value: 42,
});
Object.defineProperty(ho, "foo", {
  value: 42,
});
Object.defineProperty(ho, abcSym, {
  value: 78,
});
var hoDefSym = Symbol("def");
Object.defineProperty(ho, hoDefSym, {
  value: 26,
});

let assert = function(cond, msg) {
  if (!cond) {
    throw new Error(msg);
  }
};

// Ensure that the values are the same, and have the same order.
let arrayEqual = function(arr1, arr2) {
  if (arr1.length !== arr2.length) {
    return false;
  }
  for (var i = 0; i < arr1.length; i++) {
    if (arr1[i] !== arr2[i]) {
      return false;
    }
  }
  return true;
};

let strArr = Object.getOwnPropertyNames(ho);
// Hidden class properties should come before HO properties.
assert(arrayEqual(strArr, ["xyz", "foo", "qwerty"]),
  "getOwnPropertyNames() returns incorrectly.");
let symArr = Object.getOwnPropertySymbols(ho);
assert(arrayEqual(symArr, [abcSym, hoDefSym, defSym, numberSym]),
  "getOwnPropertySymbols() returns incorrectly.");
)");
}

TEST_P(HermesRuntimeTest, ArrayTest) {
  auto array = eval("[1, 2, 3]").getObject(*rt);
  EXPECT_TRUE(array.isArray(*rt));
  auto jsiArray = array.getArray(*rt);
  EXPECT_EQ(jsiArray.size(*rt), 3);
  EXPECT_EQ(jsiArray.getValueAtIndex(*rt, 0).asNumber(), 1);
  jsiArray.setValueAtIndex(*rt, 1, 0);
  EXPECT_EQ(jsiArray.getValueAtIndex(*rt, 1).asNumber(), 0);

  array = eval("new Proxy([4, 5, 6], {})").getObject(*rt);
  EXPECT_TRUE(array.isArray(*rt));
  jsiArray = array.getArray(*rt);
  EXPECT_EQ(jsiArray.size(*rt), 3);
  EXPECT_EQ(jsiArray.getValueAtIndex(*rt, 0).asNumber(), 4);
  jsiArray.setValueAtIndex(*rt, 1, 0);
  EXPECT_EQ(jsiArray.getValueAtIndex(*rt, 1).asNumber(), 0);
}

TEST_P(HermesRuntimeTest, HasComputedTest) {
  // The only use of JSObject::hasComputed() is in HermesRuntimeImpl,
  // so we test its Proxy support here, instead of from JS.

  EXPECT_FALSE(eval("'prop' in new Proxy({}, {})").getBool());
  EXPECT_TRUE(eval("'prop' in new Proxy({prop:1}, {})").getBool());
  EXPECT_FALSE(
      eval("'prop' in new Proxy({}, {has() { return false; }})").getBool());
  EXPECT_TRUE(
      eval("'prop' in new Proxy({}, {has() { return true; }})").getBool());

  // While we're here, test that a HostFunction can be used as a proxy
  // trap.  This could be very powerful in the right hands.
  Function returnTrue = Function::createFromHostFunction(
      *rt,
      PropNameID::forAscii(*rt, "returnTrue"),
      0,
      [](Runtime &rt, const Value &, const Value *args, size_t count) {
        EXPECT_EQ(count, 2);
        EXPECT_EQ(args[1].toString(rt).utf8(rt), "prop");
        return true;
      });
  rt->global().setProperty(*rt, "returnTrue", returnTrue);
  EXPECT_TRUE(eval("'prop' in new Proxy({}, {has: returnTrue})").getBool());
}

TEST_P(HermesRuntimeTest, GlobalObjectTest) {
  rt->global().setProperty(*rt, "a", 5);
  eval("f = function(b) { return a + b; }");
  eval("gc()");
  EXPECT_EQ(eval("f(10)").getNumber(), 15);
}

class HermesRuntimeTestWithDisableGenerator
    : public HermesRuntimeCustomConfigTest {
 public:
  HermesRuntimeTestWithDisableGenerator()
      : HermesRuntimeCustomConfigTest(
            ::hermes::vm::RuntimeConfig::Builder()
                .withEnableGenerator(false)
                .build()) {}
};

TEST_F(HermesRuntimeTestWithDisableGenerator, WithDisableGenerator) {
  EXPECT_THROW(
      rt->evaluateJavaScript(
          std::make_unique<StringBuffer>("function* foo() {}"), ""),
      facebook::jsi::JSIException)
      << "Expected JSIException";

  EXPECT_THROW(
      rt->evaluateJavaScript(
          std::make_unique<StringBuffer>("obj = {*foo() {}}"), ""),
      facebook::jsi::JSIException)
      << "Expected JSIException";

  // async function depends on generator.
  EXPECT_THROW(
      rt->evaluateJavaScript(
          std::make_unique<StringBuffer>("async function foo() {}"), ""),
      facebook::jsi::JSIException)
      << "Expected JSIException";
}

TEST_P(HermesRuntimeTest, DiagnosticHandlerTestError) {
  using DiagnosticHandler = hermes::DiagnosticHandler;

  struct BufferingDiagnosticHandler : DiagnosticHandler {
    void handle(const DiagnosticHandler::Diagnostic &d) {
      ds.push_back(d);
    }
    std::vector<DiagnosticHandler::Diagnostic> ds;
  } diagHandler;
  std::string bytecode;
  ASSERT_FALSE(
      hermes::compileJS("x++1", "", bytecode, true, true, &diagHandler));
  ASSERT_EQ(1, diagHandler.ds.size());
  EXPECT_EQ(DiagnosticHandler::Error, diagHandler.ds[0].kind);
  EXPECT_EQ(1, diagHandler.ds[0].line);
  EXPECT_EQ(4, diagHandler.ds[0].column);
}

TEST_P(HermesRuntimeTest, DiagnosticHandlerTestWarning) {
  using DiagnosticHandler = hermes::DiagnosticHandler;

  struct BufferingDiagnosticHandler : DiagnosticHandler {
    void handle(const DiagnosticHandler::Diagnostic &d) {
      ds.push_back(d);
    }
    std::vector<DiagnosticHandler::Diagnostic> ds;
  } diagHandler;
  std::string bytecode;
  // Succeeds with a warning + associated note.
  ASSERT_TRUE(
      hermes::compileJS("({a:1,a:2})", "", bytecode, true, true, &diagHandler));
  ASSERT_EQ(2, diagHandler.ds.size());

  // warning: the property "a" was set multiple times in the object definition.
  EXPECT_EQ(DiagnosticHandler::Warning, diagHandler.ds[0].kind);
  EXPECT_EQ(1, diagHandler.ds[0].line);
  EXPECT_EQ(7, diagHandler.ds[0].column);
  ASSERT_EQ(1, diagHandler.ds[0].ranges.size());
  EXPECT_EQ(6, diagHandler.ds[0].ranges[0].first);
  EXPECT_EQ(9, diagHandler.ds[0].ranges[0].second);

  // The first definition was here.
  EXPECT_EQ(DiagnosticHandler::Note, diagHandler.ds[1].kind);
  EXPECT_EQ(1, diagHandler.ds[1].line);
  EXPECT_EQ(3, diagHandler.ds[1].column);
  ASSERT_EQ(1, diagHandler.ds[1].ranges.size());
  EXPECT_EQ(2, diagHandler.ds[1].ranges[0].first);
  EXPECT_EQ(5, diagHandler.ds[1].ranges[0].second);
}

TEST_P(HermesRuntimeTest, BigIntJSI) {
  Function bigintCtor = rt->global().getPropertyAsFunction(*rt, "BigInt");
  auto BigInt = [&](const char *v) { return bigintCtor.call(*rt, eval(v)); };

  auto v0 = BigInt("0");
  auto b0 = v0.asBigInt(*rt);
  EXPECT_EQ(v0.toString(*rt).utf8(*rt), "0");
  EXPECT_EQ(b0.toString(*rt).utf8(*rt), "0");

  auto vffffffffffffffff = BigInt("0xffffffffffffffffn");
  auto bffffffffffffffff = vffffffffffffffff.asBigInt(*rt);
  EXPECT_EQ(vffffffffffffffff.toString(*rt).utf8(*rt), "18446744073709551615");
  EXPECT_EQ(bffffffffffffffff.toString(*rt, 16).utf8(*rt), "ffffffffffffffff");
  EXPECT_EQ(bffffffffffffffff.toString(*rt, 36).utf8(*rt), "3w5e11264sgsf");

  auto vNeg1 = BigInt("-1");
  auto bNeg1 = vNeg1.asBigInt(*rt);
  EXPECT_EQ(vNeg1.toString(*rt).utf8(*rt), "-1");
  EXPECT_EQ(bNeg1.toString(*rt, 16).utf8(*rt), "-1");
  EXPECT_EQ(bNeg1.toString(*rt, 36).utf8(*rt), "-1");

  EXPECT_TRUE(BigInt::strictEquals(*rt, b0, b0));
  EXPECT_TRUE(BigInt::strictEquals(*rt, bffffffffffffffff, bffffffffffffffff));
  EXPECT_FALSE(BigInt::strictEquals(*rt, bNeg1, bffffffffffffffff));
}

TEST_P(HermesRuntimeTest, BigIntJSIFromScalar) {
  Function bigintCtor = rt->global().getPropertyAsFunction(*rt, "BigInt");
  auto BigInt = [&](const char *v) {
    return bigintCtor.call(*rt, eval(v)).asBigInt(*rt);
  };

  EXPECT_TRUE(
      BigInt::strictEquals(*rt, BigInt("0"), BigInt::fromUint64(*rt, 0)));
  EXPECT_TRUE(
      BigInt::strictEquals(*rt, BigInt("0"), BigInt::fromInt64(*rt, 0)));
  EXPECT_TRUE(
      BigInt::strictEquals(
          *rt, BigInt("0xdeadbeef"), BigInt::fromUint64(*rt, 0xdeadbeef)));
  EXPECT_TRUE(
      BigInt::strictEquals(
          *rt, BigInt("0xc0ffee"), BigInt::fromInt64(*rt, 0xc0ffee)));
  EXPECT_TRUE(
      BigInt::strictEquals(
          *rt, BigInt("0xffffffffffffffffn"), BigInt::fromUint64(*rt, ~0ull)));
  EXPECT_TRUE(
      BigInt::strictEquals(*rt, BigInt("-1"), BigInt::fromInt64(*rt, ~0ull)));
}

TEST_P(HermesRuntimeTest, BigIntJSIToString) {
  auto b = BigInt::fromUint64(*rt, 1);
  // Test all possible radixes.
  for (int radix = 2; radix <= 36; ++radix) {
    EXPECT_EQ(b.toString(*rt, radix).utf8(*rt), "1") << radix;
  }

  // Test some invaild radixes.
  EXPECT_THROW(b.toString(*rt, -1), JSIException);
  EXPECT_THROW(b.toString(*rt, 0), JSIException);
  EXPECT_THROW(b.toString(*rt, 1), JSIException);
  EXPECT_THROW(b.toString(*rt, 37), JSIException);
  EXPECT_THROW(b.toString(*rt, 100), JSIException);

  Function bigintCtor = rt->global().getPropertyAsFunction(*rt, "BigInt");
  auto BigInt = [&](int value) {
    return bigintCtor.call(*rt, value).asBigInt(*rt);
  };

  // Now test that the radix is being passed to the VM.
  for (int radix = 2; radix <= 36; ++radix) {
    EXPECT_EQ(BigInt(radix + 1).toString(*rt, radix).utf8(*rt), "11") << radix;
    EXPECT_EQ(BigInt(-(radix + 1)).toString(*rt, radix).utf8(*rt), "-11")
        << radix;
  }
}

TEST_P(HermesRuntimeTest, BigIntJSITruncation) {
  auto lossless = [](uint64_t value) { return std::make_tuple(value, true); };
  auto lossy = [](uint64_t value) { return std::make_tuple(value, false); };

  auto toInt64 = [this](const BigInt &b) {
    return std::make_tuple(b.getInt64(*rt), b.isInt64(*rt));
  };

  auto toUint64 = [this](const BigInt &b) {
    return std::make_tuple(b.getUint64(*rt), b.isUint64(*rt));
  };

  Function bigintCtor = rt->global().getPropertyAsFunction(*rt, "BigInt");
  auto BigInt = [&](const char *v) {
    return bigintCtor.call(*rt, eval(v)).asBigInt(*rt);
  };

  // 0n can be truncated losslessly to either int64_t and uint64_t
  auto b = BigInt::fromUint64(*rt, 0);
  EXPECT_EQ(toUint64(b), lossless(0));
  EXPECT_TRUE(
      BigInt::strictEquals(*rt, BigInt::fromUint64(*rt, b.getUint64(*rt)), b));
  EXPECT_EQ(toInt64(b), lossless(0));
  EXPECT_TRUE(
      BigInt::strictEquals(*rt, BigInt::fromInt64(*rt, b.getInt64(*rt)), b));

  // Creating BigInt from an ~0ull. This value can't be truncated losslessly to
  // int64_t.
  b = BigInt::fromUint64(*rt, ~0ull);
  EXPECT_EQ(toUint64(b), lossless(~0ull));
  EXPECT_TRUE(
      BigInt::strictEquals(*rt, BigInt::fromUint64(*rt, b.getUint64(*rt)), b));
  EXPECT_EQ(toInt64(b), lossy(~0ull));

  // Creating BigInt from an -1ull. This value can't be truncated losslessly to
  // int64_t.
  b = BigInt::fromInt64(*rt, -1ull);
  EXPECT_EQ(toUint64(b), lossy(-1ull));
  EXPECT_EQ(toInt64(b), lossless(-1ull));
  EXPECT_TRUE(
      BigInt::strictEquals(*rt, BigInt::fromInt64(*rt, b.getInt64(*rt)), b));

  // 0x10000000000000000n can't be truncated to int64_t nor uint64_t.
  b = BigInt("0x10000000000000000n");
  EXPECT_EQ(toUint64(b), lossy(0));
  EXPECT_EQ(toInt64(b), lossy(0));

  // -0x10000000000000000n can't be truncated to int64_t nor uint64_t.
  b = BigInt("-0x10000000000000000n");
  EXPECT_EQ(toUint64(b), lossy(0));
  EXPECT_EQ(toInt64(b), lossy(0));

  // (1n << 65n) - 1n can't be truncated to int64_t nor uint64_t.
  b = BigInt("(1n << 65n) - 1n");
  EXPECT_EQ(toUint64(b), lossy(~0ull));
  EXPECT_EQ(toInt64(b), lossy(~0ull));
}

#ifdef HERMESVM_EXCEPTION_ON_OOM
class HermesRuntimeTestSmallHeap : public HermesRuntimeCustomConfigTest {
 public:
  HermesRuntimeTestSmallHeap()
      : HermesRuntimeCustomConfigTest(
            ::hermes::vm::RuntimeConfig::Builder()
                .withGCConfig(
                    ::hermes::vm::GCConfig::Builder()
                        .withInitHeapSize(8 << 20)
                        .withMaxHeapSize(8 << 20)
                        .build())
                .build()) {}
};

TEST_F(HermesRuntimeTestSmallHeap, HostFunctionPropagatesOOMExceptionTest) {
  auto func = Function::createFromHostFunction(
      *rt,
      PropNameID::forAscii(*rt, ""),
      1,
      [](Runtime &rt, const Value &, const Value *args, unsigned long count) {
        assert(count > 0);
        auto func = args[0].asObject(rt).asFunction(rt);
        return func.call(rt, args + 1, count - 1);
      });

  auto makeOOM = eval(R"#(
(function (){
  var outer = [];
  while(true){
    var inner = [];
    for (var i = 0; i < 10000; i++) inner.push({});
    outer.push(inner);
  }
})
)#");
  EXPECT_THROW(func.call(*rt, makeOOM), ::hermes::vm::JSOutOfMemoryError);
}

TEST_F(HermesRuntimeTestSmallHeap, CreateJSErrorPropagatesOOMExceptionTest) {
  // Use the default createError implementation so the overridden global Error
  // constructor below triggers the OOM.
  class RD : public RuntimeDecorator<Runtime, Runtime> {
   public:
    explicit RD(Runtime &rt) : RuntimeDecorator(rt) {}

    Value createError(const String &msg) override {
      return Runtime::createError(msg);
    }
  };
  RD drt(*rt);

  eval(R"#(
globalThis.Error = function (){
  var outer = [];
  while(true){
    var inner = [];
    for (var i = 0; i < 10000; i++) inner.push({});
    outer.push(inner);
  }
};
)#");
  EXPECT_THROW(throw JSError(drt, "Foo"), ::hermes::vm::JSOutOfMemoryError);
}

TEST_F(HermesRuntimeTestSmallHeap, InterpreterUnwindOOM) {
  // Test that the interpreter does not attempt to restore the IP in the runtime
  // during exception unwinding. This would be a unnecessary, and causes
  // assertion failures because the register stack is not unwound.
  // It is important in order to reliably test this that the function where the
  // OOM occurs does not make any calls, or contain any numbers in registers, so
  // that we reliably get a crash if the interpreter tries to access the SavedIP
  // slot during unwinding.
  const char *src = R"#(
function foo(){
  let obj = {};
  let i = "a";
  while(true) obj[ i += "a" ] = ["a", "b", "c", "d", "e", "f"];
}
foo();
)#";
  EXPECT_THROW(eval(src), ::hermes::vm::JSOutOfMemoryError);
}
#endif

TEST_P(HermesRuntimeTest, NativeExceptionDoesNotUseGlobalError) {
  Function alwaysThrows = Function::createFromHostFunction(
      *rt,
      PropNameID::forAscii(*rt, "alwaysThrows"),
      0,
      [](Runtime &, const Value &, const Value *, size_t) -> Value {
        throw std::logic_error(
            "Native std::logic_error C++ exception in Host Function");
      });
  rt->global().setProperty(*rt, "alwaysThrows", alwaysThrows);
  rt->global().setProperty(*rt, "Error", 10);

  auto test = eval(
                  R"#((function(val) {
                          'use strict';
                          try {
                            alwaysThrows(val);
                          } catch(e) {
                            return 'typeof Error is ' + typeof(Error) + '; ' + e.message;
                          }
                          throw new Error('Unreachable statement');
                       }))#")
                  .getObject(*rt)
                  .getFunction(*rt);
  EXPECT_EQ(
      "typeof Error is number; Exception in HostFunction: Native "
      "std::logic_error C++ exception in Host Function",
      test.call(*rt).getString(*rt).utf8(*rt));
}

TEST_P(HermesRuntimeTest, UTF16ConversionTest) {
  String ascii = String::createFromUtf8(*rt, "z");
  EXPECT_EQ(ascii.utf16(*rt), u"z");

  String foobar = String::createFromUtf8(*rt, "foobar");
  EXPECT_EQ(foobar.utf16(*rt), u"foobar");

  // 你 in UTF-8 encoding is 0xe4 0xbd 0xa0 and 好 is 0xe5 0xa5 0xbd
  // 你 in UTF-16 encoding is 0x4f60 and 好 is 0x597d
  String chineseHello = String::createFromUtf8(*rt, "\xe4\xbd\xa0\xe5\xa5\xbd");
  EXPECT_EQ(chineseHello.utf16(*rt), u"\x4f60\x597d");

  // 👍 in UTF-8 encoding is 0xf0 0x9f 0x91 0x8d
  // 👍 in UTF-16 encoding is 0xd83d 0xdc4d
  String thumbsUpEmoji = String::createFromUtf8(*rt, "\xf0\x9f\x91\x8d");
  EXPECT_EQ(thumbsUpEmoji.utf16(*rt), u"\xd83d\xdc4d");

  // String is foobar👍你好
  String combined = String::createFromUtf8(
      *rt, "foobar\xf0\x9f\x91\x8d\xe4\xbd\xa0\xe5\xa5\xbd");
  EXPECT_EQ(combined.utf16(*rt), u"foobar\xd83d\xdc4d\x4f60\x597d");

  // HermesSandboxRuntime converts strings to UTF-16 through UTF-8, which
  // replaces lone surrogates with U+FFFD.
  if (dynamic_cast<HermesSandboxRuntime *>(rt.get()))
    return;

  // Thumbs up emoji is encoded as 0xd83d 0xdc4d. These test UTF16 with lone
  // high and low surrogates.
  String loneHighSurrogate = eval("'\\ud83d'").getString(*rt);
  EXPECT_EQ(loneHighSurrogate.utf16(*rt), std::u16string(u"\xd83d"));

  String loneLowSurrogate = eval("'\\udc4d'").getString(*rt);
  EXPECT_EQ(loneLowSurrogate.utf16(*rt), std::u16string(u"\xdc4d"));
}

TEST_P(HermesRuntimeTest, PropNameIDForPredefinedStringUtf16Test) {
  // Predefined strings are registered as lazy identifiers, so their
  // StringPrimitive is only created when it is first requested. Reading a
  // PropNameID for one must not do that from inside a no-allocation scope.
  auto prop = PropNameID::forAscii(*rt, "+Infinity");
  EXPECT_EQ(prop.utf16(*rt), u"+Infinity");
  EXPECT_EQ(prop.utf8(*rt), "+Infinity");
}

TEST_P(HermesRuntimeTest, CreateFromUtf16Test) {
  std::u16string utf16 = u"foobar";

  auto jsString = String::createFromUtf16(*rt, utf16);
  EXPECT_EQ(jsString.utf16(*rt), utf16);
  auto prop = PropNameID::forUtf16(*rt, utf16);
  EXPECT_EQ(prop.utf16(*rt), utf16);

  // 👋 in UTF-16 encoding is 0xd83d 0xdc4b
  utf16 = u"hello!\xd83d\xdc4b";
  jsString = String::createFromUtf16(*rt, utf16.data(), utf16.length());
  EXPECT_EQ(jsString.utf16(*rt), utf16);
  prop = PropNameID::forUtf16(*rt, utf16);
  EXPECT_EQ(prop.utf16(*rt), utf16);

  // HermesSandboxRuntime converts strings to UTF-16 through UTF-8, which
  // replaces lone surrogates with U+FFFD.
  if (dynamic_cast<HermesSandboxRuntime *>(rt.get()))
    return;

  // Thumbs up emoji is encoded as 0xd83d 0xdc4d. The following tests String
  // creation with a lone surrogate.
  utf16 = u"\xd83d";
  jsString = String::createFromUtf16(*rt, utf16.data(), utf16.length());
  EXPECT_EQ(jsString.utf16(*rt), utf16);
  prop = PropNameID::forUtf16(*rt, utf16);
  EXPECT_EQ(prop.utf16(*rt), utf16);
}

TEST_P(HermesRuntimeTest, GetStringDataTest) {
  std::u16string buf;
  auto cb = [&buf](bool ascii, const void *data, size_t num) {
    // this callback copies the string content, but removes every 'o' character
    if (ascii) {
      const char *begin = (const char *)data;
      const char *end = (const char *)data + num;
      while (begin < end) {
        char curr = begin[0];
        if (curr != 'o') {
          buf.push_back((char16_t)curr);
        }
        begin++;
      }
    } else {
      const char16_t *begin = (const char16_t *)data;
      const char16_t *end = (const char16_t *)data + num;
      while (begin < end) {
        char16_t curr = begin[0];
        if (curr != 'o') {
          buf.push_back(curr);
        }
        begin++;
      }
    }
  };

  String asciiString = String::createFromUtf8(*rt, "foobar");
  asciiString.getStringData(*rt, cb);
  EXPECT_EQ(buf, u"fbar");
  buf.clear();

  // String is foobar👍你好
  String utf16Str = String::createFromUtf8(
      *rt, "foobar\xf0\x9f\x91\x8d\xe4\xbd\xa0\xe5\xa5\xbd");
  utf16Str.getStringData(*rt, cb);
  EXPECT_EQ(buf, u"fbar\xd83d\xdc4d\x4f60\x597d");
  buf.clear();
}

TEST_P(HermesRuntimeTest, GetPropNameIdDataTest) {
  std::u16string buf;
  auto cb = [&buf](bool ascii, const void *data, size_t num) {
    // this callback copies the string content, but removes every 'o' character
    if (ascii) {
      const char *begin = (const char *)data;
      const char *end = (const char *)data + num;
      while (begin < end) {
        char curr = begin[0];
        if (curr != 'o') {
          buf.push_back((char16_t)curr);
        }
        begin++;
      }
    } else {
      const char16_t *begin = (const char16_t *)data;
      const char16_t *end = (const char16_t *)data + num;
      while (begin < end) {
        char16_t curr = begin[0];
        if (curr != 'o') {
          buf.push_back(curr);
        }
        begin++;
      }
    }
  };

  PropNameID ascii = PropNameID::forAscii(*rt, "foobar");
  ascii.getPropNameIdData(*rt, cb);
  EXPECT_EQ(buf, u"fbar");
  buf.clear();

  // String is foobar👍你好
  PropNameID utf16 = PropNameID::forUtf8(
      *rt, "foobar\xf0\x9f\x91\x8d\xe4\xbd\xa0\xe5\xa5\xbd");
  utf16.getPropNameIdData(*rt, cb);
  EXPECT_EQ(buf, u"fbar\xd83d\xdc4d\x4f60\x597d");
  buf.clear();
}

TEST_P(HermesRuntimeTest, SetPrototypeOf) {
  Object prototypeObj(*rt);
  prototypeObj.setProperty(*rt, "someProperty", 123);
  Value prototype(*rt, prototypeObj);

  Object child(*rt);
  child.setPrototype(*rt, prototype);
  EXPECT_EQ(child.getProperty(*rt, "someProperty").getNumber(), 123);

  auto getPrototypeRes = child.getPrototype(*rt).asObject(*rt);
  EXPECT_EQ(getPrototypeRes.getProperty(*rt, "someProperty").getNumber(), 123);

  // Tests null value as prototype
  child.setPrototype(*rt, Value::null());
  EXPECT_TRUE(child.getPrototype(*rt).isNull());

  // Throw when prototype is neither an Object nor null
  EXPECT_THROW(child.setPrototype(*rt, Value(1)), JSError);
}

TEST_P(HermesRuntimeTest, CreateObjectWithPrototype) {
  Object prototypeObj(*rt);
  prototypeObj.setProperty(*rt, "someProperty", 123);
  Value prototype(*rt, prototypeObj);

  Object child = Object::create(*rt, prototype);
  EXPECT_EQ(child.getProperty(*rt, "someProperty").getNumber(), 123);

  // Tests null value as prototype
  child = Object::create(*rt, Value::null());
  EXPECT_TRUE(child.getPrototype(*rt).isNull());

  // Throw when prototype is neither an Object nor null
  EXPECT_THROW(Object::create(*rt, Value(1)), JSError);
}

TEST_P(HermesRuntimeTest, SetRuntimeData) {
  UUID uuid1{0xe67ab3d6, 0x09a0, 0x11f0, 0xa641, 0x325096b39f47};
  auto str = std::make_shared<std::string>("hello world");
  rt->setRuntimeData(uuid1, str);

  UUID uuid2{0xa12f99fc, 0x09a2, 0x11f0, 0x84de, 0x325096b39f47};
  auto obj1 = std::make_shared<Object>(*rt);
  rt->setRuntimeData(uuid2, obj1);

  auto storedStr =
      std::static_pointer_cast<std::string>(rt->getRuntimeData(uuid1));
  auto storedObj = std::static_pointer_cast<Object>(rt->getRuntimeData(uuid2));
  EXPECT_EQ(storedStr, str);
  EXPECT_EQ(storedObj, obj1);

  // Override the existing value at uuid1
  auto weakOldStr = std::weak_ptr<std::string>(str);
  str = std::make_shared<std::string>("goodbye world");
  rt->setRuntimeData(uuid1, str);
  storedStr = std::static_pointer_cast<std::string>(rt->getRuntimeData(uuid1));
  EXPECT_EQ(str, storedStr);
  // Verify that the old data was not held on after it was deleted
  EXPECT_EQ(weakOldStr.use_count(), 0);

  auto rt2 = GetParam()();
  UUID uuid3{0x16f55892, 0x1034, 0x11f0, 0x8f65, 0x325096b39f47};
  auto obj2 = std::make_shared<Object>(*rt2);
  rt2->setRuntimeData(uuid3, obj2);

  auto storedObj2 =
      std::static_pointer_cast<Object>(rt2->getRuntimeData(uuid3));
  EXPECT_EQ(storedObj2, obj2);

  // UUID 1 is for data in the first runtime, so we expect nullptr here
  EXPECT_FALSE(rt2->getRuntimeData(uuid1));

  // Verify that when the runtime gets destroyed, the custom data is also
  // released.
  auto weakObj2 = std::weak_ptr<Object>(obj2);
  obj2.reset();
  storedObj2.reset();
  rt2.reset();
  EXPECT_EQ(weakObj2.use_count(), 0);
}

TEST_P(HermesRuntimeTest, DeleteProperty) {
  eval("var obj = {1:2, foo: 'bar', 3:4, salt: 'pepper'}");
  auto obj = rt->global().getPropertyAsObject(*rt, "obj");

  auto prop = PropNameID::forAscii(*rt, "1");
  obj.deleteProperty(*rt, prop);
  auto hasRes = obj.hasProperty(*rt, prop);
  EXPECT_FALSE(hasRes);

  auto str = String::createFromAscii(*rt, "foo");
  obj.deleteProperty(*rt, str);
  hasRes = obj.hasProperty(*rt, str);
  EXPECT_FALSE(hasRes);

  auto valProp = Value(3);
  obj.deleteProperty(*rt, valProp);
  auto getRes = obj.getProperty(*rt, "3");
  EXPECT_TRUE(getRes.isUndefined());

  hasRes = obj.hasProperty(*rt, "salt");
  EXPECT_TRUE(hasRes);
  obj.deleteProperty(*rt, "salt");
  hasRes = obj.hasProperty(*rt, "salt");
  EXPECT_FALSE(hasRes);

  obj = eval(
            "const obj = {};"
            "Object.defineProperty(obj, 'prop', {"
            "value: 10,"
            "configurable: false,"
            "}); obj;")
            .getObject(*rt);
  prop = PropNameID::forAscii(*rt, "prop");
  EXPECT_THROW(obj.deleteProperty(*rt, prop), JSError);
  hasRes = obj.hasProperty(*rt, "prop");
  EXPECT_TRUE(hasRes);
}

TEST_P(HermesRuntimeTest, SymbolKeys) {
  // A Symbol key and a String key with the symbol's description must stay
  // apart in every operation, including deleting through a PropNameID.
  auto sym = eval("Symbol('key')").getSymbol(*rt);
  auto symVal = Value(*rt, sym);
  auto symProp = PropNameID::forSymbol(*rt, sym);
  Object obj(*rt);
  obj.setProperty(*rt, symVal, 1);
  obj.setProperty(*rt, "key", 2);
  EXPECT_TRUE(obj.hasProperty(*rt, symVal));
  EXPECT_EQ(obj.getProperty(*rt, symVal).getNumber(), 1);
  EXPECT_EQ(obj.getProperty(*rt, symProp).getNumber(), 1);

  obj.deleteProperty(*rt, symProp);
  EXPECT_FALSE(obj.hasProperty(*rt, symVal));
  EXPECT_EQ(obj.getProperty(*rt, "key").getNumber(), 2);

  obj.setProperty(*rt, symProp, 3);
  obj.deleteProperty(*rt, symVal);
  EXPECT_FALSE(obj.hasProperty(*rt, symProp));
  EXPECT_TRUE(obj.hasProperty(*rt, "key"));
}

TEST_P(HermesRuntimeTest, JSIIgnoresReplacedBuiltins) {
  // JSI methods must not depend on globals or prototype properties that JS can
  // replace. Replace every built-in that the sandbox saves at startup, or that
  // the default JSI implementations use, with a function that returns a
  // number, and check that JSI still works. Use evaluateJavaScript, since
  // eval() goes through the global eval.
  auto run = [&](const char *code) {
    return rt->evaluateJavaScript(std::make_shared<StringBuffer>(code), "");
  };
  run(R"(
    var originals = {Error, TypeError, Uint8Array};
    var replacement = function() { return 42; };
    var getter = {get: replacement, configurable: true};
    var typedArrayProto = Object.getPrototypeOf(Uint8Array.prototype);
    for (var name of ['buffer', 'byteOffset', 'byteLength', 'length'])
      Object.defineProperty(typedArrayProto, name, getter);
    Object.defineProperty(typedArrayProto, Symbol.toStringTag, getter);
    Object.defineProperty(ArrayBuffer.prototype, 'detached', getter);
    // The default runtime data needs to install this global.
    Object.defineProperty(globalThis, '_jsiRuntimeDataCleanUp', {value: 0});
    for (var name of [
        'create', 'defineProperty', 'getPrototypeOf', 'setPrototypeOf',
        'getOwnPropertyDescriptor'])
      Object[name] = replacement;
    for (var name of ['deleteProperty', 'get', 'has', 'ownKeys', 'set'])
      Reflect[name] = replacement;
    JSON.parse = replacement;
    for (var name of [
        'eval', 'queueMicrotask', 'Uint8Array', 'Error', 'EvalError',
        'RangeError', 'ReferenceError', 'SyntaxError', 'TypeError',
        'URIError'])
      globalThis[name] = replacement;
  )");

  // A string with an unpaired surrogate, which the sandbox creates with eval.
  std::u16string utf16 = u"a\xd83d";
  rt->global().setProperty(*rt, "str", String::createFromUtf16(*rt, utf16));
  EXPECT_TRUE(
      run("str.length === 2 && str.charCodeAt(1) === 0xd83d").getBool());
  Object obj(*rt);
  obj.setProperty(*rt, PropNameID::forUtf16(*rt, utf16), 1);
  rt->global().setProperty(*rt, "obj", obj);
  EXPECT_TRUE(run("obj[str] === 1").getBool());

  // Properties with a Value key.
  Object props(*rt);
  Value key(*rt, String::createFromAscii(*rt, "key"));
  props.setProperty(*rt, key, 7);
  EXPECT_TRUE(props.hasProperty(*rt, key));
  Value prop = props.getProperty(*rt, key);
  ASSERT_TRUE(prop.isNumber());
  EXPECT_EQ(prop.getNumber(), 7);

  // Runtime data.
  UUID uuid{0x3c1f2a8e, 0x5b7d, 0x4e21, 0x9a6c, 0x1f2e3d4c5b6a};
  auto data = std::make_shared<int>(1);
  rt->setRuntimeData(uuid, data);
  EXPECT_EQ(rt->getRuntimeData(uuid), data);

  // Deleting a property, by each kind of key.
  obj.setProperty(*rt, "a", 1);
  obj.setProperty(*rt, "b", 2);
  obj.deleteProperty(*rt, PropNameID::forUtf16(*rt, utf16));
  obj.deleteProperty(*rt, String::createFromAscii(*rt, "a"));
  obj.deleteProperty(*rt, Value(*rt, String::createFromAscii(*rt, "b")));
  EXPECT_TRUE(run("Object.keys(obj).length === 0").getBool());

  // Prototypes.
  Object proto(*rt);
  Object child = Object::create(*rt, Value(*rt, proto));
  EXPECT_TRUE(
      Object::strictEquals(*rt, child.getPrototype(*rt).getObject(*rt), proto));
  child.setPrototype(*rt, Value::null());
  EXPECT_TRUE(child.getPrototype(*rt).isNull());

  // JSON.
  uint8_t json[] = "[1, 2]";
  auto parsed = Value::createFromJsonUtf8(*rt, json, sizeof(json) - 1);
  ASSERT_TRUE(parsed.isObject());
  EXPECT_EQ(parsed.getObject(*rt).getArray(*rt).size(*rt), 2);

  // TypedArrays and ArrayBuffers.
  Uint8Array u8(*rt, 8);
  EXPECT_TRUE(u8.isTypedArray(*rt));
  EXPECT_TRUE(u8.isUint8Array(*rt));
  EXPECT_EQ(u8.length(*rt), 8);
  EXPECT_EQ(u8.byteLength(*rt), 8);
  EXPECT_EQ(u8.byteOffset(*rt), 0);
  auto buffer = u8.buffer(*rt);
  EXPECT_EQ(buffer.size(*rt), 8);
  EXPECT_FALSE(buffer.detached(*rt));
  Uint8Array view(*rt, buffer, 2, 4);
  EXPECT_EQ(view.byteOffset(*rt), 2);
  EXPECT_EQ(view.length(*rt), 4);
  // Inheriting from Uint8Array.prototype does not make an object a TypedArray.
  auto fake = run("({__proto__: originals.Uint8Array.prototype})");
  EXPECT_FALSE(fake.getObject(*rt).isTypedArray(*rt));
  EXPECT_FALSE(run("new Int8Array(1)").getObject(*rt).isUint8Array(*rt));

  // Errors.
  rt->global().setProperty(
      *rt, "error", JSError::createTypeError(*rt, "boom").value());
  EXPECT_TRUE(run("error instanceof originals.TypeError && "
                  "error.message === 'boom'")
                  .getBool());
  rt->global().setProperty(*rt, "error", JSError(*rt, "bang").value());
  EXPECT_TRUE(run("error instanceof originals.Error && "
                  "error.message === 'bang'")
                  .getBool());

  // Microtasks.
  run("var ran = false;");
  rt->queueMicrotask(
      run("(function() { ran = true; })").getObject(*rt).getFunction(*rt));
  rt->drainMicrotasks();
  EXPECT_TRUE(run("ran").getBool());

  // Value::toString calls the global String, and must check its result.
  run("String = replacement;");
  EXPECT_THROW(Value(1).toString(*rt), JSError);
}

TEST_P(HermesRuntimeTest, CreateFromUtf16UnpairedSurrogates) {
  // Check the results in JS, since utf16() goes through UTF-8 on the sandbox.
  // Each string has one kind of unpaired surrogate, so that a mistake with one
  // kind cannot be hidden by another.
  auto check = [&](const std::u16string &str) {
    Array codes(*rt, str.size());
    for (size_t i = 0; i < str.size(); ++i)
      codes.setValueAtIndex(*rt, i, static_cast<double>(str[i]));
    Object obj(*rt);
    obj.setProperty(*rt, PropNameID::forUtf16(*rt, str), 1);
    auto global = rt->global();
    global.setProperty(*rt, "str", String::createFromUtf16(*rt, str));
    global.setProperty(*rt, "codes", codes);
    global.setProperty(*rt, "obj", obj);
    return eval(
               "str === String.fromCharCode.apply(null, codes) && "
               "Object.keys(obj).length === 1 && obj[str] === 1")
        .getBool();
  };

  // Every code unit up to 0xFF, and the line terminators, followed by an
  // unpaired surrogate so that the sandbox creates it from a string literal.
  std::u16string escapes;
  for (char16_t c = 0; c < 0x100; ++c)
    escapes.push_back(c);
  escapes += u"\x2028\x2029\xd800";
  EXPECT_TRUE(check(escapes));
  // A low surrogate on its own.
  EXPECT_TRUE(check(u"\xdc00"));
  // A high surrogate in the middle of a string.
  EXPECT_TRUE(check(
      u"a\xdbff"
      u"b"));
  // Surrogates in the wrong order.
  EXPECT_TRUE(check(u"\xdc00\xd800"));
  // A pair followed by an unpaired high surrogate.
  EXPECT_TRUE(check(u"\xd83d\xdc4d\xd83d"));
  // A valid pair, which does not need a string literal.
  EXPECT_TRUE(check(u"\xd83d\xdc4d"));
}

TEST_P(HermesRuntimeTest, ObjectTest) {
  eval("var obj = {1:2, 3:4}");
  auto obj = rt->global().getPropertyAsObject(*rt, "obj");

  auto propVal = Value(1);
  // Check for and get existing properties
  auto hasRes = obj.hasProperty(*rt, propVal);
  EXPECT_TRUE(hasRes);
  auto getRes = obj.getProperty(*rt, propVal);
  EXPECT_EQ(getRes.getNumber(), 2);
  // Overwrite existing property
  obj.setProperty(*rt, propVal, 3);
  getRes = obj.getProperty(*rt, propVal);
  EXPECT_EQ(getRes.getNumber(), 3);

  // Tests for non-existing properties
  propVal = Value(5);
  hasRes = obj.hasProperty(*rt, propVal);
  EXPECT_FALSE(hasRes);
  getRes = obj.getProperty(*rt, propVal);
  EXPECT_TRUE(getRes.isUndefined());

  // Add new property
  obj.setProperty(*rt, propVal, "bar");
  hasRes = obj.hasProperty(*rt, propVal);
  EXPECT_TRUE(hasRes);
  getRes = obj.getProperty(*rt, propVal);
  EXPECT_EQ(getRes.getString(*rt).utf8(*rt), "bar");

  obj = eval(
            "Object.defineProperty(obj, '456', {"
            "  value: 10,"
            "  writable: false,});")
            .getObject(*rt);
  auto unwritableProp = Value(456);
  EXPECT_THROW(obj.setProperty(*rt, unwritableProp, 1), JSError);

  auto badObjKey = eval(
      "var badObj = {"
      "    toString: function() {"
      "        throw new Error('something went wrong');"
      "    }"
      "};"
      "badObj;");
  EXPECT_THROW(obj.setProperty(*rt, badObjKey, 123), JSError);
  EXPECT_THROW(obj.hasProperty(*rt, badObjKey), JSError);
  EXPECT_THROW(obj.getProperty(*rt, badObjKey), JSError);
}

TEST_P(HermesRuntimeTest, FinalizableHostFunctionConstructorTest) {
  // Create a host function that returns an object when called with truthy arg,
  // otherwise returns a non-object value
  Function hostFunc = Function::createFromHostFunction(
      *rt,
      PropNameID::forAscii(*rt, "HostFunc"),
      1,
      [](Runtime &rt, const Value &, const Value *args, size_t count) -> Value {
        if (count > 0 && args[0].getBool()) {
          Object obj(rt);
          obj.setProperty(rt, "value", 42);
          return Value(rt, obj);
        }
        return Value(123);
      });

  auto result = hostFunc.call(*rt, Value(true));
  EXPECT_TRUE(result.isObject());
  EXPECT_EQ(result.getObject(*rt).getProperty(*rt, "value").getNumber(), 42);

  // Calling as a constructor with object return should succeed (using JSI APIs)
  result = hostFunc.callAsConstructor(*rt, Value(true));
  EXPECT_TRUE(result.isObject());
  EXPECT_EQ(result.getObject(*rt).getProperty(*rt, "value").getNumber(), 42);

  // Calling as a constructor with non-object return should throw TypeError
  EXPECT_THROW(hostFunc.callAsConstructor(*rt, Value(false)), JSError);

  rt->global().setProperty(*rt, "HostFunc", hostFunc);

  // Calling as a normal function via JS.
  result = eval("HostFunc(true)");
  EXPECT_TRUE(result.isObject());
  EXPECT_EQ(result.getObject(*rt).getProperty(*rt, "value").getNumber(), 42);

  // Calling as a constructor via JS with object return should succeed.
  result = eval("new HostFunc(true)");
  EXPECT_TRUE(result.isObject());
  EXPECT_EQ(result.getObject(*rt).getProperty(*rt, "value").getNumber(), 42);

  // Calling as a constructor via JS with non-object return should
  // throw TypeError
  EXPECT_THROW(eval("new HostFunc(false)"), JSError);
}

TEST_P(HermesRuntimeTest, ArrayPush) {
  // Push to an empty array
  Array arr(*rt, 0);
  size_t newLength = arr.push(*rt, 1, 2, 3);
  EXPECT_EQ(newLength, 3);
  EXPECT_EQ(arr.length(*rt), 3);

  EXPECT_EQ(arr.getValueAtIndex(*rt, 0).getNumber(), 1);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 1).getNumber(), 2);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 2).getNumber(), 3);

  // Push to an array already containing elements
  arr = Array::createWithElements(*rt, 1, true);
  Object obj(*rt);
  newLength = arr.push(*rt, "foobar", obj);
  EXPECT_EQ(newLength, 4);
  EXPECT_EQ(arr.length(*rt), 4);

  EXPECT_EQ(arr.getValueAtIndex(*rt, 0).getNumber(), 1);
  EXPECT_TRUE(arr.getValueAtIndex(*rt, 1).getBool());
  EXPECT_EQ(arr.getValueAtIndex(*rt, 2).getString(*rt).utf8(*rt), "foobar");
  EXPECT_TRUE(
      Object::strictEquals(
          *rt, arr.getValueAtIndex(*rt, 3).getObject(*rt), obj));

  // Modifies the parent of the JSArray, tests the slow path
  arr = eval(
            "var arr = ['apple', 'orange'];"
            "var obj = {foo: 'bar'};"
            "Object.setPrototypeOf(arr, obj);"
            "arr;")
            .getObject(*rt)
            .getArray(*rt);
  newLength = arr.push(*rt, 1, 2);
  EXPECT_EQ(newLength, 4);
  EXPECT_EQ(arr.length(*rt), 4);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 0).getString(*rt).utf8(*rt), "apple");
  EXPECT_EQ(arr.getValueAtIndex(*rt, 1).getString(*rt).utf8(*rt), "orange");
  EXPECT_EQ(arr.getValueAtIndex(*rt, 2).getNumber(), 1);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 3).getNumber(), 2);

  // Push to a Proxy of a JS Array
  arr = eval("new Proxy([1], {})").getObject(*rt).getArray(*rt);
  EXPECT_EQ(arr.length(*rt), 1);

  newLength = arr.push(*rt, true, "foobar");
  EXPECT_EQ(newLength, 3);
  EXPECT_EQ(arr.length(*rt), 3);

  EXPECT_EQ(arr.getValueAtIndex(*rt, 0).getNumber(), 1);
  EXPECT_TRUE(arr.getValueAtIndex(*rt, 1).getBool());
  EXPECT_EQ(arr.getValueAtIndex(*rt, 2).getString(*rt).utf8(*rt), "foobar");

  // Push to a Proxy of a JS Array, where getting the length returns a custom
  // value
  arr = eval(
            "var arr = [1];"
            "var handler = {"
            "    get(target, property) {"
            "        if (property == 'length') {"
            "            return target.length + 1;"
            "        }"
            "        return Reflect.get(target, property);"
            "    }"
            "};"
            "var proxy = new Proxy(arr, handler);"
            "proxy;")
            .getObject(*rt)
            .getArray(*rt);
  // The handler returns the underlying array's length plus 1
  EXPECT_EQ(arr.length(*rt), 2);

  // The elements will be adding starting at element 2
  newLength = arr.push(*rt, 3, 4);
  EXPECT_EQ(newLength, 4);

  EXPECT_EQ(arr.length(*rt), 5);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 0).getNumber(), 1);
  EXPECT_TRUE(arr.getValueAtIndex(*rt, 1).isUndefined());
  EXPECT_EQ(arr.getValueAtIndex(*rt, 2).getNumber(), 3);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 3).getNumber(), 4);

  // Push to a Proxy of a JS Array, where setting the 'length' property is
  // customized
  arr = eval(
            "var arr = [1];"
            "var handler = {"
            "    set(target, property, value) {"
            "        if (property == 'length') {"
            "            return Reflect.set(target, property, value + 1);"
            "        }"
            "        return Reflect.set(target, property, value);"
            "    }"
            "};"
            "var proxy = new Proxy(arr, handler);"
            "proxy;")
            .getObject(*rt)
            .getArray(*rt);

  EXPECT_EQ(arr.length(*rt), 1);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 0).getNumber(), 1);

  newLength = arr.push(*rt, 2, 3);
  EXPECT_EQ(newLength, 3);

  // When setting the 'length' property, the handler will actually set it to 3 +
  // 1
  EXPECT_EQ(arr.length(*rt), 4);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 0).getNumber(), 1);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 1).getNumber(), 2);
  EXPECT_EQ(arr.getValueAtIndex(*rt, 2).getNumber(), 3);
  EXPECT_TRUE(arr.getValueAtIndex(*rt, 3).isUndefined());
}

TEST_P(HermesRuntimeTest, TryGetMutableBuffer) {
  auto arrayBuffer =
      eval("new ArrayBuffer(8);").getObject(*rt).getArrayBuffer(*rt);
  EXPECT_EQ(arrayBuffer.tryGetMutableBuffer(*rt), nullptr);

  // HermesSandboxRuntime cannot create an ArrayBuffer from a MutableBuffer.
  if (dynamic_cast<HermesSandboxRuntime *>(rt.get()))
    return;

  struct TestBuffer : MutableBuffer {
    size_t size() const override {
      return sizeof(arr);
    }
    uint8_t *data() override {
      return reinterpret_cast<uint8_t *>(arr.data());
    }

    std::array<uint8_t, 3> arr;
  };
  auto buf = std::make_shared<TestBuffer>();
  for (uint32_t i = 0; i < buf->arr.size(); i++) {
    buf->arr[i] = i;
  }
  arrayBuffer = ArrayBuffer(*rt, buf);

  auto mutableBuffer = arrayBuffer.tryGetMutableBuffer(*rt);
  auto *mutableBufferData = mutableBuffer->data();
  EXPECT_EQ(mutableBuffer->size(), 3);
  EXPECT_EQ(mutableBufferData[0], 0);
  EXPECT_EQ(mutableBufferData[1], 1);
  EXPECT_EQ(mutableBufferData[2], 2);

  // Modifying the returned MutableBuffer data also modifies the ArrayBuffer
  // data.
  mutableBufferData[1] = 100;
  auto *arrayBufferData = arrayBuffer.data(*rt);
  EXPECT_EQ(arrayBufferData[0], 0);
  EXPECT_EQ(arrayBufferData[1], 100);
  EXPECT_EQ(arrayBufferData[2], 2);
}

TEST_P(HermesRuntimeTest, TryGetMutableBufferAfterHiddenClassTransition) {
  // HermesSandboxRuntime cannot create an ArrayBuffer from a MutableBuffer.
  if (dynamic_cast<HermesSandboxRuntime *>(rt.get()))
    return;

  struct TestBuffer : MutableBuffer {
    size_t size() const override {
      return sizeof(arr);
    }
    uint8_t *data() override {
      return arr.data();
    }
    std::array<uint8_t, 8> arr{};
  };

  // Adding a property to an external ArrayBuffer steals the DictPropertyMap
  // so the lookup for the next one has to allocate.
  ArrayBuffer a(*rt, std::make_shared<TestBuffer>());
  a.setExternalMemoryPressure(*rt, 8);

  ArrayBuffer b(*rt, std::make_shared<TestBuffer>());
  EXPECT_NE(b.tryGetMutableBuffer(*rt), nullptr);
}

TEST_P(HermesRuntimeTest, UInt8Array) {
  // Test creating a UInt8Array with a specific length
  {
    Uint8Array uint8Array(*rt, 10);

    EXPECT_EQ(uint8Array.length(*rt), 10);
    EXPECT_EQ(uint8Array.byteLength(*rt), 10);
    EXPECT_EQ(uint8Array.byteOffset(*rt), 0);
  }

  // Test creating a UInt8Array from an ArrayBuffer with offset and length
  {
    const auto ab =
        eval("new ArrayBuffer(20)").getObject(*rt).getArrayBuffer(*rt);

    // Create a Uint8Array starting at offset 5 with length 10
    Uint8Array uint8Array(*rt, ab, 5, 10);

    EXPECT_EQ(uint8Array.length(*rt), 10);
    EXPECT_EQ(uint8Array.byteLength(*rt), 10);
    EXPECT_EQ(uint8Array.byteOffset(*rt), 5);

    // Test buffer returns the correct underlying ArrayBuffer
    auto buffer = uint8Array.buffer(*rt);
    EXPECT_EQ(buffer.size(*rt), 20);
    EXPECT_TRUE(Object::strictEquals(*rt, ab, buffer));
  }
}

TEST_F(HermesRuntimeTestMethodsTest, UInt8ArrayDetached) {
  // Test that byteLength, byteOffset, and length return 0 after detach.
  {
    Uint8Array uint8Array(*rt, 10);
    auto buf = uint8Array.buffer(*rt);
    // Detach the underlying ArrayBuffer.
    rt->global().setProperty(*rt, "buf", std::move(buf));
    eval("HermesInternal.detachArrayBuffer(buf)");

    EXPECT_EQ(uint8Array.length(*rt), 0);
    EXPECT_EQ(uint8Array.byteLength(*rt), 0);
    EXPECT_EQ(uint8Array.byteOffset(*rt), 0);
  }

  // Test with a non-zero byteOffset.
  {
    auto ab = eval("new ArrayBuffer(20)").getObject(*rt).getArrayBuffer(*rt);
    Uint8Array uint8Array(*rt, ab, 5, 10);
    rt->global().setProperty(*rt, "buf", std::move(ab));
    eval("HermesInternal.detachArrayBuffer(buf)");

    EXPECT_EQ(uint8Array.byteOffset(*rt), 0);
    EXPECT_EQ(uint8Array.byteLength(*rt), 0);
    EXPECT_EQ(uint8Array.length(*rt), 0);
  }

  // Test that .buffer() still returns a valid (detached) ArrayBuffer.
  {
    Uint8Array uint8Array(*rt, 10);
    auto buf = uint8Array.buffer(*rt);
    rt->global().setProperty(*rt, "buf", Value(*rt, buf));
    eval("HermesInternal.detachArrayBuffer(buf)");

    auto detachedBuf = uint8Array.buffer(*rt);
    EXPECT_TRUE(detachedBuf.detached(*rt));
    EXPECT_TRUE(Object::strictEquals(*rt, buf, detachedBuf));
  }

  // Test that multiple Uint8Arrays sharing the same buffer all reflect detach.
  {
    auto ab = eval("new ArrayBuffer(20)").getObject(*rt).getArrayBuffer(*rt);
    Uint8Array ta1(*rt, ab, 0, 10);
    Uint8Array ta2(*rt, ab, 10, 10);
    rt->global().setProperty(*rt, "buf", std::move(ab));
    eval("HermesInternal.detachArrayBuffer(buf)");

    EXPECT_EQ(ta1.length(*rt), 0);
    EXPECT_EQ(ta1.byteLength(*rt), 0);
    EXPECT_EQ(ta1.byteOffset(*rt), 0);
    EXPECT_EQ(ta2.length(*rt), 0);
    EXPECT_EQ(ta2.byteLength(*rt), 0);
    EXPECT_EQ(ta2.byteOffset(*rt), 0);
  }
}

TEST_P(HermesRuntimeTest, IsTypedArrayTest) {
  // Test that isTypedArray returns false for a regular object
  {
    auto obj = Object(*rt);
    EXPECT_FALSE(rt->isTypedArray(obj));
    EXPECT_THROW(obj.asTypedArray(*rt), JSIException);
  }

  // Test that isTypedArray returns true for a Uint8Array
  {
    Uint8Array uint8Array(*rt, 10);
    EXPECT_TRUE(rt->isTypedArray(uint8Array));

    auto typedArray = uint8Array.getTypedArray(*rt);
    EXPECT_EQ(typedArray.length(*rt), 10);
  }

  // Test that isTypedArray returns true for other TypedArray types
  {
    auto int32ArrayObj = eval("new Int32Array(5)").getObject(*rt);
    EXPECT_TRUE(rt->isTypedArray(int32ArrayObj));
    EXPECT_FALSE(rt->isUint8Array(int32ArrayObj));

    auto typedArray = int32ArrayObj.getTypedArray(*rt);
    EXPECT_EQ(typedArray.length(*rt), 5);
  }

  // Test that isTypedArray returns true for Float64Array
  {
    auto float64ArrayObj = eval("new Float64Array(3)").getObject(*rt);
    EXPECT_TRUE(rt->isTypedArray(float64ArrayObj));

    auto typedArray = float64ArrayObj.asTypedArray(*rt);
    EXPECT_EQ(typedArray.length(*rt), 3);
  }

  // Test that isTypedArray returns false for ArrayBuffer
  {
    auto arrayBufferObj = eval("new ArrayBuffer(10)").getObject(*rt);
    EXPECT_FALSE(rt->isTypedArray(arrayBufferObj));
  }

  // Test that isTypedArray returns false for a regular array
  {
    auto arrayObj = eval("[1, 2, 3]").getObject(*rt);
    EXPECT_FALSE(rt->isTypedArray(arrayObj));
  }
}

TEST_P(HermesRuntimeTest, IsUint8ArrayTest) {
  // Test that isUint8Array returns false for a regular object
  {
    auto obj = Object(*rt);
    EXPECT_FALSE(rt->isUint8Array(obj));
    EXPECT_THROW(obj.asUint8Array(*rt), JSIException);
  }

  // Test that isUint8Array returns true for a Uint8Array created via JS
  {
    auto uint8ArrayObj = eval("new Uint8Array(10)").getObject(*rt);
    EXPECT_TRUE(rt->isUint8Array(uint8ArrayObj));

    // getUint8Array should succeed.
    auto uint8Array = uint8ArrayObj.getUint8Array(*rt);
    EXPECT_EQ(uint8Array.length(*rt), 10);
  }
}

#ifdef JSI_UNSTABLE
class HermesSerializationTest : public HermesRuntimeTest {
 public:
  HermesSerializationTest() : HermesRuntimeTest() {
    serializationInterface = castInterface<ISerialization>(rt.get());

    auto *api = castInterface<IHermesRootAPI>(makeHermesRootAPI());
    rt2 = api->makeHermesRuntime(hermes::vm::RuntimeConfig());
    serializationInterface2 = castInterface<ISerialization>(rt2.get());
  }

  // Evaluate the given code and serialize its results
  std::shared_ptr<Serialized> evalAndSerialize(const char *code) {
    Value evalRes = eval(code);
    return serializationInterface->serialize(evalRes);
  }

  // Deserialize the given Serialized object as a jsi::Object
  Object deserializeAsObject(const std::shared_ptr<Serialized> &serialized) {
    return serializationInterface->deserialize(serialized).getObject(*rt);
  }

  std::unique_ptr<Runtime> rt2;
  ISerialization *serializationInterface;
  ISerialization *serializationInterface2;
};

TEST_P(HermesSerializationTest, SerializePrimitives) {
  auto serialized = evalAndSerialize("undefined");
  auto deserialized = serializationInterface->deserialize(serialized);
  EXPECT_TRUE(deserialized.isUndefined());

  serialized = evalAndSerialize("null");
  deserialized = serializationInterface->deserialize(serialized);
  EXPECT_TRUE(deserialized.isNull());

  serialized = evalAndSerialize("true");
  deserialized = serializationInterface->deserialize(serialized);
  EXPECT_TRUE(deserialized.getBool());

  serialized = evalAndSerialize("false");
  deserialized = serializationInterface->deserialize(serialized);
  EXPECT_FALSE(deserialized.getBool());

  serialized = evalAndSerialize("100.99");
  deserialized = serializationInterface->deserialize(serialized);
  EXPECT_EQ(deserialized.getNumber(), 100.99);

  serialized = evalAndSerialize("18446744073709551615n");
  deserialized = serializationInterface->deserialize(serialized);
  EXPECT_EQ(
      deserialized.getBigInt(*rt).toString(*rt).utf8(*rt),
      "18446744073709551615");

  serialized = evalAndSerialize("'this is an ascii string!'");
  deserialized = serializationInterface->deserialize(serialized);
  EXPECT_EQ(deserialized.getString(*rt).utf8(*rt), "this is an ascii string!");

  serialized = evalAndSerialize("'this is a utf16 string!\\ud83d\\udc4d'");
  deserialized = serializationInterface->deserialize(serialized);
  EXPECT_EQ(
      deserialized.getString(*rt).utf16(*rt),
      u"this is a utf16 string!\xd83d\xdc4d");

  auto sym = eval("Symbol('foo')");
  EXPECT_THROW(serializationInterface->serialize(sym), JSError);
}

TEST_P(HermesSerializationTest, SerializeBigInt) {
  auto serialized = evalAndSerialize("[18446744073709551615n, new Object(7n)]");
  auto arr = deserializeAsObject(serialized).getArray(*rt);
  ASSERT_EQ(arr.size(*rt), 2);
  EXPECT_EQ(
      arr.getValueAtIndex(*rt, 0).getBigInt(*rt).toString(*rt).utf8(*rt),
      "18446744073709551615");

  auto bigIntObj = arr.getValueAtIndex(*rt, 1).getObject(*rt);
  EXPECT_EQ(
      bigIntObj.getPropertyAsFunction(*rt, "valueOf")
          .callWithThis(*rt, bigIntObj)
          .getBigInt(*rt)
          .toString(*rt)
          .utf8(*rt),
      "7");
}

TEST_P(HermesSerializationTest, SerializeSimpleObjectTypes) {
  auto serialized = evalAndSerialize("new Boolean(true)");
  auto deserializedObj = deserializeAsObject(serialized);
  auto result = deserializedObj.getPropertyAsFunction(*rt, "valueOf")
                    .callWithThis(*rt, deserializedObj);
  EXPECT_TRUE(result.getBool());

  serialized = evalAndSerialize("new Boolean(false)");
  deserializedObj = deserializeAsObject(serialized);
  result = deserializedObj.getPropertyAsFunction(*rt, "valueOf")
               .callWithThis(*rt, deserializedObj);
  EXPECT_FALSE(result.getBool());

  serialized = evalAndSerialize("new Number(123.456)");
  deserializedObj = deserializeAsObject(serialized);
  result = deserializedObj.getPropertyAsFunction(*rt, "valueOf")
               .callWithThis(*rt, deserializedObj);
  EXPECT_EQ(result.getNumber(), 123.456);

  serialized = evalAndSerialize("new Number('foobar')");
  deserializedObj = deserializeAsObject(serialized);
  result = deserializedObj.getPropertyAsFunction(*rt, "valueOf")
               .callWithThis(*rt, deserializedObj);
  EXPECT_TRUE(std::isnan(result.getNumber()));

  serialized = evalAndSerialize("Object(18446744073709551615n)");
  deserializedObj = deserializeAsObject(serialized);
  result = deserializedObj.getPropertyAsFunction(*rt, "toString")
               .callWithThis(*rt, deserializedObj);
  EXPECT_EQ(result.getString(*rt).utf8(*rt), "18446744073709551615");

  serialized = evalAndSerialize("new String('this is an ascii string!')");
  deserializedObj = deserializeAsObject(serialized);
  result = deserializedObj.getPropertyAsFunction(*rt, "valueOf")
               .callWithThis(*rt, deserializedObj);
  EXPECT_EQ(result.getString(*rt).utf8(*rt), "this is an ascii string!");

  serialized =
      evalAndSerialize("new String('this is a utf16 string!\\ud83d\\udc4d')");
  deserializedObj = deserializeAsObject(serialized);
  result = deserializedObj.getPropertyAsFunction(*rt, "valueOf")
               .callWithThis(*rt, deserializedObj);
  EXPECT_EQ(
      result.getString(*rt).utf16(*rt), u"this is a utf16 string!\xd83d\xdc4d");

  serialized = evalAndSerialize("new Date(2025, 5, 4)");
  deserializedObj = deserializeAsObject(serialized);
  result = deserializedObj.getPropertyAsFunction(*rt, "toDateString")
               .callWithThis(*rt, deserializedObj);
  EXPECT_EQ(result.getString(*rt).utf8(*rt), "Wed Jun 04 2025");
}

TEST_P(HermesSerializationTest, SerializeRegExp) {
  // This will search for the word 'salty'. the 'i' flag indicate an
  // case-insensitive search
  auto exp = "/salty/i";
  auto serialized = evalAndSerialize(exp);
  auto deserializedObj = deserializeAsObject(serialized);

  auto testFunc = deserializedObj.getPropertyAsFunction(*rt, "test");
  auto result =
      testFunc.callWithThis(*rt, deserializedObj, "My cat's name is Salty.");

  EXPECT_TRUE(result.getBool());

  // Using named capture groups
  exp = "/(?<word>Foobar)/i";
  serialized = evalAndSerialize(exp);
  deserializedObj = deserializeAsObject(serialized);
  auto execFunc = deserializedObj.getPropertyAsFunction(*rt, "exec");
  auto testStr = "When I write tests, my go-to word is foobar";
  result = execFunc.callWithThis(*rt, deserializedObj, testStr);
  auto resultGroup = result.getObject(*rt).getPropertyAsObject(*rt, "groups");
  auto word = resultGroup.getProperty(*rt, "word");
  EXPECT_EQ(word.getString(*rt).utf8(*rt), "foobar");
}

TEST_P(HermesSerializationTest, SerializeObject) {
  // A simple object with 2 properties
  auto code = R"(
var obj = {1:2, foo: 'bar'};
obj;
)";
  auto serialized = evalAndSerialize(code);
  auto deserializedObj = deserializeAsObject(serialized);
  auto resultValue = deserializedObj.getProperty(*rt, "1");
  EXPECT_EQ(resultValue.getNumber(), 2);
  resultValue = deserializedObj.getProperty(*rt, "foo");
  EXPECT_EQ(resultValue.getString(*rt).utf8(*rt), "bar");

  // An object that uses the same string
  code = R"(
var obj = {foo:'bar', foo2: 'bar'};
obj;
 )";
  serialized = evalAndSerialize(code);
  deserializedObj = deserializeAsObject(serialized);
  resultValue = deserializedObj.getProperty(*rt, "foo");
  EXPECT_EQ(resultValue.getString(*rt).utf8(*rt), "bar");
  resultValue = deserializedObj.getProperty(*rt, "foo2");
  EXPECT_EQ(resultValue.getString(*rt).utf8(*rt), "bar");

  // An object that uses symbol for property key. Symbol should not be
  // serialized.
  auto sym = eval("var fooSym = Symbol('foo'); fooSym;").asSymbol(*rt);
  code = R"(
var obj = {1:2};
obj[fooSym] = 'baz';
obj;
)";
  serialized = evalAndSerialize(code);
  deserializedObj = deserializeAsObject(serialized);
  // Check the symbol is on the original object
  auto originalObj = rt->global().getPropertyAsObject(*rt, "obj");
  EXPECT_TRUE(originalObj.hasProperty(*rt, PropNameID::forSymbol(*rt, sym)));
  // Check the symbol is not on the cloned object
  resultValue = deserializedObj.getProperty(*rt, "1");
  EXPECT_EQ(resultValue.getNumber(), 2);
  EXPECT_FALSE(
      deserializedObj.hasProperty(*rt, PropNameID::forSymbol(*rt, sym)));

  // An object that contains another object as a property
  code = R"(
  var innerObj = {1: 2};
  var outerObj = {inner: innerObj, 3: 4};
  outerObj;
)";
  serialized = evalAndSerialize(code);
  deserializedObj = deserializeAsObject(serialized);
  resultValue = deserializedObj.getProperty(*rt, "3");
  EXPECT_EQ(resultValue.getNumber(), 4);
  auto innerObj = deserializedObj.getPropertyAsObject(*rt, "inner");
  resultValue = innerObj.getProperty(*rt, "1");
  EXPECT_EQ(resultValue.getNumber(), 2);

  // Referencing an object multiple times
  code = R"(
var innerObj = {1: 2};
var outerObj = {
    firstRef: innerObj,
    3: 4,
    nestedObj: {
        5: 6,
        secondRef: innerObj
    }
};
outerObj;
)";
  serialized = evalAndSerialize(code);
  deserializedObj = deserializeAsObject(serialized);
  resultValue = deserializedObj.getProperty(*rt, "3");
  EXPECT_EQ(resultValue.getNumber(), 4);
  innerObj = deserializedObj.getPropertyAsObject(*rt, "firstRef");
  resultValue = innerObj.getProperty(*rt, "1");
  EXPECT_EQ(resultValue.getNumber(), 2);
  auto nestedObj = deserializedObj.getPropertyAsObject(*rt, "nestedObj");
  resultValue = nestedObj.getProperty(*rt, "5");
  EXPECT_EQ(resultValue.getNumber(), 6);
  auto secondRef = nestedObj.getPropertyAsObject(*rt, "secondRef");
  EXPECT_TRUE(Object::strictEquals(*rt, innerObj, secondRef));

  // An object that points to itself in one of the properties
  code = R"(
var bar = 'bar';
var obj = {
    1: 2,
    foo: bar,
    foo2: bar
};
obj.myself = obj;
obj;
)";
  serialized = evalAndSerialize(code);
  deserializedObj = deserializeAsObject(serialized);
  resultValue = deserializedObj.getProperty(*rt, "1");
  EXPECT_EQ(resultValue.getNumber(), 2);
  innerObj = deserializedObj.getPropertyAsObject(*rt, "myself");
  resultValue = innerObj.getProperty(*rt, "1");
  EXPECT_EQ(resultValue.getNumber(), 2);
  // Changing the "inner" myself object should also be reflected at the high
  // level object
  innerObj.setProperty(*rt, "foo", "bar");
  resultValue = deserializedObj.getProperty(*rt, "foo");
  EXPECT_EQ(resultValue.getString(*rt).utf8(*rt), "bar");
}

TEST_P(HermesSerializationTest, SeriaalizeObjectNonEnumerableProperties) {
  auto code = R"(
var obj = {
  foo: "bar",
  apple: "banana"
};
Object.defineProperty(obj, "salt", {
  value: "pepper",
  enumerable: false
});
obj;
)";

  auto serialized = evalAndSerialize(code);
  auto deserializedObj = deserializeAsObject(serialized);
  auto hasPropertyRes = deserializedObj.hasProperty(*rt, "foo");
  EXPECT_TRUE(hasPropertyRes);
  hasPropertyRes = deserializedObj.hasProperty(*rt, "apple");
  EXPECT_TRUE(hasPropertyRes);

  // 'salt' is a non-enumerable property key on the original object, so it
  // should not be cloned in the new copy
  hasPropertyRes = deserializedObj.hasProperty(*rt, "salt");
  EXPECT_FALSE(hasPropertyRes);
}

TEST_P(HermesSerializationTest, SerializeObjectRunJavaScript) {
  // When serializing `obj`, the `foo` getter will be invoked, deleting the
  // property `salt` from the object and adding the property `hello`. However,
  // the cloned object should only contain `foo`.
  auto code = R"(
var obj = {
    get foo() {
        delete this.salt;
        this.hello = 'world';
        return 'bar';
    },
    salt: 'pepper'
};
obj;
)";

  auto serialized = evalAndSerialize(code);
  auto deserializedObj = deserializeAsObject(serialized);

  // First, check that the property 'salt' was deleted and 'hello' was added
  // to the object in the original runtime.
  auto originalObj = rt->global().getPropertyAsObject(*rt, "obj");
  auto foo = originalObj.getProperty(*rt, "foo");
  EXPECT_EQ(foo.getString(*rt).utf8(*rt), "bar");
  auto hasSalt = originalObj.hasProperty(*rt, "salt");
  EXPECT_FALSE(hasSalt);
  auto hello = originalObj.getProperty(*rt, "hello");
  EXPECT_EQ(hello.getString(*rt).utf8(*rt), "world");

  /// Check that the deserialized object only has the 'foo' property. 'salt'
  /// was deleted by the foo getter, so it was never serialized, and the
  /// algorithm shouldn't serialize any new properties added by getter.
  foo = deserializedObj.getProperty(*rt, "foo");
  EXPECT_EQ(foo.getString(*rt).utf8(*rt), "bar");
  hasSalt = deserializedObj.hasProperty(*rt, "salt");
  EXPECT_FALSE(hasSalt);
  auto hasHello = deserializedObj.hasProperty(*rt, "hello");
  EXPECT_FALSE(hasHello);
}

TEST_P(HermesSerializationTest, SerializeArrayBuffer) {
  auto code = R"(
var buffer = new ArrayBuffer(32);
buffer;
)";
  auto serialized = evalAndSerialize(code);
  auto deserializedObj = deserializeAsObject(serialized);

  auto byteLen = deserializedObj.getProperty(*rt, "byteLength");
  EXPECT_EQ(byteLen.getNumber(), 32);

  code = R"(
var buffer = new ArrayBuffer(0);
buffer;
)";
  serialized = evalAndSerialize(code);
  deserializedObj = deserializeAsObject(serialized);

  byteLen = deserializedObj.getProperty(*rt, "byteLength");
  EXPECT_EQ(byteLen.getNumber(), 0);

  // Detached ArrayBuffers are not serializable
  code = R"(
var buffer = new ArrayBuffer(8);
HermesInternal.detachArrayBuffer(buffer);
buffer;
)";
  auto detachedBuffer = eval(code);
  EXPECT_THROW(serializationInterface->serialize(detachedBuffer), JSError);
}

TEST_P(HermesSerializationTest, SerializeDataView) {
  auto code = R"(
var buffer = new ArrayBuffer(32);
var view = new DataView(buffer, 16, 4);
view.setInt16(0, -123);
view.setInt16(2, 456);
view;
)";

  auto serialized = evalAndSerialize(code);
  auto deserializedObj = deserializeAsObject(serialized);

  auto byteLen = deserializedObj.getProperty(*rt, "byteLength").getNumber();
  EXPECT_EQ(byteLen, 4);
  auto byteOffset = deserializedObj.getProperty(*rt, "byteOffset").getNumber();
  EXPECT_EQ(byteOffset, 16);
  auto getInt16Fn = deserializedObj.getPropertyAsFunction(*rt, "getInt16");
  auto getRes = getInt16Fn.callWithThis(*rt, deserializedObj, 0).getNumber();
  EXPECT_EQ(getRes, -123);
  getRes = getInt16Fn.callWithThis(*rt, deserializedObj, 2).getNumber();
  EXPECT_EQ(getRes, 456);

  // Verify that the underlying buffer was copied over completely
  auto buffer = deserializedObj.getPropertyAsObject(*rt, "buffer");
  byteLen = buffer.getProperty(*rt, "byteLength").getNumber();
  EXPECT_EQ(byteLen, 32);

  // The underlying ArrayBuffer is detached
  code = R"(
var buffer = new ArrayBuffer(32);
var view = new DataView(buffer, 16, 4);
HermesInternal.detachArrayBuffer(buffer);
view;
)";
  auto viewWithDetachedBuffer = eval(code);
  EXPECT_THROW(
      serializationInterface->serialize(viewWithDetachedBuffer), JSError);
}

TEST_P(HermesSerializationTest, VerifyTypeSerializedTypeArray) {
  // This test simply checks that we correctly serialize the type of the Array.
  // It does not verify the content of the array.
#define TYPED_ARRAY(name, type)                                  \
  {                                                              \
    auto code = "new " #name "Array();";                         \
    auto serialized = evalAndSerialize(code);                    \
    auto deserializedArr = deserializeAsObject(serialized);      \
    auto constructor =                                           \
        deserializedArr.getPropertyAsObject(*rt, "constructor"); \
    auto name = constructor.getProperty(*rt, "name");            \
    EXPECT_EQ(name.getString(*rt).utf8(*rt), #name "Array");     \
  }
#include "hermes/VM/TypedArrays.def"
} // namespace

TEST_P(HermesSerializationTest, SerializeTypedArray) {
  auto code = R"(
var buffer = new ArrayBuffer(32);
var float64arr = new Float64Array(buffer);
float64arr[0] = 12.34;
float64arr[1] = 34.56;
float64arr[2] = 56.78;
float64arr[3] = 78.90;

float64arr;
)";

  auto serialized = evalAndSerialize(code);
  auto deserializedObj = deserializeAsObject(serialized);

  // Verify that the underlying buffer was copied over completely
  auto buffer = deserializedObj.getPropertyAsObject(*rt, "buffer");
  auto byteLen = buffer.getProperty(*rt, "byteLength").getNumber();
  EXPECT_EQ(byteLen, 32);

  auto bytesPerElem = deserializedObj.getProperty(*rt, "BYTES_PER_ELEMENT");
  EXPECT_EQ(bytesPerElem.getNumber(), 8);
  byteLen = deserializedObj.getProperty(*rt, "byteLength").getNumber();
  EXPECT_EQ(byteLen, 32);
  auto byteOffset = deserializedObj.getProperty(*rt, "byteOffset").getNumber();
  EXPECT_EQ(byteOffset, 0);
  auto len = deserializedObj.getProperty(*rt, "length").getNumber();
  EXPECT_EQ(len, 4);

  auto atFn = deserializedObj.getPropertyAsFunction(*rt, "at");
  auto res = atFn.callWithThis(*rt, deserializedObj, 0).getNumber();
  EXPECT_EQ(res, 12.34);
  res = atFn.callWithThis(*rt, deserializedObj, 1).getNumber();
  EXPECT_EQ(res, 34.56);
  res = atFn.callWithThis(*rt, deserializedObj, 2).getNumber();
  EXPECT_EQ(res, 56.78);
  res = atFn.callWithThis(*rt, deserializedObj, 3).getNumber();
  EXPECT_EQ(res, 78.90);

  code = R"(
var uint32arr = new Int32Array(buffer, 4, 5);
uint32arr[0] = 100;
uint32arr[1] = 200;
uint32arr[2] = 300;
uint32arr[3] = 400;
uint32arr[4] = 500;

uint32arr;
)";

  serialized = evalAndSerialize(code);
  deserializedObj = deserializeAsObject(serialized);

  bytesPerElem = deserializedObj.getProperty(*rt, "BYTES_PER_ELEMENT");
  EXPECT_EQ(bytesPerElem.getNumber(), 4);
  byteLen = deserializedObj.getProperty(*rt, "byteLength").getNumber();
  EXPECT_EQ(byteLen, 20);
  byteOffset = deserializedObj.getProperty(*rt, "byteOffset").getNumber();
  EXPECT_EQ(byteOffset, 4);
  len = deserializedObj.getProperty(*rt, "length").getNumber();
  EXPECT_EQ(len, 5);

  atFn = deserializedObj.getPropertyAsFunction(*rt, "at");
  bytesPerElem = deserializedObj.getProperty(*rt, "BYTES_PER_ELEMENT");
  EXPECT_EQ(bytesPerElem.getNumber(), 4);
  res = atFn.callWithThis(*rt, deserializedObj, 0).getNumber();
  EXPECT_EQ(res, 100);
  res = atFn.callWithThis(*rt, deserializedObj, 1).getNumber();
  EXPECT_EQ(res, 200);
  res = atFn.callWithThis(*rt, deserializedObj, 2).getNumber();
  EXPECT_EQ(res, 300);
  res = atFn.callWithThis(*rt, deserializedObj, 3).getNumber();
  EXPECT_EQ(res, 400);
  res = atFn.callWithThis(*rt, deserializedObj, 4).getNumber();
  EXPECT_EQ(res, 500);

  // The array is empty
  code = R"(
var arr = new Int32Array(0);
arr;
)";
  serialized = evalAndSerialize(code);
  deserializedObj = deserializeAsObject(serialized);
  bytesPerElem = deserializedObj.getProperty(*rt, "BYTES_PER_ELEMENT");
  EXPECT_EQ(bytesPerElem.getNumber(), 4);
  byteLen = deserializedObj.getProperty(*rt, "byteLength").getNumber();
  EXPECT_EQ(byteLen, 0);
  byteOffset = deserializedObj.getProperty(*rt, "byteOffset").getNumber();
  EXPECT_EQ(byteOffset, 0);
  len = deserializedObj.getProperty(*rt, "length").getNumber();
  EXPECT_EQ(len, 0);

  // The underlying ArrayBuffer is detached
  code = R"(
var buffer = new ArrayBuffer(8);
var arr = new Int8Array(buffer);
HermesInternal.detachArrayBuffer(buffer);
arr;
)";
  auto arrWithDetachedBuffer = eval(code);
  EXPECT_THROW(
      serializationInterface->serialize(arrWithDetachedBuffer), JSError);
}

TEST_P(HermesSerializationTest, SerializeMap) {
  auto code = R"(
var m = new Map();
m.set('salt', 'pepper');
m.set('obj', {1: 2});
m;
)";

  auto serialized = evalAndSerialize(code);
  auto deserializedObj = deserializeAsObject(serialized);
  auto getFunc = deserializedObj.getPropertyAsFunction(*rt, "get");
  auto size = deserializedObj.getProperty(*rt, "size").getNumber();
  EXPECT_EQ(size, 2);

  auto result = getFunc.callWithThis(
      *rt, deserializedObj, String::createFromUtf8(*rt, "salt"));
  EXPECT_EQ(result.getString(*rt).utf8(*rt), "pepper");

  result = getFunc.callWithThis(
      *rt, deserializedObj, String::createFromUtf8(*rt, "obj"));
  EXPECT_EQ(result.getObject(*rt).getProperty(*rt, "1").getNumber(), 2);

  // An empty map
  serialized = evalAndSerialize("new Map();");
  deserializedObj = deserializeAsObject(serialized);
  size = deserializedObj.getProperty(*rt, "size").getNumber();
  EXPECT_EQ(size, 0);
}

TEST_P(HermesSerializationTest, SerializeMapRunJavaScript) {
  // When serializing the map, `obj` will also be serialized, the foo getter
  // will be invoked, removing the key `2` from the original map. However,
  // serialized map should still contain the key-value (2, 3)
  auto code = R"(
var map = new Map();
var obj = {
    get foo() {
        map.delete(2);
        return 'bar';
    }
};
map.set(1, obj);
map.set(2, 3);
map;
)";

  auto serialized = evalAndSerialize(code);
  auto mapCloned = deserializeAsObject(serialized);

  // Check the original map is modified by the `foo` getter in `obj`
  auto mapOriginal = rt->global().getPropertyAsObject(*rt, "map");
  auto hasFn = mapOriginal.getPropertyAsFunction(*rt, "has");
  auto result = hasFn.callWithThis(*rt, mapOriginal, 1);
  EXPECT_TRUE(result.getBool());
  result = hasFn.callWithThis(*rt, mapOriginal, 2);
  EXPECT_FALSE(result.getBool());

  // Check the cloned map has cloned both (1, obj) and (2, 3)
  auto getFn = mapCloned.getPropertyAsFunction(*rt, "get");
  result = getFn.callWithThis(*rt, mapCloned, 1);
  auto objCloned = result.getObject(*rt);
  auto foo = objCloned.getProperty(*rt, "foo");
  EXPECT_EQ(foo.getString(*rt).utf8(*rt), "bar");

  result = getFn.callWithThis(*rt, mapCloned, 2);
  EXPECT_EQ(result.getNumber(), 3);
}

TEST_P(HermesSerializationTest, SerializeSet) {
  auto code = R"(
var s = new Set(['salt', 10]);
s;
)";

  auto serialized = evalAndSerialize(code);
  auto deserializedObj = deserializeAsObject(serialized);
  auto size = deserializedObj.getProperty(*rt, "size").getNumber();
  EXPECT_EQ(size, 2);

  auto hasFunc = deserializedObj.getPropertyAsFunction(*rt, "has");
  auto result = hasFunc.callWithThis(
      *rt, deserializedObj, String::createFromUtf8(*rt, "salt"));
  EXPECT_TRUE(result.getBool());

  result = hasFunc.callWithThis(*rt, deserializedObj, Value(10));
  EXPECT_TRUE(result.getBool());

  result = hasFunc.callWithThis(
      *rt, deserializedObj, String::createFromUtf8(*rt, "pepper"));
  EXPECT_FALSE(result.getBool());

  // An empty set
  serialized = evalAndSerialize("new Set();");
  deserializedObj = deserializeAsObject(serialized);
  size = deserializedObj.getProperty(*rt, "size").getNumber();
  EXPECT_EQ(size, 0);
}

TEST_P(HermesSerializationTest, SerializeSetRunJavaScript) {
  // When serializing the set, `obj` will be serialized first and the foo
  // getter will be invoked. The getter will remove the `2` from the original
  // set. However, serialized set should contain both `obj` and `2`.
  auto code = R"(
var set = new Set();
var obj = {
    get foo() {
        set.delete('foobar');
        return 'bar';
    }
};
set.add(obj);
set.add('foobar');
var arr = [set, obj];
arr;
)";

  auto serialized = evalAndSerialize(code);
  auto arrCloned = deserializeAsObject(serialized).getArray(*rt);

  // Check the original set is modified by the `foo` getter in `obj`
  auto setOriginal = rt->global().getPropertyAsObject(*rt, "set");
  auto obj = rt->global().getPropertyAsObject(*rt, "obj");
  auto hasFn = setOriginal.getPropertyAsFunction(*rt, "has");
  auto result = hasFn.callWithThis(*rt, setOriginal, "foobar");
  EXPECT_FALSE(result.getBool());
  result = hasFn.callWithThis(*rt, setOriginal, obj);
  EXPECT_TRUE(result.getBool());

  // Check the cloned set has both obj and "foobar"
  auto setCloned = arrCloned.getValueAtIndex(*rt, 0).getObject(*rt);
  auto objCloned = arrCloned.getValueAtIndex(*rt, 1).getObject(*rt);
  result = hasFn.callWithThis(*rt, setCloned, "foobar");
  EXPECT_TRUE(result.getBool());
  result = hasFn.callWithThis(*rt, setCloned, objCloned);
  EXPECT_TRUE(result.getBool());

  auto foo = objCloned.getProperty(*rt, "foo");
  EXPECT_EQ(foo.getString(*rt).utf8(*rt), "bar");
}

TEST_P(HermesSerializationTest, SerializeError) {
  auto serialized = evalAndSerialize("new Error('some generic error');");
  auto deserializedObj = deserializeAsObject(serialized);

  auto errorName = deserializedObj.getProperty(*rt, "name").getString(*rt);
  EXPECT_EQ(errorName.utf8(*rt), "Error");

  auto msg = deserializedObj.getProperty(*rt, "message").getString(*rt);
  EXPECT_EQ(msg.utf8(*rt), "some generic error");

  serialized = evalAndSerialize("new TypeError('some type error');");
  deserializedObj = deserializeAsObject(serialized);

  errorName = deserializedObj.getProperty(*rt, "name").getString(*rt);
  EXPECT_EQ(errorName.utf8(*rt), "TypeError");

  msg = deserializedObj.getProperty(*rt, "message").getString(*rt);
  EXPECT_EQ(msg.utf8(*rt), "some type error");

  serialized = evalAndSerialize("new ReferenceError(undefined);");
  deserializedObj = deserializeAsObject(serialized);

  errorName = deserializedObj.getProperty(*rt, "name").getString(*rt);
  EXPECT_EQ(errorName.utf8(*rt), "ReferenceError");

  auto hasOwnFn = deserializedObj.getPropertyAsFunction(*rt, "hasOwnProperty");
  auto hasOwnRes = hasOwnFn.callWithThis(*rt, deserializedObj, "message");
  EXPECT_FALSE(hasOwnRes.getBool());

  auto code = R"(
var err = new SyntaxError();
err.message = {
    toString: function() {
        return 'some syntax error';
    }
};
err;
)";
  serialized = evalAndSerialize(code);
  deserializedObj = deserializeAsObject(serialized);

  errorName = deserializedObj.getProperty(*rt, "name").getString(*rt);
  EXPECT_EQ(errorName.utf8(*rt), "SyntaxError");
  msg = deserializedObj.getProperty(*rt, "message").getString(*rt);
  EXPECT_EQ(msg.utf8(*rt), "some syntax error");
}

TEST_P(HermesSerializationTest, SerializeArray) {
  // Simple array
  auto code = R"(
var arr = ['a', 'b', 'c'];
arr;
)";
  auto serialized = evalAndSerialize(code);
  auto deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  auto arrLen = deserializedArr.length(*rt);
  EXPECT_EQ(arrLen, 3);
  auto result = deserializedArr.getValueAtIndex(*rt, 1);
  EXPECT_EQ(result.getString(*rt).utf8(*rt), "b");

  // Array with objects
  code = R"(
var foo = {salt: 'pepper'};
var bar = [100, 99, 'apple', foo];
bar;
)";
  serialized = evalAndSerialize(code);
  deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  arrLen = deserializedArr.length(*rt);
  EXPECT_EQ(arrLen, 4);
  result = deserializedArr.getValueAtIndex(*rt, 1);
  EXPECT_EQ(result.getNumber(), 99);

  result = deserializedArr.getValueAtIndex(*rt, 2);
  EXPECT_EQ(result.getString(*rt).utf8(*rt), "apple");

  auto fooObj = deserializedArr.getValueAtIndex(*rt, 3).getObject(*rt);
  result = fooObj.getProperty(*rt, "salt");
  EXPECT_EQ(result.getString(*rt).utf8(*rt), "pepper");

  // Sparse Array
  code = R"(
var arr = [1,,3,,5];
arr;
)";
  serialized = evalAndSerialize(code);
  deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  arrLen = deserializedArr.length(*rt);
  EXPECT_EQ(arrLen, 5);
  result = deserializedArr.getValueAtIndex(*rt, 0);
  EXPECT_EQ(result.getNumber(), 1);
  result = deserializedArr.getValueAtIndex(*rt, 1);
  EXPECT_TRUE(result.isUndefined());
  result = deserializedArr.getValueAtIndex(*rt, 2);
  EXPECT_EQ(result.getNumber(), 3);
  result = deserializedArr.getValueAtIndex(*rt, 3);
  EXPECT_TRUE(result.isUndefined());
  result = deserializedArr.getValueAtIndex(*rt, 4);
  EXPECT_EQ(result.getNumber(), 5);

  // Serializing the array will invoke the getter, which deletes the last
  // element from the array.
  code = R"(
var obj = {};
var arr = [1, obj, 3];
Object.defineProperty(obj, "foo", {
  enumerable: true,
  get: function() {
    arr.length = 2;
    return "bar"
  }
});
arr;
)";
  serialized = evalAndSerialize(code);
  deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  arrLen = deserializedArr.length(*rt);
  // Even though the original array was resized to 2, note that the serialize
  // algorithm store the length directly, and expects the length to be set when
  // deserializing.
  EXPECT_EQ(arrLen, 3);
  result = deserializedArr.getValueAtIndex(*rt, 0);
  EXPECT_EQ(result.getNumber(), 1);
  result = deserializedArr.getValueAtIndex(*rt, 1);
  auto resultObj = result.getObject(*rt);
  EXPECT_EQ(resultObj.getProperty(*rt, "foo").getString(*rt).utf8(*rt), "bar");
  result = deserializedArr.getValueAtIndex(*rt, 2);
  EXPECT_TRUE(result.isUndefined());
  // Verify the getter modified the original arr
  auto originalArr = rt->global().getPropertyAsObject(*rt, "arr").getArray(*rt);
  arrLen = originalArr.length(*rt);
  EXPECT_EQ(arrLen, 2);
}

TEST_P(HermesSerializationTest, SerializeArrayNamedProperty) {
  // Array with named property key "1"
  auto code = R"(
var arr = [100, 200];
Object.defineProperty(arr, 1, {
  value: 300,
  configurable: false,
  enumerable: true,
  writeable: true,
});
arr;
)";
  auto serialized = evalAndSerialize(code);
  auto deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  auto result = deserializedArr.getValueAtIndex(*rt, 1);
  auto arrLen = deserializedArr.length(*rt);
  EXPECT_EQ(arrLen, 2);
  // We should be getting the named property "1" which has the value 300, and
  // not the value originally in the JSArray indexed storage
  EXPECT_EQ(result.getNumber(), 300);

  code = R"(
var arr = [100, 200];
arr.foo = "bar";
arr;
)";
  serialized = evalAndSerialize(code);
  deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  arrLen = deserializedArr.length(*rt);
  EXPECT_EQ(arrLen, 2);
  result = deserializedArr.getValueAtIndex(*rt, 1);
  EXPECT_EQ(result.getNumber(), 200);

  // An array with a getter
  code = R"(
var arr = [1, 2, 3];
Object.defineProperty(arr, 1, {
  enumerable: true,
  configurable: true,
  get: function() {
    arr[2] = 200;
    arr[3] = 300;
    return 100
  }
});
arr;
)";
  serialized = evalAndSerialize(code);
  deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  arrLen = deserializedArr.length(*rt);
  EXPECT_EQ(arrLen, 3);
  result = deserializedArr.getValueAtIndex(*rt, 0);
  EXPECT_EQ(result.getNumber(), 1);
  // Getter returns 100, even if the array storage originally had stored 2
  result = deserializedArr.getValueAtIndex(*rt, 1);
  EXPECT_EQ(result.getNumber(), 100);
  // Getter effected element at index 2
  result = deserializedArr.getValueAtIndex(*rt, 2);
  EXPECT_EQ(result.getNumber(), 200);
  // Getter added the element at index 3 to the original array.
  auto originalArr = rt->global().getPropertyAsObject(*rt, "arr").getArray(*rt);
  result = originalArr.getValueAtIndex(*rt, 3);
  EXPECT_EQ(result.getNumber(), 300);
  // However, the cloning algorithm obtains a list of all property keys to
  // serialize up-front, and does not account for properties added as a result
  // of the getter's side effects.
  result = deserializedArr.hasProperty(*rt, 3);
  EXPECT_FALSE(result.getBool());

  // The array is eligible for fast path at first, but once the object element
  // is serialized, it makes the array ineligible for fast path
  code = R"(
var obj = {};
Object.defineProperty(obj, "foo", {
  enumerable: true,
  get: function() {
    Object.defineProperty(arr, 2, {
      enumerable: true,
      configurable: false,
      value: 200,
    });
    return "bar";
  }
});
var arr = [1, obj, 2];
arr;
)";
  serialized = evalAndSerialize(code);
  deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  result = deserializedArr.getValueAtIndex(*rt, 0);
  EXPECT_EQ(result.getNumber(), 1);
  result = deserializedArr.getValueAtIndex(*rt, 1);
  auto resultObj = result.getObject(*rt);
  EXPECT_EQ(resultObj.getProperty(*rt, "foo").getString(*rt).utf8(*rt), "bar");
  result = deserializedArr.getValueAtIndex(*rt, 2);
  EXPECT_EQ(result.getNumber(), 200);
  arrLen = deserializedArr.length(*rt);
  EXPECT_EQ(arrLen, 3);
}

TEST_P(HermesSerializationTest, SerializeManyValues) {
  // https://github.com/facebook/hermes/issues/2189: the number of properties,
  // elements and entries must not be bounded by the handle limit of a GCScope.
  // Errors are included because serializing and deserializing them creates
  // handles.
  auto code = R"(
var objs = [];
var dict = {};
var errors = [];
var map = new Map();
var set = new Set();
for (var i = 0; i < 100; ++i) {
  objs.push({albumId: 1, id: i, title: 'test', url: 'https://example.com'});
  dict['key' + i] = i;
  errors.push(new Error('error' + i));
  map.set(i, new Error('error' + i));
  set.add(new Error('error' + i));
}
[objs, dict, errors, map, set];
)";
  auto serialized = evalAndSerialize(code);
  auto deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  auto objs =
      deserializedArr.getValueAtIndex(*rt, 0).getObject(*rt).getArray(*rt);
  auto dict = deserializedArr.getValueAtIndex(*rt, 1).getObject(*rt);
  auto errors =
      deserializedArr.getValueAtIndex(*rt, 2).getObject(*rt).getArray(*rt);
  auto map = deserializedArr.getValueAtIndex(*rt, 3).getObject(*rt);
  auto set = deserializedArr.getValueAtIndex(*rt, 4).getObject(*rt);
  ASSERT_EQ(objs.length(*rt), 100);
  ASSERT_EQ(errors.length(*rt), 100);
  EXPECT_EQ(map.getProperty(*rt, "size").getNumber(), 100);
  EXPECT_EQ(set.getProperty(*rt, "size").getNumber(), 100);
  auto getFn = map.getPropertyAsFunction(*rt, "get");
  for (int i = 0; i < 100; ++i) {
    auto obj = objs.getValueAtIndex(*rt, i).getObject(*rt);
    EXPECT_EQ(obj.getProperty(*rt, "id").getNumber(), i);
    EXPECT_EQ(
        obj.getProperty(*rt, "url").getString(*rt).utf8(*rt),
        "https://example.com");
    auto key = "key" + std::to_string(i);
    EXPECT_EQ(dict.getProperty(*rt, key.c_str()).getNumber(), i);
    auto message = "error" + std::to_string(i);
    auto error = errors.getValueAtIndex(*rt, i).getObject(*rt);
    EXPECT_EQ(
        error.getProperty(*rt, "message").getString(*rt).utf8(*rt), message);
    auto mapValue = getFn.callWithThis(*rt, map, i).getObject(*rt);
    EXPECT_EQ(
        mapValue.getProperty(*rt, "message").getString(*rt).utf8(*rt), message);
  }
}

TEST_P(HermesSerializationTest, SerializeManyValuesRunJavaScript) {
  // The getters delete the remaining properties and elements while the
  // values are being serialized, so they are skipped.
  auto code = R"(
var obj = {
  get first() {
    for (var i = 0; i < 100; ++i) delete this['key' + i];
    return 'first';
  },
};
var arr = [];
for (var i = 0; i < 100; ++i) {
  obj['key' + i] = i;
  arr.push(i);
}
Object.defineProperty(arr, 0, {
  enumerable: true,
  get: function () {
    arr.length = 1;
    return 'first';
  },
});
[obj, arr];
)";
  auto serialized = evalAndSerialize(code);
  auto deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  auto obj = deserializedArr.getValueAtIndex(*rt, 0).getObject(*rt);
  auto arr = deserializedArr.getValueAtIndex(*rt, 1).getObject(*rt);
  EXPECT_EQ(obj.getPropertyNames(*rt).size(*rt), 1);
  EXPECT_EQ(obj.getProperty(*rt, "first").getString(*rt).utf8(*rt), "first");
  EXPECT_EQ(arr.getProperty(*rt, "0").getString(*rt).utf8(*rt), "first");
  EXPECT_FALSE(arr.hasProperty(*rt, "1"));
}

TEST_P(HermesSerializationTest, SerializeDeeplyNestedValues) {
  // Neither should the nesting depth be bounded by the handle limit.
  auto code = R"(
function slowPair(first, second) {
  var pair = [first, second];
  // The accessor makes the elements of pair take the slow path.
  Object.defineProperty(pair, 0, {enumerable: true, get: () => first});
  return pair;
}
var obj = null;
var arr = null;
var named = null;
for (var i = 0; i < 100; ++i) {
  obj = {depth: i, child: obj};
  arr = slowPair(i, arr);
  // An array without elements, nested through a named property.
  var wrapper = [];
  wrapper.child = named;
  named = wrapper;
}
[obj, arr, named];
)";
  auto serialized = evalAndSerialize(code);
  auto deserializedArr = deserializeAsObject(serialized).getArray(*rt);
  auto obj = deserializedArr.getValueAtIndex(*rt, 0);
  auto arr = deserializedArr.getValueAtIndex(*rt, 1);
  auto named = deserializedArr.getValueAtIndex(*rt, 2);
  for (int i = 99; i >= 0; --i) {
    EXPECT_EQ(obj.getObject(*rt).getProperty(*rt, "depth").getNumber(), i);
    obj = obj.getObject(*rt).getProperty(*rt, "child");
    auto elems = arr.getObject(*rt).getArray(*rt);
    EXPECT_EQ(elems.getValueAtIndex(*rt, 0).getNumber(), i);
    arr = elems.getValueAtIndex(*rt, 1);
    EXPECT_TRUE(named.getObject(*rt).isArray(*rt));
    named = named.getObject(*rt).getProperty(*rt, "child");
  }
  EXPECT_TRUE(obj.isNull());
  EXPECT_TRUE(arr.isNull());
  EXPECT_TRUE(named.isNull());
}

TEST_P(HermesSerializationTest, DeserializeDeeplyNestedValues) {
  // Deserializing a boxed string, or a one-character string outside Latin-1,
  // creates a handle. Such values must not make handles accumulate with the
  // nesting depth either.
  auto code = R"(
var map = null;
var obj = null;
var arr = null;
var keyMap = null;
for (var i = 0; i < 100; ++i) {
  map = new Map([[new String('key'), map]]);
  var o = {};
  o[String.fromCharCode(0x100 + i)] = obj;
  obj = o;
  var a = [new String('element')];
  a.child = arr;
  arr = a;
  // Nested through a key, after an entry whose value creates a handle.
  keyMap = new Map([['key', new String('value')], [keyMap, 'next']]);
}
[map, obj, arr, keyMap];
)";
  auto serialized = evalAndSerialize(code);
  auto deserialized = deserializeAsObject(serialized);
  auto countLevels = eval(R"((function (values) {
  function count(value, next) {
    var levels = 0;
    for (; value !== null; value = next(value)) ++levels;
    return levels;
  }
  return [
    count(values[0], m => m.values().next().value),
    count(values[1], o => o[Object.keys(o)[0]]),
    count(values[2], a => a.child),
    count(values[3], m => Array.from(m.keys())[1]),
  ].join();
}))")
                         .getObject(*rt)
                         .getFunction(*rt);
  auto levels = countLevels.call(*rt, deserialized);
  EXPECT_EQ(levels.getString(*rt).utf8(*rt), "100,100,100,100");
}

TEST_P(HermesSerializationTest, SerializeUnsupported) {
  // Go through known unsupported values for serialization and make sure we
  // throw
  // Functions
  auto func = eval("(function testFunc() {})");
  EXPECT_THROW(serializationInterface->serialize(func), JSError);

  // Host Objects
  class TestHostObject : public HostObject {
    std::vector<PropNameID> getPropertyNames(Runtime &rt) override {
      return PropNameID::names(rt, "test");
    }

    Value get(Runtime &runtime, const PropNameID &name) override {
      return Value();
    }
  };
  Object hostObj =
      Object::createFromHostObject(*rt, std::make_shared<TestHostObject>());

  auto hostObjVal = Value(*rt, hostObj);
  EXPECT_THROW(serializationInterface->serialize(hostObjVal), JSError);

  auto proxy = eval(
      "var target = {};"
      "var handler = {};"
      "var proxy = new Proxy(target, handler); proxy;");

  EXPECT_THROW(serializationInterface->serialize(proxy), JSError);
}

TEST_P(HermesSerializationTest, SerializeTypedArrayInternalWithTransfer) {
  eval(
      "var bar = new Int8Array([-3, -1, 0, 2, 4]);"
      "var obj = {salt: 'pepper', foo: bar}");
  auto obj = rt->global().getProperty(*rt, "obj");
  auto bar = obj.getObject(*rt).getPropertyAsObject(*rt, "foo");
  auto buffer = bar.getPropertyAsObject(*rt, "buffer").getArrayBuffer(*rt);
  EXPECT_EQ(buffer.size(*rt), 5);

  Array transfers = Array::createWithElements(*rt, Value(*rt, buffer));
  auto serialized =
      serializationInterface->serializeWithTransfer(obj, transfers);

  // Test the original buffer has now been detached
  EXPECT_THROW(buffer.size(*rt), JSIException);

  auto deserialized =
      serializationInterface2->deserializeWithTransfer(serialized);
  // Check that the SerializedValue is consumed by deserialization process
  EXPECT_EQ(serialized, nullptr);

  EXPECT_EQ(deserialized.length(*rt2), 2);
  auto deserializeObj = deserialized.getValueAtIndex(*rt2, 0).asObject(*rt2);
  auto transferredObj = deserialized.getValueAtIndex(*rt2, 1).asObject(*rt2);

  auto pepperStr = deserializeObj.getProperty(*rt2, "salt").getString(*rt2);
  EXPECT_EQ(pepperStr.utf8(*rt2), "pepper");

  auto deserializedArr = deserializeObj.getPropertyAsObject(*rt2, "foo");
  auto deserializedBuffer =
      deserializedArr.getPropertyAsObject(*rt2, "buffer").getArrayBuffer(*rt);
  EXPECT_EQ(deserializedBuffer.size(*rt2), 5);

  auto atFn = deserializedArr.getPropertyAsFunction(*rt2, "at");
  auto bytesPerElem = deserializedArr.getProperty(*rt2, "BYTES_PER_ELEMENT");
  EXPECT_EQ(bytesPerElem.getNumber(), 1);
  auto res = atFn.callWithThis(*rt2, deserializedArr, 0).getNumber();
  EXPECT_EQ(res, -3);
  res = atFn.callWithThis(*rt2, deserializedArr, 1).getNumber();
  EXPECT_EQ(res, -1);
  res = atFn.callWithThis(*rt2, deserializedArr, 2).getNumber();
  EXPECT_EQ(res, 0);
  res = atFn.callWithThis(*rt2, deserializedArr, 3).getNumber();
  EXPECT_EQ(res, 2);
  res = atFn.callWithThis(*rt2, deserializedArr, 4).getNumber();
  EXPECT_EQ(res, 4);

  // Check that the transferred object is the same as obj.foo's buffer
  EXPECT_TRUE(Object::strictEquals(*rt2, transferredObj, deserializedBuffer));

  // Transfer an empty buffer
  obj = eval("new Int16Array();");
  buffer =
      obj.getObject(*rt).getPropertyAsObject(*rt, "buffer").getArrayBuffer(*rt);
  transfers = Array::createWithElements(*rt, Value(*rt, buffer));
  serialized = serializationInterface->serializeWithTransfer(obj, transfers);
  deserialized = serializationInterface2->deserializeWithTransfer(serialized);
  EXPECT_EQ(serialized, nullptr);

  EXPECT_EQ(deserialized.length(*rt2), 2);

  deserializeObj = deserialized.getValueAtIndex(*rt2, 0).asObject(*rt2);
  transferredObj = deserialized.getValueAtIndex(*rt2, 1).asObject(*rt2);
  deserializedBuffer =
      deserializeObj.getPropertyAsObject(*rt2, "buffer").getArrayBuffer(*rt2);

  EXPECT_EQ(deserializedBuffer.size(*rt2), 0);
  bytesPerElem = deserializeObj.getProperty(*rt2, "BYTES_PER_ELEMENT");
  EXPECT_EQ(bytesPerElem.getNumber(), 2);
  EXPECT_TRUE(Object::strictEquals(*rt2, transferredObj, deserializedBuffer));
}

TEST_P(HermesSerializationTest, SerializeTypedArrayExternalWithTransfer) {
  struct FixedBuffer : MutableBuffer {
    size_t size() const override {
      return sizeof(arr);
    }
    uint8_t *data() override {
      return reinterpret_cast<uint8_t *>(arr.data());
    }

    std::array<uint16_t, 4> arr;
  };

  auto buf = std::make_shared<FixedBuffer>();
  for (uint32_t i = 0; i < buf->arr.size(); i++) {
    buf->arr[i] = i * i;
  }
  auto arrayBuffer = ArrayBuffer(*rt, buf);
  auto makeFn = eval(
      "(function makeArray(buf) {"
      "var view = new Uint16Array(buf);"
      "return view;"
      "})");
  auto arr = makeFn.asObject(*rt).asFunction(*rt).call(*rt, arrayBuffer);
  auto buffer =
      arr.getObject(*rt).getPropertyAsObject(*rt, "buffer").getArrayBuffer(*rt);

  Array transfers = Array::createWithElements(*rt, Value(*rt, buffer));
  auto serialized =
      serializationInterface->serializeWithTransfer(arr, transfers);

  // Test the original buffer has now been detached
  EXPECT_THROW(buffer.size(*rt), JSIException);

  auto deserialized =
      serializationInterface2->deserializeWithTransfer(serialized);
  // Check that the SerializedValue is consumed by deserialization process
  EXPECT_EQ(serialized, nullptr);

  EXPECT_EQ(deserialized.length(*rt2), 2);
  auto deserializedArr = deserialized.getValueAtIndex(*rt2, 0).asObject(*rt2);
  auto transferredObj = deserialized.getValueAtIndex(*rt2, 1).asObject(*rt2);

  auto atFn = deserializedArr.getPropertyAsFunction(*rt2, "at");
  auto bytesPerElem = deserializedArr.getProperty(*rt2, "BYTES_PER_ELEMENT");
  EXPECT_EQ(bytesPerElem.getNumber(), 2);
  auto res = atFn.callWithThis(*rt2, deserializedArr, 0).getNumber();
  EXPECT_EQ(res, 0);
  res = atFn.callWithThis(*rt2, deserializedArr, 1).getNumber();
  EXPECT_EQ(res, 1);
  res = atFn.callWithThis(*rt2, deserializedArr, 2).getNumber();
  EXPECT_EQ(res, 4);
  res = atFn.callWithThis(*rt2, deserializedArr, 3).getNumber();
  EXPECT_EQ(res, 9);

  auto deserializedBuffer =
      deserializedArr.getPropertyAsObject(*rt2, "buffer").getArrayBuffer(*rt);
  EXPECT_EQ(deserializedBuffer.size(*rt2), 8);

  // Check that the transferred object is the deserialized typed array's
  // buffer.
  EXPECT_TRUE(Object::strictEquals(*rt2, transferredObj, deserializedBuffer));
}

TEST_P(HermesSerializationTest, SerializeWithTransferRunJavascript) {
  // This is constructed so that when `obj` is serialized and the `foo` getter
  // is invoked, it removes an element from `transferArr`. However, we should
  // still transfer both `ab1` and `ab2`
  eval(
      "var ab1 = new ArrayBuffer(8);"
      "var ab2 = new ArrayBuffer(16);"
      "var transferArr = [ab1, ab2];"
      "var obj = {"
      "    get foo() {"
      "        transferArr.pop();"
      "        return 'bar';"
      "    }"
      "};");
  auto obj = rt->global().getProperty(*rt, "obj");
  auto transferArr =
      rt->global().getProperty(*rt, "transferArr").getObject(*rt).getArray(*rt);

  auto ab1 =
      transferArr.getValueAtIndex(*rt, 0).getObject(*rt).getArrayBuffer(*rt);
  EXPECT_EQ(ab1.length(*rt), 8);
  auto ab2 =
      transferArr.getValueAtIndex(*rt, 1).getObject(*rt).getArrayBuffer(*rt);
  EXPECT_EQ(ab2.length(*rt), 16);

  auto serialized =
      serializationInterface->serializeWithTransfer(obj, transferArr);
  auto deserialized =
      serializationInterface2->deserializeWithTransfer(serialized);
  EXPECT_EQ(serialized, nullptr);
  // It should contain the deserialized `obj`, `ab1`, and `ab2` in that order
  EXPECT_EQ(deserialized.length(*rt2), 3);

  // Check that everything was properly deserialized into the 2nd runtime
  auto deserializedObj = deserialized.getValueAtIndex(*rt2, 0).getObject(*rt2);
  auto foo = deserializedObj.getProperty(*rt2, "foo");
  EXPECT_EQ(foo.getString(*rt2).utf8(*rt2), "bar");

  auto deserializedAb1 =
      deserialized.getValueAtIndex(*rt2, 1).getObject(*rt2).getArrayBuffer(
          *rt2);
  EXPECT_EQ(deserializedAb1.length(*rt2), 8);

  auto deserializedAb2 =
      deserialized.getValueAtIndex(*rt2, 2).getObject(*rt2).getArrayBuffer(
          *rt2);
  EXPECT_EQ(deserializedAb2.length(*rt2), 16);

  // Check that the original transfer array has been modified
  EXPECT_EQ(transferArr.length(*rt), 1);
  // Check the original ArrayBuffers have been detached
  EXPECT_THROW(ab1.size(*rt), JSINativeException);
  EXPECT_THROW(ab1.size(*rt2), JSINativeException);
}

TEST_P(HermesSerializationTest, SerializeWithTransferDetachedArrayBuffer) {
  // This is constructed so that when `obj` is serialized and the `foo` getter
  // is invoked, it detaches the ArrayBuffer `ab1`, so serialization should
  // fail.
  eval(
      "var ab1 = new ArrayBuffer(8);"
      "var ab2 = new ArrayBuffer(16);"
      "var transferArr = [ab1, ab2];"
      "var obj = {"
      "    get foo() {"
      "        HermesInternal.detachArrayBuffer(ab1);"
      "        return 'bar';"
      "    }"
      "};");
  auto obj = rt->global().getProperty(*rt, "obj");
  auto transferArr =
      rt->global().getProperty(*rt, "transferArr").getObject(*rt).getArray(*rt);

  auto ab1 =
      transferArr.getValueAtIndex(*rt, 0).getObject(*rt).getArrayBuffer(*rt);
  EXPECT_EQ(ab1.length(*rt), 8);
  auto ab2 =
      transferArr.getValueAtIndex(*rt, 1).getObject(*rt).getArrayBuffer(*rt);
  EXPECT_EQ(ab2.length(*rt), 16);
  EXPECT_THROW(
      serializationInterface->serializeWithTransfer(obj, transferArr), JSError);
}

TEST_P(HermesSerializationTest, SerializeWithTransferThrows) {
  /// Try to transfer a non-transferable value
  auto transferArr =
      eval("var transferArr = ['strings are non-transferable']; transferArr")
          .asObject(*rt)
          .asArray(*rt);

  auto val = Value::undefined();
  EXPECT_THROW(
      serializationInterface->serializeWithTransfer(val, transferArr), JSError);
}

TEST_P(HermesSerializationTest, SerializeManyExternalBuffersWithTransfer) {
  // Transferring an external ArrayBuffer creates handles, which must not
  // accumulate with the number of transferred buffers.
  struct FixedBuffer : MutableBuffer {
    size_t size() const override {
      return arr.size();
    }
    uint8_t *data() override {
      return arr.data();
    }

    std::array<uint8_t, 8> arr{};
  };

  Array buffers(*rt, 100);
  for (size_t i = 0; i < 100; ++i) {
    auto buf = std::make_shared<FixedBuffer>();
    buf->arr[0] = i;
    buffers.setValueAtIndex(*rt, i, ArrayBuffer(*rt, buf));
  }
  auto serialized = serializationInterface->serializeWithTransfer(
      Value(*rt, buffers), buffers);
  auto deserialized =
      serializationInterface2->deserializeWithTransfer(serialized);
  // The deserialized value is followed by the transferred buffers.
  ASSERT_EQ(deserialized.length(*rt2), 101);
  for (size_t i = 0; i < 100; ++i) {
    auto buffer = deserialized.getValueAtIndex(*rt2, i + 1)
                      .getObject(*rt2)
                      .getArrayBuffer(*rt2);
    EXPECT_EQ(buffer.data(*rt2)[0], i);
  }
}

class HermesWorkerTest : public HermesRuntimeTest {
 public:
  HermesWorkerTest() : HermesRuntimeTest() {}
};

#if HERMES_ENABLE_CORE_EXTENSIONS
TEST_P(HermesWorkerTest, WebWorkerBasic) {
  auto workerGlobal = rt->global().getProperty(*rt, "Worker");
  EXPECT_TRUE(!workerGlobal.isUndefined());

  EXPECT_THROW(eval("new Worker(123);"), JSError);

  // Create a simple worker that just loops forever
  auto code = R"(
var worker = new Worker(`while(true) {}`); worker;
)";
  auto worker = eval(code).asObject(*rt);

  auto terminate = worker.getPropertyAsFunction(*rt, "terminate");
  // Terminate on a non-Worker object should throw.
  Object nonWorkerObject(*rt);
  EXPECT_THROW(terminate.callWithThis(*rt, nonWorkerObject), JSError);

  // Terminate the worker
  terminate.callWithThis(*rt, worker);

  code = R"(
var worker = new Worker(`
  onmessage = function(msg) {
    print(msg);
  }
`);
var nontransferable= ["non-transferable"];
var ab = new ArrayBuffer(8);
var transfers = [ab];
worker;
)";
  worker = eval(code).asObject(*rt);

  auto postMessage = worker.getPropertyAsFunction(*rt, "postMessage");
  // postMessage on non-Worker object
  EXPECT_THROW(postMessage.callWithThis(*rt, nonWorkerObject, 1), JSError);
  // postMessage with no message
  EXPECT_THROW(postMessage.callWithThis(*rt, worker), JSError);

  // Post a message, then terminate worker
  postMessage.callWithThis(*rt, worker, "hello!");
  // Post a message with transfer, but transfers is not an Array
  EXPECT_THROW(
      postMessage.callWithThis(*rt, worker, "hello!", "not an array"), JSError);

  // Post a message with transfer, but don't the transfers array contains
  // non-transferable arguments
  auto nontransferable = rt->global().getProperty(*rt, "nontransferable");
  EXPECT_THROW(
      postMessage.callWithThis(*rt, worker, "hello!", nontransferable),
      JSError);

  // Send an ArrayBuffer as message without transfer
  auto abVal = rt->global().getProperty(*rt, "ab");
  postMessage.callWithThis(*rt, worker, abVal);
  // Make sure the original array buffer is not detached
  auto ab = abVal.asObject(*rt).getArrayBuffer(*rt);
  EXPECT_FALSE(ab.getProperty(*rt, "detached").asBool());

  // Post a message with transfer
  auto transfers = rt->global().getProperty(*rt, "transfers");
  postMessage.callWithThis(*rt, worker, "hello!", transfers);
  // Make sure the original array buffer is detached.
  EXPECT_TRUE(ab.getProperty(*rt, "detached").asBool());

  terminate = worker.getPropertyAsFunction(*rt, "terminate");
  terminate.callWithThis(*rt, worker);
}

TEST_P(HermesWorkerTest, WorkerFromArrayBuffer) {
  // Source carried in an ArrayBuffer must construct a Worker (no throw).
  auto code = R"(
var bytes = new TextEncoder().encode("var x = 1;");
var worker = new Worker(bytes.buffer);
worker;
)";
  auto worker = eval(code).asObject(*rt);
  worker.getPropertyAsFunction(*rt, "terminate").callWithThis(*rt, worker);
}

TEST_P(HermesWorkerTest, WorkerFromTypedArrayWithOffset) {
  // A Uint8Array view with a non-zero byteOffset must slice correctly.
  auto code = R"(
var whole = new TextEncoder().encode("XXvar y = 2;");
var view = new Uint8Array(whole.buffer, 2); // skip the leading "XX"
var worker = new Worker(view);
worker;
)";
  auto worker = eval(code).asObject(*rt);
  worker.getPropertyAsFunction(*rt, "terminate").callWithThis(*rt, worker);
}

TEST_P(HermesWorkerTest, WorkerFromDataView) {
  auto code = R"(
var bytes = new TextEncoder().encode("var z = 3;");
var dv = new DataView(bytes.buffer);
var worker = new Worker(dv);
worker;
)";
  auto worker = eval(code).asObject(*rt);
  worker.getPropertyAsFunction(*rt, "terminate").callWithThis(*rt, worker);
}

TEST_P(HermesWorkerTest, WorkerFromBinaryErrors) {
  // Non-buffer, non-string argument.
  EXPECT_THROW(eval("new Worker({});"), JSError);
  // Empty binary input.
  EXPECT_THROW(eval("new Worker(new ArrayBuffer(0));"), JSError);
  EXPECT_THROW(eval("new Worker(new Uint8Array(0));"), JSError);

  // Detached buffers must throw a TypeError specifically: a plain Error must
  // not leak from ArrayBuffer.size(), and a genuine but detached DataView must
  // still be recognized as a DataView rather than misclassified. Detach via
  // HermesInternal, matching the existing DetachedArrayBuffer /
  // ArrayBufferDetached tests in this file.
  auto thrownName = [&](const char *js) -> std::string {
    try {
      eval(js);
    } catch (const JSError &e) {
      return e.value()
          .asObject(*rt)
          .getProperty(*rt, "name")
          .asString(*rt)
          .utf8(*rt);
    }
    return "<no throw>";
  };
  EXPECT_EQ(
      thrownName(R"(
var ab = new ArrayBuffer(8);
HermesInternal.detachArrayBuffer(ab);
new Worker(ab);
)"),
      "TypeError");
  EXPECT_EQ(
      thrownName(R"(
var ab = new ArrayBuffer(8);
var dv = new DataView(ab);
HermesInternal.detachArrayBuffer(ab);
new Worker(dv);
)"),
      "TypeError");
}

TEST_P(HermesWorkerTest, WorkerFromBytecode) {
  // Compile a trivial script to Hermes bytecode.
  std::string bytecode;
  ASSERT_TRUE(hermes::compileJS("var x = 1;", bytecode));

  auto *api = castInterface<IHermesRootAPI>(makeHermesRootAPI());
  ASSERT_TRUE(api->isHermesBytecode(
      reinterpret_cast<const uint8_t *>(bytecode.data()), bytecode.size()));

  // Expose the bytecode to JS as a Uint8Array so `new Worker` receives the
  // exact bytes (bytecode magic contains bytes >= 0x80, which the old
  // string/utf8 path would have corrupted).
  auto u8ctor = rt->global().getPropertyAsFunction(*rt, "Uint8Array");
  auto arr =
      u8ctor.callAsConstructor(*rt, (double)bytecode.size()).asObject(*rt);
  for (size_t i = 0; i < bytecode.size(); ++i) {
    arr.setProperty(
        *rt,
        PropNameID::forUtf8(*rt, std::to_string(i)),
        (double)(uint8_t)bytecode[i]);
  }
  rt->global().setProperty(*rt, "__bc", arr);

  // Constructing from the bytecode bytes must succeed (worker thread starts
  // and evaluateJavaScript takes the bytecode path).
  auto worker = eval("var w = new Worker(__bc); w;").asObject(*rt);
  worker.getPropertyAsFunction(*rt, "terminate").callWithThis(*rt, worker);
}

INSTANTIATE_TEST_CASE_P(
    Runtimes,
    HermesWorkerTest,
    ::testing::ValuesIn(runtimeGenerators()));
#endif

INSTANTIATE_TEST_CASE_P(
    Runtimes,
    HermesSerializationTest,
    ::testing::ValuesIn(runtimeGenerators()));

#endif

TEST_P(HermesRuntimeTest, StringLengthTest) {
  // Test ASCII string length
  String ascii = String::createFromAscii(*rt, "hello");
  EXPECT_EQ(ascii.length(*rt), 5);

  // Test empty string
  String empty = String::createFromAscii(*rt, "");
  EXPECT_EQ(empty.length(*rt), 0);

  // Test euro sign (U+20AC) - BMP character, 1 code unit
  String euro = eval("'\\u20AC'").getString(*rt);
  EXPECT_EQ(euro.length(*rt), 1);

  // Test codepoint requiring 2 code units (surrogate pair)
  // U+1F600 is encoded as \uD83D\uDE00 in UTF-16
  String emoji = eval("'\\uD83D\\uDE00'").getString(*rt);
  EXPECT_EQ(emoji.length(*rt), 2);

  // Test another surrogate pair: U+10000 (first supplementary character)
  String supplementary = eval("'\\uD800\\uDC00'").getString(*rt);
  EXPECT_EQ(supplementary.length(*rt), 2);

  // Test lone high surrogate (U+D800)
  String loneHighSurrogate = eval("'\\uD800'").getString(*rt);
  EXPECT_EQ(loneHighSurrogate.length(*rt), 1);

  // Test lone low surrogate (U+DC00)
  String loneLowSurrogate = eval("'\\uDC00'").getString(*rt);
  EXPECT_EQ(loneLowSurrogate.length(*rt), 1);

  // Test lone surrogate in the middle of a string
  String mixedWithLoneSurrogate = eval("'a\\uD800b'").getString(*rt);
  EXPECT_EQ(mixedWithLoneSurrogate.length(*rt), 3);

  // Unicode Max Value is U+10FFFF, U+11FFFF is invalid
  // But it could be theoretically encoded as \uDBFF\uDFFF
  String invalid = eval("'\\uDBFF\\uDFFF'").getString(*rt);
  EXPECT_EQ(invalid.length(*rt), 2);
}

TEST_P(HermesRuntimeTest, CreateErrorTest) {
  // Test JSError::createEvalError
  {
    try {
      throw JSError::createEvalError(*rt, "eval error");
    } catch (const JSError &e) {
      // getMessage() checks the C++ JSError's cached message.
      // The "message" property check verifies the JS object was constructed
      // correctly, since they are populated independently.
      EXPECT_EQ(e.getMessage(), "eval error");
      Object caughtObj = e.value().getObject(*rt);
      EXPECT_EQ(
          caughtObj.getProperty(*rt, "message").getString(*rt).utf8(*rt),
          "eval error");
      EXPECT_TRUE(rt->instanceOf(
          caughtObj, rt->global().getPropertyAsFunction(*rt, "EvalError")));
    }
  }

  // Test JSError::createRangeError
  {
    try {
      throw JSError::createRangeError(*rt, "range error");
    } catch (const JSError &e) {
      EXPECT_EQ(e.getMessage(), "range error");
      Object caughtObj = e.value().getObject(*rt);
      EXPECT_EQ(
          caughtObj.getProperty(*rt, "message").getString(*rt).utf8(*rt),
          "range error");
      EXPECT_TRUE(rt->instanceOf(
          caughtObj, rt->global().getPropertyAsFunction(*rt, "RangeError")));
    }
  }

  // Test JSError::createReferenceError
  {
    try {
      throw JSError::createReferenceError(*rt, "reference error");
    } catch (const JSError &e) {
      EXPECT_EQ(e.getMessage(), "reference error");
      Object caughtObj = e.value().getObject(*rt);
      EXPECT_EQ(
          caughtObj.getProperty(*rt, "message").getString(*rt).utf8(*rt),
          "reference error");
      EXPECT_TRUE(rt->instanceOf(
          caughtObj,
          rt->global().getPropertyAsFunction(*rt, "ReferenceError")));
    }
  }

  // Test JSError::createSyntaxError
  {
    try {
      throw JSError::createSyntaxError(*rt, "syntax error");
    } catch (const JSError &e) {
      EXPECT_EQ(e.getMessage(), "syntax error");
      Object caughtObj = e.value().getObject(*rt);
      EXPECT_EQ(
          caughtObj.getProperty(*rt, "message").getString(*rt).utf8(*rt),
          "syntax error");
      EXPECT_TRUE(rt->instanceOf(
          caughtObj, rt->global().getPropertyAsFunction(*rt, "SyntaxError")));
    }
  }

  // Test JSError::createTypeError
  {
    try {
      throw JSError::createTypeError(*rt, "type error");
    } catch (const JSError &e) {
      EXPECT_EQ(e.getMessage(), "type error");
      Object caughtObj = e.value().getObject(*rt);
      EXPECT_EQ(
          caughtObj.getProperty(*rt, "message").getString(*rt).utf8(*rt),
          "type error");
      EXPECT_TRUE(rt->instanceOf(
          caughtObj, rt->global().getPropertyAsFunction(*rt, "TypeError")));
    }
  }

  // Test JSError::createURIError
  {
    try {
      throw JSError::createURIError(*rt, "uri error");
    } catch (const JSError &e) {
      EXPECT_EQ(e.getMessage(), "uri error");
      Object caughtObj = e.value().getObject(*rt);
      EXPECT_EQ(
          caughtObj.getProperty(*rt, "message").getString(*rt).utf8(*rt),
          "uri error");
      EXPECT_TRUE(rt->instanceOf(
          caughtObj, rt->global().getPropertyAsFunction(*rt, "URIError")));
    }
  }
}

INSTANTIATE_TEST_CASE_P(
    Runtimes,
    HermesRuntimeTest,
    ::testing::ValuesIn(runtimeGeneratorsWithSandBox()));

INSTANTIATE_TEST_CASE_P(
    Runtimes,
    HermesRuntimeBytecodeTest,
    ::testing::ValuesIn(runtimeGenerators()));
} // namespace
