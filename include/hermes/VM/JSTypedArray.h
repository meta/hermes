/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSTYPEDARRAY_H
#define HERMES_VM_JSTYPEDARRAY_H

#include "hermes/Support/Conversions.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/JSArrayBuffer.h"

namespace hermes {
namespace vm {

/// JSTypedArrayBase is the object that all JSTypedArrays inherit from, and
/// exposes common interface elements to JSTypedArrays.
class JSTypedArrayBase : public JSObject {
 public:
  // The spec is silent about the maximum size of an ArrayBuffer.  If
  // this is to be enlarged effectively, other changes (such as the
  // use of uint32_t in ObjectVTable's indexed storage members, will
  // need to be changed, too.
  using size_type = uint32_t;
  using Super = JSObject;

  CallResult<Handle<JSTypedArrayBase>> allocate(
      Runtime &runtime,
      size_type length = 0);

  /// Allocate a new instance of a TypedArray matching the runtime type of
  /// the \p src TypedArray.
  /// The newly allocated TypedArray shares the same underlying ArrayBuffer as
  /// \p src.
  /// \p beginIndex the first index of \p src to use in the new array.
  /// \p endIndex the last index of \p src to use in the new array.
  static CallResult<Handle<JSTypedArrayBase>> allocateToSameBuffer(
      Runtime &runtime,
      Handle<JSTypedArrayBase> src,
      size_type beginIndex,
      size_type endIndex);

  static CallResult<Handle<JSTypedArrayBase>> allocateSpecies(
      Runtime &runtime,
      Handle<JSTypedArrayBase> self,
      size_type length);

  /// ES6 22.2.3.5.1
  /// Validates \p thisArg to be a JSTypedArrayBase.
  /// If \p checkAttached is true, it will also ensure that the typed array is
  /// attached.
  static ExecutionStatus validateTypedArray(
      Runtime &runtime,
      Handle<> thisArg,
      bool checkAttached = true);
  static bool classof(const GCCell *cell) {
    return kindInRange(
        cell->getKind(),
        CellKind::TypedArrayBaseKind_first,
        CellKind::TypedArrayBaseKind_last);
  }

  /// Get the length of this array. This corresponds to the property visible
  /// as `.length` in JS.
  size_type getLength() const {
    return length_;
  }

  /// Get the byte length of this array. This corresponds to the property
  /// visible as `.byteLength` in JS.
  JSArrayBuffer::size_type getByteLength() const {
    return length_ * getByteWidth();
  }

  /// Get the width of the type in number of bytes. This is sizeof(T) for the
  /// T used to instantiate a TypedArray.
  uint8_t getByteWidth() const;

  /// \return The underlying ArrayBuffer that this TypedArray uses for its
  /// storage, or null if this TypedArray doesn't have a corresponding
  /// ArrayBuffer. May be called even if the underlying buffer is detached.
  JSArrayBuffer *getBuffer(Runtime &runtime) const {
    return buffer_.getNonNull(runtime);
  }

  uint8_t *data(PointerBase &base) {
    return buffer_.getNonNull(base)->getDataBlock() + offset_;
  }
  uint8_t *dataEnd(PointerBase &base) {
    return data(base) + getByteLength();
  }

  // Return true if the specified cell is a TypedArray that does not require
  // allocation for the read. In practice this includes the Bigint arrays.
  static bool isNoAllocTypedArray(GCCell *cell) {
    return kindInRange(
        cell->getKind(),
        CellKind::NoAllocTypedArrayKind_first,
        CellKind::NoAllocTypedArrayKind_last);
  }

  /// Polymorphic access to the data in the TypedArray.
  /// This function also handles the case where it is used in the interpreter
  /// loop, so optionally it saves and restores the instruction pointer.
  inline static HermesValue
  polyReadMayAlloc(JSTypedArrayBase *self, Runtime &runtime, size_type index);

  /// Polymorphic access to the data in the TypedArray but only for arrays
  /// that satisfy the isNoAllocTypedArray() condition. This function never
  /// allocates.
  inline static HermesValue
  polyReadNoAlloc(JSTypedArrayBase *self, Runtime &runtime, size_type index);

  /// Polymorphic write to the data in the TypedArray. This function never
  /// allocates.
  /// \pre self->attached() is true and index is within bound.
  static void polyWriteNoAlloc(
      JSTypedArrayBase *self,
      Runtime &runtime,
      size_type index,
      HermesValue value);

