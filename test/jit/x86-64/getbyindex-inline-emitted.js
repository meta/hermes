/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=off %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=SPEC,SPEC-%hv-mode %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit

// That the GetByIndex inline load tiers are EMITTED, and what they
// consist of -- the constant-key twin of getbyval-inline-emitted.js
// combined with getval-conversions-emitted.js's per-kind emission pins.
// getidx-guards.js is the file that runs the tiers (and their decline
// paths) against real prototype/hole/nonzero-beginIndex_/boundary
// traffic, and getidx-conversions.js the one that checks their values on
// every kind; this one looks at the instructions.
//
// Unlike getbyval-inline-emitted.js there is no key-conversion block to
// pin anywhere: K is an emit-time constant, so the JSArray guard sequence
// goes straight from the kind check to the begin-relative range test, and
// the typed-array tier goes straight from its kind check to its bounds
// test. The slow path is the same per-site INDIRECT recording call the
// ByVal sites use.
//
// This tier is emitted in every heap-value mode for the same reason
// emitGetByValFastArrayTier() is (see getbyval-inline-emitted.js's own
// comment): a load takes no write barrier, so HERMESVM_SANITIZE_HANDLES
// changes nothing here either. What is the same in all three modes is
// pinned under SPEC, and what differs -- element scale, and how the
// header packs kind and size, plus the empty-slot check's register
// shape -- is pinned under SPEC-HV64, SPEC-HV32 or SPEC-BOXED, exactly as
// getbyval-inline-emitted.js's own guards.
//
// -Xjit=force is enough here, unlike getbyid's own inline-cache tests:
// this tier reads no property cache, so it does not need the function to
// have run interpreted first.
//
// LITERAL uint8 key only: ISel.cpp lowers a literal number key
// representable as uint8 to GetByIndex; a non-literal or a key >= 256
// would lower to GetByVal instead and exercise none of this tier. The BC
// pin at the bottom is the anti-vacuity check for that.

function load(arr) {
  return arr[7];
}

var a = [0, 1, 2, 3, 4, 5, 6, 7, 8];
for (var i = 0; i < 20; ++i)
  load(a);
print(load(a));
// CHECK: 7

// SPEC-LABEL:load:
// Anti-GetByVal, anti-vacuity pin: the comment identifies this as a
// GetByIndex site (getByVal sites instead read "// getByVal r").
// SPEC: // getByIdx r
// SPEC: // Inline fast array load
// The source must be an object ...
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFF
// SPEC-HV64: jne [[SLOW:L[0-9]+]]
// SPEC-HV32: jne [[SLOW:L[0-9]+]]
// SPEC-BOXED: jne [[SLOW:L[0-9]+]]
// ... of cell kind JSArray (30, 0x1E) exactly -- no key-conversion block
// follows, unlike getbyval-inline-emitted.js's pin at this same point: K
// is an emit-time constant, so there is nothing to convert.
// SPEC: cmp byte ptr {{.*}}, 0x1E
// SPEC: jne [[SLOW]]
// ... and lies in [beginIndex_, beginIndex_ + elemCount_) -- the
// begin-relative form with K folded into an immediate (asmjit has no
// sub(Imm, Gp)): mov idx, Imm(K), then subtract beginIndex_ and compare
// against elemCount_, one unsigned compare covering both K < beginIndex_
// (via unsigned wrap) and K >= beginIndex_ + elemCount_.
// SPEC: mov {{.*}}, 7
// SPEC: sub {{.*}}, dword ptr {{.*}}
// SPEC: cmp {{.*}}, dword ptr {{.*}}
// SPEC: jae [[SLOW]]
// The element address: the index scaled by the width of one slot. Only
// the scale is pinned; the displacement is where ArrayStorageSmall's
// elements begin.
// SPEC-HV64: lea {{.*}}, [{{.*}}*8+{{[0-9]+}}]
// SPEC-HV32: lea {{.*}}, [{{.*}}*4+{{[0-9]+}}]
// SPEC-BOXED: lea {{.*}}, [{{.*}}*8+{{[0-9]+}}]
// ... at an address that does not currently hold a hole -- the same
// empty-slot check emitGetByValFastArrayTier() uses (see
// getbyval-inline-emitted.js's comment for why the 8-byte-slot case needs
// a separate register from the one holding the value while the 4-byte
// case does not).
// SPEC-HV64: mov {{.*}}, qword ptr {{.*}}
// SPEC-HV64: sar {{.*}}, 0x2F
// SPEC-HV64: cmp {{.*}}, 0xFFFFFFFFFFFFFFF2
// SPEC-BOXED: mov {{.*}}, qword ptr {{.*}}
// SPEC-BOXED: sar {{.*}}, 0x2F
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFF2
// SPEC-HV32: mov {{.*}}, dword ptr {{.*}}
// SPEC-HV32: cmp {{.*}}, 0xFFF90000
// SPEC: je [[SLOW]]
//
// And the empty check's decline, and every guard's, falls to the SAME
// slow path: the per-site RECORDING helper, reached INDIRECTLY through
// the site's own mutable function-pointer slot rather than by a direct
// call to a fixed address. The slot is what the runtime flips, after
// emission, from the recording helper to the plain one when a site is
// demoted, so the call sequence has to load the address at run time:
// `movabs r11, <&site.helper>` followed by a call through it. A direct
// `call _sh_ljs_get_by_index_rjs` here would mean the site records
// nothing and can never be demoted either.
//
// The key goes into edx BY VALUE, as an immediate -- _jit_get_by_index
// takes the uint32 itself, matching the plain helper it forwards to --
// and the two arguments past the three that helper takes (the version
// record and the site id) are set up unconditionally: under SysV a callee
// that takes fewer arguments simply ignores the extra registers, which is
// what lets one sequence serve both the recording callee and the demoted
// one.
// SPEC: [[SLOW]]:
// SPEC: mov edx, 7
// SPEC: // call _jit_get_by_index [indirect]
// SPEC: mov r11, {{.*}}
// SPEC: call qword ptr [r11]
//
// The value this specialized site produces -- the pin that the
// instructions above are not merely present but correct.
// SPEC: 7

