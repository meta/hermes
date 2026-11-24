/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Run the computed property cache test under the JIT, which reaches the
// cache through the get and put helpers.
// RUN: %hermes -O -Xjit=force -Xjit-crash-on-error %S/../hermes/computed-property-cache.js | %FileCheck --match-full-lines %S/../hermes/computed-property-cache.js
// REQUIRES: jit
