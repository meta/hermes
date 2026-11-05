/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -typed %s | %FileCheck %s --match-full-lines
// RUN: %shermes -typed -exec %s | %FileCheck %s --match-full-lines

// Inference can't use a pattern param, so the next overload is chosen.
class C {
  @Hermes.final @Hermes.overload
  m<T>([a]: [T]): T { return a; }
  @Hermes.final @Hermes.overload
  m(x: number): number { return x + 1; }
}

function f(): void {
  const c = new C();
  print(c.m(1));
// CHECK: 2
}
f();
