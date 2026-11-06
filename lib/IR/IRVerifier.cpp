/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/IR/IRVerifier.h"

#include "hermes/IR/CFG.h"
#include "hermes/IR/IRVisitor.h"
#include "hermes/IR/Instrs.h"

#include "hermes/Utils/Dumper.h"
#include "llvh/ADT/DenseMap.h"
#include "llvh/Support/Casting.h"

using llvh::dyn_cast;
using llvh::isa;
using llvh::raw_null_ostream;

using namespace hermes;

#ifdef HERMES_SLOW_DEBUG

namespace {

/// IR Verifier - the verifier checks some basic properties of the IR to make
/// sure that it is not incorrect. For example, it checks that instructions are
/// dominated by their oparands and that the entry block has no predecessors.
///
/// The verifier also checks if the IR is in optimized form that allows bytecode
/// generation. For example, it checks that there are no unreachable basic
/// blocks.
class Verifier : public InstructionVisitor<Verifier, bool> {
  class FunctionState; // Forward declaration of inner class.

  const Module &M;
  Context *Ctx{nullptr};
  /// IR type context for the module being verified.
  TypeContext &typeCtx_;
  raw_ostream &OS;
  VerificationMode verificationMode;
  irdumper::IRPrinter &printer;
  /// State for the function currently being verified.
  FunctionState *functionState{};
  /// Cached users of each value, lazily computed.
  llvh::DenseMap<const Value *, llvh::DenseSet<const Value *>> cachedUsers{};

  /// Verifier state per-function is kept here.
  class FunctionState {
    Verifier *verifier;
    /// Save the previous state here so we can restore it in our destructor.
    FunctionState *oldState;

   public:
    const Function &function;
    bool createArgumentsEncountered = false;
    bool createScopeEncountered = false;

    FunctionState(Verifier *verifier, const Function &function)
        : verifier(verifier),
          oldState(verifier->functionState),
          function(function) {
      verifier->functionState = this;
    }

    ~FunctionState() {
      verifier->functionState = oldState;
    }
  };

  bool isVerifyingOptimalIR() {
    return verificationMode == VerificationMode::IR_OPTIMIZED;
  }

 public:
  explicit Verifier(
      const Module &M,
      raw_ostream &OS,
      VerificationMode mode,
      irdumper::IRPrinter &printer)
      : M(M),
        typeCtx_(M.getTypeContext()),
        OS(OS),
        verificationMode(mode),
        printer(printer) {}

  bool verify() {
    Ctx = &M.getContext();
    return visitModule(M);
  };

  /// Reimplement in Verifier class for custom behavior.
  LLVM_NODISCARD bool verifyBeforeVisitInstruction(const Instruction &I);

  LLVM_NODISCARD bool visitModule(const Module &M);
  LLVM_NODISCARD bool visitFunction(const Function &F);
  LLVM_NODISCARD bool visitBasicBlock(const BasicBlock &BB);
  LLVM_NODISCARD bool visitVariableScope(const VariableScope &VS);

  LLVM_NODISCARD bool visitBaseCreateLexicalChildInst(
      const BaseCreateLexicalChildInst &Inst);
  LLVM_NODISCARD bool visitBaseCreateCallableInst(
      const BaseCreateCallableInst &Inst);

  LLVM_NODISCARD bool visitCreateArgumentsInst(const CreateArgumentsInst &Inst);

#define DEF_VALUE(XX, PARENT) bool visit##XX(const XX &Inst);
#define BEGIN_VALUE(XX, PARENT) DEF_VALUE(XX, PARENT)
#include "hermes/IR/Instrs.def"

 private:
  /// Implement verification for a switch-like instruction.
  template <class T>
  LLVM_NODISCARD bool visitSwitchLikeInst(const T &Inst);
  /// Implement verification for a conditional branch-like instruction.
  template <class T>
  LLVM_NODISCARD bool visitCondBranchLikeInst(const T &Inst);
  /// Implement verification for a binaryoperator-like instruction.
  template <class T>
  LLVM_NODISCARD bool visitBinaryOperatorLikeInst(const T &Inst);

  /// \return a DenseSet of users for a Value, computed lazily.
  const llvh::DenseSet<const Value *> &getUsersSetForValue(const Value *V) {
    auto &users = cachedUsers[V];
    if (users.empty()) {
      users.insert(V->users_begin(), V->users_end());
    }
    return users;
  }

  /// Assert that the attributes of \p val are allowed to be set, based on the
  /// kind of \p val.
  LLVM_NODISCARD bool verifyAttributes(const Value *val);

  /// A helper function for the Assert macros that prints the location of the
  /// the problematic instruction, if available. It also prints the
  /// instruction/BB label.
  /// \param inst the optional instruction to print the
  /// location of.
  void _assertPrintLocation(const Instruction *inst);

  /// Verify that TryStart/EndInsts are balanced. Also, verify all Try bodies
  /// are reachable only through the enclosing TryStartInst.
  LLVM_NODISCARD bool verifyTryStructure(const Function &F);

  /// Helper function to get the label of a BasicBlock.
  llvh::format_object<unsigned> bbLabel(const BasicBlock &block) {
    // Ensure the namer has the correct per-function state set up correctly
    // before we ask it for the BB number.
    printer.namer_.restoreFunctionState(block.getParent());
    return llvh::format("%%BB%u", printer.namer_.getBBNumber(&block));
  }

