/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// This should get reasonable flow types for
//   * The call to $SHBuiltin.moduleFactory:
//        type of args[1].
//
//   * The $SHBuiltin.export member expression:
//        (string, any) => void
//   * The call to $SHBuiltin.export:
//        void
//
//   * The $SHBuiltin.import member expression:
//        (number, string, any) => any
//   * The call to $SHBuiltin.import
//        type of args[2], or any if there are two args

// (Note: the (x as any).length expressions are a workaround for a
// current deficiency in static hermes.)

(function() {
    $SHBuiltin.moduleFactory(0, function(global, require, s: string): number {
        return 7 + (s as any).length;
    });
    $SHBuiltin.moduleFactory(1, function(global, require): number {
        var n: number = 100;
        $SHBuiltin.export('n', n);
        return 0;
    });
    $SHBuiltin.moduleFactory(2, function(global, require): number {
        var n2: number = ($SHBuiltin.import(1, 'n', "abc") as any).length + 2;
        return 0;
    });
    $SHBuiltin.moduleFactory(3, function(global, require): number {
        // Import-of-export without the third argument is 'any'.
        $SHBuiltin.export('n3', $SHBuiltin.import(1, 'n'));
        return 0;
    });
})

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%function.2 = function(global: any, require: any, s: string): number
// CHECK-NEXT:%function.3 = function(global: any, require: any): number

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'arguments' Var Arguments
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:            Decl %d.3 'arguments' Var Arguments
// CHECK-NEXT:        Func strict noImplicitReturn
// CHECK-NEXT:            Scope %s.3
// CHECK-NEXT:                Decl %d.4 'global' Parameter : any
// CHECK-NEXT:                Decl %d.5 'require' Parameter : any
// CHECK-NEXT:                Decl %d.6 's' Parameter : string
// CHECK-NEXT:                Decl %d.7 'arguments' Var Arguments
// CHECK-NEXT:        Func strict noImplicitReturn
// CHECK-NEXT:            Scope %s.4
// CHECK-NEXT:                Decl %d.8 'global' Parameter : any
// CHECK-NEXT:                Decl %d.9 'require' Parameter : any
// CHECK-NEXT:                Decl %d.10 'n' Var : number
// CHECK-NEXT:                Decl %d.11 'arguments' Var Arguments
// CHECK-NEXT:        Func strict noImplicitReturn
// CHECK-NEXT:            Scope %s.5
// CHECK-NEXT:                Decl %d.12 'global' Parameter : any
// CHECK-NEXT:                Decl %d.13 'require' Parameter : any
// CHECK-NEXT:                Decl %d.14 'n2' Var : number
// CHECK-NEXT:                Decl %d.15 'arguments' Var Arguments
// CHECK-NEXT:        Func strict noImplicitReturn
// CHECK-NEXT:            Scope %s.6
// CHECK-NEXT:                Decl %d.16 'global' Parameter : any
// CHECK-NEXT:                Decl %d.17 'require' Parameter : any
// CHECK-NEXT:                Decl %d.18 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            FunctionExpression : %untyped_function.1
// CHECK-NEXT:                BlockStatement
// CHECK-NEXT:                    ExpressionStatement
// CHECK-NEXT:                        CallExpression : %function.2
// CHECK-NEXT:                            MemberExpression : any
// CHECK-NEXT:                                SHBuiltin
// CHECK-NEXT:                                Id 'moduleFactory'
// CHECK-NEXT:                            NumericLiteral : 0
// CHECK-NEXT:                            FunctionExpression : %function.2
// CHECK-NEXT:                                Id 'global' [D:E:%d.4 'global']
// CHECK-NEXT:                                Id 'require' [D:E:%d.5 'require']
// CHECK-NEXT:                                Id 's' [D:E:%d.6 's']
// CHECK-NEXT:                                BlockStatement
// CHECK-NEXT:                                    ReturnStatement
// CHECK-NEXT:                                        ImplicitCheckedCast : number
// CHECK-NEXT:                                            BinaryExpression : any
// CHECK-NEXT:                                                NumericLiteral : 7
// CHECK-NEXT:                                                BinOp +
// CHECK-NEXT:                                                MemberExpression : any
// CHECK-NEXT:                                                    AsExpression : any
// CHECK-NEXT:                                                        Id 's' [D:E:%d.6 's'] : string
// CHECK-NEXT:                                                        AnyTypeAnnotation
// CHECK-NEXT:                                                    Id 'length'
// CHECK-NEXT:                    ExpressionStatement
// CHECK-NEXT:                        CallExpression : %function.3
// CHECK-NEXT:                            MemberExpression : any
// CHECK-NEXT:                                SHBuiltin
// CHECK-NEXT:                                Id 'moduleFactory'
// CHECK-NEXT:                            NumericLiteral : 1
// CHECK-NEXT:                            FunctionExpression : %function.3
// CHECK-NEXT:                                Id 'global' [D:E:%d.8 'global']
// CHECK-NEXT:                                Id 'require' [D:E:%d.9 'require']
// CHECK-NEXT:                                BlockStatement
// CHECK-NEXT:                                    VariableDeclaration
// CHECK-NEXT:                                        VariableDeclarator
// CHECK-NEXT:                                            NumericLiteral : 100
// CHECK-NEXT:                                            Id 'n' [D:E:%d.10 'n']
// CHECK-NEXT:                                    ExpressionStatement
// CHECK-NEXT:                                        CallExpression : void
// CHECK-NEXT:                                            MemberExpression : any
// CHECK-NEXT:                                                SHBuiltin
// CHECK-NEXT:                                                Id 'export'
// CHECK-NEXT:                                            StringLiteral : "n"
// CHECK-NEXT:                                            Id 'n' [D:E:%d.10 'n'] : number
// CHECK-NEXT:                                    ReturnStatement
// CHECK-NEXT:                                        NumericLiteral : 0
// CHECK-NEXT:                    ExpressionStatement
// CHECK-NEXT:                        CallExpression : %function.3
// CHECK-NEXT:                            MemberExpression : any
// CHECK-NEXT:                                SHBuiltin
// CHECK-NEXT:                                Id 'moduleFactory'
// CHECK-NEXT:                            NumericLiteral : 2
// CHECK-NEXT:                            FunctionExpression : %function.3
// CHECK-NEXT:                                Id 'global' [D:E:%d.12 'global']
// CHECK-NEXT:                                Id 'require' [D:E:%d.13 'require']
// CHECK-NEXT:                                BlockStatement
// CHECK-NEXT:                                    VariableDeclaration
// CHECK-NEXT:                                        VariableDeclarator
// CHECK-NEXT:                                            ImplicitCheckedCast : number
// CHECK-NEXT:                                                BinaryExpression : any
// CHECK-NEXT:                                                    MemberExpression : any
// CHECK-NEXT:                                                        AsExpression : any
// CHECK-NEXT:                                                            CallExpression : "abc"
// CHECK-NEXT:                                                                MemberExpression : any
// CHECK-NEXT:                                                                    SHBuiltin
// CHECK-NEXT:                                                                    Id 'import'
// CHECK-NEXT:                                                                NumericLiteral : 1
// CHECK-NEXT:                                                                StringLiteral : "n"
// CHECK-NEXT:                                                                StringLiteral : "abc"
// CHECK-NEXT:                                                            AnyTypeAnnotation
// CHECK-NEXT:                                                        Id 'length'
// CHECK-NEXT:                                                    BinOp +
// CHECK-NEXT:                                                    NumericLiteral : 2
// CHECK-NEXT:                                            Id 'n2' [D:E:%d.14 'n2']
// CHECK-NEXT:                                    ReturnStatement
// CHECK-NEXT:                                        NumericLiteral : 0
// CHECK-NEXT:                    ExpressionStatement
// CHECK-NEXT:                        CallExpression : %function.3
// CHECK-NEXT:                            MemberExpression : any
// CHECK-NEXT:                                SHBuiltin
// CHECK-NEXT:                                Id 'moduleFactory'
// CHECK-NEXT:                            NumericLiteral : 3
// CHECK-NEXT:                            FunctionExpression : %function.3
// CHECK-NEXT:                                Id 'global' [D:E:%d.16 'global']
// CHECK-NEXT:                                Id 'require' [D:E:%d.17 'require']
// CHECK-NEXT:                                BlockStatement
// CHECK-NEXT:                                    ExpressionStatement
// CHECK-NEXT:                                        CallExpression : void
// CHECK-NEXT:                                            MemberExpression : any
// CHECK-NEXT:                                                SHBuiltin
// CHECK-NEXT:                                                Id 'export'
// CHECK-NEXT:                                            StringLiteral : "n3"
// CHECK-NEXT:                                            CallExpression : any
// CHECK-NEXT:                                                MemberExpression : any
// CHECK-NEXT:                                                    SHBuiltin
// CHECK-NEXT:                                                    Id 'import'
// CHECK-NEXT:                                                NumericLiteral : 1
// CHECK-NEXT:                                                StringLiteral : "n"
// CHECK-NEXT:                                    ReturnStatement
// CHECK-NEXT:                                        NumericLiteral : 0
