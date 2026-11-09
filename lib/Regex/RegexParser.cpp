/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Platform/Unicode/CharacterProperties.h"
#include "hermes/Platform/Unicode/CodePointSet.h"

#include "hermes/Regex/Regex.h"
#include "hermes/Regex/RegexTraits.h"
namespace hermes {
namespace regex {

using llvh::Optional;

/// Parser is a class responsible for implementing the productions of the
/// regex grammar, using a handwritten recursive descent parser.
template <class RegexType, class ForwardIterator>
class Parser {
  // The character type that we are parsing.
  using CharT = typename std::iterator_traits<ForwardIterator>::value_type;

  // A Unicode code point.
  using CodePoint = uint32_t;

  // The type of a node in our regex.
  using Node = typename RegexType::Node;

  // An element of a class ranges.
  // This may be either a code point, or a CharacterClass.
  struct ClassAtom {
    CodePoint codePoint = -1;
    llvh::Optional<CharacterClass> charClass{};
    llvh::Optional<CharacterClassCodepointRanges> codePointRanges{};

    explicit ClassAtom(CodePoint cp) : codePoint(cp) {}
    ClassAtom(CharacterClass::Type cc, bool invert)
        : charClass(CharacterClass(cc, invert)) {}
    ClassAtom(llvh::ArrayRef<UnicodeRangePoolRef> ranges, bool invert)
        : codePointRanges(CharacterClassCodepointRanges(ranges, invert)) {}
  };

  // The regexp that we are building. This receives the results of our
  // productions. This may be a real regex or a dummy regex.
  RegexType *re_;

  // The current and end iterators in the regex input string.
  ForwardIterator current_;
  const ForwardIterator end_;

  // The error that was set, if any.
  constants::ErrorType error_ = constants::ErrorType::None;

  // Flags for the regex.
  SyntaxFlags curFlags_;

  // See comment --DecimalEscape--.
  const uint32_t backRefLimit_;
  uint32_t maxBackRef_ = 0;

  // When parsing, this is the default assumption we make for if there are
  // groups or not. That means this value may not reflect reality.
  bool hasNamedGroups_;

  /// Set the error \p err, if not already set to a different error.
  /// Also move our input to end, to abort parsing.
  /// \return false, for convenience.
  void setError(constants::ErrorType err) {
    if (error_ == constants::ErrorType::None) {
      error_ = err;
      current_ = end_;
    }
  }

  /// Type wrapping up a quantifier.
  /// This includes all bracket expressions /a{1, 3}/, as well as *, +, ?
  struct Quantifier {
    /// Minimum number of repetitions.
    uint32_t min_ = 0;

    /// Maximum number of repetitions, or uint32_t::max for unlimited.
    uint32_t max_ = std::numeric_limits<uint32_t>::max();

    /// Whether this quantifier is greedy.
    bool greedy_ = true;

    /// The first marked subexpression of the term that this quantifies.
    /// For example, in the regex /(a)(b)((c)|d){3, 5}/ this would be 2, because
    /// we are quantifying the marked subexpression at index 2.
    uint16_t startMarkedSubexprs_;

    /// The start node of the expression which we are quantifying. This is owned
    /// by the regex. The end node is always the last node of the regex.
    Node *quantifiedNodeStart_ = nullptr;
  };

  /// \return a quantifier prepopulated with our current state.
  Quantifier prepareQuantifier() {
    Quantifier q;
    q.startMarkedSubexprs_ = re_->markedCount();
    q.quantifiedNodeStart_ = re_->currentNode();
    return q;
  }

  /// Given a quantifier \p quant, replace the expression it captured with a
  /// quantified expression.
  void applyQuantifier(const Quantifier &quant) {
    if (quant.min_ > quant.max_) {
      setError(constants::ErrorType::BraceRange);
      return;
    }
    auto quantifiedExpression = re_->spliceOut(quant.quantifiedNodeStart_);
    re_->pushLoop(
        quant.min_,
        quant.max_,
        std::move(quantifiedExpression),
        quant.startMarkedSubexprs_,
        quant.greedy_);
  }

  /// Consume a single character which must be the next character in the string.
  /// \return the character.
  CharT consume(CharT c) {
    assert(check(c) && "Could not consume char");
    current_++;
    return c;
  }

  /// Attempt to consume a string literal.
  /// \return true if the entire string could be consumed, false if not.
  bool tryConsume(const char *seq) {
    auto cursor = current_;
    for (size_t i = 0; seq[i]; i++) {
      if (cursor == end_ || *cursor != seq[i])
        return false;
      ++cursor;
    }
    current_ = cursor;
    return true;
  }

  /// Attempt to consume a single character.
  /// \return true if the entire string could be consumed, false if not.
  bool tryConsume(CharT c) {
    if (!check(c))
      return false;
    consume(c);
    return true;
  }

  /// Check if the next character to be consumed is \p c
  /// \return true if the character is \p c, false if not.
  bool check(CharT c) const {
    return current_ != end_ && *current_ == c;
  }

  /// If the cursor is on a character (i.e. not at end_), and that character
  /// satisfies a predicate \p pred, then advance the cursor.
  /// \return the character if it satisfies the predicate, otherwise None.
  template <typename Pred>
  Optional<CharT> consumeCharIf(const Pred &pred) {
    if (current_ != end_ && pred(*current_)) {
      CharT c = *current_;
      consume(c);
      return c;
    }
    return llvh::None;
  }

  /// Our parser uses an explicit stack to avoid stack overflow for deeply
  /// nested regexps. This represents a level in the stack.
  struct ParseStackElement {
    enum Type {
      /// We are parsing an alternation: a|b|c
      Alternation,

      /// We are parsing a capturing group: ().
      CapturingGroup,

      /// We are parsing a named capturing group: (?<id ...>).
      NamedCapturingGroup,

      /// We are parsing a non-capturing group: (?:).
      NonCapturingGroup,

