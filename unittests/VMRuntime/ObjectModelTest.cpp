/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"

#include "hermes/BCGen/HBC/SimpleBytecodeBuilder.h"
#include "hermes/VM/FastArray.h"
#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSObject-inline.h"
#include "hermes/VM/PropertyAccessor.h"

using namespace hermes::hbc;
using namespace hermes::vm;

namespace {

/// Assert that obj.prop has flag set to val.
#define EXPECT_PROPERTY_FLAG(val, obj, prop, flag)                      \
  {                                                                     \
    NamedPropertyDescriptor desc;                                       \
    ASSERT_TRUE(                                                        \
        JSObject::getNamedDescriptorUnsafe(obj, runtime, prop, desc) != \
        nullptr);                                                       \
    EXPECT_##val(desc.flags.flag);                                      \
  }

static inline Handle<Callable> makeSimpleJSFunction(
    Runtime &runtime,
    RuntimeModule *runtimeModule,
    MutableHandle<Callable> out) {
  struct : public Locals {
    PinnedValue<Domain> domain;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  CodeBlock *codeBlock;

  if (runtimeModule->getBytecode()) {
    codeBlock = runtimeModule->getCodeBlockMayAllocate(0);
  } else {
    SimpleBytecodeBuilder builder;
    BytecodeInstructionGenerator instGen;
    instGen.emitLoadConstDoubleDirect(0, 10.0);
    instGen.emitRet(0);
    builder.addFunction(1, 1, instGen.acquireBytecode());
    codeBlock = createSimpleCodeBlock(runtimeModule, runtime, builder);
  }
  lv.domain = runtimeModule->getDomainUnsafe(runtime);
  out = JSFunction::create(
      runtime,
      lv.domain,
      Runtime::makeNullHandle<JSObject>(),
      Runtime::makeNullHandle<Environment>(),
      codeBlock);
  return out;
}

static inline Handle<PropertyAccessor> createPropertyAccessor(
    Runtime &runtime,
    RuntimeModule *runtimeModule,
    MutableHandle<PropertyAccessor> out) {
  struct : public Locals {
    PinnedValue<Callable> getter;
    PinnedValue<Callable> setter;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  makeSimpleJSFunction(runtime, runtimeModule, lv.getter);
  makeSimpleJSFunction(runtime, runtimeModule, lv.setter);
  out = PropertyAccessor::create(runtime, lv.getter, lv.setter);
  return out;
}

using ObjectModelTest = RuntimeTestFixture;

TEST_F(ObjectModelTest, SmokeTest) {
  struct : public Locals {
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<SymbolID> prop2ID;
    PinnedValue<JSObject> obj1;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  CallResult<bool> cr{false};

  auto prop1ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID);
  auto prop2ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop2"), lv.prop2ID);

  auto nullObj = Runtime::makeNullHandle<JSObject>();
  lv.obj1 = JSObject::create(runtime, nullObj);
  Handle<JSObject> obj1 = lv.obj1;

  // Try to get a property which hasn't been defined and expect undefined.
  EXPECT_CALLRESULT_UNDEFINED(JSObject::getNamed_RJS(obj1, runtime, *prop1ID));

  // Put obj1.prop1 = 3.14 .
  lv.num = HermesValue::encodeTrustedNumberValue(3.14);
  cr = JSObject::putNamed_RJS(obj1, runtime, *prop1ID, lv.num);
  ASSERT_TRUE(*cr);

  // Get obj1.prop1.
  EXPECT_CALLRESULT_DOUBLE(
      3.14, JSObject::getNamed_RJS(obj1, runtime, *prop1ID));

  // Get obj1.prop2.
  EXPECT_CALLRESULT_UNDEFINED(JSObject::getNamed_RJS(obj1, runtime, *prop2ID));

  // Set obj1.prop2 = true.
  cr = JSObject::putNamed_RJS(
      obj1, runtime, *prop2ID, Runtime::getBoolValue(true));
  ASSERT_TRUE(*cr);

  // Get obj1.prop1.
  EXPECT_CALLRESULT_DOUBLE(
      3.14, JSObject::getNamed_RJS(obj1, runtime, *prop1ID));

  // Get obj1.prop2.
  EXPECT_CALLRESULT_BOOL(TRUE, JSObject::getNamed_RJS(obj1, runtime, *prop2ID));
}

/// Non-exhaustive test of prototype functionality.
TEST_F(ObjectModelTest, SimplePrototypeTest) {
  struct : public Locals {
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<SymbolID> prop2ID;
    PinnedValue<JSObject> prototypeObj;
    PinnedValue<JSObject> obj;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  auto prop1ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID);
  auto prop2ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop2"), lv.prop2ID);

  // Create and populate a prototype object.
  auto nullObj = Runtime::makeNullHandle<JSObject>();
  lv.prototypeObj = JSObject::create(runtime, nullObj);
  Handle<JSObject> prototypeObj = lv.prototypeObj;

  // prototypeObj.prop1 = 10;
  lv.num = HermesValue::encodeTrustedNumberValue(10.0);
  ASSERT_TRUE(*JSObject::putNamed_RJS(prototypeObj, runtime, *prop1ID, lv.num));
  // prototypeObj.prop2 = 20;
  lv.num = HermesValue::encodeTrustedNumberValue(20.0);
  ASSERT_TRUE(*JSObject::putNamed_RJS(prototypeObj, runtime, *prop2ID, lv.num));

  // Create a child object.
  lv.obj = JSObject::create(runtime, prototypeObj);
  Handle<JSObject> obj = lv.obj;

  // Read the inherited properties.
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
  EXPECT_CALLRESULT_DOUBLE(
      20.0, JSObject::getNamed_RJS(obj, runtime, *prop2ID));

  // obj.prop1 = 100;
  lv.num = HermesValue::encodeTrustedNumberValue(100.0);
  ASSERT_TRUE(*JSObject::putNamed_RJS(obj, runtime, *prop1ID, lv.num));

  // Check the inherited property for the right value.
  EXPECT_CALLRESULT_DOUBLE(
      100.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
  // But make sure the prototype value didn't change.
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(prototypeObj, runtime, *prop1ID));
}

TEST_F(ObjectModelTest, DefineOwnPropertyTest) {
  struct : public Locals {
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<SymbolID> prop2ID;
    PinnedValue<JSObject> obj;
    PinnedValue<PropertyAccessor> accessor;
    PinnedValue<PropertyAccessor> accessor2;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{runtime, "ObjectModelTest.DefineOwnPropertyTest", 200};
  auto *runtimeModule = RuntimeModule::createUninitialized(runtime, domain);
  CallResult<bool> cr{false};

  auto prop1ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID);
  auto prop2ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop2"), lv.prop2ID);

  auto nullObj = Runtime::makeNullHandle<JSObject>();

  {
    // Empty flags.
    lv.obj = JSObject::create(runtime, nullObj);
    Handle<JSObject> obj = lv.obj;
    DefinePropertyFlags dpf{};
    ASSERT_TRUE(*JSObject::defineOwnProperty(
        obj, runtime, *prop1ID, dpf, Runtime::getUndefinedValue()));
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, configurable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, accessor);
  }

  {
    // Writable property, prevent extensions.
    lv.obj = JSObject::create(runtime, nullObj);
    Handle<JSObject> obj = lv.obj;
    DefinePropertyFlags dpf{};
    dpf.setValue = 1;
    dpf.setWritable = 1;
    dpf.writable = 1;
    lv.num = HermesValue::encodeTrustedNumberValue(10.0);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, lv.num));
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, configurable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, accessor);
    JSObject::preventExtensionsNonProxy(obj, runtime);
    lv.num = HermesValue::encodeTrustedNumberValue(20.0);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, lv.num));
    lv.num = HermesValue::encodeTrustedNumberValue(20.0);
    ASSERT_FALSE(
        *JSObject::defineOwnProperty(obj, runtime, *prop2ID, dpf, lv.num));
    EXPECT_CALLRESULT_DOUBLE(
        20.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    EXPECT_CALLRESULT_UNDEFINED(JSObject::getNamed_RJS(obj, runtime, *prop2ID));
  }

  {
    // Configurable property, change writable.
    lv.obj = JSObject::create(runtime, nullObj);
    Handle<JSObject> obj = lv.obj;
    DefinePropertyFlags dpf{};
    dpf.setValue = 1;
    dpf.setWritable = 1;
    dpf.writable = 1;
    dpf.setConfigurable = 1;
    dpf.configurable = 1;
    lv.num = HermesValue::encodeTrustedNumberValue(10.0);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, lv.num));
    lv.num = HermesValue::encodeTrustedNumberValue(11.0);
    ASSERT_TRUE(*JSObject::putNamed_RJS(obj, runtime, *prop1ID, lv.num));
    EXPECT_CALLRESULT_DOUBLE(
        11.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    dpf.setWritable = 1;
    dpf.writable = 0;
    lv.num = HermesValue::encodeTrustedNumberValue(20.0);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, lv.num));
    lv.num = HermesValue::encodeTrustedNumberValue(31.0);
    ASSERT_FALSE(*JSObject::putNamed_RJS(obj, runtime, *prop1ID, lv.num));
  }

  {
    // Accessor property
    lv.obj = JSObject::create(runtime, nullObj);
    Handle<JSObject> obj = lv.obj;
    DefinePropertyFlags dpf{};
    dpf.setGetter = 1;
    dpf.setSetter = 1;
    dpf.setConfigurable = 1;
    dpf.configurable = 1;

    auto accessor = createPropertyAccessor(runtime, runtimeModule, lv.accessor);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, accessor));
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, configurable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, accessor);

    auto accessor2 =
        createPropertyAccessor(runtime, runtimeModule, lv.accessor2);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, accessor2));
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, configurable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, accessor);
  }

  {
    // Non-configurable property.
    lv.obj = JSObject::create(runtime, nullObj);
    Handle<JSObject> obj = lv.obj;
    DefinePropertyFlags dpf{};
    dpf.setValue = 1;
    dpf.setConfigurable = 1;
    dpf.configurable = 0;
    dpf.setWritable = 1;
    dpf.writable = 1;
    dpf.setEnumerable = 1;
    dpf.enumerable = 1;

    lv.num = HermesValue::encodeTrustedNumberValue(10.0);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, lv.num));
    EXPECT_CALLRESULT_DOUBLE(
        10.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, configurable);

    dpf.writable = 0;
    lv.num = HermesValue::encodeTrustedNumberValue(20.0);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, lv.num));
    EXPECT_CALLRESULT_DOUBLE(
        20.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, configurable);
    dpf.writable = 1;

    dpf.enumerable = 0;
    lv.num = HermesValue::encodeTrustedNumberValue(20.0);
    ASSERT_FALSE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, lv.num));
    EXPECT_CALLRESULT_DOUBLE(
        20.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    dpf.enumerable = 1;

    dpf.configurable = 1;
    lv.num = HermesValue::encodeTrustedNumberValue(40.0);
    ASSERT_FALSE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, lv.num));
    EXPECT_CALLRESULT_DOUBLE(
        20.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));

    dpf.clear();
    dpf.setGetter = 1;
    dpf.setSetter = 1;
    auto accessor = createPropertyAccessor(runtime, runtimeModule, lv.accessor);
    ASSERT_FALSE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, accessor));

    // Change writable to true of non-configurable property.
    dpf.clear();
    dpf.setWritable = 1;
    dpf.writable = 1;
    ASSERT_FALSE(*JSObject::defineOwnProperty(
        obj, runtime, *prop1ID, dpf, Runtime::getUndefinedValue()));
  }
}

