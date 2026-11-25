/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_PRIMITIVEBOX_H
#define HERMES_VM_PRIMITIVEBOX_H

#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/JSObject.h"

namespace hermes {
namespace vm {

/// String object.
class JSString final : public JSObject {
 public:
  using Super = JSObject;

  friend void JSStringBuildMeta(const GCCell *, Metadata::Builder &);

  // We need one more slot for the length property.
  static const PropStorage::size_type NAMED_PROPERTY_SLOTS =
      Super::NAMED_PROPERTY_SLOTS + 1;
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSStringKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSStringKind;
  }

  /// Create a JSString using the standard String prototype.
  static CallResult<PseudoHandle<JSString>> create(
      Runtime &runtime,
      Handle<StringPrimitive> value);

  static CallResult<PseudoHandle<JSString>> create(
      Runtime &runtime,
      Handle<StringPrimitive> value,
      Handle<JSObject> prototype);

  static CallResult<PseudoHandle<JSString>> create(
      Runtime &runtime,
      Handle<StringPrimitive> value,
      Handle<JSObject> prototype,
      Handle<HiddenClass> clazz);

  /// Set the [[PrimitiveValue]] internal property from a string.
  static void setPrimitiveString(
      Handle<JSString> selfHandle,
      Runtime &runtime,
      Handle<StringPrimitive> string);

  /// Return the [[PrimitiveValue]] internal property as a string.
  static StringPrimitive *getPrimitiveString(
      const JSString *self,
      PointerBase &runtime) {
    return self->primitiveValue_.get(runtime);
  }

  JSString(
      Runtime &runtime,
      Handle<StringPrimitive> value,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz),
        primitiveValue_(runtime, *value, runtime.getHeap()) {
    assert(hasIndexedStorage(runtime) && "must have indexed storage");
    assert(
        hasFastIndexProperties(runtime) && "must have fast index properties");
  }

 protected:
  /// Check whether property with index \p index exists in indexed storage and
  /// \return true if it does.
  static bool
  _haveOwnIndexedImpl(JSObject *self, Runtime &runtime, uint32_t index);

  /// Check whether property with index \p index exists in indexed storage and
  /// extract its \c PropertyFlags (if necessary checking whether the object is
  /// frozen or sealed).
  /// \return PropertyFlags if the property exists.
  static OptValue<PropertyFlags> _getOwnIndexedPropertyFlagsImpl(
      JSObject *self,
      Runtime &runtime,
      uint32_t index);

  /// \return the range of indexes (end-exclusive) in the array.
  static std::pair<uint32_t, uint32_t> _getOwnIndexedRangeImpl(
      JSObject *selfObj,
      PointerBase &runtime);

  /// Obtain an element from the "indexed storage" of this object. The storage
  /// itself is implementation dependent.
  /// \return the value of the element or "empty" if there is no such element.
  static HermesValue _getOwnIndexedImpl(
      PseudoHandle<JSObject> self,
      Runtime &runtime,
      uint32_t index);

  /// Set an element in the "indexed storage" of this object. Depending on the
  /// semantics of the "indexed storage" the storage capacity may need to be
  /// expanded (e.g. affecting Array.length), or the write may simply be ignored
  /// (in the case of typed arrays).
  /// \return true if the write succeeded, or false if it was ignored.
  static CallResult<bool> _setOwnIndexedImpl(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      uint32_t index,
      Handle<> value);

  /// Delete an element in the "indexed storage".
  /// \return 'true' if the element was successfully deleted, or if it was
  ///     outside of the storage range. 'false' if this storage doesn't support
  ///     "holes"/deletion (e.g. typed arrays).
  static bool _deleteOwnIndexedImpl(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      uint32_t index);

 private:
  GCPointer<StringPrimitive> primitiveValue_;
};

/// StringIterator object.
/// See ES6 21.1.5.3 for Properties of String Iterator Instances.
class JSStringIterator : public JSObject {
  using Super = JSObject;

  friend void JSStringIteratorBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);

 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSStringIteratorKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSStringIteratorKind;
  }

  static PseudoHandle<JSStringIterator> create(
      Runtime &runtime,
      Handle<StringPrimitive> string);

  /// Iterate to the next element and return.
  static CallResult<HermesValue> nextElement(
      Handle<JSStringIterator> self,
      Runtime &runtime);

  JSStringIterator(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<StringPrimitive> iteratedString)
      : JSObject(runtime, *parent, *clazz),
        iteratedString_(runtime, *iteratedString, runtime.getHeap()) {}

 private:
  /// [[IteratedString]]
  /// This is null if iteration has been completed.
  GCPointer<StringPrimitive> iteratedString_;

  /// [[StringIteratorNextIndex]]
  uint32_t nextIndex_{0};
};

