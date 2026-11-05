/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// Typechecker for Flow-typed JS.
///
/// Variable type resolution:
/// When entering a block, DeclareScopeTypes and AnnotateScopeDecls are called.
/// DeclareScopeTypes creates Type* for all the bindings which create new types
/// in the scope (type aliases and class declarations) and stores them in
/// the binding table.
/// AnnotateScopeDecls then annotates variable bindings with their types,
/// either by using the explicitly annotated type or inferring them by visiting
/// initializers when possible. The corresponding Decls are associated with the
/// types when possible, 'any' if there was no better type that could be
/// assigned.
/// Once these two functions are done, the visitor is called on the AST nodes in
/// the scope, skipping any initializers visited in AnnotateScopeDecls.
/// When IdentifierNode is encountered, the corresponding Decl is looked up and
/// the type is returned if it exists. Otherwise, any is returned.
///
/// Classes:
/// All classes are handled in two phases: ParseClassType and the typechcking
/// AST visitor.
/// ParseClassType determines the fields and methods of the class as well as
/// their types, resolves the super class, and ensures all the signatures on the
/// fields/methods are valid.
/// The bodies of methods are descended into during the AST visitor on
/// ClassDeclarationNode.
///
/// Generic functions:
/// Generic function declarations are detected during AnnotateScopeDecls and
/// their Decl is marked as \c generic.
/// The generic is registered as a GenericInfo in the FlowChecker, and nothing
/// further is done.
/// When a generic function is invoked, the AST is cloned (or an existing clone
/// is used), the cloned AST is typechecked, and the type of the CallExpression
/// is set to the result type of the cloned AST now that it has been annotated.
///
/// Generic classes:
/// Generic classes can also introduce new types into the scope,
/// so they have to be handled differently from generic functions.
/// The ParseClassType phase has to be deferred until the end of
/// DeclareScopeTypes, because fields in a generic class may refer to fields in
/// other generic classes which haven't been declared yet.
/// This is done by the deferredParseGenerics_ field on FlowChecker.
/// Deferring ParseClassType is only necessary during DeclareScopeTypes;
/// if a class is specialized via a constructor call during the typechecking
/// AST traversal phase, the scope types will already be declared so we can
/// ParseClassType immediately.
/// However, the bodies of the generic class's methods shouldn't be typechecked
/// immediately. Instead they are put into a typecheckQueue_.
/// \code
///    class A<T> extends B<T> {
///      constructor() {
///        super();
///      }
///      foo(): void {}
///    }
///
///    class B<T> {
///      constructor() {}
///      method(): void {
///        new A<boolean>().foo();
///      }
///    }
///
///    new A<number>();
/// \endcode
/// If we eagerly typechecked:
///   A<number>
///   B<number> (via superClass)
///   A<boolean> (via NewExpression)
///   B<boolean> (via superClass)
///   A<boolean> <- ERROR: A<boolean> not initialized yet
/// Queueing the typechecking of the bodies until the very end means that we can
/// use the ClassType immediately after ParseClassType, without having to deal
/// with the creation of potentially more generic specializations recursively.
///
/// Generic type aliases:
/// These are handled similarly to generic classes, but they don't require a new
/// AST node (because IRGen doesn't read type aliases at all).
/// The specialization is stored as a \c Type instead, and the resolution can be
/// called from within DeclareScopeTypes or afterwards from the typechecking
/// visitor.
//===----------------------------------------------------------------------===//

#include "FlowChecker.h"

#include "ESTreeClone.h"
#include "hermes/AST/ASTUtils.h"
#include "hermes/Support/Conversions.h"

#include "llvh/ADT/BitVector.h"
#include "llvh/ADT/MapVector.h"
#include "llvh/ADT/ScopeExit.h"
#include "llvh/ADT/SetVector.h"
#include "llvh/ADT/Twine.h"
#include "llvh/Support/SaveAndRestore.h"

#if HERMES_PARSE_FLOW
#define DEBUG_TYPE "FlowChecker"

