/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s

// Regression test: U+10000 in a named capture group escape (\u{10000} or
// \uD800\uDC00) must encode as a surrogate pair rather than being truncated to
// U+0000.

var m1 = /(?<\u{10000}>a)/.exec('a');
print(Object.keys(m1.groups)[0].length, m1.groups['\u{10000}'], m1.groups['\0']);
// CHECK: 2 a undefined

var m2 = /(?<\uD800\uDC00>b)/.exec('b');
print(Object.keys(m2.groups)[0].length, m2.groups['\u{10000}'], m2.groups['\0']);
// CHECK-NEXT: 2 b undefined

var re = new RegExp('(?<\u{10000}>a)\\k<\\u{10000}>');
print(re.test('aa'), re.test('ab'));
// CHECK-NEXT: true false

var reEscaped = new RegExp('(?<\\u{10000}>a)\\k<\\u{10000}>');
print(reEscaped.test('aa'), reEscaped.test('ab'));
// CHECK-NEXT: true false

print('a'.replace(/(?<\u{10000}>a)/, '$<\u{10000}>'));
// CHECK-NEXT: a

try {
  new RegExp('(?<\u{10000}>a)(?<\\u{10000}>b)');
  print('no error');
} catch (e) {
  print(e.name);
}
// CHECK-NEXT: SyntaxError
