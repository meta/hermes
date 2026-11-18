/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/BCGen/Lowering.h"
#include "hermes/BCGen/SerializedLiteralGenerator.h"
#include "hermes/IR/Analysis.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Inst/Inst.h"
#include "hermes/Optimizer/Scalar/Utils.h"

#define DEBUG_TYPE "lowering"

using namespace hermes;

bool SwitchLowering::runOnFunction(Function *F) {
  bool changed = false;
  llvh::SmallVector<SwitchInst *, 4> switches;
  // Collect all switch instructions.
  for (BasicBlock &BB : *F)
    for (auto &it : BB) {
      if (auto *S = llvh::dyn_cast<SwitchInst>(&it))
        switches.push_back(S);
    }

  for (auto *S : switches) {
    lowerSwitchIntoIfs(S);
    changed = true;
  }

  return changed;
}

void SwitchLowering::lowerSwitchIntoIfs(SwitchInst *switchInst) {
  IRBuilder builder(switchInst->getParent()->getParent());
  builder.setLocation(switchInst->getLocation());

  BasicBlock *defaultDest = switchInst->getDefaultDestination();
  BasicBlock *next = defaultDest;
  BasicBlock *currentBlock = switchInst->getParent();

  // In this loop we are generating a sequence of IFs in reverse. We start
  // with the last IF that points to the Default case, and go back until we
  // generate the first IF. Then we connect the first IF into the entry
  // block and delete the Switch instruction.
  for (unsigned i = 0, e = switchInst->getNumCasePair(); i < e; ++i) {
    // Create an IF statement that matches the i'th case.
    BasicBlock *ifBlock = builder.createBasicBlock(currentBlock->getParent());

    // We scan the basic blocks in reverse!
    unsigned idx = (e - i - 1);
    auto caseEntry = switchInst->getCasePair(idx);

    builder.setInsertionBlock(ifBlock);
    auto *pred = builder.createBinaryOperatorInst(
        caseEntry.first,
        switchInst->getInputValue(),
        ValueKind::BinaryStrictlyEqualInstKind);
    // Cond branch - if the predicate of the comparison is true then jump
    // into the destination block. Otherwise jump to the next comparison in
    // the chain.
    builder.createCondBranchInst(pred, caseEntry.second, next);

    // Update any phis in the destination block.
    copyPhiTarget(caseEntry.second, currentBlock, ifBlock);

    if (next == defaultDest && caseEntry.second != next) {
      // If this block is responsible for jumps to the default block (true on
      // the first iteration), and the default block is distinct from the
      // destination of this block (which we have already updated) update phi
      // nodes in the default block too.
      copyPhiTarget(next, currentBlock, ifBlock);
    }

    next = ifBlock;
  }

  // Erase the phi edges that previously came from this block.
  erasePhiTarget(defaultDest, currentBlock);
  for (unsigned i = 0, e = switchInst->getNumCasePair(); i < e; ++i) {
    erasePhiTarget(switchInst->getCasePair(i).second, currentBlock);
  }

  switchInst->eraseFromParent();
  builder.setInsertionBlock(currentBlock);
  builder.createBranchInst(next);
}

/// Copy all incoming phi edges from a block to a new one
void SwitchLowering::copyPhiTarget(
    BasicBlock *block,
    BasicBlock *previousBlock,
    BasicBlock *newBlock) {
  for (auto &inst : *block) {
    auto *phi = llvh::dyn_cast<PhiInst>(&inst);
    if (!phi)
      break; // Phi must be first, so we won't find any more.

    Value *currentValue = nullptr;
    for (int i = 0, e = phi->getNumEntries(); i < e; i++) {
      auto pair = phi->getEntry(i);
      if (pair.second != previousBlock)
        continue;
      currentValue = pair.first;
      break;
    }

    if (currentValue) {
      phi->addEntry(currentValue, newBlock);
    }
  }
}

void SwitchLowering::erasePhiTarget(BasicBlock *block, BasicBlock *toDelete) {
  for (auto &inst : *block) {
    auto *phi = llvh::dyn_cast<PhiInst>(&inst);
    if (!phi)
      break; // Phi must be first, so we won't find any more.

    for (signed i = (signed)phi->getNumEntries() - 1; i >= 0; i--) {
      auto pair = phi->getEntry(i);
      if (pair.second != toDelete)
        continue;
      phi->removeEntry(i);
      // Some codegen can add multiple identical entries, so keep looking.
    }
  }
}

