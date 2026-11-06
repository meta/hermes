/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=DUMP,DUMP%jit_arch_prefix %s
// REQUIRES: jit

// Out-of-line slots far enough out that the byte offset reaches
// 32 KiB, where arm64's emit_load_from_base_offset needs its temporary
// register (HV64: slot > 4096; HV32: slot > 8192). 9000 properties
// precede `x`, so both modes qualify. The big object is a cacheable
// dictionary (no deletes), which Eligibility allows as a property
// holder, own or prototype; the prototype case's receiver,
// Object.create(holder), is not a dictionary.
function main() {
  function mk(prefix, last) {
    var o = {};
    for (var i = 0; i < 9000; ++i)
      o[prefix + i] = i;
    o.x = last;
    return o;
  }
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var r = '';
    for (var i = 0; i < n; ++i)
      r = f(arr[i % 3]);
    return r;
  }
  var bigOwn = mk('a', 'own');
  var viaBig = Object.create(mk('b', 'proto'));
  var small = {x: 'small'};
  drive(get, [bigOwn, viaBig, small], 300);
  print(get(bigOwn), get(viaBig), get(small));
}
main();

// CHECK: own proto small
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (3 entries)
// The two big-slot chain entries (own, prototype) each load through a
// register offset; both must appear before the generic tier, whose
// comment closes the window.
// DUMPARM64: ldr {{[wx][0-9]+}}, [x{{[0-9]+}}, x{{[0-9]+}} lsl {{[23]}}]
// DUMPARM64: ldr {{[wx][0-9]+}}, [x{{[0-9]+}}, x{{[0-9]+}} lsl {{[23]}}]
// DUMP: // Read property cache
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
