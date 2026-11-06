/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSARRAYBUFFER_H
#define HERMES_VM_JSARRAYBUFFER_H

#include "hermes/VM/JSObject.h"
#include "hermes/VM/NativeState.h"
#include "hermes/VM/Runtime.h"

namespace hermes {
namespace vm {

/// A JSArrayBuffer is a light container over an array of bytes.
///
/// This should be used in combination with a typed array view over the buffer
/// in order to extract its information in different ways.
class JSArrayBuffer final : public JSObject {
 public:
  // A RangeError for a failed allocation should be thrown if the requested
  // amount is larger than 2 ^ 32 - 1.
  using size_type = std::uint32_t;

  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSArrayBufferKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSArrayBufferKind;
  }

  /// Create a JSArrayBuffer using the standard ArrayBuffer prototype.
  static PseudoHandle<JSArrayBuffer> create(Runtime &runtime);

  static PseudoHandle<JSArrayBuffer> create(
      Runtime &runtime,
      Handle<JSObject> prototype);

  /// Create an ArrayBuffer and sets the data block to be used by this
  /// JSArrayBuffer to be \p data with \p size.
  static PseudoHandle<JSArrayBuffer> createWithInternalDataBlock(
      Runtime &runtime,
      Handle<JSObject> prototype,
      uint8_t *data,
      size_type size);

  /// ES7 24.1.1.4
  /// NOTE: since SharedArrayBuffer does not exist, this does not use the
  /// SpeciesConstructor, it always allocates a normal ArrayBuffer.
  static CallResult<Handle<JSArrayBuffer>> clone(
      Runtime &runtime,
      Handle<JSArrayBuffer> src,
      size_type srcByteOffset,
      size_type srcSize);

  /// ES7 6.2.6.2
  static void copyDataBlockBytes(
      Runtime &runtime,
      JSArrayBuffer *dst,
      size_type dstIndex,
      JSArrayBuffer *src,
      size_type srcIndex,
      size_type count);

  /// Creates a data block of size \p size for this JSArrayBuffer to hold.
  /// Replaces the currently used data block.
  /// \p zero if true, zero out the data in the block, else leave it
  ///   uninitialized.
  /// \return ExecutionStatus::RETURNED iff the allocation was successful.
  static ExecutionStatus createDataBlock(
      Runtime &runtime,
      Handle<JSArrayBuffer> self,
      size_type size,
      bool zero = true);

  /// Sets the data block used by this JSArrayBuffer to be \p data, with size
  /// \p size. The deleter of the shared pointer \p context should clean up the
  /// external data when it is deallocated. The JS ArrayBuffer will share the
  /// ownership of the external data via a shared pointer. When GC finalizes
  /// this JSArrayBuffer, it will release its shared ownership.
  static void setExternalDataBlock(
      Runtime &runtime,
      Handle<JSArrayBuffer> self,
      uint8_t *data,
      size_type size,
      const std::shared_ptr<void> &context);

  /// \return A shared pointer whose deleter cleans up the external data block,
  /// specified when the external data was first set. In practice, users should
  /// always set a valid pointer to clean up the data, so the context ptr should
  /// not be null.
  /// \pre attached() and external() must be true
  static std::shared_ptr<void> getExternalDataContext(
      Runtime &runtime,
      Handle<JSArrayBuffer> self);

  /// Retrieves a pointer to the held buffer.
  /// \return A pointer to the buffer owned by this object. This can be null
  ///   if the ArrayBuffer is empty.
  /// \pre attached() must be true
  uint8_t *getDataBlock() {
    assert(attached() && "Cannot get a data block from a detached ArrayBuffer");
    return data_;
  }

  /// Get the size of this buffer.
  /// Returns 0 for detached buffers.
  size_type size() const {
    return size_;
  }

  /// Whether this JSArrayBuffer is attached to some data block.
  /// NOTE: a zero size JSArrayBuffer can be attached. Make sure to check both
  /// the attached-ness and the validity of any index before using it.
  bool attached() const {
    return attached_;
  }

  /// Whether this JSArrayBuffer is attached to some external data block.
  bool external() const {
    return external_;
  }

  /// Detaches this buffer from its data block, effectively freeing the storage
  /// and setting this ArrayBuffer to have zero size.  The \p gc argument allows
  /// the GC to be informed of this external memory deletion.
  static void detach(Runtime &runtime, Handle<JSArrayBuffer> self);

  /// Marks the JS ArrayBuffer as detached and "ejects" the held data block,
  /// releasing JS ArrayBuffer's ownership of the data. If the data block is an
  /// internal buffer, it is untracked from GC, but not freed. Otherwise, the
  /// external buffer is untracked by NativeState holding the buffer. The
  /// external buffer may be cleaned up if the NativeState was the only thing
  /// holding on to the buffer. WARNING: This is very dangerous and users must
  /// obtain the ownership of the data block through getDataBlock or
  /// getExternalDataContext before ejectng the buffer. \pre attached() must be
  /// true
  static void ejectBufferUnsafe(Runtime &runtime, Handle<JSArrayBuffer> self);

 protected:
  static void _finalizeImpl(GCCell *cell, GC &gc);
  static size_t _mallocSizeImpl(GCCell *cell);
#ifdef HERMES_MEMORY_INSTRUMENTATION
  static void _snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
  static void _snapshotAddNodesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
#endif

 private:
  /// Get the internal property that retains the NativeState owning the external
  /// buffer
  /// \pre attached() and external() must be true
  static PseudoHandle<NativeState> getExternalFinalizerNativeState(
      Runtime &runtime,
      Handle<JSArrayBuffer> self);

  /// Set the internal property that retains the NativeState owning the
  /// external buffer to \p value.
  static void setExternalFinalizer(
      Runtime &runtime,
      Handle<JSArrayBuffer> self,
      Handle<> value);

  /// Free the data block owned by this JSArrayBuffer.
  void freeInternalBuffer(GC &gc);

  /// Untrack the internal buffer from GC snapshots
  void untrackInternalBuffer(GC &gc);

  /// data_ size_, and external_ are only valid when attached_ is true.
  /// if detached_, data_ is nullptr, size_ is 0, and external_ is false.
  uint8_t *data_;
  size_type size_;
  bool external_;
  bool attached_;

  /// The x86-64 JIT emits an inline typed-array element store that reads
  /// data_ directly; RuntimeOffsets is where its offset is derived with
  /// offsetof().
  friend struct RuntimeOffsets;

 public:
  JSArrayBuffer(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz);

  ~JSArrayBuffer() = default;
};

} // namespace vm
} // namespace hermes

#endif
