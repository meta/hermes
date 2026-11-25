/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_HOST_MODEL_H
#define HERMES_VM_HOST_MODEL_H

#include "hermes/VM/Callable.h"
#include "hermes/VM/DecoratedObject.h"

namespace hermes {
namespace vm {

/// A function to clean up the context used by a native function
typedef void (*FinalizeNativeFunctionPtr)(void *context);

/// This class represents a host function callable from JavaScript.
/// This is nearly the same as a NativeFunction, except an additional
/// function pointer can do cleanup when the FinalizableNativeFunction is
/// finalized.
class FinalizableNativeFunction final : public NativeFunction {
  FinalizeNativeFunctionPtr finalizePtr_;

  /// \return the slot index of the 'length' property in the class made by
  /// createClass(). It is the first slot after the overlap slots.
  static constexpr SlotIndex lengthPropIndex() {
    return numOverlapSlots<FinalizableNativeFunction>();
  }

  /// \return the slot index of the 'name' property in the class made by
  /// createClass().
  static constexpr SlotIndex namePropIndex() {
    return lengthPropIndex() + 1;
  }

 public:
  static const CallableVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::FinalizableNativeFunctionKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::FinalizableNativeFunctionKind;
  }

  /// Create the class that createWithoutPrototype() uses. It adds the
  /// 'length' and 'name' properties to \p rootClazz, with the same flags and
  /// in the same order as Callable::defineNameLengthAndPrototype().
  /// \param rootClazz the root class of FinalizableNativeFunction, whose
  ///   object parent is Function.prototype.
  /// \return the new class. The caller must root it before it allocates.
  static HiddenClass *createClass(
      Runtime &runtime,
      Handle<HiddenClass> rootClazz);

  /// Create an instance of FinalizableNativeFunction. Its parent is
  /// Function.prototype, and it has the 'length' and 'name' properties.
  /// \param functionPtr the host function
  /// \param finalizePtr the finalizer function
  /// \param paramCount number of parameters (excluding `this`)
  static CallResult<HermesValue> createWithoutPrototype(
      Runtime &runtime,
      void *context,
      NativeFunctionPtr functionPtr,
      FinalizeNativeFunctionPtr finalizePtr,
      SymbolID name,
      unsigned paramCount);

  void *getContext() {
    return context_;
  }

  FinalizableNativeFunction(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      void *context,
      NativeFunctionPtr functionPtr,
      FinalizeNativeFunctionPtr finalizePtr)
      : NativeFunction(
            runtime,
            parent,
            clazz,
            Runtime::makeNullHandle<Environment>(),
            context,
            functionPtr),
        finalizePtr_(finalizePtr) {}

 protected:
  ~FinalizableNativeFunction() {
    finalizePtr_(context_);
  }

  /// Call the native function with arguments already on the stack.
  static CallResult<PseudoHandle<>> _callImpl(
      Handle<Callable> selfHandle,
      Runtime &runtime);

  static void _finalizeImpl(GCCell *cell, GC &) {
    auto *self = vmcast<FinalizableNativeFunction>(cell);
    // Destruct the object.
    self->~FinalizableNativeFunction();
  }
};

/// When a HostObject is created, this proxy provides the business
/// logic for the HostObject's implementation.
class HostObjectProxy : public DecoratedObject::Decoration {
 public:
  // This is called when the object is finalized.
  virtual ~HostObjectProxy();

  // This is called to fetch a property value by name.
  virtual CallResult<HermesValue> get(SymbolID) = 0;

  // This is called to set a property value by name.  It will return
  // \c ExecutionStatus, and set the runtime's thrown value as appropriate.
  virtual CallResult<bool> set(SymbolID, HermesValue) = 0;

  // This is called to query names of properties.  In case of failure it will
  // return \c ExecutionStatus::EXCEPTION, and set the runtime's thrown Value
  // as appropriate. The result is stored in the provided MutableHandle.
  virtual ExecutionStatus getHostPropertyNames(
      MutableHandle<JSArray> result) = 0;
};

class HostObject final : public DecoratedObject {
 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::HostObjectKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::HostObjectKind;
  }

  /// Create an instance of HostObject with no prototype.
  static CallResult<HermesValue> createWithoutPrototype(
      Runtime &runtime,
      std::unique_ptr<HostObjectProxy> proxy);

  CallResult<HermesValue> get(SymbolID name) {
    return getProxy()->get(name);
  }

  CallResult<bool> set(SymbolID name, HermesValue value) {
    return getProxy()->set(name, value);
  }

  ExecutionStatus getHostPropertyNames(MutableHandle<JSArray> result) {
    return getProxy()->getHostPropertyNames(result);
  }

  HostObjectProxy *getProxy() {
    return static_cast<HostObjectProxy *>(this->getDecoration());
  }

  const HostObjectProxy *getProxy() const {
    return static_cast<const HostObjectProxy *>(this->getDecoration());
  }

  HostObject(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      std::unique_ptr<HostObjectProxy> proxy)
      : DecoratedObject(runtime, parent, clazz, std::move(proxy)) {}
};

} // namespace vm
} // namespace hermes

#endif
