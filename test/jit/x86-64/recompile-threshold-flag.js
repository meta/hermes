/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=8 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=EARLY %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=LATE %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// -Xjit-recompile-threshold sets how many declines of one compiled body
// are needed before a recompile is considered. Both runs below are the
// same program: under -Xjit=force, f compiles before it ever runs, its
// write cache is cold, the PutById inline tier is not emitted, and every
// one of the 20 calls declines into the helper. At =8 the eighth decline
// crosses and f gets a version 2 (its cache warmed on call one, so the
// progress check passes); at the default 64, twenty declines never
// cross and f stays on version 1.
//
// The version checks name 'f' explicitly: 'global' has ById sites of its
// own (the var accesses in the loop) and may well recompile in either
// run, which says nothing about f.

function f(o, v) {
  o.p = v;
}

var o = {p: 0};
for (var i = 0; i < 20; ++i)
  f(o, i);
print(o.p);

// EARLY: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// EARLY: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// EARLY: 19

// The -NOT is bounded to the window between f's first successful compile
// and the program's only output: that is the whole interval in which a
// version-2 banner for f could be printed at all.
// LATE: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// LATE-NOT: 'f' (version
// LATE: 19
