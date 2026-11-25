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
  struct : public Locals {
    PinnedValue<Callable> objectCons;
    PinnedValue<JSObject> prototype;
    PinnedValue<JSObject> newObj;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};
  // Object constructor.
  GET_GLOBAL(Object);
  lv.objectCons.castAndSetHermesValue<Callable>(propRes->getHermesValue());

  // Object.prototype.
  GET_VALUE(lv.objectCons, prototype);
  lv.prototype.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

  // NativeConstructor take undefined `this`.
  auto crtRes = Callable::createThisForConstruct_RJS(
      lv.objectCons, runtime, lv.objectCons);
  // Call the constructor.
  ASSERT_RETURNED(crtRes.getStatus());
  EXPECT_TRUE(crtRes->getHermesValue().isUndefined());

  auto callRes = Callable::executeCall0(
      lv.objectCons, runtime, Runtime::getUndefinedValue(), true);
  ASSERT_RETURNED(crtRes.getStatus());
  lv.newObj.castAndSetHermesValue<JSObject>(callRes->getHermesValue());

  // Make sure the prototype is correct.
  ASSERT_EQ(lv.prototype.get(), lv.newObj->getParent(runtime));
}

/// Create a new Object instance, store it in \p out and return a handle to it.
static Handle<JSObject> createObject(
    Runtime &runtime,
    MutableHandle<JSObject> out) {
  struct : public Locals {
    PinnedValue<Callable> objectCons;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  // Object constructor.
  auto propRes = JSObject::getNamed_RJS(
      runtime.getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::Object));
  assert(propRes == ExecutionStatus::RETURNED);
  lv.objectCons.castAndSetHermesValue<Callable>(propRes->getHermesValue());
  auto callRes = Callable::executeCall0(
      lv.objectCons, runtime, Runtime::getUndefinedValue(), true);
  assert(callRes == ExecutionStatus::RETURNED);
  out.castAndSetHermesValue<JSObject>(callRes->getHermesValue());
  return out;
}

TEST_F(JSLibTest, ObjectToStringTest) {
  struct : public Locals {
    PinnedValue<Callable> toStringFn;
    PinnedValue<JSObject> obj;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  // Check that "(new Object()).toString() is "[object Object]".
  auto obj = createObject(runtime, lv.obj);
  auto propRes = JSObject::getNamed_RJS(
      obj, runtime, Predefined::getSymbolID(Predefined::toString));
  ASSERT_RETURNED(propRes.getStatus());
  lv.toStringFn.castAndSetHermesValue<Callable>(propRes->getHermesValue());
  EXPECT_CALLRESULT_STRING(
      "[object Object]",
      lv.toStringFn->executeCall0(lv.toStringFn, runtime, obj));

  // Check that Object.prototype.toString.call(10) is "[object Number]".
  lv.num = HermesValue::encodeTrustedNumberValue(10);
  EXPECT_CALLRESULT_STRING(
      "[object Number]",
      lv.toStringFn->executeCall0(lv.toStringFn, runtime, lv.num));

  // Check that toStringFn.call(toStringFn) is "[object Function]".
  EXPECT_CALLRESULT_STRING(
      "[object Function]",
      lv.toStringFn->executeCall0(lv.toStringFn, runtime, lv.toStringFn));

  // Check that Operations/toString does the right thing.
  EXPECT_STRINGPRIM(
      "[object Object]", toString_RJS(runtime, obj)->getHermesValue());
}

TEST_F(JSLibTest, ObjectSealTest) {
  struct : public Locals {
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<JSObject> objectCons;
    PinnedValue<Callable> sealFn;
    PinnedValue<Callable> isSealedFn;
    PinnedValue<JSObject> obj;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};
  auto obj = createObject(runtime, lv.obj);

  GET_GLOBAL(Object);
  lv.objectCons.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

  ASSERT_RETURNED(
      (propRes = JSObject::getNamed_RJS(
           lv.objectCons, runtime, Predefined::getSymbolID(Predefined::seal)))
          .getStatus());
  lv.sealFn.castAndSetHermesValue<Callable>(propRes->getHermesValue());

  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       lv.objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::isSealed)))
                      .getStatus());
  lv.isSealedFn.castAndSetHermesValue<Callable>(propRes->getHermesValue());

  // Create a property "obj.prop1".
  auto prop1ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID);
  lv.num = HermesValue::encodeTrustedNumberValue(10);
  ASSERT_TRUE(
      JSObject::putNamed_RJS(obj, runtime, *prop1ID, lv.num) !=
      ExecutionStatus::EXCEPTION);

  // Make sure it is configurable.
  NamedPropertyDescriptor desc;
  ASSERT_TRUE(JSObject::getNamedDescriptorUnsafe(obj, runtime, *prop1ID, desc));
  ASSERT_TRUE(desc.flags.configurable);

  // Make sure it's not sealed.
  EXPECT_CALLRESULT_BOOL(
      FALSE,
      lv.isSealedFn->executeCall1(
          lv.isSealedFn,
          runtime,
          Runtime::getUndefinedValue(),
          obj.getHermesValue()));

  // obj.seal().
  ASSERT_RETURNED(lv.sealFn
                      ->executeCall1(
                          lv.sealFn,
                          runtime,
                          Runtime::getUndefinedValue(),
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
      lv.isSealedFn->executeCall1(
          lv.isSealedFn,
          runtime,
          Runtime::getUndefinedValue(),
          obj.getHermesValue()));
}

