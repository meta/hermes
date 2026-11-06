/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=off %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --match-full-lines --check-prefix=NAN %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit

// The inline typed-array GetByIndex load tier, one kind at a time: every
// element it reads is widened and encoded exactly the way the
// interpreter's TypedArray element getter does. The constant-key twin of
// test/jit/getval-conversions.js, and the file that drives the SHARED
// element-load tail (Emitter::emitTypedArrayElementLoad) through the
// ByIndex address path -- a base register and a constant displacement,
// with no index register at all, the shape the ByVal tier never produces.
//
// Interpreter/JIT equality is enforced by the actual stdout diff above --
// NOT by a shared FileCheck prefix, which would only check that each
// expected substring occurs somewhere, in order. The NaN section is the
// exception: it carries its own full-line FileCheck prefix, so that
// dropping the SHARED tail's NaN canonicalization fails a NAMED check here
// as well as in getval-conversions.js -- the two failing together is what
// proves the tail really is shared.
//
// ARCHITECTURE-INDEPENDENT. Both backends emit the GetByIndex typed-array
// load tier, so on either one these reads exercise the inline tier and its
// decline paths. What this file pins is the read SEMANTICS at a GetByIndex
// site, which must be the same on every backend.
//
// EVERY read below uses a LITERAL uint8 key, never a variable: ISel.cpp
// only lowers a literal number key representable as uint8 to GetByIndex,
// so a variable-keyed twin of any case here would lower to GetByVal and
// exercise the other tier entirely. The BC pins at the bottom are the
// anti-vacuity check, one per loader.
//
// THE CONSTANT-SITE WARM-UP RECIPE. A GetByIndex site's key is baked into
// its code, so one loader can read exactly one index -- there is no
// "loader plus index parameter" to share. A site also cannot borrow
// another site's evidence: the typed-array kind is recorded per site, so
// EACH loader has to be warmed on its OWN literal-K site with the kind it
// is meant to specialize on. Hence three loaders per kind (index 0, a
// middle index, and 255 -- the uint8 ceiling), each warmed with 100
// separate top-level calls, past the 64-decline threshold pinned on the
// RUN lines, and every probe afterwards is its own FRESH top-level call:
// installing a new version swaps the function ENTRY, never a running
// activation, so a probe made from inside the warm-up loop would still be
// running version 1.
//
// What a loader may NOT borrow is evidence; a VIEW it may. A site
// specializes on a CellKind, not on an object, so once (say) f32at0 is
// warmed on a Float32Array it takes its inline path for every
// Float32Array -- which is what lets the NaN sections below reuse the same
// loaders over differently-filled views of the same kind.
//
// Every view starts at byte 8 of its buffer and holds 256 elements, so
// index 255 exists in every kind. The address the tier computes is
// `data_ + offset_ + K * width`, with the last term folded into a constant
// displacement; with a nonzero offset_ and probes at three different
// indices, an implementation that drops the offset term or mis-scales the
// displacement reads the wrong bytes.

function putBytes(buf, at, bytes) {
  var u8 = new Uint8Array(buf);
  for (var i = 0; i < bytes.length; ++i) u8[at + i] = bytes[i];
}

// ---- Int8Array / Uint8Array / Uint8ClampedArray (width 1) ----
// The same three bytes read three ways: 0x7F, 0x80, 0xFF.
function i8at0(a) {
  return a[0];
}
function i8at128(a) {
  return a[128];
}
function i8at255(a) {
  return a[255];
}
function u8at0(a) {
  return a[0];
}
function u8at128(a) {
  return a[128];
}
function u8at255(a) {
  return a[255];
}
function u8cat0(a) {
  return a[0];
}
function u8cat128(a) {
  return a[128];
}
function u8cat255(a) {
  return a[255];
}

function fill1(buf) {
  putBytes(buf, 8 + 0, [0x7f]);
  putBytes(buf, 8 + 128, [0x80]);
  putBytes(buf, 8 + 255, [0xff]);
}

var bufI8 = new ArrayBuffer(8 + 256);
fill1(bufI8);
var taI8 = new Int8Array(bufI8, 8, 256);
for (var i = 0; i < 100; ++i) {
  i8at0(taI8);
  i8at128(taI8);
  i8at255(taI8);
}
print("I8", i8at0(taI8), i8at128(taI8), i8at255(taI8));

