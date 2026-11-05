/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes --typed --dump-sema %s 2>&1 ) | %FileCheck --match-full-lines %s

// Unsupported parameter and option shapes.

$SHBuiltin.extern_c({}, function f1({a}: {a: c_int}): void {});
// CHECK: {{.*}}:[[@LINE-1]]:37: error: ft: extern_c requires simple identifier parameters

$SHBuiltin.extern_c({}, function f2(...r: c_int[]): void {});
// CHECK: {{.*}}:[[@LINE-1]]:37: error: ft: extern_c requires simple identifier parameters

$SHBuiltin.extern_c({}, function f3(x: c_int = 1): void {});
// CHECK: {{.*}}:[[@LINE-1]]:37: error: ft: extern_c requires simple identifier parameters

$SHBuiltin.extern_c({}, function f7(x?: c_int): void {});
// CHECK: {{.*}}:[[@LINE-1]]:37: error: ft: extern_c requires simple identifier parameters

$SHBuiltin.extern_c({'include': 'x'}, function f4(): void {});
// CHECK: {{.*}}:[[@LINE-1]]:22: error: ft: extern_c: unsupported property format

$SHBuiltin.extern_c({1: true}, function f5(): void {});
// CHECK: {{.*}}:[[@LINE-1]]:22: error: ft: extern_c: unsupported property format

$SHBuiltin.extern_c({}, function f6<T>(x: c_int): c_int { return x; });
// CHECK: {{.*}}:[[@LINE-1]]:36: error: ft: type parameters not supported on function expressions
