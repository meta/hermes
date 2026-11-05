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
// UNSUPPORTED: handle_san
// (Under HERMESVM_SANITIZE_HANDLES the runtime boxes every number, so the
// encoder declines the inline-number case with a bare jump and the encode
// sequence pinned below is not emitted; see putbyid-inline-emitted.js.)

// That the PutByVal inline fast array store is EMITTED, and what it consists
// of. putbyval-inline.js is the file that runs it against a collecting heap;
// this one only looks at the instructions, which is why it is small.
//
// The tier exists in every heap-value mode, and so does this file: what is
// the same in all three is pinned under SPEC, and what differs under
// SPEC-HV64, SPEC-HV32 or SPEC-BOXED, of which exactly one is active in a
// given build (test/lit.cfg's %hv-mode). Three things differ here, all of
// them consequences of the width of a heap value slot: the element address
// is scaled by four rather than eight under compressed pointers; the hole
// test compares a whole encoded SmallHermesValue there instead of reading a
// HermesValue's ETag; and the cell header packs its size into 24 bits rather
// than 32, so the kind has to be masked off the jumbo-cell gate's load. The
// value encoding the two boxed modes add is pinned by
// putbyid-inline-emitted.js, which shares this predicate.
//
// -Xjit=force is enough here, unlike putbyid-inline-emitted.js: this tier
// reads no property cache, so it does not need the function to have run
// interpreted first.

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
// Under boxed doubles the value is encoded into a SmallHermesValue before
// any guard runs, so that a value with no inline encoding is rejected
// without paying for the chain below; the dispatch itself is pinned by
// putbyid-inline-emitted.js, which shares the encoder. Its decline is what
// names the helper label in those two modes.
// SPEC-HV32: // Encode the value as a SmallHermesValue
// SPEC-HV32: shl {{.*}}, 0x1D
// SPEC-HV32: jnz [[SLOW:L[0-9]+]]
// SPEC-BOXED: // Encode the value as a SmallHermesValue
// SPEC-BOXED: shl {{.*}}, 0x3D
// SPEC-BOXED: jnz [[SLOW:L[0-9]+]]
//
// Then the shape guards, in the order putByValWithReceiver_RJS makes them.
// The target must be an object ...
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFF
// SPEC-HV64: jne [[SLOW:L[0-9]+]]
// SPEC-HV32: jne [[SLOW]]
// SPEC-BOXED: jne [[SLOW]]
// ... of cell kind JSArray exactly ...
// SPEC: cmp byte ptr {{.*}}, {{0x[0-9A-F]+}}
// SPEC: jne [[SLOW]]
// ... whose hidden class has indexedStorage set and hasIndexLikeProperties
// and frozen clear ...
// SPEC: movzx {{.*}}, word ptr {{.*}}
// SPEC: and {{.*}}, 0x884
// SPEC: cmp {{.*}}, 0x800
// SPEC: jne [[SLOW]]
// ... a key that converts to a uint32 and back unchanged ...
// SPEC: vcvttsd2si
// SPEC: vcvtsi2sd
// SPEC: vucomisd
// SPEC: jne [[SLOW]]
// SPEC: jp [[SLOW]]
// ... which is not 0xFFFFFFFF ...
// SPEC: cmp {{.*}}, 0xFFFFFFFF
// SPEC: je [[SLOW]]
// ... and lies in [beginIndex_, beginIndex_ + elemCount_) ...
// SPEC: sub {{.*}}, dword ptr {{.*}}
// SPEC: cmp {{.*}}, dword ptr {{.*}}
// SPEC: jae [[SLOW]]
// ... in a storage cell that is not a jumbo cell: its allocated size,
// biased by one so that the "size too large to record" encoding of 0 is
// rejected too, must be below RuntimeOffsets::kMaxInlineStorage. Under
// compressed pointers the header is four bytes wide and the kind shares it
// with the size, so the kind is masked off first.
// SPEC-HV32: and {{.*}}, 0xFFFFFF
// SPEC: sub {{.*}}, 1
// SPEC: cmp {{.*}}, 0x3EBDFF
// SPEC: ja [[SLOW]]
// The element address: the index scaled by the width of one slot. Only the
// scale is pinned; the displacement is where ArrayStorageSmall's elements
// begin, and a GCCell carries two debug-only fields that move it.
// SPEC-HV64: lea {{.*}}, [{{.*}}*8+{{[0-9]+}}]
// SPEC-HV32: lea {{.*}}, [{{.*}}*4+{{[0-9]+}}]
// SPEC-BOXED: lea {{.*}}, [{{.*}}*8+{{[0-9]+}}]
// ... at an address that does not currently hold a hole. Where a slot is
// eight bytes an inline value holds the HermesValue's bits unshifted, so
// this is HermesValue's own ETag test; where it is four they have been
// shifted down, and the encoded empty value is compared whole instead.
// SPEC-HV64: cmp {{.*}}, 0xFFFFFFFFFFFFFFF2
// SPEC-HV32: cmp {{.*}}, 0xFFF90000
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFF2
// SPEC: je [[SLOW]]
//
// Then the shared barrier predicate, whose own shape is pinned by
// putbyid-inline-emitted.js.
// SPEC: // Inline store with barrier predicate
// SPEC: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC-HV64: mov qword ptr {{.*}}
// SPEC-HV32: mov dword ptr {{.*}}
// SPEC-BOXED: mov qword ptr {{.*}}
// SPEC: mov byte ptr {{.*}}, 1
//
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _sh_ljs_put_by_val_loose_rjs
