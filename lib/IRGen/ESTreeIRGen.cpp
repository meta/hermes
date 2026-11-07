/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ESTreeIRGen.h"

#include "hermes/IR/IRUtils.h"

#include "llvh/ADT/BitVector.h"
#include "llvh/ADT/ScopeExit.h"
#include "llvh/ADT/SetVector.h"
#include "llvh/ADT/StringSet.h"
#include "llvh/Support/Debug.h"
#include "llvh/Support/SaveAndRestore.h"

namespace hermes {
namespace irgen {

//===----------------------------------------------------------------------===//
// Free standing helpers.

/// \returns true if \p node is a constant expression.
bool isConstantExpr(ESTree::Node *node) {
  switch (node->getKind()) {
    case ESTree::NodeKind::StringLiteral:
    case ESTree::NodeKind::NumericLiteral:
    case ESTree::NodeKind::NullLiteral:
    case ESTree::NodeKind::BooleanLiteral:
      return true;
    default:
      return false;
  }
}

//===----------------------------------------------------------------------===//
// LReference

IRBuilder &LReference::getBuilder() {
  return irgen_->Builder;
}

Value *LReference::emitLoad() {
  auto &builder = getBuilder();
  IRBuilder::ScopedLocationChange slc(builder, loadLoc_);

  switch (kind_) {
    case Kind::Empty:
      assert(false && "empty cannot be loaded");
      return builder.getLiteralUndefined();
    case Kind::Member:
      return irgen_
          ->emitMemberLoad(
              llvh::cast<ESTree::MemberExpressionNode>(ast_), base_, property_)
          .result;
    case Kind::VarOrGlobal:
      return irgen_->emitLoad(base_, false);
    case Kind::Destructuring:
      assert(false && "destructuring cannot be loaded");
      return builder.getLiteralUndefined();
    case Kind::Error:
      return builder.getLiteralUndefined();
  }

  llvm_unreachable("invalid LReference kind");
}

void LReference::emitStore(Value *value, bool narrow) {
  switch (kind_) {
    case Kind::Empty:
      return;
    case Kind::Member:
      return irgen_->emitMemberStore(
          llvh::cast<ESTree::MemberExpressionNode>(ast_),
          value,
          base_,
          property_,
          thisValue_);
    case Kind::VarOrGlobal:
      // Destructuring and for-in/of stores have no ImplicitCheckedCast,
      // so the stored value is typed wider than it should be.
      // Narrow it against the variable's declared type here, like
      // enforceExprType does for expressions.
      // When we actually enforce Variables being the correct type (requires
      // IRGen changes), we can remove the narrow flag.
      if (auto *id = llvh::dyn_cast_or_null<ESTree::IdentifierNode>(ast_);
          id && narrow)
        value = irgen_->enforceDeclType(value, id);
      irgen_->emitStore(value, base_, declInit_);
      return;
    case Kind::Error:
      return;
    case Kind::Destructuring:
      return irgen_->emitDestructuringAssignment(
          declInit_, llvh::cast<ESTree::PatternNode>(ast_), value);
  }

  llvm_unreachable("invalid LReference kind");
}

bool LReference::canStoreWithoutSideEffects() const {
  return kind_ == Kind::VarOrGlobal && llvh::isa<Variable>(base_);
}

Variable *LReference::castAsVariable() const {
  return kind_ == Kind::VarOrGlobal ? dyn_cast_or_null<Variable>(base_)
                                    : nullptr;
}
GlobalObjectProperty *LReference::castAsGlobalObjectProperty() const {
  return kind_ == Kind::VarOrGlobal
      ? dyn_cast_or_null<GlobalObjectProperty>(base_)
      : nullptr;
}

//===----------------------------------------------------------------------===//
// ESTreeIRGen

ESTreeIRGen::ESTreeIRGen(
    Module *M,
    sema::SemContext &semCtx,
    flow::FlowContext &flowContext,
    ESTree::Node *root)
    : Mod(M),
      semCtx_(semCtx),
      kw_(M->getContext().keywords()),
      flowContext_(flowContext),
      Root(root),
      Builder(Mod),
      identDefaultExport_(Builder.createIdentifier("?default")) {}

llvh::StringRef ESTreeIRGen::propertyKeyAsString(
    llvh::SmallVectorImpl<char> &storage,
    ESTree::Node *Key) {
  // Handle String Literals.
  // http://www.ecma-international.org/ecma-262/6.0/#sec-literals-string-literals
  if (auto *Lit = llvh::dyn_cast<ESTree::StringLiteralNode>(Key)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading String Literal \"" << Lit->_value << "\"\n");
    return Lit->_value->str();
  }

  // Handle identifiers as if they are String Literals.
  if (auto *Iden = llvh::dyn_cast<ESTree::IdentifierNode>(Key)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading String Literal \"" << Iden->_name << "\"\n");
    return Iden->_name->str();
  }

  // Handle Number Literals.
  // http://www.ecma-international.org/ecma-262/6.0/#sec-literals-numeric-literals
  if (auto *Lit = llvh::dyn_cast<ESTree::NumericLiteralNode>(Key)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading Numeric Literal \"" << Lit->_value << "\"\n");
    storage.resize(NUMBER_TO_STRING_BUF_SIZE);
    return numberToString(Lit->_value, storage.data(), storage.size());
  }

  // Handle BigInt Literals, because they're a type of NumericLiteral.
  // ES15 12.9.3
  if (auto *Lit = llvh::dyn_cast<ESTree::BigIntLiteralNode>(Key)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading BigInt Literal \"" << Lit->_bigint->str()
                     << "\"\n");
    return Lit->_bigint->str();
  }

  llvm_unreachable("Don't know this kind of property key");
}

void ESTreeIRGen::doIt(llvh::StringRef topLevelFunctionName) {
  LLVM_DEBUG(llvh::dbgs() << "Processing top level program.\n");

  ESTree::ProgramNode *Program;

  Program = llvh::dyn_cast<ESTree::ProgramNode>(Root);

  if (!Program) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        SMLoc{}, "missing 'Program' AST node");
    return;
  }

  LLVM_DEBUG(llvh::dbgs() << "Found Program decl.\n");

  // The function which will "execute" the module.
  Function *const topLevelFunction = Builder.createTopLevelFunction(
      topLevelFunctionName,
      ESTree::isStrict(Program->strictness),
      Program->getSemInfo()->customDirectives,
      Program->getSourceRange());

  // Function context for topLevelFunction.
  FunctionContext topLevelFunctionContext{
      this, topLevelFunction, Program->getSemInfo()};
  Function::ScopedLexicalScopeChange lexScopeChange(
      curFunction()->function, Program->getSemInfo()->getFunctionBodyScope());

  emitFunctionPrologue(
      Program,
      Builder.createBasicBlock(topLevelFunction),
      InitES5CaptureState::Yes,
      DoEmitDeclarations::Yes,
      nullptr);

  Value *retVal;
  {
    // Allocate the return register, initialize it to undefined.
    curFunction()->globalReturnRegister = Builder.createAllocStackInst(
        genAnonymousLabelName("ret"), Type::createAnyType());
    Builder.createStoreStackInst(
        Builder.getLiteralUndefined(), curFunction()->globalReturnRegister);

    genBody(Program->_body);

    // Terminate the top-level scope with a return statement.
    retVal = Builder.createLoadStackInst(curFunction()->globalReturnRegister);
  }

  emitFunctionEpilogue(retVal);

  drainCompilationQueue();
}

