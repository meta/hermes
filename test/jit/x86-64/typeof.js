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

// typeof: TypeOf (the string-producing form), TypeOfIs (the predicate form)
// and JmpTypeOfIs (the branch form), plus JmpBuiltinIs, which has nothing to
// do with typeof but is the last predicate-shaped opcode in this milestone's
// first task.
//
// The first four RUN lines are the real check -- interpreter and JIT must
// print the same thing, at -O and -O0, with and without the type asserts --
// and they carry -Xjit-crash-on-error because every function here compiles
// in all four. The two -Xdump-jitcode=2 lines pin the compile status.
//
// EVERY typeof RESULT IS DRIVEN. `probe` below is called with a value of
// each of the nine kinds TypeOfIsTypes distinguishes (undefined, object,
// string, symbol, boolean, number, bigint, function, null), and each of the
// predicates is asked about each of them. That is what gives every tag
// helper -- is_undefined, is_symbol, is_string, is_bool, is_null, is_bigint,
// is_double and the CellKind range check behind object/function -- both a
// match and a mismatch.
//
// THE FOUR SHAPES OF TypeOfIs. The emitter picks its code layout from the
// bit set, after considering whether inverting it is cheaper, and all four
// combinations arise from plain JS:
//   typeof x === "string"   -> {String}, 1 bit, not inverted: one setcc.
//   typeof x !== "string"   -> 8 bits, inverted to {String}: one setcc with
//                              the negated condition.
//   typeof x === "object"   -> {Object, Null}, 2 bits, not inverted: the
//                              short-circuiting layout with matchLab.
//   typeof x !== "object"   -> 7 bits, inverted to {Object, Null}: the same
//                              layout, inverted.
// Object and Null together are what makes the two-bit cases interesting:
// `typeof null === "object"` is true in JS, so the Object case must fall
// through to a Null case rather than answer on its own, and the Object case
// itself is the one that loads the CellKind and range-checks it against the
// callable kinds. `isObj`/`notObj` and `branchObj`/`branchNotObj` below are
// the four sites, in both the value and the branch form.
//
// NOT REACHABLE, AND SAID PLAINLY. Both emitters have a branch for an empty
// (or all-bits) type set -- `typeof x === "notatype"`, whose answer does not
// depend on x at all. The compiler folds those to a literal before bytecode
// generation, so no plain-JS program reaches that branch; it was verified by
// dumping the bytecode for exactly such a comparison and finding no TypeOfIs
// at all. It is ported for arm64 parity and stays uncovered.
//
// JmpBuiltinIs. It is emitted by LowerBuiltinCalls, which rewrites an
// `f.call(...)` into a guarded fast path: JmpBuiltinIs tests the callee
// against the builtin Function.prototype.call and takes the direct call when
// the test succeeds.
// That is an optimizer pass, so this opcode exists at -O only, which is why
// `callThrough`'s CHECK0 pin says nothing about it -- the function still
// compiles at -O0, it just contains an ordinary call. Both OUTCOMES of the
// test are driven below: a real Function.prototype.call, which takes the
// guarded fast path, and a `call` property that is some other function
// entirely, which does not.
//
// What is NOT driven is the opcode's `invert` flag. ISel emits
// JmpBuiltinIsNot only when the fast-path block happens to be laid out
// immediately after the test, and neither of the two shapes that produce
// this opcode at all -- `f.call(...)` and `f.apply(this, arguments)` -- came
// out that way in any arrangement tried here; both dump a plain
// JmpBuiltinIs. The inverted branch is ported for arm64 parity and is
// currently uncovered.

function probe(x) {
  return typeof x;
}

// The value form, one line per bit of TypeOfIsTypes plus both polarities of
// the two multi-bit shapes.
function isUndef(x) {
  return typeof x === "undefined";
}
function isObj(x) {
  return typeof x === "object";
}
function notObj(x) {
  return typeof x !== "object";
}
function isStr(x) {
  return typeof x === "string";
}
function notStr(x) {
  return typeof x !== "string";
}
function isSym(x) {
  return typeof x === "symbol";
}
function isBool(x) {
  return typeof x === "boolean";
}
function isNum(x) {
  return typeof x === "number";
}
function isBig(x) {
  return typeof x === "bigint";
}
function isFun(x) {
  return typeof x === "function";
}

// The branch form. Each returns a distinct value per side so that a wrong
// polarity cannot be masked by both sides agreeing.
function branchStr(x) {
  if (typeof x === "string") return 1;
  return 2;
}
function branchNotStr(x) {
  if (typeof x !== "string") return 1;
  return 2;
}
function branchObj(x) {
  if (typeof x === "object") return 1;
  return 2;
}
function branchNotObj(x) {
  if (typeof x !== "object") return 1;
  return 2;
}
// A loop-carried branch, so the jump is taken and not taken repeatedly
// within one compiled body rather than once per call.
function countStrings(a) {
  var n = 0;
  for (var i = 0; i < a.length; ++i) {
    if (typeof a[i] === "string") n += 1;
    else if (typeof a[i] === "number") n += 10;
    else n += 100;
  }
  return n;
}

// JmpBuiltinIs: `f.call(thisVal, arg)`.
function callThrough(f, a) {
  return f.call(null, a);
}

function twice(x) {
  return x * 2;
}

// An object whose `call` is not Function.prototype.call, which is what the
// guard exists to catch.
var faker = {
  call: function (t, v) {
    return "faked:" + v;
  },
};

var values = [
  undefined,
  {a: 1},
  "str",
  Symbol("sym"),
  true,
  3.5,
  7n,
  twice,
  null,
];

