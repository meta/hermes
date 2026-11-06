/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// REQUIRES: jit, jit-arch-arm64
// UNSUPPORTED: handle_san
// Debugger builds compile new Function bodies with an AsyncBreakCheck,
// which the JIT does not implement.
// UNSUPPORTED: debugger

// arm64 emission of the polymorphic GetById tier: the recording call
// goes through the site record in x5, and the chain compares lazy JIT
// IDs. Lazy IDs are assigned only when emitted code bakes a class in,
// so 4200 tiny functions are compiled warm first, each specializing
// its single GetById on its own hidden class: that assigns ~4200 IDs,
// and the chain in get() then compares IDs above 4095 that are not
// multiples of 4096, which a64::Utils::isAddSubImm rejects, so each
// compare materializes the ID in w16 first.
function main() {
  var N = 4200;
  for (var i = 0; i < N; ++i) {
    var o = {};
    o['k' + i] = i;
    o.x = 1;
    var f = new Function('o', 'return o.x;');
    // Three calls: interpreted warm-up fills the cache, the threshold
    // compile (-Xjit-threshold=2) specializes on o's class.
    f(o); f(o); f(o);
  }
  function get(o) {
    return o.v;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 1]);
    return s;
  }
  function P() {}
  P.prototype.v = 2;
  print(drive(get, [{v: 1}, new P()], 1000));
}
main();

// OUT: 1500
// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'get'{{$}}
// CHECK: ldr x16, [x5, {{[0-9]+}}]
// CHECK-NEXT: blr x16
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'get'{{$}}
// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// CHECK: // Get polymorphic chain (2 entries)
// CHECK: ldrh w{{[0-9]+}}
// The own entry's receiver-ID compare, then the prototype entry's
// receiver-ID and prototype-ID compares, each through w16.
// CHECK: mov w16, 0x{{[0-9A-F]+}}
// CHECK-NEXT: cmp w{{[0-9]+}}, w16
// CHECK: mov w16, 0x{{[0-9A-F]+}}
// CHECK-NEXT: cmp w{{[0-9]+}}, w16
// CHECK: mov w16, 0x{{[0-9A-F]+}}
// CHECK-NEXT: cmp w{{[0-9]+}}, w16
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
