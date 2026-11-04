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

// The heap-value-mode matrix. Everything here is chosen because the code the
// emitter produces for it DIFFERS between the three HERMESVM_HEAP_HV_MODE
// builds, so running this one differential in each build is what gives the
// mode-specific emitter branches their coverage:
//
//   HEAP_HV_64      (default)  neither macro
//   HEAP_HV_PREFER32          HERMESVM_COMPRESSED_POINTERS +
//                             HERMESVM_BOXED_DOUBLES + HERMESVM_CONTIGUOUS_HEAP
//   HEAP_HV_BOXED             HERMESVM_BOXED_DOUBLES only
//
// What is covered, and where it lives:
//
// 1. Emit_sh_shv_decode (JitEmitter-internal.cpp) -- the SmallHermesValue to
//    HermesValue decode inlined into the GetById cache. It emits nothing in
//    HV64 and a four-way dispatch under boxed doubles: CompressedHV64,
//    pointer (String/BigInt/Object), Symbol, and BoxedDouble. `readAll`
//    reads a property of every one of those tags, out of both a direct slot
//    and an indirect one, so all four arms run.
// 2. emit_load_cp / emit_sh_cp_decode_non_null -- the narrow loads of
//    `clazz` and `propStorage` plus the heap-base add. Every property read
//    below uses them under HV32; they are plain 64-bit loads in the other
//    two builds.
// 3. newObjectWithBufferSlow (JitEmitter-object.cpp) -- reached only when
//    SmallHermesValue::canInlineDouble() rejects a literal's double, which
//    never happens in HV64 (every double inlines there) but does in both
//    boxed builds. `litSlow`'s 0.1 and 1e300 are the trigger; `litFast`,
//    whose doubles are all representable, stays on the inline buffer path in
//    every build. This is the one place where the emitter picks a different
//    code SHAPE per mode rather than a different encoding of one shape.
// 4. emit_sh_cp_decode -- the nullable decode, which needs an extra zero
//    temp on x86-64 where arm64 uses xzr. `super.tag()` emits the
//    LoadParentNoTraps that calls it.
// 5. emit_shv_string / emit_sh_cp_encode_non_null -- the object-literal
//    buffer tagging a string into a slot, in `litFast`/`litSlow`.
//
// The churn loop runs all of the above under young-generation collections,
// so a compressed pointer encoded wrong also gets a chance to be caught by
// the GC and not only by a wrong value.
//
// NO STRING OPERATIONS IN COMPILED CODE. LoadConstString and AddS still
// decline (milestone 5), so a compiled function here cannot build a string.
// Every function under test therefore returns numbers or an array, and all
// the formatting happens in top-level code, which the interpreter runs.
// String VALUES are still covered -- they reach the emitter through object
// literal buffers ("s" in litFast/litSlow) and through the String tag of the
// SHV decode, which is what matters for the modes.
//
// The last two RUN lines pin that the functions under test were compiled, so
// the differential cannot degrade into comparing the interpreter to itself.
//
// A GAP, STATED HONESTLY. This file pins WHAT the code computes in each
// build, not WHICH branch computed it. Nothing here would notice if
// litSlow silently stopped taking newObjectWithBufferSlow in a boxed build:
// the inline path returns an equal object, so the differential would still
// pass. A standing per-mode assertion would need lit to expose the
// heap-value mode as a feature, so that a RUN line could be conditioned on
// it the way this file's jit requirement is, and lit does not. (Do not
// spell that directive out in prose here -- lit scans the whole file for
// the keyword and tries to parse the rest of the line.) Branch selection was
// therefore verified by hand -- counting _interpreter_create_object_from_buffer
// in -Xdump-jitcode=1 output in each of the three builds -- and anyone who
// needs that guarantee should redo it the same way rather than assume this
// file covers it.

var sym = Symbol("s");
var big = 1234567890123456789012345678901234567890n;

// An object with a property of every SmallHermesValue tag, and enough
// properties that the tail ones live in indirect storage. The literal is
// past the inline-allocation property limit, so this is the buffer slow path
// in every build; the trailing assignments add the computed values.
function build(sym, big) {
  var o = {
    tInt: 42,
    tBool: true,
    tNull: null,
    tUndef: undefined,
    tDblInline: 3.5,
    tDblBoxed: 0.1,
    tStr: "str",
    f1: 1,
    f2: 2,
    f3: 3,
    f4: 4,
    f5: 5,
    f6: 6,
    iDbl: 1e300,
    iStr: "tail",
  };
  o.tObj = {inner: 7};
  o.tSym = sym;
  o.tBig = big;
  o.iObj = {inner: 9};
  o.iSym = sym;
  return o;
}

