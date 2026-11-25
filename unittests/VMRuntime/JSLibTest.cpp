/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"

#include "hermes/BCGen/HBC/SimpleBytecodeBuilder.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSLib/Sorting.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/SmallXString.h"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace hermes::vm;
using namespace hermes::hbc;

namespace {

using JSLibTest = RuntimeTestFixture;

TEST_F(JSLibTest, globalObjectConstTest) {
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  GET_GLOBAL(NaN);
  EXPECT_TRUE(isSameValue(
      propRes->get(),
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::quiet_NaN())));

  GET_GLOBAL(Infinity);
  EXPECT_TRUE(isSameValue(
      propRes->get(),
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::infinity())));

  GET_GLOBAL(undefined);
  EXPECT_TRUE(isSameValue(propRes->get(), HermesValue::encodeUndefinedValue()));
}

TEST_F(JSLibTest, CreateObjectTest) {
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};
  // Object constructor.
  GET_GLOBAL(Object);
  auto objectCons = runtime.makeHandle<Callable>(std::move(*propRes));

  // Object.prototype.
  GET_VALUE(objectCons, prototype);
  auto prototype = runtime.makeHandle<JSObject>(std::move(*propRes));

  // NativeConstructor take undefined `this`.
  auto crtRes =
      Callable::createThisForConstruct_RJS(objectCons, runtime, objectCons);
  // Call the constructor.
  ASSERT_RETURNED(crtRes.getStatus());
  EXPECT_TRUE(crtRes->getHermesValue().isUndefined());

  auto callRes = Callable::executeCall0(
      objectCons, runtime, Runtime::getUndefinedValue(), true);
  ASSERT_RETURNED(crtRes.getStatus());
  auto newObj = runtime.makeHandle<JSObject>(std::move(*callRes));

  // Make sure the prototype is correct.
  ASSERT_EQ(prototype.get(), newObj->getParent(runtime));
}

static Handle<JSObject> createObject(Runtime &runtime) {
  // Object constructor.
  auto propRes = JSObject::getNamed_RJS(
      runtime.getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::Object));
  assert(propRes == ExecutionStatus::RETURNED);
  auto objectCons = runtime.makeHandle<Callable>(std::move(*propRes));
  auto callRes = Callable::executeCall0(
      objectCons, runtime, Runtime::getUndefinedValue(), true);
  assert(callRes == ExecutionStatus::RETURNED);
  return runtime.makeHandle<JSObject>(std::move(*callRes));
}

TEST_F(JSLibTest, ObjectToStringTest) {
  // Check that "(new Object()).toString() is "[object Object]".
  auto obj = createObject(runtime);
  auto propRes = JSObject::getNamed_RJS(
      obj, runtime, Predefined::getSymbolID(Predefined::toString));
  ASSERT_RETURNED(propRes.getStatus());
  auto toStringFn = runtime.makeHandle<Callable>(std::move(*propRes));
  EXPECT_CALLRESULT_STRING(
      "[object Object]", toStringFn->executeCall0(toStringFn, runtime, obj));

  // Check that Object.prototype.toString.call(10) is "[object Number]".
  EXPECT_CALLRESULT_STRING(
      "[object Number]",
      toStringFn->executeCall0(
          toStringFn,
          runtime,
          runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10))));

  // Check that toStringFn.call(toStringFn) is "[object Function]".
  EXPECT_CALLRESULT_STRING(
      "[object Function]",
      toStringFn->executeCall0(toStringFn, runtime, toStringFn));

  // Check that Operations/toString does the right thing.
  EXPECT_STRINGPRIM(
      "[object Object]", toString_RJS(runtime, obj)->getHermesValue());
}

