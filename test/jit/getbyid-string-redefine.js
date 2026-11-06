/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- a | %FileCheck --check-prefix=OUTA %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- b | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -gc-sanitize-handles=1 %s -- b | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- c | %FileCheck --check-prefix=OUTA %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- d | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- a 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- b 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- c 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- d 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// Mutations of a hit Method block's property, one scenario per process
// (String.prototype cannot be reset). In each, getFoo's version 2 is
// compiled at its 16th call against String.prototype's current class
// and its Method block serves calls 17-20 (4 hits, the positive
// evidence); then the property changes and the next reads, before any
// recompile (two declines at most), must see the change.
// (a) foo redefined as an accessor with a strict getter: a flags
//     transition, a new class with no ID (HiddenClass.cpp:613 on); the
//     read runs the getter with a primitive `this`: "string".
// (b) delete foo (String.prototype becomes a cacheable dictionary, a new
//     class), read while absent: undefined; then bar takes foo's freed
//     slot and foo returns in a new slot: 3.
// (c) and (d): (a) and (b) from a cacheable-dictionary String.prototype
//     (an added property deleted before anything compiles); the
//     redefinition and the deletion each give a new no-cache dictionary
//     (HiddenClass.cpp:432-434, :595-611).
// Fails before the feature (no hits) and if the class-ID guard is not
// checked: (b) and (d) then read bar's 2 from foo's old slot, and (a) and
// (c) return the slot's accessor pair instead of the getter's result.
function main(scenario) {
  var SP = String.prototype;
  function getFoo(s) {
    return s.foo;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  if (scenario === 'c' || scenario === 'd') {
    SP.tmpDict = 0;
    delete SP.tmpDict;
  }
  SP.foo = 1;
  print(count(getFoo, 'abc', 20, 1));
  if (scenario === 'a' || scenario === 'c') {
    Object.defineProperty(SP, 'foo', {
      get: function () {
        'use strict';
        return typeof this;
      },
      configurable: true,
    });
    print(getFoo('abc'));
  } else {
    delete SP.foo;
    var absent = getFoo('abc');
    SP.bar = 2;
    SP.foo = 3;
    print(absent, getFoo('abc'));
  }
}
main(hermescli.getScriptArgs()[0]);

// OUTA: 20
// OUTA-NEXT: string
// OUTB: 20
// OUTB-NEXT: undefined 3
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdStringHits: 4{{$}}