namespace hermes {
namespace flow {

FlowChecker::FlowChecker(
    Context &astContext,
    sema::SemContext &semContext,
    FlowContext &flowContext,
    sema::DeclCollectorMapTy &declCollectorMap,
    bool compile)
    : astContext_(astContext),
      sm_(astContext.getSourceErrorManager()),
      bufferMessages_(&sm_),
      semContext_(semContext),
      flowContext_(flowContext),
      declCollectorMap_(declCollectorMap),
      kw_(semContext.kw),
      declTypes_(flowContext.declTypeMap(FlowContext::ForUpdate())),
      compile_(compile) {}

bool FlowChecker::run(ESTree::ProgramNode *rootNode) {
  if (sm_.getErrorCount())
    return false;

  assert(
      astContext_.isStrictMode() && semContext_.getGlobalFunction()->strict &&
      "Types can only be used in strict mode");
  // Defensive programming.
  if (!semContext_.getGlobalFunction()->strict) {
    sm_.error(rootNode->getStartLoc(), "ft: strict mode required");
    return false;
  }

  FunctionContext globalFunc(
      *this, rootNode, nullptr, flowContext_.getAny(), flowContext_.getVoid());
  ScopeRAII scope(*this);
  declareNativeTypes(rootNode->getScope());
  populateBuiltinMethods();
  if (!resolveScopeTypesAndAnnotate(rootNode, rootNode->getScope()))
    return false;
  visitESTreeNodeNoReplace(*this, rootNode);
  drainTypecheckQueue();
  return sm_.getErrorCount() == 0;
}

void FlowChecker::declareNativeTypes(sema::LexicalScope *rootScope) {
  Type *const number = flowContext_.getNumber();
  Type *const cptr = flowContext_.getCPtr();

  /// Declare a native type as an alias for \p type.
  auto declare = [this, rootScope](
                     llvh::StringRef name, NativeCType ctype, Type *type) {
    UniqueString *ident =
        astContext_.getIdentifier(name).getUnderlyingPointer();
    bindingTable_.try_emplace(ident, TypeDecl(type, rootScope, nullptr));
    nativeTypes_.try_emplace(ident, ctype);
  };

#define NATIVE_TYPE(name, str) \
  declare(llvh::StringLiteral("c_" #name), NativeCType::name, number);
#define NATIVE_PTR(name, str) \
  declare(llvh::StringLiteral("c_" #name), NativeCType::name, cptr);
#define NATIVE_CTYPE(name, str) \
  declare(llvh::StringLiteral("c_" #name), NativeCType::c_##name, number);
#include "hermes/AST/NativeTypes.def"
}

void FlowChecker::populateBuiltinMethods() {
  for (ESTree::FunctionDeclarationNode *funcDecl :
       semContext_.getBuiltinDeclarations()) {
    // Extract the 'this' parameter type for keying.
    // We only need the TypeKind for lookup via findBuiltinMethod.
    if (funcDecl->_params.empty()) {
      sm_.error(
          funcDecl->getStartLoc(),
          "ft: builtin function must have annotated 'this' as first parameter");
      continue;
    }

    auto *firstParam =
        llvh::dyn_cast<ESTree::IdentifierNode>(&funcDecl->_params.front());
    if (!firstParam || firstParam->_name != kw_.identThis ||
        !firstParam->_typeAnnotation) {
      sm_.error(
          funcDecl->getStartLoc(),
          "ft: builtin function must have annotated 'this' as first parameter");
      continue;
    }

    // Determine the TypeKind from the AST structure without parsing.
    // This avoids creating types with placeholders.
    auto *typeAnnotation =
        llvh::cast<ESTree::TypeAnnotationNode>(firstParam->_typeAnnotation);
    ESTree::Node *innerType = typeAnnotation->_typeAnnotation;

    TypeKind thisTypeKind = TypeKind::InferencePlaceholder;
    if (llvh::isa<ESTree::ArrayTypeAnnotationNode>(innerType)) {
      thisTypeKind = TypeKind::Array;
    } else if (llvh::isa<ESTree::StringTypeAnnotationNode>(innerType)) {
      thisTypeKind = TypeKind::String;
    } else if (llvh::isa<ESTree::NumberTypeAnnotationNode>(innerType)) {
      thisTypeKind = TypeKind::Number;
    } else {
      sm_.error(
          funcDecl->getStartLoc(),
          "ft: unsupported builtin 'this' type "
          "(only T[], string and number supported right now)");
      continue;
    }

    assert(
        thisTypeKind != TypeKind::InferencePlaceholder && "must find a type");

    // Generic builtins are not supported.
    if (funcDecl->_typeParameters) {
      sm_.error(
          funcDecl->getStartLoc(),
          "ft: generic builtin methods are not supported");
      continue;
    }

    if (!funcDecl->_returnType) {
      sm_.error(
          funcDecl->getStartLoc(),
          "ft: builtin function must have a return type annotation");
      continue;
    }

    // Name of the function being called.
    auto *id = llvh::cast<ESTree::IdentifierNode>(funcDecl->_id);

    // Currently all builtin functions from TypedLib are non-static.
    BuiltinMethodKey key{
        thisTypeKind,
        id->_name,
        /* isStatic */ false};
    flowContext_.registerBuiltinMethod(key, funcDecl);
  }
}

void FlowChecker::recursionDepthExceeded(ESTree::Node *n) {
  sm_.error(
      n->getEndLoc(),
      "ft: too many nested expressions/statements/declarations");
}

void FlowChecker::visit(ESTree::ProgramNode *node) {
  visitESTreeChildren(*this, node);
}

void FlowChecker::visitNonGenericFunctionDeclaration(
    ESTree::FunctionDeclarationNode *node,
    sema::Decl *decl) {
  Type *declType = getDeclType(decl);

  // If this declaration is of a global property, then it doesn't have a
  // function type, because that would be unsound. So, we have to parse the
  // function type here, to make it available inside the function.
  auto *ftype = llvh::dyn_cast<BaseFunctionType>(declType->info);
  if (!ftype) {
    declType = parseFunctionType(
        node->_params, node->_returnType, node->_async, node->_generator);
    ftype = llvh::cast<BaseFunctionType>(declType->info);
  }

  Type *thisType = nullptr;
  if (auto *typed = llvh::dyn_cast<TypedFunctionType>(ftype))
    thisType = typed->getThisParam();

  setNodeType(node, declType);
  FunctionContext functionContext(
      *this, node, declType, thisType, flowContext_.getVoid());
  visitFunctionLike(node, node->_body, node->_params);
}

void FlowChecker::visit(ESTree::FunctionDeclarationNode *node) {
  // Validate any decorations attached via Hermes.decorate(...) calls.
  // These are stored on the FunctionLikeDecoration rather than as AST
  // _decorators, so visitESTreeChildren does not reach them.
  if (auto *funcDeco =
          ESTree::getDecoration<ESTree::FunctionLikeDecoration>(node)) {
    for (auto &decorator : funcDeco->decorations) {
      visit(llvh::cast<ESTree::DecoratorNode>(&decorator), node);
    }
  }

  sema::Decl *decl = semContext_.getDeclarationDecl(
      llvh::cast<ESTree::IdentifierNode>(node->_id));
  assert(decl && "function declaration must have been resolved");

  // Builtin declarations are registered but their body is not type-checked.
  // They may contain implementation patterns that are not supported by the
  // type checker (e.g., string indexing in charAt).
  if (decl->kind == sema::Decl::Kind::TypedBuiltin) {
    return;
  }

  // If it has type parameters, it's either a generic declaration
  // or a specialization which should be typechecked directly at clone time
  // by calling visitNonGenericFunctionDeclaration.
  // e.g.
  //   function foo() { bar<number() }
  //   function bar<T>() {}
  // will create bar<number> in after bar<T>, but we don't want to visit after
  // visiting foo.
  if (node->_typeParameters) {
    return;
  }

  visitNonGenericFunctionDeclaration(node, decl);
}

void FlowChecker::visit(ESTree::FunctionExpressionNode *node) {
  if (node->_typeParameters) {
    sm_.error(
        node->_typeParameters->getStartLoc(),
        "ft: type parameters not supported on function expressions");
    return;
  }

  Type *ftype = parseFunctionType(
      node->_params, node->_returnType, node->_async, node->_generator);
  setNodeType(node, ftype);

  // If there is an id, resolve its type.
  if (auto id = llvh::cast_or_null<ESTree::IdentifierNode>(node->_id)) {
    sema::Decl *decl = getDecl(id);
    assert(decl && "function expression id must be resolved");
    assert(
        declTypes_.count(decl) == 0 &&
        "function expression id type already resolved");
    recordDecl(decl, ftype, id, node);
  }

  Type *thisType = nullptr;
  if (auto *typed = llvh::dyn_cast<TypedFunctionType>(ftype->info))
    thisType = typed->getThisParam();

  FunctionContext functionContext(
      *this, node, ftype, thisType, flowContext_.getVoid());
  visitFunctionLike(node, node->_body, node->_params);
}

void FlowChecker::visit(ESTree::ArrowFunctionExpressionNode *node) {
  visitArrowFunction(node, /*constraint=*/nullptr);
}

void FlowChecker::visitArrowFunction(
    ESTree::ArrowFunctionExpressionNode *node,
    TypedFunctionType *constraint) {
  if (node->_typeParameters) {
    sm_.error(
        node->_typeParameters->getStartLoc(),
        "ft: type parameters not supported on function expressions");
    return;
  }

  Type *ftype = parseFunctionType(
      node->_params,
      node->_returnType,
      node->_async,
      false /*node->_generator*/,
      /*defaultReturnType=*/nullptr,
      /*defaultThisType=*/nullptr,
      constraint);
  setNodeType(node, ftype);

  FunctionContext functionContext(
      *this,
      node,
      ftype,
      curFunctionContext_->thisParamType,
      curFunctionContext_->newTargetType,
      curFunctionContext_->superType);
  visitFunctionLike(node, node->_body, node->_params);
}

class FlowChecker::ParseClassType {
  FlowChecker &outer_;
  sema::LexicalScope *classScope_;
  /// The class's type parameter declaration, if any. Used to shadow type
  /// params with empty for static members.
  ESTree::TypeParameterDeclarationNode *classTypeParams_ = nullptr;

  llvh::SmallDenseMap<UniqueString *, ESTree::Node *> fieldNames{};
  llvh::SmallVector<ClassType::Field, 4> fields{};

  llvh::SmallDenseMap<UniqueString *, ESTree::Node *> methodNames{};
  llvh::SmallVector<ClassType::Field, 4> methods{};

  llvh::SmallDenseMap<UniqueString *, ESTree::Node *> staticNames{};
  llvh::SmallVector<ClassType::Field, 4> statics{};

  /// Maps field/method names to their index in the corresponding field
  /// vector, for O(1) lookup when checking for duplicate declarations.
  llvh::SmallDenseMap<UniqueString *, size_t> methodFieldIdx{};
  llvh::SmallDenseMap<UniqueString *, size_t> staticFieldIdx{};

  Type *constructorType = nullptr;
  /// The ClassConstructorType wrapping the class, used as the 'this' type for
  /// static methods. Never null.
  Type *classConsType_;
  Type *superClassType;

  size_t nextFieldLayoutSlotIR = 0;
  size_t nextMethodLayoutSlotIR = 0;

 public:
  ParseClassType(
      FlowChecker &outer,
      ESTree::Node *superClass,
      ESTree::Node *superTypeArguments,
      ESTree::Node *body,
      Type *classType,
      sema::LexicalScope *classScope,
      Type *classConsType,
      ESTree::Node *classTypeParameters)
      : outer_(outer),
        classScope_(classScope),
        classTypeParams_(
            llvh::cast_or_null<ESTree::TypeParameterDeclarationNode>(
                classTypeParameters)),
        classConsType_(classConsType),
        superClassType(
            resolveSuperClass(outer, superClass, superTypeArguments)) {
    assert(classConsType_ && "class must have a ClassConstructorType");
    LLVM_DEBUG(
        llvh::dbgs()
        << "ParseClassType for: "
        << llvh::cast<ClassType>(classType->info)->getClassNameOrDefault().str()
        << '\n');

    ClassType *superClassTypeInfo = nullptr;
    if (superClassType) {
      superClassTypeInfo = llvh::cast<ClassType>(superClassType->info);
      // Offset based on the superclass if necessary, to avoid overwriting
      // existing fields.
      assert(
          superClassTypeInfo->isInitialized() &&
          "superClass must be initialized");
      // Private instance fields already have real layoutSlotIR values and
      // are counted in getNumLayoutSlots(). Private methods have
      // layoutSlotIR=None and are stored in Variables, not on the home object,
      // so they don't need layout slots either.
      nextFieldLayoutSlotIR = superClassTypeInfo->getNumLayoutSlots();
      nextMethodLayoutSlotIR =
          superClassTypeInfo->getHomeObjectTypeInfo()->getNumLayoutSlots();
    }

    auto *classBody = llvh::cast<ESTree::ClassBodyNode>(body);
    for (ESTree::Node &node : classBody->_body) {
      if (auto *prop = llvh::dyn_cast<ESTree::ClassPropertyNode>(&node)) {
        Type *fieldType;
        if (prop->_static && classTypeParams_) {
          ScopeRAII staticScope(outer_);
          shadowClassTypeParamsWithEmpty();
          fieldType = parseClassProperty(prop);
        } else {
          fieldType = parseClassProperty(prop);
        }
        outer_.setNodeType(&node, fieldType);
      } else if (
          auto *prop =
              llvh::dyn_cast<ESTree::ClassPrivatePropertyNode>(&node)) {
        Type *fieldType;
        if (prop->_static && classTypeParams_) {
          ScopeRAII staticScope(outer_);
          shadowClassTypeParamsWithEmpty();
          fieldType = parseClassPrivateProperty(prop);
        } else {
          fieldType = parseClassPrivateProperty(prop);
        }
        outer_.setNodeType(&node, fieldType);
      } else if (
          auto *method = llvh::dyn_cast<ESTree::MethodDefinitionNode>(&node)) {
        Type *methodType = parseMethodDefinition(classType, classBody, method);
        // Methods have FunctionExpression values.
        // Associate the same type with both the outer and inner nodes.
        outer_.setNodeType(method, methodType);
        outer_.setNodeType(method->_value, methodType);
      } else {
        outer_.sm_.error(
            node.getSourceRange(),
            "ft: unsupported class member " + node.getNodeName());
      }
    }

    Type *homeObjectType = outer_.flowContext_.createType(
        outer_.flowContext_.createClass(Identifier{}));
    llvh::cast<ClassType>(homeObjectType->info)
        ->init(
            methods,
            /* constructorType */ nullptr,
            /* homeObjectType */ nullptr,
            /* staticObjectType */ nullptr,
            superClassTypeInfo ? superClassTypeInfo->getHomeObjectType()
                               : nullptr,
            nextMethodLayoutSlotIR,
            /* classConsType */ nullptr);

    // Create the staticObjectType if there are static elements or the
    // superclass has one (to preserve the inheritance chain).
    Type *staticObjectType = nullptr;
    if (!statics.empty() ||
        (superClassTypeInfo && superClassTypeInfo->getStaticObjectTypeInfo())) {
      staticObjectType = outer_.flowContext_.createType(
          outer_.flowContext_.createClass(Identifier{}));
      llvh::cast<ClassType>(staticObjectType->info)
          ->init(
              statics,
              /* constructorType */ nullptr,
              /* homeObjectType */ nullptr,
              /* staticObjectType */ nullptr,
              superClassTypeInfo ? superClassTypeInfo->getStaticObjectType()
                                 : nullptr,
              0,
              /* classConsType */ nullptr);
    }

    llvh::cast<ClassType>(classType->info)
        ->init(
            fields,
            constructorType,
            homeObjectType,
            staticObjectType,
            superClassType,
            nextFieldLayoutSlotIR,
            classConsType_);
  }

 private:
  /// Shadow class type parameters with empty in the current scope.
  /// Must be called after opening a ScopeRAII.
  void shadowClassTypeParamsWithEmpty() {
    assert(classTypeParams_ && "must have type params");
    outer_.shadowClassTypeParams(classTypeParams_, classScope_);
  }

  Type *resolveSuperClass(
      FlowChecker &outer_,
      ESTree::Node *superClass,
      ESTree::Node *superTypeArguments) {
    if (!superClass)
      return nullptr;
    auto *superClassIdent = llvh::dyn_cast<ESTree::IdentifierNode>(superClass);
    if (!superClassIdent) {
      outer_.sm_.error(
          superClass->getStartLoc(),
          "ft: only identifiers may be extended as superclasses");
      return nullptr;
    }

    sema::Decl *superDecl = outer_.getDecl(superClassIdent);
    if (superDecl->generic) {
      // Superclass type parameters aren't stored in _superClass,
      // but rather in a separate field in the class declaration itself,
      // so we need to handle it separately here.
      // Importantly, we can't defer the parsing of the superclass
      // because we need to be able to read the type of the superclass
      // to extend it.
      llvh::SaveAndRestore<std::vector<DeferredGenericClass> *>
          savedDeferredGenerics{outer_.deferredParseGenerics_, nullptr};
      auto *superTypeArgs =
          llvh::cast_or_null<ESTree::TypeParameterInstantiationNode>(
              superTypeArguments);
      if (!superTypeArgs) {
        outer_.sm_.error(
            superClassIdent->getSourceRange(),
            "ft: missing type arguments for superclass");
        return nullptr;
      }
      outer_.resolveGenericClassSpecialization(
          superClassIdent, superTypeArgs, superDecl);
    } else if (superTypeArguments) {
      outer_.sm_.error(
          superTypeArguments->getStartLoc(),
          "ft: type arguments are not allowed for non-generic classes");
      return nullptr;
    }

    Type *superType = outer_.flowContext_.findNodeType(superClass);
    if (!superType) {
      outer_.sm_.error(superClass->getStartLoc(), "ft: super type unknown");
      return nullptr;
    }
    auto *superClassConsType =
        llvh::dyn_cast<ClassConstructorType>(superType->info);
    if (!superClassConsType) {
      outer_.sm_.error(
          superClass->getStartLoc(), "ft: super type must be a class");
      return nullptr;
    }
    Type *superClassType = superClassConsType->getClassType();
    // The super class type must already be fully parsed. It won't be if the
    // class extends itself or if the super class is declared later in source,
    // since classes are parsed in declaration order.
    if (!llvh::cast<ClassType>(superClassType->info)->isInitialized()) {
      outer_.sm_.error(
          superClass->getStartLoc(),
          "ft: super class used before it is defined");
      return nullptr;
    }
    return superClassType;
  }

  Type *parseClassProperty(ESTree::ClassPropertyNode *prop) {
    if (prop->_computed || prop->_declare || prop->_optional ||
        prop->_variance) {
      outer_.sm_.error(
          prop->getSourceRange(), "ft: unsupported property attributes");
      return outer_.flowContext_.getAny();
    }
    return parseClassPropertyImpl(
        prop, prop->_key, prop->_value, prop->_static, prop->_typeAnnotation);
  }

  Type *parseClassPrivateProperty(ESTree::ClassPrivatePropertyNode *prop) {
    if (prop->_declare || prop->_optional || prop->_variance) {
      outer_.sm_.error(
          prop->getSourceRange(), "ft: unsupported property attributes");
      return outer_.flowContext_.getAny();
    }
    return parseClassPropertyImpl(
        prop, prop->_key, prop->_value, prop->_static, prop->_typeAnnotation);
  }

  Type *parseClassPropertyImpl(
      ESTree::Node *prop,
      ESTree::Node *key,
      ESTree::Node *value,
      bool isStatic,
      ESTree::Node *typeAnnotation) {
    bool isPrivate = llvh::isa<ESTree::ClassPrivatePropertyNode>(prop);

    if (!llvh::isa<ESTree::IdentifierNode>(key)) {
      outer_.sm_.error(
          prop->getSourceRange(), "ft: property name must be an identifier");
      return outer_.flowContext_.getAny();
    }

    auto *id = llvh::cast<ESTree::IdentifierNode>(key);
    Identifier name = isPrivate
        ? outer_.astContext_.getPrivateNameIdentifier(id->_name)
        : Identifier::getFromPointer(id->_name);

    Type *fieldType;
    if (typeAnnotation) {
      fieldType = outer_.parseTypeAnnotation(
          llvh::cast<ESTree::TypeAnnotationNode>(typeAnnotation)
              ->_typeAnnotation);
    } else if ((llvh::isa<ESTree::NullLiteralNode>(value) ||
                llvh::isa<ESTree::NumericLiteralNode>(value) ||
                llvh::isa<ESTree::BooleanLiteralNode>(value) ||
                llvh::isa<ESTree::StringLiteralNode>(value) ||
                llvh::isa<ESTree::RegExpLiteralNode>(value) ||
                llvh::isa<ESTree::BigIntLiteralNode>(value))) {
      // TODO: Figure out a more general-purpose way to do inference here.
      // We likely need to share some of the logic of AnnotateScopeDecls,
      // given that there's IDZ considerations during the inference of each
      // property's initializer.
      outer_.visitedInits_.insert(value);
      outer_.visitExpression(value, prop, nullptr);
      // A class field is mutable, so widen a fresh literal initializer type to
      // its base type, mirroring let/var widening.
      fieldType = outer_.widenLiteralType(outer_.getNodeTypeOrAny(value));
    } else {
      // Unable to infer, just assume 'any'.
      fieldType = outer_.flowContext_.getAny();
    }

    // Check if an instance field is inherited, and reuse the index.
    // Private fields are never inherited.
    // Inherited statics are always an error; the super's staticObjectType
    // field name map already covers the whole superclass chain.
    const ClassType::Field *superField = nullptr;
    if (!isPrivate && superClassType) {
      auto *superClassTypeInfo = llvh::cast<ClassType>(superClassType->info);
      if (isStatic) {
        if (auto *superStaticInfo =
                superClassTypeInfo->getStaticObjectTypeInfo()) {
          if (auto superIt = superStaticInfo->findPublicField(name)) {
            const ClassType::Field *inherited = superIt->getField();
            if (inherited->isMethod()) {
              outer_.sm_.error(
                  id->getStartLoc(),
                  "ft: cannot override method " + name.str() + " with a field");
            } else {
              // Statics live in Variables instead of slots on the class, so a
              // redeclaration would create a second, independent storage
              // location: a base-constructor-typed value holding the subclass
              // would still read the base class's Variable.
              outer_.sm_.error(
                  id->getStartLoc(),
                  "ft: cannot override static field " + name.str());
              if (ESTree::IdentifierNode *prevId = inherited->staticKeyNode)
                outer_.sm_.note(
                    prevId->getSourceRange(),
                    "ft: previous declaration of " + name.str());
            }
            return outer_.flowContext_.getAny();
          }
        }
      } else {
        auto superIt = superClassTypeInfo->findPublicField(name);
        if (superIt) {
          superField = superIt->getField();
          if (!fieldType->info->equals(superField->type->info)) {
            outer_.sm_.error(
                prop->getStartLoc(),
                "ft: incompatible field type for override");
          }
        } else if (superClassTypeInfo->getHomeObjectTypeInfo()->findPublicField(
                       name)) {
          // Methods live on the home object and fields on the instance, so a
          // field shadowing an inherited method would create two independent
          // storage locations for the same name.
          outer_.sm_.error(
              id->getStartLoc(),
              "ft: cannot override method " + name.str() + " with a field");
          return outer_.flowContext_.getAny();
        }
      }
    }

    // Fields and methods share a namespace, so check the method names too.
    if (!isStatic) {
      auto methodIt = methodNames.find(name.getUnderlyingPointer());
      if (methodIt != methodNames.end()) {
        outer_.sm_.error(
            id->getStartLoc(), "ft: field " + name.str() + " already declared");
        outer_.sm_.note(
            methodIt->second->getSourceRange(),
            "ft: previous declaration of " + name.str());
        return outer_.flowContext_.getAny();
      }
    }

    // Check if the field is already declared.
    auto &nameMap = isStatic ? staticNames : fieldNames;
    auto [it, inserted] =
        nameMap.try_emplace(name.getUnderlyingPointer(), prop);
    if (!inserted) {
      outer_.sm_.error(
          id->getStartLoc(), "ft: field " + name.str() + " already declared");
      outer_.sm_.note(
          it->second->getSourceRange(),
          "ft: previous declaration of " + name.str());
      return outer_.flowContext_.getAny();
    }

    if (isStatic) {
      staticFieldIdx[name.getUnderlyingPointer()] = statics.size();
      // Static fields use layoutSlotIR = None because they will use
      // Variables, not object property slots.
      // Create a Decl so IRGen can associate a Variable with it.
      auto *decl = outer_.semContext_.newDeclInScope(
          id->_name, sema::Decl::Kind::Const, classScope_);
      outer_.semContext_.setExpressionDecl(id, decl);
      statics.emplace_back(
          name,
          fieldType,
          OptValue<size_t>(llvh::None),
          isPrivate,
          nullptr,
          false,
          id);
    } else if (superField) {
      assert(!isPrivate);
      fields.emplace_back(
          name,
          fieldType,
          superField->layoutSlotIR,
          /* isPrivate */ false);
    } else {
      fields.emplace_back(name, fieldType, nextFieldLayoutSlotIR++, isPrivate);
    }

    return fieldType;
  }

  Type *parseMethodDefinition(
      Type *classType,
      ESTree::ClassBodyNode *classBody,
      ESTree::MethodDefinitionNode *method) {
    auto *fe = llvh::cast<ESTree::FunctionExpressionNode>(method->_value);

    ESTree::PrivateNameNode *privateNameNode =
        llvh::dyn_cast<ESTree::PrivateNameNode>(method->_key);

    if (method->_kind == outer_.kw_.identConstructor) {
      if (fe->_typeParameters) {
        outer_.sm_.error(
            fe->_typeParameters->getSourceRange(),
            "ft: type parameters not supported directly on constructor");
        return outer_.flowContext_.getAny();
      }
      if (hermes::findDecorator(
              method->_decorators,
              {outer_.kw_.identHermes, outer_.kw_.identFinal})) {
        outer_.sm_.error(
            method->getStartLoc(),
            "ft: @Hermes.final cannot be applied to a constructor");
        return outer_.flowContext_.getAny();
      }
      if (hermes::findDecorator(
              method->_decorators,
              {outer_.kw_.identHermes, outer_.kw_.identOverload})) {
        outer_.sm_.error(
            method->getStartLoc(),
            "ft: @Hermes.overload cannot be applied to a constructor");
        return outer_.flowContext_.getAny();
      }

      // Constructor
      if (fe->_returnType) {
        outer_.sm_.error(
            fe->_returnType->getSourceRange(),
            "ft: constructor cannot declare a return type");
      }
      // Check for disallowed attributes. Note that the parser already
      // prevents some of these, but it doesn't hurt to check again.
      if (method->_static || fe->_async || fe->_generator) {
        outer_.sm_.error(
            method->getStartLoc(),
            "constructor cannot be static, a generator or async");
      }

      constructorType = outer_.parseFunctionType(
          fe->_params,
          nullptr,
          false,
          false,
          outer_.flowContext_.getVoid(),
          classType);

      return constructorType;
    } else {
      // Non-constructor method

      bool finalMethod = hermes::findDecorator(
          method->_decorators, {outer_.kw_.identHermes, outer_.kw_.identFinal});
      bool overloadMethod = hermes::findDecorator(
          method->_decorators,
          {outer_.kw_.identHermes, outer_.kw_.identOverload});

      // @Hermes.overload requires @Hermes.final.
      if (overloadMethod && !finalMethod) {
        outer_.sm_.error(
            method->getStartLoc(),
            "ft: @Hermes.overload requires @Hermes.final");
        return outer_.flowContext_.getAny();
      }

      // Detect getter/setter kind.
      bool isGetter = (method->_kind == outer_.kw_.identGet);
      bool isSetter = (method->_kind == outer_.kw_.identSet);
      if (isGetter || isSetter) {
        if (overloadMethod) {
          outer_.sm_.error(
              method->getStartLoc(),
              "ft: @Hermes.overload cannot be applied to getters/setters");
          return outer_.flowContext_.getAny();
        }
        if (!finalMethod) {
          outer_.sm_.error(
              method->getStartLoc(),
              isGetter ? "ft: getters must be marked as @Hermes.final"
                       : "ft: setters must be marked as @Hermes.final");
          return outer_.flowContext_.getAny();
        }
        if (fe->_typeParameters) {
          outer_.sm_.error(
              fe->_typeParameters->getSourceRange(),
              isGetter ? "ft: generic getter not supported"
                       : "ft: generic setter not supported");
          return outer_.flowContext_.getAny();
        }
      }

      if (fe->_typeParameters && !finalMethod) {
        outer_.sm_.error(
            fe->_typeParameters->getSourceRange(),
            "ft: generic methods must be marked as final with @Hermes.final");
        return outer_.flowContext_.getAny();
      }

      if (method->_computed) {
        outer_.sm_.error(
            method->getStartLoc(),
            "ft: computed property names in classes are unsupported");
        return outer_.flowContext_.getAny();
      }
      if (!privateNameNode &&
          !llvh::isa<ESTree::IdentifierNode>(method->_key)) {
        outer_.sm_.error(
            method->_key->getSourceRange(),
            "ft: method name must be an identifier");
        return outer_.flowContext_.getAny();
      }
      if (fe->_async || fe->_generator) {
        outer_.sm_.error(
            method->getStartLoc(), "ft: async/generator methods unsupported");
        return outer_.flowContext_.getAny();
      }
      Identifier name;
      ESTree::IdentifierNode *id;
      if (privateNameNode) {
        id = llvh::cast<ESTree::IdentifierNode>(privateNameNode->_id);
        name = outer_.astContext_.getPrivateNameIdentifier(id->_name);
      } else {
        id = llvh::cast<ESTree::IdentifierNode>(method->_key);
        name = Identifier::getFromPointer(id->_name);
      }

      Type *methodType;
      if (fe->_typeParameters) {
        methodType = outer_.flowContext_.getGeneric();
        outer_.registerGenericMethod(
            method,
            classBody,
            outer_.bindingTable_.getCurrentScope(),
            classScope_,
            llvh::cast<ClassType>(classType->info),
            classTypeParams_);
      } else {
        // Create a Decl for non-generic final methods and all static
        // methods, and store it on the method key IdentifierNode.
        // Generic final methods get Decls through
        // specializedMethodDecls_.
        // Static methods always use Variables (no layout slots).
        if (finalMethod || method->_static) {
          auto *keyId = ESTree::getPropertyIdentifier(method->_key);
          auto *decl = outer_.semContext_.newDeclInScope(
              keyId->_name, sema::Decl::Kind::Const, classScope_);
          outer_.semContext_.setExpressionDecl(keyId, decl);
        }

        // A setter's return value is discarded, so it always returns void.
        Type *defaultReturnType =
            isSetter ? outer_.flowContext_.getVoid() : nullptr;

        if (method->_static && classTypeParams_) {
          ScopeRAII staticScope(outer_);
          shadowClassTypeParamsWithEmpty();
          methodType = outer_.parseFunctionType(
              fe->_params,
              fe->_returnType,
              fe->_async,
              fe->_generator,
              defaultReturnType,
              classConsType_);
        } else {
          methodType = outer_.parseFunctionType(
              fe->_params,
              fe->_returnType,
              fe->_async,
              fe->_generator,
              defaultReturnType,
              method->_static ? classConsType_ : classType);
        }
      }

      // Validate setter has exactly 1 parameter.
      if (isSetter) {
        auto *fnType = llvh::dyn_cast<TypedFunctionType>(methodType->info);
        if (fnType && fnType->getParams().size() != 1) {
          outer_.sm_.error(
              method->getStartLoc(),
              "ft: setter must have exactly one parameter");
          return outer_.flowContext_.getAny();
        }
        if (fnType && !llvh::isa<VoidType>(fnType->getReturnType()->info)) {
          outer_.sm_.error(
              fe->_returnType->getSourceRange(),
              "ft: setter return type must be void");
          return outer_.flowContext_.getAny();
        }
      }

      // Compute the field type and accessor function types.
      Type *getterFnType = nullptr;
      Type *setterFnType = nullptr;
      Type *fieldType = methodType;
      if (isGetter) {
        getterFnType = methodType;
        auto *fnType = llvh::dyn_cast<TypedFunctionType>(methodType->info);
        fieldType =
            fnType ? fnType->getReturnType() : outer_.flowContext_.getAny();
      } else if (isSetter) {
        setterFnType = methodType;
        fieldType = outer_.flowContext_.getVoid();
      }

      // Check if the method is inherited, and reuse the index.
      // Private fields are never inherited.
      // Static methods look up in the super's staticObjectType.
      const ClassType::Field *superMethod = nullptr;
      if (!privateNameNode && superClassType) {
        auto *superClassTypeInfo = llvh::cast<ClassType>(superClassType->info);
        hermes::OptValue<ClassType::FieldLookupEntry> superIt;
        if (method->_static) {
          if (auto *superStaticInfo =
                  superClassTypeInfo->getStaticObjectTypeInfo()) {
            superIt = superStaticInfo->findPublicField(name);
            if (superIt && !superIt->getField()->isMethod()) {
              outer_.sm_.error(
                  id->getSourceRange(),
                  "ft: cannot override field " + name.str() + " with a method");
              return outer_.flowContext_.getAny();
            }
            // Statics are resolved at compile time, including through 'this'
            // and subclass names, so a shadowing static would only be reached
            // through its own class name, unlike ES dispatch on the receiver.
            if (superIt) {
              outer_.sm_.error(
                  id->getSourceRange(),
                  "ft: cannot override static method " + name.str());
              if (ESTree::IdentifierNode *prev =
                      superIt->getField()->staticKeyNode)
                outer_.sm_.note(
                    prev->getSourceRange(),
                    "ft: previous declaration of " + name.str());
              return outer_.flowContext_.getAny();
            }
          }
        } else {
          superIt =
              superClassTypeInfo->getHomeObjectTypeInfo()->findPublicField(
                  name);
          if (!superIt && superClassTypeInfo->findPublicField(name)) {
            outer_.sm_.error(
                id->getSourceRange(),
                "ft: cannot override field " + name.str() + " with a method");
            return outer_.flowContext_.getAny();
          }
        }
        if (superIt) {
          if (fe->_typeParameters) {
            outer_.sm_.error(
                id->getSourceRange(),
                "ft: cannot override with generic method");
            return outer_.flowContext_.getAny();
          }
          // Field is inherited.
          superMethod = superIt->getField();
          if (superMethod->finalMethod) {
            outer_.sm_.error(
                id->getSourceRange(), "ft: cannot override final method");
            return outer_.flowContext_.getAny();
          }
          if (finalMethod) {
            outer_.sm_.error(
                id->getSourceRange(),
                "ft: cannot mark override as @Hermes.final");
            return outer_.flowContext_.getAny();
          }
          // Mark the super field as overridden.
          superIt->getFieldMut()->overridden = true;
          // Actually check the type of the method later,
          // because we have to wait for the class to be initialized.
        }
      }

      // Check the field names too.
      if (!method->_static) {
        auto fieldIt = fieldNames.find(name.getUnderlyingPointer());
        if (fieldIt != fieldNames.end()) {
          outer_.sm_.error(
              method->_key->getStartLoc(),
              "ft: method " + name.str() + " already declared");
          outer_.sm_.note(
              fieldIt->second->getSourceRange(),
              "ft: previous declaration of " + name.str());
          return outer_.flowContext_.getAny();
        }
      }

      // Check if the method is already declared.
      // Allow getter+setter pairs with the same name.
      auto &nameMap = method->_static ? staticNames : methodNames;
      auto &fieldVec = method->_static ? statics : methods;
      auto &idxMap = method->_static ? staticFieldIdx : methodFieldIdx;
      auto [it, inserted] =
          nameMap.try_emplace(name.getUnderlyingPointer(), method);
      if (!inserted) {
        // Try to merge getter+setter pair. Find the existing field.
        ClassType::Field *existingField =
            &fieldVec[idxMap[name.getUnderlyingPointer()]];
        // Statics share a single namespace, so the existing declaration may be
        // a field. Report the conflict before the accessor/overload merging
        // below, which assumes the existing declaration is a method.
        if (!existingField->isMethod()) {
          outer_.sm_.error(
              method->_key->getStartLoc(),
              "ft: method " + name.str() + " already declared");
          outer_.sm_.note(
              it->second->getSourceRange(),
              "ft: previous declaration of " + name.str());
          return outer_.flowContext_.getAny();
        }
        bool canMerge = (isGetter && existingField->hasSetter() &&
                         !existingField->hasGetter()) ||
            (isSetter && existingField->hasGetter() &&
             !existingField->hasSetter());
        if (!canMerge) {
          // Check if this is an overloaded method.
          bool existingIsOverload = existingField->isOverloaded() ||
              (existingField->method &&
               hermes::findDecorator(
                   existingField->method->_decorators,
                   {outer_.kw_.identHermes, outer_.kw_.identOverload}));

          if (overloadMethod && existingIsOverload) {
            // Both methods have @Hermes.overload — merge as overload.
            if (!existingField->isOverloaded()) {
              // First duplicate: move the existing method/type into
              // the overloads map.
              existingField->overloads.insert(
                  {existingField->method, existingField->type});
              existingField->type = nullptr;
              existingField->method = nullptr;
            }
            // For non-generic overloads, a Decl was already created
            // during final method processing before we reached the
            // duplicate name check.
            // For generic overloads, registerGenericMethod was already
            // called during generic method processing before the
            // duplicate name check.
            existingField->overloads.insert({method, methodType});
            return methodType;
          } else if (overloadMethod || existingIsOverload) {
            // Mismatch in overload expectation between two methods.
            assert(overloadMethod != existingIsOverload);
            outer_.sm_.error(
                method->_key->getStartLoc(),
                "ft: all overloads of " + name.str() +
                    " must be decorated with @Hermes.overload");
            return outer_.flowContext_.getAny();
          }

          outer_.sm_.error(
              method->_key->getStartLoc(),
              "ft: method " + name.str() + " already declared");
          outer_.sm_.note(
              it->second->getSourceRange(),
              "ft: previous declaration of " + name.str());
          return outer_.flowContext_.getAny();
        }

        // Merge the pair.
        if (isGetter) {
          // Adding getter to existing setter.
          Type *getterRetType = fieldType;
          // Validate getter return type matches setter param type.
          auto *setterFn =
              llvh::cast<TypedFunctionType>(existingField->setterType->info);
          Type *setterParamType = setterFn->getParams()[0].type;
          if (!getterRetType->info->equals(setterParamType->info)) {
            outer_.sm_.error(
                method->_key->getStartLoc(),
                "ft: getter return type must match setter parameter type");
            return outer_.flowContext_.getAny();
          }
          existingField->getterType = methodType;
          existingField->method = method;
          existingField->type = getterRetType;
        } else {
          // Adding setter to existing getter.
          auto *setterFn = llvh::cast<TypedFunctionType>(methodType->info);
          Type *setterParamType = setterFn->getParams()[0].type;
          // Validate setter param type matches getter return type.
          if (!existingField->type->info->equals(setterParamType->info)) {
            outer_.sm_.error(
                method->_key->getStartLoc(),
                "ft: setter parameter type must match getter return type");
            return outer_.flowContext_.getAny();
          }
          existingField->setterType = methodType;
          existingField->setterMethod = method;
        }
        // The pair is now both readable and writable, so the getter-only /
        // setter-only variance set at construction no longer applies.
        existingField->variance = FieldVariance::None;
        return methodType;
      }

      // For setters, the getter's method is null and setterMethod is set.
      // For getters/regular methods, method is set and setterMethod is null.
      ESTree::MethodDefinitionNode *getterMethod = isSetter ? nullptr : method;
      ESTree::MethodDefinitionNode *setMethod = isSetter ? method : nullptr;

      // Record the index for O(1) field lookup on duplicate name checks.
      idxMap[name.getUnderlyingPointer()] = fieldVec.size();

      if (method->_static) {
        // Static methods use layoutSlotIR = None because they will use
        // Variables, not object property slots.
        if (superMethod) {
          assert(privateNameNode == nullptr);
          statics.emplace_back(
              name,
              fieldType,
              superMethod->layoutSlotIR,
              /* isPrivate */ false,
              getterMethod,
              finalMethod,
              id,
              getterFnType,
              setterFnType,
              setMethod);
        } else {
          statics.emplace_back(
              name,
              fieldType,
              OptValue<size_t>(llvh::None),
              privateNameNode != nullptr,
              getterMethod,
              finalMethod,
              id,
              getterFnType,
              setterFnType,
              setMethod);
        }
      } else if (superMethod) {
        assert(privateNameNode == nullptr);
        methods.emplace_back(
            name,
            fieldType,
            superMethod->layoutSlotIR,
            /* isPrivate */ false,
            getterMethod,
            finalMethod,
            /* staticKeyNode */ nullptr,
            getterFnType,
            setterFnType,
            setMethod);
      } else {
        // Private methods are not stored in the public field map,
        // and final methods don't get a layout slot in the home object
        // because they are accessed through Variables, not the home object.
        OptValue<size_t> layoutSlotIR =
            (privateNameNode != nullptr || finalMethod)
            ? OptValue<size_t>(llvh::None)
            : OptValue<size_t>(nextMethodLayoutSlotIR++);
        methods.emplace_back(
            name,
            fieldType,
            layoutSlotIR,
            privateNameNode != nullptr,
            getterMethod,
            finalMethod,
            /* staticKeyNode */ nullptr,
            getterFnType,
            setterFnType,
            setMethod);
      }

      return methodType;
    }
  }
};

bool FlowChecker::parseClassType(
    ESTree::Node *superClass,
    ESTree::Node *superTypeArguments,
    ESTree::Node *body,
    Type *classType,
    sema::LexicalScope *classScope,
    Type *classConsType,
    ESTree::Node *classTypeParameters) {
  unsigned errorsBefore = sm_.getErrorCount();
  ParseClassType(
      *this,
      superClass,
      superTypeArguments,
      body,
      classType,
      classScope,
      classConsType,
      classTypeParameters);
  return sm_.getErrorCount() == errorsBefore;
}

void FlowChecker::visitClassNode(
    ESTree::ClassLikeNode *classNode,
    ESTree::ClassBodyNode *body,
    Type *classType) {
  assert(
      llvh::cast<ClassType>(classType->info)->isInitialized() &&
      "trying to typecheck uninitialized class");
  ClassContext classContext(*this, classType, classNode);
  visitESTreeChildren(*this, body);
}

void FlowChecker::visit(ESTree::ClassExpressionNode *node) {
  visitExpression(node->_superClass, node, nullptr);

  // Class expressions are lowered by the legacy IRGen path, which doesn't
  // create the Variables that typed statics, and private and final methods,
  // are stored in. Accessors must be final, so they are covered too. It also
  // doesn't support typed private fields.
  bool hasUnsupported = false;
  for (ESTree::Node &member :
       llvh::cast<ESTree::ClassBodyNode>(node->_body)->_body) {
    const char *what = nullptr;
    if (auto *prop = llvh::dyn_cast<ESTree::ClassPropertyNode>(&member)) {
      if (prop->_static)
        what = "static members";
    } else if (
        auto *prop =
            llvh::dyn_cast<ESTree::ClassPrivatePropertyNode>(&member)) {
      what = prop->_static ? "static members" : "private fields";
    } else if (
        auto *method = llvh::dyn_cast<ESTree::MethodDefinitionNode>(&member)) {
      if (method->_static)
        what = "static members";
      else if (llvh::isa<ESTree::PrivateNameNode>(method->_key))
        what = "private methods";
      else if (hermes::findDecorator(
                   method->_decorators, {kw_.identHermes, kw_.identFinal}))
        what = "final methods";
    }
    if (what) {
      sm_.error(
          member.getSourceRange(),
          llvh::Twine("ft: ") + what +
              " are not supported in class expressions");
      hasUnsupported = true;
    }
  }
  if (hasUnsupported)
    return;

  auto *id = llvh::cast_or_null<ESTree::IdentifierNode>(node->_id);
  Type *classType = flowContext_.createType(flowContext_.createClass(
      id ? Identifier::getFromPointer(id->_name) : Identifier{}));

  Type *consType =
      flowContext_.createType(flowContext_.createClassConstructor(classType));

  if (!parseClassType(
          node->_superClass,
          node->_superTypeArguments,
          node->_body,
          classType,
          node->getScope(),
          consType,
          node->_typeParameters)) {
    // Failed to parse class.
    return;
  }

  setNodeType(node, consType);

  // A new scope for the class expression name.
  ScopeRAII scope(*this);
  // If there was a class id T, in the new scope declare class type T and
  // declaration for class constructor T.
  if (id) {
    bindingTable_.try_emplace(
        id->_name, TypeDecl(classType, node->getScope(), node));

    sema::Decl *decl = getDecl(id);
    assert(decl && "class expression id must be resolved");
    assert(
        declTypes_.count(decl) == 0 &&
        "class expression id type already resolved");
    recordDecl(decl, consType, id, node);
  }

  visitClassNode(
      node, llvh::cast<ESTree::ClassBodyNode>(node->_body), classType);
}

void FlowChecker::visit(ESTree::ClassDeclarationNode *node) {
  sema::Decl *decl = getDecl(llvh::cast<ESTree::IdentifierNode>(node->_id));
  assert(decl && "class declaration must have been resolved");

  // If it is generic, don't typecheck because typechecking will be done
  // directly by draining the typecheck queue.
  if (node->_typeParameters) {
    return;
  }

  Type *classConsType = getDeclType(decl);
  auto *classType =
      llvh::cast<ClassConstructorType>(classConsType->info)->getClassType();

  visitClassNode(
      node, llvh::cast<ESTree::ClassBodyNode>(node->_body), classType);
}

void FlowChecker::visit(ESTree::MethodDefinitionNode *node) {
  auto *fe = llvh::cast<ESTree::FunctionExpressionNode>(node->_value);

  // Visit decorators to validate them.
  for (auto &decorator : node->_decorators) {
    visit(llvh::cast<ESTree::DecoratorNode>(&decorator), node);
  }

  // If it has type parameters, it's either a generic declaration
  // or a specialization which should be typechecked directly at clone time
  // by calling visitNonGenericFunctionDeclaration.
  // e.g.
  //   foo() { this.bar<number>() }
  //   bar<T>() {}
  // will create bar<number> in after bar<T>, but we don't want to visit after
  // visiting foo.
  if (fe->_typeParameters) {
    return;
  }

  if (node->_kind == kw_.identConstructor) {
    FunctionContext functionContext(
        *this,
        fe,
        curClassContext_->getClassTypeInfo()->getConstructorType(),
        curClassContext_->getThisType(/* isStatic */ false),
        curClassContext_->getNewTargetType(),
        curClassContext_->getSuperType(/* isStatic */ false));
    visitFunctionLike(fe, fe->_body, fe->_params);
  } else if (node->_key) {
    Identifier name;
    OptValue<ClassType::FieldLookupEntry> optField;
    if (auto *privateName =
            llvh::dyn_cast<ESTree::PrivateNameNode>(node->_key)) {
      name = astContext_.getPrivateNameIdentifier(
          llvh::cast<ESTree::IdentifierNode>(privateName->_id)->_name);
      if (node->_static) {
        if (auto *staticInfo = curClassContext_->getClassTypeInfo()
                                   ->getStaticObjectTypeInfo()) {
          optField = staticInfo->findPrivateField(name);
        }
      } else {
        optField = curClassContext_->getClassTypeInfo()
                       ->getHomeObjectTypeInfo()
                       ->findPrivateField(name);
      }
    } else if (!llvh::isa<ESTree::IdentifierNode>(node->_key)) {
      // Reported when the class was parsed.
      return;
    } else if (node->_static) {
      name = Identifier::getFromPointer(
          llvh::cast<ESTree::IdentifierNode>(node->_key)->_name);
      if (auto *staticInfo =
              curClassContext_->getClassTypeInfo()->getStaticObjectTypeInfo()) {
        optField = staticInfo->findPublicField(name);
      }
    } else {
      name = Identifier::getFromPointer(
          llvh::cast<ESTree::IdentifierNode>(node->_key)->_name);
      optField = curClassContext_->getClassTypeInfo()
                     ->getHomeObjectTypeInfo()
                     ->findPublicField(name);
    }
    // The member must have been populated on this class, not merely inherited.
    // A dropped member (due to a reported error) resolves to the superclass's
    // entry of the same name, whose type describes a different declaration.
    ClassType *owner = node->_static
        ? curClassContext_->getClassTypeInfo()->getStaticObjectTypeInfo()
        : curClassContext_->getClassTypeInfo()->getHomeObjectTypeInfo();
    if (!optField.hasValue() || optField->classType != owner) {
      assert(sm_.getErrorCount() && "member dropped without an error");
      return;
    }
    Type *funcType = optField->getField()->type;

    // For overloaded fields, find the type for this specific method.
    if (optField->getField()->isOverloaded()) {
      funcType = optField->getField()->overloads.lookup(node);
      assert(funcType && "overload type missing — overload not registered");
    }

    if (node->_kind == kw_.identGet || node->_kind == kw_.identSet) {
      // Accessors keep the property type in Field::type.
      // The function type of this specific accessor lives in
      // getterType/setterType.
      funcType = node->_kind == kw_.identGet ? optField->getField()->getterType
                                             : optField->getField()->setterType;
      if (!funcType)
        return;
    }

    // Typecheck overriding methods.
    // Private methods are not inherited, so they don't need to be checked.
    // Skip override checking for overloaded methods — each overload has a
    // different signature and override semantics are not well-defined.
    Type *superClassType =
        curClassContext_->getClassTypeInfo()->getSuperClass();
    if (!optField->getField()->isPrivate &&
        !optField->getField()->isOverloaded() &&
        !optField->getField()->isAccessor() && superClassType) {
      auto *superClassTypeInfo = llvh::cast<ClassType>(superClassType->info);
      OptValue<ClassType::FieldLookupEntry> superIt;
      if (node->_static) {
        if (auto *superStaticInfo =
                superClassTypeInfo->getStaticObjectTypeInfo()) {
          superIt = superStaticInfo->findPublicField(name);
        }
      } else {
        superIt =
            superClassTypeInfo->getHomeObjectTypeInfo()->findPublicField(name);
      }
      if (superIt) {
        auto *superMethod = superIt->getField();
        // An overloaded parent method has no single `type`; overriding it
        // is rejected earlier in phase 1 by the "cannot override final
        // method" check, so we should never reach phase 2 in that case.
        assert(
            !superMethod->isOverloaded() &&
            "overriding an overloaded parent method should have been "
            "rejected in phase 1");
        // Overriding method's function type must flow into the overridden
        // method's function type.
        bool canOverride = canAOverrideB(
            llvh::cast<BaseFunctionType>(funcType->info),
            llvh::cast<BaseFunctionType>(superMethod->type->info));
        if (!canOverride) {
          sm_.error(
              node->getStartLoc(), "ft: incompatible method type for override");
        }
      }
    }

    FunctionContext functionContext(
        *this,
        fe,
        funcType,
        curClassContext_->getThisType(node->_static),
        /* newTargetType */ flowContext_.getVoid(),
        curClassContext_->getSuperType(node->_static));
    functionContext.isSetter = node->_kind == kw_.identSet;

    // Static methods can't reference the class type parameters.
    ScopeRAII staticTypeParamScope(*this);
    if (node->_static)
      shadowClassTypeParamsInStatic();
    visitFunctionLike(fe, fe->_body, fe->_params);
  }
}

void FlowChecker::shadowClassTypeParamsInStatic() {
  ESTree::Node *typeParams = nullptr;
  if (auto *cd =
          llvh::dyn_cast<ESTree::ClassDeclarationNode>(curClassContext_->node))
    typeParams = cd->_typeParameters;
  else if (
      auto *ce =
          llvh::dyn_cast<ESTree::ClassExpressionNode>(curClassContext_->node))
    typeParams = ce->_typeParameters;
  shadowClassTypeParams(
      llvh::cast_or_null<ESTree::TypeParameterDeclarationNode>(typeParams),
      curClassContext_->node->getScope());
}

void FlowChecker::shadowClassTypeParams(
    ESTree::TypeParameterDeclarationNode *typeParams,
    sema::LexicalScope *scope) {
  if (!typeParams)
    return;
  Type *empty = flowContext_.getEmpty();
  for (auto &param : typeParams->_params) {
    auto *typeParam = llvh::cast<ESTree::TypeParameterNode>(&param);
    TypeDecl decl{empty, scope, &param};
    decl.staticTypeParam = true;
    bindingTable_.try_emplace(typeParam->_name, decl);
  }
}

void FlowChecker::visit(ESTree::DecoratorNode *node, ESTree::Node *parent) {
  // Check for the allowed @Hermes decorators.
  auto *mem = llvh::dyn_cast<ESTree::MemberExpressionNode>(node->_expression);
  if (!mem) {
    sm_.error(
        node->getSourceRange(),
        "ft: unknown decorator: must be a @Hermes decorator");
    return;
  }

  auto *objID = llvh::dyn_cast<ESTree::IdentifierNode>(mem->_object);
  auto *propID = llvh::dyn_cast<ESTree::IdentifierNode>(mem->_property);
  if (!objID || !propID) {
    sm_.error(
        node->getSourceRange(),
        "ft: unknown decorator: must be a @Hermes decorator");
    return;
  }

  if (objID->_name != kw_.identHermes) {
    sm_.error(
        node->getSourceRange(),
        "ft: unknown decorator: must be a @Hermes decorator");
    return;
  }

  if (propID->_name == kw_.identFinal) {
    if (!llvh::isa<ESTree::MethodDefinitionNode>(parent)) {
      sm_.error(node->getSourceRange(), "ft: invalid @Hermes.final");
      return;
    }
  } else if (propID->_name == kw_.identOverload) {
    if (!llvh::isa<ESTree::MethodDefinitionNode>(parent)) {
      sm_.error(
          node->getSourceRange(),
          "ft: @Hermes.overload is only valid on methods");
      return;
    }
  } else if (propID->_name == kw_.identBuiltin) {
    if (!llvh::isa<ESTree::FunctionDeclarationNode>(parent)) {
      sm_.error(
          node->getSourceRange(),
          "ft: @Hermes.builtin is only valid on function declarations");
      return;
    }
  } else {
    sm_.error(node->getSourceRange(), "ft: unknown @Hermes decorator");
  }
}

void FlowChecker::visit(ESTree::TypeAnnotationNode *node) {
  // Do nothing.
}
void FlowChecker::visit(ESTree::IdentifierNode *identifierNode) {
  // Do nothing.
}

void FlowChecker::visit(ESTree::ExpressionStatementNode *node) {
  visitExpression(node->_expression, node, nullptr);
}

void FlowChecker::visit(ESTree::IfStatementNode *node) {
  visitExpression(node->_test, node, nullptr);
  visitESTreeNode(*this, node->_consequent, node);
  visitESTreeNode(*this, node->_alternate, node);
}
void FlowChecker::visit(ESTree::SwitchStatementNode *node) {
  ScopeRAII scope(*this);
  if (!resolveScopeTypesAndAnnotate(node, node->getScope()))
    return;
  visitExpression(node->_discriminant, node, nullptr);
  visitESTreeNodeList(*this, node->_cases, node);
}
void FlowChecker::visit(ESTree::SwitchCaseNode *node) {
  visitExpression(node->_test, node, nullptr);
  visitESTreeNodeList(*this, node->_consequent, node);
}
void FlowChecker::visit(ESTree::WhileStatementNode *node) {
  visitExpression(node->_test, node, nullptr);
  visitESTreeNode(*this, node->_body, node);
}
void FlowChecker::visit(ESTree::DoWhileStatementNode *node) {
  visitESTreeNode(*this, node->_body, node);
  visitExpression(node->_test, node, nullptr);
}
Type *FlowChecker::forOfElementType(ESTree::ForOfStatementNode *node) {
  // Only a typed array produces a statically known element. for-await iterates
  // the async iterator protocol, which yields 'any' whatever the source is.
  Type *rightType = getNodeTypeOrAny(node->_right);
  if (node->_await || !flowContext_.isArrayClassType(rightType))
    return nullptr;
  return flowContext_.getArrayElementType(rightType);
}

void FlowChecker::visitLoopTarget(
    ESTree::Node *left,
    ESTree::Node *parent,
    Type *srcType,
    const char *kind) {
  auto *declaration = llvh::dyn_cast<ESTree::VariableDeclarationNode>(left);
  if (declaration) {
    visitESTreeNode(*this, left, parent);
  } else {
    // Pass the source type as the constraint so a destructuring target is
    // typed against what it actually receives instead of as a bare tuple.
    visitAssignmentTarget(left, parent, srcType);
  }

  // Without a statically known source type the loop produces `any`. That
  // still matters for an annotated pattern, which must fit exactly below.
  if (!srcType)
    srcType = flowContext_.getAny();

  ESTree::Node *targetNode = left;
  Type *lt;
  if (declaration) {
    if (declaration->_declarations.empty())
      return;
    targetNode = llvh::cast<ESTree::VariableDeclaratorNode>(
                     &declaration->_declarations.front())
                     ->_id;
    // An annotated loop variable is typed from its annotation instead of from
    // what the loop feeds it, so it needs the same check as the expression
    // form. Without an annotation the two are the same type and this is a
    // no-op.
    lt = llvh::isa<ESTree::IdentifierNode>(targetNode)
        ? flowContext_.findDeclType(
              getDecl(llvh::cast<ESTree::IdentifierNode>(targetNode)))
        : flowContext_.findNodeType(targetNode);
  } else {
    // A pattern is checked leaf by leaf by its own visitor; its own type is
    // either srcType (redundant) or an invariant aggregate of its targets,
    // which would compare falsely.
    if (llvh::isa<ESTree::PatternNode>(left))
      return;
    lt = getNodeTypeOrAny(left);
  }
  if (!lt)
    return;
  bool canFlow;
  if (llvh::isa<ESTree::IdentifierNode>(targetNode)) {
    // A single variable is stored into, so LReference::emitStore narrows.
    canFlow = tryNarrowType(srcType, lt).second.canFlow;
  } else {
    // An annotated pattern is destructured directly out of the loop value, so
    // there is no store to hang a narrowing cast on. Require an exact fit: a
    // cast to a tuple or object type is not actually checked, so allowing one
    // here would destructure a value that may not have the assumed shape.
    CanFlowResult cf = canAFlowIntoB(srcType, lt);
    canFlow = cf.canFlow && !cf.needCheckedCast;
  }
  if (canFlow)
    return;
  sm_.error(
      targetNode->getSourceRange(),
      llvh::Twine("ft: incompatible ") + kind +
          " type: cannot implicitly cast from " + srcType->messageString() +
          " to " + lt->messageString());
}

void FlowChecker::visit(ESTree::ForOfStatementNode *node) {
  if (!resolveScopeTypesAndAnnotate(node, node->getScope()))
    return;
  // A declaration left-hand side takes its type from the iterable, so
  // annotation has to visit the iterable to record the declaration; it marks
  // the iterable in visitedInits_ and the mark is consumed here so it is not
  // visited twice. That puts the iterable in the same position as a
  // `let x = init` initializer, with the same caveat: it is visited before the
  // loop variable is declared. Any other left-hand side needs no annotation,
  // so the iterable is visited here instead.
  if (!visitedInits_.erase(node->_right))
    visitExpression(node->_right, node, nullptr);
  visitLoopTarget(node->_left, node, forOfElementType(node), "for-of element");
  visitESTreeNode(*this, node->_body, node);
}
void FlowChecker::visit(ESTree::ForInStatementNode *node) {
  if (!resolveScopeTypesAndAnnotate(node, node->getScope()))
    return;
  // Visit the iterable before the left-hand side, matching the evaluation
  // order. The key type is the constant 'string', so unlike for-of, annotating
  // the loop variable doesn't depend on the iterable's type.
  visitExpression(node->_right, node, nullptr);
  // for-in always produces a string key, so a target that can't hold one is
  // statically wrong.
  visitLoopTarget(node->_left, node, flowContext_.getString(), "for-in target");
  visitESTreeNode(*this, node->_body, node);
}
void FlowChecker::visit(ESTree::ForStatementNode *node) {
  if (!resolveScopeTypesAndAnnotate(node, node->getScope()))
    return;
  if (node->_init) {
    if (llvh::isa<ESTree::VariableDeclarationNode>(node->_init))
      visitESTreeNode(*this, node->_init, node);
    else
      visitExpression(node->_init, node, nullptr);
  }
  visitExpression(node->_test, node, nullptr);
  visitExpression(node->_update, node, nullptr);
  visitESTreeNode(*this, node->_body, node);
}

void FlowChecker::visit(ESTree::ReturnStatementNode *node) {
  // The setter's return type is void, so an `any` value would otherwise be
  // accepted and fail its checked cast at runtime.
  if (curFunctionContext_->isSetter && node->_argument) {
    visitExpression(node->_argument, node, nullptr);
    sm_.error(node->getSourceRange(), "ft: setter cannot return a value");
    return;
  }

  auto *ftype = llvh::dyn_cast<TypedFunctionType>(
      curFunctionContext_->functionType->info);
  visitExpression(
      node->_argument, node, ftype ? ftype->getReturnType() : nullptr);

  // Untyped function, can't check the return type.
  if (!ftype) {
    return;
  }

  // Return without an argument and "void" return type is OK.
  if (!node->_argument && llvh::isa<VoidType>(ftype->getReturnType()->info))
    return;

  Type *argType = node->_argument ? getNodeTypeOrAny(node->_argument)
                                  : flowContext_.getVoid();

  auto [retTypeNarrow, cf] = tryNarrowType(argType, ftype->getReturnType());
  if (!cf.canFlow) {
    sm_.error(
        node->getSourceRange(),
        llvh::Twine(
            "ft: return value incompatible with return type: cannot return ") +
            argType->messageString() + " as " +
            ftype->getReturnType()->messageString());
  }
  node->_argument = implicitCheckedCast(node->_argument, retTypeNarrow, cf);
}

void FlowChecker::visit(ESTree::ThrowStatementNode *node) {
  visitExpression(node->_argument, node, nullptr);
}

void FlowChecker::visit(ESTree::BlockStatementNode *node) {
  ScopeRAII scope(*this);
  if (!resolveScopeTypesAndAnnotate(node, node->getScope()))
    return;
  visitESTreeChildren(*this, node);
}

void FlowChecker::visit(ESTree::VariableDeclarationNode *node) {
  for (ESTree::Node &n : node->_declarations) {
    auto *declarator = llvh::cast<ESTree::VariableDeclaratorNode>(&n);
    // Avoid visiting the initializer if it was already visited while inferring
    // the type of the declaration. Remove the entry from the set.
    auto it = visitedInits_.find(declarator->_init);
    if (it != visitedInits_.end()) {
      visitedInits_.erase(it);
    } else {
      Type *constraint = nullptr;
      if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(declarator->_id)) {
        sema::Decl *decl = getDecl(id);
        constraint = flowContext_.findDeclType(decl);
      } else {
        // For destructuring, the type annotation was set on the pattern node
        // during annotation.
        constraint = flowContext_.findNodeType(declarator->_id);
      }
      visitExpression(declarator->_init, declarator, constraint);
    }
    auto defaultsIt = declPatternDefaults_.find(declarator->_id);
    if (defaultsIt != declPatternDefaults_.end()) {
      auto defaults = std::move(defaultsIt->second);
      declPatternDefaults_.erase(defaultsIt);
      checkDestructuringDefaults(defaults);
    }
    // A loop head declarator has no initializer, and implicitCheckedCast
    // would dereference the null node to copy its location.
    if (!declarator->_init)
      continue;

    if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(declarator->_id)) {
      sema::Decl *decl = getDecl(id);
      Type *lt = getDeclType(decl);
      Type *rt = getNodeTypeOrAny(declarator->_init);
      auto [rtNarrow, cf] = tryNarrowType(rt, lt);
      if (!cf.canFlow) {
        sm_.error(
            declarator->getSourceRange(),
            "ft: incompatible initialization type: cannot assign " +
                rt->messageString() + " to " + lt->messageString());
      } else {
        declarator->_init =
            implicitCheckedCast(declarator->_init, rtNarrow, cf);
      }
    } else {
      Type *lt = getNodeTypeOrAny(declarator->_id);
      Type *rt = getNodeTypeOrAny(declarator->_init);

      CanFlowResult cf = canAFlowIntoB(rt, lt);
      if (!cf.canFlow) {
        sm_.error(
            declarator->getSourceRange(),
            "ft: incompatible initialization type: cannot assign " +
                rt->messageString() + " to " + lt->messageString());
      } else {
        declarator->_init = implicitCheckedCast(declarator->_init, lt, cf);
      }
    }
  }
}

void FlowChecker::visitClassProperty(
    ESTree::Node *node,
    ESTree::Node *&value,
    bool isStatic) {
  if (value) {
    auto it = visitedInits_.find(value);
    if (it != visitedInits_.end()) {
      // Already visited during inference.
      visitedInits_.erase(it);
    } else {
      Type *fieldInitFuncType =
          curClassContext_->getOrCreateFieldInitFunctionType(isStatic);
      FunctionContext fieldInitFuncContext{
          *this,
          /* declCollectorNode */ nullptr,
          fieldInitFuncType,
          curClassContext_->getThisType(isStatic),
          /* newTargetType */ flowContext_.getVoid(),
          curClassContext_->getSuperType(isStatic)};
      ScopeRAII staticTypeParamScope(*this);
      if (isStatic)
        shadowClassTypeParamsInStatic();
      visitExpression(value, node, nullptr);
    }

    Type *lt = getNodeTypeOrAny(node);
    Type *rt = getNodeTypeOrAny(value);
    CanFlowResult cf = canAFlowIntoB(rt, lt);
    if (!cf.canFlow) {
      sm_.error(node->getSourceRange(), "ft: incompatible initialization type");
      return;
    }
    value = implicitCheckedCast(value, lt, cf);
  }
}

void FlowChecker::visit(ESTree::ClassPropertyNode *node) {
  visitClassProperty(node, node->_value, node->_static);
}

void FlowChecker::visit(ESTree::ClassPrivatePropertyNode *node) {
  visitClassProperty(node, node->_value, node->_static);
}

void FlowChecker::visit(ESTree::CatchClauseNode *node) {
  ScopeRAII scope(*this);
  if (!resolveScopeTypesAndAnnotate(node, node->getScope()))
    return;
  visitESTreeChildren(*this, node);
}

void FlowChecker::visitFunctionLike(
    ESTree::FunctionLikeNode *node,
    ESTree::Node *body,
    ESTree::NodeList &params) {
  assert(node->getSemInfo()->strict && "Types can only be used in strict mode");
  ScopeRAII scope(*this);

  size_t i = 0;
  for (auto &param : params) {
    if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(&param)) {
      sema::Decl *decl = getDecl(id);
      assert(decl && "unresolved parameter");
      Type *paramType;
      auto *typedFn = llvh::dyn_cast<TypedFunctionType>(
          curFunctionContext_->functionType->info);
      if (id->_name == kw_.identThis) {
        // 'this' is stored separately, not in getParams().
        paramType = typedFn ? typedFn->getThisParam() : flowContext_.getAny();
        if (!paramType)
          paramType = flowContext_.getAny();
      } else if (typedFn && i < typedFn->getParams().size()) {
        paramType = typedFn->getParams()[i].type;
        // Optional params can receive undefined when omitted.
        if (typedFn->getParams()[i].optional) {
          Type *voidType = flowContext_.getVoid();
          paramType = flowContext_.createType(
              flowContext_.maybeCreateUnion({paramType, voidType}));
        }
        ++i;
      } else {
        paramType = flowContext_.getAny();
        ++i;
      }
      declTypes_.try_emplace(decl, paramType);
    } else if (
        auto *assign = llvh::dyn_cast<ESTree::AssignmentPatternNode>(&param)) {
      Type *paramType;
      auto *typedFn = llvh::dyn_cast<TypedFunctionType>(
          curFunctionContext_->functionType->info);
      if (typedFn && i < typedFn->getParams().size()) {
        paramType = typedFn->getParams()[i].type;
      } else {
        paramType = flowContext_.getAny();
      }
      ++i;

      if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(assign->_left)) {
        sema::Decl *decl = getDecl(id);
        assert(decl && "unresolved parameter");
        declTypes_.try_emplace(decl, paramType);

        // Typecheck the default value against the parameter type.
        visitExpression(assign->_right, assign, paramType);
        Type *initType = getNodeTypeOrAny(assign->_right);
        CanFlowResult cf = canAFlowIntoB(initType, paramType);
        if (!cf.canFlow) {
          sm_.error(
              assign->_right->getSourceRange(),
              "ft: incompatible default argument type");
        } else if (cf.needCheckedCast && compile_) {
          assign->_right = implicitCheckedCast(assign->_right, paramType, cf);
        }
      } else if (
          llvh::isa<ESTree::ObjectPatternNode>(assign->_left) ||
          llvh::isa<ESTree::ArrayPatternNode>(assign->_left)) {
        // Destructuring param with default value.
        assignDestructuringParamTypes(assign->_left, paramType);

        // Typecheck the default value against the parameter type.
        visitExpression(assign->_right, assign, paramType);
        Type *initType = getNodeTypeOrAny(assign->_right);
        CanFlowResult cf = canAFlowIntoB(initType, paramType);
        if (!cf.canFlow) {
          sm_.error(
              assign->_right->getSourceRange(),
              "ft: incompatible default argument type");
        } else if (cf.needCheckedCast && compile_) {
          assign->_right = implicitCheckedCast(assign->_right, paramType, cf);
        }
      }
    } else if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&param)) {
      // Rest parameter: assign the type from the function type params.
      auto *typedFn = llvh::dyn_cast<TypedFunctionType>(
          curFunctionContext_->functionType->info);
      Type *paramType;
      if (typedFn && i < typedFn->getParams().size()) {
        paramType = typedFn->getParams()[i].type;
      } else {
        paramType = flowContext_.getAny();
      }
      ++i;

      if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(rest->_argument)) {
        sema::Decl *decl = getDecl(id);
        assert(decl && "unresolved parameter");
        declTypes_.try_emplace(decl, paramType);
      }
    } else if (
        llvh::isa<ESTree::ObjectPatternNode>(&param) ||
        llvh::isa<ESTree::ArrayPatternNode>(&param)) {
      // Destructuring param without default value.
      Type *paramType;
      auto *typedFn = llvh::dyn_cast<TypedFunctionType>(
          curFunctionContext_->functionType->info);
      if (typedFn && i < typedFn->getParams().size()) {
        paramType = typedFn->getParams()[i].type;
      } else {
        paramType = flowContext_.getAny();
      }
      ++i;

      assignDestructuringParamTypes(&param, paramType);
    }
  }

