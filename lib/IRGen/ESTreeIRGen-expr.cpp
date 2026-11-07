/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ESTreeIRGen.h"
#include "hermes/Regex/RegexSerialization.h"
#include "hermes/Support/UTF8.h"

#include "llvh/ADT/ScopeExit.h"
#include "llvh/ADT/StringSwitch.h"

namespace hermes {
namespace irgen {

Value *ESTreeIRGen::narrowToFlowType(
    hermes::Value *value,
    flow::Type *flowType,
    ESTree::Node *loc) {
  if (!flowType)
    return value;

  Type targetIRType = flowTypeToIRType(flowType);
  Type valueType = value->getType();
  TypeContext &tc = getTypeContext();
  // NOTE: equal sets are subsets of each other, but we want to do the fast
  // check first. In theory, it should catsh 100% of the cases when IRGen is
  // fully typed.
  if (targetIRType == valueType || tc.isSubsetOf(valueType, targetIRType))
    return value;

  if (!tc.isSubsetOf(targetIRType, valueType)) {
    Mod->getContext().getSourceErrorManager().error(
        loc->getSourceRange(),
        "Internal error: Flow expr type is not a subset of value type");
    return value;
  }

  Instruction *cast = Builder.createCheckedTypeCastInst(value, targetIRType);
  cast->setLocation(loc->getDebugLoc());
  return cast;
}

Value *ESTreeIRGen::enforceExprType(hermes::Value *value, ESTree::Node *expr) {
  return narrowToFlowType(value, flowContext_.findNodeType(expr), expr);
}

Value *ESTreeIRGen::enforceDeclType(
    hermes::Value *value,
    ESTree::IdentifierNode *id) {
  if (id->isUnresolvable())
    return value;
  sema::Decl *decl = semCtx_.getExpressionDecl(id);
  if (!decl)
    return value;
  return narrowToFlowType(value, flowContext_.findDeclType(decl), id);
}

Value *ESTreeIRGen::genExpression(ESTree::Node *expr, Identifier nameHint) {
  return enforceExprType(_genExpressionImpl(expr, nameHint), expr);
}

Value *ESTreeIRGen::_genExpressionImpl(
    ESTree::Node *expr,
    Identifier nameHint) {
  LLVM_DEBUG(
      llvh::dbgs() << "IRGen expression of type " << expr->getNodeName()
                   << "\n");
  IRBuilder::ScopedLocationChange slc(Builder, expr->getDebugLoc());

  // Handle identifiers.
  if (auto *Iden = llvh::dyn_cast<ESTree::IdentifierNode>(expr)) {
    return genIdentifierExpression(Iden, false);
  }

  // Handle Null Literals.
  // http://www.ecma-international.org/ecma-262/6.0/#sec-null-literals
  if (llvh::isa<ESTree::NullLiteralNode>(expr)) {
    return Builder.getLiteralNull();
  }

  // Handle String Literals.
  // http://www.ecma-international.org/ecma-262/6.0/#sec-literals-string-literals
  if (auto *Lit = llvh::dyn_cast<ESTree::StringLiteralNode>(expr)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading String Literal \"" << Lit->_value << "\"\n");
    return Builder.getLiteralString(Lit->_value->str());
  }

  // Handle Regexp Literals.
  // http://www.ecma-international.org/ecma-262/6.0/#sec-literals-regular-expression-literals
  if (auto *Lit = llvh::dyn_cast<ESTree::RegExpLiteralNode>(expr)) {
    return genRegExpLiteral(Lit);
  }

  // Handle Boolean Literals.
  // http://www.ecma-international.org/ecma-262/6.0/#sec-boolean-literals
  if (auto *Lit = llvh::dyn_cast<ESTree::BooleanLiteralNode>(expr)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading String Literal \"" << Lit->_value << "\"\n");
    return Builder.getLiteralBool(Lit->_value);
  }

  // Handle Number Literals.
  // http://www.ecma-international.org/ecma-262/6.0/#sec-literals-numeric-literals
  if (auto *Lit = llvh::dyn_cast<ESTree::NumericLiteralNode>(expr)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading Numeric Literal \"" << Lit->_value << "\"\n");
    return Builder.getLiteralNumber(Lit->_value);
  }

  // Handle BigInt Literals.
  // https://262.ecma-international.org/#sec-ecmascript-language-types-bigint-type
  if (auto *Lit = llvh::dyn_cast<ESTree::BigIntLiteralNode>(expr)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading BitInt Literal \"" << Lit->_bigint->str()
                     << "\"\n");
    return Builder.getLiteralBigInt(Lit->_bigint);
  }

  // Handle the assignment expression.
  if (auto Assign = llvh::dyn_cast<ESTree::AssignmentExpressionNode>(expr)) {
    return genAssignmentExpr(Assign);
  }

  // Handle Call expressions.
  if (auto *call = llvh::dyn_cast<ESTree::CallExpressionNode>(expr)) {
    return genCallExpr(call);
  }

  // Handle Call expressions.
  if (auto *call = llvh::dyn_cast<ESTree::OptionalCallExpressionNode>(expr)) {
    return genOptionalCallExpr(call, nullptr);
  }

  // Handle the 'new' expressions.
  if (auto *newExp = llvh::dyn_cast<ESTree::NewExpressionNode>(expr)) {
    return genNewExpr(newExp);
  }

  // Handle MemberExpression expressions for access property.
  if (auto *Mem = llvh::dyn_cast<ESTree::MemberExpressionNode>(expr)) {
    return genMemberExpression(Mem, MemberExpressionOperation::Load).result;
  }

  // Handle MemberExpression expressions for access property.
  if (auto *mem = llvh::dyn_cast<ESTree::OptionalMemberExpressionNode>(expr)) {
    return genOptionalMemberExpression(
               mem, nullptr, MemberExpressionOperation::Load)
        .result;
  }

  // Handle Array expressions (syntax: [1,2,3]).
  if (auto *Arr = llvh::dyn_cast<ESTree::ArrayExpressionNode>(expr)) {
    return genArrayExpr(Arr);
  }

  // Handle object expressions (syntax: {"1" : "2"}).
  if (auto *Obj = llvh::dyn_cast<ESTree::ObjectExpressionNode>(expr)) {
    return genObjectExpr(Obj);
  }

  // Handle logical expressions (short circuiting).
  if (auto *L = llvh::dyn_cast<ESTree::LogicalExpressionNode>(expr)) {
    return genLogicalExpression(L);
  }

  // Handle Binary Expressions.
  if (auto *Bin = llvh::dyn_cast<ESTree::BinaryExpressionNode>(expr)) {
    return genBinaryExpression(Bin);
  }

  // Handle Unary operator Expressions.
  if (auto *U = llvh::dyn_cast<ESTree::UnaryExpressionNode>(expr)) {
    return genUnaryExpression(U);
  }

  // Handle the 'this' keyword.
  if (llvh::isa<ESTree::ThisExpressionNode>(expr)) {
    return genThisExpression();
  }

  if (auto *MP = llvh::dyn_cast<ESTree::MetaPropertyNode>(expr)) {
    return genMetaProperty(MP);
  }

  // Handle function expressions.
  if (auto *FE = llvh::dyn_cast<ESTree::FunctionExpressionNode>(expr)) {
    return genFunctionExpression(FE, nameHint);
  }

  if (auto *AF = llvh::dyn_cast<ESTree::ArrowFunctionExpressionNode>(expr)) {
    return genArrowFunctionExpression(AF, nameHint);
  }

  if (auto *U = llvh::dyn_cast<ESTree::UpdateExpressionNode>(expr)) {
    return genUpdateExpr(U);
  }

  if (auto *C = llvh::dyn_cast<ESTree::ConditionalExpressionNode>(expr)) {
    return genConditionalExpr(C);
  }

  if (auto *Sq = llvh::dyn_cast<ESTree::SequenceExpressionNode>(expr)) {
    return genSequenceExpr(Sq);
  }

  if (auto *Tl = llvh::dyn_cast<ESTree::TemplateLiteralNode>(expr)) {
    return genTemplateLiteralExpr(Tl);
  }

  if (auto *Tt = llvh::dyn_cast<ESTree::TaggedTemplateExpressionNode>(expr)) {
    return genTaggedTemplateExpr(Tt);
  }

  if (auto *Y = llvh::dyn_cast<ESTree::YieldExpressionNode>(expr)) {
    return Y->_delegate ? genYieldStarExpr(Y) : genYieldExpr(Y);
  }

  if (auto *A = llvh::dyn_cast<ESTree::AwaitExpressionNode>(expr)) {
    return genAwaitExpr(A);
  }

  if (auto *classExpr = llvh::dyn_cast<ESTree::ClassExpressionNode>(expr)) {
    return genLegacyClassExpression(classExpr, nameHint);
  }

  if (auto *ICK = llvh::dyn_cast<ESTree::ImplicitCheckedCastNode>(expr)) {
    // FIXME: emit something.
    return Builder.createCheckedTypeCastInst(
        genExpression(ICK->_argument, nameHint),
        flowTypeToIRType(flowContext_.getNodeTypeOrAny(ICK)));
  }

#if HERMES_PARSE_FLOW
  if (auto *TC = llvh::dyn_cast<ESTree::TypeCastExpressionNode>(expr)) {
    return Builder.createCheckedTypeCastInst(
        genExpression(TC->_expression, nameHint),
        flowTypeToIRType(flowContext_.getNodeTypeOrAny(TC)));
  }
  if (auto *TC = llvh::dyn_cast<ESTree::AsExpressionNode>(expr)) {
    return Builder.createCheckedTypeCastInst(
        genExpression(TC->_expression, nameHint),
        flowTypeToIRType(flowContext_.getNodeTypeOrAny(TC)));
  }
#endif

  Builder.getModule()->getContext().getSourceErrorManager().error(
      expr->getSourceRange(), Twine("Invalid expression encountered"));
  return Builder.getLiteralUndefined();
}

void ESTreeIRGen::genExpressionBranch(
    ESTree::Node *expr,
    BasicBlock *onTrue,
    BasicBlock *onFalse,
    BasicBlock *onNullish) {
  switch (expr->getKind()) {
    case ESTree::NodeKind::LogicalExpression:
      return genLogicalExpressionBranch(
          cast<ESTree::LogicalExpressionNode>(expr),
          onTrue,
          onFalse,
          onNullish);

    case ESTree::NodeKind::UnaryExpression: {
      auto *e = cast<ESTree::UnaryExpressionNode>(expr);
      // Handle `!expr`
      if (e->_operator == kw_.identBang) {
        // Do not propagate onNullish here because !expr cannot be nullish.
        return genExpressionBranch(e->_argument, onFalse, onTrue, nullptr);
      }
      break;
    }

    case ESTree::NodeKind::SequenceExpression: {
      auto *e = cast<ESTree::SequenceExpressionNode>(expr);

      ESTree::NodePtr last = nullptr;
      for (auto &ex : e->_expressions) {
        if (last)
          genExpression(last);
        last = &ex;
      }
      if (last)
        genExpressionBranch(last, onTrue, onFalse, onNullish);
      return;
    }

    default:
      break;
  }

  Value *condVal = genExpression(expr);
  if (onNullish) {
    Value *isNullish = Builder.createBinaryOperatorInst(
        condVal, Builder.getLiteralNull(), ValueKind::BinaryEqualInstKind);
    BasicBlock *notNullishBB = Builder.createBasicBlock(Builder.getFunction());
    Builder.createCondBranchInst(isNullish, onNullish, notNullishBB);
    Builder.setInsertionBlock(notNullishBB);
  }
  Builder.createCondBranchInst(condVal, onTrue, onFalse);
}

Value *ESTreeIRGen::genArrayFromElements(ESTree::NodeList &list) {
  LLVM_DEBUG(llvh::dbgs() << "Initializing a new array\n");
  AllocArrayInst::ArrayValueList elements;

  // Precalculate the minimum number of elements in case we need to call
  // AllocArrayInst at some point, as well as find out whether we have a spread
  // element (which will result in the final array having variable length).
  unsigned minElements = 0;
  bool variableLength = false;
  for (auto &E : list) {
    if (llvh::isa<ESTree::SpreadElementNode>(&E)) {
      variableLength = true;
      continue;
    }
    ++minElements;
  }

  // We store consecutive elements until we encounter elision,
  // or we enounter a non-literal in limited-register mode.
  // The rest of them has to be initialized through property sets.

  // If we have a variable length array, then we store the next index in
  // a stack location `nextIndex`, to be updated when we encounter spread
  // elements.

  // `count` is the number of elements of the list we've seen so far.
  unsigned count = 0;
  AllocStackInst *nextIndex = nullptr;
  if (variableLength) {
    // Avoid emitting the extra instructions unless we actually need to,
    // to simplify tests and because it's easy.
    nextIndex = Builder.createAllocStackInst("nextIndex", Type::createNumber());
    Builder.createStoreStackInst(Builder.getLiteralPositiveZero(), nextIndex);
  }

  bool consecutive = true;
  // Whether we've seen the first spread element yet.
  bool seenSpread = false;
  AllocArrayInst *allocArrayInst = nullptr;
  for (auto &E : list) {
    Value *value{nullptr};
    bool isSpread = false;
    if (!llvh::isa<ESTree::EmptyNode>(&E)) {
      if (auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(&E)) {
        isSpread = true;
        seenSpread = true;
        value = genExpression(spread->_argument);
      } else {
        value = genExpression(&E);
      }
    }
    if (!value || !llvh::isa<Literal>(value) || isSpread) {
      // This is either an elision,
      // or a non-literal in limited-register mode,
      // or a spread element.
      if (consecutive) {
        // So far we have been storing elements consecutively,
        // but not anymore, time to create the array.
        allocArrayInst = Builder.createAllocArrayInst(elements, minElements);
        consecutive = false;
      }
    }
    if (isSpread) {
      // Spread the SpreadElement argument into the array.
      // HermesInternal.arraySpread returns the new value of nextIndex,
      // so update nextIndex accordingly and finish this iteration of the loop.
      auto *newNextIndex = genBuiltinCall(
          BuiltinMethod::HermesBuiltin_arraySpread,
          {allocArrayInst, value, Builder.createLoadStackInst(nextIndex)});
      newNextIndex->setType(Type::createNumber());
      Builder.createStoreStackInst(newNextIndex, nextIndex);
      continue;
    }
    // The element is not a spread element, so perform the store here.
    if (value) {
      if (consecutive) {
        elements.push_back(value);
      } else if (!seenSpread) {
        Builder.createDefineOwnInDenseArrayInst(
            value, allocArrayInst, Builder.getLiteralNumber(count));
      } else {
        assert(variableLength && "seen spread so it must be variable length");
        Builder.createDefineOwnPropertyInst(
            value,
            allocArrayInst,
            cast<Value>(Builder.createLoadStackInst(nextIndex)),
            IRBuilder::PropEnumerable::Yes);
      }
    }
    // Update the next index or the count depending on if it's a variable length
    // array.
    if (variableLength) {
      // We perform this update on any leading elements before the first spread
      // element as well, but the optimizer will eliminate the extra adds
      // because we know the initial value (0) and how incrementing works.
      Builder.createStoreStackInst(
          Builder.createFBinaryMathInst(
              ValueKind::FAddInstKind,
              Builder.createLoadStackInst(nextIndex),
              Builder.getLiteralNumber(1)),
          nextIndex);
    }
    count++;
  }

  if (!allocArrayInst) {
    assert(
        !variableLength &&
        "variable length arrays must allocate their own arrays");
    allocArrayInst = Builder.createAllocArrayInst(elements, list.size());
  }
  if (!list.empty() && llvh::isa<ESTree::EmptyNode>(&list.back())) {
    // Last element is an elision, VM cannot derive the length properly.
    // We have to explicitly set it.
    Value *newLength;
    if (variableLength)
      newLength = Builder.createLoadStackInst(nextIndex);
    else
      newLength = Builder.getLiteralNumber(count);
    Builder.createStorePropertyInst(
        newLength, allocArrayInst, llvh::StringRef("length"));
  }
  return allocArrayInst;
}

void ESTreeIRGen::genFastArrayPush(Value *array, ESTree::Node &arg) {
  // Pushing a single element, just push the value.
  if (!llvh::isa<ESTree::SpreadElementNode>(&arg)) {
    Builder.createFastArrayPushInst(genExpression(&arg), array);
    return;
  }

  auto *spread = llvh::cast<ESTree::SpreadElementNode>(&arg);

  // Array.push() is typed as 'any', so the FlowChecker does not check its
  // arguments at all and anything can reach us here. Spreading a tuple will
  // work on its own once push() is a real function instead of a special case.
  // TODO: Represent Array.push() as a real function.
  if (!flowContext_.isArrayClassType(
          flowContext_.getNodeTypeOrAny(spread->_argument))) {
    Mod->getContext().getSourceErrorManager().error(
        spread->getSourceRange(), "spread argument must be an array");
    return;
  }

  auto *elems = genExpression(spread->_argument);
  // Append the elements from the spread argument to the target array.
  Builder.createFastArrayAppendInst(elems, array);
}

Value *ESTreeIRGen::genFastArrayFromElements(
    ESTree::NodeList &list,
    flow::Type *arrayType) {
  // Lower the array literal into creating an empty array followed by a series
  // of push and append.

  // Load Array<T>.prototype from the home object variable for the type.
  Value *prototype =
      emitLoadTypedHomeObject(llvh::cast<flow::ClassType>(arrayType->info));

  auto *array = Builder.createAllocFastArrayInst(
      Builder.getLiteralNumber(list.size()), prototype);
  for (auto &node : list)
    genFastArrayPush(array, node);
  return array;
}

Value *ESTreeIRGen::genTupleFromElements(ESTree::NodeList &list) {
  AllocObjectLiteralInst::ObjectPropertyMap propMap{};
  size_t i = 0;
  for (ESTree::Node &node : list) {
    propMap.emplace_back(
        Builder.getLiteralString(llvh::Twine(i)), genExpression(&node));
    ++i;
  }
  return Builder.createAllocObjectLiteralInst(propMap);
}

