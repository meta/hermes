/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

//===----------------------------------------------------------------------===//
/// \file
/// This file defines the following functions/macros on supported platforms and
/// compilers:
/// \code
///      bool sh_tryfast_f64_to_i32(double x, int32_t &intres);
///      bool sh_tryfast_f64_to_u32(double x, uint32_t &intres);
///      bool sh_tryfast_f64_to_i64(double x, int64_t &intres);
///      bool sh_tryfast_f64_to_u64(double x, uint64_t &intres);
/// \endcode
///
/// These should be used for optimistically attempting to convert a f64 to an
/// integer. The conversion succeeds if the input is representable as an integer
/// of the appropriate size and signedness \b and for 64-bit integers other
/// implementation-defined restrictions on the input are also met.
///
/// The idea is to use these fast implementations when there is a high
/// probability that the input is representable as an integer. If they fail,
/// a slower algorithm may be needed.
///
/// The implementation depends on the following helpers, which are also exposed:
///
/// \code
///      int32_t _sh_tryfast_f64_to_i32_cvt(double);
///      uint32_t _sh_tryfast_f64_to_u32_cvt(double);
///      int64_t _sh_tryfast_f64_to_i64_cvt(double);
///      uint64_t _sh_tryfast_f64_to_u64_cvt(double);
/// \endcode
///
/// These helpers convert from f64 to an integer without undefined behavior,
/// unlike a C cast. The i32/u32 versions are guaranteed to return a correctly
/// truncated value if the input is in range, or an implementation-dependent
/// value otherwise. The i64/u64 versions are not guaranteed to return a correct
/// value, but in practice they do most of the time.
///
/// The only correct way to use these helpers is by round-tripping, that is
/// casting back to double and comparing in order to determine whether the
/// conversion was a success. Like this:
/// \code
///   int64_t i = _sh_tryfast_f64_to_i64_cvt(d);
///   if ((double)i == d) ...
/// \endcode
///
/// This check can provide false negatives: in other words, it can fail even if
/// the input value was in range. However, the check will never generate false
/// positives, it other words it will never succeed if the input value is
/// invalid.
///
/// The higher level functions \c sh_tryfast_f64_to_iXX() simple wrap the
/// conversion and the comparison.
///
/// \section Fast or Slow 64-bit Conversion
///
/// This file always defines the following define to 1 or 0.
/// \code
/// #define HERMES_TRYFAST_F64_TO_64_IS_FAST 0 or 1
/// \endcode
/// 1 indicates that the truncation to a 64-bit integer can be considered
/// "fast" or at least "as fast as converting to 32-bit integer".
///
/// \section Unsupported Platforms
///
/// The file errors by default if an unsupported platform/compiler is
/// encountered unless HERMES_TRYFAST_FP_CVT_ENABLE_UNSUPPORTED is defined.
/// This enables safe but perhaps slower implementations on unsupported
/// platforms.
///
/// \section Challenges of round-tripping
///
/// In JS fast paths it is often necessary to quickly check whether a double
/// is representable as some integer type. We do it by "round-tripping"
/// (converting to integer, then back to double, then comparing):
/// \code
///   int64_t i = (int64_t)d;
///   if ((double)i == d) ...
/// \endcode
/// There are two problems in this code.
///
/// First, there is UB when \c d is out-of-range of \c int64_t. This
/// problem is solved by using the new UB-lessconversion functions provided
/// here.
///
/// The second problem is more subtle. When naively converting from double to a
/// 64-bit integer (signed or unsigned), round-tripping is not always guaranteed
/// to work. Certain cases can cause loss of precision, leading to false
/// positives in the comparison.
///
/// If the input double is within the range of i64, the underlying hardware
/// conversion produces an integer which can always be converted back to f64
/// without loss of precision.
///
/// However, when the conversion from double to i64 overflows (the value of the
/// double does not fit inside i64), the conversion produces an architecture-
/// dependent bit pattern as a result.
///
/// This arbitrary 64-bit pattern is not necessarily convertable to f64
/// accurately, because a double only has 53 bits of precision. In such cases
/// the pattern is converted to the closest representable double. It may so
/// happen that the resulting double value is the same as the original value,
/// which caused the overflow! So, the comparison will compare equal, even
/// though the double is not representable as an integer. A false positive,
/// which we cannot allow!
///
/// To make it clear, let's go through the steps.
/// 1. Input value `d` is larger than int64_t.
/// 2. The conversion `int64_t i = (int64_t)d;` causes an overflow, and the
/// "overflow" bit pattern is stored in `i`.
/// 3. The pattern in `i` has more than 53 significant bits, so it is not
/// representable as a f64 value without loss of precision.
/// 4. The conversion `(double)i` converts it to the closest representable
/// f64, which unfortunately happens to be the original `d`.
/// 5. The comparison `(double)i == d` succeeds incorrectly!
///
/// To avoid this problem we modify the behavior of the hardware conversion.
/// Our implementation is guaranteed to always return a value suitable for
/// round-tripping, at the cost of false negatives.
///
/// We achieve this by narrowing the set of the convertable values, in
/// order to ensure that the returned value can always be compared correctly.
/// That is, we have to produce false negatives: some values which theoretically
/// are representable as integers will be treated as if they are not.
///
/// \section Implementation
///
/// It turns out that only some bit patterns are "problematic" and only for
/// positive numbers (we are assuming IEEE 754 and twos-complement, etc.).
///
/// Let's look at converting to u64 first. In order for the conversion back to
/// f64 to potentially produce a value that is not representable as u64, its
/// highest bit must be set (otherwise it would be too small).
///
/// For example 2**64-1 (0xFFFFFFFF_FFFFFFFF) is not accurately representable as
/// f64 and is rounded to the nearest double: 2*64, which is outside the u64
/// range. If the highest bit (2**63) is not set, the value is smaller than
/// 2**63 and the closest representable double will always be in range.
///
/// So, all we have to do is clear bit 63 of the u64 result. This guarantees
/// that the comparison has no false positives. Note that this works regardless
/// of the underlying CPU architecture.
///
/// Now let's examine i64. The bit pattern could be negative or positive.
///
/// Let's look at negative patterns first: the most negative 64-bit number is
/// -2**63 (0x80000000_00000000). It is representable as a double without loss
/// of precision, so it cannot cause overflow. Any other negative number would,
/// at worst, be converted to -2**63. So, all negative i64 patterns are "safe".
///
/// The most positive i64 value is 2**63-1 (0x7FFFFFFF_FFFFFFFF). It cannot be
/// represented exactly as f64, instead it is rounded to 2**63, which causes
/// overflow. Any other smaller values will, at worst, be rounded up to 2**63
/// as well. In order for a positive bit pattern to be "at risk", its bit 62
/// must be set, otherwise it would be too small.
///
/// How can we deal with that? A simple solution would be to mask out bit 62.
/// (This also causes false negatives for negative patterns with a 1 in bit 62).
/// Another solution is to do a left shift, followed by a signed right
/// shift, which is a single sbfx instruction on arm64. This turns "at risk"
/// positive patterns into negative integers. (Similarly, it causes false
/// negatives for negative patterns with 0 bit 62, by turning them into "safe"
/// positive patterns). Since negative numbers with 1 in bit 62 are common in
/// practice, we prefer the shift approach.
///
/// These are the actual bit patterns that we know are returned by different
/// hardware architectures:
/// - arm64 clamps the value to the largest/smallest representable value in the
/// integer: u64 -> [0..2**64-1]. i64 -> [-2**63..2**63-1].
/// - x86/x86-64 always sets the highest bit and clears all the rest of the
/// result.
///
/// Note that x86 doesn't need any mitigations because it always generates
/// -2**63, which is a safe pattern.
//===----------------------------------------------------------------------===//

