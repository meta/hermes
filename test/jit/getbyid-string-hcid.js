/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-hc-id-limit=0 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-hc-id-limit=0 -gc-sanitize-handles=1 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-hc-id-limit=0 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// No lazy JIT ID can be assigned (-Xjit-hc-id-limit=0; JitEmitter.cpp
// returns 0 once prevHCId reaches the limit), so a charCodeAt site is
// prepared Ineligible with stringProtoId 0. Call 16 crosses in version 1
// (NotPrepared: one recompile). Version 2 emits nothing and keeps
// declining; at calls 32, 48 and 64 String.prototype's class still reads
// ID 0, equal to the stored 0, so nothing progresses and the third
// crossing demotes the site. Calls 65-160 go through the plain helper.
// NumRecompiles = 1, NumByIdDemotions = 1.
// Fails before the feature (NumRecompiles 0), and if Ineligible were
// progress regardless of IDs (a recompile at every crossing up to the
// budget of 8).
function main() {
  function getCC(s) {
    return s.charCodeAt(0);
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  print(count(getCC, 'abc', 160, 97));
}
main();

// OUT: 160
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdStringHits: 0{{$}}
