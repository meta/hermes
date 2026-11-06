/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// ToInt32/ToUint32 applied to a boolean.
//
// A comparison yields a boolean, and coercing it back to a number (`| 0`,
// `>>> 0`) emits ToInt32/ToUint32 on that boolean. The JIT's inline ToInt32
// path is a double round-trip, which a bool's tagged pattern can never pass
// (it reads as a NaN), so each site falls out of line to
// _sh_ljs_to_int32_rjs unless the emitter special-cases a statically known
// bool.
//
// This is the shape Wasm i32 comparisons produce in bulk -- FCompare + AsInt32
// for every i32.eq/ne/lt/gt -- but it needs no Wasm to reproduce.
//
// The comparisons must depend on the loop variable. A loop-invariant
// comparison gets hoisted out of the loop body, and since the JIT tracks
// bool-ness as a per-basic-block local type, the ToInt32 then no longer sees
// a statically known bool and the fast path cannot apply.
//
// The int* controls are integral numbers: the double round-trip accepts
// them, so they exercise the pre-existing INLINE path and must not move.
// The frac* controls are fractional, so they fail the round-trip and take
// the pre-existing numeric slow path; they must not move either. Both kinds
// are needed -- an integral control alone would not cover the slow path, and
// a fractional one alone would not cover the inline path.

var N = 20000000;

function eqToInt32(n) {
  'noinline';
  var s = 0;
  for (var i = 0; i < n; ++i) s += ((i & 7) === 3) | 0;
  return s;
}

function neToInt32(n) {
  'noinline';
  var s = 0;
  for (var i = 0; i < n; ++i) s += ((i & 7) !== 3) | 0;
  return s;
}

function ltToInt32(n) {
  'noinline';
  var s = 0;
  for (var i = 0; i < n; ++i) s += ((i & 7) < 3) | 0;
  return s;
}

function eqToUint32(n) {
  'noinline';
  var s = 0;
  for (var i = 0; i < n; ++i) s += ((i & 7) === 3) >>> 0;
  return s;
}

// --- controls: the operand is already a number ---

// Integral: passes emit_double_is_int's round trip, stays inline.
function intToInt32(n) {
  'noinline';
  var s = 0;
  for (var i = 0; i < n; ++i) s += (i + 1) | 0;
  return s;
}

function intToUint32(n) {
  'noinline';
  var s = 0;
  for (var i = 0; i < n; ++i) s += (i + 1) >>> 0;
  return s;
}

// Fractional: fails the round trip, takes the numeric slow path.
function fracToInt32(n) {
  'noinline';
  var s = 0;
  for (var i = 0; i < n; ++i) s += (i + 1.5) | 0;
  return s;
}

function run(name, fn) {
  // Warm up past the JIT and recompile thresholds.
  for (var w = 0; w < 200; ++w) fn(1000);
  var t = Date.now();
  var r = fn(N);
  print(name, Date.now() - t, "ms", "res", r);
}

run("eq->int32  ", eqToInt32);
run("ne->int32  ", neToInt32);
run("lt->int32  ", ltToInt32);
run("eq->uint32 ", eqToUint32);
run("int->int32 ", intToInt32);
run("int->uint32", intToUint32);
run("frac->int32", fracToInt32);
