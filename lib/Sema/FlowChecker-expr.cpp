/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// Typechecking visitors for expressions.
///
/// Expressions can have types inferred by using a 'constraint' type provided to
/// the visitExpression function. The constraint type is propagated through the
/// AST by the ExprVisitor, allowing us to recursively match parts of the
/// constraint and choose which types to emit for nested expressions.
/// The visitor may emit errors for expressions which aren't able to match the
/// constraint when it is provided, and may add implicitCheckedCasts in the AST
/// when needed to match the constraint.
/// However, the caller that provided the constraint cannot rely on the actual
/// type of the checked node being equal to the type of the constraint that was
/// passed in, so the caller still needs to check the type and report errors or
/// insert its own casts itself.
///
/// Write positions of `=` and destructuring patterns are walked through
/// `ExprVisitor::visitAssignmentTarget`, which recognizes the outermost
/// MemberExpression at each pattern leaf and tells the member-access path
/// it is a pure write. Inner subexpressions (`a.b` inside `a.b.c = ...`)
/// flow through the normal read visit. The other write cases (compound
/// assignment, `++`/`--`, `delete`, for-in/of LHS) can't destructure, so
/// `classifyMemberAccess` does a bottom-up parent check on the read path.
//===----------------------------------------------------------------------===//

#include "FlowChecker.h"

#if HERMES_PARSE_FLOW
#define DEBUG_TYPE "FlowChecker"

namespace hermes {
namespace flow {

void FlowChecker::matchConstraintToType(
    Type *startConstraint,
    const Type *startType) {
  // Use worklist to avoid recursion, with SetVector to avoid revisiting
  // looping types.
  llvh::SmallSetVector<std::pair<Type *, const Type *>, 32> worklist;
  worklist.insert({startConstraint, startType});

  for (size_t i = 0; i < worklist.size(); ++i) {
    auto [constraint, type] = worklist[i];
    if (!constraint || !type)
      continue;

    if (llvh::isa<InferencePlaceholderType>(constraint->info)) {
      // Found a placeholder, replace it with the type that matches it. Widen
      // fresh literal types so that e.g. head(['a', 'b']) infers T = string,
      // not T = "a".
      TypeKind kind = widenLiteralKind(type->info->getKind());
      constraint->info = type->info->isLiteral()
          ? flowContext_.getSingletonType(kind)->info
          : type->info;
      continue;
    }

    // It's possible at this point that the type used to be a placeholder, but
    // has been replaced with a non-placeholder. If that happened, then we
    // won't be able to infer a potentially conflicting type for the
    // placeholder.
    // For example, if we try to match (T is a placeholder):
    //   constraint=[T,T]
    //   type=[number,string]
    // it'll set T to number, and we won't be able to infer string for T.
    // Meaning that we'll always need to check the argument types
    // at the CallExpression visitor.

    if (constraint->info->getKind() != type->info->getKind()) {
      // Handle InferencePlaceholderArray constraint matched against Array
      // class type. This occurs during type inference when T[] is parsed as
      // InferencePlaceholderArrayType (because the placeholder can't be
      // specialized), but the actual argument is an Array<T> class
      // specialization.
      if (auto *constraintArr =
              llvh::dyn_cast<InferencePlaceholderArrayType>(constraint->info)) {
        if (flowContext_.isArrayClassType(type)) {
          Type *constraintElem = constraintArr->getElement();
          // Resolve the InferencePlaceholderArrayType to the actual array
          // class type.
          constraint->info = type->info;
          worklist.insert(
              {constraintElem, flowContext_.getArrayElementType(type)});
          continue;
        }
      }
      // When the constraint is a Union containing placeholders and the
      // actual type is not a Union, try to resolve placeholder arms by
      // matching them against the actual type.
      if (auto *constraintUnion = llvh::dyn_cast<UnionType>(constraint->info)) {
        for (Type *arm : constraintUnion->getTypes()) {
          worklist.insert({arm, type});
        }
        continue;
      }
      // Mismatch, nothing more to do.
      continue;
    }

    switch (type->info->getKind()) {
      // Singleton types have nothing to visit.
#define _HERMES_SEMA_FLOW_DEFKIND(name) \
  case TypeKind::name:                  \
    continue;
      _HERMES_SEMA_FLOW_SINGLETONS

      // Classes are nominally typed.
      case TypeKind::Class: {
        // If both sides are Array<T> specializations, recurse into the
        // element types so inference placeholders can be matched.
        if (flowContext_.isArrayClassType(constraint) &&
            flowContext_.isArrayClassType(type)) {
          worklist.insert(
              {flowContext_.getArrayElementType(constraint),
               flowContext_.getArrayElementType(type)});
        }
        continue;
      }
      case TypeKind::ClassConstructor:
      // Functions that we don't infer the types of here.
      case TypeKind::NativeFunction:
      case TypeKind::UntypedFunction:
      // InferencePlaceholderArray is only used as constraints, never as
      // actual types, so both sides having this kind shouldn't happen.
      case TypeKind::InferencePlaceholderArray:
      // Literals carry no nested types to match.
      case TypeKind::StringLiteral:
      case TypeKind::NumberLiteral:
      case TypeKind::BooleanLiteral:
        continue;

      case TypeKind::Union:
        // TODO: Determine how to match unions here, because any arm could
        // match any other arm, recursively. There's also potential for
        // looping union arms.
        continue;

      case TypeKind::Array: {
        // Visit the array element.
        auto *constraintArr = llvh::cast<ArrayType>(constraint->info);
        auto *typeArr = llvh::cast<ArrayType>(type->info);
        worklist.insert({constraintArr->getElement(), typeArr->getElement()});
        continue;
      }

      case TypeKind::Tuple: {
        // Visit the tuple elements.
        auto *constraintTuple = llvh::cast<TupleType>(constraint->info);
        auto *typeTuple = llvh::cast<TupleType>(type->info);
        if (constraintTuple->getTypes().size() !=
            typeTuple->getTypes().size()) {
          continue;
        }

        for (size_t i = 0, e = constraintTuple->getTypes().size(); i < e; ++i) {
          worklist.insert(
              {constraintTuple->getTypes()[i], typeTuple->getTypes()[i]});
        }
        continue;
      }

      case TypeKind::TypedFunction: {
        auto *constraintFn = llvh::cast<TypedFunctionType>(constraint->info);
        auto *typeFn = llvh::cast<TypedFunctionType>(type->info);
        // Match the common params and return type to resolve placeholders.
        // Param count mismatches are allowed here because inference only
        // needs to extract placeholder bindings; flowing compatibility is
        // checked separately.
        size_t minParams = std::min(
            constraintFn->getParams().size(), typeFn->getParams().size());

        worklist.insert({constraintFn->getThisParam(), typeFn->getThisParam()});
        for (size_t i = 0; i < minParams; ++i) {
          worklist.insert(
              {constraintFn->getParams()[i].type, typeFn->getParams()[i].type});
        }
        worklist.insert(
            {constraintFn->getReturnType(), typeFn->getReturnType()});
        continue;
      }

      case TypeKind::ExactObject: {
        // Visit the object properties.
        auto *constraintObj = llvh::cast<ExactObjectType>(constraint->info);
        auto *typeObj = llvh::cast<ExactObjectType>(type->info);
        if (constraintObj->getFields().size() != typeObj->getFields().size()) {
          continue;
        }

        for (size_t i = 0, e = constraintObj->getFields().size(); i < e; ++i) {
          worklist.insert(
              {constraintObj->getFields()[i].type,
               typeObj->getFields()[i].type});
        }
        continue;
      }

    } // switch
  }
}

class FlowChecker::ExprVisitor {
  FlowChecker &outer_;

 public:
  explicit ExprVisitor(FlowChecker &outer) : outer_(outer) {}

  bool incRecursionDepth(ESTree::Node *n) {
    return outer_.incRecursionDepth(n);
  }
  void decRecursionDepth() {
    return outer_.decRecursionDepth();
  }

  /// Default case for all ignored nodes, we still want to visit their children.
  void visit(ESTree::Node *node, ESTree::Node *parent, Type *constraint) {
    if (0) {
      LLVM_DEBUG(
          llvh::dbgs() << "Unsupported node " << node->getNodeName()
                       << " in expr context\n");
      llvm_unreachable("invalid node in expression context");
    } else {
      outer_.sm_.warning(
          node->getSourceRange(),
          "ft: expression not supported, assuming 'any'");
      visitESTreeChildren(*this, node, nullptr);
      outer_.setNodeType(node, outer_.flowContext_.getAny());
    }
  }

  void afterVisit(ESTree::Node *node, ESTree::Node *parent, Type *constraint) {
    // Now that we've found the type of the node, attempt to populate any
    // InferencePlaceholders based on the typechecked actual type of the
    // expression.
    // This must be done after every expression is visited, since the type
    // of the constraint needs to have placeholders filled in whenever possible
    // if we want to use the information to inform the type of the arguments.
    outer_.matchConstraintToType(constraint, outer_.getNodeTypeOrAny(node));
  }

  void visit(
      ESTree::FunctionExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    return outer_.visit(node);
  }
  void visit(
      ESTree::ArrowFunctionExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // If the constraint is a function type, it drives inference of unannotated
    // parameters and the return type (including any InferencePlaceholders,
    // which are filled in when the ReturnStatement argument is visited). A null
    // constraint means a plain arrow with no contextual type. Either way, all
    // parameter kinds (including destructuring) are handled uniformly by
    // visitArrowFunction -> parseFunctionType.
    outer_.visitArrowFunction(
        node,
        llvh::dyn_cast_or_null<TypedFunctionType>(
            constraint ? constraint->info : nullptr));
  }
  void visit(
      ESTree::ClassExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    return outer_.visit(node);
  }

  void visit(
      ESTree::MetaPropertyNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    auto *meta = llvh::cast<ESTree::IdentifierNode>(node->_meta);
    auto *property = llvh::cast<ESTree::IdentifierNode>(node->_property);

    // Check for new.target.
    if (meta->_name == outer_.kw_.identNew &&
        property->_name == outer_.kw_.identTarget) {
      if (!outer_.curFunctionContext_) {
        outer_.sm_.error(
            node->getSourceRange(), "ft: invalid use of new.target");
        return;
      }

      outer_.setNodeType(node, outer_.curFunctionContext_->newTargetType);
      return;
    }

    // All other meta properties are not supported.
    outer_.sm_.error(node->getSourceRange(), "ft: unsupported meta property");
  }

  void
  visit(ESTree::IdentifierNode *node, ESTree::Node *parent, Type *constraint) {
    // Skip cases where the identifier isn't a variable.
    // TODO: these should be dealt with by the parent node.
    if (auto *prop = llvh::dyn_cast<ESTree::PropertyNode>(parent)) {
      if (!prop->_computed && prop->_key == node) {
        // { identifier: ... }
        return;
      }
    }

    if (auto *mem = llvh::dyn_cast<ESTree::MemberExpressionNode>(parent)) {
      if (!mem->_computed && mem->_property == node) {
        // expr.identifier
        return;
      }
    }

    // Identifiers that aren't variables.
    if (llvh::isa<ESTree::MetaPropertyNode>(parent) ||
        llvh::isa<ESTree::BreakStatementNode>(parent) ||
        llvh::isa<ESTree::ContinueStatementNode>(parent) ||
        llvh::isa<ESTree::LabeledStatementNode>(parent)) {
      return;
    }

    // typeof
    if (auto *unary = llvh::dyn_cast<ESTree::UnaryExpressionNode>(parent)) {
      if (unary->_operator == outer_.kw_.identTypeof) {
        // FIXME: handle typeof identifier
        return;
      }
    }

    auto *decl = outer_.getDecl(node);
    assert(decl && "unresolved identifier in expression context");

    if (sema::Decl::isKindGlobal(decl->kind) &&
        decl->name.getUnderlyingPointer() == outer_.kw_.identUndefined) {
      outer_.setNodeType(node, outer_.flowContext_.getVoid());
      return;
    }

    // Inside a specialization of a generic class, a member access on the
    // class's own name refers to that specialization, so its statics can be
    // reached by name.
    if (decl->generic) {
      if (auto *mem = llvh::dyn_cast<ESTree::MemberExpressionNode>(parent);
          mem && mem->_object == node) {
        if (sema::Decl *specDecl = findEnclosingSpecializationDecl(decl)) {
          outer_.semContext_.setExpressionDecl(node, specDecl);
          decl = specDecl;
        }
      }
    }

    if (decl->generic) {
      bool isValid = false;
      if (auto *call = llvh::dyn_cast<ESTree::CallExpressionLikeNode>(parent)) {
        if (ESTree::getCallee(call) == node)
          isValid = true;
      }
      if (auto *newExpr = llvh::dyn_cast<ESTree::NewExpressionNode>(parent)) {
        if (newExpr->_callee == node)
          isValid = true;
      }
      if (auto *classDecl =
              llvh::dyn_cast<ESTree::ClassDeclarationNode>(parent)) {
        if (classDecl->_id == node)
          isValid = true;
        if (classDecl->_superClass == node)
          isValid = true;
      }
      if (!isValid) {
        // Unspecialized generic functions are only allowed in calls.
        // They can't be stored directly because they need type parameters.
        outer_.sm_.error(
            node->getSourceRange(),
            "ft: invalid use of generic function outside of call");
        return;
      }
    }

    // The type is either the type of the identifier or "any".
    Type *type = outer_.flowContext_.findDeclType(decl);

    // In typed functions, 'arguments' is only allowed in 'arguments.length'.
    if (decl->special == sema::Decl::Special::Arguments) {
      // Get the function that 'arguments' belongs to to determine its type.
      sema::FunctionInfo *argumentsOwner = decl->scope->parentFunction;
      FunctionContext *argumentsContext = outer_.curFunctionContext_;
      // Walk up the stack to find the function that 'arguments' belongs to.
      // We won't hit the null context because the FunctionContext must exist.
      while (argumentsContext->semInfo != argumentsOwner)
        argumentsContext = argumentsContext->getPreviousContext();
      Type *funcType = argumentsContext->functionType;
      // For typed functions, ensure that this is arguments.length.
      if (funcType && llvh::isa<TypedFunctionType>(funcType->info)) {
        bool isArgumentsLength = false;
        if (auto *memberParent =
                llvh::dyn_cast<ESTree::MemberExpressionNode>(parent);
            memberParent && memberParent->_object == node &&
            !memberParent->_computed) {
          if (auto *propId = llvh::dyn_cast<ESTree::IdentifierNode>(
                  memberParent->_property);
              propId && propId->_name == outer_.kw_.identLength) {
            isArgumentsLength = true;
          }
        }
        if (!isArgumentsLength)
          outer_.sm_.error(
              node->getSourceRange(),
              "ft: 'arguments' is only allowed in 'arguments.length'"
              " in typed functions");
      }
    }

    // Generic decls don't have types set because they aren't real values.
    // 'arguments' is implicitly typed as 'any' since it's a runtime object.
    if (!type && !sema::Decl::isKindGlobal(decl->kind) && !decl->generic &&
        decl->special != sema::Decl::Special::Arguments) {
      // Assume "any" during the call to setNodeType below.
      // If we're in the same function as decl was declared,
      // then IRGen can report TDZ violations early when applicable.
      // See FlowChecker::AnnotateScopeDecls doc-comment.

      // Report a warning because this is likely unintended.
      // The following code errors in Flow but not in untyped JS:
      //   x = 10;
      //   var x = x + 1;
      // So we don't error to maintain compatibility when there's no
      // annotations.
      outer_.sm_.warning(
          node->getSourceRange(),
          "local variable may be used prior to declaration, assuming 'any'");
    }

    outer_.setNodeType(node, type ? type : outer_.flowContext_.getAny());
  }

  void visit(
      ESTree::ThisExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    outer_.setNodeType(
        node,
        outer_.curFunctionContext_->thisParamType
            ? outer_.curFunctionContext_->thisParamType
            : outer_.flowContext_.getAny());
  }

  /// Member access on a ClassType. Handles Array<T> length/push/index as
  /// special cases of class member access, then dispatches to the public or
  /// private name lookup. \p isWrite is true when the access is a pure write
  /// target (the leaf of a `=` assignment or destructuring pattern).
  Type *visitMemberClass(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      Type *objType,
      ClassType *classType,
      bool isWrite) {
    // Array<T>: special-case .length / .push / numeric index. Private name
    // access on an array falls through to the generic class path below.
    // TODO: This is a HACK, fix it by making Array<T> more expressive.
    if (outer_.flowContext_.isArrayClassType(objType) &&
        (node->_computed ||
         !llvh::isa<ESTree::PrivateNameNode>(node->_property))) {
      if (node->_computed) {
        if (!checkNumberIndex(node->_property)) {
          outer_.sm_.error(
              node->_property->getSourceRange(),
              "ft: array index must be a number");
        }
        return outer_.flowContext_.getArrayElementType(objType);
      }
      auto *id = llvh::cast<ESTree::IdentifierNode>(node->_property);
      if (id->_name == outer_.kw_.identPush) {
        // TODO: Represent .push as a real function.
        return outer_.flowContext_.getAny();
      }
    }

    if (node->_computed) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: computed access to class instances not supported");
      return outer_.flowContext_.getAny();
    }

    // Read the name of the property.
    Identifier name;
    if (auto *privateName =
            llvh::dyn_cast<ESTree::PrivateNameNode>(node->_property)) {
      name = outer_.astContext_.getPrivateNameIdentifier(
          llvh::cast<ESTree::IdentifierNode>(privateName->_id)->_name);
      if (!outer_.classTypeIsEnclosing(classType)) {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            "ft: private field " + name.str() + " not visible outside class " +
                classType->getClassNameOrDefault());
      }
    } else {
      auto *id = llvh::cast<ESTree::IdentifierNode>(node->_property);
      name = Identifier::getFromPointer(id->_name);
    }
    // lookupPropertyOnClass returns null `type` (with non-null `field`) for
    // overloaded methods; resolution happens at the call site.
    auto [type, field] =
        outer_.lookupPropertyOnClass(classType, name, node->_property);
    if (!field) {
      // TODO: class declaration location.
      outer_.sm_.error(
          node->_property->getSourceRange(),
          classType->isHomeObject()
              ? "ft: property " + name.str() +
                  " not defined on the prototype object"
              : "ft: property " + name.str() + " not defined in class " +
                  classType->getClassNameOrDefault());
      return outer_.flowContext_.getAny();
    }

    // Only members stored on the object live on the home object; final
    // methods, private methods and accessors are in Variables instead.
    if (classType->isHomeObject() && !field->isStoredOnObject()) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: property " + name.str() + " is not stored on the prototype");
      return outer_.flowContext_.getAny();
    }

