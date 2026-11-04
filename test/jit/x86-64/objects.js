/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s > %t.int && %hermes -Xjit=force %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-emit-type-asserts %s > %t.jit3 && diff %t.int0 %t.jit3
// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// REQUIRES: jit

// Objects and object-literal buffers: NewObject, NewObjectWithParent,
// NewObjectWithBuffer (the inline young-gen allocation of the object plus,
// past five properties, its indirect property storage) and
// NewObjectWithBufferAndParent, plus InstanceOf. The first RUN line is the
// real check -- interpreter and JIT must print the same thing. The second
// re-runs it with the type asserts on. The last two pin that the functions
// under test were in fact compiled, so the differential cannot degrade into
// comparing the interpreter against itself.
//
// Every object here is BUILT by compiled code and READ by the interpreter.
// That was forced when this file was written -- getById declined -- and it
// is kept deliberately now that it no longer does: the reads stay in
// `global`, which still declines (on typeOf, from the `typeof` in the
// churn loops), so the values are checked by an interpreter that never saw
// the emitter. props.js covers reading properties from compiled code.
//
// The literals are all-constant on purpose. A literal with a computed value
// lowers to NewObjectWithBuffer followed by PutOwnBySlotIdx for that value,
// and PutOwnBySlotIdx still declines, so such a function would not compile.
// The same applies to nested literals ({x: {y: 1}}) and to array literals.
//
// The -O0 RUN lines carry far fewer status pins than the -O ones, and that
// is not an oversight: at -O0 there is no literal buffer at all. Every
// non-empty literal lowers to NewObject followed by one DefineOwnById per
// property, and DefineOwnById declines. So at -O0 only the three functions
// with no property definitions compile -- empty (NewObject), proto
// (NewObjectWithParent) and isa (InstanceOf) -- and those three are what the
// CHECK0 lines pin. NewObjectWithBuffer coverage is -O only.
//
// The NewObjectWithBuffer slow path (_jit_new_empty_object_for_buffer) is
// covered without any special arrangement: the hidden class it loads is a
// WeakRoot that the RuntimeModule only fills in on the first execution of
// that site, and -Xjit=force compiles each function before its first call,
// so every literal here takes the slow path once and the fast path forever
// after. That first pass is also what exercises the emitter's assumption
// that the property storage sits directly after the object, since on the
// slow path the two cells come from JSObject::create rather than from
// alloc2InYoung.
//
// NOT covered, and why:
//  - `new` with a property-storing constructor. It was unreachable when
//    this file was written -- a function constructor's body declined on
//    PutByIdLoose, a class constructor's on GetById -- and the property
//    milestone has since landed both, so props.js covers it. A derived
//    class constructor still declines on ThrowIfThisInitialized, which
//    waits for the exceptions milestone.
//  - loadParentNoTraps: emitted only for `super` -- a super method call, a
//    super property read, or the implicit callee load in a derived
//    constructor. getById no longer declines, but every one of those
//    functions still contains getByIdWithReceiver or
//    throwIfThisInitialized, both of which do, so no function containing
//    LoadParentNoTraps compiles today. It stays ported and unreachable
//    until the exceptions milestone.
//  - typedLoadParent: emitted only from typed-class IRGen, i.e. not from
//    plain JS at all.
//  - newTypedObjectWithBuffer: likewise typed-class only.
//  - newObjectWithBufferSlow: reached when a shape has more properties than
//    fit a young-gen allocation, or when a literal holds a double that
//    cannot be stored inline. In this build the first threshold is past two
//    thousand properties and the second never triggers, since HV64
//    SmallHermesValue inlines every double. A literal large enough to reach
//    it would dominate this file for no extra signal.

// NewObject: an empty literal, no buffer at all.
function empty() {
  return {};
}

