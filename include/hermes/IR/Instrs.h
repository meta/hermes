/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_IR_INSTRS_H
#define HERMES_IR_INSTRS_H

#include <string>
#include <utility>

#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/FrontEndDefs/Typeof.h"
#include "hermes/IR/IR.h"

#include "llvh/ADT/SmallVector.h"
#include "llvh/ADT/ilist_node.h"
#include "llvh/IR/CFG.h"
#include "llvh/IR/SymbolTableListTraits.h"

using llvh::ArrayRef;
using llvh::cast;

namespace hermes {

/// Base class for instructions that have exactly one operand. It guarantees
/// that only one operand is pushed and it provides getSingleOperand().
class SingleOperandInst : public Instruction {
  SingleOperandInst(const SingleOperandInst &) = delete;
  void operator=(const SingleOperandInst &) = delete;

  // Make pushOperand private to ensure derived classes don't use it.
  using Instruction::pushOperand;

 protected:
  explicit SingleOperandInst(ValueKind K, Value *Op) : Instruction(K) {
    pushOperand(Op);
  }

 public:
  enum { SingleOperandIdx };

  explicit SingleOperandInst(
      const SingleOperandInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {
    assert(operands.size() == 1 && "SingleOperandInst must have 1 operand!");
  }

  Value *getSingleOperand() const {
    return getOperand(0);
  }

  static bool hasOutput() {
    llvm_unreachable("SingleOperandInst must be inherited.");
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    llvm_unreachable("SingleOperandInst must be inherited.");
  }

 private:
  static bool classof(const Value *V);
};

/// Subclasses of this class are all able to terminate a basic
/// block. Thus, these are all the flow control type of operations.
class TerminatorInst : public Instruction {
  TerminatorInst(const TerminatorInst &) = delete;
  void operator=(const TerminatorInst &) = delete;

 protected:
  explicit TerminatorInst(ValueKind K) : Instruction(K) {
    setType(Type::createNoType());
  }

 public:
  explicit TerminatorInst(
      const TerminatorInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  unsigned getNumSuccessors() const;
  BasicBlock *getSuccessor(unsigned idx) const;
  void setSuccessor(unsigned idx, BasicBlock *B);

  static bool hasOutput() {
    llvm_unreachable("TerminatorInst must be inherited.");
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    llvm_unreachable("TerminatorInst must be inherited.");
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), TerminatorInst);
  }

  using succ_iterator = llvh::SuccIterator<TerminatorInst, BasicBlock>;
  using succ_const_iterator =
      llvh::SuccIterator<const TerminatorInst, const BasicBlock>;
  using succ_range = llvh::iterator_range<succ_iterator>;
  using succ_const_range = llvh::iterator_range<succ_const_iterator>;

 private:
  inline succ_iterator succ_begin() {
    return succ_iterator(this);
  }
  inline succ_const_iterator succ_begin() const {
    return succ_const_iterator(this);
  }
  inline succ_iterator succ_end() {
    return succ_iterator(this, true);
  }
  inline succ_const_iterator succ_end() const {
    return succ_const_iterator(this, true);
  }

 public:
  inline succ_range successors() {
    return succ_range(succ_begin(), succ_end());
  }
  inline succ_const_range successors() const {
    return succ_const_range(succ_begin(), succ_end());
  }
};

class BranchInst : public TerminatorInst {
  BranchInst(const BranchInst &) = delete;
  void operator=(const BranchInst &) = delete;

 public:
  enum { BranchDestIdx };

  BasicBlock *getBranchDest() const {
    return cast<BasicBlock>(getOperand(BranchDestIdx));
  }

  explicit BranchInst(BasicBlock *parent, BasicBlock *dest)
      : TerminatorInst(ValueKind::BranchInstKind) {
    pushOperand(dest);
  }
  explicit BranchInst(const BranchInst *src, llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::BranchInstKind;
  }

  unsigned getNumSuccessorsImpl() const {
    return 1;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    assert(idx == 0 && "BranchInst only have 1 successor!");
    return getBranchDest();
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    assert(idx == 0 && "BranchInst only have 1 successor!");
    setOperand(B, idx);
  }
};

class AddEmptyStringInst : public SingleOperandInst {
  AddEmptyStringInst(const AddEmptyStringInst &) = delete;
  void operator=(const AddEmptyStringInst &) = delete;

 public:
  explicit AddEmptyStringInst(Value *value)
      : SingleOperandInst(ValueKind::AddEmptyStringInstKind, value) {
    setType(*getInherentTypeImpl());
  }
  explicit AddEmptyStringInst(
      const AddEmptyStringInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createString();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AddEmptyStringInstKind;
  }
};

class ToPropertyKeyInst : public SingleOperandInst {
  ToPropertyKeyInst(const ToPropertyKeyInst &) = delete;
  void operator=(const ToPropertyKeyInst &) = delete;

 public:
  explicit ToPropertyKeyInst(Value *value)
      : SingleOperandInst(ValueKind::ToPropertyKeyInstKind, value) {
    setType(Type::createStringOrSymbol());
  }
  explicit ToPropertyKeyInst(
      const ToPropertyKeyInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::ToPropertyKeyInstKind;
  }
};

class CreatePrivateNameInst : public SingleOperandInst {
  CreatePrivateNameInst(const CreatePrivateNameInst &) = delete;
  void operator=(const CreatePrivateNameInst &) = delete;

 public:
  enum { PropertyIdx };
  explicit CreatePrivateNameInst(LiteralString *value)
      : SingleOperandInst(ValueKind::CreatePrivateNameInstKind, value) {
    setType(*getInherentTypeImpl());
  }
  explicit CreatePrivateNameInst(
      const CreatePrivateNameInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {
    setType(*getInherentTypeImpl());
  }

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createPrivateName();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::CreatePrivateNameInstKind;
  }
};

class AsNumberInst : public SingleOperandInst {
  AsNumberInst(const AsNumberInst &) = delete;
  void operator=(const AsNumberInst &) = delete;

 public:
  explicit AsNumberInst(Value *value)
      : SingleOperandInst(ValueKind::AsNumberInstKind, value) {
    setType(*getInherentTypeImpl());
  }
  explicit AsNumberInst(
      const AsNumberInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createNumber();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AsNumberInstKind;
  }
};

class AsNumericInst : public SingleOperandInst {
  AsNumericInst(const AsNumericInst &) = delete;
  void operator=(const AsNumericInst &) = delete;

 public:
  explicit AsNumericInst(Value *value)
      : SingleOperandInst(ValueKind::AsNumericInstKind, value) {
    if (value->getType().isNumberType()) {
      setType(Type::createNumber());
    } else if (value->getType().isBigIntType()) {
      setType(Type::createBigInt());
    } else {
      setType(Type::createNumeric());
    }
  }
  explicit AsNumericInst(
      const AsNumericInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AsNumericInstKind;
  }
};

class AsInt32Inst : public SingleOperandInst {
  AsInt32Inst(const AsInt32Inst &) = delete;
  void operator=(const AsInt32Inst &) = delete;

 public:
  explicit AsInt32Inst(Value *value)
      : SingleOperandInst(ValueKind::AsInt32InstKind, value) {
    setType(*getInherentTypeImpl());
  }
  explicit AsInt32Inst(const AsInt32Inst *src, llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createInt32();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AsInt32InstKind;
  }
};

class AsUint32Inst : public SingleOperandInst {
  AsUint32Inst(const AsUint32Inst &) = delete;
  void operator=(const AsUint32Inst &) = delete;

 public:
  explicit AsUint32Inst(Value *value)
      : SingleOperandInst(ValueKind::AsUint32InstKind, value) {
    setType(*getInherentTypeImpl());
  }
  explicit AsUint32Inst(
      const AsUint32Inst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createUint32();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AsUint32InstKind;
  }
};

class CondBranchInst : public TerminatorInst {
  CondBranchInst(const CondBranchInst &) = delete;
  void operator=(const CondBranchInst &) = delete;

 public:
  enum { ConditionIdx, TrueBlockIdx, FalseBlockIdx };

  Value *getCondition() const {
    return getOperand(ConditionIdx);
  }
  BasicBlock *getTrueDest() const {
    return cast<BasicBlock>(getOperand(TrueBlockIdx));
  }
  BasicBlock *getFalseDest() const {
    return cast<BasicBlock>(getOperand(FalseBlockIdx));
  }

  explicit CondBranchInst(
      BasicBlock *parent,
      Value *cond,
      BasicBlock *trueBlock,
      BasicBlock *falseBlock)
      : TerminatorInst(ValueKind::CondBranchInstKind) {
    pushOperand(cond);
    pushOperand(trueBlock);
    pushOperand(falseBlock);
  }
  explicit CondBranchInst(
      const CondBranchInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::CondBranchInstKind;
  }

  unsigned getNumSuccessorsImpl() const {
    return 2;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    if (idx == 0)
      return getTrueDest();
    if (idx == 1)
      return getFalseDest();
    llvm_unreachable("CondBranchInst only have 2 successors!");
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    assert(idx <= 1 && "CondBranchInst only have 2 successors!");
    setOperand(B, idx + TrueBlockIdx);
  }
};

class ReturnInst : public TerminatorInst {
  ReturnInst(const ReturnInst &) = delete;
  void operator=(const ReturnInst &) = delete;

 public:
  enum { ReturnValueIdx };

  Value *getValue() const {
    return getOperand(ReturnValueIdx);
  }

  explicit ReturnInst(Value *val) : TerminatorInst(ValueKind::ReturnInstKind) {
    pushOperand(val);
  }
  explicit ReturnInst(const ReturnInst *src, llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::ReturnInstKind;
  }

  unsigned getNumSuccessorsImpl() const {
    return 0;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    llvm_unreachable("ReturnInst has no successor!");
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    llvm_unreachable("ReturnInst has no successor!");
  }
};

class AllocStackInst : public Instruction {
  AllocStackInst(const AllocStackInst &) = delete;
  void operator=(const AllocStackInst &) = delete;

  Label variableName;

 public:
  enum { VariableNameIdx };

  explicit AllocStackInst(Identifier varName)
      : Instruction(ValueKind::AllocStackInstKind), variableName(varName) {
    pushOperand(&variableName);
  }
  explicit AllocStackInst(
      const AllocStackInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, {}), variableName(src->getVariableName()) {
    // NOTE: we are playing a little trick here since the Label is not heap
    // allocated. We can't use the normal machinery to copy the operands.
    pushOperand(&variableName);
  }

  Identifier getVariableName() const {
    return variableName.get();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AllocStackInstKind;
  }
};

class LoadStackInst : public SingleOperandInst {
  LoadStackInst(const LoadStackInst &) = delete;
  void operator=(const LoadStackInst &) = delete;

 public:
  explicit LoadStackInst(AllocStackInst *alloc)
      : SingleOperandInst(ValueKind::LoadStackInstKind, alloc) {
    setType(alloc->getType());
  }
  explicit LoadStackInst(
      const LoadStackInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  AllocStackInst *getPtr() const {
    return cast<AllocStackInst>(getSingleOperand());
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setReadStack().setIdempotent();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LoadStackInstKind;
  }
};

class StoreStackInst : public Instruction {
  StoreStackInst(const StoreStackInst &) = delete;
  void operator=(const StoreStackInst &) = delete;

 public:
  enum { StoredValueIdx, PtrIdx };

  Value *getValue() const {
    return getOperand(StoredValueIdx);
  }
  AllocStackInst *getPtr() const {
    return cast<AllocStackInst>(getOperand(PtrIdx));
  }

  explicit StoreStackInst(Value *storedValue, AllocStackInst *ptr)
      : Instruction(ValueKind::StoreStackInstKind) {
    setType(Type::createNoType());
    pushOperand(storedValue);
    pushOperand(ptr);
  }
  explicit StoreStackInst(
      const StoreStackInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setWriteStack().setIdempotent();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::StoreStackInstKind;
  }
};

/// Base class for instructions that always result in a scope. Its operand can
/// be used to determine the VariableScope associated with the resulting scope.
/// This is different from other base classes in that subclasses may perform
/// very different functionality, but it makes it convenient to traverse,
/// optimize, and verify the structure of scopes.
/// Any operand that represents a scope must be a BaseScopeInst until register
/// allocation, at which point inserting Movs will result in the loss of this
/// invariant.
class BaseScopeInst : public Instruction {
  BaseScopeInst(const BaseScopeInst &) = delete;
  void operator=(const BaseScopeInst &) = delete;

 protected:
  enum { VariableScopeIdx, LAST_IDX };

  explicit BaseScopeInst(ValueKind kind, Value *variableScope)
      : Instruction(kind) {
    setType(*getInherentTypeImpl());
    pushOperand(variableScope);
  }

 public:
  explicit BaseScopeInst(
      const BaseScopeInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {
    setType(*getInherentTypeImpl());
  }

  VariableScope *getVariableScope() const {
    return llvh::cast<VariableScope>(getOperand(VariableScopeIdx));
  }
  void setVariableScope(VariableScope *scope) {
    setOperand(scope, VariableScopeIdx);
  }

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createEnvironment();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  WordBitSet<> getChangedOperandsImpl() {
    return {};
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), BaseScopeInst);
  }
};

class GetParentScopeInst : public BaseScopeInst {
  GetParentScopeInst(const GetParentScopeInst &) = delete;
  void operator=(const GetParentScopeInst &) = delete;

  enum { ParentScopeParamIdx = BaseScopeInst::LAST_IDX };

 public:
  explicit GetParentScopeInst(
      VariableScope *scope,
      JSSpecialParam *parentScopeParam)
      : BaseScopeInst(ValueKind::GetParentScopeInstKind, scope) {
    pushOperand(parentScopeParam);
  }
  explicit GetParentScopeInst(
      const GetParentScopeInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseScopeInst(src, operands) {}

  JSSpecialParam *getParentScopeParam() const {
    return cast<JSSpecialParam>(getOperand(ParentScopeParamIdx));
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::GetParentScopeInstKind;
  }
};

class CreateScopeInst : public BaseScopeInst {
  CreateScopeInst(const CreateScopeInst &) = delete;
  void operator=(const CreateScopeInst &) = delete;

  enum { ParentScopeIdx = BaseScopeInst::LAST_IDX };

 public:
  explicit CreateScopeInst(VariableScope *scope, Value *parentScope)
      : BaseScopeInst(ValueKind::CreateScopeInstKind, scope) {
    pushOperand(parentScope);
  }
  explicit CreateScopeInst(
      const CreateScopeInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseScopeInst(src, operands) {}

  Value *getParentScope() const {
    return getOperand(ParentScopeIdx);
  }
  void setParentScope(Value *parentScope) {
    setOperand(parentScope, ParentScopeIdx);
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::CreateScopeInstKind;
  }
};

class ResolveScopeInst : public BaseScopeInst {
  ResolveScopeInst(const ResolveScopeInst &) = delete;
  void operator=(const ResolveScopeInst &) = delete;

  enum { StartVarScopeIdx = BaseScopeInst::LAST_IDX, StartScopeIdx };

 public:
  explicit ResolveScopeInst(
      VariableScope *varScope,
      VariableScope *startVarScope,
      Instruction *startScope)
      : BaseScopeInst(ValueKind::ResolveScopeInstKind, varScope) {
    pushOperand(startVarScope);
    pushOperand(startScope);
  }
  explicit ResolveScopeInst(
      const ResolveScopeInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseScopeInst(src, operands) {}

  VariableScope *getStartVarScope() const {
    return cast<VariableScope>(getOperand(StartVarScopeIdx));
  }
  Instruction *getStartScope() const {
    return cast<Instruction>(getOperand(StartScopeIdx));
  }

  void setStartScope(VariableScope *startVarScope, Instruction *scope) {
    setOperand(startVarScope, StartVarScopeIdx);
    setOperand(scope, StartScopeIdx);
  }

  /// Set only the start variable scope. \c setStartScope should be preferred,
  /// except in the rare case that the VariableScope describing the result of
  /// the incoming scope instruction is known to have changed.
  void setStartVarScope(VariableScope *startVarScope) {
    setOperand(startVarScope, StartVarScopeIdx);
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::ResolveScopeInstKind;
  }
};

class LIRResolveScopeInst : public BaseScopeInst {
  LIRResolveScopeInst(const LIRResolveScopeInst &) = delete;
  void operator=(const LIRResolveScopeInst &) = delete;

 public:
  enum { StartScopeIdx = BaseScopeInst::LAST_IDX, NumLevelsIdx };

  explicit LIRResolveScopeInst(
      VariableScope *varScope,
      Instruction *startScope,
      LiteralNumber *numLevels)
      : BaseScopeInst(ValueKind::LIRResolveScopeInstKind, varScope) {
    pushOperand(startScope);
    pushOperand(numLevels);
  }
  explicit LIRResolveScopeInst(
      const LIRResolveScopeInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseScopeInst(src, operands) {}

  Instruction *getStartScope() const {
    return cast<Instruction>(getOperand(StartScopeIdx));
  }

  LiteralNumber *getNumLevels() const {
    return cast<LiteralNumber>(getOperand(NumLevelsIdx));
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LIRResolveScopeInstKind;
  }
};

class GetClosureScopeInst : public BaseScopeInst {
  GetClosureScopeInst(const GetClosureScopeInst &) = delete;
  void operator=(const GetClosureScopeInst &) = delete;

  enum { FunctionCodeIdx = BaseScopeInst::LAST_IDX, ClosureIdx };

 public:
  explicit GetClosureScopeInst(
      VariableScope *varScope,
      Function *F,
      Value *closure)
      : BaseScopeInst(ValueKind::GetClosureScopeInstKind, varScope) {
    pushOperand(F);
    pushOperand(closure);
  }
  explicit GetClosureScopeInst(
      const GetClosureScopeInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseScopeInst(src, operands) {}

  Value *getClosure() const {
    return cast<Instruction>(getOperand(ClosureIdx));
  }
  Function *getFunctionCode() const {
    return cast<Function>(getOperand(FunctionCodeIdx));
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::GetClosureScopeInstKind;
  }
};

class LoadFrameInst : public Instruction {
  LoadFrameInst(const LoadFrameInst &) = delete;
  void operator=(const LoadFrameInst &) = delete;

 public:
  enum { ScopeIdx, VariableIdx };

  explicit LoadFrameInst(Instruction *scope, Variable *alloc)
      : Instruction(ValueKind::LoadFrameInstKind) {
    setType(alloc->getType());
    pushOperand(scope);
    pushOperand(alloc);
  }
  explicit LoadFrameInst(
      const LoadFrameInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Instruction *getScope() const {
    return cast<Instruction>(getOperand(ScopeIdx));
  }

  Variable *getLoadVariable() const {
    return cast<Variable>(getOperand(VariableIdx));
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setReadFrame().setIdempotent();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LoadFrameInstKind;
  }
};

class StoreFrameInst : public Instruction {
  StoreFrameInst(const StoreFrameInst &) = delete;
  void operator=(const StoreFrameInst &) = delete;

 public:
  enum { ScopeIdx, StoredValueIdx, VariableIdx };

  Instruction *getScope() const {
    return llvh::cast<Instruction>(getOperand(ScopeIdx));
  }
  Value *getValue() const {
    return getOperand(StoredValueIdx);
  }
  Variable *getVariable() const {
    return cast<Variable>(getOperand(VariableIdx));
  }

  explicit StoreFrameInst(Instruction *scope, Value *storedValue, Variable *ptr)
      : Instruction(ValueKind::StoreFrameInstKind) {
    setType(Type::createNoType());
    pushOperand(scope);
    pushOperand(storedValue);
    pushOperand(ptr);
  }
  explicit StoreFrameInst(
      const StoreFrameInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setWriteFrame().setIdempotent();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::StoreFrameInstKind;
  }
};

/// A base class for all instructions that create a lexical child. Some of them
/// don't return a callable.
class BaseCreateLexicalChildInst : public Instruction {
  BaseCreateLexicalChildInst(const BaseCreateLexicalChildInst &) = delete;
  void operator=(const BaseCreateLexicalChildInst &) = delete;