Value *ESTreeIRGen::genArrayExpr(ESTree::ArrayExpressionNode *Expr) {
  // If the array literal originates in typed code, produce a fast array.
  if (flowContext_.isArrayClassType(flowContext_.getNodeTypeOrAny(Expr)))
    return genFastArrayFromElements(
        Expr->_elements, flowContext_.getNodeTypeOrAny(Expr));
  // If the array literal is a tuple, produce a tuple object.
  if (llvh::isa<flow::TupleType>(flowContext_.getNodeTypeOrAny(Expr)->info))
    return genTupleFromElements(Expr->_elements);
  return genArrayFromElements(Expr->_elements);
}

Value *ESTreeIRGen::genCallExpr(ESTree::CallExpressionNode *call) {
  LLVM_DEBUG(llvh::dbgs() << "IRGen 'call' statement/expression.\n");

  // Handle native calls separately.
  if (auto *natFuncType = llvh::dyn_cast<flow::NativeFunctionType>(
          flowContext_.getNodeTypeOrAny(call->_callee)->info)) {
    return emitNativeCall(call, natFuncType);
  }

  // Check for known TypedLib builtin method call (e.g., Array map).
  if (auto *builtinDecl = flowContext_.getBuiltinCallTarget(call)) {
    return genBuiltinMethodCall(call, builtinDecl);
  }

  // Check for "special calls" -- calls with known special function names.
  if (auto *identNode = llvh::dyn_cast<ESTree::IdentifierNode>(call->_callee)) {
    // Check for a direct call to eval().
    if (identNode->_name == kw_.identEval)
      return genCallEvalExpr(call);
  }

  Value *thisVal;
  Value *callee;
  Value *target = Builder.getEmptySentinel();
  bool calleeIsAlwaysClosure = false;
  // Function type of the resolved callee when it differs from the callee
  // node's type (SH_call dispatch).
  flow::BaseFunctionType *calleeFuncType = nullptr;
  Value *newTarget = Builder.getLiteralUndefined();
  // If this is nonnull, the call is a super() call, and
  // we should call the field initialization function for the
  // given class type immediately after.
  flow::ClassType *fieldInitClassType = nullptr;

  if (auto *consType = llvh::dyn_cast<flow::ClassConstructorType>(
          flowContext_.getNodeTypeOrAny(call->_callee)->info)) {
    // A plain call `X(...)` on a typed class dispatches to the class's static
    // `SH_call` method (the FlowChecker guarantees it exists for this to
    // type-check).
    auto *classInfo = consType->getClassTypeInfo();
    auto *staticInfo = classInfo->getStaticObjectTypeInfo();
    auto optCall = staticInfo->findPublicField(
        Identifier::getFromPointer(kw_.identSHCall));
    const flow::ClassType::Field *field = optCall->getField();
    sema::Decl *decl = semCtx_.getExpressionDecl(field->staticKeyNode);
    callee = emitLoadClassMemberVar(decl);
    target = declFunctions_.lookup(decl);
    calleeIsAlwaysClosure = target != nullptr;
    calleeFuncType = llvh::cast<flow::BaseFunctionType>(field->type->info);
    // `this` for the static method is the class constructor object.
    thisVal = genExpression(call->_callee);
  } else if (
      auto *Mem = llvh::dyn_cast<ESTree::MemberExpressionNode>(call->_callee)) {
    // Handle MemberExpression expression calls that sets the 'this' property.
    // Check for SHBuiltin.
    if (llvh::isa<ESTree::SHBuiltinNode>(Mem->_object)) {
      return genSHBuiltin(
          call, llvh::cast<ESTree::IdentifierNode>(Mem->_property));
    }

    // Typed `fn.call(thisArg, args...)` lowers to a direct CallInst, the
    // same shape that `$SHBuiltin.call` produces. Sema has already verified
    // the receiver is a function type, so the property `call` is guaranteed
    // to be Function.prototype.call.
    if (!Mem->_computed) {
      if (auto *propId = llvh::dyn_cast<ESTree::IdentifierNode>(Mem->_property);
          propId && propId->_name == kw_.identCall) {
        if (llvh::isa<flow::BaseFunctionType>(
                flowContext_.getNodeTypeOrAny(Mem->_object)->info)) {
          return genTypedFunctionPrototypeCall(call, Mem);
        }
      }
    }

    MemberExpressionResult memResult =
        genMemberExpression(Mem, MemberExpressionOperation::Load);

    // Call the callee with obj as the 'this' pointer.
    thisVal = memResult.base;
    callee = memResult.result;

    if (memResult.resultFn) {
      target = memResult.resultFn;
      calleeIsAlwaysClosure = true;
    }
  } else if (
      auto *Mem =
          llvh::dyn_cast<ESTree::OptionalMemberExpressionNode>(call->_callee)) {
    MemberExpressionResult memResult = genOptionalMemberExpression(
        Mem, nullptr, MemberExpressionOperation::Load);

    // Call the callee with obj as the 'this' pointer.
    thisVal = memResult.base;
    callee = memResult.result;
  } else if (llvh::isa<ESTree::SuperNode>(call->_callee)) {
    if (curFunction()->hasLegacyClassContext()) {
      return genLegacyDirectSuper(call);
    }
    if (!curFunction()->superClassNode_) {
      // This is a typed class, where super() is only supported as the first
      // statement of the derived constructor itself; genClassDeclaration
      // rejects a constructor which doesn't start with one. SemanticResolver
      // has already verified that we are lexically inside a derived
      // constructor, so the only way to get here is a super() nested in an
      // arrow function, which has its own FunctionContext and therefore no
      // superclass, 'this' or new.target of the constructor.
      Mod->getContext().getSourceErrorManager().error(
          call->getSourceRange(),
          "super() is only supported as the first statement of a derived class constructor");
      return Builder.getLiteralUndefined();
    }
    if (curFunction()->calledSuperConstructor_) {
      // Found another super() call than the one that actually initializes the
      // base class.
      Mod->getContext().getSourceErrorManager().error(
          call->getSourceRange(), "multiple super() calls in constructor");
    }
    // Register the super() call so we detect any other usages in the function.
    curFunction()->calledSuperConstructor_ = true;
    // Check for a super() call.
    // Call with the passed-in 'this'.
    thisVal = curFunction()->jsParams[0];
    callee = genExpression(curFunction()->superClassNode_);
    newTarget = Builder.createGetNewTargetInst(
        curFunction()->function->getNewTargetParam());
    // Must call the field init function immediately after.
    fieldInitClassType = curFunction()->typedClassContext.type;
  } else {
    thisVal = Builder.getLiteralUndefined();
    callee = genExpression(call->_callee);
  }

  Value *res = emitCall(
      call,
      callee,
      target,
      calleeIsAlwaysClosure,
      thisVal,
      newTarget,
      calleeFuncType);
  if (fieldInitClassType) {
    emitTypedFieldInitCall(fieldInitClassType);
  }
  return res;
}

Value *ESTreeIRGen::genOptionalCallExpr(
    ESTree::OptionalCallExpressionNode *call,
    BasicBlock *shortCircuitBB) {
  PhiInst::ValueListType values;
  PhiInst::BasicBlockListType blocks;

  // true when this is the genOptionalCallExpr call containing
  // the logic for shortCircuitBB.
  bool isFirstOptional = shortCircuitBB == nullptr;

  // If isFirstOptional, the final result will be computed in continueBB and
  // returned.
  BasicBlock *continueBB = nullptr;

  if (!shortCircuitBB) {
    // If shortCircuitBB is null, then this is the outermost in the optional
    // chain, so we must create it here and pass it through to every other
    // OptionalCallExpression and OptionalMemberExpression in the chain.
    continueBB = Builder.createBasicBlock(Builder.getFunction());
    auto *insertionBB = Builder.getInsertionBlock();
    shortCircuitBB = Builder.createBasicBlock(Builder.getFunction());
    Builder.setInsertionBlock(shortCircuitBB);
    values.push_back(Builder.getLiteralUndefined());
    blocks.push_back(shortCircuitBB);
    Builder.createBranchInst(continueBB);
    Builder.setInsertionBlock(insertionBB);
  }

  Value *thisVal;
  Value *callee;

  // Handle MemberExpression expression calls that sets the 'this' property.
  if (auto *me = llvh::dyn_cast<ESTree::MemberExpressionNode>(call->_callee)) {
    MemberExpressionResult memResult =
        genMemberExpression(me, MemberExpressionOperation::Load);

    // Call the callee with obj as the 'this' pointer.
    thisVal = memResult.base;
    callee = memResult.result;
  } else if (
      auto *ome =
          llvh::dyn_cast<ESTree::OptionalMemberExpressionNode>(call->_callee)) {
    MemberExpressionResult memResult = genOptionalMemberExpression(
        ome, shortCircuitBB, MemberExpressionOperation::Load);

    // Call the callee with obj as the 'this' pointer.
    thisVal = memResult.base;
    callee = memResult.result;
  } else if (
      auto *oce =
          llvh::dyn_cast<ESTree::OptionalCallExpressionNode>(call->_callee)) {
    thisVal = Builder.getLiteralUndefined();
    callee = genOptionalCallExpr(oce, shortCircuitBB);
  } else {
    thisVal = Builder.getLiteralUndefined();
    callee = genExpression(getCallee(call));
  }

  if (call->_optional) {
    BasicBlock *evalRHSBB = Builder.createBasicBlock(Builder.getFunction());

    // If callee is undefined or null, then return undefined.
    // NOTE: We use `obj == null` to account for both null and undefined.
    Builder.createCondBranchInst(
        Builder.createBinaryOperatorInst(
            callee, Builder.getLiteralNull(), ValueKind::BinaryEqualInstKind),
        shortCircuitBB,
        evalRHSBB);

    // baseValue is not undefined or null.
    Builder.setInsertionBlock(evalRHSBB);
  }

  Value *callResult = emitCall(
      call,
      callee,
      /* target */ Builder.getEmptySentinel(),
      /* calleeIsAlwaysClosure */ false,
      thisVal,
      Builder.getLiteralUndefined());

  if (isFirstOptional) {
    values.push_back(callResult);
    blocks.push_back(Builder.getInsertionBlock());
    Builder.createBranchInst(continueBB);

    Builder.setInsertionBlock(continueBB);
    return Builder.createPhiInst(values, blocks);
  }

  // If this isn't the first optional, no Phi needed, just return the
  // callResult.
  return callResult;
}

Value *ESTreeIRGen::genSHBuiltin(
    ESTree::CallExpressionNode *call,
    ESTree::IdentifierNode *builtin) {
  if (builtin->_name == kw_.identCall) {
    return genSHBuiltinCall(call);
  }
  if (builtin->_name == kw_.identCNull) {
    return Builder.getLiteralPositiveZero();
  }
  if (builtin->_name == kw_.identCNativeRuntime) {
    return Builder.createGetNativeRuntimeInst();
  }
  if (builtin->_name == kw_.identExternC) {
    return genSHBuiltinExternC(call);
  }
  // %SHBuiltin.?fastArrayPush(arr: Array, ...);
  if (builtin->_name == kw_.identPrivFastArrayPush) {
    assert(call->_arguments.size() >= 1 && "?fastArrayPush without an array");
    auto it = call->_arguments.begin();
    Value *array = genExpression(&*it++);
    for (auto end = call->_arguments.end(); it != end; ++it)
      genFastArrayPush(array, *it);
    return Builder.createFastArrayLengthInst(array);
  }

  // %SHBuiltin.fastArrayPop(arr: Array<T>, n: number): T | void;
  if (builtin->_name == kw_.identFastArrayPop) {
    if (call->_arguments.size() != 2) {
      Mod->getContext().getSourceErrorManager().error(
          call->getSourceRange(),
          "fastArrayPop requires exactly two arguments");
      return Builder.getLiteralUndefined();
    }
    auto it = call->_arguments.begin();
    Value *array = genExpression(&*it++);
    Value *count = genExpression(&*it);
    return genBuiltinCall(
        BuiltinMethod::HermesBuiltin_fastArrayPop, {array, count});
  }

  // %SHBuiltin.fastArrayLength(arr: Array<T>): number;
  if (builtin->_name == kw_.identFastArrayLength) {
    if (call->_arguments.size() != 1) {
      Mod->getContext().getSourceErrorManager().error(
          call->getSourceRange(),
          "fastArrayLength requires exactly one argument");
      return Builder.getLiteralUndefined();
    }
    Value *array = genExpression(&call->_arguments.front());
    return Builder.createFastArrayLengthInst(array);
  }

  // %SHBuiltin.objectKeys(o): Array<string>;
  if (builtin->_name == kw_.identObjectKeys) {
    return genSHBuiltinObjectProps(
        call, BuiltinMethod::HermesBuiltin_objectKeys, "objectKeys");
  }

  // %SHBuiltin.objectValues(o): Array<mixed>;
  if (builtin->_name == kw_.identObjectValues) {
    return genSHBuiltinObjectProps(
        call, BuiltinMethod::HermesBuiltin_objectValues, "objectValues");
  }

  // %SHBuiltin.objectEntries(o): Array<[string, mixed]>;
  if (builtin->_name == kw_.identObjectEntries) {
    return genSHBuiltinObjectProps(
        call, BuiltinMethod::HermesBuiltin_objectEntries, "objectEntries");
  }

  if (builtin->_name == kw_.identModuleFactory) {
    return genSHBuiltinModuleFactory(call);
  }
  if (builtin->_name == kw_.identExport) {
    // We generate no code for $SHBuiltin.export; that's only used as
    // metadata, and processed by semantic resolution.
    return Builder.getLiteralUndefined();
  }
  if (builtin->_name == kw_.identImport) {
    return genSHBuiltinImport(call);
  }

  Mod->getContext().getSourceErrorManager().error(
      call->getSourceRange(), "unknown SH builtin call");
  return Builder.getLiteralUndefined();
}

Value *ESTreeIRGen::genSHBuiltinCall(ESTree::CallExpressionNode *call) {
  if (call->_arguments.size() < 2) {
    Mod->getContext().getSourceErrorManager().error(
        call->getSourceRange(),
        "SH builtin 'call' requires at least 2 arguments");
    return Builder.getLiteralUndefined();
  }

  // $SHBuiltin is recognized in untyped code too, where the FlowChecker never
  // runs, so this must be a diagnostic rather than an assert. The first two
  // arguments are consumed positionally, so neither the callee nor 'this' may
  // be a spread.
  auto it = call->_arguments.begin();
  ESTree::Node *calleeNode = &*it++;
  ESTree::Node *thisNode = &*it;
  if (llvh::isa<ESTree::SpreadElementNode>(calleeNode) ||
      llvh::isa<ESTree::SpreadElementNode>(thisNode) ||
      !canEmitTypedCallArguments(call->_arguments)) {
    Mod->getContext().getSourceErrorManager().error(
        call->getSourceRange(),
        "unsupported spread argument in $SHBuiltin.call");
    return Builder.getLiteralUndefined();
  }

  Value *callee = genExpression(calleeNode);
  Value *thisValue = genExpression(thisNode);
  // Any remaining args are passed through to the function.
  CallInst::ArgumentList args;
  emitTypedCallArguments(call->_arguments, /* offset */ 2, args);

  return Builder.createCallInst(
      callee, /* newTarget */ Builder.getLiteralUndefined(), thisValue, args);
}

Value *ESTreeIRGen::genSHBuiltinObjectProps(
    ESTree::CallExpressionNode *call,
    BuiltinMethod::Enum builtin,
    llvh::StringRef name) {
  if (call->_arguments.size() != 1) {
    Mod->getContext().getSourceErrorManager().error(
        call->getSourceRange(), name + " requires exactly one argument");
    return Builder.getLiteralUndefined();
  }
  Value *obj = genExpression(&call->_arguments.front());
  // The FlowChecker set the result to Array<T>; the builtin needs its
  // prototype to create the FastArray with the correct shape.
  flow::Type *arrayType = flowContext_.getNodeTypeOrAny(call);
  Value *prototype =
      emitLoadTypedHomeObject(llvh::cast<flow::ClassType>(arrayType->info));
  Value *res = genBuiltinCall(builtin, {obj, prototype});
  res->setType(Type::createObject());
  return res;
}

Value *ESTreeIRGen::genTypedFunctionPrototypeCall(
    ESTree::CallExpressionNode *call,
    ESTree::MemberExpressionNode *mem) {
  // The 'this' argument is consumed positionally, so it may not be a spread.
  assert(
      canEmitTypedCallArguments(call->_arguments) &&
      (call->_arguments.empty() ||
       !llvh::isa<ESTree::SpreadElementNode>(&call->_arguments.front())) &&
      "unexpandable spread should have been rejected by the typechecker");

  Value *callee = genExpression(mem->_object);
  Value *thisValue = Builder.getLiteralUndefined();
  uint32_t offset = 0;
  if (!call->_arguments.empty()) {
    thisValue = genExpression(&call->_arguments.front());
    offset = 1;
  }
  CallInst::ArgumentList args;
  emitTypedCallArguments(call->_arguments, offset, args);
  return Builder.createCallInst(
      callee,
      /* newTarget */ Builder.getLiteralUndefined(),
      thisValue,
      args);
}

Value *ESTreeIRGen::genSHBuiltinExternC(ESTree::CallExpressionNode *call) {
  // $SHBuiltin.externC({}, function fopen(path: c_ptr, mode: c_ptr): c_ptr)

  flow::Type *type = flowContext_.findNodeType(call);
  if (!type) {
    Mod->getContext().getSourceErrorManager().error(
        call->getSourceRange(), "extern_c cannot be used in legacy mode");
    return Builder.getLiteralUndefined();
  }
  // Extract the native function type and signature from the call expression
  // but only in debug mode.
#ifndef NDEBUG
  assert(type && "type not found for extern_c");
  NativeSignature *signature =
      llvh::cast<flow::NativeFunctionType>(type->info)->getSignature();
#else
  NativeSignature *signature = nullptr;
  (void)signature;
#endif

  // Extract the name from the function expression.
  UniqueString *name = llvh::cast<ESTree::IdentifierNode>(
                           llvh::cast<ESTree::FunctionExpressionNode>(
                               &*std::next(call->_arguments.begin(), 1))
                               ->_id)
                           ->_name;

  // Lookup the extern declaration.
  auto *nativeExtern =
      Mod->getContext().getNativeContext().getExistingExtern(name);
  assert(
      nativeExtern->signature() == signature &&
      "extern_c signature mismatch with the expression type");

  return Builder.getLiteralNativeExtern(nativeExtern);
}

