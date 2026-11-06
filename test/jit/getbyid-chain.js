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

// The DeltaBlue shape (benchmarks/octane/deltablue.js:439-447): an
// Inheriter instance sits between each subclass prototype and Base's,
// so `kind` is two prototypes up from every receiver, and the three
// receiver classes differ because their prototypes differ.
//
// Version 1 is compiled cold. Its generic tier compares the receiver's
// class with the read cache's, which holds Base.prototype's class, so
// each of the first 16 calls declines; calls 1-3 each record a chain
// entry (one per receiver class). Call 16 crosses the threshold, the
// unemitted entries are progress, and version 2 emits all three. Calls
// 17-300 hit inline: NumByIdChainHits = 300 - 16 = 284. Nothing declines
// afterwards, so nothing can demote. main() reads Base.prototype once
// and inheritsFrom reads parent.prototype three times.
// Fails before the feature: nothing is recorded or emitted for `kind`
// (no chain line in the dump, NumByIdChainHits absent).
function main() {
  function inheritsFrom(child, parent) {
    function Inheriter() {}
    Inheriter.prototype = parent.prototype;
    child.prototype = new Inheriter();
  }
  function Base() {}
  Base.prototype.kind = 1;
  function A() {}
  function B() {}
  function C() {}
  inheritsFrom(A, Base);
  inheritsFrom(B, Base);
  inheritsFrom(C, Base);
  function get(o) {
    return o.kind;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % 3]);
    return s;
  }
  print(drive(get, [new A(), new B(), new C()], 300));
}
main();

// OUT: 300
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (3 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 0{{$}}
// CNT: NumByIdChainHits: 284{{$}}
