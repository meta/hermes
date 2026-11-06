/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// demoteSite's strictness derivation (spec: "the matching plain
// helper ... strictness is derived from the slot's current value, no
// extra field"). store()'s ByVal site is compiled from a strict-mode
// instruction, so its slot starts out holding _jit_put_by_val_strict;
// demotion must flip it to _sh_ljs_put_by_val_strict_rjs, not the loose
// variant, or a demoted strict store into a frozen object would
// silently succeed instead of throwing.
//
// The first 300 stores are the same schedule as recompile-taval-demote
// .js: a plain-object site that can never progress declines every
// time, so its 4 * 64 = 256th decline demotes it. The demoted site is
// then exercised once more, this time against a frozen object: if the
// flip picked the strict helper, this throws TypeError, matching the
// interpreter's (non-JIT) behavior for the same store; if demoteSite
// always installed the loose helper instead (proven by scratch-
// building it forced to loose), the store silently no-ops and "caught
// TypeError" never prints.
"use strict";
function store(o, k, v) {
  o[k] = v;
}
function drive(o, n) {
  for (var i = 0; i < n; ++i)
    store(o, 'p', i);
}
var o = {};
drive(o, 300);
print(o.p);

var frozen = Object.freeze({p: 0});
try {
  store(frozen, 'p', 99);
  print('no throw');
} catch (e) {
  print('caught ' + e.name);
}

// CHECK: 299
// CHECK: caught TypeError
// CHECK: JIT counters:
// CHECK: NumByValDemotions: 1{{$}}
