/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-crash-on-error %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit3 && diff %t.int0 %t.jit3
// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// REQUIRES: jit

// Globals, and the top-level function itself: DeclareGlobalVar (one per
// module-level `var`, emitted at the top of `global`) and GetGlobalObject
// (every read or write of one, and every reference to a global native like
// `print`).
//
// THIS FILE EXISTS TO PIN `global`. Until this task `global` declined on
// DeclareGlobalVar, which meant CreateTopLevelEnvironment -- emitted in no
// other function -- had never executed in compiled code, and closures.js
// had to record that gap instead of covering it. The CHECK and CHECK0 lines
// below pin `JIT successfully compiled FunctionID 0, 'global'` at both -O
// and -O0, so that gap is now closed and cannot silently reopen.
//
// The file is deliberately written without a single string constant (the
// "use strict" directive prologue is stripped at parse time and emits no
// LoadConstString), in the top-level code and everywhere else. LoadConstString
// compiles now, so a string literal would no longer make `global` decline --
// but keeping this file string-free still isolates the globals pin from
// string coverage, which is what lets props.js, which needs string keys for
// its by-val forms, carry its own separate coverage without disturbing this
// one. Nothing else is restricted --
// the module-level `var`s below are read, written, read-modify-written and
// captured, and the last one is the target of a global function call.

// Read and read-modify-write of a global from a compiled function.
var counter = 0;
function bump(n) {
  counter = counter + n;
  return counter;
}

// A plain read of a different global, which must not disturb the first.
var base = 100;
function scaled(n) {
  return base * n + counter;
}

// A global whose value is a function, called through the global object.
var hook = function (x) {
  return x + 1;
};
function callHook(x) {
  return hook(hook(x));
}

// A global native reached through the global object. This is also the one
// call in the file that the emitted code cannot enter directly, so it is
// what moves the slow-call path.
function show(a, b) {
  print(a, b);
}

// A global written from one function and read from another, with a loop in
// between, so the write is not trivially forwarded.
function accumulate(n) {
  for (var i = 0; i < n; ++i)
    counter = counter + i;
  return counter;
}

// The same write in strict mode. PutById carries its strictness and its
// "try" flag as the seventh and eighth arguments, which on SysV travel on
// the stack rather than in registers; a loose-mode-only test would push
// (0, 0) and could not tell the two apart if they were swapped.
function bumpStrict(n) {
  "use strict";
  counter = counter + n;
  return counter;
}

// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 0, 'global'

print(bump(2), bump(3), counter);
// CHECK: JIT successfully compiled FunctionID 1, 'bump'
// CHECK-NEXT: 2 5 5
// CHECK0: JIT successfully compiled FunctionID 1, 'bump'
// CHECK0-NEXT: 2 5 5

print(scaled(2));
// CHECK: JIT successfully compiled FunctionID 2, 'scaled'
// CHECK-NEXT: 205
// CHECK0: JIT successfully compiled FunctionID 2, 'scaled'
// CHECK0-NEXT: 205

print(callHook(1));
// CHECK: JIT successfully compiled FunctionID 3, 'callHook'
// CHECK: JIT successfully compiled FunctionID 7, 'hook'
// CHECK-NEXT: 3
// CHECK0: JIT successfully compiled FunctionID 3, 'callHook'
// CHECK0: JIT successfully compiled FunctionID 7, 'hook'
// CHECK0-NEXT: 3

show(counter, base);
// CHECK: JIT successfully compiled FunctionID 4, 'show'
// CHECK-NEXT: 5 100
// CHECK0: JIT successfully compiled FunctionID 4, 'show'
// CHECK0-NEXT: 5 100

print(accumulate(1000), counter);
// CHECK: JIT successfully compiled FunctionID 5, 'accumulate'
// CHECK-NEXT: 499505 499505
// CHECK0: JIT successfully compiled FunctionID 5, 'accumulate'
// CHECK0-NEXT: 499505 499505

// Reassigning a global from the top level, which is itself compiled code
// here, and observing it from a compiled function.
hook = function (x) {
  return x * 2;
};
print(callHook(3));
// CHECK: JIT successfully compiled FunctionID 8, 'hook'
// CHECK-NEXT: 12
// CHECK0: JIT successfully compiled FunctionID 8, 'hook'
// CHECK0-NEXT: 12

print(bumpStrict(4), counter);
// CHECK: JIT successfully compiled FunctionID 6, 'bumpStrict'
// CHECK-NEXT: 499509 499509
// CHECK0: JIT successfully compiled FunctionID 6, 'bumpStrict'
// CHECK0-NEXT: 499509 499509
