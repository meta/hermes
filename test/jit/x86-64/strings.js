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

// Strings and the remaining scalar stragglers: LoadConstString,
// AddEmptyString, AddS, LoadConstBigInt, CreateRegExp, ToPropertyKey,
// DirectEval and Debugger.
//
// The first four RUN lines are the real check -- interpreter and JIT must
// print the same thing, at -O and -O0, with and without the type asserts.
// They carry -Xjit-crash-on-error because EVERY function in this file
// compiles in every one of those four configurations (the CHECK/CHECK0
// lines below enumerate them), so a decline here is a regression and must
// abort rather than silently fall back to the interpreter. The two
// -Xdump-jitcode=2 lines pin the compile status itself.
//
// LoadConstString is what unblocked `global` for the whole suite: a single
// string literal in top-level code used to make the top-level function
// decline, which is why objects.js, props.js, hvmodes.js and arrays.js were
// all written to keep their printing in the interpreter. Their headers have
// been rewritten now that this is no longer true.
//
// ADDS HAS NO FAST PATH, AND THAT IS NOT AN OVERSIGHT. AddS is emitted only
// where the compiler has already proved both operands are strings, so there
// is no type test left to inline; the concatenation itself -- allocating,
// copying, or building a rope -- is entirely the runtime's. arm64 emits a
// bare call here too. AddEmptyString is the one string opcode in this file
// with an inlined type check (input already a string => pass it through),
// and `asStr` below drives both sides of it.
//
// The compiler's shape for AddS is `("" + a) + ("" + b)`: each half lowers
// to AddEmptyString, whose result is typed string, so the outer `+` lowers
// to AddS. A plain `a + b` on two unknown parameters is the generic Add,
// which arith.js already covers; `plusMixed` below keeps one such site here
// so the string slow path of the generic Add runs alongside the AddS one.
//
// That lowering is InstSimplify's, so AddS and AddEmptyString exist only at
// -O: at -O0 every `+` in this file is the generic Add. The CHECK0 lines are
// nevertheless identical to the CHECK ones, because the same functions
// compile and print the same values either way -- what differs is which
// emitter runs, and only the -O runs exercise these two. LoadConstString,
// LoadConstBigInt, CreateRegExp, ToPropertyKey, DirectEval and Debugger are
// covered at both levels.
//
// UNREACHABLE IS COVERED, BY `mk` BELOW. PeepholeLowering's ThrowTypeError
// lowering (lowerThrowTypeError) replaces a ThrowTypeError with a call to
// the throwTypeError builtin followed by an Unreachable, because the call
// does not return. `new Ctor()` on a class whose constructor is statically
// known reaches that lowering, so `mk` contains one Unreachable, compiles,
// and emits `call _sh_unreachable`.
//
// WHAT THE PIN ON `mk` CAN AND CANNOT CATCH. Unreachable is, by
// construction, never executed: no input to this program can reach that
// instruction. So no differential RUN line can distinguish a correct
// unreachable() from one that emits nothing at all -- the printed output is
// identical either way, and it was checked that emptying the emitter's body
// leaves every RUN line in this suite passing. What the compile-status pin
// below does guard is that the opcode still COMPILES: if unreachable() ever
// went back to declining, `mk` would drop out of the -O CHECK set and this
// file would fail. That is a real regression barrier and it is the only one
// available for this opcode. Anything stronger would need the emitted text
// itself to be pinned, which this suite does not do for any opcode.
//
// Unreachable is -O only: at -O0 the lowering does not run and `mk` has no
// Unreachable in it at all. `mk` is pinned in both modes anyway, because
// this file's CHECK and CHECK0 sets enumerate every function that compiles,
// but it is only the -O pin that says anything about this opcode. (At -O0
// the constructor is not inlined, so `Ctor` compiles as a function of its
// own and is pinned there too; at -O it is inlined into `mk` and never
// compiled separately.)
//
// NOT covered here, and why:
//  - The OTHER producer of Unreachable, the async/generator lowering. The
//    tail of a lowered async body carries one, but that body -- the inner
//    `Function<?anon_0_af>`, not the outer NCFunction wrapper of the same
//    name -- still declines on catchInst and throwInst, so the emitter
//    never runs for it. (An earlier version of this note claimed the async
//    case as verified coverage. It was wrong: the function that compiled
//    was the outer wrapper, and `_sh_unreachable` appeared nowhere in the
//    emitted code.) The exceptions milestone opens that path.
//  - ProfilePoint. It emits nothing at all unless HERMESVM_PROFILER_BB is
//    defined, and no build in this project's matrix defines it. The
//    emitter is arm64's, verbatim, including that #ifdef.

