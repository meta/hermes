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

// Environments, closures and the inline young-gen allocation that creating an
// environment performs. The first RUN line is the real check -- interpreter
// and JIT must print the same thing. The second re-runs it with the type
// asserts on, which turn a sync-without-free mistake around the emitted calls
// into a trap instead of a silent misread. The FileCheck lines pin that the
// functions under test were in fact compiled, so the differential cannot
// degrade into comparing the interpreter against itself.
//
// Restricted to plain closures and environments on purpose, without array or
// object literals or string operations -- not because those decline (they
// all compile now; arrays.js, objects.js and strings.js cover them), but to
// keep this file's shapes isolated to the one opcode family it is testing.
// Every differential RUN line above carries -Xjit-crash-on-error: nothing
// here declines, in either optimization level (measured).
//
// The -O0 RUN lines are here for coverage, not for the flag. At -O the scope
// optimizer merges every scope in this file into the function's own, so the
// only environment opcodes that survive are CreateFunctionEnvironment,
// GetParentEnvironment and the load/store pair. -O0 keeps the scopes apart
// and additionally exercises CreateEnvironment (whose parent arrives in a
// register), GetEnvironment with a non-zero level and StoreNPToEnvironment.
// The last -O0 RUN line pins four of the functions it compiles, so a future
// decline cannot quietly turn the -O0 differential into interpreter versus
// interpreter. The unprefixed CHECK lines below are the -O statuses; the
// CHECK0 lines are the -O0 ones, and the FunctionIDs differ between them
// because -O inlines functions that -O0 keeps.
//
// CreateTopLevelEnvironment IS covered here, as of the property milestone.
// It is emitted only in `global`, and `global` used to decline on
// DeclareGlobalVar in both modes, so none of its code ran. Now that
// declareGlobalVar and getGlobalObject are emitted, `global` compiles at -O
// and -O0 alike, and the first CHECK/CHECK0 pair below pins that -- which is
// what makes this file's top-level environment, and every closure created
// from it, come out of compiled code. globals.js covers the globals
// themselves.

// Captures a parameter in a mutable variable, so the closure both loads from
// and stores to the environment. The environment outlives mkCounter, so the
// slot fill and the parent pointer have to survive a GC.
function mkCounter(start) {
  var n = start;
  return function () {
    n = n + 1;
    return n;
  };
}
// The captured variable is never stored to, so the inner function only reads
// its parent environment.
function mkAdder(a) {
  return function (b) {
    return a + b;
  };
}
// Curried, so the innermost closure captures one variable from its parent
// scope and one from its grandparent's. This is the only shape in the file
// whose compiled code follows Environment::parentEnvironment: at -O the scope
// optimizer flattens everything else, and a parent link written to the wrong
// field or left unwritten is invisible without it.
function mkAdd3(a) {
  return function (b) {
    return function (c) {
      return a + b + c;
    };
  };
}
// The one shape in the file that leaves a GC point between the environment's
// allocation and the first store into its slots, which is the window the slot
// fill in emit_environment_init exists to close. The closure is stored into
// the very variable it captures, so the lowering is
//   CreateFunctionEnvironment(size 1) -> CreateClosure -> StoreToEnvironment
// and the hoisted `var x = undefined` store that would normally precede
// CreateClosure is deleted by FrameLoadStoreOpts::eliminateStores as dead.
// CreateClosure allocates, so it is a GC safepoint at which the environment
// is already reachable from the frame and slot 0 still holds whatever the
// bump allocator handed us. Every other function here stores into its slots
// before it allocates again, which is why none of them can see the fill.
function mkLate(v) {
  var x;
  var g = function () {
    return x;
  };
  x = g;
  return g;
}
// Three levels of nesting. At -O the inliner folds inner() and innermost()
// into nest, which leaves nest with no calls and no environments at all, so
// it compiles here and pins only the arithmetic. At -O0 the calls survive,
// and now that calls are implemented all three levels compile: this is the
// one place in the file where a compiled function calls another compiled
// function that reads its grandparent's environment.
function nest(x) {
  var y = x * 2;
  function inner() {
    var z = y + 1;
    function innermost() {
      return z + y + x;
    }
    return innermost();
  }
  return inner();
}

