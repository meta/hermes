/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSTypedArray.h"

namespace hermes::vm {

constexpr inline uint32_t JSObject::maxYoungGenAllocationPropCount() {
  return PropStorage::capacityForAllocationSize(
             GC::maxYoungGenAllocationSize() -
             heapAlignSize(cellSize<JSObject>())) +
      DIRECT_PROPERTY_SLOTS;
}

template <typename IndexType>
#ifdef NDEBUG
LLVM_ATTRIBUTE_ALWAYS_INLINE
#endif
    inline OptValue<HermesValue> tryFastGetComputedMayAlloc(
        Runtime &runtime,
        JSObject *obj,
        IndexType index) {
  static_assert(
      std::is_unsigned<IndexType>::value,
      "IndexType must be an unsigned integer type");
  // Note that we need to watch out for the case where index is more than
  // 32-bit.

  // Fast path for JSArray
  if (vmisa<ArrayImpl>(obj)) {
    // Make sure it is a valid array index (< 0xFFFF'FFFFu).
    if (index < 0xFFFF'FFFFu) {
      // We don't need to check for fast index properties here, because if there
      // are non-fast ones, the corresponding slot will be empty.
      auto *jsArray = vmcast<ArrayImpl>(obj);
      SmallHermesValue shv = jsArray->at(runtime, index);
      if (LLVM_LIKELY(!shv.isEmpty()))
        return shv.unboxToHV(runtime);
    }
  }
  // Fast path for JSTypedArray
  else if (LLVM_LIKELY(vmisa<JSTypedArrayBase>(obj))) {
    auto *jsTypedArray = vmcast<JSTypedArrayBase>(obj);

    // No need to check whether it is a valid index, since we are already
    // comparing against length.
    if (LLVM_LIKELY(
            index < jsTypedArray->getLength() &&
            jsTypedArray->attached(runtime))) {
      return JSTypedArrayBase::polyReadMayAlloc(jsTypedArray, runtime, index);
    }
    // For out-of-bounds or detached TypedArray, return undefined
    return HermesValue::encodeUndefinedValue();
  }
  // Fall-back fast path for other objects with fast index properties.
  // Make sure that the index is within 32-bit range, since getOwnIndexed()
  // only works for 32-bit indexes.
  else if (index <= UINT32_MAX && obj->hasFastIndexProperties(runtime)) {
    HermesValue hv =
        JSObject::getOwnIndexed(createPseudoHandle(obj), runtime, index);
    if (LLVM_LIKELY(!hv.isEmpty()))
      return hv;
  }

  return llvh::None;
}

template <typename IndexType>
#ifdef NDEBUG
LLVM_ATTRIBUTE_ALWAYS_INLINE
#endif
    inline OptValue<HermesValue> tryFastGetComputedNoAlloc(
        Runtime &runtime,
        JSObject *obj,
        IndexType index) {
  static_assert(
      std::is_unsigned<IndexType>::value,
      "IndexType must be an unsigned integer type");
  // Note that we need to watch out for the case where index is more than
  // 32-bit.

  // Fast path for JSArray
  if (vmisa<ArrayImpl>(obj)) {
    // Make sure it is a valid array index (< 0xFFFF'FFFFu).
    if (index < 0xFFFF'FFFFu) {
      // We don't need to check for fast index properties here, because if there
      // are non-fast ones, the corresponding slot will be empty.
      auto *jsArray = vmcast<ArrayImpl>(obj);
      SmallHermesValue shv = jsArray->at(runtime, index);
      if (LLVM_LIKELY(!shv.isEmpty()))
        return shv.unboxToHV(runtime);
    }
  }
  // Fast path for JSTypedArray
  else if (LLVM_LIKELY(JSTypedArrayBase::isNoAllocTypedArray(obj))) {
    auto *jsTypedArray = vmcast<JSTypedArrayBase>(obj);

    // No need to check whether it is a valid index, since we are already
    // comparing against length.
    if (LLVM_LIKELY(
            index < jsTypedArray->getLength() &&
            jsTypedArray->attached(runtime))) {
      return JSTypedArrayBase::polyReadNoAlloc(jsTypedArray, runtime, index);
    }
    // For out-of-bounds or detached TypedArray, return undefined
    return HermesValue::encodeUndefinedValue();
  }

  return llvh::None;
}

} // namespace hermes::vm
