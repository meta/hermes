/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/SerializedValue.h"

#include "JSLib/JSLibInternal.h"
#include "JSLib/Object.h"
#include "SerializationManagedValue.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSDataView.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSMapImpl.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/StringPrimitive.h"

#include <map>

#include "llvh/ADT/ScopeExit.h"

namespace hermes {
namespace vm {
namespace {

/// Pack with byte alignment because we need to serialize JS value data
/// directly into a binary buffer.
LLVM_PACKED_START
struct StringMetadataElement {
  bool isAscii;
  uint32_t len;
};
LLVM_PACKED_END

/// Simple struct to hold HermesValue and its hash for comparison against
/// SerializationManagedValue
struct HermesValueInfo {
  HermesValue hv;
  uint64_t hash;
};

/// During serialization, we track previously serialized values inside a
/// ManagedChunkList in entries called SerializationManagedValue. We also want
/// to create mapping of the value to serialization ID to reuse the Record.
/// In this mapping, we use the SerializationManagedValue pointers as key.
/// Given HermesValue, we check if there is a matching
/// SerializationManagedValue. Thus, we need to supply custom getHashValue and
/// isEqual methods to properly compare the two types.
struct SerializationValueInfo
    : llvh::DenseMapInfo<SerializationManagedValue *> {
  static inline SerializationManagedValue *getEmptyKey() {
    return llvh::DenseMapInfo<SerializationManagedValue *>::getEmptyKey();
  }

  static inline SerializationManagedValue *getTombstoneKey() {
    return llvh::DenseMapInfo<SerializationManagedValue *>::getTombstoneKey();
  }

  static inline unsigned getHashValue(const SerializationManagedValue *val) {
    return val->getHash();
  }

  static inline unsigned getHashValue(const HermesValueInfo &val) {
    return val.hash;
  }

  static inline bool isEqual(
      const SerializationManagedValue *lhs,
      const SerializationManagedValue *rhs) {
    return lhs == rhs;
  }

  /// Check if the HermesValueInfo, representing unserialized HermesValue, has
  /// an equivalent entry inside the map, which represents an already serialized
  /// HermesValue.
  static inline bool isEqual(
      const HermesValueInfo lhs,
      const SerializationManagedValue *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey()) {
      return false;
    }
    auto lhsHv = lhs.hv;
    auto rhsHv = rhs->value();
    return isSameValue(lhsHv, rhsHv);
  }
};

/// After serializing a HermesValue, add a mapping of the HermesValue to
/// serialization ID, so that we can reuse the records if the same HermesValue
/// is referenced multiple times. We also supply a custom "LookupKey"
/// (SerializationValueInfo) to compare a potentially unserialized HermesValue
/// to a serialized entry. See SerializationValueInfo for more info.
using SerializationValueDenseMap = llvh::
    DenseMap<SerializationManagedValue *, uint32_t, SerializationValueInfo>;

/// After deserializing a Record with a serialization ID, store create a mapping
/// of the ID to the deserialized HermesValue. This allows the "Reference"
/// records to find the appropriate HermesValue.
using DeserializationValueDenseMap =
    llvh::DenseMap<uint32_t, SerializationManagedValue *>;

ExecutionStatus serializeImpl(
    Runtime &runtime,
    Handle<> value,
    SerializedValue &serialized,
    SerializationValueDenseMap &memoryMap);

CallResult<HermesValue> deserializeImpl(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&curr,
    DeserializationValueDenseMap &memoryMap);

/// Convert a 64-bit hash value to a 32-bit hash value by adding the top and
/// bottom bits.
inline uint32_t getUInt32Hash(uint64_t hash) {
  uint32_t low = (uint32_t)hash;
  uint32_t high = (uint32_t)(hash >> 32);
  return low ^ high;
}

/// Convert the SerializedValue::Type \p type to uint8_t
inline uint8_t getUInt8FromSerializedType(SerializedValue::Type type) {
  return static_cast<uint8_t>(type);
}

/// Given the uint8_t pointed to at \p curr, return the SerializedValue type.
/// Update the pointer to point past the deserialized type.
inline SerializedValue::Type deserializeSerializedType(const uint8_t *&curr) {
  return static_cast<SerializedValue::Type>(*curr++);
}

/// Given \p value, write it to the end of \p buffer
template <typename value_type>
inline void appendValueToBuffer(
    std::vector<uint8_t> &buffer,
    const value_type &value) {
  const uint8_t *valPtr = reinterpret_cast<const uint8_t *>(&value);
  buffer.insert(buffer.end(), valPtr, valPtr + sizeof(value));
}

/// Given an uint32_t \p val, write its value starting at \p offset of \p
/// buffer. Callers should ensure that \p buffer has enough space to hold \p
/// val.
inline void copyUInt32ToBuffer(
    std::vector<uint8_t> &buffer,
    uint32_t offset,
    uint32_t val) {
  const uint8_t *bufPtr = reinterpret_cast<const uint8_t *>(&val);
  memcpy(buffer.data() + offset, bufPtr, sizeof(uint32_t));
}

/// Deserialize the uint32_t pointed by \p content and update the pointer to
/// point past the deserialized bytes.
inline uint32_t deserializeUInt32(const uint8_t *&content) {
  uint32_t val;
  memcpy(&val, content, sizeof(uint32_t));
  content += sizeof(uint32_t);
  return val;
}

/// Given a double \p value, convert it to a 64-bit representation and write to
/// the end of \p buffer.
inline void appendDoubleToBuffer(std::vector<uint8_t> &buffer, double value) {
  uint64_t valBits = llvh::DoubleToBits(value);
  appendValueToBuffer<uint64_t>(buffer, valBits);
}

/// Deserialize the double pointed by \p content and update the pointer to
/// point past the deserialized bytes.
inline double deserializeDouble(const uint8_t *&content) {
  uint64_t val;
  memcpy(&val, content, sizeof(uint64_t));
  content += sizeof(uint64_t);
  return llvh::BitsToDouble(val);
}

/// Serialize \p bigInt at the end of \p serialize with the format (size, data)
void serializeBigInt(SerializedValue &serialized, BigIntPrimitive *bigInt) {
  const ArrayRef<uint8_t> data = bigInt->getRawDataFull();
  appendValueToBuffer<uint32_t>(serialized.content, data.size());
  serialized.content.insert(serialized.content.end(), data.begin(), data.end());
}

/// Deserialize the data pointed by \p content into a BigIntPrimitive, and
/// update the pointer to point past the BigInt Record.
CallResult<PseudoHandle<BigIntPrimitive>> deserializeBigInt(
    Runtime &runtime,
    const uint8_t *&content) {
  uint32_t dataSize = deserializeUInt32(content);
  auto dataBytes = llvh::makeArrayRef(content, dataSize);
  content += dataSize;
  auto bigIntPrimRes = BigIntPrimitive::fromBytes(runtime, dataBytes);
  if (LLVM_UNLIKELY(bigIntPrimRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return createPseudoHandle(vmcast<BigIntPrimitive>(*bigIntPrimRes));
}

/// Determines the ID for \p strPrim and append it to the content buffer of \p
/// serialized. If \p stringPrim has not been serialized already, then append
/// its encoding in the strings buffer of \p serialized, assign it a string ID,
/// and add corresponding entry in the offset buffer of \p serialized.
/// Otherwise, look up the string ID in \p memoryMap.
/// The string in the string buffer will be serialized with the format (isAscii,
/// length, string data). If isAscii is true, then the string data should be
/// interpreted as a sequence of char. Else, it should be interpreted as a
/// sequence of char16_t.
void serializeString(
    Runtime &runtime,
    SerializedValue &serialized,
    StringPrimitive *strPrim,
    SerializationValueDenseMap &memoryMap) {
  NoAllocScope noAllocScope(runtime);

  uint32_t hash = strPrim->getOrComputeHash();
  // Check if the string has already been serialized. If so, append the string
  // ID and return
  auto strHv = HermesValue::encodeStringValue(strPrim);
  HermesValueInfo hvInfo{strHv, hash};
  if (auto it = memoryMap.find_as(hvInfo); it != memoryMap.end()) {
    appendValueToBuffer<uint32_t>(serialized.content, it->second);
    return;
  }
  // Otherwise, we need to serialize the string. First, find the starting offset
  // in the string buffer and the serialization ID.
  uint32_t startOffset = serialized.strings.size();
  uint32_t strId = serialized.offsets.size();
  // Then, store the id to offset mapping in the offsets vector.
  serialized.offsets.push_back(startOffset);
  auto *valuePtr = &runtime.serializationValues_.add(strHv, hash);
  // Also store the ID in the memory map for future look ups.
  memoryMap[valuePtr] = strId;

  // Encode the content of the string in the format (isAscii, length, data)
  bool isAscii = strPrim->isASCII();
  StringMetadataElement stringMetaData;
  stringMetaData.isAscii = isAscii;
  stringMetaData.len = strPrim->getStringLength();
  appendValueToBuffer<StringMetadataElement>(
      serialized.strings, stringMetaData);

  if (isAscii) {
    auto data = strPrim->getStringRef<char>();
    serialized.strings.insert(
        serialized.strings.end(), data.begin(), data.end());
  } else {
    auto data = strPrim->getStringRef<char16_t>();
    // We need to align these to char16_t for deserialization
    serialized.strings.resize(
        llvh::alignTo(serialized.strings.size(), sizeof(char16_t)));
    serialized.strings.insert(
        serialized.strings.end(),
        reinterpret_cast<const uint8_t *>(data.data()),
        reinterpret_cast<const uint8_t *>(data.data() + data.size()));
  }

  // Append the string ID to the content buffer
  appendValueToBuffer<uint32_t>(serialized.content, strId);
}

/// Deserializes the String identified by the ID at \p curr and return it as a
/// StringPrimitive. It will check the cache \p memoryMap to see if it has been
/// deserialized before. Otherwise, it performs the deserialization steps and
/// adds it to the cache. This method also updates the \p curr to point past
/// deserialized String ID.
CallResult<PseudoHandle<StringPrimitive>> deserializeString(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&curr,
    DeserializationValueDenseMap &memoryMap) {
  const std::vector<uint8_t> &strings = serialized.strings;
  const std::vector<uint32_t> &offsets = serialized.offsets;
  uint32_t strId = deserializeUInt32(curr);

  // If we've deserialized this string before, return the cached String
  if (auto it = memoryMap.find(strId); it != memoryMap.end()) {
    return createPseudoHandle(
        vmcast<StringPrimitive>(it->getSecond()->value()));
  }

  // Get the starting offset for where the string encoding is stored
  uint32_t currStringOffset = offsets[strId];

  const auto *metadata = reinterpret_cast<const StringMetadataElement *>(
      &strings[currStringOffset]);
  currStringOffset += sizeof(StringMetadataElement);

  StringPrimitive *str;
  if (metadata->isAscii) {
    auto dataBytes =
        llvh::makeArrayRef<uint8_t>(&strings[currStringOffset], metadata->len);

    auto result = StringPrimitive::createEfficient(runtime, dataBytes);
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    str = result->getString();
  } else {
    // Find the alignment for char16_t to find where the string data starts
    currStringOffset = llvh::alignTo(currStringOffset, alignof(char16_t));
    auto dataBytes = llvh::makeArrayRef<char16_t>(
        reinterpret_cast<const char16_t *>(&strings[currStringOffset]),
        metadata->len);
    auto result = StringPrimitive::createEfficient(runtime, dataBytes);
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    str = result->getString();
  }

  NoAllocScope noAllocScope(runtime);
  // Cache the deserialized string
  auto *valuePtr = &runtime.serializationValues_.add(
      HermesValue::encodeStringValue(str), str->getOrComputeHash());
  memoryMap[strId] = valuePtr;

  return createPseudoHandle(str);
}

/// Serialized the JS RegExp \p self at the end of \p serialized with the format
/// (pattern, flags).
/// Implements step 12 of StructuredSerializeInternal
void serializeRegExp(
    Runtime &runtime,
    SerializedValue &serialized,
    JSRegExp *regExp,
    SerializationValueDenseMap &memoryMap) {
  NoAllocScope noAllocScope(runtime);

  auto pattern = JSRegExp::getPattern(regExp, runtime);
  serializeString(runtime, serialized, pattern.get(), memoryMap);
  auto flags = JSRegExp::getSyntaxFlags(regExp);
  serialized.content.push_back(flags.toByte());
}

/// Deserialize the data pointed by \p content into a RegExp. Update the pointer
/// to point past the RegExp Record.
/// Implements step 11 of StructuredDeserialize
CallResult<PseudoHandle<JSRegExp>> deserializeRegExp(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&curr,
    DeserializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<JSRegExp> self;
    PinnedValue<StringPrimitive> pattern;
    PinnedValue<StringPrimitive> flags;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  auto patternRes = deserializeString(runtime, serialized, curr, memoryMap);
  if (LLVM_UNLIKELY(patternRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.pattern = std::move(*patternRes);

  auto flagsBytes = *curr++;
  auto flagsString = regex::SyntaxFlags::fromByte(flagsBytes).toString();
  auto flagStrRes = StringPrimitive::create(runtime, flagsString);
  if (LLVM_UNLIKELY(flagStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.flags = vmcast<StringPrimitive>(*flagStrRes);

  auto initializeRes = regExpCreate(
      runtime, lv.pattern, lv.flags, MutableHandle<JSRegExp>{lv.self});
  if (LLVM_UNLIKELY(initializeRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return createPseudoHandle(*lv.self);
}

/// Determines the Error type given a HermesValue \p name, which may be a String
/// or an arbitrary value.
/// Implements 17.2 for StructuredSerializeInternal
SerializedValue::ErrorType getErrorType(Runtime &runtime, HermesValue name) {
  NoAllocScope noAllocScope(runtime);
  // 2. If name is not one of "Error", "EvalError", "RangeError",
  // "ReferenceError", "SyntaxError", "TypeError", or "URIError", then set name
  // to "Error".
  if (name.isString()) {
    auto *nameStr = name.getString();
    if (nameStr->equals(runtime.getPredefinedString(Predefined::EvalError))) {
      return SerializedValue::ErrorType::EvalError;
    }
    if (nameStr->equals(runtime.getPredefinedString(Predefined::RangeError))) {
      return SerializedValue::ErrorType::RangeError;
    }
    if (nameStr->equals(
            runtime.getPredefinedString(Predefined::ReferenceError))) {
      return SerializedValue::ErrorType::ReferenceError;
    }
    if (nameStr->equals(runtime.getPredefinedString(Predefined::SyntaxError))) {
      return SerializedValue::ErrorType::SyntaxError;
    }
    if (nameStr->equals(runtime.getPredefinedString(Predefined::TypeError))) {
      return SerializedValue::ErrorType::TypeError;
    }
    if (nameStr->equals(runtime.getPredefinedString(Predefined::URIError))) {
      return SerializedValue::ErrorType::URIError;
    }
  }
  return SerializedValue::ErrorType::Error;
}

/// Returns the Error prototype based on \p name. Implements steps 21.1 - 21.7
/// of StructuredDeserialize
Handle<JSObject> getErrorPrototype(
    Runtime &runtime,
    SerializedValue::ErrorType errorType) {
  switch (errorType) {
    case SerializedValue::ErrorType::Error:
      // 1. Let prototype be %Error.prototype%.
      return Handle<JSObject>::vmcast(&runtime.ErrorPrototype);
    case SerializedValue::ErrorType::EvalError:
      // 2. If serialized.[[Name]] is "EvalError", then set prototype to
      // %EvalError.prototype%.
      return Handle<JSObject>::vmcast(&runtime.EvalErrorPrototype);
    case SerializedValue::ErrorType::RangeError:
      // 3. If serialized.[[Name]] is "RangeError", then set prototype to
      // %RangeError.prototype%.
      return Handle<JSObject>::vmcast(&runtime.RangeErrorPrototype);
    case SerializedValue::ErrorType::ReferenceError:
      // 4. If serialized.[[Name]] is "ReferenceError", then set prototype to
      // %ReferenceError.prototype%.
      return Handle<JSObject>::vmcast(&runtime.ReferenceErrorPrototype);
    case SerializedValue::ErrorType::SyntaxError:
      // 5. If serialized.[[Name]] is "SyntaxError", then set prototype to
      // %SyntaxError.prototype%.
      return Handle<JSObject>::vmcast(&runtime.SyntaxErrorPrototype);
    case SerializedValue::ErrorType::TypeError:
      // 6. If serialized.[[Name]] is "TypeError", then set prototype to
      // %TypeError.prototype%.
      return Handle<JSObject>::vmcast(&runtime.TypeErrorPrototype);
    case SerializedValue::ErrorType::URIError:
      // 7. If serialized.[[Name]] is "URIError", then set prototype to
      // %URIError.prototype%.
      return Handle<JSObject>::vmcast(&runtime.URIErrorPrototype);
    default:
      llvm_unreachable("Unhandled Error type encountered");
  }
}

/// Serialize the JS Error \p err at the end of \p serialize with the format
/// (error type, message).
/// Implements steps 17.1 - 17.5 of StructuredSerializeInternal
ExecutionStatus serializeJsError(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSError> err,
    SerializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<> tmp;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 1. Let name be ? Get(value, "name")
  auto nameRes = JSObject::getNamed_RJS(
      err, runtime, Predefined::getSymbolID(Predefined::name));
  if (LLVM_UNLIKELY(nameRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.tmp = std::move(*nameRes);
  // 2. If name is not one of "Error", "EvalError", "RangeError",
  // "ReferenceError", "SyntaxError", "TypeError", or "URIError", then set
  // name to "Error".
  SerializedValue::ErrorType errorType = getErrorType(runtime, *lv.tmp);
  serialized.content.push_back(static_cast<uint8_t>(errorType));

  // 3. Let valueMessageDesc be ? value.[[GetOwnProperty]]("message").
  NamedPropertyDescriptor desc;
  auto exists = JSObject::getOwnNamedDescriptor(
      err, runtime, Predefined::getSymbolID(Predefined::message), desc);
  // 4. Let message be undefined if IsDataDescriptor(valueMessageDesc) is
  //    false, and ? ToString(valueMessageDesc.[[Value]]) otherwise.
  if (!exists || desc.flags.accessor) {
    lv.tmp = HermesValue::encodeUndefinedValue();
  } else {
    auto getNamedRes = JSObject::getNamedSlotValue(err, runtime, desc);
    if (LLVM_UNLIKELY(getNamedRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.tmp = std::move(*getNamedRes);
    auto strRes = toString_RJS(runtime, lv.tmp);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.tmp = std::move(*strRes);
  }
  // 5. Set serialized to {[[Type]]: "Error", [[Name]]: name, [[Message]]:
  //    message}.
  serializeImpl(runtime, lv.tmp, serialized, memoryMap);
  return ExecutionStatus::RETURNED;
}

/// Deserialize the data pointed by \p content into a JS Error. Update the
/// pointer to point past the JS Error data in the content buffer.
/// Implements steps 21.1 - 21.7 of StructuredDeserialize
CallResult<PseudoHandle<JSError>> deserializeJsError(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&content,
    DeserializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<JSError> self;
    PinnedValue<StringPrimitive> str;
    PinnedValue<> tmp;
  } lv;
  LocalsRAII errorLraii{runtime, &lv};
  // Steps 21.1 - 21.7: Finds the error prototype
  auto type = static_cast<SerializedValue::ErrorType>(*content++);
  auto proto = getErrorPrototype(runtime, type);

  // 8. Let message be serialized.[[Message]].
  auto msgRes = deserializeImpl(runtime, serialized, content, memoryMap);
  if (LLVM_UNLIKELY(msgRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.tmp = *msgRes;
  assert(
      (lv.tmp->isUndefined() || lv.tmp->isString()) &&
      "Error message must be either be undefined or a String");

  // 9. Set value to OrdinaryObjectCreate(prototype, « [[ErrorData]] »).
  lv.self = JSError::create(runtime, proto);
  // 10. Let messageDesc be PropertyDescriptor { [[Value]]: message,
  // [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }.
  // 11. If message is not undefined, then perform !
  // OrdinaryDefineOwnProperty(value, "message", messageDesc).
  if (!lv.tmp->isUndefined()) {
    JSError::setMessage(lv.self, runtime, lv.tmp);
  }
  return createPseudoHandle(*lv.self);
}

/// Serialize the JS Array \p arr into the end of \p serialized with the format
/// (length). The actual content of the Array will be handled at Step 26.
/// Implements steps 18.1 - 18.3 of StructuredSerializeInternal
ExecutionStatus serializeArray(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSArray> arr) {
  // 1. Let valueLenDescriptor be ? OrdinaryGetOwnProperty(value, "length").
  // 2. Let valueLen be valueLenDescriptor.[[Value]].
  // 3. Set serialized to {[[Type]]: "Array", [[Length]]: valueLen,
  // [[Properties]]: a new empty List}.
  uint32_t len = JSArray::getLength(*arr, runtime);
  appendValueToBuffer<uint32_t>(serialized.content, len);
  return ExecutionStatus::RETURNED;
}

/// Serialize the JSMap \p selfMap into the end of \p serialized with the format
/// (num entries, pairs) where each pair is serialized records of (key, value).
/// Implements step 26.1 of StructuredSerializeInternal
ExecutionStatus serializeMap(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSMap> selfMap,
    SerializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<ArrayStorage> copy;
    PinnedValue<> tmp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};

  uint32_t mapSize = selfMap->size();
  appendValueToBuffer<uint32_t>(serialized.content, mapSize);

  // No entries to serialize if empty map, return early
  if (mapSize == 0) {
    return ExecutionStatus::RETURNED;
  }

  // 1. Let copiedList be a new empty List.
  // The array storage will have double the size of the map, since it will store
  // both the key and values next to each other.
  auto arrRes = ArrayStorage::create(runtime, mapSize * 2);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.copy.castAndSetHermesValue<ArrayStorage>(*arrRes);
  // 2. For each Record { [[Key]], [[Value]] } entry of value.[[MapData]]:
  //   1. Let copiedEntry be a new Record {[[Key]]: entry.[[Key]],
  //      [[Value]]: entry.[[Value]]}.
  //   2. If copiedEntry.[[Key]] is not the special value empty, append
  //      copiedEntry to copiedList.
  auto copyRes = JSMap::forEachNative(
      selfMap,
      runtime,
      [&lv](Runtime &runtime, HermesValue key, HermesValue value)
          -> ExecutionStatus {
        lv.tmp = key;
        auto res = ArrayStorage::push_back(lv.copy, runtime, lv.tmp);
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.tmp = value;
        res = ArrayStorage::push_back(lv.copy, runtime, lv.tmp);
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        return ExecutionStatus::RETURNED;
      });

  if (LLVM_UNLIKELY(copyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 3. For each Record { [[Key]], [[Value]] } entry of copiedList:
  //   1. Let serializedKey be ?
  //    StructuredSerializeInternal(entry.[[Key]], forStorage, memory).
  //   2. Let serializedValue be ?
  //    StructuredSerializeInternal(entry.[[Value]], forStorage, memory).
  //   3. Append {[[Key]]: serializedKey, [[Value]]: serializedValue} to
  //      serialized.[[MapData]].
  // We stored each key-value pair consecutively in the ArrayStorage copy, so we
  // can just iterate over the copy and serialize each value in order.
  for (size_t i = 0, e = lv.copy->size(); i < e; ++i) {
    marker.flush();
    lv.tmp = lv.copy->at(i);
    auto serializeRes = serializeImpl(runtime, lv.tmp, serialized, memoryMap);
    if (LLVM_UNLIKELY(serializeRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

/// Deserialize the entries pointed to by \p content and insert into \p selfMap
/// and update the pointer to point past the Map Record.
/// Implements 24.1.1 - 24.1.3 of the StructuredDeserialize
ExecutionStatus deserializeMapEntries(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&content,
    Handle<JSMap> selfMap,
    DeserializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<> key;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};
  auto numEntries = deserializeUInt32(content);
  // 1. For each Record { [[Key]], [[Value]] } entry of
  //   serialized.[[MapData]]:
  for (size_t i = 0; i < numEntries; ++i) {
    marker.flush();
    // 1. Let deserializedKey be ? StructuredDeserialize(entry.[[Key]],
    // targetRealm, memory).
    auto deserializeKeyRes =
        deserializeImpl(runtime, serialized, content, memoryMap);
    if (LLVM_UNLIKELY(deserializeKeyRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.key = *deserializeKeyRes;
    // Also free the handles created for the key before recursing, so they
    // don't accumulate with the nesting depth.
    marker.flush();

    // 2. Let deserializedValue be ? StructuredDeserialize(entry.[[Value]],
    // targetRealm, memory).
    auto deserializeValueRes =
        deserializeImpl(runtime, serialized, content, memoryMap);
    if (LLVM_UNLIKELY(deserializeValueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.value = *deserializeValueRes;

    // 3. Append {[[Key]]: deserializedKey, [[Value]]: deserializedValue} to
    // value.[[MapData]].
    auto insertRes = JSMap::insert(selfMap, runtime, lv.key, lv.value);
    if (LLVM_UNLIKELY(insertRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

/// Serialize the JSSet \p selfSet into the end of \p serialized with the format
/// (num elements, elements) where elements is a list of serialized Record for
/// each element in the Set.
/// Implements step 26.1 of StructuredSerializeInternal
ExecutionStatus serializeSet(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSSet> selfSet,
    SerializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<ArrayStorage> copy;
    PinnedValue<> tmp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};

  uint32_t setSize = selfSet->size();
  appendValueToBuffer<uint32_t>(serialized.content, setSize);

  // No elements to serialize if empty set, return early.
  if (setSize == 0) {
    return ExecutionStatus::RETURNED;
  }

  // 1. Let copiedList be a new empty List.
  auto arrRes = ArrayStorage::create(runtime, setSize);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.copy.castAndSetHermesValue<ArrayStorage>(*arrRes);
  // 2. For each entry of value.[[SetData]]:
  //   1. If entry is not the special value empty, append entry to
  //      copiedList.
  auto copyRes = JSSet::forEachNative(
      selfSet,
      runtime,
      [&lv](Runtime &runtime, HermesValue key) -> ExecutionStatus {
        lv.tmp = key;
        auto res = ArrayStorage::push_back(lv.copy, runtime, lv.tmp);
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        return ExecutionStatus::RETURNED;
      });

  if (LLVM_UNLIKELY(copyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 3. For each entry of copiedList:
  for (size_t i = 0; i < setSize; ++i) {
    marker.flush();
    lv.tmp = lv.copy->at(i);
    // 1. Let serializedEntry be ? StructuredSerializeInternal(entry,
    //    forStorage, memory).
    // 2. Append serializedEntry to serialized.[[SetData]].
    auto serializeRes = serializeImpl(runtime, lv.tmp, serialized, memoryMap);
    if (LLVM_UNLIKELY(serializeRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

/// Deserialize the elements pointed to by \p content and insert into \p selfSet
/// and update the pointer to point past Set Record.
/// Implements 24.2.1 - 24.2.3 of the StructuredDeserialize
ExecutionStatus deserializeSetElements(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&content,
    Handle<JSSet> selfSet,
    DeserializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<> tmp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};
  auto numElems = deserializeUInt32(content);
  // 2. Otherwise, if serialized.[[Type]] is "Set", then:
  //   1. For each entry of serialized.[[SetData]]:
  for (size_t i = 0; i < numElems; ++i) {
    marker.flush();
    // 1. Let deserializedEntry be ? StructuredDeserialize(entry,
    // targetRealm, memory)
    // 2. Append deserializedEntry to value.[[SetData]].
    auto deserializeValueRes =
        deserializeImpl(runtime, serialized, content, memoryMap);
    if (LLVM_UNLIKELY(deserializeValueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.tmp = *deserializeValueRes;

    // 3. Append {[[Key]]: deserializedKey, [[Value]]: deserializedValue}
    // to value.[[MapData]].
    JSSet::insert(selfSet, runtime, lv.tmp);
  }
  return ExecutionStatus::RETURNED;
}

/// Serialize all property listed \p properties starting at \p index into the
/// end of \p serialized with the format (num elements, elements) where elements
/// is a list of (property key string, value record).
/// \pre all elements in \p properties must be a StringPrimitive
/// Helps with step 26.4 of StructuredSerializeInternal
ExecutionStatus serializeProperties(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSObject> selfObject,
    Handle<JSArray> properties,
    uint32_t index,
    SerializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<> tmp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};

  // The for-loop below can invoke getters, which can run some arbitrary JS that
  // modifies the number of properties on this object. Thus, write a dummy value
  // first, then write the actual number of serialized properties at the end.
  uint32_t numProp = 0;
  size_t numPropOffset = serialized.content.size();
  appendValueToBuffer<uint32_t>(serialized.content, numProp);
  for (JSArray::size_type i = index,
                          e = JSArray::getLength(*properties, runtime);
       i < e;
       i++) {
    marker.flush();
    auto key = properties->at(runtime, i);
    lv.tmp = key.unboxToHV(runtime);
    // 1. If !HasOwnPropertyKey(value, key) is true, then:
    ComputedPropertyDescriptor desc;
    auto getOwnRes =
        JSObject::getOwnComputedDescriptor(selfObject, runtime, lv.tmp, desc);
    if (LLVM_UNLIKELY(getOwnRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_LIKELY(*getOwnRes)) {
      auto *stringPrim = vmcast<StringPrimitive>(*lv.tmp);
      serializeString(runtime, serialized, stringPrim, memoryMap);
      // 1. Let inputValue be ? value.[[Get]] (key, value)
      // Proxys are not serializable, so we can call the Internal version here
      auto inputValue = JSObject::getComputedPropertyValueInternal_RJS(
          selfObject, runtime, selfObject, desc);
      if (LLVM_UNLIKELY(inputValue == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.tmp = std::move(*inputValue);
      // Also free the handles created in this iteration before recursing, so
      // they don't accumulate with the nesting depth.
      marker.flush();
      // 2. Let outputValue be ? StructuredSerializeInternal(inputValue,
      // forStorage, memory)
      // 3. Append {[[Key]]: key, [[Value]]: outputValue} to
      // serialized.[[Properties]].
      auto serializeRes = serializeImpl(runtime, lv.tmp, serialized, memoryMap);
      if (LLVM_UNLIKELY(serializeRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      numProp++;
    }
  }
  // Store the num of properties
  copyUInt32ToBuffer(serialized.content, numPropOffset, numProp);
  return ExecutionStatus::RETURNED;
}

/// Serialize the properties of the JS Array \p selfArray into the end of \p
/// serialized. It first serializes the array index properties in the format
/// (num indexed elements, elements), where elements is a list of pairs (index
/// number, value record). Then, it serializes the named properties in the
/// format (num properties, properties), where properties is a list of pairs
/// (property key string, value record).
/// Implements step 26.4 of StructuredSerializeInternal for JS Arrays.
ExecutionStatus serializeArrayProperties(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSArray> selfArray,
    SerializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<JSArray> propertyKeys;
    PinnedValue<> tmp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};
  // 4. Otherwise, for each key in ! EnumerableOwnProperties(value, key):
  // This will return all the enumerable property key of the array. First, it
  // will list the array indices, as numbers, in numerical value. Then, it will
  // list all the string properties. Symbols are not included.
  auto propKeyResult = JSArray::getOwnPropertyKeys(
      selfArray, runtime, OwnKeysFlags().plusIncludeNonSymbols());
  if (LLVM_UNLIKELY(propKeyResult == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propertyKeys = *propKeyResult;
  uint32_t keysLength = JSArray::getLength(*lv.propertyKeys, runtime);
  uint32_t firstStringPropIndex = keysLength;

  // The for-loop below can invoke getters, which can run some arbitrary JS that
  // modifies the number of properties on this object. Thus, write a dummy value
  // first for the number of array indices.
  uint32_t numIndexedProp = 0;
  size_t numIndexPropOffset = serialized.content.size();
  appendValueToBuffer<uint32_t>(serialized.content, numIndexedProp);
  for (JSArray::size_type i = 0; i < keysLength; i++) {
    marker.flush();
    auto key = lv.propertyKeys->at(runtime, i);
    lv.tmp = key.unboxToHV(runtime);
    // Not an index. Since propertyKeys lists array indices first, then string
    // properties. We know all elements after this will be strings.
    if (!lv.tmp->isNumber()) {
      firstStringPropIndex = i;
      break;
    }
    // 1. If !HasOwnPropertyKey(value, key) is true, then:
    ComputedPropertyDescriptor desc;
    auto getOwnRes =
        JSObject::getOwnComputedDescriptor(selfArray, runtime, lv.tmp, desc);
    if (LLVM_UNLIKELY(getOwnRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_UNLIKELY(!*getOwnRes)) {
      // HasOwnPropertyKey is false, skip
      continue;
    }
    // Serialize the array index property key
    uint32_t arrayIndex;
    [[maybe_unused]] bool convertRes =
        sh_tryfast_f64_to_u32(lv.tmp->getNumber(), arrayIndex);
    assert(convertRes && "Array index must be an uint32");
    appendValueToBuffer<uint32_t>(serialized.content, arrayIndex);

    // Let inputValue be ? value.[[Get]](key, value).
    // Fast-path: Access the index element directly.
    if (LLVM_LIKELY(selfArray->hasFastIndexProperties(runtime))) {
      SmallHermesValue shv = selfArray->at(runtime, arrayIndex);
      assert(!shv.isEmpty() && "Accessed empty value while serializing Array");
      lv.tmp = shv.unboxToHV(runtime);
    } else {
      // Slow-path
      auto inputValue = JSObject::getComputedPropertyValueInternal_RJS(
          selfArray, runtime, selfArray, desc);
      if (LLVM_UNLIKELY(inputValue == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.tmp = std::move(*inputValue);
    }
    // Also free the handles created in this iteration before recursing, so
    // they don't accumulate with the nesting depth.
    marker.flush();
    // Serialize the property value
    // 2. Let outputValue be ? StructuredSerializeInternal(inputValue,
    // forStorage, memory)
    // 3. Append {[[Key]]: key, [[Value]]: outputValue} to
    // serialized.[[Properties]].
    auto serializeRes = serializeImpl(runtime, lv.tmp, serialized, memoryMap);
    if (LLVM_UNLIKELY(serializeRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    numIndexedProp++;
  }
  // Store the num of properties
  copyUInt32ToBuffer(serialized.content, numIndexPropOffset, numIndexedProp);

  // Now, process the string property keys.
  auto res = serializeProperties(
      runtime,
      serialized,
      selfArray,
      lv.propertyKeys,
      firstStringPropIndex,
      memoryMap);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return ExecutionStatus::RETURNED;
}

/// Deserialize the properties data pointed by \p content and set each
/// property into \p self, and update the pointer to point past the property
/// data. These properties will be inserted without accounting for index-like
/// properties.
/// Implements 24.3.1 - 24.3.3 of the StructuredDeserialize
ExecutionStatus deserializeProperties(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&content,
    Handle<JSObject> selfObject,
    DeserializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<StringPrimitive> keyStrPrim;
    PinnedValue<SymbolID> keySym;
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};
  uint32_t numProperties = deserializeUInt32(content);
  // 1. For each Record {[[Key]], [[Value]]} entry of
  // serialized.[[Properties]]:
  for (uint32_t i = 0; i < numProperties; i++) {
    auto deserializeKeyRes =
        deserializeString(runtime, serialized, content, memoryMap);
    if (LLVM_UNLIKELY(deserializeKeyRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.keyStrPrim = std::move(*deserializeKeyRes);
    // Free the handles created so far, including any for the key, before
    // recursing, so they don't accumulate with the nesting depth.
    marker.flush();

    // 1. Let deserializedValue be ?StructuredDeserialize(entry.[[Value]],
    // targetRealm, memory).
    auto deserializeValueRes =
        deserializeImpl(runtime, serialized, content, memoryMap);
    if (LLVM_UNLIKELY(deserializeValueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.val = std::move(*deserializeValueRes);

    // 2. Let result be ! CreateDataProperty(value, entry.[[Key]],
    // deserializedValue).
    auto symbolIDRes =
        stringToSymbolID(runtime, createPseudoHandle(*lv.keyStrPrim));
    if (LLVM_UNLIKELY(symbolIDRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.keySym = std::move(*symbolIDRes);
    auto result = JSObject::defineNewOwnProperty(
        selfObject,
        runtime,
        *lv.keySym,
        PropertyFlags().defaultNewNamedPropertyFlags(),
        lv.val);
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

/// Deserialize the properties data, which may contain both index and string
/// properties, pointed by \p content into \p selfArray. Update the pointer \p
/// content to point past the Array Record. Index like properties will be
/// attempted to be inserted directly into the indexed storage.
/// Implements 24.3.1 - 24.3.3 of the StructuredDeserialize for JS Array.
ExecutionStatus deserializeArrayProperties(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&content,
    Handle<JSArray> selfArray,
    DeserializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};

  // Process the index properties
  uint32_t numIndexProperties = deserializeUInt32(content);
  // 1. For each Record { [[Key]], [[Value]] } entry of
  // serialized.[[Properties]]:
  for (uint32_t i = 0; i < numIndexProperties; ++i) {
    uint32_t arrayIndex = deserializeUInt32(content);

    // 1. Let deserializedValue be ? StructuredDeserialize(entry.[[Value]],
    // targetRealm, memory).
    auto deserializedValueRes =
        deserializeImpl(runtime, serialized, content, memoryMap);
    if (LLVM_UNLIKELY(deserializedValueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.val = std::move(*deserializedValueRes);

    // 2. Let result be ! CreateDataProperty(value, entry.[[Key]],
    // deserializedValue). We are dealing with index properties in a JSArray,
    // try to set into the storage directly. The length property will not be
    // updated by this operation, however, we don't care because the
    // algorithm sets the length elsewhere.
    auto setRes =
        selfArray->setElementAt(selfArray, runtime, arrayIndex, lv.val);
    if (LLVM_UNLIKELY(setRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // Flush here rather than at the start of the iteration, so that the last
    // element's handles are also freed before recursing into named properties.
    marker.flush();
  }

  // Process all other properties
  auto deserializePropRes =
      deserializeProperties(runtime, serialized, content, selfArray, memoryMap);
  if (LLVM_UNLIKELY(deserializePropRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

/// Serialize the properties of the JS Object \p selfObject into the end of \p
/// serialized with the format (num elements, elements) where elements is a list
/// of (property key string, value record).
/// Implements step 26.4 of StructuredSerializeInternal for Objects.
ExecutionStatus serializeObjectProperties(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSObject> selfObject,
    SerializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<JSArray> propertyKeys;
    PinnedValue<> tmp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  // 4. Otherwise, for each key in !EnumerableOwnProperties(value, key)
  // This excludes Symbol property keys. It only includes String property keys
  // for ordinary JS Objects, as well as array indices for JS Arrays.
  auto propRes = enumerableOwnProperties_RJS(
      runtime, selfObject, EnumerableOwnPropertiesKind::Key);
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propertyKeys = vmcast<JSArray>(*propRes);

  auto serializeRes = serializeProperties(
      runtime, serialized, selfObject, lv.propertyKeys, 0, memoryMap);
  if (LLVM_UNLIKELY(serializeRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

/// Serialize the JS ArrayBuffer \p arrayBuffer at the end of \p serialized
/// with the format (size, data)
/// Implements 13.2 of StructuredSerializeInternal
ExecutionStatus serializeArrayBuffer(
    Runtime &runtime,
    SerializedValue &serialized,
    JSArrayBuffer *arrayBuffer) {
  // 1. If IsDetachedBuffer(value) is true, then throw a "DataCloneError"
  // DOMException.
  if (!arrayBuffer->attached()) {
    return runtime.raiseError("Detached ArrayBuffers are not serializable");
  }

  NoAllocScope noAllocScope(runtime);
  // 2. Let size be value.[[ArrayBufferByteLength]].
  auto size = arrayBuffer->size();
  appendValueToBuffer<uint32_t>(serialized.content, size);
  // 3. Let dataCopy be ? CreateByteDataBlock(size).
  // 4. Perform CopyDataBlockBytes(dataCopy, 0, value.[[ArrayBufferData]], 0,
  // size).
  auto *bufferData = arrayBuffer->getDataBlock();
  serialized.content.insert(
      serialized.content.end(), bufferData, bufferData + size);

  // 5. Resizable ArrayBuffers case: Not supported
  // 6. Otherwise, set serialized to { [[Type]]: "ArrayBuffer",
  // [[ArrayBufferData]]: dataCopy, [[ArrayBufferByteLength]]: size }.
  return ExecutionStatus::RETURNED;
}

/// Deserializes the data pointed by \p content into a JS ArrayBuffer. Update
/// the pointer to point past the ArrayBuffer Record.
/// Implements step 14 of  StructuredDeserialize
CallResult<PseudoHandle<JSArrayBuffer>> deserializeArrayBuffer(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&content) {
  // Otherwise, if serialized.[[Type]] is "ArrayBuffer", then set value to a new
  // ArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot
  // value is serialized.[[ArrayBufferData]], and whose
  // [[ArrayBufferByteLength]] internal slot value is
  // serialized.[[ArrayBufferByteLength]].
  uint32_t size = deserializeUInt32(content);

  struct : Locals {
    PinnedValue<JSArrayBuffer> self;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto jsArrayBuffer =
      JSArrayBuffer::create(runtime, runtime.arrayBufferPrototype);
  lv.self = std::move(jsArrayBuffer);

  auto allocateRes = JSArrayBuffer::createDataBlock(runtime, lv.self, size);
  if (LLVM_UNLIKELY(allocateRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (size > 0) {
    auto *block = lv.self->getDataBlock();
    memcpy(block, content, size);
    content += size;
  }
  return createPseudoHandle(*lv.self);
}

/// Serialize the JS ArrayBuffer \p arrayBuffer for transfer at the end of \p
/// serialized with the format (size, index in internal buffer list) for
/// internal buffers. For external buffer, use the format (size, data pointer,
/// index in external buffer context list).
/// Implements 5.4.2.2 and 5.4.2.3 of StructuredSerializeWithTransfer
void serializeArrayBufferForTransfer(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSArrayBuffer> selfArrayBuffer) {
  // 5.4.2.3. Set dataHolder.[[ArrayBufferByteLength]] to
  // transferable.[[ArrayBufferByteLength]].
  uint32_t size = selfArrayBuffer->size();
  appendValueToBuffer<uint32_t>(serialized.content, size);

  // 5.4.2.2. Set dataHolder.[[ArrayBufferData]] to
  // transferable.[[ArrayBufferData]].
  uint8_t *bufferData = selfArrayBuffer->getDataBlock();

  if (selfArrayBuffer->external()) {
    uint32_t idx = serialized.externalBuffers.size();
    appendValueToBuffer<uint32_t>(serialized.content, idx);
    std::shared_ptr<void> ctx =
        JSArrayBuffer::getExternalDataContext(runtime, selfArrayBuffer);
    serialized.externalBuffers.emplace_back(std::pair{bufferData, ctx});
  } else {
    uint32_t idx = serialized.internalBuffers.size();
    appendValueToBuffer<uint32_t>(serialized.content, idx);
    serialized.internalBuffers.push_back(bufferData);
  }
}

/// Deserializes the data pointed by \p content into a JS ArrayBuffer. Update
/// the pointer to point past the ArrayBuffer record. Implements step 3.2 of
/// StructuredDeserializeWithTransfer
PseudoHandle<JSArrayBuffer> deserializeArrayBufferForTransfer(
    Runtime &runtime,
    SerializedValue &serialized,
    const uint8_t *&content,
    bool isInternalBuffer) {
  uint32_t size = deserializeUInt32(content);

  // Internal buffer case
  if (isInternalBuffer) {
    uint32_t idx = deserializeUInt32(content);
    uint8_t *data = serialized.internalBuffers[idx];
    PseudoHandle<JSArrayBuffer> jsArrayBuffer =
        JSArrayBuffer::createWithInternalDataBlock(
            runtime, runtime.arrayBufferPrototype, data, size);
    /// Set the corresponding entry to null, so the serialized object no longer
    /// holds ownership over the data.
    serialized.internalBuffers[idx] = nullptr;
    return jsArrayBuffer;
  }

  // External buffer case
  struct : Locals {
    PinnedValue<JSArrayBuffer> self;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto jsArrayBuffer =
      JSArrayBuffer::create(runtime, runtime.arrayBufferPrototype);
  lv.self = std::move(jsArrayBuffer);

  uint32_t idx = deserializeUInt32(content);
  auto [data, context] = serialized.externalBuffers[idx];
  JSArrayBuffer::setExternalDataBlock(runtime, lv.self, data, size, context);
  return createPseudoHandle(*lv.self);
}

/// Serialize the JS DataView \p selfView at the end of \p serialized with the
/// format (array buffer, byte length, byte offset).
/// Implements 14.1-14.5 of StructuredSerializeInternal
ExecutionStatus serializeDataView(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSDataView> selfView,
    SerializationValueDenseMap &memoryMap) {
  Handle<JSArrayBuffer> buffer = selfView->getBuffer(runtime);
  uint32_t byteLength = selfView->byteLength();
  uint32_t byteOffset = selfView->byteOffset();
  // 1. If IsArrayBufferViewOutOfBounds(value) is true, then throw a
  // "DataCloneError" DOMException.
  if (!selfView->attached(runtime) ||
      byteOffset + byteLength > buffer->size()) {
    return runtime.raiseError("DataView is out of bounds");
  }
  // 2. Let buffer be the value of value's [[ViewedArrayBuffer]] internal slot.
  // 3. Let bufferSerialized be ? StructuredSerializeInternal(buffer,
  // forStorage, memory).
  // 4. Assert: bufferSerialized.[[Type]] is "ArrayBuffer",
  // "ResizableArrayBuffer", "SharedArrayBuffer", or
  // "GrowableSharedArrayBuffer".

  auto res = serializeImpl(runtime, buffer, serialized, memoryMap);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 5. If value has a [[DataView]] internal slot, then set serialized to {
  // [[Type]]: "ArrayBufferView", [[Constructor]]: "DataView",
  // [[ArrayBufferSerialized]]: bufferSerialized, [[ByteLength]]:
  // value.[[ByteLength]], [[ByteOffset]]: value.[[ByteOffset]] }.
  // Note that instead of serializing a constructor field, we just use a
  // DataView-specific type tag
  appendValueToBuffer<uint32_t>(serialized.content, byteLength);
  appendValueToBuffer<uint32_t>(serialized.content, byteOffset);

  return ExecutionStatus::RETURNED;
}

/// Deserializes the data pointed by \p content into a JS DataView. Update the
/// pointer to point past the DataView Record.
/// Implements 16.1-16.2 of StructuredDeserialize
CallResult<PseudoHandle<JSDataView>> deserializeDataView(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&content,
    DeserializationValueDenseMap &memoryMap) {
  // 1. Let deserializedArrayBuffer be ?
  // StructuredDeserialize(serialized.[[ArrayBufferSerialized]], targetRealm,
  // memory).
  struct : Locals {
    PinnedValue<JSArrayBuffer> buffer;
    PinnedValue<JSDataView> self;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  auto res = deserializeImpl(runtime, serialized, content, memoryMap);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.buffer = vmcast<JSArrayBuffer>(*res);

  uint32_t byteLength = deserializeUInt32(content);
  uint32_t byteOffset = deserializeUInt32(content);

  lv.self = JSDataView::create(runtime, runtime.dataViewPrototype);

  lv.self->setBuffer(runtime, *lv.buffer, byteOffset, byteLength);
  return createPseudoHandle(*lv.self);
}

/// Serialize the JS TypedArray \p self at the end of \p serialized with the
/// format (array buffer, byte length, byte offset).
/// Implements 14.1-14.4, 14.6 of StructuredSerializeInternal
ExecutionStatus serializeTypedArray(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSTypedArrayBase> self,
    SerializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<JSArrayBuffer> buffer;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.buffer = self->getBuffer(runtime);
  uint32_t byteLength = self->getByteLength();
  uint32_t byteOffset = self->getByteOffset();
  // 1. If IsArrayBufferViewOutOfBounds(value) is true, then throw a
  // "DataCloneError" DOMException.
  if (!self->attached(runtime) || byteOffset + byteLength > lv.buffer->size()) {
    return runtime.raiseError("TypedArray is out of bounds");
  }
  // 2. Let buffer be the value of value's [[ViewedArrayBuffer]] internal slot.
  // 3. Let bufferSerialized be ? StructuredSerializeInternal(buffer,
  // forStorage, memory).
  // 4. Assert: bufferSerialized.[[Type]] is "ArrayBuffer",
  // "ResizableArrayBuffer", "SharedArrayBuffer", or
  // "GrowableSharedArrayBuffer".

  auto res = serializeImpl(runtime, lv.buffer, serialized, memoryMap);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 6. Otherwise:
  //  1. Assert: value has a [[TypedArrayName]] internal slot.
  //  2. Set serialized to { [[Type]]: "ArrayBufferView", [[Constructor]]:
  //  value.[[TypedArrayName]], [[ArrayBufferSerialized]]: bufferSerialized,
  //  [[ByteLength]]: value.[[ByteLength]], [[ByteOffset]]:
  //  value.[[ByteOffset]], [[ArrayLength]]: value.[[ArrayLength]] }.
  // Instead of serializing the constructor, we handle the array type using the
  // Record type tags. Also note array length is not serialized since it can be
  // calculated.
  appendValueToBuffer<uint32_t>(serialized.content, byteLength);
  appendValueToBuffer<uint32_t>(serialized.content, byteOffset);
  return ExecutionStatus::RETURNED;
}

/// Deserializes the data pointed by \p content into a JS TypedArray with the
/// type specified by \p typeTag. Update the pointer to point past the
/// TypedArray Record.
/// Implements 16.1, 16.3 of StructuredDeserialize
CallResult<PseudoHandle<JSTypedArrayBase>> deserializeTypedArray(
    Runtime &runtime,
    SerializedValue::Type typeTag,
    const SerializedValue &serialized,
    const uint8_t *&content,
    DeserializationValueDenseMap &memoryMap) {
  struct : Locals {
    PinnedValue<JSTypedArrayBase> self;
    PinnedValue<JSArrayBuffer> buffer;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 16.1 Let deserializedArrayBuffer be ?
  // StructuredDeserialize(serialized.[[ArrayBufferSerialized]], targetRealm,
  // memory).
  auto bufferRes = deserializeImpl(runtime, serialized, content, memoryMap);
  if (LLVM_UNLIKELY(bufferRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.buffer = vmcast<JSArrayBuffer>(*bufferRes);

  // 16.3 Otherwise, set value to a new typed array object in targetRealm, using
  // the constructor given by serialized.[[Constructor]], whose
  // [[ViewedArrayBuffer]] internal slot value is deserializedArrayBuffer, whose
  // [[TypedArrayName]] internal slot value is serialized.[[Constructor]], whose
  // [[ByteLength]] internal slot value is serialized.[[ByteLength]], whose
  // [[ByteOffset]] internal slot value is serialized.[[ByteOffset]], and whose
  // [[ArrayLength]] internal slot value is serialized.[[ArrayLength]].
#define TYPED_ARRAY(name, type)                                             \
  if (typeTag == SerializedValue::Type::name##Array) {                      \
    lv.self = JSTypedArray<type, CellKind::name##ArrayKind>::create(        \
        runtime, runtime.name##ArrayPrototype, runtime.class##name##Array); \
  }
#include "hermes/VM/TypedArrays.def"
  uint8_t width = lv.self->getByteWidth();
  uint32_t byteLength = deserializeUInt32(content);
  uint32_t offset = deserializeUInt32(content);
  JSTypedArrayBase::setBuffer(
      runtime, *lv.self, *lv.buffer, offset, byteLength, width);
  return createPseudoHandle(*lv.self);
}

/// If \p value has not been serialized before, append its serialized Record
/// onto the content buffer of \p serialized, adding any information
/// necessary in its string and offset buffer as well. Store an entry in \p
/// map for this value and its record ID. Else, if \p value has been
/// serialized, append a Reference record instead using the ID stored in \p
/// map.
ExecutionStatus serializeImpl(
    Runtime &runtime,
    Handle<> value,
    SerializedValue &serialized,
    SerializationValueDenseMap &memoryMap) {
  auto &content = serialized.content;
  auto &offsets = serialized.offsets;
  // 1. If memory was not supplied, let memory be an empty map.
  // 2. If memory[value] exists, then return memory[value].
  auto hash = getUInt32Hash(runtime.gcStableHashHermesValue(*value));
  HermesValueInfo hvInfo{*value, hash};
  if (auto it = memoryMap.find_as(hvInfo); it != memoryMap.end()) {
    // Append the reference record to the content buffer
    content.push_back(
        getUInt8FromSerializedType(SerializedValue::Type::Reference));
    appendValueToBuffer<uint32_t>(content, it->second);
    return ExecutionStatus::RETURNED;
  }

  // 3. Let deep be false
  auto deep = false;

  // 4. If Value is undefined, null, a Boolean, a Number, a BigInt, or a
  // String, then return {[[Type]]: "primitive , [[Value]]: value}
  if (value->isUndefined()) {
    content.push_back(
        getUInt8FromSerializedType(SerializedValue::Type::Undefined));
    return ExecutionStatus::RETURNED;
  }
  if (value->isNull()) {
    content.push_back(getUInt8FromSerializedType(SerializedValue::Type::Null));
    return ExecutionStatus::RETURNED;
  }
  if (value->isBool()) {
    content.push_back(
        getUInt8FromSerializedType(SerializedValue::Type::PrimitiveBoolean));
    content.push_back(value->getBool());
    return ExecutionStatus::RETURNED;
  }
  if (value->isNumber()) {
    content.push_back(
        getUInt8FromSerializedType(SerializedValue::Type::PrimitiveNumber));
    appendDoubleToBuffer(content, value->getNumber());
    return ExecutionStatus::RETURNED;
  }
  if (value->isBigInt()) {
    auto *bigIntPrim = value->getBigInt();
    content.push_back(
        getUInt8FromSerializedType(SerializedValue::Type::PrimitiveBigInt));
    serializeBigInt(serialized, bigIntPrim);
    return ExecutionStatus::RETURNED;
  }
  if (value->isString()) {
    auto *stringPrim = value->getString();
    content.push_back(
        getUInt8FromSerializedType(SerializedValue::Type::PrimitiveString));
    serializeString(runtime, serialized, stringPrim, memoryMap);
    return ExecutionStatus::RETURNED;
  }

  // 5. If it is a Symbol, then throw a "DataCloneError" DOMException
  if (value->isSymbol()) {
    return runtime.raiseError("Symbols are not serializable");
  }

  /// Use Handle::vmcast here to assert that the value must be an JS object at
  /// this point.
  auto selfObjHandle = Handle<JSObject>::vmcast(value);
  if (selfObjHandle->isHostObject(runtime)) {
    return runtime.raiseError("Host Objects are not serializable");
  }

  // All object types will be serialized with the following format:
  // (type tag, object ID, content).
  // Find the offset that points to the start of the serialized object, where
  // the type tag will be serialized. Assign the object an ID, which other
  // records can use to reference this object.
  uint32_t typeTagOffset = content.size();
  uint32_t id = offsets.size();

  // Place a dummy value for the type tag for now, then write the object id.
  content.push_back(0);
  appendValueToBuffer<uint32_t>(content, id);

  // Add an object ID -> typeTagOffset entry in the offset array.
  offsets.push_back(typeTagOffset);

  // 6. Let serialized be an uninitialized value.
  if (auto *jsBool = dyn_vmcast<JSBoolean>(*value)) {
    // 7. If value has a [[BooleanData]] internal slot, then set serialized to
    //    {[[Type]]: "Boolean", [[BooleanData]]: value.[[BooleanData]]}.
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::Boolean);
    content.push_back(jsBool->getPrimitiveBoolean());
  } else if (auto *jsNumber = dyn_vmcast<JSNumber>(*value)) {
    // 8. Otherwise, if value has a [[NumberData]] internal slot, then set
    //    serialized to {[[Type]]: "Number", [[NumberData]]:
    //    value.[[NumberData]}.
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::Number);
    appendDoubleToBuffer(content, jsNumber->getPrimitiveNumber());
  } else if (auto *jsBigInt = dyn_vmcast<JSBigInt>(*value)) {
    // 9. Otherwise, if value has a [[BigIntData]] internal slot, then set
    //    serialized to {[[Type]]: "BigInt", [[BigIntData]]:
    //    value.[[BigIntData]]}
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::BigInt);
    auto *bigIntPrim = JSBigInt::getPrimitiveBigInt(jsBigInt, runtime);
    serializeBigInt(serialized, bigIntPrim);
  } else if (auto *jsString = dyn_vmcast<JSString>(*value)) {
    // 10. Otherwise, if value has a [[StringData]] internal slot, then set
    //     serialized to {[[Type]]: "String", [[StringData]]:
    //     value.[[StringData]]}.
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::String);
    auto stringPrim = JSString::getPrimitiveString(jsString, runtime);
    serializeString(runtime, serialized, stringPrim, memoryMap);
  } else if (auto *jsDate = dyn_vmcast<JSDate>(*value)) {
    // 11. Otherwise, if value has a [[DateValue]] internal slot, then set
    // serialized to {[[Type]]: "Date", [[DateValue]]: value.[[DateValue]]}
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::Date);
    appendDoubleToBuffer(content, jsDate->getPrimitiveValue());
  } else if (auto *regExp = dyn_vmcast<JSRegExp>(*value)) {
    // 12. Otherwise, if value has a [[RegExpMatcher]] internal slot, then set
    // serialized to {[[Type]]: "RegExp", [[RegExpMatcher]]:
    // value.[[RegExpMatcher]], [[OriginalSource]]: value.[[OriginalSource]],
    // [[OriginalFlags]]: value.[[OriginalFlags]]}.
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::RegExp);
    serializeRegExp(runtime, serialized, regExp, memoryMap);
  } else if (auto *arrayBuffer = dyn_vmcast<JSArrayBuffer>(*value)) {
    // 13. Otherwise, if value has an [[ArrayBufferData]] internal slot, then:
    //    1. SharedArrayBuffer case is unsupported
    //    2. Otherwise:
    // Note for 13.2, we do not support Resizeable ArrayBuffers so we can
    // serialize it as ArrayBuffer directly
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::ArrayBuffer);
    auto res = serializeArrayBuffer(runtime, serialized, arrayBuffer);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    };
  } else if (auto dataView = Handle<JSDataView>::dyn_vmcast(value)) {
    // 14. Otherwise, if value has a [[ViewedArrayBuffer]] internal slot, then:
    //    1-4: Handled in helper below
    //    5. If value has a [[DataView]] internal slot
    // We handle the DataView case separately here. The TypedArray (14.6) case
    // in the next conditional block
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::DataView);
    auto res = serializeDataView(runtime, serialized, dataView, memoryMap);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    };
  } else if (auto typedArray = Handle<JSTypedArrayBase>::dyn_vmcast(value)) {
#define TYPED_ARRAY(name, type)                                         \
  if (vmisa<JSTypedArray<type, CellKind::name##ArrayKind>>(*value)) {   \
    content[typeTagOffset] =                                            \
        getUInt8FromSerializedType(SerializedValue::Type::name##Array); \
  }
#include "hermes/VM/TypedArrays.def"
    // 14.6 Otherwise:
    //    1. Assert: value has a [[TypedArrayName]] internal slot
    //    2. Handled in the serializeTypedArray helper below.
    auto res = serializeTypedArray(runtime, serialized, typedArray, memoryMap);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    };
  } else if (vmisa<JSMap>(*value)) {
    // 15. Otherwise, if value has a [[MapData]] internal slot, then:
    //    1. Set serialized to {[[Type]]: "Map", [[MapData]]: a new empty
    //    List}.
    //    2. Set deep to true.
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::Map);
    deep = true;
  } else if (vmisa<JSSet>(*value)) {
    // 16. Otherwise, if value has a [[MapData]] internal slot, then:
    //    1. Set serialized to {[[Type]]: "Map", [[MapData]]: a new empty
    //    List}.
    //    2. Set deep to true.
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::Set);
    deep = true;
  } else if (auto err = Handle<JSError>::dyn_vmcast(value)) {
    // 17. Otherwise, if value has an [[ErrorData]] internal slot and value is
    // not a platform object, then:
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::Error);
    // 1-5: Handled in the helper below.
    auto res = serializeJsError(runtime, serialized, err, memoryMap);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  } else if (auto arr = Handle<JSArray>::dyn_vmcast(value)) {
    // 18. Otherwise, if value is an Array exotic object, then:
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::Array);
    // 1-3: Handled in the helper below.
    auto res = serializeArray(runtime, serialized, arr);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // 4. Set deep to true.
    deep = true;
  } else if (vmisa<Callable>(*value)) {
    // 19, 20: Platform object case: Not supported
    // 21. Otherwise, if IsCallable(value) is true, then throw a
    // "DataCloneError" DOMException.
    return runtime.raiseError("Functions are not serializable");
  } else if (vmisa<Arguments>(*value) || vmisa<JSProxy>(*value)) {
    // 22. Otherwise, if value has any internal slot other than [[Prototype]],
    // [[Extensible]], or [[PrivateElements]], then throw a "DataCloneError"
    // DOMException.
    // 23. Otherwise, if value is an exotic object and value is not the
    // %Object.prototype% intrinsic object associated with any realm, then throw
    // a "DataCloneError" DOMException.
    return runtime.raiseError("Unserializable object type.");
  } else {
    // 24. Otherwise:
    //   1. Set serialized to {[[Type]]: "Object", [[Properties]]: a new empty
    //   List}.
    //   2. Set deep to true.
    content[typeTagOffset] =
        getUInt8FromSerializedType(SerializedValue::Type::Object);
    deep = true;
  }

  // 25. Set memory[value] to serialized.
  auto valuePtr = &runtime.serializationValues_.add(*value, hash);
  memoryMap[valuePtr] = id;

  // 26. If deep is true, then
  if (deep) {
    if (vmisa<JSMap>(*value)) {
      // 1. If value has a [[MapData]] internal slot, then:
      // Substeps are handled in the helper below.
      auto res = serializeMap(
          runtime, serialized, Handle<JSMap>::vmcast(value), memoryMap);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else if (vmisa<JSSet>(*value)) {
      // 2. Otherwise, if value has a [[SetData]] internal slot, then:
      // Substeps are handled in the helper below.
      auto res = serializeSet(
          runtime, serialized, Handle<JSSet>::vmcast(value), memoryMap);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else if (auto arr = Handle<JSArray>::dyn_vmcast(value)) {
      // The algorithm handles Array and Object in the same steps, but we can
      // serialize Array a little differently to take advantage of its indexed
      // storage.
      // 4. Otherwise,
      // Substeps are handled in the helper below.
      auto res = serializeArrayProperties(runtime, serialized, arr, memoryMap);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      // Objects case
      // 4. Otherwise,
      // Substeps are handled in the helper below.
      auto res = serializeObjectProperties(
          runtime, serialized, selfObjHandle, memoryMap);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }

  return ExecutionStatus::RETURNED;
}

/// Deserialize the JS Value record pointed to at \p curr of the content
/// buffer in \p serialized and returns the HermesValue. The supplied \p
/// memoryMap is used to map each object ID from the offset array to its
/// deserialized value. This method also updates the \p curr to point past the
/// Record deserialized.
/// Transferred ArrayBuffers are handled by this helper and needs to be handled
/// prior to this call.
CallResult<HermesValue> deserializeImpl(
    Runtime &runtime,
    const SerializedValue &serialized,
    const uint8_t *&curr,
    DeserializationValueDenseMap &memoryMap) {
  // The first byte should indicate the type of value we're deserializing
  auto typeTag = deserializeSerializedType(curr);
  // 1. If memory was not supplied, let memory be an empty map.
  // 2. If memory[serialized] exists, then return memory[serialized].
  // Check for the "Reference" Record type and find the Value in the cache map.
  // Reference Records are created after the original Record was created.
  // Since we deserialize in the same order we serialize Records, we can
  // guarantee an entry exists.
  if (typeTag == SerializedValue::Type::Reference) {
    auto id = deserializeUInt32(curr);
    if (auto it = memoryMap.find(id); it != memoryMap.end()) {
      auto managedValue = it->second;
      return managedValue->value();
    }
    llvm_unreachable("Cannot find matching value for the Reference Record.");
  }

  // 3. Let deep be false.
  bool deep = false;
  // 4. Let value be an uninitialized value.
  // 5. If serialized.[[Type]] is "primitive", then set value to
  // serialized.[[Value]]
  if (typeTag == SerializedValue::Type::Undefined) {
    return HermesValue::encodeUndefinedValue();
  }
  if (typeTag == SerializedValue::Type::Null) {
    return HermesValue::encodeNullValue();
  }
  if (typeTag == SerializedValue::Type::PrimitiveBoolean) {
    bool val = *curr++;
    return HermesValue::encodeBoolValue(val);
  }
  if (typeTag == SerializedValue::Type::PrimitiveNumber) {
    // The next 8 bytes should represent the double
    double val = deserializeDouble(curr);
    return HermesValue::encodeTrustedNumberValue(val);
  }
  if (typeTag == SerializedValue::Type::PrimitiveBigInt) {
    auto bigIntRes = deserializeBigInt(runtime, curr);
    if (LLVM_UNLIKELY(bigIntRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return bigIntRes->getHermesValue();
  }
  if (typeTag == SerializedValue::Type::PrimitiveString) {
    auto strResult = deserializeString(runtime, serialized, curr, memoryMap);
    if (LLVM_UNLIKELY(strResult == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return strResult->getHermesValue();
  }

  // All Primitive types should be handled above. From this point on, we are
  // deserializing Objects.
  assert(
      typeTag >= SerializedValue::Type::LastPrimitive &&
      "Unhandled primitive type");

  struct : Locals {
    PinnedValue<> self;
    PinnedValue<> tmp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  uint32_t id = deserializeUInt32(curr);

  if (typeTag == SerializedValue::Type::Boolean) {
    // 6. Otherwise, if serialized.[[Type]] is "Boolean", then set value to a
    // new Boolean object in targetRealm whose [[BooleanData]] internal slot
    // value is serialized.[[BooleanData]]
    bool val = *curr++;
    auto jsBool = JSBoolean::create(runtime, val, runtime.booleanPrototype);
    lv.self = jsBool.getHermesValue();
  } else if (typeTag == SerializedValue::Type::Number) {
    // 7. Otherwise, if serialized.[[Type]] is "Number", then set value to a new
    // Number object in targetRealm whose [[NumberData]] internal slot value is
    // serialized.[[NumberData]].
    double val = deserializeDouble(curr);
    auto jsNum = JSNumber::create(runtime, val, runtime.numberPrototype);
    lv.self = jsNum.getHermesValue();
  } else if (typeTag == SerializedValue::Type::BigInt) {
    // 8. Otherwise, if serialized.[[Type]] is "BigInt", then set value to a new
    // BigInt object in targetRealm whose [[BigIntData]] internal slot value is
    // serialized.[[BigIntData]].
    auto bigIntRes = deserializeBigInt(runtime, curr);
    if (LLVM_UNLIKELY(bigIntRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.tmp = std::move(*bigIntRes);
    auto jsBigInt = JSBigInt::create(
        runtime,
        Handle<BigIntPrimitive>::vmcast(&lv.tmp),
        runtime.bigintPrototype);
    lv.self = jsBigInt.getHermesValue();
  } else if (typeTag == SerializedValue::Type::String) {
    // 9. Otherwise, if serialized.[[Type]] is "String", then set value to a new
    // String object in targetRealm whose [[StringData]] internal slot value is
    // serialized.[[StringData]].
    auto result = deserializeString(runtime, serialized, curr, memoryMap);
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.tmp = std::move(*result);
    auto jsString = JSString::create(
        runtime,
        Handle<StringPrimitive>::vmcast(&lv.tmp),
        runtime.stringPrototype);
    lv.self = jsString->getHermesValue();
  } else if (typeTag == SerializedValue::Type::Date) {
    // 10. Otherwise, if serialized.[[Type]] is "Date", then set value to a new
    // Date object in targetRealm whose [[DateValue]] internal slot value is
    // serialized.[[DateValue]].
    double val = deserializeDouble(curr);
    lv.self = JSDate::create(runtime, val, runtime.datePrototype);
  } else if (typeTag == SerializedValue::Type::RegExp) {
    // 11. Otherwise, if serialized.[[Type]] is "RegExp", then set value to a
    // new RegExp object in targetRealm whose [[RegExpMatcher]] internal slot
    // value is serialized.[[RegExpMatcher]], whose [[OriginalSource]] internal
    // slot value is serialized.[[OriginalSource]], and whose [[OriginalFlags]]
    // internal slot value is serialized.[[OriginalFlags]].
    auto res = deserializeRegExp(runtime, serialized, curr, memoryMap);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.self = res->getHermesValue();
  } else if (typeTag == SerializedValue::Type::ArrayBuffer) {
    // 12-13. SharedArrayBuffer case: Unsupported
    // 14.Otherwise, if serialized.[[Type]] is "ArrayBuffer", then set value to
    // a new ArrayBuffer object in targetRealm whose [[ArrayBufferData]]
    // internal slot value is serialized.[[ArrayBufferData]], and whose
    // [[ArrayBufferByteLength]] internal slot value is
    // serialized.[[ArrayBufferByteLength]]. If this throws an exception, catch
    // it, and then throw a "DataCloneError" DOMException.
    auto res = deserializeArrayBuffer(runtime, serialized, curr);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.self = res->getHermesValue();
  } else if (typeTag == SerializedValue::Type::DataView) {
    // 15: Resizable ArrayBuffer case: Unsupported
    // 16. Otherwise, if serialized.[[Type]] is "ArrayBufferView", then:
    //   1-2: Handled in the helper below.
    auto res = deserializeDataView(runtime, serialized, curr, memoryMap);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.self = res->getHermesValue();
  } else if (
      typeTag >= SerializedValue::Type::TypedArray_first &&
      typeTag <= SerializedValue::Type::TypedArray_last) {
    // 16.3: Typed Array case. Handled in helper below
    auto res =
        deserializeTypedArray(runtime, typeTag, serialized, curr, memoryMap);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.self = res->getHermesValue();
  } else if (typeTag == SerializedValue::Type::Map) {
    // 17.Otherwise, if serialized.[[Type]] is "Map", then:
    //   1. Set value to a new Map object in targetRealm whose [[MapData]]
    //   internal slot value is a new empty List.
    //   2. Set deep to true.
    lv.self = JSMap::create(runtime, runtime.mapPrototype);
    if (LLVM_UNLIKELY(
            JSMap::initializeStorage(
                Handle<JSMap>::vmcast(&lv.self), runtime) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    deep = true;
  } else if (typeTag == SerializedValue::Type::Set) {
    // 18.Otherwise, if serialized.[[Type]] is "Set", then:
    //   1. Set value to a new Set object in targetRealm whose [[SetData]]
    //   internal slot value is a new empty List.
    //   2. Set deep to true.
    lv.self = JSSet::create(runtime, runtime.setPrototype);
    if (LLVM_UNLIKELY(
            JSSet::initializeStorage(
                Handle<JSSet>::vmcast(&lv.self), runtime) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    deep = true;
  } else if (typeTag == SerializedValue::Type::Array) {
    // 19: Otherwise, if serialized.[[Type]] is "Array", then:
    uint32_t len = deserializeUInt32(curr);
    auto arrayRes = JSArray::create(runtime, len, len);
    if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.self = std::move(*arrayRes);
    deep = true;
  } else if (typeTag == SerializedValue::Type::Object) {
    // 20. Otherwise, if serialized.[[Type]] is "Object", then:
    //   1. Set value to a new Object in targetRealm.
    lv.self = JSObject::create(runtime);
    //   2. Set deep to true.
    deep = true;
  } else if (typeTag == SerializedValue::Type::Error) {
    // 21. Otherwise, if serialized.[[Type]] is "Error", then:
    auto res = deserializeJsError(runtime, serialized, curr, memoryMap);
    lv.self = res->getHermesValue();
  }
  // 22. Platform object case: Not supported

  // 23. Set memory[serialized] to value.
  // Store this value in the ManagedChunkList. This way, GC will be aware of the
  // value while we finish deserializing, which could reference this value
  // again. Also, map the object id to the ManagedValue for potential reuse
  // later.
  memoryMap[id] = &runtime.serializationValues_.add(
      *lv.self, getUInt32Hash(runtime.gcStableHashHermesValue(*lv.self)));

  // 24. If deep is true, then:
  if (deep) {
    if (typeTag == SerializedValue::Type::Map) {
      // 1. If serialized.[[Type]] is "Map", then:
      // Substeps handled in the helper
      auto res = deserializeMapEntries(
          runtime,
          serialized,
          curr,
          Handle<JSMap>::vmcast(&lv.self),
          memoryMap);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else if (typeTag == SerializedValue::Type::Set) {
      // 2. Otherwise, if serialized.[[Type]] is "Set", then:
      // Substeps handled in the helper.
      auto res = deserializeSetElements(
          runtime,
          serialized,
          curr,
          Handle<JSSet>::vmcast(&lv.self),
          memoryMap);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else if (typeTag == SerializedValue::Type::Array) {
      // 3. Otherwise, if serialized.[[Type]] is "Array" or "Object", then:
      // We handle Array separately to take indexed properties into account
      auto res = deserializeArrayProperties(
          runtime,
          serialized,
          curr,
          Handle<JSArray>::vmcast(&lv.self),
          memoryMap);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      assert(
          typeTag == SerializedValue::Type::Object && "Expected Object record");
      // 3. Otherwise, if serialized.[[Type]] is "Array" or "Object", then:
      // Substeps handled in the helper.
      auto res = deserializeProperties(
          runtime,
          serialized,
          curr,
          Handle<JSObject>::vmcast(&lv.self),
          memoryMap);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }

  return *lv.self;
}

/// For each element in \p transferList, check its type is transferable. Then,
/// add a corresponding entry in \p serialized so it can be referenced during
/// serialization of other JS Values.
/// Returns an ArrayStorage containing everything in \p transferList. This
/// copied list is not in the spec. However, it is necessary to protect against
/// the transfer list changing during serialization, which can run arbitrary JS.
/// Implements Step 2 of StructuredSerializeWithTransfer
CallResult<PseudoHandle<ArrayStorage>> prepareTransferListForSerialization(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<JSArray> transferList,
    SerializationValueDenseMap &memoryMap) {
  auto transferListLen = JSArray::getLength(*transferList, runtime);
  auto arrStorageRes =
      ArrayStorage::create(runtime, transferListLen, transferListLen);
  if (LLVM_UNLIKELY(arrStorageRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  NoAllocScope noAlloc(runtime);
  ArrayStorage *copy = vmcast<ArrayStorage>(*arrStorageRes);

  // 2. For each transferable of transferList:
  for (JSArray::size_type i = 0, e = transferListLen; i < e; i++) {
    auto transferableHv = transferList->at(runtime, i).unboxToHV(runtime);
    auto *transferableArrayBuffer = dyn_vmcast<JSArrayBuffer>(transferableHv);
    // 1.If transferable has neither an [[ArrayBufferData]] internal slot nor a
    //   [[Detached]] internal slot, then throw a "DataCloneError" DOMException.
    // 2. SharedArrayBuffer case: Unsupported
    if (!transferableArrayBuffer) {
      // Release the NoAllocScope to create the exception.
      noAlloc.release();
      return runtime.raiseError(
          "Transfer list contains non-transferable value");
    }
    // 3. If memory[transferable] exists, then throw a "DataCloneError"
    //    DOMException.
    auto hash = runtime.gcStableHashJSObject(transferableArrayBuffer);
    HermesValueInfo hvInfo{transferableHv, hash};
    if (auto it = memoryMap.find_as(hvInfo); it != memoryMap.end()) {
      // Release the NoAllocScope to create the exception.
      noAlloc.release();
      return runtime.raiseError("Transfer list contains same value twice");
    }
    // 4. Set memory[transferable] to { [[Type]]: an uninitialized value }.
    // We add an entry to the offsets buffer and add it to the map so reference
    // records can be created during serialization if this value is referenced.
    // Actual serialization of items in the transfer list comes later.
    auto *valuePtr = &runtime.serializationValues_.add(transferableHv, hash);
    uint32_t id = serialized.offsets.size();
    memoryMap[valuePtr] = id;
    // Write a dummy location until the actual serialization of the item comes.
    serialized.offsets.push_back(0);

    // Add the transferable value to the internal copy of the transferList. We
    // already allocated the size of the ArrayStorage beforehand, so we can set
    // it directly.
    copy->set(i, transferableHv, runtime.getHeap());
  }
  return createPseudoHandle(copy);
}

/// For each ArrayBuffer in \p transferList, detach and serialize the
/// ArrayBuffer at the end of \p serialized.content and update the corresponding
/// offset in the \p serialized.offsets.
/// Implements Step 4, 5 of StructuredSerializeWithTransfer
ExecutionStatus serializeTransferList(
    Runtime &runtime,
    SerializedValue &serialized,
    Handle<ArrayStorage> transferList,
    SerializationValueDenseMap &memoryMap) {
  // 4. Let transferDataHolders be a new empty List.
  // 5. For each transferable of transferList:
  struct : Locals {
    PinnedValue<JSArrayBuffer> arrayBuffer;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  for (size_t i = 0, e = transferList->size(); i < e; ++i) {
    auto transferableHV = transferList->at(i);
    // 1. If transferable has an [[ArrayBufferData]] internal slot and
    // IsDetachedBuffer(transferable) is true, then throw a "DataCloneError"
    // DOMException.
    // 2. Platform object case: Unsupported
    lv.arrayBuffer = vmcast<JSArrayBuffer>(transferableHV);
    if (!lv.arrayBuffer->attached()) {
      return runtime.raiseError("Detached ArrayBuffers are not serializable");
    }
    // 3. Let dataHolder be memory[transferable].
    auto hash = runtime.gcStableHashJSObject(*lv.arrayBuffer);
    HermesValueInfo hvInfo{transferableHV, hash};
    auto it = memoryMap.find_as(hvInfo);

    // In Step 2.4 of StructuredSerializeWithTransfer, which is performed before
    // this function is called, we added it to the memoryMap, so it must be
    // present.
    assert(
        it != memoryMap.end() && "Transferred object not found in memory map");
    uint32_t id = it->second;
    uint32_t beginOffset = serialized.content.size();

    // Now we know where the transferred object will be serialized, write the
    // offset to the offset buffer
    serialized.offsets[id] = beginOffset;

    // 4. If transferable has an [[ArrayBufferData]] internal slot, then:
    //  1. Resizeable ArrayBuffer case: Unsupported
    //  2. Otherwise:
    //    1. Set dataHolder.[[Type]] to "ArrayBuffer".
    auto type = lv.arrayBuffer->external()
        ? SerializedValue::Type::ArrayBufferExternal
        : SerializedValue::Type::ArrayBufferInternal;
    serialized.content.push_back(getUInt8FromSerializedType(type));
    appendValueToBuffer<uint32_t>(serialized.content, id);
    //    2-3: Handled in the helper.
    serializeArrayBufferForTransfer(runtime, serialized, lv.arrayBuffer);
    //  3. Perform ? DetachArrayBuffer(transferable).
    // Detach the ArrayBuffer but do not free the data so we can transfer
    // ownership
    JSArrayBuffer::ejectBufferUnsafe(runtime, lv.arrayBuffer);
    // 5. Platform object case: Unsupported
    // 6. Append dataHolder to transferDataHolders
    // Step 6 is essentially done when we serialize the necessary data in
    // serializeArrayBufferForTransfer
  }
  return ExecutionStatus::RETURNED;
}

/// Deserialize and transfer all JSArrayBuffers in the transferList of \p
/// serialized. Returns a list of IDs of the objects that are successfully
/// transferred.
/// Implements Step 2, 3 of StructuredDeserializeWithTransfer
std::vector<uint32_t> deserializeTransferList(
    Runtime &runtime,
    SerializedValue &serialized,
    DeserializationValueDenseMap &memoryMap) {
  // 2. Let transferredValues be a new empty List.
  std::vector<uint32_t> transferredIds;
  // 3. For each transferDataHolder of
  // serializeWithTransferResult.[[TransferDataHolders]]:
  // Transferred values are assigned serialization id first, so they must start
  // at 0.
  uint32_t numTransfers =
      serialized.internalBuffers.size() + serialized.externalBuffers.size();
  GCScopeMarkerRAII marker{runtime};
  for (uint32_t i = 0; i < numTransfers; ++i) {
    marker.flush();
    auto contentOffset = serialized.offsets[i];
    const uint8_t *curr = serialized.content.data() + contentOffset;
    // 1. Let value be an uninitialized value.
    auto type = deserializeSerializedType(curr);
    // 2: Handled in the helper.
    bool isInternalBuffer = type == SerializedValue::Type::ArrayBufferInternal;
    [[maybe_unused]] uint32_t deserializedId = deserializeUInt32(curr);
    assert(
        deserializedId == i &&
        "ID in serialized record does not match the record being deserialized.");
    auto res = deserializeArrayBufferForTransfer(
        runtime, serialized, curr, isInternalBuffer);

    // 3-4. Resizeable ArrayBuffer and Platform objects: Unsupported
    // 5. Set memory[transferDataHolder] to value.
    auto *valuePtr = &runtime.serializationValues_.add(
        res.getHermesValue(), runtime.gcStableHashJSObject(res.get()));
    memoryMap[i] = valuePtr;

    // 6. Append value to transferredValues.
    transferredIds.push_back(i);
  }
  return transferredIds;
}

void freeDataAfterSerialization(SerializationValueDenseMap &memoryMap) {
  for (auto [value, _] : memoryMap) {
    value->markAsFree();
  }
}
void freeDataAfterDeserialization(DeserializationValueDenseMap &memoryMap) {
  for (auto [_, value] : memoryMap) {
    value->markAsFree();
  }
}

void cleanUpAfterDeserializeWithTransfer(
    SerializedValue &serialized,
    DeserializationValueDenseMap &memoryMap) {
  // Free any held data so GC can clean it up
  freeDataAfterDeserialization(memoryMap);
  // Clear the content buffer so the SerializedValue is invalidated.
  serialized.content.clear();
}

/// Call \p onObject with the serialization ID of \p value and \p value itself,
/// if \p value is a JS object.
ExecutionStatus reportObject(
    uint32_t id,
    SerializationManagedValue *value,
    SerializedObjectCallback onObject) {
  if (!vmisa<JSObject>(value->value())) {
    return ExecutionStatus::RETURNED;
  }
  return onObject(id, Handle<JSObject>::vmcast(&value->value()));
}

/// Call \p onObject for every JS object serialized into \p memoryMap. Does
/// nothing if \p onObject is not set.
ExecutionStatus reportSerializedObjects(
    SerializationValueDenseMap &memoryMap,
    SerializedObjectCallback onObject) {
  if (!onObject) {
    return ExecutionStatus::RETURNED;
  }
  for (auto [value, id] : memoryMap) {
    if (LLVM_UNLIKELY(
            reportObject(id, value, onObject) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

/// Call \p onObject for every JS object deserialized into \p memoryMap. Does
/// nothing if \p onObject is not set.
ExecutionStatus reportDeserializedObjects(
    DeserializationValueDenseMap &memoryMap,
    SerializedObjectCallback onObject) {
  if (!onObject) {
    return ExecutionStatus::RETURNED;
  }
  for (auto [id, value] : memoryMap) {
    if (LLVM_UNLIKELY(
            reportObject(id, value, onObject) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

} // namespace

CallResult<SerializedValue> serialize_RJS(
    Runtime &runtime,
    Handle<> value,
    SerializedObjectCallback onObject) {
  SerializedValue serialized;
  SerializationValueDenseMap memoryMap;
  auto cleanUp = llvh::make_scope_exit(
      [&memoryMap]() { freeDataAfterSerialization(memoryMap); });

  auto serializedRes = serializeImpl(runtime, value, serialized, memoryMap);
  if (LLVM_UNLIKELY(serializedRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(
          reportSerializedObjects(memoryMap, onObject) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return serialized;
}

CallResult<HermesValue> deserialize(
    Runtime &runtime,
    const SerializedValue &serialized,
    SerializedObjectCallback onObject) {
  if (serialized.content.empty()) {
    return runtime.raiseError("Cannot deserialize empty serialization data");
  }
  DeserializationValueDenseMap memoryMap;
  auto cleanUp = llvh::make_scope_exit(
      [&memoryMap]() { freeDataAfterDeserialization(memoryMap); });
  const uint8_t *start = serialized.content.data();
  auto deserializedRes = deserializeImpl(runtime, serialized, start, memoryMap);
  if (LLVM_UNLIKELY(deserializedRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(
          reportDeserializedObjects(memoryMap, onObject) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return *deserializedRes;
}

CallResult<SerializedValue> serializeWithTransfer_RJS(
    Runtime &runtime,
    Handle<> value,
    Handle<JSArray> transferList,
    SerializedObjectCallback onObject) {
  // This will hold the copy of the `transferList`. This is because the
  // serialization process can run arbitrary JS, potentially modifying the
  // original JSArray `transferList`. However, StructuredSerializeWithTransfer
  // assumes that the list of transfer values remains the same throughout the
  // whole process.
  struct : Locals {
    PinnedValue<ArrayStorage> transfersCopy;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  SerializedValue serialized;
  // 1. Let memory be an empty map.
  SerializationValueDenseMap memoryMap;
  auto cleanUp = llvh::make_scope_exit(
      [&memoryMap]() { freeDataAfterSerialization(memoryMap); });

  // 2. Handled in the helper.
  auto prepTransferListRes = prepareTransferListForSerialization(
      runtime, serialized, transferList, memoryMap);
  if (LLVM_UNLIKELY(prepTransferListRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.transfersCopy = std::move(*prepTransferListRes);

  // 3. Let serialized be ? StructuredSerializeInternal(value, false, memory).
  auto serializedRes = serializeImpl(runtime, value, serialized, memoryMap);
  if (LLVM_UNLIKELY(serializedRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (LLVM_UNLIKELY(
          reportSerializedObjects(memoryMap, onObject) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 4-5: Handled in the helper.
  auto serializeTransferListRes =
      serializeTransferList(runtime, serialized, lv.transfersCopy, memoryMap);
  if (LLVM_UNLIKELY(serializeTransferListRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 6. Return { [[Serialized]]: serialized, [[TransferDataHolders]]:
  // transferDataHolders }.
  return serialized;
}

CallResult<PseudoHandle<JSArray>> deserializeWithTransfer(
    Runtime &runtime,
    SerializedValue &serialized,
    SerializedObjectCallback onObject) {
  if (serialized.content.empty()) {
    return runtime.raiseError("Cannot deserialize empty serialization data");
  }
  // 1. Let memory be an empty map.
  DeserializationValueDenseMap memoryMap;
  auto cleanUp = llvh::make_scope_exit([&memoryMap, &serialized]() {
    cleanUpAfterDeserializeWithTransfer(serialized, memoryMap);
  });
  // 2-3: Handled in the helper.
  std::vector<uint32_t> transferredIds =
      deserializeTransferList(runtime, serialized, memoryMap);

  // 4. Let deserialized be ?
  // StructuredDeserialize(serializeWithTransferResult.[[Serialized]],
  // targetRealm, memory).
  // Deserialize the main serialized value, which is located at the front of
  // serialized.content

  // 5. Return { [[Deserialized]]: deserialized, [[TransferredValues]]:
  // transferredValues }.
  struct : Locals {
    PinnedValue<> deserialized;
    PinnedValue<JSArray> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  const uint8_t *start = serialized.content.data();
  auto valRes = deserializeImpl(runtime, serialized, start, memoryMap);
  if (LLVM_UNLIKELY(valRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.deserialized = *valRes;

  if (LLVM_UNLIKELY(
          reportDeserializedObjects(memoryMap, onObject) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto transferLen = transferredIds.size();
  auto arrayLen = transferLen + 1;
  auto ret = JSArray::create(runtime, arrayLen, arrayLen);
  if (LLVM_UNLIKELY(ret == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  lv.result = std::move(*ret);
  JSArray::setStorageEndIndex(lv.result, runtime, arrayLen);

  // Set the deserialized value as the first element.
  // This call happens outside of NoAllocScope because
  // SmallHermesValue::encodeHermesValue may allocate if valRes is not a pointer
  // type.
  JSArray::unsafeSetExistingElementAt(
      *lv.result,
      runtime,
      0,
      SmallHermesValue::encodeHermesValue(*lv.deserialized, runtime));

  // At this point, all objects are deserialized and the result Array is
  // allocated. No other allocation needs to happen.
  NoAllocScope noAlloc(runtime);

  // Add all transferred values to the result.
  for (size_t i = 0; i < transferLen; i++) {
    auto *valuePtr = memoryMap.lookup(transferredIds[i]);
    // All transferred values are deserialized, so it must have been added to
    // the memory map.
    assert(
        valuePtr != nullptr &&
        "Transferred value not found in deserialized memory map");

    auto *arrayBuffer = vmcast<JSArrayBuffer>(valuePtr->value());
    auto shv = SmallHermesValue::encodeObjectValue(arrayBuffer, runtime);

    JSArray::unsafeSetExistingElementAt(*lv.result, runtime, i + 1, shv);
  }

  return createPseudoHandle(*lv.result);
}

SerializedValue::~SerializedValue() {
  // At the end of this SerializedValue's lifetime, if it still holds any
  // ownership over ArrayBuffer internal data blocks, then we need to free the
  // storage. This can happen if the user never deserialized this object. Note:
  // No extra work is needed for external data blocks because they are managed
  // by shared pointers and released when the object is destroyed.
  for (auto *internalBuf : internalBuffers) {
    if (internalBuf) {
      free(internalBuf);
    }
  }
}

} // namespace vm
} // namespace hermes
