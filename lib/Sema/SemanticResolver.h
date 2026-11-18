/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SEMA_SEMANTICRESOLVER_H
#define HERMES_SEMA_SEMANTICRESOLVER_H

#include "DeclCollector.h"
#include "hermes/ADT/PersistentScopedMap.h"
#include "hermes/AST/RecursiveVisitor.h"

namespace hermes {

using DeclarationFileListTy = std::vector<ESTree::ProgramNode *>;

namespace sema {

class ClassContext;
class FunctionContext;
class SemContext;

/// Class the performs all resolution.
/// Reports errors if validation fails.
class SemanticResolver
    : public ESTree::RecursionDepthTracker<SemanticResolver> {
  Context &astContext_;
  /// A copy of Context::getSM() for easier access.
  SourceErrorManager &sm_;

  /// Buffer all generated messages and print them sorted in the end.
  SourceErrorManager::SaveAndBufferMessages bufferMessages_;

  /// Global function context, nullptr until populated.
  FunctionContext *globalFunctionContext_{nullptr};

  /// All semantic tables are persisted here.
  SemContext &semCtx_;

  /// Keywords we will be checking for.
  const Keywords &kw_;

  /// If not null, a list of parsed files containing global ambient declarations
  /// that should be inserted in the global scope.
  const DeclarationFileListTy *ambientDecls_;

  /// A set of names that are restricted in the global scope.
  /// https://262.ecma-international.org/14.0/#sec-hasrestrictedglobalproperty
  /// ES14.0 9.1.1.4.14 HasRestrictedGlobalProperty:
  ///   Any global properties that are defined to be non-configurable
  ///   are restricted.
  llvh::SmallDenseSet<UniqueString *, 4> restrictedGlobalProperties_{};

  /// If not null, store all instances of DeclCollector here, for use by other
  /// passes.
  DeclCollectorMapTy *const saveDecls_;

  /// Current function context.
  FunctionContext *curFunctionContext_ = nullptr;

  /// Current class context.
  ClassContext *curClassContext_ = nullptr;

  /// Current lexical scope.
  LexicalScope *curScope_{nullptr};

  /// The currently lexically visible names.
  BindingTableTy &bindingTable_;

  /// The global scope.
  BindingTableScopePtrTy globalScope_;

  /// Whether this function can currently make super references. When entering a
  /// function that was defined using method syntax, a super binding exists.
  /// Arrow functions inherit this flag. The only other super bindings exist in
  /// class field initializer values and static blocks.
  bool canReferenceSuper_{false};

  /// True if we are preparing the AST to be compiled by Hermes, including
  /// erroring on features which we parse but don't compile and transforming
  /// the AST. False if we just want to validate the AST.
  bool compile_;

  /// True if we are preparing the AST for typed JS.
  bool typed_;

  /// 'await' isn't allowed to be an identifier anywhere in the parameters
  /// of an async arrow function, including the parameters of nested arrow
  /// functions in the parameter initializers.
  /// Ordinarily we'd check for this in the parser, but async arrow functions
  /// have a reparse step, so we avoid revisiting the entire tree by checking in
  /// SemanticResolver.
  bool forbidAwaitAsIdentifier_ = false;

  /// True if we are forbidding await expressions.
  bool forbidAwaitExpression_{false};

  /// True if we are forbidding the reference to the special 'arguments' object.
  bool forbidSpecialArgumentsReference_{false};

  /// True if 'arguments' cannot be used as any identifier.
  bool forbidArgumentsAsIdentifier_{false};

 public:
  /// This constant enables the more expensive path in RecursiveVisitorDispatch,
  /// enabling us to mutate NodeList.
  static constexpr bool kEnableNodeListMutation = true;

  /// \param semCtx the result of resolution will be stored here.
  /// \param saveDecls if not null, the map will contain all DeclCollector
  ///     instances for reuse by later passes.
  /// \param compile whether this resolution is intended to compile or just
  ///   parsing.
  /// \param typed whether this resolution is for typed code.
  explicit SemanticResolver(
      Context &astContext,
      sema::SemContext &semCtx,
      const DeclarationFileListTy *ambientDecls,
      DeclCollectorMapTy *saveDecls,
      bool compile,
      bool typed = false);

  explicit SemanticResolver(
      Context &astContext,
      sema::SemContext &semCtx,
      const DeclarationFileListTy *ambientDecls,
      bool compile)
      : SemanticResolver(astContext, semCtx, ambientDecls, nullptr, compile) {}

  /// Run semantic resolution and store the result in \c semCtx_.
  /// \param rootNode the top-level program/JS module node to run resolution on.
  /// \return false on error.
  bool run(ESTree::ProgramNode *rootNode);

  /// Run semantic resolution for a lazy function and store the result in \c
  /// semCtx_.
  /// \param rootNode the top-level function node to run resolution on.
  /// \param semInfo the original FunctionInfo for the root node,
  ///   which was created on the first pass and will be populated with real
  ///   scopes now.
  /// \param parentHadSuperBinding is true if the parent of \p rootNode could
  /// make a reference to super.
  /// \return false on error.
  bool runLazy(
      ESTree::FunctionLikeNode *rootNode,
      sema::FunctionInfo *semInfo,
      bool parentHadSuperBinding);

  /// Run semantic resolution for a program as if it was executed within an
  /// existing function scope and store the result in \c semCtx_.
  /// \param rootNode the top-level program node to run resolution on.
  /// \param semInfo the original FunctionInfo for the parent of the local eval.
  /// \param parentHadSuperBinding the context in which rootNode was declared in
  /// was allowed to make property references to `super`.
  /// \return false on error.
  bool runInScope(
      ESTree::ProgramNode *rootNode,
      sema::FunctionInfo *semInfo,
      bool parentHadSuperBinding);

  /// Validate and resolve a CommonJS function expression. It will use the
  /// existing global function and global scope, which must have been created
  /// by a previous invocation of \c run().
  bool runCommonJSModule(ESTree::FunctionExpressionNode *rootNode);

  /// \return a reference to the keywords struct.
  const Keywords &keywords() const {
    return kw_;
  }

  /// \return the current function context.
  FunctionContext *functionContext() {
    return curFunctionContext_;
  }
  /// \return the current function context.
  const FunctionContext *functionContext() const {
    return curFunctionContext_;
  }

  /// Extract the declared identifiers from a declaration AST node's "id" field.
  /// Normally that is just a single identifier, but it can be more in case of
  /// destructuring.
  /// \return true if the tree contains an expression, such as a default value
  /// or a computed property key (ES2026 15.1.2, ContainsExpression).
  bool extractDeclaredIdentsFromID(
      ESTree::Node *node,
      llvh::SmallVectorImpl<ESTree::IdentifierNode *> &idents);

  /// Default case for all ignored nodes, we still want to visit their children.
  void visit(ESTree::Node *node) {
    visitESTreeChildren(*this, node);
  }

  /// A private identifier is declared via a field, method, or accessor
  /// definition. This will report errors in any illegally duplicated private
  /// identifiers. Also add the private names to the binding table and associate
  /// `Decl`s with the corresponding private name IdentifierNodes.
  void collectDeclaredPrivateIdentifiers(ESTree::ClassLikeNode *node);
  void visit(ESTree::ProgramNode *node);

  void visit(ESTree::FunctionDeclarationNode *funcDecl, ESTree::Node *parent);
  void visit(ESTree::FunctionExpressionNode *funcExpr, ESTree::Node *parent);
  void visit(
      ESTree::ArrowFunctionExpressionNode *arrowFunc,
      ESTree::Node *parent);

  void visit(ESTree::IdentifierNode *identifier, ESTree::Node *parent);
  void visit(ESTree::ObjectPatternNode *node, ESTree::Node *parent) {
    visitESTreeNodeList(*this, node->_properties, node);
  }
  void visit(ESTree::ArrayPatternNode *node, ESTree::Node *parent) {
    visitESTreeNodeList(*this, node->_elements, node);
  }

  void visit(ESTree::VariableDeclarationNode *node);

  void visit(ESTree::BinaryExpressionNode *node, ESTree::Node **ppNode);
  void visit(ESTree::AssignmentExpressionNode *assignment);
  void visit(ESTree::UpdateExpressionNode *node);
  void visit(ESTree::UnaryExpressionNode *node, ESTree::Node **ppNode);

  void visit(ESTree::BlockStatementNode *node, ESTree::Node *parent);

  void visit(ESTree::SwitchStatementNode *node);

  void visit(ESTree::ForInStatementNode *node);
  void visit(ESTree::ForOfStatementNode *node);

  void visitForInOf(
      ESTree::LoopStatementNode *node,
      ESTree::ScopeDecorationBase *scopeDeco,
      ESTree::Node *&left,
      ESTree::Node *&right,
      ESTree::Node *&body);

  void visit(ESTree::ForStatementNode *node);

  void visit(ESTree::DoWhileStatementNode *node);
  void visit(ESTree::WhileStatementNode *node);

  void visit(ESTree::LabeledStatementNode *node);

  void visit(ESTree::BreakStatementNode *node);
  void visit(ESTree::ContinueStatementNode *node);

  void visit(ESTree::WithStatementNode *node);

  void visit(ESTree::TryStatementNode *tryStatement);
  void visit(ESTree::CatchClauseNode *node);

  void visit(ESTree::RegExpLiteralNode *regexp);

  void visit(ESTree::MetaPropertyNode *node);

  void visit(ESTree::ImportDeclarationNode *importDecl);

  void visit(ESTree::ClassDeclarationNode *node);
  void visit(ESTree::ClassExpressionNode *node);
  void visitClassAsExpr(ESTree::ClassLikeNode *node);
  void visit(ESTree::PrivateNameNode *node);
  void visit(ESTree::ClassPrivatePropertyNode *node);
  void visit(ESTree::ClassPropertyNode *node);
  void visit(ESTree::StaticBlockNode *node);
  void visit(ESTree::MethodDefinitionNode *node, ESTree::Node *parent);

  void visit(ESTree::SuperNode *node, ESTree::Node *parent);

  void visit(ESTree::CallExpressionNode *node);

  void visit(ESTree::MemberExpressionNode *node, ESTree::Node *parent);
  void visit(ESTree::OptionalMemberExpressionNode *node, ESTree::Node *parent);

  void visit(ESTree::SpreadElementNode *node, ESTree::Node *parent);

  void visit(ESTree::ReturnStatementNode *node);
  void visit(ESTree::YieldExpressionNode *node);
  void visit(ESTree::AwaitExpressionNode *awaitExpr);

  void visit(ESTree::ExportNamedDeclarationNode *node);
  void visit(ESTree::ExportDefaultDeclarationNode *node);
  void visit(ESTree::ExportAllDeclarationNode *node);

  void visit(ESTree::CoverEmptyArgsNode *node);
  void visit(ESTree::CoverTrailingCommaNode *node);
  void visit(ESTree::CoverInitializerNode *node);
  void visit(ESTree::CoverRestElementNode *node);
#if HERMES_PARSE_FLOW
  void visit(ESTree::CoverTypedIdentifierNode *node);
  void visit(ESTree::TypeAliasNode *node);
  void visit(ESTree::TypeParameterDeclarationNode *node);
  void visit(ESTree::TypeParameterInstantiationNode *node);
  void visit(ESTree::TypeCastExpressionNode *node);
  void visit(ESTree::AsExpressionNode *node);
  void visit(ESTree::MatchStatementNode *node);
  void visit(ESTree::MatchExpressionNode *node);
  void visit(
      ESTree::ComponentDeclarationNode *componentDecl,
      ESTree::Node *parent);
  void visit(ESTree::HookDeclarationNode *hookDecl, ESTree::Node *parent);
#endif
#if HERMES_PARSE_TS
  void visit(ESTree::TSTypeAliasDeclarationNode *node);
  void visit(ESTree::TSTypeParameterDeclarationNode *node);
  void visit(ESTree::TSTypeParameterInstantiationNode *node);
  void visit(ESTree::TSAsExpressionNode *node);
#endif

  friend class ClassContext;
  friend class FunctionContext;
  friend class ESTree::RecursionDepthTracker<SemanticResolver>;

 private:
  /// A RAII object automatic scopes.
  /// On construction it creates a new binding table scope and a new
  /// semantic scope, setting curScope_.
  /// On destruction it destroys the binding scope and resets curScope_.
  /// \param scopeDecoration if not null, will associate the new scope with
  ///   the node provided.
  class ScopeRAII {
   public:
    /// Create a binding scope and push a semantic scope.
    /// \param scopeNode is the AST node with which to associate the scope.
    /// \param isFunctionBodyScope whether this is the scope for the function
    ///   body of the current FunctionInfo.
    explicit ScopeRAII(
        SemanticResolver &resolver,
        ESTree::ScopeDecorationBase *scopeDecoration = nullptr,
        bool isFunctionBodyScope = false);

    /// Pops the created scope if it was pushed.
    ~ScopeRAII();

    BindingTableScopeTy &getBindingScope() {
      return bindingScope_;
    }

   private:
    /// The semantic context. If non-null, pop a scope on destruction.
    SemanticResolver &resolver_;
    /// Old LexicalScope to restore on pop.
    LexicalScope *oldScope_;
    /// The binding table scope.
    BindingTableScopeTy bindingScope_;
  };

  inline FunctionInfo *curFunctionInfo();
  inline const FunctionInfo *curFunctionInfo() const;

  /// Declare 'arguments' for use in either the function or the parameters.
  void declareArguments() {
    Decl *argsDecl =
        semCtx_.funcArgumentsDecl(curFunctionInfo(), kw_.identArguments);
    bindingTable_.try_emplace(kw_.identArguments, Binding{argsDecl, nullptr});
  }

  void visitFunctionLike(
      ESTree::FunctionLikeNode *node,
      ESTree::IdentifierNode *id,
      ESTree::Node *&body,
      ESTree::NodeList &params,
      ESTree::Node *parent);
  /// Visit a function-like node with the FunctionContext already created.
  /// Used by visitFunctionLike and by runLazy.
  void visitFunctionLikeInFunctionContext(
      ESTree::FunctionLikeNode *node,
      ESTree::IdentifierNode *id,
      ESTree::Node *&body,
      ESTree::NodeList &params);

  /// Visit the rest of the function body having visited the params already.
  /// NOTE: Used by visitFunctionLikeInFunctionContext to allow ScopeRAII to be
  /// conditionally declared in a more readable way.
  void visitFunctionBodyAfterParamsVisited(
      ESTree::FunctionLikeNode *node,
      ESTree::IdentifierNode *id,
      ESTree::Node *&body,
      ESTree::BlockStatementNode *blockBody,
      bool hasParameterNamedArguments);

  void visitFunctionExpression(
      ESTree::FunctionExpressionNode *node,
      ESTree::Node *&body,
      ESTree::NodeList &params,
      ESTree::Node *parent);

  /// Visit calls to module-related $SHBuiltin functions.
  void visitModuleFactory(ESTree::CallExpressionNode *node);
  void visitModuleExport(ESTree::CallExpressionNode *node);
  void visitModuleImport(ESTree::CallExpressionNode *node);

  /// Resolve an identifier to a declaration and record the resolution.
  /// Emit a warning for undeclared identifiers in strict mode.
  /// Record an undeclared global property if no declaration is found.
  /// \return the resolved Decl.
  Decl *resolveIdentifier(ESTree::IdentifierNode *identifier, bool inTypeof);

  /// Look up \p identifier to see if it already has been resolved or has a
  /// binding assigned.
  /// Assigns the associated declaration if it exists.
  /// \return the declaration, `nullptr` if unresolvable or failed to resolve.
  Decl *checkIdentifierResolved(ESTree::IdentifierNode *identifier);

  /// Create a new declaration of a private name in the current scope.
  /// \pre An IdentifierNode which has the same _name field has not been
  /// declared in this same LexicalScope.
  /// \param identifier the AST node containing the name
  /// \param kind the kind of decl to be created.
  /// \param isStatic is only valid for methods / accessors.
  /// \return the newly created decl.
  Decl *declarePrivateName(
      ESTree::IdentifierNode *identifier,
      Decl::Kind kind,
      bool isStatic = false);

  /// Resolve an identifier for a private name to a declaration and record the
  /// resolution. Emit an error for undeclared private names.
  /// \return null when no private name declaration could be found for \p
  /// identifier; at which point an error has been raised.
  Decl *resolvePrivateName(ESTree::IdentifierNode *identifier);

  /// Declare all declarations optionally associated with \p scopeNode
  /// by the DeclCollector in the current scope.
  void processCollectedDeclarations(ESTree::Node *scopeNode);

  /// Declare all declarations in \p decls by calling
  /// `validateAndDeclareIdentifier`.
  void processDeclarations(const ScopeDecls &decls);

  /// Declare all the function declarations in \p promotedFuncDecls with
  /// Var in function scope or GlobalProperty in global scope.
  /// Add the names to the function context's promotedFuncDecls list.
  void processPromotedFuncDecls(
      llvh::ArrayRef<ESTree::FunctionDeclarationNode *> promotedFuncDecls);

  /// Extract the list of declared identifiers in a declaration node and return
  /// the declaration kind of the node.
  /// Function declarations are returned as DeclKind::ScopedFunction,
  /// so they can be distinguished.
  Decl::Kind extractIdentsFromDecl(
      ESTree::Node *node,
      llvh::SmallVectorImpl<ESTree::IdentifierNode *> &idents);

  /// Try to create a declaration of the specified kind and name in the current
  /// scope. If the declaration is invalid, print an error message without
  /// creating it.
  /// \param declKind the semantic declaration kind
  /// \param idNode the AST node containing the name
  /// \param declNode the AST node of the declaration. Used for function
  ///     declarations.
  void validateAndDeclareIdentifier(
      Decl::Kind kind,
      ESTree::IdentifierNode *ident);

  /// Ensure that the specified identifier is valid to be used in a declaration.
  /// Return true if valid, otherwise generate an error and return false.
  bool validateDeclarationName(
      Decl::Kind declKind,
      const ESTree::IdentifierNode *idNode) const;

  /// Ensure that the specified node is a valid target for an assignment, in
  /// other words it is an l-value, a Pattern (checked recursively) or an Empty
  /// (used by elision).
  /// Report errors if any are found.
  void validateAssignmentTarget(ESTree::Node *node);

  /// \return true if the `node` is an LValue: a member expression or an
  /// identifier which is a valid LValue.
  bool isLValue(ESTree::Node *node);

  /// Information about directives encountered in the beginning of a program
  /// or function.
  struct FoundDirectives {
    /// If a "use strict" directive is found, this field points to the AST node
    /// (used for error reporting),otherwise it is nullptr.
    ESTree::Node *useStrictNode = nullptr;
    /// If a source visibility directive is found, it is stored here, otherwise
    /// the value is SourceVisibility::Default.
    SourceVisibility sourceVisibility = SourceVisibility::Default;
    /// If an "inline" directive is found, this is true.
    bool alwaysInline = false;
    /// If a "noinline" directive is found, this is true.
    bool noInline = false;
    /// If a "builtin" directive is found, this is true.
    bool builtin = false;
  };

  /// Scan the list of directives in the beginning of a program or function.
  /// (see ES5.1 4.1 - a directive is a statement consisting of a single
  /// string literal).
  /// \param body list of statements to scan.
  /// \return information about the encountered directives.
  FoundDirectives scanDirectives(ESTree::NodeList &body) const;

  /// Check if a function declaration has a "builtin" directive.
  bool hasBuiltinDirective(ESTree::FunctionDeclarationNode *funcDecl) const;

  /// Check if a function declaration has a Hermes.builtin decoration attached
  /// (set up by the typed-mode AST transform that consumes
  /// `Hermes.decorate(name, Hermes.builtin)` calls).
  bool hasBuiltinDecoration(ESTree::FunctionDeclarationNode *funcDecl) const;

  /// Mark \p scope and every one of its ancestor scopes as users of local
  /// `eval()`.
  static void registerLocalEval(LexicalScope *scope);

  // \param decl must be non-null.
  // \return whether the specified declaration is in the current function.
  bool declInCurFunction(Decl *decl) {
    assert(decl && "declInCurFunction requires non-null arg");
    return decl->scope->parentFunction == curFunctionInfo();
  }

  /// We call this when we exceed the maximum recursion depth.
  void recursionDepthExceeded(ESTree::Node *n);

  /// Declare the list of ambient decls that was passed to the constructor.
  void processAmbientDecls();
};

/// RAII class which holds per-function state during resolution,
/// including a pointer to the SemContext's \c FunctionInfo.
/// Includes label tables and collected declarations.
/// Must always be constructed on the stack.
/// At construction: pushes itself onto the \c functionStack_.
/// At destruction: pops itself from the \c functionStack_.
class FunctionContext {
  SemanticResolver &resolver_;

