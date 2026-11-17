/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O -dump-ir %s | %FileCheck --check-prefix=IR %s

// Two distinct function types overlap (D5) but neither is a subtype of the
// other (D4), so their exact intersection is not expressible. intersectTy
// returns its first operand for such a pair (D9); returning NoType would type
// the cast below as `null` and make it reject the function.

(function () {

function foo(x: number | string): number { return 2; }

// After optimization `v` is known to hold foo's function type or null. The
// cast to the return type intersects that with a different function type.
function pick(b: boolean): ((x: number) => number | string) | null {
  'noinline';
  let v: any;
  if (b) v = foo;
  else v = null;
  return v;
}
// IR-LABEL: function pick(
// IR: CheckedTypeCastInst (:null|function(string|number)) %{{[0-9]+}}: null|function(number), type(null|function(string|number))

function use(b: boolean): string {
  'noinline';
  const f = pick(b);
  return f === null ? 'null' : 'function';
}

print(use(true), use(false));
// CHECK: function null

})();
