/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NapiTestFixture.h"

#include "hermes/VM/HermesValue.h"
#include "hermes/VM/StringPrimitive.h"

namespace {

using hermes::napi::NapiTestFixture;
using namespace hermes::vm;

/// Helper to open a handle scope and return the scope handle, asserting
/// success.
static napi_handle_scope openScope(napi_env env) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_ok, napi_open_handle_scope(env, &scope));
  EXPECT_NE(nullptr, scope);
  return scope;
}

/// Helper to close a handle scope, asserting success.
static void closeScope(napi_env env, napi_handle_scope scope) {
  EXPECT_EQ(napi_ok, napi_close_handle_scope(env, scope));
}

//===========================================================================
// napi_open_handle_scope / napi_close_handle_scope — API tests
//===========================================================================

TEST_F(NapiTestFixture, HandleScopeAPI_OpenClose) {
  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));
  EXPECT_NE(nullptr, scope);
  EXPECT_EQ(1, env_->open_handle_scopes);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
  EXPECT_EQ(0, env_->open_handle_scopes);
}

TEST_F(NapiTestFixture, HandleScopeAPI_OpenNullEnv) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_invalid_arg, napi_open_handle_scope(nullptr, &scope));
}

TEST_F(NapiTestFixture, HandleScopeAPI_OpenNullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_open_handle_scope(env_, nullptr));
}

TEST_F(NapiTestFixture, HandleScopeAPI_CloseNullEnv) {
  napi_handle_scope scope = openScope(env_);
  EXPECT_EQ(napi_invalid_arg, napi_close_handle_scope(nullptr, scope));
  // Clean up — scope is still open on env_.
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HandleScopeAPI_CloseNullScope) {
  EXPECT_EQ(napi_invalid_arg, napi_close_handle_scope(env_, nullptr));
}

TEST_F(NapiTestFixture, HandleScopeAPI_CloseWithoutOpen) {
  // Closing a scope when none are open should return a mismatch error.
  // Use a non-null dummy value to pass the CHECK_ARG.
  napi_handle_scope dummy = reinterpret_cast<napi_handle_scope>(1);
  EXPECT_EQ(napi_handle_scope_mismatch, napi_close_handle_scope(env_, dummy));
}

TEST_F(NapiTestFixture, HandleScopeAPI_NestedOpenClose) {
  napi_handle_scope s1 = openScope(env_);
  EXPECT_EQ(1, env_->open_handle_scopes);

  napi_handle_scope s2 = openScope(env_);
  EXPECT_EQ(2, env_->open_handle_scopes);

  napi_handle_scope s3 = openScope(env_);
  EXPECT_EQ(3, env_->open_handle_scopes);

  // Close in LIFO order.
  closeScope(env_, s3);
  EXPECT_EQ(2, env_->open_handle_scopes);

  closeScope(env_, s2);
  EXPECT_EQ(1, env_->open_handle_scopes);

  closeScope(env_, s1);
  EXPECT_EQ(0, env_->open_handle_scopes);
}

TEST_F(NapiTestFixture, HandleScopeAPI_CloseMismatch) {
  napi_handle_scope s1 = openScope(env_);
  napi_handle_scope s2 = openScope(env_);

  // Trying to close s1 (the outer scope) while s2 is still open
  // should fail with a mismatch error.
  EXPECT_EQ(napi_handle_scope_mismatch, napi_close_handle_scope(env_, s1));

  // Clean up in correct order.
  closeScope(env_, s2);
  closeScope(env_, s1);
}

//===========================================================================
// addToCurrentScope — basic functionality (using API scopes)
//===========================================================================

