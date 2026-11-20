/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- C++ -*-
//===--------------------------- regex ------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef HERMES_REGEX_COMPILER_H
#define HERMES_REGEX_COMPILER_H

#include "hermes/Regex/RegexBytecode.h"
#include "hermes/Regex/RegexNode.h"
#include "hermes/Regex/RegexSupport.h"
#include "hermes/Regex/RegexTypes.h"
#include "hermes/Support/Algorithms.h"
#include "hermes/Support/Compiler.h"

#include "llvh/ADT/Optional.h"

#include <deque>
#include <string>
#include <vector>

namespace hermes {
namespace regex {

template <class Traits>
class Regex {
  // Enable the Parser to add nodes to us.
  template <class A, class B>
  friend class Parser;

  using CharT = typename Traits::CodeUnit;
  using CodePoint = typename Traits::CodePoint;
  using Node = regex::Node;
  using BracketNode = regex::BracketNode<Traits>;

 private:
  Traits traits_;

  /// This field is the set of flags that were used at the top-level definition
  /// of the regex. However, the i/m/s flags can be modified locally within the
  /// regex. So it's not correct to read those flags from this field when
  /// constructing the individual nodes that need access to those fields.
  /// Instead, that state is maintained by the parser, and will be passed in to
  /// the constructor of the nodes that need it.
  SyntaxFlags globalFlags_ = {};

  // Number of capture groups encountered so far.
  uint16_t markedCount_ = 0;

  // Number of loops encountered so far.
  uint32_t loopCount_ = 0;

  // The list of nodes so far.
  NodeList nodes_;

  // List of all unique_ptrs to all nodes created by this Regex, used to defer
  // destructors and avoid a stack overflow.
  NodeHolder nodeHolder_;

  // The error, which may be set after parsing.
  constants::ErrorType error_ = constants::ErrorType::None;

  // Constraints on the type of strings that can match this regex.
  MatchConstraintSet matchConstraints_ = 0;

  /// The code units that can begin a match, or None if there is no start
  /// filter: the regex can match the empty string, or the analysis gave up.
  llvh::Optional<StartSet> startSet_;

  // This holds the named capture groups in the order they were defined. Each
  // distinct name appears exactly once, even if it is used by several capture
  // groups.
  std::deque<llvh::SmallVector<char16_t, 5>> orderedGroupNames_{};

  ParsedGroupNamesMapping nameMapping_{};

  // We can skip double parsing if there were no named backreferences used
  // before the definition of the first named capture group.
  bool sawNamedBackrefBeforeGroup_{false};

  // Named backrefs, which are resolved once the whole pattern has been parsed.
  llvh::SmallVector<std::pair<GroupName, BackRefNode *>, 2>
      unresolvedNamedBackRefs_;

  /// Construct and and append a node of type NodeType at the end of the nodes_
  /// list. The node should be constructible from \p args.
  /// \return an observer pointer to the new node.
  template <typename NodeType, typename... Args>
  NodeType *appendNode(Args &&...args) {
    std::unique_ptr<NodeType> node =
        std::make_unique<NodeType>(std::forward<Args>(args)...);
    NodeType *nodePtr = node.get();
    nodeHolder_.push_back(std::move(node));
    nodes_.push_back(nodePtr);
    return nodePtr;
  }

  /// \return the "current" node, which is the last (rightmost) node created.
  Node *currentNode() {
    return nodes_.back();
  }

  /// \return the number of marked subexpressions.
  uint16_t markedCount() const {
    return markedCount_;
  }

  /// Increment the number of marked subexpressions.
  /// \return the previous value of markedCount_.
  uint16_t incrementMarkedCount() {
    assert(
        markedCount_ < std::numeric_limits<uint16_t>::max() &&
        "markedCount_ will overflow");
    return markedCount_++;
  }

  /// Given that the node \p splicePoint is in our node list, remove all nodes
  /// after it. \return a list of the removed nodes.
  NodeList spliceOut(Node *splicePoint) {
    assert(splicePoint && "null node in spliceOut");
    // Find the index of the splice point. We expect it to be towards the end.
    size_t spliceIndex = nodes_.size();
    while (spliceIndex--) {
      if (nodes_[spliceIndex] == splicePoint)
        break;
    }
    assert(spliceIndex < nodes_.size() && "Node not in node list");
    // Move all nodes after the splice index into a new vector.
    // Note this may be empty.
    auto firstToMove = nodes_.begin() + spliceIndex + 1;
    NodeList result;
    std::move(firstToMove, nodes_.end(), std::back_inserter(result));
    nodes_.erase(firstToMove, nodes_.end());
    return result;
  }