 protected:
  explicit BaseCreateLexicalChildInst(
      ValueKind kind,
      Instruction *scope,
      VariableScope *varScope,
      Function *code)
      : Instruction(kind) {
    pushOperand(scope);
    pushOperand(varScope);
    pushOperand(code);
  }

 public:
  enum { ScopeIdx, VarScopeIdx, FunctionCodeIdx, LAST_IDX };

  explicit BaseCreateLexicalChildInst(
      const BaseCreateLexicalChildInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  /// Get and set the enclosing scope of the function. Note that this may be
  /// EmptySentinel if the function does not use its enclosing scope.
  Value *getScope() const {
    return getOperand(ScopeIdx);
  }
  void setScope(Value *scope) {
    setOperand(scope, ScopeIdx);
  }

  /// Get/set the enclosing VariableScope of the function. Note that this may be
  /// EmptySentinel if the function does not use its enclosing scope.
  Value *getVarScope() const {
    return getOperand(VarScopeIdx);
  }
  void setVarScope(Value *scope) {
    setOperand(scope, VarScopeIdx);
  }

  Function *getFunctionCode() const {
    return cast<Function>(getOperand(FunctionCodeIdx));
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), BaseCreateLexicalChildInst);
  }
};

/// Base class for all instruction creating and returning a callable.
class BaseCreateCallableInst : public BaseCreateLexicalChildInst {
  BaseCreateCallableInst(const BaseCreateCallableInst &) = delete;
  void operator=(const BaseCreateCallableInst &) = delete;

 protected:
  explicit BaseCreateCallableInst(
      ValueKind kind,
      Instruction *scope,
      VariableScope *varScope,
      Function *code)
      : BaseCreateLexicalChildInst(kind, scope, varScope, code) {
    setType(*getInherentTypeImpl());
  }

 public:
  explicit BaseCreateCallableInst(
      const BaseCreateCallableInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseCreateLexicalChildInst(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), BaseCreateCallableInst);
  }
};

/// Create a normal function, or an outer generator function.
class CreateFunctionInst : public BaseCreateCallableInst {
  CreateFunctionInst(const CreateFunctionInst &) = delete;
  void operator=(const CreateFunctionInst &) = delete;

 public:
  explicit CreateFunctionInst(
      Instruction *scope,
      VariableScope *varScope,
      Function *code)
      : BaseCreateCallableInst(
            ValueKind::CreateFunctionInstKind,
            scope,
            varScope,
            code) {
    assert(
        (llvh::isa<NormalFunction>(code) ||
         llvh::isa<GeneratorFunction>(code) ||
         llvh::isa<AsyncFunction>(code)) &&
        "Only NormalFunction/GeneratorFunction supported by CreateFunctionInst");
  }
  explicit CreateFunctionInst(
      const CreateFunctionInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseCreateCallableInst(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::CreateFunctionInstKind;
  }
};

class CreateClassInst : public BaseCreateCallableInst {
  CreateClassInst(const CreateClassInst &) = delete;
  void operator=(const CreateClassInst &) = delete;

 public:
  enum { SuperClassIdx = BaseCreateCallableInst::LAST_IDX, HomeObjectOutIdx };
  explicit CreateClassInst(
      BaseScopeInst *scope,
      Function *code,
      Value *superClass,
      AllocStackInst *homeObjectOutput)
      : BaseCreateCallableInst(
            ValueKind::CreateClassInstKind,
            scope,
            scope->getVariableScope(),
            code) {
    assert(
        (llvh::isa<NormalFunction>(code)) &&
        "Only NormalFunction supported by CreateClassInst");
    setType(*getInherentTypeImpl());
    pushOperand(superClass);
    pushOperand(homeObjectOutput);
  }
  explicit CreateClassInst(
      const CreateClassInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseCreateCallableInst(src, operands) {}

  Value *getHomeObjectOutput() {
    return getOperand(HomeObjectOutIdx);
  }

  Value *getSuperClass() const {
    return getOperand(SuperClassIdx);
  }

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  SideEffect getSideEffectImpl() const {
    if (llvh::isa<EmptySentinel>(getSuperClass())) {
      // Evaluating a base class constructor cannot execute any JS. It only
      // creates the new class constructor, and writes the home object to the
      // given stack location.
      return SideEffect{}.setWriteStack();
    } else {
      // Evaluating a derived class constructor will fetch the `.prototype` of
      // the super class. This property look up can potentially trigger JS. The
      // home object is also written to the given stack location.
      return SideEffect::createExecute().setWriteStack();
    }
  }

  static bool hasOutput() {
    return true;
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::CreateClassInstKind;
  }
};

class BaseCallInst : public Instruction {
  BaseCallInst(const BaseCallInst &) = delete;
  void operator=(const BaseCallInst &) = delete;

 protected:
  /// Operand index of the first arg, starting at 'this'. The arguments are
  /// assumed to be found in [thisIdx_, numOperands).
  size_t thisIdx_;
  explicit BaseCallInst(
      ValueKind kind,
      Value *callee,
      Value *target,
      LiteralBool *calleeIsAlwaysClosure,
      Value *env,
      Value *newTarget,
      Value *thisValue,
      ArrayRef<Value *> args)
      : Instruction(kind) {
    pushOperand(callee);
    pushOperand(target);
    pushOperand(calleeIsAlwaysClosure);
    pushOperand(env);
    pushOperand(newTarget);
    thisIdx_ = getNumOperands();
    pushOperand(thisValue);
    for (const auto &arg : args) {
      pushOperand(arg);
    }
  }
  /// This constructor is used when there are more operands subclasses need to
  /// add before the list of function arguments. It's required that subclass
  /// constructors perform the following in order:
  /// - Push the additional operands they need in the space from NewTargetIdx to
  ///   \p thisIdx.
  /// - Push the function arguments including 'this', which will be starting
  ///   from \p thisIdx.
  explicit BaseCallInst(
      ValueKind kind,
      Value *callee,
      Value *target,
      LiteralBool *calleeIsAlwaysClosure,
      Value *env,
      Value *newTarget,
      size_t thisIdx)
      : Instruction(kind) {
    pushOperand(callee);
    pushOperand(target);
    pushOperand(calleeIsAlwaysClosure);
    pushOperand(env);
    pushOperand(newTarget);
    thisIdx_ = thisIdx;
  }
  explicit BaseCallInst(
      const BaseCallInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {
    thisIdx_ = src->thisIdx_;
  }

  // Forces the code to use the appropriate getters instead of relying on
  // hard-coded offsets when accessing the arguments.
  using Instruction::getOperand;

 public:
  enum {
    CalleeIdx,
    TargetIdx,
    CalleeIsAlwaysClosure,
    EnvIdx,
    NewTargetIdx,
    _LastIdx
  };

  using ArgumentList = llvh::SmallVector<Value *, 2>;

  Value *getCallee() const {
    return getOperand(CalleeIdx);
  }
  void setCallee(Value *v) {
    setOperand(v, CalleeIdx);
  }
  /// \return the IR Function that the instruction is calling,
  ///   or EmptySentinel if it's not known at this time.
  Value *getTarget() const {
    return getOperand(TargetIdx);
  }
  /// Set the target to \p func.
  void setTarget(Function *func) {
    setOperand(func, TargetIdx);
  }

  /// Get a flag indicating whether the callee is known to always be a closure.
  LiteralBool *getCalleeIsAlwaysClosure() const {
    return cast<LiteralBool>(getOperand(CalleeIsAlwaysClosure));
  }

  /// Set the CalleeIsAlwaysClosure flag.
  void setCalleeIsAlwaysClosure(LiteralBool *isAlwaysClosure) {
    setOperand(isAlwaysClosure, CalleeIsAlwaysClosure);
  }

  /// \return the Environment that the target is being called with,
  ///   or EmptySentinel if it's not known at this time.
  Value *getEnvironment() const {
    return getOperand(EnvIdx);
  }
  /// Set the environment to \p env.
  void setEnvironment(Value *env) {
    setOperand(env, EnvIdx);
  }
  Value *getNewTarget() const {
    return getOperand(NewTargetIdx);
  }
  void setNewTarget(Value *newTarget) {
    return setOperand(newTarget, NewTargetIdx);
  }
  /// Get argument 0, the value for 'this'.
  Value *getThis() const {
    return getOperand(thisIdx_);
  }
  /// Get argument by index. 'this' is argument 0.
  Value *getArgument(unsigned idx) {
    return getOperand(thisIdx_ + idx);
  }
  /// Set the value \p v at index \p idx. 'this' is argument 0.
  void setArgument(Value *V, unsigned idx) {
    setOperand(V, thisIdx_ + idx);
  }

  unsigned getNumArguments() const {
    return getNumOperands() - thisIdx_;
  }

  unsigned getThisIdx() const {
    return thisIdx_;
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), BaseCallInst);
  }
};

class CallInst : public BaseCallInst {
  CallInst(const CallInst &) = delete;
  void operator=(const CallInst &) = delete;

 public:
  enum { ThisIdx = BaseCallInst::_LastIdx };
  explicit CallInst(
      Value *callee,
      Value *target,
      LiteralBool *calleeIsAlwaysClosure,
      Value *env,
      Value *newTarget,
      Value *thisValue,
      ArrayRef<Value *> args)
      : BaseCallInst(
            ValueKind::CallInstKind,
            callee,
            target,
            calleeIsAlwaysClosure,
            env,
            newTarget,
            thisValue,
            args) {}
  explicit CallInst(const CallInst *src, llvh::ArrayRef<Value *> operands)
      : BaseCallInst(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::CallInstKind;
  }
};

class HBCCallWithArgCountInst : public BaseCallInst {
  HBCCallWithArgCountInst(const HBCCallWithArgCountInst &) = delete;
  void operator=(const HBCCallWithArgCountInst &) = delete;

 public:
  enum { NumArgLiteralIdx = BaseCallInst::_LastIdx, ThisIdx };
  explicit HBCCallWithArgCountInst(
      Value *callee,
      Value *target,
      LiteralBool *calleeIsAlwaysClosure,
      Value *env,
      Value *newTarget,
      LiteralNumber *argCount,
      Value *thisValue,
      ArrayRef<Value *> args)
      : BaseCallInst(
            ValueKind::HBCCallWithArgCountInstKind,
            callee,
            target,
            calleeIsAlwaysClosure,
            env,
            newTarget,
            ThisIdx) {
    pushOperand(argCount);
    pushOperand(thisValue);
    for (const auto &arg : args) {
      pushOperand(arg);
    }
  }
  explicit HBCCallWithArgCountInst(
      const HBCCallWithArgCountInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseCallInst(src, operands) {}

  /// \return the number of arguments, including 'this'.
  /// This should always match the value in \c getNumArguments.
  Value *getNumArgumentsLiteral() const {
    return getOperand(NumArgLiteralIdx);
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::HBCCallWithArgCountInstKind;
  }
};

/// Call a VM builtin with the specified number and undefined as the "this"
/// parameter.
class CallBuiltinInst : public BaseCallInst {
  CallBuiltinInst(const CallBuiltinInst &) = delete;
  void operator=(const CallBuiltinInst &) = delete;

 public:
  enum { ThisIdx = BaseCallInst::_LastIdx };
  explicit CallBuiltinInst(
      LiteralBuiltinIdx *callee,
      EmptySentinel *target,
      LiteralBool *calleeIsAlwaysClosure,
      EmptySentinel *env,
      LiteralUndefined *undefined,
      ArrayRef<Value *> args)
      : BaseCallInst(
            ValueKind::CallBuiltinInstKind,
            callee,
            target,
            calleeIsAlwaysClosure,
            env,
            /* newTarget */ undefined,
            /* thisValue */ undefined,
            args) {
    assert(
        (uint32_t)callee->getData() < (uint32_t)BuiltinMethod::_count &&
        "invalid builtin call");
  }
  explicit CallBuiltinInst(
      const CallBuiltinInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseCallInst(src, operands) {}

  BuiltinMethod::Enum getBuiltinIndex() const {
    return cast<LiteralBuiltinIdx>(getCallee())->getData();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::CallBuiltinInstKind;
  }
};

class GetBuiltinClosureInst : public Instruction {
  GetBuiltinClosureInst(const GetBuiltinClosureInst &) = delete;
  void operator=(const GetBuiltinClosureInst &) = delete;

 public:
  enum { BuiltinIndexIdx };

  explicit GetBuiltinClosureInst(LiteralBuiltinIdx *builtinIndex)
      : Instruction(ValueKind::GetBuiltinClosureInstKind) {
    assert(
        (uint32_t)builtinIndex->getData() < BuiltinMethod::_count &&
        "invalid builtin call");
    pushOperand(builtinIndex);
    setType(*getInherentTypeImpl());
  }
  explicit GetBuiltinClosureInst(
      const GetBuiltinClosureInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  BuiltinMethod::Enum getBuiltinIndex() const {
    return cast<LiteralBuiltinIdx>(getOperand(BuiltinIndexIdx))->getData();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::GetBuiltinClosureInstKind;
  }
};

class HBCCallNInst : public BaseCallInst {
 public:
  /// The minimum number of args supported by a CallN instruction, including
  /// 'this'.
  static constexpr uint32_t kMinArgs = 1;

  /// The maximum (inclusive) number of args supported by a CallN instruction,
  /// including 'this'.
  static constexpr uint32_t kMaxArgs = 4;

  explicit HBCCallNInst(
      Value *callee,
      Value *target,
      LiteralBool *calleeIsAlwaysClosure,
      Value *env,
      Value *newTarget,
      Value *thisValue,
      ArrayRef<Value *> args)
      : BaseCallInst(
            ValueKind::HBCCallNInstKind,
            callee,
            target,
            calleeIsAlwaysClosure,
            env,
            newTarget,
            thisValue,
            args) {
    // +1 for 'this'.
    assert(
        kMinArgs <= args.size() + 1 && args.size() + 1 <= kMaxArgs &&
        "Invalid arg count for HBCCallNInst");
  }

  explicit HBCCallNInst(
      const HBCCallNInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseCallInst(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::HBCCallNInstKind;
  }
};

class LIRGetGlobalObjectInst : public Instruction {
  LIRGetGlobalObjectInst(const LIRGetGlobalObjectInst &) = delete;
  void operator=(const LIRGetGlobalObjectInst &) = delete;

 public:
  explicit LIRGetGlobalObjectInst()
      : Instruction(ValueKind::LIRGetGlobalObjectInstKind) {
    setType(*getInherentTypeImpl());
  }
  explicit LIRGetGlobalObjectInst(
      const LIRGetGlobalObjectInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRGetGlobalObjectInstKind;
  }
};

class BaseStorePropertyInst : public Instruction {
  BaseStorePropertyInst(const BaseStorePropertyInst &) = delete;
  void operator=(const BaseStorePropertyInst &) = delete;

 protected:
  explicit BaseStorePropertyInst(
      ValueKind kind,
      Value *storedValue,
      Value *object,
      Value *property)
      : Instruction(kind) {
    setType(Type::createNoType());
    pushOperand(storedValue);
    pushOperand(object);
    pushOperand(property);
  }

 public:
  enum { StoredValueIdx, ObjectIdx, PropertyIdx, _LastIdx };

  explicit BaseStorePropertyInst(
      const BaseStorePropertyInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getStoredValue() const {
    return getOperand(StoredValueIdx);
  }
  Value *getObject() const {
    return getOperand(ObjectIdx);
  };
  Value *getProperty() const {
    return getOperand(PropertyIdx);
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), BaseStorePropertyInst);
  }
};

class StorePropertyWithReceiverInst : public BaseStorePropertyInst {
  StorePropertyWithReceiverInst(const StorePropertyWithReceiverInst &) = delete;
  void operator=(const StorePropertyWithReceiverInst &) = delete;

 public:
  enum { ReceiverIdx = BaseStorePropertyInst::_LastIdx, IsStrictIdx };

  explicit StorePropertyWithReceiverInst(
      Value *storedValue,
      Value *globalObject,
      Value *property,
      Value *receiver,
      LiteralBool *isStrict)
      : BaseStorePropertyInst(
            ValueKind::StorePropertyWithReceiverInstKind,
            storedValue,
            globalObject,
            property) {
    pushOperand(receiver);
    pushOperand(isStrict);
  }
  explicit StorePropertyWithReceiverInst(
      const StorePropertyWithReceiverInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseStorePropertyInst(src, operands) {}

  Value *getReceiver() const {
    return getOperand(ReceiverIdx);
  }

  bool getIsStrict() const {
    return cast<LiteralBool>(getOperand(IsStrictIdx))->getValue();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::StorePropertyWithReceiverInstKind;
  }
};

class StorePropertyInst : public BaseStorePropertyInst {
  StorePropertyInst(const StorePropertyInst &) = delete;
  void operator=(const StorePropertyInst &) = delete;

 protected:
  explicit StorePropertyInst(
      ValueKind kind,
      Value *storedValue,
      Value *object,
      Value *property)
      : BaseStorePropertyInst(kind, storedValue, object, property) {}
  explicit StorePropertyInst(
      const StorePropertyInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseStorePropertyInst(src, operands) {}

 public:
  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), StorePropertyInst);
  }
};

class StorePropertyLooseInst : public StorePropertyInst {
  StorePropertyLooseInst(const StorePropertyLooseInst &) = delete;
  void operator=(const StorePropertyLooseInst &) = delete;

 public:
  explicit StorePropertyLooseInst(
      Value *storedValue,
      Value *object,
      Value *property)
      : StorePropertyInst(
            ValueKind::StorePropertyLooseInstKind,
            storedValue,
            object,
            property) {}
  explicit StorePropertyLooseInst(
      const StorePropertyLooseInst *src,
      llvh::ArrayRef<Value *> operands)
      : StorePropertyInst(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::StorePropertyLooseInstKind;
  }
};

class StorePropertyStrictInst : public StorePropertyInst {
  StorePropertyStrictInst(const StorePropertyStrictInst &) = delete;
  void operator=(const StorePropertyStrictInst &) = delete;

 public:
  explicit StorePropertyStrictInst(
      Value *storedValue,
      Value *object,
      Value *property)
      : StorePropertyInst(
            ValueKind::StorePropertyStrictInstKind,
            storedValue,
            object,
            property) {}
  explicit StorePropertyStrictInst(
      const StorePropertyStrictInst *src,
      llvh::ArrayRef<Value *> operands)
      : StorePropertyInst(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::StorePropertyStrictInstKind;
  }
};

class TryStoreGlobalPropertyInst : public BaseStorePropertyInst {
  TryStoreGlobalPropertyInst(const TryStoreGlobalPropertyInst &) = delete;
  void operator=(const TryStoreGlobalPropertyInst &) = delete;

 protected:
  explicit TryStoreGlobalPropertyInst(
      ValueKind kind,
      Value *storedValue,
      Value *globalObject,
      LiteralString *property)
      : BaseStorePropertyInst(kind, storedValue, globalObject, property) {
    assert(
        (llvh::isa<GlobalObject>(globalObject) ||
         llvh::isa<LIRGetGlobalObjectInst>(globalObject)) &&
        "globalObject must refer to the global object");
  }
  explicit TryStoreGlobalPropertyInst(
      const TryStoreGlobalPropertyInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseStorePropertyInst(src, operands) {}

 public:
  LiteralString *getProperty() const {
    return cast<LiteralString>(BaseStorePropertyInst::getProperty());
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), TryStoreGlobalPropertyInst);
  }
};

class TryStoreGlobalPropertyLooseInst : public TryStoreGlobalPropertyInst {
  TryStoreGlobalPropertyLooseInst(const TryStoreGlobalPropertyLooseInst &) =
      delete;
  void operator=(const TryStoreGlobalPropertyLooseInst &) = delete;

