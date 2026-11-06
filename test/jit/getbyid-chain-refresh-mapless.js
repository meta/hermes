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

// The fast refresh through a holder class with no property map, whose p
// went through a flags transition (adee03e65, dz 01a170e9).
// mkHolder(def): {p: 5, q: 6}, then p made non-enumerable: a flags
// transition into class C that keeps p at slot 0; C takes the map.
// holder and 48 twins are built this way, all on C. flag(t, mk) does a
// cached add on an object whose parent is t, which flags t
// (JSObject::tryCacheAddProperty): the first creates C', C's flagged
// child, without a map (HiddenClass.cpp:790-797); every twin moves to
// C'. holder is still on C.
// drive(get, recv, 20): call 1 walks C's map, finds p at depth 2, slot
// 0, and flags mid and holder (holder moves to C'). Calls 2-16 forward;
// their lookups steal C's map into C' (HiddenClass.cpp:1132-1136), so C
// is mapless from here on. Call 16 recompiles; calls 17-20 hit (4).
// 48 rounds of: bumpTwin(twins[i], i) adds a NEW property to a twin on
// C', which rebuilds C''s map and moves it to the new child, and bumps
// the epoch (the twin is flagged); then the read's fast refresh walks
// mid, then holder's mapless C', so findPropertyNoAlloc walks C' -> C
// (p's flags transition) -> ... and must return slot 0. It refreshes
// and returns without forwarding (48 refreshes, no decline). Then 10
// hits. NumByIdChainHits = 14.
// With adee03e65's HiddenClass.cpp hunk reverted, findPropertyNoAlloc
// returns slot 1 at C, the fast refresh never matches, every round
// forwards (rebuilding C''s map, so revalidation still refreshes) and
// declines, and 48 declines demote the site: NumByIdDemotions: 1.
// Declines outside get (GetById, PutById and PutByVal misses, per
// function): build 48 (its `tw[i] = t` appends), flag 48 (`k.k = 1`;
// each k has a different parent, which the add cache checks, so
// _jit_put_by_id never hits), bumpTwin 48 (its keyed store); each
// crosses three times. None of them has a GetById site, so
// none can demote one, and none can recompile: their ByVal stores are
// not typed-array accesses, and flag's cold PutById site never warms,
// because an add never fills the write cache's class. Object.create and
// Object.defineProperty are read once, in main, and passed in. main's
// few reads, mkHolder and churn stay far below 16. So get's recompile
// is the only one, and NumByIdDemotions counts only get's site.
function main() {
  function mkHolder(def) {
    var h = {p: 5, q: 6};
    def(h, 'p', {enumerable: false});
    return h;
  }
  function flag(t, mk) {
    var k = mk(t);
    k.k = 1;
  }
  function bumpTwin(t, i) {
    t['r' + i] = 1;
  }
  function build(n, def, mk) {
    var tw = [];
    for (var i = 0; i < n; ++i) {
      var t = mkHolder(def);
      flag(t, mk);
      tw[i] = t;
    }
    return tw;
  }
  var create = Object.create;
  var define = Object.defineProperty;
  var holder = mkHolder(define);
  var twins = build(48, define, create);
  var mid = create(holder);
  var recv = create(mid);
  function get(o) {
    return o.p;
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  function churn(f, o, b, tw, n) {
    var s = 0;
    for (var i = 0; i < n; ++i) {
      b(tw[i], i);
      s += f(o);
    }
    return s;
  }
  var s1 = drive(get, recv, 20);
  var s2 = churn(get, recv, bumpTwin, twins, 48);
  var s3 = drive(get, recv, 10);
  print(s1, s2, s3);
}
main();

// OUT: 100 240 50
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 48{{$}}
// CNT: NumByIdChainHits: 14{{$}}