    // Prototype members are read-only.
    if (classType->isHomeObject() &&
        classifyMemberAccess(node, parent, isWrite).write) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: cannot assign to a prototype method");
      return outer_.flowContext_.getAny();
    }

    if (!isDeleteOperand(node, parent) &&
        !checkFieldVariance(node, parent, field->variance, name, isWrite)) {
      return outer_.flowContext_.getAny();
    }

    checkOverloadedMethodReference(node, parent, field, name);

    if (!checkGenericMethodReference(node, parent, type, name))
      return outer_.flowContext_.getAny();

    return getClassFieldAccessType(field, type, isWrite);
  }

  /// \return the Decl of the specialization of the generic class
  ///   \p genericDecl whose body is being checked, or nullptr if there is
  ///   none.
  sema::Decl *findEnclosingSpecializationDecl(sema::Decl *genericDecl) {
    auto &generic = outer_.getGenericInfoMustExist(genericDecl);
    for (ClassContext *ctx = outer_.curClassContext_; ctx;
         ctx = ctx->getPrevContext()) {
      for (const auto &entry : generic.specializations) {
        auto *classDecl =
            llvh::dyn_cast<ESTree::ClassDeclarationNode>(entry.second);
        if (!classDecl)
          continue;
        sema::Decl *specDecl =
            outer_.getDecl(llvh::cast<ESTree::IdentifierNode>(classDecl->_id));
        auto *consType = llvh::dyn_cast_or_null<ClassConstructorType>(
            outer_.flowContext_.findDeclType(specDecl)
                ? outer_.flowContext_.findDeclType(specDecl)->info
                : nullptr);
        if (consType && consType->getClassTypeInfo() == ctx->getClassTypeInfo())
          return specDecl;
      }
    }
    return nullptr;
  }

  /// \return true if \p object names a class declaration whose statics are
  ///   \p staticInfo, so the receiver is exactly that class and not a
  ///   subclass.
  bool isDeclaringClassName(ESTree::Node *object, ClassType *staticInfo) {
    auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(object);
    if (!id)
      return false;
    sema::Decl *decl = outer_.getDecl(id);
    if (!decl || decl->kind != sema::Decl::Kind::Class)
      return false;
    auto *consType =
        llvh::dyn_cast<ClassConstructorType>(outer_.getNodeTypeOrAny(id)->info);
    return consType &&
        consType->getClassTypeInfo()->getStaticObjectTypeInfo() == staticInfo;
  }

  /// \return true if \p node is the callee of \p parent, i.e. it is being
  ///   called directly rather than referenced as a value.
  static bool isDirectCallTarget(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent) {
    auto *callParent = llvh::dyn_cast<ESTree::CallExpressionNode>(parent);
    return callParent && callParent->_callee == node;
  }

  /// Overloaded methods have no single type, so they may only be referenced as
  /// a direct call target, where overload resolution picks one.
  void checkOverloadedMethodReference(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      const ClassType::Field *field,
      Identifier name) {
    if (!field->isOverloaded() || isDirectCallTarget(node, parent))
      return;
    outer_.sm_.error(
        node->_property->getSourceRange(),
        "ft: overloaded method " + name.str() +
            " cannot be referenced outside a call expression");
  }

  /// Generic methods have no concrete type until a call site specializes them,
  /// so they may only be referenced as a direct call target.
  /// \return false if the check failed and an error was emitted.
  bool checkGenericMethodReference(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      Type *type,
      Identifier name) {
    if (!type || !llvh::isa<GenericType>(type->info) ||
        isDirectCallTarget(node, parent))
      return true;
    outer_.sm_.error(
        node->_property->getSourceRange(),
        llvh::Twine("ft: generic method ") + name.str() +
            " cannot be referenced outside a call expression");
    return false;
  }

  /// Static member access, i.e. ClassName.property / ClassName.#property, or
  /// super.property in a static member, where 'super' is the superclass
  /// constructor.
  Type *visitMemberClassStatic(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      ClassConstructorType *consType,
      bool isWrite) {
    if (node->_computed) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: computed access to class statics not supported");
      return outer_.flowContext_.getAny();
    }
    auto *classTypeInfo = llvh::cast<ClassType>(consType->getClassType()->info);
    bool isPrivate = false;
    Identifier name;
    ESTree::IdentifierNode *propId;
    if (auto *privateName =
            llvh::dyn_cast<ESTree::PrivateNameNode>(node->_property)) {
      isPrivate = true;
      propId = llvh::cast<ESTree::IdentifierNode>(privateName->_id);
      name = outer_.astContext_.getPrivateNameIdentifier(propId->_name);
      if (!outer_.classTypeIsEnclosing(classTypeInfo)) {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            "ft: private static " + name.str() + " not visible outside class " +
                classTypeInfo->getClassNameOrDefault());
      }
    } else {
      propId = llvh::cast<ESTree::IdentifierNode>(node->_property);
      name = Identifier::getFromPointer(propId->_name);
    }
    // `prototype` exposes the home object, it's not an ordinary field.
    // Reject writes.
    if (name.getUnderlyingPointer() == outer_.kw_.identPrototype) {
      if (classifyMemberAccess(node, parent, isWrite).write) {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            "ft: cannot assign to prototype");
        return outer_.flowContext_.getAny();
      }
      return classTypeInfo->getHomeObjectType();
    }

    auto *staticInfo = classTypeInfo->getStaticObjectTypeInfo();
    OptValue<ClassType::FieldLookupEntry> optStaticField;
    if (staticInfo) {
      optStaticField = isPrivate ? staticInfo->findPrivateField(name)
                                 : staticInfo->findPublicField(name);
    }
    if (!optStaticField) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          llvh::Twine("ft: static property ") + name.str() +
              " not defined in class " +
              classTypeInfo->getClassNameOrDefault());
      return outer_.flowContext_.getAny();
    }
    const auto *field = optStaticField->getField();
    if (!isDeleteOperand(node, parent) &&
        !checkFieldVariance(node, parent, field->variance, name, isWrite)) {
      return outer_.flowContext_.getAny();
    }

    // ES checks that the receiver itself has a private static, so a subclass
    // receiver throws. Statics are resolved at compile time without that
    // check, so only allow access through the declaring class name.
    if (isPrivate &&
        !isDeclaringClassName(node->_object, optStaticField->classType)) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          llvh::Twine("ft: private static ") + name.str() +
              " can only be accessed through the name of its declaring class");
      return outer_.flowContext_.getAny();
    }

    if (field->isAccessor()) {
      // Static accessor: select the getter result or setter parameter type.
      // Do NOT propagate Decl — IRGen handles the call.
      // A write through 'super' is fine here: the setter receives the
      // receiver as 'this', exactly as in ES.
      return getClassFieldAccessType(field, field->type, isWrite);
    }

    // In ES a write to an inherited static data property creates an own
    // property on the receiver, leaving the declaring class's property alone.
    // Statics live in Variables here, so such a write would silently hit the
    // declaring class's storage instead. Only allow writes through the name
    // of the declaring class itself, where the receiver is known exactly;
    // 'this', 'super' and subclass names may all be a different class.
    if (!field->isMethod() && !isDeleteOperand(node, parent) &&
        classifyMemberAccess(node, parent, isWrite).write &&
        !isDeclaringClassName(node->_object, optStaticField->classType)) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          llvh::Twine("ft: static property ") + name.str() +
              " can only be written through the name of its declaring class");
      return outer_.flowContext_.getAny();
    }

    checkOverloadedMethodReference(node, parent, field, name);

    if (!checkGenericMethodReference(node, parent, field->type, name))
      return outer_.flowContext_.getAny();

    // Propagate the Decl from the definition key to the call-site property
    // so IRGen can look it up.
    if (ESTree::IdentifierNode *keyNode = field->staticKeyNode) {
      if (auto *decl = outer_.semContext_.getExpressionDecl(keyNode))
        outer_.semContext_.setExpressionDecl(propId, decl);
    }
    return field->type;
  }

  /// Classification of a MemberExpression access as a read, write, or both.
  struct MemberAccessKind {
    bool read;
    bool write;
  };

  /// Classify whether \p node is being read, written, or both. \p isWrite is
  /// set by visitAssignmentTarget for pure write targets — `=` LHS (including
  /// leaves buried inside destructuring patterns) and for-in/of LHS. In the
  /// remaining write cases (compound assignment, ++/--, delete) the
  /// MemberExpression sits directly under the relevant parent, so a bottom-up
  /// parent check is sufficient to classify them.
  MemberAccessKind classifyMemberAccess(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      bool isWrite) {
    MemberAccessKind k{!isWrite, isWrite};
    if (isWrite)
      return k;
    if (auto *assign = llvh::dyn_cast<ESTree::AssignmentExpressionNode>(parent);
        assign && assign->_left == node) {
      // Compound assignment (+=, -=, etc) reads and writes.
      // `=` is handled via the isWrite flag set by visitAssignmentTarget.
      k.write = true;
    } else if (auto *upd = llvh::dyn_cast<ESTree::UpdateExpressionNode>(parent);
               upd && upd->_argument == node) {
      k.write = true;
    } else if (isDeleteOperand(node, parent)) {
      k.read = false;
      k.write = true;
    }
    return k;
  }

  /// \return true if \p node is the operand of `delete`. Deleting a class
  /// member is always rejected by the `delete` visitor, so variance checks
  /// skip it to avoid a second diagnostic.
  bool isDeleteOperand(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent) {
    auto *un = llvh::dyn_cast<ESTree::UnaryExpressionNode>(parent);
    return un && un->_argument == node &&
        un->_operator == outer_.kw_.identDelete;
  }

  /// Check the access classified for \p node against \p variance, emitting a
  /// diagnostic if it is forbidden.
  /// \param name the property name, or invalid for an indexer access.
  /// \return false if a diagnostic was emitted, true if the access is allowed.
  bool checkFieldVariance(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      FieldVariance variance,
      Identifier name,
      bool isWrite) {
    if (variance == FieldVariance::None)
      return true;

    MemberAccessKind access = classifyMemberAccess(node, parent, isWrite);
    bool readOnly = variance == FieldVariance::ReadOnly;
    if (readOnly ? !access.write : !access.read)
      return true;

    const char *prefix = readOnly ? "ft: cannot assign to readonly "
                                  : "ft: cannot read writeonly ";
    if (name.isValid()) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          llvh::Twine(prefix) + "property " + name.str());
    } else {
      outer_.sm_.error(
          node->_property->getSourceRange(), llvh::Twine(prefix) + "indexer");
    }

    return false;
  }

  /// \return the value type of \p field for this access. A write to an accessor
  /// uses the setter parameter type; everything else uses \p readType, which
  /// for a getter is its result type. Only a setter-only field can actually
  /// differ: a getter/setter pair must declare the same type, and the other
  /// write forms (+=, ++) are rejected as reads of a write-only property.
  Type *getClassFieldAccessType(
      const ClassType::Field *field,
      Type *readType,
      bool isWrite) {
    if (!isWrite || !field->hasSetter())
      return readType;
    // setterType is always a TypedFunctionType with one param.
    auto *setterFn = llvh::cast<TypedFunctionType>(field->setterType->info);
    return setterFn->getParams()[0].type;
  }

  /// Type a member access that resolves through an object \p indexer. \p
  /// keyType is the key type being used to index: the property's type for
  /// computed access, or `string` for dot access (the key is the identifier
  /// name).
  Type *visitMemberIndexer(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      const ExactObjectType::Indexer &indexer,
      Type *keyType,
      bool isWrite) {
    // Check the key can flow into the indexer's key type.
    if (!outer_.canAFlowIntoB(keyType, indexer.keyType).canFlow) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: object index type " + keyType->messageString() +
              " incompatible with index signature " +
              indexer.keyType->messageString());
    }

    // The result is discarded: the access still resolves to the indexer's
    // value type after the diagnostic. An indexer has no name to report.
    checkFieldVariance(node, parent, indexer.variance, Identifier{}, isWrite);

    // An indexer read resolves to `T`. The underlying load actually yields
    // `T | void` (a missing key reads as `undefined`), but IRGen emits the
    // narrowing checked cast from `T | void` to `T` at read time, so a read
    // of a missing key throws.
    return indexer.valueType;
  }

  Type *visitMemberExactObject(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      ExactObjectType *exactObjType,
      bool isWrite) {
    if (node->_computed) {
      if (const auto &indexer = exactObjType->getIndexer()) {
        return visitMemberIndexer(
            node,
            parent,
            *indexer,
            outer_.getNodeTypeOrAny(node->_property),
            isWrite);
      }
      // TODO: determine what this should do for real.
      // Flow allows this and just returns 'any' (deliberately unsound).
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: computed access to exact object types not supported");
      return outer_.flowContext_.getAny();
    }
    auto *id = llvh::cast<ESTree::IdentifierNode>(node->_property);
    auto optFieldIdx =
        exactObjType->findField(Identifier::getFromPointer(id->_name));
    if (!optFieldIdx) {
      // A dot access uses a string key; route it through the indexer if one
      // exists (indexers and named fields are mutually exclusive).
      if (const auto &indexer = exactObjType->getIndexer()) {
        return visitMemberIndexer(
            node, parent, *indexer, outer_.flowContext_.getString(), isWrite);
      }
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: property " + id->_name->str() + " not defined in object");
      return outer_.flowContext_.getAny();
    }
    const auto &field = exactObjType->getFields()[*optFieldIdx];

    // The result is discarded: the access still resolves to the field's type
    // after the diagnostic.
    checkFieldVariance(node, parent, field.variance, field.name, isWrite);
    return field.type;
  }

  /// Numeric-literal indexed access or .length on a tuple type.
  Type *visitMemberTuple(
      ESTree::MemberExpressionNode *node,
      TupleType *tupleType) {
    if (node->_computed) {
      auto *idx = llvh::dyn_cast<ESTree::NumericLiteralNode>(node->_property);
      if (!idx) {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            "ft: tuple property access requires an number literal index");
        return outer_.flowContext_.getAny();
      }
      double d = idx->_value;
      if (d < 0 || d >= tupleType->getTypes().size()) {
        outer_.sm_.error(
            node->_property->getSourceRange(), "ft: tuple index out of bounds");
        return outer_.flowContext_.getAny();
      }
      // d is in bounds of the valid integer indices so the cast is safe.
      if ((uint32_t)d != d) {
        // ulen can only compare equal to d when d is a valid uint32 integer.
        outer_.sm_.error(
            node->_property->getSourceRange(),
            "ft: tuple index must be a non-negative integer");
        return outer_.flowContext_.getAny();
      }
      return tupleType->getTypes()[(uint32_t)d];
    }
    // Named property access to tuple.
    auto *id = llvh::cast<ESTree::IdentifierNode>(node->_property);
    // TODO: We may want to allow calling into array functions with tuples
    // (providing the union of all elements as the generic type of the
    // array), but that's not supported yet.
    if (id->_name == outer_.kw_.identLength) {
      // Tuple .length is a number.
      return outer_.flowContext_.getNumber();
    }
    outer_.sm_.error(
        node->_property->getSourceRange(), "ft: unknown tuple property");
    return outer_.flowContext_.getAny();
  }

  /// \return whether \p index is a number (or any). A union of number
  /// literals is accepted too, and \p index is cast to number so IRGen sees
  /// a plain number index.
  bool checkNumberIndex(ESTree::Node *&index) {
    Type *indexType = outer_.getNodeTypeOrAny(index);
    if (llvh::isa<NumberType>(indexType->info) ||
        llvh::isa<NumberLiteralType>(indexType->info) ||
        llvh::isa<AnyType>(indexType->info))
      return true;
    Type *widened = outer_.widenLiteralType(indexType);
    if (!llvh::isa<NumberType>(widened->info))
      return false;
    index = outer_.implicitCheckedCast(
        index, widened, {.canFlow = true, .needCheckedCast = true});
    return true;
  }

  /// Indexed access, .length, or builtin method access on a string.
  Type *visitMemberString(ESTree::MemberExpressionNode *node) {
    if (node->_computed) {
      if (!checkNumberIndex(node->_property)) {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            "ft: string index must be a number");
      }
      return outer_.flowContext_.getString();
    }
    auto *id = llvh::cast<ESTree::IdentifierNode>(node->_property);
    if (id->_name == outer_.kw_.identLength)
      return outer_.flowContext_.getNumber();
    if (auto *builtinDecl = outer_.flowContext_.findBuiltinMethod(
            {TypeKind::String, id->_name, /* isStatic */ false})) {
      // Found a builtin method - store for CallExpression to use.
      // The actual type will be determined by CallExpression.
      // For now, use any.
      outer_.setBuiltinMethodDecl(node, builtinDecl);
      return outer_.flowContext_.getAny();
    }
    outer_.sm_.error(
        node->_property->getSourceRange(), "ft: unknown string property");
    return outer_.flowContext_.getAny();
  }

  /// Builtin method access on a number. Only non-computed access reaches here;
  /// numbers have no indexed access and no named non-method properties.
  Type *visitMemberNumber(ESTree::MemberExpressionNode *node) {
    auto *id = llvh::cast<ESTree::IdentifierNode>(node->_property);
    if (auto *builtinDecl = outer_.flowContext_.findBuiltinMethod(
            {TypeKind::Number, id->_name, /* isStatic */ false})) {
      // Found a builtin method - store for CallExpression to use.
      // The actual type will be determined by CallExpression.
      outer_.setBuiltinMethodDecl(node, builtinDecl);
      return outer_.flowContext_.getAny();
    }
    outer_.sm_.error(
        node->_property->getSourceRange(), "ft: unknown number property");
    return outer_.flowContext_.getAny();
  }

  /// Read a field that is common to every arm of a union of object-literal
  /// types. The field must exist in every arm; it need not sit at the same slot
  /// in each (IRGen picks a slot-indexed load when the slots happen to match
  /// and a by-name load otherwise). The result type is the union of the per-arm
  /// field types. Only non-computed reads are supported.
  Type *visitMemberUnion(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      UnionType *unionType,
      bool isWrite) {
    if (node->_computed) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: computed access to a union not supported");
      return outer_.flowContext_.getAny();
    }

    // Only reads are supported for now.
    if (classifyMemberAccess(node, parent, isWrite).write) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: cannot write to a property of a union");
      return outer_.flowContext_.getAny();
    }

    Identifier name = Identifier::getFromPointer(
        llvh::cast<ESTree::IdentifierNode>(node->_property)->_name);

    llvh::SmallVector<Type *, 4> fieldTypes{};
    fieldTypes.reserve(unionType->getTypes().size());
    for (Type *arm : unionType->getTypes()) {
      auto *objType = llvh::dyn_cast<ExactObjectType>(arm->info);
      if (!objType) {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            "ft: property '" + name.str() +
                "' cannot be read on union with non-object arm " +
                arm->messageString());
        return outer_.flowContext_.getAny();
      }
      auto optFieldIdx = objType->findField(name);
      if (!optFieldIdx) {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            "ft: property '" + name.str() +
                "' is not present in all arms of the union");
        return outer_.flowContext_.getAny();
      }
      const auto &field = objType->getFields()[*optFieldIdx];
      if (field.variance == FieldVariance::WriteOnly) {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            "ft: cannot read writeonly property '" + name.str() + "'");
        return outer_.flowContext_.getAny();
      }
      fieldTypes.push_back(field.type);
    }

    return outer_.flowContext_.createType(
        outer_.flowContext_.maybeCreateUnion(fieldTypes));
  }

  void visit(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // TODO: types
    // Encountered through the normal expression visit, so this access is a
    // read. The write path goes through visitAssignmentTarget.
    visitESTreeNode(*this, node->_object, node, nullptr);
    if (node->_computed)
      visitESTreeNode(*this, node->_property, node, nullptr);
    resolveMemberExpressionType(node, parent, /*isWrite*/ false);
  }

  /// Walk \p target which sits in the write position of a `=` assignment or
  /// destructuring pattern. Identifier and (Optional)MemberExpression are
  /// handled directly; pattern nodes are dispatched to their visitor, which
  /// recurses via this function for nested write positions. Only the
  /// outermost MemberExpression at each pattern leaf is a write — its inner
  /// `_object` (e.g. `a.b` inside `a.b.c`) is just a read of the container
  /// whose slot is being written.
  void visitAssignmentTarget(
      ESTree::Node *target,
      ESTree::Node *parent,
      Type *constraint) {
    if (auto *mem = llvh::dyn_cast<ESTree::MemberExpressionNode>(target)) {
      visitESTreeNode(*this, mem->_object, mem, nullptr);
      if (mem->_computed)
        visitESTreeNode(*this, mem->_property, mem, nullptr);
      resolveMemberExpressionType(mem, parent, /*isWrite*/ true);
      return;
    }
    visitESTreeNode(*this, target, parent, constraint);
  }

  /// Check that the source type \p srcType supplied to the already-visited
  /// destructuring target \p target can reach it. \p kind names the kind of
  /// destructuring for the diagnostic. No ImplicitCheckedCast is inserted for
  /// a narrowing source: there is no RHS expression node to hang one on, so
  /// IRGen narrows the value instead, in LReference::emitStore. That only
  /// covers identifier and member targets; a nested pattern target needs no
  /// narrowing because it is typed equal to the source.
  void checkDestructuringTarget(
      ESTree::Node *target,
      Type *srcType,
      const char *kind) {
    Type *targetType = outer_.getNodeTypeOrAny(target);
    if (outer_.tryNarrowType(srcType, targetType).second.canFlow)
      return;
    outer_.sm_.error(
        target->getSourceRange(),
        llvh::Twine("ft: incompatible ") + kind +
            " destructuring target type: cannot implicitly cast from " +
            srcType->messageString() + " to " + targetType->messageString());
  }

  /// \return the type a destructuring pattern whose parent is \p parent should
  /// be typed from, given the \p constraint the parent forwarded. An unknown
  /// source is 'any'. A `?T` source narrows to T only for the outermost
  /// pattern, where the enclosing assignment inserts the checked cast on its
  /// RHS, as it did before the pattern was typed from the source. A nested
  /// pattern has no expression node to hang a cast on, so it keeps the
  /// optional type and is rejected as not destructurable.
  Type *destructuringSourceType(ESTree::Node *parent, Type *constraint) {
    if (!constraint)
      return outer_.flowContext_.getAny();
    if (llvh::isa<ESTree::AssignmentExpressionNode>(parent))
      if (Type *nonOpt = FlowChecker::getNonOptionalSingleType(constraint))
        return nonOpt;
    return constraint;
  }

  /// \return a callback for the expand* helpers that types each child of
  /// \p pattern from the source type it receives and checks it. \p kind names
  /// the kind of destructuring for the diagnostic.
  auto destructuringChildVisitor(ESTree::Node *pattern, const char *kind) {
    return [this, pattern, kind](ESTree::Node *target, Type *srcType) {
      visitAssignmentTarget(target, pattern, srcType);
      checkDestructuringTarget(target, srcType, kind);
    };
  }

  /// Compute and set the type of \p node, assuming its \c _object (and
  /// \c _property when computed) have already been visited. \p isWrite is
  /// true when called from visitAssignmentTarget for a write-target leaf.
  void resolveMemberExpressionType(
      ESTree::MemberExpressionNode *node,
      ESTree::Node *parent,
      bool isWrite) {
    Type *objType = outer_.getNodeTypeOrAny(node->_object);
    Type *resType = outer_.flowContext_.getAny();

    // 'arguments.length' is always typed as number.
    if (auto *objId = llvh::dyn_cast<ESTree::IdentifierNode>(node->_object);
        objId && !node->_computed) {
      auto *objDecl = outer_.getDecl(objId);
      if (objDecl && objDecl->special == sema::Decl::Special::Arguments) {
        if (auto *propId =
                llvh::dyn_cast<ESTree::IdentifierNode>(node->_property);
            propId && propId->_name == outer_.kw_.identLength) {
          outer_.setNodeType(node, outer_.flowContext_.getNumber());
          return;
        }
      }
    }

    // A union of literals (e.g. "a" | "b") supports the members of its base
    // type. Cast the object so its node type reflects that.
    if (llvh::isa<UnionType>(objType->info)) {
      Type *widened = outer_.widenLiteralType(objType);
      if (widened != objType && !llvh::isa<UnionType>(widened->info)) {
        objType = widened;
        node->_object = outer_.implicitCheckedCast(
            node->_object, widened, {.canFlow = true, .needCheckedCast = true});
      } else {
        objType = widened;
      }
    }

    // Attempt to narrow object type if it doesn't currently support member
    // access.
    if (Type *narrowedObjType = outer_.getNonOptionalSingleType(objType)) {
      objType = narrowedObjType;
      node->_object = outer_.implicitCheckedCast(
          node->_object,
          narrowedObjType,
          {.canFlow = true, .needCheckedCast = true});
    }

    // Private names only resolve against a single class, so every other
    // receiver is rejected here rather than in each visitMember* helper.
    // IRGen needs the class to find the private name's storage, so `any` is
    // rejected too.
    if (llvh::isa<ESTree::PrivateNameNode>(node->_property) &&
        !llvh::isa<ClassType>(objType->info) &&
        !llvh::isa<ClassConstructorType>(objType->info)) {
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: private name access only allowed on class instances, found " +
              objType->messageString());
      outer_.setNodeType(node, resType);
      return;
    }

    if (auto *classType = llvh::dyn_cast<ClassType>(objType->info)) {
      resType = visitMemberClass(node, parent, objType, classType, isWrite);
    } else if (
        auto *consType = llvh::dyn_cast<ClassConstructorType>(objType->info)) {
      resType = visitMemberClassStatic(node, parent, consType, isWrite);
    } else if (
        auto *exactObjType = llvh::dyn_cast<ExactObjectType>(objType->info)) {
      resType = visitMemberExactObject(node, parent, exactObjType, isWrite);
    } else if (auto *tupleType = llvh::dyn_cast<TupleType>(objType->info)) {
      resType = visitMemberTuple(node, tupleType);
    } else if (
        llvh::isa<StringType>(objType->info) ||
        llvh::isa<StringLiteralType>(objType->info)) {
      // A string literal type supports all the operations of String.
      resType = visitMemberString(node);
    } else if (
        (llvh::isa<NumberType>(objType->info) ||
         llvh::isa<NumberLiteralType>(objType->info)) &&
        !node->_computed) {
      // A number literal type supports all the operations of Number.
      resType = visitMemberNumber(node);
    } else if (auto *unionType = llvh::dyn_cast<UnionType>(objType->info)) {
      resType = visitMemberUnion(node, parent, unionType, isWrite);
    } else if (!llvh::isa<AnyType>(objType->info)) {
      if (node->_computed) {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            llvh::Twine(
                "ft: indexed access only allowed on array/tuple/string, found ") +
                objType->info->getKindName());
      } else {
        outer_.sm_.error(
            node->_property->getSourceRange(),
            llvh::Twine(
                "ft: named property access only allowed on objects, found ") +
                objType->messageString());
      }
    }

    outer_.setNodeType(node, resType);
  }
  void visit(
      ESTree::OptionalMemberExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // TODO: types
    outer_.sm_.warning(
        node->getSourceRange(),
        "ft: optional member expression not implemented");
    visitESTreeNode(*this, node->_object, node, nullptr);
    if (node->_computed)
      visitESTreeNode(*this, node->_property, node, nullptr);
    // Untyped optional access has no class to resolve a private name against.
    if (llvh::isa<ESTree::PrivateNameNode>(node->_property))
      outer_.sm_.error(
          node->_property->getSourceRange(),
          "ft: private name access not supported in optional member "
          "expressions");
  }

  void visitExplicitCast(
      ESTree::Node *node,
      ESTree::Node *expression,
      ESTree::Node *typeAnnotation) {
    auto *resTy = outer_.parseTypeAnnotation(typeAnnotation);
    // Populate the type of this node before visiting the expression, since it
    // is already known. This also allows the result type to be used as context
    // while we are visiting the expression being cast. For instance, if we are
    // casting an empty array literal, the resulting type of the cast can be
    // used to set the element type of the array.
    visitESTreeNode(*this, expression, node, resTy);

    auto *expTy = outer_.getNodeTypeOrAny(expression);
    auto cf = outer_.canAFlowIntoB(expTy->info, resTy->info);
    if (!cf.canFlow) {
      outer_.sm_.error(
          node->getSourceRange(), "ft: cast from incompatible type");
    }

    outer_.setNodeType(node, resTy);
  }

  void visit(
      ESTree::TypeCastExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitExplicitCast(
        node,
        node->_expression,
        llvh::cast<ESTree::TypeAnnotationNode>(node->_typeAnnotation)
            ->_typeAnnotation);
  }

  void visit(
      ESTree::AsExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitExplicitCast(node, node->_expression, node->_typeAnnotation);
  }

  /// Visits an array expression as a tuple given the \p constraint and its
  /// TypeInfo \p tupleTy.
  void visitArrayExpressionAsTuple(
      ESTree::ArrayExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint,
      TupleType *tupleTy) {
    assert(
        constraint && tupleTy && constraint->info == tupleTy &&
        "must have a tuple type");
    auto &elements = node->_elements;
    size_t i = 0;
    bool tooFewTupleElements = false;
    for (auto it = elements.begin(); it != elements.end(); ++it, ++i) {
      if (i >= tupleTy->getTypes().size()) {
        tooFewTupleElements = true;
        break;
      }
      ESTree::Node *elem = &*it;
      Type *constraintElemTy = tupleTy->getTypes()[i];
      visitESTreeNodeNoReplace(*this, elem, node, constraintElemTy);
      if (llvh::isa<ESTree::SpreadElementNode>(elem)) {
        outer_.sm_.error(
            elem->getSourceRange(), "ft: tuple spread element not supported");
        continue;
      }
      // Check that the actual type is compatible with the constraint.
      Type *actualElemTy = outer_.getNodeTypeOrAny(elem);
      CanFlowResult cf = outer_.canAFlowIntoB(actualElemTy, constraintElemTy);
      if (!cf.canFlow) {
        outer_.sm_.error(
            elem->getSourceRange(),
            llvh::Twine("ft: incompatible tuple element type at index: ") +
                llvh::Twine(i));
        continue;
      }
      // Add a checked cast if needed.
      if (cf.canFlow && cf.needCheckedCast) {
        auto newIt = elements.insert(
            it, *outer_.implicitCheckedCast(elem, constraintElemTy, cf));
        elements.erase(it);
        it = newIt;
      }
    }

    if (tooFewTupleElements || i != tupleTy->getTypes().size()) {
      outer_.sm_.error(
          node->getSourceRange(),
          llvh::Twine("ft: incompatible tuple type, expected ") +
              llvh::Twine(tupleTy->getTypes().size()) + " elements, found " +
              llvh::Twine(elements.size()));
      outer_.setNodeType(node, outer_.flowContext_.getAny());
      return;
    }

    outer_.setNodeType(node, constraint);
  }

  /// Visits an array expression as an array.
  /// \param constraint the constraint on the array type, if it is an Array
  ///   ClassType.
  void visitArrayExpressionAsArray(
      ESTree::ArrayExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    bool hasArrayConstraint = false;
    Type *constraintElemTy = nullptr;
    if (constraint) {
      if (outer_.flowContext_.isArrayClassType(constraint)) {
        hasArrayConstraint = true;
        constraintElemTy = outer_.flowContext_.getArrayElementType(constraint);
      } else if (
          auto *inferArray =
              llvh::dyn_cast<InferencePlaceholderArrayType>(constraint->info)) {
        hasArrayConstraint = true;
        constraintElemTy = inferArray->getElement();
      }
    }

    // Construct a union of all the element types if there's no constraint.
    llvh::SmallSetVector<Type *, 4> elTypes{};
    auto &elements = node->_elements;
    size_t i = 0;
    for (auto it = elements.begin(); it != elements.end(); ++it, ++i) {
      ESTree::Node *elem = &*it;

      Type *actualElemTy = nullptr;
      if (auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(elem)) {
        Type *constraintSpreadElemTy =
            hasArrayConstraint ? constraint : nullptr;
        visitESTreeNodeNoReplace(
            *this, spread->_argument, node, constraintSpreadElemTy);
        auto *spreadTy = outer_.getNodeTypeOrAny(spread->_argument);
        if (!outer_.flowContext_.isArrayClassType(spreadTy)) {
          // TODO: Handle spread of non-arrays.
          outer_.sm_.error(
              spread->_argument->getSourceRange(),
              "ft: spread argument must be an array");
          continue;
        }
        actualElemTy = outer_.flowContext_.getArrayElementType(spreadTy);
      } else {
        visitESTreeNodeNoReplace(*this, elem, node, constraintElemTy);
        actualElemTy = outer_.getNodeTypeOrAny(elem);
      }

      // elTypes only matters without an Array constraint, where fresh literal
      // element types are widened (['a','b'] infers Array<string>). Widen
      // each element now so the union doesn't grow with every literal.
      elTypes.insert(outer_.widenLiteralType(actualElemTy));
      if (constraintElemTy) {
        // If there's a constraint on the element type, check that each
        // element conforms and insert implicit casts when necessary.
        CanFlowResult cf = outer_.canAFlowIntoB(actualElemTy, constraintElemTy);
        if (!cf.canFlow) {
          outer_.sm_.error(
              elem->getSourceRange(),
              llvh::Twine("ft: incompatible array element type at index: ") +
                  llvh::Twine(i));
          continue;
        }
        // Add a checked cast if needed. Skip spread elements, since we need
        // to cast each element they produce, rather than the spread itself.
        if (cf.needCheckedCast) {
          if (llvh::isa<ESTree::SpreadElementNode>(elem)) {
            // We don't support spread elements with checked casts yet,
            // because we have to cast each element individually,
            outer_.sm_.error(
                elem->getSourceRange(),
                "ft: spread element with checked cast not supported");
          } else {
            auto newIt = elements.insert(
                it, *outer_.implicitCheckedCast(elem, constraintElemTy, cf));
            elements.erase(it);
            it = newIt;
          }
        }
      }
    }

    if (outer_.flowContext_.isArrayClassType(constraint)) {
      outer_.setNodeType(node, constraint);
    } else {
      // Construct the element type. If there are no elements and no context,
      // infer an 'any' element type, producing an 'any[]' array.
      Type *elemUnion;
      if (elTypes.empty()) {
        outer_.sm_.warning(
            node->getSourceRange(),
            "ft: empty array with no context, assuming 'any' array");
        elemUnion = outer_.flowContext_.getAny();
      } else {
        // Otherwise, construct a union of all the (widened) element types; an
        // explicit Array<"a"> annotation is handled above via the constraint
        // and keeps the literal type.
        elemUnion = outer_.flowContext_.createType(
            outer_.flowContext_.maybeCreateUnion(elTypes.getArrayRef()));
      }
      Type *arrType = outer_.getSpecializedArrayClassType(
          elemUnion, node->getSourceRange());
      if (arrType) {
        outer_.setNodeType(node, arrType);
      } else {
        outer_.sm_.error(
            node->getSourceRange(), "ft: Array type requires TypedLib");
        outer_.setNodeType(node, outer_.flowContext_.getAny());
      }
    }
  }

  void visit(
      ESTree::ArrayExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    if (auto *tupleTy = llvh::dyn_cast_or_null<TupleType>(
            constraint ? constraint->info : nullptr)) {
      // The type of ArrayExpression is only Tuple when that constraint was
      // passed in from above.
      visitArrayExpressionAsTuple(node, parent, constraint, tupleTy);
    } else {
      visitArrayExpressionAsArray(node, parent, constraint);
    }
  }

  void visit(
      ESTree::ObjectExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // Failed to make an object type that matches the properties,
    // assume 'any' and continue.
    bool assumeAny = false;

    // Fields of the object.
    llvh::SmallVector<ExactObjectType::Field, 4> fields;
    // Name of the key, mapping to index in the fields vector.
    llvh::SmallDenseMap<UniqueString *, size_t> names;

    // A spread indexer or any computed property forces an indexer object.
    // indexerKeyType holds the spread key type; indexerValueTypes accumulates
    // value types from spreads, computed properties, and mixed-in named fields.
    Type *indexerKeyType = nullptr;
    llvh::SmallSetVector<Type *, 4> indexerValueTypes{};

    // Computed properties (e.g. `{[k]: v}`) also force an indexer object.
    bool sawComputed = false;
    // The indexer key type is the union of all computed key types, plus string
    // when named fields are present.
    llvh::SmallSetVector<Type *, 4> computedKeyTypes{};

    auto *constraintObjectType = llvh::dyn_cast_or_null<ExactObjectType>(
        constraint ? constraint->info : nullptr);

    for (ESTree::Node &node : node->_properties) {
      // Spread element: merge fields from an exact-object source.
      if (auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(&node)) {
        visitESTreeNodeNoReplace(*this, spread->_argument, spread, nullptr);
        Type *spreadTy = outer_.getNodeTypeOrAny(spread->_argument);
        if (llvh::isa<AnyType>(spreadTy->info)) {
          outer_.sm_.warning(
              node.getSourceRange(),
              "ft: unsupported property for typed object, assuming 'any'");
          assumeAny = true;
        } else if (
            auto *spreadObjTy =
                llvh::dyn_cast<ExactObjectType>(spreadTy->info)) {
          for (const auto &srcField : spreadObjTy->getFields()) {
            // Spread reads every source field; writeonly fields are not
            // readable.
            if (srcField.variance == FieldVariance::WriteOnly) {
              outer_.sm_.error(
                  spread->getSourceRange(),
                  "ft: cannot read writeonly property " +
                      srcField.name.getUnderlyingPointer()->str());
            }
            // The spread's destination is a fresh object literal, so its
            // fields are always invariant regardless of the source's
            // variance.
            auto [it, inserted] = names.try_emplace(
                srcField.name.getUnderlyingPointer(), fields.size());
            if (inserted) {
              fields.emplace_back(srcField.name, srcField.type);
            } else {
              fields[it->second].type = srcField.type;
              fields[it->second].variance = FieldVariance::None;
            }
          }
          // A source with an indexer contributes its indexer to the result.
          // Such a source has no named fields, so the loop above did nothing.
          if (const auto &srcIndexer = spreadObjTy->getIndexer()) {
            if (srcIndexer->variance == FieldVariance::WriteOnly) {
              outer_.sm_.error(
                  spread->getSourceRange(),
                  "ft: cannot read writeonly indexer");
            }
            if (!indexerKeyType) {
              indexerKeyType = srcIndexer->keyType;
            } else if (!indexerKeyType->info->equals(
                           srcIndexer->keyType->info)) {
              outer_.sm_.error(
                  spread->getSourceRange(),
                  "ft: incompatible indexer key types in spread");
            }
            indexerValueTypes.insert(srcIndexer->valueType);
          }
        } else {
          outer_.sm_.error(
              spread->getSourceRange(),
              "ft: spread argument must be an exact object type");
        }
        continue;
      }

      auto *prop = llvh::dyn_cast<ESTree::PropertyNode>(&node);
      // prop->_kind being "init" makes sure this isn't a getter/setter.
      if (!prop || prop->_kind != outer_.kw_.identInit || prop->_method) {
        // Exact object type doesn't support getters/setters/methods, so bail.
        outer_.sm_.warning(
            node.getSourceRange(),
            "ft: unsupported property for typed object, assuming 'any'");
        assumeAny = true;
        break;
      }

      if (prop->_computed) {
        visitESTreeNode(*this, prop->_key, prop, nullptr);
        Type *keyType = outer_.getNodeTypeOrAny(prop->_key);
        Type *constraintValueType = nullptr;
        if (constraintObjectType) {
          // If the result is constrained to an indexer object, the key and
          // value are checked against the indexer's types.
          if (auto optIndexer = constraintObjectType->getIndexer()) {
            constraintValueType = optIndexer->valueType;
            // Indexer keys must match exactly, so adopt the constraint's key.
            if (outer_.canAFlowIntoB(keyType, optIndexer->keyType)
                    .canFlowWithoutCast()) {
              keyType = optIndexer->keyType;
            } else {
              outer_.sm_.error(
                  prop->_key->getSourceRange(),
                  "ft: incompatible computed property key type");
            }
          }
        }
        visitESTreeNode(*this, prop->_value, prop, constraintValueType);
        Type *valueType = outer_.getNodeTypeOrAny(prop->_value);
        if (constraintValueType) {
          auto cf = outer_.canAFlowIntoB(valueType, constraintValueType);
          if (!cf.canFlow) {
            outer_.sm_.error(
                prop->_value->getSourceRange(),
                "ft: incompatible computed property value type");
          }
          if (cf.needCheckedCast) {
            prop->_value = outer_.implicitCheckedCast(
                prop->_value, constraintValueType, cf);
          }
          valueType = constraintValueType;
        }
        computedKeyTypes.insert(keyType);
        indexerValueTypes.insert(valueType);
        sawComputed = true;
        continue;
      }

      visitESTreeNode(*this, prop->_key, prop, nullptr);

      UniqueString *name = outer_.propertyKeyAsIdentifier(prop->_key);
      if (!name || name == outer_.kw_.identUnderscoreProto) {
        outer_.sm_.warning(
            prop->_key->getSourceRange(),
            "ft: unsupported key for typed object, assuming 'any'");
        assumeAny = true;
        break;
      }

      Type *constraintValueType = nullptr;
      if (constraintObjectType) {
        if (auto optField = constraintObjectType->findField(
                Identifier::getFromPointer(name))) {
          constraintValueType =
              constraintObjectType->getFields()[*optField].type;
        } else if (const auto &indexer = constraintObjectType->getIndexer()) {
          // Constructing an indexer object: every value is checked against the
          // indexer's value type.
          constraintValueType = indexer->valueType;
        }
      }

      // Use the value constraint to visit the value node.
      visitESTreeNode(*this, prop->_value, prop, constraintValueType);

      Type *valueType = outer_.getNodeTypeOrAny(prop->_value);

      if (constraintValueType) {
        auto cf = outer_.canAFlowIntoB(valueType, constraintValueType);
        if (!cf.canFlow) {
          outer_.sm_.error(
              prop->_value->getSourceRange(),
              llvh::Twine("ft: incompatible property type for '") +
                  name->str() + "'");
        }
        if (cf.needCheckedCast) {
          prop->_value =
              outer_.implicitCheckedCast(prop->_value, constraintValueType, cf);
        }
        valueType = constraintValueType;
      }

      // Handle duplicate keys.
      auto [it, inserted] = names.try_emplace(name, fields.size());
      if (inserted) {
        // New field.
        fields.emplace_back(Identifier::getFromPointer(name), valueType);
      } else {
        // Existing field, update the type to the later value's type.
        fields[it->second].type = valueType;
      }
    }

    if (assumeAny) {
      // Failed to make an object type that matches the properties.
      outer_.flowContext_.setNodeType(node, outer_.flowContext_.getAny());
      return;
    }

    // Mixing computed (indexer) and named properties is not supported. Warn,
    // but still produce the indexer type below. Named fields may come from
    // explicit properties or spread sources, so check `fields` directly.
    if (sawComputed && !fields.empty()) {
      outer_.sm_.warning(
          node->getSourceRange(),
          "ft: mixing computed and named properties in a typed object "
          "will result in an indexer");
    }

    // An un-annotated object literal (no contextual type) widens its fresh
    // literal field types to their base types, mirroring let/var widening
    // (e.g. {x: 'a'} infers {x: string}). When a constraint is present --
    // including a union constraint used for discriminated unions -- the literal
    // field types are kept so the object can flow into the expected arm.
    if (!constraint) {
      for (auto &f : fields)
        f.type = outer_.widenLiteralType(f.type);
    }

    // Produce an indexer object if any spread had an indexer or any computed
    // property was present. The key and value types are the unions of all
    // contributing key and value types.
    if (!indexerValueTypes.empty()) {
      // Fold the computed key types and string into the indexer key type.
      if (sawComputed) {
        if (!fields.empty())
          computedKeyTypes.insert(outer_.flowContext_.getString());
        if (indexerKeyType)
          computedKeyTypes.insert(indexerKeyType);
        indexerKeyType =
            outer_.flowContext_.createType(outer_.flowContext_.maybeCreateUnion(
                computedKeyTypes.getArrayRef()));
      }
      // An un-annotated indexer object widens fresh literal key types too.
      if (!constraint)
        indexerKeyType = outer_.widenLiteralType(indexerKeyType);
      // Named properties use string keys, so they require a string-keyed
      // indexer (field names cannot index a number-keyed indexer).
      if (!fields.empty() &&
          !outer_.canAFlowIntoB(outer_.flowContext_.getString(), indexerKeyType)
               .canFlow) {
        outer_.sm_.error(
            node->getSourceRange(),
            "ft: object index type string incompatible with index signature " +
                indexerKeyType->messageString());
      }
      llvh::SmallSetVector<Type *, 4> valueTypes{};
      for (const auto &f : fields)
        valueTypes.insert(f.type);
      for (Type *vt : indexerValueTypes)
        valueTypes.insert(vt);
      Type *valueType = outer_.flowContext_.createType(
          outer_.flowContext_.maybeCreateUnion(valueTypes.getArrayRef()));
      // Widen fresh literal indexer value types (computed/spread values) when
      // there is no contextual constraint, mirroring the field widening above.
      if (!constraint)
        valueType = outer_.widenLiteralType(valueType);
      ExactObjectType::Indexer indexer{
          indexerKeyType, valueType, FieldVariance::None};
      outer_.setNodeType(
          node,
          outer_.flowContext_.createType(
              outer_.flowContext_.createExactObject({}, indexer), node));
      return;
    }

    outer_.setNodeType(
        node,
        outer_.flowContext_.createType(
            outer_.flowContext_.createExactObject(fields), node));
  }

  void visit(
      ESTree::SpreadElementNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // Do nothing for the spread element itself, handled by the parent.
    visitESTreeChildren(*this, node, nullptr);
  }

  void
  visit(ESTree::NullLiteralNode *node, ESTree::Node *parent, Type *constraint) {
    outer_.setNodeType(node, outer_.flowContext_.getNull());
  }
  void visit(
      ESTree::BooleanLiteralNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // Type a boolean literal as its own BooleanLiteralType. It flows into
    // Boolean, so this is compatible with Boolean contexts; un-annotated
    // let/var declarations widen it back to Boolean during inference.
    outer_.setNodeType(
        node,
        outer_.flowContext_.createType(
            outer_.flowContext_.createBooleanLiteral(node->_value), node));
  }
  void visit(
      ESTree::StringLiteralNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // Type a string literal as its own StringLiteralType. It flows into String,
    // so this is compatible with String contexts; un-annotated let/var
    // declarations widen it back to String during inference.
    outer_.setNodeType(
        node,
        outer_.flowContext_.createType(
            outer_.flowContext_.createStringLiteral(node->_value), node));
  }
  void visit(
      ESTree::TemplateLiteralNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    for (ESTree::Node &quasi : node->_quasis)
      outer_.setNodeType(&quasi, outer_.flowContext_.getString());
    visitESTreeNodeList(*this, node->_expressions, node, nullptr);
    outer_.setNodeType(node, outer_.flowContext_.getString());
  }
  void visit(
      ESTree::SequenceExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeChildren(*this, node, nullptr);
    // Value is the value of the last expression.
    ESTree::Node *last = &node->_expressions.back();
    outer_.setNodeType(node, outer_.getNodeTypeOrAny(last));
  }
  void visit(
      ESTree::YieldExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeChildren(*this, node, nullptr);
    // The user can provide anything to 'yield', so it has type 'mixed'.
    outer_.setNodeType(node, outer_.flowContext_.getMixed());
  }
  void visit(
      ESTree::AwaitExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeChildren(*this, node, nullptr);
    // TODO: Recognize Promise<T>. this node will have type T.
    outer_.sm_.warning(
        node->getSourceRange(),
        "ft: await expression not supported, assuming 'any'");
    outer_.setNodeType(node, outer_.flowContext_.getAny());
  }
  void visit(
      ESTree::ImportExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeChildren(*this, node, nullptr);
    outer_.sm_.warning(
        node->getSourceRange(),
        "ft: import expression not supported, assuming 'any'");
    outer_.setNodeType(node, outer_.flowContext_.getAny());
  }
  void visit(
      ESTree::TaggedTemplateExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeChildren(*this, node, nullptr);
    outer_.sm_.warning(
        node->getSourceRange(),
        "ft: tagged template expression not supported, assuming 'any'");
    outer_.setNodeType(node, outer_.flowContext_.getAny());
  }
  void visit(
      ESTree::AsConstExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeNode(*this, node->_expression, node, nullptr);
    Type *t = outer_.getNodeTypeOrAny(node->_expression);
    outer_.sm_.warning(
        node->getSourceRange(),
        "ft: 'as const' expression not supported, assuming " +
            t->messageString());
    outer_.setNodeType(node, t);
  }
