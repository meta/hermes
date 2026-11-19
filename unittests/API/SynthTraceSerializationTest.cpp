/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/SynthTrace.h>

#include <hermes/Support/Algorithms.h>
#include <hermes/TraceInterpreter.h>
#include <hermes/TracingRuntime.h>

#include <gtest/gtest.h>

#include <hermes/hermes.h>

#include <limits>
#include <memory>

using namespace facebook::hermes::tracing;
using namespace facebook::hermes;
namespace jsi = facebook::jsi;

namespace {

class SynthTraceSerializationTest : public ::testing::Test {
 protected:
  SynthTrace::TimeSinceStart dummyTime{SynthTrace::TimeSinceStart::zero()};

  std::string to_string(const SynthTrace::Record &rec) {
    std::string result;
    llvh::raw_string_ostream resultStream{result};
    ::hermes::JSONEmitter json{resultStream};
    rec.toJSON(json);
    resultStream.flush();
    return result;
  }

 private:
  std::string traceFilename_;
};

TEST_F(SynthTraceSerializationTest, EncodeNumber) {
  EXPECT_EQ(
      "number:0x3ff0000000000000",
      SynthTrace::encode(SynthTrace::encodeNumber(1)));
}

TEST_F(SynthTraceSerializationTest, EncodeNaN) {
  EXPECT_EQ(
      "number:0x7ff8000000000000",
      SynthTrace::encode(
          SynthTrace::encodeNumber(std::numeric_limits<double>::quiet_NaN())));
}

TEST_F(SynthTraceSerializationTest, EncodeInfinity) {
  EXPECT_EQ(
      "number:0x7ff0000000000000",
      SynthTrace::encode(
          SynthTrace::encodeNumber(std::numeric_limits<double>::infinity())));
}

TEST_F(SynthTraceSerializationTest, EncodeNegativeInfinity) {
  EXPECT_EQ(
      "number:0xfff0000000000000",
      SynthTrace::encode(
          SynthTrace::encodeNumber(-std::numeric_limits<double>::infinity())));
}

TEST_F(SynthTraceSerializationTest, EncodeReallyBigInteger) {
  EXPECT_EQ(
      "number:0x423d2729eec71f97",
      SynthTrace::encode(SynthTrace::encodeNumber(125211111111.1234)));
}

TEST_F(SynthTraceSerializationTest, EncodeString) {
  const SynthTrace::ObjectID stringId = 1111;
  EXPECT_EQ(
      std::string("string:") + std::to_string(stringId),
      SynthTrace::encode(SynthTrace::encodeString(stringId)));
}

TEST_F(SynthTraceSerializationTest, CallNoArgs) {
  EXPECT_EQ(
      R"({"type":"CallFromNativeRecord","time":0,"functionID":1,"thisArg":"undefined:","args":[]})",
      to_string(
          SynthTrace::CallFromNativeRecord(
              dummyTime, 1, SynthTrace::encodeUndefined(), {})));
}

TEST_F(SynthTraceSerializationTest, Call) {
  EXPECT_EQ(
      R"({"type":"CallFromNativeRecord","time":0,"functionID":1,"thisArg":"object:1","args":["undefined:","bool:true"]})",
      to_string(
          SynthTrace::CallFromNativeRecord(
              dummyTime,
              1,
              SynthTrace::encodeObject(1),
              {SynthTrace::encodeUndefined(), SynthTrace::encodeBool(true)})));
}

TEST_F(SynthTraceSerializationTest, Construct) {
  EXPECT_EQ(
      R"({"type":"ConstructFromNativeRecord","time":0,"functionID":1,"thisArg":"undefined:","args":["null:"]})",
      to_string(
          SynthTrace::ConstructFromNativeRecord(
              dummyTime,
              1,
              SynthTrace::encodeUndefined(),
              {SynthTrace::encodeNull()})));
}

TEST_F(SynthTraceSerializationTest, Return) {
  EXPECT_EQ(
      R"({"type":"ReturnFromNativeRecord","time":0,"retval":"bool:true"})",
      to_string(
          SynthTrace::ReturnFromNativeRecord(
              dummyTime, SynthTrace::encodeBool(true))));
  EXPECT_EQ(
      R"({"type":"ReturnToNativeRecord","time":0,"retval":"bool:false"})",
      to_string(
          SynthTrace::ReturnToNativeRecord(
              dummyTime, SynthTrace::encodeBool(false))));
}

TEST_F(SynthTraceSerializationTest, MathCall) {
  EXPECT_EQ(
      R"({"type":"MathCallRecord","time":0,"function":"pow","args":["number:0x4000000000000000","number:0x4024000000000000"],"retval":"number:0x4090000000000000"})",
      to_string(SynthTrace::MathCallRecord(dummyTime, "pow", {2, 10}, 1024)));
}

TEST_F(SynthTraceSerializationTest, ReturnEncodeUTF8String) {
  EXPECT_EQ(
      R"({"type":"ReturnFromNativeRecord","time":0,"retval":"string:1111"})",
      to_string(
          SynthTrace::ReturnFromNativeRecord{
              dummyTime, SynthTrace::encodeString(1111)}));
}

TEST_F(SynthTraceSerializationTest, GetProperty) {
  const std::string ex =
      std::string(
          R"({"type":"GetPropertyRecord","time":0,"objID":1,"propID":"propNameID:1111")") +
#ifdef HERMESVM_API_TRACE_DEBUG
      R"(,"propName":"x")" +
#endif
      R"(})";
  auto testRec = SynthTrace::GetPropertyRecord(
      dummyTime,
      1,
      SynthTrace::encodePropNameID(1111)
#ifdef HERMESVM_API_TRACE_DEBUG
          ,
      "x"
#endif
  );
  EXPECT_EQ(ex, to_string(testRec));
}

TEST_F(SynthTraceSerializationTest, SetProperty) {
  const std::string ex =
      std::string(
          R"({"type":"SetPropertyRecord","time":0,"objID":1,"propID":"string:1111",)") +
#ifdef HERMESVM_API_TRACE_DEBUG
      R"("propName":"x",)" +
#endif
      R"("value":"string:1112"})";
  auto testRec = SynthTrace::SetPropertyRecord(
      dummyTime,
      1,
      SynthTrace::encodeString(1111),
#ifdef HERMESVM_API_TRACE_DEBUG
      "x",
#endif
      SynthTrace::encodeString(1112));
  EXPECT_EQ(ex, to_string(testRec));
}

TEST_F(SynthTraceSerializationTest, HasProperty) {
  const std::string ex =
      std::string(
          R"({"type":"HasPropertyRecord","time":0,"objID":1,"propID":"string:1111")") +
#ifdef HERMESVM_API_TRACE_DEBUG
      R"(,"propName":"a")" +
#endif
      "}";
  auto testRec = SynthTrace::HasPropertyRecord(
      dummyTime,
      1,
      SynthTrace::encodeString(1111)
#ifdef HERMESVM_API_TRACE_DEBUG
          ,
      "a"
#endif
  );
  EXPECT_EQ(ex, to_string(testRec));
}

TEST_F(SynthTraceSerializationTest, GetPropertyNames) {
  EXPECT_EQ(
      R"({"type":"GetPropertyNamesRecord","time":0,"objID":1})",
      to_string(SynthTrace::GetPropertyNamesRecord(dummyTime, 1)));
}

TEST_F(SynthTraceSerializationTest, CreateArray) {
  EXPECT_EQ(
      R"({"type":"CreateArrayRecord","time":0,"objID":1,"length":10})",
      to_string(SynthTrace::CreateArrayRecord(dummyTime, 1, 10)));
}

TEST_F(SynthTraceSerializationTest, ArrayWrite) {
  EXPECT_EQ(
      R"({"type":"ArrayWriteRecord","time":0,"objID":1,"index":0,"value":"string:1111"})",
      to_string(
          SynthTrace::ArrayWriteRecord(
              dummyTime, 1, 0, SynthTrace::encodeString(1111))));
}

TEST_F(SynthTraceSerializationTest, MarkerRecord) {
  EXPECT_EQ(
      R"({"type":"MarkerRecord","time":0,"tag":"foo"})",
      to_string(SynthTrace::MarkerRecord(dummyTime, "foo")));
}

TEST_F(SynthTraceSerializationTest, GetPropertyNative) {
  EXPECT_EQ(
      R"({"type":"GetPropertyNativeRecord","time":0,"hostObjectID":1,"propNameID":100,"propName":"foo"})",
      to_string(SynthTrace::GetPropertyNativeRecord(dummyTime, 1, 100, "foo")));
  EXPECT_EQ(
      R"({"type":"GetPropertyNativeReturnRecord","time":0,"retval":"null:"})",
      to_string(
          SynthTrace::GetPropertyNativeReturnRecord(
              dummyTime, SynthTrace::encodeNull())));
}

TEST_F(SynthTraceSerializationTest, SetPropertyNative) {
  EXPECT_EQ(
      R"({"type":"SetPropertyNativeRecord","time":0,"hostObjectID":1,"propNameID":100,"propName":"foo","value":"string:1111"})",
      to_string(
          SynthTrace::SetPropertyNativeRecord(
              dummyTime, 1, 100, "foo", SynthTrace::encodeString(1111))));
}

TEST_F(SynthTraceSerializationTest, SetPropertyNativeReturn) {
  EXPECT_EQ(
      R"({"type":"SetPropertyNativeReturnRecord","time":0})",
      to_string(SynthTrace::SetPropertyNativeReturnRecord(dummyTime)));
}

TEST_F(SynthTraceSerializationTest, TimeIsPrinted) {
  hermes::SHA1 hash{{0x64, 0x40, 0xb5, 0x37, 0xaf, 0x26, 0x79,
                     0x5e, 0x5f, 0x45, 0x2b, 0xcd, 0x32, 0x0f,
                     0xac, 0xcb, 0x02, 0x05, 0x5a, 0x4f}};
  // JSON emitters escape forward slashes.
  EXPECT_EQ(
      R"({"type":"BeginExecJSRecord","time":100,"sourceURL":"file:\/\/\/file.js","sourceHash":"6440b537af26795e5f452bcd320faccb02055a4f","sourceIsBytecode":false})",
      to_string(
          SynthTrace::BeginExecJSRecord(
              std::chrono::milliseconds(100), "file:///file.js", hash, false)));
}

TEST_F(SynthTraceSerializationTest, EndExecHasRetval) {
  EXPECT_EQ(
      R"({"type":"EndExecJSRecord","time":0,"tag":"end_global_code","retval":"null:"})",
      to_string(
          SynthTrace::EndExecJSRecord(dummyTime, SynthTrace::encodeNull())));
}

TEST_F(SynthTraceSerializationTest, TraceHeader) {
  std::string result;
  auto resultStream = std::make_unique<llvh::raw_string_ostream>(result);
  const ::hermes::vm::RuntimeConfig conf;
  auto *hermesRoot = jsi::castInterface<IHermesRootAPI>(makeHermesRootAPI());
  std::unique_ptr<TracingHermesRuntime> rt(makeTracingHermesRuntime(
      hermesRoot->makeHermesRuntime(conf), conf, std::move(resultStream)));

  rt->flushAndDisableTrace();

  auto optTrace = rt->global()
                      .getPropertyAsObject(*rt, "JSON")
                      .getPropertyAsFunction(*rt, "parse")
                      .call(*rt, result)
                      .asObject(*rt);

  EXPECT_EQ(
      SynthTrace::synthVersion(),
      optTrace.getProperty(*rt, "version").asNumber());

  auto rtConfig = optTrace.getPropertyAsObject(*rt, "runtimeConfig");

  auto gcConfig = rtConfig.getPropertyAsObject(*rt, "gcConfig");
  EXPECT_EQ(
      conf.getGCConfig().getInitHeapSize(),
      gcConfig.getProperty(*rt, "initHeapSize").asNumber());
  EXPECT_EQ(
      conf.getGCConfig().getMaxHeapSize(),
      gcConfig.getProperty(*rt, "maxHeapSize").asNumber());
  EXPECT_EQ(
      conf.getGCConfig().getOccupancyTarget(),
      gcConfig.getProperty(*rt, "occupancyTarget").asNumber());
  EXPECT_EQ(
      conf.getGCConfig().getEffectiveOOMThreshold(),
      gcConfig.getProperty(*rt, "effectiveOOMThreshold").asNumber());
  EXPECT_EQ(
      conf.getGCConfig().getShouldReleaseUnused(),
      SynthTrace::releaseUnusedFromName(
          gcConfig.getProperty(*rt, "shouldReleaseUnused")
              .asString(*rt)
              .utf8(*rt)
              .c_str()));
  EXPECT_EQ(
      conf.getGCConfig().getName(),
      gcConfig.getProperty(*rt, "name").asString(*rt).utf8(*rt));
  EXPECT_EQ(
      conf.getGCConfig().getAllocInYoung(),
      gcConfig.getProperty(*rt, "allocInYoung").asBool());

  EXPECT_EQ(
      conf.getMaxNumRegisters(),
      rtConfig.getProperty(*rt, "maxNumRegisters").asNumber());
  EXPECT_EQ(conf.getES6Proxy(), rtConfig.getProperty(*rt, "ES6Proxy").asBool());
  EXPECT_EQ(conf.getIntl(), rtConfig.getProperty(*rt, "Intl").asBool());
  EXPECT_EQ(
      conf.getEnableSampledStats(),
      rtConfig.getProperty(*rt, "enableSampledStats").asBool());
  EXPECT_EQ(
      conf.getVMExperimentFlags(),
      rtConfig.getProperty(*rt, "vmExperimentFlags").asNumber());
}

TEST_F(SynthTraceSerializationTest, FullTrace) {
  std::string result;
  auto resultStream = std::make_unique<llvh::raw_string_ostream>(result);
  const ::hermes::vm::RuntimeConfig conf;
  auto *hermesRoot = jsi::castInterface<IHermesRootAPI>(makeHermesRootAPI());
  std::unique_ptr<TracingHermesRuntime> rt(makeTracingHermesRuntime(
      hermesRoot->makeHermesRuntime(conf),
      conf,
      std::move(resultStream),
      /* forReplay */ true));

  SynthTrace::ObjectID objID;
  {
    auto obj = jsi::Object(*rt);
    objID = rt->useObjectID(obj);
    // Property name doesn't matter, just want to record that some property was
    // requested.
    auto value = obj.getProperty(*rt, "a");
    ASSERT_TRUE(value.isUndefined());
  }

  rt->flushAndDisableTrace();

  auto optTrace = rt->global()
                      .getPropertyAsObject(*rt, "JSON")
                      .getPropertyAsFunction(*rt, "parse")
                      .call(*rt, result)
                      .asObject(*rt);

  auto records = optTrace.getPropertyAsObject(*rt, "trace").asArray(*rt);

  auto record = records.getValueAtIndex(*rt, 0).asObject(*rt);
  EXPECT_EQ(
      "CreateObjectRecord",
      record.getProperty(*rt, "type").asString(*rt).utf8(*rt));
  EXPECT_TRUE(record.getProperty(*rt, "time").isNumber());
  EXPECT_EQ(objID, record.getProperty(*rt, "objID").asNumber());

  // The obj.getProperty(*rt, "a") creates a string primitive for "a".
  record = records.getValueAtIndex(*rt, 1).asObject(*rt);
  EXPECT_EQ(
      "CreateStringRecord",
      record.getProperty(*rt, "type").asString(*rt).utf8(*rt));
  EXPECT_TRUE(record.getProperty(*rt, "time").isNumber());
  EXPECT_TRUE(record.getProperty(*rt, "objID").isNumber());
  auto stringID = record.getProperty(*rt, "objID").asNumber();

  record = records.getValueAtIndex(*rt, 2).asObject(*rt);
  EXPECT_EQ(
      "GetPropertyRecord",
      record.getProperty(*rt, "type").asString(*rt).utf8(*rt));
  EXPECT_TRUE(record.getProperty(*rt, "time").isNumber());
  EXPECT_EQ(objID, record.getProperty(*rt, "objID").asNumber());
  EXPECT_EQ(
      SynthTrace::encodeString(stringID),
      SynthTrace::decode(
          record.getProperty(*rt, "propID").asString(*rt).utf8(*rt)));

  record = records.getValueAtIndex(*rt, 3).asObject(*rt);
  EXPECT_EQ(
      "ReturnToNativeRecord",
      record.getProperty(*rt, "type").asString(*rt).utf8(*rt));
  EXPECT_TRUE(record.getProperty(*rt, "time").isNumber());
  EXPECT_EQ(
      "undefined:", record.getProperty(*rt, "retval").asString(*rt).utf8(*rt));
}

TEST_F(SynthTraceSerializationTest, Utf8Record) {
  EXPECT_EQ(
      R"({"type":"Utf8Record","time":0,"objID":"string:123","retval":"hi\u00f0\u009f\u0091\u008b"})",
      to_string(
          SynthTrace::Utf8Record(
              dummyTime, SynthTrace::encodeString(123), "hi👋")));
  EXPECT_EQ(
      R"({"type":"Utf8Record","time":0,"objID":"string:111","retval":"\u00f0"})",
      to_string(
          SynthTrace::Utf8Record(
              dummyTime, SynthTrace::encodeString(111), "\xf0")));
}

TEST_F(SynthTraceSerializationTest, Utf16Record) {
  auto serialized =
      R"({"type":"Utf16Record","time":0,"objID":"string:123","retval":"hi\ud83d\udc4b"})";
  EXPECT_EQ(
      serialized,
      to_string(
          SynthTrace::Utf16Record(
              dummyTime, SynthTrace::encodeString(123), u"hi\xd83d\xdc4b")));
  serialized =
      R"({"type":"Utf16Record","time":0,"objID":"string:111","retval":"\ud83d"})";
  EXPECT_EQ(
      serialized,
      to_string(
          SynthTrace::Utf16Record(
              dummyTime, SynthTrace::encodeString(111), u"\xd83d")));
}

TEST_F(SynthTraceSerializationTest, GetStringDataRecord) {
  auto serialized =
      R"({"type":"GetStringDataRecord","time":0,"objID":"string:123","strData":"\nhello\ud83d\udc4b\\"})";
  EXPECT_EQ(
      serialized,
      to_string(
          SynthTrace::GetStringDataRecord(
              dummyTime,
              SynthTrace::encodeString(123),
              u"\nhello\xd83d\xdc4b\\")));
  serialized =
      R"({"type":"GetStringDataRecord","time":0,"objID":"propNameID:111","strData":"\ud83d"})";
  EXPECT_EQ(
      serialized,
      to_string(
          SynthTrace::GetStringDataRecord(
              dummyTime, SynthTrace::encodePropNameID(111), u"\xd83d")));
}

TEST_F(SynthTraceSerializationTest, SetPrototypeTest) {
  EXPECT_EQ(
      R"({"type":"SetPrototypeRecord","time":0,"objID":1,"value":"null:"})",
      to_string(
          SynthTrace::SetPrototypeRecord(
              dummyTime, 1, SynthTrace::encodeNull())));
  EXPECT_EQ(
      R"({"type":"SetPrototypeRecord","time":0,"objID":2,"value":"object:1"})",
      to_string(
          SynthTrace::SetPrototypeRecord(
              dummyTime, 2, SynthTrace::encodeObject(1))));
}

TEST_F(SynthTraceSerializationTest, GetPrototypeTest) {
  EXPECT_EQ(
      R"({"type":"GetPrototypeRecord","time":0,"objID":1})",
      to_string(SynthTrace::GetPrototypeRecord(dummyTime, 1)));
}

TEST_F(SynthTraceSerializationTest, CreateObjectWithPrototypeRecord) {
  EXPECT_EQ(
      R"({"type":"CreateObjectWithPrototypeRecord","time":0,"objID":1,"prototype":"null:"})",
      to_string(
          SynthTrace::CreateObjectWithPrototypeRecord(
              dummyTime, 1, SynthTrace::encodeNull())));
  EXPECT_EQ(
      R"({"type":"CreateObjectWithPrototypeRecord","time":0,"objID":2,"prototype":"object:1"})",
      to_string(
          SynthTrace::CreateObjectWithPrototypeRecord(
              dummyTime, 2, SynthTrace::encodeObject(1))));
}

TEST_F(SynthTraceSerializationTest, DeletePropertyRecord) {
  EXPECT_EQ(
      R"({"type":"DeletePropertyRecord","time":0,"objID":1,"propID":"string:123"})",
      to_string(
          SynthTrace::DeletePropertyRecord(
              dummyTime, 1, SynthTrace::encodeString(123))));

  EXPECT_EQ(
      R"({"type":"DeletePropertyRecord","time":0,"objID":1,"propID":"propNameID:123"})",
      to_string(
          SynthTrace::DeletePropertyRecord(
              dummyTime, 1, SynthTrace::encodePropNameID(123))));

  EXPECT_EQ(
      R"({"type":"DeletePropertyRecord","time":0,"objID":1,"propID":"number:0x00000000000000"})",
      to_string(
          SynthTrace::DeletePropertyRecord(
              dummyTime, 1, SynthTrace::encodeNumber(0))));
}
} // namespace
