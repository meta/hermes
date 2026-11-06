/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The retirement sweep (spec: "Slow-path demotion: the pointer flip",
// "Retirement demotes") pins that a retired body's declines are not
// merely uncounted by the staleness gate -- with the sweep they never
// reach considerRecompile at all, because retirement flips the
// still-recording site to the plain helper immediately.
//
// rec(obj, v, depth) recurses depth downto 0. Its ById site (o.p, on a
// freshly built, alternating-shape object) is cold under force and
// NEVER warms -- {p:0,q:0} and {p:0} ping-pong every call, so it
// declines on every one of the 64 descent calls from depth 64 down to
// depth 1. The 64th of those declines (at the depth=1 frame) is this
// function's first threshold crossing; the ById site's write cache
// already holds a class from the immediately preceding decline, so the
// progress check succeeds and version 2 installs mid-descent -- before
// the base case (depth 0) is ever reached.
//
// That depth=1 frame keeps running version 1's code for the rest of
// its own body (a JIT body does not change under an already-executing
// frame), so its own recursive call to depth 0 is the only call that
// resolves to the new "current" body, version 2. Everything below it
// (just depth 0) runs under version 2. Everything above it -- the 64
// frames for depth 1 through depth 64, all still inside version 1 when
// it retired -- unwinds through version 1's (now retired) ByVal site
// (obj[0] = v) once each, 64 declines in total, exactly the threshold.
//
// With the sweep: those 64 declines run through the already-demoted
// plain helper, which calls considerRecompile for none of them, so the
// only crossing this test ever produces is the one that triggered the
// recompile. NumRecompileChecks pins at 1. Without the sweep (proven by
// scratch-building with the demoteAllSitesOnRetirement call at install
// no-op'd), those same 64 declines still run through the undemoted
// recording helper, and 64 declines is exactly one more threshold
// crossing on the retired (now stale) record -- staleness only skips
// the demotion pass and the recompile, not the counter increment at
// the top of considerRecompile -- so NumRecompileChecks reads 2
// instead. Verified directly, not FileCheck-encoded here: see the fix
// report for the reproduction.
function rec(obj, v, depth) {
  var o = (depth & 1) ? {p: 0, q: 0} : {p: 0};
  o.p = v;
  if (depth > 0)
    rec(obj, v, depth - 1);
  obj[0] = v;
}
var obj = {};
rec(obj, 42, 64);
print(obj[0]);

// CHECK: 42
// CHECK: JIT counters:
// CHECK: NumRecompileChecks: 1{{$}}
// CHECK: NumRecompiles: 1{{$}}
