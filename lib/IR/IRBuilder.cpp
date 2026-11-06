/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "llvh/Support/Casting.h"
#include "llvh/Support/ErrorHandling.h"

#include "hermes/AST/Context.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRBuilder.h"

using namespace hermes;

BasicBlock *IRBuilder::createBasicBlock(Function *Parent) {
  assert(Parent && "Invalid insertion point");
  return new BasicBlock(Parent);
}

NormalFunction *IRBuilder::createFunction(
    Identifier OriginalName,
    Function::DefinitionKind definitionKind,
    bool strictMode,
    CustomDirectives customDirectives,
    SMRange sourceRange,
    Function *insertBefore) {
  // Function must have a name. If the source doesn't provide the function name,
  // Hermes will try to infer the name from the name of a variable or property
  // it is assigned to. If there was no inference, an empty string is used.
  if (!OriginalName.isValid()) {
    OriginalName = createIdentifier("");
  }
  return new NormalFunction(
      M,
      OriginalName,
      definitionKind,
      strictMode,
      customDirectives,
      sourceRange,
      insertBefore);
}

GeneratorFunction *IRBuilder::createGeneratorFunction(
    Identifier OriginalName,
    Function::DefinitionKind definitionKind,
    bool strictMode,
    CustomDirectives customDirectives,
    SMRange sourceRange,
    Function *insertBefore) {
  if (!OriginalName.isValid()) {
    // Function must have a name, even it's empty.
    // Eventually we will give it a properly inferred name.
    OriginalName = createIdentifier("");
  }
  return new GeneratorFunction(
      M,
      OriginalName,
      definitionKind,
      strictMode,
      customDirectives,
      sourceRange,
      insertBefore);
}

Function *IRBuilder::createTopLevelFunction(
    llvh::StringRef topLevelFunctionName,
    bool strictMode,
    CustomDirectives customDirectives,
    SMRange sourceRange) {
  // Notice that this synthesized name is not a legal javascript name and
  // can't collide with functions in the processed program.
  auto *F = createFunction(
      topLevelFunctionName,
      Function::DefinitionKind::ES5Function,
      strictMode,
      customDirectives,
      sourceRange);
  M->setTopLevelFunction(F);
  return F;
}

NormalFunction *IRBuilder::createFunction(
    llvh::StringRef OriginalName,
    Function::DefinitionKind definitionKind,
    bool strictMode,
    CustomDirectives customDirectives,
    SMRange sourceRange,
    Function *insertBefore) {
  Identifier OrigIden =
      OriginalName.empty() ? Identifier{} : createIdentifier(OriginalName);
  return createFunction(
      OrigIden,
      definitionKind,
      strictMode,
      customDirectives,
      sourceRange,
      insertBefore);
}

AsyncFunction *IRBuilder::createAsyncFunction(
    Identifier OriginalName,
    Function::DefinitionKind definitionKind,
    bool strictMode,
    CustomDirectives customDirectives,
    SMRange sourceRange,
    Function *insertBefore) {
  if (!OriginalName.isValid()) {
    // Function must have a name, even it's empty.
    // Eventually we will give it a properly inferred name.
    OriginalName = createIdentifier("");
  }
  return new AsyncFunction(
      M,
      OriginalName,
      definitionKind,
      strictMode,
      customDirectives,
      sourceRange,
      insertBefore);
}

GlobalObjectProperty *IRBuilder::createGlobalObjectProperty(
    Identifier name,
    bool declared) {
  return M->addGlobalProperty(name, declared);
}
GlobalObjectProperty *IRBuilder::createGlobalObjectProperty(
    llvh::StringRef name,
    bool declared) {
  return createGlobalObjectProperty(
      M->getContext().getIdentifier(name), declared);
}

/// Add a new JS parameter to function \p Parent.
JSDynamicParam *IRBuilder::createJSDynamicParam(
    Function *parent,
    Identifier name) {
  auto *param = new JSDynamicParam(parent, name);
  parent->addJSDynamicParam(param);
  return param;
}

/// Add a new JS parameter to function \p Parent.
JSDynamicParam *IRBuilder::createJSDynamicParam(
    Function *parent,
    llvh::StringRef name) {
  auto *param = new JSDynamicParam(parent, createIdentifier(name));
  parent->addJSDynamicParam(param);
  return param;
}

JSDynamicParam *IRBuilder::createJSThisParam(Function *parent) {
  auto *param = new JSDynamicParam(parent, createIdentifier("<this>"));
  parent->addJSThisParam(param);
  return param;
}

Variable *IRBuilder::createVariable(
    VariableScope *Parent,
    Identifier Name,
    Type type,
    bool hidden) {
  auto *var = new Variable(Parent, Name, hidden);
  var->setType(type);
  return var;
}

Variable *IRBuilder::createVariable(
    VariableScope *Parent,
    const llvh::Twine &Name,
    Type type,
    bool hidden) {
  return createVariable(Parent, createIdentifier(Name), type, hidden);
}

VariableScope *IRBuilder::createVariableScope(VariableScope *parentScope) {
  auto *newScope = new VariableScope(parentScope);
  M->addVariableScope(newScope);
  return newScope;
}

LiteralNumber *IRBuilder::getLiteralNumber(double value) {
  return M->getLiteralNumber(value);
}

// FIXME: use proper language semantics.
LiteralNumber *IRBuilder::getLiteralPositiveZero() {
  return M->getLiteralNumber(+0.0);
}

LiteralNumber *IRBuilder::getLiteralNegativeZero() {
  return M->getLiteralNumber(-0.0);
}

LiteralNumber *IRBuilder::getLiteralInfinity() {
  return M->getLiteralNumber(std::numeric_limits<double>::infinity());
}

LiteralNumber *IRBuilder::getLiteralNaN() {
  return M->getLiteralNumber(std::numeric_limits<double>::quiet_NaN());
}

LiteralBigInt *IRBuilder::getLiteralBigInt(UniqueString *value) {
  return M->getLiteralBigInt(value);
}

LiteralString *IRBuilder::getLiteralString(const llvh::Twine &value) {
  Identifier Iden = createIdentifier(value);
  return getLiteralString(Iden);
}

LiteralString *IRBuilder::getLiteralString(Identifier value) {
  return M->getLiteralString(value);
}

LiteralPrivateName *IRBuilder::getLiteralPrivateName(Identifier value) {
  return M->getLiteralPrivateName(value);
}

LiteralBool *IRBuilder::getLiteralBool(bool value) {
  return M->getLiteralBool(value);
}

