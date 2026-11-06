/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The GetByVal twin of recompile-taval-demote-delay.js. The `changed`
// reset delays demotion (spec: "The demotion rule", third bullet). Two
// sites, identical decline VOLUME, differing only in whether a knowledge
// transition lands inside the stability window.
//
// Site A (loadA): 256 plain-object declines and nothing else. Its
// record changes only on the very first read, so crossings 2, 3 and 4
// are all stable and the 4 * 64 = 256th decline flips it.
//
// Site B (loadB): the same 256 declines, except that decline 129 -- the
// first decline of the THIRD window, one crossing before A's deadline --
// is an out-of-range READ of a JSArray, which is the one decline shape
// that transitions a field (jsArraySeen 0 -> 1) without being
// actionable. Under monotone emission the JSArray tier is a static prior
// emitted at both sites already, so this transition cannot reset
// stability through the progress branch; only the `changed` branch can
// see it. It does, unchangedCrossings goes back to 0 at crossing 3, and
// B's fourth crossing reaches 1 instead of 3.
//
// Hence exactly ONE demotion for the two sites. Deleting the `changed`
// branch makes B demote too and this pin reads 2; that asymmetry is what
// the test is for. NumRecompileChecks is pinned as well, to show the two
// sites really did cross the threshold the same number of times (4 each)
// rather than B having simply declined less.
//
// loadB's single GetByVal instruction is what makes the JSArray decline
// land in the SAME site record as the plain-object declines: it is
// called with both a plain object and an array, but there is only one
// `return o[k]` in its body, hence one siteId. The key is always the
// `k` parameter, never a literal: a literal numeric key on a READ lowers
// to GetByIndex (ISel.cpp), which has no tier at all.
function loadA(o, k) {
  return o[k];
}
function loadB(o, k) {
  return o[k];
}
function driveA(o, n) {
  for (var i = 0; i < n; ++i)
    loadA(o, 'p');
}
function driveB(o, arr, n) {
  for (var i = 0; i < n; ++i)
    loadB(o, 'p');
  // Decline 129 of site B: an out-of-range JSArray read, so the
  // fast-array tier declines it into the recording helper.
  loadB(arr, 1000);
  for (var i = 0; i < n - 1; ++i)
    loadB(o, 'p');
}
driveA({}, 256);
driveB({}, [1, 2, 3, 4], 128);
print('done');

// CHECK: done
// CHECK: JIT counters:
// CHECK: NumRecompileChecks: 8{{$}}
// CHECK: NumByValDemotions: 1{{$}}

// BC-LABEL:Function<loadA>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<loadB>({{.*}}
// BC: GetByVal
