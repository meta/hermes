/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Demotion end to end (spec: "The demotion rule"). store()'s ByVal site
// only ever sees a plain object: it can never grow a typed-array tier,
// and the JSArray tier it carries can never hit, so every one of its
// stores declines into the recording helper forever. That is exactly
// what invariant I1 forbids, and the demotion pass ends it by flipping
// the site's helper slot to the plain _sh_ljs helper.
//
// Schedule (threshold 64, kDemotionStableCrossings 3). A site observed
// from cold spends its first crossing on a "changed" record -- the very
// first store sets otherSeen -- so the three stable crossings are the
// second through the fourth, and demotion lands AT the 4 * 64 = 256th
// decline. 300 declines clears that with room to spare; the stores past
// the flip run through the plain helper and are counted nowhere.
//
// The loop lives inside drive() so that its only per-iteration work is
// on locals and parameters: a top-level loop would drive the global
// object's ById sites too, which is a decline source this test does not
// want.
function store(o, k, v) {
  o[k] = v;
}
function drive(o, n) {
  for (var i = 0; i < n; ++i)
    store(o, 'p', i);
}
var o = {};
drive(o, 300);
print(o.p);

// CHECK: 299
// CHECK: JIT counters:
// The single hopeless site, flipped exactly once. Retirement-time flips
// are not counted here, and no recompile happens in this test anyway.
// CHECK: NumByValDemotions: 1{{$}}