var bufU8 = new ArrayBuffer(8 + 256);
fill1(bufU8);
var taU8 = new Uint8Array(bufU8, 8, 256);
for (var i = 0; i < 100; ++i) {
  u8at0(taU8);
  u8at128(taU8);
  u8at255(taU8);
}
print("U8", u8at0(taU8), u8at128(taU8), u8at255(taU8));

// Uint8Clamped is a LOAD-only addition to the tier's kind set: clamping is
// a store-side conversion and leaves no trace in the stored byte, so a
// clamped element reads exactly like a Uint8 one. The same three bytes,
// then a round of stores through the clamped view itself (300 and -5 clamp
// to 255 and 0, 127.5 rounds half-to-even to 128) read back through the
// same specialized sites.
var bufU8C = new ArrayBuffer(8 + 256);
fill1(bufU8C);
var taU8C = new Uint8ClampedArray(bufU8C, 8, 256);
for (var i = 0; i < 100; ++i) {
  u8cat0(taU8C);
  u8cat128(taU8C);
  u8cat255(taU8C);
}
print("U8C", u8cat0(taU8C), u8cat128(taU8C), u8cat255(taU8C));
taU8C[0] = 300;
taU8C[128] = -5;
taU8C[255] = 127.5;
print("U8C clamped", u8cat0(taU8C), u8cat128(taU8C), u8cat255(taU8C));

// ---- Int16Array / Uint16Array (width 2) ----
// 0x7FFF, 0x8000, 0xFFFF, little-endian.
function i16at0(a) {
  return a[0];
}
function i16at128(a) {
  return a[128];
}
function i16at255(a) {
  return a[255];
}
function u16at0(a) {
  return a[0];
}
function u16at128(a) {
  return a[128];
}
function u16at255(a) {
  return a[255];
}

function fill2(buf) {
  putBytes(buf, 8 + 0 * 2, [0xff, 0x7f]);
  putBytes(buf, 8 + 128 * 2, [0x00, 0x80]);
  putBytes(buf, 8 + 255 * 2, [0xff, 0xff]);
}

var bufI16 = new ArrayBuffer(8 + 512);
fill2(bufI16);
var taI16 = new Int16Array(bufI16, 8, 256);
for (var i = 0; i < 100; ++i) {
  i16at0(taI16);
  i16at128(taI16);
  i16at255(taI16);
}
print("I16", i16at0(taI16), i16at128(taI16), i16at255(taI16));

var bufU16 = new ArrayBuffer(8 + 512);
fill2(bufU16);
var taU16 = new Uint16Array(bufU16, 8, 256);
for (var i = 0; i < 100; ++i) {
  u16at0(taU16);
  u16at128(taU16);
  u16at255(taU16);
}
print("U16", u16at0(taU16), u16at128(taU16), u16at255(taU16));

// ---- Int32Array / Uint32Array (width 4) ----
// 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF. The last two are what separate the
// two kinds: as Uint32 they are 2147483648 and 4294967295, both ABOVE
// 2^31, which is exactly the range a signed 32-bit conversion would get
// wrong (it would print them negative).
function i32at0(a) {
  return a[0];
}
function i32at128(a) {
  return a[128];
}
function i32at255(a) {
  return a[255];
}
function u32at0(a) {
  return a[0];
}
function u32at128(a) {
  return a[128];
}
function u32at255(a) {
  return a[255];
}

function fill4(buf) {
  putBytes(buf, 8 + 0 * 4, [0xff, 0xff, 0xff, 0x7f]);
  putBytes(buf, 8 + 128 * 4, [0x00, 0x00, 0x00, 0x80]);
  putBytes(buf, 8 + 255 * 4, [0xff, 0xff, 0xff, 0xff]);
}

var bufI32 = new ArrayBuffer(8 + 1024);
fill4(bufI32);
var taI32 = new Int32Array(bufI32, 8, 256);
for (var i = 0; i < 100; ++i) {
  i32at0(taI32);
  i32at128(taI32);
  i32at255(taI32);
}
print("I32", i32at0(taI32), i32at128(taI32), i32at255(taI32));

