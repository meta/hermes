/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Parser/JSONParser.h"
#include "hermes/SourceMap/SourceMapGenerator.h"
#include "hermes/SourceMap/SourceMapParser.h"
#include "hermes/Support/Base64vlq.h"
#include "hermes/Support/SimpleDiagHandler.h"

#include "llvh/Support/MemoryBuffer.h"
#include "llvh/Support/raw_ostream.h"

#include "gtest/gtest.h"

using namespace hermes;
using namespace hermes::parser;

namespace {

/// Parse \p sourceMapContent (a test source-map string) using an empty baseDir.
/// Tests use this wrapper instead of the production MemoryBufferRef + baseDir
/// API for brevity.
std::unique_ptr<SourceMap> parseSourceMap(
    llvh::StringRef sourceMapContent,
    SourceErrorManager &sm) {
  return SourceMapParser::parse(
      llvh::MemoryBufferRef(sourceMapContent, "<source map>"),
      /* baseDir */ llvh::StringRef{},
      sm);
}

/// Test data from:
/// https://github.com/mozilla/source-map/blob/master/test/util.js
const char *TestMap = R"#({
  "version": 3,
  "file": "min.js",
  "names": ["bar", "baz", "n"],
  "sources": ["one.js", "two.js"],
  "sourceRoot": "/the/root/",
  "mappings": "CAAC,IAAI,IAAM,SAAUA,GAClB,OAAOC,IAAID;CCDb,IAAI,IAAM,SAAUE,GAClB,OAAOA"
})#";

// The following source map is missing enclosing brace so is invalid.
const char *InvalidJsonMap = R"#({
  "version": 3,
  "file": "min.js",
  "names": ["bar", "baz", "n"],
  "sources": ["one.js", "two.js"],
  "sourceRoot": "/the/root/",
)#";

// No "sourceRoot" field.
const char *TestMapNoSourceRoot = R"#({
  "version": 3,
  "file": "min.js",
  "names": ["bar", "baz", "n"],
  "sources": ["one.js", "two.js"],
  "mappings": "CAAC,IAAI,IAAM,SAAUA,GAClB,OAAOC,IAAID;CCDb,IAAI,IAAM,SAAUE,GAClB,OAAOA"
})#";

// Empty "sourceRoot" field.
const char *TestMapEmptySourceRoot = R"#({
  "version": 3,
  "file": "min.js",
  "names": ["bar", "baz", "n"],
  "sources": ["one.js", "two.js"],
  "sourceRoot": "",
  "mappings": "CAAC,IAAI,IAAM,SAAUA,GAClB,OAAOC,IAAID;CCDb,IAAI,IAAM,SAAUE,GAClB,OAAOA"
})#";

// Empty "sourceRoot" field.
const char *TestMapEmptyLines = R"#({
  "version": 3,
  "file": "min.js",
  "names": ["bar", "baz", "n"],
  "sources": ["one.js", "two.js"],
  "mappings": ";;CAAC,IAAI,IAAM,SAAUA,GAClB,OAAOC,IAAID;CCDb,IAAI,IAAM,SAAUE,GAClB,OAAOA"
})#";

/// Helper to return a Segment.
SourceMap::Segment loc(
    int32_t address /* 0-based */,
    int32_t sourceIndex,
    int32_t line /* 1-based */,
    int32_t column /* 0-based */,
    llvh::Optional<int32_t> nameIndex = llvh::None) {
  return SourceMap::Segment{address, sourceIndex, line - 1, column, nameIndex};
}

/// Helper to return a segment with no represented location.
SourceMap::Segment loc(int32_t address) {
  SourceMap::Segment seg;
  seg.generatedColumn = address;
  return seg;
}

