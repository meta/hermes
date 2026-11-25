/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifdef HERMES_MEMORY_INSTRUMENTATION

#include "hermes/VM/HeapSnapshot.h"
#include "EmptyCell.h"
#include "VMRuntimeTestHelpers.h"
#include "gtest/gtest.h"
#include "hermes/Parser/JSONParser.h"
#include "hermes/Support/Algorithms.h"
#include "hermes/Support/Allocator.h"
#include "hermes/Support/Compiler.h"
#include "hermes/VM/AlignedHeapSegment.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/DummyObject.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/JSWeakMapImpl.h"
#include "hermes/VM/RegExpBytecode.h"
#include "hermes/VM/RuntimeModule.h"
#include "hermes/VM/SymbolID.h"

#include "llvh/ADT/StringRef.h"
#include "llvh/Support/raw_ostream.h"

#include <chrono>
#include <set>
#include <sstream>

using namespace hermes::vm;
using namespace hermes::parser;

namespace hermes {
namespace unittest {
namespace heapsnapshottest {

using vm::testhelpers::DummyObject;

struct Node {
  HeapSnapshot::NodeType type;
  std::string name;
  HeapSnapshot::NodeID id;
  size_t selfSize;
  size_t edgeCount;
  size_t traceNodeID;
  int detachedness;

  Node() = default;

  explicit Node(
      HeapSnapshot::NodeType type,
      std::string name,
      HeapSnapshot::NodeID id,
      size_t selfSize,
      size_t edgeCount,
      size_t traceNodeID = 0,
      int detachedness = 0)
      : type{type},
        name{std::move(name)},
        id{id},
        selfSize{selfSize},
        edgeCount{edgeCount},
        traceNodeID{traceNodeID},
        detachedness{detachedness} {}

  static Node parse(JSONArray::iterator nodes, const JSONArray &strings) {
    // Need two levels of cast for enums because Windows complains about casting
    // doubles to enums.
    auto type = static_cast<HeapSnapshot::NodeType>(
        static_cast<unsigned>(llvh::cast<JSONNumber>(*nodes)->getValue()));
    nodes++;

    std::string name = llvh::cast<JSONString>(
                           strings[llvh::cast<JSONNumber>(*nodes)->getValue()])
                           ->str();
    nodes++;

    auto id = static_cast<HeapSnapshot::NodeID>(
        llvh::cast<JSONNumber>(*nodes)->getValue());
    nodes++;

    auto selfSize =
        static_cast<size_t>(llvh::cast<JSONNumber>(*nodes)->getValue());
    nodes++;

    auto edgeCount =
        static_cast<size_t>(llvh::cast<JSONNumber>(*nodes)->getValue());
    nodes++;

    auto traceNodeID =
        static_cast<size_t>(llvh::cast<JSONNumber>(*nodes)->getValue());
    nodes++;

    auto detachedness =
        static_cast<int>(llvh::cast<JSONNumber>(*nodes)->getValue());

    return Node{
        type,
        std::move(name),
        id,
        selfSize,
        edgeCount,
        traceNodeID,
        detachedness};
  }

  bool operator==(const Node &that) const {
    return type == that.type && name == that.name && id == that.id &&
        selfSize == that.selfSize && edgeCount == that.edgeCount &&
        traceNodeID == that.traceNodeID && detachedness == that.detachedness;
  }

  bool operator<(const Node &that) const {
    // Just IDs for comparison.
    return id < that.id;
  }
};

std::ostream &operator<<(std::ostream &os, const Node &node);

std::ostream &operator<<(std::ostream &os, const Node &node) {
  return os << "Node{type=" << HeapSnapshot::nodeTypeToName(node.type)
            << ", name=\"" << node.name << "\", id=" << node.id
            << ", selfSize=" << node.selfSize
            << ", edgeCount=" << node.edgeCount
            << ", traceNodeID=" << node.traceNodeID
            << ", detachedness=" << node.detachedness << "}";
}

struct Edge {
  HeapSnapshot::EdgeType type;
  bool isNamed;
  std::string name;
  int index;
  HeapSnapshot::NodeID toNode;

  Edge() = default;

  explicit Edge(
      HeapSnapshot::EdgeType type,
      std::string name,
      HeapSnapshot::NodeID toNode)
      : type{type},
        isNamed{true},
        name{std::move(name)},
        index{-1},
        toNode{toNode} {}

  explicit Edge(
      HeapSnapshot::EdgeType type,
      int index,
      HeapSnapshot::NodeID toNode)
      : type{type}, isNamed{false}, name{}, index{index}, toNode{toNode} {}

  static Edge parse(
      JSONArray::iterator edges,
      const JSONArray &nodes,
      const JSONArray &strings) {
    Edge edge;
    // Need two levels of cast for enums because Windows complains about casting
    // doubles to enums.
    edge.type = static_cast<HeapSnapshot::EdgeType>(
        static_cast<unsigned>(llvh::cast<JSONNumber>(*edges)->getValue()));
    ++edges;
    switch (edge.type) {
      case HeapSnapshot::EdgeType::Context:
      case HeapSnapshot::EdgeType::Internal:
      case HeapSnapshot::EdgeType::Property:
      case HeapSnapshot::EdgeType::Shortcut:
      case HeapSnapshot::EdgeType::Weak:
        edge.isNamed = true;
        edge.name = llvh::cast<JSONString>(
                        strings[llvh::cast<JSONNumber>(*edges)->getValue()])
                        ->str();
        edge.index = -1;
        break;
      default:
        edge.isNamed = false;
        // Leave name as the empty string.
        edge.index = llvh::cast<JSONNumber>(*edges)->getValue();
        break;
    }
    ++edges;

    uint32_t toNode = llvh::cast<JSONNumber>(*edges)->getValue();
    assert(
        toNode % HeapSnapshot::V8_SNAPSHOT_NODE_FIELD_COUNT == 0 &&
        "Invalid to node pointer");
    assert(toNode < nodes.size() && "Out-of-bounds node from edge");
    edge.toNode = Node::parse(nodes.begin() + toNode, strings).id;
    return edge;
  }

  bool operator==(const Edge &that) const {
    return type == that.type && isNamed == that.isNamed && name == that.name &&
        index == that.index && toNode == that.toNode;
  }

  bool operator<(const Edge &that) const {
    // Just toNode for comparison.
    return toNode < that.toNode;
  }
};

std::ostream &operator<<(std::ostream &os, const Edge &edge);

std::ostream &operator<<(std::ostream &os, const Edge &edge) {
  std::ios_base::fmtflags f(os.flags());
  os << std::boolalpha
     << "Edge{type=" << HeapSnapshot::edgeTypeToName(edge.type)
     << ", isNamed=" << edge.isNamed << ", name=\"" << edge.name
     << "\", index=" << edge.index << ", toNode=" << edge.toNode << "}";
  // Reset original flags to remove the boolalpha.
  os.flags(f);
  return os;
}

struct Sample {
  std::chrono::microseconds timestamp;
  HeapSnapshot::NodeID lastSeenObjectID;

  explicit Sample(
      std::chrono::microseconds timestamp,
      HeapSnapshot::NodeID lastSeenObjectID)
      : timestamp(timestamp), lastSeenObjectID(lastSeenObjectID) {}

  static Sample parse(JSONArray::iterator samples) {
    std::chrono::microseconds timestamp{
        static_cast<uint64_t>(llvh::cast<JSONNumber>(*samples)->getValue())};
    samples++;

    auto lastSeenObjectID = static_cast<HeapSnapshot::NodeID>(
        llvh::cast<JSONNumber>(*samples)->getValue());
    samples++;

    return Sample{timestamp, lastSeenObjectID};
  }
};

struct Location {
  Node object;
  facebook::hermes::debugger::ScriptID scriptID;
  int line;
  int column;

  Location() = default;

  explicit Location(
      Node object,
      facebook::hermes::debugger::ScriptID scriptID,
      int line,
      int column)
      : object{object}, scriptID{scriptID}, line{line}, column{column} {}

