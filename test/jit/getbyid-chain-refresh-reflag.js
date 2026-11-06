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

// Revalidation flags the replacement path. Two sites, each on a fresh
// base <- inter <- mid <- recv chain with p three prototypes up (slot
// 0): g1 revalidates in _jit_get_by_id, g2 in _jit_get_by_id_refresh.
// - drive(g, recv, 20): 16 declines, recompile, 4 hits.
// - g2 only: miss(g2, none, 48) demotes the site to the refresh helper,
//   as in getbyid-chain-refresh-demoted.js.
// - setPrototypeOf(mid, inter2), inter2 = Object.create(base): a bump
//   (mid is flagged) that keeps p at depth 3, slot 0, behind the new,
//   unflagged inter2. The next call misses; the fast refresh sees an
//   unflagged path and does nothing; after forwarding, revalidation
//   flags inter2 and adopts the epoch (one refresh per site); 9 hits.
// - inter2.p = 9 shadows base's p at depth 2. inter2 is flagged, so the
//   add bumps the epoch, and all 10 reads miss the chain and forward 9.
// Per site: 13 hits, 1 refresh. Totals: NumByIdChainHits = 26,
// NumByIdChainRefreshes = 2. g1's version 2 declines 11 times (1 + 10),
// below the threshold, so NumRecompiles = 2 (g1 and g2).
// Fails if revalidation adopts the epoch without flagging the path
// (chainAfterForward's revalidation `flagChainPath` call deleted):
// inter2 stays unflagged, the shadowing add bumps nothing, and both
// sites keep hitting base's stale 5 (`50 50` twice, 46 hits).
function main() {
  function build() {
    var base = {p: 5};
    var inter = Object.create(base);
    var mid = Object.create(inter);
    return [base, mid, Object.create(mid)];
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  function miss(f, o, n) {
    for (var i = 0; i < n; ++i)
      f(o);
  }
  // Returns the sums before and after the shadowing add.
  function run(g, demote) {
    var t = build();
    drive(g, t[2], 20);
    if (demote)
      miss(g, {}, 48);
    var inter2 = Object.create(t[0]);
    Object.setPrototypeOf(t[1], inter2);
    var before = drive(g, t[2], 10);
    inter2.p = 9;
    return before + ' ' + drive(g, t[2], 10);
  }
  function g1(o) {
    return o.p;
  }
  function g2(o) {
    return o.p;
  }
  print(run(g1, false));
  print(run(g2, true));
}
main();

// OUT: 50 90
// OUT-NEXT: 50 90
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainRefreshes: 2{{$}}
// CNT: NumByIdChainHits: 26{{$}}
