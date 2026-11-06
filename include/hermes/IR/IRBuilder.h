/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_IR_IRBUILDER_H
#define HERMES_IR_IRBUILDER_H

#include <string>
#include <utility>

#include "llvh/ADT/SmallVector.h"
#include "llvh/ADT/StringRef.h"
#include "llvh/ADT/Twine.h"

#include "hermes/AST/Context.h"
#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/Instrs.h"

namespace hermes {

/// The IRBuilder is used for creating IR. The builder APIs is split into two
/// parts. First, APIs for creating blocks and functions. These APIs are
/// stateless and do not affect the second kind of APIs which are used for
/// creating new instructions. The Instruction creation APIS are stateful and
/// follow an insertion point that can be saved, restored and manipulated.
class IRBuilder {
  IRBuilder(const IRBuilder &) = delete;
  void operator=(const IRBuilder &) = delete;

  /// The module is the root of the program that we are building.
  Module *M;
  /// This is where the next instruction will be inserted.
  BasicBlock::iterator InsertionPoint{};
  // The iterator must point into this block:
  BasicBlock *Block{};

  SMLoc Location{};

 public:
  explicit IRBuilder(Module *Mod) : M(Mod), InsertionPoint(nullptr) {}
  explicit IRBuilder(Function *F)
      : M(F->getParent()), InsertionPoint(nullptr) {}

  //--------------------------------------------------------------------------//
  //                          Stateless APIs.                                 //
  //--------------------------------------------------------------------------//

  enum class PropEnumerable { No = 0, Yes = 1 };
  enum class StoreStrict { No = 0, Yes = 1 };

  Module *getModule() {
    return M;
  }

  /// \return the TypeContext owned by the Module.
  TypeContext &getTypeContext() {
    return M->getTypeContext();
  }

  /// Create a new BasicBlock and add it to a function \p Parent.
  BasicBlock *createBasicBlock(Function *Parent);

  /// Create a new Function and add it to the Module.
  /// \param OriginalName the original name specified by the user.
  /// \param insertBefore Another function in the module where this function
  ///   should be inserted before. If null, appends to the end of the module.
  NormalFunction *createFunction(
      Identifier OriginalName,
      Function::DefinitionKind definitionKind,
      bool strictMode,
      CustomDirectives customDirectives = CustomDirectives{},
      SMRange sourceRange = SMRange{},
      Function *insertBefore = nullptr);

  /// Create a new Function and add it to the Module.
  NormalFunction *createFunction(
      llvh::StringRef OriginalName,
      Function::DefinitionKind definitionKind,
      bool strictMode,
      CustomDirectives customDirectives = CustomDirectives{},
      SMRange sourceRange = SMRange{},
      Function *insertBefore = nullptr);

  /// Create a new AsyncFunction and add it to the Module.
  /// \param OriginalName the original name specified by the user.
  /// \param insertBefore Another function in the module where this function
  ///   should be inserted before. If null, appends to the end of the module.
  AsyncFunction *createAsyncFunction(
      Identifier OriginalName,
      Function::DefinitionKind definitionKind,
      bool strictMode,
      CustomDirectives customDirectives,
      SMRange sourceRange = SMRange{},
      Function *insertBefore = nullptr);

  /// Create a new GeneratorFunction and add it to the Module.
  /// \param OriginalName the original name specified by the user.
  /// \param insertBefore Another function in the module where this function
  ///   should be inserted before. If null, appends to the end of the module.
  GeneratorFunction *createGeneratorFunction(
      Identifier OriginalName,
      Function::DefinitionKind definitionKind,
      bool strictMode,
      CustomDirectives customDirectives,
      SMRange sourceRange = SMRange{},
      Function *insertBefore = nullptr);

  /// Create the top level function representing the global scope.
  Function *createTopLevelFunction(
      llvh::StringRef topLevelFunctionName,
      bool strictMode,
      CustomDirectives customDirectives = CustomDirectives{},
      SMRange sourceRange = SMRange{});

