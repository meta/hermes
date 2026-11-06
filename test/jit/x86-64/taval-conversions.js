/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The inline typed-array PutByVal store tier, one kind at a time: every
// value it stores is converted exactly the way the interpreter's
// TypedArray element setter converts it, and every key is resolved to an
// index (or falls back to a named property) exactly the way the
// interpreter resolves it.
//
// Interpreter/JIT equality is enforced by the actual stdout diff above --
// NOT by a shared FileCheck OUT prefix, which would only check that each
// expected substring occurs somewhere, in order (so an expectation of `0`
// would spuriously match inside a printed `300`). Filling in expected
// values by hand is therefore unnecessary: the diff line IS the
// correctness check, as long as the interpreter itself gets the
// conversions right, which is covered elsewhere.
//
// Eight distinct, top-level store functions -- one per kind the tier
// supports (Uint8Clamped, Float16, and the BigInt kinds are not among
// them; see isJitSupportedTypedArrayStoreKind). Each is its own function,
// not a shared factory closure: a factory would give every kind the same
// CodeBlock and the same ByVal site, destroying the monomorphism this
// tier specializes on and letting only one kind ever compile a tier at
// all.
function storeI8(a, i, v) {
  a[i] = v;
}
function storeU8(a, i, v) {
  a[i] = v;
}
function storeI16(a, i, v) {
  a[i] = v;
}
function storeU16(a, i, v) {
  a[i] = v;
}
function storeI32(a, i, v) {
  a[i] = v;
}
function storeU32(a, i, v) {
  a[i] = v;
}
function storeF32(a, i, v) {
  a[i] = v;
}
function storeF64(a, i, v) {
  a[i] = v;
}

// Prints `count` consecutive bytes of `view` starting at `start`, as one
// line, so the printed byte sequence can be read directly against the
// element layout it is meant to prove.
function printBytes(name, view, start, count) {
  var s = name + " bytes";
  for (var k = 0; k < count; ++k)
    s += " " + view[start + k];
  print(s);
}

// Each kind gets its own ArrayBuffer and a NONZERO-offset view: a view
// starting at byte 8 into a shared buffer, with a Uint8Array over the
// same buffer to inspect the raw bytes. Warming and probing at element
// index 2 (not 0) means the address the store computes is
// `data_ + offset_ + idx * width` with every term nonzero -- an
// implementation that drops `offset_`, or mis-scales `idx`, would still
// pass an index-0-only probe but fails here.

// ---- Int8Array / Uint8Array (width 1) ----
var bufI8 = new ArrayBuffer(64);
var v8I8 = new Uint8Array(bufI8);
var taI8 = new Int8Array(bufI8, 8, 4);
for (var i = 0; i < 70; ++i)
  storeI8(taI8, 0, 1);
storeI8(taI8, 2, 42);
print("I8", taI8[0], taI8[1], taI8[2], taI8[3]);
printBytes("I8", v8I8, 8, 4);
var i8Values = [
  300, -129, 65536, -1.5, -0, NaN, Infinity, -Infinity, 2147483648,
  4294967301, 9007199254740992, Math.pow(2, 63), -Math.pow(2, 63), 1e300,
];
for (var vi = 0; vi < i8Values.length; ++vi) {
  storeI8(taI8, 1, i8Values[vi]);
  print("I8 probe", vi, taI8[1]);
}

var bufU8 = new ArrayBuffer(64);
var v8U8 = new Uint8Array(bufU8);
var taU8 = new Uint8Array(bufU8, 8, 4);
for (var i = 0; i < 70; ++i)
  storeU8(taU8, 0, 1);
storeU8(taU8, 2, 42);
print("U8", taU8[0], taU8[1], taU8[2], taU8[3]);
printBytes("U8", v8U8, 8, 4);
var u8Values = i8Values;
for (var vi = 0; vi < u8Values.length; ++vi) {
  storeU8(taU8, 1, u8Values[vi]);
  print("U8 probe", vi, taU8[1]);
}

