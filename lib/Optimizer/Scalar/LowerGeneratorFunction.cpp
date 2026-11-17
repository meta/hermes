/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
///
// The core idea of this pass is to transform the sequence of yield instructions
// within the generator function to one large switch statement that branches to
// the correct spot in the function. Each target block of a SaveAndYieldInst
// will occupy a slot in this switch statement. Each SaveAndYieldInst will then
// turn into an instruction that modifies the switch variable to point to the
// next block that the function should resume from.
//
// Exceptions:
// As a result of this new switch structure, we cannot retain the TryStartInsts
// that existed in the function originally. It would be invalid for the main
// switch to jump immediately into the middle of a block that was enclosed in a
// try. So, we remove all of the user's try instructions, and wrap the execution
// of all user blocks in one single try.  If an exception is thrown, we have to
// figure out which user block would have caught it, had we not removed all the
// try instructions. The exception handler we install needs to modify the switch
// variable to point to the correct user handler, then restart the execution of
// the main switch again. The installed handler recovers the information it
// needs by using another switch and switch variable. All of the try
// instructions (TryStart/Catch/TryEnd) modify the exception handler switch
// variable with a unique number. This is enough information to be able to
// recover which user error handler block we should jump to when an exception is
// thrown.
//
// All 'local' instructions must be removed from the inner function and lifted
// into the outer function as a variable. Local here means any instructions that
// refer to stack values, such as AllocStackInst or instructions that read
// paramters.
//
// The creation of the switch changes the dominance structure between the
// existing blocks. Values that now cross blocks without proper dominance need
// to be lifted into the outer closure.
//
//===----------------------------------------------------------------------===//

#include "hermes/BCGen/GeneratorResumeMethod.h"
#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/IR/Analysis.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/IRUtils.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/Utils.h"

namespace hermes {
namespace {
/// Set the insertion point of \p builder to \p I, and set its location to \p I.
static void moveBuilderTo(Instruction *I, IRBuilder &builder) {
  builder.setLocation(I->getLocation());
  builder.setInsertionPoint(I);
}

/// Set the insertion point of \p builder after \p I, and set its location to \p
/// I.
static void moveBuilderAfter(Instruction *I, IRBuilder &builder) {
  builder.setLocation(I->getLocation());
  builder.setInsertionPointAfter(I);
}

/// ES6.0 7.4.7
/// \return ReturnInst of object {value: \p val, done: \p done}.
static ReturnInst *
emitReturnIterResultObject(IRBuilder &builder, Value *val, bool done) {
  AllocObjectLiteralInst::ObjectPropertyMap props{};
  props.push_back({builder.getLiteralString("value"), val});
  props.push_back(
      {builder.getLiteralString("done"), builder.getLiteralBool(done)});
  return builder.createReturnInst(builder.createAllocObjectLiteralInst(props));
}

/// This class will perform a transformation of the IR for a given pair of inner
/// and outer generator functions. A state machine is created to branch to the
/// correct location inside the inner function. All of the state that needs to
/// be persisted between generator invocations is lifted to the outer function's
/// environment.
class LowerToStateMachine {
  /// The module.
  Module *M_;
  /// Outer function: a GeneratorFunction or an AsyncFunction.
  Function *outer_;
  /// Inner generator function.
  NormalFunction *inner_;
  /// The instruction that creates the inner generator function.
  CreateGeneratorInst *CGI_;
  /// Scope instruction operand passed to the CreateGenerator instruction.
  BaseScopeInst *newOuterScope_;
  /// Instruction to obtain the outer function scope. Should be created in the
  /// inner function.
  BaseScopeInst *getParentOuterScope_;
  /// Builder used to generate IR.
  IRBuilder builder_;

  /// Represents the GeneratorState internal slot.
  /// ES6.0 25.3.2.
  enum class State {
    SuspendedStart,
    SuspendedYield,
    Executing,
    Completed,
  };

 public:
  explicit LowerToStateMachine(
      Module *M,
      Function *outer,
      CreateGeneratorInst *CGI)
      : M_(M),
        outer_(outer),
        inner_(llvh::cast<NormalFunction>(CGI->getFunctionCode())),
        CGI_(CGI),
        newOuterScope_(nullptr),
        getParentOuterScope_(nullptr),
        builder_(M) {}

  /// Peform the conversion to the state machine.
  void convert();

 private:
  /// Create a new scope in the outer function that we will store all the needed
  /// variables into.
  void setupScopes();

  /// Move all 'local' values of the inner function to variables in the outer
  /// function. Local here refers to instructions referencing local parameters,
  /// and AllocStackInsts.
  void moveLocalsToOuter();

  /// Terminators need to be handled differently than other instructions
  /// consuming an AllocStackInst. We cannot insert the IR to do the copying of
  /// the dummy to the outer variable right after the instruction. No
  /// instructions are allowed after a terminator. So, we create a new BB per
  /// successor of the terminator. This new BB does the writeback of the
  /// dummyAlloc to the outer variable, and then branches to the original
  /// successor.
  ///
  /// \p TI the terminator that is taking in an AllocStackInst.
  /// \p dummyAlloc the replacement AllocStackInst for TI.
  /// \p outerVar needs to be updated with the value of dummyAlloc.
  /// \p seenBefore true if \p TI has been encountered already.
  void writebackTerminator(
      TerminatorInst *TI,
      AllocStackInst *dummyAlloc,
      Variable *outerVar,
      bool seenBefore);

  /// Lower ResumeGeneratorInst.
  void lowerResumeGenerator(
      LoadParamInst *actionParam,
      LoadParamInst *valueParam,
      Variable *genState);

  /// Lower the inner function to a switch.
  void lowerToSwitch(
      LoadParamInst *actionParam,
      LoadParamInst *valueParam,
      Variable *genState);