void verifySegment(
    SourceMap &sourceMap,
    int generatedLine,
    const std::vector<std::string> &sources,
    const SourceMap::Segment &segment) {
  llvh::Optional<SourceMapTextLocation> locOpt =
      sourceMap.getLocationForAddress(
          generatedLine, segment.generatedColumn + 1);
  if (segment.representedLocation.hasValue()) {
    EXPECT_TRUE(locOpt.hasValue());
    EXPECT_EQ(
        locOpt.getValue().fileName,
        sources[segment.representedLocation->sourceIndex]);
    EXPECT_EQ(
        locOpt.getValue().line, segment.representedLocation->lineIndex + 1);
    EXPECT_EQ(
        locOpt.getValue().column, segment.representedLocation->columnIndex + 1);
  } else {
    EXPECT_FALSE(locOpt.hasValue());
  }
}

TEST(SourceMap, Basic) {
  SourceMapGenerator map;
  EXPECT_EQ(map.getMappingsLines().size(), 0u);

  std::vector<std::string> sources{"file1", "file2"};
  for (const auto &source : sources) {
    map.addSource(source);
  }

  std::vector<SourceMap::Segment> segmentsList[] = {
      {
          loc(0, 0, 1, 1), // addr 1:0 -> file1:1:1
          loc(2, 0, 2, 1), // addr 1:2 -> file1:2:1
          loc(3, 0, 3, 1), // addr 1:3 -> file1:3:1
          loc(4), //          addr 1:4 -> unmapped
          loc(5, 0, 3, 2), // addr 1:5 -> file1:3:2
      },
      {
          loc(0, 1, 6, 6), // addr 2:0 -> file2:6:6
          loc(1, 1, 7, 2), // addr 2:1 -> file2:7:2
          loc(3, 1, 7, 3), // addr 2:3 -> file2:7:3
          loc(5, 1, 8, 1), // addr 2:5 -> file2:8:1
      }};

  uint32_t i = 0;
  for (const auto &segments : segmentsList) {
    map.addMappingsLine(segments, i++);
  }

  ASSERT_EQ(map.getMappingsLines().size(), 2u);
  EXPECT_EQ(map.getMappingsLines()[0].size(), 5u);
  EXPECT_EQ(map.getMappingsLines()[1].size(), 4u);

  std::vector<uint32_t> functionOffsets1 = {20, 23, 50, 789};
  std::vector<uint32_t> functionOffsets2 = {1, 255, 300, 500};
  map.addFunctionOffsets(std::move(functionOffsets1), 0);
  map.addFunctionOffsets(std::move(functionOffsets2), 1);
  std::string storage;
  llvh::raw_string_ostream OS(storage);
  map.outputAsJSON(OS);
  EXPECT_EQ(
      OS.str(),
      R"#({"version":3,"sources":["file1","file2"],"mappings":"AAAC,EACA,CACA,C,CAAC;ACGI,CACJ,EAAC,EACF;","x_hermes_function_offsets":{"0":[20,23,50,789],"1":[1,255,300,500]}})#");

  SourceErrorManager sm;
  SimpleDiagHandlerRAII diagHandler(sm);
  std::unique_ptr<SourceMap> sourceMap = parseSourceMap(storage, sm);
  for (uint32_t line = 0; line < sizeof(segmentsList) / sizeof(segmentsList[0]);
       ++line) {
    const auto &segments = segmentsList[line];
    for (uint32_t i = 0; i < segments.size(); ++i) {
      verifySegment(
          *sourceMap, /*generatedLine*/ line + 1, sources, segments[i]);
    }
  }
}

TEST(SourceMap, InvalidJsonMapTest) {
  SourceErrorManager sm;
  SimpleDiagHandlerRAII diagHandler(sm);
  std::unique_ptr<SourceMap> sourceMap = parseSourceMap(InvalidJsonMap, sm);
  EXPECT_TRUE(sourceMap == nullptr);
};

