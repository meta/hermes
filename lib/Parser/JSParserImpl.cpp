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

using llvh::cast;
using llvh::dyn_cast;
using llvh::isa;

namespace hermes {
namespace parser {
namespace detail {

JSParserImpl::JSParserImpl(
    Context &context,
    std::unique_ptr<llvh::MemoryBuffer> input)
    : context_(context),
      sm_(context.getSourceErrorManager()),
      lexer_(
          std::move(input),
          context.getSourceErrorManager(),
          context.getAllocator(),
          &context.getStringTable(),
          context.isStrictMode()),
      pass_(FullParse) {
  initializeIdentifiers();
}

JSParserImpl::JSParserImpl(Context &context, uint32_t bufferId, ParserPass pass)
    : context_(context),
      sm_(context.getSourceErrorManager()),
      lexer_(
          bufferId,
          context.getSourceErrorManager(),
          context.getAllocator(),
          &context.getStringTable(),
          context.isStrictMode()),
      pass_(pass) {
  preParsed_ = context.getPreParsedBufferInfo(bufferId);
  initializeIdentifiers();
}

void JSParserImpl::initializeIdentifiers() {
  getIdent_ = lexer_.getIdentifier("get");
  setIdent_ = lexer_.getIdentifier("set");
  initIdent_ = lexer_.getIdentifier("init");
  useStrictIdent_ = lexer_.getIdentifier("use strict");
  showSourceIdent_ = lexer_.getIdentifier("show source");
  hideSourceIdent_ = lexer_.getIdentifier("hide source");
  sensitiveIdent_ = lexer_.getIdentifier("sensitive");
  useStaticBuiltinIdent_ = lexer_.getIdentifier("use static builtin");
  letIdent_ = lexer_.getIdentifier("let");
  ofIdent_ = lexer_.getIdentifier("of");
  fromIdent_ = lexer_.getIdentifier("from");
  asIdent_ = lexer_.getIdentifier("as");
  implementsIdent_ = lexer_.getIdentifier("implements");
  interfaceIdent_ = lexer_.getIdentifier("interface");
  packageIdent_ = lexer_.getIdentifier("package");
  privateIdent_ = lexer_.getIdentifier("private");
  protectedIdent_ = lexer_.getIdentifier("protected");
  publicIdent_ = lexer_.getIdentifier("public");
  staticIdent_ = lexer_.getIdentifier("static");
  methodIdent_ = lexer_.getIdentifier("method");
  constructorIdent_ = lexer_.getIdentifier("constructor");
  yieldIdent_ = lexer_.getIdentifier("yield");
  newIdent_ = lexer_.getIdentifier("new");
  targetIdent_ = lexer_.getIdentifier("target");
  importIdent_ = lexer_.getIdentifier("import");
  metaIdent_ = lexer_.getIdentifier("meta");
  valueIdent_ = lexer_.getIdentifier("value");
  typeIdent_ = lexer_.getIdentifier("type");
  asyncIdent_ = lexer_.getIdentifier("async");
  awaitIdent_ = lexer_.getIdentifier("await");
  assertIdent_ = lexer_.getIdentifier("assert");

#if HERMES_PARSE_FLOW

  typeofIdent_ = lexer_.getIdentifier("typeof");
  keyofIdent_ = lexer_.getIdentifier("keyof");
  declareIdent_ = lexer_.getIdentifier("declare");
  protoIdent_ = lexer_.getIdentifier("proto");
  opaqueIdent_ = lexer_.getIdentifier("opaque");
  plusIdent_ = lexer_.getIdentifier("plus");
  minusIdent_ = lexer_.getIdentifier("minus");
  moduleIdent_ = lexer_.getIdentifier("module");
  exportsIdent_ = lexer_.getIdentifier("exports");
  esIdent_ = lexer_.getIdentifier("ES");
  commonJSIdent_ = lexer_.getIdentifier("CommonJS");
  mixinsIdent_ = lexer_.getIdentifier("mixins");
  thisIdent_ = lexer_.getIdentifier("this");

  anyIdent_ = lexer_.getIdentifier("any");
  mixedIdent_ = lexer_.getIdentifier("mixed");
  emptyIdent_ = lexer_.getIdentifier("empty");
  booleanIdent_ = lexer_.getIdentifier("boolean");
  boolIdent_ = lexer_.getIdentifier("bool");
  numberIdent_ = lexer_.getIdentifier("number");
  stringIdent_ = lexer_.getIdentifier("string");
  voidIdent_ = lexer_.getIdentifier("void");
  nullIdent_ = lexer_.getIdentifier("null");
  symbolIdent_ = lexer_.getIdentifier("symbol");
  bigintIdent_ = lexer_.getIdentifier("bigint");

  mappedTypeOptionalIdent_ = lexer_.getIdentifier("Optional");
  mappedTypePlusOptionalIdent_ = lexer_.getIdentifier("PlusOptional");
  mappedTypeMinusOptionalIdent_ = lexer_.getIdentifier("MinusOptional");

  checksIdent_ = lexer_.getIdentifier("%checks");
  assertsIdent_ = lexer_.getIdentifier("asserts");
  impliesIdent_ = lexer_.getIdentifier("implies");

  // Flow Component syntax
  componentIdent_ = lexer_.getIdentifier("component");
  rendersIdent_ = lexer_.getIdentifier("renders");
  rendersMaybeOperator_ = lexer_.getIdentifier("renders?");
  rendersStarOperator_ = lexer_.getIdentifier("renders*");
  hookIdent_ = lexer_.getIdentifier("hook");

  // Flow match expressions and statements
  matchIdent_ = lexer_.getIdentifier("match");
  underscoreIdent_ = lexer_.getIdentifier("_");
#endif

#if HERMES_PARSE_TS
  readonlyIdent_ = lexer_.getIdentifier("readonly");
  neverIdent_ = lexer_.getIdentifier("never");
  undefinedIdent_ = lexer_.getIdentifier("undefined");
  unknownIdent_ = lexer_.getIdentifier("unknown");
#endif

#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
  namespaceIdent_ = lexer_.getIdentifier("namespace");
  isIdent_ = lexer_.getIdentifier("is");
  inferIdent_ = lexer_.getIdentifier("infer");
  constIdent_ = lexer_.getIdentifier("const");
#endif

  // Generate the string representation of all tokens.
  for (unsigned i = 0; i != NUM_JS_TOKENS; ++i)
    tokenIdent_[i] = lexer_.getIdentifier(tokenKindStr((TokenKind)i));
}

Optional<ESTree::ProgramNode *> JSParserImpl::parse() {
  PerfSection parsing("Parsing JavaScript");
  tok_ = lexer_.advance();
  auto res = parseProgram();
  if (!res)
    return None;
  if (lexer_.getSourceMgr().getErrorCount() != 0)
    return None;
  return res.getValue();
}

void JSParserImpl::errorExpected(
    ArrayRef<TokenKind> toks,
    const char *where,
    const char *what,
    SMLoc whatLoc) {
  llvh::SmallString<4> str;
  llvh::raw_svector_ostream ss{str};

  for (unsigned i = 0; i < toks.size(); ++i) {
    // Insert a separator after the first token.
    if (i > 0) {
      // Use " or " instead of ", " before the last token.
      if (i == toks.size() - 1)
        ss << " or ";
      else
        ss << ", ";
    }
    ss << "'" << tokenKindStr(toks[i]) << "'";
  }

  ss << " expected";

  // Optionally append the 'where' description.
  if (where)
    ss << " " << where;

  SMLoc errorLoc = tok_->getStartLoc();
  SourceErrorManager::SourceCoords curCoords;
  SourceErrorManager::SourceCoords whatCoords;

  // If the location of 'what' is provided, find its and the error's source
  // coordinates.
  if (whatLoc.isValid()) {
    sm_.findBufferLineAndLoc(errorLoc, curCoords);
    sm_.findBufferLineAndLoc(whatLoc, whatCoords);
  }

  if (whatCoords.isSameSourceLineAs(curCoords)) {
    // If the what source coordinates are on the same line as the error, show
    // them both.
    sm_.error(
        errorLoc,
        SourceErrorManager::combineIntoRange(whatLoc, errorLoc),
        ss.str(),
        Subsystem::Parser);
  } else {
    sm_.error(errorLoc, ss.str(), Subsystem::Parser);

    if (what && whatCoords.isValid())
      sm_.note(whatLoc, what, Subsystem::Parser);
  }
}

bool JSParserImpl::need(
    TokenKind kind,
    const char *where,
    const char *what,
    SMLoc whatLoc) {
  if (tok_->getKind() == kind) {
    return true;
  }
  errorExpected(kind, where, what, whatLoc);
  return false;
}

bool JSParserImpl::eat(
    TokenKind kind,
    JSLexer::GrammarContext grammarContext,
    const char *where,
    const char *what,
    SMLoc whatLoc) {
  if (need(kind, where, what, whatLoc)) {
    advance(grammarContext);
    return true;
  }
  return false;
}

bool JSParserImpl::checkAndEat(
    TokenKind kind,
    JSLexer::GrammarContext grammarContext) {
  if (tok_->getKind() == kind) {
    advance(grammarContext);
    return true;
  }
  return false;
}

bool JSParserImpl::checkAndEat(
    UniqueString *ident,
    JSLexer::GrammarContext grammarContext) {
  if (check(ident)) {
    advance(grammarContext);
    return true;
  }
  return false;
}

bool JSParserImpl::checkAssign() const {
  return checkN(
      TokenKind::equal,
      TokenKind::starequal,
      TokenKind::slashequal,
      TokenKind::percentequal,
      TokenKind::plusequal,
      TokenKind::minusequal,
      TokenKind::lesslessequal,
      TokenKind::greatergreaterequal,
      TokenKind::greatergreatergreaterequal,
      TokenKind::starstarequal,
      TokenKind::pipepipeequal,
      TokenKind::ampampequal,
      TokenKind::questionquestionequal,
      TokenKind::ampequal,
      TokenKind::caretequal,
      TokenKind::pipeequal);
}

bool JSParserImpl::checkEndAssignmentExpression(
    OfEndsAssignment ofEndsAssignment) const {
  return checkN(
             TokenKind::rw_in,
             TokenKind::r_paren,
             TokenKind::r_brace,
             TokenKind::r_square,
             TokenKind::comma,
             TokenKind::semi,
             TokenKind::colon,
             TokenKind::eof) ||
      (ofEndsAssignment == OfEndsAssignment::Yes && check(ofIdent_)) ||
      lexer_.isNewLineBeforeCurrentToken();
}

bool JSParserImpl::checkAsyncFunction() {
  // async [no LineTerminator here] function
  // ^
  assert(
      check(asyncIdent_) && "check for async function must occur at 'async'");
  // Avoid passing TokenKind::rw_function here, because parseFunctionHelper
  // relies on seeing `async` in order to construct its AST node.
  // This function must also be idempotent to allow for branching based on its
  // result in parseStatementListItem without having to store another flag,
  // for example.
  OptValue<TokenKind> optNext = lexer_.lookahead1(llvh::None);
  return optNext.hasValue() && *optNext == TokenKind::rw_function;
}

bool JSParserImpl::eatSemi(bool optional) {
  if (tok_->getKind() == TokenKind::semi) {
    advance();
    return true;
  }

  if (tok_->getKind() == TokenKind::r_brace ||
      tok_->getKind() == TokenKind::eof ||
      lexer_.isNewLineBeforeCurrentToken()) {
    return true;
  }

  if (!optional)
    error(tok_->getStartLoc(), "';' expected");
  return false;
}

void JSParserImpl::processDirective(UniqueString *directive) {
  seenDirectives_.push_back(directive);
  if (directive == useStrictIdent_)
    setStrictMode(true);
  if (directive == useStaticBuiltinIdent_)
    setUseStaticBuiltin();
}

bool JSParserImpl::recursionDepthExceeded() {
  error(
      tok_->getStartLoc(),
      "Too many nested expressions/statements/declarations");
  return true;
}

Optional<ESTree::ProgramNode *> JSParserImpl::parseProgram() {
  SMLoc startLoc = tok_->getStartLoc();
  SaveStrictModeAndSeenDirectives saveStrictModeAndSeenDirectives{this};
  ESTree::NodeList stmtList;

  if (!parseStatementList(
          Param{}, TokenKind::eof, true, AllowImportExport::Yes, stmtList))
    return None;

  SMLoc endLoc = startLoc;
  if (!stmtList.empty()) {
    endLoc = stmtList.back().getEndLoc();
  }
  auto *program = setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::ProgramNode(std::move(stmtList)));
  return program;
}

Optional<ESTree::FunctionDeclarationNode *>
JSParserImpl::parseFunctionDeclaration(Param param, bool forceEagerly) {
  auto optRes = parseFunctionHelper(param, true, forceEagerly);
  if (!optRes)
    return None;
  return cast<ESTree::FunctionDeclarationNode>(*optRes);
}

Optional<ESTree::FunctionLikeNode *> JSParserImpl::parseFunctionHelper(
    Param param,
    bool isDeclaration,
    bool forceEagerly) {
  // function or async function
  assert(check(TokenKind::rw_function) || check(asyncIdent_));
  bool isAsync = check(asyncIdent_);

  SMLoc startLoc = advance().Start;

  if (isAsync) {
    // async function
    //       ^
    advance();
  }

  bool isGenerator = checkAndEat(TokenKind::star);

  // newParamYield setting per the grammar:
  // FunctionDeclaration: BindingIdentifier[?Yield, ?Await]
  // FunctionExpression: BindingIdentifier[~Yield, ~Await]
  // GeneratorFunctionDeclaration: BindingIdentifier[?Yield, ?Await]
  // GeneratorFunctionExpression: BindingIdentifier[+Yield, ~Await]
  // AsyncFunctionDeclaration: BindingIdentifier[?Yield, ?Await]
  // AsyncFunctionExpression: BindingIdentifier[+Yield, +Await]
  // AsyncGeneratorDeclaration: BindingIdentifier[?Yield, ?Await]
  // AsyncGeneratorExpression: BindingIdentifier[+Yield, +Await]
  bool nameParamYield = isDeclaration ? paramYield_ : isGenerator;
  llvh::SaveAndRestore<bool> saveNameParamYield(paramYield_, nameParamYield);
  bool nameParamAwait = isDeclaration ? paramAwait_ : isAsync;
  llvh::SaveAndRestore<bool> saveNameParamAwait(paramAwait_, nameParamAwait);

  // identifier
  auto optId = parseBindingIdentifier(Param{});
  // If this is a default function declaration, then we can match
  // [+Default] function ( FormalParameters ) { FunctionBody }
  // so the identifier is optional and we can make it nullptr.
  if (isDeclaration && !param.has(ParamDefault) && !optId) {
    errorExpected(
        TokenKind::identifier,
        "after 'function'",
        "location of 'function'",
        startLoc);
    return None;
  }

  ESTree::Node *typeParams = nullptr;

#if HERMES_PARSE_FLOW
  if (context_.getParseFlow() && check(TokenKind::less)) {
    auto optTypeParams = parseTypeParamsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }
#endif

#if HERMES_PARSE_TS
  if (context_.getParseTS() && check(TokenKind::less)) {
    auto optTypeParams = parseTSTypeParameters();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }
#endif

  // (
  if (!need(
          TokenKind::l_paren,
          "at start of function parameter list",
          isDeclaration ? "function declaration starts here"
                        : "function expression starts here",
          startLoc)) {
    return None;
  }

  ESTree::NodeList paramList;

  llvh::SaveAndRestore<bool> saveArgsAndBodyParamYield(
      paramYield_, isGenerator);
  llvh::SaveAndRestore<bool> saveArgsAndBodyParamAwait(paramAwait_, isAsync);

  if (!parseFormalParameters(param, paramList))
    return None;

  ESTree::Node *returnType = nullptr;
  ESTree::Node *predicate = nullptr;
#if HERMES_PARSE_FLOW
  if (context_.getParseFlow() && check(TokenKind::colon)) {
    SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
    if (!check(checksIdent_)) {
      auto optRet = parseReturnTypeAnnotationFlow(annotStart);
      if (!optRet)
        return None;
      returnType = *optRet;
    }

    if (check(checksIdent_)) {
      auto optPred = parsePredicateFlow();
      if (!optPred)
        return None;
      predicate = *optPred;
    }
  }
#endif
#if HERMES_PARSE_TS
  if (context_.getParseTS() && check(TokenKind::colon)) {
    SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
    if (!check(checksIdent_)) {
      auto optRet = parseTypeAnnotationTS(annotStart);
      if (!optRet)
        return None;
      returnType = *optRet;
    }
  }
#endif

  // {
  if (!need(
          TokenKind::l_brace,
          isDeclaration ? "in function declaration" : "in function expression",
          isDeclaration ? "start of function declaration"
                        : "start of function expression",
          startLoc)) {
    return None;
  }

  SaveStrictModeAndSeenDirectives saveStrictModeAndSeenDirectives{this};

  // Grammar context to be used when lexing the closing brace.
  auto grammarContext =
      isDeclaration ? JSLexer::AllowRegExp : JSLexer::AllowDiv;

  if (pass_ == PreParse) {
    // Create the nodes we want to keep before the AllocationScope.
    ESTree::FunctionLikeNode *node;

    if (isDeclaration) {
      auto *decl = new (context_) ESTree::FunctionDeclarationNode(
          optId ? *optId : nullptr,
          std::move(paramList),
          nullptr,
          typeParams,
          returnType,
          predicate,
          isGenerator,
          isAsync);
      // Initialize the node with a blank body.
      decl->_body = new (context_) ESTree::BlockStatementNode({});
      node = decl;
    } else {
      auto *expr = new (context_) ESTree::FunctionExpressionNode(
          optId ? *optId : nullptr,
          std::move(paramList),
          nullptr,
          typeParams,
          returnType,
          predicate,
          isGenerator,
          isAsync);
      // Initialize the node with a blank body.
      expr->_body = new (context_) ESTree::BlockStatementNode({});
      node = expr;
    }

    AllocationScope scope(context_.getAllocator());
    auto body = parseFunctionBody(
        Param{},
        false,
        saveArgsAndBodyParamYield.get(),
        saveArgsAndBodyParamAwait.get(),
        grammarContext,
        true);
    if (!body)
      return None;

    return setLocation(startLoc, body.getValue(), node);
  }

  auto parsedBody = parseFunctionBody(
      Param{},
      forceEagerly,
      saveArgsAndBodyParamYield.get(),
      saveArgsAndBodyParamAwait.get(),
      grammarContext,
      true);
  if (!parsedBody)
    return None;
  auto *body = parsedBody.getValue();

  ESTree::FunctionLikeNode *node;
  if (isDeclaration) {
    auto *decl = new (context_) ESTree::FunctionDeclarationNode(
        optId ? *optId : nullptr,
        std::move(paramList),
        body,
        typeParams,
        returnType,
        predicate,
        isGenerator,
        isAsync);
    node = decl;
  } else {
    auto *expr = new (context_) ESTree::FunctionExpressionNode(
        optId ? *optId : nullptr,
        std::move(paramList),
        body,
        typeParams,
        returnType,
        predicate,
        isGenerator,
        isAsync);
    node = expr;
  }
  return setLocation(startLoc, body, node);
}

bool JSParserImpl::parseFormalParameters(
    Param param,
    ESTree::NodeList &paramList) {
  assert(check(TokenKind::l_paren) && "FormalParameters must start with '('");
  // (
  SMLoc lparenLoc = advance().Start;

#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
  // The first parameter can be 'this' in Flow and TypeScript.
  if (context_.getParseTypes() && check(TokenKind::rw_this)) {
    auto *name = tok_->getResWordIdentifier();
    SMLoc thisParamStart = advance().Start;

    SMLoc annotStart = tok_->getStartLoc();
    if (!eat(
            TokenKind::colon,
            JSLexer::GrammarContext::Type,
            "in 'this' type annotation",
            "start of 'this'",
            thisParamStart))
      return false;

    auto optType = parseTypeAnnotation(annotStart);
    if (!optType)
      return false;
    ESTree::Node *type = *optType;
    paramList.push_back(*setLocation(
        thisParamStart,
        getPrevTokenEndLoc(),
        new (context_) ESTree::IdentifierNode(name, type, false)));

    checkAndEat(TokenKind::comma);
  }
#endif

  while (!check(TokenKind::r_paren)) {
    if (check(TokenKind::dotdotdot)) {
      // BindingRestElement.
      auto optRestElem = parseBindingRestElement(param);
      if (!optRestElem)
        return false;
      paramList.push_back(*optRestElem.getValue());
      break;
    }

    // BindingElement.
    auto optElem = parseBindingElement(param);
    if (!optElem)
      return false;

    paramList.push_back(*optElem.getValue());

    if (!checkAndEat(TokenKind::comma))
      break;
  }

  // )
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of function parameter list",
          "start of parameter list",
          lparenLoc)) {
    return false;
  }

  return true;
}

Optional<ESTree::Node *> JSParserImpl::parseStatement(Param param) {
  CHECK_RECURSION;

#define _RET(parseFunc)       \
  if (auto res = (parseFunc)) \
    return res.getValue();    \
  else                        \
    return None;

  switch (tok_->getKind()) {
    case TokenKind::l_brace:
      _RET(parseBlock(param));
    case TokenKind::rw_var:
      _RET(parseVariableStatement(Param{}));
    case TokenKind::semi:
      _RET(parseEmptyStatement());
    case TokenKind::rw_if:
      _RET(parseIfStatement(param.get(ParamReturn)));
    case TokenKind::rw_while:
      _RET(parseWhileStatement(param.get(ParamReturn)));
    case TokenKind::rw_do:
      _RET(parseDoWhileStatement(param.get(ParamReturn)));
    case TokenKind::rw_for:
      _RET(parseForStatement(param.get(ParamReturn)));
    case TokenKind::rw_continue:
      _RET(parseContinueStatement());
    case TokenKind::rw_break:
      _RET(parseBreakStatement());
    case TokenKind::rw_return:
      if (!param.has(ParamReturn) && !context_.allowReturnOutsideFunction()) {
        // Illegal location for a return statement, but we can keep parsing.
        error(tok_->getSourceRange(), "'return' not in a function");
      }
      _RET(parseReturnStatement());
    case TokenKind::rw_with:
      _RET(parseWithStatement(param.get(ParamReturn)));
    case TokenKind::rw_switch:
      _RET(parseSwitchStatement(param.get(ParamReturn)));
    case TokenKind::rw_throw:
      _RET(parseThrowStatement(Param{}));
    case TokenKind::rw_try:
      _RET(parseTryStatement(param.get(ParamReturn)));
    case TokenKind::rw_debugger:
      _RET(parseDebuggerStatement());

    default:
#if HERMES_PARSE_FLOW
      if (context_.getParseFlow() && context_.getParseFlowMatch() &&
          LLVM_UNLIKELY(checkMaybeFlowMatch())) {
        auto optMatch = tryParseMatchStatementFlow(param.get(ParamReturn));
        if (!optMatch)
          return None;
        if (*optMatch)
          return *optMatch;
      }
#endif
      _RET(parseExpressionOrLabelledStatement(param.get(ParamReturn)));
  }

#undef _RET
}

llvh::SmallVector<llvh::SmallString<24>, 1> JSParserImpl::copySeenDirectives()
    const {
  llvh::SmallVector<llvh::SmallString<24>, 1> copies;
  for (UniqueString *directive : seenDirectives_) {
    copies.emplace_back(directive->str());
  }
  return copies;
}

Optional<ESTree::BlockStatementNode *> JSParserImpl::parseFunctionBody(
    Param param,
    bool eagerly,
    bool paramYield,
    bool paramAwait,
    JSLexer::GrammarContext grammarContext,
    bool parseDirectives) {
  if (pass_ == LazyParse && !eagerly) {
    auto startLoc = tok_->getStartLoc();
    assert(
        preParsed_->functionInfo.count(startLoc) == 1 &&
        "no function info stored during preparse");
    PreParsedFunctionInfo functionInfo = preParsed_->functionInfo[startLoc];
    SMLoc endLoc = functionInfo.end;
    if ((unsigned)(endLoc.getPointer() - startLoc.getPointer()) >=
        context_.getPreemptiveFunctionCompilationThreshold()) {
      lexer_.seek(endLoc);
      advance(grammarContext);

      // Emulate parsing the "use strict" directive in parseBlock.
      setStrictMode(functionInfo.strictMode);

      // PreParse collected directives idents into \c PreParsedFunctionInfo,
      // iterate on them and fabricate directive nodes into the body node so
      // the semantic validator can scan them back.
      ESTree::NodeList stmtList;
      for (const llvh::SmallString<24> &directive : functionInfo.directives) {
        auto *strLit = new (context_)
            ESTree::StringLiteralNode(lexer_.getIdentifier(directive));
        auto *dirStmt = new (context_)
            ESTree::ExpressionStatementNode(strLit, strLit->_value);
        stmtList.push_back(*dirStmt);
      }

      auto *body =
          new (context_) ESTree::BlockStatementNode(std::move(stmtList));
      body->isLazyFunctionBody = true;
      // Set params based on what they were at the _start_ of the function's
      // source, not what they are now, because they might have changed.
      // For example,
      // get [yield]() {}
      // means different things based on the value of paramYield at `get`,
      // not at the `{`.
      body->paramYield = paramYield;
      body->paramAwait = paramAwait;
      body->bufferId = lexer_.getBufferId();
      return setLocation(startLoc, endLoc, body);
    }
  }

  auto body = parseBlock(ParamReturn, grammarContext, parseDirectives);
  if (!body)
    return None;

  if (pass_ == PreParse) {
    preParsed_->functionInfo[(*body)->getStartLoc()] = PreParsedFunctionInfo{
        (*body)->getEndLoc(), isStrictMode(), copySeenDirectives()};
  }

  return body;
}

