/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// Recursive unions where a union arm is still being completed when the cycle
// is found.

type U1 = number | {f: ?U1};
let u1: U1 = 1;

type L = null | {next: L | number};
let l: L = null;

type U2 = number | [U2 | string];
let u2: U2 = 1;

type U3 = number | (U3 | string)[];
let u3: U3 = 1;
let u3a: (U3 | string)[] = [];
u3 = u3a;

type U4 = ?{f: ?U4};
let u4: U4 = null;

type T5 = {f: ?U5};
type U5 = number | T5;
let u5: U5 = 1;

// The union containing the cycle start is completed after it.
type W = null | X;
type X = number | Y;
type Y = string | {f: Z};
type Z = boolean | X;
let w: W = "s";
let z: Z = true;
let x: X = 1;
w = x;
z = x;

// A cycle is found at T1 before its other arms are visited.
type T0 = (number | T1) | [T4];
type T1 = [{f: T0}] | {f: T0} | {f: T4};
type T4 = number | {f: {f: T1}};
let t0: T0 = 1;

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%union.2 = union(number | %object.12)
// CHECK-NEXT:%union.3 = union(null | %object.13)
// CHECK-NEXT:%union.4 = union(number | %tuple.14)
// CHECK-NEXT:%union.5 = union(number | %class.11)
// CHECK-NEXT:%union.6 = union(void | null | %object.15)
// CHECK-NEXT:%union.7 = union(null | string | number | %object.16)
// CHECK-NEXT:%union.8 = union(string | number | %object.16)
// CHECK-NEXT:%union.9 = union(boolean | string | number | %object.16)
// CHECK-NEXT:%union.10 = union(number | %tuple.17 | %object.18 | %object.19 | %tuple.20)
// CHECK-NEXT:%class.11 = class(Array<%union.21>)
// CHECK-NEXT:%object.12 = object({
// CHECK-NEXT:  f: %union.22
// CHECK-NEXT:})
// CHECK-NEXT:%object.13 = object({
// CHECK-NEXT:  next: %union.23
// CHECK-NEXT:})
// CHECK-NEXT:%tuple.14 = tuple(%union.24)
// CHECK-NEXT:%object.15 = object({
// CHECK-NEXT:  f: %union.6
// CHECK-NEXT:})
// CHECK-NEXT:%object.16 = object({
// CHECK-NEXT:  f: %union.9
// CHECK-NEXT:})
// CHECK-NEXT:%tuple.17 = tuple(%object.18)
// CHECK-NEXT:%object.18 = object({
// CHECK-NEXT:  f: %union.10
// CHECK-NEXT:})
// CHECK-NEXT:%object.19 = object({
// CHECK-NEXT:  f: %union.25
// CHECK-NEXT:})
// CHECK-NEXT:%tuple.20 = tuple(%union.25)
// CHECK-NEXT:%union.21 = union(string | number | %class.11)
// CHECK-NEXT:%union.22 = union(void | null | number | %object.12)
// CHECK-NEXT:%union.23 = union(null | number | %object.13)
// CHECK-NEXT:%union.24 = union(string | number | %tuple.14)
// CHECK-NEXT:%union.25 = union(number | %object.26)
// CHECK-NEXT:%object.26 = object({
// CHECK-NEXT:  f: %object.27
// CHECK-NEXT:})
// CHECK-NEXT:%object.27 = object({
// CHECK-NEXT:  f: %union.28
// CHECK-NEXT:})
// CHECK-NEXT:%union.28 = union(%tuple.17 | %object.18 | %object.19)

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'u1' Let : %union.2
// CHECK-NEXT:        Decl %d.3 'l' Let : %union.3
// CHECK-NEXT:        Decl %d.4 'u2' Let : %union.4
// CHECK-NEXT:        Decl %d.5 'u3' Let : %union.5
// CHECK-NEXT:        Decl %d.6 'u3a' Let : %class.11
// CHECK-NEXT:        Decl %d.7 'u4' Let : %union.6
// CHECK-NEXT:        Decl %d.8 'u5' Let : %union.2
// CHECK-NEXT:        Decl %d.9 'w' Let : %union.7
// CHECK-NEXT:        Decl %d.10 'z' Let : %union.9
// CHECK-NEXT:        Decl %d.11 'x' Let : %union.8
// CHECK-NEXT:        Decl %d.12 't0' Let : %union.10
// CHECK-NEXT:        Decl %d.13 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'U1'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'f'
// CHECK-NEXT:                        NullableTypeAnnotation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'U1'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NumericLiteral : 1
// CHECK-NEXT:                Id 'u1' [D:E:%d.2 'u1']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'L'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NullLiteralTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'next'
// CHECK-NEXT:                        UnionTypeAnnotation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'L'
// CHECK-NEXT:                            NumberTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NullLiteral : null
// CHECK-NEXT:                Id 'l' [D:E:%d.3 'l']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'U2'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:                TupleTypeAnnotation
// CHECK-NEXT:                    UnionTypeAnnotation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'U2'
// CHECK-NEXT:                        StringTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NumericLiteral : 1
// CHECK-NEXT:                Id 'u2' [D:E:%d.4 'u2']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'U3'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:                ArrayTypeAnnotation
// CHECK-NEXT:                    UnionTypeAnnotation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'U3'
// CHECK-NEXT:                        StringTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NumericLiteral : 1
// CHECK-NEXT:                Id 'u3' [D:E:%d.5 'u3']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrayExpression : %class.11
// CHECK-NEXT:                Id 'u3a' [D:E:%d.6 'u3a']
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : %class.11
// CHECK-NEXT:                Id 'u3' [D:E:%d.5 'u3'] : %union.5
// CHECK-NEXT:                Id 'u3a' [D:E:%d.6 'u3a'] : %class.11
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'U4'
// CHECK-NEXT:            NullableTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'f'
// CHECK-NEXT:                        NullableTypeAnnotation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'U4'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NullLiteral : null
// CHECK-NEXT:                Id 'u4' [D:E:%d.7 'u4']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T5'
// CHECK-NEXT:            ObjectTypeAnnotation
// CHECK-NEXT:                ObjectTypeProperty
// CHECK-NEXT:                    Id 'f'
// CHECK-NEXT:                    NullableTypeAnnotation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'U5'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'U5'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'T5'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NumericLiteral : 1
// CHECK-NEXT:                Id 'u5' [D:E:%d.8 'u5']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'W'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NullLiteralTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'X'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'X'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'Y'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'Y'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                StringTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'f'
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'Z'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'Z'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                BooleanTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'X'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                StringLiteral : "s"
// CHECK-NEXT:                Id 'w' [D:E:%d.9 'w']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                BooleanLiteral : true
// CHECK-NEXT:                Id 'z' [D:E:%d.10 'z']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NumericLiteral : 1
// CHECK-NEXT:                Id 'x' [D:E:%d.11 'x']
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : %union.8
// CHECK-NEXT:                Id 'w' [D:E:%d.9 'w'] : %union.7
// CHECK-NEXT:                Id 'x' [D:E:%d.11 'x'] : %union.8
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : %union.8
// CHECK-NEXT:                Id 'z' [D:E:%d.10 'z'] : %union.9
// CHECK-NEXT:                Id 'x' [D:E:%d.11 'x'] : %union.8
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T0'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                UnionTypeAnnotation
// CHECK-NEXT:                    NumberTypeAnnotation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'T1'
// CHECK-NEXT:                TupleTypeAnnotation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'T4'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T1'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                TupleTypeAnnotation
// CHECK-NEXT:                    ObjectTypeAnnotation
// CHECK-NEXT:                        ObjectTypeProperty
// CHECK-NEXT:                            Id 'f'
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'T0'
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'f'
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'T0'
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'f'
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'T4'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T4'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'f'
// CHECK-NEXT:                        ObjectTypeAnnotation
// CHECK-NEXT:                            ObjectTypeProperty
// CHECK-NEXT:                                Id 'f'
// CHECK-NEXT:                                GenericTypeAnnotation
// CHECK-NEXT:                                    Id 'T1'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NumericLiteral : 1
// CHECK-NEXT:                Id 't0' [D:E:%d.12 't0']
