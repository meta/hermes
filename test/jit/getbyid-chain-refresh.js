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

// The fast refresh. drive(get, recv, 20): calls 1-16 decline in version
// 1 (call 1 records the chain entry and flags mid and base), call 16
// recompiles, calls 17-20 hit (4). Then 48 rounds of: bump(unrel, i)
// adds a new property to unrel, flagged by the cached add `kid.k = 1`,
// which bumps the epoch; the read misses on the stale epoch, and the
// helper's fast refresh walks the flagged path to the same (depth,
// slot), refreshes the entry and returns the value without forwarding
// or counting (48 refreshes, no decline). drive(get, recv, 10): 10 hits.
// NumByIdChainHits = 4 + 10 = 14.
// Fails without the fast refresh: the revalidation after forwarding
// would refresh just as often, but 48 counted declines would demote the
// site (NumByIdDemotions: 1). bump()'s keyed store is a ByVal site; its
// 48 declines (three crossings) do not affect NumByIdDemotions, and
// cannot recompile bump: it has no GetById site and its store is not a
// typed-array access, so it has no progress source. NumRecompiles
// counts get's recompile alone.
function main() {
  var base = {p: 5};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  var unrel = {};
  var kid = Object.create(unrel);
  kid.k = 1;
  function get(o) {
    return o.p;
  }
  function bump(o, i) {
    o['y' + i] = i;
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  function churn(f, o, b, u, n) {
    var s = 0;
    for (var i = 0; i < n; ++i) {
      b(u, i);
      s += f(o);
    }
    return s;
  }
  var s1 = drive(get, recv, 20);
  var s2 = churn(get, recv, bump, unrel, 48);
  var s3 = drive(get, recv, 10);
  print(s1, s2, s3);
}
main();

// OUT: 100 240 50
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 48{{$}}
// CNT: NumByIdChainHits: 14{{$}}
