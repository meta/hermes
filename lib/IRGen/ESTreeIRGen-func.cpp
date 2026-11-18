/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ESTreeIRGen.h"

#include "hermes/IR/Analysis.h"
#include "hermes/IR/IRUtils.h"
#include "hermes/IR/Instrs.h"
#include "llvh/ADT/SmallString.h"

namespace hermes {
namespace irgen {

//===----------------------------------------------------------------------===//
// FunctionContext

FunctionContext::FunctionContext(
    ESTreeIRGen *irGen,
    Function *function,
    sema::FunctionInfo *semInfo)
    : irGen_(irGen),
      semInfo_(semInfo),
      oldContext_(irGen->functionContext_),
      builderSaveState_(irGen->Builder),
      function(function) {
  if (semInfo) {
    assert(
        semInfo->getFunctionBodyScope() &&
        "all `FunctionInfo`s should have a set body scope.");
  }
  irGen->functionContext_ = this;

  // Initialize it to LiteralUndefined by default to avoid corner cases.
  this->capturedState.newTarget = irGen->Builder.getLiteralUndefined();

  if (semInfo_) {
    // Allocate the label table. Each label definition will be encountered in
    // the AST before it is referenced (because of the nature of JavaScript), at
    // which point we will initialize the GotoLabel structure with basic blocks
    // targets.
    labels_.resize(semInfo_->numLabels);
  }
}

FunctionContext::~FunctionContext() {
  irGen_->functionContext_ = oldContext_;
}

Identifier FunctionContext::genAnonymousLabelName(const llvh::Twine &hint) {
  llvh::SmallString<16> buf;
  llvh::raw_svector_ostream nameBuilder{buf};
  nameBuilder << "?anon_" << anonymousLabelCounter++ << "_" << hint;
  return function->getContext().getIdentifier(nameBuilder.str());
}

//===----------------------------------------------------------------------===//
// ESTreeIRGen

CreateScopeInst *ESTreeIRGen::makeNewScope(
    VariableScope *varScope,
    Value *parentScope) {
  auto *newScope = Builder.createCreateScopeInst(varScope, parentScope);
  curFunction()->setCurScope(newScope);
  if (Mod->getContext().getDebugInfoSetting() == DebugInfoSetting::ALL) {
    auto &envs = curFunction()->function->environments();
    auto idxOfNewScope = envs.size();
    envs.push_back(newScope);
    auto newEnvID = idxOfNewScope + Instruction::kFirstScopeCreationIdIndex;
    newScope->setEnvironmentID(newEnvID);
    curFunction()->function->setEnvironmentID(newEnvID);
  }
  return newScope;
}

/// Set the curScope of the function context, and update the Builder's current
/// scope creation ID to match the ID from \p scope.
void ESTreeIRGen::restoreScope(CreateScopeInst *scope) {
  curFunction()->setCurScope(scope);
  curFunction()->function->setEnvironmentID(scope->getEnvironmentID());
}

void ESTreeIRGen::genFunctionDeclaration(
    ESTree::FunctionDeclarationNode *func) {
  // Find the name of the function.
  auto *id = llvh::cast<ESTree::IdentifierNode>(func->_id);
  Identifier functionName = Identifier::getFromPointer(id->_name);
  LLVM_DEBUG(llvh::dbgs() << "IRGen function \"" << functionName << "\".\n");

  sema::Decl *decl = getIDDecl(id);
  if (decl->generic) {
    // Skip generics that aren't specialized.
    return;
  }

  Value *funcStorage = resolveIdentifier(id);
  Value *funcStoragePromoted = resolveIdentifierPromoted(id);
  assert(funcStorage && "Function declaration storage must have been resolved");

  auto *newFuncParentScope = curFunction()->curScope()->getVariableScope();
  Function *newFunc = func->_async ? genAsyncFunction(
                                         functionName,
                                         func,
                                         newFuncParentScope,
                                         curFunction()->capturedState)
      : func->_generator
      ? genGeneratorFunction(functionName, func, newFuncParentScope)
      : genBasicFunction(functionName, func, newFuncParentScope);

  // Store the newly created closure into a frame variable with the same name.
  auto *newClosure =
      Builder.createCreateFunctionInst(curFunction()->curScope(), newFunc);

  emitStore(newClosure, funcStorage, true);
  if (funcStoragePromoted)
    emitStore(newClosure, funcStoragePromoted, true);
}

Value *ESTreeIRGen::genFunctionExpression(
    ESTree::FunctionExpressionNode *FE,
    Identifier nameHint,
    ESTree::Node *superClassNode,
    Function::DefinitionKind functionKind,
    Variable *homeObject,
    ESTree::Node *parentNode) {
  // This is the possibly empty scope containing the function expression name.
  Function::ScopedLexicalScopeChange lexScopeChange(
      curFunction()->function, FE->getScope());
  emitScopeDeclarations(FE->getScope());

  auto *id = llvh::cast_or_null<ESTree::IdentifierNode>(FE->_id);
  Identifier originalNameIden =
      id ? Identifier::getFromPointer(id->_name) : nameHint;

  auto *parentScope = curFunction()->curScope()->getVariableScope();
  auto capturedStateForAsync = curFunction()->capturedState;
  // Update the captured state of the async function to use the homeObject we
  // were given.
  capturedStateForAsync.homeObject = homeObject;
  Function *newFunc = FE->_async ? genAsyncFunction(
                                       originalNameIden,
                                       FE,
                                       parentScope,
                                       capturedStateForAsync,
                                       parentNode)
      : FE->_generator
      ? genGeneratorFunction(
            originalNameIden, FE, parentScope, homeObject, parentNode)
      : genBasicFunction(
            originalNameIden,
            FE,
            parentScope,
            superClassNode,
            functionKind,
            homeObject,
            parentNode);

  Value *closure =
      Builder.createCreateFunctionInst(curFunction()->curScope(), newFunc);

  if (id)
    emitStore(closure, resolveIdentifier(id), true);

  return closure;
}

Value *ESTreeIRGen::genArrowFunctionExpression(
    ESTree::ArrowFunctionExpressionNode *AF,
    Identifier nameHint,
    ESTree::Node *parentNode) {
  // Check if already compiled.
  if (Value *compiled = findCompiledEntity(AF)) {
    return Builder.createCreateFunctionInst(
        curFunction()->curScope(), llvh::cast<Function>(compiled));
  }

  LLVM_DEBUG(
      llvh::dbgs()
      << "Creating arrow function. "
      << Builder.getInsertionBlock()->getParent()->getInternalName() << ".\n");

  if (AF->_async) {
    return Builder.createCreateFunctionInst(
        curFunction()->curScope(),
        genAsyncFunction(
            nameHint,
            AF,
            curFunction()->curScope()->getVariableScope(),
            curFunction()->capturedState));
  }

  auto *newFunc = genCapturingFunction(
      nameHint,
      AF,
      curFunction()->curScope()->getVariableScope(),
      curFunction()->capturedState,
      Function::DefinitionKind::ES6Arrow);

  // Emit CreateFunctionInst after we have restored the builder state.
  return Builder.createCreateFunctionInst(curFunction()->curScope(), newFunc);
}

/// Get the function range for \p functionNode, with its parent \p parentNode.
/// This is done to support lazy compilation. When restarting the compilation,
/// we need to start parsing at the identifier for the function of the class
/// method or object property method.
/// The source location for the function expression node for methods
/// starts at the first left parenthesis.
/// \param parentNode may be null.
static SMRange getFunctionRange(
    ESTree::FunctionLikeNode *functionNode,
    ESTree::Node *parentNode) {
  if (llvh::dyn_cast_or_null<ESTree::MethodDefinitionNode>(parentNode) ||
      llvh::dyn_cast_or_null<ESTree::PropertyNode>(parentNode)) {
    return parentNode->getSourceRange();
  }
  return functionNode->getSourceRange();
}

void ESTreeIRGen::setFunctionTypeFromFlow(
    Function *F,
    ESTree::FunctionLikeNode *functionNode) {
  flow::Type *flowType = flowContext_.getNodeTypeOrAny(functionNode);
  if (llvh::isa<flow::TypedFunctionType>(flowType->info))
    F->setFunctionType(flowTypeToIRType(flowType));
}

NormalFunction *ESTreeIRGen::genCapturingFunction(
    Identifier originalName,
    ESTree::FunctionLikeNode *functionNode,
    VariableScope *parentScope,
    const CapturedState &capturedState,
    Function::DefinitionKind functionKind,
    ESTree::Node *parentNode) {
  auto *newFunc = Builder.createFunction(
      originalName,
      functionKind,
      ESTree::isStrict(functionNode->strictness),
      functionNode->getSemInfo()->customDirectives,
      getFunctionRange(functionNode, parentNode));

  if (llvh::isa<flow::TypedFunctionType>(
          flowContext_.getNodeTypeOrAny(functionNode)->info)) {
    newFunc->getAttributesRef(Mod).typed = true;
    // The body of an async arrow is not a closure (CreateGeneratorInst
    // creates it); the closure is the outer function's (genAsyncFunction).
    if (functionKind != Function::DefinitionKind::GeneratorInnerArrow)
      setFunctionTypeFromFlow(newFunc, functionNode);
  }

  if (auto *body = ESTree::getBlockStatement(functionNode);
      body && body->isLazyFunctionBody) {
    setupLazyFunction(
        newFunc,
        functionNode,
        parentNode,
        body,
        parentScope,
        ExtraKey::Normal,
        capturedState);
    return newFunc;
  }
  auto compileFunc = [this,
                      newFunc,
                      functionNode,
                      legacyClsCtx = curFunction()->legacyClassContext,
                      capturedState,
                      parentScope] {
    FunctionContext newFunctionContext{
        this, newFunc, functionNode->getSemInfo()};
    newFunctionContext.legacyClassContext = legacyClsCtx;
    Function::ScopedLexicalScopeChange lexScopeChange(
        curFunction()->function,
        curFunction()->getSemInfo()->getFunctionBodyScope());

    // Propagate captured "this", "new.target" and "arguments" from parents.
    curFunction()->capturedState = capturedState;

    // A GeneratorInnerArrow is always the body of an async arrow, which
    // spawnAsync starts in a single step together with its parameters, so
    // non-simple params need no separate initialization step.
    emitFunctionPrologue(
        functionNode,
        Builder.createBasicBlock(newFunc),
        InitES5CaptureState::No,
        DoEmitDeclarations::Yes,
        parentScope);

    auto *body = ESTree::getBlockStatement(functionNode);
    assert(body && "empty function body");
    genStatement(body);
    emitFunctionEpilogue(Builder.getLiteralUndefined());
  };

  enqueueCompilation(functionNode, ExtraKey::Normal, newFunc, compileFunc);

  return newFunc;
}

NormalFunction *ESTreeIRGen::genBasicFunction(
    Identifier originalName,
    ESTree::FunctionLikeNode *functionNode,
    VariableScope *parentScope,
    ESTree::Node *superClassNode,
    Function::DefinitionKind functionKind,
    Variable *homeObject,
    ESTree::Node *parentNode) {
  assert(functionNode && "Function AST cannot be null");
  assert(
      functionKind != Function::DefinitionKind::GeneratorInnerArrow &&
      "GeneratorInnerArrow should go through genCapturingFunction");

  // Check if already compiled.
  if (Value *compiled = findCompiledEntity(functionNode))
    return llvh::cast<NormalFunction>(compiled);

  auto *body = ESTree::getBlockStatement(functionNode);
  assert(body && "body of ES5 function cannot be null");

  const bool isGeneratorInnerFunction =
      functionKind == Function::DefinitionKind::GeneratorInner;
  NormalFunction *newFunction = isGeneratorInnerFunction
      ? Builder.createFunction(
            originalName,
            Function::DefinitionKind::GeneratorInner,
            ESTree::isStrict(functionNode->strictness),
            // TODO(T84292546): change to 'Sensitive' once the outer gen fn name
            // is used in the err stack trace instead of the inner gen fn name.
            CustomDirectives{
                .sourceVisibility = SourceVisibility::HideSource,
                .alwaysInline = false},
            getFunctionRange(functionNode, parentNode),
            /* insertBefore */ nullptr)
      : (Builder.createFunction(
            originalName,
            functionKind,
            ESTree::isStrict(functionNode->strictness),
            functionNode->getSemInfo()->customDirectives,
            getFunctionRange(functionNode, parentNode),
            /* insertBefore */ nullptr));

  if (llvh::isa<flow::TypedFunctionType>(
          flowContext_.getNodeTypeOrAny(functionNode)->info)) {
    newFunction->getAttributesRef(Mod).typed = true;
    // The inner function of a generator or async function is not a closure
    // (CreateGeneratorInst creates it); the closure is the outer function's
    // (genGeneratorFunction, genAsyncFunction).
    if (!isGeneratorInnerFunction)
      setFunctionTypeFromFlow(newFunction, functionNode);
  }

  if (body->isLazyFunctionBody) {
    // This function could be a method, in which case we need to remember what
    // the homeObject was going to be.
    CapturedState lazyState{};
    lazyState.homeObject = homeObject;
    setupLazyFunction(
        newFunction,
        functionNode,
        parentNode,
        body,
        parentScope,
        ExtraKey::Normal,
        lazyState);
    return newFunction;
  }

  auto compileFunc = [this,
                      functionKind,
                      newFunction,
                      functionNode,
                      typedClassContext = curFunction()->typedClassContext,
                      legacyClassContext = curFunction()->legacyClassContext,
                      isGeneratorInnerFunction,
                      superClassNode,
                      body,
                      parentScope,
                      homeObject] {
    FunctionContext newFunctionContext{
        this, newFunction, functionNode->getSemInfo()};
    newFunctionContext.superClassNode_ = superClassNode;
    newFunctionContext.typedClassContext = typedClassContext;
    newFunctionContext.capturedState.homeObject = homeObject;
    newFunctionContext.legacyClassContext = legacyClassContext;
    Function::ScopedLexicalScopeChange lexScopeChange(
        curFunction()->function,
        curFunction()->getSemInfo()->getFunctionBodyScope());

    if (isGeneratorInnerFunction) {
      // ResumeGeneratorInst at the beginning of the function, to allow for the
      // first .next() call.
      auto *initGenBB = Builder.createBasicBlock(newFunction);
      Builder.setInsertionBlock(initGenBB);
      auto *prologueBB = Builder.createBasicBlock(newFunction);
      auto *prologueResumeIsReturn = Builder.createAllocStackInst(
          genAnonymousLabelName("isReturn_prologue"), Type::createBoolean());
      genResumeGenerator(GenFinally::No, prologueResumeIsReturn, prologueBB);

      if (hasSimpleParams(functionNode) || ESTree::isAsync(functionNode)) {
        // If there are simple params, then we don't need an extra
        // yield/resume. They can simply be initialized on the first call to
        // `.next`. The same holds for the body of an async function, because
        // spawnAsync starts it in a single step that reports errors from
        // parameter initialization and from the body alike.
        Builder.setInsertionBlock(prologueBB);
        emitFunctionPrologue(
            functionNode,
            prologueBB,
            InitES5CaptureState::Yes,
            DoEmitDeclarations::Yes,
            parentScope);
      } else {
        // If there are non-simple params, then we must add a new
        // yield/resume. The `.next()` call will occur once in the outer
        // function, before the iterator is returned to the caller of the
        // `function*`.
        auto *entryPointBB = Builder.createBasicBlock(newFunction);
        auto *entryPointResumeIsReturn = Builder.createAllocStackInst(
            genAnonymousLabelName("isReturn_entry"), Type::createBoolean());

        // Initialize parameters.
        Builder.setInsertionBlock(prologueBB);
        emitFunctionPrologue(
            functionNode,
            prologueBB,
            InitES5CaptureState::Yes,
            DoEmitDeclarations::Yes,
            parentScope);
        Builder.createSaveAndYieldInst(
            Builder.getLiteralUndefined(),
            Builder.getLiteralBool(false),
            entryPointBB);

        // Actual entry point of function from the caller's perspective.
        Builder.setInsertionBlock(entryPointBB);
        genResumeGenerator(
            GenFinally::No,
            entryPointResumeIsReturn,
            Builder.createBasicBlock(newFunction));
      }
    } else {
      emitFunctionPrologue(
          functionNode,
          Builder.createBasicBlock(newFunction),
          InitES5CaptureState::Yes,
          DoEmitDeclarations::No,
          parentScope);
      if (curFunction()->hasLegacyClassContext()) {
        if (functionKind == Function::DefinitionKind::ES6BaseConstructor) {
          // Initialize `this` for the function.
          emitLegacyBaseClassThisInit();
          // We only need to generate this here for base classes. It's not
          // required for derived because they generate this call after calling
          // super().
          emitLegacyInstanceElementsInitCall();
        } else if (
            functionKind == Function::DefinitionKind::ES6DerivedConstructor) {
          // Initialize the 'checked this' in derived class constructors.
          newFunctionContext.capturedState.thisVal = Builder.createVariable(
              curFunction()->curScope()->getVariableScope(),
              Builder.createIdentifier("?CHECKED_this"),
              getTypeContext().unionTy(
                  Type::createObject(), Type::createEmpty()),
              true);
          Builder.createStoreFrameInst(
              curFunction()->curScope(),
              Builder.getLiteralEmpty(),
              newFunctionContext.capturedState.thisVal);
        }
      }
      emitFunctionDeclarations(functionNode);
      if (functionKind == Function::DefinitionKind::ES6BaseConstructor &&
          curFunction()->hasTypedClassContext()) {
        // We're compiling a typed base class constructor, emit the field inits
        // at the start.
        emitTypedFieldInitCall(typedClassContext.type);
      }
    }

    genStatement(body);
    if (functionNode->getSemInfo()->mayReachImplicitReturn) {
      emitFunctionEpilogue(Builder.getLiteralUndefined());
    } else {
      // Don't implicitly return any value.
      emitFunctionEpilogue(nullptr);
    }
  };

  enqueueCompilation(functionNode, ExtraKey::Normal, newFunction, compileFunc);

  return newFunction;
}

Function *ESTreeIRGen::genGeneratorFunction(
    Identifier originalName,
    ESTree::FunctionLikeNode *functionNode,
    VariableScope *parentScope,
    Variable *homeObject,
    ESTree::Node *parentNode) {
  assert(functionNode && "Function AST cannot be null");

  if (Value *compiled =
          findCompiledEntity(functionNode, ExtraKey::GeneratorOuter))
    return llvh::cast<Function>(compiled);

  if (!Builder.getModule()->getContext().isGeneratorEnabled()) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        functionNode->getSourceRange(), "generator compilation is disabled");
  }

