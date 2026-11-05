/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -typed -dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// Test that nested generics are correctly specialized.
function outer<T>(x: T): T {
  var innerVar: number = 3;
  function inner<U>(y: U): U {
    return y;
  }
  return inner<T>(x);
}

outer<number>(1);
outer<string>('a');

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%function.2 = function(x: number): number
// CHECK-NEXT:%function.3 = function(x: string): string

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'outer' Var
// CHECK-NEXT:        Decl %d.3 'arguments' Var Arguments
// CHECK-NEXT:        Decl %d.4 'outer' Var : %function.2
// CHECK-NEXT:        Decl %d.5 'outer' Var : %function.3
// CHECK-NEXT:        hoistedFunction outer
// CHECK-NEXT:        hoistedFunction outer
// CHECK-NEXT:        hoistedFunction outer
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:            Decl %d.6 'x' Parameter
// CHECK-NEXT:            Decl %d.7 'innerVar' Var
// CHECK-NEXT:            Decl %d.8 'inner' Var
// CHECK-NEXT:            Decl %d.9 'arguments' Var Arguments
// CHECK-NEXT:            hoistedFunction inner
// CHECK-NEXT:        Func strict noImplicitReturn
// CHECK-NEXT:            Scope %s.3
// CHECK-NEXT:                Decl %d.10 'y' Parameter
// CHECK-NEXT:                Decl %d.11 'arguments' Var Arguments
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.4
// CHECK-NEXT:            Decl %d.12 'x' Parameter : number
// CHECK-NEXT:            Decl %d.13 'innerVar' Var : number
// CHECK-NEXT:            Decl %d.14 'inner' Var
// CHECK-NEXT:            Decl %d.15 'arguments' Var Arguments
// CHECK-NEXT:            Decl %d.16 'inner' Var : %function.2
// CHECK-NEXT:            hoistedFunction inner
// CHECK-NEXT:            hoistedFunction inner
// CHECK-NEXT:        Func strict noImplicitReturn
// CHECK-NEXT:            Scope %s.5
// CHECK-NEXT:                Decl %d.17 'y' Parameter
// CHECK-NEXT:                Decl %d.18 'arguments' Var Arguments
// CHECK-NEXT:        Func strict noImplicitReturn
// CHECK-NEXT:            Scope %s.6
// CHECK-NEXT:                Decl %d.19 'y' Parameter : number
// CHECK-NEXT:                Decl %d.20 'arguments' Var Arguments
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.7
// CHECK-NEXT:            Decl %d.21 'x' Parameter : string
// CHECK-NEXT:            Decl %d.22 'innerVar' Var : number
// CHECK-NEXT:            Decl %d.23 'inner' Var
// CHECK-NEXT:            Decl %d.24 'arguments' Var Arguments
// CHECK-NEXT:            Decl %d.25 'inner' Var : %function.3
// CHECK-NEXT:            hoistedFunction inner
// CHECK-NEXT:            hoistedFunction inner
// CHECK-NEXT:        Func strict noImplicitReturn
// CHECK-NEXT:            Scope %s.8
// CHECK-NEXT:                Decl %d.26 'y' Parameter
// CHECK-NEXT:                Decl %d.27 'arguments' Var Arguments
// CHECK-NEXT:        Func strict noImplicitReturn
// CHECK-NEXT:            Scope %s.9
// CHECK-NEXT:                Decl %d.28 'y' Parameter : string
// CHECK-NEXT:                Decl %d.29 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        FunctionDeclaration : %function.2
// CHECK-NEXT:            Id 'outer' [D:E:%d.4 'outer']
// CHECK-NEXT:            Id 'x' [D:E:%d.12 'x']
// CHECK-NEXT:            BlockStatement
// CHECK-NEXT:                VariableDeclaration
// CHECK-NEXT:                    VariableDeclarator
// CHECK-NEXT:                        NumericLiteral : 3
// CHECK-NEXT:                        Id 'innerVar' [D:E:%d.13 'innerVar']
// CHECK-NEXT:                FunctionDeclaration : %function.2
// CHECK-NEXT:                    Id 'inner' [D:E:%d.16 'inner']
// CHECK-NEXT:                    Id 'y' [D:E:%d.19 'y']
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'y' [D:E:%d.19 'y'] : number
// CHECK-NEXT:                    TypeParameterDeclaration
// CHECK-NEXT:                        TypeParameter
// CHECK-NEXT:                FunctionDeclaration
// CHECK-NEXT:                    Id 'inner' [D:E:%d.14 'inner']
// CHECK-NEXT:                    Id 'y' [D:E:%d.17 'y']
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'y' [D:E:%d.17 'y']
// CHECK-NEXT:                    TypeParameterDeclaration
// CHECK-NEXT:                        TypeParameter
// CHECK-NEXT:                ReturnStatement
// CHECK-NEXT:                    CallExpression : number
// CHECK-NEXT:                        Id 'inner' [D:E:%d.16 'inner'] : %function.2
// CHECK-NEXT:                        TypeParameterInstantiation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'T'
// CHECK-NEXT:                        Id 'x' [D:E:%d.12 'x'] : number
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:        FunctionDeclaration : %function.3
// CHECK-NEXT:            Id 'outer' [D:E:%d.5 'outer']
// CHECK-NEXT:            Id 'x' [D:E:%d.21 'x']
// CHECK-NEXT:            BlockStatement
// CHECK-NEXT:                VariableDeclaration
// CHECK-NEXT:                    VariableDeclarator
// CHECK-NEXT:                        NumericLiteral : 3
// CHECK-NEXT:                        Id 'innerVar' [D:E:%d.22 'innerVar']
// CHECK-NEXT:                FunctionDeclaration : %function.3
// CHECK-NEXT:                    Id 'inner' [D:E:%d.25 'inner']
// CHECK-NEXT:                    Id 'y' [D:E:%d.28 'y']
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'y' [D:E:%d.28 'y'] : string
// CHECK-NEXT:                    TypeParameterDeclaration
// CHECK-NEXT:                        TypeParameter
// CHECK-NEXT:                FunctionDeclaration
// CHECK-NEXT:                    Id 'inner' [D:E:%d.23 'inner']
// CHECK-NEXT:                    Id 'y' [D:E:%d.26 'y']
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'y' [D:E:%d.26 'y']
// CHECK-NEXT:                    TypeParameterDeclaration
// CHECK-NEXT:                        TypeParameter
// CHECK-NEXT:                ReturnStatement
// CHECK-NEXT:                    CallExpression : string
// CHECK-NEXT:                        Id 'inner' [D:E:%d.25 'inner'] : %function.3
// CHECK-NEXT:                        TypeParameterInstantiation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'T'
// CHECK-NEXT:                        Id 'x' [D:E:%d.21 'x'] : string
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:        FunctionDeclaration
// CHECK-NEXT:            Id 'outer' [D:E:%d.2 'outer']
// CHECK-NEXT:            Id 'x' [D:E:%d.6 'x']
// CHECK-NEXT:            BlockStatement
// CHECK-NEXT:                VariableDeclaration
// CHECK-NEXT:                    VariableDeclarator
// CHECK-NEXT:                        NumericLiteral
// CHECK-NEXT:                        Id 'innerVar' [D:E:%d.7 'innerVar']
// CHECK-NEXT:                FunctionDeclaration
// CHECK-NEXT:                    Id 'inner' [D:E:%d.8 'inner']
// CHECK-NEXT:                    Id 'y' [D:E:%d.10 'y']
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 'y' [D:E:%d.10 'y']
// CHECK-NEXT:                    TypeParameterDeclaration
// CHECK-NEXT:                        TypeParameter
// CHECK-NEXT:                ReturnStatement
// CHECK-NEXT:                    CallExpression
// CHECK-NEXT:                        Id 'inner' [D:E:%d.8 'inner']
// CHECK-NEXT:                        TypeParameterInstantiation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'T'
// CHECK-NEXT:                        Id 'x' [D:E:%d.6 'x']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            CallExpression : number
// CHECK-NEXT:                Id 'outer' [D:E:%d.4 'outer'] : %function.2
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    NumberTypeAnnotation
// CHECK-NEXT:                NumericLiteral : 1
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            CallExpression : string
// CHECK-NEXT:                Id 'outer' [D:E:%d.5 'outer'] : %function.3
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    StringTypeAnnotation
// CHECK-NEXT:                StringLiteral : "a"