Function *ESTreeIRGen::doItInScope(EvalCompilationDataInst *evalDataInst) {
  LLVM_DEBUG(llvh::dbgs() << "Processing program in scope.\n");
  sema::LexicalScope *lexScope = semCtx_.getParentLexicalScope();
  assert(lexScope && "SemCtx parent lexical scope cannot be null");
  VariableScope *varScope = getLexicalScopeData(lexScope);
  assert(varScope && "eval IRGen cannot have null variable scope");

  ESTree::ProgramNode *Program;

  Program = llvh::dyn_cast<ESTree::ProgramNode>(Root);

  assert(Program && "missing 'Program' AST node");

  LLVM_DEBUG(llvh::dbgs() << "Found Program decl.\n");

  Function *const topLevelFunction = Mod->getTopLevelFunction();

  // Function context for topLevelFunction.
  FunctionContext topLevelFunctionContext{
      this, topLevelFunction, Program->getSemInfo()};

  emitLazyGlobalDeclarations(semCtx_.getGlobalScope());

  auto defKind = Function::DefinitionKind::ES5Function;
  if (evalDataInst->getCapturedThis()) {
    // If evalDataInst has a populated captured this, that means we are stopped
    // inside of an arrow function. The way to resolve new.target/this now is to
    // pretend that we are executing in an arrow function. Then set up the
    // captured state before we generate the program node.
    // If not stopped in an arrow, then new.target/this will be resolved with
    // normal IR for looking into the function's frame, which is populated when
    // we invoke this top-level eval function.
    defKind = Function::DefinitionKind::ES6Arrow;
  }
  // The function which will "execute" the module.
  Function *const newFunc = Builder.createFunction(
      "",
      defKind,
      ESTree::isStrict(Program->strictness),
      Program->getSemInfo()->customDirectives,
      Program->getSourceRange());

  // Function context for topLevelFunction.
  FunctionContext mainFunctionContext{this, newFunc, Program->getSemInfo()};
  curFunction()->capturedState.newTarget = evalDataInst->getCapturedNewTarget();
  curFunction()->capturedState.thisVal = evalDataInst->getCapturedThis();
  curFunction()->capturedState.homeObject = evalDataInst->getHomeObject();

  // If there was class context, restore it.
  if (evalDataInst->getClsConstructor()) {
    curFunction()->legacyClassContext = std::make_shared<LegacyClassContext>(
        evalDataInst->getClsConstructor(),
        evalDataInst->getClsInstElemInitFunc());
  }

  emitFunctionPrologue(
      Program,
      Builder.createBasicBlock(newFunc),
      InitES5CaptureState::Yes,
      DoEmitDeclarations::Yes,
      varScope);

  // Allocate the return register, initialize it to undefined.
  curFunction()->globalReturnRegister = Builder.createAllocStackInst(
      genAnonymousLabelName("ret"), Type::createAnyType());
  Builder.createStoreStackInst(
      Builder.getLiteralUndefined(), curFunction()->globalReturnRegister);

  genBody(Program->_body);

  Value *retVal =
      Builder.createLoadStackInst(curFunction()->globalReturnRegister);

  emitFunctionEpilogue(retVal);

  drainCompilationQueue();

  return newFunc;
}

void ESTreeIRGen::doCJSModule(
    sema::SemContext &semContext,
    uint32_t segmentID,
    uint32_t id,
    llvh::StringRef filename) {
  assert(Root && "no root in ESTreeIRGen");
  auto *func = cast<ESTree::FunctionExpressionNode>(Root);

  // Create a top level FunctionContext that will never be executed, because
  // genBasicFunction reads state from the current function context (e.g. the
  // class contexts captured when enqueueing the compilation of the module
  // function).
  FunctionContext topLevelFunctionContext{
      this, Mod->getTopLevelFunction(), nullptr};

  // Take care of the additions to the global scope that this module could
  // have done. A module can only add ambient global properties. Look for new
  // ones (customData == nullptr) and declare them.
  for (sema::Decl *decl : semContext.getGlobalScope()->decls) {
    if (decl->kind == sema::Decl::Kind::UndeclaredGlobalProperty &&
        !decl->customData) {
      setDeclData(decl, Builder.createGlobalObjectProperty(decl->name, false));
    }
  }

  Identifier functionName = Builder.createIdentifier("cjs_module");
  Function *newFunc = genBasicFunction(functionName, func, nullptr);

  drainCompilationQueue();

  Builder.getModule()->addCJSModule(
      segmentID, id, Builder.createIdentifier(filename), newFunc);
}

Function *ESTreeIRGen::doLazyFunction(Function *lazyFunc) {
  LazyCompilationDataInst *lazyDataInst =
      lazyFunc->getLazyCompilationDataInst();

  // Create a top level FunctionContext that will never be executed, because:
  // 1. IRGen assumes the first function always has global scope
  // 2. It serves as the root for dummy functions for lexical data
  FunctionContext topLevelFunctionContext{
      this, Mod->getTopLevelFunction(), nullptr};

  emitLazyGlobalDeclarations(semCtx_.getGlobalScope());

  auto *node = cast<ESTree::FunctionLikeNode>(Root);

  VariableScope *parentVarScope = lazyDataInst->getParentVarScope();

  auto *homeObj = lazyDataInst->getHomeObject();
  CapturedState capturedState{
      lazyDataInst->getCapturedThis(),
      lazyDataInst->getCapturedNewTarget(),
      lazyDataInst->getCapturedArguments(),
      homeObj};

  // If there was class context, restore it.
  if (lazyDataInst->getClsConstructor()) {
    curFunction()->legacyClassContext = std::make_shared<LegacyClassContext>(
        lazyDataInst->getClsConstructor(),
        lazyDataInst->getClsInstaneElemInitFunc());
  }

  auto freeClsCtx = llvh::make_scope_exit([this]() {
    if (curFunction()->legacyClassContext)
      curFunction()->legacyClassContext.reset();
  });

  Function *compiledFunc;
  if (auto *arrow = llvh::dyn_cast<ESTree::ArrowFunctionExpressionNode>(Root)) {
    if (arrow->_async) {
      compiledFunc = genAsyncFunction(
          lazyFunc->getOriginalOrInferredName(),
          arrow,
          parentVarScope,
          capturedState);
    } else {
      compiledFunc = genCapturingFunction(
          lazyFunc->getOriginalOrInferredName(),
          arrow,
          parentVarScope,
          capturedState,
          Function::DefinitionKind::ES6Arrow);
    }
  } else {
    // Generators have had their lazy scope set up without setting one up
    // for the inner functions. This means that we will never directly generate
    // a GeneratorInnerFunction here.
    compiledFunc = ESTree::isAsync(node)
        ? genAsyncFunction(
              lazyFunc->getOriginalOrInferredName(),
              node,
              parentVarScope,
              capturedState)
        : ESTree::isGenerator(node) ? genGeneratorFunction(
                                          lazyFunc->getOriginalOrInferredName(),
                                          node,
                                          parentVarScope,
                                          homeObj)
                                    : genBasicFunction(
                                          lazyFunc->getOriginalOrInferredName(),
                                          node,
                                          parentVarScope,
                                          /* superClassNode */ nullptr,
                                          lazyFunc->getDefinitionKind(),
                                          homeObj);
  }

  drainCompilationQueue();

  // The lazyFunc will be destroyed when the BytecodeFunction
  // is destroyed.
  // It won't be needed again, even if there is an error later in the pipeline,
  // because we store the error message in BytecodeFunction and reuse it,
  // ensuring we never use the information in lazyFunc ever again.

  // Free the allocated legacy class context.
  if (curFunction()->legacyClassContext)
    curFunction()->legacyClassContext.reset();

  // promotedDecls is used immediately and relies on IdentifierNode,
  // which is deallocated between invocations of lazy compilation.
  // Therefore, it is necessary to clear promotedDecls when freeing the
  // AST in order to avoid dangling pointers.
  semCtx_.clearPromotedDecls();
  return compiledFunc;
}

