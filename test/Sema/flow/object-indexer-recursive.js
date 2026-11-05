/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// Recursive unions reached through object indexers.

type R1 = number | {f: R1};
type R2 = string | {f: R2};
type V = {[k: string]: R1} | {[k: string]: R2};
let v: ?V = null;

type T1 = {[k: string]: ?T1} | {[k: string]: ?T2};
type T2 = number | {f: T2};
let t1: ?T1 = null;

type D = number | {[k: string]: D};
let d: D = 1;
let dObj: {[k: string]: D} = {};
d = dObj;

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%union.2 = union(number | %object.5)
// CHECK-NEXT:%union.3 = union(void | null | %object.7 | %object.8)
// CHECK-NEXT:%union.4 = union(void | null | %object.9 | %object.10)
// CHECK-NEXT:%object.5 = object({
// CHECK-NEXT:  [string]: %union.2
// CHECK-NEXT:})
// CHECK-NEXT:%object.6 = object({
// CHECK-NEXT:})
// CHECK-NEXT:%object.7 = object({
// CHECK-NEXT:  [string]: %union.11
// CHECK-NEXT:})
// CHECK-NEXT:%object.8 = object({
// CHECK-NEXT:  [string]: %union.12
// CHECK-NEXT:})
// CHECK-NEXT:%object.9 = object({
// CHECK-NEXT:  [string]: %union.4
// CHECK-NEXT:})
// CHECK-NEXT:%object.10 = object({
// CHECK-NEXT:  [string]: %union.13
// CHECK-NEXT:})
// CHECK-NEXT:%union.11 = union(number | %object.14)
// CHECK-NEXT:%union.12 = union(string | %object.15)
// CHECK-NEXT:%union.13 = union(void | null | number | %object.14)
// CHECK-NEXT:%object.14 = object({
// CHECK-NEXT:  f: %union.11
// CHECK-NEXT:})
// CHECK-NEXT:%object.15 = object({
// CHECK-NEXT:  f: %union.12
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'v' Let : %union.3
// CHECK-NEXT:        Decl %d.3 't1' Let : %union.4
// CHECK-NEXT:        Decl %d.4 'd' Let : %union.2
// CHECK-NEXT:        Decl %d.5 'dObj' Let : %object.5
// CHECK-NEXT:        Decl %d.6 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'R1'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'f'
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'R1'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'R2'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                StringTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'f'
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'R2'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'V'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeIndexer
// CHECK-NEXT:                        Id 'k'
// CHECK-NEXT:                        StringTypeAnnotation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'R1'
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeIndexer
// CHECK-NEXT:                        Id 'k'
// CHECK-NEXT:                        StringTypeAnnotation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'R2'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NullLiteral : null
// CHECK-NEXT:                Id 'v' [D:E:%d.2 'v']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T1'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeIndexer
// CHECK-NEXT:                        Id 'k'
// CHECK-NEXT:                        StringTypeAnnotation
// CHECK-NEXT:                        NullableTypeAnnotation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'T1'
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeIndexer
// CHECK-NEXT:                        Id 'k'
// CHECK-NEXT:                        StringTypeAnnotation
// CHECK-NEXT:                        NullableTypeAnnotation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'T2'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T2'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'f'
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'T2'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NullLiteral : null
// CHECK-NEXT:                Id 't1' [D:E:%d.3 't1']
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'D'
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeIndexer
// CHECK-NEXT:                        Id 'k'
// CHECK-NEXT:                        StringTypeAnnotation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'D'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                NumericLiteral : 1
// CHECK-NEXT:                Id 'd' [D:E:%d.4 'd']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.6
// CHECK-NEXT:                Id 'dObj' [D:E:%d.5 'dObj']
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : %object.5
// CHECK-NEXT:                Id 'd' [D:E:%d.4 'd'] : %union.2
// CHECK-NEXT:                Id 'dObj' [D:E:%d.5 'dObj'] : %object.5
