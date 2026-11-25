/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes %s | %FileCheck --match-full-lines %s

// super() looks up new.target.prototype. When that lookup throws, the
// exception must unwind cleanly: no Locals may be left registered with the
// runtime, otherwise the next GC walks a dead stack frame.

function F() { this.f = 1; }

var P = new Proxy(function() {}, {
  get(t, k) {
    if (k === 'prototype')
      throw new Error('boom');
    return t[k];
  }
});

function work() {
  var a = [];
  for (var i = 0; i < 20000; i++)
    a.push({i: i, s: 'x' + i});
  return a.length;
}

class B extends F {
  constructor() {
    try {
      super();
    } catch (e) {
      print('caught in ctor:', e.message);
    }
    // Allocate and collect, then call builtins that use Locals.
    work();
    gc();
    print([3, 2, 1].map(function(x) { return x + 1; }).join());
    work();
    gc();
    print(Object.keys({a: 1, b: 2}).join());
    return {};
  }
}

for (var n = 0; n < 3; n++) {
  Reflect.construct(B, [], P);
  print('constructed', n);
}
print('done');

// CHECK:caught in ctor: boom
// CHECK-NEXT:4,3,2
// CHECK-NEXT:a,b
// CHECK-NEXT:constructed 0
// CHECK-NEXT:caught in ctor: boom
// CHECK-NEXT:4,3,2
// CHECK-NEXT:a,b
// CHECK-NEXT:constructed 1
// CHECK-NEXT:caught in ctor: boom
// CHECK-NEXT:4,3,2
// CHECK-NEXT:a,b
// CHECK-NEXT:constructed 2
// CHECK-NEXT:done
