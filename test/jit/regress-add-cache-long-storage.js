/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Run the long property storage regression test under the JIT, whose
// PutById add cache hits call JSObject::addNewOwnPropertyInSlot.
// RUN: %hermes -O -Xjit=force -Xjit-crash-on-error %S/../hermes/regress-add-cache-long-storage.js | %FileCheck --match-full-lines %S/../hermes/regress-add-cache-long-storage.js
// REQUIRES: jit