// Every read below goes through a GetById cache in compiled code, which is
// where the SHV decode is inlined. Returning an array keeps the strings out.
function readAll(o) {
  return [
    o.tInt,
    o.tBool,
    o.tNull,
    o.tUndef,
    o.tDblInline,
    o.tDblBoxed,
    o.tStr,
    o.tObj.inner,
    o.tSym,
    o.tBig,
    o.iDbl,
    o.iStr,
    o.iObj.inner,
    o.iSym,
  ];
}

// Every double here is representable as an inline SmallHermesValue in every
// build, so this stays on newObjectWithBuffer's inline path everywhere.
function litFast() {
  return {a: 1, b: 2.5, c: -0.25, d: "s", e: true, f: null};
}

// 0.1 and 1e300 are not inline-representable under boxed doubles, so this
// takes newObjectWithBufferSlow in the two boxed builds and the inline path
// in HV64.
function litSlow() {
  return {a: 0.1, b: 1e300, c: "s", d: true, e: null, f: 2.5};
}

class Base {
  tag() {
    return 1;
  }
}
class Derived extends Base {
  // super.tag() is LoadParentNoTraps, i.e. the nullable compressed-pointer
  // decode. The result is a number so the method body needs no strings.
  tag() {
    return super.tag() + 10;
  }
}

function churn(n, sym, big) {
  var acc = 0;
  var last = null;
  for (var i = 0; i < n; ++i) {
    var f = litFast();
    var s = litSlow();
    acc += f.a + f.b + s.a + s.f;
    last = build(sym, big);
  }
  return acc + last.f6;
}

var o = build(sym, big);
var r = null;
for (var i = 0; i < 3000; ++i)
  r = readAll(o);
print(r.map(String).join("|"));
// `build` compiles only at -O: at -O0 an object literal lowers to NewObject
// plus a DefineOwnById per key, and every key is a LoadConstString. The same
// is true of litFast/litSlow below, and `Derived` additionally declines at
// -O0 on the ThrowIfEmpty guarding its own class binding. readAll, the two
// `tag`s and churn compile at both levels, and churn is what drives the
// literal and build paths in the -O0 configuration.
// CHECK: JIT successfully compiled FunctionID 1, 'build'
// CHECK: JIT successfully compiled FunctionID 2, 'readAll'
// CHECK: 42|true|null|undefined|3.5|0.1|str|7|Symbol(s)|1234567890123456789012345678901234567890|1e+300|tail|9|Symbol(s)
// CHECK0: JIT successfully compiled FunctionID 2, 'readAll'
// CHECK0: 42|true|null|undefined|3.5|0.1|str|7|Symbol(s)|1234567890123456789012345678901234567890|1e+300|tail|9|Symbol(s)

var lf = litFast();
var ls = litSlow();
print(lf.a, lf.b, lf.c, lf.d, lf.e, lf.f);
// CHECK: JIT successfully compiled FunctionID 3, 'litFast'
// CHECK: JIT successfully compiled FunctionID 4, 'litSlow'
// CHECK: 1 2.5 -0.25 s true null
// CHECK0: 1 2.5 -0.25 s true null
print(ls.a, ls.b, ls.c, ls.d, ls.e, ls.f);
// CHECK: 0.1 1e+300 s true null 2.5
// CHECK0: 0.1 1e+300 s true null 2.5

// FunctionID 9 is Derived's `tag`, the one containing LoadParentNoTraps.
var d = new Derived();
print(d.tag());
// CHECK: JIT successfully compiled FunctionID 9, 'tag'
// CHECK: 11
// CHECK0: JIT successfully compiled FunctionID 9, 'tag'
// CHECK0: 11

print(churn(4000, sym, big).toFixed(3));
// CHECK: JIT successfully compiled FunctionID 5, 'churn'
// CHECK: 24406.000
// CHECK0: JIT successfully compiled FunctionID 5, 'churn'
// CHECK0: 24406.000
