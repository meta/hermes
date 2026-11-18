/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -fno-std-globals -typed -dump-sema %s | %FileCheckOrRegen %s --match-full-lines

let d: {[string]: number} = {a: 1, b: 2};

// Spreading an indexer object yields an object with the same indexer.
let copy: {[string]: number} = {...d};

// Named property + indexer spread: the result is an indexer object whose value
// type is the union of the named value types and the indexer value type.
let withName: {[string]: number | string} = {...d, x: "s"};

// Order does not matter; the result still collapses to one indexer.
let nameFirst: {[string]: number | string} = {x: "s", ...d};

// Spreading an indexer object and a plain object folds the plain field types
// into the value-type union.
let plain: {a: boolean} = {a: true};
let merged: {[string]: number | boolean} = {...d, ...plain};

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%object.2 = object({
// CHECK-NEXT:  [string]: number
// CHECK-NEXT:})
// CHECK-NEXT:%object.3 = object({
// CHECK-NEXT:  [string]: %union.6
// CHECK-NEXT:})
// CHECK-NEXT:%object.4 = object({
// CHECK-NEXT:  a: boolean
// CHECK-NEXT:})
// CHECK-NEXT:%object.5 = object({
// CHECK-NEXT:  [string]: %union.7
// CHECK-NEXT:})
// CHECK-NEXT:%union.6 = union(string | number)
// CHECK-NEXT:%union.7 = union(boolean | number)

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'd' Let : %object.2
// CHECK-NEXT:        Decl %d.3 'copy' Let : %object.2
// CHECK-NEXT:        Decl %d.4 'withName' Let : %object.3
// CHECK-NEXT:        Decl %d.5 'nameFirst' Let : %object.3
// CHECK-NEXT:        Decl %d.6 'plain' Let : %object.4
// CHECK-NEXT:        Decl %d.7 'merged' Let : %object.5
// CHECK-NEXT:        Decl %d.8 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.2
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'a'
// CHECK-NEXT:                        NumericLiteral : 1
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'b'
// CHECK-NEXT:                        NumericLiteral : 2
// CHECK-NEXT:                Id 'd' [D:E:%d.2 'd']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.2
// CHECK-NEXT:                    SpreadElement
// CHECK-NEXT:                        Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                Id 'copy' [D:E:%d.3 'copy']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.3
// CHECK-NEXT:                    SpreadElement
// CHECK-NEXT:                        Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'x'
// CHECK-NEXT:                        StringLiteral : "s"
// CHECK-NEXT:                Id 'withName' [D:E:%d.4 'withName']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.3
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'x'
// CHECK-NEXT:                        StringLiteral : "s"
// CHECK-NEXT:                    SpreadElement
// CHECK-NEXT:                        Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                Id 'nameFirst' [D:E:%d.5 'nameFirst']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.4
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'a'
// CHECK-NEXT:                        BooleanLiteral : true
// CHECK-NEXT:                Id 'plain' [D:E:%d.6 'plain']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.5
// CHECK-NEXT:                    SpreadElement
// CHECK-NEXT:                        Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                    SpreadElement
// CHECK-NEXT:                        Id 'plain' [D:E:%d.6 'plain'] : %object.4
// CHECK-NEXT:                Id 'merged' [D:E:%d.7 'merged']
