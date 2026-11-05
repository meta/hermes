/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// Implementation for DeclareScopeTypes, along with FlowChecker functions that
/// allow calling into DeclareScopeTypes:
/// declareScopeTypes and resolveGenericTypeAlias.
/// Includes DeclareScopeTypes dependencies:
/// FindLoopingTypes and GenericTypeInstantiation.
///
/// Type aliases combined with unions create a **dramatic complication**, since
/// they can be mutually self recursive. We need to declare types in stages,
/// first the "direct" ones, then resolve the aliases, unions, and generics.
///
/// 1. Iterate all scope types. Forward-declare all declared types as Type *.
/// 2. Resolve the aliases, checking for self-references.
/// 3. Complete all forward declared types. They can now refer to the newly
/// declared local types.
/// 4. Run a post-processing validation pass over the resolved
/// types to catch errors that are hard to detect during resolution. For
/// example, verifies that any function-type rest parameter has type
/// Array, since the element type isn't known until all forward-declared
/// types have been completed.
///
/// Generic type aliases are registered along with the other generics,
/// but they don't require an AST node to be cloned during specialization
/// (IRGen isn't going to look at it) so their specializations are just the
/// resolved \c Type *.
//===----------------------------------------------------------------------===//

#include "FlowChecker.h"

#include "hermes/AST/ASTUtils.h"

#include "llvh/ADT/MapVector.h"
#include "llvh/ADT/ScopeExit.h"
#include "llvh/ADT/SmallPtrSet.h"
#include "llvh/Support/SaveAndRestore.h"

#include <deque>

#if HERMES_PARSE_FLOW
#define DEBUG_TYPE "FlowChecker"

namespace hermes {
namespace flow {

/// Forward declaration information for generic type instantiations in
/// aliases.
class FlowChecker::GenericTypeInstantiation {
 public:
  /// The generic TypeDecl.
  TypeDecl *typeDecl;
  /// The type annotation node. This is either a GenericTypeAnnotationNode
  /// (for explicit generic syntax like Array<T>) or an
  /// ArrayTypeAnnotationNode (for T[] shorthand syntax).
  ESTree::Node *annotation;
  /// The type arguments provided to the generic type annotation, may contain
  /// incomplete unions or other forward-declared generics.
  llvh::SmallVector<Type *, 2> typeArgTypes{};

  /// If non-null, the specialization for a class type that was resolved for
  /// this generic instantiation.
  /// Used when fixing up generic class instantiations to make sure we keep
  /// using the same one.
  ESTree::Node *classSpecialization = nullptr;

  /// The genericClassDepth_ where the annotation was resolved, e.g. in the
  /// bound of another specialization.
  unsigned depth;

  GenericTypeInstantiation(
      TypeDecl *typeDecl,
      ESTree::Node *annotation,
      unsigned depth)
      : typeDecl(typeDecl), annotation(annotation), depth(depth) {}
};

class FlowChecker::FindLoopingTypes {
  /// Visited set for the types so far.
  /// Use a SetVector so it can be quickly iterated to copy the types into
  /// loopingTypes set when a looping type is found.
  llvh::SmallSetVector<Type *, 4> visited{};

 public:
  /// Finds looping types reachable from \p type.
  /// Sets the isLooping flag on looping types.
  /// Generics must already be instantiated.
  FindLoopingTypes(Type *type) {
    isTypeLooping(type);
  }

 private:
  bool isTypeLooping(Type *type) {
    bool inserted = visited.insert(type);
    if (!inserted) {
      // Found a cycle. Mark all visited types as looping.
      for (Type *t : visited)
        t->isLooping = true;

      return true;
    }

    auto popOnExit = llvh::make_scope_exit([this]() { visited.pop_back(); });

    if (type->isLooping) {
      // Found a type that's already known to be looping.
      // Mark all visited types as looping.
      // This is necessary because we might have taken another path to end at
      // the known looping type, so we have to insert everything along that
      // second path.
      for (Type *t : visited)
        t->isLooping = true;

      return true;
    }

    switch (type->info->getKind()) {
#define _HERMES_SEMA_FLOW_DEFKIND(name)                         \
  case TypeKind::name:                                          \
    return isLooping(type, llvh::cast<name##Type>(type->info)); \
    break;
      _HERMES_SEMA_FLOW_SINGLETONS _HERMES_SEMA_FLOW_COMPLEX_TYPES
#undef _HERMES_SEMA_FLOW_DEFKIND
    }
    llvm_unreachable("all cases handled");
  }

  bool isLooping(Type *, SingletonType *) {
    // Nothing to do for singleton types.
    return false;
  }

  bool isLooping(Type *type, GenericType *) {
    hermes_fatal("GenericType must be instantiated before checking for loops");
  }

  bool isLooping(Type *, InferencePlaceholderArrayType *) {
    // Inference placeholders are transient and don't participate in loops.
    return false;
  }

  bool isLooping(Type *, StringLiteralType *) {
    // Literals carry no nested types.
    return false;
  }

  bool isLooping(Type *, NumberLiteralType *) {
    // Literals carry no nested types.
    return false;
  }

  bool isLooping(Type *, BooleanLiteralType *) {
    // Literals carry no nested types.
    return false;
  }

  bool isLooping(Type *, TypeWithId *type) {
    // Nominal type. Stop checking for recursion.
    return false;
  }

  bool isLooping(Type *, UnionType *type) {
    bool result = false;
    for (Type *t : type->getTypes()) {
      if (isTypeLooping(t)) {
        // Don't return here, have to run on all the union arms
        // so that if there's multiple looping arms they get registered.
        result = true;
      }
    }
    return result;
  }

  bool isLooping(Type *, ArrayType *type) {
    return isTypeLooping(type->getElement());
  }

  bool isLooping(Type *, TupleType *type) {
    bool result = false;
    for (Type *t : type->getTypes()) {
      if (isTypeLooping(t)) {
        // Don't return here, have to run on all types so that if there's
        // multiple looping union arms they get registered.
        result = true;
      }
    }
    return result;
  }

  bool isLooping(Type *, ExactObjectType *type) {
    bool result = false;
    for (const auto &field : type->getFields()) {
      if (isTypeLooping(field.type)) {
        // Don't return here, have to run on all types so that if there's
        // looping union arms in multiple fields, they get registered.
        result = true;
      }
    }
    return result;
  }

  bool isLooping(Type *, TypedFunctionType *type) {
    bool result = false;
    result |= isTypeLooping(type->getReturnType());
    if (type->getThisParam()) {
      result |= isTypeLooping(type->getThisParam());
    }
    for (const auto &param : type->getParams()) {
      result |= isTypeLooping(param.type);
    }
    return result;
  }

  bool isLooping(Type *, NativeFunctionType *type) {
    bool result = false;
    result |= isTypeLooping(type->getReturnType());
    for (const auto &param : type->getParams()) {
      result |= isTypeLooping(param.type);
    }
    return result;
  }

  bool isLooping(Type *, UntypedFunctionType *type) {
    return false;
  }
};

class FlowChecker::DeclareScopeTypes {
  /// Surrounding class.
  FlowChecker &outer;
  /// The current lexical scope.
  sema::LexicalScope *const scope;
  /// The current lexical scope.
  /// Only used when registering newly discovered generics,
  /// nullptr when DeclareScopeTypes is called to resolve a generic type alias.
  ESTree::Node *const scopeNode;
  /// Type aliases declared in this scope.
  llvh::SmallVector<Type *, 4> localTypeAliases{};
  /// Mapping from the LHS to the RHS type of a Type alias.
  /// e.g. type A = B;
  /// maps from the Type representing 'A' to the type representing 'B'.
  /// Used for populating the LHS after resolving the RHS with
  /// resolveTypeAnnotation.
  /// MapVector makes cycle diagnostics deterministic.
  llvh::MapVector<Type *, Type *> typeAliasResolutions{};
  /// Keep track of the generic instantiations created during type alias
  /// resolution, because they need to know the TypeInfo for the type aliases
  /// to decide whether to make a new specialization.
  llvh::MapVector<Type *, FlowChecker::GenericTypeInstantiation>
      forwardGenericInstantiations{};
  /// Maximum nesting of uncached generic type alias expansions.
  static constexpr unsigned kMaxAliasExpansionDepth = 100;
  /// Current nesting of uncached generic type alias expansions.
  unsigned aliasExpansionDepth = 0;
  /// Set once kMaxAliasExpansionDepth is exceeded. Later uncached expansions
  /// fail immediately, since sibling expansions would otherwise each run to
  /// the limit and take exponential time.
  bool aliasExpansionLimitHit = false;
  /// Expansions of generic type aliases whose type arguments were not yet
  /// resolved, keyed by the alias and the argument Types. These can't use the
  /// specialization cache, which is keyed by TypeInfo, but substituting the
  /// same Types always produces the same result.
  llvh::DenseMap<
      std::pair<ESTree::TypeAliasNode *, llvh::ArrayRef<Type *>>,
      Type *>
      unresolvedAliasExpansions{};
  /// Storage for the argument arrays referenced by unresolvedAliasExpansions.
  /// std::deque keeps them from moving.
  std::deque<llvh::SmallVector<Type *, 2>> unresolvedAliasExpansionArgs{};
  /// Keep track of all forward declarations of classes, so they can be
  /// completed.
  llvh::SmallVector<Type *, 4> forwardClassDecls{};
  /// Keep track of all union types, so they can be canonicalized.
  llvh::SmallSetVector<Type *, 4> forwardUnions{};