Optional<ESTree::Node *> JSParserImpl::parseDeclaration(Param param) {
  CHECK_RECURSION;

  assert(checkDeclaration() && "invalid start for declaration");

  if (check(TokenKind::rw_function) ||
      (check(asyncIdent_) && checkAsyncFunction())) {
    auto fdecl = parseFunctionDeclaration(Param{});
    if (!fdecl)
      return None;

    return *fdecl;
  }

  if (check(TokenKind::rw_class)) {
    auto optClass = parseClassDeclaration(Param{});
    if (!optClass)
      return None;

    return *optClass;
  }

  if (checkN(TokenKind::rw_const, letIdent_)) {
    auto optLexDecl = parseLexicalDeclaration(ParamIn);
    if (!optLexDecl)
      return None;

    return *optLexDecl;
  }

#if HERMES_PARSE_FLOW
  if (context_.getParseFlow()) {
    auto optDecl = parseFlowDeclaration();
    if (!optDecl)
      return None;
    return *optDecl;
  }
#endif

#if HERMES_PARSE_TS
  if (context_.getParseTS()) {
    auto optDecl = parseTSDeclaration();
    if (!optDecl)
      return None;
    return *optDecl;
  }
#endif

  assert(false && "checkDeclaration() returned true without a declaration");
  return None;
}

bool JSParserImpl::parseStatementListItem(
    Param param,
    AllowImportExport allowImportExport,
    ESTree::NodeList &stmtList) {
  if (checkDeclaration()) {
    auto decl = parseDeclaration(Param{});
    if (!decl)
      return false;

    stmtList.push_back(*decl.getValue());
#if HERMES_PARSE_FLOW
  } else if (context_.getParseFlow() && checkDeclareType()) {
    // declare var, declare function, declare interface, etc.
    SMLoc start = advance(JSLexer::GrammarContext::Type).Start;
    auto decl = parseDeclareFLow(start);
    if (!decl)
      return false;
    stmtList.push_back(*decl.getValue());
#endif
  } else if (tok_->getKind() == TokenKind::rw_import) {
    // 'import' can indicate an import declaration, but it's also possible a
    // Statement begins with a call to `import()`, so do a lookahead to see if
    // the next token is '('.
    // It can also be import.meta, so check for '.'.
    auto optNext = lexer_.lookahead1(None);
    if (optNext.hasValue() &&
        (*optNext == TokenKind::l_paren || *optNext == TokenKind::period)) {
      auto stmt = parseStatement(param.get(ParamReturn));
      if (!stmt)
        return false;

      stmtList.push_back(*stmt.getValue());
    } else {
      auto importDecl = parseImportDeclaration();
      if (!importDecl) {
        return false;
      }

      stmtList.push_back(*importDecl.getValue());
      if (allowImportExport == AllowImportExport::No) {
        error(
            importDecl.getValue()->getSourceRange(),
            "import declaration must be at top level of module");
      }
    }
  } else if (tok_->getKind() == TokenKind::rw_export) {
    auto exportDecl = parseExportDeclaration();
    if (!exportDecl) {
      return false;
    }

    if (allowImportExport == AllowImportExport::Yes) {
      stmtList.push_back(**exportDecl);
    } else {
      error(
          exportDecl.getValue()->getSourceRange(),
          "export declaration must be at top level of module");
    }
  } else {
    auto stmt = parseStatement(param.get(ParamReturn));
    if (!stmt)
      return false;

    stmtList.push_back(*stmt.getValue());
  }

  return true;
}

template <typename... Tail>
Optional<bool> JSParserImpl::parseStatementList(
    Param param,
    TokenKind until,
    bool parseDirectives,
    AllowImportExport allowImportExport,
    ESTree::NodeList &stmtList,
    Tail... otherUntil) {
  if (parseDirectives) {
    ESTree::ExpressionStatementNode *dirStmt;
    while (check(TokenKind::string_literal) &&
           (dirStmt = parseDirective()) != nullptr) {
      stmtList.push_back(*dirStmt);
    }
  }

  while (!check(TokenKind::eof) && !checkN(until, otherUntil...)) {
    if (!parseStatementListItem(param, allowImportExport, stmtList)) {
      return None;
    }
  }

  return true;
}

Optional<ESTree::BlockStatementNode *> JSParserImpl::parseBlock(
    Param param,
    JSLexer::GrammarContext grammarContext,
    bool parseDirectives) {
  // {
  assert(check(TokenKind::l_brace));
  SMLoc startLoc = advance().Start;

  ESTree::NodeList stmtList;

  if (!parseStatementList(
          param,
          TokenKind::r_brace,
          parseDirectives,
          AllowImportExport::No,
          stmtList)) {
    return None;
  }

  // }
  auto *body = setLocation(
      startLoc,
      tok_,
      new (context_) ESTree::BlockStatementNode(std::move(stmtList)));
  if (!eat(
          TokenKind::r_brace,
          grammarContext,
          "at end of block",
          "block starts here",
          startLoc))
    return None;

  return body;
}

bool JSParserImpl::validateBindingIdentifier(
    Param param,
    SMRange range,
    UniqueString *id,
    TokenKind kind) {
  if (id == yieldIdent_) {
    // yield is permitted as BindingIdentifier in the grammar,
    // and prohibited with static semantics.
    if (isStrictMode() || paramYield_) {
      error(range, "Unexpected usage of 'yield' as an identifier");
    }
  }

  if (id == awaitIdent_) {
    // await is permitted as BindingIdentifier in the grammar,
    // and prohibited with static semantics.
    if (paramAwait_) {
      error(range, "Unexpected usage of 'await' as an identifier");
    }
  }

  if (isStrictMode() && id == letIdent_) {
    // ES9.0 12.1.1
    // BindingIdentifier : Identifier
    // Identifier : IdentifierName (but not ReservedWord)
    // It is a Syntax Error if this phrase is contained in strict mode code
    // and the StringValue of IdentifierName is: "implements", "interface",
    // "let", "package", "private", "protected", "public", "static", or
    // "yield".
    // NOTE: All except 'let' are scanned as reserved words instead of
    // identifiers, so we only check for `let` here.
    error(
        range,
        "Invalid use of strict mode reserved word as binding identifier");
  }

  return kind == TokenKind::identifier || kind == TokenKind::rw_yield;
}

Optional<ESTree::IdentifierNode *> JSParserImpl::parseBindingIdentifier(
    Param param) {
  if (!check(TokenKind::identifier) && !tok_->isResWord()) {
    return None;
  }
  SMRange identRng = tok_->getSourceRange();

  // If we have an identifier or reserved word, then store it and the kind,
  // and pass it to the validateBindingIdentifier function.
  UniqueString *id = tok_->getResWordOrIdentifier();
  TokenKind kind = tok_->getKind();
  if (!validateBindingIdentifier(param, tok_->getSourceRange(), id, kind)) {
    return None;
  }
  advance();

  ESTree::Node *type = nullptr;
  bool optional = false;
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
  if (context_.getParseTypes()) {
    if (check(TokenKind::question)) {
      optional = true;
      advance(JSLexer::GrammarContext::Type);
    }

    if (check(TokenKind::colon)) {
      SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
      auto optType = parseTypeAnnotation(annotStart);
      if (!optType)
        return None;
      type = *optType;
    }
  }
#endif

  return setLocation(
      identRng,
      getPrevTokenEndLoc(),
      new (context_) ESTree::IdentifierNode(id, type, optional));
}

Optional<ESTree::VariableDeclarationNode *>
JSParserImpl::parseLexicalDeclaration(Param param) {
  assert(
      (check(TokenKind::rw_var) || check(TokenKind::rw_const) ||
       check(letIdent_)) &&
      "parseLexicalDeclaration() expects var/const/let");
  bool isConst = check(TokenKind::rw_const);
  auto kindIdent = tok_->getResWordOrIdentifier();

  SMLoc startLoc = advance().Start;

  ESTree::NodeList declList;
  if (!parseVariableDeclarationList(param, declList, startLoc))
    return None;

  if (!eatSemi())
    return None;

  if (isConst) {
    for (const ESTree::Node &decl : declList) {
      const auto *varDecl = cast<ESTree::VariableDeclaratorNode>(&decl);
      if (!varDecl->_init) {
        // ES9.0 13.3.1.1
        // LexicalBinding : BindingIdentifier Initializer
        // It is a Syntax Error if Initializer is not present and
        // IsConstantDeclaration of the LexicalDeclaration containing this
        // LexicalBinding is true.
        // Note that we don't perform this check in the SemanticValidator
        // because `const` declarations in `for` loops don't need initializers.
        error(
            varDecl->getSourceRange(),
            "missing initializer in const declaration");
      }
    }
  }

  auto *res = setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::VariableDeclarationNode(kindIdent, std::move(declList)));

  ensureDestructuringInitialized(res);

  return res;
}

Optional<ESTree::VariableDeclarationNode *>
JSParserImpl::parseVariableStatement(Param param) {
  return parseLexicalDeclaration(ParamIn);
}

Optional<ESTree::PrivateNameNode *> JSParserImpl::parsePrivateName() {
  assert(check(TokenKind::private_identifier));
  ESTree::Node *ident = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getPrivateIdentifier(), nullptr, false));
  SMLoc start = advance(JSLexer::GrammarContext::AllowDiv).Start;
  return setLocation(
      start, ident, new (context_) ESTree::PrivateNameNode(ident));
}

Optional<const char *> JSParserImpl::parseVariableDeclarationList(
    Param param,
    ESTree::NodeList &declList,
    SMLoc declLoc) {
  do {
    auto optDecl = parseVariableDeclaration(param, declLoc);
    if (!optDecl)
      return None;
    declList.push_back(*optDecl.getValue());
  } while (checkAndEat(TokenKind::comma));

  return "OK";
}

void JSParserImpl::ensureDestructuringInitialized(
    ESTree::VariableDeclarationNode *declNode) {
  for (auto &elem : declNode->_declarations) {
    auto *declarator = cast<ESTree::VariableDeclaratorNode>(&elem);

    if (!isa<ESTree::PatternNode>(declarator->_id) || declarator->_init)
      continue;

    error(
        declarator->_id->getSourceRange(),
        "destucturing declaration must be initialized");
  }
}

Optional<ESTree::VariableDeclaratorNode *>
JSParserImpl::parseVariableDeclaration(Param param, SMLoc declLoc) {
  ESTree::Node *target;
  SMLoc startLoc = tok_->getStartLoc();

  if (check(TokenKind::l_square, TokenKind::l_brace)) {
    auto optPat = parseBindingPattern(param);
    if (!optPat)
      return None;

    target = *optPat;
  } else {
    auto optIdent = parseBindingIdentifier(Param{});
    if (!optIdent) {
      errorExpected(
          TokenKind::identifier,
          "in declaration",
          "declaration started here",
          declLoc);
      return None;
    }

    target = *optIdent;
  }

  // No initializer?
  if (!check(TokenKind::equal)) {
    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::VariableDeclaratorNode(nullptr, target));
  };

  // Parse the initializer.
  auto debugLoc = advance().Start;

  auto expr = parseAssignmentExpression(
      param, AllowTypedArrowFunction::Yes, CoverTypedParameters::No);
  if (!expr)
    return None;

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      debugLoc,
      new (context_) ESTree::VariableDeclaratorNode(*expr, target));
}

Optional<ESTree::Node *> JSParserImpl::parseBindingPattern(Param param) {
  assert(
      check(TokenKind::l_square, TokenKind::l_brace) &&
      "BindingPattern expects '{' or '['");
  if (check(TokenKind::l_square)) {
    auto optAB = parseArrayBindingPattern(param);
    if (!optAB)
      return None;
    return *optAB;
  } else {
    auto optOB = parseObjectBindingPattern(param);
    if (!optOB)
      return None;
    return *optOB;
  }
}

Optional<ESTree::ArrayPatternNode *> JSParserImpl::parseArrayBindingPattern(
    Param param) {
  assert(check(TokenKind::l_square) && "ArrayBindingPattern expects '['");

  // Eat the '[', recording the start location.
  auto startLoc = advance().Start;

  ESTree::NodeList elemList;

  if (!check(TokenKind::r_square)) {
    for (;;) {
      if (check(TokenKind::comma)) {
        // Elision.
        elemList.push_back(
            *setLocation(tok_, tok_, new (context_) ESTree::EmptyNode()));
      } else if (check(TokenKind::dotdotdot)) {
        // BindingRestElement.
        auto optRestElem = parseBindingRestElement(param);
        if (!optRestElem)
          return None;
        elemList.push_back(*optRestElem.getValue());
        break;
      } else {
        // BindingElement.
        auto optElem = parseBindingElement(param);
        if (!optElem)
          return None;
        elemList.push_back(*optElem.getValue());
      }

      if (!checkAndEat(TokenKind::comma))
        break;
      if (check(TokenKind::r_square)) // Check for ",]".
        break;
    }
  }

  if (!eat(
          TokenKind::r_square,
          JSLexer::AllowDiv,
          "at end of array binding pattern '[...'",
          "location of '['",
          startLoc))
    return None;

  ESTree::Node *type = nullptr;
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
  if (context_.getParseTypes()) {
    if (check(TokenKind::colon)) {
      SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
      auto optType = parseTypeAnnotation(annotStart);
      if (!optType)
        return None;
      type = *optType;
    }
  }
#endif

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ArrayPatternNode(std::move(elemList), type));
}

Optional<ESTree::Node *> JSParserImpl::parseBindingElement(Param param) {
  CHECK_RECURSION;
  ESTree::Node *elem;

  if (check(TokenKind::l_square, TokenKind::l_brace)) {
    auto optPat = parseBindingPattern(param);
    if (!optPat)
      return None;
    elem = *optPat;
  } else {
    auto optIdent = parseBindingIdentifier(param);
    if (!optIdent) {
      error(
          tok_->getStartLoc(),
          "identifier, '{' or '[' expected in binding pattern");
      return None;
    }
    elem = *optIdent;
  }

  // No initializer?
  if (!check(TokenKind::equal))
    return elem;

  auto optInit = parseBindingInitializer(param, elem);
  if (!optInit)
    return None;
  return *optInit;
}

Optional<ESTree::Node *> JSParserImpl::parseBindingRestElement(Param param) {
  assert(
      check(TokenKind::dotdotdot) &&
      "BindingRestElement expected to start with '...'");

  auto startLoc = advance().Start;

  auto optElem = parseBindingElement(param);
  if (!optElem)
    return None;
  if (isa<ESTree::AssignmentPatternNode>(*optElem)) {
    error(
        optElem.getValue()->getSourceRange(),
        "rest elemenent may not have a default initializer");
    return None;
  }

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::RestElementNode(*optElem));
}

Optional<ESTree::AssignmentPatternNode *> JSParserImpl::parseBindingInitializer(
    Param param,
    ESTree::Node *left) {
  assert(check(TokenKind::equal) && "binding initializer requires '='");

  // Parse the initializer.
  auto debugLoc = advance().Start;

  auto expr = parseAssignmentExpression(ParamIn + param);
  if (!expr)
    return None;

  return setLocation(
      left,
      getPrevTokenEndLoc(),
      debugLoc,
      new (context_) ESTree::AssignmentPatternNode(left, *expr));
}

Optional<ESTree::ObjectPatternNode *> JSParserImpl::parseObjectBindingPattern(
    Param param) {
  assert(check(TokenKind::l_brace) && "ObjectBindingPattern expects '{'");

  // Eat the '{', recording the start location.
  auto startLoc = advance().Start;

  ESTree::NodeList propList{};

  if (!check(TokenKind::r_brace)) {
    for (;;) {
      if (check(TokenKind::dotdotdot)) {
        // BindingRestProperty.
        auto optRestElem = parseBindingRestProperty(param);
        if (!optRestElem)
          return None;
        propList.push_back(*optRestElem.getValue());
        break;
      }
      auto optProp = parseBindingProperty(param);
      if (!optProp)
        return None;

      propList.push_back(**optProp);

      if (!checkAndEat(TokenKind::comma))
        break;
      if (check(TokenKind::r_brace)) // check for ",}"
        break;
    }
  }

  if (!eat(
          TokenKind::r_brace,
          JSLexer::AllowDiv,
          "at end of object binding pattern '{...'",
          "location of '{'",
          startLoc))
    return None;

  ESTree::Node *type = nullptr;
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
  if (context_.getParseTypes()) {
    if (check(TokenKind::colon)) {
      SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
      auto optType = parseTypeAnnotation(annotStart);
      if (!optType)
        return None;
      type = *optType;
    }
  }
#endif

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ObjectPatternNode(std::move(propList), type));
}

Optional<ESTree::PropertyNode *> JSParserImpl::parseBindingProperty(
    Param param) {
  bool computed = check(TokenKind::l_square);
  SMLoc startLoc = tok_->getStartLoc();
  auto optKey = parsePropertyName();
  if (!optKey)
    return None;
  ESTree::Node *key = optKey.getValue();

  ESTree::Node *value = nullptr;
  bool shorthand = false;

  if (checkAndEat(TokenKind::colon)) {
    // PropertyName ":" BindingElement
    //               ^
    auto optElement = parseBindingElement(Param{});
    if (!optElement)
      return None;
    value = optElement.getValue();
  } else {
    // SingleNameBinding :
    //   BindingIdentifier Initializer[opt]
    //                     ^

    // Must validate BindingIdentifier, because there are certain identifiers
    // which are valid as PropertyName but not as BindingIdentifier.
    auto *ident = dyn_cast<ESTree::IdentifierNode>(key);
    if (!ident ||
        !validateBindingIdentifier(
            Param{},
            ident->getSourceRange(),
            ident->_name,
            TokenKind::identifier)) {
      error(startLoc, "identifier expected in object binding pattern");
      return None;
    }

    shorthand = true;

    if (check(TokenKind::equal)) {
      // BindingIdentifier Initializer
      //                   ^
      // Clone the key because parseBindingInitializer will wrap it.
      auto *left = setLocation(
          ident,
          ident,
          new (context_) ESTree::IdentifierNode(ident->_name, nullptr, false));
      auto optInit = parseBindingInitializer(param + ParamIn, left);
      if (!optInit)
        return None;

      value = *optInit;
    } else {
      // BindingIdentifier
      //                   ^
      // Shorthand property initialization, clone the key directly.
      value = setLocation(
          ident,
          ident,
          new (context_) ESTree::IdentifierNode(ident->_name, nullptr, false));
    }
  }

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::PropertyNode(
          key, value, initIdent_, computed, false, shorthand));
}

Optional<ESTree::Node *> JSParserImpl::parseBindingRestProperty(
    hermes::parser::detail::Param param) {
  assert(
      check(TokenKind::dotdotdot) &&
      "BindingRestProperty expected to start with '...'");

  auto startLoc = advance().Start;

  // NOTE: the spec says that this cannot be another pattern, even though it
  // would make sense.
#if 0
  auto optElem = parseBindingElement(param);
#else
  auto optElem = parseBindingIdentifier(param);
#endif
  if (!optElem) {
    error(
        tok_->getStartLoc(),
        "identifier expected after '...' in object pattern");
    return None;
  }

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::RestElementNode(*optElem));
}

Optional<ESTree::EmptyStatementNode *> JSParserImpl::parseEmptyStatement() {
  assert(check(TokenKind::semi));
  auto *empty =
      setLocation(tok_, tok_, new (context_) ESTree::EmptyStatementNode());
  advance();

  return empty;
}

Optional<ESTree::Node *> JSParserImpl::parseExpressionOrLabelledStatement(
    Param param) {
  bool startsWithIdentifier = check(TokenKind::identifier);

  // ES9.0 13.5
  // Lookahead cannot be any of: {, function, async function, class, let [
  // Allow execution to continue because the expression may be parsed,
  // but report an error because it will be ambiguous whether the parse was
  // correct.
  if (checkN(TokenKind::l_brace, TokenKind::rw_function, TokenKind::rw_class) ||
      (check(asyncIdent_) && checkAsyncFunction())) {
    // There's no need to stop reporting errors.
    error(
        tok_->getSourceRange(),
        "declaration not allowed as expression statement");
  }

  if (check(letIdent_)) {
    SMLoc letLoc = advance().Start;
    if (check(TokenKind::l_square)) {
      // let [
      error(
          {letLoc, tok_->getEndLoc()},
          "ambiguous 'let [': either a 'let' binding or a member expression");
    }
    lexer_.seek(letLoc);
    advance();
  }

  SMLoc startLoc = tok_->getStartLoc();
  auto optExpr = parseExpression(ParamIn, CoverTypedParameters::No);
  if (!optExpr)
    return None;

  // Check whether this is a label. The expression must have started with an
  // identifier, be just an identifier and be
  // followed by ':'
  if (startsWithIdentifier && isa<ESTree::IdentifierNode>(optExpr.getValue()) &&
      checkAndEat(TokenKind::colon)) {
    auto *id = cast<ESTree::IdentifierNode>(optExpr.getValue());

    ESTree::Node *body = nullptr;
    if (check(TokenKind::rw_function)) {
      auto optFunc = parseFunctionDeclaration(param);
      if (!optFunc)
        return None;
      /// ES9.0 13.13.1
      /// It is a Syntax Error if any source text matches this rule.
      /// LabelledItem : FunctionDeclaration
      /// NOTE: GeneratorDeclarations are disallowed as part of the grammar
      /// as well, so all FunctionDeclarations are disallowed as labeled
      /// items, except via an AnnexB extension which is unsupported in
      /// Hermes.
      error(
          optFunc.getValue()->getSourceRange().Start,
          "Function declaration not allowed as body of labeled statement");
      body = optFunc.getValue();
    } else {
      // Statement
      auto optBody = parseStatement(param.get(ParamReturn));
      if (!optBody)
        return None;
      body = optBody.getValue();
    }

    return setLocation(
        id, body, new (context_) ESTree::LabeledStatementNode(id, body));
  } else {
    if (!eatSemi())
      return None;

    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_)
            ESTree::ExpressionStatementNode(optExpr.getValue(), nullptr));
  }
}

Optional<ESTree::IfStatementNode *> JSParserImpl::parseIfStatement(
    Param param) {
  assert(check(TokenKind::rw_if));
  SMLoc startLoc = advance().Start;

  SMLoc condLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'if'",
          "location of 'if'",
          startLoc))
    return None;
  auto optTest = parseExpression();
  if (!optTest)
    return None;
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of 'if' condition",
          "'if' condition starts here",
          condLoc))
    return None;

  /// Parse a statement or (only in loose mode) a function declaration.
  /// ES2022 B.3.3 allows FunctionDeclaration as consequent and alternate.
  /// These FunctionDeclarations are supposed to be processed precisely as if
  /// they were surrounded by BlockStatement, including function promotion.
  /// To allow this, surround them with a synthetic BlockStatement.
  auto parseStatementOrFunctionDeclaration =
      [this, param]() -> Optional<ESTree::Node *> {
    if (check(TokenKind::rw_function)) {
      auto optFunction = parseFunctionDeclaration(Param{});
      if (!optFunction)
        return None;
      if (isStrictMode()) {
        error(
            (*optFunction)->getStartLoc(),
            "In strict mode, functions cannot be declared in if statements");
      }
      if ((*optFunction)->_generator || (*optFunction)->_async) {
        error(
            (*optFunction)->getStartLoc(),
            "Functions in if statements cannot be generator/async");
      }
      ESTree::NodeList stmts;
      stmts.push_back(**optFunction);
      return setLocation(
          *optFunction,
          *optFunction,
          new (context_) ESTree::BlockStatementNode(std::move(stmts)));
    }
    auto optStatement = parseStatement(param.get(ParamReturn));
    if (!optStatement)
      return None;
    return *optStatement;
  };

  auto optConsequent = parseStatementOrFunctionDeclaration();
  if (!optConsequent)
    return None;

  if (checkAndEat(TokenKind::rw_else)) {
    auto optAlternate = parseStatementOrFunctionDeclaration();
    if (!optAlternate)
      return None;

    return setLocation(
        startLoc,
        optAlternate.getValue(),
        new (context_) ESTree::IfStatementNode(
            optTest.getValue(),
            optConsequent.getValue(),
            optAlternate.getValue()));
  } else {
    return setLocation(
        startLoc,
        optConsequent.getValue(),
        new (context_) ESTree::IfStatementNode(
            optTest.getValue(), optConsequent.getValue(), nullptr));
  }
}

Optional<ESTree::WhileStatementNode *> JSParserImpl::parseWhileStatement(
    Param param) {
  assert(check(TokenKind::rw_while));
  SMLoc startLoc = advance().Start;

  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'while'",
          "location of 'while'",
          startLoc))
    return None;
  auto optTest = parseExpression();
  if (!optTest)
    return None;
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of 'while' condition",
          "location of 'while'",
          startLoc))
    return None;

  auto optBody = parseStatement(param.get(ParamReturn));
  if (!optBody)
    return None;

  return setLocation(
      startLoc,
      optBody.getValue(),
      new (context_)
          ESTree::WhileStatementNode(optBody.getValue(), optTest.getValue()));
}

Optional<ESTree::DoWhileStatementNode *> JSParserImpl::parseDoWhileStatement(
    Param param) {
  assert(check(TokenKind::rw_do));
  SMLoc startLoc = advance().Start;

  auto optBody = parseStatement(param.get(ParamReturn));
  if (!optBody)
    return None;

  SMLoc whileLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::rw_while,
          JSLexer::AllowRegExp,
          "at end of 'do-while'",
          "'do-while' starts here",
          startLoc))
    return None;

  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'do-while'",
          "location of 'while'",
          whileLoc))
    return None;
  auto optTest = parseExpression();
  if (!optTest)
    return None;
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "at end of 'do-while' condition",
          "location of 'while'",
          whileLoc))
    return None;

  eatSemi(true);

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::DoWhileStatementNode(optBody.getValue(), optTest.getValue()));
}

