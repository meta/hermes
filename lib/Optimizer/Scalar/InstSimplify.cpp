/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// Perform simple peephole optimizations.
///
/// The function \c simplifyInstruction is used to handle the actual
/// optimization of any given instruction.
/// Neither \c simplifyInstruction nor any function it calls should directly
/// erase the instruction they wish to replace - instead, they should be
/// returning the optional Value to InstSimplify::run, which will handle any
/// user replacement and destruction.
/// This allows the core loop to pass the newly simplified instruction to
/// simplifyInstruction again, if it can be further simplified.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "instsimplify"

#include "hermes/IR/Analysis.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/IREval.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Support/Statistic.h"

#include "llvh/ADT/DenseSet.h"

using namespace hermes;

STATISTIC(NumSimp, "Number of instructions simplified");

namespace {

const Type kNullOrUndef = Type::createNullOrUndef();

class InstSimplifyImpl {
 public:
  InstSimplifyImpl(Function *F)
      : F_(F), builder_(F_), typeCtx_(builder_.getTypeContext()) {}

  bool run() {
    bool changed = false;
    IRBuilder::InstructionDestroyer destroyer;

    // For all reachable blocks in the function, in RPO order.
    auto PO = postOrderAnalysis(F_);
    for (BasicBlock *BB : llvh::reverse(PO)) {
      // For all instructions:
      for (auto instIter = BB->begin(), e = BB->end(); instIter != e;) {
        Instruction *II = &*instIter;

        /// When optimizing instructions, the new instruction is inserted after
        /// the instruction to be optimized. This allows the main loop over the
        /// instructions to pick up the new instruction and potentially optimize
        /// it further.
        builder_.setInsertionPointAfter(II);

        auto optNewVal = simplifyInstruction(II);

        // Increment iterator after simplifyInstruction to make sure we step to
        // a newly created instruction.
        ++instIter;
        if (optNewVal.hasValue()) {
          auto newVal = optNewVal.getValue();
          if (!newVal)
            continue;

          changed = true;
          NumSimp++;

          // Instruction changed inplace.
          if (II == newVal)
            continue;

          // We have a better and simpler instruction. Replace the original
          // instruction and mark it for deletion.
          II->replaceAllUsesWith(newVal);
        } else {
          changed = true;
          NumSimp++;
        }
        destroyer.add(II);
      }
    }

    return changed;
  }

 private:
  template <typename T>
  Value *reduceAsNumberLike(T *asNumber) {
    auto *op = asNumber->getSingleOperand();

    // If operand is a literal, try to evaluate ToNumber(operand).
    if (auto *lit = llvh::dyn_cast<Literal>(op)) {
      if (auto *result = evalToNumber(builder_, lit)) {
        return result;
      }
    }

    // No need to convert if the value is a number already.
    if (op->getType().isNumberType()) {
      return op;
    }

    // If it can't be converted, return the original instruction.
    return asNumber;
  }

  Value *reduceAsNumber(AsNumberInst *asNumber) {
    return reduceAsNumberLike(asNumber);
  }

  Value *reduceAsNumeric(AsNumericInst *asNumeric) {
    auto *op = asNumeric->getSingleOperand();
    if (op->getType().isBigIntType()) {
      return op;
    }

    return reduceAsNumberLike(asNumeric);
  }

  Value *reduceAsInt32(AsInt32Inst *asInt32) {
    auto *op = asInt32->getSingleOperand();

    // If operand is a literal, try to evaluate ToNumber(operand).
    if (auto *lit = llvh::dyn_cast<Literal>(op)) {
      if (auto *result = evalToInt32(builder_, lit)) {
        return result;
      }
    }

    // Nothing can be done to simplify, return it as-is.
    return asInt32;
  }

  Value *reduceAsUint32(AsUint32Inst *asUint32) {
    auto *op = asUint32->getSingleOperand();

    // If operand is a literal, try to evaluate ToNumber(operand) then ToUint32.
    if (auto *lit = llvh::dyn_cast<Literal>(op)) {
      if (auto *result = evalToUint32(builder_, lit)) {
        return result;
      }
    }

    // Nothing can be done to simplify, return it as-is.
    return asUint32;
  }

  /// Attempt to simplify \p unary and return a typed instruction if possible.
  /// \return the new instruction, nullptr if it can't be simplified.
  Instruction *simplifyTypedUnaryExpression(UnaryOperatorInst *unary) {
    auto kind = unary->getKind();
    auto *op = unary->getSingleOperand();
    Type t = op->getType();

    if (t.isNumberType()) {
      switch (kind) {
        case ValueKind::UnaryMinusInstKind:
          return builder_.createFUnaryMathInst(ValueKind::FNegateKind, op);
        case ValueKind::UnaryIncInstKind:
          return builder_.createFBinaryMathInst(
              ValueKind::FAddInstKind, op, builder_.getLiteralNumber(1));
        case ValueKind::UnaryDecInstKind:
          return builder_.createFBinaryMathInst(
              ValueKind::FSubtractInstKind, op, builder_.getLiteralNumber(1));
        default:
          break;
      }
    }

    return nullptr;
  }

  Value *simplifyTypeOf(TypeOfInst *typeOf) {
    auto *op = typeOf->getArgument();
    Type t = op->getType();
    const Keywords &kw = builder_.getModule()->getContext().keywords();
    if (t.isNullType() || llvh::isa<GlobalObject>(op)) {
      return builder_.getLiteralString(
          Identifier::getFromPointer(kw.identObject));
    }
    if (t.isNumberType()) {
      return builder_.getLiteralString(
          Identifier::getFromPointer(kw.identNumber));
    }
    if (t.isUndefinedType()) {
      return builder_.getLiteralString(
          Identifier::getFromPointer(kw.identUndefined));
    }
    if (t.isBooleanType()) {
      return builder_.getLiteralString(
          Identifier::getFromPointer(kw.identBoolean));
    }
    if (t.isStringType()) {
      return builder_.getLiteralString(
          Identifier::getFromPointer(kw.identString));
    }
    if (t.isSymbolType()) {
      return builder_.getLiteralString(
          Identifier::getFromPointer(kw.identSymbol));
    }
    // Type is either multiple things or object. We cannot distinguish object
    // from closure yet, so give up.
    return nullptr;
  }

