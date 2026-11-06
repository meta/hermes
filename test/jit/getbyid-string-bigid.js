/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=DUMP,DUMP%jit_arch_prefix %s
// REQUIRES: jit
// Debugger builds compile new Function bodies with an AsyncBreakCheck,
// which the JIT does not implement.
// UNSUPPORTED: debugger

// String.prototype's class gets a lazy JIT ID above 4095 that is not a
// multiple of 4096, which arm64's compare cannot take as an immediate
// (a64::Utils::isAddSubImm), so the Method block's compare materializes
// it in w16. As in getbyid-poly-emitted-arm64.js, 4200 tiny functions
// compiled warm first each assign one ID. getCC has no loop, so its
// first two calls run in the interpreter (it compiles once its execution
// count reaches -Xjit-threshold=2); version 1
// (no string seen by JIT code: NotPrepared) declines on calls 3-18 and
// call 18 recompiles; calls 19-100 hit: 82. An unrelated property then
// gives String.prototype a class without an ID: the next 8 calls must
// miss (8 declines, no crossing), so NumByIdStringHits stays 82.
// Fails before the feature (0 hits) and, on arm64, if the compare lost
// the ID (a compare against itself always matches: 90 hits).
function main() {
  var N = 4200;
  for (var i = 0; i < N; ++i) {
    var o = {};
    o['k' + i] = i;
    o.x = 1;
    var f = new Function('o', 'return o.x;');
    // Two interpreted calls fill the cache; the third compiles and
    // specializes on o's class, assigning it an ID.
    f(o);
    f(o);
    f(o);
  }
  var SP = String.prototype;
  function getCC(s) {
    return s.charCodeAt;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  var cc = SP.charCodeAt;
  print(count(getCC, 'abc', 100, cc));
  SP.unrelatedBig = 1;
  print(count(getCC, 'abc', 8, cc));
}
main();

// OUT: 100
// OUT-NEXT: 8
// CNT: NumByIdStringHits: 82{{$}}
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// The block's ID load and its compare against an ID of four hex digits
// (above 4095), adjacent, so neither can match the read-cache path's
// slot load and `cmp ..., 6` that follow.
// DUMPX86: movzx [[RID:e[a-z0-9]+]], word ptr
// DUMPX86-NEXT: cmp [[RID]], 0x{{[1-9A-F][0-9A-F][0-9A-F][0-9A-F]}}{{$}}
// DUMPX86-NEXT: jne
// DUMPARM64: ldrh [[RID:w[0-9]+]]
// DUMPARM64-NEXT: mov w16, 0x{{[1-9A-F][0-9A-F][0-9A-F][0-9A-F]}}{{$}}
// DUMPARM64-NEXT: cmp [[RID]], w16
// DUMPARM64-NEXT: b.ne
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getCC' (version 2)
