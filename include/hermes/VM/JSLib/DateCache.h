/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSLIB_DATECACHE_H
#define HERMES_VM_JSLIB_DATECACHE_H

#include "hermes/VM/JSLib/DateUtil.h"

#include <ctime>

namespace hermes {
namespace vm {

/// Type of time represented by an epoch.
enum class TimeType : int8_t {
  Local,
  Utc,
};

/// Cache local time offset (including daylight saving offset).
///
/// Total offsets are cached in an array of time intervals. Time in the same
/// interval has a fixed offset, including any daylight saving adjustment.
/// For every new time point that is not included in any existing time interval,
/// compute its offset, try extending an existing interval, or creating a
/// new one and storing to the cache array (replace the least recently used
/// cache if it's full).
///
/// The algorithm of the offset caching is described below:
/// 1. Initialize every interval in the offset cache array to be
///    [t_max, -t_max], which is considered an "empty" interval, here t_max is
///    later than every cached time. Initialize candidate_ to point to the
///    first interval in the array. And constant kOffsetDeltaMs is the maximum
///    time range that can have at most one offset transition. Each entry also
///    has an epoch value that is used to find out the least recently used
///    entry, but we omit the operations on them in this description.
/// 2. Given a UTC time point t, check if it's included in candidate_, if yes,
/// return the offset of candidate_. Otherwise, go to step 3.
/// 3. Search the cache array and try to find two intervals:
///    before: [s_before, e_before] where s_before <= t and as close as
///    possible.
///    after: [s_after, e_after] where t < s_after and e_after ends as early as
///    possible.
///    If one interval is not found, let it point to an empty interval in the
///    cache array (if no empty interval in it, reset the least recently used
///    one to empty and use it). Assign before to candidate_.
/// 4. If the before interval is empty, compute the offset of t,
///    and set its interval to [t, t]. Otherwise, go to step 5.
/// 5. Check if t is included in the new non-empty before interval, if yes,
///    return its offset. Otherwise, go to step 6.
/// 6. If t > R_new, where R_new = before.end + kOffsetDeltaMs, compute the
///    offset for t and call extendOrRecomputeCacheEntry(after, t, offset) (this
///    function is described later) to update after, assign after to candidate_,
///    return offset. Otherwise, go to step 7.
/// 7. If R_new <= after.start, compute the offset for R_new and call
///    extendOrRecomputeCacheEntry(after, R_new, offset) to update after.
/// 8. If before.offset == after.offset, merge after to before and
///    reset after, then return the offset. Otherwise, go to step 9.
/// 9. At this step, there must be one offset transition in interval
///    (before.end, after.start]. Bisect it, extending before or after to each
///    midpoint with the same offset, until t is included in one of them, and
///    return its offset (if after is hit, assign it to candidate_). If five
///    iterations do not cover t, compute the offset of t directly. If an offset
///    computed in step 7 or 9 contradicts both cached offsets, the cache is
///    stale, for example after an undetected timezone change: reset it and
///    compute the offset of t directly.
/// 10. Given a new UTC time point, repeat step 2 - 9.
///
/// Algorithm for extendOrRecomputeAfterCache(entry, t, offset):
/// 1. If offset == entry.offset and
///    entry.start - kOffsetDeltaMs <= t <= entry.end, let entry.start = t
///    and return.
/// 2. If entry is not empty, scan every entry in the cache array (except
///    candidate_), find out the least recently used one, reset it to empty.
/// 3. Assign offset to entry and update its interval to be [t, t].
///
/// On POSIX, a lookup whose time is in no cached interval first checks the
/// offset and the DST flag at a fixed reference time, the time of the last
/// reset, for timezone changes exposed by localtime_r; the lookups of one
/// local-to-UTC conversion share a single check. Using a fixed time
/// distinguishes a timezone change from an offset transition in the queried
/// dates. Linux and Windows C libraries may cache the timezone until tzset()
/// is called, and Windows does not check at all.
class LocalTimeOffsetCache {
 public:
  /// All runtime functionality should use the instance provided in
  /// JSLibStorage, this is public only because we need to construct a instance
  /// in the unit tests (alternatively, we have to create a singleton function
  /// to be used in the tests).
  LocalTimeOffsetCache() {
    reset();
  }

  /// Refresh the system timezone and clear cached offsets.
  void reset();

  /// Compute local timezone offset (DST included).
  /// \param timeMs time in milliseconds.
  /// \param timeType whether \p timeMs is UTC or local time.
  double getLocalTimeOffset(double timeMs, TimeType timeType);

  /// \param utcTimeMs UTC epoch in milliseconds.
  /// \return Total local timezone offset at time \p utcTimeMs.
  int totalOffsetInMs(int64_t utcTimeMs);

 private:
  LocalTimeOffsetCache(const LocalTimeOffsetCache &) = delete;
  LocalTimeOffsetCache operator=(const LocalTimeOffsetCache &) = delete;

