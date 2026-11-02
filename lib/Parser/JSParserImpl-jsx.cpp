/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSParserImpl.h" // @manual

#include "llvh/Support/SaveAndRestore.h"

using llvh::cast;
using llvh::dyn_cast;
using llvh::isa;

namespace hermes {
namespace parser {
namespace detail {

#if HERMES_PARSE_JSX

Optional<ESTree::Node *> JSParserImpl::parseJSX() {
  assert(check(TokenKind::less));
  llvh::SaveAndRestore<uint32_t> saveDepth(jsxDepth_, 0);
  SMLoc start = advance(JSLexer::GrammarContext::AllowJSXIdentifier).Start;
  if (check(TokenKind::greater)) {
    return parseJSXFragment(start);
  }
  return parseJSXElement(start);
}

/// \return true if the opening and closing tag names match, which is needed
/// to define a JSXElement.
static bool tagNamesMatch(
    ESTree::JSXOpeningElementNode *opening,
    ESTree::JSXClosingElementNode *closing) {
  // Loop over member expressions or namespace names, stopping when both
  // `name1` and `name2` are the same JSXIdentifier or when they mismatch.
  ESTree::Node *name1 = opening->_name;
  ESTree::Node *name2 = closing->_name;
  for (;;) {
    if (auto *name1ID = dyn_cast<ESTree::JSXIdentifierNode>(name1)) {
      if (auto *name2ID = dyn_cast<ESTree::JSXIdentifierNode>(name2)) {
        return name1ID->_name == name2ID->_name;
      }
      return false;
    } else if (auto *name1NS = dyn_cast<ESTree::JSXNamespacedNameNode>(name1)) {
      if (auto *name2NS = dyn_cast<ESTree::JSXNamespacedNameNode>(name2)) {
        // ESTree spec dictates that both namespace and name are JSXIdentifier.
        auto *name1NSID = cast<ESTree::JSXIdentifierNode>(name1NS->_namespace);
        auto *name1ID = cast<ESTree::JSXIdentifierNode>(name1NS->_name);
        auto *name2NSID = cast<ESTree::JSXIdentifierNode>(name2NS->_namespace);
        auto *name2ID = cast<ESTree::JSXIdentifierNode>(name2NS->_name);
        return name1NSID->_name == name2NSID->_name &&
            name1ID->_name == name2ID->_name;
      }
      return false;
    } else {
      auto *name1ME = cast<ESTree::JSXMemberExpressionNode>(name1);
      if (auto *name2ME = dyn_cast<ESTree::JSXMemberExpressionNode>(name2)) {
        auto *name1ID = cast<ESTree::JSXIdentifierNode>(name1ME->_property);
        auto *name2ID = cast<ESTree::JSXIdentifierNode>(name2ME->_property);
        if (name1ID->_name != name2ID->_name) {
          return false;
        }
        // Both names are JSXMemberExpression with matching property names.
        // Compare the object names.
        name1 = name1ME->_object;
        name2 = name2ME->_object;
        continue;
      }
      return false;
    }
  }
}

Optional<ESTree::Node *> JSParserImpl::parseJSXElement(SMLoc start) {
  llvh::SaveAndRestore<uint32_t> saveDepth(jsxDepth_, jsxDepth_ + 1);
  auto optOpening = parseJSXOpeningElement(start);
  if (!optOpening)
    return None;
  if ((*optOpening)->_selfClosing) {
    return setLocation(
        start,
        *optOpening,
        new (context_) ESTree::JSXElementNode(*optOpening, {}, nullptr));
  }
  ESTree::JSXOpeningElementNode *opening = *optOpening;

  // Parse JSXChildren.
  ESTree::NodeList children{};

  auto optClosing = parseJSXChildren(children);
  if (!optClosing)
    return None;

  // Check that the closing is not a fragment and that the name matches.
  if (ESTree::JSXClosingElementNode *closing =
          dyn_cast<ESTree::JSXClosingElementNode>(*optClosing)) {
    if (!tagNamesMatch(opening, closing)) {
      error((*optClosing)->getSourceRange(), "Closing tag must match opening");
      sm_.note(opening->getSourceRange().Start, "location of opening");
    }
  } else {
    error(
        (*optClosing)->getSourceRange(), "Closing tag must not be a fragment");
    sm_.note(opening->getSourceRange().Start, "location of opening");
  }

  return setLocation(
      start,
      *optClosing,
      new (context_)
          ESTree::JSXElementNode(opening, std::move(children), *optClosing));
}

Optional<ESTree::JSXOpeningElementNode *> JSParserImpl::parseJSXOpeningElement(
    SMLoc start) {
  auto optName = parseJSXElementName(AllowJSXMemberExpression::Yes);
  if (!optName)
    return None;
  ESTree::Node *name = *optName;

  ESTree::Node *typeArgs = nullptr;
  if (check(TokenKind::less)) {
    auto optTypeArgs = parseTypeArgsFlow();
    if (!optTypeArgs) {
      return None;
    }
    typeArgs = *optTypeArgs;
  }

  ESTree::NodeList attributes{};
  while (!check(TokenKind::slash, TokenKind::greater)) {
    if (check(TokenKind::l_brace)) {
      auto optSpread = parseJSXSpreadAttribute();
      if (!optSpread)
        return None;
      attributes.push_back(**optSpread);
      continue;
    }

    auto optAttr = parseJSXAttribute();
    if (!optAttr)
      return None;
    attributes.push_back(**optAttr);
  }

  bool selfClosing = checkAndEat(TokenKind::slash);

  SMLoc end = tok_->getEndLoc();
  if (!need(TokenKind::greater, "at end of JSX tag", "start of tag", start))
    return None;

  if (selfClosing && jsxDepth_ <= 1) {
    // Done with JSX for now, return to standard JS mode.
    advance();
  } else {
    // Still in JSX, children after this element.
    lexer_.advanceInJSXChild();
  }

  return setLocation(
      start,
      end,
      new (context_) ESTree::JSXOpeningElementNode(
          name, std::move(attributes), selfClosing, typeArgs));
}

Optional<ESTree::Node *> JSParserImpl::parseJSXFragment(SMLoc start) {
  assert(check(TokenKind::greater));
  // JSXFragment:
  // < > JSXChildren[opt] < / >
  //   ^
  llvh::SaveAndRestore<uint32_t> saveDepth(jsxDepth_, jsxDepth_ + 1);
  ESTree::Node *opening =
      setLocation(start, tok_, new (context_) ESTree::JSXOpeningFragmentNode());
  lexer_.advanceInJSXChild();

  // Parse JSXChildren.
  ESTree::NodeList children{};

  auto optClosing = parseJSXChildren(children);
  if (!optClosing)
    return None;

  // Check that the closing is a fragment.
  if (!isa<ESTree::JSXClosingFragmentNode>(*optClosing)) {
    error((*optClosing)->getSourceRange(), "Closing tag must be a fragment");
    lexer_.getSourceMgr().note(
        opening->getSourceRange().Start, "location of opening");
    return None;
  }

  return setLocation(
      start,
      *optClosing,
      new (context_)
          ESTree::JSXFragmentNode(opening, std::move(children), *optClosing));
}

Optional<ESTree::Node *> JSParserImpl::parseJSXChildren(
    ESTree::NodeList &children) {
  // Keep looping until we encounter a closing element or a JSXClosingFragment.
  for (;;) {
    if (check(TokenKind::less)) {
      // JSXElement or closing tag.
      SMLoc start = advance(JSLexer::GrammarContext::AllowJSXIdentifier).Start;
      if (check(TokenKind::slash)) {
        // < /
        //   ^
        // Start of a JSXClosingElement or JSXClosingFragment.
        auto optClosing = parseJSXClosing(start);
        if (!optClosing)
          return None;
        return *optClosing;
      }
      // Using a JSXFragment as a child node appears to be disallowed by the
      // spec, but code frequently uses this pattern and all parsers appear to
      // support it.
      auto optElem = check(TokenKind::greater) ? parseJSXFragment(start)
                                               : parseJSXElement(start);
      if (!optElem)
        return None;
      children.push_back(**optElem);
    } else if (check(TokenKind::l_brace)) {
      // { JSXChildExpression[opt] }
      // ^
      SMRange startRange = advance();
      SMLoc start = startRange.Start;
      if (check(TokenKind::r_brace)) {
        // { }
        //   ^
        SMRange endRange = tok_->getSourceRange();
        children.push_back(*setLocation(
            start,
            endRange.End,
            new (context_) ESTree::JSXExpressionContainerNode(setLocation(
                startRange.End,
                endRange.Start,
                new (context_) ESTree::JSXEmptyExpressionNode()))));
      } else {
        // { JSXChildExpression }
        //   ^
        auto optChildExpr = parseJSXChildExpression(start);
        if (!optChildExpr)
          return None;
        if (!need(
                TokenKind::r_brace,
                "in JSX child expression",
                "start of expression",
                start))
          return None;
        children.push_back(**optChildExpr);
      }
      lexer_.advanceInJSXChild();
    } else {
      /// JSXText handled by the lexer.
      if (!need(TokenKind::jsx_text, "in JSX child expression", nullptr, {}))
        return None;
      children.push_back(*setLocation(
          tok_,
          tok_,
          new (context_) ESTree::JSXTextNode(
              tok_->getJSXTextValue(), tok_->getJSXTextRaw())));
      lexer_.advanceInJSXChild();
    }
  }
}

Optional<ESTree::Node *> JSParserImpl::parseJSXChildExpression(SMLoc start) {
  if (checkAndEat(TokenKind::dotdotdot)) {
    auto optAssign = parseAssignmentExpression();
    if (!optAssign)
      return None;
    return setLocation(
        start, tok_, new (context_) ESTree::JSXSpreadChildNode(*optAssign));
  }
  auto optAssign = parseAssignmentExpression();
  if (!optAssign)
    return None;
  return setLocation(
      start,
      tok_,
      new (context_) ESTree::JSXExpressionContainerNode(*optAssign));
}

Optional<ESTree::Node *> JSParserImpl::parseJSXSpreadAttribute() {
  assert(check(TokenKind::l_brace));
  SMLoc start = advance().Start;

  // { ... AssignmentExpression }
  //   ^

  if (!eat(
          TokenKind::dotdotdot,
          JSLexer::GrammarContext::AllowRegExp,
          "in JSX spread attribute",
          "location of attribute",
          start))
    return None;

  auto optAssign = parseAssignmentExpression();
  if (!optAssign)
    return None;

  SMLoc end = tok_->getEndLoc();
  if (!eat(
          TokenKind::r_brace,
          JSLexer::GrammarContext::AllowJSXIdentifier,
          "in JSX spread attribute",
          "location of attribute",
          start))
    return None;

  return setLocation(
      start, end, new (context_) ESTree::JSXSpreadAttributeNode(*optAssign));
}

Optional<ESTree::Node *> JSParserImpl::parseJSXAttribute() {
  SMLoc start = tok_->getStartLoc();

  auto optName = parseJSXElementName(AllowJSXMemberExpression::No);
  if (!optName)
    return None;
  ESTree::Node *name = *optName;

  if (!checkAndEat(
          TokenKind::equal, JSLexer::GrammarContext::AllowJSXIdentifier)) {
    return setLocation(
        *optName,
        *optName,
        new (context_) ESTree::JSXAttributeNode(*optName, nullptr));
  }

  // JSXAttributeInitializer:
  // = JSXAttributeValue
  //   ^
  ESTree::Node *value = nullptr;
  if (check(TokenKind::string_literal)) {
    UniqueString *raw = lexer_.getStringLiteral(tok_->inputStr());
    value = setLocation(
        tok_,
        tok_,
        new (context_)
            ESTree::JSXStringLiteralNode(tok_->getStringLiteral(), raw));
    advance(JSLexer::GrammarContext::AllowJSXIdentifier);
  } else {
    // { AssignmentExpression }
    // ^
    if (!need(
            TokenKind::l_brace,
            "in JSX attribute",
            "location of attribute",
            start))
      return None;

    SMLoc valueStart = advance().Start;

    auto optAssign = parseAssignmentExpression();
    if (!optAssign)
      return None;

    SMLoc valueEnd = tok_->getEndLoc();
    if (!eat(
            TokenKind::r_brace,
            JSLexer::GrammarContext::AllowJSXIdentifier,
            "in JSX attribute",
            "location of attribute",
            start))
      return None;

    value = setLocation(
        valueStart,
        valueEnd,
        new (context_) ESTree::JSXExpressionContainerNode(*optAssign));
  }

  assert(value && "value must be set by one of the branches");

  return setLocation(
      start, value, new (context_) ESTree::JSXAttributeNode(name, value));
}

Optional<ESTree::Node *> JSParserImpl::parseJSXClosing(SMLoc start) {
  assert(check(TokenKind::slash));
  advance(JSLexer::GrammarContext::AllowJSXIdentifier);

  if (check(TokenKind::greater)) {
    SMLoc end = tok_->getEndLoc();
    if (jsxDepth_ > 1) {
      lexer_.advanceInJSXChild();
    } else {
      // Done with JSX, advance normally.
      advance();
    }
    return setLocation(
        start, end, new (context_) ESTree::JSXClosingFragmentNode());
  }

  auto optName = parseJSXElementName(AllowJSXMemberExpression::Yes);
  if (!optName)
    return None;

  if (!need(
          TokenKind::greater,
          "at end of JSX closing tag",
          "start of tag",
          start))
    return None;

  SMLoc end = tok_->getEndLoc();
  if (jsxDepth_ > 1) {
    lexer_.advanceInJSXChild();
  } else {
    // Done with JSX, advance normally.
    advance();
  }

  return setLocation(
      start, end, new (context_) ESTree::JSXClosingElementNode(*optName));
}

Optional<ESTree::Node *> JSParserImpl::parseJSXElementName(
    AllowJSXMemberExpression allowJSXMemberExpression) {
  SMLoc start = tok_->getStartLoc();

  if (!check(TokenKind::identifier) && !tok_->isResWord()) {
    errorExpected(TokenKind::identifier, "as JSX element name", nullptr, {});
    return None;
  }

  ESTree::Node *name = setLocation(
      start,
      tok_,
      new (context_) ESTree::JSXIdentifierNode(tok_->getResWordOrIdentifier()));
  advance(JSLexer::GrammarContext::AllowJSXIdentifier);

  if (check(TokenKind::colon)) {
    // JSXNamespacedName:
    // JSXIdentifier : JSXIdentifier
    // ^
    advance(JSLexer::GrammarContext::AllowJSXIdentifier);
    if (!check(TokenKind::identifier) && !tok_->isResWord()) {
      errorExpected(
          TokenKind::identifier,
          "in JSX element name",
          "start of JSX element name",
          start);
      return None;
    }

    ESTree::Node *child = setLocation(
        tok_,
        tok_,
        new (context_)
            ESTree::JSXIdentifierNode(tok_->getResWordOrIdentifier()));
    advance(JSLexer::GrammarContext::AllowJSXIdentifier);
    return setLocation(
        start,
        child,
        new (context_) ESTree::JSXNamespacedNameNode(name, child));
  }

  while (check(TokenKind::period)) {
    // JSXNamespacedName:
    // JSXMemberExpression . JSXIdentifier
    // ^
    advance(JSLexer::GrammarContext::AllowJSXIdentifier);
    if (!check(TokenKind::identifier) && !tok_->isResWord()) {
      errorExpected(
          TokenKind::identifier,
          "in JSX element name",
          "start of JSX element name",
          start);
      return None;
    }

    ESTree::Node *child = setLocation(
        tok_,
        tok_,
        new (context_)
            ESTree::JSXIdentifierNode(tok_->getResWordOrIdentifier()));
    advance(JSLexer::GrammarContext::AllowJSXIdentifier);

    name = setLocation(
        start,
        child,
        new (context_) ESTree::JSXMemberExpressionNode(name, child));
  }

  if (isa<ESTree::MemberExpressionNode>(name) &&
      allowJSXMemberExpression == AllowJSXMemberExpression::No) {
    error(name->getSourceRange(), "unexpected member expression");
  }

  return name;
}

#endif

} // namespace detail
} // namespace parser
} // namespace hermes
