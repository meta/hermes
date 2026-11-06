/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xhermes-internal-test-methods -fno-inline -Xjit=off %s > %t.int
// RUN: %hermes -Xhermes-internal-test-methods -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -Xhermes-internal-test-methods -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --match-full-lines --check-prefix=TA %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit

// The inline GetByIndex load tiers' guards, checked the way
// getval-guards.js checks the ByVal tiers': an actual stdout diff between
// the interpreter and the JIT, not a FileCheck OUT pin, because what
// matters here is that every guard exit reaches the exact same runtime
// behavior the interpreter has -- prototype lookups, accessor calls and
// all -- not any particular instruction sequence. That is pinned
// separately, per backend, by getbyindex-inline-emitted.js.
//
// The typed-array section at the end is the exception: it carries a
// full-line TA prefix as well, because its answers (`undefined` for an
// out-of-bounds or detached read) are produced INLINE by the tier rather
// than by the helper, so a bounds-check defect there should fail a NAMED
// check rather than only perturb a diff.
//
// HermesInternal.detachArrayBuffer needs -Xhermes-internal-test-methods
// (see test/hermes/typedarray-detached.js); every RUN line that runs the
// script runs the detach case below, so each carries the flag.
//
// ARCHITECTURE-INDEPENDENT. arm64 has no GetByIndex load tier of either
// kind; every read below still goes through its bare helper call, which
// the interpreter diff exercises identically. The point of this file is
// the read SEMANTICS at a GetByIndex site, not any inline tier's
// presence.
//
// EVERY read below uses a LITERAL uint8 key, never a variable: ISel.cpp
// only lowers a literal number key representable as uint8 to GetByIndex
// (a non-literal or a key >= 256 lowers to GetByVal, this tier's twin,
// and would silently test the wrong opcode). The BC pins at the bottom
// are the anti-vacuity check for that -- one per function, since a
// mistaken key would otherwise perturb nothing but the bytecode itself.
//
// -fno-inline keeps each reader a real call, exactly as getval-guards.js
// explains for its own dynamic-key readers.

// Holes over a prototype carrying BOTH an indexed data property and an
// indexed accessor, mixed with out-of-range indices reaching the same two
// kinds of prototype property -- the literal-key twin of getval-guards.js's
// first section. `arr` is a fast dense JSArray with two holes (indices 1
// and 2) and length 4; its prototype answers index 1 with a data
// property, index 2 with an accessor, and two further, out-of-range
// indices (10 and 11) the same way. Every literal read below must match
// exactly what the interpreter's getOwnComputed()/prototype-chain path
// would:
//   - index 0, 3: own dense elements -- straight-through inline reads;
//   - index 1: a HOLE that finds a prototype DATA property;
//   - index 2: a HOLE that finds a prototype ACCESSOR;
//   - index 5: OUT OF RANGE with no matching prototype property --
//     undefined;
//   - index 10: OUT OF RANGE, finds a prototype DATA property;
//   - index 11: OUT OF RANGE, finds a prototype ACCESSOR.
var proto = {};
Object.defineProperty(proto, "1", {
  value: "protoData1",
  enumerable: true,
  configurable: true,
});
Object.defineProperty(proto, "2", {
  get: function () {
    return "protoAccessor2";
  },
  enumerable: true,
  configurable: true,
});
Object.defineProperty(proto, "10", {
  value: "protoData10",
  enumerable: true,
  configurable: true,
});
Object.defineProperty(proto, "11", {
  get: function () {
    return "protoAccessor11";
  },
  enumerable: true,
  configurable: true,
});

var arr = [10, /* hole */, /* hole */, 40];
Object.setPrototypeOf(arr, proto);

function readGuards0(a) {
  return a[0];
}
function readGuards1(a) {
  return a[1];
}
function readGuards2(a) {
  return a[2];
}
function readGuards3(a) {
  return a[3];
}
function readGuards5(a) {
  return a[5];
}
function readGuards10(a) {
  return a[10];
}
function readGuards11(a) {
  return a[11];
}
print("dense0", readGuards0(arr));
print("hole1", readGuards1(arr));
print("hole2", readGuards2(arr));
print("dense3", readGuards3(arr));
print("oob5", readGuards5(arr));
print("oob10", readGuards10(arr));
print("oob11", readGuards11(arr));

// Arguments objects: a different CellKind that shares ArrayImpl's storage
// layout, so the tier's exact-JSArrayKind guard must decline it to the
// helper rather than reading it inline -- but the value must still come
// out right. `readGuards3` above already reached a genuine GetByIndex
// site with a JSArray source; reusing a plain reader with an Arguments
// source here drives the same site shape (a literal key) at a different
// CellKind.
function mkargs() {
  return arguments;
}
function readArgAt3(a) {
  return a[3];
}
var ao = mkargs(10, 20, 30, 40, 50);
print("args3", readArgAt3(ao));