  Value *simplifyUnOp(UnaryOperatorInst *unary) {
    auto kind = unary->getKind();
    auto *op = unary->getSingleOperand();

    // If the operand is a literal, try to evaluate the expression.
    if (auto *lit = llvh::dyn_cast<Literal>(op)) {
      if (auto result = evalUnaryOperator(kind, builder_, lit)) {
        return result;
      }
    }

    // Try to simplify based on type information.
    switch (kind) {
      case ValueKind::UnaryBangInstKind:
        if (typeCtx_.isSubsetOf(op->getType(), kNullOrUndef)) {
          return builder_.getLiteralBool(true);
        }
        break;

      default:
        break;
    }

    if (Instruction *typed = simplifyTypedUnaryExpression(unary))
      return typed;

    return nullptr;
  }

  /// Attempt to simplify \p binary and return a typed instruction if possible.
  /// \return the new instruction, nullptr if it can't be simplified.
  Instruction *simplifyTypedBinaryExpression(BinaryOperatorInst *binary) {
    Value *lhs = binary->getLeftHandSide();
    Value *rhs = binary->getRightHandSide();
    auto kind = binary->getKind();

    bool bothNumber =
        lhs->getType().isNumberType() && rhs->getType().isNumberType();

    if (bothNumber) {
      switch (kind) {
        // Double math.
        case ValueKind::BinaryAddInstKind:
          return builder_.createFBinaryMathInst(
              ValueKind::FAddInstKind, lhs, rhs);
        case ValueKind::BinarySubtractInstKind:
          return builder_.createFBinaryMathInst(
              ValueKind::FSubtractInstKind, lhs, rhs);
        case ValueKind::BinaryMultiplyInstKind:
          return builder_.createFBinaryMathInst(
              ValueKind::FMultiplyInstKind, lhs, rhs);
        case ValueKind::BinaryDivideInstKind:
          return builder_.createFBinaryMathInst(
              ValueKind::FDivideInstKind, lhs, rhs);
        case ValueKind::BinaryModuloInstKind:
          return builder_.createFBinaryMathInst(
              ValueKind::FModuloInstKind, lhs, rhs);

        // Double comparisons.
        case ValueKind::BinaryEqualInstKind:
        case ValueKind::BinaryStrictlyEqualInstKind:
          return builder_.createFCompareInst(
              ValueKind::FEqualInstKind, lhs, rhs);
        case ValueKind::BinaryNotEqualInstKind:
        case ValueKind::BinaryStrictlyNotEqualInstKind:
          return builder_.createFCompareInst(
              ValueKind::FNotEqualInstKind, lhs, rhs);
        case ValueKind::BinaryLessThanInstKind:
          return builder_.createFCompareInst(
              ValueKind::FLessThanInstKind, lhs, rhs);
        case ValueKind::BinaryLessThanOrEqualInstKind:
          return builder_.createFCompareInst(
              ValueKind::FLessThanOrEqualInstKind, lhs, rhs);
        case ValueKind::BinaryGreaterThanInstKind:
          return builder_.createFCompareInst(
              ValueKind::FGreaterThanInstKind, lhs, rhs);
        case ValueKind::BinaryGreaterThanOrEqualInstKind:
          return builder_.createFCompareInst(
              ValueKind::FGreaterThanOrEqualInstKind, lhs, rhs);

        default:
          break;
      }
    }

    bool bothString =
        lhs->getType().isStringType() && rhs->getType().isStringType();

    // Check for operations on two strings.
    if (bothString) {
      switch (kind) {
        case ValueKind::BinaryAddInstKind:
          // Addition of two strings is just concatenation.
          return builder_.createStringConcatInst({lhs, rhs});
        default:
          break;
      }
    }

    return nullptr;
  }

  /// Simplify an equality/inequality comparison between a typeof and a string.
  /// \param strLit the string to compare the typeof result with.
  /// \param typeofInst the typeof instruction to run.
  /// \param invert whether to check for inequality instead of equality.
  /// \return a TypeOfIsInst that replaces the strict equality/inequality.
  Value *simplifyTypeOfCheck(
      LiteralString *strLit,
      TypeOfInst *typeofInst,
      bool invert) {
    TypeOfIsTypes types;
    UniqueString *str = strLit->getValue().getUnderlyingPointer();
    const Keywords &kw = builder_.getModule()->getContext().keywords();
    if (str == kw.identUndefined) {
      types = TypeOfIsTypes{}.withUndefined(true);
    } else if (str == kw.identObject) {
      // TypeOfIs supports null and object separately.
      // typeof null is "object", so we have to put both.
      types = TypeOfIsTypes{}.withNull(true).withObject(true);
    } else if (str == kw.identString) {
      types = TypeOfIsTypes{}.withString(true);
    } else if (str == kw.identSymbol) {
      types = TypeOfIsTypes{}.withSymbol(true);
    } else if (str == kw.identBoolean) {
      types = TypeOfIsTypes{}.withBoolean(true);
    } else if (str == kw.identNumber) {
      types = TypeOfIsTypes{}.withNumber(true);
    } else if (str == kw.identBigint) {
      types = TypeOfIsTypes{}.withBigint(true);
    } else if (str == kw.identFunction) {
      types = TypeOfIsTypes{}.withFunction(true);
    } else {
      // All other strings are not going to be returned by typeof.
      // true if the operator is !==, false if it is ===.
      return builder_.getLiteralBool(invert);
    }

    if (invert) {
      types = types.invert();
    }

    return builder_.createTypeOfIsInst(
        typeofInst->getArgument(), builder_.getLiteralTypeOfIsTypes(types));
  }

