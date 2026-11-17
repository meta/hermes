/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -verify-ir -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -verify-ir -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -verify-ir -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O0 -dump-ir %s | %FileCheck --check-prefix=IR %s

// Assigning a function to a static field of a structurally compatible but
// different function type. The two IR function types are unrelated under
// shallow subtyping (D4), so the store into the field's frame variable needs
// the value narrowed to the field's type.

(function main() {

function foo(x: number | string): number { return 2; }
function bar(x: number): number | string { return x; }
class K {
  static f: (x: number) => number | string = bar;
}

K.f = foo;
// IR-LABEL: function main(
// IR: [[FOO:%[0-9]+]] = CheckedTypeCastInst (:function(number)) %{{[0-9]+}}: any, type(function(number))
// IR-NEXT: [[NARROW:%[0-9]+]] = UnionNarrowTrustedInst (:function(string|number)) [[FOO]]: function(number)
// IR-NEXT: StoreFrameInst %{{[0-9]+}}: environment, [[NARROW]]: function(string|number), [%VS{{[0-9]+}}.f]: empty|function(string|number)

print(K.f(1));
// CHECK: 2

})();
