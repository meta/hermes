/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/SynthTrace.h>

#include "hermes/Support/Algorithms.h"
#include "hermes/Support/Conversions.h"
#include "hermes/Support/JSONEmitter.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/StringPrimitive.h"

#include "llvh/Support/Endian.h"
#include "llvh/Support/NativeFormatting.h"
#include "llvh/Support/raw_ostream.h"

#include <cmath>
#include <unordered_map>

namespace facebook {
namespace hermes {
namespace tracing {

namespace {

using RecordType = SynthTrace::RecordType;
using JSONEmitter = ::hermes::JSONEmitter;
using ::hermes::parser::JSONArray;
using ::hermes::parser::JSONObject;
using ::hermes::parser::JSONValue;

double decodeNumber(const std::string &numberAsString) {
  // Assume the original platform and the current platform are both little
  // endian for simplicity.
  static_assert(
      llvh::support::endian::system_endianness() ==
          llvh::support::endianness::little,
      "Only little-endian decoding allowed");
  assert(
      numberAsString.substr(0, 2) == std::string("0x") && "Invalid hex number");
  // NOTE: This should use stoul, but that's not defined on Android.
  std::stringstream ss;
  ss << std::hex << numberAsString;
  uint64_t x;
  ss >> x;
  return llvh::BitsToDouble(x);
}

std::string doublePrinter(double x) {
  // Encode a number as its little-endian hex encoding.
  // NOTE: While this could use big-endian, it makes it more complicated for
  // JS to read out (forces use of a DataView rather than a Float64Array)
  static_assert(
      llvh::support::endian::system_endianness() ==
          llvh::support::endianness::little,
      "Only little-endian encoding allowed");
  std::string result;
  llvh::raw_string_ostream resultStream{result};
  llvh::write_hex(
      resultStream,
      llvh::DoubleToBits(x),
      llvh::HexPrintStyle::PrefixLower,
      16);
  resultStream.flush();
  return result;
}

uint64_t decodeID(const std::string &s) {
  char *str_end;
  // Signed so that we can easily check for negative.
  auto id = strtoll(s.c_str(), &str_end, 10);
  assert(id >= 0 && "Malformed id");
  assert(str_end == s.c_str() + s.size() && "Malformed id");
  return id;
}

llvh::StringRef recordTypeToString(RecordType type) {
  switch (type) {
#define CASE(t)       \
  case RecordType::t: \
    return #t "Record";
    SYNTH_TRACE_RECORD_TYPES(CASE)
#undef CASE
  }
}

} // namespace

bool SynthTrace::TraceValue::operator==(const TraceValue &that) const {
  if (tag_ != that.tag_) {
    return false;
  }
  switch (tag_) {
    case Tag::Bool:
      return val_.b == that.val_.b;
    case Tag::Number:
      // For now, ignore differences that result from NaN, +0/-0.
      return val_.n == that.val_.n;
    case Tag::Object:
    case Tag::String:
    case Tag::PropNameID:
      return val_.uid == that.val_.uid;
    default:
      return true;
  }
}

SynthTrace::SynthTrace(
    const ::hermes::vm::RuntimeConfig &conf,
    std::unique_ptr<llvh::raw_ostream> traceStream,
    std::optional<ObjectID> globalObjID)
    : traceStream_(std::move(traceStream)), globalObjID_(globalObjID) {
  if (traceStream_) {
    json_ = std::make_unique<JSONEmitter>(*traceStream_, /*pretty*/ true);
    json_->openDict();
    json_->emitKeyValue("version", synthVersion());

    // RuntimeConfig section.
    json_->emitKey("runtimeConfig");
    json_->openDict();
    {
      json_->emitKey("gcConfig");
      json_->openDict();
      json_->emitKeyValue("initHeapSize", conf.getGCConfig().getInitHeapSize());
      json_->emitKeyValue("maxHeapSize", conf.getGCConfig().getMaxHeapSize());
      json_->emitKeyValue(
          "occupancyTarget", conf.getGCConfig().getOccupancyTarget());
      json_->emitKeyValue(
          "effectiveOOMThreshold",
          conf.getGCConfig().getEffectiveOOMThreshold());
      json_->emitKeyValue(
          "shouldReleaseUnused",
          nameFromReleaseUnused(conf.getGCConfig().getShouldReleaseUnused()));
      json_->emitKeyValue("name", conf.getGCConfig().getName());
      json_->emitKeyValue("allocInYoung", conf.getGCConfig().getAllocInYoung());
      json_->emitKeyValue(
          "revertToYGAtTTI", conf.getGCConfig().getRevertToYGAtTTI());
      json_->closeDict();
    }
    json_->emitKeyValue("maxNumRegisters", conf.getMaxNumRegisters());
    json_->emitKeyValue("ES6Proxy", conf.getES6Proxy());
    json_->emitKeyValue("Intl", conf.getIntl());
    json_->emitKeyValue("MicrotasksQueue", conf.getMicrotaskQueue());
    json_->emitKeyValue("enableSampledStats", conf.getEnableSampledStats());
    json_->emitKeyValue("vmExperimentFlags", conf.getVMExperimentFlags());
    json_->closeDict();

    // Build properties section
    json_->emitKey("buildProperties");
    json_->openDict();
    json_->emitKeyValue("nativePointerSize", sizeof(void *));
    json_->emitKeyValue(
        "compressedPointers",
#ifdef HERMESVM_COMPRESSED_POINTERS
        true
#else
        false
#endif

    );
    json_->emitKeyValue(
        "debugBuild",
#ifdef NDEBUG
        false
#else
        true
#endif
    );
    json_->emitKeyValue(
        "slowDebug",
#ifdef HERMES_SLOW_DEBUG
        true
#else
        false
#endif
    );
    json_->emitKeyValue(
        "enableDebugger",
#ifdef HERMES_ENABLE_DEBUGGER
        true
#else
        false
#endif
    );
    // The size of this type was a thing that varied between 64-bit Android and
    // 64-bit desktop Linux builds.  A config flag now allows us to build on
    // Linux desktop in a way that matches the sizes.
    json_->emitKeyValue(
        "sizeofExternalASCIIStringPrimitive",
        sizeof(::hermes::vm::ExternalASCIIStringPrimitive));
    json_->closeDict();

    // Both the top-level dict and the trace array remain open.  The latter is
    // added to during execution.  Both are closed by flushAndDisable.
    json_->emitKey("trace");
    json_->openArray();
  }
}

SynthTrace::TraceValue SynthTrace::encodeUndefined() {
  return TraceValue::encodeUndefinedValue();
}

SynthTrace::TraceValue SynthTrace::encodeNull() {
  return TraceValue::encodeNullValue();
}

SynthTrace::TraceValue SynthTrace::encodeBool(bool value) {
  return TraceValue::encodeBoolValue(value);
}

SynthTrace::TraceValue SynthTrace::encodeNumber(double value) {
  return TraceValue::encodeNumberValue(value);
}

SynthTrace::TraceValue SynthTrace::encodeObject(ObjectID objID) {
  return TraceValue::encodeObjectValue(objID);
}

SynthTrace::TraceValue SynthTrace::encodeBigInt(ObjectID objID) {
  return TraceValue::encodeBigIntValue(objID);
}

SynthTrace::TraceValue SynthTrace::encodeString(ObjectID objID) {
  return TraceValue::encodeStringValue(objID);
}

SynthTrace::TraceValue SynthTrace::encodePropNameID(ObjectID objID) {
  return TraceValue::encodePropNameIDValue(objID);
}

SynthTrace::TraceValue SynthTrace::encodeSymbol(ObjectID objID) {
  return TraceValue::encodeSymbolValue(objID);
}

/*static*/
std::string SynthTrace::encode(TraceValue value) {
  if (value.isUndefined()) {
    return "undefined:";
  } else if (value.isNull()) {
    return "null:";
  } else if (value.isObject()) {
    return std::string("object:") + std::to_string(value.getUID());
  } else if (value.isBigInt()) {
    return std::string("bigint:") + std::to_string(value.getUID());
  } else if (value.isString()) {
    return std::string("string:") + std::to_string(value.getUID());
  } else if (value.isPropNameID()) {
    return std::string("propNameID:") + std::to_string(value.getUID());
  } else if (value.isSymbol()) {
    return std::string("symbol:") + std::to_string(value.getUID());
  } else if (value.isNumber()) {
    return std::string("number:") + doublePrinter(value.getNumber());
  } else if (value.isBool()) {
    return std::string("bool:") + (value.getBool() ? "true" : "false");
  } else {
    llvm_unreachable("No other values allowed in the trace");
  }
}

/*static*/
SynthTrace::TraceValue SynthTrace::decode(const std::string &str) {
  auto location = str.find(':');
  assert(location < str.size() && "Must contain a type tag");
  auto tag = std::string(str.begin(), str.begin() + location);
  // Don't include the colon, so add 1
  auto rest = std::string(str.begin() + location + 1, str.end());
  // This should be a switch, but C++ doesn't allow strings in switch
  // statements.
  if (tag == "undefined") {
    return encodeUndefined();
  } else if (tag == "null") {
    return encodeNull();
  } else if (tag == "bool") {
    assert((rest == "true" || rest == "false") && "Malformed bool value");
    return encodeBool(rest == "true");
  } else if (tag == "number") {
    return encodeNumber(decodeNumber(rest));
  } else if (tag == "object") {
    return encodeObject(decodeID(rest));
  } else if (tag == "string") {
    return encodeString(decodeID(rest));
  } else if (tag == "bigint") {
    return encodeBigInt(decodeID(rest));
  } else if (tag == "propNameID") {
    return encodePropNameID(decodeID(rest));
  } else if (tag == "symbol") {
    return encodeSymbol(decodeID(rest));
  } else {
    llvm_unreachable("Illegal object encountered");
  }
}

#ifdef HERMESVM_API_TRACE_DEBUG
std::string SynthTrace::getDescriptiveString(
    jsi::Runtime &runtime,
    const jsi::Value &value) {
  if (value.isUndefined()) {
    return "undefined:";
  } else if (value.isNull()) {
    return "null:";
  } else if (value.isBool()) {
    return value.getBool() ? "true" : "false";
  } else if (value.isNumber()) {
    return std::to_string(value.getNumber());
  } else if (value.isBigInt()) {
    return value.getBigInt(runtime).toString(runtime).utf8(runtime);
  } else if (value.isString()) {
    return value.getString(runtime).utf8(runtime);
  } else if (value.isObject()) {
    return "object:";
  } else if (value.isSymbol()) {
    return value.getSymbol(runtime).toString(runtime);
  } else {
    llvm_unreachable("No other values allowed in the trace");
  }
}
#endif

void SynthTrace::Record::toJSON(JSONEmitter &json) const {
  json.openDict();
  toJSONInternal(json);
  json.closeDict();
}

void SynthTrace::Record::toJSONInternal(JSONEmitter &json) const {
  json.emitKeyValue("type", recordTypeToString(getType()));
  json.emitKeyValue("time", time_.count());
}

void SynthTrace::MarkerRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("tag", tag_);
}

void SynthTrace::CreateObjectRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
}

void SynthTrace::CreateObjectWithPrototypeRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("prototype", encode(prototype_));
}