 public:
  explicit TryStoreGlobalPropertyLooseInst(
      Value *storedValue,
      Value *globalObject,
      LiteralString *property)
      : TryStoreGlobalPropertyInst(
            ValueKind::TryStoreGlobalPropertyLooseInstKind,
            storedValue,
            globalObject,
            property) {}
  explicit TryStoreGlobalPropertyLooseInst(
      const TryStoreGlobalPropertyLooseInst *src,
      llvh::ArrayRef<Value *> operands)
      : TryStoreGlobalPropertyInst(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::TryStoreGlobalPropertyLooseInstKind;
  }
};

class TryStoreGlobalPropertyStrictInst : public TryStoreGlobalPropertyInst {
  TryStoreGlobalPropertyStrictInst(const TryStoreGlobalPropertyStrictInst &) =
      delete;
  void operator=(const TryStoreGlobalPropertyStrictInst &) = delete;

 public:
  explicit TryStoreGlobalPropertyStrictInst(
      Value *storedValue,
      Value *globalObject,
      LiteralString *property)
      : TryStoreGlobalPropertyInst(
            ValueKind::TryStoreGlobalPropertyStrictInstKind,
            storedValue,
            globalObject,
            property) {}
  explicit TryStoreGlobalPropertyStrictInst(
      const TryStoreGlobalPropertyStrictInst *src,
      llvh::ArrayRef<Value *> operands)
      : TryStoreGlobalPropertyInst(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::TryStoreGlobalPropertyStrictInstKind;
  }
};

class DefineOwnPropertyInst : public Instruction {
  DefineOwnPropertyInst(const DefineOwnPropertyInst &) = delete;
  void operator=(const DefineOwnPropertyInst &) = delete;

 public:
  enum { StoredValueIdx, ObjectIdx, PropertyIdx, IsEnumerableIdx };
  explicit DefineOwnPropertyInst(
      Value *storedValue,
      Value *object,
      Value *property,
      LiteralBool *isEnumerable)
      : Instruction(ValueKind::DefineOwnPropertyInstKind) {
    setType(Type::createNoType());
    pushOperand(storedValue);
    pushOperand(object);
    pushOperand(property);
    pushOperand(isEnumerable);
  }

  Value *getStoredValue() const {
    return getOperand(StoredValueIdx);
  }
  Value *getObject() const {
    return getOperand(ObjectIdx);
  }
  Value *getProperty() const {
    return getOperand(PropertyIdx);
  }
  bool getIsEnumerable() const {
    return cast<LiteralBool>(getOperand(IsEnumerableIdx))->getValue();
  }

  explicit DefineOwnPropertyInst(
      const DefineOwnPropertyInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::DefineOwnPropertyInstKind;
  }
};

class DefineOwnInDenseArrayInst : public Instruction {
  DefineOwnInDenseArrayInst(const DefineOwnInDenseArrayInst &) = delete;
  void operator=(const DefineOwnInDenseArrayInst &) = delete;

 public:
  enum { StoredValueIdx, ArrayIdx, ArrayIndexIdx };
  explicit DefineOwnInDenseArrayInst(
      Value *storedValue,
      Value *array,
      LiteralNumber *arrayIndex)
      : Instruction(ValueKind::DefineOwnInDenseArrayInstKind) {
    setType(Type::createNoType());
    pushOperand(storedValue);
    pushOperand(array);
    pushOperand(arrayIndex);
  }

  Value *getStoredValue() const {
    return getOperand(StoredValueIdx);
  }
  Value *getArray() const {
    return getOperand(ArrayIdx);
  }
  LiteralNumber *getArrayIndex() const {
    return llvh::cast<LiteralNumber>(getOperand(ArrayIndexIdx));
  }

  explicit DefineOwnInDenseArrayInst(
      const DefineOwnInDenseArrayInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setWriteHeap();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::DefineOwnInDenseArrayInstKind;
  }
};

class StoreOwnPrivateFieldInst : public Instruction {
  StoreOwnPrivateFieldInst(const StoreOwnPrivateFieldInst &) = delete;
  void operator=(const StoreOwnPrivateFieldInst &) = delete;

 public:
  enum { StoredValueIdx, ObjectIdx, PropertyIdx };
  explicit StoreOwnPrivateFieldInst(
      Value *storedValue,
      Value *object,
      Value *property)
      : Instruction(ValueKind::StoreOwnPrivateFieldInstKind) {
    assert(
        property->getType().isPrivateNameType() &&
        "can only store private name types");
    setType(Type::createNoType());
    pushOperand(storedValue);
    pushOperand(object);
    pushOperand(property);
  }

  explicit StoreOwnPrivateFieldInst(
      const StoreOwnPrivateFieldInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getStoredValue() const {
    return getOperand(StoredValueIdx);
  }
  Value *getObject() const {
    return getOperand(ObjectIdx);
  };
  Value *getProperty() const {
    return getOperand(PropertyIdx);
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setThrow().setReadHeap().setWriteHeap();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::StoreOwnPrivateFieldInstKind;
  }
};

class PrivateBrandCheckInst : public Instruction {
  PrivateBrandCheckInst(const PrivateBrandCheckInst &) = delete;
  void operator=(const PrivateBrandCheckInst &) = delete;

 public:
  enum { ObjectIdx, BrandIdx };
  explicit PrivateBrandCheckInst(Value *object, Value *brand)
      : Instruction(ValueKind::PrivateBrandCheckInstKind) {
    assert(
        brand->getType().isPrivateNameType() &&
        "brand must be a private name type");
    setType(Type::createNoType());
    pushOperand(object);
    pushOperand(brand);
  }

  explicit PrivateBrandCheckInst(
      const PrivateBrandCheckInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getObject() const {
    return getOperand(ObjectIdx);
  }
  Value *getBrand() const {
    return getOperand(BrandIdx);
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    // The instruction may throw (TypeError if brand not found) and reads the
    // heap (to check if brand is present).
    return SideEffect{}.setThrow().setReadHeap();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::PrivateBrandCheckInstKind;
  }
};

class AddOwnPrivateFieldInst : public Instruction {
  AddOwnPrivateFieldInst(const AddOwnPrivateFieldInst &) = delete;
  void operator=(const AddOwnPrivateFieldInst &) = delete;

 public:
  enum { StoredValueIdx, ObjectIdx, PropertyIdx };
  explicit AddOwnPrivateFieldInst(
      Value *storedValue,
      Value *object,
      Value *property)
      : Instruction(ValueKind::AddOwnPrivateFieldInstKind) {
    assert(
        property->getType().isPrivateNameType() &&
        "can only add private name types");
    setType(Type::createNoType());
    pushOperand(storedValue);
    pushOperand(object);
    pushOperand(property);
  }

  explicit AddOwnPrivateFieldInst(
      const AddOwnPrivateFieldInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getStoredValue() const {
    return getOperand(StoredValueIdx);
  }
  Value *getObject() const {
    return getOperand(ObjectIdx);
  };
  Value *getProperty() const {
    return getOperand(PropertyIdx);
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setThrow().setReadHeap().setWriteHeap();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AddOwnPrivateFieldInstKind;
  }
};

class DefineOwnGetterSetterInst : public Instruction {
  DefineOwnGetterSetterInst(const DefineOwnGetterSetterInst &) = delete;
  void operator=(const DefineOwnGetterSetterInst &) = delete;

 public:
  enum {
    StoredGetterIdx,
    StoredSetterIdx,
    ObjectIdx,
    PropertyIdx,
    IsEnumerableIdx
  };

  Value *getStoredGetter() const {
    return getOperand(StoredGetterIdx);
  }
  Value *getStoredSetter() const {
    return getOperand(StoredSetterIdx);
  }
  Value *getObject() const {
    return getOperand(ObjectIdx);
  }
  Value *getProperty() const {
    return getOperand(PropertyIdx);
  }
  bool getIsEnumerable() const {
    return cast<LiteralBool>(getOperand(IsEnumerableIdx))->getValue();
  }

  explicit DefineOwnGetterSetterInst(
      Value *storedGetter,
      Value *storedSetter,
      Value *object,
      Value *property,
      LiteralBool *isEnumerable)
      : Instruction(ValueKind::DefineOwnGetterSetterInstKind) {
    setType(Type::createNoType());
    pushOperand(storedGetter);
    pushOperand(storedSetter);
    pushOperand(object);
    pushOperand(property);
    pushOperand(isEnumerable);
  }
  explicit DefineOwnGetterSetterInst(
      const DefineOwnGetterSetterInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::DefineOwnGetterSetterInstKind;
  }
};

class DeletePropertyInst : public Instruction {
  DeletePropertyInst(const DeletePropertyInst &) = delete;
  void operator=(const DeletePropertyInst &) = delete;

 protected:
  explicit DeletePropertyInst(ValueKind kind, Value *object, Value *property)
      : Instruction(kind) {
    pushOperand(object);
    pushOperand(property);
  }
  explicit DeletePropertyInst(
      const DeletePropertyInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {
    setType(*getInherentTypeImpl());
  }

 public:
  enum { ObjectIdx, PropertyIdx };

  Value *getObject() const {
    return getOperand(ObjectIdx);
  }
  Value *getProperty() const {
    return getOperand(PropertyIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }
  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createBoolean();
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), DeletePropertyInst);
  }
};

class DeletePropertyLooseInst : public DeletePropertyInst {
  DeletePropertyLooseInst(const DeletePropertyLooseInst &) = delete;
  void operator=(const DeletePropertyLooseInst &) = delete;

 public:
  explicit DeletePropertyLooseInst(Value *object, Value *property)
      : DeletePropertyInst(
            ValueKind::DeletePropertyLooseInstKind,
            object,
            property) {}
  explicit DeletePropertyLooseInst(
      const DeletePropertyLooseInst *src,
      llvh::ArrayRef<Value *> operands)
      : DeletePropertyInst(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::DeletePropertyLooseInstKind;
  }
};

class DeletePropertyStrictInst : public DeletePropertyInst {
  DeletePropertyStrictInst(const DeletePropertyStrictInst &) = delete;
  void operator=(const DeletePropertyStrictInst &) = delete;

 public:
  explicit DeletePropertyStrictInst(Value *object, Value *property)
      : DeletePropertyInst(
            ValueKind::DeletePropertyStrictInstKind,
            object,
            property) {}
  explicit DeletePropertyStrictInst(
      const DeletePropertyStrictInst *src,
      llvh::ArrayRef<Value *> operands)
      : DeletePropertyInst(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::DeletePropertyStrictInstKind;
  }
};

class BaseLoadPropertyInst : public Instruction {
  BaseLoadPropertyInst(const BaseLoadPropertyInst &) = delete;
  void operator=(const BaseLoadPropertyInst &) = delete;

 protected:
  explicit BaseLoadPropertyInst(ValueKind kind, Value *object, Value *property)
      : Instruction(kind) {
    pushOperand(object);
    pushOperand(property);
  }

 public:
  enum { ObjectIdx, PropertyIdx, _LastIdx };

  Value *getObject() const {
    return getOperand(ObjectIdx);
  };
  Value *getProperty() const {
    return getOperand(PropertyIdx);
  }

  explicit BaseLoadPropertyInst(
      const BaseLoadPropertyInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), BaseLoadPropertyInst);
  }
};

class LoadPropertyInst : public BaseLoadPropertyInst {
  LoadPropertyInst(const LoadPropertyInst &) = delete;
  void operator=(const LoadPropertyInst &) = delete;

 public:
  explicit LoadPropertyInst(Value *object, Value *property)
      : BaseLoadPropertyInst(
            ValueKind::LoadPropertyInstKind,
            object,
            property) {}
  explicit LoadPropertyInst(
      const LoadPropertyInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseLoadPropertyInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LoadPropertyInstKind;
  }
};

class LoadPropertyWithReceiverInst : public BaseLoadPropertyInst {
  LoadPropertyWithReceiverInst(const LoadPropertyWithReceiverInst &) = delete;
  void operator=(const LoadPropertyWithReceiverInst &) = delete;

 public:
  enum { ReceiverIdx = BaseLoadPropertyInst::_LastIdx };
  explicit LoadPropertyWithReceiverInst(
      Value *object,
      Value *property,
      Value *receiver)
      : BaseLoadPropertyInst(
            ValueKind::LoadPropertyWithReceiverInstKind,
            object,
            property) {
    pushOperand(receiver);
  }
  explicit LoadPropertyWithReceiverInst(
      const LoadPropertyWithReceiverInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseLoadPropertyInst(src, operands) {}

  Value *getReceiver() const {
    return getOperand(ReceiverIdx);
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LoadPropertyWithReceiverInstKind;
  }
};

class LoadOwnPrivateFieldInst : public Instruction {
  LoadOwnPrivateFieldInst(const LoadOwnPrivateFieldInst &) = delete;
  void operator=(const LoadOwnPrivateFieldInst &) = delete;

 public:
  enum { ObjectIdx, PropertyIdx };
  explicit LoadOwnPrivateFieldInst(Value *object, Value *property)
      : Instruction(ValueKind::LoadOwnPrivateFieldInstKind) {
    assert(
        property->getType().isPrivateNameType() &&
        "can only load private name types");
    pushOperand(object);
    pushOperand(property);
  }
  explicit LoadOwnPrivateFieldInst(
      const LoadOwnPrivateFieldInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getObject() const {
    return getOperand(ObjectIdx);
  };
  Value *getProperty() const {
    return getOperand(PropertyIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setThrow().setReadHeap();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LoadOwnPrivateFieldInstKind;
  }
};

class TryLoadGlobalPropertyInst : public BaseLoadPropertyInst {
  TryLoadGlobalPropertyInst(const TryLoadGlobalPropertyInst &) = delete;
  void operator=(const TryLoadGlobalPropertyInst &) = delete;

 public:
  LiteralString *getProperty() const {
    return cast<LiteralString>(BaseLoadPropertyInst::getProperty());
  }

  explicit TryLoadGlobalPropertyInst(
      Value *globalObject,
      LiteralString *property)
      : BaseLoadPropertyInst(
            ValueKind::TryLoadGlobalPropertyInstKind,
            globalObject,
            property) {
    assert(
        (llvh::isa<GlobalObject>(globalObject) ||
         llvh::isa<LIRGetGlobalObjectInst>(globalObject)) &&
        "globalObject must refer to the global object");
  }
  explicit TryLoadGlobalPropertyInst(
      const TryLoadGlobalPropertyInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseLoadPropertyInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::TryLoadGlobalPropertyInstKind;
  }
};

class LIRAllocObjectFromBufferInst : public Instruction {
  LIRAllocObjectFromBufferInst(const LIRAllocObjectFromBufferInst &) = delete;
  void operator=(const LIRAllocObjectFromBufferInst &) = delete;

 public:
  enum { ParentObjectIdx, FirstKeyIdx };

  using ObjectPropertyMap =
      llvh::SmallVector<std::pair<Literal *, Literal *>, 4>;

  /// \param parentObj is the parent object of the new object.
  /// \param propMap is all the literal key/value entries.
  explicit LIRAllocObjectFromBufferInst(
      Value *parentObj,
      const ObjectPropertyMap &propMap)
      : Instruction(ValueKind::LIRAllocObjectFromBufferInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(parentObj);
    for (size_t i = 0; i < propMap.size(); i++) {
      pushOperand(propMap[i].first);
      pushOperand(propMap[i].second);
    }
  }
  explicit LIRAllocObjectFromBufferInst(
      const LIRAllocObjectFromBufferInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }
  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  Value *getParentObject() const {
    return getOperand(ParentObjectIdx);
  }

  /// Number of consecutive literal key/value pairs in the object.
  unsigned getKeyValuePairCount() const {
    return (getNumOperands() - FirstKeyIdx) / 2;
  }

  /// Return the \index 'd sequential literal key/value pair.
  std::pair<Literal *, Literal *> getKeyValuePair(unsigned index) const {
    return std::pair<Literal *, Literal *>{
        cast<Literal>(getOperand(2 * index + FirstKeyIdx)),
        cast<Literal>(getOperand(1 + 2 * index + FirstKeyIdx))};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRAllocObjectFromBufferInstKind;
  }
};

class LIRAllocTypedObjectFromBufferInst : public Instruction {
  LIRAllocTypedObjectFromBufferInst(const LIRAllocTypedObjectFromBufferInst &) =
      delete;
  void operator=(const LIRAllocTypedObjectFromBufferInst &) = delete;

 public:
  enum { ParentObjectIdx, FirstKeyIdx, _LastIdx };

  using ObjectPropertyMap =
      llvh::SmallVector<std::pair<Literal *, Literal *>, 4>;

  /// \sizeHint is a hint for the VM regarding the final size of this object.
  /// It is the number of entries in the object declaration including
  /// non-literal ones. \prop_map is all the literal key/value entries.
  explicit LIRAllocTypedObjectFromBufferInst(
      Value *parentObject,
      const ObjectPropertyMap &propMap)
      : Instruction(ValueKind::LIRAllocTypedObjectFromBufferInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(parentObject);
    for (size_t i = 0; i < propMap.size(); i++) {
      pushOperand(propMap[i].first);
      pushOperand(propMap[i].second);
    }
  }
  explicit LIRAllocTypedObjectFromBufferInst(
      const LIRAllocTypedObjectFromBufferInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }
  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  Value *getParentObject() const {
    return getOperand(ParentObjectIdx);
  }

  unsigned getKeyValuePairCount() const {
    return (getNumOperands() - FirstKeyIdx) / 2;
  }

  /// \return true if operand \p i represents a key.
  bool isKeyIndex(size_t i) const {
    // Alternates key and value starting at FirstKeyIdx.
    return ((i - FirstKeyIdx) % 2) == 0;
  }

  /// Return the \index 'd sequential literal key/value pair.
  std::pair<Literal *, Literal *> getKeyValuePair(unsigned index) const {
    return std::pair<Literal *, Literal *>{
        cast<Literal>(getOperand(2 * index + FirstKeyIdx)),
        cast<Literal>(getOperand(1 + 2 * index + FirstKeyIdx))};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRAllocTypedObjectFromBufferInstKind;
  }
};

class LIRAllocTypedNonEnumObjectFromBufferInst : public Instruction {
  LIRAllocTypedNonEnumObjectFromBufferInst(
      const LIRAllocTypedNonEnumObjectFromBufferInst &) = delete;
  void operator=(const LIRAllocTypedNonEnumObjectFromBufferInst &) = delete;

 public:
  enum { ParentObjectIdx, FirstKeyIdx, _LastIdx };

  using ObjectPropertyMap =
      llvh::SmallVector<std::pair<Literal *, Literal *>, 4>;

  explicit LIRAllocTypedNonEnumObjectFromBufferInst(
      Value *parentObject,
      const ObjectPropertyMap &propMap)
      : Instruction(ValueKind::LIRAllocTypedNonEnumObjectFromBufferInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(parentObject);
    for (size_t i = 0; i < propMap.size(); i++) {
      pushOperand(propMap[i].first);
      pushOperand(propMap[i].second);
    }
  }
  explicit LIRAllocTypedNonEnumObjectFromBufferInst(
      const LIRAllocTypedNonEnumObjectFromBufferInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }
  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  Value *getParentObject() const {
    return getOperand(ParentObjectIdx);
  }

  unsigned getKeyValuePairCount() const {
    return (getNumOperands() - FirstKeyIdx) / 2;
  }

  /// \return true if operand \p i represents a key.
  bool isKeyIndex(size_t i) const {
    return ((i - FirstKeyIdx) % 2) == 0;
  }

  /// Return the \index 'd sequential literal key/value pair.
  std::pair<Literal *, Literal *> getKeyValuePair(unsigned index) const {
    return std::pair<Literal *, Literal *>{
        cast<Literal>(getOperand(2 * index + FirstKeyIdx)),
        cast<Literal>(getOperand(1 + 2 * index + FirstKeyIdx))};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRAllocTypedNonEnumObjectFromBufferInstKind;
  }
};

class BaseAllocObjectLiteralInst : public Instruction {
  BaseAllocObjectLiteralInst(const BaseAllocObjectLiteralInst &) = delete;
  void operator=(const BaseAllocObjectLiteralInst &) = delete;

 public:
  enum { ParentObjectIdx, FirstKeyIdx, _LastIdx };

  using ObjectPropertyMap = llvh::SmallVector<std::pair<Literal *, Value *>, 4>;

  explicit BaseAllocObjectLiteralInst(
      ValueKind kind,
      Value *parentObject,
      const ObjectPropertyMap &propMap)
      : Instruction(kind) {
    setType(*getInherentTypeImpl());
    pushOperand(parentObject);
    for (size_t i = 0; i < propMap.size(); i++) {
      pushOperand(propMap[i].first);
      pushOperand(propMap[i].second);
    }
  }
  explicit BaseAllocObjectLiteralInst(
      const BaseAllocObjectLiteralInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  Value *getParentObject() const {
    return getOperand(ParentObjectIdx);
  }

  unsigned getKeyValuePairCount() const {
    return (getNumOperands() - FirstKeyIdx) / 2;
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), BaseAllocObjectLiteralInst);
  }

  static unsigned getKeyOperandIdx(unsigned index) {
    return 2 * index + FirstKeyIdx;
  }

  Literal *getKey(unsigned index) const {
    return cast<Literal>(getOperand(2 * index + FirstKeyIdx));
  }

  Value *getValue(unsigned index) const {
    return getOperand(2 * index + 1 + FirstKeyIdx);
  }
};

class AllocObjectLiteralInst : public BaseAllocObjectLiteralInst {
  AllocObjectLiteralInst(const AllocObjectLiteralInst &) = delete;
  void operator=(const AllocObjectLiteralInst &) = delete;