/// "test that the `sources` field has the original sources" from
/// https://github.com/mozilla/source-map/blob/master/test/test-source-map-consumer.js
TEST(SourceMap, SourcesField) {
  auto verifySources = [](const char *sourceMapContent,
                          const std::vector<std::string> &expected) {
    SourceErrorManager sm;
    SimpleDiagHandlerRAII diagHandler(sm);
    std::unique_ptr<SourceMap> sourceMap = parseSourceMap(sourceMapContent, sm);
    std::vector<llvh::StringRef> sources = sourceMap->getAllFullPathSources();
    EXPECT_EQ(sources.size(), expected.size());
    for (uint32_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(sources[i], expected[i]);
    }
  };
  verifySources(TestMap, {"/the/root/one.js", "/the/root/two.js"});
  verifySources(TestMapNoSourceRoot, {"one.js", "two.js"});
  verifySources(TestMapEmptySourceRoot, {"one.js", "two.js"});
};

/// "test that the source root is reflected in a mapping's source field" from
/// https://github.com/mozilla/source-map/blob/master/test/test-source-map-consumer.js
TEST(SourceMap, SourceRoot) {
  SourceErrorManager sm;
  SimpleDiagHandlerRAII diagHandler(sm);
  std::unique_ptr<SourceMap> sourceMap = parseSourceMap(TestMap, sm);

  llvh::Optional<SourceMapTextLocation> locOpt =
      sourceMap->getLocationForAddress(2, 2);
  EXPECT_TRUE(locOpt.hasValue());
  EXPECT_EQ(locOpt.getValue().fileName, "/the/root/two.js");

  locOpt = sourceMap->getLocationForAddress(1, 2);
  EXPECT_TRUE(locOpt.hasValue());
  EXPECT_EQ(locOpt.getValue().fileName, "/the/root/one.js");

  std::unique_ptr<SourceMap> sourceMap2 =
      parseSourceMap(TestMapNoSourceRoot, sm);

  locOpt = sourceMap2->getLocationForAddress(2, 2);
  EXPECT_TRUE(locOpt.hasValue());
  EXPECT_EQ(locOpt.getValue().fileName, "two.js");

  locOpt = sourceMap2->getLocationForAddress(1, 2);
  EXPECT_TRUE(locOpt.hasValue());
  EXPECT_EQ(locOpt.getValue().fileName, "one.js");

  std::unique_ptr<SourceMap> sourceMap3 =
      parseSourceMap(TestMapEmptySourceRoot, sm);

  locOpt = sourceMap3->getLocationForAddress(2, 2);
  EXPECT_TRUE(locOpt.hasValue());
  EXPECT_EQ(locOpt.getValue().fileName, "two.js");

  locOpt = sourceMap3->getLocationForAddress(1, 2);
  EXPECT_TRUE(locOpt.hasValue());
  EXPECT_EQ(locOpt.getValue().fileName, "one.js");
};

/// "test mapping tokens back exactly" from
/// https://github.com/mozilla/source-map/blob/master/test/test-source-map-consumer.js
TEST(SourceMap, ExactMappings) {
  SourceErrorManager sm;
  SimpleDiagHandlerRAII diagHandler(sm);
  std::unique_ptr<SourceMap> sourceMap = parseSourceMap(TestMap, sm);

  std::vector<std::string> sources = {"/the/root/one.js", "/the/root/two.js"};

  int generatedLine = 1;
  int sourceIndex = 0;
  verifySegment(*sourceMap, generatedLine, sources, loc(1, sourceIndex, 1, 1));
  verifySegment(*sourceMap, generatedLine, sources, loc(5, sourceIndex, 1, 5));
  verifySegment(*sourceMap, generatedLine, sources, loc(9, sourceIndex, 1, 11));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(18, sourceIndex, 1, 21));
  verifySegment(*sourceMap, generatedLine, sources, loc(21, sourceIndex, 2, 3));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(28, sourceIndex, 2, 10));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(32, sourceIndex, 2, 14));

  generatedLine = 2;
  sourceIndex = 1;
  verifySegment(*sourceMap, generatedLine, sources, loc(1, sourceIndex, 1, 1));
  verifySegment(*sourceMap, generatedLine, sources, loc(5, sourceIndex, 1, 5));
  verifySegment(*sourceMap, generatedLine, sources, loc(9, sourceIndex, 1, 11));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(18, sourceIndex, 1, 21));
  verifySegment(*sourceMap, generatedLine, sources, loc(21, sourceIndex, 2, 3));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(28, sourceIndex, 2, 10));
};