  if (!resolveScopeTypesAndAnnotate(
          node, node->getSemInfo()->getFunctionBodyScope()))
    return;
  visitESTreeNode(*this, body, node);
  checkImplicitReturnType(node);
}

void FlowChecker::checkImplicitReturnType(ESTree::FunctionLikeNode *node) {
  // Function body isn't known to terminate.
  // Typecheck the implicit 'return;' at the end of the block.
  auto *ftype = llvh::dyn_cast<TypedFunctionType>(
      curFunctionContext_->functionType->info);

  // Untyped function, can't check the return type.
  if (!ftype) {
    return;
  }

  // No implicit return, no need to check anything.
  if (!node->getSemInfo()->mayReachImplicitReturn) {
    return;
  }

  // Check that the implicit "undefined" (void type) can flow into the return
  // type.
  CanFlowResult cf =
      canAFlowIntoB(flowContext_.getVoid(), ftype->getReturnType());
  if (!cf.canFlow) {
    // It's possible there's no return annotation here (we may have inferred
    // it).
    if (auto *retAnnotation = ESTree::getReturnType(node)) {
      sm_.error(
          llvh::cast<ESTree::TypeAnnotationNode>(retAnnotation)
              ->_typeAnnotation->getSourceRange(),
          "ft: implicitly-returned 'undefined' incompatible with return type: " +
              ftype->getReturnType()->messageString());
    } else {
      sm_.error(
          node->getStartLoc(),
          "ft: implicitly-returned 'undefined' incompatible with return type: " +
              ftype->getReturnType()->messageString());
    }
  }
}

template <typename RecordLeafCB>
void FlowChecker::resolveDestructuringTypes(
    ESTree::Node *pattern,
    Type *patternType,
    RecordLeafCB recordLeaf,
    llvh::SmallVectorImpl<DestructuringDefault> &defaults) {
  // Use a worklist to avoid recursion.
  llvh::SmallVector<std::pair<ESTree::Node *, Type *>, 4> worklist{};
  worklist.emplace_back(pattern, patternType);

  /// Lambda passed as the callback to the expand* helpers below.
  auto addToWorklist = [&worklist](ESTree::Node *n, Type *ty) {
    worklist.emplace_back(n, ty);
  };

  while (!worklist.empty()) {
    auto [node, t] = worklist.pop_back_val();
    if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(node)) {
      // A leaf binding identifier: record its type via the caller's callback.
      if (id->_typeAnnotation) {
        setNodeType(id, flowContext_.getAny());
        sm_.error(
            id->getSourceRange(),
            "ft: type annotations not supported inside destructuring, "
            "annotate the whole pattern instead");
        continue;
      }
      sema::Decl *decl = getDecl(id);
      setNodeType(id, t);
      recordLeaf(decl, t, id);
    } else if (auto *arr = llvh::dyn_cast<ESTree::ArrayPatternNode>(node)) {
      if (auto *tuple = llvh::dyn_cast<TupleType>(t->info)) {
        // Setting the type to the tuple allows IRGen to conveniently query the
        // kind of destructuring to run by checking the annotated type on the
        // array pattern. It also avoids rerunning annotation in subsequent
        // phases of AnnotateScopeDecls.
        setNodeType(arr, t);
        if (!expandTupleDestructuring(arr, tuple, addToWorklist))
          return;
      } else if (flowContext_.isArrayClassType(t)) {
        // Array<T> destructuring: each element gets type T, and a trailing
        // rest element gets type Array<T>. Setting the node type lets IRGen
        // dispatch to the FastArray-specific destructuring path.
        setNodeType(arr, t);
        if (!expandArrayDestructuring(arr, t, addToWorklist))
          return;
      } else if (llvh::isa<AnyType>(t->info)) {
        // Propagate the 'any' type to all children. The iterator protocol will
        // be used to populate the elements during IRGen.
        setNodeType(arr, flowContext_.getAny());
        for (ESTree::Node &element : arr->_elements)
          worklist.emplace_back(&element, t);
      } else {
        setNodeType(arr, flowContext_.getAny());
        sm_.error(
            arr->getSourceRange(),
            "ft: incompatible type for array pattern, expected tuple or array");
      }
    } else if (auto *obj = llvh::dyn_cast<ESTree::ObjectPatternNode>(node)) {
      if (auto *objType = llvh::dyn_cast<ExactObjectType>(t->info)) {
        // Setting the type allows IRGen to query the destructuring kind.
        setNodeType(obj, t);
        if (!expandObjectDestructuring(obj, objType, addToWorklist))
          return;
      } else if (llvh::isa<AnyType>(t->info)) {
        // Propagate 'any' to all properties and the rest binding.
        setNodeType(obj, flowContext_.getAny());
        for (ESTree::Node &propNode : obj->_properties) {
          if (auto *prop = llvh::dyn_cast<ESTree::PropertyNode>(&propNode)) {
            worklist.emplace_back(prop->_value, t);
          } else if (
              auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&propNode)) {
            worklist.emplace_back(rest->_argument, t);
          } else {
            sm_.error(propNode.getSourceRange(), "ft: property not supported");
          }
        }
      } else {
        setNodeType(obj, flowContext_.getAny());
        sm_.error(
            obj->getSourceRange(),
            "ft: incompatible type for object pattern, "
            "expected object type");
      }
    } else if (
        auto *assign = llvh::dyn_cast<ESTree::AssignmentPatternNode>(node)) {
      // A defaulted binding `x = default` inside a destructuring pattern.
      // The caller checks it once every binding is recorded, so a default may
      // reference an earlier binding in the same pattern, e.g.
      // `let {a, b = a} = o`.
      defaults.emplace_back(assign, t);
      worklist.emplace_back(assign->_left, t);
    }
  }
}