  /// Create a new global object property.
  GlobalObjectProperty *createGlobalObjectProperty(
      Identifier name,
      bool declared);
  /// Create a new global object property.
  GlobalObjectProperty *createGlobalObjectProperty(
      llvh::StringRef name,
      bool declared);

  /// Add a new JS parameter to function \p Parent.
  JSDynamicParam *createJSDynamicParam(Function *parent, Identifier name);

  /// Add a new JS parameter to function \p Parent.
  JSDynamicParam *createJSDynamicParam(Function *parent, llvh::StringRef name);

  /// Add a JS this parameter to function \p Parent. This must be the first
  /// parameter, and should only be called once per function.
  JSDynamicParam *createJSThisParam(Function *parent);

  /// Add a new variable to scope \p Parent.
  Variable *createVariable(
      VariableScope *Parent,
      Identifier Name,
      Type type,
      bool hidden);

  /// Add a new variable to scope \p Parent.
  Variable *createVariable(
      VariableScope *Parent,
      const llvh::Twine &Name,
      Type type,
      bool hidden);

  /// Create a new VariableScope in the module.
  VariableScope *createVariableScope(VariableScope *parentScope);

  /// Create a new literal number of value \p value.
  LiteralNumber *getLiteralNumber(double value);

  /// Create a new literal positive zero.
  LiteralNumber *getLiteralPositiveZero();

  /// Create a new literal negative zero.
  LiteralNumber *getLiteralNegativeZero();

  /// Create a new literal NaN.
  LiteralNumber *getLiteralNaN();

  /// Create a new literal number value of Infinity.
  LiteralNumber *getLiteralInfinity();

  /// Create a new literal BitInt of value \p value.
  LiteralBigInt *getLiteralBigInt(UniqueString *value);

  /// Create a new literal string of value \p value.
  LiteralString *getLiteralString(const llvh::Twine &value);

  /// Create a new literal string of value \p value.
  LiteralString *getLiteralString(Identifier value);

  /// Create a new literal private name of value \p value.
  LiteralPrivateName *getLiteralPrivateName(Identifier value);

  /// Create a new literal bool of value \p value.
  LiteralBool *getLiteralBool(bool value);

  /// Create a new literal 'empty'.
  LiteralEmpty *getLiteralEmpty();

  /// Return the "Uninit" literal singletone.
  LiteralUninit *getLiteralUninit() {
    return M->getLiteralUninit();
  }

  /// Create a new literal 'undefined'.
  LiteralUndefined *getLiteralUndefined();

  /// Create a new literal null.
  LiteralNull *getLiteralNull();

  LiteralBuiltinIdx *getLiteralBuiltinIdx(BuiltinMethod::Enum builtinIdx) {
    return M->getLiteralBuiltinIdx(builtinIdx);
  }

  LiteralIRType *getLiteralIRType(Type type) {
    return M->getLiteralIRType(type);
  }

  LiteralTypeOfIsTypes *getLiteralTypeOfIsTypes(TypeOfIsTypes types) {
    return M->getLiteralTypeOfIsTypes(types);
  }

  /// Return the GlobalObject value.
  GlobalObject *getGlobalObject();

  /// Return the EmptySentinel value.
  EmptySentinel *getEmptySentinel();

  /// Convert llvh::StringRef to Identifier.
  Identifier createIdentifier(const llvh::Twine &str);

  //--------------------------------------------------------------------------//
  //                          Statefull APIs.                                 //
  //--------------------------------------------------------------------------//

 private:
  /// Insert the newly created instruction \p Inst into the basic block without
  /// touching its location or statement index.
  void justInsert(Instruction *Inst);

  /// Insert the newly created instruction \p Inst into the basic block and
  /// populate its location and statement index.
  void insert(Instruction *Inst);

 public:
  /// Set the insertion point of the builder. The next instruction will be
  /// inserted at the end of \p BB.
  void setInsertionBlock(BasicBlock *BB);

