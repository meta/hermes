/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -verify-ir -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -verify-ir -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -verify-ir -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O -dump-ir %s | %FileCheck --check-prefix=IR %s

// Assigning a nullable function to a nullable static field of a structurally
// compatible but different function type. The value is narrowed to the
// field's type; after optimization the narrow's input is known to be foo's
// function type or null, and the narrow must keep the function arm (D9).

(function () {

function foo(x: number | string): number { return 2; }
function bar(x: number): number | string { return x; }

function pick(b: boolean): ((x: number | string) => number) | null {
  'noinline';
  return b ? foo : null;
}

class K {
  static f: ((x: number) => number | string) | null = bar;
}

function setF(b: boolean): void {
  'noinline';
  K.f = pick(b);
}
// IR-LABEL: function setF(
// IR: [[PICK:%[0-9]+]] = CallInst {{.*}}(:null|function(number))
// IR-NEXT: [[NARROW:%[0-9]+]] = UnionNarrowTrustedInst (:null|function(string|number)) [[PICK]]: null|function(number)
// IR-NEXT: StoreFrameInst %{{[0-9]+}}: environment, [[NARROW]]: null|function(string|number), [%VS{{[0-9]+}}.f]: null|function(string|number)

function callF(): number | string {
  'noinline';
  const f = K.f;
  if (f === null) return 'null';
  const g: any = f;
  return g(1);
}

setF(true);
print(callF());
// CHECK: 2
setF(false);
print(callF());
// CHECK-NEXT: null

})();
