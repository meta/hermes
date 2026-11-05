/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Regex/Executor.h"
#include "hermes/Regex/RegexTraits.h"
#include "hermes/Support/OptValue.h"

#include "llvh/ADT/ScopeExit.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/Support/TrailingObjects.h"
#include "llvh/Support/raw_ostream.h"

#include <cstring>
#include <type_traits>

// This file contains the machinery for executing a regexp compiled to bytecode.

namespace hermes {
namespace regex {

template <class Traits>
struct State;

/// Describes the exit status of a RegEx execution: it either returned
/// normally or stack overflowed
enum class ExecutionStatus : uint8_t { RETURNED, STACK_OVERFLOW };

/// A tuple combining the result of a function which may have returned
/// successfully (ExecutionStatus::RETURNED) with a value, or thrown an
/// exception (ExecutionStatus::STACK_OVERFLOW).
/// This is used by some internal functions for convenience.
template <typename T>
class ExecutorResult {
  static_assert(std::is_trivial<T>::value, "T must be trivial.");

 private:
  ExecutionStatus status_;
  T value_;

 public:
  /* implicit */ ExecutorResult(const T &v)
      : status_(ExecutionStatus::RETURNED), value_(v) {}

  /* implicit */ ExecutorResult(ExecutionStatus status) : status_(status) {
    assert(status != ExecutionStatus::RETURNED);
  }

  const T &operator*() const {
    return getValue();
  }

  bool hasValue() const {
    return status_ == ExecutionStatus::RETURNED;
  }

  explicit operator bool() const {
    return hasValue();
  }

  const T &getValue() const {
    assert(getStatus() == ExecutionStatus::RETURNED);
    return *reinterpret_cast<const T *>(&value_);
  }

  ExecutionStatus getStatus() const {
    return status_;
  }
};
/// An enum describing Width1 opcodes. This is the set of regex opcodes which
/// always match exactly one character (or fail). This is broken out from Opcode
/// to get exhaustiveness checking in switch statements. Note that conversions
/// can be performed via static_cast.
enum class Width1Opcode : uint8_t {
  MatchChar8 = (uint8_t)Opcode::MatchChar8,
  MatchChar16 = (uint8_t)Opcode::MatchChar16,
  MatchCharICase8 = (uint8_t)Opcode::MatchCharICase8,
  MatchCharICase16 = (uint8_t)Opcode::MatchCharICase16,
  MatchAny = (uint8_t)Opcode::MatchAny,
  MatchAnyButNewline = (uint8_t)Opcode::MatchAnyButNewline,
  Bracket = (uint8_t)Opcode::Bracket,
  BracketICase = (uint8_t)Opcode::BracketICase,
};

/// LoopData tracks information about a loop during a match attempt. Each State
/// has one LoopData per loop.
struct LoopData {
  /// The number of times that this loop has executed in this state.
  uint32_t iterations;

  /// The input position where we entered the loop.
  uint32_t entryPosition;
};

/// Cursor is a lightweight value type which allows tracking a character pointer
/// 'current' within a range 'first' to 'last'.
/// A cursor may either be forwards, in which case it proceeds from 'first' to
/// 'last'. It may also (in the case of lookbehind assertions) be backwards, in
/// which case the cursor proceeds from 'last' to 'first'. The terms "begin" and
/// "end" denote tracking in the direction of the cursor, while "left" and
/// "right" are direction independent.
template <class Traits>
class Cursor {
  using CodeUnit = typename Traits::CodeUnit;
  using CodePoint = typename Traits::CodePoint;

 public:
  /// Construct with the range \p first and \p last, setting the current
  /// position to \p first. Note that the \p last is one past the last valid
  /// character. \p forwards decides whether the current pointer advances
  /// towards last_ (true) or first_ (false).
  Cursor(
      const CodeUnit *first,
      const CodeUnit *current,
      const CodeUnit *last,
      bool forwards)
      : first_(first),
        last_(last),
        current_(current),
        end_(forwards ? last : first),
        forwards_(forwards) {
    assert(first_ <= last_ && "first and last out of order");
    assert(
        first_ <= current_ && current <= last_ &&
        "current pointer not in range");
  }

  /// \return whether this cursor advances forwards.
  bool forwards() const {
    return forwards_;
  }

  /// Set whether this cursor advances forwards to \p flag.
  void setForwards(bool flag) {
    forwards_ = flag;
    end_ = forwards_ ? last_ : first_;
  }

  /// \return the number of code units remaining.
  uint32_t remaining() const {
    return forwards_ ? last_ - current_ : current_ - first_;
  }

  /// \return whether we are at the end of the range.
  bool atEnd() const {
    return current_ == end_;
  }

  /// \return the number of code units consumed from the leftmost character.
  /// This is called "offsetFromLeft" and not "offsetFromStart" to indicate that
  /// it does not change under backwards tracking.
  uint32_t offsetFromLeft() const {
    return current_ - first_;
  }

  /// \return the number of code units between the current position and the end
  /// of the string.
  /// This is called "offsetFromRight" and not "offsetFromEnd" to indicate that
  /// it does not change under backwards tracking.
  uint32_t offsetFromRight() const {
    return last_ - current_;
  }

  /// \return whether we are at the leftmost position.
  /// This does not change under backwards tracking.
  bool atLeft() const {
    return current_ == first_;
  }

  /// \return whether we are at the rightmost position.
  /// This does not change under backwards tracking.
  bool atRight() const {
    return current_ == last_;
  }

  /// \return the current code unit.
  CodeUnit current() const {
    // Access the character at index 0 if forwards, -1 if backwards.
    assert(!atEnd() && "Cursor is at end");
    return current_[(int)forwards_ - 1];
  }

  /// \return the current cursor position.
  const CodeUnit *currentPointer() const {
    return current_;
  }

  /// Set the current cursor position to \p current.
  void setCurrentPointer(const CodeUnit *current) {
    assert(first_ <= current && current <= last_ && "Current not in range");
    current_ = current;
  }

  /// \return the current code unit, advancing the cursor by 1.
  CodeUnit consume() {
    CodeUnit result = current();
    current_ += forwards_ ? 1 : -1;
    return result;
  }

  /// \return a code point decoded from the code units under the cursor,
  /// possibly by decoding surrogates. Advances the cursor by the number of code
  /// units consumed.
  CodePoint consumeUTF16() {
    assert(!atEnd() && "At end");

    // In ASCII we have no surrogates.
    if (sizeof(CodeUnit) >= 2 && remaining() >= 2) {
      CodeUnit hi = forwards_ ? current_[0] : current_[-2];
      CodeUnit lo = forwards_ ? current_[1] : current_[-1];
      if (isHighSurrogate(hi) && isLowSurrogate(lo)) {
        current_ += forwards_ ? 2 : -2;
        return utf16SurrogatePairToCodePoint(hi, lo);
      }
    }
    return consume();
  }

  /// \return whether a regex match performed using the given \p flags can
  /// possibly match the given \p constraints.
  bool satisfiesConstraints(
      constants::MatchFlagType flags,
      MatchConstraintSet constraints) const {
    if ((constraints & MatchConstraintNonASCII) &&
        (flags & constants::matchInputAllAscii))
      return false;
    if ((constraints & MatchConstraintAnchoredAtStart) && current_ != first_)
      return false;
    return true;
  }

 private:
  // The first code unit in the string.
  const CodeUnit *first_;

  // One past the last code unit in the string.
  const CodeUnit *last_;

  // Our position between first_ and last_.
  // If we are forwards, then the current character is current_[0].
  // If we are backwards, then the current character is current_[-1].
  const CodeUnit *current_;

  // A pointer to the end. This is either last (if forwards) or first (if not
  // forwards). If our current cursor reaches this value, we are done.
  const CodeUnit *end_;

  // Whether we are tracking forwards or backwards.
  bool forwards_;
};

/// A Context records global information about a match attempt.
template <class Traits>
struct Context {
  using CodeUnit = typename Traits::CodeUnit;
  using CodePoint = typename Traits::CodePoint;

  /// The set of backtracking opcodes. These are interpreted by the backtrack()
  /// function.
  enum class BacktrackOp : uint8_t {
    /// Set the value of a capture group to a stored value.
    SetCaptureGroup,

