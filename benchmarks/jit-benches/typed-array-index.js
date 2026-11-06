/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// GetByIndex (constant-key GetByVal twin) element-load microbenchmark.
// Same 1000 x 20000 call-count shape as typed-array-load.js, but every
// index here is an emit-time LITERAL (a[0], a[1], a[2]...), so each
// reader function is one constant-key GetByIndex site called
// repeatedly by the outer loop, rather than a variable-index GetByVal
// site looped internally
// (doc/superpowers/specs/2026-09-12-jit-getbyindex-design.md):
//
//   vec-dot-f64    length-4 Float64Array "vectors" a, b; the hot body
//                  is a[0]*b[0]+a[1]*b[1]+a[2]*b[2] (three-term dot
//                  product, six GetByIndex sites total) -- the
//                  evidence-driven typed-array tier's headline win for
//                  ByIndex.
//   vec-dot-arr    the same six-site dot product over dense
//                  4-element Arrays -- the unconditional JSArray
//                  tier's headline win for ByIndex.
//   f16-idx        Float16Array a[0], ACCUMULATING (s += a[0]) --
//                  a permanently-declining control (Float16 has no
//                  inline tier, as for GetByVal): the recording
//                  helper's tax must not be permanent.
//   holes-idx      sparse Array a[0] where only a far index is set,
//                  ACCUMULATING (s += a[0]) -- the JSArray tier's
//                  RANGE check declines on EVERY call (storage holds
//                  one element at a far beginIndex_; the empty-slot
//                  check never runs), so this is the
//                  decline/demotion control: the recording helper's
//                  per-call tax must be amortized away once the site
//                  demotes to the plain helper.
//
// Bodies are ACCUMULATING (the result is summed into a running total,
// never discarded) rather than bare reads, matching the GetByVal
// record's finding that an accumulating loop and a bare-read loop see
// different control ratios (perf-diagnosis:
// doc/superpowers/specs/2026-09-11-jit-getbyval-perf-diagnosis.md) --
// so the ByIndex control bars are compared like with like.
//
// Run precompiled, interpreter vs JIT (Release build):
//   hermes -O -emit-binary -out tai.hbc benchmarks/jit-benches/typed-array-index.js
//   hermes -b tai.hbc
//   hermes -b -Xjit=force tai.hbc
//
// Measured 2026-09-12: see the spec's Delivered section and
// .superpowers/sdd/2026-09-12-jit-getbyindex/task-5-report.md for the
// full A/B (every run, both binaries).

"use strict";

function vecDotF64(a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function vecDotArr(a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function readF16Idx(a) {
  return a[0];
}

function readHolesIdx(a) {
  return a[0];
}

var N = 1000;
var ITER = 20000;

var va = new Float64Array(4);
va[0] = 1.5; va[1] = 2.5; va[2] = 3.5; va[3] = 4.5;
var vb = new Float64Array(4);
vb[0] = 0.5; vb[1] = 1.5; vb[2] = 2.5; vb[3] = 3.5;
var s = 0;
var t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  for (var i = 0; i < N; ++i)
    s += vecDotF64(va, vb);
print("vec-dot-f64", Date.now() - t0, "ms", "check", s);

var aa = [1.5, 2.5, 3.5, 4.5];
var ab = [0.5, 1.5, 2.5, 3.5];
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  for (var i = 0; i < N; ++i)
    s += vecDotArr(aa, ab);
print("vec-dot-arr", Date.now() - t0, "ms", "check", s);

var f16 = new Float16Array(4);
f16[0] = 1.5;
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  for (var i = 0; i < N; ++i)
    s += readF16Idx(f16);
print("f16-idx", Date.now() - t0, "ms", "check", s);

// Sparse array: only a far index is set, so storage holds one
// element at beginIndex_ = N-1 and the constant-key read of index 0
// declines at the tier's RANGE check on EVERY call, forever (the
// begin-relative subtract wraps; the empty-slot check never runs --
// true empty-slot declines are exercised by the correctness tests). No kind evidence can ever remove this decline (the
// site already has its only tier), so this exercises the recording
// helper's steady-state tax and its demotion. Matches arr-holes'
// undefined-poisons-the-sum behavior (s += undefined -> NaN, and NaN
// is absorbing): the point is the per-call decline cost, not the
// printed check value.
var holes = new Array(N);
holes[N - 1] = 1;
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  for (var i = 0; i < N; ++i)
    s += readHolesIdx(holes);
print("holes-idx", Date.now() - t0, "ms", "check", s);
