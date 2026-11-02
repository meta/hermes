/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSParserImpl.h" // @manual

#include "llvh/Support/SaveAndRestore.h"

using llvh::dyn_cast;
using llvh::isa;

namespace hermes {
namespace parser {
namespace detail {

#if HERMES_PARSE_FLOW

Optional<ESTree::Node *> JSParserImpl::parseFlowDeclaration() {
  assert(checkDeclaration());
  SMLoc start = tok_->getStartLoc();

  if (context_.getParseFlowComponentSyntax() && check(asyncIdent_) &&
      checkAsyncComponentFlow()) {
    advance(); // consume 'async'
    return parseComponentDeclarationFlow(
        start, /* declare */ false, /* isAsync */ true);
  }

  if (context_.getParseFlowComponentSyntax() && check(asyncIdent_) &&
      checkAsyncHookFlow()) {
    advance(); // consume 'async'
    return parseHookDeclarationFlow(start, /* isAsync */ true);
  }

  if (context_.getParseFlowComponentSyntax() &&
      checkComponentDeclarationFlow()) {
    return parseComponentDeclarationFlow(start, /* declare */ false);
  }

  if (context_.getParseFlowComponentSyntax() && checkHookDeclarationFlow()) {
    return parseHookDeclarationFlow(start);
  }

  if (check(TokenKind::rw_enum)) {
    auto optEnum = parseEnumDeclarationFlow(start, /* declare */ false);
    if (!optEnum)
      return None;
    return *optEnum;
  }

  TypeAliasKind kind = TypeAliasKind::None;
  if (checkAndEat(declareIdent_))
    kind = TypeAliasKind::Declare;
  else if (checkAndEat(opaqueIdent_))
    kind = TypeAliasKind::Opaque;

  if (kind == TypeAliasKind::Declare &&
      !checkN(typeIdent_, interfaceIdent_, TokenKind::rw_interface)) {
    error(tok_->getSourceRange(), "invalid token in type declaration");
    return None;
  }
  if (kind == TypeAliasKind::Opaque && !check(typeIdent_)) {
    error(tok_->getSourceRange(), "invalid token in opaque type declaration");
    return None;
  }

  if (checkAndEat(typeIdent_)) {
    auto optType = parseTypeAliasFlow(start, kind);
    if (!optType)
      return None;
    return *optType;
  }

  if (checkN(interfaceIdent_, TokenKind::rw_interface)) {
    auto optType = parseInterfaceDeclarationFlow(
        kind == TypeAliasKind::Declare ? Optional<SMLoc>(start) : None);
    if (!optType)
      return None;
    return *optType;
  }

  assert(
      kind == TypeAliasKind::None &&
      "checkDeclaration() returned true without 'type' or 'interface'");
  return None;
}

Optional<ESTree::Node *> JSParserImpl::parseDeclareFLow(SMLoc start) {
  if (checkAndEat(typeIdent_)) {
    return parseTypeAliasFlow(start, TypeAliasKind::Declare);
  }
  if (checkAndEat(opaqueIdent_)) {
    if (!check(typeIdent_)) {
      error(tok_->getStartLoc(), "'type' required in opaque type declaration");
      return None;
    }
    advance(JSLexer::GrammarContext::Type);
    return parseTypeAliasFlow(start, TypeAliasKind::DeclareOpaque);
  }
  if (checkN(TokenKind::rw_interface, interfaceIdent_)) {
    return parseInterfaceDeclarationFlow(start);
  }
  if (check(TokenKind::rw_class)) {
    return parseDeclareClassFlow(start);
  }
  if (check(TokenKind::rw_function)) {
    return parseDeclareFunctionFlow(start);
  }

  if (context_.getParseFlowComponentSyntax() && checkHookDeclarationFlow()) {
    return parseDeclareHookFlow(start);
  }

  if (context_.getParseFlowComponentSyntax() && check(asyncIdent_) &&
      checkAsyncHookFlow()) {
    error(
        tok_->getStartLoc(),
        "`async` is not supported for declared hooks. "
        "Use `declare hook` instead.");
    advance(); // consume 'async'
    return parseDeclareHookFlow(start);
  }

  if (context_.getParseFlowComponentSyntax() && check(asyncIdent_) &&
      checkAsyncComponentFlow()) {
    error(
        tok_->getStartLoc(),
        "`async` is not supported for declared components. "
        "Use `declare component` instead.");
    advance(); // consume 'async'
    return parseComponentDeclarationFlow(
        start, /* declare */ true, /* isAsync */ true);
  }

  if (context_.getParseFlowComponentSyntax() &&
      checkComponentDeclarationFlow()) {
    return parseComponentDeclarationFlow(start, /* declare */ true);
  }
  if (check(TokenKind::rw_enum)) {
    return parseEnumDeclarationFlow(start, /* declare */ true);
  }
  if (check(moduleIdent_)) {
    return parseDeclareModuleFlow(start);
  }
  if (check(namespaceIdent_)) {
    return parseDeclareNamespaceFlow(start);
  }
  if (check(TokenKind::rw_var, TokenKind::rw_const) || check(letIdent_)) {
    ESTree::NodeLabel kind = tok_->getResWordOrIdentifier();
    advance();
    auto optIdent = parseBindingIdentifier(Param{});
    if (!optIdent) {
      errorExpected(
          TokenKind::identifier,
          "in var declaration",
          "start of declaration",
          start);
      return None;
    }
    if (!(*optIdent)->_typeAnnotation) {
      error(
          (*optIdent)->getSourceRange(),
          "expected type annotation on declared var");
    }
    if (!eatSemi())
      return None;
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::DeclareVariableNode(*optIdent, kind));
  }

  if (!check(TokenKind::rw_export)) {
    errorExpected(
        {TokenKind::rw_export,
         TokenKind::rw_interface,
         TokenKind::rw_function,
         TokenKind::rw_class,
         TokenKind::rw_var},
        "in declared type",
        "start of declare",
        start);
    return None;
  }

  return parseDeclareExportFlow(start);
}

bool JSParserImpl::checkComponentDeclarationFlow() {
  if (!check(componentIdent_))
    return false;

  // Don't pass an `expectedToken` so we don't advance on a match. This allows
  // `parseComponentDeclarationFlow` to reparse the token and store useful
  // information. Additionally to be used within `checkDeclaration` this
  // function must be idempotent.
  OptValue<TokenKind> optNext = lexer_.lookahead1(None);
  return optNext.hasValue() && *optNext == TokenKind::identifier;
}

bool JSParserImpl::checkAsyncComponentFlow() {
  // async [no LineTerminator here] component
  // ^
  // Callers must already check check(asyncIdent_).
  assert(check(asyncIdent_));
  JSLexer::SavePoint savePoint{&lexer_};
  advance();
  bool result =
      !lexer_.isNewLineBeforeCurrentToken() && checkComponentDeclarationFlow();
  savePoint.restore();
  return result;
}

bool JSParserImpl::checkAsyncHookFlow() {
  // async [no LineTerminator here] hook
  // ^
  // Callers must already check check(asyncIdent_).
  assert(check(asyncIdent_));
  JSLexer::SavePoint savePoint{&lexer_};
  advance();
  bool result =
      !lexer_.isNewLineBeforeCurrentToken() && checkHookDeclarationFlow();
  savePoint.restore();
  return result;
}

Optional<ESTree::Node *> JSParserImpl::parseComponentDeclarationFlow(
    SMLoc start,
    bool declare,
    bool isAsync) {
  // component
  assert(check(componentIdent_));
  advance();

  // identifier
  auto optId = parseBindingIdentifier(Param{});

  // Components always require a name identifier
  if (!optId) {
    errorExpected(
        TokenKind::identifier,
        "after 'component'",
        "location of 'component'",
        start);
    return None;
  }

  ESTree::Node *typeParams = nullptr;

  if (check(TokenKind::less)) {
    auto optTypeParams = parseTypeParamsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  if (!need(
          TokenKind::l_paren,
          "at start of component parameter list",
          "component declaration starts here",
          start)) {
    return None;
  }

  ESTree::NodeList paramList;
  ESTree::Node *rest = nullptr;

  if (declare) {
    auto restOpt = parseComponentTypeParametersFlow(Param{}, paramList);
    if (!restOpt)
      return None;
    rest = *restOpt;
  } else {
    if (!parseComponentParametersFlow(Param{}, paramList))
      return None;
  }

  ESTree::Node *rendersType = nullptr;
  if (check(rendersIdent_)) {
    auto optRenders = parseComponentRenderTypeFlow(false);
    if (!optRenders)
      return None;
    rendersType = *optRenders;
  }

  if (declare) {
    if (!eatSemi())
      return None;

    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::DeclareComponentNode(
            *optId, std::move(paramList), rest, typeParams, rendersType));
  }

  if (!need(
          TokenKind::l_brace,
          "in component declaration",
          "start of component declaration",
          start)) {
    return None;
  }

  SaveStrictModeAndSeenDirectives saveStrictModeAndSeenDirectives{this};

  auto parsedBody = parseFunctionBody(
      Param{}, false, false, isAsync, JSLexer::AllowRegExp, true);
  if (!parsedBody)
    return None;
  auto *body = parsedBody.getValue();

  return setLocation(
      start,
      body,
      new (context_) ESTree::ComponentDeclarationNode(
          *optId,
          std::move(paramList),
          body,
          typeParams,
          rendersType,
          isAsync));
}

bool JSParserImpl::parseComponentParametersFlow(
    Param param,
    ESTree::NodeList &paramList) {
  assert(
      check(TokenKind::l_paren) && "ComponentParameters must start with '('");
  // (
  SMLoc lparenLoc = advance().Start;

  while (!check(TokenKind::r_paren)) {
    if (check(TokenKind::dotdotdot)) {
      // BindingRestElement.
      auto optRestElem = parseBindingRestElement(param);
      if (!optRestElem)
        return false;
      paramList.push_back(*optRestElem.getValue());
      checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type);
      break;
    }

    // ComponentParameter.
    auto optParam = parseComponentParameterFlow(param);
    if (!optParam)
      return false;

    paramList.push_back(*optParam.getValue());

    if (!checkAndEat(TokenKind::comma))
      break;
  }

  // )
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of component parameter list",
          "start of component parameter list",
          lparenLoc)) {
    return false;
  }

  return true;
}

Optional<ESTree::Node *> JSParserImpl::parseComponentParameterFlow(
    Param param) {
  // ComponentParameter:
  //   StringLiteral as BindingElement
  //   IdentifierName
  //   IdentifierName as BindingElement

  SMLoc paramStart = tok_->getStartLoc();
  ESTree::Node *nameElem;
  if (check(TokenKind::string_literal)) {
    // StringLiteral as BindingElement
    // ^
    nameElem = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
    advance();

    if (!checkAndEat(asIdent_)) {
      error(
          nameElem->getSourceRange(),
          "string literal names require a local via `as`");
      return None;
    }

    auto optBinding = parseBindingElement(Param{});
    if (!optBinding)
      return None;

    return setLocation(
        paramStart,
        getPrevTokenEndLoc(),
        new (context_)
            ESTree::ComponentParameterNode(nameElem, *optBinding, false));
  }

  if (check(TokenKind::identifier) || tok_->isResWord()) {
    UniqueString *id = tok_->getResWordOrIdentifier();
    SMRange identRng = tok_->getSourceRange();
    TokenKind identKind = tok_->getKind();
    nameElem = setLocation(
        identRng,
        identRng,
        new (context_) ESTree::IdentifierNode(id, nullptr, false));

    advance();
    if (checkAndEat(asIdent_)) {
      // IdentifierName as BindingElement
      //                   ^
      auto optBinding = parseBindingElement(Param{});
      if (!optBinding)
        return None;

      return setLocation(
          paramStart,
          getPrevTokenEndLoc(),
          new (context_)
              ESTree::ComponentParameterNode(nameElem, *optBinding, false));
    }

    if (!validateBindingIdentifier(Param{}, identRng, id, identKind)) {
      error(identRng, "Invalid local name for component");
    }

    ESTree::Node *type = nullptr;
    bool optional = false;

    // IdentifierName?: TypeParam
    //               ^
    if (check(TokenKind::question)) {
      optional = true;
      advance(JSLexer::GrammarContext::Type);
    }

    // IdentifierName?: TypeParam
    //                ^
    if (check(TokenKind::colon)) {
      SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
      auto optType = parseTypeAnnotation(annotStart);
      if (!optType)
        return None;
      type = *optType;
    }

    auto elem = setLocation(
        identRng,
        getPrevTokenEndLoc(),
        new (context_) ESTree::IdentifierNode(id, type, optional));
    ESTree::Node *localElem;

    // IdentifierName?: TypeParam = expr
    //                            ^
    if (check(TokenKind::equal)) {
      auto optInit = parseBindingInitializer(param, elem);
      if (!optInit)
        return None;
      localElem = *optInit;
    } else {
      localElem = elem;
    }

    return setLocation(
        paramStart,
        getPrevTokenEndLoc(),
        new (context_)
            ESTree::ComponentParameterNode(nameElem, localElem, true));
  }

  error(
      tok_->getStartLoc(),
      "identifier or string literal expected in component parameter name");
  return None;
}

Optional<UniqueString *> JSParserImpl::parseRenderTypeOperator() {
  assert(tok_->getResWordOrIdentifier() == rendersIdent_);
  auto typeOperator = rendersIdent_;
  if (tok_->checkFollowingCharacter('?')) {
    SMLoc start = advance(JSLexer::GrammarContext::Type).Start;
    if (!eat(
            TokenKind::question,
            JSLexer::GrammarContext::Type,
            "in render type annotation",
            "start of render type",
            start)) {
      return None;
    }
    typeOperator = rendersMaybeOperator_;
  } else if (tok_->checkFollowingCharacter('*')) {
    SMLoc start = advance(JSLexer::GrammarContext::Type).Start;
    if (!eat(
            TokenKind::star,
            JSLexer::GrammarContext::Type,
            "in render type annotation",
            "start of render type",
            start)) {
      return None;
    }
    typeOperator = rendersStarOperator_;
  } else {
    // Normal renders, but we must still eat the renders token. We don't just
    // eat unconditionally above because the checkFollowingCharacter calls must
    // have the renders ident as the current token, so we can't advance until
    // after those calls
    advance(JSLexer::GrammarContext::Type);
  }

  return typeOperator;
}

Optional<ESTree::Node *> JSParserImpl::parseComponentRenderTypeFlow(
    bool componentType) {
  SMLoc annotStart = tok_->getStartLoc();
  auto optTypeOperator = parseRenderTypeOperator();
  Optional<ESTree::Node *> optBody;
  // This is a weird part of the Flow render syntax design that we should
  // reconsider. Because unions have higher precedence than renders, we
  // parse `component() renders null | number` as
  // `(component() renders null) | number. But with declared components
  // and component declarations we parse the entirety of the RHS of
  // `renders` as a single type, so
  // `component A() renders null | number { ... }` parses the render type
  // as a union of null | number. This was an intentional decision, and
  // prettier will make the discrepancy obvious, but it still feels
  // weird. If we give `renders` higher precedence than unions then this
  // is no longer a problem, but `keyof` has similar syntax and lower
  // precedence than a union type.
  if (componentType) {
    optBody = parsePrefixTypeAnnotationFlow();
  } else {
    optBody = parseTypeAnnotationFlow();
  }
  if (!optBody || !optTypeOperator)
    return None;
  return setLocation(
      annotStart,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TypeOperatorNode(*optTypeOperator, *optBody));
}