  /// Function types with rest params, to validate rest type is Array<T>
  /// after generic resolution.
  llvh::SmallVector<Type *, 2> funcTypesWithRest{};

  /// Generic bound checks to validate after generic class resolution.
  llvh::SmallVector<FlowChecker::GenericBoundCheck, 4>
      deferredGenericBoundChecks{};

  /// The generic specializations to be parsed after the class body is parsed.
  /// These have to be deferred here instead of in DeclareScopeTypes because
  /// we have to not defer parsing the superClass, e.g.
  std::vector<FlowChecker::DeferredGenericClass> deferredParseGenerics{};

 public:
  DeclareScopeTypes(
      FlowChecker &outer,
      const sema::ScopeDecls &decls,
      sema::LexicalScope *scope,
      ESTree::Node *scopeNode)
      : outer(outer), scope(scope), scopeNode(scopeNode) {
    LLVM_DEBUG(llvh::dbgs() << "Declaring scope types\n");
    llvh::SaveAndRestore savedDeferredGenerics{
        outer.deferredParseGenerics_, &deferredParseGenerics};
    // Generic bounds may reference classes owned by another resolver, so
    // prefer an existing queue if it already exists. If it doesn't exist, use
    // the newly created queue in DeclareScopeTypes.
    // NOTE: In practice the outer will be nullptr in this case, but this is
    // defensive and mirrors the resolveGenericTypeAlias case.
    llvh::SaveAndRestore<
        llvh::SmallVectorImpl<FlowChecker::GenericBoundCheck> *>
        savedBoundChecks{
            outer.deferredGenericBoundChecks_,
            outer.deferredGenericBoundChecks_
                ? outer.deferredGenericBoundChecks_
                : &deferredGenericBoundChecks};

    if (!createForwardDeclarations(decls))
      return;
    if (!resolveAllAliases())
      return;
    if (!completeForwardDeclarations())
      return;
    parseDeferredGenericClasses();
    validateTypes();
  }

  /// Run DeclareScopeTypes starting from a single generic type annotation.
  /// \return the resolved type for the generic type alias specialization.
  static Type *resolveGenericTypeAlias(
      FlowChecker &outer,
      ESTree::GenericTypeAnnotationNode *gta,
      TypeDecl *typeDecl) {
    LLVM_DEBUG(llvh::dbgs() << "Resolving generic type alias\n");
    DeclareScopeTypes declareScopeTypes(outer, typeDecl->scope);
    llvh::SaveAndRestore savedDeferredGenerics{
        outer.deferredParseGenerics_, &declareScopeTypes.deferredParseGenerics};
    // Generic bounds may reference classes owned by another resolver, so
    // prefer an existing queue if it already exists. If it doesn't exist, use
    // the newly created queue in DeclareScopeTypes.
    llvh::SaveAndRestore<
        llvh::SmallVectorImpl<FlowChecker::GenericBoundCheck> *>
        savedBoundChecks{
            outer.deferredGenericBoundChecks_,
            outer.deferredGenericBoundChecks_
                ? outer.deferredGenericBoundChecks_
                : &declareScopeTypes.deferredGenericBoundChecks};
    llvh::SmallDenseSet<ESTree::TypeAliasNode *> visited{};
    // Don't call createForwardDeclarations, because we're directly calling
    // resolveTypeAnnotation and we don't have a list of decls.
    Type *type = declareScopeTypes.resolveTypeAnnotation(gta, visited, 0);
    if (!declareScopeTypes.completeForwardDeclarations())
      return outer.flowContext_.getAny();
    declareScopeTypes.parseDeferredGenericClasses();
    declareScopeTypes.validateTypes();
    return type;
  }

 private:
  // Create an empty DeclareScopeTypes, used for resolveGenericTypeAlias.
  DeclareScopeTypes(FlowChecker &outer, sema::LexicalScope *scope)
      : outer(outer), scope(scope), scopeNode(nullptr) {}

  // Check if a type declaration with the specified name exists in the current
  // scope. If it exists, generate an error and return true.
  // \return true if this is a redeclaration (i.e. it is an error).
  bool isRedeclaration(ESTree::IdentifierNode *id) const {
    UniqueString *name = id->_name;
    TypeDecl *typeDecl = outer.bindingTable_.findInCurrentScope(name);
    if (!typeDecl)
      return false;

    outer.sm_.error(
        id->getStartLoc(),
        "ft: type " + name->str() + " already declared in this scope");
    if (typeDecl->astNode && typeDecl->astNode->getSourceRange().isValid()) {
      outer.sm_.note(
          typeDecl->astNode->getSourceRange(),
          "ft: previous declaration of " + name->str());
    }
    return true;
  };

  /// \return the parent of the generic (based on the scopeNode),
  ///   in which to insert new generic specializations.
  ESTree::Node *getGenericParentNode() const {
    assert(scopeNode && "no scope node found");
    if (llvh::isa<ESTree::ProgramNode>(scopeNode)) {
      return scopeNode;
    }
    if (auto *funcNode = llvh::dyn_cast<ESTree::FunctionLikeNode>(scopeNode)) {
      return ESTree::getBlockStatement(funcNode);
    }
    return scopeNode;
  }