  /// Helper function to get the label of an Instruction.
  llvh::format_object<unsigned> iLabel(const Instruction &I) {
    // Ensure the namer has the correct per-function state set up correctly
    // before we ask it for the inst number.
    printer.namer_.restoreFunctionState(I.getParent()->getParent());
    return llvh::format("%%%u", printer.namer_.getInstNumber(&I));
  }
};

void Verifier::_assertPrintLocation(const Instruction *inst) {
  const Function *f = nullptr;
  if (inst)
    f = inst->getParent()->getParent();
  else if (functionState)
    f = &functionState->function;

  if (f) {
    OS << " in function \"" << f->getInternalNameStr() << "\"";
    if (inst) {
      SourceErrorManager::SourceCoords coords{};
      if (Ctx->getSourceErrorManager().findBufferLineAndLoc(
              inst->getLocation(), coords)) {
        OS << ", ";
        Ctx->getSourceErrorManager().dumpCoords(OS, coords);
      }
    }
  }
  if (inst) {
    OS << " in " << bbLabel(*inst->getParent()) << ", " << iLabel(*inst);
    ;
  }
  OS << '\n';
}

#define ReturnIfNot(C) \
  do {                 \
    if (!(C)) {        \
      return false;    \
    }                  \
  } while (0)

/// TODO: Need to make this accept format strings
/// Check if \p C is false. If so, dump the module, forward the given
/// arguments to OS, print function information and return false in the
/// enclosing scope.
#define AssertWithMsg(C, ...)        \
  do {                               \
    if (!(C)) {                      \
      printer.visit(M);              \
      OS << __VA_ARGS__;             \
      _assertPrintLocation(nullptr); \
      return false;                  \
    }                                \
  } while (0)

/// This macro is the same as AssertWithMsg, but it will also append the
/// location information of the given instruction \p inst.
#define AssertIWithMsg(inst, C, ...)                  \
  do {                                                \
    if (!(C)) {                                       \
      printer.visit(M);                               \
      OS << inst.getKindStr() << ": " << __VA_ARGS__; \
      _assertPrintLocation(&inst);                    \
      return false;                                   \
    }                                                 \
  } while (0)

bool Verifier::verifyAttributes(const Value *val) {
  const auto &attrs = val->getAttributes(&M);
#define ATTRIBUTE(valueKind, name, _string) \
  if (attrs.name)                           \
    AssertWithMsg(                          \
        llvh::isa<valueKind>(val), #name " must be set on a " #valueKind);
#include "hermes/IR/Attributes.def"
  return true;
}

bool Verifier::visitModule(const Module &M) {
  // Verify all functions are valid
  for (Module::const_iterator I = M.begin(); I != M.end(); I++) {
    AssertWithMsg(
        I->getParent() == &M, "Function's parent does not match module");
    ReturnIfNot(visitFunction(*I));
  }
  for (auto &VS : M.getVariableScopes())
    ReturnIfNot(visitVariableScope(VS));
  // Ensure that we can compute the hash of a module successfully.
  (void)M.hash();
  return true;
}

bool Verifier::visitFunction(const Function &F) {
  AssertWithMsg(&F.getContext() == Ctx, "Function has wrong context");

  AssertWithMsg(verifyAttributes(&F), "Invalid function attributes");

  for (Value *newTargetUser : F.getNewTargetParam()->getUsers()) {
    AssertWithMsg(
        llvh::isa<GetNewTargetInst>(newTargetUser),
        "Only GetNewTargetInst may use the newTargetParam");
  }

  for (Instruction *user : F.getUsers()) {
    AssertIWithMsg(
        (*user),
        llvh::isa<BaseCallInst>(user) ||
            llvh::isa<BaseCreateLexicalChildInst>(user) ||
            llvh::isa<GetClosureScopeInst>(user) ||
            llvh::isa<CreateThisInst>(user),
        "Function can only be an operand to certain instructions");
  }

  FunctionState newFunctionState(this, F);

  // Verify all basic blocks are valid
  for (auto I = F.begin(); I != F.end(); I++) {
    AssertWithMsg(
        I->getParent() == &F,
        bbLabel(*I) << "'s parent does not match function");
    ReturnIfNot(visitBasicBlock(*I));
  }

  // Verify Dominance Tree is valid
  DominanceInfo D(const_cast<Function *>(&F));
  AssertWithMsg(
      D.getRoot() == &*F.begin(),
      "Root node in dominance tree should be the entry basic block");
  AssertWithMsg(
      pred_count(&*F.begin()) == 0,
      "The entry block should have no predecessors");
  for (const auto &I : F) {
    // Check for unreachable blocks.
    AssertWithMsg(
        D.isReachableFromEntry(&I),
        bbLabel(I) << " unreachable from entry in the Dominance Tree");

    // Domination checks within a block are linear, so for huge blocks we get
    // quadratic runtime. To avoid this, we store a set of instructions we've
    // seen so far in the current block.
    llvh::SmallPtrSet<const Instruction *, 32> seen;

    // Instruction dominance check
    for (BasicBlock::const_iterator II = I.begin(); II != I.end(); II++) {
      // Check that incoming phi node values are dominated in their incoming
      // blocks.
      if (auto *Phi = llvh::dyn_cast<PhiInst>(&*II)) {
        for (int i = 0, e = Phi->getNumEntries(); i < e; ++i) {
          auto pair = Phi->getEntry(i);
          BasicBlock *block = pair.second;
          auto *inst = llvh::dyn_cast<Instruction>(pair.first);

          // Non-instructions always dominate everything. Move on.
          if (!inst) {
            continue;
          }

          // Make sure that the incoming value dominates the incoming block.
          AssertIWithMsg(
              (*Phi),
              D.dominates(inst->getParent(), block),
              "Incoming PHI value " << bbLabel(*inst->getParent())
                                    << " must dominate incoming "
                                    << bbLabel(*block));
        }
        continue;
      }

      // Check that all instructions are dominated by their operands.
      for (unsigned i = 0; i < II->getNumOperands(); i++) {
        auto Operand = II->getOperand(i);
        if (auto *InstOp = llvh::dyn_cast<Instruction>(Operand)) {
          AssertWithMsg(
              seen.count(InstOp) || D.properlyDominates(InstOp, &*II),
              "Operand " << iLabel(*InstOp) << " must dominate the Instruction "
                         << iLabel(*II));
        }
      }
      seen.insert(&*II);
    }
  }

  // If we are supporting full debugging, then we require that all functions
  // have a CreateScopeInst.
  if (F.getContext().getDebugInfoSetting() == DebugInfoSetting::ALL &&
      !llvh::isa<GeneratorFunction>(F) && !llvh::isa<AsyncFunction>(F) &&
      !F.isLazy()) {
    AssertWithMsg(
        functionState->createScopeEncountered,
        "All non-generator, non-async functions need to have a "
        "CreateScopeInst");
  }

  return verifyTryStructure(F);
}

bool Verifier::verifyTryStructure(const Function &F) {
  // Stack of basic blocks to visit, and the TryStartInst on entry of that
  // block.
  llvh::SmallVector<std::pair<const BasicBlock *, const TryStartInst *>, 4>
      stack;
  // Holds a mapping from basic block -> innermost enclosing TryStartInst,
  // accounting for Catch/TryEnd.
  llvh::DenseMap<const BasicBlock *, const TryStartInst *> blockToEnclosingTry;

  stack.push_back({&*F.begin(), nullptr});
  blockToEnclosingTry[&*F.begin()] = nullptr;
  while (!stack.empty()) {
    auto [BB, enclosingTry] = stack.pop_back_val();
    // If this BB ends with a TryStartInst, store the try body block here.
    BasicBlock *tryBody = nullptr;

    if (llvh::isa<ReturnInst>(BB->getTerminator())) {
      AssertWithMsg(
          enclosingTry == nullptr,
          "ReturnInst " << iLabel(*BB->getTerminator()) << " but Try "
                        << iLabel(*enclosingTry) << " has not been closed.");
    } else if (auto *TSI = llvh::dyn_cast<TryStartInst>(BB->getTerminator())) {
      tryBody = TSI->getTryBody();
      AssertIWithMsg(
          (*TSI),
          llvh::isa<CatchInst>(TSI->getCatchTarget()->front()),
          "Catch target must start with a CatchInst.");
    } else if (auto *TEI = llvh::dyn_cast<TryEndInst>(BB->getTerminator())) {
      // If this block ends with a TryEnd, pop off to the nearest try.
      AssertIWithMsg(
          (*BB->getTerminator()),
          enclosingTry != nullptr,
          "ending a try outside of any TryStartInst context.");
      AssertIWithMsg(
          (*BB->getTerminator()),
          enclosingTry->getCatchTarget() == TEI->getCatchTarget(),
          "TryEndInst does not match TryStartInst.");
      assert(
          blockToEnclosingTry.find(enclosingTry->getParent()) !=
              blockToEnclosingTry.end() &&
          "enclosingTry should already be in map.");
      enclosingTry = blockToEnclosingTry[enclosingTry->getParent()];
    } else if (auto *BTI = llvh::dyn_cast<BaseThrowInst>(BB->getTerminator())) {
      if (BTI->hasCatchTarget()) {
        AssertIWithMsg(
            (*BB->getTerminator()),
            enclosingTry != nullptr,
            "throw with catch tarhet outside of any TryStartInst context.");
        assert(
            blockToEnclosingTry.find(enclosingTry->getParent()) !=
                blockToEnclosingTry.end() &&
            "throw's enclosingTry should already be in map.");
        enclosingTry = blockToEnclosingTry[enclosingTry->getParent()];
      }
    }

    for (auto *succ : successors(BB)) {
      // Only update the enclosing try when we are going to enter the try body.
      auto *succEnclosingTry = succ == tryBody
          ? llvh::cast<TryStartInst>(BB->getTerminator())
          : enclosingTry;

      // If BB already exists in blockToEnclosingTry and has a different
      // enclosing TryStartInst, error out.
      auto [enclosingInfo, inserted] =
          blockToEnclosingTry.insert({succ, succEnclosingTry});
      if (!inserted) {
        AssertWithMsg(
            enclosingInfo->second == succEnclosingTry,
            bbLabel(*succ)
                << " is reachable from multiple different TryStartInsts: "
                << iLabel(*succEnclosingTry) << " and "
                << iLabel(*enclosingInfo->second));
      } else {
        // Only expore this BB if we haven't visited it before.
        stack.push_back({succ, succEnclosingTry});
      }
    }
  }

  // Verify that the optional catch target is set correctly for all throws.
  for (auto &BB : F) {
    const TryStartInst *enclosingTry = blockToEnclosingTry.lookup(&BB);
    for (auto &I : BB) {
      auto *BTI = llvh::dyn_cast<BaseThrowInst>(&I);
      if (!BTI)
        continue;
      if (BTI->hasCatchTarget()) {
        AssertIWithMsg(
            (*BTI),
            enclosingTry != nullptr,
            "BaseThrowInst "
                << iLabel(*BTI)
                << " has catch target outside of any TryStartInst context.");
        AssertIWithMsg(
            (*BTI),
            enclosingTry &&
                enclosingTry->getCatchTarget() == BTI->getCatchTarget(),
            "BaseThrowInst " << iLabel(*BTI)
                             << " with incorrect catch target.");
      } else {
        AssertIWithMsg(
            (*BTI),
            enclosingTry == nullptr,
            "BaseThrowInst without a catch target "
                << iLabel(*BTI) << " inside a TryStartInst context.");
      }
    }
  }

  return true;
}

bool Verifier::visitBasicBlock(const BasicBlock &BB) {
  AssertWithMsg(&BB.getContext() == Ctx, bbLabel(BB) << "has wrong context");

  AssertWithMsg(BB.getTerminator(), bbLabel(BB) << " must have a terminator.");

  ReturnIfNot(verifyAttributes(&BB));

  // Verify the mutual predecessor/successor relationship
  for (auto I = succ_begin(&BB), E = succ_end(&BB); I != E; ++I) {
    AssertWithMsg(
        pred_contains(*I, &BB),
        "Cannot find self " << bbLabel(BB)
                            << "in the predecessors of a successor "
                            << bbLabel(**I));
  }
  for (auto I = pred_begin(&BB), E = pred_end(&BB); I != E; ++I) {
    AssertWithMsg(
        succ_contains(*I, &BB),
        "Cannot find self " << bbLabel(BB)
                            << " in the successors of a predecessor "
                            << bbLabel(**I));
  }

  // Verify each instruction
  for (BasicBlock::const_iterator I = BB.begin(); I != BB.end(); I++) {
    AssertWithMsg(
        I->getParent() == &BB,
        "Instruction " << iLabel(*I) << "'s parent " << bbLabel(*I->getParent())
                       << " does not match " << bbLabel(BB));

    // Use the instruction using the InstructionVisitor::visit();
    ReturnIfNot(verifyBeforeVisitInstruction(*I));
    ReturnIfNot(visit(*I));
  }
  return true;
}

bool Verifier::visitVariableScope(const hermes::VariableScope &VS) {
  for (auto *U : VS.getUsers()) {
    auto *CSI = llvh::dyn_cast<CreateScopeInst>(U);
    if (!CSI)
      continue;

    // Found a creation of this scope, retrieve the expected value of the
    // parent, if possible.
    VariableScope *curParentVS;
    if (auto *parentScope =
            llvh::dyn_cast<BaseScopeInst>(CSI->getParentScope()))
      curParentVS = parentScope->getVariableScope();
    else if (llvh::isa<EmptySentinel>(CSI->getParentScope()))
      curParentVS = nullptr;
    else
      continue;

    AssertWithMsg(
        VS.getParentScope() == curParentVS,
        "VariableScope has multiple different parents.");
  }

  // Check that every variable with a load has at least one store.
  // NOTE: Don't run this in IR_LOWERED because OptEnvironmentInit breaks this
  // assumption.
  if (verificationMode != VerificationMode::IR_LOWERED) {
    for (auto *var : VS.getVariables()) {
      bool hasLoad = false;
      bool hasStore = false;
      for (auto *varUser : VS.getUsers()) {
        hasLoad |= llvh::isa<LoadFrameInst>(varUser);
        hasStore |= llvh::isa<StoreFrameInst>(varUser);
      }
      if (hasLoad) {
        AssertWithMsg(
            hasStore,
            "Variable " << var->getName()
                        << " must have a store for it to load");
      }
    }
  }

  return true;
}

bool Verifier::verifyBeforeVisitInstruction(const Instruction &Inst) {
  // TODO: Verify the instruction is valid, need switch/case on each
  // actual Instruction type
  AssertIWithMsg(Inst, &Inst.getContext() == Ctx, "Wrong context");

  AssertIWithMsg(
      Inst, Inst.getSideEffect().isWellFormed(), "Ill-formed side effects");

  if (Inst.getType().isNoType()) {
    AssertIWithMsg(Inst, !Inst.hasOutput(), "NoType instruction has output");
  } else {
    AssertIWithMsg(
        Inst, Inst.hasOutput(), "Instruction with type does not have output");
  }

  if (Inst.getSideEffect().getFirstInBlock()) {
    if (llvh::isa<PhiInst>(&Inst)) {
      // Phis must be first in block, but they may be preceded by other Phis.
      AssertIWithMsg(
          Inst,
          !Inst.getPrevNode() || llvh::isa<PhiInst>(Inst.getPrevNode()),
          "Phi can only be preceded by other Phis");
    } else {
      // All other FirstInBlock instructions must be first in the block.
      AssertIWithMsg(
          Inst,
          !Inst.getPrevNode(),
          "FirstInBlock instruction must be first in the block");
    }
  }

  ReturnIfNot(verifyAttributes(&Inst));

  if (Inst.getEnvironmentID() >= Instruction::kFirstScopeCreationIdIndex) {
    AssertIWithMsg(
        Inst,
        Inst.getEnvironmentIDAsIndex() <
            Inst.getFunction()->environments().size(),
        "Invalid environment index ID");
  }

  bool const acceptsEmptyType = Inst.acceptsEmptyType();

  // Verify all operands are valid
  for (unsigned i = 0; i < Inst.getNumOperands(); i++) {
    auto Operand = Inst.getOperand(i);
    AssertIWithMsg(Inst, Operand != nullptr, "Invalid operand");
    if (Operand->tracksUsers()) {
      AssertIWithMsg(
          Inst,
          getUsersSetForValue(Operand).count(&Inst) == 1,
          "This instruction is not in the User list of the operand");
    }
    if (llvh::isa<Variable>(Operand)) {
      AssertIWithMsg(
          Inst,
          llvh::isa<LoadFrameInst>(Inst) || llvh::isa<StoreFrameInst>(Inst) ||
              llvh::isa<LazyCompilationDataInst>(Inst) ||
              llvh::isa<EvalCompilationDataInst>(Inst),
          "Variable can only be accessed in "
          "LoadFrame/StoreFrame/LazyData/EvalData Inst.");
    }

    // Most instructions that accepts a stack operand must write to it. If it
    // strictly reads from the stack variable, it should go through a
    // LoadStackInst.
    if (llvh::isa<AllocStackInst>(Operand)) {
      AssertIWithMsg(
          Inst,
          llvh::isa<LoadStackInst>(Inst) ||
              Inst.getSideEffect().getWriteStack(),
          "Must write to stack operand.");
    }

    // Scope instructions should only be stored in places where they can be
    // analyzed and are invisible to user code.
    if (typeCtx_.canBeType(Operand->getType(), Type::createEnvironment())) {
      AssertIWithMsg(
          Inst,
          Operand->getType().isEnvironmentType(),
          "Environment should not be mixed with other types");
      AssertIWithMsg(
          Inst,
          llvh::isa<BaseCallInst>(Inst) || llvh::isa<CreateScopeInst>(Inst) ||
              llvh::isa<ResolveScopeInst>(Inst) ||
              llvh::isa<HBCCreateFunctionEnvironmentInst>(Inst) ||
              llvh::isa<HBCResolveParentEnvironmentInst>(Inst) ||
              llvh::isa<GetParentScopeInst>(Inst) ||
              llvh::isa<LIRResolveScopeInst>(Inst) ||
              llvh::isa<BaseCreateLexicalChildInst>(Inst) ||
              llvh::isa<LoadStackInst>(Inst) ||
              llvh::isa<StoreStackInst>(Inst) || llvh::isa<PhiInst>(Inst) ||
              llvh::isa<CreateClassInst>(Inst) ||
              llvh::isa<LoadFrameInst>(Inst) || llvh::isa<StoreFrameInst>(Inst),
          "Environments can only be an operand to certain instructions.");
    }

    if (typeCtx_.canBeEmpty(Operand->getType())) {
      AssertIWithMsg(
          Inst, acceptsEmptyType, "Instruction does not accept empty type");
    } else if (typeCtx_.canBeUninit(Operand->getType())) {
      AssertIWithMsg(
          Inst, acceptsEmptyType, "Instruction does not accept uninit type");
    }
  }

  // Verify that terminator instructions never need to return a value.
  if (llvh::isa<TerminatorInst>(Inst)) {
    AssertIWithMsg(
        Inst, Inst.getNumUsers() == 0, "Terminator Inst cannot return value.");
  }

  return true;
}

static bool isTerminator(const Instruction *Inst) {
  return &*Inst->getParent()->rbegin() == Inst;
}

//===----------------------------------------------------------------------===//
// Verifications for each individual instruction.

bool Verifier::visitReturnInst(const ReturnInst &Inst) {
  AssertIWithMsg(
      Inst,
      isTerminator(&Inst),
      "Return Instruction must be the last instruction of a basic block");
  AssertIWithMsg(
      Inst,
      Inst.getNumSuccessors() == 0,
      "ReturnInst should not have successors");
  return true;
}

bool Verifier::visitSaveAndYieldInst(const SaveAndYieldInst &Inst) {
  AssertIWithMsg(
      Inst, isTerminator(&Inst), "SaveAndYield must be a terminator");
  AssertIWithMsg(
      Inst,
      Inst.getParent()->getTerminator() == &Inst,
      "SaveAndYield must be the terminator");
  AssertIWithMsg(
      Inst,
      !M.areGeneratorsLowered(),
      "Should not exist after generators are lowered");
  return true;
}

bool Verifier::visitBranchInst(const BranchInst &Inst) {
  AssertIWithMsg(
      Inst,
      isTerminator(&Inst),
      "Branch Instruction must be the last instruction of a basic block");
  AssertIWithMsg(
      Inst,
      Inst.getNumSuccessors() == 1,
      "Branch Instruction should have only 1 successor");
  AssertIWithMsg(
      Inst,
      succ_contains(Inst.getParent(), Inst.getBranchDest()),
      "Branch Dest Basic Block does not match with successor pointer");
  AssertIWithMsg(
      Inst,
      pred_contains(Inst.getBranchDest(), Inst.getParent()),
      "BranchInst Basic Block not in the predecessor list of target block");
  return true;
}

bool Verifier::visitHBCResolveParentEnvironmentInst(
    const HBCResolveParentEnvironmentInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getParentScopeParam() == Inst.getFunction()->getParentScopeParam(),
      "parentScopeParam must be the JSDynamicParam in the Function.");
  return true;
}

bool Verifier::visitToPropertyKeyInst(const ToPropertyKeyInst &Inst) {
  return true;
}

bool Verifier::visitAsNumberInst(const AsNumberInst &Inst) {
  AssertIWithMsg(
      Inst,
      !isTerminator(&Inst),
      "Non-terminator cannot be the last instruction of a basic block");
  AssertIWithMsg(
      Inst,
      Inst.getType() == Type::createNumber(),
      "AsNumberInst must return a number type");
  return true;
}

bool Verifier::visitAsNumericInst(const AsNumericInst &Inst) {
  AssertIWithMsg(
      Inst,
      !isTerminator(&Inst),
      "Non-terminator cannot be the last instruction of a basic block");
  AssertIWithMsg(
      Inst,
      typeCtx_.isSubsetOf(Inst.getType(), Type::createNumeric()),
      "AsNumericInst must return a numeric type");
  return true;
}

bool Verifier::visitAsInt32Inst(const AsInt32Inst &Inst) {
  AssertIWithMsg(
      Inst,
      !isTerminator(&Inst),
      "Non-terminator cannot be the last instruction of a basic block");
  return true;
}

bool Verifier::visitAsUint32Inst(const AsUint32Inst &Inst) {
  return true;
}

bool Verifier::visitAddEmptyStringInst(const AddEmptyStringInst &Inst) {
  AssertIWithMsg(
      Inst,
      !isTerminator(&Inst),
      "Non-terminator cannot be the last instruction of a basic block");
  AssertIWithMsg(
      Inst,
      Inst.getType() == Type::createString(),
      "AddEmptyStringInst must return a string type");
  return true;
}

bool Verifier::visitCreatePrivateNameInst(const CreatePrivateNameInst &Inst) {
  AssertIWithMsg(
      Inst,
      llvh::isa<LiteralString>(Inst.getSingleOperand()),
      "CreatePrivateNameInst must take in a literal string");
  AssertIWithMsg(
      Inst,
      Inst.getType().isPrivateNameType(),
      "CreatePrivateNameInst must return a private name type");
  return true;
}

bool Verifier::visitAllocStackInst(const AllocStackInst &Inst) {
  AssertIWithMsg(
      Inst,
      &(Inst.getParent()->back()) != &Inst,
      "Alloca Instruction cannot be the last instruction of a basic block");
  bool hasLoad = false;
  bool hasStore = false;
  for (auto *user : Inst.getUsers()) {
    hasLoad |= user->getSideEffect().getReadStack();
    hasStore |= user->getSideEffect().getWriteStack();
  }

  // TODO: Make this check better by ensuring that there's a store
  // prior to the load for every possible path through the function.
  // This isn't dominance, because there may be two stores in separate
  // branches prior to the load.
  if (hasLoad)
    AssertIWithMsg(Inst, hasStore, "LoadStackInst must have a StoreStackInst");

  return true;
}

template <class T>
bool Verifier::visitCondBranchLikeInst(const T &Inst) {
  AssertIWithMsg(
      Inst,
      isTerminator(&Inst),
      "CondBranchInst must be the last instruction of a basic block");
  AssertIWithMsg(
      Inst,
      Inst.getNumSuccessors() == 2,
      "CondBranchInst should have 2 successors");
  AssertIWithMsg(
      Inst,
      succ_contains(Inst.getParent(), Inst.getTrueDest()),
      "True dest should be a successor of CondBranchInst block");
  AssertIWithMsg(
      Inst,
      pred_contains(Inst.getTrueDest(), Inst.getParent()),
      "CondBranchInst block should be a predecessor of true dest");
  AssertIWithMsg(
      Inst,
      succ_contains(Inst.getParent(), Inst.getFalseDest()),
      "False dest should be a successor of CondBranchInst block");
  AssertIWithMsg(
      Inst,
      pred_contains(Inst.getFalseDest(), Inst.getParent()),
      "CondBranchInst block should be a predecessor of false dest");
  return true;
}

bool Verifier::visitCondBranchInst(const CondBranchInst &Inst) {
  ReturnIfNot(visitCondBranchLikeInst(Inst));
  return true;
}

bool Verifier::visitTypeOfInst(const TypeOfInst &) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitTypeOfIsInst(const TypeOfIsInst &Inst) {
  AssertIWithMsg(
      Inst,
      llvh::isa<LiteralTypeOfIsTypes>(Inst.getOperand(TypeOfIsInst::TypesIdx)),
      "TypeOfIsInst::Types must be a TypeOfIs literal");
  return true;
}

bool Verifier::visitUnaryOperatorInst(const UnaryOperatorInst &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitTryStartInst(const TryStartInst &Inst) {
  AssertIWithMsg(Inst, isTerminator(&Inst), "TryStartInst must be a TermInst");
  AssertIWithMsg(
      Inst,
      Inst.getNumSuccessors() == 2,
      "TryStartInst must have 2 successors");
  AssertIWithMsg(
      Inst,
      Inst.getCatchTarget()->front().getKind() == ValueKind::CatchInstKind,
      "Catch Target of TryStartInst must begin with a CatchInst");
  return true;
}

bool Verifier::visitTryEndInst(const TryEndInst &Inst) {
  return true;
}

bool Verifier::visitBranchIfBuiltinInst(const BranchIfBuiltinInst &Inst) {
  return true;
}

bool Verifier::visitStoreStackInst(const StoreStackInst &Inst) {
  AssertIWithMsg(
      Inst,
      !llvh::isa<AllocStackInst>(Inst.getValue()),
      "Storing the address of stack location");
  AssertIWithMsg(
      Inst,
      typeCtx_.isSubsetOf(Inst.getValue()->getType(), Inst.getPtr()->getType()),
      "Value type mismatch");
  AssertIWithMsg(
      Inst, !Inst.hasUsers(), "Store Instructions must not have users");
  return true;
}

bool Verifier::visitStoreFrameInst(const StoreFrameInst &Inst) {
  AssertIWithMsg(
      Inst, !Inst.hasUsers(), "Store Instructions must not have users");
  auto *scope = Inst.getScope();
  AssertIWithMsg(
      Inst, scope->getType().isEnvironmentType(), "Wrong scope type");
  AssertIWithMsg(
      Inst,
      typeCtx_.isSubsetOf(
          Inst.getValue()->getType(), Inst.getVariable()->getType()),
      "Value type mismatch");
  if (auto *BSI = llvh::dyn_cast<BaseScopeInst>(scope)) {
    AssertIWithMsg(
        Inst,
        BSI->getVariableScope() == Inst.getVariable()->getParent(),
        "Storing to different scope than the variable's scope.");
  }
  return true;
}

bool Verifier::visitLoadFrameInst(const LoadFrameInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getType() == Inst.getLoadVariable()->getType(),
      "LoadFrameInst type must be the same as the variable type");
  auto *scope = Inst.getScope();
  AssertIWithMsg(
      Inst, scope->getType().isEnvironmentType(), "Wrong scope type");
  if (auto *BSI = llvh::dyn_cast<BaseScopeInst>(scope)) {
    AssertIWithMsg(
        Inst,
        BSI->getVariableScope() == Inst.getLoadVariable()->getParent(),
        "Loading from different scope than the variable's scope.");
  }
  return true;
}

bool Verifier::visitLoadStackInst(const LoadStackInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getType() == Inst.getPtr()->getType(),
      "LoadStackInst type must be the same as the AllocStackInst type");
  return true;
}