/// Non-exhaustive test of read-only property.
TEST_F(ObjectModelTest, SimpleReadOnlyTest) {
  struct : public Locals {
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<SymbolID> prop2ID;
    PinnedValue<JSObject> obj;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  CallResult<bool> cr(false);

  auto prop1ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID);
  auto prop2ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop2"), lv.prop2ID);

  auto nullObj = Runtime::makeNullHandle<JSObject>();
  lv.obj = JSObject::create(runtime, nullObj);
  Handle<JSObject> obj = lv.obj;

  // Define a read-only property.
  DefinePropertyFlags dpFlags1{};
  dpFlags1.setValue = 1;
  dpFlags1.setWritable = 1;
  dpFlags1.writable = 0;
  lv.num = HermesValue::encodeTrustedNumberValue(10.0);
  cr = JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpFlags1, lv.num);
  ASSERT_TRUE(*cr);

  // Double-check the value of obj.prop1.
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));

  // Try to modify it with doThrow=false;
  lv.num = HermesValue::encodeTrustedNumberValue(11.0);
  cr = JSObject::putNamed_RJS(obj, runtime, *prop1ID, lv.num);
  ASSERT_EQ(ExecutionStatus::RETURNED, cr.getStatus());
  ASSERT_FALSE(cr.getValue());

  // Double-check the value of obj.prop1.
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));

  // TODO: enable this when Runtime::raiseTypeError() is implemented.
  /*  // Try to modify it with doThrow=true.
    cr = Object::putNamed_RJS(obj, runtime, prop1ID,
    HermesValue::encodeTrustedNumberValue(11.0), true);
    ASSERT_EQ(ExecutionStatus::EXCEPTION, cr.getStatus());

    // Double-check the value of obj.prop1.
    ASSERT_EQ(ExecutionStatus::RETURNED, Object::getNamed_RJS(obj, runtime,
    prop1ID));
    ASSERT_EQ(10.0, runtime.getReturnedValue().getDouble());*/

  // Define an ordinary property and then change it to read-only.
  // obj.prop2 = 20;
  lv.num = HermesValue::encodeTrustedNumberValue(20.0);
  ASSERT_TRUE(*JSObject::putNamed_RJS(obj, runtime, *prop2ID, lv.num));

  // Make prop2 read-only.
  DefinePropertyFlags dpFlags2{};
  dpFlags2.setWritable = 1;
  dpFlags2.writable = 0;
  cr = JSObject::defineOwnProperty(
      obj, runtime, *prop2ID, dpFlags2, Runtime::getUndefinedValue());
  ASSERT_TRUE(*cr);

  // Double-check the value of obj.prop2.
  EXPECT_CALLRESULT_DOUBLE(
      20.0, JSObject::getNamed_RJS(obj, runtime, *prop2ID));

  // Try to modify it with doThrow=false;
  lv.num = HermesValue::encodeTrustedNumberValue(21.0);
  cr = JSObject::putNamed_RJS(obj, runtime, *prop2ID, lv.num);
  ASSERT_EQ(ExecutionStatus::RETURNED, cr.getStatus());
  ASSERT_FALSE(cr.getValue());

  // Double-check the value of obj.prop2.
  EXPECT_CALLRESULT_DOUBLE(
      20.0, JSObject::getNamed_RJS(obj, runtime, *prop2ID));
}

