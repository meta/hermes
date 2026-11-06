/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -gc-sanitize-handles=1 -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit

// A pointer-valued result on the crossing call whose recompile
// allocates. Nothing before `get`'s version-2 compile assigns a lazy ID
// (main's own `.n` reads come after it), so that compile creates
// usedHCs: an allocation, which under MallocGC + handle sanitization
// moves every live object, including the result the recording helper
// holds and the candidate's recorded classes.
//
// -gc-sanitize-handles=1: a HERMESVM_SANITIZE_HANDLES build moves the
// heap at only 1% of allocations by default; the rate must be 1 for the
// move to happen at this allocation. Other builds ignore the flag.
//
// The threshold is 16, not lower: the hermes shell runs the JSI
// extensions bytecode at startup, and at a threshold of 4 its functions
// recompile and assign the first four lazy IDs (creating usedHCs) before
// this script runs; `get`'s four entries then fit in usedHCs' initial
// capacity of 8 and its compile allocates nothing. Every call misses the
// live cache (the four shapes rotate), so the 16th call is the crossing;
// it returns arr[15 & 3], i.e. {n: 4}.
//
// The DUMP check pins the precondition: the chain's first compare is
// against lazy ID 1, so this compile made the process's first lazy-ID
// assignment and created usedHCs. If anything earlier assigns an ID,
// the compare moves (to 5 at a threshold of 4) and the check fails.
function main() {
  function get(o) {
    return o.v;
  }
  function drive(f, arr, out, n) {
    for (var i = 0; i < n; ++i)
      out[i] = f(arr[i & 3]);
  }
  var objs = [
    {v: {n: 1}},
    {a: 0, v: {n: 2}},
    {b: 0, v: {n: 3}},
    {c: 0, v: {n: 4}},
  ];
  var o = [];
  drive(get, objs, o, 64);
  print(o[15].n, o[63].n);
}
main();

// CHECK: 4 4
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP-NOT: cmp
// DUMP: cmp {{[a-z0-9]+}}, 1{{$}}
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
