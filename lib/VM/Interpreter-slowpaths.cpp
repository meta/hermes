/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "vm"
#include "JSLib/JSLibInternal.h"
#include "hermes/BCGen/SerializedLiteralParser.h"
#include "hermes/Support/Statistic.h"
#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/Interpreter.h"
#include "hermes/VM/JSCallableProxy.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/Runtime-inline.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/SerializedLiteralOperations.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringPrimitiveValueDenseMapInfo-inline.h"

#include "Interpreter-internal.h"

HERMES_SLOW_STATISTIC(
    NumGetByIdCacheEvicts,
    "NumGetByIdCacheEvicts: Number of property 'read by id' cache evictions");
HERMES_SLOW_STATISTIC(
    NumGetByIdFastPaths,
    "NumGetByIdFastPaths: Number of property 'read by id' fast paths");
HERMES_SLOW_STATISTIC(
    NumGetByIdAccessor,
    "NumGetByIdAccessor: Number of property 'read by id' accessors");
HERMES_SLOW_STATISTIC(
    NumGetByIdProto,
    "NumGetByIdProto: Number of property 'read by id' in the prototype chain");
HERMES_SLOW_STATISTIC(
    NumGetByIdNotFound,
    "NumGetByIdNotFound: Number of property 'read by id' not found");
HERMES_SLOW_STATISTIC(
    NumGetByIdTransient,
    "NumGetByIdTransient: Number of property 'read by id' of non-objects");
HERMES_SLOW_STATISTIC(
    NumGetByIdSlow,
    "NumGetByIdSlow: Number of property 'read by id' slow path");

HERMES_SLOW_STATISTIC(
    NumGetByValStr,
    "NumGetByValStr: Number of GetByVal _,StringPrimitive,_");

HERMES_SLOW_STATISTIC(
    NumPutByIdCacheEvicts,
    "NumPutByIdCacheEvicts: Number of property 'write by id' cache evictions");
HERMES_SLOW_STATISTIC(
    NumPutByIdFastPaths,
    "NumPutByIdFastPaths: Number of property 'write by id' fast paths");
HERMES_SLOW_STATISTIC(
    NumPutByIdTransient,
    "NumPutByIdTransient: Number of property 'write by id' to non-objects");

HERMES_SLOW_STATISTIC(NumPutByVal, "NumPutByVal: Number of PutByVal");