  // Build the outer function which creates the generator.
  // Does not have an associated source range.
  auto *outerFn = Builder.createGeneratorFunction(
      originalName,
      Function::DefinitionKind::ES5Function,
      ESTree::isStrict(functionNode->strictness),
      functionNode->getSemInfo()->customDirectives,
      getFunctionRange(functionNode, parentNode),
      /* insertBefore */ nullptr);
  setFunctionTypeFromFlow(outerFn, functionNode);

  auto *body = ESTree::getBlockStatement(functionNode);
  if (body->isLazyFunctionBody) {
    // This function could be a method, in which case we need to remember what
    // the homeObject was going to be.
    CapturedState lazyState{};
    lazyState.homeObject = homeObject;
    setupLazyFunction(
        outerFn,
        functionNode,
        parentNode,
        body,
        parentScope,
        ExtraKey::GeneratorOuter,
        lazyState);
    return outerFn;
  }

  auto compileFunc =
      [this, outerFn, functionNode, originalName, parentScope, homeObject]() {
        FunctionContext outerFnContext{
            this, outerFn, functionNode->getSemInfo()};
        Function::ScopedLexicalScopeChange lexScopeChange(
            curFunction()->function,
            curFunction()->getSemInfo()->getFunctionBodyScope());

        // We pass InitES5CaptureState::No to emitFunctionPrologue because
        // generator functions do not create a scope and so we shouldn't be
        // trying to capture state like 'this' into its nonexistent scope. If
        // there is an arrow function inside the inner generator function which
        // needs access to captured state, then a later lowering pass is
        // responsible for lifting out instructions like CreateArguments. That
        // lowering pass also will create the required scopes.
        emitFunctionPrologue(
            functionNode,
            Builder.createBasicBlock(outerFn),
            InitES5CaptureState::No,
            DoEmitDeclarations::No,
            parentScope);

        assert(
            !llvh::isa<ESTree::ArrowFunctionExpressionNode>(functionNode) &&
            "generator arrow functions are not in the language");

        // Here we pass the homeObject because we want the actual user code to
        // be able to use the super that the outer generator had access to.
        CapturedState innerState{};
        innerState.homeObject = homeObject;
        auto *gen = emitCreateGenerator(
            originalName.isValid() ? originalName : genAnonymousLabelName(""),
            functionNode,
            parentScope,
            innerState);

        if (!hasSimpleParams(functionNode)) {
          // If there are non-simple params, step the inner function once to
          // initialize them.
          Value *next = Builder.createLoadPropertyInst(gen, "next");
          Builder.createCallInst(
              next, /* newTarget */ Builder.getLiteralUndefined(), gen, {});
        }

        emitFunctionEpilogue(gen);
      };

