/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --match-full-lines --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=0 -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=OFF %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The GetByIndex twin of recompile-getval-trigger.js, plus -- folded in
// here rather than given its own file, since the recorder and the poison
// rule are shared with GetByVal and are already pinned by two ByVal files
// -- ONE of the two poisoned orders, as `g` below.
//
// f has no ById site at all: the recompile must trigger from the
// GetByIndex decline stream and its recorded Int32Array monomorphism, and
// version 2 must answer the same reads with the inline typed-array LOAD
// tier.
//
// The key is a LITERAL uint8 (ISel.cpp lowers exactly that to
// GetByIndex); a variable key would lower to GetByVal and pin the other
// opcode's tier. The `// getByIdx r` dump comment below is what makes
// that a checked property rather than an assumption.
function f(a) {
  return a[1];
}
var ta = new Int32Array(4);
ta[0] = 11;
ta[1] = -22;
ta[3] = 2147483647;

// The warm-up sum is taken over the whole run, so it covers the reads
// that went through version 1's helper AND the ones that ran in version
// 2's inline tier: a tier that read the wrong element, or the wrong
// address, changes it.
var sum = 0;
for (var i = 0; i < 100; ++i) sum += f(ta);

// ---- poison, Int32Array first ----
//
// g's single GetByIndex site alternates Int32Array/Float64Array from call
// 1, Int32Array FIRST: the record's taKind is fixed at Int32ArrayKind (39)
// by that first observation and is never replaced -- the Float64Array
// observation one call later only sets taPoisoned, which selection and the
// progress term both ignore. So version 2 must specialize kind 39.
//
// Once version 2 has the kind-39 tier the site can no longer progress --
// taKind == specializedTAKind, and the poisoned second kind never becomes
// a second tier -- so the remaining budget must NOT be spent: no version 3
// may ever appear, however long the alternating traffic runs. The
// Float64Array half stays a permanent decline into the recording helper,
// and its values must stay exactly right.
//
// The OTHER order (Float64Array first) is not duplicated here: the
// recording helper and the poison rule are shared with GetByVal, where
// recompile-getval-poisoned-{int32,float64}-first.js already pin both
// orders. What this section adds is that a ByIndex site reaches that same
// shared recorder at all.
function g(x) {
  return x[1];
}
var ga = new Int32Array(4);
ga[1] = 7;
var gb = new Float64Array(4);
gb[1] = 0.5;
var gsum = 0;
for (var i = 0; i < 400; ++i) gsum += g(i & 1 ? gb : ga);

// Every print is deferred to here, after BOTH warm-ups, so that the four
// output lines are contiguous with no compile banner between them: that is
// what lets the OFF prefix below put its "no version 2 anywhere" check
// over the whole compiling part of the run in one directive.
//
// Each of these is a fresh top-level call, so each genuinely runs in the
// installed version -- installing a version swaps the function entry,
// never a running activation. g's Int32Array goes through its inline tier,
// its Float64Array through the helper the poisoned site kept.
print("sum", sum);
print("probe", f(ta));
print("gsum", gsum);
print("g", g(ga), g(gb));

// OUT: sum -2200
// OUT-NEXT: probe -22
// OUT-NEXT: gsum 1500
// OUT-NEXT: g 7 0.5

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: // getByIdx r
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: // Inline typed array load (kind 39)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
//
// The poisoned site keeps the FIRST kind it saw.
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'g' (version 2)
// SPEC: // Inline typed array load (kind 39)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'g' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized
// The second kind must never earn a tier of its own, and the poisoned
// site must never be mistaken for further progress: no version 3 of g.
// SPEC-NOT: {{.*}}'g' (version 3){{.*}}

// With no recompile budget every site stays on version 1 -- the JSArray
// tier plus the recording helper -- and must answer identically. Because
// every print is deferred past both warm-ups, the -NOT below spans every
// compile this run performs, f's and g's alike.
// OFF: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// OFF-NOT: {{.*}}(version 2){{.*}}
// OFF: sum -2200
// OFF-NEXT: probe -22
// OFF-NEXT: gsum 1500
// OFF-NEXT: g 7 0.5