  static Location parse(
      JSONArray::iterator locations,
      const JSONArray &nodes,
      const JSONArray &strings) {
    Location loc;

    size_t objectIndex =
        static_cast<size_t>(llvh::cast<JSONNumber>(*locations)->getValue());
    loc.object = Node::parse(nodes.begin() + objectIndex, strings);
    locations++;

    loc.scriptID = llvh::cast<JSONNumber>(*locations)->getValue();
    locations++;

    // Line numbers and column numbers are 0-based internally,
    // but 1-based when viewed.
    loc.line = llvh::cast<JSONNumber>(*locations)->getValue() + 1;
    locations++;
    loc.column = llvh::cast<JSONNumber>(*locations)->getValue() + 1;
    locations++;

    return loc;
  }

  bool operator==(const Location &that) const {
    return object == that.object && scriptID == that.scriptID &&
        line == that.line && column == that.column;
  }
};

std::ostream &operator<<(std::ostream &os, const Location &loc);

std::ostream &operator<<(std::ostream &os, const Location &loc) {
  return os << "Location{object=" << loc.object << ", scriptID=" << loc.scriptID
            << ", line=" << loc.line << ", column=" << loc.column << "}";
}

static ::testing::AssertionResult testListOfStrings(
    JSONArray::iterator begin,
    JSONArray::iterator end,
    std::initializer_list<llvh::StringRef> strs) {
  EXPECT_EQ(static_cast<unsigned long>(end - begin), strs.size());
  auto strsIt = strs.begin();
  for (auto it = begin; it != end; ++it) {
    EXPECT_EQ(llvh::cast<JSONString>(*it)->str(), *strsIt);
    ++strsIt;
  }
  return ::testing::AssertionSuccess();
}

static ::testing::AssertionResult testListOfStrings(
    const JSONArray &arr,
    std::initializer_list<llvh::StringRef> strs) {
  return testListOfStrings(arr.begin(), arr.end(), strs);
}

static Node findNodeForID(
    HeapSnapshot::NodeID id,
    const JSONArray &nodes,
    const JSONArray &strings,
    const char *file,
    int line) {
  for (auto it = nodes.begin(), e = nodes.end(); it != e;
       it += HeapSnapshot::V8_SNAPSHOT_NODE_FIELD_COUNT) {
    auto node = Node::parse(it, strings);
    if (id == node.id) {
      return node;
    }
  }
  ADD_FAILURE_AT(file, line) << "No node found for id " << id;
  return Node{};
}

static std::pair<Node, std::vector<Edge>> findNodeAndEdgesForID(
    HeapSnapshot::NodeID id,
    const JSONArray &nodes,
    const JSONArray &edges,
    const JSONArray &strings,
    const char *file,
    int line) {
  auto nodesIt = nodes.begin();
  const auto nodesEnd = nodes.end();
  auto edgesIt = edges.begin();
  while (nodesIt != nodesEnd) {
    auto node = Node::parse(nodesIt, strings);
    const auto nextNodesEdges =
        edgesIt + (HeapSnapshot::V8_SNAPSHOT_EDGE_FIELD_COUNT * node.edgeCount);
    if (id == node.id) {
      std::vector<Edge> retEdges;
      for (; edgesIt != nextNodesEdges;
           edgesIt += HeapSnapshot::V8_SNAPSHOT_EDGE_FIELD_COUNT) {
        assert(edgesIt != edges.end() && "Edges shouldn't roll off the end");
        retEdges.emplace_back(Edge::parse(edgesIt, nodes, strings));
      }
      return {node, retEdges};
    }
    nodesIt += HeapSnapshot::V8_SNAPSHOT_NODE_FIELD_COUNT;
    edgesIt = nextNodesEdges;
  }
  ADD_FAILURE_AT(file, line) << "No node found for id " << id;
  return {Node{}, std::vector<Edge>{}};
}

#define FIND_NODE_FOR_ID(...) findNodeForID(__VA_ARGS__, __FILE__, __LINE__)
#define FIND_NODE_AND_EDGES_FOR_ID(...) \
  findNodeAndEdgesForID(__VA_ARGS__, __FILE__, __LINE__)

/// \return every node named \p name.
static std::vector<Node> findNodesNamed(
    const std::string &name,
    const JSONArray &nodes,
    const JSONArray &strings) {
  std::vector<Node> found;
  for (auto it = nodes.begin(), e = nodes.end(); it != e;
       it += HeapSnapshot::V8_SNAPSHOT_NODE_FIELD_COUNT) {
    Node node = Node::parse(it, strings);
    if (node.name == name)
      found.push_back(std::move(node));
  }
  return found;
}

static Location findLocationForID(
    HeapSnapshot::NodeID id,
    const JSONArray &locations,
    const JSONArray &nodes,
    const JSONArray &strings,
    const char *file,
    int line) {
  for (auto it = locations.begin(), e = locations.end(); it != e;
       it += HeapSnapshot::V8_SNAPSHOT_LOCATION_FIELD_COUNT) {
    Location loc = Location::parse(it, nodes, strings);
    if (loc.object.id == id) {
      return loc;
    }
  }
  ADD_FAILURE_AT(file, line) << "Location for id " << id << " not found";
  return Location{};
}

#define FIND_LOCATION_FOR_ID(...) \
  findLocationForID(__VA_ARGS__, __FILE__, __LINE__)

static JSONObject *parseSnapshot(
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

static JSONObject *takeSnapshot(
    GC &gc,
    JSONFactory &factory,
    bool captureNumericValue,
    const char *file,
    int line) {
  std::string result("");
  llvh::raw_string_ostream str(result);
  gc.collect("test");
  gc.createSnapshot(str, captureNumericValue);
  str.flush();
  return parseSnapshot(result, factory, file, line);
}

#define PARSE_SNAPSHOT(...) parseSnapshot(__VA_ARGS__, __FILE__, __LINE__)
#define TAKE_SNAPSHOT(...) takeSnapshot(__VA_ARGS__, __FILE__, __LINE__)

TEST(HeapSnapshotTest, IDReversibleTest) {
  // Make sure an ID <-> Object mapping is preserved across collections.
  auto runtime = DummyRuntime::create(
      GCConfig::Builder()
          .withInitHeapSize(1024)
          .withMaxHeapSize(1024 * 100)
          .build());
  DummyRuntime &rt = *runtime;
  auto &gc = rt.getHeap();
  struct : Locals {
    PinnedValue<DummyObject> obj;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  // Make a dummy object.
  lv.obj = DummyObject::create(gc, rt);
  const auto objID = gc.getObjectID(lv.obj.get());
  // Make sure the ID can be translated back to the object pointer.
  EXPECT_EQ(lv.obj.get(), gc.getObjectForID(objID));
  // Run a collection to move things around.
  gc.collect("test");
  // Test that the ID is the same and it can be reversed.
  EXPECT_EQ(objID, gc.getObjectID(lv.obj.get()));
  EXPECT_EQ(lv.obj.get(), gc.getObjectForID(objID));
}

TEST(HeapSnapshotTest, HeaderTest) {
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  auto runtime = DummyRuntime::create(
      GCConfig::Builder()
          .withInitHeapSize(1024)
          .withMaxHeapSize(1024 * 100)
          .build());
  DummyRuntime &rt = *runtime;
  auto &gc = rt.getHeap();

  JSONObject *root = TAKE_SNAPSHOT(gc, jsonFactory, true);
  ASSERT_TRUE(root != nullptr);

  JSONObject *snapshot = llvh::cast<JSONObject>(root->at("snapshot"));

  // Hades overrides snapshotAddGCNativeEdges()/snapshotAddGCNativeNodes(),
  // which adds 2 more nodes and edges, while MallocGC does not.
#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_MALLOC
  EXPECT_EQ(llvh::cast<JSONNumber>(snapshot->at("node_count"))->getValue(), 9);
  EXPECT_EQ(llvh::cast<JSONNumber>(snapshot->at("edge_count"))->getValue(), 4);
#else
  EXPECT_EQ(llvh::cast<JSONNumber>(snapshot->at("node_count"))->getValue(), 11);
  EXPECT_EQ(llvh::cast<JSONNumber>(snapshot->at("edge_count"))->getValue(), 6);
#endif
  EXPECT_EQ(
      llvh::cast<JSONNumber>(snapshot->at("trace_function_count"))->getValue(),
      0);

  JSONObject *meta = llvh::cast<JSONObject>(snapshot->at("meta"));
  EXPECT_EQ(llvh::cast<JSONArray>(meta->at("sample_fields"))->size(), 2);

  JSONArray &nodeFields = *llvh::cast<JSONArray>(meta->at("node_fields"));
  JSONArray &nodeTypes = *llvh::cast<JSONArray>(meta->at("node_types"));
  JSONArray &edgeFields = *llvh::cast<JSONArray>(meta->at("edge_fields"));
  JSONArray &edgeTypes = *llvh::cast<JSONArray>(meta->at("edge_types"));
  JSONArray &traceFunctionInfoFields =
      *llvh::cast<JSONArray>(meta->at("trace_function_info_fields"));
  JSONArray &traceNodeFields =
      *llvh::cast<JSONArray>(meta->at("trace_node_fields"));
  JSONArray &locationFields =
      *llvh::cast<JSONArray>(meta->at("location_fields"));
  JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  JSONArray &edges = *llvh::cast<JSONArray>(root->at("edges"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  // Check that node_fields/types are correct.
  EXPECT_TRUE(testListOfStrings(
      nodeFields,
      {"type",
       "name",
       "id",
       "self_size",
       "edge_count",
       "trace_node_id",
       "detachedness"}));
  const JSONArray &nodeTypeEnum = *llvh::cast<JSONArray>(nodeTypes[0]);
  EXPECT_TRUE(testListOfStrings(
      nodeTypeEnum,
      {"hidden",
       "array",
       "string",
       "object",
       "code",
       "closure",
       "regexp",
       "number",
       "native",
       "synthetic",
       "concatenated string",
       "sliced string",
       "symbol",
       "bigint"}));
  EXPECT_TRUE(testListOfStrings(
      nodeTypes.begin() + 1,
      nodeTypes.end(),
      {"string", "number", "number", "number", "number", "number"}));
  // Check that edge_fields/types are correct.
  EXPECT_TRUE(
      testListOfStrings(edgeFields, {"type", "name_or_index", "to_node"}));
  const JSONArray &edgeTypeEnum = *llvh::cast<JSONArray>(edgeTypes[0]);
  EXPECT_TRUE(testListOfStrings(
      edgeTypeEnum,
      {"context",
       "element",
       "property",
       "internal",
       "hidden",
       "shortcut",
       "weak"}));
  EXPECT_TRUE(testListOfStrings(
      edgeTypes.begin() + 1, edgeTypes.end(), {"string_or_number", "node"}));
  EXPECT_TRUE(testListOfStrings(
      traceFunctionInfoFields,
      {"function_id", "name", "script_name", "script_id", "line", "column"}));
  EXPECT_TRUE(testListOfStrings(
      traceNodeFields,
      {"id", "function_info_index", "count", "size", "children"}));
  EXPECT_TRUE(testListOfStrings(
      locationFields, {"object_index", "script_id", "line", "column"}));

  // Test the basic root nodes.
  Node superRoot{
      HeapSnapshot::NodeType::Synthetic,
      "",
      GC::IDTracker::reserved(GC::IDTracker::ReservedObjectID::SuperRoot),
      0,
      1};
  Node gcRoots{
      HeapSnapshot::NodeType::Synthetic,
      "(GC roots)",
      GC::IDTracker::reserved(GC::IDTracker::ReservedObjectID::GCRoots),
      0,
      1};
  Node customRoots{
      HeapSnapshot::NodeType::Synthetic,
      "(Custom)",
      GC::IDTracker::reserved(GC::IDTracker::ReservedObjectID::Custom),
      0,
      0};
  EXPECT_EQ(
      FIND_NODE_AND_EDGES_FOR_ID(superRoot.id, nodes, edges, strings),
      std::make_pair(
          superRoot,
          std::vector<Edge>{
              Edge{HeapSnapshot::EdgeType::Element, 1, gcRoots.id}}));
  // Don't test edges here because they contain GC-specific nodes.
  auto actualGCRootsNode = FIND_NODE_FOR_ID(gcRoots.id, nodes, strings);
  // Since each individual GC can choose what edges
  // exist, don't test the edge count.
  gcRoots.edgeCount = actualGCRootsNode.edgeCount;
  EXPECT_EQ(actualGCRootsNode, gcRoots);
  EXPECT_EQ(
      FIND_NODE_AND_EDGES_FOR_ID(customRoots.id, nodes, edges, strings),
      std::make_pair(customRoots, std::vector<Edge>{}));
}

TEST(HeapSnapshotTest, TestNodesAndEdgesForDummyObjects) {
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  auto runtime = DummyRuntime::create(
      GCConfig::Builder()
          .withInitHeapSize(1024)
          .withMaxHeapSize(1024 * 100)
          .build());
  DummyRuntime &rt = *runtime;
  auto &gc = rt.getHeap();
  struct : Locals {
    PinnedValue<DummyObject> dummy;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  lv.dummy = DummyObject::create(gc, rt);
  auto *dummy2 = DummyObject::create(gc, rt);
  lv.dummy->setPointer(gc, dummy2);
  const auto blockSize = lv.dummy->getAllocatedSize();

  JSONObject *root = TAKE_SNAPSHOT(gc, jsonFactory, true);
  ASSERT_TRUE(root != nullptr);

  // Check the nodes and edges.
  JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  JSONArray &edges = *llvh::cast<JSONArray>(root->at("edges"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  EXPECT_EQ(llvh::cast<JSONArray>(root->at("trace_function_infos"))->size(), 0);
  EXPECT_EQ(llvh::cast<JSONArray>(root->at("trace_tree"))->size(), 0);
  EXPECT_EQ(llvh::cast<JSONArray>(root->at("samples"))->size(), 0);
  EXPECT_EQ(llvh::cast<JSONArray>(root->at("locations"))->size(), 0);

  Node firstDummy{
      HeapSnapshot::NodeType::Object,
      cellKindStr(lv.dummy->getKind()),
      gc.getObjectID(lv.dummy.get()),
      blockSize,
      // One edge to the second dummy, 5 for primitive singletons, and a WeakRef
      // to self.
      7};
  Node undefinedNode{
      HeapSnapshot::NodeType::Object,
      "undefined",
      GC::IDTracker::reserved(GC::IDTracker::ReservedObjectID::Undefined),
      0,
      0};
  Node nullNode{
      HeapSnapshot::NodeType::Object,
      "null",
      GC::IDTracker::reserved(GC::IDTracker::ReservedObjectID::Null),
      0,
      0};
  Node trueNode(
      HeapSnapshot::NodeType::Object,
      "true",
      GC::IDTracker::reserved(GC::IDTracker::ReservedObjectID::True),
      0,
      0);
  Node numberNode{
      HeapSnapshot::NodeType::Number,
      "3.14",
      gc.getIDTracker().getNumberID(lv.dummy->hvDouble.getNumber()),
      0,
      0};
  Node nativeValueNode{
      HeapSnapshot::NodeType::Number,
      "",
      gc.getIDTracker().getNumberID(lv.dummy->hvNative.getNumber()),
      0,
      0};
  Node falseNode{
      HeapSnapshot::NodeType::Object,
      "false",
      GC::IDTracker::reserved(GC::IDTracker::ReservedObjectID::False),
      0,
      0};
  Node secondDummy{
      HeapSnapshot::NodeType::Object,
      cellKindStr(lv.dummy->getKind()),
      gc.getObjectID(lv.dummy->other),
      blockSize,
      // No edges except for the primitive singletons and the WeakRef to self.
      6};

  // Common edges.
  Edge trueEdge =
      Edge(HeapSnapshot::EdgeType::Internal, "HermesBool", trueNode.id);
  Edge numberEdge =
      Edge(HeapSnapshot::EdgeType::Internal, "HermesDouble", numberNode.id);
  Edge nativeValueEdge = Edge(
      HeapSnapshot::EdgeType::Internal, "HermesNative", nativeValueNode.id);
  Edge undefinedEdge = Edge(
      HeapSnapshot::EdgeType::Internal, "HermesUndefined", undefinedNode.id);
  Edge nullEdge =
      Edge(HeapSnapshot::EdgeType::Internal, "HermesNull", nullNode.id);

  // Two dummy objects.
  EXPECT_EQ(
      FIND_NODE_AND_EDGES_FOR_ID(firstDummy.id, nodes, edges, strings),
      std::make_pair(
          firstDummy,
          std::vector<Edge>{
              Edge{HeapSnapshot::EdgeType::Internal, "other", secondDummy.id},
              trueEdge,
              numberEdge,
              undefinedEdge,
              nativeValueEdge,
              nullEdge,
              Edge{HeapSnapshot::EdgeType::Weak, "weak", firstDummy.id}}));

  EXPECT_EQ(
      FIND_NODE_AND_EDGES_FOR_ID(secondDummy.id, nodes, edges, strings),
      std::make_pair(
          secondDummy,
          std::vector<Edge>{
              trueEdge,
              numberEdge,
              undefinedEdge,
              nativeValueEdge,
              nullEdge,
              Edge{HeapSnapshot::EdgeType::Weak, "weak", secondDummy.id}}));
}

#if (HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES) && \
    HERMESVM_SANITIZE_HANDLES == 0
// This test relies on the implementation details of Hades GC.
TEST(HeapSnapshotTest, SnapshotFromCallbackContextRunInMiddleYG) {
  // The GC Heap can have at most two segments.
  const GCConfig testGCConfig =
      GCConfig::Builder(kTestGCConfigBaseBuilder)
          .withInitHeapSize(FixedSizeHeapSegment::kSize)
          .withMaxHeapSize(FixedSizeHeapSegment::kSize * 2)
          .build();
  bool triggeredTripwire = false;
  auto runtime = DummyRuntime::create(
      testGCConfig.rebuild()
          .withTripwireConfig(
              GCTripwireConfig::Builder()
                  .withLimit(0)
                  .withCallback([&triggeredTripwire](GCTripwireContext &ctx) {
                    std::ostringstream stream;
                    ctx.createSnapshot(stream, true);
                    triggeredTripwire = true;
                  })
                  .build())
          .build());
  using LargeCell = EmptyCell<FixedSizeHeapSegment::maxSize() / 2>;
  // The first YG collection resizes the YG to between 45% and 55% of the
  // segment, depending on how long it took. Size cell4 so that cell4 and cell5
  // fit in the YG at any of those sizes, and size cell1 so that the free space
  // left in its OG segment fits exactly cell4.
  constexpr size_t kCell4Size =
      heapAlignSize(FixedSizeHeapSegment::maxSize() * 2 / 5);
  using MediumCell = EmptyCell<kCell4Size>;
  using OGCell = EmptyCell<FixedSizeHeapSegment::maxSize() - kCell4Size>;
  DummyRuntime &rt = *runtime;
  struct : Locals {
    PinnedValue<OGCell> cell1;
    PinnedValue<LargeCell> cell2;
    PinnedValue<LargeCell> cell3;
    PinnedValue<MediumCell> cell4;
    PinnedValue<DummyObject> cell5;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  // Create a cell in OG and make it dead. It won't be freed until Sweep phase.
  lv.cell1 = OGCell::createLongLived(rt);
  lv.cell1 = nullptr;

  // Create two cells to make YG full. Make them dead so that they won't be
  // evacuated in next YG collection.
  lv.cell2 = LargeCell::create(rt);
  lv.cell3 = LargeCell::create(rt);
  lv.cell2 = nullptr;
  lv.cell3 = nullptr;
  // Trigger a YG collection, which starts an OG collection.
  lv.cell4 = MediumCell::create(rt);

  // Add a small cell. It fits in the resized YG, so it won't trigger another
  // YG collection.
  lv.cell5 = DummyObject::create(rt.getHeap(), rt);
  // Create another cell, this should trigger YG again, and wait for OG to
  // finish in the middle of YG. Both cell4 and cell5 need to be moved to OG,
  // but OG won't have enough space. So it'll wait for OG collection to finish,
  // which frees cell1. If the tripwire callback is called at this point, it'll
  // crash since YG has moved one object and its KindAndSize is no longer valid
  // (it stores the forwarding pointer instead).
  LargeCell::create(rt);
  EXPECT_TRUE(triggeredTripwire);
}
#endif

TEST(HeapSnapshotTest, SnapshotFromCallbackContext) {
  bool triggeredTripwire = false;
  std::ostringstream stream;
  auto runtime = DummyRuntime::create(
      kTestGCConfigSmall.rebuild()
          .withTripwireConfig(
              GCTripwireConfig::Builder()
                  .withLimit(32)
                  .withCallback(
                      [&triggeredTripwire, &stream](GCTripwireContext &ctx) {
                        triggeredTripwire = true;
                        ctx.createSnapshot(stream, true);
                      })
                  .build())
          .build());
  DummyRuntime &rt = *runtime;
  struct : Locals {
    PinnedValue<DummyObject> dummy;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};
  lv.dummy = DummyObject::create(rt.getHeap(), rt);
  const auto dummyID = rt.getHeap().getObjectID(lv.dummy.get());
  rt.collect();
  ASSERT_TRUE(triggeredTripwire);

  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  JSONObject *root = PARSE_SNAPSHOT(stream.str(), jsonFactory);
  ASSERT_TRUE(root != nullptr);

  JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  // Check that the dummy object is in the snapshot.
  auto dummyNode = FIND_NODE_FOR_ID(dummyID, nodes, strings);
  Node expected{
      HeapSnapshot::NodeType::Object,
      "DummyObject",
      dummyID,
      lv.dummy->getAllocatedSize(),
      6};
  EXPECT_EQ(dummyNode, expected);
}

using HeapSnapshotRuntimeTest = RuntimeTestFixture;

template <typename T>
size_t firstNamedPropertyEdge() {
  // __proto__, class, directProp$i
  return JSObject::DIRECT_PROPERTY_SLOTS - JSObject::numOverlapSlots<T>() + 2;
}

TEST_F(HeapSnapshotRuntimeTest, FunctionLocationForLazyCode) {
  // Similar test to the above, but for lazy-compiled source.
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};

  hbc::CompileFlags flags;
  flags.debug = true;
  flags.lazy = true;

  // Build a function that will be lazily compiled.
  std::string source = "    function myGlobal() { ";
  for (int i = 0; i < 100; i++)
    source += " Math.random(); ";
  source += "};\nmyGlobal;";

  CallResult<HermesValue> res = runtime.run(source, "file:///fake.js", flags);
  ASSERT_FALSE(isException(res));
  Handle<JSFunction> func = runtime.makeHandle(vmcast<JSFunction>(*res));
  const auto funcID = runtime.getHeap().getObjectID(func.get());

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, true);
  ASSERT_TRUE(root != nullptr);

  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  // This test requires a location to be emitted.
  auto node = FIND_NODE_FOR_ID(funcID, nodes, strings);
  Node expected{
      HeapSnapshot::NodeType::Closure,
      "myGlobal",
      funcID,
      func->getAllocatedSize(),
      firstNamedPropertyEdge<JSFunction>() + 3};
  EXPECT_EQ(node, expected);
  // Edges aren't tested in this test.

#ifdef HERMES_ENABLE_DEBUGGER
  // The location isn't emitted in fully optimized builds.
  const JSONArray &locations = *llvh::cast<JSONArray>(root->at("locations"));
  Location loc = FIND_LOCATION_FOR_ID(funcID, locations, nodes, strings);
  // The location should be the given file, at line 1 column 5 with indenting
  auto scriptId = func->getRuntimeModule()->getScriptID();
  EXPECT_EQ(loc, Location(expected, scriptId, 1, 5));
#else
  (void)findLocationForID;
#endif
}

TEST_F(HeapSnapshotRuntimeTest, RegExpBytecodeReportedByItsModule) {
  struct : public Locals {
    PinnedValue<JSFunction> f;
    PinnedValue<Domain> domain;
    PinnedValue<JSRegExp> r0;
    PinnedValue<JSRegExp> r1;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};

  CallResult<HermesValue> res = runtime.run(
      "(function () { return function () { return /a(b)c/g; }; })()",
      "file:///fake.js",
      hbc::CompileFlags{});
  ASSERT_FALSE(isException(res));
  lv.f = vmcast<JSFunction>(*res);
  Handle<JSFunction> f = lv.f;
  RuntimeModule *rm = f->getRuntimeModule();
  const size_t numRegExps = rm->getBytecode()->getRegExpTable().size();
  ASSERT_EQ(1u, numRegExps);
  // Precondition: nothing is cached yet.
  ASSERT_EQ(0u, rm->regExpCacheMemorySize());
  const auto rmID = runtime.getHeap().getNativeID(rm);
  lv.domain = rm->getDomainUnsafe(runtime);
  Handle<Domain> domain = lv.domain;
  auto domainMallocSize = [&]() {
    GCCell *cell = domain.get();
    return cell->getVT()->getMallocSize(cell);
  };
  const size_t domainSizeBefore = domainMallocSize();

  size_t rmSizeBefore;
  size_t numBytecodeNodesBefore;
  {
    JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, false);
    ASSERT_TRUE(root != nullptr);
    const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
    const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));
    // The runtime's internal modules may already report entries of their
    // own; only this module's entry is new.
    numBytecodeNodesBefore =
        findNodesNamed("RegExpBytecode", nodes, strings).size();
    rmSizeBefore = FIND_NODE_FOR_ID(rmID, nodes, strings).selfSize;
  }

  auto call = [&](MutableHandle<JSRegExp> out) {
    CallResult<PseudoHandle<>> r =
        Callable::executeCall0(f, runtime, Runtime::getUndefinedValue());
    EXPECT_FALSE(isException(r));
    out = vmcast<JSRegExp>(r->get());
    return Handle<JSRegExp>(out);
  };

  Handle<JSRegExp> r0 = call(lv.r0);
  const RegExpBytecode *entry = r0->getBytecodeEntry();
  ASSERT_EQ(RegExpBytecode::Ownership::Cached, entry->ownership());
  auto regExpMallocSize = [](Handle<JSRegExp> r) {
    GCCell *cell = r.get();
    return cell->getVT()->getMallocSize(cell);
  };
  EXPECT_EQ(0u, regExpMallocSize(r0));
  const size_t sizeAfterFirst = rm->regExpCacheMemorySize();
  // The Domain reports the cache through its module.
  EXPECT_EQ(domainSizeBefore + sizeAfterFirst, domainMallocSize());

  Handle<JSRegExp> r1 = call(lv.r1);
  EXPECT_EQ(entry, r1->getBytecodeEntry());
  EXPECT_EQ(0u, regExpMallocSize(r1));
  EXPECT_EQ(domainSizeBefore + sizeAfterFirst, domainMallocSize());
  // The second evaluation adds nothing.
  EXPECT_EQ(sizeAfterFirst, rm->regExpCacheMemorySize());

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, false);
  ASSERT_TRUE(root != nullptr);
  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &edges = *llvh::cast<JSONArray>(root->at("edges"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  // This assumes the internal modules' nodes are stable between the two
  // snapshots: only f's module changes.
  // Exactly one new node for the shared bytecode, sized by its allocation.
  const auto entryID = runtime.getHeap().getNativeID(entry);
  std::vector<Node> bytecodeNodes =
      findNodesNamed("RegExpBytecode", nodes, strings);
  EXPECT_EQ(numBytecodeNodesBefore + 1, bytecodeNodes.size());
  auto entryNodeIt = std::find_if(
      bytecodeNodes.begin(), bytecodeNodes.end(), [&](const Node &n) {
        return n.id == entryID;
      });
  ASSERT_NE(bytecodeNodes.end(), entryNodeIt);
  EXPECT_EQ(entry->mallocSize(), entryNodeIt->selfSize);

  // The module node keeps its size and points at its cache vector. Every
  // module emits a vector node (the runtime's internal module too), so the
  // vector is found through this module's edge.
  auto rmNodeAndEdges = FIND_NODE_AND_EDGES_FOR_ID(rmID, nodes, edges, strings);
  EXPECT_EQ(rmSizeBefore, rmNodeAndEdges.first.selfSize);
  auto cacheEdgeIt = std::find_if(
      rmNodeAndEdges.second.begin(),
      rmNodeAndEdges.second.end(),
      [](const Edge &edge) {
        return edge.type == HeapSnapshot::EdgeType::Internal &&
            edge.name == "regExpCache";
      });
  ASSERT_NE(rmNodeAndEdges.second.end(), cacheEdgeIt);

  // The vector node: named, sized by its capacity, with an indexed edge
  // to the entry.
  auto vectorNodeAndEdges =
      FIND_NODE_AND_EDGES_FOR_ID(cacheEdgeIt->toNode, nodes, edges, strings);
  EXPECT_EQ("std::vector<RegExpBytecode *>", vectorNodeAndEdges.first.name);
  const size_t slotSize = sizeof(llvh::IntrusiveRefCntPtr<RegExpBytecode>);
  const size_t vectorBytes = vectorNodeAndEdges.first.selfSize;
  EXPECT_GE(vectorBytes, numRegExps * slotSize);
  EXPECT_EQ(0u, vectorBytes % slotSize);
  EXPECT_EQ(
      std::vector<Edge>{Edge(HeapSnapshot::EdgeType::Element, 0, entryID)},
      vectorNodeAndEdges.second);

  // The malloc estimate agrees with the snapshot: the vector plus the
  // entry.
  EXPECT_EQ(vectorBytes + entry->mallocSize(), sizeAfterFirst);
}

TEST_F(HeapSnapshotRuntimeTest, FunctionLocationAndNameTest) {
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  hbc::CompileFlags flags;
  // Make sure that debug info is emitted for this source file when it's
  // compiled.
  flags.debug = true;
  // Indent the function slightly to test that the source location is correct
  CallResult<HermesValue> res =
      runtime.run("\n  function foo() {}; foo;", "file:///fake.js", flags);
  ASSERT_FALSE(isException(res));
  Handle<JSFunction> func = runtime.makeHandle(vmcast<JSFunction>(*res));
  const auto funcID = runtime.getHeap().getObjectID(func.get());

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, true);
  ASSERT_TRUE(root != nullptr);

  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  // This test requires a location to be emitted.
  auto node = FIND_NODE_FOR_ID(funcID, nodes, strings);
  Node expected{
      HeapSnapshot::NodeType::Closure,
      "foo",
      funcID,
      func->getAllocatedSize(),
      firstNamedPropertyEdge<JSFunction>() + 3};
  EXPECT_EQ(node, expected);
  // Edges aren't tested in this test.

#ifdef HERMES_ENABLE_DEBUGGER
  // The location isn't emitted in fully optimized builds.
  const JSONArray &locations = *llvh::cast<JSONArray>(root->at("locations"));
  Location loc = FIND_LOCATION_FOR_ID(funcID, locations, nodes, strings);
  // The location should be the given file, second line, third column
  auto scriptId = func->getRuntimeModule()->getScriptID();
  EXPECT_EQ(loc, Location(expected, scriptId, 2, 3));
#else
  (void)findLocationForID;
#endif
}

TEST_F(HeapSnapshotRuntimeTest, FunctionDisplayNameTest) {
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  hbc::CompileFlags flags;
  // Make sure that debug info is emitted for this source file when it's
  // compiled.
  flags.debug = true;
  CallResult<HermesValue> res = runtime.run(
      R"(function foo() {}; foo.displayName = "bar"; foo;)",
      "file:///fake.js",
      flags);
  ASSERT_FALSE(isException(res));
  Handle<JSFunction> func = runtime.makeHandle(vmcast<JSFunction>(*res));
  const auto funcID = runtime.getHeap().getObjectID(func.get());

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, true);
  ASSERT_TRUE(root != nullptr);

  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  auto node = FIND_NODE_FOR_ID(funcID, nodes, strings);
  Node expected{
      HeapSnapshot::NodeType::Closure,
      // Make sure the name that is reported is "bar", not "foo".
      "bar",
      funcID,
      func->getAllocatedSize(),
      firstNamedPropertyEdge<JSFunction>() + 8};
  EXPECT_EQ(node, expected);
}

TEST_F(HeapSnapshotRuntimeTest, WeakMapTest) {
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  auto mapResult = JSWeakMap::create(
      runtime, Handle<JSObject>::vmcast(&runtime.weakMapPrototype));
  ASSERT_FALSE(isException(mapResult));
  Handle<JSWeakMap> map = runtime.makeHandle(std::move(*mapResult));
  Handle<JSObject> key = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> value = runtime.makeHandle(JSObject::create(runtime));
  // Add a key so the DenseMap will exist.
  JSWeakMap::setValue(map, runtime, key, value);

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, true);
  ASSERT_TRUE(root != nullptr);
  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &edges = *llvh::cast<JSONArray>(root->at("edges"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  const auto mapID = runtime.getHeap().getObjectID(map.get());
  auto nodesAndEdges = FIND_NODE_AND_EDGES_FOR_ID(mapID, nodes, edges, strings);
  auto firstNamed = firstNamedPropertyEdge<JSWeakMap>();
  EXPECT_EQ(
      nodesAndEdges.first,
      Node(
          HeapSnapshot::NodeType::Object,
          "JSWeakMap",
          mapID,
          map->getAllocatedSize(),
          firstNamed + 3));
  EXPECT_EQ(nodesAndEdges.second.size(), firstNamed + 3);

  // Test the native edge.
  const auto nativeMapID = map->getMapID(runtime.getHeap());
  EXPECT_EQ(
      nodesAndEdges.second[firstNamed],
      Edge(HeapSnapshot::EdgeType::Internal, "map", nativeMapID));
  EXPECT_EQ(
      FIND_NODE_FOR_ID(nativeMapID, nodes, strings),
      Node(
          HeapSnapshot::NodeType::Native,
          "DenseSet",
          nativeMapID,
          map->getMallocSize(),
          0));

  // Test the weak edge.
  EXPECT_EQ(
      nodesAndEdges.second[firstNamed + 1],
      Edge(
          HeapSnapshot::EdgeType::Weak,
          "0",
          runtime.getHeap().getObjectID(key.get())));
}

TEST_F(HeapSnapshotRuntimeTest, PropertyUpdatesTest) {
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  struct : Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<SymbolID> fooSym;
    PinnedValue<SymbolID> barSym;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.obj = JSObject::create(runtime);
  {
    vm::GCScope gcScope(runtime);
    lv.fooSym = vm::stringToSymbolIDForTest(
                    runtime, vm::createNoThrowForTest(runtime, "foo"))
                    ->getHermesValue()
                    .getSymbol();
    lv.barSym = vm::stringToSymbolIDForTest(
                    runtime, vm::createNoThrowForTest(runtime, "bar"))
                    ->getHermesValue()
                    .getSymbol();
  }
  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  // Add two properties to the hidden class chain.
  ASSERT_FALSE(isException(
      JSObject::defineOwnProperty(
          lv.obj,
          runtime,
          *lv.fooSym,
          dpf,
          runtime.makeHandle(HermesValue::encodeTrustedNumberValue(100)))));
  ASSERT_FALSE(isException(
      JSObject::defineOwnProperty(
          lv.obj,
          runtime,
          *lv.barSym,
          dpf,
          runtime.makeHandle(HermesValue::encodeTrustedNumberValue(200)))));
  // Trigger update transitions for both properties.
  dpf.writable = false;
  ASSERT_FALSE(isException(
      JSObject::defineOwnProperty(
          lv.obj,
          runtime,
          *lv.fooSym,
          dpf,
          runtime.makeHandle(HermesValue::encodeTrustedNumberValue(100)))));
  ASSERT_FALSE(isException(
      JSObject::defineOwnProperty(
          lv.obj,
          runtime,
          *lv.barSym,
          dpf,
          runtime.makeHandle(HermesValue::encodeTrustedNumberValue(200)))));
  // Forcibly clear the final hidden class's property map.
  auto *clazz = lv.obj->getClass(runtime);
  clazz->clearPropertyMap(runtime.getHeap());

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, true);
  ASSERT_TRUE(root != nullptr);
  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &edges = *llvh::cast<JSONArray>(root->at("edges"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  const auto objID = runtime.getHeap().getObjectID(lv.obj.get());
  auto nodesAndEdges = FIND_NODE_AND_EDGES_FOR_ID(objID, nodes, edges, strings);

  const auto FIRST_NAMED_PROPERTY_EDGE = firstNamedPropertyEdge<JSObject>();

  EXPECT_EQ(
      nodesAndEdges.first,
      Node(
          HeapSnapshot::NodeType::Object,
          "JSObject(foo, bar)",
          objID,
          lv.obj->getAllocatedSize(),
          FIRST_NAMED_PROPERTY_EDGE + 2));
  EXPECT_EQ(nodesAndEdges.second.size(), FIRST_NAMED_PROPERTY_EDGE + 2);

// When Handle-SAN is enabled, we put all numbers on the heap, which changes
// what the snapshot ID is.
#if HERMESVM_SANITIZE_HANDLES == 0
  EXPECT_EQ(
      nodesAndEdges.second[FIRST_NAMED_PROPERTY_EDGE],
      Edge(
          HeapSnapshot::EdgeType::Property,
          "foo",
          runtime.getHeap().getIDTracker().getNumberID(100)));

  EXPECT_EQ(
      nodesAndEdges.second[FIRST_NAMED_PROPERTY_EDGE + 1],
      Edge(
          HeapSnapshot::EdgeType::Property,
          "bar",
          runtime.getHeap().getIDTracker().getNumberID(200)));
#endif
}

TEST_F(HeapSnapshotRuntimeTest, ArrayElementsCaptureNumeric) {
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  hbc::CompileFlags flags;
  // Build an array that doesn't start at index 0.
  std::string source =
      "var a = []; a[10] = {}; a[15] = 222; a[(1 << 20) + 1000] = 333; a";
  CallResult<HermesValue> res = runtime.run(source, "file:///fake.js", flags);
  ASSERT_FALSE(isException(res));
  Handle<JSArray> array = runtime.makeHandle(vmcast<JSArray>(*res));
  Handle<JSObject> firstElement = runtime.makeHandle<JSObject>(
      vmcast<JSObject>(array->at(runtime, 10).getObject(runtime)));
  const auto arrayID = runtime.getHeap().getObjectID(array.get());

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, true);
  ASSERT_TRUE(root != nullptr);

  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &edges = *llvh::cast<JSONArray>(root->at("edges"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  const auto FIRST_NAMED_PROPERTY_EDGE = firstNamedPropertyEdge<JSArray>();
  auto nodeAndEdges =
      FIND_NODE_AND_EDGES_FOR_ID(arrayID, nodes, edges, strings);
  // The edges are:
  // name=parent
  // name=class
  // name=elements
  // name=directProp3
  // name=directProp4
  // 0. name=__proto__
  // 1. name=length
  // 2. index=1049576
  // 3. name=elements
  // 4. index=10
  // 5. index=15
  EXPECT_EQ(
      nodeAndEdges.first,
      Node(
          HeapSnapshot::NodeType::Object,
          "JSArray",
          arrayID,
          array->getAllocatedSize(),
          FIRST_NAMED_PROPERTY_EDGE + 6));
// When Handle-SAN is enabled, we put all numbers on the heap, which changes
// what the snapshot ID is.
#if HERMESVM_SANITIZE_HANDLES == 0
  EXPECT_EQ(
      nodeAndEdges.second[FIRST_NAMED_PROPERTY_EDGE + 2],
      Edge(
          HeapSnapshot::EdgeType::Element,
          (1 << 20) + 1000,
          runtime.getHeap().getIDTracker().getNumberID(333)));
#endif
  EXPECT_EQ(
      nodeAndEdges.second[FIRST_NAMED_PROPERTY_EDGE + 4],
      Edge(
          HeapSnapshot::EdgeType::Element,
          10,
          runtime.getHeap().getObjectID(firstElement.get())));
// When Handle-SAN is enabled, we put all numbers on the heap, which changes
// what the snapshot ID is.
#if HERMESVM_SANITIZE_HANDLES == 0
  EXPECT_EQ(
      nodeAndEdges.second[FIRST_NAMED_PROPERTY_EDGE + 5],
      Edge(
          HeapSnapshot::EdgeType::Element,
          15,
          runtime.getHeap().getIDTracker().getNumberID(222)));
#endif

  // Verify there are numeric nodes
  bool hasNumeric = false;
  auto nodesIt = nodes.begin();
  const auto nodesEnd = nodes.end();
  while (nodesIt != nodesEnd) {
    auto node = Node::parse(nodesIt, strings);
    if (node.type == HeapSnapshot::NodeType::Number) {
      hasNumeric = true;
    }
    nodesIt += HeapSnapshot::V8_SNAPSHOT_NODE_FIELD_COUNT;
  }
  EXPECT_TRUE(hasNumeric);
}

TEST_F(HeapSnapshotRuntimeTest, ArrayElementsNoNumeric) {
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  hbc::CompileFlags flags;
  // Build an array that doesn't start at index 0.
  std::string source =
      "var a = []; a[10] = {}; a[15] = 222; a[(1 << 20) + 1000] = 333; a";
  CallResult<HermesValue> res = runtime.run(source, "file:///fake.js", flags);
  ASSERT_FALSE(isException(res));
  Handle<JSArray> array = runtime.makeHandle(vmcast<JSArray>(*res));
  Handle<JSObject> firstElement = runtime.makeHandle<JSObject>(
      vmcast<JSObject>(array->at(runtime, 10).getObject(runtime)));
  auto cr = JSObject::getComputed_RJS(
      array,
      runtime,
      runtime.makeHandle(
          HermesValue::encodeTrustedNumberValue((1 << 20) + 1000)));
  ASSERT_FALSE(isException(cr));
  const auto arrayID = runtime.getHeap().getObjectID(array.get());

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, false);
  ASSERT_TRUE(root != nullptr);

  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &edges = *llvh::cast<JSONArray>(root->at("edges"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  const auto FIRST_NAMED_PROPERTY_EDGE = firstNamedPropertyEdge<JSArray>();
  auto nodeAndEdges =
      FIND_NODE_AND_EDGES_FOR_ID(arrayID, nodes, edges, strings);
  // The edges are:
  // name=parent
  // name=class
  // name=elements
  // name=directProp3
  // name=directProp4
  // 0. name=__proto__
  // 1. name=length
  // 2. index=1049576
  // 3. name=elements
  // 4. index=10
  // 5. index=15
  EXPECT_EQ(
      nodeAndEdges.first,
      Node(
          HeapSnapshot::NodeType::Object,
          "JSArray",
          arrayID,
          array->getAllocatedSize(),
          FIRST_NAMED_PROPERTY_EDGE + 6));

// When Handle-SAN is enabled, we put all numbers on the heap, which changes
// what the snapshot ID is.
#if HERMESVM_SANITIZE_HANDLES == 0
  EXPECT_EQ(
      nodeAndEdges.second[FIRST_NAMED_PROPERTY_EDGE + 2],
      Edge(
          HeapSnapshot::EdgeType::Element,
          (1 << 20) + 1000,
          GCBase::IDTracker::reserved(
              GCBase::IDTracker::ReservedObjectID::Number)));
#endif
  EXPECT_EQ(
      nodeAndEdges.second[FIRST_NAMED_PROPERTY_EDGE + 4],
      Edge(
          HeapSnapshot::EdgeType::Element,
          10,
          runtime.getHeap().getObjectID(firstElement.get())));
// When Handle-SAN is enabled, we put all numbers on the heap, which changes
// what the snapshot ID is.
#if HERMESVM_SANITIZE_HANDLES == 0
  EXPECT_EQ(
      nodeAndEdges.second[FIRST_NAMED_PROPERTY_EDGE + 5],
      Edge(
          HeapSnapshot::EdgeType::Element,
          15,
          GCBase::IDTracker::reserved(
              GCBase::IDTracker::ReservedObjectID::Number)));
#endif

  // Verify there are no numeric nodes
  auto nodesIt = nodes.begin();
  const auto nodesEnd = nodes.end();
  while (nodesIt != nodesEnd) {
    auto node = Node::parse(nodesIt, strings);
    EXPECT_NE(node.type, HeapSnapshot::NodeType::Number);
    nodesIt += HeapSnapshot::V8_SNAPSHOT_NODE_FIELD_COUNT;
  }
}

TEST_F(HeapSnapshotRuntimeTest, GetSnapshotIDThenCaptureNumeric) {
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  hbc::CompileFlags flags;
  // Build an array that doesn't start at index 0.
  std::string source =
      "var a = []; a[10] = {}; a[15] = 222; a[(1 << 20) + 1000] = 333; a";
  CallResult<HermesValue> res = runtime.run(source, "file:///fake.js", flags);
  ASSERT_FALSE(isException(res));
  Handle<JSArray> array = runtime.makeHandle(vmcast<JSArray>(*res));
  Handle<> secondElement =
      runtime.makeHandle(array->at(runtime, 15).unboxToHV(runtime));
  auto cr = JSObject::getComputed_RJS(
      array,
      runtime,
      runtime.makeHandle(
          HermesValue::encodeTrustedNumberValue((1 << 20) + 1000)));
  ASSERT_FALSE(isException(cr));

  // Take snapshot without numeric values
  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, false);
  ASSERT_TRUE(root != nullptr);

  // Obtain snapshot ID for 222. This process should allocate an ID for it, so
  // the next time we take a snapshot with numeric values, it should have the
  // same ID.
  llvh::Optional<HeapSnapshot::NodeID> nodeId =
      runtime.getHeap().getSnapshotID(*secondElement);
  ASSERT_TRUE(nodeId.hasValue());

  // Take snapshot with numeric values this time
  root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, true);
  ASSERT_TRUE(root != nullptr);

  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));

  // Expect that the snapshot will contain a node for the number 222, and that
  // node will use the same ID as what we got from getSnapshotID.
  auto node = FIND_NODE_FOR_ID(nodeId.getValue(), nodes, strings);
  EXPECT_EQ(node.type, HeapSnapshot::NodeType::Number);
}

#ifdef HERMES_ENABLE_DEBUGGER

static HeapSnapshot::NodeID findHighestNodeID(
    const JSONArray &nodes,
    const JSONArray &strings) {
  HeapSnapshot::NodeID maxID = GCBase::IDTracker::kInvalidNode;
  for (auto it = nodes.begin(), e = nodes.end(); it != e;
       it += HeapSnapshot::V8_SNAPSHOT_NODE_FIELD_COUNT) {
    auto node = Node::parse(it, strings);
    if (node.id > maxID) {
      maxID = node.id;
    }
  }
  return maxID;
}

static std::string functionInfoToString(
    int idx,
    const JSONArray &traceFunctionInfos,
    const JSONArray &strings) {
  auto base = idx * 6;
  int functionID = llvh::cast<JSONNumber>(traceFunctionInfos[base])->getValue();
  assert(
      functionID == idx &&
      "The function info must have a matching index and id");

  auto name = llvh::cast<JSONString>(
                  strings[llvh::cast<JSONNumber>(traceFunctionInfos[base + 1])
                              ->getValue()])
                  ->str();

  auto scriptName =
      llvh::cast<JSONString>(
          strings[llvh::cast<JSONNumber>(traceFunctionInfos[base + 2])
                      ->getValue()])
          ->str();

  auto scriptID =
      llvh::cast<JSONNumber>(traceFunctionInfos[base + 3])->getValue();
  auto line = llvh::cast<JSONNumber>(traceFunctionInfos[base + 4])->getValue();
  auto col = llvh::cast<JSONNumber>(traceFunctionInfos[base + 5])->getValue();

  return std::string(name) + "(" + std::to_string((int)functionID) + ") @ " +
      std::string(scriptName) + "(" + std::to_string((int)scriptID) +
      "):" + std::to_string((int)line) + ":" + std::to_string((int)col);
}

struct ChromeStackTreeNode {
  ChromeStackTreeNode(ChromeStackTreeNode *parent, int traceFunctionInfosId)
      : parent_(parent), traceFunctionInfosId_(traceFunctionInfosId) {}

  static std::vector<std::unique_ptr<ChromeStackTreeNode>> parse(
      const JSONArray &traceNodes,
      ChromeStackTreeNode *parent,
      std::map<int, ChromeStackTreeNode *> &idNodeMap) {
    std::vector<std::unique_ptr<ChromeStackTreeNode>> res;
    if (!parent) {
      assert(
          traceNodes.size() == 5 &&
          "Allocation trace should only have a"
          "single root node");
    }
    for (size_t i = 0; i < traceNodes.size(); i += 5) {
      auto id = llvh::cast<JSONNumber>(traceNodes[i])->getValue();
      auto functionInfoIndex =
          llvh::cast<JSONNumber>(traceNodes[i + 1])->getValue();
      auto children = llvh::cast<JSONArray>(traceNodes[i + 4]);
      auto treeNode =
          std::make_unique<ChromeStackTreeNode>(parent, functionInfoIndex);
      idNodeMap.emplace(id, treeNode.get());
      treeNode->children_ = parse(*children, treeNode.get(), idNodeMap);
      res.emplace_back(std::move(treeNode));
    }
    return res;
  };

  std::string buildStackTrace(
      const JSONArray &traceFunctionInfos,
      const JSONArray &strings) {
    std::string res =
        parent_ ? parent_->buildStackTrace(traceFunctionInfos, strings) : "";
    res += "\n" +
        functionInfoToString(
               traceFunctionInfosId_, traceFunctionInfos, strings);
    return res;
  };

 private:
  ChromeStackTreeNode *parent_;
  int traceFunctionInfosId_;
  std::vector<std::unique_ptr<ChromeStackTreeNode>> children_;
};

TEST_F(HeapSnapshotRuntimeTest, AllocationTraces) {
  runtime.enableAllocationLocationTracker();
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  hbc::CompileFlags flags;
  CallResult<HermesValue> res = runtime.run(
      R"#(
function foo() {
  return new Object();
}
function bar() {
  return new Object();
}
function baz() {
  return {foo: foo(), bar: bar()};
}
baz();
      )#",
      "test.js",
      flags);
  ASSERT_FALSE(isException(res));
  ASSERT_TRUE(res->isObject());
  Handle<JSObject> resObj = runtime.makeHandle(vmcast<JSObject>(*res));
  SymbolID fooSym, barSym;
  {
    vm::GCScope gcScope(runtime);
    fooSym = vm::stringToSymbolIDForTest(
                 runtime, vm::createNoThrowForTest(runtime, "foo"))
                 ->getHermesValue()
                 .getSymbol();
    barSym = vm::stringToSymbolIDForTest(
                 runtime, vm::createNoThrowForTest(runtime, "bar"))
                 ->getHermesValue()
                 .getSymbol();
  }
  auto fooObj = JSObject::getNamed_RJS(resObj, runtime, fooSym);
  auto barObj = JSObject::getNamed_RJS(resObj, runtime, barSym);
  auto fooObjID =
      runtime.getHeap().getObjectID(vmcast<JSObject>(fooObj->get()));
  auto barObjID =
      runtime.getHeap().getObjectID(vmcast<JSObject>(barObj->get()));

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, true);
  ASSERT_TRUE(root != nullptr);

  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));
  const JSONArray &traceFunctionInfos =
      *llvh::cast<JSONArray>(root->at("trace_function_infos"));

