/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// REQUIRES: jit
// REQUIRES: gc_hades
// UNSUPPORTED: handle_san

// o's ByVal site stores a numeric key on a plain object, never a
// JSArray or a supported typed array: the JSArray tier's kind guard
// always misses, so the store always declines through the helper,
// which records "other" traffic (otherSeen), never jsArraySeen. t's
// ById site is cold under force and also declines every call; the pair
// crosses the 64 threshold (pinned on the RUN line) by call ~32, well
// inside the 100-call loop, and installs version 2 from the ById
// route. Under monotone emission otherSeen drops NOTHING: version 2
// KEEPS the JSArray tier at o's site -- it is a static prior, never
// removed, and a hopeless site is retired by slow-path demotion rather
// than by un-emitting code -- while still emitting no typed-array tier
// there, since no supported kind was ever observed. The site counts as
// observed and specializes nothing.
function f(o, t, v) {
  o[0] = v;
  t.p = v;
}
var o = {};
var t = {p: 0};
for (var i = 0; i < 100; ++i)
  f(o, t, i);
print(o[0], t.p);

// OUT: 99 99

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC-NOT: // Inline typed array store
// SPEC: // Inline fast array store
// SPEC-NOT: // Inline typed array store
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 0 specialized
