/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int && %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline %s > %t.int && %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -fno-inline -O0 %s > %t.int0 && %hermes -fno-inline -O0 -Xjit=force -Xjit-crash-on-error %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -fno-inline -O0 %s > %t.int0 && %hermes -fno-inline -O0 -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit3 && diff %t.int0 %t.jit3
// RUN: %hermes -fno-inline -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// REQUIRES: jit

// UIntSwitchImm and StringSwitchImm.
//
// The four differential RUN lines are the real check -- interpreter and JIT
// must print the same thing at -O and -O0, with and without type asserts --
// and they carry -Xjit-crash-on-error because every function here compiles in
// all four configurations. The two -Xdump-jitcode=2 lines pin the compile
// status so the differential cannot decay into comparing the interpreter with
// itself.
//
// -fno-inline keeps each switch in its own compiled frame; without it the
// optimizer would fold most of the calls below into their constant results
// and no switch would execute at all.
//
// WHAT THE JUMP TABLE HAS TO GET RIGHT. uintSwitchImm emits a table of
// 32-bit deltas, relative to the table's own address, immediately after the
// indirect jmp that consumes it, and reaches an entry with
//   target = table + (int32_t)table[value - minVal]
// So four separate things have to line up, and each has a case below:
//   - the SCALE. The deltas are 4 bytes and the index is scaled by 4 in the
//     memory operand. Getting that wrong reads the middle of a neighbouring
//     entry and jumps to garbage; `dense` below has 18 consecutive cases, so
//     any scale error mis-dispatches almost all of them.
//   - the BIAS. minVal is subtracted before indexing. `offset` starts at 100
//     and `big` starts at 0xdeadbe01, so a missing or wrong subtraction is
//     an out-of-table read rather than a wrong-but-plausible answer.
//   - the SIGN of the delta. A case whose basic block precedes the table --
//     which is most of them, since the table sits at the switch itself --
//     has a negative delta, so the load has to sign-extend (movsxd). A
//     zero-extending load would jump megabytes forward.
//   - the RANGE CHECK, including its unsigned-ness. Everything outside
//     [minVal, maxVal] must reach the default label without touching the
//     table. `big` is the interesting one: its bounds exceed INT32_MAX, and
//     the emitter encodes them as int32_t bit patterns, which is correct
//     only because both comparisons are unsigned.
//
// NaN IS THE x86-SPECIFIC HAZARD. The switch operand is checked with
// emit_double_is_uint32(), which round-trips the double through a 64-bit
// integer conversion and compares. arm64's fcmp reports an unordered compare
// as NE, so one b.ne rejects both "not an integer" and "NaN". x86's vucomisd
// reports unordered as EQUAL, so a NaN would pass the jne and index the table
// with vcvttsd2si's "integer indefinite" result, whose low 32 bits are 0 --
// i.e. `switch (NaN)` would run the case for minVal. uintSwitchImm therefore
// branches on BOTH jne and jp. `nanOf` below feeds a runtime-computed NaN
// into every one of the four switches; without the jp, dense(NaN) prints
// "c0" instead of "def" and this test fails.
//
// -0 IS NOT A HAZARD, and is here to pin that. -0 converts to the integer 0
// and back to +0.0, which compares equal to -0.0, so it is accepted as the
// index 0 -- which is what the language wants, since -0 === 0.

function say(x) {
  print(x);
}

// Dense from zero: 18 consecutive cases, minVal == 0, so no bias is
// subtracted and the table is indexed by the value itself.
function dense(x) {
  switch (x) {
    case 0: return "c0";
    case 1: return "c1";
    case 2: return "c2";
    case 3: return "c3";
    case 4: return "c4";
    case 5: return "c5";
    case 6: return "c6";
    case 7: return "c7";
    case 8: return "c8";
    case 9: return "c9";
    case 10: return "c10";
    case 11: return "c11";
    case 12: return "c12";
    case 13: return "c13";
    case 14: return "c14";
    case 15: return "c15";
    case 16: return "c16";
    case 17: return "c17";
    default: return "def";
  }
}

// Dense from 100: the same shape with a non-zero minVal, so the bias
// subtraction is live.
function offset(x) {
  switch (x) {
    case 100: return "o100";
    case 101: return "o101";
    case 102: return "o102";
    case 103: return "o103";
    case 104: return "o104";
    case 105: return "o105";
    case 106: return "o106";
    case 107: return "o107";
    case 108: return "o108";
    case 109: return "o109";
    case 110: return "o110";
    case 111: return "o111";
    case 112: return "o112";
    case 113: return "o113";
    case 114: return "o114";
    case 115: return "o115";
    case 116: return "o116";
    case 117: return "o117";
    default: return "odef";
  }
}

