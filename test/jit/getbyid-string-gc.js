/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -gc-sanitize-handles=1 -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit

// Method and Length blocks across collections. -gc-sanitize-handles=1
// makes a HERMESVM_SANITIZE_HANDLES build (cmake-build-x86jit-hsan) move
// every live object at every allocation and free the old copy; other
// builds ignore the flag. getLen and getCC are warmed to version 2 (16
// calls each; the 16th recompiles). Each of the 10 rounds then reads
// both through their blocks, allocates a string and collects. The
// Method block reloads runtime.stringPrototype, a root the collector
// updates (Runtime.cpp:630-643), so it follows String.prototype and its
// class to their new copies. 20 reads, all hits.
// Fails before the feature (no hits) and, on the hsan tree, if the block
// embedded String.prototype's address (an ASan report or a wrong value).
function main() {
  function getLen(s) {
    return s.length;
  }
  function getCC(s) {
    return s.charCodeAt;
  }
  function warm(f, s, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(s);
    return r;
  }
  function rounds(getLen, getCC, s, cc, collect, n) {
    var ok = 0;
    var keep = [];
    for (var i = 0; i < n; ++i) {
      if (getLen(s) === 3)
        ++ok;
      if (getCC(s) === cc)
        ++ok;
      keep[i] = 'r' + i + s;
      collect();
    }
    return ok;
  }
  var s = 'abc';
  warm(getLen, s, 16);
  warm(getCC, s, 16);
  print(rounds(getLen, getCC, s, String.prototype.charCodeAt, gc, 10));
}
main();

// CHECK: 20
// CHECK: JIT counters:
// CHECK: NumByIdStringHits: 20{{$}}
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP: // Get string length (site {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
