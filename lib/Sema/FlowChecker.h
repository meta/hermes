/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SEMA_FLOWCHECKER_H
#define HERMES_SEMA_FLOWCHECKER_H

#include "DeclCollector.h"

#include "hermes/ADT/PersistentScopedMap.h"
#include "hermes/AST/Context.h"
#include "hermes/AST/RecursiveVisitor.h"
#include "hermes/Sema/FlowContext.h"
#include "hermes/Sema/SemContext.h"
#include "hermes/Support/StackExecutor.h"
#include "llvh/ADT/BitVector.h"

#include "llvh/ADT/SetVector.h"

#if HERMES_PARSE_FLOW

namespace hermes {
namespace flow {

/// Specialization to allow hashing of types in accordance with the
/// TypeInfo::hash and TypeInfo::equals methods.
struct TypeArrayDenseMapInfo
    : public llvh::DenseMapInfo<llvh::ArrayRef<TypeInfo *>> {
  // static inline T getEmptyKey();
  // static inline T getTombstoneKey();
  static unsigned getHashValue(llvh::ArrayRef<TypeInfo *> val) {
    llvh::hash_code hash{0};
    for (const TypeInfo *type : val)
      hash = llvh::hash_combine(hash, type->hash());
    return hash;
  }
  static bool isEqual(
      llvh::ArrayRef<TypeInfo *> LHS,
      llvh::ArrayRef<TypeInfo *> RHS) {
    if (RHS.data() == getEmptyKey().data())
      return LHS.data() == getEmptyKey().data();
    if (RHS.data() == getTombstoneKey().data())
      return LHS.data() == getTombstoneKey().data();
    if (LHS.size() != RHS.size())
      return false;
    for (size_t i = 0, e = LHS.size(); i < e; ++i)
      if (!LHS[i]->equals(RHS[i]))
        return false;
    return true;
  }
};

/// Class the performs all resolution.
/// Reports errors if validation fails.
class FlowChecker : public ESTree::RecursionDepthTracker<FlowChecker> {
  /// AST context.
  Context &astContext_;

  /// A copy of Context::getSM() for easier access.
  SourceErrorManager &sm_;

  /// Buffer all generated messages and print them sorted in the end.
  SourceErrorManager::SaveAndBufferMessages bufferMessages_;

  sema::SemContext &semContext_;

  /// Flow types are stored here.
  FlowContext &flowContext_;

  /// Map of DeclCollector instances associated with every function.
  sema::DeclCollectorMapTy &declCollectorMap_;

  /// Keywords we will be checking for.
  const Keywords &kw_;

  /// Use an 8MB stack, which is the default size on mac and linux.
  static constexpr size_t kExecutorStackSize = 1 << 23;

  /// The executor used for cloning generics.
  std::shared_ptr<StackExecutor> stackExecutor_ =
      newStackExecutor(kExecutorStackSize);

  struct TypeDecl {
    /// nullptr is used to indicate a generic declaration.
    Type *type;
    /// The lexical scope in which the type was declared.
    /// TODO: this is not used, should we remove it?
    sema::LexicalScope *scope;
    /// Optional AST node for reporting re-declarations.
    ESTree::Node *astNode;
    /// When this is a class generic declaration, the Decl of the class.
    sema::Decl *genericClassDecl;
    /// Whether this shadows a class type parameter inside a static member,
    /// where referencing it is an error.
    bool staticTypeParam = false;
    explicit TypeDecl(
        Type *type,
        sema::LexicalScope *scope,
        ESTree::Node *astNode,
        sema::Decl *genericClassDecl = nullptr)
        : type(type),
          scope(scope),
          astNode(astNode),
          genericClassDecl(genericClassDecl) {
#ifndef NDEBUG
      if (genericClassDecl)
        assert(!type && "generic type must be null");
      assert(scope && "scope must be non-null");
#endif
    }
  };

  /// The scoped binding table mapping from string to binding.
  using TypeBindingTableTy =
      hermes::PersistentScopedMap<UniqueString *, TypeDecl>;
  using TypeBindingTableScopeTy =
      hermes::PersistentScopedMapScope<UniqueString *, TypeDecl>;
  using TypeBindingTableScopePtrTy =
      hermes::PersistentScopedMapScopePtr<UniqueString *, TypeDecl>;

  /// The currently lexically visible names of types.
  TypeBindingTableTy bindingTable_{};

  /// Information about the current function.
  class FunctionContext;

  /// The current function context.
  FunctionContext *curFunctionContext_ = nullptr;

  /// Information about the current class.
  class ClassContext;

  /// The current class context.
  ClassContext *curClassContext_ = nullptr;

  /// Holds information needed on how to instantiate a generic.
  /// \param Spec the type of the specialization, either ESTree::Node (for
  ///   generics that clone the AST) or Type (for generic type aliases, which
  ///   create a new Type).
  template <class Spec>
  class GenericInfo {
   public:
    /// Storage for type arguments.
    /// The Type pointers point to the storage in FlowContext, so they're
    /// stable.
    /// SmallVector is usable here because once these vectors are added to
    /// the typeArgStorage_ deque, they are never modified and won't move.
    using TypeArgsVector = llvh::SmallVector<TypeInfo *, 2>;

    /// Reference to a TypeArgs, used for storing in the DenseMap.
    using TypeArgsRef = llvh::ArrayRef<TypeInfo *>;

    /// Full AST for the generic.
    /// e.g. if it's a generic function, this is the FunctionLikeNode.
    ESTree::Node *originalNode;

    /// Parent of the generic declaration.
    /// Contains the NodeList to insert the AST into.
    ESTree::Node *parent;

    /// The binding table scope for the generic.
    /// Activated when the generic is instantiated.
    TypeBindingTableScopePtrTy bindingTableScope;

    /// The lexical scope of the class containing this generic method.
    /// Null for non-method generics.
    sema::LexicalScope *classScope = nullptr;

    /// The ClassType containing this generic method.
    /// Null for non-method generics.
    flow::ClassType *containingClassType = nullptr;

    /// The type parameters of the containing class, shadowed when
    /// specializing a static method. Null if the class isn't generic.
    ESTree::TypeParameterDeclarationNode *classTypeParams = nullptr;

    /// The number of type parameters declared on the generic.
    /// Cached to avoid re-computing it from the AST.
    size_t numTypeParams;

    /// Map from the list of type arguments to the specialization.
    llvh::DenseMap<TypeArgsRef, Spec *, TypeArrayDenseMapInfo>
        specializations{};

   public:
    /// \param bindingTableScope takes shared ownership of the binding table
    ///   scope.
    GenericInfo(
        ESTree::Node *originalNode,
        ESTree::Node *parent,
        const TypeBindingTableScopePtrTy &bindingTableScope)
        : originalNode(originalNode),
          parent(parent),
          bindingTableScope(bindingTableScope),
          numTypeParams(computeNumTypeParams(originalNode)) {
      // We need to insert into the parent.
      assert(
          (llvh::isa<ESTree::ProgramNode>(parent) ||
           llvh::isa<ESTree::BlockStatementNode>(parent) ||
           llvh::isa<ESTree::ClassBodyNode>(parent)) &&
          "Invalid parent");
    }

    GenericInfo(const GenericInfo &other) = delete;
    GenericInfo operator=(const GenericInfo &other) = delete;

    /// \return the specialization if it exists, otherwise nullptr.
    Spec *getSpecialization(TypeArgsRef args) {
      auto it = specializations.find(args);
      if (it != specializations.end())
        return it->second;
      return nullptr;
    }

    /// Add the specialization for \p args to the \c specializations map.
    /// \return a reference to the internal storage containing the type args.
    TypeArgsRef addSpecialization(
        FlowChecker &outer,
        TypeArgsVector &&args,
        Spec *specialization) {
      outer.typeArgStorage_.emplace_back(std::move(args));
      auto [it, inserted] = specializations.try_emplace(
          TypeArgsRef{outer.typeArgStorage_.back()}, specialization);
      assert(inserted && "double specialization not allowed");
      return it->first;
    }

   private:
    /// \return the number of type parameters declared on \p originalNode, based
    ///   on its node kind (generic function, class, method, or type alias).
    static size_t computeNumTypeParams(ESTree::Node *originalNode) {
      ESTree::Node *typeParams = nullptr;
      if (auto *func = llvh::dyn_cast<ESTree::FunctionLikeNode>(originalNode)) {
        typeParams = ESTree::getTypeParameters(func);
      } else if (
          auto *classDecl =
              llvh::dyn_cast<ESTree::ClassDeclarationNode>(originalNode)) {
        typeParams = classDecl->_typeParameters;
      } else if (
          auto *method =
              llvh::dyn_cast<ESTree::MethodDefinitionNode>(originalNode)) {
        typeParams = llvh::cast<ESTree::FunctionExpressionNode>(method->_value)
                         ->_typeParameters;
      } else if (
          auto *alias = llvh::dyn_cast<ESTree::TypeAliasNode>(originalNode)) {
        typeParams = alias->_typeParameters;
      }
      return typeParams
          ? llvh::cast<ESTree::TypeParameterDeclarationNode>(typeParams)
                ->_params.size()
          : 0;
    }
  };

  /// Types associated with declarations.
  llvh::DenseMap<const sema::Decl *, Type *> &declTypes_;
  /// The AST node for every declaration. Used for reporting re-declarations.
  llvh::DenseMap<sema::Decl *, ESTree::Node *> declNodes_{};

