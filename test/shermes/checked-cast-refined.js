/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -O -dump-ir %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -typed -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -O %s | %FileCheck --match-full-lines %s

// A checked cast whose rejected ("bad") arms are objects, while the result
// can also be an object. Every object kind is checked at runtime with the
// same plain object check, so the cast must test the accepted arms; testing
// the rejected ones would throw on every valid object.

(function () {

class A {
  constructor() {}
}
class B {
  constructor() {}
}

// After optimization the `any` local is known to hold A|B|number|string, so
// the cast to the return type rejects only the class instance B.
function rejectClass(sel: number): A | number | string {
  'noinline';
  let v: any;
  if (sel === 0) v = new A();
  else if (sel === 1) v = new B();
  else if (sel === 2) v = 1;
  else v = 's';
  return v;
}
// IR-LABEL: function rejectClass(
// IR: CheckedTypeCastInst (:string|number|classInstance(A)) %{{.*}}: string|number|classInstance(A)|classInstance(B),

// The rejected arm is the plain object that absorbs A.
function rejectObject(sel: number): A | number | string {
  'noinline';
  let v: any;
  if (sel === 0) v = new A();
  else if (sel === 1) v = {};
  else if (sel === 2) v = 1;
  else v = 's';
  return v;
}
// IR-LABEL: function rejectObject(
// IR: CheckedTypeCastInst (:string|number|classInstance(A)) %{{.*}}: string|number|object,

function run(f: (sel: number) => A | number | string): void {
  for (let i of [0, 2, 3]) {
    try {
      print(typeof f(i));
    } catch (e) {
      print('caught', e.message);
    }
  }
}

run(rejectClass);
// CHECK: object
// CHECK-NEXT: number
// CHECK-NEXT: string

run(rejectObject);
// CHECK-NEXT: object
// CHECK-NEXT: number
// CHECK-NEXT: string

})();
