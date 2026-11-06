/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Prototype traffic first, then alternating own shapes at the same
// site: a stale negMatchClazz must not block the own entries. Phase 2
// triggers version 3.
//
// The staleness has to last. An own fill through the runtime's fast
// lookup (tryGetOwnNamedDescriptorFast, StaticH.cpp) sets only `clazz`
// and leaves phase 1's negMatchClazz in place; but a receiver whose
// class has no property map yet takes the slow lookup, whose fill
// (JSObject.cpp, getNamedWithReceiver_RJS) clears negMatchClazz -- and
// from then on a recorder that wrongly required a null negMatchClazz
// would record the own shapes anyway. `peek` looks `x` up once on each
// phase-2 receiver beforehand, at its own site, so both classes have
// their maps and every phase-2 fill at `get`'s site is a fast one.
// `peek`'s two reads stay far below the threshold.
function main() {
  function P1() {}
  P1.prototype.x = 10;
  function P2() { this.k = 0; }
  P2.prototype.x = 20;
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 1]);
    return s;
  }
  function peek(o) {
    return o.x;
  }
  var own = [{x: 1}, {y: 0, x: 2}];
  drive(peek, own, 2);
  print(drive(get, [new P1(), new P2()], 100));
  print(drive(get, own, 100));
}
main();

// OUT: 1500
// OUT-NEXT: 150
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 3)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 3)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic
// CNT: NumRecompiles: 2{{$}}
