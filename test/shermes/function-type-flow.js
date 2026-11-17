/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O0 -dump-ir %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -typed -O0 -dump-ir -dump-source-location=loc %s | %FileCheck --check-prefix=LOC %s

(function main() {

function foo(x: number|string): number { return 2; }
function bar(x: number): number|string { return x; }

// Ensure foo flows into bar.
// The two function types are neither a subset nor a superset of each other
// under shallow subtyping (D4), so no cast can express the relation.
// narrowToFlowType narrows foo's value to bar's type with a trusted narrow,
// relying on the FlowChecker having proved the assignment, and never with a
// runtime CheckedTypeCast.
bar = foo;
// IR-LABEL: function main(
// IR: [[FOO:%[0-9]+]] = CheckedTypeCastInst (:function(number)) %{{[0-9]+}}: any, type(function(number))
// IR-NEXT: [[NARROW:%[0-9]+]] = UnionNarrowTrustedInst (:function(string|number)) [[FOO]]: function(number)
// IR-NEXT: StoreFrameInst %{{[0-9]+}}: environment, [[NARROW]]: function(string|number), [%VS{{[0-9]+}}.bar]: any
// The narrow has the location of the assignment target, like a checked cast.
// LOC-LABEL: function main(
// LOC: function-type-flow.js:[[@LINE-7]]:1
// LOC-NEXT: UnionNarrowTrustedInst (:function(string|number))

print(bar(1));
// CHECK: 2

})();