Value *ESTreeIRGen::genMemberExpressionProperty(
    ESTree::MemberExpressionLikeNode *Mem) {
  // If computed is true, the node corresponds to a computed (a[b]) member
  // lookup and '_property' is an Expression. Otherwise, the node
  // corresponds to a static (a.b) member lookup and '_property' is an
  // Identifier.
  // Details of the computed field are available here:
  // https://github.com/estree/estree/blob/master/spec.md#memberexpression

  if (getComputed(Mem)) {
    return genExpression(getProperty(Mem));
  }

  // Arrays and objects may be accessed with integer indices.
  if (auto N = llvh::dyn_cast<ESTree::NumericLiteralNode>(getProperty(Mem))) {
    return Builder.getLiteralNumber(N->_value);
  }

  if (auto *PN = llvh::dyn_cast<ESTree::PrivateNameNode>(getProperty(Mem))) {
    auto *id = llvh::cast<ESTree::IdentifierNode>(PN->_id);
    return genPrivateNameValue(id);
  }

  // ESTree encodes property access as MemberExpression -> Identifier.
  auto Id = cast<ESTree::IdentifierNode>(getProperty(Mem));

  Identifier fieldName = getNameFieldFromID(Id);
  LLVM_DEBUG(
      llvh::dbgs() << "Emitting direct label access to field '" << fieldName
                   << "'\n");
  return Builder.getLiteralString(fieldName);
}

bool ESTreeIRGen::canCreateLRefWithoutSideEffects(
    hermes::ESTree::Node *target) {
  // Check for an identifier bound to an existing local variable.
  if (auto *iden = llvh::dyn_cast<ESTree::IdentifierNode>(target)) {
    return llvh::isa<Variable>(resolveIdentifier(iden));
  }

  return false;
}

LReference ESTreeIRGen::createLRef(ESTree::Node *node, bool declInit) {
  SMLoc sourceLoc = node->getDebugLoc();
  IRBuilder::ScopedLocationChange slc(Builder, sourceLoc);

  if (llvh::isa<ESTree::EmptyNode>(node)) {
    LLVM_DEBUG(llvh::dbgs() << "Creating an LRef for EmptyNode.\n");
    return LReference(
        LReference::Kind::Empty,
        this,
        false,
        nullptr,
        nullptr,
        nullptr,
        sourceLoc);
  }

  /// Create lref for member expression (ex: o.f).
  if (auto *ME = llvh::dyn_cast<ESTree::MemberExpressionNode>(node)) {
    LLVM_DEBUG(llvh::dbgs() << "Creating an LRef for member expression.\n");

    Value *obj;
    // `thisVal` should only be set for `super` references.
    Value *thisVal = nullptr;
    auto *objTypeInfo = flowContext_.getNodeTypeOrAny(ME->_object)->info;
    if (llvh::isa<ESTree::SuperNode>(ME->_object)) {
      thisVal = genThisExpression();
      if (llvh::isa<flow::ClassType>(objTypeInfo) ||
          llvh::isa<flow::ClassConstructorType>(objTypeInfo)) {
        if (ME->_computed) {
          // Only super.foo is supported, matching the load path in
          // genMemberExpressionWithOperation. Without this the store would
          // fall through to the untyped path below, which looks the property
          // up starting at the receiver instead of the superclass.
          Mod->getContext().getSourceErrorManager().error(
              ME->getSourceRange(), "unsupported use of 'super'");
          return LReference(
              LReference::Kind::Error,
              this,
              false,
              nullptr,
              nullptr,
              nullptr,
              sourceLoc);
        }
        // Typed 'super' accesses never consult the object: the member either
        // lives in a scope Variable, is an accessor called with 'this' as the
        // receiver, or is a field slot on the receiver. A static
        // 'super' has no home object at all, and capturedState.homeObject is
        // populated only for legacy classes, object literal methods and eval,
        // none of which apply here, so walking it would be invalid.
        obj = thisVal;
      } else {
        auto *homeObjectVar = curFunction()->capturedState.homeObject;
        assert(homeObjectVar && "homeObjectVar not populated");
        auto *RSI = emitResolveScopeInstIfNeeded(homeObjectVar->getParent());
        Value *homeObjectVal = Builder.createLoadFrameInst(RSI, homeObjectVar);
        obj = Builder.createLoadParentNoTrapsInst(homeObjectVal);
      }
    } else {
      obj = genExpression(ME->_object);
    }

    Value *prop;
    if ((llvh::isa<flow::ClassType>(objTypeInfo) ||
         llvh::isa<flow::ClassConstructorType>(objTypeInfo)) &&
        !ME->_computed) {
      // Named types shouldn't bother with genMemberExpressionProperty,
      // which is intended for untyped code.
      if (auto *privateName =
              llvh::dyn_cast<ESTree::PrivateNameNode>(ME->_property)) {
        // If the property is an identifier, we can use the class type to
        // determine the property name.
        prop =
            Builder.getLiteralPrivateName(getNameFieldFromID(privateName->_id));
      } else {
        prop = Builder.getLiteralString(getNameFieldFromID(ME->_property));
      }
    } else {
      prop = genMemberExpressionProperty(ME);
    }

    return LReference(
        LReference::Kind::Member,
        this,
        false,
        ME,
        obj,
        prop,
        sourceLoc,
        thisVal);
  }

  /// Create lref for identifiers  (ex: a).
  if (auto *iden = llvh::dyn_cast<ESTree::IdentifierNode>(node)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Creating an LRef for identifier \""
                     << getNameFieldFromID(iden) << "\"\n");
    auto *var = resolveIdentifier(iden);
    return LReference(
        LReference::Kind::VarOrGlobal,
        this,
        declInit,
        iden,
        var,
        nullptr,
        sourceLoc);
  }

  /// Create lref for variable decls (ex: var a).
  if (auto *V = llvh::dyn_cast<ESTree::VariableDeclarationNode>(node)) {
    LLVM_DEBUG(llvh::dbgs() << "Creating an LRef for variable declaration.\n");

    assert(V->_declarations.size() == 1 && "Malformed variable declaration");
    auto *decl =
        cast<ESTree::VariableDeclaratorNode>(&V->_declarations.front());

    return createLRef(decl->_id, true);
  }

  // Destructuring assignment.
  if (auto *pat = llvh::dyn_cast<ESTree::PatternNode>(node)) {
    return LReference(this, declInit, pat);
  }

  Builder.getModule()->getContext().getSourceErrorManager().error(
      node->getSourceRange(), "unsupported assignment target");

  return LReference(
      LReference::Kind::Error,
      this,
      false,
      nullptr,
      nullptr,
      nullptr,
      sourceLoc);
}

Value *ESTreeIRGen::genHermesInternalCall(
    llvh::StringRef name,
    Value *thisValue,
    ArrayRef<Value *> args) {
  return Builder.createCallInst(
      Builder.createLoadPropertyInst(
          Builder.createTryLoadGlobalPropertyInst("HermesInternal"), name),
      /* newTarget */ Builder.getLiteralUndefined(),
      thisValue,
      args);
}

Value *ESTreeIRGen::genBuiltinCall(
    hermes::BuiltinMethod::Enum builtinIndex,
    ArrayRef<Value *> args) {
  return Builder.createCallBuiltinInst(builtinIndex, args);
}

void ESTreeIRGen::emitEnsureObject(Value *value, llvh::StringRef message) {
  // TODO: use "thisArg" when builtins get fixed to support it.
  genBuiltinCall(
      BuiltinMethod::HermesBuiltin_ensureObject,
      {value, Builder.getLiteralString(message)});
}

Value *ESTreeIRGen::emitIteratorSymbol() {
  return Builder.createLoadPropertyInst(
      Builder.createGetBuiltinClosureInst(BuiltinMethod::globalThis_Symbol),
      "iterator");
}

Value *ESTreeIRGen::emitAsyncIteratorSymbol() {
  return Builder.createLoadPropertyInst(
      Builder.createGetBuiltinClosureInst(BuiltinMethod::globalThis_Symbol),
      "asyncIterator");
}

ESTreeIRGen::IteratorRecordSlow ESTreeIRGen::emitGetIteratorSlow(Value *obj) {
  auto *method = Builder.createLoadPropertyInst(obj, emitIteratorSymbol());
  auto *iterator = Builder.createCallInst(
      method, /* newTarget */ Builder.getLiteralUndefined(), obj, {});

  emitEnsureObject(iterator, "iterator is not an object");
  auto *nextMethod = Builder.createLoadPropertyInst(iterator, "next");

  return {iterator, nextMethod};
}

