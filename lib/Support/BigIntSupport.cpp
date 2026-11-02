/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Support/BigIntSupport.h"

#include "hermes/Support/OptValue.h"

#include "llvh/ADT/APInt.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/ADT/bit.h"
#include "llvh/Support/Endian.h"
#include "llvh/Support/MathExtras.h"
#include "llvh/Support/raw_ostream.h"

#include <cmath>
#include <limits>
#include <string>

namespace hermes {
namespace bigint {
// The maximum BigInt size in bits. This is defined here and not the header file
// to prevent more bit-related definitions in the API -- which mostly operates
// in terms of Digits, and sometimes, bytes.
static constexpr BigIntDigitType BigIntMaxSizeInBits =
    BigIntMaxSizeInDigits * BigIntDigitSizeInBits;

llvh::ArrayRef<uint8_t> dropExtraSignBits(llvh::ArrayRef<uint8_t> src) {
  if (src.empty()) {
    // return an empty array ref.
    return src;
  }

  const uint8_t drop = getSignExtValue<uint8_t>(src.back());

  // Iterate over all bytes in src, in reverse order, and drop everything that
  // can be inferred with a sign-extension from the previous byte. For example,
  //
  // src = { 0x00, 0x00, 0x00, 0xff }
  //
  // results in { 0x00, 0xff } so that sign extension results in the original
  // sequence being reconstructed.

  auto previousSrc = src;
  while (!src.empty() && src.back() == drop) {
    previousSrc = src;
    src = src.drop_back();
  }

  // Invariants:
  //
  //  * previousSrc.size() > 0
  //  * previousSrc == src -> no bytes dropped from src
  //  * previousSrc != src -> previousSrc.back() == drop
  //  * src.empty() -> original src = {drop, drop, drop, ..., drop, drop} and
  //                   previousSrc[0] == drop
  //
  // The return value should be
  //  * {} iff src.empty and drop == 0x00; or
  //  * {0xff} iff src.empty and drop == 0xff; or
  //  * src iff getSignExtValue(src.back()) == drop; and
  //  * previousSrc otherwise
  //
  // which can be expressed as
  //  * src iff src.empty and drop == 0x00; or
  //  * previousSrc iff src.empty and drop == 0xff; or
  //  * src iff getSignExtValue(src.back()) == drop; and
  //  * previousSrc otherwise
  //
  // By defining
  //   lastChar = src.empty ? 0 : src.back,
  //
  // the return value can be expressed as
  //  * src iff getSignExtValue(lastChar) == drop; and
  //  * previousSrc otherwise
  const uint8_t lastChar = src.empty() ? 0u : src.back();

  return getSignExtValue<uint8_t>(lastChar) == drop ? src : previousSrc;
}

namespace {
/// Trims any digits in \p dst that can be inferred by a sign extension.
void ensureCanonicalResult(MutableBigIntRef &dst) {
  auto ptr = reinterpret_cast<uint8_t *>(dst.digits);
  const uint32_t sizeInBytes = dst.numDigits * BigIntDigitSizeInBytes;

  llvh::ArrayRef<uint8_t> compactView =
      dropExtraSignBits(llvh::makeArrayRef(ptr, sizeInBytes));
  dst.numDigits = numDigitsForSizeInBytes(compactView.size());
}

static OperationStatus initWithDigits(
    MutableBigIntRef dst,
    ImmutableBigIntRef src) {
  auto ptr = reinterpret_cast<const uint8_t *>(src.digits);
  auto size = src.numDigits * BigIntDigitSizeInBytes;
  return initWithBytes(dst, llvh::makeArrayRef(ptr, size));
}
} // namespace

// Ensure there's a compile-time failure if/when hermes is compiled for
// big-endian machines. This is needed for correct serialization and
// deserialization (the hermes bytecode format expects bigint bytes in
// little-endian format).
static_assert(
    llvh::support::endian::system_endianness() == llvh::support::little,
    "BigIntSupport expects little-endian host");

OperationStatus initWithBytes(
    MutableBigIntRef dst,
    llvh::ArrayRef<uint8_t> data) {
  const uint32_t dstSizeInBytes = dst.numDigits * BigIntDigitSizeInBytes;

  assert(dst.digits != nullptr && "buffer can't be nullptr");

  if (dstSizeInBytes < data.size()) {
    // clear numDigits in the response (i.e., sanitizing the output).
    dst.numDigits = 0;
    return OperationStatus::DEST_TOO_SMALL;
  }

  const size_t dataSizeInBytes = data.size();

  if (dataSizeInBytes == 0) {
    // data is empty, so don't bother copying it to dst; simply return 0n.
    dst.numDigits = 0;
    return OperationStatus::RETURNED;
  }

  // Get a uint8_t* to dst so we can do pointer arithmetic.
  auto *ptr = reinterpret_cast<uint8_t *>(dst.digits);

  // Copy bytes first; dataSizeInBytes may not be a multiple of
  // BigIntDigitSizeInBytes.
  memcpy(ptr, data.data(), dataSizeInBytes);

  // Now sign-extend to a length that's multiple of DigitType size. Note that
  // dataSizeInBytes is not zero (otherwise the function would have returned)
  // by now.
  const uint32_t numBytesToSet = dstSizeInBytes - dataSizeInBytes;
  const uint8_t signExtValue =
      getSignExtValue<uint8_t>(ptr[dataSizeInBytes - 1]);

  memset(ptr + dataSizeInBytes, signExtValue, numBytesToSet);

  ensureCanonicalResult(dst);
  return OperationStatus::RETURNED;
}

bool isZero(ImmutableBigIntRef lhs) {
  return lhs.numDigits == 0;
}

bool isNegative(ImmutableBigIntRef src) {
  return src.numDigits > 0 &&
      static_cast<SignedBigIntDigitType>(src.digits[src.numDigits - 1]) < 0;
}

uint32_t fromDoubleResultSize(double src) {
  uint64_t srcI = llvh::bit_cast<uint64_t>(src);
  int64_t exp = ((srcI >> 52) & 0x7ff) - 1023;

  // If the exponent is negative, |src| is in +/- 0.xyz, so
  // just return 0.
  if (exp < 0)
    return 0;

  // double needs at most numBits(mantissa) + 1 (implicit 1 in the mantissa) +
  // exp - numBits(mantissa) + 1 bits to be represented, hence the + 2 below.
  return numDigitsForSizeInBits(exp + 2);
}

OperationStatus fromDouble(MutableBigIntRef dst, double src) {
  uint32_t numDigitsToRepresentDouble = dst.numDigits;

  assert(
      numDigitsToRepresentDouble >= fromDoubleResultSize(src) &&
      "not enough digits provided for double conversion");

  const uint32_t BitsToRepresentDouble = llvh::alignTo(
      numDigitsToRepresentDouble * BigIntDigitSizeInBits + 1,
      BigIntDigitSizeInBits);
  llvh::APInt tmp =
      llvh::APIntOps::RoundDoubleToAPInt(src, BitsToRepresentDouble);

  auto *ptr = reinterpret_cast<const uint8_t *>(tmp.getRawData());
  auto size = tmp.getNumWords() * BigIntDigitSizeInBytes;
  auto bytesRef = llvh::makeArrayRef<uint8_t>(ptr, size);
  return initWithBytes(dst, dropExtraSignBits(bytesRef));
}

namespace {
/// Helper adapter for calling getSignExtValue with *BigIntRefs.
template <typename AnyBigIntRef>
BigIntDigitType getBigIntRefSignExtValue(const AnyBigIntRef &src) {
  return src.numDigits == 0
      ? static_cast<BigIntDigitType>(0)
      : getSignExtValue<BigIntDigitType>(src.digits[src.numDigits - 1]);
}

/// Copies \p src's digits to \p dst's, which must have at least
/// as many digits as \p src. Sign-extends dst to fill \p dst.numDigits.
OperationStatus initNonCanonicalWithReadOnlyBigInt(
    MutableBigIntRef &dst,
    const ImmutableBigIntRef &src) {
  // ensure dst is large enough
  if (dst.numDigits < src.numDigits) {
    return OperationStatus::DEST_TOO_SMALL;
  }

  // now copy src digits to dst.
  const uint32_t digitsToCopy = src.numDigits;
  const uint32_t bytesToCopy = digitsToCopy * BigIntDigitSizeInBytes;
  memcpy(dst.digits, src.digits, bytesToCopy);

  // and finally size-extend dst to its size.
  const uint32_t digitsToSet = dst.numDigits - digitsToCopy;
  const uint32_t bytesToSet = digitsToSet * BigIntDigitSizeInBytes;
  const BigIntDigitType signExtValue = getBigIntRefSignExtValue(src);
  memset(dst.digits + digitsToCopy, signExtValue, bytesToSet);

  return OperationStatus::RETURNED;
}

/// \return dst = -src without worrying about negative integer negation
/// overflow.
std::tuple<OperationStatus, ImmutableBigIntRef> copyAndNegate(
    MutableBigIntRef dst,
    ImmutableBigIntRef src) {
  auto res = initNonCanonicalWithReadOnlyBigInt(dst, src);
  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return std::make_tuple(res, ImmutableBigIntRef{});
  }

  // Can't call unaryMinus directly as that API checks for negation overflow.
  llvh::APInt::tcNegate(dst.digits, dst.numDigits);

  // Still return a canonical bigint.
  ensureCanonicalResult(dst);