  /// Forward declare all classes and record all aliases for later processing.
  /// \return false on error.
  bool createForwardDeclarations(const sema::ScopeDecls &decls) {
    bool result = true;
    for (ESTree::Node *declNode : decls) {
      if (llvh::isa<ESTree::VariableDeclarationNode>(declNode) ||
          llvh::isa<ESTree::ImportDeclarationNode>(declNode) ||
          llvh::isa<ESTree::CatchClauseNode>(declNode) ||
          llvh::isa<ESTree::FunctionDeclarationNode>(declNode)) {
        continue;
      }
      if (auto *classNode =
              llvh::dyn_cast<ESTree::ClassDeclarationNode>(declNode)) {
        // Class declaration.
        //
        auto *id = llvh::cast<ESTree::IdentifierNode>(classNode->_id);
        if (isRedeclaration(id)) {
          result = false;
          continue;
        }

        sema::Decl *decl = outer.getDecl(id);
        decl->generic = classNode->_typeParameters != nullptr;

        if (decl->generic) {
          // Avoid visiting the body of the class until we have an instance.
          auto [entry, _] = outer.bindingTable_.try_emplace(
              id->_name, TypeDecl(nullptr, scope, declNode, decl));

          outer.registerGeneric(
              decl,
              classNode,
              getGenericParentNode(),
              outer.bindingTable_.getCurrentScope());
          if (hermes::findDecorator(
                  classNode->_decorators,
                  {outer.kw_.identHermes, outer.kw_.identArray})) {
            outer.arrayClassDecl_ = decl;
            outer.arrayTypeDecl_ = &entry->second;
          }
          continue;
        }

        Type *newType = outer.flowContext_.createType(
            outer.flowContext_.createClass(
                Identifier::getFromPointer(id->_name)),
            classNode);
        forwardClassDecls.push_back(newType);

        outer.bindingTable_.try_emplace(
            id->_name, TypeDecl(newType, scope, declNode));

        bool success = outer.recordDecl(
            decl,
            outer.flowContext_.createType(
                outer.flowContext_.createClassConstructor(newType), classNode),
            id,
            classNode);
        assert(success && "class constructor unexpectedly re-declared");
        (void)success;
      } else if (
          auto *aliasNode = llvh::dyn_cast<ESTree::TypeAliasNode>(declNode)) {
        // Type alias.
        //
        auto *id = llvh::cast<ESTree::IdentifierNode>(aliasNode->_id);
        if (isRedeclaration(id))
          continue;

        // Generic type alias, register it but just move on.
        if (aliasNode->_typeParameters) {
          outer.bindingTable_.try_emplace(
              id->_name, TypeDecl(nullptr, scope, aliasNode));
          outer.registerGenericAlias(
              aliasNode,
              getGenericParentNode(),
              outer.bindingTable_.getCurrentScope());
          continue;
        }

        Type *newType = outer.flowContext_.createType(declNode);
        localTypeAliases.push_back(newType);
        outer.bindingTable_.try_emplace(
            id->_name, TypeDecl(newType, scope, declNode));
      } else {
        outer.sm_.error(
            declNode->getSourceRange(),
            "ft: unsupported type declaration " + declNode->getNodeName());
      }
    }

    return result;
  }

  /// Resolve all recorded aliases. At the end of this all local types should
  /// resolve to something: a primary type, a type in a surrounding scope, a
  /// local forward declared class, or a union of any of these.
  /// \return false on error.
  bool resolveAllAliases() {
    unsigned errorsBefore = outer.sm_.getErrorCount();
    for (Type *localType : localTypeAliases) {
      // Skip already resolved types.
      if (localType->info)
        continue;

      // If it's not resolved already it must be an alias.
      auto *aliasNode = llvh::cast<ESTree::TypeAliasNode>(localType->node);

      // Recursion can occur through generic annotations and name aliases.
      // Keep track of visited aliases to detect it.
      llvh::SmallDenseSet<ESTree::TypeAliasNode *> visited{};
      visited.insert(aliasNode);

      // Copy the resolved TypeInfo to the alias.
      // The alias Type is different than the Type on the right side.
      Type *resolvedType = resolveTypeAnnotation(aliasNode->_right, visited, 0);
      typeAliasResolutions[localType] = resolvedType;
    }

    // Transfer TypeInfo from the resolved Type to the alias Type.
    // This has to be done in a second pass because resolved Types
    // might still have nullptr TypeInfos until the first pass completes.
    for (Type *localType : localTypeAliases) {
      if (!localType->info) {
        populateTypeAlias(localType);
        assert(localType->info && "populateTypeAlias should populate the info");
      }

      // If it's a union, we'll have to resolve it as well.
      // Unions can turn into single types if they only contain 1 unique type.
      if (llvh::isa<UnionType>(localType->info)) {
        forwardUnions.insert(localType);
      }

      // A generic alias is completed through its target instantiation, see
      // completeForwardType.
    }

    return outer.sm_.getErrorCount() == errorsBefore;
  }