#if HERMES_PARSE_JSX
  void
  visit(ESTree::JSXElementNode *node, ESTree::Node *parent, Type *constraint) {
    visitESTreeChildren(*this, node, nullptr);
    outer_.sm_.warning(
        node->getSourceRange(),
        "ft: JSX element not supported, assuming 'any'");
    outer_.setNodeType(node, outer_.flowContext_.getAny());
  }
  void
  visit(ESTree::JSXFragmentNode *node, ESTree::Node *parent, Type *constraint) {
    visitESTreeChildren(*this, node, nullptr);
    outer_.sm_.warning(
        node->getSourceRange(),
        "ft: JSX fragment not supported, assuming 'any'");
    outer_.setNodeType(node, outer_.flowContext_.getAny());
  }
#endif
  void visit(
      ESTree::NumericLiteralNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // Type a numeric literal as its own NumberLiteralType. It flows into
    // Number, so this is compatible with Number contexts; un-annotated
    // let/var declarations widen it back to Number during inference.
    outer_.setNodeType(
        node,
        outer_.flowContext_.createType(
            outer_.flowContext_.createNumberLiteral(node->_value), node));
  }
  void visit(
      ESTree::RegExpLiteralNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    outer_.setNodeType(node, outer_.flowContext_.getAny());
  }
  void visit(
      ESTree::BigIntLiteralNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    outer_.setNodeType(node, outer_.flowContext_.getBigInt());
  }
  void
  visit(ESTree::SHBuiltinNode *node, ESTree::Node *parent, Type *constraint) {
    // SHBuiltin handled at the call expression level.
    visitESTreeChildren(*this, node, nullptr);
  }

  void visit(
      ESTree::UpdateExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeNode(*this, node->_argument, node, nullptr);
    Type *type = outer_.getNodeTypeOrAny(node->_argument);
    if (llvh::isa<NumberType>(type->info) ||
        llvh::isa<BigIntType>(type->info)) {
      // number and bigint don't change.
      outer_.setNodeType(node, type);
      return;
    }
    if (auto *unionType = llvh::dyn_cast<UnionType>(type->info)) {
      if (llvh::all_of(unionType->getTypes(), [](Type *t) -> bool {
            return llvh::isa<NumberType>(t->info) ||
                llvh::isa<BigIntType>(t->info);
          })) {
        // Unions of number/bigint don't change.
        outer_.setNodeType(node, type);
        return;
      }
    }
    if (llvh::isa<AnyType>(type->info)) {
      // any remains any.
      outer_.setNodeType(node, outer_.flowContext_.getAny());
      return;
    }
    outer_.sm_.error(
        node->getSourceRange(),
        "ft: update expression must be number or bigint");
  }

  enum class BinopKind : uint8_t {
    // clang-format off
    eq, ne, strictEq, strictNe, lt, le, gt, ge, shl, sshr, ushr,
    plus, minus, mul, div, rem, binOr, binXor, binAnd, exp, in, instanceOf
    // clang-format on
  };

  static BinopKind binopKind(llvh::StringRef str) {
    return llvh::StringSwitch<BinopKind>(str)
        .Case("==", BinopKind::eq)
        .Case("!=", BinopKind::ne)
        .Case("===", BinopKind::strictEq)
        .Case("!==", BinopKind::strictNe)
        .Case("<", BinopKind::lt)
        .Case("<=", BinopKind::le)
        .Case(">", BinopKind::gt)
        .Case(">=", BinopKind::ge)
        .Case("<<", BinopKind::shl)
        .Case(">>", BinopKind::sshr)
        .Case(">>>", BinopKind::ushr)
        .Case("+", BinopKind::plus)
        .Case("-", BinopKind::minus)
        .Case("*", BinopKind::mul)
        .Case("/", BinopKind::div)
        .Case("%", BinopKind::rem)
        .Case("|", BinopKind::binOr)
        .Case("^", BinopKind::binXor)
        .Case("&", BinopKind::binAnd)
        .Case("**", BinopKind::exp)
        .Case("in", BinopKind::in)
        .Case("instanceof", BinopKind::instanceOf);
  }

  enum class UnopKind : uint8_t {
    // clang-format off
    del, voidOp, typeOf, plus, minus, tilde, bang, inc, dec
    // clang-format on
  };

  static UnopKind unopKind(llvh::StringRef str) {
    return llvh::StringSwitch<UnopKind>(str)
        .Case("delete", UnopKind::del)
        .Case("void", UnopKind::voidOp)
        .Case("typeof", UnopKind::typeOf)
        .Case("+", UnopKind::plus)
        .Case("-", UnopKind::minus)
        .Case("~", UnopKind::tilde)
        .Case("!", UnopKind::bang)
        .Case("++", UnopKind::inc)
        .Case("--", UnopKind::dec);
  }

  static BinopKind assignKind(llvh::StringRef str) {
    return llvh::StringSwitch<BinopKind>(str)
        .Case("<<=", BinopKind::shl)
        .Case(">>=", BinopKind::sshr)
        .Case(">>>=", BinopKind::ushr)
        .Case("+=", BinopKind::plus)
        .Case("-=", BinopKind::minus)
        .Case("*=", BinopKind::mul)
        .Case("/=", BinopKind::div)
        .Case("%=", BinopKind::rem)
        .Case("|=", BinopKind::binOr)
        .Case("^=", BinopKind::binXor)
        .Case("&=", BinopKind::binAnd)
        .Case("**=", BinopKind::exp);
  }

  /// \return nullptr if the operation is not supported.
  Type *determineBinopType(BinopKind op, TypeKind lk, TypeKind rk) {
    // Literal types behave like their widened base types for operators.
    lk = widenLiteralKind(lk);
    rk = widenLiteralKind(rk);

    struct BinTypes {
      BinopKind op;
      TypeKind res;
      // None indicates a wildcard, Any indicates the actual 'any' type.
      OptValue<TypeKind> left;
      OptValue<TypeKind> right;
    };

    static const BinTypes s_types[] = {
        // clang-format off
        // eq, ne, strictEq, and strictNe are handled specially.

        {BinopKind::lt, TypeKind::Boolean, TypeKind::Number, TypeKind::Number},
        {BinopKind::lt, TypeKind::Boolean, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::lt, TypeKind::Boolean, TypeKind::String, TypeKind::String},
        {BinopKind::lt, TypeKind::Boolean, TypeKind::Any, llvh::None},
        {BinopKind::lt, TypeKind::Boolean, llvh::None, TypeKind::Any},

        {BinopKind::le, TypeKind::Boolean, TypeKind::Number, TypeKind::Number},
        {BinopKind::le, TypeKind::Boolean, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::le, TypeKind::Boolean, TypeKind::String, TypeKind::String},
        {BinopKind::le, TypeKind::Boolean, TypeKind::Any, llvh::None},
        {BinopKind::le, TypeKind::Boolean, llvh::None, TypeKind::Any},

        {BinopKind::gt, TypeKind::Boolean, TypeKind::Number, TypeKind::Number},
        {BinopKind::gt, TypeKind::Boolean, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::gt, TypeKind::Boolean, TypeKind::String, TypeKind::String},
        {BinopKind::gt, TypeKind::Boolean, TypeKind::Any, llvh::None},
        {BinopKind::gt, TypeKind::Boolean, llvh::None, TypeKind::Any},

        {BinopKind::ge, TypeKind::Boolean, TypeKind::Number, TypeKind::Number},
        {BinopKind::ge, TypeKind::Boolean, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::ge, TypeKind::Boolean, TypeKind::String, TypeKind::String},
        {BinopKind::ge, TypeKind::Boolean, TypeKind::Any, llvh::None},
        {BinopKind::ge, TypeKind::Boolean, llvh::None, TypeKind::Any},

        {BinopKind::shl, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::shl, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::shl, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::shl, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::sshr, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::sshr, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::sshr, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::sshr, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::ushr, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::ushr, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::ushr, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::ushr, TypeKind::Any, llvh::None, TypeKind::Any},

        {BinopKind::plus, TypeKind::String, TypeKind::String, TypeKind::String},
        {BinopKind::plus, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::plus, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::plus, TypeKind::String, TypeKind::Any, TypeKind::String},
        {BinopKind::plus, TypeKind::String, TypeKind::String, TypeKind::Any},
        {BinopKind::plus, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::plus, TypeKind::Any, llvh::None, TypeKind::Any},

        {BinopKind::minus, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::minus, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::minus, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::minus, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::mul, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::mul, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::mul, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::mul, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::div, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::div, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::div, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::div, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::rem, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::rem, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::rem, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::rem, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::binOr, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::binOr, TypeKind::Number, TypeKind::Any, TypeKind::Number},
        {BinopKind::binOr, TypeKind::Number, TypeKind::Number, TypeKind::Any},
        {BinopKind::binOr, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::binOr, TypeKind::BigInt, TypeKind::Any, TypeKind::BigInt},
        {BinopKind::binOr, TypeKind::BigInt, TypeKind::BigInt, TypeKind::Any},
        {BinopKind::binOr, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::binOr, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::binXor, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::binXor, TypeKind::Number, TypeKind::Any, TypeKind::Number},
        {BinopKind::binXor, TypeKind::Number, TypeKind::Number, TypeKind::Any},
        {BinopKind::binXor, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::binXor, TypeKind::BigInt, TypeKind::Any, TypeKind::BigInt},
        {BinopKind::binXor, TypeKind::BigInt, TypeKind::BigInt, TypeKind::Any},
        {BinopKind::binXor, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::binXor, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::binAnd, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::binAnd, TypeKind::Number, TypeKind::Any, TypeKind::Number},
        {BinopKind::binAnd, TypeKind::Number, TypeKind::Number, TypeKind::Any},
        {BinopKind::binAnd, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::binAnd, TypeKind::BigInt, TypeKind::Any, TypeKind::BigInt},
        {BinopKind::binAnd, TypeKind::BigInt, TypeKind::BigInt, TypeKind::Any},
        {BinopKind::binAnd, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::binAnd, TypeKind::Any, TypeKind::Any, llvh::None},
        {BinopKind::exp, TypeKind::Number, TypeKind::Number, TypeKind::Number},
        {BinopKind::exp, TypeKind::BigInt, TypeKind::BigInt, TypeKind::BigInt},
        {BinopKind::exp, TypeKind::Any, llvh::None, TypeKind::Any},
        {BinopKind::exp, TypeKind::Any, TypeKind::Any, llvh::None},

        {BinopKind::in, TypeKind::Boolean, llvh::None, TypeKind::Any},
        {BinopKind::instanceOf, TypeKind::Boolean, llvh::None, TypeKind::ClassConstructor},
        {BinopKind::instanceOf, TypeKind::Boolean, llvh::None, TypeKind::UntypedFunction},
        {BinopKind::instanceOf, TypeKind::Boolean, llvh::None, TypeKind::Any},
        // clang-format on
    };
    static const BinTypes *const s_types_end =
        s_types + sizeof(s_types) / sizeof(s_types[0]);

    // Find the start of the section for this operator.
    auto it = std::lower_bound(
        s_types, s_types_end, op, [](const BinTypes &bt, BinopKind op) {
          return bt.op < op;
        });

    // Search for a match.
    for (; it != s_types_end && it->op == op; ++it) {
      if ((!it->left || *it->left == lk) && (!it->right || *it->right == rk)) {
        return outer_.flowContext_.getSingletonType(it->res);
      }
    }

    return nullptr;
  }

  void visit(
      ESTree::BinaryExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // Private brand checks ('#x in o') aren't supported: typed private names
    // aren't resolved to Decls that IRGen can use outside a member access.
    if (llvh::isa<ESTree::PrivateNameNode>(node->_left)) {
      outer_.sm_.error(
          node->_left->getSourceRange(),
          "ft: private brand checks ('#x in obj') are not supported");
      visitESTreeNode(*this, node->_right, node, nullptr);
      outer_.setNodeType(node, outer_.flowContext_.getBoolean());
      return;
    }

    visitESTreeNode(*this, node->_left, node, nullptr);
    visitESTreeNode(*this, node->_right, node, nullptr);
    Type *lt = outer_.getNodeTypeOrAny(node->_left);
    Type *rt = outer_.getNodeTypeOrAny(node->_right);

    BinopKind op = binopKind(node->_operator->str());

    // `===` and `!==` are handled specially.
    // They are only legal when one operand's type flows into the other's
    // without a checked cast. This is a conservative check: it rejects most
    // comparisons that are always false, but may also reject overlapping types
    // (e.g. unions sharing an arm) whose values could still be equal at
    // runtime. Still allows `?Foo === null`, subclass vs superclass, etc.
    if (op == BinopKind::strictEq || op == BinopKind::strictNe) {
      if (!outer_.canAFlowIntoB(lt->info, rt->info).canFlowWithoutCast() &&
          !outer_.canAFlowIntoB(rt->info, lt->info).canFlowWithoutCast()) {
        outer_.sm_.error(
            node->getSourceRange(),
            llvh::Twine("ft: ") + node->_operator->str() +
                " cannot be applied to " + lt->messageString() + " and " +
                rt->messageString());
      }
      outer_.setNodeType(node, outer_.flowContext_.getBoolean());
      return;
    }

    // `==` and `!=` are handled specially and are generally disallowed.
    // They are legal when either operand has type `any`, or when one operand
    // has type `null` or `void` and the other operand can hold `null` or
    // `void` without a checked cast. This permits `x == null` /
    // `x == undefined` on any nullable type.
    if (op == BinopKind::eq || op == BinopKind::ne) {
      auto *li = lt->info;
      auto *ri = rt->info;
      auto acceptsNullOrVoid = [this](TypeInfo *t) {
        return outer_.canAFlowIntoB(outer_.flowContext_.getNullInfo(), t)
                   .canFlowWithoutCast() ||
            outer_.canAFlowIntoB(outer_.flowContext_.getVoidInfo(), t)
                .canFlowWithoutCast();
      };
      bool ok = false;
      if (llvh::isa<AnyType>(li) || llvh::isa<AnyType>(ri)) {
        ok = true;
      } else if (llvh::isa<NullType>(li) || llvh::isa<VoidType>(li)) {
        ok = acceptsNullOrVoid(ri);
      } else if (llvh::isa<NullType>(ri) || llvh::isa<VoidType>(ri)) {
        ok = acceptsNullOrVoid(li);
      }
      if (!ok) {
        outer_.sm_.error(
            node->getSourceRange(),
            llvh::Twine("ft: ") + node->_operator->str() +
                " cannot be applied to " + lt->messageString() + " and " +
                rt->messageString() +
                " (use === / !== for general comparisons)");
      }
      outer_.setNodeType(node, outer_.flowContext_.getBoolean());
      return;
    }

    // Unions of literals (e.g. "a" | "b") behave like their base type.
    Type *res = determineBinopType(
        op,
        outer_.widenLiteralType(lt)->info->getKind(),
        outer_.widenLiteralType(rt)->info->getKind());
    if (!res) {
      outer_.sm_.error(
          node->getSourceRange(),
          llvh::Twine("ft: incompatible binary operation: ") +
              node->_operator->str() + " cannot be applied to " +
              lt->messageString() + " and " + rt->messageString());
      res = outer_.flowContext_.getAny();
    }

    outer_.setNodeType(node, res);
  }

  Type *determineUnopType(
      ESTree::UnaryExpressionNode *node,
      UnopKind op,
      TypeKind argKind) {
    // Literal types behave like their widened base types for operators.
    argKind = widenLiteralKind(argKind);

    struct UnTypes {
      UnopKind op;
      TypeKind res;
      // None indicates a wildcard, Any indicates the actual 'any' type.
      OptValue<TypeKind> arg;
    };

    static const UnTypes s_types[] = {
        // clang-format off
        {UnopKind::voidOp, TypeKind::Void, llvh::None},
        {UnopKind::typeOf, TypeKind::String, llvh::None},
        {UnopKind::plus, TypeKind::Number, TypeKind::Number},
        {UnopKind::plus, TypeKind::Number, TypeKind::Any},
        {UnopKind::minus, TypeKind::BigInt, TypeKind::BigInt},
        {UnopKind::minus, TypeKind::Number, TypeKind::Number},
        {UnopKind::minus, TypeKind::Any, TypeKind::Any},
        {UnopKind::tilde, TypeKind::Number, TypeKind::Number},
        {UnopKind::tilde, TypeKind::BigInt, TypeKind::BigInt},
        {UnopKind::tilde, TypeKind::Any, TypeKind::Any},
        {UnopKind::bang, TypeKind::Boolean, llvh::None},
        {UnopKind::inc, TypeKind::Number, TypeKind::Number},
        {UnopKind::inc, TypeKind::BigInt, TypeKind::BigInt},
        {UnopKind::inc, TypeKind::Any, TypeKind::Any},
        {UnopKind::dec, TypeKind::Number, TypeKind::Number},
        {UnopKind::dec, TypeKind::BigInt, TypeKind::BigInt},
        {UnopKind::dec, TypeKind::Any, TypeKind::Any},
        // clang-format on
    };
    static const UnTypes *const s_types_end =
        s_types + sizeof(s_types) / sizeof(s_types[0]);

    // Find the start of the section for this operator.
    auto it = std::lower_bound(
        s_types, s_types_end, op, [](const UnTypes &bt, UnopKind op) {
          return bt.op < op;
        });

    // Search for a match.
    for (; it != s_types_end && it->op == op; ++it) {
      if (!it->arg || *it->arg == argKind) {
        return outer_.flowContext_.getSingletonType(it->res);
      }
    }

    return nullptr;
  }

  void visit(
      ESTree::UnaryExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeNode(*this, node->_argument, node, nullptr);
    Type *argType = outer_.getNodeTypeOrAny(node->_argument);

    // Handle `delete` specially.
    if (node->_operator == outer_.kw_.identDelete) {
      outer_.setNodeType(node, outer_.flowContext_.getBoolean());

      if (auto *mem = llvh::dyn_cast<ESTree::MemberExpressionLikeNode>(
              node->_argument)) {
        Type *objType = outer_.getNodeTypeOrAny(ESTree::getObject(mem));
        // `delete` on an object with an indexer removes a key from the
        // dictionary without violating its uniform value type, so it is
        // allowed.
        bool hasIndexer = false;
        if (auto *exactObj = llvh::dyn_cast<ExactObjectType>(objType->info))
          hasIndexer = exactObj->hasIndexer();
        if (!hasIndexer && !llvh::isa<AnyType>(objType->info)) {
          // `delete` removes a property and so violates any non-`any` object's
          // declared shape.
          outer_.sm_.error(
              node->getSourceRange(),
              "ft: cannot delete property of typed object");
        }
      } else {
        outer_.sm_.error(
            node->getSourceRange(),
            "ft: 'delete' can only be applied to member expressions");
      }
      return;
    }

    Type *res;
    if (Type *t = determineUnopType(
            node,
            unopKind(node->_operator->str()),
            outer_.widenLiteralType(argType)->info->getKind())) {
      res = t;
    } else {
      outer_.sm_.error(
          node->getSourceRange(),
          llvh::Twine("ft: incompatible unary operation: ") +
              node->_operator->str() + " cannot be applied to " +
              argType->info->getKindName());
      res = outer_.flowContext_.getAny();
    }

    outer_.setNodeType(node, res);
  }

  void visit(
      ESTree::LogicalExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeNode(*this, node->_left, node, constraint);
    visitESTreeNode(*this, node->_right, node, constraint);
    Type *left = outer_.flowContext_.getNodeTypeOrAny(node->_left);
    Type *right = outer_.flowContext_.getNodeTypeOrAny(node->_right);

    auto hasNull = [](TypeInfo *t) -> bool {
      if (llvh::isa<NullType>(t))
        return true;
      if (auto *u = llvh::dyn_cast<UnionType>(t))
        return u->hasNull();
      return false;
    };

    auto hasVoid = [](TypeInfo *t) -> bool {
      if (llvh::isa<VoidType>(t))
        return true;
      if (auto *u = llvh::dyn_cast<UnionType>(t))
        return u->hasVoid();
      return false;
    };

    // The result of a logical expression is the union of both sides,
    // however if the operator is ?? or ||, then we can discard any null/void
    // that only appears on the left, because those can never be returned via
    // the left.
    FlowContext::UnionExcludes excludes{};
    if (node->_operator == outer_.kw_.identLogicalOr ||
        node->_operator == outer_.kw_.identNullishCoalesce) {
      if (hasVoid(left->info) && !hasVoid(right->info))
        excludes.excludeVoid = true;
      if (hasNull(left->info) && !hasNull(right->info))
        excludes.excludeNull = true;
    }

    Type *types[2]{
        outer_.flowContext_.getNodeTypeOrAny(node->_left),
        outer_.flowContext_.getNodeTypeOrAny(node->_right)};

    Type *unionType = outer_.flowContext_.createType(
        outer_.flowContext_.maybeCreateUnion(types, excludes));

    outer_.setNodeType(node, unionType);
  }

  enum class LogicalAssignmentOp : uint8_t {
    ShortCircuitOrKind, // ||=
    ShortCircuitAndKind, // &&=
    NullishCoalesceKind, // ??=
  };

  void visit(
      ESTree::AssignmentExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    auto logicalAssign =
        llvh::StringSwitch<OptValue<LogicalAssignmentOp>>(
            node->_operator->str())
            .Case("||=", LogicalAssignmentOp::ShortCircuitOrKind)
            .Case("&&=", LogicalAssignmentOp::ShortCircuitAndKind)
            .Case("\?\?=", LogicalAssignmentOp::NullishCoalesceKind)
            .Default(llvh::None);
    if (logicalAssign) {
      outer_.sm_.error(node->getSourceRange(), "ft: unsupported");
      return;
    }

    // Destructuring evaluates the RHS first. Use its type as the constraint
    // when typing the target pattern.
    bool destructuringLHS = node->_operator == outer_.kw_.identEqual &&
        llvh::isa<ESTree::PatternNode>(node->_left);

    Type *lhsConstraint = nullptr;
    if (destructuringLHS) {
      visitESTreeNode(*this, node->_right, node, nullptr);
      lhsConstraint = outer_.getNodeTypeOrAny(node->_right);
    }
    // For `=`, route through
    // visitAssignmentTarget so MemberExpression are classified as writes while
    // their inner _object subexpressions are still classified as reads.
    if (node->_operator == outer_.kw_.identEqual)
      visitAssignmentTarget(node->_left, node, lhsConstraint);
    else
      visitESTreeNode(*this, node->_left, node, lhsConstraint);

    // Handle the assignment side of the fact that we specially handle
    // 'length' for FastArrays.
    if (auto *mem = llvh::dyn_cast<ESTree::MemberExpressionNode>(node->_left)) {
      if (!mem->_computed) {
        auto *objType = outer_.getNodeTypeOrAny(mem->_object);
        // Private names on arrays were already rejected as undefined.
        auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(mem->_property);
        if (id && outer_.flowContext_.isArrayClassType(objType)) {
          outer_.sm_.error(
              node->getSourceRange(),
              "ft: cannot assign to property '" + id->_name->str() +
                  "' of typed Array");
          return;
        }
      }
    }

    Type *lt = outer_.getNodeTypeOrAny(node->_left);

    Type *res;
    if (node->_operator->str() == "=") {
      if (!destructuringLHS) {
        // Use the type of the LHS as the constraint for the RHS for '='.
        visitESTreeNode(*this, node->_right, node, lt);
      }
      Type *rt = outer_.getNodeTypeOrAny(node->_right);

      auto [rtNarrow, cf] = outer_.tryNarrowType(rt, lt);
      if (!cf.canFlow) {
        outer_.sm_.error(
            node->getSourceRange(),
            "ft: incompatible assignment type: cannot implicitly cast from " +
                rt->messageString() + " to " + lt->messageString());
        res = lt;
      } else {
        node->_right = outer_.implicitCheckedCast(node->_right, rtNarrow, cf);

        // If we don't need a checked cast, rt is possibly narrower than lt, but
        // never wider, so we want to use it as result.
        // This helps with cases like:
        //  let a: number|string, n: number; n = a = 5;
        res = cf.needCheckedCast ? lt : rt;
      }
    } else {
      visitESTreeNode(*this, node->_right, node, nullptr);
      Type *rt = outer_.getNodeTypeOrAny(node->_right);

      Type *opResType = determineBinopType(
          assignKind(node->_operator->str()),
          outer_.widenLiteralType(lt)->info->getKind(),
          outer_.widenLiteralType(rt)->info->getKind());

      if (!opResType) {
        outer_.sm_.error(
            node->getSourceRange(),
            llvh::Twine("ft: incompatible binary operation: ") +
                node->_operator->str() + " cannot be applied to " +
                lt->info->getKindName() + " and " + rt->info->getKindName());
        opResType = outer_.flowContext_.getAny();
      }

      if (llvh::isa<AnyType>(lt->info)) {
        // If the target we are assigning to is untyped, there are no checks
        // needed.
        res = opResType;
      } else {
        // We are modifying a typed target. The type has to be compatible.
        CanFlowResult cf = outer_.canAFlowIntoB(opResType, lt);
        if (!cf.canFlow) {
          outer_.sm_.error(
              node->getSourceRange(),
              "ft: incompatible assignment type: cannot implicitly cast from " +
                  opResType->messageString() + " to " + lt->messageString());
          res = lt;
        } else if (cf.needCheckedCast) {
          // Insert an ImplicitCheckedCast around the LHS in the
          // AssignmentExpressionNode, because there's no other place in the AST
          // that indicates that we want to cast the result of the binary
          // expression.
          // IRGen is aware of this and handles it specially.
          node->_left = outer_.implicitCheckedCast(node->_left, lt, cf);
          res = lt;
        } else {
          // If we don't need a checked cast, opResType is possibly narrower
          // than lt, but never wider, so we want to use it as result.
          res = opResType;
        }
      }
    }
    outer_.setNodeType(node, res);
  }

  void visit(
      ESTree::ArrayPatternNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // This is invoked for the LHS of an AssignmentExpression and for a
    // pattern nested in another pattern — variable declaration patterns are
    // handled directly by AnnotateScopeDecls.
    assert(
        !llvh::isa<ESTree::VariableDeclaratorNode>(parent) &&
        "use AnnotateScopeDecls for declarations");

    // The enclosing assignment (or pattern) forwards the source type as
    // \p constraint. We use it to type the pattern in a single pass, so IRGen
    // can dispatch on the pattern's own type. Each element is also checked
    // against the type it actually receives, because a nested pattern has no
    // enclosing assignment to do it afterwards.
    Type *src = destructuringSourceType(parent, constraint);

    // Errors inside the expansions below are already reported, and there is
    // nothing further to check either way.
    if (outer_.flowContext_.isArrayClassType(src)) {
      outer_.setNodeType(node, src);
      (void)outer_.expandArrayDestructuring(
          node, src, destructuringChildVisitor(node, "array"));
      return;
    }

    if (auto *tupleTy = llvh::dyn_cast<TupleType>(src->info)) {
      outer_.setNodeType(node, src);
      (void)outer_.expandTupleDestructuring(
          node, tupleTy, destructuringChildVisitor(node, "tuple"));
      return;
    }

    // A known source type that is none of the above cannot be destructured as
    // an array. Same diagnostic as the declaration path in AnnotateScopeDecls.
    if (!llvh::isa<AnyType>(src->info)) {
      outer_.sm_.error(
          node->getSourceRange(),
          "ft: incompatible type for array pattern, expected tuple or array");
      outer_.setNodeType(node, outer_.flowContext_.getAny());
      return;
    }

    // The source type is unknown. The elements come from the iterator protocol
    // at runtime, so the pattern has no static type and IRGen narrows each
    // store. All slots are write bindings; route each through
    // visitAssignmentTarget.
    outer_.setNodeType(node, outer_.flowContext_.getAny());
    for (ESTree::Node &child : node->_elements)
      visitAssignmentTarget(&child, node, nullptr);
  }

  /// Object destructuring LHS (`({x: a.b} = src)`) or a nested object pattern.
  /// The parser converts an object literal in an assignment LHS into an
  /// ObjectPatternNode. Each property's `_value` is a write target; a trailing
  /// `RestElement` argument is also a write binding. \p constraint is the
  /// source type the pattern destructures, when it is known.
  void visit(
      ESTree::ObjectPatternNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    Type *src = destructuringSourceType(parent, constraint);

    if (auto *objType = llvh::dyn_cast<ExactObjectType>(src->info)) {
      // Type the pattern from the source and check each property against the
      // field it receives.
      outer_.setNodeType(node, src);
      (void)outer_.expandObjectDestructuring(
          node, objType, destructuringChildVisitor(node, "object"));
      return;
    }

    // A known source type that is not an object cannot be destructured. Same
    // diagnostic as the declaration path in AnnotateScopeDecls.
    if (!llvh::isa<AnyType>(src->info)) {
      outer_.sm_.error(
          node->getSourceRange(),
          "ft: incompatible type for object pattern, expected object type");
      outer_.setNodeType(node, outer_.flowContext_.getAny());
      return;
    }

    // The source type is unknown or 'any'; leave the pattern untyped and just
    // annotate the write targets.
    for (ESTree::Node &child : node->_properties) {
      if (auto *prop = llvh::dyn_cast<ESTree::PropertyNode>(&child)) {
        // The key is read as a property name; not a value position.
        visitESTreeNodeNoReplace(*this, prop->_key, prop, nullptr);
        visitAssignmentTarget(prop->_value, prop, nullptr);
      } else if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&child)) {
        visitAssignmentTarget(rest->_argument, rest, nullptr);
      } else {
        visitAssignmentTarget(&child, node, nullptr);
      }
    }
  }

  /// Default-value pattern (`{a = 1} = obj`, `[a = 1] = arr`). Always
  /// invoked inside a destructuring write position, so `_left` is a write
  /// target and `_right` is a read.
  void visit(
      ESTree::AssignmentPatternNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitAssignmentTarget(node->_left, node, constraint);
    Type *targetType = outer_.getNodeTypeOrAny(node->_left);
    visitESTreeNode(*this, node->_right, node, targetType);
    // The default is stored into the target when the source property is
    // missing, so it has to be assignable to it. This applies to a nested
    // pattern target too: it was typed from the source, and the default takes
    // the source's place. A pattern target requires an exact flow, because
    // nothing narrows the value before it is destructured -- see
    // checkDestructuringTarget.
    Type *defaultType = outer_.getNodeTypeOrAny(node->_right);
    CanFlowResult cf = outer_.tryNarrowType(defaultType, targetType).second;
    if (!cf.canFlow ||
        (cf.needCheckedCast && llvh::isa<ESTree::PatternNode>(node->_left))) {
      outer_.sm_.error(
          node->_right->getSourceRange(),
          "ft: incompatible destructuring default type: cannot implicitly "
          "cast from " +
              defaultType->messageString() + " to " +
              targetType->messageString());
    }
    outer_.setNodeType(node, targetType);
  }

  /// Rest binding (`[...rest]` or `{...rest}` LHS). The argument is a
  /// write target.
  void
  visit(ESTree::RestElementNode *node, ESTree::Node *parent, Type *constraint) {
    visitAssignmentTarget(node->_argument, node, constraint);
  }

  void visit(
      ESTree::ConditionalExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    visitESTreeNode(*this, node->_test, node, nullptr);
    visitESTreeNode(*this, node->_consequent, node, constraint);
    visitESTreeNode(*this, node->_alternate, node, constraint);

    Type *types[2]{
        outer_.getNodeTypeOrAny(node->_consequent),
        outer_.getNodeTypeOrAny(node->_alternate)};

    // The result of a conditional is the union of the two types.
    outer_.setNodeType(
        node,
        outer_.flowContext_.createType(
            outer_.flowContext_.maybeCreateUnion(types)));
  }

  void visit(
      ESTree::TypeParameterInstantiationNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // Do nothing.
    // These are handled in the parent node.
  }

  /// Look up the class field referenced by a named method call on a class.
  /// \pre \p callee must not be computed.
  /// \return the found Field, nullptr if none could be found.
  const ClassType::Field *lookupCallMemberField(
      ESTree::MemberExpressionNode *callee,
      Type *objType) {
    assert(!callee->_computed && "property must be non-computed");
    bool isPrivate;
    Identifier name;
    if (auto *pn = llvh::dyn_cast<ESTree::PrivateNameNode>(callee->_property)) {
      isPrivate = true;
      name = outer_.astContext_.getPrivateNameIdentifier(
          llvh::cast<ESTree::IdentifierNode>(pn->_id)->_name);
    } else {
      isPrivate = false;
      name = Identifier::getFromPointer(
          llvh::cast<ESTree::IdentifierNode>(callee->_property)->_name);
    }

    // Home objects (i.e. prototype receivers) are deliberately excluded. They
    // have no home object of their own, and the members the callers care about
    // (overloaded, generic and final methods) are exactly the ones not stored
    // on the prototype, so the access is diagnosed by
    // resolveMemberExpressionType and the callee type is any.
    if (auto *classType = llvh::dyn_cast<ClassType>(objType->info);
        classType && !classType->isHomeObject()) {
      return outer_.lookupPropertyOnClass(classType, name, callee->_property)
          .second;
    }

    auto *consType = llvh::dyn_cast<ClassConstructorType>(objType->info);
    if (!consType)
      return nullptr;
    auto *classTypeInfo = llvh::cast<ClassType>(consType->getClassType()->info);
    auto *staticInfo = classTypeInfo->getStaticObjectTypeInfo();
    if (!staticInfo)
      return nullptr;
    auto field = isPrivate ? staticInfo->findPrivateField(name)
                           : staticInfo->findPublicField(name);
    return field ? field->getField() : nullptr;
  }

  void visit(
      ESTree::CallExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // Check for $SHBuiltin.
    if (auto *methodCallee =
            llvh::dyn_cast<ESTree::MemberExpressionNode>(node->_callee)) {
      if (llvh::isa<ESTree::SHBuiltinNode>(methodCallee->_object)) {
        checkSHBuiltin(
            node, llvh::cast<ESTree::IdentifierNode>(methodCallee->_property));
        return;
      }
    }

    // Whether we have to visit the arguments or not depends on whether we
    // visited them during generic type argument inference.
    bool shouldVisitArguments = true;
    // Whether we need to visit the callee. Set to false when the callee has
    // already been visited above.
    bool shouldVisitCallee = true;
    // Whether overload resolution was already performed (e.g. via explicit
    // type arguments). Skip the later overloaded method check if true.
    bool overloadResolved = false;

    // Check for fn.call(thisArg, args...) on a function-typed receiver.
    if (auto *methodCallee =
            llvh::dyn_cast<ESTree::MemberExpressionNode>(node->_callee);
        methodCallee && !methodCallee->_computed &&
        !llvh::isa<ESTree::SuperNode>(methodCallee->_object)) {
      if (auto *propId =
              llvh::dyn_cast<ESTree::IdentifierNode>(methodCallee->_property);
          propId && propId->_name == outer_.kw_.identCall) {
        // Visit just the object so we can inspect its type.
        visitESTreeNode(*this, methodCallee->_object, methodCallee, nullptr);
        Type *objType = outer_.getNodeTypeOrAny(methodCallee->_object);
        if (llvh::isa<BaseFunctionType>(objType->info)) {
          if (node->_typeArguments) {
            outer_.sm_.error(
                node->_typeArguments->getSourceRange(),
                "ft: type arguments not allowed on function.call");
            return;
          }
          checkFunctionPrototypeCall(node, methodCallee->_object, objType);
          return;
        }
        // Receiver isn't a function: finish resolving the member expression
        // (without re-visiting the object) and let the regular path emit the
        // appropriate diagnostic.
        resolveMemberExpressionType(methodCallee, node, /*isWrite*/ false);
        shouldVisitCallee = false;
      }
    }

    // Handle generic calls and method calls.

    if (auto *identCallee =
            llvh::dyn_cast<ESTree::IdentifierNode>(node->_callee)) {
      sema::Decl *decl = outer_.getDecl(identCallee);
      if (decl->generic) {
        // The number of explicitly provided type arguments (0 if none).
        size_t numProvided = node->_typeArguments
            ? llvh::cast<ESTree::TypeParameterInstantiationNode>(
                  node->_typeArguments)
                  ->_params.size()
            : 0;
        if (node->_typeArguments &&
            numProvided >= outer_.getGenericInfoMustExist(decl).numTypeParams) {
          // All type arguments provided (or too many, which errors during
          // specialization): specialize directly.
          outer_.resolveCallToGenericFunctionSpecialization(
              node, identCallee, decl);
        } else {
          // No or partial type arguments: infer the missing trailing ones.
          auto [visited, typeArgs] =
              outer_.inferTypeArgumentsForGenericFunctionCall(
                  node, identCallee, decl);
          if (visited)
            shouldVisitArguments = false;
          if (typeArgs.empty()) {
            // Partial explicit type args already reported what's missing.
            if (!visited || !node->_typeArguments)
              outer_.sm_.error(
                  node->getStartLoc(),
                  "ft: could not infer type arguments for generic function");
            return;
          }
          outer_.resolveCallToGenericFunctionSpecializationWithParsedTypes(
              node, identCallee, typeArgs, decl);
        }
      }
    } else if (auto *memCallee =
                   llvh::dyn_cast<ESTree::MemberExpressionNode>(node->_callee);
               memCallee && node->_typeArguments && !memCallee->_computed) {
      // Handle explicit type arguments on generic method calls.
      // Visit the object early to determine the class type, unless the
      // callee has already been visited above (in which case its object
      // has been too).
      if (shouldVisitCallee)
        visitESTreeNode(*this, memCallee->_object, memCallee, nullptr);
      shouldVisitCallee = false;
      Type *objType = outer_.getNodeTypeOrAny(memCallee->_object);
      const ClassType::Field *field = lookupCallMemberField(memCallee, objType);

      if (field) {
        if (field->isOverloaded()) {
          auto *typeArgsNode =
              llvh::cast<ESTree::TypeParameterInstantiationNode>(
                  node->_typeArguments);
          if (!resolveOverloadedMethodCall(
                  node, memCallee, field, typeArgsNode))
            return;
          shouldVisitArguments = false;
          overloadResolved = true;
        } else if (llvh::isa<GenericType>(field->type->info)) {
          size_t numProvided =
              llvh::cast<ESTree::TypeParameterInstantiationNode>(
                  node->_typeArguments)
                  ->_params.size();
          if (numProvided >= outer_.getGenericMethodInfoMustExist(field->method)
                                 .numTypeParams) {
            // All type arguments provided (or too many, which errors during
            // specialization): specialize directly.
            outer_.resolveCallToGenericMethodSpecialization(
                node, memCallee, field->method);
          } else {
            // Partial type arguments: infer the missing trailing ones.
            auto [didVisitArgs, typeArgs] =
                outer_.inferTypeArgumentsForGenericMethodCall(
                    node, memCallee, field->method);
            if (didVisitArgs)
              shouldVisitArguments = false;
            if (typeArgs.empty()) {
              // The missing type argument was already reported.
              if (!didVisitArgs)
                outer_.sm_.error(
                    node->getStartLoc(),
                    "ft: could not infer type arguments for generic method");
              return;
            }
            outer_.resolveCallToGenericMethodSpecializationWithParsedTypes(
                node, memCallee, typeArgs, field->method);
          }
        } else {
          outer_.sm_.error(
              node->_typeArguments->getSourceRange(),
              "ft: type arguments provided for non-generic method");
          return;
        }
      } else if (llvh::isa<ClassType>(objType->info)) {
        // Nothing to specialize: handle unique cases like C.prototype.
        resolveMemberExpressionType(memCallee, node, /*isWrite*/ false);
      }
    } else if (node->_typeArguments) {
      // Generics handled above.
      outer_.sm_.error(
          node->_callee->getSourceRange(), "ft: invalid generic function call");
      return;
    }

    // Don't visit the arguments yet, since we may be able to constrain their
    // types using the type of the function.
    if (shouldVisitCallee) {
      visitESTreeNode(*this, node->_callee, node, nullptr);
    }
    Type *calleeType = outer_.getNodeTypeOrAny(node->_callee);

    // Handle overloaded method calls if they haven't been resolved based on
    // explicit type arguments.
    // Check before generic inference since some overloads may be generic.
    if (auto *methodCallee =
            llvh::dyn_cast<ESTree::MemberExpressionNode>(node->_callee);
        !overloadResolved && methodCallee && !methodCallee->_computed) {
      Type *objType = outer_.getNodeTypeOrAny(methodCallee->_object);
      const ClassType::Field *field =
          lookupCallMemberField(methodCallee, objType);
      if (field && field->isOverloaded()) {
        if (!resolveOverloadedMethodCall(node, methodCallee, field))
          return;
        shouldVisitArguments = false;
        calleeType = outer_.getNodeTypeOrAny(node->_callee);
      }
    }

    // Handle generic method inference (no explicit type arguments).
    // After visiting the callee, if the type is GenericType, attempt to
    // infer the type arguments from the call arguments.
    if (llvh::isa<GenericType>(calleeType->info)) {
      if (auto *memCallee =
              llvh::dyn_cast<ESTree::MemberExpressionNode>(node->_callee);
          memCallee && !memCallee->_computed) {
        Type *objType = outer_.getNodeTypeOrAny(memCallee->_object);
        const ClassType::Field *field =
            lookupCallMemberField(memCallee, objType);
        if (field && field->method) {
          auto [didVisitArgs, typeArgs] =
              outer_.inferTypeArgumentsForGenericMethodCall(
                  node, memCallee, field->method);
          if (didVisitArgs)
            shouldVisitArguments = false;
          if (typeArgs.empty()) {
            outer_.sm_.error(
                node->getStartLoc(),
                "ft: could not infer type arguments for generic method");
            return;
          }
          outer_.resolveCallToGenericMethodSpecializationWithParsedTypes(
              node, memCallee, typeArgs, field->method);
          calleeType = outer_.getNodeTypeOrAny(node->_callee);
        }
      }
    }

    // Handle builtin method calls.
    // Must be after visiting the callee since setBuiltinMethodDecl is called
    // during MemberExpression visit.
    if (auto *methodCallee =
            llvh::dyn_cast<ESTree::MemberExpressionNode>(node->_callee)) {
      if (auto *builtinDecl = outer_.getBuiltinMethodDecl(methodCallee)) {
        if (outer_.resolveBuiltinMethodCall(node, methodCallee, builtinDecl))
          shouldVisitArguments = false;
        calleeType = outer_.getNodeTypeOrAny(node->_callee);
      }
    }

    // If the callee has no type, we have nothing to do/check.
    if (llvh::isa<AnyType>(calleeType->info)) {
      visitESTreeNodeList(*this, node->_arguments, node, nullptr);
      checkMixedSpreads(node->_arguments);
      return;
    }

    // A plain call `X(...)` on a typed class dispatches to the class's static
    // `SH_call` method, if it declares one. This allows a class to be invoked
    // without `new` (e.g. `Symbol()`). The callee must name a class
    // declaration, since dispatch is resolved statically and IRGen loads
    // SH_call from its decl. If there is no usable `SH_call`, fall through to
    // the regular "callee is not a function" error below.
    auto *calleeIdent = llvh::dyn_cast<ESTree::IdentifierNode>(node->_callee);
    sema::Decl *calleeDecl =
        calleeIdent ? outer_.getDecl(calleeIdent) : nullptr;
    if (auto *consType = llvh::dyn_cast<ClassConstructorType>(calleeType->info);
        consType && calleeDecl && calleeDecl->kind == sema::Decl::Kind::Class) {
      ClassType *classInfo = consType->getClassTypeInfo();
      ClassType *staticInfo = classInfo->getStaticObjectTypeInfo();
      OptValue<ClassType::FieldLookupEntry> optCall;
      if (staticInfo)
        optCall = staticInfo->findPublicField(
            Identifier::getFromPointer(outer_.kw_.identSHCall));
      // Only a genuine static method (not a data field or accessor named
      // SH_call) can be dispatched to. A data field of function type would
      // otherwise satisfy the TypedFunctionType check below but crash IRGen,
      // which assumes a method.
      if (optCall && optCall->getField()->isMethod() &&
          !optCall->getField()->isAccessor()) {
        const ClassType::Field *field = optCall->getField();
        if (field->isOverloaded()) {
          // Overloaded SH_call requires extra work to support because we don't
          // have an extra IdentifierNodes to use to store overload data.
          // Leave it unsupported for now.
          outer_.sm_.error(
              node->getStartLoc(),
              "ft: overloaded SH_call method not supported");
          outer_.setNodeType(node, outer_.flowContext_.getAny());
          return;
        }

        if (auto *ftype =
                llvh::dyn_cast<TypedFunctionType>(field->type->info)) {
          llvh::ArrayRef<TypedFunctionType::Param> params = ftype->getParams();
          if (shouldVisitArguments)
            visitCallArgumentsWithConstraints(node->_arguments, node, params);
          outer_.setNodeType(node, ftype->getReturnType());
          checkCallArgumentTypes(params, node, node->_arguments, "function");
          return;
        }
      }
    }

    if (!llvh::isa<BaseFunctionType>(calleeType->info)) {
      outer_.sm_.error(
          node->_callee->getSourceRange(), "ft: callee is not a function");
      return;
    }

    // If the callee is an untyped function, we have nothing to check.
    if (llvh::isa<UntypedFunctionType>(calleeType->info)) {
      visitESTreeNodeList(*this, node->_arguments, node, nullptr);
      checkMixedSpreads(node->_arguments);
      outer_.setNodeType(node, outer_.flowContext_.getAny());
      return;
    }

    Type *returnType;
    llvh::ArrayRef<TypedFunctionType::Param> params{};

    if (auto *ftype = llvh::dyn_cast<TypedFunctionType>(calleeType->info)) {
      returnType = ftype->getReturnType();
      params = ftype->getParams();
    } else {
      auto *nftype = llvh::cast<NativeFunctionType>(calleeType->info);
      returnType = nftype->getReturnType();
      params = nftype->getParams();
    }

    if (shouldVisitArguments)
      visitCallArgumentsWithConstraints(node->_arguments, node, params);

    if (auto *ftype = llvh::dyn_cast<TypedFunctionType>(calleeType->info)) {
      Type *expectedThisType = ftype->getThisParam()
          ? ftype->getThisParam()
          : outer_.flowContext_.getAny();

      // Check the type of "this".
      if (auto *methodCallee =
              llvh::dyn_cast<ESTree::MemberExpressionNode>(node->_callee)) {
        Type *thisArgType = nullptr;
        if (llvh::isa<ESTree::SuperNode>(methodCallee->_object)) {
          // 'super' calls implicitly pass the current class as 'this'.
          if (!outer_.curClassContext_->classType) {
            outer_.sm_.error(
                node->_callee->getSourceRange(),
                "ft: 'super' call outside class");
            return;
          }
          // A super member access keeps 'this' as the receiver, which in a
          // static context is the class constructor rather than an instance.
          // That distinction is already baked into the enclosing function's
          // 'this' type.
          thisArgType = outer_.curFunctionContext_->thisParamType;
        } else {
          thisArgType = outer_.getNodeTypeOrAny(methodCallee->_object);
        }

        if (!outer_.canAFlowIntoB(thisArgType->info, expectedThisType->info)
                 .canFlow) {
          outer_.sm_.error(
              methodCallee->getSourceRange(), "ft: 'this' type mismatch");
          return;
        }
      } else if (llvh::isa<ESTree::SuperNode>(node->_callee)) {
        // 'super' calls implicitly pass the current class as 'this'.
        if (!outer_.curClassContext_->classType) {
          outer_.sm_.error(
              node->_callee->getSourceRange(),
              "ft: 'super' call outside class");
          return;
        }
        if (!outer_
                 .canAFlowIntoB(
                     outer_.curClassContext_->classType->info,
                     expectedThisType->info)
                 .canFlow) {
          outer_.sm_.error(
              node->_callee->getSourceRange(), "ft: 'this' type mismatch");
          return;
        }
      } else {
        if (!outer_
                 .canAFlowIntoB(
                     outer_.flowContext_.getVoid()->info,
                     expectedThisType->info)
                 .canFlow) {
          outer_.sm_.error(
              node->_callee->getSourceRange(), "ft: 'this' type mismatch");
          return;
        }
      }
    }

    outer_.setNodeType(node, returnType);
    checkCallArgumentTypes(params, node, node->_arguments, "function");
  }

  void checkSHBuiltin(
      ESTree::CallExpressionNode *call,
      ESTree::IdentifierNode *builtin) {
    if (builtin->_name == outer_.kw_.identCall) {
      checkSHBuiltinCall(call);
      return;
    }
    if (builtin->_name == outer_.kw_.identCNull) {
      checkSHBuiltinCNull(call);
      return;
    }
    if (builtin->_name == outer_.kw_.identCNativeRuntime) {
      checkSHBuiltinCNativeRuntime(call);
      return;
    }
    if (builtin->_name == outer_.kw_.identExternC) {
      checkSHBuiltinExternC(call);
      return;
    }
    if (builtin->_name == outer_.kw_.identModuleFactory) {
      checkSHBuiltinModuleFactory(call);
      return;
    }
    if (builtin->_name == outer_.kw_.identExport) {
      checkSHBuiltinExport(call);
      return;
    }
    if (builtin->_name == outer_.kw_.identImport) {
      checkSHBuiltinImport(call);
      return;
    }

    if (builtin->_name == outer_.kw_.identFastArrayPop) {
      checkSHBuiltinFastArrayPop(call);
      return;
    }

    if (builtin->_name == outer_.kw_.identFastArrayLength) {
      checkSHBuiltinFastArrayLength(call);
      return;
    }

    if (builtin->_name == outer_.kw_.identObjectKeys) {
      checkSHBuiltinObjectProps(call, "objectKeys");
      return;
    }

    if (builtin->_name == outer_.kw_.identObjectValues) {
      checkSHBuiltinObjectProps(call, "objectValues");
      return;
    }

    if (builtin->_name == outer_.kw_.identObjectEntries) {
      checkSHBuiltinObjectProps(call, "objectEntries");
      return;
    }

    outer_.sm_.error(call->getSourceRange(), "unknown SH builtin call");
  }

  /// $SHBuiltin.objectKeys(o): Array<string>,
  /// $SHBuiltin.objectValues(o): Array<mixed>,
  /// $SHBuiltin.objectEntries(o): Array<[string, mixed]>.
  /// These cover only the *enumerable own* properties of \p o, as
  /// Object.keys/values/entries do. Other ways of listing properties
  /// (own non-enumerable, symbols, the prototype chain) do not share this
  /// setup and need their own builtins.
  void checkSHBuiltinObjectProps(
      ESTree::CallExpressionNode *call,
      llvh::StringRef name) {
    visitESTreeChildren(*this, call, nullptr);
    if (call->_arguments.size() != 1) {
      outer_.sm_.error(
          call->getSourceRange(),
          llvh::Twine("ft: ") + name + " requires exactly one argument");
      return;
    }
    // The implementation reads the object's own properties directly, so the
    // argument must statically be an object.
    ESTree::Node *objArg = &call->_arguments.front();
    if (!llvh::isa<ExactObjectType>(outer_.getNodeTypeOrAny(objArg)->info)) {
      outer_.sm_.error(
          objArg->getSourceRange(),
          llvh::Twine("ft: ") + name + " argument must be an object");
      return;
    }
    Type *elemType;
    if (name == "objectKeys") {
      elemType = outer_.flowContext_.getString();
    } else if (name == "objectValues") {
      elemType = outer_.flowContext_.getMixed();
    } else {
      elemType = outer_.flowContext_.createType(outer_.flowContext_.createTuple(
          {outer_.flowContext_.getString(), outer_.flowContext_.getMixed()}));
    }
    Type *resType =
        outer_.getSpecializedArrayClassType(elemType, call->getSourceRange());
    if (!resType) {
      outer_.sm_.error(
          call->getSourceRange(),
          llvh::Twine("ft: ") + name + " cannot create the result array type");
      return;
    }
    outer_.setNodeType(call, resType);
  }

  /// $SHBuiltin.fastArrayLength(arr: Array<T>): number.
  void checkSHBuiltinFastArrayLength(ESTree::CallExpressionNode *call) {
    visitESTreeChildren(*this, call, nullptr);
    if (call->_arguments.size() != 1) {
      outer_.sm_.error(
          call->getSourceRange(),
          "ft: fastArrayLength requires exactly one argument");
      return;
    }
    ESTree::Node *arrArg = &call->_arguments.front();
    Type *argType = outer_.getNodeTypeOrAny(arrArg);
    if (!outer_.flowContext_.isArrayClassType(argType)) {
      outer_.sm_.error(
          arrArg->getSourceRange(),
          "ft: fastArrayLength argument must be an array");
      return;
    }
    outer_.setNodeType(call, outer_.flowContext_.getNumber());
  }

  /// $SHBuiltin.fastArrayPop(arr: Array<T>, n: number): T | void.
  void checkSHBuiltinFastArrayPop(ESTree::CallExpressionNode *call) {
    visitESTreeChildren(*this, call, nullptr);
    if (call->_arguments.size() != 2) {
      outer_.sm_.error(
          call->getSourceRange(),
          "ft: fastArrayPop requires exactly two arguments");
      return;
    }
    auto it = call->_arguments.begin();
    ESTree::Node *arrArg = &*it++;
    ESTree::Node *countArg = &*it;
    Type *argType = outer_.getNodeTypeOrAny(arrArg);
    if (!outer_.flowContext_.isArrayClassType(argType)) {
      outer_.sm_.error(
          arrArg->getSourceRange(),
          "ft: fastArrayPop argument must be an array");
      return;
    }
    Type *countType = outer_.getNodeTypeOrAny(countArg);
    if (!llvh::isa<NumberType>(countType->info) &&
        !llvh::isa<NumberLiteralType>(countType->info) &&
        !llvh::isa<AnyType>(countType->info)) {
      outer_.sm_.error(
          countArg->getSourceRange(),
          "ft: fastArrayPop count argument must be a number");
      return;
    }
    Type *elemType = outer_.flowContext_.getArrayElementType(argType);
    Type *voidType = outer_.flowContext_.getVoid();
    Type *resType = outer_.flowContext_.createType(
        outer_.flowContext_.maybeCreateUnion({elemType, voidType}));
    outer_.setNodeType(call, resType);
  }

  /// $SHBuiltin.call(fn, this, arg1, ...)
  /// must typecheck as an actual function call.
  void checkSHBuiltinCall(ESTree::CallExpressionNode *call) {
    auto it = call->_arguments.begin();
    if (it == call->_arguments.end()) {
      outer_.sm_.error(
          call->getSourceRange(), "ft: call requires at least two arguments");
      return;
    }
    // The callee and 'this' are consumed positionally.
    if (!checkSpreadInLeadingArgs(call, 2))
      return;
    ESTree::Node *callee = &*it;
    visitESTreeNode(*this, callee, call, nullptr);

    /// Visit the rest of the arguments without any constraints,
    /// starting at \c it.
    auto visitRemainingArgumentsWithoutConstraint =
        [this, &it, call]() -> void {
      for (auto e = call->_arguments.end(); it != e; ++it) {
        ESTree::Node *arg = &*it;
        visitESTreeNode(*this, arg, call, nullptr);
      }
    };

    Type *calleeType = outer_.getNodeTypeOrAny(callee);
    // If the callee has no type, we have nothing to do/check.
    if (llvh::isa<AnyType>(calleeType->info)) {
      ++it;
      visitRemainingArgumentsWithoutConstraint();
      flattenedArgCount(call->_arguments, 2, /* reportErrors */ true);
      return;
    }
    if (!llvh::isa<BaseFunctionType>(calleeType->info)) {
      ++it;
      visitRemainingArgumentsWithoutConstraint();
      outer_.sm_.error(
          callee->getSourceRange(), "ft: callee is not a function");
      return;
    }
    if (llvh::isa<NativeFunctionType>(calleeType->info)) {
      ++it;
      visitRemainingArgumentsWithoutConstraint();
      outer_.sm_.error(
          callee->getSourceRange(),
          "ft: callee is a native function, cannot use $SHBuiltin.call");
      return;
    }
    auto *ftype = llvh::dyn_cast<TypedFunctionType>(calleeType->info);

    // If the callee is an untyped function, we have nothing to check.
    if (!ftype) {
      ++it;
      visitRemainingArgumentsWithoutConstraint();
      flattenedArgCount(call->_arguments, 2, /* reportErrors */ true);
      outer_.setNodeType(call, outer_.flowContext_.getAny());
      return;
    }

    outer_.setNodeType(call, ftype->getReturnType());

    ++it;
    if (it == call->_arguments.end()) {
      outer_.sm_.error(
          call->getSourceRange(), "ft: call requires at least two arguments");
      return;
    }

    // Visit thisArg with no parameter constraint (it is bound to 'this',
    // not to a regular parameter).
    ESTree::Node *thisArg = &*it;
    visitESTreeNodeNoReplace(*this, thisArg, call, nullptr);

    visitCallArgumentsWithConstraints(
        call->_arguments, call, ftype->getParams(), 2);

    Type *expectedThisType = ftype->getThisParam()
        ? ftype->getThisParam()
        : outer_.flowContext_.getAny();
    Type *thisArgType = outer_.getNodeTypeOrAny(thisArg);
    if (!outer_.canAFlowIntoB(thisArgType->info, expectedThisType->info)
             .canFlow) {
      outer_.sm_.error(thisArg->getSourceRange(), "ft: 'this' type mismatch");
      return;
    }

    checkCallArgumentTypes(
        ftype->getParams(), call, call->_arguments, "function", 2);
    return;
  }

  /// Typecheck \c fn.call(thisArg, args...) on a function-typed receiver.
  /// Precondition: \p fn has been visited and \p fnType is its resolved
  /// BaseFunctionType.
  void checkFunctionPrototypeCall(
      ESTree::CallExpressionNode *call,
      ESTree::Node *fn,
      Type *fnType) {
    // 'this' is consumed positionally.
    if (!checkSpreadInLeadingArgs(call, 1))
      return;

    auto it = call->_arguments.begin();

    /// Visit the rest of the arguments without any constraints,
    /// starting at \c it.
    auto visitRemainingArgumentsWithoutConstraint =
        [this, &it, call]() -> void {
      for (auto e = call->_arguments.end(); it != e; ++it) {
        ESTree::Node *arg = &*it;
        visitESTreeNode(*this, arg, call, nullptr);
      }
    };

    assert(
        llvh::isa<BaseFunctionType>(fnType->info) &&
        "checkFunctionPrototypeCall requires a BaseFunctionType receiver");
    if (llvh::isa<NativeFunctionType>(fnType->info)) {
      visitRemainingArgumentsWithoutConstraint();
      outer_.sm_.error(
          fn->getSourceRange(),
          "ft: callee is a native function, cannot use function.call");
      return;
    }
    auto *ftype = llvh::dyn_cast<TypedFunctionType>(fnType->info);

    // If the receiver is an untyped function, we have nothing to check.
    if (!ftype) {
      visitRemainingArgumentsWithoutConstraint();
      flattenedArgCount(call->_arguments, 1, /* reportErrors */ true);
      outer_.setNodeType(call, outer_.flowContext_.getAny());
      return;
    }

    outer_.setNodeType(call, ftype->getReturnType());

    if (it == call->_arguments.end()) {
      outer_.sm_.error(
          call->getSourceRange(),
          "ft: function.call requires a 'this' argument");
      return;
    }

    // Visit thisArg with no parameter constraint (it is bound to 'this',
    // not to a regular parameter).
    ESTree::Node *thisArg = &*it;
    visitESTreeNodeNoReplace(*this, thisArg, call, nullptr);

    // Visit remaining args with constraints from the function's parameters.
    visitCallArgumentsWithConstraints(
        call->_arguments, call, ftype->getParams(), 1);

    Type *expectedThisType = ftype->getThisParam()
        ? ftype->getThisParam()
        : outer_.flowContext_.getAny();
    Type *thisArgType = outer_.getNodeTypeOrAny(thisArg);
    if (!outer_.canAFlowIntoB(thisArgType->info, expectedThisType->info)
             .canFlow) {
      outer_.sm_.error(thisArg->getSourceRange(), "ft: 'this' type mismatch");
      return;
    }

    checkCallArgumentTypes(
        ftype->getParams(), call, call->_arguments, "function.call", 1);
    return;
  }

  /// SHBuiltin.c_null().
  void checkSHBuiltinCNull(ESTree::CallExpressionNode *call) {
    visitESTreeChildren(*this, call, nullptr);
    // Check the number and types of arguments.
    if (call->_arguments.size() != 0) {
      outer_.sm_.error(call->getSourceRange(), "ft: c_null takes no arguments");
      return;
    }
    outer_.setNodeType(call, outer_.flowContext_.getCPtr());
  }

  /// SHBuiltin.c_native_runtime().
  void checkSHBuiltinCNativeRuntime(ESTree::CallExpressionNode *call) {
    visitESTreeChildren(*this, call, nullptr);
    // Check the number and types of arguments.
    if (call->_arguments.size() != 0) {
      outer_.sm_.error(
          call->getSourceRange(), "ft: c_native_runtime takes no arguments");
      return;
    }
    outer_.setNodeType(call, outer_.flowContext_.getCPtr());
  }

  /// $SHBuiltin.extern_c({options}, function name():result {...})
  void checkSHBuiltinExternC(ESTree::CallExpressionNode *call) {
    visitESTreeChildren(*this, call, nullptr);
    // Check the number and types of arguments.
    if (call->_arguments.size() != 2) {
      outer_.sm_.error(
          call->getSourceRange(),
          "ft: extern_c requires exactly two arguments");
      return;
    }

    // Check arg 1.
    auto arg = call->_arguments.begin();
    auto *options = llvh::dyn_cast<ESTree::ObjectExpressionNode>(&*arg);
    if (!options) {
      outer_.sm_.error(
          arg->getSourceRange(),
          "ft: extern_c requires an object literal as the first argument");
      return;
    }
    // Parse the options.
    bool declaredOption = false;
    UniqueString *includeOption = nullptr;
    bool allowHVOption = false;
    if (!parseExternCOptions(
            options, &declaredOption, &includeOption, &allowHVOption))
      return;

    // Check arg 2.
    UniqueString *name;
    Type *funcType;
    TypedFunctionType *funcInfo;

    ++arg;
    auto *func = llvh::dyn_cast<ESTree::FunctionExpressionNode>(&*arg);
    if (!func) {
      outer_.sm_.error(
          arg->getSourceRange(),
          "ft: extern_c requires a function as the second argument");
      return;
    }
    if (!func->_id) {
      outer_.sm_.error(
          arg->getSourceRange(),
          "ft: extern_c requires a named function as the second argument");
      return;
    }
    name = llvh::cast<ESTree::IdentifierNode>(func->_id)->_name;
    funcType = outer_.flowContext_.findNodeType(func);
    // Unset for generic function expressions, which were already reported.
    if (!funcType)
      return;
    if (!llvh::isa<TypedFunctionType>(funcType->info)) {
      outer_.sm_.error(
          arg->getSourceRange(),
          "ft: extern_c requires a typed function as the second argument");
      return;
    }
    funcInfo = llvh::cast<TypedFunctionType>(funcType->info);
    if (funcInfo->isAsync() || funcInfo->isGenerator()) {
      outer_.sm_.error(
          arg->getSourceRange(),
          "ft: extern_c does not support async or generator functions");
      return;
    }
    if (funcInfo->getThisParam()) {
      outer_.sm_.error(
          arg->getSourceRange(),
          "ft: extern_c does not support 'this' parameters");
      return;
    }

    // Extract the function signature.
    NativeSignature *signature;

    if (!func->_returnType) {
      outer_.sm_.error(
          func->getSourceRange(),
          "ft: extern_c requires a return type annotation");
      return;
    }
    llvh::SmallVector<NativeCType, 4> natParamTypes{};
    NativeCType natReturnType;

    // The return type, where we allow void.
    if (llvh::isa<VoidType>(funcInfo->getReturnType()->info))
      natReturnType = NativeCType::c_void;
    else if (!parseNativeAnnotation(
                 llvh::cast<ESTree::TypeAnnotationNode>(func->_returnType),
                 allowHVOption,
                 &natReturnType))
      return;

    for (auto &node : func->_params) {
      auto *param = llvh::dyn_cast<ESTree::IdentifierNode>(&node);
      if (!param || param->_optional) {
        outer_.sm_.error(
            node.getSourceRange(),
            "ft: extern_c requires simple identifier parameters");
        return;
      }
      if (!param->_typeAnnotation) {
        outer_.sm_.error(
            param->getSourceRange(),
            "ft: extern_c requires type annotations for all parameters");
        return;
      }
      NativeCType natParamType;
      if (!parseNativeAnnotation(
              llvh::cast<ESTree::TypeAnnotationNode>(param->_typeAnnotation),
              false,
              &natParamType))
        return;
      natParamTypes.push_back(natParamType);
    }

    signature = outer_.astContext_.getNativeContext().getSignature(
        natReturnType, natParamTypes);

    // Now that we have the signature, declare the extern and check for invalid
    // redeclaration.
    NativeExtern *ne = outer_.astContext_.getNativeContext().getExtern(
        name, signature, call->getStartLoc(), declaredOption, includeOption);
    if (ne->signature() != signature) {
      outer_.sm_.error(
          call->getSourceRange(),
          "ft: invalid redeclaration of native extern '" + name->str() + "'");
      if (ne->loc().isValid()) {
        outer_.sm_.note(ne->loc(), "ft: original declaration here");
      }
      return;
    }

    TypeInfo *nativeFuncInfo = outer_.flowContext_.createNativeFunction(
        funcInfo->getReturnType(), funcInfo->getParams(), signature);

    outer_.setNodeType(call, outer_.flowContext_.createType(nativeFuncInfo));
  }

  void checkSHBuiltinModuleFactory(ESTree::CallExpressionNode *call) {
    visitESTreeChildren(*this, call, nullptr);
    assert(
        call->_arguments.size() == 2 &&
        "Ensured by checks in SemanticResolver.");
    auto argsIter = call->_arguments.begin();
    // Skip to the second argument.
    argsIter++;

    auto *modFactoryFunctionArg = &(*argsIter);
    // The type of the call is the type of the second (function) argument.
    outer_.setNodeType(call, outer_.getNodeTypeOrAny(modFactoryFunctionArg));
  }

  void checkSHBuiltinExport(ESTree::CallExpressionNode *call) {
    visitESTreeChildren(*this, call, nullptr);
    // The type of the call is void.
    outer_.setNodeType(call, outer_.flowContext_.getVoid());
  }

  void checkSHBuiltinImport(ESTree::CallExpressionNode *call) {
    visitESTreeChildren(*this, call, nullptr);
    // SemanticResolver ensures two or three arguments.
    if (call->_arguments.size() != 3) {
      // Without the third argument the imported value is untyped.
      outer_.setNodeType(call, outer_.flowContext_.getAny());
      return;
    }
    auto argsIter = call->_arguments.begin();
    argsIter++;
    argsIter++;

    auto *importExp = &(*argsIter);
    // The type of the call is the type of the third argument.
    outer_.setNodeType(call, outer_.getNodeTypeOrAny(importExp));
  }

  /// Extract the options from the options object literal. On error print an
  /// an error message and return false.
  bool parseExternCOptions(
      ESTree::ObjectExpressionNode *options,
      bool *declaredOption,
      UniqueString **includeOption,
      bool *allowHVOption) {
    *declaredOption = false;
    *includeOption = nullptr;

    auto parseObjRes = parseExternCObjectLiteral(options);
    if (!parseObjRes)
      return false;
    auto &map = *parseObjRes;
    bool success = true;

    // NOTE: Whenever we find a supported option, we erase it.

    // Parse an option of a specified literal type. On error print an error
    // message and clear the success flag.
    // \param lit The literal type to parse. The passed value is ignored, only
    //    the type matters.
    // \param typeName The name of the type to print in the error message.
    // \param optionName The name of the option to parse.
    // \param res Output parameter for the parsed value.
    auto parseOption = [&map, &success, this](
                           auto *lit,
                           llvh::StringLiteral typeName,
                           llvh::StringLiteral optionName,
                           auto *res) {
      using LitType = std::remove_pointer_t<decltype(lit)>;
      auto it = map.find(
          outer_.astContext_.getIdentifier(optionName).getUnderlyingPointer());
      if (it != map.end()) {
        lit = llvh::dyn_cast<LitType>(it->second->_value);
        if (lit) {
          *res = lit->_value;
        } else {
          outer_.sm_.error(
              it->second->getSourceRange(),
              "ft: extern_c option '" + optionName + "' must be a " + typeName +
                  " literal");
          success = false;
        }
        map.erase(it);
      }
    };

    auto parseString = [&parseOption](
                           llvh::StringLiteral optionName, UniqueString **res) {
      parseOption(
          (ESTree::StringLiteralNode *)nullptr, "string", optionName, res);
    };
    auto parseBool = [&parseOption](llvh::StringLiteral optionName, bool *res) {
      parseOption(
          (ESTree::BooleanLiteralNode *)nullptr, "boolean", optionName, res);
    };

    parseBool("declared", declaredOption);
    parseBool("hv", allowHVOption);
    parseString("include", includeOption);

    // Check for unsupported properties.
    for (auto &prop : map) {
      outer_.sm_.error(
          prop.second->getSourceRange(),
          "ft: extern_c does not support option '" + prop.first->str() + "'");
      success = false;
    }

    return success;
  }

  /// Helper to parse the options object literal used in extern_c. Ensures that
  /// only "normal" properties are present, and that the values are literals or
  /// object literals. On error prints an error message and returns None.
  /// On success returns a map of property names to PropertyNodes. The caller
  /// can quickly scan the names. It can also use the same function to scan
  /// nested object literals.
  ///
  /// \return None on error, otherwise a map of property names to
  ///     EStree::PropertyNode.
  llvh::Optional<
      llvh::SmallMapVector<UniqueString *, ESTree::PropertyNode *, 4>>
  parseExternCObjectLiteral(ESTree::ObjectExpressionNode *objLitNode) {
    llvh::SmallMapVector<UniqueString *, ESTree::PropertyNode *, 4> res{};

    for (ESTree::Node &n : objLitNode->_properties) {
      // The dyn_cast could perhaps be a cast, but just to be safe.
      auto *prop = llvh::dyn_cast<ESTree::PropertyNode>(&n);
      if (!prop || prop->_kind != outer_.kw_.identInit || prop->_computed ||
          prop->_method || prop->_shorthand ||
          !llvh::isa<ESTree::IdentifierNode>(prop->_key)) {
        outer_.sm_.error(
            n.getSourceRange(), "ft: extern_c: unsupported property format");
        return llvh::None;
      }

      // Check that the value is a literal, another object, or an array.
      auto *value = prop->_value;
      if (!(llvh::isa<ESTree::NullLiteralNode>(value) ||
            llvh::isa<ESTree::BooleanLiteralNode>(value) ||
            llvh::isa<ESTree::StringLiteralNode>(value) ||
            llvh::isa<ESTree::NumericLiteralNode>(value) ||
            llvh::isa<ESTree::BigIntLiteralNode>(value) ||
            llvh::isa<ESTree::ObjectExpressionNode>(value))) {
        outer_.sm_.error(
            value->getSourceRange(), "ft: extern_c: unsupported property type");
        return llvh::None;
      }

      // Note that we don't care about duplicates, we just want to use the last
      // one.
      res[llvh::cast<ESTree::IdentifierNode>(prop->_key)->_name] = prop;
    }

    // Note that we have to use std::move() since we are returning an optional.
    return std::move(res);
  }

  /// Parse a native type annotation. Return true on success and store the
  /// value in \p res. On error print an error message and return false.
  ///
  /// \param node The node to parse.
  /// \param allowRegular If true, allow type annotations that are not native
  ///     types.
  /// \param res Output parameter for the parsed type.
  bool parseNativeAnnotation(
      ESTree::TypeAnnotationNode *node,
      bool allowRegular,
      NativeCType *res) {
    *res = NativeCType::c_hermes_value;
    auto *ann = llvh::dyn_cast<ESTree::GenericTypeAnnotationNode>(
        node->_typeAnnotation);
    if (!ann || ann->_typeParameters) {
      if (allowRegular)
        return true;
      outer_.sm_.error(
          node->getSourceRange(), "ft: unsupported native type annotation");
      return false;
    }
    auto *id = llvh::dyn_cast<ESTree::IdentifierNode>(ann->_id);
    if (!id) {
      if (allowRegular)
        return true;
      outer_.sm_.error(
          node->getSourceRange(), "ft: unsupported native type annotation");
      return false;
    }
    UniqueString *name = id->_name;
    auto it = outer_.nativeTypes_.find(name);
    if (it == outer_.nativeTypes_.end()) {
      if (allowRegular)
        return true;
      outer_.sm_.error(
          ann->_id->getSourceRange(),
          "ft: '" + name->str() + "' is not a native type");
      return false;
    }
    *res = it->second;
    return true;
  };

  void visit(
      ESTree::OptionalCallExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    outer_.sm_.error(
        node->getSourceRange(), "ft: optional call expression not supported");
  }

  void visit(
      ESTree::NewExpressionNode *node,
      ESTree::Node *parent,
      Type *constraint) {
    // In a static member 'this' may be a subclass, which ES would construct,
    // but the typed constructor call is resolved against the declaring class.
    if (Type *thisType = outer_.curFunctionContext_->thisParamType; thisType &&
        llvh::isa<ESTree::ThisExpressionNode>(node->_callee) &&
        llvh::isa<ClassConstructorType>(thisType->info)) {
      outer_.sm_.error(
          node->_callee->getSourceRange(),
          "ft: 'new this()' is not supported in static members, "
          "use the class name");
      return;
    }

    auto *identCallee = llvh::dyn_cast<ESTree::IdentifierNode>(node->_callee);
    sema::Decl *genericDecl = nullptr;
    if (identCallee && outer_.getDecl(identCallee)->generic)
      genericDecl = outer_.getDecl(identCallee);

    // Partial type arguments: infer the missing trailing ones from the
    // constructor arguments, which visits them.
    if (genericDecl && node->_typeArguments &&
        llvh::cast<ESTree::TypeParameterInstantiationNode>(node->_typeArguments)
                ->_params.size() <
            outer_.getGenericInfoMustExist(genericDecl).numTypeParams) {
      auto [visited, typeArgs] =
          outer_.inferTypeArgumentsForGenericClassNew(node, genericDecl);
      if (typeArgs.empty()) {
        if (!visited)
          outer_.sm_.error(
              node->_typeArguments->getSourceRange(),
              "ft: could not infer type arguments for generic class");
        return;
      }
      outer_.resolveGenericClassSpecializationWithParsedTypes(
          identCallee, node->getSourceRange(), typeArgs, genericDecl);
    } else {
      visitESTreeChildren(*this, node, nullptr);
      // Resolve generics using type arguments if necessary.
      if (genericDecl) {
        if (!node->_typeArguments) {
          outer_.sm_.error(
              node->_callee->getSourceRange(), "ft: type arguments required");
          return;
        }
        outer_.resolveGenericClassSpecialization(
            identCallee,
            llvh::cast<ESTree::TypeParameterInstantiationNode>(
                node->_typeArguments),
            genericDecl);
      }
    }
    if (!identCallee && node->_typeArguments) {
      // Generics handled above.
      outer_.sm_.error(
          node->_callee->getSourceRange(),
          "ft: generic call only works on identifiers");
      return;
    }

    Type *calleeType = outer_.getNodeTypeOrAny(node->_callee);
    // If the callee has no type, we have nothing to do/check.
    if (llvh::isa<AnyType>(calleeType->info)) {
      checkMixedSpreads(node->_arguments);
      return;
    }

    if (!llvh::isa<ClassConstructorType>(calleeType->info)) {
      outer_.sm_.error(
          node->_callee->getSourceRange(),
          "ft: callee is not a class constructor");
      return;
    }
    auto *classConsType = llvh::cast<ClassConstructorType>(calleeType->info);
    Type *classType = classConsType->getClassType();
    ClassType *classTypeInfo = classConsType->getClassTypeInfo();

    outer_.setNodeType(node, classType);

    // Find the effective constructor type by walking up the class chain.
    // An implicit constructor forwards to the nearest ancestor's constructor.
    Type *consFType = nullptr;
    for (ClassType *cur = classTypeInfo; cur; cur = cur->getSuperClassInfo()) {
      if ((consFType = cur->getConstructorType()))
        break;
    }
    if (consFType) {
      checkCallArgumentTypes(
          llvh::cast<TypedFunctionType>(consFType->info)->getParams(),
          node,
          node->_arguments,
          "class " + classTypeInfo->getClassNameOrDefault() + " constructor");
    } else if (!node->_arguments.empty()) {
      outer_.sm_.error(
          node->getSourceRange(),
          "ft: class " + classTypeInfo->getClassNameOrDefault() +
              " does not have an explicit constructor");
      return;
    }
  }

  void visit(ESTree::SuperNode *node, ESTree::Node *parent, Type *constraint) {
    if (!outer_.curClassContext_) {
      outer_.sm_.error(
          node->getSourceRange(), "ft: super only supported in class");
      return;
    }

    // Check that the super call is valid.
    ClassType *curClassType = outer_.curClassContext_->getClassTypeInfo();
    ClassType *superClassType = curClassType->getSuperClassInfo();
    if (!superClassType) {
      outer_.sm_.error(
          node->getSourceRange(), "ft: super requires a base class");
      return;
    }

    if (llvh::isa<ESTree::CallExpressionNode>(parent)) {
      // super() call calls the constructor of the super class.
      outer_.setNodeType(node, superClassType->getConstructorType());
    } else if (llvh::isa<ESTree::MemberExpressionNode>(parent)) {
      // super.property looks up on the super class: its prototype normally,
      // its statics when the receiver is the class constructor. The enclosing
      // function context records which of the two applies.
      // A context that doesn't thread superType through can't resolve the
      // lookup, so reject instead of producing a null type.
      Type *superType = outer_.curFunctionContext_->superType;
      if (!superType) {
        outer_.sm_.error(node->getSourceRange(), "ft: invalid usage of super");
        outer_.flowContext_.setNodeType(node, outer_.flowContext_.getAny());
        return;
      }
      outer_.setNodeType(node, superType);
    } else {
      outer_.sm_.error(node->getSourceRange(), "ft: invalid usage of super");
      outer_.flowContext_.setNodeType(node, outer_.flowContext_.getAny());
      return;
    }
  }

  /// \param reportErrors false to check silently, for overload resolution.
  /// \return the number of call arguments \p arg contributes. A spread of a
  ///   tuple contributes one argument per tuple element, since it is expanded
  ///   statically; anything else contributes one. Reports an error and returns
  ///   None for a spread that cannot be expanded statically.
  OptValue<size_t> spreadWidth(ESTree::Node *arg, bool reportErrors) {
    auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(arg);
    if (!spread)
      return 1;

    Type *spreadTy = outer_.getNodeTypeOrAny(spread->_argument);
    if (auto *tupleTy = llvh::dyn_cast<TupleType>(spreadTy->info))
      return tupleTy->getTypes().size();

    if (reportErrors) {
      // An array's length is not known statically, so the call's arity would
      // be unknown and the arguments could not be read out individually.
      if (outer_.flowContext_.isArrayClassType(spreadTy)) {
        outer_.sm_.error(
            spread->getSourceRange(),
            "ft: cannot spread an array into a call, because its length is "
            "not known at compile time; use a tuple");
      } else {
        outer_.sm_.error(
            spread->getSourceRange(), "ft: spread argument must be a tuple");
      }
    }
    return llvh::None;
  }

  /// Report an error if \p arguments spread a tuple alongside a spread that
  /// cannot be expanded statically. IRGen then builds the whole argument list
  /// dynamically, through the iterator protocol, and a tuple is not iterable.
  void checkMixedSpreads(ESTree::NodeList &arguments) {
    ESTree::Node *tupleSpread = nullptr;
    bool hasOtherSpread = false;
    for (ESTree::Node &arg : arguments) {
      auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(&arg);
      if (!spread)
        continue;
      if (llvh::isa<TupleType>(
              outer_.getNodeTypeOrAny(spread->_argument)->info))
        tupleSpread = tupleSpread ? tupleSpread : spread;
      else
        hasOtherSpread = true;
    }
    if (tupleSpread && hasOtherSpread) {
      outer_.sm_.error(
          tupleSpread->getSourceRange(),
          "ft: cannot spread a tuple alongside a spread that is not a tuple");
    }
  }

  /// Visit the call arguments in \p arguments, skipping the first \p offset,
  /// constraining each to the type of the parameter it will be passed to.
  /// A spread element has no single parameter, so it is visited without a
  /// constraint and advances the parameter index by the number of arguments
  /// it contributes.
  /// Errors are not reported here; checkCallArgumentTypes() reports them when
  /// it typechecks the arguments.
  void visitCallArgumentsWithConstraints(
      ESTree::NodeList &arguments,
      ESTree::Node *callNode,
      llvh::ArrayRef<TypedFunctionType::Param> params,
      uint32_t offset = 0) {
    auto it = arguments.begin();
    std::advance(it, offset);
    size_t i = 0;
    for (auto e = arguments.end(); it != e; ++it) {
      bool isSpread = llvh::isa<ESTree::SpreadElementNode>(&*it);
      Type *argConstraint =
          !isSpread && i < params.size() ? params[i].type : nullptr;
      visitESTreeNodeNoReplace(*this, &*it, callNode, argConstraint);
      // A spread's width is only known once it has been visited.
      OptValue<size_t> width = spreadWidth(&*it, /* reportErrors */ false);
      i += width ? *width : 1;
    }
  }

  /// \return the number of arguments in \p arguments, skipping the first
  ///   \p offset, with tuple spread elements expanded to their element count.
  ///   None if a spread cannot be expanded statically, in which case an error
  ///   was reported when \p reportErrors.
  /// The paths that return without typechecking their arguments call this and
  /// discard the result, purely to report those errors: that is what lets
  /// IRGen assume every surviving spread is a spread of a tuple.
  OptValue<size_t> flattenedArgCount(
      ESTree::NodeList &arguments,
      uint32_t offset,
      bool reportErrors) {
    size_t numArgs = 0;
    for (ESTree::Node &arg : arguments) {
      if (offset) {
        --offset;
        continue;
      }
      OptValue<size_t> width = spreadWidth(&arg, reportErrors);
      if (!width)
        return llvh::None;
      numArgs += *width;
    }
    return numArgs;
  }

  /// Report an error if any of the first \p argCountToNotAllowSpreads arguments
  /// of \p call is a spread element. Those slots are consumed positionally as
  /// the callee or 'this', so they cannot be expanded. On error, recover by
  /// visiting the arguments unconstrained and typing the call as 'any'.
  ///
  /// \return false if an error was reported.
  bool checkSpreadInLeadingArgs(
      ESTree::CallExpressionNode *call,
      uint32_t argCountToNotAllowSpreads) {
    auto it = call->_arguments.begin();
    for (uint32_t i = 0;
         i < argCountToNotAllowSpreads && it != call->_arguments.end();
         ++i, ++it) {
      if (!llvh::isa<ESTree::SpreadElementNode>(&*it))
        continue;
      outer_.sm_.error(
          it->getSourceRange(),
          "ft: spread element not allowed in this argument position");
      visitESTreeNodeList(*this, call->_arguments, call, nullptr);
      outer_.setNodeType(call, outer_.flowContext_.getAny());
      return false;
    }
    return true;
  }

  /// Check the types of the supplied arguments, adding checked casts if
  /// needed. If \p reportErrors is false, silently check without emitting
  /// errors or inserting casts.
  /// \param offset the number of arguments to ignore at the front of \p
  ///   arguments. Used for $SHBuiltin.call, which has extra args at the front.
  bool checkCallArgumentTypes(
      llvh::ArrayRef<TypedFunctionType::Param> params,
      ESTree::Node *callNode,
      ESTree::NodeList &arguments,
      const llvh::Twine &calleeName,
      uint32_t offset = 0,
      bool reportErrors = true) {
    auto begin = arguments.begin();
    std::advance(begin, offset);

    OptValue<size_t> flatCount =
        flattenedArgCount(arguments, offset, reportErrors);
    if (!flatCount)
      return false;
    size_t numArgs = *flatCount;

    bool hasRest = !params.empty() && params.back().rest;
    size_t numNonRestParams = hasRest ? params.size() - 1 : params.size();

    // \return whether the param may be omitted at the call site if it is
    // explicitly optional or if `void` flows into its type.
    auto paramOmittable = [this](const TypedFunctionType::Param &p) -> bool {
      return p.optional ||
          outer_.canAFlowIntoB(outer_.flowContext_.getVoid(), p.type).canFlow;
    };

    // Extract element type for rest param if present.
    Type *restElementType = nullptr;
    if (hasRest) {
      Type *restParamType = params.back().type;
      if (outer_.flowContext_.isArrayClassType(restParamType)) {
        restElementType =
            outer_.flowContext_.getArrayElementType(restParamType);
      }
    }

    if (hasRest) {
      // With rest param, need at least the required non-rest params.
      size_t numRequired = numNonRestParams;
      while (numRequired > 0 && paramOmittable(params[numRequired - 1]))
        --numRequired;
      if (numArgs < numRequired) {
        if (reportErrors) {
          outer_.sm_.error(
              callNode->getSourceRange(),
              "ft: " + calleeName + " expects at least " +
                  llvh::Twine(numRequired) + " arguments, but " +
                  llvh::Twine(numArgs) + " supplied");
        }
        return false;
      }
    } else if (
        numArgs > params.size() ||
        !llvh::all_of(params.drop_front(numArgs), paramOmittable)) {
      // Reject extra args, or fewer args when some trailing missing param
      // is not omittable.
      if (reportErrors) {
        // Count the number of required parameters.
        size_t numRequired = params.size();
        while (numRequired > 0 && paramOmittable(params[numRequired - 1]))
          --numRequired;
        outer_.sm_.error(
            callNode->getSourceRange(),
            "ft: " + calleeName + " expects " +
                (numArgs > params.size()
                     ? "at most " + llvh::Twine(params.size())
                     : (numRequired != params.size()
                            ? "at least " + llvh::Twine(numRequired)
                            : llvh::Twine(params.size()))) +
                " arguments, but " + llvh::Twine(numArgs) + " supplied");
      }
      return false;
    }

    // argIndex counts flattened arguments, so it advances by the element
    // count across a tuple spread.
    size_t argIndex = 0;

    /// Check one flattened argument of type \p argType against the parameter
    /// it is passed to, and consume its index. Errors are reported at \p loc.
    /// \param castIt the argument to replace if a checked cast is needed, or
    ///   nullptr if there is no argument node to hold the cast, which is the
    ///   case for a tuple spread element.
    /// \return false if an error was reported.
    auto checkOne = [this,
                     &arguments,
                     &params,
                     &argIndex,
                     &calleeName,
                     numNonRestParams,
                     restElementType,
                     reportErrors](
                        Type *argType,
                        llvh::SMRange loc,
                        ESTree::NodeList::iterator *castIt) -> bool {
      // A rest param without an Array<T> type is not checked.
      Type *expectedType =
          argIndex < numNonRestParams ? params[argIndex].type : restElementType;
      size_t paramIndex = argIndex++;
      if (!expectedType)
        return true;

      auto [argTypeNarrow, cf] = outer_.tryNarrowType(argType, expectedType);
      if (!cf.canFlow) {
        if (reportErrors) {
          std::string argTypeStr = argType->messageString();
          std::string expectedTypeStr = expectedType->messageString();
          if (paramIndex < numNonRestParams &&
              params[paramIndex].name.isValid()) {
            outer_.sm_.error(
                loc,
                "ft: " + calleeName + " parameter '" +
                    params[paramIndex].name.str() +
                    "' type mismatch: cannot assign " + argTypeStr + " to " +
                    expectedTypeStr);
          } else {
            outer_.sm_.error(
                loc,
                "ft: " + calleeName + " parameter #" +
                    llvh::Twine(paramIndex + 1) +
                    " type mismatch: cannot assign " + argTypeStr + " to " +
                    expectedTypeStr);
          }
        }
        return false;
      }

      if (!cf.needCheckedCast)
        return true;
      if (!castIt) {
        if (reportErrors) {
          outer_.sm_.error(
              loc, "ft: spread element with checked cast not supported");
        }
        return false;
      }
      // Insert the cast before the current node and erase the current one.
      if (reportErrors && outer_.compile_) {
        auto newIt = arguments.insert(
            *castIt, *outer_.implicitCheckedCast(&**castIt, argTypeNarrow, cf));
        arguments.erase(*castIt);
        *castIt = newIt;
      }
      return true;
    };

    for (auto it = begin, e = arguments.end(); it != e; ++it) {
      // A spread of a tuple is expanded statically into one argument per
      // element. IRGen loads the elements directly, so there is no argument
      // node on which to hang a checked cast.
      if (auto *spread = llvh::dyn_cast<ESTree::SpreadElementNode>(&*it)) {
        auto *tupleTy = llvh::cast<TupleType>(
            outer_.getNodeTypeOrAny(spread->_argument)->info);
        for (Type *elemType : tupleTy->getTypes())
          if (!checkOne(elemType, spread->getSourceRange(), nullptr))
            return false;
        continue;
      }

      if (!checkOne(outer_.getNodeTypeOrAny(&*it), it->getSourceRange(), &it))
        return false;
    }

    return true;
  }

  /// Resolve a call to an overloaded method by selecting the matching
  /// overload and setting the callee type. Does not set the return type
  /// on the call node — the caller should fall through to the normal
  /// call checking path for argument type checking and return type.
  /// On success, the call's arguments have already been visited.
  /// \param explicitTypeArgs optional explicit type arguments node from the
  ///   call expression. If non-null, only generic overloads with matching
  ///   type-parameter count are considered, and the explicit types are used
  ///   directly instead of inference; non-generic overloads are skipped.
  /// \return false on error (already reported), true on success.
  LLVM_NODISCARD bool resolveOverloadedMethodCall(
      ESTree::CallExpressionNode *node,
      ESTree::MemberExpressionNode *callee,
      const ClassType::Field *field,
      ESTree::TypeParameterInstantiationNode *explicitTypeArgs = nullptr) {
    assert(field->isOverloaded() && "field must be overloaded");

    // Pre-parse explicit type arguments if provided.
    llvh::SmallVector<Type *, 2> explicitTypeArgTypes;
    if (explicitTypeArgs) {
      for (ESTree::Node &arg : explicitTypeArgs->_params) {
        explicitTypeArgTypes.push_back(outer_.parseTypeAnnotation(&arg));
      }
    }

    // Visit arguments without constraints to determine their types.
    for (ESTree::Node &arg : node->_arguments) {
      outer_.visitExpression(&arg, node, nullptr);
    }

    // Check each overload for type compatibility with the
    // arguments, selecting the unique match.
    // If there's two matches, it's an ambiguous callsite.
    // If there's no matches it doesn't typecheck.
    Type *matchedType = nullptr;
    sema::Decl *matchedDecl = nullptr;

    for (const auto &[overloadMethod, originalOverloadType] :
         field->overloads) {
      Type *overloadType = originalOverloadType;
      sema::Decl *overloadDecl = nullptr;

      if (llvh::isa<GenericType>(originalOverloadType->info)) {
        llvh::SmallVector<Type *, 2> typeArgs;
        if (explicitTypeArgs) {
          // Only consider generic overloads whose type-parameter count
          // matches the number of explicit type arguments.
          auto *fe = llvh::cast<ESTree::FunctionExpressionNode>(
              overloadMethod->_value);
          auto *typeParams = llvh::cast<ESTree::TypeParameterDeclarationNode>(
              fe->_typeParameters);
          if (typeParams->_params.size() != explicitTypeArgTypes.size())
            continue;
          typeArgs.assign(
              explicitTypeArgTypes.begin(), explicitTypeArgTypes.end());
        } else {
          // Infer type arguments and specialize.
          auto [didVisitArgs, inferred] =
              outer_.inferTypeArgumentsForGenericMethodCall(
                  node, callee, overloadMethod);
          if (inferred.empty())
            continue;
          typeArgs = std::move(inferred);
        }
        auto result = outer_.specializeGenericMethodWithParsedTypes(
            overloadMethod, node->getSourceRange(), typeArgs);
        if (!result.type)
          continue;
        overloadType = result.type;
        overloadDecl = result.decl;
      } else {
        // Non-generic overload: explicit type arguments don't apply.
        if (explicitTypeArgs)
          continue;
        overloadDecl = outer_.semContext_.getExpressionDecl(
            ESTree::getPropertyIdentifier(overloadMethod->_key));
      }

      auto *fnType = llvh::dyn_cast<TypedFunctionType>(overloadType->info);
      if (!fnType)
        continue;

      // Check arity and argument types (silent — no error reporting).
      if (!checkCallArgumentTypes(
              fnType->getParams(),
              node,
              node->_arguments,
              /* calleeName */ "",
              /* offset */ 0,
              /* reportErrors */ false))
        continue;

      if (matchedType) {
        outer_.sm_.error(
            node->getSourceRange(),
            "ft: ambiguous call: multiple overloads match");
        return false;
      }
      matchedType = overloadType;
      matchedDecl = overloadDecl;
    }

    if (!matchedType) {
      outer_.sm_.error(
          node->getSourceRange(), "ft: no matching overload for call");
      return false;
    }

    assert(matchedDecl && "overloaded methods have a Decl (they are final)");
    outer_.semContext_.setExpressionDecl(
        ESTree::getPropertyIdentifier(callee->_property), matchedDecl);

    outer_.setNodeType(callee, matchedType);
    return true;
  }
};

void FlowChecker::visitExpression(
    ESTree::Node *node,
    ESTree::Node *parent,
    Type *constraint) {
  ExprVisitor v(*this);
  visitESTreeNode(v, node, parent, constraint);
}

void FlowChecker::visitAssignmentTarget(
    ESTree::Node *node,
    ESTree::Node *parent,
    Type *constraint) {
  ExprVisitor v(*this);
  v.visitAssignmentTarget(node, parent, constraint);
}

} // namespace flow
} // namespace hermes

#endif // HERMES_PARSE_FLOW