  /// Fold a TypeOfIs check for exactly null|undefined, or its inverse, when
  /// the argument's type is known to be entirely inside or entirely outside
  /// null|undefined. Other type sets are not folded.
  Value *simplifyTypeOfIs(TypeOfIsInst *typeOfIs) {
    TypeOfIsTypes nullOrUndef =
        TypeOfIsTypes{}.withNull(true).withUndefined(true);
    TypeOfIsTypes types = typeOfIs->getTypes()->getData();
    bool invert;
    if (types.getRaw() == nullOrUndef.getRaw())
      invert = false;
    else if (types.getRaw() == nullOrUndef.invert().getRaw())
      invert = true;
    else
      return nullptr;

    Type argTy = typeOfIs->getArgument()->getType();
    if (typeCtx_.isSubsetOf(argTy, kNullOrUndef))
      return builder_.getLiteralBool(!invert);
    if (typeCtx_.intersectTy(argTy, kNullOrUndef).isNoType())
      return builder_.getLiteralBool(invert);
    return nullptr;
  }

  /// Simplify a loose equality/inequality comparison where one of the operands
  /// is known to be null or undefined. Such a comparison only checks whether
  /// the other operand is null or undefined (Hermes has no [[IsHTMLDDA]]
  /// objects), which is a TypeOfIs check.
  /// \param invert whether the comparison is != instead of ==.
  /// \return a TypeOfIsInst that replaces the comparison, or nullptr.
  Value *simplifyLooseNullCheck(Value *lhs, Value *rhs, bool invert) {
    Value *other;
    if (typeCtx_.isSubsetOf(rhs->getType(), kNullOrUndef))
      other = lhs;
    else if (typeCtx_.isSubsetOf(lhs->getType(), kNullOrUndef))
      other = rhs;
    else
      return nullptr;

    TypeOfIsTypes types = TypeOfIsTypes{}.withNull(true).withUndefined(true);
    if (invert)
      types = types.invert();
    return builder_.createTypeOfIsInst(
        other, builder_.getLiteralTypeOfIsTypes(types));
  }

