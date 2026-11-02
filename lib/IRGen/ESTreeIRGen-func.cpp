/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ESTreeIRGen.h" // @manual

#include "llvh/ADT/SmallString.h"

#include <variant>

namespace hermes {
namespace irgen {

//===----------------------------------------------------------------------===//
// FunctionContext

FunctionContext::FunctionContext(
    ESTreeIRGen *irGen,
    Function *function,
    sem::FunctionInfo *semInfo)
    : irGen_(irGen),
      semInfo_(semInfo),
      oldContext_(irGen->functionContext_),
      builderSaveState_(irGen->Builder),
      function(function),
      anonymousIDs_(function->getContext().getStringTable()),
      enterFunctionScope(this) {
  setupFunctionScope(&enterFunctionScope);
  irGen->functionContext_ = this;
  irGen->currentIRScopeDesc_ = function->getFunctionScopeDesc();

  // Temporarily set the current IR scope to nullptr. IRGen should materialize
  // currentIRScopeDesc_ before trying to access it.
  irGen->currentIRScope_ = nullptr;

  // Initialize it to LiteralUndefined by default to avoid corner cases.
  this->capturedNewTarget = irGen->Builder.getLiteralUndefined();

  if (semInfo_) {
    // Allocate the label table. Each label definition will be encountered in
    // the AST before it is referenced (because of the nature of JavaScript), at
    // which point we will initialize the GotoLabel structure with basic blocks
    // targets.
    labels_.resize(semInfo_->labelCount);
  }
}

void FunctionContext::setupFunctionScope(EnterBlockScope *scope) {
  functionScope = &scope->blockScope_;
  blockScope = functionScope;
}

FunctionContext::~FunctionContext() {
  irGen_->functionContext_ = oldContext_;
  irGen_->Builder.setCurrentSourceLevelScope(irGen_->currentIRScopeDesc_);
}

Identifier FunctionContext::genAnonymousLabelName(llvh::StringRef hint) {
  return anonymousIDs_.next(hint);
}

//===----------------------------------------------------------------------===//
// EnterBlockScope
EnterBlockScope::EnterBlockScope(FunctionContext *currentContext)
    : currentContext_(currentContext),
      oldIRScopeDesc_(currentContext->irGen_->currentIRScopeDesc_),
      oldIRScope_(currentContext->irGen_->currentIRScope_),
      oldBlockScope_(currentContext->blockScope),
      blockScope_(currentContext->irGen_->nameTable_) {
  currentContext->blockScope = &blockScope_;
}

EnterBlockScope::~EnterBlockScope() {
  ESTreeIRGen *irgen = currentContext_->irGen_;
  currentContext_->blockScope = oldBlockScope_;
  irgen->currentIRScope_ = oldIRScope_;
  irgen->currentIRScopeDesc_ = oldIRScopeDesc_;
  irgen->Builder.setCurrentSourceLevelScope(irgen->currentIRScopeDesc_);
}

//===----------------------------------------------------------------------===//
// ESTreeIRGen

void ESTreeIRGen::genFunctionDeclaration(
    ESTree::FunctionDeclarationNode *func) {
  // Find the name of the function.
  Identifier functionName = getNameFieldFromID(func->_id);
  LLVM_DEBUG(llvh::dbgs() << "IRGen function \"" << functionName << "\".\n");

  Function *newFunc = func->_async
      ? genAsyncFunction(functionName, nullptr, func)
      : func->_generator ? genGeneratorFunction(functionName, nullptr, func)
                         : genES5Function(functionName, nullptr, func);

  functionForDecl[func] = {newFunc, AlreadyEmitted::No};
}

void ESTreeIRGen::emitCreateFunction(ESTree::FunctionDeclarationNode *func) {
  Identifier functionName = getNameFieldFromID(func->_id);

  auto it = functionForDecl.find(func);
  assert(it != functionForDecl.end() && "all inner functions should be known");

  if (it->second.second == AlreadyEmitted::Yes) {
    return;
  }

  it->second.second = AlreadyEmitted::Yes;

  auto *funcStorage = nameTable_.lookup(functionName);
  assert(
      funcStorage && "function declaration variable should have been hoisted");

  // Store the newly created closure into a frame variable with the same name.
  auto *newClosure =
      Builder.createCreateFunctionInst(it->second.first, currentIRScope_);

  emitStore(newClosure, funcStorage, true);
}

void ESTreeIRGen::hoistCreateFunctions(ESTree::Node *containingNode) {
  const auto &closures = curFunction()->getSemInfo()->closures;
  auto it = closures.find(containingNode);
  if (it == closures.end()) {
    return;
  }

  for (ESTree::FunctionDeclarationNode *funcDecl : *it->second) {
    emitCreateFunction(funcDecl);
  }
}

Value *ESTreeIRGen::genFunctionExpression(
    ESTree::FunctionExpressionNode *FE,
    Identifier nameHint) {
  LLVM_DEBUG(
      llvh::dbgs()
      << "Creating anonymous closure. "
      << Builder.getInsertionBlock()->getParent()->getInternalName() << ".\n");

  std::variant<std::monostate, NameTableScopeTy, EnterBlockScope> newScope;

  if (!Mod->getContext().getCodeGenerationSettings().enableBlockScoping) {
    newScope.emplace<NameTableScopeTy>(nameTable_);
  } else {
    newScope.emplace<EnterBlockScope>(curFunction());
    newDeclarativeEnvironment();
  }
  Variable *tempClosureVar = nullptr;

  Identifier originalNameIden = nameHint;
  if (FE->_id) {
    if (!Mod->getContext().getCodeGenerationSettings().enableBlockScoping) {
      auto closureName = genAnonymousLabelName("closure");
      tempClosureVar = Builder.createVariable(
          curFunction()->function->getFunctionScopeDesc(),
          Variable::DeclKind::Var,
          closureName);

      // Insert the synthesized variable into the name table, so it can be
      // looked up internally as well.
      nameTable_.insertIntoScope(
          curFunction()->functionScope,
          tempClosureVar->getName(),
          tempClosureVar);

      // Alias the lexical name to the synthesized variable.
      originalNameIden = getNameFieldFromID(FE->_id);
      nameTable_.insert(originalNameIden, tempClosureVar);
    } else {
      // Use the expression's ID for its closure name, as well as its variable
      // name -- which is OK because we're in a new scope.
      originalNameIden = getNameFieldFromID(FE->_id);

      auto closureName = genAnonymousLabelName(originalNameIden.str());
      tempClosureVar = Builder.createVariable(
          currentIRScopeDesc_, Variable::DeclKind::Const, closureName);

      // ES2023 dictates that function expression's ID are non-strict immutable
      // bindings.
      tempClosureVar->setStrictImmutableBinding(false);

      nameTable_.insertIntoScope(
          curFunction()->blockScope, originalNameIden, tempClosureVar);
    }
  }

  Function *newFunc = FE->_async
      ? genAsyncFunction(originalNameIden, tempClosureVar, FE)
      : FE->_generator
      ? genGeneratorFunction(originalNameIden, tempClosureVar, FE)
      : genES5Function(originalNameIden, tempClosureVar, FE);

  Value *closure = Builder.createCreateFunctionInst(newFunc, currentIRScope_);

  if (tempClosureVar)
    emitStore(closure, tempClosureVar, true);

  return closure;
}

Value *ESTreeIRGen::genArrowFunctionExpression(
    ESTree::ArrowFunctionExpressionNode *AF,
    Identifier nameHint) {
  LLVM_DEBUG(
      llvh::dbgs()
      << "Creating arrow function. "
      << Builder.getInsertionBlock()->getParent()->getInternalName() << ".\n");

  if (AF->_async) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        AF->getSourceRange(), Twine("async functions are unsupported"));
    return Builder.getLiteralUndefined();
  }