  /// Resolve a type annotation in the current scope. This assumes that all
  /// classes have been declared and all directly resolvable aliases have been
  /// resolved. It deals with the remaining cases:
  /// - a "generic" annotation which refers to another type by name. This can
  ///     lead to self-recursion. If the alias is local, it is resolved.
  /// - a primary type
  /// - a constructor type like array, which is forward declared and resolved
  /// - a union of any of the above.
  ///
  /// \param annotation the type annotation to resolve
  /// \param visited the set of visited nodes when resolving "generic"
  ///     annotations. Used to check self-recursion.
  /// \param depth track depth to avoid stack overflow.
  ///
  /// \return the resolved type, "any" on error.
  Type *resolveTypeAnnotation(
      ESTree::Node *annotation,
      llvh::SmallDenseSet<ESTree::TypeAliasNode *> &visited,
      unsigned depth) {
    /// Avoid stack overflow.
    if (++depth >= 32) {
      outer.sm_.error(
          annotation->getSourceRange(), "ft: too deeply nested aliases/unions");
      return outer.flowContext_.getAny();
    }

    /// Generic annotation. This annotation refers to another type by name.
    /// That type may be a constructor type (class, interface) or it could
    /// recursively alias to another generic annotation or a union.
    if (auto *gta =
            llvh::dyn_cast<ESTree::GenericTypeAnnotationNode>(annotation)) {
      auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(gta->_id);

      if (!id) {
        outer.sm_.error(
            gta->getSourceRange(), "ft: unsupported type annotation");
        return outer.flowContext_.getAny();
      }

      // Is it declared anywhere?
      // If so, find its innermost declaration.
      TypeDecl *typeDecl = outer.bindingTable_.find(id->_name);
      if (!typeDecl) {
        // Not declared anywhere!
        outer.sm_.error(
            id->getStartLoc(), "ft: undefined type " + id->_name->str());
        return outer.flowContext_.getAny();
      }

      if (typeDecl->staticTypeParam) {
        outer.sm_.error(
            id->getSourceRange(),
            "ft: class type parameter " + id->_name->str() +
                " cannot be used in a static member");
        return outer.flowContext_.getAny();
      }

      if (typeDecl->type && gta->_typeParameters) {
        outer.sm_.error(
            gta->_typeParameters->getSourceRange(),
            llvh::Twine("ft: type '") + id->_name->str() + "' is not generic");
        outer.sm_.note(
            typeDecl->astNode->getStartLoc(),
            "type is defined as not generic here");
        return outer.flowContext_.getAny();
      }

      if (!typeDecl->type) {
        // Found a generic TypeDecl.
        // Resolve to the generic specialization.
        if (!gta->_typeParameters) {
          outer.sm_.error(
              gta->getSourceRange(),
              llvh::Twine("ft: missing generic arguments for '") +
                  id->_name->str() + "'");
          return outer.flowContext_.getAny();
        }

        // Make a placeholder type to represent the generic type for now.
        // The TypeInfo will be replaced with the concrete Type when we are
        // able to resolve TypeInfo for all type arguments.
        // We don't yet care whether this is a generic class or type alias.
        Type *type = outer.flowContext_.createType(
            outer.flowContext_.getGenericInfo(), annotation);
        GenericTypeInstantiation instantiation{
            typeDecl, gta, outer.genericClassDepth_};

        // Populate the type arg Types.
        ESTree::NodeList &args =
            llvh::cast<ESTree::TypeParameterInstantiationNode>(
                gta->_typeParameters)
                ->_params;

        for (ESTree::Node &node : args)
          instantiation.typeArgTypes.push_back(
              resolveTypeAnnotation(&node, visited, depth));

        // Add the instantiation to the list of forward declarations,
        // so that we can resolve them later.
        forwardGenericInstantiations.insert({type, std::move(instantiation)});
        return type;
      }

      // No need to recurse here because any references to this name will
      // correctly resolve to the forward-declared Type.
      assert(typeDecl->type && "all types are populated at fwd declaration");
      return typeDecl->type;
    }

    /// Union types require resolving every union "arm".
    if (auto *uta =
            llvh::dyn_cast<ESTree::UnionTypeAnnotationNode>(annotation)) {
      llvh::SmallVector<Type *, 4> types{};
      for (ESTree::Node &node : uta->_types)
        types.push_back(resolveTypeAnnotation(&node, visited, depth));
      // Make a non-canonicalized UnionType to be resolved with the rest of the
      // forwardUnions later.
      Type *result = outer.flowContext_.createType(
          outer.flowContext_.createNonCanonicalizedUnion(std::move(types)),
          annotation);
      forwardUnions.insert(result);
      return result;
    }

    /// A nullable annotation is a simple case of a union.
    if (auto *nta =
            llvh::dyn_cast<ESTree::NullableTypeAnnotationNode>(annotation)) {
      Type *result = outer.flowContext_.createType(
          outer.flowContext_.createNonCanonicalizedUnion({
              outer.flowContext_.getVoid(),
              outer.flowContext_.getNull(),
              resolveTypeAnnotation(nta->_typeAnnotation, visited, depth),
          }),
          annotation);
      forwardUnions.insert(result);
      return result;
    }

    // Array types require resolving the array element.
    // Treat T[] the same as Array<T> — defer specialization via
    // forwardGenericInstantiations so that uncanonicalized union element
    // types are not hashed prematurely.
    if (auto *arr =
            llvh::dyn_cast<ESTree::ArrayTypeAnnotationNode>(annotation)) {
      Type *elementType =
          resolveTypeAnnotation(arr->_elementType, visited, depth);
      if (outer.arrayTypeDecl_) {
        Type *type = outer.flowContext_.createType(
            outer.flowContext_.getGenericInfo(), annotation);
        GenericTypeInstantiation instantiation{
            outer.arrayTypeDecl_, arr, outer.genericClassDepth_};
        instantiation.typeArgTypes.push_back(elementType);
        forwardGenericInstantiations.insert({type, std::move(instantiation)});
        return type;
      }
      // No TypedLib Array class — resolve to any.
      outer.sm_.error(
          annotation->getSourceRange(), "ft: Array type requires TypedLib");
      return outer.flowContext_.createType(
          outer.flowContext_.getAnyInfo(), annotation);
    }

    // Tuple types require resolving the tuple elements.
    if (auto *tuple =
            llvh::dyn_cast<ESTree::TupleTypeAnnotationNode>(annotation)) {
      return outer.processTupleTypeAnnotation(
          tuple, [this, &visited, depth](ESTree::Node *annotation) -> Type * {
            return resolveTypeAnnotation(annotation, visited, depth);
          });
    }

    // Object types require resolving the object fields.
    if (auto *object =
            llvh::dyn_cast<ESTree::ObjectTypeAnnotationNode>(annotation)) {
      return outer.processObjectTypeAnnotation(
          object, [this, &visited, depth](ESTree::Node *annotation) -> Type * {
            return resolveTypeAnnotation(annotation, visited, depth);
          });
    }

    if (auto *func =
            llvh::dyn_cast<ESTree::FunctionTypeAnnotationNode>(annotation)) {
      Type *result = outer.processFunctionTypeAnnotation(
          func, [this, &visited, depth](ESTree::Node *annotation) {
            return resolveTypeAnnotation(annotation, visited, depth);
          });
      if (func->_rest)
        funcTypesWithRest.push_back(result);
      return result;
    }

    // The specified AST node represents a nominal type, so return the type.
    return outer.parseTypeAnnotation(annotation);
  }

  /// Populate the TypeInfo of \p aliasType with its corresponding resolvedType,
  /// based on the information in the typeAliases map.
  /// Detects reference cycles using \p visited, and reports an error and sets
  /// 'any' when a cycle is detected.
  /// Populates TypeInfo for all aliases along an alias chain to avoid
  /// repeating all the lookups.
  /// \post aliasType->info is non-null.
  void populateTypeAlias(Type *aliasType) {
    // An uncanonicalized union may have been collapsed to a single type since
    // it was copied here, so refresh it too.
    if (aliasType->info && !isUnresolved(aliasType))
      return;

    // Recursion can occur through generic annotations and name aliases.
    // Keep track of visited aliases to detect it.
    llvh::SmallSetVector<Type *, 4> visited{};

    // Resolved TypeInfo to assign to all aliases in the chain.
    TypeInfo *resolvedInfo = nullptr;

    Type *curType = aliasType;
    visited.insert(curType);
    while (!resolvedInfo) {
      // Find the resolved type via map lookup.
      auto it = typeAliasResolutions.find(curType);
      assert(
          it != typeAliasResolutions.end() &&
          "all type aliases have a resolved type");
      Type *nextType = it->second;

      bool inserted = visited.insert(nextType);
      if (!inserted) {
        // Found a cycle.
        outer.sm_.error(
            nextType->node->getStartLoc(),
            "ft: type contains a circular reference to itself");
        // Set the info to 'any' to make it non-null and allow the checker to
        // continue.
        resolvedInfo = outer.flowContext_.getAnyInfo();
        break;
      }

      // Continue through generic aliases, whose placeholder TypeInfo is not
      // their resolved type, and aliases that copied an uncanonicalized union.
      if (!nextType->info ||
          (isUnresolved(nextType) && typeAliasResolutions.count(nextType))) {
        curType = nextType;
        continue;
      }

      resolvedInfo = nextType->info;
      curType = nextType;
    }

    // Set the info for all the Types in the chain.
    for (Type *t : visited) {
      t->info = resolvedInfo;
    }
  }

