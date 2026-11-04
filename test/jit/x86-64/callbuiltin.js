/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fstatic-builtins %s > %t.int && %hermes -fstatic-builtins -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fstatic-builtins %s > %t.int && %hermes -fstatic-builtins -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -fstatic-builtins -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// CallBuiltin, which is the one call shape that needs no callee value at
// all: -fstatic-builtins turns `Math.floor(x)` into a single opcode that
// names the builtin by index, so a compiled function can reach the standard
// library without any property access at all. Both sides of the
// differential pass -fstatic-builtins so that CallBuiltin is the opcode
// actually under test; without it these calls lower to ordinary property
// loads instead, which compile too now (confirmed separately) but exercise
// props.js's opcodes rather than this file's. Both RUN lines carry
// -Xjit-crash-on-error: nothing here declines (measured).
//
// The four bodies below use exactly these opcodes: LoadParam, Mov,
// CallBuiltin, AddN, DivN, Ret, LoadConstZero, LoadConstUInt8, Less,
// JmpFalse and JLess -- every one of which the backend implements, which is
// why they compile end to end.
//
// _jit_call_builtin populates the ThisArg slot itself, so the arguments the
// emitter has to place are only the real ones. `mixed` is the interesting
// shape: its first CallBuiltin writes its result straight into the argument
// slot the next one reads, so a result register left aliasing a stale
// argument slot shows up as a wrong number.

function floors(x) {
  return Math.floor(x) + Math.abs(x) + Math.trunc(x);
}
function minmax(x, y) {
  return Math.min(x, y) + Math.max(x, y);
}
// Two calls in a row that reuse the same argument slot, plus a builtin whose
// result feeds another builtin's argument.
function mixed(x) {
  return Math.abs(Math.floor(x)) + Math.ceil(x) + Math.round(x);
}
// A builtin called in a loop, so the call site runs many times and any
// register the emitter failed to sync across it shows up as a wrong sum
// rather than as a one-off.
function sumFloors(n) {
  var acc = 0;
  for (var i = 0; i < n; i = i + 1)
    acc = acc + Math.floor(i / 3);
  return acc;
}

print(floors(-3.5));
// CHECK: JIT successfully compiled FunctionID 1, 'floors'
// CHECK: -3.5
print(floors(7.25));
// CHECK: 21.25
print(minmax(2, 7));
// CHECK: JIT successfully compiled FunctionID 2, 'minmax'
// CHECK: 9
print(minmax(-1.5, -1.5));
// CHECK: -3
print(mixed(-2.25));
// CHECK: JIT successfully compiled FunctionID 3, 'mixed'
// CHECK: -1
print(mixed(4.75));
// CHECK: 14
print(sumFloors(1000));
// CHECK: JIT successfully compiled FunctionID 4, 'sumFloors'
// CHECK: 166167