// Dense WITH HOLES: 40..59 minus 44 and 52. The two holes are still table
// entries -- they hold the delta to the default block -- so they are the
// only in-range values that leave through the table rather than through the
// range check.
function holes(x) {
  switch (x) {
    case 40: return "h40";
    case 41: return "h41";
    case 42: return "h42";
    case 43: return "h43";
    case 45: return "h45";
    case 46: return "h46";
    case 47: return "h47";
    case 48: return "h48";
    case 49: return "h49";
    case 50: return "h50";
    case 51: return "h51";
    case 53: return "h53";
    case 54: return "h54";
    case 55: return "h55";
    case 56: return "h56";
    case 57: return "h57";
    case 58: return "h58";
    case 59: return "h59";
    default: return "hdef";
  }
}

// Bounds above INT32_MAX. The emitter compares against the 32-bit bit
// pattern, which is only correct because jb/ja are unsigned; a signed
// comparison would treat every one of these as negative and send every
// value to the default.
function big(x) {
  switch (x) {
    case 0xdeadbe01: return "b1";
    case 0xdeadbe02: return "b2";
    case 0xdeadbe03: return "b3";
    case 0xdeadbe04: return "b4";
    case 0xdeadbe05: return "b5";
    case 0xdeadbe06: return "b6";
    case 0xdeadbe07: return "b7";
    case 0xdeadbe08: return "b8";
    case 0xdeadbe09: return "b9";
    case 0xdeadbe0a: return "b10";
    case 0xdeadbe0b: return "b11";
    case 0xdeadbe0c: return "b12";
    case 0xdeadbe0d: return "b13";
    case 0xdeadbe0e: return "b14";
    case 0xdeadbe0f: return "b15";
    case 0xdeadbe10: return "b16";
    case 0xdeadbe11: return "b17";
    case 0xdeadbe12: return "b18";
    default: return "bdef";
  }
}

// A runtime-computed NaN, and a runtime-computed Infinity: neither can be
// constant-folded into the call sites below, so the switches really do
// receive them in a register.
function nanOf(a, b) {
  return a / b;
}

// StringSwitchImm. The emitter's whole job is the call to
// _jit_string_switch_imm_table_lookup plus "null means default"; the table
// itself is filled in by the shared driver AFTER this function finishes
// compiling, by resolving the very labels handed to stringSwitchImm. That is
// the contract this case pins: if those labels were copies, or were resolved
// too early, every hit below would land in the wrong place.
function strSwitch(s) {
  switch (s) {
    case "alpha": return 1;
    case "bravo": return 2;
    case "charlie": return 3;
    case "delta": return 4;
    case "echo": return 5;
    case "foxtrot": return 6;
    case "golf": return 7;
    case "hotel": return 8;
    case "india": return 9;
    case "juliet": return 10;
    default: return -1;
  }
}

// Edges of the dense table: minVal, maxVal, and the two values just outside.
// The pins below follow CALL order, not source order: `dense` is entered
// before `say` is, since it produces `say`'s argument.
say(dense(0) + " " + dense(17) + " " + dense(9));
// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK: JIT successfully compiled FunctionID 2, 'dense'
// CHECK: JIT successfully compiled FunctionID 1, 'say'
// CHECK-NEXT: c0 c17 c9
// CHECK0: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 2, 'dense'
// CHECK0: JIT successfully compiled FunctionID 1, 'say'
// CHECK0-NEXT: c0 c17 c9
say(dense(-1) + " " + dense(18));
// CHECK-NEXT: def def
// CHECK0-NEXT: def def
// Non-integers and out-of-range values.
say(dense(4.5) + " " + dense(-0.5) + " " + dense(1e9));
// CHECK-NEXT: def def def
// CHECK0-NEXT: def def def
// -0 is the index 0, because -0 === 0.
say(dense(-0));
// CHECK-NEXT: c0
// CHECK0-NEXT: c0
// Not numbers at all.
say(dense("3") + " " + dense(null) + " " + dense(undefined) + " " + dense({}));
// CHECK-NEXT: def def def def
// CHECK0-NEXT: def def def def

// The biased table: minVal, maxVal, an interior value, and both edges of the
// range check. Every one of these would answer differently if the bias were
// dropped or applied twice.
say(offset(100) + " " + offset(117) + " " + offset(108));
// CHECK: JIT successfully compiled FunctionID 3, 'offset'
// CHECK-NEXT: o100 o117 o108
// CHECK0: JIT successfully compiled FunctionID 3, 'offset'
// CHECK0-NEXT: o100 o117 o108
say(offset(99) + " " + offset(118) + " " + offset(0));
// CHECK-NEXT: odef odef odef
// CHECK0-NEXT: odef odef odef