  /// Record the variable initializers visited when attempting to infer the
  /// types of declarations in order to avoid visiting them again.
  llvh::DenseSet<ESTree::Node *> visitedInits_{};

  /// True if we are preparing the AST to be compiled by Hermes, including
  /// erroring on features which we parse but don't compile and transforming
  /// the AST. False if we just want to validate the AST.
  bool const compile_;

  /// Mapping from native type names to their enum code. Populated by
  /// \c declareNativeTypes()
  llvh::DenseMap<UniqueString *, NativeCType> nativeTypes_;

  /// Storage for the TypeArgs vectors.
  /// Elements of typeArgStorage_ contain the storage for the keys in
  /// \c GenericInfo::specializations.
  /// Once stored here, the TypeArgsVectors are not modified, so TypeArgsRef
  /// pointers into the vectors can be stored.
  std::deque<GenericInfo<ESTree::Node>::TypeArgsVector> typeArgStorage_{};

  /// Stable storage for the GenericInfo.
  std::deque<GenericInfo<ESTree::Node>> generics_;

  /// Maps from the Decl to a reference to an element of \c generics_.
  /// Populated values are not nullable.
  /// Store a pointer to allow for empty/tombstone values in DenseMap.
  llvh::DenseMap<const sema::Decl *, GenericInfo<ESTree::Node> *> genericsMap_;

  /// Maps from MethodDefinitionNode to GenericInfo for generic methods.
  /// Generic methods don't have a sema::Decl, so they are keyed by the
  /// MethodDefinitionNode instead.
  llvh::DenseMap<
      const ESTree::MethodDefinitionNode *,
      GenericInfo<ESTree::Node> *>
      genericMethodsMap_;

  /// Stable storage for the GenericInfo.
  std::deque<GenericInfo<Type>> genericAliases_;

  /// Maps from the Decl to a reference to an element of \c generics_.
  /// Populated values are not nullable.
  /// Store a pointer to allow for empty/tombstone values in DenseMap.
  llvh::DenseMap<const ESTree::TypeAliasNode *, GenericInfo<Type> *>
      genericAliasesMap_;

  /// Description of the generic that we have deferred typechecking for.
  struct DeferredGenericClass {
    /// The declaration node.
    ESTree::ClassDeclarationNode *specialization;
    /// The scope where the generic was defined,
    /// with the parameter names bound to the corresponding type variables.
    TypeBindingTableScopePtrTy scope;
    /// The type for the deferred generic.
    Type *classType;
    /// The ClassConstructorType wrapping classType.
    Type *classConsType;
    /// The class's own lexical scope (where private names are declared).
    sema::LexicalScope *classScope;
    /// The genericClassDepth_ of the specialization.
    unsigned depth;

    DeferredGenericClass(
        ESTree::ClassDeclarationNode *specialization,
        TypeBindingTableScopePtrTy scope,
        Type *declType,
        Type *classConsType,
        sema::LexicalScope *classScope,
        unsigned depth)
        : specialization(specialization),
          scope(scope),
          classType(declType),
          classConsType(classConsType),
          classScope(classScope),
          depth(depth) {}
  };

  /// A generic bound comparison to perform after type resolution.
  struct GenericBoundCheck {
    /// The concrete type argument.
    Type *argument;
    /// The bound annotation, resolved when the check is performed, because
    /// the types it refers to may not be resolved yet when it is queued.
    ESTree::Node *boundAnnotation;
    /// The binding table scope in which to resolve the bound, with the type
    /// parameters bound.
    TypeBindingTableScopePtrTy scope;
    /// The genericClassDepth_ in which to resolve the bound.
    unsigned depth;
    /// The generic parameter name.
    UniqueString *parameterName;
    /// The source range used for diagnostics.
    SMRange errorRange;
  };

  /// List of generics that we haven't parsed yet because they might
  /// refer to other generics that haven't been parsed yet.
  std::vector<DeferredGenericClass> *deferredParseGenerics_ = nullptr;

  /// Bound checks deferred until the active scope resolver completes types.
  llvh::SmallVectorImpl<GenericBoundCheck> *deferredGenericBoundChecks_ =
      nullptr;

  /// Queue of the generics that we haven't finished typechecking yet,
  /// which need their bodies typechecked.
  std::deque<DeferredGenericClass> typecheckQueue_;

  /// Maximum genericClassDepth_ of a generic class specialization.
  static constexpr unsigned kMaxGenericClassDepth = 100;

  /// Number of generic class specializations whose processing led to the one
  /// currently being processed, 0 outside of any.
  /// Their bodies are queued rather than checked recursively, so this is what
  /// bounds expansions like `class A<T> { m(): void { new A<T[]>(); } }`.
  unsigned genericClassDepth_ = 0;

  /// The Decl for the generic Array<T> class, identified by @Hermes.array.
  sema::Decl *arrayClassDecl_ = nullptr;
  /// The TypeDecl for the generic Array<T> class in the binding table.
  TypeDecl *arrayTypeDecl_ = nullptr;

  /// Maps MemberExpressionNode to its resolved builtin FunctionDeclarationNode.
  /// Used to pass builtin info from MemberExpressionNode to CallExpressionNode.
  llvh::DenseMap<
      ESTree::MemberExpressionNode *,
      ESTree::FunctionDeclarationNode *>
      builtinMethodDecls_;

 public:
  explicit FlowChecker(
      Context &astContext,
      sema::SemContext &semContext,
      FlowContext &flowContext,
      sema::DeclCollectorMapTy &declCollectorMap,
      bool compile);

  /// Run semantic resolution and store the result in \c semCtx_.
  /// \param rootNode the top-level program/JS module node to run resolution on.
  /// \return false on error.
  bool run(ESTree::ProgramNode *rootNode);

  /// Executed once at the top scope to define all native types. This is a hack.
  /// Eventually we should have a proper module system.
  void declareNativeTypes(sema::LexicalScope *rootScope);

  /// Populate the builtin method table from functions with "builtin" directive.
  void populateBuiltinMethods();

  /// Record that \p node resolves to the builtin \p decl.
  void setBuiltinMethodDecl(
      ESTree::MemberExpressionNode *node,
      ESTree::FunctionDeclarationNode *decl) {
    builtinMethodDecls_[node] = decl;
  }

  /// Get the builtin function declaration for \p node, or nullptr if not found.
  ESTree::FunctionDeclarationNode *getBuiltinMethodDecl(
      ESTree::MemberExpressionNode *node) {
    auto it = builtinMethodDecls_.find(node);
    return it != builtinMethodDecls_.end() ? it->second : nullptr;
  }

  /// We call this when we exceed the maximum recursion depth.
  void recursionDepthExceeded(ESTree::Node *n);

  /// Default case for all ignored nodes, we still want to visit their children.
  void visit(ESTree::Node *node) {
    visitESTreeChildren(*this, node);
  }

  void visit(ESTree::ProgramNode *node);

  /// Run typechecking on a function that's known to not be generic.
  /// Called from visit(FunctionDeclarationNode *) when there are no type
  /// parameters, or after cloning a new specialization of a generic function.
  void visitNonGenericFunctionDeclaration(
      ESTree::FunctionDeclarationNode *node,
      sema::Decl *decl);

  void visit(ESTree::FunctionDeclarationNode *node);
  void visit(ESTree::FunctionExpressionNode *node);
  void visit(ESTree::ArrowFunctionExpressionNode *node);

  /// Typecheck an arrow function. Shared by the plain and the
  /// contextually-typed (ExprVisitor) entry points.
  /// \param constraint optional expected function type that drives inference of
  ///   unannotated parameters and return type. nullptr for a plain arrow.
  void visitArrowFunction(
      ESTree::ArrowFunctionExpressionNode *node,
      TypedFunctionType *constraint);

  /// Run typechecking on the body of a class that we already have the type for.
  void visitClassNode(
      ESTree::ClassLikeNode *classNode,
      ESTree::ClassBodyNode *body,
      Type *classType);

  void visit(ESTree::ClassExpressionNode *node);
  void visit(ESTree::ClassDeclarationNode *node);
  void visit(ESTree::MethodDefinitionNode *node);

  void visit(ESTree::DecoratorNode *node, ESTree::Node *parent);

  void visit(ESTree::TypeAnnotationNode *node);
  void visit(ESTree::IdentifierNode *identifierNode);

  /// Used in an expression context.
  class ExprVisitor;

  /// Invoke the expression visitor.
  /// \param constraint the constraint the visitor will attempt to match.
  /// See file comment in FlowChecker-expr.cpp for details.
  void
  visitExpression(ESTree::Node *node, ESTree::Node *parent, Type *constraint);

  /// Invoke the expression visitor on \p node in write position, so member
  /// expression leaves are classified as writes rather than reads.
  /// \param constraint the constraint the visitor will attempt to match.
  void visitAssignmentTarget(
      ESTree::Node *node,
      ESTree::Node *parent,
      Type *constraint);

  /// \return the type each iteration of \p node produces, or nullptr if that is
  /// only known at runtime.
  Type *forOfElementType(ESTree::ForOfStatementNode *node);

  /// Visit \p left, the left-hand side of a for-in/of loop, which is either a
  /// declaration or a write target, and check that it can hold \p srcType —
  /// the value the loop feeds it on each iteration, or nullptr if that is only
  /// known at runtime. \p kind names the loop and the source for the
  /// diagnostic.
  /// A pattern LHS in expression position is not checked as a whole: it is
  /// checked leaf by leaf by its own visitor, and its own type is either
  /// \p srcType (redundant) or an invariant aggregate of its targets (which
  /// would compare falsely).
  void visitLoopTarget(
      ESTree::Node *left,
      ESTree::Node *parent,
      Type *srcType,
      const char *kind);