// ---- Int16Array / Uint16Array (width 2) ----
var bufI16 = new ArrayBuffer(64);
var v8I16 = new Uint8Array(bufI16);
var taI16 = new Int16Array(bufI16, 8, 4);
for (var i = 0; i < 70; ++i)
  storeI16(taI16, 0, 1);
storeI16(taI16, 2, 42);
print("I16", taI16[0], taI16[1], taI16[2], taI16[3]);
printBytes("I16", v8I16, 8, 8);
var i16Values = i8Values;
for (var vi = 0; vi < i16Values.length; ++vi) {
  storeI16(taI16, 1, i16Values[vi]);
  print("I16 probe", vi, taI16[1]);
}

var bufU16 = new ArrayBuffer(64);
var v8U16 = new Uint8Array(bufU16);
var taU16 = new Uint16Array(bufU16, 8, 4);
for (var i = 0; i < 70; ++i)
  storeU16(taU16, 0, 1);
storeU16(taU16, 2, 42);
print("U16", taU16[0], taU16[1], taU16[2], taU16[3]);
printBytes("U16", v8U16, 8, 8);
var u16Values = i8Values;
for (var vi = 0; vi < u16Values.length; ++vi) {
  storeU16(taU16, 1, u16Values[vi]);
  print("U16 probe", vi, taU16[1]);
}

// ---- Int32Array / Uint32Array (width 4) ----
var bufI32 = new ArrayBuffer(64);
var v8I32 = new Uint8Array(bufI32);
var taI32 = new Int32Array(bufI32, 8, 4);
for (var i = 0; i < 70; ++i)
  storeI32(taI32, 0, 1);
storeI32(taI32, 2, 42);
print("I32", taI32[0], taI32[1], taI32[2], taI32[3]);
printBytes("I32", v8I32, 8, 16);
var i32Values = i8Values;
for (var vi = 0; vi < i32Values.length; ++vi) {
  storeI32(taI32, 1, i32Values[vi]);
  print("I32 probe", vi, taI32[1]);
}

var bufU32 = new ArrayBuffer(64);
var v8U32 = new Uint8Array(bufU32);
var taU32 = new Uint32Array(bufU32, 8, 4);
for (var i = 0; i < 70; ++i)
  storeU32(taU32, 0, 1);
storeU32(taU32, 2, 42);
print("U32", taU32[0], taU32[1], taU32[2], taU32[3]);
printBytes("U32", v8U32, 8, 16);
var u32Values = i8Values;
for (var vi = 0; vi < u32Values.length; ++vi) {
  storeU32(taU32, 1, u32Values[vi]);
  print("U32 probe", vi, taU32[1]);
}

// ---- Float32Array (width 4) ----
var bufF32 = new ArrayBuffer(64);
var v8F32 = new Uint8Array(bufF32);
var taF32 = new Float32Array(bufF32, 8, 4);
for (var i = 0; i < 70; ++i)
  storeF32(taF32, 0, 1);
storeF32(taF32, 2, 42.5);
print("F32", taF32[0], taF32[1], taF32[2], taF32[3]);
printBytes("F32", v8F32, 8, 16);
var f32Values = [1.1, 16777217, NaN, Infinity];
for (var vi = 0; vi < f32Values.length; ++vi) {
  storeF32(taF32, 1, f32Values[vi]);
  print("F32 probe", vi, taF32[1]);
}

// ---- Float64Array (width 8) ----
var bufF64 = new ArrayBuffer(64);
var v8F64 = new Uint8Array(bufF64);
var taF64 = new Float64Array(bufF64, 8, 4);
for (var i = 0; i < 70; ++i)
  storeF64(taF64, 0, 1);
storeF64(taF64, 2, 42.5);
print("F64", taF64[0], taF64[1], taF64[2], taF64[3]);
printBytes("F64", v8F64, 8, 32);
// -0 cannot be told apart from 0 by printing the element directly, so
// 1/element is printed too: 1/-0 is -Infinity, 1/0 is Infinity.
var f64Values = [1.1, -0, NaN];
for (var vi = 0; vi < f64Values.length; ++vi) {
  storeF64(taF64, 1, f64Values[vi]);
  print("F64 probe", vi, taF64[1], 1 / taF64[1]);
}