namespace hermes {
namespace vm {

ExecutionStatus Interpreter::caseCreateClass(
    Runtime &runtime,
    PinnedHermesValue *frameRegs) {
  const Inst *ip = runtime.getCurrentIP();
  GCScopeMarkerRAII mark{runtime};
  /// Obtain the function index and value of the super class based on the IP.
  auto funcIdxAndSuper = [ip, frameRegs]() -> std::pair<uint32_t, Handle<>> {
    // We use toRValue here because the accesses into the ip fields will not
    // always be byte aligned. By default, the std::pair we construct would take
    // a reference to the integer field we are trying to read off of the ip. In
    // the process, the "packed" attribute is lost and it would throw an error
    // in sanitization mode when trying to read. Don't allow it to take a
    // reference by converting it to an rvalue.
    switch (ip->opCode) {
      case inst::OpCode::CreateBaseClass:
        return {toRValue(ip->iCreateBaseClass.op4), Runtime::getEmptyValue()};
      case inst::OpCode::CreateBaseClassLongIndex:
        return {
            toRValue(ip->iCreateBaseClassLongIndex.op4),
            Runtime::getEmptyValue()};
      case inst::OpCode::CreateDerivedClass:
        return {
            toRValue(ip->iCreateDerivedClass.op5),
            Handle<>{&O4REG(CreateDerivedClass)}};
      case inst::OpCode::CreateDerivedClassLongIndex:
        return {
            toRValue(ip->iCreateDerivedClassLongIndex.op5),
            Handle<>{&O4REG(CreateDerivedClassLongIndex)}};
      default:
        llvm_unreachable("called caseCreateClass on a non-CreateClass opcode");
    }
  }();

  auto classRes = createClass(
      runtime,
      funcIdxAndSuper.second,
      [&runtime, frameRegs, ip, funcIdx = funcIdxAndSuper.first](
          Handle<JSObject> ctorParent) {
        CodeBlock *curCodeBlock =
            runtime.getCurrentFrame().getCalleeCodeBlock();
        auto *runtimeModule = curCodeBlock->getRuntimeModule();

        auto envHandle = O3REG(CreateBaseClass).isUndefined()
            ? Runtime::makeNullHandle<Environment>()
            : Handle<Environment>::vmcast(&O3REG(CreateBaseClass));
        return JSClass::create(
                   runtime,
                   runtimeModule->getDomain(runtime),
                   ctorParent,
                   envHandle,
                   runtimeModule->getCodeBlockMayAllocate(funcIdx))
            .get();
      });
  if (LLVM_UNLIKELY(classRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  O2REG(CreateBaseClass) = HermesValue::encodeObjectValue(classRes->second);
  // Write the result last in case it is the same register as the prototype.
  O1REG(CreateBaseClass) = HermesValue::encodeObjectValue(classRes->first);
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseCreatePrivateName(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  CodeBlock *curCodeBlock = runtime.getCurrentFrame().getCalleeCodeBlock();
  auto *descStr = runtime.getIdentifierTable().getStringPrim(
      runtime, ID(ip->iCreatePrivateName.op2));
  struct : public Locals {
    PinnedValue<StringPrimitive> desc;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.desc = descStr;
  auto symbolRes =
      runtime.getIdentifierTable().createNotUniquedSymbol(runtime, lv.desc);
  if (LLVM_UNLIKELY(symbolRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  O1REG(CreatePrivateName) = HermesValue::encodeSymbolValue(*symbolRes);
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::casePrivateIsIn(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    CodeBlock *curCodeBlock,
    const Inst *ip) {
  if (!LLVM_LIKELY(O3REG(PrivateIsIn).isObject())) {
    return runtime.raiseTypeError("right operand of 'in' is not an object");
  }
  NamedPropertyDescriptor desc;
  auto res = JSObject::getOwnNamedDescriptor(
      Handle<JSObject>::vmcast(&O3REG(PrivateIsIn)),
      runtime,
      O2REG(PrivateIsIn).getSymbol(),
      desc);
  assert(
      (!res || desc.flags.privateName) &&
      "if a property exists here it should be a private property.");
  auto *obj = vmcast<JSObject>(O3REG(PrivateIsIn));
  auto cacheIdx = ip->iPrivateIsIn.op4;
  // We want to be able to cache negative results here, as in cache the answer
  // that an object with a given HC does not contain a property. However, in
  // dictionary mode the HC wouldn't change even if the object subsequently gets
  // the private property added to it. So we must disable caching when the HC is
  // a dictionary.
  if (cacheIdx != hbc::PROPERTY_CACHING_DISABLED &&
      !obj->getClass(runtime)->isDictionary()) {
    auto *cacheEntry = curCodeBlock->getPrivateNameCacheEntry(cacheIdx);
    cacheEntry->clazz = obj->getClassGCPtr();
    SymbolID nameSym = O2REG(PrivateIsIn).getSymbol();
    cacheEntry->nameVal = nameSym;
    cacheEntry->slot = res;
  }
  O1REG(PrivateIsIn) = HermesValue::encodeBoolValue(res);
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseDirectEval(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  auto *result = &O1REG(DirectEval);
  auto *evalText = &O2REG(DirectEval);
  bool strictCaller = ip->iDirectEval.op3;

  if (!evalText->isString()) {
    *result = *evalText;
    return ExecutionStatus::RETURNED;
  }

  GCScopeMarkerRAII gcMarker{runtime};

  auto cr = vm::directEval(
      runtime,
      Handle<StringPrimitive>::vmcast(evalText),
      strictCaller,
      nullptr,
      false);
  if (cr == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  *result = *cr;
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::defineOwnByIdSlowPath(
    Runtime &runtime,
    SmallHermesValue valueToStore,
    CodeBlock *curCodeBlock,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  uint32_t idVal;
  if (ip->opCode == inst::OpCode::DefineOwnByIdLong) {
    idVal = ip->iDefineOwnByIdLong.op4;
  } else {
    assert(
        ip->opCode == inst::OpCode::DefineOwnById &&
        "unrecognized opcode in defineOwnById slow path");
    idVal = ip->iDefineOwnById.op4;
  }
  auto *obj = vmcast<JSObject>(O1REG(DefineOwnById));
  auto cacheIdx = ip->iDefineOwnById.op3;
  auto *cacheEntry = curCodeBlock->getWriteCacheEntry(cacheIdx);
  CompressedPointer clazzPtr{obj->getClassGCPtr()};
  auto id = ID(idVal);
  NamedPropertyDescriptor desc;
  OptValue<bool> hasOwnProp =
      JSObject::tryGetOwnNamedDescriptorFast(obj, runtime, id, desc);
  if (LLVM_LIKELY(hasOwnProp.hasValue() && hasOwnProp.getValue()) &&
      !desc.flags.accessor && desc.flags.writable &&
      !desc.flags.internalSetter && desc.flags.configurable &&
      desc.flags.enumerable) {
    // We can only cache if this is not a dictionary and we have an
    // enabled cache index.
    HiddenClass *clazz = vmcast<HiddenClass>(clazzPtr.getNonNull(runtime));
    if (LLVM_LIKELY(!clazz->isDictionaryNoCache()) &&
        LLVM_LIKELY(cacheIdx != hbc::PROPERTY_CACHING_DISABLED) &&
        LLVM_LIKELY(desc.slot <= WritePropertyCacheEntry::kMaxSlot)) {
      cacheEntry->clazz = clazzPtr;
      cacheEntry->setSlot(desc.slot);
    }
    // This must be valid because an own property was already found.
    JSObject::setNamedSlotValueUnsafe(obj, runtime, desc.slot, valueToStore);
    return ExecutionStatus::RETURNED;
  }

  auto putRes = JSObject::defineOwnProperty(
      Handle<JSObject>::vmcast(&O1REG(PutByIdLoose)),
      runtime,
      id,
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      Handle<>(&O2REG(PutByIdLoose)),
      DEFAULT_PROP_OP_FLAGS(true));
  if (LLVM_UNLIKELY(putRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseDefineOwnByVal(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  return JSObject::defineOwnComputed(
             Handle<JSObject>::vmcast(&O1REG(DefineOwnByVal)),
             runtime,
             Handle<>(&O3REG(DefineOwnByVal)),
             ip->iDefineOwnByVal.op4
                 ? DefinePropertyFlags::getDefaultNewPropertyFlags()
                 : DefinePropertyFlags::getNewNonEnumerableFlags(),
             Handle<>(&O2REG(DefineOwnByVal)),
             PropOpFlags().plusThrowOnError())
      .getStatus();
}

ExecutionStatus Interpreter::caseDefineOwnGetterSetterByVal(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  DefinePropertyFlags dpFlags{};
  dpFlags.setConfigurable = 1;
  dpFlags.configurable = 1;
  dpFlags.setEnumerable = 1;
  dpFlags.enumerable = ip->iDefineOwnGetterSetterByVal.op5;

  MutableHandle<Callable> getter(runtime);
  MutableHandle<Callable> setter(runtime);
  if (LLVM_LIKELY(!O3REG(DefineOwnGetterSetterByVal).isUndefined())) {
    dpFlags.setGetter = 1;
    getter = vmcast<Callable>(O3REG(DefineOwnGetterSetterByVal));
  }
  if (LLVM_LIKELY(!O4REG(DefineOwnGetterSetterByVal).isUndefined())) {
    dpFlags.setSetter = 1;
    setter = vmcast<Callable>(O4REG(DefineOwnGetterSetterByVal));
  }
  assert(
      (dpFlags.setSetter || dpFlags.setGetter) &&
      "No accessor set in DefineOwnGetterSetterByVal");

  auto accessor = runtime.makeHandle<PropertyAccessor>(
      PropertyAccessor::create(runtime, getter, setter));

  return JSObject::defineOwnComputed(
             Handle<JSObject>::vmcast(&O1REG(DefineOwnGetterSetterByVal)),
             runtime,
             Handle<>(&O2REG(DefineOwnGetterSetterByVal)),
             dpFlags,
             accessor,
             PropOpFlags().plusThrowOnError())
      .getStatus();
}

ExecutionStatus Interpreter::caseIteratorBegin(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  if (LLVM_LIKELY(vmisa<JSArray>(O2REG(IteratorBegin)))) {
    // Attempt to get the fast path for array iteration.
    NamedPropertyDescriptor desc;
    JSObject *propObj = JSObject::getNamedDescriptorPredefined(
        Handle<JSArray>::vmcast(&O2REG(IteratorBegin)),
        runtime,
        Predefined::SymbolIterator,
        desc);
    if (LLVM_LIKELY(propObj)) {
      auto slotValueRes = JSObject::getNamedSlotValue(
          createPseudoHandle(propObj), runtime, desc);
      if (LLVM_UNLIKELY(slotValueRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      PseudoHandle<> slotValue = std::move(*slotValueRes);
      if (LLVM_LIKELY(
              slotValue->getRaw() ==
              runtime.arrayPrototypeValues.getHermesValue().getRaw())) {
        O1REG(IteratorBegin) = HermesValue::encodeTrustedNumberValue(0);
        return ExecutionStatus::RETURNED;
      }
    }
  }
  GCScopeMarkerRAII marker{runtime};
  CallResult<UncheckedIteratorRecord> iterRecord =
      getIterator(runtime, Handle<>(&O2REG(IteratorBegin)));
  if (LLVM_UNLIKELY(iterRecord == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  O2REG(IteratorBegin) = iterRecord->nextMethod.getHermesValue();
  // Write the result last in case it is the same register as O2REG.
  O1REG(IteratorBegin) = iterRecord->iterator.getHermesValue();
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseIteratorNext(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  if (LLVM_LIKELY(O2REG(IteratorNext).isNumber())) {
    JSArray::size_type i =
        O2REG(IteratorNext).getNumberAs<JSArray::size_type>();
    if (i >=
        JSArray::getLength(vmcast<JSArray>(O3REG(IteratorNext)), runtime)) {
      // Finished iterating the array, stop.
      O2REG(IteratorNext) = HermesValue::encodeUndefinedValue();
      O1REG(IteratorNext) = HermesValue::encodeUndefinedValue();
      return ExecutionStatus::RETURNED;
    }
    Handle<JSArray> arr = Handle<JSArray>::vmcast(&O3REG(IteratorNext));
    {
      // Fast path: look up the property in indexed storage.
      // Runs when there is no hole and a regular non-accessor property exists
      // at the current index, because those are the only properties stored
      // in indexed storage.
      // If there is another kind of property we have to call getComputed_RJS.
      // No need to check the fastIndexProperties flag because the indexed
      // storage would be deleted and at() would return empty in that case.
      NoAllocScope noAlloc{runtime};
      SmallHermesValue value = arr->at(runtime, i);
      if (LLVM_LIKELY(!value.isEmpty())) {
        O2REG(IteratorNext) = HermesValue::encodeTrustedNumberValue(i + 1);
        // Write the result last in case it is the same register as O2REG.
        O1REG(IteratorNext) = value.unboxToHV(runtime);
        return ExecutionStatus::RETURNED;
      }
    }
    // Slow path, just run the full getComputedPropertyValue_RJS path.
    GCScopeMarkerRAII marker{runtime};
    Handle<> idxHandle{&O2REG(IteratorNext)};
    CallResult<PseudoHandle<>> valueRes =
        JSObject::getComputed_RJS(arr, runtime, idxHandle);
    if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    O2REG(IteratorNext) = HermesValue::encodeTrustedNumberValue(i + 1);
    // Write the result last in case it is the same register as O2REG.
    O1REG(IteratorNext) = valueRes->get();
    return ExecutionStatus::RETURNED;
  }
  if (LLVM_UNLIKELY(O2REG(IteratorNext).isUndefined())) {
    O1REG(IteratorNext) = HermesValue::encodeUndefinedValue();
    return ExecutionStatus::RETURNED;
  }

  if (LLVM_UNLIKELY(!vmisa<Callable>(O3REG(IteratorNext)))) {
    return runtime.raiseTypeError("'next' method on iterator must be callable");
  }

  GCScopeMarkerRAII marker{runtime};

  CheckedIteratorRecord iterRecord{
      Handle<JSObject>::vmcast(&O2REG(IteratorNext)),
      Handle<Callable>::vmcast(&O3REG(IteratorNext))};

  CallResult<PseudoHandle<JSObject>> resultObjRes =
      iteratorNext(runtime, iterRecord, llvh::None);
  if (LLVM_UNLIKELY(resultObjRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSObject> resultObj = runtime.makeHandle(std::move(*resultObjRes));
  CallResult<PseudoHandle<>> doneRes = JSObject::getNamed_RJS(
      resultObj, runtime, Predefined::getSymbolID(Predefined::done));
  if (LLVM_UNLIKELY(doneRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (toBoolean(doneRes->get())) {
    // Done with iteration. Clear the iterator so that subsequent
    // instructions do not call next() or return().
    O2REG(IteratorNext) = HermesValue::encodeUndefinedValue();
    O1REG(IteratorNext) = HermesValue::encodeUndefinedValue();
  } else {
    // Not done iterating, so get the `value` property and store it
    // as the result.
    CallResult<PseudoHandle<>> propRes = JSObject::getNamed_RJS(
        resultObj, runtime, Predefined::getSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    O1REG(IteratorNext) = propRes->get();
    propRes->invalidate();
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseGetPNameList(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  if (LLVM_UNLIKELY(O2REG(GetPNameList).isUndefined()) ||
      LLVM_UNLIKELY(O2REG(GetPNameList).isNull())) {
    // Set the iterator to be undefined value.
    O1REG(GetPNameList) = HermesValue::encodeUndefinedValue();
    return ExecutionStatus::RETURNED;
  }

  if (LLVM_UNLIKELY(!vmisa<JSObject>(O2REG(GetPNameList)))) {
    // Convert to object and store it back to the register.
    auto res = toObject(runtime, Handle<>(&O2REG(GetPNameList)));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    O2REG(GetPNameList) = res.getValue();
  }

  struct : public Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.obj.castAndSetHermesValue<JSObject>(O2REG(GetPNameList));
  uint32_t beginIndex;
  uint32_t endIndex;
  MutableHandle<JSObject> obj{lv.obj};
  auto cr = getForInPropertyNames(runtime, obj, beginIndex, endIndex);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<ArrayStorageSmall> arr = *cr;
  O3REG(GetPNameList) = HermesValue::encodeTrustedNumberValue(beginIndex);
  O4REG(GetPNameList) = HermesValue::encodeTrustedNumberValue(endIndex);
  // Write the result last in case it is the same register as one of the in/out
  // operands.
  O1REG(GetPNameList) = arr.getHermesValue();
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseGetNextPName(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  struct : public Locals {
    PinnedValue<> tmp;
    PinnedValue<JSObject> propObj;
    PinnedValue<SymbolID> tmpPropNameStorage;
    PinnedValue<HiddenClass> cachedClass;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  assert(
      vmisa<ArrayStorageSmall>(O2REG(GetNextPName)) &&
      "GetNextPName's second op must be ArrayStorageSmall");
  auto obj = Handle<JSObject>::vmcast(&O3REG(GetNextPName));
  auto arr = Handle<ArrayStorageSmall>::vmcast(&O2REG(GetNextPName));
  uint32_t idx = O4REG(GetNextPName).getNumber();
  uint32_t size = O5REG(GetNextPName).getNumber();

  // If there's a class at index 2, it means we have a cached class.
  uint32_t startIdx = 0;
  uint32_t numObjProps = 0;
  if (LLVM_LIKELY(size > 2)) {
    auto clazzSHV = arr->at(2);
    if (clazzSHV.isObject()) {
      lv.cachedClass = vmcast<HiddenClass>(clazzSHV.getObject(runtime));
      startIdx = arr->at(0).getNumberAs<uint32_t>(runtime);
      numObjProps = arr->at(1).getNumberAs<uint32_t>(runtime);
    }
  }

  MutableHandle<JSObject> propObj{lv.propObj};
  MutableHandle<SymbolID> tmpPropNameStorage{lv.tmpPropNameStorage};
  // Loop until we find a property which is present.
  while (LLVM_LIKELY(idx < size)) {
    // If there's no caching, lv.cachedClass is nullptr and the comparison will
    // fail.
    if (LLVM_LIKELY(size > 0) && idx - startIdx < numObjProps &&
        LLVM_LIKELY(lv.cachedClass.get() == obj->getClass(runtime))) {
      // Cached.
      propObj = obj;
      break;
    }
    auto tmpSHV = arr->at(idx);
    if (tmpSHV.isSymbol()) {
      // NOTE: This call is safe because we immediately discard desc,
      // so it can't outlive the SymbolID.
      NamedPropertyDescriptor desc;
      propObj = JSObject::getNamedDescriptorUnsafe(
          obj, runtime, tmpSHV.getSymbol(), desc);
    } else {
      assert(
          (tmpSHV.isNumber() || tmpSHV.isString()) &&
          "GetNextPName must be symbol, string, number");
      ComputedPropertyDescWithSymStorage desc{tmpPropNameStorage};
      lv.tmp = tmpSHV.unboxToHV(runtime);
      ExecutionStatus status = JSObject::getComputedPrimitiveDescriptor(
          obj, runtime, lv.tmp, propObj, desc);
      if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
    if (LLVM_LIKELY(propObj))
      break;
    ++idx;
  }
  if (LLVM_LIKELY(idx < size)) {
    auto tmpSHV = arr->at(idx);
    // We must return the property as a string
    if (tmpSHV.isNumber()) {
      auto strRes = numberToStringPrimitive(runtime, tmpSHV.getNumber(runtime));
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.tmp = strRes->getHermesValue();
    } else if (LLVM_LIKELY(tmpSHV.isSymbol())) {
      // for-in enumeration only returns numbers and strings.
      // In most cases (i.e. non-Proxy), we keep the symbol around instead
      // and convert here, so that the above getNamedDescriptor call is faster.
      // Proxy has a filter so that it only returns Strings here.
      // So we don't have to check isUniqued and can convert to string
      // unconditionally.
      assert(
          tmpSHV.getSymbol().isUniqued() &&
          "Symbol primitives (non-uniqued) can't be used in for-in, "
          "not even by Proxy");
      lv.tmp = HermesValue::encodeStringValue(
          runtime.getStringPrimFromSymbolID(tmpSHV.getSymbol()));
    } else {
      assert(
          tmpSHV.isString() && "GetNextPName must be symbol, string, number");
      lv.tmp = vm::HermesValue::encodeStringValue(tmpSHV.getString(runtime));
    }
    O4REG(GetNextPName) = HermesValue::encodeTrustedNumberValue(idx + 1);
    // Write the result last in case it is the same register as O4REG.
    O1REG(GetNextPName) = lv.tmp.get();
  } else {
    O1REG(GetNextPName) = HermesValue::encodeUndefinedValue();
  }
  return ExecutionStatus::RETURNED;
}

PseudoHandle<JSRegExp> Interpreter::createRegExp(
    Runtime &runtime,
    CodeBlock *curCodeBlock,
    SymbolID patternID,
    SymbolID flagsID,
    uint32_t regexpID) {
  GCScopeMarkerRAII marker{runtime};

  struct : public Locals {
    PinnedValue<JSRegExp> re;
    PinnedValue<StringPrimitive> pattern;
    PinnedValue<StringPrimitive> flags;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Create the RegExp object.
  lv.re = JSRegExp::create(runtime);
  // Initialize the regexp.
  RuntimeModule *runtimeModule = curCodeBlock->getRuntimeModule();
  lv.pattern = runtime.getStringPrimFromSymbolID(patternID);
  lv.flags = runtime.getStringPrimFromSymbolID(flagsID);
  JSRegExp::initialize(
      lv.re,
      runtime,
      lv.pattern,
      lv.flags,
      runtimeModule->getRegExpBytecode(regexpID));

  return createPseudoHandle(*lv.re);
}

void Interpreter::caseCreateRegExp(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    CodeBlock *curCodeBlock,
    const inst::Inst *ip) {
  O1REG(CreateRegExp) =
      Interpreter::createRegExp(
          runtime,
          curCodeBlock,
          curCodeBlock->getRuntimeModule()->getSymbolIDFromStringIDMayAllocate(
              ip->iCreateRegExp.op2),
          curCodeBlock->getRuntimeModule()->getSymbolIDFromStringIDMayAllocate(
              ip->iCreateRegExp.op3),
          ip->iCreateRegExp.op4)
          .getHermesValue();
}

ExecutionStatus Interpreter::caseDelByVal(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  const PropOpFlags defaultPropOpFlags =
      DEFAULT_PROP_OP_FLAGS(ip->iDelByVal.op4 != 0);
  if (LLVM_LIKELY(O2REG(DelByVal).isObject())) {
    auto status = JSObject::deleteComputed(
        Handle<JSObject>::vmcast(&O2REG(DelByVal)),
        runtime,
        Handle<>(&O3REG(DelByVal)),
        defaultPropOpFlags);
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    O1REG(DelByVal) = HermesValue::encodeBoolValue(status.getValue());
  } else {
    // This is the "slow path".
    auto res = toObject(runtime, Handle<>(&O2REG(DelByVal)));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    struct : Locals {
      PinnedValue<> obj;
    } lv;
    LocalsRAII lraii{runtime, &lv};

    lv.obj = res.getValue();
    auto status = JSObject::deleteComputed(
        Handle<JSObject>::vmcast(&lv.obj),
        runtime,
        Handle<>(&O3REG(DelByVal)),
        defaultPropOpFlags);
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    O1REG(DelByVal) = HermesValue::encodeBoolValue(status.getValue());
  }
  return ExecutionStatus::RETURNED;
}

/// Fast path for getByValTransient() -- avoid boxing for \p base if it is
/// string primitive and \p nameHandle is an array index.
/// If the property does not exist, return Empty.
static inline PseudoHandle<>
getByValTransientFast(Runtime &runtime, Handle<> base, Handle<> nameHandle) {
  if (base->isString()) {
    ++NumGetByValStr;

    // Handle most common fast path -- array index property for string
    // primitive.
    // Since primitive string cannot have index like property we can
    // skip ObjectFlags::fastIndexProperties checking and directly
    // checking index storage from StringPrimitive.

    OptValue<uint32_t> arrayIndex = toArrayIndexFastPath(*nameHandle);
    // Get character directly from primitive if arrayIndex is within range.
    // Otherwise we need to fall back to prototype lookup.
    if (arrayIndex &&
        arrayIndex.getValue() < base->getString()->getStringLength()) {
      return createPseudoHandle(
          runtime
              .getCharacterString(base->getString()->at(arrayIndex.getValue()))
              .getHermesValue());
    }
  }
  return createPseudoHandle(HermesValue::encodeEmptyValue());
}

CallResult<PseudoHandle<>> Interpreter::getByValTransientWithReceiver_RJS(
    Runtime &runtime,
    Handle<> base,
    Handle<> name,
    Handle<> receiver) {
  // This is similar to what ES5.1 8.7.1 special [[Get]] internal
  // method did, but that section doesn't exist in ES9 anymore.
  // Instead, the [[Get]] Receiver argument serves a similar purpose.

  // Optimization: check fast path first.
  PseudoHandle<> fastRes = getByValTransientFast(runtime, base, name);
  if (!fastRes->isEmpty()) {
    return fastRes;
  }

  auto res = toObject(runtime, base);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  return JSObject::getComputedWithReceiver_RJS(
      runtime.makeHandle<JSObject>(res.getValue()), runtime, name, receiver);
}

ExecutionStatus Interpreter::caseGetByVal(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  CallResult<PseudoHandle<HermesValue>> resPH{ExecutionStatus::EXCEPTION};
  if (LLVM_LIKELY(O2REG(GetByVal).isObject())) {
    resPH = JSObject::getComputed_RJS(
        Handle<JSObject>::vmcast(&O2REG(GetByVal)),
        runtime,
        Handle<>(&O3REG(GetByVal)));
    if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    // This is the "slow path".
    resPH = Interpreter::getByValTransient_RJS(
        runtime, Handle<>(&O2REG(GetByVal)), Handle<>(&O3REG(GetByVal)));
    if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  O1REG(GetByVal) = resPH->get();
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseGetByValWithReceiver(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  CallResult<PseudoHandle<HermesValue>> resPH{ExecutionStatus::EXCEPTION};
  if (LLVM_LIKELY(O2REG(GetByIdWithReceiverLong).isObject())) {
    resPH = JSObject::getComputedWithReceiver_RJS(
        Handle<JSObject>::vmcast(&O2REG(GetByValWithReceiver)),
        runtime,
        Handle<>(&O3REG(GetByValWithReceiver)),
        Handle<>(&O4REG(GetByValWithReceiver)));
    if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    resPH = Interpreter::getByValTransientWithReceiver_RJS(
        runtime,
        Handle<>(&O2REG(GetByValWithReceiver)),
        Handle<>(&O3REG(GetByValWithReceiver)),
        Handle<>(&O4REG(GetByValWithReceiver)));
    if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  O1REG(GetByValWithReceiver) = resPH->get();
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::putByValTransient_RJS(
    Runtime &runtime,
    Handle<> base,
    Handle<> name,
    Handle<> value,
    bool strictMode) {
  auto idRes = valueToSymbolID(runtime, name);
  if (idRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  return putByIdTransient_RJS(runtime, base, **idRes, value, strictMode);
}

ExecutionStatus Interpreter::casePutByVal(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  ++NumPutByVal;
  bool strictMode = (ip->opCode == inst::OpCode::PutByValStrict);
  if (LLVM_LIKELY(O1REG(PutByValLoose).isObject())) {
    auto defaultPropOpFlags = DEFAULT_PROP_OP_FLAGS(strictMode);
    auto putRes = JSObject::putComputed_RJS(
        Handle<JSObject>::vmcast(&O1REG(PutByValLoose)),
        runtime,
        Handle<>(&O2REG(PutByValLoose)),
        Handle<>(&O3REG(PutByValLoose)),
        defaultPropOpFlags);
    if (LLVM_UNLIKELY(putRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    // This is the "slow path".
    auto retStatus = Interpreter::putByValTransient_RJS(
        runtime,
        Handle<>(&O1REG(PutByValLoose)),
        Handle<>(&O2REG(PutByValLoose)),
        Handle<>(&O3REG(PutByValLoose)),
        strictMode);
    if (LLVM_UNLIKELY(retStatus == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::casePutByValWithReceiver(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  auto defaultPropOpFlags =
      DEFAULT_PROP_OP_FLAGS(ip->iPutByValWithReceiver.op5);
  if (LLVM_LIKELY(O1REG(PutByValWithReceiver).isObject())) {
    auto res = JSObject::putComputedWithReceiver_RJS(
        Handle<JSObject>::vmcast(&O1REG(PutByValWithReceiver)),
        runtime,
        Handle<>(&O2REG(PutByValWithReceiver)),
        Handle<>(&O3REG(PutByValWithReceiver)),
        Handle<>(&O4REG(PutByValWithReceiver)),
        defaultPropOpFlags);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    auto retStatus = Interpreter::putByValTransient_RJS(
        runtime,
        Handle<>(&O1REG(PutByValLoose)),
        Handle<>(&O2REG(PutByValLoose)),
        Handle<>(&O3REG(PutByValLoose)),
        ip->iPutByValWithReceiver.op5);
    if (LLVM_UNLIKELY(retStatus == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

CallResult<HermesValue> Interpreter::createThisImpl(
    Runtime &runtime,
    PinnedHermesValue *callee,
    PinnedHermesValue *newTarget,
    uint8_t cacheIdx,
    uint32_t shapeTableIdx,
    CodeBlock *curCodeBlock) {
  if (LLVM_UNLIKELY(!callee->isObject())) {
    // Add a leading space because the value will come first when we use
    // raiseTypeErrorForValue.
    return runtime.raiseTypeErrorForValue(
        Handle<>(callee), " cannot be used as a constructor.");
  }

  auto *calleeFunc = vmcast<JSObject>(*callee);
  auto cellKind = calleeFunc->getKind();
  Callable *correctNewTarget;
  // CellKind.h documents the invariants we take advantage of here to
  // efficiently check what kind of callable we are dealing with.
  // If this is a callable that expects a `this`, then skip ahead and start
  // making the object.
  if (cellKind >= CellKind::CallableExpectsThisKind_first) {
    correctNewTarget = vmcast<Callable>(*newTarget);
  } else if (cellKind >= CellKind::CallableMakesThisKind_first) {
    // Callables that make their own this should be given undefined in a
    // construct call.
    return HermesValue::encodeUndefinedValue();
  } else if (cellKind >= CellKind::CallableUnknownMakesThisKind_first) {
    correctNewTarget = vmcast<Callable>(*newTarget);
    while (auto *bound = dyn_vmcast<BoundFunction>(calleeFunc)) {
      calleeFunc = bound->getTarget(runtime);
      // From ES15 10.4.1.2 [[Construct]] Step 5: If SameValue(F, newTarget) is
      // true, set newTarget to target.
      if (bound == vmcast<Callable>(correctNewTarget)) {
        correctNewTarget = vmcast<Callable>(calleeFunc);
      }
    }
    cellKind = calleeFunc->getKind();
    // Repeat the checks, now against the target.
    if (cellKind >= CellKind::CallableExpectsThisKind_first) {
      // Do nothing, correctNewTarget is already set up correctly.
    } else if (cellKind >= CellKind::CallableMakesThisKind_first) {
      return HermesValue::encodeUndefinedValue();
    } else {
      // If we still can't recognize what to do after advancing through bound
      // functions (if any), error out. This code path is hit when invoking a
      // NativeFunction as a constructor.
      return runtime.raiseTypeError(
          "This function cannot be used as a constructor.");
    }
  } else {
    // Not a Callable.
    return runtime.raiseTypeErrorForValue(
        Handle<>(callee), " cannot be used as a constructor.");
  }

  // We shouldn't need to check that new.target is a constructor explicitly.
  // There are 2 cases where this instruction is emitted.
  // 1. new expressions. In this case, new.target == callee. We always verify
  // that a function call is performed correctly, so don't need to double-verify
  // new.target.
  // 2. super() calls in derived constructors. In this case, new.target will be
  // set to the original callee for the new expression which triggered the
  // constructor now invoking super. This means that new.target will be checked.
  // Unfortunately, this check for constructors is done *after* this
  // instruction, so we can't add an assert that newTarget is a constructor
  // here. For example, using `new` on an arrow function, newTarget is not a
  // constructor, but we don't find that out until after this instruction.

  struct : public Locals {
    PinnedValue<Callable> newTarget;
    // This is the .prototype of new.target
    PinnedValue<JSObject> newTargetPrototype;
    // HiddenClass for the new object.
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.newTarget = correctNewTarget;

  auto *cacheEntry = curCodeBlock->getReadCacheEntry(cacheIdx);
  CompressedPointer clazzPtr{lv.newTarget->getClassGCPtr()};
  // If we have a cache hit, reuse the cached offset and immediately
  // return the property.
  if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {
    auto shvPrototype = JSObject::getNamedSlotValueUnsafe(
        *lv.newTarget, runtime, cacheEntry->getSlot());
    if (LLVM_LIKELY(shvPrototype.isObject())) {
      lv.newTargetPrototype = vmcast<JSObject>(shvPrototype.getObject(runtime));
    } else {
      lv.newTargetPrototype = runtime.objectPrototype;
    }
  } else {
    GCScopeMarkerRAII marker{runtime};
    auto newTargetProtoRes = JSObject::getNamed_RJS(
        lv.newTarget,
        runtime,
        Predefined::getSymbolID(Predefined::prototype),
        PropOpFlags(),
        cacheIdx != hbc::PROPERTY_CACHING_DISABLED ? cacheEntry : nullptr);
    if (LLVM_UNLIKELY(newTargetProtoRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if ((*newTargetProtoRes)->isObject()) {
      lv.newTargetPrototype.castAndSetHermesValue<JSObject>(
          newTargetProtoRes->get());
    } else {
      lv.newTargetPrototype = runtime.objectPrototype;
    }
  }

  RuntimeModule *runtimeModule = curCodeBlock->getRuntimeModule();
  if (auto *cachedClazz = runtimeModule->findCachedLiteralHiddenClass(
          runtime, *lv.newTargetPrototype, shapeTableIdx)) {
    lv.clazz = cachedClazz;
  } else {
    lv.clazz = *runtime.getHiddenClassForPrototype(
        *lv.newTargetPrototype, runtime.classJSObject);
    assert(!lv.clazz->isDictionary() && "Prototype must be a normal object");
    runtimeModule->setCachedLiteralHiddenClass(
        runtime, shapeTableIdx, *lv.clazz);
  }

  // Avoid passing just the prototype to avoid a lookup and a HiddenClass
  // transition. We use the cache to prepopulate the clazz.
  return JSObject::create(runtime, lv.newTargetPrototype, lv.clazz)
      .getHermesValue();
}

ExecutionStatus Interpreter::implCallBuiltin(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    CodeBlock *curCodeBlock,
    uint32_t op3) {
  const Inst *ip = runtime.getCurrentIP();
  uint8_t methodIndex = ip->iCallBuiltin.op2;
  Callable *callable = runtime.getBuiltinCallable(methodIndex);
  assert(
      isNativeBuiltin(methodIndex) &&
      "CallBuiltin must take a native builtin.");
  NativeFunction *nf = vmcast<NativeFunction>(callable);

  auto newFrame = StackFramePtr::initFrame(
      runtime.getStackPointer(),
      FRAME,
      ip,
      curCodeBlock,
      nullptr,
      op3 - 1,
      nf,
      false);
  // "thisArg" is implicitly assumed to "undefined".
  newFrame.getThisArgRef() = HermesValue::encodeUndefinedValue();

  SLOW_DEBUG(dumpCallArguments(llvh::dbgs(), runtime, newFrame));

  auto resPH = NativeFunction::_nativeCall(nf, runtime);
  if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  O1REG(CallBuiltin) = std::move(resPH->get());
  SLOW_DEBUG(
      llvh::dbgs() << "native return value r" << (unsigned)ip->iCallBuiltin.op1
                   << "=" << DumpHermesValue(O1REG(CallBuiltin)) << "\n");
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::declareGlobalVarImpl(
    Runtime &runtime,
    CodeBlock *curCodeBlock,
    const Inst *ip) {
  GCScopeMarkerRAII mark{runtime};
  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.configurable = 0;
  // Do not overwrite existing globals with undefined.
  dpf.setValue = 0;

  CallResult<bool> res = JSObject::defineOwnProperty(
      runtime.getGlobal(),
      runtime,
      ID(ip->iDeclareGlobalVar.op1),
      dpf,
      Runtime::getUndefinedValue(),
      PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    assert(
        !runtime.getGlobal()->isProxyObject(runtime) &&
        "global can't be a proxy object");
    // If the property already exists, this should be a noop.
    // Instead of incurring the cost to check every time, do it
    // only if an exception is thrown, and swallow the exception
    // if it exists, since we didn't want to make the call,
    // anyway.  This most likely means the property is
    // non-configurable.
    NamedPropertyDescriptor desc;
    if (!JSObject::getOwnNamedDescriptor(
            runtime.getGlobal(),
            runtime,
            ID(ip->iDeclareGlobalVar.op1),
            desc)) {
      return ExecutionStatus::EXCEPTION;
    }
    runtime.clearThrownValue();
  }
  return ExecutionStatus::RETURNED;
}

using BigIntBinaryOp = CallResult<HermesValue>(
    Runtime &,
    Handle<BigIntPrimitive>,
    Handle<BigIntPrimitive>);

static CallResult<HermesValue> doBigIntBinOp(
    Runtime &runtime,
    BigIntBinaryOp Oper,
    Handle<BigIntPrimitive> lhs,
    Handle<> rhs) {
  // Cannot use ToBigInt here as it would incorrectly allow boolean/string rhs.
  CallResult<HermesValue> res = toNumeric_RJS(runtime, rhs);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!res->isBigInt()) {
    return runtime.raiseTypeErrorForValue("Cannot convert ", rhs, " to BigInt");
  }
  return Oper(runtime, lhs, runtime.makeHandle(res->getBigInt()));
}

namespace {
/// BigIntOper maps the \param Oper (a Number operation) to its respective
/// BigIntPrimitive counterpart.
template <auto Oper>
int BigIntOper;

template <>
constexpr auto &BigIntOper<doDiv> = BigIntPrimitive::divide;

template <>
constexpr auto &BigIntOper<doMod> = BigIntPrimitive::remainder;

template <>
constexpr auto &BigIntOper<doSub> = BigIntPrimitive::subtract;

template <>
constexpr auto &BigIntOper<doMul> = BigIntPrimitive::multiply;

template <>
constexpr auto &BigIntOper<doBitAnd> = BigIntPrimitive::bitwiseAND;

template <>
constexpr auto &BigIntOper<doBitOr> = BigIntPrimitive::bitwiseOR;

template <>
constexpr auto &BigIntOper<doBitXor> = BigIntPrimitive::bitwiseXOR;

template <>
constexpr auto &BigIntOper<doLShift> = BigIntPrimitive::leftShift;

template <>
constexpr auto &BigIntOper<doRShift> = BigIntPrimitive::signedRightShift;

template <>
constexpr auto &BigIntOper<doURshift> = BigIntPrimitive::unsignedRightShift;

template <>
constexpr auto &BigIntOper<doInc> = BigIntPrimitive::inc;

template <>
constexpr auto &BigIntOper<doDec> = BigIntPrimitive::dec;

} // namespace

template <auto Oper, typename InstType>
ExecutionStatus doOperSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const InstType *inst) {
  Handle<> lhs(&REG(inst->op2));
  Handle<> rhs(&REG(inst->op3));
  CallResult<HermesValue> res =
      toPrimitive_RJS(runtime, lhs, PreferredType::NUMBER);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_LIKELY(!res->isBigInt())) {
    res = toNumber_RJS(runtime, runtime.makeHandle(*res));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    double left = res->getDouble();
    res = toNumber_RJS(runtime, rhs);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    REG(inst->op1) =
        HermesValue::encodeTrustedNumberValue(Oper(left, res->getDouble()));
    return ExecutionStatus::RETURNED;
  }
  res = doBigIntBinOp(
      runtime, BigIntOper<Oper>, runtime.makeHandle(res->getBigInt()), rhs);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  REG(inst->op1) = *res;
  return ExecutionStatus::RETURNED;
}

template ExecutionStatus doOperSlowPath_RJS<doDiv>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::DivInst *inst);

template ExecutionStatus doOperSlowPath_RJS<doMod>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::ModInst *inst);

template ExecutionStatus doOperSlowPath_RJS<doMul>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::MulInst *inst);

template ExecutionStatus doOperSlowPath_RJS<doSub>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::SubInst *inst);

template <auto Oper, typename InstType>
ExecutionStatus doBitOperSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const InstType *inst) {
  Handle<> lhs(&REG(inst->op2));
  Handle<> rhs(&REG(inst->op3));
  CallResult<HermesValue> res =
      toPrimitive_RJS(runtime, lhs, PreferredType::NUMBER);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_LIKELY(!res->isBigInt())) {
    res = toInt32_RJS(runtime, runtime.makeHandle(*res));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    const int32_t left = res->getNumberAs<int32_t>();
    res = toInt32_RJS(runtime, rhs);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    REG(inst->op1) = HermesValue::encodeTrustedNumberValue(
        Oper(left, res->getNumberAs<int32_t>()));
    return ExecutionStatus::RETURNED;
  }
  res = doBigIntBinOp(
      runtime,
      BigIntOper<Oper>,
      runtime.makeHandle(res->getBigInt()),
      Handle<>(rhs));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  REG(inst->op1) = *res;
  return ExecutionStatus::RETURNED;
}

template ExecutionStatus doBitOperSlowPath_RJS<doBitAnd>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::BitAndInst *ip);

template ExecutionStatus doBitOperSlowPath_RJS<doBitOr>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::BitOrInst *ip);

template ExecutionStatus doBitOperSlowPath_RJS<doBitXor>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::BitXorInst *ip);

namespace {
/// ToIntegral maps the \param Oper shift operation (on Number) to the function
/// used to convert the operation's lhs operand to integer.
template <auto Oper>
inline int ToIntegral;

// For LShift, we need to use toUInt32 first because lshift on negative
// numbers is undefined behavior in theory.
template <>
inline constexpr auto &ToIntegral<doLShift> = toUInt32_RJS;

template <>
inline constexpr auto &ToIntegral<doRShift> = toInt32_RJS;

template <>
inline constexpr auto &ToIntegral<doURshift> = toUInt32_RJS;
} // namespace

template <auto Oper, typename InstType>
ExecutionStatus doShiftOperSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const InstType *inst) {
  Handle<> lhs(&REG(inst->op2));
  Handle<> rhs(&REG(inst->op3));
  CallResult<HermesValue> res =
      toPrimitive_RJS(runtime, std::move(lhs), PreferredType::NUMBER);

  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (LLVM_LIKELY(!res->isBigInt())) {
    res = ToIntegral<Oper>(runtime, runtime.makeHandle(*res));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto lnum = hermes::truncateToInt32(res->getNumber());
    res = toUInt32_RJS(runtime, rhs);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto rnum = static_cast<uint32_t>(res->getNumber()) & 0x1f;
    REG(inst->op1) = HermesValue::encodeTrustedNumberValue((*Oper)(lnum, rnum));
    return ExecutionStatus::RETURNED;
  }
  res = doBigIntBinOp(
      runtime,
      BigIntOper<Oper>,
      runtime.makeHandle(res->getBigInt()),
      std::move(rhs));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  REG(inst->op1) = *res;
  return ExecutionStatus::RETURNED;
}

template ExecutionStatus doShiftOperSlowPath_RJS<doLShift>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::LShiftInst *inst);

template ExecutionStatus doShiftOperSlowPath_RJS<doRShift>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::RShiftInst *inst);

template ExecutionStatus doShiftOperSlowPath_RJS<doURshift>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::URshiftInst *inst);

template <auto Oper, typename InstType>
ExecutionStatus doIncDecOperSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const InstType *inst) {
  CallResult<HermesValue> res = toPrimitive_RJS(
      runtime, Handle<>(&REG(inst->op2)), PreferredType::NUMBER);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (LLVM_LIKELY(!res->isBigInt())) {
    res = toNumber_RJS(runtime, runtime.makeHandle(*res));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    REG(inst->op1) =
        HermesValue::encodeTrustedNumberValue(Oper(res->getNumber()));
    return ExecutionStatus::RETURNED;
  }

  res = BigIntOper<Oper>(runtime, runtime.makeHandle(res->getBigInt()));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  REG(inst->op1) = *res;
  return ExecutionStatus::RETURNED;
}

template ExecutionStatus doIncDecOperSlowPath_RJS<doInc>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::IncInst *inst);

template ExecutionStatus doIncDecOperSlowPath_RJS<doDec>(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const inst::DecInst *inst);

ExecutionStatus doBitNotSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  // Try converting src to a numeric.
  auto numRes = toNumeric_RJS(runtime, Handle<>(&O2REG(BitNot)));
  if (LLVM_UNLIKELY(numRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  // Test for BigInt since it is cheaper than testing for number. If it is a
  // number, truncate it and perform bitwise not.
  if (LLVM_LIKELY(!numRes->isBigInt())) {
    O1REG(BitNot) = HermesValue::encodeTrustedNumberValue(
        ~hermes::truncateToInt32(numRes->getNumber()));
    return ExecutionStatus::RETURNED;
  }

  // The result is a BigInt, perform a BigInt bitwise not.
  auto bigint = runtime.makeHandle(numRes->getBigInt());
  auto res = BigIntPrimitive::unaryNOT(runtime, bigint);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  O1REG(BitNot) = *res;
  return ExecutionStatus::RETURNED;
}

ExecutionStatus doNegateSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  // Try converting src to a numeric.
  auto numRes = toNumeric_RJS(runtime, Handle<>(&O2REG(Negate)));
  if (LLVM_UNLIKELY(numRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  // Test for BigInt since it is cheaper than testing for number. If it is a
  // number, negate it and return.
  if (LLVM_LIKELY(!numRes->isBigInt())) {
    O1REG(Negate) = HermesValue::encodeTrustedNumberValue(-numRes->getNumber());
    return ExecutionStatus::RETURNED;
  }

  // The result is a BigInt, perform a BigInt unary minus.
  auto bigint = runtime.makeHandle(numRes->getBigInt());
  auto res = BigIntPrimitive::unaryMinus(runtime, bigint);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  O1REG(Negate) = *res;
  return ExecutionStatus::RETURNED;
}

ExecutionStatus doImulSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  // Math.imul(a, b) always converts both operands with ToInt32 (which
  // raises a TypeError for a BigInt operand) and never falls back to a
  // BigInt result, unlike the other bitwise binops. So, unlike
  // doBitOperSlowPath_RJS, there is no BigInt path here.
  auto leftRes = toInt32_RJS(runtime, Handle<>(&O2REG(Imul)));
  if (LLVM_UNLIKELY(leftRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  int32_t left = leftRes->getNumberAs<int32_t>();
  auto rightRes = toInt32_RJS(runtime, Handle<>(&O3REG(Imul)));
  if (LLVM_UNLIKELY(rightRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  int32_t right = rightRes->getNumberAs<int32_t>();
  O1REG(Imul) = HermesValue::encodeTrustedNumberValue(doImul(left, right));
  return ExecutionStatus::RETURNED;
}

ExecutionStatus doCallRequireSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip,
    RuntimeModule *runtimeModule) {
  uint32_t modIndex = ip->iCallRequire.op3;

  // The value should be a Callable, or else we raise an error (as
  // Interpreter::handleCallSlowPath does).
  const auto requireFunc = Handle<>(&O2REG(CallRequire));
  auto requireFuncCallable = Handle<Callable>::dyn_vmcast(requireFunc);
  if (LLVM_UNLIKELY(!requireFuncCallable)) {
    return runtime.raiseTypeErrorForValue(requireFunc, " is not a function");
  }

  CallResult<PseudoHandle<>> modExport = Callable::executeCall1(
      requireFuncCallable,
      runtime,
      HandleRootOwner::getUndefinedValue(),
      HermesValue::encodeTrustedNumberValue(modIndex));
  if (LLVM_UNLIKELY(modExport == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  O1REG(CallRequire) = modExport->get();
  runtimeModule->setModuleExport(
      runtime, modIndex, Handle<>(&O1REG(CallRequire)));
  // Whether or not the caching above succeeded, write the module export
  // to the return register.
  return ExecutionStatus::RETURNED;
}

ExecutionStatus doGetByIdSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip,
    CodeBlock *curCodeBlock,
    uint32_t idVal,
    bool tryProp) {
  auto id = ID(idVal);
  // NOTE: it is safe to use OnREG(GetById) here because all instructions
  // have the same layout: opcode, registers, non-register operands, i.e.
  // they only differ in the width of the last "identifier" field.
  if (LLVM_UNLIKELY(!O2REG(GetById).isObject())) {
    ++NumGetByIdTransient;
    assert(!tryProp && "TryGetById can only be used on the global object");
    /* Slow path. */
    auto resPH = Interpreter::getByIdTransient_RJS(
        runtime, Handle<>(&O2REG(GetById)), id);
    if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    O1REG(GetById) = resPH->get();
    return ExecutionStatus::RETURNED;
  }

  auto *obj = vmcast<JSObject>(O2REG(GetById));
  auto cacheIdx = ip->iGetById.op3;
  auto *cacheEntry = curCodeBlock->getReadCacheEntry(cacheIdx);
  CompressedPointer clazzPtr{obj->getClassGCPtr()};

  NamedPropertyDescriptor desc;
  OptValue<bool> fastPathResult =
      JSObject::tryGetOwnNamedDescriptorFast(obj, runtime, id, desc);
  if (LLVM_LIKELY(fastPathResult.hasValue() && fastPathResult.getValue()) &&
      !desc.flags.accessor) {
    ++NumGetByIdFastPaths;

    // cacheIdx == 0 indicates no caching so don't update the cache in
    // those cases.
    HiddenClass *clazz = vmcast<HiddenClass>(clazzPtr.getNonNull(runtime));
    if (LLVM_LIKELY(!clazz->isDictionaryNoCache()) &&
        LLVM_LIKELY(cacheIdx != hbc::PROPERTY_CACHING_DISABLED) &&
        LLVM_LIKELY(desc.slot <= ReadPropertyCacheEntry::kMaxSlot)) {
#ifdef HERMES_SLOW_DEBUG
      if (cacheEntry->clazz && cacheEntry->clazz != clazzPtr)
        ++NumGetByIdCacheEvicts;
#else
      (void)NumGetByIdCacheEvicts;
#endif
      // Cache the class, id and property slot.
      cacheEntry->clazz = clazzPtr;
      cacheEntry->setSlot(desc.slot);
    }

    assert(
        !obj->isProxyObject(runtime) &&
        "tryGetOwnNamedDescriptorFast returned true on Proxy");
    O1REG(GetById) = JSObject::getNamedSlotValueUnsafe(obj, runtime, desc)
                         .unboxToHV(runtime);
    return ExecutionStatus::RETURNED;
  }

#ifdef HERMES_SLOW_DEBUG
  // Call to getNamedDescriptorUnsafe is safe because `id` is kept alive
  // by the IdentifierTable.
  JSObject *propObj = JSObject::getNamedDescriptorUnsafe(
      Handle<JSObject>::vmcast(&O2REG(GetById)), runtime, id, desc);
  if (propObj) {
    if (desc.flags.accessor)
      ++NumGetByIdAccessor;
    else if (propObj != vmcast<JSObject>(O2REG(GetById)))
      ++NumGetByIdProto;
  } else {
    ++NumGetByIdNotFound;
  }
#else
  (void)NumGetByIdAccessor;
  (void)NumGetByIdProto;
  (void)NumGetByIdNotFound;
#endif
#ifdef HERMES_SLOW_DEBUG
  // It's possible that there might be a GC between the time
  // savedClass is set and the time it is checked (to see if
  // there was an eviction.  This GC could move the HiddenClass
  // to which savedClass points, making it an invalid pointer.
  // We don't dereference this pointer, we just compare it, so
  // this won't cause a crash.  In this presumably rare case,
  // the eviction count would be incorrect.  But the
  // alternative, putting savedClass in a handle so it's a root,
  // could change GC behavior might alter the program behavior
  // in some cases: a HiddenClass might be live longer than it
  // would have been.  We're choosing to go with the first
  // problem as the lesser of two evils.
  auto *savedClass = cacheIdx != hbc::PROPERTY_CACHING_DISABLED
      ? cacheEntry->clazz.get(runtime, runtime.getHeap())
      : nullptr;
#endif
  ++NumGetByIdSlow;
  // Getting properties is not affected by strictness, so just use false.
  const auto defaultPropOpFlags = DEFAULT_PROP_OP_FLAGS(false);
  auto resPH = JSObject::getNamed_RJS(
      Handle<JSObject>::vmcast(&O2REG(GetById)),
      runtime,
      id,
      !tryProp ? defaultPropOpFlags : defaultPropOpFlags.plusMustExist(),
      cacheIdx != hbc::PROPERTY_CACHING_DISABLED ? cacheEntry : nullptr);
  if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
#ifdef HERMES_SLOW_DEBUG
  if (cacheIdx != hbc::PROPERTY_CACHING_DISABLED && savedClass &&
      cacheEntry->clazz.get(runtime, runtime.getHeap()) != savedClass) {
    ++NumGetByIdCacheEvicts;
  }
#endif

  O1REG(GetById) = resPH->get();
  return ExecutionStatus::RETURNED;
}

/// Fast path to get primitive value \p base's own properties by name \p id
/// without boxing.
/// Primitive own properties are properties fetching values from primitive
/// value itself.
/// Currently the only primitive own property is String.prototype.length.
/// If the fast path property does not exist, return Empty.
static inline PseudoHandle<>
tryGetPrimitiveOwnPropertyById(Runtime &runtime, Handle<> base, SymbolID id) {
  if (base->isString() && id == Predefined::getSymbolID(Predefined::length)) {
    return createPseudoHandle(
        HermesValue::encodeTrustedNumberValue(
            base->getString()->getStringLength()));
  }
  return createPseudoHandle(HermesValue::encodeEmptyValue());
}

CallResult<PseudoHandle<>> Interpreter::getByIdTransientWithReceiver_RJS(
    Runtime &runtime,
    Handle<> base,
    SymbolID id,
    Handle<> receiver) {
  // This is similar to what ES5.1 8.7.1 special [[Get]] internal
  // method did, but that section doesn't exist in ES9 anymore.
  // Instead, the [[Get]] Receiver argument serves a similar purpose.

  // Fast path: try to get primitive own property directly first.
  PseudoHandle<> valOpt = tryGetPrimitiveOwnPropertyById(runtime, base, id);
  if (!valOpt->isEmpty()) {
    return valOpt;
  }

  // get the property descriptor from primitive prototype without
  // boxing with vm::toObject().  This is where any properties will
  // be.
  CallResult<Handle<JSObject>> primitivePrototypeResult =
      getPrimitivePrototype(runtime, base);
  if (primitivePrototypeResult == ExecutionStatus::EXCEPTION) {
    // If an exception is thrown, likely we are trying to read property on
    // undefined/null. Passing over the name of the property
    // so that we could emit more meaningful error messages.
    return amendPropAccessErrorMsgWithPropName(runtime, base, "read", id);
  }

  return JSObject::getNamedWithReceiver_RJS(
      *primitivePrototypeResult, runtime, id, receiver);
}

ExecutionStatus doGetByIdWithReceiverSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip,
    CodeBlock *curCodeBlock) {
  if (LLVM_UNLIKELY(!O2REG(GetByIdWithReceiverLong).isObject())) {
    auto resPH = Interpreter::getByIdTransientWithReceiver_RJS(
        runtime,
        Handle<>(&O2REG(GetByIdWithReceiverLong)),
        ID(ip->iGetByIdWithReceiverLong.op5),
        Handle<>(&O4REG(GetByIdWithReceiverLong)));
    if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    O1REG(GetByIdWithReceiverLong) = resPH->getHermesValue();
    return ExecutionStatus::RETURNED;
  }
  auto cacheIdx = ip->iGetByIdWithReceiverLong.op3;
  auto *cacheEntry = curCodeBlock->getReadCacheEntry(cacheIdx);
  auto resPH = JSObject::getNamedWithReceiver_RJS(
      Handle<JSObject>::vmcast(&O2REG(GetByIdWithReceiverLong)),
      runtime,
      ID(ip->iGetByIdWithReceiverLong.op5),
      Handle<>(&O4REG(GetByIdWithReceiverLong)),
      DEFAULT_PROP_OP_FLAGS(false),
      cacheIdx != hbc::PROPERTY_CACHING_DISABLED ? cacheEntry : nullptr);
  if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  O1REG(GetByIdWithReceiverLong) = resPH->getHermesValue();
  return ExecutionStatus::RETURNED;
}

static ExecutionStatus
transientObjectPutErrorMessage(Runtime &runtime, Handle<> base, SymbolID id) {
  // Emit an error message that looks like:
  // "Cannot create property '%{id}' on ${typeof base} '${String(base)}'".
  StringView propName = runtime.getIdentifierTable().getStringView(runtime, id);
  Handle<StringPrimitive> baseType =
      runtime.makeHandle(vmcast<StringPrimitive>(typeOf(runtime, base)));
  StringView baseTypeAsString =
      StringPrimitive::createStringView(runtime, baseType);
  MutableHandle<StringPrimitive> valueAsString{runtime};
  if (base->isSymbol()) {
    // Special workaround for Symbol which can't be stringified.
    auto str = symbolDescriptiveString(runtime, Handle<SymbolID>::vmcast(base));
    if (str != ExecutionStatus::EXCEPTION) {
      valueAsString = *str;
    } else {
      runtime.clearThrownValue();
      valueAsString = StringPrimitive::createNoThrow(
          runtime, "<<Exception occurred getting the value>>");
    }
  } else {
    auto str = toString_RJS(runtime, base);
    assert(
        str != ExecutionStatus::EXCEPTION &&
        "Primitives should be convertible to string without exceptions");
    valueAsString = std::move(*str);
  }
  StringView valueAsStringPrintable =
      StringPrimitive::createStringView(runtime, valueAsString);

  SmallU16String<32> tmp1;
  SmallU16String<32> tmp2;
  return runtime.raiseTypeError(
      TwineChar16("Cannot create property '") + propName + "' on " +
      baseTypeAsString.getUTF16Ref(tmp1) + " '" +
      valueAsStringPrintable.getUTF16Ref(tmp2) + "'");
}

ExecutionStatus Interpreter::putByIdTransient_RJS(
    Runtime &runtime,
    Handle<> base,
    SymbolID id,
    Handle<> value,
    bool strictMode) {
  // ES5.1 8.7.2 special [[Get]] internal method.
  // TODO: avoid boxing primitives unless we are calling an accessor.

  // 1. Let O be ToObject(base)
  auto res = toObject(runtime, base);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    // If an exception is thrown, likely we are trying to convert
    // undefined/null to an object. Passing over the name of the property
    // so that we could emit more meaningful error messages.
    return amendPropAccessErrorMsgWithPropName(runtime, base, "set", id);
  }

  auto O = runtime.makeHandle<JSObject>(res.getValue());

  NamedPropertyDescriptor desc;
  JSObject *propObj = JSObject::getNamedDescriptorUnsafe(O, runtime, id, desc);

  // Is this a missing property, or a data property defined in the prototype
  // chain? In both cases we would need to create an own property on the
  // transient object, which is prohibited.
  if (!propObj ||
      (propObj != O.get() &&
       (!desc.flags.accessor && !desc.flags.proxyObject))) {
    if (strictMode) {
      return transientObjectPutErrorMessage(runtime, base, id);
    }
    return ExecutionStatus::RETURNED;
  }

  // Modifying an own data property in a transient object is prohibited.
  if (!desc.flags.accessor && !desc.flags.proxyObject) {
    if (strictMode) {
      return runtime.raiseTypeError(
          "Cannot modify a property in a transient object");
    }
    return ExecutionStatus::RETURNED;
  }

  if (desc.flags.accessor) {
    // This is an accessor.
    auto *accessor = vmcast<PropertyAccessor>(
        JSObject::getNamedSlotValueUnsafe(propObj, runtime, desc)
            .getObject(runtime));

    // It needs to have a setter.
    if (!accessor->setter) {
      if (strictMode) {
        return runtime.raiseTypeError("Cannot modify a read-only accessor");
      }
      return ExecutionStatus::RETURNED;
    }

    CallResult<PseudoHandle<>> setRes = Callable::executeCall1(
        runtime.makeHandle(accessor->setter), runtime, base, *value);
    if (setRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    assert(desc.flags.proxyObject && "descriptor flags are impossible");
    CallResult<bool> setRes = JSProxy::setNamed(
        runtime.makeHandle(propObj), runtime, id, value, base);
    if (setRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*setRes && strictMode) {
      return runtime.raiseTypeError("transient proxy set returned false");
    }
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus doPutByIdSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip,
    CodeBlock *curCodeBlock,
    uint32_t idVal,
    bool strictMode,
    bool tryProp) {
  return Interpreter::putByIdSlowPath_RJS(
      runtime,
      curCodeBlock,
      &O1REG(PutByIdLoose),
      &O2REG(PutByIdLoose),
      ip->iPutByIdLoose.op3,
      ID(idVal),
      strictMode,
      tryProp);
}

ExecutionStatus Interpreter::putByIdSlowPath_RJS(
    Runtime &runtime,
    CodeBlock *curCodeBlock,
    PinnedHermesValue *base,
    PinnedHermesValue *value,
    uint8_t cacheIdx,
    SymbolID id,
    bool strictMode,
    bool tryProp) {
  if (LLVM_UNLIKELY(!base->isObject())) {
    ++NumPutByIdTransient;
    assert(!tryProp && "TryPutById can only be used on the global object");
    auto retStatus = Interpreter::putByIdTransient_RJS(
        runtime, Handle<>(base), id, Handle<>(value), strictMode);
    if (retStatus == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    return ExecutionStatus::RETURNED;
  }

  auto shv = SmallHermesValue::encodeHermesValue(*value, runtime);
  auto *obj = vmcast<JSObject>(*base);
  auto *cacheEntry = curCodeBlock->getWriteCacheEntry(cacheIdx);
  CompressedPointer clazzPtr{obj->getClassGCPtr()};

  NamedPropertyDescriptor desc;
  OptValue<bool> hasOwnProp =
      JSObject::tryGetOwnNamedDescriptorFast(obj, runtime, id, desc);
  if (LLVM_LIKELY(hasOwnProp.hasValue() && hasOwnProp.getValue()) &&
      !desc.flags.accessor && desc.flags.writable &&
      !desc.flags.internalSetter) {
    ++NumPutByIdFastPaths;

    // cacheIdx == 0 indicates no caching so don't update the cache in
    // those cases.
    HiddenClass *clazz = vmcast<HiddenClass>(clazzPtr.getNonNull(runtime));
    if (LLVM_LIKELY(!clazz->isDictionaryNoCache()) &&
        LLVM_LIKELY(cacheIdx != hbc::PROPERTY_CACHING_DISABLED) &&
        LLVM_LIKELY(desc.slot <= WritePropertyCacheEntry::kMaxSlot)) {
#ifdef HERMES_SLOW_DEBUG
      if (cacheEntry->clazz && cacheEntry->clazz != clazzPtr)
        ++NumPutByIdCacheEvicts;
#else
      (void)NumPutByIdCacheEvicts;
#endif
      // Cache the class and property slot.
      // Preserve the addCacheIndex if the slot hasn't changed.
      cacheEntry->clazz = clazzPtr;
      cacheEntry->setSlot(desc.slot);
    }

    // This must be valid because an own property was already found.
    JSObject::setNamedSlotValueUnsafe(obj, runtime, desc.slot, shv);
    return ExecutionStatus::RETURNED;
  }
  const PropOpFlags defaultPropOpFlags = DEFAULT_PROP_OP_FLAGS(strictMode);
  auto putRes = JSObject::putNamed_RJS(
      Handle<JSObject>::vmcast(base),
      runtime,
      id,
      Handle<>(value),
      !tryProp ? defaultPropOpFlags : defaultPropOpFlags.plusMustExist(),
      cacheIdx != hbc::PROPERTY_CACHING_DISABLED
          ? curCodeBlock->getRuntimeModule()
          : nullptr,
      /* unit */ nullptr,
      cacheIdx != hbc::PROPERTY_CACHING_DISABLED ? cacheEntry : nullptr);
  if (LLVM_UNLIKELY(putRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

CallResult<HiddenClass *> Interpreter::getHiddenClassForBuffer(
    Runtime &runtime,
    CodeBlock *curCodeBlock,
    Handle<JSObject> parent,
    unsigned shapeTableIndex,
    ObjectAllocKind allocKind) {
  RuntimeModule *runtimeModule = curCodeBlock->getRuntimeModule();
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (auto *cachedClazz = runtimeModule->findCachedLiteralHiddenClass(
          runtime, *parent, shapeTableIndex)) {
    lv.clazz = cachedClazz;
    assert(lv.clazz->getObjectParent(runtime) == *parent && "wrong parent");
  } else {
    const auto *shapeInfo =
        &runtimeModule->getBytecode()->getObjectShapeTable()[shapeTableIndex];
    if (shapeInfo->numProps > HiddenClass::maxNumProperties()) {
      return runtime.raiseRangeError(
          TwineChar16("Object has more than ") +
          HiddenClass::maxNumProperties() + " properties");
    }

    auto keyBuffer = runtimeModule->getBytecode()->getObjectKeyBuffer().slice(
        shapeInfo->keyBufferOffset);

    if (isTypedAllocKind(allocKind)) {
      bool propertiesEnumerable =
          allocKind != ObjectAllocKind::TypedNonEnumerable;
      lv.clazz = HiddenClass::createForTypedObject(
          runtime, parent, shapeInfo->numProps);
      addTypedBufferPropertiesToHiddenClass(
          runtime,
          keyBuffer,
          shapeInfo->numProps,
          lv.clazz,
          propertiesEnumerable,
          [runtimeModule](StringID id) {
            return runtimeModule->getSymbolIDMustExist(id);
          });
    } else {
      auto *rootClazz =
          *runtime.getHiddenClassForPrototype(*parent, runtime.classJSObject);

      // Ensure that the hidden class does not start out with any properties, so
      // we just need to check the shape table entry.
      assert(rootClazz->getNumProperties() == 0);
      lv.clazz = addBufferPropertiesToHiddenClass(
          runtime,
          keyBuffer,
          shapeInfo->numProps,
          rootClazz,
          [runtimeModule](StringID id) {
            return runtimeModule->getSymbolIDMustExist(id);
          });
    }

    assert(
        shapeInfo->numProps == lv.clazz->getNumProperties() &&
        "numLiterals should match hidden class property count.");
    // Dictionary mode classes cannot be cached since they can change as the
    // resulting object is modified.
    if (LLVM_LIKELY(!lv.clazz->isDictionary()) || isTypedAllocKind(allocKind)) {
      runtimeModule->setCachedLiteralHiddenClass(
          runtime, shapeTableIndex, *lv.clazz);
    }
  }

  return *lv.clazz;
}

PseudoHandle<> Interpreter::createObjectWithParent(
    Runtime &runtime,
    CodeBlock *curCodeBlock,
    Handle<JSObject> parent,
    unsigned shapeTableIndex) {
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // No properties are added, so this can only fail on allocation failure.
  lv.clazz = runtime.ignoreAllocationFailure(getHiddenClassForBuffer(
      runtime,
      curCodeBlock,
      parent,
      shapeTableIndex,
      ObjectAllocKind::Untyped));

  // Create a new object using the built-in constructor or cached hidden class.
  // Note that the built-in constructor is empty, so we don't actually need to
  // call it.
  return createPseudoHandle(
      JSObject::create(runtime, parent, lv.clazz).getHermesValue());
}

CallResult<PseudoHandle<>> Interpreter::createObjectFromBuffer(
    Runtime &runtime,
    CodeBlock *curCodeBlock,
    Handle<JSObject> parent,
    unsigned shapeTableIndex,
    unsigned valBufferOffset,
    ObjectAllocKind allocKind) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  CallResult<HiddenClass *> clazzRes = getHiddenClassForBuffer(
      runtime, curCodeBlock, parent, shapeTableIndex, allocKind);
  if (LLVM_UNLIKELY(clazzRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.clazz = *clazzRes;

  // Create a new object using the built-in constructor or cached hidden class.
  // Note that the built-in constructor is empty, so we don't actually need to
  // call it.
  lv.obj = JSObject::create(runtime, parent, lv.clazz).get();
  assert(lv.obj->getParent(runtime) == *parent);
  auto numLiterals = lv.clazz->getNumProperties();

  // Set up the visitor to populate property values in the object.
  struct {
    void visitStringID(StringID id) {
      auto shv = SmallHermesValue::encodeStringValue(
          runtimeModule->getStringPrimFromStringIDMayAllocate(id), runtime);
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, propIndex++, shv);
    }
    void visitNumber(double d) {
      auto shv = SmallHermesValue::encodeNumberValue(d, runtime);
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, propIndex++, shv);
    }
    void visitNull() {
      static constexpr auto shv = SmallHermesValue::encodeNullValue();
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, propIndex++, shv);
    }
    void visitUndefined() {
      static constexpr auto shv = SmallHermesValue::encodeUndefinedValue();
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, propIndex++, shv);
    }
    void visitBool(bool b) {
      auto shv = SmallHermesValue::encodeBoolValue(b);
      JSObject::setNamedSlotValueUnsafe(*obj, runtime, propIndex++, shv);
    }

    Handle<JSObject> obj;
    Runtime &runtime;
    RuntimeModule *runtimeModule;
    size_t propIndex;
  } v{lv.obj, runtime, curCodeBlock->getRuntimeModule(), 0};

  // Visit each value in the given buffer, and set it in the object.
  SerializedLiteralParser::parseValueBuffer(
      curCodeBlock->getRuntimeModule()
          ->getBytecode()
          ->getLiteralValueBuffer()
          .slice(valBufferOffset),
      numLiterals,
      v);

  return createPseudoHandle(lv.obj.getHermesValue());
}

ExecutionStatus Interpreter::caseNewObjectWithBufferAndParent(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    CodeBlock *curCodeBlock,
    const inst::Inst *ip) {
  Handle<JSObject> parent = O2REG(NewObjectWithParent).isObject()
      ? Handle<JSObject>::vmcast(&O2REG(NewObjectWithParent))
      : O2REG(NewObjectWithParent).isNull()
      ? Runtime::makeNullHandle<JSObject>()
      : Handle<JSObject>::vmcast(&runtime.objectPrototype);
  auto res = createObjectFromBuffer(
      runtime,
      curCodeBlock,
      parent,
      ip->iNewObjectWithBufferAndParent.op3,
      ip->iNewObjectWithBufferAndParent.op4,
      ObjectAllocKind::Untyped);
  if (res == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  O1REG(NewObjectWithBufferAndParent) = res->getHermesValue();
  return ExecutionStatus::RETURNED;
}

CallResult<PseudoHandle<>> Interpreter::createArrayFromBuffer(
    Runtime &runtime,
    CodeBlock *curCodeBlock,
    unsigned numElements,
    unsigned numLiterals,
    unsigned bufferIndex) {
  // Create a new array using the built-in constructor, and initialize
  // the elements from a literal array buffer.
  auto arrRes = JSArray::create(runtime, numElements, numElements);
  if (arrRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // Resize the array storage in advance.
  Handle<JSArray> arr = runtime.makeHandle(std::move(*arrRes));
  JSArray::setStorageEndIndex(arr, runtime, numElements);

  // Set up the visitor to populate literal elements in the array.
  struct {
    void visitStringID(StringID id) {
      auto shv = SmallHermesValue::encodeStringValue(
          runtimeModule->getStringPrimFromStringIDMayAllocate(id), runtime);
      JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
    }
    void visitNumber(double d) {
      auto shv = SmallHermesValue::encodeNumberValue(d, runtime);
      JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
    }
    void visitNull() {
      constexpr auto shv = SmallHermesValue::encodeNullValue();
      JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
    }
    void visitUndefined() {
      constexpr auto shv = SmallHermesValue::encodeUndefinedValue();
      JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
    }
    void visitBool(bool b) {
      auto shv = SmallHermesValue::encodeBoolValue(b);
      JSArray::unsafeSetExistingElementAt(*arr, runtime, i++, shv);
    }

    Handle<JSArray> arr;
    Runtime &runtime;
    RuntimeModule *runtimeModule;
    JSArray::size_type i;
  } v{arr, runtime, curCodeBlock->getRuntimeModule(), 0};

  // Visit each serialized value in the given buffer.
  SerializedLiteralParser::parseValueBuffer(
      curCodeBlock->getRuntimeModule()
          ->getBytecode()
          ->getLiteralValueBuffer()
          .slice(bufferIndex),
      numLiterals,
      v);

  return createPseudoHandle(HermesValue::encodeObjectValue(*arr));
}

const Inst *doStringSwitchImm(
    CodeBlock *curCodeBlock,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  if (LLVM_LIKELY(O1REG(StringSwitchImm).isString())) {
    RuntimeModule *runtimeModule = curCodeBlock->getRuntimeModule();
    auto *runtimeTables = runtimeModule->getStringSwitchImmTables();
    assert(
        ip->iStringSwitchImm.op2 < runtimeModule->numStringSwitchImmTables() &&
        "String Switch index out of range.");
    StringSwitchDenseMap &runtimeTable =
        runtimeTables[ip->iStringSwitchImm.op2];
    if (LLVM_UNLIKELY(runtimeTable.empty())) {
      const auto *bytecodeTable =
          reinterpret_cast<const hbc::StringSwitchTableCase *>(llvh::alignAddr(
              (const uint8_t *)ip + ip->iStringSwitchImm.op3,
              alignof(hbc::StringSwitchTableCase)));
      runtimeModule->initializeStringSwitchImmTable(
          runtimeTable, bytecodeTable, ip->iStringSwitchImm.op5);
    }
    StringPrimitive *strVal = O1REG(StringSwitchImm).getString();
    auto iter = runtimeTable.find(strVal);
    if (iter != runtimeTable.end()) {
      return IPADD(iter->second.bytecodeOffset);
    }
  }
  // Wrong type or not found, jump to default.
  return IPADD(ip->iStringSwitchImm.op4);
}

} // namespace vm
} // namespace hermes

#undef DEBUG_TYPE