LiteralEmpty *IRBuilder::getLiteralEmpty() {
  return M->getLiteralEmpty();
}

LiteralUndefined *IRBuilder::getLiteralUndefined() {
  return M->getLiteralUndefined();
}

LiteralNull *IRBuilder::getLiteralNull() {
  return M->getLiteralNull();
}

GlobalObject *IRBuilder::getGlobalObject() {
  return M->getGlobalObject();
}

EmptySentinel *IRBuilder::getEmptySentinel() {
  return M->getEmptySentinel();
}

Identifier IRBuilder::createIdentifier(const llvh::Twine &str) {
  return M->getContext().getIdentifier(str);
}

BranchInst *IRBuilder::createBranchInst(BasicBlock *Destination) {
  auto *BI = new BranchInst(getInsertionBlock(), Destination);
  insert(BI);
  return BI;
}

CondBranchInst *
IRBuilder::createCondBranchInst(Value *Cond, BasicBlock *T, BasicBlock *F) {
  auto *CBI = new CondBranchInst(getInsertionBlock(), Cond, T, F);
  insert(CBI);
  return CBI;
}

ReturnInst *IRBuilder::createReturnInst(Value *Val) {
  auto *RI = new ReturnInst(Val);
  insert(RI);
  return RI;
}

CatchInst *IRBuilder::createCatchInst() {
  auto *CI = new CatchInst();
  insert(CI);
  return CI;
}

ThrowInst *IRBuilder::createThrowInst(
    Value *thrownValue,
    BasicBlock *catchTarget) {
  auto *TI = new ThrowInst(thrownValue, catchTarget);
  insert(TI);
  return TI;
}

ThrowTypeErrorInst *IRBuilder::createThrowTypeErrorInst(
    Value *message,
    BasicBlock *catchTarget) {
  auto *TI = new ThrowTypeErrorInst(message, catchTarget);
  insert(TI);
  return TI;
}

TryStartInst *IRBuilder::createTryStartInst(
    BasicBlock *tryBodyBlock,
    BasicBlock *catchTargetBlock) {
  auto *I = new TryStartInst(tryBodyBlock, catchTargetBlock);
  insert(I);
  return I;
}

TryEndInst *IRBuilder::createTryEndInst(
    BasicBlock *catchBlock,
    BasicBlock *branchBlock) {
  auto *I = new TryEndInst(catchBlock, branchBlock);
  insert(I);
  return I;
}

BranchIfBuiltinInst *IRBuilder::createBranchIfBuiltinInst(
    BuiltinMethod::Enum builtinIndex,
    Value *argument,
    BasicBlock *catchBlock,
    BasicBlock *branchBlock) {
  auto *I = new BranchIfBuiltinInst(
      getLiteralBuiltinIdx(builtinIndex), argument, catchBlock, branchBlock);
  insert(I);
  return I;
}

AllocStackInst *IRBuilder::createAllocStackInst(
    const llvh::Twine &varName,
    Type type) {
  Identifier Iden = createIdentifier(varName);
  return createAllocStackInst(Iden, type);
}

AllocStackInst *IRBuilder::createAllocStackInst(Identifier varName, Type type) {
  auto *AHI = new AllocStackInst(varName);
  AHI->setType(type);
  insert(AHI);
  return AHI;
}

ToPropertyKeyInst *IRBuilder::createToPropertyKeyInst(Value *val) {
  auto *ANI = new ToPropertyKeyInst(val);
  insert(ANI);
  return ANI;
}

AsNumberInst *IRBuilder::createAsNumberInst(Value *val) {
  auto *ANI = new AsNumberInst(val);
  insert(ANI);
  return ANI;
}

AsNumericInst *IRBuilder::createAsNumericInst(Value *val) {
  auto *ANI = new AsNumericInst(val);
  insert(ANI);
  return ANI;
}

AsInt32Inst *IRBuilder::createAsInt32Inst(Value *val) {
  auto *AII = new AsInt32Inst(val);
  insert(AII);
  return AII;
}

AsUint32Inst *IRBuilder::createAsUint32Inst(Value *val) {
  auto *AUI = new AsUint32Inst(val);
  insert(AUI);
  return AUI;
}

AddEmptyStringInst *IRBuilder::createAddEmptyStringInst(Value *val) {
  auto *I = new AddEmptyStringInst(val);
  insert(I);
  return I;
}

CreatePrivateNameInst *IRBuilder::createCreatePrivateNameInst(
    LiteralString *descStr) {
  auto *I = new CreatePrivateNameInst(descStr);
  insert(I);
  return I;
}

CreateClassInst *IRBuilder::createCreateClassInst(
    BaseScopeInst *scope,
    Function *code,
    Value *superClass,
    AllocStackInst *homeObjectOutput) {
  auto CC = new CreateClassInst(scope, code, superClass, homeObjectOutput);
  insert(CC);
  return CC;
}

CreateFunctionInst *IRBuilder::createCreateFunctionInst(
    BaseScopeInst *scope,
    Function *code) {
  auto CFI = new CreateFunctionInst(scope, scope->getVariableScope(), code);
  insert(CFI);
  return CFI;
}

GetParentScopeInst *IRBuilder::createGetParentScopeInst(
    VariableScope *scope,
    JSSpecialParam *parentScopeParam) {
  auto GPS = new GetParentScopeInst(scope, parentScopeParam);
  insert(GPS);
  return GPS;
}

CreateScopeInst *IRBuilder::createCreateScopeInst(
    VariableScope *scope,
    Value *parentScope) {
  auto CSI = new CreateScopeInst(scope, parentScope);
  insert(CSI);
  return CSI;
}

ResolveScopeInst *IRBuilder::createResolveScopeInst(
    VariableScope *scope,
    VariableScope *startVarScope,
    Instruction *startScope) {
  auto RSI = new ResolveScopeInst(scope, startVarScope, startScope);
  insert(RSI);
  return RSI;
}

LIRResolveScopeInst *IRBuilder::createLIRResolveScopeInst(
    VariableScope *scope,
    Instruction *startScope,
    LiteralNumber *numLevels) {
  auto LRSI = new LIRResolveScopeInst(scope, startScope, numLevels);
  insert(LRSI);
  return LRSI;
}

GetClosureScopeInst *IRBuilder::createGetClosureScopeInst(
    VariableScope *scope,
    Function *F,
    Value *closure) {
  auto *GCSI = new GetClosureScopeInst(scope, F, closure);
  insert(GCSI);
  return GCSI;
}

LoadFrameInst *IRBuilder::createLoadFrameInst(
    Instruction *scope,
    Variable *ptr) {
  auto LI = new LoadFrameInst(scope, ptr);
  insert(LI);
  return LI;
}

