/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSParserImpl.h" // @manual

#include "hermes/AST/ESTreeJSONDumper.h"
#include "hermes/Support/PerfSection.h"

#include "llvh/Support/SaveAndRestore.h"

namespace hermes {
namespace parser {
namespace detail {

#if HERMES_PARSE_TS

Optional<ESTree::Node *> JSParserImpl::parseTypeAnnotationTS(
    Optional<SMLoc> wrappedStart) {
  llvh::SaveAndRestore<bool> saveParam(allowAnonFunctionType_, true);

  SMLoc start = tok_->getStartLoc();
  ESTree::Node *result = nullptr;

  if (check(TokenKind::identifier)) {
    // Need to check if this is a predicate, which requires backtracking.
    JSLexer::SavePoint savePoint{&lexer_};
    ESTree::Node *id = setLocation(
        tok_,
        tok_,
        new (context_)
            ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
    advance(JSLexer::GrammarContext::Type);
    if (check(isIdent_)) {
      SMLoc wrappedStart = advance(JSLexer::GrammarContext::Type).Start;
      CHECK_RECURSION;
      auto optType = parseTypeAnnotationTS(wrappedStart);
      if (!optType)
        return None;
      result = setLocation(
          start,
          getPrevTokenEndLoc(),
          new (context_) ESTree::TSTypePredicateNode(id, *optType));
    } else {
      savePoint.restore();
    }
  }

  if (!result) {
    if (check(TokenKind::rw_new)) {
      advance(JSLexer::GrammarContext::Type);
      ESTree::Node *typeParams = nullptr;
      if (check(TokenKind::less)) {
        auto optTypeParams = parseTSTypeParameters();
        if (!optTypeParams)
          return None;
        typeParams = *optTypeParams;
      }
      if (!need(
              TokenKind::l_paren,
              "in constructor type",
              "start of type",
              start))
        return None;
      auto optResult = parseTSFunctionOrParenthesizedType(
          start, typeParams, IsConstructorType::Yes);
      if (!optResult)
        return None;
      result = *optResult;
    } else if (check(TokenKind::less)) {
      auto optTypeParams = parseTSTypeParameters();
      if (!optTypeParams)
        return None;
      if (!need(TokenKind::l_paren, "in function type", "start of type", start))
        return None;
      auto optResult = parseTSFunctionOrParenthesizedType(
          start, *optTypeParams, IsConstructorType::No);
      if (!optResult)
        return None;
      result = *optResult;
    } else {
      auto optResult = parseTSUnionType();
      if (!optResult)
        return None;
      result = *optResult;
    }
  }

  if (checkAndEat(TokenKind::rw_extends, JSLexer::GrammarContext::Type)) {
    // Parse a conditional type.
    auto optCheck = parseTypeAnnotationTS();
    if (!optCheck)
      return None;
    if (!eat(
            TokenKind::question,
            JSLexer::GrammarContext::Type,
            "in conditional type",
            "start of type",
            start))
      return None;

    auto optTrue = parseTypeAnnotationTS();
    if (!optTrue)
      return None;
    if (!eat(
            TokenKind::colon,
            JSLexer::GrammarContext::Type,
            "in conditional type",
            "start of type",
            start))
      return None;

    auto optFalse = parseTypeAnnotationTS();
    if (!optFalse)
      return None;

    result = setLocation(
        result,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TSConditionalTypeNode(
            result, *optCheck, *optTrue, *optFalse));
  }

  if (wrappedStart)
    return setLocation(
        *wrappedStart,
        result,
        new (context_) ESTree::TSTypeAnnotationNode(result));

  return result;
}

Optional<ESTree::Node *> JSParserImpl::parseTSUnionType() {
  SMLoc start = tok_->getStartLoc();
  checkAndEat(TokenKind::pipe, JSLexer::GrammarContext::Type);

  auto optFirst = parseTSIntersectionType();
  if (!optFirst)
    return None;

  if (!check(TokenKind::pipe)) {
    // Done with the union, move on.
    return *optFirst;
  }

  ESTree::NodeList types{};
  types.push_back(**optFirst);

  while (checkAndEat(TokenKind::pipe, JSLexer::GrammarContext::Type)) {
    auto optInt = parseTSIntersectionType();
    if (!optInt)
      return None;
    types.push_back(**optInt);
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSUnionTypeNode(std::move(types)));
}

Optional<ESTree::Node *> JSParserImpl::parseTSIntersectionType() {
  SMLoc start = tok_->getStartLoc();
  checkAndEat(TokenKind::amp, JSLexer::GrammarContext::Type);

  auto optFirst = parseTSPostfixType();
  if (!optFirst)
    return None;

  if (!check(TokenKind::amp)) {
    // Done with the union, move on.
    return *optFirst;
  }

  ESTree::NodeList types{};
  types.push_back(**optFirst);

  while (checkAndEat(TokenKind::amp, JSLexer::GrammarContext::Type)) {
    auto optInt = parseTSPostfixType();
    if (!optInt)
      return None;
    types.push_back(**optInt);
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSIntersectionTypeNode(std::move(types)));
}

Optional<ESTree::Node *> JSParserImpl::parseTSTupleType() {
  assert(check(TokenKind::l_square));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  ESTree::NodeList types{};

  while (!check(TokenKind::r_square)) {
    auto optType = parseTypeAnnotationTS();
    if (!optType)
      return None;
    types.push_back(**optType);

    if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
      break;
  }

  if (!need(
          TokenKind::r_square,
          "at end of tuple type annotation",
          "start of tuple",
          start))
    return None;

  return setLocation(
      start,
      advance(JSLexer::GrammarContext::Type).End,
      new (context_) ESTree::TSTupleTypeNode(std::move(types)));
}

Optional<ESTree::Node *> JSParserImpl::parseTSFunctionOrParenthesizedType(
    SMLoc start,
    ESTree::Node *typeParams,
    IsConstructorType isConstructorType) {
  assert(check(TokenKind::l_paren));
  // This is either
  // ( Type )
  // ^
  // or
  // ( ParamList ) => Type
  // ^
  // so we use a similar approach to arrow function parameters by keeping track
  // and reparsing in certain cases.
  advance(JSLexer::GrammarContext::Type);

  bool isFunction = typeParams != nullptr;
  bool hasRest = false;
  ESTree::Node *type = nullptr;
  ESTree::NodeList params{};

  if (check(TokenKind::rw_this)) {
    OptValue<TokenKind> optNext = lexer_.lookahead1(None);
    if (optNext.hasValue() && *optNext == TokenKind::colon) {
      SMLoc thisStart = advance(JSLexer::GrammarContext::Type).Start;
      advance(JSLexer::GrammarContext::Type);
      CHECK_RECURSION;
      auto typeAnnotation = parseTypeAnnotationTS();
      if (!typeAnnotation)
        return None;

      params.push_back(*setLocation(
          thisStart,
          getPrevTokenEndLoc(),
          new (context_) ESTree::IdentifierNode(
              thisIdent_, *typeAnnotation, /* optional */ false)));
      checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type);
    } else if (optNext.hasValue() && *optNext == TokenKind::question) {
      error(tok_->getSourceRange(), "'this' constraint may not be optional");
      return None;
    }
  }

  if (allowAnonFunctionType_ &&
      checkAndEat(TokenKind::dotdotdot, JSLexer::GrammarContext::Type)) {
    isFunction = true;
    hasRest = true;
    // Must be parameters, and this must be the last one.
    auto optName = parseTSFunctionTypeParam();
    if (!optName)
      return None;
    params.push_back(*setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::RestElementNode(*optName)));
  } else if (check(TokenKind::l_paren)) {
    auto optType = parseTypeAnnotationTS();
    if (!optType)
      return None;
    type = *optType;
  } else if (check(TokenKind::r_paren)) {
    isFunction = true;
    // ( )
    //   ^
    // No parameters, but this must be an empty param list.
  } else {
    // Not sure yet whether this is a param or simply a type.
    auto optParam = parseTSFunctionTypeParam();
    if (!optParam)
      return None;
    if (auto *param =
            llvh::dyn_cast<ESTree::TSParameterPropertyNode>(*optParam)) {
      if (param &&
          (param->_accessibility || param->_export || param->_readonly ||
           param->_static)) {
        // Must be a param.
        isFunction = true;
      }
      params.push_back(*param);
    } else if (
        auto *ident = llvh::dyn_cast<ESTree::IdentifierNode>(*optParam)) {
      params.push_back(*ident);
      type = ident->_typeAnnotation
          ? ident->_typeAnnotation
          : reparseIdentifierAsTSTypeAnnotation(ident);
      if (ident->_typeAnnotation || ident->_optional) {
        // Must be a param.
        isFunction = true;
      }
    } else {
      type = *optParam;
      params.push_back(**optParam);
    }
  }