// Exactly DIRECT_PROPERTY_SLOTS (6) properties, so the object is a single
// cell and every slot is a direct one. One value of each kind the buffer
// visitor handles apart from undefined: number, string, bool, null, and an
// integral double, plus a second number to fill the last direct slot.
function small() {
  return {a: 1, b: "two", c: true, d: null, e: 3.5, f: 6};
}

// Seven properties: one past the direct slots, so this is the smallest
// shape that allocates property storage as a second cell (alloc2InYoung)
// and switches the store base over to it mid-fill.
function seven() {
  return {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7};
}

// Wider indirect storage, and the only literal here carrying an undefined.
// undefined in a literal survives only as a hidden-class slot -- JSON drops
// it -- so it is read back explicitly below.
function wide() {
  return {
    a: 0,
    b: 1,
    c: 2,
    d: 3,
    e: 4,
    f: 5,
    g: undefined,
    h: "eight",
    i: false,
    j: null,
    k: -0.25,
    l: 12,
  };
}

// Forty properties: six direct slots and thirty-four indirect ones, so the
// buffer fill runs long past the point where it switches base registers and
// every indirect slot's displacement is distinct. Nothing about the encoding
// is special -- the widest slot here is around 280 bytes in, well inside
// even arm64's scaled store immediate -- what it buys is a wide shape whose
// every slot is checked, next to the narrow ones above.
function fat() {
  return {
    p0: 0, p1: 1, p2: 2, p3: 3, p4: 4, p5: 5, p6: 6, p7: 7,
    p8: 8, p9: 9, p10: 10, p11: 11, p12: 12, p13: 13, p14: 14, p15: 15,
    p16: 16, p17: 17, p18: 18, p19: 19, p20: 20, p21: 21, p22: 22, p23: 23,
    p24: 24, p25: 25, p26: 26, p27: 27, p28: 28, p29: 29, p30: 30, p31: 31,
    p32: 32, p33: 33, p34: 34, p35: 35, p36: 36, p37: 37, p38: 38, p39: 39,
  };
}

// Three more distinct shapes, so the shape table this function's module
// carries has several live entries and each NewObjectWithBuffer site loads
// its own WeakRoot<HiddenClass> at its own index. A site that indexed the
// table wrongly would hand back another shape's class.
function shapeA() {
  return {x: 1};
}
function shapeB() {
  return {y: 1, x: 2};
}
function shapeC() {
  return {x: 1, y: 2, z: 3};
}

// NewObjectWithParent. The parent is whatever the caller passes, which
// selects between the emitter's three cases: an object parent is decoded and
// stored, JS null stores a null parent, and anything else falls back to
// Object.prototype.
function proto(p) {
  return {__proto__: p};
}

// NewObjectWithBufferAndParent, a plain runtime call.
function protoBuf(p) {
  return {__proto__: p, a: 1, b: 2};
}

// InstanceOf. The constructor arrives as a parameter, because naming a
// global would need GetGlobalObject, which still declines.
function isa(v, C) {
  return v instanceof C;
}

// Allocate enough literals to force young-generation collections with some
// of them still live. Without this the whole file fits in one young
// generation and the GC never looks at what the emitted code wrote: an
// object whose hidden class, parent link, property-storage pointer or
// storage size field is wrong is invisible until something scans it. The
// literal is twelve properties wide, so both cells and the link between
// them are scanned, and the kept ones survive long enough to be promoted.
function churn(iters) {
  var keep = null;
  var kept = 0;
  for (var i = 0; i < iters; ++i) {
    var o = {
      a: 0,
      b: 1,
      c: 2,
      d: 3,
      e: 4,
      f: 5,
      g: undefined,
      h: "eight",
      i2: false,
      j: null,
      k: -0.25,
      l: 12,
    };
    if ((i & 4095) === 0) {
      keep = o;
      kept = kept + 1;
    }
  }
  return kept;
}