TEST_F(NapiTestFixture, HandleScope_AddNumber) {
  napi_handle_scope scope = openScope(env_);

  // Store a number value in the handle scope.
  HermesValue num = HermesValue::encodeTrustedNumberValue(42.0);
  napi_value handle = env_->addToCurrentScope(num);
  ASSERT_NE(nullptr, handle);

  // The handle should point to a PinnedHermesValue containing 42.0.
  auto *phv = reinterpret_cast<PinnedHermesValue *>(handle);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(42.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HandleScope_AddMultipleValues) {
  napi_handle_scope scope = openScope(env_);

  napi_value h1 =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(1.0));
  napi_value h2 =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(2.0));
  napi_value h3 = env_->addToCurrentScope(HermesValue::encodeBoolValue(true));

  ASSERT_NE(nullptr, h1);
  ASSERT_NE(nullptr, h2);
  ASSERT_NE(nullptr, h3);

  // All handles should be distinct pointers.
  EXPECT_NE(h1, h2);
  EXPECT_NE(h2, h3);
  EXPECT_NE(h1, h3);

  // Each should retain its value.
  auto *p1 = reinterpret_cast<PinnedHermesValue *>(h1);
  auto *p2 = reinterpret_cast<PinnedHermesValue *>(h2);
  auto *p3 = reinterpret_cast<PinnedHermesValue *>(h3);
  EXPECT_EQ(1.0, p1->getNumber());
  EXPECT_EQ(2.0, p2->getNumber());
  EXPECT_TRUE(p3->getBool());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HandleScope_AddUndefined) {
  napi_handle_scope scope = openScope(env_);

  napi_value handle =
      env_->addToCurrentScope(HermesValue::encodeUndefinedValue());
  ASSERT_NE(nullptr, handle);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(handle);
  EXPECT_TRUE(phv->isUndefined());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HandleScope_AddNull) {
  napi_handle_scope scope = openScope(env_);

  napi_value handle = env_->addToCurrentScope(HermesValue::encodeNullValue());
  ASSERT_NE(nullptr, handle);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(handle);
  EXPECT_TRUE(phv->isNull());

  closeScope(env_, scope);
}

//===========================================================================
// Block overflow — adding more than kSize handles
//===========================================================================

TEST_F(NapiTestFixture, HandleScope_BlockOverflow) {
  napi_handle_scope scope = openScope(env_);

  // Fill more than one block's worth of handles.
  const size_t count = HandleBlock::kSize + 10;
  std::vector<napi_value> handles;
  handles.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    napi_value h = env_->addToCurrentScope(
        HermesValue::encodeTrustedNumberValue(static_cast<double>(i)));
    ASSERT_NE(nullptr, h) << "Handle " << i << " was null";
    handles.push_back(h);
  }

  // All handles should still be valid and hold correct values.
  for (size_t i = 0; i < count; ++i) {
    auto *phv = reinterpret_cast<PinnedHermesValue *>(handles[i]);
    EXPECT_EQ(static_cast<double>(i), phv->getNumber())
        << "Handle " << i << " has wrong value";
  }

  // We should have at least 2 blocks now.
  EXPECT_GE(env_->blocks_.size(), 2u);

  closeScope(env_, scope);
}

//===========================================================================
// Nested scopes
//===========================================================================

TEST_F(NapiTestFixture, HandleScope_NestedScopes) {
  napi_handle_scope s1 = openScope(env_);

  napi_value outer =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(100.0));

  napi_handle_scope s2 = openScope(env_);

  napi_value inner =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(200.0));

  // Both should be valid.
  auto *pOuter = reinterpret_cast<PinnedHermesValue *>(outer);
  auto *pInner = reinterpret_cast<PinnedHermesValue *>(inner);
  EXPECT_EQ(100.0, pOuter->getNumber());
  EXPECT_EQ(200.0, pInner->getNumber());

  closeScope(env_, s2);

  // After closing inner scope, outer handle should still be valid.
  EXPECT_EQ(100.0, pOuter->getNumber());

  closeScope(env_, s1);
}

TEST_F(NapiTestFixture, HandleScope_ScopeReusesSlots) {
  napi_handle_scope s1 = openScope(env_);

  // Record the initial slot position.
  size_t initialBlock = env_->currentBlockIndex_;
  size_t initialSlot = env_->currentSlotIndex_;

  {
    napi_handle_scope s2 = openScope(env_);
    // Add some handles in the inner scope.
    env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(1.0));
    env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(2.0));
    env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(3.0));

    // Slot pointer should have advanced.
    EXPECT_EQ(initialBlock, env_->currentBlockIndex_);
    EXPECT_EQ(initialSlot + 3, env_->currentSlotIndex_);

    closeScope(env_, s2);
  }

  // After closing the inner scope, the slot pointer should be restored.
  EXPECT_EQ(initialBlock, env_->currentBlockIndex_);
  EXPECT_EQ(initialSlot, env_->currentSlotIndex_);

  closeScope(env_, s1);
}