  /// Return the current insertion block.
  BasicBlock *getInsertionBlock();

  /// Return the current function.
  Function *getFunction() {
    assert(Block && "Builder has no current function");
    return Block->getParent();
  }

  /// Set the insertion point of the builder. The next instruction will be
  /// inserted after \p IP.
  void setInsertionPointAfter(Instruction *IP);

  /// Set the insertion point of the builder. The next instruction will be
  /// inserted where the current \p IP is (thereby pushing it down).
  void setInsertionPoint(Instruction *IP);

  /// \returns true if the insertion point is set.
  bool isInsertionPointValid();

  /// Clear the insertion point. New instructions must not be created until the
  /// insertion point it set.
  void resetInsertionPoint();

  void setLocation(SMLoc loc) {
    Location = loc;
  }
  void clearLocation() {
    Location = SMLoc{};
  }
  SMLoc getLocation() const {
    return Location;
  }

  /// Move instruction \p inst from its block to the insertion point in the
  /// current block. If the instruction is a terminator, correctly update the
  /// phi-nodes that refer to the old block and point then to the current one.
  void transferInstructionToCurrentBlock(Instruction *inst);

  //--------------------------------------------------------------------------//
  //                    Instruction Creation methods                          //
  //--------------------------------------------------------------------------//

  /// Insert a clone of the instruction at the current insertion point. The
  /// location and statement index are preserved but the operands are recreated.
  /// \param source the original instruction.
  /// \param operands the (updated) operands to use in the new instruction.
  Instruction *cloneInst(
      const Instruction *source,
      llvh::ArrayRef<Value *> operands);

  BranchInst *createBranchInst(BasicBlock *Destination);

  CondBranchInst *
  createCondBranchInst(Value *Cond, BasicBlock *T, BasicBlock *F);

  ReturnInst *createReturnInst(Value *Val);

  AllocStackInst *createAllocStackInst(const llvh::Twine &varName, Type type);

  AllocStackInst *createAllocStackInst(Identifier varName, Type type);

  ToPropertyKeyInst *createToPropertyKeyInst(Value *val);

  AsNumberInst *createAsNumberInst(Value *val);

  AsNumericInst *createAsNumericInst(Value *val);

  AsInt32Inst *createAsInt32Inst(Value *val);

  AsUint32Inst *createAsUint32Inst(Value *val);

  AddEmptyStringInst *createAddEmptyStringInst(Value *val);

  CreatePrivateNameInst *createCreatePrivateNameInst(LiteralString *descStr);

  CreateClassInst *createCreateClassInst(
      BaseScopeInst *scope,
      Function *code,
      Value *superClass,
      AllocStackInst *homeObjectOutput);

  CreateFunctionInst *createCreateFunctionInst(
      BaseScopeInst *scope,
      Function *code);

  GetParentScopeInst *createGetParentScopeInst(
      VariableScope *scope,
      JSSpecialParam *parentScopeParam);

  CreateScopeInst *createCreateScopeInst(
      VariableScope *scope,
      Value *parentScope);

  ResolveScopeInst *createResolveScopeInst(
      VariableScope *scope,
      VariableScope *startVarScope,
      Instruction *startScope);

  LIRResolveScopeInst *createLIRResolveScopeInst(
      VariableScope *scope,
      Instruction *startScope,
      LiteralNumber *numLevels);

  GetClosureScopeInst *
  createGetClosureScopeInst(VariableScope *scope, Function *F, Value *closure);

  LoadStackInst *createLoadStackInst(AllocStackInst *ptr);

  LoadFrameInst *createLoadFrameInst(Instruction *scope, Variable *ptr);

  StoreStackInst *createStoreStackInst(Value *storedValue, AllocStackInst *ptr);

  StoreFrameInst *
  createStoreFrameInst(Instruction *scope, Value *storedValue, Variable *ptr);