TEST_F(JSLibTest, ObjectFreezeTest) {
  struct : public Locals {
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<JSObject> objectCons;
    PinnedValue<Callable> freezeFn;
    PinnedValue<Callable> isFrozenFn;
    PinnedValue<JSObject> obj;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  auto obj = createObject(runtime, lv.obj);

  GET_GLOBAL(Object);
  lv.objectCons.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

  ASSERT_RETURNED(
      (propRes = JSObject::getNamed_RJS(
           lv.objectCons, runtime, Predefined::getSymbolID(Predefined::freeze)))
          .getStatus());
  lv.freezeFn.castAndSetHermesValue<Callable>(propRes->getHermesValue());

  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       lv.objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::isFrozen)))
                      .getStatus());
  lv.isFrozenFn.castAndSetHermesValue<Callable>(propRes->getHermesValue());

  // Create a property "obj.prop1".
  auto prop1ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID);
  lv.num = HermesValue::encodeTrustedNumberValue(10);
  ASSERT_TRUE(
      JSObject::putNamed_RJS(obj, runtime, *prop1ID, lv.num) !=
      ExecutionStatus::EXCEPTION);

  // Make sure it is configurable.
  NamedPropertyDescriptor desc;
  ASSERT_TRUE(JSObject::getNamedDescriptorUnsafe(obj, runtime, *prop1ID, desc));
  ASSERT_TRUE(desc.flags.configurable);
  ASSERT_TRUE(desc.flags.writable);

  // Make sure it's not frozen.
  EXPECT_CALLRESULT_BOOL(
      FALSE,
      lv.isFrozenFn->executeCall1(
          lv.isFrozenFn,
          runtime,
          Runtime::getUndefinedValue(),
          obj.getHermesValue()));

  // obj.freeze().
  ASSERT_RETURNED(lv.freezeFn
                      ->executeCall1(
                          lv.freezeFn,
                          runtime,
                          Runtime::getUndefinedValue(),
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
      lv.isFrozenFn->executeCall1(
          lv.isFrozenFn,
          runtime,
          Runtime::getUndefinedValue(),
          obj.getHermesValue()));
}

