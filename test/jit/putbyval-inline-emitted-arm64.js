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
// UNSUPPORTED: handle_san

// That the PutByVal inline fast array store is EMITTED on arm64, and what it
// consists of. putbyval-inline.js is the file that runs it against a
// collecting heap on every backend; this one only looks at the instructions,
// which is why it is small. Its x86-64 counterpart is
// test/jit/x86-64/putbyval-inline-emitted.js.
//
// -Xjit=force is enough here, unlike putbyid-inline-emitted-arm64.js: this
// tier reads no property cache, so it does not need the function to have run
// interpreted first. Every one of its guards is dynamic.
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
// modes encode the value into a SmallHermesValue before any guard runs; the
// shape of that dispatch is pinned by putbyid-inline-emitted-arm64.js and
// only its comment line is checked here.
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

// SPEC-LABEL:store:
// SPEC: // Inline fast array store
//
// Under boxed doubles the value is encoded into a SmallHermesValue first,
// ahead of every guard, so that a value with no inline encoding is rejected
// before the tier pays for anything else. Nothing is emitted here in HV64.
// SPEC-HV32: // Encode the value as a SmallHermesValue
// SPEC-HV32: b.ne [[SLOW:L[0-9]+]]
// SPEC-BOXED: // Encode the value as a SmallHermesValue
// SPEC-BOXED: b.ne [[SLOW:L[0-9]+]]
//
// The shape guards, in the order putByValWithReceiver_RJS makes them. arm64
// tests a tag by shifting it down and adding, so "is an object" is
// `asr`+`cmn` rather than x86-64's `sar`+`cmp`.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC: cmn {{x[0-9]+}}, 1
// SPEC-HV64: b.ne [[SLOW:L[0-9]+]]
// SPEC-HV32: b.ne [[SLOW]]
// SPEC-BOXED: b.ne [[SLOW]]
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// ... of cell kind JSArray exactly ...
// SPEC: ldrb {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: cmp {{w[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
// SPEC: b.ne [[SLOW]]
// ... whose hidden class has indexedStorage set and hasIndexLikeProperties
// and frozen clear. The mask is three non-adjacent bits, which is not an
// AArch64 logical immediate, so unlike x86-64 it has to be materialized
// first -- in x16, because the other temporary is carrying the encoded value
// in the two boxed modes.
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
// SPEC-HV64: str {{x[0-9]+}}, {{\[}}{{x[0-9]+}}]
// SPEC-HV32: str {{w[0-9]+}}, {{\[}}{{x[0-9]+}}]
// SPEC-BOXED: str {{x[0-9]+}}, {{\[}}{{x[0-9]+}}]
// SPEC: mov w16, 1
// SPEC: strb w16, {{\[}}{{x[0-9]+}}, {{x[0-9]+}}]
//
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _sh_ljs_put_by_val_loose_rjs
