/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Platform/Unicode/CharacterProperties.h"
#include "hermes/Platform/Unicode/CodePointSet.h"
#include "hermes/Platform/Unicode/PlatformUnicode.h"
#include "hermes/Regex/RegexTraits.h"

#include "gtest/gtest.h"

#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace hermes;

namespace {

// This might look like a silly test, but it should be useful if we
// need to add tricky optimizations to isUnicodeOnlySpace
TEST(UnicodeTest, isUnicodeOnlySpace) {
  for (uint32_t i = 1; i < 128; i++) {
    EXPECT_FALSE(hermes::isUnicodeOnlySpace(i))
        << "No ASCII character is a unicode-only space. (code: " << i << ")";
  }

  std::vector<uint32_t> unicode_space_cps{
      0x1680,
      0x2000,
      0x2001,
      0x2002,
      0x2003,
      0x2004,
      0x2005,
      0x2006,
      0x2007,
      0x2008,
      0x2009,
      0x200A,
      0x202F,
      0x205F,
      0x3000};

  for (auto cp : unicode_space_cps) {
    EXPECT_TRUE(hermes::isUnicodeOnlySpace(cp))
        << "Code point " << cp << " is a unicode-only space.";
  }

  // Test for off-by-one errors.
  std::vector<uint32_t> unicode_non_space_cps{
      0x167f,
      0x1681,
      0x1fff,
      0x200B,
      0x202E,
      0x2030,
      0x205E,
      0x2060,
      0x2fff,
      0x3001};

  for (auto cp : unicode_non_space_cps) {
    EXPECT_FALSE(hermes::isUnicodeOnlySpace(cp))
        << "Code point " << cp << " is not a unicode-only space.";
  }
}

// Verify some correct canonicalizations.
TEST(UnicodeTest, LegacyU16Canonicalization) {
  regex::UTF16RegexTraits traits;
  auto canon = [&traits](char16_t c) {
    return traits.canonicalize(c, false /* not unicode */);
  };
  EXPECT_EQ('A', canon('a'));
  EXPECT_EQ('/', canon('/'));
  EXPECT_EQ(0x053D, canon(0x053D)); // "ARMENIAN CAPITAL LETTER XEH"
  EXPECT_EQ(0x053D, canon(0x056D)); // "ARMENIAN SMALL LETTER XEH"
  EXPECT_EQ(0xA78D, canon(0x0265)); // "LATIN CAPITAL LETTER TURNED H"
  EXPECT_EQ(0x042A,
            canon(0x042A)); // "CYRILLIC CAPITAL LETTER HARD SIGN"
  EXPECT_EQ(0x042A, canon(0x1C86)); // "CYRILLIC SMALL LETTER TALL HARD SIGN"
  EXPECT_EQ(0x042A, canon(0x044A)); // "CYRILLIC SMALL LETTER HARD SIGN"
  EXPECT_EQ(0x0422, canon(0x1C85)); // "CYRILLIC CAPITAL LETTER TE"
  EXPECT_EQ(0x0422, canon(0x1C84)); // "CYRILLIC CAPITAL LETTER TE"
  EXPECT_EQ(0x0422, canon(0x0442)); // "CYRILLIC SMALL LETTER TE"
  EXPECT_EQ(0x01CA, canon(0x01CA)); // "LATIN CAPITAL LETTER NJ"
  EXPECT_EQ(
      0x01CA,
      canon(0x01CB)); // "LATIN CAPITAL LETTER N WITH SMALL LETTER J"
  EXPECT_EQ(0x01CA, canon(0x01CC)); // "LATIN SMALL LETTER NJ"
  EXPECT_EQ(0x212B, canon(0x212B)); // "ANGSTROM SIGN"
}

TEST(UnicodeTest, ASCIICanonicalization) {
  regex::ASCIIRegexTraits traits;
  for (int c = 0; c <= 127; c++) {
    EXPECT_EQ(
        toupper(c), traits.canonicalize((char)c, false /* not Unicode */));
    EXPECT_EQ(tolower(c), traits.canonicalize((char)c, true /* Unicode */));
  }
}

static std::string flatten(const CodePointSet &cps) {
  std::string result;
  for (const auto &range : cps.ranges()) {
    EXPECT_GT(range.length, 0u);
    if (!result.empty())
      result += ", ";
    result += std::to_string(range.first);
    result += '-';
    result += std::to_string(range.end() - 1);
  }
  return result;
}

#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES
TEST(UnicodeTest, PropertyRangeEndpoints) {
  auto property = [](std::string_view name, bool inverted) {
    CodePointSet result;
    addRangeArrayPoolToBracket(
        &result, unicodePropertyRanges(name, ""), inverted);
    return result;
  };

  // Exercise a short range and the complete Unicode range, including its
  // inclusive upper endpoint. The latter needs the overflow representation.
  EXPECT_EQ("0-127", flatten(property("ASCII", false)));
  EXPECT_EQ("0-1114111", flatten(property("Any", false)));
  EXPECT_TRUE(property("Any", true).empty());
  auto nonAscii = property("ASCII", true);
  EXPECT_FALSE(nonAscii.contains(127));
  EXPECT_TRUE(nonAscii.contains(128));

  // Private-use ranges exercise long deltas and supplementary-plane starts.
  auto privateUse = property("Co", false);
  EXPECT_EQ(
      "57344-63743, 983040-1048573, 1048576-1114109", flatten(privateUse));
  auto nonPrivateUse = property("Co", true);
  for (uint32_t cp :
       {0xE000u, 0xF8FFu, 0xF0000u, 0xFFFFDu, 0x100000u, 0x10FFFDu}) {
    EXPECT_TRUE(privateUse.contains(cp));
    EXPECT_FALSE(nonPrivateUse.contains(cp));
  }
  for (uint32_t cp : {0xDFFFu, 0xF900u, 0xEFFFFu, 0xFFFFEu, 0x10FFFEu}) {
    EXPECT_FALSE(privateUse.contains(cp));
    EXPECT_TRUE(nonPrivateUse.contains(cp));
  }
}
#endif

TEST(UnicodeTest, CodePointSet) {
  EXPECT_EQ(flatten(CodePointSet{}), flatten(CodePointSet{}));

  CodePointSet cps;
  cps.add(10);
  EXPECT_EQ("10-10", flatten(cps));
  cps.add(5);
  EXPECT_EQ("5-5, 10-10", flatten(cps));

  cps.add(CodePointRange{2, 2});
  EXPECT_EQ("2-3, 5-5, 10-10", flatten(cps));

  cps.add(CodePointRange{3, 4});
  EXPECT_EQ("2-6, 10-10", flatten(cps));

  cps.add(CodePointRange{100, 1});
  EXPECT_EQ("2-6, 10-10, 100-100", flatten(cps));

  cps.add(CodePointRange{101, 1});
  EXPECT_EQ("2-6, 10-10, 100-101", flatten(cps));

  cps.add(CodePointRange{0, 1001});
  EXPECT_EQ("0-1000", flatten(cps));
}

TEST(UnicodeTest, AddLegacyCanonicalEquivalentCharacters) {
  auto flattenCanonically = [](const CodePointSet &cps) {
    return flatten(makeCanonicallyEquivalent(cps, false /* not unicode */));
  };

  CodePointSet cps;
  cps.add('&');
  EXPECT_EQ("38-38", flattenCanonically(cps));
  cps.add('A');
  EXPECT_EQ("38-38, 65-65, 97-97", flattenCanonically(cps));
  cps.add(CodePointRange{'A', 'Z' - 'A' + 1});
  EXPECT_EQ("38-38, 65-90, 97-122", flattenCanonically(cps));
  cps.add(0x01DF); // 479
  cps.add(0x01E3); // 483
  cps.add(0x01E6); // 486
  EXPECT_EQ(
      "38-38, 65-90, 97-122, 478-479, 482-483, 486-487",
      flattenCanonically(cps));
}

TEST(UnicodeTest, AddUnicodeCanonicalEquivalentCharacters) {
  auto flattenCanonically = [](const CodePointSet &cps) {
    return flatten(makeCanonicallyEquivalent(cps, true /* unicode */));
  };

  CodePointSet cps;
  cps.add('&');
  EXPECT_EQ("38-38", flattenCanonically(cps));
  cps.add('A');
  EXPECT_EQ("38-38, 65-65, 97-97", flattenCanonically(cps));
  cps.add(CodePointRange{'A', 'Z' - 'A' + 1});
  EXPECT_EQ(
      "38-38, 65-90, 97-122, 383-383, 8490-8490", flattenCanonically(cps));
  cps.add(0x01DF); // 479
  cps.add(0x01E3); // 483
  cps.add(0x01E6); // 486
  EXPECT_EQ(
      "38-38, 65-90, 97-122, 383-383, 478-479, 482-483, 486-487, 8490-8490",
      flattenCanonically(cps));
}

} // end anonymous namespace