static std::string createBigIntMethodToString(
    SynthTrace::CreateBigIntRecord::Method m) {
  switch (m) {
    case SynthTrace::CreateBigIntRecord::Method::FromInt64:
      return "FromInt64";
    case SynthTrace::CreateBigIntRecord::Method::FromUint64:
      return "FromUint64";
    default:
      llvm_unreachable("No other valid CreateBigInt::Method.");
  }
}

void SynthTrace::CreateBigIntRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("method", createBigIntMethodToString(method_));
  // bits is potentially an invalid number (i.e., > 53 bits), so emit it
  // as a string.
  json.emitKeyValue("bits", std::to_string(bits_));
}

void SynthTrace::BigIntToStringRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("strID", strID_);
  json.emitKeyValue("bigintID", bigintID_);
  json.emitKeyValue("radix", radix_);
}

static std::string encodingName(SynthTrace::StringEncodingType encoding) {
  switch (encoding) {
    case SynthTrace::StringEncodingType::UTF8:
      return "UTF-8";
    case SynthTrace::StringEncodingType::ASCII:
      return "ASCII";
    case SynthTrace::StringEncodingType::UTF16:
      return "UTF-16";
    default:
      llvm_unreachable("Invalid encoding type encountered.");
  }
}

void SynthTrace::CreateStringRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("encoding", encodingName(encodingType_));
  if (encodingType_ == StringEncodingType::UTF16) {
    json.emitKeyValue(
        "chars", llvh::ArrayRef(chars16_.data(), chars16_.size()));
  } else {
    // For UTF-8 Strings, copy the content to a char16 array and emit each byte
    // as a code unit. This allows us to reconstruct the exact string
    // byte-for-byte during replay.
    std::vector<char16_t> char16Vector(
        (const unsigned char *)chars_.data(),
        (const unsigned char *)chars_.data() + chars_.size());
    json.emitKeyValue("chars", llvh::ArrayRef(char16Vector));
  }
}

