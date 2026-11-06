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
// UNSUPPORTED: handle_san

// UNSUPPORTED under HERMESVM_SANITIZE_HANDLES (the `handle_san` lit feature):
// that build moves the heap on every allocation, so growing `big` to 600000
// elements is quadratic, and one -Xjit=force run takes over 25 minutes on a
// MallocGC tree. The behavioral coverage this file adds is the old-to-young
// store, which handle sanitization does not exercise any differently.

// The PutByVal inline fast array store: an element write that lands in an
// existing, non-hole slot of a fast JSArray is performed inline, guarded by
// the "safe store" barrier predicate (Emitter::emitSafeStoreOrSlow).
//
// WHAT WOULD BREAK. The interesting store is old-array <- young-pointer:
// the JIT performs it inline, and it is the emitted code, not the runtime,
// that has to dirty the card covering the element. If it does not, the next
// young-gen collection never scans that card, never sees the only reference
// to the freshly allocated value, and reclaims it. `hot` below makes that a
// real hazard rather than a theoretical one:
//  - `holder` is promoted out of the young generation before the loop starts;
//  - the stored object is allocated inside `storeFresh`, so once that call
//    returns the element is the only reference to it anywhere -- no frame
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
// UNLIKE PutById, THIS TIER NEEDS NO WARM CACHE. It is a pure type and shape
// guard on the values in registers, so it is emitted for every PutByVal
// site, and -Xjit=force exercises it just as threshold mode does. Both are
// run above anyway, since the two modes reach the emitter with different
// register assignments.
//
// ARCHITECTURE-INDEPENDENT. This file checks values, never instructions, so
// it lives here and runs on every backend, including one whose element store
// still goes through the helper. That the tier is emitted at all is pinned
// separately, per backend: x86-64/putbyval-inline-emitted.js and
// putbyval-inline-emitted-arm64.js.
//
// EVERY CHECK THE RUNTIME MAKES IS MADE HERE TOO, or the store declines to
// the helper. The list, from putByValWithReceiver_RJS (StaticH.cpp), which
// is itself JSObject::putComputedWithReceiver_RJS's first branch: the target
// is an object; the key is a double that is an exact uint32 other than
// 0xFFFFFFFF; target == receiver (always true for PutByVal); the target
// hasFastIndexProperties() (its hidden class has indexedStorage and no
// index-like properties); haveOwnIndexed, which is BOTH "the index is in the
// storage range" AND "the element is not a hole"; and, inside
// _setOwnIndexedImpl, a hidden class that is not frozen. Each has a case
// below, and each of the hole check, the frozen check and the storage-size
// gate has been confirmed to make this file fail when removed.
//
// WHAT IS NOT COVERED HERE. The predicate's marking-active and
// compaction-active exits need a concurrent old-gen phase to coincide with a
// store, which no deterministic test can arrange. Those paths are covered by
// the JIT stress differential and by running the suite under ASan.

// Allocates garbage. The object is stored into a global so that it genuinely
// escapes and is genuinely allocated.
var sink = [null];
function churn(n) {
  for (var i = 0; i < n; ++i)
    sink[0] = {j0: i, j1: i + 1, j2: i + 2, j3: i + 3, j4: i + 4, j5: i + 5};
  return sink[0].j5;
}

// The write sites under test. Each is its own function so that each is
// compiled with its own registers.

// An in-range element receiving a pointer to an object allocated right here
// -- the card-dirty path. Allocating in this frame rather than the caller's
// is what makes the element the only surviving reference once this returns.
function storeFresh(arr, i) {
  arr[2] = {n: i, m: i * 3};
}
// An in-range element receiving a number: a store that needs no card at all,
// even when the target is old.
function storeNum(arr, i, v) {
  arr[i] = v;
}
// A generic store, used for every "must decline" case below.
function store(o, k, v) {
  o[k] = v;
}

// The old-to-young test.
function hot(arr, iters, garbage) {
  var sum = 0;
  for (var i = 0; i < iters; ++i) {
    storeFresh(arr, i);
    storeNum(arr, 0, i + 1);
    churn(garbage);
    sum += arr[2].n + arr[2].m + arr[0];
  }
  return sum;
}

// Promote `holder` into the old generation: it stays reachable from the
// global object across the collections `churn` causes.
var holder = [10, 11, 12, 13];
churn(60000);

print("warm", hot(holder, 6, 100));
print("hot", hot(holder, 8, 45000));
print("fields", holder[0], holder[1], holder[3], holder.length);
print("last", holder[2].n, holder[2].m);

// Out of range on both sides. Both must reach the helper, which grows the
// storage (and, for a JSArray, the .length property) correctly.
var grow = [0, 1, 2];
for (var i = 3; i < 40; ++i)
  store(grow, i, i * 2);
print("growRight", grow.length, grow[3], grow[39], grow[2]);

var sparse = [];
sparse[100] = 1;
for (var i = 99; i >= 90; --i)
  store(sparse, i, i);
print("growLeft", sparse.length, sparse[90], sparse[99], sparse[100],
      sparse[89]);

// Storage that does not start at index 0. `off[100] = "a"` on an empty
// array places index 100 first in storage, so the inline store must rebase
// the key by beginIndex_ before its range check. Without the rebase, the
// store to index 1 below would pass the range check and overwrite the slot
// of index 101 instead of creating index 1.
var off = [];
off[100] = "a";
off[101] = "b";
for (var i = 0; i < 30; ++i)
  store(off, 100, i);
store(off, 1, "z");
print("offset", off[1], off[100], off[101], off.length);

