/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// REQUIRES: heap_hv_64

// That the PutByVal inline fast array store is EMITTED, and what it consists
// of. putbyval-inline.js is the file that runs it against a collecting heap;
// this one only looks at the instructions, which is why it is small.
//
// It is restricted to the default heap-value mode because that is the only
// one the inline store exists in: under HERMESVM_COMPRESSED_POINTERS an
// element holds a 32-bit compressed value and under HERMESVM_BOXED_DOUBLES a
// store may have to box first, so both of those builds emit the runtime
// helper call alone (HERMES_JIT_INLINE_SAFE_STORE in JitEmitter.h).
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
// The shape guards, in the order putByValWithReceiver_RJS makes them.
// SPEC: // Inline fast array store
// The target must be an object ...
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFF
// SPEC: jne [[SLOW:L[0-9]+]]
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
// rejected too, must be below RuntimeOffsets::kMaxInlineStorage ...
// SPEC: sub {{.*}}, 1
// SPEC: cmp {{.*}}, 0x3EBDFF
// SPEC: ja [[SLOW]]
// ... at an address that does not currently hold a hole.
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFF2
// SPEC: je [[SLOW]]
//
// Then the shared barrier predicate, whose own shape is pinned by
// putbyid-inline-emitted.js.
// SPEC: // Inline store with barrier predicate
// SPEC: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC: mov qword ptr {{.*}}
// SPEC: mov byte ptr {{.*}}, 1
//
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _sh_ljs_put_by_val_loose_rjs
