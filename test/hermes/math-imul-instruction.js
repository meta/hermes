/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O -fstatic-builtins %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -fstatic-builtins %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -fno-static-builtins %s | %FileCheck --match-full-lines %s
// RUN: %hermesc -O -fstatic-builtins -target=HBC -dump-bytecode %s | %FileCheck --check-prefix=BC %s

// The full semantic matrix for Math.imul, computed three different ways and
// required to agree.
//
// ACTIVATION. The three execution RUN lines are not repetitions: each one
// reaches a different implementation of the same values.
//
//   -O  -fstatic-builtins     InstSimplify folds the calls whose operands
//                             are literals, so most of the file is computed
//                             by the compiler and never runs at all.
//   -O0 -fstatic-builtins     InstSimplify does not run, so the same calls
//                             reach the Imul instruction in the interpreter.
//   -O  -fno-static-builtins  LowerBuiltinCalls refuses to resolve Math
//                             calls, so nothing is lowered and every line
//                             runs the untouched builtin.
//
// The builtin is the reference the other two must match, and it is the one
// implementation this change does not touch. -fno-static-builtins is spelled
// explicitly rather than omitted, because omitting it would leave a source
// directive or an autodetected default free to turn static builtins back on
// and quietly make that run a duplicate of the first.
//
// The fourth RUN is the anti-vacuity guard: it fails if the calls below ever
// stop being lowered.
//
// BC: Imul

// The reference formula the instruction cannot be replaced by. It agrees
// with imul only while the exact product stays below 2^53; the divergent
// section at the bottom is where it stops.
function ref(a, b) {
  return ((a | 0) * (b | 0)) | 0;
}

function agree(a, b) {
  var got = Math.imul(a, b);
  print("agree", got, got === ref(a, b));
}

// Ordinary values.
print("basic", Math.imul(3, 4), Math.imul(-5, 12), Math.imul(-5, -12));
// CHECK:basic 12 -60 60

// The 2^31 boundaries. Every one of these is a value a naive
// implementation gets wrong: the first is the classic
// "4294967294 is not an int32" trap, and the second is 2^31 itself, which
// reinterpreted as a signed int32 is -2147483648 and NOT zero.
print("wrap", Math.imul(0x7fffffff, 2));
// CHECK-NEXT:wrap -2
print("min", Math.imul(-0x80000000, -1));
// CHECK-NEXT:min -2147483648
print("edges", Math.imul(0x7fffffff, 0x7fffffff), Math.imul(0xffffffff, 5),
      Math.imul(0xffffffff, 0xffffffff), Math.imul(0x80000000, 1),
      Math.imul(0x80000000, 2), Math.imul(0x80000000, 0x80000000));
// CHECK-NEXT:edges 1 -5 1 -2147483648 0 0

// ToInt32 of an out-of-range double keeps only the low 32 bits.
print("modulo", Math.imul(4294967296, 5), Math.imul(4294967297, 5),
      Math.imul(-4294967295, 3), Math.imul(1e21, 7));
// CHECK-NEXT:modulo 0 5 3 375390208

// Non-numbers all convert through ToInt32, which turns NaN and the
// infinities into +0.
print("nan", Math.imul(NaN, 5), Math.imul(5, NaN), Math.imul(NaN, NaN));
// CHECK-NEXT:nan 0 0 0
print("inf", Math.imul(Infinity, 5), Math.imul(-Infinity, 5),
      Math.imul(5, Infinity), Math.imul(Infinity, Infinity));
// CHECK-NEXT:inf 0 0 0 0
print("undef", Math.imul(undefined, 5), Math.imul(null, 5),
      Math.imul(true, 7), Math.imul(false, 7));
// CHECK-NEXT:undef 0 0 7 0

