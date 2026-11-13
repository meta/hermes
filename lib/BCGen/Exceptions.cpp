/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "exceptions"

#include "hermes/BCGen/Exceptions.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRBuilder.h"

#include "llvh/ADT/STLExtras.h"
#include "llvh/Support/raw_ostream.h"

using namespace hermes;

/// Construct the list of basic blocks covered by each catch instruction.
/// Use recursion to handle nested catches.
/// \return A set of blocks corresponding to the blocks that end a try starting
/// at startBlock on success. On failure, return None.
/// \p F the function under consideration
/// \p catchInfoMap a CatchInfoMap that has entries for every CatchInst
/// \p aliveCatches the currently active CatchInsts (should be empty)
/// \p visited the previously visited BasicBlocks (should be empty)
/// \p startBlock the entry BasicBlock
/// \p maxRecursionDepth if the construction exceeds this many levels of
///     recursion, it adds a compilation error and returns None.
static llvh::Optional<llvh::SmallPtrSet<BasicBlock *, 4>>
constructCatchMapHelper(
    Function *F,
    CatchInfoMap &catchInfoMap,
    llvh::SmallVectorImpl<CatchInst *> &aliveCatches,
    llvh::SmallPtrSetImpl<BasicBlock *> &visited,
    BasicBlock *startBlock,
    uint32_t maxRecursionDepth) {
  assert(
      !llvh::isa<CatchInst>(startBlock->front()) &&
      "Functions and try bodies should never start with a catch instruction.");
  if (maxRecursionDepth == 0) {
    F->getContext().getSourceErrorManager().error(
        F->getSourceRange(), "Too deeply nested try/catch");
    return llvh::None;
  }

  // Stack to DFS through the CFG.
  llvh::SmallVector<BasicBlock *, 4> stack;
  // Track each of the basic blocks that follow a TryEndInst corresponding
  // to the current try.
  llvh::SmallPtrSet<BasicBlock *, 4> tryEndBlocks;

  visited.insert(startBlock);
  stack.push_back(startBlock);
  while (!stack.empty()) {
    BasicBlock *currentBlock = stack.pop_back_val();
    assert(
        visited.count(currentBlock) &&
        "All blocks on the stack must be marked as visited.");

    // For every nested try that's alive, we add the basic block into it.
    for (const auto &aliveCatch : aliveCatches)
      catchInfoMap[aliveCatch].coveredBlockList.push_back(currentBlock);

    auto *tryStartInst =
        llvh::dyn_cast<TryStartInst>(currentBlock->getTerminator());

    if (!tryStartInst) {
      if (auto *tryEndInst =
              llvh::dyn_cast<TryEndInst>(currentBlock->getTerminator())) {
        // This block marks the end of a try region. Add its successor to
        // tryEndBlocks but not to the stack. It will be visited by the caller.
        tryEndBlocks.insert(tryEndInst->getBranchDest());
      } else if (llvh::isa<BaseThrowInst>(currentBlock->getTerminator())) {
        // Do nothing. Don't follow the catch target.
      } else {
        // Common case: no TryStartInst, we add successors to the stack.
        for (BasicBlock *successor : successors(currentBlock)) {
          // Add unvisited blocks to the stack.
          if (visited.insert(successor).second)
            stack.push_back(successor);
        }
      }

      continue;
    }

    // Hit a TryStartInst, marking the start of a new try region.
    // The first instruction of the catch target block must be CatchInst.
    auto catchInst = cast<CatchInst>(&tryStartInst->getCatchTarget()->front());
    catchInfoMap[catchInst].depth = aliveCatches.size();

    // Pushing the CatchInst to the try stack, and continue scan the try body.
    aliveCatches.push_back(catchInst);
    auto endBlocks = constructCatchMapHelper(
        F,
        catchInfoMap,
        aliveCatches,
        visited,
        tryStartInst->getTryBody(),
        maxRecursionDepth - 1);
    if (!endBlocks)
      return llvh::None;
    aliveCatches.pop_back();

    for (BasicBlock *endBlock : *endBlocks) {
      if (visited.insert(endBlock).second)
        stack.push_back(endBlock);
    }

    // We also want to continue scan into the catch blocks.
    BasicBlock *catchTarget = tryStartInst->getCatchTarget();
    assert(
        visited.count(catchTarget) == 0 &&
        "Catch block must be dominated by the try start.");
    visited.insert(catchTarget);
    stack.push_back(catchTarget);
  }
  assert(
      (aliveCatches.size() || !tryEndBlocks.size()) &&
      "Block without live catches cannot have TryEndInst.");
  return {std::move(tryEndBlocks)};
}

