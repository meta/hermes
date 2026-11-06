/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=SPEC %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=0 -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=OFF %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Under -Xjit=force, f compiles before it ever runs: its write cache is
// cold and the PutById inline tier is not emitted (the known force-mode
// gap). The helper calls warm the cache and count declines; past the
// threshold (64, pinned on the RUN line) the function is recompiled
// and version 2 carries the tier. With -Xjit-max-recompiles=0 no
// version 2 may appear.

function f(o, v) {
  o.p = v;
}

var o = {p: 0};
for (var i = 0; i < 100; ++i)
  f(o, i);
print(o.p);

// OUT: 99

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// The version-1 dump for 'f' ends at its own (un-suffixed) success line;
// bounding the -NOT to this window matters because 'global' recompiles
// (and re-gains the tier for its own warmed sites) chronologically
// between here and 'f's version-2 compile, and an unbounded -NOT would
// wrongly see global's tier comments.
// SPEC-NOT: // Put to object specialization
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: // Put to object specialization
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: 99

// OFF: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// OFF-NOT: (version
// OFF: 99