  void visit(ESTree::ExpressionStatementNode *node);
  void visit(ESTree::IfStatementNode *node);
  void visit(ESTree::SwitchStatementNode *node);
  void visit(ESTree::SwitchCaseNode *node);
  void visit(ESTree::WhileStatementNode *node);
  void visit(ESTree::DoWhileStatementNode *node);
  void visit(ESTree::ForOfStatementNode *node);
  void visit(ESTree::ForInStatementNode *node);
  void visit(ESTree::ForStatementNode *node);
  void visit(ESTree::ReturnStatementNode *node);
  void visit(ESTree::ThrowStatementNode *node);
  void visit(ESTree::BlockStatementNode *node);
  void visit(ESTree::VariableDeclarationNode *node);
  void visit(ESTree::ClassPropertyNode *node);
  void visit(ESTree::ClassPrivatePropertyNode *node);

  void visit(ESTree::CatchClauseNode *node);

 private:
  /// A RAII object automatic scopes.
  class ScopeRAII {
   public:
    /// The binding table scope.
    TypeBindingTableScopeTy bindingScope;

    /// Create a binding scope and push a semantic scope.
    /// \param scopeNode is the AST node with which to associate the scope.
    explicit ScopeRAII(FlowChecker &checker)
        : bindingScope(checker.bindingTable_) {}

    /// Pops the created scope if it was pushed.
    ~ScopeRAII() = default;
  };

  void visitFunctionLike(
      ESTree::FunctionLikeNode *node,
      ESTree::Node *body,
      ESTree::NodeList &params);

  /// Check that the implicit return at the end of the function is valid if it
  /// may be reached. Report an error if it isn't valid.
  void checkImplicitReturnType(ESTree::FunctionLikeNode *node);

  /// Forward declaration information for generic type instantiations in
  /// aliases.
  /// Needed by both DeclareScopeTypes and FindLoopingTypes.
  class GenericTypeInstantiation;

  class DeclareScopeTypes;

  /// Resolve and declare all types named in a scope.
  /// Necessary because it uses DeclareScopeTypes in FlowChecker-scopetypes.cpp.
  void declareScopeTypes(
      const sema::ScopeDecls &decls,
      sema::LexicalScope *scope,
      ESTree::Node *scopeNode);

  /// Run DeclareScopeTypes starting from a single generic type annotation.
  /// \return the resolved type for the generic type alias specialization.
  /// Necessary because it uses DeclareScopeTypes in FlowChecker-scopetypes.cpp.
  Type *resolveGenericTypeAlias(
      ESTree::GenericTypeAnnotationNode *gta,
      TypeDecl *typeDecl);

  /// Check whether a type contains loops (even indirectly).
  /// Determines whether it is considered "unsortable" in union arms.
  /// Recurses through all structural types, stops on nominal types because they
  /// can be compared easily, and don't count as looping union arms.
  class FindLoopingTypes;

  /// Finds looping types reachable from \p type,
  /// provided all forward generics have been instantiated.
  void findLoopingTypes(Type *type);

  /// Parse all sema declarations type annotations and associate them
  /// with the declarations.
  class AnnotateScopeDecls;

  /// Declare all named types in the specified scope and annotate the
  /// declarations with them.
  /// \return true on success. Do not typecheck the scope if this returns false.
  /// LLVM_NODISCARD to ensure that we always enforce the check.
  LLVM_NODISCARD bool resolveScopeTypesAndAnnotate(
      ESTree::Node *scopeNode,
      sema::LexicalScope *scope);

  /// Finish typechecking all the deferred nodes that have been queued up.
  void drainTypecheckQueue();

  /// Record the declaration's type and declaring AST node, while checking for
  /// and reporting re-declarations.
  /// \return true if there was no error.
  bool recordDecl(
      sema::Decl *decl,
      Type *type,
      ESTree::IdentifierNode *id,
      ESTree::Node *astDeclNode);

  /// Record only the declaring AST node of \p decl, reporting a
  /// re-declaration if it already has one.
  /// \return true if there was no error.
  bool recordDeclNode(
      sema::Decl *decl,
      ESTree::IdentifierNode *id,
      ESTree::Node *astDeclNode);

  /// Walk a destructuring pattern for a function parameter, assigning types
  /// to each leaf identifier and recording them in declTypes_.
  void assignDestructuringParamTypes(ESTree::Node *pattern, Type *paramType);

  /// A destructuring default and the type it must flow into.
  using DestructuringDefault =
      std::pair<ESTree::AssignmentPatternNode *, Type *>;

  /// Defaults in declaration patterns, keyed by the declarator's pattern.
  /// They are checked in visit(VariableDeclarationNode) rather than during
  /// AnnotateScopeDecls, when other declarations may not be typed yet.
  llvh::DenseMap<ESTree::Node *, llvh::SmallVector<DestructuringDefault, 2>>
      declPatternDefaults_{};

  /// Walk a destructuring \p pattern, annotating every nested pattern node and
  /// leaf binding identifier with the type derived from \p patternType. For
  /// each leaf binding identifier, \p recordLeaf is invoked with its decl, its
  /// resolved type, and the identifier node, allowing the caller to record the
  /// binding in the way appropriate to its context (function parameter, var
  /// declaration, etc.).
  /// Defaults are appended to \p defaults for the caller to check with
  /// checkDestructuringDefaults once the bindings they may reference are typed.
  /// May report errors.
  template <typename RecordLeafCB>
  void resolveDestructuringTypes(
      ESTree::Node *pattern,
      Type *patternType,
      RecordLeafCB recordLeaf,
      llvh::SmallVectorImpl<DestructuringDefault> &defaults);

  /// Visit each destructuring default and check it flows into its type.
  void checkDestructuringDefaults(
      llvh::ArrayRef<DestructuringDefault> defaults);

  /// Expand a tuple destructuring pattern: validate element count and call
  /// \p onChild for each (element, type) pair. A hole consumes an element
  /// without invoking \p onChild.
  /// \return false if there was a size mismatch error.
  template <typename OnChildCB>
  LLVM_NODISCARD bool expandTupleDestructuring(
      ESTree::ArrayPatternNode *arr,
      TupleType *tuple,
      OnChildCB onChild);

  /// Expand an Array<T> destructuring pattern: every non-rest element gets
  /// type T, and a trailing RestElement (if any) gets the array type itself.
  /// \p arrayClassType must satisfy flowContext_.isArrayClassType().
  /// \return false on error.
  template <typename OnChildCB>
  LLVM_NODISCARD bool expandArrayDestructuring(
      ESTree::ArrayPatternNode *arr,
      Type *arrayClassType,
      OnChildCB onChild);

  /// Expand an object destructuring pattern: resolve field types and call
  /// \p onChild for each (propertyValue, type) pair.
  /// \return false on error.
  template <typename OnChildCB>
  LLVM_NODISCARD bool expandObjectDestructuring(
      ESTree::ObjectPatternNode *obj,
      ExactObjectType *objType,
      OnChildCB onChild);

  /// Typecheck the initializer of a public or private class property \p node.
  /// \param value a reference to the property's initializer, which is replaced
  ///   with an implicit checked cast when one is needed.
  /// \param isStatic whether the property is a static field.
  void
  visitClassProperty(ESTree::Node *node, ESTree::Node *&value, bool isStatic);

  /// Shadow the type parameters of the current class, if any, so that
  /// referencing them inside a static member is an error.
  /// Must be called after opening a ScopeRAII.
  void shadowClassTypeParamsInStatic();

  /// Shadow \p typeParams, declared in \p scope, so that referencing them is
  /// an error. Does nothing if \p typeParams is null.
  /// Must be called after opening a ScopeRAII.
  void shadowClassTypeParams(
      ESTree::TypeParameterDeclarationNode *typeParams,
      sema::LexicalScope *scope);

  /// Return the type associated with a sema::Decl. The association must
  /// exist.
  Type *getDeclType(sema::Decl *decl) const {
    return flowContext_.getDeclType(decl);
  }

  /// Associate a type with an expression node. Do nothing if the specified
  /// type is nullptr.
  void setNodeType(ESTree::Node *node, Type *type) {
    flowContext_.setNodeType(node, type);
  }

  /// Return the optional type associated with a node or "any" if no type.
  Type *getNodeTypeOrAny(ESTree::Node *node) const {
    return flowContext_.getNodeTypeOrAny(node);
  }

  /// \param optReturnTypeAnnotation is nullptr or ESTree::TypeAnnotationNode
  /// \param defaultReturnType optional return type if the return annotation
  ///     is missing. nullptr here is a shortcut for "any".
  /// \param defaultThisType optional this type if the annotation is missing.
  /// \param constraint optional expected function type used to infer the types
  ///     of unannotated parameters and return, and to match inference
  ///     placeholders. Only set for arrow functions, which have no "this"
  ///     parameter.
  Type *parseFunctionType(
      ESTree::NodeList &params,
      ESTree::Node *optReturnTypeAnnotation,
      bool isAsync,
      bool isGenerator,
      Type *defaultReturnType = nullptr,
      Type *defaultThisType = nullptr,
      TypedFunctionType *constraint = nullptr);