TEST_F(ObjectModelTest, SimpleDeleteTest) {
  struct : public Locals {
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<SymbolID> prop2ID;
    PinnedValue<SymbolID> prop3ID;
    PinnedValue<SymbolID> prop4ID;
    PinnedValue<JSObject> obj;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  NamedPropertyDescriptor desc;

  auto prop1ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID);
  auto prop2ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop2"), lv.prop2ID);
  auto prop3ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop3"), lv.prop3ID);
  auto prop4ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop4"), lv.prop4ID);

  auto nullObj = Runtime::makeNullHandle<JSObject>();
  lv.obj = JSObject::create(runtime, nullObj);
  Handle<JSObject> obj = lv.obj;

  // Attempt to delete a nonexistent property.
  ASSERT_TRUE(*JSObject::deleteNamed(obj, runtime, *prop1ID));

  // ob1.prop1 = 10.0;
  lv.num = HermesValue::encodeTrustedNumberValue(10.0);
  ASSERT_TRUE(*JSObject::putNamed_RJS(obj, runtime, *prop1ID, lv.num));

  // Validate the property slot.
  ASSERT_TRUE(JSObject::getOwnNamedDescriptor(obj, runtime, *prop1ID, desc));
  ASSERT_EQ(0u, desc.slot);

  // Attempt to delete a nonexistent property.
  ASSERT_TRUE(*JSObject::deleteNamed(obj, runtime, *prop2ID));

  // Make sure obj.prop1 is still there.
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));

  // obj.prop2 = 20.0;
  lv.num = HermesValue::encodeTrustedNumberValue(20.0);
  ASSERT_TRUE(*JSObject::putNamed_RJS(obj, runtime, *prop2ID, lv.num));
  ASSERT_TRUE(JSObject::getOwnNamedDescriptor(obj, runtime, *prop2ID, desc));
  ASSERT_EQ(1u, desc.slot);

  // Delete obj.prop1.
  ASSERT_TRUE(*JSObject::deleteNamed(obj, runtime, *prop1ID));

  // Make sure it is deleted.
  ASSERT_EQ(
      nullptr,
      JSObject::getNamedDescriptorUnsafe(obj, runtime, *prop1ID, desc));
  EXPECT_CALLRESULT_UNDEFINED(JSObject::getNamed_RJS(obj, runtime, *prop1ID));

  // Make sure obj.prop2 is still there.
  EXPECT_CALLRESULT_DOUBLE(
      20.0, JSObject::getNamed_RJS(obj, runtime, *prop2ID));
  ASSERT_TRUE(JSObject::getOwnNamedDescriptor(obj, runtime, *prop2ID, desc));
  ASSERT_EQ(1u, desc.slot);

  // obj.prop3 = 30.0;
  lv.num = HermesValue::encodeTrustedNumberValue(30.0);
  ASSERT_TRUE(*JSObject::putNamed_RJS(obj, runtime, *prop3ID, lv.num));
  ASSERT_TRUE(JSObject::getOwnNamedDescriptor(obj, runtime, *prop3ID, desc));
  ASSERT_EQ(0u, desc.slot);

  // Delete obj.prop2.
  ASSERT_TRUE(*JSObject::deleteNamed(obj, runtime, *prop2ID));

  // Make sure it is deleted.
  ASSERT_EQ(
      nullptr,
      JSObject::getNamedDescriptorUnsafe(obj, runtime, *prop2ID, desc));
  EXPECT_CALLRESULT_UNDEFINED(JSObject::getNamed_RJS(obj, runtime, *prop2ID));

  // obj.prop4 = 40.0;
  lv.num = HermesValue::encodeTrustedNumberValue(40.0);
  ASSERT_TRUE(*JSObject::putNamed_RJS(obj, runtime, *prop4ID, lv.num));
  ASSERT_TRUE(JSObject::getOwnNamedDescriptor(obj, runtime, *prop4ID, desc));
  ASSERT_EQ(1u, desc.slot);
}

