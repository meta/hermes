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

// A verdict on an unflagged path suppresses nothing. drive: 4 hits.
// miss(get, none, 48) demotes the site to _jit_get_by_id_refresh (as in
// getbyid-chain-refresh-demoted.js). setPrototypeOf(mid, empty) bumps
// the epoch; `empty` is unflagged and has no p. get(recv): the refresh
// helper's fast refresh finds no p (Rejected) and does nothing; after
// forwarding (undefined), revalidation walks again, Rejected on a path
// with an unflagged object, so refreshFailedAt is NOT set. `empty.p = 9`
// changes the answer without a bump (empty is unflagged; the cached add
// flags only empty's parents). drive(get, recv, 10): the first call
// misses, the fast refresh sees an unflagged path, and the revalidation
// after forwarding (a candidate still exists) flags empty and adopts
// the epoch (refresh 1); 9 hits. NumByIdChainHits = 13.
// Fails if a Rejected verdict sets refreshFailedAt regardless of
// allFlagged (no candidate, no revalidation: 0 refreshes, 4 hits).
function main() {
  var base = {p: 5};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  var none = {};
  function get(o) {
    return o.p;
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
  var s1 = drive(get, recv, 20);
  miss(get, none, 48);
  var empty = {};
  Object.setPrototypeOf(mid, empty);
  var gone = get(recv);
  empty.p = 9;
  var s2 = drive(get, recv, 10);
  print(s1, gone, s2);
}
main();

// OUT: 100 undefined 90
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainRefreshes: 1{{$}}
// CNT: NumByIdChainHits: 13{{$}}