// ---- the typed-array tier, all nine supported kinds ----
//
// These follow getval-conversions-emitted.js's rationale: a value-only
// test cannot see a kind silently left on the (correct-answer) helper
// path, so dropping a kind from isJitSupportedTypedArrayLoadKind, or from
// the shared element-load tail's switch, would leave that kind's
// movzx-versus-movsx-versus-64-bit-conversion distinction untested rather
// than failing. Every supported kind is pinned here, because unlike
// GetByVal no other ByIndex file pins any of them.
//
// THE CONSTANT-SITE WARM-UP RECIPE. A GetByIndex site reads exactly ONE
// index and cannot borrow another site's evidence, so each kind gets its
// OWN literal-K loader, warmed with 100 separate top-level calls past the
// 64-decline threshold pinned on the RUN lines, and its known-value probe
// is a FRESH top-level call afterwards -- installing a version swaps the
// function ENTRY, never a running activation.
//
// Every loader uses a DIFFERENT literal K, and every view sits at a
// NONZERO byte offset. The view's own offset_ is added into the base
// REGISTER at run time, so the displacement in the element access is
// exactly `K * width` -- which makes the displacement a second per-kind
// distinguisher alongside the load instruction, and is the ByIndex shape
// the ByVal tier never emits (that one scales an index REGISTER instead
// and has no constant displacement to check).
function ldI8(a) {
  return a[3];
}
function ldU8(a) {
  return a[5];
}
function ldU8C(a) {
  return a[7];
}
function ldI16(a) {
  return a[3];
}
function ldU16(a) {
  return a[5];
}
function ldI32(a) {
  return a[3];
}
function ldU32(a) {
  return a[5];
}
function ldF32(a) {
  return a[6];
}
function ldF64(a) {
  return a[3];
}

// Each value is chosen so that the WRONG instruction prints a different
// number: an unsigned kind read with movsx would come out negative, a
// signed one read with movzx would come out large and positive, and a
// Uint32 converted as a signed 32-bit value would print a negative number
// instead of one above 2^31.
var bI8 = new ArrayBuffer(32);
var vI8 = new Int8Array(bI8, 8, 8);
vI8[3] = -128;
// A short view of the SAME kind, read through the same warmed site: K = 3
// is past its end, so the tier answers `undefined` INLINE -- the value
// side of the bounds-fail path pinned below.
var vI8short = new Int8Array(bI8, 8, 2);
for (var i = 0; i < 100; ++i) ldI8(vI8);
print("I8", ldI8(vI8), ldI8(vI8short));

var bU8 = new ArrayBuffer(32);
var vU8 = new Uint8Array(bU8, 8, 8);
vU8[5] = 200;
for (var i = 0; i < 100; ++i) ldU8(vU8);
print("U8", ldU8(vU8));

var bU8C = new ArrayBuffer(32);
var vU8C = new Uint8ClampedArray(bU8C, 8, 8);
vU8C[7] = 200;
for (var i = 0; i < 100; ++i) ldU8C(vU8C);
print("U8C", ldU8C(vU8C));

var bI16 = new ArrayBuffer(32);
var vI16 = new Int16Array(bI16, 8, 8);
vI16[3] = -30000;
for (var i = 0; i < 100; ++i) ldI16(vI16);
print("I16", ldI16(vI16));

