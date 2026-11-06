/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SH_RUNTIME_H
#define HERMES_SH_RUNTIME_H

#include "hermes/VM/sh_legacy_value.h"
#include "hermes/VM/sh_segment_info.h"

#include "libhermesvm-config.h"

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SHUnit SHUnit;
typedef struct SHLocals SHLocals;

/// This struct represents an element in the exception handler stack. This
/// represents a try, and contains the information necessary to jump to its
/// associated catch in the event of an exception.
typedef struct SHJmpBuf {
  /// The previous element in the stack.
  struct SHJmpBuf *prev;

  /// The state required to longjmp back to the point at which the try was
  /// entered.
  jmp_buf buf;
} SHJmpBuf;

/// This class represents the context of the VM that is shared between generated
/// SH code and the VM implementation itself. It is intended to serve as the
/// base class of the C++ Runtime class.
typedef struct SHRuntime {
#if defined(HERMESVM_COMPRESSED_POINTERS) && !defined(HERMESVM_CONTIGUOUS_HEAP)
  /// To support 32-bit GC pointers in segmentIdx:offset form, segmentMap maps
  /// segment indices to "biased segment starts." This "bias" speeds up the
  /// decoding of a compressed pointer. If the segmentMap contained the actual
  /// segment starts, decoding an offset would require a shift to extract the
  /// segmentIdx, a lookup of the segment start, a subtraction of the shifted
  /// index to obtain the offset, and an addition to get the final native
  /// pointer. That is,
  ///   segmentIdx = basedPtr >> kLogSize;
  ///   segmentStart = segmentMap_[segmentIdx]
  ///   offset = basedPtr - (segmentIdx << kLogSize)
  ///   native_ptr = segmentStart + offset
  ///
  /// The bias allows us to avoid the subtraction: the segment table entry for
  /// index i is <segment-start> - (i << kLogSize) So the calculation can be:
  ///   segmentIdx = based_ptr >> kLogSize;
  ///   biasedSegmentStart = segmentMap_[segmentIdx]
  ///       [== segmentStart - (segmentIdx << kLogSize)]
  ///   native_ptr = basedPtr + biasedSegmentStart
  ///       [== basedPtr + segmentStart - (segmentIdx << kLogSize)
  ///        == segmentStart + (basedPtr - (segmentIdx << kLogSize))
  ///        == segmentStart + offset]
  ///
  /// Because we use the same 8-byte alignment in the offsets as we do in object
  /// pointers, the max heap size for 32-bit pointers is 4GB. With 4MB segments,
  /// that's 1024 segments, with the first slot reserved for null.
  void *segmentMap[1 << (32 - HERMESVM_LOG_HEAP_SEGMENT_SIZE)];
#else
  /// Dummy field to avoid warnings about an empty struct having different sizes
  /// in C and C++. Delete it once we add other fields.
  uint8_t dummy;
#endif

  /// The active SHUnits in this runtime.
  SHUnit *units[8];

  /// The current top of the exception handler stack.
  SHJmpBuf *shCurJmpBuf;

  /// Past-the-end pointer for the current frame. This points to the first
  /// uninitialized element at the end of the stack.
  SHLegacyValue *stackPointer;

  /// Start of the register stack.
  SHLegacyValue *registerStackStart;

  /// End of the register stack.
  SHLegacyValue *registerStackEnd;

  /// Points to the first register in the current frame. The current frame
  /// continues up to stackPointer (exclusive).
  SHLegacyValue *currentFrame;

  /// [SH] head of locals list.
  SHLocals *shLocals;

#define SHRUNTIME_HV_FIELD(name) SHLegacyValue name;
#include "hermes/VM/SHRuntimeHermesValueFields.def"
} SHRuntime;

#ifdef HERMESVM_COMPRESSED_POINTERS

/// A struct representing a compressed pointer in C code.
#define SH_COMPRESSED_POINTER_SIZE 4
typedef struct SHCompressedPointer {
  uint32_t raw;
} SHCompressedPointer;

#ifdef HERMESVM_CONTIGUOUS_HEAP

/// Encode the given non-null \p ptr as a compressed pointer and return it. This
/// is more efficient that _sh_cp_encode since we can elide the null check.
static inline SHCompressedPointer _sh_cp_encode_non_null(
    SHRuntime *shr,
    void *ptr) {
  assert(ptr && "Pointer must be non-null");
  // In contiguous heap mode, a non-null pointer is the offset from the start of
  // the heap, represented by shr.
  SHCompressedPointer cp = {(uint32_t)((uintptr_t)ptr - (uintptr_t)shr)};
  return cp;
}