LoadStackInst *IRBuilder::createLoadStackInst(AllocStackInst *ptr) {
  auto LI = new LoadStackInst(ptr);
  insert(LI);
  return LI;
}

StoreFrameInst *IRBuilder::createStoreFrameInst(
    Instruction *scope,
    Value *storedValue,
    Variable *ptr) {
  auto SI = new StoreFrameInst(scope, storedValue, ptr);
  insert(SI);
  return SI;
}

StoreStackInst *IRBuilder::createStoreStackInst(
    Value *storedValue,
    AllocStackInst *ptr) {
  auto SI = new StoreStackInst(storedValue, ptr);
  insert(SI);
  return SI;
}

CallInst *IRBuilder::createCallInst(
    Value *callee,
    Value *target,
    bool calleeIsAlwaysClosure,
    Value *env,
    Value *newTarget,
    Value *thisValue,
    ArrayRef<Value *> args) {
  auto CI = new CallInst(
      callee,
      target,
      getLiteralBool(calleeIsAlwaysClosure),
      env,
      newTarget,
      thisValue,
      args);
  insert(CI);
  return CI;
}

HBCCallWithArgCountInst *IRBuilder::createHBCCallWithArgCount(
    Value *callee,
    Value *target,
    bool calleeIsAlwaysClosure,
    Value *env,
    Value *newTarget,
    LiteralNumber *argCount,
    Value *thisValue,
    ArrayRef<Value *> args) {
  auto CI = new HBCCallWithArgCountInst(
      callee,
      target,
      getLiteralBool(calleeIsAlwaysClosure),
      env,
      newTarget,
      argCount,
      thisValue,
      args);
  insert(CI);
  return CI;
}

HBCCallNInst *IRBuilder::createHBCCallNInst(
    Value *callee,
    Value *target,
    bool calleeIsAlwaysClosure,
    Value *env,
    Value *newTarget,
    Value *thisValue,
    ArrayRef<Value *> args) {
  auto CI = new HBCCallNInst(
      callee,
      target,
      getLiteralBool(calleeIsAlwaysClosure),
      env,
      newTarget,
      thisValue,
      args);
  insert(CI);
  return CI;
}

LoadPropertyInst *IRBuilder::createLoadPropertyInst(
    Value *object,
    Value *property) {
  auto LPI = new LoadPropertyInst(object, property);
  insert(LPI);
  return LPI;
}

LoadPropertyWithReceiverInst *IRBuilder::createLoadPropertyWithReceiverInst(
    Value *object,
    Value *property,
    Value *receiver) {
  auto LPI = new LoadPropertyWithReceiverInst(object, property, receiver);
  insert(LPI);
  return LPI;
}

LoadOwnPrivateFieldInst *IRBuilder::createLoadOwnPrivateFieldInst(
    Value *object,
    Value *property) {
  auto LPI = new LoadOwnPrivateFieldInst(object, property);
  insert(LPI);
  return LPI;
}

TryLoadGlobalPropertyInst *IRBuilder::createTryLoadGlobalPropertyInst(
    LiteralString *property) {
  auto *inst = new TryLoadGlobalPropertyInst(getGlobalObject(), property);
  insert(inst);
  return inst;
}

TryLoadGlobalPropertyInst *IRBuilder::createTryLoadGlobalPropertyInst(
    GlobalObjectProperty *property) {
  return createTryLoadGlobalPropertyInst(property->getName());
}

DeletePropertyInst *IRBuilder::createDeletePropertyInst(
    Value *object,
    Value *property) {
  if (Block->getParent()->isStrictMode())
    return createDeletePropertyStrictInst(object, property);
  else
    return createDeletePropertyLooseInst(object, property);
}
DeletePropertyLooseInst *IRBuilder::createDeletePropertyLooseInst(
    Value *object,
    Value *property) {
  auto DPI = new DeletePropertyLooseInst(object, property);
  insert(DPI);
  return DPI;
}
DeletePropertyStrictInst *IRBuilder::createDeletePropertyStrictInst(
    Value *object,
    Value *property) {
  auto DPI = new DeletePropertyStrictInst(object, property);
  insert(DPI);
  return DPI;
}

StorePropertyWithReceiverInst *IRBuilder::createStorePropertyWithReceiverInst(
    Value *storedValue,
    Value *object,
    Value *property,
    Value *receiver,
    StoreStrict isStrict) {
  auto SPI = new StorePropertyWithReceiverInst(
      storedValue,
      object,
      property,
      receiver,
      getLiteralBool(isStrict == StoreStrict::Yes));
  insert(SPI);
  return SPI;
}

StorePropertyInst *IRBuilder::createStorePropertyInst(
    Value *storedValue,
    Value *object,
    Value *property) {
  if (Block->getParent()->isStrictMode())
    return createStorePropertyStrictInst(storedValue, object, property);
  else
    return createStorePropertyLooseInst(storedValue, object, property);
}
StorePropertyLooseInst *IRBuilder::createStorePropertyLooseInst(
    Value *storedValue,
    Value *object,
    Value *property) {
  auto SPI = new StorePropertyLooseInst(storedValue, object, property);
  insert(SPI);
  return SPI;
}
StorePropertyStrictInst *IRBuilder::createStorePropertyStrictInst(
    Value *storedValue,
    Value *object,
    Value *property) {
  auto SPI = new StorePropertyStrictInst(storedValue, object, property);
  insert(SPI);
  return SPI;
}

TryStoreGlobalPropertyInst *IRBuilder::createTryStoreGlobalPropertyInst(
    Value *storedValue,
    LiteralString *property) {
  if (Block->getParent()->isStrictMode())
    return createTryStoreGlobalPropertyStrictInst(storedValue, property);
  else
    return createTryStoreGlobalPropertyLooseInst(storedValue, property);
}
TryStoreGlobalPropertyLooseInst *
IRBuilder::createTryStoreGlobalPropertyLooseInst(
    Value *storedValue,
    LiteralString *property) {
  auto *inst = new TryStoreGlobalPropertyLooseInst(
      storedValue, getGlobalObject(), property);
  insert(inst);
  return inst;
}
TryStoreGlobalPropertyStrictInst *
IRBuilder::createTryStoreGlobalPropertyStrictInst(
    Value *storedValue,
    LiteralString *property) {
  auto *inst = new TryStoreGlobalPropertyStrictInst(
      storedValue, getGlobalObject(), property);
  insert(inst);
  return inst;
}
TryStoreGlobalPropertyInst *IRBuilder::createTryStoreGlobalPropertyInst(
    Value *storedValue,
    GlobalObjectProperty *property) {
  return createTryStoreGlobalPropertyInst(storedValue, property->getName());
}