TEST_F(NapiTestFixture, HandleScope_ThreeLevelNesting) {
  napi_handle_scope s1 = openScope(env_);
  napi_value h1 =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(1.0));

  napi_handle_scope s2 = openScope(env_);
  napi_value h2 =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(2.0));

  napi_handle_scope s3 = openScope(env_);
  napi_value h3 =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(3.0));

  // All three handles should be valid.
  auto *p1 = reinterpret_cast<PinnedHermesValue *>(h1);
  auto *p2 = reinterpret_cast<PinnedHermesValue *>(h2);
  auto *p3 = reinterpret_cast<PinnedHermesValue *>(h3);
  EXPECT_EQ(1.0, p1->getNumber());
  EXPECT_EQ(2.0, p2->getNumber());
  EXPECT_EQ(3.0, p3->getNumber());

  // Close innermost scope.
  closeScope(env_, s3);

  // h1 and h2 should still be valid.
  EXPECT_EQ(1.0, p1->getNumber());
  EXPECT_EQ(2.0, p2->getNumber());

  // Close middle scope.
  closeScope(env_, s2);

  // h1 should still be valid.
  EXPECT_EQ(1.0, p1->getNumber());

  closeScope(env_, s1);
}

//===========================================================================
// GC survival — handles keep GC-managed objects alive
//===========================================================================

TEST_F(NapiTestFixture, HandleScope_GCSurvival) {
  napi_handle_scope scope = openScope(env_);

  napi_value handle;
  {
    auto strHandle =
        StringPrimitive::createNoThrow(*rt_, llvh::StringRef("hello"));
    // addToCurrentScope copies into a PinnedHermesValue.
    handle = env_->addToCurrentScope(strHandle.getHermesValue());
  }

  ASSERT_NE(nullptr, handle);

  // Trigger GC — the string should survive because our handle scope
  // marks it as a root.
  collectAndDrain();

  // Verify the string is still valid after GC.
  auto *phv = reinterpret_cast<PinnedHermesValue *>(handle);
  EXPECT_TRUE(phv->isString());
  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(5u, str->getStringLength());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HandleScope_MultipleGCCycles) {
  napi_handle_scope scope = openScope(env_);

  napi_value handle;
  {
    auto strHandle =
        StringPrimitive::createNoThrow(*rt_, llvh::StringRef("world"));
    handle = env_->addToCurrentScope(strHandle.getHermesValue());
  }

  // Multiple GC cycles — the handle should keep the object alive through
  // all of them.
  for (int i = 0; i < 3; ++i) {
    collectAndDrain();
    auto *phv = reinterpret_cast<PinnedHermesValue *>(handle);
    EXPECT_TRUE(phv->isString())
        << "String became invalid after GC cycle " << i;
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HandleScope_ClosedScopeReleasesGCRoots) {
  napi_handle_scope s1 = openScope(env_);

  napi_handle_scope s2 = openScope(env_);
  {
    auto strHandle =
        StringPrimitive::createNoThrow(*rt_, llvh::StringRef("temp"));
    env_->addToCurrentScope(strHandle.getHermesValue());
  }
  closeScope(env_, s2);

  // After closing s2, the string handle is no longer in the live range.
  // A GC should not crash (the slot is outside the current range).
  collectAndDrain();

  closeScope(env_, s1);
}

//===========================================================================
// markHandleScopes — empty state
//===========================================================================

TEST_F(NapiTestFixture, HandleScope_MarkEmptyScopes) {
  // With no scopes open, markHandleScopes should be a no-op.
  // This just verifies it doesn't crash.
  collectAndDrain();
}

TEST_F(NapiTestFixture, HandleScope_MarkEmptyOpenScope) {
  napi_handle_scope scope = openScope(env_);
  // Open scope but no handles added — GC should not crash.
  collectAndDrain();
  closeScope(env_, scope);
}

//===========================================================================
// Escapable handle scope helpers
//===========================================================================

/// Helper to open an escapable handle scope, asserting success.
static napi_escapable_handle_scope openEscapableScope(napi_env env) {
  napi_escapable_handle_scope scope = nullptr;
  EXPECT_EQ(napi_ok, napi_open_escapable_handle_scope(env, &scope));
  EXPECT_NE(nullptr, scope);
  return scope;
}

/// Helper to close an escapable handle scope, asserting success.
static void closeEscapableScope(
    napi_env env,
    napi_escapable_handle_scope scope) {
  EXPECT_EQ(napi_ok, napi_close_escapable_handle_scope(env, scope));
}

//===========================================================================
// napi_open_escapable_handle_scope / napi_close_escapable_handle_scope
//===========================================================================

TEST_F(NapiTestFixture, EscapableScope_OpenClose) {
  napi_handle_scope outer = openScope(env_);

  napi_escapable_handle_scope esc = nullptr;
  ASSERT_EQ(napi_ok, napi_open_escapable_handle_scope(env_, &esc));
  EXPECT_NE(nullptr, esc);
  EXPECT_EQ(2, env_->open_handle_scopes);

  ASSERT_EQ(napi_ok, napi_close_escapable_handle_scope(env_, esc));
  EXPECT_EQ(1, env_->open_handle_scopes);

  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapableScope_OpenNullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, napi_open_escapable_handle_scope(nullptr, nullptr));
}

