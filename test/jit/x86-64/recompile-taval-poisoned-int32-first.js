/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// REQUIRES: gc_hades
// UNSUPPORTED: handle_san

// Poison preserves the FIRST kind. Budget 2. x's ByVal site alternates
// Int32Array/Float64Array from call 1, and this variant feeds it the
// Int32Array FIRST: the record's taKind is fixed at Int32ArrayKind (39)
// by that first observation and is never replaced -- the Float64Array
// observation one call later only sets taPoisoned, which selection and
// the progress term both ignore. So version 2 must specialize kind 39
// here, while the Float64-first twin
// (recompile-taval-poisoned-float64-first.js) must specialize kind 42
// from the same interleaving: the rule is deterministic GIVEN the
// traffic order, not order-independent, which is exactly why both
// orders are pinned.
//
// Phase 1: x declines every call (whichever kind is not inline yet)
// and o.p's ById site -- cold under force -- declines every call; the
// pair crosses the 64 threshold (pinned on the RUN line) by call ~32,
// well inside the 100-call phase, and installs version 2. o.q is
// untouched in phase 1 (phase=0), so it is cold at the version-2
// compile too. Once version 2 has the kind-39 tier the site can no
// longer progress -- taKind == specializedTAKind, and the poisoned
// second kind never becomes a second tier -- so the rest of phase 1
// must not spend the second recompile.
//
// Phase 2 sets phase=1: o.q now declines every call (cold at v2) while
// x keeps declining its Float64Array half; the pair crosses 64 again
// and only now, with the budget's second recompile, does version 3
// install (o.q's tier), still specializing kind 39 and nothing more.
function f(x, o, phase, v) {
  x[0] = v;
  o.p = v;
  if (phase) o.q = v;
}
var ta1 = new Int32Array(4);
var ta2 = new Float64Array(4);
var o = {p: 0, q: 0};
for (var i = 0; i < 100; ++i)
  f((i & 1) ? ta2 : ta1, o, 0, i);
print('phase two');
for (var i = 0; i < 100; ++i)
  f((i & 1) ? ta2 : ta1, o, 1, i);
print(o.p, o.q);
// Phase 3, the residual: the same alternating traffic, but the budget
// is spent and version 3 already covers kind 39, so the Float64Array
// half is a permanent decline into the recording helper -- the poisoned
// residual, which no further recompile could ever specialize. The
// record stops changing the moment version 3 installs (taKind and
// taPoisoned are both already set), so this site has no `changed`
// crossing to spend and demotes after the three stable ones, at the
// 3 * 64th residual decline. 512 alternating calls make 256 declines,
// comfortably past that; the rest run through the plain helper.
for (var i = 0; i < 512; ++i)
  f((i & 1) ? ta2 : ta1, o, 1, i);

// OUT: phase two
// OUT-NEXT: 99 99

// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// DUMP: // Inline typed array store (kind 39)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// DUMP: JIT ByVal sites: 1 observed, 1 specialized
// Budget must survive the rest of phase 1: a version-3 compile here
// would mean the poisoned site was mistaken for further progress and
// spent the second recompile early.
// DUMP-NOT: 'f' (version 3)
// DUMP: phase two
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 3)
// DUMP: // Inline typed array store (kind 39)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 3)
// DUMP: JIT ByVal sites: 1 observed, 1 specialized

// The residual demotion. One flip: x's site, in version 3's record.
// The version-1 and version-2 records were swept at retirement, which
// deliberately counts nothing.
// CNT: JIT counters:
// CNT: NumByValDemotions: 1{{$}}