  CallInst *createCallInst(
      Value *callee,
      Value *target,
      bool calleeIsAlwaysClosure,
      Value *env,
      Value *newTarget,
      Value *thisValue,
      ArrayRef<Value *> args);
  CallInst *createCallInst(
      Value *callee,
      Value *newTarget,
      Value *thisValue,
      ArrayRef<Value *> args) {
    return createCallInst(
        callee,
        getEmptySentinel(),
        /* calleeIsAlwaysClosure */ false,
        getEmptySentinel(),
        newTarget,
        thisValue,
        args);
  }

  HBCCallWithArgCountInst *createHBCCallWithArgCount(
      Value *callee,
      Value *target,
      bool calleeIsAlwaysClosure,
      Value *env,
      Value *newTarget,
      LiteralNumber *argCount,
      Value *thisValue,
      ArrayRef<Value *> args);

  HBCCallNInst *createHBCCallNInst(
      Value *callee,
      Value *target,
      bool calleeIsAlwaysClosure,
      Value *env,
      Value *newTarget,
      Value *thisValue,
      ArrayRef<Value *> args);

  CatchInst *createCatchInst();

  ThrowInst *createThrowInst(
      Value *thrownValue,
      BasicBlock *catchTarget = nullptr);

  ThrowTypeErrorInst *createThrowTypeErrorInst(
      Value *message,
      BasicBlock *catchTarget = nullptr);

  TryStartInst *createTryStartInst(
      BasicBlock *tryBodyBlock,
      BasicBlock *catchTargetBlock);

  TryEndInst *createTryEndInst(BasicBlock *catchBlock, BasicBlock *branchBlock);

  BranchIfBuiltinInst *createBranchIfBuiltinInst(
      BuiltinMethod::Enum builtinIndex,
      Value *argument,
      BasicBlock *catchBlock,
      BasicBlock *branchBlock);

  DeletePropertyInst *createDeletePropertyInst(Value *object, Value *property);
  DeletePropertyLooseInst *createDeletePropertyLooseInst(
      Value *object,
      Value *property);
  DeletePropertyStrictInst *createDeletePropertyStrictInst(
      Value *object,
      Value *property);

  LoadPropertyInst *createLoadPropertyInst(Value *object, Value *property);
  LoadPropertyWithReceiverInst *createLoadPropertyWithReceiverInst(
      Value *object,
      Value *property,
      Value *receiver);
  LoadOwnPrivateFieldInst *createLoadOwnPrivateFieldInst(
      Value *object,
      Value *property);
  TryLoadGlobalPropertyInst *createTryLoadGlobalPropertyInst(
      LiteralString *property);
  TryLoadGlobalPropertyInst *createTryLoadGlobalPropertyInst(
      GlobalObjectProperty *property);

  StorePropertyWithReceiverInst *createStorePropertyWithReceiverInst(
      Value *storedValue,
      Value *object,
      Value *property,
      Value *receiver,
      StoreStrict isStrict);

  StorePropertyInst *
  createStorePropertyInst(Value *storedValue, Value *object, Value *property);
  StorePropertyLooseInst *createStorePropertyLooseInst(
      Value *storedValue,
      Value *object,
      Value *property);
  StorePropertyStrictInst *createStorePropertyStrictInst(
      Value *storedValue,
      Value *object,
      Value *property);

  TryStoreGlobalPropertyInst *createTryStoreGlobalPropertyInst(
      Value *storedValue,
      LiteralString *property);
  TryStoreGlobalPropertyInst *createTryStoreGlobalPropertyInst(
      Value *storedValue,
      GlobalObjectProperty *property);
  TryStoreGlobalPropertyLooseInst *createTryStoreGlobalPropertyLooseInst(
      Value *storedValue,
      LiteralString *property);
  TryStoreGlobalPropertyStrictInst *createTryStoreGlobalPropertyStrictInst(
      Value *storedValue,
      LiteralString *property);