TEST_F(ObjectModelTest, EnvironmentSmokeTest) {
  struct : public Locals {
    PinnedValue<Environment> parentEnv;
    PinnedValue<Environment> env;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  auto nullParent = Runtime::makeNullHandle<Environment>();
  lv.parentEnv.castAndSetHermesValue<Environment>(
      Environment::create(runtime, nullParent, 2));
  Handle<Environment> parentEnv = lv.parentEnv;

  ASSERT_EQ(nullptr, parentEnv->getParentEnvironment(runtime));
  ASSERT_TRUE(parentEnv->slot(0).isUndefined());
  ASSERT_TRUE(parentEnv->slot(1).isUndefined());

  parentEnv->slot(0).setNonPtr(
      HermesValue::encodeBoolValue(true), runtime.getHeap());
  ASSERT_TRUE(parentEnv->slot(0).getBool());

  // Create a child environment.
  lv.env.castAndSetHermesValue<Environment>(
      Environment::create(runtime, parentEnv, 2));
  Handle<Environment> env = lv.env;

  ASSERT_EQ(parentEnv.get(), env->getParentEnvironment(runtime));
  ASSERT_TRUE(env->slot(0).isUndefined());
  ASSERT_TRUE(env->slot(1).isUndefined());
}

/// Test "computed" methods on a non-array object.
TEST_F(ObjectModelTest, NonArrayComputedTest) {
  struct : Locals {
    PinnedValue<StringPrimitive> prop1Name;
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<StringPrimitive> prop2Name;
    PinnedValue<SymbolID> tmpPropNameStorage;
    PinnedValue<JSObject> propObj;
    PinnedValue<> index5;
    PinnedValue<> index6;
    PinnedValue<> value10;
    PinnedValue<> value11;
    PinnedValue<> value12;
    PinnedValue<JSObject> obj1;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope{runtime, "ObjectModelTest.NonArrayComputedTest", 128};

  auto prop1Name = createNoThrowForTest(runtime, "prop1", lv.prop1Name);
  auto prop1ID = *stringToSymbolIDForTest(runtime, prop1Name, lv.prop1ID);
  auto prop2Name = createNoThrowForTest(runtime, "prop2", lv.prop2Name);
  lv.index5 = HermesValue::encodeTrustedNumberValue(5);
  Handle<> index5 = lv.index5;
  lv.index6 = HermesValue::encodeTrustedNumberValue(6);
  Handle<> index6 = lv.index6;

  lv.value10 = HermesValue::encodeTrustedNumberValue(10);
  Handle<> value10 = lv.value10;
  lv.value11 = HermesValue::encodeTrustedNumberValue(11);
  Handle<> value11 = lv.value11;
  lv.value12 = HermesValue::encodeTrustedNumberValue(12);
  Handle<> value12 = lv.value12;

  auto nullObj = Runtime::makeNullHandle<JSObject>();

  lv.obj1 = JSObject::create(runtime, nullObj);
  Handle<JSObject> obj1 = lv.obj1;

  DefinePropertyFlags dpf{};
  dpf.setEnumerable = 1;
  dpf.enumerable = 1;
  dpf.setWritable = 1;
  dpf.writable = 1;
  dpf.setConfigurable = 1;
  dpf.configurable = 1;
  dpf.setValue = 1;

  // Define two computed properties "5" and "prop1".
  ASSERT_TRUE(
      *JSObject::defineOwnComputed(obj1, runtime, index5, dpf, value10));
  ASSERT_TRUE(
      *JSObject::defineOwnComputed(obj1, runtime, prop1Name, dpf, value11));

  // Make sure we can obtain "prop1" as a named property.
  NamedPropertyDescriptor ndesc;
  ASSERT_TRUE(
      JSObject::getNamedDescriptorUnsafe(obj1, runtime, *prop1ID, ndesc));

  // Get the two properties computed descriptors and the values using the
  // descriptors.
  ComputedPropertyDescWithSymStorage cdesc{lv.tmpPropNameStorage};
  MutableHandle<JSObject> propObjHandle{lv.propObj};
  JSObject::getComputedPrimitiveDescriptor(
      obj1, runtime, index5, propObjHandle, cdesc);
  ASSERT_TRUE(propObjHandle);
  ASSERT_FALSE(cdesc.flags.indexed);
  ASSERT_EQ(
      value10.get(),
      JSObject::getComputedSlotValue(obj1, runtime, cdesc.get())->get());
  JSObject::getComputedPrimitiveDescriptor(
      obj1, runtime, prop1Name, propObjHandle, cdesc);
  ASSERT_TRUE(propObjHandle);
  ASSERT_FALSE(cdesc.flags.indexed);
  ASSERT_EQ(
      value11.get(),
      JSObject::getComputedSlotValue(obj1, runtime, cdesc)->get());

  // Use getComputed() to obtain the values.
  EXPECT_CALLRESULT_VALUE(
      value10.get(), JSObject::getComputed_RJS(obj1, runtime, index5));
  EXPECT_CALLRESULT_VALUE(
      value11.get(), JSObject::getComputed_RJS(obj1, runtime, prop1Name));
  // Use getComputed() to obtain a missing property.
  EXPECT_CALLRESULT_VALUE(
      HermesValue::encodeUndefinedValue(),
      JSObject::getComputed_RJS(obj1, runtime, index6));

  // Use putComputed() to update a value.
  ASSERT_TRUE(*JSObject::putComputed_RJS(obj1, runtime, index5, value12));
  EXPECT_CALLRESULT_VALUE(
      value12.get(), JSObject::getComputed_RJS(obj1, runtime, index5));

  // Try to get missing properties.
  JSObject::getComputedPrimitiveDescriptor(
      obj1, runtime, index6, propObjHandle, cdesc);
  ASSERT_FALSE(propObjHandle);
  JSObject::getComputedPrimitiveDescriptor(
      obj1, runtime, prop2Name, propObjHandle, cdesc);
  ASSERT_FALSE(propObjHandle);

  // Delete a missing property.
  ASSERT_TRUE(*JSObject::deleteComputed(obj1, runtime, index6));
  ASSERT_TRUE(*JSObject::deleteComputed(obj1, runtime, prop2Name));

  // Delete existing properties.
  ASSERT_TRUE(*JSObject::deleteComputed(obj1, runtime, index5));
  JSObject::getComputedPrimitiveDescriptor(
      obj1, runtime, index5, propObjHandle, cdesc);
  ASSERT_FALSE(propObjHandle);
  ASSERT_TRUE(*JSObject::deleteComputed(obj1, runtime, prop1Name));
  JSObject::getComputedPrimitiveDescriptor(
      obj1, runtime, prop1Name, propObjHandle, cdesc);
  ASSERT_FALSE(propObjHandle);
}

/// Test putNamedOrIndexed / getNamedOrIndexed.
TEST_F(ObjectModelTest, NamedOrIndexed) {
  struct : public Locals {
    PinnedValue<SymbolID> nonIndexID;
    PinnedValue<SymbolID> indexID1;
    PinnedValue<SymbolID> indexID2;
    PinnedValue<JSObject> nonIndexObj;
    PinnedValue<> value1;
    PinnedValue<> value2;
    PinnedValue<> value3;
    PinnedValue<> indexId1Num;
    PinnedValue<> indexId2Num;
    PinnedValue<JSObject> indexObj;
    PinnedValue<> nonIndexIDString;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{runtime, "ObjectModelTest.NamedOrIndexed", 128};
  CallResult<bool> cr{false};

  auto nonIndexID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.nonIndexID);
  auto indexID1 =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"2"), lv.indexID1);
  auto indexID2 = *getSymbolHandleForTest(
      runtime, createUTF16Ref(u"100000000"), lv.indexID2);

  auto nullObj = Runtime::makeNullHandle<JSObject>();
  lv.nonIndexObj = JSObject::create(runtime, nullObj);
  Handle<JSObject> nonIndexObj = lv.nonIndexObj;

  auto indexObjRes = JSArray::create(runtime, 10, 0);
  ASSERT_EQ(indexObjRes.getStatus(), ExecutionStatus::RETURNED);
  lv.indexObj = std::move(*indexObjRes);
  Handle<JSObject> indexObj = lv.indexObj;

  lv.value1 = HermesValue::encodeTrustedNumberValue(101);
  Handle<> value1 = lv.value1;
  lv.value2 = HermesValue::encodeTrustedNumberValue(102);
  Handle<> value2 = lv.value2;
  lv.value3 = HermesValue::encodeTrustedNumberValue(103);
  Handle<> value3 = lv.value3;

  // Initially nobody should have these properties.
  EXPECT_CALLRESULT_UNDEFINED(
      JSObject::getNamedOrIndexed(nonIndexObj, runtime, *nonIndexID));
  EXPECT_CALLRESULT_UNDEFINED(
      JSObject::getNamedOrIndexed(nonIndexObj, runtime, *indexID1));
  EXPECT_CALLRESULT_UNDEFINED(
      JSObject::getNamedOrIndexed(nonIndexObj, runtime, *indexID2));

  EXPECT_CALLRESULT_UNDEFINED(
      JSObject::getNamedOrIndexed(indexObj, runtime, *nonIndexID));
  EXPECT_CALLRESULT_UNDEFINED(
      JSObject::getNamedOrIndexed(indexObj, runtime, *indexID1));
  EXPECT_CALLRESULT_UNDEFINED(
      JSObject::getNamedOrIndexed(indexObj, runtime, *indexID2));

  // Set some properties.
  cr = JSObject::putNamedOrIndexed(indexObj, runtime, *nonIndexID, value1);
  ASSERT_TRUE(*cr);
  cr = JSObject::putNamedOrIndexed(indexObj, runtime, *indexID1, value2);
  ASSERT_TRUE(*cr);
  cr = JSObject::putNamedOrIndexed(indexObj, runtime, *indexID2, value3);
  ASSERT_TRUE(*cr);
  cr = JSObject::putNamedOrIndexed(nonIndexObj, runtime, *nonIndexID, value1);
  ASSERT_TRUE(*cr);
  cr = JSObject::putNamedOrIndexed(nonIndexObj, runtime, *indexID1, value2);
  ASSERT_TRUE(*cr);
  cr = JSObject::putNamedOrIndexed(nonIndexObj, runtime, *indexID2, value3);
  ASSERT_TRUE(*cr);

  // We expect both to be available via getNamedOrIndexed.
  EXPECT_CALLRESULT_DOUBLE(
      101, JSObject::getNamedOrIndexed(indexObj, runtime, *nonIndexID));
  EXPECT_CALLRESULT_DOUBLE(
      102, JSObject::getNamedOrIndexed(indexObj, runtime, *indexID1));
  EXPECT_CALLRESULT_DOUBLE(
      103, JSObject::getNamedOrIndexed(indexObj, runtime, *indexID2));
  EXPECT_CALLRESULT_DOUBLE(
      101, JSObject::getNamedOrIndexed(nonIndexObj, runtime, *nonIndexID));
  EXPECT_CALLRESULT_DOUBLE(
      102, JSObject::getNamedOrIndexed(nonIndexObj, runtime, *indexID1));
  EXPECT_CALLRESULT_DOUBLE(
      103, JSObject::getNamedOrIndexed(nonIndexObj, runtime, *indexID2));

  // We expect getNamed to access the non-index property in both objects, and
  // the index properties in the non-indexed object.
  EXPECT_CALLRESULT_DOUBLE(
      101, JSObject::getNamed_RJS(indexObj, runtime, *nonIndexID));
  EXPECT_CALLRESULT_DOUBLE(
      101, JSObject::getNamed_RJS(nonIndexObj, runtime, *nonIndexID));
  EXPECT_CALLRESULT_DOUBLE(
      102, JSObject::getNamed_RJS(nonIndexObj, runtime, *indexID1));
  EXPECT_CALLRESULT_DOUBLE(
      103, JSObject::getNamed_RJS(nonIndexObj, runtime, *indexID2));

  // Create non-symbol versions of these symbols and then test with
  // getComputed().
  lv.nonIndexIDString = HermesValue::encodeStringValue(
      runtime.getIdentifierTable().getStringPrim(runtime, *nonIndexID));
  Handle<> nonIndexIDString = lv.nonIndexIDString;
  lv.indexId1Num = HermesValue::encodeTrustedNumberValue(2);
  Handle<> indexId1Num = lv.indexId1Num;
  lv.indexId2Num = HermesValue::encodeTrustedNumberValue(100000000);
  Handle<> indexId2Num = lv.indexId2Num;
  EXPECT_CALLRESULT_DOUBLE(
      101, JSObject::getComputed_RJS(indexObj, runtime, nonIndexIDString));
  EXPECT_CALLRESULT_DOUBLE(
      102, JSObject::getComputed_RJS(indexObj, runtime, indexId1Num));
  EXPECT_CALLRESULT_DOUBLE(
      103, JSObject::getComputed_RJS(indexObj, runtime, indexId2Num));
  EXPECT_CALLRESULT_DOUBLE(
      101, JSObject::getComputed_RJS(nonIndexObj, runtime, nonIndexIDString));
  EXPECT_CALLRESULT_DOUBLE(
      102, JSObject::getComputed_RJS(nonIndexObj, runtime, indexId1Num));
}