 public:
  explicit AllocObjectLiteralInst(
      Value *parentObject,
      const ObjectPropertyMap &propMap)
      : BaseAllocObjectLiteralInst(
            ValueKind::AllocObjectLiteralInstKind,
            parentObject,
            propMap) {}
  explicit AllocObjectLiteralInst(
      const AllocObjectLiteralInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseAllocObjectLiteralInst(src, operands) {}

  static bool isTyped() {
    return false;
  }
  bool acceptsEmptyTypeImpl() const {
    return false;
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AllocObjectLiteralInstKind;
  }
};

class AllocTypedObjectInst : public BaseAllocObjectLiteralInst {
  AllocTypedObjectInst(const AllocTypedObjectInst &) = delete;
  void operator=(const AllocTypedObjectInst &) = delete;

 public:
  explicit AllocTypedObjectInst(
      Value *parentObject,
      const ObjectPropertyMap &propMap)
      : BaseAllocObjectLiteralInst(
            ValueKind::AllocTypedObjectInstKind,
            parentObject,
            propMap) {}
  explicit AllocTypedObjectInst(
      const AllocTypedObjectInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseAllocObjectLiteralInst(src, operands) {}

  static bool isTyped() {
    return true;
  }
  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AllocTypedObjectInstKind;
  }
};

class AllocTypedNonEnumObjectInst : public BaseAllocObjectLiteralInst {
  AllocTypedNonEnumObjectInst(const AllocTypedNonEnumObjectInst &) = delete;
  void operator=(const AllocTypedNonEnumObjectInst &) = delete;

 public:
  explicit AllocTypedNonEnumObjectInst(
      Value *parentObject,
      const ObjectPropertyMap &propMap)
      : BaseAllocObjectLiteralInst(
            ValueKind::AllocTypedNonEnumObjectInstKind,
            parentObject,
            propMap) {}
  explicit AllocTypedNonEnumObjectInst(
      const AllocTypedNonEnumObjectInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseAllocObjectLiteralInst(src, operands) {}

  static bool isTyped() {
    return true;
  }
  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AllocTypedNonEnumObjectInstKind;
  }
};

class GetTemplateObjectInst : public Instruction {
  GetTemplateObjectInst(const GetTemplateObjectInst &) = delete;
  void operator=(const GetTemplateObjectInst &) = delete;

 public:
  enum { TemplateObjIDIdx, DupIdx, RawStartIdx };

  // Note: Operands are set up using the same layout as
  // HermesBuiltin_getTemplateObject.
  /// \param cookedStrings can contain either LiteralString or Undefined.
  explicit GetTemplateObjectInst(
      LiteralNumber *templateObjID,
      LiteralBool *dup,
      llvh::ArrayRef<LiteralString *> rawStrings,
      llvh::ArrayRef<Value *> cookedStrings)
      : Instruction(ValueKind::GetTemplateObjectInstKind) {
    if (dup->getValue()) {
      assert(
          cookedStrings.empty() &&
          "no cooked strings allowed if they are the same as raw");
    } else {
      assert(
          rawStrings.size() == cookedStrings.size() &&
          "same number of raw and cooked strings required");
    }
    pushOperand(templateObjID);
    pushOperand(dup);
    for (Value *v : rawStrings)
      pushOperand(v);
    for (Value *v : cookedStrings)
      pushOperand(v);
  }
  explicit GetTemplateObjectInst(
      const GetTemplateObjectInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    // Reads/writes the template cache, but only allocates new objects.
    return {};
  }

  /// \return the ID for the template object in cache.
  uint32_t getTemplateObjID() const {
    return cast<LiteralNumber>(getOperand(TemplateObjIDIdx))->asUInt32();
  }

  /// \return whether the cooked strings are the same as the raw strings.
  bool isDup() const {
    return cast<LiteralBool>(getOperand(DupIdx))->getValue();
  }

  /// \return how many raw strings are being stored (same as the number of
  /// cooked strings if they are not duped).
  uint32_t getNumStrings() const {
    return isDup() ? getNumOperands() - RawStartIdx
                   : (getNumOperands() - RawStartIdx) / 2;
  }

  /// \return the \p idx raw string.
  Value *getRawString(uint32_t idx) const {
    return getOperand(RawStartIdx + idx);
  }

  /// For template objects that aren't reusing raw strings as cooked strings,
  /// read the \p idx cooked string.
  Value *getCookedString(uint32_t idx) const {
    assert(!isDup() && "cooked strings are the same as raw strings");
    auto cookedStrIdx = RawStartIdx + getNumStrings();
    return getOperand(cookedStrIdx + idx);
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::GetTemplateObjectInstKind;
  }
};

class AllocArrayInst : public Instruction {
  AllocArrayInst(const AllocArrayInst &) = delete;
  void operator=(const AllocArrayInst &) = delete;

 public:
  enum { SizeHintIdx, ElementStartIdx };

  using ArrayValueList = llvh::SmallVector<Value *, 4>;

  explicit AllocArrayInst(ArrayValueList &val_list, LiteralNumber *sizeHint)
      : Instruction(ValueKind::AllocArrayInstKind) {
    // TODO: refine this type annotation to "array" ?.
    setType(*getInherentTypeImpl());
    pushOperand(sizeHint);
    for (auto val : val_list) {
      pushOperand(val);
    }
  }
  explicit AllocArrayInst(
      const AllocArrayInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  /// Return the size of array to be allocated.
  LiteralNumber *getSizeHint() const {
    return cast<LiteralNumber>(getOperand(SizeHintIdx));
  }

  /// Number of consecutive literal elements in the array.
  unsigned getElementCount() const {
    return getNumOperands() - 1;
  }

  /// Return the real position and value of the \p index's non-elision
  /// element.
  Value *getArrayElement(unsigned index) const {
    return getOperand(ElementStartIdx + index);
  }

  /// Returns the index of the first non-literal element.
  /// Returns -1 if every element is a literal.
  int getFirstNonLiteralIndex() const {
    for (unsigned i = 0, e = getElementCount(); i < e; ++i) {
      if (!llvh::isa<Literal>(getArrayElement(i)))
        return i;
    }
    return -1;
  }

  /// Returns true if ths is an array with only literal elements.
  bool isLiteralArray() const {
    return getFirstNonLiteralIndex() == -1;
  }

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AllocArrayInstKind;
  }
};

class AllocFastArrayInst : public Instruction {
  AllocFastArrayInst(const AllocFastArrayInst &) = delete;
  void operator=(const AllocFastArrayInst &) = delete;

 public:
  enum { CapacityIdx, PrototypeIdx };

  explicit AllocFastArrayInst(LiteralNumber *sizeHint, Value *prototype)
      : Instruction(ValueKind::AllocFastArrayInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(sizeHint);
    pushOperand(prototype);
  }
  explicit AllocFastArrayInst(
      const AllocFastArrayInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  /// \return the capacity to allocate the array with.
  LiteralNumber *getCapacity() const {
    return cast<LiteralNumber>(getOperand(CapacityIdx));
  }

  /// \return the prototype to use for the array.
  Value *getPrototype() const {
    return getOperand(PrototypeIdx);
  }

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::AllocFastArrayInstKind;
  }
};

class CreateArgumentsInst : public Instruction {
  CreateArgumentsInst(const CreateArgumentsInst &) = delete;
  void operator=(const CreateArgumentsInst &) = delete;

 public:
  explicit CreateArgumentsInst(ValueKind kind) : Instruction(kind) {
    setType(*getInherentTypeImpl());
  }
  explicit CreateArgumentsInst(
      const CreateArgumentsInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), CreateArgumentsInst);
  }
};

class CreateArgumentsLooseInst : public CreateArgumentsInst {
  CreateArgumentsLooseInst(const CreateArgumentsLooseInst &) = delete;
  void operator=(const CreateArgumentsLooseInst &) = delete;

 public:
  explicit CreateArgumentsLooseInst()
      : CreateArgumentsInst(ValueKind::CreateArgumentsLooseInstKind) {}
  explicit CreateArgumentsLooseInst(
      const CreateArgumentsLooseInst *src,
      llvh::ArrayRef<Value *> operands)
      : CreateArgumentsInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::CreateArgumentsLooseInstKind;
  }
};

class CreateArgumentsStrictInst : public CreateArgumentsInst {
  CreateArgumentsStrictInst(const CreateArgumentsStrictInst &) = delete;
  void operator=(const CreateArgumentsStrictInst &) = delete;

 public:
  explicit CreateArgumentsStrictInst()
      : CreateArgumentsInst(ValueKind::CreateArgumentsStrictInstKind) {}
  explicit CreateArgumentsStrictInst(
      const CreateArgumentsStrictInst *src,
      llvh::ArrayRef<Value *> operands)
      : CreateArgumentsInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::CreateArgumentsStrictInstKind;
  }
};

class CreateRegExpInst : public Instruction {
  CreateRegExpInst(const CreateRegExpInst &) = delete;
  void operator=(const CreateRegExpInst &) = delete;

 public:
  enum { PatternIdx, FlagsIdx };

  LiteralString *getPattern() const {
    return cast<LiteralString>(getOperand(PatternIdx));
  }
  LiteralString *getFlags() const {
    return cast<LiteralString>(getOperand(FlagsIdx));
  }

  explicit CreateRegExpInst(LiteralString *pattern, LiteralString *flags)
      : Instruction(ValueKind::CreateRegExpInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(pattern);
    pushOperand(flags);
  }
  explicit CreateRegExpInst(
      const CreateRegExpInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::CreateRegExpInstKind;
  }
};

class UnaryOperatorInst : public SingleOperandInst {
 public:
 private:
  UnaryOperatorInst(const UnaryOperatorInst &) = delete;
  void operator=(const UnaryOperatorInst &) = delete;

  // A list of textual representation of the operators above.
  static const char *opStringRepr[HERMES_IR_CLASS_LENGTH(UnaryOperatorInst)];

 public:
  // Convert the operator string \p into the enum representation or assert
  // fail if the string is invalud.
  static ValueKind parseOperator(llvh::StringRef op);

  /// \return the string representation of the operator.
  llvh::StringRef getOperatorStr() {
    return opStringRepr[HERMES_IR_KIND_TO_OFFSET(UnaryOperatorInst, getKind())];
  }

  explicit UnaryOperatorInst(ValueKind kind, Value *value, Type type)
      : SingleOperandInst(kind, value) {
    assert(HERMES_IR_KIND_IN_CLASS(kind, UnaryOperatorInst));
    setType(type);
  }
  explicit UnaryOperatorInst(
      const UnaryOperatorInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const;

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), UnaryOperatorInst);
  }
};

class TypeOfInst : public Instruction {
 public:
 private:
  TypeOfInst(const TypeOfInst &) = delete;
  void operator=(const TypeOfInst &) = delete;

 public:
  explicit TypeOfInst(Value *op) : Instruction(ValueKind::TypeOfInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(op);
  }
  explicit TypeOfInst(const TypeOfInst *src, llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getArgument() const {
    return getOperand(0);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }
  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createString();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::TypeOfInstKind;
  }
};

class TypeOfIsInst : public Instruction {
 private:
  TypeOfIsInst(const TypeOfIsInst &) = delete;
  void operator=(const TypeOfIsInst &) = delete;

 public:
  enum { ArgumentIdx, TypesIdx };

  explicit TypeOfIsInst(Value *op, LiteralTypeOfIsTypes *types)
      : Instruction(ValueKind::TypeOfIsInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(op);
    pushOperand(types);
  }
  explicit TypeOfIsInst(
      const TypeOfIsInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getArgument() const {
    return getOperand(ArgumentIdx);
  }
  LiteralTypeOfIsTypes *getTypes() const {
    return llvh::cast<LiteralTypeOfIsTypes>(getOperand(TypesIdx));
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }
  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createBoolean();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::TypeOfIsInstKind;
  }
};

class BinaryOperatorInst : public Instruction {
 public:
  // A list of textual representation of the operators above.
  static const char *opStringRepr[HERMES_IR_CLASS_LENGTH(BinaryOperatorInst)];

  // A list of textual representation of the assignment operators that match the
  // operators above.
  static const char
      *assignmentOpStringRepr[HERMES_IR_CLASS_LENGTH(BinaryOperatorInst)];

 private:
  BinaryOperatorInst(const BinaryOperatorInst &) = delete;
  void operator=(const BinaryOperatorInst &) = delete;

 public:
  enum { LeftHandSideIdx, RightHandSideIdx };

  Value *getLeftHandSide() const {
    return getOperand(LeftHandSideIdx);
  }
  Value *getRightHandSide() const {
    return getOperand(RightHandSideIdx);
  }

  // Convert the operator string \p into the enum representation or assert
  // fail if the string is invalud.
  static ValueKind parseOperator(llvh::StringRef op);

  // Convert the assignment operator string \p into the enum representation or
  // assert fail if the string is invalud.
  static ValueKind parseAssignmentOperator(llvh::StringRef op);

  /// \return the string representation of the operator.
  llvh::StringRef getOperatorStr() {
    return opStringRepr[HERMES_IR_KIND_TO_OFFSET(
        BinaryOperatorInst, getKind())];
  }

  explicit BinaryOperatorInst(ValueKind kind, Value *left, Value *right)
      : Instruction(kind) {
    assert(
        HERMES_IR_KIND_IN_CLASS(kind, BinaryOperatorInst) &&
        "invalid kind given for binary operator");
    pushOperand(left);
    pushOperand(right);
  }
  explicit BinaryOperatorInst(
      const BinaryOperatorInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return getBinarySideEffect(
        getModule()->getTypeContext(),
        getLeftHandSide()->getType(),
        getRightHandSide()->getType(),
        getKind());
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), BinaryOperatorInst);
  }

  /// Calculate the side effect of a binary operator, given inferred types of
  /// its arguments.
  static SideEffect
  getBinarySideEffect(TypeContext &tc, Type leftTy, Type rightTy, ValueKind op);
};

class ImulInst : public Instruction {
  ImulInst(const ImulInst &) = delete;
  void operator=(const ImulInst &) = delete;

 public:
  enum { LeftIdx, RightIdx };

  explicit ImulInst(Value *left, Value *right)
      : Instruction(ValueKind::ImulInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(left);
    pushOperand(right);
  }
  explicit ImulInst(const ImulInst *src, llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  /// Math.imul always produces an Int32 number, or throws. Unlike the bitwise
  /// ops, it never produces BigInt: BigInt operands make ToInt32 throw a
  /// TypeError instead of coercing.
  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createInt32();
  }

  Value *getLeft() const {
    return getOperand(LeftIdx);
  }
  Value *getRight() const {
    return getOperand(RightIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    // Math.imul semantics: ToInt32(left) then ToInt32(right), each of which
    // may run arbitrary JS (valueOf) and may throw (BigInt/Symbol operands).
    // When both operands are statically known to be Number, the coercions
    // are pure conversions with no side effects.
    if (getLeft()->getType().isNumberType() &&
        getRight()->getType().isNumberType())
      return SideEffect{}.setIdempotent();
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::ImulInstKind;
  }
};

class CatchInst : public Instruction {
  CatchInst(const CatchInst &) = delete;
  void operator=(const CatchInst &) = delete;

 public:
  explicit CatchInst() : Instruction(ValueKind::CatchInstKind) {}
  explicit CatchInst(const CatchInst *src, llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createUnknown().setFirstInBlock();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::CatchInstKind;
  }
};

class BaseThrowInst : public TerminatorInst {
  BaseThrowInst(const BaseThrowInst &) = delete;
  void operator=(const BaseThrowInst &) = delete;

 protected:
  explicit BaseThrowInst(
      ValueKind kind,
      Value *throwInfo,
      BasicBlock *optionalCatchTarget)
      : TerminatorInst(kind) {
    pushOperand(throwInfo);
    if (optionalCatchTarget)
      pushOperand(optionalCatchTarget);
  }
  explicit BaseThrowInst(
      const BaseThrowInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

 public:
  /// - ThrowInfoIdx is the index of a value that determines what is thrown. Its
  /// interpretation depends on the instruction.
  /// - CatchTargetBlockIdx is the optional index of the surrounding catch
  /// block, if there is one.
  enum { ThrowInfoIdx, CatchTargetBlockIdx };

  Value *getThrowInfo() const {
    return getOperand(ThrowInfoIdx);
  }

  /// \return whether this instruction has the optional catch target.
  bool hasCatchTarget() const {
    return getNumOperands() > CatchTargetBlockIdx;
  }
  /// \return the catch target, which must be present.
  BasicBlock *getCatchTarget() const {
    assert(hasCatchTarget() && "no catch target");
    return cast<BasicBlock>(getOperand(CatchTargetBlockIdx));
  }
  /// \return the catch target, or nullptr if there is none.
  BasicBlock *getOptionalCatchTarget() const {
    return hasCatchTarget() ? getCatchTarget() : nullptr;
  }
  /// Update the catch target to match \p optionalCatchTarget, adding or
  /// removing as necessary.
  /// \return true if anything changed.
  bool updateCatchTarget(BasicBlock *optionalCatchTarget) {
    if (optionalCatchTarget) {
      // Add the catch target.
      if (!hasCatchTarget()) {
        pushOperand(optionalCatchTarget);
        return true;
      } else if (getOperand(CatchTargetBlockIdx) != optionalCatchTarget) {
        setOperand(optionalCatchTarget, CatchTargetBlockIdx);
        return true;
      }
    } else {
      // Remove the catch target.
      if (hasCatchTarget()) {
        removeOperand(CatchTargetBlockIdx);
        return true;
      }
    }
    return false;
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setThrow();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::ThrowInstKind ||
        kind == ValueKind::ThrowTypeErrorInstKind;
  }

  unsigned getNumSuccessorsImpl() const {
    return getNumOperands() - CatchTargetBlockIdx;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    assert(idx < getNumSuccessorsImpl() && "invalid BaseThrowInst successor");
    return llvh::cast<BasicBlock>(getOperand(idx + CatchTargetBlockIdx));
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *BB) {
    assert(idx < getNumSuccessorsImpl() && "invalid BaseThrowInst successor");
    setOperand(BB, idx + CatchTargetBlockIdx);
  }
};

class ThrowInst : public BaseThrowInst {
  ThrowInst(const ThrowInst &) = delete;
  void operator=(const ThrowInst &) = delete;

