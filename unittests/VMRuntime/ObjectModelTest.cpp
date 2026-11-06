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
    RuntimeModule *runtimeModule) {
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
  return runtime.makeHandle<JSFunction>(JSFunction::create(
      runtime,
      runtimeModule->getDomain(runtime),
      Runtime::makeNullHandle<JSObject>(),
      Runtime::makeNullHandle<Environment>(),
      codeBlock));
}

static inline Handle<PropertyAccessor> createPropertyAccessor(
    Runtime &runtime,
    RuntimeModule *runtimeModule) {
  return runtime.makeHandle<PropertyAccessor>(PropertyAccessor::create(
      runtime,
      makeSimpleJSFunction(runtime, runtimeModule),
      makeSimpleJSFunction(runtime, runtimeModule)));
}

using ObjectModelTest = RuntimeTestFixture;

TEST_F(ObjectModelTest, SmokeTest) {
  CallResult<bool> cr{false};

  auto prop1ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop1"));
  auto prop2ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop2"));

  auto nullObj = runtime.makeHandle<JSObject>(nullptr);
  auto obj1 = runtime.makeHandle(JSObject::create(runtime, nullObj));

  // Try to get a property which hasn't been defined and expect undefined.
  EXPECT_CALLRESULT_UNDEFINED(JSObject::getNamed_RJS(obj1, runtime, *prop1ID));

  // Put obj1.prop1 = 3.14 .
  cr = JSObject::putNamed_RJS(
      obj1,
      runtime,
      *prop1ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(3.14)));
  ASSERT_TRUE(*cr);

  // Get obj1.prop1.
  EXPECT_CALLRESULT_DOUBLE(
      3.14, JSObject::getNamed_RJS(obj1, runtime, *prop1ID));

  // Get obj1.prop2.
  EXPECT_CALLRESULT_UNDEFINED(JSObject::getNamed_RJS(obj1, runtime, *prop2ID));

  // Set obj1.prop2 = true.
  cr = JSObject::putNamed_RJS(
      obj1,
      runtime,
      *prop2ID,
      runtime.makeHandle(HermesValue::encodeBoolValue(true)));
  ASSERT_TRUE(*cr);

  // Get obj1.prop1.
  EXPECT_CALLRESULT_DOUBLE(
      3.14, JSObject::getNamed_RJS(obj1, runtime, *prop1ID));

  // Get obj1.prop2.
  EXPECT_CALLRESULT_BOOL(TRUE, JSObject::getNamed_RJS(obj1, runtime, *prop2ID));
}

/// Non-exhaustive test of prototype functionality.
TEST_F(ObjectModelTest, SimplePrototypeTest) {
  auto prop1ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop1"));
  auto prop2ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop2"));

  // Create and populate a prototype object.
  auto nullObj = runtime.makeHandle<JSObject>(nullptr);
  auto prototypeObj = runtime.makeHandle(JSObject::create(runtime, nullObj));

  // prototypeObj.prop1 = 10;
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      prototypeObj,
      runtime,
      *prop1ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10.0))));
  // prototypeObj.prop2 = 20;
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      prototypeObj,
      runtime,
      *prop2ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(20.0))));

  // Create a child object.
  auto obj = runtime.makeHandle(JSObject::create(runtime, prototypeObj));

  // Read the inherited properties.
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
  EXPECT_CALLRESULT_DOUBLE(
      20.0, JSObject::getNamed_RJS(obj, runtime, *prop2ID));

  // obj.prop1 = 100;
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      obj,
      runtime,
      *prop1ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(100.0))));

  // Check the inherited property for the right value.
  EXPECT_CALLRESULT_DOUBLE(
      100.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
  // But make sure the prototype value didn't change.
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(prototypeObj, runtime, *prop1ID));
}