#include <stdint.h>

/// A helper performing a fast possibly incorrect truncation from f64 to i32.
/// The truncation is always correct if the integer part of \p x is in range
/// of int32. Otherwise, an implementation-dependent value is returned (making
/// the truncation incorrect).
///
/// This function should only ever be used by comparing its result to its input
/// in order to determine \b both that the input is exactly representable as
/// i32 and that the operation succeeded.
///
/// This is a helper used by \c sh_tryfast_f64_to_i32() and should rarely be
/// used directly.
static int32_t _sh_tryfast_f64_to_i32_cvt(double x);

/// A helper performing a fast possibly incorrect truncation from f64 to u32.
/// The truncation is always correct if the integer part of \p x is in range
/// of uint32. Otherwise, an implementation-dependent value is returned (making
/// the truncation incorrect).
///
/// This function should only ever be used by comparing its result to its input
/// in order to determine \b both that the input is exactly representable as
/// u32 and that the operation succeeded.
///
/// This is a helper used by \c sh_tryfast_f64_to_u32() and should rarely be
/// used directly.
static uint32_t _sh_tryfast_f64_to_u32_cvt(double x);

/// A helper performing a fast possibly incorrect truncation from f64 to i64.
/// The truncation is incorrect if the integer part of \p x is not in range
/// of int64 \b or if other implementation-defined restrictions of the input
/// are not met. In other words, not all valid inputs can be converted.
///
/// This function should only ever be used by comparing its result to its input
/// in order to determine \b both that the input is exactly representable as
/// i64 and that the operation succeeded.
///
/// The define \c HERMES_TRYFAST_F64_TO_64_IS_FAST is set to 1, if this
/// has "good" performance on the architecture, 0 otherwise.
///
/// This is a helper used by \c sh_tryfast_f64_to_i64() and should rarely be
/// used directly.
static int64_t _sh_tryfast_f64_to_i64_cvt(double x);

