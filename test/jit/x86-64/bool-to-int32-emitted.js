/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -fno-inline -O0 %s > %t.int0
// RUN: %hermes -fno-inline -O0 -Xjit=force -Xjit-crash-on-error %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -fno-inline -Xjit=force -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// RUN: %hermes -fno-inline %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// ToInt32/ToUint32 of a statically known bool is extracted inline instead of
// going through _sh_ljs_to_int32_rjs. A bool is not a double, so the generic
// double round trip can never accept one: without the bool path every site
// here would take the helper.
//
// The comparison has to be in the same basic block as the conversion. A
// loop-invariant comparison is hoisted out, and because bool-ness is a
// per-basic-block local FR type the conversion then no longer sees a known
// bool -- so `boolHoisted` below deliberately does NOT get the fast path and
// pins that limitation.

// Both conversions on a nonconstant comparison, both outcomes exercised.
function eqInt32(a, b) {
  return (a === b) | 0;
}
function neInt32(a, b) {
  return (a !== b) | 0;
}
function ltInt32(a, b) {
  return (a < b) | 0;
}
function eqUint32(a, b) {
  return (a === b) >>> 0;
}

// The boolean is used again after being converted, so the conversion must not
// destroy it.
function reuseBool(a, b) {
  var t = a === b;
  var n = t | 0;
  return n + (t ? 10 : 20);
}

// Register pressure: several values stay live across the conversion, so the
// emitter's handling of live temps is exercised rather than assumed.
function pressure(a, b, c, d, e) {
  var p = c * 2 + 1;
  var q = d * 3 + 1;
  var r = e * 4 + 1;
  var n = (a === b) | 0;
  return n + p + q + r + (a < b ? 1 : 0);
}

// Loop-invariant comparison: hoisted out of the loop, so the conversion no
// longer sees a statically known bool and MUST still use the helper. The
// SPEC-check below asserts the helper is present, so this genuinely pins the
// same-basic-block limitation rather than merely passing either way.
function boolHoisted(a, b, n) {
  var s = 0;
  for (var i = 0; i < n; ++i) s += (a === b) | 0;
  return s;
}

print(eqInt32(3, 3), eqInt32(3, 4));
// CHECK: 1 0
print(neInt32(3, 3), neInt32(3, 4));
// CHECK-NEXT: 0 1
print(ltInt32(2, 3), ltInt32(3, 2));
// CHECK-NEXT: 1 0
print(eqUint32(3, 3), eqUint32(3, 4));
// CHECK-NEXT: 1 0
print(reuseBool(3, 3), reuseBool(3, 4));
// CHECK-NEXT: 11 20
print(pressure(3, 3, 1, 1, 1), pressure(3, 4, 1, 1, 1));
// CHECK-NEXT: 13 13
print(boolHoisted(3, 3, 5), boolHoisted(3, 4, 5));
// CHECK-NEXT: 5 0

// The bool conversion is inline: the extraction shifts the payload bit down
// and masks it, with no call to the conversion helper.
// Both helpers are checked absent in both functions: the generic path picks
// _sh_ljs_to_int32_rjs or _sh_ljs_to_uint32_rjs depending on isSigned, and
// the negative checks have to cover the deferred slow-path blocks too, which
// is why each runs to the end of its function's dump.
// SPEC-LABEL:{{.*}}'eqInt32'
// SPEC: ToInt32
// SPEC: shr
// SPEC-NEXT: and
// SPEC-NOT: _sh_ljs_to_int32_rjs
// SPEC-NOT: _sh_ljs_to_uint32_rjs
// SPEC: JIT successfully compiled{{.*}}'eqInt32'

// SPEC-LABEL:{{.*}}'eqUint32'
// SPEC: ToUint32
// SPEC: shr
// SPEC-NEXT: and
// SPEC-NOT: _sh_ljs_to_int32_rjs
// SPEC-NOT: _sh_ljs_to_uint32_rjs
// SPEC: JIT successfully compiled{{.*}}'eqUint32'

// reuseBool reads the boolean again after converting it: if the fast path
// freed the input's temp without syncing it, `t ? 10 : 20` would take the
// wrong branch and reuseBool(3,3) would return 21 instead of 11.
// SPEC-LABEL:{{.*}}'reuseBool'
// SPEC-NOT: _sh_ljs_to_int32_rjs
// SPEC: JIT successfully compiled{{.*}}'reuseBool'

// The hoisted case is the negative control for the limitation: its
// conversion does NOT get the fast path, so the helper must appear.
// SPEC-LABEL:{{.*}}'boolHoisted'
// SPEC: _sh_ljs_to_int32_rjs
// SPEC: JIT successfully compiled{{.*}}'boolHoisted'