TEST_F(NapiTestFixture, EscapableScope_OpenNullResult) {
  napi_handle_scope outer = openScope(env_);
  EXPECT_EQ(napi_invalid_arg, napi_open_escapable_handle_scope(env_, nullptr));
  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapableScope_OpenWithoutParent) {
  // Opening an escapable scope without any parent scope should fail
  // because there is no parent scope to hold the escape slot.
  napi_escapable_handle_scope esc = nullptr;
  EXPECT_EQ(
      napi_handle_scope_mismatch, napi_open_escapable_handle_scope(env_, &esc));
}

TEST_F(NapiTestFixture, EscapableScope_CloseNullEnv) {
  napi_handle_scope outer = openScope(env_);
  napi_escapable_handle_scope esc = openEscapableScope(env_);
  EXPECT_EQ(napi_invalid_arg, napi_close_escapable_handle_scope(nullptr, esc));
  closeEscapableScope(env_, esc);
  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapableScope_CloseNullScope) {
  EXPECT_EQ(napi_invalid_arg, napi_close_escapable_handle_scope(env_, nullptr));
}

TEST_F(NapiTestFixture, EscapableScope_CloseWithoutOpen) {
  napi_escapable_handle_scope dummy =
      reinterpret_cast<napi_escapable_handle_scope>(1);
  EXPECT_EQ(
      napi_handle_scope_mismatch,
      napi_close_escapable_handle_scope(env_, dummy));
}

TEST_F(NapiTestFixture, EscapableScope_CloseMismatch) {
  napi_handle_scope outer = openScope(env_);
  napi_escapable_handle_scope esc = openEscapableScope(env_);
  napi_handle_scope inner = openScope(env_);

  // Trying to close the escapable scope while inner is still open.
  EXPECT_EQ(
      napi_handle_scope_mismatch, napi_close_escapable_handle_scope(env_, esc));

  // Clean up in correct order.
  closeScope(env_, inner);
  closeEscapableScope(env_, esc);
  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapableScope_CreateValuesInsideAndClose) {
  napi_handle_scope outer = openScope(env_);

  napi_escapable_handle_scope esc = openEscapableScope(env_);

  // Create some values inside the escapable scope.
  napi_value h1 =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(10.0));
  napi_value h2 =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(20.0));
  ASSERT_NE(nullptr, h1);
  ASSERT_NE(nullptr, h2);

  auto *p1 = reinterpret_cast<PinnedHermesValue *>(h1);
  auto *p2 = reinterpret_cast<PinnedHermesValue *>(h2);
  EXPECT_EQ(10.0, p1->getNumber());
  EXPECT_EQ(20.0, p2->getNumber());

  // Close the escapable scope without escaping — values are reclaimed.
  closeEscapableScope(env_, esc);

  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapableScope_EscapeSlotPreAllocated) {
  napi_handle_scope outer = openScope(env_);

  // Record the slot position before opening the escapable scope.
  size_t slotsBefore = env_->currentSlotIndex_;

  napi_escapable_handle_scope esc = openEscapableScope(env_);

  // The escapable scope should have pre-allocated one slot in the parent
  // scope for the escape value, so the saved position in the new scope
  // descriptor should be one past the pre-open position.
  auto &desc = env_->scopeStack_.back();
  EXPECT_TRUE(desc.escapable);
  EXPECT_FALSE(desc.escapeCalled);
  EXPECT_NE(nullptr, desc.escapeSlot);

  // The escape slot should be initialized to undefined.
  EXPECT_TRUE(desc.escapeSlot->isUndefined());

  // The current slot index should have advanced by 1 (for the escape slot)
  // relative to where the outer scope started, and the new scope's saved
  // position should be at slotsBefore + 1.
  EXPECT_EQ(slotsBefore + 1, desc.slotIndex);

  closeEscapableScope(env_, esc);
  closeScope(env_, outer);
}

//===========================================================================
// napi_escape_handle
//===========================================================================