 public:
  /// Compile the regex into bytecode. Return the resulting bytecode.
  std::vector<uint8_t> compile() const {
    assert(valid() && "Cannot compile invalid regex.");
    // TODO: add validation for the loop and reduce the size of loopCount_ to
    // uint16_t.
    assert(
        markedCount_ <= constants::kMaxCaptureGroupCount &&
        "Too many capture groups");
    assert(loopCount_ <= constants::kMaxLoopCount && "Too many loops");
    RegexBytecodeHeader header{};
    header.markedCount = markedCount_;
    header.loopCount = static_cast<uint16_t>(loopCount_);
    header.syntaxFlags = globalFlags_.toByte();
    header.constraints = matchConstraints_;
    if (startSet_)
      startSet_->writeTo(header);
    RegexBytecodeStream bcs(header);
    Node::compile(nodes_, bcs);
    return bcs.acquireBytecode();
  }

  // Constructors
  explicit Regex(const CharT *p, const char16_t *f = u"")
      : Regex(
            {p, p + std::char_traits<CharT>::length(p)},
            {f, f + std::char_traits<CharT>::length(f)}) {}

  Regex(
      const llvh::ArrayRef<CharT> pattern,
      const llvh::ArrayRef<char16_t> flags = {}) {
    // Compute the SyntaxFlags based on the flags string.
    auto sflags = SyntaxFlags::fromString(flags);
    if (!sflags) {
      error_ = constants::ErrorType::InvalidFlags;
      return;
    }
    globalFlags_ = *sflags;
    error_ = parse(pattern.begin(), pattern.end());
  }

  // Disallow copy-assignment and copy-construction.
  Regex &operator=(const Regex &) = delete;
  Regex(const Regex &) = delete;

  /// Move-assignment and move-construction.
  Regex &operator=(Regex &&) = default;
  Regex(Regex &&) = default;

  // Accessors.
  SyntaxFlags globalFlags() const {
    return globalFlags_;
  }

  std::deque<llvh::SmallVector<char16_t, 5>> &getOrderedNamedGroups() {
    return orderedGroupNames_;
  }

  std::deque<llvh::SmallVector<char16_t, 5>> acquireOrderedGroupNames() {
    return std::move(orderedGroupNames_);
  }

  ParsedGroupNamesMapping &getGroupNamesMapping() {
    return nameMapping_;
  }

  ParsedGroupNamesMapping acquireGroupNamesMapping() {
    return std::move(nameMapping_);
  }

  void sawNamedBackrefBeforeGroup() {
    sawNamedBackrefBeforeGroup_ = true;
  }

  /// \return any errors produced during parsing, or ErrorType::None if none.
  constants::ErrorType getError() const {
    return error_;
  }

  /// \return whether the regex was parsed successfully.
  bool valid() const {
    return error_ == constants::ErrorType::None;
  }

  /// \return the set of match constraints for the regex.
  MatchConstraintSet matchConstraints() const {
    return matchConstraints_;
  }

 private:
  template <class ForwardIterator>
  constants::ErrorType parse(ForwardIterator first, ForwardIterator last);

  /// Attempt to parse the regex from the range [\p first, \p last), using
  /// \p backRefLimit as the maximum decimal escape to interpret as a
  /// backreference.  The maximum backreference that was in fact encountered
  /// is returned by reference in \p out_max_back_ref, if that is larger than
  /// its current value. \return an error code.
  template <class ForwardIterator>
  constants::ErrorType parseWithBackRefLimit(
      ForwardIterator first,
      ForwardIterator last,
      uint32_t backRefLimit,
      bool hasNamedGroups,
      uint32_t *outMaxBackRef);

  // Note- this method does not insert into the AST, it populates a different
  // datastructure. \p identifier may already be in use, which is only legal if
  // the two groups can never both participate in a match; that is the parser's
  // job to check.
  // \param mexp the zero-based mexp of the capture group being named.
  // \return the mexp of the previous capture group using \p identifier, or
  //   None if this is the first use of the name.
  llvh::Optional<uint32_t> addNamedCaptureGroup(
      GroupName &&identifier,
      uint32_t mexp);

  bool resolveNamedBackRefs();