bool LowerAllocObjectLiteral::runOnFunction(Function *F) {
  bool changed = false;
  llvh::SmallVector<AllocObjectLiteralInst *, 4> allocs;
  for (BasicBlock &BB : *F) {
    // We need to increase the iterator before calling lowerAllocObjectBuffer.
    // Otherwise deleting the instruction will invalidate the iterator.
    for (auto it = BB.begin(), e = BB.end(); it != e;) {
      if (auto *A = llvh::dyn_cast<BaseAllocObjectLiteralInst>(&*it++)) {
        changed |= lowerAllocObjectBuffer(A);
      }
    }
  }

  return changed;
}

bool LowerAllocObjectLiteral::lowerAllocObjectBuffer(
    BaseAllocObjectLiteralInst *allocInst) {
  Function *F = allocInst->getParent()->getParent();
  IRBuilder builder(F);
  uint32_t size = allocInst->getKeyValuePairCount();

  // Handle empty object literals.
  // If there's no parent object, don't lower it so that we don't allocate a
  // shape table entry.
  // If there is a parent object, we want to lower it in order to create a
  // shape table entry because we want to cache the HiddenClass (which
  // contains the parent), so just let this function complete as usual.
  if (size == 0 && llvh::isa<EmptySentinel>(allocInst->getParentObject())) {
    return false;
  }

  // LowerNumericProperties leaves the keys of typed objects as strings, even
  // index-like ones, so every slot of a typed object is stored directly.
  bool isTyped = !llvh::isa<AllocObjectLiteralInst>(allocInst);

  // Replace AllocObjectLiteral with lowered instruction.
  builder.setLocation(allocInst->getLocation());
  builder.setInsertionPointAfter(allocInst);
  LIRAllocObjectFromBufferInst::ObjectPropertyMap propMap;

  bool hasSeenNumericProp = false;
  for (unsigned i = 0; i < size; i++) {
    Literal *propKey = allocInst->getKey(i);
#ifndef NDEBUG
    if (auto *keyStr = llvh::dyn_cast<LiteralString>(propKey)) {
      assert(
          (isTyped || !toArrayIndex(keyStr->getValue().str()).hasValue()) &&
          "LiteralString that looks like an array index should have been converted to a number.");
    }
#endif
    Value *propVal = allocInst->getValue(i);
    bool isNumericKey = llvh::isa<LiteralNumber>(propKey);
    hasSeenNumericProp |= isNumericKey;
    if (SerializedLiteralGenerator::isSerializableLiteral(propVal)) {
      propMap.push_back({propKey, llvh::cast<Literal>(propVal)});
    } else {
      // Add the literal key in with a dummy placeholder value.
      propMap.push_back(
          std::pair<Literal *, Literal *>(propKey, builder.getLiteralNull()));
      // Patch the placeholder with the correct value.
      if (hasSeenNumericProp) {
        // We don't assume the runtime storage and layout of numeric properties.
        // So, if we have encountered a numeric property, we cannot store
        // directly into a slot.
        if (isNumericKey) {
          builder.createDefineOwnPropertyInst(
              propVal, allocInst, propKey, IRBuilder::PropEnumerable::Yes);
        } else {
          // For non-numeric keys, StorePropertyInst is more efficient because
          // it can be cached off the string ID.
          builder.createStorePropertyInst(propVal, allocInst, propKey);
        }
      } else {
        // If we haven't encountered a numeric property, we can store
        // directly into a slot.
        builder.createPrStoreInst(
            propVal,
            allocInst,
            i,
            propKey,
            builder.getTypeContext().isNonPtr(propVal->getType()));
      }
    }
  }

  // Emit lowered instruction.
  // First, we reset insertion location.
  builder.setLocation(allocInst->getLocation());
  builder.setInsertionPoint(allocInst);

  // The typed allocation LIR requires an explicit parent. The runtime treats
  // undefined as Object.prototype.
  Value *parent = allocInst->getParentObject();
  if (isTyped && llvh::isa<EmptySentinel>(parent))
    parent = builder.getLiteralUndefined();

  Instruction *alloc;
  if (llvh::isa<AllocTypedNonEnumObjectInst>(allocInst)) {
    alloc =
        builder.createLIRAllocTypedNonEnumObjectFromBufferInst(parent, propMap);
  } else if (llvh::isa<AllocTypedObjectInst>(allocInst)) {
    alloc = builder.createLIRAllocTypedObjectFromBufferInst(parent, propMap);
  } else {
    alloc = builder.createLIRAllocObjectFromBufferInst(parent, propMap);
  }

  allocInst->replaceAllUsesWith(alloc);
  allocInst->eraseFromParent();

  return true;
}

