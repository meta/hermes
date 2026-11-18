/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -typed %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec -typed %s | %FileCheck --match-full-lines %s

// The indexer makes this an untyped object, so properties can be deleted.
var obj: any = ({a: 'a', b: 'b', '1': '1'}: {[string]: string});
print(obj.a, obj.b, obj[1]);
//CHECK: a b 1

delete obj.a;
print(obj.a, obj.b, obj[1]);
//CHECK: undefined b 1

delete obj['b'];
print(obj.a, obj.b, obj[1]);
//CHECK: undefined undefined 1

delete obj[1];
print(obj.a, obj.b, obj[1]);
//CHECK: undefined undefined undefined

// Typed objects are frozen from untyped code.
var typed: any = {a: 'a'};
try {
  delete typed.a;
} catch (e) {
  print((e: any).message, typed.a);
}
//CHECK-NEXT: Property is not configurable a

// So are object rest results and Object.entries tuples.
var src: {x: number, y: string} = {x: 1, y: 'y'};
const {x: srcX, ...rest} = src;
var restAny: any = rest;
try {
  delete restAny.y;
} catch (e) {
  print((e: any).message, rest.y);
}
//CHECK-NEXT: Property is not configurable y

var entry: [string, mixed] = Object.entries(src)[0];
var entryAny: any = entry;
try {
  delete entryAny[0];
} catch (e) {
  print((e: any).message, entry[0]);
}
//CHECK-NEXT: Property is not configurable x