/// A helper performing a fast possibly incorrect truncation from f64 to u64.
/// The truncation is incorrect if the integer part of \p x is not in range
/// of uint64 \b or if other implementation-defined restrictions of the input
/// are not met. In other words, not all valid inputs can be converted.
///
/// This function should only ever be used by comparing its result to its input
/// in order to determine \b both that the input is exactly representable as
/// i64 and that the operation succeeded.
///
/// The define \c HERMES_TRYFAST_F64_TO_64_IS_FAST is set to 1, if this
/// has "good" performance on the architecture, 0 otherwise.
///
/// This is a helper used by \c sh_tryfast_f64_to_u64() and should rarely be
/// used directly.
static uint64_t _sh_tryfast_f64_to_u64_cvt(double x);

#ifdef __cplusplus
/// Attempt to convert a f64 value to an i32 value, if the f64 is exactly
/// representable as i32.
///
/// \param x the input 64-bit fp value
/// \param intres the converted result on success, undefined value on failure.
/// \return \c true if \c x is representable as i32, \c false otherwise.
inline bool sh_tryfast_f64_to_i32(double x, int32_t &intres) {
  intres = _sh_tryfast_f64_to_i32_cvt(x);
  return (double)intres == x;
}

/// Attempt to convert a f64 value to an u32 value, if the f64 is exactly
/// representable as i32.
///
/// \param x the input 64-bit fp value
/// \param intres the converted result on success, undefined value on failure.
/// \return \c true if \c x is representable as u32, \c false otherwise.
inline bool sh_tryfast_f64_to_u32(double x, uint32_t &intres) {
  intres = _sh_tryfast_f64_to_u32_cvt(x);
  return (double)intres == x;
}

/// Attempt to convert a f64 value to an i64 value, if the f64 is exactly
/// representable as i64, \b and the f64 value satisfies other implementation-
/// dependent conditions for fast conversion (see top-level doc comment). This
/// is a "best effort" function - it works for most values, but fails for some.
/// If it fails, a slower algorithm has to be used (for example std::trunc()).
///
/// The define \c HERMES_TRYFAST_F64_TO_64_IS_FAST is set to 1, if this
/// has "good" performance on the architecture, 0 otherwise.
///
/// \param x the input 64-bit fp value
/// \param intres the converted result on success, undefined value on failure.
/// \return \c true if \c x can be represented as i64, \c false otherwise.
inline bool sh_tryfast_f64_to_i64(double x, int64_t &intres) {
  intres = _sh_tryfast_f64_to_i64_cvt(x);
  return (double)intres == x;
}

