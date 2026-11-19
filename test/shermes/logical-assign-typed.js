/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -exec %s | %FileCheck --match-full-lines %s

(function() {

class C {
  f: ?number;
  constructor() {
    this.f = null;
  }
}

function main() {
  print('logical assignment typed');
  // CHECK-LABEL: logical assignment typed

  let x: ?number = null;
  print(x ??= 10, x);
  // CHECK-NEXT: 10 10
  print(x ??= 20, x);
  // CHECK-NEXT: 10 10

  let y: ?string = 'a';
  print(y ||= 'b', y);
  // CHECK-NEXT: a a
  y = null;
  print(y ||= 'c', y);
  // CHECK-NEXT: c c

  let z: ?number = 7;
  print(z &&= 8, z);
  // CHECK-NEXT: 8 8
  z = null;
  print(z &&= 9, z);
  // CHECK-NEXT: null null

  // A typed field target.
  const c = new C();
  print(c.f ??= 42, c.f);
  // CHECK-NEXT: 42 42
  print(c.f &&= 43, c.f);
  // CHECK-NEXT: 43 43

  // The base and the computed key are evaluated exactly once, whether or not
  // the assignment happens.
  const arr: number[] = [0, 1];
  function idx(i: number): number {
    print('idx', i);
    return i;
  }
  print(arr[idx(0)] ??= 5, arr[0]);
  // CHECK-NEXT: idx 0
  // CHECK-NEXT: 0 0
  print(arr[idx(1)] &&= 6, arr[1]);
  // CHECK-NEXT: idx 1
  // CHECK-NEXT: 6 6

  // The RHS is not evaluated at all when the operation short circuits.
  function side(): number {
    print('side');
    return 99;
  }
  let s: ?number = 1;
  print(s ??= side(), s);
  // CHECK-NEXT: 1 1
  print(s &&= side(), s);
  // CHECK-NEXT: side
  // CHECK-NEXT: 99 99

  // IRGen emits the body of a 'finally' twice.
  function fin(v: ?number): ?number {
    try {
      return v;
    } finally {
      v ??= 11;
      print('fin', v);
    }
  }
  print(fin(null));
  // CHECK-NEXT: fin 11
  // CHECK-NEXT: null
  print(fin(3));
  // CHECK-NEXT: fin 3
  // CHECK-NEXT: 3

  // The implicit cast on the RHS throws when it fails. It is only reached
  // when the operation doesn't short circuit.
  function castThrows(t: number, u: ?number) {
    try {
      t ||= u;
      print('no throw', t);
    } catch (e) {
      print('caught');
    }
  }
  castThrows(0, null);
  // CHECK-NEXT: caught
  castThrows(0, 5);
  // CHECK-NEXT: no throw 5
  castThrows(1, null);
  // CHECK-NEXT: no throw 1

  // The result is still a typed Array, both when the two sides are the same
  // type and when dropping the left's null leaves only the right.
  function arrs(a: number[], b: ?(number[]), c: number[]): void {
    for (const v of (a ??= c)) print('same', v);
    for (const v of (b ??= c)) print('nullish', v);
    print((a ||= c)[0], (a &&= c).length);
  }
  arrs([1, 2], null, [3, 4]);
  // CHECK-NEXT: same 1
  // CHECK-NEXT: same 2
  // CHECK-NEXT: nullish 3
  // CHECK-NEXT: nullish 4
  // CHECK-NEXT: 1 2
}

main();
})();
