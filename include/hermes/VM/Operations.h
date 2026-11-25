/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_OPERATIONS_H
#define HERMES_VM_OPERATIONS_H

#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/CallResult.h"
#include "hermes/VM/InternalProperty.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"
#include "hermes/VM/SymbolID.h"

#include "llvh/ADT/SmallVector.h"

namespace hermes {
namespace vm {

union DefinePropertyFlags;
class Runtime;

/// ES6.0 7.1.1
/// Type hint passed to toPrimitive_RJS() instead of string values.
enum class PreferredType {
  NONE,
  STRING,
  NUMBER,
};

/// ES6.0 7.1.1
CallResult<HermesValue>
toPrimitive_RJS(Runtime &runtime, Handle<> valueHandle, PreferredType hint);

/// ES6.0 7.1.1
/// The OrdinaryToPrimitive operation does not attempt to use the exotic
/// @@toPrimitive property on \p selfHandle.
CallResult<HermesValue> ordinaryToPrimitive(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    PreferredType preferredType);

/// ES5.1 9.2
bool toBoolean(HermesValue value);

/// ES5.1 9.3
CallResult<HermesValue> toNumber_RJS(Runtime &runtime, Handle<> valueHandle);

/// ES 2020 7.1.3 ToNumeric(value)
CallResult<HermesValue> toNumeric_RJS(Runtime &runtime, Handle<> valueHandle);

/// ES6 7.1.15
CallResult<HermesValue> toLength(Runtime &runtime, Handle<> valueHandle);

// a variant of toLength which returns a uint64_t
inline CallResult<uint64_t> toLengthU64(
    Runtime &runtime,
    Handle<> valueHandle) {
  auto res = toLength(runtime, valueHandle);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return res->getNumber();
}

/// ES 2018 7.1.17
CallResult<HermesValue> toIndex(Runtime &runtime, Handle<> valueHandle);

/// ES 2022 7.1.5
CallResult<HermesValue> toIntegerOrInfinity(
    Runtime &runtime,
    Handle<> valueHandle);

/// ES6 7.1.9
CallResult<HermesValue> toInt8(Runtime &runtime, Handle<> valueHandle);

/// ES6 7.1.7
CallResult<HermesValue> toInt16(Runtime &runtime, Handle<> valueHandle);

/// ES5.1 9.5
CallResult<HermesValue> toInt32_RJS(Runtime &runtime, Handle<> valueHandle);

/// ES6 7.1.10
CallResult<HermesValue> toUInt8(Runtime &runtime, Handle<> valueHandle);

/// ES6 7.1.11
uint8_t toUInt8Clamp(double number);
CallResult<HermesValue> toUInt8Clamp(Runtime &runtime, Handle<> valueHandle);

/// ES5.1 9.7
CallResult<HermesValue> toUInt16(Runtime &runtime, Handle<> valueHandle);

/// ES5.1 9.6
CallResult<HermesValue> toUInt32_RJS(Runtime &runtime, Handle<> valueHandle);

/// ES5.1 9.8
CallResult<PseudoHandle<StringPrimitive>> toString_RJS(
    Runtime &runtime,
    Handle<> valueHandle);

/// ES15 7.2.7 IsStringWellFormedUnicode
bool isStringWellFormedUnicode(StringPrimitive *string);

/// ES9 7.2.7
inline bool isPropertyKey(Handle<> valueHandle) {
  return valueHandle->isString() || valueHandle->isSymbol();
}

/// ES9 7.1.14
/// \return a Symbol or a String. The caller must pin the result before the
/// next safepoint.
inline CallResult<PseudoHandle<>> toPropertyKey(
    Runtime &runtime,
    Handle<> valueHandle) {
  CallResult<HermesValue> primRes =
      toPrimitive_RJS(runtime, valueHandle, PreferredType::STRING);
  if (LLVM_UNLIKELY(primRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (primRes->isSymbol()) {
    return PseudoHandle<>::create(*primRes);
  }
  struct : public Locals {
    PinnedValue<> prim;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.prim = *primRes;
  CallResult<PseudoHandle<StringPrimitive>> strRes =
      toString_RJS(runtime, lv.prim);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return PseudoHandle<>::create(strRes->getHermesValue());
}

/// This function is used to convert a property to property key if it's an
/// object. Check if \p valueHandle is an object, if so, convert it to a
/// key which may have side-effects. Otherwise just return the original
/// value.
/// ES6.0 7.1.14, but only for objects. Primitives are left unaltered.
/// Note: this can return a Symbol or a String if \p valueHandle is an object.
/// The caller must pin the result before the next safepoint.
inline CallResult<PseudoHandle<>> toPropertyKeyIfObject(
    Runtime &runtime,
    Handle<> valueHandle) {
  if (LLVM_UNLIKELY(valueHandle->isObject())) {
    return toPropertyKey(runtime, valueHandle);
  }
  return PseudoHandle<>::create(valueHandle.getHermesValue());
}

/// \return prototype for primitive \p base.
CallResult<Handle<JSObject>> getPrimitivePrototype(
    Runtime &runtime,
    Handle<> base);

/// ES5.1 9.9
CallResult<HermesValue> toObject(Runtime &runtime, Handle<> valueHandle);

/// Helper function. When accessing a property on an undefined/null object,
/// we can re-throw the exception with an improved error message that
/// contains the property name \id. \operationStr denotes the type of
/// operation (i.e. read, set and delete) on the object.
/// Example old error message: Cannot convert undefined value to object.
/// New error message would be:
/// Cannot \operationStr property [propName] of undefined.
ExecutionStatus amendPropAccessErrorMsgWithPropName(
    Runtime &runtime,
    Handle<> valueHandle,
    llvh::StringRef operationStr,
    SymbolID id);

/// ES5 9.10 CheckObjectCoercible
/// ES6+ 7.2.1 RequireObjectCoercible ( argument )
inline ExecutionStatus checkObjectCoercible(
    Runtime &runtime,
    Handle<> valueHandle) {
  if (LLVM_UNLIKELY(valueHandle->isUndefined() || valueHandle->isNull())) {
    return runtime.raiseTypeError("Value not coercible to object");
  }
  return ExecutionStatus::RETURNED;
}

/// ES6.0 7.2.9.
/// The SameValue(x, y) operation.
bool isSameValue(HermesValue x, HermesValue y);

/// ES6 7.2.10. The only difference from isSameValue is: +0 == -0.
inline bool isSameValueZero(HermesValue x, HermesValue y) {
  if (x.isNumber() && y.isNumber() && x.getNumber() == y.getNumber()) {
    // Takes care of +0 == -0.
    return true;
  }
  return isSameValue(x, y);
}

/// ES5.1 11.8.1.
CallResult<bool>
lessOp_RJS(Runtime &runtime, Handle<> leftHandle, Handle<> rightHandle);
/// ES5.1 11.8.2.
CallResult<bool>
greaterOp_RJS(Runtime &runtime, Handle<> leftHandle, Handle<> rightHandle);
/// ES5.1 11.8.3.
CallResult<bool>
lessEqualOp_RJS(Runtime &runtime, Handle<> leftHandle, Handle<> rightHandle);
/// ES5.1 11.8.4.
CallResult<bool>
greaterEqualOp_RJS(Runtime &runtime, Handle<> leftHandle, Handle<> rightHandle);

/// ES11 7.2.15 Abstract Equality Comparison
CallResult<bool>
abstractEqualityTest_RJS(Runtime &runtime, Handle<> xHandle, Handle<> yHandle);

/// ES5.1 11.9.6
bool strictEqualityTest(HermesValue x, HermesValue y);

/// Convert a string to a uniqued property name, stored in \p out (which must
/// be a rooted slot, e.g. a PinnedValue<SymbolID> in the caller's Locals).
inline ExecutionStatus stringToSymbolID(
    Runtime &runtime,
    PseudoHandle<StringPrimitive> strPrim,
    MutableHandle<SymbolID> out) {
  // Unique the string.
  return runtime.getIdentifierTable().getSymbolFromPrimitive(
      runtime, std::move(strPrim), out);
}

/// ES16.0 9.13
/// \return true if \p hv is suitable for use as a weak reference, i.e., an
/// Object or a non-registered Symbol.
inline bool canBeHeldWeakly(Runtime &rt, HermesValue hv) {
  return hv.isObject() ||
      (hv.isSymbol() && !rt.getSymbolRegistry().hasSymbol(hv.getSymbol()));
}
inline bool canBeHeldWeakly(Runtime &rt, SmallHermesValue hv) {
  return hv.isObject() ||
      (hv.isSymbol() && !rt.getSymbolRegistry().hasSymbol(hv.getSymbol()));
}

/// Convert a value to a uniqued property name, stored in \p out (which must be
/// a rooted slot, e.g. a PinnedValue<SymbolID> in the caller's Locals).
ExecutionStatus valueToSymbolID(
    Runtime &runtime,
    Handle<> nameValHnd,
    MutableHandle<SymbolID> out);

/// \return a string indicating the type of the operand corresponding to the
/// `typeof` operator.
HermesValue typeOf(Runtime &runtime, Handle<> valueHandle);

/// \return true if the type of \p arg corresponds to the flag in \p types.
bool matchTypeOfIs(HermesValue arg, TypeOfIsTypes types);

/// toArrayIndex where we have the StringView.
/// Useful if we don't want to materialize a StringPrimitive for a lazy
/// identifier.
inline OptValue<uint32_t> toArrayIndex(StringView str) {
  auto len = str.length();
  if (str.isASCII()) {
    const char *ptr = str.castToCharPtr();
    return hermes::toArrayIndex(ptr, ptr + len);
  }
  const char16_t *ptr = str.castToChar16Ptr();
  return hermes::toArrayIndex(ptr, ptr + len);
}

/// Convert a string to an array index following ES5.1 15.4.
/// A property name P (in the form of a String value) is an array index if and
/// only if ToString(ToUint32(P)) is equal to P and ToUint32(P) is not equal to
/// 2**32−1.
inline OptValue<uint32_t> toArrayIndex(StringPrimitive *str) {
  if (str->isASCII()) {
    auto ref = str->getStringRef<char>();
    return hermes::toArrayIndex(ref.data(), ref.data() + ref.size());
  }
  auto ref = str->getStringRef<char16_t>();
  return hermes::toArrayIndex(ref.data(), ref.data() + ref.size());
}

/// If it is possible to cheaply verify that \p value is an integer value in
/// the range [0..2**32-1), return it as an OptValue<uint32_t>. Note that
/// it this fails, the value may still be a valid index.
///
/// Note that the range is open and 0xFFFFFFFFu is not a valid index.
/// This matches JS requirements for JS arrays. However, we use this function
/// for all indexes, including those for TypedArrays. The JS spec does allow
/// typed arrays to have sizes (and indexes) up to (2**53 - 1).
/// Our implementation uses uint32_t for sizes and indexes, so 0xFFFFFFFFu is
/// not a valid index for us.
inline OptValue<uint32_t> toArrayIndexFastPath(HermesValue value) {
  // We rely on non-numbers being encoded as NaN.
  static_assert(
      HERMESVALUE_VERSION == 2,
      "HermesValue version changed, update this function");
  uint32_t index;
  if (sh_tryfast_f64_to_u32(value.f64, index) && index != 0xFFFFFFFFu)
    return index;
  return llvh::None;
}

/// \return true if the ToPrimitive function (ES5.1 9.1) performs no conversion.
/// Primitive types: Undefined, Null, Boolean, Number, and String.
inline bool isPrimitive(HermesValue val) {
  assert(!val.isEmpty() && "empty value encountered");
  return !val.isObject();
}

/// ES5.1 11.6.1
CallResult<HermesValue>
addOp_RJS(Runtime &runtime, Handle<> xHandle, Handle<> yHandle);

/// ES9.0 12.6.4
inline double expOp(double x, double y) {
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();

  // Handle special cases that std::pow handles differently.
  if (std::isnan(y)) {
    return nan;
  } else if (y == 0) {
    return 1;
  } else if (std::abs(x) == 1 && std::isinf(y)) {
    return nan;
  }

  // std::pow handles the other edge cases as the ES9.0 spec requires.
  return std::pow(x, y);
}

/// ES5.1 7.2
inline bool isWhiteSpaceChar(char16_t c) {
  return c == u'\u0009' || c == u'\u000B' || c == u'\u000C' || c == u'\u0020' ||
      c == u'\u00A0' || c == u'\uFEFF' || c == u'\u1680' ||
      (c >= u'\u2000' && c <= u'\u200A') || c == u'\u202F' || c == u'\u205F' ||
      c == u'\u3000';
}

/// ES5.1 7.3
inline bool isLineTerminatorChar(char16_t c) {
  return c == u'\u000A' || c == u'\u000D' || c == u'\u2028' || c == u'\u2029';
}

/// Takes a letter (a-z or A-Z) and makes it lowercase.
inline char16_t letterToLower(char16_t c) {
  return c | 32;
}

/// Takes a non-empty string (without the leading "0x" if hex) and parses it
/// as radix \p radix.
/// \returns the double that results, and NaN on failure.
inline double parseIntWithRadix(const StringView str, int radix) {
  auto res =
      hermes::parseIntWithRadix</* AllowNumericSeparator */ false>(str, radix);
  return res ? res.getValue() : std::numeric_limits<double>::quiet_NaN();
}

/// ES5.1 9.8.1
/// Convert \p m to its string value following the JS spec.
CallResult<PseudoHandle<StringPrimitive>> numberToStringPrimitive(
    Runtime &runtime,
    double m);

/// Takes a finite double \p number and a base \p radix (between 2 and 36
/// inclusive), and returns the string that results from converting \p number
/// into a string in base \p radix.
/// Note that this is implementation defined behavior: we output a rounded
/// string such that we're as close as possible to the actual precision encoded
/// in the double value given by number.
/// \returns the string that results.
PseudoHandle<StringPrimitive>
numberToStringWithRadix(Runtime &runtime, double number, unsigned radix);

/// ES6.0 7.3.9
CallResult<PseudoHandle<>>
getMethod(Runtime &runtime, Handle<> O, Handle<> key);

/// ES9.0 Record type for iterator records.
/// Used for caching the "next" method to avoid repeated property lookups.
template <typename T>
struct IteratorRecord {
  /// Actual iterator object.
  const Handle<JSObject> iterator;

  /// Cache for the "next" method to call to step the iterator.
  const Handle<T> nextMethod;

  IteratorRecord(Handle<JSObject> iterator, Handle<T> nextMethod)
      : iterator(iterator), nextMethod(nextMethod) {}
};

/// Rooted storage for the fields of an IteratorRecord<T>. Embed it in a
/// Locals struct and pass it to getIterator/getCheckedIterator, which return
/// a record that refers to it, so the record is valid for as long as the
/// storage is.
template <typename T>
class IteratorRecordStorage {
 public:
  PinnedValue<JSObject> iterator;
  PinnedValue<T> nextMethod;
};

// The nextMethod of this IteratorRecord could be of any type.
using UncheckedIteratorRecord = IteratorRecord<HermesValue>;
// This is an IteratorRecord that has its next method explicity checked and
// defined as a Callable type.
using CheckedIteratorRecord = IteratorRecord<Callable>;
using UncheckedIteratorRecordStorage = IteratorRecordStorage<HermesValue>;
using CheckedIteratorRecordStorage = IteratorRecordStorage<Callable>;

/// ES6.0 7.4.1
/// \param obj object to iterate over.
/// \param storage rooted storage backing the returned record.
/// \param method an optional method to call instead of retrieving @@iterator.
/// \return the iterator object
/// If the "next" method is not a Callable, throw a TypeError.
CallResult<CheckedIteratorRecord> getCheckedIterator(
    Runtime &runtime,
    Handle<> obj,
    CheckedIteratorRecordStorage &storage,
    llvh::Optional<Handle<Callable>> method = llvh::None);

/// ES6.0 7.4.1
/// \param obj object to iterate over.
/// \param storage rooted storage backing the returned record.
/// \param method an optional method to call instead of retrieving @@iterator.
/// \return the iterator object
/// This does not check if the "next" method is a Callable.
CallResult<UncheckedIteratorRecord> getIterator(
    Runtime &runtime,
    Handle<> obj,
    UncheckedIteratorRecordStorage &storage,
    llvh::Optional<Handle<Callable>> method = llvh::None);

/// ES6.0 7.4.2
CallResult<PseudoHandle<JSObject>> iteratorNext(
    Runtime &runtime,
    const CheckedIteratorRecord &iteratorRecord,
    llvh::Optional<Handle<>> value = llvh::None);

/// ES6.0 7.4.5
/// \return ? Get(iterResult, "value").
CallResult<PseudoHandle<HermesValue>> iteratorValue(
    Runtime &runtime,
    Handle<JSObject> iterResult);

/// ES2023 7.4.6
/// \return a null pointer instead of the boolean false. The caller must pin
/// the result before the next safepoint.
CallResult<PseudoHandle<JSObject>> iteratorStep(
    Runtime &runtime,
    const CheckedIteratorRecord &iteratorRecord);

/// ES16 7.4.10
/// \param iteratorRecord the iterator to get the next value from
/// \param[out] value the pointer in which the next available value will be
/// stored
/// \return false instead of DONE if the iterator has reached its end, or
/// true if the next value is available
CallResult<bool> iteratorStepValue(
    Runtime &runtime,
    const CheckedIteratorRecord &iteratorRecord,
    PinnedValue<> *value);

/// ES sec-iteratorclose
/// \param completion the thrown value to complete this operation with, empty if
/// not thrown.
ExecutionStatus
iteratorClose(Runtime &runtime, Handle<JSObject> iterator, Handle<> completion);

/// Call the IteratorClose operation following an exception being thrown.
/// \pre runtime.thrownValue_ must be populated with a thrown value.
/// \return ExecutionStatus::EXCEPTION
ExecutionStatus iteratorCloseAndRethrow(
    Runtime &runtime,
    Handle<JSObject> iterator);

/// This function combines ES2023 7.4.12 IterableToList and 7.3.18
/// CreateArrayFromList into one API.
CallResult<PseudoHandle<JSArray>> iterableToArray(
    Runtime &runtime,
    Handle<HermesValue> items);

/// Some types of errors are considered "uncatchable" by the VM.
/// If any native code wants to catch an error, it needs to check that the value
/// is catchable first.
/// \return true if the HermesValue is something that can be caught in a catch
/// block.
bool isUncatchableError(HermesValue value);

/// ES6.0 7.4.7
/// \return the new iterator result object.
HermesValue createIterResultObject(Runtime &runtime, Handle<> value, bool done);

/// ES7 7.3.20
CallResult<Handle<Callable>> speciesConstructor(
    Handle<JSObject> O,
    Runtime &runtime,
    Handle<Callable> defaultConstructor);

/// ES7 7.2.4
/// Returns true if the \c value is a constructor.  The value can be
/// Anything.
bool isConstructor(Runtime &runtime, HermesValue value);

/// ES7 7.2.4
/// Returns true if \c callable is a constructor.  Passing \c nullptr
/// is allowed, and returns false.
bool isConstructor(Runtime &runtime, Callable *callable);

/// ES15 10.1.13 OrdinaryCreateFromConstructor
/// Return an object which is intended to be used as the `this` argument in a
/// construct call.
CallResult<PseudoHandle<JSObject>> ordinaryCreateFromConstructor_RJS(
    Runtime &runtime,
    Handle<Callable> constructor,
    Handle<JSObject> intrinsicDefaultProto);

/// ES6.0 7.2.8
/// Returns true if the object is a JSRegExp or has a Symbol.match property that
/// evaluates to true.
CallResult<bool> isRegExp(Runtime &runtime, Handle<> arg);

/// ES6.0 7.3.19
CallResult<bool>
ordinaryHasInstance(Runtime &runtime, Handle<> constructor, Handle<> object);

/// ES6.0 10.1.1
/// \param cp the codepoint to convert to UTF-16.
/// \param[out] output the vector into which to place the results.
inline void utf16Encoding(
    uint32_t cp,
    llvh::SmallVectorImpl<char16_t> &output) {
  // Assert: 0 ≤ cp ≤ 0x10FFFF.
  assert(cp <= 0x10FFFF && "Invalid input to UTF16Encoding");
  // If cp ≤ 65535, return cp.
  if (cp <= 65535) {
    output.push_back((char16_t)cp);
    return;
  }
  // Let cu1 be floor((cp – 65536) / 1024) + 0xD800.
  // The floor is handled by using integer arithmetic.
  char16_t cu1 = (char16_t)(((cp - 65536) / 1024) + 0xD800);
  // Let cu2 be ((cp – 65536) modulo 1024) + 0xDC00.
  char16_t cu2 = (char16_t)(((cp - 65536) % 1024) + 0xDC00);
  // Return the code unit sequence consisting of cu1 followed by cu2.
  output.push_back(cu1);
  output.push_back(cu2);
}

/// ES6.0 10.1.2
/// Two code units, lead and trail, that form a UTF-16 surrogate pair are
/// converted to a code point.
inline uint32_t utf16Decode(char16_t lead, char16_t trail) {
  // Assert: 0xD800 ≤ lead ≤ 0xDBFF and 0xDC00 ≤ trail ≤ 0xDFFF.
  assert(
      0xD800 <= lead && lead <= 0xDBFF && 0xDC00 <= trail && trail <= 0xDFFF &&
      "Invalid input to UTF16Decode");
  // Let cp be (lead – 0xD800) × 1024 + (trail – 0xDC00) + 0x10000.
  // Return the code point cp.
  uint32_t c1 = lead - 0xD800;
  uint32_t c2 = trail - 0xDC00;
  return c1 * 1024 + c2 + 0x10000;
}

/// ES6.0 12.9.4
CallResult<bool>
instanceOfOperator_RJS(Runtime &runtime, Handle<> object, Handle<> constructor);

/// ES6.0 19.4.3.2.1 Runtime Semantics: SymbolDescriptiveString ( sym )
/// Returns "Symbol([description])" given a symbol.
CallResult<PseudoHandle<StringPrimitive>> symbolDescriptiveString(
    Runtime &runtime,
    Handle<SymbolID> sym);

/// ES9 7.2.2
CallResult<bool> isArray(Runtime &runtime, JSObject *obj);

/// ES6.0 22.1.3.1.1
CallResult<bool> isConcatSpreadable(Runtime &runtime, Handle<> value);

/// \return true if and only if \p id is a primitive SymbolID backing a JS
/// Symbol instance.
constexpr bool isSymbolPrimitive(SymbolID id) {
  return id.isNotUniqued() && !InternalProperty::isInternal(id);
}

/// \return true if and only if \p id is a primitive SymbolID backing a JS
/// Property Name.
constexpr bool isPropertyNamePrimitive(SymbolID id) {
  return id.isUniqued();
}

/// ES5.1 8.10.5. toPropertyDescriptor(O). The result is written into
/// \p flags and \p valueOrAccessor together to represent a descriptor.
ExecutionStatus toPropertyDescriptor(
    Handle<> obj,
    Runtime &runtime,
    DefinePropertyFlags &flags,
    MutableHandle<> valueOrAccessor);

/// ES9 6.2.5.4 FromPropertyDescriptor
/// \p dpFlags is used to create the property descriptor object.
/// If dpFlags.isAccessor() is true, it will be an Acessor Property Descriptor
/// and so it will have getter and setter fields (they might be undefined).
/// If dpFlags.isAccessor() is false, it will be a Data Property Descriptor, and
/// so it will have value field, and also will have writable field if
/// setWritable is true. enumurable and configurable fields will be set if
/// setEnumurable and setConfigurable are set to true respectively.
CallResult<HermesValue> objectFromPropertyDescriptor(
    Runtime &runtime,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor);

// ES2022 7.2.6 IsIntegralNumber(argument)
bool isIntegralNumber(double number);

/// ES2022 21.2.1.1.1 NumberToBigInt(number)
inline CallResult<HermesValue> numberToBigInt(Runtime &runtime, double number) {
  if (!isIntegralNumber(number)) {
    return runtime.raiseRangeError("number is not integral");
  }

  return BigIntPrimitive::fromDouble(runtime, number);
}

// ES2022 7.1.13 ToBigInt(argument)
CallResult<HermesValue> toBigInt_RJS(Runtime &runtime, Handle<> value);

// ES2022 7.1.14 StringToBigInt
CallResult<HermesValue> stringToBigInt(Runtime &runtime, Handle<> value);

// ES2022 21.2.3 Properties of the BigInt Prototype Object - thisBigIntValue
CallResult<HermesValue> thisBigIntValue(Runtime &runtime, Handle<> value);

/// Set the read-only length and raw properties on \p templateObj and \p rawObj.
/// Seal both objects.
/// Intended to be called from GetTemplateObject methods after both objects
/// have already been populated.
ExecutionStatus setTemplateObjectProps(
    Runtime &runtime,
    Handle<JSObject> templateObj,
    Handle<JSObject> rawObj);

/// ES16 24.2.1.2 GetSetRecord(obj)
/// For the given object, store the SetRecord fields in the provided pointers
/// \p size, \p hasMethod, and \p keysMethod.
ExecutionStatus getSetRecord(
    Runtime &runtime,
    Handle<> obj,
    double *size,
    PinnedValue<Callable> *hasMethod,
    PinnedValue<Callable> *keysMethod);

// ES16 24.5.1 CanonicalizeKeyedCollectionKey(key)
inline HermesValue canonicalizeKeyedCollectionKey(HermesValue key) {
  if (key.isNumber() && key.getNumber() == 0) {
    return HermesValue::encodeTrustedNumberValue(0);
  }
  return key;
}

/// ES15.0 7.3.35 GroupBy ( items, callback, keyCoercion )
/// https://tc39.es/ecma262/#sec-groupby
/// Groups a list of items into an Object based on the results of a callback
/// which operates on each item.
/// \param target the Object to which to add keyed groups.
/// \param items array of ECMAValues to be grouped.
/// \param callback callback which returns the key per given item.
ExecutionStatus groupByProperty(
    Runtime &runtime,
    Handle<JSObject> target,
    Handle<> items,
    Handle<> callback);

/// ES15.0 7.3.35 GroupBy ( items, callback, keyCoercion )
/// https://tc39.es/ecma262/#sec-groupby
/// Groups a list of items into a Map based on the results of a callback which
/// operates on each item.
/// \param target the Map to which to add keyed groups.
/// \param items array of ECMAValues to be grouped.
/// \param callback callback which returns the key per given item.
ExecutionStatus groupByCollection(
    Runtime &runtime,
    Handle<JSMapImpl<CellKind::JSMapKind>> target,
    Handle<> items,
    Handle<> callback);

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_OPERATIONS_H
