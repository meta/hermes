/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Support/SourceErrorManager.h"
#include "hermes/Support/UTF8.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/STLExtras.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/Support/raw_ostream.h"

namespace hermes {

static const char sTooManyErrors[] = "too many errors emitted";

SourceErrorManager::ICoordTranslator::~ICoordTranslator() = default;

SourceErrorManager::SourceErrorManager()
    : warningStatuses_((unsigned)Warning::_NumWarnings, true),
      warningsAreErrors_((unsigned)Warning::_NumWarnings, false) {
  sm_.setDiagHandler(SourceErrorManager::printDiagnostic, this);
}

void SourceErrorManager::BufferedMessage::addNote(
    std::vector<MessageData> &bufferedNotes,
    DiagKind dk,
    SMLoc loc,
    SMRange sm,
    std::string &&msg) {
  bufferedNotes.emplace_back(dk, loc, sm, std::move(msg));

  if (!noteCount_)
    firstNote_ = bufferedNotes.size() - 1;
  ++noteCount_;
}

llvh::iterator_range<const SourceErrorManager::MessageData *>
SourceErrorManager::BufferedMessage::notes(
    const std::vector<MessageData> &bufferedNotes) const {
  if (!noteCount_)
    return {nullptr, nullptr};
  return {
      bufferedNotes.data() + firstNote_,
      bufferedNotes.data() + firstNote_ + noteCount_};
}

void SourceErrorManager::enableBuffering() {
  ++bufferingEnabled_;
  assert(bufferingEnabled_ != 0 && "unsigned counter overflow");
}

void SourceErrorManager::disableBuffering() {
  assert(bufferingEnabled_ != 0 && "unsigned counter underflow");

  if (--bufferingEnabled_ != 0)
    return;

  // Sort pointers to the messages, so that the sort can use qsort. Messages
  // at the same location keep their emission order.
  llvh::SmallVector<const BufferedMessage *, 8> sorted;
  sorted.reserve(bufferedMessages_.size());
  for (const auto &bm : bufferedMessages_)
    sorted.push_back(&bm);
  llvh::array_pod_sort(
      sorted.begin(),
      sorted.end(),
      [](const BufferedMessage *const *pa, const BufferedMessage *const *pb) {
        const BufferedMessage *a = *pa;
        const BufferedMessage *b = *pb;
        // Make sure the "too many errors" message is always last.
        bool aLast =
            a->dk == DK_Error && !a->loc.isValid() && a->msg == sTooManyErrors;
        bool bLast =
            b->dk == DK_Error && !b->loc.isValid() && b->msg == sTooManyErrors;
        if (aLast != bLast)
          return aLast ? 1 : -1;
        if (!aLast && a->loc.getPointer() != b->loc.getPointer())
          return a->loc.getPointer() < b->loc.getPointer() ? -1 : 1;
        return a < b ? -1 : a > b ? 1 : 0;
      });

  // Print them.
  for (const BufferedMessage *bm : sorted) {
    doPrintMessage(bm->dk, bm->loc, bm->sm, bm->msg);
    for (const auto &note : bm->notes(bufferedNotes_))
      doPrintMessage(note.dk, note.loc, note.sm, note.msg);
  }

  // Clean the buffer.
  bufferedMessages_.clear();
  bufferedNotes_.clear();
}

unsigned SourceErrorManager::addNewSourceBuffer(
    std::unique_ptr<llvh::MemoryBuffer> f) {
  unsigned bufId = sm_.AddNewSourceBuffer(std::move(f), SMLoc{});
  assert(
      !isVirtualBufferId(bufId) && "unexpected virtual buf id from SourceMgr");
  return bufId;
}

/// Add a source buffer which maps to a filename. It doesn't contain any
/// source and the only operation that can be performed on that buffer is to
/// obtain the filename.
unsigned SourceErrorManager::addNewVirtualSourceBuffer(
    llvh::StringRef fileName) {
  return indexToVirtualBufferId(virtualBufferNames_.insert(fileName));
}

llvh::StringRef SourceErrorManager::getBufferFileName(unsigned bufId) const {
  if (isVirtualBufferId(bufId))
    return virtualBufferNames_[virtualBufferIdToIndex(bufId)];
  else
    return sm_.getMemoryBuffer(bufId)->getBufferIdentifier();
}

void SourceErrorManager::dumpCoords(
    llvh::raw_ostream &OS,
    const SourceCoords &coords) {
  if (coords.isValid()) {
    OS << getSourceUrl(coords.bufId) << ":" << coords.line << ":" << coords.col;
  } else {
    OS << "none:0,0";
  }
}

void SourceErrorManager::dumpCoords(llvh::raw_ostream &OS, SMLoc loc) {
  SourceCoords coords;
  findBufferLineAndLoc(loc, coords);
  dumpCoords(OS, coords);
}

void SourceErrorManager::countAndGenMessage(
    DiagKind dk,
    SMLoc loc,
    SMRange sm,
    const Twine &msg) {
  ++messageCount_[dk];
  doGenMessage(dk, loc, sm, msg);

  if (LLVM_UNLIKELY(dk == DK_Error && messageCount_[DK_Error] == errorLimit_)) {
    errorLimitReached_ = true;
    doGenMessage(DK_Error, {}, {}, sTooManyErrors);
  }
}

void SourceErrorManager::doGenMessage(
    hermes::SourceErrorManager::DiagKind dk,
    llvh::SMLoc loc,
    llvh::SMRange sm,
    llvh::Twine const &msg) {
  if (bufferingEnabled_) {
    // If this message is a note, try to associate it with the last message.
    // Note that theoretically the first buffered message could be a note, so
    // we play it safe here (even though it should never happen).
    if (dk == DK_Note && !bufferedMessages_.empty()) {
      bufferedMessages_.back().addNote(bufferedNotes_, dk, loc, sm, msg.str());
    } else {
      bufferedMessages_.emplace_back(dk, loc, sm, msg.str());
    }
  } else {
    doPrintMessage(dk, loc, sm, msg);
  }
}

void SourceErrorManager::doPrintMessage(
    DiagKind dk,
    SMLoc loc,
    SMRange sm,
    const Twine &msg) {
  sm_.PrintMessage(
      loc,
      static_cast<llvh::SourceMgr::DiagKind>(dk),
      msg,
      sm.isValid() ? llvh::ArrayRef<SMRange>(sm)
                   : llvh::ArrayRef<SMRange>(llvh::None),
      llvh::None,
      outputOptions_.showColors);
}

void SourceErrorManager::message(
    hermes::SourceErrorManager::DiagKind dk,
    llvh::SMLoc loc,
    llvh::SMRange sm,
    llvh::Twine const &msg,
    hermes::Warning w,
    Subsystem subsystem) {
  assert(dk <= DK_Note);
  if (suppressMessages_) {
    if (*suppressMessages_ == Subsystem::Unspecified) {
      return;
    }
    if (subsystem == *suppressMessages_) {
      return;
    }
  }
  // Suppress all messages once the error limit has been reached.
  if (LLVM_UNLIKELY(errorLimitReached_))
    return;
  if (dk == DK_Warning && !isWarningEnabled(w)) {
    lastMessageSuppressed_ = true;
    return;
  }
  // Automatically suppress notes if the last message was suppressed.
  if (dk == DK_Note && lastMessageSuppressed_)
    return;
  lastMessageSuppressed_ = false;

  /// Optionally upgrade warnings into errors.
  if (dk == DK_Warning && isWarningAnError(w)) {
    dk = DK_Error;
  }
  assert(static_cast<unsigned>(dk) < kMessageCountSize && "bounds check");

  if (externalMessageBuffer_) {
    externalMessageBuffer_->addMessage(dk, loc, sm, msg);
    return;
  }

  countAndGenMessage(dk, loc, sm, msg);
}

void SourceErrorManager::message(
    DiagKind dk,
    SMLoc loc,
    SMRange sm,
    const Twine &msg,
    Subsystem subsystem) {
  message(dk, loc, sm, msg, Warning::NoWarning, subsystem);
}

void SourceErrorManager::message(
    DiagKind dk,
    SMRange sm,
    const Twine &msg,
    Subsystem subsystem) {
  message(dk, sm.Start, sm, msg, subsystem);
}

void SourceErrorManager::message(
    DiagKind dk,
    SMLoc loc,
    const Twine &msg,
    Subsystem subsystem) {
  message(dk, loc, SMRange{}, msg, subsystem);
}

auto SourceErrorManager::findBufferAndLine(SMLoc loc) const
    -> llvh::Optional<LineCoord> {
  if (!loc.isValid())
    return llvh::None;

  auto bufId = sm_.FindBufferContainingLoc(loc);
  if (!bufId)
    return llvh::None;

  auto lineRefAndNo = sm_.FindLine(loc, bufId);

  return LineCoord{bufId, lineRefAndNo.second, lineRefAndNo.first};
}

/// Adjust the source location backwards making sure it doesn't point to \r or
/// in the middle of a utf-8 sequence.
static inline SMLoc adjustSourceLocation(const char *bufStart, SMLoc loc) {
  const char *ptr = loc.getPointer();
  // In the very unlikely case that `loc` points to a '\r', we skip backwards
  // until we find another character, while being careful not to fall off the
  // beginning of the buffer.
  if (LLVM_UNLIKELY(*ptr == '\r') ||
      LLVM_UNLIKELY(isUTF8ContinuationByte(*ptr))) {
    do {
      if (LLVM_UNLIKELY(ptr == bufStart)) {
        // This is highly unlikely but theoretically possible. There were only
        // '\r' between `loc` and the start of the buffer.
        break;
      }
      --ptr;
    } while (*ptr == '\r' || isUTF8ContinuationByte(*ptr));
  }
  return SMLoc::getFromPointer(ptr);
}

static bool locInside(llvh::StringRef str, SMLoc loc) {
  const char *ptr = loc.getPointer();
  return ptr >= str.begin() && ptr < str.end();
}

inline void SourceErrorManager::FindLineCache::fillCoords(
    SMLoc loc,
    SourceCoords &result) {
  loc = adjustSourceLocation(lineRef.data(), loc);
  result.bufId = bufferId;
  result.line = lineNo;
  result.col = loc.getPointer() - lineRef.data() + 1;
}

bool SourceErrorManager::findUntranslatedBufferLineAndLoc(
    llvh::SMLoc loc,
    SourceCoords &result) {
  if (!loc.isValid()) {
    result.bufId = 0;
    return false;
  }

  if (findLineCache_.bufferId) {
    // Check the cache with the hope that the lookup is within the last line or
    // the next line.
    if (locInside(findLineCache_.lineRef, loc)) {
      findLineCache_.fillCoords(loc, result);
      return true;
    }
    if (locInside(findLineCache_.nextLineRef, loc)) {
      ++findLineCache_.lineNo;
      findLineCache_.lineRef = findLineCache_.nextLineRef;
      findLineCache_.nextLineRef =
          sm_.getLineRef(findLineCache_.lineNo + 1, findLineCache_.bufferId);

      findLineCache_.fillCoords(loc, result);
      return true;
    }

    findLineCache_.bufferId = 0;
  }

  auto lineCoord = findBufferAndLine(loc);
  if (!lineCoord) {
    result.bufId = 0;
    return false;
  }

  // Populate the cache.
  findLineCache_.bufferId = lineCoord->bufId;
  findLineCache_.lineNo = lineCoord->lineNo;
  findLineCache_.lineRef = lineCoord->lineRef;
  findLineCache_.nextLineRef =
      sm_.getLineRef(findLineCache_.lineNo + 1, lineCoord->bufId);

  findLineCache_.fillCoords(loc, result);
  return true;
}

bool SourceErrorManager::findBufferLineAndLoc(
    llvh::SMLoc loc,
    SourceCoords &result) {
  if (!findUntranslatedBufferLineAndLoc(loc, result))
    return false;
  if (translator_)
    translator_->translate(result);
  return true;
}

uint32_t SourceErrorManager::findBufferIdForLoc(SMLoc loc) const {
  return sm_.FindBufferContainingLoc(loc);
}

const llvh::MemoryBuffer *SourceErrorManager::findBufferForLoc(
    SMLoc loc) const {
  uint32_t bufID = findBufferIdForLoc(loc);
  if (bufID == 0) {
    return nullptr;
  }
  return sm_.getMemoryBuffer(bufID);
}

std::pair<SMLoc, SMLoc> SourceErrorManager::findForCoordsImpl(
    unsigned bufId,
    unsigned line,
    OptValue<unsigned> col) {
  if (bufId == 0)
    return {};

  // TODO: optimize this with caching, etc.
  auto *buffer = getSourceBuffer(bufId);
  if (!buffer)
    return {};

  const char *cur = buffer->getBufferStart();
  const char *end = buffer->getBufferEnd();

  // Loop until we find the line or we reach EOF.
  unsigned lineNumber = 1;
  const char *lineEnd;
  while ((lineEnd = (const char *)std::memchr(cur, '\n', end - cur)) !=
             nullptr &&
         lineNumber != line) {
    ++lineNumber;
    cur = lineEnd + 1;
  }

  // If we didn't find LF, the end of the buffer is the end of the line.
  if (!lineEnd)
    lineEnd = end;

  // The last line we found is [cur..lineEnd) and its number is lineNumber.
  // Is it the right one?
  if (lineNumber != line)
    return {};

  // Trim a CR at start and end to account for all crazy line endings.
  if (cur != lineEnd && *cur == '\r')
    ++cur;
  if (cur != lineEnd && *(lineEnd - 1) == '\r')
    --lineEnd;

  if (!col.hasValue()) {
    return {SMLoc::getFromPointer(cur), SMLoc::getFromPointer(lineEnd)};
  }

  // Special case for empty line.
  if (cur == lineEnd) {
    // Column 1 or 0 in an empty line should work.
    if (col.getValue() <= 1)
      return {SMLoc::getFromPointer(cur), SMLoc::getFromPointer(lineEnd)};
    return {};
  }

  // Check for presence of UTF-8.
  bool utf8 = false;
  for (const char *p = cur; p != lineEnd; ++p) {
    if (LLVM_UNLIKELY(*p & 0x80)) {
      utf8 = true;
      break;
    }
  }

  // ASCII is easy - just add the offset.
  if (LLVM_LIKELY(!utf8)) {
    // Is the column in range?
    if (col.getValue() > (size_t)(lineEnd - cur))
      return {};
    return {
        SMLoc::getFromPointer(cur + col.getValue() - 1),
        SMLoc::getFromPointer(lineEnd)};
    ;
  }

  // Scan for the column while accounting for multi-byte characters.
  unsigned column = 0;
  for (; cur != lineEnd; ++cur) {
    // Skip continuation bytes.
    if (isUTF8ContinuationByte(*cur))
      continue;
    if (++column == col.getValue())
      return {SMLoc::getFromPointer(cur), SMLoc::getFromPointer(lineEnd)};
  }

  return {};
}

std::pair<std::string, std::string> SourceErrorManager::buildSourceAndCaretLine(
    const llvh::SMDiagnostic &diag,
    SourceErrorOutputOptions opts) {
  // Decode our source line to UTF-32
  // Ignore errors (UTF-8 errors will become replacement character)
  // Don't try to decode past embedded nulls
  // Map from narrow byte to column as we go
  std::vector<uint32_t> narrowByteToColumn;
  std::u32string sourceLine;
  std::string narrowSourceLine = diag.getLineContents();
  const char *cursor = narrowSourceLine.c_str();
  while (*cursor) {
    const char *prev = cursor;
    sourceLine.push_back(decodeUTF8<true>(cursor, [](const llvh::Twine &) {}));
    while (prev++ < cursor) {
      narrowByteToColumn.push_back(sourceLine.size() - 1);
    }
  }
  const size_t numColumns = sourceLine.size();

  // Widening helper
  auto widenColumn = [&](unsigned narrowColumn) -> unsigned {
    return narrowColumn < narrowByteToColumn.size()
        ? narrowByteToColumn[narrowColumn]
        : numColumns;
  };

  // Widen the caret column and ranges using our map
  // Ranges are of the form [first, last)
  assert(diag.getColumnNo() >= 0);
  const size_t columnNo = widenColumn(diag.getColumnNo());
  std::vector<std::pair<unsigned, unsigned>> ranges;
  for (const auto &r : diag.getRanges()) {
    ranges.emplace_back(widenColumn(r.first), widenColumn(r.second));
  }

  // Build the line with the caret and ranges.
  std::string caretLine(numColumns + 1, ' ');
  for (const auto &range : ranges) {
    if (range.first < caretLine.size()) {
      std::fill(
          &caretLine[range.first],
          &caretLine[std::min((size_t)range.second, caretLine.size())],
          '~');
    }
  }
  caretLine[std::min(size_t(columnNo), numColumns)] = '^';
  caretLine.erase(caretLine.find_last_not_of(' ') + 1);

  // Expand tabs to spaces in both the source and caret line
  const size_t tabStop = SourceErrorOutputOptions::TabStop;
  for (size_t pos = sourceLine.find('\t'); pos < sourceLine.size();
       pos = sourceLine.find('\t', pos)) {
    size_t expandCount = tabStop - (pos % tabStop);
    sourceLine.replace(pos, 1, expandCount, ' ');
    if (pos < caretLine.size()) {
      // Reuse the character in the caretLine, so that tabs in tildes expand to
      // more tildes
      caretLine.replace(pos, 1, expandCount, caretLine[pos]);
    }
    pos += expandCount;
  }

  // Trim the lines to respect preferredMaxErrorWidth
  // "Focus" around the caret, and any range intersecting it
  // Note ranges are of the form [start, end) and not [start, length)
  int focusStart = columnNo;
  int focusLength = 1;
  for (const auto &r : ranges) {
    if (r.first <= size_t(columnNo) && size_t(columnNo) < r.second) {
      focusStart = r.first;
      focusLength = r.second - r.first;
      break;
    }
  }
  size_t desiredLineLength = std::max(
      opts.preferredMaxErrorWidth,
      focusLength + SourceErrorOutputOptions::MinimumSourceContext);
  if (sourceLine.size() > desiredLineLength) {
    int focusCenter = focusStart + focusLength / 2;
    int leftTrimAmount = focusCenter - desiredLineLength / 2;
    if (leftTrimAmount > 0) {
      caretLine.erase(0, leftTrimAmount);
      sourceLine.erase(0, leftTrimAmount);
      std::fill(sourceLine.begin(), sourceLine.begin() + 3, '.');
    }
    if (sourceLine.size() > desiredLineLength) {
      // Trim on the right
      caretLine.erase(std::min(caretLine.size(), desiredLineLength));
      sourceLine.erase(desiredLineLength);
      std::fill(sourceLine.end() - 3, sourceLine.end(), '.');
    }
  }

  // Convert sourceLine back to narrow
  narrowSourceLine.clear();
  for (uint32_t c : sourceLine) {
    char buffer[UTF8CodepointMaxBytes] = {};
    char *buffCursor = buffer;
    encodeUTF8(buffCursor, c);
    narrowSourceLine.append(buffer, buffCursor);
  }
  return {std::move(narrowSourceLine), std::move(caretLine)};
}

/// Print a diagnostic to stderr, respecting the error output options.
/// If \p tag is non-null, print it before the diagnostic kind. It differenties
/// between the original and the translated location.
static void printDiagnosticHelper(
    const llvh::SMDiagnostic &diag,
    const SourceErrorOutputOptions &opts,
    const char *tag) {
  using llvh::raw_ostream;
  auto &S = llvh::errs();

  llvh::StringRef filename = diag.getFilename();
  // 1-based line number.
  int lineNo = diag.getLineNo();
  // 0-based column number.
  int columnNo = diag.getColumnNo();

  // Helpers to conditionally set or reset a color
  auto changeColor = [&](raw_ostream::Colors color, bool bold = true) {
    if (opts.showColors)
      S.changeColor(color, bold);
  };

  auto resetColor = [&]() {
    if (opts.showColors)
      S.resetColor();
  };

  changeColor(raw_ostream::SAVEDCOLOR);
  if (!filename.empty()) {
    S << (filename == "-" ? "<stdin>" : filename);
    if (lineNo != -1) {
      S << ':' << lineNo;
      if (columnNo != -1)
        S << ':' << (columnNo + 1);
    }
    S << ": ";
  }
  if (tag) {
    changeColor(raw_ostream::CYAN, false);
    S << tag << ' ';
  }

  switch (diag.getKind()) {
    case llvh::SourceMgr::DK_Error:
      changeColor(raw_ostream::RED);
      S << "error: ";
      break;
    case llvh::SourceMgr::DK_Warning:
      changeColor(raw_ostream::MAGENTA);
      S << "warning: ";
      break;
    case llvh::SourceMgr::DK_Note:
      changeColor(raw_ostream::BLACK);
      S << "note: ";
      break;
    case llvh::SourceMgr::DK_Remark:
      changeColor(raw_ostream::BLACK);
      S << "remark: ";
      break;
  }

  resetColor();
  changeColor(raw_ostream::SAVEDCOLOR);
  S << diag.getMessage() << '\n';
  resetColor();

  if (lineNo == -1 || columnNo == -1 || diag.getLineContents().empty())
    return;

  std::string sourceLine;
  std::string caretLine;
  std::tie(sourceLine, caretLine) =
      SourceErrorManager::buildSourceAndCaretLine(diag, opts);

  // Check for non-ASCII characters, which may have a width > 1
  // If we find them, don't try to show the caret line
  // TODO: bravely teach buildSourceAndCaretLine to use wcwidth(), lifting this
  // restriction
  bool showCaret = isAllASCII(sourceLine);

  S << sourceLine << '\n';
  if (showCaret) {
    changeColor(raw_ostream::GREEN);
    S << caretLine << '\n';
    resetColor();
  }
}

void SourceErrorManager::printDiagnostic(
    const llvh::SMDiagnostic &diag,
    void *ctx) {
  const SourceErrorManager *self = static_cast<SourceErrorManager *>(ctx);

  SourceCoords rawCoords(
      self->findBufferIdForLoc(diag.getLoc()),
      diag.getLineNo(),
      diag.getColumnNo() + 1);
  SourceCoords translated = rawCoords;
  if (self->translator_)
    self->translator_->translate(translated);

  if (translated.isValid() && translated != rawCoords) {
    // If the translator changed the location, print the original location
    // first.
    printDiagnosticHelper(
        llvh::SMDiagnostic(
            self->sm_,
            SMLoc(),
            self->getBufferFileName(translated.bufId).str(),
            translated.line,
            translated.col - 1,
            diag.getKind(),
            diag.getMessage(),
            {},
            {}),
        self->outputOptions_,
        "[original]");
    printDiagnosticHelper(diag, self->outputOptions_, "[transpiled]");
  } else {
    printDiagnosticHelper(diag, self->outputOptions_, nullptr);
  }
}

SMLoc SourceErrorManager::convertEndToLocation(SMRange range) {
  // If the range is empty, return the starting point.
  if (range.Start == range.End)
    return range.Start;

  return SMLoc::getFromPointer(range.End.getPointer() - 1);
}

} // namespace hermes