// Allocate enough environments to force young-generation collections with
// some of them still live. Without this the whole file fits in one young
// generation and the GC never looks at what the emitted code wrote: an
// environment whose cell header, size field or parent link is wrong is
// invisible until something scans it. churn itself compiles too (see its
// pin below), so both the environments it builds by calling mkCounter and
// its own loop body run as emitted code.
function churn(iters) {
  var keep = null;
  var acc = 0;
  for (var i = 0; i < iters; ++i) {
    var c = mkCounter(i);
    acc = acc + c();
    if ((i & 4095) === 0)
      keep = c;
  }
  return acc + keep();
}

// The same idea for mkLate, and the one that actually lands a young-gen
// collection inside the allocate-then-store window: with this many
// iterations some CreateClosure in mkLate is the allocation that triggers
// the collection, and the half-initialized environment is scanned then.
function churnLate(iters) {
  var keep = null;
  var n = 0;
  for (var i = 0; i < iters; ++i) {
    var m = mkLate(i);
    if (m() === m)
      n = n + 1;
    if ((i & 4095) === 0)
      keep = m;
  }
  return keep() === keep ? n : -1;
}

// Most of the returned closures are anonymous, so their status lines carry an
// empty name and only the FunctionID tells them apart: 8 is mkCounter's, 9 is
// mkAdder's, and 10 and 12 are mkAdd3's two levels. mkLate's is named 'g'
// after the variable it is assigned to. churn and churnLate name mkCounter
// and mkLate, which are global bindings; reading a global used to decline,
// so they had no status lines of their own, and now they do.

// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 0, 'global'
var c = mkCounter(10);
// CHECK: JIT successfully compiled FunctionID 1, 'mkCounter'
// CHECK0: JIT successfully compiled FunctionID 1, 'mkCounter'
print(c());
// CHECK: JIT successfully compiled FunctionID 8, ''
// CHECK-NEXT: 11
print(c());
// CHECK-NEXT: 12
print(c());
// CHECK-NEXT: 13
var add5 = mkAdder(5);
// CHECK: JIT successfully compiled FunctionID 2, 'mkAdder'
print(add5(3));
// CHECK: JIT successfully compiled FunctionID 9, ''
// CHECK-NEXT: 8
print(add5(-0.5));
// CHECK-NEXT: 4.5
print(nest(7));
// At -O the inliner leaves nest callless and it is the only one of the three
// that exists. At -O0 all three survive and all three compile, in the order
// they are first entered; inner and innermost are the file's only compiled
// calls into compiled code.
// CHECK: JIT successfully compiled FunctionID 5, 'nest'
// CHECK0: JIT successfully compiled FunctionID 5, 'nest'
// CHECK0: JIT successfully compiled FunctionID 12, 'inner'
// CHECK0: JIT successfully compiled FunctionID 14, 'innermost'
// CHECK-NEXT: 36
print(churn(30000));
// churn's own status line prints between this and the line above, so this
// one cannot be a CHECK-NEXT.
// CHECK: JIT successfully compiled FunctionID 6, 'churn'
// CHECK0: JIT successfully compiled FunctionID 6, 'churn'
// CHECK-NEXT: 450043674
var add2 = mkAdd3(1);
// CHECK: JIT successfully compiled FunctionID 3, 'mkAdd3'
var add21 = add2(2);
// CHECK: JIT successfully compiled FunctionID 10, ''
print(add21(3));
// CHECK: JIT successfully compiled FunctionID 12, ''
// CHECK-NEXT: 6
print(add21(-0.5));
// CHECK-NEXT: 2.5
print(mkAdd3(100)(20)(3));
// CHECK-NEXT: 123
var late = mkLate(0);
// CHECK: JIT successfully compiled FunctionID 4, 'mkLate'
print(late() === late);
// CHECK: JIT successfully compiled FunctionID 11, 'g'
// CHECK-NEXT: true
print(churnLate(30000));
// churnLate's status line prints in between, as churn's does.
// CHECK: JIT successfully compiled FunctionID 7, 'churnLate'
// CHECK0: JIT successfully compiled FunctionID 7, 'churnLate'
// CHECK-NEXT: 30000