void SynthTrace::CreatePropNameIDRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", propNameID_);
  json.emitKeyValue("encoding", encodingName(encodingType_));
  if (encodingType_ == StringEncodingType::UTF16) {
    json.emitKeyValue(
        "chars", llvh::ArrayRef(chars16_.data(), chars16_.size()));
  } else {
    // For UTF-8 Strings, copy the content to a char16 array and emit each byte
    // as a code unit. This allows us to reconstruct the exact string
    // byte-for-byte during replay.
    std::vector<char16_t> char16Vector(
        (const unsigned char *)chars_.data(),
        (const unsigned char *)chars_.data() + chars_.size());
    json.emitKeyValue("chars", llvh::ArrayRef(char16Vector));
  }
}

void SynthTrace::CreatePropNameIDWithValueRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", propNameID_);
  json.emitKeyValue("value", encode(traceValue_));
}

void SynthTrace::CreateHostFunctionRecord::toJSONInternal(
    JSONEmitter &json) const {
  CreateObjectRecord::toJSONInternal(json);
  json.emitKeyValue("propNameID", propNameID_);
  json.emitKeyValue("parameterCount", paramCount_);
#ifdef HERMESVM_API_TRACE_DEBUG
  json.emitKeyValue("functionName", functionName_);
#endif
}