/// Test hasNamed / hasNamedOrIndexed / hasComputed.
TEST_F(ObjectModelTest, HasProperty) {
  struct : public Locals {
    PinnedValue<SymbolID> nonIndexID;
    PinnedValue<SymbolID> indexID;
    PinnedValue<SymbolID> indexID2;
    PinnedValue<> indexIDNum;
    PinnedValue<> indexID2Num;
    PinnedValue<> nonIndexIDString;
    PinnedValue<JSObject> self;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{runtime, "ObjectModelTest.HasProperty", 256};

  auto nonIndexID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.nonIndexID);
  lv.nonIndexIDString = HermesValue::encodeStringValue(
      runtime.getIdentifierTable().getStringPrim(runtime, *nonIndexID));
  Handle<> nonIndexIDString = lv.nonIndexIDString;
  auto indexID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"5"), lv.indexID);
  lv.indexIDNum = HermesValue::encodeTrustedNumberValue(5);
  Handle<> indexIDNum = lv.indexIDNum;
  auto indexID2 =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"10"), lv.indexID2);
  lv.indexID2Num = HermesValue::encodeTrustedNumberValue(10);
  Handle<> indexID2Num = lv.indexID2Num;

  lv.self = std::move(*JSArray::create(runtime, 0, 0));
  Handle<JSObject> self = lv.self;

  ASSERT_FALSE(*JSObject::hasComputed(self, runtime, nonIndexIDString));
  ASSERT_FALSE(*JSObject::hasComputed(self, runtime, indexIDNum));
  ASSERT_FALSE(*JSObject::hasComputed(self, runtime, indexID2Num));
  ASSERT_FALSE(*JSObject::hasNamedOrIndexed(self, runtime, *nonIndexID));
  ASSERT_FALSE(*JSObject::hasNamedOrIndexed(self, runtime, *indexID));
  ASSERT_FALSE(*JSObject::hasNamedOrIndexed(self, runtime, *indexID2));
  ASSERT_FALSE(*JSObject::hasNamed(self, runtime, *nonIndexID));

  ASSERT_TRUE(*JSObject::putNamedOrIndexed(self, runtime, *nonIndexID, self));

  ASSERT_TRUE(*JSObject::hasComputed(self, runtime, nonIndexIDString));
  ASSERT_FALSE(*JSObject::hasComputed(self, runtime, indexIDNum));
  ASSERT_FALSE(*JSObject::hasComputed(self, runtime, indexID2Num));
  ASSERT_TRUE(*JSObject::hasNamedOrIndexed(self, runtime, *nonIndexID));
  ASSERT_FALSE(*JSObject::hasNamedOrIndexed(self, runtime, *indexID));
  ASSERT_FALSE(*JSObject::hasNamedOrIndexed(self, runtime, *indexID2));
  ASSERT_TRUE(*JSObject::hasNamed(self, runtime, *nonIndexID));

  ASSERT_TRUE(*JSObject::putNamedOrIndexed(self, runtime, *indexID, self));

  ASSERT_TRUE(*JSObject::hasComputed(self, runtime, nonIndexIDString));
  ASSERT_TRUE(*JSObject::hasComputed(self, runtime, indexIDNum));
  ASSERT_FALSE(*JSObject::hasComputed(self, runtime, indexID2Num));
  ASSERT_TRUE(*JSObject::hasNamedOrIndexed(self, runtime, *nonIndexID));
  ASSERT_TRUE(*JSObject::hasNamedOrIndexed(self, runtime, *indexID));
  ASSERT_FALSE(*JSObject::hasNamedOrIndexed(self, runtime, *indexID2));
  ASSERT_TRUE(*JSObject::hasNamed(self, runtime, *nonIndexID));

  DefinePropertyFlags dpf{};
  ASSERT_TRUE(*JSObject::defineOwnComputedPrimitive(
      self, runtime, indexID2Num, dpf, Runtime::getUndefinedValue()));

  ASSERT_TRUE(*JSObject::hasComputed(self, runtime, nonIndexIDString));
  ASSERT_TRUE(*JSObject::hasComputed(self, runtime, indexIDNum));
  ASSERT_TRUE(*JSObject::hasComputed(self, runtime, indexID2Num));
  ASSERT_TRUE(*JSObject::hasNamedOrIndexed(self, runtime, *nonIndexID));
  ASSERT_TRUE(*JSObject::hasNamedOrIndexed(self, runtime, *indexID));
  ASSERT_TRUE(*JSObject::hasNamedOrIndexed(self, runtime, *indexID2));
  ASSERT_TRUE(*JSObject::hasNamed(self, runtime, *nonIndexID));
}

