/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// A GetById site whose traffic is entirely uncacheable (a missing
// property) records no shape but sets `seen`, so the demotion pass
// flips it: one "changed" crossing, then three stable ones -- the flip
// lands at the 4 * 64 = 256th decline; 300 reads clear it. Its read
// cache never warms, so no recompile can happen. main() has no ById
// read of its own; the global body reads `main` once.
function main() {
  function get(o) {
    return o.missing;
  }
  function drive(f, o, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(o);
    return r;
  }
  print(drive(get, {a: 1}, 300));
}
main();

// CHECK: undefined
// CHECK: JIT counters:
// CHECK: NumRecompiles: 0{{$}}
// CHECK: NumByIdDemotions: 1{{$}}

// BC-LABEL: Function<get>({{.*}}
// BC: GetById
