/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// new A() and new B() share the empty receiver class (receiver classes
// do not depend on the prototype); PA and PB differ by their extra
// property. new C() ({q}) and new D() ({r}) differ but share the
// prototype object, hence its class. main()'s own ById reads are the
// seven `.prototype` accesses, below the threshold.
function main() {
  function A() {}
  A.prototype.run = function () { return 1; };
  A.prototype.pa = 0;
  function B() {}
  B.prototype.run = function () { return 2; };
  B.prototype.pb = 0;
  function C() { this.q = 0; }
  C.prototype.run = function () { return 3; };
  function D() { this.r = 0; }
  D.prototype = C.prototype;
  function get(t) {
    return t.run;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 3])();
    return s;
  }
  print(drive(get, [new A(), new B(), new C(), new D()], 1000));
}
main();

// OUT: 2250
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 megamorphic
// CNT: NumRecompiles: 1{{$}}
// A chain that misses keeps declining and demotes; 0 proves the entries hit.
// CNT: NumByIdDemotions: 0{{$}}
