/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"

#include "hermes/Regex/Executor.h"
#include "hermes/Regex/Regex.h"
#include "hermes/Regex/RegexTraits.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"

namespace hermes {
namespace vm {

Handle<NativeConstructor> defineSystemConstructor(
    Runtime &runtime,
    SymbolID name,
    NativeFunctionPtr nativeFunctionPtr,
    Handle<JSObject> prototypeObjectHandle,
    Handle<JSObject> constructorProtoObjectHandle,
    unsigned paramCount,
    MutableHandle<NativeConstructor> constructorOut) {
  struct : public Locals {
    PinnedValue<NativeConstructor> constructor;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.constructor = NativeConstructor::create(
      runtime,
      constructorProtoObjectHandle,
      nullptr,
      nativeFunctionPtr,
      paramCount);

  auto st = Callable::defineNameLengthAndPrototype(
      lv.constructor,
      runtime,
      name,
      paramCount,
      prototypeObjectHandle,
      Callable::WritablePrototype::No);
  (void)st;
  assert(
      st != ExecutionStatus::EXCEPTION && "defineLengthAndPrototype() failed");

  // Define the global.
  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();

  auto res = JSObject::defineOwnProperty(
      runtime.getGlobal(), runtime, name, dpf, lv.constructor);
  assert(
      res != ExecutionStatus::EXCEPTION && *res &&
      "defineOwnProperty() failed");
  (void)res;

  constructorOut.castAndSetHermesValue<NativeConstructor>(
      lv.constructor.getHermesValue());
  return constructorOut;
}

NativeFunction *defineMethod(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID propertyName,
    SymbolID methodName,
    void *context,
    NativeFunctionPtr nativeFunctionPtr,
    unsigned paramCount,
    DefinePropertyFlags dpf) {
  GCScope gcScope{runtime};
  struct : public Locals {
    PinnedValue<NativeFunction> method;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.method = NativeFunction::create(
      runtime, context, nativeFunctionPtr, methodName, paramCount);

  auto res = JSObject::defineOwnProperty(
      objectHandle, runtime, propertyName, dpf, lv.method);
  (void)res;
  assert(
      res != ExecutionStatus::EXCEPTION && *res &&
      "defineOwnProperty() failed");

  return lv.method.get();
}

Handle<NativeConstructor> defineSystemConstructor(
    Runtime &runtime,
    SymbolID name,
    NativeFunctionPtr nativeFunctionPtr,
    Handle<JSObject> prototypeObjectHandle,
    unsigned paramCount,
    MutableHandle<NativeConstructor> constructorOut) {
  return defineSystemConstructor(
      runtime,
      name,
      nativeFunctionPtr,
      prototypeObjectHandle,
      Handle<JSObject>::vmcast(&runtime.functionPrototype),
      paramCount,
      constructorOut);
}

NativeFunction *defineMethod(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    void *context,
    NativeFunctionPtr nativeFunctionPtr,
    unsigned paramCount) {
  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();
  return defineMethod(
      runtime,
      objectHandle,
      name,
      name,
      context,
      nativeFunctionPtr,
      paramCount,
      dpf);
}

void defineAccessor(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID propertyName,
    SymbolID methodName,
    void *context,
    NativeFunctionPtr getterFunc,
    NativeFunctionPtr setterFunc,
    bool enumerable,
    bool configurable) {
  assert(
      (getterFunc || setterFunc) &&
      "at least a getter or a setter must be specified");
  ExecutionStatus status{};
  (void)status;

  GCScope gcScope{runtime};

  struct : public Locals {
    PinnedValue<NativeFunction> getter;
    PinnedValue<NativeFunction> setter;
    PinnedValue<PropertyAccessor> accessor;
    PinnedValue<SymbolID> getterName;
    PinnedValue<SymbolID> setterName;
    PinnedValue<StringPrimitive> methodNameStr;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  StringView nameView =
      runtime.getIdentifierTable().getStringView(methodName, lv.methodNameStr);
  assert(nameView.isASCII() && "Only ASCII accessors are supported");
  if (getterFunc) {
    // Set the name by prepending "get ".
    llvh::SmallString<32> getterName{"get "};
    llvh::raw_svector_ostream os{getterName};
    os << nameView;

    auto strRes = runtime.ignoreAllocationFailure(
        StringPrimitive::create(runtime, getterName));
    runtime.ignoreAllocationFailure(
        runtime.getIdentifierTable().getSymbolFromPrimitive(
            runtime,
            createPseudoHandle(vmcast<StringPrimitive>(strRes)),
            lv.getterName));

    auto funcRes =
        NativeFunction::create(runtime, context, getterFunc, *lv.getterName, 0);
    lv.getter = funcRes.get();
  }
  if (setterFunc) {
    // Set the name by prepending "set ".
    llvh::SmallString<32> setterName{"set "};
    llvh::raw_svector_ostream os{setterName};
    os << nameView;

    auto strRes = runtime.ignoreAllocationFailure(
        StringPrimitive::create(runtime, setterName));
    runtime.ignoreAllocationFailure(
        runtime.getIdentifierTable().getSymbolFromPrimitive(
            runtime,
            createPseudoHandle(vmcast<StringPrimitive>(strRes)),
            lv.setterName));

    auto funcRes =
        NativeFunction::create(runtime, context, setterFunc, *lv.setterName, 1);
    lv.setter = funcRes.get();
  }

  lv.accessor = PropertyAccessor::create(runtime, lv.getter, lv.setter);

  DefinePropertyFlags dpf{};
  dpf.setEnumerable = 1;
  dpf.setConfigurable = 1;
  dpf.setGetter = 1;
  dpf.setSetter = 1;
  dpf.enumerable = enumerable;
  dpf.configurable = configurable;

  auto res = JSObject::defineOwnProperty(
      objectHandle, runtime, propertyName, dpf, lv.accessor);
  (void)res;
  assert(
      res != ExecutionStatus::EXCEPTION && *res &&
      "defineOwnProperty() failed");
}

void defineProperty(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    Handle<> value,
    DefinePropertyFlags dpf) {
  auto res = JSObject::defineOwnProperty(
      objectHandle, runtime, name, dpf, value, PropOpFlags());
  (void)res;
  assert(
      res != ExecutionStatus::EXCEPTION && *res &&
      "defineOwnProperty() failed");
}

void defineProperty(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    Handle<> value) {
  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();

  return defineProperty(runtime, objectHandle, name, value, dpf);
}

static std::vector<uint8_t> getReturnThisRegexBytecode() {
  const char16_t *returnThisRE = uR"X(^\s*return[ \t]+this\s*;?\s*$)X";
  return regex::Regex<regex::UTF16RegexTraits>(returnThisRE).compile();
}

static bool isReturnThis(Handle<StringPrimitive> str, Runtime &runtime) {
  // Fast check for minimal version.
  {
    auto minimal = runtime.getPredefinedString(Predefined::returnThis);
    if (str->equals(minimal)) {
      return true;
    }
  }
  // Regex match for variants.
  auto input = StringPrimitive::createStringView(runtime, str);
  static auto bytecode = getReturnThisRegexBytecode();
  auto result = regex::MatchRuntimeResult::NoMatch;
  if (input.isASCII()) {
    const char *begin = input.castToCharPtr();
    result = regex::searchWithBytecode(
        bytecode,
        begin,
        0,
        input.length(),
        nullptr,
        regex::constants::matchDefault | regex::constants::matchInputAllAscii,
        runtime.getOverflowGuardForRegex());
  } else {
    const char16_t *begin = input.castToChar16Ptr();
    result = regex::searchWithBytecode(
        bytecode,
        begin,
        0,
        input.length(),
        nullptr,
        regex::constants::matchDefault,
        runtime.getOverflowGuardForRegex());
  }
  return result == regex::MatchRuntimeResult::Match;
}

CallResult<HermesValue> createDynamicFunction(
    Runtime &runtime,
    NativeArgs args,
    DynamicFunctionKind kind) {
  GCScope gcScope(runtime);

  // Number of arguments supplied to Function().
  uint32_t argCount = args.getArgCount();

  // Number of parameters in the resultant function.
  uint32_t paramCount = argCount > 0 ? argCount - 1 : 0;

  // List of the parameter strings for the resultant function..
  auto arrRes = JSArray::create(runtime, paramCount, paramCount);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSArray> params;
    PinnedValue<StringPrimitive> body;
    PinnedValue<StringPrimitive> param;
    PinnedValue<StringPrimitive> element;
    PinnedValue<StringPrimitive> result;
    PinnedValue<JSFunction> function;
    PinnedValue<JSObject> fallbackProto;
    PinnedValue<JSObject> selfParent;
    PinnedValue<Domain> domain;
    PinnedValue<StringPrimitive> anonymousStr;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.params = std::move(*arrRes);

  // Body of the resultant function.

  // String length of the function in its entirety.
  // Account for commas in the argument list initially.
  // If at least two arguments to the function (3 in total), there's a comma.
  SafeUInt32 size{paramCount > 0 ? paramCount - 1 : 0};

  // es2020 19.2.1.1.1 Runtime Semantics: CreateDynamicFunction: If this is a
  // construct call, we must use new.target.prototype as the parent of the
  // function we are going to create. In order to avoid this `.prototype`
  // lookup, we make an informed guess as to what the new.target is based on the
  // function kind. If this guess is correct, we immediately know the
  // `.prototype` without a lookup. We also use this precomputed parent in the
  // case that the lookup gives a value which cannot be used as the parent, e.g.
  // it's not a function.
  PinnedValue<NativeConstructor> *expectedNewTarget;
  switch (kind) {
    case DynamicFunctionKind::Normal:
      lv.fallbackProto = Handle<JSObject>::vmcast(&runtime.functionPrototype);
      expectedNewTarget = &runtime.functionConstructor;
      break;
    case DynamicFunctionKind::Generator:
      lv.fallbackProto =
          Handle<JSObject>::vmcast(&runtime.generatorFunctionPrototype);
      expectedNewTarget = &runtime.generatorFunctionConstructor;
      break;
    case DynamicFunctionKind::Async:
      lv.fallbackProto =
          Handle<JSObject>::vmcast(&runtime.asyncFunctionPrototype);
      expectedNewTarget = &runtime.asyncFunctionConstructor;
      break;
    default:
      llvm_unreachable("unknown kind for CreateDynamicFunction.");
  }

  if (LLVM_LIKELY(
          !args.isConstructorCall() ||
          (args.getNewTarget().getRaw() ==
           expectedNewTarget->getHermesValue().getRaw()))) {
    lv.selfParent = lv.fallbackProto;
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            lv.fallbackProto);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
  }

  // Whether the params and body are all ASCII; the rest of the source is.
  bool allASCII = true;
  if (argCount == 0) {
    // No arguments, just set body to be the empty string.
    lv.body = runtime.getPredefinedString(Predefined::emptyString);
  } else {
    // If there's arguments, store the parameters and the provided body.
    auto marker = gcScope.createMarker();
    for (uint32_t i = 0; i < paramCount; ++i) {
      gcScope.flushToMarker(marker);
      auto strRes = toString_RJS(runtime, args.getArgHandle(i));
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.param = std::move(*strRes);
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.params, runtime, i, lv.param) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      size.add(lv.param->getStringLength());
      allASCII &= lv.param->isASCII();
    }

    // Last parameter is the body.
    auto strRes = toString_RJS(runtime, args.getArgHandle(paramCount));
    if (strRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.body = strRes->get();
    size.add(lv.body->getStringLength());
    allASCII &= lv.body->isASCII();

    if (kind == DynamicFunctionKind::Normal && argCount == 1 &&
        isReturnThis(lv.body, runtime)) {
      // If this raises an exception, we still return immediately.
      lv.domain = Domain::create(runtime);
      return JSFunction::create(
                 runtime,
                 lv.domain,
                 lv.selfParent,
                 runtime.makeNullHandle<Environment>(),
                 runtime.getReturnThisCodeBlock())
          .getHermesValue();
    }
  }

  // Constant parts of the function.
  ASCIIRef functionHeader;
  switch (kind) {
    case DynamicFunctionKind::Normal:
      functionHeader = createASCIIRef("(function (");
      break;
    case DynamicFunctionKind::Generator:
      functionHeader = createASCIIRef("(function*(");
      break;
    case DynamicFunctionKind::Async:
      functionHeader = createASCIIRef("(async function (");
      break;
    default:
      llvm_unreachable("unknown kind for CreateDynamicFunction.");
  };
  size.add(functionHeader.size());

  auto bodyHeader = createASCIIRef("){");
  size.add(bodyHeader.size());

  // Note: add a \n before the closing '}' in case the
  // function body ends with a line comment.
  auto functionFooter = createASCIIRef("\n})");
  size.add(functionFooter.size());

  auto separator = createASCIIRef(",");

  auto builder =
      StringBuilder::createStringBuilder(runtime, lv.result, size, allASCII);
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  builder->appendASCIIRef(functionHeader);
  for (uint32_t i = 0; i < paramCount; ++i) {
    // Copy params into str.
    lv.element = lv.params->at(runtime, i).getString(runtime);
    builder->appendStringPrim(lv.element);
    if (i < paramCount - 1) {
      // If there's more params left to put, need to add a comma.
      // We wouldn't have entered the loop if paramCount == 0,
      // so there's no overflow here.
      builder->appendASCIIRef(separator);
    }
  }
  builder->appendASCIIRef(bodyHeader);
  builder->appendStringPrim(lv.body);
  builder->appendASCIIRef(functionFooter);

  auto evalRes =
      directEval(runtime, builder->getStringPrimitive(), false, {}, true);
  if (evalRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  lv.function = vmcast<JSFunction>(evalRes.getValue());

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.enumerable = 0;
  dpf.writable = 0;

  // Define the `name` correctly.
  lv.anonymousStr = runtime.getStringPrimFromSymbolID(
      Predefined::getSymbolID(Predefined::anonymous));
  if (JSObject::defineOwnProperty(
          lv.function,
          runtime,
          Predefined::getSymbolID(Predefined::name),
          dpf,
          lv.anonymousStr) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // Set the parent.  This could be done by threading the argument
  // through to Runtime::runBytecode where the object is actually
  // created, but this is the only place we need to do this so it
  // keeps the code simpler.
  CallResult<bool> parentRes =
      JSObject::setParent(*lv.function, runtime, *lv.selfParent);
  if (LLVM_UNLIKELY(parentRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(
      *parentRes && "Setting prototype on new dynamic function returned false");

  return lv.function.getHermesValue();
}

} // namespace vm
} // namespace hermes
