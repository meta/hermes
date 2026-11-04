/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-crash-on-error %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit3 && diff %t.int0 %t.jit3
// RUN: %hermes -fno-inline %s > %t.intn && %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error %s > %t.warm && diff %t.intn %t.warm
// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// RUN: %hermes -Xjit=force -Xjit-emit-counters %s 2>&1 >/dev/null | %FileCheck --check-prefix=COUNT %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xdump-jitcode=1 %s 2>&1 | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit

// Property access: the three-tier GetById inline cache (object
// specialization, parent specialization, generic read property cache), the
// PutById runtime path, GetByVal/DelByVal/IsIn, and globals
// (GetGlobalObject/DeclareGlobalVar).
//
// The first four RUN lines are the real check -- interpreter and JIT must
// print the same thing, at -O and -O0, with and without the type asserts.
// The fifth repeats the differential in threshold mode, which is the only
// mode in which the specialized tiers are emitted at all (see below). The
// two -Xdump-jitcode=2 lines pin that the functions under test were in fact
// compiled, so the differential cannot degrade into comparing the
// interpreter against itself. The COUNT line proves emitted code both calls
// (NumCall) and takes the slow call path (NumCallSlow), which it does here
// because a compiled function now reaches `print` through the global object.
//
// Every differential RUN line carries -Xjit-crash-on-error, because nothing
// in this file declines in any of the five modes. The -O0 ones could not
// until the exceptions milestone: the implicit derived constructor contains
// a ThrowIfEmpty, which declined there. It compiles now, and the CHECK0 set
// below pins it.
//
// GLOBALS. Globals themselves are covered here -- `bump` reads and writes a
// module-level `var` and `report` reaches `print` through the global object,
// both from compiled code. `global` itself is pinned here now too: it used
// to decline on the string literals in the top-level code below (`"acc"`,
// `"alpha"`, `"beta"`, `"extra"`), which needed LoadConstString, and this
// header said so and pointed at globals.js for the pin. LoadConstString
// landed in milestone 5, `global` compiles at both -O and -O0, and the pin
// below says so. globals.js remains the file that pins `global` in
// ISOLATION, with no string constant anywhere in it, which is a different
// and still useful guarantee.
//
// THE SPECIALIZED TIERS NEED A WARM CACHE. Both specializations are emitted
// only when the site's ReadPropertyCacheEntry already has
// numGoodChanges == 1, i.e. only when the function ran interpreted before it
// was compiled. Under -Xjit=force every function is compiled before its
// first call, so its caches are cold and only the generic tier is ever
// emitted. That is why the SPEC RUN line uses threshold mode instead: getX
// reads an own property, which selects the object-specialization tier, and
// callSum reads a method off the prototype, which selects the
// parent-specialization tier. Together they are the only coverage in this
// suite of the two tiers that compare HiddenClass lazy JIT ids -- the ids
// pinned through usedHCs by initHCLazyIDMayAlloc, and the only coverage
// anywhere of emit_sh_cp_decode_non_null_preserve_input, which lives inside
// the object-specialization tier and nowhere else. Strings landing did not
// disturb this: threshold mode still runs each function interpreted before
// compiling it, so the caches still warm, and the SPEC lines below still
// match. DO NOT convert this RUN line to -Xjit=force.
//
// NOT covered, and why:
//  - Property names as string values. `o["x"]`, `delete o.x` and `"x" in o`
//    needed LoadConstString, which declined when this file was written, so
//    every by-val site here takes its key from a parameter. That is still
//    how this file is written -- it is what makes the by-val opcodes rather
//    than the by-id ones fire -- but the gap itself is closed: strings.js
//    covers the const-key forms of all three.
//  - getByValWithReceiver / putByValWithReceiver: emitted for `super[k]`
//    and assignment through it, which needs a computed key; the shapes that
//    produce them here also produce opcodes that still decline.
//  - Private names (addOwnPrivateBySym and friends) and
//    defineOwnGetterSetterByVal: emitted from class bodies, which decline on
//    other opcodes. They are ported as plain runtime calls.
//  - defineOwnInDenseArray / defineOwnByIndex: array-literal opcodes, which
//    arrive with the array task.
//  - getOwnBySlotIdx: emitted only for PrLoadInst, which comes from typed
//    (Flow) class IRGen, so it is not reachable from plain JS at all --
//    the same situation as typedLoadParent. putOwnBySlotIdx IS reachable
//    and is covered by `nested` below, but only at -O; at -O0 a literal
//    lowers to NewObject plus DefineOwnById instead.