TEST_F(JSLibTest, ObjectSealTest) {
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};
  auto obj = createObject(runtime);

  GET_GLOBAL(Object);
  auto objectCons = runtime.makeHandle<JSObject>((std::move(*propRes)));

  ASSERT_RETURNED(
      (propRes = JSObject::getNamed_RJS(
           objectCons, runtime, Predefined::getSymbolID(Predefined::seal)))
          .getStatus());
  auto sealFn = runtime.makeHandle<Callable>((std::move(*propRes)));

  ASSERT_RETURNED(
      (propRes = JSObject::getNamed_RJS(
           objectCons, runtime, Predefined::getSymbolID(Predefined::isSealed)))
          .getStatus());
  auto isSealedFn = runtime.makeHandle<Callable>((std::move(*propRes)));

  // Create a property "obj.prop1".
  auto prop1ID = *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"));
  ASSERT_TRUE(
      JSObject::putNamed_RJS(
          obj,
          runtime,
          *prop1ID,
          runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10))) !=
      ExecutionStatus::EXCEPTION);

  // Make sure it is configurable.
  NamedPropertyDescriptor desc;
  ASSERT_TRUE(JSObject::getNamedDescriptorUnsafe(obj, runtime, *prop1ID, desc));
  ASSERT_TRUE(desc.flags.configurable);

  // Make sure it's not sealed.
  EXPECT_CALLRESULT_BOOL(
      FALSE,
      isSealedFn->executeCall1(
          isSealedFn,
          runtime,
          runtime.makeHandle(HermesValue::encodeUndefinedValue()),
          obj.getHermesValue()));

  // obj.seal().
  ASSERT_RETURNED(
      sealFn
          ->executeCall1(
              sealFn,
              runtime,
              runtime.makeHandle(HermesValue::encodeUndefinedValue()),
              obj.getHermesValue())
          .getStatus());

  // Make sure it is no longer configurable.
  ASSERT_TRUE(JSObject::getNamedDescriptorUnsafe(obj, runtime, *prop1ID, desc));
  ASSERT_FALSE(desc.flags.configurable);

  // Try to delete it.
  auto res = JSObject::deleteNamed(obj, runtime, *prop1ID);
  ASSERT_FALSE(*res);

  // Make sure isSealed works.
  EXPECT_CALLRESULT_BOOL(
      TRUE,
      isSealedFn->executeCall1(
          isSealedFn,
          runtime,
          runtime.makeHandle(HermesValue::encodeUndefinedValue()),
          obj.getHermesValue()));
}

TEST_F(JSLibTest, ObjectFreezeTest) {
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  auto obj = createObject(runtime);

  GET_GLOBAL(Object);
  auto objectCons = runtime.makeHandle<JSObject>(std::move(*propRes));

  ASSERT_RETURNED(
      (propRes = JSObject::getNamed_RJS(
           objectCons, runtime, Predefined::getSymbolID(Predefined::freeze)))
          .getStatus());
  auto freezeFn = runtime.makeHandle<Callable>(std::move(*propRes));

  ASSERT_RETURNED(
      (propRes = JSObject::getNamed_RJS(
           objectCons, runtime, Predefined::getSymbolID(Predefined::isFrozen)))
          .getStatus());
  auto isFrozenFn = runtime.makeHandle<Callable>(std::move(*propRes));

  // Create a property "obj.prop1".
  auto prop1ID = *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"));
  ASSERT_TRUE(
      JSObject::putNamed_RJS(
          obj,
          runtime,
          *prop1ID,
          runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10))) !=
      ExecutionStatus::EXCEPTION);

  // Make sure it is configurable.
  NamedPropertyDescriptor desc;
  ASSERT_TRUE(JSObject::getNamedDescriptorUnsafe(obj, runtime, *prop1ID, desc));
  ASSERT_TRUE(desc.flags.configurable);
  ASSERT_TRUE(desc.flags.writable);

  // Make sure it's not frozen.
  EXPECT_CALLRESULT_BOOL(
      FALSE,
      isFrozenFn->executeCall1(
          isFrozenFn,
          runtime,
          runtime.makeHandle(HermesValue::encodeUndefinedValue()),
          obj.getHermesValue()));

  // obj.freeze().
  ASSERT_RETURNED(
      freezeFn
          ->executeCall1(
              freezeFn,
              runtime,
              runtime.makeHandle(HermesValue::encodeUndefinedValue()),
              obj.getHermesValue())
          .getStatus());

  // Make sure it is no longer configurable or writable.
  ASSERT_TRUE(JSObject::getNamedDescriptorUnsafe(obj, runtime, *prop1ID, desc));
  ASSERT_FALSE(desc.flags.configurable);
  ASSERT_FALSE(desc.flags.writable);

  // Try to delete it.
  auto res = JSObject::deleteNamed(obj, runtime, *prop1ID);
  ASSERT_FALSE(*res);

  // Make sure isFrozen works.
  EXPECT_CALLRESULT_BOOL(
      TRUE,
      isFrozenFn->executeCall1(
          isFrozenFn,
          runtime,
          runtime.makeHandle(HermesValue::encodeUndefinedValue()),
          obj.getHermesValue()));
}

