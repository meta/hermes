/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -gc-sanitize-handles=1 -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit

// The string twin of getbyid-poly-rooted-obj.js: a heap-string result
// on the crossing call whose recompile allocates usedHCs (the first
// lazy-ID assignment in the process), moving every live object under
// MallocGC + handle sanitization. See getbyid-poly-rooted-obj.js for why
// the threshold is 16 and -gc-sanitize-handles=1, and for the DUMP
// check, which pins that this compile makes the process's first lazy-ID
// assignment (the chain's first compare is against ID 1). The 16th call
// is the crossing; it returns arr[15 & 3]'s heap string, 's4'.
function main() {
  function get(o) {
    return o.v;
  }
  function drive(f, arr, out, n) {
    for (var i = 0; i < n; ++i)
      out[i] = f(arr[i & 3]);
  }
  // Heap strings, not literals.
  function str(n) {
    return 's' + n;
  }
  var strs = [
    {v: str(1)},
    {a: 0, v: str(2)},
    {b: 0, v: str(3)},
    {c: 0, v: str(4)},
  ];
  var s = [];
  drive(get, strs, s, 64);
  print(s[15], s[63]);
}
main();

// CHECK: s4 s4
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP-NOT: cmp
// DUMP: cmp {{[a-z0-9]+}}, 1{{$}}
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