/// Attempt to convert a f64 value to an i64 value, if the f64 is exactly
/// representable as i64, \b and the f64 value satisfies other implementation-
/// dependent conditions for fast conversion (see top-level doc comment). This
/// is a "best effort" function - it works for most values, but fails for some.
/// If it fails, a slower algorithm has to be used (for example std::trunc()).
///
/// The define \c HERMES_TRYFAST_F64_TO_64_IS_FAST is set to 1, if this
/// has "good" performance on the architecture, 0 otherwise.
///
/// \param x the input 64-bit fp value
/// \param intres the converted result on success, undefined value on failure.
/// \return \c true if \c x can be represented as i64, \c false otherwise.
inline bool sh_tryfast_f64_to_u64(double x, uint64_t &intres) {
  intres = _sh_tryfast_f64_to_u64_cvt(x);
  return (double)intres == x;
}
#else
/// Attempt to convert a f64 value to an i32 value, if the f64 is exactly
/// representable as i32.
///
/// \param x the input 64-bit fp value
/// \param intres the converted result on success, undefined value on failure.
/// \return \c true if \c x is representable as i32, \c false otherwise.
#define sh_tryfast_f64_to_i32(x, intres) \
  ((intres) = _sh_tryfast_f64_to_i32_cvt(x), (double)(intres) == (x))

/// Attempt to convert a f64 value to an u32 value, if the f64 is exactly
/// representable as i32.
///
/// \param x the input 64-bit fp value
/// \param intres the converted result on success, undefined value on failure.
/// \return \c true if \c x is representable as u32, \c false otherwise.
#define sh_tryfast_f64_to_u32(x, intres) \
  ((intres) = _sh_tryfast_f64_to_u32_cvt(x), (double)(intres) == (x))

/// Attempt to convert a f64 value to an i64 value, if the f64 is exactly
/// representable as i64, \b and the f64 value satisfies other implementation-
/// dependent conditions for fast conversion (see top-level doc comment). This
/// is a "best effort" function - it works for most values, but fails for some.
/// If it fails, a slower algorithm has to be used (for example std::trunc()).
///
/// The define \c HERMES_TRYFAST_F64_TO_64_IS_FAST is set to 1, if this
/// has "good" performance on the architecture, 0 otherwise.
///
/// \param x the input 64-bit fp value
/// \param intres the converted result on success, undefined value on failure.
/// \return \c true if \c x can be represented as i64, \c false otherwise.
#define sh_tryfast_f64_to_i64(x, intres) \
  ((intres) = _sh_tryfast_f64_to_i64_cvt(x), (double)(intres) == (x))

/// Attempt to convert a f64 value to an i64 value, if the f64 is exactly
/// representable as i64, \b and the f64 value satisfies other implementation-
/// dependent conditions for fast conversion (see top-level doc comment). This
/// is a "best effort" function - it works for most values, but fails for some.
/// If it fails, a slower algorithm has to be used (for example std::trunc()).
///
/// The define \c HERMES_TRYFAST_F64_TO_64_IS_FAST is set to 1, if this
/// has "good" performance on the architecture, 0 otherwise.
///
/// \param x the input 64-bit fp value
/// \param intres the converted result on success, undefined value on failure.
/// \return \c true if \c x can be represented as i64, \c false otherwise.
#define sh_tryfast_f64_to_u64(x, intres) \
  ((intres) = _sh_tryfast_f64_to_u64_cvt(x), (double)(intres) == (x))
#endif

// TODO: is u64 really safe on x86?

#if defined(__GNUC__) && defined(__i386__)
// =============================================================================
//
// _sh_tryfast_f64_to_i64_cvt() for Clang/GCC x86(SSE2 or FP)
//

#define HERMES_TRYFAST_F64_TO_64_IS_FAST 1

