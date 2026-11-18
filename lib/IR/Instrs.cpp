/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "llvh/ADT/Hashing.h"
#include "llvh/ADT/SmallString.h"
#include "llvh/Support/Casting.h"
#include "llvh/Support/ErrorHandling.h"

#include "hermes/IR/CFG.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRVisitor.h"
#include "hermes/IR/Instrs.h"

using namespace hermes;

unsigned TerminatorInst::getNumSuccessors() const {
#undef TERMINATOR
#define TERMINATOR(CLASS, PARENT)           \
  if (auto I = llvh::dyn_cast<CLASS>(this)) \
    return I->getNumSuccessorsImpl();
#define BEGIN_TERMINATOR(NAME, PARENT) TERMINATOR(NAME, PARENT)
#include "hermes/IR/Instrs.def"

  llvm_unreachable("not a terminator?!");
}

BasicBlock *TerminatorInst::getSuccessor(unsigned idx) const {
#undef TERMINATOR
#define TERMINATOR(CLASS, PARENT)           \
  if (auto I = llvh::dyn_cast<CLASS>(this)) \
    return I->getSuccessorImpl(idx);
#define BEGIN_TERMINATOR(NAME, PARENT) TERMINATOR(NAME, PARENT)
#include "hermes/IR/Instrs.def"

  llvm_unreachable("not a terminator?!");
}

void TerminatorInst::setSuccessor(unsigned idx, BasicBlock *B) {
#undef TERMINATOR
#define TERMINATOR(CLASS, PARENT)           \
  if (auto I = llvh::dyn_cast<CLASS>(this)) \
    return I->setSuccessorImpl(idx, B);
#define BEGIN_TERMINATOR(NAME, PARENT) TERMINATOR(NAME, PARENT)
#include "hermes/IR/Instrs.def"

  llvm_unreachable("not a terminator?!");
}

const char *UnaryOperatorInst::opStringRepr[] =
    {"void", "-", "~", "!", "++", "--"};

const char *BinaryOperatorInst::opStringRepr[] = {
    "==", "!=", "===", "!==", "<",  "<=", ">",         ">=",
    "<<", ">>", ">>>", "+",   "-",  "*",  "/",         "%",
    "|",  "^",  "&",   "**",  "in", "in", "instanceof"};

const char *BinaryOperatorInst::assignmentOpStringRepr[] = {
    "",   "",   "",   "",   "",   "",   "",   "",    "<<=", ">>=", ">>>=", "+=",
    "-=", "*=", "/=", "%=", "|=", "^=", "&=", "**=", "",    "",    ""};

ValueKind UnaryOperatorInst::parseOperator(llvh::StringRef op) {
  for (int i = 0; i < HERMES_IR_CLASS_LENGTH(UnaryOperatorInst); ++i) {
    if (op == UnaryOperatorInst::opStringRepr[i])
      return HERMES_IR_OFFSET_TO_KIND(UnaryOperatorInst, i);
  }

  llvm_unreachable("invalid operator string");
}

SideEffect UnaryOperatorInst::getSideEffectImpl() const {
  if (getModule()->getTypeContext().isPrimitive(
          getSingleOperand()->getType())) {
    return SideEffect{}.setIdempotent();
  }

  switch (getKind()) {
    case ValueKind::UnaryVoidInstKind: // void
      return SideEffect{}.setIdempotent();

    default:
      break;
  }

  return SideEffect::createExecute();
}

static ValueKind parseOperator_impl(
    llvh::StringRef op,
    const char **lookup_table) {
  for (int i = 0; i < HERMES_IR_CLASS_LENGTH(BinaryOperatorInst); ++i) {
    if (op == lookup_table[i])
      return HERMES_IR_OFFSET_TO_KIND(BinaryOperatorInst, i);
  }

  llvm_unreachable("invalid operator string");
}

ValueKind BinaryOperatorInst::parseOperator(llvh::StringRef op) {
  return parseOperator_impl(op, opStringRepr);
}

ValueKind BinaryOperatorInst::parseAssignmentOperator(llvh::StringRef op) {
  return parseOperator_impl(op, assignmentOpStringRepr);
}

