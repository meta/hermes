/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -lazy %s | %FileCheck --match-full-lines %s
// RUN: %hermes -lazy -Xg3 %s | %FileCheck --match-full-lines %s
// XFAIL: windows
// UNSUPPORTED: ubsan || asan

// Compiling outer() also compiles the arrow functions nested in it, and
// bytecode generation fails on the innermost one, whose environment access
// is out of reach. outer must remain usable after the failure.
// With -Xg3, the functions keep eval data, so the discarded outer owns its IR.
// Under sanitizers, the parser reports nesting overflow first.

"use strict";

function sink(x) { return x; }
function outer(a, b, c) {
  var v = sink(1);
  return ()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>()=>{ "show source"; return v; };
}

try {
  outer();
} catch (e) {
  print(1, e.name, e.message);
  // CHECK: 1 SyntaxError {{.*}}Variable environment is out-of-reach
}
try {
  outer();
} catch (e) {
  print(2, e.name, e.message);
  // CHECK-NEXT: 2 SyntaxError {{.*}}Variable environment is out-of-reach
}
print(outer.length, outer.name);
// CHECK-NEXT: 3 outer

// h is compiled after the failure, and inner gets the ID that the failed
// compilation gave to the innermost arrow function. getBytecodeAddress() adds
// up the bytecode sizes of all the functions with a lower ID.
function h() {
  return function inner() { throw new Error(); };
}
Error.prepareStackTrace = (e, sites) => sites[0].getBytecodeAddress();
try {
  h()();
} catch (e) {
  print(4, typeof e.stack);
  // CHECK-NEXT: 4 number
}
// The source of the innermost arrow function, which has a "show source"
// directive, must not be used for inner.
print(5, h().toString());
// CHECK-NEXT: 5 function inner() { [bytecode] }
