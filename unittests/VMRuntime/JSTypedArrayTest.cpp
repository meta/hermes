/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSTypedArray.h"

#include "VMRuntimeTestHelpers.h"

using namespace hermes::vm;

namespace {

using JSTypedArrayTest = RuntimeTestFixture;

TEST_F(JSTypedArrayTest, BeginAndEndMatchesOnBaseAndSub) {
  // Make sure that the begin and end pointers match for both JSTypedArrayBase,
  // and JSTypedArray<T>. Do this for each T.

#define TYPED_ARRAY(name, type)                                              \
  {                                                                          \
    auto result = name##Array::allocate(runtime, 10);                        \
    ASSERT_FALSE(isException(result));                                       \
    Handle<name##Array> array =                                              \
        Handle<name##Array>::vmcast(runtime.makeHandle(std::move(*result))); \
    EXPECT_EQ(                                                               \
        reinterpret_cast<uintptr_t>(array->data(runtime)),                   \
        reinterpret_cast<uintptr_t>(                                         \
            Handle<JSTypedArrayBase>(array)->data(runtime)));                \
    EXPECT_EQ(                                                               \
        reinterpret_cast<uintptr_t>(array->dataEnd(runtime)),                \
        reinterpret_cast<uintptr_t>(                                         \
            Handle<JSTypedArrayBase>(array)->dataEnd(runtime)));             \
  }
#include "hermes/VM/TypedArrays.def"
}

} // namespace