SideEffect BinaryOperatorInst::getBinarySideEffect(
    TypeContext &tc,
    Type leftTy,
    Type rightTy,
    ValueKind op) {
  switch (op) {
    // The 'in' and 'instanceof' operators may execute arbitrary code, or throw
    // when given primitive types:
    case ValueKind::BinaryInInstKind:
    case ValueKind::BinaryInstanceOfInstKind:
      return SideEffect::createExecute();

    // Checking for a private property cannot execute any JS, but it can throw.
    case ValueKind::BinaryPrivateInInstKind:
      return SideEffect{}.setThrow();

    // Strict equality does not throw or have other side effects (per
    // ES5 11.9.6).
    case ValueKind::BinaryStrictlyNotEqualInstKind:
    case ValueKind::BinaryStrictlyEqualInstKind:
      return SideEffect{}.setIdempotent();

    case ValueKind::BinaryEqualInstKind:
    case ValueKind::BinaryNotEqualInstKind:
    case ValueKind::BinaryLessThanInstKind:
    case ValueKind::BinaryLessThanOrEqualInstKind:
    case ValueKind::BinaryGreaterThanInstKind:
    case ValueKind::BinaryGreaterThanOrEqualInstKind:
      // This instruction does not read/write memory if the LHS and RHS types
      // are known to be primitive.
      if (tc.isPrimitive(leftTy) && tc.isPrimitive(rightTy))
        return SideEffect{}.setIdempotent();
      break;

    case ValueKind::BinaryUnsignedRightShiftInstKind:
    case ValueKind::BinaryDivideInstKind:
    case ValueKind::BinaryModuloInstKind:
    case ValueKind::BinaryExponentiationInstKind:
      // We can only reason about primitive types.
      if (!tc.isPrimitive(leftTy) || !tc.isPrimitive(rightTy))
        break;
      // if either of the operands can be a BigInt, this instruction may throw
      // for one of the following reasons:
      // - BigInt doesn't have unsigned right shift.
      // - BigInt division by zero.
      // - BigInt as exponent can't be negative
      if (tc.canBeBigInt(leftTy) || tc.canBeBigInt(rightTy))
        return SideEffect{}.setThrow();
      // We have primitive operands that are not BigInt.
      return SideEffect{}.setIdempotent();

    case ValueKind::BinaryAddInstKind:
      // We can only reason about primitive types.
      if (!tc.isPrimitive(leftTy) || !tc.isPrimitive(rightTy))
        break;
      // If one of the operands is a string, it is side effect free.
      if (leftTy.isStringType() || rightTy.isStringType())
        return SideEffect{}.setIdempotent();
      [[fallthrough]];

    case ValueKind::BinaryLeftShiftInstKind:
    case ValueKind::BinaryRightShiftInstKind:
    case ValueKind::BinarySubtractInstKind:
    case ValueKind::BinaryMultiplyInstKind:
    case ValueKind::BinaryOrInstKind:
    case ValueKind::BinaryXorInstKind:
    case ValueKind::BinaryAndInstKind:
      // We can only reason about primitive types.
      if (!tc.isPrimitive(leftTy) || !tc.isPrimitive(rightTy))
        break;
      // If both operands are BigInt, it is side effect free.
      if (leftTy.isBigIntType() && rightTy.isBigIntType())
        return SideEffect{}.setIdempotent();
      // However BigInt arithmetic operands don't mix with any other type.
      if (tc.canBeBigInt(leftTy) || tc.canBeBigInt(rightTy))
        return SideEffect{}.setThrow();
      // We have primitive operands that are not BigInt.
      return SideEffect{}.setIdempotent();

    default:
      hermes_fatal("Invalid binary operator");
  }

  // This binary operation may execute arbitrary code.
  return SideEffect::createExecute();
}

SwitchInst::SwitchInst(
    Value *input,
    BasicBlock *defaultBlock,
    llvh::ArrayRef<Literal *> values,
    llvh::ArrayRef<BasicBlock *> blocks)
    : TerminatorInst(ValueKind::SwitchInstKind) {
  pushOperand(input);
  pushOperand(defaultBlock);

  assert(blocks.size() && "Empty switch statement (no cases?)");
  assert(values.size() == blocks.size() && "Block-value pairs mismatch");

  // Push the switch targets.
  for (int i = 0, e = values.size(); i < e; ++i) {
    pushOperand(values[i]);
    pushOperand(blocks[i]);
  }
}

unsigned SwitchInst::getNumCasePair() const {
  // The number of cases is computed as the total number of operands, minus
  // the input value and the default basic block. Take this number and divide
  // it in two, because we are counting pairs.
  return (getNumOperands() - FirstCaseIdx) / 2;
}

std::pair<Literal *, BasicBlock *> SwitchInst::getCasePair(unsigned i) const {
  // The values and labels are twined together. Find the index of the pair
  // that we are fetching and return the two values.
  unsigned base = i * 2 + FirstCaseIdx;
  return std::make_pair(
      cast<Literal>(getOperand(base)), cast<BasicBlock>(getOperand(base + 1)));
}

