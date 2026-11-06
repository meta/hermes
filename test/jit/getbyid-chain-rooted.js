/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -gc-sanitize-handles=1 -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// Pointer results across flagging, which allocates. Two sites, each on
// a fresh base <- mid <- recv chain: get1 revalidates in _jit_get_by_id,
// get2 in _jit_get_by_id_refresh. Recording (call 1) flags mid and base,
// allocating their flagged classes, while the helper holds {n: 7}.
// get2 only: miss(get2, {}, 48) demotes its site to the refresh helper
// (as in getbyid-chain-refresh-demoted.js). setPrototypeOf(mid, repl)
// puts repl ({p: {n: 70}, z: 0}, or {p: ..., x: 0} for get2) in base's
// place: same depth and slot, a class whose flagged child does not
// exist yet, so the revalidating call allocates it while holding
// {n: 70}. Each site gets its own base and repl shapes, so that get1's
// flagging has not already allocated get2's flagged classes. With
// -gc-sanitize-handles=1 a HERMESVM_SANITIZE_HANDLES build moves every
// live object at every allocation (other builds ignore the flag), so an
// unrooted result reads freed memory. The revalidating read is a single
// call whose result is used after allocating; read in a loop, later
// inline hits would overwrite a stale result before any GC saw it. An
// unrooted _jit_get_by_id fails at get1's recording call; an unrooted
// _jit_get_by_id_refresh fails at get2's revalidating call (on the
// heap well-formedness assertion).
function main() {
  function get1(o) {
    return o.p;
  }
  function get2(o) {
    return o.p;
  }
  function drive(f, o, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(o);
    return r;
  }
  function miss(f, o, n) {
    for (var i = 0; i < n; ++i)
      f(o);
  }
  function run(g, demote, base, repl) {
    var mid = Object.create(base);
    var recv = Object.create(mid);
    var a = drive(g, recv, 20);
    if (demote)
      miss(g, {}, 48);
    Object.setPrototypeOf(mid, repl);
    // One call, so that its result is the one used after allocating.
    var b = g(recv);
    return a.n + ' ' + b.n;
  }
  print(run(get1, false, {p: {n: 7}, y: 0}, {p: {n: 70}, z: 0}));
  print(run(get2, true, {p: {n: 7}, w: 0}, {p: {n: 70}, x: 0}));
}
main();

// CHECK: 7 70
// CHECK-NEXT: 7 70