// Key probes: NaN, 1.5, -1, and the string "1" each exercise a different
// key-guard exit (unordered-compare decline, non-integral decline,
// negative decline, and -- for "1", the string being a canonical integer
// index -- the string->number->index path succeeding where the tier
// itself does not need to run at all, since the value has to reach the
// interpreter's ToNumber(key) first). Run through storeI32's already
// warmed and specialized site; print the whole array state and whatever
// named property the store creates, after each.
storeI32(taI32, NaN, 501);
print("I32 keyNaN", taI32[0], taI32[1], taI32[2], taI32[3], taI32.NaN);
storeI32(taI32, 1.5, 502);
print("I32 key1.5", taI32[0], taI32[1], taI32[2], taI32[3], taI32["1.5"]);
storeI32(taI32, -1, 503);
print("I32 key-1", taI32[0], taI32[1], taI32[2], taI32[3], taI32[-1]);
storeI32(taI32, "1", 504);
print("I32 keyStr1", taI32[0], taI32[1], taI32[2], taI32[3]);

// Sentinel-boundary value: 2**63 + 2048 (9223372036854777856) is exactly
// representable as a double (its ULP at that magnitude is 2**11 = 2048),
// so it survives the round trip to double and back unchanged. A 64-bit
// conversion sentinel must reject it (it is far outside any of these
// kinds' ranges), yet its ToInt32 is exactly 2048 (2**63 is a multiple of
// 2**32, so the value reduces to 2048 mod 2**32) -- a value small enough
// to be mistaken for legitimate 32-bit truncation. A tier that dropped
// the sentinel compare and stored the truncated 64-bit-conversion result
// would write 0 here instead of correctly declining and going through
// the interpreter's own ToInt32, which writes 2048. Run each through its
// own already-warmed, already-specialized site, each a fresh top-level
// call.
storeI32(taI32, 1, 9223372036854777856);
print("I32 sentinel", taI32[1]);
storeU32(taU32, 1, 9223372036854777856);
print("U32 sentinel", taU32[1]);
storeI16(taI16, 1, 9223372036854777856);
print("I16 sentinel", taI16[1]);
storeU16(taU16, 1, 9223372036854777856);
print("U16 sentinel", taU16[1]);

// Non-number values through the warmed Int32 and Float64 sites. Without
// a correct sentinel/parity decline back to the interpreter's own
// ToNumber, a tier could store raw tagged bits straight from the
// argument register instead of converting -- and, for the object case,
// skip calling valueOf entirely (its "valueOf called" print would then
// be missing from the diff, on top of whatever garbage got stored).
// Each is its own fresh top-level call, never nested with warmup.
storeI32(taI32, 1, true);
print("I32 nonnum bool", taI32[1]);
storeI32(taI32, 1, "3");
print("I32 nonnum str", taI32[1]);
storeI32(taI32, 1, null);
print("I32 nonnum null", taI32[1]);
storeI32(taI32, 1, undefined);
print("I32 nonnum undef", taI32[1]);
storeI32(taI32, 1, {valueOf: function() { print("valueOf called"); return 7; }});
print("I32 nonnum valueOf", taI32[1]);

storeF64(taF64, 1, true);
print("F64 nonnum bool", taF64[1]);
storeF64(taF64, 1, "3");
print("F64 nonnum str", taF64[1]);
storeF64(taF64, 1, null);
print("F64 nonnum null", taF64[1]);
storeF64(taF64, 1, undefined);
print("F64 nonnum undef", taF64[1]);
storeF64(taF64, 1, {valueOf: function() { print("valueOf called"); return 7; }});
print("F64 nonnum valueOf", taF64[1]);

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeI8'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeI8'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeI8' (version 2)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeI8' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeU8'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeU8'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeU8' (version 2)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeU8' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeI16'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeI16'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeI16' (version 2)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeI16' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeU16'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeU16'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeU16' (version 2)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeU16' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeI32'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeI32'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeI32' (version 2)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeI32' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeU32'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeU32'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeU32' (version 2)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeU32' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeF32'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeF32'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeF32' (version 2)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeF32' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeF64'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeF64'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'storeF64' (version 2)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeF64' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