// A short const string and a long one. The short one fits the small-string
// representation, the long one does not; both take the same emitted path (a
// load out of the identifier table's lookup vector plus a string tag), which
// is exactly why both are here -- a wrong tag or a wrong entry stride would
// show up on one and not necessarily the other.
function shortStr() {
  return "hi";
}

function longStr() {
  return "the quick brown fox jumps over the lazy dog, twice over";
}

// A non-ASCII literal, which the string table stores as UTF-16 rather than
// ASCII. The emitted code does not care, and that is the point.
function unicodeStr() {
  return "café 中文 —";
}

// AddEmptyString. Called below with a string (the inlined fast path: the
// value is passed straight through) and with a number, a bool, null,
// undefined and an object (the slow call).
function asStr(x) {
  return "" + x;
}

// AddS, via two AddEmptyStrings. Deliberately asymmetric: swapping the two
// operands of the runtime call would still produce a string of the same
// length, and only an asymmetric case can tell the difference.
function joinAny(a, b) {
  return ("" + a) + ("" + b);
}

// The generic Add with a string operand, i.e. the slow path that AddS
// exists to avoid.
function plusMixed(a, b) {
  return a + b;
}

// String constants used as property keys from compiled code: a by-val read,
// a delete and an `in`. props.js could not cover these while LoadConstString
// declined, and said so.
function byValConst(o) {
  return o["alpha"];
}

function delConst(o) {
  return delete o["alpha"];
}

function hasConst(o) {
  return "alpha" in o;
}

// LoadConstBigInt.
function bigConst() {
  return 123456789012345678901234567890n;
}

// CreateRegExp. The literal is compiled once per site and cached in the
// RuntimeModule, so the second call takes the cached path.
function reTest(s) {
  return /a(b+)c/.test(s);
}

// ToPropertyKey: a computed key whose value is a function, which is the one
// shape in plain JS that needs the key coerced before the function's name is
// set from it.
function computedKey(k) {
  return {[k]: function () {
    return 7;
  }};
}

// DirectEval.
function evalIt(s) {
  return eval(s);
}

// Debugger. It emits nothing unless the BRK dump flag is on, so this
// function exists to prove that emitting nothing is what happens -- that the
// opcode neither declines nor corrupts the surrounding code.
function dbg(x) {
  debugger;
  return x + 1;
}

// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 0, 'global'

print(shortStr(), longStr());
// CHECK: JIT successfully compiled FunctionID 1, 'shortStr'
// CHECK: JIT successfully compiled FunctionID 2, 'longStr'
// CHECK-NEXT: hi the quick brown fox jumps over the lazy dog, twice over
// CHECK0: JIT successfully compiled FunctionID 1, 'shortStr'
// CHECK0: JIT successfully compiled FunctionID 2, 'longStr'
// CHECK0-NEXT: hi the quick brown fox jumps over the lazy dog, twice over

print(unicodeStr());
// CHECK: JIT successfully compiled FunctionID 3, 'unicodeStr'
// CHECK-NEXT: café 中文 —
// CHECK0: JIT successfully compiled FunctionID 3, 'unicodeStr'
// CHECK0-NEXT: café 中文 —

// The fast path (already a string) and every slow-path input.
print(asStr("s"), asStr(42), asStr(-0.5), asStr(true), asStr(null),
      asStr(undefined), asStr([1, 2]));
// CHECK: JIT successfully compiled FunctionID 4, 'asStr'
// CHECK-NEXT: s 42 -0.5 true null undefined 1,2
// CHECK0: JIT successfully compiled FunctionID 4, 'asStr'
// CHECK0-NEXT: s 42 -0.5 true null undefined 1,2

print(joinAny(1, 2), joinAny("a", "b"), joinAny("", "x"), joinAny("x", ""));
// CHECK: JIT successfully compiled FunctionID 5, 'joinAny'
// CHECK-NEXT: 12 ab x x
// CHECK0: JIT successfully compiled FunctionID 5, 'joinAny'
// CHECK0-NEXT: 12 ab x x

print(plusMixed("n=", 5), plusMixed(5, "=n"), plusMixed(1, 2));
// CHECK: JIT successfully compiled FunctionID 6, 'plusMixed'
// CHECK-NEXT: n=5 5=n 3
// CHECK0: JIT successfully compiled FunctionID 6, 'plusMixed'
// CHECK0-NEXT: n=5 5=n 3