  /// Parse an optional type annotation. If it is nullptr, return any, otherwise
  /// parse the inner annotation (which cannot be null).
  /// \param defaultType optional type to use if the annotation is missing.
  ///     nullptr here is a shortcut for "any".
  Type *parseOptionalTypeAnnotation(
      ESTree::Node *optAnnotation,
      Type *defaultType = nullptr);

  /// Parse a type annotation into a type.
  /// \param node the non-null type annotation AST node.
  Type *parseTypeAnnotation(ESTree::Node *node);

  Type *parseUnionTypeAnnotation(ESTree::UnionTypeAnnotationNode *node);
  Type *parseNullableTypeAnnotation(ESTree::NullableTypeAnnotationNode *node);
  /// Resolve an array type with the given element type. Handles inference
  /// placeholders by creating InferencePlaceholderArrayType. Used by both
  /// T[] syntax (parseArrayTypeAnnotation) and Array<T> syntax
  /// (parseGenericTypeAnnotation).
  Type *resolveArrayType(Type *elementType, ESTree::Node *node);
  Type *parseArrayTypeAnnotation(ESTree::ArrayTypeAnnotationNode *node);
  Type *parseTupleTypeAnnotation(ESTree::TupleTypeAnnotationNode *node);
  Type *parseObjectTypeAnnotation(ESTree::ObjectTypeAnnotationNode *node);
  Type *parseGenericTypeAnnotation(ESTree::GenericTypeAnnotationNode *node);
  Type *parseFunctionTypeAnnotation(ESTree::FunctionTypeAnnotationNode *node);

  class ParseClassType;

  /// Parse a class type into an already created (but empty) class.
  /// \param classScope the lexical scope of the class.
  /// \param classConsType the ClassConstructorType wrapping classType, used as
  ///   the 'this' type for static methods. May be nullptr if the constructor
  ///   type is not yet available (e.g., for class expressions).
  /// \param classTypeParameters the TypeParameterDeclaration node for the
  ///   class, used to shadow type params with empty for static members.
  /// \return false if any error was reported while parsing the class, in which
  ///   case some members may be missing from the ClassType.
  bool parseClassType(
      ESTree::Node *superClass,
      ESTree::Node *superTypeParameters,
      ESTree::Node *body,
      Type *classType,
      sema::LexicalScope *classScope,
      Type *classConsType,
      ESTree::Node *classTypeParameters = nullptr);

  /// Visit the \p node for either resolution or parsing and call \p cb on each
  /// of the type annotations in it.
  /// \return the constructed FunctionType.
  template <typename AnnotationCB>
  inline Type *processFunctionTypeAnnotation(
      ESTree::FunctionTypeAnnotationNode *node,
      AnnotationCB cb);

  /// Visit the \p node for either resolution or parsing and call \p cb on each
  /// of the type annotations in it.
  /// \return the constructed TupleType.
  template <typename AnnotationCB>
  inline Type *processTupleTypeAnnotation(
      ESTree::TupleTypeAnnotationNode *node,
      AnnotationCB cb);

  /// Visit the \p node for either resolution or parsing and call \p cb on each
  /// of the type annotations in it.
  /// \return the constructed ObjectType.
  template <typename AnnotationCB>
  inline Type *processObjectTypeAnnotation(
      ESTree::ObjectTypeAnnotationNode *node,
      AnnotationCB cb);

  /// Result indicating whether a type can flow into another type. If it can,
  /// additionally indicates whether a checked cast is needed.
  struct CanFlowResult {
    bool canFlow = false;
    /// When the type can flow, this field indicates whether a checked cast is
    /// needed.
    bool needCheckedCast = false;

    /// \return true if the type can flow without requiring a checked cast.
    bool canFlowWithoutCast() const {
      return canFlow && !needCheckedCast;
    }
  };

  /// How to handle 'this' parameters when checking if function types can flow.
  enum class ThisFlowDirection {
    /// Supertype this parameters flow into subtype this parameters.
    Default,
    /// Subtype this parameters flow into supertype this parameters.
    MethodOverride,
  };

  /// Whether the caller can use a result that requires a checked cast. A cast
  /// can only be emitted at the outermost relation; nested structural
  /// positions have nowhere to put one, so they forbid it.
  /// Only Forbidden relations participate in the cycle check, because the
  /// assumption made when a relation is revisited ("flows, without a cast")
  /// is only sound when the consumer rejects casts anyway.
  enum class CheckedCastMode { Allowed, Forbidden };

  /// State shared by all recursive checks in one flow query.
  class CanFlowState {
   public:
    /// Forbidden-mode (a, b) flow relations currently being evaluated.
    llvh::SmallSetVector<std::pair<TypeInfo *, TypeInfo *>, 4> active{};
  };

  /// Return true if type \p a can "flow" into type \p b.
  /// TODO: generate message explaining why not.
  CanFlowResult canAFlowIntoB(Type *a, Type *b) {
    assert(a->info && b->info && "types haven't been populated yet");
    return canAFlowIntoB(a->info, b->info);
  }
  /// Recursive helper used from nested structural positions, which cannot emit
  /// a checked cast.
  CanFlowResult canAFlowIntoB(Type *a, Type *b, CanFlowState &state) {
    assert(a->info && b->info && "types haven't been populated yet");
    return canAFlowIntoB(
        a->info,
        b->info,
        state,
        ThisFlowDirection::Default,
        CheckedCastMode::Forbidden);
  }
  CanFlowResult canAFlowIntoB(TypeInfo *a, TypeInfo *b) {
    CanFlowState state{};
    // The outermost relation may use a checked cast; nested positions
    // (below) may not.
    return canAFlowIntoB(
        a, b, state, ThisFlowDirection::Default, CheckedCastMode::Allowed);
  }
  /// The primary recursive entry point, which performs the cycle check and
  /// dispatches to the per-kind overloads below.
  /// \param state cycle detection state, shared by every recursive check
  ///   within a single top-level query.
  /// \param thisFlow how to handle 'this' parameters.
  /// \param castMode whether the caller is able to emit a checked cast. Only
  ///   the outermost relation can.
  CanFlowResult canAFlowIntoB(
      TypeInfo *a,
      TypeInfo *b,
      CanFlowState &state,
      ThisFlowDirection thisFlow = ThisFlowDirection::Default,
      CheckedCastMode castMode = CheckedCastMode::Forbidden);
  // The per-kind overloads do not perform their own cycle check; recursive
  // checks must go through the TypeInfo overload above.
  // All of them take CanFlowState for uniformity, even when a particular
  // overload does not need it.
  CanFlowResult canAFlowIntoB(ClassType *a, ClassType *b, CanFlowState &state);
  CanFlowResult canAFlowIntoB(TupleType *a, TupleType *b, CanFlowState &state);
  CanFlowResult
  canAFlowIntoB(ExactObjectType *a, ExactObjectType *b, CanFlowState &state);

  /// \param thisFlow how to handle 'this' parameter.
  CanFlowResult canAFlowIntoB(
      BaseFunctionType *a,
      BaseFunctionType *b,
      CanFlowState &state,
      ThisFlowDirection thisFlow);

  /// Different from regular function type flowing, because 'this' parameters
  /// must be handled specially in the method override scenario.
  /// In only this case, 'this' in \p a must be a subtype of \p b.
  /// In canAFlowIntoB, having a parameter in \p a that is a subtype of \p b
  /// would fail to typecheck.
  /// \return whether \p a can be a method override for \p b.
  bool canAOverrideB(BaseFunctionType *a, BaseFunctionType *b) {
    CanFlowState state;
    // Call TypeInfo overload so the cycle check gets set up.
    return canAFlowIntoB(
               static_cast<TypeInfo *>(a),
               static_cast<TypeInfo *>(b),
               state,
               ThisFlowDirection::MethodOverride,
               CheckedCastMode::Allowed)
        .canFlow;
  }

  /// Try to narrow a union with a single non-optional arm to the non-optional
  /// type so that it can be used in expressions.
  /// Non-optional means non-null and non-void.
  /// Used assuming that Flow has already determined that the type is
  /// supposed to have been narrowed by some condition/assignment at this point.
  /// \return narrowed type, or nullptr if the type is not a union with a single
  /// non-optional arm.
  static Type *getNonOptionalSingleType(Type *exprType);

  /// Try to narrow the type of the node into the target type, provided that the
  /// \p targetType is a non-optional arm of the optional \p exprType.
  /// Used for AssignmentExpression or CallExpression arguments, e.g.
  /// \param exprType the type of the expression.
  /// \param targetType the type we want to narrow to.
  /// \return (resType, cf) where resType is a type that \p exprType flows into,
  /// and cf is the CanFlowResult for exprType into resType. If
  /// exprType has been narrowed to resType, then cf.needCheckedCast is true,
  /// and the caller needs to insert the implicit checked cast.
  /// resType may be targetType if the checked cast should cast to targetType.
  std::pair<Type *, CanFlowResult> tryNarrowType(
      Type *exprType,
      Type *targetType);

  /// Widen "fresh" literal types for the inferred type of an un-annotated
  /// non-const variable: a literal type becomes its base type, and direct
  /// union arms that are literals are widened (one level; union arms are
  /// already flattened, so e.g. a union of string literals becomes String).
  /// This matches TS-style let/var widening; const declarations keep the
  /// literal type.
  /// \return the widened type, or \p type unchanged if nothing was widened.
  Type *widenLiteralType(Type *type);