  /// \return Whether this JSTypedArrayBase is attached to some buffer.
  bool attached(PointerBase &base) const {
    assert(
        buffer_ && "buffer_ can only be null in partial initialization status");
    return buffer_.getNonNull(base)->attached();
  }

  /// \return The offset from the beginning of the buffer this typed array
  /// is looking into.
  JSArrayBuffer::size_type getByteOffset() const {
    return offset_;
  }

  /// Allocates a buffer using \p runtime with \p length number of
  /// elements, each of \p byteWidth size in bytes.
  static ExecutionStatus createBuffer(
      Runtime &runtime,
      Handle<JSTypedArrayBase> selfObj,
      uint64_t length);

  /// Sets the current buffer to a copy of \p src starting from
  /// \p byteOffset and going for \p srcSize bytes total.
  /// \pre
  ///   src must not be a null handle.
  ///   byteOffset + srcSize <= src->size()
  static ExecutionStatus setToCopyOfBuffer(
      Runtime &runtime,
      Handle<JSTypedArrayBase> dst,
      JSArrayBuffer::size_type dstByteOffset,
      Handle<JSArrayBuffer> src,
      JSArrayBuffer::size_type srcByteOffset,
      JSArrayBuffer::size_type count);

  /// Sets the current buffer to a copy of \p src.
  /// This is a convenient wrapper around `setToCopyOfBuffer` for when a
  /// whole TypedArray must be copied into the current one.
  /// \pre src must not be a null handle.
  static ExecutionStatus setToCopyOfTypedArray(
      Runtime &runtime,
      Handle<JSTypedArrayBase> dst,
      size_type dstIndex,
      Handle<JSTypedArrayBase> src,
      size_type srcIndex,
      size_type count);

  /// Sets this object to use \p buf as its buffer, starting from \p offset
  /// position and goes for \p size indices in the buffer, where each element
  /// is aligned on \p byteWidth.  The \p gc parameter is necessary for write
  /// barriers.
  /// \pre
  ///   Both \p offset and \p size must be aligned on byteWidth.
  ///   \p offset + size <= the size of \p buf.
  ///   Neither \p self nor \p buf can be null.
  static void setBuffer(
      Runtime &runtime,
      JSTypedArrayBase *self,
      JSArrayBuffer *buf,
      size_type offset,
      size_type size,
      uint8_t byteWidth);

 protected:
  /// buffer_ is the underlying buffer which holds the data to be viewed.
  /// This buffer may be shared with other JSTypedArray instantiations.
  GCPointer<JSArrayBuffer> buffer_;
  /// length_ is the length of the buffer in terms of the type it is viewed by.
  size_type length_;
  /// offset_ is the offset to start reading from in the underlying ArrayBuffer.
  size_type offset_;

  explicit JSTypedArrayBase(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz);

  /// Sets the current buffer's contents to the contents of a buffer from
  /// another TypedArray \p src.
  /// Copies from \p srcIndex to \p srcIndex + \p count in \p src into
  /// \p dstIndex to \p dstIndex + \p count in \p dst.
  /// \pre
  ///   Both dst and src must be the same runtime type of TypedArray.
  ///   For distinct TypedArrays, use `setToCopyOfTypedArray` instead.
  static void setToCopyOfBytes(
      Runtime &runtime,
      Handle<JSTypedArrayBase> dst,
      size_type dstIndex,
      Handle<JSTypedArrayBase> src,
      size_type srcIndex,
      size_type count);

  static std::pair<uint32_t, uint32_t> _getOwnIndexedRangeImpl(
      JSObject *selfObj,
      PointerBase &runtime);

  static bool
  _haveOwnIndexedImpl(JSObject *selfObj, Runtime &runtime, uint32_t index);
  static OptValue<PropertyFlags> _getOwnIndexedPropertyFlagsImpl(
      JSObject *selfObj,
      Runtime &runtime,
      uint32_t index);
  static bool _deleteOwnIndexedImpl(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      uint32_t index);
  /// Check whether all indexed properties satisfy the requirement specified by
  /// \p mode. Either whether they are all non-configurable, or whether they are
  /// all both non-configurable and non-writable.
  static bool _checkAllOwnIndexedImpl(
      JSObject *selfObj,
      Runtime &runtime,
      ObjectVTable::CheckAllOwnIndexedMode mode);