// A constructor that stores into `this`. Its body is PutByIdLoose twice,
// which is what objects.js could not compile.
function Point(x, y) {
  this.x = x;
  this.y = y;
}
Point.prototype.sum = function () {
  return this.x + this.y;
};

// A monomorphic read site: an own property off a single shape. This is the
// shape that selects the object-specialization tier.
function getX(p) {
  return p.x;
}

// A monomorphic write site.
function setX(p, v) {
  p.x = v;
}

// Reads a method off the prototype and calls it. The read selects the
// parent-specialization tier, whose guard checks the object's class id and
// then the parent's.
function callSum(p) {
  return p.sum();
}

// An accessor installed with defineProperty. The getter call happens inside
// the runtime slow path of the inline cache, which is exactly the point:
// the cache must decline to specialize an accessor slot.
function getAccessor(o) {
  return o.acc;
}

// A site that sees one shape for a while and then the same object with a
// transitioned hidden class. The cache entry is monomorphic when the
// transition happens, so this is what a stale guard would get wrong.
function transition(n) {
  var p = new Point(1, 2);
  var s = 0;
  for (var i = 0; i < n; ++i) {
    s = s + getX(p);
    if (i === 5)
      p.extra = 1;
  }
  return s;
}

// A site fed several distinct shapes: first two (polymorphic), then many
// (megamorphic). Every read must still produce the right value.
function poly(objs, n) {
  var s = 0;
  for (var i = 0; i < n; ++i)
    s = s + getX(objs[i % objs.length]);
  return s;
}

// By-val access with the key arriving as a parameter, so no string constant
// is needed. Covers GetByVal, PutByValLoose, DelByVal and IsIn.
function getKey(o, k) {
  return o[k];
}
function setKey(o, k, v) {
  o[k] = v;
}
function delKey(o, k) {
  return delete o[k];
}
function hasKey(k, o) {
  return k in o;
}

// A nested object literal whose inner value is computed. At -O this is the
// one shape in plain JS that lowers to PutOwnBySlotIdx: two
// NewObjectWithBuffer allocations, then a slot store for the parameter and
// another for the inner object. A flat literal like {u: a, v: 2} does not
// work here -- the optimizer elides the object entirely.
function nested(a) {
  return {x: {y: a}};
}

// Globals: a read, a read-modify-write and a call to a global native
// function, all from compiled code.
var counter = 0;
function bump(n) {
  counter = counter + n;
  return counter;
}
function report(tag, val) {
  print(tag, val);
}

// `super.who()` in a class method. This is the only source of
// LoadParentNoTraps, which was ported with the objects task and had nothing
// that could execute it: every function containing it also contained
// GetById, which declined. It does not any more, so this is that emitter's
// first coverage. The same method is also the file's only
// GetByIdWithReceiver -- a super property read loads the parent and then
// reads through it with `this` as the receiver.
class Base {
  who() {
    return 10;
  }
}
class Derived extends Base {
  who() {
    return super.who() + 5;
  }
}
function callWho(d) {
  return d.who();
}

// Allocation churn through a monomorphic read site, so the classes the
// inline caches refer to have to survive collections.
function churn(n, k) {
  var s = 0;
  for (var i = 0; i < n; ++i) {
    var p = new Point(i, 1);
    p.extra = i;
    s = s + getX(p) + getKey(p, k);
  }
  return s;
}

var p1 = new Point(3, 4);

// Warm the two inline caches before their functions cross the JIT
// threshold. This is what makes the specialized tiers reachable in the SPEC
// run: a site whose cache entry is still cold compiles with the generic tier
// only. The caches that matter are getX's and callSum's own, and in
// threshold mode both functions run interpreted for their first calls, which
// is what fills them. (This comment used to say the warm-up was interpreted
// because `global` declined. `global` compiles in threshold mode now that
// LoadConstString has landed, and it makes no difference: the cache entries
// belong to the read sites inside getX and callSum, not to their caller.
// The SPEC lines at the bottom of this file are what actually verify the
// specialized tiers are still emitted, and they still match.)
for (var w = 0; w < 50; ++w) {
  getX(p1);
  callSum(p1);
}