  std::map<int, ChromeStackTreeNode *> idNodeMap;
  auto roots = ChromeStackTreeNode::parse(
      *llvh::cast<JSONArray>(root->at("trace_tree")), nullptr, idNodeMap);

  auto fooAllocNode = FIND_NODE_FOR_ID(fooObjID, nodes, strings);
  auto fooStackTreeNode = idNodeMap.find(fooAllocNode.traceNodeID);
  ASSERT_NE(fooStackTreeNode, idNodeMap.end());
  auto fooStackStr =
      fooStackTreeNode->second->buildStackTrace(traceFunctionInfos, strings);
  EXPECT_STREQ(
      fooStackStr.c_str(),
      R"#(
(root)(0) @ (0):0:0
global(1) @ test.js(2):2:1
global(2) @ test.js(2):11:4
baz(7) @ test.js(2):9:19
foo(8) @ test.js(2):3:20)#");

  auto barAllocNode = FIND_NODE_FOR_ID(barObjID, nodes, strings);
  auto barStackTreeNode = idNodeMap.find(barAllocNode.traceNodeID);
  ASSERT_NE(barStackTreeNode, idNodeMap.end());
  auto barStackStr =
      barStackTreeNode->second->buildStackTrace(traceFunctionInfos, strings);
  ASSERT_STREQ(
      barStackStr.c_str(),
      R"#(
(root)(0) @ (0):0:0
global(1) @ test.js(2):2:1
global(2) @ test.js(2):11:4
baz(3) @ test.js(2):9:31
bar(4) @ test.js(2):6:20)#");

