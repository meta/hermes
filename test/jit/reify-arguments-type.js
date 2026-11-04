/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int && %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit && diff %t.int %t.jit
// REQUIRES: jit

// Regression test for the reifyArguments stale-type bug (dz
// 01a03bbd-4870): after ReifyArguments the lazy-arguments FR holds an
// Arguments object, but the backend must widen the FR's recorded type
// past the OtherNonPtr recorded by the initial LoadConstUndefined.
// The `a === b` below is load-bearing: with the stale non-pointer
// type, strictEqual selects the raw-bit fast tier, and under
// -Xjit-emit-type-asserts the assert traps on the object value.
// Replacing it with e.g. a length comparison would still pass but
// would silently delete this coverage.

function reifyTwice() {
  var a = arguments;
  var b = arguments;
  return (a === b) + "/" + a.length;
}

print(reifyTwice(1, 2));
