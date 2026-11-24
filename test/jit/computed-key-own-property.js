/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Run the computed-key own-property test under the JIT, where GetByVal
// with a string key calls the same runtime lookup.
// RUN: %hermes -O -Xjit=force -Xjit-crash-on-error %S/../hermes/computed-key-own-property.js | %FileCheck --match-full-lines %S/../hermes/computed-key-own-property.js
// REQUIRES: jit