TEST_F(ObjectModelTest, DefineOwnPropertyTest) {
  GCScope gcScope{runtime, "ObjectModelTest.DefineOwnPropertyTest", 200};
  auto *runtimeModule = RuntimeModule::createUninitialized(runtime, domain);
  CallResult<bool> cr{false};

  auto prop1ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop1"));
  auto prop2ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop2"));

  auto nullObj = runtime.makeHandle<JSObject>(nullptr);

  {
    // Empty flags.
    auto obj = runtime.makeHandle(JSObject::create(runtime, nullObj));
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
    auto obj = runtime.makeHandle(JSObject::create(runtime, nullObj));
    DefinePropertyFlags dpf{};
    dpf.setValue = 1;
    dpf.setWritable = 1;
    dpf.writable = 1;
    ASSERT_TRUE(*JSObject::defineOwnProperty(
        obj,
        runtime,
        *prop1ID,
        dpf,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10.0))));
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, configurable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, accessor);
    JSObject::preventExtensionsNonProxy(obj, runtime);
    ASSERT_TRUE(*JSObject::defineOwnProperty(
        obj,
        runtime,
        *prop1ID,
        dpf,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(20.0))));
    ASSERT_FALSE(*JSObject::defineOwnProperty(
        obj,
        runtime,
        *prop2ID,
        dpf,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(20.0))));
    EXPECT_CALLRESULT_DOUBLE(
        20.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    EXPECT_CALLRESULT_UNDEFINED(JSObject::getNamed_RJS(obj, runtime, *prop2ID));
  }

  {
    // Configurable property, change writable.
    auto obj = runtime.makeHandle(JSObject::create(runtime, nullObj));
    DefinePropertyFlags dpf{};
    dpf.setValue = 1;
    dpf.setWritable = 1;
    dpf.writable = 1;
    dpf.setConfigurable = 1;
    dpf.configurable = 1;
    ASSERT_TRUE(*JSObject::defineOwnProperty(
        obj,
        runtime,
        *prop1ID,
        dpf,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10.0))));
    ASSERT_TRUE(*JSObject::putNamed_RJS(
        obj,
        runtime,
        *prop1ID,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(11.0))));
    EXPECT_CALLRESULT_DOUBLE(
        11.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    dpf.setWritable = 1;
    dpf.writable = 0;
    ASSERT_TRUE(*JSObject::defineOwnProperty(
        obj,
        runtime,
        *prop1ID,
        dpf,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(20.0))));
    ASSERT_FALSE(*JSObject::putNamed_RJS(
        obj,
        runtime,
        *prop1ID,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(31.0))));
  }

  {
    // Accessor property
    auto obj = runtime.makeHandle(JSObject::create(runtime, nullObj));
    DefinePropertyFlags dpf{};
    dpf.setGetter = 1;
    dpf.setSetter = 1;
    dpf.setConfigurable = 1;
    dpf.configurable = 1;

    auto accessor = createPropertyAccessor(runtime, runtimeModule);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, accessor));
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, configurable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, accessor);

    auto accessor2 = createPropertyAccessor(runtime, runtimeModule);
    ASSERT_TRUE(
        *JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, accessor2));
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, configurable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, accessor);
  }

  {
    // Non-configurable property.
    auto obj = runtime.makeHandle(JSObject::create(runtime, nullObj));
    DefinePropertyFlags dpf{};
    dpf.setValue = 1;
    dpf.setConfigurable = 1;
    dpf.configurable = 0;
    dpf.setWritable = 1;
    dpf.writable = 1;
    dpf.setEnumerable = 1;
    dpf.enumerable = 1;

    ASSERT_TRUE(*JSObject::defineOwnProperty(
        obj,
        runtime,
        *prop1ID,
        dpf,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10.0))));
    EXPECT_CALLRESULT_DOUBLE(
        10.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, configurable);

    dpf.writable = 0;
    ASSERT_TRUE(*JSObject::defineOwnProperty(
        obj,
        runtime,
        *prop1ID,
        dpf,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(20.0))));
    EXPECT_CALLRESULT_DOUBLE(
        20.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, writable);
    EXPECT_PROPERTY_FLAG(TRUE, obj, *prop1ID, enumerable);
    EXPECT_PROPERTY_FLAG(FALSE, obj, *prop1ID, configurable);
    dpf.writable = 1;

    dpf.enumerable = 0;
    ASSERT_FALSE(*JSObject::defineOwnProperty(
        obj,
        runtime,
        *prop1ID,
        dpf,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(20.0))));
    EXPECT_CALLRESULT_DOUBLE(
        20.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));
    dpf.enumerable = 1;

    dpf.configurable = 1;
    ASSERT_FALSE(*JSObject::defineOwnProperty(
        obj,
        runtime,
        *prop1ID,
        dpf,
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(40.0))));
    EXPECT_CALLRESULT_DOUBLE(
        20.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));

    dpf.clear();
    dpf.setGetter = 1;
    dpf.setSetter = 1;
    auto accessor = createPropertyAccessor(runtime, runtimeModule);
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
  CallResult<bool> cr(false);

  auto prop1ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop1"));
  auto prop2ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop2"));

  auto nullObj = runtime.makeHandle<JSObject>(nullptr);
  auto obj = runtime.makeHandle(JSObject::create(runtime, nullObj));

  // Define a read-only property.
  DefinePropertyFlags dpFlags1{};
  dpFlags1.setValue = 1;
  dpFlags1.setWritable = 1;
  dpFlags1.writable = 0;
  cr = JSObject::defineOwnProperty(
      obj,
      runtime,
      *prop1ID,
      dpFlags1,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10.0)));
  ASSERT_TRUE(*cr);

  // Double-check the value of obj.prop1.
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));

  // Try to modify it with doThrow=false;
  cr = JSObject::putNamed_RJS(
      obj,
      runtime,
      *prop1ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(11.0)));
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
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      obj,
      runtime,
      *prop2ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(20.0))));

  // Make prop2 read-only.
  DefinePropertyFlags dpFlags2{};
  dpFlags2.setWritable = 1;
  dpFlags2.writable = 0;
  cr = JSObject::defineOwnProperty(
      obj,
      runtime,
      *prop2ID,
      dpFlags2,
      runtime.makeHandle(HermesValue::encodeUndefinedValue()));
  ASSERT_TRUE(*cr);

  // Double-check the value of obj.prop2.
  EXPECT_CALLRESULT_DOUBLE(
      20.0, JSObject::getNamed_RJS(obj, runtime, *prop2ID));

  // Try to modify it with doThrow=false;
  cr = JSObject::putNamed_RJS(
      obj,
      runtime,
      *prop2ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(21.0)));
  ASSERT_EQ(ExecutionStatus::RETURNED, cr.getStatus());
  ASSERT_FALSE(cr.getValue());

  // Double-check the value of obj.prop2.
  EXPECT_CALLRESULT_DOUBLE(
      20.0, JSObject::getNamed_RJS(obj, runtime, *prop2ID));
}