    /// Set the value of a loop data to a stored value.
    SetLoopData,

    /// Set the IP and position in the input string to a stored value.
    SetPosition,

    /// Backtrack by entering the body of a non-greedy loop.
    EnterNonGreedyLoop,

    /// Backtrack a greedy loop whose body matches exactly one character, such
    /// as /.*/.
    GreedyWidth1Loop,

    /// Backtrack a nongreedy loop whose body matches exactly one character,
    /// such as /.*?/.
    NongreedyWidth1Loop,
  };

  /// An instruction describing how to backtrack.
  union BacktrackInsn {
    /// The operation to perform.
    BacktrackOp op;

    /// List of instruction-specific fields. Note that the opcode is reproduced
    /// in every struct; this avoids padding between the opcode and the
    /// following field.

    /// Fields used by setCaptureGroup instruction.
    struct {
      BacktrackOp op;
      uint16_t mexp; /// Which capture group to set.
      CapturedRange range; /// Value to set.
    } setCaptureGroup;

    /// Fields used by SetLoopData instruction.
    struct {
      BacktrackOp op;
      uint16_t loopId; /// Which loop to set.
      LoopData loopData; /// Value to set.
    } setLoopData;

    /// Fields used by SetPosition instruction.
    struct {
      BacktrackOp op;
      uint32_t ip; /// Instruction pointer to set.
      const CodeUnit *value; /// Input string position to set.
    } setPosition;

    /// Fields used by EnterNonGreedyLoop instruction.
    struct {
      BacktrackOp op;
      uint32_t bodyIp; /// The IP of the loop body.
      LoopData loopData; /// Data for the loop to set.
      const BeginLoopInsn *loopInsn; /// The loop instruction.
    } enterNonGreedyLoop;

    /// Fields used by GreedyWidth1Loop and NongreedyWidth1Loop.
    struct {
      BacktrackOp op; /// The opcode.
      uint32_t continuation; /// The ip for the not-taken branch of the loop.
      const CodeUnit *min; /// The minimum possible match position.
      const CodeUnit *max; /// The maximum possible match position.
    } width1Loop;

    /* implicit */ BacktrackInsn(BacktrackOp op) : op(op) {}

    /// \return a SetCaptureGroup instruction.
    static BacktrackInsn makeSetCaptureGroup(
        uint16_t mexp,
        CapturedRange range) {
      BacktrackInsn result{BacktrackOp::SetCaptureGroup};
      result.setCaptureGroup.mexp = mexp;
      result.setCaptureGroup.range = range;
      return result;
    }

    /// \return a SetLoopData instruction.
    static BacktrackInsn makeSetLoopData(uint16_t loopId, LoopData loopData) {
      BacktrackInsn result{BacktrackOp::SetLoopData};
      result.setLoopData.loopId = loopId;
      result.setLoopData.loopData = loopData;
      return result;
    }

    /// \return a SetPosition instruction.
    static BacktrackInsn makeSetPosition(
        uint32_t ip,
        const CodeUnit *inputPos) {
      BacktrackInsn result = BacktrackOp::SetPosition;
      result.setPosition.ip = ip;
      result.setPosition.value = inputPos;
      return result;
    }

    /// \return an EnterNonGreedyLoop instruction.
    static BacktrackInsn makeEnterNonGreedyLoop(
        const BeginLoopInsn *loopInsn,
        uint32_t bodyIp,
        LoopData loopData) {
      BacktrackInsn result = BacktrackOp::EnterNonGreedyLoop;
      result.enterNonGreedyLoop.bodyIp = bodyIp;
      result.enterNonGreedyLoop.loopInsn = loopInsn;
      result.enterNonGreedyLoop.loopData = loopData;
      return result;
    }
  };

  /// Our stack of backtrack instructions.
  using BacktrackStack = llvh::SmallVector<BacktrackInsn, 64>;

  /// The maximum depth of our backtracking stack. Beyond this we return a stack
  /// overflow error.
  static constexpr size_t kMaxBacktrackDepth = 1u << 24;

  /// The stream of bytecode instructions, including the header.
  llvh::ArrayRef<uint8_t> bytecodeStream_;

  /// The flags associated with the match attempt.
  constants::MatchFlagType flags_;

  /// Syntax flags associated with the regex.
  SyntaxFlags syntaxFlags_;

  /// The first character in the input string.
  const CodeUnit *first_;

  /// The end of the input string (one-past the last).
  const CodeUnit *last_;

  /// Count of submatches.
  uint32_t markedCount_;

  /// Count of loops.
  uint32_t loopCount_;

  /// Traits used for canonicalization.
  Traits traits_;

  /// The remaining number of times we will attempt to backtrack.
  /// This is effectively a timeout on the regexp execution.
  uint32_t backtracksRemaining_ = kBacktrackLimit;

  /// Used to guard against stack overflow. Either uses real stack
  /// checking or call depth counter checking.
  StackOverflowGuard overflowGuard_;

  Context(
      llvh::ArrayRef<uint8_t> bytecodeStream,
      constants::MatchFlagType flags,
      SyntaxFlags syntaxFlags,
      const CodeUnit *first,
      const CodeUnit *last,
      uint32_t markedCount,
      uint32_t loopCount,
      StackOverflowGuard guard)
      : bytecodeStream_(bytecodeStream),
        flags_(flags),
        syntaxFlags_(syntaxFlags),
        first_(first),
        last_(last),
        markedCount_(markedCount),
        loopCount_(loopCount),
        overflowGuard_(guard) {}

  /// Run the given State \p state, by starting at its cursor and acting on its
  /// ip_ until the match succeeds or fails. If \p onlyAtStart is set, only
  /// test the match at \pos; otherwise test all successive input positions from
  /// pos_ through last_.
  /// \return a pointer to the start of the match if the match succeeds, nullptr
  /// if it fails. If the match succeeds, populates \p state with the state of
  /// the successful match; on failure the state's contents are undefined.
  /// Note the end of the match can be recovered as
  /// state->cursor_.currentPointer().
  ExecutorResult<const CodeUnit *> match(
      State<Traits> *state,
      bool onlyAtStart);

  /// Backtrack the given state \p s with the backtrack stack \p bts.
  /// \return true if we backtracked, false if we exhausted the stack.
  LLVM_NODISCARD
  ExecutorResult<bool> backtrack(BacktrackStack &bts, State<Traits> *s);

  /// Set the state's position to the body of a non-greedy loop.
  /// \return RETURNED if backtracking was prepared, STACK_OVERFLOW otherwise.
  LLVM_NODISCARD
  ExecutionStatus performEnterNonGreedyLoop(
      State<Traits> *s,
      const BeginLoopInsn *loop,
      uint32_t bodyIp,
      LoopData loopData,
      BacktrackStack &backtrackStack);

  /// Add a backtrack instruction to the backtrack stack \p bts.
  /// \return RETURNED on success, STACK_OVERFLOW otherwise
  LLVM_NODISCARD
  ExecutionStatus pushBacktrack(BacktrackStack &bts, BacktrackInsn insn) {
    bts.push_back(insn);
    if (LLVM_UNLIKELY(bts.size() > kMaxBacktrackDepth) ||
        LLVM_UNLIKELY(backtracksRemaining_ == 0)) {
      return ExecutionStatus::STACK_OVERFLOW;
    }
    backtracksRemaining_--;
    return ExecutionStatus::RETURNED;
  }

  /// Run the given Width1Loop \p insn on the given state \p s with the
  /// backtrack stack \p bts.
  /// \return true on success, false if we should backtrack.
  LLVM_NODISCARD
  ExecutorResult<bool> matchWidth1Loop(
      const Width1LoopInsn *insn,
      State<Traits> *s,
      BacktrackStack &bts);

 private:
  /// Do initialization of the given state before it enters the loop body
  /// described by the LoopInsn \p loop, including setting up any backtracking
  /// state.
  /// \return RETURNED if backtracking was prepared, STACK_OVERFLOW else
  LLVM_NODISCARD
  ExecutionStatus prepareToEnterLoopBody(
      State<Traits> *state,
      const BeginLoopInsn *loop,
      BacktrackStack &bts);

