/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -dump-ir -O0 %s | %FileCheck --match-full-lines %s

// D7 and D8 in doc/plans/ir-type/phase3-5-design.md.

(function () {

// D7: an object type with an indexer is an open key set, which an IR
// exactObject (a closed field list) cannot represent, so it lowers to object.
function indexed(d: {[string]: number}): number {
  return 1;
}
// CHECK-LABEL: function indexed(d: object): any [typed]

// D8: field variance is dropped, so {+x: number}, {-x: number} and
// {x: number} lower to the same IR type.
function readOnly(o: {+x: number}): number {
  return 1;
}
// CHECK-LABEL: function readOnly(o: exactObject(x: number)): any [typed]
function writeOnly(o: {-x: number}): number {
  return 1;
}
// CHECK-LABEL: function writeOnly(o: exactObject(x: number)): any [typed]
function readWrite(o: {x: number}): number {
  return 1;
}
// CHECK-LABEL: function readWrite(o: exactObject(x: number)): any [typed]

// The printed types above could coincide for distinct ids. Static fields are
// typed frame variables, and a {x: number} value is stored into the
// {+x: number} and {-x: number} fields as is: had the IR types different ids,
// IRGen would narrow the value to the field's type first.
class S {
  static ro: {+x: number};
  static wo: {-x: number};
}
function store(o: {x: number}): void {
  S.ro = o;
  S.wo = o;
}
// CHECK-LABEL: function store(o: exactObject(x: number)): any [typed]
// CHECK:  [[O1:%[0-9]+]] = CheckedTypeCastInst (:exactObject(x: number)) %{{[0-9]+}}: any, type(exactObject(x: number))
// CHECK-NEXT:  StoreFrameInst %{{[0-9]+}}: environment, [[O1]]: exactObject(x: number), [%VS{{[0-9]+}}.ro]: empty|exactObject(x: number)
// CHECK:  [[O2:%[0-9]+]] = CheckedTypeCastInst (:exactObject(x: number)) %{{[0-9]+}}: any, type(exactObject(x: number))
// CHECK-NEXT:  StoreFrameInst %{{[0-9]+}}: environment, [[O2]]: exactObject(x: number), [%VS{{[0-9]+}}.wo]: empty|exactObject(x: number)

})();
