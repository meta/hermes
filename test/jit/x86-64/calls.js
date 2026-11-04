/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-crash-on-error %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// RUN: %hermes -Xjit=force -Xjit-emit-counters %s 2>&1 >/dev/null | %FileCheck --check-prefix=COUNT %s
// REQUIRES: jit

// Calls: direct and mutual recursion, the Call1-Call4 arities and the generic
// Call, construction (CreateThis/Construct/SelectObject) and higher-order
// calls through a parameter. The first RUN line is the real check --
// interpreter and JIT must print the same thing. The second re-runs it with
// the type asserts on, which turn a sync-without-free mistake around a call
// into a trap rather than a silent misread. The third is the same
// differential at -O0, where the inliner is off and every call in the file
// survives into bytecode. The fourth pins that the functions under test were
// in fact compiled at -O, so the differential cannot degrade into comparing
// the interpreter against itself. The fifth does the same for the -O0
// differential above; the FunctionIDs happen to match the -O ones for every
// function except isOdd, which the -O inliner folds into isEven but -O0
// keeps as a real call. The last one checks that the emitted code really
// took the call path, by counting the calls it made.
//
// Nothing here reaches a global binding from inside a compiled function, but
// that is no longer a compile-time necessity -- GetGlobalObject and the
// property-access opcodes both compile now (globals.js and props.js cover
// them). Every callee here still arrives either as a parameter or through
// the enclosing function's environment, which is what makes the recursive
// functions nested inside factories; that shape is kept because it is what
// isolates the call-arity opcodes under test from property access, not
// because the alternative would fail to compile.
//
// All four differential RUN lines carry -Xjit-crash-on-error, because
// nothing here declines in any of them (measured).
//
// Two paths in the emitter are not covered by any lit test:
//   - Calling a non-callable, which throws: confirmed separately (a
//     try/catch around a call to a non-function value) to compile and match
//     the interpreter now that exceptions have landed.
//   - CallWithNewTargetLong (a register arg count), which comes from spread
//     and apply: confirmed separately (`f(...arr)`, `new C(...arr)`,
//     `f.apply(null, arr)`) to compile and match.
// GetBuiltinClosure is likewise not covered by a lit test: it is reachable
// from plain JS through a direct `eval(...)` call, which landed in
// milestone 5; checked by hand to compile and match. The async/iterator lowerings
// that also produce it remain out of reach -- async through the permanent
// AsyncBreakCheck decline, iterators through opcodes this file does not
// build.

// Direct recursion. fib refers to itself through makeFib's environment, so
// the self-call is a real call in compiled code rather than an inlined one.
function makeFib() {
  function fib(n) {
    if (n < 2)
      return n;
    return fib(n - 1) + fib(n - 2);
  }
  return fib;
}

// Mutual recursion: each function reaches the other through the shared
// environment, so neither can be inlined into the other.
function makeParity() {
  function isEven(n) {
    if (n === 0)
      return true;
    return isOdd(n - 1);
  }
  function isOdd(n) {
    if (n === 0)
      return false;
    return isEven(n - 1);
  }
  return isEven;
}

// Callees of every small arity, called with exactly the parameters they
// declare, so callAll's body is one each of Call1, Call2, Call3 and Call4.
function a0() {
  return 1;
}
function a1(x) {
  return x * 10;
}
function a2(x, y) {
  return x * 100 + y;
}
function a3(x, y, z) {
  return x * 1000 + y * 10 + z;
}
function callAll(f0, f1, f2, f3) {
  return f0() + f1(2) + f2(3, 4) + f3(5, 6, 7);
}

// Five arguments is past the point where the compiler has a dedicated
// opcode, so this one lowers to the generic Call.
function a5(a, b, c, d, e) {
  return a + b + c + d + e;
}
function callBig(f) {
  return f(1, 2, 3, 4, 5);
}

// An empty constructor compiles to LoadConstUndefined + Ret, and `new`
// through a parameter needs no property access: CreateThis does the
// prototype lookup itself, Construct is a call with a new target, and
// SelectObject picks between the constructed object and the returned value.
function C() {}
function mk(ctor) {
  return new ctor();
}

// Higher-order: the callback arrives as a parameter and is called in a loop,
// so the same call site dispatches to compiled code many times.
function applyN(f, n) {
  var acc = 0;
  for (var i = 0; i < n; i = i + 1)
    acc = acc + f(i);
  return acc;
}
function dbl(x) {
  return x + x;
}

// Allocate enough objects to force young-generation collections while a
// compiled call frame is live. Every FR that a call site failed to sync
// before handing control over is visible to the GC as garbage here, which
// is what makes this more than a slow loop.
function churn(iters, ctor) {
  var n = 0;
  for (var i = 0; i < iters; i = i + 1) {
    var o = new ctor();
    if (o === o)
      n = n + 1;
  }
  return n;
}

// Statuses are pinned in the order the JIT compiles them, which is the order
// each function is first entered, interleaved with the program's own output.
var fib = makeFib();
// CHECK: JIT successfully compiled FunctionID 1, 'makeFib'
// CHECK0: JIT successfully compiled FunctionID 1, 'makeFib'
print(fib(15));
// CHECK: JIT successfully compiled FunctionID 15, 'fib'
// CHECK0: JIT successfully compiled FunctionID 15, 'fib'
// CHECK: 610
var isEven = makeParity();
// CHECK: JIT successfully compiled FunctionID 2, 'makeParity'
// CHECK0: JIT successfully compiled FunctionID 2, 'makeParity'
print(isEven(20));
// CHECK: JIT successfully compiled FunctionID 16, 'isEven'
// CHECK0: JIT successfully compiled FunctionID 16, 'isEven'
// isOdd has no status line of its own at -O: the inliner folds it into
// isEven, which is what makes isEven's recursion go through the
// environment twice per level rather than alternating between two
// closures. -O0 does not inline it, so it compiles as FunctionID 17 --
// the one FunctionID in this file that does not match between -O and -O0.
// CHECK0: JIT successfully compiled FunctionID 17, 'isOdd'
// CHECK: true
print(callAll(a0, a1, a2, a3));
// CHECK: JIT successfully compiled FunctionID 7, 'callAll'
// CHECK: JIT successfully compiled FunctionID 3, 'a0'
// CHECK: JIT successfully compiled FunctionID 4, 'a1'
// CHECK: JIT successfully compiled FunctionID 5, 'a2'
// CHECK: JIT successfully compiled FunctionID 6, 'a3'
// CHECK: 5392
print(callBig(a5));
// CHECK: JIT successfully compiled FunctionID 9, 'callBig'
// CHECK: JIT successfully compiled FunctionID 8, 'a5'
// CHECK: 15
print(mk(C) === mk(C));
// CHECK: JIT successfully compiled FunctionID 11, 'mk'
// CHECK: JIT successfully compiled FunctionID 10, 'C'
// CHECK: false
print(mk(C) === mk);
// CHECK: false
print(applyN(dbl, 10));
// CHECK: JIT successfully compiled FunctionID 12, 'applyN'
// CHECK: JIT successfully compiled FunctionID 13, 'dbl'
// CHECK: 90
print(churn(30000, C));
// CHECK: JIT successfully compiled FunctionID 14, 'churn'
// CHECK0: JIT successfully compiled FunctionID 14, 'churn'
// CHECK: 30000

// The counters are printed to stderr at exit. NumCall counts every call the
// emitted code made; it is in the tens of thousands here, and any nonzero
// value proves the call path ran.
// COUNT: JIT counters:
// COUNT: NumCall: {{[1-9][0-9]*}}
