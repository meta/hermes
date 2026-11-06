/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
///
/// This optimization performs type inference for instructions that do not have
/// isTyped() == true.
/// It infers types for variables and function return types as well,
/// and propagates them through the IR.
///
/// Steps:
/// 1. Partition the functions into groups that use each others variables or
/// call each other. This allows us to process all the instructions that could
/// possibly influence each others' types together. If we were to visit
/// functions one at a time, we might visit the use of a variable prior to its
/// assignment in another function, and be unable to infer its type correctly.
/// 2. Clear the type information from all instructions in the group. This
/// allows us to expand the types to the smallest valid type for each value
/// instead of starting with types that are too loose prior to the pass and not
/// be able to narrow them properly.
/// 3. Infer the type of every instruction, function, and variable in the group.
/// Iterate until no more changes are made.
///
/// The pass never widens types of any value, because it intersects the result
/// types with the types prior to the pass before setting them.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "typeinference"

#include "hermes/Optimizer/Scalar/TypeInference.h"

#include "hermes/IR/Analysis.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Support/Statistic.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/DenseSet.h"
#include "llvh/ADT/EquivalenceClasses.h"
#include "llvh/ADT/MapVector.h"
#include "llvh/ADT/SmallPtrSet.h"
#include "llvh/Support/Debug.h"

using namespace hermes;
using llvh::dbgs;
using llvh::SmallPtrSetImpl;

STATISTIC(NumTI, "Number of instructions type inferred");

namespace {

/// \return if the given \p type is a BigInt|Object, which used to determine if
/// unary/binary operations may have a BigInt result.
static bool isBigIntOrObject(TypeContext &tc, Type type) {
  return tc.canBeBigInt(type) || tc.canBeObject(type);
}

static Type inferUnaryArith(
    TypeContext &tc,
    UnaryOperatorInst *UOI,
    Type numberResultType) {
  Value *op = UOI->getSingleOperand();

  if (op->getType().isNumberType()) {
    return numberResultType;
  }

  if (op->getType().isBigIntType()) {
    return Type::createBigInt();
  }

  Type mayBeBigInt = isBigIntOrObject(tc, op->getType()) ? Type::createBigInt()
                                                         : Type::createNoType();

  // - ?? => Number|?BigInt. BigInt is only possible if op.Type is
  // BigInt|Object.
  return tc.unionTy(numberResultType, mayBeBigInt);
}

static Type inferUnaryArithDefault(TypeContext &tc, UnaryOperatorInst *UOI) {
  // - Number => Number
  // - BigInt => BigInt
  // - ?? => Number|BigInt
  return inferUnaryArith(tc, UOI, Type::createNumber());
}

static Type inferTilde(TypeContext &tc, UnaryOperatorInst *UOI) {
  // ~ Number => Int32
  // ~ BigInt => BigInt
  // ~ ?? => Int32|BigInt
  return inferUnaryArith(tc, UOI, Type::createInt32());
}

/// Try to infer the type of the value that's stored into \p addr. \p addr is
/// either a stack location or a variable.
static Type inferMemoryLocationType(TypeContext &tc, Value *addr) {
  Type T = addr->getType();

  for (auto *U : addr->getUsers()) {
    Value *storedVal = nullptr;

    switch (U->getKind()) {
      case ValueKind::StoreFrameInstKind: {
        auto *SF = cast<StoreFrameInst>(U);
        storedVal = SF->getValue();
        break;
      }

      case ValueKind::StoreStackInstKind: {
        auto *SS = cast<StoreStackInst>(U);
        storedVal = SS->getValue();
        break;
      }

      // Loads do not change the type of the memory location.
      case ValueKind::LoadFrameInstKind:
      case ValueKind::LoadStackInstKind:
        continue;

      default:
        // Other instructions that may write to alloc stack thwart our analysis.
        return Type::createAnyType();
    }

    if (!storedVal)
      continue;

    Type storedType = storedVal->getType();

    T = tc.unionTy(T, storedType);
  }

  return T;
}

/// Collects all of the values that are used by a tree of PHIs, recursively.
/// Inputs are stored into \p inputs. Visited PHIs are stored into \p visited.
static void collectPHIInputs(
    SmallPtrSetImpl<Value *> &visited,
    SmallPtrSetImpl<Value *> &inputs,
    PhiInst *P) {
  // Return if we already visited this node.
  if (!visited.insert(P).second)
    return;

  // For all operands:
  for (unsigned i = 0, e = P->getNumEntries(); i < e; i++) {
    auto E = P->getEntry(i);

    // Recursively inspect PHI node operands, and insert non-phis into the input
    // list.
    if (auto *PN = llvh::dyn_cast<PhiInst>(E.first)) {
      collectPHIInputs(visited, inputs, PN);
    } else {
      inputs.insert(E.first);
    }
  }
}

static Type inferBinaryArith(
    TypeContext &tc,
    BinaryOperatorInst *BOI,
    Type numberType = Type::createNumber()) {
  Type LeftTy = BOI->getLeftHandSide()->getType();
  Type RightTy = BOI->getRightHandSide()->getType();

  // Number - Number => Number
  if (LeftTy.isNumberType() && RightTy.isNumberType()) {
    return numberType;
  }

  // BigInt - BigInt => BigInt
  if (LeftTy.isBigIntType() && RightTy.isBigIntType()) {
    return Type::createBigInt();
  }

  Type mayBeBigInt =
      (isBigIntOrObject(tc, LeftTy) && isBigIntOrObject(tc, RightTy))
      ? Type::createBigInt()
      : Type::createNoType();

  // ?? - ?? => Number|?BigInt. BigInt is only possible if both operands are
  // BigInt|Object due to the no automatic BigInt conversion.
  return tc.unionTy(numberType, mayBeBigInt);
}

static Type inferBinaryBitwise(TypeContext &tc, BinaryOperatorInst *BOI) {
  Type LeftTy = BOI->getLeftHandSide()->getType();
  Type RightTy = BOI->getRightHandSide()->getType();

  Type mayBeBigInt = tc.canBeBigInt(LeftTy) && tc.canBeBigInt(RightTy)
      ? Type::createBigInt()
      : Type::createNoType();

  // ?? - ?? => Int32|?BigInt. BigInt is only possible if both operands can be
  // BigInt due to the no automatic BigInt conversion.
  return tc.unionTy(Type::createInt32(), mayBeBigInt);
}

static Type inferBinaryInst(TypeContext &tc, BinaryOperatorInst *BOI) {
  switch (BOI->getKind()) {
    // The following operations always return a boolean result.
    // They may throw, they may read/write memory, but the result of the
    // operation must be a boolean.
    case ValueKind::BinaryEqualInstKind:
    case ValueKind::BinaryNotEqualInstKind:
    case ValueKind::BinaryStrictlyEqualInstKind:
    case ValueKind::BinaryStrictlyNotEqualInstKind:
    case ValueKind::BinaryLessThanInstKind:
    case ValueKind::BinaryLessThanOrEqualInstKind:
    case ValueKind::BinaryGreaterThanInstKind:
    case ValueKind::BinaryGreaterThanOrEqualInstKind:
    case ValueKind::BinaryInInstKind:
    case ValueKind::BinaryPrivateInInstKind:
    case ValueKind::BinaryInstanceOfInstKind:
      // Notice that the spec says that comparison of NaN should return
      // "Undefined" but all VMs return 'false'. We decided to conform to the
      // current implementation and not to the spec.
      return Type::createBoolean();

    // These arithmetic operations always return a number or bigint:
    // https://262.ecma-international.org/#sec-multiplicative-operators
    case ValueKind::BinaryDivideInstKind:
    case ValueKind::BinaryMultiplyInstKind:
    // https://262.ecma-international.org/#sec-exp-operator
    case ValueKind::BinaryExponentiationInstKind:
    // https://tc39.es/ecma262/#sec-subtraction-operator-minus
    case ValueKind::BinarySubtractInstKind:
    // https://tc39.es/ecma262/#sec-left-shift-operator
    case ValueKind::BinaryLeftShiftInstKind:
    // https://tc39.es/ecma262/#sec-signed-right-shift-operator
    case ValueKind::BinaryRightShiftInstKind:
      return inferBinaryArith(tc, BOI);

    case ValueKind::BinaryModuloInstKind:
      return inferBinaryArith(tc, BOI, Type::createInt32());

    // https://es5.github.io/#x11.7.3
    case ValueKind::BinaryUnsignedRightShiftInstKind:
      return Type::createUint32();

    // The Add operator is special:
    // https://262.ecma-international.org/#sec-addition-operator-plus
    case ValueKind::BinaryAddInstKind: {
      Type LeftTy = BOI->getLeftHandSide()->getType();
      Type RightTy = BOI->getRightHandSide()->getType();
      // String + String -> String. It is enough for one of the operands to be
      // a string to force the result to be a string.
      if (LeftTy.isStringType() || RightTy.isStringType()) {
        return Type::createString();
      }

      // Number + Number -> Number.
      if (LeftTy.isNumberType() && RightTy.isNumberType()) {
        return Type::createNumber();
      }

      // BigInt + BigInt -> BigInt.
      if (LeftTy.isBigIntType() && RightTy.isBigIntType()) {
        return Type::createBigInt();
      }

      // ?BigInt + ?BigInt => ?BigInt. Both operands need to "may be a BigInt"
      // for a possible BigInt result from this operator. This is true because
      // there's no automative BigInt type conversion.
      Type mayBeBigInt =
          (isBigIntOrObject(tc, LeftTy) && isBigIntOrObject(tc, RightTy))
          ? Type::createBigInt()
          : Type::createNoType();

      // handy alias for number|maybe(BigInt).
      Type numeric = tc.unionTy(Type::createNumber(), mayBeBigInt);

      // If both sides of the binary operand are known and both sides are known
      // to be non-string (and can't be converted to strings) then the result
      // must be of a numeric type.
      if (tc.isPrimitive(LeftTy) && tc.isPrimitive(RightTy) &&
          !tc.canBeString(LeftTy) && !tc.canBeString(RightTy)) {
        return numeric;
      }

      // The plus operator always returns a number, bigint, or a string.
      return tc.unionTy(numeric, Type::createString());
    }

    // https://tc39.es/ecma262/#sec-binary-bitwise-operators
    case ValueKind::BinaryAndInstKind:
    case ValueKind::BinaryOrInstKind:
    case ValueKind::BinaryXorInstKind:
      return inferBinaryBitwise(tc, BOI);

    default:
      LLVM_DEBUG(
          dbgs() << "Unknown binary operator in TypeInference: "
                 << BOI->getOperatorStr() << '\n');
      return Type::createAnyType();
  }
}

/// Actual implementation of type inference pass.
/// Contains the ability to infer types per-instruction.
///
/// Prior to inferring the type of the instructions, the result type of
/// each instruction is cleared out (set to "NoType"), and the inference
/// is run from first principles on each of the instructions.
///
/// Each of the "inferXXXInst" functions returns a Type.
/// The only instructions which are allowed to return NoType from their infer
/// implementations are those instructions which have no output.
/// Each of these infer functions for Instructions do NOT themselves have to
/// check if the newly inferred type is _different_ - that will be done
/// by the dispatch function. However, other infer functions that are called
/// directly by \c runOnFunction must return false if they aren't changing the
/// type.
///
/// Importantly, the Phi instruction is handled separately from the usual
/// dispatch mechanism.
class TypeInferenceImpl {
  /// The module being analyzed.
  Module *M_;