  /// Find all values that are used across basic blocks without proper
  /// dominance. Move these values to the outer function.
  void moveCrossingValuesToOuter();

  /// Find PhiInsts that have an entry block which is not a predecessor of the
  /// PhiInst. Move these instructions to the outer function.
  void moveInnerPhisToOuter(DominanceInfo &D);

  /// Create a BB to handle being in the 'Completed' state of the generator.
  BasicBlock *createCompletedStateBlock(
      LoadParamInst *action,
      LoadParamInst *value);
};

void LowerToStateMachine::convert() {
  setupScopes();
  moveLocalsToOuter();

  auto &innerFuncParams = inner_->getJSDynamicParams();
  // Clear out the original parameters in inner_, except for 'this'.
  for (size_t i = 1, e = innerFuncParams.size(); i < e; ++i)
    Value::destroy(innerFuncParams[i]);
  innerFuncParams.resize(1);

  // The inner function will take 2 parameters: action, value. Action
  // communicates what method was called on the generator: next, return, or
  // throw. For return and throw actions, value will hold the parameter that was
  // passed.
  auto *actionParam = builder_.createJSDynamicParam(
      inner_, builder_.createIdentifier("action"));
  actionParam->setType(Type::createUint32());
  auto *valueParam =
      builder_.createJSDynamicParam(inner_, builder_.createIdentifier("value"));
  movePastFirstInBlock(builder_, &*inner_->begin());
  // Reset the leaked user source location so the synthetic resume prologue is
  // not mistaken for real user code (e.g. a breakpoint on that line resolving
  // here). Use the inner function's own declaration location rather than "no
  // location": it matches a normal function's prologue and the sibling
  // scaffolding (getParentOuterScope_), and gives the executing-generator
  // TypeError thrown from this entry dispatch a real line in stack traces
  // rather than a raw-address fallback.
  builder_.setLocation(inner_->getSourceRange().Start);
  auto *loadActionParam = builder_.createLoadParamInst(actionParam);
  auto *loadValueParam = builder_.createLoadParamInst(valueParam);

  // This variable holds the state of the generator, directly corresponding to
  // ES6.0 25.3.2.
  Variable *genState = builder_.createVariable(
      getParentOuterScope_->getVariableScope(),
      "generator_state",
      Type::createInt32(),
      /* hidden */ true);

  lowerResumeGenerator(loadActionParam, loadValueParam, genState);
  lowerToSwitch(loadActionParam, loadValueParam, genState);
  // Creating the switch will break the connection between user BBs in the
  // process. Values that flow between the newly disjointed BBs are illegal and
  // should be moved to the closure. So do the promotion after these
  // operations.
  moveCrossingValuesToOuter();

  fixupCatchTargets(inner_);
}

void LowerToStateMachine::setupScopes() {
  moveBuilderTo(CGI_, builder_);
  // Create a new scope in the outer function to store our variables into. This
  // means we must change the operand to the CreateGeneratorInst with this new
  // scope.
  auto *existingScopeOperand = llvh::cast<BaseScopeInst>(CGI_->getScope());
  auto *newVarScope =
      builder_.createVariableScope(existingScopeOperand->getVariableScope());
  auto *newScope =
      builder_.createCreateScopeInst(newVarScope, existingScopeOperand);
  CGI_->setOperand(newScope, CreateGeneratorInst::ScopeIdx);
  CGI_->setVarScope(newVarScope);
  newOuterScope_ = newScope;
  movePastFirstInBlock(builder_, &inner_->front());
  getParentOuterScope_ = builder_.createGetParentScopeInst(
      newScope->getVariableScope(), inner_->getParentScopeParam());
  IRBuilder::InstructionDestroyer destroyer{};
  for (auto &I : inner_->getParentScopeParam()->getUsers()) {
    // Don't process the GPSI that we created above.
    if (I == getParentOuterScope_)
      continue;
    // We have just inserted a new layer of scope above the inner function. This
    // means that all the existing GPSI's are now inaccurate. What they
    // originally returned can be obtained using a ResolveScopeInst, starting
    // from what the current, new parent is.
    if (auto *GPSI = llvh::dyn_cast<GetParentScopeInst>(I)) {
      moveBuilderTo(GPSI, builder_);
      auto *RSI = builder_.createResolveScopeInst(
          GPSI->getVariableScope(),
          getParentOuterScope_->getVariableScope(),
          getParentOuterScope_);
      GPSI->replaceAllUsesWith(RSI);
      destroyer.add(GPSI);
    }
  }
}

void LowerToStateMachine::moveLocalsToOuter() {
  auto &innerFuncParams = inner_->getJSDynamicParams();
  // Map the parameter in the inner function to the variable declared in the
  // outer_ function scope that is replacing it.
  llvh::DenseMap<JSDynamicParam *, Variable *> paramReplacements;
  builder_.setInsertionPoint(CGI_);
  for (size_t i = 0, e = innerFuncParams.size(); i < e; ++i) {
    auto jsParam = innerFuncParams[i];
    auto outerVar = builder_.createVariable(
        getParentOuterScope_->getVariableScope(),
        jsParam->getName(),
        jsParam->getType(),
        /* hidden */ false);
    paramReplacements.insert({jsParam, outerVar});
    if (i == 0) {
      // The 'this' param already exists in outer_, so no need to
      // create a new JSDynamicParam. IRGen types it from the same function
      // node in both functions.
      JSDynamicParam *outerThis = outer_->getJSDynamicParams()[0];
      assert(
          outerThis->getType() == jsParam->getType() &&
          "outer and inner 'this' types must match");
      builder_.createStoreFrameInst(
          newOuterScope_, builder_.createLoadParamInst(outerThis), outerVar);
    } else {
      // The outer function receives the arguments, so its parameters take
      // the types IRGen gave the inner ones.
      auto *outerJSParam =
          builder_.createJSDynamicParam(outer_, jsParam->getName());
      outerJSParam->setType(jsParam->getType());
      builder_.createStoreFrameInst(
          newOuterScope_, builder_.createLoadParamInst(outerJSParam), outerVar);
    }
  }

  // We have to handle AllocStackInst separately, because we create new
  // AllocStackInsts in the process. This would lead to an infinite loop if we
  // tried to process AllocStackInst during the iteration of all instructions in
  // each BasicBlock.
  llvh::SmallVector<AllocStackInst *, 4> allocStacks;
  IRBuilder::InstructionDestroyer destroyer{};
  // This builder will be left in the correct spot to insert instructions at the
  // end of outer_. Place it right before CGI_ so we can correctly reference its
  // arguments.
  IRBuilder beforeCGI(outer_);
  beforeCGI.setInsertionPoint(CGI_);
  for (BasicBlock &BB : *inner_) {
    for (auto iter = BB.begin(), end = BB.end(); iter != end;) {
      auto &I = *iter++;
      if (auto *ASI = llvh::dyn_cast<AllocStackInst>(&I)) {
        allocStacks.push_back(ASI);
        continue;
      }

      if (auto *LPI = llvh::dyn_cast<LoadParamInst>(&I)) {
        Variable *v = paramReplacements[LPI->getParam()];
        assert(v && "JSDynamicParam should have had a replacement variable");
        moveBuilderTo(LPI, builder_);
        auto *loadReplacement =
            builder_.createLoadFrameInst(getParentOuterScope_, v);
        LPI->replaceAllUsesWith(loadReplacement);
        destroyer.add(LPI);
        continue;
      }

      if (auto *CBI = llvh::dyn_cast<CallBuiltinInst>(&I)) {
        // copyRestArgs (and its FastArray-returning variant) references the
        // local register stack of the inner function, to scan and copy the
        // parameters. However, we have transferred the parameters of the
        // inner function to the outer function. So we should also transfer
        // the call to the outer function.
        BuiltinMethod::Enum bi = CBI->getBuiltinIndex();
        if (bi == BuiltinMethod::HermesBuiltin_copyRestArgs ||
            bi == BuiltinMethod::HermesBuiltin_copyRestArgsFast) {
          Variable *outerRestArgs = builder_.createVariable(
              getParentOuterScope_->getVariableScope(),
              CBI->getName(),
              CBI->getType(),
              /* hidden */ false);
          moveBuilderTo(CBI, builder_);
          auto *replacement =
              builder_.createLoadFrameInst(getParentOuterScope_, outerRestArgs);
          CBI->replaceAllUsesWith(replacement);
          auto *outerCBI = beforeCGI.createCallBuiltinInst(
              bi,
              beforeCGI.getLiteralNumber(
                  llvh::cast<LiteralNumber>(CBI->getArgument(1))->getValue()));
          beforeCGI.createStoreFrameInst(
              newOuterScope_, outerCBI, outerRestArgs);
          destroyer.add(CBI);
        }
        continue;
      }

      if (auto *CAI = llvh::dyn_cast<CreateArgumentsInst>(&I)) {
        // Create a CreateArgumentsInst in outer_, write it to a variable in
        // outer_, and replace usages here with a read to the variable.
        Variable *argumentsVar = beforeCGI.createVariable(
            getParentOuterScope_->getVariableScope(),
            CAI->getName(),
            Type::createObject(),
            /* hidden */ false);
        auto *argInstInOuter = llvh::isa<CreateArgumentsLooseInst>(CAI)
            ? static_cast<CreateArgumentsInst *>(
                  beforeCGI.createCreateArgumentsLooseInst())
            : beforeCGI.createCreateArgumentsStrictInst();
        beforeCGI.createStoreFrameInst(
            newOuterScope_, argInstInOuter, argumentsVar);
        moveBuilderTo(CAI, builder_);
        CAI->replaceAllUsesWith(
            builder_.createLoadFrameInst(getParentOuterScope_, argumentsVar));
        destroyer.add(CAI);
        continue;
      }
    }
  }

  // This enables us to recognize when we encounter a given TerminatorInst user
  // for the first time.
  llvh::DenseSet<TerminatorInst *> processedTerminators;
  for (AllocStackInst *ASI : allocStacks) {
    auto *outerVar = builder_.createVariable(
        getParentOuterScope_->getVariableScope(),
        ASI->getVariableName(),
        ASI->getType(),
        /* hidden */ false);
    // Users of AllocStackInst must take AllocStackInst directly as they write
    // to it. To deal with this, create a 'dummy' AllocStackInst for each user
    // of the original AllocStackInst. Once it's done writing to the dummy,
    // propagate the value back to the outer_ variable. Users can also read the
    // stack variable, so initialze the dummy alloc stack to the value of the
    // outer_ variable.
    for (Instruction *user : ASI->getUsers()) {
      moveBuilderTo(user, builder_);
      // We can use simpler code paths for LoadStack and StoreStack, which
      // only read or only write to the stack space, respectively.
      if (auto *LSI = llvh::dyn_cast<LoadStackInst>(user)) {
        auto *loadFrameReplacement =
            builder_.createLoadFrameInst(getParentOuterScope_, outerVar);
        LSI->replaceAllUsesWith(loadFrameReplacement);
        destroyer.add(LSI);
      } else if (auto *SSI = llvh::dyn_cast<StoreStackInst>(user)) {
        auto *storeFrameReplacement = builder_.createStoreFrameInst(
            getParentOuterScope_, SSI->getValue(), outerVar);
        SSI->replaceAllUsesWith(storeFrameReplacement);
        destroyer.add(SSI);
      } else {
        auto *dummyAlloc = builder_.createAllocStackInst(
            ASI->getVariableName(), ASI->getType());
        builder_.createStoreStackInst(
            builder_.createLoadFrameInst(getParentOuterScope_, outerVar),
            dummyAlloc);
        if (auto *TI = llvh::dyn_cast<TerminatorInst>(user)) {
          bool seenBefore = !processedTerminators.insert(TI).second;
          writebackTerminator(TI, dummyAlloc, outerVar, seenBefore);
        } else {
          moveBuilderAfter(user, builder_);
          auto *writtenStackVal = builder_.createLoadStackInst(dummyAlloc);
          builder_.createStoreFrameInst(
              getParentOuterScope_, writtenStackVal, outerVar);
        }
        user->replaceFirstOperandWith(ASI, dummyAlloc);
      }
    }
    destroyer.add(ASI);
  }
}

void LowerToStateMachine::writebackTerminator(
    TerminatorInst *TI,
    AllocStackInst *dummyAlloc,
    Variable *outerVar,
    bool seenBefore) {
  // For a given successor BB, find its replacement BB that performs the
  // writeback and branches to the original successor.
  llvh::SmallDenseMap<BasicBlock *, BasicBlock *> replacementBBs;
  for (size_t i = 0, e = TI->getNumSuccessors(); i < e; ++i) {
    BasicBlock *origSuccBB = TI->getSuccessor(i);
    BasicBlock *updateVarBB = origSuccBB;
    if (!seenBefore) {
      // First time this terminator is seen, create the blocks.
      auto &replacementBB = replacementBBs[origSuccBB];
      if (replacementBB) {
        // If the same BB in different successor entries, we want to update the
        // BB used in the terminator for that successor, but we don't need to do
        // anything else since the replacement BB has already been created and
        // initialized with the proper IR.
        TI->setSuccessor(i, replacementBB);
        continue;
      }
      replacementBB = builder_.createBasicBlock(inner_);
      builder_.setInsertionBlock(replacementBB);
      builder_.createBranchInst(origSuccBB);
      TI->setSuccessor(i, replacementBB);
      updateVarBB = replacementBB;
      updateIncomingPhiValues(origSuccBB, TI->getParent(), replacementBB);
    }
    builder_.setInsertionPoint(&*updateVarBB->begin());
    auto *writtenStackVal = builder_.createLoadStackInst(dummyAlloc);
    builder_.createStoreFrameInst(
        getParentOuterScope_, writtenStackVal, outerVar);
  }
}

void LowerToStateMachine::lowerResumeGenerator(
    LoadParamInst *actionParam,
    LoadParamInst *valueParam,
    Variable *genState) {
  IRBuilder::InstructionDestroyer destroyer{};
  // This variable holds the value to return if the user executed .return().
  // Store this in a variable because it potentially needs to persist across
  // multiple generator invocations.
  Variable *valueToReturn = builder_.createVariable(
      getParentOuterScope_->getVariableScope(),
      "return_value",
      Type::createAnyType(),
      /* hidden */ true);
  for (BasicBlock &BB : *inner_) {
    for (auto iter = BB.begin(), end = BB.end(); iter != end; ++iter) {
      Instruction *I = &*iter;
      auto *RGI = llvh::dyn_cast<ResumeGeneratorInst>(I);
      if (!RGI)
        continue;
      // All of the instructions we generate here should contain the same
      // location information as RGI.
      builder_.setLocation(RGI->getLocation());
      // RGI checks: if .throw(valueParam) was called, then throw valueParam.
      // We are going to split the current basic block at this RGI, and put
      // the check for .throw at the end of this block.
      ++iter;
      auto *throwBlockBB = builder_.createBasicBlock(inner_);
      auto *restOfInstsBB = splitBasicBlock(&BB, iter);

      // Now put the check at the end of this block.
      builder_.setInsertionBlock(&BB);
      auto *isThrow = builder_.createBinaryOperatorInst(
          actionParam,
          builder_.getLiteralNumber((uint8_t)Action::Throw),
          ValueKind::BinaryStrictlyEqualInstKind);
      builder_.createCondBranchInst(isThrow, throwBlockBB, restOfInstsBB);

      builder_.setInsertionBlock(throwBlockBB);
      builder_.createStoreFrameInst(
          getParentOuterScope_,
          builder_.getLiteralNumber((uint8_t)State::Completed),
          genState);
      builder_.createThrowInst(valueParam);

      // Else, RGI takes an AllocStackInst and writes a boolean value to it.
      // Write true iff the method called on the generator was
      // .return(valueParam). The return value of RGI is valueParam.
      builder_.setInsertionPoint(&*restOfInstsBB->begin());
      builder_.createStoreFrameInst(
          getParentOuterScope_, valueParam, valueToReturn);
      auto *loadReturnVar =
          builder_.createLoadFrameInst(getParentOuterScope_, valueToReturn);
      RGI->replaceAllUsesWith(loadReturnVar);
      auto *isReturn = builder_.createFCompareInst(
          ValueKind::FEqualInstKind,
          actionParam,
          builder_.getLiteralNumber((uint8_t)Action::Return));
      builder_.createStoreStackInst(
          isReturn, llvh::cast<AllocStackInst>(RGI->getIsReturn()));
      destroyer.add(RGI);
      // There can't be anymore ResumeGeneratorInsts in this BB, so skip to
      // the next one.
      break;
    }
  }
}

/// Helper class for creating a SwitchInst.
class SwitchBuilder {
  IRBuilder &builder_;
  /// Values the switch index can take.
  SwitchInst::ValueListType values_{};
  /// Branches of the switch.
  SwitchInst::BasicBlockListType switchTargets_{};
  /// BB -> value in the switch that leads to it.
  llvh::DenseMap<BasicBlock *, size_t> bbMapping_{};