void SynthTrace::GetPropertyRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("propID", encode(propID_));
#ifdef HERMESVM_API_TRACE_DEBUG
  json.emitKeyValue("propName", propNameDbg_);
#endif
}

void SynthTrace::SetPropertyRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("propID", encode(propID_));
#ifdef HERMESVM_API_TRACE_DEBUG
  json.emitKeyValue("propName", propNameDbg_);
#endif
  json.emitKeyValue("value", encode(value_));
}

void SynthTrace::SetPrototypeRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("value", encode(value_));
}

void SynthTrace::GetPrototypeRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
}

void SynthTrace::HasPropertyRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("propID", encode(propID_));
#ifdef HERMESVM_API_TRACE_DEBUG
  json.emitKeyValue("propName", propNameDbg_);
#endif
}

void SynthTrace::QueueMicrotaskRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("callbackID", callbackID_);
}

void SynthTrace::DrainMicrotasksRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("maxMicrotasksHint", maxMicrotasksHint_);
}

void SynthTrace::GetPropertyNamesRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
}

void SynthTrace::CreateArrayRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("length", length_);
}

void SynthTrace::CreateUInt8ArrayRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("length", length_);
}

void SynthTrace::CreateUInt8ArrayFromArrayBufferRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("bufferID", bufferID_);
  json.emitKeyValue("offset", offset_);
  json.emitKeyValue("length", length_);
}

void SynthTrace::GetBufferFromTypedArrayRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("bufferID", bufferID_);
  json.emitKeyValue("typedArrayID", typedArrayID_);
}

namespace {
static const std::unordered_map<SynthTrace::JSErrorType, const char *> &
jsErrorTypeToString() {
  static const std::unordered_map<SynthTrace::JSErrorType, const char *> value =
      {
          {SynthTrace::JSErrorType::Error, "Error"},
          {SynthTrace::JSErrorType::EvalError, "EvalError"},
          {SynthTrace::JSErrorType::RangeError, "RangeError"},
          {SynthTrace::JSErrorType::ReferenceError, "ReferenceError"},
          {SynthTrace::JSErrorType::SyntaxError, "SyntaxError"},
          {SynthTrace::JSErrorType::TypeError, "TypeError"},
          {SynthTrace::JSErrorType::URIError, "URIError"},
      };
  return value;
}
} // namespace

void SynthTrace::CreateJSErrorRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("errorType", jsErrorTypeToString().at(errorType_));
  json.emitKeyValue("messageID", messageID_);
}

void SynthTrace::ArrayReadRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("index", index_);
}

void SynthTrace::ArrayWriteRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("index", index_);
  json.emitKeyValue("value", encode(value_));
}

void SynthTrace::ArrayPushRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKey("elements");
  json.openArray();
  for (const TraceValue &elements : elements_) {
    json.emitValue(encode(elements));
  }
  json.closeArray();
  json.emitKeyValue("length", length_);
}

void SynthTrace::CallRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("functionID", functionID_);
  json.emitKeyValue("thisArg", encode(thisArg_));
  json.emitKey("args");
  json.openArray();
  for (const TraceValue &arg : args_) {
    json.emitValue(encode(arg));
  }
  json.closeArray();
}

void SynthTrace::BeginExecJSRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("sourceURL", sourceURL_);
  json.emitKeyValue("sourceHash", ::hermes::hashAsString(sourceHash_));
  json.emitKeyValue("sourceIsBytecode", sourceIsBytecode_);
}

void SynthTrace::ReturnMixin::toJSONInternal(JSONEmitter &json) const {
  json.emitKeyValue("retval", encode(retVal_));
}

void SynthTrace::EndExecJSRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  MarkerRecord::toJSONInternal(json);
  ReturnMixin::toJSONInternal(json);
}

void SynthTrace::ReturnFromNativeRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  Record::toJSONInternal(json);
  ReturnMixin::toJSONInternal(json);
}

void SynthTrace::ReturnToNativeRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  Record::toJSONInternal(json);
  ReturnMixin::toJSONInternal(json);
}

void SynthTrace::GetOrSetPropertyNativeRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("hostObjectID", hostObjectID_);
  json.emitKeyValue("propNameID", propNameID_);
  json.emitKeyValue("propName", propName_);
}

void SynthTrace::GetPropertyNativeReturnRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  ReturnMixin::toJSONInternal(json);
}

void SynthTrace::SetPropertyNativeRecord::toJSONInternal(
    JSONEmitter &json) const {
  GetOrSetPropertyNativeRecord::toJSONInternal(json);
  json.emitKeyValue("value", encode(value_));
}

void SynthTrace::GetNativePropertyNamesRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("hostObjectID", hostObjectID_);
}

void SynthTrace::GetNativePropertyNamesReturnRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKey("propNameIDs");
  json.openArray();
  for (const auto &prop : propNameIDs_) {
    json.emitValue(encode(prop));
  }
  json.closeArray();
}

void SynthTrace::SetExternalMemoryPressureRecord::toJSONInternal(
    JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("amount", amount_);
}