  FunctionContext *const prevContext_;

 public:
  struct Label {
    /// Where it was declared.
    ESTree::IdentifierNode *declarationNode;

    /// Statement targeted by the label. It is either a LoopStatement or a
    /// LabeledStatement.
    ESTree::StatementNode *targetStatement;
  };

  /// The associated seminfo object
  sema::FunctionInfo *semInfo;

  /// The AST node of the function.
  ESTree::FunctionLikeNode *const node;

  /// The currently active labels in the function.
  llvh::DenseMap<ESTree::NodeLabel, Label> labelMap;

  /// Most nested active loop statement.
  ESTree::LoopStatementNode *currentLoop{nullptr};

  /// The most nested active loop or switch statement.
  ESTree::StatementNode *currentLoopOrSwitch{nullptr};

  /// True if we are validating a formal parameter list.
  bool isFormalParams{false};

  /// All declarations in the function.
  std::unique_ptr<DeclCollector> decls;

  /// The map of names that have been promoted to function scope by
  /// promoteScopedFunctionDecls in this function, mapped to their Var
  /// declaration in function scope.
  llvh::DenseMap<UniqueString *, Decl *> promotedFuncDecls{};

  /// The depth of the function's scope in the binding table.
  /// Populated when ScopeRAII is created within the function.
  uint32_t bindingTableScopeDepth = 0;