  // If isFunction was already forced by something previously then we
  // have no choice but to attempt to parse as a function type annotation.
  if ((isFunction || allowAnonFunctionType_) &&
      checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type)) {
    isFunction = true;
    while (!check(TokenKind::r_paren)) {
      bool isRest = !hasRest &&
          checkAndEat(TokenKind::dotdotdot, JSLexer::GrammarContext::Type);

      auto optParam = parseTSFunctionTypeParam();
      if (!optParam)
        return None;
      if (isRest) {
        params.push_back(*setLocation(
            start,
            getPrevTokenEndLoc(),
            new (context_) ESTree::RestElementNode(*optParam)));
        checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type);
        break;
      } else {
        params.push_back(**optParam);
      }

      if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
        break;
    }
  }

  if (!eat(
          TokenKind::r_paren,
          JSLexer::GrammarContext::Type,
          "at end of function type parameters",
          "start of parameters",
          start))
    return None;

  if (isFunction) {
    if (!eat(
            TokenKind::equalgreater,
            JSLexer::GrammarContext::Type,
            "in function type",
            "start of function",
            start))
      return None;
  } else if (allowAnonFunctionType_) {
    if (checkAndEat(TokenKind::equalgreater, JSLexer::GrammarContext::Type)) {
      isFunction = true;
    }
  }

