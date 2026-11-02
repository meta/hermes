/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ESTreeIRGen.h" // @manual

#include "llvh/ADT/SetVector.h"
#include "llvh/ADT/StringSet.h"
#include "llvh/Support/Debug.h"
#include "llvh/Support/SaveAndRestore.h"

#include <tuple>

namespace hermes {
namespace irgen {

//===----------------------------------------------------------------------===//
// Free standing helpers.
Instruction *loadGlobalObjectProperty(
    IRBuilder &builder,
    GlobalObjectProperty *from,
    bool inhibitThrow) {
  if (from->isDeclared() || inhibitThrow) {
    return builder.createLoadPropertyInst(
        builder.getGlobalObject(), from->getName());
  }
  return builder.createTryLoadGlobalPropertyInst(from);
}

/// \returns true if \p node is a constant expression.
bool isConstantExpr(ESTree::Node *node) {
  // TODO: a little more aggressive constant folding.
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

/// Adds the \p parent -> \p child link in the nesting graph.
static void
buildDummyLexicalParent(IRBuilder &builder, Function *parent, Function *child) {
  // FunctionScopeAnalysis works through CreateFunctionInsts, so we have to add
  // that even though these functions are never invoked.
  auto *block = builder.createBasicBlock(parent);
  builder.setInsertionBlock(block);
  builder.createUnreachableInst();
  auto *csi = builder.createCreateScopeInst(parent->getFunctionScopeDesc());
  auto *inst = builder.createCreateFunctionInst(child, csi);
  builder.createReturnInst(inst);
}

static void populateScopeFromChainLink(
    IRBuilder &builder,
    ScopeDesc *scope,
    const SerializedScope &chainLink) {
  for (const auto &var : chainLink.variables) {
    Variable *V = builder.createVariable(scope, var.declKind, var.name);
    if (var.declKind == Variable::DeclKind::Const) {
      V->setStrictImmutableBinding(var.strictImmutableBinding);
    }
  }
}

static std::tuple<Function *, ScopeDesc *> materializeScopeChain(
    IRBuilder &builder,
    Function *outmostFunction,
    const SerializedScopePtr &chain) {
  if (!chain) {
    ScopeDesc *S = outmostFunction->getFunctionScopeDesc();
    S->setSerializedScope(chain);
    return std::make_tuple(outmostFunction, S);
  }

  if (!chain->parentScope) {
    ScopeDesc *S = outmostFunction->getFunctionScopeDesc();
    populateScopeFromChainLink(builder, S, *chain);
    S->setSerializedScope(chain);
    return std::make_tuple(outmostFunction, S);
  }

  auto [currFunction, parentScope] =
      materializeScopeChain(builder, outmostFunction, chain->parentScope);

  ScopeDesc *newScope = parentScope->createInnerScope();
  if (!chain->originalName.isValid()) {
    newScope->setFunction(currFunction);
  } else {
    Function *newFunc = builder.createFunction(
        newScope,
        chain->originalName,
        Function::DefinitionKind::ES5Function,
        false,
        SourceVisibility::Sensitive,
        {},
        false);
    buildDummyLexicalParent(builder, currFunction, newFunc);
    currFunction = newFunc;
  }
  populateScopeFromChainLink(builder, newScope, *chain);
  newScope->setSerializedScope(chain);
  return std::make_tuple(currFunction, newScope);
}

static std::tuple<Function *, ScopeDesc *> materializeScopeChainForEval(
    IRBuilder &builder,
    ESTree::ProgramNode *program,
    const SerializedScopePtr &chain) {
  Function *outerFunction = builder.createFunction(
      builder.getModule()->getInitialScope()->createInnerScope(),
      "",
      Function::DefinitionKind::ES5Function,
      ESTree::isStrict(program->strictness),
      program->sourceVisibility,
      program->getSourceRange(),
      true);

  return std::make_tuple(
      outerFunction,
      std::get<ScopeDesc *>(
          materializeScopeChain(builder, outerFunction, chain)));
}

static std::tuple<Function *, ScopeDesc *> materializeScopeChainForLazyFunction(
    IRBuilder &builder,
    hbc::LazyCompilationData *lazyData,
    const SerializedScopePtr &chain) {
  Function *topLevel = builder.createTopLevelFunction(
      builder.getModule()->getInitialScope()->createInnerScope(),
      lazyData->strictMode);

  return std::make_tuple(
      topLevel,
      std::get<ScopeDesc *>(materializeScopeChain(builder, topLevel, chain)));
}

/// Recursively populate \p nameTable with \p scopeDesc's (and its parents')
/// variables.
static void populateNameTable(NameTableTy &nameTable, ScopeDesc *scopeDesc) {
  if (auto *parent = scopeDesc->getParent()) {
    // If scopeDesc has a parent scope, first add the definitions in it. This
    // way, scopeDesc definitions will take priority over those in the parents.
    populateNameTable(nameTable, parent);
  }

  // If scope->closureAlias is specified, we must create an alias binding
  // between originalName (which must be valid) and the variable identified by
  // closureAlias.
  //
  // We do this *before* inserting the other variables below to reflect that
  // the closure alias is conceptually in an outside scope and also avoid the
  // closure name incorrectly shadowing the same name inside the closure.
  if (SerializedScopePtr link = scopeDesc->getSerializedScope()) {
    if (link->closureAlias.isValid()) {
      assert(link->originalName.isValid() && "Original name invalid");
      assert(
          link->originalName != link->closureAlias &&
          "Original name must be different from the alias");

      // NOTE: the closureAlias target must exist and must be a Variable.
      auto *closureVar = cast<Variable>(nameTable.lookup(link->closureAlias));

      // Re-create the alias.
      nameTable.insert(link->originalName, closureVar);
    }
  }

  for (Variable *var : scopeDesc->getVariables()) {
    nameTable.insert(var->getName(), var);
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
      return builder.createLoadPropertyInst(base_, property_);
    case Kind::VarOrGlobal:
      return irgen_->emitLoad(base_);
    case Kind::Destructuring:
      assert(false && "destructuring cannot be loaded");
      return builder.getLiteralUndefined();
    case Kind::Error:
      return builder.getLiteralUndefined();
  }

  llvm_unreachable("invalid LReference kind");
}

void LReference::emitStore(Value *value) {
  auto &builder = getBuilder();

  switch (kind_) {
    case Kind::Empty:
      return;
    case Kind::Member:
      builder.createStorePropertyInst(value, base_, property_);
      return;
    case Kind::VarOrGlobal:
      irgen_->emitStore(value, base_, declInit_);
      return;
    case Kind::Error:
      return;
    case Kind::Destructuring:
      return irgen_->emitDestructuringAssignment(
          declInit_, destructuringTarget_, value);
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
    ESTree::Node *root,
    const DeclarationFileListTy &declFileList,
    Module *M,
    const ScopeChain &scopeChain)
    : Mod(M),
      Builder(Mod),
      instrumentIR_(M, Builder),
      Root(root),
      DeclarationFileList(declFileList),
      lexicalScopeChain(resolveScopeIdentifiers(scopeChain)),
      identEval_(Builder.createIdentifier("eval")),
      identLet_(Builder.createIdentifier("let")),
      identDefaultExport_(Builder.createIdentifier("?default")) {}

void ESTreeIRGen::doIt() {
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
  Function *topLevelFunction;

  // Function context used only when compiling in an existing lexical scope
  // chain. It is only initialized if we have a lexical scope chain.
  llvh::Optional<FunctionContext> wrapperFunctionContext{};

  if (!lexicalScopeChain) {
    topLevelFunction = Builder.createTopLevelFunction(
        Mod->getInitialScope()->createInnerScope(),
        ESTree::isStrict(Program->strictness),
        Program->sourceVisibility,
        Program->getSourceRange());

  } else {
    // If compiling in an existing lexical context, we need to install the
    // scopes in a wrapper function, which represents the "global" code.
    auto [wrapperFunction, parentScope] =
        materializeScopeChainForEval(Builder, Program, lexicalScopeChain);

    // Initialize the wrapper context.
    wrapperFunctionContext.emplace(this, wrapperFunction, nullptr);
    currentIRScopeDesc_ = parentScope;

    // Restore the previously saved parent scopes.
    populateNameTable(nameTable_, parentScope);

    // Finally create the function which will actually be executed.
    topLevelFunction = Builder.createFunction(
        parentScope->createInnerScope(),
        "eval",
        Function::DefinitionKind::ES5Function,
        ESTree::isStrict(Program->strictness),
        Program->sourceVisibility,
        Program->getSourceRange(),
        false);

    buildDummyLexicalParent(
        Builder, parentScope->getFunction(), topLevelFunction);
  }

  Mod->setTopLevelFunction(topLevelFunction);

  // Function context for topLevelFunction.
  FunctionContext topLevelFunctionContext{
      this, topLevelFunction, Program->getSemInfo()};

  // IRGen needs a pointer to the outer-most context, which is either
  // topLevelContext or wrapperFunctionContext, depending on whether the latter
  // was created.
  // We want to set the pointer to that outer-most context, but ensure that it
  // doesn't outlive the context it is pointing to.
  llvh::SaveAndRestore<FunctionContext *> saveTopLevelContext(
      topLevelContext,
      !wrapperFunctionContext.hasValue() ? &topLevelFunctionContext
                                         : &wrapperFunctionContext.getValue());

  // Now declare all externally supplied global properties, but only if we don't
  // have a lexical scope chain.
  if (!lexicalScopeChain) {
    for (auto declFile : DeclarationFileList) {
      processDeclarationFile(declFile);
    }
  }

  emitFunctionPreamble(Builder.createBasicBlock(topLevelFunction));
  initCaptureStateInES5Function();
  emitTopLevelDeclarations(Program, Program, DoEmitParameters::Yes);

  Value *retVal;
  {
    // Allocate the return register, initialize it to undefined.
    curFunction()->globalReturnRegister =
        Builder.createAllocStackInst(genAnonymousLabelName("ret"));
    Builder.createStoreStackInst(
        Builder.getLiteralUndefined(), curFunction()->globalReturnRegister);

    // Emit all CreateFunctionInsts for all global functions.
    hoistCreateFunctions(Program);

    genBody(Program->_body);

    // Terminate the top-level scope with a return statement.
    retVal = Builder.createLoadStackInst(curFunction()->globalReturnRegister);
  }

  emitFunctionEpilogue(retVal);
}

void ESTreeIRGen::doCJSModule(
    Function *topLevelFunction,
    sem::FunctionInfo *semInfo,
    uint32_t segmentID,
    uint32_t id,
    llvh::StringRef filename) {
  assert(Root && "no root in ESTreeIRGen");
  auto *func = cast<ESTree::FunctionExpressionNode>(Root);
  assert(func && "doCJSModule without a module");

  FunctionContext topLevelFunctionContext{this, topLevelFunction, semInfo};
  llvh::SaveAndRestore<FunctionContext *> saveTopLevelContext(
      topLevelContext, &topLevelFunctionContext);

  // Now declare all externally supplied global properties, but only if we don't
  // have a lexical scope chain.
  assert(
      !lexicalScopeChain &&
      "Lexical scope chain not supported for CJS modules");
  for (auto declFile : DeclarationFileList) {
    processDeclarationFile(declFile);
  }

  Identifier functionName = Builder.createIdentifier("cjs_module");
  Function *newFunc = genES5Function(functionName, nullptr, func);

  Builder.getModule()->addCJSModule(
      segmentID, id, Builder.createIdentifier(filename), newFunc);
}

std::pair<Function *, Function *> ESTreeIRGen::doLazyFunction(
    hbc::LazyCompilationData *lazyData) {
  lexicalScopeChain = lazyData->parentScope;

  // Create a top level function that will never be executed, because:
  // 1. IRGen assumes the first function always has global scope
  // 2. It serves as the root for dummy functions for lexical data
  auto [topLevel, parentScope] = materializeScopeChainForLazyFunction(
      Builder, lazyData, lexicalScopeChain);

  FunctionContext topLevelFunctionContext{this, topLevel, nullptr};

  // Note that topLevel is the Module's top level function (i.e., global()), so
  // currentIRScopeDesc_ is the descriptor for global()'s function scope. What
  // we really need instead is the lazy function's parent scope in as the
  // previous scope. Thus, manually set it here. Another option would be to
  // recursively "install" the FunctionContexts of all encompassing functions,
  // but that's not really necessary -- by setting currentIRScopeDesc_ to
  // parentScope, the ESTreeIRGen object is in a "compatible" state, ready to
  // generate code for the lazy function.
  currentIRScopeDesc_ = parentScope;

  // Save the top-level context, but ensure it doesn't outlive what it is
  // pointing to.
  llvh::SaveAndRestore<FunctionContext *> saveTopLevelContext(
      topLevelContext, &topLevelFunctionContext);

  auto *node = cast<ESTree::FunctionLikeNode>(Root);

  // Now restore scoping information in two separate ways.
  populateNameTable(nameTable_, parentScope);

  // If lazyData->closureAlias is specified, we must create an alias binding
  // between originalName (which must be valid) and the variable identified by
  // closureAlias.
  Variable *parentVar = nullptr;
  if (lazyData->closureAlias.isValid()) {
    assert(lazyData->originalName.isValid() && "Original name invalid");
    assert(
        lazyData->originalName != lazyData->closureAlias &&
        "Original name must be different from the alias");

    // NOTE: the closureAlias target must exist and must be a Variable.
    parentVar = cast<Variable>(nameTable_.lookup(lazyData->closureAlias));

    // Re-create the alias.
    nameTable_.insert(lazyData->originalName, parentVar);
  }

  assert(
      !llvh::isa<ESTree::ArrowFunctionExpressionNode>(node) &&
      "lazy compilation not supported for arrow functions");

  // Generators have had their lazy scope set up without setting one up
  // for the inner functions. This means that we will never directly generate
  // a GeneratorInnerFunction here.
  Function *func = ESTree::isAsync(node)
      ? genAsyncFunction(lazyData->originalName, parentVar, node)
      : ESTree::isGenerator(node)
      ? genGeneratorFunction(lazyData->originalName, parentVar, node)
      : genES5Function(lazyData->originalName, parentVar, node, false);

  // Now add the link between parentScope's function and func, completing the
  // nesting graph for the lazy function.
  buildDummyLexicalParent(Builder, parentScope->getFunction(), func);
  return {func, topLevel};
}

std::pair<Value *, bool> ESTreeIRGen::declareVariableOrGlobalProperty(
    ScopeDesc *inScope,
    VarDecl::Kind declKind,
    Identifier name) {
  Value *found = nameTable_.lookup(name);

  // If the variable is already declared in this scope, do not create a
  // second instance.
  if (found) {
    if (auto *var = llvh::dyn_cast<Variable>(found)) {
      if (var->getParent() == inScope)
        return {found, false};
    } else {
      assert(
          llvh::isa<GlobalObjectProperty>(found) &&
          "Invalid value found in name table");
      if (inScope->isGlobalScope() &&
          (declKind == VarDecl::Kind::Var ||
           !Mod->getContext().getCodeGenerationSettings().enableBlockScoping)) {
        // Only return the global property that was found if this is the global
        // scope and the JS source is declaring a "var" global (i.e., a
        // GlobalObjectProperty). This causes code like
        //
        // let undefined;
        //
        // to define a global variable, which will then, at runtime, be checked
        // with ThrowIfHasRestrictedGlobalProperty to ensure it is a valid
        // lexically scoped name for the given program.
        return {found, false};
      }
    }
  }

  // Create a property if global scope, variable otherwise.
  Value *res;
  NameTableScopeTy *nameTableScope;
  if (inScope->isGlobalScope() && declKind == VarDecl::Kind::Var) {
    nameTableScope = topLevelContext->functionScope;
    res = Builder.createGlobalObjectProperty(name, true);
  } else {
    if (!Mod->getContext().getCodeGenerationSettings().enableTDZ) {
      // short-circuit all declarations to be Var. TDZ dies here.
      declKind = Variable::DeclKind::Var;
    }

    nameTableScope = curFunction()->blockScope;
    res = Builder.createVariable(inScope, declKind, name);
  }

  // Register the variable in the scoped hash table.
  nameTable_.insertIntoScope(nameTableScope, name, res);
  return {res, true};
}

GlobalObjectProperty *ESTreeIRGen::declareAmbientGlobalProperty(
    Identifier name) {
  // Avoid redefining global properties.
  auto *prop = dyn_cast_or_null<GlobalObjectProperty>(nameTable_.lookup(name));
  if (prop)
    return prop;

  LLVM_DEBUG(
      llvh::dbgs() << "declaring ambient global property " << name << " "
                   << name.getUnderlyingPointer() << "\n");

  prop = Builder.createGlobalObjectProperty(name, false);
  nameTable_.insertIntoScope(topLevelContext->functionScope, name, prop);
  return prop;
}

namespace {
/// This visitor structs collects declarations within a single closure without
/// descending into child closures.
struct DeclHoisting {
  /// The list of collected identifiers (variables and functions).
  llvh::SmallVector<ESTree::VariableDeclaratorNode *, 8> decls{};

  /// A list of functions that need to be hoisted and materialized before we
  /// can generate the rest of the function.
  llvh::SmallVector<ESTree::FunctionDeclarationNode *, 8> closures;

  explicit DeclHoisting() = default;
  ~DeclHoisting() = default;

  /// Extract the variable name from the nodes that can define new variables.
  /// The nodes that can define a new variable in the scope are:
  /// VariableDeclarator and FunctionDeclaration>
  void collectDecls(ESTree::Node *V) {
    if (auto VD = llvh::dyn_cast<ESTree::VariableDeclaratorNode>(V)) {
      return decls.push_back(VD);
    }

    if (auto FD = llvh::dyn_cast<ESTree::FunctionDeclarationNode>(V)) {
      return closures.push_back(FD);
    }
  }

  bool shouldVisit(ESTree::Node *V) {
    // Collect declared names, even if we don't descend into children nodes.
    collectDecls(V);

    // Do not descend to child closures because the variables they define are
    // not exposed to the outside function.
    if (llvh::isa<ESTree::FunctionDeclarationNode>(V) ||
        llvh::isa<ESTree::FunctionExpressionNode>(V) ||
        llvh::isa<ESTree::ArrowFunctionExpressionNode>(V))
      return false;
    return true;
  }

  void enter(ESTree::Node *V) {}
  void leave(ESTree::Node *V) {}
};

} // anonymous namespace.

void ESTreeIRGen::processDeclarationFile(ESTree::ProgramNode *programNode) {
  auto Program = dyn_cast_or_null<ESTree::ProgramNode>(programNode);
  if (!Program)
    return;

  DeclHoisting DH;
  Program->visit(DH);

  // Create variable declarations for each of the hoisted variables.
  for (auto vd : DH.decls)
    declareAmbientGlobalProperty(getNameFieldFromID(vd->_id));
  for (auto fd : DH.closures)
    declareAmbientGlobalProperty(getNameFieldFromID(fd->_id));
}

Value *ESTreeIRGen::ensureVariableExists(ESTree::IdentifierNode *id) {
  assert(id && "id must be a valid Identifier node");
  Identifier name = getNameFieldFromID(id);

  // Check if this is a known variable.
  if (auto *var = nameTable_.lookup(name))
    return var;

  if (curFunction()->function->isStrictMode()) {
    // Report a warning in strict mode.
    auto currentFunc = Builder.getInsertionBlock()->getParent();

    Builder.getModule()->getContext().getSourceErrorManager().warning(
        Warning::UndefinedVariable,
        id->getSourceRange(),
        Twine("the variable \"") + name.str() + "\" was not declared in " +
            currentFunc->getDescriptiveDefinitionKindStr() + " \"" +
            currentFunc->getInternalNameStr() + "\"");
  }

  // Undeclared variable is an ambient global property.
  return declareAmbientGlobalProperty(name);
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
    return dyn_cast_or_null<Variable>(
        nameTable_.lookup(getNameFieldFromID(iden)));
  }

  return false;
}

LReference ESTreeIRGen::createLRef(ESTree::Node *node, bool declInit) {
  SMLoc sourceLoc = node->getDebugLoc();
  IRBuilder::ScopedLocationChange slc(Builder, sourceLoc);

  if (llvh::isa<ESTree::EmptyNode>(node)) {
    LLVM_DEBUG(llvh::dbgs() << "Creating an LRef for EmptyNode.\n");
    return LReference(
        LReference::Kind::Empty, this, false, nullptr, nullptr, sourceLoc);
  }

  /// Create lref for member expression (ex: o.f).
  if (auto *ME = llvh::dyn_cast<ESTree::MemberExpressionNode>(node)) {
    LLVM_DEBUG(llvh::dbgs() << "Creating an LRef for member expression.\n");
    Value *obj = genExpression(ME->_object);
    Value *prop = genMemberExpressionProperty(ME);
    return LReference(
        LReference::Kind::Member, this, false, obj, prop, sourceLoc);
  }

  /// Create lref for identifiers  (ex: a).
  if (auto *iden = llvh::dyn_cast<ESTree::IdentifierNode>(node)) {
    LLVM_DEBUG(llvh::dbgs() << "Creating an LRef for identifier.\n");
    LLVM_DEBUG(
        llvh::dbgs() << "Looking for identifier \"" << getNameFieldFromID(iden)
                     << "\"\n");
    auto *var = ensureVariableExists(iden);
    return LReference(
        LReference::Kind::VarOrGlobal, this, declInit, var, nullptr, sourceLoc);
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
      LReference::Kind::Error, this, false, nullptr, nullptr, sourceLoc);
}

Value *ESTreeIRGen::genHermesInternalCall(
    llvh::StringRef name,
    Value *thisValue,
    ArrayRef<Value *> args) {
  return Builder.createCallInst(
      CallInst::kNoTextifiedCallee,
      Builder.createLoadPropertyInst(
          Builder.createTryLoadGlobalPropertyInst("HermesInternal"), name),
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
  // FIXME: use the builtin value of @@iterator. Symbol could have been
  // overridden.
  return Builder.createLoadPropertyInst(
      Builder.createTryLoadGlobalPropertyInst("Symbol"), "iterator");
}

ESTreeIRGen::IteratorRecordSlow ESTreeIRGen::emitGetIteratorSlow(Value *obj) {
  auto *method = Builder.createLoadPropertyInst(obj, emitIteratorSymbol());
  auto *iterator =
      Builder.createCallInst(CallInst::kNoTextifiedCallee, method, obj, {});

  emitEnsureObject(iterator, "iterator is not an object");
  auto *nextMethod = Builder.createLoadPropertyInst(iterator, "next");

  return {iterator, nextMethod};
}

Value *ESTreeIRGen::emitIteratorNextSlow(IteratorRecordSlow iteratorRecord) {
  auto *nextResult = Builder.createCallInst(
      CallInst::kNoTextifiedCallee,
      iteratorRecord.nextMethod,
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

void ESTreeIRGen::emitIteratorCloseSlow(
    hermes::irgen::ESTreeIRGen::IteratorRecordSlow iteratorRecord,
    bool ignoreInnerException) {
  auto *haveReturn = Builder.createBasicBlock(Builder.getFunction());
  auto *noReturn = Builder.createBasicBlock(Builder.getFunction());

  auto *returnMethod = genBuiltinCall(
      BuiltinMethod::HermesBuiltin_getMethod,
      {iteratorRecord.iterator, Builder.getLiteralString("return")});
  Builder.createCompareBranchInst(
      returnMethod,
      Builder.getLiteralUndefined(),
      BinaryOperatorInst::OpKind::StrictlyEqualKind,
      noReturn,
      haveReturn);

  Builder.setInsertionBlock(haveReturn);
  if (ignoreInnerException) {
    emitTryCatchScaffolding(
        noReturn,
        // emitBody.
        [this, returnMethod, &iteratorRecord]() {
          Builder.createCallInst(
              CallInst::kNoTextifiedCallee,
              returnMethod,
              iteratorRecord.iterator,
              {});
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
    auto *innerResult = Builder.createCallInst(
        CallInst::kNoTextifiedCallee,
        returnMethod,
        iteratorRecord.iterator,
        {});
    emitEnsureObject(innerResult, "iterator.return() did not return an object");
    Builder.createBranchInst(noReturn);
  }

  Builder.setInsertionBlock(noReturn);
}

ESTreeIRGen::IteratorRecord ESTreeIRGen::emitGetIterator(Value *obj) {
  // Each of these will be modified by "next", so we use a stack storage.
  auto *iterStorage =
      Builder.createAllocStackInst(genAnonymousLabelName("iter"));
  auto *sourceOrNext =
      Builder.createAllocStackInst(genAnonymousLabelName("sourceOrNext"));
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
  else if (auto *OPN = llvh::dyn_cast<ESTree::ObjectPatternNode>(target))
    return emitDestructuringObject(declInit, OPN, source);
  else {
    Mod->getContext().getSourceErrorManager().error(
        target->getSourceRange(), "unsupported destructuring target");
  }
}

void ESTreeIRGen::emitDestructuringArray(
    bool declInit,
    ESTree::ArrayPatternNode *targetPat,
    Value *source) {
  const IteratorRecord iteratorRecord = emitGetIterator(source);

  /// iteratorDone = undefined.
  auto *iteratorDone =
      Builder.createAllocStackInst(genAnonymousLabelName("iterDone"));
  Builder.createStoreStackInst(Builder.getLiteralUndefined(), iteratorDone);

  auto *value =
      Builder.createAllocStackInst(genAnonymousLabelName("iterValue"));

  SharedExceptionHandler handler{};
  handler.exc = Builder.createAllocStackInst(genAnonymousLabelName("exc"));
  // All exception handlers branch to this block.
  handler.exceptionBlock = Builder.createBasicBlock(Builder.getFunction());

  bool first = true;
  bool emittedRest = false;
  // The LReference created in the previous iteration of the destructuring
  // loop. We need it because we want to put the previous store and the creation
  // of the next LReference under one try block.
  llvh::Optional<LReference> lref;

  /// If the previous LReference is valid and non-empty, store "value" into
  /// it and reset the LReference.
  auto storePreviousValue = [&lref, &handler, this, value]() {
    if (lref && !lref->isEmpty()) {
      if (lref->canStoreWithoutSideEffects()) {
        lref->emitStore(Builder.createLoadStackInst(value));
      } else {
        // If we can't store without side effects, wrap the store in try/catch.
        emitTryWithSharedHandler(&handler, [this, &lref, value]() {
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
      emitRestElement(declInit, rest, iteratorRecord, iteratorDone, &handler);
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
      emitTryWithSharedHandler(
          &handler, [this, &lref, value, target, declInit]() {
            // Store the previous value, if we have one.
            if (lref && !lref->isEmpty())
              lref->emitStore(Builder.createLoadStackInst(value));
            lref = createLRef(target, declInit);
          });
    }

    // Pseudocode of the algorithm for a step:
    //
    //   value = undefined;
    //   if (iteratorDone) goto nextBlock
    // notDoneBlock:
    //   stepResult = IteratorNext(iteratorRecord)
    //   stepDone = IteratorComplete(stepResult)
    //   iteratorDone = stepDone
    //   if (stepDone) goto nextBlock
    // newValueBlock:
    //   value = IteratorValue(stepResult)
    // nextBlock:
    //   if (value !== undefined) goto storeBlock    [if initializer present]
    //   value = initializer                         [if initializer present]
    // storeBlock:
    //   lref.emitStore(value)

    auto *notDoneBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *newValueBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *nextBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *getDefaultBlock =
        init ? Builder.createBasicBlock(Builder.getFunction()) : nullptr;
    auto *storeBlock =
        init ? Builder.createBasicBlock(Builder.getFunction()) : nullptr;

    Builder.createStoreStackInst(Builder.getLiteralUndefined(), value);

    // In the first iteration we know that "done" is false.
    if (first) {
      first = false;
      Builder.createBranchInst(notDoneBlock);
    } else {
      Builder.createCondBranchInst(
          Builder.createLoadStackInst(iteratorDone), nextBlock, notDoneBlock);
    }

    // notDoneBlock:
    Builder.setInsertionBlock(notDoneBlock);
    auto *stepValue = emitIteratorNext(iteratorRecord);
    auto *stepDone = emitIteratorComplete(iteratorRecord);
    Builder.createStoreStackInst(stepDone, iteratorDone);
    Builder.createCondBranchInst(
        stepDone, init ? getDefaultBlock : nextBlock, newValueBlock);

    // newValueBlock:
    Builder.setInsertionBlock(newValueBlock);
    Builder.createStoreStackInst(stepValue, value);
    Builder.createBranchInst(nextBlock);

    // nextBlock:
    Builder.setInsertionBlock(nextBlock);

    // NOTE: we can't use emitOptionalInitializationHere() because we want to
    // be able to jump directly to getDefaultBlock.
    if (init) {
      //    if (value !== undefined) goto storeBlock    [if initializer present]
      //    value = initializer                         [if initializer present]
      //  storeBlock:
      Builder.createCondBranchInst(
          Builder.createBinaryOperatorInst(
              Builder.createLoadStackInst(value),
              Builder.getLiteralUndefined(),
              BinaryOperatorInst::OpKind::StrictlyNotEqualKind),
          storeBlock,
          getDefaultBlock);

      Identifier nameHint = llvh::isa<ESTree::IdentifierNode>(target)
          ? getNameFieldFromID(target)
          : Identifier{};

      // getDefaultBlock:
      Builder.setInsertionBlock(getDefaultBlock);
      Builder.createStoreStackInst(genExpression(init, nameHint), value);
      Builder.createBranchInst(storeBlock);

      // storeBlock:
      Builder.setInsertionBlock(storeBlock);
    }
  }

  storePreviousValue();

  // If in the end the iterator is not done, close it. We only need to do
  // that if we didn't end with a rest element because it would have exhausted
  // the iterator.
  if (!emittedRest) {
    auto *notDoneBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *doneBlock = Builder.createBasicBlock(Builder.getFunction());
    Builder.createCondBranchInst(
        Builder.createLoadStackInst(iteratorDone), doneBlock, notDoneBlock);

    Builder.setInsertionBlock(notDoneBlock);
    emitIteratorClose(iteratorRecord, false);
    Builder.createBranchInst(doneBlock);

    Builder.setInsertionBlock(doneBlock);
  }

  // If we emitted at least one try block, generate the exception handler.
  if (handler.emittedTry) {
    IRBuilder::SaveRestore saveRestore{Builder};
    Builder.setInsertionBlock(handler.exceptionBlock);

    auto *notDoneBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *doneBlock = Builder.createBasicBlock(Builder.getFunction());

    Builder.createCondBranchInst(
        Builder.createLoadStackInst(iteratorDone), doneBlock, notDoneBlock);

    Builder.setInsertionBlock(notDoneBlock);
    emitIteratorClose(iteratorRecord, true);
    Builder.createBranchInst(doneBlock);

    Builder.setInsertionBlock(doneBlock);
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

void ESTreeIRGen::emitRestElement(
    bool declInit,
    ESTree::RestElementNode *rest,
    hermes::irgen::ESTreeIRGen::IteratorRecord iteratorRecord,
    hermes::AllocStackInst *iteratorDone,
    SharedExceptionHandler *handler) {
  // 13.3.3.8 BindingRestElement:...BindingIdentifier

  auto *notDoneBlock = Builder.createBasicBlock(Builder.getFunction());
  auto *newValueBlock = Builder.createBasicBlock(Builder.getFunction());
  auto *doneBlock = Builder.createBasicBlock(Builder.getFunction());

  llvh::Optional<LReference> lref;
  if (canCreateLRefWithoutSideEffects(rest->_argument)) {
    lref = createLRef(rest->_argument, declInit);
  } else {
    emitTryWithSharedHandler(handler, [this, &lref, rest, declInit]() {
      lref = createLRef(rest->_argument, declInit);
    });
  }

  auto *A = Builder.createAllocArrayInst({}, 0);
  auto *n = Builder.createAllocStackInst(genAnonymousLabelName("n"));

  // n = 0.
  Builder.createStoreStackInst(Builder.getLiteralPositiveZero(), n);

  Builder.createCondBranchInst(
      Builder.createLoadStackInst(iteratorDone), doneBlock, notDoneBlock);

  // notDoneBlock:
  Builder.setInsertionBlock(notDoneBlock);
  auto *stepValue = emitIteratorNext(iteratorRecord);
  auto *stepDone = emitIteratorComplete(iteratorRecord);
  Builder.createStoreStackInst(stepDone, iteratorDone);
  Builder.createCondBranchInst(stepDone, doneBlock, newValueBlock);

  // newValueBlock:
  Builder.setInsertionBlock(newValueBlock);
  auto *nVal = Builder.createLoadStackInst(n);
  nVal->setType(Type::createNumber());
  // A[n] = stepValue;
  // Unfortunately this can throw because our arrays can have limited range.
  // The spec doesn't specify what to do in this case, but the reasonable thing
  // to do is to what we would if this was a for-of loop doing the same thing.
  // See section BindingRestElement:...BindingIdentifier, step f and g:
  // https://www.ecma-international.org/ecma-262/9.0/index.html#sec-destructuring-binding-patterns-runtime-semantics-iteratorbindinginitialization
  emitTryWithSharedHandler(handler, [this, stepValue, A, nVal]() {
    Builder.createStorePropertyInst(stepValue, A, nVal);
  });
  // ++n;
  auto add = Builder.createBinaryOperatorInst(
      nVal, Builder.getLiteralNumber(1), BinaryOperatorInst::OpKind::AddKind);
  add->setType(Type::createNumber());
  Builder.createStoreStackInst(add, n);
  Builder.createBranchInst(notDoneBlock);

  // doneBlock:
  Builder.setInsertionBlock(doneBlock);
  if (lref->canStoreWithoutSideEffects()) {
    lref->emitStore(A);
  } else {
    emitTryWithSharedHandler(handler, [&lref, A]() { lref->emitStore(A); });
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
            source,
            Builder.getLiteralNull(),
            BinaryOperatorInst::OpKind::EqualKind),
        throwBB,
        doneBB);

    Builder.setInsertionBlock(throwBB);
    genBuiltinCall(
        BuiltinMethod::HermesBuiltin_throwTypeError,
        {source,
         Builder.getLiteralString(
             "Cannot destructure 'undefined' or 'null'.")});
    // throwTypeError will always throw.
    // This throw is here to ensure well-formed IR, and will not run.
    Builder.createThrowInst(Builder.getLiteralUndefined());

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
    // This size is only a hint as the true size may change if there are
    // duplicates when computedExcludedItems is processed at run-time.
    auto excludedSizeHint =
        literalExcludedItems.size() + computedExcludedItems.size();
    // Explicitly set the prototype for the object created here so it isn't
    // initialized to Object.prototype, which may be modified by the user.
    excludedObj = Builder.createAllocObjectInst(
        excludedSizeHint, Builder.getLiteralNull());

    for (Literal *key : literalExcludedItems)
      Builder.createStoreNewOwnPropertyInst(
          zeroValue, excludedObj, key, IRBuilder::PropEnumerable::Yes);

    for (Value *key : computedExcludedItems) {
      Builder.createStorePropertyInst(zeroValue, excludedObj, key);
    }
  }

  auto *restValue = genBuiltinCall(
      BuiltinMethod::HermesBuiltin_copyDataProperties,
      {Builder.createAllocObjectInst(0), source, excludedObj});

  lref.emitStore(restValue);
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
          BinaryOperatorInst::OpKind::StrictlyNotEqualKind),
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

SerializedScopePtr ESTreeIRGen::resolveScopeIdentifiers(
    const ScopeChain &chain) {
  SerializedScopePtr current{};
  for (auto it = chain.scopes.rbegin(), end = chain.scopes.rend(); it < end;
       it++) {
    auto next = std::make_shared<SerializedScope>();
    next->variables.reserve(it->variables.size());
    for (auto var : it->variables) {
      constexpr bool immutableBinding = false;
      next->variables.push_back(
          SerializedScope::Declaration{
              Builder.createIdentifier(var),
              Variable::DeclKind::Var,
              immutableBinding});
    }
    next->parentScope = current;
    current = next;
  }
  return current;
}

SerializedScopePtr ESTreeIRGen::serializeScope(
    ScopeDesc *S,
    bool includeGlobal) {
  // Serialize the global scope if and only if it's the only scope.
  // We serialize the global scope to avoid re-declaring variables,
  // and only do it once to avoid creating spurious scopes.
  if (!S->getParent() || S->getSerializedScope()) {
    return S->getSerializedScope();
  }

  auto scope = std::make_shared<SerializedScope>();
  auto *func = S->getFunction();

  if (S == func->getFunctionScopeDesc()) {
    scope->originalName = func->getOriginalOrInferredName();
    if (auto *closure = func->getLazyClosureAlias()) {
      scope->closureAlias = closure->getName();
    }
  }
  for (auto *var : S->getVariables()) {
    scope->variables.push_back(
        SerializedScope::Declaration{
            var->getName(),
            var->getDeclKind(),
            var->getStrictImmutableBinding()});
  }
  scope->parentScope = serializeScope(S->getParent(), false);
  return scope;
}

Instruction *ESTreeIRGen::emitLoad(Value *from, bool inhibitThrow) {
  if (auto *var = llvh::dyn_cast<Variable>(from)) {
    Instruction *res = Builder.createLoadFrameInst(var, currentIRScope_);
    if (var->getObeysTDZ())
      res = Builder.createThrowIfEmptyInst(res);
    return res;
  } else if (auto *globalProp = llvh::dyn_cast<GlobalObjectProperty>(from)) {
    return loadGlobalObjectProperty(Builder, globalProp, inhibitThrow);
  } else {
    llvm_unreachable("invalid value to load from");
  }
}

Instruction *
ESTreeIRGen::emitStore(Value *storedValue, Value *ptr, bool declInit) {
  if (auto *var = llvh::dyn_cast<Variable>(ptr)) {
    if (!declInit && var->getObeysTDZ()) {
      // Must verify whether the variable is initialized.
      Builder.createThrowIfEmptyInst(
          Builder.createLoadFrameInst(var, currentIRScope_));
      if (var->getDeclKind() == Variable::DeclKind::Const) {
        if (!var->getStrictImmutableBinding() &&
            !curFunction()->function->isStrictMode()) {
          // Ignore stores to non-strict immutable bindings in non-strict mode.
          return nullptr;
        }

        // Assignment to const-declared variables should result in a runtime
        // TypeError exception, so raise it here.
        Builder.getModule()->getContext().getSourceErrorManager().warning(
            Builder.getLocation(), "Assignment to constant variable");
        genRaiseNativeError(
            Builder, NativeErrorTypes::TypeError, "can't modify const value");
        // genRaiseNativeError emits a ThrowInst, which is itself a terminator;
        // thus we can't append more instructions to the current basic block.
        // Create another basic block, and set it as the insertion pointer. The
        // newly-created basic block is unreachable, and will thus be removed
        // during optimization.
        BasicBlock *unlinked =
            Builder.createBasicBlock(curFunction()->function);
        Builder.setInsertionBlock(unlinked);
      }
    }
    return Builder.createStoreFrameInst(storedValue, var, currentIRScope_);
  } else if (auto *globalProp = llvh::dyn_cast<GlobalObjectProperty>(ptr)) {
    if (globalProp->isDeclared() || !Builder.getFunction()->isStrictMode())
      return Builder.createStorePropertyInst(
          storedValue, Builder.getGlobalObject(), globalProp->getName());
    else
      return Builder.createTryStoreGlobalPropertyInst(storedValue, globalProp);
  } else {
    llvm_unreachable("unvalid value to load from");
  }
}

void ESTreeIRGen::newDeclarativeEnvironment() {
  ScopeDesc *blockScopeDesc = currentIRScopeDesc_->createInnerScope();
  blockScopeDesc->setFunction(curFunction()->function);
  currentIRScopeDesc_ = blockScopeDesc;
  currentIRScope_ =
      Builder.createCreateInnerScopeInst(currentIRScope_, blockScopeDesc);
  Builder.setCurrentSourceLevelScope(currentIRScopeDesc_);
}

void ESTreeIRGen::blockDeclarationInstantiation(ESTree::Node *containingNode) {
  newDeclarativeEnvironment();
  createScopeBindings(currentIRScopeDesc_, containingNode);
  genFunctionDeclarations(containingNode);
  hoistCreateFunctions(containingNode);
}

Instruction *ESTreeIRGen::genRaiseNativeError(
    IRBuilder &builder,
    NativeErrorTypes id,
    llvh::StringRef msg) {
  return builder.createThrowInst(builder.createCallInst(
      CallInst::kNoTextifiedCallee,
      builder.createCallBuiltinInst(
          BuiltinMethod::HermesBuiltin_getOriginalNativeErrorConstructor,
          builder.getLiteralNumber(static_cast<unsigned>(id))),
      builder.getLiteralUndefined(),
      builder.getLiteralString(msg)));
}

} // namespace irgen
} // namespace hermes
