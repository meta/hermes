/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "builtins"

#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Support/Statistic.h"
#include "hermes/Support/StringTable.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/Support/Debug.h"

STATISTIC(NumLowered, "Number of builtin calls lowered");
STATISTIC(NumImul, "Number of Math.imul builtin calls turned into ImulInst");

/// Detect calls to builtin methods like `Object.keys()` and replace them with
/// CallBuiltinInst.
///
/// LowerBuiltinCallsOptimized will also add fast paths for certain known
/// callees.

namespace hermes {

class LowerBuiltinCallsContext {
 public:
  LowerBuiltinCallsContext(StringTable &strTab);

  static LowerBuiltinCallsContext &get(Module *M) {
    auto &optContext = M->getOptimizationContext();
    if (!optContext.lowerBuiltinCallsContext)
      optContext.lowerBuiltinCallsContext =
          std::make_shared<LowerBuiltinCallsContext>(
              M->getContext().getStringTable());

    return *optContext.lowerBuiltinCallsContext;
  }

  /// Look for a builtin method \c object.method.
  hermes::OptValue<BuiltinMethod::Enum> findBuiltinMethod(
      Identifier objectName,
      Identifier methodName);

  /// \return whether this is a callee name we should try and emit a fast path
  /// for.
  bool shouldTryOptimizeCalleeName(Identifier name) {
    return calleeNamesToTryOptimize_.count(name);
  }

 public:
  /// Identifier of "HermesInternal".
  const Identifier hermesInternalID;

  /// Identifier of "apply".
  const Identifier applyID;
  /// Identifier of "call".
  const Identifier callID;

 private:
  /// Map from a builtin object to an integer "object index".
  llvh::DenseMap<Identifier, int> objects_;

  /// Map from "object index":identifier to a "method index".
  /// We are avoiding allocating a DenseMap per object.
  llvh::DenseMap<std::pair<int, Identifier>, BuiltinMethod::Enum> methods_;