  /// If \c canFlow.needCheckedCast is set and \c compile_ is set, allocate an
  /// implicit checked cast node from the specified \p argument to
  /// the specified type \p toType and return it. Otherwise return the argument.
  ESTree::Node *implicitCheckedCast(
      ESTree::Node *argument,
      Type *toType,
      CanFlowResult canFlow);

  /// Return the non-null expression sema::Decl associated with the identifier.
  sema::Decl *getDecl(ESTree::IdentifierNode *id) {
    return semContext_.getExpressionDecl(id);
  }

  /// Resolve the bound of \p check and validate that the type argument
  /// satisfies it.
  /// \p check is taken by value, because resolving the bound may add checks to
  /// the queue that holds it.
  void validateGenericBound(GenericBoundCheck check);

  /// Add the \p typeArgTypes to the binding table based on the names provided
  /// in \p params, checking that each type argument satisfies its type
  /// parameter's bound (if any). Bounds are resolved in the scope with all
  /// parameters bound, so they may reference any parameter (e.g. <T: C<T>>).
  /// Checks are queued in deferredGenericBoundChecks_ when it is set, and
  /// their bounds are only resolved when they are performed.
  /// Ensure that there are the correct number of type arguments and that they
  /// are valid to pass.
  /// \param params the type parameter declaration.
  /// \param errorRange used for reporting errors when binding fails.
  /// \param typeArgTypes the actual Types to instantiate the arguments with.
  /// \param scope the lexical scope to associate with each TypeDecl.
  /// \pre the binding table's scope is set to the new scope in which to place
  ///   the bindings (i.e. a direct child of the binding table scope the generic
  ///   was declared with).
  /// \return true on success, false on failure and report an error.
  LLVM_NODISCARD bool validateAndBindTypeParameters(
      ESTree::TypeParameterDeclarationNode *params,
      SMRange errorRange,
      llvh::ArrayRef<Type *> typeArgTypes,
      sema::LexicalScope *scope);

  /// Match a constraint type (containing placeholders) against a concrete type.
  /// Fills in InferencePlaceholder types by mutating their info pointers.
  /// \param startConstraint the constraint that may contain
  /// InferencePlaceholders.
  /// \param startType the type to start attempting to assign from.
  void matchConstraintToType(Type *startConstraint, const Type *startType);

  /// If necessary, specialize and typecheck the specialization of a generic
  /// function.
  /// Parses the type arguments in \p typeArgsNode.
  /// \param node the call expression passing the type arguments
  /// \param callee the name of the generic being called
  /// \param oldDecl the original Decl for the non-specialized generic function
  /// \return the new Decl for the specialization of the function,
  ///   nullptr on error.
  sema::Decl *specializeGeneric(
      sema::Decl *oldDecl,
      ESTree::TypeParameterInstantiationNode *typeArgsNode,
      sema::LexicalScope *scope);

  /// If necessary, specialize and typecheck the specialization of a generic
  /// function or class.
  /// \param errorRange used for reporting errors when binding fails.
  /// \param callee the name of the generic being called
  /// \param oldDecl the original Decl for the non-specialized generic
  /// \return the new Decl for the specialization of the function or class,
  ///   and the specialization itself, nullptrs on error.
  std::pair<sema::Decl *, ESTree::Node *> specializeGenericWithParsedTypes(
      sema::Decl *oldDecl,
      SMRange errorRange,
      llvh::ArrayRef<Type *> typeArgTypes,
      sema::LexicalScope *scope);

  /// If necessary, specialize and typecheck the specialization of a generic
  /// function.
  /// Set the type of the callee to the specialized version of the function.
  /// \param node the call expression passing the type arguments
  /// \param callee the name of the generic being called
  /// \param oldDecl the original Decl for the non-specialized generic function
  void resolveCallToGenericFunctionSpecialization(
      ESTree::CallExpressionNode *node,
      ESTree::IdentifierNode *callee,
      sema::Decl *oldDecl);

  /// If necessary, specialize and typecheck the specialization of a generic
  /// function.
  /// Set the type of the callee to the specialized version of the function.
  /// \param node the call expression passing the type arguments
  /// \param callee the name of the generic being called
  /// \param typeArgTypes the actual Types to instantiate the arguments with.
  /// \param oldDecl the original Decl for the non-specialized generic function
  void resolveCallToGenericFunctionSpecializationWithParsedTypes(
      ESTree::CallExpressionNode *node,
      ESTree::IdentifierNode *callee,
      llvh::ArrayRef<Type *> typeArgTypes,
      sema::Decl *oldDecl);

  /// Build constraints from function params and visit call arguments to infer
  /// type argument placeholders.
  /// \param typeParams Type parameter declaration node.
  /// \param funcParams Function parameters (for deriving constraint types).
  /// \param callArgs Call arguments to visit.
  /// \param typeArgs Type args with placeholders to be resolved in-place.
  /// \param bindScope Scope for type parameter bindings.
  /// \param callNode Parent node for visiting expressions.
  /// \param receiverType Optional receiver type for inferring 'this' param.
  /// \param paramParsingScope Optional scope to activate while parsing the
  ///   method's parameter types (e.g. to make class type params visible for
  ///   TypedLib methods). Restored before visiting call arguments so
  ///   user-defined types remain visible in callbacks.
  /// \return true if all placeholders were resolved.
  bool inferPlaceholdersFromCallArgs(
      ESTree::TypeParameterDeclarationNode *typeParams,
      ESTree::NodeList &funcParams,
      ESTree::NodeList &callArgs,
      llvh::ArrayRef<Type *> typeArgs,
      sema::LexicalScope *bindScope,
      ESTree::Node *callNode,
      Type *receiverType = nullptr,
      TypeBindingTableScopePtrTy paramParsingScope =
          TypeBindingTableScopePtrTy{});

  /// Specialize Array<T> with the given element type.
  /// \return the specialized ClassType (instance type), or nullptr on error.
  Type *getSpecializedArrayClassType(Type *elementType, SMRange errorRange);

  /// Look up a named property on a ClassType (fields + home object methods).
  /// \p propNode is the property node from the MemberExpression; if it is
  /// a PrivateNameNode, looks up private fields/methods, otherwise public.
  /// Propagates the Decl from final method definitions to the property's
  /// identifier for IRGen.
  /// \return a pair of (type, field) for the found property, or (nullptr,
  /// nullptr) if not found. If the method is overloaded, return nullptr for the
  /// type because we can't determine it uniquely.
  std::pair<Type *, const flow::ClassType::Field *> lookupPropertyOnClass(
      flow::ClassType *classType,
      Identifier propName,
      ESTree::Node *propNode);

  /// Resolve a call to a builtin method.
  /// Sets the MemberExpression callee type and registers for IRGen.
  /// \return true if arguments were visited, false otherwise.
  bool resolveBuiltinMethodCall(
      ESTree::CallExpressionNode *call,
      ESTree::MemberExpressionNode *callee,
      ESTree::FunctionDeclarationNode *builtinFunc);

  /// Try to visit the arguments to the call expression and typecheck them.
  /// Infer a possible set of type arguments to use for the call
  /// using an eager type assignment mechanism based on the types of the
  /// arguments. Any type arguments explicitly provided on \p node (a partial
  /// leading list) are used as-is; only the trailing ones are inferred.
  /// NOTE: the inferred type arguments may not be compatible with
  /// every argument, so they must be typechecked by the caller after creating a
  /// generic specialization.
  /// \return (didVisitArgs, typeArgs) where didVisitArgs is true when the
  /// arguments were visited (so the caller doesn't have to visit and typecheck
  /// the arguments). If inference was possible, typeArgs is non-empty and
  /// contains the inferred type arguments, otherwise typeArgs is empty.
  std::pair<bool, llvh::SmallVector<Type *, 2>>
  inferTypeArgumentsForGenericFunctionCall(
      ESTree::CallExpressionNode *node,
      ESTree::IdentifierNode *callee,
      sema::Decl *oldDecl);

  /// Infer the trailing type arguments of `new C<A>(...)` from the arguments
  /// to the explicit constructor of the generic class \p oldDecl. The leading
  /// type arguments on \p node are used as-is.
  /// \return (didVisitArgs, typeArgs) as for
  ///   inferTypeArgumentsForGenericFunctionCall.
  std::pair<bool, llvh::SmallVector<Type *, 2>>
  inferTypeArgumentsForGenericClassNew(
      ESTree::NewExpressionNode *node,
      sema::Decl *oldDecl);

  /// \return the explicit leading type arguments in \p typeArgsNode (may be
  ///   null) followed by inference placeholders, \p numTypeParams in total.
  llvh::SmallVector<Type *, 2> seedTypeArgsForInference(
      ESTree::Node *typeArgsNode,
      size_t numTypeParams);

  /// Report the first type parameter in \p typeParams whose argument in
  /// \p typeArgs is still an inference placeholder, at \p typeArgsNode.
  void reportUninferredTypeArg(
      ESTree::TypeParameterDeclarationNode *typeParams,
      llvh::ArrayRef<Type *> typeArgs,
      ESTree::Node *typeArgsNode);

  /// Run the typechecker on a newly created specialization of a generic
  /// function.
  /// \param typeArgTypes the type arguments passed to the generic function.
  /// \param oldDecl the original Decl for the non-specialized generic function.
  /// \param newDecl the new Decl for the specialization of the function.
  void typecheckGenericFunctionSpecialization(
      ESTree::FunctionDeclarationNode *specialization,
      llvh::ArrayRef<Type *> typeArgTypes,
      sema::Decl *oldDecl,
      sema::Decl *newDecl);

