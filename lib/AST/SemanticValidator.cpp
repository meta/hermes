/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SemanticValidator.h" // @manual

#include "hermes/AST/ESTree.h"
#include "hermes/Regex/RegexSerialization.h"

#include "llvh/ADT/ScopeExit.h"
#include "llvh/ADT/SmallSet.h"
#include "llvh/Support/SaveAndRestore.h"

using llvh::cast_or_null;
using llvh::dyn_cast;
using llvh::isa;
using llvh::SaveAndRestore;

namespace hermes {
namespace sem {

//===----------------------------------------------------------------------===//
// Keywords

Keywords::Keywords(Context &astContext)
    : identArguments(
          astContext.getIdentifier("arguments").getUnderlyingPointer()),
      identEval(astContext.getIdentifier("eval").getUnderlyingPointer()),
      identDelete(astContext.getIdentifier("delete").getUnderlyingPointer()),
      identThis(astContext.getIdentifier("this").getUnderlyingPointer()),
      identUseStrict(
          astContext.getIdentifier("use strict").getUnderlyingPointer()),
      identShowSource(
          astContext.getIdentifier("show source").getUnderlyingPointer()),
      identHideSource(
          astContext.getIdentifier("hide source").getUnderlyingPointer()),
      identSensitive(
          astContext.getIdentifier("sensitive").getUnderlyingPointer()),
      identVar(astContext.getIdentifier("var").getUnderlyingPointer()),
      identLet(astContext.getIdentifier("let").getUnderlyingPointer()),
      identConst(astContext.getIdentifier("const").getUnderlyingPointer()),
      identPlus(astContext.getIdentifier("+").getUnderlyingPointer()),
      identMinus(astContext.getIdentifier("-").getUnderlyingPointer()),
      identAssign(astContext.getIdentifier("=").getUnderlyingPointer()) {}

//===----------------------------------------------------------------------===//
// SemanticValidator

SemanticValidator::SemanticValidator(
    Context &astContext,
    sem::SemContext &semCtx,
    bool compile)
    : astContext_(astContext),
      sm_(astContext.getSourceErrorManager()),
      bufferMessages_{&sm_},
      semCtx_(semCtx),
      initialErrorCount_(sm_.getErrorCount()),
      kw_(astContext),
      compile_(compile) {}

bool SemanticValidator::doIt(Node *rootNode) {
  visitESTreeNode(*this, rootNode);
  return sm_.getErrorCount() == initialErrorCount_;
}

bool SemanticValidator::doFunction(Node *function, bool strict) {
  // Create a wrapper context since a function always assumes there is an
  // existing context.
  FunctionContext wrapperContext(this, strict, nullptr, nullptr);

  assert(
      wrapperContext.scopedClosures == nullptr &&
      "current context doesnt have a body, so it shouldn't have closures.");

  // Create a dummy closures array that will contain the closure for \p
  // function.
  FunctionInfo::BlockClosures dummyClosures;
  wrapperContext.scopedClosures = &dummyClosures;
  FunctionInfo::BlockDecls dummyDecls;
  wrapperContext.scopedDecls = &dummyDecls;

  visitESTreeNode(*this, function);
  return sm_.getErrorCount() == initialErrorCount_;
}

void SemanticValidator::visit(ProgramNode *node) {
  FunctionContext newFuncCtx{this, astContext_.isStrictMode(), node, node};

  scanDirectivePrologue(node->_body);
  setDirectiveDerivedInfo(node);

  visitESTreeChildren(*this, node);
}

void SemanticValidator::visit(VariableDeclaratorNode *varDecl, Node *parent) {
  auto *declaration = cast<VariableDeclarationNode>(parent);

  FunctionInfo::VarDecl::Kind declKind;
  if (declaration->_kind == kw_.identLet)
    declKind = FunctionInfo::VarDecl::Kind::Let;
  else if (declaration->_kind == kw_.identConst)
    declKind = FunctionInfo::VarDecl::Kind::Const;
  else {
    assert(declaration->_kind == kw_.identVar);
    declKind = FunctionInfo::VarDecl::Kind::Var;
  }

  validateDeclarationNames(
      declKind,
      varDecl->_id,
      curFunction()->varDecls,
      curFunction()->scopedDecls);
  visitESTreeChildren(*this, varDecl);
}

void SemanticValidator::visit(MetaPropertyNode *metaProp) {
  auto *meta = cast<IdentifierNode>(metaProp->_meta);
  auto *property = cast<IdentifierNode>(metaProp->_property);

  if (meta->_name->str() == "new" && property->_name->str() == "target") {
    if (curFunction()->isGlobalScope()) {
      // ES9.0 15.1.1:
      // It is a Syntax Error if StatementList Contains NewTarget unless the
      // source code containing NewTarget is eval code that is being processed
      // by a direct eval.
      // Hermes does not support local eval, so we assume that this is not
      // inside a local eval call.
      sm_.error(metaProp->getSourceRange(), "'new.target' not in a function");
    }
    return;
  }

  if (meta->_name->str() == "import" && property->_name->str() == "meta") {
    if (compile_) {
      sm_.error(
          metaProp->getSourceRange(), "'import.meta' is currently unsupported");
    }
    return;
  }

  sm_.error(
      metaProp->getSourceRange(),
      "invalid meta property " + meta->_name->str() + "." +
          property->_name->str());
}

void SemanticValidator::visit(IdentifierNode *identifier) {
  if (identifier->_name == kw_.identEval && !astContext_.getEnableEval())
    sm_.error(identifier->getSourceRange(), "'eval' is disabled");

  if (identifier->_name == kw_.identArguments) {
    if (forbidArguments_)
      sm_.error(identifier->getSourceRange(), "invalid use of 'arguments'");
    curFunction()->semInfo->usesArguments = true;
  }
}

/// Process a function declaration by creating a new FunctionContext.
void SemanticValidator::visit(FunctionDeclarationNode *funcDecl) {
  curFunction()->addHoistingCandidate(funcDecl);
  visitFunction(funcDecl, funcDecl->_id, funcDecl->_params, funcDecl->_body);
}

/// Process a function expression by creating a new FunctionContext.
void SemanticValidator::visit(FunctionExpressionNode *funcExpr) {
  visitFunction(funcExpr, funcExpr->_id, funcExpr->_params, funcExpr->_body);
}

void SemanticValidator::visit(ArrowFunctionExpressionNode *arrowFunc) {
  // Convert expression functions to a full-body to simplify IRGen.
  if (compile_ && arrowFunc->_expression) {
    auto *retStmt = new (astContext_) ReturnStatementNode(arrowFunc->_body);
    retStmt->copyLocationFrom(arrowFunc->_body);

    ESTree::NodeList stmtList;
    stmtList.push_back(*retStmt);

    auto *blockStmt = new (astContext_) BlockStatementNode(std::move(stmtList));
    blockStmt->copyLocationFrom(arrowFunc->_body);

    arrowFunc->_body = blockStmt;
    arrowFunc->_expression = false;
  }

  visitFunction(arrowFunc, nullptr, arrowFunc->_params, arrowFunc->_body);

  curFunction()->semInfo->containsArrowFunctions = true;
  curFunction()->semInfo->containsArrowFunctionsUsingArguments =
      curFunction()->semInfo->containsArrowFunctionsUsingArguments ||
      arrowFunc->getSemInfo()->containsArrowFunctionsUsingArguments ||
      arrowFunc->getSemInfo()->usesArguments;
}

#if HERMES_PARSE_FLOW
/// Process a component declaration by creating a new FunctionContext.
void SemanticValidator::visit(ComponentDeclarationNode *componentDecl) {
  visitFunction(
      componentDecl,
      componentDecl->_id,
      componentDecl->_params,
      componentDecl->_body);
}
/// Process a hook declaration by creating a new FunctionContext.
void SemanticValidator::visit(HookDeclarationNode *hookDecl) {
  visitFunction(hookDecl, hookDecl->_id, hookDecl->_params, hookDecl->_body);
}
#endif

/// Ensure that the left side of for-in is an l-value.
void SemanticValidator::visit(ForInStatementNode *forIn) {
  visitForInOf(forIn, forIn->_left);
}
void SemanticValidator::visit(ForOfStatementNode *forOf) {
  if (compile_ && forOf->_await)
    sm_.error(
        forOf->getSourceRange(),
        "for await..of loops are currently unsupported");

  visitForInOf(forOf, forOf->_left);
}

void SemanticValidator::visitForInOf(LoopStatementNode *loopNode, Node *left) {
  loopNode->setLabelIndex(curFunction()->allocateLabel());

  SaveAndRestore<LoopStatementNode *> saveLoop(
      curFunction()->activeLoop, loopNode);
  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, loopNode);

