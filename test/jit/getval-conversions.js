/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --match-full-lines --check-prefix=NAN %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit

// The inline typed-array GetByVal load tier, one kind at a time: every
// element it reads is widened and encoded exactly the way the
// interpreter's TypedArray element getter does.
//
// Interpreter/JIT equality is enforced by the actual stdout diff above --
// NOT by a shared FileCheck prefix, which would only check that each
// expected substring occurs somewhere, in order. The NaN section is the
// exception: it carries its own full-line FileCheck prefix, so that
// dropping the tier's NaN canonicalization fails a NAMED check instead of
// only perturbing a diff (see the NaN block below for why the oracle it
// pins is the load-bearing one).
//
// ARCHITECTURE-INDEPENDENT. arm64 has no typed-array load tier; every read
// below still goes through its bare helper call, which the interpreter
// diff exercises identically. What this file pins is the read SEMANTICS at
// a GetByVal site, which must be the same on every backend.
//
// NINE distinct, top-level loader functions -- one per kind the load tier
// supports (see isJitSupportedTypedArrayLoadKind: the store set plus
// Uint8Clamped). Each is its OWN function, never a shared factory or a
// shared helper: one shared loader would give every kind the same
// CodeBlock and the same ByVal site, so only the first kind through it
// would ever be specialized and the rest would poison the site and
// silently test the helper instead of the tier.
//
// EVERY read goes through a variable key (each loader's `i` parameter),
// never a literal: ISel.cpp lowers a uint8 literal number key to
// GetByIndex, which this tier never touches, so a literal-keyed twin of
// any case here would exercise nothing. The BC prefix pins that, per
// loader, in the bytecode itself.
function loadI8(a, i) {
  return a[i];
}
function loadU8(a, i) {
  return a[i];
}
function loadU8C(a, i) {
  return a[i];
}
function loadI16(a, i) {
  return a[i];
}
function loadU16(a, i) {
  return a[i];
}
function loadI32(a, i) {
  return a[i];
}
function loadU32(a, i) {
  return a[i];
}
function loadF32(a, i) {
  return a[i];
}
function loadF64(a, i) {
  return a[i];
}

// Every view below starts at byte 8 of its buffer and holds 4 elements,
// and every probe reads a NONZERO index. The address the tier computes is
// `data_ + offset_ + idx * width`, so a view at offset 0 read at index 0
// would validate neither the `offset_` term nor the index scale; with both
// nonzero, an implementation that drops either one reads the wrong bytes.
//
// The warm-up runs 100 separate top-level calls (the threshold is 64,
// pinned on the RUN lines) so that the recompile installs the specialized
// body; every probe afterwards is its own fresh top-level call and
// therefore genuinely runs in that body. Warm-up reads index 0 so it
// cannot be confused with a probe.

// ---- Int8Array / Uint8Array / Uint8ClampedArray (width 1) ----
// The same four bytes read three ways: 0x00, 0x7F, 0x80, 0xFF.
function fillBytes1(buf, at) {
  var u8 = new Uint8Array(buf);
  u8[at] = 0x00;
  u8[at + 1] = 0x7f;
  u8[at + 2] = 0x80;
  u8[at + 3] = 0xff;
}

var bufI8 = new ArrayBuffer(64);
fillBytes1(bufI8, 8);
var taI8 = new Int8Array(bufI8, 8, 4);
for (var i = 0; i < 100; ++i) loadI8(taI8, 0);
print("I8", loadI8(taI8, 0), loadI8(taI8, 1), loadI8(taI8, 2), loadI8(taI8, 3));

var bufU8 = new ArrayBuffer(64);
fillBytes1(bufU8, 8);
var taU8 = new Uint8Array(bufU8, 8, 4);
for (var i = 0; i < 100; ++i) loadU8(taU8, 0);
print("U8", loadU8(taU8, 0), loadU8(taU8, 1), loadU8(taU8, 2), loadU8(taU8, 3));

// Uint8Clamped is a LOAD-only addition to the tier's kind set: clamping is
// a store-side conversion and leaves no trace in the stored byte, so a
// clamped element reads exactly like a Uint8 one. These are the same four
// bytes as above, plus a store through the clamped view itself (300 and
// -5 clamp to 255 and 0, 127.5 rounds half-to-even to 128) read back
// through the specialized load site.
var bufU8C = new ArrayBuffer(64);
fillBytes1(bufU8C, 8);
var taU8C = new Uint8ClampedArray(bufU8C, 8, 4);
for (var i = 0; i < 100; ++i) loadU8C(taU8C, 0);
print(
  "U8C",
  loadU8C(taU8C, 0),
  loadU8C(taU8C, 1),
  loadU8C(taU8C, 2),
  loadU8C(taU8C, 3),
);
taU8C[1] = 300;
taU8C[2] = -5;
taU8C[3] = 127.5;
print("U8C clamped", loadU8C(taU8C, 1), loadU8C(taU8C, 2), loadU8C(taU8C, 3));