print(getX(p1), p1.y, callSum(p1));
// The four functions the warm-up and this line reach, in the order
// -Xjit=force compiles them: the constructor (two PutByIdLoose), the
// monomorphic read site, the prototype-method read site, and the anonymous
// `sum` the method read resolves to.
// `global` compiles here too now -- it is the first compile event of the
// run, before any of these.
// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 0, 'global'
// CHECK: JIT successfully compiled FunctionID 1, 'Point'
// CHECK: JIT successfully compiled FunctionID 2, 'getX'
// CHECK: JIT successfully compiled FunctionID 4, 'callSum'
// CHECK: JIT successfully compiled FunctionID 17, ''
// CHECK-NEXT: 3 4 7
// CHECK0: JIT successfully compiled FunctionID 1, 'Point'
// CHECK0: JIT successfully compiled FunctionID 2, 'getX'
// CHECK0: JIT successfully compiled FunctionID 4, 'callSum'
// CHECK0: JIT successfully compiled FunctionID 17, ''
// CHECK0-NEXT: 3 4 7

setX(p1, 30);
print(getX(p1), callSum(p1));
// CHECK: JIT successfully compiled FunctionID 3, 'setX'
// CHECK-NEXT: 30 34
// CHECK0: JIT successfully compiled FunctionID 3, 'setX'
// CHECK0-NEXT: 30 34

// An accessor property. The value the getter returns changes, so a cache
// that specialized the slot would freeze it.
var acc = {n: 1};
Object.defineProperty(acc, "acc", {
  get: function () {
    return this.n * 10;
  },
});
print(getAccessor(acc));
// CHECK: JIT successfully compiled FunctionID 5, 'getAccessor'
// CHECK: JIT successfully compiled FunctionID 22, 'get'
// CHECK-NEXT: 10
// CHECK0: JIT successfully compiled FunctionID 5, 'getAccessor'
// CHECK0: JIT successfully compiled FunctionID 22, 'get'
// CHECK0-NEXT: 10
acc.n = 5;
print(getAccessor(acc));
// CHECK-NEXT: 50
// CHECK0-NEXT: 50

// The shape transition: six reads of the two-property shape, then four of
// the three-property one, all through the same site.
print(transition(10));
// CHECK: JIT successfully compiled FunctionID 6, 'transition'
// CHECK-NEXT: 10
// CHECK0: JIT successfully compiled FunctionID 6, 'transition'
// CHECK0-NEXT: 10

// Polymorphic, then megamorphic.
var two = [new Point(1, 0), {x: 2}];
print(poly(two, 20));
// CHECK: JIT successfully compiled FunctionID 7, 'poly'
// CHECK-NEXT: 30
// CHECK0: JIT successfully compiled FunctionID 7, 'poly'
// CHECK0-NEXT: 30
var many = [
  new Point(1, 0),
  {x: 2},
  {a: 0, x: 3},
  {a: 0, b: 0, x: 4},
  {a: 0, b: 0, c: 0, x: 5},
  {a: 0, b: 0, c: 0, d: 0, x: 6},
  {a: 0, b: 0, c: 0, d: 0, e: 0, x: 7},
  {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, x: 8},
];
print(poly(many, 64));
// CHECK-NEXT: 288
// CHECK0-NEXT: 288

// By-val, with the keys built by the interpreter.
var bag = {alpha: 1};
setKey(bag, "beta", 2);
print(getKey(bag, "alpha"), getKey(bag, "beta"), hasKey("beta", bag));
// CHECK: JIT successfully compiled FunctionID 9, 'setKey'
// CHECK: JIT successfully compiled FunctionID 8, 'getKey'
// CHECK: JIT successfully compiled FunctionID 11, 'hasKey'
// CHECK-NEXT: 1 2 true
// CHECK0: JIT successfully compiled FunctionID 9, 'setKey'
// CHECK0: JIT successfully compiled FunctionID 8, 'getKey'
// CHECK0: JIT successfully compiled FunctionID 11, 'hasKey'
// CHECK0-NEXT: 1 2 true
print(delKey(bag, "beta"), hasKey("beta", bag), getKey(bag, "beta"));
// CHECK: JIT successfully compiled FunctionID 10, 'delKey'
// CHECK-NEXT: true false undefined
// CHECK0: JIT successfully compiled FunctionID 10, 'delKey'
// CHECK0-NEXT: true false undefined