  if (auto *VD = dyn_cast<VariableDeclarationNode>(left)) {
    assert(
        VD->_declarations.size() == 1 &&
        "for-in/for-of must have a single binding");

    auto *declarator =
        cast<ESTree::VariableDeclaratorNode>(&VD->_declarations.front());

    if (declarator->_init) {
      if (isa<ESTree::PatternNode>(declarator->_id)) {
        sm_.error(
            declarator->_init->getSourceRange(),
            "destructuring declaration cannot be initialized in for-in/for-of loop");
      } else if (!(isa<ForInStatementNode>(loopNode) &&
                   !curFunction()->strictMode && VD->_kind == kw_.identVar)) {
        sm_.error(
            declarator->_init->getSourceRange(),
            "for-in/for-of variable declaration may not be initialized");
      }
    }
  } else {
    validateAssignmentTarget(left);
  }
  visitESTreeChildren(*this, loopNode);
}

void SemanticValidator::visit(BinaryExpressionNode *bin) {
  // Handle nested +/- non-recursively.
  if (bin->_operator == kw_.identPlus || bin->_operator == kw_.identMinus) {
    auto list = linearizeLeft(bin, {"+", "-"});
    if (list.size() > MAX_NESTED_BINARY) {
      recursionDepthExceeded(bin);
      return;
    }

    visitESTreeNode(*this, list[0]->_left, list[0]);
    for (auto *e : list) {
      visitESTreeNode(*this, e->_right, e);
    }
    return;
  }

  visitESTreeChildren(*this, bin);
}

/// Ensure that the left side of assgnments is an l-value.
void SemanticValidator::visit(AssignmentExpressionNode *assignment) {
  // Handle nested "=" non-recursively.
  if (assignment->_operator == kw_.identAssign) {
    auto list = linearizeRight(assignment, {"="});
    if (list.size() > MAX_NESTED_ASSIGNMENTS) {
      recursionDepthExceeded(assignment);
      return;
    }

    for (auto *e : list) {
      validateAssignmentTarget(e->_left);
      visitESTreeNode(*this, e->_left, e);
    }
    visitESTreeNode(*this, list.back()->_right, list.back());
    return;
  }

  validateAssignmentTarget(assignment->_left);
  visitESTreeChildren(*this, assignment);
}

/// Ensure that the operand of ++/-- is an l-value.
void SemanticValidator::visit(UpdateExpressionNode *update) {
  // Check if the left-hand side is valid.
  if (!isLValue(update->_argument)) {
    sm_.error(
        update->_argument->getSourceRange(),
        "invalid operand in update operation");
  }
  visitESTreeChildren(*this, update);
}

/// Declare named labels, checking for duplicates, etc.
void SemanticValidator::visit(LabeledStatementNode *labelStmt) {
  auto id = cast<IdentifierNode>(labelStmt->_label);

  labelStmt->setLabelIndex(curFunction()->allocateLabel());

  // Determine the target statement. We need to check if it directly encloses
  // a loop or another label enclosing a loop.
  StatementNode *targetStmt = labelStmt;
  {
    LabeledStatementNode *curStmt = labelStmt;
    do {
      if (auto *LS = dyn_cast<LoopStatementNode>(curStmt->_body)) {
        targetStmt = LS;
        break;
      }
    } while ((curStmt = dyn_cast<LabeledStatementNode>(curStmt->_body)));
  }

  // Define the new label, checking for a previous definition.
  auto insertRes =
      curFunction()->labelMap.insert({id->_name, {id, targetStmt}});
  if (!insertRes.second) {
    sm_.error(
        id->getSourceRange(),
        llvh::Twine("label '") + id->_name->str() + "' is already defined");
    sm_.note(
        insertRes.first->second.declarationNode->getSourceRange(),
        "previous definition");
  }
  // Auto-erase the label on exit, if we inserted it.
  const auto &deleter = llvh::make_scope_exit(
      [this, inserted = insertRes.second, name = id->_name]() {
        if (inserted)
          curFunction()->labelMap.erase(name);
      });
  (void)deleter;

  visitESTreeChildren(*this, labelStmt);
}

/// Check RegExp syntax.
void SemanticValidator::visit(RegExpLiteralNode *regexp) {
  llvh::StringRef regexpError;
  if (compile_) {
    if (auto compiled = CompiledRegExp::tryCompile(
            regexp->_pattern->str(), regexp->_flags->str(), &regexpError)) {
      astContext_.addCompiledRegExp(
          regexp->_pattern, regexp->_flags, std::move(*compiled));
    } else {
      sm_.error(
          regexp->getSourceRange(),
          "Invalid regular expression: " + Twine(regexpError));
    }
  }
  visitESTreeChildren(*this, regexp);
}

void SemanticValidator::validateCatchClause(const Node *catchClause) {
  // The catch clause is optional, so bail early if it doesn't exist.
  if (!catchClause) {
    return;
  }
  auto *castedCatch = llvh::dyn_cast<ESTree::CatchClauseNode>(catchClause);
  if (!castedCatch) {
    return;
  }
  // Bail early if there is no identifier in the parameter of the catch.
  if (!castedCatch->_param ||
      !llvh::isa<ESTree::IdentifierNode>(castedCatch->_param)) {
    return;
  }
  auto *idNode = cast<ESTree::IdentifierNode>(castedCatch->_param);
  if (!isValidDeclarationName(idNode)) {
    sm_.error(
        idNode->getSourceRange(),
        "cannot bind to " + idNode->_name->str() +
            " in the catch clause in strict mode");
  }
}

void SemanticValidator::visit(TryStatementNode *tryStatement) {
  if (curFunction()->strictMode) {
    validateCatchClause(tryStatement->_handler);
  }
  // The catch parameter cannot bind to 'eval' or 'arguments' in strict mode.
  // A try statement with both catch and finally handlers is technically
  // two nested try statements. Transform:
  //
  //    try {
  //      tryBody;
  //    } catch {
  //      catchBody;
  //    } finally {
  //      finallyBody;
  //    }
  //
  // into
  //
  //    try {
  //      try {
  //        tryBody;
  //      } catch {
  //        catchBody;
  //      }
  //    } finally {
  //      finallyBody;
  //    }
  if (compile_ && tryStatement->_handler && tryStatement->_finalizer) {
    auto *nestedTry = new (astContext_)
        TryStatementNode(tryStatement->_block, tryStatement->_handler, nullptr);
    nestedTry->copyLocationFrom(tryStatement);
    nestedTry->setEndLoc(nestedTry->_handler->getEndLoc());

    ESTree::NodeList stmtList;
    stmtList.push_back(*nestedTry);
    tryStatement->_block =
        new (astContext_) BlockStatementNode(std::move(stmtList));
    tryStatement->_block->copyLocationFrom(nestedTry);
    tryStatement->_handler = nullptr;
  }

  visitESTreeNode(*this, tryStatement->_block, tryStatement);
  if (!blockScopingEnabled()) {
    visitESTreeNode(*this, tryStatement->_handler, tryStatement);
  } else {
    visitTryHandler(tryStatement);
  }

  visitESTreeNode(*this, tryStatement->_finalizer, tryStatement);
}

void SemanticValidator::visitTryHandler(TryStatementNode *tryStatement) {
  if (auto *handler =
          llvh::dyn_cast_or_null<CatchClauseNode>(tryStatement->_handler)) {
    auto *param = llvh::dyn_cast_or_null<IdentifierNode>(handler->_param);

    BlockContext blockScope(this, curFunction(), handler);

    if (auto *block = llvh::dyn_cast<BlockStatementNode>(handler->_body)) {
      for (auto &stmt : block->_body) {
        visitESTreeNode(*this, &stmt, block);
      }
    } else {
      visitESTreeNode(*this, tryStatement->_handler, tryStatement);
    }

    blockScope.ensureScopedNamesAreUnique(
        BlockContext::IsFunctionBody::No, param);

    // Delay adding the catch param until now to prevent Syntax Errors if the
    // handler has a var that with the same ID as the catch param (as specified
    // in ES2023 B.3.4).
    validateDeclarationNames(
        FunctionInfo::VarDecl::Kind::Let,
        param,
        curFunction()->varDecls,
        curFunction()->scopedDecls);
  }
}

void SemanticValidator::visit(BlockStatementNode *block) {
  BlockContext blockScope(this, curFunction(), block);
  visitESTreeChildren(*this, block);

  blockScope.ensureScopedNamesAreUnique(BlockContext::IsFunctionBody::No);
}

void SemanticValidator::visit(DoWhileStatementNode *loop) {
  loop->setLabelIndex(curFunction()->allocateLabel());

  SaveAndRestore<LoopStatementNode *> saveLoop(curFunction()->activeLoop, loop);
  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, loop);