TEST_F(JSLibTest, ObjectPreventExtensionsTest) {
  struct : public Locals {
    PinnedValue<JSObject> objectCons;
    PinnedValue<Callable> preventExtensionsFn;
    PinnedValue<Callable> isExtensibleFn;
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  auto obj = createObject(runtime, lv.obj);

  GET_GLOBAL(Object);

  lv.objectCons.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       lv.objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::preventExtensions)))
                      .getStatus());
  lv.preventExtensionsFn.castAndSetHermesValue<Callable>(
      propRes->getHermesValue());

  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       lv.objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::isExtensible)))
                      .getStatus());
  lv.isExtensibleFn.castAndSetHermesValue<Callable>(propRes->getHermesValue());

  // Make sure it's extensible.
  EXPECT_CALLRESULT_BOOL(
      TRUE,
      lv.isExtensibleFn->executeCall1(
          lv.isExtensibleFn,
          runtime,
          Runtime::getUndefinedValue(),
          obj.getHermesValue()));

  // obj.preventExtensions().
  ASSERT_RETURNED(lv.preventExtensionsFn
                      ->executeCall1(
                          lv.preventExtensionsFn,
                          runtime,
                          Runtime::getUndefinedValue(),
                          obj.getHermesValue())
                      .getStatus());

  // Make sure isExtensible works.
  EXPECT_CALLRESULT_BOOL(
      FALSE,
      lv.isExtensibleFn->executeCall1(
          lv.isExtensibleFn,
          runtime,
          Runtime::getUndefinedValue(),
          obj.getHermesValue()));
}

TEST_F(JSLibTest, ObjectGetPrototypeOfTest) {
  struct : public Locals {
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<JSObject> objectCons;
    PinnedValue<Callable> getPrototypeOfFn;
    PinnedValue<JSObject> objProto;
    PinnedValue<JSObject> obj2Proto;
    PinnedValue<JSObject> obj;
    PinnedValue<JSObject> obj2;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};
  auto obj = createObject(runtime, lv.obj);

  GET_GLOBAL(Object);
  lv.objectCons.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       lv.objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::getPrototypeOf)))
                      .getStatus());
  lv.getPrototypeOfFn.castAndSetHermesValue<Callable>(
      propRes->getHermesValue());

  // Object.getPrototypeOf(obj).
  auto callRes = lv.getPrototypeOfFn->executeCall1(
      lv.getPrototypeOfFn,
      runtime,
      Runtime::getUndefinedValue(),
      obj.getHermesValue());
  ASSERT_RETURNED(callRes.getStatus());
  lv.objProto.castAndSetHermesValue<JSObject>(callRes->getHermesValue());

  // Create a property "objProto.prop1".
  auto prop1ID =
      *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID);
  lv.num = HermesValue::encodeTrustedNumberValue(10);
  ASSERT_TRUE(
      JSObject::putNamed_RJS(lv.objProto, runtime, *prop1ID, lv.num) !=
      ExecutionStatus::EXCEPTION);

  auto obj2 = createObject(runtime, lv.obj2);

  // Object.getPrototypeOf(obj2).
  ASSERT_RETURNED((callRes = lv.getPrototypeOfFn->executeCall1(
                       lv.getPrototypeOfFn,
                       runtime,
                       Runtime::getUndefinedValue(),
                       obj2.getHermesValue()))
                      .getStatus());
  lv.obj2Proto.castAndSetHermesValue<JSObject>(callRes->getHermesValue());

  // Make sure that the new object's prototype is correct.
  EXPECT_CALLRESULT_DOUBLE(
      10, JSObject::getNamed_RJS(lv.obj2Proto, runtime, *prop1ID));
}

