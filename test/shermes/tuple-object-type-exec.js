/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -verify-ir -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -verify-ir -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -verify-ir -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O0 -dump-ir %s | %FileCheck --check-prefix=IR %s

// Tuple and object types, including recursive ones, lowered to IR tuple and
// exactObject types. Values of these types are passed, stored into typed
// slots (static fields and a typed variable captured by a closure) and
// returned.

(function () {

// Cycle through an object.
type Tree = null | {val: number, left: Tree, right: Tree};
function mk(val: number, left: Tree, right: Tree): Tree {
  return {val: val, left: left, right: right};
}
function sum(t: Tree): number {
  if (t === null) return 0;
  return t.val + sum(t.left) + sum(t.right);
}
const tree: Tree = mk(1, mk(2, null, mk(3, null, null)), mk(4, null, null));
print(sum(tree));
// CHECK: 10

// Cycle through a tuple.
type L = [number, L | null];
function len(l: L | null): number {
  return l === null ? 0 : 1 + len(l[1]);
}
const l3: L = [3, null];
const l2: L = [2, l3];
const l1: L = [1, l2];
function second(l: L): number {
  const next = l[1];
  return next === null ? -1 : next[0];
}
print(len(l1), second(l1));
// CHECK-NEXT: 3 2

// Cycle through a tuple and a function returning the tuple.
type TF = [number, () => TF];
function counter(n: number): TF {
  return [n, () => counter(n + 1)];
}
print(counter(1)[1]()[1]()[0]);
// CHECK-NEXT: 3

// Mutual recursion between an object alias and a tuple alias.
type O = {t: T};
type T = [O | null];
const leaf: O = {t: [null]};
const root: O = {t: [leaf]};
function depth(o: O | null): number {
  return o === null ? 0 : 1 + depth(o.t[0]);
}
print(depth(root));
// CHECK-NEXT: 2

// Identity: every Flow type equal to a recursive alias lowers to the same IR
// type. Tree2 and the declared parameter types are different Flow type
// objects that Flow considers equal to Tree. Execution cannot show this: a
// store into a typed slot of a distinct but compatible IR type gets a trusted
// narrow and runs the same. The IR check below shows that the store of
// read()'s Tree result into the Tree2 static field needs no narrow.
type Tree2 = null | {val: number, left: Tree2, right: Tree2};
class Store {
  static tree: Tree;
  static tree2: Tree2;
  static tf: TF;
  static pair: [number, string];
  static obj: {x: number, y: string};
}
function keepTree(t: Tree): Tree2 {
  let local: Tree = t;
  function read(): Tree {
    return local;
  }
  Store.tree = local;
  Store.tree2 = read();
  return Store.tree;
}
// IR-LABEL: function keepTree(
// IR: [[CALL:%[0-9]+]] = CallInst [njsf] (:null|exactObject(val: number, left: null|^1, right: null|^1)) {{.*}}
// IR-NEXT: StoreFrameInst %{{[0-9]+}}: environment, [[CALL]]: null|exactObject(val: number, left: null|^1, right: null|^1), [%VS{{[0-9]+}}.tree2]: null|exactObject(val: number, left: null|^1, right: null|^1)
print(sum(keepTree(tree)), sum(Store.tree2));
// CHECK-NEXT: 10 10
function keepTF(t: TF): TF {
  let local: TF = t;
  function read(): TF {
    return local;
  }
  Store.tf = read();
  return local;
}
print(keepTF(counter(5))[1]()[0], Store.tf[0]);
// CHECK-NEXT: 6 5

// Non-recursive tuple and object literals stored into typed slots.
function keepPair(n: number): [number, string] {
  let local: [number, string] = [n, 'p'];
  function read(): [number, string] {
    return local;
  }
  Store.pair = read();
  return local;
}
function keepObj(n: number): {x: number, y: string} {
  let local: {x: number, y: string} = {x: n, y: 'o'};
  function read(): {x: number, y: string} {
    return local;
  }
  Store.obj = read();
  return local;
}
print(keepPair(7)[1], Store.pair[0], keepObj(8).y, Store.obj.x);
// CHECK-NEXT: p 7 o 8

})();