  visitESTreeChildren(*this, loop);
}
void SemanticValidator::visit(ForStatementNode *loop) {
  loop->setLabelIndex(curFunction()->allocateLabel());

  SaveAndRestore<LoopStatementNode *> saveLoop(curFunction()->activeLoop, loop);
  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, loop);

  visitESTreeChildren(*this, loop);
}
void SemanticValidator::visit(WhileStatementNode *loop) {
  loop->setLabelIndex(curFunction()->allocateLabel());

  SaveAndRestore<LoopStatementNode *> saveLoop(curFunction()->activeLoop, loop);
  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, loop);

  visitESTreeChildren(*this, loop);
}
void SemanticValidator::visit(SwitchStatementNode *switchStmt) {
  switchStmt->setLabelIndex(curFunction()->allocateLabel());

  BlockContext switchContext(this, curFunction(), switchStmt);

  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, switchStmt);

  visitESTreeChildren(*this, switchStmt);

  switchContext.ensureScopedNamesAreUnique(BlockContext::IsFunctionBody::No);
}

void SemanticValidator::visit(BreakStatementNode *breakStmt) {
  if (auto id = cast_or_null<IdentifierNode>(breakStmt->_label)) {
    // A labeled break.
    // Find the label in the label map.
    auto labelIt = curFunction()->labelMap.find(id->_name);
    if (labelIt != curFunction()->labelMap.end()) {
      auto labelIndex = getLabelDecorationBase(labelIt->second.targetStatement)
                            ->getLabelIndex();
      breakStmt->setLabelIndex(labelIndex);
    } else {
      sm_.error(
          id->getSourceRange(),
          Twine("label '") + id->_name->str() + "' is not defined");
    }
  } else {
    // Anonymous break.
    if (curFunction()->activeSwitchOrLoop) {
      auto labelIndex =
          getLabelDecorationBase(curFunction()->activeSwitchOrLoop)
              ->getLabelIndex();
      breakStmt->setLabelIndex(labelIndex);
    } else {
      sm_.error(
          breakStmt->getSourceRange(), "'break' not within a loop or a switch");
    }
  }

  visitESTreeChildren(*this, breakStmt);
}

void SemanticValidator::visit(ContinueStatementNode *continueStmt) {
  if (auto id = cast_or_null<IdentifierNode>(continueStmt->_label)) {
    // A labeled continue.
    // Find the label in the label map.
    auto labelIt = curFunction()->labelMap.find(id->_name);
    if (labelIt != curFunction()->labelMap.end()) {
      if (isa<LoopStatementNode>(labelIt->second.targetStatement)) {
        auto labelIndex =
            getLabelDecorationBase(labelIt->second.targetStatement)
                ->getLabelIndex();
        continueStmt->setLabelIndex(labelIndex);
      } else {
        sm_.error(
            id->getSourceRange(),
            llvh::Twine("continue label '") + id->_name->str() +
                "' is not a loop label");
        sm_.note(
            labelIt->second.declarationNode->getSourceRange(),
            "label defined here");
      }
    } else {
      sm_.error(
          id->getSourceRange(),
          Twine("label '") + id->_name->str() + "' is not defined");
    }
  } else {
    // Anonymous continue.
    if (curFunction()->activeLoop) {
      auto labelIndex = curFunction()->activeLoop->getLabelIndex();
      continueStmt->setLabelIndex(labelIndex);
    } else {
      sm_.error(continueStmt->getSourceRange(), "'continue' not within a loop");
    }
  }
  visitESTreeChildren(*this, continueStmt);
}