/// Encode the given \p ptr as a compressed pointer and return the new value.
static inline SHCompressedPointer _sh_cp_encode(SHRuntime *shr, void *ptr) {
  // Null pointers require an additional check, since they are not in the range
  // of the contiguous heap.
  if (ptr)
    return _sh_cp_encode_non_null(shr, ptr);
  SHCompressedPointer cp = {0};
  return cp;
}

/// Decode the non-null compressed pointer \p ptr into a normal pointer and
/// return it. This is more efficient than _sh_cp_decode since we can elide the
/// null check.
static inline void *_sh_cp_decode_non_null(
    SHRuntime *shr,
    SHCompressedPointer ptr) {
  assert(ptr.raw && "Pointer must be non-null");
  // Decode the pointer by adding the offset back to the base.
  return (void *)(ptr.raw + (uintptr_t)shr);
}

/// Decode the compressed pointer \p ptr into a normal pointer and return it.
static inline void *_sh_cp_decode(SHRuntime *shr, SHCompressedPointer ptr) {
  // Null pointers are encoded as 0, so check for them directly.
  if (ptr.raw)
    return _sh_cp_decode_non_null(shr, ptr);
  return 0;
}

#else

/// Encode the given non-null \p ptr as a compressed pointer and return it. This
/// is more efficient that _sh_cp_encode since we can elide the null check.
static inline SHCompressedPointer _sh_cp_encode_non_null(
    SHRuntime *shr,
    void *ptr) {
  assert(ptr && "Pointer must be non-null");
  uintptr_t uptr = (uintptr_t)ptr;
  uintptr_t offsetMask = (1 << HERMESVM_LOG_HEAP_SEGMENT_SIZE) - 1;
  // Calculate the offset from the segment start.
  uint32_t offset = uptr & offsetMask;
  // Now get the segment index, and shift it so that its bits do not overlap
  // with those of offset.
  // TODO: Store the shifted index in SegmentInfo to save an operation here.
  SHSegmentInfo *info = (SHSegmentInfo *)(uptr & ~offsetMask);
  SHCompressedPointer cp;
  cp.raw = (info->index << HERMESVM_LOG_HEAP_SEGMENT_SIZE) | offset;
  return cp;
}

/// Encode the given \p ptr as a compressed pointer and return the new value.
static inline SHCompressedPointer _sh_cp_encode(SHRuntime *shr, void *ptr) {
  // Encoding relies on retrieving information from the start of the segment. We
  // need to explicitly check for null and encode it as 0, since it is not in a
  // segment.
  if (ptr)
    return _sh_cp_encode_non_null(shr, ptr);
  SHCompressedPointer cp = {0};
  return cp;
}

/// Decode the compressed pointer \p ptr into a normal pointer and return it.
static inline void *_sh_cp_decode(SHRuntime *shr, SHCompressedPointer ptr) {
  // Get the segment index by shifting out the low bits.
  unsigned segIdx = ptr.raw >> HERMESVM_LOG_HEAP_SEGMENT_SIZE;
  // Index into the segment map to retrieve the base address, and add the
  // compressed pointer to it.
  char *segBase = (char *)shr->segmentMap[segIdx];
  return segBase + ptr.raw;
}

/// Decode the non-null compressed pointer \p ptr into a normal pointer and
/// return it.
static inline void *_sh_cp_decode_non_null(
    SHRuntime *shr,
    SHCompressedPointer ptr) {
  assert(ptr.raw && "Pointer must be non-null");
  return _sh_cp_decode(shr, ptr);
}

#endif
#else

/// A struct that wraps an ordinary pointer, to be used in place of a compressed
/// pointer when compressed pointers are disabled.
#define SH_COMPRESSED_POINTER_SIZE HERMESVM_SIZEOF_VOID_P
typedef struct SHCompressedPointer {
  uintptr_t raw;
} SHCompressedPointer;

/// Wrap the given \p ptr in an SHCompressedPointer and return it.
static inline SHCompressedPointer _sh_cp_encode(SHRuntime *shr, void *ptr) {
  SHCompressedPointer cp = {(uintptr_t)ptr};
  return cp;
}