/// "test mapping tokens fuzzy" from
/// https://github.com/mozilla/source-map/blob/master/test/test-source-map-consumer.js
TEST(SourceMap, FuzzyMappings) {
  SourceErrorManager sm;
  SimpleDiagHandlerRAII diagHandler(sm);
  std::unique_ptr<SourceMap> sourceMap = parseSourceMap(TestMap, sm);

  std::vector<std::string> sources = {"/the/root/one.js", "/the/root/two.js"};

  verifySegment(*sourceMap, 1, sources, loc(20, 0, 1, 21));
  verifySegment(*sourceMap, 1, sources, loc(30, 0, 2, 10));
  verifySegment(*sourceMap, 2, sources, loc(12, 1, 1, 11));
};

/// Test to make sure we can parse mappings with no represented location
TEST(SourceMap, NoRepresentedLocation) {
  SourceErrorManager sm;
  SimpleDiagHandlerRAII diagHandler(sm);
  std::unique_ptr<SourceMap> sourceMap = parseSourceMap(
      R"#({
        "version": 3,
        "sources": ["a.js", "b.js"],
        "mappings": "CACC,E,G;A,A,CCCC"
      })#",
      sm);

  std::vector<std::string> sources = {"a.js", "b.js"};

  int generatedLine = 1;
  int sourceIndex = 0;
  verifySegment(*sourceMap, generatedLine, sources, loc(1, sourceIndex, 2, 1));
  verifySegment(*sourceMap, generatedLine, sources, loc(3));
  verifySegment(*sourceMap, generatedLine, sources, loc(6));

  generatedLine = 2;
  sourceIndex = 1;
  verifySegment(*sourceMap, generatedLine, sources, loc(0));
  verifySegment(*sourceMap, generatedLine, sources, loc(0));
  verifySegment(*sourceMap, generatedLine, sources, loc(1, sourceIndex, 3, 2));
};

TEST(SourceMap, OutOfRangeSourceIndex) {
  // A segment whose sourceIndex names no entry in "sources" must be rejected.
  // Otherwise getLocationForAddress() indexes the sources vector out of bounds.
  // "sources" has one entry; "AgqjGAA" decodes to sourceIndex 100000, and
  // "ADAA" decodes to sourceIndex -1.
  for (const char *mappings : {"AgqjGAA", "ADAA"}) {
    SourceErrorManager sm;
    SimpleDiagHandlerRAII diagHandler(sm);
    std::string content = std::string(R"#({"version":3,"sources":["a.js"],)#") +
        R"#("mappings":")#" + mappings + R"#("})#";
    std::unique_ptr<SourceMap> sourceMap = parseSourceMap(content, sm);
    EXPECT_TRUE(sourceMap == nullptr);
  }
};

