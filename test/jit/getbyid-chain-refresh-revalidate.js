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

// Revalidation. drive(get, recv, 20): 4 hits. setPrototypeOf(mid, ...)
// bumps the epoch (mid is flagged) and puts a new, unflagged object
// with p at the same slot (0) where base was. The next call misses; the
// fast refresh walks a path with an unflagged object and does nothing;
// after forwarding, revalidation flags the new object and adopts the
// epoch (refresh 1, one decline). 9 hits follow. NumByIdChainHits = 13.
// Fails if revalidation does not adopt the epoch (`same->epoch = now`
// deleted in chainAfterForward): the counter still counts the first
// miss, and the second call's fast refresh, on the now flagged path,
// does the refresh: 2 refreshes, 12 hits.
function main() {
  var base = {p: 5, w: 0};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  function get(o) {
    return o.p;
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  var s1 = drive(get, recv, 20);
  Object.setPrototypeOf(mid, {p: 7, z: 0});
  var s2 = drive(get, recv, 10);
  print(s1, s2);
}
main();

// OUT: 100 70
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 1{{$}}
// CNT: NumByIdChainHits: 13{{$}}