  if (!isFunction) {
    type->incParens();
    return type;
  }

  auto optReturnType = parseTypeAnnotationTS();
  if (!optReturnType)
    return None;

  if (isConstructorType == IsConstructorType::Yes) {
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TSConstructorTypeNode(
            std::move(params), *optReturnType, typeParams));
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSFunctionTypeNode(
          std::move(params), *optReturnType, typeParams));
}

bool JSParserImpl::parseTSFunctionTypeParams(
    SMLoc start,
    ESTree::NodeList &params) {
  assert(check(TokenKind::l_paren));

  advance(JSLexer::GrammarContext::Type);

  while (!check(TokenKind::r_paren)) {
    auto optParam = parseTSFunctionTypeParam();
    if (!optParam)
      return false;
    params.push_back(**optParam);

    if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
      break;
  }

  if (!eat(
          TokenKind::r_paren,
          JSLexer::GrammarContext::Type,
          "at end of function type parameters",
          "start of parameters",
          start))
    return false;

  return true;
}

Optional<ESTree::Node *> JSParserImpl::parseTSFunctionTypeParam() {
  SMLoc start = tok_->getStartLoc();

  ESTree::NodeLabel accessibilityNode = nullptr;
  bool readonlyNode = false;
  bool staticNode = false;
  bool exportNode = false;

  while (checkN(
      TokenKind::identifier, TokenKind::rw_static, TokenKind::rw_export)) {
    // Check if this is a modifier.
    if (!staticNode && checkN(TokenKind::rw_static, staticIdent_)) {
      advance(JSLexer::GrammarContext::Type);
      if (checkN(
              TokenKind::identifier,
              TokenKind::rw_static,
              TokenKind::rw_export)) {
        staticNode = true;
        continue;
      }
    }
    if (!exportNode && checkN(TokenKind::rw_export)) {
      advance(JSLexer::GrammarContext::Type);
      if (checkN(
              TokenKind::identifier,
              TokenKind::rw_static,
              TokenKind::rw_export)) {
        exportNode = true;
        continue;
      }
    }
    if (!readonlyNode && checkN(readonlyIdent_)) {
      advance(JSLexer::GrammarContext::Type);
      if (checkN(
              TokenKind::identifier,
              TokenKind::rw_static,
              TokenKind::rw_export)) {
        readonlyNode = true;
        continue;
      }
    }
    if (!accessibilityNode) {
      if (checkN(TokenKind::rw_public, publicIdent_)) {
        advance(JSLexer::GrammarContext::Type);
        if (checkN(
                TokenKind::identifier,
                TokenKind::rw_static,
                TokenKind::rw_export)) {
          accessibilityNode = publicIdent_;
          continue;
        }
      }
      if (checkN(TokenKind::rw_private, privateIdent_)) {
        advance(JSLexer::GrammarContext::Type);
        if (checkN(
                TokenKind::identifier,
                TokenKind::rw_static,
                TokenKind::rw_export)) {
          accessibilityNode = privateIdent_;
          continue;
        }
      }
      if (checkN(TokenKind::rw_protected, protectedIdent_)) {
        advance(JSLexer::GrammarContext::Type);
        if (checkN(
                TokenKind::identifier,
                TokenKind::rw_static,
                TokenKind::rw_export)) {
          accessibilityNode = protectedIdent_;
          continue;
        }
      }
    }

    // Not a modifier.
    break;
  }

  auto optParam = parseBindingElement(Param{});
  if (!optParam)
    return None;

  if (accessibilityNode || readonlyNode || staticNode || exportNode) {
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TSParameterPropertyNode(
            *optParam,
            accessibilityNode,
            readonlyNode,
            staticNode,
            exportNode));
  }

  return *optParam;
}