  enqueueCompilation(
      functionNode, ExtraKey::GeneratorOuter, outerFn, compileFunc);

  return outerFn;
}

CreateGeneratorInst *ESTreeIRGen::emitCreateGenerator(
    Identifier innerName,
    ESTree::FunctionLikeNode *functionNode,
    VariableScope *parentScope,
    const CapturedState &capturedState) {
  NormalFunction *innerFn;
  if (llvh::isa<ESTree::ArrowFunctionExpressionNode>(functionNode)) {
    assert(
        ESTree::isAsync(functionNode) &&
        "only async arrows are compiled to generators");
    // The body of an async arrow forwards the captured state of the parent.
    innerFn = genCapturingFunction(
        innerName,
        functionNode,
        parentScope,
        capturedState,
        Function::DefinitionKind::GeneratorInnerArrow);
  } else {
    innerFn = genBasicFunction(
        innerName,
        functionNode,
        parentScope,
        /* superClassNode */ nullptr,
        Function::DefinitionKind::GeneratorInner,
        capturedState.homeObject);
  }

  // If the original AST node was async, this inner generator is driven by
  // spawnAsync (which handles raw yields). Otherwise, if it's an async
  // generator, the driver is AsyncGenerator which needs OverloadYield.
  innerFn->setIsFromAsyncFunction(ESTree::isAsync(functionNode));

  // The outer function does not create its own scope, so the inner function
  // is a child of the parent's scope.
  GetParentScopeInst *parentScopeInst = Builder.createGetParentScopeInst(
      parentScope, curFunction()->function->getParentScopeParam());
  // Create a generator object, which will store the arguments.
  return Builder.createCreateGeneratorInst(parentScopeInst, innerFn);
}

