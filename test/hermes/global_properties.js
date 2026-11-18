/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -non-strict -O -target=HBC %s | %FileCheck --match-full-lines %s
// RUN: %hermes -non-strict -O -target=HBC -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

print('isNaN');
// CHECK-LABEL: isNaN

// isNaN()
print(isNaN(5));
// CHECK-NEXT: false

print(isNaN(false));
// CHECK-NEXT: false

print(isNaN(undefined));
// CHECK-NEXT: true

print(isNaN({}));
// CHECK-NEXT: true

print(isNaN(NaN));
// CHECK-NEXT: true

// isFinite()
print(isFinite(NaN));
// CHECK-NEXT: false

print(isFinite(Infinity));
// CHECK-NEXT: false

print(isFinite(-Infinity));
// CHECK-NEXT: false

print(isFinite("1234567"));
// CHECK-NEXT: true

print(isFinite(0x555));
// CHECK-NEXT: true

print(isFinite(false));
// CHECK-NEXT: true

print('parseInt');
// CHECK-LABEL: parseInt

// parseInt()
var R_digit = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"];
for (var i = 2; i <= 36; i++) {
  print(parseInt(R_digit[i - 2] + "$", i));
}
// CHECK-NEXT: 1
// CHECK-NEXT: 2
// CHECK-NEXT: 3
// CHECK-NEXT: 4
// CHECK-NEXT: 5
// CHECK-NEXT: 6
// CHECK-NEXT: 7
// CHECK-NEXT: 8
// CHECK-NEXT: 9
// CHECK-NEXT: 10
// CHECK-NEXT: 11
// CHECK-NEXT: 12
// CHECK-NEXT: 13
// CHECK-NEXT: 14
// CHECK-NEXT: 15
// CHECK-NEXT: 16
// CHECK-NEXT: 17
// CHECK-NEXT: 18
// CHECK-NEXT: 19
// CHECK-NEXT: 20
// CHECK-NEXT: 21
// CHECK-NEXT: 22
// CHECK-NEXT: 23
// CHECK-NEXT: 24
// CHECK-NEXT: 25
// CHECK-NEXT: 26
// CHECK-NEXT: 27
// CHECK-NEXT: 28
// CHECK-NEXT: 29
// CHECK-NEXT: 30
// CHECK-NEXT: 31
// CHECK-NEXT: 32
// CHECK-NEXT: 33
// CHECK-NEXT: 34
// CHECK-NEXT: 35

print(parseInt("0x1000000000000081"));
// CHECK-NEXT: 1152921504606847200

print(parseInt("1000000000000000000000000000000000000000000000000000010000001", 2));
// CHECK-NEXT: 1152921504606847200

print(parseInt("0x1000000000000084"));
// CHECK-NEXT: 1152921504606847200

print(parseInt("0x1000000000000079"));
// CHECK-NEXT: 1152921504606847000

print(parseInt("1000000000000000000000000000000000000000000000000000001111001", 2));
// CHECK-NEXT: 1152921504606847000

print(parseInt("1234567890123456789012345"));
// CHECK-NEXT: 1.2345678901234568e+24

print(parseInt("123456789012345678"), parseInt("99999999999999999999"));
// CHECK-NEXT: 123456789012345680 100000000000000000000

print(parseInt("-123456789012345680000"), parseInt("9".repeat(309)));
// CHECK-NEXT: -123456789012345680000 Infinity

// The lexer parses legacy decimal literals with parseIntWithRadix too.
print(099999999999999999999);
// CHECK-NEXT: 100000000000000000000

print(parseInt("0xdeAdBeeF"));
// CHECK-NEXT: 3735928559

print(parseInt("-123"));
// CHECK-NEXT: -123

print(parseInt("abc"))
// CHECK-NEXT: NaN

print('parseFloat');
// CHECK-LABEL: parseFloat

print(parseFloat("NaN$"));
// CHECK-NEXT: NaN

print(parseFloat("Infinity$"));
// CHECK-NEXT: Infinity

print(parseFloat("-Infinity$"));
// CHECK-NEXT: -Infinity

print(parseFloat(""));
// CHECK-NEXT: NaN

print(parseFloat("-5.5"));
// CHECK-NEXT: -5.5

print(parseFloat("+1e+50"));
// CHECK-NEXT: 1e+50

print(parseFloat("-1e-50"));
// CHECK-NEXT: -1e-50

print(parseFloat("1.2.3.4.5"));
// CHECK-NEXT: 1.2

print(parseFloat("12e2e3"));
// CHECK-NEXT: 1200

print(parseFloat("5.5e2.2"));
// CHECK-NEXT: 550

print(parseFloat("6853294837411.2184921374291384"));
// CHECK-NEXT: 6853294837411.219

print(parseFloat("+1e+"));
// CHECK-NEXT: 1

print(parseFloat("-55e.5"));
// CHECK-NEXT: -55

print(parseFloat('+'));
// CHECK-NEXT: NaN

try {
  new isNaN();
} catch (e) {
  print(e.name);
}
// CHECK-NEXT: TypeError