Optional<ESTree::Node *> JSParserImpl::parseTSDeclaration() {
  assert(checkDeclaration());

  SMLoc start = tok_->getStartLoc();

  if (checkN(TokenKind::rw_interface, interfaceIdent_)) {
    return parseTSInterfaceDeclaration();
  }

  if (checkAndEat(typeIdent_, JSLexer::GrammarContext::Type)) {
    return parseTSTypeAliasDeclaration(start);
  }

  if (check(namespaceIdent_)) {
    return parseTSNamespaceDeclaration();
  }

  assert(check(TokenKind::rw_enum));
  return parseTSEnumDeclaration();
}

Optional<ESTree::Node *> JSParserImpl::parseTSTypeAliasDeclaration(
    SMLoc start) {
  if (!need(
          TokenKind::identifier, "in type alias", "start of type alias", start))
    return None;

  ESTree::Node *id = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optTypeParams = parseTSTypeParameters();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  if (!eat(
          TokenKind::equal,
          JSLexer::GrammarContext::Type,
          "in type alias",
          "start of type alias",
          start))
    return None;

  auto optRight = parseTypeAnnotationTS();
  if (!optRight)
    return None;
  ESTree::Node *right = *optRight;

  if (!eatSemi(true))
    return None;

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSTypeAliasDeclarationNode(id, typeParams, right));
}

Optional<ESTree::Node *> JSParserImpl::parseTSInterfaceDeclaration() {
  assert(checkN(TokenKind::rw_interface, interfaceIdent_));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  if (!check(TokenKind::identifier) && !tok_->isResWord()) {
    errorExpected(
        TokenKind::identifier,
        "in interface declaration",
        "start of interface",
        start);
    return None;
  }

  ESTree::Node *id = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::IdentifierNode(
          tok_->getResWordOrIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optTypeParams = parseTSTypeParameters();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  ESTree::NodeList extends{};
  if (checkAndEat(
          TokenKind::rw_extends, JSLexer::GrammarContext::AllowRegExp)) {
    do {
      auto optExpr = parseTSTypeReference();
      if (!optExpr)
        return None;
      ESTree::TSTypeReferenceNode *expr = *optExpr;

      ESTree::Node *typeArgs = nullptr;
      if (expr->_typeParameters) {
        typeArgs = expr->_typeParameters;
        expr->_typeParameters = nullptr;
      }

      extends.push_back(*setLocation(
          start,
          getPrevTokenEndLoc(),
          new (context_) ESTree::TSInterfaceHeritageNode(expr, typeArgs)));

      if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
        break;
    } while (!check(TokenKind::l_brace));
  }

  SMLoc bodyStart = tok_->getStartLoc();

  if (!eat(
          TokenKind::l_brace,
          JSLexer::GrammarContext::Type,
          "in interface declaration",
          "start of interface",
          start))
    return None;

  ESTree::NodeList members{};

  while (!check(TokenKind::r_brace)) {
    auto optMember = parseTSObjectTypeMember();
    if (!optMember)
      return None;
    members.push_back(**optMember);

    bool hasNext = check(TokenKind::comma, TokenKind::semi);
    if (hasNext) {
      advance(JSLexer::GrammarContext::Type);
    } else {
      break;
    }
  }

  if (!eat(
          TokenKind::r_brace,
          JSLexer::GrammarContext::Type,
          "at end of object type",
          "start of object type",
          start))
    return None;

  ESTree::Node *body = setLocation(
      bodyStart,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSInterfaceBodyNode(std::move(members)));

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSInterfaceDeclarationNode(
          id, body, std::move(extends), typeParams));
}