  // N.B.: numDigits could have been modified in ensureCanonicalResult.
  return std::make_tuple(
      OperationStatus::RETURNED, ImmutableBigIntRef{dst.digits, dst.numDigits});
}

/// \return whether the given \p mantissa needs to be rounded up or not. \p
/// firstDigit is the least significant digit in the bigint, \p currDigit is the
/// current digit being tested. \p bottomUnsetMantissaBits is non-zero if
/// mantissa has low-order bits that were not populated from the bigint.
/// \p numUnusedBitsInCurrDigit represents how many bits in \p *currDigit are
/// not part of the mantissa. For more information about these parameters, look
/// at their definitions in toDouble below.
bool roundMantissaUp(
    uint64_t mantissa,
    const BigIntDigitType *firstDigit,
    const BigIntDigitType *currDigit,
    uint32_t bottomUnsetMantissaBits,
    uint32_t numUnusedBitsInCurrDigit) {
  if (bottomUnsetMantissaBits != 0) {
    // There were not enough bits in the BigInt for completelly populating the
    // mantissa, so no need to round the number up.
    return false;
  }

  if (numUnusedBitsInCurrDigit == 0) {
    // All bits in currDigit are part of the mantissa. So we need to inspect the
    // next digit.
    if (firstDigit == currDigit) {
      // But when there's no next digit, the next bit is zero, thus round the
      // number down.
      return false;
    }

    // None of the BigIntDigitSizeInBits bits in currDigit are in the mantissa.
    --currDigit;
    numUnusedBitsInCurrDigit = BigIntDigitSizeInBits;
  }

  assert(currDigit >= firstDigit && "can't access currDigit");

  // Bit mask for testing the most significant bit not in the mantissa.
  const BigIntDigitType mostSignificantUnusedBit = 1ull
      << (numUnusedBitsInCurrDigit - 1);

  // If the most significant bit not in the mantissa is zero, round down.
  if (!(*currDigit & mostSignificantUnusedBit)) {
    return false;
  }

  // The most significant bit not in the mantissa is one. If any leftover digits
  // in currDigit are one, round up.
  if (*currDigit & (mostSignificantUnusedBit - 1)) {
    return true;
  }

  // currDigit is in the format |mmm|M|nnn|, where |mmm| is part of the
  // mantissa, |M| (the most significative bit not in the mantissa) is 1, and
  // and |nnn| is zero. Thus if any digit in [firstDigit, currDigit) are
  // non-zero, round up.
  while (currDigit > firstDigit) {
    if (*(--currDigit)) {
      return true;
    }
  }

  //                           |mantissa|M|nnnnnnnnnnnnnnnnnnnn...n|
  // The number has the format |mmmmmmmm|1|00000000000000000000...0|. Thus round
  // it up if the mantissa's least significative bit is 1.
  return mantissa & 1;
}
} // namespace

OperationStatus toDouble(double &dst, ImmutableBigIntRef src) {
  if (src.numDigits == 0) {
    dst = 0.0;
    return OperationStatus::RETURNED;
  }

  const bool isSrcNegative = isNegative(src);
  uint32_t tmpStorageSize = isSrcNegative ? src.numDigits : 0;

  // temporary storage used to negate negative operands.
  TmpStorage tmpStorage(tmpStorageSize);

  if (isSrcNegative) {
    MutableBigIntRef tmp{
        tmpStorage.requestNumDigits(tmpStorageSize), tmpStorageSize};
    auto [res, newSrc] = copyAndNegate(tmp, src);
    if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
      return res;
    }
    src = newSrc;
  }

  assert(src.numDigits > 0 && "empty src");

  // src's most significant digit may be zero in case zero-extension was needed
  // to preserve the bigint's sign.
  const BigIntDigitType *currDigit = src.digits + src.numDigits - 1;
  if (!*currDigit) {
    --currDigit;
  }

  // Even if currDigit is not the most significant digit, there should be at
  // least 1 digit in src -- otherwise, src == 0n, and that has already been
  // handled.
  assert(currDigit >= src.digits && *currDigit && "non-canonical BigInt found");

  // src's layout is
  //
  //                                         numBits
  //                          <----------------------------------------->
  //             sign               mantissa          low-order-bits
  //  <----------------------><-----------------> <--------------------->
  // |0000000000000000|00000001mm...mmm|mmm...mmmMnnnnnn|nnnnnnnn...nnnnn|
  //                                   ^         ^                       ^
  //                                currDigit  MSBNITM               firstDigit
  //
  // and it needs to be converted to the double |s|exp|mantissa|. The challenge
  // are:
  //
  // 1. Properly assembling a mantissa numeric_limits<double>::digits bits.
  //
  // 2. Computing the exponent to be used in the double
  //
  // 3. Deciding whether to round the mantissa.

  // Compute the number of leading zeros in currDigit. currDigit is never zero,
  // so clearBitsInMSD is in [0, 64).
  const uint32_t clearBitsInMSD =
      llvh::countLeadingZeros(*currDigit, llvh::ZeroBehavior::ZB_Width);
  assert(clearBitsInMSD < BigIntDigitSizeInBits);

  // numBits represents how many bits in src can pontentially be part of the
  // mantissa. This number is essentially the total number of bits in src minus
  // the number of upper zero bits.
  const uint32_t numBits =
      (currDigit - src.digits + 1) * BigIntDigitSizeInBits - clearBitsInMSD;

  // The exponent needs to account the fact that numBits include the most
  // significant non-zero bit in src. ieee 754 always drops that bit (as it is
  // implicitly one).
  uint32_t exp = numBits - 1;
  static_assert(std::numeric_limits<double>::max_exponent == 1024, "");
  if (exp > std::numeric_limits<double>::max_exponent - 1) {
    // The exponent is too large to represent in a double, thus return Inf or
    // (if src is negative), -Inf.
    dst = isSrcNegative ? -std::numeric_limits<double>::infinity()
                        : std::numeric_limits<double>::infinity();
    return OperationStatus::RETURNED;
  }

  // Number of bits in the mantissa of an ieee 754 double.
  constexpr uint32_t mantissaSizeInBits = 52;

  constexpr uint32_t numBitsSignExp =
      static_cast<uint32_t>(sizeof(double)) * 8 - mantissaSizeInBits;

  // Start assembling the mantissa. The idea is to shift the most significant
  // digit left enough times to ensure that all leading zeros in src, as well as
  // the most significant non-zero bit are dropped from the mantissa. Note that
  // msdShift may be 64 in case the most significant digit in src is 1 -- in
  // this case none of the bits in currDigit will be part of the mantissa.
  const uint32_t msdShift = clearBitsInMSD + 1;
  assert(0 <= msdShift && msdShift <= 64);
  uint64_t mantissa =
      msdShift == BigIntDigitSizeInBits ? 0 : (*currDigit << msdShift);
  // And now shift the mantissa right to make space for the sign and the
  // exponent in the final result.
  mantissa >>= numBitsSignExp;
  // Number of bits in the mantissa that need to be filled with with the most
  // significant bits of src's previous digit.
  uint32_t bottomUnsetMantissaBits =
      msdShift < numBitsSignExp ? 0 : msdShift - numBitsSignExp;

  // Number of bits in currDigit that are not part of the mantissa.
  uint32_t numUnusedBitsInCurrDigit =
      numBitsSignExp < msdShift ? 0 : numBitsSignExp - msdShift;
  if (bottomUnsetMantissaBits != 0 && currDigit > src.digits) {
    // The mantissa still has bottomUnsetMantissaBits unset bits. Extract those
    // from currDigit.
    --currDigit;

    // Copy the upper bottomUnsetMantissaBits bits from currDigit to the lower
    // bottomUnsetMantissaBits bits in the mantissa.
    mantissa |= *currDigit >> (BigIntDigitSizeInBits - bottomUnsetMantissaBits);

    // There are still left-over bits in currDigit, and those should be used for
    // rounding.
    numUnusedBitsInCurrDigit = BigIntDigitSizeInBits - bottomUnsetMantissaBits;

    // No more unset bits in the mantissa.
    bottomUnsetMantissaBits = 0;
  }

  // The mantissa is built, now we round it up if needed. The idea is to round
  // the number mmmm...mmmm * (2 ** exp) according to M and nnnn...nnnn. Thus,
  // rounding up is needed if he most significant bit not in the mantissa
  // (MSBNITM) is 1 and
  //
  //   1. the low-order-bits are not zero; or
  //   2. the mantissa's least significant digit is one.
  //
  // This is equivalent to rounding,
  //   a. 155.4 to 155;
  //   b. 155.6 to 156; and
  //   c. 155.5 to 156.
  if (roundMantissaUp(
          mantissa,
          src.digits,
          currDigit,
          bottomUnsetMantissaBits,
          numUnusedBitsInCurrDigit)) {
    // Rounding the result up means incrementing the mantissa by 1.
    if (++mantissa == 1ull << 52) {
      // The incremented mantissa is too large, so we need to increment the
      // exponent for the number.
      mantissa = 0;
      if (++exp > std::numeric_limits<double>::max_exponent - 1) {
        // Incrementing the exponent can cause an overflow, meaning the bigint
        // should be converted to (-)Inf
        dst = isSrcNegative ? -std::numeric_limits<double>::infinity()
                            : std::numeric_limits<double>::infinity();
        return OperationStatus::RETURNED;
      }
    }
  }

  // Now construct the resulting floating point value.
  // Sign is the left-most bit.
  const uint64_t sign =
      isSrcNegative ? (1ull << (sizeof(double) * 8 - 1)) : 0ull;
  // Exponent is 1023-biased, and shifted left of the mantissa.
  static constexpr uint64_t bias = 0x3FFull;
  const uint64_t exponent = (exp + bias) << mantissaSizeInBits;
  const uint64_t number = sign | exponent | mantissa;
  dst = llvh::bit_cast<double>(number);
  assert(!std::isnan(dst) && "BigInt to number should return a number!");
  return OperationStatus::RETURNED;
}

namespace {
/// ES5.1 7.2
/// Copied from Operations.h to preserve layering.
static inline bool isWhiteSpaceChar(char16_t c) {
  return c == u'\u0009' || c == u'\u000B' || c == u'\u000C' || c == u'\u0020' ||
      c == u'\u00A0' || c == u'\uFEFF' || c == u'\u1680' ||
      (c >= u'\u2000' && c <= u'\u200A') || c == u'\u202F' || c == u'\u205F' ||
      c == u'\u3000';
}

template <typename ConcreteParser>
struct ConcreteParserTraits;

/// A class with several utility methods used for parsing strings as bigints.
/// The spec has multiple types of bigint strings; each "type" should have
/// its own parser class that inherits from this.
template <typename ConcreteParser>
class BigIntLiteralParsingToolBox {
  BigIntLiteralParsingToolBox(const BigIntLiteralParsingToolBox &) = delete;
  BigIntLiteralParsingToolBox &operator=(const BigIntLiteralParsingToolBox &) =
      delete;
  BigIntLiteralParsingToolBox(BigIntLiteralParsingToolBox &&) = delete;
  BigIntLiteralParsingToolBox &operator=(BigIntLiteralParsingToolBox &&) =
      delete;

 protected:
  using StringRefT = typename ConcreteParserTraits<ConcreteParser>::StringRefT;

  // The underlying string's char type.
  using CharT = std::remove_cv_t<std::remove_reference_t<
      decltype(*typename StringRefT::const_iterator{})>>;

  BigIntLiteralParsingToolBox(
      StringRefT str,
      uint8_t &radix,
      std::string &bigintDigits,
      ParsedSign &sign,
      std::string *outError)
      : it_(str.begin()),
#ifndef NDEBUG
        begin_(str.begin()),
#endif // NDEBUG
        end_(str.end()),
        radix_(radix),
        bigintDigits_(bigintDigits),
        sign_(sign),
        outError_(outError) {
    bigintDigits_.clear();
    bigintDigits_.reserve(end_ - it_);

    sign_ = ParsedSign::None;
  }

  bool nonDecimalIntegerLiteral() {
    return binaryIntegerLiteral() || octalIntegerLiteral() ||
        hexIntegerLiteral();
  }

  bool binaryIntegerLiteral() {
#define BIGINT_BINARY_PREFIX 'B', 'b'
#define BIGINT_BINARY_DIGITS '0', '1'
    if (lookaheadAndEatIfAnyOf<BIGINT_BINARY_PREFIX>()) {
      radix_ = 2;
      dispatchBuildBigIntWithDigitsToConcrete<BIGINT_BINARY_DIGITS>();
      return bigintDigits_.size() > 0;
    }

    return false;
#undef BIGINT_BINARY_PREFIX
#undef BIGINT_BINARY_DIGITS
  }