  DefineOwnPropertyInst *createDefineOwnPropertyInst(
      Value *storedValue,
      Value *object,
      Value *property,
      PropEnumerable isEnumerable);
  DefineOwnInDenseArrayInst *createDefineOwnInDenseArrayInst(
      Value *storedValue,
      Value *object,
      LiteralNumber *arrayIndex);
  StoreOwnPrivateFieldInst *createStoreOwnPrivateFieldInst(
      Value *storedValue,
      Value *object,
      Value *property);
  AddOwnPrivateFieldInst *createAddOwnPrivateFieldInst(
      Value *storedValue,
      Value *object,
      Value *property);
  PrivateBrandCheckInst *createPrivateBrandCheckInst(
      Value *object,
      Value *brand);

  DefineOwnGetterSetterInst *createDefineOwnGetterSetterInst(
      Value *storedGetter,
      Value *storedSetter,
      Value *object,
      Value *property,
      PropEnumerable isEnumerable);
  DeletePropertyInst *createDeletePropertyInst(
      Value *object,
      llvh::StringRef property);

  LoadPropertyInst *createLoadPropertyInst(
      Value *object,
      llvh::StringRef property);
  TryLoadGlobalPropertyInst *createTryLoadGlobalPropertyInst(
      llvh::StringRef property);

  StorePropertyInst *createStorePropertyInst(
      Value *storedValue,
      Value *object,
      llvh::StringRef property);

  DeletePropertyInst *createDeletePropertyInst(
      Value *object,
      Identifier property);

  LoadPropertyInst *createLoadPropertyInst(Value *object, Identifier property);
  TryLoadGlobalPropertyInst *createTryLoadGlobalPropertyInst(
      Identifier property);

  StorePropertyInst *createStorePropertyInst(
      Value *storedValue,
      Value *object,
      Identifier property);
  TryStoreGlobalPropertyInst *createTryStoreGlobalPropertyInst(
      Value *storedValue,
      Identifier property);

  AllocObjectLiteralInst *createAllocObjectLiteralInst(
      const AllocObjectLiteralInst::ObjectPropertyMap &propMap = {},
      Value *parentObject = nullptr);

  AllocTypedObjectInst *createAllocTypedObjectInst(
      const AllocTypedObjectInst::ObjectPropertyMap &propMap,
      Value *parentObject);

  AllocTypedNonEnumObjectInst *createAllocTypedNonEnumObjectInst(
      const AllocTypedNonEnumObjectInst::ObjectPropertyMap &propMap,
      Value *parentObject);

  AllocFastArrayInst *createAllocFastArrayInst(
      LiteralNumber *sizeHint,
      Value *prototype);

  AllocArrayInst *createAllocArrayInst(
      LiteralNumber *sizeHint,
      AllocArrayInst::ArrayValueList val_list);

  AllocArrayInst *createAllocArrayInst(
      AllocArrayInst::ArrayValueList val_list,
      unsigned sizeHint);

  GetTemplateObjectInst *createGetTemplateObjectInst(
      uint32_t templateObjID,
      bool dup,
      llvh::ArrayRef<LiteralString *> rawStrings,
      llvh::ArrayRef<Value *> cookedStrings);

  CreateArgumentsLooseInst *createCreateArgumentsLooseInst();
  CreateArgumentsStrictInst *createCreateArgumentsStrictInst();

  GetNewTargetInst *createGetNewTargetInst(Value *newTargetParam);

  ThrowIfInst *createThrowIfInst(Value *checkedValue, Type invalidTypes);

  ThrowIfThisInitializedInst *createThrowIfThisInitializedInst(
      Value *subclassCheckedThis);

  LIRGetGlobalObjectInst *createLIRGetGlobalObjectInst();

  CreateRegExpInst *createRegExpInst(Identifier pattern, Identifier flags);

  TypeOfInst *createTypeOfInst(Value *input);
  TypeOfIsInst *createTypeOfIsInst(Value *input, LiteralTypeOfIsTypes *types);

  UnaryOperatorInst *createUnaryOperatorInst(
      Value *value,
      ValueKind kind,
      Type type = Type::createAnyType());

