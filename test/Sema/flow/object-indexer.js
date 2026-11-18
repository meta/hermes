/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -fno-std-globals -typed -dump-sema %s | %FileCheckOrRegen %s --match-full-lines

// A string-keyed indexer.
type Dict = {[string]: number};
let d: Dict = {a: 1, b: 2};
// A read is typed T, a write requires the value type T.
let n: number = d["x"];
d["y"] = 3;

// A number-keyed indexer.
type NumDict = {[number]: string};
let nd: NumDict = {};
let s: string = nd[0];
nd[1] = "x";

// Variance sigils on the indexer.
type RO = {+[string]: number};
type WO = {-[string]: number};
function readRO(o: RO): number {
  return o["k"];
}
function writeWO(o: WO): void {
  o["k"] = 1;
}

// An empty object literal flows into an indexer type.
let empty: Dict = {};

// Dot access routes through the indexer too, treating the property name as a
// string key.
let dotRead: number = d.x;
d.y = 4;

// `delete` removes a key from an indexer object. It is allowed (it does not
// violate the uniform value type) for read-write and write-only indexers, via
// either computed or dot access.
delete d["y"];
delete d.x;
delete nd[0];
function delWO(o: WO): void {
  delete o["k"];
}

// Auto-generated content below. Please do not modify manually.

// CHECK:%untyped_function.1 = untyped_function()
// CHECK-NEXT:%object.2 = object({
// CHECK-NEXT:  [string]: number
// CHECK-NEXT:})
// CHECK-NEXT:%object.3 = object({
// CHECK-NEXT:  [number]: string
// CHECK-NEXT:})
// CHECK-NEXT:%object.4 = object({
// CHECK-NEXT:  +[string]: number
// CHECK-NEXT:})
// CHECK-NEXT:%object.5 = object({
// CHECK-NEXT:  -[string]: number
// CHECK-NEXT:})
// CHECK-NEXT:%function.6 = function(o: %object.4): number
// CHECK-NEXT:%function.7 = function(o: %object.5): void
// CHECK-NEXT:%object.8 = object({
// CHECK-NEXT:})

// CHECK:SemContext
// CHECK-NEXT:Func strict mayReachImplicitReturn
// CHECK-NEXT:    Scope %s.1
// CHECK-NEXT:        Decl %d.1 'exports' Parameter : any
// CHECK-NEXT:        Decl %d.2 'd' Let : %object.2
// CHECK-NEXT:        Decl %d.3 'n' Let : number
// CHECK-NEXT:        Decl %d.4 'nd' Let : %object.3
// CHECK-NEXT:        Decl %d.5 's' Let : string
// CHECK-NEXT:        Decl %d.6 'readRO' Var : %function.6
// CHECK-NEXT:        Decl %d.7 'writeWO' Var : %function.7
// CHECK-NEXT:        Decl %d.8 'empty' Let : %object.2
// CHECK-NEXT:        Decl %d.9 'dotRead' Let : number
// CHECK-NEXT:        Decl %d.10 'delWO' Var : %function.7
// CHECK-NEXT:        Decl %d.11 'arguments' Var Arguments
// CHECK-NEXT:        hoistedFunction readRO
// CHECK-NEXT:        hoistedFunction writeWO
// CHECK-NEXT:        hoistedFunction delWO
// CHECK-NEXT:    Func strict noImplicitReturn
// CHECK-NEXT:        Scope %s.2
// CHECK-NEXT:            Decl %d.12 'o' Parameter : %object.4
// CHECK-NEXT:            Decl %d.13 'arguments' Var Arguments
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.3
// CHECK-NEXT:            Decl %d.14 'o' Parameter : %object.5
// CHECK-NEXT:            Decl %d.15 'arguments' Var Arguments
// CHECK-NEXT:    Func strict mayReachImplicitReturn
// CHECK-NEXT:        Scope %s.4
// CHECK-NEXT:            Decl %d.16 'o' Parameter : %object.5
// CHECK-NEXT:            Decl %d.17 'arguments' Var Arguments