  /// Just a tag for readability when invoking the special constructor.
  struct ExistingGlobalScopeTag {};

  /// Create a function context for the existing global scope.
  explicit FunctionContext(
      SemanticResolver &resolver,
      FunctionInfo *globalSemInfo,
      ExistingGlobalScopeTag);

  explicit FunctionContext(
      SemanticResolver &resolver,
      ESTree::FunctionLikeNode *node,
      FunctionInfo *parentSemInfo,
      bool strict,
      FunctionInfo::ConstructorKind consKind,
      CustomDirectives customDirectives);

  explicit FunctionContext(
      SemanticResolver &resolver,
      FunctionInfo *newFunctionInfo);

  /// Although a static block is not technically a function, we should be able
  /// to construct a function context starting from a static block node. This is
  /// because static blocks introduce a new "var"-scope in the same way that
  /// function nodes do. So we must be able to collect and initialize \p decls
  /// starting from the static block node.
  explicit FunctionContext(
      SemanticResolver &resolver,
      ESTree::StaticBlockNode *node,
      FunctionInfo *newFunctionInfo);

  /// Tag for readability to show lazy compilation constructor.
  struct LazyTag {};

  /// Constructor for lazy compilation, takes existing \p semInfoLazy.
  explicit FunctionContext(
      SemanticResolver &resolver,
      ESTree::FunctionLikeNode *node,
      FunctionInfo *semInfoLazy,
      LazyTag);

