/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xjit=force -Xjit-crash-on-error %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// Test that frameUpToDate for global regs is reset between basic blocks.
//
// The test only means anything if `i` actually gets a global register, which
// is not a given on a backend with a smaller global pool: x86-64 has three
// (rbx, r12, r13) against arm64's sixteen. It does get one there --
// -Xdump-jitcode=3 prints `alloc: r3 <= r0` in F28's prologue, and the loop
// counter is FR0 (`LoadConstZero r0` syncs into rbx) -- so the shape this
// test was written for is exercised on both backends.

function F28 (scene) {
    for (var i = 0; i < scene.things.length; ++i) {
        // i < scene.things.length syncs i and sets its frameUpToDate
        // if it remains set here, the new value isn't used by getValById.
        print(i, scene.things[i]);
    }
};

var sc = {things: ["a", "b", "c"]};
print(JSON.stringify(sc));
F28(sc);
// CHECK: {"things":["a","b","c"]}
// CHECK-NEXT: 0 a
// CHECK-NEXT: 1 b
// CHECK-NEXT: 2 c