var bU16 = new ArrayBuffer(32);
var vU16 = new Uint16Array(bU16, 8, 8);
vU16[5] = 60000;
for (var i = 0; i < 100; ++i) ldU16(vU16);
print("U16", ldU16(vU16));

var bI32 = new ArrayBuffer(64);
var vI32 = new Int32Array(bI32, 8, 8);
vI32[3] = -2000000000;
for (var i = 0; i < 100; ++i) ldI32(vI32);
print("I32", ldI32(vI32));

var bU32 = new ArrayBuffer(64);
var vU32 = new Uint32Array(bU32, 8, 8);
vU32[5] = 4000000000;
for (var i = 0; i < 100; ++i) ldU32(vU32);
print("U32", ldU32(vU32));

var bF32 = new ArrayBuffer(64);
var vF32 = new Float32Array(bF32, 8, 8);
vF32[6] = -2.5;
for (var i = 0; i < 100; ++i) ldF32(vF32);
print("F32", ldF32(vF32));

var bF64 = new ArrayBuffer(128);
var vF64 = new Float64Array(bF64, 8, 8);
vF64[3] = 2.5;
for (var i = 0; i < 100; ++i) ldF64(vF64);
print("F64", ldF64(vF64));

// Each section is anchored on the version-2 compile banner rather than on
// a CHECK-LABEL: a recompiled function prints its name line once per
// version, so a label on it would not be unique. Each section ends with
// the value that kind produces, which the dump prints right after that
// kind's compiles and before the next kind's -- so the instructions and
// the number they compute are pinned together.
//
// Every element load is anchored TWICE, and both anchors are there
// because of the two silent-match hazards getval-conversions-emitted.js
// documents.
//
// The first hazard -- the key conversion's own vcvtsi2sd, which a
// floating convert pattern would happily match instead of the element
// convert -- does not exist on this opcode at all, since there is no key
// to convert. Every convert is still pinned with SPEC-NEXT rather than
// SPEC, so the pins keep meaning what they say if that ever changes.
//
// The second hazard is live here, and the displacement alone does NOT
// settle it: the tier's own address computation contains two other dword
// loads -- the view's offset_ in every mode, and under HV32 the buffer's
// compressed-pointer decode -- whose displacements are STRUCT OFFSETS,
// and a struct offset can coincide with some kind's `K * width`. It does:
// under handle_san+HV32 the buffer decode sits at displacement 20, which
// is exactly Uint32's K*4 here. So each element load is pinned as the
// instruction IMMEDIATELY AFTER the attachedness branch and the add of
// offset_ into the base register -- a position nothing else in the tier
// can occupy -- and the exact `K * width` displacement (closing bracket
// included, so +12 cannot match +120) is then the per-kind distinguisher
// it was meant to be rather than the thing holding the pin up.
//
// The register-width patterns end-anchor on purpose: `r8` is a prefix of
// `r8d`, so an unanchored "64-bit register" pattern would match the
// 32-bit form too and stop distinguishing the Uint32 conversion from
// every other one.

// Int8Array is CellKind 34. Sign-extending byte load at K*1 = 3, 32-bit
// conversion. This section also carries the bounds-fail path, which is
// the same in every kind: an out-of-range index is `undefined`, produced
// INLINE -- no call, and therefore no recording either.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldI8' (version 2)
// SPEC: // Inline typed array load (kind 34)
// The bounds test, with K an immediate: the compare is against the length
// field DIRECTLY, so the operand order and the condition are the reverse
// of the ByVal tier's -- `jbe` is taken exactly when `length_ <= K`.
// SPEC: cmp dword ptr {{.*}}, 3
// SPEC: jbe [[UNDEF:L[0-9]+]]
// Attachedness, via the buffer's data_ pointer: it reaches the same
// inline `undefined`. The add that follows is the view's byte offset_
// going into the base register, and the element load is ALWAYS the
// instruction right after it -- which is what anchors every element-load
// pin in this file (see the note above).
// SPEC: test {{.*}}
// SPEC-NEXT: jz [[UNDEF]]
// SPEC-NEXT: add {{.*}}
// SPEC-NEXT: movsx {{.*}}, byte ptr [{{.*}}+3]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: [[UNDEF]]:
// SPEC: mov {{.*}}, 0xFFFA000000000000
// The kind miss lands in the JSArray tier, which is still emitted here.
// SPEC: // Inline fast array load
// SPEC: I8 -128 undefined

// Uint8Array is CellKind 33: the same width, ZERO-extended, at K*1 = 5.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldU8' (version 2)
// SPEC: // Inline typed array load (kind 33)
// SPEC: jz L{{[0-9]+}}
// SPEC-NEXT: add {{.*}}
// SPEC-NEXT: movzx {{.*}}, byte ptr [{{.*}}+5]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: U8 200

