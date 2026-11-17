/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// Mirror of destr-param.js for arrow functions. Destructuring parameters work
// the same way on arrows as on regular functions, both without a contextual
// type (the const has no annotation) and with one (the const is annotated with
// a function type, which drives the contextual-typing path).

'use strict';

// Object destructuring param.
const foo = ({x, y}: {x: number, y: string}): void => {
  let a: number = x;
  let b: string = y;
};

// Array destructuring param (tuple).
const bar = ([a, b]: [number, string]): void => {
  let c: number = a;
  let d: string = b;
};

// Nested destructuring param.
const baz = ({a: {b}}: {a: {b: number}}): void => {
  let c: number = b;
};

// Destructuring with default value.
const qux = ({x}: {x: number} = {x: 0}): void => {
  let a: number = x;
};

// Contextually-typed arrow: the function-type annotation on the const is the
// constraint. Destructuring, defaults and nested patterns are all supported.
const cObj: ({x: number, y: string}) => number =
    ({x, y}: {x: number, y: string}): number => x;
const cTup: ([number, string]) => number =
    ([a, b]: [number, string]): number => a;
const cDflt: ({x: number}) => number =
    ({x}: {x: number} = {x: 0}): number => x;
const cORest: ({a: number, b: number}) => number =
    ({a, ...rest}: {a: number, b: number}): number => a;

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%function.2 = function(%object.6): number
// CHECK-NEXT:%function.3 = function(%tuple.8): number
// CHECK-NEXT:%function.4 = function(%object.13): number
// CHECK-NEXT:%function.5 = function(%object.16): number
// CHECK-NEXT:%object.6 = object({
// CHECK-NEXT:  x: number
// CHECK-NEXT:  y: string
// CHECK-NEXT:})
// CHECK-NEXT:%function.7 = function(%object.6): void
// CHECK-NEXT:%tuple.8 = tuple(number, string)
// CHECK-NEXT:%function.9 = function(%tuple.8): void
// CHECK-NEXT:%object.10 = object({
// CHECK-NEXT:  b: number
// CHECK-NEXT:})
// CHECK-NEXT:%object.11 = object({
// CHECK-NEXT:  a: %object.10
// CHECK-NEXT:})
// CHECK-NEXT:%function.12 = function(%object.11): void
// CHECK-NEXT:%object.13 = object({
// CHECK-NEXT:  x: number
// CHECK-NEXT:})
// CHECK-NEXT:%function.14 = function(%object.13): void
// CHECK-NEXT:%function.15 = function(%object.13): number
// CHECK-NEXT:%object.16 = object({
// CHECK-NEXT:  a: number
// CHECK-NEXT:  b: number
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'foo' Const : %function.7
// CHECK-NEXT:        Decl %d.3 'bar' Const : %function.9
// CHECK-NEXT:        Decl %d.4 'baz' Const : %function.12
// CHECK-NEXT:        Decl %d.5 'qux' Const : %function.14
// CHECK-NEXT:        Decl %d.6 'cObj' Const : %function.2
// CHECK-NEXT:        Decl %d.7 'cTup' Const : %function.3
// CHECK-NEXT:        Decl %d.8 'cDflt' Const : %function.4
// CHECK-NEXT:        Decl %d.9 'cORest' Const : %function.5
// CHECK-NEXT:        Decl %d.10 'arguments' Var Arguments
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:            Decl %d.11 'x' Parameter : number
// CHECK-NEXT:            Decl %d.12 'y' Parameter : string
// CHECK-NEXT:            Decl %d.13 'a' Let : number
// CHECK-NEXT:            Decl %d.14 'b' Let : string
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.3
// CHECK-NEXT:            Decl %d.15 'a' Parameter : number
// CHECK-NEXT:            Decl %d.16 'b' Parameter : string
// CHECK-NEXT:            Decl %d.17 'c' Let : number
// CHECK-NEXT:            Decl %d.18 'd' Let : string
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.4
// CHECK-NEXT:            Decl %d.19 'b' Parameter : number
// CHECK-NEXT:            Decl %d.20 'c' Let : number
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.5
// CHECK-NEXT:            Decl %d.21 'x' Parameter : number
// CHECK-NEXT:            Scope %s.6
// CHECK-NEXT:                Decl %d.22 'a' Let : number
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.7
// CHECK-NEXT:            Decl %d.23 'x' Parameter : number
// CHECK-NEXT:            Decl %d.24 'y' Parameter : string
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.8
// CHECK-NEXT:            Decl %d.25 'a' Parameter : number
// CHECK-NEXT:            Decl %d.26 'b' Parameter : string
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.9
// CHECK-NEXT:            Decl %d.27 'x' Parameter : number
// CHECK-NEXT:            Scope %s.10
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.11
// CHECK-NEXT:            Decl %d.28 'a' Parameter : number
// CHECK-NEXT:            Decl %d.29 'rest' Parameter : %object.10

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            StringLiteral : "use strict"
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.7
// CHECK-NEXT:                    ObjectPattern : %object.6
// CHECK-NEXT:                        Property
// CHECK-NEXT:                            Id 'x'
// CHECK-NEXT:                            Id 'x' [D:E:%d.11 'x'] : number
// CHECK-NEXT:                        Property
// CHECK-NEXT:                            Id 'y'
// CHECK-NEXT:                            Id 'y' [D:E:%d.12 'y'] : string
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        VariableDeclaration
// CHECK-NEXT:                            VariableDeclarator
// CHECK-NEXT:                                Id 'x' [D:E:%d.11 'x'] : number
// CHECK-NEXT:                                Id 'a' [D:E:%d.13 'a']
// CHECK-NEXT:                        VariableDeclaration
// CHECK-NEXT:                            VariableDeclarator
// CHECK-NEXT:                                Id 'y' [D:E:%d.12 'y'] : string
// CHECK-NEXT:                                Id 'b' [D:E:%d.14 'b']
// CHECK-NEXT:                Id 'foo' [D:E:%d.2 'foo']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.9
// CHECK-NEXT:                    ArrayPattern : %tuple.8
// CHECK-NEXT:                        Id 'a' [D:E:%d.15 'a'] : number
// CHECK-NEXT:                        Id 'b' [D:E:%d.16 'b'] : string
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        VariableDeclaration
// CHECK-NEXT:                            VariableDeclarator
// CHECK-NEXT:                                Id 'a' [D:E:%d.15 'a'] : number
// CHECK-NEXT:                                Id 'c' [D:E:%d.17 'c']
// CHECK-NEXT:                        VariableDeclaration
// CHECK-NEXT:                            VariableDeclarator
// CHECK-NEXT:                                Id 'b' [D:E:%d.16 'b'] : string
// CHECK-NEXT:                                Id 'd' [D:E:%d.18 'd']
// CHECK-NEXT:                Id 'bar' [D:E:%d.3 'bar']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.12
// CHECK-NEXT:                    ObjectPattern : %object.11
// CHECK-NEXT:                        Property
// CHECK-NEXT:                            Id 'a'
// CHECK-NEXT:                            ObjectPattern : %object.10
// CHECK-NEXT:                                Property
// CHECK-NEXT:                                    Id 'b'
// CHECK-NEXT:                                    Id 'b' [D:E:%d.19 'b'] : number
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        VariableDeclaration
// CHECK-NEXT:                            VariableDeclarator
// CHECK-NEXT:                                Id 'b' [D:E:%d.19 'b'] : number
// CHECK-NEXT:                                Id 'c' [D:E:%d.20 'c']
// CHECK-NEXT:                Id 'baz' [D:E:%d.4 'baz']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.14
// CHECK-NEXT:                    AssignmentPattern
// CHECK-NEXT:                        ObjectPattern : %object.13
// CHECK-NEXT:                            Property
// CHECK-NEXT:                                Id 'x'
// CHECK-NEXT:                                Id 'x' [D:E:%d.21 'x'] : number
// CHECK-NEXT:                        ObjectExpression : %object.13
// CHECK-NEXT:                            Property
// CHECK-NEXT:                                Id 'x'
// CHECK-NEXT:                                NumericLiteral : 0
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        VariableDeclaration
// CHECK-NEXT:                            VariableDeclarator
// CHECK-NEXT:                                Id 'x' [D:E:%d.21 'x'] : number
// CHECK-NEXT:                                Id 'a' [D:E:%d.22 'a']
// CHECK-NEXT:                Id 'qux' [D:E:%d.5 'qux']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.2
// CHECK-NEXT:                    ObjectPattern : %object.6
// CHECK-NEXT:                        Property
// CHECK-NEXT:                            Id 'x'
// CHECK-NEXT:                            Id 'x' [D:E:%d.23 'x'] : number
// CHECK-NEXT:                        Property
// CHECK-NEXT:                            Id 'y'
// CHECK-NEXT:                            Id 'y' [D:E:%d.24 'y'] : string
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'x' [D:E:%d.23 'x'] : number
// CHECK-NEXT:                Id 'cObj' [D:E:%d.6 'cObj']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.3
// CHECK-NEXT:                    ArrayPattern : %tuple.8
// CHECK-NEXT:                        Id 'a' [D:E:%d.25 'a'] : number
// CHECK-NEXT:                        Id 'b' [D:E:%d.26 'b'] : string
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'a' [D:E:%d.25 'a'] : number
// CHECK-NEXT:                Id 'cTup' [D:E:%d.7 'cTup']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.15
// CHECK-NEXT:                    AssignmentPattern
// CHECK-NEXT:                        ObjectPattern : %object.13
// CHECK-NEXT:                            Property
// CHECK-NEXT:                                Id 'x'
// CHECK-NEXT:                                Id 'x' [D:E:%d.27 'x'] : number
// CHECK-NEXT:                        ObjectExpression : %object.13
// CHECK-NEXT:                            Property
// CHECK-NEXT:                                Id 'x'
// CHECK-NEXT:                                NumericLiteral : 0
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'x' [D:E:%d.27 'x'] : number
// CHECK-NEXT:                Id 'cDflt' [D:E:%d.8 'cDflt']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.5
// CHECK-NEXT:                    ObjectPattern : %object.16
// CHECK-NEXT:                        Property
// CHECK-NEXT:                            Id 'a'
// CHECK-NEXT:                            Id 'a' [D:E:%d.28 'a'] : number
// CHECK-NEXT:                        RestElement : %object.10
// CHECK-NEXT:                            Id 'rest' [D:E:%d.29 'rest'] : %object.10
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'a' [D:E:%d.28 'a'] : number
// CHECK-NEXT:                Id 'cORest' [D:E:%d.9 'cORest']