void SemanticValidator::visit(ReturnStatementNode *returnStmt) {
  if (curFunction()->isGlobalScope() &&
      !astContext_.allowReturnOutsideFunction())
    sm_.error(returnStmt->getSourceRange(), "'return' not in a function");
  visitESTreeChildren(*this, returnStmt);
}

void SemanticValidator::visit(YieldExpressionNode *yieldExpr) {
  if (curFunction()->isGlobalScope() ||
      (curFunction()->node && !ESTree::isGenerator(curFunction()->node)))
    sm_.error(
        yieldExpr->getSourceRange(), "'yield' not in a generator function");

  if (isFormalParams_) {
    // For generators functions (the only time YieldExpression is parsed):
    // It is a Syntax Error if UniqueFormalParameters Contains YieldExpression
    // is true.
    sm_.error(
        yieldExpr->getSourceRange(),
        "'yield' not allowed in a formal parameter");
  }

  visitESTreeChildren(*this, yieldExpr);
}

void SemanticValidator::visit(AwaitExpressionNode *awaitExpr) {
  if (forbidAwaitExpression_)
    sm_.error(awaitExpr->getSourceRange(), "'await' not in an async function");

  visitESTreeChildren(*this, awaitExpr);
}

void SemanticValidator::visit(UnaryExpressionNode *unaryExpr) {
  // Check for unqualified delete in strict mode.
  if (unaryExpr->_operator == kw_.identDelete) {
    if (curFunction()->strictMode &&
        isa<IdentifierNode>(unaryExpr->_argument)) {
      sm_.error(
          unaryExpr->getSourceRange(),
          "'delete' of a variable is not allowed in strict mode");
    }
  }
  visitESTreeChildren(*this, unaryExpr);
}

void SemanticValidator::visit(ArrayPatternNode *AP) {
  visitESTreeChildren(*this, AP);
}

void SemanticValidator::visit(SpreadElementNode *S, Node *parent) {
  if (!isa<ESTree::ObjectExpressionNode>(parent) &&
      !isa<ESTree::ArrayExpressionNode>(parent) &&
      !isa<ESTree::CallExpressionNode>(parent) &&
      !isa<ESTree::OptionalCallExpressionNode>(parent) &&
      !isa<ESTree::NewExpressionNode>(parent))
    sm_.error(S->getSourceRange(), "spread operator is not supported");
  visitESTreeChildren(*this, S);
}

void SemanticValidator::visit(ClassExpressionNode *node) {
  SaveAndRestore<bool> oldStrictMode{curFunction()->strictMode, true};
  visitESTreeChildren(*this, node);
}

void SemanticValidator::visit(ClassDeclarationNode *node) {
  SaveAndRestore<bool> oldStrictMode{curFunction()->strictMode, true};
  visitESTreeChildren(*this, node);
}

void SemanticValidator::visit(PrivateNameNode *node) {
  if (compile_)
    sm_.error(node->getSourceRange(), "private properties are not supported");
  visitESTreeChildren(*this, node);
}

void SemanticValidator::visit(ClassPrivatePropertyNode *node) {
  if (compile_)
    sm_.error(node->getSourceRange(), "private properties are not supported");
  visitESTreeNode(*this, node->_key);
  {
    SaveAndRestore<bool> oldForbidAwait{forbidAwaitExpression_, true};
    // ES14.0 15.7.1
    // It is a Syntax Error if Initializer is present and ContainsArguments of
    // Initializer is true.
    SaveAndRestore<bool> oldForbidArguments{forbidArguments_, true};
    visitESTreeNode(*this, node->_value);
  }
}

void SemanticValidator::visit(ClassPropertyNode *node) {
  visitESTreeNode(*this, node->_key);
  {
    SaveAndRestore<bool> oldForbidAwait{forbidAwaitExpression_, true};
    // ES14.0 15.7.1
    // It is a Syntax Error if Initializer is present and ContainsArguments of
    // Initializer is true.
    SaveAndRestore<bool> oldForbidArguments{forbidArguments_, true};
    visitESTreeNode(*this, node->_value);
  }
}

void SemanticValidator::visit(StaticBlockNode *node) {
  if (compile_)
    sm_.error(node->getSourceRange(), "class static blocks are not supported");
  // ES14.0 15.7.1
  // It is a Syntax Error if ClassStaticBlockStatementList Contains await is
  // true.
  llvh::SaveAndRestore<bool> oldForbidAwait{forbidAwaitExpression_, true};
  visitESTreeChildren(*this, node);
}

void SemanticValidator::visit(ImportDeclarationNode *importDecl) {
  // Like variable declarations, imported names must be hoisted.
  if (!astContext_.getTransformCJSModules()) {
    sm_.error(
        importDecl->getSourceRange(),
        "'import' statement requires module mode");
  }

  if (compile_ && !importDecl->_assertions.empty()) {
    sm_.error(
        importDecl->getSourceRange(), "import assertions are not supported");
  }

  curFunction()->semInfo->imports.push_back(importDecl);
  visitESTreeChildren(*this, importDecl);
}

void SemanticValidator::visit(ImportDefaultSpecifierNode *importDecl) {
  // import defaultProperty from 'file.js';
  validateDeclarationNames(
      FunctionInfo::VarDecl::Kind::Var,
      importDecl->_local,
      curFunction()->varDecls,
      curFunction()->scopedDecls);
  visitESTreeChildren(*this, importDecl);
}

void SemanticValidator::visit(ImportNamespaceSpecifierNode *importDecl) {
  // import * as File from 'file.js';
  validateDeclarationNames(
      FunctionInfo::VarDecl::Kind::Var,
      importDecl->_local,
      curFunction()->varDecls,
      curFunction()->scopedDecls);
  visitESTreeChildren(*this, importDecl);
}

void SemanticValidator::visit(ImportSpecifierNode *importDecl) {
  // import {x as y} as File from 'file.js';
  // import {x} as File from 'file.js';
  validateDeclarationNames(
      FunctionInfo::VarDecl::Kind::Var,
      importDecl->_local,
      curFunction()->varDecls,
      curFunction()->scopedDecls);
  visitESTreeChildren(*this, importDecl);
}

void SemanticValidator::visit(ExportNamedDeclarationNode *exportDecl) {
  if (!astContext_.getTransformCJSModules()) {
    sm_.error(
        exportDecl->getSourceRange(),
        "'export' statement requires module mode");
  }

  visitESTreeChildren(*this, exportDecl);
}

void SemanticValidator::visit(ExportDefaultDeclarationNode *exportDecl) {
  if (!astContext_.getTransformCJSModules() &&
      !astContext_.getTransformCJSModules()) {
    sm_.error(
        exportDecl->getSourceRange(),
        "'export' statement requires module mode");
  }

  if (auto *funcDecl =
          dyn_cast<ESTree::FunctionDeclarationNode>(exportDecl->_declaration)) {
    if (compile_ && !funcDecl->_id) {
      // If the default function declaration has no name, then change it to a
      // FunctionExpression node for cleaner IRGen.
      auto *funcExpr = new (astContext_) ESTree::FunctionExpressionNode(
          funcDecl->_id,
          std::move(funcDecl->_params),
          funcDecl->_body,
          funcDecl->_typeParameters,
          funcDecl->_returnType,
          funcDecl->_predicate,
          funcDecl->_generator,
          /* async */ false);
      funcExpr->strictness = funcDecl->strictness;
      funcExpr->copyLocationFrom(funcDecl);

      exportDecl->_declaration = funcExpr;
    }
  }

  visitESTreeChildren(*this, exportDecl);
}