Value *ESTreeIRGen::genSHBuiltinModuleFactory(
    ESTree::CallExpressionNode *call) {
  ESTree::NodeList &args = ESTree::getArguments(call);
  assert(
      call->_arguments.size() == 2 && "Ensured by checks in SemanticResolver.");

  auto argsIter = args.begin();

  ESTree::Node *modIdArg = &(*argsIter);
  // Success of cast ensured by checks in SemanticResolver.
  const auto *modIdNumLit = llvh::cast<ESTree::NumericLiteralNode>(modIdArg);
  unsigned exportModId = static_cast<unsigned>(modIdNumLit->_value);
  (void)exportModId;
  assert(
      static_cast<double>(exportModId) == modIdNumLit->_value &&
      "Ensured by checks in SemanticResolver.");

  ++argsIter;
  ESTree::Node *modFactoryFuncArg = &(*argsIter);

  // Now evaluate the function argument as the factory function of the
  // current module.
  Value *res = genExpression(modFactoryFuncArg);
  // The cast below should be justified by a check in SemanticResolver;
  // module factory functions are required to be function expressions.
  Function *calledFunc = llvh::cast<CreateFunctionInst>(res)->getFunctionCode();
  // We make all JS module factory functions noInline: we need to be able to
  // analyze them, following the use chains of their arguments to identify
  // require calls (calls to the specified require argument).  If allowed
  // the module factory functions to be inlined, this would not be possible.
  // These functions are executed at most once, so the performance impact
  // of inhibiting this optimization should be negligible.
  calledFunc->setNoInline();
  // Remember the factory functions.  Unless we're doing lazy compilation,
  // in which case the optimization that needs them will never run.
  if (!Mod->getContext().isLazyCompilation()) {
    Mod->jsModuleFactoryFunctions().insert(calledFunc);
  }
  return res;
}

Value *ESTreeIRGen::genSHBuiltinImport(ESTree::CallExpressionNode *call) {
  ESTree::NodeList &args = getArguments(call);
  // SemanticResolver allows two arguments for an import-of-export, but the
  // arguments of $SHBuiltin.export are never generated.
  if (args.size() != 3) {
    Mod->getContext().getSourceErrorManager().error(
        call->getSourceRange(),
        "$SHBuiltin.import with two arguments is only allowed inside "
        "$SHBuiltin.export");
    return Builder.getLiteralUndefined();
  }
  auto argsIter = args.begin();
  ++argsIter;
  ++argsIter;

  ESTree::Node *importExp = &(*argsIter);

  // For now, we just reduce this to the import expression.
  return genExpression(importExp);
}

bool ESTreeIRGen::canEmitTypedCallArguments(ESTree::NodeList &arguments) {
  for (ESTree::Node &arg : arguments) {
    auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(&arg);
    if (spread &&
        !llvh::isa<flow::TupleType>(
            flowContext_.getNodeTypeOrAny(spread->_argument)->info))
      return false;
  }
  return true;
}

void ESTreeIRGen::emitTypedCallArguments(
    ESTree::NodeList &arguments,
    uint32_t offset,
    CallInst::ArgumentList &args) {
  auto it = arguments.begin();
  std::advance(it, offset);
  for (auto e = arguments.end(); it != e; ++it) {
    auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(&*it);
    if (!spread) {
      args.push_back(genExpression(&*it));
      continue;
    }

    // A tuple is an object with the properties "0", "1", ..., so read each
    // element out with a PrLoadInst, exactly as a tuple member access does.
    // Evaluate the tuple itself exactly once, even if it is empty and
    // contributes no arguments.
    auto *tupleType = llvh::cast<flow::TupleType>(
        flowContext_.getNodeTypeOrAny(spread->_argument)->info);
    Value *tuple = genExpression(spread->_argument);
    uint32_t index = 0;
    for (flow::Type *elemType : tupleType->getTypes()) {
      args.push_back(Builder.createPrLoadInst(
          tuple,
          index,
          Builder.getLiteralString(llvh::Twine(index)),
          flowTypeToIRType(elemType)));
      ++index;
    }
  }
}

Value *ESTreeIRGen::emitCall(
    ESTree::CallExpressionLikeNode *call,
    Value *callee,
    Value *target,
    bool calleeIsAlwaysClosure,
    Value *thisVal,
    Value *newTarget,
    flow::BaseFunctionType *calleeFuncType) {
  if (canEmitTypedCallArguments(getArguments(call))) {
    CallInst::ArgumentList args;
    emitTypedCallArguments(getArguments(call), /* offset */ 0, args);

    auto *callInst = Builder.createCallInst(
        callee,
        target,
        calleeIsAlwaysClosure,
        /* env */ Builder.getEmptySentinel(),
        newTarget,
        thisVal,
        args);
    auto *baseFunctionType = calleeFuncType
        ? calleeFuncType
        : llvh::dyn_cast<flow::BaseFunctionType>(
              flowContext_.getNodeTypeOrAny(getCallee(call))->info);
    if (baseFunctionType) {
      // Every BaseFunctionType currently is going to be compiled to a
      // NativeJSFunction, so always set this flag.
      // Eventually we will have bytecode, etc.
      callInst->getAttributesRef(Mod).isNativeJSFunction = true;
      if (auto *typedFunc =
              llvh::dyn_cast<flow::TypedFunctionType>(baseFunctionType)) {
        callInst->setType(flowTypeToIRType(typedFunc->getReturnType()));
      }
    }
    return callInst;
  }

  // Otherwise, there exists a spread argument that cannot be expanded
  // statically, so the number of arguments is variable.
  // Generate IR for this by creating an array and populating it with the
  // arguments.
  auto *args = genArrayFromElements(getArguments(call));
  // If there is a new.target that should be set, use the correct builtin
  if (!llvh::isa<LiteralUndefined>(newTarget)) {
    return genBuiltinCall(
        BuiltinMethod::HermesBuiltin_applyWithNewTarget,
        {callee, args, thisVal, newTarget});
  }
  return genBuiltinCall(
      BuiltinMethod::HermesBuiltin_apply, {callee, args, thisVal});
}

Value *ESTreeIRGen::emitNativeCall(
    ESTree::CallExpressionNode *call,
    flow::NativeFunctionType *natFuncType) {
  Value *callee = genExpression(call->_callee);

  CallInst::ArgumentList args;
  emitTypedCallArguments(getArguments(call), /* offset */ 0, args);

  return Builder.createNativeCallInst(
      flowTypeToIRType(natFuncType->getReturnType()),
      callee,
      natFuncType->getSignature(),
      args);
}

Value *ESTreeIRGen::genBuiltinMethodCall(
    ESTree::CallExpressionNode *call,
    ESTree::FunctionDeclarationNode *builtinDecl) {
  auto *builtinId = llvh::cast<ESTree::IdentifierNode>(builtinDecl->_id);
  sema::Decl *decl = semCtx_.getDeclarationDecl(builtinId);

  // Generic builtins don't have Variables created for them, so we can't
  // generate a direct call.
  // This should never happen but just report an error and fail gracefully.
  if (decl->generic) {
    Mod->getContext().getSourceErrorManager().error(
        call->getSourceRange(),
        "invalid AST: cannot call generic method as builtin");
    return Builder.getLiteralUndefined();
  }

  // All builtin calls must have a member expression.
  auto *memExpr = llvh::cast<ESTree::MemberExpressionNode>(call->_callee);

  // Load the closure from the variable.
  Variable *var = llvh::cast<Variable>(getDeclData(decl));
  Value *callee = emitLoadClassMemberVar(decl);

  // Generate the arguments.
  Value *receiver = genExpression(memExpr->_object);
  CallInst::ArgumentList args;
  emitTypedCallArguments(call->_arguments, /* offset */ 0, args);

  // Find the function based on the closure creation.
  Function *func = nullptr;
  bool found = false;
  for (auto *varUser : var->getUsers()) {
    if (auto *closure = llvh::dyn_cast<CreateFunctionInst>(varUser)) {
      if (found) {
        found = false;
        func = nullptr;
        break;
      }
      found = true;
      func = closure->getFunctionCode();
    }
  }

  return Builder.createCallInst(
      callee,
      /* target */ found ? (Value *)func : Builder.getEmptySentinel(),
      /* calleeIsAlwaysClosure */ true,
      /* env */ Builder.getEmptySentinel(),
      /* newTarget */ Builder.getLiteralUndefined(),
      /* thisValue */ receiver,
      args);
}

ESTreeIRGen::MemberExpressionResult ESTreeIRGen::genMemberExpression(
    ESTree::MemberExpressionNode *mem,
    MemberExpressionOperation op) {
  if (auto *superNode = llvh::dyn_cast<ESTree::SuperNode>(mem->_object)) {
    if (op == MemberExpressionOperation::Delete) {
      Builder.createCallBuiltinInst(
          BuiltinMethod::HermesBuiltin_throwReferenceError,
          {Builder.getLiteralString("Cannot delete a super property.")});
      return MemberExpressionResult{
          Builder.getLiteralUndefined(),
          nullptr,
          Builder.getLiteralUndefined()};
    }
    // A ClassConstructorType here means a static 'super', which resolves
    // against the superclass's statics rather than its prototype.
    if (auto *superTypeInfo = flowContext_.getNodeTypeOrAny(superNode)->info;
        llvh::isa<flow::ClassType>(superTypeInfo) ||
        llvh::isa<flow::ClassConstructorType>(superTypeInfo)) {
      auto *property = llvh::dyn_cast<ESTree::IdentifierNode>(mem->_property);
      if (!property || mem->_computed) {
        // We can only handle super.foo, where foo is an identifier.
        Mod->getContext().getSourceErrorManager().error(
            mem->getSourceRange(), "unsupported use of 'super'");
        return MemberExpressionResult{
            Builder.getLiteralUndefined(),
            nullptr,
            Builder.getLiteralUndefined()};
      }
      return emitTypedSuperLoad(mem);
    }
    // Generate `this` value first. This handles the case where we are in a
    // derived constructor but no call to `super` has been made. We must throw
    // an error before we do the property load.
    Value *thisValue = genThisExpression();
    auto *homeObjectVar = curFunction()->capturedState.homeObject;
    assert(homeObjectVar && "homeObjectVar not populated");
    auto *RSI = emitResolveScopeInstIfNeeded(homeObjectVar->getParent());
    Value *homeObjectVal = Builder.createLoadFrameInst(RSI, homeObjectVar);
    // We know that home objects are always ordinary objects.
    Value *superObj = Builder.createLoadParentNoTrapsInst(homeObjectVal);
    Value *propVal = Builder.createLoadPropertyWithReceiverInst(
        superObj, genMemberExpressionProperty(mem), thisValue);
    return MemberExpressionResult{propVal, nullptr, thisValue};
  }

  Value *baseValue = genExpression(mem->_object);
  Value *prop;
  auto *objTypeInfo = flowContext_.getNodeTypeOrAny(mem->_object)->info;
  if ((llvh::isa<flow::ClassType>(objTypeInfo) ||
       llvh::isa<flow::ClassConstructorType>(objTypeInfo)) &&
      !mem->_computed) {
    // Named types for typed fields with known layout shouldn't bother
    // with genMemberExpressionProperty, which is intended for untyped code.
    if (auto *privateName =
            llvh::dyn_cast<ESTree::PrivateNameNode>(mem->_property)) {
      // If the property is an identifier, we can use the class type to
      // determine the property name.
      prop =
          Builder.getLiteralPrivateName(getNameFieldFromID(privateName->_id));
    } else {
      prop = Builder.getLiteralString(getNameFieldFromID(mem->_property));
    }
  } else {
    prop = genMemberExpressionProperty(mem);
  }

  switch (op) {
    case MemberExpressionOperation::Load:
      return emitMemberLoad(mem, baseValue, prop);
    case MemberExpressionOperation::Delete:
      return MemberExpressionResult{
          Builder.createDeletePropertyInst(baseValue, prop),
          nullptr,
          baseValue};
  }
  llvm_unreachable("No other kind of member expression");
}

sema::Decl *ESTreeIRGen::getTypedMemberExpressionDeclIfExists(
    ESTree::MemberExpressionNode *mem) {
  if (mem->_computed)
    return nullptr;
  // Only applies to typed classes (ClassType for instance access,
  // ClassConstructorType for static access). Legacy classes use different
  // mechanisms for private names.
  auto *objTypeInfo = flowContext_.getNodeTypeOrAny(mem->_object)->info;
  if (!llvh::isa<flow::ClassType>(objTypeInfo) &&
      !llvh::isa<flow::ClassConstructorType>(objTypeInfo))
    return nullptr;

  sema::Decl *decl = nullptr;
  if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(mem->_property)) {
    decl = semCtx_.getExpressionDecl(id);
  } else if (
      auto *pn = llvh::dyn_cast<ESTree::PrivateNameNode>(mem->_property)) {
    decl =
        semCtx_.getExpressionDecl(llvh::cast<ESTree::IdentifierNode>(pn->_id));
  }

  // Only return the Decl if it has customData set (indicating that the field is
  // stored in a Variable instead of on the object).
  return decl && decl->customData ? decl : nullptr;
}

OptValue<flow::ClassType::FieldLookupEntry> ESTreeIRGen::findFieldForMemberProp(
    ESTree::MemberExpressionNode *mem,
    flow::ClassType *classType) {
  assert(!mem->_computed && "expected non-computed member expression");
  if (auto *pn = llvh::dyn_cast<ESTree::PrivateNameNode>(mem->_property)) {
    Identifier propIdent = Mod->getContext().getPrivateNameIdentifier(
        getNameFieldFromID(pn->_id).getUnderlyingPointer());
    return classType->findPrivateField(propIdent);
  }
  Identifier propIdent = getNameFieldFromID(mem->_property);
  return classType->findPublicField(propIdent);
}

Value *ESTreeIRGen::emitTypedGetterCall(
    sema::Decl *decl,
    Value *thisArg,
    flow::Type *resultType) {
  Value *closure = emitLoadClassMemberVar(decl);
  Value *target = closure;
  if (auto *func = declFunctions_.lookup(decl))
    target = func;
  auto *callInst = Builder.createCallInst(
      closure,
      target,
      /* calleeIsAlwaysClosure */ true,
      Builder.getEmptySentinel(),
      Builder.getLiteralUndefined(),
      thisArg,
      {});
  callInst->setType(flowTypeToIRType(resultType));
  callInst->getAttributesRef(Mod).isNativeJSFunction = true;
  return callInst;
}

void ESTreeIRGen::emitTypedSetterCall(
    sema::Decl *decl,
    Value *thisArg,
    Value *storedValue) {
  Value *closure = emitLoadClassMemberVar(decl);
  Value *target = closure;
  if (auto *func = declFunctions_.lookup(decl))
    target = func;
  auto *callInst = Builder.createCallInst(
      closure,
      target,
      /* calleeIsAlwaysClosure */ true,
      Builder.getEmptySentinel(),
      Builder.getLiteralUndefined(),
      thisArg,
      {storedValue});
  callInst->setType(Type::createUndefined());
  callInst->getAttributesRef(Mod).isNativeJSFunction = true;
}

void ESTreeIRGen::emitTypedClassMethodClosureStore(
    ESTree::MethodDefinitionNode *method,
    sema::Decl *decl) {
  auto *funcExpr = llvh::cast<ESTree::FunctionExpressionNode>(method->_value);
  ESTree::IdentifierNode *id = ESTree::getPropertyIdentifier(method->_key);
  Identifier name = llvh::isa<ESTree::PrivateNameNode>(method->_key)
      ? Mod->getContext().getPrivateNameIdentifier(id->_name)
      : Identifier::getFromPointer(id->_name);
  Value *function = genFunctionExpression(funcExpr, name);
  storeClassMemberVar(decl, function);
  if (auto *CFI = llvh::dyn_cast<CreateFunctionInst>(function)) {
    // Don't populate nonOverriddenMethods_ here,
    // because it's only read for methods placed in the vtable.
    declFunctions_.try_emplace(decl, CFI->getFunctionCode());
  }
}