 public:
  explicit ThrowInst(Value *thrownValue, BasicBlock *optionalCatchTarget)
      : BaseThrowInst(
            ValueKind::ThrowInstKind,
            thrownValue,
            optionalCatchTarget) {}
  explicit ThrowInst(const ThrowInst *src, llvh::ArrayRef<Value *> operands)
      : BaseThrowInst(src, operands) {}

  Value *getThrownValue() const {
    return getThrowInfo();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::ThrowInstKind;
  }
};

class ThrowTypeErrorInst : public BaseThrowInst {
  ThrowTypeErrorInst(const ThrowTypeErrorInst &) = delete;
  void operator=(const ThrowTypeErrorInst &) = delete;

 public:
  explicit ThrowTypeErrorInst(Value *message, BasicBlock *optionalCatchTarget)
      : BaseThrowInst(
            ValueKind::ThrowTypeErrorInstKind,
            message,
            optionalCatchTarget) {}
  explicit ThrowTypeErrorInst(
      const ThrowTypeErrorInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseThrowInst(src, operands) {}

  Value *getMessage() const {
    return getThrowInfo();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::ThrowTypeErrorInstKind;
  }
};

class SwitchInst : public TerminatorInst {
  SwitchInst(const SwitchInst &) = delete;
  void operator=(const SwitchInst &) = delete;

 public:
  enum { InputIdx, DefaultBlockIdx, FirstCaseIdx };

  using ValueListType = llvh::SmallVector<Literal *, 8>;
  using BasicBlockListType = llvh::SmallVector<BasicBlock *, 8>;

  /// \returns the number of switch case values.
  unsigned getNumCasePair() const;

  /// Returns the n'th pair of value-basicblock that represent a case
  /// destination.
  std::pair<Literal *, BasicBlock *> getCasePair(unsigned i) const;

  /// \returns the destination of the default target.
  BasicBlock *getDefaultDestination() const;

  /// \returns the input value. This is the value we switch on.
  Value *getInputValue() const;

  explicit SwitchInst(
      Value *input,
      BasicBlock *defaultBlock,
      llvh::ArrayRef<Literal *> values,
      llvh::ArrayRef<BasicBlock *> blocks);
  explicit SwitchInst(const SwitchInst *src, llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::SwitchInstKind;
  }

  unsigned getNumSuccessorsImpl() const {
    return getNumCasePair() + 1;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const;
  void setSuccessorImpl(unsigned idx, BasicBlock *B);
};

class GetPNamesInst : public TerminatorInst {
  GetPNamesInst(const GetPNamesInst &) = delete;
  void operator=(const GetPNamesInst &) = delete;

 public:
  enum { IteratorIdx, BaseIdx, IndexIdx, SizeIdx, OnEmptyIdx, OnSomeIdx };

  explicit GetPNamesInst(
      BasicBlock *parent,
      AllocStackInst *iteratorAddr,
      AllocStackInst *baseAddr,
      AllocStackInst *indexAddr,
      AllocStackInst *sizeAddr,
      BasicBlock *onEmpty,
      BasicBlock *onSome);
  explicit GetPNamesInst(
      const GetPNamesInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  Value *getIterator() const {
    return getOperand(IteratorIdx);
  }
  Value *getBase() const {
    return getOperand(BaseIdx);
  }
  Value *getIndex() const {
    return getOperand(IndexIdx);
  }
  Value *getSize() const {
    return getOperand(SizeIdx);
  }
  BasicBlock *getOnEmptyDest() const {
    return cast<BasicBlock>(getOperand(OnEmptyIdx));
  }
  BasicBlock *getOnSomeDest() const {
    return cast<BasicBlock>(getOperand(OnSomeIdx));
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute().setReadStack().setWriteStack();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::GetPNamesInstKind;
  }

  unsigned getNumSuccessorsImpl() const {
    return 2;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    if (idx == 0)
      return getOnEmptyDest();
    if (idx == 1)
      return getOnSomeDest();
    llvm_unreachable("GetPNamesInst only have 2 successors!");
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    if (idx == 0)
      return setOperand(B, OnEmptyIdx);
    if (idx == 1)
      return setOperand(B, OnSomeIdx);
    llvm_unreachable("GetPNamesInst only have 2 successors!");
  }
};

class GetNextPNameInst : public TerminatorInst {
  GetNextPNameInst(const GetNextPNameInst &) = delete;
  void operator=(const GetNextPNameInst &) = delete;

 public:
  enum {
    PropertyIdx,
    BaseIdx,
    IndexIdx,
    SizeIdx,
    IteratorIdx,
    OnLastIdx,
    OnSomeIdx
  };

  explicit GetNextPNameInst(
      BasicBlock *parent,
      AllocStackInst *propertyAddr,
      AllocStackInst *baseAddr,
      AllocStackInst *indexAddr,
      AllocStackInst *sizeAddr,
      AllocStackInst *iteratorAddr,
      BasicBlock *onLast,
      BasicBlock *onSome);
  explicit GetNextPNameInst(
      const GetNextPNameInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute().setReadStack().setWriteStack();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::GetNextPNameInstKind;
  }

  Value *getPropertyAddr() const {
    return getOperand(PropertyIdx);
  }
  Value *getBaseAddr() const {
    return getOperand(BaseIdx);
  }
  Value *getIndexAddr() const {
    return getOperand(IndexIdx);
  }
  Value *getSizeAddr() const {
    return getOperand(SizeIdx);
  }
  Value *getIteratorAddr() const {
    return getOperand(IteratorIdx);
  }
  BasicBlock *getOnLastDest() const {
    return cast<BasicBlock>(getOperand(OnLastIdx));
  }
  BasicBlock *getOnSomeDest() const {
    return cast<BasicBlock>(getOperand(OnSomeIdx));
  }

  unsigned getNumSuccessorsImpl() const {
    return 2;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    if (idx == 0)
      return getOnLastDest();
    if (idx == 1)
      return getOnSomeDest();
    llvm_unreachable("GetNextPNameInst only have 2 successors!");
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    if (idx == 0)
      return setOperand(B, OnLastIdx);
    if (idx == 1)
      return setOperand(B, OnSomeIdx);
    llvm_unreachable("GetNextPNameInst only have 2 successors!");
  }
};

class TryStartInst : public TerminatorInst {
  TryStartInst(const TryStartInst &) = delete;
  void operator=(const TryStartInst &) = delete;

  // CatchTarget is positioned before TryBody in the successor list,
  // so that during Post Order Scan, we lay out TryBody first.
 public:
  enum { CatchTargetBlockIdx, TryBodyBlockIdx };

  explicit TryStartInst(BasicBlock *tryBodyBlock, BasicBlock *catchTargetBlock)
      : TerminatorInst(ValueKind::TryStartInstKind) {
    pushOperand(catchTargetBlock);
    pushOperand(tryBodyBlock);
  }
  explicit TryStartInst(
      const TryStartInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  BasicBlock *getTryBody() const {
    return cast<BasicBlock>(getOperand(TryBodyBlockIdx));
  }
  BasicBlock *getCatchTarget() const {
    return cast<BasicBlock>(getOperand(CatchTargetBlockIdx));
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createUnknown();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::TryStartInstKind;
  }

  unsigned getNumSuccessorsImpl() const {
    return 2;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    return cast<BasicBlock>(getOperand(idx));
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    setOperand(B, idx);
  }
};

class TryEndInst : public TerminatorInst {
  TryEndInst(const TryEndInst &) = delete;
  void operator=(const TryEndInst &) = delete;

 public:
  enum { CatchTargetBlockIdx, BranchDestIdx };

  explicit TryEndInst(BasicBlock *catchBlock, BasicBlock *branchBlock)
      : TerminatorInst(ValueKind::TryEndInstKind) {
    setType(Type::createNoType());
    pushOperand(catchBlock);
    pushOperand(branchBlock);
  }
  explicit TryEndInst(const TryEndInst *src, llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  BasicBlock *getCatchTarget() const {
    return cast<BasicBlock>(getOperand(CatchTargetBlockIdx));
  }
  BasicBlock *getBranchDest() const {
    return cast<BasicBlock>(getOperand(BranchDestIdx));
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createUnknown();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::TryEndInstKind;
  }

  unsigned getNumSuccessorsImpl() const {
    return 2;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    return cast<BasicBlock>(getOperand(idx));
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    setOperand(B, idx);
  }
};

class BranchIfBuiltinInst : public TerminatorInst {
  BranchIfBuiltinInst(const BranchIfBuiltinInst &) = delete;
  void operator=(const BranchIfBuiltinInst &) = delete;

 public:
  enum { BuiltinIdx, ArgumentIdx, TrueBlockIdx, FalseBlockIdx };

  explicit BranchIfBuiltinInst(
      LiteralBuiltinIdx *builtin,
      Value *argument,
      BasicBlock *trueBlock,
      BasicBlock *falseBlock)
      : TerminatorInst(ValueKind::BranchIfBuiltinInstKind) {
    setType(Type::createNoType());
    pushOperand(builtin);
    pushOperand(argument);
    pushOperand(trueBlock);
    pushOperand(falseBlock);
  }
  explicit BranchIfBuiltinInst(
      const BranchIfBuiltinInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  BuiltinMethod::Enum getBuiltinIndex() const {
    return cast<LiteralBuiltinIdx>(getOperand(BuiltinIdx))->getData();
  }
  Value *getArgument() {
    return getOperand(ArgumentIdx);
  }
  BasicBlock *getTrueBlock() const {
    return cast<BasicBlock>(getOperand(TrueBlockIdx));
  }
  BasicBlock *getFalseBlock() const {
    return cast<BasicBlock>(getOperand(FalseBlockIdx));
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::BranchIfBuiltinInstKind;
  }

  unsigned getNumSuccessorsImpl() const {
    return 2;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    return cast<BasicBlock>(getOperand(TrueBlockIdx + idx));
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    setOperand(B, TrueBlockIdx + idx);
  }
};

class PhiInst : public Instruction {
  PhiInst(const PhiInst &) = delete;
  void operator=(const PhiInst &) = delete;

 public:
  using ValueListType = llvh::SmallVector<Value *, 8>;
  using BasicBlockListType = llvh::SmallVector<BasicBlock *, 8>;

  /// \returns the number of phi incoming values.
  unsigned getNumEntries() const;

  /// Returns the n'th pair of value-basicblock that represent a case
  /// destination.
  std::pair<Value *, BasicBlock *> getEntry(unsigned i) const;

  /// Update the n'th pair of value-basicblock that represent a case
  /// destination.
  void updateEntry(unsigned i, Value *val, BasicBlock *BB);

  /// Add a pair of value-basicblock that represent a case destination.
  void addEntry(Value *val, BasicBlock *BB);

  /// Remove an entry pair (incoming basic block and value) at index \p index.
  void removeEntry(unsigned index);

  /// Remove an entry pair (incoming basic block and value) for incoming block
  // \p BB.
  void removeEntry(BasicBlock *BB);

  explicit PhiInst(
      const ValueListType &values,
      const BasicBlockListType &blocks);
  explicit PhiInst(const PhiInst *src, llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }
  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setFirstInBlock();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::PhiInstKind;
  }

  /// Calculate the result type as a union of all incoming types and update it.
  /// Must be called only after the inst is inserted into a basic block, since
  /// it goes through getModule() to reach the TypeContext.
  void recalculateResultType();

 private:
  /// Remove an entry without recalculating the result type.
  void removeEntryHelper(unsigned index);
};

class MovInst : public SingleOperandInst {
  MovInst(const MovInst &) = delete;
  void operator=(const MovInst &) = delete;

 public:
  explicit MovInst(Value *input)
      : SingleOperandInst(ValueKind::MovInstKind, input) {
    setType(input->getType());
  }
  explicit MovInst(const MovInst *src, llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }
  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::MovInstKind;
  }
};

/// ImplicitMovInst may be emitted as part of lowering to express instructions
/// which perform a Mov as part of their implementation, to registers not
/// explicitly marked as a destination. They serve as IR optimization barriers
/// but emit no bytecode.
class ImplicitMovInst : public SingleOperandInst {
 public:
  explicit ImplicitMovInst(Value *input)
      : SingleOperandInst(ValueKind::ImplicitMovInstKind, input) {
    setType(input->getType());
  }

  explicit ImplicitMovInst(
      const ImplicitMovInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::ImplicitMovInstKind;
  }
};

class CoerceThisNSInst : public SingleOperandInst {
  CoerceThisNSInst(const MovInst &) = delete;
  void operator=(const CoerceThisNSInst &) = delete;

 public:
  explicit CoerceThisNSInst(Value *input)
      : SingleOperandInst(ValueKind::CoerceThisNSInstKind, input) {
    setType(*getInherentTypeImpl());
  }
  explicit CoerceThisNSInst(
      const CoerceThisNSInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::CoerceThisNSInstKind;
  }
};

class DebuggerInst : public Instruction {
  DebuggerInst(const DebuggerInst &) = delete;
  void operator=(const DebuggerInst &) = delete;

 public:
  explicit DebuggerInst() : Instruction(ValueKind::DebuggerInstKind) {
    setType(Type::createNoType());
  }
  explicit DebuggerInst(
      const DebuggerInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createUnknown();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::DebuggerInstKind;
  }
};

class GetNewTargetInst : public Instruction {
  GetNewTargetInst(const GetNewTargetInst &) = delete;
  void operator=(const GetNewTargetInst &) = delete;

 public:
  enum { GetNewTargetParamIdx };

  explicit GetNewTargetInst(Value *param)
      : Instruction(ValueKind::GetNewTargetInstKind) {
    pushOperand(param);
    setType(param->getType());
  }
  explicit GetNewTargetInst(
      const GetNewTargetInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::GetNewTargetInstKind;
  }
};

/// Throw if the operand is "empty", otherwise return it.
class ThrowIfInst : public Instruction {
  ThrowIfInst(const ThrowIfInst &) = delete;
  void operator=(const ThrowIfInst &) = delete;

  /// Record the result type when the instruction was created. The recorded type
  /// must be correct with respect to the instructions that consume it, which
  /// is an invariant enforced by IRGen.
  /// If through optimization we get to a point where the input type is Empty,
  /// meaning that the TDZ is always going to throw, the result type of
  /// ThrowIfInst would theoretically need to be something that represents
  /// an "unreachable" type. Such a type would violate the type requirements of
  /// all instructions consuming the result of ThrowIf, even though they
  /// will never execute. Handling this "unreachable" type everywhere would
  /// require a lot of complexity.
  /// Instead, when we get to that point, we simply return the recorded type.
  Type savedResultType_;

 public:
  enum { CheckedValueIdx, InvalidTypesIdx };

  /// Precondition (enforced by IRBuilder::createThrowIfInst):
  /// invalidTypes->getData() is non-empty and a subset of `Empty | Uninit`.
  /// \p resultType must equal `subtractTy(checkedValue->getType(),
  /// invalidTypes->getData())`.
  explicit ThrowIfInst(
      Value *checkedValue,
      LiteralIRType *invalidTypes,
      Type resultType)
      : Instruction(ValueKind::ThrowIfInstKind),
        savedResultType_(Type::createAnyType()) {
    pushOperand(checkedValue);
    pushOperand(invalidTypes);

    // Result type was computed by the builder, where TypeContext is available.
    setType(resultType);
    updateSavedResultType();
  }
  explicit ThrowIfInst(const ThrowIfInst *src, llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands), savedResultType_(src->savedResultType_) {}

  /// For use only by TypeInference. Returns the result type that was recorded
  /// before type inference. Please see doc for \c savedResultType_.
  Type getSavedResultType() const {
    return savedResultType_;
  }
  /// Record the current result type in \c savedResultType_ . This method only
  /// needs to be called externally (by IRGen) if the operand type wasn't
  /// correct during construction and was changed afterwards.
  void updateSavedResultType() {
    savedResultType_ = getType();
  }

  Value *getCheckedValue() const {
    return getOperand(CheckedValueIdx);
  }
  void setCheckedValue(Value *value) {
    setOperand(value, CheckedValueIdx);
  }

  LiteralIRType *getInvalidTypes() const {
    return cast<LiteralIRType>(getOperand(InvalidTypesIdx));
  }
  /// Update the invalid types set. The new set must satisfy the same
  /// precondition as at construction (non-empty subset of `Empty | Uninit`).
  /// The invariant is enforced by the only caller (InstSimplify), which
  /// derives the new set via intersection with the original.
  void setInvalidTypes(LiteralIRType *invalidTypes) {
    setOperand(invalidTypes, InvalidTypesIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setThrow();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::ThrowIfInstKind;
  }
};

class ThrowIfThisInitializedInst : public Instruction {
  ThrowIfThisInitializedInst(const ThrowIfThisInitializedInst &) = delete;
  void operator=(const ThrowIfThisInitializedInst &) = delete;

 public:
  enum { DerivedClassCheckedThisIdx };

  explicit ThrowIfThisInitializedInst(Value *derivedClassCheckedThis)
      : Instruction(ValueKind::ThrowIfThisInitializedInstKind) {
    pushOperand(derivedClassCheckedThis);
    setType(Type::createNoType());
  }
  explicit ThrowIfThisInitializedInst(
      const ThrowIfThisInitializedInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getDerivedClassCheckedThis() const {
    return getOperand(DerivedClassCheckedThisIdx);
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setThrow();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::ThrowIfThisInitializedInstKind;
  }
};

class HBCResolveParentEnvironmentInst : public BaseScopeInst {
  HBCResolveParentEnvironmentInst(const HBCResolveParentEnvironmentInst &) =
      delete;
  void operator=(const HBCResolveParentEnvironmentInst &) = delete;

 public:
  enum { NumLevelsIdx = BaseScopeInst::LAST_IDX, ParentScopeParamIdx };

  explicit HBCResolveParentEnvironmentInst(
      VariableScope *scope,
      LiteralNumber *numLevels,
      JSSpecialParam *parentScopeParam)
      : BaseScopeInst(ValueKind::HBCResolveParentEnvironmentInstKind, scope) {
    setType(*getInherentTypeImpl());
    pushOperand(numLevels);
    pushOperand(parentScopeParam);
  }
  explicit HBCResolveParentEnvironmentInst(
      const HBCResolveParentEnvironmentInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseScopeInst(src, operands) {}

  JSSpecialParam *getParentScopeParam() const {
    return cast<JSSpecialParam>(getOperand(ParentScopeParamIdx));
  }

  LiteralNumber *getNumLevels() const {
    return cast<LiteralNumber>(getOperand(NumLevelsIdx));
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createEnvironment();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::HBCResolveParentEnvironmentInstKind;
  }
};

/// Superclass for "immediate" switch instructions: those that have a list
/// of pairs of constant case-value types (for some consistent type), and
/// BasicBlock jump targets.
class BaseSwitchImmInst : public TerminatorInst {
  BaseSwitchImmInst(const BaseSwitchImmInst &) = delete;
  void operator=(const BaseSwitchImmInst &) = delete;

 public:
  enum { InputIdx, DefaultBlockIdx, SizeIdx };
  using BasicBlockListType = llvh::SmallVector<BasicBlock *, 8>;

  /// \returns the first case index (which depends on the subtype).
  inline unsigned getFirstCaseIdx() const;

  /// \returns the number of switch case values.
  unsigned getNumCasePair() const {
    // The number of cases is computed as the total number of operands, minus
    // the input value and the default basic block. Take this number and divide
    // it in two, because we are counting pairs.
    return (getNumOperands() - getFirstCaseIdx()) / 2;
  }

  /// \returns the destination of the default target.
  BasicBlock *getDefaultDestination() const {
    return cast<BasicBlock>(getOperand(DefaultBlockIdx));
  }

  /// \returns the input value. This is the value we switch on.
  Value *getInputValue() const {
    return getOperand(InputIdx);
  }