bool LowerNumericProperties::stringToNumericProperty(
    IRBuilder &builder,
    Instruction &Inst,
    unsigned operandIdx) {
  auto strLit = llvh::dyn_cast<LiteralString>(Inst.getOperand(operandIdx));
  if (!strLit)
    return false;

  // Check if the string looks exactly like an array index.
  auto num = toArrayIndex(strLit->getValue().str());
  if (num) {
    Inst.setOperand(builder.getLiteralNumber(*num), operandIdx);
    return true;
  }

  return false;
}

bool LowerNumericProperties::runOnFunction(Function *F) {
  IRBuilder builder(F);
  IRBuilder::InstructionDestroyer destroyer{};

  bool changed = false;
  for (BasicBlock &BB : *F) {
    for (Instruction &Inst : BB) {
      if (llvh::isa<BaseLoadPropertyInst>(&Inst)) {
        changed |= stringToNumericProperty(
            builder, Inst, LoadPropertyInst::PropertyIdx);
      } else if (llvh::isa<StorePropertyInst>(&Inst)) {
        changed |= stringToNumericProperty(
            builder, Inst, StorePropertyInst::PropertyIdx);
      } else if (llvh::isa<DefineOwnPropertyInst>(&Inst)) {
        changed |= stringToNumericProperty(
            builder, Inst, DefineOwnPropertyInst::PropertyIdx);
      } else if (llvh::isa<DeletePropertyInst>(&Inst)) {
        changed |= stringToNumericProperty(
            builder, Inst, DeletePropertyInst::PropertyIdx);
      } else if (llvh::isa<DefineOwnGetterSetterInst>(&Inst)) {
        changed |= stringToNumericProperty(
            builder, Inst, DefineOwnGetterSetterInst::PropertyIdx);
      } else if (llvh::isa<AllocObjectLiteralInst>(&Inst)) {
        auto allocInst = cast<AllocObjectLiteralInst>(&Inst);
        for (unsigned i = 0; i < allocInst->getKeyValuePairCount(); i++) {
          changed |= stringToNumericProperty(
              builder, Inst, AllocObjectLiteralInst::getKeyOperandIdx(i));
        }
      }
    }
  }
  return changed;
}

static llvh::SmallVector<Value *, 4> getArgumentsWithoutThis(CallInst *CI) {
  llvh::SmallVector<Value *, 4> args;
  for (size_t i = 1; i < CI->getNumArguments(); i++) {
    args.push_back(CI->getArgument(i));
  }
  return args;
}

bool LowerCalls::runOnFunction(Function *F) {
  IRBuilder::InstructionDestroyer destroyer;
  const Module *mod = F->getParent();
  IRBuilder builder(F);
  bool changed = false;
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      if (auto *CI = llvh::dyn_cast<CallInst>(&I)) {
        if (CI->getAttributes(mod).isMetroRequire) {
          // We'll convert calls with this attribute to a specific
          // bytecode later.
          continue;
        }

        unsigned argCount = CI->getNumArguments();
        if (argCount > UINT8_MAX) {
          builder.setLocation(CI->getLocation());
          builder.setInsertionPoint(CI);
          auto *replacement = builder.createHBCCallWithArgCount(
              CI->getCallee(),
              CI->getTarget(),
              CI->getCalleeIsAlwaysClosure()->getValue(),
              CI->getEnvironment(),
              CI->getNewTarget(),
              builder.getLiteralNumber(argCount),
              CI->getThis(),
              getArgumentsWithoutThis(CI));
          CI->replaceAllUsesWith(replacement);
          destroyer.add(CI);
          changed = true;
          continue;
        }
        // HBCCallNInst can only be used when new.target is undefined.
        if (HBCCallNInst::kMinArgs <= argCount &&
            argCount <= HBCCallNInst::kMaxArgs &&
            llvh::isa<LiteralUndefined>(CI->getNewTarget())) {
          builder.setLocation(CI->getLocation());
          builder.setInsertionPoint(CI);
          HBCCallNInst *newCall = builder.createHBCCallNInst(
              CI->getCallee(),
              CI->getTarget(),
              CI->getCalleeIsAlwaysClosure()->getValue(),
              CI->getEnvironment(),
              CI->getNewTarget(),
              CI->getThis(),
              getArgumentsWithoutThis(CI));
          newCall->setType(CI->getType());
          CI->replaceAllUsesWith(newCall);
          destroyer.add(CI);
          changed = true;
        }
      }
    }
  }
  return changed;
}