Optional<ESTree::Node *> JSParserImpl::parseComponentTypeAnnotationFlow() {
  // component
  assert(check(componentIdent_));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  // identifier
  if (check(TokenKind::identifier)) {
    error(
        tok_->getSourceRange(),
        "component type annotations should not contain a name");
    advance(JSLexer::GrammarContext::Type);
  }

  ESTree::Node *typeParams = nullptr;

  if (check(TokenKind::less)) {
    auto optTypeParams = parseTypeParamsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  if (!need(
          TokenKind::l_paren,
          "at start of component parameter list",
          "component type annotation starts here",
          start)) {
    return None;
  }

  ESTree::NodeList paramList;
  auto restOpt = parseComponentTypeParametersFlow(Param{}, paramList);
  if (!restOpt)
    return None;
  ESTree::Node *rest = *restOpt;

  ESTree::Node *rendersType = nullptr;
  if (check(rendersIdent_)) {
    auto optRenders = parseComponentRenderTypeFlow(true);
    if (!optRenders)
      return None;
    rendersType = *optRenders;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ComponentTypeAnnotationNode(
          std::move(paramList), rest, typeParams, rendersType));
}

Optional<ESTree::Node *> JSParserImpl::parseComponentTypeParametersFlow(
    Param param,
    ESTree::NodeList &paramList) {
  assert(
      check(TokenKind::l_paren) &&
      "ComponentTypeParameters must start with '('");
  // (
  SMLoc lparenLoc = advance(JSLexer::GrammarContext::Type).Start;
  ESTree::Node *rest = nullptr;

  while (!check(TokenKind::r_paren)) {
    if (check(TokenKind::dotdotdot)) {
      // ComponentTypeRestParameter.
      auto optRest = parseComponentTypeRestParameterFlow(param);
      if (!optRest)
        return None;
      rest = *optRest;
      break;
    }

    // ComponentTypeParameter.
    auto optParam = parseComponentTypeParameterFlow(param);
    if (!optParam)
      return None;

    paramList.push_back(*optParam.getValue());

    if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
      break;
  }

  // )
  if (!eat(
          TokenKind::r_paren,
          JSLexer::GrammarContext::Type,
          "at end of component type parameter list",
          "start of component type parameter list",
          lparenLoc)) {
    return None;
  }

  return rest;
}

Optional<ESTree::Node *> JSParserImpl::parseComponentTypeRestParameterFlow(
    Param param) {
  // ComponentTypeRestParameter:
  //   ...IdentifierName: TypeParam
  //   ...IdentifierName?: TypeParam
  //   ...TypeParam

  assert(check(TokenKind::dotdotdot));

  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  auto optLeft = parseTypeAnnotationBeforeColonFlow();
  if (!optLeft)
    return None;

  ESTree::Node *name = nullptr;
  ESTree::Node *typeAnnotation = nullptr;
  bool optional = false;

  if (check(TokenKind::colon, TokenKind::question)) {
    // The node is actually supposed to be an identifier, not a TypeAnnotation.
    auto optName = reparseTypeAnnotationAsIdentifierFlow(*optLeft);
    if (!optName)
      return None;
    name = *optName;
    optional = checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type);
    if (!eat(
            TokenKind::colon,
            JSLexer::GrammarContext::Type,
            "in component parameter type annotation",
            "start of parameter",
            start))
      return None;
    auto optType = parseTypeAnnotationFlow();
    if (!optType)
      return None;
    typeAnnotation = *optType;
  } else {
    typeAnnotation = *optLeft;
  }

  checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type);

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::ComponentTypeParameterNode(name, typeAnnotation, optional));
}

Optional<ESTree::Node *> JSParserImpl::parseComponentTypeParameterFlow(
    Param param) {
  // ComponentTypeParameter:
  //   StringLiteral?: TypeParam
  //   IdentifierName?: TypeParam

  SMLoc paramStart = tok_->getStartLoc();
  ESTree::Node *nameElem;
  if (check(TokenKind::string_literal)) {
    // StringLiteral: TypeParam
    // ^
    nameElem = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
    advance(JSLexer::GrammarContext::Type);
  } else if (check(TokenKind::identifier) || tok_->isResWord()) {
    // IdentifierName: TypeParam
    // ^
    nameElem = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(
            tok_->getResWordOrIdentifier(), nullptr, false));
    advance(JSLexer::GrammarContext::Type);
  } else {
    error(
        tok_->getStartLoc(),
        "identifier or string literal expected in component type parameter name");
    return None;
  }

  if (check(asIdent_)) {
    error(tok_->getStartLoc(), "'as' not allowed in component type parameter");
    return None;
  }

  bool optional = false;

  // Name?: TypeParam
  //     ^
  if (checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type)) {
    optional = true;
  }

  // Name?: TypeParam
  //      ^
  if (!eat(
          TokenKind::colon,
          JSLexer::GrammarContext::Type,
          "in component type parameter",
          "start of parameter",
          paramStart))
    return None;

  // Name?: TypeParam
  //        ^
  auto optType = parseTypeAnnotation();
  if (!optType)
    return None;

  return setLocation(
      paramStart,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::ComponentTypeParameterNode(nameElem, *optType, optional));
}

bool JSParserImpl::checkHookDeclarationFlow() {
  if (!check(hookIdent_))
    return false;

  // Don't pass an `expectedToken` so we don't advance on a match. This allows
  // `parseHookDeclarationFlow` to reparse the token and store useful
  // information. Additionally to be used within `checkDeclaration` this
  // function must be idempotent.
  OptValue<TokenKind> optNext = lexer_.lookahead1(None);
  return optNext.hasValue() && *optNext == TokenKind::identifier;
}

Optional<ESTree::Node *> JSParserImpl::parseHookDeclarationFlow(
    SMLoc start,
    bool isAsync) {
  // hook
  assert(check(hookIdent_));
  advance();

  // identifier
  auto optId = parseBindingIdentifier(Param{});

  // Hooks always require a name identifier
  if (!optId) {
    errorExpected(
        TokenKind::identifier, "after 'hook'", "location of 'hook'", start);
    return None;
  }

  ESTree::Node *typeParams = nullptr;

  if (check(TokenKind::less)) {
    auto optTypeParams = parseTypeParamsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  if (!need(
          TokenKind::l_paren,
          "at start of hook parameter list",
          "hook declaration starts here",
          start)) {
    return None;
  }

  ESTree::NodeList paramList;

  llvh::SaveAndRestore<bool> saveParamAwait(paramAwait_, isAsync);

  if (!parseFormalParameters(Param{}, paramList))
    return None;

  ESTree::Node *returnType = nullptr;

  if (check(TokenKind::colon)) {
    SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
    // %checks predicates are unsupported in hooks.
    if (!check(checksIdent_)) {
      auto optRet = parseReturnTypeAnnotationFlow(annotStart);
      if (!optRet)
        return None;
      returnType = *optRet;
    } else {
      error(tok_->getStartLoc(), "checks predicates unsupported with hooks");
      return None;
    }
  }

  if (!need(
          TokenKind::l_brace,
          "in hook declaration",
          "start of hook declaration",
          start)) {
    return None;
  }

  SaveStrictModeAndSeenDirectives saveStrictModeAndSeenDirectives{this};

  auto parsedBody = parseFunctionBody(
      Param{}, false, false, isAsync, JSLexer::AllowRegExp, true);
  if (!parsedBody)
    return None;
  auto *body = parsedBody.getValue();

  return setLocation(
      start,
      body,
      new (context_) ESTree::HookDeclarationNode(
          *optId, std::move(paramList), body, typeParams, returnType, isAsync));
}

bool JSParserImpl::checkMaybeFlowMatchSlowPath() {
  assert(check(matchIdent_));
  OptValue<TokenKind> optNext = lexer_.lookahead1(None);
  return optNext.hasValue() && *optNext == TokenKind::l_paren;
}

ESTree::Node *JSParserImpl::reparseArgumentsAsMatchArgumentFlow(
    SMRange range,
    ESTree::NodeList &&argList) {
  if (argList.empty()) {
    error(range, "'match' argument must not be empty");
  }
  for (const ESTree::Node &arg : argList) {
    if (isa<ESTree::SpreadElementNode>(arg)) {
      error(
          arg.getSourceRange(),
          "'match' argument cannot contain spread elements");
    }
  }
  if (argList.size() == 1) {
    auto expr = &argList.front();
    argList.clear();
    return expr;
  }
  return setLocation(
      range,
      range,
      new (context_) ESTree::SequenceExpressionNode(std::move(argList)));
}

Optional<ESTree::Node *> JSParserImpl::tryParseMatchStatementFlow(Param param) {
  SMLoc startLoc;
  SMLoc argsStartLoc;
  SMLoc argsEndLoc;
  ESTree::NodeList argList;
  {
    // This save point is required because Flow supports both match statements
    // and match expressions, and we do not reserve `match` as a keyword. If
    // either of those points change in the future, this could be removed - see
    // blame for discussion.
    // We don't need to suppress errors as the only place that could error
    // in this section is `parseArguments`, and it would equivalently error
    // if this was parsed as an expression.
    JSLexer::SavePoint savePoint{&lexer_};

    // Checked already by `checkMaybeFlowMatch`
    assert(check(matchIdent_));
    startLoc = advance().Start;
    // Checked already by `checkMaybeFlowMatch`
    assert(!lexer_.isNewLineBeforeCurrentToken());

    argsStartLoc = tok_->getStartLoc();
    if (!parseArguments(argList, argsEndLoc))
      return None;

    if (lexer_.isNewLineBeforeCurrentToken() || !check(TokenKind::l_brace)) {
      // This is not a match statement.
      savePoint.restore();
      return nullptr;
    }
  }
  // We are unambiguously parsing a match statement now.

  auto arg = reparseArgumentsAsMatchArgumentFlow(
      SMRange{argsStartLoc, argsEndLoc}, std::move(argList));

  assert(check(TokenKind::l_brace));
  SMLoc lbraceLoc = advance().Start;

  ESTree::NodeList cases;

  while (!check(TokenKind::r_brace)) {
    SMLoc caseStartLoc = tok_->getStartLoc();

    auto optPattern = parseMatchPatternFlow();
    if (!optPattern)
      return None;

    ESTree::Node *guard = nullptr;
    if (checkAndEat(TokenKind::rw_if)) {
      auto optGuard = parseExpression(ParamIn, CoverTypedParameters::No);
      if (!optGuard)
        return None;
      guard = optGuard.getValue();
    }

    if (!eat(
            TokenKind::colon,
            JSLexer::AllowRegExp,
            "after match pattern",
            "location of pattern",
            caseStartLoc))
      return None;

    auto optBody = parseBlock(param.get(ParamReturn));
    if (!optBody)
      return None;

    cases.push_back(*setLocation(
        caseStartLoc,
        *optBody,
        new (context_) ESTree::MatchStatementCaseNode(
            optPattern.getValue(), optBody.getValue(), guard)));

    if (check(TokenKind::comma, TokenKind::semi)) {
      advance();
    }
  }

  SMLoc endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_brace,
          JSLexer::AllowRegExp,
          "at end of 'match' statement",
          "location of '{'",
          lbraceLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::MatchStatementNode(arg, std::move(cases)));
}

Optional<ESTree::Node *> JSParserImpl::parseMatchCallOrMatchExpressionFlow() {
  SMLoc startLoc = tok_->getStartLoc();
  ESTree::Node *matchIdentNode = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance();

  auto parenLoc = tok_->getStartLoc();
  ESTree::NodeList argList;
  SMLoc endLoc;

  if (!parseArguments(argList, endLoc))
    return None;

  if (!lexer_.isNewLineBeforeCurrentToken() && check(TokenKind::l_brace)) {
    auto arg = reparseArgumentsAsMatchArgumentFlow(
        SMRange{parenLoc, endLoc}, std::move(argList));
    return parseMatchExpressionFlow(startLoc, arg);
  }

  auto callNode = setLocation(
      startLoc,
      endLoc,
      parenLoc,
      new (context_) ESTree::CallExpressionNode(
          matchIdentNode, nullptr, std::move(argList)));
  auto optExpr = parseOptionalExpressionExceptNew_tail(
      IsConstructorCall::No, startLoc, callNode);
  if (!optExpr) {
    return None;
  }
  return parseLeftHandSideExpressionTail(startLoc, optExpr.getValue());
}

Optional<ESTree::Node *> JSParserImpl::parseMatchExpressionFlow(
    SMLoc startLoc,
    ESTree::Node *argument) {
  assert(check(TokenKind::l_brace));
  SMLoc lbraceLoc = advance().Start;

  ESTree::NodeList cases;

  while (!check(TokenKind::r_brace)) {
    SMLoc caseStartLoc = tok_->getStartLoc();

    auto optPattern = parseMatchPatternFlow();
    if (!optPattern)
      return None;

    ESTree::Node *guard = nullptr;
    if (checkAndEat(TokenKind::rw_if)) {
      auto optGuard = parseExpression(ParamIn, CoverTypedParameters::No);
      if (!optGuard)
        return None;
      guard = optGuard.getValue();
    }

    if (!eat(
            TokenKind::colon,
            JSLexer::AllowRegExp,
            "after match pattern",
            "location of pattern",
            caseStartLoc))
      return None;

    auto optBody = parseAssignmentExpression();
    if (!optBody)
      return None;

    cases.push_back(*setLocation(
        caseStartLoc,
        *optBody,
        new (context_) ESTree::MatchExpressionCaseNode(
            optPattern.getValue(), optBody.getValue(), guard)));

    if (!(checkAndEat(TokenKind::comma) || checkAndEat(TokenKind::semi)))
      break;
  }

  SMLoc endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_brace,
          JSLexer::AllowRegExp,
          "at end of 'match' expression",
          "location of '{'",
          lbraceLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::MatchExpressionNode(argument, std::move(cases)));
}

Optional<ESTree::Node *> JSParserImpl::parseMatchPatternFlow() {
  SMLoc startLoc = tok_->getStartLoc();
  auto optFirstPattern = parseMatchSubpatternFlow();
  if (!optFirstPattern)
    return None;
  ESTree::Node *pattern = optFirstPattern.getValue();
  if (check(TokenKind::pipe)) {
    ESTree::NodeList patterns{};
    patterns.push_back(*optFirstPattern.getValue());
    while (checkAndEat(TokenKind::pipe)) {
      auto optPattern = parseMatchSubpatternFlow();
      if (!optPattern)
        return None;
      patterns.push_back(*optPattern.getValue());
    }
    pattern = setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::MatchOrPatternNode(std::move(patterns)));
  }
  if (checkAndEat(asIdent_)) {
    ESTree::Node *target = nullptr;
    if (checkN(TokenKind::rw_const, TokenKind::rw_var, letIdent_)) {
      auto optTarget = parseMatchBindingPatternFlow();
      if (!optTarget)
        return None;
      target = optTarget.getValue();
    } else if (check(TokenKind::identifier) || tok_->isResWord()) {
      auto optTarget = parseMatchBindingIdentifierFlow();
      if (!optTarget)
        return None;
      target = optTarget.getValue();
    } else {
      error(tok_->getSourceRange(), "expected identifier or binding pattern");
      return None;
    }
    pattern = setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::MatchAsPatternNode(pattern, target));
  }
  return pattern;
}

Optional<ESTree::Node *> JSParserImpl::parseMatchSubpatternFlow() {
  switch (tok_->getKind()) {
    case TokenKind::rw_null: {
      auto *lit =
          setLocation(tok_, tok_, new (context_) ESTree::NullLiteralNode());
      auto *pat = setLocation(
          tok_, tok_, new (context_) ESTree::MatchLiteralPatternNode(lit));
      advance(JSLexer::AllowDiv);
      return pat;
    }

    case TokenKind::rw_true:
    case TokenKind::rw_false: {
      auto *lit = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::BooleanLiteralNode(
              tok_->getKind() == TokenKind::rw_true));
      auto *pat = setLocation(
          tok_, tok_, new (context_) ESTree::MatchLiteralPatternNode(lit));
      advance(JSLexer::AllowDiv);
      return pat;
    }

    case TokenKind::numeric_literal: {
      auto *lit = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::NumericLiteralNode(tok_->getNumericLiteral()));
      auto *pat = setLocation(
          tok_, tok_, new (context_) ESTree::MatchLiteralPatternNode(lit));
      advance(JSLexer::AllowDiv);
      return pat;
    }

    case TokenKind::bigint_literal: {
      auto *lit = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::BigIntLiteralNode(tok_->getBigIntLiteral()));
      auto *pat = setLocation(
          tok_, tok_, new (context_) ESTree::MatchLiteralPatternNode(lit));
      advance(JSLexer::AllowDiv);
      return pat;
    }

    case TokenKind::string_literal: {
      auto *lit = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
      auto *pat = setLocation(
          tok_, tok_, new (context_) ESTree::MatchLiteralPatternNode(lit));
      advance(JSLexer::AllowDiv);
      return pat;
    }

    case TokenKind::identifier: {
      if (check(underscoreIdent_)) {
        auto *pat = setLocation(
            tok_, tok_, new (context_) ESTree::MatchWildcardPatternNode());
        advance(JSLexer::AllowDiv);
        return pat;
      }
      if (check(letIdent_)) {
        auto pat = parseMatchBindingPatternFlow();
        if (!pat)
          return None;
        return pat.getValue();
      }
      SMLoc start_loc = tok_->getStartLoc();
      auto *ident = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
      ESTree::Node *pat = setLocation(
          tok_, tok_, new (context_) ESTree::MatchIdentifierPatternNode(ident));
      advance(JSLexer::AllowDiv);

      while (check(TokenKind::period, TokenKind::l_square)) {
        if (checkAndEat(TokenKind::period)) {
          if (!need(
                  TokenKind::identifier,
                  "in match member pattern",
                  nullptr,
                  {}))
            return None;
          auto *property = setLocation(
              tok_,
              tok_,
              new (context_) ESTree::IdentifierNode(
                  tok_->getIdentifier(), nullptr, false));
          advance(JSLexer::AllowDiv);
          pat = setLocation(
              start_loc,
              getPrevTokenEndLoc(),
              new (context_) ESTree::MatchMemberPatternNode(pat, property));
        } else {
          SMLoc computedStartLoc = advance().Start; // Eat `[`
          ESTree::Node *property = nullptr;
          switch (tok_->getKind()) {
            case TokenKind::numeric_literal: {
              property = setLocation(
                  tok_,
                  tok_,
                  new (context_)
                      ESTree::NumericLiteralNode(tok_->getNumericLiteral()));
              advance(JSLexer::AllowDiv);
              break;
            }
            case TokenKind::bigint_literal: {
              property = setLocation(
                  tok_,
                  tok_,
                  new (context_)
                      ESTree::BigIntLiteralNode(tok_->getBigIntLiteral()));
              advance(JSLexer::AllowDiv);
              break;
            }
            case TokenKind::string_literal: {
              property = setLocation(
                  tok_,
                  tok_,
                  new (context_)
                      ESTree::StringLiteralNode(tok_->getStringLiteral()));
              advance(JSLexer::AllowDiv);
              break;
            }
            default: {
              errorExpected(
                  {TokenKind::numeric_literal,
                   TokenKind::bigint_literal,
                   TokenKind::string_literal},
                  "in match member pattern computed property",
                  "start of computed property",
                  computedStartLoc);
              return None;
            }
          }
          if (!eat(
                  TokenKind::r_square,
                  JSLexer::AllowDiv,
                  "at end of computed member property",
                  "location of '['",
                  computedStartLoc))
            return None;
          pat = setLocation(
              start_loc,
              getPrevTokenEndLoc(),
              new (context_) ESTree::MatchMemberPatternNode(pat, property));
        }
      }
      return pat;
    }

    case TokenKind::plus:
    case TokenKind::minus: {
      UniqueString *op = getTokenIdent(tok_->getKind());
      SMLoc startLoc = advance().Start;

      ESTree::Node *argument;
      switch (tok_->getKind()) {
        case TokenKind::numeric_literal: {
          argument = setLocation(
              tok_,
              tok_,
              new (context_)
                  ESTree::NumericLiteralNode(tok_->getNumericLiteral()));
          advance(JSLexer::AllowDiv);
          break;
        }
        case TokenKind::bigint_literal: {
          argument = setLocation(
              tok_,
              tok_,
              new (context_)
                  ESTree::BigIntLiteralNode(tok_->getBigIntLiteral()));
          advance(JSLexer::AllowDiv);
          break;
        }
        default:
          error(tok_->getStartLoc(), "invalid match unary pattern argument");
          return None;
      }
      return setLocation(
          startLoc,
          getPrevTokenEndLoc(),
          new (context_) ESTree::MatchUnaryPatternNode(argument, op));
    }

    case TokenKind::rw_const:
    case TokenKind::rw_var: {
      auto pat = parseMatchBindingPatternFlow();
      if (!pat)
        return None;
      return pat.getValue();
    }

    case TokenKind::l_paren: {
      SMLoc startLoc = advance().Start;
      auto optPattern = parseMatchPatternFlow();
      if (!optPattern)
        return None;

      if (!eat(
              TokenKind::r_paren,
              JSLexer::AllowDiv,
              "at end of a match pattern group",
              "location of '('",
              startLoc))
        return None;

      return optPattern.getValue();
    }

    case TokenKind::l_brace:
      return parseMatchObjectPatternFlow();

    case TokenKind::l_square:
      return parseMatchArrayPatternFlow();

    default:
      error(tok_->getStartLoc(), "invalid match pattern");
      return None;
  }
}

