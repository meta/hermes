/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xdump-jitcode=2 %s | %FileCheck --check-prefix=FORCE %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xdump-jitcode=2 %s | %FileCheck --check-prefix=WARM %s
// REQUIRES: jit

// Under -Xjit=force the function compiles before it ever runs, so the
// write cache at `o.p = v` is cold and the site is reported. Under a
// threshold, the interpreter warms the cache first and no cold site is
// reported for f.

function f(o, v) {
  o.p = v;
}

var o = {p: 0};
for (var i = 0; i < 20; ++i)
  f(o, i);
print(o.p);

// FORCE: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// FORCE: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// FORCE: JIT cold ById sites: {{[0-9]+}}
// FORCE: 19

// WARM: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// WARM-NOT: JIT cold ById sites
// WARM: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// WARM: 19
