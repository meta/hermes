/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

print('optional chaining');
// CHECK-LABEL: optional chaining

var a = undefined;
var b = {
  x: 1,
  y: function(arg) {
    return arg;
  },
  getThis: function() {
    return this;
  }
};

function foo() {
  print('foo called');
  return 50;
}

print(a?.x);
// CHECK-NEXT: undefined
print(b?.x);
// CHECK-NEXT: 1

print(a?.(42));
// CHECK-NEXT: undefined
print(a?.(foo()));
// CHECK-NEXT: undefined
print(a?.y?.(42));
// CHECK-NEXT: undefined
print(a?.b.c);
// CHECK-NEXT: undefined
print(a?.b().c);
// CHECK-NEXT: undefined
print(a?.b?.().c);
// CHECK-NEXT: undefined
print(a?.().b);
// CHECK-NEXT: undefined

print(b?.y(42));
// CHECK-NEXT: 42
print(b?.y?.(42));
// CHECK-NEXT: 42
print(b.y?.(foo()));
// CHECK-NEXT: foo called
// CHECK-NEXT: 50
print(b?.y(foo()));
// CHECK-NEXT: foo called
// CHECK-NEXT: 50
print(b?.y?.(foo()));
// CHECK-NEXT: foo called
// CHECK-NEXT: 50
print(b?.z?.(42));
// CHECK-NEXT: undefined
print(b?.getThis?.(42) === b);
// CHECK-NEXT: true
print((b.getThis)?.(42) === b);
// CHECK-NEXT: true
print((b?.getThis)?.(42) === b);
// CHECK-NEXT: true

var obj = {
  a: {b: 3}
};
print(obj?.a?.b);
// CHECK-NEXT: 3
print(delete obj?.a?.b);
// CHECK-NEXT: true
print(obj?.a?.b);
// CHECK-NEXT: undefined
print(delete obj?.a?.b);
// CHECK-NEXT: true
print(obj?.a?.b);
// CHECK-NEXT: undefined
print(delete obj?.a);
// CHECK-NEXT: true
print(obj?.a?.b);
// CHECK-NEXT: undefined

// A short-circuited chain evaluates to undefined, not a reference, so
// deleting through it returns true.
print(delete a?.x);
// CHECK-NEXT: true
print(delete null?.x);
// CHECK-NEXT: true
print(delete a?.[foo()]);
// CHECK-NEXT: true
print(delete a?.b.c);
// CHECK-NEXT: true
print(delete a?.b().c);
// CHECK-NEXT: true
print(delete obj?.a?.b);
// CHECK-NEXT: true