Optional<ESTree::IdentifierNode *>
JSParserImpl::parseMatchBindingIdentifierFlow() {
  UniqueString *id = tok_->getResWordOrIdentifier();
  TokenKind kind = tok_->getKind();
  if (!validateBindingIdentifier(Param{}, tok_->getSourceRange(), id, kind))
    return None;
  advance();
  return setLocation(
      tok_, tok_, new (context_) ESTree::IdentifierNode(id, nullptr, false));
}

Optional<ESTree::MatchBindingPatternNode *>
JSParserImpl::parseMatchBindingPatternFlow() {
  assert(checkN(TokenKind::rw_const, TokenKind::rw_var, letIdent_));
  auto kind = tok_->getResWordOrIdentifier();
  SMLoc startLoc = advance().Start;
  if (!check(TokenKind::identifier) && !tok_->isResWord()) {
    errorExpected(
        TokenKind::identifier,
        "in match binding pattern",
        "start of binding pattern",
        startLoc);
  }
  auto optIdent = parseMatchBindingIdentifierFlow();
  if (!optIdent)
    return None;
  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::MatchBindingPatternNode(optIdent.getValue(), kind));
}

Optional<ESTree::Node *> JSParserImpl::parseMatchRestPatternFlow() {
  assert(check(TokenKind::dotdotdot));
  SMLoc restStartLoc = advance().Start;
  ESTree::Node *arg = nullptr;
  if (checkN(TokenKind::rw_const, TokenKind::rw_var, letIdent_)) {
    auto optArg = parseMatchBindingPatternFlow();
    if (!optArg)
      return None;
    arg = optArg.getValue();
  }
  return setLocation(
      restStartLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::MatchRestPatternNode(arg));
}

Optional<ESTree::Node *> JSParserImpl::parseMatchObjectPatternFlow() {
  assert(check(TokenKind::l_brace));
  auto startLoc = advance().Start;
  ESTree::NodeList properties{};
  ESTree::Node *rest = nullptr;

  while (!check(TokenKind::r_brace)) {
    if (check(TokenKind::dotdotdot)) {
      auto optRest = parseMatchRestPatternFlow();
      if (!optRest) {
        return None;
      }
      rest = optRest.getValue();
      break;
    }

    ESTree::Node *prop = nullptr;
    auto propStartLoc = tok_->getStartLoc();
    if (checkN(TokenKind::rw_const, TokenKind::rw_var, letIdent_)) {
      // Shorthand syntax: e.g. `const x` is shorthand for `x: const x`
      auto optBindingPattern = parseMatchBindingPatternFlow();
      if (!optBindingPattern)
        return None;
      auto bindingPattern = optBindingPattern.getValue();
      prop = setLocation(
          propStartLoc,
          getPrevTokenEndLoc(),
          new (context_) ESTree::MatchObjectPatternPropertyNode(
              bindingPattern->_id, bindingPattern, true));
    } else {
      // Normal property
      ESTree::Node *key = nullptr;
      // Property key parsing
      switch (tok_->getKind()) {
        case TokenKind::identifier: {
          key = setLocation(
              tok_,
              tok_,
              new (context_) ESTree::IdentifierNode(
                  tok_->getIdentifier(), nullptr, false));
          advance(JSLexer::AllowDiv);
          break;
        }
        case TokenKind::string_literal: {
          key = setLocation(
              tok_,
              tok_,
              new (context_)
                  ESTree::StringLiteralNode(tok_->getStringLiteral()));
          advance(JSLexer::AllowDiv);
          break;
        }
        case TokenKind::numeric_literal: {
          key = setLocation(
              tok_,
              tok_,
              new (context_)
                  ESTree::NumericLiteralNode(tok_->getNumericLiteral()));
          advance(JSLexer::AllowDiv);
          break;
        }
        case TokenKind::bigint_literal: {
          key = setLocation(
              tok_,
              tok_,
              new (context_)
                  ESTree::BigIntLiteralNode(tok_->getBigIntLiteral()));
          advance(JSLexer::AllowDiv);
          break;
        }
        default: {
          errorExpected(
              {TokenKind::identifier,
               TokenKind::string_literal,
               TokenKind::numeric_literal,
               TokenKind::bigint_literal},
              "in match object pattern property key",
              "start of match object pattern property key",
              propStartLoc);
          return None;
        }
      }
      if (!eat(
              TokenKind::colon,
              JSLexer::AllowRegExp,
              "in match object pattern property",
              "start of match object pattern property",
              propStartLoc))
        return None;
      auto optPattern = parseMatchPatternFlow();
      prop = setLocation(
          propStartLoc,
          getPrevTokenEndLoc(),
          new (context_) ESTree::MatchObjectPatternPropertyNode(
              key, optPattern.getValue(), false));
    }
    properties.push_back(*prop);
    if (!checkAndEat(TokenKind::comma))
      break;
  }
  if (!eat(
          TokenKind::r_brace,
          JSLexer::AllowDiv,
          "at end of object match pattern",
          "location of '{'",
          startLoc))
    return None;

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::MatchObjectPatternNode(std::move(properties), rest));
}

Optional<ESTree::Node *> JSParserImpl::parseMatchArrayPatternFlow() {
  assert(check(TokenKind::l_square));
  auto startLoc = advance().Start;
  ESTree::NodeList elements{};
  ESTree::Node *rest = nullptr;

  while (!check(TokenKind::r_square)) {
    if (check(TokenKind::dotdotdot)) {
      auto optRest = parseMatchRestPatternFlow();
      if (!optRest) {
        return None;
      }
      rest = optRest.getValue();
      break;
    }

    auto optPattern = parseMatchPatternFlow();
    if (!optPattern)
      return None;
    elements.push_back(*optPattern.getValue());
    if (!checkAndEat(TokenKind::comma))
      break;
  }
  if (!eat(
          TokenKind::r_square,
          JSLexer::AllowDiv,
          "at end of array match pattern",
          "location of '['",
          startLoc))
    return None;

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::MatchArrayPatternNode(std::move(elements), rest));
}

Optional<ESTree::Node *> JSParserImpl::parseTypeAliasFlow(
    SMLoc start,
    TypeAliasKind kind) {
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
    auto optTypeParams = parseTypeParamsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  ESTree::Node *supertype = nullptr;
  if ((kind == TypeAliasKind::Opaque || kind == TypeAliasKind::DeclareOpaque) &&
      checkAndEat(TokenKind::colon, JSLexer::GrammarContext::Type)) {
    auto optSuper = parseTypeAnnotationFlow();
    if (!optSuper)
      return None;
    supertype = *optSuper;
  }

  ESTree::Node *right = nullptr;
  if (kind != TypeAliasKind::DeclareOpaque) {
    if (!eat(
            TokenKind::equal,
            JSLexer::GrammarContext::Type,
            "in type alias",
            "start of type alias",
            start))
      return None;

    auto optRight = parseTypeAnnotationFlow();
    if (!optRight)
      return None;
    right = *optRight;
  }

  if (!eatSemi())
    return None;

  if (kind == TypeAliasKind::DeclareOpaque) {
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_)
            ESTree::DeclareOpaqueTypeNode(id, typeParams, right, supertype));
  }
  if (kind == TypeAliasKind::Declare) {
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::DeclareTypeAliasNode(id, typeParams, right));
  }
  if (kind == TypeAliasKind::Opaque) {
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_)
            ESTree::OpaqueTypeNode(id, typeParams, right, supertype));
  }
  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TypeAliasNode(id, typeParams, right));
}

Optional<ESTree::Node *> JSParserImpl::parseInterfaceDeclarationFlow(
    Optional<SMLoc> declareStart) {
  assert(checkN(TokenKind::rw_interface, interfaceIdent_));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  if (!need(
          TokenKind::identifier,
          "in interface declaration",
          "start of interface",
          start))
    return None;

  auto *id = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optParams = parseTypeParamsFlow();
    if (!optParams)
      return None;
    typeParams = *optParams;
  }

  ESTree::NodeList extends{};

  auto optBody = parseInterfaceTailFlow(start, extends);
  if (!optBody)
    return None;

  if (declareStart.hasValue()) {
    return setLocation(
        *declareStart,
        *optBody,
        new (context_) ESTree::DeclareInterfaceNode(
            id, typeParams, std::move(extends), *optBody));
  }
  return setLocation(
      start,
      *optBody,
      new (context_) ESTree::InterfaceDeclarationNode(
          id, typeParams, std::move(extends), *optBody));
}

Optional<ESTree::Node *> JSParserImpl::parseInterfaceTailFlow(
    SMLoc start,
    ESTree::NodeList &extends) {
  if (checkAndEat(TokenKind::rw_extends)) {
    do {
      if (!need(
              TokenKind::identifier,
              "in extends clause",
              "location of interface",
              start))
        return None;
      if (!parseInterfaceExtends(start, extends))
        return None;
    } while (checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type));
  }

  if (!need(TokenKind::l_brace, "in interface", "location of interface", start))
    return None;

  return parseObjectTypeAnnotationFlow(
      AllowProtoProperty::No, AllowStaticProperty::No, AllowSpreadProperty::No);
}

bool JSParserImpl::parseInterfaceExtends(
    SMLoc start,
    ESTree::NodeList &extends) {
  assert(check(TokenKind::identifier));
  auto optGeneric = parseGenericTypeFlow();
  if (!optGeneric)
    return false;
  ESTree::GenericTypeAnnotationNode *generic = *optGeneric;
  extends.push_back(*setLocation(
      generic,
      generic,
      new (context_) ESTree::InterfaceExtendsNode(
          generic->_id, generic->_typeParameters)));
  return true;
}

Optional<ESTree::Node *> JSParserImpl::parseDeclareFunctionOrHookFlow(
    SMLoc start,
    bool hook) {
  advance(JSLexer::GrammarContext::Type);

  if (!need(
          TokenKind::identifier,
          "in declare function type",
          "location of declare",
          start))
    return None;

  UniqueString *id = tok_->getIdentifier();
  SMLoc idStart = advance(JSLexer::GrammarContext::Type).Start;

  SMLoc funcStart = tok_->getStartLoc();

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optParams = parseTypeParamsFlow();
    if (!optParams)
      return None;
    typeParams = *optParams;
  }

  if (!need(
          TokenKind::l_paren,
          "in declare function type",
          "location of declare",
          start))
    return None;

  ESTree::NodeList params{};
  ESTree::Node *thisConstraint = nullptr;
  auto optRest =
      parseFunctionTypeAnnotationParamsFlow(params, thisConstraint, hook);
  if (!optRest)
    return None;

  if (!eat(
          TokenKind::colon,
          JSLexer::GrammarContext::Type,
          "in declare function type",
          "location of declare",
          start))
    return None;

  auto optReturn = parseReturnTypeAnnotationFlow();
  if (!optReturn)
    return None;
  ESTree::Node *returnType = *optReturn;
  SMLoc funcEnd = getPrevTokenEndLoc();

  ESTree::Node *predicate = nullptr;
  if (check(checksIdent_) && !hook) {
    auto optPred = parsePredicateFlow();
    if (!optPred)
      return None;
    predicate = *optPred;
  }

  if (!eatSemi())
    return None;

  if (!hook) {
    auto *func = setLocation(
        funcStart,
        funcEnd,
        new (context_) ESTree::TypeAnnotationNode(setLocation(
            funcStart,
            funcEnd,
            new (context_) ESTree::FunctionTypeAnnotationNode(
                std::move(params),
                thisConstraint,
                returnType,
                *optRest,
                typeParams))));
    auto *ident = setLocation(
        idStart, func, new (context_) ESTree::IdentifierNode(id, func, false));
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::DeclareFunctionNode(ident, predicate));
  } else {
    auto *func = setLocation(
        funcStart,
        funcEnd,
        new (context_) ESTree::TypeAnnotationNode(setLocation(
            funcStart,
            funcEnd,
            new (context_) ESTree::HookTypeAnnotationNode(
                std::move(params), returnType, *optRest, typeParams))));
    auto *ident = setLocation(
        idStart, func, new (context_) ESTree::IdentifierNode(id, func, false));
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::DeclareHookNode(ident));
  }
}

Optional<ESTree::Node *> JSParserImpl::parseDeclareFunctionFlow(SMLoc start) {
  assert(check(TokenKind::rw_function));
  return parseDeclareFunctionOrHookFlow(start, false);
}

Optional<ESTree::Node *> JSParserImpl::parseDeclareHookFlow(SMLoc start) {
  assert(check(hookIdent_));
  return parseDeclareFunctionOrHookFlow(start, true);
}

Optional<ESTree::Node *> JSParserImpl::parseDeclareModuleFlow(SMLoc start) {
  assert(check(moduleIdent_));
  advance(JSLexer::GrammarContext::Type);

  if (checkAndEat(TokenKind::period, JSLexer::GrammarContext::Type)) {
    if (!checkAndEat(exportsIdent_, JSLexer::GrammarContext::Type)) {
      error(tok_->getSourceRange(), "expected module.exports declaration");
      return None;
    }

    SMLoc annotStart = tok_->getStartLoc();
    if (!eat(
            TokenKind::colon,
            JSLexer::GrammarContext::Type,
            "in module.exports declaration",
            "start of declaration",
            start))
      return None;
    auto optType = parseTypeAnnotationFlow(annotStart);
    if (!optType)
      return None;
    eatSemi(true);
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::DeclareModuleExportsNode(*optType));
  }

  // declare module Identifier {[opt]
  //                ^
  ESTree::Node *id = nullptr;
  if (check(TokenKind::string_literal)) {
    id = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
  } else {
    if (!need(
            TokenKind::identifier,
            "in module declaration",
            "start of declaration",
            start))
      return None;
    id = setLocation(
        tok_,
        tok_,
        new (context_)
            ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  }
  advance(JSLexer::GrammarContext::Type);

  // declare module Identifier {
  //                           ^
  SMLoc bodyStart = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_brace,
          JSLexer::GrammarContext::Type,
          "in module declaration",
          "start of declaration",
          start))
    return None;

  ESTree::NodeList declarations{};

  while (!check(TokenKind::r_brace)) {
    if (!parseStatementListItem(Param{}, AllowImportExport::Yes, declarations))
      return None;
  }

  SMLoc bodyEnd = advance(JSLexer::GrammarContext::Type).End;

  ESTree::Node *body = setLocation(
      bodyStart,
      bodyEnd,
      new (context_) ESTree::BlockStatementNode(std::move(declarations)));

  return setLocation(
      start, body, new (context_) ESTree::DeclareModuleNode(id, body));
}

