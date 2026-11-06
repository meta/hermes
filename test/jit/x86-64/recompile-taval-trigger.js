/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=0 -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=OFF %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// f has no ById site at all: the recompile must trigger from the
// ByVal decline stream and its recorded Int32Array monomorphism.
function f(a, v) {
  a[0] = v;
}
var ta = new Int32Array(4);
for (var i = 0; i < 100; ++i)
  f(ta, i);
print(ta[0]);

// OUT: 99

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: // Inline typed array store
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: JIT ByVal sites: 1 observed, 1 specialized

// OFF: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// OFF-NOT: (version 2)
// OFF: 99
