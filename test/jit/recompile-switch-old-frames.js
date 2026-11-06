/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=0 -Xjit-recompile-threshold=64 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit

// A string switch dispatches through a jump table that belongs to the body
// executing it, not to the newest compiled version of the function. This test
// recompiles the function in the middle of a deep recursion, so that every
// frame still on the native stack belongs to version 1, and then keeps
// switching in those frames all the way out of the recursion.
//
// Each level performs one string switch on the way down and another on the
// way up, and one PutById that declines, so the decline threshold is crossed
// deep inside a single outer call to rec(): version 2 is installed while
// roughly 60 version-1 frames are live, and those frames run their remaining
// switches afterwards. Run on the ASan build tree.
//
// The -Xjit-max-recompiles=0 line is the differential: with recompilation
// disabled there is only ever one body, so it must produce the same output.

// Twelve strings for ten cases: "yankee" and "zulu" miss, exercising the
// default path of the switch as well.
var LABELS = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
              "golf", "hotel", "india", "juliet", "yankee", "zulu"];

function rec(o, n) {
  // Switch on the way down, in a loop, so a level performs many of them.
  var down = 0;
  for (var i = 0; i < 4; ++i) {
    switch (LABELS[(n + i) % 12]) {
      case "alpha": down += 1; break;
      case "bravo": down += 2; break;
      case "charlie": down += 3; break;
      case "delta": down += 4; break;
      case "echo": down += 5; break;
      case "foxtrot": down += 6; break;
      case "golf": down += 7; break;
      case "hotel": down += 8; break;
      case "india": down += 9; break;
      case "juliet": down += 10; break;
    }
  }

  // The PutById that declines once per level, driving the recompile.
  o.p = n;

  var below = n > 0 ? rec(o, n - 1) : 0;

  // Switch on the way up: for the deeper levels this runs after version 2
  // has been installed, in a frame that belongs to version 1.
  var up = 0;
  for (var j = 0; j < 4; ++j) {
    switch (LABELS[(n + j + 5) % 12]) {
      case "alpha": up += 100; break;
      case "bravo": up += 200; break;
      case "charlie": up += 300; break;
      case "delta": up += 400; break;
      case "echo": up += 500; break;
      case "foxtrot": up += 600; break;
      case "golf": up += 700; break;
      case "hotel": up += 800; break;
      case "india": up += 900; break;
      case "juliet": up += 1000; break;
    }
  }

  return down + up + below;
}

print("start");
// CHECK: start
// DUMP: start

print(rec({p: -1}, 100));
// CHECK-NEXT: 189950
// The DUMP checks pin the version-2 banner between the call and its
// result: it prints before 189950, which is only printed once the single
// outer call to rec() has returned, so the recompile lands inside that
// call, while its version-1 frames are still on the stack.
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec' (version 2)
// DUMP: 189950

print("done");
// CHECK-NEXT: done
// DUMP: done