Optional<ESTree::Node *> JSParserImpl::parseForStatement(Param param) {
  assert(check(TokenKind::rw_for));
  SMLoc startLoc = advance().Start;

  bool await = false;
  SMRange awaitRng;
  if (paramAwait_ && check(awaitIdent_)) {
    awaitRng = advance();
    await = true;
  }

  SMLoc lparenLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'for'",
          "location of 'for'",
          startLoc))
    return None;

  ESTree::VariableDeclarationNode *decl = nullptr;
  ESTree::NodePtr expr1 = nullptr;

  if (checkN(TokenKind::rw_var, TokenKind::rw_const, letIdent_)) {
    // Productions valid here:
    //   for ( var/let/const VariableDeclarationList
    //   for [await] ( var/let/const VariableDeclaration
    SMLoc varStartLoc = tok_->getStartLoc();
    auto *declIdent = tok_->getResWordOrIdentifier();
    advance();

    ESTree::NodeList declList;
    if (!parseVariableDeclarationList(Param{}, declList, varStartLoc))
      return None;

    auto endLoc = declList.back().getEndLoc();
    decl = setLocation(
        varStartLoc,
        endLoc,
        new (context_)
            ESTree::VariableDeclarationNode(declIdent, std::move(declList)));
  } else {
    if (!check(TokenKind::semi)) {
      llvh::Optional<ESTree::Node *> optExpr1;
      if (await) {
        //   for await ( LeftHandSideExpression
        //               ^
        optExpr1 = parseLeftHandSideExpression();
      } else {
        // ForStatement:
        //   for ( Expression_opt
        //         ^
        // ForInOfStatement:
        //   for ( LeftHandSideExpression
        //         ^
        // Lookahead for LeftHandSideExpression cannot be 'let' or 'async of'.
        // We've handled `let` above.
        // To distinguish between the two productions here, we let the resolver
        // check that the LHS of the `of` or `in` is valid (the resolver will
        // throw the error instead of the parser).
        optExpr1 = parseExpression(Param{});
      }
      if (!optExpr1)
        return None;
      expr1 = optExpr1.getValue();
    }
  }

  if (checkN(TokenKind::rw_in, ofIdent_)) {
    // Productions valid here:
    //   for [await] ( var/let/const VariableDeclaration[In] in/of
    //   for [await] ( LeftHandSideExpression in/of

    if (decl && decl->_declarations.size() > 1) {
      error(
          decl->getSourceRange(),
          "Only one binding must be declared in a for-in/for-of loop");
      return None;
    }

    // Check for destructuring pattern on the left and reparse it.
    if (expr1 &&
        (isa<ESTree::ArrayExpressionNode>(expr1) ||
         isa<ESTree::ObjectExpressionNode>(expr1))) {
      auto optExpr1 = reparseAssignmentPattern(expr1, false);
      if (!optExpr1)
        return None;
      expr1 = *optExpr1;
    }

    // Remember whether we are parsing for-in or for-of.
    bool const forInLoop = check(TokenKind::rw_in);
    advance();

    if (forInLoop && await)
      error(awaitRng, "unexpected 'await' in for..in loop");

    auto optRightExpr =
        forInLoop ? parseExpression() : parseAssignmentExpression(ParamIn);

    if (!eat(
            TokenKind::r_paren,
            JSLexer::AllowRegExp,
            "after 'for(... in/of ...'",
            "location of '('",
            lparenLoc))
      return None;

    auto optBody = parseStatement(param.get(ParamReturn));
    if (!optBody || !optRightExpr)
      return None;

    ESTree::Node *node;
    if (forInLoop) {
      node = new (context_) ESTree::ForInStatementNode(
          decl ? decl : expr1, optRightExpr.getValue(), optBody.getValue());
    } else {
      node = new (context_) ESTree::ForOfStatementNode(
          decl ? decl : expr1,
          optRightExpr.getValue(),
          optBody.getValue(),
          await);
    }
    return setLocation(startLoc, optBody.getValue(), node);
  } else if (checkAndEat(TokenKind::semi)) {
    // Productions valid here:
    //   for ( var/let/const VariableDeclarationList[In] ; Expressionopt ;
    //   Expressionopt )
    //       Statement
    //   for ( Expression[In]opt ; Expressionopt ; Expressionopt ) Statement

    if (await)
      error(awaitRng, "unexpected 'await' in for loop without 'of'");

    if (decl)
      ensureDestructuringInitialized(decl);

    ESTree::NodePtr test = nullptr;
    if (!check(TokenKind::semi)) {
      auto optTest = parseExpression();
      if (!optTest)
        return None;
      test = optTest.getValue();
    }

    if (!eat(
            TokenKind::semi,
            JSLexer::AllowRegExp,
            "after 'for( ... ; ...'",
            "location of '('",
            lparenLoc))
      return None;

    ESTree::NodePtr update = nullptr;
    if (!check(TokenKind::r_paren)) {
      auto optUpdate = parseExpression();
      if (!optUpdate)
        return None;
      update = optUpdate.getValue();
    }

    if (!eat(
            TokenKind::r_paren,
            JSLexer::AllowRegExp,
            "after 'for( ... ; ... ; ...'",
            "location of '('",
            lparenLoc))
      return None;

    auto optBody = parseStatement(param.get(ParamReturn));
    if (!optBody)
      return None;

    return setLocation(
        startLoc,
        optBody.getValue(),
        new (context_) ESTree::ForStatementNode(
            decl ? decl : expr1, test, update, optBody.getValue()));
  } else {
    errorExpected(
        TokenKind::semi,
        TokenKind::rw_in,
        "inside 'for'",
        "location of the 'for'",
        startLoc);
    return None;
  }
}

Optional<ESTree::ContinueStatementNode *>
JSParserImpl::parseContinueStatement() {
  assert(check(TokenKind::rw_continue));
  SMLoc startLoc = advance().Start;

  if (eatSemi(true))
    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::ContinueStatementNode(nullptr));

  if (!need(
          TokenKind::identifier,
          "after 'continue'",
          "location of 'continue'",
          startLoc))
    return None;
  auto *id = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance();

  if (!eatSemi())
    return None;

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ContinueStatementNode(id));
}

Optional<ESTree::BreakStatementNode *> JSParserImpl::parseBreakStatement() {
  assert(check(TokenKind::rw_break));
  SMLoc startLoc = advance().Start;

  if (eatSemi(true))
    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::BreakStatementNode(nullptr));

  if (!need(
          TokenKind::identifier,
          "after 'break'",
          "location of 'break'",
          startLoc))
    return None;
  auto *id = setLocation(
      tok_,
      tok_,
      new (context_)
          ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
  advance();

  if (!eatSemi())
    return None;

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::BreakStatementNode(id));
}

Optional<ESTree::ReturnStatementNode *> JSParserImpl::parseReturnStatement() {
  assert(check(TokenKind::rw_return));
  SMLoc startLoc = advance().Start;

  if (eatSemi(true))
    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::ReturnStatementNode(nullptr));

  auto optArg = parseExpression();
  if (!optArg)
    return None;

  if (!eatSemi())
    return None;

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ReturnStatementNode(optArg.getValue()));
}

Optional<ESTree::WithStatementNode *> JSParserImpl::parseWithStatement(
    Param param) {
  assert(check(TokenKind::rw_with));
  SMLoc startLoc = advance().Start;

  SMLoc lparenLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'with'",
          "location of 'with'",
          startLoc))
    return None;

  auto optExpr = parseExpression();
  if (!optExpr)
    return None;

  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "after 'with (...'",
          "location of '('",
          lparenLoc))
    return None;

  auto optBody = parseStatement(param.get(ParamReturn));
  if (!optBody)
    return None;

  return setLocation(
      startLoc,
      optBody.getValue(),
      new (context_)
          ESTree::WithStatementNode(optExpr.getValue(), optBody.getValue()));
}

Optional<ESTree::SwitchStatementNode *> JSParserImpl::parseSwitchStatement(
    Param param) {
  assert(check(TokenKind::rw_switch));
  SMLoc startLoc = advance().Start;

  SMLoc lparenLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "after 'switch'",
          "location of 'switch'",
          startLoc))
    return None;

  auto optDiscriminant = parseExpression();
  if (!optDiscriminant)
    return None;

  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowRegExp,
          "after 'switch (...'",
          "location of '('",
          lparenLoc))
    return None;

  SMLoc lbraceLoc = tok_->getStartLoc();
  if (!eat(
          TokenKind::l_brace,
          JSLexer::AllowRegExp,
          "after 'switch (...)'",
          "'switch' starts here",
          startLoc))
    return None;

  ESTree::NodeList clauseList;
  SMLoc defaultLocation; // location of the 'default' clause

  // Parse the switch body.
  while (!check(TokenKind::r_brace)) {
    SMLoc clauseStartLoc = tok_->getStartLoc();

    ESTree::NodePtr testExpr = nullptr;
    bool ignoreClause = false; // Set to true in error recovery when we want to
                               // parse but ignore the parsed statements.
    ESTree::NodeList stmtList;

    SMLoc caseLoc = tok_->getStartLoc();
    if (checkAndEat(TokenKind::rw_case)) {
      auto optTestExpr = parseExpression(ParamIn, CoverTypedParameters::No);
      if (!optTestExpr)
        return None;
      testExpr = optTestExpr.getValue();
    } else if (checkAndEat(TokenKind::rw_default)) {
      if (defaultLocation.isValid()) {
        error(clauseStartLoc, "more than one 'default' clause in 'switch'");
        sm_.note(defaultLocation, "first 'default' clause was defined here");

        // We want to continue parsing but ignore the statements.
        ignoreClause = true;
      } else {
        defaultLocation = clauseStartLoc;
      }
    } else {
      errorExpected(
          TokenKind::rw_case,
          TokenKind::rw_default,
          "inside 'switch'",
          "location of 'switch'",
          startLoc);
      return None;
    }

    SMLoc colonLoc =
        tok_->getEndLoc(); // save the location in case the clause is empty
    if (!eat(
            TokenKind::colon,
            JSLexer::AllowRegExp,
            "after 'case ...' or 'default'",
            "location of 'case'/'default'",
            caseLoc))
      return None;

    /// case Expression : StatementList[opt]
    ///                   ^
    if (!parseStatementList(
            param.get(ParamReturn),
            TokenKind::rw_default,
            false,
            AllowImportExport::No,
            stmtList,
            TokenKind::rw_case,
            TokenKind::r_brace))
      return None;

    if (!ignoreClause) {
      auto clauseEndLoc =
          stmtList.empty() ? colonLoc : stmtList.back().getEndLoc();
      clauseList.push_back(*setLocation(
          clauseStartLoc,
          clauseEndLoc,
          new (context_)
              ESTree::SwitchCaseNode(testExpr, std::move(stmtList))));
    }
  }

  SMLoc endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_brace,
          JSLexer::AllowRegExp,
          "at end of 'switch' statement",
          "location of '{'",
          lbraceLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::SwitchStatementNode(
          optDiscriminant.getValue(), std::move(clauseList)));
}

Optional<ESTree::ThrowStatementNode *> JSParserImpl::parseThrowStatement(
    Param param) {
  assert(check(TokenKind::rw_throw));
  SMLoc startLoc = advance().Start;

  if (lexer_.isNewLineBeforeCurrentToken()) {
    error(tok_->getStartLoc(), "'throw' argument must be on the same line");
    sm_.note(startLoc, "location of the 'throw'");
    return None;
  }

  auto optExpr = parseExpression();
  if (!optExpr)
    return None;

  if (!eatSemi())
    return None;

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ThrowStatementNode(optExpr.getValue()));
}

Optional<ESTree::TryStatementNode *> JSParserImpl::parseTryStatement(
    Param param) {
  assert(check(TokenKind::rw_try));
  SMLoc startLoc = advance().Start;

  if (!need(TokenKind::l_brace, "after 'try'", "location of 'try'", startLoc))
    return None;
  auto optTryBody = parseBlock(param.get(ParamReturn));
  if (!optTryBody)
    return None;

  ESTree::CatchClauseNode *catchHandler = nullptr;
  ESTree::BlockStatementNode *finallyHandler = nullptr;

  // Parse the optional 'catch' handler.
  SMLoc handlerStartLoc = tok_->getStartLoc();
  if (checkAndEat(TokenKind::rw_catch)) {
    ESTree::Node *catchParam = nullptr;
    if (checkAndEat(TokenKind::l_paren)) {
      // CatchClause param is optional.
      if (check(TokenKind::l_square, TokenKind::l_brace)) {
        auto optPattern = parseBindingPattern(Param{});
        if (!optPattern)
          return None;
        catchParam = *optPattern;
      } else {
        auto optIdent = parseBindingIdentifier(Param{});
        if (!optIdent) {
          errorExpected(
              TokenKind::identifier,
              "inside catch list",
              "location of 'catch'",
              handlerStartLoc);
          return None;
        }
        catchParam = *optIdent;
      }

      if (!eat(
              TokenKind::r_paren,
              JSLexer::AllowRegExp,
              "after 'catch (...'",
              "location of 'catch'",
              handlerStartLoc))
        return None;
    }

    if (!need(
            TokenKind::l_brace,
            "after 'catch(...)'",
            "location of 'catch'",
            handlerStartLoc))
      return None;
    auto optCatchBody = parseBlock(param.get(ParamReturn));
    if (!optCatchBody)
      return None;

    catchHandler = setLocation(
        handlerStartLoc,
        optCatchBody.getValue(),
        new (context_)
            ESTree::CatchClauseNode(catchParam, optCatchBody.getValue()));
  }

  // Parse the optional 'finally' handler.
  SMLoc finallyLoc = tok_->getStartLoc();
  if (checkAndEat(TokenKind::rw_finally)) {
    if (!need(
            TokenKind::l_brace,
            "after 'finally'",
            "location of 'finally'",
            finallyLoc))
      return None;
    auto optFinallyBody = parseBlock(param.get(ParamReturn));
    if (!optFinallyBody)
      return None;

    finallyHandler = optFinallyBody.getValue();
  }

  // At least one handler must be present.
  if (!catchHandler && !finallyHandler) {
    errorExpected(
        TokenKind::rw_catch,
        TokenKind::rw_finally,
        "after 'try' block",
        "location of 'try'",
        startLoc);
    return None;
  }

  // Use the last handler's location as the end location.
  SMLoc endLoc =
      finallyHandler ? finallyHandler->getEndLoc() : catchHandler->getEndLoc();
  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::TryStatementNode(
          optTryBody.getValue(), catchHandler, finallyHandler));
}

Optional<ESTree::DebuggerStatementNode *>
JSParserImpl::parseDebuggerStatement() {
  assert(check(TokenKind::rw_debugger));
  SMRange startLoc = advance();

  if (!eatSemi())
    return None;

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::DebuggerStatementNode());
}

Optional<ESTree::Node *> JSParserImpl::parsePrimaryExpression() {
  CHECK_RECURSION;

  switch (tok_->getKind()) {
    case TokenKind::rw_this: {
      auto *res =
          setLocation(tok_, tok_, new (context_) ESTree::ThisExpressionNode());
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::identifier: {
      if (check(yieldIdent_)) {
        // yield is only allowed as an IdentifierReference when ParamYield is
        // false.
        if (paramYield_) {
          error(
              tok_->getSourceRange(),
              "Unexpected usage of 'yield' as an identifier reference");
        }
      }
      if (check(asyncIdent_) && checkAsyncFunction()) {
        auto func = parseFunctionExpression();
        if (!func)
          return None;
        return func.getValue();
      }
#if HERMES_PARSE_FLOW
      if (context_.getParseFlow() && context_.getParseFlowMatch() &&
          checkMaybeFlowMatch()) {
        return parseMatchCallOrMatchExpressionFlow();
      }
#endif
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::rw_null: {
      auto *res =
          setLocation(tok_, tok_, new (context_) ESTree::NullLiteralNode());
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::rw_true:
    case TokenKind::rw_false: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::BooleanLiteralNode(
              tok_->getKind() == TokenKind::rw_true));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::numeric_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::NumericLiteralNode(tok_->getNumericLiteral()));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::bigint_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::BigIntLiteralNode(tok_->getBigIntLiteral()));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::string_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::regexp_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::RegExpLiteralNode(
              tok_->getRegExpLiteral()->getBody(),
              tok_->getRegExpLiteral()->getFlags()));
      advance(JSLexer::AllowDiv);
      return res;
    }

    case TokenKind::l_square: {
      auto res = parseArrayLiteral();
      if (!res)
        return None;
      return res.getValue();
    }

    case TokenKind::l_brace: {
      auto res = parseObjectLiteral();
      if (!res)
        return None;
      return res.getValue();
    }

    case TokenKind::l_paren: {
      SMLoc startLoc = advance().Start;

      // Cover "()".
      if (check(TokenKind::r_paren)) {
        SMLoc endLoc = advance().End;
        return setLocation(
            startLoc, endLoc, new (context_) ESTree::CoverEmptyArgsNode());
      }

      ESTree::Node *expr;
      if (check(TokenKind::dotdotdot)) {
        auto optRest = parseBindingRestElement(ParamIn);
        if (!optRest)
          return None;

        expr = setLocation(
            *optRest,
            *optRest,
            new (context_) ESTree::CoverRestElementNode(*optRest));
      } else {
        auto optExpr = parseExpression(ParamIn, CoverTypedParameters::Yes);
        if (!optExpr)
          return None;
        expr = *optExpr;
      }

#if HERMES_PARSE_FLOW
      if (context_.getParseFlow()) {
        // In both these cases, we set the location to encompass the `()`.
        // (x: T)
        // ^^^^^^
        // by using `startLoc` and `tok_` as the start/end.
        // If `tok_` is not `)`, then we'll error on the `eat` call immediately
        // after these checks.
        if (auto *cover = dyn_cast<ESTree::CoverTypedIdentifierNode>(expr)) {
          if (cover->_right && !cover->_optional) {
            expr = setLocation(
                startLoc,
                tok_,
                new (context_) ESTree::TypeCastExpressionNode(
                    cover->_left, cover->_right));
          }
        } else if (check(TokenKind::colon)) {
          SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
          auto optType = parseTypeAnnotationFlow(annotStart);
          if (!optType)
            return None;
          ESTree::Node *type = *optType;
          expr = setLocation(
              startLoc,
              tok_,
              new (context_) ESTree::TypeCastExpressionNode(expr, type));
        }
      }
#endif

      if (!eat(
              TokenKind::r_paren,
              JSLexer::AllowDiv,
              "at end of parenthesized expression",
              "started here",
              startLoc))
        return None;
      // Record the number of parens surrounding an expression.
      expr->incParens();
      return expr;
    }

    case TokenKind::rw_function: {
      auto fExpr = parseFunctionExpression();
      if (!fExpr)
        return None;
      return fExpr.getValue();
    }

    case TokenKind::rw_class: {
      auto optClass = parseClassExpression();
      if (!optClass)
        return None;
      return optClass.getValue();
    }

    case TokenKind::no_substitution_template:
    case TokenKind::template_head: {
      auto optTemplate = parseTemplateLiteral(Param{});
      if (!optTemplate) {
        return None;
      }
      return optTemplate.getValue();
    }

#if HERMES_PARSE_JSX
    case TokenKind::less:
      if (context_.getParseJSX()) {
        auto optJSX = parseJSX();
        if (!optJSX)
          return None;
        return optJSX.getValue();
      }
      error(
          tok_->getStartLoc(),
          "invalid expression (possible JSX: pass -parse-jsx to parse)");
      return None;
#endif

    default:
      error(tok_->getStartLoc(), "invalid expression");
      return None;
  }
}

Optional<ESTree::ArrayExpressionNode *> JSParserImpl::parseArrayLiteral() {
  assert(check(TokenKind::l_square));
  SMLoc startLoc = advance().Start;

  ESTree::NodeList elemList;

  bool trailingComma = false;

  if (!check(TokenKind::r_square)) {
    for (;;) {
      // Elision.
      if (check(TokenKind::comma)) {
        elemList.push_back(
            *setLocation(tok_, tok_, new (context_) ESTree::EmptyNode()));
      } else if (check(TokenKind::dotdotdot)) {
        // Spread.
        auto optSpread = parseSpreadElement();
        if (!optSpread)
          return None;

        elemList.push_back(**optSpread);
      } else {
        auto expr = parseAssignmentExpression();
        if (!expr)
          return None;
        elemList.push_back(*expr.getValue());
      }

      if (!checkAndEat(TokenKind::comma))
        break;
      if (check(TokenKind::r_square)) {
        // Check for ",]".
        trailingComma = true;
        break;
      }
    }
  }

  SMLoc endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_square,
          JSLexer::AllowDiv,
          "at end of array literal '[...'",
          "location of '['",
          startLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_)
          ESTree::ArrayExpressionNode(std::move(elemList), trailingComma));
}

Optional<ESTree::ObjectExpressionNode *> JSParserImpl::parseObjectLiteral() {
  assert(check(TokenKind::l_brace));
  SMLoc startLoc = advance().Start;

  ESTree::NodeList elemList;

  if (!check(TokenKind::r_brace)) {
    for (;;) {
      if (check(TokenKind::dotdotdot)) {
        // Spread.
        auto optSpread = parseSpreadElement();
        if (!optSpread)
          return None;

        elemList.push_back(**optSpread);
      } else {
        auto prop = parsePropertyAssignment(false);
        if (!prop)
          return None;

        elemList.push_back(*prop.getValue());
      }

      if (!checkAndEat(TokenKind::comma))
        break;
      if (check(TokenKind::r_brace)) // check for ",}"
        break;
    }
  }

  SMLoc endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_brace,
          JSLexer::AllowDiv,
          "at end of object literal '{...'",
          "location of '{'",
          startLoc))
    return None;

  return setLocation(
      startLoc,
      endLoc,
      new (context_) ESTree::ObjectExpressionNode(std::move(elemList)));
}

Optional<ESTree::Node *> JSParserImpl::parseSpreadElement() {
  assert(check(TokenKind::dotdotdot) && "SpreadElement must start with '...'");
  auto spreadStartLoc = advance();

  auto optExpr = parseAssignmentExpression();
  if (!optExpr)
    return None;

  return setLocation(
      spreadStartLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::SpreadElementNode(*optExpr));
}