/// Test source map merging behavior.
///
/// Suppose we're compiling file1 and file2 and we have input source maps for
/// both files:
///
///           (I)
///     file1 -> file1orig
///     file2 -> file2orig
///
/// If we generate mappings to file1 and file2,
///
///           (II)
///     output -> file1 + file2
///
/// after merging them with (I) we expect to see:
///
///          (III)
///     output -> file1orig + file2orig (+ file1 + file2)
///
/// No mappings to file1 and file2 may remain in (III) because we consider
/// those to be intermediate artifacts (based on the fact that they have source
/// maps).
TEST(SourceMap, MergedWithInputSourceMaps) {
  static const char file1MapJson[] =
      R"#({
             "version": 3,
             "sources": ["file1orig"],
             "mappings": "CAAA"
          })#";

  static const char file2MapJson[] =
      R"#({
            "version": 3,
            "sourceRoot": "/foo/",
            "sources": ["file2orig"],
            "mappings": "CACA,KACC"
          })#";

  std::vector<std::unique_ptr<SourceMap>> inputSourceMaps{};
  SourceErrorManager sm;
  SimpleDiagHandlerRAII diagHandler(sm);
  inputSourceMaps.push_back(parseSourceMap(file1MapJson, sm));
  inputSourceMaps.push_back(parseSourceMap(file2MapJson, sm));

  std::vector<std::string> sources = {"file1", "file2"};
  SourceMap::SegmentList segments = {
      loc(0, 0, 1, 0), // addr 1:0 -> file1:1:0   (unmapped in file1orig)
      loc(2, 0, 1, 1), // addr 1:2 -> file1:1:1 -> file1orig:1:0
      loc(3, 0, 1, 4), // addr 1:3 -> file1:1:4 -> file1orig:1:0
      loc(4, 0, 2, 0), // addr 1:4 -> file1:2:0   (unmapped in file1orig)
      loc(5), //          addr 1:5 -> unmapped
      loc(6, 1, 1, 0), // addr 1:6 -> file2:1:0   (unmapped in file2orig)
      loc(7, 1, 1, 1), // addr 1:7 -> file2:1:1 -> file2orig:2:0
      loc(8, 1, 1, 4), // addr 1:8 -> file2:1:4 -> file2orig:2:0
      loc(9, 1, 1, 6), // addr 1:9 -> file2:1:6 -> file2orig:3:1
  };

  std::vector<std::string> expectedSources = {"file1orig", "/foo/file2orig"};
  SourceMap::SegmentList expectedSegments = {
      loc(0), //          addr 1:0(-> file1:1:0)-> unmapped
      loc(2, 0, 1, 0), // addr 1:2 -> file1:1:1 -> file1orig:1:0
      loc(3, 0, 1, 0), // addr 1:3 -> file1:1:4 -> file1orig:1:0
      loc(4), //          addr 1:4(-> file1:2:0)-> unmapped
      loc(5), //          addr 1:5 -> unmapped
      loc(6), //          addr 1:6(-> file2:1:0)-> unmapped
      loc(7, 1, 2, 0), // addr 1:7 -> file2:1:1 -> file2orig:2:0
      loc(8, 1, 2, 0), // addr 1:8 -> file2:1:4 -> file2orig:2:0
      loc(9, 1, 3, 1), // addr 1:9 -> file2:1:6 -> file2orig:3:1
  };

  SourceMapGenerator gen;
  for (const auto &source : sources) {
    gen.addSource(source);
  }

  gen.setInputSourceMaps(std::move(inputSourceMaps));
  gen.addMappingsLine(segments, 0);

  std::string storage;
  llvh::raw_string_ostream OS(storage);
  gen.outputAsJSON(OS);
  EXPECT_EQ(
      OS.str(),
      R"#({"version":3,"sources":["file1orig","\/foo\/file2orig"],)#"
      R"#("mappings":"A,EAAA,CAAA,C,C,C,CCCA,CAAA,CACC;"})#");

  std::unique_ptr<SourceMap> sourceMap = parseSourceMap(storage, sm);
  for (uint32_t i = 0; i < expectedSegments.size(); ++i) {
    verifySegment(
        *sourceMap, /*generatedLine*/ 1, expectedSources, expectedSegments[i]);
  }
}

class SimpleJSONParser {
  std::shared_ptr<JSLexer::Allocator> alloc_;
  JSONFactory factory_;
  SourceErrorManager sm_;
  JSONParser parser_;
  JSONValue *value_;

 public:
  const JSONValue *getValue() const {
    return value_;
  }

  JSONSharedValue getSharedValue() const {
    return JSONSharedValue(getValue(), alloc_);
  }

  SimpleJSONParser(llvh::StringRef input)
      : alloc_(std::make_shared<JSLexer::Allocator>()),
        factory_(*alloc_),
        parser_(factory_, input, sm_) {
    value_ = parser_.parse().getValue();
  }
};

