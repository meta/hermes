/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// Bit operations (BitAnd/BitOr/BitXor/LShift/RShift/URshift, BitNot) and the
// int conversions (ToInt32/ToUint32). As in arith.js, the first RUN line is
// the real check -- interpreter and JIT must print the same thing -- and the
// second only pins that the functions under test were compiled, so the
// differential cannot degrade into interpreter-vs-interpreter. The first RUN
// line carries -Xjit-crash-on-error: nothing here declines (measured).
//
// One value per print, and the operands always arrive as parameters: a
// literal `2147483647 | 0` would be folded at compile time and never reach
// the emitter at all.

function and2(a, b) { return a & b; }
function or2(a, b) { return a | b; }
function xor2(a, b) { return a ^ b; }
function shl2(a, b) { return a << b; }
function sar2(a, b) { return a >> b; }
function shr2(a, b) { return a >>> b; }
function not1(a) { return ~a; }
// `x | 0` and `x >>> 0` are the ToInt32 and ToUint32 opcodes, not BitOr and
// URshift; only a non-constant right operand produces the binops above.
function toint(a) { return a | 0; }
function touint(a) { return a >>> 0; }
// The result is added to in double space, which pins that the int32 was
// converted back as a signed value.
function orInc(a, b) { return (a | b) + 1; }
// The result FR aliases an operand: first the left, then the count. The
// count case matters on x86-64, where the count must live in rcx.
function shlSelfLeft(a, b) { a = a << b; return a; }
function shlSelfRight(a, b) { b = a << b; return b; }
// The count is read again after the first shift. This one also drives the
// x86-64 spill path: the count parameter is live in rcx when the first
// shift starts, so vacating rcx for the count temp has to spill it and
// reload it. If that spill were dropped, the count temp would land
// somewhere else and the function would decline -- which the compiled-
// successfully CHECK below would catch.
function shTwice(a, b) { return (a << b) + (a >> b); }
// Both operands are the same FR, so one value feeds both int32 temps and,
// for the shifts, is also the count that must be in rcx.
function shlSelf(a) { return a << a; }
function shrSelf(a) { return a >>> a; }
function andSelf(a) { return a & a; }
// Two bit ops in a row, so the second one's operand comes out of the first.
function chain(a, b, c) { var x = a & b; return x | c; }

// Integer operands.
print(and2(12, 10));
// CHECK: JIT successfully compiled FunctionID 1, 'and2'
// CHECK-NEXT: 8
print(or2(12, 10));
// CHECK: JIT successfully compiled FunctionID 2, 'or2'
// CHECK-NEXT: 14
print(xor2(12, 10));
// CHECK: JIT successfully compiled FunctionID 3, 'xor2'
// CHECK-NEXT: 6
print(and2(-1, 255));
// CHECK-NEXT: 255
print(or2(-16, 15));
// CHECK-NEXT: -1
print(xor2(-1, -1));
// CHECK-NEXT: 0

// Operands that are doubles: exact integers take the same fast path, values
// with a fraction do not and go to the runtime.
print(and2(12.0, 10.0));
// CHECK-NEXT: 8
print(or2(2.5, 1));
// CHECK-NEXT: 3
print(xor2(1, 2.5));
// CHECK-NEXT: 3
print(and2(-2.5, -1));
// CHECK-NEXT: -2

// Every shift with counts 0, 1, 31, 32, 33 and -1. 32 and 33 pin the count
// masking (count & 31), -1 pins that the mask is taken from the low bits of
// the int32 rather than from its sign.
print(shl2(1, 0));
// CHECK: JIT successfully compiled FunctionID 4, 'shl2'
// CHECK-NEXT: 1
print(shl2(1, 1));
// CHECK-NEXT: 2
print(shl2(1, 31));
// CHECK-NEXT: -2147483648
print(shl2(1, 32));
// CHECK-NEXT: 1
print(shl2(1, 33));
// CHECK-NEXT: 2
print(shl2(1, -1));
// CHECK-NEXT: -2147483648
print(sar2(-16, 0));
// CHECK: JIT successfully compiled FunctionID 5, 'sar2'
// CHECK-NEXT: -16
print(sar2(-16, 1));
// CHECK-NEXT: -8
print(sar2(-16, 31));
// CHECK-NEXT: -1
print(sar2(-16, 32));
// CHECK-NEXT: -16
print(sar2(-16, 33));
// CHECK-NEXT: -8
print(sar2(-16, -1));
// CHECK-NEXT: -1
print(shr2(-16, 0));
// CHECK: JIT successfully compiled FunctionID 6, 'shr2'
// CHECK-NEXT: 4294967280
print(shr2(-16, 1));
// CHECK-NEXT: 2147483640
print(shr2(-16, 31));
// CHECK-NEXT: 1
print(shr2(-16, 32));
// CHECK-NEXT: 4294967280
print(shr2(-16, 33));
// CHECK-NEXT: 2147483640
print(shr2(-16, -1));
// CHECK-NEXT: 1

// The unsigned shift is the only op whose result is converted back as an
// unsigned int32; a signed conversion would print a negative number here.
print(shr2(-1, 0));
// CHECK-NEXT: 4294967295
print(shr2(-2147483648, 0));
// CHECK-NEXT: 2147483648
print(touint(-1));
// CHECK: JIT successfully compiled FunctionID 9, 'touint'
// CHECK-NEXT: 4294967295
print(touint(-2147483648));
// CHECK-NEXT: 2147483648
print(touint(-0.0));
// CHECK-NEXT: 0

