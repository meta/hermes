/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/Optimizer/PassManager/PassManager.h"

#include "gtest/gtest.h"

#include <limits>

using namespace hermes;

namespace {

/// LowerBuiltinCalls peepholes a CallBuiltinInst for Math.imul into ImulInst
/// wherever it finds one, not only where it just created one from a resolved
/// Math.imul call. No JavaScript source produces such an instruction on its
/// own: the pass is the compiler's only producer of it, and it rewrites the
/// one it creates in the same walk. Building the IR directly is therefore the
/// only way to hand the pass a builtin call it did not make itself, which is
/// both the case a lit test cannot reach and the one a future IR producer
/// would land in.
class LowerBuiltinCallsTest : public ::testing::Test {
 public:
  LowerBuiltinCallsTest()
      : context_(std::make_shared<Context>()),
        module_(context_),
        builder_(&module_) {
    auto *top = builder_.createFunction(
        "global", Function::DefinitionKind::ES5Function, true);
    module_.setTopLevelFunction(top);
    builder_.setInsertionBlock(builder_.createBasicBlock(top));
    builder_.createReturnInst(builder_.getLiteralUndefined());
  }

 protected:
  /// Distinct marker operands, so a mixed up operand order is visible.
  static constexpr double kFirst = 7;
  static constexpr double kSecond = 11;
  static constexpr double kThird = 13;

  /// Build a function returning CallBuiltin(Math.imul) applied to the first
  /// \p numArgs markers. \return the function.
  Function *buildImulBuiltinCall(llvh::StringRef name, unsigned numArgs) {
    auto *F = builder_.createFunction(
        name, Function::DefinitionKind::ES5Function, true);
    builder_.setInsertionBlock(builder_.createBasicBlock(F));

    const double markers[] = {kFirst, kSecond, kThird};
    llvh::SmallVector<Value *, 3> args{};
    for (unsigned i = 0; i < numArgs; ++i)
      args.push_back(builder_.getLiteralNumber(markers[i]));

    auto *call = builder_.createCallBuiltinInst(BuiltinMethod::Math_imul, args);
    builder_.createReturnInst(call);
    return F;
  }

  void runLowerBuiltinCalls() {
    PassManager PM("LowerBuiltinCallsTest");
    PM.addLowerBuiltinCalls();
    PM.run(&module_);
  }

  /// \return the value \p F returns, as an ImulInst, or nullptr.
  static ImulInst *returnedImul(Function *F) {
    auto *ret = llvh::cast<ReturnInst>(F->front().getTerminator());
    return llvh::dyn_cast<ImulInst>(ret->getValue());
  }

  /// \return the value of \p v, or NaN if it is not a literal number.
  static double literal(Value *v) {
    auto *num = llvh::dyn_cast<LiteralNumber>(v);
    return num ? num->getValue() : std::numeric_limits<double>::quiet_NaN();
  }

  /// Assert that \p F returns ImulInst(\p left, \p right).
  static void expectImul(Function *F, double left, double right) {
    auto *imul = returnedImul(F);
    ASSERT_NE(nullptr, imul)
        << "no ImulInst in " << F->getInternalNameStr().str();
    EXPECT_EQ(left, literal(imul->getLeft()));
    EXPECT_EQ(right, literal(imul->getRight()));
  }

  std::shared_ptr<Context> context_;
  Module module_;
  IRBuilder builder_;
};

/// Math.imul reads exactly two arguments. A missing one is undefined and
/// ToInt32(undefined) is 0, so the peephole pads with the literal 0; the
/// arguments past the second are dropped.
TEST_F(LowerBuiltinCallsTest, ImulBuiltinCallIsLoweredAtEveryArity) {
  Function *noArgs = buildImulBuiltinCall("noArgs", 0);
  Function *oneArg = buildImulBuiltinCall("oneArg", 1);
  Function *twoArgs = buildImulBuiltinCall("twoArgs", 2);
  Function *threeArgs = buildImulBuiltinCall("threeArgs", 3);

  runLowerBuiltinCalls();

  expectImul(noArgs, 0, 0);
  expectImul(oneArg, kFirst, 0);
  expectImul(twoArgs, kFirst, kSecond);
  expectImul(threeArgs, kFirst, kSecond);
}

/// The builtin call is erased, not merely left unused alongside the
/// instruction.
TEST_F(LowerBuiltinCallsTest, ImulBuiltinCallIsErased) {
  Function *F = buildImulBuiltinCall("oneArg", 1);

  runLowerBuiltinCalls();

  for (auto &BB : *F)
    for (auto &inst : BB)
      EXPECT_FALSE(llvh::isa<CallBuiltinInst>(&inst));
}

/// A builtin call that is not Math.imul is left exactly as it was.
TEST_F(LowerBuiltinCallsTest, OtherBuiltinCallsAreUntouched) {
  auto *F = builder_.createFunction(
      "hypot", Function::DefinitionKind::ES5Function, true);
  builder_.setInsertionBlock(builder_.createBasicBlock(F));
  Value *args[] = {builder_.getLiteralNumber(kFirst)};
  auto *call = builder_.createCallBuiltinInst(BuiltinMethod::Math_hypot, args);
  builder_.createReturnInst(call);

  runLowerBuiltinCalls();

  auto *ret = llvh::cast<ReturnInst>(F->front().getTerminator());
  auto *stillBuiltin = llvh::dyn_cast<CallBuiltinInst>(ret->getValue());
  ASSERT_NE(nullptr, stillBuiltin);
  EXPECT_EQ(BuiltinMethod::Math_hypot, stillBuiltin->getBuiltinIndex());
}

} // namespace