// The same churn for the single-cell shapes, so the direct-slot fill and the
// empty object also get scanned by a collection.
function churnSmall(iters) {
  var keep = null;
  for (var i = 0; i < iters; ++i) {
    var o = {a: 1, b: "two", c: true, d: null, e: 3.5};
    var e = {};
    if ((i & 4095) === 0)
      keep = i === 0 ? e : o;
  }
  return keep === null ? -1 : 0;
}

var eo = empty();
// CHECK: JIT successfully compiled FunctionID 1, 'empty'
// CHECK0: JIT successfully compiled FunctionID 1, 'empty'
print(
    typeof eo,
    Object.keys(eo).length,
    Object.getPrototypeOf(eo) === Object.prototype);
// CHECK: object 0 true
// CHECK0: object 0 true

var s = small();
// CHECK: JIT successfully compiled FunctionID 2, 'small'
print(s.a, s.b, s.c, s.d, s.e, s.f);
// CHECK: 1 two true null 3.5 6
print(Object.keys(s).join(","));
// CHECK-NEXT: a,b,c,d,e,f

var s7 = seven();
// CHECK: JIT successfully compiled FunctionID 3, 'seven'
print(s7.a, s7.b, s7.c, s7.d, s7.e, s7.f, s7.g);
// CHECK: 1 2 3 4 5 6 7

var w = wide();
// CHECK: JIT successfully compiled FunctionID 4, 'wide'
print(w.a, w.f, w.h, w.i, w.j, w.k, w.l);
// CHECK: 0 5 eight false null -0.25 12
print(w.g === undefined, "g" in w, Object.keys(w).length);
// CHECK-NEXT: true true 12

var ft = fat();
// CHECK: JIT successfully compiled FunctionID 5, 'fat'
print(ft.p0, ft.p5, ft.p39, Object.keys(ft).length);
// CHECK: 0 5 39 40

var a1 = shapeA(), b1 = shapeB(), c1 = shapeC();
// CHECK: JIT successfully compiled FunctionID 6, 'shapeA'
// CHECK: JIT successfully compiled FunctionID 7, 'shapeB'
// CHECK: JIT successfully compiled FunctionID 8, 'shapeC'
print(
    Object.keys(a1).join(","),
    Object.keys(b1).join(","),
    Object.keys(c1).join(","));
// CHECK: x y,x x,y,z
print(a1.x, b1.x, b1.y, c1.x, c1.y, c1.z);
// CHECK-NEXT: 1 2 1 1 2 3

// The three parent cases, in the order the emitter tests them.
var base = shapeC();
var po = proto(base);
// CHECK: JIT successfully compiled FunctionID 9, 'proto'
// CHECK0: JIT successfully compiled FunctionID 9, 'proto'
print(Object.getPrototypeOf(po) === base, po.x, po.z);
// CHECK: true 1 3
// CHECK0: true 1 3
var pn = proto(null);
print(Object.getPrototypeOf(pn));
// CHECK-NEXT: null
// CHECK0-NEXT: null
var pu = proto(17);
print(Object.getPrototypeOf(pu) === Object.prototype);
// CHECK-NEXT: true
// CHECK0-NEXT: true

var pb = protoBuf(base);
// CHECK: JIT successfully compiled FunctionID 10, 'protoBuf'
print(Object.getPrototypeOf(pb) === base, pb.a, pb.b, pb.z);
// CHECK: true 1 2 3

print(isa(base, Object), isa(3, Object), isa(base, Array));
// CHECK: JIT successfully compiled FunctionID 11, 'isa'
// CHECK0: JIT successfully compiled FunctionID 11, 'isa'
// CHECK: true false false
// CHECK0: true false false

print(churn(30000));
// churn's own status line prints between this and the line above, so this
// one cannot be a CHECK-NEXT.
// CHECK: JIT successfully compiled FunctionID 12, 'churn'
// CHECK: 8
print(churnSmall(30000));
// CHECK: JIT successfully compiled FunctionID 13, 'churnSmall'
// CHECK: 0