  auto *newFunc = Builder.createFunction(
      newScopeDesc(),
      genAnonymousFunctionNameIfNeeded(nameHint),
      Function::DefinitionKind::ES6Arrow,
      ESTree::isStrict(AF->strictness),
      AF->sourceVisibility,
      AF->getSourceRange());

  {
    FunctionContext newFunctionContext{this, newFunc, AF->getSemInfo()};

    // Propagate captured "this", "new.target" and "arguments" from parents.
    auto *prev = curFunction()->getPreviousContext();
    curFunction()->capturedThis = prev->capturedThis;
    curFunction()->capturedNewTarget = prev->capturedNewTarget;
    curFunction()->capturedArguments = prev->capturedArguments;

    emitFunctionPreamble(Builder.createBasicBlock(newFunc));
    emitTopLevelDeclarations(AF, AF->_body, DoEmitParameters::YesMultiScopes);

    genFunctionBody(AF->_body);
    emitFunctionEpilogue(Builder.getLiteralUndefined());
  }

  // Emit CreateFunctionInst after we have restored the builder state.
  return Builder.createCreateFunctionInst(newFunc, currentIRScope_);
}

namespace {
ESTree::NodeKind getLazyFunctionKind(ESTree::FunctionLikeNode *node) {
  if (node->isMethodDefinition) {
    // This is not a regular function expression but getter/setter.
    // If we want to reparse it later, we have to start from an
    // identifier and not from a 'function' keyword.
    return ESTree::NodeKind::Property;
  }
  return node->getKind();
}
} // namespace
Function *ESTreeIRGen::genES5Function(
    Identifier originalName,
    Variable *lazyClosureAlias,
    ESTree::FunctionLikeNode *functionNode,
    bool isGeneratorInnerFunction) {
  assert(functionNode && "Function AST cannot be null");

  auto *body = ESTree::getBlockStatement(functionNode);
  assert(body && "body of ES5 function cannot be null");

  Function *newFunction = isGeneratorInnerFunction
      ? Builder.createGeneratorInnerFunction(
            newScopeDesc(),
            genAnonymousFunctionNameIfNeeded(originalName),
            Function::DefinitionKind::ES5Function,
            ESTree::isStrict(functionNode->strictness),
            functionNode->getSourceRange(),
            /* insertBefore */ nullptr)
      : Builder.createFunction(
            newScopeDesc(),
            genAnonymousFunctionNameIfNeeded(originalName),
            Function::DefinitionKind::ES5Function,
            ESTree::isStrict(functionNode->strictness),
            functionNode->sourceVisibility,
            functionNode->getSourceRange(),
            /* isGlobal */ false,
            /* insertBefore */ nullptr);

  newFunction->setLazyClosureAlias(lazyClosureAlias);

  if (auto *bodyBlock = llvh::dyn_cast<ESTree::BlockStatementNode>(body)) {
    if (bodyBlock->isLazyFunctionBody) {
      assert(
          !isGeneratorInnerFunction &&
          "generator inner function should be included with outer function");
      setupLazyScope(functionNode, newFunction, body);
      return newFunction;
    }
  }

  FunctionContext newFunctionContext{
      this, newFunction, functionNode->getSemInfo()};

  if (isGeneratorInnerFunction) {
    // StartGeneratorInst
    // ResumeGeneratorInst
    // at the beginning of the function, to allow for the first .next() call.
    auto *initGenBB = Builder.createBasicBlock(newFunction);
    Builder.setInsertionBlock(initGenBB);
    Builder.createStartGeneratorInst();
    auto *prologueBB = Builder.createBasicBlock(newFunction);
    auto *prologueResumeIsReturn = Builder.createAllocStackInst(
        genAnonymousLabelName("isReturn_prologue"));
    genResumeGenerator(GenFinally::No, prologueResumeIsReturn, prologueBB);

    if (hasSimpleParams(functionNode)) {
      // If there are simple params, then we don't need an extra yield/resume.
      // They can simply be initialized on the first call to `.next`.
      Builder.setInsertionBlock(prologueBB);
      emitFunctionPreamble(prologueBB);
      initCaptureStateInES5Function();
      emitTopLevelDeclarations(
          functionNode, body, DoEmitParameters::YesMultiScopes);
    } else {
      // If there are non-simple params, then we must add a new yield/resume.
      // The `.next()` call will occur once in the outer function, before
      // the iterator is returned to the caller of the `function*`.
      auto *entryPointBB = Builder.createBasicBlock(newFunction);
      auto *entryPointResumeIsReturn =
          Builder.createAllocStackInst(genAnonymousLabelName("isReturn_entry"));

      // Initialize parameters.
      Builder.setInsertionBlock(prologueBB);
      emitFunctionPreamble(prologueBB);
      initCaptureStateInES5Function();
      emitTopLevelDeclarations(
          functionNode, body, DoEmitParameters::YesMultiScopes);
      Builder.createSaveAndYieldInst(
          Builder.getLiteralUndefined(), entryPointBB);

      // Actual entry point of function from the caller's perspective.
      Builder.setInsertionBlock(entryPointBB);
      genResumeGenerator(
          GenFinally::No,
          entryPointResumeIsReturn,
          Builder.createBasicBlock(newFunction));
    }
  } else {
    emitFunctionPreamble(Builder.createBasicBlock(newFunction));
    initCaptureStateInES5Function();
    emitTopLevelDeclarations(
        functionNode, body, DoEmitParameters::YesMultiScopes);
  }

  genFunctionBody(body);
  emitFunctionEpilogue(Builder.getLiteralUndefined());

  return curFunction()->function;
}

