/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=SPEC,SPEC-%hv-mode %s
// REQUIRES: jit
// REQUIRES: jit-arch-arm64
// UNSUPPORTED: handle_san

// That the PutById inline tier is EMITTED on arm64, and what it consists of.
// putbyid-inline.js is the file that runs it against a collecting heap on
// every backend; this one only looks at the instructions, which is why it is
// small. Its x86-64 counterpart is test/jit/x86-64/putbyid-inline-emitted.js.
//
// Threshold mode, not -Xjit=force: the tier is emitted only when the site's
// write cache already names a hidden class, which needs the function to have
// run interpreted first. See the header of putbyid-inline.js.
//
// The tier exists in every heap-value mode, and so does this file: the parts
// that are the same in all three are pinned under SPEC, and the parts that
// differ under SPEC-HV64, SPEC-HV32 or SPEC-BOXED, of which exactly one is
// active in a given build (test/lit.cfg's %hv-mode). Two things differ. A
// slot is four bytes wide under compressed pointers and eight otherwise, so
// the store is a word there and a doubleword elsewhere. And under boxed
// doubles a slot holds a SmallHermesValue rather than a HermesValue, so the
// value is encoded -- the dispatch pinned below, whose number arm sends a
// double that does not fit inline to the box arm. Field displacements are
// wildcarded throughout: they move under compressed pointers.
//
// THE ENCODE FOLLOWS THE BARRIER PREDICATE'S DECLINING CHECKS, and the
// order below pins that: once a double that needs a box is boxed inline
// instead of declined, no encode outcome is a cheap early reject any more,
// and the box allocation must follow every check that can decline, so
// that a box is never allocated and abandoned (spec
// 2026-10-29-jit-inline-boxed-double-store-design.md).
//
// UNSUPPORTED under HERMESVM_SANITIZE_HANDLES (the `handle_san` lit feature):
// there the runtime boxes every number so that a SmallHermesValue holding one
// is always a pointer, so the emitter declines the whole inline-value case
// with a bare branch and the tst/lsr sequence pinned below is not emitted at
// all. That is a deliberate configuration difference, not a regression; the
// behavioral tests are what cover a handle-sanitized build.

// `p` is the eighth property, so with six direct slots it lands in the
// indirect property storage -- the slot form that has to load and decode
// propStorage before it has an address to store to.
function make() {
  return {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, p: 0};
}

function setIndirect(o, v) {
  o.p = v;
}

var o = make();
for (var i = 0; i < 20; ++i)
  setIndirect(o, i);
print(o.p);