void FlowChecker::checkDestructuringDefaults(
    llvh::ArrayRef<DestructuringDefault> defaults) {
  for (auto [assign, t] : defaults) {
    visitExpression(assign->_right, assign, t);
    Type *initType = getNodeTypeOrAny(assign->_right);
    CanFlowResult cf = canAFlowIntoB(initType, t);
    if (!cf.canFlow) {
      sm_.error(
          assign->_right->getSourceRange(),
          "ft: incompatible default value type");
    } else {
      assign->_right = implicitCheckedCast(assign->_right, t, cf);
    }
  }
}

void FlowChecker::assignDestructuringParamTypes(
    ESTree::Node *pattern,
    Type *paramType) {
  // Param defaults can't see body declarations, so check them right away.
  llvh::SmallVector<DestructuringDefault, 2> defaults{};
  resolveDestructuringTypes(
      pattern,
      paramType,
      [this](sema::Decl *decl, Type *t, ESTree::IdentifierNode *) {
        declTypes_.try_emplace(decl, t);
      },
      defaults);
  checkDestructuringDefaults(defaults);
}

/// AnnotateScopeDecls needs to not report errors when visiting a closure
/// to infer a variable declaration's type:
/// \code
///   let func = () => { let a = b; }
///   let b = 10;
/// \endcode
///
/// So we do the following:
/// 1. Iterate variable declarations with annotations/literals to record types,
///   but do not visit their initializers.
/// 2. Iterate all declarations and visit variable initializers to do inference,
///   descending into functions along the way.
///
/// While visiting an identifier in step 2, if the variable has no type yet,
/// it means that it has no type annotation AND its type hasn't been inferred.
/// Assume "any", because:
/// * If it's 'var': then it's untyped and may be init'ed later, assume "any".
/// * If it's 'let':
///   * If we're in the same function, it's declared late, assume "any",
///     and IRGen will report a TDZ error early when possible.
///   * If we're in a different function the variable could be initialized in
///     the other function, assume "any".
class FlowChecker::AnnotateScopeDecls {
  FlowChecker &outer;

 public:
  AnnotateScopeDecls(
      FlowChecker &outer,
      const sema::ScopeDecls &decls,
      ESTree::Node *scopeNode)
      : outer(outer) {
    setTypesForAnnotatedVariables(decls);
    annotateAllScopeDecls(decls, scopeNode);
  }

 private:
  /// Step 1 above.
  /// Iterate all variable declarations with type annotations or literal
  /// initializers and register them.
  void setTypesForAnnotatedVariables(const sema::ScopeDecls &decls) {
    for (ESTree::Node *declNode : decls) {
      if (auto *declaration =
              llvh::dyn_cast<ESTree::VariableDeclarationNode>(declNode)) {
        for (ESTree::Node &n : declaration->_declarations) {
          auto *declarator = llvh::cast<ESTree::VariableDeclaratorNode>(&n);
          if (auto *id =
                  llvh::dyn_cast<ESTree::IdentifierNode>(declarator->_id)) {
            sema::Decl *decl = outer.getDecl(id);
            // Global properties don't have sound types, let the
            // annotateVariableDeclaration call handle it later.
            if (sema::Decl::isKindGlobal(decl->kind)) {
              continue;
            }
            if (id->_typeAnnotation) {
              // Found a type annotation on a local variable declaration.
              Type *type = outer.parseTypeAnnotation(
                  llvh::cast<ESTree::TypeAnnotationNode>(id->_typeAnnotation)
                      ->_typeAnnotation);
              outer.recordDecl(decl, type, id, declarator);
              continue;
            }
            // Check for literal initializers.
            if (declarator->_init &&
                (llvh::isa<ESTree::NullLiteralNode>(declarator->_init) ||
                 llvh::isa<ESTree::NumericLiteralNode>(declarator->_init) ||
                 llvh::isa<ESTree::BooleanLiteralNode>(declarator->_init) ||
                 llvh::isa<ESTree::StringLiteralNode>(declarator->_init) ||
                 llvh::isa<ESTree::RegExpLiteralNode>(declarator->_init) ||
                 llvh::isa<ESTree::BigIntLiteralNode>(declarator->_init))) {
              outer.visitExpression(declarator->_init, declarator, nullptr);
              Type *initType = outer.getNodeTypeOrAny(declarator->_init);
              // let/var widen a fresh literal type to its base type; const
              // keeps the literal type.
              if (decl->kind != sema::Decl::Kind::Const)
                initType = outer.widenLiteralType(initType);
              outer.recordDecl(decl, initType, id, declarator);
              continue;
            }
          } else if (
              auto *arr =
                  llvh::dyn_cast<ESTree::ArrayPatternNode>(declarator->_id)) {
            if (arr->_typeAnnotation) {
              // Found a type annotation on a local variable declaration that
              // destructures into an array pattern.
              Type *type = outer.parseTypeAnnotation(
                  llvh::cast<ESTree::TypeAnnotationNode>(arr->_typeAnnotation)
                      ->_typeAnnotation);
              annotateDestructuringTarget(declarator, arr, type);
              continue;
            }
          } else if (
              auto *obj =
                  llvh::dyn_cast<ESTree::ObjectPatternNode>(declarator->_id)) {
            if (obj->_typeAnnotation) {
              // Found a type annotation on a local variable declaration that
              // destructures into an object pattern.
              Type *type = outer.parseTypeAnnotation(
                  llvh::cast<ESTree::TypeAnnotationNode>(obj->_typeAnnotation)
                      ->_typeAnnotation);
              annotateDestructuringTarget(declarator, obj, type);
              continue;
            }
          }
        }
      }
    }
  }