      /// We are parsing a lookaround:
      // Positive lookahead (?=)
      // Negative lookahead (?!)
      // Positive lookbehind (?<=)
      // Negative lookbehind (?<!)
      LookAround,

      /// We are parsing a modifier group: (?ims-ims:).
      ModifierGroup,

    } type;

    /// The splice point.
    /// For Alternation elements, nodes after this represent an alternative.
    /// For Group elements, nodes after this are part of the group.
    Node *splicePoint{nullptr};

    /// For group elements, the marked subexpression index for this element.
    /// For lookaround elements, the marked subexpression index when this
    /// lookaround element was pushed onto the stack.
    /// Ignored for alternation elements.
    uint32_t mexp{0};

    /// A quantifier prepared for the currently parsed group.
    /// Ignored for alternations (which cannot be quantified).
    Quantifier quant{};

    /// If this is an Alternation, the list of alternatives
    /// Ignored for group elements.
    std::vector<NodeList> alternatives{};

    // True if this lookaround represents a negative lookaround. Ignored for
    // non-lookarounds
    bool negateLookaround{false};

    // True if this lookaround is a lookahead. Ignored for non-lookarounds.
    bool forwardLookaround{false};

    /// Saved flags to restore when closing a ModifierGroup.
    SyntaxFlags savedFlags{};

    explicit ParseStackElement(Type type) : type(type) {}
  };

  using ParseStack = llvh::SmallVector<ParseStackElement, 4>;

  /// Push an empty alternation onto the parse stack \p stack.
  void openAlternation(ParseStack &stack) {
    // Should never have two adjacent alternations on the stack
    assert(
        stack.empty() || stack.back().type != ParseStackElement::Alternation);
    ParseStackElement elem(ParseStackElement::Alternation);
    elem.splicePoint = re_->currentNode();
    stack.push_back(std::move(elem));
  }

  /// Close any alternation on top of the stack.
  void closeAlternation(ParseStack &stack) {
    if (!stack.empty() && stack.back().type == ParseStackElement::Alternation) {
      auto alternatives = std::move(stack.back().alternatives);
      auto last = re_->spliceOut(stack.back().splicePoint);
      stack.pop_back();
      alternatives.push_back(std::move(last));
      re_->pushAlternation(std::move(alternatives));
    }
  }

  /// Open a capturing group, pushing it onto \p stack.
  void openCapturingGroup(ParseStack &stack) {
    ParseStackElement elem(ParseStackElement::CapturingGroup);
    // Quantifier must be prepared before incrementing the marked counter
    // because the newly opened capture group is the first one being quantified
    // by it.
    elem.quant = prepareQuantifier();
    if (LLVM_UNLIKELY(re_->markedCount() >= constants::kMaxCaptureGroupCount)) {
      setError(constants::ErrorType::PatternExceedsParseLimits);
      return;
    }
    elem.mexp = re_->incrementMarkedCount();
    elem.splicePoint = re_->currentNode();
    stack.push_back(std::move(elem));
  }

  /// Open a named group, pushing it onto \p stack.
  void openNamedCapturingGroup(ParseStack &stack) {
    ParseStackElement elem(ParseStackElement::NamedCapturingGroup);
    // Quantifier must be prepared before incrementing the marked counter
    // because the newly opened capture group is the first one being quantified
    // by it.
    elem.quant = prepareQuantifier();
    if (LLVM_UNLIKELY(re_->markedCount() >= constants::kMaxCaptureGroupCount)) {
      setError(constants::ErrorType::PatternExceedsParseLimits);
      return;
    }
    elem.mexp = re_->incrementMarkedCount();
    elem.splicePoint = re_->currentNode();

    GroupName identifier;
    if (!tryConsumeGroupName(identifier)) {
      setError(constants::ErrorType::InvalidCaptureGroupName);
      return;
    }

    // If we couldn't add this named group, that means a name already existed.
    if (!re_->addNamedCaptureGroup(std::move(identifier), elem.mexp)) {
      setError(constants::ErrorType::DuplicateCaptureGroupName);
      return;
    }

    hasNamedGroups_ = true;
    stack.push_back(std::move(elem));
  }
  /// Open a non-capturing group, pushing it onto \p stack.
  void openNonCapturingGroup(ParseStack &stack) {
    ParseStackElement elem(ParseStackElement::NonCapturingGroup);
    elem.quant = prepareQuantifier();
    elem.splicePoint = re_->currentNode();
    stack.push_back(std::move(elem));
  }

  /// Consume modifiers after '(?' has been consumed:
  // (?setFlags:pattern)
  // (?setFlags-clearFlags:pattern)
  // Note that pattern isn't consumed here, only up to the flags. The allowed
  // flags are: i,m, and s.
  /// \return the new SyntaxFlags, or None if there was an error.
  Optional<SyntaxFlags> consumeModifiers() {
    SyntaxFlags newFlags = curFlags_;
    // Keep track of the flags we've seen so far. No flag should ever appear
    // twice.
    SyntaxFlags seenFlags{};
    bool parsingNegativeFlags = false;
    bool hasModifierChar = false;

    /// Get and set modifier flags on a SyntaxFlags by character.
    auto getFlag = [](const SyntaxFlags &flags, CharT c) -> bool {
      switch (c) {
        case 'i':
          return flags.ignoreCase;
        case 'm':
          return flags.multiline;
        case 's':
          return flags.dotAll;
        default:
          llvm_unreachable("invalid flag");
      }
    };
    auto setFlagValue = [](SyntaxFlags &flags, CharT c, bool value) {
      switch (c) {
        case 'i':
          flags.ignoreCase = value;
          break;
        case 'm':
          flags.multiline = value;
          break;
        case 's':
          flags.dotAll = value;
          break;
        default:
          llvm_unreachable("invalid flag");
      }
    };

    // Iterate until all modifiers are parsed. Any break out of this
    // loop is treated as an error.
    while (current_ != end_) {
      CharT c = *current_;
      if (c == 'i' || c == 'm' || c == 's') {
        if (getFlag(seenFlags, c)) {
          // Error, duplicate flags.
          break;
        }
        setFlagValue(seenFlags, c, 1);
        setFlagValue(newFlags, c, parsingNegativeFlags ? 0 : 1);
        hasModifierChar = true;
        consume(c);
      } else if (tryConsume('-')) {
        if (parsingNegativeFlags) {
          // Error, can't have `-` twice.
          break;
        } else {
          parsingNegativeFlags = true;
        }
      } else if (tryConsume(':')) {
        if (!hasModifierChar) {
          // Error, ending the modifiers with no flags specified.
          break;
        }
        return newFlags;
      } else {
        // Error, unexpected character.
        break;
      }
    }
    setError(constants::ErrorType::InvalidFlags);
    return llvh::None;
  }

