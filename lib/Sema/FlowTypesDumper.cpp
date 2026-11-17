/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FlowTypesDumper.h"

#include "hermes/AST/RecursiveVisitor.h"
#include "hermes/Support/Conversions.h"

namespace hermes {
namespace flow {

/// \return the prefix string denoting a field's variance, if any.
static llvh::StringRef getVariancePrefix(FieldVariance variance) {
  switch (variance) {
    case FieldVariance::None:
      return "";
    case FieldVariance::ReadOnly:
      return "+";
    case FieldVariance::WriteOnly:
      return "-";
  }
  llvm_unreachable("invalid FieldVariance");
}

/// Get the type name formatted as a variable identifier.
static llvh::StringRef getTypeAsVarName(const TypeInfo *type) {
  switch (type->getKind()) {
    case TypeKind::NativeFunction:
      return "native_function";
    case TypeKind::UntypedFunction:
      return "untyped_function";
    case TypeKind::ClassConstructor:
      return "class_constructor";
    default:
      return type->getKindName();
  }
}

size_t FlowTypesDumper::getNumber(const TypeInfo *type) {
  if (type->isSingleton() || type->isLiteral())
    return 0;

  auto [it, inserted] = typeNumber_.try_emplace(type, types_.size());
  if (inserted)
    types_.push_back(type);
  return it->second + 1;
}

void FlowTypesDumper::printTypeRef(
    llvh::raw_ostream &os,
    const TypeInfo *type) {
  if (auto *strLit = llvh::dyn_cast<StringLiteralType>(type)) {
    os << '"' << strLit->getValue()->str() << '"';
  } else if (auto *numLit = llvh::dyn_cast<NumberLiteralType>(type)) {
    char buf[NUMBER_TO_STRING_BUF_SIZE];
    os << numberToString(numLit->getValue(), buf, sizeof(buf));
  } else if (auto *boolLit = llvh::dyn_cast<BooleanLiteralType>(type)) {
    os << (boolLit->getValue() ? "true" : "false");
  } else if (!type->isSingleton())
    os << "%" << getTypeAsVarName(type) << "." << getNumber(type);
  else
    os << getTypeAsVarName(type);
}

void FlowTypesDumper::printTypeDescription(
    llvh::raw_ostream &os,
    const TypeInfo *type) {
  os << getTypeAsVarName(type);
  if (type->isSingleton()) {
    os << '\n';
    return;
  }

  switch (type->getKind()) {
    case TypeKind::Void:
    case TypeKind::Null:
    case TypeKind::Boolean:
    case TypeKind::String:
    case TypeKind::CPtr:
    case TypeKind::Generic:
    case TypeKind::InferencePlaceholder:
    case TypeKind::Number:
    case TypeKind::Symbol:
    case TypeKind::BigInt:
    case TypeKind::Any:
    case TypeKind::Empty:
    case TypeKind::Mixed:
      llvm_unreachable("singletons already handled");

    case TypeKind::Union: {
      os << '(';
      bool first = true;
      for (Type *t : llvh::cast<UnionType>(type)->getTypes()) {
        if (!first)
          os << " | ";
        first = false;
        printTypeRef(os, t);
      }
      os << ')';
    } break;

    case TypeKind::TypedFunction: {
      auto *funcType = llvh::cast<TypedFunctionType>(type);
      if (funcType->isAsync())
        os << "async ";
      if (funcType->isGenerator())
        os << "generator ";
      os << '(';

      bool first = true;
      if (Type *thisType = funcType->getThisParam()) {
        os << "this: ";
        printTypeRef(os, thisType);
        first = false;
      }
      for (const TypedFunctionType::Param &param : funcType->getParams()) {
        if (!first)
          os << ", ";
        first = false;
        if (param.rest)
          os << "...";
        if (param.name.isValid()) {
          os << param.name;
          if (param.optional)
            os << '?';
          os << ": ";
        }
        printTypeRef(os, param.type);
      }
      os << "): ";
      printTypeRef(os, funcType->getReturnType());
    } break;

    case TypeKind::NativeFunction: {
      auto *nfuncType = llvh::cast<NativeFunctionType>(type);
      os << '(';

      bool first = true;
      for (const TypedFunctionType::Param &param : nfuncType->getParams()) {
        if (!first)
          os << ", ";
        first = false;
        if (param.rest)
          os << "...";
        if (param.name.isValid()) {
          os << param.name;
          if (param.optional)
            os << '?';
        }
        os << ": ";
        printTypeRef(os, param.type);
      }
      os << "): ";
      printTypeRef(os, nfuncType->getReturnType());
      os << " [" << *nfuncType->getSignature() << ']';
    } break;

    case TypeKind::UntypedFunction:
      os << "()";
      break;

    case TypeKind::Class: {
      auto *classType = llvh::cast<ClassType>(type);
      // Array class types: print abbreviated form to avoid churn
      // when Array<T> methods change.
      if (arrayClassInfos_.count(type)) {
        os << '(';
        if (classType->getClassName().isValid())
          os << classType->getClassName();
        auto it = arrayElementTypes_.find(type);
        if (it != arrayElementTypes_.end()) {
          os << '<';
          printTypeRef(os, it->second);
          os << '>';
        }
        os << ')';
        break;
      }
      os << '(';
      if (classType->getClassName().isValid())
        os << classType->getClassName();
      if (classType->getSuperClass()) {
        os << " extends ";
        printTypeRef(os, classType->getSuperClass());
      }
      os << " {\n";
      if (auto *constrType = classType->getConstructorType()) {
        os << "  %constructor: ";
        printTypeRef(os, constrType);
        os << '\n';
      }
      if (auto *homeObject = classType->getHomeObjectType()) {
        os << "  %homeObject: ";
        printTypeRef(os, homeObject);
        os << '\n';
      }
      if (auto *staticObject = classType->getStaticObjectType()) {
        os << "  %staticObject: ";
        printTypeRef(os, staticObject);
        os << '\n';
      }
      for (const ClassType::Field &field : classType->getFields()) {
        os << "  " << field.name;
        if (field.isMethod())
          os << (field.overridden ? " [overridden]" : " [final]");
        if (field.isOverloaded()) {
          os << " [overloaded]:";
          for (const auto &[overloadMethod, overloadType] : field.overloads) {
            os << ' ';
            printTypeRef(os, overloadType);
          }
          os << '\n';
        } else {
          os << ": ";
          printTypeRef(os, field.type);
          os << '\n';
        }
      }
      os << "})";
    } break;

    case TypeKind::ClassConstructor:
      os << '(';
      printTypeRef(os, llvh::cast<ClassConstructorType>(type)->getClassType());
      os << ')';
      break;

    case TypeKind::InferencePlaceholderArray:
      os << '(';
      printTypeRef(
          os, llvh::cast<InferencePlaceholderArrayType>(type)->getElement());
      os << ')';
      break;

    case TypeKind::Tuple: {
      os << '(';
      bool first = true;
      for (Type *t : llvh::cast<TupleType>(type)->getTypes()) {
        if (!first)
          os << ", ";
        first = false;
        printTypeRef(os, t);
      }
      os << ')';
      break;
    }

    case TypeKind::StringLiteral:
      os << '"' << llvh::cast<StringLiteralType>(type)->getValue()->str()
         << '"';
      break;

    case TypeKind::NumberLiteral: {
      char buf[NUMBER_TO_STRING_BUF_SIZE];
      os << numberToString(
          llvh::cast<NumberLiteralType>(type)->getValue(), buf, sizeof(buf));
      break;
    }

    case TypeKind::BooleanLiteral: {
      bool value = llvh::cast<BooleanLiteralType>(type)->getValue();
      os << (value ? "true" : "false");
      break;
    }

    case TypeKind::ExactObject: {
      auto *exactObj = llvh::cast<ExactObjectType>(type);
      os << "({\n";
      for (const auto &field : exactObj->getFields()) {
        os << "  " << getVariancePrefix(field.variance) << field.name << ": ";
        printTypeRef(os, field.type);
        os << '\n';
      }
      if (const auto &indexer = exactObj->getIndexer()) {
        os << "  " << getVariancePrefix(indexer->variance) << '[';
        printTypeRef(os, indexer->keyType);
        os << "]: ";
        printTypeRef(os, indexer->valueType);
        os << '\n';
      }
      os << "})";
      break;
    }
  }

  os << '\n';
}

void FlowTypesDumper::printAllNumberedTypes(llvh::raw_ostream &os) {
  for (size_t i = 0; i != types_.size(); ++i) {
    os << "%t." << (i + 1) << " = ";
    printTypeDescription(os, types_[i]);
  }
}

void FlowTypesDumper::printAllTypes(
    llvh::raw_ostream &os,
    const FlowContext &flowTypes,
    ESTree::Node *root) {
  // Walk the AST and collect all referenced TypeInfos.
  llvh::DenseSet<const TypeInfo *> referencedTypes{};
  struct {
    llvh::DenseSet<const TypeInfo *> &referencedTypes;
    const FlowContext &flowTypes;

    bool incRecursionDepth(ESTree::Node *) {
      return true;
    }
    void decRecursionDepth() {}

    void addType(Type *type) {
      if (type && type->info)
        referencedTypes.insert(type->info);
    }

    void visitNode(ESTree::Node *node) {
      addType(flowTypes.findNodeType(node));
      ESTree::visitESTreeChildren(*this, node);
    }

    void visit(ESTree::Node *node) {
      visitNode(node);
    }

    void visit(ESTree::IdentifierNode *ident) {
      if (ident->decl_) {
        auto *decl = static_cast<sema::Decl *>(ident->decl_);
        addType(flowTypes.findDeclType(decl));
      }
      visitNode(ident);
    }
  } visitor{referencedTypes, flowTypes};

  ESTree::visitESTreeNodeNoReplace(visitor, root);

  // Collect array class types for abbreviated printing.
  for (const Type &t : flowTypes.allocTypes_) {
    if (flowTypes.isArrayClassType(&t) && t.info) {
      arrayClassInfos_.insert(t.info);
      arrayElementTypes_[t.info] = flowTypes.getArrayElementType(&t)->info;
      // Also suppress the homeObject ClassType.
      auto *classType = llvh::cast<ClassType>(t.info);
      if (auto *homeObj = classType->getHomeObjectType())
        arrayClassInfos_.insert(homeObj->info);
    }
  }

  // Seed the worklist with referenced types in allocation order.
  for (const Type &t : flowTypes.allocTypes_) {
    if (t.info && referencedTypes.count(t.info)) {
      getNumber(t.info);
    }
  }

  // Process the worklist. As we print type descriptions, printTypeRef
  // calls getNumber, which may add new sub-types to types_, extending
  // the worklist.
  for (size_t i = 0; i < types_.size(); ++i) {
    const TypeInfo *info = types_[i];
    printTypeRef(os, info);
    os << " = ";
    printTypeDescription(os, info);
  }
}

void FlowTypesDumper::printNativeExterns(
    llvh::raw_ostream &os,
    const NativeContext &nativeContext) {
  for (NativeExtern *ne : nativeContext.getAllExterns()) {
    os << "extern \"C\" ";
    ne->signature()->format(os, ne->name()->c_str());
    os << ';';
    if (ne->declared())
      os << " //declared";
    os << '\n';
  }
}

} // namespace flow
} // namespace hermes
