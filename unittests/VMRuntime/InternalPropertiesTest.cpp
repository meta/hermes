/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"

#include "hermes/VM/JSDate.h"

using namespace hermes::vm;
using namespace hermes::hbc;

namespace {

using InternalPropertiesTest = RuntimeTestFixture;

#define FORCE_DICTIONARY_MODE(obj)                                       \
  do {                                                                   \
    EXPECT_CALLRESULT_BOOL_RAW(                                          \
        TRUE,                                                            \
        JSObject::putNamed_RJS(                                          \
            obj,                                                         \
            runtime,                                                     \
            Predefined::getSymbolID(Predefined::Object),                 \
            HandleRootOwner::getUndefinedValue()));                      \
    EXPECT_CALLRESULT_BOOL_RAW(                                          \
        TRUE,                                                            \
        JSObject::deleteNamed(                                           \
            obj, runtime, Predefined::getSymbolID(Predefined::Object))); \
  } while (0)

TEST_F(InternalPropertiesTest, NamedInternalPropertyTest) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  lv.obj =
      JSObject::create(runtime, HandleRootOwner::makeNullHandle<JSObject>());
  Handle<JSObject> obj = lv.obj;

  // A key feature of named internal properties is that they may be added to
  // any object at any point. Let's put the object in dictionary mode to test
  // this.
  FORCE_DICTIONARY_MODE(obj);

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();

  auto propID = Predefined::getSymbolID(
      Predefined::InternalPropertyNamedPropForUnitTestOnly);

  lv.val = HermesValue::encodeTrustedNumberValue(10.0);
  EXPECT_CALLRESULT_BOOL_RAW(
      TRUE, JSObject::defineOwnProperty(obj, runtime, propID, dpf, lv.val));

  EXPECT_CALLRESULT_DOUBLE(10.0, JSObject::getNamed_RJS(obj, runtime, propID));

  // Internal properties are not exposed to the user in any way.
  auto propNames =
      JSObject::getOwnPropertyNames(obj, runtime, false /* onlyEnumerable */);
  ASSERT_RETURNED(propNames);
  ASSERT_EQ(JSArray::getLength((*propNames).get(), runtime), 0);

  auto propSymbols = JSObject::getOwnPropertySymbols(obj, runtime);
  ASSERT_RETURNED(propSymbols);
  ASSERT_EQ(JSArray::getLength((*propSymbols).get(), runtime), 0);
}

TEST_F(InternalPropertiesTest, AddInternalPropertySealedObject) {
  struct : Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.obj =
      JSObject::create(runtime, HandleRootOwner::makeNullHandle<JSObject>());
  FORCE_DICTIONARY_MODE(lv.obj);

  auto symbolId = Predefined::getSymbolID(
      Predefined::InternalPropertyNamedPropForUnitTestOnly);
  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();

  // Seal an object
  auto sealRes = JSObject::seal(lv.obj, runtime);
  ASSERT_RETURNED(sealRes);

  // Add property without `InternalForce` flag should fail
  auto defineOwnRes = JSObject::defineOwnProperty(
      lv.obj,
      runtime,
      symbolId,
      dpf,
      HandleRootOwner::getOneValue(),
      PropOpFlags());
  EXPECT_FALSE(*defineOwnRes);

  // With `InternalForce`
  defineOwnRes = JSObject::defineOwnProperty(
      lv.obj,
      runtime,
      symbolId,
      dpf,
      HandleRootOwner::getOneValue(),
      PropOpFlags().plusInternalForce());
  EXPECT_TRUE(*defineOwnRes);
  EXPECT_CALLRESULT_VALUE(
      HVConstants::kOne, JSObject::getNamed_RJS(lv.obj, runtime, symbolId));

  // Internal property should be updatable when sealed
  defineOwnRes = JSObject::defineOwnProperty(
      lv.obj,
      runtime,
      symbolId,
      dpf,
      HandleRootOwner::getNegOneValue(),
      PropOpFlags());
  EXPECT_TRUE(*defineOwnRes);
  EXPECT_CALLRESULT_VALUE(
      HVConstants::kNegOne, JSObject::getNamed_RJS(lv.obj, runtime, symbolId));
}

TEST_F(InternalPropertiesTest, AddInternalPropertyFrozenObject) {
  struct : Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.obj =
      JSObject::create(runtime, HandleRootOwner::makeNullHandle<JSObject>());
  FORCE_DICTIONARY_MODE(lv.obj);

  auto symbolId = Predefined::getSymbolID(
      Predefined::InternalPropertyNamedPropForUnitTestOnly);
  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();

  // Freeze an object
  auto freezeRes = JSObject::freeze(lv.obj, runtime);
  ASSERT_RETURNED(freezeRes);

  // Add property without `InternalForce` flag should fail
  auto defineOwnRes = JSObject::defineOwnProperty(
      lv.obj,
      runtime,
      symbolId,
      dpf,
      HandleRootOwner::getOneValue(),
      PropOpFlags());
  EXPECT_FALSE(*defineOwnRes);

  // With `InternalForce`
  defineOwnRes = JSObject::defineOwnProperty(
      lv.obj,
      runtime,
      symbolId,
      dpf,
      HandleRootOwner::getOneValue(),
      PropOpFlags().plusInternalForce());
  EXPECT_TRUE(*defineOwnRes);
  EXPECT_CALLRESULT_VALUE(
      HVConstants::kOne, JSObject::getNamed_RJS(lv.obj, runtime, symbolId));
}

TEST_F(InternalPropertiesTest, UpdateInternalPropertyFrozenObject) {
  struct : Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.obj =
      JSObject::create(runtime, HandleRootOwner::makeNullHandle<JSObject>());
  FORCE_DICTIONARY_MODE(lv.obj);

  auto symbolId = Predefined::getSymbolID(
      Predefined::InternalPropertyNamedPropForUnitTestOnly);
  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();

  auto defineOwnRes = JSObject::defineOwnProperty(
      lv.obj,
      runtime,
      symbolId,
      dpf,
      HandleRootOwner::getOneValue(),
      PropOpFlags());
  EXPECT_TRUE(*defineOwnRes);
  EXPECT_CALLRESULT_VALUE(
      HVConstants::kOne, JSObject::getNamed_RJS(lv.obj, runtime, symbolId));

  // Freeze an object
  auto freezeRes = JSObject::freeze(lv.obj, runtime);
  ASSERT_RETURNED(freezeRes);

  // Updating without `InternalForce` should fail, property not updated
  defineOwnRes = JSObject::defineOwnProperty(
      lv.obj,
      runtime,
      symbolId,
      dpf,
      HandleRootOwner::getNegOneValue(),
      PropOpFlags());
  EXPECT_FALSE(*defineOwnRes);
  EXPECT_CALLRESULT_VALUE(
      HVConstants::kOne, JSObject::getNamed_RJS(lv.obj, runtime, symbolId));

  defineOwnRes = JSObject::defineOwnProperty(
      lv.obj,
      runtime,
      symbolId,
      dpf,
      HandleRootOwner::getNegOneValue(),
      PropOpFlags().plusInternalForce());
  EXPECT_TRUE(*defineOwnRes);
  EXPECT_CALLRESULT_VALUE(
      HVConstants::kNegOne, JSObject::getNamed_RJS(lv.obj, runtime, symbolId));
}

} // anonymous namespace
