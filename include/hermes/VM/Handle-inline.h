/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_HANDLE_INLINE_H
#define HERMES_VM_HANDLE_INLINE_H

#include "hermes/VM/Handle.h"

#include "hermes/VM/Casting.h"
#include "hermes/VM/HandleRootOwner.h"

namespace hermes {
namespace vm {

template <typename T>
template <typename U>
inline PseudoHandle<T> PseudoHandle<T>::vmcast(PseudoHandle<U> &&other) {
  auto result = PseudoHandle<T>::create(hermes::vm::vmcast<T>(other.get()));
  other.invalidate();
  return result;
}

template <typename T>
template <typename U>
inline PseudoHandle<T> PseudoHandle<T>::dyn_vmcast(PseudoHandle<U> &&other) {
  auto result = PseudoHandle<T>::create(hermes::vm::dyn_vmcast<T>(other.get()));
  other.invalidate();
  return result;
}

template <typename T>
template <typename U>
inline Handle<T> Handle<T>::dyn_vmcast(const Handle<U> &other) {
  return vmisa<T>(other.get()) ? Handle<T>(other, true)
                               : HandleRootOwner::makeNullHandle<T>();
}

} // namespace vm
} // namespace hermes

#endif
