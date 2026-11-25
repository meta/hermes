/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_HANDLEROOTOWNER_INLINE_H
#define HERMES_VM_HANDLEROOTOWNER_INLINE_H

#include "hermes/VM/Handle.h"
#include "hermes/VM/HandleRootOwner.h"

namespace hermes {
namespace vm {

template <class T>
inline Handle<T> HandleRootOwner::makeNullHandle() {
  return Handle<T>::vmcast_or_null(&nullPointer_);
}

inline Handle<HermesValue> HandleRootOwner::getUndefinedValue() {
  return Handle<HermesValue>(&undefinedValue_);
}

inline Handle<HermesValue> HandleRootOwner::getNullValue() {
  return Handle<HermesValue>(&nullValue_);
}

inline Handle<HermesValue> HandleRootOwner::getEmptyValue() {
  return Handle<HermesValue>(&emptyValue_);
}

inline Handle<HermesValue> HandleRootOwner::getBoolValue(bool b) {
  return Handle<HermesValue>(b ? &trueValue_ : &falseValue_);
}

inline Handle<HermesValue> HandleRootOwner::getZeroValue() {
  return Handle<HermesValue>(&zeroValue_);
}

inline Handle<HermesValue> HandleRootOwner::getOneValue() {
  return Handle<HermesValue>(&oneValue_);
}

inline Handle<HermesValue> HandleRootOwner::getNegOneValue() {
  return Handle<HermesValue>(&negOneValue_);
}

} // namespace vm
} // namespace hermes

#endif
