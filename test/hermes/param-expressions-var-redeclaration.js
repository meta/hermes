/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -lazy %s | %FileCheck --match-full-lines %s
// RUN: %hermes -Xes6-block-scoping -Xenable-tdz %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// With parameter expressions, a body "var" that redeclares a parameter is a
// separate binding initialized from the parameter's value.

print('START');
// CHECK-LABEL: START

function f(x, get = () => x) {
  var x = 2;
  return [x, get()].join();
}
print(f(1));
// CHECK-NEXT: 2,1

function g(a, set = () => { a = 5; }) {
  var a;
  set();
  return a;
}
print(g(1));
// CHECK-NEXT: 1

function hoisted(x, get = () => x) {
  function x() {}
  return [typeof x, get()].join();
}
print(hoisted(1));
// CHECK-NEXT: function,1

function destructured({x}, get = () => x) {
  var x = 2;
  return [x, get()].join();
}
print(destructured({x: 1}));
// CHECK-NEXT: 2,1

function nested(x, get = () => x) {
  {
    var x = 2;
  }
  return [x, get()].join();
}
print(nested(1));
// CHECK-NEXT: 2,1

function* gen(x, get = () => x) {
  var x = 2;
  yield x;
  yield get();
}
print([...gen(1)].join());
// CHECK-NEXT: 2,1

var arrow = (x, get = () => x) => {
  var x = 2;
  return [x, get()].join();
};
print(arrow(1));
// CHECK-NEXT: 2,1

class C {
  m(x, get = () => x) {
    var x = 2;
    return [x, get()].join();
  }
}
print(new C().m(1));
// CHECK-NEXT: 2,1

// Without parameter expressions, the body "var" is the parameter binding.
function simple(x) {
  var x;
  return x;
}
print(simple(3));
// CHECK-NEXT: 3

async function af(x, get = () => x) {
  var x = 2;
  return [x, get()].join();
}
af(1).then(print);
// CHECK-NEXT: 2,1