  /// Number of cached time intervals for timezone offsets.
  static constexpr unsigned kCacheSize = 32;
  /// Maximum interval over which we assume at most one offset transition.
  /// Keep this at most 6 days and 23 hours: America/Boa_Vista, Noronha and
  /// Recife changed their offsets twice within that time in October 2000.
  static constexpr int64_t kOffsetDeltaMs = 4 * MS_PER_DAY;
  /// The largest time that a 32-bit time_t can represent.
  static constexpr int64_t kMaxEpochTimeInMs =
      std::numeric_limits<int32_t>::max() * MS_PER_SECOND;
  // [kMinNativeTimeMs, kMaxNativeTimeMs] is the range of UTC times passed to
  // the OS unchanged. totalOffsetInMs() maps other times to an equivalent year.
  // needsEquivalentYear() in lib/Platform/Unicode/PlatformDateFormat.cpp makes
  // the same choice for toLocale*String(); change both together.
#ifdef _WINDOWS
  /// The earliest time passed to localtime_s, which rejects negative times.
  static constexpr int64_t kMinNativeTimeMs = 0;
  /// The latest time passed to localtime_s unchanged.
  static constexpr int64_t kMaxNativeTimeMs = kMaxEpochTimeInMs;
#else
  /// The latest time passed to localtime_r unchanged. A 64-bit time_t covers
  /// every query whose offset is within kLocalTimeOffsetBoundMs: the TimeClip
  /// range, widened for local-to-UTC conversion and its probes.
  static constexpr int64_t kMaxNativeTimeMs =
      sizeof(std::time_t) >= sizeof(int64_t)
      ? TIME_RANGE_MS + 2 * detail::kLocalTimeOffsetBoundMs
      : kMaxEpochTimeInMs;
  /// The earliest time passed to localtime_r unchanged.
  static constexpr int64_t kMinNativeTimeMs = -kMaxNativeTimeMs;
#endif
  /// Start of an empty interval, later than every cached time.
  static constexpr int64_t kEmptyStartMs = kMaxNativeTimeMs + 1;

  struct OffsetCacheEntry {
    /// Start and end time of this offset cache interval, in UTC time.
    int64_t startMs{kEmptyStartMs};
    int64_t endMs{-kEmptyStartMs};
    /// The offset in [startMs, endMs].
    int offsetMs{0};
    /// Used for LRU.
    int epoch{0};

    /// \return whether this is a valid interval.
    bool isEmpty() const {
      assert(
          (startMs <= endMs ||
           (startMs == kEmptyStartMs && endMs == -kEmptyStartMs)) &&
          "Ill-formed OffsetCacheEntry");
      return startMs > endMs;
    }

    /// \return True if \p utcTimeMs is included in this interval.
    bool include(int64_t utcTimeMs) const {
      return startMs <= utcTimeMs && utcTimeMs <= endMs;
    }
  };

  /// Like the public overload, but check for a system timezone change on a
  /// cache miss only if \p checkTimezone is set, and clear it after checking.
  /// This lets several lookups for one conversion share a single check.
  int totalOffsetInMs(int64_t utcTimeMs, bool &checkTimezone);

  /// Query the system for the total offset at UTC time \p utcTimeMs. If
  /// \p isDST is not null, also set it to whether DST is in effect then.
  static int computeTotalOffset(int64_t utcTimeMs, bool *isDST = nullptr);

  /// Increase the epoch counter and return it.
  int bumpEpoch() {
    ++epoch_;
    return epoch_;
  }

  /// Find the least recently used offset cache and reuse it.
  /// \param skip do not scan this cache. Technically, it can be nullptr, which
  /// means we don't skip any entry in the cache array. But currently we always
  /// passed in an meaningful entry pointer (e.g., candidate_).
  /// \return an empty offset cache. This should never return nullptr.
  OffsetCacheEntry *leastRecentlyUsedExcept(const OffsetCacheEntry *const skip);

  /// Scan the offset caches to find a cached interval starts at or before \p
  /// timeMs (as late as possible) and an interval ends after \p timeMs (as
  /// early as possible). If none is found, reuse an empty cache. Return the
  /// found two cache entries.
  std::tuple<OffsetCacheEntry *, OffsetCacheEntry *> findBeforeAndAfterEntries(
      int64_t timeMs);

  /// If entry->offsetMs == \p offsetMs and \p timeMs is included in
  /// [entry->startMs - kOffsetDeltaMs, entry->endMs], extend entry to
  /// [timeMs, entry->endMs].
  /// Otherwise, let \p entry point to the least recently used cache entry
  /// (except the candidate_ cache) and update its interval to be [timeMs,
  /// timeMs], and its offset to be \p offsetMs.
  void extendOrRecomputeCacheEntry(
      OffsetCacheEntry *&entry,
      int64_t timeMs,
      int offsetMs);

  /// Integer counter used to find least recently used cache.
  int epoch_;
  /// Point to one entry in the caches_ array, and this invariant always
  /// holds: candidate_->startMs <= t <= candidate_->endMs, where t is the last
  /// seen time point. It likely will cover subsequent time points.
  OffsetCacheEntry *candidate_;
  /// A list of cached intervals computed for previously seen time points.
  /// In the beginning, each interval is initialized to empty. When every
  /// interval is non-empty and we need a new empty one, reset the least
  /// recently used one (by comparing the epoch value) to empty.
  std::array<OffsetCacheEntry, kCacheSize> caches_;
#ifndef _WINDOWS
  /// A fixed instant used to detect system timezone changes.
  ///
  /// A single reference time cannot detect a change between timezones that
  /// agree there in both offset and DST flag but differ at other times. For
  /// example, America/Denver and America/Phoenix both use MST (-07:00) in
  /// winter, but Denver uses MDT (-06:00) in summer. After a change between
  /// them during winter, offsets cached for summer dates stay stale until they
  /// are evicted or the cache is reset, while newly computed offsets follow
  /// the new timezone. Checking a second time in the other season would detect
  /// such changes, at the cost of another OS query on every cache miss.
  int64_t referenceTimeMs_;
  /// The total offset at referenceTimeMs_.
  int referenceOffsetMs_;
  /// Whether DST is in effect at referenceTimeMs_.
  bool referenceIsDST_;
#endif
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_JSLIB_DATECACHE_H