 public:
  explicit SwitchBuilder(IRBuilder &builder) : builder_(builder) {}

  /// \return the index \p BB occupies in the switch. If \p BB is not in the
  /// switch, then add it.
  size_t getBBIdx(BasicBlock *BB) {
    auto idx = bbMapping_.size();
    auto [pair, didInsert] = bbMapping_.try_emplace(BB, idx);
    if (didInsert) {
      values_.push_back(builder_.getLiteralNumber(idx));
      switchTargets_.push_back(BB);
    }
    return pair->second;
  }

  /// Construct and \return the IR for the switch.
  /// \p operand *must* be a hit on valid on a switch case.
  TerminatorInst *generate(Value *operand) {
    assert(!values_.empty() && "empty switch");
    // A single case is equivalent to an unconditional branch.
    if (values_.size() == 1) {
      return builder_.createBranchInst(switchTargets_[0]);
    } else {
      // We only need to perform numCases-1 comparisons. If we have checked the
      // first numCases-1 cases and have found no match, we can unconditionally
      // branch to the last case. We use the default block mechanism in the
      // SwitchInst to do this.
      return builder_.createSwitchInst(
          operand,
          switchTargets_.back(),
          llvh::ArrayRef<Literal *>{values_}.drop_back(1),
          llvh::ArrayRef<BasicBlock *>{switchTargets_}.drop_back(1));
    }
  }

