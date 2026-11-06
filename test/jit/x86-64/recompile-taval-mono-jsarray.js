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

// arr's ByVal site sees nothing but JSArray traffic. The JSArray tier
// is emitted at every version regardless (a static prior), so what is
// pinned here is that nothing about a pure-JSArray site makes the
// recompile emit a typed-array tier or count a specialization.
//
// The tier no longer instruments itself, so a store that HITS is
// silent: the site's only evidence is a decline. Exactly one is
// arranged -- the store at index 5 of a length-1 array is past the
// dense region, which the tier's range guard rejects, so it goes
// through the helper and records jsArraySeen. Without it the site
// would be observed zero times and the "JIT ByVal sites" line would
// not be printed at all (the dump suppresses a zero-observed record),
// leaving nothing to pin.
//
// o.p's ById site is cold under force and every call declines it; past
// the threshold (64, pinned on the RUN line, crossed well inside the
// 100 calls) considerRecompile fires from the ById route. Version 2
// keeps the JSArray tier, emits no typed-array tier at all -- there
// was never any typed-array traffic -- and reports the site as
// observed but unspecialized.
function f(arr, o, i, v) {
  arr[i] = v;
  o.p = v;
}
var arr = [0];
var o = {p: 0};
// The one decline: index 5 is past the dense region of a length-1
// array, so this store declines into the recording helper.
f(arr, o, 5, -1);
for (var i = 0; i < 100; ++i)
  f(arr, o, 0, i);
print(arr[0], arr[5], o.p);

// OUT: 99 -1 99

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC-NOT: // Inline typed array store
// SPEC: // Inline fast array store
// SPEC-NOT: // Inline typed array store
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 0 specialized