  /// Map from various values to their types prior to the pass.
  /// Store types for Instruction, Parameter, Variable.
  /// Store return type for Function.
  llvh::DenseMap<Value *, Type> prePassTypes_{};

  /// IR type context for the module being analyzed. Bound to M_ at
  /// construction.
  TypeContext &typeCtx_;

 public:
  explicit TypeInferenceImpl(Module *M)
      : M_(M), typeCtx_(M->getTypeContext()) {}

  /// Run type inference on every instruction in the module.
  /// \return true when some types were changed.
  bool runOnModule();

 private:
  /// Run type inference on an instruction.
  /// This just does a case-based dispatch.
  /// \return true when another iteration will be required to fully infer the
  ///   type of this instruction (either the type has changed or it hasn't been
  ///   fully resolved yet).
  bool inferInstruction(Instruction *inst) {
    if (inst->isTyped()) {
      // Typed instructions are excluded from TypeInference.
      return false;
    }

    Type originalTy = inst->getType();

    // Handle Phi instructions separately by invoking inferPhi directly.
    // Phi instructions can have a NoType operand (e.g. in a loop) that would
    // be unresolvable due to a cycle if we didn't visit it anyway.
    // If we didn't special-case this, we would have a cycle that would cause
    // an infinite loop due to always returning true from this function (below).
    if (auto *phi = llvh::dyn_cast<PhiInst>(inst)) {
      return inferPhi(phi);
    }

    // If one of the operands hasn't had its type inferred yet,
    // skip it and come back later - we'll be back if anything else gets its
    // type set (which might allow us to infer the type of this instruction).
    for (unsigned int i = 0, e = inst->getNumOperands(); i < e; ++i) {
      Value *operand = inst->getOperand(i);
      if (operand->getType().isNoType()) {
        LLVM_DEBUG(
            dbgs() << llvh::format("Missing type for operand %u of ", i)
                   << inst->getName() << "(" << operand->getKindStr() << ")\n");
        return false;
      }
    }

    Type inferredTy = Type::createAnyType();

    // Attempt inference for the given instruction.
    // It's possible that inference will result in the same type being
    // assigned.
    switch (inst->getKind()) {
#define DEF_VALUE(CLASS, PARENT)                  \
  case ValueKind::CLASS##Kind:                    \
    inferredTy = infer##CLASS(cast<CLASS>(inst)); \
    break;
#define DEF_TAG(NAME, PARENT)                       \
  case ValueKind::NAME##Kind:                       \
    inferredTy = infer##PARENT(cast<PARENT>(inst)); \
    break;
#include "hermes/IR/Instrs.def"
      default:
        llvm_unreachable("Invalid kind");
    }

    // Only return true if the type actually changed.
    inst->setType(inferredTy);
    checkAndSetPrePassType(inst);
    inferredTy = inst->getType();
    bool changed = inferredTy != originalTy;

    // For debugging, only output if things changed.
    if (changed) {
      ++NumTI;
      LLVM_DEBUG(
          dbgs() << "Inferred " << inst->getName() << ": "
                 << typeCtx_.fmt(inst->getType()) << "\n");
    }

    return changed;
  }