  /// Create the ClassType for the specialization.
  /// Record the declaration in \p newDecl.
  /// If deferredGenericSpecializations_ is set,
  /// add the specialization to the list of deferred specializations.
  /// Otherwise, parse immediately and enqueue a typecheck.
  /// \param typeArgTypes the type arguments passed to the generic function.
  /// \param oldDecl the original Decl for the non-specialized generic function.
  /// \param newDecl the new Decl for the specialization of the function.
  void typecheckGenericClassSpecialization(
      ESTree::ClassDeclarationNode *specialization,
      llvh::ArrayRef<Type *> typeArgTypes,
      sema::Decl *oldDecl,
      sema::Decl *newDecl);

  /// Given a generic type for a class, resolve it into a class constructor.
  /// Annotate the \p nameNode with the resolved ClassConstructorType.
  /// \return the ClassType corresponding to the constructor.
  Type *resolveGenericClassSpecialization(
      ESTree::IdentifierNode *nameNode,
      ESTree::TypeParameterInstantiationNode *typeArgsNode,
      sema::Decl *oldDecl);

  /// Same as resolveGenericClassSpecialization, with already parsed
  /// \p typeArgTypes.
  Type *resolveGenericClassSpecializationWithParsedTypes(
      ESTree::IdentifierNode *nameNode,
      SMRange errorRange,
      llvh::ArrayRef<Type *> typeArgTypes,
      sema::Decl *oldDecl);

  /// Given a generic type for a class, resolve it into a class constructor.
  /// \return the ClassType corresponding to the constructor.
  Type *resolveGenericClassSpecializationForType(
      ESTree::GenericTypeAnnotationNode *genericTypeNode,
      sema::Decl *oldDecl);

  /// Register the \p decl generic with its original AST \p node.
  /// Increment refcount of \p bindingTableScope so it can be kept alive and
  /// activated on specialization.
  /// \param decl the generic declaration.
  /// \param node the original AST node.
  /// \param parent the parent of \p node.
  void registerGeneric(
      sema::Decl *decl,
      ESTree::Node *node,
      ESTree::Node *parent,
      const TypeBindingTableScopePtrTy &bindingTableScope);

  /// Register the \p decl generic with its original AST \p node.
  /// Increment refcount of \p bindingTableScope so it can be kept alive and
  /// activated on specialization.
  /// \param decl the generic declaration.
  /// \param node the original AST node.
  /// \param parent the parent of \p node.
  void registerGenericAlias(
      ESTree::TypeAliasNode *node,
      ESTree::Node *parent,
      const TypeBindingTableScopePtrTy &bindingTableScope);

  /// \pre \p decl is a generic declaration that has been registered.
  /// \return the generic info associated with \p decl.
  GenericInfo<ESTree::Node> &getGenericInfoMustExist(sema::Decl *decl);

  /// \pre \p node is a generic type alias that has been registered.
  /// \return the generic info associated with \p decl.
  GenericInfo<Type> &getGenericAliasInfoMustExist(
      const ESTree::TypeAliasNode *decl);

  /// Register a generic method with its original AST node.
  /// \param method the MethodDefinitionNode for the generic method.
  /// \param classBody the ClassBodyNode containing the method.
  /// \param classScope the lexical scope of the containing class.
  /// \param classTypeInfo the ClassType containing this method.
  /// \param classTypeParams the type parameters of the containing class, if
  ///   any.
  void registerGenericMethod(
      ESTree::MethodDefinitionNode *method,
      ESTree::ClassBodyNode *classBody,
      const TypeBindingTableScopePtrTy &bindingTableScope,
      sema::LexicalScope *classScope,
      flow::ClassType *classTypeInfo,
      ESTree::TypeParameterDeclarationNode *classTypeParams);

  /// \pre \p method is a generic method that has been registered.
  /// \return the generic info associated with \p method.
  GenericInfo<ESTree::Node> &getGenericMethodInfoMustExist(
      const ESTree::MethodDefinitionNode *method);

  /// Specialize a generic method call with explicit type arguments.
  /// Set the type of the callee to the specialized version of the method.
  /// \param node the call expression passing the type arguments.
  /// \param callee the member expression for the method call.
  /// \param method the MethodDefinitionNode for the generic method.
  void resolveCallToGenericMethodSpecialization(
      ESTree::CallExpressionNode *node,
      ESTree::MemberExpressionNode *callee,
      ESTree::MethodDefinitionNode *method);

  /// Specialize a generic method call with pre-parsed type arguments.
  /// Set the type of the callee to the specialized version of the method.
  /// \param node the call expression.
  /// \param callee the member expression for the method call.
  /// \param typeArgTypes the parsed type arguments.
  /// \param method the MethodDefinitionNode for the generic method.
  void resolveCallToGenericMethodSpecializationWithParsedTypes(
      ESTree::CallExpressionNode *node,
      ESTree::MemberExpressionNode *callee,
      llvh::ArrayRef<Type *> typeArgTypes,
      ESTree::MethodDefinitionNode *method);

  /// Try to infer type arguments for a generic method call.
  /// Any type arguments explicitly provided on \p node (a partial leading list)
  /// are used as-is; only the trailing ones are inferred.
  /// \param node the call expression.
  /// \param callee the member expression for the method call.
  /// \param method the MethodDefinitionNode for the generic method.
  /// \return (didVisitArgs, typeArgs) where didVisitArgs is true when the
  /// arguments were visited. If inference was possible, typeArgs is non-empty.
  std::pair<bool, llvh::SmallVector<Type *, 2>>
  inferTypeArgumentsForGenericMethodCall(
      ESTree::CallExpressionNode *node,
      ESTree::MemberExpressionNode *callee,
      ESTree::MethodDefinitionNode *method);

  /// Result of specializing a generic method.
  struct GenericMethodSpecializationResult {
    /// The specialized function type, or nullptr on error.
    Type *type = nullptr;
    /// The Decl for the specialized method, or nullptr on error.
    sema::Decl *decl = nullptr;
  };

  /// Specialize and typecheck a generic method with the given type arguments.
  /// \param method the MethodDefinitionNode for the generic method.
  /// \param errorRange used for reporting errors.
  /// \param typeArgTypes the type arguments to specialize with.
  /// \return the specialized result containing type and method node.
  /// The declaring class is taken from the method's GenericInfo, not from the
  /// call site: specializations are cached by type arguments alone, so they
  /// must not depend on the receiver.
  GenericMethodSpecializationResult specializeGenericMethodWithParsedTypes(
      ESTree::MethodDefinitionNode *method,
      SMRange errorRange,
      llvh::ArrayRef<Type *> typeArgTypes);

  /// Implementation for specializing generics: handles cloning, caching,
  /// and insertion. Caller handles scope management and typechecking.
  /// \param generic the GenericInfo containing original node, parent, and
  /// cache.
  /// \param cloneScope scope for cloning (passed to cloneNode).
  /// \param errorRange for error reporting.
  /// \param typeArgTypes the type arguments to specialize with.
  /// \return {specialization node, wasNewlyCreated}, or {nullptr, false} on
  /// error.
  std::pair<ESTree::Node *, bool> specializeGenericImpl(
      GenericInfo<ESTree::Node> &generic,
      sema::LexicalScope *cloneScope,
      SMRange errorRange,
      llvh::ArrayRef<Type *> typeArgTypes);

  /// \param key the property key to attempt to convert to an identifier,
  ///   e.g. StringLiteralNode, IdentifierNode, or NumericLiteralNode.
  /// \return the identifier that represents the property key, or nullptr if
  ///   the key cannot be converted.
  UniqueString *propertyKeyAsIdentifier(ESTree::Node *Key);

  /// Convert a Flow VarianceNode AST node to a FieldVariance.
  /// "plus"/"readonly" -> ReadOnly, "minus"/"writeonly" -> WriteOnly.
  /// "in"/"out" are type-parameter variances and are not valid on fields:
  /// emit an error and return None. nullptr returns None (invariant).
  FieldVariance parseVariance(ESTree::VarianceNode *varianceNode);

  /// Try to find a an enclosing class.
  /// Private names can only be used in lexical descendants of the class
  /// in which they are declared, so this function lets us check that the
  /// private name is currently visible.
  ///
  /// \param classType the LHS of the member expression we're doing the lookup
  ///   on.
  /// \return true if the class is found, false otherwise.
  bool classTypeIsEnclosing(ClassType *classType);
};

class FlowChecker::FunctionContext {
  FlowChecker &outer_;
  /// The previous context to be restored on destruction. nullptr if this is the
  /// global function.
  FunctionContext *const prevContext_;

 public:
  /// The DeclCollector associated with this function.
  /// nullptr if this function has no declCollector associated.
  const sema::DeclCollector *const declCollector;

  /// The external signature of the current function. If nullptr, this is the
  /// global function.
  Type *const functionType;

  /// Semantic information for this function. Null for synthetic contexts that
  /// do not correspond to a function-like AST node.
  sema::FunctionInfo *const semInfo;

  /// The type of the "this" parameter. If nullptr, this is a global function
  /// with an implicit "this" paramater. Depending on the compilation mode,
  /// usages may be invalid.
  ///
  /// Note that the type of "this" is not necessarily the same as
  /// FunctionType::thisParamType, because of arrow functions.
  Type *const thisParamType;

