/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// PutByVal element-store microbenchmark. Five fill loops, same 1000 x
// 20000 shape throughout, covering the per-site shape feedback + inline
// typed-array store tier ("JIT: specialize PutByVal on observed
// per-site shapes") and its monotone-emission/demotion follow-up
// (doc/superpowers/specs/2026-09-09-jit-putbyval-monotone-demotion-design.md):
//
//   ta-store       Int32Array fill -- the tier's headline win.
//   arr-store      dense Array fill -- the pre-existing fast-array
//                  tier; a regression check for it, unaffected by the
//                  typed-array work.
//   f16-store      Float16Array fill -- a permanently-declining site
//                  (Float16 has no inline tier), so this is invariant
//                  I1's regression check: the recording helper's tax
//                  must not be permanent (monotone emission + demotion
//                  ends it).
//   poisoned-store alternating Int32Array/Float64Array fill, each call
//                  passing a different array to the same call site, so
//                  the underlying PutByVal site sees both kinds --
//                  Int32Array first. Run twice, in both traffic orders
//                  (poisoned-store / poisoned-store-rev), since
//                  poison-keeps-first-kind specializes whichever kind
//                  arrives first (order-dependent by design; see the
//                  spec's "Poison preserves the first kind").
//
// Run precompiled, interpreter vs JIT (Release build):
//   hermes -O -emit-binary -out tas.hbc benchmarks/jit-benches/typed-array-store.js
//   hermes -b tas.hbc
//   hermes -b -Xjit=force tas.hbc
//
// Measured 2026-09-08 (Release/clang x86-64, HV64), pre-monotone-emission:
// ta-store 252 -> 65 ms (3.90x) with the tier; ~1.03x before it;
// arr-store ~73 ms under the JIT, unchanged by the tier.
//
// Measured 2026-09-09 (Release/clang x86-64, HV64), candidate vs the
// pre-feature base (0006114a6), 3-run averages, precompiled -O:
//   ta-store            interp 260.0 -> JIT 63.7 ms (4.08x); base-JIT
//                        242.0 ms (no tier there): candidate 3.80x faster.
//   arr-store            interp 241.0 -> JIT 71.7 ms (3.36x); base-JIT
//                        81.0 ms: candidate 1.13x FASTER (>= 0.99x floor).
//   f16-store            interp 312.0 -> JIT 273.7 ms (1.14x); base-JIT
//                        311.3 ms (no tier, ~flat there): candidate 1.14x
//                        faster (>= 0.95x floor) -- I1's regression fixed.
//   poisoned-store        interp 257.3 -> JIT 150.0 ms (1.72x, >= 1.2x).
//   poisoned-store-rev    interp 259.3 -> JIT 142.0 ms (1.83x, >= 1.2x).
// All five spec acceptance thresholds met. Full A/B (every run, both
// binaries) in the spec's "Delivered" section and
// .superpowers/sdd/2026-09-09-jit-putbyval-monotone-demotion/task-6-report.md.

"use strict";

function fillTA(a, n, v) {
  for (var i = 0; i < n; ++i)
    a[i] = v + i;
  return a[n - 1];
}

function fillA(a, n, v) {
  for (var i = 0; i < n; ++i)
    a[i] = v + i;
  return a[n - 1];
}

function fillF16(a, n, v) {
  for (var i = 0; i < n; ++i)
    a[i] = v + i;
  return a[n - 1];
}

// Two distinct functions (rather than one shared one) so each traffic
// order exercises its own PutByVal site: sharing a site across both
// orders would make the second order's "first kind" whatever the first
// order already fixed, instead of testing each order in isolation.
function fillPoisonedFwd(a, n, v) {
  for (var i = 0; i < n; ++i)
    a[i] = v + i;
  return a[n - 1];
}

function fillPoisonedRev(a, n, v) {
  for (var i = 0; i < n; ++i)
    a[i] = v + i;
  return a[n - 1];
}

var N = 1000;
var ITER = 20000;

var ta = new Int32Array(N);
var s = 0;
var t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += fillTA(ta, N, j);
print("ta-store", Date.now() - t0, "ms", "check", s);

var arr = new Array(N);
for (var i = 0; i < N; ++i)
  arr[i] = 0;
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += fillA(arr, N, j);
print("arr-store", Date.now() - t0, "ms", "check", s);

var f16 = new Float16Array(N);
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += fillF16(f16, N, j);
print("f16-store", Date.now() - t0, "ms", "check", s);

// Int32Array-first: call 0 passes ta1 (Int32Array), call 1 passes ta2
// (Float64Array), alternating thereafter.
var ta1 = new Int32Array(N);
var ta2 = new Float64Array(N);
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += fillPoisonedFwd((j & 1) ? ta2 : ta1, N, j);
print("poisoned-store", Date.now() - t0, "ms", "check", s);

// Float64Array-first: the same shape, opposite traffic order, on its
// own site.
var ta3 = new Int32Array(N);
var ta4 = new Float64Array(N);
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += fillPoisonedRev((j & 1) ? ta3 : ta4, N, j);
print("poisoned-store-rev", Date.now() - t0, "ms", "check", s);
