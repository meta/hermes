/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -dump-ir %s 2>&1 ) | %FileCheck --match-full-lines %s
// RUN: (! %shermes -typed -dump-ir %s 2>&1 ) | %FileCheck --match-full-lines %s

// Two-argument $SHBuiltin.import is only an import-of-export.

$SHBuiltin.moduleFactory(0, function(global, require) {
  $SHBuiltin.export('y', $SHBuiltin.import(1, 'x'));
  var x = $SHBuiltin.import(1, 'x');
// CHECK: {{.*}}:[[@LINE-1]]:11: error: $SHBuiltin.import with two arguments is only allowed inside $SHBuiltin.export
});