  bool octalIntegerLiteral() {
#define BIGINT_OCTAL_PREFIX 'O', 'o'
#define BIGINT_OCTAL_DIGITS '0', '1', '2', '3', '4', '5', '6', '7'
    if (lookaheadAndEatIfAnyOf<BIGINT_OCTAL_PREFIX>()) {
      radix_ = 8;
      dispatchBuildBigIntWithDigitsToConcrete<BIGINT_OCTAL_DIGITS>();
      return bigintDigits_.size() > 0;
    }

    return false;
#undef BIGINT_OCTAL_PREFIX
#undef BIGINT_OCTAL_DIGITS
  }

  bool hexIntegerLiteral() {
#define BIGINT_HEX_PREFIX 'X', 'x'
#define BIGINT_HEX_DIGITS                                                    \
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', \
      'F', 'a', 'b', 'c', 'd', 'e', 'f'

    if (lookaheadAndEatIfAnyOf<BIGINT_HEX_PREFIX>()) {
      radix_ = 16;
      dispatchBuildBigIntWithDigitsToConcrete<BIGINT_HEX_DIGITS>();
      return bigintDigits_.size() > 0;
    }

    return false;
#undef BIGINT_HEX_PREFIX
#undef BIGINT_HEX_DIGITS
  }

  bool nonZeroDecimalLiteral() {
#define BIGINT_NONZERO_DEC_DIGITS '1', '2', '3', '4', '5', '6', '7', '8', '9'
#define BIGINT_DEC_DIGITS '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'
    if (nextIsAnyOf<BIGINT_NONZERO_DEC_DIGITS>()) {
      radix_ = 10;
      dispatchBuildBigIntWithDigitsToConcrete<BIGINT_DEC_DIGITS>();
      return bigintDigits_.size() > 0;
    }
    return false;
#undef BIGINT_NONZERO_DEC_DIGITS
#undef BIGINT_DEC_DIGITS
  }

  bool decimalDigits() {
    // first trims all leading zeroes, but keep 1 if the input is just zeroes.
    auto ch0 = peek();
    while (ch0 && *ch0 == '0') {
      auto chNext = peek(1);
      if (!chNext) {
        break;
      }
      eat();
      ch0 = chNext;
    }

#define BIGINT_DEC_DIGITS '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'
    if (nextIsAnyOf<BIGINT_DEC_DIGITS>()) {
      radix_ = 10;
      dispatchBuildBigIntWithDigitsToConcrete<BIGINT_DEC_DIGITS>();
      return bigintDigits_.size() > 0;
    }
    return false;
#undef BIGINT_NONZERO_DEC_DIGITS
  }

  /// Helper for dispatching a call to buildBigIntWithDigits to the concrete
  /// class.
  template <char... digits>
  void dispatchBuildBigIntWithDigitsToConcrete() {
    static_cast<ConcreteParser *>(this)
        ->template buildBigIntWithDigits<digits...>();
  }

  bool fail(const char *err) {
    if (outError_) {
      *outError_ = err;
    }
    return false;
  }

  /// \return true if there are no more characters to be consumed.
  bool isEnd() {
    auto ch = peek();
    // parsing suceeded if there are no more characters to be consumed, or if
    // the next charater is a null terminator.
    return !(ch && *ch != 0);
  }

  bool checkEnd(const char *err) {
    // parsing suceeded if there are no more characters to be consumed, or if
    // the next charater is a null terminator.
    return isEnd() ? true : fail(err);
  }

  template <char c>
  static bool anyOf(char rhs) {
    return c == rhs;
  }

  template <CharT c, CharT d, CharT... rest>
  static bool anyOf(CharT rhs) {
    return (c == rhs) || anyOf<d, rest...>(rhs);
  }

  template <CharT... chars>
  OptValue<CharT> lookaheadAndEatIfAnyOf() {
    OptValue<CharT> ret;

    if (auto ch = nextIsAnyOf<chars...>()) {
      eat();
      ret = ch;
    }

    return ret;
  }

  template <CharT... chars>
  OptValue<CharT> nextIsAnyOf() {
    OptValue<CharT> ret;

    if (auto ch = peek()) {
      if (anyOf<chars...>(*ch)) {
        ret = ch;
      }
    }

    return ret;
  }

  /// An opaque type that represents the current state of the number parser.
  using ParserState = typename StringRefT::const_iterator;

  /// \return the parser's current state, which can later be used as an argument
  /// to restoreParserState to reset the parser. Useful for cases that need
  /// backtracking.
  ParserState getCurrentParserState() const {
    return it_;
  }

  /// Restore the parser state to that of \p state.
  void restoreParserState(const ParserState &state) {
    assert(
        begin_ <= state &&
        "invalid parser state - pointing before input start");
    assert(state <= end_ && "invalid parser state - pointing past input end");
    it_ = state;
  }

  /// Eats (i.e., advances) 1 char in the input string.
  /// \return empty if at the end of input; or the "eaten" char if not.
  OptValue<CharT> eat() {
    OptValue<CharT> c = peek();
    if (c) {
      it_ += 1;
    }
    return c;
  }

  /// Peeks (i.e., returns, but does not advance) the \p which-th char from the
  /// current position.
  /// \return empty if \p which is greater than the remaining chars in the input
  /// buffer; or the \p which-th char from the current position.
  OptValue<CharT> peek(std::ptrdiff_t which = 0) const {
    OptValue<CharT> ret;
    if (it_ + which < end_) {
      ret = *(it_ + which);
    }
    return ret;
  }

  typename StringRefT::const_iterator it_;
#ifndef NDEBUG
  typename StringRefT::const_iterator begin_;
#endif // NDEBUG
  typename StringRefT::const_iterator end_;
  uint8_t &radix_;
  std::string &bigintDigits_;
  ParsedSign &sign_;
  std::string *outError_;
};

template <typename StringRef>
class StringIntegerLiteralParser;

template <typename T>
struct ConcreteParserTraits<StringIntegerLiteralParser<T>> {
  using StringRefT = T;
};

/// A class for parsing StringIntegerLiteral as bigints. Used to parse string
/// arguments to the BigInt constructor.
/// See https://tc39.es/ecma262/#sec-stringintegerliteral-grammar.
template <typename StringRefT>
class StringIntegerLiteralParser : public BigIntLiteralParsingToolBox<
                                       StringIntegerLiteralParser<StringRefT>> {
 public:
  using CharT = typename BigIntLiteralParsingToolBox<
      StringIntegerLiteralParser<StringRefT>>::CharT;

  StringIntegerLiteralParser(
      StringRefT str,
      uint8_t &radix,
      std::string &bigintDigits,
      ParsedSign &sign,
      std::string *outError)
      : BigIntLiteralParsingToolBox<StringIntegerLiteralParser<StringRefT>>(
            str,
            radix,
            bigintDigits,
            sign,
            outError) {
    if (this->it_ < this->end_ && *(this->end_ - 1) == 0) {
      --this->end_;
    }

    // StringIntegerLiterals may have leading/trailing whitespaces; skip them
    // now.
    while (this->it_ < this->end_ && isWhiteSpaceChar(*this->it_)) {
      ++this->it_;
    }

    while (this->it_ < this->end_ && isWhiteSpaceChar(*(this->end_ - 1))) {
      --this->end_;
    }
  }

  /// goal function for parsing a string passed to %BigInt% -- the bigint
  /// conversion function. \p return true if parsing suceeds, and false
  /// otherwise.
  bool goal() && {
    auto ch = this->peek();
    if (!ch) {
      this->radix_ = 10;
      this->bigintDigits_ = "0";
      return true;
    } else {
      if (*ch == '0') {
        // Saving the parser state in case this is not a non-decimal integer,
        // but rather a decimal integer with leading zeros.
        auto atZero = this->getCurrentParserState();

        // discard the current char -- possibly the '0' in 0x, 0o, or 0b.
        this->eat();

        // NonDecimalIntegerLiteral
        if (this->nonDecimalIntegerLiteral()) {
          return this->checkEnd("trailing data in non-decimal literal");
        }

        // Put the parser back at the initial 0, and retry parsing input as a
        // decimal string.
        this->restoreParserState(atZero);
      }

      if (auto signCh = this->template lookaheadAndEatIfAnyOf<'+', '-'>()) {
        this->sign_ = *ch == '+' ? ParsedSign::Plus : ParsedSign::Minus;
      }

      // This must be NonZeroDecimalLiteral
      if (this->decimalDigits()) {
        return this->checkEnd("trailing data in decimal literal");
      }
    }

    return this->fail("invalid bigint literal");
  }

 public:
  template <CharT... digits>
  void buildBigIntWithDigits() {
    OptValue<CharT> ch = this->template lookaheadAndEatIfAnyOf<digits...>();
    while (ch.hasValue()) {
      this->bigintDigits_.push_back(*ch);
      ch = this->template lookaheadAndEatIfAnyOf<digits...>();
    }
  }
};

template <>
struct ConcreteParserTraits<class NumericValueParser> {
  using StringRefT = llvh::StringRef;
};
/// A class for parsing NumericValues as bigints. Used to parse bigint literals.
/// See https://tc39.es/ecma262/#sec-numericvalue
class NumericValueParser
    : public BigIntLiteralParsingToolBox<NumericValueParser> {
 public:
  NumericValueParser(
      llvh::StringRef str,
      uint8_t &radix,
      std::string &bigintDigits,
      ParsedSign &sign,
      std::string *outError)
      : BigIntLiteralParsingToolBox(str, radix, bigintDigits, sign, outError) {}

  /// goal function for parsing a BigInt literal (e.g., 123n) from JS.
  /// \p return true if parsing suceeds, and false otherwise.
  bool goal() && {
    if (auto ch = peek()) {
      if (*ch == '0') {
        // discard the current char -- it is known to be '0'.
        eat();

        // This is either:
        //    (1) 0 BigIntLiteralSuffix
        //    (2) NonDecimalIntegerLiteral BigIntLiteralSuffix

        // Try matching (1).
        if (isEnd()) {
          radix_ = 10;
          bigintDigits_ = "0";
          return true;
        }

        // Try matching (2).
        if (nonDecimalIntegerLiteral()) {
          return checkEnd("trailing data in non-decimal literal");
        }
      } else {
        // This must be NonZeroDecimalLiteral BigIntLiteralSuffix
        if (nonZeroDecimalLiteral()) {
          return checkEnd("trailing data in decimal literal");
        }
      }
    }

    return fail("invalid bigint literal");
  }

  template <char... digits>
  void buildBigIntWithDigits() {
    OptValue<char> ch = lookaheadAndEatIfAnyOf<digits...>();
    while (ch.hasValue()) {
      bigintDigits_.push_back(*ch);
      auto atSep = getCurrentParserState();
      ch = lookaheadAndEatIfAnyOf<digits...>();
      if (!ch) {
        restoreParserState(atSep);
      }
    }
  }
};

/// \return How many bits to request when creating the APInt for \p str
/// using \p radix. Round the result so we always request a whole number of
/// BigIntDigitType words.
template <typename StringRefT>
static unsigned numBitsForBigintDigits(StringRefT str, uint8_t radix) {
  assert(
      (radix == 2 || radix == 4 || radix == 8 || radix == 10 || radix == 16) &&
      "unpected bigint radix");

  // For power of 2 radixes we know exactly how many bits each digit
  // consumes in binary.  For base 10, we have to guess, so we assume the
  // maximum bits each digit consumes.
  uint8_t maxBitsPerChar = radix == 10 ? 4 : llvh::findFirstSet(radix);

  // maxBitsPerChar * str.size() gives the exact-ish size S to represent str as
  // a BigIntDigitType, and the + 1 adds the sign bit.
  return numDigitsForSizeInBits(maxBitsPerChar * str.size() + 1) *
      BigIntDigitSizeInBits;
}

template <typename ParserT, typename StringRefT>
static std::optional<std::string> getDigitsWith(
    StringRefT src,
    uint8_t &radix,
    ParsedSign &sign,
    std::string *outError) {
  std::string bigintDigits;
  std::optional<std::string> ret;
  if (ParserT{src, radix, bigintDigits, sign, outError}.goal()) {
    ret = std::move(bigintDigits);
  }
  return ret;
}
} // namespace

