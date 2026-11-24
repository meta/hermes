/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=4 %s > %t.force && diff %t.int %t.force
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-recompile-threshold=4 %s > %t.warm && diff %t.int %t.warm
// RUN: %hermes -fno-inline %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-recompile-threshold=4 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=DUMP,DUMP-%hv-mode %s
// REQUIRES: jit
// UNSUPPORTED: handle_san || gc_malloc

// Values at the edges of the SmallHermesValue encoding, stored through the
// PutByVal JSArray tier, the PutById tier (a direct and an out-of-line
// slot) and a duo PutByVal site, into young and old targets, and read back
// before and after a collection. They must come back bit-exact (same():
// Object.is semantics). Boxed in both boxed modes: +-0.1, +-5e-324 (bits
// 0x1), 1e-310 (subnormal), +-DBL_MAX, 2^53 - 1, 1 + 2^-52. Boxed under HV32
// only: +-(2^28 - 1), whose low 3 bits are zero. Inline everywhere: 0.5,
// -0, NaN, Infinity.
//
// storeDuo first stores into a Float64Array: its four declines cross
// -Xjit-recompile-threshold=4, and version 2 puts the typed-array tier
// ahead of the JSArray tier (the x86-64 tier's highest register pressure).
// The DUMP lines check that version 2 still has the box arm.
//
// Fails if the box arm stored only part of the double (plan Task 1,
// mutation D: the JIT output differs from the interpreter's); the DUMP
// lines fail before the feature.

function storeElem(arr, i, v) {
  arr[i] = v;
}
function setP(o, v) {
  o.p = v;
}
function setQ(o, v) {
  o.q = v;
}
function storeDuo(t, i, v) {
  t[i] = v;
}

function same(a, b) {
  if (a !== a)
    return b !== b;
  return a === b && (a !== 0 || 1 / a === 1 / b);
}

function show(v) {
  return v === 0 && 1 / v < 0 ? '-0' : String(v);
}

function main(values, gc) {
  var n = values.length;
  function makeObj() {
    return {p: 0};
  }
  function makeWide() {
    return {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, q: 0};
  }
  function makeArr() {
    var a = [];
    for (var i = 0; i < n; ++i)
      a.push(0);
    return a;
  }
  // Warm the PutById write caches (threshold mode compiles at the fifth
  // call) with inline-encodable values.
  var wo = makeObj(), ww = makeWide(), wa = makeArr();
  for (var i = 0; i < 5; ++i) {
    setP(wo, i);
    setQ(ww, i);
    storeElem(wa, 0, i);
  }
  var f64 = new Float64Array(4);
  for (var i = 0; i < 8; ++i)
    storeDuo(f64, 1, i);

  function pass(label, old) {
    var os = [], ws = [], a = makeArr(), d = makeArr();
    for (var i = 0; i < n; ++i) {
      os.push(makeObj());
      ws.push(makeWide());
    }
    if (old)
      gc();
    for (var i = 0; i < n; ++i) {
      var v = values[i];
      setP(os[i], v);
      setQ(ws[i], v);
      storeElem(a, i, v);
      storeDuo(d, i, v);
    }
    function check() {
      var bad = 0;
      for (var i = 0; i < n; ++i) {
        var v = values[i];
        if (!same(os[i].p, v) || !same(ws[i].q, v) || !same(a[i], v) ||
            !same(d[i], v))
          ++bad;
      }
      return bad;
    }
    var before = check();
    gc();
    print(label, 'mismatches', before, check());
    print(label, a.map(show).join(' '));
  }
  pass('young', false);
  pass('old', true);
}

main(
    [0.1, -0.1, 5e-324, -5e-324, 1e-310, 1.7976931348623157e308,
     -1.7976931348623157e308, 268435455, -268435455, 9007199254740991,
     1.0000000000000002, 0.5, -0, NaN, Infinity],
    gc);

// CHECK: young mismatches 0 0
// CHECK-NEXT: young 0.1 -0.1 5e-324 -5e-324 1e-310 1.7976931348623157e+308 -1.7976931348623157e+308 268435455 -268435455 9007199254740991 1.0000000000000002 0.5 -0 NaN Infinity
// CHECK-NEXT: old mismatches 0 0
// CHECK-NEXT: old 0.1 -0.1 5e-324 -5e-324 1e-310 1.7976931348623157e+308 -1.7976931348623157e+308 268435455 -268435455 9007199254740991 1.0000000000000002 0.5 -0 NaN Infinity

// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'storeDuo' (version 2)
// DUMP: // Inline typed array store (kind {{[0-9]+}})
// DUMP: // Inline fast array store
// DUMP-HV32: // Box the double in the young generation
// DUMP-BOXED: // Box the double in the young generation
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeDuo' (version 2)