void SynthTrace::Utf8Record::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", encode(objID_));
  // For UTF-8 Strings, copy the content to a char16 array and emit each byte as
  // a code unit. This allows us to reconstruct the exact string byte-for-byte
  // during replay.
  std::vector<char16_t> char16Vector(
      (const unsigned char *)retVal_.data(),
      (const unsigned char *)retVal_.data() + retVal_.size());
  json.emitKeyValue("retval", llvh::ArrayRef(char16Vector));
}

void SynthTrace::Utf16Record::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", encode(objID_));
  json.emitKeyValue("retval", llvh::ArrayRef(retVal_.data(), retVal_.size()));
}

void SynthTrace::GetStringDataRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", encode(objID_));
  json.emitKeyValue(
      "strData", llvh::ArrayRef(strData_.data(), strData_.size()));
}

void SynthTrace::DeletePropertyRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
  json.emitKeyValue("propID", encode(propID_));
}

void SynthTrace::SerializeRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("value", encode(value_));
}

void SynthTrace::DeserializeRecord::toJSONInternal(
    ::hermes::JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKey("offsets");
  json.openArray();
  json.emitValues(llvh::ArrayRef<uint32_t>(offsets_.data(), offsets_.size()));
  json.closeArray();

  json.emitKey("content");
  json.openArray();
  json.emitValues(llvh::ArrayRef<uint8_t>(content_.data(), content_.size()));
  json.closeArray();

  json.emitKey("strings");
  json.openArray();
  json.emitValues(llvh::ArrayRef<uint8_t>(strings_.data(), strings_.size()));
  json.closeArray();
}

void SynthTrace::MathCallRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("function", function_);
  json.emitKey("args");
  json.openArray();
  for (double arg : args_) {
    json.emitValue(encode(encodeNumber(arg)));
  }
  json.closeArray();
  json.emitKeyValue("retval", encode(encodeNumber(retVal_)));
}

void SynthTrace::GlobalRecord::toJSONInternal(JSONEmitter &json) const {
  Record::toJSONInternal(json);
  json.emitKeyValue("objID", objID_);
}

const char *SynthTrace::nameFromReleaseUnused(::hermes::vm::ReleaseUnused ru) {
  switch (ru) {
    case ::hermes::vm::ReleaseUnused::kReleaseUnusedNone:
      return "none";
    case ::hermes::vm::ReleaseUnused::kReleaseUnusedOld:
      return "old";
    case ::hermes::vm::ReleaseUnused::kReleaseUnusedYoungOnFull:
      return "youngOnFull";
    case ::hermes::vm::ReleaseUnused::kReleaseUnusedYoungAlways:
      return "youngAlways";
    default:
      llvm_unreachable("No other valid value.");
  }
}

::hermes::vm::ReleaseUnused SynthTrace::releaseUnusedFromName(
    const char *rawName) {
  std::string name{rawName};
  if (name == "none") {
    return ::hermes::vm::ReleaseUnused::kReleaseUnusedNone;
  }
  if (name == "old") {
    return ::hermes::vm::ReleaseUnused::kReleaseUnusedOld;
  }
  if (name == "youngOnFull") {
    return ::hermes::vm::ReleaseUnused::kReleaseUnusedYoungOnFull;
  }
  if (name == "youngAlways") {
    return ::hermes::vm::ReleaseUnused::kReleaseUnusedYoungAlways;
  }
  throw std::invalid_argument("Name for ReleaseUnused not recognized");
}

void SynthTrace::flushAndDisable(const ::hermes::vm::GCExecTrace &gcTrace) {
  if (!json_) {
    return;
  }

  // Close the still-open "trace" array.
  json_->closeArray();

  // Now emit the history information, if we're in trace debug mode.
  gcTrace.emit(*json_);

  // Close the top level dictionary (the one opened in the ctor).
  json_->closeDict();

  // Now flush the stream, and reset the fields: further tracing doesn't make
  // sense.
  os().flush();
  json_.reset();
  traceStream_.reset();
}

} // namespace tracing
} // namespace hermes
} // namespace facebook
