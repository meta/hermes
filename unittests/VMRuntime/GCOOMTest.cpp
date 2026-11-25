/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/sh_config.h"

#if HERMESVM_GCKIND != _HERMESVM_GCVALUE_MALLOC

#include "gtest/gtest.h"

#include "EmptyCell.h"
#include "VMRuntimeTestHelpers.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/JSWeakMapImpl.h"
#include "hermes/VM/LimitedStorageProvider.h"

#include <deque>

using namespace hermes::vm;

namespace {

TEST(GCOOMDeathTest, SuperSegment) {
  auto fn = [] {
    using SuperSegmentCell = EmptyCell<GC::maxNormalAllocationSize() * 2>;
    auto runtime = DummyRuntime::create(kTestGCConfig);
    SuperSegmentCell::create(*runtime);
  };
  EXPECT_OOM(fn());
}

static void exceedMaxHeap(
    GCConfig::Builder baseConfig = kTestGCConfigBaseBuilder) {
  static constexpr size_t kSegments = 10;
  static constexpr size_t kHeapSizeHint =
      FixedSizeHeapSegment::maxSize() * kSegments;
  // Only one of these cells will fit into a segment, with the maximum amount of
  // space wasted in the segment.
  using AwkwardCell = EmptyCell<FixedSizeHeapSegment::maxSize() / 2 + 1>;

  auto runtime =
      DummyRuntime::create(TestGCConfigFixedSize(kHeapSizeHint, baseConfig));
  DummyRuntime &rt = *runtime;
  struct : Locals {
    PinnedValue<AwkwardCell> handles[20]; // kSegments + 2 + some extra
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  // Exceed the maximum size of the heap. Note we need 2 extra segments instead
  // of just one because Hades can sometimes hide the memory for a segment
  // during compaction.
  for (size_t i = 0; i < kSegments + 2; ++i)
    lv.handles[i] = AwkwardCell::create(rt);
}

// When handlesan is ON, we won't check the heap footprint when creating new
// heap segment. And if mmap storage provider is used, there's no limit on the
// number of segments as well. For simplicity, just disable these tests under
// handlesan.
#if HERMESVM_SANITIZE_HANDLES == 0
TEST(GCOOMDeathTest, Fragmentation) {
  EXPECT_OOM(exceedMaxHeap());
}

TEST(GCOOMDeathTest, WeakMapMarking) {
  auto fn = [] {
    auto rt = Runtime::create(
        RuntimeConfig::Builder().withGCConfig(kTestGCConfig).build());
    auto &runtime = *rt;
    struct : public Locals {
      PinnedValue<JSWeakMap> map;
      PinnedValue<ArrayStorage> keysSlot;
      PinnedValue<JSObject> key;
      PinnedValue<JSObject> value;
    } lv;
    LocalsRAII lraii(runtime, &lv);
    auto mapResult = JSWeakMap::create(
        runtime, Handle<JSObject>::vmcast(&runtime.weakMapPrototype));
    lv.map = std::move(*mapResult);
    MutableHandle<ArrayStorage> keys{lv.keysSlot};
    keys = vmcast<ArrayStorage>(*ArrayStorage::create(runtime, 5));
    while (true) {
      lv.key = JSObject::create(runtime);
      ArrayStorage::push_back(keys, runtime, lv.key);
      lv.value = JSObject::create(runtime);
      JSWeakMap::setValue(lv.map, runtime, lv.key, lv.value);
    }
  };
  EXPECT_OOM(fn());
}
#endif

} // namespace

#endif