void SemanticValidator::visit(ExportAllDeclarationNode *exportDecl) {
  if (!astContext_.getTransformCJSModules()) {
    sm_.error(
        exportDecl->getSourceRange(),
        "'export' statement requires CommonJS module mode");
  }
  visitESTreeChildren(*this, exportDecl);
}

void SemanticValidator::visit(CoverEmptyArgsNode *CEA) {
  sm_.error(CEA->getSourceRange(), "invalid empty parentheses '( )'");
}

void SemanticValidator::visit(CoverTrailingCommaNode *CTC) {
  sm_.error(CTC->getSourceRange(), "expression expected after ','");
}

void SemanticValidator::visit(CoverInitializerNode *CI) {
  sm_.error(CI->getStartLoc(), "':' expected in property initialization");
}

void SemanticValidator::visit(CoverRestElementNode *R) {
  sm_.error(R->getSourceRange(), "'...' not allowed in this context");
}

#if HERMES_PARSE_FLOW
void SemanticValidator::visit(CoverTypedIdentifierNode *CTI) {
  sm_.error(CTI->getSourceRange(), "typecast not allowed in this context");
}
#endif

void SemanticValidator::visitFunction(
    FunctionLikeNode *node,
    Node *id,
    NodeList &params,
    Node *body) {
  FunctionContext newFuncCtx{
      this,
      haveActiveContext() && curFunction()->strictMode,
      node,
      body,
      haveActiveContext() ? curFunction()->sourceVisibility
                          : SourceVisibility::Default};

  if (compile_ && ESTree::isAsync(node) && ESTree::isGenerator(node)) {
    sm_.error(node->getSourceRange(), "async generators are unsupported");
  }

  // It is a Syntax Error if UniqueFormalParameters Contains YieldExpression
  // is true.
  // NOTE: isFormalParams_ is reset to false on encountering a new function,
  // because the semantics for "x Contains y" always return `false` when "x" is
  // a function definition.
  llvh::SaveAndRestore<bool> oldIsFormalParamsFn{isFormalParams_, false};

  Node *useStrictNode = nullptr;

  // Note that body might me empty (for lazy functions) or an expression (for
  // arrow functions).
  if (auto *bodyNode = dyn_cast<ESTree::BlockStatementNode>(body)) {
    if (bodyNode->isLazyFunctionBody) {
      // If it is a lazy function body, then the directive nodes in the body are
      // fabricated without location, so don't set useStrictNode.
      scanDirectivePrologue(bodyNode->_body);
    } else {
      useStrictNode = scanDirectivePrologue(bodyNode->_body);
    }
    setDirectiveDerivedInfo(node);
  }

  if (id)
    validateDeclarationNames(
        FunctionInfo::VarDecl::Kind::Var, id, nullptr, nullptr);

#if HERMES_PARSE_FLOW
  if (astContext_.getParseFlow() && !params.empty()) {
    // Skip 'this' parameter annotation, and error if it's an arrow parameter,
    // because arrow functions inherit 'this'.
    if (auto *ident = dyn_cast<ESTree::IdentifierNode>(&params.front())) {
      if (ident->_name == kw_.identThis) {
        if (isa<ArrowFunctionExpressionNode>(node)) {
          sm_.error(
              ident->getSourceRange(), "'this' not allowed as parameter name");
        }
        if (compile_) {
          // Delete the node because it cannot be compiled.
          params.erase(params.begin());
        }
      }
    }
  }
#endif

  for (auto &param : params) {
#if HERMES_PARSE_FLOW
    if (isa<ComponentParameterNode>(param)) {
      validateDeclarationNames(
          FunctionInfo::VarDecl::Kind::Var,
          dyn_cast<ComponentParameterNode>(&param)->_local,
          &newFuncCtx.semInfo->paramNames,
          nullptr);
      continue;
    }
#endif
    validateDeclarationNames(
        FunctionInfo::VarDecl::Kind::Var,
        &param,
        &newFuncCtx.semInfo->paramNames,
        nullptr);
  }

  bool simpleParameterList = ESTree::hasSimpleParams(node);
  if (!simpleParameterList && useStrictNode) {
    sm_.error(
        useStrictNode->getSourceRange(),
        "'use strict' not allowed inside function with non-simple parameter list");
  }

  // Check repeated parameter names when they are supposed to be unique.
  if (!simpleParameterList || curFunction()->strictMode ||
      isa<ArrowFunctionExpressionNode>(node)) {
    llvh::SmallSet<NodeLabel, 8> paramNameSet;
    for (const auto &curIdNode : newFuncCtx.semInfo->paramNames) {
      auto insert_result = paramNameSet.insert(curIdNode.identifier->_name);
      if (insert_result.second == false) {
        sm_.error(
            curIdNode.identifier->getSourceRange(),
            "cannot declare two parameters with the same name '" +
                curIdNode.identifier->_name->str() + "'");
      }
    }
  }

  // 'await' forbidden outside async functions.
  llvh::SaveAndRestore<bool> oldForbidAwait{
      forbidAwaitExpression_, !ESTree::isAsync(node)};
  // Forbidden-ness of 'arguments' passes through arrow functions because they
  // use the same 'arguments'.
  llvh::SaveAndRestore<bool> oldForbidArguments{
      forbidArguments_,
      llvh::isa<ESTree::ArrowFunctionExpressionNode>(node) ? forbidArguments_
                                                           : false};

  visitParamsAndBody(node);
}