Optional<ESTree::Node *> JSParserImpl::parsePropertyAssignment(bool eagerly) {
  SMLoc startLoc = tok_->getStartLoc();
  ESTree::NodePtr key = nullptr;

  SaveStrictModeAndSeenDirectives saveStrictModeAndSeenDirectives{this};

  bool computed = false;
  bool generator = false;
  bool async = false;
  bool method = false;

  if (check(getIdent_)) {
    UniqueString *ident = tok_->getResWordOrIdentifier();
    SMRange identRng = tok_->getSourceRange();
    advance();

    // This could either be a getter, or a property named 'get'.
    if (check(TokenKind::colon, TokenKind::l_paren)) {
      // This is just a property.
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
    } else if (context_.getParseTypes() && check(TokenKind::less)) {
      // This is a method definition.
      method = true;
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
#endif
    } else if (check(TokenKind::comma, TokenKind::r_brace)) {
      // If the next token is "," or "}", this is a shorthand property
      // definition.
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
      auto *value = setLocation(
          key,
          key,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
      return setLocation(
          startLoc,
          value,
          new (context_)
              ESTree::PropertyNode(key, value, initIdent_, false, false, true));
    } else {
      // A getter method.
      computed = check(TokenKind::l_square);
      auto optKey = parsePropertyName();
      if (!optKey)
        return None;

      if (!eat(
              TokenKind::l_paren,
              JSLexer::AllowRegExp,
              "in getter declaration",
              "start of getter declaration",
              startLoc))
        return None;
      if (!eat(
              TokenKind::r_paren,
              JSLexer::AllowRegExp,
              "in empty getter parameter list",
              "start of getter declaration",
              startLoc))
        return None;

      ESTree::Node *returnType = nullptr;
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
      if (context_.getParseTypes() && check(TokenKind::colon)) {
        SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
        auto optRet = parseReturnTypeAnnotation(annotStart);
        if (!optRet)
          return None;
        returnType = *optRet;
      }
#endif

      llvh::SaveAndRestore<bool> oldParamYield(paramYield_, false);
      llvh::SaveAndRestore<bool> oldParamAwait(paramAwait_, false);
      if (!need(
              TokenKind::l_brace,
              "in getter declaration",
              "start of getter declaration",
              startLoc))
        return None;
      auto block = parseFunctionBody(
          ParamReturn,
          eagerly,
          oldParamYield.get(),
          oldParamAwait.get(),
          JSLexer::AllowRegExp,
          true);
      if (!block)
        return None;

      auto *funcExpr = new (context_) ESTree::FunctionExpressionNode(
          nullptr,
          ESTree::NodeList{},
          block.getValue(),
          nullptr,
          returnType,
          /* predicate */ nullptr,
          false,
          false);
      funcExpr->isMethodDefinition = true;
      setLocation(startLoc, block.getValue(), funcExpr);

      auto *node = new (context_) ESTree::PropertyNode(
          optKey.getValue(), funcExpr, getIdent_, computed, false, false);
      return setLocation(startLoc, block.getValue(), node);
    }
  } else if (check(setIdent_)) {
    UniqueString *ident = tok_->getResWordOrIdentifier();
    SMRange identRng = tok_->getSourceRange();
    advance();

    // This could either be a setter, or a property named 'set'.
    if (check(TokenKind::colon, TokenKind::l_paren)) {
      // This is just a property.
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
    } else if (context_.getParseTypes() && check(TokenKind::less)) {
      // This is a method definition.
      method = true;
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
#endif
    } else if (check(TokenKind::comma, TokenKind::r_brace)) {
      // If the next token is "," or "}", this is a shorthand property
      // definition.
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
      auto *value = setLocation(
          key,
          key,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
      return setLocation(
          startLoc,
          value,
          new (context_)
              ESTree::PropertyNode(key, value, initIdent_, false, false, true));
    } else {
      // A setter method.
      computed = check(TokenKind::l_square);
      auto optKey = parsePropertyName();
      if (!optKey)
        return None;

      llvh::SaveAndRestore<bool> oldParamYield(paramYield_, false);
      llvh::SaveAndRestore<bool> oldParamAwait(paramAwait_, false);

      ESTree::NodeList params;
      eat(TokenKind::l_paren,
          JSLexer::AllowRegExp,
          "in setter declaration",
          "start of setter declaration",
          startLoc);

      // PropertySetParameterList -> FormalParameter -> BindingElement
      auto optParam = parseBindingElement(Param{});
      if (!optParam)
        return None;
      params.push_back(**optParam);

      if (!eat(
              TokenKind::r_paren,
              JSLexer::AllowRegExp,
              "at end of setter parameter list",
              "start of setter declaration",
              startLoc))
        return None;

      ESTree::Node *returnType = nullptr;
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
      if (context_.getParseTypes() && check(TokenKind::colon)) {
        SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
        auto optRet = parseReturnTypeAnnotation(annotStart);
        if (!optRet)
          return None;
        returnType = *optRet;
      }
#endif

      if (!need(
              TokenKind::l_brace,
              "in setter declaration",
              "start of setter declaration",
              startLoc))
        return None;
      auto block = parseFunctionBody(
          ParamReturn,
          eagerly,
          oldParamYield.get(),
          oldParamAwait.get(),
          JSLexer::AllowRegExp,
          true);
      if (!block)
        return None;

      auto *funcExpr = new (context_) ESTree::FunctionExpressionNode(
          nullptr,
          std::move(params),
          block.getValue(),
          nullptr,
          returnType,
          /* predicate */ nullptr,
          false,
          false);
      funcExpr->isMethodDefinition = true;
      setLocation(startLoc, block.getValue(), funcExpr);

      auto *node = new (context_) ESTree::PropertyNode(
          optKey.getValue(), funcExpr, setIdent_, computed, false, false);
      return setLocation(startLoc, block.getValue(), node);
    }
  } else if (check(asyncIdent_)) {
    UniqueString *ident = tok_->getResWordOrIdentifier();
    SMRange identRng = tok_->getSourceRange();
    advance();

    // This could either be an async function, or a property named 'async'.
    if (check(TokenKind::colon, TokenKind::l_paren)) {
      // This is just a property (or method) called 'async'.
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
    } else if (context_.getParseTypes() && check(TokenKind::less)) {
      // This is a method definition.
      method = true;
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
#endif
    } else if (check(TokenKind::comma, TokenKind::r_brace)) {
      // If the next token is "," or "}", this is a shorthand property
      // definition.
      key = setLocation(
          identRng,
          identRng,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
      auto *value = setLocation(
          key,
          key,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));
      return setLocation(
          startLoc,
          value,
          new (context_)
              ESTree::PropertyNode(key, value, initIdent_, false, false, true));
    } else {
      if (lexer_.isNewLineBeforeCurrentToken()) {
        error(
            tok_->getSourceRange(),
            "newline not allowed after 'async' in a method definition");
      }
      // This is an async function, parse the key and set `async` to true.
      async = true;
      method = true;
      generator = checkAndEat(TokenKind::star);
      computed = check(TokenKind::l_square);
      auto optKey = parsePropertyName();
      if (!optKey)
        return None;

      key = optKey.getValue();
    }
  } else if (check(TokenKind::identifier)) {
    auto *ident = tok_->getIdentifier();
    key = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(ident, nullptr, false));
    advance();
    // If the next token is "," or "}", this is a shorthand property definition.
    if (check(TokenKind::comma, TokenKind::r_brace)) {
      auto *value = setLocation(
          key,
          key,
          new (context_) ESTree::IdentifierNode(ident, nullptr, false));

      return setLocation(
          startLoc,
          value,
          new (context_)
              ESTree::PropertyNode(key, value, initIdent_, false, false, true));
    }
  } else {
    generator = checkAndEat(TokenKind::star);
    computed = check(TokenKind::l_square);
    auto optKey = parsePropertyName();
    if (!optKey)
      return None;

    key = optKey.getValue();
  }

  ESTree::Node *value;
  bool shorthand = false;

  if (isa<ESTree::IdentifierNode>(key) && check(TokenKind::equal)) {
    // Check for CoverInitializedName: IdentifierReference Initializer
    auto startLoc = advance().Start;
    auto optInit = parseAssignmentExpression();
    if (!optInit)
      return None;

    shorthand = true;

    value = setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::CoverInitializerNode(*optInit));
  } else if (check(TokenKind::l_paren, TokenKind::less) || async) {
    // Try this branch when we have '(' or '<' to indicate a method
    // or when we know this is async, because async must also indicate a method,
    // and we must avoid parsing ordinary properties from ':'.

    // Parse the MethodDefinition manually here.
    // Do not use `parseClassElement` because we had to parsePropertyName
    // in this function ourselves and check for SingleNameBindings, which are
    // not parsed with `parsePropertyName`.
    // MethodDefinition:
    // PropertyName "(" UniqueFormalParameters ")" "{" FunctionBody "}"
    //               ^
    llvh::SaveAndRestore<bool> oldParamYield(paramYield_, generator);
    llvh::SaveAndRestore<bool> oldParamAwait(paramAwait_, async);

    method = true;

    ESTree::Node *typeParams = nullptr;
#if HERMES_PARSE_FLOW
    if (context_.getParseFlow() && check(TokenKind::less)) {
      auto optTypeParams = parseTypeParamsFlow();
      if (!optTypeParams)
        return None;
      typeParams = *optTypeParams;
    }
#endif
#if HERMES_PARSE_TS
    if (context_.getParseTS() && check(TokenKind::less)) {
      auto optTypeParams = parseTSTypeParameters();
      if (!optTypeParams)
        return None;
      typeParams = *optTypeParams;
    }
#endif

    // (
    if (!need(
            TokenKind::l_paren,
            "in method definition",
            "start of method definition",
            startLoc))
      return None;

    ESTree::NodeList args{};
    if (!parseFormalParameters(Param{}, args))
      return None;

    ESTree::Node *returnType = nullptr;
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
    if (context_.getParseTypes() && check(TokenKind::colon)) {
      SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
      auto optRet = parseReturnTypeAnnotation(annotStart);
      if (!optRet)
        return None;
      returnType = *optRet;
    }
#endif

    if (!need(
            TokenKind::l_brace,
            "in method definition",
            "start of method definition",
            startLoc))
      return None;
    auto optBody = parseFunctionBody(
        ParamReturn,
        eagerly,
        oldParamYield.get(),
        oldParamAwait.get(),
        JSLexer::AllowRegExp,
        true);
    if (!optBody)
      return None;

    auto *funcExpr = new (context_) ESTree::FunctionExpressionNode(
        nullptr,
        std::move(args),
        optBody.getValue(),
        typeParams,
        returnType,
        /* predicate */ nullptr,
        generator,
        async);
    funcExpr->isMethodDefinition = true;
    setLocation(startLoc, optBody.getValue(), funcExpr);

    value = funcExpr;
  } else {
    if (!eat(
            TokenKind::colon,
            JSLexer::AllowRegExp,
            "in property initialization",
            "start of property initialization",
            startLoc))
      return None;

    auto optValue = parseAssignmentExpression();
    if (!optValue)
      return None;
    value = *optValue;
  }

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::PropertyNode(
          key, value, initIdent_, computed, method, shorthand));
}

Optional<ESTree::Node *> JSParserImpl::parsePropertyName() {
  switch (tok_->getKind()) {
    case TokenKind::string_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
      advance();
      return res;
    }

    case TokenKind::numeric_literal: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::NumericLiteralNode(tok_->getNumericLiteral()));
      advance();
      return res;
    }

    case TokenKind::identifier: {
      auto *res = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
      advance();
      return res;
    }

    case TokenKind::l_square: {
      SMLoc start = advance().Start;
      auto optExpr = parseAssignmentExpression(ParamIn);
      if (!optExpr) {
        return None;
      }
      if (!need(
              TokenKind::r_square,
              "at end of computed property key",
              "start of property key",
              start)) {
        return llvh::None;
      }
      advance();
      return *optExpr;
    }

    default:
      if (tok_->isResWord()) {
        auto *res = setLocation(
            tok_,
            tok_,
            new (context_) ESTree::IdentifierNode(
                tok_->getResWordIdentifier(), nullptr, false));
        advance();
        return res;
      } else {
        error(
            tok_->getSourceRange(),
            "invalid property name - must be a string, number or identifier");
        return None;
      }
  }
}

Optional<ESTree::Node *> JSParserImpl::parseTemplateLiteral(Param param) {
  assert(checkTemplateLiteral() && "invalid template literal start");

  SMLoc start = tok_->getStartLoc();

  ESTree::NodeList quasis;
  ESTree::NodeList expressions;

  /// Push the current TemplateElement onto quasis and advance the lexer.
  /// \param tail true if pushing the last element.
  /// \return false on failure.
  auto pushTemplateElement = [&quasis, &param, this](bool tail) -> bool {
    if (tok_->getTemplateLiteralContainsNotEscapes() &&
        !param.has(ParamTagged)) {
      error(
          tok_->getSourceRange(),
          "untagged template literal contains invalid escape sequence");
      return false;
    }
    auto *quasi = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::TemplateElementNode(
            tail, tok_->getTemplateValue(), tok_->getTemplateRawValue()));
    quasis.push_back(*quasi);
    return true;
  };

  // TemplateSpans
  while (
      !check(TokenKind::no_substitution_template, TokenKind::template_tail)) {
    // TemplateSpans
    // Alternate TemplateMiddle and Expression until TemplateTail.
    if (!check(TokenKind::template_head, TokenKind::template_middle)) {
      error(tok_->getSourceRange(), "expected template literal");
      return None;
    }

    // First, push the TemplateElement.
    if (!pushTemplateElement(false))
      return None;
    SMLoc subStart = advance().Start;

    // Parse the next expression and add it to the expressions.
    auto optExpr = parseExpression(ParamIn);
    if (!optExpr)
      return None;
    expressions.push_back(*optExpr.getValue());

    if (!check(TokenKind::r_brace)) {
      errorExpected(
          TokenKind::r_brace,
          "at end of substition in template literal",
          "start of substitution",
          subStart);
      return None;
    }

    // The } at the end of the expression must be rescanned as a
    // TemplateMiddle or TemplateTail.
    lexer_.rescanRBraceInTemplateLiteral();
  }

  // TemplateTail or NoSubstitutionTemplate
  if (!pushTemplateElement(true))
    return None;

  return setLocation(
      start,
      advance().End,
      new (context_) ESTree::TemplateLiteralNode(
          std::move(quasis), std::move(expressions)));
}

Optional<ESTree::FunctionExpressionNode *>
JSParserImpl::parseFunctionExpression(bool forceEagerly) {
  auto optRes = parseFunctionHelper(Param{}, false, forceEagerly);
  if (!optRes)
    return None;
  return cast<ESTree::FunctionExpressionNode>(*optRes);
}

Optional<ESTree::Node *> JSParserImpl::parseOptionalExpressionExceptNew(
    IsConstructorCall isConstructorCall) {
  SMLoc startLoc = tok_->getStartLoc();

  ESTree::NodePtr expr;
  if (check(TokenKind::rw_super)) {
    // SuperProperty can be used the same way as PrimaryExpression, but
    // must not have a TemplateLiteral immediately after the `super` keyword.
    expr = setLocation(tok_, tok_, new (context_) ESTree::SuperNode());
    advance();
    if (!checkN(TokenKind::l_paren, TokenKind::l_square, TokenKind::period)) {
      errorExpected(
          {TokenKind::l_paren, TokenKind::l_square, TokenKind::period},
          "after 'super' keyword",
          "location of 'super'",
          startLoc);
      return None;
    }
  } else if (check(TokenKind::rw_import)) {
    SMRange importRange = advance();
    if (checkAndEat(TokenKind::period)) {
      // ImportMeta: import . meta
      //                      ^
      if (!check(metaIdent_)) {
        error(tok_->getSourceRange(), "'meta' expected in member expression");
        sm_.note(
            importRange.Start, "start of member expression", Subsystem::Parser);
        return None;
      }
      auto *meta = setLocation(
          importRange,
          importRange,
          new (context_) ESTree::IdentifierNode(importIdent_, nullptr, false));
      auto *prop = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::IdentifierNode(metaIdent_, nullptr, false));
      advance();
      expr = setLocation(
          meta,
          getPrevTokenEndLoc(),
          new (context_) ESTree::MetaPropertyNode(meta, prop));
    } else {
      // Guard against parseAssignmentExpression without parsePrimaryExpression.
      CHECK_RECURSION;

      // ImportCall must be a call with an AssignmentExpression as the
      // argument.
      if (!eat(
              TokenKind::l_paren,
              JSLexer::AllowRegExp,
              "in import call",
              "location of 'import'",
              startLoc))
        return None;

      auto optSource = parseAssignmentExpression(ParamIn);
      if (!optSource)
        return None;
      ESTree::Node *source = *optSource;

      checkAndEat(TokenKind::comma);

      ESTree::Node *attributes = nullptr;
      if (!check(TokenKind::r_paren)) {
        auto optAttributes = parseAssignmentExpression();
        if (!optAttributes)
          return None;
        attributes = *optAttributes;
        checkAndEat(TokenKind::comma);
      }

      SMLoc endLoc = tok_->getEndLoc();
      if (!eat(
              TokenKind::r_paren,
              JSLexer::AllowRegExp,
              "in import call",
              "location of 'import'",
              startLoc))
        return None;

      expr = setLocation(
          startLoc,
          endLoc,
          new (context_) ESTree::ImportExpressionNode(source, attributes));
    }
  } else {
    auto primExpr = parsePrimaryExpression();
    if (!primExpr)
      return None;
    expr = primExpr.getValue();
  }

  return parseOptionalExpressionExceptNew_tail(
      isConstructorCall, startLoc, expr);
}

Optional<ESTree::Node *> JSParserImpl::parseOptionalExpressionExceptNew_tail(
    IsConstructorCall isConstructorCall,
    SMLoc startLoc,
    ESTree::Node *expr) {
  SMLoc objectLoc = startLoc;
  bool seenOptionalChain = false;
  llvh::SaveAndRestore<unsigned> savedRecursionDepth{
      recursionDepth_, recursionDepth_};
  while (
      checkN(TokenKind::l_square, TokenKind::period, TokenKind::questiondot) ||
      checkTemplateLiteral()) {
    ++recursionDepth_;
    if (LLVM_UNLIKELY(recursionDepthCheck())) {
      return None;
    }
    SMLoc nextObjectLoc = tok_->getStartLoc();
    if (checkN(
            TokenKind::l_square, TokenKind::period, TokenKind::questiondot)) {
      if (check(TokenKind::questiondot)) {
        seenOptionalChain = true;
        if (isConstructorCall == IsConstructorCall::Yes) {
          // Report the error here, but continue on because we can still parse
          // the rest of the file.
          error(
              tok_->getSourceRange(),
              "Constructor calls may not contain an optional chain");
        }
      }
      // MemberExpression [ Expression ]
      // MemberExpression . IdentifierName
      // MemberExpression OptionalChain
      auto msel =
          parseMemberSelect(startLoc, objectLoc, expr, seenOptionalChain);
      if (!msel)
        return None;
      objectLoc = nextObjectLoc;
      expr = msel.getValue();
    } else {
      assert(checkTemplateLiteral());
      if (isa<ESTree::SuperNode>(expr)) {
        error(
            expr->getSourceRange(),
            "invalid use of 'super' as a template literal tag");
        return None;
      }
      if (seenOptionalChain) {
        // This construction is allowed by the grammar but accounted for by
        // static semantics in order to prevent ASI from being used like this:
        // \code
        // a?.b
        // `abc`;
        // \endcode
        error(
            tok_->getSourceRange(),
            "invalid use of tagged template literal in optional chain");
        sm_.note(expr->getSourceRange(), "location of optional chain");
      }
      // MemberExpression TemplateLiteral
      auto optTemplate = parseTemplateLiteral(ParamTagged);
      if (!optTemplate)
        return None;
      expr = setLocation(
          startLoc,
          optTemplate.getValue(),
          new (context_) ESTree::TaggedTemplateExpressionNode(
              expr, optTemplate.getValue()));
      objectLoc = nextObjectLoc;
    }
  }

  return expr;
}

Optional<const char *> JSParserImpl::parseArguments(
    ESTree::NodeList &argList,
    SMLoc &endLoc) {
  assert(check(TokenKind::l_paren));
  SMLoc startLoc = advance().Start;
  if (!check(TokenKind::r_paren)) {
    for (;;) {
      SMLoc argStart = tok_->getStartLoc();
      bool isSpread = checkAndEat(TokenKind::dotdotdot);

      auto arg = parseAssignmentExpression();
      if (!arg)
        return None;

      if (isSpread) {
        argList.push_back(*setLocation(
            argStart,
            getPrevTokenEndLoc(),
            new (context_) ESTree::SpreadElementNode(arg.getValue())));
      } else {
        argList.push_back(*arg.getValue());
      }

      if (!checkAndEat(TokenKind::comma))
        break;

      // Check for ",)".
      if (check(TokenKind::r_paren))
        break;
    }
  }
  endLoc = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_paren,
          JSLexer::AllowDiv,
          "at end of function call",
          "location of '('",
          startLoc))
    return None;

  return "OK";
}

Optional<ESTree::Node *> JSParserImpl::parseMemberSelect(
    SMLoc startLoc,
    SMLoc objectLoc,
    ESTree::NodePtr expr,
    bool seenOptionalChain) {
  assert(
      checkN(TokenKind::l_square, TokenKind::period, TokenKind::questiondot));
  SMLoc puncLoc = tok_->getStartLoc();
  bool optional = checkAndEat(TokenKind::questiondot);
  if (checkAndEat(TokenKind::l_square)) {
    // Parsing another Expression directly without going through
    // PrimaryExpression. This can overflow, so check.
    CHECK_RECURSION;
    auto propExpr = parseExpression();
    if (!propExpr)
      return None;
    SMLoc endLoc = tok_->getEndLoc();
    if (!eat(
            TokenKind::r_square,
            JSLexer::AllowDiv,
            "at end of member expression '[...'",
            "location iof '['",
            puncLoc))
      return None;

    if (optional || seenOptionalChain) {
      return setLocation(
          startLoc,
          endLoc,
          puncLoc,
          new (context_) ESTree::OptionalMemberExpressionNode(
              expr, propExpr.getValue(), true, optional));
    }
    return setLocation(
        startLoc,
        endLoc,
        puncLoc,
        new (context_)
            ESTree::MemberExpressionNode(expr, propExpr.getValue(), true));
  } else if (
      checkAndEat(TokenKind::period) ||
      (optional &&
       !(check(TokenKind::l_paren) ||
         (context_.getParseFlow() && check(TokenKind::less))))) {
    if (!check(TokenKind::identifier, TokenKind::private_identifier) &&
        !tok_->isResWord()) {
      // Just use the pattern here, even though we know it will fail.
      if (!need(
              TokenKind::identifier,
              "after '.' or '?.' in member expression",
              "start of member expression",
              objectLoc))
        return None;
    }

    ESTree::Node *id = nullptr;
    if (check(TokenKind::private_identifier)) {
      auto optId = parsePrivateName();
      if (!optId)
        return None;
      id = *optId;
    } else {
      id = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::IdentifierNode(
              tok_->getResWordOrIdentifier(), nullptr, false));
      advance(JSLexer::AllowDiv);
    }

    if (optional || seenOptionalChain) {
      return setLocation(
          startLoc,
          id,
          puncLoc,
          new (context_)
              ESTree::OptionalMemberExpressionNode(expr, id, false, optional));
    }
    return setLocation(
        startLoc,
        id,
        puncLoc,
        new (context_) ESTree::MemberExpressionNode(expr, id, false));
  } else {
    assert(
        optional &&
        (check(TokenKind::l_paren) ||
         (context_.getParseFlow() && check(TokenKind::less))) &&
        "must be ?.() at this point");
    // ?. Arguments :
    // ?. ( ArgumentList )
    //      ^
    auto debugLoc = tok_->getStartLoc();

    ESTree::NodePtr typeArgs = nullptr;
#if HERMES_PARSE_FLOW
    if (context_.getParseFlow() && check(TokenKind::less)) {
      auto optTypeArgs = parseTypeArgsFlow();
      if (!optTypeArgs) {
        return None;
      }

      typeArgs = *optTypeArgs;

      if (!need(
              TokenKind::l_paren,
              "after type arguments in optional call",
              "start of optional call",
              objectLoc))
        return None;
    }
#endif
#if HERMES_PARSE_TS
    if (context_.getParseTS() && check(TokenKind::less)) {
      auto optTypeArgs = parseTSTypeArguments();
      if (!optTypeArgs) {
        return None;
      }

      typeArgs = *optTypeArgs;

      if (!need(
              TokenKind::l_paren,
              "after type arguments in optional call",
              "start of optional call",
              objectLoc))
        return None;
    }
#endif

    ESTree::NodeList argList;
    SMLoc endLoc;
    if (!parseArguments(argList, endLoc))
      return None;

    return setLocation(
        startLoc,
        endLoc,
        debugLoc,
        new (context_) ESTree::OptionalCallExpressionNode(
            expr, typeArgs, std::move(argList), true));
  }

  llvm_unreachable("Invalid token in parseMemberSelect");
}

Optional<ESTree::Node *> JSParserImpl::parseCallExpression(
    SMLoc startLoc,
    ESTree::NodePtr expr,
    ESTree::NodePtr typeArgs,
    bool seenOptionalChain,
    bool optional) {
  assert(checkN(
      TokenKind::l_paren,
      TokenKind::no_substitution_template,
      TokenKind::template_head));

  SMLoc objectLoc = startLoc;

  for (;;) {
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
    if ((context_.getParseFlowAmbiguous() || context_.getParseTS()) &&
        !typeArgs && check(TokenKind::less)) {
      JSLexer::SavePoint savePoint{&lexer_};
      // Each call in a chain may have type arguments.
      // As such, we must attempt to parse them upon encountering '<',
      // but roll back if it just ended up being a comparison operator.
      SourceErrorManager::SaveAndSuppressMessages suppress{
          &sm_, Subsystem::Parser};
      auto optTypeArgs = parseTypeArguments();
      if (optTypeArgs && check(TokenKind::l_paren)) {
        // Call expression with type arguments.
        typeArgs = *optTypeArgs;
      } else {
        // Failed to parse a call expression with type arguments,
        // simply roll back and start again.
        savePoint.restore();
      }
    }
#endif
    if (check(TokenKind::l_paren)) {
      auto debugLoc = tok_->getStartLoc();
      ESTree::NodeList argList;
      SMLoc endLoc;

      // parseArguments can result in another call to parseCallExpression
      // without parsing another primary or declaration.
      CHECK_RECURSION;
      if (!parseArguments(argList, endLoc))
        return None;

      if (seenOptionalChain) {
        expr = setLocation(
            startLoc,
            endLoc,
            debugLoc,
            new (context_) ESTree::OptionalCallExpressionNode(
                expr, typeArgs, std::move(argList), optional));
      } else {
        expr = setLocation(
            startLoc,
            endLoc,
            debugLoc,
            new (context_)
                ESTree::CallExpressionNode(expr, typeArgs, std::move(argList)));
      }

      // typeArgs have been used, discard them so the next item in the call
      // chain can populate them if necessary.
      typeArgs = nullptr;
    } else if (checkN(
                   TokenKind::l_square,
                   TokenKind::period,
                   TokenKind::questiondot)) {
      if (check(TokenKind::questiondot)) {
        seenOptionalChain = true;
      }

      SMLoc nextObjectLoc = tok_->getStartLoc();
      auto msel =
          parseMemberSelect(startLoc, objectLoc, expr, seenOptionalChain);
      if (!msel)
        return None;
      objectLoc = nextObjectLoc;
      expr = msel.getValue();
    } else if (check(
                   TokenKind::no_substitution_template,
                   TokenKind::template_head)) {
      auto debugLoc = tok_->getStartLoc();
      auto optTemplate = parseTemplateLiteral(ParamTagged);
      if (!optTemplate)
        return None;
      expr = setLocation(
          startLoc,
          optTemplate.getValue(),
          debugLoc,
          new (context_) ESTree::TaggedTemplateExpressionNode(
              expr, optTemplate.getValue()));
    } else {
      break;
    }
  }

  return expr;
}

//  Parsing NewExpression, MemberExpression and CallExpression is tricky.
//
//  The key realizations are that NewExpression can be one or more
//  constructor calls without arguments (the s-expressions below represent
//  productions that have been recursively matched, not AST nodes):
//
//       new foo ->
//           (NewExpression (MemberExpression))
//       new new foo ->
//           (NewExpression (NewExpression (MemberExpression)))
//
//  MemberExpression can be one or more constructor calls with arguments:
//
//       new foo() ->
//           (MemberExpression (MemberExpression))
//       new new foo()()
//           (MemberExpression (MemberExpression (MemberExpression))
//
//  Call expression are formed from arguments that don't match up with a `new`:
//
//       foo() ->
//           (CallExpression (MemberExpression))
//       new foo()()
//           (CallExpression (MemberExpression (MemberExpression)))

