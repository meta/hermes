/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=SPEC,SPEC-%hv-mode %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit
// REQUIRES: jit-arch-arm64

// That the GetByIndex inline load tiers are EMITTED on arm64, and what
// they consist of -- the constant-key twin of
// getbyval-inline-emitted-arm64.js combined with
// getval-conversions-emitted-arm64.js's per-kind emission pins.
// getidx-guards.js is the file that runs the tiers (and their decline
// paths) against real prototype/hole/nonzero-beginIndex_/boundary
// traffic, and getidx-conversions.js the one that checks their values on
// every kind; this one only looks at the instructions. Its x86-64
// counterpart is test/jit/x86-64/getbyindex-inline-emitted.js.
//
// Unlike getbyval-inline-emitted-arm64.js there is no key-conversion
// block to pin anywhere: K is an emit-time constant, so the JSArray
// guard sequence goes straight from the kind check to the begin-relative
// range test, and the typed-array tier goes straight from its kind check
// to its bounds test. The slow path is the same per-site INDIRECT
// recording call the ByVal sites use.
//
// UNLIKE putbyval-inline-emitted-arm64.js, and like
// getbyval-inline-emitted-arm64.js, this file carries no "unsupported
// under handle_san" annotation. That annotation exists there because
// HERMESVM_SANITIZE_HANDLES changes emitted CODE on the PUT side: it
// makes canInlineCompressibleOrNumberHV64() refuse every number, so
// emit_shv_encode_or_slow() (JitEmitter-internal.cpp) takes its
// HERMESVM_SANITIZE_HANDLES branch and declines the inline-number case
// with a bare jump instead of emitting the encode sequence
// putbyval-inline-emitted-arm64.js pins. GetByIndex has no encode step at
// all -- the fast array tier only ever DECODES an already-stored
// SmallHermesValue, through Emit_sh_shv_decode
// (Emitter::emitGetByIndexFastArrayTier() -> emit_sh_shv_decode()), and
// the typed-array tier reads raw machine values out of a malloc'd buffer
// and touches no heap value at all. Neither references
// HERMESVM_SANITIZE_HANDLES, so this file's output is identical whether
// or not Handle-San is on, and gating it would just hide the tiers from
// that configuration's suite for no reason.
//
// -Xjit=force is enough, unlike putbyid-inline-emitted-arm64.js: these
// tiers read no property cache, so they do not need the function to have
// run interpreted first. The JSArray tier is additionally unconditional
// -- unlike the fast array STORE tier it carries no
// HERMES_JIT_INLINE_SAFE_STORE gate, because a load takes no write
// barrier.
//
// The JSArray tier exists in every heap-value mode, and so does this
// file: what is the same in all three is pinned under plain SPEC, and
// what differs under SPEC-HV64, SPEC-HV32 or SPEC-BOXED, of which
// exactly one is active in a given build (test/lit.cfg's %hv-mode).
// Field displacements are wildcarded throughout, because they move under
// compressed pointers. Nothing in the TYPED-ARRAY tier is mode-shaped: a
// typed array's elements are raw machine values in a malloc'd buffer,
// not SmallHermesValues, and the encode of the result is a number
// HermesValue in every mode.
//
// LITERAL uint8 key only: ISel.cpp lowers a literal number key
// representable as uint8 to GetByIndex; a non-literal or a key >= 256
// would lower to GetByVal instead and exercise none of this. The BC pins
// at the bottom are the anti-vacuity check for that.

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
// The source must be an object. arm64 tests a tag by shifting it down
// and adding, so this is `asr`+`cmn` rather than x86-64's `sar`+`cmp`.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC-NEXT: cmn {{x[0-9]+}}, 1
// SPEC-NEXT: b.ne [[SLOW:L[0-9]+]]
// SPEC-NEXT: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// ... of cell kind JSArray exactly ...
// SPEC-NEXT: ldrb {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-NEXT: cmp {{w[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
// SPEC-NEXT: b.ne [[SLOW]]
// ... and NOTHING about the object's flags, for the same reason
// getbyval-inline-emitted-arm64.js pins their absence on the ByVal side:
// a non-fast indexed property leaves its slot EMPTY and the hole test
// below already declines on that. The store tier's masked compare is two
// instructions no other guard here emits.
// SPEC-NOT: mov w16, 0x14
// SPEC-NOT: and {{w[0-9]+}}, {{w[0-9]+}}, w16
// ... and K lies in [beginIndex_, beginIndex_ + elemCount_). The
// begin-relative form with K an IMMEDIATE, which is the whole shape this
// opcode has where the ByVal tier has its key round trip: K has to be
// materialized because it is the MINUEND of the subtraction, and the
// subtraction has to happen at run time because a first indexed write
// can set beginIndex_ to any index -- a constant key does NOT make the
// storage offset constant. One unsigned compare covers both
// K < beginIndex_ (via unsigned wrap) and K >= beginIndex_ + elemCount_.
// Out of range is a DECLINE and not an inline `undefined`, unlike the
// typed-array tier's own bounds check: the prototype chain may carry an
// indexed property there.
// SPEC-NEXT: mov {{w[0-9]+}}, 7
// SPEC-NEXT: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-NEXT: sub {{w[0-9]+}}, {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-NEXT: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-NEXT: cmp {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-NEXT: b.hs [[SLOW]]
// The indexed storage -- a compressed-pointer load, four bytes under
// compressed pointers and eight otherwise -- and then the element
// address: the index scaled by the width of one slot, then the offset of
// the first element. There is no jumbo-cell size gate here, unlike the
// store tier: that guard exists only for the write barrier's card math.
// SPEC-HV64-NEXT: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC-HV32-NEXT: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV32-NEXT: add {{x[0-9]+}}, x19, {{x[0-9]+}}
// SPEC-HV32-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}, 2
// SPEC-BOXED-NEXT: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-BOXED-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
//
// The HOLE CHECK, emitGetByValFastArrayTier()'s verbatim, including its
// reason for not calling emit_shv_load_is_empty(): the loaded bits are
// the value this tier must go on to DECODE, so where a slot is eight
// bytes the ETag shift has to land in a register of its OWN. The
// CHECK-NOT is what gives that a pin rather than a comment -- [[EL]] is
// the register the element was loaded into, and between the shift and
// the branch the only comparison allowed is one against some OTHER
// register.
// SPEC-HV64-NEXT: ldr [[EL:x[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-HV64-NEXT: asr {{x[0-9]+}}, [[EL]], 0x2F
// SPEC-HV64-NOT: cmn [[EL]], 0xE
// SPEC-BOXED-NEXT: ldr [[EL:x[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-BOXED-NEXT: asr {{x[0-9]+}}, [[EL]], 0x2F
// SPEC-BOXED-NOT: cmn [[EL]], 0xE
// Where a slot is four bytes the whole encoded empty value is compared
// instead, and a `cmp` never writes its operand -- so that form needs no
// separate register and has no such hazard.
// SPEC-HV32-NEXT: ldr {{w[0-9]+}}, {{\[}}{{x[0-9]+}}]
// SPEC-HV32-NEXT: mov w16, 0xFFF90000
// SPEC-HV32-NEXT: cmp {{w[0-9]+}}, w16
// SPEC: b.eq [[SLOW]]
//
// Then the unbox of the SmallHermesValue into the HermesValue the result
// register must hold. Under HV64 a slot already holds the HermesValue's
// bits, so NOTHING is emitted -- the decode's dispatch would start with
// a `tst` of the HV32 tag bits, and there is none.
// SPEC-HV64-NOT: tst {{x[0-9]+}}, 7
// SPEC-HV32: tst {{x[0-9]+}}, 7
// SPEC-HV32-NEXT: b.ne {{L[0-9]+}}
// SPEC-HV32-NEXT: lsl {{x[0-9]+}}, {{x[0-9]+}}, 0x20
// SPEC-BOXED: tst {{x[0-9]+}}, 7
// SPEC-BOXED-NEXT: b.ne {{L[0-9]+}}
//
// And the helper call every decline falls back to: the per-site
// RECORDING helper, reached INDIRECTLY through the site's own mutable
// function pointer slot rather than by a direct call to a fixed address.
// The slot is what the runtime flips, after emission, from the recording
// helper to the plain one when a site is demoted, so the call sequence
// has to load the callee at run time. A direct call to
// _sh_ljs_get_by_index_rjs here would mean the site records nothing and
// could never be demoted either.
//
// The key goes into w2 BY VALUE, as an immediate -- _jit_get_by_index
// takes the uint32 itself, matching the plain helper it forwards to --
// and the two arguments past the three that helper takes (the version
// record and the site id) are set up unconditionally: under AAPCS64 a
// callee that takes fewer arguments simply ignores the extra registers,
// which is what lets one sequence serve both the recording callee and
// the demoted one. Only the two instructions that make the call indirect
// are pinned -- how the slot ADDRESS reaches x16 is loadBits64InGp's
// choice, and depends on where the record happens to live.
// SPEC: [[SLOW]]:
// SPEC: mov w2, 7
// SPEC: mov w4, {{[0-9]+}}
// SPEC: // call _jit_get_by_index [indirect]
// SPEC: ldr x16, {{\[}}x16]
// SPEC-NEXT: blr x16
//
// The value this specialized site produces -- the pin that the
// instructions above are not merely present but correct.
// SPEC: 7