  /// The type of "new.target". If nullptr, this is a global function.
  /// Depending on the compilation mode, usages may be invalid.
  Type *const newTargetType;

  /// The type "super" refers to in a member access: the superclass instance,
  /// or the superclass constructor in a static context. Null when there is no
  /// superclass, and also whenever the enclosing context doesn't thread it
  /// through, so a member access on "super" must reject a null instead of
  /// assuming one. Stored rather than derived, like \c thisParamType, because
  /// arrow functions inherit it from the enclosing context.
  Type *const superType;

  /// Whether this is the body of a setter, which may not return a value.
  bool isSetter = false;

  FunctionContext(const FunctionContext &) = delete;
  void operator=(const FunctionContext &) = delete;

  /// \param declCollectorNode the AST node with an associated DeclCollector.
  FunctionContext(
      FlowChecker &outer,
      ESTree::FunctionLikeNode *declCollectorNode,
      Type *functionType,
      Type *thisParamType,
      Type *newTargetType,
      Type *superType = nullptr)
      : outer_(outer),
        prevContext_(outer.curFunctionContext_),
        declCollector(
            declCollectorNode
                ? outer.declCollectorMap_.find(declCollectorNode)->second.get()
                : nullptr),
        functionType(functionType),
        semInfo(declCollectorNode ? declCollectorNode->getSemInfo() : nullptr),
        thisParamType(thisParamType),
        newTargetType(newTargetType),
        superType(superType) {
    assert(
        (!declCollectorNode ||
         outer.declCollectorMap_.count(declCollectorNode)) &&
        "no declCollector for this node");
    outer.curFunctionContext_ = this;
  }

  ~FunctionContext() {
    outer_.curFunctionContext_ = prevContext_;
  }

  /// \return the previous FunctionContext.
  FunctionContext *getPreviousContext() const {
    return prevContext_;
  }
};

// TODO: this is probably not needed. We can directly visit the class members,
//       passing in the required info.
class FlowChecker::ClassContext {
  FlowChecker &outer_;

  /// The previous context, restored on destruction.
  ClassContext *const prevContext_;

  /// The type of the field initializer function, indexed by whether it is the
  /// static one. The two differ only in their 'this' param.
  Type *fieldInitFunctionType_[2] = {nullptr, nullptr};

 public:
  Type *const classType;

  ESTree::ClassLikeNode *const node;

  ClassContext(const ClassContext &) = delete;
  void operator=(const ClassContext &) = delete;

  ClassContext(
      FlowChecker &outer,
      Type *const classType,
      ESTree::ClassLikeNode *node)
      : outer_(outer),
        prevContext_(outer.curClassContext_),
        classType(classType),
        node(node) {
    outer.curClassContext_ = this;
  }

  /// \return the type of new.target inside the class, i.e. this class's
  ///   ClassConstructorType.
  Type *getNewTargetType() const {
    Type *consType = getClassTypeInfo()->getClassConsType();
    assert(consType && "class must have a ClassConstructorType");
    return consType;
  }

  /// \return the type of 'this' inside a member of this class: the class
  ///   constructor in a static context (ES2023 §15.7.14), the instance
  ///   otherwise.
  Type *getThisType(bool isStatic) const {
    return isStatic ? getNewTargetType() : classType;
  }

  /// \return the type of 'super' inside a member of this class, or nullptr if
  ///   there is no superclass. A static member's receiver is the class
  ///   constructor, so 'super' there is the superclass's constructor and
  ///   resolves against its statics rather than its prototype.
  Type *getSuperType(bool isStatic) const {
    Type *superClass = getClassTypeInfo()->getSuperClass();
    if (!superClass)
      return nullptr;
    if (!isStatic)
      return superClass;
    auto *superInfo = llvh::cast<ClassType>(superClass->info);
    assert(
        superInfo->getClassConsType() &&
        "a class that can be extended must have a constructor type");
    return superInfo->getClassConsType();
  }

  ~ClassContext() {
    outer_.curClassContext_ = prevContext_;
  }

  ClassType *getClassTypeInfo() const {
    return llvh::cast<ClassType>(classType->info);
  }

  /// \return the type of the initializer function for the static or instance
  ///   elements, creating it if it doesn't exist. Its 'this' param must agree
  ///   with the FunctionContext's thisParamType, so the two variants are
  ///   cached separately.
  Type *getOrCreateFieldInitFunctionType(bool isStatic) {
    Type *&cached = fieldInitFunctionType_[isStatic];
    if (!cached) {
      cached = outer_.flowContext_.createType(
          outer_.flowContext_.createFunction(
              outer_.flowContext_.getVoid(),
              getThisType(isStatic),
              {},
              false,
              false),
          node);
    }
    return cached;
  }

  ClassContext *getPrevContext() const {
    return prevContext_;
  }
};

template <typename OnChildCB>
inline bool FlowChecker::expandTupleDestructuring(
    ESTree::ArrayPatternNode *arr,
    TupleType *tuple,
    OnChildCB onChild) {
  size_t i = 0;
  bool tooFewTupleElements = false;
  for (ESTree::Node &element : arr->_elements) {
    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&element)) {
      sm_.error(
          rest->getSourceRange(),
          "ft: rest element not allowed when destructuring a tuple");
      return false;
    }
    if (llvh::isa<ESTree::AssignmentPatternNode>(&element)) {
      sm_.error(
          element.getSourceRange(),
          "ft: default values are not yet supported "
          "in typed tuple destructuring");
      return false;
    }
    if (i >= tuple->getTypes().size()) {
      tooFewTupleElements = true;
      break;
    }
    // A hole ([, x]) still consumes a tuple element, but binds nothing.
    if (!llvh::isa<ESTree::EmptyNode>(&element))
      onChild(&element, tuple->getTypes()[i]);
    ++i;
  }
  if (tooFewTupleElements || i != tuple->getTypes().size()) {
    sm_.error(
        arr->getSourceRange(),
        llvh::Twine("ft: cannot destructure tuple, expected ") +
            llvh::Twine(tuple->getTypes().size()) + " elements, found " +
            llvh::Twine(arr->_elements.size()));
    return false;
  }
  return true;
}

template <typename OnChildCB>
inline bool FlowChecker::expandArrayDestructuring(
    ESTree::ArrayPatternNode *arr,
    Type *arrayClassType,
    OnChildCB onChild) {
  assert(
      flowContext_.isArrayClassType(arrayClassType) &&
      "expandArrayDestructuring requires Array<T> class type");
  Type *elementType = flowContext_.getArrayElementType(arrayClassType);
  for (auto it = arr->_elements.begin(), end = arr->_elements.end(); it != end;
       ++it) {
    ESTree::Node &element = *it;
    if (llvh::isa<ESTree::EmptyNode>(&element))
      continue;
    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&element)) {
      // Parser guarantees rest is last; assert defensively.
      assert(std::next(it) == end && "rest element must be the last element");
      onChild(rest->_argument, arrayClassType);
      break;
    }
    if (llvh::isa<ESTree::AssignmentPatternNode>(&element)) {
      sm_.error(
          element.getSourceRange(),
          "ft: default values are not yet supported "
          "in typed array destructuring");
      return false;
    }
    onChild(&element, elementType);
  }
  return true;
}