Optional<ESTree::Node *> JSParserImpl::parseTSEnumDeclaration() {
  assert(check(TokenKind::rw_enum));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  auto optName = parseBindingIdentifier(Param{});
  if (!optName) {
    errorExpected(
        TokenKind::identifier, "in enum declaration", "start of enum", start);
    return None;
  }
  ESTree::Node *name = *optName;

  if (!eat(
          TokenKind::l_brace,
          JSLexer::GrammarContext::Type,
          "in enum declaration",
          "start of enum",
          start))
    return None;

  ESTree::NodeList members{};

  while (!check(TokenKind::r_brace)) {
    auto optMember = parseTSEnumMember();
    if (!optMember)
      return None;
    members.push_back(**optMember);

    if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
      break;
  }

  if (!eat(
          TokenKind::r_brace,
          JSLexer::GrammarContext::Type,
          "in enum declaration",
          "start of enum",
          start))
    return None;

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSEnumDeclarationNode(name, std::move(members)));
}

Optional<ESTree::Node *> JSParserImpl::parseTSEnumMember() {
  SMLoc start = tok_->getStartLoc();

  auto optName = parseBindingIdentifier(Param{});
  if (!optName) {
    errorExpected(
        TokenKind::identifier, "in enum member", "start of member", start);
    return None;
  }
  ESTree::Node *name = *optName;

  ESTree::Node *init = nullptr;
  if (checkAndEat(TokenKind::equal)) {
    auto optExpr = parseAssignmentExpression();
    if (!optExpr)
      return None;
    init = *optExpr;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSEnumMemberNode(name, init));
}

Optional<ESTree::Node *> JSParserImpl::parseTSNamespaceDeclaration() {
  assert(check(namespaceIdent_));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  auto optName = parseTSQualifiedName();
  if (!optName) {
    errorExpected(
        TokenKind::identifier,
        "in namespace declaration",
        "start of namespace",
        start);
    return None;
  }
  ESTree::Node *name = *optName;

  if (!eat(
          TokenKind::l_brace,
          JSLexer::GrammarContext::Type,
          "in namespace declaration",
          "start of namespace",
          start))
    return None;

  ESTree::NodeList members{};

  while (!check(TokenKind::r_brace)) {
    auto optMember =
        parseStatementListItem(Param{}, AllowImportExport::Yes, members);
    if (!optMember)
      return None;
  }

  if (!eat(
          TokenKind::r_brace,
          JSLexer::GrammarContext::Type,
          "in namespace declaration",
          "start of namespace",
          start))
    return None;

  ESTree::Node *body = setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSModuleBlockNode(std::move(members)));

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSModuleMemberNode(name, body));
}

Optional<ESTree::Node *> JSParserImpl::parseTSTypeParameters() {
  assert(check(TokenKind::less));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  ESTree::NodeList params{};

  do {
    auto optType = parseTSTypeParameter();
    if (!optType)
      return None;
    params.push_back(**optType);

    if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
      break;
  } while (!check(TokenKind::greater));

  SMLoc end = tok_->getEndLoc();
  if (!eat(
          TokenKind::greater,
          JSLexer::GrammarContext::Type,
          "at end of type parameters",
          "start of type parameters",
          start))
    return None;

  return setLocation(
      start,
      end,
      new (context_) ESTree::TSTypeParameterDeclarationNode(std::move(params)));
}

Optional<ESTree::Node *> JSParserImpl::parseTSTypeParameter() {
  SMLoc start = tok_->getStartLoc();

  if (!need(TokenKind::identifier, "in type parameter", nullptr, {}))
    return None;
  ESTree::IdentifierNode *name = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  ESTree::Node *constraint = nullptr;
  if (checkAndEat(TokenKind::rw_extends, JSLexer::GrammarContext::Type)) {
    auto optType = parseTypeAnnotationTS();
    if (!optType)
      return None;
    constraint = *optType;
  }

  ESTree::Node *init = nullptr;
  if (checkAndEat(TokenKind::equal, JSLexer::GrammarContext::Type)) {
    auto optType = parseTypeAnnotationTS();
    if (!optType)
      return None;
    init = *optType;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSTypeParameterNode(name, constraint, init));
}

