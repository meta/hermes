/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: TZ="MST+7" %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: TZ="MST+7" %shermes -exec %s | %FileCheck --match-full-lines %s
"use strict";

// An ISO date-time string with an out-of-range field is invalid, so Date.parse
// returns NaN instead of rolling over into the next month, day or hour.
print('Date.parse');
// CHECK-LABEL: Date.parse
print(Date.parse('2020-13-01'));
// CHECK-NEXT: NaN
print(Date.parse('2020-00-10'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-32'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-00'));
// CHECK-NEXT: NaN
print(Date.parse('2020-13'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01T25:00Z'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01T23:60Z'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01T23:59:60Z'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01T24:30Z'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01T24:00:00.001Z'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01T00:00+24:00'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01T00:00+01:60'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01T00:00-25:00'));
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01T25:00'));
// CHECK-NEXT: NaN
print(new Date('2024-13-08T00:00:00Z').getTime());
// CHECK-NEXT: NaN
print(new Date('1999-00-00').getTime());
// CHECK-NEXT: NaN
print(Date.parse('2020-01-01 25:00'));
// CHECK-NEXT: NaN

// The largest value of each field is still accepted.
print(Date.parse('2020-12-31'));
// CHECK-NEXT: 1609372800000
print(Date.parse('2020-01-31T23:59:59.999Z'));
// CHECK-NEXT: 1580515199999
print(Date.parse('2020-01-01T24:00:00.000Z'));
// CHECK-NEXT: 1577923200000
print(Date.parse('2020-01-01T00:00+23:59'));
// CHECK-NEXT: 1577750460000
print(Date.parse('2020-01-01T23:59'));
// CHECK-NEXT: 1577948340000

// The day is checked against 31 only, so a day past the end of a shorter
// month still rolls over.
print(Date.parse('2020-02-30'));
// CHECK-NEXT: 1583020800000