template <typename OnChildCB>
inline bool FlowChecker::expandObjectDestructuring(
    ESTree::ObjectPatternNode *obj,
    ExactObjectType *objType,
    OnChildCB onChild) {
  // An object with an indexer has no named fields (they are mutually
  // exclusive), so destructuring resolves every property through the indexer.
  if (const auto &indexer = objType->getIndexer()) {
    for (ESTree::Node &propNode : obj->_properties) {
      if (auto *prop = llvh::dyn_cast<ESTree::PropertyNode>(&propNode)) {
        if (prop->_computed) {
          sm_.error(
              prop->_key->getSourceRange(),
              "ft: computed properties not supported in destructuring");
          return false;
        }
        if (!llvh::isa<ESTree::IdentifierNode>(prop->_key)) {
          sm_.error(
              prop->_key->getSourceRange(),
              "ft: property key must be an identifier");
          return false;
        }
        // A named property uses a string key; it must flow into the indexer's
        // key type (e.g. a `{[number]: T}` indexer rejects named properties).
        if (!canAFlowIntoB(flowContext_.getString(), indexer->keyType)
                 .canFlow) {
          sm_.error(
              prop->_key->getSourceRange(),
              "ft: object index type " +
                  flowContext_.getString()->messageString() +
                  " incompatible with index signature " +
                  indexer->keyType->messageString());
        }
        // Destructuring reads through the indexer, so writeonly is not allowed.
        if (indexer->variance == FieldVariance::WriteOnly) {
          sm_.error(
              prop->_key->getSourceRange(),
              "ft: cannot read writeonly indexer");
        }
        onChild(prop->_value, indexer->valueType);
      } else if (
          auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&propNode)) {
        // The parser guarantees a rest element is the last property. The rest
        // binding reads through the indexer and collects the remaining keys
        // into a fresh exact object with the same indexer.
        if (indexer->variance == FieldVariance::WriteOnly) {
          sm_.error(
              rest->getSourceRange(), "ft: cannot read writeonly indexer");
        }
        Type *restType = flowContext_.createType(
            flowContext_.createExactObject({}, *indexer));
        onChild(rest->_argument, restType);
        break;
      }
    }
    return true;
  }

  // Track which fields of \p objType have been consumed by named
  // properties, so a trailing rest element can be typed as an exact object
  // of the remaining fields.
  llvh::BitVector consumed(objType->getFields().size());
  for (ESTree::Node &propNode : obj->_properties) {
    if (auto *prop = llvh::dyn_cast<ESTree::PropertyNode>(&propNode)) {
      if (prop->_computed) {
        sm_.error(
            prop->_key->getSourceRange(),
            "ft: computed properties not supported in destructuring");
        return false;
      }
      auto *keyId = llvh::dyn_cast<ESTree::IdentifierNode>(prop->_key);
      if (!keyId) {
        sm_.error(
            prop->_key->getSourceRange(),
            "ft: property key must be an identifier");
        return false;
      }
      auto propName = Identifier::getFromPointer(keyId->_name);
      auto optFieldIdx = objType->findField(propName);
      if (!optFieldIdx) {
        sm_.error(
            prop->_key->getSourceRange(),
            "ft: property '" + propName.str() + "' not found in object type");
        return false;
      }
      consumed.set(*optFieldIdx);
      const auto &field = objType->getFields()[*optFieldIdx];
      // Destructuring reads the source field, so writeonly fields are not
      // allowed.
      if (field.variance == FieldVariance::WriteOnly) {
        sm_.error(
            prop->_key->getSourceRange(),
            "ft: cannot read writeonly property " + propName.str());
      }
      onChild(prop->_value, field.type);
    } else if (
        auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&propNode)) {
      // The parser guarantees a rest element is the last property.
      llvh::SmallVector<ExactObjectType::Field, 4> restFields;
      for (size_t i = 0, e = objType->getFields().size(); i < e; ++i) {
        if (consumed.test(i))
          continue;
        const auto &srcField = objType->getFields()[i];
        // The rest binding reads every remaining source field.
        if (srcField.variance == FieldVariance::WriteOnly) {
          sm_.error(
              rest->getSourceRange(),
              "ft: cannot read writeonly property " +
                  srcField.name.getUnderlyingPointer()->str());
        }
        // The rest binding's destination is a fresh exact object, so its
        // fields are always invariant.
        restFields.emplace_back(srcField.name, srcField.type);
      }
      Type *restType =
          flowContext_.createType(flowContext_.createExactObject(restFields));
      onChild(rest->_argument, restType);
      break;
    }
  }
  return true;
}

template <typename AnnotationCB>
Type *FlowChecker::processFunctionTypeAnnotation(
    ESTree::FunctionTypeAnnotationNode *node,
    AnnotationCB cb) {
  if (node->_typeParameters) {
    sm_.error(node->getSourceRange(), "unsupported function type params");
  }

  Type *thisType = node->_this
      ? cb(llvh::cast<ESTree::FunctionTypeParamNode>(node->_this)
               ->_typeAnnotation)
      : nullptr;
  Type *returnType = cb(node->_returnType);

  bool seenOptional = false;

  llvh::SmallVector<TypedFunctionType::Param, 4> paramsList{};
  for (ESTree::Node &n : node->_params) {
    if (auto *param = llvh::dyn_cast<ESTree::FunctionTypeParamNode>(&n)) {
      auto *id = llvh::cast_or_null<ESTree::IdentifierNode>(param->_name);
      if (param->_optional) {
        seenOptional = true;
      } else if (seenOptional) {
        sm_.error(param->getSourceRange(), "ft: optional params must be last");
      }
      paramsList.push_back(
          {Identifier::getFromPointer(id ? id->_name : nullptr),
           param->_typeAnnotation ? cb(param->_typeAnnotation) : nullptr,
           param->_optional});
    } else {
      sm_.warning(
          n.getSourceRange(),
          "ft: typing of pattern parameters not implemented, :any assumed");
      size_t idx = paramsList.size();
      paramsList.push_back(
          {astContext_.getIdentifier(
               (llvh::Twine("?param_") + llvh::Twine(idx)).str()),
           flowContext_.getAny(),
           false});
    }
  }

  // Handle the rest parameter if present.
  if (node->_rest) {
    auto *restParam = llvh::cast<ESTree::FunctionTypeParamNode>(node->_rest);

    UniqueString *name = nullptr;
    if (!restParam->_name) {
      name = kw_.identEmptyString;
    } else if (
        auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(restParam->_name)) {
      name = id->_name;
    }
    if (!name)
      sm_.error(restParam->getSourceRange(), "unsupported rest param");

    paramsList.push_back(
        {Identifier::getFromPointer(name),
         restParam->_typeAnnotation ? cb(restParam->_typeAnnotation) : nullptr,
         /*optional=*/false,
         /*rest=*/true});
  }

  return flowContext_.createType(
      flowContext_.createFunction(
          returnType,
          thisType,
          paramsList,
          /* isAsync */ false,
          /* isGenerator */ false),
      node);
}

template <typename AnnotationCB>
Type *FlowChecker::processTupleTypeAnnotation(
    ESTree::TupleTypeAnnotationNode *node,
    AnnotationCB cb) {
  llvh::SmallVector<Type *, 4> types{};
  for (auto &n : node->_elementTypes) {
    if (llvh::isa<ESTree::TupleTypeSpreadElementNode>(&n)) {
      sm_.error(n.getSourceRange(), "ft: tuple spread unsupported");
      continue;
    }
    if (llvh::isa<ESTree::TupleTypeLabeledElementNode>(&n)) {
      sm_.error(n.getSourceRange(), "ft: tuple labels unsupported");
      continue;
    }
    types.push_back(cb(&n));
  }
  return flowContext_.createType(flowContext_.createTuple(types), node);
}

template <typename AnnotationCB>
Type *FlowChecker::processObjectTypeAnnotation(
    ESTree::ObjectTypeAnnotationNode *node,
    AnnotationCB cb) {
  if (!node->_callProperties.empty()) {
    sm_.error(
        node->_callProperties.front().getStartLoc(),
        "ft: call properties are not supported in object types");
    return flowContext_.getAny();
  }
  if (!node->_internalSlots.empty()) {
    sm_.error(
        node->_internalSlots.front().getStartLoc(),
        "ft: internal slots are not supported in object types");
    return flowContext_.getAny();
  }
  if (node->_inexact) {
    sm_.error(
        node->getStartLoc(), "ft: inexact object types are not supported");
    return flowContext_.getAny();
  }

  llvh::SmallVector<ExactObjectType::Field, 4> fields{};
  // Used for checking for duplicate names.
  llvh::SmallDenseSet<UniqueString *> names{};
  for (auto &n : node->_properties) {
    auto *prop = llvh::dyn_cast<ESTree::ObjectTypePropertyNode>(&n);
    if (!prop) {
      sm_.error(n.getSourceRange(), "ft: unsupported object type property");
      continue;
    }
    if (prop->_static) {
      sm_.error(
          n.getSourceRange(),
          "ft: object type static property is not supported");
      continue;
    }
    if (prop->_proto) {
      sm_.error(
          n.getSourceRange(),
          "ft: object type __proto__ property is not supported");
      continue;
    }
    if (prop->_method) {
      sm_.error(
          n.getSourceRange(),
          "ft: object type method property is not supported");
      continue;
    }
    if (prop->_optional) {
      sm_.error(
          n.getSourceRange(),
          "ft: object type optional property is not supported");
      continue;
    }
    if (prop->_kind != kw_.identInit) {
      sm_.error(n.getSourceRange(), "ft: object type accessors not supported");
      continue;
    }

    UniqueString *name = propertyKeyAsIdentifier(prop->_key);
    if (!name) {
      sm_.error(
          prop->_key->getSourceRange(), "ft: unsupported object type key");
      continue;
    }

    // Check for duplicates.
    auto [it, inserted] = names.insert(name);
    if (!inserted) {
      sm_.error(
          prop->_key->getSourceRange(),
          llvh::Twine("ft: duplicate object type property: ") + name->str());
      continue;
    }

    // Found a property we support, add it to the list.
    FieldVariance variance = parseVariance(
        llvh::cast_or_null<ESTree::VarianceNode>(prop->_variance));
    fields.emplace_back(
        Identifier::getFromPointer(name), cb(prop->_value), variance);
  }

  // Parse the optional index signature.
  OptValue<ExactObjectType::Indexer> indexer{};
  if (!node->_indexers.empty()) {
    auto it = node->_indexers.begin();
    auto *indexerNode = llvh::cast<ESTree::ObjectTypeIndexerNode>(&*it);
    // At most one indexer is allowed.
    if (std::next(it) != node->_indexers.end()) {
      sm_.error(
          std::next(it)->getStartLoc(),
          "ft: at most one indexer is allowed in an object type");
    }
    // An indexer can't be combined with named properties.
    if (!fields.empty()) {
      sm_.error(
          indexerNode->getStartLoc(),
          "ft: indexers cannot be combined with named properties");
    } else {
      FieldVariance variance = parseVariance(
          llvh::cast_or_null<ESTree::VarianceNode>(indexerNode->_variance));
      indexer = ExactObjectType::Indexer{
          cb(indexerNode->_key), cb(indexerNode->_value), variance};
    }
  }

  // It's possible we've failed on one of the properties, just continue
  // with an empty object type because we're going to fail anyway.
  return flowContext_.createType(
      flowContext_.createExactObject(fields, indexer), node);
}

} // namespace flow
} // namespace hermes

#endif // HERMES_PARSE_FLOW
#endif // HERMES_SEMA_FLOWCHECKER_H