std::optional<std::string> getStringIntegerLiteralDigitsAndSign(
    llvh::ArrayRef<char> src,
    uint8_t &radix,
    ParsedSign &sign,
    std::string *outError) {
  return getDigitsWith<StringIntegerLiteralParser<llvh::ArrayRef<char>>>(
      src, radix, sign, outError);
}

std::optional<std::string> getStringIntegerLiteralDigitsAndSign(
    llvh::ArrayRef<char16_t> src,
    uint8_t &radix,
    ParsedSign &sign,
    std::string *outError) {
  return getDigitsWith<StringIntegerLiteralParser<llvh::ArrayRef<char16_t>>>(
      src, radix, sign, outError);
}

std::optional<std::string> getNumericValueDigits(
    llvh::StringRef src,
    uint8_t &radix,
    std::string *outError) {
  ParsedSign sign;
  return getDigitsWith<NumericValueParser>(src, radix, sign, outError);
}

namespace {
template <typename ParserT, typename StringRefT>
static std::optional<std::vector<uint8_t>> parsedBigIntFrom(
    StringRefT input,
    std::string *outError) {
  uint8_t radix;
  ParsedSign sign;
  std::optional<std::string> bigintDigits =
      getDigitsWith<ParserT>(input, radix, sign, outError);

  std::optional<std::vector<uint8_t>> result;
  if (bigintDigits) {
    llvh::APInt i(
        numBitsForBigintDigits(*bigintDigits, radix), *bigintDigits, radix);

    assert(
        i.getBitWidth() % sizeof(llvh::APInt::WordType) == 0 &&
        "Must always allocate full words");

    auto *ptr = reinterpret_cast<const uint8_t *>(i.getRawData());
    size_t size = i.getNumWords() * sizeof(llvh::APInt::WordType);

    if (sign == ParsedSign::Minus) {
      i.negate();
    }

    result = std::vector<uint8_t>(ptr, ptr + size);
  }

  return result;
}
} // namespace

std::optional<ParsedBigInt> ParsedBigInt::parsedBigIntFromStringIntegerLiteral(
    llvh::ArrayRef<char> input,
    std::string *outError) {
  std::optional<ParsedBigInt> ret;
  if (auto maybeBytes =
          parsedBigIntFrom<StringIntegerLiteralParser<llvh::ArrayRef<char>>>(
              input, outError)) {
    ret = ParsedBigInt(*maybeBytes);
  }

  return ret;
}

std::optional<ParsedBigInt> ParsedBigInt::parsedBigIntFromStringIntegerLiteral(
    llvh::ArrayRef<char16_t> input,
    std::string *outError) {
  std::optional<ParsedBigInt> ret;
  if (auto maybeBytes = parsedBigIntFrom<
          StringIntegerLiteralParser<llvh::ArrayRef<char16_t>>>(
          input, outError)) {
    ret = ParsedBigInt(*maybeBytes);
  }

  return ret;
}

std::optional<ParsedBigInt> ParsedBigInt::parsedBigIntFromNumericValue(
    llvh::StringRef input,
    std::string *outError) {
  std::optional<ParsedBigInt> ret;
  if (auto maybeBytes = parsedBigIntFrom<NumericValueParser>(input, outError)) {
    ret = ParsedBigInt(std::move(*maybeBytes));
  }

  return ret;
}

std::string toString(ImmutableBigIntRef src, uint8_t radix) {
  assert(radix >= 2 && radix <= 36);

  if (isZero(src)) {
    return "0";
  }

  const unsigned numBits = src.numDigits * BigIntDigitSizeInBytes * 8;
  const bool sign = isNegative(src);
  llvh::APInt tmp(numBits, llvh::makeArrayRef(src.digits, src.numDigits));

  if (sign) {
    // negate negative numbers, and then add a "-" to the output.
    tmp.negate();
  }

  std::string digits;

  // avoid trashing the heap by pre-allocating the largest possible string
  // returned by this function. The "1" below is to account for a possible "-"
  // sign.
  digits.reserve(1 + src.numDigits * maxCharsPerDigitInRadix(radix));
  do {
    llvh::APInt quoc;
    uint64_t rem;
    llvh::APInt::udivrem(tmp, static_cast<uint64_t>(radix), quoc, rem);

    if (rem < 10) {
      digits.push_back('0' + rem);
    } else {
      digits.push_back('a' + rem - 10);
    }

    tmp = std::move(quoc);
  } while (tmp != 0);

  if (sign) {
    digits.push_back('-');
  }

  std::reverse(digits.begin(), digits.end());
  return digits;
}

OperationStatus
toString(std::string &out, llvh::ArrayRef<uint8_t> bytes, uint8_t radix) {
  unsigned numDigits = numDigitsForSizeInBytes(bytes.size());
  if (tooManyDigits(numDigits)) {
    return OperationStatus::TOO_MANY_DIGITS;
  }

  TmpStorage tmp(numDigits);
  MutableBigIntRef dst{tmp.requestNumDigits(numDigits), numDigits};
  auto res = initWithBytes(dst, bytes);
  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return res;
  }

  out = toString(ImmutableBigIntRef{dst.digits, dst.numDigits}, radix);
  return OperationStatus::RETURNED;
}

int compare(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  const int kLhsGreater = 1;
  const int kRhsGreater = -kLhsGreater;

  const bool lhsSign = isNegative(lhs);
  const bool rhsSign = isNegative(rhs);

  // Different signs:
  //   1) lhsSign => !rhsSign => lhs < rhs; or
  //   2) !lhsSign => rhsSign => lhs > rhs
  if (lhsSign != rhsSign) {
    return lhsSign ? kRhsGreater : kLhsGreater;
  }

  int result;

  if (lhs.numDigits == rhs.numDigits) {
    // Defer to APInt's comparison routine.
    result = llvh::APInt::tcCompare(lhs.digits, rhs.digits, lhs.numDigits);
  } else {
    // bigints are always created using their compact representation, thus
    // their sizes (in number of digits) can be used to compare them, with
    // the bigint that has more digits being greater/less (depending on
    // their sign).
    if (lhsSign) {
      // negative numbers -- the one with fewer digits is the greater.
      result = lhs.numDigits < rhs.numDigits ? kLhsGreater : kRhsGreater;
    } else {
      // positive numbers -- the one with most digits is the greater.
      result = lhs.numDigits < rhs.numDigits ? kRhsGreater : kLhsGreater;
    }
  }

  return result;
}

namespace {
/// Create an ImmutableBigIntRef wrapping the given digit. Note that \p digit is
/// not modified at all, so it could have been taking by const ref. That would
/// be dangerous, though, as calls like
///
/// makeImmutableRefFromSignedDigit(1)
///
/// would be valid, even though the returned ImmutableBigIntRef would be
/// pointing to invalid memory. A non-const ref requires the caller to allocate
/// the storage.
ImmutableBigIntRef makeImmutableRefFromSignedDigit(
    SignedBigIntDigitType &digit) {
  // Initially the bigint has 1 digit...
  uint32_t numDigits = 1;
  MutableBigIntRef mr{reinterpret_cast<BigIntDigitType *>(&digit), numDigits};
  // make sure mr is canonicalized, otherwise comparisons may fail -- they
  // assume all inputs to be canonical. This will happen when digit is 0.
  ensureCanonicalResult(mr);
  // Return a canonical, immutable bigint ref.
  return ImmutableBigIntRef{
      reinterpret_cast<BigIntDigitType *>(&digit), numDigits};
}
} // namespace

int compare(ImmutableBigIntRef lhs, SignedBigIntDigitType rhs) {
  const int kLhsGreater = 1;
  const int kRhsGreater = -kLhsGreater;

  if (rhs == 0) {
    if (isZero(lhs)) return 0;
    return isNegative(lhs) ? kRhsGreater : kLhsGreater;
  } else if (rhs == 1) {
    if (isZero(lhs) || isNegative(lhs)) return kRhsGreater;
    if (lhs.numDigits > 1) return kLhsGreater;
    return static_cast<SignedBigIntDigitType>(lhs.digits[0]) == 1 ? 0 : kLhsGreater;
  } else if (rhs == -1) {
    if (!isNegative(lhs)) return kLhsGreater;
    if (lhs.numDigits > 1) return kRhsGreater;
    return static_cast<SignedBigIntDigitType>(lhs.digits[0]) == -1 ? 0 : kRhsGreater;
  }

  // wrapping rhs in a ImmutableBigIntRef is safe -- rhs outlives the temporary.
  return compare(lhs, makeImmutableRefFromSignedDigit(rhs));
}

bool isSingleDigitTruncationLossless(
    ImmutableBigIntRef src,
    bool signedTruncation) {
  if (src.numDigits == 0) {
    // 0n can be represented in either signed or unsigned digits.
    return true;
  }

  if (signedTruncation) {
    // converting src to int64_t: any single digit bigint is truncated
    // losslessly.
    return src.numDigits == 1;
  }
  // converting src to uint64_t:
  // * any single digit bigint in the range [0, 0x8000000000000000)
  // * any two digit bigint whose second digit is used for zero-extension is
  //   truncated losslessly.
  return (src.numDigits == 1 &&
          src.digits[0] <= std::numeric_limits<int64_t>::max()) ||
      (src.numDigits == 2 && src.digits[1] == 0);
}

OperationStatus
asUintNResultSize(uint64_t n, ImmutableBigIntRef src, uint32_t &resultSize) {
  static_assert(
      BigIntMaxSizeInDigits < std::numeric_limits<uint32_t>::max(),
      "uint32_t is not large enough to represent max bigint digits.");

  const uint64_t numBitsSrc = src.numDigits * BigIntDigitSizeInBits;

  uint64_t numBitsResult;
  if (!isNegative(src)) {
    // for src >= 0, the result is limited to the same number of digits in src,
    // plus an extra bit for the sign.
    numBitsResult = std::min(n, numBitsSrc) + 1;
  } else {
    // for src < 0, the result is potentially unlimited as negative bigints have
    // infinite size.
    numBitsResult = n + 1;
  }

  if (numBitsResult > BigIntMaxSizeInBits) {
    return OperationStatus::TOO_MANY_DIGITS;
  }

  resultSize = numDigitsForSizeInBits(numBitsResult);
  return OperationStatus::RETURNED;
}