// Sign of zero. ToInt32 produces +0 for -0, so every zero result of imul is
// a POSITIVE zero -- including the cases where the corresponding double
// multiply would have produced -0. Plain equality cannot tell the two
// apart, so this is pinned with Object.is.
print("zerosign", Object.is(Math.imul(-0, 5), 0),
      Object.is(Math.imul(-1, 0), 0), Object.is(Math.imul(-1, -0), 0),
      Object.is(Math.imul(NaN, -0), 0));
// CHECK-NEXT:zerosign true true true true
print("negzero", Object.is(Math.imul(-1, 0), -0),
      Object.is(-1 * 0, -0), 1 / Math.imul(-1, 0));
// CHECK-NEXT:negzero false true Infinity

// Fractions truncate toward zero, they do not round.
print("frac", Math.imul(2.9, 3), Math.imul(-2.9, 3), Math.imul(1.9999, 2),
      Math.imul(-0.5, 100), Math.imul(2.5, 2.5));
// CHECK-NEXT:frac 6 -6 2 0 4

// Strings go through ToNumber first, so hex literals and whitespace work
// and garbage becomes NaN, hence 0.
print("string", Math.imul("5", "3"), Math.imul("0x10", 2),
      Math.imul(" 7 ", 3), Math.imul("abc", 2), Math.imul("", 9),
      Math.imul("2.9", 3));
// CHECK-NEXT:string 15 32 21 0 0 6

// Objects are coerced with valueOf, left operand first. Both conversions
// happen, and they happen in source order.
function tracer(name, v) {
  return {
    valueOf: function () {
      print("valueOf", name);
      return v;
    },
  };
}
print("order", Math.imul(tracer("left", 6), tracer("right", 7)));
// CHECK-NEXT:valueOf left
// CHECK-NEXT:valueOf right
// CHECK-NEXT:order 42

// And when the left conversion throws, the right one must never run.
function leftThrows() {
  return {
    valueOf: function () {
      print("left-throws");
      throw new Error("boom");
    },
  };
}
try {
  Math.imul(leftThrows(), tracer("right-never-reached", 1));
} catch (e) {
  print("caught", e.message);
}
// CHECK-NEXT:left-throws
// CHECK-NEXT:caught boom

// BigInt and Symbol have no ToNumber, so both operands throw a TypeError.
// Math.imul is NOT a bitwise operator: `1n & 2n` computes in BigInt, while
// this must throw.
function tryImul(a, b) {
  try {
    return String(Math.imul(a, b));
  } catch (e) {
    return e.name;
  }
}
print("bigint", tryImul(1n, 2n), tryImul(1n, 2), tryImul(2, 2n));
// CHECK-NEXT:bigint TypeError TypeError TypeError
print("symbol", tryImul(Symbol("s"), 2), tryImul(2, Symbol("s")));
// CHECK-NEXT:symbol TypeError TypeError

// The same throw with the RESULT UNUSED. At -O nothing may delete this
// instruction: its operands are not statically numbers, so its effect
// summary keeps it alive. If DCE ever removes it, "unreached" prints.
function discardedThrow() {
  Math.imul(3n, 4n);
  return "unreached";
}
try {
  print("dce", discardedThrow());
} catch (e) {
  print("dce", e.name);
}
// CHECK-NEXT:dce TypeError

function discardedSymbolThrow(s) {
  Math.imul(s, 4);
  return "unreached";
}
try {
  print("dce-sym", discardedSymbolThrow(Symbol("s")));
} catch (e) {
  print("dce-sym", e.name);
}
// CHECK-NEXT:dce-sym TypeError

// A discarded conversion side effect must survive too, for the same reason.
var effects = 0;
function discardedEffect() {
  Math.imul({ valueOf: function () { ++effects; return 3; } }, 4);
  return effects;
}
print("dce-effect", discardedEffect());
// CHECK-NEXT:dce-effect 1

// Every arity becomes the instruction. A missing argument is undefined and
// ToInt32 of it is 0, which the literal 0 the lowering pads with reproduces
// exactly; arguments past the second are ignored. These are the values the
// builtin produces, and computing them with the instruction must not change
// any of them.
print("arity", Math.imul(), Math.imul(7), Math.imul(0x7fffffff),
      Math.imul(3, 4, 5), Math.imul(0x7fffffff, 2, 99));
