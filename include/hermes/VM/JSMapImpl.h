/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSMAPIMPL_H
#define HERMES_VM_JSMAPIMPL_H

#include "hermes/VM/Callable.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/IterationKind.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/OrderedHashMap.h"

namespace hermes {
namespace vm {

/// The implementation of a JSMap object. Internally, a Map works the same way
/// as a Set, and hence we share the same implementation for them, with
/// different CellKind.
template <CellKind C>
class JSMapImpl final : public JSObject,
                        public OrderedHashMapBase<
                            std::conditional_t<
                                C == CellKind::JSMapKind,
                                HashMapEntry,
                                HashSetEntry>,
                            JSMapImpl<C>> {
  using HashMapEntryType =
      std::conditional_t<C == CellKind::JSMapKind, HashMapEntry, HashSetEntry>;
  using Super = JSObject;
  using ContainerSuper = OrderedHashMapBase<HashMapEntryType, JSMapImpl<C>>;

 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return C;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == C;
  }

  /// Create a JSMapImpl using the appropriate prototype from runtime.
  static PseudoHandle<JSMapImpl<C>> create(Runtime &runtime);

  static PseudoHandle<JSMapImpl<C>> create(
      Runtime &runtime,
      Handle<JSObject> parentHandle);

  /// Call \p callbackfn for each entry, with \p thisArg as this.
  static ExecutionStatus forEach(
      Handle<JSMapImpl> self,
      Runtime &runtime,
      Handle<Callable> callbackfn,
      Handle<> thisArg) {
    self->assertInitialized();
    auto iterCtx = self->newIterator(runtime);
    while (self->advanceIterator(runtime, iterCtx)) {
      if (LLVM_UNLIKELY(
              Callable::executeCall3(
                  callbackfn,
                  runtime,
                  thisArg,
                  self->iteratorValue(runtime, iterCtx),
                  self->iteratorKey(runtime, iterCtx),
                  self.getHermesValue()) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
    return ExecutionStatus::RETURNED;
  }

  /// Call the native \p callbackfn for each entry, with \p thisArg as this.
  /// \param callback Number of parameters depends on if this is a Map or Set.
  /// For Map, (Runtime&, HermesValue, HermesValue) -> ExecutionStatus. For Set,
  /// (Runtime&, HermesValue) -> ExecutionStatus.
  template <typename CB>
  static ExecutionStatus
  forEachNative(Handle<JSMapImpl> self, Runtime &runtime, CB callback) {
    self->assertInitialized();
    auto iterCtx = self->newIterator(runtime);
    while (self->advanceIterator(runtime, iterCtx)) {
      if constexpr (std::is_same_v<HashMapEntryType, HashMapEntry>) {
        if (LLVM_UNLIKELY(
                callback(
                    runtime,
                    self->iteratorKey(runtime, iterCtx),
                    self->iteratorValue(runtime, iterCtx)) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      } else {
        if (LLVM_UNLIKELY(
                callback(runtime, self->iteratorKey(runtime, iterCtx)) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }
    }

    return ExecutionStatus::RETURNED;
  }

  JSMapImpl(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz) {}

 private:
  /// Finalizer function to be called when GC is cleaning up this object.
  static void _finalizeImpl(GCCell *cell, GC &gc);
};

/// JSMapTypeTraits binds iterator type and its corresponding container type.
template <CellKind T>
struct JSMapTypeTraits;
template <>
struct JSMapTypeTraits<CellKind::JSSetIteratorKind> {
  static constexpr CellKind ContainerKind = CellKind::JSSetKind;
};
template <>
struct JSMapTypeTraits<CellKind::JSMapIteratorKind> {
  static constexpr CellKind ContainerKind = CellKind::JSMapKind;
};

template <CellKind C>
class JSMapIteratorImpl final : public JSObject {
  using Super = JSObject;
  using HashMapEntryType = typename std::conditional<
      C == CellKind::JSMapIteratorKind,
      HashMapEntry,
      HashSetEntry>::type;

 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return C;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == C;
  }

  /// Create a handle of JSMapIterator.
  static PseudoHandle<JSMapIteratorImpl<C>> create(
      Runtime &runtime,
      Handle<JSObject> prototype);

  /// Initialize the iterator, set its targeting Set data and iteration kind.
  /// The \p gc parameter is necessary for write barriers.
  void initializeIterator(
      Runtime &runtime,
      Handle<JSMapImpl<JSMapTypeTraits<C>::ContainerKind>> data,
      IterationKind kind) {
    data_.set(runtime, data.get(), runtime.getHeap());
    iterationKind_ = kind;

    assert(data_ && "Invalid storage data");
  }

  /// Check if the iterator has been properly initialized.
  /// data_ is nullptr either when the iterator is not initialized,
  /// or iteration has finished.
  bool isInitialized() const {
    return data_ || iterationFinished_;
  }

  /// Iterate to the next element and return.
  static CallResult<HermesValue> nextElement(
      Handle<JSMapIteratorImpl> self,
      Runtime &runtime) {
    struct : public Locals {
      PinnedValue<> value;
      PinnedValue<JSArray> arr;
    } lv;
    LocalsRAII lraii(runtime, &lv);
    if (!self->iterationFinished_) {
      // Iteration has not yet reached the end previously.
      assert(self->data_ && "Storage uninitialized");
      if (!self->iterCtx_.initialized()) {
        self->iterCtx_ = self->data_.getNonNull(runtime)->newIterator(runtime);
      }
      // Advance the iterator.
      if (self->data_.getNonNull(runtime)->advanceIterator(
              runtime, self->iterCtx_)) {
        switch (self->iterationKind_) {
          case IterationKind::Key:
            lv.value = self->data_.getNonNull(runtime)->iteratorKey(
                runtime, self->iterCtx_);
            break;
          case IterationKind::Value:
            lv.value = self->data_.getNonNull(runtime)->iteratorValue(
                runtime, self->iterCtx_);
            break;
          case IterationKind::Entry: {
            // If we are iterating both key and value, we need to create an
            // array.
            auto arrRes = JSArray::create(runtime, 2, 2);
            if (arrRes == ExecutionStatus::EXCEPTION) {
              return ExecutionStatus::EXCEPTION;
            }
            lv.arr = std::move(*arrRes);
            lv.value = self->data_.getNonNull(runtime)->iteratorKey(
                runtime, self->iterCtx_);
            if (LLVM_UNLIKELY(
                    JSArray::setElementAt(lv.arr, runtime, 0, lv.value) ==
                    ExecutionStatus::EXCEPTION))
              return ExecutionStatus::EXCEPTION;
            if constexpr (std::is_same_v<HashMapEntryType, HashMapEntry>) {
              lv.value = self->data_.getNonNull(runtime)->iteratorValue(
                  runtime, self->iterCtx_);
            }
            if (LLVM_UNLIKELY(
                    JSArray::setElementAt(lv.arr, runtime, 1, lv.value) ==
                    ExecutionStatus::EXCEPTION))
              return ExecutionStatus::EXCEPTION;
            lv.value = lv.arr.getHermesValue();
            break;
          };
          case IterationKind::NumKinds:
            llvm_unreachable("Invalid iteration kind");
            return HermesValue::encodeEmptyValue();
        }
      } else {
        // If the next element in the iterator is invalid, we have
        // reached the end.
        self->iterationFinished_ = true;
        self->iterCtx_.reset();
        self->data_.setNull(runtime.getHeap());
      }
    }
    return createIterResultObject(runtime, lv.value, self->iterationFinished_);
  }

  /// Build the metadata for this map implementation, and store it into \p mb.
  static void MapOrSetIteratorBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);

  JSMapIteratorImpl(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz) {}

 private:
  /// Finalizer function to be called when GC is cleaning up this object.
  static void _finalizeImpl(GCCell *cell, GC &gc);

  /// The internal pointer to the Map data. nullptr if the iterator has not been
  /// initialized or the iteration has ended.
  GCPointer<JSMapImpl<JSMapTypeTraits<C>::ContainerKind>> data_{nullptr};

  /// Holds context for the iterator.
  typename JSMapImpl<JSMapTypeTraits<C>::ContainerKind>::IteratorContext
      iterCtx_{};

  IterationKind iterationKind_;

  /// Indicating whether iteration has reached the end.
  bool iterationFinished_{false};
};

using JSMap = JSMapImpl<CellKind::JSMapKind>;
using JSSet = JSMapImpl<CellKind::JSSetKind>;
using JSMapIterator = JSMapIteratorImpl<CellKind::JSMapIteratorKind>;
using JSSetIterator = JSMapIteratorImpl<CellKind::JSSetIteratorKind>;

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_JSMAPIMPL_H