  /// Step 2 above.
  /// Visit all declarations and attempt to infer variable types.
  void annotateAllScopeDecls(
      const sema::ScopeDecls &decls,
      ESTree::Node *scopeNode) {
    // Annotate function declaration signatures first, mirroring JS function
    // hoisting, so their types are available to variable initializers that
    // reference them regardless of textual order.
    for (ESTree::Node *declNode : decls) {
      auto *funcDecl =
          llvh::dyn_cast<ESTree::FunctionDeclarationNode>(declNode);
      if (!funcDecl)
        continue;
      ESTree::Node *parent = nullptr;
      if (auto *program = llvh::dyn_cast<ESTree::ProgramNode>(scopeNode)) {
        parent = program;
      } else if (
          auto *func = llvh::dyn_cast<ESTree::FunctionLikeNode>(scopeNode)) {
        parent = ESTree::getBlockStatement(func);
      } else {
        parent = scopeNode;
        assert(
            llvh::isa<ESTree::BlockStatementNode>(parent) ||
            llvh::isa<ESTree::SwitchStatementNode>(parent));
      }
      annotateFunctionDeclaration(funcDecl, parent);
    }

    for (ESTree::Node *declNode : decls) {
      if (auto *declaration =
              llvh::dyn_cast<ESTree::VariableDeclarationNode>(declNode)) {
        // VariableDeclaration.
        //
        annotateVariableDeclaration(declaration, scopeNode);
      } else if (llvh::isa<ESTree::FunctionDeclarationNode>(declNode)) {
        // FunctionDeclaration: already annotated in the hoisting pass above.
        //
        continue;
      } else if (
          auto *id = llvh::dyn_cast<ESTree::ImportDeclarationNode>(declNode)) {
        // ImportDeclaration.
        //
        outer.sm_.error(id->getStartLoc(), "ft: import not implemented yet");
      } else if (
          auto *catchClause =
              llvh::dyn_cast<ESTree::CatchClauseNode>(declNode)) {
        // CatchClause.
        //
        annotateCatchClause(catchClause);
      } else {
        // All the rest.
        //
        continue;
      }
    }
  }

  void annotateVariableDeclaration(
      ESTree::VariableDeclarationNode *declaration,
      ESTree::Node *scopeNode) {
    // A for-in/of loop variable has no initializer; it takes its type from what
    // the loop feeds it. For for-of that is the element type of the iterable,
    // which is inferred here so that it is visited in the same position as a
    // `let x = init` initializer; for for-in it is always a string key.
    //
    // A `var` loop variable is hoisted out of the loop scope by DeclCollector,
    // so scopeNode is the enclosing function and it keeps 'any'.
    Type *noInitType = outer.flowContext_.getAny();
    if (auto *forOf = llvh::dyn_cast<ESTree::ForOfStatementNode>(scopeNode)) {
      // visit(ForOfStatementNode *) relies on visitedInits_ to not visit the
      // iterable a second time.
      outer.visitExpression(forOf->_right, forOf, nullptr);
      outer.visitedInits_.insert(forOf->_right);
      if (Type *elemType = outer.forOfElementType(forOf))
        noInitType = elemType;
    } else if (llvh::isa<ESTree::ForInStatementNode>(scopeNode)) {
      noInitType = outer.flowContext_.getString();
    }

    /// Attempt to infer the RHS of the declarator by calling the typecheck
    /// visitor on it to see if it's able to associate a type with the init
    /// node.
    /// \return the inferred type, or nullptr if no inference was possible.
    auto tryInferInitExpression =
        [this](ESTree::VariableDeclaratorNode *declarator) -> Type * {
      outer.visitExpression(declarator->_init, declarator, nullptr);
      outer.visitedInits_.insert(declarator->_init);
      if (Type *inferred = outer.flowContext_.findNodeType(declarator->_init)) {
        return inferred;
      }
      return nullptr;
    };

    for (ESTree::Node &n : declaration->_declarations) {
      auto *declarator = llvh::cast<ESTree::VariableDeclaratorNode>(&n);
      if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(declarator->_id)) {
        sema::Decl *decl = outer.getDecl(id);
        // Already handled in setTypesForAnnotatedVariables, unless it was a
        // hoisted function, which recordDecl below reports as redeclared.
        if (outer.flowContext_.findDeclType(decl) &&
            !llvh::dyn_cast_or_null<ESTree::FunctionDeclarationNode>(
                outer.declNodes_.lookup(decl))) {
          continue;
        }
        Type *type = outer.parseOptionalTypeAnnotation(id->_typeAnnotation);

        // Global properties don't have sound types, since they can be
        // overwritten without our knowledge and control.
        if (decl->kind == sema::Decl::Kind::GlobalProperty) {
          type = outer.flowContext_.getAny();
          if (id->_typeAnnotation) {
            outer.sm_.warning(
                id->_typeAnnotation->getSourceRange(),
                "ft: global property type annotations are unsound and are ignored");
          }
        } else if (!id->_typeAnnotation && declarator->_init) {
          if (Type *inferred = tryInferInitExpression(declarator)) {
            // let/var widen a fresh literal type to its base type; const keeps
            // the literal type.
            type = decl->kind != sema::Decl::Kind::Const
                ? outer.widenLiteralType(inferred)
                : inferred;
          }
        } else if (!id->_typeAnnotation) {
          type = noInitType;
        }

        outer.recordDecl(decl, type, id, declarator);
      } else if (
          auto *arr =
              llvh::dyn_cast<ESTree::ArrayPatternNode>(declarator->_id)) {
        if (outer.flowContext_.findNodeType(arr)) {
          // This array pattern was already handled in
          // setTypesForAnnotatedVariables.
          continue;
        }
        // It's possible to not have an _init if this declarator is part of a
        // for loop:
        // for ([x, y] of iterable) {}
        Type *type = noInitType;
        if (declarator->_init) {
          if (Type *inferred = tryInferInitExpression(declarator))
            type = inferred;
        }
        annotateDestructuringTarget(declarator, arr, type);
      } else if (
          auto *obj =
              llvh::dyn_cast<ESTree::ObjectPatternNode>(declarator->_id)) {
        if (outer.flowContext_.findNodeType(obj)) {
          // This object pattern was already handled in
          // setTypesForAnnotatedVariables.
          continue;
        }
        // It's possible to not have an _init if this declarator is part of a
        // for loop:
        // for ({x, y} of iterable) {}
        Type *type = noInitType;
        if (declarator->_init) {
          if (Type *inferred = tryInferInitExpression(declarator))
            type = inferred;
        }
        annotateDestructuringTarget(declarator, obj, type);
      } else {
        // Fallback for any other pattern types.
        outer.sm_.warning(
            declarator->_id->getSourceRange(),
            "ft: typing of pattern declarators not implemented, :any assumed");
      }
    }
  }

  /// Annotate the elements of the destructuring pattern given that the pattern
  /// itself has type \p type.
  /// Associates the target node and nested patterns with their respective
  /// types, and records the types of decls that were declared by the pattern.
  void annotateDestructuringTarget(
      ESTree::VariableDeclaratorNode *declarator,
      ESTree::PatternNode *pattern,
      Type *patternType) {
    llvh::SmallVector<DestructuringDefault, 2> defaults{};
    outer.resolveDestructuringTypes(
        pattern,
        patternType,
        [this, declarator](
            sema::Decl *decl, Type *t, ESTree::IdentifierNode *id) {
          outer.recordDecl(decl, t, id, declarator);
        },
        defaults);
    if (!defaults.empty())
      outer.declPatternDefaults_[pattern] = std::move(defaults);
  }

  void annotateFunctionDeclaration(
      ESTree::FunctionDeclarationNode *funcDecl,
      ESTree::Node *parent) {
    auto *id = llvh::cast<ESTree::IdentifierNode>(funcDecl->_id);
    sema::Decl *decl = outer.getDecl(id);
    assert(decl && "unresolved identifier");
    if (funcDecl->_typeParameters) {
      // Generics have no Decl type, but still record the node to catch
      // re-declarations.
      if (!outer.recordDeclNode(decl, id, funcDecl))
        return;
      decl->generic = true;
      outer.registerGeneric(
          decl, funcDecl, parent, outer.bindingTable_.getCurrentScope());
      return;
    }

    Type *type = outer.parseFunctionType(
        funcDecl->_params,
        funcDecl->_returnType,
        funcDecl->_async,
        funcDecl->_generator);

    // Global properties don't have sound types, since they can be
    // overwritten without our knowledge and control.
    if (decl->kind == sema::Decl::Kind::GlobalProperty) {
      if (llvh::isa<TypedFunctionType>(type->info)) {
        outer.sm_.warning(
            funcDecl->getStartLoc(),
            "ft: global property type annotations are unsound and are ignored");
      }
      type = outer.flowContext_.getAny();
    }

    outer.recordDecl(decl, type, id, funcDecl);
  }

  /// Annotate the type of the catch parameter if it exists.
  void annotateCatchClause(ESTree::CatchClauseNode *catchClause) {
    if (!catchClause->_param)
      return;

    if (auto *id =
            llvh::dyn_cast<ESTree::IdentifierNode>(catchClause->_param)) {
      sema::Decl *decl = outer.getDecl(id);
      Type *type = outer.parseOptionalTypeAnnotation(id->_typeAnnotation);
      if (!type)
        type = outer.flowContext_.getAny();

      if (!llvh::isa<AnyType>(type->info) &&
          !llvh::isa<MixedType>(type->info)) {
        outer.sm_.error(
            catchClause->_param->getSourceRange(),
            "ft: catch parameter must be 'any' or 'mixed'");
      }

      outer.recordDecl(decl, type, id, catchClause);
    } else {
      outer.sm_.warning(
          catchClause->_param->getSourceRange(),
          "ft: typing of pattern declarators not implemented, :any assumed");
    }
  }
};

bool FlowChecker::resolveScopeTypesAndAnnotate(
    ESTree::Node *scopeNode,
    sema::LexicalScope *scope) {
  const sema::ScopeDecls *decls =
      curFunctionContext_->declCollector->getScopeDeclsForNode(scopeNode);
  if (!decls || decls->empty())
    return true;

  assert(scope && "declarations found but no lexical scope");
  unsigned errorsBefore = sm_.getErrorCount();

  declareScopeTypes(*decls, scope, scopeNode);
  if (sm_.getErrorCount() != errorsBefore)
    return false;
  AnnotateScopeDecls(*this, *decls, scopeNode);

  // If there were any errors during annotation, fail.
  // Don't count errors that happened during typechecking other nodes.
  return sm_.getErrorCount() == errorsBefore;
}

void FlowChecker::drainTypecheckQueue() {
  auto savedScope = bindingTable_.getCurrentScope();

  while (!typecheckQueue_.empty()) {
    DeferredGenericClass deferred{std::move(typecheckQueue_.front())};
    typecheckQueue_.pop_front();
    ESTree::ClassDeclarationNode *specialization = deferred.specialization;
    LLVM_DEBUG(
        llvh::dbgs()
        << "Typechecking deferred generic class: "
        << llvh::cast<ESTree::IdentifierNode>(specialization->_id)->_name->str()
        << "\n");
    bindingTable_.activateScope(deferred.scope);
    visitClassNode(
        deferred.specialization,
        llvh::cast<ESTree::ClassBodyNode>(specialization->_body),
        deferred.classType);
  }

  bindingTable_.activateScope(savedScope);
}

/// Record the declaration's type and declaring AST node, while checking for
/// and reporting re-declarations.
bool FlowChecker::recordDecl(
    sema::Decl *decl,
    Type *type,
    ESTree::IdentifierNode *id,
    ESTree::Node *astDeclNode) {
  if (!recordDeclNode(decl, id, astDeclNode))
    return false;
  declTypes_.try_emplace(decl, type);
  return true;
}

bool FlowChecker::recordDeclNode(
    sema::Decl *decl,
    ESTree::IdentifierNode *id,
    ESTree::Node *astDeclNode) {
  assert(decl && "unresolved identifier");
  auto [it, inserted] = declNodes_.try_emplace(decl, astDeclNode);
  if (inserted)
    return true;
  sm_.error(id->getStartLoc(), "ft: redeclaration of " + id->_name->str());
  sm_.note(
      it->second->getSourceRange(),
      "ft: first declaration of " + id->_name->str());
  return false;
}

Type *FlowChecker::parseFunctionType(
    ESTree::NodeList &params,
    ESTree::Node *optReturnTypeAnnotation,
    bool isAsync,
    bool isGenerator,
    Type *defaultReturnType,
    Type *defaultThisType,
    TypedFunctionType *constraint) {
  llvh::SmallVector<TypedFunctionType::Param, 4> paramsList{};

  // If the default return type is expected, then we are parsing a typed
  // function, even if it doesn't have any explicit type annotations. A
  // constraint likewise produces a typed function, so the parameter and return
  // types inferred from it are preserved.
  bool isTyped = (defaultReturnType != nullptr) || (constraint != nullptr);

  bool seenOptional = false;

  /// Resolve a parameter's type, consulting the constraint to infer the type of
  /// unannotated parameters and to match inference placeholders.
  /// \p paramType is the parsed annotation type, may be nullptr.
  /// \p paramConstraintType is the constraint type for the current parameter,
  /// may be nullptr.
  auto constrainParam =
      [this](Type *paramType, Type *paramConstraintType) -> Type * {
    if (paramType) {
      if (paramConstraintType)
        matchConstraintToType(paramConstraintType, paramType);
      return paramType;
    } else if (paramConstraintType) {
      return paramConstraintType;
    } else {
      return flowContext_.getAny();
    }
  };

  // Index of the current parameter.
  size_t constraintIdx = 0;
  for (ESTree::Node &n : params) {
    const TypedFunctionType::Param *paramConstraint =
        (constraint && constraintIdx < constraint->getParams().size())
        ? &constraint->getParams()[constraintIdx]
        : nullptr;
    Type *paramConstraintType =
        paramConstraint ? paramConstraint->type : nullptr;

    if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(&n)) {
      if (id->_optional) {
        seenOptional = true;
      } else if (seenOptional) {
        sm_.error(id->getSourceRange(), "ft: optional params must be last");
      }
      Type *annot = id->_typeAnnotation
          ? parseOptionalTypeAnnotation(id->_typeAnnotation)
          : nullptr;
      paramsList.push_back(
          {Identifier::getFromPointer(id->_name),
           constrainParam(annot, paramConstraintType),
           id->_optional});
      isTyped |= (id->_typeAnnotation != nullptr);
    } else if (
        auto *assign = llvh::dyn_cast<ESTree::AssignmentPatternNode>(&n)) {
      if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(assign->_left)) {
        seenOptional = true;
        Type *annot = id->_typeAnnotation
            ? parseOptionalTypeAnnotation(id->_typeAnnotation)
            : nullptr;
        paramsList.push_back(
            {Identifier::getFromPointer(id->_name),
             constrainParam(annot, paramConstraintType),
             /*optional=*/true});
        isTyped |= (id->_typeAnnotation != nullptr);
      } else if (
          auto *annot = ESTree::getPatternTypeAnnotation(assign->_left)) {
        // Destructuring param with default value and type annotation.
        seenOptional = true;
        isTyped = true;
        paramsList.push_back(
            {Identifier(),
             constrainParam(
                 parseOptionalTypeAnnotation(annot), paramConstraintType),
             /*optional=*/true});
      } else if (
          llvh::isa<ESTree::ObjectPatternNode>(assign->_left) ||
          llvh::isa<ESTree::ArrayPatternNode>(assign->_left)) {
        // Destructuring param with default value but no annotation.
        seenOptional = true;
        if (!paramConstraintType)
          sm_.error(
              assign->_left->getSourceRange(),
              "ft: destructuring parameters must have a type annotation");
        paramsList.push_back(
            {Identifier(),
             constrainParam(nullptr, paramConstraintType),
             /*optional=*/true});
      } else {
        sm_.warning(
            n.getSourceRange(),
            "ft: typing of pattern parameters not implemented, :any assumed");
        paramsList.push_back({Identifier(), flowContext_.getAny(), true});
      }
    } else if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&n)) {
      if (auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(rest->_argument)) {
        Type *annot = id->_typeAnnotation
            ? parseOptionalTypeAnnotation(id->_typeAnnotation)
            : nullptr;
        if (annot && !flowContext_.isArrayClassType(annot)) {
          sm_.error(
              id->_typeAnnotation->getSourceRange(),
              "ft: rest parameter type must be Array<T>");
        }
        Type *restConstraintType =
            (paramConstraintType &&
             flowContext_.isArrayClassType(paramConstraintType))
            ? paramConstraintType
            : nullptr;
        paramsList.push_back(
            {Identifier::getFromPointer(id->_name),
             constrainParam(annot, restConstraintType),
             /*optional=*/false,
             /*rest=*/true});
        isTyped |= (id->_typeAnnotation != nullptr);
      } else {
        sm_.warning(
            n.getSourceRange(),
            "ft: typing of rest pattern parameters not implemented,"
            " :any assumed");
        paramsList.push_back(
            {Identifier(), flowContext_.getAny(), false, /*rest=*/true});
      }
    } else if (auto *annot = ESTree::getPatternTypeAnnotation(&n)) {
      // Destructuring param without default value, with type annotation.
      isTyped = true;
      paramsList.push_back(
          {Identifier(),
           constrainParam(
               parseOptionalTypeAnnotation(annot), paramConstraintType),
           false});
    } else if (
        llvh::isa<ESTree::ObjectPatternNode>(&n) ||
        llvh::isa<ESTree::ArrayPatternNode>(&n)) {
      // Destructuring param without annotation.
      if (!paramConstraintType)
        sm_.error(
            n.getSourceRange(),
            "ft: destructuring parameters must have a type annotation");
      paramsList.push_back(
          {Identifier(),
           constrainParam(nullptr, paramConstraintType),
           /*optional=*/false});
    } else {
      sm_.warning(
          n.getSourceRange(),
          "ft: typing of pattern parameters not implemented, :any assumed");
      paramsList.push_back({Identifier(), flowContext_.getAny(), false});
    }
    ++constraintIdx;
  }

  Type *returnType =
      parseOptionalTypeAnnotation(optReturnTypeAnnotation, defaultReturnType);
  isTyped |= (optReturnTypeAnnotation != nullptr);
  if (constraint) {
    if (optReturnTypeAnnotation) {
      // Explicit return annotation: fill inference placeholders from it.
      matchConstraintToType(constraint->getReturnType(), returnType);
    } else if (!defaultReturnType) {
      // No annotation: borrow the constraint's return type, which may itself be
      // an inference placeholder filled in when return statements are visited.
      returnType = constraint->getReturnType();
    }
  }

  Type *thisParamType = defaultThisType;
  llvh::ArrayRef<TypedFunctionType::Param> paramsRef(paramsList);

  // Check if the first parameter is "this", since it is treated specially.
  if (!paramsRef.empty() &&
      paramsRef.front().name.getUnderlyingPointer() == kw_.identThis) {
    // User is allowed to specify a "this" on a method where it's already known,
    // but it must be the same type as the given type.
    if (thisParamType &&
        !thisParamType->info->equals(paramsRef.front().type->info)) {
      sm_.error(
          paramsRef.front().type->node->getSourceRange(),
          "ft: incompatible 'this' type annotation");
    }

    thisParamType = paramsRef.front().type;
    paramsRef = paramsRef.drop_front();
  }

  BaseFunctionType *res = isTyped
      ? static_cast<BaseFunctionType *>(flowContext_.createFunction(
            returnType, thisParamType, paramsRef, isAsync, isGenerator))
      : flowContext_.createUntypedFunction(isAsync, isGenerator);
  return flowContext_.createType(res);
}

Type *FlowChecker::parseOptionalTypeAnnotation(
    ESTree::Node *optAnnotation,
    Type *defaultType) {
  if (!optAnnotation)
    return defaultType ? defaultType : flowContext_.getAny();
  return parseTypeAnnotation(
      llvh::cast<ESTree::TypeAnnotationNode>(optAnnotation)->_typeAnnotation);
}

Type *FlowChecker::parseTypeAnnotation(ESTree::Node *node) {
  assert(
      node &&
      "parseTypeAnnotation requires an annotation, "
      "use parseOptionalTypeAnnotation otherwise");

  switch (node->getKind()) {
    case ESTree::NodeKind::VoidTypeAnnotation:
      return flowContext_.getVoid();
    case ESTree::NodeKind::NullLiteralTypeAnnotation:
      return flowContext_.getNull();
    case ESTree::NodeKind::BooleanTypeAnnotation:
      return flowContext_.getBoolean();
    case ESTree::NodeKind::StringTypeAnnotation:
      return flowContext_.getString();
    case ESTree::NodeKind::StringLiteralTypeAnnotation:
      return flowContext_.createType(
          flowContext_.createStringLiteral(
              llvh::cast<ESTree::StringLiteralTypeAnnotationNode>(node)
                  ->_value),
          node);
    case ESTree::NodeKind::NumberTypeAnnotation:
      return flowContext_.getNumber();
    case ESTree::NodeKind::SymbolTypeAnnotation:
      return flowContext_.getSymbol();
    case ESTree::NodeKind::NumberLiteralTypeAnnotation:
      return flowContext_.createType(
          flowContext_.createNumberLiteral(
              llvh::cast<ESTree::NumberLiteralTypeAnnotationNode>(node)
                  ->_value),
          node);
    case ESTree::NodeKind::BooleanLiteralTypeAnnotation:
      return flowContext_.createType(
          flowContext_.createBooleanLiteral(
              llvh::cast<ESTree::BooleanLiteralTypeAnnotationNode>(node)
                  ->_value),
          node);
    case ESTree::NodeKind::BigIntTypeAnnotation:
      return flowContext_.getBigInt();
    case ESTree::NodeKind::AnyTypeAnnotation:
      return flowContext_.getAny();
    case ESTree::NodeKind::MixedTypeAnnotation:
      return flowContext_.getMixed();
    case ESTree::NodeKind::UnionTypeAnnotation:
      return parseUnionTypeAnnotation(
          llvh::cast<ESTree::UnionTypeAnnotationNode>(node));
    case ESTree::NodeKind::NullableTypeAnnotation:
      return parseNullableTypeAnnotation(
          llvh::cast<ESTree::NullableTypeAnnotationNode>(node));
    case ESTree::NodeKind::ArrayTypeAnnotation:
      return parseArrayTypeAnnotation(
          llvh::cast<ESTree::ArrayTypeAnnotationNode>(node));
    case ESTree::NodeKind::TupleTypeAnnotation:
      return parseTupleTypeAnnotation(
          llvh::cast<ESTree::TupleTypeAnnotationNode>(node));
    case ESTree::NodeKind::ObjectTypeAnnotation:
      return parseObjectTypeAnnotation(
          llvh::cast<ESTree::ObjectTypeAnnotationNode>(node));
    case ESTree::NodeKind::GenericTypeAnnotation:
      return parseGenericTypeAnnotation(
          llvh::cast<ESTree::GenericTypeAnnotationNode>(node));
    case ESTree::NodeKind::FunctionTypeAnnotation:
      return parseFunctionTypeAnnotation(
          llvh::cast<ESTree::FunctionTypeAnnotationNode>(node));

    default:
      sm_.error(
          node->getSourceRange(),
          "ft: unimplemented type annotation " + node->getNodeName());
      return flowContext_.getAny();
  }
}