// A NONZERO beginIndex_ array: `arrHigh` starts with no indexed storage at
// all, so its FIRST indexed write (JSArray.cpp's
// "indexedStorage hasn't even been allocated" branch) sets beginIndex_ to
// the index written, here 100, with elemCount_ 1. A read at that same
// index (100) must HIT the tier -- the relative index is 0, inside
// [0, elemCount_) -- while a read at a smaller literal index (50) must
// DECLINE: 50 - 100 wraps to a huge unsigned value, which is never below
// elemCount_, so the tier correctly falls back to the helper, which
// resolves it through `protoHigh`, a prototype carrying its own indexed
// data property at 50.
//
// The K=0 read is the case that catches a begin-relative compare broken
// into a direct `K vs elemCount_` test (no beginIndex_ subtraction): 0 is
// less than elemCount_ (1), so a broken compare would ACCEPT it as a hit
// and read storage slot 0 -- which holds the real element at absolute
// index 100 ("hi100") -- instead of correctly declining to the helper,
// which finds no property at index 0 anywhere and answers `undefined`.
var protoHigh = {};
Object.defineProperty(protoHigh, "50", {
  value: "protoData50",
  enumerable: true,
  configurable: true,
});
var arrHigh = [];
Object.setPrototypeOf(arrHigh, protoHigh);
arrHigh[100] = "hi100";

function readHigh100(a) {
  return a[100];
}
function readHigh50(a) {
  return a[50];
}
function readHigh0(a) {
  return a[0];
}
print("beginHit", readHigh100(arrHigh));
print("beginDecline", readHigh50(arrHigh));
print("beginZero", readHigh0(arrHigh));

// An empty, storage-less array: indexedStorage_ is null and elemCount_ is
// 0, so the range test declines for any literal index without ever
// touching the (nonexistent) storage.
var empty = [];
function readEmpty(a) {
  return a[0];
}
print("empty", readEmpty(empty));

// Frozen and sealed dense arrays: freezing/sealing moves the array to a
// frozen/sealed hidden class (and a subsequent defineProperty would make
// its index properties slow, though freezing does not by itself), but the
// read side's tier checks neither -- only the element being non-empty and
// in range -- so both must read exactly like an ordinary dense array,
// including an out-of-range read that must still decline correctly.
var frozen = [1, 2, 3];
Object.freeze(frozen);
function readFrozen1(a) {
  return a[1];
}
function readFrozen5(a) {
  return a[5];
}
print("frozen", readFrozen1(frozen), readFrozen5(frozen));

var sealed = [4, 5, 6];
Object.seal(sealed);
function readSealed2(a) {
  return a[2];
}
function readSealed9(a) {
  return a[9];
}
print("sealed", readSealed2(sealed), readSealed9(sealed));

// ---- the typed-array load tier's own guards ----
//
// THE CONSTANT-SITE WARM-UP RECIPE, as getidx-conversions.js states it: a
// GetByIndex site reads exactly ONE index, and it cannot borrow another
// site's evidence, so each probe below has its OWN literal-K function,
// warmed with 100 separate top-level calls past the 64-decline threshold
// pinned on the RUN lines, and every probe afterwards is its own FRESH
// top-level call (installing a version swaps the function ENTRY, not a
// running activation). What a site specializes on is a CellKind, not an
// object, so a function warmed on one Int32Array takes its inline path for
// every Int32Array -- which is what lets a single warmed K=255 reader
// probe two Int32Array views of different LENGTHS below.
//
// THE EQUALITY BOUNDARY IS THE POINT OF THIS SECTION. Out of bounds on a
// typed array is `undefined` -- not a decline, not a prototype lookup --
// and the tier answers it inline, so `length_ <= K` has to reject K ==
// length_ exactly. Every view whose end a probe sits on has a KNOWN
// NONZERO word planted immediately past it, so an inclusive accept reads
// the planted word and prints a number instead of undefined, failing a
// NAMED TA check rather than merely perturbing a diff. A probe at a
// merely-larger K would not catch that at all.
function readTA3(a) {
  return a[3];
}
function readTA4(a) {
  return a[4];
}
function readTA5(a) {
  return a[5];
}
function readTA255(a) {
  return a[255];
}
function readTA0(a) {
  return a[0];
}
function readDet0(a) {
  return a[0];
}

// A 4-element Int32Array at a NONZERO byte offset, with known nonzero
// words planted at the two element positions immediately past its end.
// K == 3 is length-1, the last in-bounds index; K == 4 is the EQUALITY
// boundary (an inclusive compare would print 287454020); K == 5 is beyond
// it (an inclusive compare would still reject this one, which is exactly
// why the K == 4 case has to exist).
var tbuf = new ArrayBuffer(64);
var tguard = new Int32Array(tbuf, 8, 4);
tguard[0] = 10;
tguard[3] = 40;
new Int32Array(tbuf, 24, 2)[0] = 0x11223344;
new Int32Array(tbuf, 24, 2)[1] = 0x55667788;
for (var i = 0; i < 100; ++i) {
  readTA3(tguard);
  readTA4(tguard);
  readTA5(tguard);
}
print("ta last", readTA3(tguard));
print("ta at length", readTA4(tguard));
print("ta past length", readTA5(tguard));