  /// Open a modifier group, pushing it onto \p stack.
  /// \p newFlags are the flags to use inside the group.
  void openModifierGroup(ParseStack &stack, SyntaxFlags newFlags) {
    ParseStackElement elem(ParseStackElement::ModifierGroup);
    elem.quant = prepareQuantifier();
    elem.splicePoint = re_->currentNode();
    elem.savedFlags = curFlags_;
    curFlags_ = newFlags;
    stack.push_back(std::move(elem));
  }

  /// Open a lookaround, pushing it onto \p stack.
  void openLookaround(ParseStack &stack, bool negate, bool forwards) {
    ParseStackElement elem(ParseStackElement::LookAround);
    elem.mexp = re_->markedCount();
    elem.forwardLookaround = forwards;
    elem.negateLookaround = negate;
    elem.quant = prepareQuantifier();
    elem.splicePoint = re_->currentNode();
    stack.push_back(std::move(elem));
  }

  /// Close a group or lookaround on top of \p stack, emitting the proper nodes
  /// into the regex.
  void closeGroup(ParseStack &stack) {
    assert(!stack.empty() && "Stack must not be empty");
    ParseStackElement elem = std::move(stack.back());
    stack.pop_back();
    bool quantifierAllowed = true;
    switch (elem.type) {
      case ParseStackElement::Alternation:
        llvm_unreachable("Alternations must be popped via closeAlternation()");
        break;

      case ParseStackElement::NamedCapturingGroup:
      case ParseStackElement::CapturingGroup:
        re_->pushMarkedSubexpression(
            re_->spliceOut(elem.splicePoint), elem.mexp);
        break;

      case ParseStackElement::NonCapturingGroup:
        break;

      case ParseStackElement::ModifierGroup:
        curFlags_ = elem.savedFlags;
        break;

      case ParseStackElement::LookAround: {
        bool negate = elem.negateLookaround;
        bool forwards = elem.forwardLookaround;
        // ES11 Annex B.1.4 extends RegExp to allow quantifiers for
        // lookaheads when unicode is disabled.
        quantifierAllowed = !(curFlags_.unicode) && forwards;
        auto mexpStart = elem.mexp;
        auto mexpEnd = re_->markedCount();
        auto expr = re_->spliceOut(elem.splicePoint);
        re_->pushLookaround(
            std::move(expr), mexpStart, mexpEnd, negate, forwards);
        break;
      }
    }

    // If the group is followed by a quantifier, then quantify it.
    if (tryConsumeQuantifier(&elem.quant)) {
      if (!quantifierAllowed) {
        setError(constants::ErrorType::InvalidRepeat);
        return;
      }
      applyQuantifier(elem.quant);
    }
  }

  /// ES6 21.2.2.3 Disjunction.
  void consumeDisjunction() {
    Node *const firstNode = re_->currentNode();
    ParseStack stack;

    while (current_ != end_) {
      switch (*current_) {
        case '|': {
          consume('|');
          auto *splicePoint =
              stack.empty() ? firstNode : stack.back().splicePoint;
          auto nodes = re_->spliceOut(splicePoint);
          if (stack.empty() ||
              stack.back().type != ParseStackElement::Alternation) {
            // Open a new alternation
            openAlternation(stack);
          }
          stack.back().alternatives.push_back(std::move(nodes));
          break;
        }

        case '(': {
          // Open a new group of the right type.
          if (tryConsume("(?=")) {
            // Positive lookahead, negate = false, forwards = true
            openLookaround(stack, false, true);
          } else if (tryConsume("(?!")) {
            // Negative lookahead, negate = true, forwards = true
            openLookaround(stack, true, true);
          } else if (tryConsume("(?<=")) {
            // Positive lookbehind, negate = false, forwards = false
            openLookaround(stack, false, false);
          } else if (tryConsume("(?<!")) {
            // Negative lookbehind, negate = true, forwards = false
            openLookaround(stack, true, false);
          } else if (tryConsume("(?<")) {
            openNamedCapturingGroup(stack);
          } else if (tryConsume("(?:")) {
            openNonCapturingGroup(stack);
          } else if (tryConsume("(?")) {
            // This must be the beginning of a modifier group
            if (auto newFlags = consumeModifiers()) {
              openModifierGroup(stack, *newFlags);
              break;
            } else {
              assert(error_ != constants::ErrorType::None);
              return;
            }
          } else {
            consume('(');
            openCapturingGroup(stack);
          }
          break;
        }

        case ')': {
          // Close any in-flight alternation and pop a group stack
          // element.
          consume(')');
          closeAlternation(stack);
          if (stack.empty()) {
            setError(constants::ErrorType::UnbalancedParenthesis);
            return;
          }
          closeGroup(stack);
          break;
        }
      }
      consumeTerm();
    }

    assert(current_ == end_ && "Should have consumed all input");
    closeAlternation(stack);
    if (!stack.empty()) {
      setError(constants::ErrorType::UnbalancedParenthesis);
    }
  }