Value *ESTreeIRGen::emitAwaitAsyncGenerator(Value *value) {
  auto *fn = Builder.createGetBuiltinClosureInst(
      BuiltinMethod::HermesBuiltin_awaitAsyncGenerator);
  return Builder.createCallInst(
      fn,
      Builder.getLiteralUndefined(),
      Builder.getLiteralUndefined(),
      {value});
}

ESTreeIRGen::IteratorRecordSlow ESTreeIRGen::emitGetAsyncIteratorSlow(
    Value *obj) {
  auto *makeAsyncIterator = Builder.createGetBuiltinClosureInst(
      BuiltinMethod::HermesBuiltin_makeAsyncIterator);

  auto *iterator = Builder.createCallInst(
      makeAsyncIterator,
      Builder.getLiteralUndefined(),
      Builder.getLiteralUndefined(),
      {obj});
  auto *nextMethod = Builder.createLoadPropertyInst(iterator, "next");

  return {iterator, nextMethod};
}

Value *ESTreeIRGen::emitIteratorNextSlow(IteratorRecordSlow iteratorRecord) {
  auto *nextResult = Builder.createCallInst(
      iteratorRecord.nextMethod,
      /* newTarget */ Builder.getLiteralUndefined(),
      iteratorRecord.iterator,
      {});
  emitEnsureObject(nextResult, "iterator.next() did not return an object");
  return nextResult;
}

Value *ESTreeIRGen::emitIteratorCompleteSlow(Value *iterResult) {
  return Builder.createLoadPropertyInst(iterResult, "done");
}

Value *ESTreeIRGen::emitIteratorValueSlow(Value *iterResult) {
  return Builder.createLoadPropertyInst(iterResult, "value");
}

void ESTreeIRGen::_emitIteratorCloseImpl(
    ESTree::Node *astNode,
    hermes::irgen::ESTreeIRGen::IteratorRecordSlow iteratorRecord,
    bool ignoreInnerException,
    bool isAsyncIterator) {
  bool needsOverloadYield = isAsyncIterator &&
      curFunction()->function->isInnerGenerator() &&
      !curFunction()->function->isFromAsyncFunction();

  auto *haveReturn = Builder.createBasicBlock(Builder.getFunction());
  auto *noReturn = Builder.createBasicBlock(Builder.getFunction());

  if (ignoreInnerException) {
    // Per spec §7.4.11 IteratorClose step 5 / §7.4.13 AsyncIteratorClose
    // step 5 ("If completion is a throw completion, return ? completion"):
    // if the original completion is already a throw, any error from
    // GetMethod or from calling return() must be suppressed in favor of the
    // original. Wrap the entire getMethod + call sequence in the try/catch
    // so that a throw from the return getter is also caught and discarded.
    emitTryCatchScaffolding(
        noReturn,
        // emitBody.
        [this, &iteratorRecord, isAsyncIterator, astNode, needsOverloadYield](
            BasicBlock *catchBlock) {
          SurroundingTry thisTry{
              curFunction(),
              astNode,
              catchBlock,
              {},
              [](ESTree::Node *,
                 ControlFlowChange cfc,
                 BasicBlock *continueTarget) {}};

          auto *haveReturn = Builder.createBasicBlock(Builder.getFunction());
          auto *noReturn = Builder.createBasicBlock(Builder.getFunction());

          auto *returnMethod = genBuiltinCall(
              BuiltinMethod::HermesBuiltin_getMethod,
              {iteratorRecord.iterator, Builder.getLiteralString("return")});
          auto *returnIsUndefined = Builder.createBinaryOperatorInst(
              returnMethod,
              Builder.getLiteralUndefined(),
              ValueKind::BinaryStrictlyEqualInstKind);
          Builder.createCondBranchInst(returnIsUndefined, noReturn, haveReturn);

          Builder.setInsertionBlock(haveReturn);
          auto *callResult = Builder.createCallInst(
              returnMethod,
              /* newTarget */ Builder.getLiteralUndefined(),
              iteratorRecord.iterator,
              {});
          if (isAsyncIterator) {
            genYieldOrAwaitExpr(
                needsOverloadYield ? emitAwaitAsyncGenerator(callResult)
                                   : callResult);
          }
          Builder.createBranchInst(noReturn);

          Builder.setInsertionBlock(noReturn);
        },
        // emitNormalCleanup.
        []() {},
        // emitHandler.
        [this](BasicBlock *nextBlock) {
          // We need to catch the exception, even if we don't used it.
          Builder.createCatchInst();
          Builder.createBranchInst(nextBlock);
        });
  } else {
    auto *returnMethod = genBuiltinCall(
        BuiltinMethod::HermesBuiltin_getMethod,
        {iteratorRecord.iterator, Builder.getLiteralString("return")});
    auto *returnIsUndefined = Builder.createBinaryOperatorInst(
        returnMethod,
        Builder.getLiteralUndefined(),
        ValueKind::BinaryStrictlyEqualInstKind);
    Builder.createCondBranchInst(returnIsUndefined, noReturn, haveReturn);

    Builder.setInsertionBlock(haveReturn);
    auto *callResult = Builder.createCallInst(
        returnMethod,
        /* newTarget */ Builder.getLiteralUndefined(),
        iteratorRecord.iterator,
        {});
    emitEnsureObject(
        isAsyncIterator
            ? genYieldOrAwaitExpr(
                  needsOverloadYield ? emitAwaitAsyncGenerator(callResult)
                                     : callResult)
            : callResult,
        "iterator.return() did not return an object");
    Builder.createBranchInst(noReturn);
  }

  Builder.setInsertionBlock(noReturn);
}

void ESTreeIRGen::emitIteratorCloseSlow(
    ESTree::Node *astNode,
    hermes::irgen::ESTreeIRGen::IteratorRecordSlow iteratorRecord,
    bool ignoreInnerException) {
  _emitIteratorCloseImpl(astNode, iteratorRecord, ignoreInnerException, false);
}

void ESTreeIRGen::emitAsyncIteratorCloseSlow(
    ESTree::Node *astNode,
    hermes::irgen::ESTreeIRGen::IteratorRecordSlow iteratorRecord,
    bool ignoreInnerException) {
  _emitIteratorCloseImpl(astNode, iteratorRecord, ignoreInnerException, true);
}

ESTreeIRGen::IteratorRecord ESTreeIRGen::emitGetIterator(Value *obj) {
  // Each of these will be modified by "next", so we use a stack storage.
  auto *iterStorage = Builder.createAllocStackInst(
      genAnonymousLabelName("iter"), Type::createAnyType());
  auto *sourceOrNext = Builder.createAllocStackInst(
      genAnonymousLabelName("sourceOrNext"), Type::createAnyType());
  Builder.createStoreStackInst(obj, sourceOrNext);
  auto *iter = Builder.createIteratorBeginInst(sourceOrNext);
  Builder.createStoreStackInst(iter, iterStorage);
  return IteratorRecord{iterStorage, sourceOrNext};
}

void ESTreeIRGen::emitDestructuringAssignment(
    bool declInit,
    ESTree::PatternNode *target,
    Value *source) {
  if (auto *APN = llvh::dyn_cast<ESTree::ArrayPatternNode>(target))
    return emitDestructuringArray(declInit, APN, source);
  else if (auto *OPN = llvh::dyn_cast<ESTree::ObjectPatternNode>(target)) {
    flow::Type *patType = flowContext_.getNodeTypeOrAny(OPN);
    if (auto *exact = llvh::dyn_cast<flow::ExactObjectType>(patType->info))
      return emitDestructuringTypedObject(declInit, OPN, exact, source);
    return emitDestructuringObject(declInit, OPN, source);
  } else {
    Mod->getContext().getSourceErrorManager().error(
        target->getSourceRange(), "unsupported destructuring target");
  }
}

