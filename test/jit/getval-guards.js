/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xhermes-internal-test-methods -fno-inline %s > %t.int
// RUN: %hermes -Xhermes-internal-test-methods -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -Xhermes-internal-test-methods -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --match-full-lines --check-prefix=TA %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit

// The inline GetByVal load tiers' guards, checked the way
// test/jit/x86-64/taval-guards.js checks the store tier's: an actual
// stdout diff between the interpreter and the JIT, not a FileCheck OUT
// pin, because what matters here is that every guard exit reaches the
// exact same runtime behavior the interpreter has -- prototype lookups,
// accessor calls and all -- not any particular instruction sequence. That
// is pinned separately, per backend, by getbyval-inline-emitted.js.
//
// The typed-array section at the end is the exception: it carries a
// full-line TA prefix as well, because its answers (`undefined` for an
// out-of-bounds or detached read) are produced INLINE by the tier rather
// than by the helper, so a bounds-check defect there should fail a NAMED
// check rather than only perturb a diff.
//
// HermesInternal.detachArrayBuffer needs -Xhermes-internal-test-methods
// (see test/hermes/typedarray-detached.js); every RUN line that runs the
// script runs the detach case below, so each carries the flag.
//
// ARCHITECTURE-INDEPENDENT. arm64 has no JSArray load tier yet; every read
// below still goes through its bare helper call, which the interpreter
// diff exercises identically. The point of this file is the read
// SEMANTICS at a GetByVal site, not any inline tier's presence.
//
// EVERY read below goes through a variable key (`readAt`'s `i` parameter,
// or `arguments[idx]`'s `idx`), never a literal: ISel.cpp only lowers a
// uint8 LITERAL number key to GetByIndex, so a literal-keyed "twin" of any
// case here would silently test GetByIndex instead and exercise none of
// this tier. -fno-inline keeps the callee body real code rather than
// something the inliner could constant-fold the key through.

function readAt(a, i) {
  return a[i];
}

// Holes over a prototype carrying BOTH an indexed data property and an
// indexed accessor, mixed with out-of-range indices reaching the same
// two kinds of prototype property. `arr` is a fast dense JSArray with
// two holes (indices 1 and 2, from the elided elements) and length 4;
// its prototype answers index 1 with a data property, index 2 with an
// accessor, and two further, out-of-range indices (10 and 11) the same
// way. Every case below must read exactly what the interpreter's
// getOwnComputed()/prototype-chain path would:
//   - index 0, 3: own dense elements -- straight-through inline reads;
//   - index 1: a HOLE that finds a prototype DATA property;
//   - index 2: a HOLE that finds a prototype ACCESSOR;
//   - indices 4-9, 12: OUT OF RANGE with no matching prototype property
//     -- undefined;
//   - index 10: OUT OF RANGE, finds a prototype DATA property;
//   - index 11: OUT OF RANGE, finds a prototype ACCESSOR.
var proto = {};
Object.defineProperty(proto, "1", {
  value: "protoData1",
  enumerable: true,
  configurable: true,
});
Object.defineProperty(proto, "2", {
  get: function () {
    return "protoAccessor2";
  },
  enumerable: true,
  configurable: true,
});
Object.defineProperty(proto, "10", {
  value: "protoData10",
  enumerable: true,
  configurable: true,
});
Object.defineProperty(proto, "11", {
  get: function () {
    return "protoAccessor11";
  },
  enumerable: true,
  configurable: true,
});

var arr = [10, /* hole */, /* hole */, 40];
Object.setPrototypeOf(arr, proto);

for (var i = 0; i < 13; ++i) {
  print("dense", i, readAt(arr, i));
}

// Non-uint32 keys: negative, fractional, exactly 2^32 (one past the
// largest representable uint32), a string that never parses as a number,
// and -0 (which the array index conversion accepts as index 0 -- unlike
// the others, this one must NOT decline). Every one of these must match
// the interpreter exactly, whichever way that turns out.
print("neg", readAt(arr, -1));
print("frac", readAt(arr, 1.5));
print("2^32", readAt(arr, 4294967296));
print("str", readAt(arr, "abc"));
print("neg0", readAt(arr, -0));

// Arguments objects: a different CellKind that shares ArrayImpl's storage
// layout, so the tier's exact-JSArrayKind guard must decline it to the
// helper rather than reading it inline -- but the VALUES must still come
// out right, including an out-of-range read and a negative one.
//
// The object has to reach a genuine GetByVal site to test that guard at
// all: reading `arguments[idx]` directly inside the function that owns
// that `arguments` binding lowers to GetArgumentsPropByValLoose, a
// different opcode this tier does not touch. Materializing the Arguments
// object in one function (`mkargs`) and reading it back through the
// existing dynamic-key `readAt` helper is what actually drives a
// GetByVal site with an Arguments source.
function mkargs() {
  return arguments;
}
var ao = mkargs(10, 20, 30);
for (var i = -1; i <= 4; ++i) {
  print("args", i, readAt(ao, i));
}

