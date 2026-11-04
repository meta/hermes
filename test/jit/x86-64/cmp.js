/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// Comparisons: Less/LessEq/Greater/GreaterEq/Eq/Neq, StrictEq/StrictNeq and
// Not. The first RUN line is the real check -- the same program run by the
// interpreter and by the JIT must print the same thing, and carries
// -Xjit-crash-on-error since nothing here declines (measured). The second
// RUN line only makes sure the functions under test were in fact compiled,
// so the differential cannot degrade into comparing the interpreter against
// itself.
//
// Straight-line bodies only: these produce comparison *values*. Conditional
// branches on comparisons are exercised by loops.js.
//
// The `n` suffixed functions coerce their operands with unary plus first, so
// the JIT knows both are numbers and emits the fast path with no slow-path
// call. That path is the one where a NaN operand is still possible (the JS
// NaN is a number) and nothing has routed it away, which on x86 matters for
// the equal family: `vucomisd` reports unordered by setting ZF, the same
// flag `sete`/`setne` read.
//
// The `b` suffixed functions compare values the JIT knows are booleans,
// which selects StrictEq's raw-bit tier.
function lt(a, b) { return a < b; }
function le(a, b) { return a <= b; }
function gt(a, b) { return a > b; }
function ge(a, b) { return a >= b; }
function eq(a, b) { return a == b; }
function ne(a, b) { return a != b; }
function seq(a, b) { return a === b; }
function sne(a, b) { return a !== b; }
function ltn(a, b) { var x = +a, y = +b; return x < y; }
function len(a, b) { var x = +a, y = +b; return x <= y; }
function gtn(a, b) { var x = +a, y = +b; return x > y; }
function gen(a, b) { var x = +a, y = +b; return x >= y; }
function eqn(a, b) { var x = +a, y = +b; return x == y; }
function nen(a, b) { var x = +a, y = +b; return x != y; }
function seqn(a, b) { var x = +a, y = +b; return x === y; }
function snen(a, b) { var x = +a, y = +b; return x !== y; }
function seqb(a, b) { return (a < b) === (a > b); }
function sneb(a, b) { return (a < b) !== (a > b); }
function not1(a) { return !a; }
function notn(a) { var x = +a; return !x; }
function notb(a, b) { return !(a < b); }

print(lt(1, 2), lt(2, 1), lt(1, 1));
// CHECK: JIT successfully compiled FunctionID 1, 'lt'
// CHECK-NEXT: true false false
print(le(1, 2), le(2, 1), le(1, 1));
// CHECK: JIT successfully compiled FunctionID 2, 'le'
// CHECK-NEXT: true false true
print(gt(1, 2), gt(2, 1), gt(1, 1));
// CHECK: JIT successfully compiled FunctionID 3, 'gt'
// CHECK-NEXT: false true false
print(ge(1, 2), ge(2, 1), ge(1, 1));
// CHECK: JIT successfully compiled FunctionID 4, 'ge'
// CHECK-NEXT: false true true
print(eq(1, 2), eq(2, 1), eq(1, 1));
// CHECK: JIT successfully compiled FunctionID 5, 'eq'
// CHECK-NEXT: false false true
print(ne(1, 2), ne(2, 1), ne(1, 1));
// CHECK: JIT successfully compiled FunctionID 6, 'ne'
// CHECK-NEXT: true true false
print(seq(1, 2), seq(1, 1));
// CHECK: JIT successfully compiled FunctionID 7, 'seq'
// CHECK-NEXT: false true
print(sne(1, 2), sne(1, 1));
// CHECK: JIT successfully compiled FunctionID 8, 'sne'
// CHECK-NEXT: true false

print(lt(NaN, 1), lt(1, NaN), lt(NaN, NaN));
// CHECK-NEXT: false false false
print(le(NaN, 1), le(1, NaN), le(NaN, NaN));
// CHECK-NEXT: false false false
print(gt(NaN, 1), gt(1, NaN), gt(NaN, NaN));
// CHECK-NEXT: false false false
print(ge(NaN, 1), ge(1, NaN), ge(NaN, NaN));
// CHECK-NEXT: false false false
print(eq(NaN, 1), eq(1, NaN), eq(NaN, NaN));
// CHECK-NEXT: false false false
print(ne(NaN, 1), ne(1, NaN), ne(NaN, NaN));
// CHECK-NEXT: true true true
print(seq(NaN, 1), seq(1, NaN), seq(NaN, NaN));
// CHECK-NEXT: false false false
print(sne(NaN, 1), sne(1, NaN), sne(NaN, NaN));
// CHECK-NEXT: true true true

print(lt(0, -0), le(0, -0), gt(0, -0), ge(0, -0));
// CHECK-NEXT: false true false true
print(eq(0, -0), ne(0, -0), seq(0, -0), sne(0, -0));
// CHECK-NEXT: true false true false