Optional<ESTree::Node *> JSParserImpl::parseNewExpressionOrOptionalExpression(
    IsConstructorCall isConstructorCall) {
  if (!check(TokenKind::rw_new))
    return parseOptionalExpressionExceptNew(isConstructorCall);

  SMRange newRange = advance();

  if (checkAndEat(TokenKind::period)) {
    // NewTarget: new . target
    //                  ^
    if (!check(targetIdent_)) {
      error(tok_->getSourceRange(), "'target' expected in member expression");
      sm_.note(newRange.Start, "start of member expression");
      return None;
    }
    auto *meta = setLocation(
        newRange,
        newRange,
        new (context_) ESTree::IdentifierNode(newIdent_, nullptr, false));
    auto *prop = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(targetIdent_, nullptr, false));
    advance();
    auto *expr = setLocation(
        meta, prop, new (context_) ESTree::MetaPropertyNode(meta, prop));
    return parseOptionalExpressionExceptNew_tail(
        isConstructorCall, newRange.Start, expr);
  }

  CHECK_RECURSION;
  auto optExpr = parseNewExpressionOrOptionalExpression(IsConstructorCall::Yes);
  if (!optExpr)
    return None;
  ESTree::NodePtr expr = optExpr.getValue();

  ESTree::Node *typeArgs = nullptr;
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
  if ((context_.getParseFlowAmbiguous() || context_.getParseTS()) &&
      check(TokenKind::less)) {
    JSLexer::SavePoint savePoint{&lexer_};
    // Attempt to parse type args upon encountering '<',
    // but roll back if it just ended up being a comparison operator.
    SourceErrorManager::SaveAndSuppressMessages suppress{
        &sm_, Subsystem::Parser};
    auto optTypeArgs = parseTypeArguments();
    if (optTypeArgs) {
      // New expression with type arguments.
      typeArgs = *optTypeArgs;
    } else {
      // Failed to parse a call expression with type arguments,
      // simply roll back and start again.
      savePoint.restore();
    }
  }
#endif

  // Do we have arguments to a child MemberExpression? If yes, then it really
  // was a 'new MemberExpression(args)', otherwise it is a NewExpression
  if (!check(TokenKind::l_paren)) {
    return setLocation(
        newRange,
        getPrevTokenEndLoc(),
        new (context_)
            ESTree::NewExpressionNode(expr, typeArgs, ESTree::NodeList{}));
  }

  auto debugLoc = tok_->getStartLoc();
  ESTree::NodeList argList;
  SMLoc endLoc;
  if (!parseArguments(argList, endLoc))
    return None;

  expr = setLocation(
      newRange,
      endLoc,
      debugLoc,
      new (context_)
          ESTree::NewExpressionNode(expr, typeArgs, std::move(argList)));

  SMLoc objectLoc = newRange.Start;
  while (
      checkN(TokenKind::l_square, TokenKind::period, TokenKind::questiondot)) {
    SMLoc nextObjectLoc = tok_->getStartLoc();
    auto optMSel = parseMemberSelect(newRange.Start, objectLoc, expr, false);
    if (!optMSel)
      return None;
    objectLoc = nextObjectLoc;
    expr = optMSel.getValue();
  }

  return expr;
}

Optional<ESTree::Node *> JSParserImpl::parseLeftHandSideExpression() {
  SMLoc startLoc = tok_->getStartLoc();

  auto optExpr = parseNewExpressionOrOptionalExpression(IsConstructorCall::No);
  if (!optExpr)
    return None;

  return parseLeftHandSideExpressionTail(startLoc, optExpr.getValue());
}

Optional<ESTree::Node *> JSParserImpl::parseLeftHandSideExpressionTail(
    SMLoc startLoc,
    ESTree::Node *expr) {
  bool optional = checkAndEat(TokenKind::questiondot);
  bool seenOptionalChain = optional ||
      (expr->getParens() == 0 &&
       (llvh::isa<ESTree::OptionalMemberExpressionNode>(expr) ||
        llvh::isa<ESTree::OptionalCallExpressionNode>(expr)));

  ESTree::Node *typeArgs = nullptr;
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
  // If the less than sign is immediately following a question dot then it
  // cannot be a binary expression and is unambiguously Flow type syntax.
  if (((optional ? context_.getParseFlow()
                 : context_.getParseFlowAmbiguous()) ||
       context_.getParseTS()) &&
      check(TokenKind::less)) {
    JSLexer::SavePoint savePoint{&lexer_};
    // Suppress messages from the parser while still displaying lexer messages.
    SourceErrorManager::SaveAndSuppressMessages suppress{
        &sm_, Subsystem::Parser};
    auto optTypeArgs = parseTypeArguments();
    if (optTypeArgs && check(TokenKind::l_paren)) {
      // Call expression with type arguments.
      typeArgs = *optTypeArgs;
    } else {
      // Failed to parse a call expression with type arguments,
      // simply roll back and start again.
      savePoint.restore();
    }
  }
#endif

  // Is this a CallExpression?
  if (checkN(
          TokenKind::l_paren,
          TokenKind::no_substitution_template,
          TokenKind::template_head)) {
    auto optCallExpr = parseCallExpression(
        startLoc, expr, typeArgs, seenOptionalChain, optional);
    if (!optCallExpr)
      return None;
    expr = optCallExpr.getValue();
  }

  return expr;
}

Optional<ESTree::Node *> JSParserImpl::parsePostfixExpression() {
  SMLoc startLoc = tok_->getStartLoc();
  auto optLHandExpr = parseLeftHandSideExpression();
  if (!optLHandExpr)
    return None;

  if (check(TokenKind::plusplus, TokenKind::minusminus) &&
      !lexer_.isNewLineBeforeCurrentToken()) {
    auto *res = setLocation(
        startLoc,
        tok_,
        tok_,
        new (context_) ESTree::UpdateExpressionNode(
            getTokenIdent(tok_->getKind()), optLHandExpr.getValue(), false));
    advance(JSLexer::AllowDiv);
    return res;
  } else {
    return optLHandExpr.getValue();
  }
}

Optional<ESTree::Node *> JSParserImpl::parseUnaryExpression() {
  SMLoc startLoc = tok_->getStartLoc();

  switch (tok_->getKind()) {
    case TokenKind::rw_delete:
    case TokenKind::rw_void:
    case TokenKind::rw_typeof:
    case TokenKind::plus:
    case TokenKind::minus:
    case TokenKind::tilde:
    case TokenKind::exclaim: {
      UniqueString *op = getTokenIdent(tok_->getKind());
      advance();
      CHECK_RECURSION;
      auto expr = parseUnaryExpression();
      if (!expr)
        return None;

      if (check(TokenKind::starstar)) {
        // ExponentiationExpression only allows UpdateExpressionNode on the
        // left. The simplest way to enforce that the left operand is not
        // an unparenthesized UnaryExpression is to check here.
        error(
            {startLoc, tok_->getEndLoc()},
            "Unary operator before ** must use parens to disambiguate");
      }

      return setLocation(
          startLoc,
          getPrevTokenEndLoc(),
          new (context_)
              ESTree::UnaryExpressionNode(op, expr.getValue(), true));
    }

    case TokenKind::plusplus:
    case TokenKind::minusminus: {
      UniqueString *op = getTokenIdent(tok_->getKind());
      advance();
      CHECK_RECURSION;
      auto expr = parseUnaryExpression();
      if (!expr)
        return None;

      return setLocation(
          startLoc,
          getPrevTokenEndLoc(),
          new (context_)
              ESTree::UpdateExpressionNode(op, expr.getValue(), true));
    }

#if HERMES_PARSE_TS
    case TokenKind::less:
      if (context_.getParseTS() && !context_.getParseJSX()) {
        // TSTypeAssertions are only parsed if JSX is disabled,
        // so there's no backtracking necessary here.
        // < Type > UnaryExpression
        // ^
        advance(JSLexer::GrammarContext::Type);
        auto optType = parseTypeAnnotationTS();
        if (!optType)
          return None;
        if (!eat(
                TokenKind::greater,
                JSLexer::GrammarContext::AllowRegExp,
                "in type assertion",
                "start of assertion",
                startLoc))
          return None;
        CHECK_RECURSION;
        auto optExpr = parseUnaryExpression();
        if (!optExpr)
          return None;
        return setLocation(
            startLoc,
            getPrevTokenEndLoc(),
            new (context_) ESTree::TSTypeAssertionNode(*optType, *optExpr));
      }
      break;
#endif

    case TokenKind::identifier:
      if (check(awaitIdent_) && paramAwait_) {
        advance();
        CHECK_RECURSION;
        auto optExpr = parseUnaryExpression();
        if (!optExpr)
          return None;
        return setLocation(
            startLoc,
            getPrevTokenEndLoc(),
            new (context_) ESTree::AwaitExpressionNode(optExpr.getValue()));
      }
      // Default for all other identifiers.
      break;

    default:
      break;
  }
  return parsePostfixExpression();
}

namespace {

/// Associates precedence levels with binary operators. Higher precedences are
/// represented by higher values.
/// \returns the precedence level starting from 1, or 0 if not a binop.
inline unsigned getPrecedence(TokenKind kind) {
  // Record the precedence of all binary operators.
  static const unsigned precedence[] = {
#define TOK(...) 0,
#define BINOP(name, str, precedence) precedence,
#define IDENT_OP(name, str, precedence) precedence,

// There are two reserved words that are binary operators.
#define RESWORD(name)                                       \
  (TokenKind::rw_##name == TokenKind::rw_in ||              \
           TokenKind::rw_##name == TokenKind::rw_instanceof \
       ? 8                                                  \
       : 0),
#include "hermes/Parser/TokenKinds.def"
  };

  return precedence[static_cast<unsigned>(kind)];
}

/// \return true if \p kind is left associative, false if right associative.
inline bool isLeftAssoc(TokenKind kind) {
  return kind != TokenKind::starstar;
}

/// Return the precedence of \p token unless it happens to be equal to \p
/// except, in which case return 0.
/// \param asIdent if not null, the "as" UniqueString used to parse TS
///   AsExpressions.
inline unsigned getPrecedenceExcept(const Token *token, TokenKind except) {
  const TokenKind kind = token->getKind();
  return LLVM_LIKELY(kind != except) ? getPrecedence(kind) : 0;
}
} // namespace

inline void JSParserImpl::convertIdentOpIfPossible() {
#if HERMES_PARSE_TS || HERMES_PARSE_FLOW
  if (LLVM_UNLIKELY(tok_->getKind() == TokenKind::identifier) &&
      context_.getParseTypes()) {
    if (tok_->getIdentifier() == asIdent_)
      lexer_.convertCurTokenToIdentOp(TokenKind::as_operator);
  }
#endif
};

Optional<ESTree::Node *> JSParserImpl::parseBinaryExpression(Param param) {
  // The stack can never go deeper than the number of precedence levels,
  // unless we have a right-associative operator.
  // We have 10 precedence levels.
  constexpr unsigned STACK_SIZE = 16;

  struct PrecedenceStackEntry {
    /// Left hand side expression.
    ESTree::NodePtr expr;
    // Operator for this expression.
    TokenKind opKind;
    // Start location for the left hand side expression.
    SMLoc exprStartLoc;

    PrecedenceStackEntry(
        ESTree::NodePtr expr,
        TokenKind opKind,
        SMLoc exprStartLoc)
        : expr(expr), opKind(opKind), exprStartLoc(exprStartLoc) {}
  };
  llvh::SmallVector<PrecedenceStackEntry, STACK_SIZE> stack{};

  // True upon encountering a '??' operator.
  bool hasNullish = false;
  // True upon encountering a '&&' or '||' operator.
  bool hasBoolean = false;

  /// Allocate a binary expression node with the specified children and
  /// operator.
  const auto newBinNode = [this, &hasNullish, &hasBoolean](
                              ESTree::NodePtr left,
                              TokenKind opKind,
                              ESTree::NodePtr right,
                              SMLoc startLoc,
                              SMLoc endLoc) -> ESTree::NodePtr {
    UniqueString *opIdent = getTokenIdent(opKind);
    if (opKind == TokenKind::ampamp || opKind == TokenKind::pipepipe ||
        opKind == TokenKind::questionquestion) {
      if ((hasNullish && opKind != TokenKind::questionquestion) ||
          (hasBoolean && opKind == TokenKind::questionquestion)) {
        // This error doesn't prevent parsing the rest of the binary expression,
        // because it's only there to avoid confusion from the JS author's
        // perspective. Report the error but continue parsing.
        // The question marks are escaped to avoid triggering a trigraph.
        error(
            {left->getStartLoc(), right->getEndLoc()},
            "Mixing '\?\?' with '&&' or '||' requires parentheses");
      }
      if (opKind == TokenKind::questionquestion) {
        hasNullish = true;
      } else {
        hasBoolean = true;
      }
      return setLocation(
          startLoc,
          endLoc,
          new (context_) ESTree::LogicalExpressionNode(left, right, opIdent));
#if HERMES_PARSE_TS || HERMES_PARSE_FLOW
    } else if (LLVM_UNLIKELY(opKind == TokenKind::as_operator)) {
#if HERMES_PARSE_TS
      if (context_.getParseTS()) {
        return setLocation(
            startLoc,
            endLoc,
            new (context_) ESTree::TSAsExpressionNode(left, right));
      }
#endif
#if HERMES_PARSE_FLOW
      assert(context_.getParseFlow() && "must be parsing types");
      if (auto *gen = llvh::dyn_cast<ESTree::GenericTypeAnnotationNode>(right);
          gen && !gen->_typeParameters && gen->getParens() == 0) {
        if (auto *ident = llvh::dyn_cast<ESTree::IdentifierNode>(gen->_id)) {
          if (ident->_name == constIdent_ && !ident->_optional &&
              !ident->_typeAnnotation) {
            // Special case for `x as const`,
            // which only is used when the `const` type has no parens
            // (otherwise, it's just a GenericTypeAnnotationNode).
            return setLocation(
                startLoc,
                endLoc,
                new (context_) ESTree::AsConstExpressionNode(left));
          }
        }
      }
      return setLocation(
          startLoc,
          endLoc,
          new (context_) ESTree::AsExpressionNode(left, right));
#endif
      llvm_unreachable("Must be parsing types");
#endif
    } else {
      return setLocation(
          startLoc,
          endLoc,
          new (context_) ESTree::BinaryExpressionNode(left, right, opIdent));
    }
  };

  /// Parse a private identifier, which can only be used as LHS in an `in`.
  /// If it's not in a valid position, report an error.
  const auto consumePrivateIdentifier = [this, &stack]() -> ESTree::Node * {
    assert(check(TokenKind::private_identifier));
    ESTree::Node *name = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::PrivateNameNode(setLocation(
            tok_,
            tok_,
            new (context_) ESTree::IdentifierNode(
                tok_->getPrivateIdentifier(), nullptr, false))));
    advance(JSLexer::GrammarContext::AllowDiv);
    unsigned prevPrec = stack.empty() ? 0 : getPrecedence(stack.back().opKind);
    // Check the precedence of the previous operator on the stack if it exists.
    // If prevPrec is higher precedence than `in`, the private name will end
    // up as the RHS in an invalid binary expression, so report an error.
    if (!check(TokenKind::rw_in) || prevPrec >= getPrecedence(TokenKind::rw_in))
      error(
          name->getSourceRange(),
          "Private name can only be used as left-hand side of `in` expression");
    return name;
  };

  // Decide whether to recognize "in" as a binary operator.
  const TokenKind exceptKind =
      !param.has(ParamIn) ? TokenKind::rw_in : TokenKind::none;

  SMLoc topExprStartLoc = tok_->getStartLoc();
  ESTree::Node *topExpr = nullptr;
  if (LLVM_UNLIKELY(check(TokenKind::private_identifier))) {
    topExpr = consumePrivateIdentifier();
  } else {
    auto optExpr = parseUnaryExpression();
    if (!optExpr)
      return None;
    topExpr = optExpr.getValue();
  }
  SMLoc topExprEndLoc = getPrevTokenEndLoc();
  convertIdentOpIfPossible();

  // While the current token is a binary operator.
  while (unsigned precedence = getPrecedenceExcept(tok_, exceptKind)) {
    // If the next operator has no greater precedence than the operator on the
    // stack, pop the stack, creating a new binary expression.
    while (!stack.empty() && precedence <= getPrecedence(stack.back().opKind)) {
      if (precedence == getPrecedence(stack.back().opKind) &&
          !isLeftAssoc(stack.back().opKind)) {
        // If the precedences are equal, then we avoid popping for
        // right-associative operators to allow for the entire right-associative
        // expression to be built from the right.
        break;
      }
      topExpr = newBinNode(
          stack.back().expr,
          stack.back().opKind,
          topExpr,
          stack.back().exprStartLoc,
          topExprEndLoc);
      topExprStartLoc = topExpr->getStartLoc();
      stack.pop_back();
    }

    // The next operator has a higher precedence than the previous one (or there
    // is no previous one). The situation looks something like this:
    //    .... + topExpr * rightExpr ....
    //                     ^
    //                 We are here
    // Push topExpr and the '*', so we can parse rightExpr.
    stack.emplace_back(topExpr, tok_->getKind(), topExprStartLoc);

#if HERMES_PARSE_TS || HERMES_PARSE_FLOW
    if (LLVM_UNLIKELY(stack.back().opKind == TokenKind::as_operator)) {
      advance(JSLexer::GrammarContext::Type);
      topExprStartLoc = tok_->getStartLoc();
      auto optRightExpr = parseTypeAnnotation();
      if (!optRightExpr)
        return None;
      topExpr = optRightExpr.getValue();
    } else
#endif
    {
      advance();
      topExprStartLoc = tok_->getStartLoc();
      if (LLVM_UNLIKELY(check(TokenKind::private_identifier))) {
        topExpr = consumePrivateIdentifier();
      } else {
        auto optRightExpr = parseUnaryExpression();
        if (!optRightExpr)
          return None;
        topExpr = optRightExpr.getValue();
      }
    }

    topExprEndLoc = getPrevTokenEndLoc();
    convertIdentOpIfPossible();
  }

  // We have consumed all binary operators. Pop the stack, creating expressions.
  while (!stack.empty()) {
    topExpr = newBinNode(
        stack.back().expr,
        stack.back().opKind,
        topExpr,
        stack.back().exprStartLoc,
        topExprEndLoc);
    stack.pop_back();
  }

  assert(
      stack.empty() &&
      "Stack must be empty when done parsing binary expression");

  return topExpr;
}

Optional<ESTree::Node *> JSParserImpl::parseConditionalExpression(
    Param param,
    CoverTypedParameters coverTypedParameters) {
  SMLoc startLoc = tok_->getStartLoc();
  auto optTest = parseBinaryExpression(param);
  if (!optTest)
    return None;
  ESTree::Node *test = *optTest;

  if (!check(TokenKind::question)) {
    // No '?', so this isn't a conditional expression.
    // If CoverTypedParameters::Yes, we still need to account for this
    // being formal parameters, so try that.
    // TS doesn't have type casts with this syntax, but we must still
    // cover the typed identifier node for when it turns into an arrow function.
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
    if (context_.getParseTypes() &&
        coverTypedParameters == CoverTypedParameters::Yes) {
      auto optCover = tryParseCoverTypedIdentifierNode(test, false);
      if (!optCover)
        return None;
      if (*optCover)
        return *optCover;
    }
#endif

    // No CoverTypedParameters found, just return the LHS.
    return test;
  }

  ESTree::Node *consequent = nullptr;
  SMRange questionRange = tok_->getSourceRange();

#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
  if (context_.getParseTypes()) {
    // Save here to save the question mark (we can only save on punctuators).
    // Early returns will happen if we find anything that leads to
    // short-circuiting out of the traditional conditional expression.
    JSLexer::SavePoint savePoint{&lexer_};
    advance();

    // If CoverTypedParameters::Yes, we still need to account for this
    // being formal parameters, so try that,
    // in which case the '?' was part of an optional parameter, not a
    // conditional expression.
    if (coverTypedParameters == CoverTypedParameters::Yes) {
      auto optCover = tryParseCoverTypedIdentifierNode(test, true);
      if (!optCover)
        return None;
      if (*optCover)
        return *optCover;
    }

    // It is also possible to have a '?' without ':' but not be a conditional
    // expression, in the case of typed arrow parameters that didn't have a type
    // annotation. For example:
    // (foo?) => 1
    //      ^
    // The tokens which can come here are limited to ',', '=', and ')'.
    if (coverTypedParameters == CoverTypedParameters::Yes &&
        checkN(TokenKind::comma, TokenKind::r_paren, TokenKind::equal)) {
      return setLocation(
          startLoc,
          questionRange,
          new (context_) ESTree::CoverTypedIdentifierNode(test, nullptr, true));
    }

    // Now we're in the real backtracking stage.
    // First, parse with AllowTypedArrowFunction::Yes to allow for the
    // possibility of a concise arrow function with return types. However, we
    // want to avoid the possibility of eating the ':' that we'll need for the
    // conditional expression's alternate. For example:
    // a ? b1 => (c1) : b2 => (c2)
    // We want to account for b2 incorrectly being parsed as the returnType
    // of an arrow function returned by the arrow function with param b1.
    // Thus, after parsing with AllowTypedArrowFunction::Yes, we check to
    // see if there is a ':' afterwards. If there isn't, failure is assured,
    // so we restore to the '?' and try again below, with
    // AllowTypedArrowFunction::No.
    SourceErrorManager::SaveAndSuppressMessages suppress{
        &sm_, Subsystem::Parser};
    CHECK_RECURSION;
    auto optConsequent = parseAssignmentExpression(
        ParamIn, AllowTypedArrowFunction::Yes, CoverTypedParameters::No);
    if (optConsequent && check(TokenKind::colon)) {
      consequent = *optConsequent;
    } else {
      // Parsing with typed arrow functions failed because we don't have a :,
      // so reset and try again.
      savePoint.restore();
    }
  }
#endif

  // Calls to parseAssignmentExpression may recursively invoke
  // parseConditionalExpression.
  CHECK_RECURSION;

  // Only try with AllowTypedArrowFunction::No if we haven't already set
  // up the consequent using AllowTypedArrowFunction::Yes.
  if (!consequent) {
    // Consume the '?' (either for the first time or after savePoint.restore()).
    advance();
    auto optConsequent = parseAssignmentExpression(
        ParamIn, AllowTypedArrowFunction::No, CoverTypedParameters::No);
    if (!optConsequent)
      return None;
    consequent = *optConsequent;
  }

  if (!eat(
          TokenKind::colon,
          JSLexer::AllowRegExp,
          "in conditional expression after '... ? ...'",
          "location of '?'",
          questionRange.Start))
    return None;

  auto optAlternate = parseAssignmentExpression(
      param, AllowTypedArrowFunction::Yes, CoverTypedParameters::No);
  if (!optAlternate)
    return None;
  ESTree::Node *alternate = *optAlternate;

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_)
          ESTree::ConditionalExpressionNode(test, alternate, consequent));
}

#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
Optional<ESTree::Node *> JSParserImpl::tryParseCoverTypedIdentifierNode(
    ESTree::Node *test,
    bool optional) {
  assert(context_.getParseTypes() && "must be parsing types");
  // In the case of flow types in arrow function parameters, we may have
  // optional parameters which look like:
  // Identifier ? : TypeAnnotation
  // Because the colon and the type annotation are optional, we check and
  // consume the colon here and return a CoverTypedIdentifierNode if it's
  // possible we are parsing typed arrow parameters.
  if (check(TokenKind::colon) && test->getParens() == 0) {
    if (isa<ESTree::IdentifierNode>(test) ||
        isa<ESTree::ObjectExpressionNode>(test) ||
        isa<ESTree::ArrayExpressionNode>(test)) {
      // Deliberately wrap the type annotation later when reparsing.
      SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
      auto optRet = parseTypeAnnotation(annotStart);
      if (!optRet)
        return None;
      ESTree::Node *type = *optRet;

      return setLocation(
          test,
          getPrevTokenEndLoc(),
          new (context_)
              ESTree::CoverTypedIdentifierNode(test, type, optional));
    }
    // The colon must indicate something another than the typeAnnotation for
    // the parameter. Continue as usual.
  }
  return nullptr;
}
#endif

Optional<ESTree::YieldExpressionNode *> JSParserImpl::parseYieldExpression(
    Param param) {
  assert(
      paramYield_ && check(TokenKind::rw_yield, TokenKind::identifier) &&
      tok_->getResWordOrIdentifier() == yieldIdent_ &&
      "yield expression must start with 'yield'");
  SMRange yieldLoc = advance();

  if (check(TokenKind::semi) ||
      checkEndAssignmentExpression(OfEndsAssignment::No)) {
    // 'of' doesn't end the assignment expression in a yield.
    //    yield of;
    //          ^
    // is a valid position here and should simply yield a variable called 'of'.
    return setLocation(
        yieldLoc,
        yieldLoc,
        new (context_) ESTree::YieldExpressionNode(nullptr, false));
  }

  bool delegate = checkAndEat(TokenKind::star);

  auto optArg = parseAssignmentExpression(
      param.get(ParamIn),
      AllowTypedArrowFunction::Yes,
      CoverTypedParameters::No);
  if (!optArg)
    return None;

  return setLocation(
      yieldLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::YieldExpressionNode(optArg.getValue(), delegate));
}

Optional<ESTree::ClassDeclarationNode *> JSParserImpl::parseClassDeclaration(
    Param param) {
  assert(check(TokenKind::rw_class) && "class must start with 'class'");
  // NOTE: Class definition is always strict mode code.
  SaveStrictModeAndSeenDirectives saveStrictModeAndSeenDirectives{this};
  setStrictMode(true);

  SMLoc startLoc = advance().Start;

  ESTree::Node *name = nullptr;
  ESTree::Node *typeParams = nullptr;

  if (check(TokenKind::identifier)) {
    auto optName = parseBindingIdentifier(Param{});
    if (!optName) {
      errorExpected(
          TokenKind::identifier,
          "in class declaration",
          "location of 'class'",
          startLoc);
      return None;
    }
    name = *optName;
  } else if (!param.has(ParamDefault)) {
    // Identifier is required unless we have +Default parameter.
    errorExpected(
        TokenKind::identifier,
        "after 'class'",
        "location of 'class'",
        startLoc);
    return None;
  }

#if HERMES_PARSE_FLOW
  if (context_.getParseFlow() && check(TokenKind::less)) {
    auto optParams = parseTypeParamsFlow();
    if (!optParams)
      return None;
    typeParams = *optParams;
  }
#endif
#if HERMES_PARSE_TS
  if (context_.getParseTS() && check(TokenKind::less)) {
    auto optParams = parseTSTypeParameters();
    if (!optParams)
      return None;
    typeParams = *optParams;
  }
#endif

  auto optClass =
      parseClassTail(startLoc, name, typeParams, ClassParseKind::Declaration);
  if (!optClass)
    return None;
  return llvh::cast<ESTree::ClassDeclarationNode>(*optClass);
}