BasicBlock *SwitchInst::getSuccessorImpl(unsigned idx) const {
  assert(idx < getNumSuccessorsImpl() && "getSuccessor out of bound!");
  if (idx == 0)
    return getDefaultDestination();
  return getCasePair(idx - 1).second;
}

void SwitchInst::setSuccessorImpl(unsigned idx, BasicBlock *B) {
  assert(idx < getNumSuccessorsImpl() && "setSuccessor out of bound!");
  if (idx == 0) {
    setOperand(B, DefaultBlockIdx);
    return;
  }
  setOperand(B, FirstCaseIdx + (idx - 1) * 2 + 1);
}

BasicBlock *SwitchInst::getDefaultDestination() const {
  return cast<BasicBlock>(getOperand(DefaultBlockIdx));
}

Value *SwitchInst::getInputValue() const {
  return getOperand(InputIdx);
}

PhiInst::PhiInst(const ValueListType &values, const BasicBlockListType &blocks)
    : Instruction(ValueKind::PhiInstKind) {
  assert(values.size() == blocks.size() && "Block-value pairs mismatch");
  // Push the incoming values.
  for (int i = 0, e = values.size(); i < e; ++i) {
    pushOperand(values[i]);
    pushOperand(blocks[i]);
  }
  // Result type is computed by the builder after insertion, since
  // recalculateResultType requires getModule() (parent chain).
}

unsigned PhiInst::getNumEntries() const {
  // The PHI operands are just pairs of values and basic blocks.
  return getNumOperands() / 2;
}

/// Returns the index of the first operand for phi entry pair.
static unsigned indexOfPhiEntry(unsigned index) {
  return index * 2;
}

std::pair<Value *, BasicBlock *> PhiInst::getEntry(unsigned i) const {
  return std::make_pair(
      getOperand(indexOfPhiEntry(i)),
      cast<BasicBlock>(getOperand(indexOfPhiEntry(i) + 1)));
}

void PhiInst::updateEntry(unsigned i, Value *val, BasicBlock *BB) {
  setOperand(val, indexOfPhiEntry(i));
  setOperand(BB, indexOfPhiEntry(i) + 1);
  recalculateResultType();
}

void PhiInst::addEntry(Value *val, BasicBlock *BB) {
  pushOperand(val);
  pushOperand(BB);
  setType(getModule()->getTypeContext().unionTy(getType(), val->getType()));
}

void PhiInst::removeEntry(unsigned index) {
  removeEntryHelper(index);
  recalculateResultType();
}

void PhiInst::removeEntry(BasicBlock *BB) {
  bool needRecalc = false;
  unsigned i = 0;
  // For each one of the entries:
  while (i < getNumEntries()) {
    // If this entry is from the BB we want to remove, then remove it.
    if (getEntry(i).second == BB) {
      removeEntryHelper(i);
      needRecalc = true;
      // keep the current iteration index.
      continue;
    }
    // Else, move to the next entry.
    i++;
  }
  if (needRecalc)
    recalculateResultType();
}

void PhiInst::removeEntryHelper(unsigned index) {
  // Remove the pair at the right offset. See calculation of getEntry above.
  unsigned startIdx = indexOfPhiEntry(index);
  // Remove the value:
  removeOperand(startIdx);
  // Remove the basic block. Notice that we use the same index because the
  // list is shifted.
  removeOperand(startIdx);
}

void PhiInst::recalculateResultType() {
  TypeContext &tc = getModule()->getTypeContext();
  Type res = Type::createNoType();
  for (unsigned i = 0, e = getNumEntries(); i != e; ++i) {
    // It's possible the Phi has a nullptr operand,
    // e.g. if the value was erased but this Phi wasn't erased yet.
    // setOperand handles this scenario gracefully, so Phi must as well.
    if (getEntry(i).first) {
      res = tc.unionTy(res, getEntry(i).first->getType());
    }
  }
  setType(res);
}

GetPNamesInst::GetPNamesInst(
    BasicBlock *parent,
    AllocStackInst *iteratorAddr,
    AllocStackInst *baseAddr,
    AllocStackInst *indexAddr,
    AllocStackInst *sizeAddr,
    BasicBlock *onEmpty,
    BasicBlock *onSome)
    : TerminatorInst(ValueKind::GetPNamesInstKind) {
  pushOperand(iteratorAddr);
  pushOperand(baseAddr);
  pushOperand(indexAddr);
  pushOperand(sizeAddr);
  pushOperand(onEmpty);
  pushOperand(onSome);
}

