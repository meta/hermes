/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=0 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=0 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Recording and refreshing a chain entry that is never emitted: with a
// recompile budget of 0, `get` keeps its cold version 1 and every read
// goes through _jit_get_by_id. 10 + 3 + 3 = 16 calls, 16 declines, one
// threshold crossing (NumRecompileChecks: 1).
// - Call 1 forwards, the read cache yields nothing (p is two prototypes
//   up), and the walk finds p on an unflagged path: mid and base are
//   flagged and a chain entry is recorded at the current epoch.
// - `unrel.y = 2` bumps the epoch: unrel was flagged by the cached add
//   `kid.k = 1` (JSObject::tryCacheAddProperty). Call 11's fast refresh
//   finds the stale entry, walks a fully flagged path to the same depth
//   and slot, and refreshes it (refresh 1). The entry is not emitted, so
//   the call still forwards and counts its decline; returning early
//   would leave 15 declines and no crossing.
// - setPrototypeOf(mid, ...) bumps again and puts an unflagged object
//   with p at the same slot in base's place. Call 14's fast refresh sees
//   an unflagged path and does nothing; after forwarding, revalidation
//   flags the new object and refreshes the entry (refresh 2).
// main() itself declines a handful of times (Object.create,
// Object.setPrototypeOf, the two adds), far below 16.
// Fails before the feature (no NumByIdChainRefreshes counter).
function main() {
  var base = {p: 5, w: 0};
  var mid = Object.create(base);
  var recv = Object.create(mid);
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
  var s1 = drive(get, recv, 10);
  unrel.y = 2;
  var s2 = drive(get, recv, 3);
  Object.setPrototypeOf(mid, {p: 7, z: 0});
  var s3 = drive(get, recv, 3);
  print(s1, s2, s3);
}
main();

// OUT: 50 15 21
// CNT: NumRecompileChecks: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 2{{$}}
