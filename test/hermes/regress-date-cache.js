/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: TZ=PST %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: TZ=PST %shermes -O0 -exec %s | %FileCheck --match-full-lines %s

print("START");
//CHECK: START

// This test produces very large time epoch values (because it doubles the day
// values and use it as month). So it could exercise the corner case paths in
// DateCache:
// 1. When the input time epoch is larger than kMaxNativeTimeMs, we need to
// compute an equivalent time from it first.
// 2. When the input time is close to kMaxNativeTimeMs, and we extend the
// cache entry that covers it, we need to make sure that the new end of the
// entry does not overflow kMaxNativeTimeMs.
// kMaxNativeTimeMs is the 32-bit time_t limit on Windows and with a 32-bit
// time_t. A 64-bit time_t passes all of these dates to the OS unchanged.
var working = new Date(2014, 2, 1)
for (let i = 0; i < 10000; ++i) {
    var day = working.getDate();
    var year = working.getFullYear();
    var month = working.getMonth();
    day += day-1;
    working = new Date(year, day, day);
    if (working.getHours() == 0) {
        day--;
        working = new Date(year, month, day);
    }
}

print("FINISH");
//CHECK: FINISH