Type *FlowChecker::parseUnionTypeAnnotation(
    ESTree::UnionTypeAnnotationNode *node) {
  llvh::SmallVector<Type *, 4> types{};
  for (auto &n : node->_types) {
    types.push_back(parseTypeAnnotation(&n));
    // Need to check for looping types here because n may not have been used in
    // a union before and we need looping information for canonicalizing unions.
    findLoopingTypes(types.back());
  }
  return flowContext_.createType(flowContext_.maybeCreateUnion(types), node);
}

Type *FlowChecker::parseNullableTypeAnnotation(
    ESTree::NullableTypeAnnotationNode *node) {
  Type *t = parseTypeAnnotation(node->_typeAnnotation);
  // Need to check for looping types here because n may not have been used in
  // a union before and we need looping information for canonicalizing unions.
  findLoopingTypes(t);
  return flowContext_.createType(flowContext_.createPopulatedNullable(t), node);
}

Type *FlowChecker::resolveArrayType(Type *elementType, ESTree::Node *node) {
  Type *arrClassType =
      getSpecializedArrayClassType(elementType, node->getSourceRange());
  if (!arrClassType) {
    // If the element type is an inference placeholder, create an
    // InferencePlaceholderArrayType to preserve the constraint structure
    // for type inference.
    if (elementType->info &&
        llvh::isa<InferencePlaceholderType>(elementType->info)) {
      return flowContext_.createType(
          flowContext_.createInferencePlaceholderArray(elementType), node);
    }
    sm_.error(node->getSourceRange(), "ft: Array type requires TypedLib");
    return flowContext_.createType(flowContext_.getAnyInfo(), node);
  }
  return arrClassType;
}

Type *FlowChecker::parseArrayTypeAnnotation(
    ESTree::ArrayTypeAnnotationNode *node) {
  Type *elementType = parseTypeAnnotation(node->_elementType);
  return resolveArrayType(elementType, node);
}

Type *FlowChecker::parseTupleTypeAnnotation(
    ESTree::TupleTypeAnnotationNode *node) {
  return processTupleTypeAnnotation(
      node, [this](ESTree::Node *annotation) -> Type * {
        return parseTypeAnnotation(annotation);
      });
}

Type *FlowChecker::parseObjectTypeAnnotation(
    ESTree::ObjectTypeAnnotationNode *node) {
  return processObjectTypeAnnotation(
      node, [this](ESTree::Node *annotation) -> Type * {
        return parseTypeAnnotation(annotation);
      });
}

Type *FlowChecker::parseGenericTypeAnnotation(
    ESTree::GenericTypeAnnotationNode *node) {
  auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(node->_id);

  if (!id) {
    sm_.error(node->getSourceRange(), "ft: unsupported type annotation");
    return flowContext_.getAny();
  }

  TypeDecl *td = bindingTable_.find(id->_name);

  if (!td) {
    sm_.error(id->getSourceRange(), "ft: undefined type " + id->_name->str());
    return flowContext_.getAny();
  }

  if (td->staticTypeParam) {
    sm_.error(
        id->getSourceRange(),
        "ft: class type parameter " + id->_name->str() +
            " cannot be used in a static member");
    return flowContext_.getAny();
  }

  if (!td->type) {
    if (!node->_typeParameters) {
      sm_.error(
          node->getSourceRange(),
          llvh::Twine("ft: missing generic arguments for '") +
              id->_name->str() + "'");
      return flowContext_.getAny();
    }
    if (td->genericClassDecl) {
      // Array<T> is equivalent to T[], so use the same code path which
      // properly handles inference placeholders.
      if (td->genericClassDecl == arrayClassDecl_) {
        auto *typeArgsNode = llvh::cast<ESTree::TypeParameterInstantiationNode>(
            node->_typeParameters);
        if (typeArgsNode->_params.size() == 1) {
          Type *elementType =
              parseTypeAnnotation(&typeArgsNode->_params.front());
          return resolveArrayType(elementType, node);
        }
      }
      // Resolve to the specialized class.
      return resolveGenericClassSpecializationForType(
          node, td->genericClassDecl);
    } else {
      // This is a generic type alias.
      return resolveGenericTypeAlias(node, td);
    }
  }

  return td->type;
}

Type *FlowChecker::parseFunctionTypeAnnotation(
    ESTree::FunctionTypeAnnotationNode *node) {
  Type *result = processFunctionTypeAnnotation(
      node, [this](ESTree::Node *annotation) -> Type * {
        return parseTypeAnnotation(annotation);
      });
  // Validate rest param is Array<T>.
  if (node->_rest) {
    auto *restParam = llvh::cast<ESTree::FunctionTypeParamNode>(node->_rest);
    if (restParam->_typeAnnotation) {
      auto *typedFunc = llvh::cast<TypedFunctionType>(result->info);
      auto params = typedFunc->getParams();
      if (!params.empty() && params.back().rest &&
          !flowContext_.isArrayClassType(params.back().type)) {
        sm_.error(
            restParam->_typeAnnotation->getSourceRange(),
            "ft: rest parameter type must be Array<T>");
      }
    }
  }
  return result;
}

void FlowChecker::validateGenericBound(const GenericBoundCheck &check) {
  if (!canAFlowIntoB(check.argument, check.bound).canFlowWithoutCast()) {
    sm_.error(
        check.errorRange,
        llvh::Twine("ft: type argument for type parameter '") +
            check.parameterName->str() + "' is incompatible with its bound");
  }
}

FlowChecker::CanFlowResult FlowChecker::canAFlowIntoB(
    TypeInfo *a,
    TypeInfo *b,
    CanFlowState &state,
    ThisFlowDirection thisFlow,
    CheckedCastMode castMode) {
  // Terminal fast paths answer without recursing, so check them before
  // paying for cycle bookkeeping.
  if (a == b)
    return {.canFlow = true};

  // _ -> any
  // _ -> mixed
  if (llvh::isa<AnyType>(b) || llvh::isa<MixedType>(b))
    return {.canFlow = true};
  // any -> _
  if (llvh::isa<AnyType>(a))
    return {.canFlow = true, .needCheckedCast = true};
  // empty -> _ (bottom type, flows into everything)
  if (llvh::isa<EmptyType>(a))
    return {.canFlow = true};

  // Only Forbidden relations can repeat: Allowed (and MethodOverride) exist
  // only at the outermost relation and propagate solely through the union
  // loops, which strictly shrink the pair because unions are flattened.
  //
  // Revisiting an active relation provides no new information, so assume it
  // flows without a cast and let the original check validate the rest of the
  // type structure. Every Forbidden consumer rejects a result that needs a
  // cast, so this is the usual coinductive hypothesis: if it's wrong, the
  // original check fails, and nothing derived under it is retained.
  auto &active = state.active;
  bool pushed = false;
  if (castMode == CheckedCastMode::Forbidden) {
    if (!active.insert({a, b}))
      return {.canFlow = true};
    pushed = true;
  }
  auto popOnExit = llvh::make_scope_exit([&active, pushed]() {
    if (pushed)
      active.pop_back();
  });

  // if `a` is union, all of its arms must be able to flow into `b`.
  if (UnionType *unionA = llvh::dyn_cast<UnionType>(a)) {
    bool needCheckedCast = false;
    for (auto *aType : unionA->getTypes()) {
      CanFlowResult tmp =
          canAFlowIntoB(aType->info, b, state, thisFlow, castMode);
      if (!tmp.canFlow)
        return tmp;
      needCheckedCast |= tmp.needCheckedCast;
    }
    return {.canFlow = true, .needCheckedCast = needCheckedCast};
  }

  // if `b` is union, `a` must be able to flow into at least one arm.
  if (UnionType *unionB = llvh::dyn_cast<UnionType>(b)) {
    // Shortcut the check if `b` has "all-accepting" arms.
    if (unionB->hasAny() || unionB->hasMixed())
      return {.canFlow = true};

    // Check if `a` can flow into at least one of b's arms.
    // If we find an arm that does not need a checked cast, we're done.
    bool foundWithCast = false;
    for (auto *bType : unionB->getTypes()) {
      CanFlowResult tmp =
          canAFlowIntoB(a, bType->info, state, thisFlow, castMode);
      if (tmp.canFlow) {
        if (!tmp.needCheckedCast)
          return {.canFlow = true};
        foundWithCast = true;
      }
    }
    return foundWithCast
        ? CanFlowResult{.canFlow = true, .needCheckedCast = true}
        : CanFlowResult{};
  }

  // Tuples are invariant, so if `a` is an tuple, `b` must be an tuple with the
  // same types.
  if (auto *tupleA = llvh::dyn_cast<TupleType>(a)) {
    auto *tupleB = llvh::dyn_cast<TupleType>(b);
    if (!tupleB)
      return {};
    return canAFlowIntoB(tupleA, tupleB, state);
  }

  // Objects are invariant, so if `a` is an object, `b` must be an object with
  // the same types.
  if (auto *objectA = llvh::dyn_cast<ExactObjectType>(a)) {
    auto *objectB = llvh::dyn_cast<ExactObjectType>(b);
    if (!objectB)
      return {};
    return canAFlowIntoB(objectA, objectB, state);
  }

  if (ClassType *classA = llvh::dyn_cast<ClassType>(a)) {
    ClassType *classB = llvh::dyn_cast<ClassType>(b);
    if (!classB)
      return {};
    return canAFlowIntoB(classA, classB, state);
  }

  if (auto *consA = llvh::dyn_cast<ClassConstructorType>(a)) {
    auto *consB = llvh::dyn_cast<ClassConstructorType>(b);
    if (!consB)
      return {};
    // Delegate to the underlying ClassType comparison.
    return canAFlowIntoB(
        llvh::cast<ClassType>(consA->getClassType()->info),
        llvh::cast<ClassType>(consB->getClassType()->info),
        state);
  }

  if (BaseFunctionType *funcA = llvh::dyn_cast<BaseFunctionType>(a)) {
    BaseFunctionType *funcB = llvh::dyn_cast<BaseFunctionType>(b);
    if (!funcB)
      return {};
    return canAFlowIntoB(funcA, funcB, state, thisFlow);
  }

  // A literal flows into its base type, and into another literal of the same
  // kind when the value is identical.
  if (a->isLiteral()) {
    if (b->getKind() == widenLiteralKind(a->getKind()))
      return {.canFlow = true};
    if (a->getKind() == b->getKind())
      return {.canFlow = a->equals(b)};
    return {};
  }

  return {};
}

FlowChecker::CanFlowResult
FlowChecker::canAFlowIntoB(ClassType *a, ClassType *b, CanFlowState &) {
  // It can flow into any superclass.
  ClassType *cur = a;
  while (cur) {
    // `b` is in the inheritance chain of `a`.
    if (cur == b)
      return {.canFlow = true};
    cur = cur->getSuperClassInfo();
  }
  return {};
}

FlowChecker::CanFlowResult
FlowChecker::canAFlowIntoB(TupleType *a, TupleType *b, CanFlowState &) {
  auto aTypes = a->getTypes();
  auto bTypes = b->getTypes();
  if (aTypes.size() != bTypes.size()) {
    return {};
  }

  for (size_t i = 0, e = aTypes.size(); i < e; ++i) {
    // TODO: This will be more complex when we allow variance in tuple types.
    if (!aTypes[i]->info->equals(bTypes[i]->info)) {
      return {};
    }
  }

  return {.canFlow = true};
}

FlowChecker::CanFlowResult FlowChecker::canAFlowIntoB(
    ExactObjectType *a,
    ExactObjectType *b,
    CanFlowState &state) {
  // Decide whether a source value of variance \p av and type \p aVal flows
  // into a destination value of variance \p bv and type \p bVal (used for both
  // named fields and indexers).
  auto valueFlows =
      [this, &state](
          Type *aVal, FieldVariance av, Type *bVal, FieldVariance bv) -> bool {
    // Invariant source can flow into a destination of any variance.
    // A ReadOnly or WriteOnly source can only flow into the same variance.
    if (av != FieldVariance::None && av != bv)
      return false;
    switch (bv) {
      case FieldVariance::None:
        // Invariant: types must be equal.
        return aVal->info->equals(bVal->info);
      case FieldVariance::ReadOnly:
        // Covariant: a's value must flow into b's value without a cast.
        return canAFlowIntoB(aVal, bVal, state).canFlowWithoutCast();
      case FieldVariance::WriteOnly:
        // Contravariant: b's value must flow into a's value without a cast.
        return canAFlowIntoB(bVal, aVal, state).canFlowWithoutCast();
    }
    llvm_unreachable("invalid FieldVariance");
  };

  // The destination is an indexer object. The source flows in if it provides
  // values compatible with the indexer, either via its own indexer or, for a
  // string-keyed indexer, via its named fields (object width subtyping).
  if (b->hasIndexer()) {
    const ExactObjectType::Indexer &bi = *b->getIndexer();
    if (a->hasIndexer()) {
      const ExactObjectType::Indexer &ai = *a->getIndexer();
      // Keys are invariant.
      if (!ai.keyType->info->equals(bi.keyType->info))
        return {};
      return valueFlows(ai.valueType, ai.variance, bi.valueType, bi.variance)
          ? CanFlowResult{.canFlow = true}
          : CanFlowResult{};
    } else {
      // Source has named fields (possibly none). An empty object flows into any
      // indexer; a named field requires a string-keyed indexer since field
      // names are strings.
      if (!a->getFields().empty() && !llvh::isa<StringType>(bi.keyType->info))
        return {};
      for (const auto &f : a->getFields()) {
        if (!valueFlows(f.type, f.variance, bi.valueType, bi.variance))
          return {};
      }
      return {.canFlow = true};
    }
  }

  // The destination has no indexer, so an indexer source can't flow in.
  if (a->hasIndexer())
    return {};

  auto aFields = a->getFields();
  auto bFields = b->getFields();
  if (aFields.size() != bFields.size()) {
    return {};
  }

  for (size_t i = 0, e = aFields.size(); i < e; ++i) {
    if (aFields[i].name != bFields[i].name)
      return {};
    if (!valueFlows(
            aFields[i].type,
            aFields[i].variance,
            bFields[i].type,
            bFields[i].variance))
      return {};
  }

  return {.canFlow = true};
}

FlowChecker::CanFlowResult FlowChecker::canAFlowIntoB(
    BaseFunctionType *a,
    BaseFunctionType *b,
    CanFlowState &state,
    ThisFlowDirection thisFlow) {
  // Function a can flow into b when:
  // * they're the same kind of function (async, generator, etc)
  // * all parameters of b can flow into parameters of a
  // * return type of a can flow into return type of b
  // * no checked casts are needed (can't flow into an 'any' parameter)

  if (a->isAsync() != b->isAsync())
    return {};
  if (a->isGenerator() != b->isGenerator())
    return {};

  // Typed, untyped and native functions are incompatible types.
  if (a->getKind() != b->getKind())
    return {};

  if (llvh::isa<UntypedFunctionType>(a)) {
    assert(
        llvh::isa<UntypedFunctionType>(b) &&
        "we already checked that the kinds are the same!");
    // Untyped functions can flow into each other.
    return {.canFlow = true};
  }

  if (llvh::isa<NativeFunctionType>(a)) {
    auto *nativeA = llvh::cast<NativeFunctionType>(a);
    auto *nativeB = llvh::cast<NativeFunctionType>(b);
    // Native functions can flow into each other only if their signatures are
    // the same.
    if (nativeA->getSignature() != nativeB->getSignature())
      return {};
    return {.canFlow = true};
  }

  auto *aType = llvh::cast<TypedFunctionType>(a);
  auto *bType = llvh::cast<TypedFunctionType>(b);

  // Handle 'this' parameter compatibility.
  // A function without 'this' can flow into a function with 'this: void',
  // because arrow functions (which have no 'this') can be used as callbacks.
  if (aType->getThisParam() && !bType->getThisParam()) {
    // 'a' has 'this' but 'b' does not - can't flow.
    return {};
  }
  if (aType->getThisParam() && bType->getThisParam()) {
    // Both functions have `this`, it must be checked.
    CanFlowResult flowRes = thisFlow == ThisFlowDirection::Default
        ? canAFlowIntoB(bType->getThisParam(), aType->getThisParam(), state)
        : canAFlowIntoB(aType->getThisParam(), bType->getThisParam(), state);
    if (!flowRes.canFlow || flowRes.needCheckedCast)
      return {};
  }

  bool needCheckedCast = false;

  {
    // Compare using non-rest param counts.
    bool aHasRest =
        !aType->getParams().empty() && aType->getParams().back().rest;
    bool bHasRest =
        !bType->getParams().empty() && bType->getParams().back().rest;
    size_t aNonRest =
        aHasRest ? aType->getParams().size() - 1 : aType->getParams().size();
    size_t bNonRest =
        bHasRest ? bType->getParams().size() - 1 : bType->getParams().size();

    auto restElemOrNull = [this](TypedFunctionType *t) -> Type * {
      assert(
          !t->getParams().empty() && t->getParams().back().rest &&
          "restElemOrNull called on function without rest param");
      Type *restType = t->getParams().back().type;
      return flowContext_.isArrayClassType(restType)
          ? flowContext_.getArrayElementType(restType)
          : nullptr;
    };

    if (aNonRest < bNonRest) {
      // It's valid to flow from a function with fewer parameters to one
      // with more, but we need a checked cast.
      needCheckedCast = true;
      // If a has a rest param, b's extra fixed params get passed into a's
      // rest array, so each must flow into a's rest element type.
      if (aHasRest) {
        Type *aElem = restElemOrNull(aType);
        if (!aElem) {
          return {};
        }
        for (size_t i = aNonRest; i < bNonRest; ++i) {
          CanFlowResult flowRes =
              canAFlowIntoB(bType->getParams()[i].type, aElem, state);
          if (!flowRes.canFlow || flowRes.needCheckedCast)
            return {};
        }
      }
    } else if (aNonRest > bNonRest) {
      // Removing optional parameters is always valid.
      const auto &extraParam = aType->getParams()[bNonRest];
      assert(
          !extraParam.rest &&
          "extra param at bNonRest index should never be a rest param");
      if (!extraParam.optional) {
        return {};
      }
      needCheckedCast = true;
      // If b has a rest param, calls through b can supply values for a's
      // extra (optional) fixed params, so b's rest element must flow into
      // each of those fixed param types.
      if (bHasRest) {
        Type *bElem = restElemOrNull(bType);
        if (!bElem) {
          return {};
        }
        for (size_t i = bNonRest; i < aNonRest; ++i) {
          CanFlowResult flowRes =
              canAFlowIntoB(bElem, aType->getParams()[i].type, state);
          if (!flowRes.canFlow || flowRes.needCheckedCast)
            return {};
        }
      }
    }

    size_t minParamCount = std::min(aNonRest, bNonRest);
    for (size_t i = 0; i < minParamCount; ++i) {
      Type *paramA = aType->getParams()[i].type;
      Type *paramB = bType->getParams()[i].type;
      CanFlowResult flowRes = canAFlowIntoB(paramB, paramA, state);
      if (!flowRes.canFlow || flowRes.needCheckedCast)
        return {};
    }

    if (aHasRest != bHasRest) {
      // Adding rest params is like adding other params.
      // Removing rest params is like removing optional params.
      // Both require checked casts.
      needCheckedCast = true;
    } else if (aHasRest && bHasRest) {
      // Both have rest: compare element types contravariantly.
      Type *aElem = restElemOrNull(aType);
      Type *bElem = restElemOrNull(bType);
      if (!aElem || !bElem)
        return {};
      CanFlowResult flowRes = canAFlowIntoB(bElem, aElem, state);
      if (!flowRes.canFlow || flowRes.needCheckedCast)
        return {};
    }
  }

  {
    CanFlowResult flowRes =
        canAFlowIntoB(aType->getReturnType(), bType->getReturnType(), state);
    if (!flowRes.canFlow || flowRes.needCheckedCast)
      return {};
  }

  // If these are native functions, their signatures must match exactly.
  if (auto *nativeA = llvh::dyn_cast<NativeFunctionType>(a)) {
    auto *nativeB = llvh::cast<NativeFunctionType>(b);
    if (nativeA->getSignature()->compare(nativeB->getSignature()) != 0)
      return {};
  }

  return {.canFlow = true, .needCheckedCast = needCheckedCast};
}

Type *FlowChecker::getNonOptionalSingleType(Type *exprType) {
  if (auto *unionTy = llvh::dyn_cast<UnionType>(exprType->info)) {
    Type *singleType = nullptr;
    // There is only one useful type to narrow to.
    for (Type *unionArm : unionTy->getTypes()) {
      if (llvh::isa<VoidType>(unionArm->info))
        continue;
      if (llvh::isa<NullType>(unionArm->info))
        continue;
      // Found a second type.
      if (singleType) {
        return nullptr;
      }
      singleType = unionArm;
    }
    return singleType;
  }

  return nullptr;
}