  /// All types declared in the scope have been resolved at the first level.
  /// Resolve the remaining forward declared types and canonicalize forward
  /// unions as much as possible.
  /// \return false on error.
  bool completeForwardDeclarations() {
    unsigned errorsBefore = outer.sm_.getErrorCount();

    // Instantiating generic type aliases may cause new forward declarations
    // to be created. To account for this, keep iterating until no more are
    // introduced.
    // Once that's done, resync the TypeInfo across aliases.
    size_t unionIdx = 0;
    size_t genericIdx = 0;
    while (unionIdx < forwardUnions.size() ||
           genericIdx < forwardGenericInstantiations.size()) {
      // Now simplify and canonicalize as many union arms as possible.
      for (size_t e = forwardUnions.size(); unionIdx < e; ++unionIdx) {
        Type *type = forwardUnions[unionIdx];
        llvh::SetVector<Type *> visited{};
        completeForwardType(type, visited);
      }
      // Now simplify and canonicalize the foward generics.
      for (size_t e = forwardGenericInstantiations.size(); genericIdx < e;
           ++genericIdx) {
        // forwardGenericInstantiations may expand so we can't store the
        // iterator.
        auto &[type, generic] =
            *(forwardGenericInstantiations.begin() + genericIdx);
        llvh::SetVector<Type *> visited{};
        completeForwardType(type, visited);
      }
    }
    // Earlier aliases may still contain a generic placeholder copied from an
    // alias later in the chain. Propagate the completed type now; this also
    // reports alias-only cycles that never reached a concrete type.
    for (auto &resolution : typeAliasResolutions)
      populateTypeAlias(resolution.first);
    syncForwardGenericClassAliasCopies();

    // It's possible that unions and generic class specialization keys can be
    // further simplified now that we've instantiated all generics, so use a
    // post-processing step for that.
    fixupUnionsAndGenericTables();

    // Parse all forward-declared class types.
    for (Type *type : forwardClassDecls) {
      // This is necessary because we need to defer parsing the class to allow
      // using types defined after the class inside the class:
      //     class C {
      //       x: D
      //     };
      //     type D = number;
      auto *classNode = llvh::cast<ESTree::ClassDeclarationNode>(type->node);
      outer.visitExpression(classNode->_superClass, classNode, nullptr);
      // Get the ClassConstructorType for the class declaration to use as
      // the 'this' type for static methods.
      sema::Decl *classDecl =
          outer.getDecl(llvh::cast<ESTree::IdentifierNode>(classNode->_id));
      Type *classConsType = outer.getDeclType(classDecl);
      outer.parseClassType(
          classNode->_superClass,
          classNode->_superTypeArguments,
          classNode->_body,
          type,
          classNode->getScope(),
          classConsType);
    }

    return outer.sm_.getErrorCount() == errorsBefore;
  }

  /// \return whether \p type is in forwardGenericInstantiations and has a
  /// genericClassDecl.
  bool isForwardGenericClass(Type *type) const {
    auto it = forwardGenericInstantiations.find(type);
    return it != forwardGenericInstantiations.end() &&
        it->second.typeDecl->genericClassDecl;
  }

  /// \return the forward generic class source for \p type when its alias chain
  /// resolves to one, otherwise nullptr.
  Type *getForwardGenericClassAliasSource(Type *type) const {
    llvh::SmallDenseSet<Type *, 4> visited;
    Type *source = type;
    while (visited.insert(source).second) {
      auto aliasIt = typeAliasResolutions.find(source);
      if (aliasIt == typeAliasResolutions.end())
        return nullptr;

      source = aliasIt->second;
      if (isForwardGenericClass(source))
        return source;
    }
    return nullptr;
  }

  void syncForwardGenericClassAlias(Type *type, Type *source) {
    assert(isForwardGenericClass(source) && "expected a generic class source");
    type->info = source->info;

    auto aliasIt = forwardGenericInstantiations.find(type);
    auto sourceIt = forwardGenericInstantiations.find(source);
    if (aliasIt != forwardGenericInstantiations.end() &&
        sourceIt != forwardGenericInstantiations.end()) {
      aliasIt->second.classSpecialization =
          sourceIt->second.classSpecialization;
    }
  }

  /// Refresh alias Types that copied a forward generic class, since the source
  /// TypeInfo can change during completion/fixup.
  void syncForwardGenericClassAliasCopies() {
    for (auto &resolution : typeAliasResolutions) {
      if (Type *source = getForwardGenericClassAliasSource(resolution.first)) {
        syncForwardGenericClassAlias(resolution.first, source);
      }
    }
  }

  /// Complete the forward declaration of the given \p type,
  /// replacing its \c info field with the resolved type.
  /// Must be the both the entry point of the DFS and the recursive step,
  /// because it handles push/pop of the visited set.
  void completeForwardType(Type *type, llvh::SetVector<Type *> &visited) {
    LLVM_DEBUG(
        llvh::dbgs() << "Completing forward type: " << type << " "
                     << type->info->getKindName() << " at depth "
                     << visited.size() << '\n');

    if (Type *source = getForwardGenericClassAliasSource(type)) {
      completeForwardType(source, visited);
      syncForwardGenericClassAlias(type, source);
      return;
    }

    // Check for looping types and mark them.
    bool inserted = visited.insert(type);
    if (!inserted || type->isLooping) {
      for (Type *t : visited)
        t->isLooping = true;
    }

    if (!inserted) {
      // Already attempting to complete this type,
      // but hit a cycle on this branch of the DFS.
      LLVM_DEBUG(
          llvh::dbgs() << "Found a cycle while completing forward type: "
                       << type << "\n");

      // First check to see if we should report an error,
      // which we must if it's just a cycle of unions/aliases/generics
      // which don't have a real type to eventually complete to.
      for (auto *t : llvh::reverse(visited)) {
        // A generic alias is inconclusive until expansion. If it resolves to a
        // union, checkResolvedGenericAliasCycle rechecks its arms.
        if (!llvh::isa<UnionType>(t->info)) {
          break;
        }
        if (t == type) {
          // Managed to enumerate the whole cycle without finding a real type,
          // so this is an error.
          outer.sm_.error(
              type->node->getSourceRange(),
              "ft: type contains a circular reference to itself");
          type->info = outer.flowContext_.getAnyInfo();
          return;
        }
      }

      // Don't canonicalize a union here: its frame may not have visited all of
      // its arms yet. The frame canonicalizes it on the way back, and generics
      // that need it are completed by a later DFS that reaches them.

      LLVM_DEBUG(
          llvh::dbgs() << "Completed cyclic forward type: " << type << " "
                       << type->info->getKindName() << '\n');

      // Early return because the rest of the function assumes we've inserted
      // into visited and attempts to pop from visited as a result.
      return;
    }

    // Remove the most recent visited element when going back up.
    auto popOnExit = llvh::make_scope_exit([&visited]() {
      visited.pop_back();
      LLVM_DEBUG(
          llvh::dbgs() << "Returning to depth " << visited.size() << '\n');
    });

    // An alias of an unresolved generic completes through its target. It is
    // never a separate instantiation, so cycles through it reach the target.
    auto aliasIt = typeAliasResolutions.find(type);
    if (aliasIt != typeAliasResolutions.end() &&
        llvh::isa<GenericType>(type->info)) {
      completeForwardType(aliasIt->second, visited);
      populateTypeAlias(type);
      checkResolvedGenericAliasCycle(type, visited);
      return;
    }

    // First try and complete any generics so we can proceed.
    if (llvh::isa<GenericType>(type->info) ||
        (isForwardGenericClass(type) &&
         !forwardGenericInstantiations.at(type).classSpecialization)) {
      // Forward generics must be instantiated and handled.
      assert(
          forwardGenericInstantiations.count(type) &&
          "type must have a forward generic instantiation");
      completeForwardGeneric(type, visited);
    }

    // The generic was resolved to a more useful type we may have to follow.
    // Now we must continue through any structural types,
    // because they can have loops and may contain information needed to
    // complete the initial type.
    // Unions are special because we may have to canonicalize.
    // Otherwise, we just continue through any structural types.

    if (auto *unionType = llvh::dyn_cast<UnionType>(type->info)) {
      // Forward unions must be checked for loops and canonicalized if possible.
      completeForwardUnion(type, unionType, visited);
    } else if (auto *array = llvh::dyn_cast<ArrayType>(type->info)) {
      completeForwardType(array->getElement(), visited);
    } else if (auto *tuple = llvh::dyn_cast<TupleType>(type->info)) {
      for (Type *t : tuple->getTypes())
        completeForwardType(t, visited);
    } else if (auto *obj = llvh::dyn_cast<ExactObjectType>(type->info)) {
      for (auto &field : obj->getFields())
        completeForwardType(field.type, visited);
    } else if (auto *ftype = llvh::dyn_cast<TypedFunctionType>(type->info)) {
      completeForwardType(ftype->getReturnType(), visited);
      if (ftype->getThisParam())
        completeForwardType(ftype->getThisParam(), visited);
      for (auto &param : ftype->getParams())
        completeForwardType(param.type, visited);
    }

    LLVM_DEBUG(
        llvh::dbgs() << "Completed forward type: " << type << " "
                     << type->info->getKindName() << '\n');
  }