// ---- the typed-array tier, all nine supported kinds ----
//
// These follow getval-conversions-emitted-arm64.js's rationale: a
// value-only test cannot see a kind silently left on the (correct-answer)
// helper path, so dropping a kind from isJitSupportedTypedArrayLoadKind,
// or from the shared element-load tail's switch, would leave that kind's
// ldrsb-versus-ldrb-versus-scvtf-versus-ucvtf distinction untested rather
// than failing. All nine are pinned here, because no other ByIndex file
// pins any of them.
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
// the ByVal tier never emits (that one hands the shared tail a UXTW-
// scaled index REGISTER and has no constant displacement to check).
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
// number: an unsigned kind read with a sign-extending load would come out
// negative, a signed one read with a zero-extending load would come out
// large and positive, and a Uint32 converted with scvtf instead of ucvtf
// would print a negative number instead of one above 2^31.
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
// the number they compute are pinned together, and a section's pins
// cannot silently drift into the next section's code.
//
// Every convert is pinned with SPEC-NEXT, not SPEC, for the reason
// getval-conversions-emitted-arm64.js gives: requiring the convert to be
// the instruction IMMEDIATELY after the element load leaves it nothing
// else to match. On this opcode the hazard that motivated that rule --
// the key round trip's own ucvtf -- does not exist at all, since there is
// no key to convert; the pins stay SPEC-NEXT so they keep meaning what
// they say if that ever changes.
//
// Every element load is anchored TWICE, and the second anchor is there
// because the displacement alone does NOT settle it: the tier's own
// address computation contains two other word loads -- the view's
// offset_ in every mode, and under HV32/BOXED the buffer's
// compressed-pointer decode -- whose displacements are STRUCT OFFSETS,
// and a struct offset can coincide with some kind's `K * width`. So each
// element load is pinned as the instruction IMMEDIATELY AFTER the
// attachedness `cbz` and the `add` of offset_ into the base register -- a
// position nothing else in the tier can occupy -- and the exact
// `K * width` displacement (closing bracket included, so 12 cannot match
// 120) is then the per-kind distinguisher it was meant to be rather than
// the thing holding the pin up.