TEST_F(ObjectModelTest, UpdatePropertyFlagsWithoutTransitionsTest) {
  struct : public Locals {
    PinnedValue<SymbolID> aHnd;
    PinnedValue<SymbolID> bHnd;
    PinnedValue<SymbolID> cHnd;
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{
      runtime, "ObjectModelTest.UpdatePropertyFlagsWithoutTransitionsTest", 48};
  auto aHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"a"), lv.aHnd);
  auto bHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"b"), lv.bHnd);
  auto cHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"c"), lv.cHnd);

  auto nullObj = Runtime::makeNullHandle<JSObject>();
  lv.obj = JSObject::create(runtime, nullObj);
  Handle<JSObject> obj = lv.obj;
  ASSERT_TRUE(*JSObject::defineOwnProperty(
      obj,
      runtime,
      *aHnd,
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      Runtime::getUndefinedValue()));
  ASSERT_TRUE(*JSObject::defineOwnProperty(
      obj,
      runtime,
      *bHnd,
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      Runtime::getUndefinedValue()));
  ASSERT_TRUE(*JSObject::defineOwnProperty(
      obj,
      runtime,
      *cHnd,
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      Runtime::getUndefinedValue()));

  // Only freeze obj.a and obj.c.
  std::vector<SymbolID> propsToFreeze;
  propsToFreeze.push_back(*aHnd);
  propsToFreeze.push_back(*cHnd);

  PropertyFlags clearFlags;
  clearFlags.writable = 1;
  clearFlags.configurable = 1;
  PropertyFlags setFlags;

  JSObject::updatePropertyFlagsWithoutTransitions(
      obj,
      runtime,
      clearFlags,
      setFlags,
      llvh::ArrayRef<SymbolID>(propsToFreeze));
  // check each property descriptor.
  EXPECT_PROPERTY_FLAG(FALSE, obj, *aHnd, writable);
  EXPECT_PROPERTY_FLAG(FALSE, obj, *aHnd, configurable);

  EXPECT_PROPERTY_FLAG(TRUE, obj, *bHnd, writable);
  EXPECT_PROPERTY_FLAG(TRUE, obj, *bHnd, configurable);

  EXPECT_PROPERTY_FLAG(FALSE, obj, *cHnd, writable);
  EXPECT_PROPERTY_FLAG(FALSE, obj, *cHnd, configurable);

  // Freeze all properties.
  JSObject::updatePropertyFlagsWithoutTransitions(
      obj, runtime, clearFlags, setFlags, llvh::None);
  // check each property descriptor.
  EXPECT_PROPERTY_FLAG(FALSE, obj, *aHnd, writable);
  EXPECT_PROPERTY_FLAG(FALSE, obj, *aHnd, configurable);

  EXPECT_PROPERTY_FLAG(FALSE, obj, *bHnd, writable);
  EXPECT_PROPERTY_FLAG(FALSE, obj, *bHnd, configurable);

  EXPECT_PROPERTY_FLAG(FALSE, obj, *cHnd, writable);
  EXPECT_PROPERTY_FLAG(FALSE, obj, *cHnd, configurable);
}

TEST_F(ObjectModelTest, ParentCacheEpochTest) {
  auto *runtimeModule = RuntimeModule::createUninitialized(runtime, domain);

  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<JSObject> parent;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Start with setting the epoch to 1, which matches the low 24 bits the add
  // cache sees after the epoch wraps.
  runtime.testSetParentCacheEpoch(1);
  runtime.setArrayFastPathParentEpoch();
  EXPECT_TRUE(runtime.checkArrayFastPathParentEpoch());

  lv.parent = JSObject::create(runtime);
  lv.obj = JSObject::create(runtime, lv.parent);
  uint32_t idx = *runtimeModule->allocateAddCacheEntry();
  AddPropertyCacheEntry &entry = runtimeModule->getAddCacheEntry(idx);
  entry.parent = lv.obj->getParentGCPtr(runtime);
  entry.startClazz = lv.obj->getClassGCPtr();
  entry.setParentEpochAndSlot(runtime.getParentCacheEpoch(), 5);

  // Ensure that these get invalidated by incrementing.
  runtime.testSetParentCacheEpoch(AddPropertyCacheEntry::kMaxParentEpoch - 2);
  EXPECT_EQ(
      AddPropertyCacheEntry::kMaxParentEpoch - 1,
      runtime.incParentCacheEpoch());
  EXPECT_TRUE(entry.startClazz);
  EXPECT_EQ(1, runtime.incParentCacheEpoch());
  EXPECT_FALSE(entry.startClazz);
  EXPECT_FALSE(runtime.checkArrayFastPathParentEpoch());
  // The full counter did not wrap: it went from kMaxParentEpoch - 1
  // straight to kMaxParentEpoch + 2, whose low 24 bits are 1.
  EXPECT_EQ(
      (uint64_t)AddPropertyCacheEntry::kMaxParentEpoch + 2,
      runtime.getFullParentCacheEpoch());
  // The array fast path compares full counters.
  runtime.setArrayFastPathParentEpoch();
  EXPECT_TRUE(runtime.checkArrayFastPathParentEpoch());
  EXPECT_EQ(2, runtime.incParentCacheEpoch());
  EXPECT_FALSE(runtime.checkArrayFastPathParentEpoch());
}

