/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -gc-sanitize-handles=1 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// Receivers that must not take a string block, at sites that hold one.
// getLen's and getCC's version 2 (call 16 each) hold the Length and
// Method blocks; calls 17-20 hit (8). Then numbers and booleans (not
// pointers), symbols (not a pointer tag), BigInts (the pointer tag next
// to the string tag) and String objects (object tag) read through the
// object path or the slow call: 5 declines per site, under 16. null and
// undefined throw TypeError from the slow call, which longjmps past the
// helper (no decline). The last two counts hit again: 20.
// NumByIdStringHits = 28, NumRecompiles = 2.
// Fails before the feature (no hits), and if the string compare admitted
// other pointer tags (the BigInt and the String object would be read as
// StringPrimitives).
function main() {
  var SP = String.prototype;
  function getLen(x) {
    return x.length;
  }
  function getCC(x) {
    return x.charCodeAt;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  // "TypeError" if f(x) throws one.
  function tryGet(f, x) {
    try {
      f(x);
      return 'no throw';
    } catch (e) {
      return e instanceof TypeError ? 'TypeError' : 'other';
    }
  }
  var cc = SP.charCodeAt;
  print(count(getLen, 'abc', 20, 3), count(getCC, 'abc', 20, cc));
  print(
      getLen(5),
      getLen(true),
      getLen(new String('abcde')),
      getLen(Symbol('q')),
      getLen(10n));
  print(
      getCC(5),
      getCC(false),
      getCC(new String('x')) === cc,
      getCC(Symbol('q')),
      getCC(10n));
  print(tryGet(getLen, null), tryGet(getLen, undefined), tryGet(getCC, null));
  print(count(getLen, 'abcd', 10, 4), count(getCC, 'xy', 10, cc));
}
main();

// OUT: 20 20
// OUT-NEXT: undefined undefined 5 undefined undefined
// OUT-NEXT: undefined undefined true undefined undefined
// OUT-NEXT: TypeError TypeError TypeError
// OUT-NEXT: 10 10
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdStringHits: 28{{$}}