bool Verifier::visitBaseCreateLexicalChildInst(
    const hermes::BaseCreateLexicalChildInst &Inst) {
  auto *scope = Inst.getScope();
  AssertIWithMsg(
      Inst,
      llvh::isa<EmptySentinel>(scope) || scope->getType().isUndefinedType() ||
          scope->getType().isEnvironmentType(),
      "Wrong scope type");
  // Verify that any GetParentScope inside the function produces the same
  // VariableScope that the function is being created with.
  if (auto *BSI = llvh::dyn_cast<BaseScopeInst>(scope)) {
    for (auto *U : Inst.getFunctionCode()->getParentScopeParam()->getUsers()) {
      if (auto *GPSI = llvh::dyn_cast<GetParentScopeInst>(U)) {
        AssertIWithMsg(
            Inst,
            GPSI->getVariableScope() == BSI->getVariableScope(),
            "Parent scope mismatch.");
      }
    }
  }
  return true;
}

bool Verifier::visitBaseCreateCallableInst(const BaseCreateCallableInst &Inst) {
  ReturnIfNot(visitBaseCreateLexicalChildInst(Inst));
  AssertIWithMsg(
      Inst,
      llvh::isa<NormalFunction>(Inst.getFunctionCode()) ||
          llvh::isa<GeneratorFunction>(Inst.getFunctionCode()) ||
          llvh::isa<AsyncFunction>(Inst.getFunctionCode()),
      "BaseCreateCallableInst only accepts NormalFunction/GeneratorFunction");
  return true;
}
bool Verifier::visitCreateFunctionInst(const CreateFunctionInst &Inst) {
  ReturnIfNot(visitBaseCreateCallableInst(Inst));
  return true;
}