void ESTreeIRGen::emitDestructuringArray(
    bool declInit,
    ESTree::ArrayPatternNode *targetPat,
    Value *source) {
  flow::Type *patType = flowContext_.getNodeTypeOrAny(targetPat);
  if (auto *tuple = llvh::dyn_cast<flow::TupleType>(patType->info)) {
    emitDestructuringTypedTuple(declInit, targetPat, tuple, source);
    return;
  }
  if (flowContext_.isArrayClassType(patType)) {
    emitDestructuringTypedArray(declInit, targetPat, patType, source);
    return;
  }

  const IteratorRecord iteratorRecord = emitGetIterator(source);

  auto *value = Builder.createAllocStackInst(
      genAnonymousLabelName("iterValue"), Type::createAnyType());

  SharedExceptionHandler handler{};
  handler.exc = Builder.createAllocStackInst(
      genAnonymousLabelName("exc"), Type::createAnyType());
  // All exception handlers branch to this block.
  handler.exceptionBlock = Builder.createBasicBlock(Builder.getFunction());
  bool fastDestructure =
      Mod->getContext().getCodeGenerationSettings().enableFastDestructure;

  bool emittedRest = false;
  // The LReference created in the previous iteration of the destructuring
  // loop. We need it because we want to put the previous store and the creation
  // of the next LReference under one try block.
  llvh::Optional<LReference> lref;

  /// If the previous LReference is valid and non-empty, store "value" into
  /// it and reset the LReference.
  auto storePreviousValue =
      [&lref, &handler, this, value, iteratorRecord, fastDestructure]() {
        if (lref && !lref->isEmpty()) {
          if (fastDestructure || lref->canStoreWithoutSideEffects()) {
            lref->emitStore(Builder.createLoadStackInst(value));
          } else {
            // If we can't store without side effects, wrap the store in
            // try/catch.
            emitTryWithSharedHandler(
                &handler,
                [this, &lref, value, iteratorRecord](BasicBlock *catchBlock) {
                  SurroundingTry thisTry{
                      curFunction(),
                      lref->getNode(),
                      catchBlock,
                      {},
                      [this, iteratorRecord](
                          ESTree::Node *,
                          ControlFlowChange cfc,
                          BasicBlock *continueTarget) {
                        if (cfc == ControlFlowChange::Break) {
                          emitIteratorClose(iteratorRecord, false);
                        }
                      }};
                  lref->emitStore(Builder.createLoadStackInst(value));
                });
          }
          lref.reset();
        }
      };

  for (auto &elem : targetPat->_elements) {
    ESTree::Node *target = &elem;
    ESTree::Node *init = nullptr;

    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(target)) {
      storePreviousValue();
      emitRestElement(declInit, rest, iteratorRecord, &handler);
      emittedRest = true;
      break;
    }

    // If we have an initializer, unwrap it.
    if (auto *assign = llvh::dyn_cast<ESTree::AssignmentPatternNode>(target)) {
      target = assign->_left;
      init = assign->_right;
    }

    // Can we create the new LReference without side effects and avoid a
    // try/catch. The complexity comes from having to check whether the last
    // LReference also can avoid a try/catch or not.
    if (canCreateLRefWithoutSideEffects(target)) {
      // We don't need a try/catch, but last lref might. Just let the routine
      // do the right thing.
      storePreviousValue();
      lref = createLRef(target, declInit);
    } else {
      // We need a try/catch, but last lref might not. If it doesn't, emit it
      // directly and clear it, so we won't do anything inside our try/catch.
      if (lref && lref->canStoreWithoutSideEffects()) {
        lref->emitStore(Builder.createLoadStackInst(value));
        lref.reset();
      }
      if (fastDestructure) {
        if (lref && !lref->isEmpty())
          lref->emitStore(Builder.createLoadStackInst(value));
        lref = createLRef(target, declInit);
      } else {
        emitTryWithSharedHandler(
            &handler,
            [this, &lref, value, target, declInit, iteratorRecord](
                BasicBlock *catchBlock) {
              SurroundingTry thisTry{
                  curFunction(),
                  target,
                  catchBlock,
                  {},
                  [this, iteratorRecord](
                      ESTree::Node *,
                      ControlFlowChange cfc,
                      BasicBlock *continueTarget) {
                    if (cfc == ControlFlowChange::Break) {
                      emitIteratorClose(iteratorRecord, false);
                    }
                  }};
              // Store the previous value, if we have one.
              if (lref && !lref->isEmpty())
                lref->emitStore(Builder.createLoadStackInst(value));
              lref = createLRef(target, declInit);
            });
      }
    }

    Builder.createStoreStackInst(emitIteratorNext(iteratorRecord), value);

    if (init) {
      auto *getDefaultBlock = Builder.createBasicBlock(Builder.getFunction());
      auto *storeBlock = Builder.createBasicBlock(Builder.getFunction());
      Builder.createCondBranchInst(
          Builder.createBinaryOperatorInst(
              Builder.createLoadStackInst(value),
              Builder.getLiteralUndefined(),
              ValueKind::BinaryStrictlyNotEqualInstKind),
          storeBlock,
          getDefaultBlock);

      Identifier nameHint = llvh::isa<ESTree::IdentifierNode>(target)
          ? getNameFieldFromID(target)
          : Identifier{};

      // getDefaultBlock:
      Builder.setInsertionBlock(getDefaultBlock);
      if (fastDestructure) {
        Builder.createStoreStackInst(genExpression(init, nameHint), value);
      } else {
        emitTryWithSharedHandler(
            &handler,
            [this, init, nameHint, value, iteratorRecord](
                BasicBlock *catchBlock) {
              SurroundingTry thisTry{
                  curFunction(),
                  init,
                  catchBlock,
                  {},
                  [this, iteratorRecord](
                      ESTree::Node *,
                      ControlFlowChange cfc,
                      BasicBlock *continueTarget) {
                    if (cfc == ControlFlowChange::Break) {
                      emitIteratorClose(iteratorRecord, false);
                    }
                  }};
              Builder.createStoreStackInst(
                  genExpression(init, nameHint), value);
            });
      }
      Builder.createBranchInst(storeBlock);

      // storeBlock:
      Builder.setInsertionBlock(storeBlock);
    }
  }

  storePreviousValue();

  // A rest element exhausts the iterator. Otherwise, IteratorClose
  // closes it only if it is still active.
  if (!emittedRest) {
    emitIteratorClose(iteratorRecord, false);
  }

  // If we emitted at least one try block, generate the exception handler.
  if (handler.emittedTry) {
    IRBuilder::SaveRestore saveRestore{Builder};
    Builder.setInsertionBlock(handler.exceptionBlock);

    emitIteratorClose(iteratorRecord, true);
    Builder.createThrowInst(Builder.createLoadStackInst(handler.exc));
  } else {
    // If we didn't use the exception block, we need to delete it, otherwise
    // it fails IR validation even though it will be never executed.
    handler.exceptionBlock->eraseFromParent();

    // Delete the not needed exception stack allocation. It would be optimized
    // out later, but it is nice to produce cleaner non-optimized IR, if it is
    // easy to do so.
    assert(
        !handler.exc->hasUsers() &&
        "should not have any users if no try/catch was emitted");
    handler.exc->eraseFromParent();
  }
}

void ESTreeIRGen::emitDestructuringTypedTuple(
    bool declInit,
    ESTree::ArrayPatternNode *targetPat,
    flow::TupleType *type,
    Value *source) {
  size_t i = 0;
  for (ESTree::Node &elem : targetPat->_elements) {
    // It's possible that getting the LRef will have side effects,
    // so we must emit it before the PrLoad.
    // We don't need a try/catch here because there's no iterator to close
    // in typed mode.
    // The PrLoad itself won't have side effects.
    LReference elemLRef = createLRef(&elem, declInit);
    assert(
        i < type->getTypes().size() &&
        "index out of bounds on typechecked tuple");
    Value *val = Builder.createPrLoadInst(
        source,
        i,
        Builder.getLiteralString(llvh::Twine(i)),
        flowTypeToIRType(type->getTypes()[i]));
    elemLRef.emitStore(val);
    ++i;
  }
}

