/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=SPEC,SPEC-%hv-mode %s
// REQUIRES: jit

// That the GetByVal inline fast array load tier is EMITTED, and what it
// consists of. getval-guards.js is the file that runs it (and its decline
// paths) against real prototype/hole/Arguments traffic; this one only
// looks at the instructions, which is why it is small.
//
// UNLIKE putbyval-inline-emitted.js, this file carries no "unsupported
// under handle_san" annotation. That annotation exists there because
// HERMESVM_SANITIZE_HANDLES changes emitted CODE on the PUT side: it makes
// canInlineCompressibleOrNumberHV64() refuse every number, so
// emit_shv_encode_or_slow() (JitEmitter-internal.cpp) takes its
// HERMESVM_SANITIZE_HANDLES branch and declines the inline-number case
// with a bare jump instead of emitting the encode sequence putbyval-
// inline-emitted.js pins. GetByVal has no encode step at all -- the tier
// only ever DECODES an already-stored SmallHermesValue, through
// Emit_sh_shv_decode (Emitter::emitGetByValFastArrayTier() ->
// emit_sh_shv_decode()), and neither that class nor anything else this
// tier emits references HERMESVM_SANITIZE_HANDLES. So this file's output
// is identical whether or not Handle-San is on, and gating it would just
// hide the tier from that configuration's suite for no reason.
//
// The tier exists in every heap-value mode, and so does this file: what is
// the same in all three is pinned under SPEC, and what differs under
// SPEC-HV64, SPEC-HV32 or SPEC-BOXED is exactly what differs for the put
// tier's own guards (element scale, and how the header packs kind and
// size) plus one difference of its own: the empty-slot check. The put
// tier's emit_shv_load_is_empty() can clobber its temp register once it
// has answered the predicate, because the put tier never needs the loaded
// bits again; the load tier needs to DECODE them afterward, so under HV64
// and BOXED (an 8-byte slot) it runs the ETag shift through a SEPARATE
// register from the one holding the value, while HV32's whole-value
// compare (a `cmp`, which never writes its operand) needs no such split.
//
// -Xjit=force is enough here, unlike getbyid's own inline-cache tests:
// this tier reads no property cache, so it does not need the function to
// have run interpreted first -- exactly the reasoning
// putbyval-inline-emitted.js gives for its own PutByVal tier.

function load(arr, i) {
  return arr[i];
}

var a = [0, 1, 2, 3];
for (var i = 0; i < 20; ++i)
  load(a, 1);
print(load(a, 1));
// CHECK: 1

// Two specialized sites, one per shape of the typed-array load tier: an
// integer kind, whose element has to be widened through a GP register and
// an integer-to-double conversion, and a float kind, whose element is
// already a double and instead needs the NaN canonicalization. Each is its
// own function, so each site is monomorphic and earns its own tier; each
// is warmed with 100 separate top-level calls, past the 64-decline
// threshold pinned on the RUN lines, so that version 2 exists to be
// inspected. Both views start at a NONZERO byte offset.
function loadTA(ta, i) {
  return ta[i];
}
function loadTAF(ta, i) {
  return ta[i];
}
var tbuf = new ArrayBuffer(64);
var ti8 = new Int8Array(tbuf, 8, 4);
ti8[1] = -3;
var tf64 = new Float64Array(tbuf, 24, 4);
tf64[1] = 2.5;
for (var i = 0; i < 100; ++i) {
  loadTA(ti8, 0);
  loadTAF(tf64, 0);
}
// The values these specialized sites produce, in bounds and one past the
// end, are pinned at the very end of the SPEC sequence below -- after the
// instructions, because that is where they appear in the dump.
print(loadTA(ti8, 1), loadTA(ti8, 4), loadTAF(tf64, 1), loadTAF(tf64, 4));