// Uint8ClampedArray is CellKind 35, and reads exactly like Uint8:
// clamping is a store-side conversion and leaves no trace in the stored
// byte. Its K is 7, so the displacement is what tells this section's
// element load apart from Uint8's identical instruction.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldU8C' (version 2)
// SPEC: // Inline typed array load (kind 35)
// SPEC: jz L{{[0-9]+}}
// SPEC-NEXT: add {{.*}}
// SPEC-NEXT: movzx {{.*}}, byte ptr [{{.*}}+7]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: U8C 200

// Int16Array is CellKind 37. Sign-extending WORD load at K*2 = 6.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldI16' (version 2)
// SPEC: // Inline typed array load (kind 37)
// SPEC: jz L{{[0-9]+}}
// SPEC-NEXT: add {{.*}}
// SPEC-NEXT: movsx {{.*}}, word ptr [{{.*}}+6]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: I16 -30000

// Uint16Array is CellKind 36: the same width, ZERO-extended, at K*2 = 10.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldU16' (version 2)
// SPEC: // Inline typed array load (kind 36)
// SPEC: jz L{{[0-9]+}}
// SPEC-NEXT: add {{.*}}
// SPEC-NEXT: movzx {{.*}}, word ptr [{{.*}}+10]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: U16 60000

// Int32Array is CellKind 39. A 32-bit load at K*4 = 12, converted as a
// SIGNED 32-BIT value.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldI32' (version 2)
// SPEC: // Inline typed array load (kind 39)
// SPEC: jz L{{[0-9]+}}
// SPEC-NEXT: add {{.*}}
// SPEC-NEXT: mov {{e[a-z][a-z]}}, dword ptr [{{.*}}+12]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{(e[a-z][a-z]|r[0-9]+d)$}}
// SPEC: I32 -2000000000

// Uint32Array is CellKind 38, and is the one integer kind whose values
// can exceed INT32_MAX. Its 32-bit load at K*4 = 20 zero-extends into the
// full register and the conversion is the 64-BIT form -- a 64-bit
// register operand, not the 32-bit one every other integer kind uses.
// x86 has no unsigned integer-to-double conversion at all, so this is
// what keeps values above 2^31 from coming out negative, as the value
// below shows.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldU32' (version 2)
// SPEC: // Inline typed array load (kind 38)
// SPEC: jz L{{[0-9]+}}
// SPEC-NEXT: add {{.*}}
// SPEC-NEXT: mov {{e[a-z][a-z]}}, dword ptr [{{.*}}+20]
// SPEC-NEXT: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{r(ax|bx|cx|dx|si|di|bp|sp|8|9|1[0-5])$}}
// SPEC: U32 4000000000

// Float32Array is CellKind 41: no integer conversion at all, but a
// WIDENING one at K*4 = 24, followed by the NaN canonicalization that
// only the float kinds get. The self-compare is written with the same
// capture twice.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldF32' (version 2)
// SPEC: // Inline typed array load (kind 41)
// SPEC: jz L{{[0-9]+}}
// SPEC-NEXT: add {{.*}}
// SPEC-NEXT: vmovss {{.*}}, dword ptr [{{.*}}+24]
// SPEC-NEXT: vcvtss2sd
// SPEC-NEXT: vucomisd [[V:xmm[0-9]+]], [[V]]
// SPEC-NEXT: vmovq
// SPEC-NEXT: jnp [[FDONE:L[0-9]+]]
// SPEC-NEXT: mov {{.*}}, 0x7FF8000000000000
// SPEC: F32 -2.5

// Float64Array is CellKind 42. Its element is already a double, so there
// is no convert instruction to distinguish it by; what is pinned instead
// is its 8-byte load at K*8 = 24 and the NaN-canonicalization sequence
// that follows it immediately -- the one place a bug would forge a tagged
// non-number out of a NaN payload, and the sequence the ByIndex tier
// shares with the ByVal one.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldF64' (version 2)
// SPEC: // Inline typed array load (kind 42)
// SPEC: jz L{{[0-9]+}}
// SPEC-NEXT: add {{.*}}
// SPEC-NEXT: vmovsd {{.*}}, qword ptr [{{.*}}+24]
// SPEC-NEXT: vucomisd [[W:xmm[0-9]+]], [[W]]
// SPEC-NEXT: vmovq
// SPEC-NEXT: jnp [[DDONE:L[0-9]+]]
// SPEC-NEXT: mov {{.*}}, 0x7FF8000000000000
// SPEC: F64 2.5

// BC-LABEL:Function<load>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<ldI8>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<ldU8>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<ldU8C>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<ldI16>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<ldU16>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<ldI32>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<ldU32>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<ldF32>({{.*}}
// BC: GetByIndex
// BC-LABEL:Function<ldF64>({{.*}}
// BC: GetByIndex