var obj = {alpha: 11, beta: 22};
print(byValConst(obj), hasConst(obj), delConst(obj), hasConst(obj),
      byValConst(obj));
// CHECK: JIT successfully compiled FunctionID 7, 'byValConst'
// CHECK: JIT successfully compiled FunctionID 9, 'hasConst'
// CHECK: JIT successfully compiled FunctionID 8, 'delConst'
// CHECK-NEXT: 11 true true false undefined
// CHECK0: JIT successfully compiled FunctionID 7, 'byValConst'
// CHECK0: JIT successfully compiled FunctionID 9, 'hasConst'
// CHECK0: JIT successfully compiled FunctionID 8, 'delConst'
// CHECK0-NEXT: 11 true true false undefined

print(bigConst(), bigConst() + 1n);
// CHECK: JIT successfully compiled FunctionID 10, 'bigConst'
// CHECK-NEXT: 123456789012345678901234567890 123456789012345678901234567891
// CHECK0: JIT successfully compiled FunctionID 10, 'bigConst'
// CHECK0-NEXT: 123456789012345678901234567890 123456789012345678901234567891

print(reTest("xabbbcy"), reTest("ac"), reTest("abc"));
// CHECK: JIT successfully compiled FunctionID 11, 'reTest'
// CHECK-NEXT: true false true
// CHECK0: JIT successfully compiled FunctionID 11, 'reTest'
// CHECK0-NEXT: true false true

var ck = computedKey("dyn");
print(ck.dyn(), ck.dyn.name, computedKey(7)[7]());
// The inner function is the value of the computed property, so it compiles
// on its own as soon as it is called.
// CHECK: JIT successfully compiled FunctionID 12, 'computedKey'
// CHECK: JIT successfully compiled FunctionID 18, ''
// CHECK-NEXT: 7 dyn 7
// CHECK0: JIT successfully compiled FunctionID 12, 'computedKey'
// CHECK0: JIT successfully compiled FunctionID 18, ''
// CHECK0-NEXT: 7 dyn 7

// The eval'd programs are compiled too -- each is its own module whose top
// level is FunctionID 0, named 'eval'.
print(evalIt("1 + 1"), evalIt("'e' + 'v'"));
// CHECK: JIT successfully compiled FunctionID 13, 'evalIt'
// CHECK: JIT successfully compiled FunctionID 0, 'eval'
// CHECK: JIT successfully compiled FunctionID 0, 'eval'
// CHECK-NEXT: 2 ev
// CHECK0: JIT successfully compiled FunctionID 13, 'evalIt'
// CHECK0: JIT successfully compiled FunctionID 0, 'eval'
// CHECK0: JIT successfully compiled FunctionID 0, 'eval'
// CHECK0-NEXT: 2 ev

print(dbg(41));
// CHECK: JIT successfully compiled FunctionID 14, 'dbg'
// CHECK-NEXT: 42
// CHECK0: JIT successfully compiled FunctionID 14, 'dbg'
// CHECK0-NEXT: 42

// A loop that keeps allocating strings, so the concatenations above run
// under young-generation collections rather than only once.
function churn(n) {
  var acc = "";
  var total = 0;
  for (var i = 0; i < n; ++i) {
    acc = ("" + i) + ("" + (n - i));
    total += acc.length;
  }
  return total;
}
print(churn(2000));
// CHECK: JIT successfully compiled FunctionID 15, 'churn'
// CHECK-NEXT: 13783
// CHECK0: JIT successfully compiled FunctionID 15, 'churn'
// CHECK0-NEXT: 13783

// Unreachable. `new Ctor()` on a statically known class constructor makes
// PeepholeLowering emit the throwTypeError builtin call for the
// called-without-new path, followed by an Unreachable because that call does
// not return. `mk` is therefore the file's only Unreachable site; see the
// header for exactly what its pin guards.
class Ctor {
  constructor() {
    this.x = 1;
  }
}
function mk() {
  return new Ctor();
}
print(mk().x);
// CHECK: JIT successfully compiled FunctionID 16, 'mk'
// CHECK-NEXT: 1
// CHECK0: JIT successfully compiled FunctionID 16, 'mk'
// CHECK0: JIT successfully compiled FunctionID 17, 'Ctor'
// CHECK0-NEXT: 1