DefineOwnPropertyInst *IRBuilder::createDefineOwnPropertyInst(
    Value *storedValue,
    Value *object,
    Value *property,
    PropEnumerable isEnumerable) {
  auto SPI = new DefineOwnPropertyInst(
      storedValue,
      object,
      property,
      getLiteralBool(isEnumerable == PropEnumerable::Yes));
  insert(SPI);
  return SPI;
}

DefineOwnInDenseArrayInst *IRBuilder::createDefineOwnInDenseArrayInst(
    Value *storedValue,
    Value *object,
    LiteralNumber *arrayIndex) {
  auto *inst = new DefineOwnInDenseArrayInst(storedValue, object, arrayIndex);
  insert(inst);
  return inst;
}
StoreOwnPrivateFieldInst *IRBuilder::createStoreOwnPrivateFieldInst(
    Value *storedValue,
    Value *object,
    Value *property) {
  auto *inst = new StoreOwnPrivateFieldInst(storedValue, object, property);
  insert(inst);
  return inst;
}
AddOwnPrivateFieldInst *IRBuilder::createAddOwnPrivateFieldInst(
    Value *storedValue,
    Value *object,
    Value *property) {
  auto *inst = new AddOwnPrivateFieldInst(storedValue, object, property);
  insert(inst);
  return inst;
}
PrivateBrandCheckInst *IRBuilder::createPrivateBrandCheckInst(
    Value *object,
    Value *brand) {
  auto *inst = new PrivateBrandCheckInst(object, brand);
  insert(inst);
  return inst;
}

DefineOwnGetterSetterInst *IRBuilder::createDefineOwnGetterSetterInst(
    Value *storedGetter,
    Value *storedSetter,
    Value *object,
    Value *property,
    PropEnumerable isEnumerable) {
  auto *SGSI = new DefineOwnGetterSetterInst(
      storedGetter,
      storedSetter,
      object,
      property,
      getLiteralBool(isEnumerable == PropEnumerable::Yes));
  insert(SGSI);
  return SGSI;
}

DeletePropertyInst *IRBuilder::createDeletePropertyInst(
    Value *object,
    llvh::StringRef property) {
  Identifier Iden = createIdentifier(property);
  return createDeletePropertyInst(object, Iden);
}

LoadPropertyInst *IRBuilder::createLoadPropertyInst(
    Value *object,
    llvh::StringRef property) {
  Identifier Iden = createIdentifier(property);
  return createLoadPropertyInst(object, Iden);
}

TryLoadGlobalPropertyInst *IRBuilder::createTryLoadGlobalPropertyInst(
    llvh::StringRef property) {
  return createTryLoadGlobalPropertyInst(createIdentifier(property));
}

StorePropertyInst *IRBuilder::createStorePropertyInst(
    Value *storedValue,
    Value *object,
    llvh::StringRef property) {
  Identifier Iden = createIdentifier(property);
  return createStorePropertyInst(storedValue, object, Iden);
}

LoadPropertyInst *IRBuilder::createLoadPropertyInst(
    Value *object,
    Identifier property) {
  auto L = getLiteralString(property);
  return createLoadPropertyInst(object, L);
}

TryLoadGlobalPropertyInst *IRBuilder::createTryLoadGlobalPropertyInst(
    Identifier property) {
  auto *inst = new TryLoadGlobalPropertyInst(
      getGlobalObject(), getLiteralString(property));
  insert(inst);
  return inst;
}

DeletePropertyInst *IRBuilder::createDeletePropertyInst(
    Value *object,
    Identifier property) {
  auto L = getLiteralString(property);
  return createDeletePropertyInst(object, L);
}

StorePropertyInst *IRBuilder::createStorePropertyInst(
    Value *storedValue,
    Value *object,
    Identifier property) {
  auto L = getLiteralString(property);
  return createStorePropertyInst(storedValue, object, L);
}

TryStoreGlobalPropertyInst *IRBuilder::createTryStoreGlobalPropertyInst(
    Value *storedValue,
    Identifier property) {
  return createTryStoreGlobalPropertyInst(
      storedValue, getLiteralString(property));
}

AllocFastArrayInst *IRBuilder::createAllocFastArrayInst(
    LiteralNumber *sizeHint,
    Value *prototype) {
  auto A = new AllocFastArrayInst(sizeHint, prototype);
  insert(A);
  return A;
}

AllocArrayInst *IRBuilder::createAllocArrayInst(
    LiteralNumber *sizeHint,
    AllocArrayInst::ArrayValueList val_list) {
  auto AAI = new AllocArrayInst(val_list, sizeHint);
  insert(AAI);
  return AAI;
}

AllocArrayInst *IRBuilder::createAllocArrayInst(
    AllocArrayInst::ArrayValueList val_list,
    unsigned sizeHint) {
  return createAllocArrayInst(this->getLiteralNumber(sizeHint), val_list);
}

GetTemplateObjectInst *IRBuilder::createGetTemplateObjectInst(
    uint32_t templateObjID,
    bool dup,
    llvh::ArrayRef<LiteralString *> rawStrings,
    llvh::ArrayRef<Value *> cookedStrings) {
  auto *inst = new GetTemplateObjectInst(
      getLiteralNumber(templateObjID),
      getLiteralBool(dup),
      rawStrings,
      cookedStrings);
  insert(inst);
  return inst;
}

CreateArgumentsLooseInst *IRBuilder::createCreateArgumentsLooseInst() {
  auto *CAI = new CreateArgumentsLooseInst();
  insert(CAI);
  return CAI;
}
CreateArgumentsStrictInst *IRBuilder::createCreateArgumentsStrictInst() {
  auto *CAI = new CreateArgumentsStrictInst();
  insert(CAI);
  return CAI;
}

GetNewTargetInst *IRBuilder::createGetNewTargetInst(Value *newTargetParam) {
  auto *inst = new GetNewTargetInst(newTargetParam);
  insert(inst);
  return inst;
}

ThrowIfInst *IRBuilder::createThrowIfInst(
    Value *checkedValue,
    Type invalidTypes) {
  TypeContext &tc = getTypeContext();
  assert(
      !invalidTypes.isNoType() &&
      tc.isSubsetOf(invalidTypes, Type::createEmptyOrUninit()) &&
      "invalidTypes set can only contain Empty or Uninit");
  Type resultType = tc.subtractTy(checkedValue->getType(), invalidTypes);
  auto *inst =
      new ThrowIfInst(checkedValue, getLiteralIRType(invalidTypes), resultType);
  insert(inst);
  return inst;
}