void SemanticValidator::visitParamsAndBody(FunctionLikeNode *node) {
  switch (node->getKind()) {
    case NodeKind::FunctionExpression: {
      auto *fe = cast<ESTree::FunctionExpressionNode>(node);
      visitESTreeNode(*this, fe->_id, fe);
      for (auto &param : fe->_params) {
        llvh::SaveAndRestore<bool> oldIsFormalParams{isFormalParams_, true};
        visitESTreeNode(*this, &param, fe);
      }
      visitBody(fe->_body, fe);
      break;
    }
    case NodeKind::ArrowFunctionExpression: {
      auto *fe = cast<ESTree::ArrowFunctionExpressionNode>(node);
      visitESTreeNode(*this, fe->_id, fe);
      for (auto &param : fe->_params) {
        llvh::SaveAndRestore<bool> oldIsFormalParams{isFormalParams_, true};
        visitESTreeNode(*this, &param, fe);
      }
      visitBody(fe->_body, fe);
      break;
    }
    case NodeKind::FunctionDeclaration: {
      auto *fe = cast<ESTree::FunctionDeclarationNode>(node);
      visitESTreeNode(*this, fe->_id, fe);
      for (auto &param : fe->_params) {
        llvh::SaveAndRestore<bool> oldIsFormalParams{isFormalParams_, true};
        visitESTreeNode(*this, &param, fe);
      }
      visitBody(fe->_body, fe);
      visitESTreeNode(*this, fe->_returnType, fe);
      break;
    }
#if HERMES_PARSE_FLOW
    case NodeKind::ComponentDeclaration: {
      auto *fe = cast<ESTree::ComponentDeclarationNode>(node);
      visitESTreeNode(*this, fe->_id, fe);
      for (auto &param : fe->_params) {
        llvh::SaveAndRestore<bool> oldIsFormalParams{isFormalParams_, true};
        visitESTreeNode(*this, &param, fe);
      }
      visitBody(fe->_body, fe);
      visitESTreeNode(*this, fe->_rendersType, fe);
      break;
    }
    case NodeKind::HookDeclaration: {
      auto *fe = cast<ESTree::HookDeclarationNode>(node);
      visitESTreeNode(*this, fe->_id, fe);
      for (auto &param : fe->_params) {
        llvh::SaveAndRestore<bool> oldIsFormalParams{isFormalParams_, true};
        visitESTreeNode(*this, &param, fe);
      }
      visitBody(fe->_body, fe);
      visitESTreeNode(*this, fe->_returnType, fe);
      break;
    }
#endif
    default:
      visitESTreeChildren(*this, node);
  }
}

void SemanticValidator::visitBody(Node *body, FunctionLikeNode *func) {
  if (auto *block = dyn_cast<ESTree::BlockStatementNode>(body)) {
    // Avoid creating yet another block scope for function declarations like
    //
    // (function func() { ... })
    //                  ^ BlockStatementNode
    //
    // In those cases, the scope for the BlockStatementNode is the same as
    // func's.
    for (auto &stmt : block->_body) {
      visitESTreeNode(*this, &stmt, block);
    }
    return;
  }

  visitESTreeNode(*this, body, func);
}

void SemanticValidator::tryOverrideSourceVisibility(
    SourceVisibility newSourceVisibility) {
  if (newSourceVisibility > curFunction()->sourceVisibility) {
    curFunction()->sourceVisibility = newSourceVisibility;
  }
}

Node *SemanticValidator::scanDirectivePrologue(NodeList &body) {
  Node *result = nullptr;
  for (auto &nodeRef : body) {
    auto *exprSt = dyn_cast<ESTree::ExpressionStatementNode>(&nodeRef);
    if (!exprSt || !exprSt->_directive)
      break;

    auto *directive = exprSt->_directive;

    if (directive == kw_.identUseStrict) {
      curFunction()->strictMode = true;
      if (!result)
        result = &nodeRef;
    }
    if (directive == kw_.identShowSource) {
      tryOverrideSourceVisibility(SourceVisibility::ShowSource);
    }
    if (directive == kw_.identHideSource) {
      tryOverrideSourceVisibility(SourceVisibility::HideSource);
    }
    if (directive == kw_.identSensitive) {
      tryOverrideSourceVisibility(SourceVisibility::Sensitive);
    }
  }

  return result;
}

bool SemanticValidator::isLValue(const Node *node) const {
  if (isa<MemberExpressionNode>(node))
    return true;
  if (!isa<IdentifierNode>(node))
    return false;

  auto *idNode = cast<IdentifierNode>(node);

  /// 'arguments' cannot be modified in strict mode, but we also don't
  /// support modifying it in non-strict mode yet.
  if (idNode->_name == kw_.identArguments)
    return false;

  // 'eval' cannot be used as a variable in strict mode. If it is disabled we
  // we don't report an error because it will be reported separately.
  if (idNode->_name == kw_.identEval && curFunction()->strictMode &&
      astContext_.getEnableEval())
    return false;

  return true;
}

bool SemanticValidator::isValidDeclarationName(
    const IdentifierNode *idNode) const {
  // 'arguments' cannot be redeclared in strict mode.
  if (idNode->_name == kw_.identArguments && curFunction()->strictMode)
    return false;

  // 'eval' cannot be redeclared in strict mode. If it is disabled we
  // we don't report an error because it will be reported separately.
  if (idNode->_name == kw_.identEval && curFunction()->strictMode &&
      astContext_.getEnableEval())
    return false;

  return true;
}

void SemanticValidator::validateDeclarationNames(
    FunctionInfo::VarDecl::Kind declKind,
    Node *node,
    FunctionInfo::BlockDecls *varIdents,
    FunctionInfo::BlockDecls *scopedIdents) {
  assert(
      (varIdents || !scopedIdents) &&
      "Variable scope must always be provided if a scoped scope is provided.");
  // The identifier is sometimes optional, in which case it is valid.
  if (!node)
    return;

  if (auto *idNode = dyn_cast<IdentifierNode>(node)) {
    if (!blockScopingEnabled()) {
      // Block scoping is disabled, so short-circuit both BlockDecls to use the
      // var environment.
      scopedIdents = varIdents;
    }
    if (varIdents) {
      if (declKind == FunctionInfo::VarDecl::Kind::Var) {
        varIdents->emplace_back(FunctionInfo::VarDecl{declKind, idNode});
      } else {
        assert(
            scopedIdents &&
            "const/let declaration, but no scopedIdents array.");
        scopedIdents->emplace_back(FunctionInfo::VarDecl{declKind, idNode});
      }
    }
    if (!isValidDeclarationName(idNode)) {
      sm_.error(
          node->getSourceRange(),
          "cannot declare '" + cast<IdentifierNode>(node)->_name->str() + "'");
    }

    if (declKind != FunctionInfo::VarDecl::Kind::Var &&
        idNode->_name == kw_.identLet) {
      // ES9.0 13.3.1.1
      // LexicalDeclaration : LetOrConst BindingList
      // It is a Syntax Error if the BoundNames of BindingList
      // contains "let".
      sm_.error(
          node->getSourceRange(),
          "'let' is disallowed as a lexically bound name");
    }

    return;
  }

  if (isa<EmptyNode>(node))
    return;

  if (auto *assign = dyn_cast<AssignmentPatternNode>(node))
    return validateDeclarationNames(
        declKind, assign->_left, varIdents, scopedIdents);

  if (auto *array = dyn_cast<ArrayPatternNode>(node)) {
    for (auto &elem : array->_elements) {
      validateDeclarationNames(declKind, &elem, varIdents, scopedIdents);
    }
    return;
  }

  if (auto *restElem = dyn_cast<RestElementNode>(node)) {
    return validateDeclarationNames(
        declKind, restElem->_argument, varIdents, scopedIdents);
  }

  if (auto *obj = dyn_cast<ObjectPatternNode>(node)) {
    for (auto &propNode : obj->_properties) {
      if (auto *prop = dyn_cast<PropertyNode>(&propNode)) {
        validateDeclarationNames(
            declKind, prop->_value, varIdents, scopedIdents);
      } else {
        auto *rest = cast<RestElementNode>(&propNode);
        validateDeclarationNames(
            declKind, rest->_argument, varIdents, scopedIdents);
      }
    }
    return;
  }

  sm_.error(node->getSourceRange(), "invalid destructuring target");
}