  /// ES6 21.2.2.5 Term.
  void consumeTerm() {
    while (current_ != end_) {
      Quantifier quant = prepareQuantifier();
      bool quantifierAllowed = true;
      const CharT c = *current_;
      switch (c) {
        case '^':
          re_->pushLeftAnchor(curFlags_.multiline);
          consume('^');
          quantifierAllowed = false;
          break;

        case '$':
          re_->pushRightAnchor(curFlags_.multiline);
          consume('$');
          quantifierAllowed = false;
          break;

        case '\\': {
          consume('\\');
          // This may be an ES6 21.2.2.6 Assertion (\b or \B) or an AtomeEscape.
          if (current_ == end_) {
            setError(constants::ErrorType::EscapeIncomplete);
            return;
          } else if (*current_ == 'b' || *current_ == 'B') {
            re_->pushWordBoundary(
                *current_ == 'B' /* invert */, curFlags_.ignoreCase);
            consume(*current_);
            quantifierAllowed = false;
          } else {
            consumeAtomEscape();
          }
          break;
        }

        case '.': {
          consume('.');
          re_->pushMatchAny(curFlags_.dotAll);
          break;
        }

        case '[': {
          consumeCharacterClass();
          break;
        }

        case '*':
        case '+':
        case '?': {
          setError(constants::ErrorType::InvalidRepeat);
          return;
        }

        case '{': {
          // Under Unicode, this is always an error.
          // Without Unicode, it is an error if it is a valid quantifier.
          // (extension from ES11 Annex B.1.4)
          Quantifier tmp;
          if (tryConsumeQuantifier(&tmp)) {
            setError(constants::ErrorType::InvalidRepeat);
            return;
          } else if (curFlags_.unicode) {
            setError(constants::ErrorType::InvalidQuantifierBracket);
            return;
          }
          re_->pushChar(consume('{'), curFlags_.ignoreCase);
          break;
        }

        case '|':
        case '(':
        case ')': {
          // This is a new alternation, or the beginning or end of a group or
          // assertion. Transfer control back to consumeDisjunction() so it can
          // act on it.
          return;
        }

        case '}':
        case ']': {
          // These syntax characters are allowed as atoms in
          // ExtendedPatternCharacter production of ES9 Annex B 1.4.
          // However they are disallowed under Unicode, where Annex B does not
          // apply.
          if (curFlags_.unicode) {
            setError(
                c == '}' ? constants::ErrorType::InvalidQuantifierBracket
                         : constants::ErrorType::UnbalancedBracket);
            return;
          }
        }
          [[fallthrough]];

        default: {
          // Ordinary character or surrogate pair.
          if (auto cp = tryConsumeSurrogatePair()) {
            re_->pushChar(*cp, curFlags_.ignoreCase);
          } else {
            re_->pushChar(consume(c), curFlags_.ignoreCase);
          }
          break;
        }
      }

      // We just parsed one term. Try parsing a quantifier.
      if (tryConsumeQuantifier(&quant)) {
        if (!quantifierAllowed) {
          setError(constants::ErrorType::InvalidRepeat);
          return;
        }
        applyQuantifier(quant);
      }
    }
  }

  /// If Unicode is set, try to consume a surrogate pair.
  Optional<CodePoint> tryConsumeSurrogatePair() {
    if (!(curFlags_.unicode))
      return llvh::None;
    auto saved = current_;
    auto hi = consumeCharIf(isHighSurrogate);
    auto lo = consumeCharIf(isLowSurrogate);
    if (hi && lo) {
      return utf16SurrogatePairToCodePoint(*hi, *lo);
    }
    current_ = saved;
    return llvh::None;
  }

  // If this can consume a surrogate pair, it writes two characters into the
  // UTF16 str and advances past the pair.
  bool tryConsumeAndAppendSurrogatePair(
      GroupName &str,
      bool (*lambdaPredicate)(uint32_t)) {
    auto saved = current_;
    auto hi = consumeCharIf(isHighSurrogate);
    auto lo = consumeCharIf(isLowSurrogate);
    if (hi && lo && lambdaPredicate(utf16SurrogatePairToCodePoint(*hi, *lo))) {
      str.push_back(*hi);
      str.push_back(*lo);
      return true;
    }
    current_ = saved;
    return false;
  }

  /// ES2021 22.2.1 Patterns - GroupName
  /// \return true if an identifier was successfully parsed.
  /// Else, \return false.
  bool tryConsumeGroupName(GroupName &identifierName) {
    bool firstChar = true;
    for (;;) {
      if (current_ == end_) {
        return false;
      }
      const CharT c = *current_;
      switch (c) {
        case '>':
          if (identifierName.size() == 0) {
            return false;
          }
          consume('>');
          return true;
        default: {
          if (firstChar) {
            bool matchedIdentifierStart =
                tryConsumeRegExpIdentifier(identifierName, isUnicodeIDStart);
            if (!matchedIdentifierStart) {
              return false;
            }
          } else {
            bool matchedIdentifierPart =
                tryConsumeRegExpIdentifier(identifierName, isUnicodeIDContinue);
            if (!matchedIdentifierPart) {
              return false;
            }
          }
        }
      }
      firstChar = false;
    }
  }

  /// ES2021 22.2.1 Patterns - RegExpIdentifier(Start|Part)
  // \return true if a RegExpIdentifier(Start|Part) could be consumed.
  // lambdaPredicate should be either UnicodeIDStart or UnicodeIDContinue, as
  // described in the spec.
  bool tryConsumeRegExpIdentifier(
      GroupName &identifierName,
      bool (*lambdaPredicate)(uint32_t)) {
    auto c = *current_;
    if (lambdaPredicate(c)) {
      consume(c);
      identifierName.push_back(c);
      return true;
    }
    auto saved = current_;
    if (tryConsume("\\") && check('u')) {
      if (auto cp = tryConsumeUnicodeEscapeSequence(true)) {
        if (!lambdaPredicate(*cp)) {
          return false;
        }
        writeCodePointToUTF16(*cp, identifierName);
        return true;
      }
      saved = current_;
    }
    if (tryConsumeAndAppendSurrogatePair(identifierName, lambdaPredicate)) {
      return true;
    }
    return false;
  }

