/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// REQUIRES: jit-arch-arm64

// That the arm64 typed-array load tier is actually EMITTED for every kind
// test/jit/getval-conversions.js exercises, and that each kind's element
// access really is the instruction pair its width, signedness and number
// class call for.
//
// This file exists because its companion cannot do this.
// getval-conversions.js is ARCHITECTURE-NEUTRAL, so it can only check
// values, and identical values come out whether a kind is specialized
// inline or answered by the helper. Without an emission pin, dropping a
// kind from isJitSupportedTypedArrayLoadKind (or from
// emitTypedArrayElementLoad()'s switch) would silently move that kind
// onto the helper and leave the ldrsb-versus-ldrb-versus-scvtf-versus-
// ucvtf distinctions untested rather than failing. Its x86-64
// counterpart is test/jit/x86-64/getval-conversions-emitted.js, which
// pins six kinds because three of them are pinned by other x86-64 files;
// on arm64 no other file pins any typed-array LOAD kind, so all NINE are
// here.
//
// Float16 and the BigInt kinds are NOT supported and must never be
// specialized; test/jit/getval-guards.js covers their decline.
//
// One function per kind, each warmed with 100 separate top-level calls --
// past the 64-decline threshold pinned on the RUN lines -- so that a
// version 2 exists to inspect. A shared loader would specialize only its
// first kind and poison the rest. Every view sits at a NONZERO byte
// offset and every probe reads a nonzero index, and the values are
// diffed against the interpreter, so the instructions pinned below are
// checked to be correct as well as present.
//
// Every read goes through a variable key: a literal-keyed twin would
// lower to GetByIndex, which is a different opcode with tiers of its own.
//
// Nothing here is heap-value-mode dependent, so there is one prefix: a
// typed array's elements are raw machine values in a malloc'd buffer, not
// SmallHermesValues, and the encode of the result is a number
// HermesValue in every mode.
//
// UNLIKE putbyval-inline-emitted-arm64.js, and like the other two arm64
// load-tier pin files, this one carries no "unsupported under
// handle_san" annotation. That annotation exists there because
// HERMESVM_SANITIZE_HANDLES changes emitted CODE on the PUT side: it
// makes canInlineCompressibleOrNumberHV64() refuse every number, so
// emit_shv_encode_or_slow() (JitEmitter-internal.cpp, the one and only
// place in either backend's emitter that mentions the macro) takes its
// HERMESVM_SANITIZE_HANDLES branch and declines the inline-number case
// with a bare jump instead of emitting the encode sequence
// putbyval-inline-emitted-arm64.js pins. The typed-array LOAD tier
// never reaches that path: it reads raw machine values out of a
// malloc'd buffer and writes a plain number HermesValue, with no
// SmallHermesValue encode anywhere in it. So this file's output is
// identical whether or not Handle-San is on, and gating it would just
// hide all nine kinds from that configuration's suite for no reason.
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

var buf = new ArrayBuffer(256);

// Each value below is chosen so that the WRONG instruction prints a
// different number: an unsigned kind read with a sign-extending load
// would come out negative, a signed one read with a zero-extending load
// would come out large and positive, and a Uint32 converted with scvtf
// instead of ucvtf would print a negative number instead of one above
// 2^31.
var i8 = new Int8Array(buf, 8, 4);
i8[1] = -100;
for (var i = 0; i < 100; ++i) loadI8(i8, 0);
print("I8", loadI8(i8, 1));

var u8 = new Uint8Array(buf, 16, 4);
u8[1] = 200;
for (var i = 0; i < 100; ++i) loadU8(u8, 0);
print("U8", loadU8(u8, 1));

var u8c = new Uint8ClampedArray(buf, 24, 4);
u8c[1] = 200;
for (var i = 0; i < 100; ++i) loadU8C(u8c, 0);
print("U8C", loadU8C(u8c, 1));

var i16 = new Int16Array(buf, 32, 4);
i16[1] = -30000;
for (var i = 0; i < 100; ++i) loadI16(i16, 0);
print("I16", loadI16(i16, 1));

var u16 = new Uint16Array(buf, 48, 4);
u16[1] = 60000;
for (var i = 0; i < 100; ++i) loadU16(u16, 0);
print("U16", loadU16(u16, 1));

var i32 = new Int32Array(buf, 64, 4);
i32[1] = -2000000000;
for (var i = 0; i < 100; ++i) loadI32(i32, 0);
print("I32", loadI32(i32, 1));

var u32 = new Uint32Array(buf, 96, 4);
u32[1] = 4000000000;
for (var i = 0; i < 100; ++i) loadU32(u32, 0);
print("U32", loadU32(u32, 1));

var f32 = new Float32Array(buf, 128, 4);
f32[1] = -2.5;
for (var i = 0; i < 100; ++i) loadF32(f32, 0);
print("F32", loadF32(f32, 1));

var f64 = new Float64Array(buf, 160, 4);
f64[1] = 2.5;
for (var i = 0; i < 100; ++i) loadF64(f64, 0);
print("F64", loadF64(f64, 1));