  /// DFS through unions starting at \p type so they get canonicalized in the
  /// right order when possible.
  /// After this function completes, the \c info field of \p type will contain
  /// either a UnionType with canonicalized arms or a single type (if everything
  /// has been deduplicated).
  /// Mutually recursive with completeForwardType and completeForwardGeneric.
  void completeForwardUnion(
      Type *type,
      UnionType *unionType,
      llvh::SetVector<Type *> &visited) {
    assert(unionType && "Expected a union");
    assert(type->info == unionType && "wrong unionType");
    if (unionType->getNumNonLoopingTypes() >= 0) {
      // Already been completed.
      return;
    }

    // Complete all arms of the union before attempting to canonicalize.
    // Copy the arms, since a recursive call may canonicalize this union.
    llvh::SmallVector<Type *, 4> arms(
        unionType->getTypes().begin(), unionType->getTypes().end());
    for (Type *unionArm : arms) {
      completeForwardType(unionArm, visited);
    }

    // It's possible that the info of the type was modified in a recursive call,
    // so recheck it.
    // Canonicalize if necessary.
    // An arm may still be in progress further up the DFS. Then this union is
    // canonicalized by a later DFS that reaches it, e.g. from its own
    // forwardUnions entry.
    unionType = llvh::dyn_cast<UnionType>(type->info);
    if (unionType && llvh::none_of(unionType->getTypes(), isUnresolved)) {
      canonicalizeUnionType(type, unionType);
    }
  }

  /// \return whether \p type is an unexpanded generic or a union that isn't
  /// canonicalized yet. canonicalizeUnionType and specialization lookup can't
  /// use them: unions must be canonical, and all GenericTypes compare equal.
  static bool isUnresolved(Type *type) {
    if (llvh::isa<GenericType>(type->info))
      return true;
    auto *unionType = llvh::dyn_cast<UnionType>(type->info);
    return unionType && unionType->getNumNonLoopingTypes() < 0;
  }

  /// Helper function to canonicalize the \p unionType that is the TypeInfo for
  /// \p type: separate the looping and non-looping types, unique everything,
  /// and sort the non-looping types.
  ///
  /// Populate the info field of \p type with the canonicalized type,
  /// which may be different than the original type (if a union is no longer
  /// required).
  /// Does not error.
  ///
  /// \pre \p unionType is the info field of \p type.
  void canonicalizeUnionType(Type *type, UnionType *unionType) {
    assert(type->info == unionType && "incorrect union type provided");

    // Categorize types.
    llvh::SmallVector<Type *, 4> nonLoopingTypes{};
    llvh::SmallVector<Type *, 4> loopingTypes{};
    UnionType::canonicalizeTypes(
        unionType->getTypes(),
        nonLoopingTypes,
        loopingTypes,
        outer.flowContext_.getBoolean());

    LLVM_DEBUG(
        llvh::dbgs() << "Result type for union: " << nonLoopingTypes.size()
                     << " non-looping and " << loopingTypes.size()
                     << " looping\n");

    // Only one type? Just use it, otherwise we still need a union.
    Type *singleType = nullptr;
    if (nonLoopingTypes.size() == 1 && loopingTypes.empty())
      singleType = nonLoopingTypes.front();
    else if (nonLoopingTypes.empty() && loopingTypes.size() == 1)
      singleType = loopingTypes.front();

    if (singleType) {
      // The arm may still be unresolved or a temporary class, so alias it to
      // pick up the final info.
      typeAliasResolutions[type] = singleType;
      type->info = singleType->info;
    } else {
      unionType->setCanonicalTypes(
          std::move(nonLoopingTypes), std::move(loopingTypes));
    }
  }

  /// Check for a cycle hidden by an expanded generic alias in a union.
  void checkResolvedGenericAliasCycle(
      Type *type,
      llvh::SetVector<Type *> &visited) {
    auto *unionType = llvh::dyn_cast<UnionType>(type->info);
    if (!unionType)
      return;

    // Copy the arms, since a recursive call may canonicalize this union.
    llvh::SmallVector<Type *, 4> arms(
        unionType->getTypes().begin(), unionType->getTypes().end());
    for (Type *arm : arms)
      completeForwardType(arm, visited);
  }

