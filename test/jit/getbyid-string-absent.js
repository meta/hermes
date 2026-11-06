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

// Sites whose name String.prototype's class does not own: `foo` exists
// nowhere, `hasOwnProperty` is inherited from Object.prototype. Each
// site's version 1 is compiled before any string arrives (NotPrepared).
// Calls 1-16 take the recording helper; the first sets sawString (and
// changed) and the 16th crosses the threshold, where the NotPrepared term
// recompiles. Version 2 prepares the site Absent and emits nothing, so
// calls 17-100 still decline. Its crossings at calls 32, 48 and 64 find
// the same class ID, a String.prototype that is not a dictionary, and
// nothing changed, so the third demotes the site to the plain helper;
// calls 65-100 count nothing. Per site: one recompile, one demotion.
// main() runs two recording reads once each (2 declines); count() none.
// Fails before the feature and without the NotPrepared term
// (NumRecompiles 0), if `changed` were set for every string receiver
// (NumByIdDemotions 0: every crossing would see a change), and if Absent
// were progress (NumRecompiles above 2).
function main() {
  function getFoo(s) {
    return s.foo;
  }
  function getHas(s) {
    return s.hasOwnProperty;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  var has = Object.prototype.hasOwnProperty;
  print(count(getFoo, 'abc', 100, undefined), count(getHas, 'abc', 100, has));
}
main();

// OUT: 100 100
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 2{{$}}
// CNT: NumByIdStringHits: 0{{$}}
