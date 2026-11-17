/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// Mirror of object-destr-rest-param.js for arrow functions.

'use strict';

// Indexer-typed parameter: the rest binding is an exact object with the same
// indexer.
const fi = ({a, ...restIdx}: {[string]: number}) => {
  return restIdx;
};

// Regular exact-object parameter: the rest binding is an exact object of the
// remaining fields.
const fr = ({a, ...restReg}: {a: number, b: number}) => {
  return restReg;
};

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%object.2 = object({
// CHECK-NEXT:  [string]: number
// CHECK-NEXT:})
// CHECK-NEXT:%function.3 = function(%object.2): any
// CHECK-NEXT:%object.4 = object({
// CHECK-NEXT:  a: number
// CHECK-NEXT:  b: number
// CHECK-NEXT:})
// CHECK-NEXT:%function.5 = function(%object.4): any
// CHECK-NEXT:%object.6 = object({
// CHECK-NEXT:  b: number
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'fi' Const : %function.3
// CHECK-NEXT:        Decl %d.3 'fr' Const : %function.5
// CHECK-NEXT:        Decl %d.4 'arguments' Var Arguments
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:            Decl %d.5 'a' Parameter : number
// CHECK-NEXT:            Decl %d.6 'restIdx' Parameter : %object.2
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.3
// CHECK-NEXT:            Decl %d.7 'a' Parameter : number
// CHECK-NEXT:            Decl %d.8 'restReg' Parameter : %object.6

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            StringLiteral : "use strict"
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.3
// CHECK-NEXT:                    ObjectPattern : %object.2
// CHECK-NEXT:                        Property
// CHECK-NEXT:                            Id 'a'
// CHECK-NEXT:                            Id 'a' [D:E:%d.5 'a'] : number
// CHECK-NEXT:                        RestElement
// CHECK-NEXT:                            Id 'restIdx' [D:E:%d.6 'restIdx'] : %object.2
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'restIdx' [D:E:%d.6 'restIdx'] : %object.2
// CHECK-NEXT:                Id 'fi' [D:E:%d.2 'fi']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.5
// CHECK-NEXT:                    ObjectPattern : %object.4
// CHECK-NEXT:                        Property
// CHECK-NEXT:                            Id 'a'
// CHECK-NEXT:                            Id 'a' [D:E:%d.7 'a'] : number
// CHECK-NEXT:                        RestElement : %object.6
// CHECK-NEXT:                            Id 'restReg' [D:E:%d.8 'restReg'] : %object.6
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'restReg' [D:E:%d.8 'restReg'] : %object.6
// CHECK-NEXT:                Id 'fr' [D:E:%d.3 'fr']