  Value *simplifyBinOp(BinaryOperatorInst *binary) {
    const Keywords &kw = builder_.getModule()->getContext().keywords();
    Value *lhs = binary->getLeftHandSide();
    Value *rhs = binary->getRightHandSide();
    auto kind = binary->getKind();

    // Try simplifying without replacing the instruction.
    auto *litLhs = llvh::dyn_cast<Literal>(lhs);
    auto *litRhs = llvh::dyn_cast<Literal>(rhs);
    if (litLhs && litRhs) {
      if (auto result = evalBinaryOperator(kind, builder_, litLhs, litRhs)) {
        return result;
      }
    }

    // In some cases, the instruction can be replaced with a faster version
    // or type information can be used to simplify some non-constant
    // expressions.
    Type leftTy = lhs->getType();
    Type rightTy = rhs->getType();
    const bool primitiveTypes =
        typeCtx_.isPrimitive(leftTy) && typeCtx_.isPrimitive(rightTy);

    // This flag helps to simplify equality comparisons (==, ===, !=, !===).
    // Indicate that the operands are the same value which has a primitive type
    // and always compares equal to itself when the comparison does NOT perform
    // any conversions.
    // NaN is never equal to itself, and comparisons between non-primitive types
    // have side effects and can't be predicted.
    const bool identicalForEquality =
        primitiveTypes && lhs == rhs && notNaN(lhs);

    // This flag helps to simplify relational comparisons (<, <=, >, >=).
    // In additional to the equality checks, it also checks that the operands
    // will not be converted to NaN when the relational comparison invokes
    // toNumeric().
    // We know the identical operand is a primitive, and the only primitive
    // types that can be converted to NaN by toNumeric() are string and
    // undefined. However, if the operands are strings, the comparison is
    // performed before toNumeric(). So the only case that for NaN that remains
    // is "undefined".
    bool identicalForRelational =
        identicalForEquality && !typeCtx_.canBeUndefined(leftTy);

    switch (kind) {
      case ValueKind::BinaryEqualInstKind: // ==
        // Identical operands must be equal.
        if (identicalForEquality) {
          return builder_.getLiteralBool(true);
        }

        // Promote equality to strict equality if we know that the types are
        // identical primitive types.
        if (typeCtx_.isKnownPrimitiveType(leftTy) && rightTy == leftTy) {
          return builder_.createBinaryOperatorInst(
              lhs, rhs, ValueKind::BinaryStrictlyEqualInstKind);
        }

        // ~(null|undefined) == null|undefined is always false.
        if ((typeCtx_.intersectTy(leftTy, kNullOrUndef).isNoType() &&
             typeCtx_.isSubsetOf(rightTy, kNullOrUndef)) ||
            (typeCtx_.intersectTy(rightTy, kNullOrUndef).isNoType() &&
             typeCtx_.isSubsetOf(leftTy, kNullOrUndef))) {
          return builder_.getLiteralBool(false);
        }

        // x == null|undefined only checks whether x is null or undefined.
        if (Value *res = simplifyLooseNullCheck(lhs, rhs, false))
          return res;
        break;

      case ValueKind::BinaryNotEqualInstKind: // !=
        // Identical operands can't be non-equal.
        if (identicalForEquality) {
          return builder_.getLiteralBool(false);
        }

        // Promote inequality to strict inequality if we know that the types are
        // identical primitive types.
        if (typeCtx_.isKnownPrimitiveType(leftTy) && rightTy == leftTy) {
          return builder_.createBinaryOperatorInst(
              lhs, rhs, ValueKind::BinaryStrictlyNotEqualInstKind);
        }

        // ~(null|undefined) != null|undefined is always true.
        if ((typeCtx_.intersectTy(leftTy, kNullOrUndef).isNoType() &&
             typeCtx_.isSubsetOf(rightTy, kNullOrUndef)) ||
            (typeCtx_.intersectTy(rightTy, kNullOrUndef).isNoType() &&
             typeCtx_.isSubsetOf(leftTy, kNullOrUndef))) {
          return builder_.getLiteralBool(true);
        }

        // x != null|undefined only checks whether x is not null or undefined.
        if (Value *res = simplifyLooseNullCheck(lhs, rhs, true))
          return res;
        break;

      case ValueKind::BinaryStrictlyEqualInstKind: // ===
        // Identical operand must be strictly equal.
        if (identicalForEquality) {
          return builder_.getLiteralBool(true);
        }

        // Operands of different types can't be strictly equal.
        if (typeCtx_.intersectTy(leftTy, rightTy).isNoType()) {
          return builder_.getLiteralBool(false);
        }
        break;

      case ValueKind::BinaryStrictlyNotEqualInstKind: // !===
        // Identical operands can't be non-equal.
        if (identicalForEquality) {
          return builder_.getLiteralBool(false);
        }
        // Operands of different types can't be strictly equal.
        if (typeCtx_.intersectTy(leftTy, rightTy).isNoType()) {
          return builder_.getLiteralBool(true);
        }
        break;

      case ValueKind::BinaryLessThanInstKind: // <
        if (identicalForRelational) {
          return builder_.getLiteralBool(false);
        }
        break;

      case ValueKind::BinaryLessThanOrEqualInstKind: // <=
        // Handle comparison to self:
        if (identicalForRelational) {
          return builder_.getLiteralBool(true);
        }
        break;

      case ValueKind::BinaryGreaterThanInstKind: // >
        // Handle comparison to self:
        if (identicalForRelational) {
          return builder_.getLiteralBool(false);
        }
        break;

      case ValueKind::BinaryGreaterThanOrEqualInstKind: // >=
        // Handle comparison to self:
        if (identicalForRelational) {
          return builder_.getLiteralBool(true);
        }
        break;

      case ValueKind::BinaryAddInstKind:
        // Convert ("" + x) or (x + "") as AsString(x).
        if (llvh::isa<LiteralString>(lhs) &&
            cast<LiteralString>(lhs)->getValue().getUnderlyingPointer() ==
                kw.identEmptyString) {
          return builder_.createAddEmptyStringInst(rhs);
        } else if (
            llvh::isa<LiteralString>(rhs) &&
            cast<LiteralString>(rhs)->getValue().getUnderlyingPointer() ==
                kw.identEmptyString) {
          return builder_.createAddEmptyStringInst(lhs);
        }
        break;

      case ValueKind::BinaryOrInstKind: { // |
        // Convert (x | 0) of (0 | x) to AsInt32.
        Value *nonzeroOp = nullptr;
        if (llvh::isa<LiteralNumber>(lhs) &&
            cast<LiteralNumber>(lhs)->getValue() == 0) {
          nonzeroOp = rhs;
        } else if (
            llvh::isa<LiteralNumber>(rhs) &&
            cast<LiteralNumber>(rhs)->getValue() == 0) {
          nonzeroOp = lhs;
        }
        if (nonzeroOp) {
          return reduceAsInt32(builder_.createAsInt32Inst(nonzeroOp));
        }
        break;
      }
      case ValueKind::BinaryUnsignedRightShiftInstKind: { // >>>
        // Convert (x >>> 0) to AsUint32.
        if (llvh::isa<LiteralNumber>(rhs) &&
            llvh::cast<LiteralNumber>(rhs)->getValue() == 0) {
          return reduceAsUint32(builder_.createAsUint32Inst(lhs));
        }
        break;
      }

      default:
        break;
    }

    // If the typeof is one side of an (in)equality and the other side is a
    // string literal, we can use TypeOfIs directly.
    bool isEquality = kind == ValueKind::BinaryEqualInstKind ||
        kind == ValueKind::BinaryStrictlyEqualInstKind;
    bool isInequality = kind == ValueKind::BinaryNotEqualInstKind ||
        kind == ValueKind::BinaryStrictlyNotEqualInstKind;
    if (isEquality || isInequality) {
      if (llvh::isa<TypeOfInst>(lhs) && llvh::isa<LiteralString>(rhs)) {
        return simplifyTypeOfCheck(
            llvh::cast<LiteralString>(rhs),
            llvh::cast<TypeOfInst>(lhs),
            isInequality);
      }
      if (llvh::isa<TypeOfInst>(rhs) && llvh::isa<LiteralString>(lhs)) {
        return simplifyTypeOfCheck(
            llvh::cast<LiteralString>(lhs),
            llvh::cast<TypeOfInst>(rhs),
            isInequality);
      }
    }

    if (Instruction *typed = simplifyTypedBinaryExpression(binary))
      return typed;

    return nullptr;
  }

  Value *simplifyPhiInst(PhiInst *P) {
    // Optimize PHI nodes where all incoming values that are not self-edges are
    // the same, by replacing them with that single source value. Note that Phis
    // that have no inputs, or where all inputs are self-edges, must be dead,
    // and will be left untouched.
    return getSinglePhiValue(P);
  }

  Value *simplifyCondBranchInst(CondBranchInst *CBI) {
    auto *cond = CBI->getCondition();

    // Replace branches where the condition is negation of some expr with the
    // expr itself. Example:
    //
    // %1 = NOT %0
    // COND_BR %1, %BB1, %BB2
    //
    // into:
    //
    // %1 = NOT %0
    // COND_BR %0, %BB2, %BB1
    //
    if (auto *U = llvh::dyn_cast<UnaryOperatorInst>(cond)) {
      if (U->getSideEffect().mayReadOrWorse() &&
          U->getKind() == ValueKind::UnaryBangInstKind) {
        // Strip the negation.
        CBI->setOperand(U->getSingleOperand(), 0);
        // Swap the destination blocks:
        Value *BB1 = CBI->getOperand(1);
        Value *BB2 = CBI->getOperand(2);
        CBI->setOperand(BB1, 2);
        CBI->setOperand(BB2, 1);
        return CBI;
      }
    }
    return nullptr;
  }

