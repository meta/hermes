/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

'use strict';

let obj: {a: number, b: string, c: boolean} =
    {a: 1, b: 'x', c: true};

// Basic rest: the rest binding is an exact object of the remaining fields.
let {a, ...rest1} = obj;

// Renamed property + rest.
let {a: aa, ...rest2} = obj;

// All fields consumed: rest is an empty exact object.
let {a: a3, b: b3, c: c3, ...rest3} = obj;

// 'any' source: rest propagates 'any'.
function f(x: any) {
  let {p, ...restAny} = x;
  return restAny;
}

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%object.2 = object({
// CHECK-NEXT:  a: number
// CHECK-NEXT:  b: string
// CHECK-NEXT:  c: boolean
// CHECK-NEXT:})
// CHECK-NEXT:%function.3 = function(x: any): any
// CHECK-NEXT:%object.4 = object({
// CHECK-NEXT:  b: string
// CHECK-NEXT:  c: boolean
// CHECK-NEXT:})
// CHECK-NEXT:%object.5 = object({
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'obj' Let : %object.2
// CHECK-NEXT:        Decl %d.3 'a' Let : number
// CHECK-NEXT:        Decl %d.4 'rest1' Let : %object.4
// CHECK-NEXT:        Decl %d.5 'aa' Let : number
// CHECK-NEXT:        Decl %d.6 'rest2' Let : %object.4
// CHECK-NEXT:        Decl %d.7 'a3' Let : number
// CHECK-NEXT:        Decl %d.8 'b3' Let : string
// CHECK-NEXT:        Decl %d.9 'c3' Let : boolean
// CHECK-NEXT:        Decl %d.10 'rest3' Let : %object.5
// CHECK-NEXT:        Decl %d.11 'f' Var : %function.3
// CHECK-NEXT:        Decl %d.12 'arguments' Var Arguments
// CHECK-NEXT:        hoistedFunction f
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:            Decl %d.13 'x' Parameter : any
// CHECK-NEXT:            Decl %d.14 'p' Let : any
// CHECK-NEXT:            Decl %d.15 'restAny' Let : any
// CHECK-NEXT:            Decl %d.16 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            StringLiteral : "use strict"
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.2
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'a'
// CHECK-NEXT:                        NumericLiteral : 1
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'b'
// CHECK-NEXT:                        StringLiteral : "x"
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'c'
// CHECK-NEXT:                        BooleanLiteral : true
// CHECK-NEXT:                Id 'obj' [D:E:%d.2 'obj']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'obj' [D:E:%d.2 'obj'] : %object.2
// CHECK-NEXT:                ObjectPattern : %object.2
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'a'
// CHECK-NEXT:                        Id 'a' [D:E:%d.3 'a'] : number
// CHECK-NEXT:                    RestElement : %object.4
// CHECK-NEXT:                        Id 'rest1' [D:E:%d.4 'rest1'] : %object.4
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'obj' [D:E:%d.2 'obj'] : %object.2
// CHECK-NEXT:                ObjectPattern : %object.2
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'a'
// CHECK-NEXT:                        Id 'aa' [D:E:%d.5 'aa'] : number
// CHECK-NEXT:                    RestElement : %object.4
// CHECK-NEXT:                        Id 'rest2' [D:E:%d.6 'rest2'] : %object.4
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'obj' [D:E:%d.2 'obj'] : %object.2
// CHECK-NEXT:                ObjectPattern : %object.2
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'a'
// CHECK-NEXT:                        Id 'a3' [D:E:%d.7 'a3'] : number
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'b'
// CHECK-NEXT:                        Id 'b3' [D:E:%d.8 'b3'] : string
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'c'
// CHECK-NEXT:                        Id 'c3' [D:E:%d.9 'c3'] : boolean
// CHECK-NEXT:                    RestElement : %object.5
// CHECK-NEXT:                        Id 'rest3' [D:E:%d.10 'rest3'] : %object.5
// CHECK-NEXT:        FunctionDeclaration : %function.3
// CHECK-NEXT:            Id 'f' [D:E:%d.11 'f']
// CHECK-NEXT:            Id 'x' [D:E:%d.13 'x']
// CHECK-NEXT:            BlockStatement
// CHECK-NEXT:                VariableDeclaration
// CHECK-NEXT:                    VariableDeclarator
// CHECK-NEXT:                        Id 'x' [D:E:%d.13 'x'] : any
// CHECK-NEXT:                        ObjectPattern : any
// CHECK-NEXT:                            Property
// CHECK-NEXT:                                Id 'p'
// CHECK-NEXT:                                Id 'p' [D:E:%d.14 'p'] : any
// CHECK-NEXT:                            RestElement
// CHECK-NEXT:                                Id 'restAny' [D:E:%d.15 'restAny'] : any
// CHECK-NEXT:                ReturnStatement
// CHECK-NEXT:                    Id 'restAny' [D:E:%d.15 'restAny'] : any
