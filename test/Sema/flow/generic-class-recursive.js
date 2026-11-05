/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

class C<T> {}
class D<T> {}
type Direct = C<Direct>;
let direct: Direct;
type DirectAgain = C<Direct>;
let directAgain: DirectAgain = direct;
let directRoundTrip: Direct = directAgain;

type W1 = D<Direct>;
type W2 = D<C<Direct>>;
let a: W1;
let b: W2;
a = b;

type Nullable = C<Nullable> | null;
let nullable: Nullable;

type JsonArray = JsonArray[] | null;
let jsonArray: JsonArray;

type JsonGenericArray = Array<JsonGenericArray> | null;
let jsonGenericArray: JsonGenericArray;

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%class.2 = class(C {
// CHECK-NEXT:  %homeObject: %class.10
// CHECK-NEXT:})
// CHECK-NEXT:%class.3 = class(D {
// CHECK-NEXT:  %homeObject: %class.11
// CHECK-NEXT:})
// CHECK-NEXT:%union.4 = union(null | %class.12)
// CHECK-NEXT:%union.5 = union(null | %class.13)
// CHECK-NEXT:%union.6 = union(null | %class.14)
// CHECK-NEXT:%class_constructor.7 = class_constructor(%class.12)
// CHECK-NEXT:%class_constructor.8 = class_constructor(%class.2)
// CHECK-NEXT:%class_constructor.9 = class_constructor(%class.3)
// CHECK-NEXT:%class.10 = class( {
// CHECK-NEXT:})
// CHECK-NEXT:%class.11 = class( {
// CHECK-NEXT:})
// CHECK-NEXT:%class.12 = class(C {
// CHECK-NEXT:  %homeObject: %class.15
// CHECK-NEXT:})
// CHECK-NEXT:%class.13 = class(Array<%union.5>)
// CHECK-NEXT:%class.14 = class(Array<%union.6>)
// CHECK-NEXT:%class.15 = class( {
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'C' Class
// CHECK-NEXT:        Decl %d.3 'D' Class
// CHECK-NEXT:        Decl %d.4 'direct' Let : %class.2
// CHECK-NEXT:        Decl %d.5 'directAgain' Let : %class.2
// CHECK-NEXT:        Decl %d.6 'directRoundTrip' Let : %class.2
// CHECK-NEXT:        Decl %d.7 'a' Let : %class.3
// CHECK-NEXT:        Decl %d.8 'b' Let : %class.3
// CHECK-NEXT:        Decl %d.9 'nullable' Let : %union.4
// CHECK-NEXT:        Decl %d.10 'jsonArray' Let : %union.5
// CHECK-NEXT:        Decl %d.11 'jsonGenericArray' Let : %union.6
// CHECK-NEXT:        Decl %d.12 'arguments' Var Arguments
// CHECK-NEXT:        Decl %d.13 'C' Class : %class_constructor.7
// CHECK-NEXT:        Decl %d.14 'C' Class : %class_constructor.8
// CHECK-NEXT:        Decl %d.15 'D' Class : %class_constructor.9
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:        Scope %s.3
// CHECK-NEXT:        Scope %s.4
// CHECK-NEXT:        Scope %s.5
// CHECK-NEXT:        Scope %s.6
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.7
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.8
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.9
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.10
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.11

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        ClassDeclaration Scope %s.4
// CHECK-NEXT:            Id 'C' [D:E:%d.13 'C']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.5
// CHECK-NEXT:            Id 'C' [D:E:%d.14 'C']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.2
// CHECK-NEXT:            Id 'C' [D:E:%d.2 'C']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.6
// CHECK-NEXT:            Id 'D' [D:E:%d.15 'D']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.3
// CHECK-NEXT:            Id 'D' [D:E:%d.3 'D']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'Direct'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'C'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'Direct'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'direct' [D:E:%d.4 'direct']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'DirectAgain'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'C'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'Direct'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'direct' [D:E:%d.4 'direct'] : %class.2
// CHECK-NEXT:                Id 'directAgain' [D:E:%d.5 'directAgain']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'directAgain' [D:E:%d.5 'directAgain'] : %class.2
// CHECK-NEXT:                Id 'directRoundTrip' [D:E:%d.6 'directRoundTrip']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'W1'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'D'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'Direct'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'W2'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'D'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'C'
// CHECK-NEXT:                        TypeParameterInstantiation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'Direct'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'a' [D:E:%d.7 'a']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'b' [D:E:%d.8 'b']
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : %class.3
// CHECK-NEXT:                Id 'a' [D:E:%d.7 'a'] : %class.3
// CHECK-NEXT:                Id 'b' [D:E:%d.8 'b'] : %class.3
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'Nullable'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'C'
// CHECK-NEXT:                    TypeParameterInstantiation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'Nullable'
// CHECK-NEXT:                NullLiteralTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'nullable' [D:E:%d.9 'nullable']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'JsonArray'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                ArrayTypeAnnotation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'JsonArray'
// CHECK-NEXT:                NullLiteralTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'jsonArray' [D:E:%d.10 'jsonArray']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'JsonGenericArray'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'Array'
// CHECK-NEXT:                    TypeParameterInstantiation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'JsonGenericArray'
// CHECK-NEXT:                NullLiteralTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 'jsonGenericArray' [D:E:%d.11 'jsonGenericArray']