  /// Given a Width1Opcode \p w1opcode, return true if the given char \p c
  /// matches the instruction \p insn (with that opcode).
  template <Width1Opcode w1opcode>
  inline bool matchWidth1(const Insn *insn, CodeUnit c) const;

  /// \return true if all chars, stored in contiguous memory after \p insn,
  /// match the chars in state \p s in the same order, case insensitive. Note
  /// the count of chars is given in \p insn.
  inline bool matchesNCharICase8(
      const MatchNCharICase8Insn *insn,
      State<Traits> &s);

  /// Execute the given Width1 instruction \p loopBody on cursor \p c up to \p
  /// max times. \return the number of matches made, not to exceed \p max.
  /// Note we deliberately accept \p c by value.
  template <Width1Opcode w1opcode>
  inline uint32_t
  matchWidth1LoopBody(const Insn *loopBody, Cursor<Traits> c, uint32_t max);

  /// ES6 21.2.5.2.3 AdvanceStringIndex.
  /// Return the index of the next character to check.
  /// This is typically just the index + 1, except if Unicode is enabled we need
  /// to skip surrogate pairs.
  inline size_t advanceStringIndex(
      const CodeUnit *start,
      size_t index,
      size_t lastIndex) const;
};

/// We store loop and captured range data contiguously in a single allocation at
/// the end of the State. Use this union to simplify the use of
/// llvh::TrailingObjects.
union LoopOrCapturedRange {
  struct LoopData loopData;
  struct CapturedRange capturedRange;
};

/// State represents a set of in-flight capture groups and loop datas, along
/// with the IP and input position.
template <typename Traits>
struct State {
  using CharT = typename Traits::CodeUnit;

  /// The cursor in the input string.
  Cursor<Traits> cursor_;

  /// The instruction pointer position in the bytecode stream.
  uint32_t ip_ = 0;

  /// List of captured ranges. This has size equal to the number of marked
  /// subexpressions for the regex.
  llvh::SmallVector<CapturedRange, 16> capturedRanges_;

  /// List of loop datas. This has size equal to the number of loops for the
  /// regex.
  llvh::SmallVector<LoopData, 16> loopDatas_;

  /// \return the loop data at index \p idx.
  LoopData &getLoop(uint32_t idx) {
    assert(idx < loopDatas_.size() && "Invalid loop index");
    return loopDatas_[idx];
  }

  /// \return the captured range at index \p idx.
  CapturedRange &getCapturedRange(uint32_t idx) {
    // Captured ranges are allocated after loops, so add the loop count.
    assert(idx < capturedRanges_.size() && "Invalid captured range index");
    return capturedRanges_[idx];
  }

  /// Construct a state which with the given \p cursor, which can hold \p
  /// markedCount submatches and \p loopCount loop datas.
  State(Cursor<Traits> cursor, uint32_t markedCount, uint32_t loopCount)
      : cursor_(cursor),
        capturedRanges_(markedCount, {kNotMatched, kNotMatched}),
        loopDatas_(loopCount, {0, 0}) {}

