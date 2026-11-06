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

// The GetByIndex twin of recompile-getval-demote.js (spec: "The demotion
// rule"), and the end-to-end pin for the FOURTH helper identity: the
// demotion machinery has to recognize _jit_get_by_index as a recording
// helper and map it back to the plain _sh_ljs_get_by_index_rjs.
//
// load()'s ByIndex site only ever sees a plain object: it can never grow
// a typed-array tier, and the JSArray tier it carries can never hit (a
// plain object is not of CellKind JSArray), so every one of its reads
// declines into the recording helper forever. That is exactly what
// invariant I1 forbids, and the demotion pass ends it by flipping the
// site's helper slot.
//
// Schedule (threshold 64, kDemotionStableCrossings 3), identical to the
// ByVal twin's: a site observed from cold spends its first crossing on a
// "changed" record -- the very first read sets otherSeen -- so the three
// stable crossings are the second through the fourth, and demotion lands
// AT the 4 * 64 = 256th decline. 300 declines clears that with room to
// spare; the reads past the flip run through the plain helper and are
// counted nowhere.
//
// The object carries a real property at "0", so the value printed after
// the flip is a value the demoted PLAIN helper had to resolve -- a
// mismapped identity that flipped the slot to some other helper would not
// print it.
//
// The loop lives inside drive() so that its only per-iteration work is on
// locals and parameters: a top-level loop would drive the global object's
// ById sites too, which is a decline source this test does not want.
//
// The key is a LITERAL uint8, which is precisely what makes this
// GetByIndex rather than GetByVal (ISel.cpp); the BC check below pins
// that.
function load(o) {
  return o[0];
}
function drive(o, n) {
  for (var i = 0; i < n; ++i)
    load(o);
}
var o = {0: 'zero'};
drive(o, 300);
print(load(o));

// CHECK: zero
// CHECK: JIT counters:
// The single hopeless site, flipped exactly once. Retirement-time flips
// are not counted here, and no recompile happens in this test anyway.
// CHECK: NumByValDemotions: 1{{$}}

// BC-LABEL:Function<load>({{.*}}
// BC: GetByIndex
