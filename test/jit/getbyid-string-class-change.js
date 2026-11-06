/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -gc-sanitize-handles=1 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// getCC's version 2 holds a Method block guarded by String.prototype's
// class ID; calls 17-20 hit (4). An unrelated property moves
// String.prototype to a new class with no lazy ID, so the guard misses
// and version 2 declines again. At its 16th decline (the 16th call of
// the second count) the class-ID term reads 0 against the stored ID and
// recompiles; version 3 prepares against the new class, and calls 17-40
// of the second count hit (24). NumByIdStringHits = 28, NumRecompiles =
// 2, NumByIdDemotions = 0.
// Fails before the feature, and without the class-ID progress term (no
// version 3: 4 hits, NumRecompiles 1).
function main() {
  var SP = String.prototype;
  function getCC(s) {
    return s.charCodeAt;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  var cc = SP.charCodeAt;
  print(count(getCC, 'abc', 20, cc));
  SP.unrelatedCC = 1;
  print(count(getCC, 'abc', 40, cc));
}
main();

// OUT: 20
// OUT-NEXT: 40
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 3)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdStringHits: 28{{$}}
