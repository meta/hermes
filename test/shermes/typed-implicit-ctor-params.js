/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -O0 -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -O0 -verify-ir %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -O -verify-ir %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O -verify-ir -dump-ir -Xdump-functions=global,Opt %s | %FileCheck --check-prefix=OPT %s

// A class without an explicit constructor forwards its arguments to the
// nearest ancestor's explicit constructor.

'use strict';

print('rest');
// CHECK-LABEL: rest

// The rest parameter receives every argument, not just the first.
class RestBase {
  s: number;
  constructor(...xs: number[]) {
    let sum: number = 0;
    for (let i: number = 0; i < xs.length; ++i) sum += xs[i];
    this.s = xs.length * 100 + sum;
  }
}
class Rest extends RestBase {}
print(new Rest().s, new Rest(1, 2, 3).s);
// CHECK-NEXT: 0 306

print('fixed and rest');
// CHECK-LABEL: fixed and rest

// Fixed parameters followed by a rest parameter, forwarded through two
// implicit constructors, the first of which also initializes a field.
class FixedRestBase {
  s: string;
  constructor(a: string, b?: string, ...xs: string[]) {
    let r: string = a + ':' + (b ?? 'u') + ':';
    for (let i: number = 0; i < xs.length; ++i) r += xs[i];
    this.s = r;
  }
}
class FixedRest extends FixedRestBase {
  f: number = 7;
}
class FixedRest2 extends FixedRest {}
print(new FixedRest('1').s, new FixedRest('2', 'b').s);
// CHECK-NEXT: 1:u: 2:b:
print(new FixedRest('3', 'b', 'x', 'y').s);
// CHECK-NEXT: 3:b:xy
print(new FixedRest2('4', 'c', 'p', 'q', 'r').s, new FixedRest2('5').f);
// CHECK-NEXT: 4:c:pqr 7

print('length');
// CHECK-LABEL: length

// An implicit constructor has length 0, like a default derived constructor
// in JS, whatever the length of the constructor it forwards to.
class DefaultBase {
  constructor(a: number, b: number = 1) {}
}
class Default extends DefaultBase {}
const restBase: any = RestBase, rest: any = Rest;
const fixedRestBase: any = FixedRestBase, fixedRest: any = FixedRest;
const fixedRest2: any = FixedRest2;
const defaultBase: any = DefaultBase, defaultDerived: any = Default;
print(restBase.length, rest.length);
// CHECK-NEXT: 0 0
print(fixedRestBase.length, fixedRest.length, fixedRest2.length);
// CHECK-NEXT: 2 0 0
print(defaultBase.length, defaultDerived.length);
// CHECK-NEXT: 1 0

print('optional');
// CHECK-LABEL: optional

// An omitted optional parameter is undefined. At -O, OptBase's constructor
// is inlined into Opt's, which is too large to be inlined into its two
// callers; if the forwarded parameter were typed 'number', the check for
// undefined would fold to false and 'v' would hold undefined. Opt's body
// survives even where it is inlined, so the OPT checks also require that
// both callers still call it.
class OptBase {
  v: number;
  constructor(x?: number) {
    this.v = x === undefined ? -1 : x;
  }
}
class Opt extends OptBase {
  f0: number = 0; f1: number = 1; f2: number = 2; f3: number = 3;
  f4: number = 4; f5: number = 5; f6: number = 6; f7: number = 7;
  f8: number = 8; f9: number = 9; f10: number = 10; f11: number = 11;
  f12: number = 12; f13: number = 13; f14: number = 14; f15: number = 15;
  f16: number = 16; f17: number = 17; f18: number = 18; f19: number = 19;
  f20: number = 20; f21: number = 21; f22: number = 22; f23: number = 23;
  f24: number = 24; f25: number = 25; f26: number = 26; f27: number = 27;
  f28: number = 28; f29: number = 29; f30: number = 30; f31: number = 31;
  f32: number = 32; f33: number = 33; f34: number = 34; f35: number = 35;
  f36: number = 36; f37: number = 37; f38: number = 38; f39: number = 39;
}
print(new Opt().v, new Opt(5).v);
// CHECK-NEXT: -1 5

// OPT-LABEL: function global(): undefined
// OPT: CallInst (:undefined) {{.*}}, %Opt(): functionCode, {{.*}}: classInstance(Opt){{$}}
// OPT: CallInst (:undefined) {{.*}}, %Opt(): functionCode, {{.*}}: classInstance(Opt), 5: number{{$}}
// OPT-LABEL: derived constructor Opt(x: undefined|number)
// OPT-NEXT: %BB0:
// OPT: [[X:%[0-9]+]] = LoadParamInst (:undefined|number) %x: undefined|number
// OPT: BinaryStrictlyEqualInst (:boolean) [[X]]: undefined|number, undefined: undefined
// OPT: function_end
