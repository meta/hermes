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

#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES

static CodePointSet propertySet(
    std::string_view name,
    std::string_view value = "",
    bool inverted = false) {
  CodePointSet result;
  addRangeArrayPoolToBracket(
      &result, unicodePropertyRanges(name, value), inverted);
  return result;
}

TEST(UnicodeTest, PropertyRangeBoundaries) {
  EXPECT_EQ("0-127", flatten(propertySet("ASCII")));
  EXPECT_EQ("0-1114111", flatten(propertySet("Any")));
  auto letters = propertySet("L");
  EXPECT_TRUE(letters.contains('A'));
  EXPECT_FALSE(letters.contains('0'));
  EXPECT_TRUE(letters.contains(0x10400));
  EXPECT_FALSE(letters.contains(UNICODE_MAX_VALUE));
}

TEST(UnicodeTest, InvertedPropertyRangeBoundaries) {
  auto inverted = propertySet("ASCII", "", true);
  EXPECT_FALSE(inverted.contains(0));
  EXPECT_FALSE(inverted.contains(127));
  EXPECT_TRUE(inverted.contains(128));
  EXPECT_TRUE(inverted.contains(0x10400));
  EXPECT_TRUE(inverted.contains(UNICODE_MAX_VALUE));
}

TEST(UnicodeTest, CompoundPropertyRangeResets) {
  CodePointSet letters;
  for (auto category : {"Ll", "Lt", "Lu", "Lm", "Lo"}) {
    addRangeArrayPoolToBracket(
        &letters, unicodePropertyRanges(category, ""), false);
  }
  EXPECT_EQ(flatten(letters), flatten(propertySet("L")));
  EXPECT_EQ(flatten(propertySet("Cn")), flatten(propertySet("Script", "Zzzz")));
}

TEST(UnicodeTest, ScriptExtensionRangeResets) {
  EXPECT_FALSE(propertySet("Script", "Hira").contains(0x30A0));
  auto hiragana = propertySet("Script_Extensions", "Hira");
  EXPECT_TRUE(hiragana.contains(0x30A0));
  EXPECT_TRUE(hiragana.contains(0x3042));
  EXPECT_FALSE(hiragana.contains('A'));

  auto latin = propertySet("Script_Extensions", "Latin");
  auto inverted = propertySet("Script_Extensions", "Latin", true);
  EXPECT_TRUE(latin.contains(0x202F));
  EXPECT_FALSE(inverted.contains(0x202F));
  EXPECT_TRUE(latin.contains('A'));
  EXPECT_FALSE(inverted.contains('A'));
  EXPECT_FALSE(latin.contains(0x4F60));
  EXPECT_TRUE(inverted.contains(0x4F60));
  EXPECT_TRUE(inverted.contains(UNICODE_MAX_VALUE));
}

#endif

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