Optional<ESTree::Node *> JSParserImpl::parseDeclareNamespaceFlow(SMLoc start) {
  assert(check(namespaceIdent_));
  advance(JSLexer::GrammarContext::Type);

  // declare namespace Identifier {[opt]
  //                   ^
  if (!need(
          TokenKind::identifier,
          "in namespace declaration",
          "start of declaration",
          start))
    return None;
  ESTree::Node *id = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  // declare namespace Identifier {
  //                              ^
  SMLoc bodyStart = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_brace,
          JSLexer::GrammarContext::Type,
          "in namespace declaration",
          "start of declaration",
          start))
    return None;

  ESTree::NodeList declarations{};

  while (!check(TokenKind::r_brace)) {
    if (!parseStatementListItem(Param{}, AllowImportExport::Yes, declarations))
      return None;
  }

  SMLoc bodyEnd = advance(JSLexer::GrammarContext::Type).End;

  ESTree::Node *body = setLocation(
      bodyStart,
      bodyEnd,
      new (context_) ESTree::BlockStatementNode(std::move(declarations)));

  return setLocation(
      start, body, new (context_) ESTree::DeclareNamespaceNode(id, body));
}

Optional<ESTree::Node *> JSParserImpl::parseDeclareClassFlow(SMLoc start) {
  assert(check(TokenKind::rw_class));
  advance(JSLexer::GrammarContext::Type);

  // NOTE: Class definition is always strict mode code.
  SaveStrictModeAndSeenDirectives saveStrictMode{this};
  setStrictMode(true);

  if (!need(
          TokenKind::identifier,
          "in class declaration",
          "start of declaration",
          start))
    return None;

  auto *id = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optParams = parseTypeParamsFlow();
    if (!optParams)
      return None;
    typeParams = *optParams;
  }

  ESTree::NodeList extends{};
  if (checkAndEat(TokenKind::rw_extends)) {
    if (!need(
            TokenKind::identifier,
            "in class 'extends'",
            "start of declaration",
            start))
      return None;
    if (!parseInterfaceExtends(start, extends))
      return None;
  }

  ESTree::NodeList mixins{};
  if (checkAndEat(mixinsIdent_)) {
    do {
      if (!need(
              TokenKind::identifier,
              "in class 'mixins'",
              "start of declaration",
              start))
        return None;
      if (!parseInterfaceExtends(start, mixins))
        return None;
    } while (checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type));
  }

  ESTree::NodeList implements{};
  if (checkAndEat(TokenKind::rw_implements)) {
    do {
      if (!need(
              TokenKind::identifier,
              "in class 'implements'",
              "start of declaration",
              start))
        return None;
      auto optImpl = parseClassImplementsFlow();
      if (!optImpl)
        return None;
      implements.push_back(**optImpl);
    } while (checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type));
  }

  if (!need(
          TokenKind::l_brace,
          "in declared class",
          "start of declaration",
          start))
    return None;

  auto optBody = parseObjectTypeAnnotationFlow(
      AllowProtoProperty::Yes,
      AllowStaticProperty::Yes,
      AllowSpreadProperty::No);
  if (!optBody)
    return None;

  return setLocation(
      start,
      *optBody,
      new (context_) ESTree::DeclareClassNode(
          id,
          typeParams,
          std::move(extends),
          std::move(implements),
          std::move(mixins),
          *optBody));
}

Optional<ESTree::Node *> JSParserImpl::parseExportTypeDeclarationFlow(
    SMLoc startLoc) {
  assert(check(typeIdent_));
  SMLoc typeIdentLoc = advance().Start;

  if (checkAndEat(TokenKind::star)) {
    // export type * FromClause;
    //               ^
    auto optFromClause = parseFromClause();
    if (!optFromClause) {
      return None;
    }
    if (!eatSemi()) {
      return None;
    }
    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_)
            ESTree::ExportAllDeclarationNode(*optFromClause, typeIdent_));
  }

  if (check(TokenKind::l_brace)) {
    ESTree::NodeList specifiers{};
    llvh::SmallVector<SMRange, 2> invalids{};

    auto optExportClause = parseExportClause(specifiers, invalids);
    if (!optExportClause) {
      return None;
    }

    ESTree::Node *source = nullptr;
    if (check(fromIdent_)) {
      // export ExportClause FromClause ;
      auto optFromClause = parseFromClause();
      if (!optFromClause) {
        return None;
      }
      source = *optFromClause;
    } else {
      // export ExportClause ;
      // ES9.0 15.2.3.1
      // When there is no FromClause, any ranges added to invalids are
      // actually invalid, and should be reported as errors.
      for (const SMRange &range : invalids) {
        error(range, "Invalid exported name");
      }
    }

    if (!eatSemi()) {
      return None;
    }

    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::ExportNamedDeclarationNode(
            nullptr, std::move(specifiers), source, typeIdent_));
  }

  if (check(TokenKind::identifier)) {
    auto optAlias = parseTypeAliasFlow(typeIdentLoc, TypeAliasKind::None);
    if (!optAlias)
      return None;
    return setLocation(
        startLoc,
        *optAlias,
        new (context_) ESTree::ExportNamedDeclarationNode(
            *optAlias, {}, nullptr, typeIdent_));
  }

  errorExpected(
      {TokenKind::star, TokenKind::l_brace, TokenKind::identifier},
      "in export type declaration",
      "start of export",
      startLoc);
  return None;
}

Optional<ESTree::Node *> JSParserImpl::parseDeclareExportFlow(SMLoc start) {
  assert(check(TokenKind::rw_export));
  advance(JSLexer::GrammarContext::Type);
  SMLoc declareStart = tok_->getStartLoc();

  if (checkAndEat(TokenKind::rw_default, JSLexer::GrammarContext::Type)) {
    declareStart = tok_->getStartLoc();
    if (check(TokenKind::rw_function)) {
      auto optFunc = parseDeclareFunctionFlow(declareStart);
      if (!optFunc)
        return None;
      return setLocation(
          start,
          *optFunc,
          new (context_) ESTree::DeclareExportDeclarationNode(
              *optFunc, {}, nullptr, true));
    }
    if (context_.getParseFlowComponentSyntax() && checkHookDeclarationFlow()) {
      auto optFunc = parseDeclareHookFlow(declareStart);
      if (!optFunc)
        return None;
      return setLocation(
          start,
          *optFunc,
          new (context_) ESTree::DeclareExportDeclarationNode(
              *optFunc, {}, nullptr, true));
    }
    if (context_.getParseFlowComponentSyntax() && check(asyncIdent_) &&
        checkAsyncHookFlow()) {
      error(
          tok_->getStartLoc(),
          "`async` is not supported for declared hooks. "
          "Use `declare hook` instead.");
      advance(); // consume 'async'
      auto optHook = parseDeclareHookFlow(declareStart);
      if (!optHook)
        return None;
      return setLocation(
          start,
          *optHook,
          new (context_) ESTree::DeclareExportDeclarationNode(
              *optHook, {}, nullptr, true));
    }
    if (context_.getParseFlowComponentSyntax() && check(asyncIdent_) &&
        checkAsyncComponentFlow()) {
      error(
          tok_->getStartLoc(),
          "`async` is not supported for declared components. "
          "Use `declare component` instead.");
      advance(); // consume 'async'
      auto optComponent = parseComponentDeclarationFlow(
          start, /* declare */ true, /* isAsync */ true);
      if (!optComponent)
        return None;
      return setLocation(
          start,
          *optComponent,
          new (context_) ESTree::DeclareExportDeclarationNode(
              *optComponent, {}, nullptr, true));
    }
    if (context_.getParseFlowComponentSyntax() &&
        checkComponentDeclarationFlow()) {
      auto optComponent =
          parseComponentDeclarationFlow(start, /* declare */ true);
      if (!optComponent)
        return None;
      return setLocation(
          start,
          *optComponent,
          new (context_) ESTree::DeclareExportDeclarationNode(
              *optComponent, {}, nullptr, true));
    }
    if (check(TokenKind::rw_class)) {
      auto optClass = parseDeclareClassFlow(declareStart);
      if (!optClass)
        return None;
      return setLocation(
          start,
          *optClass,
          new (context_) ESTree::DeclareExportDeclarationNode(
              *optClass, {}, nullptr, true));
    }
    auto optType = parseTypeAnnotationFlow();
    if (!optType)
      return None;
    if (!eatSemi())
      return None;
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_)
            ESTree::DeclareExportDeclarationNode(*optType, {}, nullptr, true));
  }

  if (check(TokenKind::rw_function)) {
    auto optFunc = parseDeclareFunctionFlow(declareStart);
    if (!optFunc)
      return None;
    return setLocation(
        start,
        *optFunc,
        new (context_)
            ESTree::DeclareExportDeclarationNode(*optFunc, {}, nullptr, false));
  }

  if (context_.getParseFlowComponentSyntax() && checkHookDeclarationFlow()) {
    auto optFunc = parseDeclareHookFlow(declareStart);
    if (!optFunc)
      return None;
    return setLocation(
        start,
        *optFunc,
        new (context_)
            ESTree::DeclareExportDeclarationNode(*optFunc, {}, nullptr, false));
  }

  if (context_.getParseFlowComponentSyntax() && check(asyncIdent_) &&
      checkAsyncHookFlow()) {
    error(
        tok_->getStartLoc(),
        "`async` is not supported for declared hooks. "
        "Use `declare hook` instead.");
    advance(); // consume 'async'
    auto optHook = parseDeclareHookFlow(declareStart);
    if (!optHook)
      return None;
    return setLocation(
        start,
        *optHook,
        new (context_)
            ESTree::DeclareExportDeclarationNode(*optHook, {}, nullptr, false));
  }

  if (check(TokenKind::rw_class)) {
    auto optClass = parseDeclareClassFlow(declareStart);
    if (!optClass)
      return None;
    return setLocation(
        start,
        *optClass,
        new (context_) ESTree::DeclareExportDeclarationNode(
            *optClass, {}, nullptr, false));
  }

  if (context_.getParseFlowComponentSyntax() && check(asyncIdent_) &&
      checkAsyncComponentFlow()) {
    error(
        tok_->getStartLoc(),
        "`async` is not supported for declared components. "
        "Use `declare component` instead.");
    advance(); // consume 'async'
    auto optComponent = parseComponentDeclarationFlow(
        start, /* declare */ true, /* isAsync */ true);
    if (!optComponent)
      return None;
    return setLocation(
        start,
        *optComponent,
        new (context_) ESTree::DeclareExportDeclarationNode(
            *optComponent, {}, nullptr, false));
  }

  if (context_.getParseFlowComponentSyntax() &&
      checkComponentDeclarationFlow()) {
    auto optComponent =
        parseComponentDeclarationFlow(start, /* declare */ true);
    if (!optComponent)
      return None;
    return setLocation(
        start,
        *optComponent,
        new (context_) ESTree::DeclareExportDeclarationNode(
            *optComponent, {}, nullptr, false));
  }

  if (check(TokenKind::rw_enum)) {
    auto optEnum = parseEnumDeclarationFlow(start, /* declare */ true);
    if (!optEnum)
      return None;
    return setLocation(
        start,
        *optEnum,
        new (context_)
            ESTree::DeclareExportDeclarationNode(*optEnum, {}, nullptr, false));
  }

  if (checkN(TokenKind::rw_var, TokenKind::rw_const, letIdent_)) {
    ESTree::NodeLabel kind = tok_->getResWordOrIdentifier();
    SMLoc varStart = advance(JSLexer::GrammarContext::Type).Start;
    auto optIdent = parseBindingIdentifier(Param{});
    if (!optIdent) {
      errorExpected(
          TokenKind::identifier,
          "in var declaration",
          "start of declaration",
          start);
      return None;
    }
    if (!(*optIdent)->_typeAnnotation) {
      error(
          (*optIdent)->getSourceRange(),
          "expected type annotation on declared var");
    }
    if (!eatSemi())
      return None;

    SMLoc end = getPrevTokenEndLoc();
    return setLocation(
        start,
        end,
        new (context_) ESTree::DeclareExportDeclarationNode(
            setLocation(
                varStart,
                end,
                new (context_) ESTree::DeclareVariableNode(*optIdent, kind)),
            {},
            nullptr,
            false));
  }

  if (checkAndEat(opaqueIdent_, JSLexer::GrammarContext::Type)) {
    if (!check(typeIdent_)) {
      error(tok_->getStartLoc(), "'type' required in opaque type declaration");
      return None;
    }
    advance(JSLexer::GrammarContext::Type);
    auto optType =
        parseTypeAliasFlow(declareStart, TypeAliasKind::DeclareOpaque);
    if (!optType)
      return None;
    return setLocation(
        start,
        *optType,
        new (context_)
            ESTree::DeclareExportDeclarationNode(*optType, {}, nullptr, false));
  }

  if (check(typeIdent_)) {
    advance(JSLexer::GrammarContext::Type);
    auto optType = parseTypeAliasFlow(declareStart, TypeAliasKind::None);
    if (!optType)
      return None;
    return setLocation(
        start,
        *optType,
        new (context_)
            ESTree::DeclareExportDeclarationNode(*optType, {}, nullptr, false));
  }

  if (checkN(TokenKind::rw_interface, interfaceIdent_)) {
    auto optInterface = parseInterfaceDeclarationFlow();
    if (!optInterface)
      return None;
    return setLocation(
        start,
        *optInterface,
        new (context_) ESTree::DeclareExportDeclarationNode(
            *optInterface, {}, nullptr, false));
  }

  if (checkAndEat(TokenKind::star, JSLexer::GrammarContext::Type)) {
    // declare export * from 'foo';
    //                  ^
    if (!check(fromIdent_)) {
      error(
          tok_->getStartLoc(), "expected 'from' clause in export declaration");
      return None;
    }
    auto optSource = parseFromClause();
    if (!optSource)
      return None;
    if (!eatSemi())
      return None;
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::DeclareExportAllDeclarationNode(*optSource));
  }

  if (!need(
          TokenKind::l_brace, "in export specifier", "start of declare", start))
    return None;

  ESTree::NodeList specifiers{};
  llvh::SmallVector<SMRange, 2> invalids{};
  if (!parseExportClause(specifiers, invalids))
    return None;

  ESTree::Node *source = nullptr;
  if (check(fromIdent_)) {
    auto optSource = parseFromClause();
    if (!optSource)
      return None;
    source = *optSource;
  }

  if (!eatSemi())
    return None;

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::DeclareExportDeclarationNode(
          nullptr, std::move(specifiers), source, false));
}

Optional<ESTree::Node *> JSParserImpl::parseReturnTypeAnnotationFlow(
    Optional<SMLoc> wrappedStart,
    AllowAnonFunctionType allowAnonFunctionType) {
  SMLoc start = tok_->getStartLoc();
  ESTree::Node *returnType = nullptr;
  if (check(assertsIdent_)) {
    // TypePredicate (asserts = true) or TypeAnnotation:
    //   TypeAnnotation
    //   asserts IdentifierName
    //   asserts IdentifierName is TypeAnnotation
    auto optType = parseTypeAnnotationFlow(None, allowAnonFunctionType);
    if (!optType)
      return None;

    if (check(TokenKind::identifier)) {
      // Validate the "asserts" token was an identifier not a more complex type.
      auto optId = reparseTypeAnnotationAsIdentifierFlow(*optType);
      if (!optId)
        return None;
      ESTree::Node *id = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
      advance(JSLexer::GrammarContext::Type);
      ESTree::Node *typeAnnotation = nullptr;
      if (checkAndEat(isIdent_, JSLexer::GrammarContext::Type)) {
        // assert IdentifierName is TypeAnnotation
        //                          ^
        auto optType = parseTypeAnnotationFlow(None, allowAnonFunctionType);
        if (!optType)
          return None;
        typeAnnotation = *optType;
      }
      returnType = setLocation(
          start,
          getPrevTokenEndLoc(),
          new (context_)
              ESTree::TypePredicateNode(id, typeAnnotation, assertsIdent_));
    } else {
      returnType = *optType;
    }
  } else if (check(impliesIdent_)) {
    // TypePredicate (implies = true) or TypeAnnotation:
    //   TypeAnnotation
    //   implies IdentifierName is TypeAnnotation

    //   implies IdentifierName is TypeAnnotation
    //   ^
    auto optType = parseTypeAnnotationFlow(None, allowAnonFunctionType);
    if (!optType)
      return None;

    if (check(TokenKind::identifier, TokenKind::rw_this)) {
      // Validate the "implies" token was an identifier not a more complex type.
      if (auto *generic = dyn_cast<ESTree::GenericTypeAnnotationNode>(*optType);
          !(generic && !generic->_typeParameters)) {
        error(
            tok_->getStartLoc(),
            "invalid return annotation. 'implies' type guard needs to be followed by identifier");
        return None;
      }

      //   implies IdentifierName is TypeAnnotation
      //           ^
      ESTree::Node *id = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::IdentifierNode(
              tok_->getResWordOrIdentifier(), nullptr, false));
      advance(JSLexer::GrammarContext::Type);

      //   implies IdentifierName is TypeAnnotation
      //                          ^
      if (!checkAndEat(isIdent_, JSLexer::GrammarContext::Type)) {
        error(
            tok_->getStartLoc(),
            "expecting 'is' after parameter of 'implies' type guard");
        return None;
      }
      //   implies IdentifierName is TypeAnnotation
      //                             ^
      auto optTypeT = parseTypeAnnotationFlow(None, allowAnonFunctionType);
      if (!optTypeT)
        return None;
      returnType = setLocation(
          start,
          getPrevTokenEndLoc(),
          new (context_)
              ESTree::TypePredicateNode(id, *optTypeT, impliesIdent_));
    } else {
      // implies (as type -- okay)
      returnType = *optType;
    }
  } else {
    // TypePredicate (asserts = false && implies = false) or TypeAnnotation:
    //   TypeAnnotation
    //   IdentifierName is TypeAnnotation

    auto optType = parseTypeAnnotationFlow(None, allowAnonFunctionType);
    if (!optType)
      return None;

    if (checkAndEat(isIdent_, JSLexer::GrammarContext::Type)) {
      auto optId = reparseTypeAnnotationAsIdentifierFlow(*optType);
      if (!optId)
        return None;
      auto optType = parseTypeAnnotationFlow(None, allowAnonFunctionType);
      if (!optType)
        return None;
      returnType = setLocation(
          start,
          getPrevTokenEndLoc(),
          new (context_) ESTree::TypePredicateNode(*optId, *optType, nullptr));
    } else {
      returnType = *optType;
    }
  }

  if (wrappedStart) {
    return setLocation(
        *wrappedStart,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TypeAnnotationNode(returnType));
  }
  return returnType;
}