  ~FunctionContext();

  /// \return true if this is the "global scope" function context, in other
  /// words not a real function.
  bool isGlobalScope() const {
    return this == resolver_.globalFunctionContext_;
  }

  /// \return the optional function name, or nullptr.
  UniqueString *getFunctionName() const;
};

inline FunctionInfo *SemanticResolver::curFunctionInfo() {
  return functionContext()->semInfo;
}
const FunctionInfo *SemanticResolver::curFunctionInfo() const {
  return functionContext()->semInfo;
}

/// A "context" (an RAII class that records state on entry to a scope, and
/// restores the previous state on exit from the scope) recording information
/// about the current class when the semantic resolver is within a class.
class ClassContext {
 public:
  /// Create a class context for the class with the given \p classDeclaration.
  ClassContext(SemanticResolver &resolver, ESTree::ClassLikeNode *classNode);

  /// May only be called after the body of the current class has been
  /// visited, and hasConstructor, below, is valid.  If the current
  /// class has no explicit constructor, creates a FunctionInfo for an
  /// implicit constructor, and stores it in the context.  Will be
  /// returned by subsequent getImplicitConstructorFunctionInfo calls,
  /// below.
  void createImplicitConstructorFunctionInfo();

  /// On first call, creates a FunctionInfo for an implicit function to do the
  /// instance elements initializations. On subsequent calls, return that
  /// FunctionInfo.
  FunctionInfo *getOrCreateInstanceElementsInitFunctionInfo();