TEST(SourceMap, PropagateFbMetadataFromInputs) {
  SourceMapGenerator gen;

  static const char file1MapJson[] =
      R"#({
             "version": 3,
             "sources": ["file1orig"],
             "x_facebook_sources": [[{
               "names": ["FILE1ORIG"],
               "mappings": "AAA,AAA"
             }]],
             "mappings": "CAAA"
          })#";

  static const char file2MapJson[] =
      R"#({
            "version": 3,
            "sourceRoot": "/foo/",
            "sources": ["file2orig"],
            "x_facebook_sources": [[{
              "names": ["FILE2ORIG"],
              "mappings": "AAA"
            }]],
            "mappings": "CACA,KACC"
          })#";

  SimpleJSONParser file2MetadataJson(
      R"#([{
             "names": ["FILE2"],
             "mappings": "AAA"
           }, 42])#");

  std::vector<std::unique_ptr<SourceMap>> inputSourceMaps{};
  SourceErrorManager sm;
  SimpleDiagHandlerRAII diagHandler(sm);
  inputSourceMaps.push_back(parseSourceMap(file1MapJson, sm));
  inputSourceMaps.push_back(parseSourceMap(file2MapJson, sm));

  SourceMap::SegmentList segments = {
      loc(0, 0, 1, 0), // addr 1:0 -> file1:1:0   (unmapped in file1orig)
      loc(2, 0, 1, 1), // addr 1:2 -> file1:1:1 -> file1orig:1:0
      loc(3, 0, 1, 4), // addr 1:3 -> file1:1:4 -> file1orig:1:0
      loc(4, 0, 2, 0), // addr 1:4 -> file1:2:0   (unmapped in file1orig)
      loc(5), //          addr 1:5 -> unmapped
      loc(6, 1, 1, 0), // addr 1:6 -> file2:1:0   (unmapped in file2orig)
      loc(7, 1, 1, 1), // addr 1:7 -> file2:1:1 -> file2orig:2:0
      loc(8, 1, 1, 4), // addr 1:8 -> file2:1:4 -> file2orig:2:0
      loc(9, 1, 1, 6), // addr 1:9 -> file2:1:6 -> file2orig:3:1
  };

  gen.addSource("file1", llvh::None);
  gen.addSource("file2", file2MetadataJson.getSharedValue());
  gen.setInputSourceMaps(std::move(inputSourceMaps));
  gen.addMappingsLine(segments, 0);

  std::string storage;
  llvh::raw_string_ostream OS(storage);
  gen.outputAsJSON(OS);
  EXPECT_EQ(
      OS.str(),
      R"#({"version":3,"sources":["file1orig","\/foo\/file2orig"],)#"
      R"#("x_facebook_sources":[[{"mappings":"AAA,AAA","names":["FILE1ORIG"]}],)#"
      R"#([{"mappings":"AAA","names":["FILE2ORIG"]}]],)#"
      R"#("mappings":"A,EAAA,CAAA,C,C,C,CCCA,CAAA,CACC;"})#");
}

/// Test that we output the x_facebook_sources field if we have data for it
TEST(SourceMap, GenerateWithFbMetadata) {
  SourceMapGenerator gen;

  SimpleJSONParser file2MetadataJson(
      R"#([{
             "names": ["<global>"],
             "mappings": "AAA"
           }, 42])#");

  SourceMap::SegmentList segments = {
      loc(0, 0, 1, 0), // addr 1:0 -> file1:1:0
      loc(1, 1, 1, 0), // addr 1:1 -> file2:1:0
  };

  gen.addSource("file1", llvh::None);
  gen.addSource("file2", file2MetadataJson.getSharedValue());
  gen.addMappingsLine(segments, 0);

  std::string storage;
  llvh::raw_string_ostream OS(storage);
  gen.outputAsJSON(OS);
  EXPECT_EQ(
      OS.str(),
      R"#({"version":3,"sources":["file1","file2"],"x_facebook_sources":)#"
      R"#([null,[{"mappings":"AAA","names":["<global>"]},42]],"mappings":)#"
      R"#("AAAA,CCAA;"})#");
}