Function *ESTreeIRGen::genGeneratorFunction(
    Identifier originalName,
    Variable *lazyClosureAlias,
    ESTree::FunctionLikeNode *functionNode) {
  assert(functionNode && "Function AST cannot be null");

  if (!Builder.getModule()->getContext().isGeneratorEnabled()) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        functionNode->getSourceRange(), "generator compilation is disabled");
  }

  // Build the outer function which creates the generator.
  // Does not have an associated source range.
  auto *outerFn = Builder.createGeneratorFunction(
      newScopeDesc(),
      genAnonymousFunctionNameIfNeeded(originalName),
      Function::DefinitionKind::ES5Function,
      ESTree::isStrict(functionNode->strictness),
      functionNode->sourceVisibility,
      functionNode->getSourceRange(),
      /* insertBefore */ nullptr);
  outerFn->setLazyClosureAlias(lazyClosureAlias);

  auto *body = ESTree::getBlockStatement(functionNode);
  if (auto *bodyBlock = llvh::dyn_cast<ESTree::BlockStatementNode>(body)) {
    if (bodyBlock->isLazyFunctionBody) {
      setupLazyScope(functionNode, outerFn, body);
      return outerFn;
    }
  }

  {
    FunctionContext outerFnContext{this, outerFn, functionNode->getSemInfo()};

    // Build the inner function. This must be done in the outerFnContext
    // since it's lexically considered a child function.
    auto *innerFn = genES5Function(
        genAnonymousLabelName(originalName.isValid() ? originalName.str() : ""),
        nullptr,
        functionNode,
        true);

    emitFunctionPreamble(Builder.createBasicBlock(outerFn));
    initCaptureStateInES5Function();
    emitTopLevelDeclarations(
        functionNode,
        ESTree::getBlockStatement(functionNode),
        DoEmitParameters::No);

    // Create a generator function, which will store the arguments.
    auto *gen = Builder.createCreateGeneratorInst(innerFn, currentIRScope_);

    if (!hasSimpleParams(functionNode)) {
      // If there are non-simple params, step the inner function once to
      // initialize them.
      Value *next = Builder.createLoadPropertyInst(gen, "next");
      Builder.createCallInst(CallInst::kNoTextifiedCallee, next, gen, {});
    }

    emitFunctionEpilogue(gen);
  }

  return outerFn;
}

