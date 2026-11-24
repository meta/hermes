/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=SPEC,SPEC-%hv-mode %s
// REQUIRES: jit
// REQUIRES: jit-arch-arm64
// UNSUPPORTED: handle_san

// That the PutByVal inline store tiers are EMITTED on arm64, and what they
// consist of: the unconditional fast array store, the recording helper call
// the guards fall back to, and -- at a site whose recorded shape earned one --
// the typed-array store tier. putbyval-inline.js and taval-conversions.js are
// the files that run these against a collecting heap on every backend; this
// one only looks at the instructions. Its x86-64 counterpart is
// test/jit/x86-64/putbyval-inline-emitted.js.
//
// -Xjit=force is enough for the fast array tier, unlike
// putbyid-inline-emitted-arm64.js: it reads no property cache, so it does not
// need the function to have run interpreted first. Every one of its guards is
// dynamic. The typed-array tier is different -- it is emitted only in a
// RECOMPILE, once the recording slow path has observed the site's kind --
// which is what the recompile threshold on the RUN lines is for.
//
// The tier exists in every heap-value mode, and so does this file: what is
// the same in all three is pinned under plain SPEC, and what differs under
// SPEC-HV64, SPEC-HV32 or SPEC-BOXED, of which exactly one is active in a
// given build (test/lit.cfg's %hv-mode). Field displacements are wildcarded
// throughout, because they move under compressed pointers.
//
// Four things are genuinely mode-shaped here, and all four are consequences
// of the slot width rather than of the encoding: the width of the
// indexed-storage load, the presence of the mask that clears the cell kind
// out of a 24-bit size field, the element scale, and the form of the hole
// test -- an ETag test where a slot is eight bytes, a comparison against the
// whole encoded empty value where it is four. On top of those, the two boxed
// modes encode the value inside the barrier predicate and box a double that
// needs it; the encode's dispatch is pinned by putbyid-inline-emitted-arm64.js,
// its position and the box arm here too.
//
// UNSUPPORTED under HERMESVM_SANITIZE_HANDLES (the `handle_san` lit feature)
// for the same reason its siblings are: there the runtime boxes every number,
// so the encoder declines the whole inline-value case and the emitted shape
// differs deliberately. The behavioral tests are what cover such a build.

function store(arr, i, v) {
  arr[i] = v;
}

var a = [0, 1, 2, 3];
for (var i = 0; i < 20; ++i)
  store(a, 1, i);
print(a[1]);
// CHECK: 19

// Two more store sites, each fed one typed-array kind so that its record
// names that kind and its recompile emits the tier. They are separate
// functions because a shared one would specialize on whichever kind reached
// it first and poison the other. The warm loops live in functions of their
// own, so the only thing running per iteration is locals and parameters.
function taStoreI32(ta, i, v) {
  ta[i] = v;
}
function taStoreF32(ta, i, v) {
  ta[i] = v;
}
function warmI32(ta, n) {
  for (var i = 0; i < n; ++i)
    taStoreI32(ta, 1, i + 0.5);
}
function warmF32(ta, n) {
  for (var i = 0; i < n; ++i)
    taStoreF32(ta, 1, i + 0.5);
}
// Nonzero-offset views: the address each store computes then has every term
// nonzero, so a tier that dropped offset_ or mis-scaled the index would be
// caught by the interpreter diff as well as by the instructions below.
var i32 = new Int32Array(new ArrayBuffer(64), 8, 4);
warmI32(i32, 70);
print(i32[1]);
// CHECK: 69

var f32 = new Float32Array(new ArrayBuffer(64), 8, 4);
warmF32(f32, 70);
print(f32[1]);
// CHECK: 69.5

