/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "objectstackpromotion"

#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Support/Statistic.h"

STATISTIC(ObjsEliminated, "Object allocations eliminated");

namespace hermes {
namespace {

/// Collect \p alloc, followed by every UnionNarrowTrustedInst reachable from it
/// through a chain of UnionNarrowTrustedInsts, into \p aliases. A trusted
/// narrow only changes the static type of its operand, so each collected value
/// is the object allocated by \p alloc, whoever emitted the narrow: IRGen's
/// narrow of a typed allocation, its narrow of a value stored into a binding of
/// a structurally compatible type, or any other. Every narrow appears after its
/// operand.
void collectObjectAliases(
    BaseAllocObjectLiteralInst *alloc,
    llvh::SmallVectorImpl<Instruction *> &aliases) {
  aliases.push_back(alloc);
  for (size_t i = 0; i < aliases.size(); ++i)
    for (Instruction *U : aliases[i]->getUsers())
      if (llvh::isa<UnionNarrowTrustedInst>(U))
        aliases.push_back(U);
}

/// Try to promote the given object to a series of AllocStackInsts if it does
/// not escape its enclosing function. If successful, \p alloc, its trusted
/// narrows (see collectObjectAliases) and all of their users are eliminated
/// and replaced with stack operations. For now, this only handles objects with
/// constant key properties, that are accessed with known existing properties.
/// \p alloc the object to promote
/// \p destroyer is used to eliminate instructions
bool tryPromoteObject(
    BaseAllocObjectLiteralInst *alloc,
    IRBuilder::InstructionDestroyer &destroyer) {
  IRBuilder builder(alloc->getFunction());
  auto numElems = alloc->getKeyValuePairCount();
  // Used as scratch space to perform the conversion of a number to a string.
  char conversionBuf[NUMBER_TO_STRING_BUF_SIZE];

  // Map from number to its string representation.
  llvh::SmallDenseMap<LiteralNumber *, LiteralString *> conversionMapping;
  /// Convert \p LN to a LiteralString.
  auto convertNumber =
      [&conversionMapping, &conversionBuf, &builder](LiteralNumber *LN) {
        auto &cachedLS = conversionMapping[LN];
        if (cachedLS) {
          return cachedLS;
        }
        auto strRef = numberToString(
            LN->getValue(), conversionBuf, NUMBER_TO_STRING_BUF_SIZE);
        auto *LS = builder.getLiteralString(strRef);
        cachedLS = LS;
        return LS;
      };

  // Map from property key string -> slot index.
  // Keys must be either LiteralString or LiteralPrivateName.
  llvh::SmallDenseMap<Literal *, size_t> objLayout;
  // Populate the object layout, ensuring all property keys are represented as
  // LiteralStrings.
  for (size_t i = 0; i < numElems; ++i) {
    auto *propKey = alloc->getKey(i);
    if (auto *LS = llvh::dyn_cast<LiteralString>(propKey)) {
      objLayout.insert({LS, i});
    } else if (auto *LPN = llvh::dyn_cast<LiteralPrivateName>(propKey)) {
      objLayout.insert({LPN, i});
    } else {
      auto *LN = llvh::cast<LiteralNumber>(propKey);
      objLayout.insert({convertNumber(LN), i});
    }
  }

  /// \return true if \p V is known to be an existing property key on the
  /// object. This will convert LiteralNumbers to LiteralStrings to do the
  /// check, since the layout is defined in terms of strings.
  auto isInLayout = [&objLayout, &convertNumber](Value *V) -> bool {
    if (auto *LS = llvh::dyn_cast<LiteralString>(V)) {
      return objLayout.count(LS);
    }
    if (auto *LPN = llvh::dyn_cast<LiteralPrivateName>(V)) {
      return objLayout.count(LPN);
    }
    if (auto *LN = llvh::dyn_cast<LiteralNumber>(V)) {
      return objLayout.count(convertNumber(LN));
    }
    return false;
  };

  const bool isTypedObject = llvh::isa<AllocTypedObjectInst>(alloc) ||
      llvh::isa<AllocTypedNonEnumObjectInst>(alloc);

  // The object is used through the allocation and through its trusted narrows
  // (e.g. to the type of a typed object literal or class instance).
  llvh::SmallVector<Instruction *, 4> aliases;
  collectObjectAliases(alloc, aliases);
  /// \return true if \p V is the allocated object.
  auto isAlias = [&aliases](Value *V) -> bool {
    return llvh::is_contained(aliases, V);
  };
  // The users of the object, other than the narrows in aliases.
  llvh::SmallVector<Instruction *, 8> users;
  for (Instruction *A : aliases)
    for (Instruction *U : A->getUsers())
      if (!llvh::isa<UnionNarrowTrustedInst>(U))
        users.push_back(U);

  for (auto *U : users) {
    // Loading from the object does not escape.
    if ([[maybe_unused]] auto *L = llvh::dyn_cast<PrLoadInst>(U)) {
      assert(isAlias(L->getObject()) && "Load from a different object");
      continue;
    }
    if ([[maybe_unused]] auto *LP = llvh::dyn_cast<TypedLoadParentInst>(U)) {
      assert(isAlias(LP->getObject()) && "Load from a different object");
      // The parent must be specified in the AllocObjectLiteralInst for us to
      // guarantee that a LoadStack for it will be preceded by a StoreStack.
      if (llvh::isa<EmptySentinel>(alloc->getParentObject()))
        return false;
      continue;
    }

    // Storing into the object is fine, but storing the object anywhere
    // (including into itself) means we cannot elide the allocation.
    if (auto *S = llvh::dyn_cast<PrStoreInst>(U)) {
      if (!isAlias(S->getStoredValue())) {
        assert(isAlias(S->getObject()) && "Unknown usage of object.");
        continue;
      }
    }

    if (auto *SP = llvh::dyn_cast<StorePropertyInst>(U)) {
      // Untyped property stores on typed objects can throw at runtime.
      // Replacing them with stack operations would remove that observable
      // behavior.
      if (isTypedObject)
        return false;
      if (isAlias(SP->getStoredValue()))
        return false;
      if (!isInLayout(SP->getProperty()))
        return false;
      continue;
    }

    if (auto *SOP = llvh::dyn_cast<DefineOwnPropertyInst>(U)) {
      // Typed object properties are non-configurable, redefining fails at
      // runtime.
      if (isTypedObject)
        return false;
      if (isAlias(SOP->getStoredValue()))
        return false;
      if (!isInLayout(SOP->getProperty()))
        return false;
      continue;
    }

    if (auto *LP = llvh::dyn_cast<LoadPropertyInst>(U)) {
      if (!isInLayout(LP->getProperty()))
        return false;
      continue;
    }

    // For now, all other instructions are considered to escape.
    return false;
  }

  // The object does not escape, create a stack location for each of its fields.
  builder.setLocation(alloc->getLocation());
  builder.setInsertionPoint(alloc);
  llvh::SmallVector<AllocStackInst *, 8> stackLocs;
  stackLocs.reserve(numElems);

  auto *parentLoc =
      builder.createAllocStackInst("[parent]", Type::createAnyType());
  // If a parent is specified, initialize the parent stack location. Note that
  // if no parent is specified in the AllocObjectLiteralInst, we cannot have any
  // loads here, so the IR will still be valid. Technically, if the parent is
  // not specified, we know it will be Object.prototype, but we don't have a way
  // of retrieving that here.
  if (!llvh::isa<EmptySentinel>(alloc->getParentObject()))
    builder.createStoreStackInst(alloc->getParentObject(), parentLoc);
  auto numericPropName = builder.createIdentifier("[numeric prop]");
  for (size_t i = 0; i < numElems; ++i) {
    auto *LS = llvh::dyn_cast<LiteralString>(alloc->getKey(i));
    auto name = LS ? LS->getValue() : numericPropName;
    Type valueType = alloc->getValue(i)->getType();
    TypeContext &tc = builder.getTypeContext();
    auto *loc = builder.createAllocStackInst(
        name,
        tc.canBeUninit(valueType)
            ? tc.unionTy(Type::createAnyType(), Type::createUninit())
            : Type::createAnyType());
    stackLocs.push_back(loc);
    builder.createStoreStackInst(alloc->getValue(i), loc);
  }

  /// \return the AllocStackInst replacement of the given property key \p
  /// propKey.
  auto stackLocOfPropKey =
      [&stackLocs, &objLayout, &conversionMapping](Literal *propKey) {
        if (auto *LS = llvh::dyn_cast<LiteralString>(propKey)) {
          return stackLocs[objLayout[LS]];
        }
        if (auto *LPN = llvh::dyn_cast<LiteralPrivateName>(propKey)) {
          return stackLocs[objLayout[LPN]];
        }
        auto *LN = llvh::cast<LiteralNumber>(propKey);
        assert(
            conversionMapping.count(LN) &&
            "numeric key should have already been converted");
        return stackLocs[objLayout[conversionMapping[LN]]];
      };

  for (auto *U : users) {
    builder.setLocation(U->getLocation());
    builder.setInsertionPoint(U);
    destroyer.add(U);

    // Replace loads with loads from the stack location. Narrow the result type
    // to honor the original type.
    if (auto *L = llvh::dyn_cast<PrLoadInst>(U)) {
      auto *load = builder.createLoadStackInst(stackLocs[L->getPropIndex()]);
      auto *narrowed = builder.createUnionNarrowTrustedInst(load, L->getType());
      L->replaceAllUsesWith(narrowed);
      continue;
    }
    if (auto *LP = llvh::dyn_cast<TypedLoadParentInst>(U)) {
      auto *load = builder.createLoadStackInst(parentLoc);
      auto *narrowed =
          builder.createUnionNarrowTrustedInst(load, LP->getType());
      LP->replaceAllUsesWith(narrowed);
      continue;
    }

    // Replace stores with stores to the stack location.
    if (auto *S = llvh::dyn_cast<PrStoreInst>(U)) {
      builder.createStoreStackInst(
          S->getStoredValue(), stackLocs[S->getPropIndex()]);
      continue;
    }

    if (auto *LP = llvh::dyn_cast<LoadPropertyInst>(U)) {
      auto *replace = builder.createLoadStackInst(
          stackLocOfPropKey(llvh::cast<Literal>(LP->getProperty())));
      LP->replaceAllUsesWith(replace);
      continue;
    }

    if (auto *SP = llvh::dyn_cast<StorePropertyInst>(U)) {
      auto *replace = builder.createStoreStackInst(
          SP->getStoredValue(),
          stackLocOfPropKey(llvh::cast<Literal>(SP->getProperty())));
      SP->replaceAllUsesWith(replace);
      continue;
    }

    if (auto *SOP = llvh::dyn_cast<DefineOwnPropertyInst>(U)) {
      auto *replace = builder.createStoreStackInst(
          SOP->getStoredValue(),
          stackLocOfPropKey(llvh::cast<Literal>(SOP->getProperty())));
      SOP->replaceAllUsesWith(replace);
      continue;
    }

    llvm_unreachable("Unhandled instruction");
  }
  ObjsEliminated++;
  // Destroy the narrows after their users and before their operands; the
  // allocation is aliases[0].
  for (Instruction *A : llvh::reverse(aliases))
    destroyer.add(A);

  return true;
}

bool runObjectStackPromotion(Function *F) {
  bool changed = false;
  // Iterate over all instructions in the function and try to promote any
  // BaseAllocObjectLiteralInsts.
  IRBuilder::InstructionDestroyer destroyer;
  for (auto &BB : *F)
    for (auto &I : BB)
      if (auto *alloc = llvh::dyn_cast<BaseAllocObjectLiteralInst>(&I))
        changed |= tryPromoteObject(alloc, destroyer);
  return changed;
}

} // namespace

Pass *createObjectStackPromotion() {
  class ThisPass : public FunctionPass {
   public:
    explicit ThisPass() : FunctionPass("ObjectStackPromotion") {}
    ~ThisPass() override = default;

    bool runOnFunction(Function *M) override {
      return runObjectStackPromotion(M);
    }
  };
  return new ThisPass();
}

} // namespace hermes
#undef DEBUG_TYPE