  DirectEvalInst *createDirectEvalInst(Value *evalText, bool strictCaller);

  DeclareGlobalVarInst *createDeclareGlobalVarInst(LiteralString *name);

  SwitchInst *createSwitchInst(
      Value *input,
      BasicBlock *defaultBlock,
      llvh::ArrayRef<Literal *> values,
      llvh::ArrayRef<BasicBlock *> blocks);

  PhiInst *createPhiInst(
      const PhiInst::ValueListType &values,
      const PhiInst::BasicBlockListType &blocks);

  PhiInst *createPhiInst();

  BinaryOperatorInst *
  createBinaryOperatorInst(Value *left, Value *right, ValueKind kind);

  ImulInst *createImulInst(Value *left, Value *right);

  GetPNamesInst *createGetPNamesInst(
      AllocStackInst *iteratorAddr,
      AllocStackInst *baseAddr,
      AllocStackInst *indexAddr,
      AllocStackInst *sizeAddr,
      BasicBlock *onEmpty,
      BasicBlock *onSome);

  GetNextPNameInst *createGetNextPNameInst(
      AllocStackInst *propertyAddr,
      AllocStackInst *baseAddr,
      AllocStackInst *indexAddr,
      AllocStackInst *sizeAddr,
      AllocStackInst *iteratorAddr,
      BasicBlock *onLast,
      BasicBlock *onSome);

  MovInst *createMovInst(Value *input);

  ImplicitMovInst *createImplicitMovInst(Value *input);

  CoerceThisNSInst *createCoerceThisNSInst(Value *input);

  DebuggerInst *createDebuggerInst();

  SaveAndYieldInst *createSaveAndYieldInst(
      Value *result,
      LiteralBool *isDelegated,
      BasicBlock *nextBlock);

  CreateGeneratorInst *createCreateGeneratorInst(
      BaseScopeInst *scope,
      NormalFunction *innerFn);

  ResumeGeneratorInst *createResumeGeneratorInst(AllocStackInst *isReturn);

  //--------------------------------------------------------------------------//
  //                  Target specific insertions                              //
  //--------------------------------------------------------------------------//

  HBCResolveParentEnvironmentInst *createHBCResolveParentEnvironmentInst(
      VariableScope *scope,
      LiteralNumber *numLevels,
      JSSpecialParam *parentScopeParam);

  UIntSwitchImmInst *createUIntSwitchImmInst(
      Value *input,
      BasicBlock *defaultBlock,
      LiteralNumber *minValue,
      LiteralNumber *size,
      const UIntSwitchImmInst::ValueListType &values,
      const BaseSwitchImmInst::BasicBlockListType &blocks);

  StringSwitchImmInst *createStringSwitchImmInst(
      Value *input,
      BasicBlock *defaultBlock,
      LiteralNumber *size,
      const StringSwitchImmInst::ValueListType &values,
      const BaseSwitchImmInst::BasicBlockListType &blocks);

  LIRLoadConstInst *createLIRLoadConstInst(Literal *value);

  LoadParamInst *createLoadParamInst(JSDynamicParam *param);

  HBCCreateFunctionEnvironmentInst *createHBCCreateFunctionEnvironmentInst(
      VariableScope *scope,
      JSSpecialParam *parentScopeParam);

  LIRGetThisNSInst *createLIRGetThisNSInst();

  LIRGetArgumentsPropByValLooseInst *createLIRGetArgumentsPropByValLooseInst(
      Value *index,
      AllocStackInst *lazyReg);
  LIRGetArgumentsPropByValStrictInst *createLIRGetArgumentsPropByValStrictInst(
      Value *index,
      AllocStackInst *lazyReg);

  LIRGetArgumentsLengthInst *createLIRGetArgumentsLengthInst(
      Value *lazyRegValue);

  LIRReifyArgumentsLooseInst *createLIRReifyArgumentsLooseInst(
      AllocStackInst *lazyReg);
  LIRReifyArgumentsStrictInst *createLIRReifyArgumentsStrictInst(
      AllocStackInst *lazyReg);