ESTreeIRGen::MemberExpressionResult ESTreeIRGen::emitMemberLoad(
    ESTree::MemberExpressionNode *mem,
    Value *baseValue,
    Value *propValue) {
  // Final methods and static fields/methods are lowered to scope variables.
  // Getters never have their Decl propagated to the call-site, so they
  // won't be caught here — they are handled below.
  if (sema::Decl *decl = getTypedMemberExpressionDeclIfExists(mem)) {
    Value *val = emitLoadClassMemberVar(decl);
    return MemberExpressionResult{val, declFunctions_.lookup(decl), baseValue};
  }

  // Static getter: the Decl is not propagated to the call-site, so
  // look up the getter on the static type and emit the call.
  if (auto *consType = llvh::dyn_cast<flow::ClassConstructorType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info)) {
    if (!mem->_computed) {
      if (sema::Decl *decl = findStaticAccessor(mem, consType).getterDecl) {
        Value *call = emitTypedGetterCall(
            decl, baseValue, flowContext_.getNodeTypeOrAny(mem));
        return MemberExpressionResult{call, nullptr, baseValue};
      }
      // `A.prototype` reads the home object, which is stored as a named
      // property on the constructor closure by genClassDeclaration.
      // Type it as the home object so that PrLoad on the result is legal.
      if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(mem->_property);
          id && id->_name == kw_.identPrototype) {
        auto *load = Builder.createLoadPropertyInst(baseValue, propValue);
        load->setType(flowTypeToIRType(flowContext_.getNodeTypeOrAny(mem)));
        return MemberExpressionResult{load, nullptr, baseValue};
      }
    }
  }

  // Check if we are loading from a FastArray, and generate the typed IR.
  // This must be checked before the general ClassType case because Array<T>
  // is a ClassType but needs special handling for computed access.
  // NOTE: This is required for correctness, since a regular property load from
  // a FastArray will simply return undefined if it is out-of-bounds.
  if (flowContext_.isArrayClassType(
          flowContext_.getNodeTypeOrAny(mem->_object))) {
    flow::TypeInfo *propInfo =
        flowContext_.getNodeTypeOrAny(mem->_property)->info;
    // A number or number-literal index uses the typed fast-array path.
    if (mem->_computed &&
        (llvh::isa<flow::NumberType>(propInfo) ||
         llvh::isa<flow::NumberLiteralType>(propInfo))) {
      return MemberExpressionResult{
          Builder.createFastArrayLoadInst(
              baseValue,
              propValue,
              flowTypeToIRType(flowContext_.getNodeTypeOrAny(mem))),
          nullptr,
          baseValue};
    }
  }

  if (auto *classType = llvh::dyn_cast<flow::ClassType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info)) {
    if (!mem->_computed) {
      Identifier propIdent;
      Literal *propName;
      OptValue<flow::ClassType::FieldLookupEntry> optFieldLookup;
      bool isPrivate;
      if (auto *privateName =
              llvh::dyn_cast<ESTree::PrivateNameNode>(mem->_property)) {
        // If the property is an identifier, we can use the class type to
        // determine the property name.
        isPrivate = true;
        propIdent = Mod->getContext().getPrivateNameIdentifier(
            getNameFieldFromID(privateName->_id).getUnderlyingPointer());
        optFieldLookup = classType->findPrivateField(propIdent);
        propName = Builder.getLiteralPrivateName(propIdent);
      } else {
        isPrivate = false;
        propIdent = getNameFieldFromID(mem->_property);
        optFieldLookup = classType->findPublicField(propIdent);
        propName = Builder.getLiteralString(propIdent);
      }
      if (optFieldLookup && optFieldLookup->getField()->isStoredOnObject()) {
        return MemberExpressionResult{
            emitTypedFieldLoad(
                baseValue, *optFieldLookup->getField(), propName),
            nullptr,
            baseValue};
      }
      // Failed to find a class field, check the home object for methods.
      if (isPrivate) {
        // Private methods are stored in variables instead of on the object.
        OptValue<flow::ClassType::FieldLookupEntry> optMethodLookup =
            classType->getHomeObjectTypeInfo()->findPrivateField(propIdent);
        auto *privateName = llvh::cast<ESTree::PrivateNameNode>(mem->_property);
        // Getter: use the definition key's Decl (the usage site's Decl
        // is the SemanticResolver's PrivateGetterSetter Decl, which
        // doesn't have the Variable stored on it).
        if (optMethodLookup->getField()->hasGetter()) {
          auto *methodKey = ESTree::getPropertyIdentifier(
              optMethodLookup->getField()->method->_key);
          sema::Decl *getterDecl = semCtx_.getExpressionDecl(methodKey);
          Value *call = emitTypedGetterCall(
              getterDecl, baseValue, flowContext_.getNodeTypeOrAny(mem));
          return MemberExpressionResult{call, nullptr, baseValue};
        }
        sema::Decl *decl =
            getIDDecl(llvh::cast<ESTree::IdentifierNode>(privateName->_id));
        Value *closure = emitLoadClassMemberVar(decl);
        return MemberExpressionResult{
            closure,
            nonOverriddenMethods_.lookup(optMethodLookup->getField()),
            baseValue};
      }

      OptValue<flow::ClassType::FieldLookupEntry> optMethodLookup =
          classType->getHomeObjectTypeInfo()->findPublicField(propIdent);
      assert(
          optMethodLookup && "must have typechecked as either method or field");
      const auto *field = optMethodLookup->getField();

      // Getter: load closure from Variable, call it, return result.
      if (field->hasGetter()) {
        auto *methodKey = ESTree::getPropertyIdentifier(field->method->_key);
        sema::Decl *decl = semCtx_.getExpressionDecl(methodKey);
        Value *call = emitTypedGetterCall(
            decl, baseValue, flowContext_.getNodeTypeOrAny(mem));
        return MemberExpressionResult{call, nullptr, baseValue};
      }

      // Final methods are handled by the early return above, which
      // loads the closure from the Decl set on the call-site property.
      assert(
          !field->finalMethod &&
          "final methods should be handled by the Decl early return");
      assert(field->isStoredOnObject() && "method must be on the home object");
      size_t methodIndex = *field->layoutSlotIR;
      // Lookup method on the parent, return baseValue in the result to
      // correctly populate 'this' argument.
      return MemberExpressionResult{
          Builder.createPrLoadInst(
              Builder.createTypedLoadParentInst(baseValue),
              methodIndex,
              propName,
              flowTypeToIRType(field->type)),
          nonOverriddenMethods_.lookup(field),
          baseValue};
    }
  }

  if (auto *objType = llvh::dyn_cast<flow::ExactObjectType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info)) {
    if (!mem->_computed) {
      auto propName = Identifier::getFromPointer(
          llvh::cast<ESTree::IdentifierNode>(mem->_property)->_name);
      auto optIndex = objType->findField(propName);
      if (optIndex.hasValue()) {
        const auto &field = objType->getFields()[*optIndex];
        return MemberExpressionResult{
            Builder.createPrLoadInst(
                baseValue,
                *optIndex,
                Builder.getLiteralString(propName),
                flowTypeToIRType(field.type)),
            nullptr,
            baseValue};
      }
    }
    // Either a computed access or a dot access with no named field: this is an
    // indexer read. The dynamic load yields `T | void` (a missing key reads as
    // `undefined`); narrow it to `T` with a checked cast, so a read of a
    // missing key throws. The FlowChecker types the member expression as `T`.
    if (const auto &indexer = objType->getIndexer()) {
      Type valueIRType = flowTypeToIRType(indexer->valueType);
      auto *loadProp = Builder.createLoadPropertyInst(baseValue, propValue);
      loadProp->setType(
          getTypeContext().unionTy(valueIRType, Type::createUndefined()));
      return MemberExpressionResult{
          Builder.createCheckedTypeCastInst(loadProp, valueIRType),
          nullptr,
          baseValue};
    }
  }

  // Reading a field common to every arm of an object-literal union.
  // FlowChecker has verified the field exists in every exact-object arm and
  // typed the member expression as the union of the per-arm field types.
  // If the field is the same slot in every arm, use PrLoad, else use
  // LoadProperty.
  if (auto *unionType = llvh::dyn_cast<flow::UnionType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info)) {
    if (!mem->_computed) {
      auto propName = Identifier::getFromPointer(
          llvh::cast<ESTree::IdentifierNode>(mem->_property)->_name);
      Type resultTy = flowTypeToIRType(flowContext_.getNodeTypeOrAny(mem));
      // Determine whether the field lives at the same slot in every arm.
      OptValue<size_t> commonSlot = llvh::None;
      bool sameSlot = true;
      for (flow::Type *arm : unionType->getTypes()) {
        auto *objType = llvh::cast<flow::ExactObjectType>(arm->info);
        auto optIndex = objType->findField(propName);
        assert(optIndex && "FlowChecker guarantees the field in every arm");
        if (!commonSlot) {
          commonSlot = *optIndex;
        } else if (*commonSlot != *optIndex) {
          sameSlot = false;
          break;
        }
      }
      if (sameSlot) {
        return MemberExpressionResult{
            Builder.createPrLoadInst(
                baseValue,
                *commonSlot,
                Builder.getLiteralString(propName),
                resultTy),
            nullptr,
            baseValue};
      }
      auto *loadProp = Builder.createLoadPropertyInst(baseValue, propValue);
      loadProp->setType(resultTy);
      return MemberExpressionResult{loadProp, nullptr, baseValue};
    }
  }

  // Check if we are loading a tuple element, and generate the typed IR.
  if (auto *tupleType = llvh::dyn_cast<flow::TupleType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info)) {
    if (auto *idx = llvh::dyn_cast<ESTree::NumericLiteralNode>(mem->_property);
        idx && mem->_computed) {
      double d = idx->_value;
      // Safe cast because this has been typechecked.
      uint32_t ulen = ubcastFromDouble<uint32_t>(d);
      return MemberExpressionResult{
          Builder.createPrLoadInst(
              baseValue,
              ulen,
              Builder.getLiteralString(llvh::Twine(ulen)),
              flowTypeToIRType(tupleType->getTypes()[ulen])),
          nullptr,
          baseValue};
    }
    Mod->getContext().getSourceErrorManager().error(
        mem->getSourceRange(), "invalid tuple access");
  }

  // Check if we are loading a string property.
  if (llvh::isa<flow::StringType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info) ||
      llvh::isa<flow::StringLiteralType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info)) {
    if (!mem->_computed) {
      auto *ident = llvh::cast<ESTree::IdentifierNode>(mem->_property);
      if (ident->_name == kw_.identLength) {
        // str.length returns number.
        auto *loadProp = Builder.createLoadPropertyInst(baseValue, propValue);
        loadProp->setType(Type::createNumber());
        return MemberExpressionResult{loadProp, nullptr, baseValue};
      }
    } else {
      auto *loadProp = Builder.createLoadPropertyInst(baseValue, propValue);
      loadProp->setType(
          getTypeContext().unionTy(
              Type::createString(), Type::createUndefined()));
      auto *cast =
          Builder.createCheckedTypeCastInst(loadProp, Type::createString());
      return MemberExpressionResult{cast, nullptr, baseValue};
    }
  }

  Value *loadProp;
  if (auto *PN = llvh::dyn_cast<ESTree::PrivateNameNode>(getProperty(mem))) {
    loadProp = emitPrivateLookup(baseValue, propValue, PN);
  } else {
    loadProp = Builder.createLoadPropertyInst(baseValue, propValue);
  }
  return MemberExpressionResult{loadProp, nullptr, baseValue};
}

ESTreeIRGen::StaticAccessor ESTreeIRGen::findStaticAccessor(
    ESTree::MemberExpressionNode *mem,
    flow::ClassConstructorType *consType) {
  if (mem->_computed)
    return {};
  auto *classInfo = llvh::cast<flow::ClassType>(consType->getClassType()->info);
  auto *staticInfo = classInfo->getStaticObjectTypeInfo();
  if (!staticInfo)
    return {};
  auto optField = findFieldForMemberProp(mem, staticInfo);
  if (!optField || !optField->getField()->isAccessor())
    return {};
  const flow::ClassType::Field *field = optField->getField();
  // staticKeyNode is the key of whichever half was declared first, so each
  // Decl has to come from its own MethodDefinitionNode.
  // Callers pass these Decls straight to emitTypedGetterCall/SetterCall, which
  // dereference them, so catch a missing Decl here rather than there.
  auto declFor = [this](ESTree::MethodDefinitionNode *method) {
    sema::Decl *decl =
        semCtx_.getExpressionDecl(ESTree::getPropertyIdentifier(method->_key));
    assert(decl && "static accessor must have a Decl on its key");
    return decl;
  };
  return {
      field->hasGetter() ? declFor(field->method) : nullptr,
      field->hasSetter() ? declFor(field->setterMethod) : nullptr};
}

Value *ESTreeIRGen::emitLoadClassMemberVar(sema::Decl *decl) {
  Value *val = emitLoad(getDeclData(decl), false);
  // A class member Variable holds empty until the class declaration stores
  // its closure, or its initializer runs, so check loads that may come first.
  // Method closures are always empty until then; fields only when their type
  // has no representable literal default. Like a TDZ variable, empty is used
  // rather than uninit so that TDZDedup understands the initial store.
  if (getTypeContext().canBeEmpty(val->getType()))
    val = Builder.createThrowIfInst(val, Type::createEmpty());
  return val;
}

Value *ESTreeIRGen::emitLoadTypedHomeObject(flow::ClassType *classInfo) {
  auto it = classConstructors_.find(classInfo);
  assert(it != classConstructors_.end() && "class not compiled");
  return Builder.createLoadFrameInst(
      emitResolveScopeInstIfNeeded(it->second.homeObjectVar->getParent()),
      it->second.homeObjectVar);
}

ESTreeIRGen::MemberExpressionResult ESTreeIRGen::emitTypedSuperLoad(
    ESTree::MemberExpressionNode *mem) {
  auto *superNode = llvh::cast<ESTree::SuperNode>(mem->_object);
  auto *property = llvh::cast<ESTree::IdentifierNode>(mem->_property);
  auto propName = Identifier::getFromPointer(property->_name);
  Value *thisValue = genThisExpression();

  // Final methods (both generic specializations and non-generic) have a
  // Decl set on the call-site property by the FlowChecker. Load the
  // closure from the Variable stored in the Decl's data.
  if (auto *decl = semCtx_.getExpressionDecl(property)) {
    Value *closure = emitLoadClassMemberVar(decl);
    return MemberExpressionResult{
        closure, declFunctions_.lookup(decl), thisValue};
  }

  // A static 'super' has no instance to read through: its members are the
  // superclass's statics, which live in Variables. Only accessors and
  // 'prototype' reach here, because every other static carries a Decl.
  if (auto *consType = llvh::dyn_cast<flow::ClassConstructorType>(
          flowContext_.getNodeTypeOrAny(superNode)->info)) {
    if (propName.getUnderlyingPointer() == kw_.identPrototype) {
      return MemberExpressionResult{
          emitLoadTypedHomeObject(
              llvh::cast<flow::ClassType>(consType->getClassType()->info)),
          nullptr,
          thisValue};
    }
    sema::Decl *decl = findStaticAccessor(mem, consType).getterDecl;
    assert(decl && "static super without a Decl must be a getter");
    Value *call = emitTypedGetterCall(
        decl, thisValue, flowContext_.getNodeTypeOrAny(mem));
    return MemberExpressionResult{call, nullptr, thisValue};
  }

  // Everything below reads through the superclass instance.
  auto *classType = llvh::cast<flow::ClassType>(
      flowContext_.getNodeTypeOrAny(superNode)->info);
  if (auto optFieldLookup = classType->findPublicField(propName);
      optFieldLookup && optFieldLookup->getField()->isStoredOnObject()) {
    // Found the field on the class, so load it directly from 'this'.
    return MemberExpressionResult{
        emitTypedFieldLoad(
            thisValue,
            *optFieldLookup->getField(),
            Builder.getLiteralString(propName)),
        nullptr,
        thisValue};
  }
  // Failed to find a class field, check the home object for methods.
  auto optMethodLookup =
      classType->getHomeObjectTypeInfo()->findPublicField(propName);
  assert(optMethodLookup && "must have typechecked as either method or field");
  const auto *field = optMethodLookup->getField();

  // Getter: load closure from Variable, call it, return result.
  if (field->hasGetter()) {
    auto *methodKey = ESTree::getPropertyIdentifier(field->method->_key);
    sema::Decl *decl = semCtx_.getExpressionDecl(methodKey);
    Value *call = emitTypedGetterCall(decl, thisValue, field->type);
    return MemberExpressionResult{call, nullptr, thisValue};
  }

  // Final methods are handled by the Decl early return above.
  assert(
      !field->finalMethod &&
      "final methods should be handled by the Decl early return");
  assert(field->isStoredOnObject() && "method must be on the home object");
  size_t methodIndex = *field->layoutSlotIR;
  // Lookup method on the parent, return thisValue in the result to
  // correctly populate 'this' argument.
  Value *superHomeObject = emitLoadTypedHomeObject(classType);
  return MemberExpressionResult{
      Builder.createPrLoadInst(
          superHomeObject,
          methodIndex,
          Builder.getLiteralString(propName),
          flowTypeToIRType(field->type)),
      nullptr,
      thisValue};
}

Value *ESTreeIRGen::emitTypedFieldLoad(
    Value *object,
    const flow::ClassType::Field &field,
    Literal *propName) {
  assert(field.isStoredOnObject() && "field must be stored on the object");
  TypeContext &tc = getTypeContext();
  Type irType = flowTypeToIRType(field.type);
  if (!tc.isIDZType(irType)) {
    // Has a literal default that doesn't need IDZ.
    return Builder.createPrLoadInst(
        object, *field.layoutSlotIR, propName, irType);
  }
  // No representable literal default (object types, symbol): the field is
  // stored uninitialized, so IDZ-check it on load.
  return Builder.createThrowIfInst(
      Builder.createPrLoadInst(
          object,
          *field.layoutSlotIR,
          propName,
          tc.unionTy(irType, Type::createUninit())),
      Type::createUninit());
}

void ESTreeIRGen::emitTypedFieldStore(
    flow::ClassType *classType,
    Identifier name,
    bool isPrivate,
    Value *object,
    Value *value) {
  Literal *propName;
  OptValue<flow::ClassType::FieldLookupEntry> optFieldLookup;
  if (isPrivate) {
    Identifier privateName =
        Mod->getContext().getPrivateNameIdentifier(name.getUnderlyingPointer());
    optFieldLookup = classType->findPrivateField(privateName);
    propName = Builder.getLiteralPrivateName(privateName);
  } else {
    optFieldLookup = classType->findPublicField(name);
    propName = Builder.getLiteralString(name);
  }
  assert(optFieldLookup && "field lookup must succeed after typechecking");
  const auto *field = optFieldLookup->getField();
  assert(field->isStoredOnObject() && "field must be stored on the object");
  size_t fieldIndex = *field->layoutSlotIR;
  Builder.createPrStoreInst(
      value,
      object,
      fieldIndex,
      propName,
      getTypeContext().isNonPtr(flowTypeToIRType(field->type)));
}