Optional<ESTree::Node *> JSParserImpl::parseTSPostfixType() {
  SMLoc start = tok_->getStartLoc();
  auto optPrimary = parseTSPrimaryType();
  if (!optPrimary)
    return None;

  ESTree::Node *result = *optPrimary;

  // Parse any [] after the primary type.
  while (!lexer_.isNewLineBeforeCurrentToken() &&
         checkAndEat(TokenKind::l_square, JSLexer::GrammarContext::Type)) {
    if (check(TokenKind::r_square)) {
      result = setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::TSArrayTypeNode(result));
    } else {
      auto optType = parseTypeAnnotationTS();
      if (!optType)
        return None;
      if (!eat(
              TokenKind::r_square,
              JSLexer::GrammarContext::Type,
              "in indexed access type",
              "start of type",
              start))
        return None;
      result = setLocation(
          start,
          getPrevTokenEndLoc(),
          new (context_) ESTree::TSIndexedAccessTypeNode(result, *optType));
    }
  }

  return result;
}

Optional<ESTree::Node *> JSParserImpl::parseTSPrimaryType() {
  CHECK_RECURSION;
  SMLoc start = tok_->getStartLoc();
  switch (tok_->getKind()) {
    case TokenKind::star:
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::ExistsTypeAnnotationNode());
    case TokenKind::l_paren:
      return parseTSFunctionOrParenthesizedType(
          start, nullptr, IsConstructorType::No);
    case TokenKind::l_brace:
      return parseTSObjectType();
    case TokenKind::rw_interface:
      return parseTSInterfaceDeclaration();
    case TokenKind::rw_typeof:
      return parseTSTypeQuery();
    case TokenKind::l_square:
      return parseTSTupleType();
    case TokenKind::rw_this:
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::TSThisTypeNode());
    case TokenKind::rw_static:
    case TokenKind::identifier:
      if (tok_->getResWordOrIdentifier() == anyIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::TSAnyKeywordNode());
      }
      if (tok_->getResWordOrIdentifier() == booleanIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::TSBooleanKeywordNode());
      }
      if (tok_->getResWordOrIdentifier() == numberIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::TSNumberKeywordNode());
      }
      if (tok_->getResWordOrIdentifier() == symbolIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::TSSymbolKeywordNode());
      }
      if (tok_->getResWordOrIdentifier() == stringIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::TSStringKeywordNode());
      }
      if (tok_->getResWordOrIdentifier() == bigintIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::TSBigIntKeywordNode());
      }
      if (tok_->getResWordOrIdentifier() == neverIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::TSNeverKeywordNode());
      }
      if (tok_->getResWordOrIdentifier() == undefinedIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::TSUndefinedKeywordNode());
      }
      if (tok_->getResWordOrIdentifier() == unknownIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::TSUnknownKeywordNode());
      }

      {
        auto optRef = parseTSTypeReference();
        if (!optRef)
          return None;
        return *optRef;
      }

    case TokenKind::rw_null: {
      SMLoc end = advance(JSLexer::GrammarContext::Type).End;
      return setLocation(
          start,
          end,
          new (context_) ESTree::TSLiteralTypeNode(setLocation(
              start, end, new (context_) ESTree::NullLiteralNode())));
    }

    case TokenKind::rw_void:
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::TSVoidKeywordNode());

    case TokenKind::string_literal: {
      UniqueString *str = tok_->getStringLiteral();
      SMLoc end = advance(JSLexer::GrammarContext::Type).End;
      return setLocation(
          start,
          end,
          new (context_) ESTree::TSLiteralTypeNode(setLocation(
              start, end, new (context_) ESTree::StringLiteralNode(str))));
    }

    case TokenKind::numeric_literal: {
      double str = tok_->getNumericLiteral();
      SMLoc end = advance(JSLexer::GrammarContext::Type).End;
      return setLocation(
          start,
          end,
          new (context_) ESTree::TSLiteralTypeNode(setLocation(
              start, end, new (context_) ESTree::NumericLiteralNode(str))));
    }

    case TokenKind::bigint_literal: {
      UniqueString *raw = tok_->getBigIntLiteral();
      SMLoc end = advance(JSLexer::GrammarContext::Type).End;
      return setLocation(
          start,
          end,
          new (context_) ESTree::TSLiteralTypeNode(setLocation(
              start, end, new (context_) ESTree::BigIntLiteralNode(raw))));
    }

    case TokenKind::rw_true:
    case TokenKind::rw_false: {
      bool value = check(TokenKind::rw_true);
      SMLoc end = advance(JSLexer::GrammarContext::Type).End;
      return setLocation(
          start,
          end,
          new (context_) ESTree::TSLiteralTypeNode(setLocation(
              start, end, new (context_) ESTree::BooleanLiteralNode(value))));
    }

    default:
      if (tok_->isResWord()) {
        auto optRef = parseTSTypeReference();
        if (!optRef)
          return None;
        return *optRef;
      }
      error(tok_->getStartLoc(), "unexpected token in type annotation");
      return None;
  }
}

