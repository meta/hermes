/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Regex/Regex.h"
#include "hermes/Regex/Executor.h"
#include "hermes/Regex/RegexTraits.h"

#include <cstdio>
#include <cstring>
#include <string>
#include "gtest/gtest.h"

using namespace hermes::regex;

namespace {

using cregex = Regex<UTF16RegexTraits>;
using cmatch = std::vector<CapturedRange>;

bool search(
    const char16_t *start,
    const char16_t *end,
    cmatch &m,
    const cregex &cr,
    constants::MatchFlagType flags = constants::matchDefault) {
  return searchWithBytecode(cr.compile(), start, 0, end - start, &m, flags) ==
      MatchRuntimeResult::Match;
}

bool search(
    const char16_t *str,
    cmatch &m,
    const cregex &cr,
    constants::MatchFlagType flags = constants::matchDefault) {
  return search(
      str, str + std::char_traits<char16_t>::length(str), m, cr, flags);
}

bool search(
    const std::u16string &text,
    cmatch &m,
    const cregex &cr,
    constants::MatchFlagType flags = constants::matchDefault) {
  return search(text.c_str(), m, cr, flags);
}

// Flatten a list of captured ranges into a string representation like:
// "(0-5) (1-3)"
std::string flatten(const std::vector<CapturedRange> &ranges) {
  std::string result;
  for (const auto &range : ranges) {
    if (!result.empty())
      result += " ";
    if (!range.matched()) {
      result += "nm";
    } else {
      result += '(';
      result += std::to_string(range.start);
      result += '-';
      result += std::to_string(range.end);
      result += ')';
    }
  }
  return result;
}

TEST(Regex, Short) {
  std::u16string pattern = u"a([0-9]+)b";
  cregex reg(pattern.c_str());
  std::vector<CapturedRange> matchRanges;

  const std::u16string text1 = u"a0b";
  EXPECT_TRUE(search(text1, matchRanges, reg, constants::matchDefault));
  EXPECT_EQ("(0-3) (1-2)", flatten(matchRanges));

  const std::u16string text2 = u"a1234b";
  EXPECT_TRUE(search(text2, matchRanges, reg, constants::matchDefault));
  EXPECT_EQ("(0-6) (1-5)", flatten(matchRanges));
}

MatchRuntimeResult searchResult(
    const char16_t *text,
    const char16_t *pattern,
    const char16_t *flags = u"") {
  cmatch m;
  return searchWithBytecode(
      cregex(pattern, flags).compile(),
      text,
      0,
      std::char_traits<char16_t>::length(text),
      &m,
      constants::matchDefault);
}

#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES
TEST(Regex, PackedUnicodePropertyRanges) {
  // Exercise inline and overflow ranges through parsing, bytecode generation,
  // and execution. Explicit lengths preserve the NUL character.
  const std::u16string inputs[] = {
      std::u16string(1, 0),
      u"\x7f",
      u"\x80",
      u"\ue000",
      u"\uf8ff",
      u"\U000f0000",
      u"\U000ffffd",
      u"\U00100000",
      u"\U0010fffd",
      u"\U0010ffff"};
  const bool isAscii[] = {
      true, true, false, false, false, false, false, false, false, false};
  const bool isPrivateUse[] = {
      false, false, false, true, true, true, true, true, true, false};
  for (size_t i = 0; i < std::size(inputs); ++i) {
    SCOPED_TRACE(i);
    cmatch matches;
    const auto *start = inputs[i].data();
    const auto *end = start + inputs[i].size();
    EXPECT_TRUE(search(start, end, matches, cregex(u"^\\p{Any}$", u"u")));
    EXPECT_EQ(
        isAscii[i], search(start, end, matches, cregex(u"^\\p{ASCII}$", u"u")));
    EXPECT_EQ(
        isPrivateUse[i],
        search(start, end, matches, cregex(u"^\\p{Co}$", u"u")));
    EXPECT_EQ(
        !isPrivateUse[i],
        search(start, end, matches, cregex(u"^\\P{Co}$", u"u")));
  }

  cmatch matches;
  EXPECT_TRUE(search(
      u"Az\ue000\U00100000\u03a9",
      matches,
      cregex(u"^(\\p{ASCII}{2})(\\p{Co}{2})(\\P{ASCII})$", u"u")));
  EXPECT_EQ("(0-6) (0-2) (2-5) (5-6)", flatten(matches));
}
#endif

TEST(Regex, ExecFail) {
  EXPECT_EQ(
      searchResult(u"", u"(){999999999}"), MatchRuntimeResult::StackOverflow);
  EXPECT_EQ(
      searchResult(u"", u"(?=^){999999999}"),
      MatchRuntimeResult::StackOverflow);
}

/* The following tests are adapted from re.alg.search/ecma.pass.cpp in libc++ */
TEST(Regex, FromLibCXX) {
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"a")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ab";
    EXPECT_TRUE(search(s, m, cregex(u"ab")));
    EXPECT_EQ("(0-2)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ab";
    EXPECT_FALSE(search(s, m, cregex(u"ba")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"aab";
    EXPECT_TRUE(search(s, m, cregex(u"ab")));
    EXPECT_EQ("(1-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcd";
    EXPECT_TRUE(search(s, m, cregex(u"bc")));
    EXPECT_EQ("(1-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab*c")));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ababc";
    EXPECT_TRUE(search(s, m, cregex(u"(ab)*c")));
    EXPECT_EQ("(0-5) (2-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcdefghijk";
    EXPECT_TRUE(search(s, m, cregex(u"cd((e)fg)hi")));
    EXPECT_EQ("(2-9) (4-7) (4-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcd";
    EXPECT_TRUE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"aabc";
    EXPECT_FALSE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"efabc";
    EXPECT_TRUE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("(2-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"efabcg";
    EXPECT_FALSE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"acc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"acc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcdef";
    EXPECT_TRUE(search(s, m, cregex(u"(.*).*")));
    EXPECT_EQ("(0-6) (0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"bc";
    EXPECT_TRUE(search(s, m, cregex(u"(a*)*")));
    EXPECT_EQ("(0-0) nm", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ("(0-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbbbc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adec";
    EXPECT_FALSE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefgc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefghc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefghic";
    EXPECT_FALSE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"tournament";
    EXPECT_TRUE(search(s, m, cregex(u"tour|to|tournament")));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"tournamenttotour";
    EXPECT_TRUE(search(s, m, cregex(u"(tour|to|tournament)+")));
    EXPECT_EQ("(0-4) (0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ttotour";
    EXPECT_TRUE(search(s, m, cregex(u"(tour|to|t)+")));
    EXPECT_EQ("(0-7) (3-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"-ab,ab-";
    EXPECT_FALSE(search(s, m, cregex(u"-(.*),\1-")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"-ab,ab-";
    EXPECT_TRUE(search(s, m, cregex(u"-.*,.*-")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"^[a]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"^[ab]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"c";
    EXPECT_TRUE(search(s, m, cregex(u"^[a-f]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"g";
    EXPECT_FALSE(search(s, m, cregex(u"^[a-f]$")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Iraqi";
    EXPECT_TRUE(search(s, m, cregex(u"q[^u]")));
    EXPECT_EQ("(3-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Iraq";
    EXPECT_FALSE(search(s, m, cregex(u"q[^u]")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"01a45cef9";
    EXPECT_TRUE(search(s, m, cregex(u"[ace1-9]*")));
    EXPECT_EQ("(0-0)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"01a45cef9";
    EXPECT_TRUE(search(s, m, cregex(u"[ace1-9]+")));
    EXPECT_EQ("(1-7)", flatten(m));
  }
  {
    const char16_t r[] = u"^[-+]?[0-9]+[CF]$";
    std::ptrdiff_t sr = std::char_traits<char16_t>::length(r);
    using FI = const char16_t *;
    using BI = const char16_t *;
    cregex regex(llvh::ArrayRef<char16_t>(FI(r), FI(r + sr)));
    cmatch m;
    const char16_t s[] = u"-40C";
    std::ptrdiff_t ss = std::char_traits<char16_t>::length(s);
    EXPECT_TRUE(search(BI(s), BI(s + ss), m, regex));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Jeff Jeffs u";
    EXPECT_TRUE(search(s, m, cregex(u"Jeff(?=s\\b)")));
    EXPECT_EQ("(5-9)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Jeffs Jeff";
    EXPECT_TRUE(search(s, m, cregex(u"Jeff(?!s\\b)")));
    EXPECT_EQ("(6-10)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"5%k";
    EXPECT_TRUE(search(s, m, cregex(u"\\d[\\W]k")));
    EXPECT_EQ("(0-3)", flatten(m));
  }

  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"a")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ab";
    EXPECT_TRUE(search(s, m, cregex(u"ab")));
    EXPECT_EQ("(0-2)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ab";
    EXPECT_FALSE(search(s, m, cregex(u"ba")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"aab";
    EXPECT_TRUE(search(s, m, cregex(u"ab")));
    EXPECT_EQ("(1-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcd";
    EXPECT_TRUE(search(s, m, cregex(u"bc")));
    EXPECT_EQ("(1-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab*c")));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ababc";
    EXPECT_TRUE(search(s, m, cregex(u"(ab)*c")));
    EXPECT_EQ("(0-5) (2-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcdefghijk";
    EXPECT_TRUE(search(s, m, cregex(u"cd((e)fg)hi")));
    EXPECT_EQ("(2-9) (4-7) (4-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcd";
    EXPECT_TRUE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"aabc";
    EXPECT_FALSE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"efabc";
    EXPECT_TRUE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("(2-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"efabcg";
    EXPECT_FALSE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"acc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"acc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcdef";
    EXPECT_TRUE(search(s, m, cregex(u"(.*).*")));
    EXPECT_EQ("(0-6) (0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"bc";
    EXPECT_TRUE(search(s, m, cregex(u"(a*)*")));
    EXPECT_EQ("(0-0) nm", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbbbc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adec";
    EXPECT_FALSE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefgc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefghc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefghic";
    EXPECT_FALSE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"tournament";
    EXPECT_TRUE(search(s, m, cregex(u"tour|to|tournament")));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"tournamenttotour";
    EXPECT_TRUE(search(s, m, cregex(u"(tour|to|tournament)+")));
    EXPECT_EQ("(0-4) (0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ttotour";
    EXPECT_TRUE(search(s, m, cregex(u"(tour|to|t)+")));
    EXPECT_EQ("(0-7) (3-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"-ab,ab-";
    EXPECT_FALSE(search(s, m, cregex(u"-(.*),\1-")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"-ab,ab-";
    EXPECT_TRUE(search(s, m, cregex(u"-.*,.*-")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"^[a]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"^[ab]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"c";
    EXPECT_TRUE(search(s, m, cregex(u"^[a-f]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"g";
    EXPECT_FALSE(search(s, m, cregex(u"^[a-f]$")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Iraqi";
    EXPECT_TRUE(search(s, m, cregex(u"q[^u]")));
    EXPECT_EQ("(3-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Iraq";
    EXPECT_FALSE(search(s, m, cregex(u"q[^u]")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"01a45cef9";
    EXPECT_TRUE(search(s, m, cregex(u"[ace1-9]*")));
    EXPECT_EQ("(0-0)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"01a45cef9";
    EXPECT_TRUE(search(s, m, cregex(u"[ace1-9]+")));
    EXPECT_EQ("(1-7)", flatten(m));
  }
  {
    const char16_t r[] = u"^[-+]?[0-9]+[CF]$";
    std::ptrdiff_t sr = std::char_traits<char16_t>::length(r);
    using FI = const char16_t *;
    using BI = const char16_t *;
    cregex regex(llvh::ArrayRef<char16_t>(FI(r), FI(r + sr)));
    cmatch m;
    const char16_t s[] = u"-40C";
    std::ptrdiff_t ss = std::char_traits<char16_t>::length(s);
    EXPECT_TRUE(search(BI(s), BI(s + ss), m, regex));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Jeff Jeffs u";
    EXPECT_TRUE(search(s, m, cregex(u"Jeff(?=s\\b)")));
    EXPECT_EQ("(5-9)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Jeffs Jeff";
    EXPECT_TRUE(search(s, m, cregex(u"Jeff(?!s\\b)")));
    EXPECT_EQ("(6-10)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"5%k";
    EXPECT_TRUE(search(s, m, cregex(u"\\d[\\W]k")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
}

static constants::ErrorType error_for(
    const char16_t *pattern,
    const char16_t *flags = u"") {
  return cregex(pattern, flags).getError();
};

TEST(Regex, Invalid) {
  using namespace constants;
  EXPECT_EQ(error_for(u""), ErrorType::None);
  EXPECT_EQ(error_for(u"abc\\"), ErrorType::EscapeIncomplete);
  EXPECT_EQ(error_for(u"abc\\5"), ErrorType::None);
  EXPECT_EQ(error_for(u"abc\\5def"), ErrorType::None);
  EXPECT_EQ(error_for(u"abc\\99999999999999999999999999def"), ErrorType::None);
  EXPECT_EQ(error_for(u"abc[def"), ErrorType::UnbalancedBracket);
  EXPECT_EQ(error_for(u"abc(def"), ErrorType::UnbalancedParenthesis);
  EXPECT_EQ(error_for(u"abc{3"), ErrorType::None);
  EXPECT_EQ(error_for(u"abc{3,e}"), ErrorType::None);
  EXPECT_EQ(error_for(u"{3,,5}"), ErrorType::None);
  EXPECT_EQ(error_for(u"{3,5}"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"abc{10,3}"), ErrorType::BraceRange);
  EXPECT_EQ(error_for(u"abc[b-a]"), ErrorType::CharacterRange);
  EXPECT_EQ(error_for(u"(*)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"(+)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"({1})"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"(?)"), ErrorType::InvalidFlags);
  EXPECT_EQ(error_for(u"abc", u"a"), ErrorType::InvalidFlags);
  EXPECT_EQ(error_for(u"abc", u"gg"), ErrorType::InvalidFlags);
  EXPECT_EQ(error_for(u"(?=a){1}", u"u"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"(?<=a){1}", u"u"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"(?=a){1}"), ErrorType::None);
  EXPECT_EQ(error_for(u"(?!a){1}"), ErrorType::None);
  EXPECT_EQ(error_for(u"(?<=a){1}"), ErrorType::InvalidRepeat);
}

TEST(Regex, InvalidFromLibCXX) {
  using namespace constants;
  const char16_t *pat1 = u"a(b)c\\1234";
  EXPECT_EQ(
      cregex(llvh::ArrayRef<char16_t>(pat1, pat1 + 7)).getError(),
      ErrorType::None);

  EXPECT_EQ(error_for(u"[\\a]"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\a"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\"), ErrorType::EscapeIncomplete);
  EXPECT_EQ(error_for(u"[\\e]"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\e"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\c:]"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\c:"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\c"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\cA]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\CA]"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\cA"), ErrorType::None);

  EXPECT_EQ(error_for(u"?a"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"*a"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"+a"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"{a"), ErrorType::None);

  EXPECT_EQ(error_for(u"?(a+)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"*(a+)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"+(a+)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"{(a+)"), ErrorType::None);
}

TEST(RegExp, RangeChecks) {
  using namespace constants;
  EXPECT_EQ(error_for(u"[a-b]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[b-a]"), ErrorType::CharacterRange);
  EXPECT_EQ(error_for(u"[\\uD800-\\uDFFF]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\uDFFF-\\uD800]"), ErrorType::CharacterRange);
  // In the following we are required to interpret - as a ClassAtom and not
  // part of a range.
  EXPECT_EQ(error_for(u"[a\\s-b]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[x-\\S]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\d-\\S]"), ErrorType::None);
}

static bool regexIsAnchored(
    const char16_t *pattern,
    const char16_t *flags = u"") {
  return cregex(pattern, flags).matchConstraints() &
      MatchConstraintAnchoredAtStart;
}

static bool regexIsNonASCII(
    const char16_t *pattern,
    const char16_t *flags = u"") {
  return cregex(pattern, flags).matchConstraints() & MatchConstraintNonASCII;
}

TEST(Regex, LineStartAnchoring) {
  EXPECT_TRUE(regexIsAnchored(u"^abc"));
  EXPECT_TRUE(regexIsAnchored(u"abc^"));
  EXPECT_TRUE(regexIsAnchored(u"abc^"));
  EXPECT_TRUE(regexIsAnchored(u"abc^|^def"));
  EXPECT_TRUE(regexIsAnchored(u"abc^|^def"));
  EXPECT_TRUE(regexIsAnchored(u"^foo|^bar|^baz"));
  EXPECT_TRUE(regexIsAnchored(u"(^bar)"));
  EXPECT_TRUE(regexIsAnchored(u"(?=^bar)\\w+"));
  EXPECT_FALSE(regexIsAnchored(u"(?!^bar)\\w+"));
  EXPECT_FALSE(regexIsAnchored(u"(?=^bar)|\\w+"));
  EXPECT_FALSE(regexIsAnchored(u"abc"));
  EXPECT_FALSE(regexIsAnchored(u"abc|^def"));
  EXPECT_FALSE(regexIsAnchored(u"abc^|^def|\\w"));
  EXPECT_FALSE(regexIsAnchored(u"abc^", u"m"));
  EXPECT_FALSE(regexIsAnchored(u"abc", u"m"));
}

TEST(Regex, NonASCII) {
  EXPECT_TRUE(regexIsNonASCII(u"ab\xFF"));
  EXPECT_TRUE(regexIsNonASCII(u"ab(?:\xFF)"));
  EXPECT_TRUE(regexIsNonASCII(u"[\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"[0\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"[^\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"[\\w\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"[\x7F\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"abFF"));
  EXPECT_FALSE(regexIsNonASCII(u"^abFF"));
  EXPECT_FALSE(regexIsNonASCII(u"ab(\xFF|c)"));

  // This regex will take a very long time unless we exploit the non-ASCII
  // optimization.

  const std::u16string ascii = u"This is a pretty long all-ASCII string";
  cmatch matchRanges;
  EXPECT_FALSE(search(
      ascii,
      matchRanges,
      cregex(u"(((.*)*)*)*\xFF"),
      constants::matchInputAllAscii));
  EXPECT_FALSE(search(
      ascii,
      matchRanges,
      cregex(u"(?!(((.*)*)*)*\xFF)abc"),
      constants::matchInputAllAscii));
}

/// \return the start filter in the header of \p bytecode as a string:
/// "none" if absent; otherwise the ASCII members as hex ranges in
/// increasing order, then "nonascii" if StartFilterNonASCII is set, then
/// "single" if StartFilterSingleChar is set; "empty" for a present filter
/// with no members. Checks that StartFilterSingleChar and startChar agree
/// with the bitmap.
std::string describeStartFilter(const std::vector<uint8_t> &bytecode) {
  RegexBytecodeHeader header;
  EXPECT_GE(bytecode.size(), sizeof(header));
  std::memcpy(&header, bytecode.data(), sizeof(header));
  if (!(header.startFlags & StartFilterPresent)) {
    EXPECT_EQ(0u, header.startFlags);
    return "none";
  }
  std::string out;
  unsigned count = 0;
  for (uint32_t u = 0; u < 128;) {
    if (!startSetHasASCII(header, u)) {
      ++u;
      continue;
    }
    uint32_t last = u;
    while (last + 1 < 128 && startSetHasASCII(header, last + 1))
      ++last;
    char buf[16];
    if (last == u)
      snprintf(buf, sizeof(buf), "0x%02x", u);
    else
      snprintf(buf, sizeof(buf), "0x%02x-0x%02x", u, last);
    if (!out.empty())
      out += ' ';
    out += buf;
    count += last - u + 1;
    u = last + 1;
  }
  if (header.startFlags & StartFilterNonASCII) {
    if (!out.empty())
      out += ' ';
    out += "nonascii";
  }
  const bool single = header.startFlags & StartFilterSingleChar;
  EXPECT_EQ(count == 1, single) << out;
  if (single) {
    EXPECT_TRUE(startSetHasASCII(header, header.startChar)) << out;
    out += " single";
  }
  return out.empty() ? "empty" : out;
}

/// \return describeStartFilter() of the regex \p pattern with \p flags.
std::string startFilterOf(
    const std::u16string &pattern,
    const char16_t *flags = u"") {
  cregex re(pattern.c_str(), flags);
  EXPECT_TRUE(re.valid());
  if (!re.valid())
    return "invalid";
  return describeStartFilter(re.compile());
}

/// \return \p depth nested capture groups around the pattern "a".
std::u16string nestedGroups(unsigned depth) {
  return std::u16string(depth, u'(') + u"a" + std::u16string(depth, u')');
}

TEST(Regex, StartFilterHeader) {
  // Literals. /abc/ is coalesced into one MatchCharNode.
  EXPECT_EQ("0x61 single", startFilterOf(u"abc"));
  EXPECT_EQ("0x61 single", startFilterOf(u"(){999999999}a"));

  // Case folding: both cases of an ASCII letter; under /iu also every
  // non-ASCII unit, for U+017F -> 's' and U+212A -> 'k'.
  EXPECT_EQ("0x41 0x61", startFilterOf(u"a", u"i"));
  EXPECT_EQ("0x41 0x61 nonascii", startFilterOf(u"a", u"iu"));
  EXPECT_EQ("0x53 0x73 nonascii", startFilterOf(u"s", u"iu"));
  EXPECT_EQ("0x4b 0x6b nonascii", startFilterOf(u"k", u"iu"));
  EXPECT_EQ("0x53 0x73", startFilterOf(u"s", u"i"));
  EXPECT_EQ("nonascii", startFilterOf(u"\\u017f", u"i"));
  EXPECT_EQ("0x53 0x73 nonascii", startFilterOf(u"\\u017f", u"iu"));

  // Alternation, including an empty alternative.
  EXPECT_EQ("0x61 0x63", startFilterOf(u"ab|cd"));
  EXPECT_EQ("none", startFilterOf(u"a|"));

  // Loops, including one that never runs and a lazy one.
  EXPECT_EQ("0x78-0x79", startFilterOf(u"x*y"));
  EXPECT_EQ("0x78 single", startFilterOf(u"x+y"));
  EXPECT_EQ("0x61-0x62", startFilterOf(u"a{0}b"));
  EXPECT_EQ("0x61-0x62", startFilterOf(u"a*?b"));
  EXPECT_EQ("0x61-0x62", startFilterOf(u"(a|)b"));

  // Zero-width nodes are transparent; lookarounds are not descended into.
  EXPECT_EQ("0x61 single", startFilterOf(u"^a"));
  EXPECT_EQ("0x61 single", startFilterOf(u"\\ba"));
  EXPECT_EQ("0x61 single", startFilterOf(u"(?=b)a"));
  EXPECT_EQ("0x61 single", startFilterOf(u"(?!b)a"));
  EXPECT_EQ("0x61 single", startFilterOf(u"(?<=b)a"));
  EXPECT_EQ("none", startFilterOf(u"\\b"));

  // A back-reference first gives up; after a consuming node it is never
  // reached.
  EXPECT_EQ("none", startFilterOf(u"\\1(a)"));
  EXPECT_EQ("none", startFilterOf(u"(?=(a))\\1"));
  EXPECT_EQ("0x61 single", startFilterOf(u"(a)\\1"));
  EXPECT_EQ("none", startFilterOf(u"(?<=(a))\\1"));
  EXPECT_EQ("none", startFilterOf(u"(?<=(a))\\1b"));

  // Any character.
  EXPECT_EQ("0x00-0x09 0x0b-0x0c 0x0e-0x7f nonascii", startFilterOf(u"."));
  EXPECT_EQ("0x00-0x7f nonascii", startFilterOf(u".", u"s"));

  // Brackets and classes: exact on ASCII, a superset on non-ASCII.
  EXPECT_EQ("0x61-0x63", startFilterOf(u"[a-c]"));
  EXPECT_EQ("0x00-0x60 0x62-0x7f nonascii", startFilterOf(u"[^a]"));
  EXPECT_EQ("0x30-0x39", startFilterOf(u"\\d"));
  EXPECT_EQ("0x30-0x39", startFilterOf(u"[\\d]"));
  EXPECT_EQ("0x00-0x2f 0x3a-0x7f nonascii", startFilterOf(u"\\D"));
  EXPECT_EQ(
      "0x30-0x39 0x41-0x5a 0x5f 0x61-0x7a nonascii", startFilterOf(u"\\w"));
  EXPECT_EQ("0x09-0x0d 0x20 nonascii", startFilterOf(u"\\s"));
  EXPECT_EQ(
      "0x00-0x2f 0x3a-0x40 0x5b-0x5e 0x60 0x7b-0x7f nonascii",
      startFilterOf(u"\\W", u"iu"));
  EXPECT_EQ(
      "0x30-0x39 0x41-0x5a 0x5f 0x61-0x7a nonascii",
      startFilterOf(u"\\w", u"iu"));
  EXPECT_EQ("0x09-0x0d 0x20 nonascii", startFilterOf(u"[\\s\u00e9]"));
  EXPECT_EQ("0x61 nonascii single", startFilterOf(u"[a\\u00e9]"));
  EXPECT_EQ("0x41-0x5a 0x61-0x7a", startFilterOf(u"[a-z]", u"i"));
  EXPECT_EQ("0x41-0x5a 0x61-0x7a nonascii", startFilterOf(u"[a-z]", u"iu"));
  // An empty bracket rejects every unit; a full one keeps non-ASCII units.
  EXPECT_EQ("empty", startFilterOf(u"[]"));
  EXPECT_EQ("0x00-0x7f nonascii", startFilterOf(u"[^]"));
#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES
  // Property escapes are ranges added by the parser, not classes.
  EXPECT_EQ("0x41-0x5a nonascii", startFilterOf(u"\\p{Lu}", u"u"));
  EXPECT_EQ("0x00-0x40 0x5b-0x7f nonascii", startFilterOf(u"\\P{Lu}", u"u"));
  EXPECT_EQ("0x41-0x5a 0x61-0x7a nonascii", startFilterOf(u"\\p{Lu}", u"iu"));
#endif

  // Surrogates under /u: an astral literal starts with its high surrogate;
  // a lone surrogate is itself non-ASCII.
  EXPECT_EQ("nonascii", startFilterOf(u"\\u{1F600}", u"u"));
  EXPECT_EQ("nonascii", startFilterOf(u"\\uD83D", u"u"));

  // Modifier groups make icase node-local.
  EXPECT_EQ("0x41 0x61", startFilterOf(u"(?i:a)b"));
  EXPECT_EQ("0x61 single", startFilterOf(u"a(?i:b)"));

  // The depth cap: the innermost list of N nested groups is at depth N.
  EXPECT_EQ("0x61 single", startFilterOf(nestedGroups(63)));
  EXPECT_EQ("none", startFilterOf(nestedGroups(64)));
  EXPECT_EQ("none", startFilterOf(nestedGroups(100)));
}

/// \return the outcome of searching \p text (\p length units) for
/// \p bytecode from \p start with \p flags: "nomatch", "overflow", or
/// "match" and the captures.
template <typename CharT>
std::string searchOutcome(
    const std::vector<uint8_t> &bytecode,
    const CharT *text,
    uint32_t length,
    uint32_t start,
    constants::MatchFlagType flags) {
  cmatch m;
  switch (searchWithBytecode(bytecode, text, start, length, &m, flags)) {
    case MatchRuntimeResult::Match:
      return "match " + flatten(m);
    case MatchRuntimeResult::NoMatch:
      return "nomatch";
    case MatchRuntimeResult::StackOverflow:
      return "overflow";
  }
  return "?";
}

/// \return \p s with each non-ASCII code unit written as \uXXXX.
std::string printable(const std::u16string &s) {
  std::string out;
  for (char16_t c : s) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
      out += buf;
    }
  }
  return out;
}

/// \return \p s with each byte >= 0x80 written as \xXX.
std::string printableBytes(const std::string &s) {
  std::string out;
  for (char c : s) {
    const unsigned u = static_cast<unsigned char>(c);
    if (u < 0x80) {
      out += c;
    } else {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\x%02x", u);
      out += buf;
    }
  }
  return out;
}

/// Check that the start filter of \p pattern with \p flags never changes a
/// search result: over UTF-16, ASCII and other byte inputs, at every start
/// index, with and without matchOnlyAtStart (sticky), the filtered search
/// and the unfiltered one (matchNoStartFilter) give the same outcome,
/// unless the unfiltered one overflows: a position the filter skips could
/// spend backtracking budget before its first unit is tested.
void expectStartFilterTransparent(
    const std::u16string &pattern,
    const std::u16string &flags) {
  // Case folds to ASCII (U+017F, U+212A), line terminators, a surrogate
  // pair, lone surrogates, Latin-1, and ASCII units held as UTF-16.
  // \x escapes are split so no hex digit follows them.
  static const std::u16string utf16Inputs[] = {
      u"xaAbsSkK\u017f\u212a",
      u"b\u2028a\nc\rd e\tf_9",
      u"\xD83D\xDE00"
      u"a"
      u"\xD83D\xDE00",
      u"a\xD83D"
      u"b\xDE00"
      u"c",
      u"\u00e9\u00c9x1_9",
      u"abcAB 1\nxyz",
      // More than kMaxStartFilterPasses passing units, then a gap: the
      // search must resume at the unit right after the gap.
      u"aaaaaaaaaaaaaaaaaaaazabczaab",
      // The only 'a' is the last unit.
      u"bcdefa",
      u"",
  };
  static const std::string asciiInputs[] = {
      "xaAbsSkK",
      "b a\nc\rd\te_9",
      "abcabd012_",
      "aaaaaaaaaaaaaaaaaaaazabczaab",
      "bcdefa",
      "",
  };
  // Inputs of the char path that are not 7-bit, like the UTF-8 text the
  // dependency extractor passes: units >= 0x80, searched without
  // matchInputAllAscii. \x escapes are split so no hex digit follows them.
  static const std::string byteInputs[] = {
      "caf\xc3\xa9 x\xe9y\x80",
      "\xe2\x80\xa8"
      "a\xff"
      "b",
  };
  cregex re(pattern.c_str(), flags.c_str());
  ASSERT_TRUE(re.valid()) << printable(pattern);
  const std::vector<uint8_t> bytecode = re.compile();
  for (constants::MatchFlagType mode :
       {constants::matchDefault, constants::matchOnlyAtStart}) {
    for (const std::u16string &in : utf16Inputs) {
      const uint32_t len = in.size();
      for (uint32_t start = 0; start <= len; ++start) {
        std::string unfiltered = searchOutcome(
            bytecode,
            in.data(),
            len,
            start,
            mode | constants::matchNoStartFilter);
        if (unfiltered == "overflow")
          continue;
        EXPECT_EQ(
            unfiltered, searchOutcome(bytecode, in.data(), len, start, mode))
            << "/" << printable(pattern) << "/" << printable(flags) << " on \""
            << printable(in) << "\" from " << start << (mode ? " sticky" : "");
      }
    }
    for (const std::string &in : asciiInputs) {
      const uint32_t len = in.size();
      const constants::MatchFlagType asciiMode =
          mode | constants::matchInputAllAscii;
      for (uint32_t start = 0; start <= len; ++start) {
        std::string unfiltered = searchOutcome(
            bytecode,
            in.data(),
            len,
            start,
            asciiMode | constants::matchNoStartFilter);
        if (unfiltered == "overflow")
          continue;
        EXPECT_EQ(
            unfiltered,
            searchOutcome(bytecode, in.data(), len, start, asciiMode))
            << "/" << printable(pattern) << "/" << printable(flags)
            << " on ascii \"" << in << "\" from " << start
            << (mode ? " sticky" : "");
      }
    }
    for (const std::string &in : byteInputs) {
      const uint32_t len = in.size();
      for (uint32_t start = 0; start <= len; ++start) {
        std::string unfiltered = searchOutcome(
            bytecode,
            in.data(),
            len,
            start,
            mode | constants::matchNoStartFilter);
        if (unfiltered == "overflow")
          continue;
        EXPECT_EQ(
            unfiltered, searchOutcome(bytecode, in.data(), len, start, mode))
            << "/" << printable(pattern) << "/" << printable(flags)
            << " on bytes of \"" << printableBytes(in) << "\" from " << start
            << (mode ? " sticky" : "");
      }
    }
  }
}

TEST(Regex, StartFilterTransparent) {
  const std::pair<std::u16string, std::u16string> corpus[] = {
      {u"abc", u""},
      {u"a", u""},
      {u"a", u"i"},
      {u"a", u"iu"},
      {u"s", u"iu"},
      {u"k", u"iu"},
      {u"s", u"i"},
      {u"K", u"i"},
      {u"\\u017f", u"i"},
      {u"\\u017f", u"iu"},
      {u"\\u212a", u"iu"},
      {u"ab|cd", u""},
      {u"a|", u""},
      {u"x*y", u""},
      {u"x+y", u""},
      {u"^a", u""},
      {u"^b", u"m"},
      {u"a$", u"m"},
      {u"\\ba", u""},
      {u"\\bb", u"i"},
      {u"(?=b)a", u""},
      {u"(?<=a)b", u""},
      {u"(?!a)b", u""},
      {u"(?!a)\\w", u""},
      {u"(?<!a)\\w", u"u"},
      {u"\\1(a)", u""},
      {u"(?=(a))\\1", u""},
      {u"(a)\\1", u"i"},
      {u".", u""},
      {u".", u"s"},
      {u".", u"u"},
      {u".a", u"su"},
      {u"[a-c]", u""},
      {u"[^a]", u""},
      {u"[^a]", u"u"},
      {u"\\d", u""},
      {u"\\D", u""},
      {u"\\w", u""},
      {u"\\w", u"iu"},
      {u"\\W", u"iu"},
      {u"\\s", u""},
      {u"\\s+x", u""},
      {u"[\\s\\S]a", u""},
      {u"\\S", u""},
      {u"[a-z]", u"i"},
      {u"[a-z]", u"iu"},
      {u"[a\\u00e9]", u""},
      {u"[]", u""},
      {u"[^]", u""},
      {u"\\u{1F600}", u"u"},
      {u"[\\u{1F600}x]", u"u"},
      {u"\\uD83D", u"u"},
      {u"\\uDE00", u"u"},
      {u"\\uD83D", u""},
      // 32-bit operands whose low byte is ASCII ('a'), searched over bytes.
      {u"\\uD861", u"u"},
      {u"\\uD861", u"iu"},
      {u"\\u{1F461}", u"u"},
      {u"\\u{1F461}", u"iu"},
      {u"\\u2028|\\n", u""},
      {u"(?i:a)b", u""},
      {u"a(?i:b)", u""},
      {u"[\\s\u00e9]", u""},
      {u"[a\u00e9]x", u""},
      {u"a{0}b", u""},
      {u"a*?b", u""},
      {u"(a|)b", u""},
      {u"(?<=(a))\\1", u""},
      {u"(?<=(a))\\1b", u""},
#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES
      {u"\\p{Lu}", u"u"},
      {u"\\P{Lu}", u"u"},
      {u"\\p{Lu}", u"iu"},
#endif
      {nestedGroups(63), u""},
      {nestedGroups(64), u""},
  };
  for (const auto &entry : corpus)
    expectStartFilterTransparent(entry.first, entry.second);
}

/// The char executor must not truncate a 32-bit pattern code point to its
/// low byte: a byte can never equal U+D861 or U+1F461, whose low byte is
/// 'a'.
TEST(Regex, CharPathDoesNotTruncateCodePoints) {
  static const char16_t *const patterns[][2] = {
      {u"\\uD861", u"u"},
      {u"\\uD861", u"iu"},
      {u"\\u{1F461}", u"u"},
      {u"\\u{1F461}", u"iu"},
  };
  static const std::string inputs[] = {
      "a",
      "\xC3\xA9"
      "a",
      "xa\x80"
      "a\xff",
      "\xe2\x80\xa8"
      "a",
  };
  for (const auto &p : patterns) {
    cregex re(p[0], p[1]);
    ASSERT_TRUE(re.valid()) << printable(p[0]) << "/" << printable(p[1]);
    const std::vector<uint8_t> bytecode = re.compile();
    for (const std::string &in : inputs) {
      for (constants::MatchFlagType mode :
           {constants::matchDefault,
            constants::matchDefault | constants::matchNoStartFilter}) {
        EXPECT_EQ(
            "nomatch", searchOutcome(bytecode, in.data(), in.size(), 0, mode))
            << "/" << printable(p[0]) << "/" << printable(p[1])
            << " on bytes of \"" << printableBytes(in) << "\"";
      }
    }
  }
}

TEST(Regex, StartFilterAvoidsOverflow) {
  // Unfiltered, position 0 enters the loop and overflows the
  // backtrack stack before testing 'a'; filtered, no position is tried. It
  // runs once each way: an unfiltered run builds 2^24 backtrack records.
  const std::vector<uint8_t> bytecode = cregex(u"(){999999999}a").compile();
  const char16_t text[] = u"b";
  cmatch m;
  EXPECT_EQ(
      MatchRuntimeResult::StackOverflow,
      searchWithBytecode(
          bytecode, text, 0, 1, &m, constants::matchNoStartFilter));
  EXPECT_EQ(
      MatchRuntimeResult::NoMatch,
      searchWithBytecode(bytecode, text, 0, 1, &m, constants::matchDefault));
}

} // end anonymous namespace