TEST_F(JSLibTest, ObjectGetOwnPropertyDescriptorTest) {
  struct : public Locals {
    PinnedValue<SymbolID> prop1ID;
    PinnedValue<SymbolID> prop1ID2;
    PinnedValue<Domain> domain;
    PinnedValue<JSFunction> getter;
    PinnedValue<JSFunction> setter;
    PinnedValue<PropertyAccessor> accessor;
    PinnedValue<JSObject> objectCons;
    PinnedValue<Callable> getOwnPropertyDescriptorFn;
    PinnedValue<JSObject> desc;
    PinnedValue<JSObject> obj;
    PinnedValue<> num;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope scope{runtime, "JSLibTest.ObjectGetOwnPropertyDescriptorTest", 128};
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  {
    auto obj = createObject(runtime, lv.obj);

    GET_GLOBAL(Object);
    lv.objectCons.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             lv.objectCons,
             runtime,
             Predefined::getSymbolID(Predefined::getOwnPropertyDescriptor)))
            .getStatus());
    lv.getOwnPropertyDescriptorFn.castAndSetHermesValue<Callable>(
        propRes->getHermesValue());

    // Create a property "objProto.prop1".
    auto prop1ID =
        *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID);
    lv.num = HermesValue::encodeTrustedNumberValue(10);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(obj, runtime, *prop1ID, lv.num) !=
        ExecutionStatus::EXCEPTION);

    // Object.getOwnPropertyDescriptor(obj).
    auto callRes = Callable::executeCall2(
        lv.getOwnPropertyDescriptorFn,
        runtime,
        Runtime::getUndefinedValue(),
        obj.getHermesValue(),
        HermesValue::encodeStringValue(
            runtime.getStringPrimFromSymbolID(*prop1ID)));
    ASSERT_RETURNED(callRes.getStatus());
    lv.desc.castAndSetHermesValue<JSObject>(callRes->getHermesValue());

    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            lv.desc, runtime, Predefined::getSymbolID(Predefined::writable)));
    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            lv.desc, runtime, Predefined::getSymbolID(Predefined::enumerable)));
    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            lv.desc,
            runtime,
            Predefined::getSymbolID(Predefined::configurable)));
    EXPECT_CALLRESULT_DOUBLE(
        10,
        JSObject::getNamed_RJS(
            lv.desc, runtime, Predefined::getSymbolID(Predefined::value)));
  }

  {
    auto obj = createObject(runtime, lv.obj);

    GET_GLOBAL(Object);
    lv.objectCons.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             lv.objectCons,
             runtime,
             Predefined::getSymbolID(Predefined::getOwnPropertyDescriptor)))
            .getStatus());
    lv.getOwnPropertyDescriptorFn.castAndSetHermesValue<Callable>(
        propRes->getHermesValue());

    // Create a property "objProto.prop1".
    auto prop1ID =
        *getSymbolHandleForTest(runtime, createUTF16Ref(u"prop1"), lv.prop1ID2);

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
    lv.domain = runtimeModule->getDomainUnsafe(runtime);
    lv.getter = JSFunction::create(
        runtime,
        lv.domain,
        runtime.makeNullHandle<JSObject>(),
        runtime.makeNullHandle<Environment>(),
        codeBlock);
    lv.setter = JSFunction::create(
        runtime,
        lv.domain,
        runtime.makeNullHandle<JSObject>(),
        runtime.makeNullHandle<Environment>(),
        codeBlock);
    lv.accessor = PropertyAccessor::create(runtime, lv.getter, lv.setter);
    ASSERT_TRUE(
        JSObject::defineOwnProperty(obj, runtime, *prop1ID, dpf, lv.accessor) !=
        ExecutionStatus::EXCEPTION);

    // Object.getOwnPropertyDescriptor(obj).
    auto callRes = Callable::executeCall2(
        lv.getOwnPropertyDescriptorFn,
        runtime,
        Runtime::getUndefinedValue(),
        obj.getHermesValue(),
        HermesValue::encodeStringValue(
            runtime.getStringPrimFromSymbolID(*prop1ID)));
    ASSERT_RETURNED(callRes.getStatus());
    lv.desc.castAndSetHermesValue<JSObject>(callRes->getHermesValue());

    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            lv.desc, runtime, Predefined::getSymbolID(Predefined::enumerable)));
    EXPECT_CALLRESULT_BOOL(
        TRUE,
        JSObject::getNamed_RJS(
            lv.desc,
            runtime,
            Predefined::getSymbolID(Predefined::configurable)));
    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             lv.desc, runtime, Predefined::getSymbolID(Predefined::get)))
            .getStatus());
    EXPECT_EQ(lv.getter.get(), (*propRes)->getPointer());
    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             lv.desc, runtime, Predefined::getSymbolID(Predefined::set)))
            .getStatus());
    EXPECT_EQ(lv.setter.get(), (*propRes)->getPointer());
  }
}