  /// \return the one possible return value for the function \p F
  /// for callsite \p callSite, or nullptr if none found.
  Value *getKnownReturnValue(Function *F, CallInst *callSite) {
    llvh::DenseSet<Value *> returnValues;

    for (auto &BB : F->getBasicBlockList()) {
      auto *term = BB.getTerminator();
      auto *ret = llvh::dyn_cast<ReturnInst>(term);
      if (!ret)
        continue;

      returnValues.insert(ret->getValue());
    }

    // Found too many return values, or not enough. Notice that
    // in some cases the function may not return anything and just
    // throw and stay in an infinite loop, and this code is legal.
    if (returnValues.size() != 1)
      return nullptr;

    Value *v = *returnValues.begin();

    // The function always returns a single literal.
    if (llvh::isa<Literal>(v))
      return v;

    // The function returns a parameter, let's inspect the call
    // site and find the value.
    if (auto *LPI = llvh::dyn_cast<LoadParamInst>(v)) {
      uint32_t idx = LPI->getParam()->getIndexInParamList();

      // Returning unpassed parameter results in undef.
      if (idx >= callSite->getNumArguments())
        return builder_.getLiteralUndefined();

      // return the argument that the user passed in (ignore the
      // 'this' argument).
      return callSite->getArgument(idx);
    }

    // Returning some locally computed value.
    return nullptr;
  }

  Value *simplifyCallInst(CallInst *CI) {
    bool changed = false;
    if (Function *F = llvh::dyn_cast<Function>(CI->getTarget())) {
      if (CI->hasUsers()) {
        if (Value *V = getKnownReturnValue(F, CI)) {
          CI->replaceAllUsesWith(V);
          changed = true;
        }
      }

      // Check if the function uses the supplied new.target. Note that we have
      // to leave new.target intact for functions that have restrictions on how
      // they are called, since it is used to generate the runtime error.
      if (!F->getNewTargetParam()->hasUsers() &&
          !llvh::isa<LiteralUndefined>(CI->getNewTarget()) &&
          F->getProhibitInvoke() == Function::ProhibitInvoke::ProhibitNone) {
        // The function does not use the supplied new.target param, replace it
        // with undefined. This has two advantages:
        // 1. It removes a usage of the closure, making it easier to analyze and
        // potentially eliminate in the future.
        // 2. It turns constructor calls into the same form as ordinary calls,
        // potentially allowing for better codegen. This is particularly true
        // for super() calls, where the callee and new.target are not the same.
        CI->setNewTarget(builder_.getLiteralUndefined());
        changed = true;
      }

      // Check if the function uses the supplied scope operand. If it does not,
      // remove it, to eliminate a user of the scope.
      if (!F->getParentScopeParam()->hasUsers() &&
          !llvh::isa<EmptySentinel>(CI->getEnvironment())) {
        CI->setEnvironment(builder_.getEmptySentinel());
        changed = true;
      }
    }

    return changed ? CI : nullptr;
  }

  Value *simplifyGetConstructedObjectInst(GetConstructedObjectInst *GCOI) {
    // If we can statically determine whether the return value of the
    // constructor will be an object, we can eliminate this instruction.
    auto opTy = GCOI->getConstructorReturnValue()->getType();
    if (opTy.isObjectType())
      return GCOI->getConstructorReturnValue();
    // Ideally, all we would need to check is that opTy cannot be an object.
    // However, there might be cases where we have inferred the return type of
    // ConstructorReturnValue, and have not yet inferred that ThisValue is an
    // object. In that case, it would be invalid to replace
    // GetConstructedObject, which is of type object, with ThisValue, which is
    // not of type object.
    if (!typeCtx_.canBeObject(opTy) &&
        GCOI->getThisValue()->getType().isObjectType())
      return GCOI->getThisValue();
    return nullptr;
  }

  Value *simplifyAsNumber(AsNumberInst *asNumber) {
    Value *reduced = reduceAsNumber(asNumber);
    return reduced == asNumber ? nullptr : reduced;
  }

  bool isUnaryIncOrDec(Value *value) {
    if (auto *unOp = llvh::dyn_cast<UnaryOperatorInst>(value)) {
      switch (unOp->getKind()) {
        default:
          break;
        case ValueKind::UnaryIncInstKind:
        case ValueKind::UnaryDecInstKind:
          return true;
      }
    }

    return false;
  }

  Value *simplifyAsNumeric(AsNumericInst *asNumeric) {
    // reduced == asNumeric means that it wasn't possible to optimize asNumeric.
    Value *reduced = asNumeric;

    if (asNumeric->hasOneUser()) {
      if (isUnaryIncOrDec(*asNumeric->users_begin())) {
        // Single-use ToNumeric that feeds Inc/Dec can be optimized out as
        // Inc/Dec will perform ToNumeric internally.
        reduced = asNumeric->getSingleOperand();
      }
    }

    if (reduced == asNumeric) {
      // asNumeric can be optimized out if its (single) argument is provably
      // numeric.
      reduced = reduceAsNumeric(asNumeric);
    }

    return reduced == asNumeric ? nullptr : reduced;
  }

  Value *simplifyAsInt32(AsInt32Inst *asInt32) {
    Value *reduced = reduceAsInt32(asInt32);
    return (reduced == asInt32) ? nullptr : reduced;
  }
  Value *simplifyAsUint32(AsUint32Inst *asUint32) {
    Value *reduced = reduceAsUint32(asUint32);
    return (reduced == asUint32) ? nullptr : reduced;
  }

