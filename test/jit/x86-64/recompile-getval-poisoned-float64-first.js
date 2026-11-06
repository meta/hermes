/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xjit-recompile-threshold=64 %s | %FileCheck --match-full-lines --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The other order of recompile-getval-poisoned-int32-first.js: the SAME
// alternating Int32Array/Float64Array traffic through one GetByVal site,
// but the Float64Array arrives first, so the record's taKind is fixed at
// Float64ArrayKind (42) and version 2 must specialize the FLOAT tier here
// -- which also makes this the file that pins the float tier's NaN
// canonicalization being present in a specialized body, since the
// Int32-first twin never emits one. The rule is deterministic given the
// traffic order, not order-independent; pinning only one order would let
// an implementation that always picked, say, the numerically smaller kind
// pass.
//
// As in the twin, the site cannot progress once it has its first kind's
// tier, so the second recompile must never be spent: no version 3, however
// long the alternating traffic runs, and the Int32Array half stays a
// permanent decline into the recording helper with exactly right values.
//
// The key is f's `i` parameter, never a literal: a literal-keyed twin
// would lower to GetByIndex, which has no tier. The `// getByVal r` dump
// comment is the pin that this is a GetByVal site.
function f(x, i) {
  return x[i];
}
var ta1 = new Int32Array(4);
ta1[0] = 1;
ta1[1] = 2;
ta1[2] = 3;
ta1[3] = 4;
var ta2 = new Float64Array(4);
ta2[0] = 0.5;
ta2[1] = 1.5;
ta2[2] = 2.5;
ta2[3] = 3.5;
var sum = 0;
for (var i = 0; i < 400; ++i) sum += f(i & 1 ? ta1 : ta2, i & 3);
print("sum", sum);
// Fresh top-level calls in version 2: the Float64Array through its inline
// tier, the Int32Array through the helper the poisoned site kept.
print("f64", f(ta2, 0), f(ta2, 3), f(ta2, 4));
print("i32", f(ta1, 0), f(ta1, 3), f(ta1, 4));
// A NaN written into the specialized float view, read back through the
// same specialized site: the tier's canonicalization has to survive here
// too, not only in getval-conversions.js's dedicated loaders.
ta2[1] = NaN;
print("nan", f(ta2, 1), f(ta2, 1) !== f(ta2, 1), Object.is(f(ta2, 1), NaN));

// OUT: sum 900
// OUT-NEXT: f64 0.5 3.5 undefined
// OUT-NEXT: i32 1 4 undefined
// OUT-NEXT: nan NaN true true

// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// DUMP: // getByVal r
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// DUMP: // Inline typed array load (kind 42)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// DUMP: JIT ByVal sites: 1 observed, 1 specialized
// The second kind must never earn a tier of its own, and the poisoned
// site must never be mistaken for further progress: no version 3 of f.
// DUMP-NOT: {{.*}}'f' (version 3){{.*}}
