/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Two phases pin that a RETIRED body's slow-path declines cannot spend
// the recompile budget (they land in the retired version's own record),
// while the current body's declines still can.
//
// The deep call: descent stores o.p at every level; v1's declines cross
// the threshold (64, pinned on the RUN line) mid-descent, p's cache is
// warm, version 2 installs with p's tier and q on its cold list (q has
// never executed). The REMAINING DESCENT runs v2; only the UNWIND
// executes in the ~64 live v1 frames. The unwind stores o.q at every
// level: ~37 declines from v2 frames, ~64 from retired v1 frames, and
// the helper calls warm q's cache. Under shared counting the v1
// declines would cross the threshold with q warmed and produce
// version 3 DURING the unwind; per-version counters must not (phase 1
// pin). Phase 2's shallow calls
// each decline once on q under v2, crossing v2's own threshold at ~27
// calls: version 3 must appear, with q's tier -- proving the budget
// survived phase 1 and triggering still works.

function rec(o, n) {
  o.p = n;
  if (n > 0)
    rec(o, n - 1);
  o.q = n;
  return o.p;
}

var o = {p: -1, q: -1};
print(rec(o, 100));
print('phase two');
for (var i = 0; i < 40; ++i)
  rec(o, 0);
print(o.q);

// OUT: 0
// OUT-NEXT: phase two
// OUT-NEXT: 0

// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec'
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec' (version 2)
// DUMP-NOT: 'rec' (version 3)
// DUMP: phase two
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec' (version 3)
// DUMP: 0
