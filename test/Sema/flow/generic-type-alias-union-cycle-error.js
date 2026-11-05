/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -ferror-limit=0 -fno-std-globals --typed --dump-sema %s 2>&1) | %FileCheck %s

function direct(): void {
  type Id<T> = T;
  type U = number | Id<U>;
}

function nullable(): void {
  type Id<T> = T;
  type U = ?Id<U>;
}

function mixed(): void {
  type Id<T> = T;
  type Alias<T> = Id<T>;
  type U = number | Alias<U>;
}

function mutual(): void {
  type Id<T> = T;
  type U = number | Id<V>;
  type V = string | Id<U>;
}

function duplicate(): void {
  type Id<T> = T;
  type U = Id<U> | Id<U>;
}

function aliasOnly(): void {
  type A<T> = B<T>;
  type B<T> = A<T>;
  type Invalid = A<number>;
}

function expandedUnion(): void {
  type Union<T> = number | T;
  type U = Union<U>;
}

function recursiveArgumentAliasOnly(): void {
  type A<T> = B<T>;
  type B<T> = A<T>;
  type U = A<U>;
}

// Cycles hidden behind a cached generic alias expansion.
function cachedExpansion(): void {
  type Id<T> = T;
  type U = Id<number | Id<U>>;
}

function cachedNullable(): void {
  type Id<T> = T;
  type U = ?Id<?Id<U>>;
}

function cachedMutual(): void {
  type Id<T> = T;
  type U = Id<number | Id<V>>;
  type V = Id<string | Id<U>>;
}

// Expansions that never reach a type.
class Box<T> {}

function growingArgument(): void {
  type A<T> = A<Box<T>>;
  type U = A<U>;
}

function guardedGrowingArgument(): void {
  type A<T> = Box<A<Box<T>>>;
  type U = A<number>;
}

function swappedGrowingArguments(): void {
  type A<X, Y> = [A<Y, Box<X>>];
  type U = A<number, string>;
}

function equalGrowingArguments(): void {
  type A<X, Y> = [A<Box<X>, Box<Y>>];
  type U = A<boolean, boolean>;
}

function growingTuple(): void {
  type A<T> = [A<[T]>];
  type U = A<number>;
}

// The cycle is first found through the tuple, before the aliases are
// expanded.
function cycleAfterTuple(): void {
  type G<X> = X | number;
  type T0 = ?([T0] | G<boolean> | G<T0>);
}

// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = number | Id<U>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = ?Id<U>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = number | Alias<U>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = number | Id<V>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = Id<U> | Id<U>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = Id<U> | Id<U>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type B<T> = A<T>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = Union<U>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type B<T> = A<T>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = Id<number | Id<U>>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = ?Id<?Id<U>>;
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type U = Id<number | Id<V>>;
// CHECK: error: ft: too deeply nested generic type alias expansion
// CHECK-NEXT: type A<T> = A<Box<T>>;
// CHECK: error: ft: too deeply nested generic type alias expansion
// CHECK-NEXT: type A<T> = Box<A<Box<T>>>;
// CHECK: error: ft: too deeply nested generic type alias expansion
// CHECK-NEXT: type A<X, Y> = [A<Y, Box<X>>];
// CHECK: error: ft: too deeply nested generic type alias expansion
// CHECK-NEXT: type A<X, Y> = [A<Box<X>, Box<Y>>];
// CHECK: error: ft: too deeply nested generic type alias expansion
// CHECK-NEXT: type A<T> = [A<[T]>];
// CHECK: error: ft: type contains a circular reference to itself
// CHECK-NEXT: type T0 = ?([T0] | G<boolean> | G<T0>);
// CHECK: Emitted 18 errors. exiting.
