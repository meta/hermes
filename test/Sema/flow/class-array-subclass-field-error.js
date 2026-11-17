/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -typed -dump-sema -ferror-limit=0 %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

// Instances of a class deriving from Array<T> are fast arrays, which cannot
// hold stored fields. A subclass of a class with fields inherits them, so it
// is rejected too. Methods, private methods and statics are not stored on the
// instance, so a class with only those is accepted.

class WithField extends Array<number> {
  x: number = 0;
}
class SubOfWithField extends WithField {}
class WithPrivateField extends Array<number> {
  #p: number;
}

class MethodsOnly extends Array<number> {
  m(): number {
    return this.#pm();
  }
  #pm(): number {
    return 1;
  }
  @Hermes.final
  f(): number {
    return 2;
  }
  static s: number = 3;
}
class SubOfMethodsOnly extends MethodsOnly {
  m(): number {
    return 4;
  }
}

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}class-array-subclass-field-error.js:15:25: error: ft: a class deriving from Array cannot have instance fields
// CHECK-NEXT:class WithField extends Array<number> {
// CHECK-NEXT:                        ^~~~~
// CHECK-NEXT:{{.*}}class-array-subclass-field-error.js:18:30: error: ft: a class deriving from Array cannot have instance fields
// CHECK-NEXT:class SubOfWithField extends WithField {}
// CHECK-NEXT:                             ^~~~~~~~~
// CHECK-NEXT:{{.*}}class-array-subclass-field-error.js:19:32: error: ft: a class deriving from Array cannot have instance fields
// CHECK-NEXT:class WithPrivateField extends Array<number> {
// CHECK-NEXT:                               ^~~~~
// CHECK-NEXT:Emitted 3 errors. exiting.