void ESTreeIRGen::setupLazyScope(
    ESTree::FunctionLikeNode *functionNode,
    Function *function,
    ESTree::BlockStatementNode *bodyBlock) {
  assert(
      bodyBlock->isLazyFunctionBody &&
      "setupLazyScope can only be used with lazy function bodies");
  // Set the AST position and variable context so we can continue later.
  // Save the scope chain starting from function's parent (i.e., the last
  // materialized scope).
  function->setLazyScope(
      saveScopeChain(function->getFunctionScopeDesc()->getParent()));
  auto &lazySource = function->getLazySource();
  lazySource.bufferId = bodyBlock->bufferId;
  lazySource.nodeKind = getLazyFunctionKind(functionNode);
  lazySource.functionRange = functionNode->getSourceRange();
  lazySource.paramYield = bodyBlock->paramYield;
  lazySource.paramAwait = bodyBlock->paramAwait;

  // Set the function's .length.
  function->setExpectedParamCountIncludingThis(
      countExpectedArgumentsIncludingThis(functionNode));
}

Function *ESTreeIRGen::genAsyncFunction(
    Identifier originalName,
    Variable *lazyClosureAlias,
    ESTree::FunctionLikeNode *functionNode) {
  assert(functionNode && "Function AST cannot be null");

  if (!Builder.getModule()->getContext().isGeneratorEnabled()) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        functionNode->getSourceRange(),
        "async function compilation requires enabling generator");
  }

  auto *asyncFn = Builder.createAsyncFunction(
      newScopeDesc(),
      genAnonymousFunctionNameIfNeeded(originalName),
      Function::DefinitionKind::ES5Function,
      ESTree::isStrict(functionNode->strictness),
      functionNode->sourceVisibility,
      functionNode->getSourceRange(),
      /* insertBefore */ nullptr);

  // Setup lazy compilation
  asyncFn->setLazyClosureAlias(lazyClosureAlias);

  auto *body = ESTree::getBlockStatement(functionNode);
  if (auto *bodyBlock = llvh::dyn_cast<ESTree::BlockStatementNode>(body)) {
    if (bodyBlock->isLazyFunctionBody) {
      setupLazyScope(functionNode, asyncFn, body);
      return asyncFn;
    }
  }

  {
    FunctionContext asyncFnContext{this, asyncFn, functionNode->getSemInfo()};

    // Build the inner generator. This must be done in the outerFnContext
    // since it's lexically considered a child function.
    auto *gen = genGeneratorFunction(
        genAnonymousLabelName(originalName.isValid() ? originalName.str() : ""),
        lazyClosureAlias,
        functionNode);

    // The outer async function need not emit code for parameters.
    // It would simply delegate `arguments` object down to inner generator.
    // This avoid emitting code e.g. destructuring parameters twice.
    emitFunctionPreamble(Builder.createBasicBlock(asyncFn));
    initCaptureStateInES5Function();
    emitTopLevelDeclarations(
        functionNode,
        ESTree::getBlockStatement(functionNode),
        DoEmitParameters::No);

    auto *genClosure = Builder.createCreateFunctionInst(gen, currentIRScope_);
    auto *thisArg = curFunction()->function->getThisParameter();
    auto *argumentsList = curFunction()->createArgumentsInst;

    auto *spawnAsyncClosure = Builder.createGetBuiltinClosureInst(
        BuiltinMethod::HermesBuiltin_spawnAsync);

    auto *res = Builder.createCallInst(
        CallInst::kNoTextifiedCallee,
        spawnAsyncClosure,
        Builder.getLiteralUndefined(),
        {genClosure, thisArg, argumentsList});

    emitFunctionEpilogue(res);
  }
  return asyncFn;
}