Function *ESTreeIRGen::genAsyncFunction(
    Identifier originalName,
    ESTree::FunctionLikeNode *functionNode,
    VariableScope *parentScope,
    const CapturedState &capturedState,
    ESTree::Node *parentNode) {
  assert(functionNode && "Function AST cannot be null");

  if (auto *compiled = findCompiledEntity(functionNode, ExtraKey::AsyncOuter))
    return llvh::cast<Function>(compiled);

  if (!Builder.getModule()->getContext().isGeneratorEnabled()) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        functionNode->getSourceRange(),
        "async function compilation requires enabling generator");
  }

  auto *asyncFn = Builder.createAsyncFunction(
      originalName,
      Function::DefinitionKind::ES5Function,
      ESTree::isStrict(functionNode->strictness),
      functionNode->getSemInfo()->customDirectives,
      getFunctionRange(functionNode, parentNode),
      /* insertBefore */ nullptr);
  setFunctionTypeFromFlow(asyncFn, functionNode);

  auto *body = ESTree::getBlockStatement(functionNode);
  if (body->isLazyFunctionBody) {
    setupLazyFunction(
        asyncFn,
        functionNode,
        parentNode,
        body,
        parentScope,
        ExtraKey::AsyncOuter,
        capturedState);
    return asyncFn;
  }

  auto compileFunc = [this,
                      asyncFn,
                      functionNode,
                      originalName,
                      parentScope,
                      capturedState]() {
    FunctionContext asyncFnContext{this, asyncFn, functionNode->getSemInfo()};
    Function::ScopedLexicalScopeChange lexScopeChange(
        curFunction()->function,
        curFunction()->getSemInfo()->getFunctionBodyScope());

    // Like the outer function of a generator, the async function creates no
    // scope and captures no state: the whole body, including the parameters,
    // is in the inner generator, which receives this function's `this` and
    // arguments through CreateGenerator.
    emitFunctionPrologue(
        functionNode,
        Builder.createBasicBlock(asyncFn),
        InitES5CaptureState::No,
        DoEmitDeclarations::No,
        parentScope);

    auto *gen = emitCreateGenerator(
        genAnonymousLabelName(originalName.isValid() ? originalName.str() : ""),
        functionNode,
        parentScope,
        capturedState);

    auto *spawnAsyncClosure = Builder.createGetBuiltinClosureInst(
        BuiltinMethod::HermesBuiltin_spawnAsync);

    auto *res = Builder.createCallInst(
        spawnAsyncClosure,
        /* newTarget */ Builder.getLiteralUndefined(),
        /* thisValue */ Builder.getLiteralUndefined(),
        {gen});

    emitFunctionEpilogue(res);
  };

  enqueueCompilation(functionNode, ExtraKey::AsyncOuter, asyncFn, compileFunc);

  return asyncFn;
}

void ESTreeIRGen::initCaptureStateInES5FunctionHelper() {
  auto defKind = curFunction()->function->getDefinitionKind();
  bool isLegacyClassConstructor = curFunction()->hasLegacyClassContext() &&
      (defKind == Function::DefinitionKind::ES6DerivedConstructor ||
       defKind == Function::DefinitionKind::ES6BaseConstructor);
  // Capture "this", "new.target" and "arguments" if there are inner arrows.
  // Also capture state in class constructors if debugging is enabled. We do
  // this because we have to generate the eval function as an arrow function in
  // order to correctly look up `this`, and that arrow function will require the
  // full captured state.
  if (!(curFunction()->getSemInfo()->containsArrowFunctions ||
        ((Mod->getContext().getDebugInfoSetting() == DebugInfoSetting::ALL) &&
         isLegacyClassConstructor)))
    return;

  auto *scope = curFunction()->curScope()->getVariableScope();

  // `this` is managed separately in the case of a legacy class constructor.
  if (!isLegacyClassConstructor) {
    auto *th = Builder.createVariable(
        scope,
        genAnonymousLabelName("this"),
        Type::createAnyType(),
        /* hidden */ true);
    curFunction()->capturedState.thisVal = th;
    emitStore(curFunction()->jsParams[0], th, true);
  }

  Value *newTarget = genNewTarget();
  // "new.target".
  Variable *capturedNewTarget = Builder.createVariable(
      scope,
      genAnonymousLabelName("new.target"),
      curFunction()->function->getNewTargetParam()->getType(),
      /* hidden */ true);
  curFunction()->capturedState.newTarget = capturedNewTarget;
  emitStore(newTarget, curFunction()->capturedState.newTarget, true);

  // "arguments".
  if (curFunction()->getSemInfo()->containsArrowFunctionsUsingArguments) {
    auto *args = Builder.createVariable(
        scope,
        genAnonymousLabelName("arguments"),
        Type::createObject(),
        /* hidden */ true);
    curFunction()->capturedState.arguments = args;
    emitStore(curFunction()->createArgumentsInst, args, true);
  }
}

void ESTreeIRGen::emitFunctionPrologue(
    ESTree::FunctionLikeNode *funcNode,
    BasicBlock *entry,
    InitES5CaptureState doInitES5CaptureState,
    DoEmitDeclarations doEmitDeclarations,
    VariableScope *parentScope) {
  auto *newFunc = curFunction()->function;
  auto *semInfo = curFunction()->getSemInfo();

  Builder.setLocation(newFunc->getSourceRange().Start);

  // Start pumping instructions into the entry basic block.
  Builder.setInsertionBlock(entry);

  // Always insert a CreateArgumentsInst. We will delete it later if it is
  // unused.
  curFunction()->createArgumentsInst = newFunc->isStrictMode()
      ? (CreateArgumentsInst *)Builder.createCreateArgumentsStrictInst()
      : Builder.createCreateArgumentsLooseInst();

  // If "arguments" is declared in the current function, bind it to its value.
  if (semInfo->argumentsDecl.hasValue()) {
    setDeclData(
        semInfo->argumentsDecl.getValue(), curFunction()->createArgumentsInst);
  }

  // Always create the "this" parameter. It needs to be created before we
  // initialized the ES5 capture state.
  JSDynamicParam *thisParam = Builder.createJSThisParam(newFunc);
  if (flow::TypedFunctionType *ftype = llvh::dyn_cast<flow::TypedFunctionType>(
          flowContext_.getNodeTypeOrAny(funcNode)->info);
      ftype && ftype->getThisParam()) {
    thisParam->setType(flowTypeToIRType(ftype->getThisParam()));
  } else if (
      flow::ClassType *classInfo = curFunction()->typedClassContext.type) {
    // Field initializers have no FunctionLikeNode, but the 'this' type is
    // derived from the class. The static initializer's 'this' is the class
    // constructor object rather than an instance, but both lower to the same
    // IR type, so the class is usable for either.
    thisParam->setType(flowTypeToIRType(classInfo));
  }

  // Save the "this" parameter. We will delete it later if unused.
  // In strict mode just use param 0 directly. In non-strict, we must coerce
  // it to an object.
  {
    Instruction *thisVal = Builder.createLoadParamInst(thisParam);
    assert(
        curFunction()->jsParams.empty() &&
        "jsParams must be empty in new function");
    curFunction()->jsParams.push_back(
        newFunc->isStrictMode() ? thisVal
                                : Builder.createCoerceThisNSInst(thisVal));
  }

  // Create the function level scope for this function. If a parent scope is
  // provided, use it, otherwise, this function does not have a lexical parent.
  Value *baseScope;
  if (parentScope) {
    baseScope = Builder.createGetParentScopeInst(
        parentScope, newFunc->getParentScopeParam());
  } else {
    baseScope = Builder.getEmptySentinel();
  }
  // GeneratorFunctions and AsyncFunctions should not have a scope created. It
  // will be created later during a lowering pass.
  if (!llvh::isa<GeneratorFunction>(curFunction()->function) &&
      !llvh::isa<AsyncFunction>(curFunction()->function)) {
    makeNewScope(Builder.createVariableScope(parentScope), baseScope);
  }

  if (doInitES5CaptureState != InitES5CaptureState::No)
    initCaptureStateInES5FunctionHelper();

  // Construct the parameter list. Create function parameters and register
  // them in the scope.
  newFunc->setExpectedParamCountIncludingThis(
      countExpectedArgumentsIncludingThis(funcNode));

  if (doEmitDeclarations == DoEmitDeclarations::No)
    return;
  emitFunctionDeclarations(funcNode);
}