// SPEC-LABEL:load:
// Anti-GetByIndex, anti-vacuity pin: a literal-keyed twin of this function
// would lower to GetByIndex instead (ISel.cpp's uint8-literal special
// case), which this tier never touches, and the comment below would then
// read "// getByIdx" -- the dynamic key above is what keeps this GetByVal.
// SPEC: // getByVal r
// SPEC: // Inline fast array load
// The source must be an object ...
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFF
// This tier has no encode step (unlike the put tier's, which under HV32/
// BOXED defines SLOW earlier still, in its value-encode section) -- this
// is the FIRST decline point in every mode, so all three prefixes define
// the capture here rather than only HV64 defining it and the other two
// reusing it.
// SPEC-HV64: jne [[SLOW:L[0-9]+]]
// SPEC-HV32: jne [[SLOW:L[0-9]+]]
// SPEC-BOXED: jne [[SLOW:L[0-9]+]]
// ... of cell kind JSArray exactly. Unlike putbyval-inline-emitted.js's
// pin at this same point, no fastIndexProperties/frozen flags check
// follows: the read side needs none (see
// Emitter::emitGetByValFastArrayTier()'s doc comment for why the empty
// check below subsumes it).
// SPEC: cmp byte ptr {{.*}}, {{0x[0-9A-F]+}}
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
// The element address: the index scaled by the width of one slot. Only
// the scale is pinned; the displacement is where ArrayStorageSmall's
// elements begin, and a GCCell carries two debug-only fields that move
// it.
// SPEC-HV64: lea {{.*}}, [{{.*}}*8+{{[0-9]+}}]
// SPEC-HV32: lea {{.*}}, [{{.*}}*4+{{[0-9]+}}]
// SPEC-BOXED: lea {{.*}}, [{{.*}}*8+{{[0-9]+}}]
// ... at an address that does not currently hold a hole. Where a slot is
// eight bytes the loaded bits are checked through a SEPARATE register
// (the load tier's own difference from the put tier's in-place check,
// documented above); where it is four the whole encoded empty value is
// compared directly, which needs no such split.
// SPEC-HV64: mov {{.*}}, qword ptr {{.*}}
// SPEC-HV64: sar {{.*}}, 0x2F
// SPEC-HV64: cmp {{.*}}, 0xFFFFFFFFFFFFFFF2
// SPEC-BOXED: mov {{.*}}, qword ptr {{.*}}
// SPEC-BOXED: sar {{.*}}, 0x2F
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFF2
// SPEC-HV32: mov {{.*}}, dword ptr {{.*}}
// SPEC-HV32: cmp {{.*}}, 0xFFF90000
// SPEC: je [[SLOW]]
//
// And the helper call every decline falls back to: the per-site RECORDING
// helper, reached INDIRECTLY through the site's own mutable function
// pointer slot rather than by a direct call to a fixed address. The slot
// is what the runtime flips, after emission, from the recording helper to
// the plain one when a site is demoted, so the call sequence has to load
// the address at run time: `movabs r11, <&site.helper>` followed by a call
// through it. A direct `call _sh_ljs_get_by_val_rjs` here would mean the
// site records nothing and can never be demoted either.
//
// The two arguments past the three the plain helper takes -- the version
// record and the site id -- are set up unconditionally: under SysV a
// callee that takes fewer arguments simply ignores the extra registers,
// which is what lets one sequence serve both the recording callee and the
// demoted one.
// SPEC: [[SLOW]]:
// SPEC: // call _jit_get_by_val [indirect]
// SPEC: mov r11, {{.*}}
// SPEC: call qword ptr [r11]
//
// ---- the typed-array tier, integer kind ----
//
// Int8Array is CellKind 34 (0x22). No CHECK-LABEL anchors these two
// sections: a recompiled function's name line appears once per version, so
// a label on it would not be unique; the version-2 compile banner is.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadTA' (version 2)
// SPEC: // Inline typed array load (kind 34)
// The exact-kind guard, whose miss chains into the JSArray tier rather
// than into the helper.
// SPEC: cmp byte ptr {{.*}}, 0x22
// SPEC: jne [[KINDMISS:L[0-9]+]]
// The shared key conversion, exactly the fast array tier's, minus the
// 0xFFFFFFFF exclusion (the bounds check below rejects it).
// SPEC: vcvttsd2si
// SPEC: vucomisd
// SPEC: jne [[TASLOW:L[0-9]+]]
// SPEC: jp [[TASLOW]]
// Bounds, and then attachedness via the buffer's data_ pointer. NEITHER
// declines: both branch to this tier's own inline `undefined`, because
// that is what an out-of-bounds or detached typed-array read evaluates to.
// SPEC: cmp {{.*}}, dword ptr {{.*}}
// SPEC: jae [[UNDEF:L[0-9]+]]
// SPEC: test {{.*}}
// SPEC: jz [[UNDEF]]
// The element load and its widening: a sign-extending byte load into a GP
// register, then an integer-to-double conversion, then the encode -- which
// under NaN-boxing is a bare move of the double's bits, since a converted
// integer is always a finite double and needs no canonicalization.
// SPEC: movsx {{.*}}, byte ptr {{.*}}
// SPEC: vcvtsi2sd
// SPEC: vmovq
// The inline `undefined`, which is the whole of the bounds-fail path: no
// call, and therefore no recording either.
// SPEC: [[UNDEF]]:
// SPEC: mov {{.*}}, 0xFFFA000000000000
// The kind miss lands in the JSArray tier, which is still emitted here.
// SPEC: [[KINDMISS]]:
// SPEC: // Inline fast array load
//
// ---- the typed-array tier, float kind ----
//
// Float64Array is CellKind 42 (0x2A). Its element is already a double, so
// there is no conversion -- but there IS the NaN canonicalization, which
// the integer kinds above have no need of and do not emit.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'loadTAF' (version 2)
// SPEC: // Inline typed array load (kind 42)
// SPEC: cmp byte ptr {{.*}}, 0x2A
// SPEC: jne [[FKINDMISS:L[0-9]+]]
// SPEC: jae [[FUNDEF:L[0-9]+]]
// SPEC: jz [[FUNDEF]]
// The element load, then the canonicalization: a SELF-compare, whose only
// unordered outcome is a NaN of some payload, and on that outcome the
// whole result is replaced by the canonical quiet NaN
// (0x7FF8000000000000). Without it the element's raw bits would be encoded
// verbatim, and a NaN payload that aliases this engine's tag space would
// become a forged non-number.
// SPEC: vmovsd {{.*}}, qword ptr {{.*}}
// SPEC: vucomisd [[FV:xmm[0-9]+]], [[FV]]
// SPEC: vmovq
// SPEC: jnp [[FDONE:L[0-9]+]]
// SPEC: mov {{.*}}, 0x7FF8000000000000
// SPEC: [[FUNDEF]]:
// SPEC: mov {{.*}}, 0xFFFA000000000000
// SPEC: [[FKINDMISS]]:
// SPEC: // Inline fast array load
//
// And the values both specialized sites produce, in bounds and one past
// the end -- the pin that the instructions above are not merely present
// but correct.
// SPEC: -3 undefined 2.5 undefined