Type *FlowChecker::widenLiteralType(Type *type) {
  if (type->info->isLiteral())
    return flowContext_.getSingletonType(
        widenLiteralKind(type->info->getKind()));

  if (auto *unionType = llvh::dyn_cast<UnionType>(type->info)) {
    // Widen any direct literal arms to their base type. Don't recurse into
    // the arms: union arms are already flattened, and recursing could loop on
    // cyclic union arms.
    bool changed = false;
    llvh::SmallVector<Type *, 4> arms{};
    for (Type *arm : unionType->getTypes()) {
      if (arm->info->isLiteral()) {
        arms.push_back(flowContext_.getSingletonType(
            widenLiteralKind(arm->info->getKind())));
        changed = true;
      } else {
        arms.push_back(arm);
      }
    }
    if (!changed)
      return type;
    return flowContext_.createType(flowContext_.maybeCreateUnion(arms));
  }

  return type;
}

std::pair<Type *, FlowChecker::CanFlowResult> FlowChecker::tryNarrowType(
    Type *exprType,
    Type *targetType) {
  // If the types are already compatible, no need to do anything.
  auto cf = canAFlowIntoB(exprType, targetType);
  if (cf.canFlow) {
    // Signal a cast to the targetType in the caller if necessary.
    return {cf.needCheckedCast ? targetType : exprType, cf};
  }

  // Try to narrow the expression type to a single type.
  Type *narrowType = getNonOptionalSingleType(exprType);
  // Try to widen literal arms and retry, so that e.g.
  // ?("x" | "y") narrows to string.
  if (!narrowType)
    narrowType = getNonOptionalSingleType(widenLiteralType(exprType));
  // Done if we haven't managed to find a narrower type.
  if (!narrowType)
    return {exprType, cf};

  auto cfCast = canAFlowIntoB(narrowType, targetType);
  if (!cfCast.canFlow) {
    return {exprType, cf};
  }

  // Collapse the two chained checked casts into a single one.
  if (cfCast.needCheckedCast) {
    return {targetType, cfCast};
  }

  // Otherwise, going from narrowType to targetType doesn't require a checked
  // cast, but we do need to cast from exprType to targetType because targetType
  // could be a union that shares arms with exprType.
  // Example: casting from `?T` to `T|void` needs to go to `T|void` properly
  // because IRGen cannot assume that the type is `T` afterwards.
  cfCast.needCheckedCast = true;
  return {targetType, cfCast};
}

ESTree::Node *FlowChecker::implicitCheckedCast(
    ESTree::Node *argument,
    Type *toType,
    CanFlowResult canFlow) {
  if (!canFlow.needCheckedCast || !compile_)
    return argument;
  auto *cast = new (astContext_) ESTree::ImplicitCheckedCastNode(argument);
  cast->copyLocationFrom(argument);
  setNodeType(cast, toType);
  return cast;
}

sema::Decl *FlowChecker::specializeGeneric(
    sema::Decl *oldDecl,
    ESTree::TypeParameterInstantiationNode *typeArgsNode,
    sema::LexicalScope *scope) {
  // Parse and populate the type arguments.
  llvh::SmallVector<Type *, 2> typeArgTypes{};
  for (ESTree::Node &arg : typeArgsNode->_params) {
    Type *type = parseTypeAnnotation(&arg);
    typeArgTypes.push_back(type);
  }

  return specializeGenericWithParsedTypes(
             oldDecl, typeArgsNode->getSourceRange(), typeArgTypes, scope)
      .first;
}

/// Get the NodeList from a parent node for inserting specializations.
static ESTree::NodeList &getParentNodeList(ESTree::Node *parent) {
  switch (parent->getKind()) {
    case ESTree::NodeKind::Program:
      return llvh::cast<ESTree::ProgramNode>(parent)->_body;
    case ESTree::NodeKind::BlockStatement:
      return llvh::cast<ESTree::BlockStatementNode>(parent)->_body;
    case ESTree::NodeKind::ClassBody:
      return llvh::cast<ESTree::ClassBodyNode>(parent)->_body;
    default:
      hermes_fatal("invalid parent node for generic specialization");
  }
}

std::pair<ESTree::Node *, bool> FlowChecker::specializeGenericImpl(
    GenericInfo<ESTree::Node> &generic,
    sema::LexicalScope *cloneScope,
    SMRange errorRange,
    llvh::ArrayRef<Type *> typeArgTypes) {
  assert(cloneScope && "must have cloneScope");

  // Extract info from types.
  GenericInfo<ESTree::Node>::TypeArgsVector typeArgs{};
  for (Type *type : typeArgTypes) {
    assert(type->info && "missing type info for generic specialization");
    typeArgs.push_back(type->info);
  }

  // Retrieve specialization if it exists.
  GenericInfo<ESTree::Node>::TypeArgsRef typeArgsRef = typeArgs;
  ESTree::Node *specialization = generic.getSpecialization(typeArgsRef);

  bool wasNewlyCreated = !specialization;

  // Create specialization if it doesn't exist.
  if (wasNewlyCreated) {
    LLVM_DEBUG(llvh::dbgs() << "Creating specialization for generic\n");
    // Avoid stack overflow in the clone by executing in a separate stack.
    executeInStack(
        *stackExecutor_, [this, &generic, cloneScope, &specialization]() {
          specialization = cloneNode(
              astContext_,
              semContext_,
              declCollectorMap_,
              generic.originalNode,
              cloneScope->parentFunction,
              cloneScope);
        });
    if (!specialization) {
      sm_.error(errorRange, "failed to create specialization for generic");
      return {nullptr, false};
    }
    auto &nodeList = getParentNodeList(generic.parent);
    nodeList.insert(generic.originalNode->getIterator(), *specialization);
    typeArgsRef =
        generic.addSpecialization(*this, std::move(typeArgs), specialization);
  }

  return {specialization, wasNewlyCreated};
}

std::pair<sema::Decl *, ESTree::Node *>
FlowChecker::specializeGenericWithParsedTypes(
    sema::Decl *oldDecl,
    SMRange errorRange,
    llvh::ArrayRef<Type *> typeArgTypes,
    sema::LexicalScope *scope) {
  GenericInfo<ESTree::Node> &generic = getGenericInfoMustExist(oldDecl);

  // A type argument may be a class whose specialization is still in progress,
  // so bound checks wait until the outermost specialization completes.
  llvh::SmallVector<GenericBoundCheck, 4> boundChecks{};
  bool ownsBoundChecks = !deferredGenericBoundChecks_;
  llvh::SaveAndRestore<llvh::SmallVectorImpl<GenericBoundCheck> *>
      savedBoundChecks{
          deferredGenericBoundChecks_,
          ownsBoundChecks ? &boundChecks : deferredGenericBoundChecks_};
  auto drainBoundChecks =
      llvh::make_scope_exit([this, ownsBoundChecks, &boundChecks]() {
        if (!ownsBoundChecks)
          return;
        for (const auto &check : boundChecks)
          validateGenericBound(check);
      });

  auto [specialization, wasNewlyCreated] =
      specializeGenericImpl(generic, scope, errorRange, typeArgTypes);
  if (!specialization) {
    return {nullptr, nullptr};
  }

  // Get the new Decl for the specialization.
  sema::Decl *newDecl = nullptr;
  if (auto *func =
          llvh::dyn_cast<ESTree::FunctionDeclarationNode>(specialization)) {
    newDecl = getDecl(llvh::cast<ESTree::IdentifierNode>(func->_id));
  } else if (
      auto *classDecl =
          llvh::dyn_cast<ESTree::ClassDeclarationNode>(specialization)) {
    newDecl = getDecl(llvh::cast<ESTree::IdentifierNode>(classDecl->_id));
  }
  newDecl->generic = false;

  // Typecheck the specialization if newly created.
  if (wasNewlyCreated) {
    TypeBindingTableScopePtrTy savedScope = bindingTable_.getCurrentScope();
    bindingTable_.activateScope(generic.bindingTableScope);
    auto onExit = llvh::make_scope_exit(
        [this, &savedScope]() { bindingTable_.activateScope(savedScope); });

    {
      // Put the parameter types in the binding table.
      ESTree::TypeParameterDeclarationNode *typeParamsNode = nullptr;
      if (auto *func =
              llvh::dyn_cast<ESTree::FunctionDeclarationNode>(specialization)) {
        typeParamsNode = llvh::cast<ESTree::TypeParameterDeclarationNode>(
            ESTree::getTypeParameters(func));
      } else if (
          auto *classDecl =
              llvh::dyn_cast<ESTree::ClassDeclarationNode>(specialization)) {
        typeParamsNode = llvh::cast<ESTree::TypeParameterDeclarationNode>(
            classDecl->_typeParameters);
      }

      assert(typeParamsNode);

      ScopeRAII paramScope{*this};
      bool populated = validateAndBindTypeParameters(
          typeParamsNode,
          errorRange,
          typeArgTypes,
          oldDecl->scope,
          [this](ESTree::Node *n) { return parseTypeAnnotation(n); });
      if (!populated) {
        LLVM_DEBUG(llvh::dbgs() << "Failed to bind type parameters\n");
        return {nullptr, nullptr};
      }

      if (auto *func =
              llvh::dyn_cast<ESTree::FunctionDeclarationNode>(specialization)) {
        typecheckGenericFunctionSpecialization(
            func, typeArgTypes, oldDecl, newDecl);
      } else if (
          auto *classDecl =
              llvh::dyn_cast<ESTree::ClassDeclarationNode>(specialization)) {
        typecheckGenericClassSpecialization(
            classDecl, typeArgTypes, oldDecl, newDecl);
      }
    }
    assert(flowContext_.findDeclType(newDecl) && "expected valid type");
  }

  // Register Array<T> specializations here so every path that creates or
  // reaches one is covered, including the Array body referencing its own
  // in-flight specialization.
  if (oldDecl == arrayClassDecl_) {
    if (Type *classConsType = flowContext_.findDeclType(newDecl)) {
      flowContext_.registerArrayClassType(
          llvh::cast<ClassConstructorType>(classConsType->info)->getClassType(),
          typeArgTypes[0]);
    }
  }

  return {newDecl, specialization};
}

void FlowChecker::resolveCallToGenericFunctionSpecialization(
    ESTree::CallExpressionNode *node,
    ESTree::IdentifierNode *callee,
    sema::Decl *oldDecl) {
  assert(oldDecl && "expected valid oldDecl");

  sema::Decl *newDecl = specializeGeneric(
      oldDecl,
      llvh::cast<ESTree::TypeParameterInstantiationNode>(node->_typeArguments),
      oldDecl->scope);

  if (newDecl) {
    semContext_.setExpressionDecl(callee, newDecl);
    setNodeType(callee, getDeclType(newDecl));
  }
}

void FlowChecker::resolveCallToGenericFunctionSpecializationWithParsedTypes(
    ESTree::CallExpressionNode *node,
    ESTree::IdentifierNode *callee,
    llvh::ArrayRef<Type *> typeArgTypes,
    sema::Decl *oldDecl) {
  assert(oldDecl && "expected valid oldDecl");

  sema::Decl *newDecl =
      specializeGenericWithParsedTypes(
          oldDecl, node->getSourceRange(), typeArgTypes, oldDecl->scope)
          .first;

  if (newDecl) {
    semContext_.setExpressionDecl(callee, newDecl);
    setNodeType(callee, getDeclType(newDecl));
  }
}

bool FlowChecker::inferPlaceholdersFromCallArgs(
    ESTree::TypeParameterDeclarationNode *typeParams,
    ESTree::NodeList &funcParams,
    ESTree::NodeList &callArgs,
    llvh::ArrayRef<Type *> typeArgs,
    sema::LexicalScope *bindScope,
    ESTree::Node *callNode,
    Type *receiverType,
    TypeBindingTableScopePtrTy paramParsingScope) {
  // Build argument constraints by binding type params and parsing param types.
  llvh::SmallVector<Type *, 2> callArgConstraints{};

  // If a separate scope is needed for parsing param types (e.g., TypedLib
  // methods where class type params like T in Array<T> are in a different
  // scope), activate it for the duration of param type parsing only.
  TypeBindingTableScopePtrTy savedScope;
  if (paramParsingScope) {
    savedScope = bindingTable_.getCurrentScope();
    bindingTable_.activateScope(paramParsingScope);
  }

  {
    ScopeRAII paramScope{*this};
    // Bind type parameters to our type arguments.
    size_t i = 0;
    for (ESTree::Node &paramNode : typeParams->_params) {
      auto *typeParam = llvh::cast<ESTree::TypeParameterNode>(&paramNode);
      bindingTable_.try_emplace(
          typeParam->_name, TypeDecl{typeArgs[i], bindScope, &paramNode});
      ++i;
    }

    // Parse remaining parameter types to get constraints.
    bool first = true;
    for (ESTree::Node &param : funcParams) {
      auto *ident = llvh::dyn_cast<ESTree::IdentifierNode>(&param);

      if (!ident) {
        // Cannot infer anything.
        return false;
      }

      // 'this' type annotation needs to be handled specially.
      // It can only be on the first param.
      if (first && ident->_name == kw_.identThis && ident->_typeAnnotation) {
        first = false;

        Type *thisParamType = parseTypeAnnotation(
            llvh::cast<ESTree::TypeAnnotationNode>(ident->_typeAnnotation)
                ->_typeAnnotation);

        if (receiverType)
          matchConstraintToType(thisParamType, receiverType);

        continue;
      }

      first = false;

      if (!ident->_typeAnnotation) {
        callArgConstraints.push_back(nullptr);
        continue;
      }
      callArgConstraints.push_back(parseTypeAnnotation(
          llvh::cast<ESTree::TypeAnnotationNode>(ident->_typeAnnotation)
              ->_typeAnnotation));
    }
  }

  // Restore the caller's scope before visiting call arguments, so
  // user-defined types are visible in callbacks.
  if (savedScope) {
    bindingTable_.activateScope(savedScope);
  }

  // Snapshot which constraints are simple (direct InferencePlaceholder),
  // since pass 1 will mutate them.
  llvh::BitVector isSimple(callArgConstraints.size());
  for (size_t i = 0, e = callArgConstraints.size(); i < e; ++i) {
    if (callArgConstraints[i] &&
        llvh::isa<InferencePlaceholderType>(callArgConstraints[i]->info))
      isSimple.set(i);
  }
  auto constraintAt = [&callArgConstraints](size_t paramIdx) -> Type * {
    return paramIdx < callArgConstraints.size() ? callArgConstraints[paramIdx]
                                                : nullptr;
  };

  // A tuple spread supplies one argument per element, so record the index of
  // the param each call argument starts at. A spread's width is only known
  // once it has been typed, so visit spreads first, unconstrained, and match
  // their element types against the params they supply directly.
  // Any other spread counts as one argument; checkCallArgumentTypes() rejects
  // it after inference.
  llvh::SmallVector<size_t, 4> firstParamIdx;
  size_t numFlatArgs = 0;
  for (ESTree::Node &arg : callArgs) {
    firstParamIdx.push_back(numFlatArgs);
    auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(&arg);
    if (!spread) {
      ++numFlatArgs;
      continue;
    }
    visitExpression(spread, callNode, nullptr);
    auto *tupleTy =
        llvh::dyn_cast<TupleType>(getNodeTypeOrAny(spread->_argument)->info);
    if (!tupleTy) {
      ++numFlatArgs;
      continue;
    }
    for (Type *elemType : tupleTy->getTypes())
      matchConstraintToType(constraintAt(numFlatArgs++), elemType);
  }

  // Pass 1: visit arguments with simple placeholder constraints (bare T).
  // Resolving these first allows complex constraints referencing the same
  // type params (e.g., T => string) to use the resolved type in pass 2,
  // even if the simple placeholder comes after the complex constraint.
  {
    size_t argIdx = 0;
    for (ESTree::Node &arg : callArgs) {
      size_t paramIdx = firstParamIdx[argIdx++];
      if (!llvh::isa<ESTree::SpreadElementNode>(&arg) &&
          paramIdx < isSimple.size() && isSimple[paramIdx])
        visitExpression(&arg, callNode, callArgConstraints[paramIdx]);
    }
    // Infer void for simple placeholder params with no corresponding arg.
    for (size_t i = numFlatArgs, e = callArgConstraints.size(); i < e; ++i) {
      if (isSimple[i])
        callArgConstraints[i]->info = flowContext_.getVoidInfo();
    }
  }

  // Pass 2: visit remaining arguments with complex constraints.
  {
    size_t argIdx = 0;
    for (ESTree::Node &arg : callArgs) {
      size_t paramIdx = firstParamIdx[argIdx++];
      if (!llvh::isa<ESTree::SpreadElementNode>(&arg) &&
          !(paramIdx < isSimple.size() && isSimple[paramIdx]))
        visitExpression(&arg, callNode, constraintAt(paramIdx));
    }
  }

  // Inference succeeded iff none of the typeArgs are still
  // InferencePlaceholderType.
  return llvh::none_of(typeArgs, [](Type *typeArg) -> bool {
    return llvh::isa<InferencePlaceholderType>(typeArg->info);
  });
}

Type *FlowChecker::getSpecializedArrayClassType(
    Type *elementType,
    SMRange errorRange) {
  if (!arrayClassDecl_) {
    return nullptr;
  }
  // Avoid specializing if the element type is not concrete (e.g., it is a
  // generic type parameter), to prevent issues during recursive typechecking
  // of the Array class body.
  if (llvh::isa<GenericType>(elementType->info) ||
      llvh::isa<InferencePlaceholderType>(elementType->info)) {
    return nullptr;
  }
  auto [newDecl, specialization] = specializeGenericWithParsedTypes(
      arrayClassDecl_, errorRange, {elementType}, arrayClassDecl_->scope);
  if (!newDecl)
    return nullptr;
  Type *classConsType = getDeclType(newDecl);
  return llvh::cast<ClassConstructorType>(classConsType->info)->getClassType();
}

std::pair<Type *, const ClassType::Field *> FlowChecker::lookupPropertyOnClass(
    ClassType *classType,
    Identifier propName,
    ESTree::Node *propNode) {
  bool isPrivate = llvh::isa<ESTree::PrivateNameNode>(propNode);
  auto optField = isPrivate ? classType->findPrivateField(propName)
                            : classType->findPublicField(propName);
  if (optField)
    return {optField->getField()->type, optField->getField()};
  auto *homeObj = classType->getHomeObjectTypeInfo();
  if (!homeObj)
    return {nullptr, nullptr};
  auto optMethod = isPrivate ? homeObj->findPrivateField(propName)
                             : homeObj->findPublicField(propName);
  if (optMethod) {
    const auto *field = optMethod->getField();
    // For overloaded methods, type is null. Callers must handle this
    // and perform overload resolution at the call site.
    Type *type = field->type;

    if (field->isAccessor()) {
      // Accessor: result type is the field type (getter return type, or
      // void for setter-only). Do NOT propagate the Decl — IRGen handles
      // accessor calls via the Field directly.
      return {type, field};
    }

    // For non-generic, non-overloaded final methods, propagate the Decl
    // from the method definition key to the call-site property so IRGen
    // can look it up. Generic final methods get their Decls set
    // on the call-site property through specializedMethodDecls_.
    // Overloaded methods get their Decls set during overload resolution
    // in the CallExpression visitor.
    if (field->finalMethod && !field->isOverloaded() &&
        !llvh::isa<GenericType>(type->info)) {
      auto *methodKey = ESTree::getPropertyIdentifier(field->method->_key);
      if (auto *decl = semContext_.getExpressionDecl(methodKey)) {
        auto *idNode = ESTree::getPropertyIdentifier(propNode);
        semContext_.setExpressionDecl(idNode, decl);
      }
    }
    assert(
        (field->isOverloaded() || llvh::isa<BaseFunctionType>(type->info) ||
         llvh::isa<GenericType>(type->info)) &&
        "methods must be functions, generic, or overloaded");
    return {type, field};
  }
  return {nullptr, nullptr};
}

bool FlowChecker::resolveBuiltinMethodCall(
    ESTree::CallExpressionNode *call,
    ESTree::MemberExpressionNode *callee,
    ESTree::FunctionDeclarationNode *builtinFunc) {
  assert(!callee->_computed && "computed properties not allowed for builtin");
  assert(
      !builtinFunc->_typeParameters &&
      "generic builtins should have been rejected");

  // Parse function type directly (no generic substitution needed).
  auto &params = ESTree::getParams(builtinFunc);
  auto *returnType = ESTree::getReturnType(builtinFunc);
  assert(returnType && "builtin must have return type annotation");
  Type *funcType = parseFunctionType(
      params,
      returnType,
      ESTree::isAsync(builtinFunc),
      ESTree::isGenerator(builtinFunc));

  setNodeType(callee, funcType);

  // Register for direct call generation in IRGen.
  flowContext_.setBuiltinCallTarget(call, builtinFunc);
  // Arguments are visited by the caller.
  return false;
}

std::pair<bool, llvh::SmallVector<Type *, 2>>
FlowChecker::inferTypeArgumentsForGenericFunctionCall(
    ESTree::CallExpressionNode *node,
    ESTree::IdentifierNode *callee,
    sema::Decl *oldDecl) {
  // Retrieve generic info.
  GenericInfo<ESTree::Node> &generic = getGenericInfoMustExist(oldDecl);
  auto *genericDecl =
      llvh::dyn_cast<ESTree::FunctionDeclarationNode>(generic.originalNode);
  if (!genericDecl)
    return {false, {}};

  auto *typeParams = llvh::cast<ESTree::TypeParameterDeclarationNode>(
      genericDecl->_typeParameters);
  size_t numTypeParams = typeParams->_params.size();
  assert(numTypeParams > 0 && "expected at least one type parameter");

  // Seed the leading type arguments with any explicitly provided ones (parsed
  // in the call-site scope), and fill the trailing ones with placeholders to be
  // inferred from the call arguments.
  llvh::SmallVector<Type *, 2> typeArgs =
      seedTypeArgsForInference(node->_typeArguments, numTypeParams);

  // Infer placeholders from call arguments.
  if (!inferPlaceholdersFromCallArgs(
          typeParams,
          ESTree::getParams(genericDecl),
          node->_arguments,
          typeArgs,
          oldDecl->scope,
          node)) {
    if (node->_typeArguments)
      reportUninferredTypeArg(typeParams, typeArgs, node->_typeArguments);
    return {true, {}};
  }

  return {true, std::move(typeArgs)};
}

