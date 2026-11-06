/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=SPEC,SPEC-%hv-mode %s
// REQUIRES: jit
// REQUIRES: jit-arch-arm64

// That the GetByVal inline fast array LOAD tier is EMITTED on arm64, and
// what it consists of: the guards, the hole check, the unbox of the
// element, and the recording helper call every guard falls back to.
// getval-guards.js and getbyval-inline.js are the files that run this
// tier (and its decline paths) against real prototype/hole/Arguments
// traffic on every backend; this one only looks at the instructions. Its
// x86-64 counterpart is test/jit/x86-64/getbyval-inline-emitted.js.
//
// UNLIKE putbyval-inline-emitted-arm64.js, this file carries no
// "unsupported under handle_san" annotation. That annotation exists
// there because HERMESVM_SANITIZE_HANDLES changes emitted CODE on the
// PUT side: it makes canInlineCompressibleOrNumberHV64() refuse every
// number, so emit_shv_encode_or_slow() (JitEmitter-internal.cpp) takes
// its HERMESVM_SANITIZE_HANDLES branch and declines the inline-number
// case with a bare jump instead of emitting the encode sequence
// putbyval-inline-emitted-arm64.js pins. GetByVal has no encode step at
// all -- the tier only ever DECODES an already-stored SmallHermesValue,
// through Emit_sh_shv_decode (Emitter::emitGetByValFastArrayTier() ->
// emit_sh_shv_decode()), and neither that class nor anything else this
// tier emits references HERMESVM_SANITIZE_HANDLES. So this file's
// output is identical whether or not Handle-San is on, and gating it
// would just hide the tier from that configuration's suite for no
// reason.
//
// The typed-array load tier is NOT pinned here -- all nine of its kinds
// are, per kind, in getval-conversions-emitted-arm64.js.
//
// -Xjit=force is enough, unlike putbyid-inline-emitted-arm64.js: this
// tier reads no property cache, so it does not need the function to have
// run interpreted first. Every one of its guards is dynamic, and the
// tier is unconditional -- unlike the fast array STORE tier it carries no
// HERMES_JIT_INLINE_SAFE_STORE gate, because a load takes no write
// barrier.
//
// The tier exists in every heap-value mode, and so does this file: what
// is the same in all three is pinned under plain SPEC, and what differs
// under SPEC-HV64, SPEC-HV32 or SPEC-BOXED, of which exactly one is
// active in a given build (test/lit.cfg's %hv-mode). Field displacements
// are wildcarded throughout, because they move under compressed
// pointers.
//
// Three things are genuinely mode-shaped, and all three are consequences
// of the slot width or the slot encoding: the width of the
// indexed-storage load, the element scale, and -- the pair this file
// exists to pin -- the hole test and the decode that follows it.

function load(arr, i) {
  return arr[i];
}

var a = [0, 1, 2, 3];
for (var i = 0; i < 20; ++i)
  load(a, 1);
print(load(a, 1));
// CHECK: 1

// SPEC-LABEL:load:
// Anti-GetByIndex, anti-vacuity pin: a literal-keyed twin of this
// function would lower to GetByIndex instead (ISel.cpp's uint8-literal
// special case), which this tier never touches, and the comment below
// would then read "// getByIdx" -- the dynamic key above is what keeps
// this GetByVal.
// SPEC: // getByVal r
// SPEC: // Inline fast array load
// The source must be an object. arm64 tests a tag by shifting it down
// and adding, so this is `asr`+`cmn` rather than x86-64's `sar`+`cmp`.
// This tier has no encode step (unlike the put tier's, which under
// HV32/BOXED defines its slow label earlier still), so this is the first
// decline point in every mode.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC-NEXT: cmn {{x[0-9]+}}, 1
// SPEC-NEXT: b.ne [[SLOW:L[0-9]+]]
// SPEC-NEXT: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// ... of cell kind JSArray exactly ...
// SPEC-NEXT: ldrb {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-NEXT: cmp {{w[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
// SPEC-NEXT: b.ne [[SLOW]]
// ... and NOTHING about the object's flags. This is the one guard
// putbyval-inline-emitted-arm64.js pins at exactly this point that the
// load side must not have: the read path needs no
// fastIndexProperties/frozen check at all, because a non-fast indexed
// property leaves its slot EMPTY and the hole test below already
// declines on that (tryFastGetComputedNoAlloc(), JSObject-inline.h). The
// store tier's masked compare is two instructions no other guard in this
// tier emits -- a w16 mask materialization and an `and` against it --
// so their absence between the kind guard and the key conversion is
// checkable directly.
// SPEC-NOT: mov w16, 0x14
// SPEC-NOT: and {{w[0-9]+}}, {{w[0-9]+}}, w16
// ... a key that converts to a uint32 and back unchanged. There is no
// second exit for a NaN key, unlike x86-64's parity check: an unordered
// fcmp leaves Z clear, so the b.ne below already takes it.
// SPEC: fcvtzu {{w[0-9]+}}, {{d[0-9]+}}
// SPEC-NEXT: ucvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC-NEXT: fcmp {{d[0-9]+}}, {{d[0-9]+}}
// SPEC-NEXT: b.ne [[SLOW]]
// ... which is not 0xFFFFFFFF; cmn sets Z exactly when the index wraps.
// SPEC-NEXT: cmn {{w[0-9]+}}, 1
// SPEC-NEXT: b.eq [[SLOW]]
// ... and lies in [beginIndex_, beginIndex_ + elemCount_). Out of range
// is a DECLINE and not an inline `undefined`, unlike the typed-array
// tier's own bounds check: the prototype chain may carry an indexed
// property there.
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
// The HOLE CHECK, which is this tier's own difference from the store
// tier's and the reason it cannot call emit_shv_load_is_empty(). The
// loaded bits are the value this tier must go on to DECODE, so where a
// slot is eight bytes the ETag shift has to land in a register of its
// OWN: emit_shv_load_is_empty() shifts in place
// (emit_sh_ljs_is_empty(a, xTempReg, xTempReg)), which would leave the
// result register holding the shifted ETag instead of the element, and
// every non-hole element would then decode garbage.
//
// The CHECK-NOT below is what gives that a pin rather than a comment:
// [[EL]] is the register the element was loaded into, and between the
// shift and the branch the ONLY comparison allowed is one against some
// OTHER register. Aliasing the temp with the input makes the emitted
// `cmn` read [[EL]], which this rejects by name. (The positive `cmn`
// cannot carry the check itself: a CHECK-NOT's window ends where the
// next positive directive matches, so a `cmn` directive would exclude
// the very line under test.)
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
// Under the two boxed modes it is the full Emit_sh_shv_decode dispatch,
// the same one getOwnBySlotIdx() emits for a property slot; only its
// entry is pinned here, and HV32's extra shift of an inline "compressed
// HV64" up into place is what distinguishes the two.
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
// _sh_ljs_get_by_val_rjs here would mean the site records nothing and
// could never be demoted either.
//
// The two arguments past the three the plain helper takes -- the version
// record and the site id -- are set up unconditionally: under AAPCS64 a
// callee that takes fewer arguments simply ignores the extra registers,
// which is what lets one sequence serve both the recording callee and
// the demoted one. Only the two instructions that make the call indirect
// are pinned -- how the slot ADDRESS reaches x16 is loadBits64InGp's
// choice, and depends on where the record happens to live.
// SPEC: [[SLOW]]:
// SPEC: mov w4, {{[0-9]+}}
// SPEC: // call _jit_get_by_val [indirect]
// SPEC: ldr x16, {{\[}}x16]
// SPEC-NEXT: blr x16