ThrowIfThisInitializedInst *IRBuilder::createThrowIfThisInitializedInst(
    Value *subclassCheckedThis) {
  auto *inst = new ThrowIfThisInitializedInst(subclassCheckedThis);
  insert(inst);
  return inst;
}

LIRGetGlobalObjectInst *IRBuilder::createLIRGetGlobalObjectInst() {
  auto inst = new LIRGetGlobalObjectInst();
  insert(inst);
  return inst;
}

CreateRegExpInst *IRBuilder::createRegExpInst(
    Identifier pattern,
    Identifier flags) {
  auto res =
      new CreateRegExpInst(getLiteralString(pattern), getLiteralString(flags));
  insert(res);
  return res;
}

TypeOfInst *IRBuilder::createTypeOfInst(Value *input) {
  auto *inst = new TypeOfInst(input);
  insert(inst);
  return inst;
}

TypeOfIsInst *IRBuilder::createTypeOfIsInst(
    Value *input,
    LiteralTypeOfIsTypes *types) {
  auto *inst = new TypeOfIsInst(input, types);
  insert(inst);
  return inst;
}

UnaryOperatorInst *
IRBuilder::createUnaryOperatorInst(Value *value, ValueKind kind, Type type) {
  auto UOI = new UnaryOperatorInst(kind, value, type);
  insert(UOI);
  return UOI;
}

BinaryOperatorInst *
IRBuilder::createBinaryOperatorInst(Value *left, Value *right, ValueKind kind) {
  auto BOI = new BinaryOperatorInst(kind, left, right);
  insert(BOI);
  return BOI;
}

ImulInst *IRBuilder::createImulInst(Value *left, Value *right) {
  auto *II = new ImulInst(left, right);
  insert(II);
  return II;
}

SwitchInst *IRBuilder::createSwitchInst(
    Value *input,
    BasicBlock *defaultBlock,
    llvh::ArrayRef<Literal *> values,
    llvh::ArrayRef<BasicBlock *> blocks) {
  auto SI = new SwitchInst(input, defaultBlock, values, blocks);
  insert(SI);
  return SI;
}

PhiInst *IRBuilder::createPhiInst() {
  PhiInst::ValueListType values;
  PhiInst::BasicBlockListType blocks;

  return createPhiInst(values, blocks);
}

PhiInst *IRBuilder::createPhiInst(
    const PhiInst::ValueListType &values,
    const PhiInst::BasicBlockListType &blocks) {
  auto PI = new PhiInst(values, blocks);
  insert(PI);
  // Compute the result type now that the inst is in a block (so getModule()
  // works inside recalculateResultType).
  PI->recalculateResultType();
  return PI;
}

GetPNamesInst *IRBuilder::createGetPNamesInst(
    AllocStackInst *iteratorAddr,
    AllocStackInst *baseAddr,
    AllocStackInst *indexAddr,
    AllocStackInst *sizeAddr,
    BasicBlock *onEmpty,
    BasicBlock *onSome) {
  auto GP = new GetPNamesInst(
      getInsertionBlock(),
      iteratorAddr,
      baseAddr,
      indexAddr,
      sizeAddr,
      onEmpty,
      onSome);
  insert(GP);
  return GP;
}

GetNextPNameInst *IRBuilder::createGetNextPNameInst(
    AllocStackInst *propertyAddr,
    AllocStackInst *baseAddr,
    AllocStackInst *indexAddr,
    AllocStackInst *sizeAddr,
    AllocStackInst *iteratorAddr,
    BasicBlock *onLast,
    BasicBlock *onSome) {
  auto GNP = new GetNextPNameInst(
      getInsertionBlock(),
      propertyAddr,
      baseAddr,
      indexAddr,
      sizeAddr,
      iteratorAddr,
      onLast,
      onSome);
  insert(GNP);
  return GNP;
}

MovInst *IRBuilder::createMovInst(Value *input) {
  auto MI = new MovInst(input);
  insert(MI);
  return MI;
}

ImplicitMovInst *IRBuilder::createImplicitMovInst(Value *input) {
  auto IMI = new ImplicitMovInst(input);
  insert(IMI);
  return IMI;
}

CoerceThisNSInst *IRBuilder::createCoerceThisNSInst(Value *input) {
  auto *inst = new CoerceThisNSInst(input);
  insert(inst);
  return inst;
}

DebuggerInst *IRBuilder::createDebuggerInst() {
  auto DI = new DebuggerInst();
  insert(DI);
  return DI;
}

SaveAndYieldInst *IRBuilder::createSaveAndYieldInst(
    Value *result,
    LiteralBool *isDelegated,
    BasicBlock *nextBlock) {
  auto *I = new SaveAndYieldInst(result, isDelegated, nextBlock);
  insert(I);
  return I;
}

CreateGeneratorInst *IRBuilder::createCreateGeneratorInst(
    BaseScopeInst *scope,
    NormalFunction *innerFn) {
  auto *I = new CreateGeneratorInst(scope, scope->getVariableScope(), innerFn);
  insert(I);
  return I;
}

ResumeGeneratorInst *IRBuilder::createResumeGeneratorInst(
    AllocStackInst *isReturn) {
  auto *I = new ResumeGeneratorInst(isReturn);
  insert(I);
  return I;
}

HBCResolveParentEnvironmentInst *
IRBuilder::createHBCResolveParentEnvironmentInst(
    VariableScope *scope,
    LiteralNumber *numLevels,
    JSSpecialParam *parentScopeParam) {
  auto *inst =
      new HBCResolveParentEnvironmentInst(scope, numLevels, parentScopeParam);
  insert(inst);
  return inst;
}

UIntSwitchImmInst *IRBuilder::createUIntSwitchImmInst(
    Value *input,
    BasicBlock *defaultBlock,
    LiteralNumber *minValue,
    LiteralNumber *size,
    const UIntSwitchImmInst::ValueListType &values,
    const UIntSwitchImmInst::BasicBlockListType &blocks) {
  auto inst = new UIntSwitchImmInst(
      input, defaultBlock, minValue, size, values, blocks);
  insert(inst);
  return inst;
}

StringSwitchImmInst *IRBuilder::createStringSwitchImmInst(
    Value *input,
    BasicBlock *defaultBlock,
    LiteralNumber *size,
    const StringSwitchImmInst::ValueListType &values,
    const StringSwitchImmInst::BasicBlockListType &blocks) {
  auto inst =
      new StringSwitchImmInst(input, defaultBlock, size, values, blocks);
  insert(inst);
  return inst;
}

