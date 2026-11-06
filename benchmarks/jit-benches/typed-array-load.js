/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// GetByVal element-load microbenchmark. Five sum loops, same 1000 x
// 20000 shape as typed-array-store.js, covering the per-site shape
// feedback + inline GetByVal tiers
// (doc/superpowers/specs/2026-09-11-jit-getbyval-design.md):
//
//   ta-load        Int32Array sum -- the evidence-driven typed-array
//                  tier's headline win.
//   arr-load       dense Array sum -- the unconditional JSArray tier
//                  (live on every build/heap-mode/GC config).
//   f16-load       Float16Array sum -- a permanently-declining site
//                  (Float16 has no inline tier, as for stores), so this
//                  is the regression check: the recording helper's tax
//                  must not be permanent.
//   poisoned-load  alternating Int32Array/Float64Array sum, each call
//                  passing a different array to the same call site, so
//                  the underlying GetByVal site sees both kinds --
//                  Int32Array first. Run twice, in both traffic orders
//                  (poisoned-load / poisoned-load-rev), since
//                  poison-keeps-first-kind specializes whichever kind
//                  arrives first (order-dependent by design).
//   arr-holes      sparse-array sum -- 999 of 1000 reads hit holes,
//                  so the JSArray tier declines to the helper on EVERY
//                  call (the tier is already unconditionally present;
//                  no kind evidence can ever help a hole). This is the
//                  decline/demotion control: the recording helper's
//                  per-call tax must be amortized away once the site
//                  demotes to the plain helper.
//
// Run precompiled, interpreter vs JIT (Release build):
//   hermes -O -emit-binary -out tal.hbc benchmarks/jit-benches/typed-array-load.js
//   hermes -b tal.hbc
//   hermes -b -Xjit=force tal.hbc
//
// Measured 2026-09-11: see the spec's Delivered section and
// .superpowers/sdd/2026-09-11-jit-getbyval/task-5-report.md for the
// full A/B (every run, both binaries).

"use strict";

function sumTA(a, n) {
  var s = 0;
  for (var i = 0; i < n; ++i)
    s += a[i];
  return s;
}

function sumA(a, n) {
  var s = 0;
  for (var i = 0; i < n; ++i)
    s += a[i];
  return s;
}

function sumF16(a, n) {
  var s = 0;
  for (var i = 0; i < n; ++i)
    s += a[i];
  return s;
}

// Two distinct functions (rather than one shared one) so each traffic
// order exercises its own GetByVal site: sharing a site across both
// orders would make the second order's "first kind" whatever the first
// order already fixed, instead of testing each order in isolation.
function sumPoisonedFwd(a, n) {
  var s = 0;
  for (var i = 0; i < n; ++i)
    s += a[i];
  return s;
}

function sumPoisonedRev(a, n) {
  var s = 0;
  for (var i = 0; i < n; ++i)
    s += a[i];
  return s;
}

function sumHoles(a, n) {
  var s = 0;
  for (var i = 0; i < n; ++i)
    s += a[i];
  return s;
}

var N = 1000;
var ITER = 20000;

var ta = new Int32Array(N);
for (var i = 0; i < N; ++i)
  ta[i] = i;
var s = 0;
var t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += sumTA(ta, N);
print("ta-load", Date.now() - t0, "ms", "check", s);

var arr = new Array(N);
for (var i = 0; i < N; ++i)
  arr[i] = i;
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += sumA(arr, N);
print("arr-load", Date.now() - t0, "ms", "check", s);

var f16 = new Float16Array(N);
for (var i = 0; i < N; ++i)
  f16[i] = i;
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += sumF16(f16, N);
print("f16-load", Date.now() - t0, "ms", "check", s);

// Int32Array-first: call 0 passes ta1 (Int32Array), call 1 passes ta2
// (Float64Array), alternating thereafter.
var ta1 = new Int32Array(N);
var ta2 = new Float64Array(N);
for (var i = 0; i < N; ++i) {
  ta1[i] = i;
  ta2[i] = i;
}
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += sumPoisonedFwd((j & 1) ? ta2 : ta1, N);
print("poisoned-load", Date.now() - t0, "ms", "check", s);

// Float64Array-first: the same shape, opposite traffic order, on its
// own site.
var ta3 = new Int32Array(N);
var ta4 = new Float64Array(N);
for (var i = 0; i < N; ++i) {
  ta3[i] = i;
  ta4[i] = i;
}
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += sumPoisonedRev((j & 1) ? ta3 : ta4, N);
print("poisoned-load-rev", Date.now() - t0, "ms", "check", s);

// Sparse array: only index 0 is set, and a first write allocates
// storage with elemCount_ = 1, so (N-1)/N of the reads decline at the
// tier's RANGE check (they never reach the empty-slot check; true
// empty-slot declines are exercised by the correctness tests), every
// call, forever. No kind evidence can ever remove this decline
// (the site already has its only tier), so this exercises the
// recording helper's steady-state tax and its demotion.
var holes = new Array(N);
holes[0] = 1;
s = 0;
t0 = Date.now();
for (var j = 0; j < ITER; ++j)
  s += sumHoles(holes, N);
print("arr-holes", Date.now() - t0, "ms", "check", s);
