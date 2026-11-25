/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_ARRAYLIKE_H
#define HERMES_VM_ARRAYLIKE_H

#include "hermes/VM/CallResult.h"
#include "hermes/VM/Handle.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"

namespace hermes {
namespace vm {

/// ES9 7.3.17 CreateListFromArrayLike
/// Returns the length of the List
inline CallResult<uint64_t> getArrayLikeLength_RJS(
    Handle<JSObject> arrayLikeHandle,
    Runtime &runtime) {
  auto propRes = JSObject::getNamed_RJS(
      arrayLikeHandle, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<> prop;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.prop = std::move(*propRes);
  return toLengthU64(runtime, lv.prop);
}

/// ES9 7.3.17 CreateListFromArrayLike
/// This will call ElementCB length times, once for each element,
/// passing \c Runtime*, the \c uint64_t index, and \c PseudoHandle<>
/// value.  ElementCB is responsible for consuming the elements
/// however it likes.  It is permitted to allocate, and it must return
/// ExecutionStatus.
template <typename ElementCB>
ExecutionStatus createListFromArrayLike_RJS(
    Handle<JSObject> arrayLikeHandle,
    Runtime &runtime,
    uint64_t length,
    const ElementCB &elementCB) {
  Handle<ArrayImpl> elemArray = Handle<ArrayImpl>::dyn_vmcast(arrayLikeHandle);
  if (LLVM_LIKELY(elemArray)) {
    for (uint64_t elemIdx = 0; elemIdx < length; ++elemIdx) {
      // Fast path: we already have an array, so try and bypass the getComputed
      // checks and the handle loads & stores. Directly call ArrayImpl::at,
      // and only call getComputed if the element is empty.
      PseudoHandle<> elem = createPseudoHandle(
          elemArray->at(runtime, elemIdx).unboxToHV(runtime));
      if (LLVM_LIKELY(!elem->isEmpty())) {
        if (LLVM_UNLIKELY(
                elementCB(runtime, elemIdx, std::move(elem)) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        continue;
      }
      // Slow path fallback: the actual getComputed on this,
      // because the real value could be up the prototype chain.
      CallResult<PseudoHandle<>> propRes =
          getIndexed_RJS(runtime, arrayLikeHandle, elemIdx);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_UNLIKELY(
              elementCB(runtime, elemIdx, std::move(*propRes)) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  } else {
    // Not an array. Use this slow path.
    for (uint64_t elemIdx = 0; elemIdx < length; ++elemIdx) {
      CallResult<PseudoHandle<>> propRes =
          getIndexed_RJS(runtime, arrayLikeHandle, elemIdx);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_UNLIKELY(
              elementCB(runtime, elemIdx, std::move(*propRes)) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }
  return ExecutionStatus::RETURNED;
}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_ARRAYLIKE_H