TEST_F(ObjectModelTest, SimpleDeleteTest) {
  NamedPropertyDescriptor desc;

  auto prop1ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop1"));
  auto prop2ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop2"));
  auto prop3ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop3"));
  auto prop4ID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop4"));

  auto nullObj = runtime.makeHandle<JSObject>(nullptr);
  auto obj = runtime.makeHandle(JSObject::create(runtime, nullObj));

  // Attempt to delete a nonexistent property.
  ASSERT_TRUE(*JSObject::deleteNamed(obj, runtime, *prop1ID));

  // ob1.prop1 = 10.0;
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      obj,
      runtime,
      *prop1ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10.0))));

  // Validate the property slot.
  ASSERT_TRUE(JSObject::getOwnNamedDescriptor(obj, runtime, *prop1ID, desc));
  ASSERT_EQ(0u, desc.slot);

  // Attempt to delete a nonexistent property.
  ASSERT_TRUE(*JSObject::deleteNamed(obj, runtime, *prop2ID));

  // Make sure obj.prop1 is still there.
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(obj, runtime, *prop1ID));

  // obj.prop2 = 20.0;
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      obj,
      runtime,
      *prop2ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(20.0))));
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
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      obj,
      runtime,
      *prop3ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(30.0))));
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
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      obj,
      runtime,
      *prop4ID,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(40.0))));
  ASSERT_TRUE(JSObject::getOwnNamedDescriptor(obj, runtime, *prop4ID, desc));
  ASSERT_EQ(1u, desc.slot);
}