  /// Phi instructions are to be treated specially by the inference algorithm,
  /// so we put the logic for handling them directly in this function.
  /// \return true if the type changed or we need another iteration of
  ///   inference.
  bool inferPhi(PhiInst *inst) {
    // Check if the types of all incoming values match and if they do set the
    // value of the PHI to match the incoming values.
    unsigned numEntries = inst->getNumEntries();
    if (numEntries < 1)
      return false;

    llvh::SmallPtrSet<Value *, 8> visited;
    llvh::SmallPtrSet<Value *, 8> values;
    collectPHIInputs(visited, values, inst);

    Type originalTy = inst->getType();

    Type newTy = Type::createNoType();

    // Union all possible incoming values into this phi:
    for (auto *input : values) {
      newTy = typeCtx_.unionTy(input->getType(), newTy);
    }

    inst->setType(newTy);
    return newTy != originalTy;
  }

  /// Run type inference on a the provided set of functions and variables to a
  /// fixed point.
  bool runOnFunctionsAndVars(
      llvh::ArrayRef<Function *> functions,
      llvh::ArrayRef<Variable *> vars);

  Type inferSingleOperandInst(SingleOperandInst *inst) {
    hermes_fatal("This is not a concrete instruction");
  }
  Type inferAddEmptyStringInst(AddEmptyStringInst *inst) {
    return *inst->getInherentType();
  }
  Type inferToPropertyKeyInst(ToPropertyKeyInst *inst) {
    Type t = inst->getSingleOperand()->getType();
    if (t.isSymbolType()) {
      return Type::createSymbol();
    }
    if (typeCtx_.isPrimitive(t) && !typeCtx_.canBeSymbol(t)) {
      return Type::createString();
    }
    return Type::createStringOrSymbol();
  }
  Type inferCreatePrivateNameInst(CreatePrivateNameInst *inst) {
    return *inst->getInherentType();
  }
  Type inferAsNumberInst(AsNumberInst *inst) {
    return *inst->getInherentType();
  }
  Type inferAsNumericInst(AsNumericInst *inst) {
    if (inst->getSingleOperand()->getType().isNumberType()) {
      return Type::createNumber();
    }
    if (inst->getSingleOperand()->getType().isBigIntType()) {
      return Type::createBigInt();
    }
    return Type::createNumeric();
  }
  Type inferAsInt32Inst(AsInt32Inst *inst) {
    return *inst->getInherentType();
  }
  Type inferAsUint32Inst(AsUint32Inst *inst) {
    return *inst->getInherentType();
  }
  Type inferLoadStackInst(LoadStackInst *inst) {
    return inst->getSingleOperand()->getType();
  }
  Type inferMovInst(MovInst *inst) {
    Type srcType = inst->getSingleOperand()->getType();
    return srcType;
  }
  Type inferImplicitMovInst(ImplicitMovInst *inst) {
    Type srcType = inst->getSingleOperand()->getType();
    return srcType;
  }
  Type inferCoerceThisNSInst(CoerceThisNSInst *inst) {
    return *inst->getInherentType();
  }
  Type inferUnaryOperatorInst(UnaryOperatorInst *inst) {
    switch (inst->getKind()) {
      case ValueKind::UnaryVoidInstKind: // void
        return Type::createUndefined();
      // https://tc39.es/ecma262/#sec-prefix-increment-operator
      // https://tc39.es/ecma262/#sec-postfix-increment-operator
      case ValueKind::UnaryIncInstKind: // ++
      // https://tc39.es/ecma262/#sec-prefix-decrement-operator
      // https://tc39.es/ecma262/#sec-postfix-decrement-operator
      case ValueKind::UnaryDecInstKind: // --
      // https://tc39.es/ecma262/#sec-unary-minus-operator
      case ValueKind::UnaryMinusInstKind: // -
        return inferUnaryArithDefault(typeCtx_, inst);
      // https://tc39.es/ecma262/#sec-bitwise-not-operator
      case ValueKind::UnaryTildeInstKind: // ~
        return inferTilde(typeCtx_, inst);
      case ValueKind::UnaryBangInstKind: // !
        return Type::createBoolean();
      default:
        hermes_fatal("Invalid unary operator");
    }
  }
  Type inferDirectEvalInst(DirectEvalInst *inst) {
    return Type::createAnyType();
  }
  Type inferDeclareGlobalVarInst(DeclareGlobalVarInst *inst) {
    return Type::createNoType();
  }
  Type inferLoadFrameInst(LoadFrameInst *inst) {
    Type T = inst->getLoadVariable()->getType();
    return T;
  }
  Type inferLIRLoadConstInst(LIRLoadConstInst *inst) {
    return inst->getSingleOperand()->getType();
  }
  Type inferLoadParamInst(LoadParamInst *inst) {
    // Return the type that has been inferred for the parameter.
    return inst->getParam()->getType();
  }
  Type inferHBCResolveParentEnvironmentInst(
      HBCResolveParentEnvironmentInst *inst) {
    return *inst->getInherentType();
  }
  Type inferLIRGetArgumentsLengthInst(LIRGetArgumentsLengthInst *inst) {
    return Type::createAnyType();
  }
  Type inferHBCReifyArgumentsInst(HBCReifyArgumentsInst *inst) {
    hermes_fatal("This is not a concrete instruction");
  }
  Type inferLIRReifyArgumentsLooseInst(LIRReifyArgumentsLooseInst *inst) {
    // Does not return a value, uses a lazy register instead.
    return Type::createNoType();
  }
  Type inferLIRReifyArgumentsStrictInst(LIRReifyArgumentsStrictInst *inst) {
    // Does not return a value, uses a lazy register instead.
    return Type::createNoType();
  }
  Type inferLIRSpillMovInst(LIRSpillMovInst *inst) {
    return inst->getSingleOperand()->getType();
  }

