/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_impl.h"

#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

#include "gtest/gtest.h"

#include <memory>
#include <string>

namespace hermes {
namespace napi {

/// Test fixture that creates a Runtime and a napi_env.
class NapiModuleLoadTest : public ::testing::Test {
 protected:
  std::shared_ptr<vm::Runtime> rt_;
  napi_env env_ = nullptr;

  void SetUp() override {
    auto config = vm::RuntimeConfig::Builder()
                      .withGCConfig(
                          vm::GCConfig::Builder()
                              .withInitHeapSize(1 << 16)
                              .withMaxHeapSize(1 << 19)
                              .build())
                      .build();
    rt_ = vm::Runtime::create(config);
    env_ = hermes_napi_create_env(&*rt_);
  }

  void TearDown() override {
    // The env is owned by the Runtime and torn down as part of
    // ~Runtime. Drop the borrowed pointer and reset the runtime.
    env_ = nullptr;
    rt_.reset();
  }
};

/// Test loading a non-existent file. Should set a pending exception.
TEST_F(NapiModuleLoadTest, LoadNonExistent) {
  napi_handle_scope scope;
  napi_open_handle_scope(env_, &scope);

  napi_value result;
  napi_status status =
      hermes_napi_load_module(env_, "/nonexistent/path/addon.node", &result);
  ASSERT_NE(status, napi_ok);

  // Clear the pending exception so the env can be destroyed cleanly.
  env_->pendingException = vm::HermesValue::encodeUndefinedValue();
  env_->hasPendingException = false;

  napi_close_handle_scope(env_, scope);
}

/// Test loading an invalid file (not a shared library).
TEST_F(NapiModuleLoadTest, LoadInvalidFile) {
  napi_handle_scope scope;
  napi_open_handle_scope(env_, &scope);

  napi_value result;
  napi_status status = hermes_napi_load_module(env_, "/dev/null", &result);
  ASSERT_NE(status, napi_ok);

  env_->pendingException = vm::HermesValue::encodeUndefinedValue();
  env_->hasPendingException = false;

  napi_close_handle_scope(env_, scope);
}

/// Helper to get the path to the test addon shared library.
/// The CMake build passes NAPI_TEST_ADDON_PATH as a compile
/// definition pointing to the test addon .node file.
static std::string getTestAddonPath() {
  return NAPI_TEST_ADDON_PATH;
}

/// Test loading the test addon shared library.
TEST_F(NapiModuleLoadTest, LoadTestAddon) {
  std::string addonPath = getTestAddonPath();

  napi_handle_scope scope;
  napi_open_handle_scope(env_, &scope);

  // Load the module. This should succeed.
  napi_value napiResult;
  napi_status status =
      hermes_napi_load_module(env_, addonPath.c_str(), &napiResult);
  ASSERT_EQ(status, napi_ok);

  // The result should be an object (the exports).
  auto *phv = reinterpret_cast<vm::PinnedHermesValue *>(napiResult);
  vm::HermesValue hv = *phv;
  ASSERT_TRUE(hv.isObject());

  // Verify the "hello" property is set to "world".
  {
    struct : public vm::Locals {
      vm::PinnedValue<vm::JSObject> obj;
    } lv;
    vm::LocalsRAII lraii(*rt_, &lv);
    lv.obj = vm::vmcast<vm::JSObject>(hv);
    vm::Handle<vm::JSObject> objHandle = lv.obj;

    // Look up "hello".
    auto symRes =
        rt_->getIdentifierTable().getSymbolID(*rt_, vm::ASCIIRef("hello", 5));
    ASSERT_EQ(symRes.getStatus(), vm::ExecutionStatus::RETURNED);

    auto propRes = vm::JSObject::getNamed_RJS(objHandle, *rt_, *symRes);
    ASSERT_EQ(propRes.getStatus(), vm::ExecutionStatus::RETURNED);

    vm::HermesValue propVal = propRes->get();
    ASSERT_TRUE(propVal.isString());

    auto *str = vm::vmcast<vm::StringPrimitive>(propVal);
    ASSERT_TRUE(str->isASCII());
    auto ref = str->getStringRef<char>();
    EXPECT_EQ(
        llvh::StringRef(ref.data(), ref.size()), llvh::StringRef("world"));
  }

  napi_close_handle_scope(env_, scope);
}

} // namespace napi
} // namespace hermes
