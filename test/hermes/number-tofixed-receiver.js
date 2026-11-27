/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// Reject invalid receivers before coercing or range-checking fractionDigits.
[{}, null, undefined, true, "1", Symbol(), 1n,
 new Proxy(new Number(1), {})].forEach(function (receiver, index) {
  var coerced = false;
  var coercionError;
  var rangeError;
  try {
    Number.prototype.toFixed.call(receiver, {
      valueOf: function () {
        coerced = true;
        throw new Error("fractionDigits coerced");
      }
    });
  } catch (e) {
    coercionError = e.name;
  }
  try {
    Number.prototype.toFixed.call(receiver, 101);
  } catch (e) {
    rangeError = e.name;
  }
  print("receiver", index, coercionError, coerced, rangeError);
});
// CHECK: receiver 0 TypeError false TypeError
// CHECK-NEXT: receiver 1 TypeError false TypeError
// CHECK-NEXT: receiver 2 TypeError false TypeError
// CHECK-NEXT: receiver 3 TypeError false TypeError
// CHECK-NEXT: receiver 4 TypeError false TypeError
// CHECK-NEXT: receiver 5 TypeError false TypeError
// CHECK-NEXT: receiver 6 TypeError false TypeError
// CHECK-NEXT: receiver 7 TypeError false TypeError

// Primitive and boxed numbers still coerce fractionDigits once.
[1.25, new Number(1.25)].forEach(function (receiver) {
  var calls = 0;
  print(Number.prototype.toFixed.call(receiver, {
    valueOf: function () { ++calls; return 2; }
  }), calls);
});
// CHECK-NEXT: 1.25 1
// CHECK-NEXT: 1.25 1
