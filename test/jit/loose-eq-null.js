/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Run the loose null/undefined equality test under the JIT, where the
// comparisons compile to inline tag checks.
// RUN: %hermes -O -Xjit=force -Xjit-crash-on-error %S/../hermes/loose-eq-null.js | %FileCheck --match-full-lines %S/../hermes/loose-eq-null.js
// REQUIRES: jit