/// Test to make sure we can properly parse empty lines.
TEST(SourceMap, EmptyLines) {
  SourceErrorManager sm;
  SimpleDiagHandlerRAII diagHandler(sm);
  std::unique_ptr<SourceMap> sourceMap = parseSourceMap(TestMapEmptyLines, sm);

  std::vector<std::string> sources = {"one.js", "two.js"};

  int generatedLine = 3;
  int sourceIndex = 0;
  verifySegment(*sourceMap, generatedLine, sources, loc(1, sourceIndex, 1, 1));
  verifySegment(*sourceMap, generatedLine, sources, loc(5, sourceIndex, 1, 5));
  verifySegment(*sourceMap, generatedLine, sources, loc(9, sourceIndex, 1, 11));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(18, sourceIndex, 1, 21));
  verifySegment(*sourceMap, generatedLine, sources, loc(21, sourceIndex, 2, 3));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(28, sourceIndex, 2, 10));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(32, sourceIndex, 2, 14));

  generatedLine = 4;
  sourceIndex = 1;
  verifySegment(*sourceMap, generatedLine, sources, loc(1, sourceIndex, 1, 1));
  verifySegment(*sourceMap, generatedLine, sources, loc(5, sourceIndex, 1, 5));
  verifySegment(*sourceMap, generatedLine, sources, loc(9, sourceIndex, 1, 11));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(18, sourceIndex, 1, 21));
  verifySegment(*sourceMap, generatedLine, sources, loc(21, sourceIndex, 2, 3));
  verifySegment(
      *sourceMap, generatedLine, sources, loc(28, sourceIndex, 2, 10));
}

TEST(SourceMap, VLQRandos) {
  // clang-format off
  const std::vector<int32_t> inputs = {0, 1, -1, 2, -2, 5298, -23498,
                                       INT_MIN/2, INT_MAX/2, INT_MIN+1,
                                       INT_MAX-1, INT_MIN, INT_MAX, INT_MIN+1,
                                       0, 100, 0, 100, 42};
  // clang-format on

  // Encode every input into the string.
  std::string storage;
  llvh::raw_string_ostream OS(storage);
  for (int32_t x : inputs)
    base64vlq::encode(OS, x);
  OS.flush();

  // Decode from the string.
  std::vector<int32_t> outputs;
  const char *begin = storage.c_str();
  const char *end = begin + storage.size();
  while (auto decoded = base64vlq::decode(begin, end))
    outputs.push_back(*decoded);
  EXPECT_EQ(outputs, inputs);
}

TEST(SourceMap, VLQDecodeInvalids) {
  auto decode = [](const char *s) -> OptValue<int32_t> {
    return base64vlq::decode(s, s + strlen(s));
  };
  auto opt = [](int32_t x) -> OptValue<int32_t> { return x; };
  const OptValue<int32_t> none{llvh::None};
  EXPECT_EQ(opt(INT32_MAX), decode("+/////D")); // 2**31-1
  EXPECT_EQ(opt(-INT32_MAX), decode("//////D")); // -2**31+1
  // note http://www.murzwin.com/base64vlq.html gets this wrong!
  EXPECT_EQ(opt(INT32_MIN), decode("hgggggE"));
  EXPECT_EQ(none, decode("jgggggE")); // -2**31-1.
  EXPECT_EQ(opt(1 << 30), decode("ggggggC")); // 2**30
  EXPECT_EQ(none, decode("gggggggC")); // Something very big and positive.
  EXPECT_EQ(none, decode("hggggggC")); // Something very big and negative.
  EXPECT_EQ(none, decode(""));
  EXPECT_EQ(none, decode("!"));
  EXPECT_EQ(opt(1024), decode("ggC"));
  EXPECT_EQ(none, decode(" ggC"));
  EXPECT_EQ(none, decode("//")); // too many continuation bits.
  EXPECT_EQ(none, decode("67")); // too many continuation bits.

  EXPECT_EQ(opt(0), decode("A")); // plain old zero
  // SourceMap encoding is one's complement, so this represents an integer
  // "negative zero." Treat it as zero.
  EXPECT_EQ(opt(0), decode("B"));
}

} // end anonymous namespace
