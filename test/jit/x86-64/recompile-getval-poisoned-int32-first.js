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

// Poison preserves the FIRST kind, on the load side too. f's single
// GetByVal site alternates Int32Array/Float64Array from call 1, and this
// variant feeds it the Int32Array FIRST: the record's taKind is fixed at
// Int32ArrayKind (39) by that first observation and is never replaced --
// the Float64Array observation one call later only sets taPoisoned, which
// selection and the progress term both ignore. So version 2 must
// specialize kind 39 here, while the Float64-first twin
// (recompile-getval-poisoned-float64-first.js) must specialize kind 42
// from the same interleaving: the rule is deterministic GIVEN the traffic
// order, not order-independent, which is exactly why both orders are
// pinned.
//
// Once version 2 has the kind-39 tier the site can no longer progress --
// taKind == specializedTAKind, and the poisoned second kind never becomes
// a second tier -- so the remaining budget must NOT be spent: no version 3
// may ever appear, however long the alternating traffic runs. The
// Float64Array half stays a permanent decline into the recording helper,
// and its values must stay exactly right.
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
for (var i = 0; i < 400; ++i) sum += f(i & 1 ? ta2 : ta1, i & 3);
print("sum", sum);
// Fresh top-level calls in version 2: the Int32Array through its inline
// tier, the Float64Array through the helper the poisoned site kept.
print("i32", f(ta1, 0), f(ta1, 3), f(ta1, 4));
print("f64", f(ta2, 0), f(ta2, 3), f(ta2, 4));

// OUT: sum 900
// OUT-NEXT: i32 1 4 undefined
// OUT-NEXT: f64 0.5 3.5 undefined

// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// DUMP: // getByVal r
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// DUMP: // Inline typed array load (kind 39)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// DUMP: JIT ByVal sites: 1 observed, 1 specialized
// The second kind must never earn a tier of its own, and the poisoned
// site must never be mistaken for further progress: no version 3 of f.
// DUMP-NOT: {{.*}}'f' (version 3){{.*}}