bool Verifier::visitCallInst(const CallInst &Inst) {
  // Nothing to verify at this point.
  return true;
}
bool Verifier::visitHBCCallWithArgCountInst(
    const HBCCallWithArgCountInst &Inst) {
  // NumArgumentsLiteral is not always a number literal. For example, it will be
  // lowered into LIRLoadConstInst.
  if (auto *LN = llvh::dyn_cast<LiteralNumber>(Inst.getNumArgumentsLiteral())) {
    AssertIWithMsg(
        Inst,
        LN->getValue() == Inst.getNumArguments(),
        "HBCCallWithArgCountInst mismatched arg count");
  }
  return true;
}

bool Verifier::visitHBCCallNInst(const HBCCallNInst &Inst) {
  AssertIWithMsg(
      Inst,
      HBCCallNInst::kMinArgs <= Inst.getNumArguments() &&
          Inst.getNumArguments() <= HBCCallNInst::kMaxArgs,
      "CallNInst has too many args");
  AssertIWithMsg(
      Inst,
      llvh::isa<LiteralUndefined>(Inst.getNewTarget()),
      "CallNInst NewTarget must be undefined");
  return true;
}

bool Verifier::visitCreateClassInst(const CreateClassInst &Inst) {
  // Nothing to verify.
  return true;
}

