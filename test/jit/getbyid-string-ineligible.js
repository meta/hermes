/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -gc-sanitize-handles=1 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// An Ineligible site against a String.prototype class that HAS a lazy JIT
// ID (getbyid-string-hcid.js covers ID 0). String.prototype.acc is an own
// accessor, so prepareByIdString assigns the class an ID (the lazyId call
// comes before any tier test) and then, finding an accessor, prepares
// Ineligible, which emits no block: every string read of s.acc declines
// into _jit_get_by_id.
// - Version 1 of getAcc is compiled at its first call, before any string
//   (NotPrepared). Call 1 records sawString (changed); call 16 crosses:
//   NotPrepared progresses, so getAcc recompiles (NumRecompiles 1).
// - Version 2 prepares Ineligible with stringProtoId = the class's ID.
//   Later strings record nothing (recordByIdObservation marks only the
//   first), and a string receiver adds no shape entry. At the crossings at
//   calls 32, 48 and 64 the class is unchanged, so its ID equals the
//   stored one and byIdStringCanProgress is false (the dictionary-growth
//   term is for Absent only): nothing progresses and nothing changed. The
//   third such crossing (kDemotionStableCrossings = 3) demotes the site to
//   the plain helper (NumByIdDemotions 1), which counts no declines, so
//   calls 65-160 cross nothing.
// NumRecompiles = 1, NumByIdDemotions = 1, NumByIdStringHits = 0.
// main()'s own recording reads (String.prototype, Object.defineProperty)
// are two, under 16; its global reads are TryGetById, which does not record.
// Fails before the feature (no string recompile: NumRecompiles 0), and if
// Ineligible progressed whatever the ID (a recompile at every crossing
// until the budget of 8 runs out, and no demotion).
function main() {
  var SP = String.prototype;
  Object.defineProperty(SP, 'acc', {
    get: function () {
      return 7;
    },
    configurable: true,
  });
  function getAcc(s) {
    return s.acc;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  print(count(getAcc, 'abc', 160, 7));
}
main();

// OUT: 160
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdStringHits: 0{{$}}