TEST_F(JSLibTest, ObjectPreventExtensionsTest) {
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  auto obj = createObject(runtime);

  GET_GLOBAL(Object);

  auto objectCons = runtime.makeHandle<JSObject>(std::move(*propRes));

  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::preventExtensions)))
                      .getStatus());
  auto preventExtensionsFn = runtime.makeHandle<Callable>(std::move(*propRes));

  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::isExtensible)))
                      .getStatus());
  auto isExtensibleFn = runtime.makeHandle<Callable>(std::move(*propRes));

  // Make sure it's extensible.
  EXPECT_CALLRESULT_BOOL(
      TRUE,
      isExtensibleFn->executeCall1(
          isExtensibleFn,
          runtime,
          runtime.makeHandle(HermesValue::encodeUndefinedValue()),
          obj.getHermesValue()));

  // obj.preventExtensions().
  ASSERT_RETURNED(
      preventExtensionsFn
          ->executeCall1(
              preventExtensionsFn,
              runtime,
              runtime.makeHandle(HermesValue::encodeUndefinedValue()),
              obj.getHermesValue())
          .getStatus());

  // Make sure isExtensible works.
  EXPECT_CALLRESULT_BOOL(
      FALSE,
      isExtensibleFn->executeCall1(
          isExtensibleFn,
          runtime,
          runtime.makeHandle(HermesValue::encodeUndefinedValue()),
          obj.getHermesValue()));
}

TEST_F(JSLibTest, ObjectGetPrototypeOfTest) {
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};
  auto obj = createObject(runtime);

  GET_GLOBAL(Object);
  auto objectCons = runtime.makeHandle<JSObject>(std::move(*propRes));

  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::getPrototypeOf)))
                      .getStatus());
  auto getPrototypeOfFn = runtime.makeHandle<Callable>(std::move(*propRes));

  // Object.getPrototypeOf(obj).
  auto callRes = getPrototypeOfFn->executeCall1(
      getPrototypeOfFn,
      runtime,
      runtime.makeHandle(HermesValue::encodeUndefinedValue()),
      obj.getHermesValue());
  ASSERT_RETURNED(callRes.getStatus());
  auto objProto = runtime.makeHandle<JSObject>(std::move(*callRes));

  // Create a property "objProto.prop1".
  auto prop1ID = *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"));
  ASSERT_TRUE(
      JSObject::putNamed_RJS(
          objProto,
          runtime,
          *prop1ID,
          runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10))) !=
      ExecutionStatus::EXCEPTION);

  auto obj2 = createObject(runtime);

  // Object.getPrototypeOf(obj2).
  ASSERT_RETURNED((callRes = getPrototypeOfFn->executeCall1(
                       getPrototypeOfFn,
                       runtime,
                       runtime.makeHandle(HermesValue::encodeUndefinedValue()),
                       obj2.getHermesValue()))
                      .getStatus());
  auto obj2Proto = runtime.makeHandle<JSObject>(std::move(*callRes));

  // Make sure that the new object's prototype is correct.
  EXPECT_CALLRESULT_DOUBLE(
      10, JSObject::getNamed_RJS(obj2Proto, runtime, *prop1ID));
}

