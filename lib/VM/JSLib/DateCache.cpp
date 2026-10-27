/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSLib/DateCache.h"

namespace hermes {
namespace vm {

void LocalTimeOffsetCache::reset() {
  ::tzset();
  caches_.fill(OffsetCacheEntry{});
  candidate_ = caches_.data();
  prevCandidate_ = caches_.data();
  epoch_ = 0;
#ifndef _WINDOWS
  // Keep the reference time in the range that the OS accepts.
  referenceTimeMs_ =
      std::min<int64_t>(std::time(nullptr) * MS_PER_SECOND, kMaxNativeTimeMs);
  referenceOffsetMs_ = computeTotalOffset(referenceTimeMs_, &referenceIsDST_);
#endif
}

double LocalTimeOffsetCache::getLocalTimeOffset(
    double timeMs,
    TimeType timeType) {
  if (!std::isfinite(timeMs))
    return std::numeric_limits<double>::quiet_NaN();

  if (timeType == TimeType::Utc) {
    // Out of allowed range by the spec, return NaN.
    if (timeMs < -TIME_RANGE_MS || timeMs > TIME_RANGE_MS)
      return std::numeric_limits<double>::quiet_NaN();

    return totalOffsetInMs(std::floor(timeMs));
  }

  // A local time slightly outside the TimeClip range may convert to a valid
  // UTC time. Bound it before converting to int64_t, allowing for the offset.
  constexpr int64_t bound = detail::kLocalTimeOffsetBoundMs;
  if (timeMs < -TIME_RANGE_MS - bound || timeMs > TIME_RANGE_MS + bound)
    return std::numeric_limits<double>::quiet_NaN();

  // Discover the offsets on either side of a nearby transition. This assumes
  // offsets are smaller than 48 hours in magnitude and there is at most one
  // transition in this four-day window, as with the supported timezone data.
  // The three lookups share one check for a system timezone change.
  bool checkTimezone = true;
  int before = totalOffsetInMs(std::floor(timeMs - bound), checkTimezone);
  int after = totalOffsetInMs(std::floor(timeMs + bound), checkTimezone);
  // For a transition at T with offsets a -> b, subtracting max(a, b) gives
  // guessUTC < T exactly when timeMs < T + max(a, b). Thus skipped and repeated
  // local times use the pre-transition offset, as required by UTC(t), while
  // the first unambiguous time after the transition uses the new offset.
  double guessUTC = timeMs - std::max(before, after);
  // An offset of 48 hours or more breaks the assumptions above, and near the
  // ends of the range moves the guess beyond the times that can be queried.
  if (guessUTC < -TIME_RANGE_MS - 2 * bound ||
      guessUTC > TIME_RANGE_MS + 2 * bound)
    return std::numeric_limits<double>::quiet_NaN();
  return totalOffsetInMs(std::floor(guessUTC), checkTimezone);
}

int LocalTimeOffsetCache::computeTotalOffset(int64_t utcTimeMs, bool *isDST) {
  if (isDST)
    *isDST = false;
  std::time_t t = std::floor(utcTimeMs / MS_PER_SECOND);
  std::tm tm;
#ifdef _WINDOWS
  if (::localtime_s(&tm, &t))
    return 0;
  if (isDST)
    *isDST = tm.tm_isdst > 0;

  // Windows tm has no tm_gmtoff. Interpret its local calendar fields as UTC
  // using timezone-independent arithmetic, then subtract the queried instant.
  // Both sides must use whole seconds, including at a day or year boundary.
  double localMs = makeDate(
      makeDay(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday),
      makeTime(tm.tm_hour, tm.tm_min, tm.tm_sec, 0));
  return localMs - t * MS_PER_SECOND;
#else
  if (!::localtime_r(&t, &tm))
    return 0;
  if (isDST)
    *isDST = tm.tm_isdst > 0;
  return tm.tm_gmtoff * MS_PER_SECOND;
#endif
}

int LocalTimeOffsetCache::totalOffsetInMs(int64_t utcTimeMs) {
  bool checkTimezone = true;
  return totalOffsetInMs(utcTimeMs, checkTimezone);
}

int LocalTimeOffsetCache::totalOffsetInMs(
    int64_t utcTimeMs,
    [[maybe_unused]] bool &checkTimezone) {
  // Pass the time to the OS unchanged when it is in the native range, so that
  // the offset follows the timezone's history and current rules, as in V8.
  // Otherwise, map the time to a year with the same leap-year-ness and
  // starting day of the week (ES5.1 15.9.1.8).
  if (utcTimeMs < kMinNativeTimeMs || utcTimeMs > kMaxNativeTimeMs) {
    utcTimeMs = detail::equivalentTime(std::floor(utcTimeMs / MS_PER_SECOND)) *
        MS_PER_SECOND;
  }

  // Reset the counter to avoid overflow. Each call of this function may
  // increase epoch_ by more than 1 (conservatively smaller than 10), so we need
  // to subtract it from max value. In practice, this won't happen frequently
  // since most time we should see cache hit.
  if (LLVM_UNLIKELY(
          epoch_ >= std::numeric_limits<decltype(epoch_)>::max() - 10)) {
    reset();
  }

  // Cache hit.
  if (candidate_->include(utcTimeMs)) {
    candidate_->epoch = bumpEpoch();
    return candidate_->offsetMs;
  }
  // Lookups often alternate between two intervals, such as the probes on
  // either side of a transition, so also try the previous candidate.
  if (prevCandidate_->include(utcTimeMs)) {
    std::swap(candidate_, prevCandidate_);
    candidate_->epoch = bumpEpoch();
    return candidate_->offsetMs;
  }
  prevCandidate_ = candidate_;

  // Try to find cached intervals that happen before/after utcTimeMs.
  OffsetCacheEntry *before;
  OffsetCacheEntry *after;
  std::tie(before, after) = findBeforeAndAfterEntries(utcTimeMs);

  // Hits in the cached interval.
  if (before && before->include(utcTimeMs)) {
    candidate_ = before;
    before->epoch = bumpEpoch();
    return before->offsetMs;
  }

#ifndef _WINDOWS
  // The time is in no cached interval, so the OS may need to be queried. Check
  // it for a timezone change first.
  // Check a fixed instant, rather than the requested date: historical and
  // seasonal offset changes must not be mistaken for system timezone changes.
  // The DST flag also distinguishes zones whose offsets match there because
  // only one of them observes DST. This remains a best-effort check; see
  // referenceTimeMs_ for the changes it cannot detect.
  // A reset empties every entry, so search again. Check at most once, so a
  // concurrent timezone change cannot cause an unbounded loop.
  if (checkTimezone) {
    checkTimezone = false;
    bool isDST;
    if (computeTotalOffset(referenceTimeMs_, &isDST) != referenceOffsetMs_ ||
        isDST != referenceIsDST_) {
      reset();
      std::tie(before, after) = findBeforeAndAfterEntries(utcTimeMs);
    }
  }
#endif

  // This is a miss, so reuse empty caches for intervals that were not found.
  reuseEmptyEntries(before, after, utcTimeMs);

  // Set candidate_ to before by default, and reassign it in case that the
  // after cache is hit or used.
  candidate_ = before;

  // No cached interval yet, compute a new one with utcTimeMs.
  if (before->isEmpty()) {
    int offset = computeTotalOffset(utcTimeMs);
    before->offsetMs = offset;
    before->startMs = utcTimeMs;
    before->endMs = utcTimeMs;
    before->epoch = bumpEpoch();
    return offset;
  }

  // If utcTimeMs is larger than before->endMs + kOffsetDeltaMs, we can't safely
  // extend before, because it could have more than one offset transition in the
  // interval. Instead, try if we can extend after (or recompute it).
  if ((utcTimeMs - kOffsetDeltaMs) > before->endMs) {
    int offset = computeTotalOffset(utcTimeMs);
    extendOrRecomputeCacheEntry(after, utcTimeMs, offset);
    // May help cache hit in subsequent calls (in case that the passed in time
    // values are adjacent).
    candidate_ = after;
    return offset;
  }

  // Now, utcTimeMs is in the range of (before->endMs, before->endMs +
  // kOffsetDeltaMs].

  before->epoch = bumpEpoch();
  // If before->endMs gets too large, we need to make sure it won't overflow
  // kMaxNativeTimeMs after extending it.
  int64_t newAfterStart = before->endMs < kMaxNativeTimeMs - kOffsetDeltaMs
      ? before->endMs + kOffsetDeltaMs
      : kMaxNativeTimeMs;
  // If after starts too late, recompute it or extend it to newAfterStart. An
  // empty after starts later than every cached time, so it is recomputed.
  if (newAfterStart <= after->startMs) {
    int offset = computeTotalOffset(newAfterStart);
    // The OS contradicts the cache at the same time, so the cache is stale,
    // for example after an undetected timezone change. Start over.
    if (newAfterStart == after->startMs && offset != after->offsetMs) {
      reset();
      return computeTotalOffset(utcTimeMs);
    }
    extendOrRecomputeCacheEntry(after, newAfterStart, offset);
  } else {
    after->epoch = bumpEpoch();
  }

  // Now after->startMs is in (before->endMs, before->endMs + kOffsetDeltaMs].

  // If before and after have the same offset, merge them.
  if (before->offsetMs == after->offsetMs) {
    before->endMs = after->endMs;
    *after = OffsetCacheEntry{};
    return before->offsetMs;
  }

  // Binary search in (before->endMs, after->startMs] for offset transition
  // point. Note that after->startMs could be smaller than before->endMs
  // + kOffsetDeltaMs, but that small interval has the same offset, so we
  // can ignore them in the below search.
  // Though 5 iterations should be enough to cover kOffsetDeltaMs, if the
  // assumption of only one transition in kOffsetDeltaMs no longer holds, we may
  // not be able to search the result. We'll stop the loop after 5 iterations
  // anyway.
  for (int i = 4; i >= 0; --i) {
    int delta = after->startMs - before->endMs;
    int64_t middle = before->endMs + delta / 2;
    int middleOffset = computeTotalOffset(middle);
    if (before->offsetMs == middleOffset) {
      before->endMs = middle;
      if (utcTimeMs <= before->endMs) {
        return middleOffset;
      }
    } else if (after->offsetMs == middleOffset) {
      after->startMs = middle;
      if (utcTimeMs >= after->startMs) {
        // May help cache hit in subsequent calls (in case that the passed in
        // time values are adjacent).
        candidate_ = after;
        return middleOffset;
      }
    } else {
      // A third offset in a window with at most one transition means that the
      // cache is stale, for example after an undetected timezone change.
      reset();
      return computeTotalOffset(utcTimeMs);
    }
  }

  // Fallthrough path of the binary search, just compute the offset for
  // utcTimeMs.
  return computeTotalOffset(utcTimeMs);
}

LocalTimeOffsetCache::OffsetCacheEntry *
LocalTimeOffsetCache::leastRecentlyUsedExcept(
    const OffsetCacheEntry *const skip) {
  OffsetCacheEntry *result = nullptr;
  for (auto &cache : caches_) {
    if (&cache == skip)
      continue;
    if (!result || result->epoch > cache.epoch)
      result = &cache;
  }
  *result = OffsetCacheEntry{};
  return result;
}

std::tuple<
    LocalTimeOffsetCache::OffsetCacheEntry *,
    LocalTimeOffsetCache::OffsetCacheEntry *>
LocalTimeOffsetCache::findBeforeAndAfterEntries(int64_t timeMs) {
  LocalTimeOffsetCache::OffsetCacheEntry *before = nullptr;
  LocalTimeOffsetCache::OffsetCacheEntry *after = nullptr;

  // `before` should start as late as possible, while `after` should end as
  // early as possible so that they're closest to timeMs.
  for (auto &cache : caches_) {
    if (cache.startMs <= timeMs) {
      if (!before || before->startMs < cache.startMs)
        before = &cache;
    } else if (timeMs < cache.endMs) {
      if (!after || after->endMs > cache.endMs)
        after = &cache;
    }
  }

  return {before, after};
}

void LocalTimeOffsetCache::reuseEmptyEntries(
    OffsetCacheEntry *&before,
    OffsetCacheEntry *&after,
    [[maybe_unused]] int64_t timeMs) {
  // None is found, reuse an empty cache for later computation.
  if (!before) {
    before = leastRecentlyUsedExcept(after);
  }
  if (!after) {
    after = leastRecentlyUsedExcept(before);
  }

  assert(
      before && after && before != after &&
      "`before` and `after` interval should be valid");
  assert(
      before->isEmpty() ||
      before->startMs <= timeMs &&
          "the start time of `before` must start on or before timeMs");
  assert(
      after->isEmpty() ||
      timeMs < after->startMs &&
          "The start time of `after` must start after timeMs");
  assert(
      before->isEmpty() || after->isEmpty() ||
      before->endMs < after->startMs &&
          "`before` interval must strictly start before `after` interval");
}

void LocalTimeOffsetCache::extendOrRecomputeCacheEntry(
    OffsetCacheEntry *&entry,
    int64_t timeMs,
    int offsetMs) {
  // It's safe to extend the interval if timeMs is in the checked range.
  if (entry->offsetMs == offsetMs &&
      entry->startMs - kOffsetDeltaMs <= timeMs && timeMs <= entry->endMs) {
    entry->startMs = timeMs;
  } else {
    // Recompute the after cache using timeMs.
    if (!entry->isEmpty()) {
      entry = leastRecentlyUsedExcept(candidate_);
    }
    entry->startMs = timeMs;
    entry->endMs = timeMs;
    entry->offsetMs = offsetMs;
    entry->epoch = bumpEpoch();
  }
}

} // namespace vm
} // namespace hermes