void ESTreeIRGen::emitDestructuringTypedArray(
    bool declInit,
    ESTree::ArrayPatternNode *targetPat,
    flow::Type *arrayClassType,
    Value *source) {
  assert(
      flowContext_.isArrayClassType(arrayClassType) &&
      "emitDestructuringTypedArray requires Array<T>");
  flow::Type *elemType = flowContext_.getArrayElementType(arrayClassType);
  Type irElemType = flowTypeToIRType(elemType);

  size_t i = 0;
  for (auto it = targetPat->_elements.begin(), end = targetPat->_elements.end();
       it != end;
       ++it, ++i) {
    ESTree::Node &elem = *it;

    if (llvh::isa<ESTree::EmptyNode>(&elem))
      continue;

    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&elem)) {
      assert(std::next(it) == end && "rest element must be the last element");
      // Delegate to fastArraySlice, which copies the tail [i, length) into a
      // fresh FastArray.
      LReference restLRef = createLRef(rest->_argument, declInit);
      auto *newArr = genBuiltinCall(
          BuiltinMethod::HermesBuiltin_fastArraySlice,
          {source, Builder.getLiteralNumber(i)});
      restLRef.emitStore(newArr);
    } else {
      LReference lref = createLRef(&elem, declInit);
      Value *val = Builder.createFastArrayLoadInst(
          source, Builder.getLiteralNumber(i), irElemType);
      lref.emitStore(val);
    }
  }
}

void ESTreeIRGen::emitRestElement(
    bool declInit,
    ESTree::RestElementNode *rest,
    hermes::irgen::ESTreeIRGen::IteratorRecord iteratorRecord,
    SharedExceptionHandler *handler) {
  // 13.3.3.8 BindingRestElement:...BindingIdentifier

  auto *notDoneBlock = Builder.createBasicBlock(Builder.getFunction());
  auto *newValueBlock = Builder.createBasicBlock(Builder.getFunction());
  auto *doneBlock = Builder.createBasicBlock(Builder.getFunction());

  llvh::Optional<LReference> lref;
  if (canCreateLRefWithoutSideEffects(rest->_argument)) {
    lref = createLRef(rest->_argument, declInit);
  } else {
    emitTryWithSharedHandler(
        handler,
        [this, &lref, rest, declInit, iteratorRecord](BasicBlock *catchBlock) {
          SurroundingTry thisTry{
              curFunction(),
              rest,
              catchBlock,
              {},
              [this, iteratorRecord](
                  ESTree::Node *,
                  ControlFlowChange cfc,
                  BasicBlock *continueTarget) {
                if (cfc == ControlFlowChange::Break) {
                  emitIteratorClose(iteratorRecord, false);
                }
              }};
          lref = createLRef(rest->_argument, declInit);
        });
  }

  auto *A = Builder.createAllocArrayInst({}, 0);
  auto *n = Builder.createAllocStackInst(
      genAnonymousLabelName("n"), Type::createNumber());

  // n = 0.
  Builder.createStoreStackInst(Builder.getLiteralPositiveZero(), n);

  Builder.createBranchInst(notDoneBlock);

  // notDoneBlock:
  Builder.setInsertionBlock(notDoneBlock);
  auto *stepValue = emitIteratorNext(iteratorRecord);
  auto *stepDone = emitIteratorComplete(iteratorRecord);
  Builder.createCondBranchInst(stepDone, doneBlock, newValueBlock);

  // newValueBlock:
  Builder.setInsertionBlock(newValueBlock);
  auto *nVal = Builder.createLoadStackInst(n);
  // A[n] = stepValue;
  // Unfortunately this can throw because our arrays can have limited range.
  // The spec doesn't specify what to do in this case, but the reasonable thing
  // to do is to what we would if this was a for-of loop doing the same thing.
  // See section BindingRestElement:...BindingIdentifier, step f and g:
  // https://www.ecma-international.org/ecma-262/9.0/index.html#sec-destructuring-binding-patterns-runtime-semantics-iteratorbindinginitialization
  emitTryWithSharedHandler(handler, [this, stepValue, A, nVal](BasicBlock *) {
    Builder.createStorePropertyInst(stepValue, A, nVal);
  });
  // ++n;
  auto add = Builder.createBinaryOperatorInst(
      nVal, Builder.getLiteralNumber(1), ValueKind::BinaryAddInstKind);
  add->setType(Type::createNumber());
  Builder.createStoreStackInst(add, n);
  Builder.createBranchInst(notDoneBlock);

  // doneBlock:
  Builder.setInsertionBlock(doneBlock);
  if (lref->canStoreWithoutSideEffects()) {
    lref->emitStore(A);
  } else {
    emitTryWithSharedHandler(handler, [&lref, A, this](BasicBlock *catchBlock) {
      SurroundingTry thisTry{
          curFunction(),
          lref->getNode(),
          catchBlock,
          {},
          [](ESTree::Node *,
             ControlFlowChange cfc,
             BasicBlock *continueTarget) {}};
      lref->emitStore(A);
    });
  }
}

void ESTreeIRGen::emitDestructuringObject(
    bool declInit,
    ESTree::ObjectPatternNode *target,
    Value *source) {
  // Keep track of which keys have been destructured.
  llvh::SmallVector<Value *, 4> excludedItems{};

  if (target->_properties.empty() ||
      llvh::isa<ESTree::RestElementNode>(target->_properties.front())) {
    // ES10.0 13.3.3.5
    // 1. Perform ? RequireObjectCoercible(value).

    // The extremely unlikely case that the user is attempting to destructure
    // into {} or {...rest}. Any other object destructuring will fail upon
    // attempting to retrieve a real property from `source`.
    // We must check that the source can be destructured,
    // and the only time this will throw is if source is undefined or null.
    auto *throwBB = Builder.createBasicBlock(Builder.getFunction());
    auto *doneBB = Builder.createBasicBlock(Builder.getFunction());

    // Use == instead of === to account for both undefined and null.
    Builder.createCondBranchInst(
        Builder.createBinaryOperatorInst(
            source, Builder.getLiteralNull(), ValueKind::BinaryEqualInstKind),
        throwBB,
        doneBB);

    Builder.setInsertionBlock(throwBB);
    Builder.createThrowTypeErrorInst(
        Builder.getLiteralString("Cannot destructure 'undefined' or 'null'."));

    Builder.setInsertionBlock(doneBB);
  }

  for (auto &elem : target->_properties) {
    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&elem)) {
      emitRestProperty(declInit, rest, excludedItems, source);
      break;
    }
    auto *propNode = cast<ESTree::PropertyNode>(&elem);

    ESTree::Node *valueNode = propNode->_value;
    ESTree::Node *init = nullptr;

    // If we have an initializer, unwrap it.
    if (auto *assign =
            llvh::dyn_cast<ESTree::AssignmentPatternNode>(valueNode)) {
      valueNode = assign->_left;
      init = assign->_right;
    }

    Identifier nameHint = llvh::isa<ESTree::IdentifierNode>(valueNode)
        ? getNameFieldFromID(valueNode)
        : Identifier{};

    if (llvh::isa<ESTree::IdentifierNode>(propNode->_key) &&
        !propNode->_computed) {
      Identifier key = getNameFieldFromID(propNode->_key);
      excludedItems.push_back(Builder.getLiteralString(key));
      LReference lref = createLRef(valueNode, declInit);
      Value *propLoad = Builder.createLoadPropertyInst(source, key);
      Value *optInit = emitOptionalInitialization(propLoad, init, nameHint);
      lref.emitStore(optInit);
    } else {
      Value *key = genExpression(propNode->_key);
      excludedItems.push_back(key);
      LReference lref = createLRef(valueNode, declInit);
      Value *propLoad = Builder.createLoadPropertyInst(source, key);
      Value *optInit = emitOptionalInitialization(propLoad, init, nameHint);
      lref.emitStore(optInit);
    }
  }
}

