/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | sed 's/0x[0-9A-Fa-f]*//g' > %t.1
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=3 %s | sed 's/0x[0-9A-Fa-f]*//g' > %t.2
// RUN: grep -q "(version 2)" %t.1
// RUN: diff %t.1 %t.2
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Recompilation must not make emission nondeterministic: two identical
// runs produce byte-identical dumps, version-2 bodies included. The grep
// guards against vacuous success if recompilation stops triggering: diff
// alone would still pass on two identical version-1-only dumps.
//
// The dumps embed absolute addresses (e.g. helper-call targets
// `mov r11, 0x...`) that ASLR moves independently between the two
// separate processes below, even though both run the same binary in
// the same lit invocation. Confirmed empirically: without the sed
// filter, every diffing line has that shape. The filter strips hex
// literals so the comparison stays structural (instructions, registers,
// ordering, version-2 bodies) without hiding a real emission change.

function f(o, v) {
  o.p = v;
}
var o = {p: 0};
for (var i = 0; i < 100; ++i)
  f(o, i);
print(o.p);
