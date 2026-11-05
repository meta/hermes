/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -ferror-limit=0 -typed -dump-sema %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

function missingReturn(this: string) {}
Hermes.decorate(missingReturn, Hermes.builtin);
"x".missingReturn();

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}builtin-decl-error.js:10:1: error: ft: builtin function must have a return type annotation
// CHECK-NEXT:function missingReturn(this: string) {}
// CHECK-NEXT:^
// CHECK-NEXT:{{.*}}builtin-decl-error.js:12:5: error: ft: unknown string property
// CHECK-NEXT:"x".missingReturn();
// CHECK-NEXT:    ^~~~~~~~~~~~~
// CHECK-NEXT:Emitted 2 errors. exiting.
