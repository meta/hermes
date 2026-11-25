/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/CallResult.h"
#include "hermes/VM/Runtime.h"

#include "VMRuntimeTestHelpers.h"

#include <climits>

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

TEST(CallResultTest, TrivialTest) {
  struct TrivialStruct {
    int x;
    int y;
  };
  static_assert(
      std::is_trivial<TrivialStruct>::value, "TrivialStruct is not trivial");

  TrivialStruct ts{1, 2};
  CallResult<TrivialStruct> cr(ts);
  EXPECT_EQ(ExecutionStatus::RETURNED, cr.getStatus());
  EXPECT_EQ(1, cr.getValue().x);
  EXPECT_EQ(1, cr->x);
  EXPECT_EQ(1, (*cr).x);
}

TEST(CallResultTest, BoolTest) {
  bool x = true;
  CallResult<bool> cr(x);
  EXPECT_EQ(ExecutionStatus::RETURNED, cr.getStatus());
  EXPECT_EQ(true, cr.getValue());
  EXPECT_EQ(true, *cr);
}

TEST(CallResultTest, PointerTest) {
  struct Wrapper {
    int x;
  };
  Wrapper w{1};
  Wrapper *ptr = &w;
  CallResult<Wrapper *> cr(ptr);
  EXPECT_EQ(ExecutionStatus::RETURNED, cr.getStatus());
  EXPECT_EQ(1, cr.getValue()->x);
  EXPECT_EQ(1, cr->x);
  EXPECT_EQ(1, (*cr)->x);
}

using CallResultRuntimeTest = RuntimeTestFixture;

TEST_F(CallResultRuntimeTest, HandleTest) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  PseudoHandle<JSObject> pseudo = JSObject::create(runtime);
  lv.obj = std::move(pseudo);
  Handle<JSObject> handle = lv.obj;
  CallResult<Handle<JSObject>> cr(handle);
  EXPECT_EQ(ExecutionStatus::RETURNED, cr.getStatus());
  EXPECT_TRUE(strictEqualityTest(
      handle.getHermesValue(), cr.getValue().getHermesValue()));
  EXPECT_TRUE(
      strictEqualityTest(handle.getHermesValue(), cr->getHermesValue()));
  EXPECT_TRUE(
      strictEqualityTest(handle.getHermesValue(), (*cr).getHermesValue()));

  CallResult<Handle<JSObject>> cr2{ExecutionStatus::EXCEPTION};
  cr2 = handle;
  EXPECT_EQ(ExecutionStatus::RETURNED, cr.getStatus());
  EXPECT_TRUE(
      strictEqualityTest(handle.getHermesValue(), cr->getHermesValue()));
}

TEST_F(CallResultRuntimeTest, PseudoHandleTest) {
  PseudoHandle<JSObject> pseudo = JSObject::create(runtime);
  HermesValue hValue = pseudo.getHermesValue();
  // No allocations from here on out.
  CallResult<PseudoHandle<JSObject>> cr(std::move(pseudo));
  EXPECT_EQ(ExecutionStatus::RETURNED, cr.getStatus());
  EXPECT_TRUE(strictEqualityTest(hValue, cr.getValue().getHermesValue()));
  EXPECT_TRUE(strictEqualityTest(hValue, cr->getHermesValue()));
  EXPECT_TRUE(strictEqualityTest(hValue, (*cr).getHermesValue()));
}

TEST(CallResultTest, OperatorTestNonTrivial) {
  // This test uses a variety of techniques to verify the correct behavior of
  // assignment and moves in CallResult with None type.
  int count = 0;
  struct NonTrivial {
    // counter for the number of ctors and dtors run.
    int *count;
    // A large array detects double frees.
    std::unique_ptr<int[]> arr;
    // Verify that self always points to 'this'.
    const NonTrivial *self;
    NonTrivial(int *count) : count(count), arr(new int[1 << 16]), self(this) {
      *count += 1;
    }
    NonTrivial(NonTrivial &&rhs)
        : count(rhs.count), arr(std::move(rhs.arr)), self(this) {
      *count += 1;
    }
    NonTrivial(const NonTrivial &rhs)
        : count(rhs.count), arr(new int[1 << 16]), self(this) {
      *count += 1;
    }
    NonTrivial &operator=(const NonTrivial &rhs) {
      *count -= 1;
      count = rhs.count;
      *count += 1;
      return *this;
    }
    ~NonTrivial() {
      EXPECT_EQ(self, this);
      *count -= 1;
    }
  };
  auto nonTrivialType = detail::GetCallResultSpecialize<NonTrivial>::value;
  ASSERT_EQ(nonTrivialType, detail::CallResultSpecialize::None);
  CallResult<NonTrivial> cr{ExecutionStatus::EXCEPTION};
  CallResult<NonTrivial> local(&count);
  for (int i = 0; i < 10; i++) {
    // operator=(CallResult &&)
    cr = CallResult<NonTrivial>(NonTrivial(&count));
    // operator=(const CallResult &)
    cr = local;

    CallResult<NonTrivial> tmp(&count);
    // move ctor
    CallResult<NonTrivial> tmp2(std::move(tmp));
    // copy ctor
    CallResult<NonTrivial> tmp3(tmp2);
    (void)tmp3;
  }
  EXPECT_EQ(count, 2);
  cr = ExecutionStatus::EXCEPTION;
  EXPECT_EQ(count, 1);
}

static CallResult<HermesValue> returnFailure() {
  return ExecutionStatus::EXCEPTION;
}

static CallResult<HermesValue> returnSuccess(HermesValue hv) {
  return hv;
}

TEST(CallResultTest, HermesValueTest) {
  static_assert(
      std::is_trivial<HermesValue>::value, "HermesValue must be trivial");
  auto cr1 = returnFailure();
  EXPECT_EQ(ExecutionStatus::EXCEPTION, cr1.getStatus());
  EXPECT_EQ(ExecutionStatus::EXCEPTION, cr1);

  auto cr2 = returnSuccess(HermesValue::encodeTrustedNumberValue(3.14));
  EXPECT_EQ(ExecutionStatus::RETURNED, cr2.getStatus());
  EXPECT_EQ(ExecutionStatus::RETURNED, cr2);
  EXPECT_TRUE(cr2.getValue().isNumber());
  EXPECT_TRUE(cr2->isNumber());
  EXPECT_TRUE((*cr2).isNumber());
  EXPECT_EQ(3.14, cr2->getNumber());
}

} // namespace
