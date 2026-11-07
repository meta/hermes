/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "utils"

#include "hermes/Optimizer/Scalar/Utils.h"

#include "hermes/IR/Analysis.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/IRUtils.h"
#include "hermes/IR/Instrs.h"

#include "llvh/ADT/SetVector.h"

namespace hermes {

Value *isStoreOnceVariable(Variable *V) {
  Value *res = nullptr;

  for (auto *U : V->getUsers()) {
    if (llvh::isa<LoadFrameInst>(U)) {
      continue;
    }
    if (auto *SF = llvh::dyn_cast<StoreFrameInst>(U)) {
      auto *val = SF->getValue();

      // We found a stored value. Make sure that there is only one stored value.
      if (res && val != res)
        return nullptr;

      res = val;
      continue;
    }

    llvm_unreachable("invalid user!");
  }

  return res;
}

Value *isStoreOnceStackLocation(AllocStackInst *AS) {
  Value *res = nullptr;

  for (auto *U : AS->getUsers()) {
    if (llvh::isa<LoadStackInst>(U)) {
      continue;
    }
    if (auto *SS = llvh::dyn_cast<StoreStackInst>(U)) {
      // Theoretically someone might try storing a stack address in another
      // stack location. That is not allowed, but we might as well be correct
      // here.
      if (SS->getPtr() != AS)
        continue;

      auto *val = SS->getValue();

      // We found a stored value. Make sure that there is only one stored value.
      if (res && val != res)
        return nullptr;

      res = val;
      continue;
    }

    llvm_unreachable("invalid user!");
  }

  return res;
}

llvh::SmallVector<BaseCallInst *, 2> getKnownCallsites(Function *F) {
  llvh::SmallVector<BaseCallInst *, 2> result{};
  for (Instruction *user : F->getUsers()) {
    if (auto *call = llvh::dyn_cast<BaseCallInst>(user)) {
      assert(
          call->getTarget() == F &&
          "invalid usage of Function as operand of BaseCallInst");
      result.push_back(call);
    }
  }
  return result;
}

BasicBlock *splitBasicBlock(
    BasicBlock *BB,
    BasicBlock::InstListType::iterator it) {
  Function *F = BB->getParent();

  IRBuilder builder(F);
  auto *newBB = builder.createBasicBlock(F);

  for (auto *succ : successors(BB))
    updateIncomingPhiValues(succ, BB, newBB);

  // Move the instructions after the split point into the new BB.
  newBB->getInstList().splice(
      newBB->end(), BB->getInstList(), it, BB->getInstList().end());

  // setParent is not called by splice, so add it ourselves.
  for (auto &movedInst : *newBB)
    movedInst.setParent(newBB);

  return newBB;
}

/// Delete all incoming arrows from \p incoming in PhiInsts in \p blockToModify.
bool deleteIncomingBlockFromPhis(
    BasicBlock *blockToModify,
    BasicBlock *incoming) {
  bool changed = false;
  for (auto &I : *blockToModify) {
    auto *phi = llvh::dyn_cast<PhiInst>(&I);
    if (!phi)
      break;

    for (signed i = (signed)phi->getNumEntries() - 1; i >= 0; i--) {
      auto entry = phi->getEntry(i);
      if (entry.second != incoming)
        continue;

      phi->removeEntry(i);
      changed = true;
    }
  }
  return changed;
}

Value *getSinglePhiValue(PhiInst *P) {
  Value *incoming = nullptr;
  for (int i = 0, e = P->getNumEntries(); i < e; i++) {
    auto E = P->getEntry(i);
    // Ignore self edges.
    if (E.first == P)
      continue;

    // Record the first valid input.
    if (!incoming) {
      incoming = E.first;
      continue;
    }

    // Found another unique value. Bail out.
    if (incoming != E.first)
      return nullptr;
  }

  return incoming;
}

void splitCriticalEdge(IRBuilder *builder, BasicBlock *from, BasicBlock *to) {
  // Special case: If the target block is Catch block, there's only one
  // possible arrow and we can't insert anything between them. Just
  // start writing after the Catch statement.
  if (auto *tryStart = llvh::dyn_cast<TryStartInst>(from->getTerminator())) {
    if (tryStart->getCatchTarget() == to) {
      builder->setInsertionPointAfter(&to->front());
      return;
    }
  }

  // General case: insert a new block and rewrite Phis
  auto *newBlock = builder->createBasicBlock(from->getParent());
  builder->setInsertionBlock(newBlock);
  auto *branch = builder->createBranchInst(to);
  int updates = 0;

  Instruction *terminator = from->getTerminator();
  for (int i = 0, e = terminator->getNumOperands(); i < e; i++) {
    if (terminator->getOperand(i) == to) {
      terminator->setOperand(newBlock, i);
      for (auto &I : *to) {
        auto *phi = llvh::dyn_cast<PhiInst>(&I);
        if (!phi)
          break;
        for (int j = 0, f = phi->getNumEntries(); j < f; j++) {
          auto entry = phi->getEntry(j);
          if (entry.second != from)
            continue;
          phi->updateEntry(j, entry.first, newBlock);
        }
      }
      updates++;
    }
  }

  if (!updates) {
    llvm_unreachable("There were no current transitions between blocks");
  }
  builder->setInsertionPoint(branch);
}

bool deleteUnusedVariables(Module *M) {
  bool changed = false;
  auto &scopeList = M->getVariableScopes();
  for (auto it = scopeList.begin(); it != scopeList.end();) {
    // If the scope is unused, delete the whole scope.
    if (!it->hasUsers()) {
      // Remove the scope from its parent. Note that its children are also dead,
      // but may still have users in dead functions, so just move them to the
      // parent and leave it to function DCE to eliminate their usage.
      it->removeFromScopeChain();
      VariableScope *varScope = &*it++;
      M->destroyVariableScope(varScope);
      continue;
    }

    llvh::SmallVectorImpl<Variable *> &vars = it->getVariables();
    ++it;
    // Delete variables without any users. Do this in a separate loop since we
    // are putting vars in an invalid state.
    for (Variable *&var : vars) {
      if (!var->hasUsers()) {
        Value::destroy(var);
        var = nullptr;
        changed = true;
      }
    }
    // Clean up the variable list to remove destroyed entries.
    llvh::erase_if(vars, [](Variable *var) { return !var; });
  }

  return changed;
}

bool deleteUnusedFunctionsAndVariables(Module *M) {
  // A list of unused functions to deallocate from memory.
  // We need to destroy the memory at the very end of this function because a
  // dead function may have a variable that is referenced by an inner function
  // (which will also become dead once the outer function is removed). However,
  // we cannot destroy the outer function right away until we destroy the inner
  // function.
  llvh::SmallVector<Function *, 16> toDestroy;

  bool changed = false, localChanged = false;
  do {
    // A list of unused functions to remove from the module without being
    // destroyed. We have to collect these separately since we can't remove the
    // functions as we iterate over the module.
    llvh::SmallSetVector<Function *, 16> toRemove;
    for (auto &F : *M) {
      // Delete any functions that do not have any uses other than in their own
      // bodies. The top level function does not have an explicit user, so check
      // for it directly. CommonJS module functions are only referenced by the
      // module's CJS module table, so they must also be kept.
      if (&F != M->getTopLevelFunction() && !M->findCJSModule(&F) &&
          llvh::all_of(F.getUsers(), [&F](Instruction *user) {
            // Use must be from another function to be meaningful.
            return user->getFunction() == &F;
          })) {
        toRemove.insert(&F);
      }
    }

    // We erase the basic blocks and instructions from each function in
    // toRemove, and also remove the function from the module. However, the
    // memory of the function remains alive.
    for (size_t i = 0; i < toRemove.size(); ++i) {
      auto *F = toRemove[i];

      // All users of F must also be dead so add them to the worklist. This is
      // also necessary for correctness since it avoids leaving dangling
      // references.
      for (auto *U : F->getUsers())
        toRemove.insert(U->getFunction());

      F->eraseFromParentNoDestroy();
      toDestroy.push_back(F);
    }
    localChanged = !toRemove.empty();
    changed |= localChanged;
  } while (localChanged);

  // Now that all instructions have been destroyed from each dead function, it's
  // now safe to destroy them including the variables in them.
  for (auto *F : toDestroy) {
    assert(F->empty() && "All basic blocks should have been deleted.");
    Value::destroy(F);
  }

  // Deleting functions will make some variables unused, delete them.
  changed |= deleteUnusedVariables(M);
  return changed;
}

bool functionHasTryCatch(Function *F) {
  for (const auto &BB : *F) {
    if (llvh::isa<TryStartInst>(BB.getTerminator())) {
      return true;
    }
  }
  return false;
}

} // namespace hermes

#undef DEBUG_TYPE