  friend void TypedArrayBaseBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);
  /// The x86-64 JIT emits an inline typed-array element store that reads the
  /// three fields above directly; RuntimeOffsets is where their offsets are
  /// derived with offsetof().
  friend struct RuntimeOffsets;
};

#ifdef NDEBUG
LLVM_ATTRIBUTE_ALWAYS_INLINE
#endif
inline HermesValue JSTypedArrayBase::polyReadMayAlloc(
    JSTypedArrayBase *self,
    Runtime &runtime,
    size_type index) {
  assert(self->attached(runtime) && "at() requires a JSArrayBuffer");
  assert(
      index < self->getLength() &&
      "That index is out of bounds of this TypedArray");

  uint8_t *dataPtr = self->data(runtime);
  switch (self->getKind()) {
    case CellKind::Int8ArrayKind:
      return HermesValue::encodeTrustedNumberValue(
          reinterpret_cast<int8_t *>(dataPtr)[index]);
    case CellKind::Uint8ArrayKind:
    case CellKind::Uint8ClampedArrayKind:
      return HermesValue::encodeTrustedNumberValue(
          reinterpret_cast<uint8_t *>(dataPtr)[index]);
    case CellKind::Int16ArrayKind:
      return HermesValue::encodeTrustedNumberValue(
          reinterpret_cast<int16_t *>(dataPtr)[index]);
    case CellKind::Uint16ArrayKind:
      return HermesValue::encodeTrustedNumberValue(
          reinterpret_cast<uint16_t *>(dataPtr)[index]);
    case CellKind::Int32ArrayKind:
      return HermesValue::encodeTrustedNumberValue(
          reinterpret_cast<int32_t *>(dataPtr)[index]);
    case CellKind::Uint32ArrayKind:
      return HermesValue::encodeTrustedNumberValue(
          reinterpret_cast<uint32_t *>(dataPtr)[index]);
    case CellKind::Float16ArrayKind:
      return HermesValue::encodeUntrustedNumberValue(
          float16ToDouble(reinterpret_cast<uint16_t *>(dataPtr)[index]));
    case CellKind::Float32ArrayKind:
      return HermesValue::encodeUntrustedNumberValue(
          reinterpret_cast<float *>(dataPtr)[index]);
    case CellKind::Float64ArrayKind:
      return HermesValue::encodeUntrustedNumberValue(
          reinterpret_cast<double *>(dataPtr)[index]);
    case CellKind::BigInt64ArrayKind: {
      // Bigint allocation doesn't allocate handles.
      NoHandleScope noHandleScope(runtime);
      auto cr = BigIntPrimitive::fromSigned(
          runtime, reinterpret_cast<int64_t *>(dataPtr)[index]);
      if (LLVM_UNLIKELY(cr.getStatus() == ExecutionStatus::EXCEPTION))
        hermes_fatal("Failed to encode BigInt in polyReadMayAlloc");
      return *cr;
    }
    case CellKind::BigUint64ArrayKind: {
      // Bigint allocation doesn't consume handles.
      NoHandleScope noHandleScope(runtime);
      auto cr = BigIntPrimitive::fromUnsigned(
          runtime, reinterpret_cast<uint64_t *>(dataPtr)[index]);
      if (LLVM_UNLIKELY(cr.getStatus() == ExecutionStatus::EXCEPTION))
        hermes_fatal("Failed to encode BigInt in polyReadMayAlloc");
      return *cr;
    }
    default:
      llvm_unreachable("Unknown TypedArray kind");
  }
}

#ifdef NDEBUG
LLVM_ATTRIBUTE_ALWAYS_INLINE
#endif
inline HermesValue JSTypedArrayBase::polyReadNoAlloc(
    JSTypedArrayBase *self,
    Runtime &runtime,
    size_type index) {
  assert(
      isNoAllocTypedArray(self) &&
      "polyReadNoAlloc() requires a no-alloc TypedArray");
  assert(self->attached(runtime) && "at() requires a JSArrayBuffer");
  assert(
      index < self->getLength() &&
      "That index is out of bounds of this TypedArray");

  uint8_t *dataPtr = self->data(runtime);

  CellKind kind = self->getKind();

  // Instead of a computed goto/switch, use a small series of conditional
  // branches to differentiate between integer and fp arrays, and between
  // element sizes in each group.
  // Once we have the integer element size, we load it and compute both an
  // unsigned and a sign extended version. Then we choose the correct one based
  // on the lowest bit of the cell kind, using a conditional move.
  // The low bit of cell kind is arranged so that it is always different for
  // signed and unsigned versions within the same size.

  // Assert the ordering of the CellKind enum.
  static_assert(
      CellKind::Uint8ArrayKind < CellKind::Int8ArrayKind &&
          CellKind::Int8ArrayKind < CellKind::Uint8ClampedArrayKind &&
          CellKind::Uint8ClampedArrayKind < CellKind::Uint16ArrayKind &&
          CellKind::Uint16ArrayKind < CellKind::Int16ArrayKind &&
          CellKind::Int16ArrayKind < CellKind::Uint32ArrayKind &&
          CellKind::Uint32ArrayKind < CellKind::Int32ArrayKind &&
          CellKind::Int32ArrayKind < CellKind::Float16ArrayKind &&
          CellKind::Float16ArrayKind < CellKind::Float32ArrayKind &&
          CellKind::Float32ArrayKind < CellKind::Float64ArrayKind,
      "CellKind ordering is not correct");
  // Assert that signed and unsigned versions of the same size have different
  // low bit.
  static_assert(
      ((int)CellKind::Uint8ArrayKind & 1) !=
              ((int)CellKind::Int8ArrayKind & 1) &&
          ((int)CellKind::Uint8ClampedArrayKind & 1) !=
              ((int)CellKind::Int8ArrayKind & 1) &&
          ((int)CellKind::Uint16ArrayKind & 1) !=
              ((int)CellKind::Int16ArrayKind & 1) &&
          ((int)CellKind::Uint32ArrayKind & 1) !=
              ((int)CellKind::Int32ArrayKind & 1),
      "CellKind ordering is not correct");

  if (kind <= CellKind::Int16ArrayKind) {
    if (kind <= CellKind::Uint8ClampedArrayKind) {
      // 8-bit
      constexpr unsigned uBitValue = (unsigned)CellKind::Uint8ArrayKind & 1;
      int32_t u = reinterpret_cast<uint8_t *>(dataPtr)[index];
      int32_t s = int32_t(uint32_t(u) << 24) >> 24;
      return HermesValue::encodeTrustedNumberValue(
          ((unsigned)kind & 1) == uBitValue ? u : s);
    } else {
      // 16-bit
      constexpr unsigned uBitValue = (unsigned)CellKind::Uint16ArrayKind & 1;
      int32_t u = reinterpret_cast<uint16_t *>(dataPtr)[index];
      int32_t s = int32_t(uint32_t(u) << 16) >> 16;
      return HermesValue::encodeTrustedNumberValue(
          ((unsigned)kind & 1) == uBitValue ? u : s);
    }
  } else {
    // kind >= CellKind::Uint32ArrayKind
    if (kind <= CellKind::Int32ArrayKind) {
      // 32-bit
      constexpr unsigned uBitValue = (unsigned)CellKind::Uint32ArrayKind & 1;
      if constexpr (sizeof(size_t) > 4) {
        int32_t s = reinterpret_cast<uint32_t *>(dataPtr)[index];
        uint32_t u = s;
        return HermesValue::encodeTrustedNumberValue(
            ((unsigned)kind & 1) == uBitValue ? (int64_t)u : (int64_t)s);
      } else {
        int32_t s = reinterpret_cast<uint32_t *>(dataPtr)[index];
        uint32_t u = s;
        return HermesValue::encodeTrustedNumberValue(
            ((unsigned)kind & 1) == uBitValue ? (double)u : (double)s);
      }
    } else {
      // Float16, Float32, or Float64
      if (kind == CellKind::Float16ArrayKind) {
        return HermesValue::encodeUntrustedNumberValue(
            float16ToDouble(reinterpret_cast<uint16_t *>(dataPtr)[index]));
      } else if (kind == CellKind::Float32ArrayKind) {
        return HermesValue::encodeUntrustedNumberValue(
            reinterpret_cast<float *>(dataPtr)[index]);
      } else {
        return HermesValue::encodeUntrustedNumberValue(
            reinterpret_cast<double *>(dataPtr)[index]);
      }
    }
  }
}

/// JSTypedArray is a collection with array semantics (random access indexing),
/// but stores a typed value with a known size, and has a static total size.
///
/// The templated versions represent the TypedArrays family from JS,
/// for example Int8Array is JSTypedArray<int8_t, CellKind::Int8ArrayKind>
template <typename T, CellKind C>
class JSTypedArray final : public JSTypedArrayBase {
 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return C;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == C;
  }

