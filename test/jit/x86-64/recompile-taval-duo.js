/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// REQUIRES: gc_hades
// UNSUPPORTED: handle_san

// One ByVal site sees both a dense array and an Int32Array,
// interleaved. The array stores always hit the JSArray tier (index 0
// of a hole-free array) and decline never, so they leave NO trace in
// the record at all; the Int32Array stores always miss that tier's
// kind guard and decline through the helper, which records a mono
// typed-array kind. Only the Int32Array declines drive the shared
// counter, so the loop must give them, alone, at least 64 declines to
// cross the threshold (64, pinned on the RUN line) -- 70 each,
// interleaved, does that with margin. Version 2 emits both tiers side
// by side: the typed-array tier from the recorded kind, the JSArray
// tier as the unconditional prior, chained so a kind miss in the first
// falls into the second.
function f(x, v) {
  x[0] = v;
}
var arr = [0];
var ta = new Int32Array(4);
for (var i = 0; i < 70; ++i) {
  f(arr, i);
  f(ta, i + 1000);
}
print(arr[0], ta[0]);

// OUT: 69 1069

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: // Inline typed array store
// SPEC: // Inline fast array store
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
