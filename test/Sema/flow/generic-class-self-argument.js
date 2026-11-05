/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// Generic class specializations whose type argument is the specialization
// itself must not be specialized twice.

class C<T> {
  x: ?T = null;
  m(): C<T> {
    return this;
  }
}
type C1 = C<C2>;
type C2 = C<C2>;
let c: C1 = new C<C2>();

type A1 = ?(A2[]);
type A2 = A2[];
let a1: A1 = null;
let a2: A2 = [];
a1 = a2;

type B1 = B2[];
type B2 = B3[];
type B3 = B2[];
let b1: B1 = [];
let b3: B3 = b1;

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%class.2 = class(C {
// CHECK-NEXT:  %homeObject: %class.9
// CHECK-NEXT:  x: %union.7
// CHECK-NEXT:})
// CHECK-NEXT:%union.3 = union(void | null | %class.4)
// CHECK-NEXT:%class.4 = class(Array<%class.4>)
// CHECK-NEXT:%class.5 = class(Array<%class.10>)
// CHECK-NEXT:%class_constructor.6 = class_constructor(%class.2)
// CHECK-NEXT:%union.7 = union(void | null | %class.2)
// CHECK-NEXT:%function.8 = function(this: %class.2): %class.2
// CHECK-NEXT:%class.9 = class( {
// CHECK-NEXT:  m [final]: %function.8
// CHECK-NEXT:})
// CHECK-NEXT:%class.10 = class(Array<%class.5>)

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'C' Class
// CHECK-NEXT:        Decl %d.3 'c' Let : %class.2
// CHECK-NEXT:        Decl %d.4 'a1' Let : %union.3
// CHECK-NEXT:        Decl %d.5 'a2' Let : %class.4
// CHECK-NEXT:        Decl %d.6 'b1' Let : %class.5
// CHECK-NEXT:        Decl %d.7 'b3' Let : %class.5
// CHECK-NEXT:        Decl %d.8 'arguments' Var Arguments
// CHECK-NEXT:        Decl %d.9 'C' Class : %class_constructor.6
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:        Scope %s.3
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.4
// CHECK-NEXT:            Decl %d.10 'arguments' Var Arguments
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.5
// CHECK-NEXT:            Decl %d.11 'arguments' Var Arguments
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.6
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.7
// CHECK-NEXT:            Decl %d.12 'arguments' Var Arguments
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.8
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.9
// CHECK-NEXT:            Decl %d.13 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        ClassDeclaration Scope %s.3
// CHECK-NEXT:            Id 'C' [D:E:%d.9 'C']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:                ClassProperty : %union.7
// CHECK-NEXT:                    Id 'x'
// CHECK-NEXT:                    NullLiteral : null
// CHECK-NEXT:                MethodDefinition : %function.8
// CHECK-NEXT:                    Id 'm'
// CHECK-NEXT:                    FunctionExpression : %function.8
// CHECK-NEXT:                        BlockStatement
// CHECK-NEXT:                            ReturnStatement
// CHECK-NEXT:                                ThisExpression : %class.2
// CHECK-NEXT:        ClassDeclaration Scope %s.2
// CHECK-NEXT:            Id 'C' [D:E:%d.2 'C']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:                ClassProperty
// CHECK-NEXT:                    Id 'x'
// CHECK-NEXT:                    NullLiteral
// CHECK-NEXT:                MethodDefinition
// CHECK-NEXT:                    Id 'm'
// CHECK-NEXT:                    FunctionExpression
// CHECK-NEXT:                        BlockStatement
// CHECK-NEXT:                            ReturnStatement
// CHECK-NEXT:                                ThisExpression
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'C1'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'C'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'C2'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'C2'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'C'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'C2'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NewExpression : %class.2
// CHECK-NEXT:                    Id 'C' [D:E:%d.9 'C'] : %class_constructor.6
// CHECK-NEXT:                    TypeParameterInstantiation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'C2'
// CHECK-NEXT:                Id 'c' [D:E:%d.3 'c']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'A1'
// CHECK-NEXT:            NullableTypeAnnotation
// CHECK-NEXT:                ArrayTypeAnnotation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'A2'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'A2'
// CHECK-NEXT:            ArrayTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'A2'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NullLiteral : null
// CHECK-NEXT:                Id 'a1' [D:E:%d.4 'a1']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrayExpression : %class.4
// CHECK-NEXT:                Id 'a2' [D:E:%d.5 'a2']
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : %class.4
// CHECK-NEXT:                Id 'a1' [D:E:%d.4 'a1'] : %union.3
// CHECK-NEXT:                Id 'a2' [D:E:%d.5 'a2'] : %class.4
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'B1'
// CHECK-NEXT:            ArrayTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'B2'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'B2'
// CHECK-NEXT:            ArrayTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'B3'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'B3'
// CHECK-NEXT:            ArrayTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'B2'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrayExpression : %class.5
// CHECK-NEXT:                Id 'b1' [D:E:%d.6 'b1']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'b1' [D:E:%d.6 'b1'] : %class.5
// CHECK-NEXT:                Id 'b3' [D:E:%d.7 'b3']