// ---- Int16Array / Uint16Array (width 2) ----
// 0x0000, 0x7FFF, 0x8000, 0xFFFF, little-endian.
function fillBytes2(buf, at) {
  var u8 = new Uint8Array(buf);
  u8[at] = 0x00;
  u8[at + 1] = 0x00;
  u8[at + 2] = 0xff;
  u8[at + 3] = 0x7f;
  u8[at + 4] = 0x00;
  u8[at + 5] = 0x80;
  u8[at + 6] = 0xff;
  u8[at + 7] = 0xff;
}

var bufI16 = new ArrayBuffer(64);
fillBytes2(bufI16, 8);
var taI16 = new Int16Array(bufI16, 8, 4);
for (var i = 0; i < 100; ++i) loadI16(taI16, 0);
print(
  "I16",
  loadI16(taI16, 0),
  loadI16(taI16, 1),
  loadI16(taI16, 2),
  loadI16(taI16, 3),
);

var bufU16 = new ArrayBuffer(64);
fillBytes2(bufU16, 8);
var taU16 = new Uint16Array(bufU16, 8, 4);
for (var i = 0; i < 100; ++i) loadU16(taU16, 0);
print(
  "U16",
  loadU16(taU16, 0),
  loadU16(taU16, 1),
  loadU16(taU16, 2),
  loadU16(taU16, 3),
);

// ---- Int32Array / Uint32Array (width 4) ----
// 0x00000000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF. The last two are what
// separate the two kinds: as Uint32 they are 2147483648 and 4294967295,
// both ABOVE 2^31, which is exactly the range a signed 32-bit conversion
// would get wrong (it would print them negative).
function fillBytes4(buf, at) {
  var u8 = new Uint8Array(buf);
  var words = [
    [0x00, 0x00, 0x00, 0x00],
    [0xff, 0xff, 0xff, 0x7f],
    [0x00, 0x00, 0x00, 0x80],
    [0xff, 0xff, 0xff, 0xff],
  ];
  for (var w = 0; w < 4; ++w)
    for (var b = 0; b < 4; ++b) u8[at + w * 4 + b] = words[w][b];
}

var bufI32 = new ArrayBuffer(64);
fillBytes4(bufI32, 8);
var taI32 = new Int32Array(bufI32, 8, 4);
for (var i = 0; i < 100; ++i) loadI32(taI32, 0);
print(
  "I32",
  loadI32(taI32, 0),
  loadI32(taI32, 1),
  loadI32(taI32, 2),
  loadI32(taI32, 3),
);

var bufU32 = new ArrayBuffer(64);
fillBytes4(bufU32, 8);
var taU32 = new Uint32Array(bufU32, 8, 4);
for (var i = 0; i < 100; ++i) loadU32(taU32, 0);
print(
  "U32",
  loadU32(taU32, 0),
  loadU32(taU32, 1),
  loadU32(taU32, 2),
  loadU32(taU32, 3),
);

// ---- Float32Array (width 4) ----
// 1.5, -0, Infinity, and 16777217 (which is not representable in f32 and
// reads back as 16777216). -0 cannot be told apart from 0 by printing the
// element, so 1/element is printed too.
var bufF32 = new ArrayBuffer(64);
var taF32 = new Float32Array(bufF32, 8, 4);
taF32[0] = 1.5;
taF32[1] = -0;
taF32[2] = Infinity;
taF32[3] = 16777217;
for (var i = 0; i < 100; ++i) loadF32(taF32, 0);
print(
  "F32",
  loadF32(taF32, 0),
  loadF32(taF32, 1),
  1 / loadF32(taF32, 1),
  loadF32(taF32, 2),
  loadF32(taF32, 3),
);

// ---- Float64Array (width 8) ----
var bufF64 = new ArrayBuffer(64);
var taF64 = new Float64Array(bufF64, 8, 4);
taF64[0] = 1.5;
taF64[1] = -0;
taF64[2] = Infinity;
taF64[3] = -Infinity;
for (var i = 0; i < 100; ++i) loadF64(taF64, 0);
print(
  "F64",
  loadF64(taF64, 0),
  loadF64(taF64, 1),
  1 / loadF64(taF64, 1),
  loadF64(taF64, 2),
  loadF64(taF64, 3),
);

