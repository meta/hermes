/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xjit=on -fno-inline -Xjit -Xjit-crash-on-error -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// Demonstrate the various JIT levels.
// Update if JIT heuristics are updated to illustrate that they still work.
//
// loop1 and loop2 compile warm, so each `arr.length` read gets the
// single-shape specialization, which the body records as an emitted
// GetById shape: the `JIT ById sites` line counts those reads (loop
// rotation duplicates each loop condition, hence 2 and 4 sites). Both
// backends record GetById shapes and print the line.

function loop0(arr) {
  var sum = 0;
  return sum;
}

function loop1(arr) {
  var sum = 0;
  for (var i = 0; i < arr.length; ++i)
    sum += arr[i];
  return sum;
}

function loop2(arr) {
  var sum = 0;
  for (var i = 0; i < arr.length; ++i)
    for (var j = 0; j < arr.length; ++j)
      sum += arr[i] + arr[j];
  return sum;
}

var arr = Array(10);
arr.fill(0, 0, 10)
for (var i = 0; i < 50; ++i) {
  print(i);
  loop0(arr);
  loop1(arr);
  loop2(arr);
}

// CHECK:0
// CHECK-NEXT:1
// CHECK-NEXT:2

// CHECK:JIT compilation of FunctionID 3, 'loop2'

// CHECK:JIT successfully compiled FunctionID 3, 'loop2'
// CHECK-NEXT:JIT ById sites: 4 recorded, 0 polymorphic, 0 megamorphic
// CHECK-NEXT:3
// CHECK-NEXT:4
// CHECK-NEXT:5
// CHECK-NEXT:6
// CHECK-NEXT:7
// CHECK-NEXT:8

// CHECK:JIT compilation of FunctionID 2, 'loop1'

// CHECK:JIT successfully compiled FunctionID 2, 'loop1'
// CHECK-NEXT:JIT ById sites: 2 recorded, 0 polymorphic, 0 megamorphic
// CHECK-NEXT:9
// CHECK-NEXT:10
// CHECK-NEXT:11
// CHECK-NEXT:12
// CHECK-NEXT:13
// CHECK-NEXT:14
// CHECK-NEXT:15
// CHECK-NEXT:16
// CHECK-NEXT:17
// CHECK-NEXT:18
// CHECK-NEXT:19
// CHECK-NEXT:20
// CHECK-NEXT:21
// CHECK-NEXT:22
// CHECK-NEXT:23
// CHECK-NEXT:24
// CHECK-NEXT:25
// CHECK-NEXT:26
// CHECK-NEXT:27
// CHECK-NEXT:28
// CHECK-NEXT:29
// CHECK-NEXT:30
// CHECK-NEXT:31
// CHECK-NEXT:32

// CHECK:JIT compilation of FunctionID 1, 'loop0'

// CHECK:JIT successfully compiled FunctionID 1, 'loop0'
// CHECK-NEXT:33
// CHECK-NEXT:34
// CHECK-NEXT:35
// CHECK-NEXT:36
// CHECK-NEXT:37
// CHECK-NEXT:38
// CHECK-NEXT:39
// CHECK-NEXT:40
// CHECK-NEXT:41
// CHECK-NEXT:42
// CHECK-NEXT:43
// CHECK-NEXT:44
// CHECK-NEXT:45
// CHECK-NEXT:46
// CHECK-NEXT:47
// CHECK-NEXT:48
// CHECK-NEXT:49