void ESTreeIRGen::emitMemberStore(
    ESTree::MemberExpressionNode *mem,
    Value *storedValue,
    Value *baseValue,
    Value *propValue,
    Value *thisValue) {
  // Destructuring and for-in/of stores have no ImplicitCheckedCast,
  // so the stored value is typed wider than it should be.
  storedValue = enforceExprType(storedValue, mem);

  // Static fields/methods are stored in scope variables, not object properties.
  if (auto *decl = getTypedMemberExpressionDeclIfExists(mem)) {
    emitStore(storedValue, getDeclData(decl), false);
    return;
  }

  // Static setter case: the Decl is not propagated to the callsite, so
  // look up the setter on the static type and emit the call. The receiver is
  // \p baseValue in both forms: createLRef uses 'this' as the object for a
  // typed 'super', so it already equals \p thisValue there.
  if (auto *consType = llvh::dyn_cast<flow::ClassConstructorType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info)) {
    if (sema::Decl *decl = findStaticAccessor(mem, consType).setterDecl) {
      emitTypedSetterCall(decl, baseValue, storedValue);
      return;
    }
  }

  // A named store to a typed class instance, through 'super' or not. In ES a
  // 'super' store goes to the receiver, and the receiver's layout contains the
  // superclass's slots, so both cases store through \p baseValue: createLRef
  // uses 'this' as the object for a typed 'super', so it already equals
  // \p thisValue there.
  if (auto *classType = llvh::dyn_cast<flow::ClassType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info);
      classType && !mem->_computed) {
    // Accessor setters are stored as methods in the home object.
    auto optMethod =
        findFieldForMemberProp(mem, classType->getHomeObjectTypeInfo());
    if (optMethod && optMethod->getField()->hasSetter()) {
      const auto *field = optMethod->getField();
      auto *setterKeyId =
          ESTree::getPropertyIdentifier(field->setterMethod->_key);
      sema::Decl *decl = semCtx_.getExpressionDecl(setterKeyId);
      emitTypedSetterCall(decl, baseValue, storedValue);
      return;
    }
    emitTypedFieldStore(
        classType,
        getNameFieldFromID(ESTree::getPropertyIdentifier(mem->_property)),
        llvh::isa<ESTree::PrivateNameNode>(mem->_property),
        baseValue,
        storedValue);
    return;
  }

  // If \p thisValue is set, this is a store to an untyped `super`.
  if (thisValue) {
    Builder.createStorePropertyWithReceiverInst(
        storedValue,
        baseValue,
        propValue,
        thisValue,
        curFunction()->function->isStrictMode() ? IRBuilder::StoreStrict::Yes
                                                : IRBuilder::StoreStrict::No);
    return;
  }

  // Check if we are storing to a FastArray, and generate the specialised
  // instruction for it. Only computed accesses reach here, because the typed
  // ClassType case above already handled named ones, and Array<T> is a
  // ClassType.
  if (flowContext_.isArrayClassType(
          flowContext_.getNodeTypeOrAny(mem->_object))) {
    flow::TypeInfo *propInfo =
        flowContext_.getNodeTypeOrAny(mem->_property)->info;
    // A number or number-literal index uses the typed fast-array path.
    if (mem->_computed &&
        (llvh::isa<flow::NumberType>(propInfo) ||
         llvh::isa<flow::NumberLiteralType>(propInfo))) {
      Builder.createFastArrayStoreInst(storedValue, baseValue, propValue);
      return;
    }
  }

  if (auto *objType = llvh::dyn_cast<flow::ExactObjectType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info)) {
    if (!mem->_computed) {
      auto propName = Identifier::getFromPointer(
          llvh::cast<ESTree::IdentifierNode>(mem->_property)->_name);
      auto optIndex = objType->findField(propName);
      if (optIndex.hasValue()) {
        const auto &field = objType->getFields()[*optIndex];
        Builder.createPrStoreInst(
            storedValue,
            baseValue,
            *optIndex,
            Builder.getLiteralString(propName),
            getTypeContext().isNonPtr(flowTypeToIRType(field.type)));
        return;
      }
      // No named field: this is an indexer object. Fall through to dynamic
      // by-name access, the same lowering used for computed indexer access.
    }
  }

  // Check if we are loading a tuple element, and generate the typed IR.
  if (auto *tupleType = llvh::dyn_cast<flow::TupleType>(
          flowContext_.getNodeTypeOrAny(mem->_object)->info)) {
    if (auto *idx = llvh::dyn_cast<ESTree::NumericLiteralNode>(mem->_property);
        idx && mem->_computed) {
      double d = idx->_value;
      // Safe cast because this has been typechecked.
      uint32_t ulen = ubcastFromDouble<uint32_t>(d);
      Builder.createPrStoreInst(
          storedValue,
          baseValue,
          ulen,
          Builder.getLiteralString(llvh::Twine(ulen)),
          getTypeContext().isNonPtr(
              flowTypeToIRType(tupleType->getTypes()[ulen])));
      return;
    }
    Mod->getContext().getSourceErrorManager().error(
        mem->getSourceRange(), "invalid tuple access");
  }

  if (auto *PN = llvh::dyn_cast<ESTree::PrivateNameNode>(getProperty(mem))) {
    emitPrivateStore(baseValue, storedValue, propValue, PN);
  } else {
    Builder.createStorePropertyInst(storedValue, baseValue, propValue);
  }
}

ESTreeIRGen::MemberExpressionResult ESTreeIRGen::genOptionalMemberExpression(
    ESTree::OptionalMemberExpressionNode *mem,
    BasicBlock *shortCircuitBB,
    MemberExpressionOperation op) {
  PhiInst::ValueListType values;
  PhiInst::ValueListType baseValues;
  PhiInst::BasicBlockListType blocks;

  // true when this is the genOptionalMemberExpression call containing
  // the logic for shortCircuitBB.
  bool isFirstOptional = shortCircuitBB == nullptr;

  // If isFirstOptional, the final result will be computed in continueBB and
  // returned.
  BasicBlock *continueBB = nullptr;

  if (isFirstOptional) {
    // If shortCircuitBB is null, then this is the outermost in the optional
    // chain, so we must create it here and pass it through to every other
    // OptionalCallExpression and OptionalMemberExpression in the chain.
    continueBB = Builder.createBasicBlock(Builder.getFunction());
    auto *insertionBB = Builder.getInsertionBlock();
    shortCircuitBB = Builder.createBasicBlock(Builder.getFunction());
    Builder.setInsertionBlock(shortCircuitBB);
    values.push_back(Builder.getLiteralUndefined());
    // When short circuiting, just use 'undefined' as this, because the short
    // circuit will result in an attempt to call 'undefined' that will throw
    // anyway.
    baseValues.push_back(Builder.getLiteralUndefined());
    blocks.push_back(shortCircuitBB);
    Builder.createBranchInst(continueBB);
    Builder.setInsertionBlock(insertionBB);
  }

  Value *baseValue = nullptr;
  if (ESTree::OptionalMemberExpressionNode *ome =
          llvh::dyn_cast<ESTree::OptionalMemberExpressionNode>(mem->_object)) {
    baseValue = genOptionalMemberExpression(
                    ome, shortCircuitBB, MemberExpressionOperation::Load)
                    .result;
  } else if (
      ESTree::OptionalCallExpressionNode *oce =
          llvh::dyn_cast<ESTree::OptionalCallExpressionNode>(mem->_object)) {
    baseValue = genOptionalCallExpr(oce, shortCircuitBB);
  } else {
    baseValue = genExpression(mem->_object);
  }

  if (mem->_optional) {
    BasicBlock *evalRHSBB = Builder.createBasicBlock(Builder.getFunction());

    // If baseValue is undefined or null, then return undefined.
    // NOTE: We use `obj == null` to account for both null and undefined.
    Builder.createCondBranchInst(
        Builder.createBinaryOperatorInst(
            baseValue,
            Builder.getLiteralNull(),
            ValueKind::BinaryEqualInstKind),
        shortCircuitBB,
        evalRHSBB);

    // baseValue is not undefined, look up the property properly.
    Builder.setInsertionBlock(evalRHSBB);
  }

  Value *prop = genMemberExpressionProperty(mem);
  Value *result = nullptr;
  switch (op) {
    case MemberExpressionOperation::Load:
      if (auto *PN =
              llvh::dyn_cast<ESTree::PrivateNameNode>(getProperty(mem))) {
        result = emitPrivateLookup(baseValue, prop, PN);
      } else {
        result = Builder.createLoadPropertyInst(baseValue, prop);
      }
      break;
    case MemberExpressionOperation::Delete:
      result = Builder.createDeletePropertyInst(baseValue, prop);
      break;
  }
  assert(result && "result must be set");

  if (isFirstOptional) {
    values.push_back(result);
    baseValues.push_back(baseValue);
    blocks.push_back(Builder.getInsertionBlock());
    Builder.createBranchInst(continueBB);

    Builder.setInsertionBlock(continueBB);
    PhiInst *valuePhi = Builder.createPhiInst(values, blocks);
    PhiInst *basePhi = Builder.createPhiInst(baseValues, blocks);
    return {valuePhi, nullptr, basePhi};
  }

  // If this isn't the first optional, no Phi needed, just return the result.
  return {result, nullptr, baseValue};
}

Value *ESTreeIRGen::genCallEvalExpr(ESTree::CallExpressionNode *call) {
  Value *callee = genExpression(call->_callee);

  llvh::SmallVector<Value *, 1> args;
  for (auto &arg : call->_arguments) {
    args.push_back(genExpression(&arg));
  }

  Function *function = Builder.getFunction();
  BasicBlock *evalBlock = Builder.createBasicBlock(function);
  BasicBlock *callBlock = Builder.createBasicBlock(function);
  BasicBlock *nextBlock = Builder.createBasicBlock(function);

  // Check if it is actually calling eval().
  auto *isEval = Builder.createBinaryOperatorInst(
      callee,
      Builder.createGetBuiltinClosureInst(BuiltinMethod::globalThis_eval),
      ValueKind::BinaryStrictlyEqualInstKind);
  Builder.createCondBranchInst(isEval, evalBlock, callBlock);

  // Perform a direct eval.
  Builder.setInsertionBlock(evalBlock);
  Value *evalRes = Builder.createDirectEvalInst(
      args.empty() ? Builder.getLiteralUndefined() : args[0],
      curFunction()->function->isStrictMode());
  Builder.createBranchInst(nextBlock);

  // Perform a normal call.
  Builder.setInsertionBlock(callBlock);
  Value *callRes = Builder.createCallInst(
      callee,
      /* newTarget */ Builder.getLiteralUndefined(),
      /* thisValue */ Builder.getLiteralUndefined(),
      args);
  Builder.createBranchInst(nextBlock);

  Builder.setInsertionBlock(nextBlock);
  PhiInst *result = Builder.createPhiInst();
  result->addEntry(evalRes, evalBlock);
  result->addEntry(callRes, callBlock);
  return result;
}

Value *ESTreeIRGen::genObjectExpr(ESTree::ObjectExpressionNode *Expr) {
  LLVM_DEBUG(llvh::dbgs() << "Initializing a new object\n");

  if (flow::ExactObjectType *objType = llvh::dyn_cast<flow::ExactObjectType>(
          flowContext_.getNodeTypeOrAny(Expr)->info)) {
    // Indexer-typed object literals (e.g. produced by spreading a dictionary)
    // have no fixed slots, so the slot-based typed lowering cannot serve them.
    // Fall through to the dynamic lowering below, which copies spread sources
    // with copyDataProperties.
    if (!objType->hasIndexer())
      return genTypedObjectExpr(Expr, objType);
  }

  /// Store information about a property. Is it an accessor (getter/setter) or
  /// a value, and the actual value.
  class PropertyValue {
   public:
    /// Is this a getter/setter value.
    bool isAccessor = false;
    /// Tracks the state of generating IR for this value.
    enum { None, Placeholder, IRGenerated } state{None};
    /// The value, if this is a regular property
    ESTree::Node *valueNode{};
    /// Getter PropertyNode, if this is an accessor property.
    ESTree::PropertyNode *getterProp{};
    /// Getter accessor, if this is an accessor property.
    ESTree::FunctionExpressionNode *getterNode{};
    /// Setter PropertyNode, if this is an accessor property.
    ESTree::PropertyNode *setterProp{};
    /// Setter accessor, if this is an accessor property.
    ESTree::FunctionExpressionNode *setterNode{};

    void setValue(ESTree::Node *val) {
      isAccessor = false;
      valueNode = val;
      getterNode = setterNode = nullptr;
    }
    void setGetter(
        ESTree::FunctionExpressionNode *get,
        ESTree::PropertyNode *prop) {
      if (!isAccessor) {
        valueNode = nullptr;
        setterNode = nullptr;
        setterProp = nullptr;
        isAccessor = true;
      }
      getterNode = get;
      getterProp = prop;
    }
    void setSetter(
        ESTree::FunctionExpressionNode *set,
        ESTree::PropertyNode *prop) {
      if (!isAccessor) {
        valueNode = nullptr;
        getterNode = nullptr;
        getterProp = nullptr;
        isAccessor = true;
      }
      setterNode = set;
      setterProp = prop;
    }
  };

  // First accumulate all getters and setters. We walk all properties, convert
  // them to string and store the value into propMap, possibly overwriting the
  // previous value.
  // Note that computed properties are not stored in the propMap as we do not
  // know the keys at compilation time.
  llvh::StringMap<PropertyValue> propMap;
  // The first location where a given property name is encountered.
  llvh::StringMap<SMRange> firstLocMap;
  llvh::SmallVector<char, 32> stringStorage;
  /// The optional __proto__ property.
  ESTree::PropertyNode *protoProperty = nullptr;
  // True if we need to capture the object literal being built into a Variable.
  // This is in order to support the `super` references.
  bool shouldCaptureObjForHomeObject = false;

  for (auto &P : Expr->_properties) {
    if (llvh::isa<ESTree::SpreadElementNode>(&P)) {
      continue;
    }

    // We are reusing the storage, so make sure it is cleared at every
    // iteration.
    stringStorage.clear();

    auto *prop = cast<ESTree::PropertyNode>(&P);
    shouldCaptureObjForHomeObject |= prop->_method ||
        prop->_kind == kw_.identSet || prop->_kind == kw_.identGet;
    if (prop->_computed) {
      // Can't store any useful information if the name is computed.
      // Just generate the code in the next loop.
      continue;
    }

    auto propName = propertyKeyAsString(stringStorage, prop->_key);

    // protoProperty should only be recorded if the property is not a method
    // nor a shorthand value.
    if (prop->_kind == kw_.identInit && propName == "__proto__" &&
        !prop->_method && !prop->_shorthand) {
      if (!protoProperty) {
        protoProperty = prop;
      } else {
        Builder.getModule()->getContext().getSourceErrorManager().error(
            prop->getSourceRange(),
            "__proto__ was set multiple times in the object definition.");
        Builder.getModule()->getContext().getSourceErrorManager().note(
            protoProperty->getSourceRange(), "The first definition was here.");
      }
      continue;
    }

    PropertyValue *propValue = &propMap[propName];
    if (prop->_kind == kw_.identGet) {
      propValue->setGetter(
          cast<ESTree::FunctionExpressionNode>(prop->_value), prop);
    } else if (prop->_kind == kw_.identSet) {
      propValue->setSetter(
          cast<ESTree::FunctionExpressionNode>(prop->_value), prop);
    } else {
      assert(prop->_kind == kw_.identInit && "invalid PropertyNode kind");
      // We record the propValue if this is a regular property
      propValue->setValue(prop->_value);
    }

    std::string key = (prop->_kind->str() + propName).str();
    auto iterAndSuccess = firstLocMap.try_emplace(key, prop->getSourceRange());
    if (!iterAndSuccess.second) {
      Builder.getModule()->getContext().getSourceErrorManager().warning(
          prop->getSourceRange(),
          Twine("the property \"") + propName +
              "\" was set multiple times in the object definition.");

      Builder.getModule()->getContext().getSourceErrorManager().note(
          iterAndSuccess.first->second, "The first definition was here.");
    }
  }

  /// Attempt to determine whether we can directly use the value of the
  /// __proto__ property as a parent when creating the object for an object
  /// initializer, instead of setting it later with
  /// HermesInternal.silentSetPrototypeOf(). That is not possible to determine
  /// statically in the general case, but we can check for the simple cases:
  /// - __proto__ property is first.
  /// - the value of __proto__ is constant.
  Value *objectParent = nullptr;
  if (protoProperty &&
      (&Expr->_properties.front() == protoProperty ||
       isConstantExpr(protoProperty->_value))) {
    objectParent = genExpression(protoProperty->_value);
  }

  // Allocate a new javascript object on the heap.
  auto Obj = Builder.createAllocObjectLiteralInst({}, objectParent);
  Variable *capturedObj = nullptr;
  if (shouldCaptureObjForHomeObject) {
    // Capture the value of this object literal, which will become the home
    // object for any methods/accessors defined in this literal.
    capturedObj = Builder.createVariable(
        curFunction()->curScope()->getVariableScope(),
        "?obj",
        Type::createObject(),
        true);
    Builder.createStoreFrameInst(curFunction()->curScope(), Obj, capturedObj);
  }

  // Initialize all properties. We check whether the value of each property
  // will be overwritten (by comparing against what we have saved in propMap).
  // In that case we still compute the value (it could have side effects), but
  // we don't store it. The exception to this are accessor functions - there
  // is no need to create them if we don't use them because creating a function
  // has no side effects.
  for (auto &P : Expr->_properties) {
    if (auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(&P)) {
      genBuiltinCall(
          BuiltinMethod::HermesBuiltin_copyDataProperties,
          {Obj, genExpression(spread->_argument)});
      continue;
    }

    // We are reusing the storage, so make sure it is cleared at every
    // iteration.
    stringStorage.clear();

    auto *prop = cast<ESTree::PropertyNode>(&P);

    if (prop->_computed) {
      // If true, we need to set the "name" property of the value being
      // generated.
      bool needsSetFunctionName = false;
      int prefix = 0;
      if (prop->_method) {
        needsSetFunctionName = true;
      } else if (prop->_kind == kw_.identSet || prop->_kind == kw_.identGet) {
        needsSetFunctionName = true;
        prefix = prop->_kind == kw_.identGet ? 1 : 2;
      } else {
        auto *valueNode = prop->_value;
        if (auto *fe =
                llvh::dyn_cast<ESTree::FunctionExpressionNode>(valueNode)) {
          needsSetFunctionName = !fe->_id;
        } else if (llvh::isa<ESTree::ArrowFunctionExpressionNode>(valueNode)) {
          needsSetFunctionName = true;
        } else if (
            auto *ce = llvh::dyn_cast<ESTree::ClassExpressionNode>(valueNode)) {
          needsSetFunctionName = !ce->_id;
        }
      }

      auto *key = genExpression(prop->_key);
      Value *propKey =
          needsSetFunctionName ? Builder.createToPropertyKeyInst(key) : key;

      Value *value;
      if (prop->_method) {
        value = genFunctionExpression(
            llvh::cast<ESTree::FunctionExpressionNode>(prop->_value),
            Identifier{},
            nullptr,
            Function::DefinitionKind::ES6Method,
            capturedObj,
            prop);
      } else if (prop->_kind == kw_.identSet || prop->_kind == kw_.identGet) {
        value = genFunctionExpression(
            llvh::cast<ESTree::FunctionExpressionNode>(prop->_value),
            Identifier{},
            /* superClassNode */ nullptr,
            Function::DefinitionKind::ES5Function,
            /* homeObject */ capturedObj,
            /* parentNode */ prop);
      } else {
        value = genExpression(prop->_value, Identifier{});
      }

      if (needsSetFunctionName) {
        genBuiltinCall(
            BuiltinMethod::HermesBuiltin_setFunctionName,
            {value, propKey, Builder.getLiteralNumber(prefix)});
      }
      if (prop->_kind == kw_.identGet) {
        Builder.createDefineOwnGetterSetterInst(
            value,
            Builder.getLiteralUndefined(),
            Obj,
            propKey,
            IRBuilder::PropEnumerable::Yes);
      } else if (prop->_kind == kw_.identSet) {
        Builder.createDefineOwnGetterSetterInst(
            Builder.getLiteralUndefined(),
            value,
            Obj,
            propKey,
            IRBuilder::PropEnumerable::Yes);
      } else {
        Builder.createDefineOwnPropertyInst(
            value, Obj, propKey, IRBuilder::PropEnumerable::Yes);
      }
      continue;
    }

    llvh::StringRef keyStr = propertyKeyAsString(stringStorage, prop->_key);

    if (prop == protoProperty) {
      // This is the first definition of __proto__. If we already used it
      // as an object parent we just skip it, but otherwise we must
      // explicitly set the parent now by calling \c
      // HermesInternal.silentSetPrototypeOf().
      if (!objectParent) {
        auto *parent = genExpression(prop->_value);

        IRBuilder::SaveRestore saveState{Builder};
        Builder.setLocation(prop->_key->getDebugLoc());

        genBuiltinCall(
            BuiltinMethod::HermesBuiltin_silentSetPrototypeOf, {Obj, parent});
      }

      continue;
    }

    PropertyValue *propValue = &propMap[keyStr];

    // For any node that has a corresponding propValue, we need to ensure that
    // the we insert either a placeholder or the final IR before the end of this
    // iteration.
    auto checkState = llvh::make_scope_exit(
        [&] { assert(propValue->state != PropertyValue::None); });

    auto *Key = Builder.getLiteralString(keyStr);

    auto maybeInsertPlaceholder = [&] {
      if (propValue->state == PropertyValue::None) {
        // This value is going to be overwritten, but insert a placeholder in
        // order to maintain insertion order.
        Builder.createDefineOwnPropertyInst(
            Builder.getLiteralNull(), Obj, Key, IRBuilder::PropEnumerable::Yes);
        propValue->state = PropertyValue::Placeholder;
      }
    };

    if (prop->_kind == kw_.identGet || prop->_kind == kw_.identSet) {
      // If  we already generated it, skip.
      if (propValue->state == PropertyValue::IRGenerated)
        continue;

      if (!propValue->isAccessor) {
        maybeInsertPlaceholder();
        continue;
      }

      Value *getter = Builder.getLiteralUndefined();
      Value *setter = Builder.getLiteralUndefined();

      if (propValue->getterNode) {
        assert(propValue->getterProp && "PropertyNode must exist");
        getter = genFunctionExpression(
            propValue->getterNode,
            Builder.createIdentifier("get " + keyStr.str()),
            /* superClassNode */ nullptr,
            Function::DefinitionKind::ES5Function,
            /* homeObject */ capturedObj,
            /* parentNode */ propValue->getterProp);
      }

      if (propValue->setterNode) {
        assert(propValue->setterProp && "PropertyNode must exist");
        setter = genFunctionExpression(
            propValue->setterNode,
            Builder.createIdentifier("set " + keyStr.str()),
            /* superClassNode */ nullptr,
            Function::DefinitionKind::ES5Function,
            /* homeObject */ capturedObj,
            /* parentNode */ propValue->setterProp);
      }

      Builder.createDefineOwnGetterSetterInst(
          getter, setter, Obj, Key, IRBuilder::PropEnumerable::Yes);

      propValue->state = PropertyValue::IRGenerated;

      continue;
    }

    // Always generate the values, even if we don't need it, for the side
    // effects.
    auto nameHint = Builder.createIdentifier(keyStr);
    auto *value = prop->_method
        ? genFunctionExpression(
              llvh::cast<ESTree::FunctionExpressionNode>(prop->_value),
              nameHint,
              nullptr,
              Function::DefinitionKind::ES6Method,
              capturedObj,
              prop)
        : genExpression(prop->_value, nameHint);

    // Only store the value if it won't be overwritten.
    if (propMap[keyStr].valueNode == prop->_value) {
      assert(
          propValue->state != PropertyValue::IRGenerated &&
          "IR can only be generated once");
      Builder.createDefineOwnPropertyInst(
          value, Obj, Key, IRBuilder::PropEnumerable::Yes);
      propValue->state = PropertyValue::IRGenerated;
    } else {
      maybeInsertPlaceholder();
    }
  }

  // Return the newly allocated object (because this is an expression, not a
  // statement).
  return Obj;
}