  uint32_t getSize() const {
    return cast<LiteralNumber>(getOperand(SizeIdx))->asUInt32();
  }

  BasicBlock *getSwitchTarget(unsigned i) const {
    // The values and labels are twined together. Find the index of the pair
    // that we are fetching and return the jump target.
    unsigned base = i * 2 + getFirstCaseIdx();
    return cast<BasicBlock>(getOperand(base + 1));
  }

  /// \p input is the discriminator value.
  /// \p defaultBlock is the block to jump to if nothing matches.
  /// \p size     the difference between minValue and the largest value + 1.
  explicit BaseSwitchImmInst(
      ValueKind kind,
      Value *input,
      BasicBlock *defaultBlock,
      LiteralNumber *size);
  explicit BaseSwitchImmInst(
      const BaseSwitchImmInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), BaseSwitchImmInst);
  }

  unsigned getNumSuccessorsImpl() const {
    return getNumCasePair() + 1;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const;
  void setSuccessorImpl(unsigned idx, BasicBlock *B);
};

class UIntSwitchImmInst : public BaseSwitchImmInst {
  UIntSwitchImmInst(const UIntSwitchImmInst &) = delete;
  void operator=(const UIntSwitchImmInst &) = delete;

 public:
  enum { MinValueIdx = BaseSwitchImmInst::SizeIdx + 1, FirstCaseIdx };

  using ValueListType = llvh::SmallVector<LiteralNumber *, 8>;

  /// Returns the n'th pair of value-basicblock that represent a case
  /// destination.
  std::pair<LiteralNumber *, BasicBlock *> getCasePair(unsigned i) const {
    // The values and labels are twined together. Find the index of the pair
    // that we are fetching and return the two values.
    unsigned base = i * 2 + FirstCaseIdx;
    return std::make_pair(
        cast<LiteralNumber>(getOperand(base)),
        cast<BasicBlock>(getOperand(base + 1)));
  }

  uint32_t getMinValue() const {
    return cast<LiteralNumber>(getOperand(MinValueIdx))->asUInt32();
  }

  /// \p input is the discriminator value.
  /// \p defaultBlock is the block to jump to if nothing matches.
  /// \p minValue the smallest (integer) value of all switch cases.
  /// \p size     the difference between minValue and the largest value + 1.
  explicit UIntSwitchImmInst(
      Value *input,
      BasicBlock *defaultBlock,
      LiteralNumber *minValue,
      LiteralNumber *size,
      const ValueListType &values,
      const BasicBlockListType &blocks);
  explicit UIntSwitchImmInst(
      const UIntSwitchImmInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseSwitchImmInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::UIntSwitchImmInstKind;
  }
};

class StringSwitchImmInst : public BaseSwitchImmInst {
  StringSwitchImmInst(const StringSwitchImmInst &) = delete;
  void operator=(const StringSwitchImmInst &) = delete;

 public:
  enum { FirstCaseIdx = BaseSwitchImmInst::SizeIdx + 1 };

  using ValueListType = llvh::SmallVector<LiteralString *, 8>;

  /// Returns the n'th pair of value-basicblock that represent a case
  /// destination.
  std::pair<LiteralString *, BasicBlock *> getCasePair(unsigned i) const {
    // The values and labels are twined together. Find the index of the pair
    // that we are fetching and return the two values.
    unsigned base = i * 2 + FirstCaseIdx;
    return std::make_pair(
        cast<LiteralString>(getOperand(base)),
        cast<BasicBlock>(getOperand(base + 1)));
  }

  /// \returns the destination of the default target.
  BasicBlock *getDefaultDestination() const {
    return cast<BasicBlock>(getOperand(DefaultBlockIdx));
  }

  /// \p input is the discriminator value.
  /// \p defaultBlock is the block to jump to if nothing matches.
  /// \p minValue the smallest (integer) value of all switch cases.
  /// \p size     the difference between minValue and the largest value + 1.
  explicit StringSwitchImmInst(
      Value *input,
      BasicBlock *defaultBlock,
      LiteralNumber *size,
      const ValueListType &values,
      const BasicBlockListType &blocks);
  explicit StringSwitchImmInst(
      const StringSwitchImmInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseSwitchImmInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::StringSwitchImmInstKind;
  }
};

// Now that we've declared the two subtypes of SwitchImmInst, we can implement
// this function.
/* static */
unsigned BaseSwitchImmInst::getFirstCaseIdx() const {
  ValueKind kind = getKind();
  if (kind == ValueKind::UIntSwitchImmInstKind) {
    return UIntSwitchImmInst::FirstCaseIdx;
  } else if (kind == ValueKind::StringSwitchImmInstKind) {
    return StringSwitchImmInst::FirstCaseIdx;
  }
  assert(false && "Unknown SwitchImmInst subtype.\n");
  return 0;
}

class HBCCmpBrTypeOfIsInst : public TerminatorInst {
  HBCCmpBrTypeOfIsInst(const HBCCmpBrTypeOfIsInst &) = delete;
  void operator=(const HBCCmpBrTypeOfIsInst &) = delete;

 public:
  enum { ArgumentIdx, TypesIdx, TrueBlockIdx, FalseBlockIdx };

  explicit HBCCmpBrTypeOfIsInst(
      Value *op,
      LiteralTypeOfIsTypes *types,
      BasicBlock *trueBlock,
      BasicBlock *falseBlock)
      : TerminatorInst(ValueKind::HBCCmpBrTypeOfIsInstKind) {
    setType(Type::createNoType());
    pushOperand(op);
    pushOperand(types);
    pushOperand(trueBlock);
    pushOperand(falseBlock);
  }

  Value *getArgument() const {
    return getOperand(ArgumentIdx);
  }
  LiteralTypeOfIsTypes *getTypes() const {
    return llvh::cast<LiteralTypeOfIsTypes>(getOperand(TypesIdx));
  }

  BasicBlock *getTrueDest() const {
    return cast<BasicBlock>(getOperand(TrueBlockIdx));
  }
  BasicBlock *getFalseDest() const {
    return cast<BasicBlock>(getOperand(FalseBlockIdx));
  }
  explicit HBCCmpBrTypeOfIsInst(
      const HBCCmpBrTypeOfIsInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  unsigned getNumSuccessorsImpl() const {
    return 2;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    if (idx == 0)
      return getTrueDest();
    if (idx == 1)
      return getFalseDest();
    llvm_unreachable("HBCCmpBrTypeOfIsInstKind only have 2 successors!");
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    assert(idx <= 1 && "HBCCmpBrTypeOfIsInstKind only have 2 successors!");
    setOperand(B, idx + TrueBlockIdx);
  }
  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::HBCCmpBrTypeOfIsInstKind;
  }
};

class SaveAndYieldInst : public TerminatorInst {
  SaveAndYieldInst(const SaveAndYieldInst &) = delete;
  void operator=(const SaveAndYieldInst &) = delete;

 public:
  enum { ResultIdx, IsDelegatedIdx, NextBlockIdx };

  Value *getResult() const {
    return getOperand(ResultIdx);
  }

  bool getIsDelegated() const {
    return llvh::cast<LiteralBool>(getOperand(IsDelegatedIdx))->getValue();
  }

  BasicBlock *getNextBlock() const {
    return cast<BasicBlock>(getOperand(NextBlockIdx));
  }

  explicit SaveAndYieldInst(
      Value *result,
      LiteralBool *isDelegated,
      BasicBlock *nextBlock)
      : TerminatorInst(ValueKind::SaveAndYieldInstKind) {
    pushOperand(result);
    pushOperand(isDelegated);
    pushOperand(nextBlock);
  }
  explicit SaveAndYieldInst(
      const SaveAndYieldInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  unsigned getNumSuccessorsImpl() const {
    return 1;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    assert(idx == 0 && "SaveAndYieldInst should only have 1 successor");
    return getNextBlock();
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    assert(idx == 0 && "HBCCompareBranchInst only have 2 successors!");
    setOperand(B, NextBlockIdx);
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::SaveAndYieldInstKind;
  }
};

class DirectEvalInst : public Instruction {
  DirectEvalInst(const DirectEvalInst &) = delete;
  void operator=(const DirectEvalInst &) = delete;

 public:
  enum { EvalTextIdx = 0, StrictCallerIdx = 1 };

  explicit DirectEvalInst(Value *evalText, LiteralBool *strictCaller)
      : Instruction(ValueKind::DirectEvalInstKind) {
    pushOperand(evalText);
    pushOperand(strictCaller);
  }
  explicit DirectEvalInst(
      const DirectEvalInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getEvalText() const {
    return getOperand(EvalTextIdx);
  }
  bool getStrictCaller() const {
    return llvh::cast<LiteralBool>(getOperand(StrictCallerIdx))->getValue();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    // This is equivalent to executing an inner function.
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::DirectEvalInstKind;
  }
};

class DeclareGlobalVarInst : public SingleOperandInst {
  DeclareGlobalVarInst(const DeclareGlobalVarInst &) = delete;
  void operator=(const DeclareGlobalVarInst &) = delete;

 public:
  enum { NameIdx = 0 };

  explicit DeclareGlobalVarInst(LiteralString *value)
      : SingleOperandInst(ValueKind::DeclareGlobalVarInstKind, value) {
    setType(Type::createNoType());
  }
  explicit DeclareGlobalVarInst(
      const DeclareGlobalVarInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  LiteralString *getName() const {
    return llvh::cast<LiteralString>(getSingleOperand());
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::DeclareGlobalVarInstKind;
  }
};

class HBCCreateFunctionEnvironmentInst : public BaseScopeInst {
  HBCCreateFunctionEnvironmentInst(const HBCCreateFunctionEnvironmentInst &) =
      delete;
  void operator=(const HBCCreateFunctionEnvironmentInst &) = delete;

  enum { ParentScopeParamIdx = BaseScopeInst::LAST_IDX };

 public:
  static constexpr size_t kMaxScopeSize = UINT8_MAX;

  explicit HBCCreateFunctionEnvironmentInst(
      VariableScope *scope,
      JSSpecialParam *parentScopeParam)
      : BaseScopeInst(ValueKind::HBCCreateFunctionEnvironmentInstKind, scope) {
    assert(
        scope->getVariables().size() <=
            HBCCreateFunctionEnvironmentInst::kMaxScopeSize &&
        "Scope is too large");
    setType(*getInherentTypeImpl());
    pushOperand(parentScopeParam);
  }
  explicit HBCCreateFunctionEnvironmentInst(
      const HBCCreateFunctionEnvironmentInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseScopeInst(src, operands) {}

  JSSpecialParam *getParentScopeParam() const {
    return cast<JSSpecialParam>(getOperand(ParentScopeParamIdx));
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createEnvironment();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::HBCCreateFunctionEnvironmentInstKind;
  }
};

class HBCProfilePointInst : public Instruction {
  HBCProfilePointInst(const HBCProfilePointInst &) = delete;
  void operator=(const HBCProfilePointInst &) = delete;

  uint16_t pointIndex_{0};

 public:
  explicit HBCProfilePointInst(uint16_t pointIndex)
      : Instruction(ValueKind::HBCProfilePointInstKind),
        pointIndex_(pointIndex) {
    setType(Type::createNoType());
  }

  explicit HBCProfilePointInst(
      const HBCProfilePointInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands), pointIndex_(src->pointIndex_) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::HBCProfilePointInstKind;
  }

  uint16_t getPointIndex() const {
    return pointIndex_;
  }
};

class LIRLoadConstInst : public SingleOperandInst {
  LIRLoadConstInst(const LIRLoadConstInst &) = delete;
  void operator=(const LIRLoadConstInst &) = delete;

 public:
  explicit LIRLoadConstInst(Literal *input)
      : SingleOperandInst(ValueKind::LIRLoadConstInstKind, input) {
    setType(input->getType());
  }
  explicit LIRLoadConstInst(
      const LIRLoadConstInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  Literal *getConst() const {
    return cast<Literal>(getSingleOperand());
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRLoadConstInstKind;
  }
};

/// Load a formal parameter. Parameter 0 is "this", the rest of the params start
/// at index 1.
class LoadParamInst : public SingleOperandInst {
  LoadParamInst(const LoadParamInst &) = delete;
  void operator=(const LoadParamInst &) = delete;

 public:
  explicit LoadParamInst(JSDynamicParam *param)
      : SingleOperandInst(ValueKind::LoadParamInstKind, param) {
    setType(param->getType());
  }
  explicit LoadParamInst(
      const LoadParamInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  JSDynamicParam *getParam() const {
    return cast<JSDynamicParam>(getSingleOperand());
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LoadParamInstKind;
  }
};

/// Load the "this" parameter in non-strict mode, which coerces it to an object,
/// boxing primitives and converting "null" and "undefined" to the global
/// object.
class LIRGetThisNSInst : public Instruction {
  LIRGetThisNSInst(const LIRGetThisNSInst &) = delete;
  void operator=(const LIRGetThisNSInst &) = delete;

 public:
  explicit LIRGetThisNSInst() : Instruction(ValueKind::LIRGetThisNSInstKind) {
    setType(*getInherentTypeImpl());
  }
  explicit LIRGetThisNSInst(
      const LIRGetThisNSInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRGetThisNSInstKind;
  }
};

// Get `arguments.length`, without having to create a real array.
class LIRGetArgumentsLengthInst : public SingleOperandInst {
  LIRGetArgumentsLengthInst(const LIRGetArgumentsLengthInst &) = delete;
  void operator=(const LIRGetArgumentsLengthInst &) = delete;

 public:
  explicit LIRGetArgumentsLengthInst(Value *lazyRegValue)
      : SingleOperandInst(
            ValueKind::LIRGetArgumentsLengthInstKind,
            lazyRegValue) {}
  explicit LIRGetArgumentsLengthInst(
      const LIRGetArgumentsLengthInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  Value *getLazyRegister() const {
    return getSingleOperand();
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRGetArgumentsLengthInstKind;
  }
};

// Get `arguments[i]` without having to create a real array.
// This is the base class for the strict and loose variants defined below.
class HBCGetArgumentsPropByValInst : public Instruction {
  HBCGetArgumentsPropByValInst(const HBCGetArgumentsPropByValInst &) = delete;
  void operator=(const HBCGetArgumentsPropByValInst &) = delete;

 protected:
  explicit HBCGetArgumentsPropByValInst(
      ValueKind kind,
      Value *index,
      AllocStackInst *reg)
      : Instruction(kind) {
    pushOperand(index);
    pushOperand(reg);
  }
  explicit HBCGetArgumentsPropByValInst(
      const HBCGetArgumentsPropByValInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

 public:
  enum { IndexIdx, LazyRegisterIdx };

  Value *getIndex() const {
    return getOperand(IndexIdx);
  }
  Value *getLazyRegister() const {
    return getOperand(LazyRegisterIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute().setReadStack().setWriteStack();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), HBCGetArgumentsPropByValInst);
  }
};

class LIRGetArgumentsPropByValLooseInst : public HBCGetArgumentsPropByValInst {
  LIRGetArgumentsPropByValLooseInst(const LIRGetArgumentsPropByValLooseInst &) =
      delete;
  void operator=(const LIRGetArgumentsPropByValLooseInst &) = delete;

 public:
  explicit LIRGetArgumentsPropByValLooseInst(Value *index, AllocStackInst *reg)
      : HBCGetArgumentsPropByValInst(
            ValueKind::LIRGetArgumentsPropByValLooseInstKind,
            index,
            reg) {}
  explicit LIRGetArgumentsPropByValLooseInst(
      const LIRGetArgumentsPropByValLooseInst *src,
      llvh::ArrayRef<Value *> operands)
      : HBCGetArgumentsPropByValInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRGetArgumentsPropByValLooseInstKind;
  }
};

class LIRGetArgumentsPropByValStrictInst : public HBCGetArgumentsPropByValInst {
  LIRGetArgumentsPropByValStrictInst(
      const LIRGetArgumentsPropByValStrictInst &) = delete;
  void operator=(const LIRGetArgumentsPropByValStrictInst &) = delete;

 public:
  explicit LIRGetArgumentsPropByValStrictInst(Value *index, AllocStackInst *reg)
      : HBCGetArgumentsPropByValInst(
            ValueKind::LIRGetArgumentsPropByValStrictInstKind,
            index,
            reg) {}
  explicit LIRGetArgumentsPropByValStrictInst(
      const LIRGetArgumentsPropByValStrictInst *src,
      llvh::ArrayRef<Value *> operands)
      : HBCGetArgumentsPropByValInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRGetArgumentsPropByValStrictInstKind;
  }
};

// Create a real array for `arguments` for when getting the length and elements
// by index isn't enough.
// This is the base class for the strict and loose variants defined below.
class HBCReifyArgumentsInst : public SingleOperandInst {
  HBCReifyArgumentsInst(const HBCReifyArgumentsInst &) = delete;
  void operator=(const HBCReifyArgumentsInst &) = delete;

 protected:
  explicit HBCReifyArgumentsInst(ValueKind kind, AllocStackInst *reg)
      : SingleOperandInst(kind, reg) {
    setType(Type::createNoType());
  }
  explicit HBCReifyArgumentsInst(
      const HBCReifyArgumentsInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

 public:
  Value *getLazyRegister() const {
    return getSingleOperand();
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    // This instruction could throw in theory, if there are too many arguments.
    // However, this is not a meaningful side-effect, since this instruction
    // should be freely reordered or deleted.
    return SideEffect{}.setReadStack().setWriteStack().setIdempotent();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), HBCReifyArgumentsInst);
  }
};

class LIRReifyArgumentsStrictInst : public HBCReifyArgumentsInst {
  LIRReifyArgumentsStrictInst(const LIRReifyArgumentsStrictInst &) = delete;
  void operator=(const LIRReifyArgumentsStrictInst &) = delete;

 public:
  explicit LIRReifyArgumentsStrictInst(AllocStackInst *reg)
      : HBCReifyArgumentsInst(ValueKind::LIRReifyArgumentsStrictInstKind, reg) {
  }
  explicit LIRReifyArgumentsStrictInst(
      const LIRReifyArgumentsStrictInst *src,
      llvh::ArrayRef<Value *> operands)
      : HBCReifyArgumentsInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRReifyArgumentsStrictInstKind;
  }
};

class LIRReifyArgumentsLooseInst : public HBCReifyArgumentsInst {
  LIRReifyArgumentsLooseInst(const LIRReifyArgumentsLooseInst &) = delete;
  void operator=(const LIRReifyArgumentsLooseInst &) = delete;

 public:
  explicit LIRReifyArgumentsLooseInst(AllocStackInst *reg)
      : HBCReifyArgumentsInst(ValueKind::LIRReifyArgumentsLooseInstKind, reg) {}
  explicit LIRReifyArgumentsLooseInst(
      const LIRReifyArgumentsLooseInst *src,
      llvh::ArrayRef<Value *> operands)
      : HBCReifyArgumentsInst(src, operands) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRReifyArgumentsLooseInstKind;
  }
};

/// Create a 'this' object to be filled in by a constructor.
class CreateThisInst : public Instruction {
  CreateThisInst(const CreateThisInst &) = delete;
  void operator=(const CreateThisInst &) = delete;

 public:
  enum { ClosureIdx, NewTargetIdx, FunctionCodeIdx };

