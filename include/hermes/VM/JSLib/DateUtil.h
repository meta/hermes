/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSLIB_DATEUTIL_H
#define HERMES_VM_JSLIB_DATEUTIL_H

#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/StringView.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include "llvh/ADT/SmallString.h"

/// Helper functions used by the JavaScript Date object.
/// Specified in ES5.1 15.9.1.

namespace hermes {
namespace vm {

class LocalTimeOffsetCache;

//===----------------------------------------------------------------------===//
// Conversion constants

constexpr double HOURS_PER_DAY = 24;

constexpr double MINUTES_PER_HOUR = 60;
constexpr double MINUTES_PER_DAY = MINUTES_PER_HOUR * HOURS_PER_DAY;

constexpr double SECONDS_PER_MINUTE = 60;
constexpr double SECONDS_PER_HOUR = SECONDS_PER_MINUTE * MINUTES_PER_HOUR;
constexpr double SECONDS_PER_DAY = SECONDS_PER_HOUR * HOURS_PER_DAY;

constexpr double MS_PER_SECOND = 1000;
constexpr double MS_PER_MINUTE = MS_PER_SECOND * SECONDS_PER_MINUTE;
constexpr double MS_PER_HOUR = MS_PER_MINUTE * MINUTES_PER_HOUR;
constexpr double MS_PER_DAY = MS_PER_HOUR * HOURS_PER_DAY;

// Time value ranges from ES2024 21.4.1.1.
constexpr int64_t TIME_RANGE_SECS = SECONDS_PER_DAY * 100000000LL;
constexpr int64_t TIME_RANGE_MS = TIME_RANGE_SECS * MS_PER_SECOND;

//===----------------------------------------------------------------------===//
// Current time

/// \return the current time since Jan 1 1970 in milliseconds.
std::chrono::milliseconds::rep curTime();

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.2

/// The number of days that have passed at \p t milliseconds after Jan 1 1970.
double day(double t);

/// The amount of time that has passed within day(t) at time \p t.
double timeWithinDay(double t);

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.3

/// The number of days in year \p y.
/// \return either 365 or 366.
uint32_t daysInYear(double y);

/// Day number of the first day of year \p y.
double dayFromYear(double y);

/// Timestamp in milliseconds of the first day of the year \p y.
double timeFromYear(double y);

/// Year in which the timestamp at \p t milliseconds occurs.
double yearFromTime(double t);

/// \return true if millisecond timestamp \p t occurs within a leap year.
bool inLeapYear(double t);

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.4

/// Given a timestamp \p t in milliseconds, find the month in which it occurs.
/// \return an integer in the range [0,12) indicating the month.
uint32_t monthFromTime(double t);

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.5

/// Given a timestamp \p t in milliseconds, get the date of the month in which
/// it occurs.
double dateFromTime(double t);

//===----------------------------------------------------------------------===//
// ES9.0 20.3.1.6

/// Given a timestamp \p t in milliseconds, get the day of the week it is in.
/// \return 0 for Sunday, 1 for Monday, etc.
int32_t weekDay(double t);

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.8

namespace detail {
/// Bound on supported timezone offsets. Local-to-UTC conversion probes this
/// far on either side of a local time to discover nearby offset transitions.
constexpr int64_t kLocalTimeOffsetBoundMs = 2 * MS_PER_DAY;

// Exposed for test only
int32_t equivalentTime(int64_t epochSecs);
} // namespace detail
//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.9

/// Conversion from UTC to local time.
double localTime(double t, LocalTimeOffsetCache &);

/// Conversion from local time to UTC.
/// Spec refers to this as the UTC() function.
double utcTime(double t, LocalTimeOffsetCache &);

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.10

/// Hour component of the timestamp at t.
double hourFromTime(double t);

/// Minute component of the timestamp at t.
double minFromTime(double t);

/// Second component of the timestamp at t.
double secFromTime(double t);

/// Millisecond component of the timestamp at t.
double msFromTime(double t);

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.11

/// Get the number of milliseconds to the given hour, minute, sec, ms time.
double makeTime(double hour, double min, double sec, double ms);

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.12

/// Get the day offset from Jan 1 1970 (number of days since Unix time started).
/// \param year the year of the day.
/// \param month zero-indexed month for the day.
/// \param date one-indexed date within the month.
double makeDay(double year, double month, double date);

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.13

/// Get the number of milliseconds to time \p t on the given \p day since
/// Jan 1 1970.
double makeDate(double day, double t);

//===----------------------------------------------------------------------===//
// ES5.1 15.9.1.14

/// Clips the time to a valid number of milliseconds.
double timeClip(double t);

//===----------------------------------------------------------------------===//
// toString Functions

/// Creates an ISO 8601 format date string.
/// The string is of the format YYYY-MM-DD.
/// \param t timestamp in milliseconds since Jan 1 1970.
/// \param tza unused, placed for compatibility with the other toStrings.
/// \param buf the buffer to be populated with the resultant string.
void dateToISOString(double t, double tza, llvh::SmallVectorImpl<char> &buf);

/// Creates an ISO 8601 format time string.
/// The string is of the format HH:mm:ss.sssZ where Z is timezone.
/// \param t timestamp in milliseconds since Jan 1 1970.
/// \param tza time zone adjustment in milliseconds, used for the TZ.
/// \param buf the buffer to be populated with the resultant string.
void timeToISOString(double t, double tza, llvh::SmallVectorImpl<char> &buf);

/// Creates an ISO 8601 format string, in the provided timezone.
/// The string is of the format YYYY-MM-DDTHH:mm:ss.sssZ where Z is timezone.
/// The format is taken from ES5.1 15.9.1.15.
/// Note: The output of this function is implementation-defined by spec.
/// \param t timestamp in milliseconds since Jan 1 1970.
/// \param tza time zone adjustment in milliseconds, used for the TZ.
/// \param buf the buffer to be populated with the resultant string.
void datetimeToISOString(
    double t,
    double tza,
    llvh::SmallVectorImpl<char> &buf);

void datetimeToLocaleString(double t, llvh::SmallVectorImpl<char16_t> &buf);

void dateToLocaleString(double t, llvh::SmallVectorImpl<char16_t> &buf);

void timeToLocaleString(double t, llvh::SmallVectorImpl<char16_t> &buf);

/// ES9.0 20.3.4.41.2 DateString
/// Returns a spec-compliant string representing the date of \p t.
/// \param t timestamp in milliseconds since Jan 1 1970.
/// \param tza unused, placed for compatibility with the other toStrings.
/// \param buf the buffer to be populated with the resultant string.
void dateString(double t, double tza, llvh::SmallVectorImpl<char> &buf);

/// ES9.0 20.3.4.41.1 TimeString
/// Returns a spec-compliant string representing the time of \p t.
/// \param t timestamp in milliseconds since Jan 1 1970.
/// \param tza time zone adjustment in milliseconds, used for the TZ.
/// \param buf the buffer to be populated with the resultant string.
void timeString(double t, double tza, llvh::SmallVectorImpl<char> &buf);

/// ES9.0 20.3.4.41.3 TimeZoneString
/// Returns a spec-compliant string representing the timezone of \p t.
/// \param t timestamp in milliseconds since Jan 1 1970.
/// \param tza time zone adjustment in milliseconds, used for the TZ.
/// \param buf the buffer to be populated with the resultant string.
void timeZoneString(double t, double tza, llvh::SmallVectorImpl<char> &buf);

/// ES9.0 20.3.4.41.4 ToDateString
/// Returns a spec-compliant string representing the datetime of \p t.
/// \param t timestamp in milliseconds since Jan 1 1970.
/// \param tza unused, placed for compatibility with the other toStrings.
/// \param buf the buffer to be populated with the resultant string.
void dateTimeString(double t, double tza, llvh::SmallVectorImpl<char> &buf);

/// ES9.0 20.3.4.43 Main logic of Date.prototype.toUTCString.
/// Returns a spec-compliant string representing the datetime of \p t.
/// \param t timestamp in milliseconds since Jan 1 1970.
/// \param tza unused, placed for compatibility with the other toStrings.
/// \param buf the buffer to be populated with the resultant string.
void dateTimeUTCString(double t, double tza, llvh::SmallVectorImpl<char> &buf);

/// ES9.0 20.3.4.42 ToTimeString
/// Returns a spec-compliant string representing only the time of \p t.
/// \param t timestamp in milliseconds since Jan 1 1970.
/// \param tza unused, placed for compatibility with the other toStrings.
/// \param buf the buffer to be populated with the resultant string.
void timeTZString(double t, double tza, llvh::SmallVectorImpl<char> &buf);

//===----------------------------------------------------------------------===//
// Date parsing

/// Parses the given \p str and returns a double representing the time,
/// in milliseconds, as an offset from Jan 1 1970 UTC.
/// Currently supported formats:
///   - ISO 8601 from ES5.1 15.9.1.15
///     - Date: YYYY, YYYY-MM, or YYYY-MM-DD
///     - May be followed by an optional Time: THH:mm, THH:mm:ss, THH:mm:ss.sss
///     - May be followed by a time zone: Z or [+-]HH:mm
///     - The Date may also be extended to 6 digits with a sign: [+-]YYYYYY
///   - ISO 8601 as above, but with a space instead of 'T' as a separator.
///   - Full ES9.0 toString() format:
///     - EEE is day of the week
///     - EEE MMM DD YYYY HH:mm:ss GMT-HHmm
///     - EEE, DD MMM YYYY HH:mm:ss GMT
///     - Optionally followed by "(Timezone Description)"
/// Note: the only requirements of parsing are that the 15.9.1.15 format works,
/// and for a given Date x with a milliseconds value of 0, these are all equal:
///   - x.valueOf()
///   - Date.parse(x.toString())
///   - Date.parse(x.toUTCString())
///   - Date.parse(x.toISOString())
/// We can extend this to support other formats as well, when the given str
/// does not conform to the 15.9.1.15 format.
double parseDate(StringView str, LocalTimeOffsetCache &);

} // namespace vm
} // namespace hermes

#endif