TEST_F(JSLibTest, ObjectDefinePropertyTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> propHandle;
    PinnedValue<SymbolID> propID;
    PinnedValue<StringPrimitive> tmp;
    PinnedValue<SymbolID> propID2;
    PinnedValue<JSObject> objectCons;
    PinnedValue<Callable> definePropertyFn;
    PinnedValue<Callable> toStringFn;
    PinnedValue<JSObject> attributes;
    PinnedValue<JSObject> accessorAttributes;
    PinnedValue<> value;
    PinnedValue<> prop;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope scope{runtime, "JSLibTest.ObjectDefinePropertyTest", 128};
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  // Get global object.
  GET_GLOBAL(Object);
  lv.objectCons.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

  // Get Object.defineProperty() function.
  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       lv.objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::defineProperty),
                       PropOpFlags().plusMustExist()))
                      .getStatus());
  lv.definePropertyFn.castAndSetHermesValue<Callable>(
      propRes->getHermesValue());

  {
    // Create a PropertyDescriptor object with enumerable and configurable set.
    auto attributes = createObject(runtime, lv.attributes);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            attributes,
            runtime,
            Predefined::getSymbolID(Predefined::enumerable),
            Runtime::getBoolValue(true),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            attributes,
            runtime,
            Predefined::getSymbolID(Predefined::configurable),
            Runtime::getBoolValue(true),
            PropOpFlags().plusThrowOnError())
            .getValue());

    // Add value to the PropertyDescriptor.
    lv.value = HermesValue::encodeTrustedNumberValue(123);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            attributes,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            lv.value,
            PropOpFlags().plusThrowOnError())
            .getValue());

    // Call Object.defineProperty() with prop.
    auto propHandle =
        createNoThrowForTest(runtime, createUTF16Ref(u"newkey"), lv.propHandle);
    ASSERT_RETURNED(lv.definePropertyFn
                        ->executeCall3(
                            lv.definePropertyFn,
                            runtime,
                            Runtime::getUndefinedValue(),
                            lv.objectCons.getHermesValue(),
                            propHandle.getHermesValue(),
                            attributes.getHermesValue(),
                            false)
                        .getStatus());

    // Now fetch the property/value and verify it matches the setup.
    NamedPropertyDescriptor desc;
    auto propID =
        valueToSymbolIDForTest(runtime, propHandle, lv.propID).getValue();
    JSObject::getNamedDescriptorUnsafe(lv.objectCons, runtime, *propID, desc);
    EXPECT_TRUE(desc.flags.enumerable);
    EXPECT_TRUE(desc.flags.configurable);
    EXPECT_FALSE(desc.flags.writable);

    EXPECT_CALLRESULT_DOUBLE(
        123,
        JSObject::getNamed_RJS(
            lv.objectCons, runtime, *propID, PropOpFlags().plusMustExist()));
  }

  {
    // Test getter and setters in the attributes.
    // Use toString() as the setter and the getter.
    auto accessorAttributes = createObject(runtime, lv.accessorAttributes);
    auto propRes = JSObject::getNamed_RJS(
        accessorAttributes,
        runtime,
        Predefined::getSymbolID(Predefined::toString));
    ASSERT_RETURNED(propRes.getStatus());
    lv.toStringFn.castAndSetHermesValue<Callable>(propRes->getHermesValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            accessorAttributes,
            runtime,
            Predefined::getSymbolID(Predefined::set),
            lv.toStringFn,
            PropOpFlags().plusThrowOnError())
            .getValue());

    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            accessorAttributes,
            runtime,
            Predefined::getSymbolID(Predefined::get),
            lv.toStringFn,
            PropOpFlags().plusThrowOnError())
            .getValue());

    // Call Object.defineProperty() with prop.
    lv.prop = HermesValue::encodeStringValue(
        createNoThrowForTest(runtime, createUTF16Ref(u"newkey1"), lv.tmp)
            .get());
    ASSERT_RETURNED(lv.definePropertyFn
                        ->executeCall3(
                            lv.definePropertyFn,
                            runtime,
                            Runtime::getUndefinedValue(),
                            lv.objectCons.getHermesValue(),
                            lv.prop.get(),
                            accessorAttributes.getHermesValue(),
                            false)
                        .getStatus());

    // Now fetch the property/value and verify it matches the setup.
    NamedPropertyDescriptor desc;
    auto propID =
        valueToSymbolIDForTest(runtime, lv.prop, lv.propID2).getValue();
    JSObject::getNamedDescriptorUnsafe(lv.objectCons, runtime, *propID, desc);
    EXPECT_TRUE(desc.flags.accessor);
    EXPECT_FALSE(desc.flags.writable);

    // Get the accessor and verify it has the correct setter and getter.
    PseudoHandle<> accessor = std::move(
        JSObject::getNamedSlotValue(lv.objectCons, runtime, desc).getValue());
    ASSERT_TRUE(accessor->isPointer());
    ASSERT_TRUE(accessor->getPointer() != nullptr);

    auto accessorPtr =
        PseudoHandle<PropertyAccessor>::dyn_vmcast(std::move(accessor));
    ASSERT_TRUE(accessorPtr.get() != nullptr);
    EXPECT_EQ(
        accessorPtr->getter.get(runtime),
        vmcast<Callable>(lv.toStringFn.getHermesValue()));
    EXPECT_EQ(
        accessorPtr->setter.get(runtime),
        vmcast<Callable>(lv.toStringFn.getHermesValue()));

    // Call the getter, it should return a string.
    ASSERT_RETURNED(
        (propRes = JSObject::getNamed_RJS(
             lv.objectCons, runtime, *propID, PropOpFlags().plusMustExist()))
            .getStatus());
    EXPECT_TRUE((*propRes)->isString());
  }
}