  /// DFS step for forward-declared generic.
  /// Specializes forward-declared generics when necessary, and when this step
  /// completes, \p type will have a TypeInfo that is a real ClassType (no
  /// longer Generic).
  /// Mutually recursive with completeForwardType and completeForwardUnion.
  void completeForwardGeneric(Type *type, llvh::SetVector<Type *> &visited) {
    // Copy because of potential reallocation in the loop during recursive
    // discovery of new forwardGenericInstantiations.
    const GenericTypeInstantiation generic =
        forwardGenericInstantiations.at(type);
    TypeDecl *typeDecl = generic.typeDecl;
    assert(!typeDecl->type && "typeDecl must be generic");

    bool isGenericClass = typeDecl->genericClassDecl;
    if (!llvh::isa<GenericType>(type->info) &&
        (!isGenericClass ||
         forwardGenericInstantiations.at(type).classSpecialization))
      return;

    if (isGenericClass && llvh::isa<GenericType>(type->info)) {
      // Install a temporary nominal type before completing arguments so
      // recursive references like C<Cycle> see a cycle breaker below.
      // We need a new type for each one so that the specialization isn't
      // deduplicated.
      //
      // This is NOT necessarily the deduplicated class specialization yet.
      // The same forward Type gets fixed up below after
      // specializeGenericWithParsedTypes uses the completed argument TypeInfos.
      type->info = outer.flowContext_.createClass(
          typeDecl->genericClassDecl->name, generic.typeArgTypes);
    }

    // Whether all type arguments have a concrete TypeInfo.
    bool argsResolved = true;
    for (Type *arg : generic.typeArgTypes) {
      completeForwardType(arg, visited);

      // An argument may still be resolving further up the DFS.
      if (isUnresolved(arg)) {
        // A generic class needs concrete arguments, so a later DFS that
        // reaches it completes it.
        if (isGenericClass)
          return;
        // A generic alias only needs them to memoize the expansion.
        argsResolved = false;
      }
    }

    if (!isGenericClass) {
      // No genericClassDecl, this is a generic type alias.
      auto *aliasNode = llvh::cast<ESTree::TypeAliasNode>(typeDecl->astNode);
      GenericInfo<Type> &genericInfo =
          outer.getGenericAliasInfoMustExist(aliasNode);

      GenericInfo<Type>::TypeArgsVector typeArgs;
      LLVM_DEBUG(
          llvh::dbgs()
          << "Trying specialization for "
          << llvh::cast<ESTree::IdentifierNode>(aliasNode->_id)->_name->str()
          << "<");
      for (Type *arg : generic.typeArgTypes) {
        LLVM_DEBUG(
            llvh::dbgs() << arg << "(" << arg->info->getKindName() << ") ");
        typeArgs.push_back(arg->info);
      }
      LLVM_DEBUG(llvh::dbgs() << ">\n");

      Type *cached = nullptr;
      if (argsResolved) {
        cached = genericInfo.getSpecialization(typeArgs);
      } else {
        auto it = unresolvedAliasExpansions.find(
            std::make_pair(
                aliasNode, llvh::ArrayRef<Type *>(generic.typeArgTypes)));
        if (it != unresolvedAliasExpansions.end())
          cached = it->second;
      }
      if (Type *resolved = cached) {
        LLVM_DEBUG(
            llvh::errs() << "Found specialization for " << type << ": "
                         << resolved << " " << resolved->info->getKindName()
                         << "\n");
        typeAliasResolutions[type] = resolved;
        populateTypeAlias(type);
        checkResolvedGenericAliasCycle(type, visited);
        return;
      }

      // Expansions that keep producing new type arguments never hit the
      // specialization cache, e.g. type A<T> = [A<[T]>], or through bounds,
      // e.g. type A<T: A<[T]>> = T.
      if (aliasExpansionLimitHit ||
          aliasExpansionDepth >= kMaxAliasExpansionDepth ||
          generic.depth >= kMaxGenericClassDepth) {
        if (!aliasExpansionLimitHit) {
          outer.sm_.error(
              generic.annotation->getSourceRange(),
              "ft: too deeply nested generic type alias expansion");
          aliasExpansionLimitHit = true;
        }
        type->info = outer.flowContext_.getAnyInfo();
        return;
      }
      ++aliasExpansionDepth;
      auto restoreDepth =
          llvh::make_scope_exit([this]() { --aliasExpansionDepth; });

      // This is not a generic class, it's a generic type alias.
      // Specialize it with the provided arguments by resolving the type.
      Type *resolved;
      {
        // Bind the parameters and resolve the RHS in the alias's declaration
        // scope. paramScope below is where the type parameters are bound;
        // bound checks resolve their bounds in it later, so a bound may
        // reference any parameter (e.g. <T, U: T>).
        // The arguments are already-resolved concrete Types.
        auto savedScope = outer.bindingTable_.getCurrentScope();
        outer.bindingTable_.activateScope(genericInfo.bindingTableScope);
        // Restored before the bookkeeping below.
        auto restoreScope = llvh::make_scope_exit([this, &savedScope]() {
          outer.bindingTable_.activateScope(savedScope);
        });

        ScopeRAII paramScope{outer};
        bool populated;
        {
          // Expansions created by the bounds are nested in this one.
          llvh::SaveAndRestore savedDepth{
              outer.genericClassDepth_, generic.depth + 1};
          populated = outer.validateAndBindTypeParameters(
              llvh::cast<ESTree::TypeParameterDeclarationNode>(
                  aliasNode->_typeParameters),
              generic.annotation->getSourceRange(),
              generic.typeArgTypes,
              scope);
        }
        if (!populated) {
          LLVM_DEBUG(llvh::dbgs() << "Failed to bind type parameters\n");
          type->info = outer.flowContext_.getAnyInfo();
          return;
        }

        unsigned errorsBefore = outer.sm_.getErrorCount();

        // Resolve the generic type alias to its specialization.
        // This may add to forwardGenericInstantiations and forwardUnions.
        llvh::SmallDenseSet<ESTree::TypeAliasNode *> visitedTypes{};
        resolved = resolveTypeAnnotation(aliasNode->_right, visitedTypes, 0);

        // If we errored during resolution this isn't a valid type to try
        // specialization on, so give up.
        // This prevents trying to specialize "any" to multiple resolutions.
        if (errorsBefore != outer.sm_.getErrorCount()) {
          type->info = outer.flowContext_.getAnyInfo();
          return;
        }
      }

      if (argsResolved) {
        (void)genericInfo.addSpecialization(
            outer, std::move(typeArgs), resolved);
      } else {
        unresolvedAliasExpansionArgs.push_back(generic.typeArgTypes);
        unresolvedAliasExpansions.try_emplace(
            std::make_pair(
                aliasNode,
                llvh::ArrayRef<Type *>(unresolvedAliasExpansionArgs.back())),
            resolved);
      }

      // "resolved" may still be a forward GenericType (mid alias chain).
      typeAliasResolutions[type] = resolved;

      // First pass: copy any concrete info from "resolved" into type->info
      // before completing it. For cyclic aliases where completing "resolved"
      // recurses into "type", a concrete type->info stops the cycle scan from
      // wrongly reporting a circular reference.
      populateTypeAlias(type);
      completeForwardType(resolved, visited);

      // Second pass: in a chain like type A<X> = X; type B<Y> = A<Y>; ... B<C>,
      // "resolved" was still a forward GenericType during the first pass.
      // completeForwardType set its final info, so re-run to propagate it up.
      if (llvh::isa<GenericType>(type->info))
        populateTypeAlias(type);
      checkResolvedGenericAliasCycle(type, visited);
      return;
    }

    // Handle generic classes.
    // It's possible the arguments aren't fully canonicalized here,
    // which will be handled in fixupUnionsAndGenericTables.
    assert(typeDecl->genericClassDecl && "Expected a generic class");
    llvh::SaveAndRestore savedDepth{outer.genericClassDepth_, generic.depth};
    auto [newDecl, specialization] = outer.specializeGenericWithParsedTypes(
        typeDecl->genericClassDecl,
        generic.annotation->getSourceRange(),
        generic.typeArgTypes,
        typeDecl->genericClassDecl->scope);
    if (!newDecl) {
      type->info = outer.flowContext_.getAnyInfo();
      return;
    }

    // Write it back to the actual vector.
    forwardGenericInstantiations.at(type).classSpecialization = specialization;

    Type *classConsType = outer.flowContext_.findDeclType(newDecl);
    if (!classConsType) {
      outer.sm_.error(
          generic.annotation->getSourceRange(),
          "ft: type contains a circular reference to itself");
      type->info = outer.flowContext_.getAnyInfo();
      return;
    }
    Type *classType =
        llvh::cast<ClassConstructorType>(classConsType->info)->getClassType();
    type->info = classType->info;
  }

  /// Fixup a union by first fixing up any non-looping arms then
  /// recanonicalizing it.
  void fixupNonLoopingArmsRecursive(
      Type *type,
      UnionType *unionType,
      llvh::DenseSet<Type *> visited) {
    assert(unionType->getNumNonLoopingTypes() >= 0 && "must be initialized");
    assert(type->info == unionType && "incorrect unionType");

    if (!visited.insert(type).second)
      return;

    // Copy the arms, since another Type may share this union and be
    // canonicalized by the recursive call.
    llvh::SmallVector<Type *, 4> arms(
        unionType->getNonLoopingTypes().begin(),
        unionType->getNonLoopingTypes().end());
    for (Type *arm : arms) {
      if (auto *unionArm = llvh::dyn_cast<UnionType>(arm->info)) {
        LLVM_DEBUG(llvh::dbgs() << "Fixup union arm recurse: " << type << '\n');
        fixupNonLoopingArmsRecursive(arm, unionArm, visited);
      }
    }

    canonicalizeUnionType(type, unionType);
  }