Optional<ESTree::TSTypeReferenceNode *> JSParserImpl::parseTSTypeReference() {
  assert(check(TokenKind::identifier) || tok_->isResWord());
  SMLoc start = tok_->getStartLoc();

  auto optName = parseTSQualifiedName();
  if (!optName)
    return None;
  ESTree::Node *typeName = *optName;

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optTypeParams = parseTSTypeArguments();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSTypeReferenceNode(typeName, typeParams));
}

Optional<ESTree::Node *> JSParserImpl::parseTSQualifiedName() {
  SMLoc start = tok_->getStartLoc();
  ESTree::Node *typeName = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::IdentifierNode(
          tok_->getResWordOrIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  while (checkAndEat(TokenKind::period, JSLexer::GrammarContext::Type)) {
    if (!check(TokenKind::identifier) && !tok_->isResWord()) {
      errorExpected(
          TokenKind::identifier,
          "in qualified type name",
          "start of type name",
          start);
      return None;
    }
    ESTree::Node *right = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(
            tok_->getResWordOrIdentifier(), nullptr, false));
    advance(JSLexer::GrammarContext::Type);
    typeName = setLocation(
        typeName,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TSQualifiedNameNode(typeName, right));
  }

  return typeName;
}

Optional<ESTree::Node *> JSParserImpl::parseTSTypeQuery() {
  assert(check(TokenKind::rw_typeof));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  if (!(tok_->isResWord() || check(TokenKind::identifier))) {
    errorExpected(
        TokenKind::identifier, "in type query", "start of type query", start);
    return None;
  }

  ESTree::Node *typeName = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::IdentifierNode(
          tok_->getResWordOrIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  while (checkAndEat(TokenKind::period, JSLexer::GrammarContext::Type)) {
    if (!check(TokenKind::identifier) && !tok_->isResWord()) {
      errorExpected(
          TokenKind::identifier,
          "in qualified type name",
          "start of type name",
          start);
      return None;
    }
    ESTree::Node *right = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(
            tok_->getResWordOrIdentifier(), nullptr, false));
    advance(JSLexer::GrammarContext::Type);
    typeName = setLocation(
        typeName,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TSQualifiedNameNode(typeName, right));
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSTypeQueryNode(typeName));
}

Optional<ESTree::Node *> JSParserImpl::parseTSTypeArguments() {
  assert(check(TokenKind::less));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  ESTree::NodeList params{};

  while (!check(TokenKind::greater)) {
    auto optType = parseTypeAnnotationTS();
    if (!optType)
      return None;
    params.push_back(**optType);

    if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
      break;
  }

  SMLoc end = tok_->getEndLoc();
  if (!eat(
          TokenKind::greater,
          JSLexer::GrammarContext::Type,
          "at end of type parameters",
          "start of type parameters",
          start))
    return None;

  return setLocation(
      start,
      end,
      new (context_)
          ESTree::TSTypeParameterInstantiationNode(std::move(params)));
}

Optional<ESTree::Node *> JSParserImpl::parseTSObjectType() {
  assert(check(TokenKind::l_brace));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  ESTree::NodeList members{};

  while (!check(TokenKind::r_brace)) {
    auto optMember = parseTSObjectTypeMember();
    if (!optMember)
      return None;
    members.push_back(**optMember);

    bool hasNext = check(TokenKind::comma, TokenKind::semi);
    if (hasNext) {
      advance(JSLexer::GrammarContext::Type);
    } else {
      break;
    }
  }

  if (!eat(
          TokenKind::r_brace,
          JSLexer::GrammarContext::Type,
          "at end of object type",
          "start of object type",
          start))
    return None;

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSTypeLiteralNode(std::move(members)));
}