void ESTreeIRGen::emitRestProperty(
    bool declInit,
    ESTree::RestElementNode *rest,
    const llvh::SmallVectorImpl<Value *> &excludedItems,
    hermes::Value *source) {
  auto lref = createLRef(rest->_argument, declInit);

  // Construct the excluded items.
  llvh::SmallVector<Value *, 4> computedExcludedItems{};
  // Keys must be deduplicated so we don't create StoreNewOwnProperty with the
  // same key twice. Use a SetVector for deterministic ordering.
  llvh::SetVector<Literal *> literalExcludedItems;
  auto *zeroValue = Builder.getLiteralPositiveZero();

  for (Value *key : excludedItems) {
    if (auto *lit = llvh::dyn_cast<LiteralString>(key)) {
      // If the key is a literal string, we can use StoreNewOwnProperty which
      // will allow us to create the object with a single instruction after
      // optimization.
      literalExcludedItems.insert(lit);
    } else {
      // If the key is not a supported literal, then we have to dynamically
      // populate the excluded object with regular stores.
      computedExcludedItems.push_back(key);
    }
  }

  Value *excludedObj;
  if (excludedItems.empty()) {
    excludedObj = Builder.getLiteralUndefined();
  } else {
    // Explicitly set the prototype for the object created here so it isn't
    // initialized to Object.prototype, which may be modified by the user.
    excludedObj =
        Builder.createAllocObjectLiteralInst({}, Builder.getLiteralNull());

    for (Literal *key : literalExcludedItems)
      Builder.createDefineOwnPropertyInst(
          zeroValue, excludedObj, key, IRBuilder::PropEnumerable::Yes);

    for (Value *key : computedExcludedItems) {
      Builder.createStorePropertyInst(zeroValue, excludedObj, key);
    }
  }

  auto *restValue = genBuiltinCall(
      BuiltinMethod::HermesBuiltin_copyDataProperties,
      {Builder.createAllocObjectLiteralInst(), source, excludedObj});

  lref.emitStore(restValue);
}

void ESTreeIRGen::emitDestructuringTypedObject(
    bool declInit,
    ESTree::ObjectPatternNode *target,
    flow::ExactObjectType *srcType,
    Value *source) {
  // An indexer object has no fixed-slot named fields, so the slot-based PrLoad
  // path below cannot serve it. Lower it dynamically: each named property is a
  // by-name load narrowed to the indexer value type (a missing key throws),
  // and the rest binding copies the remaining keys via copyDataProperties.
  if (const auto &indexer = srcType->getIndexer()) {
    Type valueIRType = flowTypeToIRType(indexer->valueType);
    llvh::SmallVector<Value *, 4> excludedItems{};
    for (auto &elem : target->_properties) {
      if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&elem)) {
        emitRestProperty(declInit, rest, excludedItems, source);
        break;
      }
      auto *propNode = cast<ESTree::PropertyNode>(&elem);

      ESTree::Node *valueNode = propNode->_value;
      ESTree::Node *init = nullptr;
      if (auto *assign =
              llvh::dyn_cast<ESTree::AssignmentPatternNode>(valueNode)) {
        valueNode = assign->_left;
        init = assign->_right;
      }

      Identifier nameHint = llvh::isa<ESTree::IdentifierNode>(valueNode)
          ? getNameFieldFromID(valueNode)
          : Identifier{};

      // FlowChecker validated the key is a non-computed identifier.
      Identifier key = getNameFieldFromID(
          llvh::cast<ESTree::IdentifierNode>(propNode->_key));
      excludedItems.push_back(Builder.getLiteralString(key));
      // The target is evaluated before the property is read.
      LReference lref = createLRef(valueNode, declInit);
      Value *loaded = Builder.createLoadPropertyInst(source, key);
      // With no initializer the cast narrows `T | void` to `T`, so a missing
      // key throws. With one, a missing key reads as `undefined` and selects
      // the initializer, whose type may differ from `T`, so only the loaded
      // value is narrowed.
      if (init) {
        Value *cast = Builder.createCheckedTypeCastInst(
            loaded,
            getTypeContext().unionTy(valueIRType, Type::createUndefined()));
        lref.emitStore(emitOptionalInitialization(cast, init, nameHint));
      } else {
        lref.emitStore(Builder.createCheckedTypeCastInst(loaded, valueIRType));
      }
    }
    return;
  }

  // Track which fields of \p srcType the named properties have consumed,
  // so a trailing rest element can be built from the remaining fields.
  llvh::BitVector consumed(srcType->getFields().size());

  for (auto &elem : target->_properties) {
    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&elem)) {
      // Allocate a new exact object containing the remaining fields, in the
      // same order as srcType. The order matches the rest binding's
      // ExactObjectType that FlowChecker constructed.
      AllocObjectLiteralInst::ObjectPropertyMap propMap{};
      llvh::SmallVector<size_t, 4> remainingSrcIdx{};
      for (size_t i = 0, e = srcType->getFields().size(); i < e; ++i) {
        if (consumed.test(i))
          continue;
        const auto &field = srcType->getFields()[i];
        propMap.emplace_back(
            Builder.getLiteralString(field.name),
            getDefaultInitValue(field.type));
        remainingSrcIdx.push_back(i);
      }

      Value *restObj = propMap.empty()
          ? Builder.createAllocObjectLiteralInst()
          : Builder.createAllocObjectLiteralInst(propMap);

      for (size_t destIdx = 0, n = remainingSrcIdx.size(); destIdx < n;
           ++destIdx) {
        size_t srcIdx = remainingSrcIdx[destIdx];
        const auto &field = srcType->getFields()[srcIdx];
        auto *nameLit = Builder.getLiteralString(field.name);
        Type irType = flowTypeToIRType(field.type);
        Value *loaded =
            Builder.createPrLoadInst(source, srcIdx, nameLit, irType);
        Builder.createPrStoreInst(
            loaded,
            restObj,
            destIdx,
            nameLit,
            getTypeContext().isNonPtr(irType));
      }

      LReference lref = createLRef(rest->_argument, declInit);
      lref.emitStore(restObj);
      break;
    }

    auto *propNode = cast<ESTree::PropertyNode>(&elem);

    ESTree::Node *valueNode = propNode->_value;
    ESTree::Node *init = nullptr;
    if (auto *assign =
            llvh::dyn_cast<ESTree::AssignmentPatternNode>(valueNode)) {
      valueNode = assign->_left;
      init = assign->_right;
    }

    Identifier nameHint = llvh::isa<ESTree::IdentifierNode>(valueNode)
        ? getNameFieldFromID(valueNode)
        : Identifier{};

    // FlowChecker already validated that the key is a non-computed identifier
    // and that the field exists in srcType.
    auto *keyId = llvh::cast<ESTree::IdentifierNode>(propNode->_key);
    Identifier key = getNameFieldFromID(keyId);
    auto optIdx = srcType->findField(key);
    assert(optIdx.hasValue() && "FlowChecker should have rejected this");
    size_t srcIdx = *optIdx;
    consumed.set(srcIdx);

    const auto &field = srcType->getFields()[srcIdx];
    // The target is evaluated before the property is read.
    LReference lref = createLRef(valueNode, declInit);
    Value *loaded = Builder.createPrLoadInst(
        source,
        srcIdx,
        Builder.getLiteralString(field.name),
        flowTypeToIRType(field.type));
    lref.emitStore(emitOptionalInitialization(loaded, init, nameHint));
  }
}