Optional<ESTree::Node *> JSParserImpl::parseTypeAnnotationBeforeColonFlow() {
  // If the identifier name is a known keyword we need to lookahead to see if
  // its a type or an identifier otherwise it could fail to parse.
  if (check(TokenKind::identifier) &&
      (context_.getParseFlowComponentSyntax())) {
    if ((tok_->getResWordOrIdentifier() == componentIdent_) ||
        (tok_->getResWordOrIdentifier() == hookIdent_) ||
        (tok_->getResWordOrIdentifier() == rendersIdent_ &&
         !tok_->checkFollowingCharacter('?'))) {
      OptValue<TokenKind> optNext = lexer_.lookahead1(None);
      if (optNext.hasValue() &&
          (*optNext == TokenKind::colon || *optNext == TokenKind::question)) {
        auto id = setLocation(
            tok_,
            tok_,
            new (context_) ESTree::GenericTypeAnnotationNode(
                setLocation(
                    tok_,
                    tok_,
                    new (context_) ESTree::IdentifierNode(
                        tok_->getResWordOrIdentifier(), nullptr, false)),
                nullptr));
        advance(JSLexer::GrammarContext::Type);
        return id;
      }
    } else if (
        tok_->getResWordOrIdentifier() == rendersIdent_ &&
        tok_->checkFollowingCharacter('?')) {
      SMLoc startLoc = tok_->getStartLoc();
      auto id = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::GenericTypeAnnotationNode(
              setLocation(
                  tok_,
                  tok_,
                  new (context_) ESTree::IdentifierNode(
                      tok_->getResWordOrIdentifier(), nullptr, false)),
              nullptr));
      advance(JSLexer::GrammarContext::Type);
      OptValue<TokenKind> optNext = lexer_.lookahead1(None);
      if (optNext.hasValue() && (*optNext == TokenKind::colon)) {
        return id;
      } else {
        if (!eat(
                TokenKind::question,
                JSLexer::GrammarContext::Type,
                "in render type annotation",
                "start of render type",
                startLoc)) {
          return None;
        }
        auto optBody = parsePrefixTypeAnnotationFlow();
        if (!optBody)
          return None;
        return setLocation(
            startLoc,
            getPrevTokenEndLoc(),
            new (context_)
                ESTree::TypeOperatorNode(rendersMaybeOperator_, *optBody));
      }
    }
  }

  return parseTypeAnnotationFlow();
}

Optional<ESTree::Node *> JSParserImpl::parseTypeAnnotationFlow(
    Optional<SMLoc> wrappedStart,
    AllowAnonFunctionType allowAnonFunctionType) {
  llvh::SaveAndRestore<bool> saveParam(
      allowAnonFunctionType_,
      allowAnonFunctionType == AllowAnonFunctionType::Yes);
  auto optType = parseConditionalTypeAnnotationFlow();
  if (!optType)
    return None;
  if (wrappedStart) {
    return setLocation(
        *wrappedStart,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TypeAnnotationNode(*optType));
  }
  return *optType;
}

Optional<ESTree::Node *> JSParserImpl::parseConditionalTypeAnnotationFlow() {
  SMLoc start = tok_->getStartLoc();
  llvh::SaveAndRestore<bool> saveParam(allowConditionalType_, true);
  auto optCheck = parseUnionTypeAnnotationFlow();
  if (!optCheck)
    return None;
  if (!checkAndEat(TokenKind::rw_extends, JSLexer::GrammarContext::Type)) {
    return optCheck;
  }
  Optional<ESTree::Node *> optExtends;
  {
    // We need to enter the state of parsing the extends_type disallowing
    // conditional types not wrapped by parantheses, so that the following
    // sequence `A extends infer B extends C ? D : E` will be interpreted
    // as `A extends (infer B extends C) ? D : E`.
    llvh::SaveAndRestore<bool> saveParam(allowConditionalType_, false);
    optExtends = parseUnionTypeAnnotationFlow();
  }
  if (!optExtends)
    return None;

  if (!eat(
          TokenKind::question,
          JSLexer::GrammarContext::Type,
          "in conditional type",
          "start of type",
          start))
    return None;

  auto optTrue = parseTypeAnnotationFlow();
  if (!optTrue)
    return None;
  if (!eat(
          TokenKind::colon,
          JSLexer::GrammarContext::Type,
          "in conditional type",
          "start of type",
          start))
    return None;

  auto optFalse = parseTypeAnnotationFlow();
  if (!optFalse)
    return None;

  return setLocation(
      *optCheck,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ConditionalTypeAnnotationNode(
          *optCheck, *optExtends, *optTrue, *optFalse));
}

Optional<ESTree::Node *> JSParserImpl::parseUnionTypeAnnotationFlow() {
  SMLoc start = tok_->getStartLoc();
  checkAndEat(TokenKind::pipe, JSLexer::GrammarContext::Type);

  auto optFirst = parseIntersectionTypeAnnotationFlow();
  if (!optFirst)
    return None;

  if (!check(TokenKind::pipe)) {
    // Done with the union, move on.
    return *optFirst;
  }

  ESTree::NodeList types{};
  types.push_back(**optFirst);

  while (checkAndEat(TokenKind::pipe, JSLexer::GrammarContext::Type)) {
    auto optInt = parseIntersectionTypeAnnotationFlow();
    if (!optInt)
      return None;
    types.push_back(**optInt);
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::UnionTypeAnnotationNode(std::move(types)));
}

Optional<ESTree::Node *> JSParserImpl::parseIntersectionTypeAnnotationFlow() {
  SMLoc start = tok_->getStartLoc();
  checkAndEat(TokenKind::amp, JSLexer::GrammarContext::Type);

  auto optFirst = parseAnonFunctionWithoutParensTypeAnnotationFlow();
  if (!optFirst)
    return None;

  if (!check(TokenKind::amp)) {
    // Done with the union, move on.
    return *optFirst;
  }

  ESTree::NodeList types{};
  types.push_back(**optFirst);

  while (checkAndEat(TokenKind::amp, JSLexer::GrammarContext::Type)) {
    auto optInt = parseAnonFunctionWithoutParensTypeAnnotationFlow();
    if (!optInt)
      return None;
    types.push_back(**optInt);
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::IntersectionTypeAnnotationNode(std::move(types)));
}

Optional<ESTree::Node *>
JSParserImpl::parseAnonFunctionWithoutParensTypeAnnotationFlow() {
  SMLoc start = tok_->getStartLoc();
  auto optParam = parsePrefixTypeAnnotationFlow();
  if (!optParam)
    return None;

  if (allowAnonFunctionType_ && check(TokenKind::equalgreater)) {
    // ParamType => ReturnType
    //           ^
    ESTree::NodeList params{};
    // "Reparse" the param into a FunctionTypeParam so it can be used for
    // parseFunctionTypeAnnotationWithParamsFlow.
    params.push_back(*setLocation(
        *optParam,
        *optParam,
        new (context_) ESTree::FunctionTypeParamNode(
            /* name */ nullptr, *optParam, /* optional */ false)));
    ESTree::Node *rest = nullptr;
    ESTree::Node *typeParams = nullptr;
    return parseFunctionTypeAnnotationWithParamsFlow(
        start, std::move(params), nullptr, rest, typeParams, /* hook */ false);
  }

  return *optParam;
}

Optional<ESTree::Node *> JSParserImpl::parsePrefixTypeAnnotationFlow() {
  SMLoc start = tok_->getStartLoc();
  if (checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type)) {
    auto optPrefix = parsePrefixTypeAnnotationFlow();
    if (!optPrefix)
      return None;
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::NullableTypeAnnotationNode(*optPrefix));
  }
  return parsePostfixTypeAnnotationFlow();
}

Optional<ESTree::Node *> JSParserImpl::parsePostfixTypeAnnotationFlow() {
  SMLoc start = tok_->getStartLoc();
  auto optPrimary = parsePrimaryTypeAnnotationFlow();
  if (!optPrimary)
    return None;

  ESTree::Node *result = *optPrimary;
  bool seenOptionalIndexedAccess = false;

  while (check(TokenKind::l_square, TokenKind::questiondot) &&
         !lexer_.isNewLineBeforeCurrentToken()) {
    bool optional = checkAndEat(TokenKind::questiondot);
    seenOptionalIndexedAccess = seenOptionalIndexedAccess || optional;

    if (!eat(
            TokenKind::l_square,
            JSLexer::GrammarContext::Type,
            "in indexed access type or postfix array type syntax",
            "start of a type",
            start))
      return None;

    if (!optional &&
        checkAndEat(TokenKind::r_square, JSLexer::GrammarContext::Type)) {
      // Legacy Array syntax `T[]`
      result = setLocation(
          start,
          getPrevTokenEndLoc(),
          new (context_) ESTree::ArrayTypeAnnotationNode(result));
    } else {
      // Indexed Access `T[K]` (`T?.[K]` if `optional`)
      auto optIndexType = parseTypeAnnotationFlow();
      if (!optIndexType)
        return None;
      if (!need(
              TokenKind::r_square,
              "in indexed access type",
              "start of type",
              start))
        return None;
      if (seenOptionalIndexedAccess) {
        result = setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::OptionalIndexedAccessTypeNode(
                result, *optIndexType, optional));
      } else {
        result = setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_)
                ESTree::IndexedAccessTypeNode(result, *optIndexType));
      }
    }
  }

  return result;
}

Optional<ESTree::Node *> JSParserImpl::parsePrimaryTypeAnnotationFlow() {
  SMLoc start = tok_->getStartLoc();
  switch (tok_->getKind()) {
    case TokenKind::star:
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::ExistsTypeAnnotationNode());
    case TokenKind::less:
      return parseFunctionTypeAnnotationFlow();
    case TokenKind::l_paren:
      return parseFunctionOrGroupTypeAnnotationFlow();
    case TokenKind::l_brace:
    case TokenKind::l_bracepipe:
      return parseObjectTypeAnnotationFlow(
          AllowProtoProperty::No,
          AllowStaticProperty::No,
          AllowSpreadProperty::Yes);
    case TokenKind::rw_interface: {
      advance(JSLexer::GrammarContext::Type);
      ESTree::NodeList extends{};
      auto optBody = parseInterfaceTailFlow(start, extends);
      if (!optBody)
        return None;
      return setLocation(
          start,
          *optBody,
          new (context_) ESTree::InterfaceTypeAnnotationNode(
              std::move(extends), *optBody));
    }
    case TokenKind::rw_typeof:
      return parseTypeofTypeAnnotationFlow();

    case TokenKind::l_square:
      return parseTupleTypeAnnotationFlow();
    case TokenKind::rw_static:
    case TokenKind::rw_this:
    case TokenKind::identifier:
      if (tok_->getResWordOrIdentifier() == anyIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::AnyTypeAnnotationNode());
      }
      if (tok_->getResWordOrIdentifier() == mixedIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::MixedTypeAnnotationNode());
      }
      if (tok_->getResWordOrIdentifier() == emptyIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::EmptyTypeAnnotationNode());
      }
      if (tok_->getResWordOrIdentifier() == booleanIdent_ ||
          tok_->getResWordOrIdentifier() == boolIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::BooleanTypeAnnotationNode());
      }
      if (tok_->getResWordOrIdentifier() == numberIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::NumberTypeAnnotationNode());
      }
      if (tok_->getResWordOrIdentifier() == symbolIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::SymbolTypeAnnotationNode());
      }
      if (tok_->getResWordOrIdentifier() == stringIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::StringTypeAnnotationNode());
      }
      if (tok_->getResWordOrIdentifier() == bigintIdent_) {
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::BigIntTypeAnnotationNode());
      }
      if (tok_->getResWordOrIdentifier() == keyofIdent_) {
        advance(JSLexer::GrammarContext::Type);
        auto optBody = parsePrefixTypeAnnotationFlow();
        if (!optBody)
          return None;
        return setLocation(
            start,
            getPrevTokenEndLoc(),
            new (context_) ESTree::KeyofTypeAnnotationNode(*optBody));
      }
      if (context_.getParseFlowComponentSyntax() &&
          tok_->getResWordOrIdentifier() == rendersIdent_) {
        auto optTypeOperator = parseRenderTypeOperator();
        auto optBody = parsePrefixTypeAnnotationFlow();
        if (!optBody || !optTypeOperator)
          return None;
        return setLocation(
            start,
            getPrevTokenEndLoc(),
            new (context_)
                ESTree::TypeOperatorNode(*optTypeOperator, *optBody));
      }
      if (context_.getParseFlowComponentSyntax() &&
          tok_->getResWordOrIdentifier() == componentIdent_) {
        auto optComponent = parseComponentTypeAnnotationFlow();
        if (!optComponent)
          return None;
        return *optComponent;
      }
      if (context_.getParseFlowComponentSyntax() &&
          tok_->getResWordOrIdentifier() == hookIdent_) {
        auto optHook = parseHookTypeAnnotationFlow();
        if (!optHook)
          return None;
        return *optHook;
      }
      if (tok_->getResWordOrIdentifier() == interfaceIdent_) {
        advance(JSLexer::GrammarContext::Type);
        ESTree::NodeList extends{};
        auto optBody = parseInterfaceTailFlow(start, extends);
        if (!optBody)
          return None;
        return setLocation(
            start,
            *optBody,
            new (context_) ESTree::InterfaceTypeAnnotationNode(
                std::move(extends), *optBody));
      }
      if (tok_->getResWordOrIdentifier() == inferIdent_) {
        advance(JSLexer::GrammarContext::Type);

        if (!need(TokenKind::identifier, "in type parameter", nullptr, {}))
          return None;
        UniqueString *name = tok_->getIdentifier();
        advance(JSLexer::GrammarContext::Type);

        ESTree::Node *bound = nullptr;
        if (check(TokenKind::rw_extends)) {
          // When we see an extends keyword,
          // we enter the parsing logic that might need backtracking.
          //
          // For `infer A extends B ...`, is the `extends B` part of an infer
          // type, or part of a larger conditional type like `infer A extends B
          // ? C : D`?
          //
          // We don't know, so we assume it's part of the infer type for now,
          // and later backtrack if the assumption is wrong.
          JSLexer::SavePoint savePoint{&lexer_};
          advance(JSLexer::GrammarContext::Type);
          auto parsedBound = parseUnionTypeAnnotationFlow();
          if ((allowConditionalType_ && check(TokenKind::question)) ||
              !parsedBound) {
            // If we look ahead and see `?`, it might be the case that we are
            // parsing a conditional type like `infer A extends B ? C : D`. If
            // the current context allow parsing conditional type, then we must
            // backtrack so that only `infer A` is treated as part of the infer
            // type.
            //
            // Of course, if we fail to parse the type after extends, we also
            // need to backtrack.
            savePoint.restore();
          } else {
            bound = *parsedBound;
          }
        }

        return setLocation(
            start,
            getPrevTokenEndLoc(),
            new (context_) ESTree::InferTypeAnnotationNode(setLocation(
                start,
                getPrevTokenEndLoc(),
                new (context_) ESTree::TypeParameterNode(
                    name, false, bound, nullptr, nullptr, true))));
      }

      {
        auto optGeneric = parseGenericTypeFlow();
        if (!optGeneric)
          return None;
        return *optGeneric;
      }

    case TokenKind::rw_null:
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::NullLiteralTypeAnnotationNode());

    case TokenKind::rw_void:
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::VoidTypeAnnotationNode());

    case TokenKind::string_literal: {
      UniqueString *str = tok_->getStringLiteral();
      UniqueString *raw = lexer_.getStringLiteral(tok_->inputStr());
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::StringLiteralTypeAnnotationNode(str, raw));
    }

    case TokenKind::numeric_literal: {
      double value = tok_->getNumericLiteral();
      UniqueString *raw = lexer_.getStringLiteral(tok_->inputStr());
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::NumberLiteralTypeAnnotationNode(value, raw));
    }

    case TokenKind::bigint_literal: {
      UniqueString *raw = tok_->getBigIntLiteral();
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::BigIntLiteralTypeAnnotationNode(raw));
    }

    case TokenKind::minus: {
      advance(JSLexer::GrammarContext::Type);
      if (check(TokenKind::numeric_literal)) {
        // Negate the literal.
        double value = -tok_->getNumericLiteral();
        UniqueString *raw = lexer_.getStringLiteral(
            llvh::StringRef(
                start.getPointer(),
                tok_->getEndLoc().getPointer() - start.getPointer()));
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::NumberLiteralTypeAnnotationNode(value, raw));
      } else if (check(TokenKind::bigint_literal)) {
        UniqueString *raw = lexer_.getStringLiteral(
            llvh::StringRef(
                start.getPointer(),
                tok_->getEndLoc().getPointer() - start.getPointer()));
        return setLocation(
            start,
            advance(JSLexer::GrammarContext::Type).End,
            new (context_) ESTree::BigIntLiteralTypeAnnotationNode(raw));
      } else {
        errorExpected(
            TokenKind::numeric_literal,
            "in type annotation",
            "start of annotation",
            start);
        return None;
      }
    }

    case TokenKind::rw_true:
    case TokenKind::rw_false: {
      bool value = check(TokenKind::rw_true);
      auto *raw = tok_->getResWordIdentifier();
      return setLocation(
          start,
          advance(JSLexer::GrammarContext::Type).End,
          new (context_) ESTree::BooleanLiteralTypeAnnotationNode(value, raw));
    }
    default:
      if (tok_->isResWord()) {
        auto optGeneric = parseGenericTypeFlow();
        if (!optGeneric)
          return None;
        return *optGeneric;
      }
      error(tok_->getStartLoc(), "unexpected token in type annotation");
      return None;
  }
}

