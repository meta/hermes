/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// load's ByVal site sees nothing but JSArray traffic, and every one of
// its reads is a DECLINE: index 1 is a hole (the element was elided from
// the literal) and index 5 is past the dense region, so neither ever
// takes the inline fast-array LOAD tier's hit path. This is NOT the put
// tier's own mono-jsarray recipe (recompile-taval-mono-jsarray.js): that
// file drives its recompile from an UNRELATED ById site (o.p) so that
// the JSArray-only ByVal site can be pinned as observed-but-never-
// specializing in a version 2 dump. That recipe cannot be reused here --
// an ById site declining every call would itself cross the threshold and
// recompile, and a GetByVal site can never supply the progress term
// (byValShapeProgress only fires on a recorded TYPED-ARRAY kind; JSArray
// traffic is a static prior, never a progress source) -- so a
// JSArray-only site, with no OTHER decline source anywhere in the
// program, can never trigger a recompile at all. What it CAN do is go
// stable and demote, exactly like recompile-getval-demote.js's plain-
// object site, which is the schedule this file actually exercises.
//
// Schedule (threshold 64, kDemotionStableCrossings 3), identical in
// shape to recompile-getval-demote.js: the very first decline sets
// jsArraySeen 0 -> 1, a "changed" record, so crossings 2 through 4 are
// the stable ones and the 4 * 64 = 256th decline flips the site's helper
// slot. 300 declines clears that with room to spare, and the two probe
// reads afterward run through the plain helper and are counted nowhere.
//
// The loop lives inside drive() so its only per-iteration work is on
// locals and parameters: a top-level loop would drive the global
// object's ById sites too, which this test does not want as a second
// decline source.
//
// The key is load's `i` parameter, never a literal: ISel.cpp lowers a
// uint8 literal number key on a READ to GetByIndex, which has no tier at
// all -- unlike a WRITE, where a literal key stays PutByVal (see the put
// demote twins' `o['p'] = v`). A literal-keyed twin of this file would
// silently test GetByIndex and pin nothing about GetByVal at all.
function load(a, i) {
  return a[i];
}
function drive(a, n) {
  for (var i = 0; i < n; ++i)
    load(a, i & 1 ? 5 : 1);
}
var proto = {};
Object.defineProperty(proto, "1", {
  value: "protoData1",
  enumerable: true,
  configurable: true,
});
// A dense 3-element array with index 1 elided (a hole); index 5 is out of
// range for either the array or its own prototype.
var arr = [10, /* hole */, 30];
Object.setPrototypeOf(arr, proto);
drive(arr, 300);
// Fresh top-level calls, running through the by-now-demoted plain
// helper: the hole still resolves through the prototype, and the
// out-of-range read is still undefined -- demotion is a pointer flip in
// the SAME compiled body, not a recompile, so nothing about read
// semantics changes.
print(load(arr, 1), load(arr, 5));

// CHECK: protoData1 undefined
// CHECK: JIT counters:
// The single hopeless site, flipped exactly once, and no recompile ever
// spent: this site can never supply the progress term.
// CHECK: NumRecompiles: 0{{$}}
// CHECK: NumByValDemotions: 1{{$}}

// Nothing anywhere in this program's compiled output ever prints a
// version suffix: load's only compile is its first (unsuffixed), and
// neither drive's nor the global code's own ById declines ever warm
// into a recompile either (see the discussion above).
// DUMP-NOT: {{.*}}(version{{.*}}
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'load'
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'load'
// DUMP-NOT: {{.*}}(version{{.*}}
