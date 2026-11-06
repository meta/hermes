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
// UNSUPPORTED: handle_san || gc_malloc
// The PutById inline tier is barrier-gated and Config.h compiles it out
// entirely under MallocGC, so none of the pinned inline instructions
// are emitted; pre-existing, unrelated to GetByVal (loads have no write
// barrier and are never gated).

// That the PutById inline tier is EMITTED, and what it consists of.
// putbyid-inline.js is the file that runs it against a collecting heap; this
// one only looks at the instructions, which is why it is small.
//
// The tier exists in every heap-value mode, and so does this file: the parts
// that are the same in all three are pinned under SPEC, and the parts that
// differ under SPEC-HV64, SPEC-HV32 or SPEC-BOXED, of which exactly one is
// active in a given build (test/lit.cfg's %hv-mode). Two things differ. A
// slot is four bytes wide under compressed pointers and eight otherwise, so
// the store is a dword there and a qword elsewhere. And under boxed doubles
// a slot holds a SmallHermesValue rather than a HermesValue, so the value is
// encoded first -- the dispatch pinned below, whose one declining arm is a
// double that would have to be boxed on the heap.
//
// THE ENCODE COMES FIRST, ahead of every guard, and the order below pins
// that. It is not cosmetic: a value with no inline encoding is the cheapest
// thing this tier can reject, and rejecting it before the guards is what
// keeps a store that will decline from paying for the class check as well.
// On Box2D two thirds of the stores reaching these tiers decline.
//
// Threshold mode, not -Xjit=force: the tier is emitted only when the site's
// write cache already names a hidden class, which needs the function to have
// run interpreted first. See the header of putbyid-inline.js.
//
// UNSUPPORTED under HERMESVM_SANITIZE_HANDLES (the `handle_san` lit
// feature): there the runtime boxes every number so that a SmallHermesValue
// holding one is always a pointer, so the emitter declines the whole
// inline-value case with a bare jump and the shl/shr sequence pinned below
// is not emitted at all. That is a deliberate configuration difference, not
// a regression; the behavioral tests are what cover a handle-sanitized
// build.

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
// CHECK: 19

// SPEC-LABEL:setIndirect:
// SPEC: // Put to object specialization
//
// Under boxed doubles the value is encoded into a SmallHermesValue before
// anything else, mirroring HermesValue32::encodeHermesValue(): the ETag
// selects between the inline ("compressed HV64") case, the three pointer
// tags and Symbol. The inline case is the one that can fail -- a double
// whose low bits are not zero would need a heap-allocated BoxedDouble, and
// emitted code cannot allocate, so it goes to the helper with nothing
// stored. Nothing at all is emitted here in HV64, not even the comment,
// because there a slot holds the HermesValue itself.
// SPEC-HV32: // Encode the value as a SmallHermesValue
// SPEC-HV32: sar {{.*}}, 0x2F
// SPEC-HV32: cmp {{.*}}, 0xFFFFFFFFFFFFFFF6
// SPEC-HV32: ja [[NOTNUM:L[0-9]+]]
// The compression test: with a 29-bit payload, the low 35 bits must be zero.
// SPEC-HV32: shl {{.*}}, 0x1D
// SPEC-HV32: jnz [[SLOW:L[0-9]+]]
// SPEC-HV32: shr {{.*}}, 0x20
// SPEC-HV32: [[NOTNUM]]:
// SPEC-HV32: cmp {{.*}}, 0xFFFFFFFFFFFFFFFA
// SPEC-HV32: jb [[SYM:L[0-9]+]]
// A pointer: the HermesValue tag biased into the HV32 one, or-ed into the
// compressed pointer.
// SPEC-HV32: sar {{.*}}, 1
// SPEC-HV32: add {{.*}}, 4
// SPEC-HV32: sub {{.*}}, r15
// SPEC-HV32: or {{.*}}
// SPEC-HV32: [[SYM]]:
// SPEC-HV32: cmp {{.*}}, 0xFFFFFFFFFFFFFFF7
// SPEC-HV32: jne [[SLOW]]
// SPEC-HV32: shl {{.*}}, 3
// SPEC-HV32: or {{.*}}, 5
//
// The same dispatch in the boxed build without compressed pointers: a slot
// is a full eight bytes there, so the payload is 61 bits, the compression
// test is on the low three bits alone, the inline case needs no shift at
// all, and a pointer needs no compression either.
// SPEC-BOXED: // Encode the value as a SmallHermesValue
// SPEC-BOXED: sar {{.*}}, 0x2F
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFF6
// SPEC-BOXED: ja [[NOTNUM:L[0-9]+]]
// SPEC-BOXED: shl {{.*}}, 0x3D
// SPEC-BOXED: jnz [[SLOW:L[0-9]+]]
// SPEC-BOXED: [[NOTNUM]]:
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFFA
// SPEC-BOXED: jb [[SYM:L[0-9]+]]
// SPEC-BOXED: sar {{.*}}, 1
// SPEC-BOXED: add {{.*}}, 4
// SPEC-BOXED: or {{.*}}
// SPEC-BOXED: [[SYM]]:
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFF7
// SPEC-BOXED: jne [[SLOW]]
// SPEC-BOXED: shl {{.*}}, 3
// SPEC-BOXED: or {{.*}}, 5
//
// Only now the guard: the target must be an object, and its hidden class
// must be the one the write cache recorded, compared by lazy JIT id. In HV64
// this is the first thing in the tier, so it is where the helper label gets
// its name there; in the two boxed modes the encode above already named it.
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFF
// SPEC-HV64: jne [[SLOW:L[0-9]+]]
// SPEC-HV32: jne [[SLOW]]
// SPEC-BOXED: jne [[SLOW]]
// SPEC: movzx {{.*}}, word ptr {{.*}}
// SPEC: cmp {{.*}}
// SPEC: jne [[SLOW]]
//
// The barrier predicate. The young-generation exit stores with no barrier at
// all; every other exit either stores and dirties one card, or declines to
// the helper without storing.
// SPEC: // Inline store with barrier predicate
// SPEC: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC: je [[YOUNG:L[0-9]+]]
// SPEC: cmp byte ptr {{.*}}, 0
// SPEC: jne [[SLOW]]
// SPEC: cmp qword ptr {{.*}}, 1
// SPEC: jne [[SLOW]]
// SPEC: cmp word ptr {{.*}}, 1
// SPEC: jne [[SLOW]]
// The store itself: one slot wide, which is four bytes only under compressed
// pointers.
// SPEC-HV64: mov qword ptr {{.*}}
// SPEC-HV32: mov dword ptr {{.*}}
// SPEC-BOXED: mov qword ptr {{.*}}
// The card-dirty store, reached only for an old target holding a pointer to
// a young cell: the byte at segment start + (slot - segment start) >> 9. The
// pointer test is made on the original HermesValue in every mode, which is
// why its tag compare is the same everywhere.
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFD
// SPEC: shr {{.*}}, 9
// SPEC: mov byte ptr {{.*}}, 1
// SPEC: [[YOUNG]]:
// SPEC-HV64: mov qword ptr {{.*}}
// SPEC-HV32: mov dword ptr {{.*}}
// SPEC-BOXED: mov qword ptr {{.*}}
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _jit_put_by_id