  /// Get or create a synthetic function information for the static elements
  /// initializer of a class.
  FunctionInfo *getOrCreateStaticElementsInitFunctionInfo();

  /// Create a synthetic function information for a static initialization block.
  FunctionInfo *createStaticBlockFunctionInfo(ESTree::StaticBlockNode *node);

  /// \return true if the current class of this context is a derived class.
  bool isDerivedClass() const {
    // It's a derived class if it has a super class node.
    return getSuperClass(classNode_);
  }

  /// Whether the class has an explicit constructor.  Only valid after
  /// the body of the class has been visited.
  bool hasConstructor = false;

  ~ClassContext();

 private:
  SemanticResolver &resolver_;
  ClassContext *const prevContext_;

  /// The (decorator of) the class of the context.  Will store the
  /// member initializer function info if one is required.
  ESTree::ClassLikeNode *classNode_;
};

/// Visitor pass for marking variables as Unresolvable based on local `eval()`
/// or `with`.
class Unresolver {
 public:
  /// Mark all declarations that are at a lower depth than \p depth as
  /// unresolvable, starting at \p root.
  static void run(SemContext &semCtx, uint32_t depth, ESTree::Node *root);

  void visit(ESTree::Node *node) {
    visitESTreeChildren(*this, node);
  }

  void visit(ESTree::IdentifierNode *node);

  /// Needed by RecursiveVisitorDispatch. Optionally can protect against too
  /// deep nesting.
  bool incRecursionDepth(ESTree::Node *) {
    return true;
  }
  void decRecursionDepth() {}

 private:
  explicit Unresolver(SemContext &semCtx, uint32_t depth)
      : semCtx_(semCtx), depth_(depth) {}

 private:
  SemContext &semCtx_;

  /// Depth of the scope which contains the construct which could shadow
  /// variables dynamically.
  /// e.g. the depth of the function containing a local `eval()`.
  uint32_t depth_;
};

/// Runs a conservative check to determine whether there are any possible paths
/// through the function which end in an implicit 'undefined' return.
/// \param root the function, which has already been successfully run through
///   SemanticResolver's function visitor.
/// \return true if \p root can run the implicit 'return undefined'.
bool mayReachImplicitReturn(ESTree::FunctionLikeNode *root);

} // namespace sema
} // namespace hermes

#endif