  static PseudoHandle<JSTypedArray<T, C>> create(
      Runtime &runtime,
      Handle<JSObject> prototype,
      Handle<HiddenClass> clazz);

  T *begin(PointerBase &base) {
    return reinterpret_cast<T *>(data(base));
  }
  T *end(PointerBase &base) {
    return begin(base) + length_;
  }

  /// Monomorphic access to the \p i'th element of the buffer. This is
  /// implemented for each TypedArray subclass.
  /// \pre
  ///   This cannot be called on a detached TypedArray.
  ///   i must be less than the length of the TypedArray.
  T &monoAt(PointerBase &base, size_type i) {
    assert(attached(base) && "at() requires a JSArrayBuffer");
    assert(i < getLength() && "That index is out of bounds of this TypedArray");
    return begin(base)[i];
  }

  static Handle<JSObject> getPrototype(const Runtime &runtime);
  static Handle<HiddenClass> getRootHiddenClass(const Runtime &runtime);
  static Handle<Callable> getConstructor(const Runtime &runtime);
  static SymbolID getName(Runtime &runtime);

  /// Allocate a new instance of a TypedArray of this type.
  /// \p length the length of the TypedArray to create.
  static CallResult<Handle<JSTypedArrayBase>> allocate(
      Runtime &runtime,
      size_type length = 0);

