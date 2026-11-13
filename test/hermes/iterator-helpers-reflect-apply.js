/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s

// Iterator helpers call callbacks and iterator methods with the spec's Call
// operation, which user code cannot intercept. Replace Reflect.apply before
// the first iterator helper runs and check that the helpers never call the
// replacement.

print('iterator helpers');
// CHECK-LABEL: iterator helpers

var realApply = Reflect.apply;
var applyCalls = 0;
Reflect.apply = function (target, thisArg, args) {
  ++applyCalls;
  return realApply(target, thisArg, args);
};

function values() {
  return [1, 2, 3, 4].values();
}
function double(x) {
  return x * 2;
}
function isOdd(x) {
  return (x & 1) === 1;
}

print(values().map(double).toArray());
// CHECK-NEXT: 2,4,6,8
print(values().filter(isOdd).toArray());
// CHECK-NEXT: 1,3
print(values().take(2).toArray(), values().drop(2).toArray());
// CHECK-NEXT: 1,2 3,4
print(values().flatMap(function (x) { return [x, -x].values(); }).toArray());
// CHECK-NEXT: 1,-1,2,-2,3,-3,4,-4
print(values().reduce(function (a, b) { return a + b; }));
// CHECK-NEXT: 10
print(values().some(isOdd), values().every(isOdd), values().find(function (x) { return x > 2; }));
// CHECK-NEXT: true false 3
var sum = 0;
values().forEach(function (x) { sum += x; });
print(sum);
// CHECK-NEXT: 10
print(Iterator.from({ next: function () { return { done: true }; } }).toArray().length);
// CHECK-NEXT: 0
print(Iterator.concat([1].values(), [2].values()).toArray());
// CHECK-NEXT: 1,2

var closed = 0;
var source = Object.setPrototypeOf({
  next: function () { return { done: false, value: 1 }; },
  return: function () { ++closed; return {}; },
}, Iterator.prototype);
print(source.map(double).take(1).toArray(), closed);
// CHECK-NEXT: 2 1

print('Reflect.apply calls:', applyCalls);
// CHECK-NEXT: Reflect.apply calls: 0