DirectEvalInst *IRBuilder::createDirectEvalInst(
    Value *evalText,
    bool strictCaller) {
  auto *inst = new DirectEvalInst(evalText, getLiteralBool(strictCaller));
  insert(inst);
  return inst;
}

DeclareGlobalVarInst *IRBuilder::createDeclareGlobalVarInst(
    LiteralString *name) {
  auto *inst = new DeclareGlobalVarInst(name);
  insert(inst);
  return inst;
}

LIRLoadConstInst *IRBuilder::createLIRLoadConstInst(Literal *value) {
  auto inst = new LIRLoadConstInst(value);
  insert(inst);
  return inst;
}

LoadParamInst *IRBuilder::createLoadParamInst(JSDynamicParam *param) {
  auto inst = new LoadParamInst(param);
  insert(inst);
  return inst;
}

HBCCreateFunctionEnvironmentInst *
IRBuilder::createHBCCreateFunctionEnvironmentInst(
    VariableScope *scope,
    JSSpecialParam *parentScopeParam) {
  auto *inst = new HBCCreateFunctionEnvironmentInst(scope, parentScopeParam);
  insert(inst);
  return inst;
}

LIRGetThisNSInst *IRBuilder::createLIRGetThisNSInst() {
  auto inst = new LIRGetThisNSInst();
  insert(inst);
  return inst;
}
LIRGetArgumentsPropByValLooseInst *
IRBuilder::createLIRGetArgumentsPropByValLooseInst(
    Value *index,
    AllocStackInst *lazyReg) {
  auto inst = new LIRGetArgumentsPropByValLooseInst(index, lazyReg);
  insert(inst);
  return inst;
}
LIRGetArgumentsPropByValStrictInst *
IRBuilder::createLIRGetArgumentsPropByValStrictInst(
    Value *index,
    AllocStackInst *lazyReg) {
  auto inst = new LIRGetArgumentsPropByValStrictInst(index, lazyReg);
  insert(inst);
  return inst;
}
LIRGetArgumentsLengthInst *IRBuilder::createLIRGetArgumentsLengthInst(
    Value *lazyRegValue) {
  auto inst = new LIRGetArgumentsLengthInst(lazyRegValue);
  insert(inst);
  return inst;
}
LIRReifyArgumentsLooseInst *IRBuilder::createLIRReifyArgumentsLooseInst(
    AllocStackInst *lazyReg) {
  auto inst = new LIRReifyArgumentsLooseInst(lazyReg);
  insert(inst);
  return inst;
}
LIRReifyArgumentsStrictInst *IRBuilder::createLIRReifyArgumentsStrictInst(
    AllocStackInst *lazyReg) {
  auto inst = new LIRReifyArgumentsStrictInst(lazyReg);
  insert(inst);
  return inst;
}
CreateThisInst *IRBuilder::createCreateThisInst(
    Value *closure,
    Value *newTarget) {
  auto inst = new CreateThisInst(closure, newTarget, getEmptySentinel());
  insert(inst);
  return inst;
}
GetConstructedObjectInst *IRBuilder::createGetConstructedObjectInst(
    Instruction *thisValue,
    Value *constructorReturnValue) {
  auto inst = new GetConstructedObjectInst(thisValue, constructorReturnValue);
  insert(inst);
  return inst;
}

HBCProfilePointInst *IRBuilder::createHBCProfilePointInst(uint16_t pointIndex) {
  auto inst = new HBCProfilePointInst(pointIndex);
  insert(inst);
  return inst;
}

CallBuiltinInst *IRBuilder::createCallBuiltinInst(
    BuiltinMethod::Enum builtinIndex,
    ArrayRef<Value *> arguments) {
  auto *inst = new CallBuiltinInst(
      getLiteralBuiltinIdx(builtinIndex),
      getEmptySentinel(),
      getLiteralBool(false),
      getEmptySentinel(),
      getLiteralUndefined(),
      arguments);
  insert(inst);
  return inst;
}

GetBuiltinClosureInst *IRBuilder::createGetBuiltinClosureInst(
    BuiltinMethod::Enum builtinIndex) {
  auto *inst = new GetBuiltinClosureInst(getLiteralBuiltinIdx(builtinIndex));
  insert(inst);
  return inst;
}

LIRSpillMovInst *IRBuilder::createLIRSpillMovInst(Instruction *value) {
  auto *inst = new LIRSpillMovInst(value);
  insert(inst);
  return inst;
}

LIRAllocObjectFromBufferInst *IRBuilder::createLIRAllocObjectFromBufferInst(
    Value *parentObj,
    LIRAllocObjectFromBufferInst::ObjectPropertyMap prop_map) {
  auto *inst = new LIRAllocObjectFromBufferInst(parentObj, prop_map);
  insert(inst);
  return inst;
}

LIRAllocTypedObjectFromBufferInst *
IRBuilder::createLIRAllocTypedObjectFromBufferInst(
    Value *parentObject,
    const LIRAllocTypedObjectFromBufferInst::ObjectPropertyMap &propMap) {
  auto *inst = new LIRAllocTypedObjectFromBufferInst(parentObject, propMap);
  insert(inst);
  return inst;
}

LIRAllocTypedNonEnumObjectFromBufferInst *
IRBuilder::createLIRAllocTypedNonEnumObjectFromBufferInst(
    Value *parentObject,
    const LIRAllocTypedNonEnumObjectFromBufferInst::ObjectPropertyMap
        &propMap) {
  auto *inst =
      new LIRAllocTypedNonEnumObjectFromBufferInst(parentObject, propMap);
  insert(inst);
  return inst;
}

AllocObjectLiteralInst *IRBuilder::createAllocObjectLiteralInst(
    const AllocObjectLiteralInst::ObjectPropertyMap &propMap,
    Value *parentObject) {
  auto *inst = new AllocObjectLiteralInst(
      parentObject ? parentObject : getEmptySentinel(), propMap);
  insert(inst);
  return inst;
}

AllocTypedObjectInst *IRBuilder::createAllocTypedObjectInst(
    const AllocTypedObjectInst::ObjectPropertyMap &propMap,
    Value *parentObject) {
  auto *inst = new AllocTypedObjectInst(parentObject, propMap);
  insert(inst);
  return inst;
}

AllocTypedNonEnumObjectInst *IRBuilder::createAllocTypedNonEnumObjectInst(
    const AllocTypedNonEnumObjectInst::ObjectPropertyMap &propMap,
    Value *parentObject) {
  auto *inst = new AllocTypedNonEnumObjectInst(parentObject, propMap);
  insert(inst);
  return inst;
}