// Each section is anchored on its function's version-2 compile banner
// rather than on a CHECK-LABEL: a recompiled function prints its name
// line once per version, so a label on it would not be unique. Each
// section ends with the value that kind produces, which the dump prints
// right after that kind's compiles and before the next kind's -- so the
// instructions and the number they compute are pinned together, and a
// section's pins cannot silently drift into the next section's code.
//
// Every convert is pinned with SPEC-NEXT, not SPEC, and that is what
// makes these pins mean anything: the key round trip emits its own
// `ucvtf` and `fcmp` at every kind and in both tiers, so a floating
// pattern that failed on the element convert would simply scan forward
// and match one of those instead. Requiring the convert to be the
// instruction IMMEDIATELY after the element load leaves it nothing else
// to match.
//
// Each element load is in turn pinned by its full addressing form: a
// base register plus a W INDEX register extended `uxtw` by this kind's
// log width, which is the second per-kind distinguisher (the scale IS
// the width) and is emitted by no other load in the tier -- the bounds,
// offset and buffer loads all use immediate displacements. A byte access
// prints with no `uxtw` suffix at all, because its scale is zero.

// Int8Array is CellKind 34 (0x22). A SIGN-extending byte load, then the
// signed integer-to-double convert.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadI8' (version 2)
// SPEC: // Inline typed array load (kind 34)
// SPEC: ldrsb {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, {{w[0-9]+}}]
// SPEC-NEXT: scvtf [[V:d[0-9]+]], {{w[0-9]+}}
// The encode: a number HermesValue is the double's raw bits, and an
// integer convert can never produce a NaN, so there is no
// canonicalization here -- only the move.
// SPEC-NEXT: fmov {{x[0-9]+}}, [[V]]
// SPEC: I8 -100

// Uint8Array is CellKind 33 (0x21): the same width, ZERO-extended. The
// value 200 is what separates the two loads behaviorally.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadU8' (version 2)
// SPEC: // Inline typed array load (kind 33)
// SPEC: ldrb {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, {{w[0-9]+}}]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: U8 200

// Uint8ClampedArray is CellKind 35 (0x23), and reads exactly like Uint8:
// clamping is a store-side conversion and leaves no trace in the stored
// byte.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadU8C' (version 2)
// SPEC: // Inline typed array load (kind 35)
// SPEC: ldrb {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, {{w[0-9]+}}]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: U8C 200

// Int16Array is CellKind 37 (0x25): a SIGN-extending halfword load,
// scaled by 2.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadI16' (version 2)
// SPEC: // Inline typed array load (kind 37)
// SPEC: ldrsh {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, {{w[0-9]+}} uxtw 1]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: I16 -30000

// Uint16Array is CellKind 36 (0x24): the same width, ZERO-extended.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadU16' (version 2)
// SPEC: // Inline typed array load (kind 36)
// SPEC: ldrh {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, {{w[0-9]+}} uxtw 1]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: U16 60000

// Int32Array is CellKind 39 (0x27): a word load, scaled by 4, and the
// SIGNED convert.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadI32' (version 2)
// SPEC: // Inline typed array load (kind 39)
// SPEC: ldr {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, {{w[0-9]+}} uxtw 2]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: I32 -2000000000

// Uint32Array is CellKind 38 (0x26), and is the one integer kind whose
// values can exceed INT32_MAX. Its load is the same word load; what
// differs -- and the whole reason this kind gets its own case -- is the
// UNSIGNED convert. arm64 has it as one instruction, so unlike x86-64
// (which has no unsigned convert and has to widen to a signed 64-bit
// value first) there is no width trick to look for here, just ucvtf
// where every other integer kind has scvtf. The value below is what it
// buys: with scvtf it would print as a negative number.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadU32' (version 2)
// SPEC: // Inline typed array load (kind 38)
// SPEC: ldr {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, {{w[0-9]+}} uxtw 2]
// SPEC-NEXT: ucvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: U32 4000000000

// Float32Array is CellKind 41 (0x29): the element is loaded through the
// register's SINGLE-precision view and WIDENED, and then -- unlike every
// integer kind above -- canonicalized. A float element may hold any NaN
// bit pattern, and this engine's tag space lives inside the NaN space, so
// encoding such an element verbatim would forge a non-number; the
// self-compare is unordered for exactly those patterns and nothing else,
// and b.vc skips the replacement on every ordered value. The self-compare
// is written with the same capture twice, which the key round trip's own
// fcmp (two different registers) cannot match.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadF32' (version 2)
// SPEC: // Inline typed array load (kind 41)
// SPEC: ldr [[S:s[0-9]+]], {{\[}}{{x[0-9]+}}, {{w[0-9]+}} uxtw 2]
// SPEC-NEXT: fcvt [[FV:d[0-9]+]], [[S]]
// SPEC-NEXT: fcmp [[FV]], [[FV]]
// SPEC-NEXT: b.vc [[FDONE:L[0-9]+]]
// SPEC-NEXT: mov [[FNAN:x[0-9]+]], 0x7FF8000000000000
// SPEC-NEXT: fmov [[FV]], [[FNAN]]
// SPEC-NEXT: [[FDONE]]:
// SPEC-NEXT: fmov {{x[0-9]+}}, [[FV]]
// SPEC: F32 -2.5

// Float64Array is CellKind 42 (0x2A): the element is already a double, so
// there is no convert at all -- the load goes straight into the
// canonicalization, scaled by 8.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadF64' (version 2)
// SPEC: // Inline typed array load (kind 42)
// SPEC: ldr [[DV:d[0-9]+]], {{\[}}{{x[0-9]+}}, {{w[0-9]+}} uxtw 3]
// SPEC-NEXT: fcmp [[DV]], [[DV]]
// SPEC-NEXT: b.vc [[DDONE:L[0-9]+]]
// SPEC-NEXT: mov {{x[0-9]+}}, 0x7FF8000000000000
// SPEC: F64 2.5
