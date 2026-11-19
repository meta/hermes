/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/SynthTraceParser.h>

#include <gtest/gtest.h>

#include <sstream>

namespace {

using namespace facebook::hermes::tracing;

struct SynthTraceParserTest : public ::testing::Test {
  std::unique_ptr<llvh::MemoryBuffer> bufFromStr(const std::string &str) {
    llvh::StringRef ref{str.data(), str.size()};
    return llvh::MemoryBuffer::getMemBufferCopy(ref);
  }
};

TEST_F(SynthTraceParserTest, ParseHeader) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 258,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912,
      "occupancyTarget": 0.75,
      "effectiveOOMThreshold": 20,
      "shouldReleaseUnused": "none",
      "name": "foo",
      "allocInYoung": false,
    },
    "maxNumRegisters": 100,
    "ES6Proxy": false,
    "Intl": false,
    "enableSampledStats": true,
    "vmExperimentFlags": 123
  },
  "trace": []
}
)";
  auto result = parseSynthTrace(bufFromStr(src));
  const SynthTrace &trace = std::get<0>(result);
  hermes::vm::RuntimeConfig rtconf =
      std::get<1>(result).withGCConfig(std::get<2>(result).build()).build();

  EXPECT_EQ(trace.records().size(), 0);

  const ::hermes::vm::GCConfig &gcconf = rtconf.getGCConfig();
  EXPECT_EQ(gcconf.getInitHeapSize(), 33554432);
  EXPECT_EQ(gcconf.getMaxHeapSize(), 536870912);
  EXPECT_EQ(gcconf.getOccupancyTarget(), 0.75);
  EXPECT_EQ(gcconf.getEffectiveOOMThreshold(), 20);
  EXPECT_FALSE(gcconf.getShouldReleaseUnused());
  EXPECT_EQ(gcconf.getName(), "foo");
  EXPECT_FALSE(gcconf.getAllocInYoung());

  EXPECT_EQ(rtconf.getMaxNumRegisters(), 100);
  EXPECT_FALSE(rtconf.getES6Proxy());
  EXPECT_FALSE(rtconf.getIntl());
  EXPECT_TRUE(rtconf.getEnableSampledStats());
  EXPECT_EQ(rtconf.getVMExperimentFlags(), 123);
}

TEST_F(SynthTraceParserTest, RuntimeConfigDefaults) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 258,
  "runtimeConfig": {},
  "trace": []
}
  )";
  auto result = parseSynthTrace(bufFromStr(src));
  hermes::vm::RuntimeConfig rtconf = std::get<1>(result).build();

  EXPECT_EQ(rtconf.getGCConfig().getInitHeapSize(), 33554432);
  EXPECT_EQ(rtconf.getGCConfig().getMaxHeapSize(), 3221225472);
  EXPECT_FALSE(rtconf.getEnableSampledStats());
}

TEST_F(SynthTraceParserTest, SynthVersionMismatch) {
  const char *src = R"(
{
  "version": 0,
  "globalObjID": 258,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912
    }
  },
  "trace": []
}
  )";
  EXPECT_DEATH_IF_SUPPORTED(parseSynthTrace(bufFromStr(src)), "");
}

TEST_F(SynthTraceParserTest, ParsePropID) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 258,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912
    }
  },
  "trace": [
    {
      "type": "GetPropertyRecord",
      "time": 0,
      "objID": 0,
      "propID": "propNameID:111",
      "propName": "prop111",
      "value": "number:0x0"
    },
    {
      "type": "GetPropertyRecord",
      "time": 0,
      "objID": 0,
      "propID": "string:222",
      "propName": "prop222",
      "value": "number:0x0"
    }
  ]
}
  )";
  auto parseResult = parseSynthTrace(bufFromStr(src));
  SynthTrace &trace = std::get<0>(parseResult);

  auto record0 = dynamic_cast<const SynthTrace::GetPropertyRecord &>(
      *trace.records().at(0));
  ASSERT_EQ(SynthTrace::encodePropNameID(111), record0.propID_);

  auto record1 = dynamic_cast<const SynthTrace::GetPropertyRecord &>(
      *trace.records().at(1));
  ASSERT_EQ(SynthTrace::encodeString(222), record1.propID_);
}

TEST_F(SynthTraceParserTest, SynthVersionInvalidKind) {
  const char *src = R"(
{
  "version": true,
  "globalObjID": 258,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912
    }
  },
  "trace": []
}
  )";
  EXPECT_DEATH_IF_SUPPORTED(parseSynthTrace(bufFromStr(src)), "");
}

TEST_F(SynthTraceParserTest, SynthMissingVersion) {
  const char *src = R"(
{
  "globalObjID": 258,
  "runtimeConfig": {
  },
  "trace": []
}
  )";
  parseSynthTrace(bufFromStr(src));
}

