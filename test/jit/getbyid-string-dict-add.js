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

// String.prototype is a cacheable dictionary (an added property
// deleted), whose class keeps its identity and lazy ID when a property is
// added. getFoo's version 2 (call 16, NotPrepared) prepares `foo` Absent
// with the class's property count; calls 17-24 decline. Adding foo raises
// the count without changing the class, so at version 2's 16th decline
// (call 32, the 8th of the second count) only the count term can
// progress, and it recompiles; version 3 finds foo and emits a Method
// block: calls 33-64 hit (32). NumRecompiles = 2, NumByIdDemotions = 0,
// NumByIdStringHits = 32.
// Fails before the feature, and without the count term (no version 3:
// version 2 demotes at call 64; NumRecompiles 1, NumByIdDemotions 1, 0
// hits).
function main() {
  var SP = String.prototype;
  SP.tmpDict = 0;
  delete SP.tmpDict;
  function getFoo(s) {
    return s.foo;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  print(count(getFoo, 'abc', 24, undefined));
  SP.foo = 42;
  print(count(getFoo, 'abc', 40, 42));
}
main();

// OUT: 24
// OUT-NEXT: 40
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getFoo' (version 3)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdStringHits: 32{{$}}