print(lt(1, Infinity), lt(Infinity, 1), lt(Infinity, Infinity));
// CHECK-NEXT: true false false
print(le(Infinity, Infinity), ge(Infinity, Infinity), gt(Infinity, Infinity));
// CHECK-NEXT: true true false
print(lt(-Infinity, Infinity), gt(-Infinity, Infinity));
// CHECK-NEXT: true false
print(eq(Infinity, Infinity), seq(Infinity, -Infinity));
// CHECK-NEXT: true false

print(ltn(1, 2), ltn(2, 1), ltn(1, 1));
// CHECK: JIT successfully compiled FunctionID 9, 'ltn'
// CHECK-NEXT: true false false
print(len(1, 2), len(2, 1), len(1, 1));
// CHECK: JIT successfully compiled FunctionID 10, 'len'
// CHECK-NEXT: true false true
print(gtn(1, 2), gtn(2, 1), gtn(1, 1));
// CHECK: JIT successfully compiled FunctionID 11, 'gtn'
// CHECK-NEXT: false true false
print(gen(1, 2), gen(2, 1), gen(1, 1));
// CHECK: JIT successfully compiled FunctionID 12, 'gen'
// CHECK-NEXT: false true true
print(eqn(1, 2), eqn(1, 1), nen(1, 2), nen(1, 1));
// CHECK: JIT successfully compiled FunctionID 13, 'eqn'
// CHECK: JIT successfully compiled FunctionID 14, 'nen'
// CHECK-NEXT: false true true false
print(seqn(1, 2), seqn(1, 1), snen(1, 2), snen(1, 1));
// CHECK: JIT successfully compiled FunctionID 15, 'seqn'
// CHECK: JIT successfully compiled FunctionID 16, 'snen'
// CHECK-NEXT: false true true false

print(ltn(NaN, 1), ltn(1, NaN), ltn(NaN, NaN));
// CHECK-NEXT: false false false
print(len(NaN, 1), len(1, NaN), len(NaN, NaN));
// CHECK-NEXT: false false false
print(gtn(NaN, 1), gtn(1, NaN), gtn(NaN, NaN));
// CHECK-NEXT: false false false
print(gen(NaN, 1), gen(1, NaN), gen(NaN, NaN));
// CHECK-NEXT: false false false
print(eqn(NaN, 1), eqn(1, NaN), eqn(NaN, NaN));
// CHECK-NEXT: false false false
print(nen(NaN, 1), nen(1, NaN), nen(NaN, NaN));
// CHECK-NEXT: true true true
print(seqn(NaN, 1), seqn(NaN, NaN), snen(NaN, 1), snen(NaN, NaN));
// CHECK-NEXT: false false true true
print(ltn(0, -0), len(0, -0), eqn(0, -0), seqn(0, -0));
// CHECK-NEXT: false true true true

print(eq(1, "1"), ne(1, "1"), eq(null, undefined), ne(null, undefined));
// CHECK-NEXT: true false true false
print(eq(null, 0), eq(undefined, 0), eq(true, 1), eq("", 0));
// CHECK-NEXT: false false true true
print(lt("a", "b"), lt("b", "a"), le("a", "a"), gt("b", "a"));
// CHECK-NEXT: true false true true
print(lt(true, 2), ge(true, 1), gt(null, -1), le(undefined, 1));
// CHECK-NEXT: true true true false

print(seq("abc", "abc"), seq("abc", "abd"), seq("abc", "ab"));
// CHECK-NEXT: true false false
print(sne("abc", "abc"), sne("abc", "ab"));
// CHECK-NEXT: false true
print(seq(null, null), seq(null, undefined), seq(true, true), seq(true, 1));
// CHECK-NEXT: true false true false
print(seq("1", 1), sne("1", 1));
// CHECK-NEXT: false true

print(seqb(1, 2), seqb(1, 1), sneb(1, 2), sneb(1, 1));
// CHECK: JIT successfully compiled FunctionID 17, 'seqb'
// CHECK: JIT successfully compiled FunctionID 18, 'sneb'
// CHECK-NEXT: false true true false

print(not1(true), not1(false), not1(0), not1(1), not1(NaN));
// CHECK: JIT successfully compiled FunctionID 19, 'not1'
// CHECK-NEXT: false true true false true
print(not1(""), not1("x"), not1(null), not1(undefined));
// CHECK-NEXT: true false true true
print(notn(0), notn(1), notn(NaN), notn(-0));
// CHECK: JIT successfully compiled FunctionID 20, 'notn'
// CHECK-NEXT: true false true true
print(notb(1, 2), notb(2, 1), notb(NaN, 1));
// CHECK: JIT successfully compiled FunctionID 21, 'notb'
// CHECK-NEXT: false true true
