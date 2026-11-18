/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SUPPORT_CONVERSIONS_H
#define HERMES_SUPPORT_CONVERSIONS_H

#include "hermes/Support/FastStrToDouble.h"
#include "hermes/Support/OptValue.h"
#include "hermes/Support/sh_tryfast_fp_cvt.h"

#include "llvh/ADT/ArrayRef.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/ADT/StringRef.h"
#include "llvh/Support/Compiler.h"
#include "llvh/Support/MathExtras.h"

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace hermes {

/// Helper function to silence UBSAN complaints when truncating a double to
/// float, even though that is not really UB, since the behavior is governed
/// by IEEE 754.
float truncDoubleToFloat(double d) LLVM_NO_SANITIZE("float-cast-overflow");
inline float truncDoubleToFloat(double d) {
  return (float)d;
}

/// Convert a double to a 32-bit integer according to ES5.1 section 9.5.
/// It can also be used for converting to an unsigned integer, which has the
/// same bit pattern.
/// NaN and Infinity are always converted to 0. The rest of the numbers are
/// converted to a (conceptually) infinite-width integer and the low 32 bits of
/// the integer are then returned.
/// This is the out-of-line "slow path" which performs a relatively slow
/// conversion by manipulating bits in the representation of the double value.
int32_t truncateToInt32SlowPath(double d);

/// Convert a double to a 32-bit integer according to ES5.1 section 9.5.
/// It can also be used for converting to an unsigned integer, which has the
/// same bit pattern.
/// NaN and Infinity are always converted to 0. The rest of the numbers are
/// converted to a (conceptually) infinite-width integer and the low 32 bits of
/// the integer are then returned.
inline int32_t truncateToInt32(double d) {
  // If we are compiling with ARM v8.3 or above, there is a special instruction
  // to do the conversion.
#ifdef __ARM_FEATURE_JCVT
  return __builtin_arm_jcvt(d);
#endif

  // NOTE: this implementation should be consistent with _sh_to_int32_double()
  // in VM/static_h.h

  // Use HERMES_BUILTIN_CONSTANT_P for better perf and to avoid UB caused by
  // constant propagation.
  if (HERMES_BUILTIN_CONSTANT_P(d)) {
    // Be aggressive on constant path, use the maximum precision bits
    // of double type for range check.
    if (d >= (int64_t)(-1ULL << 53) && d <= (1LL << 53))
      return (int32_t)(int64_t)d;
    return truncateToInt32SlowPath(d);
  }

  if (HERMES_TRYFAST_F64_TO_64_IS_FAST) {
    int64_t fast;
    if (LLVM_LIKELY(sh_tryfast_f64_to_i64(d, fast)))
      return (int32_t)fast;
  } else {
    int32_t fast;
    if (LLVM_LIKELY(sh_tryfast_f64_to_i32(d, fast)))
      return fast;
  }

  return truncateToInt32SlowPath(d);
}

inline uint32_t truncateToUInt32(double d) {
  return (uint32_t)truncateToInt32(d);
}

/// Convert a string in the range defined by [first, last) to an array index
/// following ES5.1 15.4:
/// "A property name P (in the form of a String value) is an array index if and
/// only if ToString(ToUint32(P)) is equal to P and ToUint32(P) is not equal to
/// 2**32−1."
/// IT must meet the requirements of InputIterator and its 'value_type' must be
/// an integral type holding a superset of ASCII, typically char, char16_t,
/// char32_t or wchar_t.
/// \return an "empty" OptValue if the string isn't a valid index, or the
///   converted uint32_t value otherwise.
template <typename IT>
OptValue<uint32_t> toArrayIndex(IT first, IT last) {
  /// Empty string is invalid.
  if (first == last)
    return llvh::None;

  // Leading 0 is special.
  if (*first == '0') {
    ++first;
    // Just "0"?
    if (first == last)
      return 0;
    // Leading 0 is invalid otherwise.
    return llvh::None;
  }

  uint32_t res = 0;
  do {
    auto ch = *first;
    if (ch < '0' || ch > '9')
      return llvh::None;
    uint64_t tmp = (uint64_t)res * 10 + (ch - '0');
    // Check for overflow.
    if (tmp & ((uint64_t)0xFFFFFFFFu << 32))
      return llvh::None;
    res = (uint32_t)tmp;
  } while (++first != last);

  // 0xFFFFFFFF is not a valid array index.
  if (res == 0xFFFFFFFFu)
    return llvh::None;

  return res;
}

/// A convenient wrapper around 'toArrayIndex(first,last)'.
inline OptValue<uint32_t> toArrayIndex(llvh::StringRef str) {
  return toArrayIndex(str.begin(), str.end());
}

/// Attempt to convert a double to a valid JavaScript array number.
inline OptValue<uint32_t> doubleToArrayIndex(double d) {
  uint32_t index;
  if (sh_tryfast_f64_to_u32(d, index) && index != 0xFFFFFFFFu)
    return index;
  return llvh::None;
}

/// Convert a IEEE 754 half-precision float16 (stored as uint16_t) to double.
/// NaN inputs always produce a quiet NaN output (the quiet bit is preserved
/// or set).
double float16ToDouble(uint16_t bits);

/// Convert a double to IEEE 754 half-precision float16 (stored as uint16_t).
/// Uses round-to-nearest-even as required by the ECMAScript spec.
/// NaN inputs always produce a quiet NaN.
uint16_t doubleToFloat16(double d);

/// Size of buffer that must be passed to numberToString.
const size_t NUMBER_TO_STRING_BUF_SIZE = 32;

/// The slow path of numberToString(): the full ES5.1 9.8.1 conversion, with no
/// integer fast path. Prefer numberToString() unless you specifically need the
/// two extra guarantees this makes: the result always starts at \p dest, and
/// \p dest is null terminated just past it.
/// \param m the number to convert
/// \param dest output buffer
/// \param destSize size of dest, at least NUMBER_TO_STRING_BUF_SIZE
/// \return the string, starting at \p dest.
llvh::StringRef numberToStringSlowPath(double m, char *dest, size_t destSize);

/// Convert an unsigned integer \p value to its decimal string representation,
/// not null terminated. Least significant digit will be written to the last
/// position in \p buf.
/// \param value is the value to be converted.
/// \param buf is a pre-allocated buffer to hold the string contents. Must be
///   at least 20 characters to hold the maximum uint64_t value.
/// \return the digits, pointing into \p buf.
template <typename T>
llvh::StringRef uintToStr(T value, llvh::MutableArrayRef<char> buf) {
  static_assert(std::is_integral<T>::value, "T must be integral");
  static_assert(std::is_unsigned<T>::value, "T must be unsigned");
  // uint64_t max number is 20 digits.
  assert(buf.size() >= 20 && "buffer must hold at least 20 characters");
  char *bufEnd = buf.end() - 1;
  if (value == 0) {
    *bufEnd = '0';
    return llvh::StringRef(bufEnd, 1);
  }
  // Extract digits least-significant first.
  for (; value > 0; value /= 10, --bufEnd) {
    T digit = value % 10;
    *bufEnd = (char)('0' + digit);
  }
  return llvh::StringRef(bufEnd + 1, buf.end() - (bufEnd + 1));
}

/// If \p m is a positive integer representable as int32_t, write its decimal
/// digits into \p buf. This is by far the most common input in JS, so it is
/// worth skipping the general conversion machinery entirely.
///
/// NOTE: the "> 0" test is what excludes 0 and -0 from this path:
/// sh_tryfast_f64_to_i32 succeeds for both (since 0.0 == -0.0) and yields 0.
/// NaN and +-Infinity are rejected by the round-trip check inside
/// sh_tryfast_f64_to_i32 itself.
///
/// \param m the number to convert.
/// \param buf scratch buffer, at least NUMBER_TO_STRING_BUF_SIZE characters.
/// \param bufSize size of \p buf.
/// \return the digits, pointing into \p buf. The result is NOT null
///   terminated and does NOT necessarily start at buf[0]. Returns None if
///   \p m does not qualify for the fast path.
inline OptValue<llvh::StringRef>
tryFastPositiveIntToString(double m, char *buf, size_t bufSize) {
  assert(bufSize >= NUMBER_TO_STRING_BUF_SIZE && "buffer is too small");
  int32_t n;
  if (LLVM_LIKELY(sh_tryfast_f64_to_i32(m, n)) && n > 0)
    return uintToStr((uint32_t)n, llvh::MutableArrayRef<char>(buf, bufSize));
  return llvh::None;
}

/// Convert a double number to string, following ES5.1 9.8.1. Takes the positive
/// integer fast path when possible, and otherwise falls back to
/// numberToStringSlowPath().
/// \param m the number to convert.
/// \param buf scratch buffer, at least NUMBER_TO_STRING_BUF_SIZE characters.
/// \param bufSize size of \p buf.
/// \return the string, pointing into \p buf. The result is NOT null
///   terminated and does NOT necessarily start at buf[0].
inline llvh::StringRef numberToString(double m, char *buf, size_t bufSize) {
  if (auto fast = tryFastPositiveIntToString(m, buf, bufSize))
    return *fast;
  return numberToStringSlowPath(m, buf, bufSize);
}

/// Takes a letter (a-z or A-Z) and makes it lowercase.
template <typename T>
inline T charLetterToLower(T ch) {
  return ch | 32;
}

/// Takes a non-empty string (without the leading "0x" if hex) and parses it
/// as radix \p radix, calling \p digitCallback with the value of each digit,
/// going from left to right.
/// \param AllowNumericSeparator when true, allow '_' as a separator and ignore
/// it when parsing.
/// \returns true if the string was successfully parsed, false otherwise.
template <bool AllowNumericSeparator, class Iterable, typename Callback>
bool parseIntWithRadixDigits(Iterable str, int radix, Callback digitCallback) {
  assert(
      radix >= 2 && radix <= 36 && "Invalid radix passed to parseIntWithRadix");
  assert(str.begin() != str.end() && "Empty string");
  for (auto it = str.begin(); it != str.end(); ++it) {
    auto c = *it;
    auto cLow = charLetterToLower(c);
    if ('0' <= c && c <= '9' && c < '0' + radix) {
      digitCallback(c - '0');
    } else if ('a' <= cLow && cLow < 'a' + radix - 10) {
      digitCallback(cLow - 'a' + 0xa);
    } else if (AllowNumericSeparator && LLVM_UNLIKELY(c == '_')) {
      // Ensure the '_' is in a valid location.
      // It can only be between two existing digits.
      if (it == str.begin() || it == str.end() - 1) {
        return false;
      }
      // Note that the previous character must not be '_' if the current
      // character is '_', because we would have returned None.
      // So just check if the next character is '_'.
      char next = *(it + 1);
      if (next == '_') {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

/// Takes a non-empty string (without the leading "0x" if hex) and parses it
/// as radix \p radix.
/// \param AllowNumericSeparator when true, allow '_' as a separator and ignore
///    it when parsing.
/// \returns the double that results on success, empty on error.
template <bool AllowNumericSeparator, class Iterable>
OptValue<double> parseIntWithRadix(Iterable str, int radix) {
  double result = 0;
  bool success = parseIntWithRadixDigits<AllowNumericSeparator>(
      str, radix, [&result, radix](uint8_t d) {
        result *= radix;
        result += d;
      });
  if (!success)
    return llvh::None;

  // The largest value that fits in the 53-bit mantissa (2**53).
  const double MAX_MANTISSA = 9007199254740992.0;
  if (result >= MAX_MANTISSA && radix == 10) {
    // Above 2**53 the loop above rounds after every digit, but radix 10 may
    // only zero digits after the 20th (ES2026 19.2.5, step 14), so round
    // the exact value once with a correctly rounded decimal parser.
    llvh::SmallVector<char, 32> digits;
    for (auto c : str) {
      if (!AllowNumericSeparator || c != '_')
        digits.push_back(static_cast<char>(c));
    }
    return fastStrToDouble(llvh::ArrayRef<char>{digits}).value;
  }
  if (result >= MAX_MANTISSA && llvh::isPowerOf2_32(radix)) {
    // If the result is too high, manually reconstruct the double if
    // the radix is 2, 4, 8, 16, 32.
    // Go through the digits bit by bit, and manually round when necessary.
    result = 0;

    // Keep track of how far along parsing is using this enum.
    enum Mode {
      LEADING_ZERO, // Haven't seen a set bit yet.
      MANTISSA, // Lower bits that allow exact representation.
      EXP_LOW_BIT, // Lowest bit of the exponent (determine rounding).
      EXP_LEADING_ZERO, // Zeros in the exponent.
      EXPONENT, // Seen a set bit in the exponent.
    };

    size_t remainingMantissa = 53;
    double expFactor = 0.0;
    size_t curDigit = 0;

    bool lastMantissaBit = false;
    bool lowestExponentBit = false;

    Mode curMode = Mode::LEADING_ZERO;
    auto itr = str.begin();
    auto e = str.end();
    for (size_t bitMask = 0;;) {
      if (bitMask == 0) {
        // Only need to do this check every log2(radix) iterations.
        if (itr == e) {
          break;
        }
        // We know it fits in 7 bits after the first pass.
        char c = (char)*itr;
        if (AllowNumericSeparator && LLVM_UNLIKELY(c == '_')) {
          ++itr;
          continue;
        }
        auto cLow = charLetterToLower(c);
        if ('0' <= c && c <= '9') {
          curDigit = c - '0';
        } else {
          // Must be valid, else we would have returned NaN on first pass.
          assert('a' <= cLow && cLow < 'a' + radix - 10);
          curDigit = cLow - 'a' + 0xa;
        }
        ++itr;
        // Reset bitmask to look at the first bit.
        bitMask = radix >> 1;
      }
      bool curBit = (curDigit & bitMask) != 0;
      bitMask >>= 1;

      switch (curMode) {
        case Mode::LEADING_ZERO:
          // Go through the string until we hit a nonzero bit.
          if (curBit) {
            --remainingMantissa;
            result = 1;
            // No more leading zeros.
            curMode = Mode::MANTISSA;
          }
          break;
        case Mode::MANTISSA:
          // Read into the lower bits of the mantissa (plain binary).
          result *= 2;
          result += curBit;
          --remainingMantissa;
          if (remainingMantissa == 0) {
            // Out of bits, set the last bit and go to the next curMode.
            lastMantissaBit = curBit;
            curMode = Mode::EXP_LOW_BIT;
          }
          break;
        case Mode::EXP_LOW_BIT:
          lowestExponentBit = curBit;
          expFactor = 2.0;
          curMode = Mode::EXP_LEADING_ZERO;
          break;
        case Mode::EXP_LEADING_ZERO:
          if (curBit) {
            curMode = Mode::EXPONENT;
          }
          expFactor *= 2.0;
          break;
        case Mode::EXPONENT:
          expFactor *= 2.0;
          break;
      }
    }
    switch (curMode) {
      case Mode::LEADING_ZERO:
      case Mode::MANTISSA:
      case Mode::EXP_LOW_BIT:
        // Nothing to do here, already read those in.
        break;
      case Mode::EXP_LEADING_ZERO:
        // Rounding up.
        result += lowestExponentBit && lastMantissaBit;
        result *= expFactor;
        break;
      case Mode::EXPONENT:
        // Rounding up.
        result += lowestExponentBit;
        result *= expFactor;
        break;
    }
  }
  return result;
}

} // namespace hermes

extern "C" {
size_t hermes_numberToString(double m, char *dest, size_t destSize);
}

#endif // HERMES_SUPPORT_CONVERSIONS_H