void ESTreeIRGen::initCaptureStateInES5Function() {
  // Capture "this", "new.target" and "arguments" if there are inner arrows.
  if (!curFunction()->getSemInfo()->containsArrowFunctions)
    return;

  // "this".
  curFunction()->capturedThis = Builder.createVariable(
      currentIRScopeDesc_,
      Variable::DeclKind::Var,
      genAnonymousLabelName("this"));
  emitStore(
      Builder.getFunction()->getThisParameter(),
      curFunction()->capturedThis,
      true);

  // "new.target".
  curFunction()->capturedNewTarget = Builder.createVariable(
      currentIRScopeDesc_,
      Variable::DeclKind::Var,
      genAnonymousLabelName("new.target"));
  emitStore(
      Builder.createGetNewTargetInst(), curFunction()->capturedNewTarget, true);

  // "arguments".
  if (curFunction()->getSemInfo()->containsArrowFunctionsUsingArguments) {
    curFunction()->capturedArguments = Builder.createVariable(
        currentIRScopeDesc_,
        Variable::DeclKind::Var,
        genAnonymousLabelName("arguments"));
    emitStore(
        curFunction()->createArgumentsInst,
        curFunction()->capturedArguments,
        true);
  }
}

void ESTreeIRGen::emitFunctionPreamble(BasicBlock *entry) {
  auto *newFunc = curFunction()->function;

  Builder.setLocation(newFunc->getSourceRange().Start);
  Builder.setCurrentSourceLevelScope(nullptr);

  BasicBlock *realEntry = &newFunc->front();
  if (realEntry->empty()) {
    Builder.setInsertionBlock(realEntry);
  } else {
    Builder.setInsertionPoint(&realEntry->front());
  }
  // Create the function scope.
  currentIRScope_ =
      Builder.createCreateScopeInst(newFunc->getFunctionScopeDesc());

  // Start pumping instructions into the entry basic block.
  Builder.setInsertionBlock(entry);
  Builder.setCurrentSourceLevelScope(newFunc->getFunctionScopeDesc());

  // Always insert a CreateArgumentsInst. We will delete it later if it is
  // unused.
  curFunction()->createArgumentsInst = Builder.createCreateArgumentsInst();

  // Always create the "this" parameter. It needs to be created before we
  // initialized the ES5 capture state.
  Builder.createThisParameter(newFunc);
}

