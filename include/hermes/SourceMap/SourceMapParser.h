/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SUPPORT_SOURCEMAPPARSER_H
#define HERMES_SUPPORT_SOURCEMAPPARSER_H

#include "hermes/SourceMap/SourceMapGenerator.h"

#include "llvh/Support/MemoryBuffer.h"

namespace hermes {

/// JavaScript version 3 source map parser.
/// See https://sourcemaps.info/spec.html for the spec that this class
/// parses.
class SourceMapParser {
 public:
  /// Parse input \p sourceMap and return parsed SourceMap. \p sourceMap must
  /// have a past-the-end null terminator.
  /// On failure if malformed, prints an error message and returns nullptr.
  static std::unique_ptr<SourceMap> parse(
      llvh::MemoryBufferRef sourceMap,
      llvh::StringRef baseDir,
      SourceErrorManager &sm);

 private:
  SourceMapParser() = delete;
  SourceMapParser(SourceMapParser &) = delete;
  SourceMapParser(SourceMapParser &&) = delete;
  SourceMapParser &operator=(SourceMapParser &) = delete;
  SourceMapParser &operator=(SourceMapParser &&) = delete;

  /// Delta encoding state.
  struct State {
    int32_t generatedColumn = 0;
    int32_t sourceIndex = 0;
    int32_t representedLine = 0;
    int32_t representedColumn = 0;
    int32_t nameIndex = 0;
  };

  /// Parse "mappings" section from \p sourceMappings. The parsed line mappings
  /// are returned in \p lines. \p sourcesCount is the number of entries in the
  /// "sources" array; any segment whose sourceIndex falls outside it is
  /// rejected.
  static bool parseMappings(
      llvh::StringRef sourceMappings,
      size_t sourcesCount,
      std::vector<SourceMap::SegmentList> &lines);

  /// Parse single segment in mapping.
  static llvh::Optional<SourceMap::Segment>
  parseSegment(const State &state, const char *&pCur, const char *pSegEnd);
};

} // namespace hermes

#endif // HERMES_SUPPORT_SOURCEMAPPARSER_H
