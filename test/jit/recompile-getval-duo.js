/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --match-full-lines --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// One GetByVal site sees both a dense array and an Int32Array,
// interleaved. The array reads always hit the JSArray tier (a non-hole
// element of a dense array) and decline never, so they leave NO trace in
// the record at all; the Int32Array reads always miss that tier's kind
// guard and decline through the helper, which records a mono typed-array
// kind. Only the Int32Array declines drive the shared counter, so the loop
// must give them, alone, at least 64 declines to cross the threshold (64,
// pinned on the RUN line) -- 70 each, interleaved, does that with margin.
//
// Version 2 emits both tiers side by side: the typed-array tier from the
// recorded kind, the JSArray tier as the unconditional prior, chained so a
// kind miss in the first falls into the second. That chaining is what this
// file exists for, and it is a distinct emitter path from poisoning: both
// targets must keep reading correctly AFTERWARDS, through a single site
// that now begins with a kind guard for only one of them.
//
// The key is f's `i` parameter, never a literal: a literal-keyed twin
// would lower to GetByIndex, which has no tier. The `// getByVal r` dump
// comment is the pin that this is a GetByVal site.
function f(x, i) {
  return x[i];
}
var arr = [5, 6, 7, 8];
var ta = new Int32Array(4);
ta[0] = 100;
ta[1] = 200;
ta[2] = 300;
ta[3] = 400;
var sum = 0;
for (var i = 0; i < 70; ++i) {
  sum += f(arr, i & 3);
  sum += f(ta, i & 3);
}
print("sum", sum);
// Fresh top-level calls, each genuinely running in version 2: the array
// reaches the JSArray tier only through the typed-array tier's kind miss,
// and the typed array reaches its own tier directly.
print("arr", f(arr, 0), f(arr, 3), f(arr, 4));
print("ta", f(ta, 0), f(ta, 3), f(ta, 4));

// OUT: sum 17753
// OUT-NEXT: arr 5 8 undefined
// OUT-NEXT: ta 100 400 undefined

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: // getByVal r
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// Both tiers, in chain order: the recorded typed-array kind first, its
// kind miss falling into the retained JSArray tier.
// SPEC: // Inline typed array load (kind 39)
// SPEC: // Inline fast array load
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