// BitNot on the corner values.
print(not1(0));
// CHECK: JIT successfully compiled FunctionID 7, 'not1'
// CHECK-NEXT: -1
print(not1(-0));
// CHECK-NEXT: -1
print(not1(NaN));
// CHECK-NEXT: -1
print(not1(2147483648));
// CHECK-NEXT: 2147483647
print(not1(-1));
// CHECK-NEXT: 0

// The signed-conversion cases, plus the int32 boundary itself: 2147483647+1
// must be a double, not a wrapped int.
print(orInc(2147483647, 0));
// CHECK: JIT successfully compiled FunctionID 10, 'orInc'
// CHECK-NEXT: 2147483648
print(sar2(-2147483648, 1));
// CHECK-NEXT: -1073741824
print(toint(2147483647));
// CHECK: JIT successfully compiled FunctionID 8, 'toint'
// CHECK-NEXT: 2147483647
print(toint(-2147483648));
// CHECK-NEXT: -2147483648

// Doubles around the int32 boundary. All of these are exact integers except
// the .5 ones, and all are outside int32 range, so they exercise the
// wrap-around that ToInt32 specifies.
print(toint(2147483648));
// CHECK-NEXT: -2147483648
print(toint(2147483647.5));
// CHECK-NEXT: 2147483647
print(toint(-2147483649));
// CHECK-NEXT: 2147483647
print(toint(4294967296));
// CHECK-NEXT: 0
print(toint(4294967297));
// CHECK-NEXT: 1
print(touint(2147483648));
// CHECK-NEXT: 2147483648
print(touint(-2147483649));
// CHECK-NEXT: 2147483647
print(touint(4294967296));
// CHECK-NEXT: 0
print(or2(2147483648, 0));
// CHECK-NEXT: -2147483648
print(or2(-2147483649, 0));
// CHECK-NEXT: 2147483647
print(shr2(4294967296, 0));
// CHECK-NEXT: 0

// The far boundaries. On x86-64 an out-of-range conversion produces
// INT64_MIN, which is also the legitimate conversion of -2^63 -- so -2^63
// takes the fast path (correctly: its low 32 bits are 0) while +2^63 and
// everything beyond fails the round trip and goes to the runtime.
print(toint(-9223372036854775808));
// CHECK-NEXT: 0
print(toint(9223372036854775808));
// CHECK-NEXT: 0
print(toint(1e300));
// CHECK-NEXT: 0
print(toint(-1e300));
// CHECK-NEXT: 0
print(or2(-9223372036854775808, 0));
// CHECK-NEXT: 0
print(or2(9223372036854775808, 0));
// CHECK-NEXT: 0

// Infinities and NaN: never integers, always the runtime.
print(toint(Infinity));
// CHECK-NEXT: 0
print(toint(-Infinity));
// CHECK-NEXT: 0
print(toint(NaN));
// CHECK-NEXT: 0
print(touint(NaN));
// CHECK-NEXT: 0
print(or2(Infinity, 0));
// CHECK-NEXT: 0
print(and2(NaN, -1));
// CHECK-NEXT: 0

// Non-numbers, on either side. These take the runtime slow call, which is a
// call inside the compiled function, not a reason to decline it.
print(or2("8", 0));
// CHECK-NEXT: 8
print(or2(0, "8"));
// CHECK-NEXT: 8
print(shl2(true, 1));
// CHECK-NEXT: 2
print(shl2(1, true));
// CHECK-NEXT: 2
print(and2(null, -1));
// CHECK-NEXT: 0
print(xor2(undefined, 5));
// CHECK-NEXT: 5
print(not1(true));
// CHECK-NEXT: -2
print(not1("7"));
// CHECK-NEXT: -8
print(toint("9"));
// CHECK-NEXT: 9
print(touint(true));
// CHECK-NEXT: 1

// Result aliasing and reuse.
print(shlSelfLeft(3, 4));
// CHECK: JIT successfully compiled FunctionID 11, 'shlSelfLeft'
// CHECK-NEXT: 48
print(shlSelfRight(3, 4));
// CHECK: JIT successfully compiled FunctionID 12, 'shlSelfRight'
// CHECK-NEXT: 48
print(shTwice(48, 2));
// CHECK: JIT successfully compiled FunctionID 13, 'shTwice'
// CHECK-NEXT: 204
print(shTwice(48, true));
// CHECK-NEXT: 120
print(chain(255, 60, 3));
// CHECK: JIT successfully compiled FunctionID 17, 'chain'
// CHECK-NEXT: 63
print(chain(255.0, 60, 2.5));
// CHECK-NEXT: 62
print(shlSelf(3));
// CHECK: JIT successfully compiled FunctionID 14, 'shlSelf'
// CHECK-NEXT: 24
print(shlSelf(-1));
// CHECK-NEXT: -2147483648
print(shlSelf(2.5));
// CHECK-NEXT: 8
print(shrSelf(-1));
// CHECK: JIT successfully compiled FunctionID 15, 'shrSelf'
// CHECK-NEXT: 1
print(shrSelf(33));
// CHECK-NEXT: 16
print(andSelf(-5));
// CHECK: JIT successfully compiled FunctionID 16, 'andSelf'
// CHECK-NEXT: -5
print(andSelf(NaN));
// CHECK-NEXT: 0