static inline int64_t _sh_tryfast_f64_to_i64_cvt(double x) {
  int64_t result;
  unsigned short control_word, new_control_word;

  // Read the current FPU control word
  asm volatile("fnstcw %0" : "=m"(control_word));

  // Modify the control word to set rounding mode to truncation (round toward
  // zero). Bits 10,11 control the rounding, 0b11 means round toward zero.
  new_control_word = control_word | 0x0C00;

  // Load the new control word
  asm volatile("fldcw %0" : : "m"(new_control_word));

  // Perform the conversion
  asm volatile(
      "fldl %1\n\t" // Load the double value onto the FPU stack
      "fistpll %0" // Convert the top of the FPU stack to int (truncation)
      : "=m"(result) // Output: store result in memory
      : "m"(x) // Input: the double value in memory
      : "st");

  // Restore the original control word
  asm volatile("fldcw %0" : : "m"(control_word));

  return result;
}
#elif defined(_MSC_VER) && defined(_M_IX86)
// =============================================================================
//
// _sh_tryfast_f64_to_i64_cvt() for MSVC x86(SSE2 or FP)
//

#define HERMES_TRYFAST_F64_TO_64_IS_FAST 1

static inline __int64 _sh_tryfast_f64_to_i64_cvt(double x) {
  unsigned __int64 result;
  unsigned short control_word, new_control_word;

  // clang-format off
  __asm {
    // Save the current FPU control word
    fnstcw control_word

    // Modify control word: set rounding mode to truncation (11b)
    mov ax, control_word
    or  ax, 0x0C00 // Set bits 10 and 11 to 11 (truncate)
    mov new_control_word, ax

    // Load the modified control word
    fldcw new_control_word

    // Perform the conversion
    fld qword ptr [x] // Load the double value onto the FPU stack
    fistp qword ptr [result] // Convert the top FPU value to int (truncation)

    // Restore the original FPU control word
    fldcw control_word
  }
  // clang-format on

  return result;
}
#endif

#if defined(__GNUC__) && defined(__i386__) || \
    defined(_MSC_VER) && defined(_M_IX86)
// =============================================================================
//
// _sh_tryfast_f64_to_u64_cvt() for Clang/GCC/MSVC x86(SSE2 or FP)
//

static inline uint64_t _sh_tryfast_f64_to_u64_cvt(double x) {
  const double two63 = (double)((uint64_t)1 << 63);
  // Bit indicating whether conversion would overflow a positive signed int64.
  unsigned char ovf = 0;
  if (x >= two63) {
    ovf = 1;
    x -= two63;
  }
  return _sh_tryfast_f64_to_i64_cvt(x) | ((uint64_t)ovf << 63);
}
#endif

#if defined(__GNUC__) && defined(__arm__)
// =============================================================================
//
// _sh_tryfast_f64_to_i64_cvt()/_sh_tryfast_f64_to_u64_cvt() Clang/GCC AArch32
// TODO: Unfortunately we don't have fully UB-safe implementations of these.

#define HERMES_TRYFAST_F64_TO_64_IS_FAST 0

#if defined(__has_attribute) && __has_attribute(no_sanitize)
static inline int64_t _sh_tryfast_f64_to_i64_cvt(double x)
    __attribute__((no_sanitize("float-cast-overflow")));
static inline uint64_t _sh_tryfast_f64_to_u64_cvt(double x)
    __attribute__((no_sanitize("float-cast-overflow")));
#endif

static inline int64_t _sh_tryfast_f64_to_i64_cvt(double x) {
  // Note: this eliminates all unsafe positive bit patterns, but also makes
  // it very unlikely that the compiler can exploit the UB here.
  return (((int64_t)x) << 1) >> 1;
}

static inline uint64_t _sh_tryfast_f64_to_u64_cvt(double x) {
  // Note: this eliminates all unsafe positive bit patterns, but also makes
  // it very unlikely that the compiler can exploit the UB here.
  return (uint64_t)x & ~(1ull << 63);
}

#endif

#if (defined(__GNUC__) && defined(__x86_64__)) || \
    (defined(_MSC_VER) && defined(_M_X64))
// =============================================================================
//
// Clang/GCC/MSVC x86-64
//

#include <emmintrin.h>

