/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSDATAVIEW_H
#define HERMES_VM_JSDATAVIEW_H

#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSObject.h"

#include "llvh/Support/Endian.h"
#include "llvh/Support/SwapByteOrder.h"

namespace hermes {
namespace vm {

class JSDataView final : public JSObject {
 public:
  using size_type = JSArrayBuffer::size_type;
  using Super = JSObject;

  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSDataViewKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSDataViewKind;
  }

  /// Create a JSDataView using the standard DataView prototype.
  static PseudoHandle<JSDataView> create(Runtime &runtime);

  static PseudoHandle<JSDataView> create(
      Runtime &runtime,
      Handle<JSObject> prototype);

  /// \return the held buffer, which must not be null. The pointer is not
  /// rooted: pin it before the next allocation.
  JSArrayBuffer *getBuffer(Runtime &runtime) const {
    return buffer_.getNonNull(runtime);
  }

  /// \return the number of bytes viewed by this DataView.
  size_type byteLength() const {
    return length_;
  }

  /// \return the position within the buffer that this DataView points at.
  size_type byteOffset() const {
    return offset_;
  }

  /// Get the value stored in the bytes from offset to offset + sizeof(T), in
  /// either little or big endian order.
  /// \p offset The distance (in bytes) from the beginning of the DataView to
  ///   look at.
  /// \p littleEndian If true, then read the bytes in little endian order, else
  ///   read them as big endian order.
  /// \return The value which the bytes requested to view represent, as a type.
  /// \pre attached() must be true
  template <typename T>
  T get(Runtime &runtime, size_type offset, bool littleEndian) const;

  /// Set the value stored in the bytes from offset to offset + sizeof(T), in
  /// either little or big endian order.
  /// \p offset The distance (in bytes) from the beginning of the DataView to
  ///   look at.
  /// \p value The typed value to insert into the underlying storage.
  /// \p littleEndian Whether to write the value as little or big endian.
  /// \pre attached() must be true
  template <typename T>
  void set(Runtime &runtime, size_type offset, T value, bool littleEndian);

  /// Check if the underlying JSArrayBuffer is attached.
  /// \return true iff the JSArrayBuffer being viewed by this JSDataView is
  ///   attached to some storage.
  bool attached(Runtime &runtime) const {
    assert(
        buffer_ &&
        "Cannot call attached() when there is not even a buffer set");
    return buffer_.getNonNull(runtime)->attached();
  }

  void setBuffer(
      Runtime &runtime,
      JSArrayBuffer *buffer,
      size_type offset,
      size_type length) {
    assert(
        offset + length <= buffer->size() &&
        "A DataView cannot be looking outside of the storage");
    buffer_.setNonNull(runtime, buffer, runtime.getHeap());
    offset_ = offset;
    length_ = length;
  }

 public:
  friend void JSDataViewBuildMeta(const GCCell *cell, Metadata::Builder &mb);

 private:
  /// buffer_ is the underlying storage of the bytes for a DataView.
  GCPointer<JSArrayBuffer> buffer_;
  /// offset_ is the position within the buffer that the DataView begins at.
  size_type offset_;
  /// length_ is the amount of bytes the DataView views inside the storage.
  size_type length_;

 public:
  JSDataView(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz);
};

/// @name Implementations
/// @{

template <typename T>
T JSDataView::get(
    Runtime &runtime,
    JSDataView::size_type offset,
    bool littleEndian) const {
  assert(attached(runtime) && "Cannot get on a detached buffer");
  assert(
      offset + sizeof(T) <= length_ &&
      "Trying to read past the end of the storage");
  T result;
  ::memcpy(
      &result,
      buffer_.getNonNull(runtime)->getDataBlock() + offset_ + offset,
      sizeof(T));
  return llvh::support::endian::byte_swap(
      result,
      littleEndian ? llvh::support::endianness::little
                   : llvh::support::endianness::big);
}

template <typename T>
void JSDataView::set(
    Runtime &runtime,
    JSDataView::size_type offset,
    T value,
    bool littleEndian) {
  assert(attached(runtime) && "Cannot set on a detached buffer");
  assert(
      offset + sizeof(T) <= length_ &&
      "Trying to write past the end of the storage");
  value = llvh::support::endian::byte_swap(
      value,
      littleEndian ? llvh::support::endianness::little
                   : llvh::support::endianness::big);
  memcpy(
      buffer_.getNonNull(runtime)->getDataBlock() + offset_ + offset,
      &value,
      sizeof(T));
}

/// @}

} // namespace vm
} // namespace hermes

#endif