  Type inferPhiInst(PhiInst *inst) {
    // This is a dummy function that shouldn't be called.
    hermes_fatal("Phis are to be handled specially by inferPhi()");
  }
  Type inferBinaryOperatorInst(BinaryOperatorInst *inst) {
    return inferBinaryInst(typeCtx_, inst);
  }
  Type inferImulInst(ImulInst *inst) {
    // Math.imul(left, right) always produces an Int32 number, or throws.
    // Unlike the bitwise ops, it never produces BigInt: BigInt operands
    // make ToInt32 throw a TypeError instead of coercing.
    return Type::createInt32();
  }
  Type inferStorePropertyWithReceiverInst(StorePropertyWithReceiverInst *inst) {
    return Type::createNoType();
  }
  Type inferStorePropertyLooseInst(StorePropertyLooseInst *inst) {
    return Type::createNoType();
  }
  Type inferStorePropertyStrictInst(StorePropertyStrictInst *inst) {
    return Type::createNoType();
  }
  Type inferTryStoreGlobalPropertyLooseInst(
      TryStoreGlobalPropertyLooseInst *inst) {
    return Type::createNoType();
  }
  Type inferTryStoreGlobalPropertyStrictInst(
      TryStoreGlobalPropertyStrictInst *inst) {
    return Type::createNoType();
  }

  Type inferDefineOwnPropertyInst(DefineOwnPropertyInst *inst) {
    return Type::createNoType();
  }
  Type inferDefineOwnInDenseArrayInst(DefineOwnInDenseArrayInst *inst) {
    return Type::createNoType();
  }
  Type inferStoreOwnPrivateFieldInst(StoreOwnPrivateFieldInst *inst) {
    return Type::createNoType();
  }
  Type inferAddOwnPrivateFieldInst(AddOwnPrivateFieldInst *inst) {
    return Type::createNoType();
  }
  Type inferPrivateBrandCheckInst(PrivateBrandCheckInst *inst) {
    return Type::createNoType();
  }