// Frozen and sealed dense arrays: freezing/sealing moves the array to a
// frozen/sealed hidden class (and a subsequent defineProperty would make its
// index properties slow, though freezing does not by itself), but the read
// side's tier checks neither -- only the element being non-empty and in
// range -- so both must read exactly like an ordinary dense array,
// including an out-of-range read that must still decline correctly.
var frozen = [1, 2, 3];
Object.freeze(frozen);
print("frozen", readAt(frozen, 1), readAt(frozen, 5));

var sealed = [4, 5, 6];
Object.seal(sealed);
print("sealed", readAt(sealed, 2), readAt(sealed, 9));

// ---- the typed-array load tier's own guards ----
//
// Each of these runs through its OWN function, warmed past the recompile
// threshold (64, pinned on the RUN lines) with 100 separate top-level
// calls so that the probes afterwards genuinely execute the specialized
// body. readAt above is deliberately not reused: it is already polymorphic
// by this point, so it would test whichever kind it happened to see first.
function readI32(a, i) {
  return a[i];
}
function readF16(a, i) {
  return a[i];
}
function readB64(a, i) {
  return a[i];
}

// A 4-element Int32Array at a NONZERO byte offset, with a KNOWN nonzero
// word planted immediately past its end. Out of bounds on a typed array is
// `undefined` -- not a decline, not a prototype lookup -- and that is the
// tier's own inline answer, so the just-past-end index is where an
// off-by-one bounds check (`<=` instead of `<`) would show up: it would
// read the planted word and print 287454020 instead of undefined.
var tbuf = new ArrayBuffer(64);
var tguard = new Int32Array(tbuf, 8, 4);
tguard[0] = 10;
tguard[3] = 40;
new Int32Array(tbuf, 24, 1)[0] = 0x11223344;
for (var i = 0; i < 100; ++i) readI32(tguard, 0);
print("ta in", readI32(tguard, 0), readI32(tguard, 3));
print("ta past", readI32(tguard, 4));
print("ta oob", readI32(tguard, 5), readI32(tguard, 1000000));
print("ta huge", readI32(tguard, 4294967295), readI32(tguard, 4294967296));
print("ta neg", readI32(tguard, -1), readI32(tguard, -0));

// Detached: a separate, never-modified Int32Array read through the same
// warmed, specialized site. A detached buffer has no data at all, so every
// index reads `undefined` -- including index 0, which was in bounds a
// moment earlier.
var tdet = new Int32Array(4);
tdet[0] = 77;
for (var i = 0; i < 100; ++i) readI32(tdet, 0);
print("det before", readI32(tdet, 0), tdet.length);
HermesInternal.detachArrayBuffer(tdet.buffer);
print("det after", readI32(tdet, 0), readI32(tdet, 3), tdet.length);

// Kinds the load tier does NOT support (see
// isJitSupportedTypedArrayLoadKind): Float16 would need a software or
// F16C conversion, and the BigInt kinds would have to allocate a BigInt
// result, which no inline tier does. Both must decline to the helper --
// no tier is ever emitted for them, since the recording helper files them
// as "other" rather than as a typed-array kind -- and still answer
// exactly, in bounds and out.
var f16 = new Float16Array(4);
f16[0] = 1.5;
f16[1] = -0.25;
for (var i = 0; i < 100; ++i) readF16(f16, 0);
print("f16", readF16(f16, 0), readF16(f16, 1), readF16(f16, 2), readF16(f16, 9));

var b64 = new BigInt64Array(4);
b64[0] = -9223372036854775808n;
b64[1] = 42n;
for (var i = 0; i < 100; ++i) readB64(b64, 0);
print("b64", readB64(b64, 0), readB64(b64, 1), readB64(b64, 9));
print("b64 type", typeof readB64(b64, 1));

// TA: ta in 10 40
// TA-NEXT: ta past undefined
// TA-NEXT: ta oob undefined undefined
// TA-NEXT: ta huge undefined undefined
// TA-NEXT: ta neg undefined 10
// TA-NEXT: det before 77 4
// TA-NEXT: det after undefined undefined 0
// TA-NEXT: f16 1.5 -0.25 0 undefined
// TA-NEXT: b64 -9223372036854775808 42 undefined
// TA-NEXT: b64 type bigint

// Anti-GetByIndex, anti-vacuity pin: every reader must hold a GetByVal, or
// it would be testing GetByIndex (which has no tier) instead.
// BC-LABEL:Function<readAt>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readI32>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readF16>({{.*}}
// BC: GetByVal
// BC-LABEL:Function<readB64>({{.*}}
// BC: GetByVal