void ESTreeIRGen::emitFunctionDeclarations(ESTree::FunctionLikeNode *funcNode) {
  auto *semInfo = curFunction()->getSemInfo();
  if (funcNode)
    emitParameters(funcNode);
  emitScopeDeclarations(semInfo->getFunctionBodyScope());
  // Generate the code for import declarations before generating the rest of the
  // body.
  for (auto importDecl : semInfo->imports) {
    genImportDeclaration(importDecl);
  }
}

void ESTreeIRGen::emitScopeDeclarations(sema::LexicalScope *scope) {
  if (!scope)
    return;
  setLexicalScopeData(scope, curFunction()->curScope()->getVariableScope());

  bool tdz = Mod->getContext().getCodeGenerationSettings().enableTDZ;
  for (sema::Decl *decl : scope->decls) {
    if (decl->generic) {
      // Skip generics that aren't specialized.
      continue;
    }

    Variable *var = nullptr;
    bool init = false;
    switch (decl->kind) {
      case sema::Decl::Kind::Let:
      case sema::Decl::Kind::Const:
      case sema::Decl::Kind::Class:
      case sema::Decl::Kind::TypedBuiltin:
      case sema::Decl::Kind::Catch:
        assert(
            ((curFunction()->debugAllowRecompileCounter != 0) ||
             (decl->customData == nullptr)) &&
            "customData can be bound only if recompiling AST");

        if (!decl->customData) {
          var = Builder.createVariable(
              curFunction()->curScope()->getVariableScope(),
              decl->name,
              tdz ? getTypeContext().unionTy(
                        Type::createAnyType(), Type::createEmpty())
                  : Type::createAnyType(),
              false);
          var->setObeysTDZ(tdz);
          var->setConstness(sema::Decl::getKindConstness(decl->kind));
          setDeclData(decl, var);
        } else {
          var = llvh::cast<Variable>(getDeclData(decl));
        }
        init = true;
        break;

      case sema::Decl::Kind::Var:
        // 'arguments' must have already been bound in the function prologue.
        if (decl->special == sema::Decl::Special::Arguments) {
          assert(
              decl->customData &&
              "'arguments', if it exists, must be bound in the function prologue");
          continue;
        }
        [[fallthrough]];
      case sema::Decl::Kind::Import:
      case sema::Decl::Kind::ES5Catch:
      case sema::Decl::Kind::FunctionExprName:
      case sema::Decl::Kind::ClassExprName:
      case sema::Decl::Kind::ScopedFunction: {
        // NOTE: we are overwriting the decl's customData, even if it is already
        // set. Ordinarily we shouldn't be evaluating the same declarations
        // twice, except when we are compiling the body of a "finally" handler.
        assert(
            ((curFunction()->debugAllowRecompileCounter != 0) ||
             (decl->customData == nullptr)) &&
            "customData can be bound only if recompiling AST");

        auto isClsExpr = decl->kind == sema::Decl::Kind::ClassExprName;
        if (!decl->customData) {
          var = Builder.createVariable(
              curFunction()->curScope()->getVariableScope(),
              decl->name,
              (isClsExpr && tdz)
                  ? getTypeContext().unionTy(
                        Type::createAnyType(), Type::createEmpty())
                  : Type::createAnyType(),
              // FunctionExprName isn't supposed to show up in the list when
              // debugging.
              /* hidden */ decl->kind == sema::Decl::Kind::FunctionExprName);
          var->setConstness(sema::Decl::getKindConstness(decl->kind));
          if (isClsExpr) {
            var->setObeysTDZ(tdz);
          }
          setDeclData(decl, var);
        } else {
          var = llvh::cast<Variable>(getDeclData(decl));
        }
        // Var declarations and class expression names must be initialized to
        // undefined at the beginning of the scope.
        init = (decl->kind == sema::Decl::Kind::Var || isClsExpr);
        break;
      }

      case sema::Decl::Kind::PrivateField:
      case sema::Decl::Kind::PrivateMethod:
      case sema::Decl::Kind::PrivateGetter:
      case sema::Decl::Kind::PrivateSetter:
      case sema::Decl::Kind::PrivateGetterSetter:
        // Private names are handled separately.
      case sema::Decl::Kind::Parameter:
        // Skip parameters, they are handled separately.
        continue;

      case sema::Decl::Kind::GlobalProperty:
      case sema::Decl::Kind::UndeclaredGlobalProperty: {
        assert(
            ((curFunction()->debugAllowRecompileCounter != 0) ||
             (decl->customData == nullptr)) &&
            "customData can be bound only if recompiling AST");

        if (!decl->customData) {
          bool declared = decl->kind == sema::Decl::Kind::GlobalProperty;
          auto *prop = Builder.createGlobalObjectProperty(decl->name, declared);
          setDeclData(decl, prop);
          if (declared)
            Builder.createDeclareGlobalVarInst(prop->getName());
        }
      } break;
    }

    if (init) {
      assert(var);
      Value *initValue = var->getObeysTDZ()
          ? (Value *)Builder.getLiteralEmpty()
          : (Value *)Builder.getLiteralUndefined();
      // With parameter expressions, a body "var" that shares a parameter's name
      // is a separate binding initialized from the parameter (ES2026 10.2.11,
      // step 30.e.i.4).
      sema::FunctionInfo *funcInfo = scope->parentFunction;
      sema::LexicalScope *separateParamScope =
          funcInfo->hasParameterExpressions &&
              scope == funcInfo->getFunctionBodyScope()
          ? funcInfo->getParameterScope()
          : nullptr;
      if (separateParamScope && decl->kind == sema::Decl::Kind::Var) {
        for (sema::Decl *param : separateParamScope->decls) {
          if (param->kind != sema::Decl::Kind::Parameter)
            break;
          if (param->name == decl->name) {
            initValue = emitLoad(getDeclData(param), false);
            break;
          }
        }
      }
      Builder.createStoreFrameInst(curFunction()->curScope(), initValue, var);
    }

    // A typed class's static elements and non-virtual methods live in
    // Variables in this scope. Default them at the top of the scope so that
    // early reads don't violate compiler invariants.
    if (decl->kind == sema::Decl::Kind::Class)
      declareTypedClassMemberVars(decl);
  }

  // Generate and initialize the code for the hoisted function declarations
  // before generating the rest of the body.
  for (auto *funcDecl : scope->hoistedFunctions) {
    emitHoistedFunctionDeclaration(scope, funcDecl);
  }
}