  explicit CreateThisInst(Value *closure, Value *newTarget, Value *functionCode)
      : Instruction(ValueKind::CreateThisInstKind) {
    pushOperand(closure);
    pushOperand(newTarget);
    pushOperand(functionCode);
  }
  explicit CreateThisInst(
      const CreateThisInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getClosure() const {
    return getOperand(ClosureIdx);
  }
  Value *getNewTarget() const {
    return getOperand(NewTargetIdx);
  }
  Value *getFunctionCode() const {
    return getOperand(FunctionCodeIdx);
  }
  void setFunctionCode(Function *F) {
    setOperand(F, FunctionCodeIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    // If we know we are calling a legacy class constructor, then we know this
    // instruction is a no-op because it will just produce undefined.
    if (Function *F = llvh::dyn_cast<Function>(getFunctionCode()))
      if (F->getDefinitionKind() ==
              Function::DefinitionKind::ES6BaseConstructor ||
          F->getDefinitionKind() ==
              Function::DefinitionKind::ES6DerivedConstructor) {
        return SideEffect{}.setIdempotent();
      }
    // Otherwise, this instruction will fetch the .prototype property on the
    // newTarget. If it's a proxy, that can execute JS.
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::CreateThisInstKind;
  }
};

/// Choose between 'this' and the object returned by a constructor.
class GetConstructedObjectInst : public Instruction {
  GetConstructedObjectInst(const GetConstructedObjectInst &) = delete;
  void operator=(const GetConstructedObjectInst &) = delete;

 public:
  enum { ThisValueIdx, ConstructorReturnValueIdx };

  explicit GetConstructedObjectInst(
      Instruction *thisValue,
      Value *constructorReturnValue)
      : Instruction(ValueKind::GetConstructedObjectInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(thisValue);
    pushOperand(constructorReturnValue);
  }
  explicit GetConstructedObjectInst(
      const GetConstructedObjectInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getThisValue() const {
    // While originally a CreateThisInst, it may have been replaced by a mov
    // or similar.
    return getOperand(ThisValueIdx);
  }
  Value *getConstructorReturnValue() const {
    return getOperand(ConstructorReturnValueIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }
  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  SideEffect getSideEffectImpl() const {
    // This instruction cannot be hoisted above the instructions that precede
    // it, the instructions that specifically validate that the construct call
    // happened legally. Otherwise this instruction could end up returning some
    // value that doesn't make sense, since the construct call was going to
    // throw before this instruction should be executed.
    return SideEffect{}.setIdempotent().setUnhoistable();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::GetConstructedObjectInstKind;
  }
};

/// Identical to a Mov, except it should never be eliminated.
/// Elimination will undo spilling and cause failures during bc gen.
class LIRSpillMovInst : public SingleOperandInst {
  LIRSpillMovInst(const LIRSpillMovInst &) = delete;
  void operator=(const LIRSpillMovInst &) = delete;

 public:
  explicit LIRSpillMovInst(Instruction *value)
      : SingleOperandInst(ValueKind::LIRSpillMovInstKind, value) {
    setType(value->getType());
  }
  explicit LIRSpillMovInst(
      const LIRSpillMovInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands) {}

  Instruction *getValue() const {
    return cast<Instruction>(getSingleOperand());
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::LIRSpillMovInstKind;
  }
};

class HBCCompareBranchInst : public TerminatorInst {
  HBCCompareBranchInst(const HBCCompareBranchInst &) = delete;
  void operator=(const HBCCompareBranchInst &) = delete;

 public:
  enum { LeftHandSideIdx, RightHandSideIdx, TrueBlockIdx, FalseBlockIdx };

  Value *getLeftHandSide() const {
    return getOperand(LeftHandSideIdx);
  }
  Value *getRightHandSide() const {
    return getOperand(RightHandSideIdx);
  }
  BasicBlock *getTrueDest() const {
    return cast<BasicBlock>(getOperand(TrueBlockIdx));
  }
  BasicBlock *getFalseDest() const {
    return cast<BasicBlock>(getOperand(FalseBlockIdx));
  }

  /// \return the string representation of the operator.
  llvh::StringRef getOperatorStr() {
    return BinaryOperatorInst::opStringRepr[HERMES_IR_KIND_TO_OFFSET(
        HBCCompareBranchInst, getKind())];
  }

  explicit HBCCompareBranchInst(
      ValueKind kind,
      Value *left,
      Value *right,
      BasicBlock *trueBlock,
      BasicBlock *falseBlock)
      : TerminatorInst(kind) {
    assert(HERMES_IR_KIND_IN_CLASS(kind, HBCCompareBranchInst));
    pushOperand(left);
    pushOperand(right);
    pushOperand(trueBlock);
    pushOperand(falseBlock);
  }
  explicit HBCCompareBranchInst(
      const HBCCompareBranchInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  /// Return the BinaryOperatorInst ValueKind corresponding to this comparison.
  ValueKind toBinaryOperatorValueKind() const {
    return HERMES_IR_OFFSET_TO_KIND(
        BinaryOperatorInst,
        HERMES_IR_KIND_TO_OFFSET(HBCCompareBranchInst, getKind()));
  }

  /// Convert a BinaryOperatorInst ValueKind into a HBCCompareBranchInst one.
  static ValueKind fromBinaryOperatorValueKind(ValueKind kind) {
    int ofs = HERMES_IR_KIND_TO_OFFSET(BinaryOperatorInst, kind);
    assert(
        ofs >= 0 && ofs < HERMES_IR_CLASS_LENGTH(HBCCompareBranchInst) &&
        "Invalid CmpBr ValueKind");
    return HERMES_IR_OFFSET_TO_KIND(HBCCompareBranchInst, ofs);
  }

  // static ValueKind kindFromBinaryOperatorValueKind

  SideEffect getSideEffectImpl() const {
    return BinaryOperatorInst::getBinarySideEffect(
        getModule()->getTypeContext(),
        getLeftHandSide()->getType(),
        getRightHandSide()->getType(),
        toBinaryOperatorValueKind());
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), HBCCompareBranchInst);
  }

  unsigned getNumSuccessorsImpl() const {
    return 2;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    if (idx == 0)
      return getTrueDest();
    if (idx == 1)
      return getFalseDest();
    llvm_unreachable("HBCCompareBranchInst only have 2 successors!");
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    assert(idx <= 1 && "HBCCompareBranchInst only have 2 successors!");
    setOperand(B, idx + TrueBlockIdx);
  }
};

/// Create a generator instance with an inner function. Note that the result is
/// not a callable.
class CreateGeneratorInst : public BaseCreateLexicalChildInst {
  CreateGeneratorInst(const CreateGeneratorInst &) = delete;
  void operator=(const CreateGeneratorInst &) = delete;

 public:
  explicit CreateGeneratorInst(
      Instruction *scope,
      VariableScope *varScope,
      NormalFunction *genFunction)
      : BaseCreateLexicalChildInst(
            ValueKind::CreateGeneratorInstKind,
            scope,
            varScope,
            genFunction) {
    setType(*getInherentTypeImpl());
  }
  explicit CreateGeneratorInst(
      const CreateGeneratorInst *src,
      llvh::ArrayRef<Value *> operands)
      : BaseCreateLexicalChildInst(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::CreateGeneratorInstKind;
  }
};

class ResumeGeneratorInst : public Instruction {
  ResumeGeneratorInst(const ResumeGeneratorInst &) = delete;
  void operator=(const ResumeGeneratorInst &) = delete;

 public:
  enum { IsReturnIdx };

  explicit ResumeGeneratorInst(AllocStackInst *isReturn)
      : Instruction(ValueKind::ResumeGeneratorInstKind) {
    pushOperand(isReturn);
  }
  explicit ResumeGeneratorInst(
      const ResumeGeneratorInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createUnknown().setWriteStack();
  }

  Value *getIsReturn() {
    return getOperand(IsReturnIdx);
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::ResumeGeneratorInstKind;
  }
};

class IteratorBeginInst : public Instruction {
  IteratorBeginInst(const IteratorBeginInst &) = delete;
  void operator=(const IteratorBeginInst &) = delete;

 public:
  enum { SourceOrNextIdx };

  explicit IteratorBeginInst(AllocStackInst *sourceOrNext)
      : Instruction(ValueKind::IteratorBeginInstKind) {
    pushOperand(sourceOrNext);
  }
  explicit IteratorBeginInst(
      const IteratorBeginInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute().setReadStack().setWriteStack();
  }

  Value *getSourceOrNext() const {
    return getOperand(SourceOrNextIdx);
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::IteratorBeginInstKind;
  }
};

class IteratorNextInst : public Instruction {
  IteratorNextInst(const IteratorNextInst &) = delete;
  void operator=(const IteratorNextInst &) = delete;

 public:
  enum { IteratorIdx, SourceOrNextIdx };

  explicit IteratorNextInst(AllocStackInst *iterator, Value *sourceOrNext)
      : Instruction(ValueKind::IteratorNextInstKind) {
    pushOperand(iterator);
    pushOperand(sourceOrNext);
  }
  explicit IteratorNextInst(
      const IteratorNextInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute().setReadStack().setWriteStack();
  }

  Value *getIterator() const {
    return getOperand(IteratorIdx);
  }
  Value *getSourceOrNext() const {
    return getOperand(SourceOrNextIdx);
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::IteratorNextInstKind;
  }
};

class IteratorCloseInst : public Instruction {
  IteratorCloseInst(const IteratorCloseInst &) = delete;
  void operator=(const IteratorCloseInst &) = delete;

 public:
  enum { IteratorIdx, IgnoreInnerExceptionIdx };

  using TargetList = llvh::SmallVector<Value *, 2>;

  explicit IteratorCloseInst(Value *iterator, LiteralBool *ignoreInnerException)
      : Instruction(ValueKind::IteratorCloseInstKind) {
    pushOperand(iterator);
    pushOperand(ignoreInnerException);
  }
  explicit IteratorCloseInst(
      const IteratorCloseInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  Value *getIterator() const {
    return getOperand(IteratorIdx);
  }

  bool getIgnoreInnerException() const {
    return cast<LiteralBool>(getOperand(IgnoreInnerExceptionIdx))->getValue();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::IteratorCloseInstKind;
  }
};

/// A bytecode version of llvm_unreachable, for use in stubs and similar.
class UnreachableInst : public TerminatorInst {
  UnreachableInst(const UnreachableInst &) = delete;
  void operator=(const UnreachableInst &) = delete;

 public:
  explicit UnreachableInst() : TerminatorInst(ValueKind::UnreachableInstKind) {}
  explicit UnreachableInst(
      const UnreachableInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return false;
  }

  unsigned getNumSuccessorsImpl() const {
    return 0;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    llvm_unreachable("unreachable has no successors");
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    llvm_unreachable("unreachable has no successors");
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createExecute();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::UnreachableInstKind;
  }
};

class PrLoadInst : public Instruction {
 public:
  enum { ObjectIdx, PropIndexIdx, PropNameIdx };

  explicit PrLoadInst(
      Value *object,
      LiteralNumber *propIndex,
      Literal *propName,
      Type checkedType)
      : Instruction(ValueKind::PrLoadInstKind) {
    assert(
        llvh::isa<LiteralString>(propName) ||
        llvh::isa<LiteralPrivateName>(propName));
    setType(checkedType);
    pushOperand(object);
    pushOperand(propIndex);
    pushOperand(propName);
  }
  explicit PrLoadInst(const PrLoadInst *src, llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::PrLoadInstKind;
  }
  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setReadHeap().setIdempotent();
  }

  Value *getObject() const {
    return getOperand(ObjectIdx);
  }
  size_t getPropIndex() const {
    return (size_t)llvh::cast<LiteralNumber>(getOperand(PropIndexIdx))
        ->getValue();
  }
  LiteralString *getPropName() const {
    return llvh::cast<LiteralString>(getOperand(PropNameIdx));
  }
};

class PrStoreInst : public Instruction {
 public:
  enum { StoredValueIdx, ObjectIdx, PropIndexIdx, PropNameIdx, NonPointerIdx };

  /// \param nonPointer set to true when we know that both the old and new
  ///     value are non-pointers.
  explicit PrStoreInst(
      Value *storedValue,
      Value *object,
      LiteralNumber *propIndex,
      Literal *propName,
      LiteralBool *nonPointer)
      : Instruction(ValueKind::PrStoreInstKind) {
    assert(
        llvh::isa<LiteralString>(propName) ||
        llvh::isa<LiteralPrivateName>(propName));
    setType(Type::createNoType());
    pushOperand(storedValue);
    pushOperand(object);
    pushOperand(propIndex);
    pushOperand(propName);
    pushOperand(nonPointer);
  }
  explicit PrStoreInst(const PrStoreInst *src, llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::PrStoreInstKind;
  }
  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setWriteHeap().setIdempotent();
  }

  Value *getStoredValue() const {
    return getOperand(StoredValueIdx);
  }
  Value *getObject() const {
    return getOperand(ObjectIdx);
  }
  size_t getPropIndex() const {
    return (size_t)llvh::cast<LiteralNumber>(getOperand(PropIndexIdx))
        ->getValue();
  }
  LiteralString *getPropName() const {
    return llvh::cast<LiteralString>(getOperand(PropNameIdx));
  }
  bool getNonPointer() const {
    return llvh::cast<LiteralBool>(getOperand(NonPointerIdx))->getValue();
  }
};

class FastArrayLoadInst : public Instruction {
 public:
  enum { ArrayIdx, IndexIdx };

  explicit FastArrayLoadInst(Value *array, Value *index, Type checkedType)
      : Instruction(ValueKind::FastArrayLoadInstKind) {
    setType(checkedType);
    pushOperand(array);
    pushOperand(index);
  }
  explicit FastArrayLoadInst(
      const FastArrayLoadInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::FastArrayLoadInstKind;
  }
  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setReadHeap().setThrow();
  }

  Value *getArray() const {
    return getOperand(ArrayIdx);
  }
  Value *getIndex() const {
    return getOperand(IndexIdx);
  }
};

class FastArrayStoreInst : public Instruction {
 public:
  enum { StoredValueIdx, ArrayIdx, IndexIdx };

  explicit FastArrayStoreInst(Value *storedValue, Value *array, Value *index)
      : Instruction(ValueKind::FastArrayStoreInstKind) {
    setType(Type::createNoType());
    pushOperand(storedValue);
    pushOperand(array);
    pushOperand(index);
  }
  explicit FastArrayStoreInst(
      const FastArrayStoreInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::FastArrayStoreInstKind;
  }
  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setWriteHeap().setThrow();
  }

  Value *getStoredValue() const {
    return getOperand(StoredValueIdx);
  }
  Value *getArray() const {
    return getOperand(ArrayIdx);
  }
  Value *getIndex() const {
    return getOperand(IndexIdx);
  }
};

class FastArrayPushInst : public Instruction {
 public:
  enum { PushedValueIdx, ArrayIdx };

  explicit FastArrayPushInst(Value *pushedValue, Value *array)
      : Instruction(ValueKind::FastArrayPushInstKind) {
    setType(Type::createNoType());
    pushOperand(pushedValue);
    pushOperand(array);
  }
  explicit FastArrayPushInst(
      const FastArrayPushInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::FastArrayPushInstKind;
  }
  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setWriteHeap().setThrow();
  }

  Value *getArray() const {
    return getOperand(ArrayIdx);
  }
  Value *getPushedValue() const {
    return getOperand(PushedValueIdx);
  }
};

class FastArrayAppendInst : public Instruction {
 public:
  enum { OtherIdx, ArrayIdx };

  explicit FastArrayAppendInst(Value *other, Value *array)
      : Instruction(ValueKind::FastArrayAppendInstKind) {
    setType(Type::createNoType());
    pushOperand(other);
    pushOperand(array);
  }
  explicit FastArrayAppendInst(
      const FastArrayAppendInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::FastArrayAppendInstKind;
  }
  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setReadHeap().setWriteHeap().setThrow();
  }

  Value *getArray() const {
    return getOperand(ArrayIdx);
  }
  Value *getOther() const {
    return getOperand(OtherIdx);
  }
};

class FastArrayLengthInst : public Instruction {
 public:
  enum { ArrayIdx };

  explicit FastArrayLengthInst(Value *array)
      : Instruction(ValueKind::FastArrayLengthInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(array);
  }
  explicit FastArrayLengthInst(
      const FastArrayLengthInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::FastArrayLengthInstKind;
  }
  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }
  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createUint32();
  }
  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setReadHeap();
  }

  Value *getArray() const {
    return getOperand(ArrayIdx);
  }
};

class TypedLoadParentInst : public Instruction {
  TypedLoadParentInst(const TypedLoadParentInst &) = delete;
  void operator=(const TypedLoadParentInst &) = delete;

 public:
  enum { ObjectIdx };

  explicit TypedLoadParentInst(Value *object)
      : Instruction(ValueKind::TypedLoadParentInstKind) {
    setType(*getInherentTypeImpl());
    pushOperand(object);
  }
  explicit TypedLoadParentInst(
      const TypedLoadParentInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    return Type::createObject();
  }

  Value *getObject() {
    return getOperand(ObjectIdx);
  }

  const Value *getObject() const {
    return getOperand(ObjectIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setReadHeap().setIdempotent();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::TypedLoadParentInstKind;
  }
};

class LoadParentNoTrapsInst : public Instruction {
  LoadParentNoTrapsInst(const LoadParentNoTrapsInst &) = delete;
  void operator=(const LoadParentNoTrapsInst &) = delete;

 public:
  enum { ObjectIdx };

  explicit LoadParentNoTrapsInst(Value *object)
      : Instruction(ValueKind::LoadParentNoTrapsInstKind) {
    assert(
        object->getType().isObjectType() &&
        "object input must be of type object");
    setType(*getInherentTypeImpl());
    pushOperand(object);
  }
  explicit LoadParentNoTrapsInst(
      const LoadParentNoTrapsInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static llvh::Optional<Type> getInherentTypeImpl() {
    // The parent of an object is either another object or null.
    return Type::createObjectOrNull();
  }

  Value *getObject() {
    return getOperand(ObjectIdx);
  }

  const Value *getObject() const {
    return getOperand(ObjectIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setReadHeap().setIdempotent();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LoadParentNoTrapsInstKind;
  }
};

class FUnaryMathInst : public Instruction {
  FUnaryMathInst(const FUnaryMathInst &) = delete;
  void operator=(const FUnaryMathInst &) = delete;

 public:
  enum { ArgIdx };

  explicit FUnaryMathInst(ValueKind kind, Value *arg) : Instruction(kind) {
    assert(arg->getType().isNumberType() && "invalid input FUnaryMathInst");
    setType(Type::createNumber());
    pushOperand(arg);
  }
  explicit FUnaryMathInst(
      const FUnaryMathInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getArg() {
    return getOperand(ArgIdx);
  }
  const Value *getArg() const {
    return getOperand(ArgIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), FUnaryMathInst);
  }
};

class FBinaryMathInst : public Instruction {
  FBinaryMathInst(const FBinaryMathInst &) = delete;
  void operator=(const FBinaryMathInst &) = delete;

 public:
  enum { LeftIdx, RightIdx };

  explicit FBinaryMathInst(ValueKind kind, Value *left, Value *right)
      : Instruction(kind) {
    assert(left->getType().isNumberType() && "invalid input FBinaryMathInst");
    assert(right->getType().isNumberType() && "invalid input FBinaryMathInst");
    setType(Type::createNumber());
    pushOperand(left);
    pushOperand(right);
  }
  explicit FBinaryMathInst(
      const FBinaryMathInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getLeft() {
    return getOperand(LeftIdx);
  }
  const Value *getLeft() const {
    return getOperand(LeftIdx);
  }

  Value *getRight() {
    return getOperand(RightIdx);
  }
  const Value *getRight() const {
    return getOperand(RightIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), FBinaryMathInst);
  }
};

class FCompareInst : public Instruction {
  FCompareInst(const FCompareInst &) = delete;
  void operator=(const FCompareInst &) = delete;

 public:
  enum { LeftIdx, RightIdx };

  explicit FCompareInst(ValueKind kind, Value *left, Value *right)
      : Instruction(kind) {
    assert(HERMES_IR_KIND_IN_CLASS(kind, FCompareInst) && "invalid kind");
    assert(left->getType().isNumberType() && "invalid input FCompareInst");
    assert(right->getType().isNumberType() && "invalid input FCompareInst");
    setType(Type::createBoolean());
    pushOperand(left);
    pushOperand(right);
  }
  explicit FCompareInst(
      const FCompareInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getLeft() {
    return getOperand(LeftIdx);
  }
  const Value *getLeft() const {
    return getOperand(LeftIdx);
  }

  Value *getRight() {
    return getOperand(RightIdx);
  }
  const Value *getRight() const {
    return getOperand(RightIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), FCompareInst);
  }
};

class HBCFCompareBranchInst : public TerminatorInst {
  HBCFCompareBranchInst(const HBCFCompareBranchInst &) = delete;
  void operator=(const HBCFCompareBranchInst &) = delete;

 public:
  enum { LeftHandSideIdx, RightHandSideIdx, TrueBlockIdx, FalseBlockIdx };

  Value *getLeftHandSide() const {
    return getOperand(LeftHandSideIdx);
  }
  Value *getRightHandSide() const {
    return getOperand(RightHandSideIdx);
  }
  BasicBlock *getTrueDest() const {
    return cast<BasicBlock>(getOperand(TrueBlockIdx));
  }
  BasicBlock *getFalseDest() const {
    return cast<BasicBlock>(getOperand(FalseBlockIdx));
  }

  explicit HBCFCompareBranchInst(
      ValueKind kind,
      Value *left,
      Value *right,
      BasicBlock *trueBlock,
      BasicBlock *falseBlock)
      : TerminatorInst(kind) {
    assert(HERMES_IR_KIND_IN_CLASS(kind, HBCFCompareBranchInst));
    pushOperand(left);
    pushOperand(right);
    pushOperand(trueBlock);
    pushOperand(falseBlock);
  }
  explicit HBCFCompareBranchInst(
      const HBCFCompareBranchInst *src,
      llvh::ArrayRef<Value *> operands)
      : TerminatorInst(src, operands) {}

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return true;
  }

  /// Return the ValueKind corresponding to this comparison.
  ValueKind toFCompareValueKind() const {
    return HERMES_IR_OFFSET_TO_KIND(
        FCompareInst,
        HERMES_IR_KIND_TO_OFFSET(HBCFCompareBranchInst, getKind()));
  }

  /// Convert a FCompare ValueKind into a HBCFCompareBranchInst one.
  static ValueKind fromFCompareValueKind(ValueKind kind) {
    int ofs = HERMES_IR_KIND_TO_OFFSET(FCompareInst, kind);
    assert(
        ofs >= 0 && ofs < HERMES_IR_CLASS_LENGTH(HBCFCompareBranchInst) &&
        "Invalid CmpBr ValueKind");
    return HERMES_IR_OFFSET_TO_KIND(HBCFCompareBranchInst, ofs);
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), HBCFCompareBranchInst);
  }