uint32_t asIntNResultSize(uint64_t n, ImmutableBigIntRef src) {
  static_assert(
      BigIntMaxSizeInDigits < std::numeric_limits<uint32_t>::max(),
      "uint32_t is not large enough to represent max bigint digits.");

  // The result size for asIntN is limited to src.numDigits. Moreover, there's
  // no need to account for a sign bit due to the two's complement
  // representation.
  const uint64_t numDigitsN = numDigitsForSizeInBits(n);

  return std::min<uint64_t>(src.numDigits, numDigitsN);
}

namespace {
enum class BigIntAs { IntN, UintN };

/// Helper that implements BigInt.asIntN and BigInt.asUintN. It works as
/// as follows
///
/// ----+-----------+-----------+-----------+-----------+-----------+----
/// ... | Digit k+2 | Digit k+1 |  Digit k  | Digit k-1 | Digit k-2 | ...
/// ----+-----------+-----------+-----------+-----------+-----------+----
///                              | fill |^
///                                      n
///
/// First, digits 0 through k are copied from \p src to \p dst, with
/// sign-extension.
///
/// Next, Digit k needs to be adjusted, either by zeroing out | fill | (if
/// \p operation == BigIntAs::UintN), or by copying \p n to | fill | (if
/// \p operation == BigIntAs::IntN). Note that nothing needs to happen to
/// Digit k if n == BigIntDigitSizeInBits - 1.
///
/// Finally, initialize the remaining bits in \p dst with zeros (AsUintN),
/// or Digit k's sign (AsIntN).
static OperationStatus bigintAsImpl(
    MutableBigIntRef dst,
    uint32_t numDigits,
    uint64_t n,
    ImmutableBigIntRef src,
    BigIntAs operation) {
  if (dst.numDigits < numDigits) {
    return OperationStatus::DEST_TOO_SMALL;
  }
  dst.numDigits = numDigits;

  // Special cases:
  //     src == 0n => dst == 0n for any n.
  //     n = 0 => dst == 0n for any src.
  if (src.numDigits == 0 || n == 0) {
    return initWithDigits(dst, src);
  }

  // figure out the k-th digit -- i.e., the digit where bit n lives. Also figure
  // out which bit in k is the last that should be copied to the output.
  const uint64_t k = (n - 1) / BigIntDigitSizeInBits;
  const uint32_t bitWithinK = (n - 1) % BigIntDigitSizeInBits;

  // sanity-check: k (zero based) should be less than dst.numDigits for
  // operation == BigInt::AsUintN when src < 0n. For BigInt.asIntN(), however,
  // that restriction can be relaxed. For example, BigInt.asIntN(9999, -1n) will
  // need exactly 1 digit. In general, BigInt.asIntN() can always be represented
  // with at most src.numDigits.
  assert(
      (k < dst.numDigits || !isNegative(src) || operation == BigIntAs::IntN) &&
      "result is missing digits");

  // only copy the first k digits from src into dst, unless k is more digits --
  // then simply copy all of src. The uint64_t to uint32_t truncation is safe
  // because the result will either be src.numDigits (which is an uint32_t), or
  // k + 1 bound by src.numDigits. Note that the truncation is only safe after
  // std::min returns.
  const uint32_t numDigitsToCopy = std::min<uint64_t>(k + 1, src.numDigits);

  // only initialize the first k digits in dst, unless k is more digits -- then
  // fully initialize dst.
  // N.B.: This can't be const because it is used to initialize a
  // MutableBigIntRef.
  uint32_t numDigitsDst = std::min<uint64_t>(k + 1, dst.numDigits);
  MutableBigIntRef limitedDst{dst.digits, numDigitsDst};

  // copy digits from src to dst, sign-extending src if needed.
  auto res = initNonCanonicalWithReadOnlyBigInt(
      limitedDst, ImmutableBigIntRef{src.digits, numDigitsToCopy});
  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return res;
  }

  if (k < dst.numDigits) {
    // decide on zero- vs sign-extension based on the operation parameter.
    const bool hasSign = operation == BigIntAs::IntN;

    // then figure out what the result sign is. the result is unsigned if the
    // operation if asUintN, or if the n-th bit in dst is set.
    // N.B.: src may not have an n-th bit (i.e., n is larger than the number of
    // bits in src).
    // N.B.: the n-th bit in dst is bit n-1.
    const bool sign = hasSign && (dst.digits[k] & (1ull << bitWithinK)) != 0;

    if (bitWithinK < BigIntDigitSizeInBits - 1) {
      // compute a mask for sign-extension -- i.e., a mask that, when or'd with
      // the digit where n lives results in that digit being sign extended. if
      // the result needs to be zero-extended, then use the complement mask to
      // clear the upper bits. Note how this creates a mask with with upper
      // (i.e., Trailing) bits -- that's because dst is correct up-to, and
      // including, bit n-1.
      const BigIntDigitType signExtMask =
          llvh::maskTrailingZeros<BigIntDigitType>(bitWithinK + 1);

      if (sign) {
        dst.digits[k] |= signExtMask;
      } else {
        dst.digits[k] &= ~signExtMask;
      }
    }

    // There could be digits past k. Those should be initialized with 0, or ~0,
    // according to sign. Note that sign will always be false for
    // operation == UintN, so the result will always be zero extended.
    const uint32_t numDigitsToSet =
        (k + 1 < dst.numDigits) ? (dst.numDigits - k - 1) : 0;

    // sanity-check: BigInt.asUintN should always return a non-negative number.
    // Thus, either the k-th digit is positive (when converted to a signed
    // bigint digit), or there should exist more digits after k (for zero
    // extension).
    assert(
        (operation == BigIntAs::IntN ||
         static_cast<SignedBigIntDigitType>(dst.digits[k]) >= 0 ||
         dst.numDigits > k + 1) &&
        "BigInt.asUintN will result in negative number.");

    // zero-/sign-extend the result's upper digits, if any.
    memset(
        dst.digits + k + 1,
        sign ? 0xff : 0,
        numDigitsToSet * BigIntDigitSizeInBytes);
  }

  ensureCanonicalResult(dst);
  return OperationStatus::RETURNED;
}
} // namespace

OperationStatus
asUintN(MutableBigIntRef dst, uint64_t n, ImmutableBigIntRef src) {
  uint32_t numDigits;
  OperationStatus s = asUintNResultSize(n, src, numDigits);
  if (LLVM_UNLIKELY(s != OperationStatus::RETURNED)) {
    return s;
  }
  return bigintAsImpl(dst, numDigits, n, src, BigIntAs::UintN);
}

OperationStatus
asIntN(MutableBigIntRef dst, uint64_t n, ImmutableBigIntRef src) {
  const uint32_t numDigits = asIntNResultSize(n, src);
  return bigintAsImpl(dst, numDigits, n, src, BigIntAs::IntN);
}

uint32_t unaryMinusResultSize(ImmutableBigIntRef src) {
  // negating a non-negative number requires at most the same number of digits,
  // but it could require less; negating a negative number could require an
  // extra digit to hold the sign bit(s). Specifically, negating
  //
  // 0x8000000000000000n (which is a negative number)
  //
  // requires an extra digit:
  //
  // 0x0000000000000000 0x8000000000000000n
  return !isNegative(src) ? src.numDigits : src.numDigits + 1;
}

OperationStatus unaryMinus(MutableBigIntRef dst, ImmutableBigIntRef src) {
  auto res = initNonCanonicalWithReadOnlyBigInt(dst, src);
  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return res;
  }

  llvh::APInt::tcNegate(dst.digits, dst.numDigits);
  ensureCanonicalResult(dst);

  assert(
      ((isNegative(ImmutableBigIntRef{dst.digits, dst.numDigits}) !=
        isNegative(src)) ||
       isZero(src)) &&
      "unaryMinus overflow");
  return OperationStatus::RETURNED;
}

uint32_t unaryNotResultSize(ImmutableBigIntRef src) {
  // ~ 0n requires one extra digit;
  // ~ anything else requires at most src.numDigits digits.
  return std::max(1u, src.numDigits);
}

OperationStatus unaryNot(MutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  auto res = initNonCanonicalWithReadOnlyBigInt(lhs, rhs);
  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return res;
  }

  llvh::APInt::tcComplement(lhs.digits, lhs.numDigits);
  ensureCanonicalResult(lhs);
  return OperationStatus::RETURNED;
}

namespace {
using AdditiveOp = BigIntDigitType (*)(
    BigIntDigitType *,
    const BigIntDigitType *,
    BigIntDigitType,
    unsigned);
using AdditiveOpPart =
    BigIntDigitType (*)(BigIntDigitType *, BigIntDigitType, unsigned);
using AdditiveOpPostProcess = void (*)(MutableBigIntRef &);

OperationStatus additiveOperation(
    AdditiveOp op,
    AdditiveOpPart opPart,
    AdditiveOpPostProcess opPost,
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  // Requirement: lhs should have at most rhs.numDigits digits. This allows for
  // an efficient implementation of the operation as follows:
  //
  // dst = sign-ext lhs
  // dst op= rhs
  //
  // Which fits nicely into the APInt model where
  //  1. operands should have the same size; and
  //  2. operations are in-place
  assert(
      lhs.numDigits <= rhs.numDigits &&
      "lhs should have fewer digits than rhs");

  if (dst.numDigits < rhs.numDigits) {
    return OperationStatus::DEST_TOO_SMALL;
  }

  // The caller provided dst may be too big -- i.e., have more digits than
  // actually needed. Precisely rhs.numDigits + 1 digits are needed to simulate
  // infinite precision. Thus limit the result size to rhs.numDigits + 1 if dst
  // has more digits than that.
  if (rhs.numDigits + 1 < dst.numDigits) {
    dst.numDigits = rhs.numDigits + 1;
  }

  // dst = sign-ext lhs.
  auto res = initNonCanonicalWithReadOnlyBigInt(dst, lhs);
  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return res;
  }

  // dst op= rhs
  const BigIntDigitType carryIn = 0;
  BigIntDigitType carryOut =
      (*op)(dst.digits, rhs.digits, carryIn, rhs.numDigits);
  (*opPart)(
      dst.digits + rhs.numDigits,
      carryOut + getBigIntRefSignExtValue(rhs),
      dst.numDigits - rhs.numDigits);

  // perform any post-op transformation.
  (*opPost)(dst);

  // Resize dst appropriately to ensure the resulting bigint is canonical.
  ensureCanonicalResult(dst);
  return OperationStatus::RETURNED;
}

BigIntDigitType
noopAdditiveOpPart(BigIntDigitType *, BigIntDigitType, unsigned numDigits) {
  assert(
      numDigits == 0 &&
      "noop additive part was given digits; noop additive part is free!");
  return 0;
}

void noopAdditiveOpPostProcess(MutableBigIntRef &) {}