  const JSONArray &samples = *llvh::cast<JSONArray>(root->at("samples"));
  // Must have at least one sample
  EXPECT_GT(samples.size(), 0u);
  for (auto it = samples.begin(), e = samples.end(); it != e;
       it += HeapSnapshot::V8_SNAPSHOT_SAMPLE_FIELD_COUNT) {
    auto sample = Sample::parse(it);
    EXPECT_NE(sample.lastSeenObjectID, toRValue(GC::IDTracker::kInvalidNode));
    if (it != samples.begin()) {
      auto prevSample =
          Sample::parse(it - HeapSnapshot::V8_SNAPSHOT_SAMPLE_FIELD_COUNT);
      EXPECT_GT(sample.timestamp, prevSample.timestamp);
      EXPECT_GT(sample.lastSeenObjectID, prevSample.lastSeenObjectID);
    }
  }
  auto highestNodeID = findHighestNodeID(nodes, strings);
  auto lastSample = Sample::parse(
      samples.end() - HeapSnapshot::V8_SNAPSHOT_SAMPLE_FIELD_COUNT);
  EXPECT_GE(lastSample.lastSeenObjectID, highestNodeID);
}

TEST_F(HeapSnapshotRuntimeTest, TwoPathsToFunction) {
  runtime.enableAllocationLocationTracker();
  JSONFactory::Allocator alloc;
  JSONFactory jsonFactory{alloc};
  hbc::CompileFlags flags;
  CallResult<HermesValue> res = runtime.run(
      R"#(
var objects = [];
function A() {
  B(A);
}
function B(allocatingFunction) {
  objects.push({ AllocatingFunction: allocatingFunction });
}
function C() {
  B(C);
}
function D() {
  B(D);
}
A();
C();
D();
objects[0];
      )#",
      "test.js",
      flags);
  ASSERT_FALSE(isException(res));
  ASSERT_TRUE(res->isObject());
  Handle<JSObject> obj = runtime.makeHandle(vmcast<JSObject>(*res));
  auto objID = runtime.getHeap().getObjectID(*obj);