void ESTreeIRGen::emitTopLevelDeclarations(
    ESTree::FunctionLikeNode *funcNode,
    ESTree::Node *body,
    DoEmitParameters doEmitParameters) {
  // There is a lot happening in this function w.r.t. function scopes, but that
  // can be summarizes as follows:
  //
  //    topLevelScope = Scope();                                #1
  //    currentScope = topLevelScope
  //
  //    if !isStrict and hasParamExpressions:
  //        paramExpressionScope = InnerScope(currentScope)
  //        currentScope = paramExpressionScope                 #2
  //
  //    << emit parameter expressions >>
  //
  //    if hasParamExpressions:
  //       varScope = InnerScope(currentScope)
  //       currentScope = varScope                              #3
  //
  //    << emit var declarations >>
  //
  //    if !isStrict:
  //       lexicalScope = InnerScope(currentScope)              #4
  //       currentScope = lexicalScope
  //
  //    << emit lexical declarations >>
  //
  // Thus at the end of this method, currentIRScopeDesc_ (i.e., currentScope
  // above) should be
  //
  // 1. !isStrict and !hasParamExpressions:
  //        currentIRScopeDesc_ == lexicalScope
  //        lexicalScope.parent == topLevelScope
  //        paramExpressionScope == null
  //        varScope == null
  // 2. !isStrict and hasParamExpressions:
  //        currentIRScopeDesc_ == lexicalScope
  //        lexicalScope.parent == varScope
  //        varScope.parent == paramExpressionScope
  //        paramExpressionScope.parent == topLevelScope
  // 3. isStrict and !hasParamExpressions:
  //        currentIRScopeDesc_ == topLevelScope
  //        paramExpressionScope == null
  //        varScope == null
  //        lexicalScope == null
  // 4. isStrict and hasParamExpressions:
  //        currentIRScopeDesc_ == varScope
  //        varScope.parent == topLevelScope
  //        paramExpressionScope == null
  //        lexicalScope == null
  //
  // The following variables are used to assert those conditions.
  ScopeDesc *topLevelScope = currentIRScopeDesc_;
  ScopeDesc *paramExpressionScope{};
  ScopeDesc *varScope{};
  ScopeDesc *lexicalScope{};

  if (!Mod->getContext().getCodeGenerationSettings().enableBlockScoping) {
    if (doEmitParameters == DoEmitParameters::YesMultiScopes) {
      // Block scoping is disabled, so there's no point in emitting separate
      // scopes.
      doEmitParameters = DoEmitParameters::Yes;
    }
  }

  const bool hasParamExpressions = ESTree::hasParamExpressions(funcNode);
  if (doEmitParameters == DoEmitParameters::YesMultiScopes) {
    if (!ESTree::isStrict(funcNode->strictness) && hasParamExpressions) {
      curFunction()->enterOptionalFunctionScope(
          &FunctionContext::enterParamScope);
      newDeclarativeEnvironment();
      paramExpressionScope = currentIRScopeDesc_;
    }
  }

  if (doEmitParameters != DoEmitParameters::No) {
    // Create function parameters, register them in the scope, and initialize
    // them with their income values.
    emitParameters(funcNode, hasParamExpressions);
  } else {
    curFunction()->function->setExpectedParamCountIncludingThis(
        countExpectedArgumentsIncludingThis(funcNode));
  }

  auto *semInfo = curFunction()->getSemInfo();
  if (doEmitParameters != DoEmitParameters::YesMultiScopes ||
      !hasParamExpressions) {
    // Create variable declarations for each of the hoisted variables and
    // functions. Initialize them to undefined.
    for (const sem::FunctionInfo::VarDecl &decl : semInfo->varScoped) {
      createNewBinding(
          currentIRScopeDesc_,
          decl.kind,
          decl.identifier,
          decl.needsInitializer);
    }

  } else {
    curFunction()->enterOptionalFunctionScope(&FunctionContext::enterVarScope);
    newDeclarativeEnvironment();
    varScope = currentIRScopeDesc_;

    for (const sem::FunctionInfo::VarDecl &decl : semInfo->varScoped) {
      Value *init = nameTable_.lookup(getNameFieldFromID(decl.identifier));
      if (init) {
        init = emitLoad(init);
      }
      createNewBinding(
          currentIRScopeDesc_,
          decl.kind,
          decl.identifier,
          decl.needsInitializer,
          init);
    }
  }

  if (doEmitParameters == DoEmitParameters::YesMultiScopes) {
    if (!ESTree::isStrict(funcNode->strictness)) {
      curFunction()->enterOptionalFunctionScope(
          &FunctionContext::enterTopLevelLexicalDeclarationsScope);
      newDeclarativeEnvironment();
      lexicalScope = currentIRScopeDesc_;
    }
  }

  // Now that scope creation is completed, ensure that the expectations hold:
  if (doEmitParameters == DoEmitParameters::YesMultiScopes) {
    (void)topLevelScope;
    (void)paramExpressionScope;
    (void)varScope;
    (void)lexicalScope;
    if (!ESTree::isStrict(funcNode->strictness)) {
      if (!hasParamExpressions) {
        // 1. !isStrict and !hasParamExpressions:
        assert(currentIRScopeDesc_ == lexicalScope);
        assert(lexicalScope->getParent() == topLevelScope);
        assert(paramExpressionScope == nullptr);
        assert(varScope == nullptr);
      } else {
        // 2. !isStrict and hasParamExpressions:
        assert(currentIRScopeDesc_ == lexicalScope);
        assert(lexicalScope->getParent() == varScope);
        assert(varScope->getParent() == paramExpressionScope);
        assert(paramExpressionScope->getParent() == topLevelScope);
      }
    } else {
      if (!hasParamExpressions) {
        // 3. isStrict and !hasParamExpressions:
        assert(currentIRScopeDesc_ == topLevelScope);
        assert(paramExpressionScope == nullptr);
        assert(varScope == nullptr);
        assert(lexicalScope == nullptr);
      } else {
        // 4. isStrict and hasParamExpressions:
        assert(currentIRScopeDesc_ == varScope);
        assert(varScope->getParent() == topLevelScope);
        assert(paramExpressionScope == nullptr);
        assert(lexicalScope == nullptr);
      }
    }
  }

  // Now create variable declarations for each scoped variable/function in body.
  // let/const bindings are initialized to empty, and functions to undefined.
  createScopeBindings(currentIRScopeDesc_, body);

  // Generate the code for import declarations before generating the rest of the
  // body.
  for (ESTree::ImportDeclarationNode *importDecl : semInfo->imports) {
    genImportDeclaration(importDecl);
  }

  // Generate all closures declared in body. Any hoisted
  // functions from inner scopes have already been declared.
  genFunctionDeclarations(body);

  // Pre-hoists all functions that are defined within body (but not in
  // BlockStatments in it).
  hoistCreateFunctions(body);
}