void ESTreeIRGen::emitLazyGlobalDeclarations(sema::LexicalScope *globalScope) {
  // Iterate in reverse order because we want to emit the most recently declared
  // global vars.
  for (auto *decl : llvh::reverse(globalScope->decls)) {
    // Found the most recently emitted global declaration.
    // Everything at lower index has been emitted before.
    if (decl->customData)
      return;

    assert(
        decl->kind == sema::Decl::Kind::UndeclaredGlobalProperty &&
        "Lazy functions can't declare variables in the global scope");
    auto *prop =
        Builder.createGlobalObjectProperty(decl->name, /* declared */ false);
    setDeclData(decl, prop);
  }
}

void ESTreeIRGen::emitHoistedFunctionDeclaration(
    sema::LexicalScope *scope,
    ESTree::FunctionDeclarationNode *funcDecl) {
  genFunctionDeclaration(funcDecl);

  if (!funcDecl->_id)
    return;

  sema::Decl *decl =
      getIDDecl(llvh::cast<ESTree::IdentifierNode>(funcDecl->_id));

  if (decl->generic) {
    // Skip generics that aren't specialized.
    return;
  }

  // Function-level var-scoped functions may have a previous store of
  // 'undefined', which is now dead. If this isn't a function-level scope, don't
  // try to delete anything.
  if (scope->parentFunction->getFunctionBodyScope() != scope)
    return;

  Value *storage = getDeclData(decl);

  // This cleanup is only focused on the case where there are two stores,
  // one of which we'll clean up.
  if (storage->getNumUsers() != 2)
    return;

  assert(
      storage->getUsers()[0]->getParent() ==
          storage->getUsers()[1]->getParent() &&
      "both stores must be in the same block");

  IRBuilder::InstructionDestroyer destroyer{};
  for (auto *user : storage->getUsers()) {
    if (auto *store = llvh::dyn_cast<StoreFrameInst>(user); store &&
        store->getVariable() == storage &&
        llvh::isa<LiteralUndefined>(store->getValue())) {
      // This is a dead 'undefined' store that we can clean up
      // because we know there's another store.
      destroyer.add(store);
      break;
    }
  }
}

void ESTreeIRGen::emitParameters(ESTree::FunctionLikeNode *funcNode) {
  auto *newFunc = curFunction()->function;
  sema::FunctionInfo *semInfo = funcNode->getSemInfo();
  Function::ScopedLexicalScopeChange lexScopeChange(
      curFunction()->function, semInfo->getParameterScope());

  LLVM_DEBUG(llvh::dbgs() << "IRGen function parameters.\n");

  // Create a variable for every parameter.
  for (sema::Decl *decl : semInfo->getParameterScope()->decls) {
    if (decl->kind != sema::Decl::Kind::Parameter)
      break;
    // Skip the 'this' parameter, because it's implicit.
    if (decl->name.getUnderlyingPointer() == kw_.identThis)
      continue;

    LLVM_DEBUG(llvh::dbgs() << "Adding parameter: " << decl->name << "\n");

    // If not simple parameter list, enable TDZ and init every param.
    bool tdz = !semInfo->simpleParameterList &&
        Mod->getContext().getCodeGenerationSettings().enableTDZ;
    Variable *var = Builder.createVariable(
        curFunction()->curScope()->getVariableScope(),
        decl->name,
        tdz ? getTypeContext().unionTy(
                  Type::createAnyType(), Type::createEmpty())
            : Type::createAnyType(),
        /* hidden */ false);
    setDeclData(decl, var);

    // If not simple parameter list, enable TDZ and init every param.
    if (!semInfo->simpleParameterList) {
      var->setObeysTDZ(tdz);
      Builder.createStoreFrameInst(
          curFunction()->curScope(),
          tdz ? (Literal *)Builder.getLiteralEmpty()
              : (Literal *)Builder.getLiteralUndefined(),
          var);
    }
  }

  uint32_t paramIndex = uint32_t{0} - 1;
  for (auto &elem : ESTree::getParams(funcNode)) {
    ESTree::Node *param = &elem;
    ESTree::Node *init = nullptr;

    // Skip the typed 'this' parameter if it exists.
    // Don't increment paramIndex because 'this' doesn't count.
    if (auto *idNode = llvh::dyn_cast<ESTree::IdentifierNode>(param);
        idNode && idNode->_name == kw_.identThis) {
      continue;
    }

    ++paramIndex;

    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(param)) {
      // For typed rest params declared as Array<T>, use the FastArray-
      // returning variant so subsequent FastArray ops work on the result.
      BuiltinMethod::Enum builtin = BuiltinMethod::HermesBuiltin_copyRestArgs;
      if (auto *ftype = llvh::dyn_cast<flow::TypedFunctionType>(
              flowContext_.getNodeTypeOrAny(funcNode)->info);
          ftype && paramIndex < ftype->getParams().size() &&
          flowContext_.isArrayClassType(ftype->getParams()[paramIndex].type)) {
        builtin = BuiltinMethod::HermesBuiltin_copyRestArgsFast;
      }
      createLRef(rest->_argument, true)
          .emitStore(
              genBuiltinCall(builtin, Builder.getLiteralNumber(paramIndex)));
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

    size_t jsParamIndex = newFunc->getJSDynamicParams().size();
    if (jsParamIndex > UINT32_MAX) {
      Mod->getContext().getSourceErrorManager().error(
          param->getSourceRange(), "too many parameters");
      break;
    }
    auto *jsParam = Builder.createJSDynamicParam(newFunc, formalParamName);
    if (flow::TypedFunctionType *ftype =
            llvh::dyn_cast<flow::TypedFunctionType>(
                flowContext_.getNodeTypeOrAny(funcNode)->info);
        ftype && paramIndex < ftype->getParams().size()) {
      Type irType = flowTypeToIRType(ftype->getParams()[paramIndex].type);
      // Optional parameters can receive undefined when omitted.
      if (ftype->getParams()[paramIndex].optional)
        irType = getTypeContext().unionTy(irType, Type::createUndefined());
      jsParam->setType(irType);
    }
    Instruction *formalParam = Builder.createLoadParamInst(jsParam);
    curFunction()->jsParams.push_back(formalParam);
    createLRef(param, true)
        .emitStore(
            emitOptionalInitialization(formalParam, init, formalParamName));
  }
}

uint32_t ESTreeIRGen::countExpectedArgumentsIncludingThis(
    ESTree::FunctionLikeNode *funcNode) {
  // Start at 1 to account for "this".
  uint32_t count = 1;
  // Implicit functions, whose funcNode is null, take no arguments.
  if (funcNode) {
    for (auto &param : ESTree::getParams(funcNode)) {
      // Skip the typed 'this' parameter - it's not a real argument.
      if (auto *idNode = llvh::dyn_cast<ESTree::IdentifierNode>(&param)) {
        if (idNode->_name == kw_.identThis) {
          continue;
        }
      }
      if (llvh::isa<ESTree::AssignmentPatternNode>(param) ||
          llvh::isa<ESTree::RestElementNode>(param)) {
        // Found an initializer or a rest parameter, stop counting expected
        // arguments.
        break;
      }
      ++count;
    }
  }
  return count;
}