Optional<ESTree::Node *> JSParserImpl::parseTypeofTypeAnnotationFlow() {
  assert(check(TokenKind::rw_typeof));
  SMLoc startLoc = advance().Start;
  uint32_t parenCount = 0;

  while (checkAndEat(TokenKind::l_paren))
    ++parenCount;

  if (!need(TokenKind::identifier, "in typeof type", "start of type", startLoc))
    return None;

  ESTree::Node *ident = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  while (checkAndEat(TokenKind::period)) {
    if (!check(TokenKind::identifier) && !tok_->isResWord()) {
      errorExpected(
          TokenKind::identifier,
          "in qualified typeof type",
          "start of type",
          startLoc);
      return None;
    }
    ESTree::Node *next = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(
            tok_->getResWordOrIdentifier(), nullptr, false));
    advance(JSLexer::GrammarContext::Type);
    ident = setLocation(
        ident,
        next,
        new (context_) ESTree::QualifiedTypeofIdentifierNode(ident, next));
  }

  for (; parenCount > 0; --parenCount) {
    if (!eat(
            TokenKind::r_paren,
            JSLexer::GrammarContext::Type,
            "in typeof type",
            "start of type",
            startLoc))
      return None;
    ident->incParens();
  }

  ESTree::Node *typeArguments = nullptr;
  if (check(TokenKind::less) && !lexer_.isNewLineBeforeCurrentToken()) {
    auto optTypeArgs = parseTypeArgsFlow();
    if (!optTypeArgs)
      return None;
    typeArguments = *optTypeArgs;
  }

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TypeofTypeAnnotationNode(ident, typeArguments));
}

Optional<ESTree::Node *> JSParserImpl::parseTupleTypeAnnotationFlow() {
  assert(check(TokenKind::l_square));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  ESTree::NodeList types{};
  bool inexact = false;

  while (!check(TokenKind::r_square)) {
    SMLoc startLoc = tok_->getStartLoc();
    bool startsWithDotDotDot =
        checkAndEat(TokenKind::dotdotdot, JSLexer::GrammarContext::Type);

    // ...]
    if (startsWithDotDotDot && check(TokenKind::r_square)) {
      inexact = true;
      // ...,
    } else if (startsWithDotDotDot && check(TokenKind::comma)) {
      error(
          tok_->getSourceRange(),
          "trailing commas after inexact tuple types are not allowed");
      advance(JSLexer::GrammarContext::Type);
    } else {
      auto optType = parseTupleElementFlow(startLoc, startsWithDotDotDot);
      if (!optType)
        return None;
      types.push_back(**optType);

      if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type))
        break;
    }
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
      new (context_)
          ESTree::TupleTypeAnnotationNode(std::move(types), inexact));
}

Optional<ESTree::Node *> JSParserImpl::parseTupleElementFlow(
    SMLoc startLoc,
    bool startsWithDotDotDot) {
  ESTree::Node *label = nullptr;
  ESTree::Node *elementType = nullptr;
  ESTree::Node *variance = nullptr;

  // ...Identifier : Type
  // ...Type
  // ^
  if (startsWithDotDotDot) {
    auto optType = parseTypeAnnotationBeforeColonFlow();
    if (!optType)
      return None;
    if (checkAndEat(TokenKind::colon, JSLexer::GrammarContext::Type)) {
      auto optLabel = reparseTypeAnnotationAsIdentifierFlow(*optType);
      if (!optLabel)
        return None;
      label = *optLabel;
      auto optType = parseTypeAnnotationFlow();
      if (!optType)
        return None;
      elementType = *optType;
      return setLocation(
          startLoc,
          getPrevTokenEndLoc(),
          new (context_)
              ESTree::TupleTypeSpreadElementNode(label, elementType));
    }

    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TupleTypeSpreadElementNode(label, *optType));
  }

  /// +Identifier : Type
  /// -Identifier : Type
  /// ^
  if (check(TokenKind::plus, TokenKind::minus)) {
    variance = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::VarianceNode(
            check(TokenKind::plus) ? plusIdent_ : minusIdent_));
    advance(JSLexer::GrammarContext::Type);
  }

  /// Identifier [?] : Type
  /// Type
  /// ^
  auto optType = parseTypeAnnotationBeforeColonFlow();
  if (!optType)
    return None;

  /// Identifier [?] : Type
  ///             ^
  if (check(TokenKind::colon, TokenKind::question)) {
    bool optional =
        checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type);

    if (!eat(
            TokenKind::colon,
            JSLexer::GrammarContext::Type,
            "in labeled tuple type element",
            "location of tuple",
            startLoc))
      return None;

    auto optLabel = reparseTypeAnnotationAsIdentifierFlow(*optType);
    if (!optLabel)
      return None;
    label = *optLabel;
    auto optType = parseTypeAnnotationFlow();
    if (!optType)
      return None;
    elementType = *optType;

    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TupleTypeLabeledElementNode(
            label, elementType, optional, variance));
  }

  if (variance) {
    error(
        variance->getSourceRange(),
        "Variance can only be used with labeled tuple elements");
  }

  return *optType;
}

Optional<ESTree::Node *> JSParserImpl::parseHookTypeAnnotationFlow() {
  // hook
  assert(check(hookIdent_));
  advance(JSLexer::GrammarContext::Type);
  return parseFunctionOrHookTypeAnnotationFlow(true);
}

Optional<ESTree::Node *> JSParserImpl::parseFunctionTypeAnnotationFlow() {
  return parseFunctionOrHookTypeAnnotationFlow(false);
}

Optional<ESTree::Node *> JSParserImpl::parseFunctionOrHookTypeAnnotationFlow(
    bool hook) {
  SMLoc start = tok_->getStartLoc();

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optParams = parseTypeParamsFlow();
    if (!optParams)
      return None;
    typeParams = *optParams;
  }

  if (!need(
          TokenKind::l_paren,
          "in function type annotation",
          "start of annotation",
          start))
    return None;

  ESTree::NodeList params{};
  ESTree::Node *thisConstraint = nullptr;
  auto optRest =
      parseFunctionTypeAnnotationParamsFlow(params, thisConstraint, hook);
  if (!optRest)
    return None;
  ESTree::Node *rest = *optRest;

  if (!need(
          TokenKind::equalgreater,
          "in function type annotation",
          "start of annotation",
          start))
    return None;

  return parseFunctionTypeAnnotationWithParamsFlow(
      start, std::move(params), thisConstraint, rest, typeParams, hook);
}

Optional<ESTree::Node *>
JSParserImpl::parseFunctionTypeAnnotationWithParamsFlow(
    SMLoc start,
    ESTree::NodeList &&params,
    ESTree::Node *thisConstraint,
    ESTree::Node *rest,
    ESTree::Node *typeParams,
    bool hook) {
  assert(check(TokenKind::equalgreater));
  advance(JSLexer::GrammarContext::Type);

  auto optReturnType = parseReturnTypeAnnotationFlow();
  if (!optReturnType)
    return None;

  if (!hook) {
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::FunctionTypeAnnotationNode(
            std::move(params),
            thisConstraint,
            *optReturnType,
            rest,
            typeParams));
  } else {
    return setLocation(
        start,
        getPrevTokenEndLoc(),
        new (context_) ESTree::HookTypeAnnotationNode(
            std::move(params), *optReturnType, rest, typeParams));
  }
}

