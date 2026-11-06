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
// value is encoded first -- the dispatch pinned below, whose one declining
// arm is a double that would have to be boxed on the heap. Field
// displacements are wildcarded throughout: they move under compressed
// pointers.
//
// THE ENCODE COMES FIRST, ahead of every guard, and the order below pins
// that. It is not cosmetic: a value with no inline encoding is the cheapest
// thing this tier can reject, and rejecting it before the guards is what
// keeps a store that will decline from paying for the class check as well.
// On Box2D two thirds of the stores reaching these tiers decline.
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
// Under boxed doubles the value is encoded into a SmallHermesValue before
// anything else, mirroring HermesValue32::encodeHermesValue(): the ETag
// selects between the inline ("compressed HV64") case, the three pointer tags
// and Symbol. arm64 dispatches with `cmn` against the negated ETag rather
// than x86-64's `cmp` against a sign-extended immediate -- the ETags are
// negative and their negations encode directly -- and tests
// inline-representability with a single `tst` against a mask of low ones,
// which is an AArch64 logical immediate, where x86-64 shifts those bits off
// the top of a copy. The inline case is the one that can fail: a double whose
// low bits are not zero would need a heap-allocated BoxedDouble, and emitted
// code cannot allocate, so it goes to the helper with nothing stored. Nothing
// at all is emitted here in HV64, not even the comment, because there a slot
// holds the HermesValue itself.
// SPEC-HV32: // Encode the value as a SmallHermesValue
// SPEC-HV32: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x2F
// SPEC-HV32: cmn {{x[0-9]+}}, 0xA
// SPEC-HV32: b.hi [[NOTNUM:L[0-9]+]]
// The compression test: with a 29-bit payload, the low 35 bits must be zero.
// SPEC-HV32: tst {{x[0-9]+}}, 0x7FFFFFFFF
// SPEC-HV32: b.ne [[SLOW:L[0-9]+]]
// SPEC-HV32: lsr {{x[0-9]+}}, {{x[0-9]+}}, 0x20
// SPEC-HV32: [[NOTNUM]]:
// SPEC-HV32: cmn {{x[0-9]+}}, 6
// SPEC-HV32: b.lo [[SYM:L[0-9]+]]
// A pointer: the HermesValue tag biased into the HV32 one, or-ed into the
// compressed pointer.
// SPEC-HV32: asr {{x[0-9]+}}, {{x[0-9]+}}, 1
// SPEC-HV32: add {{x[0-9]+}}, {{x[0-9]+}}, 4
// SPEC-HV32: sub {{x[0-9]+}}, {{x[0-9]+}}, x19
// SPEC-HV32: orr {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-HV32: [[SYM]]:
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
// SPEC-BOXED: // Encode the value as a SmallHermesValue
// SPEC-BOXED: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x2F
// SPEC-BOXED: cmn {{x[0-9]+}}, 0xA
// SPEC-BOXED: b.hi [[NOTNUM:L[0-9]+]]
// SPEC-BOXED: tst {{x[0-9]+}}, 7
// SPEC-BOXED: b.ne [[SLOW:L[0-9]+]]
// SPEC-BOXED: mov {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-BOXED: [[NOTNUM]]:
// SPEC-BOXED: cmn {{x[0-9]+}}, 6
// SPEC-BOXED: b.lo [[SYM:L[0-9]+]]
// SPEC-BOXED: asr {{x[0-9]+}}, {{x[0-9]+}}, 1
// SPEC-BOXED: add {{x[0-9]+}}, {{x[0-9]+}}, 4
// SPEC-BOXED: orr {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-BOXED: [[SYM]]:
// SPEC-BOXED: cmn {{x[0-9]+}}, 9
// SPEC-BOXED: b.ne [[SLOW]]
// SPEC-BOXED: mov {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-BOXED: lsl {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC-BOXED: add {{x[0-9]+}}, {{x[0-9]+}}, 5
//
// Only now the guard: the target must be an object, and its hidden class must
// be the one the write cache recorded, compared by lazy JIT id. arm64 tests a
// tag by shifting it down and adding, so "is an object" is `asr`+`cmn` rather
// than x86-64's `sar`+`cmp`. In HV64 this is the first thing in the tier, so
// it is where the helper label gets its name there; in the two boxed modes
// the encode above already named it.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC: cmn {{x[0-9]+}}, 1
// SPEC-HV64: b.ne [[SLOW:L[0-9]+]]
// SPEC-HV32: b.ne [[SLOW]]
// SPEC-BOXED: b.ne [[SLOW]]
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
// The barrier predicate. The young-generation exit stores with no barrier at
// all; every other exit either stores and dirties one card, or declines to
// the helper without storing. Unlike x86-64, which compares against memory,
// each of the runtime words tested here is loaded into a register first, and
// that register is x16 -- the backend's non-allocated scratch -- because
// neither temporary is free: one holds the segment start, the other may be
// carrying the encoded value.
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
// The store itself: one slot wide, which is four bytes only under compressed
// pointers.
// SPEC-HV64: str [[VAL:x[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-HV32: str [[VAL:w[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-BOXED: str [[VAL:x[0-9]+]], {{\[}}{{x[0-9]+}}]
//
// The card-dirty store, reached only for an old target holding a pointer to
// a young cell: the byte at segment start + (slot - segment start) >> 9. The
// pointer test is made on the original HermesValue in every mode, which is
// why its tag compare is the same everywhere.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC: cmn {{x[0-9]+}}, 3
// SPEC: b.lo [[DONE:L[0-9]+]]
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC: ldr x16, [x19, {{[0-9]+}}]
// SPEC: cmp {{x[0-9]+}}, x16
// SPEC: b.ne [[DONE]]
// SPEC: sub {{x[0-9]+}}, {{x[0-9]+}}, [[SEG]]
// SPEC: lsr {{x[0-9]+}}, {{x[0-9]+}}, 9
// SPEC: mov w16, 1
// SPEC: strb w16, {{\[}}[[SEG]], {{x[0-9]+}}]
// SPEC: b [[DONE]]
// SPEC: [[YOUNG]]:
// SPEC-HV64: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC-HV32: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC-BOXED: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC: [[DONE]]:
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _jit_put_by_id
