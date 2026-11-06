/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -gc-sanitize-handles=1 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// A string block and a poly chain at the same site.
// getLen: 300 calls over an array, an object and a string, by i % 3.
// In version 1 every call declines: the two object classes alternate
// through the single read cache entry and strings never use it; the
// array's and the object's own `length` shapes are recorded and strings
// set sawString. Call 16 (i = 15) recompiles; version 2 emits the Length
// block, then a 2-entry poly chain, and every later call hits inline.
// Strings in version 2: i = 17, 20, ..., 299: 95 hits.
// getCC: 200 calls alternating an object with an own charCodeAt (i
// even) and a string (i odd). The object misses only at i = 0 (its class
// then stays in the read cache, which strings never touch), so the 16
// version-1 declines are i = 0 and the strings at i = 1, 3, ..., 29; call
// i = 29 recompiles. Version 2 emits the Method block, then a 1-entry
// chain. Strings in version 2: i = 31, 33, ..., 199: 85 hits.
// NumByIdStringHits = 180: object hits are not counted. NumRecompiles =
// 2; NumByIdDemotions = 0, since nothing declines in version 2.
// Fails before the feature (no string block), and if objects at a site
// with a string block missed their poly chain (NumByIdDemotions 2).
function main() {
  function getLen(x) {
    return x.length;
  }
  function getCC(x) {
    return x.charCodeAt;
  }
  // n calls of f(a), f(b), f(c) by i % 3; the sum of the results.
  function sum3(f, a, b, c, n) {
    var s = 0;
    for (var i = 0; i < n; ++i) {
      var k = i % 3;
      s += f(k === 0 ? a : k === 1 ? b : c);
    }
    return s;
  }
  // n calls alternating f(a) (i even) and f(b) (i odd);
  // "<matches of wa> <matches of wb>".
  function alt(f, a, wa, b, wb, n) {
    var ca = 0;
    var cb = 0;
    for (var i = 0; i < n; ++i) {
      if (i & 1) {
        if (f(b) === wb)
          ++cb;
      } else if (f(a) === wa) {
        ++ca;
      }
    }
    return ca + ' ' + cb;
  }
  var cc = String.prototype.charCodeAt;
  print(sum3(getLen, [1, 2], {length: 5}, 'abc', 300));
  print(alt(getCC, {charCodeAt: 7}, 7, 'abc', cc, 200));
}
main();

// OUT: 1000
// OUT-NEXT: 100 100
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP: // Get string length (site {{[0-9]+}})
// DUMP: // Get polymorphic chain (2 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic
// DUMP-NEXT: JIT ById string sites: 1 length, 0 method
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic
// DUMP-NEXT: JIT ById string sites: 0 length, 1 method
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdStringHits: 180{{$}}
