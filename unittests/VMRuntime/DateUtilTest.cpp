/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"

#include "hermes/VM/JSLib/DateCache.h"
#include "hermes/VM/JSLib/DateUtil.h"

#include <cstdlib>

using namespace hermes::vm;

namespace {
TEST(DateUtilTest, DayNumberAndTimeWithinDayTest) {
  EXPECT_EQ(0, day(0)); // Jan 1, 1970
  EXPECT_EQ(-2, day(-86400100)); // Dec 31, 1969
  EXPECT_EQ(17238, day(1489439675469)); // Mar 13, 2017

  EXPECT_EQ(0, timeWithinDay(0)); // Jan 1, 1970
  EXPECT_EQ(86399900, timeWithinDay(-86400100)); // Dec 31, 1969
  EXPECT_EQ(76475469, timeWithinDay(1489439675469)); // Mar 13, 2017
}

TEST(DateUtilTest, YearNumberTest) {
  EXPECT_EQ(365u, daysInYear(2017));
  EXPECT_EQ(366u, daysInYear(2004));
  EXPECT_EQ(365u, daysInYear(1900));
  EXPECT_EQ(366u, daysInYear(2000));
  EXPECT_EQ(365u, daysInYear(-2017));
  EXPECT_EQ(366u, daysInYear(-2004));
  EXPECT_EQ(365u, daysInYear(-1900));
  EXPECT_EQ(366u, daysInYear(-2000));

  EXPECT_EQ(17167, dayFromYear(2017));
  EXPECT_EQ(10957, dayFromYear(2000));
  EXPECT_EQ(-25567, dayFromYear(1900));

  EXPECT_EQ(1483228800000, timeFromYear(2017));
  EXPECT_EQ(946684800000, timeFromYear(2000));
  EXPECT_EQ(-2208988800000, timeFromYear(1900));

  EXPECT_EQ(2017, yearFromTime(1483228800000)); // Jan 1, 2017
  EXPECT_EQ(2016, yearFromTime(1483228790000)); // Dec 31, 2016
  EXPECT_EQ(2017, yearFromTime(1483228812000)); // Jan 1, 2017
  EXPECT_EQ(2017, yearFromTime(1489439675469)); // Mar 13, 2017
  EXPECT_EQ(2000, yearFromTime(946684803000)); // Jan 1, 2000
  EXPECT_EQ(1900, yearFromTime(-2208988800000)); // Jan 1, 1900

  EXPECT_FALSE(inLeapYear(1483228800000)); // Jan 1, 2017
  EXPECT_TRUE(inLeapYear(1483228790000)); // Dec 31, 2016
  EXPECT_FALSE(inLeapYear(1483228812000)); // Jan 1, 2017
  EXPECT_FALSE(inLeapYear(1489439675469)); // Mar 13, 2017
  EXPECT_TRUE(inLeapYear(946684803000)); // Jan 1, 2000
  EXPECT_FALSE(inLeapYear(-2208988800000)); // Jan 1, 1900
}

TEST(DateUtilTest, MonthNumberTest) {
  EXPECT_EQ(0u, monthFromTime(1483228800000)); // Jan 1, 2017
  EXPECT_EQ(11u, monthFromTime(1483228790000)); // Dec 31, 2016
  EXPECT_EQ(0u, monthFromTime(1483228812000)); // Jan 1, 2017
  EXPECT_EQ(0u, monthFromTime(946684803000)); // Jan 1, 2000
  EXPECT_EQ(0u, monthFromTime(-2208988800000)); // Jan 1, 1900
  EXPECT_EQ(1u, monthFromTime(1456732800000)); // Feb 29, 2016

  EXPECT_EQ(0u, monthFromTime(1483776000000)); // Jan 7, 2017
  EXPECT_EQ(1u, monthFromTime(1486454400000)); // Feb 7, 2017
  EXPECT_EQ(2u, monthFromTime(1488873600000)); // Mar 7, 2017
  EXPECT_EQ(3u, monthFromTime(1491548400000)); // Apr 7, 2017
  EXPECT_EQ(4u, monthFromTime(1494140400000)); // May 7, 2017
  EXPECT_EQ(5u, monthFromTime(1496818800000)); // Jun 7, 2017
  EXPECT_EQ(6u, monthFromTime(1500274800000)); // Jul 17, 2017
  EXPECT_EQ(7u, monthFromTime(1502953200000)); // Aug 17, 2017
  EXPECT_EQ(8u, monthFromTime(1505631600000)); // Sep 17, 2017
  EXPECT_EQ(9u, monthFromTime(1508223600000)); // Oct 17, 2017
  EXPECT_EQ(10u, monthFromTime(1510905600000)); // Nov 17, 2017
  EXPECT_EQ(11u, monthFromTime(1513497600000)); // Dec 17, 2017
}

TEST(DateUtilTest, DateNumberTest) {
  EXPECT_EQ(1, dateFromTime(1483228800000)); // Jan 1, 2017
  EXPECT_EQ(31, dateFromTime(1483228790000)); // Dec 31, 2016
  EXPECT_EQ(1, dateFromTime(1483228812000)); // Jan 1, 2017
  EXPECT_EQ(1, dateFromTime(946684803000)); // Jan 1, 2000
  EXPECT_EQ(1, dateFromTime(-2208988800000)); // Jan 1, 1900
  EXPECT_EQ(29, dateFromTime(1456732800000)); // Feb 29, 2016

  EXPECT_EQ(7, dateFromTime(1483776000000)); // Jan 7, 2017
  EXPECT_EQ(7, dateFromTime(1486454400000)); // Feb 7, 2017
  EXPECT_EQ(7, dateFromTime(1488873600000)); // Mar 7, 2017
  EXPECT_EQ(7, dateFromTime(1491548400000)); // Apr 7, 2017
  EXPECT_EQ(7, dateFromTime(1494140400000)); // May 7, 2017
  EXPECT_EQ(7, dateFromTime(1496818800000)); // Jun 7, 2017
  EXPECT_EQ(17, dateFromTime(1500274800000)); // Jul 17, 2017
  EXPECT_EQ(17, dateFromTime(1502953200000)); // Aug 17, 2017
  EXPECT_EQ(17, dateFromTime(1505631600000)); // Sep 17, 2017
  EXPECT_EQ(17, dateFromTime(1508223600000)); // Oct 17, 2017
  EXPECT_EQ(17, dateFromTime(1510905600000)); // Nov 17, 2017
  EXPECT_EQ(17, dateFromTime(1513497600000)); // Dec 17, 2017

  EXPECT_EQ(7, dateFromTime(1452168000000)); // Jan 7, 2016
  EXPECT_EQ(7, dateFromTime(1454846400000)); // Feb 7, 2016
  EXPECT_EQ(7, dateFromTime(1457352000000)); // Mar 7, 2016
  EXPECT_EQ(7, dateFromTime(1460030400000)); // Apr 7, 2016
  EXPECT_EQ(7, dateFromTime(1462622400000)); // May 7, 2016
  EXPECT_EQ(7, dateFromTime(1462622400000)); // Jun 7, 2016
  EXPECT_EQ(17, dateFromTime(1468756800000)); // Jul 17, 2016
  EXPECT_EQ(17, dateFromTime(1471435200000)); // Aug 17, 2016
  EXPECT_EQ(17, dateFromTime(1474113600000)); // Sep 17, 2016
  EXPECT_EQ(17, dateFromTime(1476705600000)); // Oct 17, 2016
  EXPECT_EQ(17, dateFromTime(1479384000000)); // Nov 17, 2016
  EXPECT_EQ(17, dateFromTime(1481976000000)); // Dec 17, 2016
}

TEST(DateUtilTest, WeekDayTest) {
  EXPECT_EQ(4, weekDay(0)); // Thu, Jan 1, 1970
  EXPECT_EQ(4, weekDay(100)); // Thu, Jan 1, 1970
  EXPECT_EQ(1, weekDay(1489439675469)); // Mon, Mar 13, 2017
  EXPECT_EQ(3, weekDay(23415386789000)); // Wed, Jan 3, 2712
}

namespace {
/// `localtime_r()` caches the timezone. To correctly
/// handle different timezones in a single test, let's call `tzset()` explicitly
/// to update the cached timezone.
void setTimeZone(const char *tzname) {
  hermes::oscompat::set_env("TZ", tzname);
  ::tzset();
}

/// Encode civil fields with a one-based month, without a timezone adjustment.
double timeValue(int year, int month, int date, int hour = 0, int minute = 0) {
  return makeDate(makeDay(year, month - 1, date), makeTime(hour, minute, 0, 0));
}

/// Check whether the IANA timezone database is available by setting TZ to
/// known IANA names (America/New_York and Pacific/Auckland) and verifying
/// their offsets on July 1, 2018.
///
/// \return false on Windows (which does not support IANA names) and
/// on systems without tzdata installed, where IANA names silently fall back to
/// UTC.
bool hasIANATimezoneDB() {
#ifdef _WINDOWS
  return false;
#else
  static hermes::OptValue<bool> result = llvh::None;
  if (result.hasValue())
    return result.getValue();
  setTimeZone("America/New_York");
  LocalTimeOffsetCache cache;
  auto nyOffset = cache.totalOffsetInMs(timeValue(2018, 7, 1));
  setTimeZone("Pacific/Auckland");
  cache.reset();
  auto aucklandOffset = cache.totalOffsetInMs(timeValue(2018, 7, 1));
  result = nyOffset == -4 * MS_PER_HOUR && aucklandOffset == 12 * MS_PER_HOUR;
  return *result;
#endif
}
} // namespace

TEST(DateUtilTest, TotalOffsetTest) {
  // On Windows, TZ env can only be set to a very limited format,
  // as documented in Microsoft Docs for _tzset. Specifically,
  // * They cannot be set to IANA TZDB zone names.
  // * Whenever DST rule is enabled (by specifying any 3 letter after the
  //   digits), Windows always uses current US DST rules. It does NOT
  //   follow historic rules for old dates or rules of other regions.

  LocalTimeOffsetCache cache;
  const double january = timeValue(2019, 1, 15);
  const double july = timeValue(2019, 7, 15);

  // US Pacific: DST is from Mar to Nov
  setTimeZone(hasIANATimezoneDB() ? "America/Los_Angeles" : "PST8PDT");
  cache.reset();
  EXPECT_EQ(-8 * MS_PER_HOUR, cache.totalOffsetInMs(january));
  EXPECT_EQ(-7 * MS_PER_HOUR, cache.totalOffsetInMs(july));

  // New Zealand: DST is from Oct to Apr.
  setTimeZone(hasIANATimezoneDB() ? "Pacific/Auckland" : "NZT-12");
  cache.reset();
  EXPECT_EQ(
      (hasIANATimezoneDB() ? 13 : 12) * MS_PER_HOUR,
      cache.totalOffsetInMs(january));
  EXPECT_EQ(12 * MS_PER_HOUR, cache.totalOffsetInMs(july));

  // Disable DST entirely and make sure the offset is the same.
  // Test both positive and negative zone.

  // Negative fixed zone
  setTimeZone(hasIANATimezoneDB() ? "Etc/GMT+8" : "PST8");
  cache.reset();
  EXPECT_EQ(-8 * MS_PER_HOUR, cache.totalOffsetInMs(january));
  EXPECT_EQ(-8 * MS_PER_HOUR, cache.totalOffsetInMs(july));

  // Positive fixed zone
  setTimeZone(hasIANATimezoneDB() ? "Asia/Tokyo" : "JST-9");
  cache.reset();
  EXPECT_EQ(9 * MS_PER_HOUR, cache.totalOffsetInMs(january));
  EXPECT_EQ(9 * MS_PER_HOUR, cache.totalOffsetInMs(july));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, EquivalentTimeTest) {
  // 2008-01-01, 2018-12-31
  EXPECT_EQ(1199145600, detail::equivalentTime(1199145600));
  EXPECT_EQ(1199190660, detail::equivalentTime(1199190660));
  // 2033-01-01
  EXPECT_EQ(1988150400, detail::equivalentTime(1988150400));
  // 1970-01-01
  EXPECT_EQ(0, detail::equivalentTime(0));
  // 2037-01-01
  EXPECT_EQ(2114380800, detail::equivalentTime(2114380800));
  // 1969-01-01 -> 2014-01-01, 1969-12-31 -> 2014-12-31
  EXPECT_EQ(1388534400, detail::equivalentTime(-31536000));
  EXPECT_EQ(1388579460, detail::equivalentTime(-31490940));
  // 2038-01-01 -> 2010-01-01, 2038-12-31 -> 2010-12-31
  EXPECT_EQ(1262304000, detail::equivalentTime(2145916800));
  EXPECT_EQ(1262349060, detail::equivalentTime(2145961860));
  // -123456-01-01 -> 2020-01-01, -123456-12-31 -> 2020-12-31
  EXPECT_EQ(1577836800, detail::equivalentTime(-3958062278400));
  EXPECT_EQ(1577881860, detail::equivalentTime(-3958062233340));
  // -123457-01-01 -> 2030-01-01, -123457-12-31 -> 2030-12-31
  EXPECT_EQ(1893456000, detail::equivalentTime(-3958093814400));
  EXPECT_EQ(1893501060, detail::equivalentTime(-3958093769340));
  // +123456-01-01 -> 2008-01-01, +123456-12-31 -> 2008-12-31
  EXPECT_EQ(1199145600, detail::equivalentTime(3833727840000));
  EXPECT_EQ(1199190660, detail::equivalentTime(3833727885060));
  // +123457-01-01 -> 2026-01-01, +123457-12-31 -> 2020-12-31
  EXPECT_EQ(1767225600, detail::equivalentTime(3833759462400));
  EXPECT_EQ(1767270660, detail::equivalentTime(3833759507460));
}

TEST(DateUtilTest, HistoricalTotalOffsetTest) {
  LocalTimeOffsetCache localTimeOffsetCache;
  auto totalOffset = [&localTimeOffsetCache](double timeMs) {
    return localTimeOffsetCache.totalOffsetInMs(timeMs);
  };

  setTimeZone(hasIANATimezoneDB() ? "America/Los_Angeles" : "PST8PDT");
  localTimeOffsetCache.reset();
  EXPECT_EQ(-7 * MS_PER_HOUR, totalOffset(1489530532000)); // Mar 14, 2017
  EXPECT_EQ(-7 * MS_PER_HOUR, totalOffset(1019514530000)); // Apr 22, 2002
  EXPECT_EQ(-8 * MS_PER_HOUR, totalOffset(1487111330000)); // Feb 14, 2017
  // Apr 1, 2002 was before the 2007 US DST rule change. Only IANA tzdata
  // has the historic rules needed to correctly report no DST for this date.
  if (hasIANATimezoneDB())
    EXPECT_EQ(-8 * MS_PER_HOUR, totalOffset(1017700130000)); // Apr 1, 2002

  setTimeZone(hasIANATimezoneDB() ? "America/Chicago" : "CST6CDT");
  localTimeOffsetCache.reset();
  EXPECT_EQ(-5 * MS_PER_HOUR, totalOffset(1489530532000)); // Mar 14, 2017
  EXPECT_EQ(-5 * MS_PER_HOUR, totalOffset(1019514530000)); // Apr 22, 2002
  EXPECT_EQ(-6 * MS_PER_HOUR, totalOffset(1487111330000)); // Feb 14, 2017
  // Apr 1, 2002 was before the 2007 US DST rule change. Only IANA tzdata
  // has the historic rules needed to correctly report no DST for this date.
  if (hasIANATimezoneDB())
    EXPECT_EQ(-6 * MS_PER_HOUR, totalOffset(1017700130000)); // Apr 1, 2002

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, LocalTimeTest) {
  // On Windows, TZ env can only be set to a very limited format,
  // as documented in Microsoft Docs for _tzset. Specifically,
  // * They cannot be set to IANA TZDB zone names.
  // * Whenever DST rule is enabled (by specifying any 3 letter after the
  //   digits), Windows always uses current US DST rules. It does NOT
  //   follow historic rules for old dates or rules of other regions.

  // These test cases cover DST in effect, not in effect, and no DST.
  // It also covers positive zone and negative zone.
  // 2018-07-01T16:00:00Z
  // 2018-07-01T09:00:00-0700[America/Los_Angeles] (DST in effect)
  // 2018-07-01T12:00:00-0400[America/New_York] (DST in effect)
  // 2018-07-02T04:00:00+1200[America/New_York] (DST not in effect)
  // 2018-07-02T01:00:00+0900[Asia/Tokyo] (DST not observed)

  LocalTimeOffsetCache localTimeOffsetCache;

  setTimeZone(hasIANATimezoneDB() ? "America/Los_Angeles" : "PST8PDT");
  localTimeOffsetCache.reset();
  EXPECT_EQ(1530435600000, localTime(1530460800000, localTimeOffsetCache));
  EXPECT_EQ(1530460800000, utcTime(1530435600000, localTimeOffsetCache));

  setTimeZone(hasIANATimezoneDB() ? "America/New_York" : "EST5EDT");
  localTimeOffsetCache.reset();
  EXPECT_EQ(1530446400000, localTime(1530460800000, localTimeOffsetCache));
  EXPECT_EQ(1530460800000, utcTime(1530446400000, localTimeOffsetCache));

  if (hasIANATimezoneDB()) {
    setTimeZone("Pacific/Auckland");
    localTimeOffsetCache.reset();
    EXPECT_EQ(1530504000000, localTime(1530460800000, localTimeOffsetCache));
    EXPECT_EQ(1530460800000, utcTime(1530504000000, localTimeOffsetCache));
  }

  setTimeZone(hasIANATimezoneDB() ? "Asia/Tokyo" : "JST-9");
  localTimeOffsetCache.reset();
  EXPECT_EQ(1530493200000, localTime(1530460800000, localTimeOffsetCache));
  EXPECT_EQ(1530460800000, utcTime(1530493200000, localTimeOffsetCache));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, HalfHourDaylightSavingTest) {
  if (!hasIANATimezoneDB())
    return;
  setTimeZone("Australia/Lord_Howe");
  LocalTimeOffsetCache cache;

  EXPECT_EQ(660 * MS_PER_MINUTE, cache.totalOffsetInMs(timeValue(2019, 1, 15)));
  EXPECT_EQ(630 * MS_PER_MINUTE, cache.totalOffsetInMs(timeValue(2019, 7, 15)));
  EXPECT_EQ(
      timeValue(2019, 1, 15, 23), localTime(timeValue(2019, 1, 15, 12), cache));
  EXPECT_EQ(
      timeValue(2019, 7, 15, 22, 30),
      localTime(timeValue(2019, 7, 15, 12), cache));
  EXPECT_EQ(
      timeValue(2019, 1, 15, 12), utcTime(timeValue(2019, 1, 15, 23), cache));
  EXPECT_EQ(
      timeValue(2019, 7, 15, 12),
      utcTime(timeValue(2019, 7, 15, 22, 30), cache));

  // The 30-minute fold repeats 01:30..02:00; select the earlier occurrence.
  EXPECT_EQ(
      timeValue(2019, 4, 6, 14, 45),
      utcTime(timeValue(2019, 4, 7, 1, 45), cache));
  EXPECT_EQ(
      timeValue(2019, 4, 7, 1, 45),
      localTime(timeValue(2019, 4, 6, 14, 45), cache));
  EXPECT_EQ(
      timeValue(2019, 4, 7, 1, 45),
      localTime(timeValue(2019, 4, 6, 15, 15), cache));

  // The gap skips 02:00..02:30; the first valid time is 02:30.
  EXPECT_EQ(
      timeValue(2019, 10, 5, 15, 29),
      utcTime(timeValue(2019, 10, 6, 1, 59), cache));
  EXPECT_EQ(
      timeValue(2019, 10, 5, 15, 45),
      utcTime(timeValue(2019, 10, 6, 2, 15), cache));
  EXPECT_EQ(
      timeValue(2019, 10, 5, 15, 30),
      utcTime(timeValue(2019, 10, 6, 2, 30), cache));
  EXPECT_EQ(
      timeValue(2019, 10, 5, 15, 45),
      utcTime(timeValue(2019, 10, 6, 2, 45), cache));
  EXPECT_EQ(
      timeValue(2019, 10, 6, 2, 30),
      localTime(timeValue(2019, 10, 5, 15, 30), cache));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, TwoHourDaylightSavingTest) {
  if (!hasIANATimezoneDB())
    return;
  setTimeZone("Antarctica/Troll");
  LocalTimeOffsetCache cache;

  EXPECT_EQ(0, cache.totalOffsetInMs(timeValue(2019, 1, 15)));
  EXPECT_EQ(2 * MS_PER_HOUR, cache.totalOffsetInMs(timeValue(2019, 7, 15)));
  EXPECT_EQ(
      timeValue(2019, 1, 15, 12), localTime(timeValue(2019, 1, 15, 12), cache));
  EXPECT_EQ(
      timeValue(2019, 7, 15, 14), localTime(timeValue(2019, 7, 15, 12), cache));
  EXPECT_EQ(
      timeValue(2019, 1, 15, 12), utcTime(timeValue(2019, 1, 15, 12), cache));
  EXPECT_EQ(
      timeValue(2019, 7, 15, 12), utcTime(timeValue(2019, 7, 15, 14), cache));

  // The two-hour gap skips 01:00..03:00; the first valid time is 03:00.
  EXPECT_EQ(
      timeValue(2019, 3, 31, 1), utcTime(timeValue(2019, 3, 31, 1), cache));
  EXPECT_EQ(
      timeValue(2019, 3, 31, 2, 30),
      utcTime(timeValue(2019, 3, 31, 2, 30), cache));
  EXPECT_EQ(
      timeValue(2019, 3, 31, 1), utcTime(timeValue(2019, 3, 31, 3), cache));
  EXPECT_EQ(
      timeValue(2019, 3, 31, 1, 15),
      utcTime(timeValue(2019, 3, 31, 3, 15), cache));
  EXPECT_EQ(
      timeValue(2019, 3, 31, 3), localTime(timeValue(2019, 3, 31, 1), cache));

  // Both UTC instants map to 02:30 in the fold; select the earlier one.
  EXPECT_EQ(
      timeValue(2019, 10, 27, 0, 30),
      utcTime(timeValue(2019, 10, 27, 2, 30), cache));
  EXPECT_EQ(
      timeValue(2019, 10, 27, 2, 30),
      localTime(timeValue(2019, 10, 27, 0, 30), cache));
  EXPECT_EQ(
      timeValue(2019, 10, 27, 2, 30),
      localTime(timeValue(2019, 10, 27, 2, 30), cache));
  EXPECT_EQ(
      timeValue(2019, 10, 27, 3), utcTime(timeValue(2019, 10, 27, 3), cache));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, HistoricalStandardOffsetChangeTest) {
  if (!hasIANATimezoneDB())
    return;
  setTimeZone("Asia/Kathmandu");
  LocalTimeOffsetCache cache;

  // The standard offset changed from +05:30 to +05:45 at the start of 1986.
  const double transition = timeValue(1985, 12, 31, 18, 30);
  EXPECT_EQ(330 * MS_PER_MINUTE, cache.totalOffsetInMs(transition - 1));
  EXPECT_EQ(345 * MS_PER_MINUTE, cache.totalOffsetInMs(transition));
  EXPECT_EQ(timeValue(1986, 1, 1, 0, 15), localTime(transition, cache));
  EXPECT_EQ(transition, utcTime(timeValue(1986, 1, 1, 0, 15), cache));
  EXPECT_EQ(
      timeValue(1985, 12, 31, 18, 35),
      utcTime(timeValue(1986, 1, 1, 0, 5), cache));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, PermanentDaylightTimeTest) {
  if (!hasIANATimezoneDB())
    return;
  setTimeZone("America/Vancouver");
  LocalTimeOffsetCache cache;
  auto offset = [&cache](double utc) { return localTime(utc, cache) - utc; };

  // British Columbia kept -07:00 all year from 2026-11-01, recorded in tzdata
  // as a new standard time. Older tzdata keeps switching to -08:00.
  if (offset(timeValue(2027, 1, 15, 12)) != -7 * MS_PER_HOUR)
    return;

  // Earlier dates keep their own offsets, rather than the current standard
  // offset plus an hour of DST.
  EXPECT_EQ(-8 * MS_PER_HOUR, offset(timeValue(2026, 1, 15, 12)));
  EXPECT_EQ(-7 * MS_PER_HOUR, offset(timeValue(2026, 7, 15, 12)));
  EXPECT_EQ(-7 * MS_PER_HOUR, offset(timeValue(2026, 11, 4, 12)));
  EXPECT_EQ(timeValue(2026, 1, 1, 8), utcTime(timeValue(2026, 1, 1), cache));
  EXPECT_EQ(timeValue(2026, 11, 1, 7), utcTime(timeValue(2026, 11, 1), cache));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, CloselySpacedOffsetTransitionsTest) {
  if (!hasIANATimezoneDB())
    return;
  setTimeZone("America/Boa_Vista");
  LocalTimeOffsetCache cache;

  // DST lasted only 6 days 23 hours. Equal offsets on either side must not
  // cause the cache to merge an interval across both transitions.
  EXPECT_EQ(
      -4 * MS_PER_HOUR, cache.totalOffsetInMs(timeValue(2000, 10, 7, 12)));
  EXPECT_EQ(
      -4 * MS_PER_HOUR, cache.totalOffsetInMs(timeValue(2000, 10, 16, 12)));
  EXPECT_EQ(
      -3 * MS_PER_HOUR, cache.totalOffsetInMs(timeValue(2000, 10, 10, 12)));

  const double start = timeValue(2000, 10, 8, 4);
  const double end = timeValue(2000, 10, 15, 3);
  EXPECT_EQ(-4 * MS_PER_HOUR, cache.totalOffsetInMs(start - 1));
  EXPECT_EQ(-3 * MS_PER_HOUR, cache.totalOffsetInMs(start));
  EXPECT_EQ(-3 * MS_PER_HOUR, cache.totalOffsetInMs(end - 1));
  EXPECT_EQ(-4 * MS_PER_HOUR, cache.totalOffsetInMs(end));

  // Look up the times just outside the DST period first. A window longer than
  // 6 days 23 hours would merge them into one interval.
  LocalTimeOffsetCache freshCache;
  EXPECT_EQ(-4 * MS_PER_HOUR, freshCache.totalOffsetInMs(start - 1));
  EXPECT_EQ(-4 * MS_PER_HOUR, freshCache.totalOffsetInMs(end));
  EXPECT_EQ(
      -3 * MS_PER_HOUR,
      freshCache.totalOffsetInMs(timeValue(2000, 10, 10, 12)));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, LocalTimeRangeBoundaryTest) {
  const struct {
    const char *zone;
    double offset;
  } cases[] = {{"JST-9", 9 * MS_PER_HOUR}, {"PST8", -8 * MS_PER_HOUR}};
  const double limit = TIME_RANGE_MS;
  const double localLimit = limit + 2 * MS_PER_DAY;

  for (const auto &test : cases) {
    setTimeZone(test.zone);
    LocalTimeOffsetCache cache;
    for (double utc : {-limit, limit}) {
      const double local = localTime(utc, cache);
      EXPECT_EQ(utc + test.offset, local);
      EXPECT_EQ(utc, utcTime(local, cache));
    }

    // A local value outside TimeClip can still represent an in-range UTC time.
    const double outside = test.offset > 0 ? limit + 1 : -limit - 1;
    EXPECT_EQ(outside - test.offset, utcTime(outside, cache));

    // At the local-input bound, the outer probe reaches 96 hours past TimeClip.
    for (double local : {-localLimit, localLimit}) {
      EXPECT_EQ(test.offset, cache.getLocalTimeOffset(local, TimeType::Local));
    }
    EXPECT_TRUE(std::isnan(utcTime(localLimit + 1, cache)));
    EXPECT_TRUE(std::isnan(utcTime(-localLimit - 1, cache)));
    EXPECT_TRUE(std::isnan(localTime(limit + 1, cache)));
    EXPECT_TRUE(std::isnan(localTime(-limit - 1, cache)));
  }

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, InvalidTimeOffsetTest) {
  setTimeZone("JST-9");
  LocalTimeOffsetCache cache;
  const double invalidTimes[] = {
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::max(),
      -std::numeric_limits<double>::max()};
  for (double t : invalidTimes) {
    EXPECT_TRUE(std::isnan(localTime(t, cache)));
    EXPECT_TRUE(std::isnan(utcTime(t, cache)));
    EXPECT_TRUE(std::isnan(cache.getLocalTimeOffset(t, TimeType::Utc)));
    EXPECT_TRUE(std::isnan(cache.getLocalTimeOffset(t, TimeType::Local)));
  }

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, Int32EpochOffsetBoundaryTest) {
  // Windows passes only [0, 2^31 - 1] seconds to the OS and a 32-bit time_t
  // only [-(2^31 - 1), 2^31 - 1] seconds; other times use an equivalent year.
  const struct {
    const char *zone;
    double offset;
  } cases[] = {{"JST-9", 9 * MS_PER_HOUR}, {"PST8", -8 * MS_PER_HOUR}};
  const int64_t maxInt32Time =
      int64_t{std::numeric_limits<int32_t>::max()} * 1000;
  const int64_t times[] = {
      -maxInt32Time - 1,
      -maxInt32Time,
      -maxInt32Time + 1,
      -1,
      0,
      1,
      maxInt32Time - 1,
      maxInt32Time,
      maxInt32Time + 1};
  for (const auto &test : cases) {
    setTimeZone(test.zone);
    LocalTimeOffsetCache cache;
    for (int64_t t : times) {
      EXPECT_EQ(test.offset, cache.totalOffsetInMs(t));
      EXPECT_EQ(t + test.offset, localTime(t, cache));
      EXPECT_EQ(t, utcTime(t + test.offset, cache));
    }
  }

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, EpochOffsetContinuityTest) {
  if (!hasIANATimezoneDB())
    return;
  // Britain kept +01:00 all year from 1968 to 1971, so the offset is the same
  // on both sides of the epoch, and local midnight on 1970-01-01 exists.
  setTimeZone("Europe/London");
  LocalTimeOffsetCache cache;
  EXPECT_EQ(MS_PER_HOUR, cache.totalOffsetInMs(-1000));
  EXPECT_EQ(MS_PER_HOUR, cache.totalOffsetInMs(0));
  EXPECT_EQ(-MS_PER_HOUR, utcTime(timeValue(1970, 1, 1), cache));
  EXPECT_EQ(timeValue(1970, 1, 1), localTime(-MS_PER_HOUR, cache));
  const double halfPast = timeValue(1970, 1, 1, 0, 30);
  EXPECT_EQ(halfPast, localTime(utcTime(halfPast, cache), cache));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, OffsetOutsideEquivalentYearRangeTest) {
  if (!hasIANATimezoneDB())
    return;
  // Moscow used +03:00 in 1969 and has again since October 2014, but used
  // +04:00 in 2014 itself, the equivalent year of both 1969 and 2042.
  setTimeZone("Europe/Moscow");
  LocalTimeOffsetCache cache;
  EXPECT_EQ(3 * MS_PER_HOUR, cache.totalOffsetInMs(timeValue(1969, 7, 1)));
  // A 32-bit time_t cannot represent 2042, so it still uses 2014.
  if (sizeof(std::time_t) >= sizeof(int64_t))
    EXPECT_EQ(3 * MS_PER_HOUR, cache.totalOffsetInMs(timeValue(2042, 7, 1)));

  hermes::oscompat::unset_env("TZ");
}

#ifndef _WINDOWS
TEST(DateUtilTest, TimeZoneChangeOnCacheMissTest) {
  setTimeZone("PST8");
  LocalTimeOffsetCache cache;
  const double january = timeValue(2019, 1, 15);
  const double july = timeValue(2019, 7, 15);
  EXPECT_EQ(-8 * MS_PER_HOUR, cache.totalOffsetInMs(january));

  // A miss in the new timezone must invalidate previously cached offsets.
  setTimeZone("JST-9");
  EXPECT_EQ(9 * MS_PER_HOUR, cache.totalOffsetInMs(july));
  EXPECT_EQ(9 * MS_PER_HOUR, cache.totalOffsetInMs(january));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, TimeZoneChangeWithMatchingOffsetTest) {
  // While the first zone observes DST, both zones use -04:00, so only the DST
  // flag at the reference time reveals the change. Otherwise, the offsets
  // differ there.
  setTimeZone("EST5EDT,M3.2.0,M11.1.0");
  LocalTimeOffsetCache cache;
  const double january = timeValue(2019, 1, 15);
  EXPECT_EQ(-5 * MS_PER_HOUR, cache.totalOffsetInMs(january));

  setTimeZone("AST4");
  EXPECT_EQ(-4 * MS_PER_HOUR, cache.totalOffsetInMs(timeValue(2019, 7, 15)));
  EXPECT_EQ(-4 * MS_PER_HOUR, cache.totalOffsetInMs(january));

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, UndetectedTimeZoneChangeTest) {
  if (!hasIANATimezoneDB())
    return;
  // Europe/Lisbon and Europe/London have had the same offsets and DST flags
  // since 1996, so the reference time cannot detect a change between them. In
  // March 1993, Lisbon changed from +01:00 to +02:00.
  setTimeZone("Europe/Lisbon");
  LocalTimeOffsetCache cache;
  EXPECT_EQ(MS_PER_HOUR, cache.totalOffsetInMs(timeValue(1993, 3, 25, 1)));
  EXPECT_EQ(2 * MS_PER_HOUR, cache.totalOffsetInMs(timeValue(1993, 3, 28, 13)));

  // London used +00:00 between the cached times, an offset that matches
  // neither cached interval, so the cache is stale and must start over.
  setTimeZone("Europe/London");
  EXPECT_EQ(0, cache.totalOffsetInMs(timeValue(1993, 3, 27, 13)));
  EXPECT_EQ(MS_PER_HOUR, cache.totalOffsetInMs(timeValue(1993, 3, 28, 13)));

  hermes::oscompat::unset_env("TZ");
}
#endif

TEST(DateUtilTest, LargeTimeZoneOffsetTest) {
  // Some C libraries accept offsets of 48 hours or more, which local-to-UTC
  // conversion does not support. Near the ends of the range, such an offset
  // must not move a lookup outside the times that can be queried.
  setTimeZone("<+50>-50");
  LocalTimeOffsetCache cache;
  const double localLimit = TIME_RANGE_MS + 2 * MS_PER_DAY;
  for (double local : {-localLimit, localLimit}) {
    const double offset = cache.getLocalTimeOffset(local, TimeType::Local);
    EXPECT_TRUE(std::isnan(offset) || std::abs(offset) <= 50 * MS_PER_HOUR);
  }

  hermes::oscompat::unset_env("TZ");
}

TEST(DateUtilTest, HoursMinutesSecondsMsTest) {
  // Uses the formulae from spec, perform sanity check.
  double t = 0;
  EXPECT_EQ(0, hourFromTime(t));
  EXPECT_EQ(0, minFromTime(t));
  EXPECT_EQ(0, secFromTime(t));
  EXPECT_EQ(0, msFromTime(t));

  // Tue, 14 Mar 2017 13:13:38.245 GMT
  t = 1489497218245;
  EXPECT_EQ(13, hourFromTime(t));
  EXPECT_EQ(13, minFromTime(t));
  EXPECT_EQ(38, secFromTime(t));
  EXPECT_EQ(245, msFromTime(t));
}

TEST(DateUtilTest, MakeTimeTest) {
  EXPECT_EQ(0, makeTime(0, 0, 0, 0));
  EXPECT_EQ(MS_PER_DAY, makeTime(24, 0, 0, 0));
  EXPECT_EQ(86399999, makeTime(23, 59, 59, 999));
  EXPECT_EQ(59126000, makeTime(16, 25, 26, 0));
  EXPECT_EQ(59126746, makeTime(16, 25, 26, 746));
}

TEST(DateUtilTest, MakeDayTest) {
  EXPECT_EQ(0, makeDay(1970, 0, 1)); // Jan 1, 1970
  EXPECT_EQ(-25567, makeDay(1900, 0, 1)); // Jan 1, 1900
  EXPECT_EQ(12403, makeDay(2003, 11, 17)); // Dec 17, 2003
  EXPECT_EQ(17240, makeDay(2017, 2, 15)); // Mar 15, 2017
}

TEST(DateUtilTest, MakeDateTest) {
  EXPECT_EQ(0, makeDate(0, 0)); // Jan 1, 1970
  EXPECT_EQ(-2208988799000, makeDate(-25567, 1000)); // Jan 1, 1900 + 1 sec
  EXPECT_EQ(1489572389000, makeDate(17240, 36389000)); // Mar 15, 2017 10:06:29
}

TEST(DateUtilTest, TimeClipTest) {
  EXPECT_TRUE(std::isnan(timeClip(std::numeric_limits<double>::infinity())));
  EXPECT_TRUE(std::isnan(timeClip(-std::numeric_limits<double>::infinity())));
  EXPECT_TRUE(std::isnan(timeClip(8.7e15)));

  EXPECT_FALSE(std::signbit(timeClip(+0.0)));
  EXPECT_EQ(0, timeClip(+0.0));
  EXPECT_FALSE(std::signbit(timeClip(-0.0)));
  EXPECT_EQ(0, timeClip(-0.0));

  EXPECT_EQ(1, timeClip(1.5));
  EXPECT_EQ(-1, timeClip(-1.5));
}
} // anonymous namespace