HBCCompareBranchInst *IRBuilder::createHBCCompareBranchInst(
    Value *left,
    Value *right,
    ValueKind kind,
    BasicBlock *trueBlock,
    BasicBlock *falseBlock) {
  auto *inst =
      new HBCCompareBranchInst(kind, left, right, trueBlock, falseBlock);
  insert(inst);
  return inst;
}

HBCCmpBrTypeOfIsInst *IRBuilder::createHBCCmpBrTypeOfIsInst(
    Value *arg,
    LiteralTypeOfIsTypes *types,
    BasicBlock *trueBlock,
    BasicBlock *falseBlock) {
  auto *inst = new HBCCmpBrTypeOfIsInst(arg, types, trueBlock, falseBlock);
  insert(inst);
  return inst;
}

IteratorBeginInst *IRBuilder::createIteratorBeginInst(
    AllocStackInst *sourceOrNext) {
  auto *I = new IteratorBeginInst(sourceOrNext);
  insert(I);
  return I;
}

IteratorNextInst *IRBuilder::createIteratorNextInst(
    AllocStackInst *iterator,
    Value *sourceOrNext) {
  auto *I = new IteratorNextInst(iterator, sourceOrNext);
  insert(I);
  return I;
}

IteratorCloseInst *IRBuilder::createIteratorCloseInst(
    Value *iterator,
    bool ignoreInnerException) {
  auto *I =
      new IteratorCloseInst(iterator, getLiteralBool(ignoreInnerException));
  insert(I);
  return I;
}

UnreachableInst *IRBuilder::createUnreachableInst() {
  auto *I = new UnreachableInst();
  insert(I);
  return I;
}

PrLoadInst *IRBuilder::createPrLoadInst(
    Value *object,
    size_t propIndex,
    Literal *propName,
    Type checkedType) {
  auto *I = new PrLoadInst(
      object, getLiteralNumber((double)propIndex), propName, checkedType);
  insert(I);
  return I;
}

PrStoreInst *IRBuilder::createPrStoreInst(
    Value *storedValue,
    Value *object,
    size_t propIndex,
    Literal *propName,
    bool nonPointer) {
  auto *I = new PrStoreInst(
      storedValue,
      object,
      getLiteralNumber((double)propIndex),
      propName,
      getLiteralBool(nonPointer));
  insert(I);
  return I;
}

FastArrayLoadInst *IRBuilder::createFastArrayLoadInst(
    Value *array,
    Value *index,
    Type checkedType) {
  auto *I = new FastArrayLoadInst(array, index, checkedType);
  insert(I);
  return I;
}

FastArrayStoreInst *IRBuilder::createFastArrayStoreInst(
    Value *storedValue,
    Value *array,
    Value *index) {
  auto *I = new FastArrayStoreInst(storedValue, array, index);
  insert(I);
  return I;
}

FastArrayPushInst *IRBuilder::createFastArrayPushInst(
    Value *pushedValue,
    Value *array) {
  auto *I = new FastArrayPushInst(pushedValue, array);
  insert(I);
  return I;
}
FastArrayAppendInst *IRBuilder::createFastArrayAppendInst(
    Value *other,
    Value *array) {
  auto *I = new FastArrayAppendInst(other, array);
  insert(I);
  return I;
}

FastArrayLengthInst *IRBuilder::createFastArrayLengthInst(Value *array) {
  auto *I = new FastArrayLengthInst(array);
  insert(I);
  return I;
}

LoadParentNoTrapsInst *IRBuilder::createLoadParentNoTrapsInst(Value *object) {
  auto *inst = new LoadParentNoTrapsInst(object);
  insert(inst);
  return inst;
}

TypedLoadParentInst *IRBuilder::createTypedLoadParentInst(Value *object) {
  auto *inst = new TypedLoadParentInst(object);
  insert(inst);
  return inst;
}

FUnaryMathInst *IRBuilder::createFUnaryMathInst(ValueKind kind, Value *arg) {
  auto *inst = new FUnaryMathInst(kind, arg);
  insert(inst);
  return inst;
}
FBinaryMathInst *
IRBuilder::createFBinaryMathInst(ValueKind kind, Value *left, Value *right) {
  auto *inst = new FBinaryMathInst(kind, left, right);
  insert(inst);
  return inst;
}

FCompareInst *
IRBuilder::createFCompareInst(ValueKind kind, Value *left, Value *right) {
  auto *inst = new FCompareInst(kind, left, right);
  insert(inst);
  return inst;
}

HBCFCompareBranchInst *IRBuilder::createHBCFCompareBranchInst(
    Value *left,
    Value *right,
    ValueKind kind,
    BasicBlock *trueBlock,
    BasicBlock *falseBlock) {
  auto *inst =
      new HBCFCompareBranchInst(kind, left, right, trueBlock, falseBlock);
  insert(inst);
  return inst;
}

StringConcatInst *IRBuilder::createStringConcatInst(
    llvh::ArrayRef<Value *> operands) {
  auto *inst = new StringConcatInst(operands);
  insert(inst);
  return inst;
}

HBCStringConcatInst *IRBuilder::createHBCStringConcatInst(
    Value *left,
    Value *right) {
  auto *inst = new HBCStringConcatInst(left, right);
  insert(inst);
  return inst;
}

UnionNarrowTrustedInst *IRBuilder::createUnionNarrowTrustedInst(
    Value *value,
    Type type) {
  auto *inst = new UnionNarrowTrustedInst(value, type);
  insert(inst);
  return inst;
}

CheckedTypeCastInst *IRBuilder::createCheckedTypeCastInst(
    Value *value,
    Type type) {
  auto *inst = new CheckedTypeCastInst(value, getLiteralIRType(type));
  insert(inst);
  return inst;
}

LIRDeadValueInst *IRBuilder::createLIRDeadValueInst(Type type) {
  auto *inst = new LIRDeadValueInst(type);
  insert(inst);
  return inst;
}

LiteralNativeSignature *IRBuilder::getLiteralNativeSignature(
    NativeSignature *sig) {
  return M->getLiteralNativeSignature(sig);
}
LiteralNativeExtern *IRBuilder::getLiteralNativeExtern(
    NativeExtern *nativeExtern) {
  return M->getLiteralNativeExtern(nativeExtern);
}

NativeCallInst *IRBuilder::createNativeCallInst(
    Type type,
    Value *callee,
    NativeSignature *sig,
    ArrayRef<Value *> args) {
  auto *inst =
      new NativeCallInst(type, callee, getLiteralNativeSignature(sig), args);
  insert(inst);
  return inst;
}