// ---- NaN canonicalization (float kinds only) ----
//
// A float element holds whatever bits were written into it, and every NaN
// payload is a legal element value -- but this engine encodes non-numbers
// (pointers, bools, symbols, undefined) as tagged patterns INSIDE the
// double NaN space. A load tier that moved such an element's bits into the
// result verbatim would hand JavaScript a forged non-number. The tier must
// therefore replace any NaN it loads with the canonical quiet NaN before
// encoding, which is what the interpreter's own read does.
//
// The NaN families are written through an ALIASED integer view (the only
// way to put a chosen bit pattern into a float element -- assigning NaN
// would store the canonical one and prove nothing):
//   - a positive quiet NaN with a nonzero payload;
//   - a negative quiet NaN BELOW the tag space;
//   - a signaling NaN;
//   - a NEGATIVE NaN whose bits 51:48 land in the engine's tag space, the
//     family a verbatim encode would actually forge a non-number out of.
//
// TWO oracles per value, and the SECOND is the load-bearing one:
// `x !== x` is true for any NaN whatsoever, so it would pass even for a
// retained noncanonical payload; `Object.is(x, NaN)` goes through
// isSameValue, which recognizes only the canonical NaN payload (with the
// sign bit masked) before falling back to raw-bit equality, so it is false
// for exactly the patterns canonicalization is there to remove.
//
// ORDER MATTERS HERE, in two ways, and both are about what a broken tier
// prints rather than about what a correct one does:
//   - Float32 comes FIRST, because it is the kind with the extra
//     conversion step and so the one whose oracle is most worth seeing
//     fail on its own.
//   - Within each kind the TAG-ALIASING pattern comes LAST. A tier that
//     skipped canonicalization would hand `typeof` a forged non-number and
//     abort the whole run there; putting that probe last means the other
//     families' `Object.is` lines are already printed by then, so the
//     failure is a NAMED check failing on real output rather than only a
//     crash.
function nanProbe(name, ld, ta, i) {
  var x = ld(ta, i);
  print(name, i, x, x !== x, Object.is(x, NaN), typeof x);
}

var wordsF32 = new Uint32Array(bufF32);
// Element k of taF32 starts at byte 8 + k*4, i.e. at 32-bit word 2 + k.
//
// A float32 NaN is WIDENED to a double before it is encoded, and the
// widening shifts the payload: cvtss2sd maps mantissa bit 22 (the quiet
// bit) to bit 51, so an f32 pattern reaches the engine's tag space only
// when its mantissa bits 22:19 are at least 9 -- 0xFFC00001 does NOT
// (it promotes to 0xFFF8000020000000, just below the space), while
// 0xFFC80001 does (0xFFF9000020000000). Pinning only the former would
// leave the forgery family untested on exactly the kind whose conversion
// could lose it.
wordsF32[2] = 0x7fc00001; // quiet, positive, payload 1
wordsF32[3] = 0xffc00001; // quiet, negative, below the tag space
wordsF32[4] = 0x7f800001; // signaling
wordsF32[5] = 0xffc80001; // negative, promotes INTO the tag space
nanProbe("F32 nan", loadF32, taF32, 0);
nanProbe("F32 nan", loadF32, taF32, 1);
nanProbe("F32 nan", loadF32, taF32, 2);
nanProbe("F32 nan", loadF32, taF32, 3);

var wordsF64 = new Uint32Array(bufF64);
// Element k of taF64 starts at byte 8 + k*8, i.e. at 32-bit words
// 2 + k*2 (low half) and 3 + k*2 (high half), little-endian. No widening
// here, so the stored bits ARE the bits that would be encoded.
wordsF64[2] = 0x00000001;
wordsF64[3] = 0x7ff80000; // quiet, positive, payload 1
wordsF64[4] = 0x00000001;
wordsF64[5] = 0x7ff00000; // signaling
wordsF64[6] = 0x00000000;
wordsF64[7] = 0xfff90000; // negative, bits 51:48 == 9: the tag space
nanProbe("F64 nan", loadF64, taF64, 0);
nanProbe("F64 nan", loadF64, taF64, 1);
nanProbe("F64 nan", loadF64, taF64, 2);

// NAN: F32 nan 0 NaN true true number
// NAN: F32 nan 1 NaN true true number
// NAN: F32 nan 2 NaN true true number
// NAN: F32 nan 3 NaN true true number
// NAN: F64 nan 0 NaN true true number
// NAN: F64 nan 1 NaN true true number
// NAN: F64 nan 2 NaN true true number

// Anti-GetByIndex, anti-vacuity pin: every loader must hold a GetByVal,
// because a literal-keyed twin would lower to GetByIndex and exercise none
// of this tier.
// BC-LABEL:Function<loadI8>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<loadU8>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<loadU8C>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<loadI16>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<loadU16>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<loadI32>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<loadU32>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<loadF32>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<loadF64>({{.*}}
// BC: GetByVal