  CreateThisInst *createCreateThisInst(Value *closure, Value *newTarget);

  GetConstructedObjectInst *createGetConstructedObjectInst(
      Instruction *thisValue,
      Value *constructorReturnValue);

  HBCProfilePointInst *createHBCProfilePointInst(uint16_t pointIndex);

  CallBuiltinInst *createCallBuiltinInst(
      BuiltinMethod::Enum builtinIndex,
      ArrayRef<Value *> arguments);

  GetBuiltinClosureInst *createGetBuiltinClosureInst(
      BuiltinMethod::Enum builtinIndex);

  LIRSpillMovInst *createLIRSpillMovInst(Instruction *value);

  LIRAllocObjectFromBufferInst *createLIRAllocObjectFromBufferInst(
      Value *parentObj,
      LIRAllocObjectFromBufferInst::ObjectPropertyMap prop_map);
  LIRAllocTypedObjectFromBufferInst *createLIRAllocTypedObjectFromBufferInst(
      Value *parentObject,
      const LIRAllocTypedObjectFromBufferInst::ObjectPropertyMap &propMap);
  LIRAllocTypedNonEnumObjectFromBufferInst *
  createLIRAllocTypedNonEnumObjectFromBufferInst(
      Value *parentObject,
      const LIRAllocTypedNonEnumObjectFromBufferInst::ObjectPropertyMap
          &propMap);

  HBCCompareBranchInst *createHBCCompareBranchInst(
      Value *left,
      Value *right,
      ValueKind kind,
      BasicBlock *trueBlock,
      BasicBlock *falseBlock);
  HBCCmpBrTypeOfIsInst *createHBCCmpBrTypeOfIsInst(
      Value *arg,
      LiteralTypeOfIsTypes *types,
      BasicBlock *trueBlock,
      BasicBlock *falseBlock);

  IteratorBeginInst *createIteratorBeginInst(AllocStackInst *sourceOrNext);

  IteratorNextInst *createIteratorNextInst(
      AllocStackInst *iterator,
      Value *sourceOrNext);

  IteratorCloseInst *createIteratorCloseInst(
      Value *iterator,
      bool ignoreInnerException);

  UnreachableInst *createUnreachableInst();

  PrLoadInst *createPrLoadInst(
      Value *object,
      size_t propIndex,
      Literal *propName,
      Type checkedType);

  /// \param nonPointer can be set to true when it is known that both the old
  ///     and the new value are not pointers.
  PrStoreInst *createPrStoreInst(
      Value *storedValue,
      Value *object,
      size_t propIndex,
      Literal *propName,
      bool nonPointer);

  FastArrayLoadInst *
  createFastArrayLoadInst(Value *array, Value *index, Type checkedType);

  FastArrayStoreInst *
  createFastArrayStoreInst(Value *storedValue, Value *array, Value *index);
  FastArrayPushInst *createFastArrayPushInst(Value *pushedValue, Value *array);
  FastArrayAppendInst *createFastArrayAppendInst(Value *other, Value *array);
  FastArrayLengthInst *createFastArrayLengthInst(Value *array);

  LoadParentNoTrapsInst *createLoadParentNoTrapsInst(Value *object);
  TypedLoadParentInst *createTypedLoadParentInst(Value *object);

  FUnaryMathInst *createFUnaryMathInst(ValueKind kind, Value *arg);
  FBinaryMathInst *
  createFBinaryMathInst(ValueKind kind, Value *left, Value *right);
  FCompareInst *createFCompareInst(ValueKind kind, Value *left, Value *right);
  HBCFCompareBranchInst *createHBCFCompareBranchInst(
      Value *left,
      Value *right,
      ValueKind kind,
      BasicBlock *trueBlock,
      BasicBlock *falseBlock);

  StringConcatInst *createStringConcatInst(llvh::ArrayRef<Value *> operands);
  HBCStringConcatInst *createHBCStringConcatInst(Value *left, Value *right);