  void writeCodePointToUTF16(uint32_t cp, GroupName &output) {
    assert(isValidCodePoint(cp) && "Invalid Unicode code point");
    if (cp <= 0x10000) {
      output.push_back((char16_t)cp);
      return;
    }
    // folowing conversion of codepoint -> surrogate pair taken from
    // JSLib/escape.cpp
    char16_t hi = (((cp - 0x10000) >> 10) & 0x3ff) + UTF16_HIGH_SURROGATE;
    char16_t lo = ((cp - 0x10000) & 0x3ff) + UTF16_LOW_SURROGATE;
    output.push_back(hi);
    output.push_back(lo);
  }

  /// ES6 21.2.2.7 Quantifier.
  bool tryConsumeQuantifier(Quantifier *quantifier) {
    if (!tryConsumeQuantifierPrefix(quantifier)) {
      return false;
    }
    quantifier->greedy_ = !tryConsume('?');
    return true;
  }

  /// ES6 21.2.2.7 QuantifierPrefix.
  bool tryConsumeQuantifierPrefix(Quantifier *quantifier) {
    if (current_ == end_) {
      return false;
    }
    constexpr uint32_t uintmax = std::numeric_limits<uint32_t>::max();
    switch (*current_) {
      case '*':
        consume('*');
        quantifier->min_ = 0;
        quantifier->max_ = uintmax;
        return true;

      case '+':
        consume('+');
        quantifier->min_ = 1;
        quantifier->max_ = uintmax;
        return true;

      case '?':
        consume('?');
        quantifier->min_ = 0;
        quantifier->max_ = 1;
        return true;

      case '{': {
        auto saved = current_;
        consume('{');
        if (auto v1 = tryConsumeDecimalIntegerLiteral()) {
          quantifier->min_ = *v1;
          if (!tryConsume(',')) {
            // Like {3}
            quantifier->max_ = *v1;
          } else if (auto v2 = tryConsumeDecimalIntegerLiteral()) {
            // Like {3,5}
            quantifier->max_ = *v2;
          } else {
            // Like {3,}
            quantifier->max_ = uintmax;
          }
          if (tryConsume('}')) {
            return true;
          }
        }
        current_ = saved;
        return false;
      }

      default:
        return false;
    }
  }

  /// ES6 21.2.2.13 CharacterClass.
  void consumeCharacterClass() {
    consume('[');
    bool unicode = curFlags_.unicode;
    bool negate = tryConsume('^');
    auto bracket = re_->startBracketList(negate, curFlags_.ignoreCase);

    // Helper to add a ClassAtom to our bracket.
    auto addClassAtom = [&bracket](const ClassAtom &atom) {
      if (atom.charClass) {
        bracket->addClass(*atom.charClass);
      } else if (atom.codePointRanges) {
        bracket->addCodePointRanges(
            atom.codePointRanges->rangeArray, atom.codePointRanges->inverted_);
      } else {
        bracket->addChar(atom.codePoint);
      }
      return true;
    };

    for (;;) {
      if (current_ == end_) {
        setError(constants::ErrorType::UnbalancedBracket);
        return;
      }

      if (tryConsume(']')) {
        // End of bracket.
        return;
      }

      // Parse a code point or character class.
      Optional<ClassAtom> first = tryConsumeBracketClassAtom();
      if (!first)
        continue;

      // See if we have a dash.
      if (!tryConsume('-')) {
        if (!addClassAtom(*first)) {
          return;
        }
        continue;
      }

      // We have a dash; we may have a range.
      Optional<ClassAtom> second = tryConsumeBracketClassAtom();
      if (!second) {
        // No second atom. For example: [a-].
        addClassAtom(*first);
        addClassAtom(ClassAtom('-'));
        continue;
      }

      // We have a range like [a-z].
      if (unicode &&
          (first->codePointRanges || second->codePointRanges ||
           first->charClass || second->charClass)) {
        // Unicode mode does not allow ranges with properties or character
        // classes: [\d-z] is invalid, so is [\p{Number}-z]
        setError(constants::ErrorType::CharacterRange);
        return;
      }

      if (first->charClass || second->charClass) {
        // The non-unicode path just pretends the range doesn't exist.
        // /[\d-A]/ is the same as /[\dA-]/.
        // Note we still have to process all three characters. For
        // example:
        // [\d-a-z] contains the atoms \d, -, a, -, z.
        // It does NOT contain the range a-z.
        addClassAtom(*first);
        addClassAtom(ClassAtom('-'));
        addClassAtom(*second);
        continue;
      }

      // Here we know it's a real range: [a-z] and not [\d-f].
      // However it could be out of order: [z-a]
      // ES6 21.2.2.15.1 "If i > j, throw a SyntaxError exception"
      if (first->codePoint > second->codePoint) {
        setError(constants::ErrorType::CharacterRange);
        return;
      }
      // This range has been validated.
      bracket->addRange(first->codePoint, second->codePoint);
    }
  }

