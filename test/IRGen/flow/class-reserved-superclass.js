/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=useD2,getT,setCb,getT2 -typed -dump-ir -O0 %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -verify-ir -O0 -exec %s | %FileCheck --check-prefix=EXEC --match-full-lines %s
// RUN: %shermes -typed -verify-ir -O -exec %s | %FileCheck --check-prefix=EXEC --match-full-lines %s

// A class's fields are lowered while the class is reserved but not yet
// filled. The reserved class must already know its superclass, or a union of
// the class and its superclass built in that window keeps both arms, while the
// same Flow type built later lowers to the superclass alone. Such a union
// survives in cached structural types (tuples, functions), so the lowering of
// a type would depend on the order in which classes are lowered.

(function () {

// Declared first, so that lowering their signatures lowers D and D2 directly,
// before anything else that refers to them.
function useD(d: D): void {}
function useD2(d: D2): number {
  const same: D2 = d;
  return same.n;
}

class Base {
  constructor() {}
}
function fnBase(x: Base): void {}

// D's own fields refer to `D | Base`, which is Base.
class D extends Base {
  t: [D | Base];
  cb: (x: D | Base) => void;
  constructor(b: Base) {
    super();
    this.t = [b];
    this.cb = fnBase;
  }
}
function getT(d: D): [D | Base] {
  return d.t;
}
// The field's function type is the same IR type as `(x: Base) => void`, so
// the store needs no narrow.
function setCb(d: D, f: (x: Base) => void): void {
  d.cb = f;
}

// A field of the superclass refers to `D2 | Base2`. Lowering D2 lowers
// its superclass Base2, whose field lowers D2 again.
class Base2 {
  n: number;
  t: [D2 | Base2];
  constructor() {
    this.n = 2;
    this.t = [this];
  }
}
class D2 extends Base2 {
  constructor() {
    super();
  }
}
function getT2(b: Base2): [D2 | Base2] {
  return b.t;
}

const b = new Base();
const d = new D(b);
useD(d);
setCb(d, fnBase);
d.cb(d);
const d2 = new D2();
print(getT(d)[0] === b, useD2(d2), getT2(d2)[0] === d2);
// EXEC: true 2 true

})();

// D2 has a single IR type: the declared type of `same` is the type of `d`.
// CHECK-LABEL: function useD2(d: classInstance(D2)): any [typed]
// CHECK-NOT: {{.*}}UnionNarrowTrustedInst{{.*}}
// CHECK: %{{[0-9]+}} = PrLoadInst (:number) %{{[0-9]+}}: classInstance(D2), 0: number, "n": string
// CHECK-LABEL: function_end

// CHECK-LABEL: function getT(d: classInstance(D)): any [typed]
// CHECK: %{{[0-9]+}} = PrLoadInst (:uninit|tuple(classInstance(Base))) %{{[0-9]+}}: classInstance(D), 0: number, "t": string
// CHECK-LABEL: function_end

// CHECK-LABEL: function setCb(d: classInstance(D), f: function(undefined)): any [typed]
// CHECK: [[F:%[0-9]+]] = CheckedTypeCastInst (:function(undefined)) %{{[0-9]+}}: any, type(function(undefined))
// CHECK-NEXT: PrStoreInst [[F]]: function(undefined), %{{[0-9]+}}: classInstance(D), 1: number, "cb": string, false: boolean
// CHECK-LABEL: function_end

// CHECK-LABEL: function getT2(b: classInstance(Base2)): any [typed]
// CHECK: %{{[0-9]+}} = PrLoadInst (:uninit|tuple(classInstance(Base2))) %{{[0-9]+}}: classInstance(Base2), 1: number, "t": string
// CHECK-LABEL: function_end