/// Generate IR for the typed object literal \p Expr.
Value *ESTreeIRGen::genTypedObjectExpr(
    ESTree::ObjectExpressionNode *Expr,
    flow::ExactObjectType *type) {
  // Evaluate the value expressions and keep pointers to the vales.
  // Mapping from property name to value and PrLoad index.
  // If Value is nullptr, then the value was already emitted as part of the
  // object literal and shouldn't be PrStored.
  // The order of PrStores doesn't matter because they have no side effects,
  // it just needs to be deterministic, so use MapVector for iteration.
  llvh::SmallMapVector<Identifier, std::pair<Value *, size_t>, 8>
      storedValues{};
  llvh::SmallVector<char, 32> stringStorage{};
  for (auto &node : Expr->_properties) {
    if (auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(&node)) {
      // FlowChecker degrades the enclosing object literal to 'any' when
      // a spread argument has type 'any'. This function is not
      // run in that case, so the spread type must be ExactObjectType if we're
      // here.
      auto *spreadInfo = flowContext_.getNodeTypeOrAny(spread->_argument)->info;
      assert(
          llvh::isa<flow::ExactObjectType>(spreadInfo) &&
          "spread source in typed object literal must be ExactObjectType");
      auto *srcType = llvh::cast<flow::ExactObjectType>(spreadInfo);
      Value *src = genExpression(spread->_argument);
      for (size_t i = 0, e = srcType->getFields().size(); i < e; ++i) {
        const auto &srcField = srcType->getFields()[i];
        Value *loaded = Builder.createPrLoadInst(
            src,
            i,
            Builder.getLiteralString(srcField.name),
            flowTypeToIRType(srcField.type));
        auto optIndex = type->findField(srcField.name);
        assert(
            optIndex.hasValue() &&
            "Spread field must exist in destination type");
        storedValues[srcField.name] = {loaded, *optIndex};
      }
      continue;
    }

    auto *prop = llvh::cast<ESTree::PropertyNode>(&node);
    assert(
        !prop->_computed && !prop->_method && prop->_kind == kw_.identInit &&
        "Unexpected property kind in typechecked object");

    stringStorage.clear();
    Identifier propName = Builder.createIdentifier(
        propertyKeyAsString(stringStorage, prop->_key));

    auto optIndex = type->findField(propName);
    assert(optIndex.hasValue() && "Expected field to exist");

    Value *storedValue = genExpression(prop->_value);
    // Overwrite the previous value if there is one.
    // We've already run the expression for side effects.
    storedValues[propName] = {storedValue, *optIndex};
  }

  // TODO: Have a specific instruction for allocating an object
  // that uses the prop map, makes a sealed object, uses uninitialized fields
  // instead of default init value, etc.

  // Allocate the object with the correct property names.
  AllocObjectLiteralInst::ObjectPropertyMap propMap{};
  propMap.reserve(type->getFields().size());
  for (size_t i = 0, e = type->getFields().size(); i < e; ++i) {
    const auto &field = type->getFields()[i];
    auto it = storedValues.find(field.name);
    assert(
        it != storedValues.end() &&
        "Missing stored value in typechecked object literal");
    // Use a literal if possible, otherwise use the default init value.
    // Avoid putting non-literals here because we want to emit PrStore
    // instead of DefineOwnPropertyInst.
    Value *initValue = nullptr;
    if (llvh::isa<Literal>(it->second.first)) {
      initValue = it->second.first;
      // Prevent emitting PrStore later for literals.
      it->second.first = nullptr;
      assert(it->second.second == i && "Stored value must have same index");
    } else {
      initValue = getDefaultInitValue(field.type);
    }
    propMap.emplace_back(Builder.getLiteralString(field.name), initValue);
  }

  Value *result = propMap.empty()
      ? Builder.createAllocObjectLiteralInst()
      : Builder.createAllocObjectLiteralInst(propMap);

  // Store the remaining non-literal properties.
  for (const auto &[name, valueAndIdx] : storedValues) {
    const auto &[storedValue, idx] = valueAndIdx;
    if (storedValue) {
      // We only put strings into propMap.
      auto *name = llvh::cast<LiteralString>(propMap[idx].first);
      Builder.createPrStoreInst(
          storedValue,
          result,
          idx,
          name,
          getTypeContext().isNonPtr(
              flowTypeToIRType(type->getFields()[idx].type)));
    }
  }

  return result;
}

Value *ESTreeIRGen::genSequenceExpr(ESTree::SequenceExpressionNode *Sq) {
  Value *result = Builder.getLiteralUndefined();

  // Generate all expressions in the sequence, but take only the last one.
  for (auto &Ex : Sq->_expressions) {
    result = genExpression(&Ex);
  }

  return result;
}

Value *ESTreeIRGen::genYieldExpr(ESTree::YieldExpressionNode *Y) {
  assert(!Y->_delegate && "must use genYieldStarExpr for yield*");

  Value *value = Y->_argument ? genExpression(Y->_argument)
                              : Builder.getLiteralUndefined();
  return genYieldOrAwaitExpr(value);
}

Value *ESTreeIRGen::genAwaitExpr(ESTree::AwaitExpressionNode *A) {
  Value *value = genExpression(A->_argument);
  return genYieldOrAwaitExpr(value);
}

Value *ESTreeIRGen::genYieldOrAwaitExpr(Value *value) {
  auto *bb = Builder.getInsertionBlock();
  auto *next = Builder.createBasicBlock(bb->getParent());

  auto *resumeIsReturn = Builder.createAllocStackInst(
      genAnonymousLabelName("isReturn"), Type::createBoolean());

  Builder.createSaveAndYieldInst(value, Builder.getLiteralBool(false), next);
  Builder.setInsertionBlock(next);
  return genResumeGenerator(
      GenFinally::Yes,
      resumeIsReturn,
      Builder.createBasicBlock(bb->getParent()));
}

/// Generate the code for `yield* value`.
/// We use some stack locations to store state while iterating:
/// - received (the value passed by the user to .next(), etc)
/// - result (the final result of the yield* expression)
///
/// iteratorRecord stores the iterator which we are iterating over.
///
/// Final IR has the following basic blocks for normal control flow:
///
/// getNext: Get the next value from the iterator.
/// - Call next() on the iteratorRecord and stores to `result`
/// - If done, go to exit
/// - Otherwise, go to yieldNextRes
///
/// resumeGenTryStartBB: Runs the ResumeGenerator instruction, wrapped in a try.
/// - Code for `finally` is also emitted here.
///
/// yieldNextRes: Yield the result of the next() call
///
/// exit: Returns `result` which should have the final results stored in it.
///
/// When the user calls `.return`, the finalizer is executed to call
/// `iteratorRecord.return` if it exists. The code for that is contained within
/// the SurroundingTry. If the .return function is defined, it is called and the
/// 'done' property of the result of the call is used to either branch back to
/// the 'resume' block or to propagate the return.
///
/// When the user calls '.throw', the code in emitHandler is executed. All
/// generators used as delegates must have a .throw() method, so that is checked
/// for and called. The result is then used to either resume if not done, or to
/// return immediately by branching to the 'exit' block.
Value *ESTreeIRGen::genYieldStarExpr(ESTree::YieldExpressionNode *Y) {
  assert(Y->_delegate && "must use genYieldExpr for yield");
  auto *function = Builder.getInsertionBlock()->getParent();
  auto *getNextBlock = Builder.createBasicBlock(function);
  auto *yieldNextRes = Builder.createBasicBlock(function);
  auto *exitBlock = Builder.createBasicBlock(function);

  // Calls ResumeGenerator and returns or throws if requested.
  auto *resumeGenTryStartBB = Builder.createBasicBlock(function);

  // Ends the opened TryStartInst.
  auto *tryEndBB = Builder.createBasicBlock(function);

  auto *exprValue = genExpression(Y->_argument);
  IteratorRecordSlow iteratorRecord = emitGetIteratorSlow(exprValue);

  // The "received" value when the user resumes the generator.
  // Initialized to undefined on the first run, then stored to immediately
  // following any genResumeGenerator.
  auto *received = Builder.createAllocStackInst(
      genAnonymousLabelName("received"), Type::createAnyType());
  Builder.createStoreStackInst(Builder.getLiteralUndefined(), received);

  // The "isReturn" value when the user resumes the generator.
  // Stored to immediately following any genResumeGenerator.
  auto *resumeIsReturn = Builder.createAllocStackInst(
      genAnonymousLabelName("isReturn"), Type::createBoolean());

  // The final result of the `yield*` expression.
  // This can be set from either the body or the handler, so it is placed
  // in the stack to allow populating it from anywhere.
  auto *result = Builder.createAllocStackInst(
      genAnonymousLabelName("result"), Type::createAnyType());

  Builder.createBranchInst(getNextBlock);

  // 7.a.i.  Let innerResult be ? Call(
  //   iteratorRecord.[[NextMethod]],
  //   iteratorRecord.[[Iterator]],
  //   <received.[[Value]]>
  // )
  // Avoid using emitIteratorNext here because the spec does not.
  Builder.setInsertionBlock(getNextBlock);
  auto *nextResult = Builder.createCallInst(
      iteratorRecord.nextMethod,
      /* newTarget */ Builder.getLiteralUndefined(),
      iteratorRecord.iterator,
      {Builder.createLoadStackInst(received)});
  emitEnsureObject(nextResult, "iterator.next() did not return an object");

  Builder.createStoreStackInst(nextResult, result);
  auto *done = emitIteratorCompleteSlow(nextResult);
  Builder.createCondBranchInst(done, exitBlock, yieldNextRes);

  // The primary call path for yielding the next result.
  // It's important to put the SaveAndYield for the .next() value in a separate
  // BB then ResumeGenerator. This is because other BBs need to jump to
  // ResumeGenerator after completing a different SaveAndYield.
  Builder.setInsertionBlock(yieldNextRes);
  Builder.createSaveAndYieldInst(
      nextResult, Builder.getLiteralBool(true), resumeGenTryStartBB);

  Builder.setInsertionBlock(resumeGenTryStartBB);
  emitTryCatchScaffolding(
      getNextBlock,
      // emitBody.
      [this,
       Y,
       resumeIsReturn,
       received,
       resumeGenTryStartBB,
       &iteratorRecord,
       tryEndBB](BasicBlock *catchBlock) {
        // Generate IR for the body of Try
        SurroundingTry thisTry{
            curFunction(),
            Y,
            catchBlock,
            {},
            [this, &iteratorRecord, received, resumeGenTryStartBB](
                ESTree::Node *, ControlFlowChange cfc, BasicBlock *) {
              if (cfc == ControlFlowChange::Break) {
                // This finalizer block is executed upon early return during
                // the yield*, which happens when the user requests a .return().
                auto *function = Builder.getFunction();
                auto *haveReturnBB = Builder.createBasicBlock(function);
                auto *noReturnBB = Builder.createBasicBlock(function);
                auto *isDoneBB = Builder.createBasicBlock(function);
                auto *isNotDoneBB = Builder.createBasicBlock(function);

                // Check if "returnMethod" is undefined.
                auto *returnMethod = genBuiltinCall(
                    BuiltinMethod::HermesBuiltin_getMethod,
                    {iteratorRecord.iterator,
                     Builder.getLiteralString("return")});
                auto *noReturn = Builder.createBinaryOperatorInst(
                    returnMethod,
                    Builder.getLiteralUndefined(),
                    ValueKind::BinaryStrictlyEqualInstKind);
                Builder.createCondBranchInst(
                    noReturn, noReturnBB, haveReturnBB);

                Builder.setInsertionBlock(haveReturnBB);
                // iv. Let innerReturnResult be
                // ? Call(return, iterator, received.[[Value]]).
                auto *innerReturnResult = Builder.createCallInst(
                    returnMethod,
                    /* newTarget */ Builder.getLiteralUndefined(),
                    iteratorRecord.iterator,
                    {Builder.createLoadStackInst(received)});
                // vi. If Type(innerReturnResult) is not Object,
                // throw a TypeError exception.
                emitEnsureObject(
                    innerReturnResult,
                    "iterator.return() did not return an object");
                // vii. Let done be ? IteratorComplete(innerReturnResult).
                auto *done = emitIteratorCompleteSlow(innerReturnResult);
                Builder.createCondBranchInst(done, isDoneBB, isNotDoneBB);

                Builder.setInsertionBlock(isDoneBB);
                // viii. 1. Let value be ? IteratorValue(innerReturnResult).
                auto *value = emitIteratorValueSlow(innerReturnResult);
                genFinallyBeforeControlChange(
                    curFunction()->surroundingTry,
                    nullptr,
                    ControlFlowChange::Break);
                // viii. 2. Return Completion
                // { [[Type]]: return, [[Value]]: value, [[Target]]: empty }.
                Builder.createReturnInst(value);

                // x. Else, set received to GeneratorYield(innerReturnResult).
                Builder.setInsertionBlock(isNotDoneBB);
                Builder.createSaveAndYieldInst(
                    innerReturnResult,
                    Builder.getLiteralBool(true),
                    resumeGenTryStartBB);

                // If return is undefined, return Completion(received).
                Builder.setInsertionBlock(noReturnBB);
              }
            }};

        // Note that resumeGenTryStartBB was created above to allow all
        // SaveAndYield insts to have the same resume point (including
        // SaveAndYield in the catch handler), but we must populate it inside
        // the scaffolding so that the SurroundingTry is correct for the
        // genFinallyBeforeControlChange call emitted by genResumeGenerator.
        genResumeGenerator(GenFinally::Yes, resumeIsReturn, tryEndBB, received);

        // SaveAndYieldInst is a Terminator, but emitTryCatchScaffolding
        // needs a block from which to Branch to the TryEnd instruction.
        // Make a dummy block which can do that.
        Builder.setInsertionBlock(tryEndBB);
      },
      // emitNormalCleanup.
      []() {},
      // emitHandler.
      [this, exitBlock, result, &iteratorRecord, &resumeGenTryStartBB, Y](
          BasicBlock *getNextBlock) {
        auto *catchReg = Builder.createCatchInst();

        auto *function = Builder.getFunction();
        auto *hasThrowMethodBB = Builder.createBasicBlock(function);
        auto *noThrowMethodBB = Builder.createBasicBlock(function);
        auto *isDoneBB = Builder.createBasicBlock(function);
        auto *isNotDoneBB = Builder.createBasicBlock(function);

        // b.i. Let throw be ? GetMethod(iterator, "throw").
        auto *throwMethod = genBuiltinCall(
            BuiltinMethod::HermesBuiltin_getMethod,
            {iteratorRecord.iterator, Builder.getLiteralString("throw")});
        auto *hasThrowMethod = Builder.createBinaryOperatorInst(
            throwMethod,
            Builder.getLiteralUndefined(),
            ValueKind::BinaryStrictlyEqualInstKind);
        Builder.createCondBranchInst(
            hasThrowMethod, noThrowMethodBB, hasThrowMethodBB);

        // ii. If throw is not undefined, then
        Builder.setInsertionBlock(hasThrowMethodBB);
        // ii. 1. Let innerResult be
        //        ? Call(throw, iterator, « received.[[Value]] »).
        // ii. 3. NOTE: Exceptions from the inner iterator throw method are
        // propagated. Normal completions from an inner throw method are
        // processed similarly to an inner next.
        auto *innerResult = Builder.createCallInst(
            throwMethod,
            /* newTarget */ Builder.getLiteralUndefined(),
            iteratorRecord.iterator,
            {catchReg});
        // ii. 4. If Type(innerResult) is not Object,
        //        throw a TypeError exception.
        emitEnsureObject(
            innerResult, "iterator.throw() did not return an object");
        // ii. 5. Let done be ? IteratorComplete(innerResult).
        auto *done = emitIteratorCompleteSlow(innerResult);
        Builder.createCondBranchInst(done, isDoneBB, isNotDoneBB);

        // ii. 6. If done is true, then return ? IteratorValue(innerResult).
        Builder.setInsertionBlock(isDoneBB);
        Builder.createStoreStackInst(innerResult, result);
        Builder.createBranchInst(exitBlock);

        // ii. 8. Else, set received to GeneratorYield(innerResult).
        Builder.setInsertionBlock(isNotDoneBB);
        Builder.createSaveAndYieldInst(
            innerResult, Builder.getLiteralBool(true), resumeGenTryStartBB);

        // NOTE: If iterator does not have a throw method, this throw is
        // going to terminate the yield* loop. But first we need to give
        // iterator a chance to clean up.
        Builder.setInsertionBlock(noThrowMethodBB);
        emitIteratorCloseSlow(Y, iteratorRecord, false);
        Builder.createThrowTypeErrorInst(Builder.getLiteralString(
            "yield* delegate must have a .throw() method"));
      });

  Builder.setInsertionBlock(exitBlock);

  return emitIteratorValueSlow(Builder.createLoadStackInst(result));
}

