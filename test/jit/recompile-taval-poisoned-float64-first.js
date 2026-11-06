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

// The Float64-first half of the poisoned pair; see
// recompile-taval-poisoned-int32-first.js for the full rationale. The
// two files differ in ONE character -- the sense of the interleaving
// ternary -- and in the kind they pin, which is the point: the first
// observed kind is the one the site keeps, so reversing the order
// reverses the specialization. Here the Float64Array arrives first and
// taKind is fixed at Float64ArrayKind (42); the Int32Array observation
// that follows only sets taPoisoned, and version 2 must still
// specialize kind 42.
function f(x, o, phase, v) {
  x[0] = v;
  o.p = v;
  if (phase) o.q = v;
}
var ta1 = new Int32Array(4);
var ta2 = new Float64Array(4);
var o = {p: 0, q: 0};
for (var i = 0; i < 100; ++i)
  f((i & 1) ? ta1 : ta2, o, 0, i);
print('phase two');
for (var i = 0; i < 100; ++i)
  f((i & 1) ? ta1 : ta2, o, 1, i);
print(o.p, o.q);
// Phase 3, the residual: the same alternating traffic, but the budget
// is spent and version 3 already covers kind 42, so the Int32Array half
// is a permanent decline into the recording helper -- the poisoned
// residual, which no further recompile could ever specialize. The
// record stops changing the moment version 3 installs (taKind and
// taPoisoned are both already set), so this site has no `changed`
// crossing to spend and demotes after the three stable ones, at the
// 3 * 64th residual decline. 512 alternating calls make 256 declines,
// comfortably past that; the rest run through the plain helper.
for (var i = 0; i < 512; ++i)
  f((i & 1) ? ta1 : ta2, o, 1, i);

// OUT: phase two
// OUT-NEXT: 99 99

// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// DUMP: // Inline typed array store (kind 42)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// DUMP: JIT ByVal sites: 1 observed, 1 specialized
// Budget must survive the rest of phase 1: a version-3 compile here
// would mean the poisoned site was mistaken for further progress and
// spent the second recompile early.
// DUMP-NOT: 'f' (version 3)
// DUMP: phase two
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 3)
// DUMP: // Inline typed array store (kind 42)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 3)
// DUMP: JIT ByVal sites: 1 observed, 1 specialized

// The residual demotion. One flip: x's site, in version 3's record.
// The version-1 and version-2 records were swept at retirement, which
// deliberately counts nothing.
// CNT: JIT counters:
// CNT: NumByValDemotions: 1{{$}}