  Type inferDefineOwnGetterSetterInst(DefineOwnGetterSetterInst *inst) {
    return Type::createNoType();
  }
  Type inferDeletePropertyLooseInst(DeletePropertyLooseInst *inst) {
    return *inst->getInherentType();
  }
  Type inferDeletePropertyStrictInst(DeletePropertyStrictInst *inst) {
    return *inst->getInherentType();
  }
  Type inferLoadPropertyInst(LoadPropertyInst *inst) {
    return Type::createAnyType();
  }
  Type inferLoadPropertyWithReceiverInst(LoadPropertyWithReceiverInst *inst) {
    return Type::createAnyType();
  }
  Type inferLoadOwnPrivateFieldInst(LoadOwnPrivateFieldInst *inst) {
    return Type::createAnyType();
  }
  Type inferTryLoadGlobalPropertyInst(TryLoadGlobalPropertyInst *inst) {
    return Type::createAnyType();
  }
  Type inferStoreStackInst(StoreStackInst *inst) {
    return Type::createNoType();
  }
  Type inferStoreFrameInst(StoreFrameInst *inst) {
    return Type::createNoType();
  }
  Type inferAllocStackInst(AllocStackInst *inst) {
    // AllocStackInst is an exceptional case, since as a convenience we have
    // decided that it assumes the type of the allocated value (instead if
    // "pointer to the type of the allocated value"). So, if it is never used,
    // we can't infer anything, ending up with "notype". But we can't allow an
    // instruction with an output to have type "notype". So, if there are no
    // users, just assume the type is "any" as a convenience.
    return inst->hasUsers() ? inferMemoryLocationType(typeCtx_, inst)
                            : Type::createAnyType();
  }
  Type inferAllocArrayInst(AllocArrayInst *inst) {
    return *inst->getInherentType();
  }
  Type inferAllocFastArrayInst(AllocFastArrayInst *inst) {
    return *inst->getInherentType();
  }
  Type inferGetTemplateObjectInst(GetTemplateObjectInst *inst) {
    return *inst->getInherentType();
  }
  Type inferAllocObjectLiteralInst(AllocObjectLiteralInst *inst) {
    return *inst->getInherentType();
  }
  Type inferAllocTypedObjectInst(AllocTypedObjectInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferAllocTypedNonEnumObjectInst(AllocTypedNonEnumObjectInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferCreateArgumentsLooseInst(CreateArgumentsLooseInst *inst) {
    return *inst->getInherentType();
  }
  Type inferCreateArgumentsStrictInst(CreateArgumentsStrictInst *inst) {
    return *inst->getInherentType();
  }
  Type inferCatchInst(CatchInst *inst) {
    return Type::createAnyType();
  }
  Type inferDebuggerInst(DebuggerInst *inst) {
    return Type::createNoType();
  }
  Type inferCreateRegExpInst(CreateRegExpInst *inst) {
    return *inst->getInherentType();
  }
  Type inferTryEndInst(TryEndInst *inst) {
    return Type::createNoType();
  }
  Type inferBranchIfBuiltinInst(BranchIfBuiltinInst *inst) {
    return Type::createNoType();
  }
  Type inferGetNewTargetInst(GetNewTargetInst *inst) {
    return inst->getOperand(GetNewTargetInst::GetNewTargetParamIdx)->getType();
  }
  Type inferTypeOfInst(TypeOfInst *inst) {
    return *inst->getInherentType();
  }
  Type inferTypeOfIsInst(TypeOfIsInst *inst) {
    return *inst->getInherentType();
  }
  Type inferThrowIfInst(ThrowIfInst *inst) {
    Type type = inst->getCheckedValue()->getType();
    assert(!type.isNoType() && "input to throwIfEmpty cannot be NoType");

    if (LLVM_UNLIKELY(type.isEmptyType() || type.isUninitType())) {
      // We remove "Empty" and "Uninit" from the possible types of inst,
      // which would result in a "NoType" (i.e. TDZ is always going to throw),
      // so instead we use the last "valid" type we know about.
      // While this might seem "hacky", it eliminates a lot of complexity that
      // would result from having to deal with unreachable code expressed via
      // the type system (T134361858).
      return inst->getSavedResultType();
    }

    return typeCtx_.subtractTy(type, Type::createEmpty());
  }
  Type inferThrowIfThisInitializedInst(ThrowIfThisInitializedInst *inst) {
    return Type::createNoType();
  }
  Type inferIteratorBeginInst(IteratorBeginInst *inst) {
    return Type::createAnyType();
  }
  Type inferIteratorNextInst(IteratorNextInst *inst) {
    return Type::createAnyType();
  }
  Type inferIteratorCloseInst(IteratorCloseInst *inst) {
    return Type::createAnyType();
  }
  Type inferUnreachableInst(UnreachableInst *inst) {
    return Type::createNoType();
  }

  Type inferCreateFunctionInst(CreateFunctionInst *inst) {
    return *inst->getInherentType();
  }
  Type inferCreateGeneratorInst(CreateGeneratorInst *inst) {
    return *inst->getInherentType();
  }

  Type inferCreateClassInst(CreateClassInst *inst) {
    return *inst->getInherentType();
  }

  Type inferTerminatorInst(TerminatorInst *inst) {
    hermes_fatal("This is not a concrete instruction");
  }
  Type inferBranchInst(BranchInst *inst) {
    return Type::createNoType();
  }
  Type inferReturnInst(ReturnInst *inst) {
    return Type::createNoType();
  }
  Type inferThrowInst(ThrowInst *inst) {
    return Type::createNoType();
  }
  Type inferThrowTypeErrorInst(ThrowTypeErrorInst *inst) {
    return Type::createNoType();
  }
  Type inferSwitchInst(SwitchInst *inst) {
    return Type::createNoType();
  }
  Type inferCondBranchInst(CondBranchInst *inst) {
    return Type::createNoType();
  }
  Type inferGetPNamesInst(GetPNamesInst *inst) {
    return Type::createNoType();
  }
  Type inferGetNextPNameInst(GetNextPNameInst *inst) {
    return Type::createNoType();
  }
  Type inferTryStartInst(TryStartInst *inst) {
    return Type::createNoType();
  }
  Type inferHBCCompareBranchInst(HBCCompareBranchInst *inst) {
    return Type::createNoType();
  }
  Type inferHBCCmpBrTypeOfIsInst(HBCCmpBrTypeOfIsInst *inst) {
    return Type::createNoType();
  }
  Type inferSwitchImmInst(BaseSwitchImmInst *inst) {
    hermes_fatal("This is not a concrete instruction");
  }
  Type inferUIntSwitchImmInst(UIntSwitchImmInst *inst) {
    return Type::createNoType();
  }
  Type inferStringSwitchImmInst(StringSwitchImmInst *inst) {
    return Type::createNoType();
  }
  Type inferSaveAndYieldInst(SaveAndYieldInst *inst) {
    return Type::createNoType();
  }

  Type inferCallInst(CallInst *inst) {
    // If the target of this call is known, propagate its return type.
    if (auto *F = llvh::dyn_cast<Function>(inst->getTarget()))
      return F->getReturnType();
    return Type::createAnyType();
  }
  Type inferHBCCallWithArgCountInst(HBCCallWithArgCountInst *inst) {
    // If the target of this call is known, propagate its return type.
    if (auto *F = llvh::dyn_cast<Function>(inst->getTarget()))
      return F->getType();
    return Type::createAnyType();
  }
  Type inferCallBuiltinInst(CallBuiltinInst *inst) {
    switch (inst->getBuiltinIndex()) {
      case BuiltinMethod::Math_abs:
      case BuiltinMethod::Math_acos:
      case BuiltinMethod::Math_acosh:
      case BuiltinMethod::Math_asin:
      case BuiltinMethod::Math_asinh:
      case BuiltinMethod::Math_atan:
      case BuiltinMethod::Math_atanh:
      case BuiltinMethod::Math_atan2:
      case BuiltinMethod::Math_cbrt:
      case BuiltinMethod::Math_ceil:
      case BuiltinMethod::Math_cos:
      case BuiltinMethod::Math_cosh:
      case BuiltinMethod::Math_exp:
      case BuiltinMethod::Math_expm1:
      case BuiltinMethod::Math_floor:
      case BuiltinMethod::Math_hypot:
      case BuiltinMethod::Math_imul:
      case BuiltinMethod::Math_log:
      case BuiltinMethod::Math_log10:
      case BuiltinMethod::Math_log1p:
      case BuiltinMethod::Math_log2:
      case BuiltinMethod::Math_max:
      case BuiltinMethod::Math_min:
      case BuiltinMethod::Math_pow:
      case BuiltinMethod::Math_round:
      case BuiltinMethod::Math_sin:
      case BuiltinMethod::Math_sinh:
      case BuiltinMethod::Math_sqrt:
      case BuiltinMethod::Math_tan:
      case BuiltinMethod::Math_tanh:
      case BuiltinMethod::Math_trunc:
        return Type::createNumber();
      case BuiltinMethod::HermesBuiltin_objectKeys:
      case BuiltinMethod::HermesBuiltin_objectValues:
      case BuiltinMethod::HermesBuiltin_objectEntries:
        return Type::createObject();
      default:
        return Type::createAnyType();
    }
  }
  Type inferHBCCallNInst(HBCCallNInst *inst) {
    // unimplemented
    return Type::createAnyType();
  }

  Type inferGetBuiltinClosureInst(GetBuiltinClosureInst *inst) {
    return *inst->getInherentType();
  }
  Type inferResumeGeneratorInst(ResumeGeneratorInst *inst) {
    // Result of ResumeGeneratorInst is whatever the user passes to .next()
    // or .throw() to resume the generator, which we don't yet support
    // understanding the types of.
    return Type::createAnyType();
  }

  Type inferGetParentScopeInst(GetParentScopeInst *inst) {
    return *inst->getInherentType();
  }
  Type inferCreateScopeInst(CreateScopeInst *inst) {
    return *inst->getInherentType();
  }
  Type inferResolveScopeInst(ResolveScopeInst *inst) {
    return *inst->getInherentType();
  }
  Type inferLIRResolveScopeInst(LIRResolveScopeInst *inst) {
    return *inst->getInherentType();
  }
  Type inferGetClosureScopeInst(GetClosureScopeInst *inst) {
    return *inst->getInherentType();
  }

  // These are target dependent instructions:

  Type inferLIRGetGlobalObjectInst(LIRGetGlobalObjectInst *inst) {
    return *inst->getInherentType();
  }
  Type inferHBCCreateFunctionEnvironmentInst(
      HBCCreateFunctionEnvironmentInst *inst) {
    return *inst->getInherentType();
  }
  Type inferLIRGetThisNSInst(LIRGetThisNSInst *inst) {
    return *inst->getInherentType();
  }
  Type inferCreateThisInst(CreateThisInst *inst) {
    return Type::createObjectOrUndef();
  }
  Type inferHBCGetArgumentsPropByValInst(HBCGetArgumentsPropByValInst *inst) {
    hermes_fatal("This is not a concrete instruction");
  }
  Type inferLIRGetArgumentsPropByValLooseInst(
      LIRGetArgumentsPropByValLooseInst *inst) {
    return Type::createAnyType();
  }
  Type inferLIRGetArgumentsPropByValStrictInst(
      LIRGetArgumentsPropByValStrictInst *inst) {
    return Type::createAnyType();
  }
  Type inferGetConstructedObjectInst(GetConstructedObjectInst *inst) {
    return *inst->getInherentType();
  }
  Type inferLIRAllocObjectFromBufferInst(LIRAllocObjectFromBufferInst *inst) {
    return *inst->getInherentType();
  }
  Type inferLIRAllocTypedObjectFromBufferInst(
      LIRAllocTypedObjectFromBufferInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferLIRAllocTypedNonEnumObjectFromBufferInst(
      LIRAllocTypedNonEnumObjectFromBufferInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferHBCProfilePointInst(HBCProfilePointInst *inst) {
    return Type::createNoType();
  }
  Type inferLoadParentNoTrapsInst(LoadParentNoTrapsInst *inst) {
    return *inst->getInherentType();
  }
  Type inferPrLoadInst(PrLoadInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferPrStoreInst(PrStoreInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferFastArrayLoadInst(FastArrayLoadInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferFastArrayStoreInst(FastArrayStoreInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferFastArrayPushInst(FastArrayPushInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferFastArrayAppendInst(FastArrayAppendInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferFastArrayLengthInst(FastArrayLengthInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferTypedLoadParentInst(TypedLoadParentInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferNativeCallInst(NativeCallInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferGetNativeRuntimeInst(GetNativeRuntimeInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferFUnaryMathInst(FUnaryMathInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferFBinaryMathInst(FBinaryMathInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferFCompareInst(FCompareInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferHBCFCompareBranchInst(HBCFCompareBranchInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferStringConcatInst(StringConcatInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferHBCStringConcatInst(HBCStringConcatInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferLazyCompilationDataInst(LazyCompilationDataInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferEvalCompilationDataInst(EvalCompilationDataInst *inst) {
    hermes_fatal("typed instruction");
  }
  Type inferUnionNarrowTrustedInst(UnionNarrowTrustedInst *inst) {
    auto res = typeCtx_.intersectTy(
        inst->getSavedResultType(), inst->getSingleOperand()->getType());

    // It may be possible that the input value would be proven to always be
    // empty and this code is unreachable. Similarly to ThrowIf, in that
    // case we simply return something to avoid breaking invariants.
    if (LLVM_UNLIKELY(res.isNoType())) {
      return inst->getSavedResultType();
    }
    return res;
  }
  Type inferCheckedTypeCastInst(CheckedTypeCastInst *inst) {
    Type inputType = inst->getCheckedValue()->getType();
    assert(
        !inputType.isNoType() && "input to CheckedTypeCast cannot be NoType");

    Type resultType =
        typeCtx_.intersectTy(inst->getSpecifiedType()->getData(), inputType);

    if (LLVM_UNLIKELY(resultType.isNoType())) {
      // Some checked casts can be proven at compile time to always throw. In
      // that case, the result type of CheckedTypeCastInst would theoretically
      // need to be something that represents an "unreachable" type. Handling
      // this "unreachable" type everywhere would require a lot of complexity.
      // Instead, when we get to that point, we simply return the known type.
      return inst->getSpecifiedType()->getData();
    }

    return resultType;
  }
  Type inferLIRDeadValueInst(LIRDeadValueInst *inst) {
    return inst->getSavedResultType();
  }

  /// If all call sites of this Function are known, propagate
  /// information from actuals to formals.
  bool inferParams(Function *F) {
    bool changed = false;
    if (!F->allCallsitesKnown()) {
      LLVM_DEBUG(
          dbgs() << F->getInternalName().str() << " has unknown call sites.\n");
      // If there are unknown call sites, we can't infer anything about the
      // parameters.
      for (auto *param : F->getJSDynamicParams()) {
        Type originalTy = param->getType();
        param->setType(Type::createAnyType());
        checkAndSetPrePassType(param);
        changed |= originalTy != param->getType();
      }
      return changed;
    }
    auto callsites = getKnownCallsites(F);
    LLVM_DEBUG(
        dbgs() << F->getInternalName().str() << " has " << callsites.size()
               << " call sites.\n");
    return propagateArgs(callsites, F);
  }

  /// Propagate type information from call sites of F to formals of F.
  /// This assumes that all call sites of F are known.
  /// Cannot narrow the type of any parameter from its type prior to this
  /// function being called.
  bool propagateArgs(llvh::ArrayRef<BaseCallInst *> callSites, Function *F) {
    bool changed = false;
    // Hermes does not support using 'arguments' to modify the arguments to a
    // function in loose mode. Therefore, we can safely propagate the parameter
    // types to their usage regardless of the function's strictness.
    IRBuilder builder(F);
    for (uint32_t i = 0, e = F->getJSDynamicParams().size(); i < e; ++i) {
      auto *P = F->getJSDynamicParam(i);
      Type originalTy = P->getType();
      Type paramTy = originalTy;

      // For each call sites.
      for (auto *call : callSites) {
        // The argument default value is undefined.
        Value *arg = builder.getLiteralUndefined();

        // Load the argument that's passed in.
        if (i < call->getNumArguments()) {
          arg = call->getArgument(i);
        }

        LLVM_DEBUG(
            dbgs() << F->getInternalName().c_str()
                   << "::" << P->getName().c_str() << " found arg of type: "
                   << typeCtx_.fmt(arg->getType()) << '\n');
        paramTy = typeCtx_.unionTy(paramTy, arg->getType());
      }

      P->setType(paramTy);
      checkAndSetPrePassType(P);

      if (P->getType() != originalTy) {
        LLVM_DEBUG(
            dbgs() << F->getInternalName().c_str()
                   << "::" << P->getName().c_str() << " changed to "
                   << typeCtx_.fmt(P->getType()) << "\n");
        changed = true;
      }
    }

    return changed;
  }

  /// Infer the return type of \p F and register it.
  /// Cannot narrow the type of \p F from the type prior to this function being
  /// called.
  /// \return true if the return type was changed.
  bool inferFunctionReturnType(Function *F) {
    Type originalTy = F->getReturnType();
    Type returnTy = originalTy;

    // Inner generator functions may be called with `.return()` with any value
    // of any type.
    if (F->isInnerGenerator()) {
      F->setReturnType(Type::createAnyType());
      checkAndSetPrePassType(F);
      return F->getReturnType() != originalTy;
    }

    for (auto &bbit : *F) {
      if (auto *returnInst =
              llvh::dyn_cast_or_null<ReturnInst>(bbit.getTerminator())) {
        returnTy =
            typeCtx_.unionTy(returnTy, returnInst->getValue()->getType());
      }
    }
    F->setReturnType(returnTy);
    checkAndSetPrePassType(F);
    return F->getReturnType() != originalTy;
  }

  /// Attempt to infer the type of a variable stored in memory.
  /// \return true if the type changed.
  bool inferMemoryType(Value *V) {
    Type originalTy = V->getType();
    Type T = inferMemoryLocationType(typeCtx_, V);

    // We were able to identify the type of the value. Record this info.
    if (T != V->getType()) {
      V->setType(T);
      checkAndSetPrePassType(V);
      return V->getType() != originalTy;
    }
    return false;
  }

  /// Clear every type for instructions, return types, parameters and variables
  /// in the function provided.
  /// Store the pre-pass types in prePassTypes_.
  void clearTypesInFunction(Function *f) {
    // Instructions
    for (auto &bbit : *f) {
      for (auto &it : bbit) {
        Instruction *inst = &it;
        if (inst->isTyped()) {
          // Typed instructions preserve their types in TypeInference.
          // Don't modify them.
          continue;
        }
        llvh::Optional<Type> inherent = inst->getInherentType();
        prePassTypes_.try_emplace(inst, inst->getType());
        // Clear to the inherent type if possible.
        inst->setType(inherent ? *inherent : Type::createNoType());
      }
    }
    // Parameters
    for (auto *P : f->getJSDynamicParams()) {
      prePassTypes_.try_emplace(P, P->getType());
      P->setType(Type::createNoType());
    }
    // Return type
    prePassTypes_.try_emplace(f, f->getReturnType());
    f->setReturnType(Type::createNoType());
  }

  /// Reset every return type and parameters in the function provided to the
  /// pre-pass type, to handle the cases where the type is notype due to
  /// unreachable or non-returning code.
  ///
  /// \return whether anything changed.
  bool resetReturnAndParamNoTypesToPrePass(Function *F) {
    LLVM_DEBUG(
        llvh::dbgs() << "Resetting types in " << F->getInternalName() << '\n');
    bool changed = false;

    // Parameters
    for (auto *P : F->getJSDynamicParams()) {
      if (P->getType().isNoType()) {
        assert(
            F->allCallsitesKnown() &&
            "params should be 'any' for unknown callsites");
        // We know all callsites, so we can infer the type of the parameter.
        // If it's notype, then there must be no reachable callsites.
        F->getAttributesRef(F->getParent()).unreachable = true;
        auto it = prePassTypes_.find(P);
        assert(it != prePassTypes_.end() && "Missing pre-pass type.");
        P->setType(it->second);
        LLVM_DEBUG(
            llvh::dbgs() << "Reset parameter type for " << P->getKindStr()
                         << " to " << it->second << '\n');
        changed = true;
      }
    }
    // Return type
    if (F->getReturnType().isNoType()) {
      auto it = prePassTypes_.find(F);
      assert(it != prePassTypes_.end() && "Missing pre-pass type.");
      F->setReturnType(it->second);
      if (!F->getAttributes(F->getParent()).unreachable) {
        // Don't mark the Function as noReturn if it's unreachable,
        // because the return type can be notype even if there is a return in
        // the Function.
        F->getAttributesRef(F->getParent()).noReturn = true;
      }
      changed |= !F->getReturnType().isNoType();
    }

    return changed;
  }

  /// Reset the type of \p V to the pre-pass type, to handle the cases where the
  /// type is notype due to unreachable or non-returning code.
  ///
  /// \return whether anything changed.
  bool resetVariableNoTypesToPrePass(Variable *V) {
    if (V->getType().isNoType()) {
      auto it = prePassTypes_.find(V);
      assert(it != prePassTypes_.end() && "Missing pre-pass type.");
      V->setType(it->second);
      return V->getType() != Type::createNoType();
    }
    return false;
  }

  /// Ensure that the type of \p val is not wider than its type prior to the
  /// pass by checking against the pre-pass type and intersecting the type with
  /// it when the pre-pass type is different than \p val's type.
  /// If \p val is a Function, then update its return type instead of its return
  /// type.
  /// \return true when the type of \p val was changed.
  bool checkAndSetPrePassType(Value *val) {
    Type originalTy = val->getType();
    if (auto *func = llvh::dyn_cast<Function>(val)) {
      originalTy = func->getReturnType();
    }
    auto it = prePassTypes_.find(val);
    if (it == prePassTypes_.end()) {
      return false;
    }
    if (it->second != originalTy) {
      Type intersection = typeCtx_.intersectTy(it->second, originalTy);
      // Narrow the type to include what we knew before the pass.
      LLVM_DEBUG(
          llvh::errs() << "Intersecting type of " << val->getKindStr()
                       << " from " << typeCtx_.fmt(val->getType()) << " with "
                       << typeCtx_.fmt(it->second) << " to "
                       << typeCtx_.fmt(intersection) << '\n');
      if (auto *func = llvh::dyn_cast<Function>(val)) {
        func->setReturnType(intersection);
      } else {
        val->setType(intersection);
      }
      return true;
    }
    return false;
  }
};

bool TypeInferenceImpl::runOnFunctionsAndVars(
    llvh::ArrayRef<Function *> functions,
    llvh::ArrayRef<Variable *> vars) {
  LLVM_DEBUG(
      dbgs() << "\nStart Type Inference on " << functions.size()
             << " functions and " << vars.size() << "vars.\n");

  // Post-order traversal of blocks for each function.
  // Reverse when iterating, so that we visit the blocks in reverse post-order.
  // Precomputed to avoid recomputing it in the inner loop.
  llvh::DenseMap<Function *, std::vector<BasicBlock *>> funcPostOrders{};

  // Begin by clearing the existing types and storing pre-pass types.
  // This prevents us from relying on the previous inference pass's type info,
  // which can be too loose (if things have been simplified, etc.).
  // Also precompute the post-order traversal for each function.
  for (Function *F : functions) {
    clearTypesInFunction(F);
    funcPostOrders.try_emplace(F, postOrderAnalysis(F));
  }

  for (Variable *V : vars) {
    prePassTypes_.try_emplace(V, V->getType());
    V->setType(Type::createNoType());
  }

  // Inferring the types of instructions can help us figure out the types of
  // variables. Typed variables can help us deduce the types of loads and other
  // values. This means that we need to iterate until we reach convergence.
  bool localChanged = false;
  // Whether we've already run the reset step once (don't do it twice, because
  // it wouldn't do anything).
  bool haveRunReset = false;
  do {
    LLVM_DEBUG(dbgs() << "\nStart TypeInference pass:\n");

    localChanged = false;

    // Infer the type of formal parameters, based on knowing the (full) set
    // of call sites from which this function may be invoked.
    bool inferredParam = false;
    for (Function *F : functions) {
      inferredParam |= inferParams(F);
    }
    if (inferredParam)
      LLVM_DEBUG(dbgs() << ">> Inferred a parameter\n");
    localChanged |= inferredParam;

    // Infer types of instructions.
    bool inferredInst = false;
    for (Function *F : functions) {
      llvh::ArrayRef<BasicBlock *> postOrder = funcPostOrders[F];
      for (auto *bbit : llvh::reverse(postOrder)) {
        for (auto &it : *bbit) {
          Instruction *I = &it;
          inferredInst |= inferInstruction(I);
        }
      }
    }
    if (inferredInst)
      LLVM_DEBUG(dbgs() << ">> Inferred an instruction\n");
    localChanged |= inferredInst;

    // Infer the return type of the function based on the type of return
    // instructions in the function.
    bool inferredRetType = false;
    for (Function *F : functions) {
      inferredRetType |= inferFunctionReturnType(F);
    }
    if (inferredRetType)
      LLVM_DEBUG(dbgs() << ">> Inferred function return type\n");
    localChanged |= inferredRetType;

    // Infer type of the supplied variables.
    bool inferredVarType = false;
    for (auto *var : vars) {
      inferredVarType |= inferMemoryType(var);
    }
    if (inferredVarType)
      LLVM_DEBUG(dbgs() << ">> Inferred variable type\n");
    localChanged |= inferredVarType;

    // The standard loop above failed to find any changes.
    // Run the reset step to populate remaining NoTypes.
    // Then we continue running the loop to ensure that we converge to the
    // correct types (e.g. PhiInst type should be the union of the operands).
    // We only have to do this once for this set of functions, so also check
    // haveRunReset.
    if (!localChanged && !haveRunReset) {
      for (Function *F : functions) {
        localChanged |= resetReturnAndParamNoTypesToPrePass(F);
      }
      for (Variable *var : vars) {
        localChanged |= resetVariableNoTypesToPrePass(var);
      }
      haveRunReset = true;
      if (localChanged)
        LLVM_DEBUG(dbgs() << ">> Reset NoTypes\n");
    }
  } while (localChanged);

  // Since we always infer from scratch, the inference has always "changed".
  return true;
}

/// Type of a group of functions and variables that should be processed
/// together by type inference.
using Partition = std::pair<std::vector<Function *>, std::vector<Variable *>>;

/// Partition the functions in \p M into groups such that each group contains
/// all functions and variables that have usages within the same group, and no
/// usages outside the group.
static std::vector<Partition> partitionFunctionsAndVars(Module *M) {
  // EquivalenceClasses basically implements union-find (disjoint-set).
  // This is convenient for finding all the functions use each other, as well as
  // their shared variables.
  llvh::EquivalenceClasses<const Value *> groups{};
  for (Function &F : *M) {
    // Add the function to the equivalence class, in case it doesn't have any
    // users or captured vars, we will create a new group.
    groups.insert(&F);

    // NOTE: unionSets automatically inserts both arguments if they don't exist
    // before unioning.

    // Include any users of the function, to account for known callsites
    // as well as closure creation.
    for (const Instruction *user : F.getUsers()) {
      groups.unionSets(&F, user->getFunction());
    }
  }

  // Iterate over all the variables, and union them with the functions that use
  // them. This ensures that all functions that access a variable are in the
  // same group as the variable. Unlike with functions, we disregard variables
  // that are unused, since there is nothing to meaningfully infer.
  for (VariableScope &VS : M->getVariableScopes()) {
    for (const Variable *V : VS.getVariables()) {
      for (const Instruction *user : V->getUsers()) {
        groups.unionSets(V, user->getFunction());
      }
    }
  }

  // Convert the EquivalenceClasses into a vector of vectors for faster
  // iteration.
  // Can't iterate the EquivalenceClasses directly because it uses pointers as
  // keys and we want a deterministic ordering.

  // Map from leader to index so we can use findLeader.
  llvh::DenseMap<const Value *, unsigned> groupIndices;
  // List of the groups, where group i has a leader and
  // groupIndices[leader] == i.
  std::vector<Partition> res;
  for (Function &F : *M) {
    const Value *leader = *groups.findLeader(&F);
    auto [it, inserted] = groupIndices.try_emplace(leader, res.size());
    if (inserted)
      res.emplace_back();

    res[it->second].first.push_back(&F);
  }

  for (VariableScope &VS : M->getVariableScopes()) {
    for (Variable *V : VS.getVariables()) {
      // Skip Variables that are not in a group, since they are unused.
      auto leaderIt = groups.findLeader(V);
      if (leaderIt == groups.member_end())
        continue;

      // Any variable that is in a group must have an associated function that
      // uses it, which would have already been inserted.
      const Value *leader = *leaderIt;
      auto it = groupIndices.find(leader);
      assert(it != groupIndices.end() && "Group not found");
      res[it->second].second.push_back(V);
    }
  }
  return res;
}

bool TypeInferenceImpl::runOnModule() {
  bool changed = false;
  LLVM_DEBUG(dbgs() << "\nStart Type Inference on Module\n");

  auto partitionedFuncsAndVars = partitionFunctionsAndVars(M_);
  for (const auto &[funcs, vars] : partitionedFuncsAndVars) {
    changed |= runOnFunctionsAndVars(funcs, vars);
  }
  return changed;
}

} // anonymous namespace

bool TypeInference::runOnModule(Module *M) {
  TypeInferenceImpl impl{M};
  return impl.runOnModule();
}

Pass *hermes::createTypeInference() {
  return new TypeInference();
}

#undef DEBUG_TYPE
