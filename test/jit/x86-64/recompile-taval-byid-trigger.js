/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// REQUIRES: gc_hades
// UNSUPPORTED: handle_san

// f's ById site (o.p) is cold under force and warms through helper
// calls; its ByVal site sees only Int32Array stores, recorded by the
// same helper stream. The ById progress source triggers the recompile;
// version 2 must add the typed-array tier at the ByVal site while
// KEEPING the JSArray tier there. That is the monotone rule: tiers are
// only ever added, and the JSArray tier is a static prior emitted
// wherever the build can emit one -- absence of JSArray evidence is
// not evidence, because nothing observes that tier's hits. Emission
// order is typed-array first, so the two comments are pinned in that
// order, and their coexistence is what proves the duo chaining.
function f(a, o, v) {
  a[0] = v;
  o.p = v;
}
var ta = new Int32Array(4);
var o = {p: 0};
for (var i = 0; i < 100; ++i)
  f(ta, o, i);
print(ta[0], o.p);

// OUT: 99 99

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: // Inline typed array store (kind 39)
// SPEC: // Inline fast array store
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