TEST_F(JSLibTest, ObjectGetOwnPropertyDescriptorTest) {
  GCScope scope{runtime, "JSLibTest.ObjectGetOwnPropertyDescriptorTest", 128};
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  {
    auto obj = createObject(runtime);

    GET_GLOBAL(Object);
    auto objectCons = runtime.makeHandle<JSObject>(std::move(*propRes));

    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             objectCons,
             runtime,
             Predefined::getSymbolID(Predefined::getOwnPropertyDescriptor)))
            .getStatus());
    auto getOwnPropertyDescriptorFn =
        runtime.makeHandle<Callable>(std::move(*propRes));

    // Create a property "objProto.prop1".
    auto prop1ID = *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"));
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            obj,
            runtime,
            *prop1ID,
            runtime.makeHandle(HermesValue::encodeTrustedNumberValue(10))) !=
        ExecutionStatus::EXCEPTION);

    // Object.getOwnPropertyDescriptor(obj).
    auto callRes = Callable::executeCall2(
        getOwnPropertyDescriptorFn,
        runtime,
        runtime.makeHandle(HermesValue::encodeUndefinedValue()),
        obj.getHermesValue(),
        HermesValue::encodeStringValue(
            runtime.getStringPrimFromSymbolID(*prop1ID)));
    ASSERT_RETURNED(callRes.getStatus());
    auto desc = runtime.makeHandle<JSObject>(std::move(*callRes));

    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            desc, runtime, Predefined::getSymbolID(Predefined::writable)));
    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            desc, runtime, Predefined::getSymbolID(Predefined::enumerable)));
    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            desc, runtime, Predefined::getSymbolID(Predefined::configurable)));
    EXPECT_CALLRESULT_DOUBLE(
        10,
        JSObject::getNamed_RJS(
            desc, runtime, Predefined::getSymbolID(Predefined::value)));
  }

  {
    auto obj = createObject(runtime);

    GET_GLOBAL(Object);
    auto objectCons = runtime.makeHandle<JSObject>(std::move(*propRes));

    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             objectCons,
             runtime,
             Predefined::getSymbolID(Predefined::getOwnPropertyDescriptor)))
            .getStatus());
    auto getOwnPropertyDescriptorFn =
        runtime.makeHandle<Callable>(std::move(*propRes));

    // Create a property "objProto.prop1".
    auto prop1ID = *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"));

    DefinePropertyFlags dpf{};
    dpf.setGetter = 1;
    dpf.setSetter = 1;
    dpf.setConfigurable = 1;
    dpf.configurable = 1;
    dpf.setEnumerable = 1;
    dpf.enumerable = 1;
    auto runtimeModule = RuntimeModule::createUninitialized(runtime, domain);

    SimpleBytecodeBuilder BMG;
    BytecodeInstructionGenerator BFG;
    BFG.emitLoadConstDouble(0, 18);
    BFG.emitRet(0);
    BMG.addFunction(1, 1, BFG.acquireBytecode());
    auto *codeBlock = createSimpleCodeBlock(runtimeModule, runtime, BMG);
    auto getter = runtime.makeHandle<JSFunction>(JSFunction::create(
        runtime,
        runtime.makeHandle(runtimeModule->getDomainUnsafe(runtime)),
        runtime.makeNullHandle<JSObject>(),
        runtime.makeNullHandle<Environment>(),
        codeBlock));
    auto setter = runtime.makeHandle<JSFunction>(JSFunction::create(
        runtime,
        runtime.makeHandle(runtimeModule->getDomainUnsafe(runtime)),
        runtime.makeNullHandle<JSObject>(),
        runtime.makeNullHandle<Environment>(),
        codeBlock));
    auto accessor = runtime.makeHandle<PropertyAccessor>(
        PropertyAccessor::create(runtime, getter, setter));
    ASSERT_TRUE(
        JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, accessor) !=
        ExecutionStatus::EXCEPTION);

    // Object.getOwnPropertyDescriptor(obj).
    auto callRes = Callable::executeCall2(
        getOwnPropertyDescriptorFn,
        runtime,
        runtime.makeHandle(HermesValue::encodeUndefinedValue()),
        obj.getHermesValue(),
        HermesValue::encodeStringValue(
            runtime.getStringPrimFromSymbolID(*prop1ID)));
    ASSERT_RETURNED(callRes.getStatus());
    auto desc = runtime.makeHandle<JSObject>(std::move(*callRes));

    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            desc, runtime, Predefined::getSymbolID(Predefined::enumerable)));
    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            desc, runtime, Predefined::getSymbolID(Predefined::configurable)));
    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             desc, runtime, Predefined::getSymbolID(Predefined::get)))
            .getStatus());
    EXPECT_EQ(getter.get(), (*propRes)->getPointer());
    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             desc, runtime, Predefined::getSymbolID(Predefined::set)))
            .getStatus());
    EXPECT_EQ(setter.get(), (*propRes)->getPointer());
  }
}

