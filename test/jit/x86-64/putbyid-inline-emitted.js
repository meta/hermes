/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// REQUIRES: heap_hv_64

// That the PutById inline tier is EMITTED, and what it consists of.
// putbyid-inline.js is the file that runs it against a collecting heap; this
// one only looks at the instructions, which is why it is small.
//
// It is restricted to the default heap-value mode because that is the only
// one the tier exists in: under HERMESVM_COMPRESSED_POINTERS a slot holds a
// 32-bit compressed value and under HERMESVM_BOXED_DOUBLES a store may have
// to box first, so both of those builds emit the runtime helper call alone
// (HERMES_JIT_INLINE_SAFE_STORE in JitEmitter.h).
//
// Threshold mode, not -Xjit=force: the tier is emitted only when the site's
// write cache already names a hidden class, which needs the function to have
// run interpreted first. See the header of putbyid-inline.js.

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
// The guard: the target must be an object, and its hidden class must be the
// one the write cache recorded, compared by lazy JIT id.
// SPEC: // Put to object specialization
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFF
// SPEC: movzx {{.*}}, word ptr {{.*}}
// SPEC: cmp {{.*}}
// SPEC: jne [[SLOW:L[0-9]+]]
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
// SPEC: mov qword ptr {{.*}}
// The card-dirty store, reached only for an old target holding a pointer to
// a young cell: the byte at segment start + (slot - segment start) >> 9.
// SPEC: shr {{.*}}, 9
// SPEC: mov byte ptr {{.*}}, 1
// SPEC: [[YOUNG]]:
// SPEC: mov qword ptr {{.*}}
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _jit_put_by_id