void ESTreeIRGen::genFunctionDeclarations(ESTree::Node *containingNode) {
  auto *semInfo = curFunction()->getSemInfo();

  auto it = semInfo->closures.find(containingNode);
  if (it != semInfo->closures.end()) {
    for (ESTree::FunctionDeclarationNode *fd : *it->second) {
      genFunctionDeclaration(fd);
    }
  }
}

void ESTreeIRGen::createScopeBindings(
    ScopeDesc *scopeDesc,
    ESTree::Node *containingNode) {
  auto *semInfo = curFunction()->getSemInfo();

  auto it = semInfo->lexicallyScoped.find(containingNode);
  if (it != semInfo->lexicallyScoped.end()) {
    for (const sem::FunctionInfo::VarDecl &decl : *it->second) {
      LLVM_DEBUG(
          llvh::dbgs() << "creating binding " << decl.identifier->_name
                       << " in scope " << scopeDesc << "\n");
      createNewBinding(
          scopeDesc, decl.kind, decl.identifier, decl.needsInitializer);
      if (Mod->getContext().getCodeGenerationSettings().enableBlockScoping) {
        if (decl.kind != VarDecl::Kind::Var && scopeDesc->isGlobalScope() &&
            llvh::isa<ESTree::ProgramNode>(containingNode)) {
          // The newly created Variable is a const/let declaration, so the
          // running program must check whether it is a valid name. For example,
          //
          // let undefined;
          //
          // is an invalid global let declaration because undefined is a
          // restricted global name.
          IRBuilder::ScopedLocationChange slc(
              Builder, decl.identifier->getSourceRange().Start);
          Builder.createThrowIfHasRestrictedGlobalPropertyInst(
              decl.identifier->_name->str());
        }
      }
    }
  }
}

void ESTreeIRGen::createNewBinding(
    ScopeDesc *scopeDesc,
    VarDecl::Kind kind,
    ESTree::Node *id,
    bool needsInitializer,
    Value *init) {
  Identifier name = getNameFieldFromID(id);
  auto res = declareVariableOrGlobalProperty(scopeDesc, kind, name);
  // If this is not a frame variable or it was already declared, skip.
  auto *var = llvh::dyn_cast<Variable>(res.first);
  if (!needsInitializer || !var || !res.second)
    return;

  if (!init) {
    init = var->getObeysTDZ() ? (Literal *)Builder.getLiteralEmpty()
                              : (Literal *)Builder.getLiteralUndefined();
  }

  // Otherwise, initialize it to undefined or empty, depending on TDZ.
  Builder.createStoreFrameInst(init, var, currentIRScope_);
}