bool LimitAllocArray::runOnFunction(Function *F) {
  bool changed = false;
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      auto *inst = llvh::dyn_cast<AllocArrayInst>(&I);
      if (!inst || inst->getElementCount() == 0)
        continue;

      IRBuilder builder(F);
      builder.setInsertionPointAfter(inst);
      builder.setLocation(inst->getLocation());

      // Checks if any operand of an AllocArray is unserializable.
      // If it finds one, the loop removes it along with every operand past it.
      {
        bool seenUnserializable = false;
        unsigned ind = -1;
        unsigned i = AllocArrayInst::ElementStartIdx;
        unsigned e = inst->getElementCount() + AllocArrayInst::ElementStartIdx;
        while (i < e) {
          ind++;
          seenUnserializable |=
              inst->getOperand(i)->getKind() == ValueKind::LiteralBigIntKind ||
              inst->getOperand(i)->getKind() == ValueKind::LiteralUndefinedKind;
          if (seenUnserializable) {
            e--;
            builder.createDefineOwnPropertyInst(
                inst->getOperand(i),
                inst,
                builder.getLiteralNumber(ind),
                IRBuilder::PropEnumerable::Yes);
            inst->removeOperand(i);
            changed = true;
            continue;
          }
          i++;
        }
      }

      if (inst->getElementCount() == 0)
        continue;

      // Since we remove elements from inst until it fits in maxSize_,
      // the final addition to totalElems will make it equal maxSize_. Any
      // AllocArray past that would have all its operands removed, and add
      // 0 to totalElems.
      for (unsigned i = inst->getElementCount() - 1; i >= maxSize_; i--) {
        int operandOffset = AllocArrayInst::ElementStartIdx + i;
        builder.createDefineOwnPropertyInst(
            inst->getOperand(operandOffset),
            inst,
            builder.getLiteralNumber(i),
            IRBuilder::PropEnumerable::Yes);
        inst->removeOperand(operandOffset);
      }
      changed = true;
    }
  }
  return changed;
}

bool LowerCondBranch::isOperatorSupported(ValueKind kind) {
  switch (kind) {
    case ValueKind::BinaryLessThanInstKind: // <
    case ValueKind::BinaryLessThanOrEqualInstKind: // <=
    case ValueKind::BinaryGreaterThanInstKind: // >
    case ValueKind::BinaryGreaterThanOrEqualInstKind: // >=
    case ValueKind::BinaryStrictlyEqualInstKind:
    case ValueKind::BinaryStrictlyNotEqualInstKind:
    case ValueKind::BinaryNotEqualInstKind: // !=
    case ValueKind::BinaryEqualInstKind: // ==
      return true;
    default:
      return false;
  }
}

