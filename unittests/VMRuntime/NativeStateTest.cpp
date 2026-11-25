/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/NativeState.h"

#include "VMRuntimeTestHelpers.h"
#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

using NativeStateTest = RuntimeTestFixture;

void inc(GC &, NativeState *ns) {
  ++*static_cast<int *>(ns->context());
}

TEST_F(NativeStateTest, NativeStateFinalizerRunsOnce) {
  int counter = 0;
  {
    struct : public Locals {
      PinnedValue<NativeState> state;
    } lv;
    LocalsRAII lraii(runtime, &lv);
    lv.state = NativeState::create(runtime, &counter, inc);
    runtime.collect("test");
    // should not have been finalized yet
    EXPECT_EQ(0, counter);
  }
  // should finalize once
  runtime.collect("test");
  EXPECT_EQ(1, counter);
  runtime.collect("test");
  runtime.collect("test");
  EXPECT_EQ(1, counter);
}

} // namespace