/// Construct the list of basic blocks covered by each catch instruction.
/// Use recursion to handle nested catches.
/// \return true on success, false if recursion maximum is exceeded.
/// \p F the function under consideration
/// \p catchInfoMap a CatchInfoMap that has entries for every CatchInst
/// \p startBlock the entry BasicBlock
static bool constructCatchMap(
    Function *F,
    CatchInfoMap &catchInfoMap,
    BasicBlock *startBlock) {
  llvh::SmallVector<CatchInst *, 4> aliveCatches{};
  llvh::SmallPtrSet<BasicBlock *, 32> visited{};
  static constexpr uint32_t MAX_RECURSION_DEPTH = 1024;

  return constructCatchMapHelper(
             F,
             catchInfoMap,
             aliveCatches,
             visited,
             startBlock,
             MAX_RECURSION_DEPTH)
      .hasValue();
}

ExceptionEntryList hermes::generateExceptionHandlers(
    CatchInfoMap &catchInfoMap,
    BasicBlockInfoMap &bbMap,
    Function *F) {
  // Construct the list of blocks and depth covered by each CatchInst.
  if (!constructCatchMap(F, catchInfoMap, &F->front()))
    return {};

  ExceptionEntryList exception_entries;
  for (auto I : catchInfoMap) {
    auto &catchInfo = I.second;
    // The basic blocks covered by a catch instruction may not be continuous.
    // For each basic block, we walk through the current list of ranges,
    // and try to merge them into a minimum number of ranges.
    llvh::SmallVector<std::pair<uint32_t, uint32_t>, 4> catch_ranges;
    for (auto BB : catchInfo.coveredBlockList) {
      auto it = bbMap.find(BB);
      assert(it != bbMap.end() && "Basic Block missing.");

      auto resolved_loc = it->second;
      if (resolved_loc.first == resolved_loc.second) {
        // Empty basic block, skip.
        continue;
      }
      catch_ranges.push_back(resolved_loc);
    }
    llvh::array_pod_sort(catch_ranges.begin(), catch_ranges.end());
    // After ranges are sorted, a range could only be merged with it's
    // previous range, if they are adjacent.
    // Note: no range can overlap, as basic blocks do not overlap.
    int nextIndex = 0;
    for (auto resolved_loc : catch_ranges) {
      // If we are looking at the first range, or the range cannot
      // be merged with the previous range, we store this range.
      if (nextIndex == 0 ||
          catch_ranges[nextIndex - 1].second != resolved_loc.first) {
        catch_ranges[nextIndex++] = resolved_loc;
        continue;
      }
      // Otherwise we merge with the previous range.
      catch_ranges[nextIndex - 1].second = resolved_loc.second;
    }
    // The merging happened in-place. Hence we need to throw away the rest.
    catch_ranges.resize(nextIndex);
    // For each range, we register it as an exception handler entry.
    for (auto range : catch_ranges) {
      exception_entries.push_back(
          {(uint32_t)range.first,
           (uint32_t)range.second,
           (uint32_t)catchInfo.catchLocation,
           catchInfo.depth});
    }
  }
  // Sort ranges by depth. In hermes, depth increase when you nest try inside
  // try/catch/finally.
  llvh::array_pod_sort(exception_entries.begin(), exception_entries.end());
  return exception_entries;
}

#undef DEBUG_TYPE
