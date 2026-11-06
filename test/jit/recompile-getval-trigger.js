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

// f has no ById site at all: the recompile must trigger from the GetByVal
// decline stream and its recorded Int32Array monomorphism, and version 2
// must answer the same reads with the inline typed-array LOAD tier.
//
// The key is f's `i` parameter, never a literal: ISel.cpp lowers a uint8
// literal number key to GetByIndex, which has no tier at all, so a
// literal-keyed twin of this file would compile to a different opcode and
// pin nothing. The `// getByVal r` dump comment below is the pin that this
// really is a GetByVal site.
function f(a, i) {
  return a[i];
}
var ta = new Int32Array(4);
ta[0] = 11;
ta[1] = -22;
ta[3] = 2147483647;

// The warm-up sum is taken over the whole run, so it covers the reads that
// went through version 1's helper AND the ones that ran in version 2's
// inline tier: a tier that read the wrong element, or the wrong address,
// changes it.
var sum = 0;
for (var i = 0; i < 100; ++i) sum += f(ta, i & 3);
print("sum", sum);
// Fresh top-level calls, so each genuinely runs in version 2.
print("probe", f(ta, 0), f(ta, 1), f(ta, 2), f(ta, 3));
print("oob", f(ta, 4), f(ta, 1000000));

// OUT: sum 53687090900
// OUT-NEXT: probe 11 -22 0 2147483647
// OUT-NEXT: oob undefined undefined

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: // getByVal r
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: // Inline typed array load (kind 39)
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized

// With no recompile budget the site stays on version 1 -- the JSArray tier
// plus the recording helper -- and must answer identically.
// OFF: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// OFF-NOT: {{.*}}(version 2){{.*}}
// OFF: sum 53687090900
// OFF-NEXT: probe 11 -22 0 2147483647
// OFF-NEXT: oob undefined undefined