var bufU32 = new ArrayBuffer(8 + 1024);
fill4(bufU32);
var taU32 = new Uint32Array(bufU32, 8, 256);
for (var i = 0; i < 100; ++i) {
  u32at0(taU32);
  u32at128(taU32);
  u32at255(taU32);
}
print("U32", u32at0(taU32), u32at128(taU32), u32at255(taU32));

// ---- Float32Array (width 4) ----
// 1.5, -0 and 16777217 (which is not representable in f32 and reads back
// as 16777216). -0 cannot be told apart from 0 by printing the element, so
// 1/element is printed too. A second view of the SAME kind, read through
// the same warmed sites, carries Infinity and -Infinity.
function f32at0(a) {
  return a[0];
}
function f32at128(a) {
  return a[128];
}
function f32at255(a) {
  return a[255];
}

var bufF32 = new ArrayBuffer(8 + 1024);
var taF32 = new Float32Array(bufF32, 8, 256);
taF32[0] = 1.5;
taF32[128] = -0;
taF32[255] = 16777217;
for (var i = 0; i < 100; ++i) {
  f32at0(taF32);
  f32at128(taF32);
  f32at255(taF32);
}
print(
  "F32",
  f32at0(taF32),
  f32at128(taF32),
  1 / f32at128(taF32),
  f32at255(taF32),
);

var bufF32b = new ArrayBuffer(8 + 1024);
var taF32b = new Float32Array(bufF32b, 8, 256);
taF32b[0] = Infinity;
taF32b[255] = -Infinity;
print("F32 inf", f32at0(taF32b), f32at255(taF32b));

// ---- Float64Array (width 8) ----
function f64at0(a) {
  return a[0];
}
function f64at128(a) {
  return a[128];
}
function f64at255(a) {
  return a[255];
}

var bufF64 = new ArrayBuffer(8 + 2048);
var taF64 = new Float64Array(bufF64, 8, 256);
taF64[0] = 1.5;
taF64[128] = -0;
taF64[255] = Infinity;
for (var i = 0; i < 100; ++i) {
  f64at0(taF64);
  f64at128(taF64);
  f64at255(taF64);
}
print(
  "F64",
  f64at0(taF64),
  f64at128(taF64),
  1 / f64at128(taF64),
  f64at255(taF64),
);

var bufF64b = new ArrayBuffer(8 + 2048);
var taF64b = new Float64Array(bufF64b, 8, 256);
taF64b[0] = -Infinity;
taF64b[128] = 4503599627370497; // 2^52+1, exact in f64, not in f32
print("F64 more", f64at0(taF64b), f64at128(taF64b));

// ---- NaN canonicalization (float kinds only) ----
//
// A float element holds whatever bits were written into it, and every NaN
// payload is a legal element value -- but this engine encodes non-numbers
// (pointers, bools, symbols, undefined) as tagged patterns INSIDE the
// double NaN space. A load tier that moved such an element's bits into the
// result verbatim would hand JavaScript a forged non-number. The tier must
// therefore replace any NaN it loads with the canonical quiet NaN before
// encoding, which is what the interpreter's own read does -- and on this
// path that replacement lives in the tail SHARED with the GetByVal tier,
// so corrupting it must fail named checks in both files.
//
// The NaN families are written through an ALIASED integer view (the only
// way to put a chosen bit pattern into a float element -- assigning NaN
// would store the canonical one and prove nothing), into fresh views of
// the same kinds the loaders above were warmed on, so every probe still
// runs the specialized body.
//
// TWO oracles per value, and the SECOND is the load-bearing one:
// `x !== x` is true for any NaN whatsoever, so it would pass even for a
// retained noncanonical payload; `Object.is(x, NaN)` goes through
// isSameValue, which recognizes only the canonical NaN payload (with the
// sign bit masked) before falling back to raw-bit equality, so it is false
// for exactly the patterns canonicalization is there to remove.
//
// ORDER MATTERS HERE, and it is about what a broken tier prints rather
// than about what a correct one does: within each kind the TAG-ALIASING
// pattern comes LAST, because a tier that skipped canonicalization would
// hand `typeof` a forged non-number and could abort the whole run there --
// putting that probe last means the other families' `Object.is` lines are
// already printed by then, so the failure is a NAMED check failing on real
// output rather than only a crash. Float32 comes first, being the kind
// with the extra conversion step.
function nanProbe(name, x) {
  print(name, x, x !== x, Object.is(x, NaN), typeof x);
}