  Optional<ClassAtom> tryConsumeBracketClassAtom() {
    if (current_ == end_) {
      return llvh::None;
    }
    CharT c = *current_;
    switch (c) {
      case ']': {
        // End of bracket. Note we don't consume it here.
        return llvh::None;
      }

      case '\\': {
        consume('\\');
        if (current_ == end_) {
          setError(constants::ErrorType::EscapeIncomplete);
          return llvh::None;
        }
        CharT ec = *current_;
        switch (ec) {
          case 'c':
            return ClassAtom(
                tryConsumeControlEscape(true /* inClass */).getValueOr('\\'));

            /// ES6 21.2.2.12 CharacterClassEscape.
            /// Note this is used inside brackets only, like /[\d]/.
          case 'd':
          case 'D': {
            consume(ec);
            return ClassAtom(CharacterClass::Digits, ec == 'D' /* invert */);
          }
          case 's':
          case 'S': {
            consume(ec);
            return ClassAtom(CharacterClass::Spaces, ec == 'S' /* invert */);
          }
          case 'w':
          case 'W': {
            consume(ec);
            return ClassAtom(CharacterClass::Words, ec == 'W' /* invert */);
          }

#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES
          case 'p':
          case 'P': {
            if (curFlags_.unicode) {
              consume(ec);
              std::string propertyName;
              std::string propertyValue;
              if (!tryConsume('{') ||
                  !tryConsumeUnicodePropertyValueExpression(
                      propertyName, propertyValue)) {
                setError(constants::ErrorType::InvalidPropertyName);
                return llvh::None;
              }

              auto codePointRanges =
                  unicodePropertyRanges(propertyName, propertyValue);
              if (codePointRanges.empty()) {
                setError(constants::ErrorType::InvalidPropertyName);
                return llvh::None;
              }
              return ClassAtom(codePointRanges, ec == 'P' /* invert */);
            } else {
              // When not in Unicode mode, this is just a regular `p` or `P`
              // (unnecessary) escape.
              return ClassAtom(consumeCharacterEscape());
            }
          }
#endif

          case 'b': {
            // "Return the CharSet containing the single character <BS>
            // U+0008 (BACKSPACE)"
            consume('b');
            return ClassAtom(0x08);
          }

          case '-':
            // ES6 21.2.1 ClassEscape: \- escapes -, in Unicode expressions
            // only.
            if ((curFlags_.unicode) && tryConsume('-')) {
              return ClassAtom('-');
            }
            [[fallthrough]];

          default: {
            return ClassAtom(consumeCharacterEscape());
          }
        }
      }

      default: {
        // Ordinary character or surrogate pair.
        if (auto cp = tryConsumeSurrogatePair()) {
          return ClassAtom(*cp);
        } else {
          return ClassAtom(consume(c));
        }
      }
    }
  }

  // ES6 B.1.2 LegacyOctalEscapeSequence
  // Note this is required by Annex B for regexp even in strict mode.
  CodePoint consumeLegacyOctalEscapeSequence() {
    // LegacyOctalEscapeSequence:
    //   OctalDigit [lookahead not OctalDigit]
    //   ZeroToThree OctalDigit [lookahead not OctalDigit]
    //   FourToSeven OctalDigit
    //   ZeroToThree OctalDigit OctalDigit
    // We implement this more directly.
    // Note this is forbidden in Unicode.
    if (curFlags_.unicode) {
      setError(constants::ErrorType::EscapeInvalid);
      return 0;
    }
    auto isOctalDigit = [](CharT c) { return '0' <= c && c <= '7'; };
    assert(
        current_ != end_ && isOctalDigit(*current_) &&
        "Should have leading octal digit");
    auto d1 = *current_++;
    auto d2 = consumeCharIf(isOctalDigit);
    auto d3 = (d1 <= '3' ? consumeCharIf(isOctalDigit) : llvh::None);

    char16_t result = d1 - '0';
    if (d2)
      result = result * 8 + (*d2 - '0');
    if (d3)
      result = result * 8 + (*d3 - '0');
    return result;
  }

  /// ES6 11.8.3 DecimalIntegerLiteral .
  /// If the value would overflow, uint32_t::max() is returned.
  /// All decimal digits are consumed regardless.
  CodePoint consumeDecimalIntegerLiteral() {
    auto decimalDigit = [](CharT c) { return '0' <= c && c <= '9'; };
    assert(
        current_ != end_ && decimalDigit(*current_) &&
        "Not a decimal integer literal");
    // Note that 'max' is a 64 bit value, but contains the max of u32.
    const uint64_t u32max = std::numeric_limits<uint32_t>::max();
    uint64_t bigResult = 0;
    while (auto digit = consumeCharIf(decimalDigit)) {
      bigResult = bigResult * 10 + (*digit - '0');
      if (bigResult > u32max) {
        bigResult = u32max;
      }
    }
    return static_cast<uint32_t>(bigResult);
  }

  /// ES6 11.8.3 DecimalIntegerLiteral .
  Optional<CodePoint> tryConsumeDecimalIntegerLiteral() {
    if (current_ != end_ && '0' <= *current_ && *current_ <= '9')
      return consumeDecimalIntegerLiteral();
    return llvh::None;
  }

  /// ES6 11.8.3 HexDigit .
  /// \return a uint derived from exactly \p count hex digits, or None.
  Optional<CodePoint> tryConsumeHexDigits(uint32_t count) {
    auto hexDigitValue = [](CharT c) -> Optional<uint32_t> {
      if ('0' <= c && c <= '9')
        return c - '0';
      if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
      if ('A' <= c && c <= 'F')
        return c - 'A' + 10;
      return llvh::None;
    };

    auto saved = current_;
    uint32_t result = 0;
    for (uint32_t i = 0; i < count; i++) {
      if (auto c = consumeCharIf(hexDigitValue)) {
        result = result * 16 + *hexDigitValue(*c);
      } else {
        current_ = saved;
        return llvh::None;
      }
    }
    return result;
  }