TEST_F(SynthTraceParserTest, CreateBigIntRecord) {
  const char *src = R"(
{
  "globalObjID": 258,
  "runtimeConfig": {
  },
  "trace": [
    {
      "type": "CreateBigIntRecord",
      "time": 0,
      "objID": 0,
      "method": "FromInt64",
      "bits": "999999999999"
    },
    {
      "type": "CreateBigIntRecord",
      "time": 0,
      "objID": 0,
      "method": "FromUint64",
      "bits": "123456789"
    }
  ]
}
  )";
  parseSynthTrace(bufFromStr(src));
}

TEST_F(SynthTraceParserTest, BigIntToStringRecord) {
  const char *src = R"(
{
  "globalObjID": 258,
  "runtimeConfig": {
  },
  "trace": [
    {
      "type": "BigIntToStringRecord",
      "time": 0,
      "strID": 0,
      "bigintID": 0,
      "radix": 20
    }
  ]
}
  )";
  parseSynthTrace(bufFromStr(src));
}

TEST_F(SynthTraceParserTest, ParseUtf8Record) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 258,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912
    }
  },
  "trace": [
    {
      "type": "Utf8Record",
      "time": 1234,
      "objID": "string:1110",
      "retval": "hi"
    },
    {
      "type": "Utf8Record",
      "time": 1234,
      "objID": "string:1112",
      "retval": "\u00ed\u00a0\u00bd"
    },
    {
      "type": "Utf8Record",
      "time": 1234,
      "objID": "string:1113",
      "retval": "nice\u00f0\u009f\u0091\u008d"
    }
  ]
}
  )";
  auto parseResult = parseSynthTrace(bufFromStr(src));
  SynthTrace &trace = std::get<0>(parseResult);

  auto record0 =
      dynamic_cast<const SynthTrace::Utf8Record &>(*trace.records().at(0));
  ASSERT_EQ(record0.retVal_, "hi");

  // This is testing that SynthTraceParser is able to parse invalid UTF-8
  auto record1 =
      dynamic_cast<const SynthTrace::Utf8Record &>(*trace.records().at(1));
  ASSERT_EQ(record1.retVal_, "\xed\xa0\xbd");

  auto record2 =
      dynamic_cast<const SynthTrace::Utf8Record &>(*trace.records().at(2));
  ASSERT_EQ(record2.retVal_, "nice👍");
}

TEST_F(SynthTraceParserTest, ParseMathCallRecord) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 258,
  "runtimeConfig": {},
  "trace": [
    {
      "type": "MathCallRecord",
      "time": 1234,
      "function": "pow",
      "args": ["number:0x4000000000000000", "number:0x4024000000000000"],
      "retval": "number:0x4090000000000000"
    }
  ]
}
  )";
  auto parseResult = parseSynthTrace(bufFromStr(src));
  SynthTrace &trace = std::get<0>(parseResult);

  auto record =
      dynamic_cast<const SynthTrace::MathCallRecord &>(*trace.records().at(0));
  EXPECT_EQ("pow", record.function_);
  EXPECT_EQ((std::vector<double>{2, 10}), record.args_);
  EXPECT_EQ(1024, record.retVal_);
}

TEST_F(SynthTraceParserTest, ParseUtf16Record) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 258,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912
    }
  },
  "trace": [
    {
      "type": "Utf16Record",
      "time": 1234,
      "objID": "string:1110",
      "retval": "hi"
    },
    {
      "type": "Utf16Record",
      "time": 1234,
      "objID": "string:1111",
      "retval": "\ud83d"
    },
    {
      "type": "Utf16Record",
      "time": 1234,
      "objID": "string:1112",
      "retval": "nice\ud83d\udc4d"
    }
  ]
}
  )";
  auto parseResult = parseSynthTrace(bufFromStr(src));
  SynthTrace &trace = std::get<0>(parseResult);

  auto record0 =
      dynamic_cast<const SynthTrace::Utf16Record &>(*trace.records().at(0));
  ASSERT_EQ(record0.retVal_, u"hi");

  // We should be able to parse a lone surrogate
  auto record1 =
      dynamic_cast<const SynthTrace::Utf16Record &>(*trace.records().at(1));
  ASSERT_EQ(record1.retVal_, u"\xd83d");

  auto record2 =
      dynamic_cast<const SynthTrace::Utf16Record &>(*trace.records().at(2));
  ASSERT_EQ(record2.retVal_, u"nice\xd83d\xdc4d");
}

TEST_F(SynthTraceParserTest, ParseGetStringDataRecord) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 258,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912
    }
  },
  "trace": [
    {
      "type": "GetStringDataRecord",
      "time": 1234,
      "objID": "string:1110",
      "strData": "\nhello\ud83d\udc4b\\"
    },
    {
      "type": "GetStringDataRecord",
      "time": 1234,
      "objID": "propNameID:1111",
      "strData": "\ud83d"
    }
  ]
}
  )";
  auto parseResult = parseSynthTrace(bufFromStr(src));
  SynthTrace &trace = std::get<0>(parseResult);

  auto record0 = dynamic_cast<const SynthTrace::GetStringDataRecord &>(
      *trace.records().at(0));
  ASSERT_EQ(record0.strData_, u"\nhello\xd83d\xdc4b\\");
  ASSERT_EQ(record0.objID_, SynthTrace::encodeString(1110));

  auto record1 = dynamic_cast<const SynthTrace::GetStringDataRecord &>(
      *trace.records().at(1));
  ASSERT_EQ(record1.strData_, u"\xd83d");
  ASSERT_EQ(record1.objID_, SynthTrace::encodePropNameID(1111));
}