// FastArrays must be sealed and non-extensible via their class flags.
TEST_F(ObjectModelTest, FastArrayClassSealedNonExtensible) {
  struct : public Locals {
    PinnedValue<FastArray> arr;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto arrRes = FastArray::create(runtime);
  ASSERT_EQ(ExecutionStatus::RETURNED, arrRes.getStatus());
  lv.arr = vmcast<FastArray>(*arrRes);
  auto *clazz = lv.arr->getClass(runtime);
  EXPECT_FALSE(clazz->isExtensible());
  EXPECT_TRUE(clazz->isSealed());
}

TEST_F(ObjectModelTest, IsFrozenClassFlag) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<SymbolID> propID;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto propID =
      *getSymbolHandleForTest(runtime, createASCIIRef("prop"), lv.propID);
  lv.obj = JSObject::create(runtime);

  DefinePropertyFlags dpf{};
  dpf.setValue = 1;
  dpf.setWritable = 1;
  dpf.writable = 0;
  dpf.setConfigurable = 1;
  dpf.configurable = 0;
  ASSERT_TRUE(*JSObject::defineOwnProperty(
      lv.obj, runtime, *propID, dpf, Runtime::getUndefinedValue()));
  JSObject::preventExtensionsNonProxy(lv.obj, runtime);

  // The object is already semantically frozen, but the class frozen flag is a
  // lazy cache.
  ASSERT_FALSE(lv.obj->getClass(runtime)->isFrozen());
  EXPECT_TRUE(JSObject::isFrozen(createPseudoHandle(lv.obj.get()), runtime));
  EXPECT_TRUE(lv.obj->getClass(runtime)->isFrozen());
}

// Freezing, sealing or preventing extensions on objects that share a class
// must move them all to one cached class and leave sibling objects alone.
TEST_F(ObjectModelTest, ClassFlagsTransitionShared) {
  struct : public Locals {
    PinnedValue<JSObject> a;
    PinnedValue<JSObject> b;
    PinnedValue<JSObject> sibling;
    PinnedValue<HiddenClass> baseClass;
    PinnedValue<HiddenClass> newClass;
    PinnedValue<SymbolID> propID;
    PinnedValue<SymbolID> otherID;
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto propID =
      *getSymbolHandleForTest(runtime, createASCIIRef("prop"), lv.propID);
  auto otherID =
      *getSymbolHandleForTest(runtime, createASCIIRef("other"), lv.otherID);
  lv.val = HermesValue::encodeTrustedNumberValue(1);
  Handle<> val = lv.val;

  enum class Op { Freeze, Seal, PreventExtensions };
  for (Op op : {Op::Freeze, Op::Seal, Op::PreventExtensions}) {
    PinnedValue<JSObject> *objs[] = {&lv.a, &lv.b, &lv.sibling};
    for (PinnedValue<JSObject> *obj : objs) {
      *obj = JSObject::create(runtime);
      ASSERT_TRUE(*JSObject::putNamed_RJS(*obj, runtime, *propID, val));
    }
    lv.baseClass = lv.a->getClass(runtime);
    ASSERT_EQ(*lv.baseClass, lv.b->getClass(runtime));
    ASSERT_EQ(*lv.baseClass, lv.sibling->getClass(runtime));

    for (PinnedValue<JSObject> *obj : {&lv.a, &lv.b}) {
      switch (op) {
        case Op::Freeze:
          ASSERT_EQ(ExecutionStatus::RETURNED, JSObject::freeze(*obj, runtime));
          break;
        case Op::Seal:
          ASSERT_EQ(ExecutionStatus::RETURNED, JSObject::seal(*obj, runtime));
          break;
        case Op::PreventExtensions:
          JSObject::preventExtensionsNonProxy(*obj, runtime);
          break;
      }
    }

    lv.newClass = lv.a->getClass(runtime);
    EXPECT_NE(*lv.baseClass, *lv.newClass);
    // The second object reuses the cached FlagsTransition.
    EXPECT_EQ(*lv.newClass, lv.b->getClass(runtime));
    EXPECT_FALSE(lv.newClass->isExtensible());
    EXPECT_EQ(op != Op::PreventExtensions, lv.newClass->isSealed());
    EXPECT_EQ(op == Op::Freeze, lv.newClass->isFrozen());

    // The sibling and the base class are untouched.
    EXPECT_EQ(*lv.baseClass, lv.sibling->getClass(runtime));
    EXPECT_TRUE(lv.baseClass->isExtensible());
    EXPECT_FALSE(lv.baseClass->isSealed());
    EXPECT_FALSE(lv.baseClass->isFrozen());
    EXPECT_TRUE(lv.sibling->isExtensible(runtime));
    EXPECT_FALSE(
        JSObject::isSealed(createPseudoHandle(lv.sibling.get()), runtime));
    ASSERT_TRUE(*JSObject::putNamed_RJS(lv.sibling, runtime, *otherID, val));
  }
}

// An object created with property storage longer than its class needs, as
// JSON.parse creates one from text with a duplicate key, gets properties
// through addNewOwnPropertyInSlot, as a PutById add cache hit adds them: the
// adds whose slot already exists store in place, and the next one grows the
// storage. Every value reads back. Fails if the helper appends instead of
// storing in place, on its assertion that the slot is the storage's size.
TEST_F(ObjectModelTest, AddNewOwnPropertyInSlotLongStorage) {
  // The class gets kNumBefore properties by the generic path; then kNumAdded
  // more by the helper. The storage has kNumExtra slots more than the first
  // kNumBefore properties need, so the first kNumExtra adds land in existing
  // slots and the last one grows it.
  constexpr unsigned kNumBefore = JSObject::DIRECT_PROPERTY_SLOTS + 1;
  constexpr unsigned kNumExtra = 3;
  constexpr unsigned kNumAdded = kNumExtra + 1;
  constexpr unsigned kNumNames = kNumBefore + kNumAdded;

  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<JSObject> ref;
    PinnedValue<HiddenClass> clazz;
    PinnedValue<> value;
    PinnedValue<SymbolID> names[kNumNames];
  } lv;
  LocalsRAII lraii{runtime, &lv};

  SymbolID names[kNumNames];
  for (unsigned i = 0; i < kNumNames; ++i) {
    std::string s = "p" + std::to_string(i);
    names[i] = **getSymbolHandleForTest(
        runtime, createASCIIRef(s.c_str()), lv.names[i]);
  }
  auto valueOf = [](unsigned i) {
    return HermesValue::encodeTrustedNumberValue(100 + i);
  };

  lv.obj = JSObject::create(
      runtime,
      runtime.objectPrototype,
      runtime.classJSObject,
      kNumBefore + kNumExtra);
  // ref is an ordinary object that takes every property by the generic path:
  // its classes and slots are those a real add gives.
  lv.ref = JSObject::create(runtime);
  for (unsigned i = 0; i < kNumBefore; ++i) {
    lv.value = valueOf(i);
    ASSERT_TRUE(*JSObject::putNamed_RJS(lv.obj, runtime, names[i], lv.value));
    ASSERT_TRUE(*JSObject::putNamed_RJS(lv.ref, runtime, names[i], lv.value));
  }
  ASSERT_EQ(lv.obj->getClass(runtime), lv.ref->getClass(runtime));

  for (unsigned i = kNumBefore; i < kNumNames; ++i) {
    SCOPED_TRACE(i);
    lv.value = valueOf(i);
    ASSERT_TRUE(*JSObject::putNamed_RJS(lv.ref, runtime, names[i], lv.value));
    lv.clazz = lv.ref->getClass(runtime);
    NamedPropertyDescriptor desc;
    ASSERT_TRUE(
        JSObject::getOwnNamedDescriptor(lv.ref, runtime, names[i], desc));
    ASSERT_EQ(desc.slot, i);
    SmallHermesValue shv =
        SmallHermesValue::encodeHermesValue(*lv.value, runtime);
    JSObject::addNewOwnPropertyInSlot(*lv.obj, runtime, *lv.clazz, i, shv);
    EXPECT_EQ(lv.obj->getClass(runtime), *lv.clazz);
    // Every property so far, the neighbours of the new one included.
    for (unsigned j = 0; j <= i; ++j) {
      SCOPED_TRACE(j);
      EXPECT_CALLRESULT_DOUBLE(
          100.0 + j, JSObject::getNamed_RJS(lv.obj, runtime, names[j]));
    }
  }
}