Optional<ESTree::Node *> JSParserImpl::parseTSObjectTypeMember() {
  SMLoc start = tok_->getStartLoc();

  if (check(TokenKind::l_paren)) {
    ESTree::NodeList params{};
    if (!parseTSFunctionTypeParams(start, params))
      return None;
    ESTree::Node *returnType = nullptr;
    if (checkAndEat(TokenKind::colon, JSLexer::GrammarContext::Type)) {
      auto optType = parseTypeAnnotationTS();
      if (!optType)
        return None;
      returnType = *optType;
    }
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TSCallSignatureDeclarationNode(
            std::move(params), returnType));
  }

  bool optional = false;
  bool computed = false;

  // TODO: Parse modifiers.
  bool readonly = false;
  bool isStatic = false;
  bool isExport = false;

  ESTree::Node *key = nullptr;

  // TODO: Parse initializer.
  ESTree::Node *init = nullptr;

  if (checkAndEat(TokenKind::l_square, JSLexer::GrammarContext::Type)) {
    computed = true;

    if (check(TokenKind::identifier)) {
      auto optNext = lexer_.lookahead1(llvh::None);
      if (optNext.hasValue() && *optNext == TokenKind::colon) {
        // Unambiguously an index signature.
        return parseTSIndexSignature(start);
      }
    }

    auto optExpr = parseAssignmentExpression(ParamIn);
    if (!optExpr)
      return None;
    key = *optExpr;

    if (!eat(
            TokenKind::r_square,
            JSLexer::GrammarContext::Type,
            "at end of computed property type",
            "start of property",
            start))
      return None;

    if (checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type)) {
      optional = true;
    }
  } else {
    if (!need(TokenKind::identifier, "in property", "start of property", start))
      return None;
    key = setLocation(
        tok_,
        tok_,
        new (context_)
            ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
    advance(JSLexer::GrammarContext::Type);

    if (checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type)) {
      optional = true;
    }
  }

  if (check(TokenKind::colon)) {
    SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
    auto optType = parseTypeAnnotationTS(annotStart);
    if (!optType)
      return None;
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TSPropertySignatureNode(
            key,
            *optType,
            init,
            optional,
            computed,
            readonly,
            isStatic,
            isExport));
  }

  if (check(TokenKind::l_paren)) {
    ESTree::NodeList params{};
    if (!parseTSFunctionTypeParams(start, params))
      return None;

    ESTree::Node *returnType = nullptr;
    if (check(TokenKind::colon)) {
      SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
      auto optType = parseTypeAnnotationTS(annotStart);
      if (!optType)
        return None;
      returnType = *optType;
    }

    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TSMethodSignatureNode(
            key, std::move(params), returnType, computed));
  }

  ESTree::Node *returnType = nullptr;
  if (check(TokenKind::colon)) {
    SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
    auto optType = parseTypeAnnotationTS(annotStart);
    if (!optType)
      return None;
    returnType = *optType;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TSPropertySignatureNode(
          key,
          returnType,
          init,
          optional,
          computed,
          readonly,
          isStatic,
          isExport));
}

Optional<ESTree::Node *> JSParserImpl::parseTSIndexSignature(SMLoc start) {
  ESTree::NodeList params{};

  while (!check(TokenKind::r_square)) {
    auto optKey = parseBindingIdentifier(Param{});
    if (!optKey) {
      errorExpected(
          TokenKind::identifier, "in property", "start of property", start);

      return None;
    }
    params.push_back(**optKey);

    if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
      break;
  }

  if (!eat(
          TokenKind::r_square,
          JSLexer::GrammarContext::Type,
          "at end of indexer type annotation",
          "start of indexer",
          start))
    return None;

  ESTree::Node *returnType = nullptr;
  if (check(TokenKind::colon)) {
    SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
    auto optType = parseTypeAnnotationTS(annotStart);
    if (!optType)
      return None;
    returnType = *optType;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::TSIndexSignatureNode(std::move(params), returnType));
}

ESTree::Node *JSParserImpl::reparseIdentifierAsTSTypeAnnotation(
    ESTree::IdentifierNode *ident) {
  UniqueString *name = ident->_name;
  if (name == anyIdent_) {
    return setLocation(ident, ident, new (context_) ESTree::TSAnyKeywordNode());
  }
  if (name == booleanIdent_) {
    return setLocation(
        ident, ident, new (context_) ESTree::TSBooleanKeywordNode());
  }
  if (name == numberIdent_) {
    return setLocation(
        ident, ident, new (context_) ESTree::TSNumberKeywordNode());
  }
  if (name == symbolIdent_) {
    return setLocation(
        ident, ident, new (context_) ESTree::TSSymbolKeywordNode());
  }
  if (name == stringIdent_) {
    return setLocation(
        ident, ident, new (context_) ESTree::TSStringKeywordNode());
  }

  return setLocation(
      ident, ident, new (context_) ESTree::TSTypeReferenceNode(ident, {}));
}

#endif

} // namespace detail
} // namespace parser
} // namespace hermes