static inline int _sh_tryfast_f64_to_i32_cvt(double x) {
  return _mm_cvttsd_si32(_mm_set_sd(x));
}
static inline unsigned _sh_tryfast_f64_to_u32_cvt(double x) {
  return _mm_cvttsd_si64(_mm_set_sd(x));
}

#define HERMES_TRYFAST_F64_TO_64_IS_FAST 1

static inline int64_t _sh_tryfast_f64_to_i64_cvt(double x) {
  return _mm_cvttsd_si64(_mm_set_sd(x));
}
static inline uint64_t _sh_tryfast_f64_to_u64_cvt(double x) {
  __m128d val = _mm_set_sd(x);
  // The truncated x, or 2**63 on overflow.
  int64_t truncOrTwo63 = _mm_cvttsd_si64(val);
  // The truncated (x - 2**63)
  int64_t truncMinusTwo63 =
      _mm_cvttsd_si64(_mm_sub_sd(val, _mm_set_sd(1ull << 63)));
  // If truncOrTwo63 overflowed, (truncMinusTwo63 | 2**63), otherwise
  // truncOrTwo63
  return (truncMinusTwo63 & (truncOrTwo63 >> 63)) | truncOrTwo63;
}

#elif (defined(__GNUC__) && defined(__i386__) && defined(__SSE2__)) || \
    (defined(_MSC_VER) && defined(_M_IX86) && defined(_M_IX86_FP) &&   \
     _M_IX86_FP >= 2)
// =============================================================================
//
// Clang/GCC/MSVC x86+SSE2
//

#include <emmintrin.h>

static inline int _sh_tryfast_f64_to_i32_cvt(double x) {
  return _mm_cvttsd_si32(_mm_set_sd(x));
}

static inline unsigned _sh_tryfast_f64_to_u32_cvt(double x) {
  __m128d val = _mm_set_sd(x);
  // The truncated x, or 2**31 on overflow.
  int truncOrTwo31 = _mm_cvttsd_si32(val);
  // The truncated (x - 2**31)
  int truncMinusTwo31 = _mm_cvttsd_si32(_mm_sub_sd(val, _mm_set_sd(1u << 31)));
  // If truncOrTwo31 overflowed, (truncMinusTwo31 | 2**31), otherwise
  // truncOrTwo31
  return (truncMinusTwo31 & (truncOrTwo31 >> 31)) | truncOrTwo31;
}

#elif defined(__GNUC__) && defined(__i386__)
// =============================================================================
//
// Clang/GCC x86+FP
//

static inline int _sh_tryfast_f64_to_i32_cvt(double x) {
  int result;
  unsigned short control_word, new_control_word;

  // Read the current FPU control word
  asm volatile("fnstcw %0" : "=m"(control_word));

  // Modify the control word to set rounding mode to truncation (round toward
  // zero). Bits 10,11 control the rounding, 0b11 means round toward zero.
  new_control_word = control_word | 0x0C00;

  // Load the new control word
  asm volatile("fldcw %0" : : "m"(new_control_word));

  // Perform the conversion
  asm volatile(
      "fldl %1\n\t" // Load the double value onto the FPU stack
      "fistpl %0" // Convert the top of the FPU stack to int (truncation)
      : "=m"(result) // Output: store result in memory
      : "m"(x) // Input: the double value in memory
      : "st");

  // Restore the original control word
  asm volatile("fldcw %0" : : "m"(control_word));

  return result;
}

static inline unsigned _sh_tryfast_f64_to_u32_cvt(double x) {
  return (unsigned)_sh_tryfast_f64_to_i64_cvt(x);
}

#elif defined(_MSC_VER) && defined(_M_IX86)
// =============================================================================
//
// MSVC x86+FP
//

static inline int _sh_tryfast_f64_to_i32_cvt(double x) {
  int result;
  unsigned short control_word, new_control_word;

  // clang-format off
  __asm {
    // Save the current FPU control word
    fnstcw control_word

    // Modify control word: set rounding mode to truncation (11b)
    mov ax, control_word
    or  ax, 0x0C00 // Set bits 10 and 11 to 11 (truncate)
    mov new_control_word, ax

    // Load the modified control word
    fldcw new_control_word

    // Perform the conversion
    fld qword ptr [x] // Load the double value onto the FPU stack
    fistp dword ptr [result] // Convert the top FPU value to int (truncation)

    // Restore the original FPU control word
    fldcw control_word
  }
  // clang-format on

  return result;
}

