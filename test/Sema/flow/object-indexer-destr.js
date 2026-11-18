/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

'use strict';

let obj: {[string]: number} = {a: 1, b: 2, c: 3};

// Named property + rest: the property resolves through the indexer (type
// number), and the rest binding is an exact object with the same indexer.
let {a, ...rest1} = obj;

// Rest only: an exact object with the same indexer.
let {...rest2} = obj;

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%object.2 = object({
// CHECK-NEXT:  [string]: number
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'obj' Let : %object.2
// CHECK-NEXT:        Decl %d.3 'a' Let : number
// CHECK-NEXT:        Decl %d.4 'rest1' Let : %object.2
// CHECK-NEXT:        Decl %d.5 'rest2' Let : %object.2
// CHECK-NEXT:        Decl %d.6 'arguments' Var Arguments

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
// CHECK-NEXT:                        NumericLiteral : 2
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'c'
// CHECK-NEXT:                        NumericLiteral : 3
// CHECK-NEXT:                Id 'obj' [D:E:%d.2 'obj']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'obj' [D:E:%d.2 'obj'] : %object.2
// CHECK-NEXT:                ObjectPattern : %object.2
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'a'
// CHECK-NEXT:                        Id 'a' [D:E:%d.3 'a'] : number
// CHECK-NEXT:                    RestElement
// CHECK-NEXT:                        Id 'rest1' [D:E:%d.4 'rest1'] : %object.2
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'obj' [D:E:%d.2 'obj'] : %object.2
// CHECK-NEXT:                ObjectPattern : %object.2
// CHECK-NEXT:                    RestElement
// CHECK-NEXT:                        Id 'rest2' [D:E:%d.5 'rest2'] : %object.2