  /// Allocate a new instance of a TypedArray from the species constructor
  /// of the given \p self, and forward the \p length parameter to that
  /// constructor.
  static CallResult<Handle<JSTypedArrayBase>> allocateSpecies(
      Handle<JSTypedArrayBase> self,
      Runtime &runtime,
      size_type length);

  /// Converts a \p numeric to the type used by this typed array.
  /// NOTE: this function has specializations for types which don't use a
  /// truncated int32 representation. See the bottom of this file for their
  /// implementations.
  static inline T toDestType(const HermesValue &numeric) {
    return hermes::truncateToInt32(numeric.getNumber());
  }

 protected:
  /// Retrieve an indexed property.
  static HermesValue _getOwnIndexedImpl(
      PseudoHandle<JSObject> self,
      Runtime &runtime,
      uint32_t index);
  static CallResult<bool> _setOwnIndexedImpl(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      uint32_t index,
      Handle<> value);

 public:
  // NOTE: If any fields are ever added beyond the base class, then the
  // *BuildMeta functions must be updated to call addJSObjectOverlapSlots.

  explicit JSTypedArray(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz);
};

/// @name toDestType specializations
/// @{

template <>
inline uint8_t
JSTypedArray<uint8_t, CellKind::Uint8ClampedArrayKind>::toDestType(
    const HermesValue &numeric) {
  return toUInt8Clamp(numeric.getNumber());
}

template <>
inline uint16_t JSTypedArray<uint16_t, CellKind::Float16ArrayKind>::toDestType(
    const HermesValue &numeric) {
  return doubleToFloat16(numeric.getNumber());
}

template <>
inline float JSTypedArray<float, CellKind::Float32ArrayKind>::toDestType(
    const HermesValue &numeric) {
  // This can overflow a float, but float overflow goes to Infinity
  // (the correct behavior) on all modern platforms.
  return truncDoubleToFloat(numeric.getNumber());
}

template <>
inline double JSTypedArray<double, CellKind::Float64ArrayKind>::toDestType(
    const HermesValue &numeric) {
  return numeric.getNumber();
}

template <>
int64_t JSTypedArray<int64_t, CellKind::BigInt64ArrayKind>::toDestType(
    const HermesValue &numeric);

template <>
uint64_t JSTypedArray<uint64_t, CellKind::BigUint64ArrayKind>::toDestType(
    const HermesValue &numeric);

/// @}

/// Specialization declaration for Float16Array's _getOwnIndexedImpl. Must be
/// declared because it has an explicit instantiation.
template <>
HermesValue
JSTypedArray<uint16_t, CellKind::Float16ArrayKind>::_getOwnIndexedImpl(
    PseudoHandle<JSObject> self,
    Runtime &runtime,
    uint32_t index);

#define TYPED_ARRAY(name, type) \
  using name##Array = JSTypedArray<type, CellKind::name##ArrayKind>;
#include "hermes/VM/TypedArrays.def"

} // namespace vm
} // namespace hermes

#endif
