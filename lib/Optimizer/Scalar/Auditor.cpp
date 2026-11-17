/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "auditor"

#include "hermes/Optimizer/Scalar/Auditor.h"

#include "hermes/IR/Analysis.h"
#include "hermes/IR/CFG.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Support/Statistic.h"

#include "llvh/Support/Debug.h"

using namespace hermes;
using llvh::dbgs;

STATISTIC(
    CallsCreate,
    "Number of call instructions: callee obtained from definition");
STATISTIC(
    CallsProp,
    "Number of call instructions: callee obtained from prop lookup");
STATISTIC(
    CallsFrame,
    "Number of call instructions: callee obtained from frame");
STATISTIC(
    CallsStack,
    "Number of call instructions: callee obtained from stack");
STATISTIC(CallsPhi, "Number of call instructions: callee obtained from phi");
STATISTIC(
    CallsParameter,
    "Number of call instructions: callee obtained from a parameter");
STATISTIC(
    CallsCall,
    "Number of call instructions: callee obtained from a call");
STATISTIC(
    CallsOther,
    "Number of call instructions: callee obtained in other ways");
STATISTIC(Calls, "Number of call instructions of all kinds");
STATISTIC(GetParentScopes, "Number of GetParentScopeInst instructions");
STATISTIC(ResolveScopes, "Number of ResolveScopeInst instructions");
STATISTIC(GetClosureScopes, "Number of GetClosureScopeInst instructions");
STATISTIC(Functions, "Number of functions");

STATISTIC(TypeUndefined, "Number of instructions with type undefined");
STATISTIC(TypeNull, "Number of instructions with type null");
STATISTIC(TypeBool, "Number of instructions with type boolean");
STATISTIC(TypeString, "Number of instructions with type string");
STATISTIC(TypeNumber, "Number of instructions with type number");
STATISTIC(TypeObject, "Number of instructions with type object");
STATISTIC(TypeAny, "Number of instructions with type any");
STATISTIC(TypeOther, "Number of instructions with type other");

#if !defined(NDEBUG) || LLVM_FORCE_ENABLE_STATS
STATISTIC(NumTryCatchFunctions, "Number of Functions with try/catch");
#endif

static void auditInstructions(Function *F) {
  for (BasicBlock &BB : *F) {
    for (Instruction &II : BB) {
      if (II.getKind() == ValueKind::CallInstKind) {
        auto *CI = cast<CallInst>(&II);
        Value *callee = CI->getCallee();

        Calls += 1;

        switch (callee->getKind()) {
          case ValueKind::CreateFunctionInstKind:
            CallsCreate += 1;
            break;
          case ValueKind::LoadPropertyInstKind:
            CallsProp += 1;
            break;
          case ValueKind::LoadFrameInstKind: {
            CallsFrame += 1;
          } break;
          case ValueKind::LoadStackInstKind:
            CallsStack += 1;
            break;
          case ValueKind::PhiInstKind:
            CallsPhi += 1;
            break;
          case ValueKind::JSDynamicParamKind:
            CallsParameter += 1;
            break;
          case ValueKind::CallInstKind:
            CallsCall += 1;
            break;
          default:
            // Other cases; for example, property enumeration.
            CallsOther += 1;
            break;
        }
      } else if (II.getKind() == ValueKind::GetParentScopeInstKind) {
        GetParentScopes++;
      } else if (II.getKind() == ValueKind::ResolveScopeInstKind) {
        ResolveScopes++;
      } else if (II.getKind() == ValueKind::GetClosureScopeInstKind) {
        GetClosureScopes++;
      }
    }
  }
}

static void auditInferredTypes(Function *F) {
  for (BasicBlock &BB : *F) {
    for (Instruction &II : BB) {
      Type t = II.getType();

      if (t.isUndefinedType()) {
        TypeUndefined++;
      } else if (t.isNullType()) {
        TypeNull++;
      } else if (t.isBooleanType()) {
        TypeBool++;
      } else if (t.isStringType()) {
        TypeString++;
      } else if (t.isNumberType()) {
        TypeNumber++;
      } else if (
          !t.isNoType() &&
          F->getParent()->getTypeContext().isSubsetOf(
              t, Type::createObject())) {
        // Plain object or any refinement of it (class instance, array,
        // function, ...). NoType, the type of instructions without an output,
        // is a subset of every type, so exclude it.
        TypeObject++;
      } else if (F->getParent()->getTypeContext().canBeAny(t)) {
        TypeAny++;
      } else {
        // Other cases not counted above, e.g. union types.
        TypeOther++;
      }
    }
  }
}

bool Auditor::runOnFunction(Function *F) {
  LLVM_DEBUG(dbgs() << "Auditing calls in " << F->getInternalNameStr() << "\n");

  auditInstructions(F);

  LLVM_DEBUG(
      dbgs() << "Auditing instruction return types in "
             << F->getInternalNameStr() << "\n");

  auditInferredTypes(F);

  Functions++;

#if !defined(NDEBUG) || LLVM_FORCE_ENABLE_STATS
  if (functionHasTryCatch(F)) {
    NumTryCatchFunctions++;
  }
#endif

  return false;
}

Pass *hermes::createAuditor() {
  return new Auditor();
}

#undef DEBUG_TYPE