void negateAdditiveOpPostProcess(MutableBigIntRef &dst) {
  llvh::APInt::tcNegate(dst.digits, dst.numDigits);
}

template <auto Op>
BigIntDigitType tcBitwiseWithCarry(
    BigIntDigitType *lhs,
    const BigIntDigitType *rhs,
    BigIntDigitType /*unused*/,
    uint32_t numDigits) {
  Op(lhs, rhs, numDigits);
  return 0;
}
} // namespace

uint32_t bitwiseANDResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  return std::max(lhs.numDigits, rhs.numDigits);
}

OperationStatus bitwiseAND(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  // bitwiseAND is commutative, so lhs and rhs can be swapped at will.
  const auto &[srcWithFewerDigits, srcWithMostDigits] =
      lhs.numDigits <= rhs.numDigits ? std::make_tuple(lhs, rhs)
                                     : std::make_tuple(rhs, lhs);

  return additiveOperation(
      tcBitwiseWithCarry<llvh::APInt::tcAnd>,
      noopAdditiveOpPart,
      noopAdditiveOpPostProcess,
      dst,
      srcWithFewerDigits,
      srcWithMostDigits);
}

uint32_t bitwiseORResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  return std::max(lhs.numDigits, rhs.numDigits);
}

OperationStatus bitwiseOR(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  // bitwiseOR is commutative, so lhs and rhs can be swapped at will.
  const auto &[srcWithFewerDigits, srcWithMostDigits] =
      lhs.numDigits <= rhs.numDigits ? std::make_tuple(lhs, rhs)
                                     : std::make_tuple(rhs, lhs);

  return additiveOperation(
      tcBitwiseWithCarry<llvh::APInt::tcOr>,
      noopAdditiveOpPart,
      noopAdditiveOpPostProcess,
      dst,
      srcWithFewerDigits,
      srcWithMostDigits);
}

uint32_t bitwiseXORResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  return std::max(lhs.numDigits, rhs.numDigits);
}

OperationStatus bitwiseXOR(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  // bitwiseXOR is commutative, so lhs and rhs can be swapped at will.
  const auto &[srcWithFewerDigits, srcWithMostDigits] =
      lhs.numDigits <= rhs.numDigits ? std::make_tuple(lhs, rhs)
                                     : std::make_tuple(rhs, lhs);

  return additiveOperation(
      tcBitwiseWithCarry<llvh::APInt::tcXor>,
      noopAdditiveOpPart,
      noopAdditiveOpPostProcess,
      dst,
      srcWithFewerDigits,
      srcWithMostDigits);
}

uint32_t addResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  // simulate infinite precision by requiring an extra digits in the result,
  // regardless of the operands. It could be smarter -- carry will only happen
  // if the operands are the same size, and the sign_bit ^ last_bit is 1 for
  // either.
  return std::max(lhs.numDigits, rhs.numDigits) + 1;
}

OperationStatus
add(MutableBigIntRef dst, ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  // Addition is commutative, so lhs and rhs can be swapped at will.
  const auto &[srcWithFewerDigits, srcWithMostDigits] =
      lhs.numDigits <= rhs.numDigits ? std::make_tuple(lhs, rhs)
                                     : std::make_tuple(rhs, lhs);

  return additiveOperation(
      llvh::APInt::tcAdd,
      llvh::APInt::tcAddPart,
      noopAdditiveOpPostProcess,
      dst,
      srcWithFewerDigits,
      srcWithMostDigits);
}

uint32_t addSignedResultSize(
    ImmutableBigIntRef lhs,
    SignedBigIntDigitType sImm) {
  return addResultSize(lhs, makeImmutableRefFromSignedDigit(sImm));
}

OperationStatus addSigned(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    SignedBigIntDigitType sImm) {
  return add(dst, lhs, makeImmutableRefFromSignedDigit(sImm));
}

uint32_t subtractResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  // Simulate infinite precision by requiring an extra digit in the result,
  // regardless of the operands. It could be smarter -- carry will only happen
  // if the operands are the same size, and the sign_bit ˆ last_bit is 1 for
  // either.
  return std::max(lhs.numDigits, rhs.numDigits) + 1;
}

OperationStatus
subtract(MutableBigIntRef dst, ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  // Subtraction is not commutative, so the result may need to be negated, in
  // case rhs has fewer digits than lhs.
  const auto &[srcWithFewerDigits, srcWithMostDigits, postProcess] =
      lhs.numDigits <= rhs.numDigits
      ? std::make_tuple(lhs, rhs, noopAdditiveOpPostProcess)
      : std::make_tuple(rhs, lhs, negateAdditiveOpPostProcess);

  return additiveOperation(
      llvh::APInt::tcSubtract,
      llvh::APInt::tcSubtractPart,
      postProcess,
      dst,
      srcWithFewerDigits,
      srcWithMostDigits);
}

uint32_t subtractSignedResultSize(
    ImmutableBigIntRef lhs,
    SignedBigIntDigitType sImm) {
  return subtractResultSize(lhs, makeImmutableRefFromSignedDigit(sImm));
}

OperationStatus subtractSigned(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    SignedBigIntDigitType sImm) {
  return subtract(dst, lhs, makeImmutableRefFromSignedDigit(sImm));
}

uint32_t multiplyResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  return (!lhs.numDigits || !rhs.numDigits) ? 0
                                            : lhs.numDigits + rhs.numDigits + 1;
}

OperationStatus
multiply(MutableBigIntRef dst, ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  const uint32_t oldDstSize = multiplyResultSize(lhs, rhs);
  const bool isLhsNegative = isNegative(lhs);
  const bool isRhsNegative = isNegative(rhs);

  // tcFullMultiply operates on unsigned quantities, so lhs/rhs may need to be
  // negated. They could temporarily negated in place, but that violates the
  // promise the API makes by taking ImmutableBigIntRefs. The solution is thus
  // to allocate temporary buffers for negating the inputs when needed.
  //
  // There's no need to worry about the need for an extra digit to prevent
  // negation overflow (i.e., -MIN_INT == MIN_INT). For example,
  //
  // if lhs = -0x80000000_00000000n, then
  //    * lhs.numDigits = 1; and
  //    * lhs.digits[0] = 0x8000000000000000
  //
  // and negating that usually yields
  //
  //     * result.numDigits = 2; and
  //     * result.digits[0] = 0x8000000000000000
  //     * result.digits[1] = 0x0000000000000000
  //
  // i.e., there's an explicit zero-extension of the result, which is
  // superfluous given tcFullMultiply assumption of unsigned operands.
  uint32_t tmpStorageSizeLhs = isLhsNegative ? lhs.numDigits : 0;
  uint32_t tmpStorageSizeRhs = isRhsNegative ? rhs.numDigits : 0;
  const uint32_t tmpStorageSize = tmpStorageSizeLhs + tmpStorageSizeRhs;

  // temporary storage used to negate negative operands.
  TmpStorage tmpStorage(tmpStorageSize);

  if (isLhsNegative) {
    MutableBigIntRef tmp{
        tmpStorage.requestNumDigits(tmpStorageSizeLhs), tmpStorageSizeLhs};
    auto [res, newLhs] = copyAndNegate(tmp, lhs);
    if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
      return res;
    }
    lhs = newLhs;
  }

  if (isRhsNegative) {
    MutableBigIntRef tmp{
        tmpStorage.requestNumDigits(tmpStorageSizeRhs), tmpStorageSizeRhs};
    auto [res, newRhs] = copyAndNegate(tmp, rhs);
    if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
      return res;
    }
    rhs = newRhs;
  }

  // The operands may have been negated, but their sizes should not have
  // changed.
  const uint32_t dstSize = multiplyResultSize(lhs, rhs);
  assert(
      oldDstSize == dstSize &&
      "multiplication result size can't change even if operands were negated.");
  (void)oldDstSize;

  if (dst.numDigits < dstSize) {
    return OperationStatus::DEST_TOO_SMALL;
  }

  // Truncate dst to the exact result size.
  dst.numDigits = dstSize;

  // if dstSize is zero, then there's no need to perform the multiplication.
  if (dstSize > 0) {
    // tcFullMultiply returns a result with lhs.numDigits + rhs.numDigits. Thus,
    // there could be extraneous digits in dst that are not initialized by it.
    llvh::APInt::tcFullMultiply(
        dst.digits, lhs.digits, rhs.digits, lhs.numDigits, rhs.numDigits);

    // Zero out extranous digits in dst. These digits are used to simulate
    // infinite precision when multiplying negative and positive numbers.
    const uint32_t resultSize = lhs.numDigits + rhs.numDigits;
    memset(
        dst.digits + resultSize,
        0,
        (dst.numDigits - resultSize) * BigIntDigitSizeInBytes);

    // The result must be negated if the srcs' signs don't match.
    const bool negateResult = isLhsNegative != isRhsNegative;
    if (negateResult) {
      llvh::APInt::tcNegate(dst.digits, dst.numDigits);
    }
  }

  ensureCanonicalResult(dst);
  return OperationStatus::RETURNED;
}