Optional<ESTree::ClassExpressionNode *> JSParserImpl::parseClassExpression() {
  assert(check(TokenKind::rw_class) && "class must start with 'class'");
  // NOTE: A class definition is always strict mode code.
  SaveStrictModeAndSeenDirectives saveStrictModeAndSeenDirectives{this};
  setStrictMode(true);

  SMLoc start = advance().Start;

  ESTree::Node *name = nullptr;
  ESTree::Node *typeParams = nullptr;

  if (!check(TokenKind::rw_extends, TokenKind::l_brace) &&
      !(context_.getParseFlow() &&
        check(TokenKind::rw_implements, TokenKind::less)) &&
      !(context_.getParseTS() && check(TokenKind::less))) {
    // Try to parse a BindingIdentifier if we did not see a ClassHeritage
    // or a '{'.
    auto optName = parseBindingIdentifier(Param{});
    if (!optName) {
      errorExpected(
          TokenKind::identifier,
          "in class expression",
          "location of 'class'",
          start);
      return None;
    }
    name = *optName;
  }

#if HERMES_PARSE_FLOW
  if (context_.getParseFlow() && check(TokenKind::less)) {
    auto optParams = parseTypeParamsFlow();
    if (!optParams)
      return None;
    typeParams = *optParams;
  }
#endif
#if HERMES_PARSE_TS
  if (context_.getParseTS() && check(TokenKind::less)) {
    auto optParams = parseTSTypeParameters();
    if (!optParams)
      return None;
    typeParams = *optParams;
  }
#endif

  auto optClass =
      parseClassTail(start, name, typeParams, ClassParseKind::Expression);
  if (!optClass)
    return None;
  return llvh::cast<ESTree::ClassExpressionNode>(*optClass);
}

Optional<ESTree::Node *> JSParserImpl::parseClassTail(
    SMLoc startLoc,
    ESTree::Node *name,
    ESTree::Node *typeParams,
    ClassParseKind kind) {
  ESTree::Node *superClass = nullptr;
  ESTree::Node *superTypeParams = nullptr;

  if (checkAndEat(TokenKind::rw_extends)) {
    // ClassHeritage[opt] { ClassBody[opt] }
    // ^
    auto optSuperClass = parseLeftHandSideExpression();
    if (!optSuperClass)
      return None;
    superClass = *optSuperClass;
#if HERMES_PARSE_FLOW
    if (context_.getParseFlow() && check(TokenKind::less)) {
      auto optParams = parseTypeArgsFlow();
      if (!optParams)
        return None;
      superTypeParams = *optParams;
    }
#endif
#if HERMES_PARSE_TS
    if (context_.getParseTS() && check(TokenKind::less)) {
      auto optParams = parseTSTypeArguments();
      if (!optParams)
        return None;
      superTypeParams = *optParams;
    }
#endif
  }

  ESTree::NodeList implements{};
#if HERMES_PARSE_FLOW
  if (context_.getParseFlow()) {
    if (checkAndEat(TokenKind::rw_implements) ||
        checkAndEat(implementsIdent_)) {
      while (!check(TokenKind::l_brace)) {
        if (!need(
                TokenKind::identifier,
                "in class 'implements'",
                "start of class",
                startLoc))
          return None;
        auto optImpl = parseClassImplementsFlow();
        if (!optImpl)
          return None;
        implements.push_back(**optImpl);
        if (!checkAndEat(TokenKind::comma)) {
          break;
        }
      }
    }
  }
#endif

  if (!need(
          TokenKind::l_brace,
          "in class definition",
          "start of class",
          startLoc)) {
    return None;
  }

  auto optBody = parseClassBody(startLoc);
  if (!optBody)
    return None;

  if (kind == ClassParseKind::Declaration) {
    return setLocation(
        startLoc,
        *optBody,
        new (context_) ESTree::ClassDeclarationNode(
            name,
            typeParams,
            superClass,
            superTypeParams,
            std::move(implements),
            {},
            *optBody));
  }
  return setLocation(
      startLoc,
      *optBody,
      new (context_) ESTree::ClassExpressionNode(
          name,
          typeParams,
          superClass,
          superTypeParams,
          std::move(implements),
          {},
          *optBody));
}

Optional<ESTree::ClassBodyNode *> JSParserImpl::parseClassBody(SMLoc startLoc) {
  assert(check(TokenKind::l_brace) && "class body must begin with '{'");
  SMLoc braceLoc = advance().Start;

  // It is a Syntax Error if PrototypePropertyNameList of ClassElementList
  // contains more than one occurrence of  "constructor".
  ESTree::Node *constructor = nullptr;

  ESTree::NodeList body{};
  while (!check(TokenKind::r_brace)) {
    bool isStatic = false;
    SMRange startRange = tok_->getSourceRange();

    bool declare = false;
    bool readonly = false;
    ESTree::NodeLabel accessibility = nullptr;

#if HERMES_PARSE_FLOW
    if (context_.getParseFlow() && check(declareIdent_)) {
      // Check for "declare" class properties.
      auto optNext = lexer_.lookahead1(llvh::None);
      if (optNext.hasValue() &&
          (*optNext == TokenKind::rw_static ||
           *optNext == TokenKind::identifier || *optNext == TokenKind::plus ||
           *optNext == TokenKind::minus)) {
        declare = true;
        advance();
      }
    }
#endif

#if HERMES_PARSE_TS
    if (context_.getParseTS()) {
      // In TS, modifiers may appear in this order: accessibility - static -
      // readonly. And all of them can be used as identifier.
      if (checkN(
              TokenKind::rw_private,
              TokenKind::rw_protected,
              TokenKind::rw_public)) {
        if (canFollowModifierTS(lexer_.lookahead1(llvh::None))) {
          accessibility = tok_->getResWordIdentifier();
          advance();
        }
      }

      if (check(TokenKind::rw_static)) {
        if (canFollowModifierTS(lexer_.lookahead1(llvh::None))) {
          isStatic = true;
          advance();
        }
      }

      if (check(readonlyIdent_)) {
        if (canFollowModifierTS(lexer_.lookahead1(llvh::None))) {
          readonly = true;
          advance();
        }
      }
    }
#endif

    switch (tok_->getKind()) {
      case TokenKind::semi:
        advance();
        break;

      case TokenKind::rw_static:
        if (context_.getParseTS() && (readonly || isStatic)) {
          // Don't advance() when `readonly` or `static` is already seen,
          // so the current one can be regarded as an identifier.
          // `static` modifier cannot come after `readonly` in TS.
        } else {
          // static MethodDefinition
          // static FieldDefinition
          isStatic = true;
          advance();
        }
        LLVM_FALLTHROUGH;
      default: {
        // ClassElement
        auto optElem = parseClassElement(
            isStatic, startRange, declare, readonly, accessibility);
        if (!optElem)
          return None;
        if (auto *method = dyn_cast<ESTree::MethodDefinitionNode>(*optElem)) {
          if (method->_kind == constructorIdent_) {
            if (constructor) {
              // Cannot have duplicate constructors, but report the error
              // and move on to parse the rest of the class.
              error(
                  method->getSourceRange(), "duplicate constructors in class");
              sm_.note(
                  constructor->getSourceRange(),
                  "first constructor definition",
                  Subsystem::Parser);
            } else {
              constructor = method;
            }
          }
        } else if (auto *prop = dyn_cast<ESTree::ClassPropertyNode>(*optElem)) {
          if (auto *propId = dyn_cast<ESTree::IdentifierNode>(prop->_key)) {
            if (propId->_name == constructorIdent_) {
              error(prop->getSourceRange(), "invalid class property name");
            }
          } else if (
              auto *propStr = dyn_cast<ESTree::StringLiteralNode>(prop->_key)) {
            if (propStr->_value == constructorIdent_) {
              error(prop->getSourceRange(), "invalid class property name");
            }
          }
        }

        body.push_back(**optElem);
      }
    }
  }

  if (!need(
          TokenKind::r_brace,
          "at end of class definition",
          "start of class",
          startLoc)) {
    return None;
  }

  return setLocation(
      braceLoc,
      advance().End,
      new (context_) ESTree::ClassBodyNode(std::move(body)));
}

Optional<ESTree::Node *> JSParserImpl::parseClassElement(
    bool isStatic,
    SMRange startRange,
    bool declare,
    bool readonly,
    ESTree::NodeLabel accessibility,
    bool eagerly) {
  SMLoc startLoc = tok_->getStartLoc();

  bool optional = false;
  bool isPrivate = false;

  enum class SpecialKind {
    None,
    Get,
    Set,
    Generator,
    Async,
    AsyncGenerator,
    ClassProperty,
  };

  // Indicates if this method is out of the ordinary.
  // In particular, indicates getters and setters.
  SpecialKind special = SpecialKind::None;

  // When true, call parsePropertyName.
  // Set to false if the identifiers 'get' or 'set' were already parsed as
  // function names instead of as getter/setter specifiers.
  bool doParsePropertyName = true;

  /// \return true when the current token indicates that `static` was actually
  /// the property name and not a modifier.
  /// e.g. `static() {}` returns true when the current tok is '(',
  /// but `static x;` returns false when the current tok is 'x'.
  /// When this returns `true`, we're done parsing the property name,
  /// and we might have to use `static` as the property name if it was given in.
  const auto staticIsPropertyName = [this]() -> bool {
    if (checkN(
            TokenKind::l_paren,
            TokenKind::equal,
            TokenKind::r_brace,
            TokenKind::semi)) {
      return true;
    }
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
    if (context_.getParseTypes() && check(TokenKind::less, TokenKind::colon)) {
      return true;
    }
#endif
    return false;
  };

  ESTree::Node *prop = nullptr;
  if (check(getIdent_)) {
    SMRange range = advance();
    if (!checkN(
            TokenKind::less,
            TokenKind::l_paren,
            TokenKind::r_brace,
            TokenKind::equal,
            TokenKind::colon,
            TokenKind::semi)) {
      // This was actually a getter.
      special = SpecialKind::Get;
    } else {
      prop = setLocation(
          range,
          range,
          new (context_) ESTree::IdentifierNode(getIdent_, nullptr, false));
      doParsePropertyName = false;
    }
  } else if (check(setIdent_)) {
    SMRange range = advance();
    if (!checkN(
            TokenKind::less,
            TokenKind::l_paren,
            TokenKind::r_brace,
            TokenKind::equal,
            TokenKind::colon,
            TokenKind::semi)) {
      // If we don't see '(' then this was actually a setter.
      special = SpecialKind::Set;
    } else {
      prop = setLocation(
          range,
          range,
          new (context_) ESTree::IdentifierNode(setIdent_, nullptr, false));
      doParsePropertyName = false;
    }
  } else if (check(asyncIdent_)) {
    SMRange range = advance();
    if (!checkN(
            TokenKind::less,
            TokenKind::l_paren,
            TokenKind::r_brace,
            TokenKind::equal,
            TokenKind::colon,
            TokenKind::semi) &&
        !lexer_.isNewLineBeforeCurrentToken()) {
      // If we don't see '(' then this was actually an async method.
      // Async methods cannot have a newline between 'async' and the name.
      // These can be either Async or AsyncGenerator, so check for that.
      special = checkAndEat(TokenKind::star) ? SpecialKind::AsyncGenerator
                                             : SpecialKind::Async;
    } else {
      prop = setLocation(
          range,
          range,
          new (context_) ESTree::IdentifierNode(asyncIdent_, nullptr, false));
      doParsePropertyName = false;
    }
  } else if (checkAndEat(TokenKind::star)) {
    special = SpecialKind::Generator;
  } else if (isStatic && checkAndEat(TokenKind::l_brace)) {
    // This is a static block.
    // ES14.0 15.7
    // ClassStaticBlock :
    //   static { ClassStaticBlockBody }
    //          ^
    SMLoc braceLoc = tok_->getStartLoc();
    ESTree::NodeList body;

    {
      // ClassStaticBlockStatementList :
      //   StatementList[~Yield, +Await, ~Return]opt
      //   ^
      llvh::SaveAndRestore oldParamYield{paramYield_, false};
      llvh::SaveAndRestore oldParamAwait{paramAwait_, true};
      if (!parseStatementList(
              Param{},
              TokenKind::r_brace,
              /* parseDirectives */ false,
              AllowImportExport::No,
              body)) {
        return None;
      }
    }
    if (!eat(
            TokenKind::r_brace,
            JSLexer::GrammarContext::AllowRegExp,
            "at end of static block",
            "static block starts here",
            braceLoc))
      return None;

    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::StaticBlockNode(std::move(body)));
  } else if (isStatic && staticIsPropertyName()) {
    // This is the name of the property/method.
    // We've already parsed 'static', but it must be used as the
    // PropertyName and not as an indicator for a static function.
    prop = setLocation(
        startRange,
        startRange,
        new (context_) ESTree::IdentifierNode(staticIdent_, nullptr, false));
    isStatic = false;
    doParsePropertyName = false;
  }

  ESTree::Node *variance = nullptr;
#if HERMES_PARSE_FLOW
  if (context_.getParseFlow() && check(TokenKind::plus, TokenKind::minus)) {
    variance = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::VarianceNode(
            check(TokenKind::plus) ? plusIdent_ : minusIdent_));
    advance(JSLexer::GrammarContext::Type);
  }
#endif

  bool computed = false;
  if (doParsePropertyName) {
    if (check(TokenKind::private_identifier)) {
      isPrivate = true;
      prop = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::IdentifierNode(
              tok_->getPrivateIdentifier(), nullptr, false));
      advance();
    } else {
      computed = check(TokenKind::l_square);
      auto optProp = parsePropertyName();
      if (!optProp)
        return None;
      prop = *optProp;
    }
  }

  // Store the propName for comparisons, used for SyntaxErrors.
  UniqueString *propName = nullptr;
  if (auto *id = dyn_cast<ESTree::IdentifierNode>(prop)) {
    propName = id->_name;
  } else if (auto *str = dyn_cast<ESTree::StringLiteralNode>(prop)) {
    propName = str->_value;
  }

  bool isConstructor =
      !isStatic && !computed && propName && propName->str() == "constructor";

  if (special == SpecialKind::None &&
      !check(TokenKind::less, TokenKind::l_paren)) {
    // Parse a class property, because this can't be a method definition.
    // Attempt ASI after the fact, and continue on, letting the next iteration
    // error if it wasn't actually a class property.
    // FieldDefinition ;
    //                 ^
    ESTree::Node *typeAnnotation = nullptr;
#if HERMES_PARSE_TS
    if (context_.getParseTS() && checkAndEat(TokenKind::question)) {
      optional = true;
    }
#endif
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
    if (context_.getParseTypes() && check(TokenKind::colon)) {
      SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
      auto optType = parseTypeAnnotation(annotStart);
      if (!optType)
        return None;
      typeAnnotation = *optType;
    }
#endif
    ESTree::Node *value = nullptr;
    if (checkAndEat(TokenKind::equal)) {
      // ClassElementName Initializer[opt]
      //                  ^
      // NOTE: This is technically non-compliant, but having yield/await in the
      // field initializer doesn't make sense.
      // See https://github.com/tc39/ecma262/issues/3333
      // Do [~Yield, +Await, ~Return] as suggested and error in resolution.
      llvh::SaveAndRestore<bool> saveParamYield{paramYield_, false};
      llvh::SaveAndRestore<bool> saveParamAwait{paramAwait_, true};
      auto optValue = parseAssignmentExpression();
      if (!optValue)
        return None;
      value = *optValue;
      if (declare) {
        error(startRange, "Invalid 'declare' with initializer");
      }
    }
    // ASI is allowed for separating class elements.
    if (!eatSemi(true) && !typeAnnotation) {
      errorExpected(
          TokenKind::semi,
          "after class property",
          "start of class property",
          startRange.Start);
      return None;
    }
    if (isPrivate) {
      if (accessibility) {
        error(
            startRange,
            "An accessibility modifier cannot be used with a private identifier");
      }
      ESTree::Node *modifiers = nullptr;
#if HERMES_PARSE_TS
      if (context_.getParseTS()) {
        modifiers = new (context_) ESTree::TSModifiersNode(nullptr, readonly);
      }
#endif
      return setLocation(
          prop,
          getPrevTokenEndLoc(),
          new (context_) ESTree::ClassPrivatePropertyNode(
              prop,
              value,
              isStatic,
              declare,
              optional,
              variance,
              typeAnnotation,
              modifiers));
    }
    ESTree::Node *modifiers = nullptr;
#if HERMES_PARSE_TS
    if (context_.getParseTS()) {
      modifiers =
          new (context_) ESTree::TSModifiersNode(accessibility, readonly);
    }
#endif
    return setLocation(
        startRange,
        getPrevTokenEndLoc(),
        new (context_) ESTree::ClassPropertyNode(
            prop,
            value,
            computed,
            isStatic,
            declare,
            optional,
            variance,
            typeAnnotation,
            modifiers));
  }

  if (declare) {
    error(startRange, "Invalid 'declare' in class method");
  }

  SMLoc funcExprStartLoc = tok_->getStartLoc();

  ESTree::Node *typeParams = nullptr;
#if HERMES_PARSE_FLOW
  if (context_.getParseFlow() && check(TokenKind::less)) {
    auto optTypeParams = parseTypeParamsFlow();
    if (!optTypeParams)
      return None;
    typeParams = *optTypeParams;
  }
#endif

  // (
  if (!need(
          TokenKind::l_paren,
          "in method definition",
          "start of method definition",
          startLoc))
    return None;
  ESTree::NodeList args{};

  llvh::SaveAndRestore<bool> saveArgsAndBodyParamYield(
      paramYield_,
      special == SpecialKind::Generator ||
          special == SpecialKind::AsyncGenerator);

  llvh::SaveAndRestore<bool> saveArgsAndBodyParamAwait(
      paramAwait_,
      special == SpecialKind::Async || special == SpecialKind::AsyncGenerator);

  if (!parseFormalParameters(Param{}, args))
    return None;

  ESTree::Node *returnType = nullptr;
#if HERMES_PARSE_FLOW || HERMES_PARSE_TS
  if (context_.getParseTypes() && check(TokenKind::colon)) {
    SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
    auto optRet = parseReturnTypeAnnotation(annotStart);
    if (!optRet)
      return None;
    returnType = *optRet;
  }
#endif

  if (!need(
          TokenKind::l_brace,
          "in method definition",
          "start of method definition",
          startLoc))
    return None;

  auto optBody = parseFunctionBody(
      ParamReturn,
      eagerly,
      saveArgsAndBodyParamYield.get(),
      saveArgsAndBodyParamAwait.get(),
      JSLexer::AllowRegExp,
      true);
  if (!optBody)
    return None;

  auto *funcExpr = setLocation(
      funcExprStartLoc,
      optBody.getValue(),
      new (context_) ESTree::FunctionExpressionNode(
          nullptr,
          std::move(args),
          optBody.getValue(),
          typeParams,
          returnType,
          /* predicate */ nullptr,
          special == SpecialKind::Generator ||
              special == SpecialKind::AsyncGenerator,
          special == SpecialKind::Async ||
              special == SpecialKind::AsyncGenerator));
  assert(isStrictMode() && "parseClassElement should only be used for classes");
  funcExpr->isMethodDefinition = true;

  if (special == SpecialKind::Get && funcExpr->_params.size() != 0) {
    error(
        {startLoc, funcExpr->getEndLoc()},
        Twine("getter method must no one formal arguments, found ") +
            Twine(funcExpr->_params.size()));
  }

  if (special == SpecialKind::Set && funcExpr->_params.size() != 1) {
    error(
        {startLoc, funcExpr->getEndLoc()},
        Twine("setter method must have exactly one formal argument, found ") +
            Twine(funcExpr->_params.size()));
  }

#if HERMES_PARSE_FLOW
  if ((special == SpecialKind::Get || special == SpecialKind::Set) &&
      typeParams != nullptr) {
    error(
        {startLoc, funcExpr->getEndLoc()},
        "accessor method may not have type parameters");
  }
#endif

  if (isStatic && propName && propName->str() == "prototype") {
    // ClassElement : static MethodDefinition
    // It is a Syntax Error if PropName of MethodDefinition is "prototype".
    error(
        {startLoc, funcExpr->getEndLoc()},
        "prototype method must not be static");
    return None;
  }

  UniqueString *kind = methodIdent_;
  if (isConstructor) {
    if (special != SpecialKind::None) {
      // It is a Syntax Error if PropName of MethodDefinition is "constructor"
      // and SpecialMethod of MethodDefinition is true.
      // TODO: Account for generator methods in SpecialMethod here.
      error(
          {startLoc, funcExpr->getEndLoc()},
          "constructor method must not be a getter or setter");
      return None;
    }
    kind = constructorIdent_;
  } else if (special == SpecialKind::Get) {
    kind = getIdent_;
  } else if (special == SpecialKind::Set) {
    kind = setIdent_;
  }

  if (isPrivate) {
    prop = setLocation(
        startLoc, prop, new (context_) ESTree::PrivateNameNode(prop));
  }

  if (variance) {
    error(variance->getSourceRange(), "Unexpected variance sigil");
  }

  return setLocation(
      startRange,
      optBody.getValue(),
      new (context_) ESTree::MethodDefinitionNode(
          prop, funcExpr, kind, computed, isStatic));
}

bool JSParserImpl::reparseArrowParameters(
    ESTree::Node *node,
    bool hasNewLine,
    ESTree::NodeList &paramList,
    bool &isAsync) {
  // Empty argument list "()".
  if (node->getParens() == 0 && isa<ESTree::CoverEmptyArgsNode>(node))
    return true;

  // A single identifier without parens.
  if (node->getParens() == 0 && isa<ESTree::IdentifierNode>(node)) {
    paramList.push_back(*node);
    return validateBindingIdentifier(
        Param{},
        node->getSourceRange(),
        cast<ESTree::IdentifierNode>(node)->_name,
        TokenKind::identifier);
  }

  ESTree::NodeList nodeList{};

  if (auto *callNode = dyn_cast<ESTree::CallExpressionNode>(node)) {
    // Async function parameters look like call expressions. For example:
    // async(x,y)
    // It must have no surrounding parens and the name must be 'async'.
    // It must also not already be `async`, because the CallExpression
    // determines whether it is `async`.
    // It must not have a newline between 'async' and the parameters.
    // Set `isAsync = true` to indicate that this was async.
    auto *callee = dyn_cast<ESTree::IdentifierNode>(callNode->_callee);
    if (!isAsync && callNode->getParens() == 0 && callee &&
        callee->_name == asyncIdent_ && !hasNewLine) {
      nodeList = std::move(callNode->_arguments);
      isAsync = true;
    } else {
      error(node->getSourceRange(), "invalid arrow function parameter list");
      return false;
    }
  } else {
    if (node->getParens() != 1) {
      error(node->getSourceRange(), "invalid arrow function parameter list");
      return false;
    }

    if (auto *seqNode = dyn_cast<ESTree::SequenceExpressionNode>(node)) {
      nodeList = std::move(seqNode->_expressions);
    } else {
      node->clearParens();
      nodeList.push_back(*node);
    }
  }

  llvh::SaveAndRestore<bool> oldParamAwait(paramAwait_, paramAwait_ || isAsync);

  // If the node has 0 parentheses, return true, otherwise print an error and
  // return false.
  auto checkParens = [this](ESTree::Node *n) {
    if (n->getParens() == 0)
      return true;

    error(n->getSourceRange(), "parentheses are not allowed around parameters");
    return false;
  };

  for (auto it = nodeList.begin(), e = nodeList.end(); it != e;) {
    auto *expr = &*it;
    it = nodeList.erase(it);

    if (!checkParens(expr))
      continue;

    if (auto *CRE = dyn_cast<ESTree::CoverRestElementNode>(expr)) {
      if (it != e)
        error(expr->getSourceRange(), "rest parameter must be last");
      else
        paramList.push_back(*CRE->_rest);
      continue;
    }

    if (auto *spread = dyn_cast<ESTree::SpreadElementNode>(expr)) {
      // async arrow heads are initially parsed as CallExpression,
      // which means that Rest elements are parsed as SpreadElement.
      if (it != e)
        error(expr->getSourceRange(), "rest parameter must be last");
      else
        paramList.push_back(*new (context_)
                                ESTree::RestElementNode(spread->_argument));
      continue;
    }

    if (isa<ESTree::CoverTrailingCommaNode>(expr)) {
      assert(
          it == e &&
          "CoverTrailingCommaNode should have been only parsed last");
      // Just skip it.
      continue;
    }

    ESTree::AssignmentExpressionNode *asn = nullptr;
    ESTree::Node *init = nullptr;

    // If we encounter an initializer, unpack it.
    if ((asn = dyn_cast<ESTree::AssignmentExpressionNode>(expr))) {
      if (asn->_operator == getTokenIdent(TokenKind::equal)) {
        expr = asn->_left;
        init = asn->_right;

        if (!checkParens(expr))
          continue;
      }
    }

    auto optParam = reparseAssignmentPattern(expr, true);
    if (!optParam)
      continue;
    expr = *optParam;

    if (init) {
      expr = setLocation(
          asn, asn, new (context_) ESTree::AssignmentPatternNode(expr, init));
    }

    if (auto *ident = dyn_cast<ESTree::IdentifierNode>(expr)) {
      validateBindingIdentifier(
          Param{},
          ident->getSourceRange(),
          ident->_name,
          TokenKind::identifier);
    }

    paramList.push_back(*expr);
  }

  return true;
}