Optional<ESTree::Node *>
JSParserImpl::parseFunctionOrGroupTypeAnnotationFlow() {
  assert(check(TokenKind::l_paren));
  // This is either
  // ( Type )
  // ^
  // or
  // ( ParamList ) => Type
  // ^
  // so we use a similar approach to arrow function parameters by keeping track
  // and reparsing in certain cases.
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  bool isFunction = false;
  ESTree::Node *type = nullptr;
  ESTree::Node *rest = nullptr;
  ESTree::NodeList params{};
  ESTree::Node *thisConstraint = nullptr;

  if (check(TokenKind::rw_this)) {
    OptValue<TokenKind> optNext = lexer_.lookahead1(None);
    if (optNext.hasValue() && *optNext == TokenKind::colon) {
      SMLoc thisStart = advance(JSLexer::GrammarContext::Type).Start;
      advance(JSLexer::GrammarContext::Type);
      auto optType = parseTypeAnnotationFlow();
      if (!optType)
        return None;
      ESTree::Node *typeAnnotation = *optType;

      thisConstraint = setLocation(
          thisStart,
          getPrevTokenEndLoc(),
          new (context_) ESTree::FunctionTypeParamNode(
              /* name */ nullptr, typeAnnotation, /* optional */ false));
      checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type);
    } else if (optNext.hasValue() && *optNext == TokenKind::question) {
      error(tok_->getSourceRange(), "'this' constraint may not be optional");
    }
  }

  if (allowAnonFunctionType_ &&
      checkAndEat(TokenKind::dotdotdot, JSLexer::GrammarContext::Type)) {
    isFunction = true;
    // Must be parameters, and this must be the last one.
    auto optParam = parseFunctionTypeAnnotationParamFlow();
    if (!optParam)
      return None;
    // Rest param must be the last param.
    rest = *optParam;
  } else if (check(TokenKind::r_paren)) {
    isFunction = true;
    // ( )
    //   ^
    // No parameters, but this must be an empty param list.
  } else {
    // Not sure yet whether this is a param or simply a type.
    auto optParam = parseFunctionTypeAnnotationParamFlow();
    if (!optParam)
      return None;
    ESTree::FunctionTypeParamNode *param = *optParam;
    type = param->_typeAnnotation;
    if (param->_name || param->_optional) {
      // Must be a param if it has a name.or if it was optional.
      isFunction = true;
    }
    params.push_back(*param);
  }

  // If isFunction was already forced by something previously then we
  // have no choice but to attempt to parse as a function type annotation.
  if ((isFunction || allowAnonFunctionType_) &&
      checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type)) {
    isFunction = true;
    while (!check(TokenKind::r_paren)) {
      bool isRest = !rest &&
          checkAndEat(TokenKind::dotdotdot, JSLexer::GrammarContext::Type);

      auto optParam = parseFunctionTypeAnnotationParamFlow();
      if (!optParam)
        return None;
      if (isRest) {
        rest = *optParam;
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
          "at end of function annotation parameters",
          "start of parameters",
          start))
    return None;

  if (isFunction) {
    if (!eat(
            TokenKind::equalgreater,
            JSLexer::GrammarContext::Type,
            "in function type annotation",
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

  auto optReturnType = parseReturnTypeAnnotationFlow(
      None,
      allowAnonFunctionType_ ? AllowAnonFunctionType::Yes
                             : AllowAnonFunctionType::No);
  if (!optReturnType)
    return None;

  ESTree::Node *typeParams = nullptr;
  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::FunctionTypeAnnotationNode(
          std::move(params), thisConstraint, *optReturnType, rest, typeParams));
}

Optional<ESTree::Node *> JSParserImpl::parseObjectTypeAnnotationFlow(
    AllowProtoProperty allowProtoProperty,
    AllowStaticProperty allowStaticProperty,
    AllowSpreadProperty allowSpreadProperty) {
  assert(check(TokenKind::l_brace, TokenKind::l_bracepipe));
  bool exact = check(TokenKind::l_bracepipe);
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  ESTree::NodeList properties{};
  ESTree::NodeList indexers{};
  ESTree::NodeList callProperties{};
  ESTree::NodeList internalSlots{};
  bool inexact = false;

  if (!parseObjectTypePropertiesFlow(
          allowProtoProperty,
          allowStaticProperty,
          allowSpreadProperty,
          properties,
          indexers,
          callProperties,
          internalSlots,
          inexact))
    return None;

  if (exact && inexact) {
    // Doesn't prevent parsing from continuing, but it is an error.
    error(
        start,
        "Explicit inexact syntax cannot appear inside an explicit exact object type");
  }

  SMLoc end = tok_->getEndLoc();
  if (!eat(
          exact ? TokenKind::piper_brace : TokenKind::r_brace,
          JSLexer::GrammarContext::Type,
          "at end of exact object type annotation",
          "start of object",
          start))
    return None;

  return setLocation(
      start,
      end,
      new (context_) ESTree::ObjectTypeAnnotationNode(
          std::move(properties),
          std::move(indexers),
          std::move(callProperties),
          std::move(internalSlots),
          inexact,
          exact));
}

bool JSParserImpl::parseObjectTypePropertiesFlow(
    AllowProtoProperty allowProtoProperty,
    AllowStaticProperty allowStaticProperty,
    AllowSpreadProperty allowSpreadProperty,
    ESTree::NodeList &properties,
    ESTree::NodeList &indexers,
    ESTree::NodeList &callProperties,
    ESTree::NodeList &internalSlots,
    bool &inexact) {
  while (!check(TokenKind::r_brace, TokenKind::piper_brace)) {
    SMLoc start = tok_->getStartLoc();
    if (check(TokenKind::dotdotdot)) {
      // Spread property or explicit '...' for inexact.
      advance(JSLexer::GrammarContext::Type);
      if (check(TokenKind::comma, TokenKind::semi)) {
        inexact = true;
        advance(JSLexer::GrammarContext::Type);
        // Explicit '...' must be the last element in the type annotation.
        return true;
      } else if (check(TokenKind::r_brace, TokenKind::piper_brace)) {
        inexact = true;
        return true;
      } else {
        if (allowSpreadProperty == AllowSpreadProperty::No) {
          error(
              start, "Spreading a type is only allowed inside an object type");
        }
        auto optType = parseTypeAnnotationFlow();
        if (!optType)
          return false;
        properties.push_back(*setLocation(
            start,
            getPrevTokenEndLoc(),
            new (context_) ESTree::ObjectTypeSpreadPropertyNode(*optType)));
      }
    } else {
      if (!parsePropertyTypeAnnotationFlow(
              allowProtoProperty,
              allowStaticProperty,
              properties,
              indexers,
              callProperties,
              internalSlots))
        return false;
    }

    if (check(TokenKind::comma, TokenKind::semi)) {
      advance(JSLexer::GrammarContext::Type);
    } else if (check(TokenKind::r_brace, TokenKind::piper_brace)) {
      return true;
    } else {
      errorExpected(
          {TokenKind::comma,
           TokenKind::semi,
           TokenKind::r_brace,
           TokenKind::piper_brace},
          "after property",
          "start of property",
          start);
      return false;
    }
  }

  return true;
}

bool JSParserImpl::parsePropertyTypeAnnotationFlow(
    AllowProtoProperty allowProtoProperty,
    AllowStaticProperty allowStaticProperty,
    ESTree::NodeList &properties,
    ESTree::NodeList &indexers,
    ESTree::NodeList &callProperties,
    ESTree::NodeList &internalSlots) {
  SMRange startRange = tok_->getSourceRange();
  SMLoc start = startRange.Start;

  ESTree::Node *variance = nullptr;
  bool isStatic = false;
  bool proto = false;

  if (check(protoIdent_)) {
    proto = true;
    advance(JSLexer::GrammarContext::Type);
  }

  if (!proto && (check(TokenKind::rw_static) || check(staticIdent_))) {
    isStatic = true;
    advance(JSLexer::GrammarContext::Type);
  }

  if (check(TokenKind::plus, TokenKind::minus)) {
    variance = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::VarianceNode(
            check(TokenKind::plus) ? plusIdent_ : minusIdent_));
    advance(JSLexer::GrammarContext::Type);
  }

  if (checkAndEat(TokenKind::l_square, JSLexer::GrammarContext::Type)) {
    if (checkAndEat(TokenKind::l_square, JSLexer::GrammarContext::Type)) {
      if (variance) {
        error(variance->getSourceRange(), "Unexpected variance sigil");
      }
      if (proto) {
        error(startRange, "invalid 'proto' modifier");
      }
      if (isStatic && allowStaticProperty == AllowStaticProperty::No) {
        error(startRange, "invalid 'static' modifier");
      }
      // Internal slot
      if (!check(TokenKind::identifier) && !tok_->isResWord()) {
        errorExpected(
            TokenKind::identifier,
            "in internal slot",
            "start of internal slot",
            start);
        return false;
      }
      ESTree::IdentifierNode *id = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::IdentifierNode(
              tok_->getResWordOrIdentifier(), nullptr, false));
      advance(JSLexer::GrammarContext::Type);

      if (!eat(
              TokenKind::r_square,
              JSLexer::GrammarContext::Type,
              "at end of internal slot",
              "start of internal slot",
              start))
        return false;
      if (!eat(
              TokenKind::r_square,
              JSLexer::GrammarContext::Type,
              "at end of internal slot",
              "start of internal slot",
              start))
        return false;

      bool optional = false;
      bool method = false;
      ESTree::Node *value = nullptr;

      if (check(TokenKind::less, TokenKind::l_paren)) {
        // Type params and method.
        method = true;
        ESTree::Node *typeParams = nullptr;
        if (check(TokenKind::less)) {
          auto optParams = parseTypeParamsFlow();
          if (!optParams)
            return false;
          typeParams = *optParams;
        }
        auto optMethodish = parseMethodishTypeAnnotationFlow(start, typeParams);
        if (!optMethodish)
          return false;
        value = *optMethodish;
      } else {
        // Standard type annotation.
        optional =
            checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type);
        if (!eat(
                TokenKind::colon,
                JSLexer::GrammarContext::Type,
                "in type annotation",
                "start of annotation",
                start))
          return false;
        auto optValue = parseTypeAnnotationFlow();
        if (!optValue)
          return false;
        value = *optValue;
      }

      assert(value && "value must be set by a branch");
      internalSlots.push_back(*setLocation(
          start,
          getPrevTokenEndLoc(),
          new (context_) ESTree::ObjectTypeInternalSlotNode(
              id, value, optional, isStatic, method)));
    } else {
      // Indexer or Mapped Type
      // We can have
      // [ Identifier : TypeAnnotation ]
      //   ^
      // or
      // [ TypeAnnotation ]
      //   ^
      // or
      // [ TypeParameter in TypeAnnotation ]
      //   ^
      // Because we cannot differentiate without looking ahead for the `in`
      // or `:`, we call `parseTypeAnnotation`, check for the next token
      // and then convert the TypeAnnotation to the appropriate node.
      auto optLeft = parseTypeAnnotationBeforeColonFlow();
      if (!optLeft)
        return false;
      ESTree::Node *left = *optLeft;

      if (checkAndEat(TokenKind::rw_in, JSLexer::GrammarContext::Type)) {
        auto optProp = parseTypeMappedTypePropertyFlow(start, left, variance);
        if (!optProp)
          return false;
        properties.push_back(**optProp);
      } else {
        auto optIndexer =
            parseTypeIndexerPropertyFlow(start, left, variance, isStatic);
        if (!optIndexer)
          return false;
        indexers.push_back(**optIndexer);
      }

      if (proto) {
        error(startRange, "invalid 'proto' modifier");
      }
      if (isStatic && allowStaticProperty == AllowStaticProperty::No) {
        error(startRange, "invalid 'static' modifier");
      }
    }
    return true;
  }

  ESTree::Node *key = nullptr;

  if (check(TokenKind::less, TokenKind::l_paren)) {
    if ((isStatic && allowStaticProperty == AllowStaticProperty::No) ||
        (proto && allowProtoProperty == AllowProtoProperty::No)) {
      key = setLocation(
          startRange,
          startRange,
          new (context_) ESTree::IdentifierNode(
              isStatic ? staticIdent_ : protoIdent_, nullptr, false));
      isStatic = false;
      proto = false;
      if (variance) {
        error(variance->getSourceRange(), "Unexpected variance sigil");
      }
      auto optProp = parseMethodTypePropertyFlow(start, isStatic, key);
      if (!optProp)
        return false;
      properties.push_back(**optProp);
      return true;
    }
    if (variance != nullptr) {
      error(
          variance->getSourceRange(),
          "call property must not specify variance");
    }
    if (proto) {
      error(startRange, "invalid 'proto' modifier");
    }
    auto optCall = parseTypeCallPropertyFlow(start, isStatic);
    if (!optCall)
      return false;
    callProperties.push_back(**optCall);
    return true;
  }

  if ((isStatic || proto) && check(TokenKind::colon, TokenKind::question)) {
    if (variance) {
      error(variance->getSourceRange(), "Unexpected variance sigil");
    }
    key = setLocation(
        startRange,
        startRange,
        new (context_) ESTree::IdentifierNode(
            isStatic ? staticIdent_ : protoIdent_, nullptr, false));
    isStatic = false;
    proto = false;
    auto optProp = parseTypePropertyFlow(start, variance, isStatic, proto, key);
    if (!optProp)
      return false;
    properties.push_back(**optProp);
    return true;
  }

  auto optKey = parsePropertyName();
  if (!optKey)
    return false;
  key = *optKey;

  if (check(TokenKind::less, TokenKind::l_paren)) {
    if (variance) {
      error(variance->getSourceRange(), "Unexpected variance sigil");
    }
    if (proto) {
      error(startRange, "invalid 'proto' modifier");
    }
    if (isStatic && allowStaticProperty == AllowStaticProperty::No) {
      error(startRange, "invalid 'static' modifier");
    }
    auto optProp = parseMethodTypePropertyFlow(start, isStatic, key);
    if (!optProp)
      return false;
    properties.push_back(**optProp);
    return true;
  }

  if (check(TokenKind::colon, TokenKind::question)) {
    if (proto && allowProtoProperty == AllowProtoProperty::No) {
      error(startRange, "invalid 'proto' modifier");
    }
    if (isStatic && allowStaticProperty == AllowStaticProperty::No) {
      error(startRange, "invalid 'static' modifier");
    }
    auto optProp = parseTypePropertyFlow(start, variance, isStatic, proto, key);
    if (!optProp)
      return false;
    properties.push_back(**optProp);
    return true;
  }

  if (auto *ident = dyn_cast<ESTree::IdentifierNode>(key)) {
    if (ident->_name == getIdent_ || ident->_name == setIdent_) {
      if (variance != nullptr) {
        error(
            variance->getSourceRange(),
            "accessor property must not specify variance");
      }
      if (proto) {
        error(startRange, "invalid 'proto' modifier");
      }
      if (isStatic && allowStaticProperty == AllowStaticProperty::No) {
        error(startRange, "invalid 'static' modifier");
      }
      auto optKey = parsePropertyName();
      if (!optKey)
        return false;
      key = *optKey;
      auto optGetSet = parseGetOrSetTypePropertyFlow(
          start, isStatic, ident->_name == getIdent_, key);
      if (!optGetSet)
        return false;
      properties.push_back(**optGetSet);
      return true;
    }
  }

  errorExpected(
      {TokenKind::colon, TokenKind::question},
      "in property type annotation",
      "start of properties",
      start);
  return false;
}

Optional<ESTree::Node *> JSParserImpl::parseTypePropertyFlow(
    SMLoc start,
    ESTree::Node *variance,
    bool isStatic,
    bool proto,
    ESTree::Node *key) {
  assert(check(TokenKind::colon, TokenKind::question));

  bool optional =
      checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type);
  if (!eat(
          TokenKind::colon,
          JSLexer::GrammarContext::Type,
          "in type property",
          "start of property",
          start))
    return None;

  auto optValue = parseTypeAnnotationFlow();
  if (!optValue)
    return None;
  ESTree::Node *value = *optValue;

  bool method = false;
  UniqueString *kind = initIdent_;

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ObjectTypePropertyNode(
          key, value, method, optional, isStatic, proto, variance, kind));
}

Optional<ESTree::Node *> JSParserImpl::parseMethodTypePropertyFlow(
    SMLoc start,
    bool isStatic,
    ESTree::Node *key) {
  assert(check(TokenKind::less, TokenKind::l_paren));

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optTypeParams = parseTypeParamsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  auto optValue = parseMethodishTypeAnnotationFlow(start, typeParams);
  if (!optValue)
    return None;
  ESTree::Node *value = *optValue;

  bool method = true;
  bool optional = false;
  bool proto = false;
  UniqueString *kind = initIdent_;

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ObjectTypePropertyNode(
          key,
          value,
          method,
          optional,
          isStatic,
          proto,
          /* variance */ nullptr,
          kind));
}

Optional<ESTree::Node *> JSParserImpl::parseGetOrSetTypePropertyFlow(
    SMLoc start,
    bool isStatic,
    bool isGetter,
    ESTree::Node *key) {
  auto optValue = parseMethodishTypeAnnotationFlow(start, nullptr);
  if (!optValue)
    return None;

  ESTree::FunctionTypeAnnotationNode *value = *optValue;
  bool method = false;
  bool optional = false;
  bool proto = false;
  ESTree::Node *variance = nullptr;
  UniqueString *kind = isGetter ? getIdent_ : setIdent_;

  // Check the number of parameters, but we can continue parsing anyway.
  if (isGetter) {
    if (value->_params.size() != 0) {
      error(value->getSourceRange(), "Getter must have 0 parameters");
    }
  } else {
    if (value->_params.size() != 1) {
      error(value->getSourceRange(), "Setter must have 1 parameter");
    }
  }

  if (value->_this) {
    error(
        value->_this->getSourceRange(),
        "Accessors must not have 'this' annotations");
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ObjectTypePropertyNode(
          key, value, method, optional, isStatic, proto, variance, kind));
}

Optional<ESTree::Node *> JSParserImpl::parseTypeMappedTypePropertyFlow(
    SMLoc start,
    ESTree::Node *left,
    ESTree::Node *variance) {
  auto idOpt = reparseTypeAnnotationAsIdFlow(left);
  if (!idOpt)
    return None;
  UniqueString *id = *idOpt;
  ESTree::Node *keyTparam = setLocation(
      left,
      left,
      new (context_) ESTree::TypeParameterNode(
          id, false, nullptr, nullptr, nullptr, false));

  auto optSourceType = parseTypeAnnotationFlow();
  if (!optSourceType)
    return None;

  if (!eat(
          TokenKind::r_square,
          JSLexer::GrammarContext::Type,
          "in mapped type",
          "start of mapped type",
          start))
    return None;

  UniqueString *optional = nullptr;
  if (checkAndEat(TokenKind::plus, JSLexer::GrammarContext::Type)) {
    if (!eat(
            TokenKind::question,
            JSLexer::GrammarContext::Type,
            "in mapped type",
            "start of mapped type",
            start))
      return None;

    optional = mappedTypePlusOptionalIdent_;
  } else if (checkAndEat(TokenKind::minus, JSLexer::GrammarContext::Type)) {
    if (!eat(
            TokenKind::question,
            JSLexer::GrammarContext::Type,
            "in mapped type",
            "start of mapped type",
            start))
      return None;

    optional = mappedTypeMinusOptionalIdent_;
  } else if (checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type)) {
    optional = mappedTypeOptionalIdent_;
  }

  if (!eat(
          TokenKind::colon,
          JSLexer::GrammarContext::Type,
          "in mapped type",
          "start of mapped type",
          start))
    return None;

  auto optPropType = parseTypeAnnotationFlow();
  if (!optPropType)
    return None;

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ObjectTypeMappedTypePropertyNode(
          keyTparam, *optPropType, *optSourceType, variance, optional));
}

Optional<ESTree::Node *> JSParserImpl::parseTypeIndexerPropertyFlow(
    SMLoc start,
    ESTree::Node *left,
    ESTree::Node *variance,
    bool isStatic) {
  ESTree::IdentifierNode *id = nullptr;
  ESTree::Node *key = nullptr;

  if (checkAndEat(TokenKind::colon, JSLexer::GrammarContext::Type)) {
    auto optId = reparseTypeAnnotationAsIdentifierFlow(left);
    if (!optId)
      return None;
    id = *optId;
    auto optKey = parseTypeAnnotationFlow();
    if (!optKey)
      return None;
    key = *optKey;
  } else {
    key = left;
  }

  if (!eat(
          TokenKind::r_square,
          JSLexer::GrammarContext::Type,
          "in indexer",
          "start of indexer",
          start))
    return None;

  if (!eat(
          TokenKind::colon,
          JSLexer::GrammarContext::Type,
          "in indexer",
          "start of indexer",
          start))
    return None;

  auto optValue = parseTypeAnnotationFlow();
  if (!optValue)
    return None;
  ESTree::Node *value = *optValue;

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::ObjectTypeIndexerNode(id, key, value, isStatic, variance));
}

Optional<ESTree::Node *> JSParserImpl::parseTypeCallPropertyFlow(
    SMLoc start,
    bool isStatic) {
  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optTypeParams = parseTypeParamsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }
  auto optValue = parseMethodishTypeAnnotationFlow(start, typeParams);
  if (!optValue)
    return None;
  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ObjectTypeCallPropertyNode(*optValue, isStatic));
}

Optional<ESTree::Node *> JSParserImpl::parseTypeParamsFlow() {
  assert(check(TokenKind::less));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  ESTree::NodeList params{};

  do {
    auto optType = parseTypeParamFlow();
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
      new (context_) ESTree::TypeParameterDeclarationNode(std::move(params)));
}

Optional<ESTree::Node *> JSParserImpl::parseTypeParamFlow() {
  SMLoc start = tok_->getStartLoc();
  bool isConst = false;
  ESTree::Node *variance = nullptr;
  if (check(TokenKind::rw_const)) {
    isConst = true;
    advance(JSLexer::GrammarContext::Type);
  }

  if (check(TokenKind::plus, TokenKind::minus)) {
    variance = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::VarianceNode(
            check(TokenKind::plus) ? plusIdent_ : minusIdent_));
    advance(JSLexer::GrammarContext::Type);
  }

  if (!need(TokenKind::identifier, "in type parameter", nullptr, {}))
    return None;
  UniqueString *name = tok_->getIdentifier();
  advance(JSLexer::GrammarContext::Type);

  ESTree::Node *bound = nullptr;
  bool usesExtendsBound = false;
  if (check(TokenKind::colon)) {
    SMLoc boundStart = advance(JSLexer::GrammarContext::Type).Start;
    auto optType = parseTypeAnnotationFlow();
    if (!optType)
      return None;
    bound = setLocation(
        boundStart,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TypeAnnotationNode(*optType));
  } else if (check(TokenKind::rw_extends)) {
    usesExtendsBound = true;
    SMLoc boundStart = advance(JSLexer::GrammarContext::Type).Start;
    auto optType = parseTypeAnnotationFlow();
    if (!optType)
      return None;
    bound = setLocation(
        boundStart,
        getPrevTokenEndLoc(),
        new (context_) ESTree::TypeAnnotationNode(*optType));
  }

  ESTree::Node *initializer = nullptr;
  if (checkAndEat(TokenKind::equal, JSLexer::GrammarContext::Type)) {
    auto optInit = parseTypeAnnotationFlow();
    if (!optInit)
      return None;
    initializer = *optInit;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::TypeParameterNode(
          name, isConst, bound, variance, initializer, usesExtendsBound));
}

Optional<ESTree::Node *> JSParserImpl::parseTypeArgsFlow() {
  assert(check(TokenKind::less));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  ESTree::NodeList params{};

  while (!check(TokenKind::greater)) {
    auto optType = parseTypeAnnotationFlow();
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
      new (context_) ESTree::TypeParameterInstantiationNode(std::move(params)));
}

Optional<ESTree::FunctionTypeAnnotationNode *>
JSParserImpl::parseMethodishTypeAnnotationFlow(
    SMLoc start,
    ESTree::Node *typeParams) {
  ESTree::NodeList params{};
  ESTree::Node *thisConstraint = nullptr;

  if (!need(TokenKind::l_paren, "at start of parameters", nullptr, {}))
    return None;
  auto optRest =
      parseFunctionTypeAnnotationParamsFlow(params, thisConstraint, false);
  if (!optRest)
    return None;

  if (!eat(
          TokenKind::colon,
          JSLexer::GrammarContext::Type,
          "in function type annotation",
          "start of annotation",
          start))
    return None;

  auto optReturn = parseReturnTypeAnnotationFlow();
  if (!optReturn)
    return None;

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::FunctionTypeAnnotationNode(
          std::move(params), thisConstraint, *optReturn, *optRest, typeParams));
}

