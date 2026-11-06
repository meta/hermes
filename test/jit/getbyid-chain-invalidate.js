/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Six mutations of an emitted chain's path, one site each (g1..g6,
// since entries are per site). run() builds a fresh base <- mid <- recv
// chain (p two prototypes up), drives its site 20 times (16 declines in
// version 1, recompile, 4 hits: 24 hits over six cases), mutates, and
// reads twice.
// 1. Shadowing add on mid (a class change of a flagged object, so a
//    bump): the walk finds p at depth 1 and the read cache yields a
//    prototype hit; both reads miss the chain. 10 10.
// 2. setPrototypeOf(mid, {x: 0, p: 20}): p is now at slot 1, a new
//    identity, recorded but not emitted; both reads miss. (Replacing it
//    with p at the same slot is the revalidation case of
//    getbyid-chain-refresh-revalidate.js.) 20 20.
// 3. delete base.p: base becomes a dictionary, which the walk rejects.
//    undefined undefined.
// 4. p redefined as a getter on base: an accessor, rejected. 40 40.
// 5. p made non-writable: a flags transition (a bump) that keeps p's
//    slot; the first read's fast refresh finds the same (depth, slot)
//    on a flagged path and refreshes (the only refresh in this test);
//    the second read hits. 1 1.
// 6. base.p = 60: no class change, no bump; both reads hit. 60 60.
// NumByIdChainHits = 24 + 1 + 2 = 27. NumByIdChainRefreshes = 1.
// Removing the emitted epoch compare makes case 1 read 1 1. Later cases
// may then crash (on x86-64 case 2 read 0 0 and case 3 aborted on an
// empty-value assertion); only case 1's failure is relied on.
function main() {
  function drive(f, o, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(o);
    return r;
  }
  function build() {
    var base = {p: 1};
    var mid = Object.create(base);
    return [base, mid, Object.create(mid)];
  }
  function run(g, mutate) {
    var t = build();
    drive(g, t[2], 20);
    mutate(t);
    return g(t[2]) + ' ' + g(t[2]);
  }
  function g1(o) {
    return o.p;
  }
  function g2(o) {
    return o.p;
  }
  function g3(o) {
    return o.p;
  }
  function g4(o) {
    return o.p;
  }
  function g5(o) {
    return o.p;
  }
  function g6(o) {
    return o.p;
  }
  print(run(g1, function (t) { t[1].p = 10; }));
  print(run(g2, function (t) { Object.setPrototypeOf(t[1], {x: 0, p: 20}); }));
  print(run(g3, function (t) { delete t[0].p; }));
  print(run(g4, function (t) {
    Object.defineProperty(t[0], 'p', {get: function () { return 40; }});
  }));
  print(run(g5, function (t) {
    Object.defineProperty(t[0], 'p', {writable: false});
  }));
  print(run(g6, function (t) { t[0].p = 60; }));
}
main();

// OUT: 10 10
// OUT-NEXT: 20 20
// OUT-NEXT: undefined undefined
// OUT-NEXT: 40 40
// OUT-NEXT: 1 1
// OUT-NEXT: 60 60
// CNT: NumRecompiles: 6{{$}}
// CNT: NumByIdChainRefreshes: 1{{$}}
// CNT: NumByIdChainHits: 27{{$}}