Optional<ESTree::Node *> JSParserImpl::parseArrowFunctionExpression(
    Param param,
    ESTree::Node *leftExpr,
    bool hasNewLine,
    ESTree::Node *typeParams,
    ESTree::Node *returnType,
    ESTree::Node *predicate,
    SMLoc startLoc,
    AllowTypedArrowFunction allowTypedArrowFunction,
    bool forceAsync) {
  // ArrowFunction : ArrowParameters [no line terminator] => ConciseBody.
  assert(
      check(TokenKind::equalgreater) && !lexer_.isNewLineBeforeCurrentToken() &&
      "ArrowFunctionExpression expects [no new line] '=>'");

  llvh::SaveAndRestore<bool> argsParamAwait(paramAwait_, forceAsync);

  if (!eat(
          TokenKind::equalgreater,
          JSLexer::GrammarContext::AllowRegExp,
          "in arrow function expression",
          "start of arrow function",
          startLoc))
    return None;

  bool isAsync = forceAsync;
  ESTree::NodeList paramList;
  if (!reparseArrowParameters(leftExpr, hasNewLine, paramList, isAsync))
    return None;

  SaveStrictModeAndSeenDirectives saveStrictModeAndSeenDirectives{this};
  ESTree::Node *body;
  bool expression;

  llvh::SaveAndRestore<bool> oldParamYield(paramYield_, false);
  llvh::SaveAndRestore<bool> bodyParamAwait(paramAwait_, isAsync);
  if (check(TokenKind::l_brace)) {
    auto optBody = parseFunctionBody(
        Param{},
        true,
        oldParamYield.get(),
        argsParamAwait.get(),
        JSLexer::AllowDiv,
        true);
    if (!optBody)
      return None;
    body = *optBody;
    expression = false;
  } else {
    // It's possible to recurse onto parseAssignmentExpression directly
    // and get stuck without a depth check if we don't have one here.
    CHECK_RECURSION;
    auto optConcise = parseAssignmentExpression(
        param.get(ParamIn),
        allowTypedArrowFunction,
        CoverTypedParameters::No,
        nullptr);
    if (!optConcise)
      return None;
    body = *optConcise;
    expression = true;
  }

  auto *arrow = new (context_) ESTree::ArrowFunctionExpressionNode(
      nullptr,
      std::move(paramList),
      body,
      typeParams,
      returnType,
      predicate,
      expression,
      isAsync);

  return setLocation(startLoc, getPrevTokenEndLoc(), arrow);
}

Optional<ESTree::Node *> JSParserImpl::reparseAssignmentPattern(
    ESTree::Node *node,
    bool inDecl) {
  if (!node->getParens()) {
    if (auto *AEN = dyn_cast<ESTree::ArrayExpressionNode>(node)) {
      return reparseArrayAsignmentPattern(AEN, inDecl);
    }
    if (auto *OEN = dyn_cast<ESTree::ObjectExpressionNode>(node)) {
      return reparseObjectAssignmentPattern(OEN, inDecl);
    }
    if (auto *ident = dyn_cast<ESTree::IdentifierNode>(node)) {
      // Validate in this function to avoid validating in each branch of
      // the conditions within the other reparse functions.
      // Validation does not prevent progress of the parse here, so we can
      // return node regardless of whether we failed to validate.
      validateBindingIdentifier(
          Param{},
          ident->getSourceRange(),
          ident->_name,
          TokenKind::identifier);
      return node;
    }
    if (isa<ESTree::PatternNode>(node)) {
      // Pattern nodes validate their binding identifiers when they are
      // initially parsed, no work to do here.
      return node;
    }
#if HERMES_PARSE_FLOW
    if (auto *cover = dyn_cast<ESTree::CoverTypedIdentifierNode>(node)) {
      auto optAssn = reparseAssignmentPattern(cover->_left, inDecl);
      // type may be nullptr, but the patterns may have null typeAnnotation.
      auto *type = cover->_right;
      if (!optAssn)
        return None;
      if (auto *apn = dyn_cast<ESTree::ArrayPatternNode>(*optAssn)) {
        apn->_typeAnnotation = type;
        return setLocation(cover, cover, apn);
      }
      if (auto *opn = dyn_cast<ESTree::ObjectPatternNode>(*optAssn)) {
        opn->_typeAnnotation = type;
        return setLocation(cover, cover, opn);
      }
      if (auto *id = dyn_cast<ESTree::IdentifierNode>(*optAssn)) {
        id->_typeAnnotation = type;
        id->_optional = cover->_optional;
        return setLocation(cover, cover, id);
      }
    }
    if (auto *typecast = dyn_cast<ESTree::TypeCastExpressionNode>(node)) {
      auto optAssn = reparseAssignmentPattern(typecast->_expression, inDecl);
      auto *type = typecast->_typeAnnotation;
      if (!optAssn)
        return None;
      if (auto *apn = dyn_cast<ESTree::ArrayPatternNode>(*optAssn)) {
        apn->_typeAnnotation = type;
        return setLocation(apn, type, apn);
      }
      if (auto *opn = dyn_cast<ESTree::ObjectPatternNode>(*optAssn)) {
        opn->_typeAnnotation = type;
        return setLocation(opn, type, opn);
      }
      if (auto *id = dyn_cast<ESTree::IdentifierNode>(*optAssn)) {
        id->_typeAnnotation = type;
        return setLocation(id, type, id);
      }
    }
#endif
  }

  if (inDecl) {
    error(node->getSourceRange(), "identifier or pattern expected");
    return None;
  }

  return node;
}

Optional<ESTree::Node *> JSParserImpl::reparseArrayAsignmentPattern(
    ESTree::ArrayExpressionNode *AEN,
    bool inDecl) {
  ESTree::NodeList elements{};

  for (auto it = AEN->_elements.begin(), e = AEN->_elements.end(); it != e;) {
    ESTree::Node *elem = &*it++;
    AEN->_elements.remove(*elem);

    // Every element in the array assignment pattern is optional,
    // because we can parse the Elision production.
    if (isa<ESTree::EmptyNode>(elem)) {
      elements.push_back(*elem);
      continue;
    }

    if (auto *spread = dyn_cast<ESTree::SpreadElementNode>(elem)) {
      if (it != e || AEN->_trailingComma) {
        error(spread->getSourceRange(), "rest element must be last");
        continue;
      }

      auto optSubPattern = reparseAssignmentPattern(spread->_argument, inDecl);
      if (!optSubPattern)
        continue;
      elem = setLocation(
          spread,
          spread,
          new (context_) ESTree::RestElementNode(*optSubPattern));
    } else {
      ESTree::AssignmentExpressionNode *asn = nullptr;
      ESTree::Node *init = nullptr;

      // If we encounter an initializer, unpack it.
      if (!elem->getParens()) {
        if ((asn = dyn_cast<ESTree::AssignmentExpressionNode>(elem))) {
          if (asn->_operator == getTokenIdent(TokenKind::equal)) {
            elem = asn->_left;
            init = asn->_right;
          }
        }
      }

      // Reparse {...} or [...]
      auto optSubPattern = reparseAssignmentPattern(elem, inDecl);
      if (!optSubPattern)
        continue;
      elem = *optSubPattern;

      if (init) {
        elem = setLocation(
            asn, asn, new (context_) ESTree::AssignmentPatternNode(elem, init));
      }
    }

    elements.push_back(*elem);
  }

  return setLocation(
      AEN->getStartLoc(),
      AEN->getEndLoc(),
      new (context_) ESTree::ArrayPatternNode(std::move(elements), nullptr));
}

Optional<ESTree::Node *> JSParserImpl::reparseObjectAssignmentPattern(
    ESTree::ObjectExpressionNode *OEN,
    bool inDecl) {
  ESTree::NodeList elements{};

  for (auto it = OEN->_properties.begin(), e = OEN->_properties.end();
       it != e;) {
    auto *node = &*it++;
    OEN->_properties.remove(*node);

    if (auto *spread = dyn_cast<ESTree::SpreadElementNode>(node)) {
      if (it != e) {
        error(spread->getSourceRange(), "rest property must be last");
        continue;
      }

      // NOTE: the spec says that AssignmentRestProperty cannot be another
      // pattern (see
      // https://www.ecma-international.org/ecma-262/9.0/index.html#sec-destructuring-assignment-static-semantics-early-errors)
      // even though it would be logical.
#if 0
      auto optSubPattern = reparseAssignmentPattern(spread->_argument, inDecl);
      if (!optSubPattern)
        continue;
      node = *optSubPattern;
#else
      node = spread->_argument;
      if (inDecl) {
        if (!isa<ESTree::IdentifierNode>(node)) {
          error(
              node->getSourceRange(), "identifier expected in parameter list");
          continue;
        }
      }
#endif
      node = setLocation(
          spread, spread, new (context_) ESTree::RestElementNode(node));
    } else {
      auto *propNode = cast<ESTree::PropertyNode>(node);

      if (propNode->_kind != initIdent_) {
        error(
            SourceErrorManager::combineIntoRange(
                propNode->getStartLoc(), propNode->_key->getStartLoc()),
            "invalid destructuring target");
        continue;
      }

      ESTree::Node *value = propNode->_value;
      ESTree::Node *init = nullptr;
      SMLoc endLoc = value->getEndLoc();

      // If we encounter an initializer, unpack it.
      if (auto *asn = dyn_cast<ESTree::AssignmentExpressionNode>(value)) {
        if (asn->_operator == getTokenIdent(TokenKind::equal)) {
          value = asn->_left;
          init = asn->_right;
        }
      } else if (
          auto *coverInitializer =
              dyn_cast<ESTree::CoverInitializerNode>(value)) {
        assert(
            isa<ESTree::IdentifierNode>(propNode->_key) &&
            "CoverInitializedName must start with an identifier");
        // Clone the key.
        auto *ident = cast<ESTree::IdentifierNode>(propNode->_key);
        value = new (context_) ESTree::IdentifierNode(
            ident->_name, ident->_typeAnnotation, ident->_optional);
        value->copyLocationFrom(propNode->_key);

        init = coverInitializer->_init;
      }

      // Reparse {...} or [...]
      auto optSubPattern = reparseAssignmentPattern(value, inDecl);
      if (!optSubPattern)
        continue;
      value = *optSubPattern;

      // If we have an initializer, create an AssignmentPattern.
      if (init) {
        value = setLocation(
            value,
            endLoc,
            new (context_) ESTree::AssignmentPatternNode(value, init));
      }

      propNode->_value = value;
    }

    elements.push_back(*node);
  }

  auto *OP =
      new (context_) ESTree::ObjectPatternNode(std::move(elements), nullptr);
  OP->copyLocationFrom(OEN);
  return OP;
}

#if HERMES_PARSE_FLOW
Optional<ESTree::Node *> JSParserImpl::tryParseTypedAsyncArrowFunction(
    Param param) {
  assert(context_.getParseFlow());
  assert(check(asyncIdent_));
  JSLexer::SavePoint savePoint{&lexer_};
  SMLoc start = advance().Start;

  ESTree::Node *leftExpr = nullptr;
  bool hasNewLine = false;
  ESTree::Node *typeParams = nullptr;
  ESTree::Node *returnType = nullptr;
  ESTree::Node *predicate = nullptr;
  {
    SourceErrorManager::SaveAndSuppressMessages suppress{
        &sm_, Subsystem::Parser};
    if (check(TokenKind::less)) {
      auto optTypeParams = parseTypeParamsFlow();
      if (!optTypeParams) {
        savePoint.restore();
        return None;
      }
      typeParams = *optTypeParams;
    }

    if (!check(TokenKind::l_paren)) {
      savePoint.restore();
      return None;
    }

    hasNewLine = lexer_.isNewLineBeforeCurrentToken();
    auto optLeftExpr =
        parseConditionalExpression(param, CoverTypedParameters::Yes);
    if (!optLeftExpr) {
      savePoint.restore();
      return None;
    }
    leftExpr = *optLeftExpr;

    if (check(TokenKind::colon)) {
      SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
      if (!check(checksIdent_)) {
        auto optType = parseReturnTypeAnnotationFlow(
            annotStart, AllowAnonFunctionType::No);
        if (!optType) {
          savePoint.restore();
          return None;
        }
        returnType = *optType;
      }
      if (check(checksIdent_)) {
        auto optPredicate = parsePredicateFlow();
        if (!optPredicate) {
          savePoint.restore();
          return None;
        }
        predicate = *optPredicate;
      }
    }

    if (!check(TokenKind::equalgreater)) {
      savePoint.restore();
      return None;
    }
  }

  return parseArrowFunctionExpression(
      param,
      leftExpr,
      hasNewLine,
      typeParams,
      returnType,
      predicate,
      start,
      AllowTypedArrowFunction::Yes,
      /* forceAsync */ true);
}
#endif

Optional<ESTree::Node *> JSParserImpl::parseAssignmentExpression(
    Param param,
    AllowTypedArrowFunction allowTypedArrowFunction,
    CoverTypedParameters coverTypedParameters,
    ESTree::Node *typeParams) {
  struct State {
    SMLoc leftStartLoc = {};
    Optional<ESTree::Node *> optLeftExpr = llvh::None;
    bool hasNewLine = false;
    UniqueString *op = nullptr;
    SMLoc debugLoc = {};

    explicit State() {}
  };

  auto parseHelper = [this](
                         State &state,
                         Param param,
                         AllowTypedArrowFunction allowTypedArrowFunction,
                         CoverTypedParameters coverTypedParameters,
                         ESTree::Node *typeParams) -> Optional<ESTree::Node *> {
    // Check for yield, which may be lexed as a reserved word, but only in
    // strict mode.
    if (paramYield_ && check(TokenKind::rw_yield, TokenKind::identifier) &&
        tok_->getResWordOrIdentifier() == yieldIdent_) {
      auto optYieldExpr = parseYieldExpression(param.get(ParamIn));
      if (!optYieldExpr)
        return None;
      ESTree::YieldExpressionNode *yieldExpr = *optYieldExpr;
      if (yieldExpr->_argument && !checkEndAssignmentExpression()) {
        error(tok_->getStartLoc(), "unexpected token after yield expression");
        return None;
      }
      return yieldExpr;
    }

    SMLoc startLoc = tok_->getStartLoc();
    bool forceAsync = false;
    if (check(asyncIdent_)) {
      OptValue<TokenKind> optNext = lexer_.lookahead1(TokenKind::identifier);
      if (optNext.hasValue() && *optNext == TokenKind::identifier) {
        forceAsync = true;
      }
#if HERMES_PARSE_FLOW
      if (context_.getParseFlow() && optNext.hasValue() &&
          (*optNext == TokenKind::less || *optNext == TokenKind::l_paren)) {
        auto optAsyncArrow = tryParseTypedAsyncArrowFunction(param);
        if (optAsyncArrow.hasValue()) {
          return *optAsyncArrow;
        }
      }
#endif
    }

#if HERMES_PARSE_FLOW
    if (context_.getParseFlow() &&
        allowTypedArrowFunction == AllowTypedArrowFunction::Yes &&
        !typeParams && check(TokenKind::less)) {
      JSLexer::SavePoint savePoint{&lexer_};
      // Suppress messages from the parser while still displaying lexer
      // messages.
      CollectMessagesRAII collect{&sm_, true};
      // Do as the flow parser does due to JSX ambiguities.
      // First we try and parse as an assignment expression disallowing
      // typed arrow functions. If that fails, then try again while allowing
      // typed arrow functions and attach the type parameters after the fact.
      auto optAssign = parseAssignmentExpression(
          param,
          AllowTypedArrowFunction::No,
          CoverTypedParameters::No,
          nullptr);
      if (optAssign) {
        // That worked, so just return it directly.
        collect.setDiscardMessages(false);
        return *optAssign;
      } else {
        // Consume the type parameters and try again.
        savePoint.restore();
        auto optTypeParams = parseTypeParamsFlow();
        // Type parameters must be followed by a '(' to be meaningful.
        if (optTypeParams && check(TokenKind::l_paren)) {
          typeParams = *optTypeParams;
          optAssign = parseAssignmentExpression(
              param,
              AllowTypedArrowFunction::Yes,
              CoverTypedParameters::No,
              typeParams);
          if (optAssign) {
            // We've got the arrow function now, return it directly.
            return *optAssign;
          } else {
            // That's everything we can try.
            error(
                typeParams->getSourceRange(),
                "type parameters must be used in an arrow function expression");
            return None;
          }
        } else {
          // Invalid type params, and also invalid JSX. Bail.
          savePoint.restore();
        }
      }
    }
#endif

    state.leftStartLoc = tok_->getStartLoc();
    state.hasNewLine = lexer_.isNewLineBeforeCurrentToken();
    state.optLeftExpr = parseConditionalExpression(param, coverTypedParameters);
    if (!state.optLeftExpr)
      return None;

    ESTree::Node *returnType = nullptr;
    ESTree::Node *predicate = nullptr;
#if HERMES_PARSE_FLOW
    if (context_.getParseFlow()) {
      if (allowTypedArrowFunction == AllowTypedArrowFunction::Yes &&
          ((*state.optLeftExpr)->getParens() != 0 ||
           isa<ESTree::CoverEmptyArgsNode>(*state.optLeftExpr)) &&
          check(TokenKind::colon)) {
        JSLexer::SavePoint savePoint{&lexer_};
        // Defer our decision on whether to show or suppress messages for this
        // next section.
        // If we are unsuccessful during the parse, it can mean that we need to
        // start parsing JSX children inside tags, instead of function type
        // parameters. We need to suppress lexer messages because the lexing
        // rules inside JSX are quite different from JS/Flow. For example: x ?
        // (1) : <tag>#{foo}</tag>;
        //         ^
        // and
        // x ? (1) : <tag>"</tag>;
        //         ^
        // must be able to handle the lexer errors that would occur if we lexed
        // the inside of the JSX tags as JS.
        CollectMessagesRAII collect{&sm_, true};
        SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
        bool startsWithPredicate = check(checksIdent_);
        auto optType = startsWithPredicate
            ? llvh::None
            : parseReturnTypeAnnotationFlow(
                  annotStart, AllowAnonFunctionType::No);
        if (optType)
          returnType = *optType;
        if (optType || startsWithPredicate) {
          if (check(TokenKind::equalgreater)) {
            assert(
                !startsWithPredicate && "no returnType if startsWithPredicate");
            // Done parsing the return type and predicate.
            // Successful parse, show any messages that the lexer emitted.
            collect.setDiscardMessages(false);
          } else if (check(checksIdent_)) {
            auto optPred = parsePredicateFlow();
            if (optPred && check(TokenKind::equalgreater)) {
              // Done parsing the return type and predicate.
              predicate = *optPred;
              // Successful parse, show any messages that the lexer emitted.
              collect.setDiscardMessages(false);
            } else {
              savePoint.restore();
            }
          } else {
            savePoint.restore();
          }
        } else {
          savePoint.restore();
        }
      }
    }
#endif

#if HERMES_PARSE_TS
    if (context_.getParseTS()) {
      // Separate logic for TS parsing here, because the semantics don't
      // require as much complexity as Flow due to a lack of predicates.
      if (allowTypedArrowFunction == AllowTypedArrowFunction::Yes &&
          ((*state.optLeftExpr)->getParens() != 0 ||
           isa<ESTree::CoverEmptyArgsNode>(*state.optLeftExpr)) &&
          check(TokenKind::colon)) {
        JSLexer::SavePoint savePoint{&lexer_};
        // Defer our decision on whether to show or suppress messages for this
        // next section.
        // If we are unsuccessful during the parse, it can mean that we need to
        // start parsing JSX children inside tags, instead of function type
        // parameters. We need to suppress lexer messages because the lexing
        // rules inside JSX are quite different from TS. For example: x ? (1) :
        // <tag>#{foo}</tag>;
        //         ^
        // and
        // x ? (1) : <tag>"</tag>;
        //         ^
        // must be able to handle the lexer errors that would occur if we lexed
        // the inside of the JSX tags as JS.
        CollectMessagesRAII collect{&sm_, true};
        SMLoc annotStart = advance(JSLexer::GrammarContext::Type).Start;
        auto optType = parseTypeAnnotationTS(annotStart);
        if (optType)
          returnType = *optType;
        if (optType) {
          if (check(TokenKind::equalgreater)) {
            // Done parsing the return type.
            // Successful parse, show any messages that the lexer emitted.
            collect.setDiscardMessages(false);
          } else {
            savePoint.restore();
          }
        } else {
          savePoint.restore();
        }
      }
    }

#endif

    // Check for ArrowFunction.
    //   ArrowFunction : ArrowParameters [no line terminator] => ConciseBody.
    //   AsyncArrowFunction :
    //   async [no line terminator] ArrowParameters [no line terminator] =>
    //   ConciseBody.
    if (check(TokenKind::equalgreater) &&
        !lexer_.isNewLineBeforeCurrentToken()) {
      return parseArrowFunctionExpression(
          param,
          *state.optLeftExpr,
          state.hasNewLine,
          typeParams,
          returnType,
          predicate,
          typeParams ? typeParams->getStartLoc() : startLoc,
          allowTypedArrowFunction,
          forceAsync);
    }

#if HERMES_PARSE_FLOW
    if (typeParams) {
      errorExpected(
          TokenKind::equalgreater,
          "in generic arrow function",
          "start of function",
          typeParams->getStartLoc());
      return None;
    }
#endif

    if (!checkAssign())
      return *state.optLeftExpr;

    // Check for destructuring assignment.
    if (check(TokenKind::equal) &&
        (isa<ESTree::ArrayExpressionNode>(*state.optLeftExpr) ||
         isa<ESTree::ObjectExpressionNode>(*state.optLeftExpr))) {
      state.optLeftExpr = reparseAssignmentPattern(*state.optLeftExpr, false);
      if (!state.optLeftExpr)
        return None;
    }

    state.op = getTokenIdent(tok_->getKind());
    state.debugLoc = advance().Start;
    return nullptr;
  };

  llvh::SmallVector<State, 2> stack;

  stack.emplace_back();
  auto optRes = parseHelper(
      stack.back(),
      param,
      allowTypedArrowFunction,
      coverTypedParameters,
      typeParams);

  for (;;) {
    if (!optRes)
      return None;
    if (!stack.back().op) {
      stack.pop_back();
      break;
    }
    if (stack.size() > ESTree::MAX_NESTED_ASSIGNMENTS) {
      recursionDepthExceeded();
      return None;
    }
    stack.emplace_back();
    optRes = parseHelper(
        stack.back(),
        param,
        AllowTypedArrowFunction::Yes,
        CoverTypedParameters::No,
        nullptr);
  }

  assert(optRes.getValue() != nullptr);

  while (!stack.empty()) {
    if (!checkEndAssignmentExpression()) {
      // Note: We don't assert the valid end of an AssignmentExpression here
      // because we do not know yet whether the entire file is well-formed.
      // This check errors here to ensure that we still catch missing elements
      // in `checkEndAssignmentExpression` while allowing us to avoid actually
      // asserting and crashing.
      error(
          tok_->getStartLoc(), "unexpected token after assignment expression");
      return None;
    }
    auto &top = stack.back();
    optRes = setLocation(
        top.leftStartLoc,
        getPrevTokenEndLoc(),
        top.debugLoc,
        new (context_) ESTree::AssignmentExpressionNode(
            top.op, top.optLeftExpr.getValue(), optRes.getValue()));
    stack.pop_back();
  }

  return optRes.getValue();
}

Optional<ESTree::Node *> JSParserImpl::parseExpression(
    Param param,
    CoverTypedParameters coverTypedParameters) {
  SMLoc startLoc = tok_->getStartLoc();
  auto optExpr = parseAssignmentExpression(
      param, AllowTypedArrowFunction::Yes, coverTypedParameters, nullptr);
  if (!optExpr)
    return None;

  if (!check(TokenKind::comma))
    return optExpr.getValue();

  ESTree::NodeList exprList;
  exprList.push_back(*optExpr.getValue());

  while (check(TokenKind::comma)) {
    // Eat the ",".
    auto commaRng = advance();

    // CoverParenthesizedExpressionAndArrowParameterList: (Expression ,)
    if (check(TokenKind::r_paren)) {
      auto *coverNode = setLocation(
          commaRng,
          tok_->getStartLoc(),
          new (context_) ESTree::CoverTrailingCommaNode());
      exprList.push_back(*coverNode);
      break;
    }

    ESTree::Node *expr2;

    if (check(TokenKind::dotdotdot)) {
      auto optRest = parseBindingRestElement(param);
      if (!optRest)
        return None;
      expr2 = setLocation(
          *optRest,
          *optRest,
          new (context_) ESTree::CoverRestElementNode(*optRest));
    } else {
      auto optExpr2 = parseAssignmentExpression(param);
      if (!optExpr2)
        return None;
      expr2 = *optExpr2;
    }

    exprList.push_back(*expr2);
  }

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::SequenceExpressionNode(std::move(exprList)));
}

Optional<ESTree::StringLiteralNode *> JSParserImpl::parseFromClause() {
  SMLoc startLoc = tok_->getStartLoc();

  if (!checkAndEat(fromIdent_)) {
    error(startLoc, "'from' expected");
    return None;
  }

  if (!need(
          TokenKind::string_literal,
          "after 'from'",
          "location of 'from'",
          startLoc)) {
    return None;
  }

  auto *source = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));

  advance();
  return source;
}