namespace {
namespace div_rem {
static uint32_t getResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  return std::max(lhs.numDigits, rhs.numDigits) + 1;
}

static OperationStatus compute(
    MutableBigIntRef quoc,
    MutableBigIntRef rem,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  assert(
      ((quoc.digits != nullptr) != (rem.digits != nullptr)) &&
      "untested -- calling with both or neither quoc and rem");

  const uint32_t resultSize = divideResultSize(lhs, rhs);
  // set quoc's and rem's numDigits if their digits buffer is nullptr, which
  // allows querying either for determining the result size.
  if (quoc.digits == nullptr) {
    quoc.numDigits = resultSize;
  } else {
    rem.numDigits = resultSize;
  }

  // Ensure quoc is large enough; rem is the same size.
  if (quoc.numDigits < resultSize) {
    return OperationStatus::DEST_TOO_SMALL;
  }

  // make sure to drop any extraneous digits.
  quoc.numDigits = resultSize;
  rem.numDigits = resultSize;

  // Signal division by zero.
  if (isZero(rhs)) {
    return OperationStatus::DIVISION_BY_ZERO;
  }

  // tcDivide operates on unsigned number, so just like multiply, the operands
  // must be negated (and the result as well, if appropriate) if they are
  // negative.
  const bool isLhsNegative = isNegative(lhs);
  const bool isRhsNegative = isNegative(rhs);

  // If rhs has fewer digits than quoc/rem it must be resized.
  const bool needToResizeRhs = rhs.numDigits < resultSize;

  // Figure out which temporary buffers are needed -- this determines how much
  // temporary storage will be allocated for the division.
  const bool needTmpQuoc = quoc.digits == nullptr;
  const bool needTmpRem = rem.digits == nullptr;
  const bool needTmpRhs = isRhsNegative || needToResizeRhs;

  uint32_t tmpStorageSizeScratch = resultSize;
  uint32_t tmpStorageSizeQuoc = needTmpQuoc ? resultSize : 0;
  uint32_t tmpStorageSizeRem = needTmpRem ? resultSize : 0;
  uint32_t tmpStorageSizeRhs = needTmpRhs ? resultSize : 0;

  const uint32_t tmpStorageSize = tmpStorageSizeScratch + tmpStorageSizeQuoc +
      tmpStorageSizeRem + tmpStorageSizeRhs;

  TmpStorage tmpStorage(tmpStorageSize);

  BigIntDigitType *scratch = tmpStorage.requestNumDigits(tmpStorageSizeScratch);

  if (needTmpQuoc) {
    assert(quoc.numDigits == tmpStorageSizeQuoc);
    quoc.digits = tmpStorage.requestNumDigits(tmpStorageSizeQuoc);
  } else {
    assert(rem.numDigits == tmpStorageSizeRem);
    rem.digits = tmpStorage.requestNumDigits(tmpStorageSizeRem);
  }

  if (needTmpRhs) {
    MutableBigIntRef tmpRhs{
        tmpStorage.requestNumDigits(tmpStorageSizeRhs), tmpStorageSizeRhs};
    auto res = initNonCanonicalWithReadOnlyBigInt(tmpRhs, rhs);
    assert(res == OperationStatus::RETURNED && "temporary array is too small");
    (void)res;
    if (isRhsNegative) {
      llvh::APInt::tcNegate(tmpRhs.digits, tmpRhs.numDigits);
    }
    rhs = ImmutableBigIntRef{tmpRhs.digits, tmpRhs.numDigits};
  }

  // tcDivide is in-place (i.e. quoc is lhs), so division will be expressed as
  //
  // quoc = signExt(lhs)
  // quoc, rem, scratch /= signExt(rhs)
  auto res = initNonCanonicalWithReadOnlyBigInt(quoc, lhs);
  assert(res == OperationStatus::RETURNED && "quoc array is too small");
  (void)res;

  // lhs can't be modified, but it has been sign-extended into quoc; thus, if
  // lhs < 0 negate quoc.
  if (isLhsNegative) {
    llvh::APInt::tcNegate(quoc.digits, quoc.numDigits);
  }

  llvh::APInt::tcDivide(
      quoc.digits, rhs.digits, rem.digits, scratch, resultSize);

  // post-process quoc if no space was allocated for it in the temporary storage
  // -- i.e., the caller wants the quoc.
  if (!needTmpQuoc) {
    // quoc must be negated if lhs' and rhs' signs don't match.
    const bool negateQuoc = isLhsNegative != isRhsNegative;
    if (negateQuoc) {
      llvh::APInt::tcNegate(quoc.digits, quoc.numDigits);
    }
    ensureCanonicalResult(quoc);
  }

  // post-process rem if no space was allocated for it in the temporary storage
  // -- i.e., the caller wants the rem.
  if (!needTmpRem) {
    // rem must be negated if lhs is negative.
    if (isLhsNegative) {
      llvh::APInt::tcNegate(rem.digits, rem.numDigits);
    }
    ensureCanonicalResult(rem);
  }

  return OperationStatus::RETURNED;
}
} // namespace div_rem
} // namespace

uint32_t divideResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  return div_rem::getResultSize(lhs, rhs);
}

OperationStatus
divide(MutableBigIntRef dst, ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  uint32_t numRemDigits = 0;
  MutableBigIntRef nullRem{nullptr, numRemDigits};
  return div_rem::compute(dst, nullRem, lhs, rhs);
}

uint32_t remainderResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  return div_rem::getResultSize(lhs, rhs);
}

OperationStatus remainder(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  uint32_t numQuocDigits = 0;
  MutableBigIntRef nullQuoc{nullptr, numQuocDigits};
  return div_rem::compute(nullQuoc, dst, lhs, rhs);
}

namespace {
/// Implements the fast-path for computing \p dst = 2n ** \p rhs.
static OperationStatus exponentiatePowerOf2(
    MutableBigIntRef dst,
    uint32_t exponent) {
  const uint32_t numDigitsResult = 1 + (exponent / BigIntDigitSizeInBits);
  const uint32_t numDigits = 1 + numDigitsResult;
  const uint32_t bitToSet = exponent % BigIntDigitSizeInBits;

  if (BigIntMaxSizeInDigits < numDigits) {
    return OperationStatus::TOO_MANY_DIGITS;
  }

  if (dst.numDigits < numDigits) {
    return OperationStatus::DEST_TOO_SMALL;
  }
  dst.numDigits = numDigits;

  // dummyDigit is used for creating a temporary 0n that is used to initialize
  // dst -- i.e., zero-initialize all elements in dst.
  BigIntDigitType dummyDigit[1];
  ImmutableBigIntRef zero{dummyDigit, 0};
  auto res = initNonCanonicalWithReadOnlyBigInt(dst, zero);
  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return res;
  }
  // then set the only bit in result.
  dst.digits[numDigitsResult - 1] = 1ull << bitToSet;
  return OperationStatus::RETURNED;
}

/// Helper class that holds a reference to a MutableBigIntRef and what's the
/// maximum number of digits in ref. This is needed during exponentiateSlowPath
/// below so that the intermediate results can be canonicalized and later
/// restored to an "empty" state. It exposes conversion operators to make
/// passing the underlaying ref as input/output to other function in the API
/// (e.g., multiply).
class MutableBigIntAndMaxSize {
 public:
  // This (implicit) convenience constructor assumes r.numDigits is also the
  // maximum number of digits that \p r can hold.
  MutableBigIntAndMaxSize(MutableBigIntRef &ref)
      : r(&ref), maxDigits(ref.numDigits) {}

  /*implicit*/ operator MutableBigIntRef &() {
    return ref();
  }

  /*implicit*/ operator ImmutableBigIntRef() const {
    return ImmutableBigIntRef{r->digits, r->numDigits};
  }

  void swap(MutableBigIntAndMaxSize &other) {
    std::swap(r, other.r);
    std::swap(maxDigits, other.maxDigits);
  }

  MutableBigIntRef &ref() {
    return *r;
  }

  void resetRefNumDigits() {
    r->numDigits = maxDigits;
  }

  uint32_t getMaxDigits() const {
    return maxDigits;
  }

 private:
  MutableBigIntRef *r;
  uint32_t maxDigits;
};

/// Helper method for converting the status returned by multiply in
/// exponentiateSlowPath to the actual status that needs to be reported to the
/// user.
OperationStatus multiplyStatusToExponentiateStatus(
    OperationStatus status,
    uint32_t maxDigitsDst) {
  // if dst has max digits, then don't return DEST_TOO_SMALL -- because there's
  // no way to create a BigInt with more digits.
  if (status == OperationStatus::DEST_TOO_SMALL &&
      maxDigitsDst >= BigIntMaxSizeInDigits) {
    return OperationStatus::TOO_MANY_DIGITS;
  }

  // The multiply status should be bubbled up.
  return status;
}

/// Slow path for bigint exponentiation. Computes the following series
///
//    (lhs ** 1      * rhs_0) +
//    (lhs ** 2      * rhs_1) +
//    (lhs ** 4      * rhs_2) +
//          ...          +
//    (lhs ** 2 ** n * rhs_n)
//
// where lhs_i is the i-th bit in rhs.
OperationStatus exponentiateSlowPath(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    uint32_t exponent) {
  // At each iteration i, runningSquare will have the current value for
  // lhs ** 2 ** i, and nextRunningSquare will be used to compute
  // runningSquare * runningSquare (i.e.,
  // nextRunningSquare = runningSquare ** 2) which will be used by the next
  // iteration in the algorithm. This is necessary because multiply()
  // requires/assumes dst's digits don't overload with lhs' or rhs'.
  uint32_t runningSquareSize0 = BigIntMaxSizeInDigits;
  uint32_t runningSquareSize1 = BigIntMaxSizeInDigits;
  uint32_t tmpResultTmpSize = BigIntMaxSizeInDigits;
  TmpStorage tmpBuffers(
      runningSquareSize0 + runningSquareSize1 + tmpResultTmpSize);
  MutableBigIntRef runningSquare0{
      tmpBuffers.requestNumDigits(runningSquareSize0), runningSquareSize0};
  MutableBigIntRef runningSquare1{
      tmpBuffers.requestNumDigits(runningSquareSize1), runningSquareSize1};

  // at each iteration i, result is the exponentiation result, i.e.,
  //
  //   result = result * runningSquare, if lhs_i is 1
  //   result = result                , otherwise
  //
  // Thus, a temporary result buffer is needed (because multiply() requires).

  MutableBigIntRef tmpResult{
      tmpBuffers.requestNumDigits(tmpResultTmpSize), tmpResultTmpSize};

  // These are the MutableBigIntRef "adapters". Besides being implicitly
  // converted to ImmutableBigIntRef and MutableBigIntRef, their contents can
  // also be swapp()ed with one another.
  MutableBigIntAndMaxSize runningSquare = runningSquare0;
  MutableBigIntAndMaxSize tmpRunningSquare = runningSquare1;
  MutableBigIntAndMaxSize result = dst;
  MutableBigIntAndMaxSize nextResult = tmpResult;

  // runningSquare is initialized to base, i.e., lhs ** 2 ** 0.
  auto res = initWithDigits(runningSquare, lhs);
  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return res;
  }

  // initialize the result. It is either 0 (if x_0 = 0), or lhs (otherwise).
  if ((exponent & 1) == 0) {
    result.ref().numDigits = 0;
  } else {
    res = initWithDigits(result, lhs);
    if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
      return res;
    }
  }

  // now iterate over the exponent until there are no more bits set in it.
  for (exponent >>= 1; exponent > 0; exponent >>= 1) {
    // runningSquare <<= 2;
    tmpRunningSquare.resetRefNumDigits();
    res = multiply(tmpRunningSquare, runningSquare, runningSquare);
    res = multiplyStatusToExponentiateStatus(
        res, tmpRunningSquare.getMaxDigits());
    tmpRunningSquare.swap(runningSquare);

    if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
      return res;
    }

    // include runningSquare in the result if lhs_i is 1.
    if ((exponent & 1) != 0) {
      nextResult.resetRefNumDigits();
      if (isZero(result)) {
        res = initWithDigits(nextResult, runningSquare);
      } else {
        res = multiply(nextResult, result, runningSquare);
        res =
            multiplyStatusToExponentiateStatus(res, nextResult.getMaxDigits());
      }
      if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
        return res;
      }
      nextResult.swap(result);
    }
  }

  res = OperationStatus::RETURNED;
  if (&result.ref() != &dst) {
    nextResult.resetRefNumDigits();
    res = initNonCanonicalWithReadOnlyBigInt(nextResult, result);
  }

  return res;
}
} // namespace

static_assert(
    static_cast<SignedBigIntDigitType>(BigIntMaxSizeInBits) > 0,
    "BigIntMaxSizeInBits overflow");