TEST_F(SynthTraceParserTest, ParseSetAndGetPrototypeRecord) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 258,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912
    }
  },
  "trace": [
    {
      "type": "SetPrototypeRecord",
      "time": 1234,
      "objID": 1,
      "value": "null:"
    },
    {
      "type": "SetPrototypeRecord",
      "time": 12,
      "objID": 2,
      "value": "object:1"
    },
    {
      "type": "GetPrototypeRecord",
      "time": 123,
      "objID": 1
    },
    {
      "type": "GetPrototypeRecord",
      "time": 1234,
      "objID": 2
    },
  ]
}
  )";
  auto parseResult = parseSynthTrace(bufFromStr(src));
  SynthTrace &trace = std::get<0>(parseResult);

  auto record0 = dynamic_cast<const SynthTrace::SetPrototypeRecord &>(
      *trace.records().at(0));
  ASSERT_EQ(record0.objID_, 1);
  ASSERT_EQ(record0.value_, SynthTrace::encodeNull());

  auto record1 = dynamic_cast<const SynthTrace::SetPrototypeRecord &>(
      *trace.records().at(1));
  ASSERT_EQ(record1.objID_, 2);
  ASSERT_EQ(record1.value_, SynthTrace::encodeObject(1));

  auto record2 = dynamic_cast<const SynthTrace::GetPrototypeRecord &>(
      *trace.records().at(2));
  ASSERT_EQ(record2.objID_, 1);

  auto record3 = dynamic_cast<const SynthTrace::GetPrototypeRecord &>(
      *trace.records().at(3));
  ASSERT_EQ(record3.objID_, 2);
}

TEST_F(SynthTraceParserTest, ParseCreateObjectWithPrototypeRecord) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 258,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912
    }
  },
  "trace": [
    {
      "type": "CreateObjectWithPrototypeRecord",
      "time": 1234,
      "objID": 1,
      "prototype": "null:"
    },
    {
      "type": "CreateObjectWithPrototypeRecord",
      "time": 12345,
      "objID": 2,
      "prototype": "object:1"
    }
  ]
}
  )";
  auto parseResult = parseSynthTrace(bufFromStr(src));
  SynthTrace &trace = std::get<0>(parseResult);

  auto record0 =
      dynamic_cast<const SynthTrace::CreateObjectWithPrototypeRecord &>(
          *trace.records().at(0));
  ASSERT_EQ(record0.objID_, 1);
  ASSERT_EQ(record0.prototype_, SynthTrace::encodeNull());

  auto record1 =
      dynamic_cast<const SynthTrace::CreateObjectWithPrototypeRecord &>(
          *trace.records().at(1));
  ASSERT_EQ(record1.objID_, 2);
  ASSERT_EQ(record1.prototype_, SynthTrace::encodeObject(1));
}

TEST_F(SynthTraceParserTest, ParseDeletePropertyRecord) {
  const char *src = R"(
{
  "version": 5,
  "globalObjID": 100,
  "runtimeConfig": {
    "gcConfig": {
      "initHeapSize": 33554432,
      "maxHeapSize": 536870912
    }
  },
  "trace": [
    {
      "type": "DeletePropertyRecord",
      "time": 101,
      "objID": 1,
      "propID": "string:123"
    },
    {
      "type": "DeletePropertyRecord",
      "time": 102,
      "objID": 2,
      "propID": "propNameID:456"
    },
    {
      "type": "DeletePropertyRecord",
      "time": 103,
      "objID": 2,
      "propID": "number:0x0"
    },
  ]
}
  )";

  auto parseResult = parseSynthTrace(bufFromStr(src));
  SynthTrace &trace = std::get<0>(parseResult);

  auto record0 = dynamic_cast<const SynthTrace::DeletePropertyRecord &>(
      *trace.records().at(0));
  EXPECT_EQ(record0.objID_, 1);
  EXPECT_EQ(record0.propID_, SynthTrace::encodeString(123));

  auto record1 = dynamic_cast<const SynthTrace::DeletePropertyRecord &>(
      *trace.records().at(1));
  EXPECT_EQ(record1.objID_, 2);
  EXPECT_EQ(record1.propID_, SynthTrace::encodePropNameID(456));

  auto record3 = dynamic_cast<const SynthTrace::DeletePropertyRecord &>(
      *trace.records().at(2));
  EXPECT_EQ(record3.objID_, 2);
  EXPECT_EQ(record3.propID_, SynthTrace::encodeNumber(0));
}

} // namespace
