/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/SynthTraceParser.h"

#include "hermes/Parser/JSLexer.h"
#include "hermes/Parser/JSONParser.h"
#include "hermes/Support/ErrorHandling.h"
#include "hermes/Support/HermesSafeMath.h"
#include "hermes/Support/SourceErrorManager.h"

#include <sstream>
#include <unordered_map>

namespace facebook {
namespace hermes {
namespace tracing {

using namespace ::hermes::parser;

namespace {

static const std::unordered_map<std::string, SynthTrace::JSErrorType>
    kStringToJSErrorType = {
        {"Error", SynthTrace::JSErrorType::Error},
        {"EvalError", SynthTrace::JSErrorType::EvalError},
        {"RangeError", SynthTrace::JSErrorType::RangeError},
        {"ReferenceError", SynthTrace::JSErrorType::ReferenceError},
        {"SyntaxError", SynthTrace::JSErrorType::SyntaxError},
        {"TypeError", SynthTrace::JSErrorType::TypeError},
        {"URIError", SynthTrace::JSErrorType::URIError},
};

SynthTrace::StringEncodingType getStringEncodingType(
    const std::string &encodingStr) {
  if (encodingStr == "ASCII") {
    return SynthTrace::StringEncodingType::ASCII;
  }
  if (encodingStr == "UTF-8") {
    return SynthTrace::StringEncodingType::UTF8;
  }
  assert(encodingStr == "UTF-16");
  return SynthTrace::StringEncodingType::UTF16;
}

/// Converts the data in the JSONString \p str into a u16string
std::u16string jsonStringToU16String(
    const ::hermes::parser::JSONString &jsonStr) {
  auto strRef = jsonStr.str();
  std::u16string ret;
  ::hermes::convertUTF8WithSurrogatesToUTF16(
      std::back_inserter(ret), strRef.begin(), strRef.end());
  return ret;
}

/// Converts the data in the JSONString \p str into a u8string. Each code unit
/// encoded in the JSONString is expected to represent each byte of the UTF-8
/// String.
std::string jsonStringToU8String(const ::hermes::parser::JSONString &jsonStr) {
  auto strRef = jsonStr.str();
  std::string ret;
  ::hermes::convertUTF8WithSurrogatesToUTF16(
      std::back_inserter(ret), strRef.begin(), strRef.end());
  return ret;
}

::hermes::SHA1 parseHashStrAsNumber(llvh::StringRef hashStr) {
  ::hermes::SHA1 sourceHash{};
  // Each byte is 2 characters.
  if (hashStr.size() != 2 * sourceHash.size()) {
    ::hermes::hermes_fatal("sourceHash is not the right length");
  }
  for (size_t i = 0; i < sourceHash.size(); ++i) {
    auto byte = llvh::StringRef{hashStr.data() + (i * 2), 2};
    sourceHash[i] =
        ::hermes::parseIntWithRadix</* AllowNumericSeparator */ false>(byte, 16)
            .getValue();
  }
  return sourceHash;
}

JSONObject *parseJSON(
    JSONFactory::Allocator &alloc,
    std::unique_ptr<llvh::MemoryBuffer> stream) {
  JSONFactory factory(alloc);
  ::hermes::SourceErrorManager sm;
  JSONParser parser(factory, std::move(stream), sm);
  auto rootObj = parser.parse();
  if (!rootObj) {
    // The source error manager will print to stderr.
    ::hermes::hermes_fatal("JSON invalid");
  }
  return llvh::cast<JSONObject>(rootObj.getValue());
}

/// Get a number from a JSON value.
template <typename NumericType>
NumericType getNumberAs(const JSONValue *val) {
  if (!val) {
    ::hermes::hermes_fatal("value doesn't exist");
  }
  if (val->getKind() != JSONKind::Number) {
    ::hermes::hermes_fatal("value is not a number");
  }
  return static_cast<NumericType>(llvh::cast<JSONNumber>(val)->getValue());
}

/// Get a number from a JSON value.
/// Use \param dflt if the JSON value doesn't exist.
template <typename NumericType>
NumericType getNumberAs(const JSONValue *val, NumericType dflt) {
  if (!val) {
    return dflt;
  } else {
    return getNumberAs<NumericType>(val);
  }
}

/// Parses the json \p value (which must be a string) as a uint64_t.
uint64_t jsonStringToUint64(const ::hermes::parser::JSONValue *val) {
  if (!val) {
    ::hermes::hermes_fatal("value doesn't exist");
  }

  auto str = llvh::dyn_cast<JSONString>(val);
  if (!str) {
    ::hermes::hermes_fatal("value is not a string");
  }

  uint64_t ret = std::strtoull(str->c_str(), nullptr, 10);
  return ret;
}

::hermes::vm::GCConfig::Builder getGCConfig(JSONObject *rtConfig) {
  // This function should extract all fields from GCConfig that can affect
  // performance metrics. Configs for debugging can be ignored.
  ::hermes::vm::GCConfig::Builder gcconf;
  if (!rtConfig) {
    return gcconf;
  }
  auto *val = rtConfig->get("gcConfig");
  if (!val) {
    return gcconf;
  }
  if (val->getKind() != JSONKind::Object) {
    ::hermes::hermes_fatal("gcConfig should be an object");
  }
  auto *gcConfig = llvh::cast<JSONObject>(val);
  if (auto *sz = gcConfig->get("initHeapSize")) {
    gcconf.withInitHeapSize(getNumberAs<::hermes::vm::gcheapsize_t>(sz));
  }
  if (auto *sz = gcConfig->get("maxHeapSize")) {
    gcconf.withMaxHeapSize(getNumberAs<::hermes::vm::gcheapsize_t>(sz));
  }
  if (auto *occupancy = gcConfig->get("occupancyTarget")) {
    gcconf.withOccupancyTarget(getNumberAs<double>(occupancy));
  }
  if (auto *threshold = gcConfig->get("effectiveOOMThreshold")) {
    gcconf.withEffectiveOOMThreshold(getNumberAs<unsigned>(threshold));
  }
  if (auto *shouldRelease = gcConfig->get("shouldReleaseUnused")) {
    gcconf.withShouldReleaseUnused(
        SynthTrace::releaseUnusedFromName(
            llvh::cast<JSONString>(shouldRelease)->c_str()));
  }
  if (auto *name = gcConfig->get("name")) {
    gcconf.withName(llvh::cast<JSONString>(name)->str());
  }
  if (auto *allocInYoung = gcConfig->get("allocInYoung")) {
    gcconf.withAllocInYoung(llvh::cast<JSONBoolean>(allocInYoung)->getValue());
  }
  if (auto *revertAtTTI = gcConfig->get("revertToYGAtTTI")) {
    gcconf.withRevertToYGAtTTI(
        llvh::cast<JSONBoolean>(revertAtTTI)->getValue());
  }
  return gcconf;
}

::hermes::vm::RuntimeConfig::Builder getRuntimeConfig(JSONObject *rtConfig) {
  ::hermes::vm::RuntimeConfig::Builder conf;
  if (!rtConfig) {
    // If the config doesn't exist, return some default values.
    return conf;
  }

  if (auto *maxNumRegisters = rtConfig->get("maxNumRegisters")) {
    conf.withMaxNumRegisters(getNumberAs<unsigned>(maxNumRegisters));
  }
  if (auto *proxy = rtConfig->get("ES6Proxy")) {
    conf.withES6Proxy(llvh::cast<JSONBoolean>(proxy)->getValue());
  }
  if (auto *intl = rtConfig->get("Intl")) {
    conf.withIntl(llvh::cast<JSONBoolean>(intl)->getValue());
  }
  if (auto *microtasks = rtConfig->get("MicrotasksQueue")) {
    conf.withMicrotaskQueue(llvh::cast<JSONBoolean>(microtasks)->getValue());
  }
  if (auto *enableSampledStats = rtConfig->get("enableSampledStats")) {
    conf.withEnableSampledStats(
        llvh::cast<JSONBoolean>(enableSampledStats)->getValue());
  }
  if (auto *vmExperimentFlags = rtConfig->get("vmExperimentFlags")) {
    conf.withVMExperimentFlags(getNumberAs<uint32_t>(vmExperimentFlags));
  }

  return conf;
}

template <template <typename, typename> class Collection>
Collection<std::string, std::allocator<std::string>> getListOfStrings(
    JSONArray *array) {
  Collection<std::string, std::allocator<std::string>> strings;
  std::transform(
      array->begin(),
      array->end(),
      std::back_inserter(strings),
      [](const JSONValue *value) -> std::string {
        if (value->getKind() != JSONKind::String) {
          ::hermes::hermes_fatal("Array should contain only strings");
        }
        return std::string(llvh::cast<JSONString>(value)->c_str());
      });
  return strings;
}

/// Converts the JSON array \p array of numbers (doubles) into a vector of
/// integers of type \p T. The type must be converted into int8, int16, or
/// int32.
template <
    typename T,
    typename = typename std::enable_if<std::is_integral<T>::value, void>::type>
std::vector<T> getNumberArrayFromJSON(JSONArray *array) {
  std::vector<T> values;
  std::transform(
      array->begin(),
      array->end(),
      std::back_inserter(values),
      [](const JSONValue *value) -> T {
        if (value->getKind() != JSONKind::Number) {
          ::hermes::hermes_fatal(
              "Serialization arrays should only contain numbers");
        }
        return ::hermes::ubcastFromDouble<T>(
            llvh::cast<JSONNumber>(value)->getValue());
      });
  return values;
}

SynthTrace getTrace(
    JSONArray *array,
    std::optional<SynthTrace::ObjectID> globalObjID) {
  using RecordType = SynthTrace::RecordType;
  SynthTrace trace(
      ::hermes::vm::RuntimeConfig(), /* traceStream */ nullptr, globalObjID);
  auto getListOfTraceValues =
      [](JSONArray *array) -> std::vector<SynthTrace::TraceValue> {
    std::vector<SynthTrace::TraceValue> values;
    std::transform(
        array->begin(),
        array->end(),
        std::back_inserter(values),
        [](const JSONValue *value) -> SynthTrace::TraceValue {
          if (value->getKind() != JSONKind::String) {
            ::hermes::hermes_fatal("Array should contain only strings");
          }
          return SynthTrace::decode(llvh::cast<JSONString>(value)->c_str());
        });
    return values;
  };

  auto strToRecordType = [](llvh::StringRef str) {
#define CASE(t)           \
  if (str == #t "Record") \
    return RecordType::t;
    SYNTH_TRACE_RECORD_TYPES(CASE)
#undef CASE
    ::hermes::hermes_fatal("Unknown record type");
  };

  for (auto *val : *array) {
    auto *obj = llvh::cast<JSONObject>(val);
    auto timeFromStart =
        std::chrono::milliseconds(getNumberAs<uint64_t>(obj->get("time"), 0));
    RecordType kind =
        strToRecordType(llvh::cast<JSONString>(obj->get("type"))->str());
    // Common properties, they may not exist on all objects so use a
    // cast_or_null.
    // objID can be either a JSONNumber or JSONString, depending on the Record,
    // so use a dyn_cast
    auto *objID = llvh::dyn_cast_or_null<JSONNumber>(obj->get("objID"));
    auto *hostObjID = llvh::cast_or_null<JSONNumber>(obj->get("hostObjectID"));
    auto *funcID = llvh::cast_or_null<JSONNumber>(obj->get("functionID"));
    auto *propID = llvh::cast_or_null<JSONString>(obj->get("propID"));
    auto *propNameID = llvh::cast_or_null<JSONNumber>(obj->get("propNameID"));
    auto *propName = llvh::cast_or_null<JSONString>(obj->get("propName"));
    auto *propValue = llvh::cast_or_null<JSONString>(obj->get("value"));
    auto *arrayIndex = llvh::cast_or_null<JSONNumber>(obj->get("index"));
    auto *callArgs = llvh::cast_or_null<JSONArray>(obj->get("args"));
    auto *thisArg = llvh::cast_or_null<JSONString>(obj->get("thisArg"));
    auto *retval = llvh::cast_or_null<JSONString>(obj->get("retval"));
    switch (kind) {
      case RecordType::BeginExecJS: {
        std::string sourceURL;
        ::hermes::SHA1 hash{};
        bool sourceIsBytecode{false};
        if (JSONString *sourceURLJSON =
                llvh::cast_or_null<JSONString>(obj->get("sourceURL"))) {
          sourceURL = sourceURLJSON->str();
        }
        if (JSONString *sourceHash =
                llvh::cast_or_null<JSONString>(obj->get("sourceHash"))) {
          hash = parseHashStrAsNumber(sourceHash->str());
        }
        if (JSONBoolean *sourceIsBytecodeJson =
                llvh::cast_or_null<JSONBoolean>(obj->get("sourceIsBytecode"))) {
          sourceIsBytecode = sourceIsBytecodeJson->getValue();
        }
        trace.emplace_back<SynthTrace::BeginExecJSRecord>(
            timeFromStart,
            std::move(sourceURL),
            std::move(hash),
            sourceIsBytecode);
        break;
      }
      case RecordType::EndExecJS:
        trace.emplace_back<SynthTrace::EndExecJSRecord>(
            timeFromStart, SynthTrace::decode(retval->c_str()));
        break;
      case RecordType::Marker:
        trace.emplace_back<SynthTrace::MarkerRecord>(
            timeFromStart, llvh::cast<JSONString>(obj->get("tag"))->c_str());
        break;
      case RecordType::CreateObject:
        trace.emplace_back<SynthTrace::CreateObjectRecord>(
            timeFromStart, objID->getValue());
        break;
      case RecordType::CreateObjectWithPrototype: {
        auto *prototype = llvh::cast<JSONString>(obj->get("prototype"));
        trace.emplace_back<SynthTrace::CreateObjectWithPrototypeRecord>(
            timeFromStart,
            objID->getValue(),
            SynthTrace::decode(prototype->c_str()));
        break;
      }
      case RecordType::QueueMicrotask: {
        auto callbackID =
            getNumberAs<SynthTrace::ObjectID>(obj->get("callbackID"));
        trace.emplace_back<SynthTrace::QueueMicrotaskRecord>(
            timeFromStart, callbackID);
        break;
      }
      case RecordType::DrainMicrotasks: {
        int maxMicrotasksHint = getNumberAs<int>(obj->get("maxMicrotasksHint"));
        trace.emplace_back<SynthTrace::DrainMicrotasksRecord>(
            timeFromStart, maxMicrotasksHint);
        break;
      }
      case RecordType::CreateBigInt: {
        auto method = llvh::cast<JSONString>(obj->get("method"));
        SynthTrace::CreateBigIntRecord::Method creationMethod =
            SynthTrace::CreateBigIntRecord::Method::FromUint64;
        if (method->str() == "FromInt64") {
          creationMethod = SynthTrace::CreateBigIntRecord::Method::FromInt64;
        } else {
          assert(method->str() == "FromUint64");
        }
        trace.emplace_back<SynthTrace::CreateBigIntRecord>(
            timeFromStart,
            objID->getValue(),
            creationMethod,
            jsonStringToUint64(obj->get("bits")));
        break;
      }
      case RecordType::BigIntToString: {
        auto *strID = llvh::cast<JSONNumber>(obj->get("strID"));
        auto *bigintID = llvh::cast<JSONNumber>(obj->get("bigintID"));
        trace.emplace_back<SynthTrace::BigIntToStringRecord>(
            timeFromStart,
            strID->getValue(),
            bigintID->getValue(),
            getNumberAs<int>(obj->get("radix")));
        break;
      }
      case RecordType::CreateString: {
        auto encoding = llvh::cast<JSONString>(obj->get("encoding"));
        auto str = llvh::cast<JSONString>(obj->get("chars"));
        switch (getStringEncodingType(encoding->str())) {
          case SynthTrace::StringEncodingType::ASCII: {
            trace.emplace_back<SynthTrace::CreateStringRecord>(
                timeFromStart,
                objID->getValue(),
                str->str().data(),
                str->str().size());
            break;
          }
          case SynthTrace::StringEncodingType::UTF8: {
            auto utf8Str = jsonStringToU8String(*str);
            trace.emplace_back<SynthTrace::CreateStringRecord>(
                timeFromStart,
                objID->getValue(),
                reinterpret_cast<const uint8_t *>(utf8Str.data()),
                utf8Str.size());
            break;
          }
          case SynthTrace::StringEncodingType::UTF16: {
            auto utf16Str = jsonStringToU16String(*str);
            trace.emplace_back<SynthTrace::CreateStringRecord>(
                timeFromStart,
                objID->getValue(),
                utf16Str.data(),
                utf16Str.size());
            break;
          }
        }
        break;
      }
      case RecordType::CreatePropNameID: {
        auto id = llvh::cast<JSONNumber>(obj->get("objID"));
        auto encoding = llvh::cast<JSONString>(obj->get("encoding"));
        auto str = llvh::cast<JSONString>(obj->get("chars"));
        switch (getStringEncodingType(encoding->str())) {
          case SynthTrace::StringEncodingType::ASCII: {
            trace.emplace_back<SynthTrace::CreatePropNameIDRecord>(
                timeFromStart,
                id->getValue(),
                str->str().data(),
                str->str().size());
            break;
          }
          case SynthTrace::StringEncodingType::UTF8: {
            auto utf8Str = jsonStringToU8String(*str);
            trace.emplace_back<SynthTrace::CreatePropNameIDRecord>(
                timeFromStart,
                id->getValue(),
                reinterpret_cast<const uint8_t *>(utf8Str.data()),
                utf8Str.size());
            break;
          }
          case SynthTrace::StringEncodingType::UTF16: {
            auto utf16Str = jsonStringToU16String(*str);
            trace.emplace_back<SynthTrace::CreatePropNameIDRecord>(
                timeFromStart,
                id->getValue(),
                utf16Str.data(),
                utf16Str.size());
            break;
          }
        }
        break;
      }
      case RecordType::CreatePropNameIDWithValue: {
        trace.emplace_back<SynthTrace::CreatePropNameIDWithValueRecord>(
            timeFromStart,
            objID->getValue(),
            SynthTrace::decode(propValue->c_str()));
        break;
      }
      case RecordType::CreateHostObject:
        trace.emplace_back<SynthTrace::CreateHostObjectRecord>(
            timeFromStart, objID->getValue());
        break;
      case RecordType::CreateHostFunction: {
        unsigned paramCount = 0;
        if (JSONNumber *jsonParamCount =
                llvh::cast_or_null<JSONNumber>(obj->get("parameterCount"))) {
          paramCount = jsonParamCount->getValue();
        }
        std::string functionName;
        if (JSONString *jsonFunctionName =
                llvh::cast_or_null<JSONString>(obj->get("functionName"))) {
          functionName = jsonFunctionName->str();
        }
        trace.emplace_back<SynthTrace::CreateHostFunctionRecord>(
            timeFromStart,
            objID->getValue(),
            propNameID->getValue(),
#ifdef HERMESVM_API_TRACE_DEBUG
            functionName,
#endif
            paramCount);
        break;
      }
      case RecordType::GetProperty: {
        trace.emplace_back<SynthTrace::GetPropertyRecord>(
            timeFromStart,
            objID->getValue(),
            SynthTrace::decode(propID->str())
#ifdef HERMESVM_API_TRACE_DEBUG
                ,
            std::string(propName->c_str()),
#endif
        );
        break;
      }
      case RecordType::SetProperty:
        trace.emplace_back<SynthTrace::SetPropertyRecord>(
            timeFromStart,
            objID->getValue(),
            SynthTrace::decode(propID->str()),
#ifdef HERMESVM_API_TRACE_DEBUG
            std::string(propName->c_str()),
#endif
            SynthTrace::decode(propValue->c_str()));
        break;
      case RecordType::HasProperty:
        trace.emplace_back<SynthTrace::HasPropertyRecord>(
            timeFromStart,
            objID->getValue(),
            SynthTrace::decode(propID->str())
#ifdef HERMESVM_API_TRACE_DEBUG
                ,
            std::string(propName->c_str())
#endif
        );
        break;
      case RecordType::DeleteProperty: {
        trace.emplace_back<SynthTrace::DeletePropertyRecord>(
            timeFromStart,
            objID->getValue(),
            SynthTrace::decode(propID->str()));
        break;
      }
      case RecordType::GetPropertyNames: {
        trace.emplace_back<SynthTrace::GetPropertyNamesRecord>(
            timeFromStart, objID->getValue());
        break;
      }
      case RecordType::CreateArray:
        trace.emplace_back<SynthTrace::CreateArrayRecord>(
            timeFromStart,
            objID->getValue(),
            getNumberAs<uint64_t>(obj->get("length")));
        break;
      case RecordType::CreateUInt8Array: {
        trace.emplace_back<SynthTrace::CreateUInt8ArrayRecord>(
            timeFromStart,
            objID->getValue(),
            getNumberAs<uint64_t>(obj->get("length")));
        break;
      }
      case RecordType::CreateUInt8ArrayFromArrayBuffer: {
        auto *objID = llvh::cast<JSONNumber>(obj->get("objID"));
        auto *bufferID = llvh::cast<JSONNumber>(obj->get("bufferID"));
        trace.emplace_back<SynthTrace::CreateUInt8ArrayFromArrayBufferRecord>(
            timeFromStart,
            objID->getValue(),
            bufferID->getValue(),
            getNumberAs<uint64_t>(obj->get("offset")),
            getNumberAs<uint64_t>(obj->get("length")));
        break;
      }
      case RecordType::GetBufferFromTypedArray: {
        auto *bufferID = llvh::cast<JSONNumber>(obj->get("bufferID"));
        auto *typedArrayID = llvh::cast<JSONNumber>(obj->get("typedArrayID"));
        trace.emplace_back<SynthTrace::GetBufferFromTypedArrayRecord>(
            timeFromStart, bufferID->getValue(), typedArrayID->getValue());
        break;
      }
      case RecordType::CreateJSError: {
        auto *errorTypeStr = llvh::cast<JSONString>(obj->get("errorType"));
        auto *messageID = llvh::cast<JSONNumber>(obj->get("messageID"));
        auto it = kStringToJSErrorType.find(errorTypeStr->str());
        assert(
            it != kStringToJSErrorType.end() && "Unknown error type in trace");
        trace.emplace_back<SynthTrace::CreateJSErrorRecord>(
            timeFromStart,
            objID->getValue(),
            it->second,
            messageID->getValue());
        break;
      }
      case RecordType::ArrayRead: {
        trace.emplace_back<SynthTrace::ArrayReadRecord>(
            timeFromStart, objID->getValue(), arrayIndex->getValue());
        break;
      }
      case RecordType::ArrayWrite:
        trace.emplace_back<SynthTrace::ArrayWriteRecord>(
            timeFromStart,
            objID->getValue(),
            arrayIndex->getValue(),
            SynthTrace::decode(propValue->c_str()));
        break;
      case RecordType::ArrayPush: {
        auto *elements = llvh::dyn_cast<JSONArray>(obj->get("elements"));
        auto *length = llvh::dyn_cast<JSONNumber>(obj->get("length"));
        trace.emplace_back<SynthTrace::ArrayPushRecord>(
            timeFromStart,
            objID->getValue(),
            getListOfTraceValues(elements),
            length->getValue());
        break;
      }
      case RecordType::CallFromNative:
        trace.emplace_back<SynthTrace::CallFromNativeRecord>(
            timeFromStart,
            funcID->getValue(),
            SynthTrace::decode(thisArg->c_str()),
            getListOfTraceValues(callArgs));
        break;
      case RecordType::ConstructFromNative:
        trace.emplace_back<SynthTrace::ConstructFromNativeRecord>(
            timeFromStart,
            funcID->getValue(),
            SynthTrace::decode(thisArg->c_str()),
            getListOfTraceValues(callArgs));
        break;
      case RecordType::ReturnFromNative:
        trace.emplace_back<SynthTrace::ReturnFromNativeRecord>(
            timeFromStart, SynthTrace::decode(retval->c_str()));
        break;
      case RecordType::ReturnToNative:
        trace.emplace_back<SynthTrace::ReturnToNativeRecord>(
            timeFromStart, SynthTrace::decode(retval->c_str()));
        break;
      case RecordType::CallToNative:
        trace.emplace_back<SynthTrace::CallToNativeRecord>(
            timeFromStart,
            funcID->getValue(),
            SynthTrace::decode(thisArg->c_str()),
            getListOfTraceValues(callArgs));
        break;
      case RecordType::GetPropertyNative:
        trace.emplace_back<SynthTrace::GetPropertyNativeRecord>(
            timeFromStart,
            hostObjID->getValue(),
            propNameID->getValue(),
            propName->c_str());
        break;
      case RecordType::GetPropertyNativeReturn:
        trace.emplace_back<SynthTrace::GetPropertyNativeReturnRecord>(
            timeFromStart, SynthTrace::decode(retval->c_str()));
        break;
      case RecordType::SetPropertyNative:
        trace.emplace_back<SynthTrace::SetPropertyNativeRecord>(
            timeFromStart,
            hostObjID->getValue(),
            propNameID->getValue(),
            propName->c_str(),
            SynthTrace::decode(propValue->c_str()));
        break;
      case RecordType::SetPropertyNativeReturn:
        trace.emplace_back<SynthTrace::SetPropertyNativeReturnRecord>(
            timeFromStart);
        break;
      case RecordType::GetNativePropertyNames:
        trace.emplace_back<SynthTrace::GetNativePropertyNamesRecord>(
            timeFromStart, hostObjID->getValue());
        break;
      case RecordType::GetNativePropertyNamesReturn: {
        auto *pnids = llvh::cast<JSONArray>(obj->get("propNameIDs"));
        trace.emplace_back<SynthTrace::GetNativePropertyNamesReturnRecord>(
            timeFromStart, getListOfTraceValues(pnids));
        break;
      }
      case RecordType::SetExternalMemoryPressure: {
        size_t amount = getNumberAs<size_t>(obj->get("amount"));
        trace.emplace_back<SynthTrace::SetExternalMemoryPressureRecord>(
            timeFromStart, objID->getValue(), amount);
        break;
      }
      case RecordType::Utf8: {
        auto *objId = llvh::cast<JSONString>(obj->get("objID"));
        trace.emplace_back<SynthTrace::Utf8Record>(
            timeFromStart,
            SynthTrace::decode(objId->str()),
            jsonStringToU8String(*retval));
        break;
      }
      case RecordType::Utf16: {
        auto *objId = llvh::cast<JSONString>(obj->get("objID"));
        trace.emplace_back<SynthTrace::Utf16Record>(
            timeFromStart,
            SynthTrace::decode(objId->str()),
            jsonStringToU16String(*retval));
        break;
      }
      case RecordType::GetStringData: {
        auto *objId = llvh::cast<JSONString>(obj->get("objID"));
        auto *strData = llvh::cast<JSONString>(obj->get("strData"));
        trace.emplace_back<SynthTrace::GetStringDataRecord>(
            timeFromStart,
            SynthTrace::decode(objId->str()),
            jsonStringToU16String(*strData));
        break;
      }
      case RecordType::SetPrototype: {
        trace.emplace_back<SynthTrace::SetPrototypeRecord>(
            timeFromStart,
            objID->getValue(),
            SynthTrace::decode(propValue->c_str()));
        break;
      }
      case RecordType::GetPrototype:
        trace.emplace_back<SynthTrace::GetPrototypeRecord>(
            timeFromStart, objID->getValue());
        break;
      case RecordType::Serialize:
        trace.emplace_back<SynthTrace::SerializeRecord>(
            timeFromStart, SynthTrace::decode(propValue->c_str()));
        break;
      case RecordType::Deserialize: {
        auto offsetsJSON = llvh::cast<JSONArray>(obj->get("offsets"));
        auto contentJSON = llvh::cast<JSONArray>(obj->get("content"));
        auto stringsJSON = llvh::cast<JSONArray>(obj->get("strings"));
        auto offsets = getNumberArrayFromJSON<uint32_t>(offsetsJSON);
        auto contents = getNumberArrayFromJSON<uint8_t>(contentJSON);
        auto strings = getNumberArrayFromJSON<uint8_t>(stringsJSON);
        trace.emplace_back<SynthTrace::DeserializeRecord>(
            timeFromStart, offsets, contents, strings);
        break;
      }
      case RecordType::MathCall: {
        std::vector<double> args;
        for (const SynthTrace::TraceValue &arg :
             getListOfTraceValues(callArgs)) {
          args.push_back(arg.getNumber());
        }
        trace.emplace_back<SynthTrace::MathCallRecord>(
            timeFromStart,
            llvh::cast<JSONString>(obj->get("function"))->str().str(),
            std::move(args),
            SynthTrace::decode(retval->c_str()).getNumber());
        break;
      }
      case RecordType::Global: {
        trace.emplace_back<SynthTrace::GlobalRecord>(
            timeFromStart, objID->getValue());
        break;
      }
    }
  }
  return trace;
}

} // namespace

std::tuple<
    SynthTrace,
    ::hermes::vm::RuntimeConfig::Builder,
    ::hermes::vm::GCConfig::Builder>
parseSynthTrace(std::unique_ptr<llvh::MemoryBuffer> trace) {
  JSLexer::Allocator alloc;
  JSONObject *root = llvh::cast<JSONObject>(parseJSON(alloc, std::move(trace)));
  if (auto *ver = root->get("version")) {
    // Version exists, validate that it is a number, and the correct version.
    if (auto *verNum = llvh::dyn_cast<JSONNumber>(ver)) {
      // version is a number.
      const uint32_t version = verNum->getValue();
      if (version != SynthTrace::synthVersion()) {
        ::hermes::hermes_fatal(
            "Trace version mismatch, expected " +
            std::to_string(SynthTrace::synthVersion()) + ", actual: " +
            std::to_string(static_cast<uint32_t>(verNum->getValue())));
      }
    } else {
      ::hermes::hermes_fatal(
          "version exists, but is not a Number. Found instead value of kind: " +
          std::string(::hermes::parser::JSONKindToString(ver->getKind())));
    }
  }
  // Else, for backwards compatibility, allow no version to be specified, which
  // will imply "latest version".

  auto *gid = llvh::cast_or_null<JSONNumber>(root->get("globalObjID"));
  std::optional<SynthTrace::ObjectID> globalObjID = gid
      ? getNumberAs<SynthTrace::ObjectID>(gid)
      : std::optional<SynthTrace::ObjectID>();
  // Get and parse the records list.
  JSONObject *const rtConfig =
      llvh::cast_or_null<JSONObject>(root->get("runtimeConfig"));
  return std::make_tuple(
      getTrace(llvh::cast<JSONArray>(root->at("trace")), globalObjID),
      getRuntimeConfig(rtConfig),
      getGCConfig(rtConfig));
}

std::tuple<
    SynthTrace,
    ::hermes::vm::RuntimeConfig::Builder,
    ::hermes::vm::GCConfig::Builder>
parseSynthTrace(const std::string &tracefile) {
  return parseSynthTrace(
      std::move(llvh::MemoryBuffer::getFile(tracefile).get()));
}

} // namespace tracing
} // namespace hermes
} // namespace facebook