void ESTreeIRGen::emitFunctionEpilogue(Value *returnValue) {
  Builder.setLocation(
      SourceErrorManager::convertEndToLocation(
          Builder.getFunction()->getSourceRange()));
  if (returnValue) {
    if (curFunction()->hasLegacyClassContext()) {
      if (curFunction()->getSemInfo()->constructorKind ==
          sema::FunctionInfo::ConstructorKind::Derived) {
        returnValue = genLegacyDerivedConstructorRet(nullptr, returnValue);
      } else if (
          curFunction()->getSemInfo()->constructorKind ==
          sema::FunctionInfo::ConstructorKind::Base) {
        returnValue = genLegacyBaseConstructorRet(nullptr, returnValue);
      }
    }
    Builder.createReturnInst(returnValue);
  } else {
    Builder.createUnreachableInst();
  }

  // Delete CreateArgumentsInst if it is unused.
  if (!curFunction()->createArgumentsInst->hasUsers())
    curFunction()->createArgumentsInst->eraseFromParent();

  // Delete the load of "this" if unused.
  if (!curFunction()->jsParams.empty()) {
    Instruction *I = curFunction()->jsParams[0];
    if (!I->hasUsers()) {
      // If the instruction is CoerceThisNSInst, we may have to delete its
      // operand too.
      Instruction *load = nullptr;
      if (auto *CT = llvh::dyn_cast<CoerceThisNSInst>(I)) {
        load = llvh::dyn_cast<Instruction>(CT->getSingleOperand());
      }
      I->eraseFromParent();
      if (load && !load->hasUsers())
        load->eraseFromParent();
    }
  }

  onCompiledFunction(curFunction()->function);
}

Function *ESTreeIRGen::genFieldInitFunction(bool staticElements) {
  const auto &typedClassContext = curFunction()->typedClassContext;
  ESTree::ClassDeclarationNode *classNode = typedClassContext.node;
  auto *classDecoration =
      ESTree::getDecoration<ESTree::ClassLikeDecoration>(classNode);
  sema::FunctionInfo *initFuncInfo = staticElements
      ? classDecoration->staticElementsInitFunctionInfo
      : classDecoration->instanceElementsInitFunctionInfo;
  if (initFuncInfo == nullptr) {
    return nullptr;
  }

  // Check if already compiled (e.g., when recompiling a finally block).
  ExtraKey extraKey = staticElements
      ? ExtraKey::ImplicitStaticElementsInitializer
      : ExtraKey::ImplicitFieldInitializer;
  if (Value *compiled = findCompiledEntity(classNode, extraKey))
    return llvh::cast<Function>(compiled);

  llvh::StringRef namePrefix = staticElements
      ? "<static_elements_initializer:"
      : "<instance_members_initializer:";
  auto initFunc = llvh::cast<Function>(Builder.createFunction(
      (llvh::Twine(namePrefix) + typedClassContext.type->getClassName().str() +
       ">")
          .str(),
      Function::DefinitionKind::ES5Function,
      true /*strictMode*/));

  auto compileFunc = [this,
                      initFunc,
                      initFuncInfo,
                      typedClassContext,
                      staticElements,
                      parentScope =
                          curFunction()->curScope()->getVariableScope()] {
    FunctionContext newFunctionContext{this, initFunc, initFuncInfo};
    newFunctionContext.typedClassContext = typedClassContext;
    Function::ScopedLexicalScopeChange lexScopeChange(
        curFunction()->function,
        curFunction()->getSemInfo()->getFunctionBodyScope());

    auto *prologueBB = Builder.createBasicBlock(initFunc);
    Builder.setInsertionBlock(prologueBB);

    // Field initializers may contain arrow functions, which need the captured
    // state to resolve 'this'.
    emitFunctionPrologue(
        nullptr,
        prologueBB,
        InitES5CaptureState::Yes,
        DoEmitDeclarations::No,
        parentScope);

    auto *classBody =
        llvh::cast<ESTree::ClassBodyNode>(typedClassContext.node->_body);
    for (ESTree::Node &it : classBody->_body) {
      ESTree::Node *key;
      ESTree::Node *init;
      bool isStatic;
      bool isPrivate;
      if (auto *prop = llvh::dyn_cast<ESTree::ClassPropertyNode>(&it)) {
        key = prop->_key;
        init = prop->_value;
        isStatic = prop->_static;
        isPrivate = false;
      } else if (
          auto *prop = llvh::dyn_cast<ESTree::ClassPrivatePropertyNode>(&it)) {
        key = prop->_key;
        init = prop->_value;
        isStatic = prop->_static;
        // The key is a bare IdentifierNode, so privateness must be stated
        // explicitly.
        isPrivate = true;
      } else {
        continue;
      }

      if (isStatic != staticElements || !init)
        continue;

      Value *value = genExpression(init);
      if (staticElements) {
        auto *id = llvh::cast<ESTree::IdentifierNode>(key);
        sema::Decl *decl = semCtx_.getExpressionDecl(id);
        emitStore(value, getDeclData(decl), false);
      } else {
        emitTypedFieldStore(
            typedClassContext.type,
            getNameFieldFromID(key),
            isPrivate,
            genThisExpression(),
            value);
      }
    }

    emitFunctionEpilogue(Builder.getLiteralUndefined());
    initFunc->setReturnType(Type::createUndefined());
  };
  enqueueCompilation(classNode, extraKey, initFunc, compileFunc);
  return initFunc;
}

void ESTreeIRGen::emitCreateTypedFieldInitFunction() {
  Function *initFunc = genFieldInitFunction(/* staticElements */ false);
  if (initFunc == nullptr) {
    return;
  }

  auto *classType = curFunction()->typedClassContext.type;
  ClassFieldInitInfo &classInfo = classFieldInitInfo_[classType];

  classInfo.fieldInitFunction = initFunc;

  CreateFunctionInst *createFieldInitFunc =
      Builder.createCreateFunctionInst(curFunction()->curScope(), initFunc);
  // Reuse the Variable when recompiling (e.g. a class inside a finally block).
  // The constructor is only compiled once, against the first Variable, so
  // creating a second one here would leave it loading undefined.
  if (!classInfo.fieldInitFunctionVar) {
    classInfo.fieldInitFunctionVar = Builder.createVariable(
        curFunction()->curScope()->getVariableScope(),
        (llvh::Twine("<fieldInitFuncVar:") + classType->getClassName().str() +
         ">"),
        Type::createObject(),
        /* hidden */ true);
  }
  Variable *fieldInitFuncVar = classInfo.fieldInitFunctionVar;
  Builder.createStoreFrameInst(
      emitResolveScopeInstIfNeeded(fieldInitFuncVar->getParent()),
      createFieldInitFunc,
      fieldInitFuncVar);
}

