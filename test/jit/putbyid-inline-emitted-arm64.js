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
// Today arm64 only has the tier in the default heap-value mode --
// HERMES_JIT_INLINE_SAFE_STORE excludes compressed pointers and boxed
// doubles there until the SmallHermesValue encoder is ported -- so the tier
// is pinned under SPEC-HV64 (test/lit.cfg's %hv-mode), and the other modes
// pin only the helper call. Field displacements are wildcarded: they differ
// between builds.
//
// UNSUPPORTED under HERMESVM_SANITIZE_HANDLES (the `handle_san` lit feature)
// for the same reason its x86-64 counterpart is: the emitted shape differs
// there deliberately, and the behavioral tests are what cover such a build.

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
// SPEC-HV64: // Put to object specialization
//
// The guard: the target must be an object, and its hidden class must be the
// one the write cache recorded, compared by lazy JIT id. arm64 tests a tag by
// shifting it down and adding, so "is an object" is `asr`+`cmn` rather than
// x86-64's `sar`+`cmp`.
// SPEC-HV64: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC-HV64: cmn {{x[0-9]+}}, 1
// SPEC-HV64: b.ne [[SLOW:L[0-9]+]]
// SPEC-HV64: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// SPEC-HV64: ldrh {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64: cmp {{w[0-9]+}}, {{[0-9]+}}
// SPEC-HV64: b.ne [[SLOW]]
//
// An indirect slot: load the property storage, then add the element's byte
// offset within it. A direct slot is the same `add` without the load.
// SPEC-HV64: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV64: add {{x[0-9]+}}, {{x[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
//
// The barrier predicate. The young-generation exit stores with no barrier at
// all; every other exit either stores and dirties one card, or declines to
// the helper without storing. Unlike x86-64, which compares against memory,
// each of the three runtime words tested here is loaded into a register
// first.
// SPEC-HV64: // Inline store with barrier predicate
// SPEC-HV64: and [[SEG:x[0-9]+]], {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC-HV64: ldr {{x[0-9]+}}, [x19, {{[0-9]+}}]
// SPEC-HV64: cmp [[SEG]], {{x[0-9]+}}
// SPEC-HV64: b.eq [[YOUNG:L[0-9]+]]
// SPEC-HV64: ldrb {{w[0-9]+}}, [x19, {{[0-9]+}}]
// SPEC-HV64: cbnz {{w[0-9]+}}, [[SLOW]]
// SPEC-HV64: ldr {{x[0-9]+}}, [x19, {{[0-9]+}}]
// SPEC-HV64: cmp {{x[0-9]+}}, 1
// SPEC-HV64: b.ne [[SLOW]]
// SPEC-HV64: ldrh {{w[0-9]+}}, {{\[}}[[SEG]], 4]
// SPEC-HV64: cmp {{w[0-9]+}}, 1
// SPEC-HV64: b.ne [[SLOW]]
// The store itself: one slot wide, which is eight bytes in this mode.
// SPEC-HV64: str [[VAL:x[0-9]+]], {{\[}}{{x[0-9]+}}]
//
// The card-dirty store, reached only for an old target holding a pointer to
// a young cell: the byte at segment start + (slot - segment start) >> 9.
// x16 is the backend's non-allocated scratch, which the card sequence needs
// because both temporaries are already live at that point.
// SPEC-HV64: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC-HV64: cmn {{x[0-9]+}}, 3
// SPEC-HV64: b.lo [[DONE:L[0-9]+]]
// SPEC-HV64: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// SPEC-HV64: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC-HV64: ldr x16, [x19, {{[0-9]+}}]
// SPEC-HV64: cmp {{x[0-9]+}}, x16
// SPEC-HV64: b.ne [[DONE]]
// SPEC-HV64: sub {{x[0-9]+}}, {{x[0-9]+}}, [[SEG]]
// SPEC-HV64: lsr {{x[0-9]+}}, {{x[0-9]+}}, 9
// SPEC-HV64: mov w16, 1
// SPEC-HV64: strb w16, {{\[}}[[SEG]], {{x[0-9]+}}]
// SPEC-HV64: b [[DONE]]
// SPEC-HV64: [[YOUNG]]:
// SPEC-HV64: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC-HV64: [[DONE]]:
// And the unchanged helper call the guards fall back to.
// SPEC-HV64: [[SLOW]]:
// SPEC: call _jit_put_by_id
