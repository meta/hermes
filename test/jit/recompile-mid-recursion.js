/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck %s
// REQUIRES: jit

// The recursion performs one PutById decline per level. The decline
// threshold (64, pinned on the RUN line) is crossed deep inside a
// single outer invocation, so the recompile installs version 2 while
// ~64 frames of version 1 are still on the native stack; they must
// unwind through the retired body correctly. Run on the ASan build
// tree, where a freed or corrupted retired body would be caught.

function r(o, n) {
  o.p = n;
  if (n > 0)
    r(o, n - 1);
  return o.p;
}

print(r({p: -1}, 100));
// CHECK: 0
