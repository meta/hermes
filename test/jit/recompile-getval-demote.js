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

// The GetByVal twin of recompile-taval-demote.js (spec: "The demotion
// rule"). load()'s ByVal site only ever sees a plain object: it can
// never grow a typed-array tier, and the JSArray tier it carries can
// never hit, so every one of its reads declines into the recording
// helper forever. That is exactly what invariant I1 forbids, and the
// demotion pass ends it by flipping the site's helper slot to the plain
// _sh_ljs_get_by_val_rjs helper.
//
// Schedule (threshold 64, kDemotionStableCrossings 3), identical to the
// put twin's: a site observed from cold spends its first crossing on a
// "changed" record -- the very first read sets otherSeen -- so the three
// stable crossings are the second through the fourth, and demotion lands
// AT the 4 * 64 = 256th decline. 300 declines clears that with room to
// spare; the reads past the flip run through the plain helper and are
// counted nowhere.
//
// The loop lives inside drive() so that its only per-iteration work is
// on locals and parameters: a top-level loop would drive the global
// object's ById sites too, which is a decline source this test does not
// want.
//
// The key is load's `k` parameter, never a literal: a numeric literal
// key on a READ lowers to GetByIndex (ISel.cpp), which has no tier at
// all -- unlike a WRITE, where a literal key stays PutByVal. Here the
// key is the string 'p', which is never subject to that special-casing
// either way, but it is still passed as a parameter rather than written
// directly into load's own body, and the BC check below pins that this
// really compiles to GetByVal and not some other opcode.
function load(o, k) {
  return o[k];
}
function drive(o, n) {
  for (var i = 0; i < n; ++i)
    load(o, 'p');
}
var o = {};
drive(o, 300);
print(load(o, 'p'));

// CHECK: undefined
// CHECK: JIT counters:
// The single hopeless site, flipped exactly once. Retirement-time flips
// are not counted here, and no recompile happens in this test anyway.
// CHECK: NumByValDemotions: 1{{$}}

// BC-LABEL:Function<load>({{.*}}
// BC: GetByVal