  /// ES6 21.2.2.10 CharacterEscape.
  /// Given that we have consumed a backslash and there is a following
  /// character, consume the next character as a CharacterEscape. \return the
  /// escaped character. Note this cannot fail as IdentityEscape is a fallback
  /// (e.g. /\q/ is the same as /q/). The caller must handle \c, whose meaning
  /// depends on context (see tryConsumeControlEscape).
  CodePoint consumeCharacterEscape() {
    if (current_ == end_) {
      setError(constants::ErrorType::EscapeIncomplete);
      return 0;
    }
    const CharT c = *current_;
    assert(c != 'c' && "\\c must be handled by tryConsumeControlEscape");
    switch (c) {
      case 'f':
        consume('f');
        return 0xC;
      case 'n':
        consume('n');
        return 0xA;
      case 'r':
        consume('r');
        return 0xD;
      case 't':
        consume('t');
        return 0x9;
      case 'v':
        consume('v');
        return 0xB;

      case '0': {
        // CharacterEscape :: "0 [lookahead != DecimalDigit]"
        auto saved = current_;
        consume('0');
        if (current_ == end_ || !('0' <= *current_ && *current_ <= '9')) {
          return '\0';
        } else {
          current_ = saved;
          return consumeLegacyOctalEscapeSequence();
        }
      }

      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7': {
        return consumeLegacyOctalEscapeSequence();
      }

      case 'u': {
        if (auto ret = tryConsumeUnicodeEscapeSequence()) {
          return *ret;
        } else {
          // IdentityEscape
          return identityEscape(consume(c));
        }
      }

      case 'x': {
        consume(c);
        if (auto ret = tryConsumeHexDigits(2)) {
          return *ret;
        } else {
          // Not followed by sufficient hex digits.
          // Note this is not an error; for example /\x1Z/ matches "x1Z" via
          // IdentityEscape.
          return identityEscape(c);
        }
      }

      default: {
        // IdentityEscape
        return identityEscape(consume(c));
      }
    }
  }

  /// ES6 21.2.1 IdentityEscape
  CodePoint identityEscape(CharT c) {
    // In Unicode regexps, only syntax characters and '/' may be escaped.
    if (curFlags_.unicode) {
      if (c == 0 || c > 127 || !strchr("^$\\.*+?()[]{}|/", c)) {
        setError(constants::ErrorType::EscapeInvalid);
      }
    }
    // TODO: disallow "UnicodeIDContinue".
    return c;
  }

  /// CharacterEscape :: c AsciiLetter, with the Annex B extensions that apply
  /// without Unicode. Here the backslash has been consumed and the cursor is
  /// on the 'c'. If \p inClass, a decimal digit or '_' following the 'c' also
  /// forms a control character (Annex B ClassEscape :: c ClassControlLetter).
  /// \return the control character, or None if the backslash is a literal
  /// (Annex B ExtendedAtom and ClassAtomNoDash :: \ [lookahead = c]). The 'c'
  /// is not consumed in that case, so it is parsed as the next atom.
  Optional<CodePoint> tryConsumeControlEscape(bool inClass) {
    assert(check('c') && "Cursor must be on the 'c' of a control escape");
    const bool unicode = curFlags_.unicode;
    auto isControlLetter = [inClass, unicode](CharT c) {
      return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
          (inClass && !unicode && (('0' <= c && c <= '9') || c == '_'));
    };
    auto next = std::next(current_);
    if (next != end_ && isControlLetter(*next)) {
      consume('c');
      return consume(*next) % 32;
    }
    if (unicode) {
      // Annex B does not apply, so \c must begin a valid control escape.
      setError(constants::ErrorType::EscapeInvalid);
    }
    return llvh::None;
  }

  /// ES6 21.2.2.10 RegExpUnicodeEscapeSequence
  Optional<CodePoint> tryConsumeUnicodeEscapeSequence(
      bool overrideUnicodeFlag = false) {
    auto saved = current_;
    if (!consume('u')) {
      return llvh::None;
    }

    // Non-unicode path only supports \uABCD style escapes.
    if (!overrideUnicodeFlag && !(curFlags_.unicode)) {
      if (auto ret = tryConsumeHexDigits(4)) {
        return *ret;
      }
      current_ = saved;
      return llvh::None;
    }

    // Unicode path.
    // Check for \u{ABCD123} style escapes.
    // Note that all \u in Unicode regexps must result in a valid escape; there
    // is no fallback to IdentityEscape. This means we must not return None.
    if (tryConsume('{')) {
      uint32_t result = 0;
      size_t digitCount = 0;
      while (auto digit = tryConsumeHexDigits(1)) {
        digitCount++;
        result = result * 16 + *digit;
        // 21.2.1.1: It is a Syntax Error if the MV of HexDigits > 1114111
        if (result > 1114111) {
          setError(constants::ErrorType::EscapeOverflow);
          return 0;
        }
      }
      if (!tryConsume('}')) {
        setError(constants::ErrorType::EscapeInvalid);
        return 0;
      }
      if (digitCount == 0) {
        // input was like \u{}
        setError(constants::ErrorType::EscapeInvalid);
        return 0;
      }
      return result;
    }

    // Check for \uABCD style escapes.
    if (auto hi = tryConsumeHexDigits(4)) {
      if (isHighSurrogate(*hi)) {
        // This is a leading surrogate.
        // Look for a trailing surrogate.
        auto saved2 = current_;
        if (tryConsume("\\u")) {
          if (auto lo = tryConsumeHexDigits(4)) {
            if (isLowSurrogate(*lo)) {
              return utf16SurrogatePairToCodePoint(*hi, *lo);
            }
          }
        }
        // No trailing surrogate.
        current_ = saved2;
      }
      return *hi;
    }

    // All \u failed escapes in Unicode regexps are an error.
    setError(constants::ErrorType::EscapeInvalid);
    return 0;
  }

  bool tryConsumeUnicodeProperty(
      std::string &propertyName,
      std::string &propertyValue) {
    if (current_ == end_) {
      return false;
    }

    while (auto ch = consumeCharIf(isUnicodePropertyName)) {
      propertyName.push_back(*ch);
    }
    if (tryConsume('=')) {
      while (auto ch = consumeCharIf(isUnicodePropertyValue)) {
        propertyValue.push_back(*ch);
      }
      return !propertyValue.empty();
    }

    return true;
  }

  /// ES2018 22.2.1 UnicodePropertyValueExpression
  /// Here the `\p{` or `\P{` has been consumed.
  /// \return true if the expression was successfully parsed, \p propertyName
  /// and \p propertyValue will contain the parsed values.
  bool tryConsumeUnicodePropertyValueExpression(
      std::string &propertyName,
      std::string &propertyValue) {
    assert(
        curFlags_.unicode &&
        "Non-unicode regexps do not support property escapes");

    // Unicode path.
    // Note that all \p and \P in Unicode regexps must result in a valid escape.
    if (!tryConsumeUnicodeProperty(propertyName, propertyValue)) {
      setError(constants::ErrorType::InvalidPropertyName);
      return false;
    }

    if (!tryConsume('}')) {
      setError(constants::ErrorType::InvalidPropertyName);
      return false;
    }
    return true;
  }