bool JSParserImpl::parseAssertClause(ESTree::NodeList &attributes) {
  assert(check(assertIdent_));
  SMLoc start = advance().Start;

  // assert { }
  // assert { AssertEntries ,[opt] }
  //        ^

  if (!eat(
          TokenKind::l_brace,
          JSLexer::GrammarContext::AllowRegExp,
          "in import assertion",
          "start of assertion",
          start))
    return false;

  while (!check(TokenKind::r_brace)) {
    // AssertionKey : StringLiteral
    // ^
    ESTree::Node *key = nullptr;
    if (check(TokenKind::string_literal)) {
      key = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
      advance();
    } else {
      if (!need(
              TokenKind::identifier,
              "in import assertion",
              "start of assertion",
              start))
        return false;

      key = setLocation(
          tok_,
          tok_,
          new (context_)
              ESTree::IdentifierNode(tok_->getIdentifier(), nullptr, false));
      advance();
    }

    if (!eat(
            TokenKind::colon,
            JSLexer::GrammarContext::AllowRegExp,
            "in import assertion",
            "start of assertion",
            start))
      return false;

    // AssertionKey : StringLiteral
    //                ^

    if (!need(
            TokenKind::string_literal,
            "in import assertion",
            "start of assertion",
            start))
      return false;

    ESTree::Node *value = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
    advance();
    attributes.push_back(*setLocation(
        key, value, new (context_) ESTree::ImportAttributeNode(key, value)));

    if (!checkAndEat(TokenKind::comma))
      break;
  }

  // assert { AssertEntries ,[opt] }
  //                               ^

  if (!eat(
          TokenKind::r_brace,
          JSLexer::GrammarContext::AllowRegExp,
          "in import assertion",
          "start of assertion",
          start))
    return false;

  return true;
}

Optional<ESTree::ImportDeclarationNode *>
JSParserImpl::parseImportDeclaration() {
  assert(
      check(TokenKind::rw_import) &&
      "import declaration must start with 'import'");
  SMLoc startLoc = advance().Start;

  if (check(TokenKind::string_literal)) {
    // import ModuleSpecifier ;
    // If the first token is a string literal, there are no specifiers,
    // so the import clause should not be parsed.
    auto *source = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
    advance();

    ESTree::NodeList attributes{};
    if (check(assertIdent_) && !lexer_.isNewLineBeforeCurrentToken()) {
      if (!parseAssertClause(attributes))
        return None;
    }

    if (!eatSemi()) {
      return None;
    }

    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_) ESTree::ImportDeclarationNode(
            {}, source, std::move(attributes), valueIdent_));
  }

  ESTree::NodeList specifiers;
  auto optKind = parseImportClause(specifiers);
  if (!optKind)
    return None;
  UniqueString *kind = *optKind;

  auto optFromClause = parseFromClause();
  if (!optFromClause) {
    return None;
  }

  ESTree::NodeList attributes{};
  if (check(assertIdent_) && !lexer_.isNewLineBeforeCurrentToken()) {
    if (!parseAssertClause(attributes))
      return None;
  }

  if (!eatSemi()) {
    return None;
  }

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ImportDeclarationNode(
          std::move(specifiers), *optFromClause, std::move(attributes), kind));
}

Optional<UniqueString *> JSParserImpl::parseImportClause(
    ESTree::NodeList &specifiers) {
  SMLoc startLoc = tok_->getStartLoc();

  UniqueString *kind = valueIdent_;
  SMRange kindRange{};
#if HERMES_PARSE_FLOW
  if (context_.getParseFlow()) {
    if (checkN(typeIdent_, TokenKind::rw_typeof)) {
      kind = tok_->getResWordOrIdentifier();
      kindRange = advance();
    }
  }
#endif
#if HERMES_PARSE_TS
  if (context_.getParseTS()) {
    if (checkN(typeIdent_)) {
      kind = tok_->getResWordOrIdentifier();
      kindRange = advance();
    }
  }
#endif

  if (check(TokenKind::identifier)) {
    if (check(fromIdent_) && kind == typeIdent_) {
      // Not actually a type import, just import default with the name 'type'.
      kind = valueIdent_;
      auto *defaultBinding = setLocation(
          kindRange,
          kindRange,
          new (context_) ESTree::IdentifierNode(typeIdent_, nullptr, false));
      specifiers.push_back(*setLocation(
          defaultBinding,
          defaultBinding,
          new (context_) ESTree::ImportDefaultSpecifierNode(defaultBinding)));
    } else {
      // ImportedDefaultBinding
      // ImportedDefaultBinding , NameSpaceImport
      // ImportedDefaultBinding , NamedImports
      auto optDefaultBinding = parseBindingIdentifier(Param{});
      if (!optDefaultBinding) {
        errorExpected(
            TokenKind::identifier,
            "in import clause",
            "start of import clause",
            startLoc);
        return None;
      }
      ESTree::IdentifierNode *defaultBinding = *optDefaultBinding;
      specifiers.push_back(*setLocation(
          defaultBinding,
          defaultBinding,
          new (context_) ESTree::ImportDefaultSpecifierNode(defaultBinding)));
    }
    if (!checkAndEat(TokenKind::comma)) {
      // If there was no comma, there's no more bindings to parse,
      // so return immediately.
      return kind;
    }
  }

  // At this point, either:
  // - the ImportedDefaultBinding was parsed and had a comma after it
  // - there was no ImportedDefaultBinding and we simply continue

  if (check(TokenKind::star)) {
    // NameSpaceImport
    auto optNsImport = parseNameSpaceImport();
    if (!optNsImport) {
      return None;
    }
    specifiers.push_back(*optNsImport.getValue());
    return kind;
  }

  // NamedImports is the only remaining possibility.
  if (!need(
          TokenKind::l_brace,
          "in import specifier clause",
          "location of import specifiers",
          startLoc)) {
    return kind;
  }

  if (!parseNamedImports(specifiers))
    return None;
  return kind;
}

Optional<ESTree::Node *> JSParserImpl::parseNameSpaceImport() {
  assert(check(TokenKind::star) && "import namespace must start with *");

  SMLoc startLoc = advance().Start;
  if (!checkAndEat(asIdent_)) {
    error(tok_->getStartLoc(), "'as' expected");
    return None;
  }

  auto optLocal = parseBindingIdentifier(Param{});
  if (!optLocal) {
    errorExpected(
        TokenKind::identifier,
        "in namespace import",
        "location of namespace import",
        startLoc);
    return None;
  }

  return setLocation(
      startLoc,
      *optLocal,
      new (context_) ESTree::ImportNamespaceSpecifierNode(*optLocal));
}

bool JSParserImpl::parseNamedImports(ESTree::NodeList &specifiers) {
  assert(check(TokenKind::l_brace) && "named imports must start with {");
  SMLoc startLoc = advance().Start;

  // BoundNames to check for duplicate entries in ImportDeclaration.
  // Values are the actual IdentifierNodes, used for error reporting.
  llvh::DenseMap<UniqueString *, ESTree::IdentifierNode *> boundNames{};

  while (!check(TokenKind::r_brace)) {
    auto optSpecifier = parseImportSpecifier(startLoc);
    if (!optSpecifier) {
      return false;
    }

    // Check if the bound name was duplicated.
    ESTree::IdentifierNode *localIdent =
        cast<ESTree::IdentifierNode>(optSpecifier.getValue()->_local);
    auto insertRes = boundNames.try_emplace(localIdent->_name, localIdent);
    if (insertRes.second) {
      specifiers.push_back(*optSpecifier.getValue());
    } else {
      // Report the error but continue parsing to see if there's any others.
      error(
          localIdent->getSourceRange(),
          "Duplicate entry in import declaration list");
      sm_.note(
          insertRes.first->second->getSourceRange(), "first usage of name");
    }

    if (!checkAndEat(TokenKind::comma)) {
      break;
    }
  }
  if (!eat(
          TokenKind::r_brace,
          JSLexer::AllowDiv,
          "at end of named imports",
          "location of '{'",
          startLoc)) {
    return false;
  }

  return true;
}

Optional<ESTree::ImportSpecifierNode *> JSParserImpl::parseImportSpecifier(
    SMLoc importLoc) {
  // ImportSpecifier:
  //   ImportedBinding
  //   IdentifierName as ImportedBinding
  SMLoc startLoc = tok_->getStartLoc();

  UniqueString *kind = valueIdent_;
  ESTree::IdentifierNode *imported = nullptr;
  ESTree::IdentifierNode *local = nullptr;
  TokenKind localKind;

#if HERMES_PARSE_FLOW
  if (context_.getParseFlow() && checkAndEat(TokenKind::rw_typeof)) {
    kind = typeofIdent_;
  }
#endif

  // This isn't wrapped in #if HERMES_PARSE_FLOW, as it is entangled
  // in the rest of the import specifier parsing code and doesn't actually
  // depend on JSParserImpl-flow specific code at all.
  if (HERMES_PARSE_FLOW && context_.getParseFlow() && check(typeIdent_) &&
      kind == valueIdent_) {
    // Consume 'type', but make no assumptions about what it means yet.
    SMRange typeRange = advance();
    if (check(TokenKind::r_brace, TokenKind::comma)) {
      // 'type'
      imported = setLocation(
          typeRange,
          typeRange,
          new (context_) ESTree::IdentifierNode(typeIdent_, nullptr, false));
      local = imported;
      localKind = TokenKind::identifier;
    } else if (check(asIdent_)) {
      SMRange asRange = advance();
      if (check(TokenKind::r_brace, TokenKind::comma)) {
        // 'type' 'as'
        kind = typeIdent_;
        imported = setLocation(
            asRange,
            asRange,
            new (context_) ESTree::IdentifierNode(asIdent_, nullptr, false));
        local = imported;
        localKind = TokenKind::identifier;
        advance();
      } else if (checkAndEat(asIdent_)) {
        // 'type' 'as' 'as' Identifier
        //                  ^
        if (!check(TokenKind::identifier) && !tok_->isResWord()) {
          errorExpected(
              TokenKind::identifier,
              "in import specifier",
              "specifiers start",
              importLoc);
          return None;
        }
        kind = typeIdent_;
        imported = setLocation(
            asRange,
            asRange,
            new (context_) ESTree::IdentifierNode(asIdent_, nullptr, false));
        local = setLocation(
            tok_,
            tok_,
            new (context_) ESTree::IdentifierNode(
                tok_->getResWordOrIdentifier(), nullptr, false));
        localKind = TokenKind::identifier;
        advance();
      } else {
        // 'type' 'as' Identifier
        //             ^
        if (!check(TokenKind::identifier) && !tok_->isResWord()) {
          errorExpected(
              TokenKind::identifier,
              "in import specifier",
              "specifiers start",
              importLoc);
          return None;
        }
        imported = setLocation(
            typeRange,
            typeRange,
            new (context_) ESTree::IdentifierNode(typeIdent_, nullptr, false));
        local = setLocation(
            tok_,
            tok_,
            new (context_) ESTree::IdentifierNode(
                tok_->getResWordOrIdentifier(), nullptr, false));
        localKind = TokenKind::identifier;
        advance();
      }
    } else {
      // 'type' Identifier
      //        ^
      kind = typeIdent_;
      if (!check(TokenKind::identifier) && !tok_->isResWord()) {
        errorExpected(
            TokenKind::identifier,
            "in import specifier",
            "specifiers start",
            importLoc);
        return None;
      }
      imported = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::IdentifierNode(
              tok_->getResWordOrIdentifier(), nullptr, false));
      local = imported;
      localKind = tok_->getKind();
      advance();
      if (checkAndEat(asIdent_)) {
        // type Identifier 'as' Identifier
        //                      ^
        if (!check(TokenKind::identifier) && !tok_->isResWord()) {
          errorExpected(
              TokenKind::identifier,
              "in import specifier",
              "specifiers start",
              importLoc);
          return None;
        }
        local = setLocation(
            tok_,
            tok_,
            new (context_) ESTree::IdentifierNode(
                tok_->getResWordOrIdentifier(), nullptr, false));
        localKind = tok_->getKind();
        advance();
      }
    }
  } else {
    // Not attempting to parse a type identifier.
    if (!check(TokenKind::identifier) && !tok_->isResWord()) {
      errorExpected(
          TokenKind::identifier,
          "in import specifier",
          "specifiers start",
          importLoc);
      return None;
    }
    imported = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(
            tok_->getResWordOrIdentifier(), nullptr, false));
    local = imported;
    localKind = tok_->getKind();
    advance();

    if (checkAndEat(asIdent_)) {
      if (!check(TokenKind::identifier) && !tok_->isResWord()) {
        errorExpected(
            TokenKind::identifier,
            "in import specifier",
            "specifiers start",
            importLoc);
        return None;
      }
      local = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::IdentifierNode(
              tok_->getResWordOrIdentifier(), nullptr, false));
      localKind = tok_->getKind();
      advance();
    }
  }

  // Only the local name must be parsed as a binding identifier.
  // We need to check for 'as' before knowing what the local name is.
  // Thus, we need to validate the binding identifier for the local name
  // after the fact.
  if (!validateBindingIdentifier(
          Param{}, local->getSourceRange(), local->_name, localKind)) {
    error(local->getSourceRange(), "Invalid local name for import");
  }

  return setLocation(
      startLoc,
      getPrevTokenEndLoc(),
      new (context_) ESTree::ImportSpecifierNode(imported, local, kind));
}

Optional<ESTree::Node *> JSParserImpl::parseExportDeclaration() {
  assert(
      check(TokenKind::rw_export) &&
      "parseExportDeclaration requires 'export'");
  SMLoc startLoc = advance().Start;

#if HERMES_PARSE_FLOW
  if (context_.getParseFlow() && check(typeIdent_)) {
    return parseExportTypeDeclarationFlow(startLoc);
  }
#endif

  if (checkAndEat(TokenKind::star)) {
    // export * FromClause;
    // export * as IdentifierName FromClause;
    ESTree::Node *exportAs = nullptr;
    if (checkAndEat(asIdent_)) {
      // export * as IdentifierName FromClause;
      //             ^
      if (!check(TokenKind::identifier) && !tok_->isResWord()) {
        errorExpected(
            TokenKind::identifier,
            "in export clause",
            "start of export",
            startLoc);
        return None;
      }
      exportAs = setLocation(
          tok_,
          tok_,
          new (context_) ESTree::IdentifierNode(
              tok_->getResWordOrIdentifier(), nullptr, false));
      advance();
    }
    auto optFromClause = parseFromClause();
    if (!optFromClause) {
      return None;
    }
    if (!eatSemi()) {
      return None;
    }
    if (exportAs) {
      ESTree::NodeList specifiers{};
      specifiers.push_back(*setLocation(
          startLoc,
          getPrevTokenEndLoc(),
          new (context_) ESTree::ExportNamespaceSpecifierNode(exportAs)));
      return setLocation(
          startLoc,
          getPrevTokenEndLoc(),
          new (context_) ESTree::ExportNamedDeclarationNode(
              nullptr, std::move(specifiers), *optFromClause, valueIdent_));
    }
    return setLocation(
        startLoc,
        getPrevTokenEndLoc(),
        new (context_)
            ESTree::ExportAllDeclarationNode(*optFromClause, valueIdent_));
  } else if (checkAndEat(TokenKind::rw_default)) {
    CHECK_RECURSION;
    // export default
    if (check(TokenKind::rw_function) ||
        (check(asyncIdent_) && checkAsyncFunction())) {
      // export default HoistableDeclaration
      // Currently, the only hoistable declarations are functions.
      auto optFunDecl = parseFunctionDeclaration(ParamDefault);
      if (!optFunDecl) {
        return None;
      }
      return setLocation(
          startLoc,
          *optFunDecl,
          new (context_) ESTree::ExportDefaultDeclarationNode(*optFunDecl));
    } else if (check(TokenKind::rw_class)) {
      auto optClassDecl = parseClassDeclaration(ParamDefault);
      if (!optClassDecl) {
        return None;
      }
      return setLocation(
          startLoc,
          *optClassDecl,
          new (context_) ESTree::ExportDefaultDeclarationNode(*optClassDecl));
#if HERMES_PARSE_FLOW
    } else if (
        context_.getParseFlow() && context_.getParseFlowComponentSyntax() &&
        check(asyncIdent_) && checkAsyncComponentFlow()) {
      SMLoc compStart = advance().Start;
      auto optComponent = parseComponentDeclarationFlow(
          compStart, /* declare */ false, /* isAsync */ true);
      if (!optComponent) {
        return None;
      }
      return setLocation(
          startLoc,
          *optComponent,
          new (context_) ESTree::ExportDefaultDeclarationNode(*optComponent));
    } else if (
        context_.getParseFlow() && context_.getParseFlowComponentSyntax() &&
        checkComponentDeclarationFlow()) {
      auto optComponent = parseComponentDeclarationFlow(
          tok_->getStartLoc(), /* declare */ false);
      if (!optComponent) {
        return None;
      }
      return setLocation(
          startLoc,
          *optComponent,
          new (context_) ESTree::ExportDefaultDeclarationNode(*optComponent));
    } else if (
        context_.getParseFlow() && context_.getParseFlowComponentSyntax() &&
        check(asyncIdent_) && checkAsyncHookFlow()) {
      SMLoc hookStart = advance().Start;
      auto optHook = parseHookDeclarationFlow(hookStart, /* isAsync */ true);
      if (!optHook) {
        return None;
      }
      return setLocation(
          startLoc,
          *optHook,
          new (context_) ESTree::ExportDefaultDeclarationNode(*optHook));
    } else if (
        context_.getParseFlow() && context_.getParseFlowComponentSyntax() &&
        checkHookDeclarationFlow()) {
      auto optHook = parseHookDeclarationFlow(tok_->getStartLoc());
      if (!optHook) {
        return None;
      }
      return setLocation(
          startLoc,
          *optHook,
          new (context_) ESTree::ExportDefaultDeclarationNode(*optHook));
    } else if (context_.getParseFlow() && check(TokenKind::rw_enum)) {
      auto optEnum =
          parseEnumDeclarationFlow(tok_->getStartLoc(), /* declare */ false);
      if (!optEnum) {
        return None;
      }
      return setLocation(
          startLoc,
          *optEnum,
          new (context_) ESTree::ExportDefaultDeclarationNode(*optEnum));
#endif
    } else {
      // export default AssignmentExpression ;
      auto optExpr = parseAssignmentExpression(ParamIn);
      if (!optExpr) {
        return None;
      }
      if (!eatSemi()) {
        return None;
      }
      return setLocation(
          startLoc,
          getPrevTokenEndLoc(),
          new (context_) ESTree::ExportDefaultDeclarationNode(*optExpr));
    }
  } else if (check(TokenKind::l_brace)) {
    // export ExportClause FromClause ;
    // export ExportClause ;
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
      // When there is no FromClause, any ranges added to invalids are actually
      // invalid, and should be reported as errors.
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
            nullptr, std::move(specifiers), source, valueIdent_));
  } else if (check(TokenKind::rw_var)) {
    // Could find another AssignmentExpression without hitting
    // PrimaryExpression.
    CHECK_RECURSION;
    // export VariableStatement
    auto optVar = parseVariableStatement(Param{});
    if (!optVar) {
      return None;
    }
    return setLocation(
        startLoc,
        *optVar,
        new (context_) ESTree::ExportNamedDeclarationNode(
            *optVar, {}, nullptr, valueIdent_));
  }

  // export Declaration [~Yield]

  if (!checkDeclaration()) {
    error(tok_->getSourceRange(), "expected declaration in export");
    return None;
  }

  auto optDecl = parseDeclaration(Param{});
  if (!optDecl) {
    return None;
  }
  ESTree::Node *decl = *optDecl;

  UniqueString *kind = valueIdent_;
#if HERMES_PARSE_FLOW
  if (isa<ESTree::TypeAliasNode>(decl) || isa<ESTree::OpaqueTypeNode>(decl) ||
      isa<ESTree::DeclareTypeAliasNode>(decl) ||
      isa<ESTree::InterfaceDeclarationNode>(decl)) {
    kind = typeIdent_;
  }
#endif

  return setLocation(
      startLoc,
      decl,
      new (context_)
          ESTree::ExportNamedDeclarationNode(decl, {}, nullptr, kind));
}

bool JSParserImpl::parseExportClause(
    ESTree::NodeList &specifiers,
    llvh::SmallVectorImpl<SMRange> &invalids) {
  // ExportClause:
  //   { }
  //   { ExportsList }
  //   { ExportsList , '}

  assert(check(TokenKind::l_brace) && "ExportClause requires '{'");
  SMLoc startLoc = advance().Start;

  while (!check(TokenKind::r_brace)) {
    // Read all the elements of the ExportsList.
    auto optSpecifier = parseExportSpecifier(startLoc, invalids);
    if (!optSpecifier) {
      return false;
    }
    specifiers.push_back(*optSpecifier.getValue());

    if (!checkAndEat(TokenKind::comma)) {
      break;
    }
  }

  return eat(
      TokenKind::r_brace,
      JSLexer::AllowDiv,
      "at end of export clause",
      "location of export",
      startLoc);
}

Optional<ESTree::Node *> JSParserImpl::parseExportSpecifier(
    SMLoc exportLoc,
    llvh::SmallVectorImpl<SMRange> &invalids) {
  // ExportSpecifier:
  //   IdentifierName
  //   IdentifierName as IdentifierName

  if (!check(TokenKind::identifier) && !tok_->isResWord()) {
    errorExpected(
        TokenKind::identifier,
        "in export clause",
        "location of export clause",
        exportLoc);
    return None;
  }

  // ES9.0 15.2.3.1 Early errors for ReferencedBindings in ExportClause.
  // Add potentially error-raising identifier ranges to the invalids list here,
  // and the owner of the invalids list will report the ranges as errors if
  // necessary.
  if (tok_->isResWord() || check(implementsIdent_) || check(interfaceIdent_) ||
      check(letIdent_) || check(packageIdent_) || check(privateIdent_) ||
      check(protectedIdent_) || check(publicIdent_) || check(staticIdent_)) {
    invalids.push_back(tok_->getSourceRange());
  }

  auto *local = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::IdentifierNode(
          tok_->getResWordOrIdentifier(), nullptr, false));
  advance();
  ESTree::Node *exported;
  if (checkAndEat(asIdent_)) {
    // IdentifierName as IdentifierName
    if (!check(TokenKind::identifier) && !tok_->isResWord()) {
      errorExpected(
          TokenKind::identifier,
          "in export clause",
          "location of export clause",
          exportLoc);
      return None;
    }
    exported = setLocation(
        tok_,
        tok_,
        new (context_) ESTree::IdentifierNode(
            tok_->getResWordOrIdentifier(), nullptr, false));
    advance();
  } else {
    // IdentifierName
    exported = local;
  }

  return setLocation(
      local,
      exported,
      new (context_) ESTree::ExportSpecifierNode(exported, local));
}

ESTree::ExpressionStatementNode *JSParserImpl::parseDirective() {
  // Is the current token a directive?
  if (!lexer_.isCurrentTokenADirective())
    return nullptr;

  // Allocate a StringLiteralNode for the directive.
  auto *strLit = setLocation(
      tok_,
      tok_,
      new (context_) ESTree::StringLiteralNode(tok_->getStringLiteral()));
  auto endLoc = tok_->getEndLoc();

  // Actually process the directive. Note that we want to do that before we
  // have consumed any more tokens - strictness can affect the interpretation
  // of tokens.
  processDirective(strLit->_value);

  advance(JSLexer::AllowDiv);

  // Consume the optional semicolon.
  if (check(TokenKind::semi))
    endLoc = advance().End;

  // Allocate an ExpressionStatementNode for the directive.
  return setLocation(
      strLit,
      endLoc,
      new (context_) ESTree::ExpressionStatementNode(strLit, strLit->_value));
}

namespace {
/// Upcast an Optional node type to a generic NodePtr, e.g.
/// \p Optional<FunctionExpressionNode> to \p Optional<NodePtr>.
template <typename T>
Optional<ESTree::NodePtr> castNode(Optional<T> node) {
  if (!node.hasValue())
    return None;
  return Optional<ESTree::NodePtr>(node.getValue());
}

class PreParser {
  AllocationScope scope_;

 public:
  JSParserImpl parser;

  explicit PreParser(Context &context, uint32_t bufferId)
      : scope_(context.getAllocator()), parser(context, bufferId, PreParse) {}
};

} // namespace

std::shared_ptr<JSParserImpl> JSParserImpl::preParseBuffer(
    Context &context,
    uint32_t bufferId) {
  PerfSection preparsing("Pre-Parsing JavaScript");
  auto preParser = std::make_shared<PreParser>(context, bufferId);
  auto result = preParser->parser.parse();
  if (!result.hasValue())
    return nullptr;

  // Return a pointer only to JSParserImpl, while aliasing and maintaining
  // ownership of the entire PreParser object.
  return std::shared_ptr<JSParserImpl>(preParser, &preParser->parser);
}

Optional<ESTree::NodePtr> JSParserImpl::parseLazyFunction(
    ESTree::NodeKind kind,
    bool paramYield,
    bool paramAwait,
    SMLoc start) {
  seek(start);

  paramYield_ = paramYield;
  paramAwait_ = paramAwait;

  switch (kind) {
    case ESTree::NodeKind::FunctionExpression:
      return castNode(parseFunctionExpression(true));

    case ESTree::NodeKind::FunctionDeclaration:
      return castNode(parseFunctionDeclaration(ParamReturn, true));

    case ESTree::NodeKind::Property: {
      auto node = parsePropertyAssignment(true);
      assert(node && "Reparsing of property assignment failed");
      if (auto *prop = dyn_cast<ESTree::PropertyNode>(*node)) {
        return prop->_value;
      } else {
        // This isn't technically (llvm_)unreachable, since it'll happen if you
        // fudge the source buffer during execution. Assert and fail instead.
        assert(false && "Expected a getter/setter function");
        return None;
      }
    }

    default:
      llvm_unreachable("Asked to parse unexpected node type");
  }
}
} // namespace detail
} // namespace parser
} // namespace hermes