  /// \return number of cases in the switch.
  size_t size() const {
    return values_.size();
  }
};

void LowerToStateMachine::lowerToSwitch(
    LoadParamInst *actionParam,
    LoadParamInst *valueParam,
    Variable *genState) {
  // Collect the original user blocks, before we created any.
  llvh::SmallVector<BasicBlock *, 6> userBBs;
  for (auto &BB : *inner_) {
    userBBs.push_back(&BB);
  }

  // Before we modify anything, record the relationship of each block to its
  // enclosing try.
  auto blockToEnclosingTry = findEnclosingTrysPerBlock(inner_);

  // This is the main switch that will contain all target blocks of
  // SaveAndYieldInsts and all catch handlers of a try-catch.
  SwitchBuilder mainSwitch(builder_);
  Variable *switchIdx = builder_.createVariable(
      getParentOuterScope_->getVariableScope(),
      "idx",
      Type::createInt32(),
      /* hidden */ true);

  // Original entry point to the function.
  auto *oldBeginBB = &*inner_->begin();
  // New entry point to the function.
  auto *newBeginBB = builder_.createBasicBlock(inner_);
  inner_->moveBlockToEntry(newBeginBB);
  // Execute the switch, potentially wrapped in a try.
  auto *executeSwitchBB = builder_.createBasicBlock(inner_);
  // Holds the switch that jumps to the correct user block after yields.
  auto *userCodeSwitchBB = builder_.createBasicBlock(inner_);

  // We begin by checking the current state of the generator.
  // If executing, throw.
  // Else, if the generator is completed, go to completedStateBB
  // Else, dispatch to switch.
  auto *throwBecauseExecutingBB = builder_.createBasicBlock(inner_);
  auto *checkIfCompletedBB = builder_.createBasicBlock(inner_);
  builder_.setInsertionBlock(newBeginBB);
  // Don't leak a user source line onto the synthetic entry dispatch; this
  // location persists through the scaffolding created below.
  builder_.setLocation(inner_->getSourceRange().Start);

  auto *loadState =
      builder_.createLoadFrameInst(getParentOuterScope_, genState);
  // Move these instructions to the new beginning block so that
  // they don't cross any blocks without dominance.
  valueParam->moveBefore(loadState);
  actionParam->moveBefore(loadState);

  // Note also that this must be before genState since it is an operand.
  getParentOuterScope_->moveBefore(loadState);

  auto *isExecuting = builder_.createBinaryOperatorInst(
      loadState,
      builder_.getLiteralNumber((uint8_t)State::Executing),
      ValueKind::BinaryStrictlyEqualInstKind);
  builder_.createCondBranchInst(
      isExecuting, throwBecauseExecutingBB, checkIfCompletedBB);

  // Transfer the EvalCompilationDataInst from the old beginning BB, if it
  // exists.
  if (auto *evalDataInst =
          llvh::dyn_cast<EvalCompilationDataInst>(&oldBeginBB->front())) {
    evalDataInst->moveBefore(&newBeginBB->front());
  }

  builder_.setInsertionBlock(throwBecauseExecutingBB);
  builder_.createStoreFrameInst(
      getParentOuterScope_,
      builder_.getLiteralNumber((uint8_t)State::Completed),
      genState);
  builder_.createThrowTypeErrorInst(builder_.getLiteralString(
      "Generator functions may not be called on executing generators"));

  auto *completedStateBB = createCompletedStateBlock(actionParam, valueParam);
  // If the state of the generator is completed, go to completedStateBB. Else,
  // try to execute the main switch.
  builder_.setInsertionBlock(checkIfCompletedBB);
  auto *isCompleted = builder_.createBinaryOperatorInst(
      builder_.createLoadFrameInst(getParentOuterScope_, genState),
      builder_.getLiteralNumber((uint8_t)State::Completed),
      ValueKind::BinaryStrictlyEqualInstKind);
  builder_.createCondBranchInst(isCompleted, completedStateBB, executeSwitchBB);

  // Initialize the main switch index and generator state variables.
  builder_.setInsertionPoint(CGI_);
  // oldBeginBB is where the original first BB instructions are. This should
  // be the first case that is executed, so initialize the switch index
  // variable to the correct index.
  // Use createGeneratorScopeArg_ here since we are emitting this IR in the
  // outer function.
  builder_.createStoreFrameInst(
      newOuterScope_,
      builder_.getLiteralNumber(mainSwitch.getBBIdx(oldBeginBB)),
      switchIdx);
  builder_.createStoreFrameInst(
      newOuterScope_,
      builder_.getLiteralNumber((uint8_t)State::SuspendedStart),
      genState);

  // This switch will be executed when an exception is thrown and is responsible
  // for setting up switchIdx to point to the correct block for the user catch
  // handler, or to just rethrow the exception if there is no user catch block.
  SwitchBuilder exceptionSwitch(builder_);
  // The top level catch will propagate the caught error to the user
  // handlers via this value.
  Variable *thrownValPlaceholder = builder_.createVariable(
      getParentOuterScope_->getVariableScope(),
      "catchVal",
      Type::createAnyType(),
      /* hidden */ true);
  // The default case in the exception handler switch is to simply re-throw the
  // exception. Also mark the generator as completed.
  BasicBlock *rethrowBB = builder_.createBasicBlock(inner_);
  builder_.setInsertionBlock(rethrowBB);
  builder_.createStoreFrameInst(
      getParentOuterScope_,
      builder_.getLiteralNumber((uint8_t)State::Completed),
      genState);
  auto *loadVal =
      builder_.createLoadFrameInst(getParentOuterScope_, thrownValPlaceholder);
  builder_.createThrowInst(loadVal);
  size_t rethrowBBIdx = exceptionSwitch.getBBIdx(rethrowBB);
  builder_.setInsertionPoint(CGI_);
  // Initialize the exception index to rethrow idx, which is equivalent to
  // being outside of any try body.
  Variable *exceptionSwitchIdx = builder_.createVariable(
      getParentOuterScope_->getVariableScope(),
      "exception_handler_idx",
      Type::createInt32(),
      /* hidden */ true);
  builder_.createStoreFrameInst(
      newOuterScope_,
      builder_.getLiteralNumber(rethrowBBIdx),
      exceptionSwitchIdx);
  // Wrap execution of switch in a try.
  builder_.setInsertionBlock(executeSwitchBB);
  // This BB guards the entire execution of user blocks.
  BasicBlock *surroundingCatchBB = builder_.createBasicBlock(inner_);
  builder_.createTryStartInst(userCodeSwitchBB, surroundingCatchBB);

  // This holds the mapping of user error handler to the exception switch index
  // that will lead to the execution of that handler.
  llvh::DenseMap<BasicBlock *, size_t> handlersToExceptionSwitchIdx;
  // Get the index in handlersToExceptionSwitchIdx that would execute \p BB, or
  // return rethrowBBIdx if \p BB is not enclosed in any try.
  auto getEnclosingHandlerIdx = [rethrowBBIdx,
                                 &blockToEnclosingTry,
                                 &handlersToExceptionSwitchIdx](
                                    BasicBlock *BB) {
    // Check to see if BB is enclosed in a try. If so, the index is the user
    // handler of the enclosing TryStartInst. If not, use the index for the
    // re-throwing block.
    size_t idx = rethrowBBIdx;
    auto enclosing = blockToEnclosingTry->find(BB);
    if (enclosing != blockToEnclosingTry->end() && enclosing->second) {
      idx = handlersToExceptionSwitchIdx[enclosing->second->getCatchTarget()];
    }
    return idx;
  };

  // Emit IR to return an iteration result object, potentially wrapped in a
  // block with a TryEnd. builder_ should be set in the desired location for the
  // return to be emitted before calling.
  auto emitInnerReturn = [this, surroundingCatchBB](
                             Value *val, bool isDelegated, bool done) {
    // If there are existing trys, then end the try in a separate BB before we
    // emit the return instruction.
    if (surroundingCatchBB) {
      auto *nextBlock = builder_.createBasicBlock(inner_);
      builder_.createTryEndInst(surroundingCatchBB, nextBlock);
      builder_.setInsertionBlock(nextBlock);
    }
    if (isDelegated) {
      // Delegated yields (yield*) should not wrap the given value in an
      // iteration object.
      builder_.createReturnInst(val);
    } else {
      emitReturnIterResultObject(builder_, val, done);
    }
  };

  IRBuilder::InstructionDestroyer destroyer{};
  for (auto *BB : userBBs) {
    for (Instruction &I : *BB) {
      if (auto *YI = llvh::dyn_cast<SaveAndYieldInst>(&I)) {
        // Return a value, and set switchIdx to jump to the next block
        // specified in the parameter of SaveAndYieldInst.
        auto *nextBB = YI->getNextBlock();
        // We want to execute nextBB via the switch on the next function
        // invocation. Add nextBB to the switch, and set the switch index
        // variable correctly.
        size_t idx = mainSwitch.getBBIdx(nextBB);
        moveBuilderTo(YI, builder_);
        builder_.createStoreFrameInst(
            getParentOuterScope_, builder_.getLiteralNumber(idx), switchIdx);
        builder_.createStoreFrameInst(
            getParentOuterScope_,
            builder_.getLiteralNumber((uint8_t)State::SuspendedYield),
            genState);
        emitInnerReturn(
            YI->getResult(),
            /*isDelegated*/ YI->getIsDelegated(),
            /*done*/ false);
        destroyer.add(YI);
      } else if (auto *TSI = llvh::dyn_cast<TryStartInst>(&I)) {
        auto *userCatchBB = TSI->getCatchTarget();
        // Replace the catch with a read from the variable holding the thrown
        // value.
        auto *catchInst = llvh::cast<CatchInst>(&*userCatchBB->begin());
        moveBuilderTo(catchInst, builder_);
        catchInst->replaceAllUsesWith(builder_.createLoadFrameInst(
            getParentOuterScope_, thrownValPlaceholder));
        destroyer.add(catchInst);
        // Entering into the catch handler changes what handler should be
        // called if an exception is thrown.
        builder_.createStoreFrameInst(
            getParentOuterScope_,
            builder_.getLiteralNumber(getEnclosingHandlerIdx(userCatchBB)),
            exceptionSwitchIdx);

        // This block will be dispatched to from the top level catch switch. It
        // is responsible for setting up the main switch index to point to the
        // correct user catch handler.
        auto *setupUserHandlerBB = builder_.createBasicBlock(inner_);
        builder_.setInsertionBlock(setupUserHandlerBB);
        builder_.createStoreFrameInst(
            getParentOuterScope_,
            builder_.getLiteralNumber(mainSwitch.getBBIdx(userCatchBB)),
            switchIdx);
        builder_.createBranchInst(executeSwitchBB);
        size_t setupBBIdx = exceptionSwitch.getBBIdx(setupUserHandlerBB);
        handlersToExceptionSwitchIdx.insert({userCatchBB, setupBBIdx});

        // The try body should update the exception switch index to point at
        // setupUserHandlerBB.
        auto *tryBody = TSI->getTryBody();
        movePastFirstInBlock(builder_, tryBody);
        builder_.createStoreFrameInst(
            getParentOuterScope_,
            builder_.getLiteralNumber(setupBBIdx),
            exceptionSwitchIdx);

        // Turn the TryStartInst into an unconditional branch to the try body.
        moveBuilderTo(TSI, builder_);
        auto *branch = builder_.createBranchInst(tryBody);
        TSI->replaceAllUsesWith(branch);
        destroyer.add(TSI);
      } else if (auto *TEI = llvh::dyn_cast<TryEndInst>(&I)) {
        // TryEndInst needs to update the exception switch index because it
        // changes what handler should be invoked if an exception were to be
        // thrown.
        moveBuilderTo(TEI, builder_);
        builder_.createStoreFrameInst(
            getParentOuterScope_,
            builder_.getLiteralNumber(
                getEnclosingHandlerIdx(TEI->getBranchDest())),
            exceptionSwitchIdx);
        builder_.createBranchInst(TEI->getBranchDest());
        destroyer.add(TEI);
      } else if (auto *RI = llvh::dyn_cast<ReturnInst>(&I)) {
        // ReturnInsts from IRGen have special semantics: they are meant to
        // complete the generator.
        moveBuilderTo(RI, builder_);
        builder_.createStoreFrameInst(
            getParentOuterScope_,
            builder_.getLiteralNumber((uint8_t)State::Completed),
            genState);
        emitInnerReturn(
            RI->getValue(),
            /*isDelegated*/ false,
            /*done*/ true);
        destroyer.add(RI);
      }
    }
  }

  builder_.setInsertionBlock(userCodeSwitchBB);
  // The user-block loop above left a user source line on the builder; reset it
  // so the synthetic dispatch/catch scaffolding isn't taken for user code.
  builder_.setLocation(inner_->getSourceRange().Start);
  builder_.createStoreFrameInst(
      getParentOuterScope_,
      builder_.getLiteralNumber((uint8_t)State::Executing),
      genState);
  mainSwitch.generate(
      builder_.createLoadFrameInst(getParentOuterScope_, switchIdx));

  // Set the thrown value placeholder variable and execute the switch.
  builder_.setInsertionBlock(surroundingCatchBB);
  auto *catchVal = builder_.createCatchInst();
  builder_.createStoreFrameInst(
      getParentOuterScope_, catchVal, thrownValPlaceholder);
  exceptionSwitch.generate(
      builder_.createLoadFrameInst(getParentOuterScope_, exceptionSwitchIdx));
}

BasicBlock *LowerToStateMachine::createCompletedStateBlock(
    LoadParamInst *actionParam,
    LoadParamInst *valueParam) {
  // The done state functionally replicates ResumeGeneratorInst.
  // 1. If the method called was .throw(X), then throw X
  // 2. Else if the method called was .return(X), then return X
  // 3. Else, return undefined

  auto *checkIsThrowBB = builder_.createBasicBlock(inner_);
  auto *throwValueBB = builder_.createBasicBlock(inner_);
  auto *checkIsReturnBB = builder_.createBasicBlock(inner_);
  auto *returnValueBB = builder_.createBasicBlock(inner_);
  auto *returnUndefBB = builder_.createBasicBlock(inner_);

  builder_.setInsertionBlock(checkIsThrowBB);
  auto *isThrow = builder_.createBinaryOperatorInst(
      actionParam,
      builder_.getLiteralNumber((uint8_t)Action::Throw),
      ValueKind::BinaryStrictlyEqualInstKind);
  builder_.createCondBranchInst(isThrow, throwValueBB, checkIsReturnBB);

  builder_.setInsertionBlock(checkIsReturnBB);
  auto *isReturn = builder_.createBinaryOperatorInst(
      actionParam,
      builder_.getLiteralNumber((uint8_t)Action::Return),
      ValueKind::BinaryStrictlyEqualInstKind);
  builder_.createCondBranchInst(isReturn, returnValueBB, returnUndefBB);

  builder_.setInsertionBlock(returnUndefBB);
  emitReturnIterResultObject(builder_, builder_.getLiteralUndefined(), true);

  builder_.setInsertionBlock(returnValueBB);
  emitReturnIterResultObject(builder_, valueParam, true);

  builder_.setInsertionBlock(throwValueBB);
  builder_.createThrowInst(valueParam);

  return checkIsThrowBB;
}

void LowerToStateMachine::moveCrossingValuesToOuter() {
  DominanceInfo D(inner_);
  // Move PhiInsts first. That promotion can potentially localize the
  // lifetime of a value to a single BB, obviating the need to lift it
  // additionally into the outer scope in this pass.
  moveInnerPhisToOuter(D);

  // Only spill CreateScopeInst under full debug info. This is to ensure they
  // can still be reached in debugging.
  bool forceSpillCreateScope =
      M_->getContext().getDebugInfoSetting() == DebugInfoSetting::ALL;
  for (BasicBlock &BB : *inner_) {
    for (Instruction &I : BB) {
      // Store the illegal users separately as we cannot modify the users list
      // as we iterate it.
      llvh::SmallVector<Instruction *, 2> illegalUsers;
      for (const auto &user : I.getUsers()) {
        if (llvh::isa<PhiInst>(user)) {
          // Phis were already processed.
          continue;
        }
        BasicBlock *userBB = user->getParent();
        if (userBB != &BB && !D.properlyDominates(&BB, userBB)) {
          illegalUsers.push_back(user);
        }
      }
      if (!(forceSpillCreateScope && llvh::isa<CreateScopeInst>(I)) &&
          illegalUsers.empty())
        continue;

      // The current instruction is used across a BB it does not dominate. Store
      // the value of the instruction into the environment, and replace the
      // illegal usages with a read.
      Variable *storedValueOfI = builder_.createVariable(
          getParentOuterScope_->getVariableScope(),
          I.getName(),
          I.getType(),
          /* hidden */ false);
      // This is a small optimization to reduce the number of reads to the
      // replacement variable we need to create for each illegal user. Illegal
      // users that reside in the same block can all share the same single read
      // from the variable.
      llvh::DenseMap<BasicBlock *, LoadFrameInst *> loadFramePerBlock;
      moveBuilderAfter(&I, builder_);
      builder_.createStoreFrameInst(getParentOuterScope_, &I, storedValueOfI);
      // If this is a CreateScopeInst and the create scope has a valid ID
      // associated with it, we must update the value in the Function's
      // environment list to be the spilled Variable.
      if (llvh::isa<CreateScopeInst>(I) &&
          I.getEnvironmentID() >= Instruction::kFirstScopeCreationIdIndex) {
        inner_->environments()[I.getEnvironmentIDAsIndex()] = storedValueOfI;
      }
      for (const auto &user : illegalUsers) {
        auto *userBB = user->getParent();
        LoadFrameInst *&load = loadFramePerBlock[userBB];
        if (!load) {
          movePastFirstInBlock(builder_, userBB);
          load = builder_.createLoadFrameInst(
              getParentOuterScope_, storedValueOfI);
        }
        moveBuilderTo(user, builder_);
        user->replaceFirstOperandWith(&I, load);
      }
    }
  }
}

/// \return true if an entry BasicBlock in \p PI is not a predecessor of
/// the BasicBlock \p PI resides in, or a Value operand no longer dominates
/// \p PI.
static bool shouldMovePhiInst(
    PhiInst *PI,
    const llvh::DenseSet<BasicBlock *> &predBBs,
    const DominanceInfo &D) {
  for (size_t i = 0, e = PI->getNumEntries(); i < e; ++i) {
    auto [val, valBB] = PI->getEntry(i);
    if (!predBBs.count(valBB)) {
      return true;
    }
    if (auto *valInst = llvh::dyn_cast<Instruction>(val);
        valInst && !D.properlyDominates(valInst, PI)) {
      return true;
    }
  }
  return false;
}

void LowerToStateMachine::moveInnerPhisToOuter(DominanceInfo &D) {
  IRBuilder::InstructionDestroyer destroyer{};
  for (BasicBlock &BB : *inner_) {
    llvh::DenseSet<BasicBlock *> predBBs;
    predBBs.insert(pred_begin(&BB), pred_end(&BB));
    for (Instruction &I : BB) {
      if (auto *PI = llvh::dyn_cast<PhiInst>(&I)) {
        if (!shouldMovePhiInst(PI, predBBs, D))
          continue;
        auto outerVar = builder_.createVariable(
            getParentOuterScope_->getVariableScope(),
            PI->getName(),
            PI->getType(),
            /* hidden */ false);
        for (size_t i = 0, e = PI->getNumEntries(); i < e; ++i) {
          // For each possible value that PhiInst can take, create a
          // corresponding storeFrameInst in the BB it needs to come from in
          // order to have that value. The PhiInst will then read from the
          // frame to obtain the value.
          const auto &[val, predBB] = PI->getEntry(i);
          moveBuilderTo(&predBB->back(), builder_);
          builder_.createStoreFrameInst(getParentOuterScope_, val, outerVar);
        }
        movePastFirstInBlock(builder_, PI->getParent());
        auto loadReplacement =
            builder_.createLoadFrameInst(getParentOuterScope_, outerVar);
        PI->replaceAllUsesWith(loadReplacement);
        destroyer.add(PI);
      }
    }
  }
}
} // namespace

/// \return the CreateGeneratorInst in the given \p outer generator or async
/// function, or nullptr if there is none. In the case of a generator
/// function which is unreachable, the inner function may have been deleted
/// because it will never be created.
static CreateGeneratorInst *findCreateGenerator(Function *outer) {
  for (BasicBlock &BB : *outer) {
    for (Instruction &I : BB) {
      if (auto *CGI = llvh::dyn_cast<CreateGeneratorInst>(&I)) {
        return CGI;
      }
    }
  }
  return nullptr;
}

Pass *createLowerGeneratorFunction() {
  /// Process all GeneratorFunctions, AsyncFunctions and
  /// GeneratorInnerFunctions. Turn them into regular function IR, removing all
  /// generator-specific IR.
  class LowerGeneratorFunction : public ModulePass {
   public:
    explicit LowerGeneratorFunction() : ModulePass("LowerGeneratorFunction") {}
    ~LowerGeneratorFunction() override = default;
    bool runOnModule(Module *M) override {
      // This lowering pass may be inserted both in optimization and lowering.
      // In that case, we shoud simply return early.
      if (M->areGeneratorsLowered())
        return false;
      bool changed = false;
      for (auto &F : *M) {
        if (!llvh::isa<GeneratorFunction>(F) && !llvh::isa<AsyncFunction>(F))
          continue;
        if (auto *inner = findCreateGenerator(&F)) {
          LowerToStateMachine(M, &F, inner).convert();
          changed = true;
        }
      }
      M->setGeneratorsLowered(true);
      return changed;
    }
  };
  return new LowerGeneratorFunction();
}
} // namespace hermes
