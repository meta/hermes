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

// cc's version 2 holds a Method block for charCodeAt (calls 17-20 hit).
// Assigning a new function to String.prototype.charCodeAt changes the
// property's value, not String.prototype's class, so the block keeps
// hitting (calls 21-30); it reloads the slot on every hit, so the site
// calls the new function. NumByIdStringHits = 4 + 10 = 14; one recompile.
// Fails before the feature (no hits), and if the block returned the
// value it saw when compiled (the second count would be 0).
function main() {
  var SP = String.prototype;
  function cc(s) {
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
  print(count(cc, 'abc', 20, 97));
  SP.charCodeAt = function () {
    return -1;
  };
  print(count(cc, 'abc', 10, -1));
}
main();

// OUT: 20
// OUT-NEXT: 10
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'cc' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP-NOT: 'cc' (version 3)
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdStringHits: 14{{$}}