  UnionNarrowTrustedInst *createUnionNarrowTrustedInst(Value *value, Type type);
  CheckedTypeCastInst *createCheckedTypeCastInst(Value *value, Type type);

  LIRDeadValueInst *createLIRDeadValueInst(Type type);

  LiteralNativeSignature *getLiteralNativeSignature(NativeSignature *sig);

  LiteralNativeExtern *getLiteralNativeExtern(NativeExtern *nativeExtern);

  NativeCallInst *createNativeCallInst(
      Type type,
      Value *callee,
      NativeSignature *sig,
      ArrayRef<Value *> args);
  GetNativeRuntimeInst *createGetNativeRuntimeInst();

  LazyCompilationDataInst *createLazyCompilationDataInst(
      LazyCompilationData &&data,
      Variable *capturedThis,
      Value *capturedNewTarget,
      Variable *capturedArguments,
      Variable *homeObject,
      Variable *classCtxConstructor,
      Variable *classCtxInitFuncVar,
      VariableScope *parentVarScope);

  EvalCompilationDataInst *createEvalCompilationDataInst(
      EvalCompilationData &&data,
      Variable *capturedThis,
      Value *capturedNewTarget,
      Variable *capturedArguments,
      Variable *homeObject,
      Variable *classCtxConstructor,
      Variable *classCtxInitFuncVar,
      ArrayRef<VariableScope *> varScopes);

  /// This is an RAII object that saves and restores the source location of the
  /// IRBuilder.
  class ScopedLocationChange {
    ScopedLocationChange(const ScopedLocationChange &) = delete;
    void operator=(const ScopedLocationChange &) = delete;

    IRBuilder &builder_;
    SMLoc oldLocation_;

   public:
    /// Save the builder source location when constructing the object.
    explicit ScopedLocationChange(IRBuilder &builder, SMLoc location)
        : builder_(builder), oldLocation_(builder.getLocation()) {
      builder_.setLocation(location);
    }

    /// Resotre source location when the object is destroyed.
    ~ScopedLocationChange() {
      builder_.setLocation(oldLocation_);
    }
  };

  //--------------------------------------------------------------------------//
  //                  Insertion point Save and Restore                        //
  //--------------------------------------------------------------------------//

  /// This is an RAII object that saves and restores the insertion point of the
  /// IRBuilder.
  class SaveRestore {
    SaveRestore(const SaveRestore &) = delete;
    void operator=(const SaveRestore &) = delete;

    IRBuilder &Builder;
    BasicBlock *BB;
    SMLoc Location;

   public:
    // Save the builder insertion point when constructing the object.
    explicit SaveRestore(IRBuilder &builder)
        : Builder(builder),
          BB(builder.getInsertionBlock()),
          Location(builder.getLocation()) {
      builder.clearLocation();
    }

    // Restore insertion point when the object is destroyed.
    ~SaveRestore() {
      Builder.setInsertionBlock(BB);
      Builder.setLocation(Location);
    }
  };

  /// This is an RAII object that destroys instructions when it is destroyed.
  class InstructionDestroyer {
    InstructionDestroyer(const InstructionDestroyer &) = delete;
    void operator=(const InstructionDestroyer &) = delete;

    llvh::SmallVector<Instruction *, 8> list{};

   public:
    explicit InstructionDestroyer() = default;

    /// \returns true if the instruction \p A is already in the destruction
    /// queue. Notice that this is an O(n) search and should only be used for
    /// debugging.
    bool hasInstruction(Instruction *A) {
      return std::find(list.begin(), list.end(), A) != list.end();
    }

    /// Add the instruction \p  A to the list of instructions to delete.
    void add(Instruction *A) {
#ifndef NDEBUG
      assert(!hasInstruction(A) && "Instruction already in list!");
#endif
      list.push_back(A);
    }

    ~InstructionDestroyer() {
      for (auto *I : list) {
        I->eraseFromParent();
      }
    }
  };
};

} // end namespace hermes

#endif