TEST_F(JSLibTest, ObjectDefinePropertyTest) {
  GCScope scope{runtime, "JSLibTest.ObjectDefinePropertyTest", 128};
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  // Get global object.
  GET_GLOBAL(Object);
  auto objectCons = runtime.makeHandle<JSObject>(std::move(*propRes));

  // Get Object.defineProperty() function.
  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::defineProperty),
                       PropOpFlags().plusMustExist()))
                      .getStatus());
  auto definePropertyFn = runtime.makeHandle<Callable>(std::move(*propRes));

  {
    // Create a PropertyDescriptor object with enumerable and configurable set.
    auto attributes = createObject(runtime);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            attributes,
            runtime,
            Predefined::getSymbolID(Predefined::enumerable),
            runtime.makeHandle(HermesValue::encodeBoolValue(true)),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            attributes,
            runtime,
            Predefined::getSymbolID(Predefined::configurable),
            runtime.makeHandle(HermesValue::encodeBoolValue(true)),
            PropOpFlags().plusThrowOnError())
            .getValue());

    // Add value to the PropertyDescriptor.
    auto value = HermesValue::encodeTrustedNumberValue(123);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            attributes,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            runtime.makeHandle(value),
            PropOpFlags().plusThrowOnError())
            .getValue());

    // Call Object.defineProperty() with prop.
    auto propHandle = createNoThrowForTest(runtime, createUTF16Ref(u"newkey"));
    ASSERT_RETURNED(
        definePropertyFn
            ->executeCall3(
                definePropertyFn,
                runtime,
                runtime.makeHandle(HermesValue::encodeUndefinedValue()),
                objectCons.getHermesValue(),
                propHandle.getHermesValue(),
                attributes.getHermesValue(),
                false)
            .getStatus());

    // Now fetch the property/value and verify it matches the setup.
    NamedPropertyDescriptor desc;
    auto propID = valueToSymbolIDForTest(runtime, propHandle).getValue();
    JSObject::getNamedDescriptorUnsafe(objectCons, runtime, *propID, desc);
    EXPECT_TRUE(desc.flags.enumerable);
    EXPECT_TRUE(desc.flags.configurable);
    EXPECT_FALSE(desc.flags.writable);

    EXPECT_CALLRESULT_DOUBLE(
        123,
        JSObject::getNamed_RJS(
            objectCons, runtime, *propID, PropOpFlags().plusMustExist()));
  }

  {
    // Test getter and setters in the attributes.
    // Use toString() as the setter and the getter.
    auto accessorAttributes = createObject(runtime);
    auto propRes = JSObject::getNamed_RJS(
        accessorAttributes,
        runtime,
        Predefined::getSymbolID(Predefined::toString));
    ASSERT_RETURNED(propRes.getStatus());
    auto toStringFn = runtime.makeHandle<Callable>(std::move(*propRes));
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            accessorAttributes,
            runtime,
            Predefined::getSymbolID(Predefined::set),
            toStringFn,
            PropOpFlags().plusThrowOnError())
            .getValue());

    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            accessorAttributes,
            runtime,
            Predefined::getSymbolID(Predefined::get),
            toStringFn,
            PropOpFlags().plusThrowOnError())
            .getValue());

    // Call Object.defineProperty() with prop.
    auto prop = runtime.makeHandle(
        HermesValue::encodeStringValue(
            createNoThrowForTest(runtime, createUTF16Ref(u"newkey1")).get()));
    ASSERT_RETURNED(
        definePropertyFn
            ->executeCall3(
                definePropertyFn,
                runtime,
                runtime.makeHandle(HermesValue::encodeUndefinedValue()),
                objectCons.getHermesValue(),
                prop.get(),
                accessorAttributes.getHermesValue(),
                false)
            .getStatus());

    // Now fetch the property/value and verify it matches the setup.
    NamedPropertyDescriptor desc;
    auto propID = valueToSymbolIDForTest(runtime, prop).getValue();
    JSObject::getNamedDescriptorUnsafe(objectCons, runtime, *propID, desc);
    EXPECT_TRUE(desc.flags.accessor);
    EXPECT_FALSE(desc.flags.writable);

    // Get the accessor and verify it has the correct setter and getter.
    PseudoHandle<> accessor = std::move(
        JSObject::getNamedSlotValue(objectCons, runtime, desc).getValue());
    ASSERT_TRUE(accessor->isPointer());
    ASSERT_TRUE(accessor->getPointer() != nullptr);

    auto accessorPtr =
        PseudoHandle<PropertyAccessor>::dyn_vmcast(std::move(accessor));
    ASSERT_TRUE(accessorPtr.get() != nullptr);
    EXPECT_EQ(
        accessorPtr->getter.get(runtime),
        vmcast<Callable>(toStringFn.getHermesValue()));
    EXPECT_EQ(
        accessorPtr->setter.get(runtime),
        vmcast<Callable>(toStringFn.getHermesValue()));

    // Call the getter, it should return a string.
    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             objectCons, runtime, *propID, PropOpFlags().plusMustExist()))
            .getStatus());
    EXPECT_TRUE((*propRes)->isString());
  }
}

