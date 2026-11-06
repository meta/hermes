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

// The refresh after demotion. drive(get, recv, 20): 4 hits as in
// getbyid-chain-refresh.js. miss(get, none, 48): `none` has no p; each
// call misses the chain and declines in version 2 without changing the
// record (the walk is Rejected for a class with no entries), so the
// three crossings are stable and the third demotes the site. It holds
// an emitted chain entry, so the slot becomes _jit_get_by_id_refresh.
// `unrel.y = 2` bumps the epoch. drive(get, recv, 10): the first call
// misses, the refresh helper's fast refresh refreshes (1) and returns
// the value; 9 hits. NumByIdChainHits = 13.
// Fails if demotion always flips to the plain helper (no refresh, 4
// hits).
function main() {
  var base = {p: 5};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  var none = {};
  var unrel = {};
  var kid = Object.create(unrel);
  kid.k = 1;
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
  unrel.y = 2;
  var s2 = drive(get, recv, 10);
  print(s1, s2);
}
main();

// OUT: 100 50
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainRefreshes: 1{{$}}
// CNT: NumByIdChainHits: 13{{$}}