  /// Post-processing pass for completeForwardDeclarations to clean up unions we
  /// weren't able to more effectively canonicalize when they were created.
  /// Update GenericInfos for classes when this happens,
  /// because classes are nominally typed:
  /// C<T | T> needs to flow into C<T> and vice versa.
  void fixupUnionsAndGenericTables() {
    // Use this visited set to avoid unnecessary work.
    // Now that all the generics have been discovered, this should all be doable
    // by visiting each type at most one time.
    llvh::DenseSet<Type *> visited{};

    // Canonicalize all unions as much as possible.
    for (Type *type : forwardUnions) {
      if (auto *unionType = llvh::dyn_cast<UnionType>(type->info)) {
        LLVM_DEBUG(llvh::dbgs() << "Fixup: " << type << '\n');
        fixupNonLoopingArmsRecursive(type, unionType, visited);
      }
    }

    // Reattempt non-recursive union canonicalization on unions that we visited
    // before we understood all their generics.
    for (auto &[type, generic] : forwardGenericInstantiations) {
      LLVM_DEBUG(llvh::dbgs() << "Fixup: " << type << '\n');
      TypeDecl *typeDecl = generic.typeDecl;
      if (!typeDecl->genericClassDecl)
        continue;

      // We're only concerned with generic classes here,
      // because they're nominally typed so we need to make sure that the
      // specialization for C<T | T> is the exact same as the one for C<T>
      // (either of which may be instantiated after DeclareScopeTypes finishes
      // via an explicit type annotation on some expression).

      GenericInfo<ESTree::Node> &genericInfo =
          outer.getGenericInfoMustExist(typeDecl->genericClassDecl);
      ESTree::Node *oldSpecialization = generic.classSpecialization;
      if (!oldSpecialization) {
        // There's no valid oldSpecialization.
        // There was an error specializing, just ignore this one.
        continue;
      }

      // Make sure the type arguments have been handled.
      GenericInfo<ESTree::Node>::TypeArgsVector typeArgs{};
      for (Type *argType : generic.typeArgTypes) {
        if (auto *unionType = llvh::dyn_cast<UnionType>(argType->info)) {
          fixupNonLoopingArmsRecursive(argType, unionType, visited);
          LLVM_DEBUG(llvh::dbgs() << "Fixup successful: " << type << '\n');
        }
        typeArgs.push_back(argType->info);
      }

      // Get the "real" specialization, make a new one if it doesn't exist yet.
      GenericInfo<ESTree::Node>::TypeArgsRef typeArgsRef = typeArgs;
      ESTree::Node *newSpecialization =
          genericInfo.getSpecialization(typeArgsRef);
      if (!newSpecialization) {
        // Need to make a new specialization entry in the table,
        // but the value can just be the old specialization.
        newSpecialization = oldSpecialization;
        typeArgsRef = genericInfo.addSpecialization(
            outer, std::move(typeArgs), newSpecialization);
      }
      assert(newSpecialization && "must have a specialization");

      if (newSpecialization != oldSpecialization) {
        // Assign the correct specialization to the type.
        // Note that this can't change any looping structure because classes are
        // nominally typed.
        sema::Decl *newDecl = outer.getDecl(
            llvh::cast<ESTree::IdentifierNode>(
                llvh::cast<ESTree::ClassDeclarationNode>(newSpecialization)
                    ->_id));
        assert(newDecl && "already specialized");
        Type *classConsType = outer.getDeclType(newDecl);
        type->info = llvh::cast<ClassConstructorType>(classConsType->info)
                         ->getClassType()
                         ->info;
        generic.classSpecialization = newSpecialization;
        syncForwardGenericClassAliasCopies();
      }
    }
  }

  /// Parse every element of deferredGenericSpecializations,
  /// and enqueue them to be typechecked when the typecheckQueue is drained.
  void parseDeferredGenericClasses() {
    auto savedScope = outer.bindingTable_.getCurrentScope();

    // It's possible we don't want to increment i every iteration,
    // so don't do it in the loop update clause.
    for (size_t i = 0; i < deferredParseGenerics.size(); ++i) {
      // Move out because the vector may grow while we iterate over it.
      DeferredGenericClass deferred{std::move(deferredParseGenerics[i])};
      auto *specialization = deferred.specialization;

      LLVM_DEBUG(
          llvh::dbgs() << "Parsing deferred generic class: "
                       << llvh::cast<ESTree::IdentifierNode>(
                              specialization->_id)
                              ->_name->str()
                       << "\n");

      outer.bindingTable_.activateScope(deferred.scope);
      llvh::SaveAndRestore savedDepth{outer.genericClassDepth_, deferred.depth};

      // Actually parse and move on to the next one.
      // The superClass node has already been visited,
      // and must have been parsed before the current one.
      bool success = outer.parseClassType(
          specialization->_superClass,
          specialization->_superTypeArguments,
          specialization->_body,
          deferred.classType,
          deferred.classScope,
          deferred.classConsType,
          specialization->_typeParameters);
      // Members that failed to parse are not in the class,
      // so typechecking them produces issues.
      if (!success)
        continue;

      // Don't typecheck right now, because we need to parse everything in
      // current scope before descending into child functions.
      outer.typecheckQueue_.emplace_back(std::move(deferred));
    }

    outer.bindingTable_.activateScope(savedScope);
    deferredParseGenerics.clear();
  }

  /// Run a post-processing validation step to check certain errors that are
  /// hard to find during resolution.
  void validateTypes() {
    // Resolving a bound may queue more checks.
    for (size_t i = 0; i < deferredGenericBoundChecks.size(); ++i)
      outer.validateGenericBound(deferredGenericBoundChecks[i]);

    /// Ensure that rest params have valid types.
    for (Type *funcType : funcTypesWithRest) {
      auto *typedFunc = llvh::cast<TypedFunctionType>(funcType->info);
      auto params = typedFunc->getParams();
      if (params.empty() || !params.back().rest)
        continue;
      if (params.back().type &&
          !outer.flowContext_.isArrayClassType(params.back().type)) {
        auto *funcNode =
            llvh::cast<ESTree::FunctionTypeAnnotationNode>(funcType->node);
        auto *restParam =
            llvh::cast<ESTree::FunctionTypeParamNode>(funcNode->_rest);
        outer.sm_.error(
            restParam->_typeAnnotation->getSourceRange(),
            "ft: rest parameter type must be Array<T>");
      }
    }
  }
};

/// Function allows FlowChecker.cpp to call into DeclareScopeTypes.
void FlowChecker::declareScopeTypes(
    const sema::ScopeDecls &decls,
    sema::LexicalScope *scope,
    ESTree::Node *scopeNode) {
  DeclareScopeTypes(*this, decls, scope, scopeNode);
}

/// Function allows FlowChecker.cpp to call into DeclareScopeTypes.
Type *FlowChecker::resolveGenericTypeAlias(
    ESTree::GenericTypeAnnotationNode *gta,
    TypeDecl *typeDecl) {
  return DeclareScopeTypes::resolveGenericTypeAlias(*this, gta, typeDecl);
}

void FlowChecker::findLoopingTypes(Type *type) {
  FindLoopingTypes{type};
}

} // namespace flow
} // namespace hermes

#endif // HERMES_PARSE_FLOW