void ESTreeIRGen::emitParameters(
    ESTree::FunctionLikeNode *funcNode,
    bool hasParamExpressions) {
  auto *newFunc = curFunction()->function;

  LLVM_DEBUG(llvh::dbgs() << "IRGen function parameters.\n");

  if (!Mod->getContext().getCodeGenerationSettings().enableBlockScoping) {
    // Disable parameter TDZ if block scoping support is disabled.
    hasParamExpressions = false;
  }

  llvh::SmallVector<Variable *, 4> tdzParams;

  // Create a variable for every parameter.
  Value *empty = Builder.getLiteralEmpty();
  Variable::DeclKind paramKind =
      hasParamExpressions ? Variable::DeclKind::Let : Variable::DeclKind::Var;
  for (auto paramDecl : funcNode->getSemInfo()->paramNames) {
    Identifier paramName = getNameFieldFromID(paramDecl.identifier);
    LLVM_DEBUG(llvh::dbgs() << "Adding parameter: " << paramName << "\n");
    auto *paramStorage = Builder.createVariable(
        newFunc->getFunctionScopeDesc(), paramKind, paramName);
    if (hasParamExpressions) {
      // funcNode has parameter expressions, thus the parameters are first
      // copied to tdz variables which need to be initialized to empty.
      Builder.createStoreFrameInst(empty, paramStorage, currentIRScope_);

      // Also keep track of all created Variables to copy them into regular Var
      // declarations.
      tdzParams.push_back(paramStorage);
    }
    // Register the storage for the parameter.
    nameTable_.insert(paramName, paramStorage);
  }

  uint32_t paramIndex = uint32_t{0} - 1;
  for (auto &elem : ESTree::getParams(funcNode)) {
    ESTree::Node *param = &elem;
    ESTree::Node *init = nullptr;
    ++paramIndex;

    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(param)) {
      createLRef(rest->_argument, true)
          .emitStore(genBuiltinCall(
              BuiltinMethod::HermesBuiltin_copyRestArgs,
              Builder.getLiteralNumber(paramIndex)));
      break;
    }

    // Unpack the optional initialization.
    if (auto *assign = llvh::dyn_cast<ESTree::AssignmentPatternNode>(param)) {
      param = assign->_left;
      init = assign->_right;
    }

    Identifier formalParamName = llvh::isa<ESTree::IdentifierNode>(param)
        ? getNameFieldFromID(param)
        : genAnonymousLabelName("param");

    auto *formalParam = Builder.createParameter(newFunc, formalParamName);
    createLRef(param, true)
        .emitStore(
            emitOptionalInitialization(formalParam, init, formalParamName));
  }

  // Now copy the TDZ parameters to Var declarations. This improves codegen by
  // removing tdz checks when accessing the parameters.
  assert(
      (tdzParams.empty() || hasParamExpressions) &&
      "funcNode does not have param expressions, so it doesn't"
      " need tdz params.");
  for (Variable *oldParamStorage : tdzParams) {
    auto *paramStorage = Builder.createVariable(
        newFunc->getFunctionScopeDesc(),
        Variable::DeclKind::Let,
        oldParamStorage->getName());
    constexpr bool declInit = true;
    emitStore(emitLoad(oldParamStorage), paramStorage, declInit);
    nameTable_.setInCurrentScope(oldParamStorage->getName(), paramStorage);
  }

  newFunc->setExpectedParamCountIncludingThis(
      countExpectedArgumentsIncludingThis(funcNode));
}

uint32_t ESTreeIRGen::countExpectedArgumentsIncludingThis(
    ESTree::FunctionLikeNode *funcNode) {
  // Start at 1 to account for "this".
  uint32_t count = 1;
  for (auto &param : ESTree::getParams(funcNode)) {
    if (llvh::isa<ESTree::AssignmentPatternNode>(param)) {
      // Found an initializer, stop counting expected arguments.
      break;
    }
    ++count;
  }
  return count;
}

void ESTreeIRGen::emitFunctionEpilogue(Value *returnValue) {
  if (returnValue) {
    Builder.setLocation(
        SourceErrorManager::convertEndToLocation(
            Builder.getFunction()->getSourceRange()));
    Builder.createReturnInst(returnValue);
  }

  // Delete CreateArgumentsInst if it is unused.
  if (!curFunction()->createArgumentsInst->hasUsers())
    curFunction()->createArgumentsInst->eraseFromParent();

  curFunction()->function->clearStatementCount();
}

void ESTreeIRGen::genDummyFunction(Function *dummy) {
  IRBuilder builder{dummy};

  builder.createThisParameter(dummy);
  BasicBlock *firstBlock = builder.createBasicBlock(dummy);
  builder.setInsertionBlock(firstBlock);
  builder.createUnreachableInst();
  builder.createReturnInst(builder.getLiteralUndefined());
}

/// Generate a function which immediately throws the specified SyntaxError
/// message.
Function *ESTreeIRGen::genSyntaxErrorFunction(
    Module *M,
    ScopeDesc *scopeDesc,
    Identifier originalName,
    SMRange sourceRange,
    llvh::StringRef error) {
  IRBuilder builder{M};

  Function *function = builder.createFunction(
      scopeDesc,
      originalName,
      Function::DefinitionKind::ES5Function,
      true,
      SourceVisibility::Sensitive,
      sourceRange,
      false);

  builder.createThisParameter(function);
  BasicBlock *firstBlock = builder.createBasicBlock(function);
  builder.setInsertionBlock(firstBlock);
  builder.createCreateScopeInst(scopeDesc);

  genRaiseNativeError(builder, NativeErrorTypes::SyntaxError, error);

  return function;
}

} // namespace irgen
} // namespace hermes
