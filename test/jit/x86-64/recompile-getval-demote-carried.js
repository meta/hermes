/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xjit-recompile-threshold=64 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- 8 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xjit-recompile-threshold=64 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- 4096 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit
// UNSUPPORTED: handle_san || gc_malloc
// NumRecompileChecks is off by one under MallocGC (ById inline-cache
// warm/cold timing diverges from the GC these counts were tuned
// against) -- the same pre-existing, tier-independent quirk as its put
// twin recompile-taval-demote-carried.js; unrelated to GetByVal.

// The GetByVal twin of recompile-taval-demote-carried.js. Demotion is
// carried across a recompile, and recording really has stopped (spec:
// "Terminal is terminal" and "Observability for tests").
//
// There is no mid-run counter snapshot, so "this site records nothing
// any more" is proven by an A/B pair instead: the two RUN lines above
// share a byte-identical prefix -- including the PutById declines that
// trigger the recompile -- and differ ONLY in how much post-demotion
// ByVal traffic runs in the suffix, 8 reads versus 4096. Every other
// decline source is quiet there. A still-recording site would count
// those 4096 reads as declines and cross the 64 threshold dozens of
// extra times, so the single pinned NumRecompileChecks value below can
// only hold if the flip survived the recompile.
//
// f's own ByVal site (o[k]) can never grow a tier -- o stays a plain
// object throughout -- so it is the hopeless site under test; p.q is an
// ordinary PutById, used only as an independent progress source, exactly
// as in the put twin.
//
// Phase 1: 256 calls with phase 0. Only f's ByVal site declines (a plain
// object: no tier can ever cover it), so its 4 * 64 = 256th decline
// flips it, at the 4th crossing. p.q has not run at all, so its cache is
// cold and no recompile is triggered here.
//
// Phase 2: 128 calls with phase 1. The ByVal site is demoted and silent
// now, so the only declines are p.q's; its 64th is the 5th crossing, and
// by then its write cache names a class -- progress -- so version 2
// installs. Carry-forward copies the demoted slot VALUE, and version 1
// is retired (its sweep finds nothing left to flip, and would not have
// counted it if it had).
//
// Phase 3: the suffix, phase 0 again, so p.q is untouched and the only
// thing running is the demoted ByVal site.
//
// The loops live in drive() so the suffix's only per-iteration work is
// on locals and parameters; a top-level loop would drive the global
// object's ById sites, which would differ between the two runs.
//
// The key is f's `k` parameter, never a literal: a literal numeric key
// on a READ lowers to GetByIndex (ISel.cpp), which has no tier at all.
function f(o, p, k, v, phase) {
  var r = o[k];
  if (phase) p.q = v;
  return r;
}
function drive(o, p, n, phase) {
  for (var i = 0; i < n; ++i)
    f(o, p, 'p', i, phase);
}
var suffix = +hermescli.getScriptArgs()[0];
var o = {};
var p = {q: 0};
drive(o, p, 256, 0);
drive(o, p, 128, 1);
drive(o, p, suffix, 0);
print('done');

// CHECK: done
// CHECK: JIT counters:
// Exactly the 5 crossings of phases 1 and 2, in BOTH runs.
// CHECK: NumRecompileChecks: 5{{$}}
// CHECK: NumRecompiles: 1{{$}}
// CHECK: NumByValDemotions: 1{{$}}

// BC-LABEL:Function<f>({{.*}}
// BC: GetByVal
