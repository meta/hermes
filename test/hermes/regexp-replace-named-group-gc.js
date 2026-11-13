/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O -gc-sanitize-handles=1 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -gc-sanitize-handles=1 %s | %FileCheck --match-full-lines %s

// GetSubstitution used to keep a raw pointer into the replacement string's
// storage while looking up the $<name> group in the identifier table. Creating
// a new identifier allocates, so a GC in that window moved the replacement
// string out from under the pointer. The replacement must be a UTF-16 string
// (an ASCII one is copied) and the group name must not already be interned.

print('named group replacement');
// CHECK-LABEL: named group replacement

var name = 'Āgrp' + Math.random().toString(36).slice(2);
var re = new RegExp('(?<' + name + '>b)');
print('abc'.replace(re, '[$<' + name + '>]'));
// CHECK-NEXT: a[b]c

// Same shape through replaceAll and a global RegExp, with a fresh name.
var name2 = 'āgrp' + Math.random().toString(36).slice(2);
var re2 = new RegExp('(?<' + name2 + '>\\d)', 'g');
print('1-2-3'.replaceAll(re2, '<$<' + name2 + '>>'));
// CHECK-NEXT: <1>-<2>-<3>

// A name that is not a group still runs the lookup and yields the empty string.
var name3 = 'Ămissing' + Math.random().toString(36).slice(2);
print('xyz'.replace(/(?<g>y)/, '$<' + name3 + '>'));
// CHECK-NEXT: xz