bool Verifier::visitCallBuiltinInst(CallBuiltinInst const &Inst) {
  assert(
      isNativeBuiltin(Inst.getBuiltinIndex()) &&
      "CallBuiltin must take a native builtin.");
  return true;
}

bool Verifier::visitGetBuiltinClosureInst(GetBuiltinClosureInst const &Inst) {
  assert(
      Inst.getBuiltinIndex() < BuiltinMethod::_count &&
      "Out of bound BuiltinMethod index.");
  return true;
}

bool Verifier::visitLoadPropertyInst(const LoadPropertyInst &Inst) {
  return true;
}
bool Verifier::visitLoadPropertyWithReceiverInst(
    const LoadPropertyWithReceiverInst &Inst) {
  return true;
}
bool Verifier::visitLoadOwnPrivateFieldInst(
    const LoadOwnPrivateFieldInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getProperty()->getType().isPrivateNameType(),
      "LoadOwnPrivateFieldInst must be loading a private name");
  return true;
}
bool Verifier::visitTryLoadGlobalPropertyInst(
    const TryLoadGlobalPropertyInst &Inst) {
  return true;
}

bool Verifier::visitDeletePropertyLooseInst(
    const DeletePropertyLooseInst &Inst) {
  // Nothing to verify at this point.
  return true;
}
bool Verifier::visitDeletePropertyStrictInst(
    const DeletePropertyStrictInst &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitStorePropertyWithReceiverInst(
    const StorePropertyWithReceiverInst &Inst) {
  return true;
}

bool Verifier::visitStorePropertyLooseInst(const StorePropertyLooseInst &Inst) {
  return true;
}
bool Verifier::visitStorePropertyStrictInst(
    const StorePropertyStrictInst &Inst) {
  return true;
}

bool Verifier::visitTryStoreGlobalPropertyLooseInst(
    const TryStoreGlobalPropertyLooseInst &Inst) {
  return true;
}
bool Verifier::visitTryStoreGlobalPropertyStrictInst(
    const TryStoreGlobalPropertyStrictInst &Inst) {
  return true;
}

bool Verifier::visitDefineOwnPropertyInst(const DefineOwnPropertyInst &Inst) {
  AssertIWithMsg(
      Inst,
      llvh::isa<LiteralBool>(
          Inst.getOperand(DefineOwnPropertyInst::IsEnumerableIdx)),
      "DefineOwnPropertyInst::IsEnumerable must be a boolean literal");
  return true;
}
bool Verifier::visitDefineOwnInDenseArrayInst(
    const DefineOwnInDenseArrayInst &Inst) {
  return true;
}
bool Verifier::visitStoreOwnPrivateFieldInst(
    const StoreOwnPrivateFieldInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getProperty()->getType().isPrivateNameType(),
      "AddOwnPrivatePropertyInst::Property must be a private name");
  return true;
}
bool Verifier::visitAddOwnPrivateFieldInst(const AddOwnPrivateFieldInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getProperty()->getType().isPrivateNameType(),
      "AddOwnPrivateFieldInst::Property must be a private name");
  return true;
}

bool Verifier::visitPrivateBrandCheckInst(const PrivateBrandCheckInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getBrand()->getType().isPrivateNameType(),
      "PrivateBrandCheckInst::Brand must be a private name");
  return true;
}

bool Verifier::visitDefineOwnGetterSetterInst(
    const DefineOwnGetterSetterInst &Inst) {
  AssertIWithMsg(
      Inst,
      llvh::isa<LiteralBool>(
          Inst.getOperand(DefineOwnGetterSetterInst::IsEnumerableIdx)),
      "StoreGetterSetterInsr::IsEnumerable must be a boolean constant");
  return true;
}

bool Verifier::visitAllocFastArrayInst(const hermes::AllocFastArrayInst &Inst) {
  LiteralNumber *size = Inst.getCapacity();
  AssertIWithMsg(
      Inst, size->isUInt32Representible(), "Invalid AllocArrayInst size hint");
  return true;
}