bool LowerCondBranch::runOnFunction(Function *F) {
  IRBuilder builder(F);
  bool changed = false;

  IRBuilder::InstructionDestroyer destroyer{};

  for (auto &BB : *F) {
    auto *cbInst = llvh::dyn_cast<CondBranchInst>(BB.getTerminator());
    // This also matches constructors.
    if (!cbInst)
      continue;

    Value *cond = cbInst->getCondition();

    if (auto *binopInst = llvh::dyn_cast<BinaryOperatorInst>(cond)) {
      // If the condition has more than one user, we can't lower it.
      if (!cond->hasOneUser())
        continue;

      auto *LHS = binopInst->getLeftHandSide();
      auto *RHS = binopInst->getRightHandSide();

      // The condition must either be side-effect free, or it must be the
      // previous instruction.
      if (binopInst->getSideEffect().mayReadOrWorse())
        if (cbInst->getPrevNode() != binopInst)
          continue;

      // Only certain operators are supported.
      if (!isOperatorSupported(binopInst->getKind()))
        continue;

      builder.setInsertionPoint(cbInst);
      builder.setLocation(cbInst->getLocation());
      auto *cmpBranch = builder.createHBCCompareBranchInst(
          LHS,
          RHS,
          HBCCompareBranchInst::fromBinaryOperatorValueKind(
              binopInst->getKind()),
          cbInst->getTrueDest(),
          cbInst->getFalseDest());

      cbInst->replaceAllUsesWith(cmpBranch);
      destroyer.add(cbInst);
      destroyer.add(binopInst);
      changed = true;
    } else if (auto *fcompare = llvh::dyn_cast<FCompareInst>(cond)) {
      // The condition is pure and fast to execute, so it doesn't matter how
      // many users it has.
      assert(fcompare->getSideEffect().isPure() && "FCompare has side effect");

      auto *LHS = fcompare->getLeft();
      auto *RHS = fcompare->getRight();

      builder.setInsertionPoint(cbInst);
      builder.setLocation(cbInst->getLocation());
      auto *cmpBranch = builder.createHBCFCompareBranchInst(
          LHS,
          RHS,
          HBCFCompareBranchInst::fromFCompareValueKind(fcompare->getKind()),
          cbInst->getTrueDest(),
          cbInst->getFalseDest());

      cbInst->replaceAllUsesWith(cmpBranch);
      destroyer.add(cbInst);
      if (!fcompare->hasUsers())
        destroyer.add(fcompare);
      changed = true;
    } else if (auto *typeOfIs = llvh::dyn_cast<TypeOfIsInst>(cond)) {
      builder.setInsertionPoint(cbInst);
      builder.setLocation(cbInst->getLocation());
      auto *cmpBranch = builder.createHBCCmpBrTypeOfIsInst(
          typeOfIs->getArgument(),
          typeOfIs->getTypes(),
          cbInst->getTrueDest(),
          cbInst->getFalseDest());

      cbInst->replaceAllUsesWith(cmpBranch);
      destroyer.add(cbInst);
      if (!typeOfIs->hasUsers())
        destroyer.add(typeOfIs);
      changed = true;
    } else {
      continue;
    }
  }
  return changed;
}

Pass *hermes::createLowerPrivateBrandCheck() {
  class ThisPass : public FunctionPass {
   public:
    explicit ThisPass() : FunctionPass("LowerPrivateBrandCheck") {}

    bool runOnFunction(Function *F) override {
      IRBuilder builder{F};
      bool changed = false;

      // Collect all PrivateBrandCheckInst first to avoid iterator invalidation
      // when splitting blocks.
      llvh::SmallVector<PrivateBrandCheckInst *, 4> brandChecks;
      for (auto &BB : *F) {
        for (auto &I : BB) {
          if (auto *PBC = llvh::dyn_cast<PrivateBrandCheckInst>(&I)) {
            brandChecks.push_back(PBC);
          }
        }
      }

      for (auto *PBC : brandChecks) {
        auto *currentBB = PBC->getParent();

        auto *continueBB =
            splitBasicBlock(currentBB, std::next(PBC->getIterator()));

        auto *throwBB = builder.createBasicBlock(F);

        builder.setInsertionPoint(PBC);

        auto *checkResult = builder.createBinaryOperatorInst(
            PBC->getBrand(),
            PBC->getObject(),
            ValueKind::BinaryPrivateInInstKind);

        builder.createCondBranchInst(checkResult, continueBB, throwBB);

        builder.setInsertionBlock(throwBB);
        builder.createThrowTypeErrorInst(
            builder.getLiteralString("Private element not found"));

        PBC->eraseFromParent();
        changed = true;
      }

      // Fix up catch targets for any ThrowTypeErrorInst we created inside
      // try blocks.
      if (changed) {
        fixupCatchTargets(F);
      }

      return changed;
    }
  };
  return new ThisPass();
}

#undef DEBUG_TYPE