OperationStatus exponentiate(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  if (isNegative(rhs)) {
    return OperationStatus::NEGATIVE_EXPONENT;
  }

  const BigIntDigitType firstExponentDigit = rhs.numDigits ? rhs.digits[0] : 0;

  // Handle cases in the following order:

  // 1. If rhs is zero, the result is 1.
  // 2. If |lhs| < 2, we can efficiently calculate the result and permit
  //    arbitrarily large exponents.
  // 3. Ensure that the exponent fits in an uint32, which is required by helper
  //    functions used in subsequent cases, since larger exponents are
  //    guaranteed to produce a BigInt larger than the max size.
  // 4. If |lhs| == 2, use a fast-path for that.
  // 5. Fall back to the slow path.
  OperationStatus res = OperationStatus::RETURNED;
  if (isZero(rhs)) {
    // lhs ** 0 => 1, for all lhs
    // N.B.: JS defines 0n ** 0n == 1.
    if (dst.numDigits < 1) {
      res = OperationStatus::DEST_TOO_SMALL;
    } else {
      dst.numDigits = 1;
      dst.digits[0] = 1;
    }
  } else if (isZero(lhs)) {
    // 0 ** rhs => 0, for rhs > 0
    dst.numDigits = 0;
  } else if (dst.numDigits < 1) {
    // |lhs| != 0, rhs > 0 =>  |result| != 0, i.e., the result requires at least
    // one digit.
    res = OperationStatus::DEST_TOO_SMALL;
  } else if (compare(lhs, 1) == 0) {
    // 1 ** rhs => 1, for all rhs
    assert(rhs.numDigits > 0 && "should have handled 0n");
    dst.numDigits = 1;
    dst.digits[0] = 1;
  } else if (compare(lhs, -1) == 0) {
    // -1 ** rhs => 1, for even rhs, -1 for odd
    assert(rhs.numDigits > 0 && "should have handled 0n");
    dst.numDigits = 1;
    // Note that rhs > 0, therefore rhs % 2n === exponent % 2.
    dst.digits[0] = (firstExponentDigit % 2 == 0) ? 1ull : -1ull;
  } else if (rhs.numDigits > 1 || firstExponentDigit >= BigIntMaxSizeInBits) {
    // At this point, we know that |lhs| >= 2, so any rhs >= BigIntMaxSizeInBits
    // will result in a BigInt that is too large.
    // The above check, together with the static_assert below, also guarantees
    // that later cases will receive an exponent that fits in a uint32, since
    // exponentiation helper functions only accept uint32 exponents.
    static_assert(BigIntMaxSizeInBits <= std::numeric_limits<uint32_t>::max());
    res = OperationStatus::TOO_MANY_DIGITS;
  } else if (firstExponentDigit == 1) {
    // lhs ** 1n => lhs, for any lhs
    res = initWithDigits(dst, lhs);
  } else if (compare(lhs, 2) == 0) {
    // Fast-path for 2n ** rhs
    res = exponentiatePowerOf2(dst, firstExponentDigit);
  } else if (compare(lhs, -2) == 0) {
    // Fast-path for -2n ** rhs
    res = exponentiatePowerOf2(dst, firstExponentDigit);
    if (firstExponentDigit % 2 != 0) {
      llvh::APInt::tcNegate(dst.digits, dst.numDigits);
    }
  } else {
    // Slow path
    res = exponentiateSlowPath(dst, lhs, firstExponentDigit);
  }

  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return res;
  }

  ensureCanonicalResult(dst);
  return OperationStatus::RETURNED;
}

namespace {
using IsRightShiftFn = bool (*)(ImmutableBigIntRef);
using GetShiftAmntFn = uint32_t (*)(ImmutableBigIntRef);

/// Computes the size of \p lhs shiftOp \p shiftAmnt, where
///
///   shiftOp = >>, isRightShift
///   shiftOp = <<, !isRightShift
enum class ShiftOpIs { Left, Right };

static uint32_t shiftResultSizeImpl(
    ShiftOpIs shiftOp,
    ImmutableBigIntRef lhs,
    uint32_t shiftAmnt) {
  uint32_t extraDigits = 0;
  if (shiftOp == ShiftOpIs::Left) {
    // left shifts will preppend zero digits to the bigint, thus the result
    // should have extra bits (when compared to lhs)
    extraDigits = numDigitsForSizeInBits(shiftAmnt);
  }

  const uint32_t result = lhs.numDigits + extraDigits;
  assert(extraDigits <= result && "too many digits in result");
  return result;
}

static constexpr SignedBigIntDigitType MaxPositiveShiftAmountInBits =
    BigIntMaxSizeInBits;

static constexpr SignedBigIntDigitType MinNegativeShiftAmountInBits =
    -static_cast<SignedBigIntDigitType>(BigIntMaxSizeInBits);

// If any of the following static_asserts ever fail then getShiftAmountAndSign
// should return uint64_t.
static_assert(
    1 + MaxPositiveShiftAmountInBits < std::numeric_limits<uint32_t>::max(),
    "non negative shift amounts don't fit uint32_t");
static_assert(
    1 + MinNegativeShiftAmountInBits > std::numeric_limits<int32_t>::min(),
    "non negative shift amounts don't fit int32_t");

/// Returns the shift amount that \p shiftAmnt specifies, and a boolean
/// specifying whether \p shiftAmnt was negative or not. This effecitvely limits
/// shift amount to a single BigIntDigit, which shouldn't be a problem as long
/// as BigIntDigitType can represent
/// BigIntMaxSizeInDigits * BigIntDigitTypeSizeInBits + 1 -- which is ensured by
/// the static_asserts above.
static std::tuple<uint32_t, bool> getShiftAmountAndSign(
    ImmutableBigIntRef shiftAmnt) {
  // reallyLargeShiftAmount represents a shift amount that's either going to
  // produce a 0 result (in case of a signed right shift), or an impossibly
  // large one (i.e., one with over BigIntMaxSizeInDigits). It is used as the
  // return value when shiftAmnt is outsize
  // [MinNegativeShiftAmountInBits, MaxPositiveShiftAmountInBits].
  const BigIntDigitType reallyLargeShiftAmount =
      numDigitsForSizeInBytes(MaxPositiveShiftAmountInBits + 1);

  const bool shiftAmntIsNeg = isNegative(shiftAmnt);

  if (compare(shiftAmnt, MinNegativeShiftAmountInBits) < 0 ||
      compare(shiftAmnt, MaxPositiveShiftAmountInBits) > 0) {
    // shiftAmnt is outside of the
    // [MinNegativeShiftAmountInBits, MaxPositiveShiftAmountInBits]; thus return
    // a really large shift amount.
    return std::make_tuple(
        static_cast<uint32_t>(reallyLargeShiftAmount), shiftAmntIsNeg);
  }

  const SignedBigIntDigitType sa = (shiftAmnt.numDigits == 0)
      ? 0ll
      : static_cast<SignedBigIntDigitType>(shiftAmnt.digits[0]);
  assert(
      (shiftAmnt.numDigits == 0 ||
       shiftAmnt.digits[0] != std::numeric_limits<BigIntDigitType>::min()) &&
      "shiftAmnt is MIN_INT, hence -signedShiftAmnt is MIN_INT");
  // Always return a positive result -- thus negate sa if shiftAmnt is negative.
  return std::make_tuple(
      static_cast<uint32_t>(shiftAmntIsNeg ? -sa : sa), shiftAmntIsNeg);
}

} // namespace

uint32_t leftShiftResultSize(ImmutableBigIntRef lhs, ImmutableBigIntRef rhs) {
  const auto &[shiftAmnt, isNegative] = getShiftAmountAndSign(rhs);
  const auto shiftOp = isNegative ? ShiftOpIs::Right : ShiftOpIs::Left;
  return shiftResultSizeImpl(shiftOp, lhs, shiftAmnt);
}

uint32_t signedRightShiftResultSize(
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  const auto &[shiftAmnt, isNegative] = getShiftAmountAndSign(rhs);
  const auto shiftOp = isNegative ? ShiftOpIs::Left : ShiftOpIs::Right;
  return shiftResultSizeImpl(shiftOp, lhs, shiftAmnt);
}

namespace {
static std::tuple<uint32_t, ShiftOpIs, uint32_t>
getShiftAmountSignAndResultSize(
    ShiftOpIs shiftOp,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  const auto &[shiftAmnt, isNegative] = getShiftAmountAndSign(rhs);
  const auto actualShiftOp = (shiftOp == ShiftOpIs::Left) == isNegative
      ? ShiftOpIs::Right
      : ShiftOpIs::Left;

  return std::make_tuple(
      shiftAmnt,
      actualShiftOp,
      shiftResultSizeImpl(actualShiftOp, lhs, shiftAmnt));
}

/// An adapter for using APInt's usigned right shift as a signed right shift.
/// This is accomplished by **complementing** digits if it is a negative number
/// before and after tcShiftRight. The idea is that, for negative numbers
///
/// ~(~digits >>> shiftAmnt)
///
/// This works because the unsigned right shift operation shifts 0 in. For
/// example,
///
/// -4 >> 1 = 0b11111100 >> 1 = ~(~0b11111100 >> 1) = ~(0b00000011 >> 1) =
///         = ~(0b00000001) = 0b11111110 = -2 .
///
/// Intuitively, by using the negative number's complement, the 0 shifted in by
/// the >>> operator are the "complemented" sign. Curiously, using negation
/// does not work for any signed right shift's resultig in -1, e.g.,
///
/// -8 >> 1024 = - (8 >> 1024) = 0
void signedRightShiftAdapter(
    BigIntDigitType *digits,
    uint32_t numDigits,
    uint32_t shiftAmnt) {
  const bool dstNegative = isNegative(ImmutableBigIntRef{digits, numDigits});

  if (dstNegative) {
    llvh::APInt::tcComplement(digits, numDigits);
  }

  llvh::APInt::tcShiftRight(digits, numDigits, shiftAmnt);

  if (dstNegative) {
    llvh::APInt::tcComplement(digits, numDigits);
  }
}

/// A generic \p dst = \p lhs shiftOp \p shiftAmnt
OperationStatus shiftImpl(
    ShiftOpIs shiftOp,
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  auto [shiftAmnt, actualShiftOp, numDigits] =
      getShiftAmountSignAndResultSize(shiftOp, lhs, rhs);
  auto op = (actualShiftOp == ShiftOpIs::Right) ? signedRightShiftAdapter
                                                : llvh::APInt::tcShiftLeft;

  if (dst.numDigits < numDigits) {
    return OperationStatus::DEST_TOO_SMALL;
  }

  auto res = initNonCanonicalWithReadOnlyBigInt(dst, lhs);
  if (LLVM_UNLIKELY(res != OperationStatus::RETURNED)) {
    return res;
  }

  (*op)(dst.digits, dst.numDigits, shiftAmnt);

  ensureCanonicalResult(dst);
  return OperationStatus::RETURNED;
}
} // namespace

OperationStatus leftShift(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  return shiftImpl(ShiftOpIs::Left, dst, lhs, rhs);
}

OperationStatus signedRightShift(
    MutableBigIntRef dst,
    ImmutableBigIntRef lhs,
    ImmutableBigIntRef rhs) {
  return shiftImpl(ShiftOpIs::Right, dst, lhs, rhs);
}

} // namespace bigint
} // namespace hermes
