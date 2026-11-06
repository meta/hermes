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

// The GetByVal twin of recompile-taval-demote-retired.js. The retirement
// sweep (spec: "Slow-path demotion: the pointer flip", "Retirement
// demotes") pins that a retired body's declines are not merely
// uncounted by the staleness gate -- with the sweep they never reach
// considerRecompile at all, because retirement flips the still-recording
// site to the plain helper immediately.
//
// rec(obj, idx, v, depth) recurses depth downto 0, threading its result
// back up through return values only -- no global accumulator -- so the
// only decline sources in the whole program are its own two sites: the
// ById site (o.p, on a freshly built, alternating-shape object) and the
// ByVal site under test (obj[idx]). A global accumulator variable would
// add a THIRD decline source (a PutById on the global object for every
// assignment), which would perturb the schedule below; that is exactly
// why the running total is threaded through `inner`/the return value
// instead.
//
// o.p is cold under force and NEVER warms -- {p:0,q:0} and {p:0}
// ping-pong every call, so it declines on every one of the 64 descent
// calls from depth 64 down to depth 1. The 64th of those declines (at
// the depth=1 frame) is this function's first threshold crossing; the
// ById site's write cache already holds a class from the immediately
// preceding decline, so the progress check succeeds and version 2
// installs mid-descent -- before the base case (depth 0) is ever
// reached.
//
// That depth=1 frame keeps running version 1's code for the rest of its
// own body (a JIT body does not change under an already-executing
// frame), so its own recursive call to depth 0 is the only call that
// resolves to the new "current" body, version 2. Everything below it
// (just depth 0) runs under version 2. Everything above it -- the 64
// frames for depth 1 through depth 64, all still inside version 1 when
// it retired -- unwinds through version 1's (now retired) ByVal site
// (obj[idx]) once each, 64 declines in total, exactly the threshold.
//
// With the sweep: those 64 declines run through the already-demoted
// plain helper, which calls considerRecompile for none of them, so the
// only crossing this test ever produces is the one that triggered the
// recompile. NumRecompileChecks pins at 1. Without the sweep, those same
// 64 declines still run through the undemoted recording helper, and 64
// declines is exactly one more threshold crossing on the retired (now
// stale) record -- staleness only skips the demotion pass and the
// recompile, not the counter increment at the top of considerRecompile
// -- so NumRecompileChecks reads 2 instead. See the report for this
// task's prove-can-fail reproduction (a scratch no-op of
// demoteAllSitesOnRetirement).
//
// The key is rec's `idx` parameter, never a literal: a literal numeric
// key on a READ lowers to GetByIndex (ISel.cpp), which has no tier at
// all -- unlike the put twin's `obj[0] = v`, which stays PutByVal even
// with a literal key.
function rec(obj, idx, v, depth) {
  var o = (depth & 1) ? {p: 0, q: 0} : {p: 0};
  o.p = v;
  var inner = depth > 0 ? rec(obj, idx, v, depth - 1) : 0;
  return inner + obj[idx];
}
var obj = {};
obj[0] = 42;
// 65 reads of obj[0] (depths 64 downto 0), each contributing 42: the sum
// proves every one of them, across both versions and the demoted and
// still-recording states of the site, read the correct value.
print(rec(obj, 0, 42, 64), obj[0]);

// CHECK: 2730 42
// CHECK: JIT counters:
// CHECK: NumRecompileChecks: 1{{$}}
// CHECK: NumRecompiles: 1{{$}}

// BC-LABEL:Function<rec>({{.*}}
// BC: GetByVal
