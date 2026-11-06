/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The first cacheable shape arrives on the very call that crosses the
// threshold on which an uncacheable-only site would demote. It is
// recorded before the crossing is processed, so the site recompiles
// with a chain (the legacy single-shape tier would show "Get from
// object specialization" instead).
//
// Crossing 1 (call 16): `seen` is new, changed. Crossings 2, 3 (calls
// 32, 48): stable 1, 2. Crossing 4 (call 64) would reach 3 and demote;
// call 64 is the first to read an existing `x`.
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f) {
    var s = 0;
    for (var i = 1; i <= 64; ++i)
      s += f(i === 64 ? {x: 5} : {}) === undefined ? 0 : 1;
    return s;
  }
  print(drive(get));
}
main();

// CNT: {{^}}1{{$}}
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
