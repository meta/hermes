/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSLib/DateUtil.h"
#include "hermes/VM/JSLib/DateCache.h"

#include "hermes/Platform/Unicode/PlatformUnicode.h"
#include "hermes/Support/Compiler.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/VM/CallResult.h"
#include "hermes/VM/SmallXString.h"

#include "llvh/Support/ErrorHandling.h"
#include "llvh/Support/Format.h"
#include "llvh/Support/raw_ostream.h"

#include <cassert>
#include <cctype>
#include <cmath>

namespace hermes {
namespace vm {

/// Set \p quot to the largest integral value that is smaller than or equal to
/// the algebraic quotient of \p x divided by \p y.
/// Set \p rem to the floor modulus of \p x divided by \p y.
static void floorDivMod(int64_t x, int64_t y, int64_t *quot, int64_t *rem) {
  int64_t q = x / y;
  // signs are different && not evenly divisable
  if ((x ^ y) < 0 && q * y != x) {
    q--;
  }
  *quot = q;
  *rem = x - y * q;
}

/// \return the floor modulus of \p x divided by \p y.
static int64_t floorMod(int64_t x, int64_t y) {
  int64_t quot, rem;
  floorDivMod(x, y, &quot, &rem);
  return rem;
}

/// Perform the fmod operation and adjusts the result so that it's not negative.
/// Useful in computing dates before Jan 1 1970.
static inline double posfmod(double x, double y) {
  double result = std::fmod(x, y);
  return result < 0 ? result + y : result;
}

//===----------------------------------------------------------------------===//
// Current time

std::chrono::milliseconds::rep curTime() {
  // Use std::chrono here because we need millisecond precision, which
  // std::time() fails to provide.
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.2

double day(double t) {
  return std::floor(t / MS_PER_DAY);
}

double timeWithinDay(double t) {
  return posfmod(t, MS_PER_DAY);
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.3

/// \return true if year \p y is a leap year.
static bool isLeapYear(double y) {
  if (std::fmod(y, 4) != 0) {
    return false;
  }
  // y % 4 == 0
  if (std::fmod(y, 100) != 0) {
    return true;
  }
  // y % 100 == 0
  if (std::fmod(y, 400) != 0) {
    return false;
  }
  // y % 400 == 0
  return true;
}

/// \return true if year \p y is a leap year.
static bool isLeapYear(int32_t y) {
  if (y % 4 != 0) {
    return false;
  }
  // y % 4 == 0
  if (y % 100 != 0) {
    return true;
  }
  // y % 100 == 0
  if (y % 400 != 0) {
    return false;
  }
  // y % 400 == 0
  return true;
}

uint32_t daysInYear(double y) {
  return isLeapYear(y) ? 366 : 365;
}

double dayFromYear(double y) {
  // Use the formula given in the spec for computing the day from year.
  return 365 * (y - 1970) + std::floor((y - 1969) / 4.0) -
      std::floor((y - 1901) / 100.0) + std::floor((y - 1601) / 400.0);
}

double timeFromYear(double y) {
  return MS_PER_DAY * dayFromYear(y);
}

double yearFromTime(double t) {
  if (!std::isfinite(t)) {
    // If t is infinitely in the future be done.
    return t;
  }

  // Estimate y using the average year length.
  double y = std::floor(t / (MS_PER_DAY * 365.2425)) + 1970;

  // Actual time for year y.
  double yt = timeFromYear(y);

  while (yt > t) {
    // Estimate was too high, decrement until we're correct.
    --y;
    yt = timeFromYear(y);
  }
  while (yt + daysInYear(y) * MS_PER_DAY <= t) {
    // t is more than a year away from the start of y.
    // Increment y until we're correct.
    ++y;
    yt = timeFromYear(y);
  }

  assert(
      timeFromYear(y) <= t && timeFromYear(y + 1) > t &&
      "yearFromTime incorrect");
  return y;
}

bool inLeapYear(double t) {
  return daysInYear(yearFromTime(t)) == 366;
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.4

uint32_t monthFromTime(double t) {
  double dayWithinYear = day(t) - dayFromYear(yearFromTime(t));
  constexpr int8_t kDaysInMonthNonLeap[11] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30};
  double curDay = 0.0;
  for (uint32_t i = 0; i < 11; ++i) {
    curDay += (i == 1 && inLeapYear(t)) ? kDaysInMonthNonLeap[i] + 1
                                        : kDaysInMonthNonLeap[i];
    if (dayWithinYear < curDay)
      return i;
  }
  // Must be December.
  return 11;
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.5

/// Gives the offset of the first day in month m.
/// \param leap indicates if \p m falls in a leap year.
static uint32_t dayFromMonth(uint32_t m, bool leap) {
  static const uint16_t standardTable[]{
      0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
  static const uint16_t leapYearTable[]{
      0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};
  assert(m < 12 && "invalid month supplied to dayFromMonth");
  return leap ? leapYearTable[m] : standardTable[m];
}

double dateFromTime(double t) {
  double dayWithinYear = day(t) - dayFromYear(yearFromTime(t));
  bool leap = inLeapYear(t);
  return dayWithinYear - dayFromMonth(monthFromTime(t), leap) + 1;
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.6

int32_t weekDay(double t) {
  return posfmod((day(t) + 4), 7);
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.8

static const int32_t DAYS_IN_1_YEAR = 365;
static const int32_t DAYS_IN_4_YEARS = DAYS_IN_1_YEAR * 4 + 1;
static const int32_t DAYS_IN_100_YEARS = DAYS_IN_4_YEARS * 25 - 1;
static const int32_t DAYS_IN_400_YEARS = DAYS_IN_100_YEARS * 4 + 1;
// ES5.1 15.9.1.1
// The actual range of times supported by ECMAScript Date objects is slightly
// smaller: exactly –100,000,000 days to 100,000,000 days measured relative to
// midnight at the beginning of 01 January, 1970 UTC.
static const int32_t BASE_YEAR = -274000;
static const int32_t DAYS_FROM_BASE_YEAR_TO_1970 =
    (-BASE_YEAR / 400) * DAYS_IN_400_YEARS + 4 * DAYS_IN_400_YEARS +
    3 * DAYS_IN_100_YEARS + 17 * DAYS_IN_4_YEARS + 2 * DAYS_IN_1_YEAR;

/// \p year will be set to the year the \p epochDays falls in.
/// \p yearAsEpochDays will be set to the number of days from 1970-01-01 to Jan
///    1st of \p year (e.g. 0 represents 1970, 365 represents 1971,
///    1096 represents 1973).
/// \p dayOfYear will be set to the date the \p epochDays fall on, represented
///    as number of days since Jan 1st (e.g. 0 represents Jan 1;
///    59 represents Feb 29 if \p year is a leap year, Mar 1 otherwise).
static void decomposeEpochDays(
    int32_t epochDays,
    int32_t *year,
    int32_t *yearAsEpochDays,
    int32_t *dayOfYear) {
  *year = BASE_YEAR;
  *yearAsEpochDays = -DAYS_FROM_BASE_YEAR_TO_1970;
  *dayOfYear = epochDays + DAYS_FROM_BASE_YEAR_TO_1970;

  int32_t countOf400Years = *dayOfYear / DAYS_IN_400_YEARS;
  *year += countOf400Years * 400;
  *yearAsEpochDays += countOf400Years * DAYS_IN_400_YEARS;
  *dayOfYear -= countOf400Years * DAYS_IN_400_YEARS;

  int32_t countOf100Years = *dayOfYear / DAYS_IN_100_YEARS;
  *year += countOf100Years * 100;
  *yearAsEpochDays += countOf100Years * DAYS_IN_100_YEARS;
  *dayOfYear -= countOf100Years * DAYS_IN_100_YEARS;

  int32_t countOf4Years = *dayOfYear / DAYS_IN_4_YEARS;
  *year += countOf4Years * 4;
  *yearAsEpochDays += countOf4Years * DAYS_IN_4_YEARS;
  *dayOfYear -= countOf4Years * DAYS_IN_4_YEARS;

  int32_t countOf1Year = *dayOfYear / DAYS_IN_1_YEAR;
  *year += countOf1Year * 1;
  *yearAsEpochDays += countOf1Year * DAYS_IN_1_YEAR;
  *dayOfYear -= countOf1Year * DAYS_IN_1_YEAR;
}

static int32_t weekDayFromEpochDays(int32_t epochDays) {
  return floorMod(epochDays + 4, 7);
}

static int32_t epochDaysForYear2006To2033[] = {
    13149, 13514, 13879, 14245, 14610, 14975, 15340, 15706, 16071, 16436,
    16801, 17167, 17532, 17897, 18262, 18628, 18993, 19358, 19723, 20089,
    20454, 20819, 21184, 21550, 21915, 22280, 22645, 23011};

/// Returns an equivalent year, represented as number of days since 1970-01-01,
/// for the purpose of determining DST using the rules in ES5.1 15.9.1.8
/// Daylight Saving Time Adjustment.
/// The returned year is guaranteed to be in range [1970, 2037].
/// \p yearAsEpochDays must be set to the number of days from 1970-01-01 to Jan
/// 1st of \p year.
static int32_t equivalentYearAsEpochDays(
    int32_t year,
    int32_t yearAsEpochDays) {
  if (year >= 1970 && year <= 2037) {
    // This avoids surprising results for current year and nearby years.
    // It also reduces overhead for the most common cases.
    return yearAsEpochDays;
  }
  int32_t wkDay = weekDayFromEpochDays(yearAsEpochDays);
  // * 2006-01-01 and 2012-01-01 are both Sundays.
  // * Starting 2006/2012, for the 40 years after it, there is a leap year
  //   every 4 years, with no exceptions (i.e. 100 year rules).
  //   This is the basis of the following two bullet points.
  // * any_int * 12 % 28 is guaranteed to be a multiple of 4.
  //   As a result, the following operations does not change
  //   whether a year is a leap year or not.
  // * Every 4 years, there is 1 leap year and 3 non-leap years.
  //   (365*3+366) % 7 = 5. This is the number of extra days on top of
  //   full weeks we get every 4 years.
  //   * After 28 (4 * 7) years, we get (5 * 7) % 7 = 0 day of extra day.
  //     This is why subtracting 28 years does not change whether a year
  //     is a leap year.
  //   * After 12 (4 * 3) years, we get (5 * 3) % 7 = 1 day of extra day.
  //     That's why adding 12 years increments weekday by 1.
  int32_t eqYear = (isLeapYear(year) ? 2012 : 2006) + (wkDay * 12) % 28;
  // Find the year in the range 2006..2033 that is equivalent mod 28.
  // This is to avoid anything above year 2037.
  eqYear = 2006 + (eqYear - 2006) % 28;
  return epochDaysForYear2006To2033[eqYear - 2006];
}

/// Returns an equivalent time for the purpose of determining DST using the
/// rules in ES5.1 15.9.1.8 Daylight Saving Time Adjustment
///
/// \p epochSecs must be within the ECMAScript time range, extended by twice
/// kLocalTimeOffsetBoundMs for local-to-UTC conversion and its offset probes.
///
/// Some library calls doesn't work when the input date-time cannot be
/// represented as a 32-bit non-negative number of seconds since
/// 1970-01-01T00:00:00. (e.g. std::localtime on Windows)
///
/// Note: not "static" so that it can be tested directly.
int32_t detail::equivalentTime(int64_t epochSecs) {
  // The math behind this implementation is similar to the EquivalentTime
  // function in https://github.com/v8/v8/blob/master/src/date.h
  [[maybe_unused]] constexpr int64_t maxTime =
      TIME_RANGE_SECS + 2 * kLocalTimeOffsetBoundMs / MS_PER_SECOND;
  assert(epochSecs >= -maxTime && epochSecs <= maxTime);
  int64_t epochDays, secsOfDay;
  floorDivMod(epochSecs, SECONDS_PER_DAY, &epochDays, &secsOfDay);
  int32_t year, yearAsEpochDays, dayOfYear;
  // Narrowing of epochDays will not result in truncation
  decomposeEpochDays(epochDays, &year, &yearAsEpochDays, &dayOfYear);
  int32_t eqYearAsEpochDays = equivalentYearAsEpochDays(year, yearAsEpochDays);
  return (eqYearAsEpochDays + dayOfYear) * SECONDS_PER_DAY + secsOfDay;
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.9

/// https://tc39.es/ecma262/#sec-localtime
/// Conversion from UTC to local time.
double localTime(double t, LocalTimeOffsetCache &localTimeOffsetCache) {
  // The spec requires that localTime() accepts only finite time value, but for
  // simplicity, we do the check here instead of the caller site.
  if (!std::isfinite(t)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return t + localTimeOffsetCache.getLocalTimeOffset(t, TimeType::Utc);
}

/// https://tc39.es/ecma262/#sec-utc-t
/// Conversion from local time to UTC.
///
/// There is time ambiguity when converting local time to UTC time. For example,
/// when offsets change in backward direction (transition from DST), the same
/// local time is repeated, and mapped to two different UTC times. When
/// offsets change in forward direction (transition to DST), local times are
/// skipped. ECMA262 requires that for both cases, time \t should be interpreted
/// using the time zone offset *before* the transition.
/// Consider time zone `America/New_York`, 1:30 AM on 5 November 2017 is
/// repeated twice, it should be converted to UTC epoch 1509859800000
/// (1:30 AM UTC-04) instead of 1509863400000 (1:30 AM UTC-05). And 2:30 AM on
/// 12 March 2017 is skipped, it should be interpreted as 2:30 AM UTC-05
/// instead of 3:30 AM UTC-04. However, in this case, both have the same UTC
/// epoch.
double utcTime(double t, LocalTimeOffsetCache &localTimeOffsetCache) {
  if (!std::isfinite(t)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return t - localTimeOffsetCache.getLocalTimeOffset(t, TimeType::Local);
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.10

double hourFromTime(double t) {
  return posfmod(std::floor(t / MS_PER_HOUR), HOURS_PER_DAY);
}

double minFromTime(double t) {
  return posfmod(std::floor(t / MS_PER_MINUTE), MINUTES_PER_HOUR);
}

double secFromTime(double t) {
  return posfmod(std::floor(t / MS_PER_SECOND), SECONDS_PER_MINUTE);
}

double msFromTime(double t) {
  return posfmod(t, MS_PER_SECOND);
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.11

double makeTime(double hour, double min, double sec, double ms) {
  if (!std::isfinite(hour) || !std::isfinite(min) || !std::isfinite(sec) ||
      !std::isfinite(ms)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double h = std::trunc(hour);
  double m = std::trunc(min);
  double s = std::trunc(sec);
  double milli = trunc(ms);
  return h * MS_PER_HOUR + m * MS_PER_MINUTE + s * MS_PER_SECOND + milli;
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.12

double makeDay(double year, double month, double date) {
  if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double y = std::trunc(year);
  double m = std::trunc(month);
  double dt = std::trunc(date);

  // Actual year and month, accounting for the month being greater than 11.
  // Need to do this because it changes the leap year calculations.
  double ym = y + std::floor(m / 12);
  double mn = posfmod(m, 12);

  bool leap = isLeapYear(ym);

  // Day of the first day of the year ym.
  double yd = std::floor(timeFromYear(ym) / MS_PER_DAY);
  // Day of the first day of the month mn.
  double md = dayFromMonth(mn, leap);

  // Final date is the first day of the year, offset by the first day of the
  // month, with dt - 1 to account for the day within the month.
  return yd + md + dt - 1;
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.13

double makeDate(double day, double t) {
  if (!std::isfinite(day) || !std::isfinite(t)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Some compilers may contract the multiplication and addition into a single
  // FMA when they are part of the same expression. This would result in
  // non-standard results, so to avoid it, split them into separate expressions.
  // Note that this applies only when compiling with -ffp-contract=on. If
  // -ffp-contract=fast is used, the compiler will still be permitted to emit an
  // FMA operation for the separate expressions.
  double dayMs = day * MS_PER_DAY;
  return dayMs + t;
}

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.14

double timeClip(double t) {
  if (!std::isfinite(t) || std::abs(t) > 8.64e15) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Truncate and make -0 into +0.
  return std::trunc(t) + 0;
}

//===----------------------------------------------------------------------===//
// toString Functions

void dateToISOString(double t, double, llvh::SmallVectorImpl<char> &buf) {
  llvh::raw_svector_ostream os{buf};

  /// Make these ints here because we're printing and we have bounds on
  /// their values. Makes printing very easy.
  int32_t y = yearFromTime(t);
  int32_t m = monthFromTime(t) + 1; // monthFromTime(t) is 0-indexed.
  int32_t d = dateFromTime(t);

  if (y < 0 || y > 9999) {
    // Handle extended years.
    os << llvh::format("%+07d-%02d-%02d", y, m, d);
  } else {
    os << llvh::format("%04d-%02d-%02d", y, m, d);
  }
}

void timeToISOString(double t, double tza, llvh::SmallVectorImpl<char> &buf) {
  llvh::raw_svector_ostream os{buf};

  /// Make all of these ints here because we're printing and we have bounds on
  /// their values. Makes printing very easy.
  int32_t h = hourFromTime(t);
  int32_t min = minFromTime(t);
  int32_t s = secFromTime(t);
  int32_t ms = msFromTime(t);

  if (tza == 0) {
    // Zulu time, output Z as the time zone.
    os << llvh::format("%02d:%02d:%02d.%03dZ", h, min, s, ms);
  } else {
    // Calculate the +HH:mm expression for the time zone adjustment.
    // First account for the sign, then perform calculations on positive TZA.
    char sign = tza >= 0 ? '+' : '-';
    double tzaPos = std::abs(tza);
    int32_t tzh = hourFromTime(tzaPos);
    int32_t tzm = minFromTime(tzaPos);
    os << llvh::format(
        "%02d:%02d:%02d.%03d%c%02d:%02d", h, min, s, ms, sign, tzh, tzm);
  }
}

static void datetimeToISOString(
    double t,
    double tza,
    llvh::SmallVectorImpl<char> &buf,
    char separator) {
  dateToISOString(t, tza, buf);
  buf.push_back(separator);
  timeToISOString(t, tza, buf);
}

void datetimeToISOString(
    double t,
    double tza,
    llvh::SmallVectorImpl<char> &buf) {
  return datetimeToISOString(t, tza, buf, 'T');
}

void datetimeToLocaleString(double t, llvh::SmallVectorImpl<char16_t> &buf) {
  return platform_unicode::dateFormat(t, true, true, buf);
}

void dateToLocaleString(double t, llvh::SmallVectorImpl<char16_t> &buf) {
  return platform_unicode::dateFormat(t, true, false, buf);
}

void timeToLocaleString(double t, llvh::SmallVectorImpl<char16_t> &buf) {
  return platform_unicode::dateFormat(t, false, true, buf);
}

// ES9.0 Table 46
static const char *const weekdayNames[7]{
    "Sun",
    "Mon",
    "Tue",
    "Wed",
    "Thu",
    "Fri",
    "Sat",
};

// ES9.0 Table 47
static const char *const monthNames[12]{
    "Jan",
    "Feb",
    "Mar",
    "Apr",
    "May",
    "Jun",
    "Jul",
    "Aug",
    "Sep",
    "Oct",
    "Nov",
    "Dec",
};

void dateString(double t, double, llvh::SmallVectorImpl<char> &buf) {
  llvh::raw_svector_ostream os{buf};

  // Make these ints here because we're printing and we have bounds on
  // their values. Makes printing very easy.
  int32_t y = yearFromTime(t);
  int32_t m = monthFromTime(t); // monthFromTime(t) is 0-indexed.
  int32_t d = dateFromTime(t);
  int32_t wd = weekDay(t);

  // 7. Return the string-concatenation of weekday, the code unit 0x0020
  // (SPACE), month, the code unit 0x0020 (SPACE), day, the code unit 0x0020
  // (SPACE), and year.
  // Example: Mon Jul 22 2019
  os << llvh::format("%s %s %02d %0.4d", weekdayNames[wd], monthNames[m], d, y);
}

void timeString(double t, double tza, llvh::SmallVectorImpl<char> &buf) {
  llvh::raw_svector_ostream os{buf};

  int32_t hour = hourFromTime(t);
  int32_t minute = minFromTime(t);
  int32_t second = secFromTime(t);

  // Example: 15:50:49 GMT
  os << llvh::format("%02d:%02d:%02d GMT", hour, minute, second);
}

void timeZoneString(double t, double tza, llvh::SmallVectorImpl<char> &buf) {
  llvh::raw_svector_ostream os{buf};

  // We've already computed the TZA, so use that as the offset.
  double offset = tza;

  // 4. If offset >= 0, let offsetSign be "+"; otherwise, let offsetSign be "-".
  char offsetSign = offset >= 0 ? '+' : '-';

  // 5. Let offsetMin be the String representation of MinFromTime(abs(offset)),
  // formatted as a two-digit decimal number, padded to the left with a zero if
  // necessary.
  int32_t offsetMin = minFromTime(std::abs(offset));

  // 6. Let offsetHour be the String representation of
  // HourFromTime(abs(offset)), formatted as a two-digit decimal number, padded
  // to the left with a zero if necessary.
  int32_t offsetHour = hourFromTime(std::abs(offset));

  // 7. Let tzName be an implementation-defined string that is either the empty
  // string or the string-concatenation of the code unit 0x0020 (SPACE), the
  // code unit 0x0028 (LEFT PARENTHESIS), an implementation-dependent timezone
  // name, and the code unit 0x0029 (RIGHT PARENTHESIS).
  // TODO: Make this something other than empty string.

  // 8. Return the string-concatenation of offsetSign, offsetHour, offsetMin,
  // and tzName.
  // Example: -0700
  os << llvh::format("%c%02d%02d", offsetSign, offsetHour, offsetMin);
}

void dateTimeString(double tv, double tza, llvh::SmallVectorImpl<char> &buf) {
  llvh::raw_svector_ostream os{buf};
  dateString(tv, tza, buf);
  // Return the string-concatenation of DateString(t), the code unit 0x0020
  // (SPACE), TimeString(t), and TimeZoneString(tv).
  // Example: Mon Jul 22 2019 15:51:50 GMT-0700
  os << " ";
  timeString(tv, tza, buf);
  timeZoneString(tv, tza, buf);
}

void dateTimeUTCString(
    double tv,
    double tza,
    llvh::SmallVectorImpl<char> &buf) {
  llvh::raw_svector_ostream os{buf};

  // Make these ints here because we're printing and we have bounds on
  // their values. Makes printing very easy.
  int32_t y = yearFromTime(tv);
  int32_t m = monthFromTime(tv); // monthFromTime(t) is 0-indexed.
  int32_t d = dateFromTime(tv);
  int32_t wd = weekDay(tv);

  // 8. Return the string-concatenation of weekday, ",", the code unit 0x0020
  // (SPACE), day, the code unit 0x0020 (SPACE), month, the code unit 0x0020
  // (SPACE), year, the code unit 0x0020 (SPACE), and TimeString(tv).
  // Example: Mon Jul 22 2019 15:51:50 GMT
  os << llvh::format(
      "%s, %02d %s %0.4d ", weekdayNames[wd], d, monthNames[m], y);
  timeString(tv, tza, buf);
}

void timeTZString(double tv, double tza, llvh::SmallVectorImpl<char> &buf) {
  // Return the string-concatenation of TimeString(t) and TimeZoneString(tv).
  // Example: 15:51:50 GMT-0700
  timeString(tv, tza, buf);
  timeZoneString(tv, tza, buf);
}

//===----------------------------------------------------------------------===//
// Date parsing

/// \return true if c represents a digit between 0 and 9.
static inline bool isDigit(char16_t c) {
  return u'0' <= c && c <= u'9';
}

/// \return true if c represents an alphabet letter.
static inline bool isAlpha(char16_t c) {
  c |= 'a' ^ 'A'; // Lowercase
  return 'a' <= c && c <= 'z';
}

/// Read a number from the iterator at \p it into \p x.
/// Can read integers that consist entirely of digits.
/// \param[in,out] it is modified to the new start point of the scan if
/// successful.
/// \param end the end of the string.
/// \param[out] x modified to contain the scanned integer.
/// \return true if successful, false if failed.
template <class InputIter>
static bool scanInt(InputIter &it, const InputIter end, int32_t &x) {
  llvh::SmallString<16> str{};
  if (it == end) {
    return false;
  }
  for (; it != end && isDigit(*it); ++it) {
    str += static_cast<char>(*it);
  }
  llvh::StringRef ref{str};
  // getAsInteger returns false to signify success.
  return !ref.getAsInteger(10, x);
}

static double parseISODate(
    StringView u16str,
    LocalTimeOffsetCache &localTimeOffsetCache) {
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();

  auto it = u16str.begin();
  auto end = u16str.end();

  // Used to indicate the negation multiplier on an integer.
  // 1 for positive, -1 for negative.
  double sign;

  // Initialize these fields to their defaults.
  int32_t y, m{1}, d{1}, h{0}, min{0}, s{0}, ms{0}, tzh{0}, tzm{0};

  auto consume = [&](char16_t ch) {
    if (it != end && *it == ch) {
      ++it;
      return true;
    }
    return false;
  };

  // Must read the year.
  sign = 1;
  if (consume(u'+')) {
    sign = 1;
  } else if (consume(u'-')) {
    sign = -1;
  }
  if (!scanInt(it, end, y)) {
    return nan;
  }
  y *= sign;
  if (consume(u'-')) {
    // Try to read the month.
    if (!scanInt(it, end, m)) {
      return nan;
    }
    if (consume(u'-')) {
      // Try to read the date.
      if (!scanInt(it, end, d)) {
        return nan;
      }
    }
  }

  // See if there's a time.
  if (consume(u'T') || consume(u' ')) {
    // Hours and minutes must exist.
    if (!scanInt(it, end, h)) {
      return nan;
    }
    if (!consume(u':')) {
      return nan;
    }
    if (!scanInt(it, end, min)) {
      return nan;
    }
    if (consume(u':')) {
      // Try to read seconds.
      if (!scanInt(it, end, s)) {
        return nan;
      }
      if (consume(u'.')) {
        // Try to read fraction of a second.
        if (it == end || !isDigit(*it)) {
          return nan;
        }

        // Position of the milliseconds counter.
        // Start at the 100s place and discard anything after the third digit by
        // dividing by 10 every iteration.
        int32_t pos = 100;

        for (; it != end && isDigit(*it); ++it) {
          ms += pos * (*it - '0');
          pos /= 10;
        }
      }
    }

    if (it == end) {
      // ES12 21.4.3.2: When the UTC offset representation is absent, date-only
      // forms are interpreted as a UTC time and date-time forms are interpreted
      // as a local time.
      double t = makeDate(makeDay(y, m - 1, d), makeTime(h, min, s, ms));
      t = utcTime(t, localTimeOffsetCache);
      return t;
    }

    // Try to parse a timezone.
    if (consume(u'Z')) {
      tzh = 0;
      tzm = 0;
    } else {
      // Try to parse the fully specified timezone: [+/-]HH:mm.
      if (consume(u'+')) {
        sign = 1;
      } else if (consume(u'-')) {
        sign = -1;
      } else {
        // Need a + or a -.
        return nan;
      }
      if (it > end - 2) {
        return nan;
      }
      if (!scanInt(it, it + 2, tzh)) {
        return nan;
      }
      tzh *= sign;
      consume(u':');
      if (it > end - 2) {
        return nan;
      }
      if (!scanInt(it, it + 2, tzm)) {
        return nan;
      }
      tzm *= sign;
    }
  }

  if (it != end) {
    // Should be done parsing.
    return nan;
  }

  // Account for the fact that m was 1-indexed and the timezone offset.
  return makeDate(makeDay(y, m - 1, d), makeTime(h - tzh, min - tzm, s, ms));
}

static double parseESDate(
    StringView str,
    LocalTimeOffsetCache &localTimeOffsetCache) {
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  StringView tok = str;

  // Initialize these fields to their defaults.
  int32_t y, m{1}, d{1}, h{0}, min{0}, s{0}, ms{0}, tzh{0}, tzm{0};
  double sign = 1;

  // Example strings to parse:
  // Mon Jul 15 2019 14:33:22 GMT-0700 (PDT)
  // Mon, 15 Jul 2019 14:33:22 GMT
  // The comma, time zone adjustment, and description are optional,

  // Current index we are parsing.
  auto it = str.begin();
  auto end = str.end();

  /// Read a string starting at `it` into `tok`.
  /// \p len the number of characters to scan in the string.
  /// \return true if successful, false if failed.
  auto scanStr = [&str, &tok, &it](int32_t len) -> bool {
    if (it + len > str.end()) {
      return false;
    }
    tok = str.slice(it, it + len);
    it += len;
    return true;
  };

  /// Reads the next \p len characters into `tok`,
  /// but instead of consuming \p len chars, it consumes a single word
  /// whatever how long it is (i.e. until a space or dash is encountered).
  /// e.g.
  ///     &str ="Garbage G MayG"
  ///     scanStrAndSkipWord(3); consumeSpaces();  // &str="G MayG", &tok="Gar"
  ///     scanStrAndSkipWord(3); consumeSpaces();  // &str="MayG"  , &tok="G M"
  ///     scanStrAndSkipWord(3); consumeSpaces();  // &str=""      , &tok="May"
  ///     scanStrAndSkipWord(3);                   // -> false
  /// \return true if successful, false if failed.
  auto scanStrAndSkipWord = [&str, &tok, &it](int32_t len) -> bool {
    if (it + len > str.end())
      return false;
    tok = str.slice(it, it + len);
    while (it != str.end() && !std::isspace(*it) && *it != '-')
      it++;
    return true;
  };

  auto consume = [&](char16_t ch) {
    if (it != str.end() && *it == ch) {
      ++it;
      return true;
    }
    return false;
  };

  auto consumeSpaces = [&]() {
    while (it != str.end() && std::isspace(*it))
      ++it;
  };

  /// Only one dash is ever allowed, and the dash must come first.
  /// There is no limit to the number of spaces.
  /// This is in line with V8's behavior.
  auto consumeSpacesOrDash = [&]() {
    auto first = true;
    while (it != str.end()) {
      if (std::isspace(*it) || (first && *it == '-')) {
        ++it;
      } else {
        return;
      }
      first = false;
    }
  };

  // Weekday
  if (!scanStr(3))
    return nan;
  bool foundWeekday = false;
  for (const char *name : weekdayNames) {
    if (tok.equals(llvh::arrayRefFromStringRef(name))) {
      foundWeekday = true;
      break;
    }
  }
  if (!foundWeekday)
    return nan;

  /// If we found a valid Month string from the current `tok`.
  auto tokIsMonth = [&]() -> bool {
    for (uint32_t i = 0; i < sizeof(monthNames) / sizeof(monthNames[0]); ++i) {
      if (tok.equals(llvh::arrayRefFromStringRef(monthNames[i]))) {
        // m is 1-indexed.
        m = i + 1;
        return true;
      }
    }
    return false;
  };

  // Day Month Year
  // or
  // Month Day Year
  while (it != str.end()) {
    if (isDigit(*it)) {
      // Day
      scanInt(it, end, d);
      // Month
      consumeSpacesOrDash();
      // e.g. `Janwhatever` will get read as `Jan`
      if (!scanStrAndSkipWord(3))
        return nan;
      // `tok` is now set to the Month candidate.
      if (!tokIsMonth())
        return nan;
      break;
    }
    if (isAlpha(*it)) {
      // try Month
      if (!scanStrAndSkipWord(3))
        return nan;
      // `tok` is now set to the Month candidate.
      if (tokIsMonth()) {
        // Day
        consumeSpacesOrDash();
        if (!scanInt(it, end, d))
          return nan;
        break;
      }
      // Continue scanning for Month.
      continue;
    }
    // Ignore any garbage.
    ++it;
  }

  // Year
  consumeSpacesOrDash();
  if (!scanInt(it, end, y))
    return nan;

  // Hour:minute:second.
  consumeSpacesOrDash();

  if (it != end) {
    if (!scanInt(it, end, h))
      return nan;
    if (!consume(':'))
      return nan;
    if (!scanInt(it, end, min))
      return nan;
    if (!consume(':'))
      return nan;
    if (!scanInt(it, end, s))
      return nan;
  }

  // Space and time zone.
  consumeSpaces();

  if (it == end) {
    // Default to local time zone if no time zone provided
    double t = makeDate(makeDay(y, m - 1, d), makeTime(h, min, s, ms));
    t = utcTime(t, localTimeOffsetCache);
    return t;
  }

  struct KnownTZ {
    const char *tz;
    int32_t tzh;
  };

  // Known time zones per RFC 2822.
  // All other obsolete time zones that aren't in this array treated as +00:00.
  static constexpr KnownTZ knownTZs[] = {
      {"GMT", 0},
      {"EDT", -4},
      {"EST", -5},
      {"CDT", -5},
      {"CST", -6},
      {"MDT", -6},
      {"MST", -7},
      {"PDT", -7},
      {"PST", -8},
  };

  // TZ name is optional, but if there is a letter, it is the only option.
  if ('A' <= *it && *it <= 'Z') {
    if (!scanStr(3))
      return nan;
    for (const KnownTZ &knownTZ : knownTZs) {
      if (tok.equals(llvh::arrayRefFromStringRef(knownTZ.tz))) {
        tzh = knownTZ.tzh;
        break;
      }
    }
  }

  if (it == end)
    goto complete;

  // Prevent "CDT+0700", for example.
  if (tzh != 0 && it != end)
    return nan;

  // Sign of the timezone adjustment.
  if (consume('+'))
    sign = 1;
  else if (consume('-'))
    sign = -1;
  else
    return nan;

  // Hour and minute of timezone adjustment.
  if (it > end - 4)
    return nan;
  if (!scanInt(it, it + 2, tzh))
    return nan;
  tzh *= sign;
  if (!scanInt(it, it + 2, tzm))
    return nan;
  tzm *= sign;

  if (it != end) {
    // Optional parenthesized description of timezone (must be at the end).
    if (!consume(' '))
      return nan;
    if (!consume('('))
      return nan;
    while (it != end && *it != ')')
      ++it;
    if (!consume(')'))
      return nan;
  }

  if (it != end)
    return nan;

complete:
  // Account for the fact that m was 1-indexed and the timezone offset.
  return makeDate(makeDay(y, m - 1, d), makeTime(h - tzh, min - tzm, s, ms));
}

double parseDate(StringView str, LocalTimeOffsetCache &localTimeOffsetCache) {
  double result = parseISODate(str, localTimeOffsetCache);
  if (!std::isnan(result)) {
    return result;
  }

  return parseESDate(str, localTimeOffsetCache);
}

} // namespace vm
} // namespace hermes
