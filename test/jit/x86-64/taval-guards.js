/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xhermes-internal-test-methods -fno-inline %s > %t.int
// RUN: %hermes -Xhermes-internal-test-methods -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -Xhermes-internal-test-methods -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The inline typed-array PutByVal store tier's guards: the class-flags
// check (fast index properties, frozen clear) and the bounds check
// (idx < length_), both through a SINGLE, specialized, strict-mode
// store site. Every guard exit here must decline to the same helper the
// interpreter uses, so the strict-mode throw/no-throw/no-op behavior is
// identical whether the site is interpreted or running its specialized
// tier -- checked by the actual stdout diff below, following
// taval-conversions.js and test/jit/putbyval-inline.js, not by a shared
// FileCheck OUT prefix.
//
// HermesInternal.detachArrayBuffer needs -Xhermes-internal-test-methods
// (see test/hermes/typedarray-detached.js); every RUN line here runs the
// whole script, including the detach case below, so every RUN line
// carries the flag.

"use strict";

function f(a, i, v) {
  a[i] = v;
}

// Warm the single ByVal site in f with a normal, extensible Int32Array,
// via 70 SEPARATE top-level calls. Recompilation publishes the
// specialized entry point for invocations AFTER the threshold is
// crossed, not for the call that crosses it -- so warming (which
// necessarily runs mostly in version 1, through the helper, which is
// what records the site's Int32Array evidence) and every probe below
// (each its own call, so each genuinely runs in version 2 once it
// exists) must be separate invocations, never nested in one activation.
var warm = new Int32Array(4);
for (var i = 0; i < 70; ++i)
  f(warm, 0, i);
print("warm", warm[0]);

// flags-guard regression -- the spec's P1 counterexample. Defining an
// out-of-range own property and then freezing gives the class index-like
// properties (defineProperty) and sets frozen (freeze), so the
// tier's masked flags compare no longer matches "extensible fast
// array": the store must decline to the helper, which -- a strict store
// on a frozen typed array -- throws.
var b = new Int32Array(1);
Object.defineProperty(b, "2", { value: 0 });
Object.freeze(b);
try {
  f(b, 0, 7);
  print("b: no throw");
} catch (e) {
  print("caught", e.name);
}
print("b state", b.length, b[0]);

// observable fallback: preventExtensions() alone (no defineProperty, no
// freeze), then an out-of-bounds store. This also throws in the
// interpreter -- read off the diff, not assumed in advance -- so the
// tier's decline here (whether through the flags check or the bounds
// check) has to reach the exact same strict-mode throw.
var c = new Int32Array(4);
Object.preventExtensions(c);
try {
  f(c, 10, 9);
  print("c: no throw");
} catch (e) {
  print("caught", e.name);
}
print("c state", c.length, c[0]);

// genuine no-op: an out-of-bounds store on a plain, extensible
// Int32Array is silently ignored by the interpreter -- neither the
// length nor any element changes, and nothing throws even in strict
// mode. This is what the bounds-check decline, on its own, must
// reproduce.
var g = new Int32Array(4);
try {
  f(g, 10, 9);
  print("g: no throw");
} catch (err) {
  print("caught", err.name);
}
print("g state", g.length, g[0]);

// detached: a SEPARATE, still-extensible Int32Array (unlike c, this one
// was never made non-extensible, so any throw below cannot be blamed on
// that) whose buffer is detached, then an in-bounds store through the
// same warmed, specialized site. A detached typed array has no indexed
// properties at all, so the store falls through to a generic property
// assignment; whatever the interpreter does with that -- throw or not --
// is pinned by the diff below, not assumed here.
var d = new Int32Array(4);
HermesInternal.detachArrayBuffer(d.buffer);
try {
  f(d, 0, 9);
  print("d: no throw");
} catch (err) {
  print("caught", err.name);
}
print("d state", d.length);

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