/// BigInt object.
/// N.B.: users can create boxed bigints with
///   let o = new Object(1n),
/// but not with the BigInt constructor, i.e.,
///   let o = new BigInt(1)
/// as BigInt() is not a constructor.
class JSBigInt final : public JSObject {
 public:
  using Super = JSObject;

  friend void JSBigIntBuildMeta(const GCCell *, Metadata::Builder &);

  static const PropStorage::size_type NAMED_PROPERTY_SLOTS =
      Super::NAMED_PROPERTY_SLOTS;
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSBigIntKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSBigIntKind;
  }

  static PseudoHandle<JSBigInt> create(
      Runtime &runtime,
      Handle<BigIntPrimitive> value,
      Handle<JSObject> prototype);

  static CallResult<PseudoHandle<JSBigInt>> create(
      Runtime &runtime,
      Handle<JSObject> prototype) {
    struct : public Locals {
      PinnedValue<BigIntPrimitive> value;
    } lv;
    LocalsRAII lraii(runtime, &lv);

    auto bigIntRes = BigIntPrimitive::fromSigned(runtime, 0);
    if (LLVM_UNLIKELY(bigIntRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.value = bigIntRes->getBigInt();

    return create(runtime, lv.value, prototype);
  }

  /// Return the [[PrimitiveValue]] internal property as a bigint.
  static BigIntPrimitive *getPrimitiveBigInt(
      const JSBigInt *self,
      PointerBase &runtime) {
    return self->primitiveValue_.get(runtime);
  }

  JSBigInt(
      Runtime &runtime,
      Handle<BigIntPrimitive> value,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz),
        primitiveValue_(runtime, *value, runtime.getHeap()) {}

 private:
  GCPointer<BigIntPrimitive> primitiveValue_;
};

/// Number object.
class JSNumber final : public JSObject {
 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSNumberKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSNumberKind;
  }

  /// Create a JSNumber using the standard Number prototype.
  static PseudoHandle<JSNumber> create(Runtime &runtime, double value);

  static PseudoHandle<JSNumber>
  create(Runtime &runtime, double value, Handle<JSObject> prototype);

  static PseudoHandle<JSNumber> create(
      Runtime &runtime,
      double value,
      Handle<JSObject> prototype,
      Handle<HiddenClass> clazz);

  JSNumber(
      Runtime &runtime,
      double value,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz), primitiveValue_(value) {}

  double getPrimitiveNumber() const {
    return primitiveValue_;
  }

  void setPrimitiveNumber(double value) {
    primitiveValue_ = value;
  }

 private:
  double primitiveValue_;
};

/// Boolean object.
class JSBoolean final : public JSObject {
 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSBooleanKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSBooleanKind;
  }

  /// Create a JSBoolean using the standard Boolean prototype.
  static PseudoHandle<JSBoolean> create(Runtime &runtime, bool value);

  static PseudoHandle<JSBoolean>
  create(Runtime &runtime, bool value, Handle<JSObject> prototype);

  static PseudoHandle<JSBoolean> create(
      Runtime &runtime,
      bool value,
      Handle<JSObject> prototype,
      Handle<HiddenClass> clazz);

  JSBoolean(
      Runtime &runtime,
      bool value,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz), primitiveValue_(value) {}

  void setPrimitiveBoolean(bool b) {
    primitiveValue_ = b;
  }

  bool getPrimitiveBoolean() const {
    return primitiveValue_;
  }

 private:
  bool primitiveValue_;
};

/// Symbol object.
class JSSymbol final : public JSObject {
  friend void JSSymbolBuildMeta(const GCCell *cell, Metadata::Builder &mb);

 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSSymbolKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSSymbolKind;
  }

  static PseudoHandle<JSSymbol>
  create(Runtime &runtime, SymbolID value, Handle<JSObject> prototype);

  static PseudoHandle<JSSymbol> create(
      Runtime &runtime,
      SymbolID value,
      Handle<JSObject> prototype,
      Handle<HiddenClass> clazz);

  /// Return the [[PrimitiveValue]] internal property as a SymbolID.
  PseudoHandle<SymbolID> getPrimitiveSymbol() const {
    return PseudoHandle<SymbolID>::create(primitiveValue_);
  }

  JSSymbol(
      Runtime &runtime,
      SymbolID value,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz), primitiveValue_(value) {}

 private:
  const GCSymbolID primitiveValue_;
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_PRIMITIVEBOX_H
