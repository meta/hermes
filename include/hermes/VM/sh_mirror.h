/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SH_MIRROR_H
#define HERMES_SH_MIRROR_H

#include "hermes/VM/JIT/Config.h"
#include "hermes/VM/sh_legacy_value.h"
#include "hermes/VM/sh_runtime.h"

#include <stddef.h>

#ifdef HERMESVM_COMPRESSED_POINTERS
typedef uint32_t SHCompressedPointerRawType;
#else
typedef uintptr_t SHCompressedPointerRawType;
#endif

typedef uint32_t SHSymbolID;
typedef uint32_t SHWeakRootSymbolID;

typedef struct SHNativeFuncInfo SHNativeFuncInfo;
typedef struct SHUnit SHUnit;

/// Struct mirroring the layout of PropertyCacheEntry. This allows us to expose
/// the offsets of certain fields without needing to make the actual C++ version
/// available here.

typedef struct SHWritePropertyCacheEntry {
  SHCompressedPointerRawType clazz;
  uint32_t slotAndAddCacheIndex;
} SHWritePropertyCacheEntry;

typedef struct SHReadPropertyCacheEntry {
  SHCompressedPointerRawType clazz;
  SHCompressedPointerRawType negMatchClazz;
  uint16_t _slot16;
  uint8_t numChanges;
} SHReadPropertyCacheEntry;

typedef struct SHPrivateNameCacheEntry {
  SHCompressedPointerRawType clazz;
  SHWeakRootSymbolID nameVal;
  uint32_t slot;
} SHPrivateNameCacheEntry;

// Constants for the SHKindAndSize struct.
#define SH_KINDANDSIZE_NUM_KIND_BITS 8

// Conditioned on the size of SHCompressedPointerRawType.
#if SH_COMPRESSED_POINTER_SIZE == 4
#define SH_KINDANDSIZE_NUM_BITS 32
#define SH_KINDANDSIZE_NUM_SIZE_BITS 24
#else
#define SH_KINDANDSIZE_NUM_BITS 64
#define SH_KINDANDSIZE_NUM_SIZE_BITS 32
#endif

/// Struct mirroring the layout of KindAndSize class.
/// This mirrors the single raw field layout from KindAndSize.
typedef struct SHKindAndSize {
  SHCompressedPointerRawType size : SH_KINDANDSIZE_NUM_SIZE_BITS;
  SHCompressedPointerRawType kind : SH_KINDANDSIZE_NUM_KIND_BITS;
} SHKindAndSize;

/// Struct mirroring the layout of GCCell.
typedef struct SHGCCell {
  SHKindAndSize kindAndSize;
#ifndef NDEBUG
  uint16_t magic;
  uint32_t debugAllocationId;
#endif
} SHGCCell;

/// Struct mirroring the layout of JSObject (without the direct props).
typedef struct SHJSObject {
  SHGCCell base;
  SHObjectFlags flags;
  SHCompressedPointerRawType clazz;
  SHCompressedPointerRawType propStorage;
} SHJSObject;

#ifdef HERMESVM_BOXED_DOUBLES
typedef SHCompressedPointerRawType SHGCSmallHermesValue;
#else
typedef SHLegacyValue SHGCSmallHermesValue;
#endif

typedef struct SHJSObjectAndDirectProps {
  SHJSObject base;
  SHGCSmallHermesValue directProps[HERMESVM_DIRECT_PROPERTY_SLOTS];
} SHJSObjectAndDirectProps;

/// Struct mirroring the layout of Callable.
typedef struct SHCallable {
  SHJSObject base;
  SHCompressedPointer environment;
} SHCallable;

/// A pointer to native function.
typedef SHLegacyValue (*NativeJSFunctionPtr)(SHRuntime *shr);

/// Struct mirroring the layout of NativeJSFunction.
typedef struct SHNativeJSFunction {
  SHCallable base;
  NativeJSFunctionPtr functionPtr;
  SHNativeFuncInfo *funcInfo;
  SHUnit *unit;
} SHNativeJSFunction;

/// Struct mirroring the layout of Environment.
typedef struct SHEnvironment {
  SHGCCell base;
  SHCompressedPointer parentEnvironment;
  uint32_t size;

  SHLegacyValue slots[0];
} SHEnvironment;

/// Struct mirroring the layout of FastArray.
typedef struct SHFastArray {
  SHJSObject base;
  SHCompressedPointer indexedStorage;
  SHGCSmallHermesValue length;
} SHFastArray;

/// Struct mirroring the layout of ArrayStorageSmall.
typedef struct SHArrayStorageSmall {
  SHGCCell base;

  uint32_t size;
  SHGCSmallHermesValue storage[0];
} SHArrayStorageSmall;

/// Struct mirroring the layout of ArrayStorage.
typedef struct SHArrayStorage {
  SHGCCell base;

  uint32_t size;
  SHLegacyValue storage[0];
} SHArrayStorage;

/// Struct mirroring the layout of BoxedDouble.
typedef struct SHBoxedDouble {
  SHGCCell base;
  double value_;
} SHBoxedDouble;

/// Struct mirroring the layout of detail::Transition.
typedef struct SHHiddenClassTransition {
  uint32_t symbolID;
  uint16_t propertyFlags;
  uint16_t classFlags;
} SHTransition;

/// Struct mirroring the layout of detail::TransitionMap.
typedef struct SHHiddenClassTransitionMap {
  SHTransition smallKey;
  void *unionMember;
} SHTransitionMap;

/// Struct mirroring the layout of HiddenClass.
typedef struct SHHiddenClass {
  SHGCCell base;
  SHCompressedPointerRawType propertyMap;
  SHCompressedPointerRawType parent;
  SHCompressedPointerRawType objectParent;
  SHCompressedPointerRawType forInCache;
  SHSymbolID symbolID;
  uint16_t propertyFlags;
  unsigned numProperties;
#if HERMESVM_JIT
  uint16_t lazyJITId;
#endif
  uint16_t flags;
  SHTransitionMap transitionMap;
  void *parentTransitionMap;
} SHHiddenClass;

enum SHCellKind {
#define CELL_KIND(name) SHCellKind_##name,
#define SHARED_CELL_KIND(name) SHCellKind_##name,
#define CELL_RANGE(rangeName, first, last)             \
  SHCellKind_##rangeName##_first = SHCellKind_##first, \
  SHCellKind_##rangeName##_last = SHCellKind_##last,
#include "hermes/VM/CellKinds.def"
};

#endif