TEST_F(JSLibTest, ObjectDefinePropertiesTest) {
  GCScope scope{runtime, "JSLibTest.ObjectDefinePropertiesTest", 128};
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  auto str1 = createNoThrowForTest(runtime, createUTF16Ref(u"key1")).get();
  auto id1 =
      valueToSymbolIDForTest(
          runtime, runtime.makeHandle(HermesValue::encodeStringValue(str1)))
          .getValue();

  auto str2 = createNoThrowForTest(runtime, createUTF16Ref(u"key2")).get();
  auto id2 =
      valueToSymbolIDForTest(
          runtime, runtime.makeHandle(HermesValue::encodeStringValue(str2)))
          .getValue();

  auto properties = createObject(runtime);

  // Create the first property descriptor object.
  {
    auto property1 = createObject(runtime);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::enumerable),
            runtime.makeHandle(HermesValue::encodeBoolValue(true)),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::configurable),
            runtime.makeHandle(HermesValue::encodeBoolValue(true)),
            PropOpFlags().plusThrowOnError())
            .getValue());
    auto value1 = HermesValue::encodeTrustedNumberValue(123);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            runtime.makeHandle(value1),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            properties,
            runtime,
            *id1,
            property1,
            PropOpFlags().plusThrowOnError())
            .getValue());
  }
  // Create the second property descriptor object.
  {
    auto property2 = createObject(runtime);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property2,
            runtime,
            Predefined::getSymbolID(Predefined::writable),
            runtime.makeHandle(HermesValue::encodeBoolValue(true)),
            PropOpFlags().plusThrowOnError())
            .getValue());
    auto value2 = HermesValue::encodeNullValue();
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property2,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            runtime.makeHandle(value2),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            properties,
            runtime,
            *id2,
            property2,
            PropOpFlags().plusThrowOnError())
            .getValue());
  }
  // Get global object.
  GET_GLOBAL(Object);
  auto objectCons = runtime.makeHandle<JSObject>(std::move(*propRes));

  // Get Object.defineProperties() function.
  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::defineProperties),
                       PropOpFlags().plusMustExist()))
                      .getStatus());
  auto definePropertiesFn = runtime.makeHandle<Callable>(std::move(*propRes));

  // Define the properties.
  auto obj = createObject(runtime);
  ASSERT_RETURNED(
      definePropertiesFn
          ->executeCall2(
              definePropertiesFn,
              runtime,
              runtime.makeHandle(HermesValue::encodeUndefinedValue()),
              obj.getHermesValue(),
              properties.getHermesValue(),
              false)
          .getStatus());

  // Verify the first property.
  {
    NamedPropertyDescriptor desc;
    JSObject::getNamedDescriptorUnsafe(obj, runtime, *id1, desc);
    EXPECT_TRUE(desc.flags.enumerable);
    EXPECT_TRUE(desc.flags.configurable);
    EXPECT_FALSE(desc.flags.writable);
    EXPECT_EQ(
        JSObject::getNamedSlotValue(obj, runtime, desc).getValue()->getDouble(),
        123);
  }

  // Verify the second property.
  {
    NamedPropertyDescriptor desc;
    JSObject::getNamedDescriptorUnsafe(obj, runtime, *id2, desc);
    EXPECT_TRUE(desc.flags.writable);
    EXPECT_FALSE(desc.flags.enumerable);
    EXPECT_FALSE(desc.flags.configurable);
    EXPECT_TRUE(
        JSObject::getNamedSlotValue(obj, runtime, desc).getValue()->isNull());
  }
}

