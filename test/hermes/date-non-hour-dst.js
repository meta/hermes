/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: TZ="<+1030>-10:30<+11>-11,M10.1.0,M4.1.0" %hermes -O %s | %FileCheck --match-full-lines --check-prefix=HALF %s
// RUN: TZ="<+1030>-10:30<+11>-11,M10.1.0,M4.1.0" %shermes -exec %s | %FileCheck --match-full-lines --check-prefix=HALF %s
// RUN: TZ="<+00>0<+02>-2,M3.5.0/1,M10.5.0/3" %hermes -O %s | %FileCheck --match-full-lines --check-prefix=TWO %s
// RUN: TZ="<+00>0<+02>-2,M3.5.0/1,M10.5.0/3" %shermes -exec %s | %FileCheck --match-full-lines --check-prefix=TWO %s
// XFAIL: windows
"use strict";

// Daylight saving time that is not one hour long, from
// https://github.com/facebook/hermes/issues/2246. The POSIX TZ strings follow
// Australia/Lord_Howe, whose DST adds 30 minutes, and Antarctica/Troll, whose
// DST adds two hours, so the test does not need the IANA timezone database.

function show(date) {
  print(date + ' | ' + date.toISOString() + ' | ' + date.getTimezoneOffset());
}

if (new Date(2019, 0, 15).getTimezoneOffset() === -660) {
  print('Half-hour DST');
// HALF-LABEL: Half-hour DST

  // Standard time and DST.
  show(new Date(2019, 6, 15, 12));
// HALF-NEXT: Mon Jul 15 2019 12:00:00 GMT+1030 | 2019-07-15T01:30:00.000Z | -630
  show(new Date(2019, 0, 15, 12));
// HALF-NEXT: Tue Jan 15 2019 12:00:00 GMT+1100 | 2019-01-15T01:00:00.000Z | -660

  // On 2019-10-06, local time skips from 02:00 to 02:30. A skipped time uses
  // the offset from before the transition.
  show(new Date(2019, 9, 6, 1, 59));
// HALF-NEXT: Sun Oct 06 2019 01:59:00 GMT+1030 | 2019-10-05T15:29:00.000Z | -630
  show(new Date(2019, 9, 6, 2, 15));
// HALF-NEXT: Sun Oct 06 2019 02:45:00 GMT+1100 | 2019-10-05T15:45:00.000Z | -660
  show(new Date(2019, 9, 6, 2, 30));
// HALF-NEXT: Sun Oct 06 2019 02:30:00 GMT+1100 | 2019-10-05T15:30:00.000Z | -660

  // On 2019-04-07, local time repeats 01:30 to 02:00. A repeated time uses
  // the earlier instant.
  show(new Date(2019, 3, 7, 1, 45));
// HALF-NEXT: Sun Apr 07 2019 01:45:00 GMT+1100 | 2019-04-06T14:45:00.000Z | -660
  show(new Date(Date.UTC(2019, 3, 6, 15, 15)));
// HALF-NEXT: Sun Apr 07 2019 01:45:00 GMT+1030 | 2019-04-06T15:15:00.000Z | -630
  show(new Date(2019, 3, 7, 2));
// HALF-NEXT: Sun Apr 07 2019 02:00:00 GMT+1030 | 2019-04-06T15:30:00.000Z | -630

  // Setters convert local time in the same way.
  var half = new Date(2019, 9, 6, 1);
  half.setMinutes(75);
  show(half);
// HALF-NEXT: Sun Oct 06 2019 02:45:00 GMT+1100 | 2019-10-05T15:45:00.000Z | -660
} else {
  print('Two-hour DST');
// TWO-LABEL: Two-hour DST

  // Standard time and DST.
  show(new Date(2019, 6, 15, 12));
// TWO-NEXT: Mon Jul 15 2019 12:00:00 GMT+0200 | 2019-07-15T10:00:00.000Z | -120
  show(new Date(2019, 0, 15, 12));
// TWO-NEXT: Tue Jan 15 2019 12:00:00 GMT+0000 | 2019-01-15T12:00:00.000Z | 0

  // On 2019-03-31, local time skips from 01:00 to 03:00. A skipped time uses
  // the offset from before the transition.
  show(new Date(2019, 2, 31, 0, 59));
// TWO-NEXT: Sun Mar 31 2019 00:59:00 GMT+0000 | 2019-03-31T00:59:00.000Z | 0
  show(new Date(2019, 2, 31, 2, 30));
// TWO-NEXT: Sun Mar 31 2019 04:30:00 GMT+0200 | 2019-03-31T02:30:00.000Z | -120
  show(new Date(2019, 2, 31, 3));
// TWO-NEXT: Sun Mar 31 2019 03:00:00 GMT+0200 | 2019-03-31T01:00:00.000Z | -120

  // On 2019-10-27, local time repeats 01:00 to 03:00. A repeated time uses
  // the earlier instant.
  show(new Date(2019, 9, 27, 2, 30));
// TWO-NEXT: Sun Oct 27 2019 02:30:00 GMT+0200 | 2019-10-27T00:30:00.000Z | -120
  show(new Date(Date.UTC(2019, 9, 27, 2, 30)));
// TWO-NEXT: Sun Oct 27 2019 02:30:00 GMT+0000 | 2019-10-27T02:30:00.000Z | 0
  show(new Date(2019, 9, 27, 3));
// TWO-NEXT: Sun Oct 27 2019 03:00:00 GMT+0000 | 2019-10-27T03:00:00.000Z | 0

  // Setters convert local time in the same way.
  var two = new Date(2019, 2, 31, 0);
  two.setHours(2);
  show(two);
// TWO-NEXT: Sun Mar 31 2019 04:00:00 GMT+0200 | 2019-03-31T02:00:00.000Z | -120
}