Value *ESTreeIRGen::emitOptionalInitialization(
    Value *value,
    ESTree::Node *init,
    Identifier nameHint) {
  if (!init)
    return value;

  auto *currentBlock = Builder.getInsertionBlock();
  auto *getDefaultBlock = Builder.createBasicBlock(Builder.getFunction());
  auto *storeBlock = Builder.createBasicBlock(Builder.getFunction());

  //    if (value !== undefined) goto storeBlock    [if initializer present]
  //    value = initializer                         [if initializer present]
  //  storeBlock:
  Builder.createCondBranchInst(
      Builder.createBinaryOperatorInst(
          value,
          Builder.getLiteralUndefined(),
          ValueKind::BinaryStrictlyNotEqualInstKind),
      storeBlock,
      getDefaultBlock);

  // getDefaultBlock:
  Builder.setInsertionBlock(getDefaultBlock);
  auto *defaultValue = genExpression(init, nameHint);
  auto *defaultResultBlock = Builder.getInsertionBlock();
  Builder.createBranchInst(storeBlock);

  // storeBlock:
  Builder.setInsertionBlock(storeBlock);
  return Builder.createPhiInst(
      {value, defaultValue}, {currentBlock, defaultResultBlock});
}

Instruction *ESTreeIRGen::emitLoad(Value *from, bool inhibitThrow) {
  if (auto *var = llvh::dyn_cast<Variable>(from)) {
    Instruction *res;
    auto *RSI = emitResolveScopeInstIfNeeded(var->getParent());
    if (var->getObeysTDZ()) {
      // We don't need to perform a runtime check for TDZ when in the
      // variable's function, since we know whether it has been initialized.
      // TODO(T182345760): Move this TDZ check into the resolver.
      if (llvh::isa<CreateScopeInst>(RSI)) {
        // If not initialized, throw.
        if (curFunction()->initializedTDZVars.count(var) == 0) {
          // Report an error or warning using the builder's location.
          assert(
              Builder.getLocation().isValid() &&
              "emitLoad() with invalid source location");
          if (Builder.getLocation().isValid()) {
            auto &sm = Mod->getContext().getSourceErrorManager();
            sm.message(
                Mod->getContext().getCodeGenerationSettings().test262
                    ? SourceErrorManager::DK_Warning
                    : SourceErrorManager::DK_Error,
                Builder.getLocation(),
                "TDZ violation: reading from uninitialized variable '" +
                    var->getName().str() + "'");
          }

          auto *thr = Builder.createThrowIfInst(
              Builder.getLiteralEmpty(), Type::createEmpty());
          // Pretend that the instruction, which always throws, returns a
          // value with the correct type.
          thr->setType(
              getTypeContext().subtractTy(var->getType(), Type::createEmpty()));
          thr->updateSavedResultType();
          res = thr;
        } else {
          res = Builder.createUnionNarrowTrustedInst(
              Builder.createLoadFrameInst(RSI, var),
              getTypeContext().subtractTy(var->getType(), Type::createEmpty()));
        }
      } else {
        res = Builder.createThrowIfInst(
            Builder.createLoadFrameInst(RSI, var), Type::createEmpty());
      }
    } else {
      res = Builder.createLoadFrameInst(RSI, var);
    }

    return res;
  } else if (auto *globalProp = llvh::dyn_cast<GlobalObjectProperty>(from)) {
    if (globalProp->isDeclared() || inhibitThrow) {
      return Builder.createLoadPropertyInst(
          Builder.getGlobalObject(), globalProp->getName());
    } else {
      return Builder.createTryLoadGlobalPropertyInst(globalProp);
    }
  } else {
    llvm_unreachable("invalid value to load from");
  }
}

void ESTreeIRGen::emitStore(Value *storedValue, Value *ptr, bool declInit) {
  if (auto *var = llvh::dyn_cast<Variable>(ptr)) {
    auto *RSI = emitResolveScopeInstIfNeeded(var->getParent());
    // TODO(T182345760): Move the TDZ tracking and checking into the resolver.
    if (declInit) {
      assert(
          llvh::isa<CreateScopeInst>(RSI) &&
          "variable must be initialized in its own function");

      // If this is a TDZ variable, record that it has been initialized.
      if (var->getObeysTDZ()) {
        curFunction()->initializedTDZVars.insert(var);
        // Note that a variable can be initialized more than once, for example
        // when a "var" declaration is shadowed by a catch variable or a
        // parameter.
      }
    } else {
      if (var->getObeysTDZ()) {
        // We don't need to perform a runtime check for TDZ when in the
        // variable's function, since we know whether it has been initialized.
        if (llvh::isa<CreateScopeInst>(RSI)) {
          if (curFunction()->initializedTDZVars.count(var) == 0) {
            // Report an error or warning using the builder's location.
            assert(
                Builder.getLocation().isValid() &&
                "emitStore() with invalid source location");
            if (Builder.getLocation().isValid()) {
              auto &sm = Mod->getContext().getSourceErrorManager();
              sm.message(
                  Mod->getContext().getCodeGenerationSettings().test262
                      ? SourceErrorManager::DK_Warning
                      : SourceErrorManager::DK_Error,
                  Builder.getLocation(),
                  "TDZ violation: writing to uninitialized variable '" +
                      var->getName().str() + "'");
            }

            auto thr = Builder.createThrowIfInst(
                Builder.getLiteralEmpty(), Type::createEmpty());
            thr->setType(Type::createUndefined());
            thr->updateSavedResultType();
          }
        } else {
          // Must verify whether the variable is initialized.
          Builder.createThrowIfInst(
              Builder.createLoadFrameInst(RSI, var), Type::createEmpty());
        }
      }
      auto constness = var->getConstness();
      if (constness == sema::Decl::Constness::Always ||
          (Builder.getFunction()->isStrictMode() &&
           constness == sema::Decl::Constness::StrictModeOnly)) {
        // Strict mode or always-const: throw TypeError at runtime.
        Builder.createThrowTypeErrorInst(Builder.getLiteralString(
            "assignment to constant variable '" + var->getName().str() + "'"));
        // Create a new block, since ThrowTypeError is a terminator.
        Builder.setInsertionBlock(
            Builder.createBasicBlock(Builder.getFunction()));
      } else if (constness == sema::Decl::Constness::StrictModeOnly) {
        // Per ES2025 §9.1.1.1.5 step 5.b: immutable-binding assignment
        // throws only if S=true; in sloppy mode (S=false) it's a no-op
        // and the binding keeps its initialized value.
        return;
      }
    }

    Builder.createStoreFrameInst(RSI, storedValue, var);
    return;
  } else if (auto *globalProp = llvh::dyn_cast<GlobalObjectProperty>(ptr)) {
    if (globalProp->isDeclared() || !Builder.getFunction()->isStrictMode()) {
      Builder.createStorePropertyInst(
          storedValue, Builder.getGlobalObject(), globalProp->getName());
    } else {
      Builder.createTryStoreGlobalPropertyInst(storedValue, globalProp);
    }
    return;
  } else {
    llvm_unreachable("invalid value to load from");
  }
}

Instruction *ESTreeIRGen::emitResolveScopeInstIfNeeded(
    VariableScope *targetVarScope) {
  auto [startScope, startVarScope] = getResolveScopeStart(
      curFunction()->curScope(),
      curFunction()->curScope()->getVariableScope(),
      targetVarScope);
  if (startVarScope == targetVarScope)
    return startScope;
  return Builder.createResolveScopeInst(
      targetVarScope, startVarScope, startScope);
}

VariableScope *FunctionContext::getOrCreateInnerVariableScope(
    ESTree::Node *node) {
  auto [it, inserted] = innerScopes_.insert({node, nullptr});
  if (inserted)
    it->second =
        irGen_->Builder.createVariableScope(currentScope_->getVariableScope());
  assert(
      it->second->getParentScope() == currentScope_->getVariableScope() &&
      "Inner scope created from multiple contexts");
  return it->second;
}

void ESTreeIRGen::drainCompilationQueue() {
  while (!compilationQueue_.empty()) {
    compilationQueue_.front()();
    compilationQueue_.pop_front();
  }
}

} // namespace irgen
} // namespace hermes