void ESTreeIRGen::emitTypedFieldInitCall(flow::ClassType *classType) {
  auto iter = classFieldInitInfo_.find(classType);
  if (iter == classFieldInitInfo_.end()) {
    return;
  }
  Function *fieldInitFunc = iter->second.fieldInitFunction;
  Variable *fieldInitFuncVar = iter->second.fieldInitFunctionVar;
  assert(
      fieldInitFuncVar &&
      "If entry is in classFieldInitInfo_, var should be set");
  auto *scope = emitResolveScopeInstIfNeeded(fieldInitFuncVar->getParent());
  Value *funcVal = Builder.createLoadFrameInst(scope, fieldInitFuncVar);

  funcVal->setType(Type::createObject());
  Builder
      .createCallInst(
          funcVal,
          fieldInitFunc,
          /* calleeIsAlwaysClosure */ true,
          Builder.getEmptySentinel(),
          /*newTarget*/ Builder.getLiteralUndefined(),
          genThisExpression(),
          /*args*/ {})
      ->setType(Type::createUndefined());
}

void ESTreeIRGen::genDummyFunction(Function *dummy) {
  IRBuilder builder{dummy};

  builder.createJSThisParam(dummy);
  BasicBlock *firstBlock = builder.createBasicBlock(dummy);
  builder.setInsertionBlock(firstBlock);
  builder.createUnreachableInst();
}

/// \return the NodeKind of the FunctionLikeNode (used for lazy parsing).
/// \param parentNode can optionally be set to the parent AST node of the
///  function node.
static ESTree::NodeKind getLazyFunctionKind(
    ESTree::FunctionLikeNode *node,
    ESTree::Node *parentNode) {
  if (llvh::dyn_cast_or_null<ESTree::MethodDefinitionNode>(parentNode)) {
    return ESTree::NodeKind::MethodDefinition;
  }
  if (node->isMethodDefinition) {
    // This is not a regular function expression but getter/setter.
    // If we want to reparse it later, we have to start from an
    // identifier and not from a 'function' keyword.
    return ESTree::NodeKind::Property;
  }
  return node->getKind();
}

void ESTreeIRGen::setupLazyFunction(
    Function *F,
    ESTree::FunctionLikeNode *functionNode,
    ESTree::Node *parentNode,
    ESTree::BlockStatementNode *bodyBlock,
    VariableScope *parentVarScope,
    ExtraKey extraKey,
    const CapturedState &capturedState) {
  // Avoid modifying Builder state because this isn't enqueued.
  IRBuilder::SaveRestore saveBuilder{Builder};

  // Populate the IR function for this node.
  CompiledMapKey key(functionNode, (unsigned)extraKey);
  auto [_, inserted] = compiledEntities_.try_emplace(key, F);
  (void)inserted;
  assert(inserted && "Function already compiled");

  Builder.createJSThisParam(F);
  BasicBlock *bb = Builder.createBasicBlock(F);
  Builder.setInsertionBlock(bb);
  Variable *constructor = nullptr;
  Variable *initFuncVar = nullptr;
  if (curFunction()->hasLegacyClassContext()) {
    constructor = curFunction()->legacyClassContext->constructor;
    initFuncVar = curFunction()->legacyClassContext->instElemInitFuncVar;
  }

  LazyCompilationData data{
      bodyBlock->bufferId,
      functionNode->getSemInfo(),
      getLazyFunctionKind(functionNode, parentNode),
      ESTree::isStrict(functionNode->strictness),
      bodyBlock->paramYield,
      bodyBlock->paramAwait};

  Builder.createLazyCompilationDataInst(
      std::move(data),
      capturedState.thisVal,
      capturedState.newTarget,
      capturedState.arguments,
      capturedState.homeObject,
      constructor,
      initFuncVar,
      parentVarScope);
  Builder.createUnreachableInst();

  // Set the function's .length.
  F->setExpectedParamCountIncludingThis(
      countExpectedArgumentsIncludingThis(functionNode));
}

/// Generate a function which immediately throws the specified SyntaxError
/// message.
Function *ESTreeIRGen::genSyntaxErrorFunction(
    Module *M,
    Identifier originalName,
    SMRange sourceRange,
    llvh::StringRef error) {
  IRBuilder::SaveRestore saveRestore(Builder);

  Function *function = Builder.createFunction(
      originalName,
      Function::DefinitionKind::ES5Function,
      true,
      CustomDirectives{
          .sourceVisibility = SourceVisibility::Sensitive,
          .alwaysInline = false,
      },
      sourceRange);

  Builder.createJSThisParam(function);
  BasicBlock *firstBlock = Builder.createBasicBlock(function);
  Builder.setInsertionBlock(firstBlock);

  Builder.createThrowInst(Builder.createCallInst(
      emitLoad(Builder.createGlobalObjectProperty("SyntaxError", false), false),
      /* newTarget */ Builder.getLiteralUndefined(),
      /* thisValue */ Builder.getLiteralUndefined(),
      Builder.getLiteralString(error)));

  return function;
}

void ESTreeIRGen::onCompiledFunction(hermes::Function *F) {
  curFunction()->function->clearStatementCount();
  curFunction()->function->clearLexicalScope();
  curFunction()->function->clearEnvironmentID();

  // Delete any unreachable blocks produced while emitting this function.
  deleteUnreachableBasicBlocks(curFunction()->function);

  fixupCatchTargets(F);

  // Postprocessing for debugging: make the EvalCompilationData
  // and add the function's VariableScope to the data so we can compile REPL
  // commands from the debugger.
  // Skip outer generator and async functions. It's impossible to be stopped
  // inside of one, so we don't need to support `eval`ing inside of them.
  if ((Mod->getContext().getDebugInfoSetting() == DebugInfoSetting::ALL) &&
      !llvh::isa<GeneratorFunction>(F) && !llvh::isa<AsyncFunction>(F)) {
    BasicBlock &entry = *F->begin();

    IRBuilder::ScopedLocationChange slc(Builder, F->getSourceRange().Start);
    Builder.setInsertionPoint(&*entry.begin());

    EvalCompilationData data{curFunction()->getSemInfo()};
    // Only persist `this` and `new.target` if we are saving eval data for an
    // arrow function. Note that the home object should always be persisted,
    // since elsewhere in IRGen we always look into the captured state to find
    // the home object (e.g. for arrow and non-arrow)
    bool isArrow = F->getDefinitionKind() == Function::DefinitionKind::ES6Arrow;
    bool isNestedInDerivedCons =
        semCtx_.nearestNonArrow(curFunction()->getSemInfo())->constructorKind ==
        sema::FunctionInfo::ConstructorKind::Derived;
    bool shouldCaptureState = isArrow || isNestedInDerivedCons;
    Variable *clsConstructor = nullptr;
    Variable *clsInitFunc = nullptr;
    if (curFunction()->hasLegacyClassContext()) {
      clsConstructor = curFunction()->legacyClassContext->constructor;
      clsInitFunc = curFunction()->legacyClassContext->instElemInitFuncVar;
    }
    llvh::SmallVector<VariableScope *, 4> allVarScopes;
    allVarScopes.push_back(curFunction()->curScope()->getVariableScope());
    for (auto [_, varScope] : curFunction()->getInnerScopes()) {
      allVarScopes.push_back(varScope);
    }
    auto *evalData = Builder.createEvalCompilationDataInst(
        std::move(data),
        shouldCaptureState ? curFunction()->capturedState.thisVal : nullptr,
        shouldCaptureState ? curFunction()->capturedState.newTarget : nullptr,
        nullptr,
        curFunction()->capturedState.homeObject,
        clsConstructor,
        clsInitFunc,
        allVarScopes);
    // This is never emitted, it has no location.
    evalData->setLocation({});

    assert(!F->isLazy());
  }
}

} // namespace irgen
} // namespace hermes
