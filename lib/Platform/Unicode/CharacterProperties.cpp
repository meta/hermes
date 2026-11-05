/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Platform/Unicode/CharacterProperties.h"
#include "hermes/Platform/Unicode/CodePointSet.h"

#include <algorithm>
#include <array>
#include <climits>
#include <iterator>
#include <string>
#include <utility>

namespace hermes {

#include "UnicodeData.inc"

namespace {
struct UnicodeRangeComp {
  bool operator()(UnicodeRange p, uint32_t s) const {
    return p.second < s;
  }
  bool operator()(uint32_t s, UnicodeRange p) const {
    return s < p.first;
  }
};
} // namespace

template <typename UnicodeRangeTable>
inline bool lookup(const UnicodeRangeTable &table, const uint32_t cp) {
  if constexpr (sizeof(table) == 0) {
    // If the array is empty, std::begin doesn't work (fails to compile).
    return false;
  } else {
    return std::binary_search(
        std::begin(table), std::end(table), cp, UnicodeRangeComp());
  }
}

bool isUnicodeOnlyLetter(uint32_t cp) {
  // "any character in the Unicode categories “Uppercase letter (Lu)”,
  // “Lowercase letter (Ll)”, “Titlecase letter (Lt)”, “Modifier letter (Lm)”,
  // “Other letter (Lo)”, or “Letter number (Nl)”".
  // ASCII characters are not "UnicodeOnly" and so we return false.
  if (cp <= 0x7F)
    return false;

  return lookup(UNICODE_LETTERS, cp);
}

bool isUnicodeOnlyIDStart(uint32_t cp) {
  // "any character in the Unicode categories “Uppercase letter (Lu)”,
  // “Lowercase letter (Ll)”, “Titlecase letter (Lt)”, “Modifier letter (Lm)”,
  // “Other letter (Lo)”, or “Letter number (Nl)”".
  // ASCII characters are not "UnicodeOnly" and so we return false.
  if (cp <= 0x7F)
    return false;

  // Unicode spec defines ID_Start as:
  //     Lu + Ll + Lt + Lm + Lo + Nl
  //   + Other_ID_Start
  //   - Pattern_Syntax
  //   - Pattern_White_Space
  // So check this by checking all UNICODE_LETTERS that aren't
  // UNICODE_PATTERN_LETTER, and then check for Other_ID_Start.
  //
  // UNICODE_PATTERN_LETTER is all ther Pattern_White_Space and Pattern_Syntax
  // that are also in the Letter categories.

  return (lookup(UNICODE_LETTERS, cp) && !lookup(UNICODE_PATTERN_LETTER, cp)) ||
      lookup(UNICODE_OTHER_ID_START, cp);
}

bool isUnicodeOnlyIDContinue(uint32_t cp) {
  // ASCII characters are not "UnicodeOnly" and so we return false.
  if (cp <= 0x7F)
    return false;

  // Unicode spec defines ID_Continue as:
  // #  Generated from:
  //     ID_Start
  //   + Mn + Mc + Nd + Pc
  //   + Other_ID_Continue
  //   - Pattern_Syntax
  //   - Pattern_White_Space
  //
  // UNICODE_PATTERN_CONTINUE is all the Pattern_White_Space and Pattern_Syntax
  // that are also in the Mn, Mc, Nd, Pc categories.

  return isUnicodeOnlyIDStart(cp) ||
      ((lookup(UNICODE_COMBINING_MARK, cp) || lookup(UNICODE_DIGIT, cp) ||
        lookup(UNICODE_CONNECTOR_PUNCTUATION, cp)) &&
       !lookup(UNICODE_PATTERN_CONTINUE, cp)) ||
      lookup(UNICODE_OTHER_ID_CONTINUE, cp);
}

// Special cased due to small number of separate values.
bool isUnicodeOnlySpace(uint32_t cp) {
  // "Other category “Zs”: Any other Unicode “space separator”"
  // Exclude ASCII.
  if (cp <= 0x7F)
    return false;

  switch (cp) {
    case 0xa0:
    case 0x1680:
    case 0x2000:
    case 0x2001:
    case 0x2002:
    case 0x2003:
    case 0x2004:
    case 0x2005:
    case 0x2006:
    case 0x2007:
    case 0x2008:
    case 0x2009:
    case 0x200a:
    case 0x202f:
    case 0x205f:
    case 0x3000:
      return true;
    default:
      return false;
  }
}

bool isUnicodeCombiningMark(uint32_t cp) {
  // "any character in the Unicode categories “Non-spacing mark (Mn)” or
  // “Combining spacing mark (Mc)”"
  return lookup(UNICODE_COMBINING_MARK, cp);
}

bool isUnicodeDigit(uint32_t cp) {
  // "any character in the Unicode category “Decimal number (Nd)”"
  // 0-9 is the common case.
  return (cp >= '0' && cp <= '9') || lookup(UNICODE_DIGIT, cp);
};

bool isUnicodeConnectorPunctuation(uint32_t cp) {
  // "any character in the Unicode category “Connector punctuation (Pc)"
  // _ is the common case.
  return cp == '_' || lookup(UNICODE_CONNECTOR_PUNCTUATION, cp);
}

static uint32_t applyTransform(const UnicodeTransformRange &r, uint32_t cp) {
  assert(
      r.start <= cp && cp < r.start + r.count &&
      "range does not contain this cp");
  assert(cp <= INT32_MAX && "cp is too big");
  // Check if our code point has a mod of 0.
  if ((cp - r.start) % r.modulo == 0) {
    int32_t cps = static_cast<int32_t>(cp) + r.delta;
    assert(cps >= 0 && "delta underflowed");
    return static_cast<uint32_t>(cps);
  }
  return cp;
}

// Predicate used to enable binary search on UnicodeTransformRange.
// Here we return true if the last element of the range is < cp, so <= is
// correct.
static bool operator<(const UnicodeTransformRange &m, uint32_t cp) {
  return m.start + m.count <= cp;
}

/// Find all code points which canonicalize to a value in \p range, and add them
/// to \p receiver. This is a slow linear search across all ranges.
static void addPrecanonicalCharacters(
    CodePointRange range,
    CodePointSet *receiver,
    bool unicode) {
  if (range.length == 0)
    return;
  // TODO: if range is ASCII and unicode is not set, we can stop the search
  // after the ASCII part as nothing outside ASCII can canonicalize to an ASCII
  // character.
  const auto start =
      unicode ? std::begin(UNICODE_FOLDS) : std::begin(LEGACY_CANONS);
  const auto end = unicode ? std::end(UNICODE_FOLDS) : std::end(LEGACY_CANONS);
  for (auto iter = start; iter != end; ++iter) {
    const UnicodeTransformRange &transform = *iter;
    // Get the range of transformed-from and transformed-to characters.
    CodePointRange fromRange{transform.start, transform.count};
    CodePointRange toRange = fromRange;
    toRange.first += transform.delta;

    // See if some character in our range will be transformed-to.
    if (!range.overlaps(toRange))
      continue;

    // Looks like it. Add everything.
    for (uint32_t cp = fromRange.first; cp < fromRange.end(); cp++) {
      uint32_t tcp = applyTransform(transform, cp);
      if (tcp != cp && range.first <= tcp && tcp < range.end()) {
        receiver->add(cp);
      }
    }
  }
}

/// For each code point in \p range, canonicalize it and add the canonicalized
/// values to \p receiver.
static void
canonicalizeRange(CodePointRange range, CodePointSet *receiver, bool unicode) {
  assert(range.length > 0 && "Range should never be empty");

  /// Find the first transform that contains or starts after our range.
  const auto start =
      unicode ? std::begin(UNICODE_FOLDS) : std::begin(LEGACY_CANONS);
  const auto end = unicode ? std::end(UNICODE_FOLDS) : std::end(LEGACY_CANONS);
  auto transform = std::lower_bound(start, end, range.first);

  uint32_t curcp = range.first;
  uint32_t endcp = range.end();
  while (curcp < endcp && transform != end) {
    uint32_t transformEnd = transform->start + transform->count;
    assert(transformEnd > curcp && "transform should end after our current cp");
    if (transform->start > curcp) {
      // Our transform started after the current value, so skip to the
      // transform.
      curcp = transform->start;
    } else {
      // Apply this transform for the code points that are in the transform and
      // also in our range.
      for (; curcp < transformEnd && curcp < endcp; curcp++) {
        receiver->add(applyTransform(*transform, curcp));
      }
      // We have now exhausted this transform; go to the next one.
      ++transform;
    }
  }
}

CodePointSet makeCanonicallyEquivalent(const CodePointSet &set, bool unicode) {
  // Canonicalize all characters in the set, and then find all characters which
  // canonicalize to some element in the set.
  CodePointSet canonicalized = set;
  for (const auto &range : set.ranges()) {
    canonicalizeRange(range, &canonicalized, unicode);
  }

  CodePointSet result = canonicalized;
  for (const auto &range : canonicalized.ranges()) {
    addPrecanonicalCharacters(range, &result, unicode);
  }
  return result;
}

uint32_t canonicalize(uint32_t cp, bool unicode) {
  const auto start =
      unicode ? std::begin(UNICODE_FOLDS) : std::begin(LEGACY_CANONS);
  const auto end = unicode ? std::end(UNICODE_FOLDS) : std::end(LEGACY_CANONS);
  auto where = std::lower_bound(start, end, cp);
  if (where != end && where->start <= cp && cp < where->start + where->count) {
    return applyTransform(*where, cp);
  } else {
    // No transform for this character, so it canonicalizes to itself.
    return cp;
  }
}

#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES

/// Find a matching entry (such as \p NameMapEntry or \p RangeMapEntry) by
/// matching a string \p name against the entry's \p name field.
template <class T>
static const T *findMapEntry(
    const llvh::ArrayRef<T> &arrayRef,
    const std::string_view name) {
  auto it = std::lower_bound(
      std::begin(arrayRef),
      std::end(arrayRef),
      name,
      [](const T &a, std::string_view b) {
        return UNICODE_DATA_STRING_POOL.compare(a.name.offset, a.name.size, b) <
            0;
      });
  if (it == std::end(arrayRef) ||
      UNICODE_DATA_STRING_POOL.compare(it->name.offset, it->name.size, name) !=
          0) {
    return nullptr;
  }
  return it;
}

llvh::ArrayRef<UnicodeRangePoolRef> unicodePropertyRanges(
    std::string_view propertyName,
    std::string_view propertyValue) {
  const NameMapEntry *canonicalNameEntry;
  llvh::ArrayRef<RangeMapEntry> rangeMap;

  if (propertyValue.empty()) {
    // There was no property value, this is either a binary property or a value
    // from General_Category, as per `LoneUnicodePropertyNameOrValue`.
    if ((canonicalNameEntry = findMapEntry(
             llvh::ArrayRef(canonicalPropertyNameMap_BinaryProperty),
             propertyName))) {
      rangeMap = unicodePropertyRangeMap_BinaryProperty;
    } else if ((canonicalNameEntry = findMapEntry(
                    llvh::ArrayRef(canonicalPropertyNameMap_GeneralCategory),
                    propertyName))) {
      rangeMap = unicodePropertyRangeMap_GeneralCategory;
    }
  } else {
    // There was a property value, assume the name is a category.
    if ((propertyName == "General_Category" || propertyName == "gc") &&
        (canonicalNameEntry = findMapEntry(
             llvh::ArrayRef(canonicalPropertyNameMap_GeneralCategory),
             propertyValue))) {
      rangeMap = unicodePropertyRangeMap_GeneralCategory;
    } else if (
        (propertyName == "Script" || propertyName == "sc") &&
        (canonicalNameEntry = findMapEntry(
             llvh::ArrayRef(canonicalPropertyNameMap_Script), propertyValue))) {
      rangeMap = unicodePropertyRangeMap_Script;
    } else if (
        (propertyName == "Script_Extensions" || propertyName == "scx") &&
        // Since Script_Extensions is a superset of Script, they share
        // a name map.
        (canonicalNameEntry = findMapEntry(
             llvh::ArrayRef(canonicalPropertyNameMap_Script), propertyValue))) {
      rangeMap = unicodePropertyRangeMap_ScriptExtensions;
    } else {
      return llvh::ArrayRef<UnicodeRangePoolRef>();
    }
  }

  if (canonicalNameEntry == nullptr) {
    return llvh::ArrayRef<UnicodeRangePoolRef>();
  }

  // Look up the range arrays for the property.
  auto canonicalName = UNICODE_DATA_STRING_POOL.substr(
      canonicalNameEntry->canonical.offset, canonicalNameEntry->canonical.size);
  auto rangeMapEntry = findMapEntry(rangeMap, canonicalName);
  if (rangeMapEntry == nullptr) {
    if (rangeMap.data() == unicodePropertyRangeMap_ScriptExtensions) {
      // Script_Extensions is a superset of Script. If a script has no explicit
      // Script_Extensions entries, fall back to the Script ranges and try
      // again.
      rangeMapEntry = findMapEntry(
          llvh::ArrayRef(unicodePropertyRangeMap_Script), canonicalName);
      if (rangeMapEntry == nullptr) {
        return llvh::ArrayRef<UnicodeRangePoolRef>();
      }
    } else {
      return llvh::ArrayRef<UnicodeRangePoolRef>();
    }
  }

  return llvh::ArrayRef{
      &UNICODE_RANGE_ARRAY_POOL[rangeMapEntry->rangeArrayPoolOffset],
      rangeMapEntry->rangeArraySize};
}

/// Decode one range from the property pool without allocating storage.
static UnicodeRange decodeUnicodeRange(uint32_t packed) {
  const uint32_t delta = packed & UNICODE_RANGE_POOL_DELTA_MASK;
  const uint32_t startOrIndex = packed >> UNICODE_RANGE_POOL_DELTA_BITS;
  if (delta == UNICODE_RANGE_POOL_DELTA_MASK)
    return UNICODE_RANGE_POOL.overflow[startOrIndex];
  return {startOrIndex, startOrIndex + delta};
}

void addRangeArrayPoolToBracket(
    CodePointSet *receiver,
    const llvh::ArrayRef<UnicodeRangePoolRef> rangeArrayPool,
    bool inverted) {
  auto addAllRanges = [&rangeArrayPool](CodePointSet *target) {
    for (auto rangePoolRef : rangeArrayPool) {
      auto rangePool = llvh::ArrayRef<uint32_t>{
          &UNICODE_RANGE_POOL.ranges[rangePoolRef.offset], rangePoolRef.size};
      for (auto packed : rangePool) {
        const auto range = decodeUnicodeRange(packed);
        const uint32_t length = range.second - range.first + 1;
        target->add(CodePointRange{range.first, length});
      }
    }
  };

  if (inverted) {
    CodePointSet positive;
    addAllRanges(&positive);
    positive.invert(UNICODE_MAX_VALUE);
    for (const auto &range : positive.ranges())
      receiver->add(range);
  } else {
    addAllRanges(receiver);
  }
}

#else

llvh::ArrayRef<UnicodeRangePoolRef> unicodePropertyRanges(
    std::string_view propertyName,
    std::string_view propertyValue) {
  return llvh::ArrayRef<UnicodeRangePoolRef>();
}

void addRangeArrayPoolToBracket(
    CodePointSet *receiver,
    const llvh::ArrayRef<UnicodeRangePoolRef> rangeArrayPool,
    bool inverted) {}

#endif

} // namespace hermes
