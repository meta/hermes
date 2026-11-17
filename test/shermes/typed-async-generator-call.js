/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -O0 -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -verify-ir %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O0 -Xdump-functions=main -dump-ir %s | %FileCheck --match-full-lines --check-prefix=IR %s

// A call to a typed async function returns a promise, and a call to a typed
// generator returns a generator object, not a value of the annotated return
// type. Typed mode has no promise or generator types, so the FlowChecker
// types such a call 'mixed' and IRGen types it 'object'. Assigning the result
// to a concrete type is a compile error, tested in
// Sema/flow/async-generator-call-error.js.

'use strict';

function main(): void {
  async function af(): number {
    return 1;
  }
  function* g(): number {
    yield 5;
    return 1;
  }

  // The value is a promise. The CallInst is 'object', so -O does not fold
  // typeof to the name of a primitive type.
  const r = af();
  print(typeof r);
  // CHECK: object

  // The call is 'mixed' in Flow, which flows into 'any', so `.next()`
  // type-checks on the 'any' binding; at -O the CallInst is not intersected
  // with a generator object into NoType.
  const it: any = g();
  print(typeof it, it.next().value);
  // CHECK-NEXT: object 5

  // These calls are not typed by IRGen; the Flow type of the call alone
  // decides whether a cast is emitted.
  print(typeof $SHBuiltin.call(af, undefined));
  // CHECK-NEXT: object
  print(typeof af.call(undefined));
  // CHECK-NEXT: object

  // `await` accepts the 'mixed' result (its own type is 'any' for now).
  async function useAwait(): number {
    const v: any = await af();
    return 2;
  }
  print(typeof useAwait());
  // CHECK-NEXT: object
}

main();

// The printed IR function type shows only the return type, without the async
// or generator flag, so af and g both print as function(number).
// IR-LABEL: function main(): any [typed]
// IR:         {{%.*}} = LoadFrameInst (:any) %1: environment, [%VS1.af]: any
// IR-NEXT:    [[AF:%.*]] = CheckedTypeCastInst (:function(number)) {{%.*}}: any, type(function(number))
// IR-NEXT:    {{%.*}} = CallInst [njsf] (:object) [[AF]]: function(number), {{.*}}
// IR:         {{%.*}} = LoadFrameInst (:any) %1: environment, [%VS1.g]: any
// IR-NEXT:    [[G:%.*]] = CheckedTypeCastInst (:function(number)) {{%.*}}: any, type(function(number))
// IR-NEXT:    {{%.*}} = CallInst [njsf] (:object) [[G]]: function(number), {{.*}}
