/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -fno-std-globals --typed --dump-sema %s | %FileCheckOrRegen %s --match-full-lines

(function() {
  class C {
    f: ?number;
    constructor() {
      this.f = null;
    }
  }

  function foo(x: ?number, y: number|null, z: number, w: void) {
    // '??=' and '||=' can never produce the left's null/void.
    x ??= z;
    x ||= z;
    // '&&=' can: a falsy left is the result.
    x &&= z;
    // Only null is dropped here, the right contributes void.
    x ??= w;
    y ??= z;
    // The left can't short circuit into the assignment, but that's not an
    // error, same as 'z ?? 1'.
    z ??= z;
  }

  function members(c: C, o: {p: number|string}) {
    c.f ??= 1;
    c.f &&= 1;
    o.p ||= 'x';
  }

  // The RHS needs an ImplicitCheckedCast to reach the LHS type. Use '||='
  // and '&&=' so the assignment is reachable; '??=' on a non-nullish LHS
  // would make the cast dead.
  function needsCast(n: number, opt: ?number) {
    n ||= opt;
    n &&= opt;
  }
})

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%class.2 = class(C {
// CHECK-NEXT:  %constructor: %function.5
// CHECK-NEXT:  %homeObject: %class.13
// CHECK-NEXT:  f: %union.4
// CHECK-NEXT:})
// CHECK-NEXT:%class_constructor.3 = class_constructor(%class.2)
// CHECK-NEXT:%union.4 = union(void | null | number)
// CHECK-NEXT:%function.5 = function(this: %class.2): void
// CHECK-NEXT:%union.6 = union(null | number)
// CHECK-NEXT:%function.7 = function(x: %union.4, y: %union.6, z: number, w: void): any
// CHECK-NEXT:%union.8 = union(string | number)
// CHECK-NEXT:%object.9 = object({
// CHECK-NEXT:  p: %union.8
// CHECK-NEXT:})
// CHECK-NEXT:%function.10 = function(c: %class.2, o: %object.9): any
// CHECK-NEXT:%function.11 = function(n: number, opt: %union.4): any
// CHECK-NEXT:%union.12 = union(void | number)
// CHECK-NEXT:%class.13 = class( {
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'arguments' Var Arguments
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:            Decl %d.3 'C' Class : %class_constructor.3
// CHECK-NEXT:            Decl %d.4 'foo' Var : %function.7
// CHECK-NEXT:            Decl %d.5 'members' Var : %function.10
// CHECK-NEXT:            Decl %d.6 'needsCast' Var : %function.11
// CHECK-NEXT:            Decl %d.7 'arguments' Var Arguments
// CHECK-NEXT:            hoistedFunction foo
// CHECK-NEXT:            hoistedFunction members
// CHECK-NEXT:            hoistedFunction needsCast
// CHECK-NEXT:            Scope %s.3
// CHECK-NEXT:        Func strict mayReachImplicitReturn
// CHECK-NEXT:            Scope %s.4
// CHECK-NEXT:                Decl %d.8 'arguments' Var Arguments
// CHECK-NEXT:        Func strict mayReachImplicitReturn
// CHECK-NEXT:            Scope %s.5
// CHECK-NEXT:                Decl %d.9 'x' Parameter : %union.4
// CHECK-NEXT:                Decl %d.10 'y' Parameter : %union.6
// CHECK-NEXT:                Decl %d.11 'z' Parameter : number
// CHECK-NEXT:                Decl %d.12 'w' Parameter : void
// CHECK-NEXT:                Decl %d.13 'arguments' Var Arguments
// CHECK-NEXT:        Func strict mayReachImplicitReturn
// CHECK-NEXT:            Scope %s.6
// CHECK-NEXT:                Decl %d.14 'c' Parameter : %class.2
// CHECK-NEXT:                Decl %d.15 'o' Parameter : %object.9
// CHECK-NEXT:                Decl %d.16 'arguments' Var Arguments
// CHECK-NEXT:        Func strict mayReachImplicitReturn
// CHECK-NEXT:            Scope %s.7
// CHECK-NEXT:                Decl %d.17 'n' Parameter : number
// CHECK-NEXT:                Decl %d.18 'opt' Parameter : %union.4
// CHECK-NEXT:                Decl %d.19 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            FunctionExpression : %untyped_function.1
// CHECK-NEXT:                BlockStatement
// CHECK-NEXT:                    ClassDeclaration Scope %s.3
// CHECK-NEXT:                        Id 'C' [D:E:%d.3 'C']
// CHECK-NEXT:                        ClassBody
// CHECK-NEXT:                            ClassProperty : %union.4
// CHECK-NEXT:                                Id 'f'
// CHECK-NEXT:                            MethodDefinition : %function.5
// CHECK-NEXT:                                Id 'constructor'
// CHECK-NEXT:                                FunctionExpression : %function.5
// CHECK-NEXT:                                    BlockStatement
// CHECK-NEXT:                                        ExpressionStatement
// CHECK-NEXT:                                            AssignmentExpression : null
// CHECK-NEXT:                                                MemberExpression : %union.4
// CHECK-NEXT:                                                    ThisExpression : %class.2
// CHECK-NEXT:                                                    Id 'f'
// CHECK-NEXT:                                                NullLiteral : null
// CHECK-NEXT:                    FunctionDeclaration : %function.7
// CHECK-NEXT:                        Id 'foo' [D:E:%d.4 'foo']
// CHECK-NEXT:                        Id 'x' [D:E:%d.9 'x']
// CHECK-NEXT:                        Id 'y' [D:E:%d.10 'y']
// CHECK-NEXT:                        Id 'z' [D:E:%d.11 'z']
// CHECK-NEXT:                        Id 'w' [D:E:%d.12 'w']
// CHECK-NEXT:                        BlockStatement
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : number
// CHECK-NEXT:                                    Id 'x' [D:E:%d.9 'x'] : %union.4
// CHECK-NEXT:                                    Id 'z' [D:E:%d.11 'z'] : number
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : number
// CHECK-NEXT:                                    Id 'x' [D:E:%d.9 'x'] : %union.4
// CHECK-NEXT:                                    Id 'z' [D:E:%d.11 'z'] : number
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : %union.4
// CHECK-NEXT:                                    Id 'x' [D:E:%d.9 'x'] : %union.4
// CHECK-NEXT:                                    Id 'z' [D:E:%d.11 'z'] : number
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : %union.12
// CHECK-NEXT:                                    Id 'x' [D:E:%d.9 'x'] : %union.4
// CHECK-NEXT:                                    Id 'w' [D:E:%d.12 'w'] : void
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : number
// CHECK-NEXT:                                    Id 'y' [D:E:%d.10 'y'] : %union.6
// CHECK-NEXT:                                    Id 'z' [D:E:%d.11 'z'] : number
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : number
// CHECK-NEXT:                                    Id 'z' [D:E:%d.11 'z'] : number
// CHECK-NEXT:                                    Id 'z' [D:E:%d.11 'z'] : number
// CHECK-NEXT:                    FunctionDeclaration : %function.10
// CHECK-NEXT:                        Id 'members' [D:E:%d.5 'members']
// CHECK-NEXT:                        Id 'c' [D:E:%d.14 'c']
// CHECK-NEXT:                        Id 'o' [D:E:%d.15 'o']
// CHECK-NEXT:                        BlockStatement
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : number
// CHECK-NEXT:                                    MemberExpression : %union.4
// CHECK-NEXT:                                        Id 'c' [D:E:%d.14 'c'] : %class.2
// CHECK-NEXT:                                        Id 'f'
// CHECK-NEXT:                                    NumericLiteral : 1
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : %union.4
// CHECK-NEXT:                                    MemberExpression : %union.4
// CHECK-NEXT:                                        Id 'c' [D:E:%d.14 'c'] : %class.2
// CHECK-NEXT:                                        Id 'f'
// CHECK-NEXT:                                    NumericLiteral : 1
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : %union.8
// CHECK-NEXT:                                    MemberExpression : %union.8
// CHECK-NEXT:                                        Id 'o' [D:E:%d.15 'o'] : %object.9
// CHECK-NEXT:                                        Id 'p'
// CHECK-NEXT:                                    StringLiteral : "x"
// CHECK-NEXT:                    FunctionDeclaration : %function.11
// CHECK-NEXT:                        Id 'needsCast' [D:E:%d.6 'needsCast']
// CHECK-NEXT:                        Id 'n' [D:E:%d.17 'n']
// CHECK-NEXT:                        Id 'opt' [D:E:%d.18 'opt']
// CHECK-NEXT:                        BlockStatement
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : number
// CHECK-NEXT:                                    Id 'n' [D:E:%d.17 'n'] : number
// CHECK-NEXT:                                    ImplicitCheckedCast : number
// CHECK-NEXT:                                        Id 'opt' [D:E:%d.18 'opt'] : %union.4
// CHECK-NEXT:                            ExpressionStatement
// CHECK-NEXT:                                AssignmentExpression : number
// CHECK-NEXT:                                    Id 'n' [D:E:%d.17 'n'] : number
// CHECK-NEXT:                                    ImplicitCheckedCast : number
// CHECK-NEXT:                                        Id 'opt' [D:E:%d.18 'opt'] : %union.4