  /// Math.imul on two literals is a pure 32-bit multiply, so evaluate it
  /// here. Both operands must be literals that evalToInt32 can convert, and
  /// it converts only literals whose ToInt32 runs nothing and cannot throw:
  /// numbers, booleans, null and undefined (not yet strings). It refuses
  /// BigInt, whose ToInt32 throws a TypeError, so that instruction is left
  /// alone to throw at run time. Getting this wrong would be silent -- the
  /// driver destroys the instruction as soon as we hand back a replacement, so
  /// a coercion we skipped is a coercion nobody performs.
  Value *simplifyImul(ImulInst *imul) {
    auto *left = llvh::dyn_cast<Literal>(imul->getLeft());
    auto *right = llvh::dyn_cast<Literal>(imul->getRight());
    if (!left || !right)
      return nullptr;

    LiteralNumber *leftInt = evalToInt32(builder_, left);
    LiteralNumber *rightInt = evalToInt32(builder_, right);
    if (!leftInt || !rightInt)
      return nullptr;

    // Multiply as unsigned to avoid signed overflow UB, then reinterpret the
    // result as signed: that is the product modulo 2^32 that Math.imul is
    // defined to return.
    uint32_t product = static_cast<uint32_t>(leftInt->truncateToInt32()) *
        static_cast<uint32_t>(rightInt->truncateToInt32());
    return builder_.getLiteralNumber(static_cast<int32_t>(product));
  }

  Value *simplifyAddEmptyString(AddEmptyStringInst *AES) {
    auto *op = AES->getSingleOperand();

    // If operand is a literal, try to evaluate ToString(operand).
    if (auto *lit = llvh::dyn_cast<Literal>(op)) {
      if (auto *result = evalToString(builder_, lit)) {
        return result;
      }
    }

    // No need to convert if the value is a string already.
    if (op->getType().isStringType()) {
      return op;
    }

    return nullptr;
  }

  Value *simplifyToPropertyKey(ToPropertyKeyInst *TPK) {
    auto *op = TPK->getSingleOperand();

    // ToPropertyKey is an identity operation for strings and symbols.
    if (op->getType().isStringType() || op->getType().isSymbolType()) {
      return op;
    }

    return nullptr;
  }

  Value *simplifyCoerceThisNS(CoerceThisNSInst *coerce) {
    auto *operand = coerce->getSingleOperand();

    // null or undefined produce global scope.
    if (typeCtx_.isSubsetOf(operand->getType(), kNullOrUndef)) {
      return builder_.getGlobalObject();
    }

    // Objects are unchanged.
    if (operand->getType().isObjectType())
      return operand;

    return nullptr;
  }

  /// Try to simplify ThrowIfInst
  /// \returns one of:
  ///   - nullptr if the instruction cannot be simplified.
  ///   - the instruction itself, if it was changed inplace.
  ///   - a new instruction to replace the original one
  ///   - llvh::None if the instruction should be deleted.
  OptValue<Value *> simplifyThrowIf(ThrowIfInst *TIE) {
    // If the operand does not contain the "poison" type, it can be safely
    // eliminated.
    const Type invalidTypes = TIE->getInvalidTypes()->getData();

    // The subset of invalid types that the operand could actually be.
    Type invalidSubset =
        typeCtx_.intersectTy(TIE->getCheckedValue()->getType(), invalidTypes);
    // If all invalid types are possible, there is nothing we can optimize.
    if (invalidSubset == invalidTypes)
      return nullptr;
    // If the operand does not contain any invalid type, it can be safely
    // eliminated.
    if (invalidSubset.isNoType())
      return TIE->getCheckedValue();
    // Make the throwIf invalid types narrower.
    TIE->setInvalidTypes(builder_.getLiteralIRType(invalidSubset));
    return nullptr;
  }

  /// Try to simplify FUnaryMath
  /// \returns one of:
  ///   - nullptr if the instruction cannot be simplified.
  ///   - the instruction itself, if it was changed inplace.
  ///   - a new instruction to replace the original one
  ///   - llvh::None if the instruction should be deleted.
  OptValue<Value *> simplifyFUnaryMath(FUnaryMathInst *inst) {
    // If the arg is a literal, try to evaluate the expression.
    if (auto *lit = llvh::dyn_cast<LiteralNumber>(inst->getArg())) {
      switch (inst->getKind()) {
        case ValueKind::FNegateKind:
          return builder_.getLiteralNumber(-lit->getValue());
        default:
          break;
      }
    }

    return nullptr;
  }
  /// Try to simplify FBinaryMath
  /// \returns one of:
  ///   - nullptr if the instruction cannot be simplified.
  ///   - the instruction itself, if it was changed inplace.
  ///   - a new instruction to replace the original one
  ///   - llvh::None if the instruction should be deleted.
  OptValue<Value *> simplifyFBinaryMath(FBinaryMathInst *inst) {
    // If the arg is a literal, try to evaluate the expression.
    if (auto *l = llvh::dyn_cast<LiteralNumber>(inst->getLeft())) {
      if (auto *r = llvh::dyn_cast<LiteralNumber>(inst->getRight())) {
        switch (inst->getKind()) {
          case ValueKind::FAddInstKind:
            return builder_.getLiteralNumber(l->getValue() + r->getValue());
          case ValueKind::FSubtractInstKind:
            return builder_.getLiteralNumber(l->getValue() - r->getValue());
          case ValueKind::FMultiplyInstKind:
            return builder_.getLiteralNumber(l->getValue() * r->getValue());
          case ValueKind::FDivideInstKind:
            return builder_.getLiteralNumber(l->getValue() / r->getValue());
          case ValueKind::FModuloInstKind:
            return builder_.getLiteralNumber(
                std::fmod(l->getValue(), r->getValue()));
          default:
            break;
        }
      }
    }

    return nullptr;
  }

