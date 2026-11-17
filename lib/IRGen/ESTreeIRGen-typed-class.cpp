/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ESTreeIRGen.h"
#include "llvh/ADT/Hashing.h"
#include "llvh/ADT/ScopeExit.h"

namespace hermes {
namespace irgen {

namespace {

/// Call the appropriate callback for every member of \p type that is backed by
/// a Variable rather than stored on the object. Generic method declarations
/// are skipped because their concrete specializations are handled separately.
/// \p onField defaults to a no-op, for callers that only care about methods.
template <
    typename OnMethod,
    typename OnField = void (*)(const flow::ClassType::Field &)>
void forEachVariableBackedClassMember(
    flow::ClassType *type,
    OnMethod &&onMethod,
    OnField &&onField = [](const flow::ClassType::Field &) {}) {
  if (!type)
    return;

  /// Call onMethod for any non-generic method.
  /// Specialized generics are handled separately via getSpecializedMethodDecls.
  auto visitMethod = [&onMethod](ESTree::MethodDefinitionNode *method) {
    if (!method)
      return;
    if (llvh::cast<ESTree::FunctionExpressionNode>(method->_value)
            ->_typeParameters)
      return;
    onMethod(method);
  };

  for (const flow::ClassType::Field &field : type->getFields()) {
    if (field.isStoredOnObject())
      continue;

    if (!field.isMethod()) {
      onField(field);
      continue;
    }

    if (field.isOverloaded()) {
      for (const auto &overload : field.overloads)
        visitMethod(overload.first);
      continue;
    }

    visitMethod(field.method);
    // getter is stored in field.method, so that's already covered.
    visitMethod(field.setterMethod);
  }
}

} // namespace

void ESTreeIRGen::genClassDeclaration(ESTree::ClassDeclarationNode *node) {
  auto *id = llvh::cast<ESTree::IdentifierNode>(node->_id);
  sema::Decl *decl = getIDDecl(id);
  if (decl->generic) {
    // Skip generics that aren't specialized.
    return;
  }

  flow::Type *declType = flowContext_.findDeclType(decl);
  flow::ClassConstructorType *consType =
      llvh::dyn_cast_or_null<flow::ClassConstructorType>(
          declType ? declType->info : nullptr);

  // If the class is not annotated with a type, it is legacy.
  if (!consType) {
    genLegacyClassDeclaration(node);
    return;
  }

  flow::ClassType *classType = consType->getClassTypeInfo();

  Value *superClass = nullptr;
  if (node->_superClass) {
    superClass = genExpression(node->_superClass);
  }

  // Push a new typed class context.
  TypedClassContext savedClsCtx = curFunction()->typedClassContext;
  curFunction()->typedClassContext = {node, classType};
  // Pop the class context on function exit.
  auto popClsContext = llvh::make_scope_exit([&savedClsCtx, this]() {
    curFunction()->typedClassContext = savedClsCtx;
  });

  // Store the Variable-backed closure for a method.
  auto emitMethodClosure = [this](ESTree::MethodDefinitionNode *method) {
    sema::Decl *decl =
        semCtx_.getExpressionDecl(ESTree::getPropertyIdentifier(method->_key));
    assert(decl && "Variable-backed method must have a declaration");
    emitTypedClassMethodClosureStore(method, decl);
  };
  forEachVariableBackedClassMember(
      classType->getHomeObjectTypeInfo(), emitMethodClosure);
  forEachVariableBackedClassMember(
      classType->getStaticObjectTypeInfo(), emitMethodClosure);

  // Create the implicit field initializer function; store the closure
  // for it in a variable, and save that variable in a table indexed by
  // the ClassDeclarationNode.
  emitCreateTypedFieldInitFunction();

  // Emit the explicit constructor, if present.
  Value *consFunction;
  Identifier consName = classType->getClassName().isValid()
      ? classType->getClassName()
      : Identifier();
  if (ESTree::MethodDefinitionNode *consMethod = semCtx_.getConstructor(node)) {
    // Check that 'super()' call is the first statement in SH for derived
    // classes.
    // TODO: This is intentionally overly restrictive. In the future, we can
    // check that super() call happens prior to any function calls or 'this'
    // accesses.
    if (node->_superClass) {
      // Attempt to extract the super() call from the first statement of the
      // block.
      ESTree::NodeList &blockStmtBody =
          llvh::cast<ESTree::BlockStatementNode>(
              llvh::cast<ESTree::FunctionExpressionNode>(consMethod->_value)
                  ->_body)
              ->_body;
      ESTree::Node *firstStatement = nullptr;
      for (ESTree::Node &it : blockStmtBody) {
        firstStatement = &it;
        auto *exprSt = llvh::dyn_cast<ESTree::ExpressionStatementNode>(&it);
        // Skip directives.
        if (!exprSt || !exprSt->_directive) {
          break;
        }
      }
      ESTree::ExpressionStatementNode *exprStatement =
          llvh::dyn_cast_or_null<ESTree::ExpressionStatementNode>(
              firstStatement);
      ESTree::CallExpressionNode *superCall = exprStatement
          ? llvh::dyn_cast<ESTree::CallExpressionNode>(
                exprStatement->_expression)
          : nullptr;
      if (!superCall || !llvh::isa<ESTree::SuperNode>(superCall->_callee)) {
        Mod->getContext().getSourceErrorManager().error(
            node->getSourceRange(),
            "first statement in derived class constructor must be super();");
      }
    }

    consFunction = genFunctionExpression(
        llvh::cast<ESTree::FunctionExpressionNode>(consMethod->_value),
        consName,
        node->_superClass,
        node->_superClass ? Function::DefinitionKind::ES6DerivedConstructor
                          : Function::DefinitionKind::ES6BaseConstructor);
  } else {
    // The constructor is implicit.
    consFunction = genTypedImplicitConstructor(consName, superClass);
  }
  emitStore(consFunction, getDeclData(decl), true);

  // Create and populate the "prototype" property (vtable).
  // Must be done even if there are no methods to enable 'instanceof'.
  Value *vtable;
  if (superClass) {
    Value *superHomeObject =
        emitLoadTypedHomeObject(classType->getSuperClassInfo());
    // TODO: This will be known to be the actual type when we properly use an
    // instruction for class creation, but for now we need an object here
    // because we want to use PrLoad on it.
    vtable = Builder.createUnionNarrowTrustedInst(
        superHomeObject, Type::createObject());
  } else {
    vtable = Builder.getLiteralNull();
  }

  auto *homeObject = emitTypedClassAllocation(
      classType->getHomeObjectTypeInfo(),
      vtable,
      /* propertiesEnumerable */ false);

  // Handle generic method specializations.
  // Compile each specialization and store its closure in the Variable that
  // was declared at the top of the enclosing scope.
  for (const auto &[specializedMethod, decl] :
       classType->getSpecializedMethodDecls()) {
    emitTypedClassMethodClosureStore(specializedMethod, decl);
  }

  // Store the home object in a variable so that we can reference it later,
  // e.g. when we emit method calls. Check if we already have a cached entry
  // (e.g., when recompiling a finally block) and reuse it.
  Variable *homeObjectVar;
  auto existingIt = classConstructors_.find(classType);
  if (existingIt != classConstructors_.end()) {
    // Reuse existing variable from previous compilation.
    homeObjectVar = existingIt->second.homeObjectVar;
  } else {
    // First compilation: create new variable.
    homeObjectVar = Builder.createVariable(
        curFunction()->curScope()->getVariableScope(),
        Builder.createIdentifier(
            llvh::Twine("?") + classType->getClassName().str() + ".prototype"),
        flowTypeToIRType(classType->getHomeObjectType()),
        /* hidden */ true);
  }
  Builder.createStoreFrameInst(
      curFunction()->curScope(), homeObject, homeObjectVar);

  // Check to make sure this is a valid class definition,
  // because there may have been errors.
  if (auto *createCallable =
          llvh::dyn_cast<BaseCreateCallableInst>(consFunction)) {
    auto [it, inserted] = classConstructors_.try_emplace(
        classType, createCallable->getFunctionCode(), homeObjectVar);
    (void)it;
    (void)inserted;
    // On recompilation, the entry already exists. Verify consistency.
    assert(
        it->second.constructorFunc == createCallable->getFunctionCode() &&
        "redefinition of constructor function");
    assert(
        it->second.homeObjectVar == homeObjectVar &&
        "redefinition with different homeObjectVar");
  }

  // The 'prototype' property is initially set as non-configurable,
  // and we're overwriting it with our own.
  // So we can't use StoreOwnProperty here because that attempts to define a
  // configurable property.
  // TODO: Do this properly by using a new instruction for class creation.
  Builder.createStorePropertyStrictInst(
      homeObject,
      consFunction,
      Builder.getLiteralString(kw_.identPrototype->str()));

  if (Function *staticElementsCode =
          genFieldInitFunction(/* staticElements */ true)) {
    CreateFunctionInst *staticElementsFunc = Builder.createCreateFunctionInst(
        curFunction()->curScope(), staticElementsCode);
    Builder
        .createCallInst(
            staticElementsFunc,
            staticElementsCode,
            /* calleeIsAlwaysClosure */ true,
            /* env */ curFunction()->curScope(),
            /* newTarget */ Builder.getLiteralUndefined(),
            consFunction,
            /* args */ {})
        ->setType(Type::createUndefined());
  }
}

Variable *ESTreeIRGen::defineClassMemberVar(
    sema::Decl *decl,
    Identifier name,
    Type type,
    Value *init) {
  if (!decl->customData) {
    auto *var = Builder.createVariable(
        curFunction()->curScope()->getVariableScope(),
        name,
        type,
        /* hidden */ true);
    setDeclData(decl, var);
  }
  return storeClassMemberVar(decl, init);
}

Variable *ESTreeIRGen::storeClassMemberVar(sema::Decl *decl, Value *value) {
  assert(decl->customData && "class member Variable must be declared first");
  auto *var = llvh::cast<Variable>(getDeclData(decl));
  Builder.createStoreFrameInst(
      emitResolveScopeInstIfNeeded(var->getParent()), value, var);
  return var;
}

void ESTreeIRGen::declareTypedClassMemberVars(sema::Decl *classDecl) {
  flow::Type *declType = flowContext_.findDeclType(classDecl);
  auto *consType = llvh::dyn_cast_or_null<flow::ClassConstructorType>(
      declType ? declType->info : nullptr);
  // A class without a ClassConstructorType is legacy, and has no members in
  // Variables.
  if (!consType)
    return;
  flow::ClassType *classType = consType->getClassTypeInfo();

  // Closures have no representable literal default, so like object-typed
  // statics they start out empty and are checked on load.
  Type closureStorageType =
      getTypeContext().unionTy(Type::createObject(), Type::createEmpty());

  /// Declare the Variable backing the closure of \p method. The closure itself
  /// is stored when the class declaration is emitted.
  auto declareMethodVar = [this, closureStorageType](
                              ESTree::MethodDefinitionNode *method) {
    ESTree::IdentifierNode *id = ESTree::getPropertyIdentifier(method->_key);
    sema::Decl *decl = semCtx_.getExpressionDecl(id);
    assert(decl && "Variable-backed method must have a declaration");
    Identifier name = llvh::isa<ESTree::PrivateNameNode>(method->_key)
        ? Mod->getContext().getPrivateNameIdentifier(id->_name)
        : Identifier::getFromPointer(id->_name);
    defineClassMemberVar(
        decl, name, closureStorageType, Builder.getLiteralEmpty());
  };
  auto declareFieldVar = [this](const flow::ClassType::Field &field) {
    assert(
        field.staticKeyNode &&
        "non-method Variable-backed field must be static");
    sema::Decl *decl = semCtx_.getExpressionDecl(field.staticKeyNode);
    auto [initValue, storageType] = getClassMemberVarInit(field.type);
    defineClassMemberVar(decl, field.name, storageType, initValue);
  };

  // Final, private and accessor methods are not stored on the home object.
  forEachVariableBackedClassMember(
      classType->getHomeObjectTypeInfo(), declareMethodVar, declareFieldVar);

  // Specializations of generic methods each get their own Variable.
  for (const auto &[specializedMethod, specializedDecl] :
       classType->getSpecializedMethodDecls()) {
    defineClassMemberVar(
        specializedDecl,
        specializedDecl->name,
        closureStorageType,
        Builder.getLiteralEmpty());
  }

  // All static elements are currently Variable-backed.
  forEachVariableBackedClassMember(
      classType->getStaticObjectTypeInfo(), declareMethodVar, declareFieldVar);
}

CreateFunctionInst *ESTreeIRGen::genTypedImplicitConstructor(
    const Identifier &consName,
    Value *superClass) {
  Function *func;

  // Use the compiledEntities_ cache even though we're not enqueuing a
  // function compilation (because the function is trivial).
  // This way we avoid making multiple implicit constructors for the same
  // classType, allowing us to populate the target operand of CallInsts.
  if (Value *found = findCompiledEntity(
          curFunction()->typedClassContext.node,
          ExtraKey::ImplicitClassConstructor)) {
    func = llvh::cast<Function>(found);
  } else {
    IRBuilder::SaveRestore saveState{Builder};

    // Retrieve the FunctionInfo for the implicit constructor, which must exist.
    sema::FunctionInfo *funcInfo =
        ESTree::getDecoration<ESTree::ClassLikeDecoration>(
            curFunction()->typedClassContext.node)
            ->implicitCtorFunctionInfo;
    assert(
        funcInfo &&
        "Semantic resolver failed to decorate class with implicit ctor");

    // AST Node for the superClass, null if no superclass.
    ESTree::Node *superClassNode = superClass
        ? curFunction()->typedClassContext.node->_superClass
        : nullptr;

    // Determine if the implicit constructor needs to emit a super() call.
    // This is needed only if some ancestor has an explicit constructor or
    // field initializers that must be triggered.
    bool needsSuperCall = false;
    if (superClassNode) {
      for (auto *cur =
               curFunction()->typedClassContext.type->getSuperClassInfo();
           cur;
           cur = cur->getSuperClassInfo()) {
        if (cur->getConstructorType() || classFieldInitInfo_.count(cur)) {
          needsSuperCall = true;
          break;
        }
      }
    }

    func = Builder.createFunction(
        consName,
        needsSuperCall ? Function::DefinitionKind::ES6DerivedConstructor
                       : Function::DefinitionKind::ES5Function,
        true,
        funcInfo->customDirectives);

    auto compileFunc = [this,
                        func,
                        funcInfo,
                        superClassNode,
                        needsSuperCall,
                        typedClassContext = curFunction()->typedClassContext,
                        parentScope =
                            curFunction()->curScope()->getVariableScope()] {
      FunctionContext newFunctionContext{this, func, funcInfo};
      newFunctionContext.typedClassContext = typedClassContext;
      newFunctionContext.superClassNode_ = superClassNode;

      auto *prologueBB = Builder.createBasicBlock(func);
      Builder.setInsertionBlock(prologueBB);

      emitFunctionPrologue(
          nullptr,
          prologueBB,
          InitES5CaptureState::No,
          DoEmitDeclarations::No,
          parentScope);

      if (needsSuperCall) {
        // Generate implicit super call forwarding the parameters.
        // Walk up the class chain to find the nearest explicit constructor.
        flow::TypedFunctionType *superCtorTypeInfo = nullptr;
        for (auto *cur = typedClassContext.type->getSuperClassInfo(); cur;
             cur = cur->getSuperClassInfo()) {
          if (cur->getConstructorType()) {
            superCtorTypeInfo = cur->getConstructorTypeInfo();
            break;
          }
        }

        // Forward the super constructor's parameters, if any.
        CallInst::ArgumentList args;
        if (superCtorTypeInfo) {
          auto params = superCtorTypeInfo->getParams();
          for (const flow::TypedFunctionType::Param &param : params) {
            auto *jsParam = Builder.createJSDynamicParam(func, param.name);
            jsParam->setType(flowTypeToIRType(param.type));
            auto *loadParam = Builder.createLoadParamInst(jsParam);
            curFunction()->jsParams.push_back(loadParam);
            args.push_back(loadParam);
          }
          // +1 for 'this'.
          func->setExpectedParamCountIncludingThis(params.size() + 1);
        }

        // Load the super class callee and call it.
        Value *callee = genExpression(superClassNode);
        Value *thisVal = curFunction()->jsParams[0];
        Value *newTarget =
            Builder.createGetNewTargetInst(func->getNewTargetParam());

        Builder.createCallInst(
            callee,
            /* target */ Builder.getEmptySentinel(),
            /* calleeIsAlwaysClosure */ true,
            Builder.getEmptySentinel(),
            newTarget,
            thisVal,
            args);
      }

      emitTypedFieldInitCall(curFunction()->typedClassContext.type);

      emitFunctionEpilogue(Builder.getLiteralUndefined());
    };
    enqueueCompilation(
        curFunction()->typedClassContext.node,
        ExtraKey::ImplicitClassConstructor,
        func,
        compileFunc);
  }

  return Builder.createCreateFunctionInst(curFunction()->curScope(), func);
}

Value *ESTreeIRGen::emitTypedClassAllocation(
    flow::ClassType *classType,
    Value *parent,
    bool propertiesEnumerable) {
  assert(parent && "parent must be specified");
  // TODO: should create a sealed object, etc.
  AllocTypedObjectInst::ObjectPropertyMap propMap{};

  // Number of layout slots, which includes both public and private fields.
  // Variable-backed methods don't get layout slots, so use
  // getNumLayoutSlots() instead of getFieldNameMap().size().
  size_t numFields = classType->getNumLayoutSlots();
  propMap.resize(numFields);
  auto addField = [this, &propMap, classType, parent](
                      const flow::ClassType::FieldLookupEntry &entry) {
    const flow::ClassType::Field &field = *entry.getField();

    if (!field.isStoredOnObject())
      return;

    // Verify that each layout slot is filled exactly once.
    assert(
        (!field.layoutSlotIR.hasValue() ||
         propMap[*field.layoutSlotIR].first == nullptr) &&
        "every layout slot must be filled exactly once");

    Literal *name = field.isPrivate
        ? Builder.getLiteralPrivateName(field.name)
        : static_cast<Literal *>(Builder.getLiteralString(field.name));

    if (field.isMethod()) {
      if (entry.classType == classType) {
        // Create the code for the method.
        Value *function = genFunctionExpression(
            llvh::cast<ESTree::FunctionExpressionNode>(field.method->_value),
            field.name);
        propMap[*field.layoutSlotIR] = {name, function};
        if (auto *CFI = llvh::dyn_cast<CreateFunctionInst>(function)) {
          // If this field represents a non-overridden method, record the
          // IR function so we can populate the target of calls.
          if (!field.overridden) {
            auto [it, success] = nonOverriddenMethods_.try_emplace(
                &field, CFI->getFunctionCode());
            (void)it;
            (void)success;
            // On recompilation (e.g., finally blocks), the entry may already
            // exist. Verify it has the same Function.
            assert(
                (success || it->second == CFI->getFunctionCode()) &&
                "Method already emitted with different function");
          }
        }
      } else {
        assert(parent && "inherited field without parent ClassType");
        // Method is inherited. Read it from the parent.
        propMap[*field.layoutSlotIR] = {
            name,
            Builder.createPrLoadInst(
                parent,
                *field.layoutSlotIR,
                name,
                flowTypeToIRType(field.type))};
      }
    } else {
      // Class element is a field.
      // Types without a representable literal default (object types and symbol)
      // are stored uninitialized and IDZ-checked on load.
      Type irType = flowTypeToIRType(field.type);
      Value *initValue = getTypeContext().isIDZType(irType)
          ? Builder.getLiteralUninit()
          : getDefaultInitValue(field.type);
      propMap[*field.layoutSlotIR] = {name, initValue};
    }
  };

  // Generate code for each field, place it in the propMap.
  for (const auto &it : classType->getFieldNameMap()) {
    flow::ClassType::FieldLookupEntry entry = it.second;
    addField(entry);
  }
  // Private fields aren't in fieldNameMap_, so iterate over superclasses to
  // find them. Variable-backed private methods are ignored by addField.
  for (auto *cur = classType; cur; cur = cur->getSuperClassInfo()) {
    for (const auto &[name, idx] : cur->getPrivateFieldNameMap()) {
      flow::ClassType::FieldLookupEntry entry{cur, idx};
      addField(entry);
    }
  }

  Value *obj = propertiesEnumerable
      ? static_cast<Value *>(
            Builder.createAllocTypedObjectInst(propMap, parent))
      : static_cast<Value *>(
            Builder.createAllocTypedNonEnumObjectInst(propMap, parent));
  // The allocation instruction's inherent type is the generic `object`. Narrow
  // it (trusted -- we just built it, no runtime check) to the class instance
  // type so the home object, static object, and real instances all carry their
  // refined ClassInstance type. This makes the home-object StoreFrame
  // type-check and turns the redundant object->instance casts at `new` sites
  // into no-ops.
  return Builder.createUnionNarrowTrustedInst(obj, flowTypeToIRType(classType));
}

Value *ESTreeIRGen::getDefaultInitValue(flow::Type *type) {
  switch (type->info->getKind()) {
    case flow::TypeKind::Void:
      return Builder.getLiteralUndefined();
    case flow::TypeKind::Null:
      return Builder.getLiteralNull();
    case flow::TypeKind::Boolean:
      return Builder.getLiteralBool(false);
    case flow::TypeKind::String:
      return Builder.getLiteralString("");
    case flow::TypeKind::StringLiteral:
      return Builder.getLiteralString(
          Identifier::getFromPointer(
              llvh::cast<flow::StringLiteralType>(type->info)->getValue()));
    case flow::TypeKind::NumberLiteral:
      return Builder.getLiteralNumber(
          llvh::cast<flow::NumberLiteralType>(type->info)->getValue());
    case flow::TypeKind::BooleanLiteral:
      return Builder.getLiteralBool(
          llvh::cast<flow::BooleanLiteralType>(type->info)->getValue());
    case flow::TypeKind::CPtr:
    case flow::TypeKind::Number:
      return Builder.getLiteralPositiveZero();
    case flow::TypeKind::BigInt:
      return Builder.getLiteralBigInt(
          Mod->getContext().getIdentifier("0").getUnderlyingPointer());
    case flow::TypeKind::Symbol:
    case flow::TypeKind::Any:
    case flow::TypeKind::Empty:
    case flow::TypeKind::Mixed:
      return Builder.getLiteralUndefined();
    case flow::TypeKind::Union: {
      for (auto *arm : llvh::cast<flow::UnionType>(type->info)->getTypes()) {
        // Try to find a type that is not IDZ.
        if (!getTypeContext().isIDZType(flowTypeToIRType(arm)))
          return getDefaultInitValue(arm);
      }
      return Builder.getLiteralPositiveZero();
    }
    case flow::TypeKind::TypedFunction:
    case flow::TypeKind::NativeFunction:
    case flow::TypeKind::UntypedFunction:
    case flow::TypeKind::Class:
    case flow::TypeKind::ClassConstructor:
    case flow::TypeKind::Tuple:
    case flow::TypeKind::ExactObject:
      return Builder.getLiteralPositiveZero();
    case flow::TypeKind::Generic:
    case flow::TypeKind::InferencePlaceholder:
    case flow::TypeKind::InferencePlaceholderArray:
      hermes_fatal("invalid typekind");
  }
  llvm_unreachable("all cases handled");
}

std::pair<Value *, Type> ESTreeIRGen::getClassMemberVarInit(flow::Type *type) {
  TypeContext &tc = getTypeContext();
  Type irType = flowTypeToIRType(type);
  // Types without a representable literal default (object types and symbol)
  // start out empty, like a TDZ variable, and are checked on load.
  if (tc.isIDZType(irType))
    return {Builder.getLiteralEmpty(), tc.unionTy(irType, Type::createEmpty())};
  return {getDefaultInitValue(type), irType};
}

unsigned FlowTypeStructuralInfo::getHashValue(const flow::TypeInfo *t) {
  // TypeInfo::hash reads only t's own node, so every function signature of
  // one arity hashes alike, as does every tuple of one length and every
  // object with one number of fields. Mix in the children's hashes (and the
  // field names, which TypeInfo::equals compares) to spread them.
  unsigned h = t->hash();
  if (auto *ft = llvh::dyn_cast<flow::TypedFunctionType>(t)) {
    h = (unsigned)llvh::hash_combine(h, ft->getReturnType()->info->hash());
    if (ft->getThisParam())
      h = (unsigned)llvh::hash_combine(h, ft->getThisParam()->info->hash());
    for (const flow::TypedFunctionType::Param &p : ft->getParams())
      h = (unsigned)llvh::hash_combine(h, p.type->info->hash());
  } else if (auto *tt = llvh::dyn_cast<flow::TupleType>(t)) {
    for (const flow::Type *elem : tt->getTypes())
      h = (unsigned)llvh::hash_combine(h, elem->info->hash());
  } else if (auto *ot = llvh::dyn_cast<flow::ExactObjectType>(t)) {
    for (const flow::ExactObjectType::Field &f : ot->getFields()) {
      h = (unsigned)llvh::hash_combine(
          h, f.name.getUnderlyingPointer(), f.type->info->hash());
    }
  }
  return h;
}

bool FlowTypeStructuralInfo::isEqual(
    const flow::TypeInfo *a,
    const flow::TypeInfo *b) {
  if (a == b)
    return true;
  if (a == getEmptyKey() || a == getTombstoneKey() || b == getEmptyKey() ||
      b == getTombstoneKey())
    return false;
  return a->equals(b);
}

llvh::Optional<Type> ESTreeIRGen::beginStructuralLowering(
    const flow::TypeInfo *key,
    Type (TypeContext::*reserve)()) {
  auto done = loweredStructuralTypes_.find(key);
  if (done != loweredStructuralTypes_.end())
    return done->second;
  auto [it, inserted] =
      loweringInProgress_.try_emplace(key, Type::createNoType());
  if (inserted)
    return llvh::None;
  // Reached again while lowering the children of key (or of a type equal to
  // it): the type is recursive. Reserve an entry for it, to be filled when its
  // lowering finishes. Only the kind and the id of the reserved entry are
  // valid until then, which is all the type algebra reads from an Array,
  // Function, Tuple or ExactObject entry.
  if (it->second == Type::createNoType())
    it->second = (getTypeContext().*reserve)();
  return it->second;
}

Type ESTreeIRGen::finishStructuralLowering(const flow::TypeInfo *key) {
  auto it = loweringInProgress_.find(key);
  assert(it != loweringInProgress_.end() && "structural lowering not begun");
  Type reserved = it->second;
  loweringInProgress_.erase(it);
  return reserved;
}

Type ESTreeIRGen::recordStructuralLowering(
    const flow::TypeInfo *key,
    Type result) {
  bool inserted = loweredStructuralTypes_.insert({key, result}).second;
  (void)inserted;
  assert(inserted && "structural type lowered twice");
  return result;
}

Type ESTreeIRGen::lowerTypedFunction(flow::TypedFunctionType *ft) {
  if (llvh::Optional<Type> known =
          beginStructuralLowering(ft, &TypeContext::reserveFunction))
    return *known;

  TypeContext &tc = getTypeContext();
  Type ret = flowTypeToIRType(ft->getReturnType());
  Type thisTy = ft->getThisParam() ? flowTypeToIRType(ft->getThisParam())
                                   : Type::createNoType();
  Type restTy = Type::createNoType();
  llvh::SmallVector<FunctionParam, 4> params;
  for (const flow::TypedFunctionType::Param &p : ft->getParams()) {
    Type pty = flowTypeToIRType(p.type);
    if (p.rest) {
      restTy = pty;
      continue;
    }
    params.push_back(FunctionParam{pty.getRawId(), p.optional});
  }

  Type reserved = finishStructuralLowering(ft);
  if (reserved == Type::createNoType()) {
    return recordStructuralLowering(
        ft,
        tc.createFunction(
            ret, thisTy, restTy, params, ft->isAsync(), ft->isGenerator()));
  }
  tc.fillFunction(
      reserved, ret, thisTy, restTy, params, ft->isAsync(), ft->isGenerator());
  return recordStructuralLowering(ft, reserved);
}

Type ESTreeIRGen::lowerArrayClassType(flow::ClassType *arrayClass) {
  // The Array ClassType is canonical per element type: Flow caches generic
  // specializations by their type arguments.
  const flow::TypeInfo *key = arrayClass;
  if (llvh::Optional<Type> known =
          beginStructuralLowering(key, &TypeContext::reserveArray))
    return *known;

  Type elem = flowTypeToIRType(flowContext_.getArrayElementType(arrayClass));

  TypeContext &tc = getTypeContext();
  Type reserved = finishStructuralLowering(key);
  if (reserved == Type::createNoType())
    return recordStructuralLowering(key, tc.createArray(elem));
  tc.fillArray(reserved, elem);
  return recordStructuralLowering(key, reserved);
}

Type ESTreeIRGen::lowerTupleType(flow::TupleType *tt) {
  if (llvh::Optional<Type> known =
          beginStructuralLowering(tt, &TypeContext::reserveTuple))
    return *known;

  llvh::SmallVector<Type, 4> elems;
  for (flow::Type *elem : tt->getTypes())
    elems.push_back(flowTypeToIRType(elem));

  TypeContext &tc = getTypeContext();
  Type reserved = finishStructuralLowering(tt);
  if (reserved == Type::createNoType())
    return recordStructuralLowering(tt, tc.createTuple(elems));
  tc.fillTuple(reserved, elems);
  return recordStructuralLowering(tt, reserved);
}

Type ESTreeIRGen::lowerExactObjectType(flow::ExactObjectType *ot) {
  // D7 (phase3-5-design.md): an object type with an indexer, e.g.
  // {[string]: number}, lowers to Object. An indexer describes an open key
  // set, which the closed, ordered field list of an IR ExactObject cannot
  // represent. Its key and value types are not lowered, so a recursive type
  // that cycles through an indexer is cut here.
  if (ot->hasIndexer())
    return Type::createObject();

  if (llvh::Optional<Type> known =
          beginStructuralLowering(ot, &TypeContext::reserveExactObject))
    return *known;

  // D8 (phase3-5-design.md): the FieldVariance of each field is dropped, so
  // {+x: number}, {-x: number} and {x: number} lower to the same IR type.
  // As a consequence, the IR field types are not an upper bound on what an
  // object of this type holds: Flow lets {x: number | string} flow into
  // {-x: number}. Nothing may type a value loaded from such an object by the
  // field types of its IR type; IRGen types member loads by the Flow type.
  llvh::SmallVector<ExactObjectField, 4> fields;
  for (const flow::ExactObjectType::Field &f : ot->getFields())
    fields.push_back(ExactObjectField{f.name, flowTypeToIRType(f.type)});

  TypeContext &tc = getTypeContext();
  Type reserved = finishStructuralLowering(ot);
  if (reserved == Type::createNoType())
    return recordStructuralLowering(ot, tc.createExactObject(fields));
  tc.fillExactObject(reserved, fields);
  return recordStructuralLowering(ot, reserved);
}

Type ESTreeIRGen::flowTypeToIRType(flow::Type *flowType) {
  return flowTypeToIRType(flowType->info);
}

Type ESTreeIRGen::flowTypeToIRType(flow::TypeInfo *flowType) {
  switch (flowType->getKind()) {
    case flow::TypeKind::Void:
      return Type::createUndefined();
    case flow::TypeKind::Null:
      return Type::createNull();
    case flow::TypeKind::Boolean:
    case flow::TypeKind::BooleanLiteral:
      return Type::createBoolean();
    case flow::TypeKind::String:
    case flow::TypeKind::StringLiteral:
      return Type::createString();
    case flow::TypeKind::CPtr:
    case flow::TypeKind::Number:
    case flow::TypeKind::NumberLiteral:
      return Type::createNumber();
    case flow::TypeKind::Symbol:
      return Type::createSymbol();
    case flow::TypeKind::BigInt:
      return Type::createBigInt();
    case flow::TypeKind::Any:
    case flow::TypeKind::Empty:
    case flow::TypeKind::Mixed:
      return Type::createAnyType();
    case flow::TypeKind::Union: {
      TypeContext &tc = getTypeContext();
      Type res = Type::createNoType();
      for (flow::Type *elemType :
           llvh::cast<flow::UnionType>(flowType)->getTypes()) {
        res = tc.unionTy(res, flowTypeToIRType(elemType));
      }
      return res;
    }
    case flow::TypeKind::NativeFunction:
      return Type::createNumber();
    case flow::TypeKind::TypedFunction:
      return lowerTypedFunction(llvh::cast<flow::TypedFunctionType>(flowType));
    case flow::TypeKind::UntypedFunction:
      return Type::createObject();
    case flow::TypeKind::Class: {
      auto *classType = llvh::cast<flow::ClassType>(flowType);
      // Surface arrays (T[], Array<T>) are specializations of the builtin
      // Array class. Lower them to an IR Array, which keeps the element type.
      if (flowContext_.isArrayClassType(classType))
        return lowerArrayClassType(classType);
      // Identity cache: return the reserved/finished entry if present.
      auto it = loweredClassTypes_.find(classType);
      if (it != loweredClassTypes_.end())
        return it->second;

      TypeContext &tc = getTypeContext();
      // Lower the superclass first, so that this class is reserved with its
      // superclass already set and subtyping against it is exact while its
      // fields are lowered: in `class D extends Base`, a field of type
      // `D | Base` lowers to Base. Lowering the superclass may lower this
      // class too, through a field of the superclass, so look it up again.
      Type superTy = Type::createNoType();
      if (flow::Type *super = classType->getSuperClass()) {
        superTy = flowTypeToIRType(super);
        it = loweredClassTypes_.find(classType);
        if (it != loweredClassTypes_.end())
          return it->second;
      }

      // Reserve and record this class BEFORE lowering its fields, so a field
      // referencing it (or a mutually recursive sibling) resolves to the
      // reserved Type instead of recursing forever.
      Type t = tc.reserveClassInstance(superTy);
      bool inserted = loweredClassTypes_.insert({classType, t}).second;
      (void)inserted;
      assert(inserted && "class lowered twice");

      llvh::SmallVector<ExactObjectField, 8> fields;
      for (const flow::ClassType::Field &f : classType->getFields()) {
        // Only data fields carry a value type in the IR class type; methods
        // are represented separately and skipped here.
        if (f.isMethod())
          continue;
        fields.push_back(ExactObjectField{f.name, flowTypeToIRType(f.type)});
      }

      tc.fillClassInstance(t, classType->getClassName(), fields);
      return t;
    }
    case flow::TypeKind::ClassConstructor: {
      auto *cc = llvh::cast<flow::ClassConstructorType>(flowType);
      flow::ClassType *ct = cc->getClassTypeInfo();
      // No explicit constructor -> no signature; keep Object. (getConstructor
      // TypeInfo() would null-deref constructorType_ here.)
      if (!ct->getConstructorType())
        return Type::createObject();
      // A constructor's Flow type is always a TypedFunctionType: FlowChecker
      // parses it with a default return type.
      return lowerTypedFunction(ct->getConstructorTypeInfo());
    }
    case flow::TypeKind::Tuple:
      return lowerTupleType(llvh::cast<flow::TupleType>(flowType));
    case flow::TypeKind::ExactObject:
      return lowerExactObjectType(llvh::cast<flow::ExactObjectType>(flowType));
    case flow::TypeKind::Generic:
    case flow::TypeKind::InferencePlaceholder:
    case flow::TypeKind::InferencePlaceholderArray:
      hermes_fatal("invalid typekind");
  }
  llvm_unreachable("all cases handled");
}

} // namespace irgen
} // namespace hermes