// SPEC-LABEL:setIndirect:
// SPEC: // Put to object specialization
//
// The guard comes first in every mode: the target must be an object, and its
// hidden class must be the one the write cache recorded, compared by lazy JIT
// id. arm64 tests a tag by shifting it down and adding, so "is an object" is
// `asr`+`cmn` rather than x86-64's `sar`+`cmp`. It is the tier's first exit,
// so it is where the helper label gets its name.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC: cmn {{x[0-9]+}}, 1
// SPEC-HV64: b.ne [[SLOW:L[0-9]+]]
// SPEC-HV32: b.ne [[SLOW:L[0-9]+]]
// SPEC-BOXED: b.ne [[SLOW:L[0-9]+]]
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// SPEC: ldrh {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: cmp {{w[0-9]+}}, {{[0-9]+}}
// SPEC: b.ne [[SLOW]]
//
// An indirect slot: load the property storage, then add the element's byte
// offset within it. A direct slot is the same `add` without the load. The
// load is a compressed-pointer load, four bytes wide under compressed
// pointers and eight otherwise.
// SPEC-HV64: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV32: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-BOXED: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: add {{x[0-9]+}}, {{x[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
//
// The barrier predicate. Every exit before the store goes to the helper
// without storing, or, for a young target, to the store. Unlike x86-64,
// which compares against memory, each runtime word tested here is loaded
// into x16, the backend's non-allocated scratch, first: one temporary holds
// the segment start, and the other two belong to the encode and the box
// arm.
// SPEC: // Inline store with barrier predicate
// SPEC: and [[SEG:x[0-9]+]], {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC: ldr x16, [x19, {{[0-9]+}}]
// SPEC: cmp [[SEG]], x16
// SPEC: b.eq [[YOUNG:L[0-9]+]]
// SPEC: ldrb w16, [x19, {{[0-9]+}}]
// SPEC: cbnz w16, [[SLOW]]
// SPEC: ldr x16, [x19, {{[0-9]+}}]
// SPEC: cmp x16, 1
// SPEC: b.ne [[SLOW]]
// SPEC: ldrh w16, {{\[}}[[SEG]], 4]
// SPEC: cmp w16, 1
// SPEC: b.ne [[SLOW]]
//
// Under boxed doubles the young exit lands on the join, after the last
// declining check, where the value is encoded into a SmallHermesValue,
// mirroring HermesValue32::encodeHermesValue(): the ETag selects between the
// inline ("compressed HV64") case, the three pointer tags and Symbol. arm64
// dispatches with `cmn` against the negated ETag and tests
// inline-representability with one `tst` against a mask of low ones. A
// double whose low bits are not zero goes to the box arm; only a value with
// no SmallHermesValue encoding (RawHV32) still declines. Nothing at all is
// emitted for this in HV64, not even the comment.
// SPEC-HV32: {{^}}[[YOUNG]]:
// SPEC-HV32-NEXT: // Encode the value as a SmallHermesValue
// SPEC-HV32-NEXT: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x2F
// SPEC-HV32-NEXT: cmn {{x[0-9]+}}, 0xA
// SPEC-HV32-NEXT: b.hi [[NOTNUM:L[0-9]+]]
// The compression test: with a 29-bit payload, the low 35 bits must be zero.
// SPEC-HV32: tst {{x[0-9]+}}, 0x7FFFFFFFF
// SPEC-HV32-NEXT: b.ne [[BOX:L[0-9]+]]{{$}}
// SPEC-HV32: lsr {{x[0-9]+}}, {{x[0-9]+}}, 0x20
// SPEC-HV32: {{^}}[[NOTNUM]]:
// SPEC-HV32: cmn {{x[0-9]+}}, 6
// SPEC-HV32: b.lo [[SYM:L[0-9]+]]
// A pointer: the HermesValue tag biased into the HV32 one, or-ed into the
// compressed pointer.
// SPEC-HV32: asr {{x[0-9]+}}, {{x[0-9]+}}, 1
// SPEC-HV32: add {{x[0-9]+}}, {{x[0-9]+}}, 4
// SPEC-HV32: sub {{x[0-9]+}}, {{x[0-9]+}}, x19
// SPEC-HV32: orr {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-HV32: {{^}}[[SYM]]:
// SPEC-HV32: cmn {{x[0-9]+}}, 9
// SPEC-HV32: b.ne [[SLOW]]
// The symbol tag goes in with an `add`, not an `orr`: the shift has just
// cleared the low three bits and 0b101 is not an AArch64 logical immediate.
// SPEC-HV32: mov {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-HV32: lsl {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC-HV32: add {{x[0-9]+}}, {{x[0-9]+}}, 5
//
// The same dispatch in the boxed build without compressed pointers: a slot
// is a full eight bytes there, so the payload is 61 bits, the compression
// test is on the low three bits alone, the inline case needs no shift at
// all, and a pointer needs no compression either.
// SPEC-BOXED: {{^}}[[YOUNG]]:
// SPEC-BOXED-NEXT: // Encode the value as a SmallHermesValue
// SPEC-BOXED-NEXT: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x2F
// SPEC-BOXED-NEXT: cmn {{x[0-9]+}}, 0xA
// SPEC-BOXED-NEXT: b.hi [[NOTNUM:L[0-9]+]]
// SPEC-BOXED: tst {{x[0-9]+}}, 7
// SPEC-BOXED-NEXT: b.ne [[BOX:L[0-9]+]]{{$}}
// SPEC-BOXED: mov {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-BOXED: {{^}}[[NOTNUM]]:
// SPEC-BOXED: cmn {{x[0-9]+}}, 6
// SPEC-BOXED: b.lo [[SYM:L[0-9]+]]
// SPEC-BOXED: asr {{x[0-9]+}}, {{x[0-9]+}}, 1
// SPEC-BOXED: add {{x[0-9]+}}, {{x[0-9]+}}, 4
// SPEC-BOXED: orr {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-BOXED: {{^}}[[SYM]]:
// SPEC-BOXED: cmn {{x[0-9]+}}, 9
// SPEC-BOXED: b.ne [[SLOW]]
// SPEC-BOXED: mov {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-BOXED: lsl {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC-BOXED: add {{x[0-9]+}}, {{x[0-9]+}}, 5
//
// The store itself: one slot wide, which is four bytes only under compressed
// pointers. Under boxed doubles a young target, which reached it through the
// join, is then done: no barrier for any value.
// SPEC-HV64: str [[VAL:x[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-HV32: str [[VAL:w[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-HV32-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-HV32-NEXT: cmp [[SEG]], x16
// SPEC-HV32-NEXT: b.eq {{L[0-9]+$}}
// SPEC-BOXED: str [[VAL:x[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-BOXED-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-BOXED-NEXT: cmp [[SEG]], x16
// SPEC-BOXED-NEXT: b.eq {{L[0-9]+$}}
//
// The card decision for an inline value, reached only for an old target
// holding a pointer to a young cell. The pointer test is made on the
// original HermesValue in every mode, which is why its tag compare is the
// same everywhere.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC: cmn {{x[0-9]+}}, 3
// SPEC: b.lo [[DONE:L[0-9]+]]
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC: ldr x16, [x19, {{[0-9]+}}]
// SPEC: cmp {{x[0-9]+}}, x16
// SPEC: b.ne [[DONE]]
//
// Under boxed doubles the inline path then branches to the card block it
// shares with the box arm, which follows at once (no counting block without
// -Xjit-emit-counters). The box arm: bump-allocate (a full young generation
// goes to the helper), write the double, tag the pointer, store it from the
// same register into the same slot, and recompute the segment start into
// the same register (it was an allocation temporary): a young target is
// done, an old one always gets its card.
// SPEC-HV32: {{^ *}}b [[DIRTY:L[0-9]+]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[BOX]]:
// SPEC-HV32-NEXT: // Box the double in the young generation
// SPEC-HV32-NEXT: ldr [[CELL:x[0-9]+]], [x19, {{[0-9]+}}]
// SPEC-HV32: b.hi [[SLOW]]{{$}}
// SPEC-HV32: str {{x[0-9]+}}, {{\[}}[[CELL]], {{[0-9]+}}]
// SPEC-HV32-NEXT: sub [[CELL]], [[CELL]], x19
// SPEC-HV32-NEXT: orr [[CELL]], [[CELL]], {{(0x)?}}4
// SPEC-HV32-NEXT: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC-HV32-NEXT: and [[SEG]], {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC-HV32-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-HV32-NEXT: cmp [[SEG]], x16
// SPEC-HV32-NEXT: b.eq [[DONE]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[DIRTY]]:
// SPEC-BOXED: {{^ *}}b [[DIRTY:L[0-9]+]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[BOX]]:
// SPEC-BOXED-NEXT: // Box the double in the young generation
// SPEC-BOXED-NEXT: ldr [[CELL:x[0-9]+]], [x19, {{[0-9]+}}]
// SPEC-BOXED: b.hi [[SLOW]]{{$}}
// SPEC-BOXED: str {{x[0-9]+}}, {{\[}}[[CELL]], {{[0-9]+}}]
// SPEC-BOXED-NEXT: orr [[CELL]], [[CELL]], {{(0x)?}}4
// SPEC-BOXED-NEXT: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC-BOXED-NEXT: and [[SEG]], {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC-BOXED-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-BOXED-NEXT: cmp [[SEG]], x16
// SPEC-BOXED-NEXT: b.eq [[DONE]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[DIRTY]]:
//
// The card-dirty store: the byte at segment start + (slot - segment start)
// >> 9.
// SPEC: sub {{x[0-9]+}}, {{x[0-9]+}}, [[SEG]]
// SPEC: lsr {{x[0-9]+}}, {{x[0-9]+}}, 9
// SPEC: mov w16, 1
// SPEC: strb w16, {{\[}}[[SEG]], {{x[0-9]+}}]
// In HV64 the card block jumps over the young-target store that follows it.
// SPEC-HV64: b [[DONE]]
// SPEC-HV64: [[YOUNG]]:
// SPEC-HV64: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC: [[DONE]]:
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _jit_put_by_id