print(nested(5).x.y);
// CHECK: JIT successfully compiled FunctionID 12, 'nested'
// CHECK-NEXT: 5
// CHECK0: JIT successfully compiled FunctionID 12, 'nested'
// CHECK0-NEXT: 5

// Globals, read and written from compiled code, and a native call reached
// through the global object.
print(bump(2), bump(3), counter);
// CHECK: JIT successfully compiled FunctionID 13, 'bump'
// CHECK-NEXT: 2 5 5
// CHECK0: JIT successfully compiled FunctionID 13, 'bump'
// CHECK0-NEXT: 2 5 5
report("counter", counter);
// CHECK: JIT successfully compiled FunctionID 14, 'report'
// CHECK-NEXT: counter 5
// CHECK0: JIT successfully compiled FunctionID 14, 'report'
// CHECK0-NEXT: counter 5

print(callWho(new Derived()));
// The implicit derived constructor contains a ThrowIfEmpty for the TDZ check
// on the base class binding, which is why it used to compile at -O but not
// at -O0. Both modes compile it now. The two 'who' methods and the call site
// compile in both modes; 21 is Derived's, the one holding the
// LoadParentNoTraps.
// CHECK: JIT successfully compiled FunctionID 20, 'Derived'
// CHECK: JIT successfully compiled FunctionID 18, 'Base'
// CHECK: JIT successfully compiled FunctionID 15, 'callWho'
// CHECK: JIT successfully compiled FunctionID 21, 'who'
// CHECK: JIT successfully compiled FunctionID 19, 'who'
// CHECK-NEXT: 15
// CHECK0: JIT successfully compiled FunctionID 20, 'Derived'
// CHECK0: JIT successfully compiled FunctionID 18, 'Base'
// CHECK0: JIT successfully compiled FunctionID 15, 'callWho'
// CHECK0: JIT successfully compiled FunctionID 21, 'who'
// CHECK0: JIT successfully compiled FunctionID 19, 'who'
// CHECK0-NEXT: 15

print(churn(20000, "extra"));
// CHECK: JIT successfully compiled FunctionID 16, 'churn'
// CHECK-NEXT: 399980000
// CHECK0: JIT successfully compiled FunctionID 16, 'churn'
// CHECK0-NEXT: 399980000

// The two specialized tiers, emitted only in threshold mode. getX's cache
// is a plain own-property hit, so its site takes the object-specialization
// tier; callSum reads a method off Point.prototype, so its site takes the
// parent-specialization tier, whose guard checks the object's class id and
// then the parent's. Both compare 16-bit HiddenClass lazy JIT ids, which is
// what pins those classes in usedHCs.
//
// Each tier comment is anchored between its own function's code label and
// its own completion message. A function's dump is contiguous and the SPEC
// checks are ordered, so each match is forced into that window and a tier
// emitted for some other function cannot satisfy the check. That matters:
// three sites in this file take the object tier, and unanchored, any of
// them would do -- including one in a function whose specialization is
// incidental to what this test is about.
// SPEC: getX:
// SPEC: // Get from object specialization
// SPEC: JIT successfully compiled FunctionID 2, 'getX'
// SPEC: callSum:
// SPEC: // Get from parent specialization
// SPEC: JIT successfully compiled FunctionID 4, 'callSum'

// NumCall counts every call the emitted code made and NumCallSlow every one
// that could not go straight to a compiled function -- `print` and the other
// natives reached through the global object. Any nonzero value proves the
// path ran.
// COUNT: JIT counters:
// COUNT: NumCall: {{[1-9][0-9]*}}
// COUNT: NumCallSlow: {{[1-9][0-9]*}}
