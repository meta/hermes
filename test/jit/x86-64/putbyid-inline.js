/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s > %t.force && diff %t.int %t.force
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-asserts -Xjit-emit-type-asserts %s > %t.forcea && diff %t.int %t.forcea
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error %s > %t.warm && diff %t.int %t.warm
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-asserts -Xjit-emit-type-asserts %s > %t.warma && diff %t.int %t.warma
// RUN: %hermes -fno-inline %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// The PutById inline tier and the "safe store" barrier predicate that guards
// it (Emitter::emitSafeStoreOrSlow).
//
// WHAT WOULD BREAK. The interesting store is old-object <- young-pointer:
// the JIT performs it inline, and it is the emitted code, not the runtime,
// that has to dirty the card covering the slot. If it does not, the next
// young-gen collection never scans that card, never sees the only reference
// to the freshly allocated value, and reclaims it. `hot` below makes that a
// real hazard rather than a theoretical one:
//  - `holder` is promoted out of the young generation before the loop starts;
//  - the stored object is allocated inside `storeFresh`, so once that call
//    returns the property is the only reference to it anywhere -- no frame
//    slot of `hot` holds a second one, which is why every RUN line passes
//    -fno-inline;
//  - `churn` then allocates enough to collect the young generation twice
//    before the value is read back.
// With the card-dirty store removed from emitSafeStoreOrSlow, this file
// fails. In a HERMES_SLOW_DEBUG build it fails loudly, on HadesGC's own
// verifyCardTable() assertion; that check is compiled out otherwise, so in
// a release build the same defect surfaces as a wrong printed value, or as
// nothing at all if the reclaimed memory happens not to be reused before
// the read.
//
// THE INLINE TIER NEEDS A WARM CACHE, exactly like the two GetById
// specialization tiers (see props.js). It is emitted only when the site's
// WritePropertyCacheEntry already names a hidden class, which happens once
// the function has run interpreted. Under -Xjit=force every function is
// compiled before its first call, so its write caches are cold and only the
// helper call is emitted. That is why the threshold-mode RUN lines exist,
// and why the SPEC line uses threshold mode. DO NOT convert them to
// -Xjit=force: that would leave the inline tier with no coverage at all
// while this file still passed. That the tier is emitted at all is pinned
// separately, by putbyid-inline-emitted.js -- which this file cannot do,
// because it runs in every heap-value mode and the tier exists only in the
// default one.
//
// WHAT IS NOT COVERED HERE. The predicate's marking-active and
// compaction-active exits need a concurrent old-gen phase to coincide with a
// store, which no deterministic test can arrange. Those paths are covered by
// the JIT stress differential and by running the suite under ASan.

// Allocates garbage. The object is stored into a global array so that it
// genuinely escapes and is genuinely allocated -- a purely local literal is
// removed by the optimizer and collects nothing.
var sink = [null];
function churn(n) {
  for (var i = 0; i < n; ++i)
    sink[0] = {j0: i, j1: i + 1, j2: i + 2, j3: i + 3, j4: i + 4, j5: i + 5};
  return sink[0].j5;
}

// Eight properties: with six direct slots, `a`..`f` are direct and `g`
// and `p` are indirect, so both slot forms of the inline tier are
// reachable from this one shape.
function makeWide(k) {
  return {a: k, b: k + 1, c: k + 2, d: k + 3, e: k + 4, f: k + 5, g: k + 6,
          p: null};
}

// A differently shaped object, used to make `polySet` polymorphic.
function makeNarrow(k) {
  return {p: k, z: k + 1};
}

// The monomorphic write sites under test, each with its own write cache and
// each called often enough to be compiled.

// An indirect slot receiving a pointer to an object allocated right here --
// the card-dirty path. Allocating in this frame rather than the caller's is
// what makes the property the only surviving reference once this returns.
function storeFresh(obj, i) {
  obj.p = {n: i, m: i * 3};
}
// A direct slot receiving a number: a store that needs no card at all, even
// when the target is old.
function storeNum(obj, v) {
  obj.a = v;
}
// An indirect slot receiving a value passed in, so the same site sees both
// pointers and numbers.
function storeAny(obj, v) {
  obj.p = v;
}

// A polymorphic site: two hidden classes reach it, so the guard against the
// single cached class fails about half the time and those stores fall back
// to the runtime helper.
function polySet(obj, v) {
  obj.p = v;
}

// Target and value in the SAME frame register, which is the one shape in
// which the inline tier's `target` and `value` registers alias.
function selfStore(obj) {
  obj.p = obj;
}

// The old-to-young test.
function hot(holder, iters, garbage) {
  var sum = 0;
  for (var i = 0; i < iters; ++i) {
    storeFresh(holder, i);
    storeNum(holder, i + 1);
    churn(garbage);
    sum += holder.p.n + holder.p.m + holder.a;
  }
  return sum;
}

// Promote `holder` into the old generation: it stays reachable from the
// global object across the collections `churn` causes.
var holder = makeWide(100);
churn(60000);

// Warm the sites so that they are compiled with a populated write cache,
// then run the part that would notice a missing card.
print("warm", hot(holder, 6, 100));
print("hot", hot(holder, 8, 45000));

// The promoted object's other properties must be untouched by all of this.
print("fields", holder.a, holder.b, holder.c, holder.d, holder.e, holder.f,
      holder.g);
print("last", holder.p.n, holder.p.m);

// The polymorphic site. Alternate the two shapes so that neither the inline
// guard nor the helper is the only path taken, and check every value.
var wide = makeWide(1);
var narrow = makeNarrow(2);
var acc = 0;
for (var i = 0; i < 200; ++i) {
  polySet(wide, i);
  polySet(narrow, i + 1);
  acc += wide.p + narrow.p;
}
print("poly", acc, wide.p, narrow.p, wide.a, narrow.z);

// Non-pointer values into a promoted object through a warm site.
for (var i = 0; i < 100; ++i)
  storeAny(holder, i * 7);
print("nonptr", holder.p);

// And back to pointers through that same site, to prove it did not get stuck
// on the tag it last saw.
for (var i = 0; i < 10; ++i) {
  storeAny(holder, makeNarrow(i));
  churn(20000);
}
print("again", holder.p.p, holder.p.z);

var self = makeWide(50);
for (var i = 0; i < 50; ++i)
  selfStore(self);
print("self", self.p === self, self.a);

// CHECK-LABEL:warm 81
// CHECK-NEXT:hot 148
// CHECK-NEXT:fields 8 101 102 103 104 105 106
// CHECK-NEXT:last 7 21
// CHECK-NEXT:poly 40000 199 200 1 3
// CHECK-NEXT:nonptr 693
// CHECK-NEXT:again 9 10
// CHECK-NEXT:self true 50
