/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O0 -dump-ir %s | %FileCheck --check-prefix=IR %s

(function main() {

function foo(x: number|string): number { return 2; }
function bar(x: number): number|string { return x; }

// Ensure foo flows into bar.
// The two function types are neither a subset nor a superset of each other
// under shallow subtyping, so narrowToFlowType stores foo's value into bar as
// is, with no cast.
bar = foo;
// IR-LABEL: function main(
// IR: [[FOO:%[0-9]+]] = CheckedTypeCastInst (:function(number)) %{{[0-9]+}}: any, type(function(number))
// IR-NEXT: StoreFrameInst %{{[0-9]+}}: environment, [[FOO]]: function(number), [%VS{{[0-9]+}}.bar]: any

print(bar(1));
// CHECK: 2

})();
