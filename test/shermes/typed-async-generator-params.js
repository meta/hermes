/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -O0 -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -verify-ir %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O0 -Xcustom-opt=lowergeneratorfunction -Xdump-functions=af,g -dump-ir %s | %FileCheck --match-full-lines --check-prefix=IR %s

// Typed parameters of async and generator functions are moved to the outer
// function when the generator is lowered, and keep their types there.
// Generators and `.call` are invoked through `any`, so the calls do not depend
// on how typed mode types the result of an async or generator call.

class C {
  v: number;
  constructor() {
    this.v = 7;
  }
}

async function af(x: number, s: string): number {
  print('af', x, s);
  const y: number = x + 1;
  return y;
}

function* g(x: number, s: string): number {
  print('g', x, s);
  yield x;
  return x + 1;
}

function* gt(this: C, x: number): number {
  print('gt', this.v, x);
  yield x;
  return x;
}

async function at(this: C, x: number): number {
  print('at', this.v, x);
  return x;
}

const aa = async (x: number): number => {
  print('aa', x);
  return x;
};

function* gd(x: number = 5, y?: number): number {
  print('gd', x, y);
  yield x;
  return x;
}

async function ar(x: number, ...rest: number[]): number {
  print('ar', x, rest.length);
  return x;
}

const anyG: any = g;
const anyGt: any = gt;
const anyAt: any = at;
const anyGd: any = gd;
const c: C = new C();

// An async function runs synchronously until its first await.
af(1, 'a');
// CHECK: af 1 a

const r: any = anyG(2, 'b').next();
print(r.value, r.done);
// CHECK-NEXT: g 2 b
// CHECK-NEXT: 2 false

print(anyGt.call(c, 3).next().value);
// CHECK-NEXT: gt 7 3
// CHECK-NEXT: 3

anyAt.call(c, 4);
// CHECK-NEXT: at 7 4

aa(6);
// CHECK-NEXT: aa 6

print(anyGd().next().value);
// CHECK-NEXT: gd 5 undefined
// CHECK-NEXT: 5

ar(8, 9, 10);
// CHECK-NEXT: ar 8 2

// IR-LABEL: function af(x: number, s: string): any
// IR:         [[X:%[0-9]+]] = LoadParamInst (:number) %x: number
// IR-NEXT:         StoreFrameInst {{%[0-9]+}}: environment, [[X]]: number, [%VS1.x]: number
// IR-NEXT:    [[S:%[0-9]+]] = LoadParamInst (:string) %s: string
// IR-NEXT:         StoreFrameInst {{%[0-9]+}}: environment, [[S]]: string, [%VS1.s]: string
// IR-LABEL: function g(x: number, s: string): any
// IR:         [[X2:%[0-9]+]] = LoadParamInst (:number) %x: number
// IR-NEXT:         StoreFrameInst {{%[0-9]+}}: environment, [[X2]]: number, [%VS2.x]: number
// IR-NEXT:    [[S2:%[0-9]+]] = LoadParamInst (:string) %s: string
// IR-NEXT:         StoreFrameInst {{%[0-9]+}}: environment, [[S2]]: string, [%VS2.s]: string