function describe(x) {
  return (
    probe(x) +
    "|" +
    (isUndef(x) ? "u" : "-") +
    (isObj(x) ? "o" : "-") +
    (notObj(x) ? "O" : "-") +
    (isStr(x) ? "s" : "-") +
    (notStr(x) ? "S" : "-") +
    (isSym(x) ? "y" : "-") +
    (isBool(x) ? "b" : "-") +
    (isNum(x) ? "n" : "-") +
    (isBig(x) ? "i" : "-") +
    (isFun(x) ? "f" : "-") +
    "|" +
    branchStr(x) +
    branchNotStr(x) +
    branchObj(x) +
    branchNotObj(x)
  );
}

// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 0, 'global'

for (var i = 0; i < values.length; ++i) print(describe(values[i]));
// CHECK: JIT successfully compiled FunctionID 19, 'describe'
// CHECK: JIT successfully compiled FunctionID 1, 'probe'
// CHECK: JIT successfully compiled FunctionID 2, 'isUndef'
// CHECK: JIT successfully compiled FunctionID 3, 'isObj'
// CHECK: JIT successfully compiled FunctionID 4, 'notObj'
// CHECK: JIT successfully compiled FunctionID 5, 'isStr'
// CHECK: JIT successfully compiled FunctionID 6, 'notStr'
// CHECK: JIT successfully compiled FunctionID 7, 'isSym'
// CHECK: JIT successfully compiled FunctionID 8, 'isBool'
// CHECK: JIT successfully compiled FunctionID 9, 'isNum'
// CHECK: JIT successfully compiled FunctionID 10, 'isBig'
// CHECK: JIT successfully compiled FunctionID 11, 'isFun'
// CHECK: JIT successfully compiled FunctionID 12, 'branchStr'
// CHECK: JIT successfully compiled FunctionID 13, 'branchNotStr'
// CHECK: JIT successfully compiled FunctionID 14, 'branchObj'
// CHECK: JIT successfully compiled FunctionID 15, 'branchNotObj'
// CHECK-NEXT: undefined|u-O-S-----|2121
// CHECK-NEXT: object|-o--S-----|2112
// CHECK-NEXT: string|--Os------|1221
// CHECK-NEXT: symbol|--O-Sy----|2121
// CHECK-NEXT: boolean|--O-S-b---|2121
// CHECK-NEXT: number|--O-S--n--|2121
// CHECK-NEXT: bigint|--O-S---i-|2121
// CHECK-NEXT: function|--O-S----f|2121
// CHECK-NEXT: object|-o--S-----|2112
// CHECK0: JIT successfully compiled FunctionID 19, 'describe'
// CHECK0: JIT successfully compiled FunctionID 1, 'probe'
// CHECK0: JIT successfully compiled FunctionID 2, 'isUndef'
// CHECK0: JIT successfully compiled FunctionID 3, 'isObj'
// CHECK0: JIT successfully compiled FunctionID 4, 'notObj'
// CHECK0: JIT successfully compiled FunctionID 5, 'isStr'
// CHECK0: JIT successfully compiled FunctionID 6, 'notStr'
// CHECK0: JIT successfully compiled FunctionID 7, 'isSym'
// CHECK0: JIT successfully compiled FunctionID 8, 'isBool'
// CHECK0: JIT successfully compiled FunctionID 9, 'isNum'
// CHECK0: JIT successfully compiled FunctionID 10, 'isBig'
// CHECK0: JIT successfully compiled FunctionID 11, 'isFun'
// CHECK0: JIT successfully compiled FunctionID 12, 'branchStr'
// CHECK0: JIT successfully compiled FunctionID 13, 'branchNotStr'
// CHECK0: JIT successfully compiled FunctionID 14, 'branchObj'
// CHECK0: JIT successfully compiled FunctionID 15, 'branchNotObj'
// CHECK0-NEXT: undefined|u-O-S-----|2121
// CHECK0-NEXT: object|-o--S-----|2112
// CHECK0-NEXT: string|--Os------|1221
// CHECK0-NEXT: symbol|--O-Sy----|2121
// CHECK0-NEXT: boolean|--O-S-b---|2121
// CHECK0-NEXT: number|--O-S--n--|2121
// CHECK0-NEXT: bigint|--O-S---i-|2121
// CHECK0-NEXT: function|--O-S----f|2121
// CHECK0-NEXT: object|-o--S-----|2112

print(countStrings(["a", 1, {}, "b", 2, null, "c"]));
// CHECK: JIT successfully compiled FunctionID 16, 'countStrings'
// CHECK-NEXT: 223
// CHECK0: JIT successfully compiled FunctionID 16, 'countStrings'
// CHECK0-NEXT: 223

print(callThrough(twice, 21), callThrough.call(null, twice, 5));
// CHECK: JIT successfully compiled FunctionID 17, 'callThrough'
// CHECK: JIT successfully compiled FunctionID 18, 'twice'
// CHECK-NEXT: 42 10
// CHECK0: JIT successfully compiled FunctionID 17, 'callThrough'
// CHECK0: JIT successfully compiled FunctionID 18, 'twice'
// CHECK0-NEXT: 42 10

print(faker.call(null, 9));
// CHECK: JIT successfully compiled FunctionID 20, 'call'
// CHECK-NEXT: faked:9
// CHECK0: JIT successfully compiled FunctionID 20, 'call'
// CHECK0-NEXT: faked:9

print(probe(probe), probe(values), probe(null), probe(undefined));
// CHECK-NEXT: function object object undefined
// CHECK0-NEXT: function object object undefined
