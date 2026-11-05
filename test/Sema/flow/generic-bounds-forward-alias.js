/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// Generic class bounds referring to type aliases declared after the class,
// specialized while the aliases are still being resolved.

class A<T: U[]> {}
type U = number[];
type XA = A<number[][]>;

class B<T: ?V> {}
type V = number[];
type XB = B<null>;
type XB2 = B<number[]>;

class C<T: ?W> {}
type W = string | C<string>;
let w: W = "s";

// A bound referring to its own generic is resolved once the specialization
// exists.
class Cmp<T: Cmp<T>> {}
class Foo extends Cmp<Foo> {}
let foo: Cmp<Foo> = new Foo();

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%union.2 = union(string | %class.11)
// CHECK-NEXT:%class.3 = class(Foo extends %class.9 {
// CHECK-NEXT:  %homeObject: %class.12
// CHECK-NEXT:})
// CHECK-NEXT:%class_constructor.4 = class_constructor(%class.3)
// CHECK-NEXT:%class_constructor.5 = class_constructor(%class.11)
// CHECK-NEXT:%class_constructor.6 = class_constructor(%class.13)
// CHECK-NEXT:%class_constructor.7 = class_constructor(%class.14)
// CHECK-NEXT:%class_constructor.8 = class_constructor(%class.15)
// CHECK-NEXT:%class.9 = class(Cmp {
// CHECK-NEXT:  %homeObject: %class.16
// CHECK-NEXT:})
// CHECK-NEXT:%class_constructor.10 = class_constructor(%class.9)
// CHECK-NEXT:%class.11 = class(C {
// CHECK-NEXT:  %homeObject: %class.17
// CHECK-NEXT:})
// CHECK-NEXT:%class.12 = class( extends %class.16 {
// CHECK-NEXT:})
// CHECK-NEXT:%class.13 = class(A {
// CHECK-NEXT:  %homeObject: %class.18
// CHECK-NEXT:})
// CHECK-NEXT:%class.14 = class(B {
// CHECK-NEXT:  %homeObject: %class.19
// CHECK-NEXT:})
// CHECK-NEXT:%class.15 = class(B {
// CHECK-NEXT:  %homeObject: %class.20
// CHECK-NEXT:})
// CHECK-NEXT:%class.16 = class( {
// CHECK-NEXT:})
// CHECK-NEXT:%class.17 = class( {
// CHECK-NEXT:})
// CHECK-NEXT:%class.18 = class( {
// CHECK-NEXT:})
// CHECK-NEXT:%class.19 = class( {
// CHECK-NEXT:})
// CHECK-NEXT:%class.20 = class( {
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'A' Class
// CHECK-NEXT:        Decl %d.3 'B' Class
// CHECK-NEXT:        Decl %d.4 'C' Class
// CHECK-NEXT:        Decl %d.5 'w' Let : %union.2
// CHECK-NEXT:        Decl %d.6 'Cmp' Class
// CHECK-NEXT:        Decl %d.7 'Foo' Class : %class_constructor.4
// CHECK-NEXT:        Decl %d.8 'foo' Let : %class.9
// CHECK-NEXT:        Decl %d.9 'arguments' Var Arguments
// CHECK-NEXT:        Decl %d.10 'C' Class : %class_constructor.5
// CHECK-NEXT:        Decl %d.11 'A' Class : %class_constructor.6
// CHECK-NEXT:        Decl %d.12 'B' Class : %class_constructor.7
// CHECK-NEXT:        Decl %d.13 'B' Class : %class_constructor.8
// CHECK-NEXT:        Decl %d.14 'Cmp' Class : %class_constructor.10
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:        Scope %s.3
// CHECK-NEXT:        Scope %s.4
// CHECK-NEXT:        Scope %s.5
// CHECK-NEXT:        Scope %s.6
// CHECK-NEXT:        Scope %s.7
// CHECK-NEXT:        Scope %s.8
// CHECK-NEXT:        Scope %s.9
// CHECK-NEXT:        Scope %s.10
// CHECK-NEXT:        Scope %s.11
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.12
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.13
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.14
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.15
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.16
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.17
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.18
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.19
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.20
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.21

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        ClassDeclaration Scope %s.8
// CHECK-NEXT:            Id 'A' [D:E:%d.11 'A']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.2
// CHECK-NEXT:            Id 'A' [D:E:%d.2 'A']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'U'
// CHECK-NEXT:            ArrayTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'XA'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'A'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    ArrayTypeAnnotation
// CHECK-NEXT:                        ArrayTypeAnnotation
// CHECK-NEXT:                            NumberTypeAnnotation
// CHECK-NEXT:        ClassDeclaration Scope %s.9
// CHECK-NEXT:            Id 'B' [D:E:%d.12 'B']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.10
// CHECK-NEXT:            Id 'B' [D:E:%d.13 'B']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.3
// CHECK-NEXT:            Id 'B' [D:E:%d.3 'B']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'V'
// CHECK-NEXT:            ArrayTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'XB'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'B'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    NullLiteralTypeAnnotation
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'XB2'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'B'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    ArrayTypeAnnotation
// CHECK-NEXT:                        NumberTypeAnnotation
// CHECK-NEXT:        ClassDeclaration Scope %s.7
// CHECK-NEXT:            Id 'C' [D:E:%d.10 'C']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.4
// CHECK-NEXT:            Id 'C' [D:E:%d.4 'C']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'W'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                StringTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'C'
// CHECK-NEXT:                    TypeParameterInstantiation
// CHECK-NEXT:                        StringTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                StringLiteral : "s"
// CHECK-NEXT:                Id 'w' [D:E:%d.5 'w']
// CHECK-NEXT:        ClassDeclaration Scope %s.11
// CHECK-NEXT:            Id 'Cmp' [D:E:%d.14 'Cmp']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.5
// CHECK-NEXT:            Id 'Cmp' [D:E:%d.6 'Cmp']
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        ClassDeclaration Scope %s.6
// CHECK-NEXT:            Id 'Foo' [D:E:%d.7 'Foo']
// CHECK-NEXT:            Id 'Cmp' [D:E:%d.14 'Cmp'] : %class_constructor.10
// CHECK-NEXT:            TypeParameterInstantiation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'Foo'
// CHECK-NEXT:            ClassBody
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NewExpression : %class.3
// CHECK-NEXT:                    Id 'Foo' [D:E:%d.7 'Foo'] : %class_constructor.4
// CHECK-NEXT:                Id 'foo' [D:E:%d.8 'foo']