void SemanticValidator::validateAssignmentTarget(const Node *node) {
  if (isa<EmptyNode>(node) || isLValue(node)) {
    return;
  }

  if (auto *assign = dyn_cast<AssignmentPatternNode>(node)) {
    return validateAssignmentTarget(assign->_left);
  }

  if (auto *APN = dyn_cast<ArrayPatternNode>(node)) {
    for (auto &elem : APN->_elements) {
      validateAssignmentTarget(&elem);
    }
    return;
  }

  if (auto *RP = dyn_cast<RestElementNode>(node)) {
    return validateAssignmentTarget(RP->_argument);
  }

  if (auto *obj = dyn_cast<ObjectPatternNode>(node)) {
    for (auto &propNode : obj->_properties) {
      if (auto *prop = dyn_cast<PropertyNode>(&propNode)) {
        assert(
            prop->_kind->str() == "init" &&
            "getters and setters must have been reported by the parser");
        validateAssignmentTarget(prop->_value);
      } else {
        auto *rest = cast<RestElementNode>(&propNode);
        validateAssignmentTarget(rest->_argument);
      }
    }
    return;
  }

  sm_.error(node->getSourceRange(), "invalid assignment left-hand side");
}

void SemanticValidator::setDirectiveDerivedInfo(FunctionLikeNode *node) {
  node->strictness = ESTree::makeStrictness(curFunction()->strictMode);
  node->sourceVisibility = curFunction()->sourceVisibility;
}

LabelDecorationBase *SemanticValidator::getLabelDecorationBase(
    StatementNode *node) {
  if (auto *LS = dyn_cast<LoopStatementNode>(node))
    return LS;
  if (auto *SS = dyn_cast<SwitchStatementNode>(node))
    return SS;
  if (auto *BS = dyn_cast<BreakStatementNode>(node))
    return BS;
  if (auto *CS = dyn_cast<ContinueStatementNode>(node))
    return CS;
  if (auto *LabS = dyn_cast<LabeledStatementNode>(node))
    return LabS;
  llvm_unreachable("invalid node type");
}

void SemanticValidator::recursionDepthExceeded(Node *n) {
  sm_.error(
      n->getEndLoc(), "Too many nested expressions/statements/declarations");
}

void SemanticValidator::reportRedeclaredIdentifier(
    const IdentifierNode &id1,
    const IdentifierNode &id2) {
  // The redeclared ID is the one that appears later in the source code.
  const IdentifierNode &redeclaredId = id1.getSourceRange().Start.getPointer() <
          id2.getSourceRange().Start.getPointer()
      ? id2
      : id1;
  // The original ID is the one that appears first in the source code.
  const IdentifierNode &originalId = &redeclaredId == &id2 ? id1 : id2;

  sm_.error(
      redeclaredId.getSourceRange(),
      "Identifier '" + redeclaredId._name->str() +
          "' has already been declared");
  sm_.note(
      originalId.getSourceRange(),
      "'" + redeclaredId._name->str() + "' previously defined here.");
  return;
}

//===----------------------------------------------------------------------===//
// BlockContext

/// Helper function that ensures the given \p ptr is not nullptr. If it is,
/// \p ptr will be initialized to a new \p T. \return \p ptr.
template <typename T>
std::unique_ptr<T> &initializeIfNull(std::unique_ptr<T> &ptr) {
  if (!ptr) {
    ptr.reset(new T);
  }
  return ptr;
}

BlockContext::BlockContext(
    SemanticValidator *validator,
    FunctionContext *curFunction,
    Node *nextScopeNode)
    : validator_(validator),
      curFunction_(curFunction),
      previousScopedDecls_(curFunction_->scopedDecls),
      previousScopedClosures_(curFunction_->scopedClosures),
      varDeclaredBegin_(curFunction_->semInfo->varScoped.size()) {
  if (nextScopeNode) {
    // nextScopeNode is nullptr for lazy compilation's wrapper context -- in
    // which case there is no need for setting up the block environment.

    if (!validator->blockScopingEnabled()) {
      // Block scope is disabled, so short-circuit nextScopeNode to be the
      // function body (or the Program node if the semantic validator is
      // currently traversing the global scope). This means all declarations
      // will be placed in the top-level function/global scope.
      nextScopeNode = curFunction->body;
    }

    // Now set up curFunction_'s scopedDecls and scopedClosures pointer. Note
    // that this will either create new containers (when block scoping is
    // enabled), or reuse the existing containers for the function's top-level
    // scope
    curFunction_->scopedDecls =
        initializeIfNull(curFunction_->semInfo->lexicallyScoped[nextScopeNode])
            .get();
    curFunction_->scopedClosures =
        initializeIfNull(curFunction_->semInfo->closures[nextScopeNode]).get();
  }
}

BlockContext::~BlockContext() {
  curFunction_->scopedDecls = previousScopedDecls_;
  curFunction_->scopedClosures = previousScopedClosures_;
}

void BlockContext::stopHoisting(IdentifierNode *id) {
  // The hoisting candidates are stored in a map of name to list of all known
  // functions with that name. Therefore, if name is in hoistingCandidates_,
  // clearing the list is all that's needed to stop function hoisting. Note
  // that the code __clears__ the list instead of removing it from the map, thus
  // preserving any memory allocated by the list (so, in case the same
  // identifier appears again the compiler won't incur in (possibly) heap
  // allocating memory).
  auto it = curFunction_->hoistingCandidates_.find(id->_name);
  if (it != curFunction_->hoistingCandidates_.end()) {
    curFunction_->hoistingCandidates_.erase(it);
  }
}

