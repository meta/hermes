/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=COLD %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit
// UNSUPPORTED: gc_malloc
// Under MallocGC `mixed` reports one cold ById site, not two: the same
// pre-existing ById cache warm/cold timing divergence recompile-cold-
// sites.js is gated for, and unrelated to anything here. No handle_san
// gate: unlike recompile-byid-warm.js this file pins no PUT-side
// emission, and all four RUN lines were verified green five times over
// on a HERMESVM_SANITIZE_HANDLES tree.

// That a GetById site with a COLD read cache is reported as a cold ById
// site, and that the report is what a later recompile acts on.
//
// This is the READ half of what recompile-cold-sites.js and
// recompile-byid-warm.js cover for writes, and it is deliberately
// architecture-neutral: both backends push a cold GetById site onto
// coldReadCacheIdxs_ (JitEmitter-property.cpp, the `else if
// (!cacheEntry->clazz...)` arm of emitGetById's specialization gate),
// and nothing about either the count or the recompile it enables is
// per-backend.
//
// Why a file of its own rather than more checks in the two above:
// neither of them can see the read append at all. recompile-cold-sites.js
// wildcards the count (`{{[0-9]+}}`) and its only ById site is a Put, and
// recompile-byid-warm.js pins the Put tier. Deleting the read-side append
// on either backend leaves both of them passing.
//
// Three functions, because three different things are being pinned.
//
// getOnly has ONE ById site and it is a read, so its count is the read
// append's output on its own: a `1` that no write site can account for.
// mixed has one of each, and its `2` is what proves the two appends
// pool into the same counter rather than one shadowing the other.
// Both are called exactly once each, so neither accumulates the
// declines a recompile would need and neither is compiled twice; under
// -Xjit=force a function is compiled on entry, before it has ever run,
// which is what leaves the caches cold at compile time.
//
// warmed is the recompile. Its GetById site is cold at first compile
// like the others', and warms on the very first call; the crossings
// that make the JIT reconsider it come from an UNRELATED site in the
// same body -- the PutByVal into a plain object, which can never
// specialize (the store tiers want a JSArray or a typed array) and so
// calls its recording helper, and counts a decline, on every single
// iteration. That separation is the point: the cold-read append is the
// ONLY thing that can make considerRecompile's progress check return
// true here. The ByVal site is observed but stuck -- `otherSeen` with
// no typed-array kind -- so its own progress term is false, and there
// is no write site in the body at all. With the read append removed
// the body still crosses the threshold just as often and still must
// not be recompiled.
//
// The version-2 window is where the upgrade shows up: version 1 emits
// only the generic read-property-cache sequence for `o.p`, and version
// 2, compiled from the now-monomorphic cache, emits the object
// specialization ahead of it. The version-1 -NOT is bounded to
// warmed's own first-compile window (compile line to un-suffixed
// success line) for the reason recompile-byid-warm.js spells out: the
// global body recompiles chronologically in between and regains
// specializations for its own warmed sites, and an unbounded -NOT
// would see those.
//
// Every read here is literal-keyed, so it lowers to a GetById-family
// opcode (GetByIdShort) and not GetByVal, which has tiers -- and a
// recompile progress source -- of its own. The BC RUN line pins that.

function getOnly(o) {
  return o.p;
}

function mixed(o, v) {
  o.q = v;
  return o.p;
}

function warmed(o, a, v) {
  a[0] = v;
  return o.p;
}

// One class for every `o` below: a second shape would push the read
// cache polymorphic, numGoodChanges would leave 1, and the site would
// no longer be one a recompile could act on.
var o = {p: 1, q: 0};
print(getOnly(o));
print(mixed(o, 2));

// A plain object, not an array: the store tiers decline it forever.
var bag = {};
var t = 0;
for (var i = 0; i < 100; ++i)
  t += warmed(o, bag, i);
print(t);

// OUT: 1
// OUT: 1
// OUT: 100

// A read-only ById site is reported, and reported as one site.
// COLD: JIT successfully compiled FunctionID {{[0-9]+}}, 'getOnly'
// COLD-NEXT: JIT cold ById sites: 1
// One read plus one write in the same body is reported as two.
// COLD: JIT successfully compiled FunctionID {{[0-9]+}}, 'mixed'
// COLD-NEXT: JIT cold ById sites: 2

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'warmed'
// SPEC-NOT: // Get from object specialization
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'warmed'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'warmed' (version 2)
// SPEC: // Get from object specialization
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'warmed' (version 2)
// SPEC: 100

// BC-LABEL:Function<getOnly>({{.*}}
// BC: GetByIdShort {{.*}}"p"
// BC-LABEL:Function<mixed>({{.*}}
// BC: PutByIdLoose {{.*}}"q"
// BC: GetByIdShort {{.*}}"p"
// BC-LABEL:Function<warmed>({{.*}}
// BC: PutByValLoose
// BC: GetByIdShort {{.*}}"p"