GetNextPNameInst::GetNextPNameInst(
    BasicBlock *parent,
    AllocStackInst *propertyAddr,
    AllocStackInst *baseAddr,
    AllocStackInst *indexAddr,
    AllocStackInst *sizeAddr,
    AllocStackInst *iteratorAddr,
    BasicBlock *onLast,
    BasicBlock *onSome)
    : TerminatorInst(ValueKind::GetNextPNameInstKind) {
  pushOperand(propertyAddr);
  pushOperand(baseAddr);
  pushOperand(indexAddr);
  pushOperand(sizeAddr);
  pushOperand(iteratorAddr);
  pushOperand(onLast);
  pushOperand(onSome);
}

BaseSwitchImmInst::BaseSwitchImmInst(
    ValueKind kind,
    Value *input,
    BasicBlock *defaultBlock,
    LiteralNumber *size)
    : TerminatorInst(kind) {
  pushOperand(input);
  pushOperand(defaultBlock);
  pushOperand(size);
}

UIntSwitchImmInst::UIntSwitchImmInst(
    Value *input,
    BasicBlock *defaultBlock,
    LiteralNumber *minValue,
    LiteralNumber *size,
    const ValueListType &values,
    const BasicBlockListType &blocks)
    : BaseSwitchImmInst(
          ValueKind::UIntSwitchImmInstKind,
          input,
          defaultBlock,
          size) {
  assert(minValue->isUInt32Representible() && "minValue must be uint32_t");
  pushOperand(minValue);
  assert(size->isUInt32Representible() && "size must be uint32_t");
  assert(
      minValue->asUInt32() + size->asUInt32() >= minValue->asUInt32() &&
      "minValue + size must not overflow");

  assert(blocks.size() && "Empty switch statement (no cases?)");
  assert(values.size() == blocks.size() && "Block-value pairs mismatch");

  // Push the switch targets.
  for (size_t i = 0, e = values.size(); i < e; ++i) {
    assert(values[i]->isUInt32Representible() && "case value must be uint32_t");
    pushOperand(values[i]);
    pushOperand(blocks[i]);
  }
}

StringSwitchImmInst::StringSwitchImmInst(
    Value *input,
    BasicBlock *defaultBlock,
    LiteralNumber *size,
    const ValueListType &values,
    const BasicBlockListType &blocks)
    : BaseSwitchImmInst(
          ValueKind::StringSwitchImmInstKind,
          input,
          defaultBlock,
          size) {
  assert(blocks.size() && "Empty switch statement (no cases?)");
  assert(values.size() == blocks.size() && "Block-value pairs mismatch");

  // Push the switch targets.
  for (size_t i = 0, e = values.size(); i < e; ++i) {
    pushOperand(values[i]);
    pushOperand(blocks[i]);
  }
}

BasicBlock *BaseSwitchImmInst::getSuccessorImpl(unsigned idx) const {
  assert(idx < getNumSuccessorsImpl() && "getSuccessor out of bound!");
  if (idx == 0)
    return getDefaultDestination();
  return getSwitchTarget(idx - 1);
}

void BaseSwitchImmInst::setSuccessorImpl(unsigned idx, BasicBlock *B) {
  assert(idx < getNumSuccessorsImpl() && "setSuccessor out of bound!");
  if (idx == 0) {
    setOperand(B, DefaultBlockIdx);
    return;
  }
  setOperand(B, getFirstCaseIdx() + (idx - 1) * 2 + 1);
}

bool Instruction::isIdenticalTo(const Instruction *RHS) const {
  // Check if both instructions have the same kind, type and number of
  // operands. This should filter out most cases.
  if (getKind() != RHS->getKind() || getType() != RHS->getType() ||
      getNumOperands() != RHS->getNumOperands())
    return false;

  // Check operands.
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
    if (getOperand(i) != RHS->getOperand(i))
      return false;

  return true;
}

namespace {
/// Return the hash code of the visited instruction.
class InstructionHashConstructor
    : public InstructionVisitor<InstructionHashConstructor, llvh::hash_code> {
 public:
  /// Return default hash code.
  llvh::hash_code visitInstruction(const Instruction &V) {
    return llvh::hash_code();
  }

  /// IMPLEMENT anything that needs special handling here. visitInstruction will
  /// be called for Instructions we do not know how to handle speically.
};
} // namespace

llvh::hash_code Instruction::getSimpleHashCode() const {
  assert(
      getKind() != ValueKind::PhiInstKind &&
      "Simple hash code doesn't handle Phi's, because they allow loops.");
  llvh::hash_code hc =
      llvh::hash_combine((unsigned)getKind(), getNumOperands());

  // Check operands.
  for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
    hc = llvh::hash_combine(hc, getOperand(i));

  // Hash in any special attributes for an instruction.
  return llvh::hash_combine(hc, InstructionHashConstructor().visit(*this));
}
