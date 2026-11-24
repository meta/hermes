/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s > %t.force && diff %t.int %t.force
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-asserts -Xjit-emit-type-asserts %s > %t.forcea && diff %t.int %t.forcea
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error %s > %t.warm && diff %t.int %t.warm
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-asserts -Xjit-emit-type-asserts %s > %t.warma && diff %t.int %t.warma
// RUN: %hermes -fno-inline %s | %FileCheck --match-full-lines %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// UNSUPPORTED under HERMESVM_SANITIZE_HANDLES (the `handle_san` lit feature):
// there the runtime boxes every number, so the emitter declines the
// inline-value encodings this file is about with a bare branch, and the build
// moves the heap on every allocation, which makes the churn below cost over 13
// minutes per RUN line on a MallocGC tree.

// Every SmallHermesValue shape, stored through both inline write tiers into
// an OLD object and an OLD array, with young-generation collections in
// between and a read-back afterwards.
//
// WHY THIS FILE EXISTS. Under HERMESVM_BOXED_DOUBLES (both the HV32 and the
// BOXED trees) a heap slot does not hold the HermesValue the emitter carries
// in a register: it holds a SmallHermesValue, which the store site has to
// ENCODE first (Emitter::emitSafeStoreOrSlow, through
// emit_shv_encode_or_slow). The encoding is per-tag, and one case
// allocates: a double whose bits do not survive the compression is boxed
// in a BoxedDouble, which the tiers bump-allocate in the young generation
// (only a full young generation sends it to the runtime helper, which
// allocates it there). Every arm of that dispatch is
// exercised below:
//
//   undefined / null / true / false   compressible non-numbers
//   a small integer, 3.5, -0.25       doubles that compress
//   0.1, 1e300                        doubles that do NOT: inline BoxedDouble
//   a string, an object, a bigint     the three pointer tags
//   a symbol                          the symbol tag
//
// In the default HV64 build the encode is a no-op and this is simply another
// behavioral run of the two tiers, which is the point: the same values must
// come back in every mode.
//
// The values are read back only after `churn` has collected the young
// generation several times, so a pointer stored into the old object without
// its card being dirtied is collected and the read-back sees garbage -- the
// same hazard putbyid-inline.js is built around, here with every tag rather
// than one.
//
// -fno-inline on every RUN line, for the same reason as putbyid-inline.js:
// the stored object must not stay live in a caller's frame register.
//
// Both threshold-mode and force-mode RUN lines are present because the two
// tiers need different warm-up: the PutById tier is emitted only when the
// site's write cache already names a hidden class (threshold mode), while
// the PutByVal tier reads no cache and is emitted under -Xjit=force too.

var sym = Symbol("s");
var big = 1234567890123456789012345678901234567890n;

// Allocates garbage into a global, so the objects genuinely escape and the
// young generation genuinely fills up and collects.
var sink = [null];
function churn(n) {
  for (var i = 0; i < n; ++i)
    sink[0] = {j0: i, j1: i + 1, j2: i + 2, j3: i + 3, j4: i + 4, j5: i + 5};
  return sink[0].j5;
}

// Eight properties, so that `g` and `p` land in indirect storage with six
// direct slots: both slot forms of the PutById tier are reachable.
function makeWide(k) {
  return {a: k, b: k + 1, c: k + 2, d: k + 3, e: k + 4, f: k + 5, g: k + 6,
          p: null};
}

// The monomorphic write sites under test. `storeProp` is the PutById tier,
// `storeElem` the PutByVal fast array store.
function storeProp(obj, v) {
  obj.p = v;
}
function storeElem(arr, i, v) {
  arr[i] = v;
}

// A fresh object allocated in this frame, so that after the call the only
// reference to it is the slot that was just written.
function storeFreshProp(obj, i) {
  obj.p = {n: i, m: i * 3};
}
function storeFreshElem(arr, i) {
  arr[0] = {n: i, m: i * 3};
}

function show(v) {
  if (typeof v === "symbol")
    return String(v);
  if (typeof v === "bigint")
    return v.toString() + "n";
  if (v !== null && typeof v === "object")
    return "obj:" + v.tag;
  return String(v);
}

// Promote `holder` and `arr` into the old generation: they stay reachable
// from the global object across the collections `churn` causes.
var holder = makeWide(100);
var arr = [0, 1, 2, 3];
churn(60000);

// Warm both sites so they are compiled with a populated write cache.
for (var i = 0; i < 20; ++i) {
  storeProp(holder, i);
  storeElem(arr, 1, i);
}

// Every shape, through both tiers, with a young-generation collection after
// each store and the value read back only after that collection.
var shapes = [undefined, null, true, false, 42, 3.5, -0.25, 0.1, 1e300,
              "str", {tag: 7}, big, sym];
var propOut = [];
var elemOut = [];
for (var i = 0; i < shapes.length; ++i) {
  storeProp(holder, shapes[i]);
  storeElem(arr, 1, shapes[i]);
  churn(20000);
  propOut.push(show(holder.p));
  elemOut.push(show(arr[1]));
}
print("prop", propOut.join("|"));
print("elem", elemOut.join("|"));
// CHECK-LABEL:prop undefined|null|true|false|42|3.5|-0.25|0.1|1e+300|str|obj:7|1234567890123456789012345678901234567890n|Symbol(s)
// CHECK-NEXT:elem undefined|null|true|false|42|3.5|-0.25|0.1|1e+300|str|obj:7|1234567890123456789012345678901234567890n|Symbol(s)

// The old-to-young card path, for both tiers: the stored object is allocated
// in the callee's frame and read back only after two young-gen collections.
var sum = 0;
for (var i = 0; i < 8; ++i) {
  storeFreshProp(holder, i);
  storeFreshElem(arr, i);
  churn(45000);
  sum += holder.p.n + holder.p.m + arr[0].n + arr[0].m;
}
print("fresh", sum);
// CHECK-NEXT:fresh 224

// A non-compressible double stored repeatedly into an old object: the
// tier boxes each one in the young generation and dirties the slot's
// card, and the box must survive the collections that follow -- a missed
// card would leave the old slot pointing at a box a young collection
// freed.
var acc = 0;
for (var i = 0; i < 8; ++i) {
  storeProp(holder, 0.1 + i);
  storeElem(arr, 2, 1e300 + i);
  churn(45000);
  acc += holder.p + arr[2] / 1e300;
}
print("boxed", acc.toFixed(3));
// CHECK-NEXT:boxed 36.800

// The promoted object's other properties, and the array's other elements,
// must be untouched by all of this.
print("fields", holder.a, holder.b, holder.c, holder.d, holder.e, holder.f,
      holder.g);
print("elems", show(arr[1]), arr[3], arr.length);
// CHECK-NEXT:fields 100 101 102 103 104 105 106
// CHECK-NEXT:elems Symbol(s) 3 4
