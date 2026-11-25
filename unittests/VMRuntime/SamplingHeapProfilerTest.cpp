/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gtest/gtest.h"

#include "VMRuntimeTestHelpers.h"
#include "hermes/Parser/JSONParser.h"
#include "hermes/Support/Algorithms.h"
#include "hermes/Support/Allocator.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/HeapSnapshot.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/JSWeakMapImpl.h"
#include "hermes/VM/SymbolID.h"

#include "llvh/ADT/StringRef.h"
#include "llvh/Support/raw_ostream.h"

using namespace hermes::vm;
using namespace hermes::parser;

namespace hermes {
namespace {

// The sampling heap profiler only works with location traces enabled.
#ifdef HERMES_MEMORY_INSTRUMENTATION

static JSONObject *parseProfile(
    const std::string &json,
    JSONFactory &factory,
    const char *file,
    int line) {
  if (json.empty()) {
    ADD_FAILURE_AT(file, line) << "Snapshot wasn't written out";
    return nullptr;
  }

  SourceErrorManager sm;
  JSONParser parser{factory, json, sm};
  auto optSnapshot = parser.parse();
  if (!optSnapshot) {
    ADD_FAILURE_AT(file, line) << "Snapshot isn't valid JSON";
    return nullptr;
  }
  JSONValue *root = optSnapshot.getValue();
  if (!llvh::isa<JSONObject>(root)) {
    ADD_FAILURE_AT(file, line) << "Snapshot isn't a JSON object";
    return nullptr;
  }

  return llvh::cast<JSONObject>(root);
}

static JSONObject *takeProfile(
    Runtime &runtime,
    JSONFactory &factory,
    const char *file,
    int line) {
  std::string result("");
  llvh::raw_string_ostream str(result);
  runtime.collect("test");
  runtime.disableSamplingHeapProfiler(str);
  str.flush();
  return parseProfile(result, factory, file, line);
}

class SamplingProfileTree final {
 public:
  struct Node final {
    Node *parent;
    int64_t selfSize;
    int64_t id;
    struct CallFrame {
      std::string functionName;
      std::string scriptId;
      std::string url;
      int64_t lineNumber;
      int64_t columnNumber;
    } callFrame;
    std::vector<std::shared_ptr<Node>> children;
  };

  explicit SamplingProfileTree(const JSONObject &tree);

  const Node *getRoot() const {
    return root_.get();
  }

  const Node &getNodeByID(int64_t id) const {
    return *idToNode_.lookup(id);
  }

 private:
  std::shared_ptr<Node> root_;
  llvh::DenseMap<int64_t, std::shared_ptr<Node>> idToNode_;

  std::shared_ptr<Node> makeNode(Node *parent, const JSONObject &source);
};

SamplingProfileTree::SamplingProfileTree(const JSONObject &tree) {
  root_ = makeNode(nullptr, tree);
}

std::shared_ptr<SamplingProfileTree::Node> SamplingProfileTree::makeNode(
    Node *parent,
    const JSONObject &source) {
  std::shared_ptr<Node> node = std::make_shared<Node>();
  node->parent = parent;
  node->selfSize = llvh::cast<JSONNumber>(source.at("selfSize"))->getValue();
  node->id = llvh::cast<JSONNumber>(source.at("id"))->getValue();

  const JSONObject &callFrame = *llvh::cast<JSONObject>(source.at("callFrame"));
  node->callFrame.functionName =
      llvh::cast<JSONString>(callFrame.at("functionName"))->str();
  node->callFrame.scriptId =
      llvh::cast<JSONString>(callFrame.at("scriptId"))->str();
  node->callFrame.url = llvh::cast<JSONString>(callFrame.at("url"))->str();
  node->callFrame.lineNumber =
      llvh::cast<JSONNumber>(callFrame.at("lineNumber"))->getValue();
  node->callFrame.columnNumber =
      llvh::cast<JSONNumber>(callFrame.at("columnNumber"))->getValue();

  idToNode_[node->id] = node;
  const JSONArray &children = *llvh::cast<JSONArray>(source.at("children"));
  for (auto it = children.begin(), end = children.end(); it != end; ++it) {
    node->children.emplace_back(
        makeNode(node.get(), *llvh::cast<JSONObject>(*it)));
  }
  return node;
}

#define PARSE_PROFILE(...) parseProfile(__VA_ARGS__, __FILE__, __LINE__)
#define TAKE_PROFILE(...) takeProfile(__VA_ARGS__, __FILE__, __LINE__)

using SamplingHeapProfilerTest = RuntimeTestFixture;

TEST_F(SamplingHeapProfilerTest, Basic) {
  struct : public Locals {
    PinnedValue<JSArray> arrayToHold;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  // Use a fixed seed so the samples are deterministic.
  runtime.enableSamplingHeapProfiler(1 << 10, /*seed*/ 10);

  std::string source = R"(
// Use a separate function to make the test easier to write.
function allocator() {
  return new Object(); // the allocation happens here, line 4 col 20.
}
function foo() {
  var arr = [];
  for (var i = 0; i < 500; i++) {
    arr[i] = allocator();
  }
  // Make sure to return the array so that it isn't collected.
  return arr;
}
foo();
  )";
  hbc::CompileFlags flags;
  CallResult<HermesValue> res = runtime.run(source, "file:///fake.js", flags);
  ASSERT_FALSE(isException(res));
  // Hold onto this array so it isn't collected when the profile is finished.
  lv.arrayToHold.castAndSetHermesValue<JSArray>(*res);
  // Make sure it's the correct array.
  ASSERT_EQ(JSArray::getLength(*lv.arrayToHold, runtime), 500);

  JSONObject *root = TAKE_PROFILE(runtime, jsonFactory);
  ASSERT_TRUE(root != nullptr);
  const JSONObject &jsonTree = *llvh::cast<JSONObject>(root->at("head"));
  const JSONArray &samples = *llvh::cast<JSONArray>(root->at("samples"));
  EXPECT_NE(jsonTree.size(), 0ul);

  // The samples can happen at random times, so there's no predicting how many
  // objects were sampled, or what size they'll be. So we'll just test that the
  // correct keys exist, and reference existing nodes in the tree.
  SamplingProfileTree tree{jsonTree};
  EXPECT_TRUE(tree.getRoot() != nullptr);

  EXPECT_NE(samples.size(), 0ul) << "Should be at least one sample";
  for (auto it = samples.begin(), end = samples.end(); it != end; ++it) {
    const JSONObject &sample = *llvh::cast<JSONObject>(*it);
    const int64_t size = llvh::cast<JSONNumber>(sample.at("size"))->getValue();
    EXPECT_NE(size, 0);
    const int64_t id = llvh::cast<JSONNumber>(sample.at("nodeId"))->getValue();
    const auto &node = tree.getNodeByID(id);
    if (node.callFrame.functionName != "allocator") {
      // A sample might happen in another function, ignore them.
      continue;
    }
    EXPECT_EQ(node.callFrame.scriptId, "2");
    EXPECT_EQ(node.callFrame.url, "file:///fake.js");
    // Lines and columns are zero-based, so add 1 before comparing.
    EXPECT_EQ(node.callFrame.lineNumber + 1, 4);
    EXPECT_EQ(node.callFrame.columnNumber + 1, 20);
    EXPECT_NE(node.selfSize, 0);
  }
}

#endif

} // namespace
} // namespace hermes
