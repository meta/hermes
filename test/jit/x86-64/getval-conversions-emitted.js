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
// UNSUPPORTED: handle_san

// That the typed-array load tier is actually EMITTED for the kinds
// test/jit/getval-conversions.js exercises, and that each kind's element
// load really is the instruction its width and signedness call for.
//
// This file exists because its companion cannot do this. getval-conversions.js
// is ARCHITECTURE-NEUTRAL -- it runs on arm64, which has no such tier -- so
// it can only check values, and identical values come out whether a kind is
// specialized inline or answered by the helper. Without an emission pin
// somewhere, dropping a kind from isJitSupportedTypedArrayLoadKind (or from
// emitGetByValTypedArrayTier's switch) would silently move that kind onto
// the helper and leave the movzx-versus-movsx-versus-64-bit-conversion
// distinctions untested rather than failing.
//
// SIX kinds are pinned here -- the ones no other file pins. The other
// three supported kinds already have emission pins elsewhere, and are
// deliberately not repeated:
//   - Int8 (34) and Float64 (42): x86-64/getbyval-inline-emitted.js, which
//     pins their full guard chains, not just the convert;
//   - Int32 (39): x86-64/recompile-getval-{trigger,duo,poisoned-int32-first}.js;
//   - Float64 (42) again: x86-64/recompile-getval-poisoned-float64-first.js.
// Float16 and the BigInt kinds are NOT supported and must never be
// specialized; test/jit/getval-guards.js covers their decline.
//
// One function per kind, each warmed with 100 separate top-level calls --
// past the 64-decline threshold pinned on the RUN lines -- so that a
// version 2 exists to inspect. A shared loader would specialize only its
// first kind and poison the rest. Every view sits at a NONZERO byte offset
// and every probe reads a nonzero index, and the values are diffed against
// the interpreter, so the instructions pinned below are checked to be
// correct as well as present.
//
// Every read goes through a variable key: a literal-keyed twin would lower
// to GetByIndex, which has no tier at all.
function loadU8(a, i) {
  return a[i];
}
function loadU8C(a, i) {
  return a[i];
}
function loadU16(a, i) {
  return a[i];
}
function loadI16(a, i) {
  return a[i];
}
function loadU32(a, i) {
  return a[i];
}
function loadF32(a, i) {
  return a[i];
}

var buf = new ArrayBuffer(128);

// Each value below is chosen so that the WRONG instruction prints a
// different number: an unsigned kind read with movsx would come out
// negative, a signed one read with movzx would come out large and
// positive, and a Uint32 converted as a signed 32-bit value would print
// a negative number instead of one above 2^31.
var u8 = new Uint8Array(buf, 8, 4);
u8[1] = 200;
for (var i = 0; i < 100; ++i) loadU8(u8, 0);
print("U8", loadU8(u8, 1));

var u8c = new Uint8ClampedArray(buf, 16, 4);
u8c[1] = 200;
for (var i = 0; i < 100; ++i) loadU8C(u8c, 0);
print("U8C", loadU8C(u8c, 1));

var u16 = new Uint16Array(buf, 24, 4);
u16[1] = 60000;
for (var i = 0; i < 100; ++i) loadU16(u16, 0);
print("U16", loadU16(u16, 1));

var i16 = new Int16Array(buf, 40, 4);
i16[1] = -30000;
for (var i = 0; i < 100; ++i) loadI16(i16, 0);
print("I16", loadI16(i16, 1));

var u32 = new Uint32Array(buf, 56, 4);
u32[1] = 4000000000;
for (var i = 0; i < 100; ++i) loadU32(u32, 0);
print("U32", loadU32(u32, 1));

var f32 = new Float32Array(buf, 72, 4);
f32[1] = -2.5;
for (var i = 0; i < 100; ++i) loadF32(f32, 0);
print("F32", loadF32(f32, 1));