// Int8Array is CellKind 34 (0x22). A SIGN-extending byte load at K*1 = 3,
// then the signed integer-to-double convert. This section also carries
// the bounds-fail path, which is the same in every kind: an out-of-range
// index is `undefined`, produced INLINE -- no call, and therefore no
// recording either.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldI8' (version 2)
// SPEC: // Inline typed array load (kind 34)
// The bounds test, with K an immediate: the compare is against the loaded
// length DIRECTLY, so the operand order and the condition are the reverse
// of the ByVal tier's `cmp idx, length_` / `b.hs` -- `b.ls` is taken
// exactly when `length_ <= K`, which keeps acceptance STRICT.
// SPEC: cmp {{w[0-9]+}}, 3
// SPEC-NEXT: b.ls [[UNDEF:L[0-9]+]]
// Attachedness, via the buffer's data_ pointer: it reaches the same
// inline `undefined`. The add that follows is the view's byte offset_
// going into the base register, and the element load is ALWAYS the
// instruction right after it -- which is what anchors every element-load
// pin in this file (see the note above).
// SPEC: cbz {{x[0-9]+}}, [[UNDEF]]
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-NEXT: ldrsb {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, 3]
// SPEC-NEXT: scvtf [[V:d[0-9]+]], {{w[0-9]+}}
// The encode: a number HermesValue is the double's raw bits, and an
// integer convert can never produce a NaN, so there is no
// canonicalization here -- only the move.
// SPEC-NEXT: fmov {{x[0-9]+}}, [[V]]
// SPEC: [[UNDEF]]:
// SPEC-NEXT: mov {{x[0-9]+}}, 0xFFFA000000000000
// The kind miss lands in the JSArray tier, which is still emitted here.
// SPEC: // Inline fast array load
// SPEC: I8 -128 undefined

// Uint8Array is CellKind 33 (0x21): the same width, ZERO-extended, at
// K*1 = 5.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldU8' (version 2)
// SPEC: // Inline typed array load (kind 33)
// SPEC: cbz {{x[0-9]+}}, L{{[0-9]+}}
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-NEXT: ldrb {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, 5]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: U8 200

