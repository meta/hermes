/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.force && diff %t.int %t.force
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-asserts -Xjit-emit-type-asserts %s > %t.forcea && diff %t.int %t.forcea
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.warm && diff %t.int %t.warm
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-asserts -Xjit-emit-type-asserts %s > %t.warma && diff %t.int %t.warma
// RUN: %hermes -fno-inline %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit

// The GetByVal inline load tiers over a MIXED workload, the read-side twin
// of putbyval-inline.js: the same read sites see dense arrays, several
// typed-array kinds, holes, out-of-range indices and non-index keys, and
// the whole run is diffed against the interpreter.
//
// ARCHITECTURE-INDEPENDENT. This file checks values, never instructions,
// so it runs on every backend, including arm64, whose GetByVal has no
// inline tier at all and answers every read below through its helper. That
// the tiers are emitted, and what they consist of, is pinned separately
// and per backend by x86-64/getbyval-inline-emitted.js; which tier a site
// gets is pinned by the x86-64/recompile-getval-*.js policy tests.
//
// TWO KINDS OF SITE are deliberately mixed here. `readAt` is polymorphic:
// it sees dense arrays, Arguments objects, plain objects and several typed
// arrays, so its site poisons and keeps a single (first-seen) typed-array
// tier with the JSArray tier chained behind it -- the duo/poisoned shapes,
// driven by real traffic rather than by a dump pin. The per-kind readers
// below it are monomorphic, each specialized on its own kind, which is
// what makes their out-of-bounds and hole behavior a test of the INLINE
// path rather than of the helper.
//
// EVERY read goes through a variable key, never a literal: ISel.cpp lowers
// a uint8 literal number key to GetByIndex, a different opcode with no
// inline tier, so a literal-keyed twin of any case here would test
// nothing. The BC prefix pins that in the bytecode.

function readAt(a, i) {
  return a[i];
}
function readArr(a, i) {
  return a[i];
}
function readI8(a, i) {
  return a[i];
}
function readU8C(a, i) {
  return a[i];
}
function readI32(a, i) {
  return a[i];
}
function readU32(a, i) {
  return a[i];
}
function readF32(a, i) {
  return a[i];
}
function readF64(a, i) {
  return a[i];
}

// Joins a reader's answers over a range of indices into one line, so a
// single wrong element is visible in the diff at the index it happened at.
function scan(name, rd, a, from, to) {
  var s = name;
  for (var i = from; i <= to; ++i) s += " " + rd(a, i);
  print(s);
}

// ---- dense JSArray, the unconditional tier ----
// A monomorphic array site, warmed past the recompile threshold so that it
// is running its final body, read straight through and then off both ends.
var dense = [10, 20, 30, 40];
for (var i = 0; i < 100; ++i) readArr(dense, 1);
scan("dense", readArr, dense, -1, 5);

// ---- holes and the prototype chain ----
// A hole is NOT undefined: the read must resolve through the prototype,
// which here answers with a data property at one index and an ACCESSOR at
// another, and the accessor must actually run. Out-of-range indices reach
// the same two kinds of prototype property. An inline tier that answered a
// hole or an out-of-range index with `undefined` instead of declining
// would lose all four.
var accessorHits = 0;
var proto = {};
Object.defineProperty(proto, "1", {value: "protoData1", configurable: true});
Object.defineProperty(proto, "2", {
  get: function () {
    ++accessorHits;
    return "protoAccessor2";
  },
  configurable: true,
});
Object.defineProperty(proto, "9", {value: "protoData9", configurable: true});
Object.defineProperty(proto, "10", {
  get: function () {
    ++accessorHits;
    return "protoAccessor10";
  },
  configurable: true,
});
var holey = [0, /* hole */, /* hole */, 3];
Object.setPrototypeOf(holey, proto);
for (var i = 0; i < 100; ++i) readArr(holey, 0);
scan("holey", readArr, holey, 0, 11);
print("accessorHits", accessorHits);

// ---- typed arrays, one monomorphic site per kind ----
// Each view starts at a NONZERO byte offset, so the tier's `offset_` term
// is exercised; each is read in range, at both boundaries, one past the
// end, and far out of range. An out-of-bounds typed-array read is
// `undefined` -- not a decline, and not a prototype lookup -- so these
// lines are what the tier's inline `undefined` path has to reproduce.
var tbuf = new ArrayBuffer(128);

var tI8 = new Int8Array(tbuf, 8, 4);
tI8[0] = -1;
tI8[1] = 127;
tI8[2] = -128;
tI8[3] = 42;
for (var i = 0; i < 100; ++i) readI8(tI8, 0);
scan("I8", readI8, tI8, -1, 5);
print("I8 far", readI8(tI8, 1000000), readI8(tI8, 4294967295));

var tU8C = new Uint8ClampedArray(tbuf, 16, 4);
tU8C[0] = 300;
tU8C[1] = -5;
tU8C[2] = 127.5;
tU8C[3] = 255;
for (var i = 0; i < 100; ++i) readU8C(tU8C, 0);
scan("U8C", readU8C, tU8C, -1, 5);

var tI32 = new Int32Array(tbuf, 24, 4);
tI32[0] = -2147483648;
tI32[1] = 2147483647;
tI32[2] = 0;
tI32[3] = -1;
for (var i = 0; i < 100; ++i) readI32(tI32, 0);
scan("I32", readI32, tI32, -1, 5);