// CHECK:FunctionExpression : %untyped_function.1
// CHECK-NEXT:    Id 'exports' [D:E:%d.1 'exports']
// CHECK-NEXT:    BlockStatement
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'Dict'
// CHECK-NEXT:            ObjectTypeAnnotation
// CHECK-NEXT:                ObjectTypeIndexer
// CHECK-NEXT:                    StringTypeAnnotation
// CHECK-NEXT:                    NumberTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.2
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'a'
// CHECK-NEXT:                        NumericLiteral : 1
// CHECK-NEXT:                    Property
// CHECK-NEXT:                        Id 'b'
// CHECK-NEXT:                        NumericLiteral : 2
// CHECK-NEXT:                Id 'd' [D:E:%d.2 'd']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                MemberExpression : number
// CHECK-NEXT:                    Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                    StringLiteral : "x"
// CHECK-NEXT:                Id 'n' [D:E:%d.3 'n']
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : 3
// CHECK-NEXT:                MemberExpression : number
// CHECK-NEXT:                    Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                    StringLiteral : "y"
// CHECK-NEXT:                NumericLiteral : 3
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'NumDict'
// CHECK-NEXT:            ObjectTypeAnnotation
// CHECK-NEXT:                ObjectTypeIndexer
// CHECK-NEXT:                    NumberTypeAnnotation
// CHECK-NEXT:                    StringTypeAnnotation
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.8
// CHECK-NEXT:                Id 'nd' [D:E:%d.4 'nd']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                MemberExpression : string
// CHECK-NEXT:                    Id 'nd' [D:E:%d.4 'nd'] : %object.3
// CHECK-NEXT:                    NumericLiteral : 0
// CHECK-NEXT:                Id 's' [D:E:%d.5 's']
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : "x"
// CHECK-NEXT:                MemberExpression : string
// CHECK-NEXT:                    Id 'nd' [D:E:%d.4 'nd'] : %object.3
// CHECK-NEXT:                    NumericLiteral : 1
// CHECK-NEXT:                StringLiteral : "x"
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'RO'
// CHECK-NEXT:            ObjectTypeAnnotation
// CHECK-NEXT:                ObjectTypeIndexer
// CHECK-NEXT:                    StringTypeAnnotation
// CHECK-NEXT:                    NumberTypeAnnotation
// CHECK-NEXT:                    Variance
// CHECK-NEXT:        TypeAlias
// CHECK-NEXT:            Id 'WO'
// CHECK-NEXT:            ObjectTypeAnnotation
// CHECK-NEXT:                ObjectTypeIndexer
// CHECK-NEXT:                    StringTypeAnnotation
// CHECK-NEXT:                    NumberTypeAnnotation
// CHECK-NEXT:                    Variance
// CHECK-NEXT:        FunctionDeclaration : %function.6
// CHECK-NEXT:            Id 'readRO' [D:E:%d.6 'readRO']
// CHECK-NEXT:            Id 'o' [D:E:%d.12 'o']
// CHECK-NEXT:            BlockStatement
// CHECK-NEXT:                ReturnStatement
// CHECK-NEXT:                    MemberExpression : number
// CHECK-NEXT:                        Id 'o' [D:E:%d.12 'o'] : %object.4
// CHECK-NEXT:                        StringLiteral : "k"
// CHECK-NEXT:        FunctionDeclaration : %function.7
// CHECK-NEXT:            Id 'writeWO' [D:E:%d.7 'writeWO']
// CHECK-NEXT:            Id 'o' [D:E:%d.14 'o']
// CHECK-NEXT:            BlockStatement
// CHECK-NEXT:                ExpressionStatement
// CHECK-NEXT:                    AssignmentExpression : 1
// CHECK-NEXT:                        MemberExpression : number
// CHECK-NEXT:                            Id 'o' [D:E:%d.14 'o'] : %object.5
// CHECK-NEXT:                            StringLiteral : "k"
// CHECK-NEXT:                        NumericLiteral : 1
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                ObjectExpression : %object.8
// CHECK-NEXT:                Id 'empty' [D:E:%d.8 'empty']
// CHECK-NEXT:        VariableDeclaration
// CHECK-NEXT:            VariableDeclarator
// CHECK-NEXT:                MemberExpression : number
// CHECK-NEXT:                    Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                    Id 'x'
// CHECK-NEXT:                Id 'dotRead' [D:E:%d.9 'dotRead']
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            AssignmentExpression : 4
// CHECK-NEXT:                MemberExpression : number
// CHECK-NEXT:                    Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                    Id 'y'
// CHECK-NEXT:                NumericLiteral : 4
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            UnaryExpression : boolean
// CHECK-NEXT:                MemberExpression : number
// CHECK-NEXT:                    Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                    StringLiteral : "y"
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            UnaryExpression : boolean
// CHECK-NEXT:                MemberExpression : number
// CHECK-NEXT:                    Id 'd' [D:E:%d.2 'd'] : %object.2
// CHECK-NEXT:                    Id 'x'
// CHECK-NEXT:        ExpressionStatement
// CHECK-NEXT:            UnaryExpression : boolean
// CHECK-NEXT:                MemberExpression : string
// CHECK-NEXT:                    Id 'nd' [D:E:%d.4 'nd'] : %object.3
// CHECK-NEXT:                    NumericLiteral : 0
// CHECK-NEXT:        FunctionDeclaration : %function.7
// CHECK-NEXT:            Id 'delWO' [D:E:%d.10 'delWO']
// CHECK-NEXT:            Id 'o' [D:E:%d.16 'o']
// CHECK-NEXT:            BlockStatement
// CHECK-NEXT:                ExpressionStatement
// CHECK-NEXT:                    UnaryExpression : boolean
// CHECK-NEXT:                        MemberExpression : number
// CHECK-NEXT:                            Id 'o' [D:E:%d.16 'o'] : %object.5
// CHECK-NEXT:                            StringLiteral : "k"
