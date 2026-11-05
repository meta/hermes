/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// T1 copies the union from D<T0[]> before it collapses to a single type.

type D<T> = T | T;
type H<T> = null | {h: T};
type T0 = T1;
type T1 = T2;
type T2 = D<T0[]>;
type T3 = (x: H<T1>) => ?T2;
let t0: T0 = [];
let t2: ?T2 = t0;
let t3: T3 = (x: H<T1>): ?T2 => t2;

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%class.2 = class(Array<%class.2>)
// CHECK-NEXT:%function.3 = function(x: %union.4): %union.5
// CHECK-NEXT:%union.4 = union(null | %object.6)
// CHECK-NEXT:%union.5 = union(void | null | %class.2)
// CHECK-NEXT:%object.6 = object({
// CHECK-NEXT:  h: %class.2
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 't0' Let : %class.2
// CHECK-NEXT:        Decl %d.3 't2' Let : %union.5
// CHECK-NEXT:        Decl %d.4 't3' Let : %function.3
// CHECK-NEXT:        Decl %d.5 'arguments' Var Arguments
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:            Decl %d.6 'x' Parameter : %union.4

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'D'
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'T'
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'T'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'H'
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                NullLiteralTypeAnnotation
// CHECK-NEXT:                ObjectTypeAnnotation
// CHECK-NEXT:                    ObjectTypeProperty
// CHECK-NEXT:                        Id 'h'
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'T'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T0'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'T1'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T1'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'T2'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T2'
// CHECK-NEXT:            GenericTypeAnnotation
// CHECK-NEXT:                Id 'D'
// CHECK-NEXT:                TypeParameterInstantiation
// CHECK-NEXT:                    ArrayTypeAnnotation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'T0'
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T3'
// CHECK-NEXT:            FunctionTypeAnnotation
// CHECK-NEXT:                FunctionTypeParam
// CHECK-NEXT:                    Id 'x'
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'H'
// CHECK-NEXT:                        TypeParameterInstantiation
// CHECK-NEXT:                            GenericTypeAnnotation
// CHECK-NEXT:                                Id 'T1'
// CHECK-NEXT:                NullableTypeAnnotation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'T2'
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrayExpression : %class.2
// CHECK-NEXT:                Id 't0' [D:E:%d.2 't0']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                Id 't0' [D:E:%d.2 't0'] : %class.2
// CHECK-NEXT:                Id 't2' [D:E:%d.3 't2']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ArrowFunctionExpression : %function.3
// CHECK-NEXT:                    Id 'x' [D:E:%d.6 'x']
// CHECK-NEXT:                    BlockStatement
// CHECK-NEXT:                        ReturnStatement
// CHECK-NEXT:                            Id 't2' [D:E:%d.3 't2'] : %union.5
// CHECK-NEXT:                Id 't3' [D:E:%d.4 't3']
