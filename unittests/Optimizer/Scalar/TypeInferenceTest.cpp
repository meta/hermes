/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/IR/IR.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/Optimizer/PassManager/PassManager.h"

#include "gtest/gtest.h"

#include "llvh/Support/raw_ostream.h"

using namespace hermes;

namespace {

/// Print \p t for a failure message; gtest streams to std::ostream, which
/// cannot take a PrintedType.
std::string typeStr(const TypeContext &tc, Type t) {
  std::string s;
  llvh::raw_string_ostream os(s);
  os << tc.fmt(t);
  return os.str();
}

/// TypeInference runs before LowerCalls in every compiler pipeline, so no
/// compiler pipeline hands it an HBCCallWithArgCountInst. Building the IR
/// directly is the only way to reach that case.
TEST(TypeInferenceTest, HBCCallWithArgCountTakesCalleeReturnType) {
  auto context = std::make_shared<Context>();
  Module M(context);
  IRBuilder builder(&M);

  auto *F = builder.createFunction(
      "callee", Function::DefinitionKind::ES5Function, true);
  builder.setInsertionBlock(builder.createBasicBlock(F));
  builder.createReturnInst(builder.getLiteralNumber(1));

  auto *top = builder.createTopLevelFunction("global", true);
  builder.setInsertionBlock(builder.createBasicBlock(top));
  auto *call = builder.createHBCCallWithArgCount(
      builder.getLiteralUndefined(),
      F,
      false,
      builder.getEmptySentinel(),
      builder.getLiteralUndefined(),
      builder.getLiteralNumber(1),
      builder.getLiteralUndefined(),
      {});
  builder.createReturnInst(call);

  PassManager PM("TypeInferenceTest");
  PM.addTypeInference();
  PM.run(&M);

  TypeContext &tc = M.getTypeContext();
  EXPECT_EQ(F->getReturnType(), Type::createNumber())
      << "callee return type: " << typeStr(tc, F->getReturnType());
  EXPECT_EQ(call->getType(), Type::createNumber())
      << "call type: " << typeStr(tc, call->getType());
}

} // namespace
