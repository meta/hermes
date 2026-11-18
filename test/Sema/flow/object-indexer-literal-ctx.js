/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -fno-std-globals --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// Literals in field initializers and constructor arguments are constrained by
// the declared types, so they become indexer objects.
class K {
  f: {[string]: number} = {a: 1};
  constructor(o: {[string]: number}) {}
}
new K({x: 1});

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%class.2 = class(K {
// CHECK-NEXT:  %constructor: %function.5
// CHECK-NEXT:  %homeObject: %class.6
// CHECK-NEXT:  f: %object.4
// CHECK-NEXT:})
// CHECK-NEXT:%class_constructor.3 = class_constructor(%class.2)
// CHECK-NEXT:%object.4 = object({
// CHECK-NEXT:  [string]: number
// CHECK-NEXT:})
// CHECK-NEXT:%function.5 = function(this: %class.2, o: %object.4): void
// CHECK-NEXT:%class.6 = class( {
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'K' Class : %class_constructor.3
// CHECK-NEXT:        Decl %d.3 'arguments' Var Arguments
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.3
// CHECK-NEXT:            Decl %d.4 'arguments' Var Arguments
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.4
// CHECK-NEXT:            Decl %d.5 'o' Parameter : %object.4
// CHECK-NEXT:            Decl %d.6 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        ClassDeclaration Scope %s.2
// CHECK-NEXT:            Id 'K' [D:E:%d.2 'K']
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:                ClassProperty : %object.4
// CHECK-NEXT:                    Id 'f'
// CHECK-NEXT:                    ObjectExpression : %object.4
// CHECK-NEXT:                        Property
// CHECK-NEXT:                            Id 'a'
// CHECK-NEXT:                            NumericLiteral : 1
// CHECK-NEXT:                MethodDefinition : %function.5
// CHECK-NEXT:                    Id 'constructor'
// CHECK-NEXT:                    FunctionExpression : %function.5
// CHECK-NEXT:                        Id 'o' [D:E:%d.5 'o']
// CHECK-NEXT:                        BlockStatement
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            NewExpression : %class.2
// CHECK-NEXT:                Id 'K' [D:E:%d.2 'K'] : %class_constructor.3
// CHECK-NEXT:                ObjectExpression : %object.4
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'x'
// CHECK-NEXT:                        NumericLiteral : 1