  State(const State &) = default;
  State &operator=(const State &) = default;
  State(State &&) = default;
  State &operator=(State &&) = default;
};

/// ES5.1 7.3
template <class CharT>
bool isLineTerminator(CharT c) {
  return c == u'\u000A' || c == u'\u000D' || c == u'\u2028' || c == u'\u2029';
}

template <class Traits>
bool matchesLeftAnchor(Context<Traits> &ctx, State<Traits> &s, bool multiline) {
  bool matchesAnchor = false;
  const Cursor<Traits> &c = s.cursor_;
  if (c.atLeft()) {
    // Beginning of text.
    matchesAnchor = true;
  } else if (
      multiline && !c.atLeft() && isLineTerminator(c.currentPointer()[-1])) {
    // Multiline and after line terminator.
    matchesAnchor = true;
  }
  return matchesAnchor;
}

template <class Traits>
bool matchesRightAnchor(
    Context<Traits> &ctx,
    State<Traits> &s,
    bool multiline) {
  bool matchesAnchor = false;
  const Cursor<Traits> &c = s.cursor_;
  if (c.atRight() && !(ctx.flags_ & constants::matchNotEndOfLine)) {
    matchesAnchor = true;
  } else if (
      multiline && (!c.atRight()) && isLineTerminator(c.currentPointer()[0])) {
    matchesAnchor = true;
  }
  return matchesAnchor;
}

/// \return true if the cursor is at a word boundary, using the given
/// \p isWordChar predicate to classify characters. The result is xored with
/// \p invert.
template <class Traits, typename IsWordCharFn>
bool matchesWordBoundary(
    const Cursor<Traits> &c,
    bool invert,
    IsWordCharFn isWordChar) {
  const auto *charPointer = c.currentPointer();

  bool prevIsWordchar = false;
  if (!c.atLeft())
    prevIsWordchar = isWordChar(charPointer[-1]);

  bool currentIsWordchar = false;
  if (!c.atRight())
    currentIsWordchar = isWordChar(charPointer[0]);

  bool isWordBoundary = (prevIsWordchar != currentIsWordchar);
  return isWordBoundary != invert;
}

/// \return true if all chars, stored in contiguous memory after \p insn,
/// match the chars in state \p s in the same order. Note the count of chars
/// is given in \p insn.
template <class Traits>
bool matchesNChar8(const MatchNChar8Insn *insn, State<Traits> &s) {
  Cursor<Traits> &c = s.cursor_;
  auto insnCharPtr = reinterpret_cast<const char *>(insn + 1);
  auto charCount = insn->charCount;
  for (int idx = 0; idx < charCount; idx++) {
    if (c.consume() != insnCharPtr[idx]) {
      return false;
    }
  }
  return true;
}

template <class Traits>
bool Context<Traits>::matchesNCharICase8(
    const MatchNCharICase8Insn *insn,
    State<Traits> &s) {
  Cursor<Traits> &c = s.cursor_;
  auto insnCharPtr = reinterpret_cast<const char *>(insn + 1);
  auto charCount = insn->charCount;
  bool unicode = syntaxFlags_.unicode;
  for (int idx = 0; idx < charCount; idx++) {
    auto c1 = c.consume();
    char instC = insnCharPtr[idx];
    if (c1 != instC &&
        (char32_t)traits_.canonicalize(c1, unicode) != (char32_t)instC) {
      return false;
    }
  }
  return true;
}

/// Match a backreference against the input. Compares the captured text
/// (from \p cr) against the current cursor position character by character.
/// If \p icase is true, characters are compared after canonicalization.
/// \return true if the backreference matches, advancing \p c past the matched
/// text. Returns false (without modifying \p c) if the match fails.
template <class Traits>
bool matchBackRef(
    const Context<Traits> &ctx,
    CapturedRange cr,
    Cursor<Traits> &c,
    bool icase) {
  // TODO: this can be optimized by hoisting the branches out of the loop.
  bool unicode = ctx.syntaxFlags_.unicode;
  auto capturedStart = ctx.first_ + cr.start;
  auto capturedEnd = ctx.first_ + cr.end;
  Cursor<Traits> cursor2(
      capturedStart,
      c.forwards() ? capturedStart : capturedEnd,
      capturedEnd,
      c.forwards());
  Cursor<Traits> cursor1 = c;
  bool matched = true;
  while (matched && !cursor2.atEnd()) {
    if (cursor1.atEnd()) {
      matched = false;
    } else if (!icase) {
      // Direct comparison. Here we don't need to decode surrogate pairs.
      matched = (cursor1.consume() == cursor2.consume());
    } else if (!unicode) {
      // Case-insensitive non-Unicode comparison, no decoding of surrogate
      // pairs.
      auto c1 = cursor1.consume();
      auto c2 = cursor2.consume();
      matched =
          (c1 == c2 ||
           ctx.traits_.canonicalize(c1, unicode) ==
               ctx.traits_.canonicalize(c2, unicode));
    } else {
      // Unicode: we do need to decode surrogate pairs.
      auto cp1 = cursor1.consumeUTF16();
      auto cp2 = cursor2.consumeUTF16();
      matched =
          (cp1 == cp2 ||
           ctx.traits_.canonicalize(cp1, unicode) ==
               ctx.traits_.canonicalize(cp2, unicode));
    }
  }
  if (matched) {
    c.setCurrentPointer(cursor1.currentPointer());
  }
  return matched;
}

/// \return true if the character \p ch matches a bracket instruction \p insn,
/// containing the bracket ranges \p ranges. Note the count of ranges is given
/// in \p insn.
template <class Traits>
bool bracketMatchesChar(
    const Context<Traits> &ctx,
    const BracketInsn *insn,
    const BracketRange32 *ranges,
    typename Traits::CodePoint ch,
    bool icase) {
  return bracketMatchesCodePoint(
      ctx.traits_,
      insn->positiveCharClasses,
      insn->negativeCharClasses,
      insn->negate,
      llvh::makeArrayRef(ranges, insn->rangeCount),
      ch,
      icase,
      ctx.syntaxFlags_.unicode);
}

template <class Traits>
ExecutionStatus Context<Traits>::prepareToEnterLoopBody(
    State<Traits> *s,
    const BeginLoopInsn *loop,
    BacktrackStack &bts) {
  LoopData &loopData = s->getLoop(loop->loopId);
  auto res = pushBacktrack(
      bts, BacktrackInsn::makeSetLoopData(loop->loopId, loopData));
  if (res != ExecutionStatus::RETURNED) {
    return res;
  }
  loopData.iterations++;
  loopData.entryPosition = s->cursor_.offsetFromLeft();

  // Backtrack and reset contained capture groups.
  for (uint32_t mexp = loop->mexpBegin; mexp != loop->mexpEnd; mexp++) {
    auto &captureRange = s->getCapturedRange(mexp);
    res = pushBacktrack(
        bts, BacktrackInsn::makeSetCaptureGroup(mexp, captureRange));
    if (res != ExecutionStatus::RETURNED) {
      return res;
    }
    captureRange = {kNotMatched, kNotMatched};
  }
  return ExecutionStatus::RETURNED;
}

template <class Traits>
ExecutionStatus Context<Traits>::performEnterNonGreedyLoop(
    State<Traits> *s,
    const BeginLoopInsn *loop,
    uint32_t bodyIp,
    LoopData loopData,
    BacktrackStack &backtrackStack) {
  assert(loop->opcode == Opcode::BeginLoop && "Not a BeginLoopInsn");
  s->getLoop(loop->loopId) = loopData;

  // Set the IP and input position, and initialize the state for entering the
  // loop.
  s->ip_ = bodyIp;
  s->cursor_.setCurrentPointer(first_ + loopData.entryPosition);
  return prepareToEnterLoopBody(s, loop, backtrackStack);
}

template <class Traits>
ExecutorResult<bool> Context<Traits>::backtrack(
    BacktrackStack &bts,
    State<Traits> *s) {
  while (!bts.empty()) {
    BacktrackInsn &binsn = bts.back();
    switch (binsn.op) {
      case BacktrackOp::SetCaptureGroup:
        s->getCapturedRange(binsn.setCaptureGroup.mexp) =
            binsn.setCaptureGroup.range;
        bts.pop_back();
        break;

      case BacktrackOp::SetLoopData:
        s->getLoop(binsn.setLoopData.loopId) = binsn.setLoopData.loopData;
        bts.pop_back();
        break;

      case BacktrackOp::SetPosition:
        s->cursor_.setCurrentPointer(binsn.setPosition.value);
        s->ip_ = binsn.setPosition.ip;
        bts.pop_back();
        return true;

      case BacktrackOp::EnterNonGreedyLoop: {
        auto fields = binsn.enterNonGreedyLoop;
        bts.pop_back();
        auto res = performEnterNonGreedyLoop(
            s, fields.loopInsn, fields.bodyIp, fields.loopData, bts);
        if (res != ExecutionStatus::RETURNED) {
          return res;
        }
        return true;
      }

      case BacktrackOp::GreedyWidth1Loop:
      case BacktrackOp::NongreedyWidth1Loop: {
        // In both of these instructions, we have a range [min, max] containing
        // possible match locations, and the match failed at the max location
        // (if we are greedy) or the min location (nongreedy). Backtrack by
        // decrementing the max (incrementing the min) if we are greedy
        // (nongreedy), setting the IP to that location, and jumping to the loop
        // exit. Note that if we are tracking backwards (lookbehind assertion)
        // our maximum is before our minimum, so we have to reverse the
        // direction of increment/decrement.
        bool forwards = s->cursor_.forwards();
        assert(
            (forwards ? binsn.width1Loop.min <= binsn.width1Loop.max
                      : binsn.width1Loop.min >= binsn.width1Loop.max) &&
            "Loop min should be <= max (or >= max if backwards)");
        if (binsn.width1Loop.min == binsn.width1Loop.max) {
          // We have backtracked as far as possible. Give up.
          bts.pop_back();
          break;
        }
        if (binsn.op == BacktrackOp::GreedyWidth1Loop) {
          binsn.width1Loop.max += forwards ? -1 : 1;
          s->cursor_.setCurrentPointer(binsn.width1Loop.max);
        } else {
          binsn.width1Loop.min += forwards ? 1 : -1;
          s->cursor_.setCurrentPointer(binsn.width1Loop.min);
        }
        s->ip_ = binsn.width1Loop.continuation;
        return true;
      }
    }
  }
  // Exhausted the backtracking stack.
  return false;
}

template <class Traits>
template <Width1Opcode w1opcode>
bool Context<Traits>::matchWidth1(const Insn *base, CodeUnit c) const {
  // Note this switch should resolve at compile time.
  assert(
      base->opcode == static_cast<Opcode>(w1opcode) &&
      "Instruction has wrong opcode");
  switch (w1opcode) {
    case Width1Opcode::MatchChar8: {
      const auto *insn = llvh::cast<MatchChar8Insn>(base);
      return c == insn->c;
    }

    case Width1Opcode::MatchChar16: {
      const auto *insn = llvh::cast<MatchChar16Insn>(base);
      return c == insn->c;
    }

    case Width1Opcode::MatchCharICase8: {
      const auto *insn = llvh::cast<MatchCharICase8Insn>(base);
      return c == (CodePoint)insn->c ||
          (CodePoint)traits_.canonicalize(c, syntaxFlags_.unicode) ==
          (CodePoint)insn->c;
    }

    case Width1Opcode::MatchCharICase16: {
      const auto *insn = llvh::cast<MatchCharICase16Insn>(base);
      return c == insn->c ||
          (char32_t)traits_.canonicalize(c, syntaxFlags_.unicode) ==
          (char32_t)insn->c;
    }

    case Width1Opcode::MatchAny:
      return true;

    case Width1Opcode::MatchAnyButNewline:
      return !isLineTerminator(c);

    case Width1Opcode::Bracket: {
      // BracketInsn is followed by a list of BracketRange32s.
      assert(
          !(syntaxFlags_.unicode) &&
          "Unicode should not be set for Width 1 brackets");
      const BracketInsn *insn = llvh::cast<BracketInsn>(base);
      const BracketRange32 *ranges =
          reinterpret_cast<const BracketRange32 *>(insn + 1);
      return bracketMatchesChar<Traits>(*this, insn, ranges, c, false);
    }

    case Width1Opcode::BracketICase: {
      assert(
          !(syntaxFlags_.unicode) &&
          "Unicode should not be set for Width 1 brackets");
      const BracketICaseInsn *insn = llvh::cast<BracketICaseInsn>(base);
      const BracketRange32 *ranges =
          reinterpret_cast<const BracketRange32 *>(insn + 1);
      return bracketMatchesChar<Traits>(*this, insn, ranges, c, true);
    }
  }
  llvm_unreachable("Invalid width 1 opcode");
}

template <class Traits>
template <Width1Opcode w1opcode>
uint32_t Context<Traits>::matchWidth1LoopBody(
    const Insn *insn,
    Cursor<Traits> c,
    uint32_t max) {
  uint32_t iters = 0;
  for (; iters < max; iters++) {
    if (!matchWidth1<w1opcode>(insn, c.consume()))
      break;
  }
  return iters;
}

template <class Traits>
ExecutorResult<bool> Context<Traits>::matchWidth1Loop(
    const Width1LoopInsn *insn,
    State<Traits> *s,
    BacktrackStack &bts) {
  // Note we copy the cursor here.
  Cursor<Traits> c = s->cursor_;
  uint32_t matched = 0, minMatch = insn->min, maxMatch = insn->max;

  // Limit our max to the smaller of the maximum in the loop and number of
  // number of characters remaining. This allows us to avoid having to test for
  // end of input in the loop body.
  maxMatch = std::min(c.remaining(), maxMatch);

  // The loop body follows the loop instruction.
  const Insn *body = static_cast<const Insn *>(&insn[1]);

  // Match as far as we can up to maxMatch. Note we do this even if the loop is
  // non-greedy: we compute how far we might conceivably have to backtrack
  // (except in non-greedy loops we're "backtracking" by moving forwards).
  using W1 = Width1Opcode;
  switch (static_cast<Width1Opcode>(body->opcode)) {
    case W1::MatchChar8:
      matched = matchWidth1LoopBody<W1::MatchChar8>(body, c, maxMatch);
      break;
    case W1::MatchChar16:
      matched = matchWidth1LoopBody<W1::MatchChar16>(body, c, maxMatch);
      break;
    case W1::MatchCharICase8:
      matched = matchWidth1LoopBody<W1::MatchCharICase8>(body, c, maxMatch);
      break;
    case W1::MatchCharICase16:
      matched = matchWidth1LoopBody<W1::MatchCharICase16>(body, c, maxMatch);
      break;
    case W1::MatchAny:
      matched = matchWidth1LoopBody<W1::MatchAny>(body, c, maxMatch);
      break;
    case W1::MatchAnyButNewline:
      matched = matchWidth1LoopBody<W1::MatchAnyButNewline>(body, c, maxMatch);
      break;
    case W1::Bracket:
      matched = matchWidth1LoopBody<W1::Bracket>(body, c, maxMatch);
      break;
    case W1::BracketICase:
      matched = matchWidth1LoopBody<W1::BracketICase>(body, c, maxMatch);
      break;
  }

  // If we iterated less than the minimum, we failed to match.
  if (matched < minMatch) {
    return false;
  }
  assert(
      minMatch <= matched && matched <= maxMatch &&
      "matched should be between min and max match count");

  // Now we know the valid match range.
  // Compute the beginning and end pointers in this range.
  bool forwards = s->cursor_.forwards();
  const CodeUnit *pos = s->cursor_.currentPointer();
  const CodeUnit *minPos = forwards ? pos + minMatch : pos - minMatch;
  const CodeUnit *maxPos = forwards ? pos + matched : pos - matched;

  // If min == max (e.g. /a{3}/) then no backtracking is possible. If min < max,
  // backtracking is possible and we need to add a backtracking instruction.
  if (minMatch < matched) {
    BacktrackInsn backtrack{
        insn->greedy ? BacktrackOp::GreedyWidth1Loop
                     : BacktrackOp::NongreedyWidth1Loop};
    backtrack.width1Loop.continuation = insn->notTakenTarget;
    backtrack.width1Loop.min = minPos;
    backtrack.width1Loop.max = maxPos;
    auto res = pushBacktrack(bts, backtrack);
    if (res != ExecutionStatus::RETURNED)
      return res;
  }
  // Set the state's current position to either the minimum or maximum location,
  // and point it to the exit of the loop.
  s->cursor_.setCurrentPointer(insn->greedy ? maxPos : minPos);
  s->ip_ = insn->notTakenTarget;
  return true;
}

/// ES6 21.2.5.2.3. Effectively this skips surrogate pairs if the regexp has the
/// Unicode flag set.
template <class Traits>
inline size_t Context<Traits>::advanceStringIndex(
    const CodeUnit *start,
    size_t index,
    size_t length) const {
  if (sizeof(CodeUnit) == 1) {
    // The input string is ASCII and therefore cannot have surrogate pairs.
    return index + 1;
  }
  // "If unicode is false, return index+1."
  // "If index+1 >= length, return index+1."
  if (LLVM_LIKELY(!(syntaxFlags_.unicode)) || (index + 1 >= length))
    return index + 1;

  // Let first be the code unit value at index index in S
  // If first < 0xD800 or first > 0xDBFF, return index+1
  // Let second be the code unit value at index index+1 in S.
  // If second < 0xDC00 or second > 0xDFFF, return index+1.
  CodeUnit first = start[index];
  CodeUnit second = start[index + 1];
  if (LLVM_LIKELY(!isHighSurrogate(first)) ||
      LLVM_LIKELY(!isLowSurrogate(second))) {
    return index + 1;
  }
  // Return index+2.
  return index + 2;
}

/// The number of consecutive attempted start positions that pass the start
/// filter after which a search stops consulting it. Dropping the filter
/// only adds positions to try, so the search still finds the same match.
static constexpr unsigned kMaxStartFilterPasses = 16;

/// \return whether the code unit \p cu can begin a match according to the
/// start filter of \p header. A unit >= 0x80 passes iff StartFilterNonASCII
/// is set. The char path is not always 7-bit: the VM passes only ASCII
/// strings, but other callers of searchWithBytecode(const char *) pass
/// UTF-8 bytes, which are then units >= 0x80 like any other.
template <typename CU>
static inline bool startUnitPasses(const RegexBytecodeHeader &header, CU cu) {
  const uint32_t u = static_cast<typename std::make_unsigned<CU>::type>(cu);
  return u >= 0x80 ? (header.startFlags & StartFilterNonASCII) != 0
                   : startSetHasASCII(header, u);
}

/// \return the first index i in [\p from, \p length) such that the code
/// unit \p start[i] can begin a match according to the start filter of
/// \p header (see startUnitPasses()), or \p length if there is none.
template <typename CU>
static inline size_t findStartCandidate(
    const RegexBytecodeHeader &header,
    const CU *start,
    size_t from,
    size_t length) {
  if (from >= length)
    return length;
  if constexpr (sizeof(CU) == 1) {
    // With one ASCII member and no unit >= 0x80, a byte search finds it.
    if ((header.startFlags & (StartFilterSingleChar | StartFilterNonASCII)) ==
        StartFilterSingleChar) {
      const void *found =
          std::memchr(start + from, header.startChar, length - from);
      return found ? static_cast<const CU *>(found) - start : length;
    }
  }
  for (size_t i = from; i < length; ++i) {
    if (startUnitPasses(header, start[i]))
      return i;
  }
  return length;
}

template <class Traits>
auto Context<Traits>::match(State<Traits> *s, bool onlyAtStart)
    -> ExecutorResult<const CodeUnit *> {
  using State = State<Traits>;
  BacktrackStack backtrackStack;

  // We'll refer to the cursor often.
  Cursor<Traits> &c = s->cursor_;

  // Pull out the instruction portion of the bytecode, following the header.
  const uint8_t *const bytecode = &bytecodeStream_[sizeof(RegexBytecodeHeader)];

  // Save the incoming IP in case we have to loop.
  const auto startIp = s->ip_;

  const CodeUnit *const startLoc = c.currentPointer();

  // Use offsetFromRight() instead of remaining() here so that the length passed
  // to advanceStringIndex is accurate even when the cursor is going backwards.
  const size_t charsToRight = c.offsetFromRight();

  // Decide how many locations we'll need to check.
  // Note that we do want to check the empty range at the end, so add one to
  // charsToRight.
  const size_t locsToCheckCount = onlyAtStart ? 1 : 1 + charsToRight;

  // The start-position filter. Only a search that tries several positions
  // uses it: sticky and anchored searches and every lookaround recursion
  // (which may run a backwards cursor) pass onlyAtStart.
  // startFilterPasses counts the consecutive attempted positions whose unit
  // passed the filter. The filter is in use while it is below
  // kMaxStartFilterPasses: past that, the filter is not paying for itself
  // and is dropped for the rest of this search. One variable holds both, to
  // keep register pressure off the matching loop below.
  const auto *header =
      reinterpret_cast<const RegexBytecodeHeader *>(bytecodeStream_.data());
  unsigned startFilterPasses = !onlyAtStart &&
          (header->startFlags & StartFilterPresent) &&
          !(flags_ & constants::matchNoStartFilter)
      ? 0
      : kMaxStartFilterPasses;

  // If we are tracking backwards, we should only ever have one potential match
  // location. This is because advanceStringIndex only ever tracks forwards.
  assert(
      (c.forwards() || locsToCheckCount == 1) &&
      "Can only check one location when cursor is backwards");

#ifndef HERMES_CHECK_NATIVE_STACK
  StackOverflowGuard::CallFrameRAII frame{overflowGuard_};
#endif

  // Make sure we are not exceeding the set limit of the amount of times we can
  // recurse.
  if (overflowGuard_.isOverflowing()) {
    return ExecutionStatus::STACK_OVERFLOW;
  }

  // Macro used when a state fails to match.
#define BACKTRACK()                            \
  do {                                         \
    auto btRes = backtrack(backtrackStack, s); \
    if (LLVM_UNLIKELY(!btRes))                 \
      return btRes.getStatus();                \
    if (*btRes)                                \
      goto backtrackingSucceeded;              \
    goto backtrackingExhausted;                \
  } while (0)

  for (size_t locIndex = 0; locIndex < locsToCheckCount;
       locIndex = advanceStringIndex(startLoc, locIndex, charsToRight)) {
    if (startFilterPasses < kMaxStartFilterPasses) {
      // A filtered regex cannot match the empty string, so the position at
      // the end of the input needs no attempt.
      if (LLVM_UNLIKELY(locIndex >= charsToRight))
        return nullptr;
      if (startUnitPasses(*header, startLoc[locIndex])) {
        ++startFilterPasses;
      } else {
        // Skip to the next position whose unit can begin a match. Under /u,
        // every non-ASCII unit is treated alike, so the scan stops on a high
        // surrogate before its low surrogate, and advanceStringIndex then
        // steps over the pair.
        startFilterPasses = 0;
        locIndex =
            findStartCandidate(*header, startLoc, locIndex + 1, charsToRight);
        if (locIndex >= charsToRight)
          return nullptr;
      }
    }
    const CodeUnit *potentialMatchLocation = startLoc + locIndex;
    c.setCurrentPointer(potentialMatchLocation);
    s->ip_ = startIp;
  backtrackingSucceeded:
    for (;;) {
      const Insn *base = reinterpret_cast<const Insn *>(&bytecode[s->ip_]);
      switch (base->opcode) {
        case Opcode::Goal:
          return potentialMatchLocation;

        case Opcode::LeftAnchor:
          if (!matchesLeftAnchor(*this, *s, false))
            BACKTRACK();
          s->ip_ += sizeof(LeftAnchorInsn);
          break;

        case Opcode::LeftAnchorMultiline:
          if (!matchesLeftAnchor(*this, *s, true))
            BACKTRACK();
          s->ip_ += sizeof(LeftAnchorMultilineInsn);
          break;

        case Opcode::RightAnchor:
          if (!matchesRightAnchor(*this, *s, false))
            BACKTRACK();
          s->ip_ += sizeof(RightAnchorInsn);
          break;

        case Opcode::RightAnchorMultiline:
          if (!matchesRightAnchor(*this, *s, true))
            BACKTRACK();
          s->ip_ += sizeof(RightAnchorMultilineInsn);
          break;

        case Opcode::MatchAny:
          if (c.atEnd() ||
              !matchWidth1<Width1Opcode::MatchAny>(base, c.consume()))
            BACKTRACK();
          s->ip_ += sizeof(MatchAnyInsn);
          break;

        case Opcode::U16MatchAny:
          if (c.atEnd())
            BACKTRACK();
          c.consumeUTF16();
          s->ip_ += sizeof(U16MatchAnyInsn);
          break;

        case Opcode::MatchAnyButNewline:
          if (c.atEnd() ||
              !matchWidth1<Width1Opcode::MatchAnyButNewline>(base, c.consume()))
            BACKTRACK();
          s->ip_ += sizeof(MatchAnyButNewlineInsn);
          break;

        case Opcode::U16MatchAnyButNewline:
          if (c.atEnd() || isLineTerminator(c.consumeUTF16()))
            BACKTRACK();
          s->ip_ += sizeof(U16MatchAnyButNewlineInsn);
          break;

        case Opcode::MatchChar8: {
          if (c.atEnd() ||
              !matchWidth1<Width1Opcode::MatchChar8>(base, c.consume()))
            BACKTRACK();
          s->ip_ += sizeof(MatchChar8Insn);
          break;
        }

        case Opcode::MatchChar16: {
          if (c.atEnd() ||
              !matchWidth1<Width1Opcode::MatchChar16>(base, c.consume()))
            BACKTRACK();
          s->ip_ += sizeof(MatchChar16Insn);
          break;
        }

        case Opcode::U16MatchChar32: {
          const auto *insn = llvh::cast<U16MatchChar32Insn>(base);
          // Compare as uint32_t: casting the operand to CodePoint would
          // truncate it to a byte in the char executor.
          if (c.atEnd() || (uint32_t)c.consumeUTF16() != insn->c)
            BACKTRACK();
          s->ip_ += sizeof(U16MatchChar32Insn);
          break;
        }

        case Opcode::MatchCharICase8: {
          if (c.atEnd() ||
              !matchWidth1<Width1Opcode::MatchCharICase8>(base, c.consume()))
            BACKTRACK();
          s->ip_ += sizeof(MatchCharICase8Insn);
          break;
        }

        case Opcode::MatchCharICase16: {
          if (c.atEnd() ||
              !matchWidth1<Width1Opcode::MatchCharICase16>(base, c.consume()))
            BACKTRACK();
          s->ip_ += sizeof(MatchCharICase16Insn);
          break;
        }

        case Opcode::U16MatchCharICase32: {
          const auto *insn = llvh::cast<U16MatchCharICase32Insn>(base);
          bool matched = false;
          if (!c.atEnd()) {
            // Compare as uint32_t so a wide operand is not truncated to
            // CodePoint (a byte in the char executor).
            uint32_t cp = c.consumeUTF16();
            matched =
                (cp == insn->c ||
                 (uint32_t)traits_.canonicalize(cp, true) == insn->c);
          }
          if (!matched)
            BACKTRACK();
          s->ip_ += sizeof(U16MatchCharICase32Insn);
          break;
        }

        case Opcode::MatchNChar8: {
          const auto *insn = llvh::cast<MatchNChar8Insn>(base);
          if (c.remaining() < insn->charCount || !matchesNChar8(insn, *s))
            BACKTRACK();
          s->ip_ += insn->totalWidth();
          break;
        }

        case Opcode::MatchNCharICase8: {
          const auto *insn = llvh::cast<MatchNCharICase8Insn>(base);
          if (c.remaining() < insn->charCount || !matchesNCharICase8(insn, *s))
            BACKTRACK();
          s->ip_ += insn->totalWidth();
          break;
        }

        case Opcode::Alternation: {
          // We have an alternation. Determine which of our first and second
          // branches are viable. If both are, we have to split our state.
          const AlternationInsn *alt = llvh::cast<AlternationInsn>(base);
          bool primaryViable =
              c.satisfiesConstraints(flags_, alt->primaryConstraints);
          bool secondaryViable =
              c.satisfiesConstraints(flags_, alt->secondaryConstraints);
          if (primaryViable && secondaryViable) {
            // We need to explore both branches. Explore the primary branch
            // first, backtrack to the secondary one.
            s->ip_ += sizeof(AlternationInsn);
            auto res = pushBacktrack(
                backtrackStack,
                BacktrackInsn::makeSetPosition(
                    alt->secondaryBranch, c.currentPointer()));
            if (res != ExecutionStatus::RETURNED) {
              return res;
            }
          } else if (primaryViable) {
            s->ip_ += sizeof(AlternationInsn);
          } else if (secondaryViable) {
            s->ip_ = alt->secondaryBranch;
          } else {
            BACKTRACK();
          }
          break;
        }

        case Opcode::Jump32:
          s->ip_ = llvh::cast<Jump32Insn>(base)->target;
          break;

        case Opcode::Bracket: {
          if (c.atEnd() ||
              !matchWidth1<Width1Opcode::Bracket>(base, c.consume()))
            BACKTRACK();
          s->ip_ += llvh::cast<BracketInsn>(base)->totalWidth();
          break;
        }

        case Opcode::BracketICase: {
          if (c.atEnd() ||
              !matchWidth1<Width1Opcode::BracketICase>(base, c.consume()))
            BACKTRACK();
          s->ip_ += llvh::cast<BracketICaseInsn>(base)->totalWidth();
          break;
        }

        case Opcode::U16Bracket: {
          const U16BracketInsn *insn = llvh::cast<U16BracketInsn>(base);
          const BracketRange32 *ranges =
              reinterpret_cast<const BracketRange32 *>(insn + 1);
          if (c.atEnd() ||
              !bracketMatchesChar<Traits>(
                  *this, insn, ranges, c.consumeUTF16(), false))
            BACKTRACK();
          s->ip_ += insn->totalWidth();
          break;
        }

        case Opcode::U16BracketICase: {
          const auto *insn = llvh::cast<U16BracketICaseInsn>(base);
          const BracketRange32 *ranges =
              reinterpret_cast<const BracketRange32 *>(insn + 1);
          if (c.atEnd() ||
              !bracketMatchesChar<Traits>(
                  *this, insn, ranges, c.consumeUTF16(), true))
            BACKTRACK();
          s->ip_ += insn->totalWidth();
          break;
        }

        case Opcode::WordBoundary: {
          const auto *insn = llvh::cast<WordBoundaryInsn>(base);
          if (matchesWordBoundary(c, insn->invert, [&](auto ch) {
                return traits_.characterHasType(ch, CharacterClass::Words);
              }))
            s->ip_ += sizeof(WordBoundaryInsn);
          else
            BACKTRACK();
          break;
        }

        case Opcode::WordBoundaryICase: {
          const auto *insn = llvh::cast<WordBoundaryICaseInsn>(base);
          bool unicode = syntaxFlags_.unicode;
          if (matchesWordBoundary(c, insn->invert, [&](auto ch) {
                return traits_.characterHasType(ch, CharacterClass::Words) ||
                    traits_.characterHasType(
                        Traits::canonicalize(ch, unicode),
                        CharacterClass::Words);
              }))
            s->ip_ += sizeof(WordBoundaryICaseInsn);
          else
            BACKTRACK();
          break;
        }

        case Opcode::BeginMarkedSubexpression: {
          const auto *insn = llvh::cast<BeginMarkedSubexpressionInsn>(base);
          auto res = pushBacktrack(
              backtrackStack,
              BacktrackInsn::makeSetCaptureGroup(
                  insn->mexp, {kNotMatched, kNotMatched}));
          if (res != ExecutionStatus::RETURNED) {
            return res;
          }
          // When tracking backwards (in a lookbehind assertion) we traverse our
          // input backwards, so set the end before the start.
          auto &range = s->getCapturedRange(insn->mexp);
          if (c.forwards()) {
            range.start = c.offsetFromLeft();
          } else {
            range.end = c.offsetFromLeft();
          }
          s->ip_ += sizeof(BeginMarkedSubexpressionInsn);
          break;
        }

        case Opcode::EndMarkedSubexpression: {
          const auto *insn = llvh::cast<EndMarkedSubexpressionInsn>(base);
          auto &range = s->getCapturedRange(insn->mexp);
          if (c.forwards()) {
            assert(
                range.start != kNotMatched && "Capture group was not entered");
            range.end = c.offsetFromLeft();
          } else {
            assert(range.end != kNotMatched && "Capture group was not entered");
            range.start = c.offsetFromLeft();
          }
          assert(range.start <= range.end && "Captured range end before start");
          s->ip_ += sizeof(EndMarkedSubexpressionInsn);
          break;
        }

        // ES10 21.2.2.9.1
        case Opcode::BackRef: {
          const auto insn = llvh::cast<BackRefInsn>(base);
          // a. Let cap be x's captures List.
          // b. Let s be cap[n].
          CapturedRange cr = s->getCapturedRange(insn->mexp);

          // c. If s is undefined, return c(x).
          // Note we have to check both cr.start and cr.end here. If we are
          // currently in the middle of matching a capture group (going either
          // forwards or backwards) we should just return success.
          if (cr.start == kNotMatched || cr.end == kNotMatched) {
            // Backreferences to a capture group that did not match always
            // succeed (ES10 21.2.2.9)
            s->ip_ += sizeof(BackRefInsn);
            break;
          }
          if (!matchBackRef(*this, cr, c, false))
            BACKTRACK();
          s->ip_ += sizeof(BackRefInsn);
          break;
        }

        case Opcode::BackRefICase: {
          const auto insn = llvh::cast<BackRefICaseInsn>(base);
          CapturedRange cr = s->getCapturedRange(insn->mexp);
          if (cr.start == kNotMatched || cr.end == kNotMatched) {
            s->ip_ += sizeof(BackRefICaseInsn);
            break;
          }
          if (!matchBackRef(*this, cr, c, true))
            BACKTRACK();
          s->ip_ += sizeof(BackRefICaseInsn);
          break;
        }

        case Opcode::Lookaround: {
          const LookaroundInsn *insn = llvh::cast<LookaroundInsn>(base);
          bool matched = false;
          if (c.satisfiesConstraints(flags_, insn->constraints)) {
            // Copy the state. This is because if the match fails (or if we are
            // inverted) we need to restore its capture groups.
            State savedState{*s};

            // Set the direction of the cursor.
            c.setForwards(insn->forwards);

            // Invoke match() recursively with our expression.
            // Save and restore the position because lookaheads do not consume
            // anything.
            s->ip_ += sizeof(LookaroundInsn);
            auto match = this->match(s, true /* onlyAtStart */);
            // If the match errored out due to stack overflow, then we need to
            // return an error here as well.
            if (LLVM_UNLIKELY(!match)) {
              return match.getStatus();
            }
            // We got a match if the value is non-null.
            matched = match.getValue() != nullptr;
            c.setCurrentPointer(savedState.cursor_.currentPointer());
            c.setForwards(savedState.cursor_.forwards());

            // Restore capture groups unless we are a positive lookaround that
            // successfully matched. If we are a successfully matching positive
            // lookaround, set up backtracking to reset the capture groups. Note
            // we never backtrack INTO a successfully matched lookahead:
            // once a lookahead finds a match it forgets all other ways it could
            // have matched. (ES 5.1 15.10.2.8 Note 2).
            if (matched && !insn->invert) {
              // Backtrack capture groups in the lookahead expression.
              for (uint32_t i = insn->mexpBegin, e = insn->mexpEnd; i < e;
                   i++) {
                CapturedRange cr = savedState.getCapturedRange(i);
                auto res = pushBacktrack(
                    backtrackStack, BacktrackInsn::makeSetCaptureGroup(i, cr));
                if (res != ExecutionStatus::RETURNED)
                  return res;
              }
            } else {
              // Restore the saved state.
              *s = std::move(savedState);
            }
          }

          // 'matched' tells us whether the enclosed assertion expression
          // matched the input. This instruction matched the input if it is a
          // positive assertion (invert == false) and the expression matched,
          // or a negative assertion (invert == true) and the expression did
          // not match. Hence xor with invert.
          if (matched ^ insn->invert)
            s->ip_ = insn->continuation;
          else
            BACKTRACK();
          break;
        }

        case Opcode::BeginLoop: {
          // Here we are entering a loop from outside, not jumping back into
          // it.
          const BeginLoopInsn *loop = llvh::cast<BeginLoopInsn>(base);
          LoopData &loopData = s->getLoop(loop->loopId);
          // A nested loop may be entered again while backtrack entries from
          // its previous entry are still on the stack. Save its state so that
          // backtracking into the previous entry restores its iteration count.
          if (loopData.iterations != 0) {
            auto res = pushBacktrack(
                backtrackStack,
                BacktrackInsn::makeSetLoopData(loop->loopId, loopData));
            if (res != ExecutionStatus::RETURNED)
              return res;
          }
          loopData.iterations = 0;
          // Check to see if the loop body is viable. If not, and the loop has
          // a nonzero minimum iteration, then we know we won't match and we
          // can reject the state. If it does have a minimum iteration, we can
          // just skip to the not-taken target. Note that this is a static
          // property of the loop so we don't need to check it on every
          // iteration, only the first one.
          if (!c.satisfiesConstraints(flags_, loop->loopeeConstraints)) {
            if (loop->min > 0) {
              BACKTRACK();
            } else {
              s->ip_ = loop->notTakenTarget;
              break;
            }
          }
          goto runLoop;
        }

        case Opcode::EndLoop:
          // This is reached after the body of a loop finishes executing.
          // Move the IP to the loop and run it again immediately.
          s->ip_ = llvh::cast<EndLoopInsn>(base)->target;
          base = reinterpret_cast<const Insn *>(&bytecode[s->ip_]);
          // Note fall through.

        runLoop: {
          const BeginLoopInsn *loop = llvh::cast<BeginLoopInsn>(base);
          auto &loopData = s->getLoop(loop->loopId);
          uint32_t iteration = loopData.iterations;

          const uint32_t loopTakenIp = s->ip_ + sizeof(BeginLoopInsn);

          assert(loop->min <= loop->max && "Inconsistent loop bounds");

          // Check to see if we have looped more than the minimum number of
          // iterations, and if so, whether the subexpression we looped over
          // matched an empty string. ES6 21.2.2.5.1 Note 4: "once the
          // minimum number of repetitions has been satisfied, any more
          // expansions of Atom that match the empty character sequence are
          // not considered for further repetitions."
          if (iteration > loop->min &&
              loopData.entryPosition == c.offsetFromLeft())
            BACKTRACK();

          if (iteration < loop->min) {
            auto res = prepareToEnterLoopBody(s, loop, backtrackStack);
            if (res != ExecutionStatus::RETURNED)
              return res;
            s->ip_ = loopTakenIp;
          } else if (iteration == loop->max) {
            s->ip_ = loop->notTakenTarget;
          } else {
            // We are within the target iteration range, figure out whether we
            // should continue or exit.
            assert(iteration >= loop->min && iteration < loop->max);
            if (!loop->greedy) {
              // Backtrack by entering this non-greedy loop.
              loopData.entryPosition = c.offsetFromLeft();
              auto res = pushBacktrack(
                  backtrackStack,
                  BacktrackInsn::makeEnterNonGreedyLoop(
                      loop, loopTakenIp, loopData));
              if (res != ExecutionStatus::RETURNED) {
                return res;
              }
              s->ip_ = loop->notTakenTarget;
            } else {
              // Backtrack by exiting this greedy loop.
              auto pushRes = pushBacktrack(
                  backtrackStack,
                  BacktrackInsn::makeSetPosition(
                      loop->notTakenTarget, c.currentPointer()));
              if (pushRes != ExecutionStatus::RETURNED)
                return pushRes;

              auto prepRes = prepareToEnterLoopBody(s, loop, backtrackStack);
              if (prepRes != ExecutionStatus::RETURNED)
                return prepRes;
              s->ip_ = loopTakenIp;
            }
          }
          break;
        }

        case Opcode::BeginSimpleLoop: {
          // Here we are entering a simple loop from outside,
          // not jumping back into it.
          const BeginSimpleLoopInsn *loop =
              llvh::cast<BeginSimpleLoopInsn>(base);

          if (!c.satisfiesConstraints(flags_, loop->loopeeConstraints)) {
            s->ip_ = loop->notTakenTarget;
            break;
          }

          goto runSimpleLoop;
        }

        case Opcode::EndSimpleLoop:
          s->ip_ = llvh::cast<EndSimpleLoopInsn>(base)->target;
          base = reinterpret_cast<const Insn *>(&bytecode[s->ip_]);
          // Note: fall-through.

        runSimpleLoop: {
          const BeginSimpleLoopInsn *loop =
              llvh::cast<BeginSimpleLoopInsn>(base);
          // Since this is a simple loop, we'll always need to explore both
          // exiting the loop at this point and continuing to loop.
          // Note simple loops are always greedy.
          auto res = pushBacktrack(
              backtrackStack,
              BacktrackInsn::makeSetPosition(
                  loop->notTakenTarget, c.currentPointer()));
          if (res != ExecutionStatus::RETURNED) {
            return res;
          }
          s->ip_ += sizeof(BeginSimpleLoopInsn);
          break;
        }

        case Opcode::Width1Loop: {
          const Width1LoopInsn *loop = llvh::cast<Width1LoopInsn>(base);
          auto matchRes = matchWidth1Loop(loop, s, backtrackStack);
          if (LLVM_UNLIKELY(!matchRes))
            return matchRes.getStatus();
          if (!*matchRes)
            BACKTRACK();
          break;
        }
      }
    }
  // The search failed at this location.
  backtrackingExhausted:
    continue;
  }
#undef BACKTRACK
  // The match failed.
  return nullptr;
}

/// Entry point for searching a string via regex compiled bytecode.
/// Given the bytecode \p bytecode, search the range starting at \p first up to
/// (not including) \p last with the flags \p matchFlags. If the search
/// succeeds, poopulate MatchResults with the capture groups. \return true if
/// some portion of the string matched the regex represented by the bytecode,
/// false otherwise.
template <typename CharT, class Traits>
MatchRuntimeResult searchWithBytecodeImpl(
    llvh::ArrayRef<uint8_t> bytecode,
    const CharT *first,
    uint32_t start,
    uint32_t length,
    std::vector<CapturedRange> *m,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard) {
  assert(
      bytecode.size() >= sizeof(RegexBytecodeHeader) && "Bytecode too small");
  auto header = reinterpret_cast<const RegexBytecodeHeader *>(bytecode.data());

  // Check for match impossibility before doing anything else.
  Cursor<Traits> cursor{
      first, first + start, first + length, true /* forwards */};
  if (!cursor.satisfiesConstraints(matchFlags, header->constraints))
    return MatchRuntimeResult::NoMatch;

  auto markedCount = header->markedCount;
  auto loopCount = header->loopCount;

  Context<Traits> ctx(
      bytecode,
      matchFlags,
      SyntaxFlags::fromByte(header->syntaxFlags),
      first,
      first + length,
      header->markedCount,
      header->loopCount,
      guard);
  State<Traits> state{cursor, markedCount, loopCount};

  // We check only one location if either the regex pattern constrains us to, or
  // the flags request it (via the sticky flag 'y').
  bool onlyAtStart = (header->constraints & MatchConstraintAnchoredAtStart) ||
      (matchFlags & constants::matchOnlyAtStart);

  auto res = ctx.match(&state, onlyAtStart);
  if (!res) {
    assert(res.getStatus() == ExecutionStatus::STACK_OVERFLOW);
    return MatchRuntimeResult::StackOverflow;
  }
  if (const CharT *matchStartLoc = res.getValue()) {
    // Match succeeded. Return captured ranges. The first range is the total
    // match, followed by any capture groups.
    if (m != nullptr) {
      uint32_t totalStart = static_cast<uint32_t>(matchStartLoc - first);
      uint32_t totalEnd =
          static_cast<uint32_t>(state.cursor_.currentPointer() - first);
      m->clear();
      m->push_back(CapturedRange{totalStart, totalEnd});
      std::copy_n(
          state.capturedRanges_.begin(), markedCount, std::back_inserter(*m));
    }
    return MatchRuntimeResult::Match;
  }
  return MatchRuntimeResult::NoMatch;
}

MatchRuntimeResult searchWithBytecode(
    llvh::ArrayRef<uint8_t> bytecode,
    const char16_t *first,
    uint32_t start,
    uint32_t length,
    std::vector<CapturedRange> *m,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard) {
  return searchWithBytecodeImpl<char16_t, UTF16RegexTraits>(
      bytecode, first, start, length, m, matchFlags, guard);
}

MatchRuntimeResult searchWithBytecode(
    llvh::ArrayRef<uint8_t> bytecode,
    const char *first,
    uint32_t start,
    uint32_t length,
    std::vector<CapturedRange> *m,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard) {
  return searchWithBytecodeImpl<char, ASCIIRegexTraits>(
      bytecode, first, start, length, m, matchFlags, guard);
}

} // namespace regex
} // namespace hermes
