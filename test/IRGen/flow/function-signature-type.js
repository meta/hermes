/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=sameSig,paramType,paramCount,paramOrder,optional,rest,thisParam -typed -dump-ir -O0 %s | %FileCheck --match-full-lines %s

// Every part of a function signature is lowered into its IR function type.
// Printing shows only the return type, so each case below is a union of two
// function types that have the same return type and differ in one part of
// the signature. Distinct IR function types are unrelated (D4), so the union
// keeps both arms if and only if the lowered types differ. Dropping a part of
// the signature in the lowering collapses the corresponding union to a single
// arm.

(function () {

type Base = (x: number) => string;

// Parameter names are not part of the type: one arm.
function sameSig(u: Base | ((y: number) => string)): void {}
// CHECK-LABEL: function sameSig(u: function(string)): any [typed]

function paramType(u: Base | ((x: string) => string)): void {}
// CHECK-LABEL: function paramType(u: function(string)|function(string)): any [typed]

function paramCount(u: Base | ((x: number, y: number) => string)): void {}
// CHECK-LABEL: function paramCount(u: function(string)|function(string)): any [typed]

function paramOrder(
  u: ((x: number, y: string) => string) | ((x: string, y: number) => string),
): void {}
// CHECK-LABEL: function paramOrder(u: function(string)|function(string)): any [typed]

function optional(u: Base | ((x?: number) => string)): void {}
// CHECK-LABEL: function optional(u: function(string)|function(string)): any [typed]

function rest(u: Base | ((x: number, ...xs: number[]) => string)): void {}
// CHECK-LABEL: function rest(u: function(string)|function(string)): any [typed]

function thisParam(u: Base | ((this: number, x: number) => string)): void {}
// CHECK-LABEL: function thisParam(u: function(string)|function(string)): any [typed]

})();
