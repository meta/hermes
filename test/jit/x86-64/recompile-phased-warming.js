/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// considerRecompile's progress check must require one of the SPECIFIC
// sites the last compile recorded as cold to have warmed -- not just any
// unrelated cache. `o.s = 1` writes to an accessor, so its write cache
// never records a class and that site declines forever, driving the
// decline counter every call. `o.q = 2` only runs once `late` is true,
// so its site starts cold too. `return o.p` is a plain data read that
// warms on the very first call and is unrelated to either write site.
// With the pre-fix bug (any non-null cache entry counts as progress),
// the warm `o.p` read satisfies the check as soon as the decline
// threshold is crossed by `o.s`'s misses -- long before `late` is ever
// true -- so the budget (of 1) is spent recompiling a byte-identical
// body, and `o.q` never gets a chance to be specialized.

function f(o, late) {
  o.s = 1;
  if (late) o.q = 2;
  return o.p;
}
var o = {p: 0, q: 0, set s(v) {}};
for (var i = 0; i < 200; ++i) f(o, false);
print('late phase');
for (var i = 0; i < 200; ++i) f(o, true);
print(o.q);

// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// v1 has two sites it could not specialize: the setter write (never
// warms) and the not-yet-taken `o.q` write. `o.p`'s read cache warms on
// the first call, so it is not counted.
// CHECK-NEXT: JIT cold ById sites: 2
// The budget must survive all of phase one -- bound the -NOT up to
// 'late phase' so a premature version-2 body (the pre-fix bug: the
// unrelated warm `o.p` read cache satisfying the old progress check)
// would be caught here.
// CHECK-NOT: (version
// CHECK: late phase
// Only once `o.q`'s write cache warms, on the first taken branch in
// phase two, does considerRecompile see a recorded cold site that has
// actually changed, and the budget (of 1) is spent installing version 2.
// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// Version 2 specializes `o.q`; only the setter site remains cold.
// CHECK-NEXT: JIT cold ById sites: 1
// CHECK-NEXT: 2