TEST_F(JSLibTest, ObjectCreateTest) {
  GCScope scope{runtime, "JSLibTest.ObjectCreateTest", 128};
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  auto str1 = createNoThrowForTest(runtime, createUTF16Ref(u"key1")).get();
  auto id1 =
      valueToSymbolIDForTest(
          runtime, runtime.makeHandle(HermesValue::encodeStringValue(str1)))
          .getValue();

  auto str2 = createNoThrowForTest(runtime, createUTF16Ref(u"key2")).get();
  auto id2 =
      valueToSymbolIDForTest(
          runtime, runtime.makeHandle(HermesValue::encodeStringValue(str2)))
          .getValue();

  auto properties = createObject(runtime);

  // Create the first property descriptor object.
  {
    auto property1 = createObject(runtime);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::enumerable),
            runtime.makeHandle(HermesValue::encodeBoolValue(true)),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::configurable),
            runtime.makeHandle(HermesValue::encodeBoolValue(true)),
            PropOpFlags().plusThrowOnError())
            .getValue());
    auto value1 = HermesValue::encodeTrustedNumberValue(123);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            runtime.makeHandle(value1),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            properties,
            runtime,
            *id1,
            property1,
            PropOpFlags().plusThrowOnError())
            .getValue());
  }
  // Create the second property descriptor object.
  {
    auto property2 = createObject(runtime);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property2,
            runtime,
            Predefined::getSymbolID(Predefined::writable),
            runtime.makeHandle(HermesValue::encodeBoolValue(true)),
            PropOpFlags().plusThrowOnError())
            .getValue());
    auto value2 = HermesValue::encodeNullValue();
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property2,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            runtime.makeHandle(value2),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            properties,
            runtime,
            *id2,
            property2,
            PropOpFlags().plusThrowOnError())
            .getValue());
  }
  // Get global object.
  GET_GLOBAL(Object);
  auto objectCons = runtime.makeHandle<JSObject>(std::move(*propRes));

  // Get Object.create() function.
  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::create),
                       PropOpFlags().plusMustExist()))
                      .getStatus());
  auto createFn = runtime.makeHandle<Callable>(std::move(*propRes));

  // Call Object.create().
  auto prototype = createObject(runtime);
  auto callRes = createFn->executeCall2(
      createFn,
      runtime,
      runtime.makeHandle(HermesValue::encodeUndefinedValue()),
      prototype.getHermesValue(),
      properties.getHermesValue(),
      false);
  ASSERT_RETURNED(callRes.getStatus());

  auto obj = runtime.makeHandle<JSObject>(std::move(*callRes));

  // Verify the first property.
  {
    NamedPropertyDescriptor desc;
    JSObject::getNamedDescriptorUnsafe(obj, runtime, *id1, desc);
    EXPECT_TRUE(desc.flags.enumerable);
    EXPECT_TRUE(desc.flags.configurable);
    EXPECT_FALSE(desc.flags.writable);
    EXPECT_EQ(
        JSObject::getNamedSlotValue(obj, runtime, desc).getValue()->getDouble(),
        123);
  }

  // Verify the second property.
  {
    NamedPropertyDescriptor desc;
    JSObject::getNamedDescriptorUnsafe(obj, runtime, *id2, desc);
    EXPECT_TRUE(desc.flags.writable);
    EXPECT_FALSE(desc.flags.enumerable);
    EXPECT_FALSE(desc.flags.configurable);
    EXPECT_TRUE(
        JSObject::getNamedSlotValue(obj, runtime, desc).getValue()->isNull());
  }
}

TEST_F(JSLibTest, CreateStringTest) {
  GCScope scope(runtime);
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  // String constructor.
  GET_GLOBAL(String);
  auto stringCons = runtime.makeHandle<Callable>(std::move(*propRes));

  // String.prototype.
  GET_VALUE(stringCons, prototype);
  auto prototype = runtime.makeHandle<JSObject>(std::move(*propRes));

  // create a new instance.
  auto crtRes = Callable::executeCall0(
      stringCons, runtime, Runtime::getUndefinedValue(), true);
  ASSERT_RETURNED(crtRes.getStatus());
  auto newStr = runtime.makeHandle<JSObject>(std::move(*crtRes));

  // Make sure the prototype is correct.
  ASSERT_EQ(prototype.get(), newStr->getParent(runtime));
}

