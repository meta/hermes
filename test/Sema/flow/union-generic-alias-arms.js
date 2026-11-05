/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// A cycle is found at T0 through Array before its generic alias arms are
// expanded. The arms must not be deduplicated as equal.

type G<X> = X | number;
type H<X> = X | number;
type T0 = ?(T0[] | G<boolean> | H<string>);
let t: T0 = "s";
t = true;

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%union.2 = union(void | null | boolean | string | number | %class.3)
// CHECK-NEXT:%class.3 = class(Array<%union.2>)

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 't' Let : %union.2
// CHECK-NEXT:        Decl %d.3 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'G'
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'X'
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'H'
// CHECK-NEXT:            TypeParameterDeclaration
// CHECK-NEXT:                TypeParameter
// CHECK-NEXT:            UnionTypeAnnotation
// CHECK-NEXT:                GenericTypeAnnotation
// CHECK-NEXT:                    Id 'X'
// CHECK-NEXT:                NumberTypeAnnotation
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'T0'
// CHECK-NEXT:            NullableTypeAnnotation
// CHECK-NEXT:                UnionTypeAnnotation
// CHECK-NEXT:                    ArrayTypeAnnotation
// CHECK-NEXT:                        GenericTypeAnnotation
// CHECK-NEXT:                            Id 'T0'
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'G'
// CHECK-NEXT:                        TypeParameterInstantiation
// CHECK-NEXT:                            BooleanTypeAnnotation
// CHECK-NEXT:                    GenericTypeAnnotation
// CHECK-NEXT:                        Id 'H'
// CHECK-NEXT:                        TypeParameterInstantiation
// CHECK-NEXT:                            StringTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                StringLiteral : "s"
// CHECK-NEXT:                Id 't' [D:E:%d.2 't']
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : true
// CHECK-NEXT:                Id 't' [D:E:%d.2 't'] : %union.2
// CHECK-NEXT:                BooleanLiteral : true