  void pushLeftAnchor(bool multiline);
  void pushRightAnchor(bool multiline);
  void pushMatchAny(bool dotAll);
  void pushLoop(
      uint32_t min,
      uint32_t max,
      NodeList loopedList,
      uint32_t mexp_begin,
      bool greedy);
  BracketNode *startBracketList(bool negate, bool icase);
  void pushChar(CodePoint c, bool icase);
  void pushCharClass(CharacterClass c, bool icase);
  void pushBackRef(uint32_t i, bool icase);
  void pushNamedBackRef(GroupName &&identifier, bool icase);
  void pushAlternation(std::vector<NodeList> alternatives);
  void pushMarkedSubexpression(NodeList, uint32_t mexp);
  void pushWordBoundary(bool invert, bool icase);
  void pushLookaround(NodeList, uint16_t, uint16_t, bool, bool);
};

template <typename Receiver>
constants::ErrorType parseRegex(
    const char16_t *start,
    const char16_t *end,
    Receiver *receiver,
    SyntaxFlags flags,
    uint32_t backRefLimit,
    bool hasNamedGroups,
    uint32_t *outMaxBackRef);

template <class Traits>
template <class ForwardIterator>
constants::ErrorType Regex<Traits>::parse(
    ForwardIterator first,
    ForwardIterator last) {
  uint32_t maxBackRef = 0;
  bool hasNamedGroups = globalFlags_.unicode;
  auto result = parseWithBackRefLimit(
      first,
      last,
      constants::kMaxCaptureGroupCount,
      hasNamedGroups,
      &maxBackRef);

  // Validate loop and capture group count.
  if (loopCount_ > constants::kMaxLoopCount) {
    return constants::ErrorType::PatternExceedsParseLimits;
  }

  // See comment --DecimalEscape--
  // We parsed without a backreference limit because we had to parse to discover
  // the limit. Now we know that we wrongly interpreted a decimal escape as a
  // backreference. See ES6 Annex B.1.4 DecimalEscape "but only if the integer
  // value DecimalEscape is <= NCapturingParens". Now that we know the true
  // capture group count, either produce an error (if Unicode) or re-parse with
  // that as the limit so overlarge decimal escapes will be ignored.
  bool reparseForNumberedBackref =
      result == constants::ErrorType::None && maxBackRef > markedCount_;
  // We must also reparse if there were any named capture groups used and it is
  // not unicode mode.
  bool reparseForNamedBackref = false;
  if (!globalFlags_.unicode && nameMapping_.size() > 0 &&
      sawNamedBackrefBeforeGroup_) {
    reparseForNamedBackref = true;
    hasNamedGroups = true;
  }

  if (reparseForNumberedBackref || reparseForNamedBackref) {
    if (globalFlags_.unicode) {
      return constants::ErrorType::EscapeInvalid;
    }

    uint32_t backRefLimit = markedCount_;
    uint32_t reparsedMaxBackRef = 0;
    result = parseWithBackRefLimit(
        first, last, backRefLimit, hasNamedGroups, &reparsedMaxBackRef);
    assert(
        reparsedMaxBackRef <= backRefLimit &&
        "invalid backreference generated");
    (void)reparsedMaxBackRef;
  }
  return result;
}

template <class Traits>
template <class ForwardIterator>
constants::ErrorType Regex<Traits>::parseWithBackRefLimit(
    ForwardIterator first,
    ForwardIterator last,
    uint32_t backRefLimit,
    bool hasNamedGroups,
    uint32_t *outMaxBackRef) {
  // Discard everything a previous pass produced; parse() may run two.
  loopCount_ = 0;
  markedCount_ = 0;
  matchConstraints_ = 0;
  nameMapping_.clear();
  orderedGroupNames_.clear();
  unresolvedNamedBackRefs_.clear();
  // Initialize our node list with a single no-op node (it must never be empty.)
  nodes_.clear();
  startSet_ = llvh::None;
  appendNode<EmptyNode>();
  auto result = parseRegex(
      first,
      last,
      this,
      globalFlags_,
      backRefLimit,
      hasNamedGroups,
      outMaxBackRef);

  // If we succeeded, add a goal node as the last node and perform optimizations
  // on the list.
  if (result == constants::ErrorType::None) {
    appendNode<GoalNode>();
    Node::optimizeNodeList(nodes_, globalFlags_, nodeHolder_);
    if (!resolveNamedBackRefs()) {
      return constants::ErrorType::NonexistentNamedCaptureReference;
    }
  }

  // Compute any match constraints.
  matchConstraints_ = Node::matchConstraintsForList(nodes_);

  // Compute the start-position filter.
  if (result == constants::ErrorType::None) {
    StartSet set;
    if (Node::addStartSetForList(nodes_, set, 0) == StartSetResult::Consumes)
      startSet_ = set;
  }

  return result;
}

template <class Traits>
bool Regex<Traits>::resolveNamedBackRefs() {
  for (auto &[name, backRef] : unresolvedNamedBackRefs_) {
    auto search = nameMapping_.find(name);
    if (search == nameMapping_.end()) {
      return false;
    }
    backRef->setBackRefs(search->second);
  }
  return true;
}

template <class Traits>
void Regex<Traits>::pushLoop(
    uint32_t min,
    uint32_t max,
    NodeList loopedExpr,
    uint32_t mexp_begin,
    bool greedy) {
  appendNode<LoopNode>(
      loopCount_++,
      min,
      max,
      greedy,
      mexp_begin,
      markedCount_,
      std::move(loopedExpr));
}

template <class Traits>
void Regex<Traits>::pushChar(CodePoint c, bool icase) {
  bool unicode = globalFlags_.unicode;
  if (icase)
    c = traits_.canonicalize(c, unicode);
  appendNode<MatchCharNode>(Node::CodePointList{c}, icase, unicode);
}

template <class Traits>
void Regex<Traits>::pushCharClass(CharacterClass c, bool icase) {
  auto bracket = startBracketList(false, icase);
  bracket->addClass(c);
}

template <class Traits>
void Regex<Traits>::pushMarkedSubexpression(NodeList nodes, uint32_t mexp) {
  appendNode<MarkedSubexpressionNode>(std::move(nodes), mexp);
}

template <class Traits>
void Regex<Traits>::pushLeftAnchor(bool multiline) {
  appendNode<LeftAnchorNode>(multiline);
}

template <class Traits>
void Regex<Traits>::pushRightAnchor(bool multiline) {
  appendNode<RightAnchorNode>(multiline);
}

template <class Traits>
void Regex<Traits>::pushMatchAny(bool dotAll) {
  bool unicode = globalFlags_.unicode;
  appendNode<MatchAnyNode>(dotAll, unicode);
}

template <class Traits>
void Regex<Traits>::pushWordBoundary(bool invert, bool icase) {
  appendNode<WordBoundaryNode>(invert, icase);
}

template <class Traits>
void Regex<Traits>::pushBackRef(uint32_t i, bool icase) {
  appendNode<BackRefNode>(i, icase);
}

template <class Traits>
void Regex<Traits>::pushNamedBackRef(GroupName &&identifier, bool icase) {
  // A named backreference refers to every capture group using that name,
  // including any not parsed yet, and the name may turn out never to be
  // defined at all, so resolve it once the whole pattern has been parsed.
  BackRefNode *backRef = appendNode<BackRefNode>(0, icase);
  unresolvedNamedBackRefs_.emplace_back(std::move(identifier), backRef);
}

template <class Traits>
void Regex<Traits>::pushAlternation(std::vector<NodeList> alternatives) {
  appendNode<AlternationNode>(std::move(alternatives));
}

template <class Traits>
BracketNode<Traits> *Regex<Traits>::startBracketList(bool negate, bool icase) {
  bool unicode = globalFlags_.unicode;
  return appendNode<BracketNode>(traits_, negate, icase, unicode);
}

template <class Traits>
void Regex<Traits>::pushLookaround(
    NodeList exp,
    uint16_t mexpBegin,
    uint16_t mexpEnd,
    bool invert,
    bool forwards) {
  if (!forwards) {
    Node::reverseNodeList(exp);
  }
  nodeHolder_.push_back(std::make_unique<GoalNode>());
  exp.push_back(nodeHolder_.back().get());
  appendNode<LookaroundNode>(
      std::move(exp), mexpBegin, mexpEnd, invert, forwards);
}

template <class Traits>
llvh::Optional<uint32_t> Regex<Traits>::addNamedCaptureGroup(
    GroupName &&identifier,
    uint32_t mexp) {
  uint32_t matchIndex = mexp + 1;
  auto existing = nameMapping_.find(identifier);
  if (LLVM_UNLIKELY(existing != nameMapping_.end())) {
    // Keep one entry per distinct name, so orderedGroupNames_ stays in
    // first-appearance order and without repeats.
    uint32_t prevMexp = existing->second.back() - 1;
    existing->second.push_back(matchIndex);
    return prevMexp;
  }
  // The name is interned in orderedGroupNames_, a deque, so the map key stays
  // valid as more names are added.
  auto &elm = orderedGroupNames_.emplace_back(std::move(identifier));
  nameMapping_.try_emplace(elm, GroupNums{matchIndex});
  return llvh::None;
}

} // namespace regex
} // namespace hermes

#endif // HERMES_REGEX_COMPILER_H