llvh::SmallVector<Type *, 2> FlowChecker::seedTypeArgsForInference(
    ESTree::Node *typeArgsNode,
    size_t numTypeParams) {
  // Explicit type arguments are parsed in the call-site scope.
  llvh::SmallVector<Type *, 2> typeArgs{};
  if (auto *node = llvh::cast_or_null<ESTree::TypeParameterInstantiationNode>(
          typeArgsNode)) {
    for (ESTree::Node &arg : node->_params)
      typeArgs.push_back(parseTypeAnnotation(&arg));
  }
  assert(
      typeArgs.size() < numTypeParams &&
      "explicit type args must be partial when inferring");
  while (typeArgs.size() < numTypeParams) {
    typeArgs.push_back(
        flowContext_.createType(flowContext_.getInferencePlaceholderInfo()));
  }
  return typeArgs;
}

void FlowChecker::reportUninferredTypeArg(
    ESTree::TypeParameterDeclarationNode *typeParams,
    llvh::ArrayRef<Type *> typeArgs,
    ESTree::Node *typeArgsNode) {
  size_t i = 0;
  for (ESTree::Node &param : typeParams->_params) {
    if (llvh::isa<InferencePlaceholderType>(typeArgs[i++]->info)) {
      sm_.error(
          typeArgsNode->getSourceRange(),
          llvh::Twine(
              "ft: could not infer type argument for type parameter '") +
              llvh::cast<ESTree::TypeParameterNode>(&param)->_name->str() +
              "'");
      return;
    }
  }
}

std::pair<bool, llvh::SmallVector<Type *, 2>>
FlowChecker::inferTypeArgumentsForGenericClassNew(
    ESTree::NewExpressionNode *node,
    sema::Decl *oldDecl) {
  GenericInfo<ESTree::Node> &generic = getGenericInfoMustExist(oldDecl);
  auto *classDecl =
      llvh::dyn_cast<ESTree::ClassDeclarationNode>(generic.originalNode);
  if (!classDecl)
    return {false, {}};

  // Inference uses the explicit constructor's params; an implicit one has none
  // that mention the class's type parameters.
  ESTree::FunctionExpressionNode *ctor = nullptr;
  for (ESTree::Node &member :
       llvh::cast<ESTree::ClassBodyNode>(classDecl->_body)->_body) {
    auto *method = llvh::dyn_cast<ESTree::MethodDefinitionNode>(&member);
    if (method && method->_kind == kw_.identConstructor) {
      ctor = llvh::cast<ESTree::FunctionExpressionNode>(method->_value);
      break;
    }
  }
  if (!ctor)
    return {false, {}};

  auto *typeParams = llvh::cast<ESTree::TypeParameterDeclarationNode>(
      classDecl->_typeParameters);
  llvh::SmallVector<Type *, 2> typeArgs = seedTypeArgsForInference(
      node->_typeArguments, typeParams->_params.size());

  // Parse the constructor's param types in the class's declaration scope.
  if (!inferPlaceholdersFromCallArgs(
          typeParams,
          ctor->_params,
          node->_arguments,
          typeArgs,
          oldDecl->scope,
          node,
          /*receiverType=*/nullptr,
          generic.bindingTableScope)) {
    reportUninferredTypeArg(typeParams, typeArgs, node->_typeArguments);
    return {true, {}};
  }

  return {true, std::move(typeArgs)};
}

void FlowChecker::typecheckGenericFunctionSpecialization(
    ESTree::FunctionDeclarationNode *specialization,
    llvh::ArrayRef<Type *> typeArgTypes,
    sema::Decl *oldDecl,
    sema::Decl *newDecl) {
  LLVM_DEBUG(
      llvh::dbgs() << "Typechecking generic function specialization: "
                   << newDecl->name.str() << "\n");

  // Record the type of the specialization.
  Type *ftype = parseFunctionType(
      ESTree::getParams(specialization),
      ESTree::getReturnType(specialization),
      ESTree::isAsync(specialization),
      ESTree::isGenerator(specialization));
  setNodeType(specialization, ftype);
  // Specializations of function declarations always have an Identifier name.
  auto *ident =
      llvh::cast<ESTree::IdentifierNode>(ESTree::getIdentifier(specialization));
  recordDecl(newDecl, ftype, ident, specialization);

  // Run typechecker.
  visitNonGenericFunctionDeclaration(specialization, newDecl);
}

void FlowChecker::typecheckGenericClassSpecialization(
    ESTree::ClassDeclarationNode *specialization,
    llvh::ArrayRef<Type *> typeArgTypes,
    sema::Decl *oldDecl,
    sema::Decl *newDecl) {
  // Create the new type of the specialization.
  auto *id = llvh::cast<ESTree::IdentifierNode>(specialization->_id);
  assert(newDecl == getDecl(id) && "expected same decl");
  Type *classType = flowContext_.createType(
      flowContext_.createClass(
          Identifier::getFromPointer(id->_name), typeArgTypes),
      specialization);
  Type *classConsType = flowContext_.createType(
      flowContext_.createClassConstructor(classType), specialization);

  bool recorded = recordDecl(newDecl, classConsType, id, specialization);
  assert(recorded && "class constructor unexpectedly re-declared");
  (void)recorded;

  // Visit the super class first so that it gets enqueued/deferred before the
  // child class.
  visitExpression(specialization->_superClass, specialization, nullptr);

  if (deferredParseGenerics_) {
    LLVM_DEBUG(
        llvh::dbgs() << "Deferring parsing of generic class specialization: "
                     << newDecl->name.str() << "\n");
    deferredParseGenerics_->emplace_back(
        specialization,
        bindingTable_.getCurrentScope(),
        classType,
        classConsType,
        specialization->getScope());
  } else {
    bool success = parseClassType(
        specialization->_superClass,
        specialization->_superTypeArguments,
        specialization->_body,
        classType,
        specialization->getScope(),
        classConsType,
        specialization->_typeParameters);
    if (success) {
      // Don't bother typechecking if parsing failed.
      typecheckQueue_.emplace_back(
          specialization,
          bindingTable_.getCurrentScope(),
          classType,
          classConsType,
          specialization->getScope());
    }
  }
}

Type *FlowChecker::resolveGenericClassSpecialization(
    ESTree::IdentifierNode *nameNode,
    ESTree::TypeParameterInstantiationNode *typeArgsNode,
    sema::Decl *oldDecl) {
  assert(oldDecl && "expected valid oldDecl");

  sema::Decl *newDecl =
      specializeGeneric(oldDecl, typeArgsNode, oldDecl->scope);

  if (!newDecl)
    return flowContext_.getAny();

  Type *classConsType = flowContext_.findDeclType(newDecl);
  if (!classConsType) {
    // Still in-flight: a bound re-entered it through a superclass
    // (e.g. "class A<T: B<Base>>; class B<T> extends A<T>").
    sm_.error(
        nameNode->getSourceRange(),
        "ft: type parameter bound cannot reference the generic being declared");
    return flowContext_.getAny();
  }
  // Set these unconditionally, but it's not actually necessary if this is
  // called from a GenericTypeAnnotation.
  semContext_.setExpressionDecl(nameNode, newDecl);
  setNodeType(nameNode, classConsType);
  return llvh::cast<ClassConstructorType>(classConsType->info)->getClassType();
}

Type *FlowChecker::resolveGenericClassSpecializationWithParsedTypes(
    ESTree::IdentifierNode *nameNode,
    SMRange errorRange,
    llvh::ArrayRef<Type *> typeArgTypes,
    sema::Decl *oldDecl) {
  sema::Decl *newDecl = specializeGenericWithParsedTypes(
                            oldDecl, errorRange, typeArgTypes, oldDecl->scope)
                            .first;
  if (!newDecl)
    return flowContext_.getAny();
  Type *classConsType = getDeclType(newDecl);
  semContext_.setExpressionDecl(nameNode, newDecl);
  setNodeType(nameNode, classConsType);
  return llvh::cast<ClassConstructorType>(classConsType->info)->getClassType();
}

Type *FlowChecker::resolveGenericClassSpecializationForType(
    ESTree::GenericTypeAnnotationNode *genericTypeNode,
    sema::Decl *oldDecl) {
  assert(oldDecl && "expected valid oldDecl");

  auto *typeArgsNode = llvh::cast<ESTree::TypeParameterInstantiationNode>(
      genericTypeNode->_typeParameters);

  sema::Decl *newDecl =
      specializeGeneric(oldDecl, typeArgsNode, oldDecl->scope);

  if (!newDecl)
    return flowContext_.getAny();

  Type *classConsType = flowContext_.findDeclType(newDecl);
  if (!classConsType) {
    // The specialization is still in-flight: reaching it again means its own
    // bound re-entered it (e.g. "class Node<T: Node<Base>>"). A bound may not
    // reference the generic being declared, so report it instead of silently
    // resolving to "any" (which would accept any argument).
    sm_.error(
        genericTypeNode->getSourceRange(),
        "ft: type parameter bound cannot reference the generic being declared");
    return flowContext_.getAny();
  }
  return llvh::cast<ClassConstructorType>(classConsType->info)->getClassType();
}

void FlowChecker::registerGeneric(
    sema::Decl *decl,
    ESTree::Node *node,
    ESTree::Node *parent,
    const TypeBindingTableScopePtrTy &bindingScope) {
  assert(decl->generic && "expected a generic decl");
  GenericInfo<ESTree::Node> &info =
      generics_.emplace_back(node, parent, bindingScope);
  auto [it, inserted] = genericsMap_.try_emplace(decl, &info);
  assert(inserted && "duplicate generic decl");
  (void)it;
}

void FlowChecker::registerGenericAlias(
    ESTree::TypeAliasNode *node,
    ESTree::Node *parent,
    const TypeBindingTableScopePtrTy &bindingScope) {
  GenericInfo<Type> &info =
      genericAliases_.emplace_back(node, parent, bindingScope);
  auto [it, inserted] = genericAliasesMap_.try_emplace(node, &info);
  assert(inserted && "duplicate generic decl");
  (void)it;
}

/// \return the generic info associated with \p decl.
FlowChecker::GenericInfo<ESTree::Node> &FlowChecker::getGenericInfoMustExist(
    sema::Decl *decl) {
  auto it = genericsMap_.find(decl);
  assert(it != genericsMap_.end() && "generic was never registered");
  return *it->second;
}

/// \return the generic info associated with \p decl.
FlowChecker::GenericInfo<Type> &FlowChecker::getGenericAliasInfoMustExist(
    const ESTree::TypeAliasNode *decl) {
  auto it = genericAliasesMap_.find(decl);
  assert(it != genericAliasesMap_.end() && "generic was never registered");
  return *it->second;
}

void FlowChecker::registerGenericMethod(
    ESTree::MethodDefinitionNode *method,
    ESTree::ClassBodyNode *classBody,
    const TypeBindingTableScopePtrTy &bindingScope,
    sema::LexicalScope *classScope,
    flow::ClassType *classTypeInfo,
    ESTree::TypeParameterDeclarationNode *classTypeParams) {
  assert(classScope && "expected class scope");
  assert(
      llvh::cast<ESTree::FunctionExpressionNode>(method->_value)
          ->_typeParameters &&
      "expected generic method");
  GenericInfo<ESTree::Node> &info =
      generics_.emplace_back(method, classBody, bindingScope);
  info.classScope = classScope;
  info.containingClassType = classTypeInfo;
  info.classTypeParams = classTypeParams;
  [[maybe_unused]] auto [it, inserted] =
      genericMethodsMap_.try_emplace(method, &info);
  assert(inserted && "duplicate generic method");
}

FlowChecker::GenericInfo<ESTree::Node> &
FlowChecker::getGenericMethodInfoMustExist(
    const ESTree::MethodDefinitionNode *method) {
  auto it = genericMethodsMap_.find(method);
  assert(
      it != genericMethodsMap_.end() && "generic method was never registered");
  return *it->second;
}

void FlowChecker::resolveCallToGenericMethodSpecialization(
    ESTree::CallExpressionNode *node,
    ESTree::MemberExpressionNode *callee,
    ESTree::MethodDefinitionNode *method) {
  assert(method && "expected valid method");

  // Parse the type arguments.
  auto *typeArgsNode =
      llvh::cast<ESTree::TypeParameterInstantiationNode>(node->_typeArguments);
  llvh::SmallVector<Type *, 2> typeArgTypes{};
  for (ESTree::Node &arg : typeArgsNode->_params) {
    Type *type = parseTypeAnnotation(&arg);
    typeArgTypes.push_back(type);
  }

  auto result = specializeGenericMethodWithParsedTypes(
      method, node->getSourceRange(), typeArgTypes);

  if (result.type) {
    setNodeType(callee, result.type);
    // Record the specialized method's Decl on the property IdentifierNode so
    // IRGen can look it up.
    semContext_.setExpressionDecl(
        ESTree::getPropertyIdentifier(callee->_property), result.decl);
  }
}

void FlowChecker::resolveCallToGenericMethodSpecializationWithParsedTypes(
    ESTree::CallExpressionNode *node,
    ESTree::MemberExpressionNode *callee,
    llvh::ArrayRef<Type *> typeArgTypes,
    ESTree::MethodDefinitionNode *method) {
  assert(method && "expected valid method");

  auto result = specializeGenericMethodWithParsedTypes(
      method, node->getSourceRange(), typeArgTypes);

  if (result.type) {
    setNodeType(callee, result.type);
    // Record the specialized method's Decl on the property IdentifierNode so
    // IRGen can look it up.
    semContext_.setExpressionDecl(
        ESTree::getPropertyIdentifier(callee->_property), result.decl);
  }
}

std::pair<bool, llvh::SmallVector<Type *, 2>>
FlowChecker::inferTypeArgumentsForGenericMethodCall(
    ESTree::CallExpressionNode *node,
    ESTree::MemberExpressionNode *callee,
    ESTree::MethodDefinitionNode *method) {
  // Retrieve generic info.
  GenericInfo<ESTree::Node> &generic = getGenericMethodInfoMustExist(method);
  auto *methodNode =
      llvh::cast<ESTree::MethodDefinitionNode>(generic.originalNode);
  auto *fe = llvh::cast<ESTree::FunctionExpressionNode>(methodNode->_value);

  auto *typeParams =
      llvh::cast<ESTree::TypeParameterDeclarationNode>(fe->_typeParameters);
  size_t numTypeParams = typeParams->_params.size();
  assert(numTypeParams > 0 && "expected at least one type parameter");

  // Seed the leading type arguments with any explicitly provided ones (parsed
  // in the call-site scope), and fill the trailing ones with placeholders to be
  // inferred from the call arguments.
  llvh::SmallVector<Type *, 2> typeArgs =
      seedTypeArgsForInference(node->_typeArguments, numTypeParams);

  // Get the object type to use for receiver type inference.
  Type *objType = getNodeTypeOrAny(callee->_object);

  // Pass the binding table scope from when the generic method was registered,
  // so that class type parameters (e.g. T in Array<T>) are visible when
  // parsing the method's parameter type annotations. The scope is only
  // activated during param type parsing and restored before visiting call
  // arguments, so user-defined types remain visible in callbacks.

  // Infer placeholders from call arguments.
  if (!inferPlaceholdersFromCallArgs(
          typeParams,
          fe->_params,
          node->_arguments,
          typeArgs,
          generic.classScope,
          node,
          objType,
          generic.bindingTableScope)) {
    if (node->_typeArguments)
      reportUninferredTypeArg(typeParams, typeArgs, node->_typeArguments);
    return {true, {}};
  }

  return {true, std::move(typeArgs)};
}

FlowChecker::GenericMethodSpecializationResult
FlowChecker::specializeGenericMethodWithParsedTypes(
    ESTree::MethodDefinitionNode *method,
    SMRange errorRange,
    llvh::ArrayRef<Type *> typeArgTypes) {
  GenericInfo<ESTree::Node> &generic = getGenericMethodInfoMustExist(method);
  assert(
      generic.containingClassType &&
      "generic method must have been registered with its class");

  auto [specialization, wasNewlyCreated] = specializeGenericImpl(
      generic, generic.classScope, errorRange, typeArgTypes);
  if (!specialization) {
    return {};
  }

  auto *specializedMethod =
      llvh::cast<ESTree::MethodDefinitionNode>(specialization);
  auto *specializedFE =
      llvh::cast<ESTree::FunctionExpressionNode>(specializedMethod->_value);

  Type *ftype = nullptr;
  sema::Decl *decl = nullptr;
  if (wasNewlyCreated) {
    TypeBindingTableScopePtrTy savedScope = bindingTable_.getCurrentScope();
    bindingTable_.activateScope(generic.bindingTableScope);
    auto onExit = llvh::make_scope_exit(
        [this, &savedScope]() { bindingTable_.activateScope(savedScope); });

    auto *typeParamsNode = llvh::cast<ESTree::TypeParameterDeclarationNode>(
        specializedFE->_typeParameters);

    // Static methods can't reference the class type parameters. Shadow them
    // in an outer scope so the method's own type parameters still win.
    ScopeRAII staticTypeParamScope{*this};
    if (specializedMethod->_static)
      shadowClassTypeParams(generic.classTypeParams, generic.classScope);

    ScopeRAII paramScope{*this};
    if (!validateAndBindTypeParameters(
            typeParamsNode,
            errorRange,
            typeArgTypes,
            generic.classScope,
            [this](ESTree::Node *n) { return parseTypeAnnotation(n); })) {
      LLVM_DEBUG(llvh::dbgs() << "Failed to bind type parameters\n");
      return {};
    }

    // 'this' and 'super' in the body describe the class that declares the
    // method.
    Type *declConsType = generic.containingClassType->getClassConsType();
    assert(declConsType && "declaring class must have a constructor type");
    Type *declClassType =
        llvh::cast<ClassConstructorType>(declConsType->info)->getClassType();

    ftype = parseFunctionType(
        specializedFE->_params,
        specializedFE->_returnType,
        specializedFE->_async,
        specializedFE->_generator,
        nullptr,
        specializedMethod->_static ? declConsType : declClassType);
    setNodeType(specializedMethod, ftype);
    setNodeType(specializedFE, ftype);

    // Create a Decl for the specialized method and register it with the
    // containing ClassType. This allows IRGen to store the compiled Function
    // in the Decl's customData.
    auto *methodId = ESTree::getPropertyIdentifier(specializedMethod->_key);
    decl = semContext_.newDeclInScope(
        methodId->_name, sema::Decl::Kind::Const, generic.classScope);
    generic.containingClassType->addSpecializedMethodDecl(
        specializedMethod, decl);

    // Establish a ClassContext so that references to 'this', 'super',
    // and class type information are available during typechecking of the
    // specialized method body.
    ClassContext classContext(*this, declClassType, nullptr);
    FunctionContext functionContext(
        *this,
        specializedFE,
        ftype,
        classContext.getThisType(specializedMethod->_static),
        flowContext_.getVoid(),
        classContext.getSuperType(specializedMethod->_static));
    visitFunctionLike(
        specializedFE, specializedFE->_body, specializedFE->_params);
  } else {
    ftype = getNodeTypeOrAny(specializedMethod);
    // Look up the existing Decl for this specialization.
    decl = generic.containingClassType->findSpecializedMethodDecl(
        specializedMethod);
  }

  return {ftype, decl};
}

UniqueString *FlowChecker::propertyKeyAsIdentifier(ESTree::Node *Key) {
  // Handle String Literals.
  // http://www.ecma-international.org/ecma-262/6.0/#sec-literals-string-literals
  if (auto *Lit = llvh::dyn_cast<ESTree::StringLiteralNode>(Key)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading String Literal \"" << Lit->_value << "\"\n");
    return Lit->_value;
  }

  // Handle identifiers as if they are String Literals.
  if (auto *Iden = llvh::dyn_cast<ESTree::IdentifierNode>(Key)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading String Literal \"" << Iden->_name << "\"\n");
    return Iden->_name;
  }

  // Handle Number Literals.
  // http://www.ecma-international.org/ecma-262/6.0/#sec-literals-numeric-literals
  if (auto *Lit = llvh::dyn_cast<ESTree::NumericLiteralNode>(Key)) {
    LLVM_DEBUG(
        llvh::dbgs() << "Loading Numeric Literal \"" << Lit->_value << "\"\n");
    char buf[NUMBER_TO_STRING_BUF_SIZE];
    return astContext_
        .getIdentifier(numberToString(Lit->_value, buf, sizeof(buf)))
        .getUnderlyingPointer();
  }

  return nullptr;
}

FieldVariance FlowChecker::parseVariance(ESTree::VarianceNode *varianceNode) {
  if (!varianceNode)
    return FieldVariance::None;
  UniqueString *kind = varianceNode->_kind;
  if (kind == kw_.identFlowPlus || kind == kw_.identReadonly)
    return FieldVariance::ReadOnly;
  if (kind == kw_.identFlowMinus || kind == kw_.identWriteonly)
    return FieldVariance::WriteOnly;
  // "in" / "out" are type-parameter variances and have no meaning here.
  sm_.error(
      varianceNode->getSourceRange(),
      "ft: '" + kind->str() + "' is not a valid field variance");
  return FieldVariance::None;
}

bool FlowChecker::classTypeIsEnclosing(ClassType *classType) {
  // Loop through lexically enclosing class contexts.
  for (const ClassContext *cur = curClassContext_; cur != nullptr;
       cur = cur->getPrevContext()) {
    auto *curClass = cur->getClassTypeInfo();
    if (classType == curClass) {
      return true;
    }
  }
  return false;
}

} // namespace flow
} // namespace hermes

#endif // HERMES_PARSE_FLOW
