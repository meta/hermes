/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// The first functions the x86-64 backend compiled natively: prologue,
// epilogue, the FR register file, and the LoadConst*/LoadParam/Mov/Ret
// emitters. That was true when this file was the entire backend; as of
// milestone 5 the backend reaches arm64's full opcode surface (only
// AsyncBreakCheck stays permanently unsupported, in code shared with arm64),
// so nothing here declines for lack of coverage any more. This file is kept
// as a minimal regression check on the original emitters, with each
// function's compile status still checked alongside its output.

function ret42() {
  return 42;
}
function identity(a) {
  return a;
}
function shuffle(a, b) {
  var t = a;
  a = b;
  b = t;
  return b;
}

print(ret42());
// CHECK: JIT successfully compiled FunctionID 1, 'ret42'
// CHECK: 42
print(identity("xy"));
// CHECK: JIT successfully compiled FunctionID 2, 'identity'
// CHECK: xy
print(shuffle(1, 2));
// CHECK: JIT successfully compiled FunctionID 3, 'shuffle'
// CHECK: 1