Value *ESTreeIRGen::genResumeGenerator(
    GenFinally genFinally,
    AllocStackInst *isReturn,
    BasicBlock *nextBB,
    AllocStackInst *received) {
  auto *resume = Builder.createResumeGeneratorInst(isReturn);
  if (received) {
    Builder.createStoreStackInst(resume, received);
  }
  auto *retBB =
      Builder.createBasicBlock(Builder.getInsertionBlock()->getParent());

  Builder.createCondBranchInst(
      Builder.createLoadStackInst(isReturn), retBB, nextBB);

  Builder.setInsertionBlock(retBB);
  if (received) {
    Builder.createStoreStackInst(resume, received);
  }
  if (genFinally == GenFinally::Yes) {
    genFinallyBeforeControlChange(
        curFunction()->surroundingTry, nullptr, ControlFlowChange::Break);
  }
  Builder.createReturnInst(resume);

  Builder.setInsertionBlock(nextBB);
  return resume;
}

Value *ESTreeIRGen::genBinaryExpression(ESTree::BinaryExpressionNode *bin) {
  // Handle long chains of +/- non-recursively.
  if (bin->_operator == kw_.identPlus || bin->_operator == kw_.identMinus) {
    auto list = linearizeLeft(bin, {"+", "-"});

    Value *LHS = genExpression(list[0]->_left);
    for (auto *e : list) {
      Value *RHS = genExpression(e->_right);
      Builder.setLocation(e->getDebugLoc());
      auto Kind = BinaryOperatorInst::parseOperator(e->_operator->str());
      BinaryOperatorInst *result =
          Builder.createBinaryOperatorInst(LHS, RHS, Kind);
      LHS = result;
    }

    return LHS;
  }

  ValueKind kind;
  Value *LHS;
  if (auto *PN = llvh::dyn_cast<ESTree::PrivateNameNode>(bin->_left)) {
    // If we are seeing the form `#privateName in val`, generate a different
    // kind of `in` operator.
    assert(bin->_operator == kw_.identIn);
    kind = ValueKind::BinaryPrivateInInstKind;
    LHS = genPrivateNameValue(llvh::cast<ESTree::IdentifierNode>(PN->_id));
  } else {
    kind = BinaryOperatorInst::parseOperator(bin->_operator->str());
    LHS = genExpression(bin->_left);
  }
  Value *RHS = genExpression(bin->_right);

  Instruction *res = Builder.createBinaryOperatorInst(LHS, RHS, kind);

  // If the binary operator is a comparison, set the result type to boolean.
  static_assert(
      (int)ValueKind::BinaryGreaterThanOrEqualInstKind -
              (int)ValueKind::BinaryEqualInstKind + 1 ==
          8,
      "Comparison instructions must be consecutive");
  if ((kind >= ValueKind::BinaryEqualInstKind &&
       kind <= ValueKind::BinaryGreaterThanOrEqualInstKind) ||
      kind == ValueKind::BinaryInInstKind ||
      kind == ValueKind::BinaryPrivateInInstKind ||
      kind == ValueKind::BinaryInstanceOfInstKind) {
    res->setType(Type::createBoolean());
  }

  return res;
}

Value *ESTreeIRGen::genUnaryExpression(ESTree::UnaryExpressionNode *U) {
  UniqueString *oper = U->_operator;

  // Handle the delete unary expression. https://es5.github.io/#x11.4.1
  if (oper == kw_.identDelete) {
    if (auto *memberExpr =
            llvh::dyn_cast<ESTree::MemberExpressionNode>(U->_argument)) {
      LLVM_DEBUG(llvh::dbgs() << "IRGen delete member expression.\n");

      return genMemberExpression(memberExpr, MemberExpressionOperation::Delete)
          .result;
    }

    if (auto *memberExpr = llvh::dyn_cast<ESTree::OptionalMemberExpressionNode>(
            U->_argument)) {
      LLVM_DEBUG(llvh::dbgs() << "IRGen delete optional member expression.\n");

      return genOptionalMemberExpression(
                 memberExpr, nullptr, MemberExpressionOperation::Delete)
          .result;
    }

    // Check for "delete identifier". Note that deleting unqualified identifiers
    // is prohibited in strict mode, so that case is handled earlier in the
    // semantic validator. Here we are left to handle the non-strict mode case.
    if (auto *iden = llvh::dyn_cast<ESTree::IdentifierNode>(U->_argument)) {
      assert(
          !curFunction()->function->isStrictMode() &&
          "delete identifier encountered in strict mode");
      // Check if this is a known variable.
      Identifier name = getNameFieldFromID(iden);
      auto *var = resolveIdentifier(iden);

      if (llvh::isa<GlobalObjectProperty>(var)) {
        // If the variable doesn't exist or if it is global, we must generate
        // a delete global property instruction.
        return Builder.createDeletePropertyInst(
            Builder.getGlobalObject(), Builder.getLiteralString(name));
      } else {
        // Otherwise it is a local variable which can't be deleted and we just
        // return false.
        return Builder.getLiteralBool(false);
      }
    }

    // Generate the code for the delete operand.
    genExpression(U->_argument);

    // Deleting any value or a result of an expression returns True.
    return Builder.getLiteralBool(true);
  }

  // Need to handle the special case of "typeof <undefined variable>".
  if (oper == kw_.identTypeof) {
    if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(U->_argument)) {
      Value *argument = genIdentifierExpression(id, true);
      return Builder.createTypeOfInst(argument);
    }
  }

  // Generate the unary operand:
  Value *argument = genExpression(U->_argument);

  if (oper == kw_.identPlus) {
    return Builder.createAsNumberInst(argument);
  } else if (oper == kw_.identTypeof) {
    return Builder.createTypeOfInst(argument);
  } else {
    ValueKind kind = UnaryOperatorInst::parseOperator(oper->str());

    // Set the type of unary operators that have a fixed result type.
    Type type = Type::createAnyType();
    switch (kind) {
      case ValueKind::UnaryVoidInstKind:
        type = Type::createUndefined();
        break;
      case ValueKind::UnaryBangInstKind:
        type = Type::createBoolean();
        break;
      default:
        break;
    }
    return Builder.createUnaryOperatorInst(argument, kind, type);
  }
}

Value *ESTreeIRGen::genUpdateExpr(ESTree::UpdateExpressionNode *updateExpr) {
  LLVM_DEBUG(llvh::dbgs() << "IRGen update expression.\n");
  bool isPrefix = updateExpr->_prefix;

  ValueKind opKind;
  if (updateExpr->_operator == kw_.identPlusPlus) {
    opKind = ValueKind::UnaryIncInstKind;
  } else if (updateExpr->_operator == kw_.identMinusMinus) {
    opKind = ValueKind::UnaryDecInstKind;
  } else {
    llvm_unreachable("Invalid update operator");
  }

  LReference lref = createLRef(updateExpr->_argument, false);

  // Load the original value.
  Value *original = enforceExprType(lref.emitLoad(), updateExpr->_argument);

  // Postfix updates need to convert the original value to numeric before
  // Inc/Dec to ensure the updateExpr has the proper result value.
  if (!isPrefix &&
      !getTypeContext().isSubsetOf(original->getType(), Type::createNumeric()))
    original = Builder.createAsNumericInst(original);

  // Create the inc or dec.
  Value *result = Builder.createUnaryOperatorInst(original, opKind);
  // Number and BigInt types are preserved, otherwise the result is numeric.
  result->setType(
      original->getType().isNumberType() || original->getType().isBigIntType()
          ? original->getType()
          : Type::createNumeric());

  result = enforceExprType(result, updateExpr);

  // Store the result.
  lref.emitStore(result);

  // Depending on the prefixness return the previous value or the modified
  // value.
  return (isPrefix ? result : original);
}

/// Extract a name hint from a LReference.
static Identifier extractNameHint(const LReference &lref) {
  Identifier nameHint{};
  if (auto *var = lref.castAsVariable()) {
    nameHint = var->getName();
  } else if (auto *globProp = lref.castAsGlobalObjectProperty()) {
    nameHint = globProp->getName()->getValue();
  }
  return nameHint;
}

Value *ESTreeIRGen::genAssignmentExpr(ESTree::AssignmentExpressionNode *AE) {
  LLVM_DEBUG(llvh::dbgs() << "IRGen assignment operator.\n");

  auto opStr = AE->_operator->str();

  // Handle nested normal assignments non-recursively.
  if (opStr == "=") {
    auto list = ESTree::linearizeRight(AE, {"="});

    // Create an LReference for every assignment left side.
    llvh::SmallVector<LReference, 1> lrefs;
    lrefs.reserve(list.size());
    for (auto *e : list) {
      lrefs.push_back(createLRef(e->_left, false));
    }

    Value *RHS = nullptr;
    auto lrefIterator = lrefs.end();
    for (auto *e : llvh::make_range(list.rbegin(), list.rend())) {
      --lrefIterator;
      if (!RHS)
        RHS = genExpression(e->_right, extractNameHint(*lrefIterator));
      Builder.setLocation(e->getDebugLoc());
      lrefIterator->emitStore(RHS);
    }

    return RHS;
  }

  assert(opStr != "=" && "Assignment was already handled");

  // The implicit cast may exist here to signal that the result of the binary
  // expression should be implicitly casted, so skip it when computing the lref.
  auto *optImplicitCast =
      llvh::dyn_cast<ESTree::ImplicitCheckedCastNode>(AE->_left);
  ESTree::Node *left = optImplicitCast ? optImplicitCast->_argument : AE->_left;

  LReference lref = createLRef(left, false);
  Identifier nameHint = extractNameHint(lref);

  auto logicalAssign =
      llvh::StringSwitch<OptValue<LogicalAssignmentOp>>(opStr)
          .Case("||=", LogicalAssignmentOp::ShortCircuitOrKind)
          .Case("&&=", LogicalAssignmentOp::ShortCircuitAndKind)
          .Case("\?\?=", LogicalAssignmentOp::NullishCoalesceKind)
          .Default(llvh::None);
  if (logicalAssign) {
    return genLogicalAssignmentExpr(AE, *logicalAssign, lref, nameHint);
  }

  auto AssignmentKind = BinaryOperatorInst::parseAssignmentOperator(opStr);

  // Section 11.13.1 specifies that we should first load the
  // LHS before materializing the RHS. Unlike in C, this
  // code is well defined: "x+= x++".
  // https://es5.github.io/#x11.13.1
  Value *V = lref.emitLoad();
  V = enforceExprType(V, left);

  Value *RHS = genExpression(AE->_right, nameHint);
  Value *result;
  result = Builder.createBinaryOperatorInst(V, RHS, AssignmentKind);

  // Cast the result of the binary op if it was on the LHS.
  if (optImplicitCast) {
    result = Builder.createCheckedTypeCastInst(
        result,
        flowTypeToIRType(flowContext_.getNodeTypeOrAny(optImplicitCast)));
  }

  result = enforceExprType(result, AE);
  lref.emitStore(result);

  // Return the value that we stored as the result of the expression.
  return result;
}

Value *ESTreeIRGen::genRegExpLiteral(ESTree::RegExpLiteralNode *RE) {
  LLVM_DEBUG(llvh::dbgs() << "IRGen reg exp literal.\n");
  LLVM_DEBUG(
      llvh::dbgs() << "Loading regexp Literal \"" << RE->_pattern->str()
                   << " / " << RE->_flags->str() << "\"\n");
  auto exp = Builder.createRegExpInst(
      Identifier::getFromPointer(RE->_pattern),
      Identifier::getFromPointer(RE->_flags));

  auto &regexp = Builder.getModule()->getContext().getCompiledRegExp(
      RE->_pattern, RE->_flags);

  if (regexp.getMapping().size()) {
    auto &mapping = regexp.getMapping();
    AllocObjectLiteralInst::ObjectPropertyMap propMap;
    for (auto &identifier : regexp.getOrderedGroupNames()) {
      std::string converted;
      convertUTF16ToUTF8WithSingleSurrogates(converted, identifier);
      auto *key = Builder.getLiteralString(converted);
      auto groupIdxRes = mapping.find(identifier);
      assert(
          groupIdxRes != mapping.end() &&
          "identifier not found in named groups");
      auto groupIdx = groupIdxRes->second;
      auto *val = Builder.getLiteralNumber(groupIdx);
      propMap.emplace_back(key, val);
    }
    auto literalObj =
        Builder.createAllocObjectLiteralInst(propMap, Builder.getLiteralNull());

    Value *params[] = {exp, literalObj};
    Builder.createCallBuiltinInst(
        BuiltinMethod::HermesBuiltin_initRegexNamedGroups, params);
  }

  return exp;
}

Value *ESTreeIRGen::genLogicalAssignmentExpr(
    ESTree::AssignmentExpressionNode *AE,
    LogicalAssignmentOp assignmentKind,
    LReference lref,
    Identifier nameHint) {
  // Logical assignment expressions must use short-circuiting logic.
  // BB which actually performs the assignment.
  BasicBlock *assignBB = Builder.createBasicBlock(Builder.getFunction());
  // BB which simply continues without performing the assignment.
  BasicBlock *continueBB = Builder.createBasicBlock(Builder.getFunction());
  auto *lhs = lref.emitLoad();

  PhiInst::ValueListType values;
  PhiInst::BasicBlockListType blocks;

  values.push_back(lhs);
  blocks.push_back(Builder.getInsertionBlock());

  switch (assignmentKind) {
    case LogicalAssignmentOp::ShortCircuitOrKind:
      Builder.createCondBranchInst(lhs, continueBB, assignBB);
      break;
    case LogicalAssignmentOp::ShortCircuitAndKind:
      Builder.createCondBranchInst(lhs, assignBB, continueBB);
      break;
    case LogicalAssignmentOp::NullishCoalesceKind:
      Builder.createCondBranchInst(
          Builder.createBinaryOperatorInst(
              lhs, Builder.getLiteralNull(), ValueKind::BinaryEqualInstKind),
          assignBB,
          continueBB);
      break;
    default:
      llvm_unreachable("invalid AssignmentKind in this branch");
  }

  Builder.setInsertionBlock(assignBB);
  auto *rhs = genExpression(AE->_right, nameHint);
  auto *result = rhs;
  lref.emitStore(result);
  values.push_back(result);
  blocks.push_back(Builder.getInsertionBlock());
  Builder.createBranchInst(continueBB);

  Builder.setInsertionBlock(continueBB);
  // Final result is either the original value or the value assigned,
  // depending on which branch was taken.
  return Builder.createPhiInst(std::move(values), std::move(blocks));
}

