/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xhermes-internal-test-methods %s -- 512 > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xhermes-internal-test-methods %s -- 512 > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- 8 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- 512 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xhermes-internal-test-methods -Xdump-jitcode=3 %s -- 8 | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The integer typed-array store tier accepts FRACTIONAL values and
// truncates them, exactly as the interpreter's element setter does --
// it does not prove the value converts back unchanged and decline
// everything else.
//
// This is a cross-backend requirement, not a per-backend detail, which
// is why this test is architecture-neutral. A backend whose integer
// conversion admitted only exactly-representable integers would still
// STORE the right numbers (the declined stores reach the interpreter's
// own conversion and come out identical), so a value check alone cannot
// see the difference. What it would change is the DECLINE STREAM: a
// specialized site fed nothing but fractions would decline every single
// one of them, cross the recompile threshold over and over, and
// eventually demote -- while the same site on a backend that truncates
// inline is silent. The counters below are what that difference shows
// up in.
//
// The two counter RUN lines are an A/B pair sharing a byte-identical
// warm-up prefix and differing ONLY in how much fractional traffic runs
// after each site has specialized: 8 stores per site versus 512. Both
// pin the SAME counts. A defective conversion declining every
// fractional store would add 512 declines per site to the long run --
// eight further threshold crossings each -- and the stable-crossing
// demotion pass would then flip both sites, so the long run would fail
// on NumRecompileChecks and on NumByValDemotions both.
//
// Two INDEPENDENT store functions, one per kind, each with its own
// CodeBlock and therefore its own decline counter and its own ByVal
// site: a shared helper would specialize on whichever kind reached it
// first and poison the other, and its counter would mix the two.
function storeI32(ta, i, v) {
  ta[i] = v;
}
function storeU8(ta, i, v) {
  ta[i] = v;
}

// Every loop lives in a function whose per-iteration work is on locals
// and parameters only. A top-level loop would drive the global object's
// ById sites, whose own declines would land in the global body's
// counter and differ between the two runs.
function warmI32(ta, n) {
  for (var i = 0; i < n; ++i)
    storeI32(ta, 1, i);
}
function warmU8(ta, n) {
  for (var i = 0; i < n; ++i)
    storeU8(ta, 1, i);
}
// The fractional suffix. Alternating signs: truncation is toward zero,
// so -0.5 must store 0 and not -1, which is what a conversion that
// rounded or floored instead would get wrong.
function fracI32(ta, n) {
  for (var i = 0; i < n; ++i)
    storeI32(ta, 1, (i & 1) ? -(i + 0.5) : i + 0.5);
}
function fracU8(ta, n) {
  for (var i = 0; i < n; ++i)
    storeU8(ta, 1, (i & 1) ? -(i + 0.25) : i + 0.75);
}

var suffix = +hermescli.getScriptArgs()[0];

// Nonzero-offset views, so the address each store computes has every
// term nonzero.
var bufI32 = new ArrayBuffer(64);
var i32 = new Int32Array(bufI32, 8, 4);
var bufU8 = new ArrayBuffer(64);
var u8 = new Uint8Array(bufU8, 8, 4);

// 70 calls each: the 64th decline crosses the threshold and installs a
// version 2 with the typed-array tier, and the remaining calls run it.
warmI32(i32, 70);
warmU8(u8, 70);
print("warm", i32[1], u8[1]);

fracI32(i32, suffix);
fracU8(u8, suffix);
print("frac", i32[1], u8[1]);

// A handful of named fractional values through the same specialized
// sites, so the truncation is pinned on values chosen rather than on
// whatever the loop happened to end at. Each is a fresh top-level call.
storeI32(i32, 1, 7.9);
print("I32 7.9", i32[1]);
storeI32(i32, 1, -7.9);
print("I32 -7.9", i32[1]);
storeI32(i32, 1, -0.5);
print("I32 -0.5", i32[1], 1 / i32[1]);
storeU8(u8, 1, 3.9);
print("U8 3.9", u8[1]);
storeU8(u8, 1, -0.9);
print("U8 -0.9", u8[1]);
print("done");

// The stored values themselves are checked by the interpreter/JIT diff
// on the RUN lines above, not here.
// CHECK: done
// CHECK: JIT counters:
// Exactly one crossing per site, from the warm-up, in BOTH runs: the
// fractional suffix adds none.
// CHECK: NumRecompileChecks: 2{{$}}
// CHECK: NumRecompiles: 2{{$}}
// CHECK: NumByValDemotions: 0{{$}}

// Both sites really are specialized before the suffix runs, so the
// counts above are about a tier that exists.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeI32' (version 2)
// SPEC: // Inline typed array store
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeI32' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeU8' (version 2)
// SPEC: // Inline typed array store
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeU8' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