bool Verifier::visitAllocArrayInst(const hermes::AllocArrayInst &Inst) {
  LiteralNumber *size = Inst.getSizeHint();
  AssertIWithMsg(
      Inst, size->isUInt32Representible(), "Invalid AllocArrayInst size hint");
  AssertIWithMsg(
      Inst,
      Inst.isLiteralArray(),
      "Array elements must be literal when registers are limited");
  return true;
}

bool Verifier::visitGetTemplateObjectInst(
    const hermes::GetTemplateObjectInst &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitCreateArgumentsInst(const CreateArgumentsInst &Inst) {
  AssertIWithMsg(Inst, functionState, "functionState cannot be null");
  AssertIWithMsg(
      Inst,
      !functionState->createArgumentsEncountered,
      "There should be only one CreateArgumentsInst in a function");
  functionState->createArgumentsEncountered = true;

  BasicBlock *BB = Inst.getParent();
  Function *F = BB->getParent();
  if (!M.areGeneratorsLowered() && F->isInnerGenerator()) {
    auto secondBB = F->begin();
    ++secondBB;
    AssertIWithMsg(
        Inst,
        BB == &*secondBB,
        "CreateArgumentsInst must be in the second basic block in unlowered inner generators");
  } else {
    AssertIWithMsg(
        Inst,
        BB == &*F->begin(),
        "CreateArgumentsInst must be in the first basic block");
  }
  return true;
}

bool Verifier::visitCreateArgumentsLooseInst(
    const CreateArgumentsLooseInst &Inst) {
  return visitCreateArgumentsInst(Inst);
}
bool Verifier::visitCreateArgumentsStrictInst(
    const CreateArgumentsStrictInst &Inst) {
  return visitCreateArgumentsInst(Inst);
}

bool Verifier::visitCreateRegExpInst(CreateRegExpInst const &Inst) {
  // Nothing to verify at this point.
  return true;
}

template <class T>
bool Verifier::visitBinaryOperatorLikeInst(const T &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitBinaryOperatorInst(const hermes::BinaryOperatorInst &Inst) {
  return visitBinaryOperatorLikeInst(Inst);
}

bool Verifier::visitImulInst(const ImulInst &Inst) {
  // Both operands may be of any type: they coerce via ToInt32, or throw.
  AssertIWithMsg(
      Inst,
      Inst.getNumOperands() == 2,
      "ImulInst must have exactly 2 operands");
  return true;
}

bool Verifier::visitCatchInst(const CatchInst &Inst) {
  AssertIWithMsg(
      Inst,
      &Inst.getParent()->front() == &Inst,
      "Catch instruction must be the first in a basic block");
  unsigned tryStarts = 0;
  unsigned normalInsts = 0;
  for (const auto &predBB : predecessors(Inst.getParent())) {
    if (auto *TSI = llvh::dyn_cast<TryStartInst>(predBB->getTerminator())) {
      ++tryStarts;
      AssertIWithMsg(
          (*TSI),
          TSI->getCatchTarget() == Inst.getParent(),
          "CatchInst must be the catch target of its TryStartInst.");
    } else if (
        auto *TEI = llvh::dyn_cast<TryEndInst>(predBB->getTerminator())) {
      AssertIWithMsg(
          (*TEI),
          TEI->getCatchTarget() == Inst.getParent(),
          "CatchInst must be the catch target of its TryEndInst.");
    } else if (!llvh::isa<BaseThrowInst>(predBB->getTerminator())) {
      ++normalInsts;
    }
  }
  AssertIWithMsg(
      Inst,
      tryStarts == 1,
      "CatchInst must have exactly one TryStart predecessor.");
  AssertIWithMsg(
      Inst,
      normalInsts == 0,
      "CatchInst must have only TryStart and TryEnd predecessors.");
  return true;
}

bool Verifier::visitThrowInst(const ThrowInst &Inst) {
  AssertIWithMsg(Inst, isTerminator(&Inst), "ThrowInst must be a terminator");
  return true;
}

bool Verifier::visitThrowTypeErrorInst(const ThrowTypeErrorInst &Inst) {
  AssertIWithMsg(
      Inst, isTerminator(&Inst), "ThrowTypeErrorInst must be a terminator");
  return true;
}

bool Verifier::visitGetNextPNameInst(const GetNextPNameInst &Inst) {
  AssertIWithMsg(
      Inst, isTerminator(&Inst), "GetNextPNameInst must terminate the block");
  AssertIWithMsg(
      Inst,
      Inst.getNumSuccessors() == 2,
      "GetNextPNameInst should have 2 successors");
  AssertIWithMsg(
      Inst,
      succ_contains(Inst.getParent(), Inst.getOnSomeDest()),
      "OnSome dest should be a successor of GetNextPNameInst block");
  AssertIWithMsg(
      Inst,
      pred_contains(Inst.getOnSomeDest(), Inst.getParent()),
      "GetNextPNameInst block should be a predecessor of OnSome dest");
  AssertIWithMsg(
      Inst,
      succ_contains(Inst.getParent(), Inst.getOnLastDest()),
      "OnLast dest should be a successor of GetNextPNameInst block");
  AssertIWithMsg(
      Inst,
      pred_contains(Inst.getOnLastDest(), Inst.getParent()),
      "GetNextPNameInst block should be a predecessor of OnLast dest");
  return true;
}

bool Verifier::visitGetPNamesInst(const GetPNamesInst &Inst) {
  AssertIWithMsg(
      Inst, isTerminator(&Inst), "GetPNamesInst must terminate the block");
  AssertIWithMsg(
      Inst,
      Inst.getNumSuccessors() == 2,
      "GetPNamesInst should have 2 successors");
  AssertIWithMsg(
      Inst,
      succ_contains(Inst.getParent(), Inst.getOnSomeDest()),
      "OnSome dest should be a successor of GetPNamesInst block");
  AssertIWithMsg(
      Inst,
      pred_contains(Inst.getOnSomeDest(), Inst.getParent()),
      "GetPNamesInst block should be a predecessor of OnSome dest");
  AssertIWithMsg(
      Inst,
      succ_contains(Inst.getParent(), Inst.getOnEmptyDest()),
      "OnEmpty dest should be a successor of GetPNamesInst block");
  AssertIWithMsg(
      Inst,
      pred_contains(Inst.getOnEmptyDest(), Inst.getParent()),
      "GetPNamesInst block should be a predecessor of OnEmpty dest");
  return true;
}

bool Verifier::visitMovInst(const hermes::MovInst &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitImplicitMovInst(const hermes::ImplicitMovInst &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitCoerceThisNSInst(CoerceThisNSInst const &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitPhiInst(const hermes::PhiInst &Inst) {
  // We verify the dominance property when we scan the whole function. In here
  // we only verify local properties.

  llvh::SmallDenseSet<BasicBlock *, 8> entries;

  // Check that every input block enters only once:
  for (int i = 0, e = Inst.getNumEntries(); i < e; ++i) {
    auto pair = Inst.getEntry(i);
    BasicBlock *block = pair.second;
    AssertIWithMsg(
        Inst,
        pred_contains(Inst.getParent(), block),
        "Predecessor not covered by phi node!");
    AssertIWithMsg(
        Inst,
        succ_contains(block, Inst.getParent()),
        "Phi node should be Successor!");

    // In theory, it is legitimate for a Phi to have multiple entries for a
    // block as long as all the associated values are the same. However, such an
    // invariant would be more complicated, and requires care to maintain when
    // updating operands of the Phi.
    auto [it, first] = entries.insert(block);
    AssertIWithMsg(
        Inst, first, "Phi node has multiple entries for the same block");
  }

  AssertIWithMsg(
      Inst,
      entries.size() == pred_count_unique(Inst.getParent()),
      "number of predecessors does not match phi inputs");
  AssertIWithMsg(Inst, !entries.empty(), "Phi has no entries");
  return true;
}

template <class T>
bool Verifier::visitSwitchLikeInst(const T &Inst) {
  AssertIWithMsg(Inst, isTerminator(&Inst), "SwitchInst must be a terminator");

  AssertIWithMsg(
      Inst,
      Inst.getNumCasePair() > 0,
      "SwitchInst must have some case destinations");

  AssertIWithMsg(
      Inst, Inst.getDefaultDestination(), "Invalid destination block");

  AssertIWithMsg(Inst, Inst.getInputValue(), "Invalid input value");

  AssertIWithMsg(
      Inst,
      Inst.getNumSuccessors() == Inst.getNumCasePair() + 1,
      "Number of successors of SwitchInst does not match.");
  AssertIWithMsg(
      Inst,
      succ_contains(Inst.getParent(), Inst.getDefaultDestination()),
      "Default destination must be a successor of SwitchInst block");
  AssertIWithMsg(
      Inst,
      pred_contains(Inst.getDefaultDestination(), Inst.getParent()),
      "SwitchInst block must be a predecessor of default destination.");
  for (unsigned idx = 0, e = Inst.getNumCasePair(); idx < e; ++idx) {
    AssertIWithMsg(
        Inst,
        succ_contains(Inst.getParent(), Inst.getCasePair(idx).second),
        "Case target must be a successor of SwitchInst block");
    AssertIWithMsg(
        Inst,
        pred_contains(Inst.getCasePair(idx).second, Inst.getParent()),
        "SwitchInst block must be a predecessor of case target");
  }

  // Verify that each case is unique.
  llvh::SmallPtrSet<Literal *, 8> values;
  for (unsigned idx = 0, e = Inst.getNumCasePair(); idx < e; ++idx) {
    AssertIWithMsg(
        Inst,
        values.insert(Inst.getCasePair(idx).first).second,
        "switch values must be unique");
  }
  return true;
}

bool Verifier::visitSwitchInst(const hermes::SwitchInst &Inst) {
  return visitSwitchLikeInst(Inst);
}

bool Verifier::visitUIntSwitchImmInst(const hermes::UIntSwitchImmInst &Inst) {
  // For integer jump tables (SwitchImmInst, above), we assert that the
  // LiteralNumber is a UInt32.
  ReturnIfNot(visitSwitchLikeInst(Inst));
  for (unsigned idx = 0, e = Inst.getNumCasePair(); idx < e; ++idx) {
    AssertIWithMsg(
        Inst,
        Inst.getCasePair(idx).first->isUInt32Representible(),
        "case value must be a uint32");
  }

  return true;
}

bool Verifier::visitStringSwitchImmInst(
    const hermes::StringSwitchImmInst &Inst) {
  ReturnIfNot(visitSwitchLikeInst(Inst));
  // No checking of the cases is necessary for string switches.
  return true;
}

bool Verifier::visitDebuggerInst(DebuggerInst const &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitDirectEvalInst(DirectEvalInst const &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitDeclareGlobalVarInst(const DeclareGlobalVarInst &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitHBCCreateFunctionEnvironmentInst(
    const HBCCreateFunctionEnvironmentInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getParentScopeParam() == Inst.getFunction()->getParentScopeParam(),
      "Using incorect parent scope parameter.");
  return true;
}
bool Verifier::visitGetParentScopeInst(const GetParentScopeInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getParentScopeParam() == Inst.getFunction()->getParentScopeParam(),
      "Using incorect parent scope parameter.");

  for (auto *U : Inst.getFunction()->getUsers()) {
    if (auto *BCLI = llvh::dyn_cast<BaseCreateLexicalChildInst>(U)) {
      AssertIWithMsg(
          Inst,
          BCLI->getVarScope() == Inst.getVariableScope(),
          "Scope result does not match function creation");
      break;
    }
  }
  return true;
}
bool Verifier::visitCreateScopeInst(const CreateScopeInst &Inst) {
  functionState->createScopeEncountered = true;
  return true;
}
bool Verifier::visitResolveScopeInst(const ResolveScopeInst &Inst) {
  return true;
}
bool Verifier::visitLIRResolveScopeInst(
    const hermes::LIRResolveScopeInst &Inst) {
  return true;
}
bool Verifier::visitGetClosureScopeInst(
    const hermes::GetClosureScopeInst &Inst) {
  for (auto *U : Inst.getFunctionCode()->getUsers()) {
    if (auto *BCLI = llvh::dyn_cast<BaseCreateLexicalChildInst>(U)) {
      AssertIWithMsg(
          Inst,
          BCLI->getVarScope() == Inst.getVariableScope(),
          "Scope result does not match function creation");
      return true;
    }
  }
  return true;
}

bool Verifier::visitHBCProfilePointInst(const HBCProfilePointInst &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitLIRAllocObjectFromBufferInst(
    const hermes::LIRAllocObjectFromBufferInst &Inst) {
  return true;
}

bool Verifier::visitLIRAllocTypedObjectFromBufferInst(
    const hermes::LIRAllocTypedObjectFromBufferInst &Inst) {
  AssertIWithMsg(
      Inst,
      !llvh::isa<EmptySentinel>(Inst.getParentObject()),
      "LIRAllocTypedObjectFromBufferInst requires a parent object");
  return true;
}

bool Verifier::visitLIRAllocTypedNonEnumObjectFromBufferInst(
    const hermes::LIRAllocTypedNonEnumObjectFromBufferInst &Inst) {
  AssertIWithMsg(
      Inst,
      !llvh::isa<EmptySentinel>(Inst.getParentObject()),
      "LIRAllocTypedNonEnumObjectFromBufferInst requires a parent object");
  return true;
}

bool Verifier::visitAllocObjectLiteralInst(
    const hermes::AllocObjectLiteralInst &Inst) {
  return true;
}

bool Verifier::visitAllocTypedObjectInst(
    const hermes::AllocTypedObjectInst &Inst) {
  return true;
}

bool Verifier::visitAllocTypedNonEnumObjectInst(
    const hermes::AllocTypedNonEnumObjectInst &Inst) {
  return true;
}

bool Verifier::visitLIRGetGlobalObjectInst(const LIRGetGlobalObjectInst &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitLIRLoadConstInst(hermes::LIRLoadConstInst const &Inst) {
  // Nothing to verify at this point.
  return true;
}

bool Verifier::visitLoadParamInst(hermes::LoadParamInst const &Inst) {
  return true;
}
bool Verifier::visitHBCCompareBranchInst(const HBCCompareBranchInst &Inst) {
  return visitCondBranchLikeInst(Inst) && visitBinaryOperatorLikeInst(Inst);
}
bool Verifier::visitHBCCmpBrTypeOfIsInst(const HBCCmpBrTypeOfIsInst &Inst) {
  AssertIWithMsg(
      Inst,
      llvh::isa<LiteralTypeOfIsTypes>(
          Inst.getOperand(HBCCmpBrTypeOfIsInst::TypesIdx)),
      "HBCCmpBrTypeOfIsInst::Types must be a TypeOfIs literal");
  return visitCondBranchLikeInst(Inst);
}

bool Verifier::visitCreateGeneratorInst(const CreateGeneratorInst &Inst) {
  ReturnIfNot(visitBaseCreateLexicalChildInst(Inst));
  AssertIWithMsg(
      Inst,
      Inst.getFunctionCode()->isInnerGenerator(),
      "CreateGeneratorInst must take a GeneratorInnerFunction");
  AssertIWithMsg(
      Inst,
      llvh::isa<BaseScopeInst>(Inst.getScope()),
      "CreateGeneratorInst must take a BaseScopeInst");
  return true;
}
bool Verifier::visitResumeGeneratorInst(const ResumeGeneratorInst &Inst) {
  AssertIWithMsg(
      Inst,
      !M.areGeneratorsLowered(),
      "Should not exist after generators are lowered");
  return true;
}

bool Verifier::visitLIRGetThisNSInst(const LIRGetThisNSInst &Inst) {
  // Nothing to verify at this point.
  return true;
}
bool Verifier::visitLIRGetArgumentsPropByValLooseInst(
    const LIRGetArgumentsPropByValLooseInst &Inst) {
  // Nothing to verify at this point.
  return true;
}
bool Verifier::visitLIRGetArgumentsPropByValStrictInst(
    const LIRGetArgumentsPropByValStrictInst &Inst) {
  // Nothing to verify at this point.
  return true;
}
bool Verifier::visitLIRGetArgumentsLengthInst(
    const LIRGetArgumentsLengthInst &Inst) {
  // Nothing to verify at this point.
  return true;
}
bool Verifier::visitLIRReifyArgumentsLooseInst(
    const LIRReifyArgumentsLooseInst &Inst) {
  // Nothing to verify at this point.
  return true;
}
bool Verifier::visitLIRReifyArgumentsStrictInst(
    const LIRReifyArgumentsStrictInst &Inst) {
  // Nothing to verify at this point.
  return true;
}
bool Verifier::visitCreateThisInst(const CreateThisInst &Inst) {
  return true;
}
bool Verifier::visitGetConstructedObjectInst(
    const GetConstructedObjectInst &Inst) {
  return true;
}

bool Verifier::visitLIRSpillMovInst(const LIRSpillMovInst &Inst) {
  return true;
}
bool Verifier::visitUnreachableInst(const UnreachableInst &Inst) {
  return true;
}

bool Verifier::visitIteratorBeginInst(const IteratorBeginInst &Inst) {
  AssertIWithMsg(
      Inst,
      llvh::isa<AllocStackInst>(Inst.getSourceOrNext()),
      "SourceOrNext must be an AllocStackInst");
  return true;
}
bool Verifier::visitIteratorNextInst(const IteratorNextInst &Inst) {
  // Nothing to verify at this point.
  return true;
}
bool Verifier::visitIteratorCloseInst(const IteratorCloseInst &Inst) {
  AssertIWithMsg(
      Inst,
      llvh::isa<LiteralBool>(
          Inst.getOperand(IteratorCloseInst::IgnoreInnerExceptionIdx)),
      "IgnoreInnerException must be a LiteralBool in IteratorCloseInst");
  return true;
}

bool Verifier::visitGetNewTargetInst(GetNewTargetInst const &Inst) {
  auto definitionKind = Inst.getParent()->getParent()->getDefinitionKind();
  AssertIWithMsg(
      Inst,
      definitionKind == Function::DefinitionKind::ES5Function ||
          definitionKind == Function::DefinitionKind::ES6BaseConstructor ||
          definitionKind == Function::DefinitionKind::ES6DerivedConstructor ||
          definitionKind == Function::DefinitionKind::ES6Method,
      "GetNewTargetInst can only be used in ES6 constructors, ES5 functions, and ES6 methods");
  AssertIWithMsg(
      Inst,
      Inst.getParent()->getParent()->getNewTargetParam() ==
          Inst.getOperand(GetNewTargetInst::GetNewTargetParamIdx),
      "GetNewTargetInst must use correct getNewTargetParam");
  return true;
}

bool Verifier::visitThrowIfInst(const ThrowIfInst &Inst) {
  const Type invTypes = Inst.getInvalidTypes()->getData();
  AssertIWithMsg(
      Inst,
      !invTypes.isNoType() &&
          typeCtx_.isSubsetOf(
              invTypes,
              typeCtx_.unionTy(Type::createEmpty(), Type::createUninit())),
      "ThrowIfInst invalid types set can only contain Empty or Uninit");

  // Note: we are not performing a subtraction and equality check here, because
  // if the invalid types and the input types are proven disjoint after
  // TypeInference, we deliberately don't return NoType.
  AssertIWithMsg(
      Inst,
      typeCtx_.intersectTy(Inst.getType(), invTypes).isNoType(),
      "ThrowIfInst must throw away all invalid types");

  // A variable may only perform the following state transitions:
  // empty -> uninit -> initialized
  // empty -> initialized
  // uninit -> initialized
  // So, it can never be empty after it has been checked for uninit.
  AssertIWithMsg(
      Inst,
      !typeCtx_.canBeEmpty(Inst.getType()),
      "ThrowIfInst can never return type Empty");
  return true;
}

bool Verifier::visitThrowIfThisInitializedInst(
    const ThrowIfThisInitializedInst &Inst) {
  return true;
}

bool Verifier::visitPrLoadInst(const PrLoadInst &Inst) {
  return true;
}
bool Verifier::visitPrStoreInst(const PrStoreInst &Inst) {
  return true;
}

bool Verifier::visitFastArrayLoadInst(const FastArrayLoadInst &Inst) {
  return true;
}
bool Verifier::visitFastArrayStoreInst(const FastArrayStoreInst &Inst) {
  return true;
}
bool Verifier::visitFastArrayPushInst(const FastArrayPushInst &Inst) {
  return true;
}
bool Verifier::visitFastArrayAppendInst(const FastArrayAppendInst &Inst) {
  return true;
}
bool Verifier::visitFastArrayLengthInst(const FastArrayLengthInst &Inst) {
  return true;
}

bool Verifier::visitLoadParentNoTrapsInst(const LoadParentNoTrapsInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getObject()->getType().isObjectType(),
      "input object value must be of object type");
  return true;
}

bool Verifier::visitTypedLoadParentInst(const TypedLoadParentInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getObject()->getType().isObjectType(),
      "input object value must be of object type");
  return true;
}

bool Verifier::visitFUnaryMathInst(const FUnaryMathInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getArg()->getType().isNumberType() && Inst.getType().isNumberType(),
      "FUnaryMathInst wrong type");
  return true;
}
bool Verifier::visitFBinaryMathInst(const FBinaryMathInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getLeft()->getType().isNumberType() &&
          Inst.getRight()->getType().isNumberType() &&
          Inst.getType().isNumberType(),
      "FBinaryMathInst wrong type");
  return true;
}
bool Verifier::visitFCompareInst(const FCompareInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getLeft()->getType().isNumberType() &&
          Inst.getRight()->getType().isNumberType() &&
          Inst.getType().isBooleanType(),
      "FCompare wrong type");
  return true;
}
bool Verifier::visitHBCFCompareBranchInst(const HBCFCompareBranchInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getLeftHandSide()->getType().isNumberType() &&
          Inst.getRightHandSide()->getType().isNumberType(),
      "HBCFCompareBranch wrong type");
  return visitCondBranchLikeInst(Inst) && visitBinaryOperatorLikeInst(Inst);
}
bool Verifier::visitStringConcatInst(const StringConcatInst &Inst) {
  AssertIWithMsg(
      Inst, Inst.getType().isStringType(), "StringConcat wrong type");
  for (unsigned i = 0, e = Inst.getNumOperands(); i < e; ++i) {
    AssertIWithMsg(
        Inst,
        Inst.getOperand(i)->getType().isStringType(),
        "StringConcat wrong type");
  }
  return true;
}
bool Verifier::visitHBCStringConcatInst(const HBCStringConcatInst &Inst) {
  AssertIWithMsg(
      Inst, Inst.getType().isStringType(), "HBCStringConcat wrong type");
  AssertIWithMsg(
      Inst,
      Inst.getLeft()->getType().isStringType(),
      "HBCStringConcat left operand wrong type");
  AssertIWithMsg(
      Inst,
      Inst.getRight()->getType().isStringType(),
      "HBCStringConcat right operand wrong type");
  return true;
}

bool Verifier::visitUnionNarrowTrustedInst(const UnionNarrowTrustedInst &Inst) {
  return true;
}
bool Verifier::visitCheckedTypeCastInst(const CheckedTypeCastInst &Inst) {
  return true;
}
bool Verifier::visitLIRDeadValueInst(const LIRDeadValueInst &Inst) {
  return true;
}

bool Verifier::visitNativeCallInst(const hermes::NativeCallInst &Inst) {
  // FIXM: this should be added when types are propagated.
  //  Assert(
  //      Inst.getCallee()->getType().isNumberType(),
  //      "NativeCallInst callee must be a number");
  return true;
}

bool Verifier::visitGetNativeRuntimeInst(
    const hermes::GetNativeRuntimeInst &Inst) {
  return true;
}

bool Verifier::visitLazyCompilationDataInst(
    const hermes::LazyCompilationDataInst &Inst) {
  return true;
}

bool Verifier::visitEvalCompilationDataInst(
    const hermes::EvalCompilationDataInst &Inst) {
  return true;
}

} // namespace

#endif

bool hermes::verifyModule(
    const Module &M,
    raw_ostream *OS,
    VerificationMode mode) {
#ifdef HERMES_SLOW_DEBUG
  raw_null_ostream NullStr;
  NullStr.SetUnbuffered();
  raw_ostream &stream = OS ? *OS : NullStr;
  irdumper::IRPrinter printer(
      M, stream, /*escape*/ false, /*labelAllInsts*/ true);
  Verifier V(M, stream, mode, printer);
  bool result = V.verify();
  return result;
#else
  return true;
#endif
}
