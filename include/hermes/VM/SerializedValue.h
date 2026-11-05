/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_SERIALIZEDVALUE_H
#define HERMES_VM_SERIALIZEDVALUE_H

#include "hermes/VM/NativeState.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Runtime.h"

#include "llvh/ADT/STLExtras.h"

namespace hermes {
namespace vm {

/// Optional last argument of the functions below. After the whole value is
/// (de)serialized, it is called for every JS object in it, including the
/// transferred ones, with the object's serialization ID. An object gets the
/// same ID in serialization and deserialization.
using SerializedObjectCallback =
    llvh::function_ref<ExecutionStatus(uint32_t id, Handle<JSObject> obj)>;

/// Produced by serialized or serializedWithTransfer, this encapsulates all
/// serialized data of JS Value and, if applicable, any transferred JS Values.
/// This object should only be used to deserialize JS values into a runtime via
/// deserialize or deserializeWithTransfer.
class SerializedValue {
 public:
  /// Each element stored in this vector refers to the offset in the buffer
  /// vectors. For example, a string with a serialization ID X may be encoded at
  /// strings[stringOffset]. Then, the element at index X of this vector will
  /// store stringOffset.
  std::vector<uint32_t> offsets;

  /// Contains the Record for the serialized JS value, as well as a nested
  /// Record for all nested JS values. In general, each value is serialized in
  /// the form (type tag, content). The type tag indicates the type of JS value
  /// the record represents, and the content will encode all information needed
  /// to reconstruct the value. Note that the content may encode other JS values
  /// directly, or contain Reference records to an existing serialized value.
  std::vector<uint8_t> content;

  /// Contains all strings needed for deserialization. Each string is encoded
  /// with the format (string serialization ID, isAscii, length, data)
  std::vector<uint8_t> strings;

  /// For JS ArrayBuffers with internal data blocks, store the pointer to the
  /// underlying data block that needs to be transferred.
  std::vector<uint8_t *> internalBuffers;

  /// For JS ArrayBuffers with external data blocks, store a pair contain the
  /// data block pointer, and the context responsible for cleaning up the data
  /// block.
  std::vector<std::pair<uint8_t *, std::shared_ptr<void>>> externalBuffers;

  /// Describes the type of JS value for some serialized content. The special
  /// Reference type is used to point at a JS value serialized at some other
  /// location.
  enum class Type : uint8_t {
    Undefined,
    Null,
    PrimitiveBoolean,
    PrimitiveNumber,
    PrimitiveBigInt,
    PrimitiveString,
    Boolean,
    Number,
    BigInt,
    String,
    Date,
    RegExp,
    ArrayBuffer,
    ArrayBufferInternal,
    ArrayBufferExternal,
    DataView,
#define TYPED_ARRAY(name, string) name##Array,
#include "hermes/VM/TypedArrays.def"
    Map,
    Set,
    Error,
    Array,
    Object,
    Reference,
    LastPrimitive = PrimitiveString,
    TypedArray_first = DataView + 1,
    TypedArray_last = Map - 1,
  };

  enum class ErrorType : uint8_t {
    Error,
    EvalError,
    RangeError,
    ReferenceError,
    SyntaxError,
    TypeError,
    URIError
  };

  SerializedValue() {}

  SerializedValue(SerializedValue &&) = default;
  SerializedValue &operator=(SerializedValue &&) = default;

  /// A SerializedValue object must not be copied since it may hold ownership
  /// over data buffers
  SerializedValue(const SerializedValue &) = delete;
  void operator=(const SerializedValue &) = delete;
  ~SerializedValue();
};

/// Serializes a HermesValue \p value. Follows StructuredSerializeInternal
/// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
CallResult<SerializedValue> serialize_RJS(
    Runtime &runtime,
    Handle<> value,
    SerializedObjectCallback onObject = {});

/// Deserializes the SerializedValue \p serialized following the structure clone
/// algorithm. The SerializedValue \p serialized must be created by
/// serialize_RJS only. Follows StructuredDeserialize.
/// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
CallResult<HermesValue> deserialize(
    Runtime &runtime,
    const SerializedValue &serialized,
    SerializedObjectCallback onObject = {});

/// Serializes a HermesValue \p value. Given the length property of \p
/// transferList, it will transfer everything at index [0, length - 1] out of
/// the current runtime.
///  Follows StructuredSerializeWithTransfer.
/// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializewithtransfer
CallResult<SerializedValue> serializeWithTransfer_RJS(
    Runtime &runtime,
    Handle<> value,
    Handle<JSArray> transferList,
    SerializedObjectCallback onObject = {});

/// Deserializes the SerializedValue \p serialized following the structure clone
/// algorithm. The SerializedValue \p serialized must be created by
/// serializeWithTransfer_RJS. This performs any transfer as necessary, and
/// consumes the serialized object so it cannot be deserialized again.
/// Returns a JS Array where the first element is the deserialized value,
/// followed by all transferred values.
/// Follows StructuredDeserializeWithTransfer.
/// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializewithtransfer
CallResult<PseudoHandle<JSArray>> deserializeWithTransfer(
    Runtime &runtime,
    SerializedValue &serialized,
    SerializedObjectCallback onObject = {});

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_SERIALIZEDVALUE_H