// Each section is anchored on the version-2 compile banner rather than on
// a CHECK-LABEL: a recompiled function prints its name line once per
// version, so a label on it would not be unique. Each section ends with
// the value that kind produces, which the dump prints right after that
// kind's compiles and before the next kind's -- so the instructions and
// the number they compute are pinned together.
//
// The register-width patterns end-anchor on purpose: `r8` is a prefix of
// `r8d`, so an unanchored "64-bit register" pattern would match the 32-bit
// form too and stop distinguishing the Uint32 conversion from every other
// one. The anchors were checked to reject the wrong width.
//
// Every convert is pinned with SPEC-NEXT, not SPEC, and that is what makes
// the width pin mean anything: the key round trip emits its own vcvtsi2sd,
// always 64-bit, at every kind and in both tiers, so a floating pattern
// that failed on the element convert would simply scan forward and match
// one of those instead. Requiring the convert to be the instruction
// IMMEDIATELY after the element load leaves it nothing else to match.
//
// Each element load is in turn pinned by its full addressing form, an
// INDEX REGISTER scaled by the element width. That is both the second
// per-kind distinguisher (the scale IS the width) and what keeps these
// pins heap-value-mode independent: under HV32 the buffer's compressed
// pointer is decoded with a `mov e.., dword ptr [reg+disp]` that a looser
// anchor matches instead, and the element load is the only dword load in
// the tier with an index register.

// Uint8Array is CellKind 33. Zero-extending byte load, 32-bit conversion.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadU8' (version 2)
// SPEC: // Inline typed array load (kind 33)
// SPEC: movzx {{.*}}, byte ptr [{{.*}}+{{.*}}]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: U8 200

// Uint8ClampedArray is CellKind 35, and reads exactly like Uint8: clamping
// is a store-side conversion and leaves no trace in the stored byte.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadU8C' (version 2)
// SPEC: // Inline typed array load (kind 35)
// SPEC: movzx {{.*}}, byte ptr [{{.*}}+{{.*}}]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: U8C 200

// Uint16Array is CellKind 36. Zero-extending WORD load -- the width is the
// other half of what distinguishes it from Uint8.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadU16' (version 2)
// SPEC: // Inline typed array load (kind 36)
// SPEC: movzx {{.*}}, word ptr [{{.*}}+{{.*}}*2]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: U16 60000

// Int16Array is CellKind 37: the same width, SIGN-extended.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadI16' (version 2)
// SPEC: // Inline typed array load (kind 37)
// SPEC: movsx {{.*}}, word ptr [{{.*}}+{{.*}}*2]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: I16 -30000

// Uint32Array is CellKind 38, and is the one integer kind whose values can
// exceed INT32_MAX. Its 32-bit load zero-extends into the full register and
// the conversion is the 64-BIT form -- a 64-bit register operand, not the
// 32-bit one every other integer kind uses. x86 has no unsigned
// integer-to-double conversion at all, so this is what keeps values above
// 2^31 from coming out negative, as the value below shows.
//
// The `mov e.., dword ptr [reg+reg*4]` anchor is what makes the conversion
// pin refer to the ELEMENT load rather than to the key round trip, whose
// own vcvtsi2sd is 64-bit at every kind.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadU32' (version 2)
// SPEC: // Inline typed array load (kind 38)
// SPEC: mov {{e[a-z][a-z]}}, dword ptr [{{.*}}+{{.*}}*4]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{r(ax|bx|cx|dx|si|di|bp|sp|8|9|1[0-5])$}}
// SPEC: U32 4000000000

// Float32Array is CellKind 41: no integer conversion at all, but a WIDENING
// one, followed by the NaN canonicalization that only the float kinds get.
// The self-compare is written with the same capture twice, which the key
// conversion's own vucomisd (two different registers) cannot match.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadF32' (version 2)
// SPEC: // Inline typed array load (kind 41)
// SPEC: vmovss {{.*}}, dword ptr [{{.*}}+{{.*}}*4]
// SPEC-NEXT: vcvtss2sd
// SPEC-NEXT: vucomisd [[V:xmm[0-9]+]], [[V]]
// SPEC-NEXT: vmovq
// SPEC-NEXT: jnp [[FDONE:L[0-9]+]]
// SPEC-NEXT: mov {{.*}}, 0x7FF8000000000000
// SPEC: F32 -2.5
