/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %hermesc -typed -dump-ir %s 2>&1 ) | %FileCheck --match-full-lines %s

// In a typed class, super() is only supported as the first statement of the
// derived constructor. A super() nested in an arrow function must report an
// error instead of crashing IRGen.

class A {
  x: number;
  constructor() {
    this.x = 1;
  }
}

class B extends A {
  constructor() {
    super();
    (() => {
      super();
    })();
  }
}
// CHECK: {{.*}}super-in-arrow-error.js:25:7: error: super() is only supported as the first statement of a derived class constructor
// CHECK-NEXT:      super();
// CHECK-NEXT:      ^~~~~~~

class C extends A {
  constructor() {
    super();
    var f = async () => {
      super();
    };
  }
}
// CHECK: {{.*}}super-in-arrow-error.js:37:7: error: super() is only supported as the first statement of a derived class constructor
// CHECK-NEXT:      super();
// CHECK-NEXT:      ^~~~~~~

// CHECK: Emitted 2 errors. exiting.
