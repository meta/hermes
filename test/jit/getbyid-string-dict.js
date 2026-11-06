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

// String.prototype is a cacheable dictionary before anything compiles. A
// cacheable dictionary is eligible (JitHandlers.cpp:542-546), so getCC's
// version 2 (call 16) emits a Method block; calls 17-40 hit (24).
// Fails before the feature, and if dictionaries were Ineligible (0 hits).
function main() {
  var SP = String.prototype;
  SP.tmpDict = 0;
  delete SP.tmpDict;
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
  print(count(getCC, 'abc', 40, SP.charCodeAt));
}
main();

// OUT: 40
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdStringHits: 24{{$}}