  /// Set of callee names that we should try to optimize.
  llvh::DenseSet<Identifier> calleeNamesToTryOptimize_{};
};

LowerBuiltinCallsContext::LowerBuiltinCallsContext(StringTable &strTab)
    : hermesInternalID(strTab.getIdentifier("HermesInternal")),
      applyID(strTab.getIdentifier("apply")),
      callID(strTab.getIdentifier("call")) {
  // First insert all objects.
  int objIndex = 0;
#define NORMAL_OBJECT(object)
#define NORMAL_METHOD(object, name)
#define BUILTIN_OBJECT(name) objects_[strTab.getIdentifier(#name)] = objIndex++;
#include "hermes/FrontEndDefs/Builtins.def"

  // Now insert all methods.
#define NORMAL_OBJECT(object)
#define NORMAL_METHOD(object, name)
#define BUILTIN_METHOD(object, name)                                           \
  methods_[std::make_pair(                                                     \
      objects_[strTab.getIdentifier(#object)], strTab.getIdentifier(#name))] = \
      BuiltinMethod::object##_##name;
#include "hermes/FrontEndDefs/Builtins.def"

  calleeNamesToTryOptimize_.insert(applyID);
  calleeNamesToTryOptimize_.insert(callID);
}

hermes::OptValue<BuiltinMethod::Enum>
LowerBuiltinCallsContext::findBuiltinMethod(
    Identifier objectName,
    Identifier methodName) {
  auto objIt = objects_.find(objectName);
  if (objIt == objects_.end())
    return llvh::None;

  auto methIt = methods_.find(std::make_pair(objIt->second, methodName));
  if (methIt == methods_.end())
    return llvh::None;

  return methIt->second;
}

/// Attempt to lower the \p callInst to a static builtin call.
/// Look for an instruction sequence of the kind:
///    (Call
///        (LoadProperty
///            (LoadProperty globalObject, "ObjectName")
///            Prop)
///        ...)
/// where \c ObjectName.Prop is a pre-defined builtin.
/// Note: we only want to check for call. Not for construct.
/// \param callInst the call instruction to lower.
/// \param loadProp the load property instruction that loads the builtin method.
/// \param propLit the string literal that is the property name.
/// \return the CallBuiltinInst that replaced \p callInst, or nullptr if it was
///   left alone. The caller needs the replacement so it can run the same
///   peephole over it as over a builtin call that was already there.
static CallBuiltinInst *tryLowerStaticBuiltin(
    IRBuilder &builder,
    Function *F,
    CallInst *callInst,
    BaseLoadPropertyInst *loadProp,
    LiteralString *propLit) {
  auto &builtins = LowerBuiltinCallsContext::get(F->getParent());

  auto *loadGlobalProp =
      llvh::dyn_cast<BaseLoadPropertyInst>(loadProp->getObject());
  if (!loadGlobalProp)
    return nullptr;
  if (!llvh::isa<GlobalObject>(loadGlobalProp->getObject()))
    return nullptr;
  LiteralString *objLit =
      llvh::dyn_cast<LiteralString>(loadGlobalProp->getProperty());
  if (!objLit)
    return nullptr;

  // Uncomment this to get a dump of all global calls detected during
  // compilation.
  // llvh::outs() << "global call: " << objLit->getValue() << "."
  //             << propLit->getValue() << "\n";

  auto builtinIndex =
      builtins.findBuiltinMethod(objLit->getValue(), propLit->getValue());
  if (!builtinIndex)
    return nullptr;

  LLVM_DEBUG(
      llvh::dbgs() << "Found builtin [" << (int)*builtinIndex << "] "
                   << getBuiltinMethodName(*builtinIndex) << "()\n");

  // Always lower HermesInternal.xxx() calls, but only lower the rest if
  // -fstatic-builtins is enabled.
  if (objLit->getValue() != builtins.hermesInternalID &&
      !F->getContext().getOptimizationSettings().staticBuiltins) {
    return nullptr;
  }

  builder.setInsertionPoint(callInst);
  builder.setLocation(callInst->getLocation());

  // The rewrite discards argument 0, the receiver: getNumArguments() counts
  // it, and a builtin call does not read it, so there is no condition on its
  // value.
  llvh::SmallVector<Value *, 8> args{};
  unsigned numArgsExcludingThis = callInst->getNumArguments() - 1;
  args.reserve(numArgsExcludingThis);
  for (unsigned i = 0; i < numArgsExcludingThis; ++i)
    args.push_back(callInst->getArgument(i + 1));

  CallBuiltinInst *replacement =
      builder.createCallBuiltinInst(*builtinIndex, args);
  callInst->replaceAllUsesWith(replacement);
  callInst->eraseFromParent();

  // The property access instructions are not normally optimizable since
  // they have side effects, but in this case it is safe to remove them.
  if (!loadProp->hasUsers())
    loadProp->eraseFromParent();
  if (!loadGlobalProp->hasUsers())
    loadGlobalProp->eraseFromParent();

  ++NumLowered;
  return replacement;
}

/// Replace a builtin call to Math.imul with ImulInst, which computes it
/// inline instead of calling out to the builtin. This runs on every
/// CallBuiltinInst the pass walks over, not only on the ones it creates
/// itself, so a Math.imul builtin call from any producer running before this
/// pass becomes the instruction as well. The pass runs in both backend
/// lowering pipelines at every optimization level, which today is every
/// producer there is.
/// \return true if \p callBuiltin was replaced.
static bool tryLowerImulBuiltin(
    IRBuilder &builder,
    CallBuiltinInst *callBuiltin) {
  if (callBuiltin->getBuiltinIndex() != BuiltinMethod::Math_imul)
    return false;

  builder.setInsertionPoint(callBuiltin);
  builder.setLocation(callBuiltin->getLocation());

  // Argument 0 is the receiver, which Math.imul ignores. It reads exactly two
  // arguments: a missing one is undefined and ToInt32(undefined) is 0, so pad
  // with the literal 0 instead. Same value, but a literal number leaves the
  // padded operand statically numeric, so ImulInst can still be idempotent
  // when the arguments that were supplied are numbers -- padding with
  // undefined would make it unconditionally opaque. Arguments past the second
  // are dropped; they were already evaluated by their own instructions, which
  // is exactly what the builtin does with them.
  auto arg = [callBuiltin, &builder](unsigned index) -> Value * {
    return index + 1 < callBuiltin->getNumArguments()
        ? callBuiltin->getArgument(index + 1)
        : builder.getLiteralNumber(0);
  };
  // Evaluation order of function arguments is unspecified, so read the
  // operands into locals first.
  Value *left = arg(0);
  Value *right = arg(1);

  auto *imul = builder.createImulInst(left, right);
  callBuiltin->replaceAllUsesWith(imul);
  callBuiltin->eraseFromParent();

  ++NumImul;
  return true;
}

static bool tryOptimizeKnownCallable(
    IRBuilder &builder,
    Function *F,
    CallInst *callInst,
    BaseLoadPropertyInst *loadProp,
    LiteralString *propLit) {
  assert(callInst && loadProp && propLit && "no nullptrs");

  auto &builtins = LowerBuiltinCallsContext::get(F->getParent());
  IRBuilder::InstructionDestroyer destroyer;

  if (propLit->getValue() == builtins.callID) {
    // Add a fast path for "call" by splitting the basic block and trying to
    // simply call the function if it's known to be Function.prototype.call.
    BasicBlock *oldBB = callInst->getParent();
    BasicBlock *newBB = splitBasicBlock(oldBB, callInst->getIterator());

    BasicBlock *fastBB = builder.createBasicBlock(F);
    BasicBlock *slowBB = builder.createBasicBlock(F);

    builder.setInsertionBlock(oldBB);
    builder.createBranchIfBuiltinInst(
        BuiltinMethod::HermesBuiltin_functionPrototypeCall,
        callInst->getCallee(),
        fastBB,
        slowBB);

    builder.setInsertionBlock(fastBB);
    llvh::SmallVector<Value *, 8> args{};
    // f.call(this, arg1, arg2, ...)
    //              ^ start here
    // CallInst.getArgument(0) is the "this" value.
    for (unsigned i = 2, e = callInst->getNumArguments(); i < e; ++i) {
      args.push_back(callInst->getArgument(i));
    }
    auto *fastCall = builder.createCallInst(
        callInst->getThis(),
        /* newTarget */ builder.getLiteralUndefined(),
        /* thisValue */ callInst->getNumArguments() > 1
            ? callInst->getArgument(1)
            : builder.getLiteralUndefined(),
        args);
    builder.createBranchInst(newBB);

    builder.setInsertionBlock(slowBB);
    auto *branchToNew = builder.createBranchInst(newBB);
    callInst->moveBefore(branchToNew);

    builder.setInsertionPoint(&*newBB->begin());
    auto *phi = builder.createPhiInst();
    callInst->replaceAllUsesWith(phi);
    phi->addEntry(fastCall, fastBB);
    phi->addEntry(callInst, slowBB);

    return true;
  }

  if (propLit->getValue() == builtins.applyID &&
      callInst->getNumArguments() == 3 &&
      llvh::isa<CreateArgumentsInst>(callInst->getArgument(2)) &&
      callInst->getArgument(2)->hasOneUser()) {
    // We require that the CreateArgumentsInst has only one user: this call.
    // LoadProperty may invoke a getter on Object.prototype and leak arguments.
    // Other calls must be validated to be safe as well.
    // TODO: Handle more complex cases.

    // Add a fast path for "apply(thisVal, arguments)"
    // by splitting the basic block and trying to do a "fast apply" by copying
    // the arguments directly via HermesBuiltin_applyArguments.
    BasicBlock *oldBB = callInst->getParent();
    BasicBlock *newBB = splitBasicBlock(oldBB, callInst->getIterator());

    BasicBlock *slowBB = builder.createBasicBlock(F);
    BasicBlock *fastBB = builder.createBasicBlock(F);

    builder.setInsertionBlock(oldBB);
    builder.createBranchIfBuiltinInst(
        BuiltinMethod::HermesBuiltin_functionPrototypeApply,
        callInst->getCallee(),
        fastBB,
        slowBB);

    builder.setInsertionBlock(fastBB);
    auto *fastCall = builder.createCallBuiltinInst(
        BuiltinMethod::HermesBuiltin_applyArguments,
        {callInst->getThis(), callInst->getArgument(1)});
    builder.createBranchInst(newBB);

    builder.setInsertionBlock(slowBB);
    auto *branchToNew = builder.createBranchInst(newBB);
    callInst->moveBefore(branchToNew);

    builder.setInsertionPoint(&*newBB->begin());
    auto *phi = builder.createPhiInst();
    callInst->replaceAllUsesWith(phi);
    phi->addEntry(fastCall, fastBB);
    phi->addEntry(callInst, slowBB);

    return true;
  }

  return false;
}

static bool run(Function *F, bool optimize) {
  IRBuilder builder{F};
  bool changed = false;

  // Use a worklist for optimizing calls into fast paths to avoid modifying the
  // BB and Inst lists while iterating them.
  // Contains CallInst for which the callee is LoadPropertyInst with a literal
  // name belonging to calleeNamesToTryOptimize_.
  llvh::SmallVector<CallInst *, 8> callsToOptimize{};

  auto &builtins = LowerBuiltinCallsContext::get(F->getParent());

  for (auto &BB : *F) {
    for (auto it = BB.begin(), e = BB.end(); it != e;) {
      // Get a pointer to the instruction and increment the iterator so we
      // can delete the instruction if we want to.
      Instruction *inst = &*it++;

      // CallBuiltinInst is a sibling of CallInst, not a subclass, so an
      // existing builtin call skips this block and goes straight to the
      // peephole below.
      if (inst->getKind() == ValueKind::CallInstKind) {
        auto *callInst = cast<CallInst>(inst);
        auto *loadProp =
            llvh::dyn_cast<BaseLoadPropertyInst>(callInst->getCallee());
        if (!loadProp)
          continue;
        auto propLit = llvh::dyn_cast<LiteralString>(loadProp->getProperty());
        if (!propLit)
          continue;

        CallBuiltinInst *lowered =
            tryLowerStaticBuiltin(builder, F, callInst, loadProp, propLit);
        if (!lowered) {
          if (optimize &&
              builtins.shouldTryOptimizeCalleeName(propLit->getValue())) {
            // Add to the worklist for later processing. We can't process it
            // now because we don't want to alter the BasicBlock list.
            callsToOptimize.push_back(callInst);
          }
          continue;
        }
        changed = true;
        // Fall through, so the builtin call we just created is peepholed
        // exactly like one that was already here.
        inst = lowered;
      }

      if (auto *callBuiltin = llvh::dyn_cast<CallBuiltinInst>(inst))
        changed |= tryLowerImulBuiltin(builder, callBuiltin);
    }
  }

  // Optimize calls to known callables.
  for (auto *callInst : callsToOptimize) {
    auto *loadProp = llvh::cast<BaseLoadPropertyInst>(callInst->getCallee());
    auto *propLit = llvh::cast<LiteralString>(loadProp->getProperty());
    changed |=
        tryOptimizeKnownCallable(builder, F, callInst, loadProp, propLit);
  }

  return changed;
}

Pass *createLowerBuiltinCalls() {
  class ThisPass : public FunctionPass {
   public:
    explicit ThisPass() : FunctionPass("LowerBuiltinCalls") {}
    ~ThisPass() override = default;

    bool runOnFunction(Function *F) override {
      return run(F, false);
    }
  };
  return new ThisPass();
}

Pass *createLowerBuiltinCallsOptimized() {
  class ThisPass : public FunctionPass {
   public:
    explicit ThisPass() : FunctionPass("LowerBuiltinCallsOptimized") {}
    ~ThisPass() override = default;

    bool runOnFunction(Function *F) override {
      return run(F, true);
    }
  };
  return new ThisPass();
}

} // namespace hermes

#undef DEBUG_TYPE