// SPEC-LABEL:store:
// SPEC: // Inline fast array store
//
// In every mode the object check below is the tier's first exit, so it is
// where the helper label gets its name: the value is encoded inside the
// barrier predicate, not ahead of the guards.
//
// The shape guards, in the order putByValWithReceiver_RJS makes them. arm64
// tests a tag by shifting it down and adding, so "is an object" is
// `asr`+`cmn` rather than x86-64's `sar`+`cmp`.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC: cmn {{x[0-9]+}}, 1
// SPEC-HV64: b.ne [[SLOW:L[0-9]+]]
// SPEC-HV32: b.ne [[SLOW:L[0-9]+]]
// SPEC-BOXED: b.ne [[SLOW:L[0-9]+]]
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// ... of cell kind JSArray exactly ...
// SPEC: ldrb {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: cmp {{w[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
// SPEC: b.ne [[SLOW]]
// ... whose hidden class has indexedStorage set and hasIndexLikeProperties
// and frozen clear. The mask is three non-adjacent bits, which is not an
// AArch64 logical immediate, so unlike x86-64 it has to be materialized
// first -- in x16, the backend's non-allocated scratch.
// SPEC: ldrh {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: mov w16, 0x884
// SPEC: and {{w[0-9]+}}, {{w[0-9]+}}, w16
// SPEC: cmp {{w[0-9]+}}, 0x800
// SPEC: b.ne [[SLOW]]
// ... a key that converts to a uint32 and back unchanged. There is no second
// exit for a NaN key, unlike x86-64's parity check: an unordered fcmp leaves
// Z clear, so the b.ne below already takes it.
// SPEC: fcvtzu {{w[0-9]+}}, {{d[0-9]+}}
// SPEC: ucvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC: fcmp {{d[0-9]+}}, {{d[0-9]+}}
// SPEC: b.ne [[SLOW]]
// ... which is not 0xFFFFFFFF; cmn sets Z exactly when the index wraps.
// SPEC: cmn {{w[0-9]+}}, 1
// SPEC: b.eq [[SLOW]]
// ... and lies in [beginIndex_, beginIndex_ + elemCount_) ...
// SPEC: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: sub {{w[0-9]+}}, {{w[0-9]+}}, {{w[0-9]+}}
// SPEC: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: cmp {{w[0-9]+}}, {{w[0-9]+}}
// SPEC: b.hs [[SLOW]]
// ... in a storage cell that is not a jumbo cell: its allocated size, biased
// by one so that the "size too large to record" encoding of 0 is rejected
// too, must be below RuntimeOffsets::kMaxInlineStorage. This gate is what
// satisfies the barrier predicate's first-unit precondition; removing it
// makes putbyval-inline.js fail on HadesGC's verifyCardTable().
//
// The indexed-storage load is a compressed-pointer load: four bytes under
// compressed pointers, eight otherwise. The cell-header load below it is
// always 32 bits, but KindAndSize packs an 8-bit kind above the size, so
// where that header is only four bytes wide the kind has to be masked off
// before the size compare.
// SPEC-HV64: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64: ldr {{w[0-9]+}}, [{{x[0-9]+}}]
// SPEC-HV32: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV32: add {{x[0-9]+}}, x19, {{x[0-9]+}}
// SPEC-HV32: ldr {{w[0-9]+}}, [{{x[0-9]+}}]
// SPEC-HV32: and {{w[0-9]+}}, {{w[0-9]+}}, 0xFFFFFF
// SPEC-BOXED: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-BOXED: ldr {{w[0-9]+}}, [{{x[0-9]+}}]
// SPEC: sub {{w[0-9]+}}, {{w[0-9]+}}, 1
// SPEC: mov w16, 0x3EBDFF
// SPEC: cmp {{w[0-9]+}}, w16
// SPEC: b.hi [[SLOW]]
// The element address: the index scaled by the width of one slot, four bytes
// under compressed pointers and eight otherwise, then the offset of the first
// element. Only the scale is pinned; the displacement is where
// ArrayStorageSmall's elements begin, and a GCCell carries two debug-only
// fields that move it.
// SPEC-HV64: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC-HV32: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}, 2
// SPEC-BOXED: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC: add {{x[0-9]+}}, {{x[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
// ... at an address that does not currently hold a hole. Where a slot is
// eight bytes an inline value holds the HermesValue's bits unshifted, so this
// is HermesValue's own ETag test; where it is four those bits have been
// shifted down out of ETag position, so the whole encoded empty value is
// compared instead.
// SPEC-HV64: ldr {{x[0-9]+}}, [{{x[0-9]+}}]
// SPEC-HV64: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x2F
// SPEC-HV64: cmn {{x[0-9]+}}, 0xE
// SPEC-HV32: ldr {{w[0-9]+}}, [{{x[0-9]+}}]
// SPEC-HV32: mov w16, 0xFFF90000
// SPEC-HV32: cmp {{w[0-9]+}}, w16
// SPEC-BOXED: ldr {{x[0-9]+}}, [{{x[0-9]+}}]
// SPEC-BOXED: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x2F
// SPEC-BOXED: cmn {{x[0-9]+}}, 0xE
// SPEC: b.eq [[SLOW]]
//
// Then the shared barrier predicate, whose own shape is pinned by
// putbyid-inline-emitted-arm64.js.
// SPEC: // Inline store with barrier predicate
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// Under boxed doubles a young target jumps to the join, where the value is
// encoded once every declining check has run; a double with no inline
// encoding goes to the box arm rather than to the helper.
// SPEC-HV32: b.eq [[JOIN:L[0-9]+]]{{$}}
// SPEC-HV32: {{^}}[[JOIN]]:
// SPEC-HV32-NEXT: // Encode the value as a SmallHermesValue
// SPEC-HV32: tst {{x[0-9]+}}, 0x7FFFFFFFF
// SPEC-HV32-NEXT: b.ne [[BOX:L[0-9]+]]{{$}}
// SPEC-BOXED: b.eq [[JOIN:L[0-9]+]]{{$}}
// SPEC-BOXED: {{^}}[[JOIN]]:
// SPEC-BOXED-NEXT: // Encode the value as a SmallHermesValue
// SPEC-BOXED: tst {{x[0-9]+}}, 7
// SPEC-BOXED-NEXT: b.ne [[BOX:L[0-9]+]]{{$}}
// SPEC-HV64: str {{x[0-9]+}}, {{\[}}{{x[0-9]+}}]
// SPEC-HV32: str {{w[0-9]+}}, {{\[}}{{x[0-9]+}}]
// SPEC-BOXED: str {{x[0-9]+}}, {{\[}}{{x[0-9]+}}]
// The inline value's path ends in a jump to the card block it shares with
// the box arm, which follows at once (no counting block without
// -Xjit-emit-counters). The box arm bump-allocates the BoxedDouble -- a
// full young generation goes to the helper -- writes the double, tags the
// (under HV32, compressed) pointer, stores it, and dirties an old target's
// card unconditionally: a box is always young.
// SPEC-HV32: {{^ *}}b [[DIRTY:L[0-9]+]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[BOX]]:
// SPEC-HV32-NEXT: // Box the double in the young generation
// SPEC-HV32-NEXT: ldr [[CELL:x[0-9]+]], [x19, {{[0-9]+}}]
// SPEC-HV32: b.hi [[SLOW]]{{$}}
// SPEC-HV32: str {{x[0-9]+}}, {{\[}}[[CELL]], {{[0-9]+}}]
// SPEC-HV32-NEXT: sub [[CELL]], [[CELL]], x19
// SPEC-HV32-NEXT: orr [[CELL]], [[CELL]], {{(0x)?}}4
// SPEC-HV32-NEXT: str {{w[0-9]+}}, {{\[}}{{x[0-9]+}}]
// SPEC-HV32-NEXT: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC-HV32-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-HV32-NEXT: cmp {{x[0-9]+}}, x16
// SPEC-HV32-NEXT: b.eq [[DONE:L[0-9]+]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[DIRTY]]:
// SPEC-BOXED: {{^ *}}b [[DIRTY:L[0-9]+]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[BOX]]:
// SPEC-BOXED-NEXT: // Box the double in the young generation
// SPEC-BOXED-NEXT: ldr [[CELL:x[0-9]+]], [x19, {{[0-9]+}}]
// SPEC-BOXED: b.hi [[SLOW]]{{$}}
// SPEC-BOXED: str {{x[0-9]+}}, {{\[}}[[CELL]], {{[0-9]+}}]
// SPEC-BOXED-NEXT: orr [[CELL]], [[CELL]], {{(0x)?}}4
// SPEC-BOXED-NEXT: str [[CELL]], {{\[}}{{x[0-9]+}}]
// SPEC-BOXED-NEXT: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC-BOXED-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-BOXED-NEXT: cmp {{x[0-9]+}}, x16
// SPEC-BOXED-NEXT: b.eq [[DONE:L[0-9]+]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[DIRTY]]:
// SPEC: mov w16, 1
// SPEC: strb w16, {{\[}}{{x[0-9]+}}, {{x[0-9]+}}]
// SPEC-HV32-NEXT: {{^}}[[DONE]]:
// SPEC-BOXED-NEXT: {{^}}[[DONE]]:
//
// And the helper call the guards fall back to: an INDIRECT call through this
// site's own mutable helper slot (spec: "Slow-path demotion: the pointer
// flip"), which currently holds the recording helper that forwards to
// _sh_ljs_put_by_val_loose_rjs after recording the site's observed shape. The
// slot's address is a constant this body embeds; the callee is whatever the
// slot holds when the call runs, so the load of it is a second, dependent
// load. Only the two instructions that make the call indirect are pinned --
// how the slot ADDRESS reaches x16 is loadBits64InGp's choice, and depends on
// where the record happens to live.
// SPEC: [[SLOW]]:
// SPEC: // call _jit_put_by_val_loose [indirect]
// SPEC: ldr x16, {{\[}}x16]
// SPEC-NEXT: blr x16