  /// 21.2.2.9 AtomEscape.
  /// Here the backslash has been consumed.
  void consumeAtomEscape() {
    if (current_ == end_) {
      setError(constants::ErrorType::EscapeIncomplete);
      return;
    }

    CharT c = *current_;
    switch (c) {
      case 'd':
      case 'D':
        consume(c);
        re_->pushCharClass(
            {CharacterClass::Digits, c == 'D' /* invert */},
            curFlags_.ignoreCase);
        break;

      case 's':
      case 'S':
        consume(c);
        re_->pushCharClass(
            {CharacterClass::Spaces, c == 'S' /* invert */},
            curFlags_.ignoreCase);
        break;

      case 'w':
      case 'W':
        consume(c);
        re_->pushCharClass(
            {CharacterClass::Words, c == 'W' /* invert */},
            curFlags_.ignoreCase);
        break;

#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES
      case 'p':
      case 'P': {
        if (curFlags_.unicode) {
          consume(c);
          std::string propertyName;
          std::string propertyValue;
          if (!tryConsume('{') ||
              !tryConsumeUnicodePropertyValueExpression(
                  propertyName, propertyValue)) {
            setError(constants::ErrorType::InvalidPropertyName);
            return;
          }
          auto bracket = re_->startBracketList(false, curFlags_.ignoreCase);
          auto codePointRanges =
              unicodePropertyRanges(propertyName, propertyValue);
          if (codePointRanges.empty()) {
            setError(constants::ErrorType::InvalidPropertyName);
            return;
          }
          bracket->addCodePointRanges(codePointRanges, c == 'P');
          break;
        } else {
          // When not in Unicode mode, this is just a regular `p` or `P`
          // (unnecessary) escape.
          re_->pushChar(consumeCharacterEscape(), curFlags_.ignoreCase);
        }
        break;
      }
#endif

        // Note backreferences may NOT begin with 0.
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9': {
        // In Unicode mode, this is always a backreference.
        // In non-unicode mode, this is a backreference if its value does not
        // exceed the number of capture groups. Otherwise it is an octal escape
        // if its value is octal. Otherwise it is IdentityEscape.
        auto saved = current_;
        uint32_t decimal = consumeDecimalIntegerLiteral();
        bool unicode = curFlags_.unicode;
        if (unicode || decimal <= backRefLimit_) {
          // Backreference.
          maxBackRef_ = std::max(maxBackRef_, decimal);
          // Subtract 1 so the marked subexpression index starts at zero, to
          // line up with other instructions.
          re_->pushBackRef(decimal - 1, curFlags_.ignoreCase);
        } else if (c < '8' && !unicode) {
          // Octal.
          current_ = saved;
          re_->pushChar(
              consumeLegacyOctalEscapeSequence(), curFlags_.ignoreCase);
        } else {
          // IdentityEscape.
          current_ = saved;
          re_->pushChar(identityEscape(consume(c)), curFlags_.ignoreCase);
        }
        break;
      }

      case 'c':
        re_->pushChar(
            tryConsumeControlEscape(false /* inClass */).getValueOr('\\'),
            curFlags_.ignoreCase);
        break;

      case 'k': {
        if (curFlags_.unicode || hasNamedGroups_) {
          consume('k');
          GroupName refIdentifer;
          if (!tryConsume('<') || !tryConsumeGroupName(refIdentifer)) {
            setError(constants::ErrorType::InvalidNamedReference);
            return;
          }
          re_->pushNamedBackRef(std::move(refIdentifer), curFlags_.ignoreCase);
          break;
        }
        re_->sawNamedBackrefBeforeGroup();
        // If we are not in unicode and we don't have named groups, then fall
        // through and we will consume the k as an identity escape.
        [[fallthrough]];
      }
      default: {
        re_->pushChar(consumeCharacterEscape(), curFlags_.ignoreCase);
        break;
      }
    }
  }

 public:
  /// Constructor from a regular expression \p re and regexp input string
  /// range \p start to \p end. Escapes like \123 are considered to be
  /// backrefs if they do not exceed backRefLimit.
  Parser(
      RegexType *re,
      ForwardIterator start,
      ForwardIterator end,
      SyntaxFlags flags,
      uint32_t backRefLimit,
      bool hasNamedGroups)
      : re_(re),
        current_(start),
        end_(end),
        curFlags_(flags),
        backRefLimit_(backRefLimit),
        hasNamedGroups_(hasNamedGroups) {}

  constants::ErrorType performParse() {
    consumeDisjunction();
    assert(
        current_ == end_ && "We should always consume all input even on error");
    return error_;
  }

  /// \return the maximum number of backreferences encountered
  uint32_t maxBackRef() const {
    return maxBackRef_;
  }
};

template <typename Receiver>
constants::ErrorType parseRegex(
    const char16_t *start,
    const char16_t *end,
    Receiver *receiver,
    SyntaxFlags flags,
    uint32_t backRefLimit,
    bool hasNamedGroups,
    uint32_t *outMaxBackRef) {
  Parser<Receiver, const char16_t *> parser(
      receiver, start, end, flags, backRefLimit, hasNamedGroups);
  auto result = parser.performParse();
  *outMaxBackRef = parser.maxBackRef();
  return result;
}

// Explicitly instantiate the parser for UTF16RegexTraits only.
template constants::ErrorType parseRegex(
    const char16_t *start,
    const char16_t *end,
    Regex<UTF16RegexTraits> *receiver,
    SyntaxFlags flags,
    uint32_t backRefLimit,
    bool hasNamedGroups,
    uint32_t *outMaxBackRef);

} // namespace regex
} // namespace hermes
