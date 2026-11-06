/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-hc-id-limit=0 -Xjit-max-recompiles=3 -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=RUN2 %s
// REQUIRES: jit

// With the lazy hidden-class id space exhausted (-Xjit-hc-id-limit=0),
// initHCLazyIDMayAlloc() returns 0 for the write cache's class even
// though the cache is warm and non-null. That must not be reported as a
// cold ById site: such a site can never be specialized by a recompile,
// so counting it cold falsely satisfies considerRecompile's progress
// check and every threshold crossing burns budget recompiling a
// byte-identical body. f must therefore compile exactly once, with no
// cold ById sites and no version-2 body, even though the loop below
// crosses the threshold (4) many times over.

function f(o, v) {
  o.p = v;
}

var o = {p: 0};
for (var i = 0; i < 200; ++i)
  f(o, i);
print(o.p);

// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// "JIT cold ById sites" is only ever printed right after a successful
// compile's own success line (see JitCompiler.cpp), so this is the
// earliest point where the line could actually appear; anchoring the
// -NOT here, rather than before the success line where it can never
// fire, is what makes it actually assert the central invariant: f's
// cold count is zero. The same window bounds the version -NOT below;
// 'global' may recompile around the same time, but not between here
// and the final print, since its own ById site (in print(o.p)) runs
// only once and can never cross the decline threshold that triggers a
// recompile.
// CHECK-NOT: JIT cold ById sites
// CHECK-NOT: (version
// CHECK: 199

// RUN2: 199