Value *ESTreeIRGen::genConditionalExpr(ESTree::ConditionalExpressionNode *C) {
  auto parentFunc = Builder.getInsertionBlock()->getParent();

  PhiInst::ValueListType values;
  PhiInst::BasicBlockListType blocks;

  auto alternateBlock = Builder.createBasicBlock(parentFunc);
  auto consequentBlock = Builder.createBasicBlock(parentFunc);
  auto continueBlock = Builder.createBasicBlock(parentFunc);

  // Implement the ternary operator using control flow. We must use control
  // flow because the expressions may have side effects.
  genExpressionBranch(C->_test, consequentBlock, alternateBlock, nullptr);

  // The 'then' side:
  Builder.setInsertionBlock(consequentBlock);

  values.push_back(genExpression(C->_consequent));
  blocks.push_back(Builder.getInsertionBlock());
  Builder.createBranchInst(continueBlock);

  // The 'else' side:
  Builder.setInsertionBlock(alternateBlock);
  values.push_back(genExpression(C->_alternate));
  blocks.push_back(Builder.getInsertionBlock());
  Builder.createBranchInst(continueBlock);

  // Continue:
  Builder.setInsertionBlock(continueBlock);
  return Builder.createPhiInst(values, blocks);
}

Value *ESTreeIRGen::genIdentifierExpression(
    ESTree::IdentifierNode *Iden,
    bool afterTypeOf) {
  auto StrName = getNameFieldFromID(Iden);
  auto *decl = getIDDecl(Iden);
  auto *Var = getDeclData(decl);

  if (decl->special == sema::Decl::Special::Arguments) {
    // If it is captured, we must use the captured value.
    if (curFunction()->capturedState.arguments) {
      auto *capturedArguments = curFunction()->capturedState.arguments;
      auto *RSI = emitResolveScopeInstIfNeeded(capturedArguments->getParent());
      return Builder.createLoadFrameInst(RSI, capturedArguments);
    }

    return curFunction()->createArgumentsInst;
  }

  // For uses of undefined/Infinity/NaN as the global property, we make an
  // optimization to always return the constant directly.
  if (llvh::isa<GlobalObjectProperty>(Var)) {
    if (StrName.getUnderlyingPointer() == kw_.identUndefined) {
      return Builder.getLiteralUndefined();
    }
    if (StrName.getUnderlyingPointer() == kw_.identInfinity) {
      return Builder.getLiteralInfinity();
    }
    if (StrName.getUnderlyingPointer() == kw_.identNaN) {
      return Builder.getLiteralNaN();
    }
  }

  LLVM_DEBUG(
      llvh::dbgs() << "Found variable " << StrName << " accessed in function \""
                   << curFunction()->function->getInternalNameStr() << "\"\n");

  // Typeof <variable> does not throw.
  return emitLoad(Var, afterTypeOf);
}

Value *ESTreeIRGen::genPrivateNameValue(ESTree::IdentifierNode *Iden) {
  sema::Decl *decl = semCtx_.getExpressionDecl(Iden);
  assert(decl && "identifier must be resolved");
  assert(
      sema::Decl::isKindPrivateName(decl->kind) &&
      "must refer to a private name");
  // Fields each have their own name value.
  if (decl->kind == sema::Decl::Kind::PrivateField) {
    return emitLoad(getDeclData(decl), false);
  }
  // Methods and accessors all share the same class brand symbol as their "name
  // value".
  auto *entry = getDeclDataPrivate<PrivateNameFunctionTable::BaseEntry>(decl);
  return emitLoad(entry->classBrand, false);
}

Value *ESTreeIRGen::genMetaProperty(ESTree::MetaPropertyNode *MP) {
  // Recognize "new.target"
  if (cast<ESTree::IdentifierNode>(MP->_meta)->_name == kw_.identNew) {
    if (cast<ESTree::IdentifierNode>(MP->_property)->_name == kw_.identTarget) {
      return genNewTarget();
    }
  }

  llvm_unreachable("invalid MetaProperty");
}

Value *ESTreeIRGen::genNewTarget() {
  Value *value;

  switch (curFunction()->function->getDefinitionKind()) {
    case Function::DefinitionKind::ES5Function:
    case Function::DefinitionKind::ES6BaseConstructor:
    case Function::DefinitionKind::ES6DerivedConstructor:
      value = Builder.createGetNewTargetInst(
          curFunction()->function->getNewTargetParam());
      break;
    case Function::DefinitionKind::GeneratorInnerArrow:
    case Function::DefinitionKind::ES6Arrow:
      value = curFunction()->capturedState.newTarget;
      break;
    // Generators cannot be invoked with new, so new.target will be
    // undefined.
    case Function::DefinitionKind::GeneratorInner:
    case Function::DefinitionKind::ES6Method:
      value = Builder.getLiteralUndefined();
      break;
  }

  // If it is a variable, we must issue a load.
  if (auto *V = llvh::dyn_cast<Variable>(value)) {
    auto *RSI = emitResolveScopeInstIfNeeded(V->getParent());
    return Builder.createLoadFrameInst(RSI, V);
  }

  return value;
}

Value *ESTreeIRGen::genNewExpr(ESTree::NewExpressionNode *N) {
  LLVM_DEBUG(llvh::dbgs() << "IRGen 'new' statement/expression.\n");

  Value *callee = genExpression(N->_callee);

  bool canEmitTypedArgs = canEmitTypedCallArguments(N->_arguments);

  // Is this a statically typed new?
  if (auto *consType = llvh::dyn_cast<flow::ClassConstructorType>(
          flowContext_.getNodeTypeOrAny(N->_callee)->info)) {
    flow::ClassType *classType = consType->getClassTypeInfo();
    assert(
        canEmitTypedArgs &&
        "unexpandable spread should have been rejected by the typechecker");

    // If we know the ClassType but it hasn't been populated in
    // classConstructors_ yet, then it must occur later in the same function.
    // If it was created in a surrounding function and captured, the function
    // queueing mechanism would result in compiling this CallInst after
    // it anyway.
    // This is an error, since we can statically determine that the constructor
    // will not have been populated.
    // TODO: This error should be raised earlier, when the callee is loaded.
    auto it = classConstructors_.find(classType);
    if (it == classConstructors_.end()) {
      Builder.getModule()->getContext().getSourceErrorManager().error(
          N->_callee->getSourceRange(),
          Twine("Cannot construct class before its definition."));
      return Builder.getLiteralUndefined();
    }

    // Since the callee has already been loaded at this point, we know that the
    // constructor has already beenn created and initialized. This means that we
    // do not need to perform any TDZ check on the home object variable.
    auto *RSI =
        emitResolveScopeInstIfNeeded(it->second.homeObjectVar->getParent());
    auto *proto = Builder.createUnionNarrowTrustedInst(
        Builder.createLoadFrameInst(RSI, it->second.homeObjectVar),
        Type::createObject());
    Value *newInst = emitTypedClassAllocation(
        classType, proto, /* propertiesEnumerable */ true);

    // Call the constructor, if necessary. There is always a constructor,
    // either explicit or implicit. We must invoke it if it has an explicit
    // type, performs field initializations, or has a super class to forward
    // to. An implicit constructor takes no arguments, so there is nothing to
    // evaluate when the call is skipped.
    if (classType->getConstructorType() || classType->getSuperClass() ||
        (classFieldInitInfo_.find(classType) != classFieldInitInfo_.end())) {
      CallInst::ArgumentList args;
      emitTypedCallArguments(N->_arguments, /* offset */ 0, args);

      Function *target = it->second.constructorFunc;
      Builder.createCallInst(
          callee,
          target,
          /* calleeIsAlwaysClosure */ true,
          Builder.getEmptySentinel(),
          callee,
          newInst,
          args);
    }
    return newInst;
  }

  if (canEmitTypedArgs) {
    CallInst::ArgumentList args;
    emitTypedCallArguments(N->_arguments, /* offset */ 0, args);
    auto *thisArg = Builder.createCreateThisInst(callee, callee);
    auto *res = Builder.createCallInst(callee, callee, thisArg, args);
    return Builder.createGetConstructedObjectInst(thisArg, res);
  }

  // Otherwise, there exists a spread argument that cannot be expanded
  // statically, so the number of arguments is variable.
  // Generate IR for this by creating an array and populating it with the
  // arguments, then calling HermesInternal.apply.
  auto *args = genArrayFromElements(N->_arguments);

  return genBuiltinCall(BuiltinMethod::HermesBuiltin_apply, {callee, args});
}

Value *ESTreeIRGen::genLogicalExpression(
    ESTree::LogicalExpressionNode *logical) {
  auto opStr = logical->_operator->str();
  LLVM_DEBUG(llvh::dbgs() << "IRGen of short circuiting: " << opStr << ".\n");

  enum class Kind {
    And, // &&
    Or, // ||
    Coalesce, // ??
  };

  Kind kind;

  if (opStr == "&&") {
    kind = Kind::And;
  } else if (opStr == "||") {
    kind = Kind::Or;
  } else if (opStr == "??") {
    kind = Kind::Coalesce;
  } else {
    llvm_unreachable("Invalid update operator");
  }

  // Generate a new temporary stack allocation.
  auto tempVarName = genAnonymousLabelName("logical");
  auto parentFunc = Builder.getInsertionBlock()->getParent();
  auto tempVar =
      Builder.createAllocStackInst(tempVarName, Type::createAnyType());

  auto evalRHSBlock = Builder.createBasicBlock(parentFunc);
  auto continueBlock = Builder.createBasicBlock(parentFunc);

  auto LHS = genExpression(logical->_left);

  // Store the LHS value of the expression in preparation for the case where we
  // won't need to evaluate the RHS side of the expression. In that case, we
  // jump to continueBlock, which returns tempVar.
  Builder.createStoreStackInst(LHS, tempVar);

  // Notice that instead of negating the condition we swap the operands of the
  // branch.
  switch (kind) {
    case Kind::And:
      // Evaluate RHS only when the LHS is true.
      Builder.createCondBranchInst(LHS, evalRHSBlock, continueBlock);
      break;
    case Kind::Or:
      // Evaluate RHS only when the LHS is false.
      Builder.createCondBranchInst(LHS, continueBlock, evalRHSBlock);
      break;
    case Kind::Coalesce:
      // Evaluate RHS only if the value is undefined or null.
      // Use == instead of === to account for both values at once.
      Builder.createCondBranchInst(
          Builder.createBinaryOperatorInst(
              LHS, Builder.getLiteralNull(), ValueKind::BinaryEqualInstKind),
          evalRHSBlock,
          continueBlock);

      break;
  }

  // Continue the evaluation of the right-hand-side of the expression.
  Builder.setInsertionBlock(evalRHSBlock);
  auto RHS = genExpression(logical->_right);

  // Evaluate the RHS and store the result into the temporary variable.
  Builder.createStoreStackInst(RHS, tempVar);

  // Finally, jump to the continuation block.
  Builder.createBranchInst(continueBlock);

  // Load the content of the temp variable that was set in one of the branches.
  Builder.setInsertionBlock(continueBlock);
  return Builder.createLoadStackInst(tempVar);
}

Value *ESTreeIRGen::genThisExpression() {
  // Accessing `this` in derived legacy class constructors must be guarded.
  if (curFunction()->hasLegacyClassContext()) {
    if (semCtx_.nearestNonArrow(curFunction()->getSemInfo())->constructorKind ==
        sema::FunctionInfo::ConstructorKind::Derived) {
      return genLegacyDerivedThis();
    }
  }
  // Generators may be used as arrows in async arrow functions, in which case
  // they should load from the captured this.
  // Untyped base class constructors create `this` internally, so they also have
  // to access `this` as a Variable.
  auto funcDefKind = curFunction()->function->getDefinitionKind();
  if ((funcDefKind == Function::DefinitionKind::ES6Arrow) ||
      (funcDefKind == Function::DefinitionKind::GeneratorInnerArrow) ||
      (curFunction()->hasLegacyClassContext() &&
       funcDefKind == Function::DefinitionKind::ES6BaseConstructor)) {
    assert(
        curFunction()->capturedState.thisVal &&
        "arrow function must have a captured this");
    auto *RSI = emitResolveScopeInstIfNeeded(
        curFunction()->capturedState.thisVal->getParent());
    return Builder.createLoadFrameInst(
        RSI, curFunction()->capturedState.thisVal);
  }
  return curFunction()->jsParams[0];
}

void ESTreeIRGen::genLogicalExpressionBranch(
    ESTree::LogicalExpressionNode *logical,
    BasicBlock *onTrue,
    BasicBlock *onFalse,
    BasicBlock *onNullish) {
  auto opStr = logical->_operator->str();
  LLVM_DEBUG(
      llvh::dbgs() << "IRGen of short circuiting: " << opStr << " branch.\n");

  auto parentFunc = Builder.getInsertionBlock()->getParent();
  auto *block = Builder.createBasicBlock(parentFunc);

  if (opStr == "&&") {
    genExpressionBranch(logical->_left, block, onFalse, onNullish);
  } else if (opStr == "||") {
    genExpressionBranch(logical->_left, onTrue, block, onNullish);
  } else {
    assert(opStr == "??" && "invalid logical operator");
    genExpressionBranch(logical->_left, onTrue, onFalse, block);
  }

  Builder.setInsertionBlock(block);
  genExpressionBranch(logical->_right, onTrue, onFalse, onNullish);
}

Value *ESTreeIRGen::genTemplateLiteralExpr(ESTree::TemplateLiteralNode *Expr) {
  LLVM_DEBUG(llvh::dbgs() << "IRGen 'TemplateLiteral' expression.\n");

  assert(
      Expr->_quasis.size() == Expr->_expressions.size() + 1 &&
      "The string count should always be one more than substitution count.");

  // Construct an argument list for calling HermesInternal.concat():
  // cookedStr0, substitution0, cookedStr1, ..., substitutionN, cookedStrN + 1,
  // skipping any empty string, except for the first cooked string, which is
  // going to be the `this` to the concat call.

  // Get the first cooked string.
  auto strItr = Expr->_quasis.begin();
  auto *tempEltNode = cast<ESTree::TemplateElementNode>(&*strItr);
  auto *firstCookedStr = Builder.getLiteralString(tempEltNode->_cooked->str());
  ++strItr;
  // If the template literal is effectively only one string, directly return it.
  if (strItr == Expr->_quasis.end()) {
    return firstCookedStr;
  }
  // argList includes firstCookedStr as the first element.
  CallInst::ArgumentList argList;
  argList.push_back(firstCookedStr);
  // Whether all the expressions are strings.
  // Determines if we can use StringConcatInst.
  bool allStrings = true;
  auto exprItr = Expr->_expressions.begin();
  while (strItr != Expr->_quasis.end()) {
    auto *sub = genExpression(&*exprItr);
    if (!sub->getType().isStringType()) {
      // Uses the IR type instead of the FlowContext type because
      // StringConcat needs to know that its operands are String types in the
      // IR, and not all values have their types correctly brought over from the
      // annotated AST.
      allStrings = false;
    }
    argList.push_back(sub);
    tempEltNode = cast<ESTree::TemplateElementNode>(&*strItr);
    auto cookedStr = tempEltNode->_cooked->str();
    if (!cookedStr.empty()) {
      argList.push_back(Builder.getLiteralString(cookedStr));
    }
    ++strItr;
    ++exprItr;
  }
  assert(
      exprItr == Expr->_expressions.end() &&
      "All the substitutions must have been collected.");

  if (allStrings) {
    // Fast path: all the substitutions are strings, so we can use
    // StringConcatInst.
    return Builder.createStringConcatInst(argList);
  } else {
    // Generate a function call to HermesInternal.concat() with these arguments.
    return genHermesInternalCall(
        "concat", argList[0], llvh::ArrayRef<Value *>(argList).drop_front(1));
  }
}

Value *ESTreeIRGen::genTaggedTemplateExpr(
    ESTree::TaggedTemplateExpressionNode *Expr) {
  LLVM_DEBUG(llvh::dbgs() << "IRGen 'TaggedTemplateExpression' expression.\n");
  // Step 1: get the template object.
  auto *templateLit = cast<ESTree::TemplateLiteralNode>(Expr->_quasi);

  // Empty if same as rawStringArgs.
  llvh::SmallVector<Value *, 2> cookedStrings{};

  Module::RawStringList rawStringsForMap;
  // dup is true if the cooked strings and raw strings are duplicated.
  bool dup = true;
  for (auto &n : templateLit->_quasis) {
    auto element = cast<ESTree::TemplateElementNode>(&n);
    if (element->_cooked != element->_raw) {
      dup = false;
    }
    rawStringsForMap.push_back(Builder.getLiteralString(element->_raw->str()));
  }

  // Retrieve template object ID.
  // rawStringsForMap is moved, rawStrings is a reference to the same array now
  // that it lives in the map.
  auto [rawStrings, templateObjID] =
      Mod->emplaceTemplateObject(std::move(rawStringsForMap));

  // If the cooked strings are not the same as raw strings, append them to
  // argument list.
  if (!dup) {
    for (auto &node : templateLit->_quasis) {
      auto *templateElt = cast<ESTree::TemplateElementNode>(&node);
      if (templateElt->_cooked) {
        cookedStrings.push_back(
            Builder.getLiteralString(templateElt->_cooked->str()));
      } else {
        cookedStrings.push_back(Builder.getLiteralUndefined());
      }
    }
  }

  auto *templateObj = Builder.createGetTemplateObjectInst(
      templateObjID, dup, rawStrings, cookedStrings);

  // Step 2: call the tag function, passing the template object followed by a
  // list of substitutions as arguments.
  CallInst::ArgumentList tagFuncArgList;
  tagFuncArgList.push_back(templateObj);
  for (auto &sub : templateLit->_expressions) {
    tagFuncArgList.push_back(genExpression(&sub));
  }

  Value *callee;
  Value *thisVal;
  // Tag function is a member expression.
  if (auto *Mem = llvh::dyn_cast<ESTree::MemberExpressionNode>(Expr->_tag)) {
    Value *obj = genExpression(Mem->_object);
    Value *prop = genMemberExpressionProperty(Mem);
    // Call the callee with obj as the 'this'.
    thisVal = obj;
    callee = Builder.createLoadPropertyInst(obj, prop);
  } else {
    thisVal = Builder.getLiteralUndefined();
    callee = genExpression(Expr->_tag);
  }

  return Builder.createCallInst(
      callee,
      /* newTarget */ Builder.getLiteralUndefined(),
      thisVal,
      tagFuncArgList);
}

} // namespace irgen
} // namespace hermes