// A float32 NaN is WIDENED to a double before it is encoded, and the
// widening shifts the payload: cvtss2sd maps mantissa bit 22 (the quiet
// bit) to bit 51, so an f32 pattern reaches the engine's tag space only
// when its mantissa bits 22:19 are at least 9 -- 0xFFC00001 does NOT (it
// promotes to 0xFFF8000020000000, just below the space), while 0xFFC80001
// does (0xFFF9000020000000). Pinning only the former would leave the
// forgery family untested on exactly the kind whose conversion could lose
// it.
var bufN32 = new ArrayBuffer(8 + 1024);
var taN32 = new Float32Array(bufN32, 8, 256);
var wordsN32 = new Uint32Array(bufN32);
// Element k of the view starts at byte 8 + k*4, i.e. at 32-bit word 2 + k.
wordsN32[2 + 0] = 0x7fc00001; // quiet, positive, payload 1
wordsN32[2 + 128] = 0xffc00001; // quiet, negative, below the tag space
wordsN32[2 + 255] = 0x7f800001; // signaling
nanProbe("F32 nan 0", f32at0(taN32));
nanProbe("F32 nan 128", f32at128(taN32));
nanProbe("F32 nan 255", f32at255(taN32));

var bufN32b = new ArrayBuffer(8 + 1024);
var taN32b = new Float32Array(bufN32b, 8, 256);
var wordsN32b = new Uint32Array(bufN32b);
wordsN32b[2 + 0] = 0xffc80001; // negative, promotes INTO the tag space
nanProbe("F32 nan tag", f32at0(taN32b));

var bufN64 = new ArrayBuffer(8 + 2048);
var taN64 = new Float64Array(bufN64, 8, 256);
var wordsN64 = new Uint32Array(bufN64);
// Element k starts at byte 8 + k*8, i.e. at 32-bit words 2 + k*2 (low
// half) and 3 + k*2 (high half), little-endian. No widening here, so the
// stored bits ARE the bits that would be encoded.
wordsN64[2 + 0 * 2] = 0x00000001;
wordsN64[3 + 0 * 2] = 0x7ff80000; // quiet, positive, payload 1
wordsN64[2 + 128 * 2] = 0x00000001;
wordsN64[3 + 128 * 2] = 0x7ff00000; // signaling
wordsN64[2 + 255 * 2] = 0x00000000;
wordsN64[3 + 255 * 2] = 0xfff90000; // negative, bits 51:48 == 9: the tag space
nanProbe("F64 nan 0", f64at0(taN64));
nanProbe("F64 nan 128", f64at128(taN64));
nanProbe("F64 nan 255", f64at255(taN64));

// NAN: F32 nan 0 NaN true true number
// NAN-NEXT: F32 nan 128 NaN true true number
// NAN-NEXT: F32 nan 255 NaN true true number
// NAN-NEXT: F32 nan tag NaN true true number
// NAN-NEXT: F64 nan 0 NaN true true number
// NAN-NEXT: F64 nan 128 NaN true true number
// NAN-NEXT: F64 nan 255 NaN true true number

// Anti-GetByVal, anti-vacuity pin: every loader must hold a GetByIndex,
// because a variable-keyed twin would lower to GetByVal and exercise the
// other tier entirely.
// BC-LABEL:Function<i8at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<i8at128>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<i8at255>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u8at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u8at128>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u8at255>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u8cat0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u8cat128>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u8cat255>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<i16at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<i16at128>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<i16at255>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u16at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u16at128>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u16at255>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<i32at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<i32at128>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<i32at255>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u32at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u32at128>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<u32at255>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<f32at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<f32at128>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<f32at255>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<f64at0>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<f64at128>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<f64at255>({{.*}}
// BC: GetByIndex