var tU32 = new Uint32Array(tbuf, 40, 4);
tU32[0] = 4294967295;
tU32[1] = 2147483648;
tU32[2] = 2147483647;
tU32[3] = 0;
for (var i = 0; i < 100; ++i) readU32(tU32, 0);
scan("U32", readU32, tU32, -1, 5);

var tF32 = new Float32Array(tbuf, 56, 4);
tF32[0] = 1.5;
tF32[1] = -0;
tF32[2] = Infinity;
tF32[3] = NaN;
for (var i = 0; i < 100; ++i) readF32(tF32, 0);
scan("F32", readF32, tF32, -1, 5);
print("F32 negzero", 1 / readF32(tF32, 1), Object.is(readF32(tF32, 3), NaN));

var tF64 = new Float64Array(tbuf, 72, 4);
tF64[0] = 1.5;
tF64[1] = -0;
tF64[2] = -Infinity;
tF64[3] = NaN;
for (var i = 0; i < 100; ++i) readF64(tF64, 0);
scan("F64", readF64, tF64, -1, 5);
print("F64 negzero", 1 / readF64(tF64, 1), Object.is(readF64(tF64, 3), NaN));

// ---- non-index keys through an already specialized site ----
// A negative, a fractional, a >= 2^32 and a string key each take a
// different exit out of the key conversion, and -0 is the one that must
// NOT decline (it is the index 0). Every one of them has to match the
// interpreter, whichever way that turns out -- the typed-array site is
// used here because its tier's key guard is the one with no 0xFFFFFFFF
// special case.
print(
  "I32 keys",
  readI32(tI32, -1),
  readI32(tI32, 1.5),
  readI32(tI32, 4294967296),
  readI32(tI32, "1"),
  readI32(tI32, "nope"),
  readI32(tI32, -0),
  readI32(tI32, NaN),
);
print("I32 named", readI32(tI32, "length"), readI32(tI32, "byteOffset"));

// ---- one polymorphic site ----
// `readAt` sees a dense array, a hole-bearing array over the prototype
// above, an Arguments object (a different CellKind that shares ArrayImpl's
// storage layout and must therefore decline), a plain object with
// index-like own properties, and TWO typed-array kinds -- which poisons
// the site: it keeps whichever kind it saw first and answers the other
// through the helper. Every answer must still be exact.
function mkargs() {
  return arguments;
}
var ao = mkargs("a", "b", "c");
var plain = {0: "p0", 1: "p1"};
var mixed = 0;
for (var i = 0; i < 120; ++i) {
  mixed += readAt(dense, i & 3);
  readAt(holey, i & 3);
  readAt(ao, i & 3);
  readAt(plain, i & 1);
  readAt(tI32, i & 3);
  readAt(tF64, i & 3);
}
print("mixed", mixed);
scan("poly dense", readAt, dense, 0, 4);
scan("poly holey", readAt, holey, 0, 4);
scan("poly args", readAt, ao, -1, 3);
scan("poly plain", readAt, plain, 0, 2);
scan("poly I32", readAt, tI32, 0, 4);
scan("poly F64", readAt, tF64, 0, 4);
print("accessorHits", accessorHits);

// CHECK-LABEL:dense undefined 10 20 30 40 undefined undefined
// CHECK-NEXT:holey 0 protoData1 protoAccessor2 3 undefined undefined undefined undefined undefined protoData9 protoAccessor10 undefined
// CHECK-NEXT:accessorHits 2
// CHECK-NEXT:I8 undefined -1 127 -128 42 undefined undefined
// CHECK-NEXT:I8 far undefined undefined
// CHECK-NEXT:U8C undefined 255 0 128 255 undefined undefined
// CHECK-NEXT:I32 undefined -2147483648 2147483647 0 -1 undefined undefined
// CHECK-NEXT:U32 undefined 4294967295 2147483648 2147483647 0 undefined undefined
// CHECK-NEXT:F32 undefined 1.5 0 Infinity NaN undefined undefined
// CHECK-NEXT:F32 negzero -Infinity true
// CHECK-NEXT:F64 undefined 1.5 0 -Infinity NaN undefined undefined
// CHECK-NEXT:F64 negzero -Infinity true
// CHECK-NEXT:I32 keys undefined undefined undefined 2147483647 undefined -2147483648 undefined
// CHECK-NEXT:I32 named 4 24
// CHECK-NEXT:mixed 3000
// CHECK-NEXT:poly dense 10 20 30 40 undefined
// CHECK-NEXT:poly holey 0 protoData1 protoAccessor2 3 undefined
// CHECK-NEXT:poly args undefined a b c undefined
// CHECK-NEXT:poly plain p0 p1 undefined
// CHECK-NEXT:poly I32 -2147483648 2147483647 0 -1 undefined
// CHECK-NEXT:poly F64 1.5 0 -Infinity NaN undefined
// CHECK-NEXT:accessorHits 33

// Anti-GetByIndex, anti-vacuity pin: every reader must hold a GetByVal.
// BC-LABEL:Function<readAt>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readArr>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readI8>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readU8C>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readI32>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readU32>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readF32>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readF64>({{.*}}
// BC: GetByVal
