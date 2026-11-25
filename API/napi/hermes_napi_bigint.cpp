/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/VM/BigIntPrimitive.h"

#include "hermes/Support/BigIntSupport.h"

#include <cstring>
#include <vector>

//===========================================================================
// BigInt creation
//===========================================================================

napi_status NAPI_CDECL
napi_create_bigint_int64(napi_env env, int64_t value, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  auto bigintRes = BigIntPrimitive::fromSigned(env->runtime, value);
  if (LLVM_UNLIKELY(bigintRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(*bigintRes);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  auto bigintRes = BigIntPrimitive::fromUnsigned(env->runtime, value);
  if (LLVM_UNLIKELY(bigintRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(*bigintRes);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_bigint_words(
    napi_env env,
    int sign_bit,
    size_t word_count,
    const uint64_t *words,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, words);
  CHECK_ARG(env, result);

  RETURN_STATUS_IF_FALSE(env, word_count <= INT_MAX, napi_invalid_arg);

  using namespace hermes::vm;

  // Check if the requested word count exceeds the maximum BigInt size.
  // NAPI words are uint64_t, same size as Hermes BigInt digits. We may
  // need one extra digit for sign extension. If the total exceeds the
  // Hermes limit, throw a RangeError (matching V8 behavior).
  if (word_count > 0 &&
      word_count + 1 > hermes::bigint::BigIntMaxSizeInDigits) {
    (void)env->runtime.raiseRangeError("Maximum BigInt size exceeded");
    return captureRuntimeException(env, napi_pending_exception);
  }

  if (word_count == 0) {
    // Zero value: create a BigInt from a zero byte.
    uint8_t zero = 0;
    auto bigintRes =
        BigIntPrimitive::fromBytes(env->runtime, llvh::makeArrayRef(&zero, 1));
    if (LLVM_UNLIKELY(bigintRes == ExecutionStatus::EXCEPTION)) {
      return captureRuntimeException(env, napi_generic_failure);
    }
    *result = env->addToCurrentScope(*bigintRes);
    return napi_clear_last_error(env);
  }

  // NAPI uses sign-magnitude representation: an array of uint64_t words
  // (little-endian digit order) plus a sign bit. Hermes uses two's
  // complement little-endian bytes. We need to convert.
  //
  // For positive numbers (sign_bit == 0): the magnitude words are the
  // two's complement representation, but we need to add a leading zero
  // byte if the MSB of the last word is set (to prevent it being
  // interpreted as negative).
  //
  // For negative numbers (sign_bit != 0): we need to negate the
  // magnitude to get two's complement. We do this by creating the
  // positive value first, then negating it with BigIntPrimitive::unaryMinus.

  // Build the magnitude bytes (little-endian).
  size_t byteCount = word_count * sizeof(uint64_t);

  // For positive values, we may need an extra zero byte to ensure the
  // value is interpreted as non-negative in two's complement.
  bool needExtraZero = false;
  if (sign_bit == 0) {
    uint64_t msWord = words[word_count - 1];
    if (msWord & (uint64_t{1} << 63)) {
      needExtraZero = true;
    }
  }

  // For negative values, add an extra zero byte so the magnitude is
  // positive in two's complement before we negate.
  if (sign_bit != 0) {
    // Check if the MSB is set — if so, we need an extra byte.
    uint64_t msWord = words[word_count - 1];
    if (msWord & (uint64_t{1} << 63)) {
      needExtraZero = true;
    }
  }

  size_t totalBytes = byteCount + (needExtraZero ? sizeof(uint64_t) : 0);
  std::vector<uint8_t> bytes(totalBytes, 0);
  std::memcpy(bytes.data(), words, byteCount);
  // Extra bytes are already zero-initialized.

  struct : public Locals {
    PinnedValue<BigIntPrimitive> bigint;
  } lv;
  LocalsRAII lraii(env->runtime, &lv);

  auto bigintRes = BigIntPrimitive::fromBytes(
      env->runtime, llvh::makeArrayRef(bytes.data(), totalBytes));
  if (LLVM_UNLIKELY(bigintRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_generic_failure);
  }

  if (sign_bit != 0) {
    // Check if the magnitude is zero — -0 should be 0.
    bool isZero = true;
    for (size_t i = 0; i < word_count; ++i) {
      if (words[i] != 0) {
        isZero = false;
        break;
      }
    }
    if (!isZero) {
      // Negate the positive magnitude to get two's complement negative.
      lv.bigint.castAndSetHermesValue<BigIntPrimitive>(*bigintRes);
      auto negRes = BigIntPrimitive::unaryMinus(env->runtime, lv.bigint);
      if (LLVM_UNLIKELY(negRes == ExecutionStatus::EXCEPTION)) {
        return captureRuntimeException(env, napi_generic_failure);
      }
      *result = env->addToCurrentScope(*negRes);
      return napi_clear_last_error(env);
    }
  }

  *result = env->addToCurrentScope(*bigintRes);
  return napi_clear_last_error(env);
}

//===========================================================================
// BigInt extraction
//===========================================================================

napi_status NAPI_CDECL napi_get_value_bigint_int64(
    napi_env env,
    napi_value value,
    int64_t *result,
    bool *lossless) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  CHECK_ARG(env, lossless);

  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isBigInt(), napi_bigint_expected);

  auto *bi = vmcast<BigIntPrimitive>(*phv);

  // truncateToSingleDigit returns the first 64 bits (or 0 if no digits).
  // For signed extraction, interpret as int64_t.
  uint64_t digit = bi->truncateToSingleDigit();
  *result = static_cast<int64_t>(digit);

  // Check if the truncation is lossless for signed interpretation.
  *lossless = bi->isTruncationToSingleDigitLossless(
      /*signedTruncation=*/true);

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_bigint_uint64(
    napi_env env,
    napi_value value,
    uint64_t *result,
    bool *lossless) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  CHECK_ARG(env, lossless);

  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isBigInt(), napi_bigint_expected);

  auto *bi = vmcast<BigIntPrimitive>(*phv);

  // truncateToSingleDigit returns the first 64 bits (or 0 if no digits).
  *result = bi->truncateToSingleDigit();

  // Check if the truncation is lossless for unsigned interpretation.
  *lossless = bi->isTruncationToSingleDigitLossless(
      /*signedTruncation=*/false);

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_bigint_words(
    napi_env env,
    napi_value value,
    int *sign_bit,
    size_t *word_count,
    uint64_t *words) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, word_count);

  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isBigInt(), napi_bigint_expected);

  auto *bi = vmcast<BigIntPrimitive>(*phv);

  // Hermes stores BigInts in two's complement. NAPI expects sign-magnitude.
  // We need to convert.
  bool isNeg = bi->sign();

  if (sign_bit == nullptr && words == nullptr) {
    // Query mode: just return the word count needed.
    // The number of magnitude words needed. Hermes internally stores
    // digits that may include sign-extension, so we use the compact
    // representation to determine the magnitude size.
    if (!isNeg) {
      // Positive: compact bytes give us the magnitude directly.
      auto compact = bi->getRawDataCompact();
      // Number of 64-bit words needed to hold the compact bytes,
      // rounded up.
      size_t needed =
          (compact.size() + sizeof(uint64_t) - 1) / sizeof(uint64_t);
      // If the compact representation includes a sign-extension zero
      // that pushes us into an extra word, that's fine — the extra word
      // will just be zero.
      *word_count = needed;
    } else {
      // Negative: we need to negate to get the magnitude. The number of
      // magnitude words is at most the number of digits in the original.
      // Use the full digits count as an upper bound.
      auto digits = bi->getDigits();
      *word_count = digits.size();
    }
    return napi_clear_last_error(env);
  }

  CHECK_ARG(env, sign_bit);
  CHECK_ARG(env, words);

  *sign_bit = isNeg ? 1 : 0;

  if (!isNeg) {
    // Positive (or zero): the two's complement digits are the magnitude.
    auto digits = bi->getDigits();
    size_t available = *word_count;
    size_t toCopy = std::min(available, digits.size());
    for (size_t i = 0; i < toCopy; ++i) {
      words[i] = digits[i];
    }
    // Zero-fill any extra slots the caller provided.
    for (size_t i = toCopy; i < available; ++i) {
      words[i] = 0;
    }
    *word_count = toCopy;
  } else {
    // Negative: we need the magnitude (absolute value) in sign-magnitude.
    // The magnitude is the two's complement negation of the stored digits.
    // Two's complement negation: invert all bits and add 1.
    auto digits = bi->getDigits();
    size_t available = *word_count;
    size_t toCopy = std::min(available, digits.size());

    uint64_t carry = 1;
    for (size_t i = 0; i < toCopy; ++i) {
      uint64_t inverted = ~digits[i];
      uint64_t sum = inverted + carry;
      carry = (sum < inverted) ? 1 : 0;
      words[i] = sum;
    }
    // Zero-fill any extra slots.
    for (size_t i = toCopy; i < available; ++i) {
      words[i] = 0;
    }
    *word_count = toCopy;
  }

  return napi_clear_last_error(env);
}