  JSONObject *root = TAKE_SNAPSHOT(runtime.getHeap(), jsonFactory, true);
  ASSERT_TRUE(root != nullptr);

  const JSONArray &nodes = *llvh::cast<JSONArray>(root->at("nodes"));
  const JSONArray &strings = *llvh::cast<JSONArray>(root->at("strings"));
  const JSONArray &traceFunctionInfos =
      *llvh::cast<JSONArray>(root->at("trace_function_infos"));

  std::map<int, ChromeStackTreeNode *> idNodeMap;
  auto roots = ChromeStackTreeNode::parse(
      *llvh::cast<JSONArray>(root->at("trace_tree")), nullptr, idNodeMap);

  auto allocNode = FIND_NODE_FOR_ID(objID, nodes, strings);
  auto stackTreeNode = idNodeMap.find(allocNode.traceNodeID);
  ASSERT_NE(stackTreeNode, idNodeMap.end());
  auto stackStr =
      stackTreeNode->second->buildStackTrace(traceFunctionInfos, strings);
  EXPECT_STREQ(
      stackStr.c_str(),
      R"#(
(root)(0) @ (0):0:0
global(1) @ test.js(2):2:1
global(11) @ test.js(2):15:2
A(12) @ test.js(2):4:4
B(4) @ test.js(2):7:15)#");
}

#endif // HERMES_ENABLE_DEBUGGER

} // namespace heapsnapshottest
} // namespace unittest
} // namespace hermes

#endif // HERMES_MEMORY_INSTRUMENTATION