static inline unsigned _sh_tryfast_f64_to_u32_cvt(double x) {
  return (unsigned)_sh_tryfast_f64_to_i64_cvt(x);
}

#elif (defined(__GNUC__) && defined(__aarch64__)) || \
    (defined(_MSC_VER) && defined(_M_ARM64))
// =============================================================================
//
// Clang/GCC/MSVC aarch64
//

#include <arm_neon.h>

#define HERMES_TRYFAST_F64_TO_64_IS_FAST 1

#ifdef __clang__
/*
 NOTE: the intrinsics work in Clang, but generate an extra asm instruction.
        fcvtzs	d0, d0
        fmov	w0, s0

 GCC produces what we expect. Not sure if it makes a difference.
 */

static inline int _sh_tryfast_f64_to_i32_cvt(double x) {
  int result;
  asm("fcvtzs %w0, %d1" // Convert double to int32
      : "=r"(result) // Output: result in a general-purpose register
      : "w"(x) // Input: double in a floating-point register
  );
  return result;
}
static inline unsigned _sh_tryfast_f64_to_u32_cvt(double x) {
  unsigned result;
  asm("fcvtzu %w0, %d1" // Convert double to uint32
      : "=r"(result) // Output: result in a general-purpose register
      : "w"(x) // Input: double in a floating-point register
  );
  return result;
}
static inline int64_t _sh_tryfast_f64_to_i64_cvt(double x) {
  int64_t result;
  asm("fcvtzs %x0, %d1" // Convert double to int32
      : "=r"(result) // Output: result in a general-purpose register
      : "w"(x) // Input: double in a floating-point register
  );
  // See file doc-comment for details on round-tripping mitigations.
  return (result << 1) >> 1;
}
static inline uint64_t _sh_tryfast_f64_to_u64_cvt(double x) {
  uint64_t result;
  asm("fcvtzu %x0, %d1" // Convert double to uint32
      : "=r"(result) // Output: result in a general-purpose register
      : "w"(x) // Input: double in a floating-point register
  );
  // See file doc-comment for details on round-tripping mitigations.
  return result & ~(1ull << 63);
}
#else
static inline int _sh_tryfast_f64_to_i32_cvt(double x) {
  return (int)vget_lane_s64(vcvt_s64_f64(vdup_n_f64(x)), 0);
}
static inline unsigned _sh_tryfast_f64_to_u32_cvt(double x) {
  return (unsigned)vget_lane_u64(vcvt_u64_f64(vdup_n_f64(x)), 0);
}
static inline int64_t _sh_tryfast_f64_to_i64_cvt(double x) {
  int64_t result = vget_lane_s64(vcvt_s64_f64(vdup_n_f64(x)), 0);
  // See file doc-comment for details on round-tripping mitigations.
  return (result << 1) >> 1;
}
static inline uint64_t _sh_tryfast_f64_to_u64_cvt(double x) {
  uint64_t result = vget_lane_u64(vcvt_u64_f64(vdup_n_f64(x)), 0);
  // See file doc-comment for details on round-tripping mitigations.
  return result & ~(1ull << 63);
}
#endif

#elif defined(__GNUC__) && defined(__arm__) && defined(__VFP_FP__)
// =============================================================================
//
// Clang/GCC AArch32 (armv7-a and later, incl. 32-bit armv8-a)
//