TEST_F(JSLibTest, ObjectDefinePropertiesTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str1;
    PinnedValue<SymbolID> id1;
    PinnedValue<StringPrimitive> str2;
    PinnedValue<SymbolID> id2;
    PinnedValue<JSObject> objectCons;
    PinnedValue<Callable> definePropertiesFn;
    PinnedValue<JSObject> properties;
    PinnedValue<JSObject> property1;
    PinnedValue<JSObject> property2;
    PinnedValue<JSObject> obj;
    PinnedValue<> value1;
    PinnedValue<> value2;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope scope{runtime, "JSLibTest.ObjectDefinePropertiesTest", 128};
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  createNoThrowForTest(runtime, createUTF16Ref(u"key1"), lv.str1);
  auto id1 = valueToSymbolIDForTest(runtime, lv.str1, lv.id1).getValue();

  createNoThrowForTest(runtime, createUTF16Ref(u"key2"), lv.str2);
  auto id2 = valueToSymbolIDForTest(runtime, lv.str2, lv.id2).getValue();

  auto properties = createObject(runtime, lv.properties);

  // Create the first property descriptor object.
  {
    auto property1 = createObject(runtime, lv.property1);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::enumerable),
            Runtime::getBoolValue(true),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::configurable),
            Runtime::getBoolValue(true),
            PropOpFlags().plusThrowOnError())
            .getValue());
    lv.value1 = HermesValue::encodeTrustedNumberValue(123);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            lv.value1,
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
    auto property2 = createObject(runtime, lv.property2);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property2,
            runtime,
            Predefined::getSymbolID(Predefined::writable),
            Runtime::getBoolValue(true),
            PropOpFlags().plusThrowOnError())
            .getValue());
    lv.value2 = HermesValue::encodeNullValue();
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property2,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            lv.value2,
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
  lv.objectCons.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

  // Get Object.defineProperties() function.
  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       lv.objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::defineProperties),
                       PropOpFlags().plusMustExist()))
                      .getStatus());
  lv.definePropertiesFn.castAndSetHermesValue<Callable>(
      propRes->getHermesValue());

  // Define the properties.
  auto obj = createObject(runtime, lv.obj);
  ASSERT_RETURNED(lv.definePropertiesFn
                      ->executeCall2(
                          lv.definePropertiesFn,
                          runtime,
                          Runtime::getUndefinedValue(),
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
  struct : public Locals {
    PinnedValue<StringPrimitive> str1;
    PinnedValue<SymbolID> id1;
    PinnedValue<StringPrimitive> str2;
    PinnedValue<SymbolID> id2;
    PinnedValue<JSObject> objectCons;
    PinnedValue<Callable> createFn;
    PinnedValue<JSObject> obj;
    PinnedValue<JSObject> properties;
    PinnedValue<JSObject> property1;
    PinnedValue<JSObject> property2;
    PinnedValue<JSObject> prototype;
    PinnedValue<> value1;
    PinnedValue<> value2;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope scope{runtime, "JSLibTest.ObjectCreateTest", 128};
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  createNoThrowForTest(runtime, createUTF16Ref(u"key1"), lv.str1);
  auto id1 = valueToSymbolIDForTest(runtime, lv.str1, lv.id1).getValue();

  createNoThrowForTest(runtime, createUTF16Ref(u"key2"), lv.str2);
  auto id2 = valueToSymbolIDForTest(runtime, lv.str2, lv.id2).getValue();

  auto properties = createObject(runtime, lv.properties);

  // Create the first property descriptor object.
  {
    auto property1 = createObject(runtime, lv.property1);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::enumerable),
            Runtime::getBoolValue(true),
            PropOpFlags().plusThrowOnError())
            .getValue());
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::configurable),
            Runtime::getBoolValue(true),
            PropOpFlags().plusThrowOnError())
            .getValue());
    lv.value1 = HermesValue::encodeTrustedNumberValue(123);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property1,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            lv.value1,
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
    auto property2 = createObject(runtime, lv.property2);
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property2,
            runtime,
            Predefined::getSymbolID(Predefined::writable),
            Runtime::getBoolValue(true),
            PropOpFlags().plusThrowOnError())
            .getValue());
    lv.value2 = HermesValue::encodeNullValue();
    ASSERT_TRUE(
        JSObject::putNamed_RJS(
            property2,
            runtime,
            Predefined::getSymbolID(Predefined::value),
            lv.value2,
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
  lv.objectCons.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

  // Get Object.create() function.
  ASSERT_RETURNED((propRes = JSObject::getNamed_RJS(
                       lv.objectCons,
                       runtime,
                       Predefined::getSymbolID(Predefined::create),
                       PropOpFlags().plusMustExist()))
                      .getStatus());
  lv.createFn.castAndSetHermesValue<Callable>(propRes->getHermesValue());

  // Call Object.create().
  auto prototype = createObject(runtime, lv.prototype);
  auto callRes = lv.createFn->executeCall2(
      lv.createFn,
      runtime,
      Runtime::getUndefinedValue(),
      prototype.getHermesValue(),
      properties.getHermesValue(),
      false);
  ASSERT_RETURNED(callRes.getStatus());

  lv.obj.castAndSetHermesValue<JSObject>(callRes->getHermesValue());

  // Verify the first property.
  {
    NamedPropertyDescriptor desc;
    JSObject::getNamedDescriptorUnsafe(lv.obj, runtime, *id1, desc);
    EXPECT_TRUE(desc.flags.enumerable);
    EXPECT_TRUE(desc.flags.configurable);
    EXPECT_FALSE(desc.flags.writable);
    EXPECT_EQ(
        JSObject::getNamedSlotValue(lv.obj, runtime, desc)
            .getValue()
            ->getDouble(),
        123);
  }

  // Verify the second property.
  {
    NamedPropertyDescriptor desc;
    JSObject::getNamedDescriptorUnsafe(lv.obj, runtime, *id2, desc);
    EXPECT_TRUE(desc.flags.writable);
    EXPECT_FALSE(desc.flags.enumerable);
    EXPECT_FALSE(desc.flags.configurable);
    EXPECT_TRUE(
        JSObject::getNamedSlotValue(lv.obj, runtime, desc)
            .getValue()
            ->isNull());
  }
}

TEST_F(JSLibTest, CreateStringTest) {
  struct : public Locals {
    PinnedValue<Callable> stringCons;
    PinnedValue<JSObject> prototype;
    PinnedValue<JSObject> newStr;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope scope(runtime);
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};

  // String constructor.
  GET_GLOBAL(String);
  lv.stringCons.castAndSetHermesValue<Callable>(propRes->getHermesValue());

  // String.prototype.
  GET_VALUE(lv.stringCons, prototype);
  lv.prototype.castAndSetHermesValue<JSObject>(propRes->getHermesValue());

  // create a new instance.
  auto crtRes = Callable::executeCall0(
      lv.stringCons, runtime, Runtime::getUndefinedValue(), true);
  ASSERT_RETURNED(crtRes.getStatus());
  lv.newStr.castAndSetHermesValue<JSObject>(crtRes->getHermesValue());

  // Make sure the prototype is correct.
  ASSERT_EQ(lv.prototype.get(), lv.newStr->getParent(runtime));
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