// CHECK-NEXT:arity 0 0 0 12 -2

// Padding supplies a value, not a shortcut. A one-argument call still
// converts the argument it was given, so valueOf runs and a conversion that
// fails still throws. Folding Math.imul(x) to a constant 0 would keep every
// one-argument entry on the arity line above correct -- those are all 0
// anyway -- and would fail each of the three checks below.
function tryImul1(a) {
  try {
    return String(Math.imul(a));
  } catch (e) {
    return e.name;
  }
}
print("one-arg-effect", Math.imul(tracer("lonely", 6)));
// CHECK-NEXT:valueOf lonely
// CHECK-NEXT:one-arg-effect 0
try {
  Math.imul(leftThrows());
} catch (e) {
  print("one-arg-throw", e.message);
}
// CHECK-NEXT:left-throws
// CHECK-NEXT:one-arg-throw boom
print("one-arg-symbol", tryImul1(Symbol("s")), tryImul1(3n));
// CHECK-NEXT:one-arg-symbol TypeError TypeError

// The same conversion with the result discarded, matching the two-argument
// DCE cases above: the instruction's operand is not statically a number, so
// nothing may delete it.
var lonelyEffects = 0;
function discardedOneArg() {
  Math.imul({ valueOf: function () { ++lonelyEffects; return 3; } });
  return lonelyEffects;
}
print("dce-one-arg", discardedOneArg());
// CHECK-NEXT:dce-one-arg 1

// Dropping the extra arguments drops operands, not evaluation: those
// expressions ran at the call site before Math.imul was ever entered.
var extras = 0;
function bump() {
  ++extras;
  return 99;
}
print("extra-args", Math.imul(3, 4, bump(), bump()), extras);
// CHECK-NEXT:extra-args 12 2

// A zero-argument call has nothing to convert, so it is plain +0 and cannot
// throw.
print("no-arg", Math.imul(), Object.is(Math.imul(), 0));
// CHECK-NEXT:no-arg 0 true

// A shadowed Math is not the builtin at all and must keep calling the
// local function.
function shadowed() {
  var Math = { imul: function (a, b) { return "shadow:" + a + ":" + b; } };
  return Math.imul(3, 4);
}
print("shadow", shadowed());
// CHECK-NEXT:shadow shadow:3:4

// Agreement with ((a|0)*(b|0))|0 wherever the exact product stays below
// 2^53. The outer |0 is what supplies the wrap -- without it the reference
// would say 4294967294 for the first pair below.
agree(0x7fffffff, 2);
// CHECK-NEXT:agree -2 true
agree(-0x80000000, -1);
// CHECK-NEXT:agree -2147483648 true
agree(65536, 65536);
// CHECK-NEXT:agree 0 true
agree(65535, 65537);
// CHECK-NEXT:agree -1 true
agree(-3, 1000000);
// CHECK-NEXT:agree -3000000 true
agree(1000003, 1000033);
// CHECK-NEXT:agree -691379869 true
agree(-16777216, 256);
// CHECK-NEXT:agree 0 true

// And where it does not. Each pair's exact product exceeds 2^53, so the
// double multiply the reference performs has already dropped the low bits
// imul is defined by; no sequence of existing arithmetic operators can
// compute these.
function diverge(a, b) {
  print("diverge", Math.imul(a, b), ref(a, b));
}
diverge(0x7fffffff, 0x7fffffff);
// CHECK-NEXT:diverge 1 0
diverge(0x40000001, 0x40000001);
// CHECK-NEXT:diverge -2147483647 -2147483648
diverge(123456789, 987654321);
// CHECK-NEXT:diverge -67153019 -67153024
diverge(-2147483647, 2147483647);
// CHECK-NEXT:diverge -1 0
diverge(1234567890, -987654321);
// CHECK-NEXT:diverge 671530190 671530240