  /// Try to simplify FCompare
  /// \returns one of:
  ///   - nullptr if the instruction cannot be simplified.
  ///   - the instruction itself, if it was changed inplace.
  ///   - a new instruction to replace the original one
  ///   - llvh::None if the instruction should be deleted.
  OptValue<Value *> simplifyFCompare(FCompareInst *inst) {
    // If the arg is a literal, try to evaluate the expression.
    if (auto *l = llvh::dyn_cast<LiteralNumber>(inst->getLeft())) {
      if (auto *r = llvh::dyn_cast<LiteralNumber>(inst->getRight())) {
        switch (inst->getKind()) {
          case ValueKind::FEqualInstKind:
            return builder_.getLiteralBool(l->getValue() == r->getValue());
          case ValueKind::FNotEqualInstKind:
            return builder_.getLiteralBool(l->getValue() != r->getValue());
          case ValueKind::FLessThanInstKind:
            return builder_.getLiteralBool(l->getValue() < r->getValue());
          case ValueKind::FLessThanOrEqualInstKind:
            return builder_.getLiteralBool(l->getValue() <= r->getValue());
          case ValueKind::FGreaterThanInstKind:
            return builder_.getLiteralBool(l->getValue() > r->getValue());
          case ValueKind::FGreaterThanOrEqualInstKind:
            return builder_.getLiteralBool(l->getValue() >= r->getValue());
          default:
            break;
        }
      }
    }

    return nullptr;
  }

  /// Try to simplify StringConcat.
  /// Either evaluate the concatenation at compile time, or flatten a
  /// concat of multiple concats into a single StringConcat.
  /// \returns one of:
  ///   - nullptr if the instruction cannot be simplified.
  ///   - the instruction itself, if it was changed inplace.
  ///   - a new instruction to replace the original one
  ///   - llvh::None if the instruction should be deleted.
  OptValue<Value *> simplifyStringConcat(StringConcatInst *inst) {
    // We can simplify the concatenation if:
    // * There are consecutive LiteralString operands.
    // * There are StringConat operands unused outside of this instruction.

    // Eagerly try to simplify, and keep track of whether we changed anything in
    // the 'changed' flag.
    bool changed = false;

    llvh::SmallVector<Value *, 8> newOperands{};
    unsigned i = 0;
    unsigned e = inst->getNumOperands();
    while (i < e) {
      // Check for at least 2 consecutive LiteralStrings that can be joined.
      if (llvh::isa<LiteralString>(inst->getOperand(i)) && i < e - 1 &&
          llvh::isa<LiteralString>(inst->getOperand(i + 1))) {
        llvh::SmallString<256> result;
        // Inner loop appends all consecutive strings into a single literal.
        while (i < e && llvh::isa<LiteralString>(inst->getOperand(i))) {
          result.append(
              llvh::cast<LiteralString>(inst->getOperand(i))->getValue().str());
          ++i;
        }
        newOperands.push_back(builder_.getLiteralString(result.str()));
        // Go to next operand directly.
        changed = true;
        continue;
      }

      // Check for StringConcat that can be flattened because it has one user.
      if (auto *concatOperand =
              llvh::dyn_cast<StringConcatInst>(inst->getOperand(i));
          concatOperand && concatOperand->hasOneUser()) {
        for (unsigned j = 0, f = concatOperand->getNumOperands(); j < f; ++j) {
          newOperands.push_back(concatOperand->getOperand(j));
        }
        ++i;
        changed = true;
        continue;
      }

      // Nothing to do for this operand.
      newOperands.push_back(inst->getOperand(i));
      ++i;
    }

    assert(i == e && "Did not consume all operands.");
    assert(!newOperands.empty() && "Expected at least one operand.");

    if (!changed)
      return nullptr;

    // Only one operand left, no need for a no-op StringConcat.
    // e.g. this is a LiteralString that we've now evaluated at compile time.
    if (newOperands.size() == 1)
      return newOperands.front();

    return builder_.createStringConcatInst(newOperands);
  }

  /// Try to simplify UnionNarrowTrustedInst
  /// \returns one of:
  ///   - nullptr if the instruction cannot be simplified.
  ///   - a new value to replace the original one
  OptValue<Value *> simplifyUnionNarrowTrusted(UnionNarrowTrustedInst *UNT) {
    if (typeCtx_.isSubsetOf(UNT->getSingleOperand()->getType(), UNT->getType()))
      return UNT->getSingleOperand();
    return nullptr;
  }
  /// \returns one of:
  ///   - nullptr if the instruction cannot be simplified.
  ///   - a new value to replace the original one
  OptValue<Value *> simplifyCheckedTypeCast(CheckedTypeCastInst *ctc) {
    Type resType =
        typeCtx_.intersectTy(ctc->getType(), ctc->getCheckedValue()->getType());
    // This is a cast that always fails. Do nothing.
    if (resType.isNoType())
      return nullptr;

    // A widening checked cast is pointless.
    if (typeCtx_.isSubsetOf(ctc->getCheckedValue()->getType(), ctc->getType()))
      return ctc->getCheckedValue();

    // If the result type is wider than necessary, narrow it.
    if (ctc->getType() != resType)
      ctc->setType(resType);

    // Casting a cast is pointless. Use the first cast's operand as operand.
    if (auto *inputCast =
            llvh::dyn_cast<CheckedTypeCastInst>(ctc->getCheckedValue())) {
      ctc->setOperand(inputCast->getCheckedValue(), 0);
    }
    return nullptr;
  }

  OptValue<Value *> simplifyResolveScopeInst(ResolveScopeInst *RSI) {
    auto [inst, varScope] = getResolveScopeStart(
        RSI->getStartScope(), RSI->getStartVarScope(), RSI->getVariableScope());

    // If we got to the target scope, replace this instruction with it.
    if (varScope == RSI->getVariableScope())
      return inst;

    // If we were able to walk some amount up the chain, replace the starting
    // operand with the new one.
    if (varScope != RSI->getStartVarScope()) {
      RSI->setStartScope(varScope, inst);
      return RSI;
    }

    // The resulting scope is the same as the input, nothing changes.
    return nullptr;
  }