/// Wrap the given non-null \p ptr in an SHCompressedPointer and return it.
static inline SHCompressedPointer _sh_cp_encode_non_null(
    SHRuntime *shr,
    void *ptr) {
  assert(ptr && "Pointer must be non-null");
  return _sh_cp_encode(shr, ptr);
}

/// Unwrap the given \p ptr into a normal pointer and return it.
static inline void *_sh_cp_decode(SHRuntime *shr, SHCompressedPointer ptr) {
  return (void *)ptr.raw;
}

/// Unwrap the given non-null \p ptr into a normal pointer and return it.
static inline void *_sh_cp_decode_non_null(
    SHRuntime *shr,
    SHCompressedPointer ptr) {
  assert(ptr.raw && "Pointer must be non-null");
  return _sh_cp_decode(shr, ptr);
}

#endif

/// Number of stack words after the top of frame that we always ensure are
/// available. This is necessary so we can perform native calls with small
/// number of arguments without checking.
#define SH_STACK_RESERVE 32

static inline uint32_t _sh_available_stack_size(const SHRuntime *shr) {
  return (uint32_t)(shr->registerStackEnd - shr->stackPointer);
}

/// Check whether there is space to allocate <tt>count + SH_STACK_RESERVE</tt>
/// stack registers.
/// \param shr pointer to SHRuntime
/// \param count number of registers to check for
/// \return true if there is enough space
static inline bool _sh_check_available_stack(
    const SHRuntime *shr,
    uint32_t count) {
  // Note: use 64-bit arithmetic to avoid overflow. We could also do it with
  // a couple of comparisons, but that is likely to be slower.
  return _sh_available_stack_size(shr) >= (uint64_t)count + SH_STACK_RESERVE;
}

/// Allocate stack space for \p count registers, but keep it uninitialized.
/// The caller should initialize it ASAP.
/// \param shr pointer to SHRuntime
/// \param count number of registers to allocate
/// \return the new stack pointer
static inline SHLegacyValue *_sh_alloc_uninitialized_stack(
    SHRuntime *shr,
    uint32_t count) {
  assert(_sh_available_stack_size(shr) >= count && "register stack overflow");
  return (shr->stackPointer += count);
}

/// Initialize stack space with zeroes using fast vectorized pattern.
/// \param base the starting address of the space to initialize
/// \param count the number of SHLegacyValues to initialize
static inline void _sh_init_stack_with_zeroes(
    SHLegacyValue *base,
    uint32_t count) {
  SHLegacyValue *lim = base + count;
  // Fill the first count % 4 elements so we can fill 4 at a time in the loop.
  if (count & 1)
    *(base++) = _sh_ljs_raw_zero_value_unsafe();

  if (count & 2) {
    *(base++) = _sh_ljs_raw_zero_value_unsafe();
    *(base++) = _sh_ljs_raw_zero_value_unsafe();
  }

  // Store the remaining elements 4 at a time, allowing the compiler to use
  // vectorized stores for up to 4 elements at a time. Note that the empty asm
  // statement at the end of the loop prevents the compiler from storing more
  // elements together.
  while (base < lim) {
    *(base++) = _sh_ljs_raw_zero_value_unsafe();
    *(base++) = _sh_ljs_raw_zero_value_unsafe();
    *(base++) = _sh_ljs_raw_zero_value_unsafe();
    *(base++) = _sh_ljs_raw_zero_value_unsafe();

#ifdef __GNUC__
    // This empty asm statement tells the compiler that we want each write to
    // be observable, so it cannot turn the loop into a memset call.
    asm volatile("" : : : "memory");
#endif
  }
  assert(base == lim && "Fill failed");
}

/// Check whether <tt>count + STACK_RESERVE</tt> stack registers are available
/// and allocate \p count registers.
/// \param count number of registers to allocate.
/// \return \c true if allocation was successful.
static inline bool _sh_check_and_alloc_stack(SHRuntime *shr, uint32_t count) {
  if (!_sh_check_available_stack(shr, count))
    return false;

  SHLegacyValue *fillPtr = shr->stackPointer;
  _sh_alloc_uninitialized_stack(shr, count);
  // Initialize the new registers.
  _sh_init_stack_with_zeroes(fillPtr, count);
  return true;
}

#ifdef __cplusplus
}
#endif
#endif
