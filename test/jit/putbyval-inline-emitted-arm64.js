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
// Today arm64 only has the tier in the default heap-value mode --
// HERMES_JIT_INLINE_SAFE_STORE excludes compressed pointers and boxed doubles
// there until the SmallHermesValue encoder is ported -- so the tier is pinned
// under SPEC-HV64 (test/lit.cfg's %hv-mode), and the other modes pin only the
// helper call. The notes below on what will hold in every mode describe the
// split this file gets once the tier exists there: instruction shapes,
// ordering, and the immediates that come from the object model rather than from
// the slot width. Field displacements are wildcarded for the same reason the
// x86-64 pin test wildcards them -- they move under compressed pointers. What
// is genuinely HV64-shaped carries the mode prefix instead -- the width of the
// indexed-storage load, of the element-slot load and of the store, the
// eight-byte element scale, and the ETag form of the hole test.
//
// Stage 5c has to re-split this file, not merely add prefixes to it: under
// compressed pointers the hole test compares a whole encoded
// SmallHermesValue instead of an ETag, and the jumbo gate gains an `and` that
// masks the kind out of the 24-bit size field -- a line that has no SPEC-HV32
// counterpart here because no such line is emitted today.
//
// UNSUPPORTED under HERMESVM_SANITIZE_HANDLES (the `handle_san` lit feature)
// for the same reason its siblings are: no arm64 handle-sanitized tree exists
// in the matrix, and in the boxed modes that build implies, the encoder the
// arm64 gate is still waiting on declines every number outright -- a
// deliberate emission difference that the behavioral tests, not this file,
// are there to cover.

function store(arr, i, v) {
  arr[i] = v;
}

var a = [0, 1, 2, 3];
for (var i = 0; i < 20; ++i)
  store(a, 1, i);
print(a[1]);
// CHECK: 19

// SPEC-LABEL:store:
// SPEC-HV64: // Inline fast array store
//
// The shape guards, in the order putByValWithReceiver_RJS makes them. arm64
// tests a tag by shifting it down and adding, so "is an object" is
// `asr`+`cmn` rather than x86-64's `sar`+`cmp`.
// SPEC-HV64: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC-HV64: cmn {{x[0-9]+}}, 1
// SPEC-HV64: b.ne [[SLOW:L[0-9]+]]
// SPEC-HV64: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// ... of cell kind JSArray exactly ...
// SPEC-HV64: ldrb {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64: cmp {{w[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
// SPEC-HV64: b.ne [[SLOW]]
// ... whose hidden class has indexedStorage set and hasIndexLikeProperties
// and frozen clear. The mask is three non-adjacent bits, which is not an
// AArch64 logical immediate, so unlike x86-64 it has to be materialized
// first.
// SPEC-HV64: ldrh {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64: mov {{w[0-9]+}}, 0x884
// SPEC-HV64: and {{w[0-9]+}}, {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-HV64: cmp {{w[0-9]+}}, 0x800
// SPEC-HV64: b.ne [[SLOW]]
// ... a key that converts to a uint32 and back unchanged. There is no second
// exit for a NaN key, unlike x86-64's parity check: an unordered fcmp leaves
// Z clear, so the b.ne below already takes it.
// SPEC-HV64: fcvtzu {{w[0-9]+}}, {{d[0-9]+}}
// SPEC-HV64: ucvtf {{d[0-9]+}}, {{w[0-9]+}}
// SPEC-HV64: fcmp {{d[0-9]+}}, {{d[0-9]+}}
// SPEC-HV64: b.ne [[SLOW]]
// ... which is not 0xFFFFFFFF; cmn sets Z exactly when the index wraps.
// SPEC-HV64: cmn {{w[0-9]+}}, 1
// SPEC-HV64: b.eq [[SLOW]]
// ... and lies in [beginIndex_, beginIndex_ + elemCount_) ...
// SPEC-HV64: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64: sub {{w[0-9]+}}, {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-HV64: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64: cmp {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-HV64: b.hs [[SLOW]]
// ... in a storage cell that is not a jumbo cell: its allocated size, biased
// by one so that the "size too large to record" encoding of 0 is rejected
// too, must be below RuntimeOffsets::kMaxInlineStorage. This gate is what
// satisfies the barrier predicate's first-unit precondition; removing it
// makes putbyval-inline.js fail on HadesGC's verifyCardTable().
// The indexed-storage load is a compressed-pointer load: eight bytes here,
// four under compressed pointers, which is why it carries a mode prefix while
// the cell-header load below it does not.
// SPEC-HV64: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64: ldr {{w[0-9]+}}, [{{x[0-9]+}}]
// SPEC-HV64: sub {{w[0-9]+}}, {{w[0-9]+}}, 1
// SPEC-HV64: mov {{w[0-9]+}}, 0x3EBDFF
// SPEC-HV64: cmp {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-HV64: b.hi [[SLOW]]
// The element address: the index scaled by the width of one slot, which is
// eight bytes in this mode, then the offset of the first element. Only the
// scale is pinned; the displacement is where ArrayStorageSmall's elements
// begin, and a GCCell carries two debug-only fields that move it.
// SPEC-HV64: add {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC-HV64: add {{x[0-9]+}}, {{x[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
// ... at an address that does not currently hold a hole. Where a slot is
// eight bytes an inline value holds the HermesValue's bits unshifted, so
// this is HermesValue's own ETag test.
// SPEC-HV64: ldr {{x[0-9]+}}, [{{x[0-9]+}}]
// SPEC-HV64: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x2F
// SPEC-HV64: cmn {{x[0-9]+}}, 0xE
// SPEC-HV64: b.eq [[SLOW]]
//
// Then the shared barrier predicate, whose own shape is pinned by
// putbyid-inline-emitted-arm64.js.
// SPEC-HV64: // Inline store with barrier predicate
// SPEC-HV64: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC-HV64: str {{x[0-9]+}}, {{\[}}{{x[0-9]+}}]
// SPEC-HV64: mov w16, 1
// SPEC-HV64: strb w16, {{\[}}{{x[0-9]+}}, {{x[0-9]+}}]
//
// And the unchanged helper call the guards fall back to.
// SPEC-HV64: [[SLOW]]:
// SPEC: call _sh_ljs_put_by_val_loose_rjs
