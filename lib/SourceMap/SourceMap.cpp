/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/SourceMap/SourceMap.h"

#include "llvh/Support/Path.h"

namespace hermes {

SourceMap::SourceMap(
    llvh::StringRef sourceRoot,
    llvh::StringRef originalSourceRoot,
    std::vector<std::string> &&sources,
    std::vector<SegmentList> &&lines,
    MetadataList &&sourcesMetadata)
    : sourceRoot_(sourceRoot),
      originalSourceRoot_(originalSourceRoot),
      sources_(std::move(sources)),
      lines_(std::move(lines)),
      sourcesMetadata_(std::move(sourcesMetadata)) {
  // Prepend sourceRoot_ to sources_ if it is not empty.
  // Make sure to leave absolute paths alone.
  if (!sourceRoot_.empty()) {
    rootedSources_.reserve(sources_.size());
    llvh::SmallString<32> buf{};
    for (const auto &p : sources_) {
      // Source-map paths are always POSIX-style (forward slashes) regardless of
      // the host platform, so force posix style rather than the native default
      // (which would emit backslashes on Windows).
      if (llvh::sys::path::is_absolute(p, llvh::sys::path::Style::posix)) {
        rootedSources_.push_back(p);
      } else {
        buf = sourceRoot_;
        llvh::sys::path::append(buf, llvh::sys::path::Style::posix, p);
        llvh::sys::path::remove_dots(buf, true, llvh::sys::path::Style::posix);
        rootedSources_.push_back(buf.str());
      }
    }
  }
}

/// Query source map text location for \p line and \p column.
/// In both the input and output of this function, line and column numbers
/// are 1-based.
llvh::Optional<SourceMapTextLocationFIndex>
SourceMap::getLocationForAddressFIndex(uint32_t line, uint32_t column) const {
  auto seg = this->getSegmentForAddress(line, column);
  // Unmapped location
  if (!seg.hasValue() || !seg->representedLocation.hasValue()) {
    return llvh::None;
  }
  // parseMappings() rejects any segment with an out-of-range sourceIndex.
  assert(
      (size_t)seg->representedLocation->sourceIndex < sources_.size() &&
      "SourceIndex is out-of-range.");
  return SourceMapTextLocationFIndex{
      (uint32_t)seg->representedLocation->sourceIndex,
      (uint32_t)seg->representedLocation->lineIndex + 1,
      (uint32_t)seg->representedLocation->columnIndex + 1};
}

llvh::Optional<SourceMapTextLocation> SourceMap::getLocationForAddress(
    uint32_t line,
    uint32_t column) const {
  auto loc = getLocationForAddressFIndex(line, column);
  if (!loc)
    return llvh::None;

  return SourceMapTextLocation{
      getSourceFullPath(loc->fileIndex), loc->line, loc->column};
}

llvh::Optional<SourceMap::Segment> SourceMap::getSegmentForAddress(
    uint32_t line,
    uint32_t column) const {
  if (line == 0 || line > lines_.size()) {
    return llvh::None;
  }

  // line is 1-based.
  uint32_t lineIndex = line - 1;
  auto &segments = lines_[lineIndex];
  if (segments.empty()) {
    return llvh::None;
  }
  assert(column >= 1 && "the column argument to this function is 1-based");
  uint32_t columnIndex = column - 1;
  // Algorithm: we wanted to locate the segment covering
  // the needle(`column`) -- segment.generatedColumn <= column.
  // We achieve it by binary searching the first sentinel
  // segment strictly greater than needle(`column`) and then move backward
  // one slot.
  auto segIter = std::upper_bound(
      segments.begin(),
      segments.end(),
      columnIndex,
      [](uint32_t column, const Segment &seg) {
        return column < (uint32_t)seg.generatedColumn;
      });
  // The found sentinel segment is the first one. No covering segment.
  if (segIter == segments.begin()) {
    return llvh::None;
  }
  // Move back one slot.
  const Segment &target =
      segIter == segments.end() ? segments.back() : *(--segIter);
  return target;
}

} // namespace hermes
