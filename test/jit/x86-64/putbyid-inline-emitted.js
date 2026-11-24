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
// encoded -- the dispatch pinned below, whose number arm sends a double that
// does not fit inline to the box arm.
//
// THE ENCODE FOLLOWS THE BARRIER PREDICATE'S DECLINING CHECKS, and the
// order below pins that: once a double that needs a box is boxed inline
// instead of declined, no encode outcome is a cheap early reject any more,
// and the box allocation must follow every check that can decline, so
// that a box is never allocated and abandoned (spec
// 2026-10-29-jit-inline-boxed-double-store-design.md).
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
// The guard comes first in every mode: the target must be an object, and
// its hidden class must be the one the write cache recorded, compared by
// lazy JIT id. It is the tier's first exit, so it is where the helper label
// gets its name.
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFF
// SPEC-HV64: jne [[SLOW:L[0-9]+]]
// SPEC-HV32: jne [[SLOW:L[0-9]+]]
// SPEC-BOXED: jne [[SLOW:L[0-9]+]]
// SPEC: movzx {{.*}}, word ptr {{.*}}
// SPEC: cmp {{.*}}
// SPEC: jne [[SLOW]]
//
// The barrier predicate. Every exit before the store goes to the helper
// without storing, or, for a young target, to the store.
// SPEC: // Inline store with barrier predicate
// SPEC: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC: je [[YOUNG:L[0-9]+]]
// SPEC: cmp byte ptr {{.*}}, 0
// SPEC: jne [[SLOW]]
// SPEC: cmp qword ptr {{.*}}, 1
// SPEC: jne [[SLOW]]
// SPEC: cmp word ptr {{.*}}, 1
// SPEC: jne [[SLOW]]
//
// Under boxed doubles the young exit lands on the join, after the last
// declining check, where the value is encoded into a SmallHermesValue,
// mirroring HermesValue32::encodeHermesValue(): the ETag selects between
// the inline ("compressed HV64") case, the three pointer tags and Symbol. A
// double whose low bits are not zero goes to the box arm; only a value with
// no SmallHermesValue encoding (RawHV32) still declines. Nothing at all is
// emitted for this in HV64, not even the comment.
// SPEC-HV32: {{^}}[[YOUNG]]:
// SPEC-HV32-NEXT: // Encode the value as a SmallHermesValue
// SPEC-HV32: sar {{.*}}, 0x2F
// SPEC-HV32: cmp {{.*}}, 0xFFFFFFFFFFFFFFF6
// SPEC-HV32: ja [[NOTNUM:L[0-9]+]]
// The compression test: with a 29-bit payload, the low 35 bits must be zero.
// SPEC-HV32: shl {{.*}}, 0x1D
// SPEC-HV32-NEXT: jnz [[BOX:L[0-9]+]]{{$}}
// SPEC-HV32: shr {{.*}}, 0x20
// SPEC-HV32: {{^}}[[NOTNUM]]:
// SPEC-HV32: cmp {{.*}}, 0xFFFFFFFFFFFFFFFA
// SPEC-HV32: jb [[SYM:L[0-9]+]]
// A pointer: the HermesValue tag biased into the HV32 one, or-ed into the
// compressed pointer.
// SPEC-HV32: sar {{.*}}, 1
// SPEC-HV32: add {{.*}}, 4
// SPEC-HV32: sub {{.*}}, r15
// SPEC-HV32: or {{.*}}
// SPEC-HV32: {{^}}[[SYM]]:
// SPEC-HV32: cmp {{.*}}, 0xFFFFFFFFFFFFFFF7
// SPEC-HV32: jne [[SLOW]]
// SPEC-HV32: shl {{.*}}, 3
// SPEC-HV32: or {{.*}}, 5
//
// The same dispatch in the boxed build without compressed pointers: a slot
// is a full eight bytes there, so the payload is 61 bits, the compression
// test is on the low three bits alone, the inline case needs no shift at
// all, and a pointer needs no compression either.
// SPEC-BOXED: {{^}}[[YOUNG]]:
// SPEC-BOXED-NEXT: // Encode the value as a SmallHermesValue
// SPEC-BOXED: sar {{.*}}, 0x2F
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFF6
// SPEC-BOXED: ja [[NOTNUM:L[0-9]+]]
// SPEC-BOXED: shl {{.*}}, 0x3D
// SPEC-BOXED-NEXT: jnz [[BOX:L[0-9]+]]{{$}}
// SPEC-BOXED: {{^}}[[NOTNUM]]:
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFFA
// SPEC-BOXED: jb [[SYM:L[0-9]+]]
// SPEC-BOXED: sar {{.*}}, 1
// SPEC-BOXED: add {{.*}}, 4
// SPEC-BOXED: or {{.*}}
// SPEC-BOXED: {{^}}[[SYM]]:
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFF7
// SPEC-BOXED: jne [[SLOW]]
// SPEC-BOXED: shl {{.*}}, 3
// SPEC-BOXED: or {{.*}}, 5
//
// The store itself: one slot wide, which is four bytes only under
// compressed pointers. Under boxed doubles a young target, which reached it
// through the join, is then done: no barrier for any value.
// SPEC-HV64: mov qword ptr {{.*}}
// SPEC-HV32: mov dword ptr {{.*}}
// SPEC-HV32-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-HV32-NEXT: je [[DONE:L[0-9]+]]{{$}}
// SPEC-BOXED: mov qword ptr {{.*}}
// SPEC-BOXED-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-BOXED-NEXT: je [[DONE:L[0-9]+]]{{$}}
//
// The card decision for an inline value, made on the original HermesValue
// in every mode, which is why its tag compare is the same everywhere: only
// an old target holding a pointer to a young cell needs a card.
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFD
//
// Under boxed doubles the inline path then jumps to the card block it
// shares with the box arm, which follows at once (no counting block without
// -Xjit-emit-counters). The box arm: bump-allocate (a full young generation
// goes to the helper), write the double, tag the pointer, store it into the
// same slot, and recompute the segment start (t1 was an allocation
// temporary): a young target is done, an old one always gets its card.
// SPEC-HV32: jmp [[DIRTY:L[0-9]+]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[BOX]]:
// SPEC-HV32-NEXT: // Box the double in the young generation
// SPEC-HV32-NEXT: mov [[CELL:r[0-9a-z]+]], qword ptr [r15+{{[0-9]+}}]
// SPEC-HV32: ja [[SLOW]]{{$}}
// SPEC-HV32: mov qword ptr {{\[}}[[CELL]]+{{[0-9]+}}], {{r[0-9a-z]+}}
// SPEC-HV32-NEXT: sub [[CELL]], r15
// SPEC-HV32-NEXT: or [[CELL]], 4
// SPEC-HV32-NEXT: mov dword ptr {{.*}}, {{.*}}
// SPEC-HV32: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC-HV32-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-HV32-NEXT: je [[DONE]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[DIRTY]]:
// SPEC-BOXED: jmp [[DIRTY:L[0-9]+]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[BOX]]:
// SPEC-BOXED-NEXT: // Box the double in the young generation
// SPEC-BOXED-NEXT: mov [[CELL:r[0-9a-z]+]], qword ptr [r15+{{[0-9]+}}]
// SPEC-BOXED: ja [[SLOW]]{{$}}
// SPEC-BOXED: mov qword ptr {{\[}}[[CELL]]+{{[0-9]+}}], {{r[0-9a-z]+}}
// SPEC-BOXED-NEXT: or [[CELL]], 4
// SPEC-BOXED-NEXT: mov qword ptr {{.*}}, [[CELL]]
// SPEC-BOXED: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC-BOXED-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-BOXED-NEXT: je [[DONE]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[DIRTY]]:
//
// The card-dirty store: the byte at segment start + (slot - segment start)
// >> 9.
// SPEC: shr {{.*}}, 9
// SPEC: mov byte ptr {{.*}}, 1
// In HV64 the young-target store follows the card block.
// SPEC-HV64: [[YOUNG]]:
// SPEC-HV64: mov qword ptr {{.*}}
// SPEC-HV32-NEXT: {{^}}[[DONE]]:
// SPEC-BOXED-NEXT: {{^}}[[DONE]]:
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _jit_put_by_id
