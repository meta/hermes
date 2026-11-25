/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_HANDLEROOTOWNER_H
#define HERMES_VM_HANDLEROOTOWNER_H

#include "hermes/VM/Casting.h"
#include "hermes/VM/GCDecl.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/HermesValueTraits.h"
#include "hermes/VM/SymbolID.h"

namespace hermes {
namespace vm {

// Forward declarations.
class HandleBase;
template <typename T>
class Handle;
template <typename T>
class MutableHandle;

/// A base class that provides statically allocated constant handles and
/// convenience functions to access them.
class HandleRootOwner {
 public:
  /// Make a null handle of the specified type, viewing a static constant.
  template <class T>
  static inline Handle<T> makeNullHandle();

  /// An efficient way to pass undefined to a function accepting Handle.
  static inline Handle<HermesValue> getUndefinedValue();

  /// An efficient way to pass null to a function accepting Handle.
  static inline Handle<HermesValue> getNullValue();

  /// An efficient way to pass empty to a function accepting Handle.
  static inline Handle<HermesValue> getEmptyValue();

  /// An efficient way to pass bools to a function accepting Handle.
  static inline Handle<HermesValue> getBoolValue(bool b);

  /// An efficient way to pass 0 to a function accepting Handle.
  static inline Handle<HermesValue> getZeroValue();

  /// An efficient way to pass 1 to a function accepting Handle.
  static inline Handle<HermesValue> getOneValue();

  /// An efficient way to pass -1 to a function accepting Handle.
  static inline Handle<HermesValue> getNegOneValue();

 protected:
  /// Used for efficient construction of Handle<>(..., nullptr).
  static constexpr PinnedHermesValue nullPointer_{
      HermesValue::encodeNullptrObjectValueUnsafe()};
  /// Used for efficient construction of Handle(undefined).
  static constexpr PinnedHermesValue undefinedValue_{
      HermesValue::encodeUndefinedValue()};
  /// Used for efficient construction of Handle(null).
  static constexpr PinnedHermesValue nullValue_{HermesValue::encodeNullValue()};
  /// Used for efficient construction of Handle(empty).
  static constexpr PinnedHermesValue emptyValue_{
      HermesValue::encodeEmptyValue()};
  /// Used for efficient construction of Handle(bool).
  static constexpr PinnedHermesValue trueValue_{
      HermesValue::encodeBoolValue(true)};
  static constexpr PinnedHermesValue falseValue_{
      HermesValue::encodeBoolValue(false)};
  /// Used for efficient construction of Handle(0).
  static constexpr PinnedHermesValue zeroValue_{HVConstants::kZero};
  /// Used for efficient construction of Handle(1).
  static constexpr PinnedHermesValue oneValue_{HVConstants::kOne};
  /// Used for efficient construction of Handle(-1).
  static constexpr PinnedHermesValue negOneValue_{HVConstants::kNegOne};
};

} // namespace vm
} // namespace hermes

#endif