// K == 0 on an ATTACHED, ZERO-LENGTH view over the same buffer: length_ is
// 0, so even index 0 is out of bounds and must read `undefined`. This is
// the other case an inclusive compare gets wrong -- 0 is not less than 0,
// but it IS `<=` 0 -- and the buffer underneath it holds tguard[0]'s 10,
// so an inclusive accept would print 10.
var tzero = new Int32Array(tbuf, 8, 0);
for (var i = 0; i < 100; ++i) readTA0(tzero);
print("ta zerolen", readTA0(tzero), tzero.length);

// K == 255, the uint8 ceiling, against views of length 255 and 256. The
// same warmed site reads both, since both are Int32Arrays. On the
// 255-element view 255 is the equality boundary and must be `undefined`,
// and a known nonzero word is planted in the element position immediately
// past it; on the 256-element view it is the last in-bounds element.
var bufTop = new ArrayBuffer(8 + 256 * 4);
var t255 = new Int32Array(bufTop, 8, 255);
var t256 = new Int32Array(bufTop, 8, 256);
t256[254] = 254254;
t256[255] = 0x778899aa;
for (var i = 0; i < 100; ++i) readTA255(t256);
print("ta 255 of 256", readTA255(t256));
print("ta 255 of 255", readTA255(t255));

// Detached: a separate, never-modified Int32Array read through its own
// warmed, specialized site. A detached buffer has no data at all, so every
// index reads `undefined` -- including index 0, which was in bounds a
// moment earlier.
var tdet = new Int32Array(4);
tdet[0] = 77;
for (var i = 0; i < 100; ++i) readDet0(tdet);
print("det before", readDet0(tdet), tdet.length);
HermesInternal.detachArrayBuffer(tdet.buffer);
print("det after", readDet0(tdet), tdet.length);

// Kinds the load tier does NOT support (see
// isJitSupportedTypedArrayLoadKind): Float16 would need a software or F16C
// conversion, and the BigInt kinds would have to allocate a BigInt result,
// which no inline tier does. Both must DECLINE to the helper -- no tier is
// ever emitted for them, since the recording helper files them as "other"
// rather than as a typed-array kind -- and still answer exactly, in bounds
// and out.
function readF16at0(a) {
  return a[0];
}
function readF16at1(a) {
  return a[1];
}
function readF16at9(a) {
  return a[9];
}
function readB64at0(a) {
  return a[0];
}
function readB64at1(a) {
  return a[1];
}
function readB64at9(a) {
  return a[9];
}

var f16 = new Float16Array(4);
f16[0] = 1.5;
f16[1] = -0.25;
for (var i = 0; i < 100; ++i) {
  readF16at0(f16);
  readF16at1(f16);
  readF16at9(f16);
}
print("f16", readF16at0(f16), readF16at1(f16), readF16at9(f16));

var b64 = new BigInt64Array(4);
b64[0] = -9223372036854775808n;
b64[1] = 42n;
for (var i = 0; i < 100; ++i) {
  readB64at0(b64);
  readB64at1(b64);
  readB64at9(b64);
}
print("b64", readB64at0(b64), readB64at1(b64), readB64at9(b64));
print("b64 type", typeof readB64at1(b64));

// TA: ta last 40
// TA-NEXT: ta at length undefined
// TA-NEXT: ta past length undefined
// TA-NEXT: ta zerolen undefined 0
// TA-NEXT: ta 255 of 256 2005440938
// TA-NEXT: ta 255 of 255 undefined
// TA-NEXT: det before 77 4
// TA-NEXT: det after undefined 0
// TA-NEXT: f16 1.5 -0.25 undefined
// TA-NEXT: b64 -9223372036854775808 42 undefined
// TA-NEXT: b64 type bigint

// Anti-vacuity pin: every reader above must hold a GetByIndex, or it would
// be testing GetByVal (which has its own, already-pinned tier) instead.
// BC-LABEL:Function<readGuards0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readGuards1>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readGuards2>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readGuards3>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readGuards5>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readGuards10>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readGuards11>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readArgAt3>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readHigh100>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readHigh50>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readHigh0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readEmpty>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readFrozen1>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readFrozen5>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readSealed2>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readSealed9>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readTA3>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readTA4>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readTA5>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readTA255>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readTA0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readDet0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readF16at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readF16at1>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readF16at9>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readB64at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readB64at1>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<readB64at9>({{.*}}
// BC: GetByIndex