  unsigned getNumSuccessorsImpl() const {
    return 2;
  }
  BasicBlock *getSuccessorImpl(unsigned idx) const {
    assert(idx <= 1 && "FCompareBranchInst only have 2 successors!");
    return idx == 0 ? getTrueDest() : getFalseDest();
  }
  void setSuccessorImpl(unsigned idx, BasicBlock *B) {
    assert(idx <= 1 && "FCompareBranchInst only have 2 successors!");
    setOperand(B, idx + TrueBlockIdx);
  }
};

class StringConcatInst : public Instruction {
  StringConcatInst(const StringConcatInst &) = delete;
  void operator=(const StringConcatInst &) = delete;

 public:
  explicit StringConcatInst(llvh::ArrayRef<Value *> operands)
      : Instruction(ValueKind::StringConcatInstKind) {
    setType(Type::createString());
    assert(!operands.empty() && "no strings to concatenate");
    for (Value *op : operands) {
      assert(op->getType().isStringType() && "invalid input StringConcatInst");
      pushOperand(op);
    }
  }
  explicit StringConcatInst(
      const StringConcatInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::StringConcatInstKind;
  }
};

class HBCStringConcatInst : public Instruction {
  HBCStringConcatInst(const HBCStringConcatInst &) = delete;
  void operator=(const HBCStringConcatInst &) = delete;

 public:
  enum { LeftIdx, RightIdx };

  explicit HBCStringConcatInst(Value *left, Value *right)
      : Instruction(ValueKind::HBCStringConcatInstKind) {
    setType(Type::createString());
    pushOperand(left);
    pushOperand(right);
  }
  explicit HBCStringConcatInst(
      const HBCStringConcatInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getLeft() {
    return getOperand(LeftIdx);
  }
  const Value *getLeft() const {
    return getOperand(LeftIdx);
  }
  Value *getRight() {
    return getOperand(RightIdx);
  }
  const Value *getRight() const {
    return getOperand(RightIdx);
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    return {};
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::HBCStringConcatInstKind;
  }
};

class UnionNarrowTrustedInst : public SingleOperandInst {
  UnionNarrowTrustedInst(const UnionNarrowTrustedInst &) = delete;
  void operator=(const UnionNarrowTrustedInst &) = delete;

  /// The original result type of the instruction, when it was created.
  /// This is used to intersect with the type of the operand during type
  /// inference, and also to avoid corner cases where the type of the operand is
  /// narrowed so that the intersection is empty.
  /// Also see the comment in ThrowIfInst.
  Type savedResultType_;

 public:
  explicit UnionNarrowTrustedInst(Value *src, Type type)
      : SingleOperandInst(ValueKind::UnionNarrowTrustedInstKind, src),
        savedResultType_(type) {
    setType(type);
  }
  explicit UnionNarrowTrustedInst(
      const UnionNarrowTrustedInst *src,
      llvh::ArrayRef<Value *> operands)
      : SingleOperandInst(src, operands),
        savedResultType_(src->savedResultType_) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    // UnionNarrowTrusted participates in TypeInference because its type is
    // based on the input type when possible.
    return false;
  }

  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  SideEffect getSideEffectImpl() const {
    // This instruction is unhoistable. It is preceded by instructions that
    // ensure its own execution invariants, so it would be invalid to lift it
    // before them. For example, in TDZ there will first be a throwing check
    // emitted, followed by this instruction. So this instruction would only
    // valid in the non-throwing case of the preceding instructions, so it
    // cannot get lifted to an earlier point before those assumptions are
    // checked.
    return SideEffect{}.setIdempotent().setUnhoistable();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::UnionNarrowTrustedInstKind;
  }

  /// \return the original result type that was set before TypeInference.
  Type getSavedResultType() const {
    return savedResultType_;
  }
};

class CheckedTypeCastInst : public Instruction {
  CheckedTypeCastInst(const CheckedTypeCastInst &) = delete;
  void operator=(const CheckedTypeCastInst &) = delete;

 public:
  enum { CheckedValueIdx, SpecifiedTypeIdx };

  explicit CheckedTypeCastInst(Value *src, LiteralIRType *type)
      : Instruction(ValueKind::CheckedTypeCastInstKind) {
    setType(type->getData());
    pushOperand(src);
    pushOperand(type);
  }
  explicit CheckedTypeCastInst(
      const CheckedTypeCastInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getCheckedValue() const {
    return getOperand(CheckedValueIdx);
  }

  LiteralIRType *getSpecifiedType() const {
    return llvh::cast<LiteralIRType>(getOperand(SpecifiedTypeIdx));
  }

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setThrow();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::CheckedTypeCastInstKind;
  }
};

class LIRDeadValueInst : public Instruction {
  LIRDeadValueInst(const LIRDeadValueInst &) = delete;
  void operator=(const LIRDeadValueInst &) = delete;

  /// This type is returned during type inference.
  Type savedResultType_;

 public:
  explicit LIRDeadValueInst(Type type)
      : Instruction(ValueKind::LIRDeadValueInstKind), savedResultType_(type) {
    setType(type);
  }
  explicit LIRDeadValueInst(
      const LIRDeadValueInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands), savedResultType_(src->savedResultType_) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return false;
  }

  SideEffect getSideEffectImpl() const {
    return SideEffect::createUnknown();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LIRDeadValueInstKind;
  }

  /// \return the original result type that was set.
  Type getSavedResultType() const {
    return savedResultType_;
  }
};

class NativeCallInst : public Instruction {
  NativeCallInst(const NativeCallInst &) = delete;
  void operator=(const NativeCallInst &) = delete;

 public:
  enum { CalleeIdx, SignatureIdx, FirstArgIdx };

  explicit NativeCallInst(
      Type type,
      Value *callee,
      LiteralNativeSignature *signature,
      llvh::ArrayRef<Value *> args)
      : Instruction(ValueKind::NativeCallInstKind) {
    setType(type);
    // FIXME: this should be added when types are propagated.
    // assert(callee->getType().isNumberType() && "callee must be a number");
    pushOperand(callee);
    pushOperand(signature);
    for (auto arg : args)
      pushOperand(arg);
  }
  explicit NativeCallInst(
      const NativeCallInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    return SideEffect::createUnknown();
  }
  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::NativeCallInstKind;
  }

  Value *getCallee() const {
    return getOperand(CalleeIdx);
  }
  LiteralNativeSignature *getSignature() const {
    return llvh::cast<LiteralNativeSignature>(getOperand(SignatureIdx));
  }
  unsigned getNumArgs() const {
    return getNumOperands() - FirstArgIdx;
  }
  Value *getArg(unsigned i) const {
    assert(i < getNumArgs() && "invalid arg index");
    return getOperand(FirstArgIdx + i);
  }
};

class GetNativeRuntimeInst : public Instruction {
  GetNativeRuntimeInst(const GetNativeRuntimeInst &) = delete;
  void operator=(const GetNativeRuntimeInst &) = delete;

 public:
  explicit GetNativeRuntimeInst()
      : Instruction(ValueKind::GetNativeRuntimeInstKind) {
    setType(Type::createNumber());
  }
  explicit GetNativeRuntimeInst(
      const GetNativeRuntimeInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  static bool hasOutput() {
    return true;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    return SideEffect{}.setIdempotent();
  }
  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::GetNativeRuntimeInstKind;
  }
};

/// Data for LazyCompilationDataInst to use.
class LazyCompilationData {
 public:
  /// The source buffer ID in which we can find the function source.
  uint32_t bufferId;

  /// The FunctionInfo for the function being compiled.
  sema::FunctionInfo *semInfo;

  /// The type of function, e.g. statement or expression.
  ESTree::NodeKind nodeKind;

  /// Whether or not the function is strict.
  bool strictMode;

  /// The Yield param to restore when parsing.
  bool paramYield;

  /// The Await param to restore when parsing.
  bool paramAwait;

  /// If not None, the bytecode function ID that has already been assigned to
  /// the function. Used to avoid assigning new function IDs to lazy Functions
  /// that have already had BytecodeFunctions created.
  OptValue<uint32_t> bcFunctionID{llvh::None};

 public:
  LazyCompilationData(
      uint32_t bufferId,
      sema::FunctionInfo *semInfo,
      ESTree::NodeKind nodeKind,
      bool strictMode,
      bool paramYield,
      bool paramAwait)
      : bufferId(bufferId),
        semInfo(semInfo),
        nodeKind(nodeKind),
        strictMode(strictMode),
        paramYield(paramYield),
        paramAwait(paramAwait) {}
};

class LazyCompilationDataInst : public Instruction {
  LazyCompilationDataInst(const LazyCompilationDataInst &) = delete;
  void operator=(const LazyCompilationDataInst &) = delete;

  LazyCompilationData data_;

 public:
  enum {
    CapturedThisIdx,
    CapturedNewTargetIdx,
    CapturedArgumentsIdx,
    HomeObjectIdx,
    ClsConstructorIdx,
    ClsInstanceElemInitFuncIdx,
    ParentVarScopeIdx
  };

  explicit LazyCompilationDataInst(
      LazyCompilationData &&data,
      Value *capturedThis,
      Value *capturedNewTarget,
      Value *capturedArguments,
      Value *capturedHomeObject,
      Value *clsConstructor,
      Value *clsInstanceElemInitFunc,
      VariableScope *parentVarScope)
      : Instruction(ValueKind::LazyCompilationDataInstKind), data_(data) {
    // Store the captured variables as EmptySentinel if they were null.
    // The getters translate back to nullptr, convenient for the caller to use.
    assert(
        llvh::isa<EmptySentinel>(capturedThis) ||
        llvh::isa<Variable>(capturedThis));
    assert(
        llvh::isa<EmptySentinel>(capturedArguments) ||
        llvh::isa<Variable>(capturedArguments));
    setType(Type::createNoType());
    pushOperand(capturedThis);
    pushOperand(capturedNewTarget);
    pushOperand(capturedArguments);
    pushOperand(capturedHomeObject);
    pushOperand(clsConstructor);
    pushOperand(clsInstanceElemInitFunc);
    // Push all VariableScopes which must be kept alive to properly compile this
    // function.
    // NOTE: LazyCompilationData relies on the fact that we don't delete
    // Variables during lazy compilation. That means no stack
    // promotion, and the existing full optimization pipeline cannot run,
    // because we haven't yet figured out which variables are captured by child
    // functions.
    // Variables themselves are not stored here because we don't delete them
    // between lazy compilation invocations (there's no telling whether they're
    // going to be used or not), instead we just delete them when their owning
    // VariableScopes are destroyed (when they have no users).
    for (VariableScope *cur = parentVarScope; cur; cur = cur->getParentScope())
      pushOperand(cur);
  }
  explicit LazyCompilationDataInst(
      const LazyCompilationDataInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands), data_(src->data_) {}

  const LazyCompilationData &getData() const {
    return data_;
  }
  LazyCompilationData &getData() {
    return data_;
  }
  const VariableScope *getParentVarScope() const {
    return cast<VariableScope>(getOperand(ParentVarScopeIdx));
  }
  VariableScope *getParentVarScope() {
    return cast<VariableScope>(getOperand(ParentVarScopeIdx));
  }

  /// \return the captured \c this value Variable, nullptr if there is none.
  Variable *getCapturedThis() {
    return llvh::dyn_cast<Variable>(getOperand(CapturedThisIdx));
  }
  /// \return the captured \c new.target value, may be literal undefined.
  Value *getCapturedNewTarget() {
    return getOperand(CapturedNewTargetIdx);
  }
  /// \return the captured \c arguments Variable, nullptr if there is none.
  Variable *getCapturedArguments() {
    return llvh::dyn_cast<Variable>(getOperand(CapturedArgumentsIdx));
  }
  /// \return the captured \c homeObject Variable, nullptr if there is none.
  Variable *getHomeObject() {
    return llvh::dyn_cast<Variable>(getOperand(HomeObjectIdx));
  }
  const Variable *getHomeObject() const {
    return llvh::dyn_cast<Variable>(getOperand(HomeObjectIdx));
  }
  /// \return the class context constructor Variable, nullptr if there is none.
  Variable *getClsConstructor() {
    return llvh::dyn_cast<Variable>(getOperand(ClsConstructorIdx));
  }
  /// \return the class context instance elements initializer Variable, nullptr
  /// if there is none.
  Variable *getClsInstaneElemInitFunc() {
    return llvh::dyn_cast<Variable>(getOperand(ClsInstanceElemInitFuncIdx));
  }

  // Optional lazy-compilation variables can be empty.
  bool acceptsEmptyTypeImpl() const {
    return true;
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    // First in block to make it easy to find during compilation.
    return SideEffect::createUnknown().setFirstInBlock();
  }
  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LazyCompilationDataInstKind;
  }
};

/// Data for EvalCompilationDataInst to use.
class EvalCompilationData {
 public:
  /// The FunctionInfo for the function being compiled.
  sema::FunctionInfo *semInfo;

  /// If not None, the bytecode function ID that has already been assigned to
  /// the function. Used to avoid assigning new function IDs to Functions
  /// that have already had BytecodeFunctions created.
  OptValue<uint32_t> bcFunctionID{llvh::None};

 public:
  EvalCompilationData(sema::FunctionInfo *semInfo) : semInfo(semInfo) {}
};

class EvalCompilationDataInst : public Instruction {
  EvalCompilationDataInst(const EvalCompilationDataInst &) = delete;
  void operator=(const EvalCompilationDataInst &) = delete;

  EvalCompilationData data_;

 public:
  enum {
    CapturedThisIdx,
    CapturedNewTargetIdx,
    CapturedArgumentsIdx,
    HomeObjectIdx,
    ClsConstructorIdx,
    ClsInstElemInitFuncIdx,
    FuncVarScopeIdx
  };

  explicit EvalCompilationDataInst(
      EvalCompilationData &&data,
      Value *capturedThis,
      Value *capturedNewTarget,
      Value *capturedArguments,
      Value *homeObject,
      Value *classCtxConstructor,
      Value *classCtxInitFuncVar,
      ArrayRef<VariableScope *> varScopes)
      : Instruction(ValueKind::EvalCompilationDataInstKind), data_(data) {
    assert(
        llvh::isa<EmptySentinel>(capturedThis) ||
        llvh::isa<Variable>(capturedThis));
    assert(
        llvh::isa<EmptySentinel>(capturedArguments) ||
        llvh::isa<Variable>(capturedArguments));
    setType(Type::createNoType());
    pushOperand(capturedThis);
    pushOperand(capturedNewTarget);
    pushOperand(capturedArguments);
    pushOperand(homeObject);
    pushOperand(classCtxConstructor);
    pushOperand(classCtxInitFuncVar);
    // Push all VariableScopes which must be kept alive to properly compile this
    // function.
    // NOTE: EvalCompilationData relies on the fact that we don't delete
    // Variables during eval compilation. That means no stack
    // promotion, and the existing full optimization pipeline cannot run,
    // because we haven't yet figured out which variables are captured by child
    // functions.
    // Variables themselves are not stored here because we don't delete them
    // between eval compilation invocations (there's no telling whether they're
    // going to be used or not), instead we just delete them when their owning
    // VariableScopes are destroyed (when they have no users).
    llvh::DenseSet<VariableScope *> seen;
    for (VariableScope *VS : varScopes) {
      for (VariableScope *cur = VS; cur; cur = cur->getParentScope()) {
        auto [_, inserted] = seen.insert(cur);
        if (!inserted)
          break;
        pushOperand(cur);
      }
    }
  }
  explicit EvalCompilationDataInst(
      const EvalCompilationDataInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands), data_(src->data_) {}

  const EvalCompilationData &getData() const {
    return data_;
  }
  EvalCompilationData &getData() {
    return data_;
  }
  const VariableScope *getFuncVarScope() const {
    return cast<VariableScope>(getOperand(FuncVarScopeIdx));
  }
  VariableScope *getFuncVarScope() {
    return cast<VariableScope>(getOperand(FuncVarScopeIdx));
  }

  /// \return the captured \c this value Variable, nullptr if there is none.
  Variable *getCapturedThis() {
    return llvh::dyn_cast<Variable>(getOperand(CapturedThisIdx));
  }
  /// \return the captured \c new.target value, may be literal undefined.
  Value *getCapturedNewTarget() {
    return getOperand(CapturedNewTargetIdx);
  }
  /// \return the captured \c arguments Variable, nullptr if there is none.
  Variable *getCapturedArguments() {
    return llvh::dyn_cast<Variable>(getOperand(CapturedArgumentsIdx));
  }
  /// \return the captured \c homeObject Variable, nullptr if there is none.
  Variable *getHomeObject() {
    return llvh::dyn_cast<Variable>(getOperand(HomeObjectIdx));
  }
  /// \return the class context constructor Variable, nullptr if there is none.
  Variable *getClsConstructor() {
    return llvh::dyn_cast<Variable>(getOperand(ClsConstructorIdx));
  }
  /// \return the class context instance elements initializer Variable, nullptr
  /// if there is none.
  Variable *getClsInstElemInitFunc() {
    return llvh::dyn_cast<Variable>(getOperand(ClsInstElemInitFuncIdx));
  }

  static bool hasOutput() {
    return false;
  }
  static bool isTyped() {
    return true;
  }
  SideEffect getSideEffectImpl() const {
    // First in block to make it easy to find during compilation.
    return SideEffect::createUnknown().setFirstInBlock();
  }
  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::EvalCompilationDataInstKind;
  }
};

} // end namespace hermes

#endif