LazyCompilationDataInst *IRBuilder::createLazyCompilationDataInst(
    LazyCompilationData &&data,
    Variable *capturedThis,
    Value *capturedNewTarget,
    Variable *capturedArguments,
    Variable *homeObject,
    Variable *classCtxConstructor,
    Variable *classCtxInitFuncVar,
    VariableScope *parentVarScope) {
  auto *inst = new LazyCompilationDataInst(
      std::move(data),
      capturedThis ? static_cast<Value *>(capturedThis) : getEmptySentinel(),
      capturedNewTarget ? capturedNewTarget : getLiteralUndefined(),
      capturedArguments ? static_cast<Value *>(capturedArguments)
                        : getEmptySentinel(),
      homeObject ? static_cast<Value *>(homeObject) : getEmptySentinel(),
      classCtxConstructor ? static_cast<Value *>(classCtxConstructor)
                          : getEmptySentinel(),
      classCtxInitFuncVar ? static_cast<Value *>(classCtxInitFuncVar)
                          : getEmptySentinel(),
      parentVarScope);
  insert(inst);
  return inst;
}

EvalCompilationDataInst *IRBuilder::createEvalCompilationDataInst(
    EvalCompilationData &&data,
    Variable *capturedThis,
    Value *capturedNewTarget,
    Variable *capturedArguments,
    Variable *homeObject,
    Variable *classCtxConstructor,
    Variable *classCtxInitFuncVar,
    ArrayRef<VariableScope *> varScopes) {
  auto *inst = new EvalCompilationDataInst(
      std::move(data),
      capturedThis ? static_cast<Value *>(capturedThis) : getEmptySentinel(),
      capturedNewTarget ? capturedNewTarget : getLiteralUndefined(),
      capturedArguments ? static_cast<Value *>(capturedArguments)
                        : getEmptySentinel(),
      homeObject ? static_cast<Value *>(homeObject) : getEmptySentinel(),
      classCtxConstructor ? static_cast<Value *>(classCtxConstructor)
                          : getEmptySentinel(),
      classCtxInitFuncVar ? static_cast<Value *>(classCtxInitFuncVar)
                          : getEmptySentinel(),
      varScopes);
  insert(inst);
  return inst;
}

GetNativeRuntimeInst *IRBuilder::createGetNativeRuntimeInst() {
  auto *inst = new GetNativeRuntimeInst();
  insert(inst);
  return inst;
}

inline void IRBuilder::justInsert(Instruction *Inst) {
  assert(!Inst->getParent() && "Instr that's already inserted elsewhere");
  Inst->setParent(Block);
  Block->getInstList().insert(InsertionPoint, Inst);
}

void IRBuilder::insert(Instruction *Inst) {
  // Set the statement of the new instruction based on the current function's
  // statement counter.
  OptValue<uint32_t> statementOpt = getFunction()->getStatementCount();
  uint32_t statement;
  if (LLVM_LIKELY(statementOpt.hasValue())) {
    statement = *statementOpt;
  } else {
    // If we've cleared the statement count,
    // then just set the new instruction's statement to the statement of the
    // instruction we're inserting at (0 if it doesn't exist).
    statement = InsertionPoint != Block->getInstList().end()
        ? InsertionPoint->getStatementIndex()
        : 0;
  }
  Inst->setStatementIndex(statement);

  if (auto envIDOpt = getFunction()->getEnvironmentID()) {
    Inst->setEnvironmentID(*envIDOpt);
  } else {
    Inst->setEnvironmentID(
        InsertionPoint != Block->getInstList().end()
            ? InsertionPoint->getEnvironmentID()
            : 0);
  }

  // Set the statement of the new instruction based on the current function's
  // statement counter.
  if (auto lexScope = getFunction()->getLexicalScope()) {
    Inst->setLexicalScope(*lexScope);
  } else {
    // Try to inherit the lexical scope of the instruction we're inserting at.
    Inst->setLexicalScope(
        InsertionPoint != Block->getInstList().end()
            ? InsertionPoint->getLexicalScope()
            : nullptr);
  }

  Inst->setLocation(Location);

  return justInsert(Inst);
}

void IRBuilder::setInsertionBlock(BasicBlock *BB) {
  if (BB) {
    InsertionPoint = BB->end();
    Block = BB;
  } else {
    InsertionPoint = BasicBlock::iterator(nullptr);
    Block = nullptr;
  }
}

BasicBlock *IRBuilder::getInsertionBlock() {
  return Block;
}

void IRBuilder::setInsertionPointAfter(Instruction *IP) {
  InsertionPoint = IP->getIterator();
  InsertionPoint++;
  Block = IP->getParent();
}

void IRBuilder::setInsertionPoint(Instruction *IP) {
  InsertionPoint = IP->getIterator();
  Block = IP->getParent();
}

bool IRBuilder::isInsertionPointValid() {
  return InsertionPoint.getNodePtr();
}

void IRBuilder::resetInsertionPoint() {
  InsertionPoint = BasicBlock::iterator();
  Block = nullptr;
}

void IRBuilder::transferInstructionToCurrentBlock(Instruction *inst) {
  auto *oldBlock = inst->getParent();
  inst->removeFromParent();
  inst->setParent(Block);
  Block->getInstList().insert(InsertionPoint, inst);

  if (oldBlock == Block || !llvh::isa<TerminatorInst>(inst))
    return;

  // If we moved the terminator, we want to update all successors' phi-nodes.
  for (auto *use : oldBlock->getUsers()) {
    auto *phi = llvh::dyn_cast<PhiInst>(use);
    if (!phi)
      continue;
    for (unsigned i = 0, e = phi->getNumOperands(); i != e; ++i) {
      if (phi->getOperand(i) == oldBlock)
        phi->setOperand(Block, i);
    }
  }
}

Instruction *IRBuilder::cloneInst(
    const Instruction *source,
    llvh::ArrayRef<Value *> operands) {
  Instruction *inst;
  switch (source->getKind()) {
#define DEF_VALUE(name, parent)                    \
  case ValueKind::name##Kind:                      \
    inst = new name(cast<name>(source), operands); \
    break;
#define DEF_TAG(name, parent)                          \
  case ValueKind::name##Kind:                          \
    inst = new parent(cast<parent>(source), operands); \
    break;

#include "hermes/IR/Instrs.def"
    default:
      llvm_unreachable("invalid kind");
  }

  justInsert(inst);
  return inst;
}