  OptValue<Value *> simplifyCreateThisInst(CreateThisInst *CTI) {
    auto *F = llvh::dyn_cast<Function>(CTI->getFunctionCode());
    if (!F)
      return nullptr;
    // If we know we are calling a legacy class constructor, we can replace all
    // the users with undefined.
    if (F->getDefinitionKind() ==
            Function::DefinitionKind::ES6BaseConstructor ||
        F->getDefinitionKind() ==
            Function::DefinitionKind::ES6DerivedConstructor) {
      return builder_.getLiteralUndefined();
    }
    // We know we are calling a constructor that expects to receive a this
    // object allocated by the caller. Emit the instructions to do that
    // directly.
    // Note that CreateThisInst does not currently test for whether the callee
    // can be called as a constructor, so it is semantically equivalent to
    // replace it even in cases where the callee cannot be called as a
    // constructor.
    builder_.setInsertionPoint(CTI);
    const Keywords &kw = builder_.getModule()->getContext().keywords();
    auto *proto = builder_.createLoadPropertyInst(
        CTI->getClosure(),
        builder_.getLiteralString(
            Identifier::getFromPointer(kw.identPrototype)));
    return builder_.createAllocObjectLiteralInst({}, proto);
  }

  /// Try to simplify the instruction \p I.
  /// \returns one of:
  ///   - nullptr if the instruction cannot be simplified.
  ///   - the instruction itself, if it was changed inplace.
  ///   - a new instruction to replace the original one
  ///   - llvh::None if the instruction should be deleted.
  OptValue<Value *> simplifyInstruction(Instruction *I) {
    // Dispatch the different simplification kinds:
    if (llvh::isa<UnaryOperatorInst>(I))
      return simplifyUnOp(llvh::cast<UnaryOperatorInst>(I));
    if (llvh::isa<BinaryOperatorInst>(I))
      return simplifyBinOp(llvh::cast<BinaryOperatorInst>(I));
    if (llvh::isa<FUnaryMathInst>(I))
      return simplifyFUnaryMath(llvh::cast<FUnaryMathInst>(I));
    if (llvh::isa<FBinaryMathInst>(I))
      return simplifyFBinaryMath(llvh::cast<FBinaryMathInst>(I));
    if (llvh::isa<FCompareInst>(I))
      return simplifyFCompare(llvh::cast<FCompareInst>(I));
    if (llvh::isa<StringConcatInst>(I))
      return simplifyStringConcat(llvh::cast<StringConcatInst>(I));
    switch (I->getKind()) {
      case ValueKind::AsNumberInstKind:
        return simplifyAsNumber(cast<AsNumberInst>(I));
      case ValueKind::AsNumericInstKind:
        return simplifyAsNumeric(cast<AsNumericInst>(I));
      case ValueKind::AsInt32InstKind:
        return simplifyAsInt32(cast<AsInt32Inst>(I));
      case ValueKind::AsUint32InstKind:
        return simplifyAsUint32(cast<AsUint32Inst>(I));
      case ValueKind::ImulInstKind:
        return simplifyImul(cast<ImulInst>(I));
      case ValueKind::AddEmptyStringInstKind:
        return simplifyAddEmptyString(cast<AddEmptyStringInst>(I));
      case ValueKind::ToPropertyKeyInstKind:
        return simplifyToPropertyKey(cast<ToPropertyKeyInst>(I));
      case ValueKind::PhiInstKind:
        return simplifyPhiInst(cast<PhiInst>(I));
      case ValueKind::CondBranchInstKind:
        return simplifyCondBranchInst(cast<CondBranchInst>(I));
      case ValueKind::CallInstKind:
        return simplifyCallInst(cast<CallInst>(I));
      case ValueKind::GetConstructedObjectInstKind:
        return simplifyGetConstructedObjectInst(
            cast<GetConstructedObjectInst>(I));
      case ValueKind::CoerceThisNSInstKind:
        return simplifyCoerceThisNS(cast<CoerceThisNSInst>(I));
      case ValueKind::ThrowIfInstKind:
        return simplifyThrowIf(cast<ThrowIfInst>(I));
      case ValueKind::UnionNarrowTrustedInstKind:
        return simplifyUnionNarrowTrusted(cast<UnionNarrowTrustedInst>(I));
      case ValueKind::CheckedTypeCastInstKind:
        return simplifyCheckedTypeCast(cast<CheckedTypeCastInst>(I));
      case ValueKind::ResolveScopeInstKind:
        return simplifyResolveScopeInst(cast<ResolveScopeInst>(I));
      case ValueKind::TypeOfInstKind:
        return simplifyTypeOf(cast<TypeOfInst>(I));
      case ValueKind::TypeOfIsInstKind:
        return simplifyTypeOfIs(cast<TypeOfIsInst>(I));
      case ValueKind::CreateThisInstKind:
        return simplifyCreateThisInst(cast<CreateThisInst>(I));

      default:
        // TODO: handle other kinds of instructions.
        return nullptr;
    }
  }

 private:
  /// \return true if the value is known to not be NaN. Note that it may still
  ///    be convertible to NaN.
  bool notNaN(Value *value) {
    // Only numbers can be NaN.
    if (!typeCtx_.canBeNumber(value->getType())) {
      return true;
    }

    // If it is a literal, we can simply check.
    if (auto *literalNumber = llvh::dyn_cast<LiteralNumber>(value)) {
      return !std::isnan(literalNumber->getValue());
    }

    // The value could be a number, so it could be NaN.
    return false;
  }

  /// The function being optimized.
  Function *F_;

  /// Shared builder used for creating the optimized instructions.
  IRBuilder builder_;

  /// IR type context for the module being optimized.
  TypeContext &typeCtx_;
};

} // namespace

Pass *hermes::createInstSimplify() {
  class InstSimplify : public FunctionPass {
   public:
    explicit InstSimplify() : FunctionPass("InstSimplify") {}
    ~InstSimplify() override = default;

    bool runOnFunction(Function *F) override {
      return InstSimplifyImpl(F).run();
    }
  };
  return new InstSimplify();
}

#undef DEBUG_TYPE