Optional<ESTree::FunctionTypeParamNode *>
JSParserImpl::parseFunctionTypeAnnotationParamsFlow(
    ESTree::NodeList &params,
    ESTree::NodePtr &thisConstraint,
    bool hook) {
  assert(check(TokenKind::l_paren));
  SMLoc start = advance(JSLexer::GrammarContext::Type).Start;

  ESTree::FunctionTypeParamNode *rest = nullptr;
  thisConstraint = nullptr;

  if (check(TokenKind::rw_this) && !hook) {
    OptValue<TokenKind> optNext = lexer_.lookahead1(None);
    if (optNext.hasValue() && *optNext == TokenKind::colon) {
      SMLoc thisStart = advance(JSLexer::GrammarContext::Type).Start;
      advance(JSLexer::GrammarContext::Type);
      auto optType = parseTypeAnnotationFlow();
      if (!optType)
        return None;
      ESTree::Node *typeAnnotation = *optType;

      thisConstraint = setLocation(
          thisStart,
          getPrevTokenEndLoc(),
          new (context_) ESTree::FunctionTypeParamNode(
              /* name */ nullptr, typeAnnotation, /* optional */ false));
      checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type);
    } else if (optNext.hasValue() && *optNext == TokenKind::question) {
      error(tok_->getSourceRange(), "'this' constraint may not be optional");
    }
  }

  while (!check(TokenKind::r_paren)) {
    bool isRest =
        checkAndEat(TokenKind::dotdotdot, JSLexer::GrammarContext::Type);

    auto optParam = hook ? parseHookTypeAnnotationParamFlow()
                         : parseFunctionTypeAnnotationParamFlow();
    if (!optParam)
      return None;

    if (isRest) {
      // Rest param must be the last param.
      rest = *optParam;
      checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type);
      break;
    } else {
      params.push_back(**optParam);
      if (!checkAndEat(TokenKind::comma, JSLexer::GrammarContext::Type)) {
        break;
      }
    }
  }

  if (!eat(
          TokenKind::r_paren,
          JSLexer::GrammarContext::Type,
          "at end of function annotation parameters",
          "start of parameters",
          start))
    return None;

  return rest;
}

Optional<ESTree::FunctionTypeParamNode *>
JSParserImpl::parseHookTypeAnnotationParamFlow() {
  if (check(TokenKind::rw_this)) {
    OptValue<TokenKind> optNext = lexer_.lookahead1(None);
    if (optNext.hasValue() && *optNext == TokenKind::colon) {
      error(tok_->getSourceRange(), "hooks do not support 'this' constraints");
    }
  }
  return parseFunctionTypeAnnotationParamFlow();
}

Optional<ESTree::FunctionTypeParamNode *>
JSParserImpl::parseFunctionTypeAnnotationParamFlow() {
  SMLoc start = tok_->getStartLoc();

  if (check(TokenKind::rw_this)) {
    OptValue<TokenKind> optNext = lexer_.lookahead1(None);
    if (optNext.hasValue() && *optNext == TokenKind::colon) {
      error(
          tok_->getSourceRange(),
          "'this' constraint must be the first parameter");
    }
  }

  auto optLeft = parseTypeAnnotationBeforeColonFlow();
  if (!optLeft)
    return None;

  ESTree::Node *name = nullptr;
  ESTree::Node *typeAnnotation = nullptr;
  bool optional = false;

  if (check(TokenKind::colon, TokenKind::question)) {
    // The node is actually supposed to be an identifier, not a TypeAnnotation.
    auto optName = reparseTypeAnnotationAsIdentifierFlow(*optLeft);
    if (!optName)
      return None;
    name = *optName;
    optional = checkAndEat(TokenKind::question, JSLexer::GrammarContext::Type);
    if (!eat(
            TokenKind::colon,
            JSLexer::GrammarContext::Type,
            "in function parameter type annotation",
            "start of parameter",
            start))
      return None;
    auto optType = parseTypeAnnotationFlow();
    if (!optType)
      return None;
    typeAnnotation = *optType;
  } else {
    typeAnnotation = *optLeft;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::FunctionTypeParamNode(name, typeAnnotation, optional));
}

Optional<ESTree::GenericTypeAnnotationNode *>
JSParserImpl::parseGenericTypeFlow() {
  assert(check(TokenKind::identifier) || tok_->isResWord());
  SMLoc start = tok_->getStartLoc();

  ESTree::Node *id = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::IdentifierNode(
          tok_->getResWordOrIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  while (checkAndEat(TokenKind::period, JSLexer::GrammarContext::Type)) {
    if (!check(TokenKind::identifier) && !tok_->isResWord()) {
      errorExpected(
          TokenKind::identifier,
          "in qualified generic type name",
          "start of type name",
          start);
      return None;
    }
    ESTree::Node *next = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(
            tok_->getResWordOrIdentifier(), nullptr, false));
    advance(JSLexer::GrammarContext::Type);
    id = setLocation(
        id, next, new (context_) ESTree::QualifiedTypeIdentifierNode(id, next));
  }

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optTypeParams = parseTypeArgsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::GenericTypeAnnotationNode(id, typeParams));
}

Optional<ESTree::ClassImplementsNode *>
JSParserImpl::parseClassImplementsFlow() {
  assert(check(TokenKind::identifier));
  SMLoc start = tok_->getStartLoc();

  ESTree::Node *id = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  ESTree::Node *typeParams = nullptr;
  if (check(TokenKind::less)) {
    auto optTypeParams = parseTypeArgsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }

  return setLocation(
      start,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ClassImplementsNode(id, typeParams));
}

Optional<ESTree::Node *> JSParserImpl::parsePredicateFlow() {
  assert(check(checksIdent_));
  SMRange checksRng = advance(JSLexer::GrammarContext::Type);
  if (checkAndEat(TokenKind::l_paren, JSLexer::GrammarContext::AllowRegExp)) {
    auto optCond = parseConditionalExpression();
    if (!optCond)
      return None;
    SMLoc end = tok_->getEndLoc();
    if (!eat(
            TokenKind::r_paren,
            JSLexer::GrammarContext::Type,
            "in declared predicate",
            "start of predicate",
            checksRng.Start))
      return None;
    return setLocation(
        checksRng, end, new (context_) ESTree::DeclaredPredicateNode(*optCond));
  }
  return setLocation(
      checksRng, checksRng, new (context_) ESTree::InferredPredicateNode());
}

Optional<UniqueString *> JSParserImpl::reparseTypeAnnotationAsIdFlow(
    ESTree::Node *typeAnnotation) {
  UniqueString *id = nullptr;
  if (isa<ESTree::AnyTypeAnnotationNode>(typeAnnotation)) {
    id = anyIdent_;
  } else if (isa<ESTree::EmptyTypeAnnotationNode>(typeAnnotation)) {
    id = emptyIdent_;
  } else if (isa<ESTree::BooleanTypeAnnotationNode>(typeAnnotation)) {
    id = booleanIdent_;
  } else if (isa<ESTree::NumberTypeAnnotationNode>(typeAnnotation)) {
    id = numberIdent_;
  } else if (isa<ESTree::StringTypeAnnotationNode>(typeAnnotation)) {
    id = stringIdent_;
  } else if (isa<ESTree::SymbolTypeAnnotationNode>(typeAnnotation)) {
    id = symbolIdent_;
  } else if (isa<ESTree::NullLiteralTypeAnnotationNode>(typeAnnotation)) {
    id = nullIdent_;
  } else if (
      auto *generic =
          dyn_cast<ESTree::GenericTypeAnnotationNode>(typeAnnotation)) {
    if (!generic->_typeParameters) {
      if (auto *genericId = dyn_cast<ESTree::IdentifierNode>(generic->_id)) {
        id = genericId->_name;
      }
    }
  }

  if (!id) {
    error(typeAnnotation->getSourceRange(), "identifier expected");
    return None;
  }

  return id;
}

Optional<ESTree::IdentifierNode *>
JSParserImpl::reparseTypeAnnotationAsIdentifierFlow(
    ESTree::Node *typeAnnotation) {
  auto idOpt = reparseTypeAnnotationAsIdFlow(typeAnnotation);
  if (!idOpt)
    return None;
  UniqueString *id = *idOpt;
  return setLocation(
      typeAnnotation,
      typeAnnotation,
      new (context_) ESTree::IdentifierNode(id, nullptr, false));
}

Optional<ESTree::Node *> JSParserImpl::parseEnumDeclarationFlow(
    SMLoc start,
    bool declare) {
  assert(check(TokenKind::rw_enum));
  advance();

  if (!check(TokenKind::identifier)) {
    errorExpected(
        TokenKind::identifier,
        "in enum declaration",
        "start of declaration",
        start);
    return None;
  }
  ESTree::Node *id = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance(JSLexer::GrammarContext::Type);

  OptValue<EnumKind> optKind = llvh::None;
  Optional<SMLoc> explicitTypeStart = None;
  if (check(ofIdent_)) {
    explicitTypeStart = advance().Start;

    if (checkAndEat(stringIdent_)) {
      optKind = EnumKind::String;
    } else if (checkAndEat(numberIdent_)) {
      optKind = EnumKind::Number;
    } else if (checkAndEat(bigintIdent_)) {
      optKind = EnumKind::BigInt;
    } else if (checkAndEat(booleanIdent_)) {
      optKind = EnumKind::Boolean;
    } else if (checkAndEat(symbolIdent_)) {
      optKind = EnumKind::Symbol;
    }
  }

  if (!need(
          TokenKind::l_brace,
          "in enum declaration",
          "start of declaration",
          start))
    return None;

  auto optBody = parseEnumBodyFlow(optKind, explicitTypeStart);
  if (!optBody)
    return None;

  if (declare)
    return setLocation(
        start, *optBody, new (context_) ESTree::DeclareEnumNode(id, *optBody));
  return setLocation(
      start,
      *optBody,
      new (context_) ESTree::EnumDeclarationNode(id, *optBody));
}

Optional<ESTree::Node *> JSParserImpl::parseEnumBodyFlow(
    OptValue<EnumKind> optKind,
    Optional<SMLoc> explicitTypeStart) {
  assert(check(TokenKind::l_brace));
  SMLoc start = advance().Start;

  ESTree::NodeList members{};
  bool hasUnknownMembers = false;
  while (!check(TokenKind::r_brace)) {
    if (check(TokenKind::dotdotdot)) {
      SMLoc dotdotdotLoc = advance(JSLexer::GrammarContext::Type).Start;
      if (!check(TokenKind::r_brace)) {
        error(
            dotdotdotLoc,
            "The `...` must come after all enum members. "
            "Move it to the end of the enum body.");
        return None;
      }
      hasUnknownMembers = true;
      break;
    }
    if (!need(
            TokenKind::identifier,
            "in enum declaration",
            "start of declaration",
            start))
      return None;

    auto optMember = parseEnumMemberFlow();
    if (!optMember)
      return None;
    ESTree::Node *member = *optMember;
    OptValue<EnumKind> optMemberKind = getMemberEnumKindFlow(member);

    if (optKind.hasValue()) {
      // We've already figured out the type of the enum, so ensure that the
      // new member is compatible with this.
      if (optMemberKind.hasValue()) {
        if (*optKind != *optMemberKind) {
          error(
              member->getSourceRange(),
              llvh::Twine("cannot use ") + enumKindStrFlow(*optMemberKind) +
                  " initializer in " + enumKindStrFlow(*optKind) + " enum");
          sm_.note(start, "start of enum body", Subsystem::Parser);
          return None;
        }
      }
    } else {
      optKind = optMemberKind;
    }

    members.push_back(*member);
    if (!checkAndEat(TokenKind::comma))
      break;
  }

  if (!members.empty()) {
    // Ensure that enum members use initializers consistently.
    // This is vacuously true when `members` is empty, so just make sure
    // all members use initializers iff the first member does.
    bool usesInitializers =
        !isa<ESTree::EnumDefaultedMemberNode>(members.front());
    for (const ESTree::Node &member : members) {
      if (usesInitializers != !isa<ESTree::EnumDefaultedMemberNode>(member)) {
        error(
            member.getSourceRange(),
            "enum members need to consistently either all use initializers, "
            "or use no initializers");
        sm_.note(
            members.front().getSourceRange(),
            "first enum member",
            Subsystem::Parser);
        return None;
      }
    }

    if (!usesInitializers) {
      // It's only legal to use defaulted members for string and symbol enums,
      // because other kinds of enums can't infer values from names.
      if (optKind.hasValue() && *optKind != EnumKind::String &&
          *optKind != EnumKind::Symbol) {
        error(start, "number and boolean enums must use initializers");
        return None;
      }
    }
  }

  SMLoc end = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_brace,
          JSLexer::GrammarContext::AllowRegExp,
          "in enum body",
          "start of body",
          start))
    return None;

  bool hasExplicitType = explicitTypeStart.hasValue();
  if (hasExplicitType) {
    start = *explicitTypeStart;
  }

  if (!optKind.hasValue()) {
    return setLocation(
        start,
        end,
        new (context_) ESTree::EnumStringBodyNode(
            std::move(members), hasExplicitType, hasUnknownMembers));
  }

  // There are different node kinds per enum kind.
  switch (*optKind) {
    case EnumKind::String:
      return setLocation(
          start,
          end,
          new (context_) ESTree::EnumStringBodyNode(
              std::move(members), hasExplicitType, hasUnknownMembers));
    case EnumKind::Number:
      return setLocation(
          start,
          end,
          new (context_) ESTree::EnumNumberBodyNode(
              std::move(members), hasExplicitType, hasUnknownMembers));
    case EnumKind::BigInt:
      return setLocation(
          start,
          end,
          new (context_) ESTree::EnumBigIntBodyNode(
              std::move(members), hasExplicitType, hasUnknownMembers));
    case EnumKind::Boolean:
      return setLocation(
          start,
          end,
          new (context_) ESTree::EnumBooleanBodyNode(
              std::move(members), hasExplicitType, hasUnknownMembers));
    case EnumKind::Symbol:
      assert(
          hasExplicitType && "symbol enums can only be made via explicit type");
      return setLocation(
          start,
          end,
          new (context_) ESTree::EnumSymbolBodyNode(
              std::move(members), hasUnknownMembers));
  }
  llvm_unreachable("No other kind of enum");
}

Optional<ESTree::Node *> JSParserImpl::parseEnumMemberFlow() {
  assert(check(TokenKind::identifier));
  ESTree::Node *id = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance();

  ESTree::Node *member = nullptr;
  if (checkAndEat(TokenKind::equal)) {
    // Parse initializer.
    if (check(TokenKind::rw_true, TokenKind::rw_false)) {
      ESTree::Node *init = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::BooleanLiteralNode(check(TokenKind::rw_true)));
      member = setLocation(
          id, tok_, new (context_) ESTree::EnumBooleanMemberNode(id, init));
    } else if (check(TokenKind::string_literal)) {
      ESTree::Node *init = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
      member = setLocation(
          id, tok_, new (context_) ESTree::EnumStringMemberNode(id, init));
    } else if (check(TokenKind::minus)) {
      SMLoc start = tok_->getStartLoc();
      advance();
      if (check(TokenKind::numeric_literal)) {
        // Negate the literal.
        double value = -tok_->getNumericLiteral();
        ESTree::Node *init = setLocation(
            start, tok_, new (context_) ESTree::NumericLiteralNode(value));
        member = setLocation(
            id, tok_, new (context_) ESTree::EnumNumberMemberNode(id, init));
      } else {
        errorExpected(
            TokenKind::numeric_literal,
            "in negated enum member initializer",
            "start of negated enum member",
            id->getStartLoc());
        return None;
      }
    } else if (check(TokenKind::numeric_literal)) {
      ESTree::Node *init = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::NumericLiteralNode(tok_->getNumericLiteral()));
      member = setLocation(
          id, tok_, new (context_) ESTree::EnumNumberMemberNode(id, init));
    } else if (check(TokenKind::bigint_literal)) {
      ESTree::Node *init = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::BigIntLiteralNode(tok_->getBigIntLiteral()));
      member = setLocation(
          id, tok_, new (context_) ESTree::EnumBigIntMemberNode(id, init));
    } else {
      errorExpected(
          {TokenKind::rw_true,
           TokenKind::rw_false,
           TokenKind::string_literal,
           TokenKind::numeric_literal,
           TokenKind::bigint_literal},
          "in enum member initializer",
          "start of enum member",
          id->getStartLoc());
      return None;
    }
    advance();
  } else {
    member =
        setLocation(id, id, new (context_) ESTree::EnumDefaultedMemberNode(id));
  }

  assert(member != nullptr && "member must have been parsed");
  return member;
}

#endif

} // namespace detail
} // namespace parser
} // namespace hermes