TEST_F(NapiTestFixture, EscapeHandle_Basic) {
  napi_handle_scope outer = openScope(env_);

  napi_escapable_handle_scope esc = openEscapableScope(env_);

  // Create a value inside the escapable scope.
  napi_value inner =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(42.0));

  // Escape the value to the parent scope.
  napi_value escaped = nullptr;
  ASSERT_EQ(napi_ok, napi_escape_handle(env_, esc, inner, &escaped));
  ASSERT_NE(nullptr, escaped);

  // The escaped value should be different from the inner value (it's in the
  // parent scope's slot).
  EXPECT_NE(inner, escaped);

  // The escaped value should hold the same JS value.
  auto *phv = reinterpret_cast<PinnedHermesValue *>(escaped);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(42.0, phv->getNumber());

  closeEscapableScope(env_, esc);

  // After closing the escapable scope, the escaped value should still be
  // valid because it lives in the parent scope.
  EXPECT_EQ(42.0, phv->getNumber());

  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapeHandle_CalledTwice) {
  napi_handle_scope outer = openScope(env_);
  napi_escapable_handle_scope esc = openEscapableScope(env_);

  napi_value v1 =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(1.0));
  napi_value v2 =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(2.0));

  // First escape should succeed.
  napi_value escaped = nullptr;
  ASSERT_EQ(napi_ok, napi_escape_handle(env_, esc, v1, &escaped));

  // Second escape should fail.
  napi_value escaped2 = nullptr;
  EXPECT_EQ(
      napi_escape_called_twice, napi_escape_handle(env_, esc, v2, &escaped2));

  closeEscapableScope(env_, esc);
  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapeHandle_NullArgs) {
  napi_handle_scope outer = openScope(env_);
  napi_escapable_handle_scope esc = openEscapableScope(env_);

  napi_value v =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(1.0));
  napi_value escaped = nullptr;

  // Null env.
  EXPECT_EQ(napi_invalid_arg, napi_escape_handle(nullptr, esc, v, &escaped));

  // Null scope.
  EXPECT_EQ(napi_invalid_arg, napi_escape_handle(env_, nullptr, v, &escaped));

  // Null escapee.
  EXPECT_EQ(napi_invalid_arg, napi_escape_handle(env_, esc, nullptr, &escaped));

  // Null result.
  EXPECT_EQ(napi_invalid_arg, napi_escape_handle(env_, esc, v, nullptr));

  closeEscapableScope(env_, esc);
  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapeHandle_ScopeMismatch) {
  napi_handle_scope outer = openScope(env_);
  napi_escapable_handle_scope esc = openEscapableScope(env_);

  // Open another scope on top of the escapable scope.
  napi_handle_scope inner = openScope(env_);

  napi_value v =
      env_->addToCurrentScope(HermesValue::encodeTrustedNumberValue(1.0));
  napi_value escaped = nullptr;

  // Trying to escape from esc while inner is the topmost scope should
  // fail because the scope tag doesn't match.
  EXPECT_EQ(
      napi_handle_scope_mismatch, napi_escape_handle(env_, esc, v, &escaped));

  closeScope(env_, inner);
  closeEscapableScope(env_, esc);
  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapeHandle_GCSurvival) {
  napi_handle_scope outer = openScope(env_);

  napi_value escaped = nullptr;
  {
    napi_escapable_handle_scope esc = openEscapableScope(env_);

    napi_value handle;
    {
      auto strHandle =
          StringPrimitive::createNoThrow(*rt_, llvh::StringRef("escaped"));
      handle = env_->addToCurrentScope(strHandle.getHermesValue());
    }

    // Escape the string to the parent scope.
    ASSERT_EQ(napi_ok, napi_escape_handle(env_, esc, handle, &escaped));
    closeEscapableScope(env_, esc);
  }

  // The escaped value should survive GC because it lives in the parent
  // scope.
  collectAndDrain();

  auto *phv = reinterpret_cast<PinnedHermesValue *>(escaped);
  EXPECT_TRUE(phv->isString());
  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(7u, str->getStringLength());

  closeScope(env_, outer);
}

TEST_F(NapiTestFixture, EscapableScope_ClearsErrorOnSuccess) {
  napi_handle_scope outer = openScope(env_);

  napi_escapable_handle_scope esc = nullptr;
  ASSERT_EQ(napi_ok, napi_open_escapable_handle_scope(env_, &esc));
  EXPECT_EQ(napi_ok, env_->last_error.error_code);

  ASSERT_EQ(napi_ok, napi_close_escapable_handle_scope(env_, esc));
  EXPECT_EQ(napi_ok, env_->last_error.error_code);

  closeScope(env_, outer);
}

} // namespace