void BlockContext::ensureScopedNamesAreUnique(
    IsFunctionBody isFunctionBody,
    IdentifierNode *catchParam) {
  if (!validator_->blockScopingEnabled()) {
    return;
  }

  if (isFunctionBody == IsFunctionBody::Yes) {
    if (!curFunction_->scopedClosures) {
      // This happens during semantic validation for lazy compilation.
      return;
    }
  }

  llvh::SmallDenseMap<UniqueString *, IdentifierNode *, 8>
      lexicallyDeclaredNames;

  // It is a Syntax Error if the LexicallyDeclaredNames of StatementList
  // contains any duplicate entries. LexicallyDeclaredNames includes let/const
  // declarations. Function identifiers are not considered
  // LexicallyDeclaredNames in function scopes.
  if (isFunctionBody != IsFunctionBody::Yes) {
    // Keep track of all scopedClosures that are not regular functions (i.e.,
    // generators and async functions).
    llvh::SmallDenseSet<UniqueString *, 8> generatorOrAsync;
    for (FunctionDeclarationNode *scopedClosure :
         *curFunction_->scopedClosures) {
      auto *funName =
          llvh::dyn_cast_or_null<IdentifierNode>(scopedClosure->_id);

      if (!funName) {
        // Anonymous function names are always unique.
        assert(
            !scopedClosure->_id &&
            "FunctionLikeNode's _id is not an IdentifierNode");
        continue;
      }

      // ES2023 B.3.2.4 requires that duplicate identifiers within
      // FunctionDeclarations is not an error in non-strict mode. Thus, keep
      // track of the closures that are not regular function declarations so the
      // error can be reported. N.B.: adding the name here ensures that, should
      // the check for duplicate below fail, an error would still be reported
      // even if all previous instances of the duplicate symbol were
      // FunctionDeclarations and scopedClosure was not.
      if (ESTree::isAsync(scopedClosure) || scopedClosure->_generator) {
        generatorOrAsync.insert(funName->_name);
      }

      auto res = lexicallyDeclaredNames.insert(
          std::make_pair(funName->_name, funName));
      if (!res.second) {
        // ES2023 B.3.2.4 Changes to Block Static Semantics: Early Errors
        //
        // Block: {StatementList}
        //     It is a Syntax Error if the LexicallyDeclaredNames of
        //     StatementList contains any duplicate entries, **unless the
        //     source code matching this production is not strict mode code
        //     and the duplicate entries are only bound by
        //     FunctionDeclarations**.
        if (ESTree::isStrict(scopedClosure->strictness) ||
            generatorOrAsync.count(funName->_name)) {
          validator_->reportRedeclaredIdentifier(*res.first->second, *funName);
          return;
        }

        // Keep the ID that was declared earliest in the JS source. This is fine
        // since the validation stops on the first duplicate ID found.
        if (funName->getSourceRange().Start.getPointer() <
            res.first->second->getSourceRange().Start.getPointer()) {
          res.first->second = funName;
        }
      }
    }
  }

  // Nothing special for const/let declarations -- thus, just iterate over them,
  // and complain about duplicates.
  for (const FunctionInfo::VarDecl &scopedDecl : *curFunction_->scopedDecls) {
    // According to ES2023 B.3.2.1 and ES2023 B.3.2.2 functions should not be
    // hoisted if doing so would cause early errors. Therefore, any functions
    // that share its ID with scopedDecl cannot possibly be hoisted past this
    // scope.
    stopHoisting(scopedDecl.identifier);

    auto res = lexicallyDeclaredNames.insert(
        std::make_pair(scopedDecl.identifier->_name, scopedDecl.identifier));
    if (!res.second) {
      validator_->reportRedeclaredIdentifier(
          *res.first->second, *scopedDecl.identifier);
      return;
    }
  }

  if (catchParam) {
    auto it = lexicallyDeclaredNames.find(catchParam->_name);
    if (it != lexicallyDeclaredNames.end()) {
      validator_->reportRedeclaredIdentifier(*it->second, *catchParam);
      return;
    }
  }

  // Now ensure that the var decls in this scope don't clash with its
  // LexicallyDeclaredNames.
  for (auto it = curFunction_->semInfo->varScoped.begin() + varDeclaredBegin_,
            end = curFunction_->semInfo->varScoped.end();
       it != end;
       ++it) {
    IdentifierNode *name = it->identifier;
    auto pos = lexicallyDeclaredNames.find(name->_name);
    if (pos != lexicallyDeclaredNames.end()) {
      validator_->reportRedeclaredIdentifier(*pos->second, *name);
      return;
    }
  }
}

//===----------------------------------------------------------------------===//
// FunctionContext

FunctionContext::FunctionContext(
    SemanticValidator *validator,
    bool strictMode,
    FunctionLikeNode *node,
    Node *body,
    SourceVisibility sourceVisibility)
    : validator_(validator),
      oldContextValue_(validator->funcCtx_),
      node(node),
      body(body),
      semInfo(validator->semCtx_.createFunction()),
      varDecls(&semInfo->varScoped),
      strictMode(strictMode),
      sourceVisibility(sourceVisibility),
      functionScope_(validator, this, body) {
  validator->funcCtx_ = this;

  if (node)
    node->setSemInfo(semInfo);
}

FunctionContext::~FunctionContext() {
  functionScope_.ensureScopedNamesAreUnique(BlockContext::IsFunctionBody::Yes);
  validator_->funcCtx_ = oldContextValue_;
  finalizeHoisting();
}

void FunctionContext::addHoistingCandidate(FunctionDeclarationNode *funDecl) {
  auto *funDeclId = llvh::cast<IdentifierNode>(funDecl->_id);

  if (functionHoistingEnabled()) {
    hoistingCandidates_[funDeclId->_name].emplace_back(funDecl);
  }

  scopedClosures->emplace_back(funDecl);
}

void FunctionContext::finalizeHoisting() {
  assert(
      (functionHoistingEnabled() || hoistingCandidates_.size() == 0) &&
      "should not have hoisting candidates in strict mode.");

  if (node && !ESTree::isStrict(node->strictness)) {
    // Hoisting only happens in non-strict mode per ES2023 B.3.2.1 and
    // ES2023 B.3.2.2;
    for (const auto &[name, nodes] : hoistingCandidates_) {
      assert(
          !nodes.empty() && "empty vectors should be removed by stopHoisting");

      // Add a new var declaration to this function's varDecls. Only one
      // declaration per identifier is needed regardless of how many
      // functions with that id are hoisted.
      FunctionDeclarationNode *first = nodes[0];
      varDecls->emplace_back(
          FunctionInfo::VarDecl::withoutInitializer(
              FunctionInfo::VarDecl::Kind::Var,
              cast<ESTree::IdentifierNode>(first->_id)));

      // Now mark all functions as hoisted, which prevents creation of the
      // binding identifier below.
      for (FunctionDeclarationNode *fdn : nodes) {
        fdn->getSemInfo()->hoisted = true;
      }
    }
  }

  // Now add a scoped var decl (in the proper scope) for all functions that
  // weren't hoisted.
  assert(
      (validator_->astContext_.getCodeGenerationSettings().enableBlockScoping ||
       semInfo->closures.size() <= 1) &&
      "All closures should be added to the same container when block scoping "
      "is disabled");
  for (auto &[containingNode, containingNodeClosures] : semInfo->closures) {
    if (containingNodeClosures->empty()) {
      continue;
    }

    // For nested functions at the top level of an enclosing function,
    // the nested function is added to the enclosing function's
    // var list.  For nested functions in blocks in an enclosing
    // function, the nested function is added to the scope's
    // list.  For functions in the global scope, the function
    // is added to the global scope var list.  For functions
    // in a scope nested in the global scope, the function is added
    // to the scope's list.
    FunctionInfo::BlockDecls *decls =
        (containingNode == body && functionHoistingEnabled())
        ? &semInfo->varScoped
        : semInfo->lexicallyScoped[containingNode].get();

    for (auto it = containingNodeClosures->begin(),
              end = containingNodeClosures->end();
         it < end;
         ++it) {
      if (!(*it)->getSemInfo()->hoisted) {
        decls->emplace_back(
            FunctionInfo::VarDecl::withoutInitializer(
                FunctionInfo::VarDecl::Kind::Var,
                cast<ESTree::IdentifierNode>((*it)->_id)));
      }
    }
  }
}
} // namespace sem
} // namespace hermes