TEST_F(JSLibTest, SmallSortTest) {
  // Small test, with some duplication
  struct StringByLength : public SortModel {
    std::vector<std::string> v;
    StringByLength(std::vector<std::string> _v) : v(_v) {}
    ExecutionStatus swap(uint32_t a, uint32_t b) override {
      std::swap(v[a], v[b]);
      return ExecutionStatus::RETURNED;
    }
    CallResult<int> compare(uint32_t a, uint32_t b) override {
      return (int)v[a].size() - (int)v[b].size();
    }
  };
  StringByLength sbl(
      {"zero",
       "one",
       "two",
       "three",
       "four",
       "five",
       "six",
       "seven",
       "eight",
       "nine"});
  ASSERT_EQ(ExecutionStatus::RETURNED, quickSort(&sbl, 0, sbl.v.size()));
  std::vector<std::string> expected = {
      "one",
      "two",
      "six",
      "zero",
      "four",
      "five",
      "nine",
      "three",
      "seven",
      "eight"};
  EXPECT_EQ(expected, sbl.v);

  // Exhaustive test of all permutations of 7 unique elements
  std::vector<std::string> vs(7);
  for (unsigned i = 0; i < vs.size(); ++i)
    vs[i] = std::string(i, 'x');
  do {
    StringByLength sm(vs);
    ASSERT_EQ(ExecutionStatus::RETURNED, quickSort(&sm, 0, vs.size()));
    for (unsigned i = 0; i < vs.size(); ++i)
      EXPECT_EQ(i, sm.v[i].size());
  } while (std::next_permutation(vs.begin(), vs.end()));
}

TEST_F(JSLibTest, HugeSortTest) {
  // Huge random array, with duplication
  struct Uint64ByHigh32 : public SortModel {
    std::vector<uint64_t> v;
    Uint64ByHigh32(std::vector<uint64_t> _v) : v(_v) {}
    ExecutionStatus swap(uint32_t a, uint32_t b) override {
      std::swap(v[a], v[b]);
      return ExecutionStatus::RETURNED;
    }
    CallResult<int> compare(uint32_t a, uint32_t b) override {
      return ((int)(v[a] >> 32)) - ((int)(v[b] >> 32));
    }
  };
  // Make a shuffled array where each element is equivalent to 9 others.
  uint64_t size = 1000 * 1000;
  std::vector<uint64_t> v(size);
  for (uint64_t i = 0; i < size; ++i)
    v[i] = ((i / 10) << 32);
  std::shuffle(v.begin(), v.end(), std::mt19937_64());
  // Tag low bits of each element with its index before sorting.
  for (uint64_t i = 0; i < size; ++i)
    v[i] |= i;
  Uint64ByHigh32 ubh(v);
  ASSERT_EQ(ExecutionStatus::RETURNED, quickSort(&ubh, 0, ubh.v.size()));
  for (uint64_t i = 0; i < size; ++i) {
    auto cur = ubh.v[i];
    EXPECT_EQ(i / 10, cur >> 32);
    if (i > 0) {
      auto prev = ubh.v[i - 1];
      // If equivalent, then lower index should come first.
      if ((prev >> 32) == (cur >> 32)) {
        EXPECT_LT(prev & 0xffffffff, cur & 0xffffffff);
      }
    }
  }
  // Ensure sorting returns without exception even if "compare" is inconsistent.
  struct RandomLess : public SortModel {
    std::mt19937_64 rng;
    ExecutionStatus swap(uint32_t a, uint32_t b) override {
      return ExecutionStatus::RETURNED;
    }
    CallResult<int> compare(uint32_t a, uint32_t b) override {
      // -1, 0, or 1
      return ((int)(rng() % 3)) - 1;
    }
  };
  RandomLess rl;
  ASSERT_EQ(ExecutionStatus::RETURNED, quickSort(&rl, 0, 1000 * 1000));
}

class JSLibMockedEnvironmentTest : public RuntimeTestFixtureBase {
 public:
  JSLibMockedEnvironmentTest()
      : RuntimeTestFixtureBase(
            RuntimeConfig::Builder()
                .withGCConfig(kTestGCConfig)
                .withSynthTraceMode(::hermes::vm::SynthTraceMode::Tracing)
                .withEnableSampledStats(true)
                .build()) {}
};

} // anonymous namespace