// Keys that are not array indices. Every one of them must become a named
// property instead of an element.
var keys = [0, 1, 2, 3];
store(keys, 1.5, "frac");
store(keys, -1, "neg");
store(keys, "x", "str");
store(keys, NaN, "nan");
store(keys, 4294967295, "max");     // 0xFFFFFFFF is NOT an array index
store(keys, 4294967296, "over");    // 2^32
store(keys, -0, "negzero");         // -0 IS index 0
print("keys", keys.length, keys[0], keys[1], keys[2], keys[3]);
print("named", keys[1.5], keys[-1], keys.x, keys[NaN], keys[4294967295],
      keys[4294967296]);

// A plain object with an index-like own property is not a JSArray, so the
// cell-kind guard must send it to the helper.
var obj = {0: "a", 1: "b"};
for (var i = 0; i < 30; ++i)
  store(obj, 1, i);
print("plainObj", obj[0], obj[1]);

// An Arguments object shares ArrayImpl's indexed storage but is a different
// CellKind, so it must decline too.
function args1() {
  for (var i = 0; i < 30; ++i)
    store(arguments, 1, i);
  return arguments[0] + ":" + arguments[1];
}
print("arguments", args1("a", "b"));

// A typed array is yet another indexed-storage kind, and its element write
// coerces the value. Declining is what keeps that coercion.
var ta = new Int8Array(4);
for (var i = 0; i < 30; ++i)
  store(ta, 1, 300);
print("typed", ta[0], ta[1], ta.length);

// Frozen: _setOwnIndexedImpl refuses the write, and freezing does NOT make
// the index properties slow, so only the explicit frozen check keeps this
// correct.
var frozen = [1, 2, 3];
Object.freeze(frozen);
for (var i = 0; i < 30; ++i)
  store(frozen, 1, i);
print("frozen", frozen[0], frozen[1], frozen[2]);

// Sealed, by contrast, still allows writes to existing elements.
var sealed = [1, 2, 3];
Object.seal(sealed);
for (var i = 0; i < 30; ++i)
  store(sealed, 1, i);
print("sealed", sealed[0], sealed[1], sealed[2]);

// A large array whose indexed storage is bigger than one heap segment unit,
// so the storage cell is allocated in a multi-unit JumboHeapSegment. The
// card status array of such a segment is out of line and the cell spans
// several units, so `loc & ~(unit-1)` is not the segment start and the
// inline card-dirty store would write into the cell's own payload while the
// real card stayed clean. The storage-size gate
// (RuntimeOffsets::kMaxInlineStorage) is what declines this; the barrier
// predicate's own segment-size test cannot, and this array is filled so as
// to prove it cannot.
//
// The fill value is the denormal whose bit pattern is 0x0000000100010000:
// bits 47..32 and bits 31..16 are both 1, so whichever way a unit boundary
// falls inside the element array, the 16-bit word the predicate reads at
// (unit start + 4) -- where it expects SHSegmentInfo::shiftedSegmentSize --
// reads as 1 and lets the store through. Remove the storage-size gate and
// this section fails on HadesGC's verifyCardTable() assertion.
var pad = Number.MIN_VALUE * (4294967296 + 65536);
var big = [];
for (var i = 0; i < 600000; ++i)
  big[i] = pad;
churn(30000);
for (var i = 0; i < 20; ++i)
  storeFresh(big, i);          // index 2: inside the first unit
// Several elements far past the first unit, each holding the only reference
// to a freshly allocated object.
function storeFar(arr, i) {
  arr[599990 + i] = {n: i, m: i * 5};
}
for (var i = 0; i < 10; ++i)
  storeFar(big, i);
churn(30000);
print("big", big.length, big[2].n, big[2].m, big[599999].n, big[599999].m,
      big[100] === pad);

// Holes. haveOwnIndexed() reports false for an `empty` element, so the
// runtime does NOT take the fast path for one: it resolves the property
// normally, which can find a setter on the prototype. Skipping that check
// inline would silently overwrite the hole instead of calling the setter.
var setterHits = 0;
var setterLast = 0;
Object.defineProperty(Array.prototype, "3", {
  set: function (v) { ++setterHits; setterLast = v; },
  get: function () { return "proto3"; },
  configurable: true,
});
function holeStore(arr, v) {
  arr[3] = v;
}
var holey = [0, 1, 2, , 4];
for (var i = 0; i < 30; ++i)
  holeStore(holey, i);
print("hole", setterHits, setterLast, holey[3], holey[4]);
// A non-hole element at the same index in a different array still takes the
// inline path and does not reach the setter.
var dense = [0, 1, 2, 3, 4];
for (var i = 0; i < 30; ++i)
  holeStore(dense, i);
print("dense", setterHits, dense[3], dense[4]);

// CHECK-LABEL:warm 81
// CHECK-NEXT:hot 148
// CHECK-NEXT:fields 8 11 13 4
// CHECK-NEXT:last 7 21
// CHECK-NEXT:growRight 40 6 78 2
// CHECK-NEXT:growLeft 101 90 99 1 undefined
// CHECK-NEXT:offset z 29 b 102
// CHECK-NEXT:keys 4 negzero 1 2 3
// CHECK-NEXT:named frac neg str nan max over
// CHECK-NEXT:plainObj a 29
// CHECK-NEXT:arguments a:29
// CHECK-NEXT:typed 0 44 4
// CHECK-NEXT:frozen 1 2 3
// CHECK-NEXT:sealed 1 29 3
// CHECK-NEXT:big 600000 19 57 9 45 true
// CHECK-NEXT:hole 30 29 proto3 4
// CHECK-NEXT:dense 30 29 4