// ---- The typed-array store tier ----
//
// Each section is anchored on its function's version-2 compile banner rather
// than on a label: a recompiled function prints its name line once per
// version, so a label on it would not be unique. Within a section every
// instruction of a guard is pinned with SPEC-NEXT, which is what keeps these
// pins honest -- the key round trip below emits an `fcmp` of its own, and a
// floating pattern that failed on the value guard would simply scan forward
// and match that one instead.
//
// Int32Array is CellKind 39. The integer value conversion is the part of this
// tier that is genuinely arm64's own: x86-64 converts first and recognizes
// its conversion's out-of-range sentinel, which saturation makes unsound
// here, so the guards come first and the conversion is then exact for
// everything they admit.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'taStoreI32' (version 2)
// SPEC: // Inline typed array store (kind 39)
// The target must be an object ...
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC-NEXT: cmn {{x[0-9]+}}, 1
// SPEC-NEXT: b.ne [[TASLOW:L[0-9]+]]
// ... of exactly the recorded kind. A kind miss goes to a DIFFERENT label
// from every other exit: at this duo site it chains into the JSArray tier,
// which handles the shapes this tier cannot.
// SPEC: ldrb {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-NEXT: cmp {{w[0-9]+}}, 0x27
// SPEC-NEXT: b.ne [[TAKINDMISS:L[0-9]+]]
// ... whose hidden class has fast index properties and is not frozen, the
// same masked compare the fast array tier makes, and through x16 for the same
// reason.
// SPEC: ldrh {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-NEXT: mov w16, 0x884
// SPEC-NEXT: and {{w[0-9]+}}, {{w[0-9]+}}, w16
// SPEC-NEXT: cmp {{w[0-9]+}}, 0x800
// SPEC-NEXT: b.ne [[TASLOW]]
// The value conversion, which is also the "is this a number this element type
// can hold" guard. An unordered self-compare declines every NaN, which is
// every non-number -- without it a store of an object would skip the
// interpreter's ToNumber, valueOf and all (taval-conversions.js is where that
// is checked behaviorally).
// SPEC-NEXT: fcmp [[V:d[0-9]+]], [[V]]
// SPEC-NEXT: b.vs [[TASLOW]]
// Then the magnitude guard: 2^63 as a double, materialized once by this tier,
// against the absolute value. It is what keeps fcvtzs inside the range where
// it truncates rather than saturates, so it declines both infinities and
// everything at or beyond 2^63 -- exactly the values x86-64's sentinel
// compare declines.
// SPEC-NEXT: mov {{x[0-9]+}}, 0x43E0000000000000
// SPEC-NEXT: fmov [[LIMIT:d[0-9]+]], {{x[0-9]+}}
// SPEC-NEXT: fabs [[ABS:d[0-9]+]], [[V]]
// SPEC-NEXT: fcmp [[ABS]], [[LIMIT]]
// SPEC-NEXT: b.ge [[TASLOW]]
// And the conversion itself: a plain truncation toward zero, with no round
// trip proving the value was an integer to begin with. A fractional value is
// STORED here, truncated, exactly as the interpreter stores it --
// recompile-taval-fractional.js is what pins that, on every backend.
// SPEC-NEXT: fcvtzs [[INT:x[0-9]+]], [[V]]
// The key, which unlike the value must be exact: convert to uint32 and back
// and compare. One exit covers every rejection, a NaN key included.
// SPEC-NEXT: fcvtzu [[IDX:w[0-9]+]], [[K:d[0-9]+]]
// SPEC-NEXT: ucvtf [[KTMP:d[0-9]+]], [[IDX]]
// SPEC-NEXT: fcmp [[KTMP]], [[K]]
// SPEC-NEXT: b.ne [[TASLOW]]
// Bounds against length_, unsigned, which rejects 0xFFFFFFFF too -- so unlike
// the JSArray tier there is no separate exclusion of it.
// SPEC-NEXT: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-NEXT: cmp [[IDX]], {{w[0-9]+}}
// SPEC-NEXT: b.hs [[TASLOW]]
// The element base: the view's byte offset_ (in a real temp, never x16), the
// buffer, its data_ -- null exactly when the buffer is detached, which is why
// the attached check is free.
// SPEC-NEXT: ldr [[OFS:w[0-9]+]], [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64-NEXT: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV32-NEXT: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV32-NEXT: add {{x[0-9]+}}, x19, {{x[0-9]+}}
// SPEC-BOXED-NEXT: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: ldr [[BASE:x[0-9]+]], [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-NEXT: cbz [[BASE]], [[TASLOW]]
// SPEC-NEXT: add [[BASE]], [[BASE]], {{x[0-9]+}}
// The store: the truncated value's low 32 bits, at an ELEMENT index scaled by
// this kind's width.
// SPEC-NEXT: str {{w[0-9]+}}, {{\[}}[[BASE]], [[IDX]] uxtw 2]
// Falling out of the tier means the store is done; the kind miss lands on the
// JSArray tier, whose comment is the next thing emitted.
// SPEC-NEXT: b {{L[0-9]+}}
// SPEC-NEXT: [[TAKINDMISS]]:
// SPEC-NEXT: // Inline fast array store
//
// Float32Array is CellKind 41: the same guards, but the value needs no
// integer conversion at all -- only the NaN exit, which is there because the
// helper stores the canonical NaN and these raw bits need not be it -- and
// then a NARROWING convert whose single-precision result is what gets stored.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'taStoreF32' (version 2)
// SPEC: // Inline typed array store (kind 41)
// SPEC: cmp {{w[0-9]+}}, 0x29
// SPEC-NEXT: b.ne {{L[0-9]+}}
// SPEC: fcmp [[FV:d[0-9]+]], [[FV]]
// SPEC-NEXT: b.vs [[FSLOW:L[0-9]+]]
// SPEC-NEXT: fcvt [[FS:s[0-9]+]], [[FV]]
// SPEC-NEXT: fcvtzu [[FIDX:w[0-9]+]], {{d[0-9]+}}
// SPEC: str [[FS]], {{\[}}{{x[0-9]+}}, [[FIDX]] uxtw 2]