TEST_F(ObjectModelTest, EnvironmentSmokeTest) {
  auto nullParent = runtime.makeHandle<Environment>(nullptr);
  auto parentEnv = runtime.makeHandle<Environment>(
      Environment::create(runtime, nullParent, 2));

  ASSERT_EQ(nullptr, parentEnv->getParentEnvironment(runtime));
  ASSERT_TRUE(parentEnv->slot(0).isUndefined());
  ASSERT_TRUE(parentEnv->slot(1).isUndefined());

  parentEnv->slot(0).setNonPtr(
      HermesValue::encodeBoolValue(true), runtime.getHeap());
  ASSERT_TRUE(parentEnv->slot(0).getBool());

  // Create a child environment.
  auto env = runtime.makeHandle<Environment>(
      Environment::create(runtime, parentEnv, 2));

  ASSERT_EQ(parentEnv.get(), env->getParentEnvironment(runtime));
  ASSERT_TRUE(env->slot(0).isUndefined());
  ASSERT_TRUE(env->slot(1).isUndefined());
}

/// Test "computed" methods on a non-array object.
TEST_F(ObjectModelTest, NonArrayComputedTest) {
  struct : Locals {
    PinnedValue<SymbolID> tmpPropNameStorage;
    PinnedValue<JSObject> propObj;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope{runtime, "ObjectModelTest.NonArrayComputedTest", 128};

  auto prop1Name = StringPrimitive::createNoThrow(runtime, "prop1");
  auto prop1ID = *runtime.getIdentifierTable().getSymbolHandleFromPrimitive(
      runtime, prop1Name);
  auto prop2Name = StringPrimitive::createNoThrow(runtime, "prop2");
  auto index5 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(5));
  auto index6 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(6));

  auto value10 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10));
  auto value11 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(11));
  auto value12 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(12));

  auto nullObj = runtime.makeHandle<JSObject>(nullptr);

  auto obj1 = runtime.makeHandle(JSObject::create(runtime, nullObj));

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
  GCScope gcScope{runtime, "ObjectModelTest.NamedOrIndexed", 128};
  CallResult<bool> cr{false};

  auto nonIndexID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop1"));
  auto indexID1 = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"2"));
  auto indexID2 = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"100000000"));

  auto nullObj = runtime.makeHandle<JSObject>(nullptr);
  auto nonIndexObj = runtime.makeHandle(JSObject::create(runtime, nullObj));

  auto indexObjRes = JSArray::create(runtime, 10, 0);
  ASSERT_EQ(indexObjRes.getStatus(), ExecutionStatus::RETURNED);
  Handle<JSObject> indexObj = runtime.makeHandle(std::move(*indexObjRes));

  auto value1 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(101));
  auto value2 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(102));
  auto value3 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(103));

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
  auto nonIndexIDString = runtime.makeHandle(
      HermesValue::encodeStringValue(
          runtime.getIdentifierTable().getStringPrim(runtime, *nonIndexID)));
  auto indexId1Num =
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(2));
  auto indexId2Num =
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(100000000));
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
  GCScope gcScope{runtime, "ObjectModelTest.HasProperty", 256};

  auto nonIndexID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"prop1"));
  auto nonIndexIDString = runtime.makeHandle(
      HermesValue::encodeStringValue(
          runtime.getIdentifierTable().getStringPrim(runtime, *nonIndexID)));
  auto indexID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"5"));
  auto indexIDNum =
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(5));
  auto indexID2 = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"10"));
  auto indexID2Num =
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10));

  Handle<JSObject> self =
      runtime.makeHandle(std::move(*JSArray::create(runtime, 0, 0)));

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
  GCScope gcScope{
      runtime, "ObjectModelTest.UpdatePropertyFlagsWithoutTransitionsTest", 48};
  auto aHnd = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"a"));
  auto bHnd = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"b"));
  auto cHnd = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createUTF16Ref(u"c"));

  auto nullObj = runtime.makeHandle<JSObject>(nullptr);
  auto obj = runtime.makeHandle(JSObject::create(runtime, nullObj));
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
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto propID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createASCIIRef("prop"));
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
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto propID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createASCIIRef("prop"));
  auto otherID = *runtime.getIdentifierTable().getSymbolHandle(
      runtime, createASCIIRef("other"));
  auto val = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(1));

  enum class Op { Freeze, Seal, PreventExtensions };
  for (Op op : {Op::Freeze, Op::Seal, Op::PreventExtensions}) {
    GCScopeMarkerRAII marker{runtime};
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

} // namespace