#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
using ObjectModelLargeHeapTest = LargeHeapRuntimeTestFixture;

/// An object with maxYoungGenAllocationPropCount() property slots is allocated
/// together with its property storage in a single young gen allocation.
TEST_F(ObjectModelLargeHeapTest, CreateWithMaxYoungGenAllocationPropCount) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  constexpr uint32_t propCount = JSObject::maxYoungGenAllocationPropCount();
  // The object and its property storage exactly fill the maximum young gen
  // allocation.
  EXPECT_EQ(
      cellSize<JSObject>() +
          heapAlignSize(
              PropStorage::allocationSize(
                  propCount - JSObject::DIRECT_PROPERTY_SLOTS)),
      GC::maxYoungGenAllocationSize());

  lv.obj = JSObject::create(
      runtime, runtime.objectPrototype, runtime.classJSObject, propCount);

  // Add a property, then make sure it survives a collection.
  auto lengthID = Predefined::getSymbolID(Predefined::length);
  lv.value = 1.0_hd;
  ASSERT_TRUE(*JSObject::putNamed_RJS(lv.obj, runtime, lengthID, lv.value));
  runtime.collect("test");
  EXPECT_CALLRESULT_DOUBLE(
      1.0, JSObject::getNamed_RJS(lv.obj, runtime, lengthID));
}
#endif

// tryGetOwnNamedDescriptorOfKeyFast answers only for a key that is a uniqued
// string, and then gives what the named lookup gives. Fails if the uniqued
// test is dropped (getUniqueID asserts), or if a key of another kind is
// answered.
TEST_F(ObjectModelTest, TryGetOwnNamedDescriptorOfKeyFast) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<StringPrimitive> uniquedA;
    PinnedValue<StringPrimitive> uniquedZz;
    PinnedValue<StringPrimitive> builtA;
    PinnedValue<> value;
    PinnedValue<SymbolID> a;
    PinnedValue<SymbolID> b;
    PinnedValue<SymbolID> zz;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  SymbolID a = **getSymbolHandleForTest(runtime, createASCIIRef("a"), lv.a);
  SymbolID b = **getSymbolHandleForTest(runtime, createASCIIRef("b"), lv.b);
  SymbolID zz = **getSymbolHandleForTest(runtime, createASCIIRef("zz"), lv.zz);
  lv.uniquedA = runtime.getStringPrimFromSymbolID(a);
  lv.uniquedZz = runtime.getStringPrimFromSymbolID(zz);
  lv.builtA.castAndSetHermesValue<StringPrimitive>(
      runtime.ignoreAllocationFailure(
          StringPrimitive::create(runtime, createASCIIRef("a"))));
  ASSERT_TRUE(lv.uniquedA->isUniqued());
  ASSERT_TRUE(lv.uniquedZz->isUniqued());
  ASSERT_FALSE(lv.builtA->isUniqued());

  lv.obj = JSObject::create(runtime);
  lv.value = HermesValue::encodeTrustedNumberValue(1);
  ASSERT_TRUE(*JSObject::putNamed_RJS(lv.obj, runtime, b, lv.value));
  ASSERT_TRUE(*JSObject::putNamed_RJS(lv.obj, runtime, a, lv.value));

  // The named lookup of the slow path, which also gives the class its
  // property map; the fast lookups below need one.
  NamedPropertyDescriptor expected;
  ASSERT_TRUE(JSObject::getOwnNamedDescriptor(lv.obj, runtime, a, expected));
  ASSERT_EQ(1u, expected.slot);

  // Nothing below allocates.
  NamedPropertyDescriptor desc;
  hermes::OptValue<bool> res = JSObject::tryGetOwnNamedDescriptorOfKeyFast(
      *lv.obj, runtime, lv.uniquedA.getHermesValue(), desc);
  ASSERT_TRUE(res.hasValue());
  EXPECT_TRUE(*res);
  EXPECT_EQ(expected.slot, desc.slot);
  EXPECT_EQ(expected.flags, desc.flags);

  // A name the object lacks.
  res = JSObject::tryGetOwnNamedDescriptorOfKeyFast(
      *lv.obj, runtime, lv.uniquedZz.getHermesValue(), desc);
  ASSERT_TRUE(res.hasValue());
  EXPECT_FALSE(*res);

  // The same characters in a string that is not uniqued: no answer.
  res = JSObject::tryGetOwnNamedDescriptorOfKeyFast(
      *lv.obj, runtime, lv.builtA.getHermesValue(), desc);
  EXPECT_FALSE(res.hasValue());

  // Keys that are not strings: no answer.
  res = JSObject::tryGetOwnNamedDescriptorOfKeyFast(
      *lv.obj, runtime, HermesValue::encodeTrustedNumberValue(1), desc);
  EXPECT_FALSE(res.hasValue());
  res = JSObject::tryGetOwnNamedDescriptorOfKeyFast(
      *lv.obj, runtime, HermesValue::encodeSymbolValue(a), desc);
  EXPECT_FALSE(res.hasValue());
}

} // namespace