// The holes fall through the table to the default block; their neighbours
// must not.
say(holes(40) + " " + holes(59) + " " + holes(43) + " " + holes(45));
// CHECK: JIT successfully compiled FunctionID 4, 'holes'
// CHECK-NEXT: h40 h59 h43 h45
// CHECK0: JIT successfully compiled FunctionID 4, 'holes'
// CHECK0-NEXT: h40 h59 h43 h45
say(holes(44) + " " + holes(52) + " " + holes(39) + " " + holes(60));
// CHECK-NEXT: hdef hdef hdef hdef
// CHECK0-NEXT: hdef hdef hdef hdef

// Bounds above INT32_MAX.
say(big(0xdeadbe01) + " " + big(0xdeadbe12) + " " + big(0xdeadbe0a));
// CHECK: JIT successfully compiled FunctionID 5, 'big'
// CHECK-NEXT: b1 b18 b10
// CHECK0: JIT successfully compiled FunctionID 5, 'big'
// CHECK0-NEXT: b1 b18 b10
say(big(0xdeadbe00) + " " + big(0xdeadbe13) + " " + big(0) + " " +
    big(0x7fffffff));
// CHECK-NEXT: bdef bdef bdef bdef
// CHECK0-NEXT: bdef bdef bdef bdef

// THE NaN CASE. All four must reach the default label, but only ONE of them
// depends on the jp. A NaN converts to the uint32 value 0, so:
//   - `dense` starts at 0, so that 0 passes the range check and indexes the
//     table. Without the jp, dense(NaN) prints "c0" instead of "def". This
//     is the case the jp exists for, and it is the one the prove-can-fail
//     run breaks.
//   - `offset`, `holes` and `big` start at 100, 40 and 0xdeadbe01, so the
//     `cmp ecx, minVal` / `jb` range check rejects the 0 before the table is
//     reached. They answer correctly with or without the jp; they are here
//     to cover a NaN operand against a BIASED table, not to test the jp.
var nan = nanOf(0, 0);
say(dense(nan) + " " + offset(nan) + " " + holes(nan) + " " + big(nan));
// CHECK: JIT successfully compiled FunctionID 6, 'nanOf'
// CHECK-NEXT: def odef hdef bdef
// CHECK0: JIT successfully compiled FunctionID 6, 'nanOf'
// CHECK0-NEXT: def odef hdef bdef
// Infinities are rejected by the jne, not the jp, since vcvttsd2si turns
// them into INT64_MIN, whose round trip back to -2^63 is an ordered
// mismatch.
var inf = nanOf(1, 0);
say(dense(inf) + " " + dense(-inf) + " " + big(inf));
// CHECK-NEXT: def def bdef
// CHECK0-NEXT: def def bdef
// 2^32 and 2^31: past the top of any uint32 table, and past INT32_MAX.
say(dense(4294967296) + " " + big(4294967296) + " " + big(2147483648));
// CHECK-NEXT: def bdef bdef
// CHECK0-NEXT: def bdef bdef

// String switch: every case, a miss, and operands that are not strings at
// all (the lookup returns null for those too).
var strs = ["alpha", "bravo", "charlie", "delta", "echo",
            "foxtrot", "golf", "hotel", "india", "juliet"];
var acc = "";
for (var i = 0; i < strs.length; ++i)
  acc += strSwitch(strs[i]) + ",";
say(acc);
// CHECK: JIT successfully compiled FunctionID 7, 'strSwitch'
// CHECK-NEXT: 1,2,3,4,5,6,7,8,9,10,
// CHECK0: JIT successfully compiled FunctionID 7, 'strSwitch'
// CHECK0-NEXT: 1,2,3,4,5,6,7,8,9,10,
say(strSwitch("zulu") + " " + strSwitch("") + " " + strSwitch("alph"));
// CHECK-NEXT: -1 -1 -1
// CHECK0-NEXT: -1 -1 -1
say(strSwitch(7) + " " + strSwitch(null) + " " + strSwitch(undefined));
// CHECK-NEXT: -1 -1 -1
// CHECK0-NEXT: -1 -1 -1
// A string built at runtime, so it is not the same StringPrimitive as the
// case label: the lookup has to compare by value, not by identity.
say(strSwitch("al" + "p" + "ha") + " " + strSwitch(strs[3].toUpperCase()));
// CHECK-NEXT: 1 -1
// CHECK0-NEXT: 1 -1