static inline int _sh_tryfast_f64_to_i32_cvt(double x) {
  int result;
  float temp __attribute__((unused));
  asm("vcvt.s32.f64 %1, %P2\n\t" // Convert double to int32 with truncation
      "vmov %0, %1" // Move the lower part of d0 (s0) to the result
      : "=r"(result), "=t"(temp)
      : "w"(x) // Input: the double value
  );
  return result;
}
static inline unsigned _sh_tryfast_f64_to_u32_cvt(double x) {
  unsigned result;
  float temp __attribute__((unused));
  asm("vcvt.u32.f64 %1, %P2\n\t" // Convert double to uint32 with truncation
      "vmov %0, %1" // Move the lower part of d0 (s0) to the result
      : "=r"(result), "=t"(temp)
      : "w"(x) // Input: the double value
  );
  return result;
}

#elif defined(__wasm__)
// =============================================================================
//
// Wasm
//
// A C cast is undefined for out-of-range values even if the final Wasm
// instruction saturates. LLVM can consequently remove the range check from
// the conversion's round trip, accepting negative inputs as unsigned zero.
// Use defined saturating intrinsics when the target supports them, and guard
// the casts otherwise, so that the round-trip check is preserved.

#define HERMES_TRYFAST_F64_TO_64_IS_FAST 1

#if defined(__wasm_nontrapping_fptoint__)

static inline int32_t _sh_tryfast_f64_to_i32_cvt(double x) {
  return __builtin_wasm_trunc_saturate_s_i32_f64(x);
}
static inline uint32_t _sh_tryfast_f64_to_u32_cvt(double x) {
  return __builtin_wasm_trunc_saturate_u_i32_f64(x);
}

static inline int64_t _sh_tryfast_f64_to_i64_cvt(double x) {
  int64_t result = __builtin_wasm_trunc_saturate_s_i64_f64(x);
  // Reject saturated values that round back to the overflowing input double.
  // See the file doc-comment for the same mitigation on other architectures.
  return (int64_t)((uint64_t)result << 1) >> 1;
}
static inline uint64_t _sh_tryfast_f64_to_u64_cvt(double x) {
  return __builtin_wasm_trunc_saturate_u_i64_f64(x) & ~(1ull << 63);
}

#else

// Ordered comparisons reject NaN as well as infinities before any C cast.
// Check the range of the truncated value, including fractions just below the
// minimum integer and just above the maximum integer.
static inline int32_t _sh_tryfast_f64_to_i32_cvt(double x) {
  return x > -2147483649.0 && x < 2147483648.0 ? (int32_t)x : 0;
}
static inline uint32_t _sh_tryfast_f64_to_u32_cvt(double x) {
  return x > -1.0 && x < 4294967296.0 ? (uint32_t)x : 0;
}

// INT64_MAX and UINT64_MAX round up to powers of two as doubles. Exclusive
// upper bounds reject those overflowing values before conversion, so these
// results do not need the saturated-result mitigation above.
static inline int64_t _sh_tryfast_f64_to_i64_cvt(double x) {
  return x >= -9223372036854775808.0 && x < 9223372036854775808.0 ? (int64_t)x
                                                                  : 0;
}
static inline uint64_t _sh_tryfast_f64_to_u64_cvt(double x) {
  return x > -1.0 && x < 18446744073709551616.0 ? (uint64_t)x : 0;
}

#endif

#else
// =============================================================================
//
// Unsupported compiler/platform
//

#ifdef HERMES_TRYFAST_FP_CVT_ENABLE_UNSUPPORTED

static inline int32_t _sh_tryfast_f64_to_i32_cvt(double x) {
  return x < INT32_MIN ? 0 : x > INT32_MAX ? 0 : (int32_t)x;
}
static inline uint32_t _sh_tryfast_f64_to_u32_cvt(double x) {
  return x < 0 ? 0 : x > UINT32_MAX ? 0 : (uint32_t)x;
}

#define HERMES_TRYFAST_F64_TO_64_IS_FAST 0

static inline int64_t _sh_tryfast_f64_to_i64_cvt(double x) {
  return x < INT64_MIN ? 0 : x > INT64_MAX ? 0 : (int64_t)x;
}
static inline uint64_t _sh_tryfast_f64_to_u64_cvt(double x) {
  return x < 0 ? 0 : x > UINT64_MAX ? 0 : (uint64_t)x;
}

#else
#error Unsupported compiler platform for _sh_tryfast_f64
#endif

#endif