// Uint8ClampedArray is CellKind 35 (0x23), and reads exactly like Uint8:
// clamping is a store-side conversion and leaves no trace in the stored
// byte. Its K is 7, so the displacement is what tells this section's
// element load apart from Uint8's identical instruction.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldU8C' (version 2)
// SPEC: // Inline typed array load (kind 35)
// SPEC: cbz {{x[0-9]+}}, L{{[0-9]+}}
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-NEXT: ldrb {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, 7]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: U8C 200

// Int16Array is CellKind 37 (0x25): a SIGN-extending halfword load at
// K*2 = 6.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldI16' (version 2)
// SPEC: // Inline typed array load (kind 37)
// SPEC: cbz {{x[0-9]+}}, L{{[0-9]+}}
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-NEXT: ldrsh {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, 6]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: I16 -30000

// Uint16Array is CellKind 36 (0x24): the same width, ZERO-extended, at
// K*2 = 10.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldU16' (version 2)
// SPEC: // Inline typed array load (kind 36)
// SPEC: cbz {{x[0-9]+}}, L{{[0-9]+}}
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-NEXT: ldrh {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, 10]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: U16 60000

// Int32Array is CellKind 39 (0x27): a word load at K*4 = 12, and the
// SIGNED convert.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldI32' (version 2)
// SPEC: // Inline typed array load (kind 39)
// SPEC: cbz {{x[0-9]+}}, L{{[0-9]+}}
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-NEXT: ldr {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, 12]
// SPEC-NEXT: scvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: I32 -2000000000

// Uint32Array is CellKind 38 (0x26), and is the one integer kind whose
// values can exceed INT32_MAX. Its load is the same word load, at
// K*4 = 20; what differs -- and the whole reason this kind gets its own
// case -- is the UNSIGNED convert. arm64 has it as one instruction, so
// unlike x86-64 (which has no unsigned convert and has to widen to a
// signed 64-bit value first) there is no width trick to look for here,
// just ucvtf where every other integer kind has scvtf. The value below is
// what it buys: with scvtf it would print as a negative number.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldU32' (version 2)
// SPEC: // Inline typed array load (kind 38)
// SPEC: cbz {{x[0-9]+}}, L{{[0-9]+}}
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-NEXT: ldr {{w[0-9]+}}, {{\[}}{{x[0-9]+}}, 20]
// SPEC-NEXT: ucvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: U32 4000000000

// Float32Array is CellKind 41 (0x29): the element is loaded at K*4 = 24
// through the register's SINGLE-precision view and WIDENED, and then --
// unlike every integer kind above -- canonicalized. A float element may
// hold any NaN bit pattern, and this engine's tag space lives inside the
// NaN space, so encoding such an element verbatim would forge a
// non-number; the self-compare is unordered for exactly those patterns
// and nothing else, and b.vc skips the replacement on every ordered
// value. The self-compare is written with the same capture twice.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldF32' (version 2)
// SPEC: // Inline typed array load (kind 41)
// SPEC: cbz {{x[0-9]+}}, L{{[0-9]+}}
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-NEXT: ldr [[S:s[0-9]+]], {{\[}}{{x[0-9]+}}, 24]
// SPEC-NEXT: fcvt [[FV:d[0-9]+]], [[S]]
// SPEC-NEXT: fcmp [[FV]], [[FV]]
// SPEC-NEXT: b.vc [[FDONE:L[0-9]+]]
// SPEC-NEXT: mov [[FNAN:x[0-9]+]], 0x7FF8000000000000
// SPEC-NEXT: fmov [[FV]], [[FNAN]]
// SPEC-NEXT: [[FDONE]]:
// SPEC-NEXT: fmov {{x[0-9]+}}, [[FV]]
// SPEC: F32 -2.5

// Float64Array is CellKind 42 (0x2A): the element is already a double, so
// there is no convert at all -- the load at K*8 = 24 goes straight into
// the canonicalization, which is the one place a bug would forge a tagged
// non-number out of a NaN payload, and the sequence the ByIndex tier
// shares with the ByVal one.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'ldF64' (version 2)
// SPEC: // Inline typed array load (kind 42)
// SPEC: cbz {{x[0-9]+}}, L{{[0-9]+}}
// SPEC-NEXT: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-NEXT: ldr [[DV:d[0-9]+]], {{\[}}{{x[0-9]+}}, 24]
// SPEC-NEXT: fcmp [[DV]], [[DV]]
// SPEC-NEXT: b.vc [[DDONE:L[0-9]+]]
// SPEC-NEXT: mov {{x[0-9]+}}, 0x7FF8000000000000
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
