/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -typed -parse-ts -dump-transpiled-ast -pretty -script %s | %FileCheck %s

const expression = function (value: number): string {
  return String(value);
};

const arrow = (value: number): string => String(value);

class Example {
  method(value: number): string {
    return String(value);
  }
}

// CHECK:       "type": "FunctionExpression",
// CHECK:       "returnType": {
// CHECK-NEXT:    "type": "TypeAnnotation",
// CHECK-NEXT:    "typeAnnotation": {
// CHECK-NEXT:      "type": "StringTypeAnnotation"

// CHECK:       "type": "ArrowFunctionExpression",
// CHECK:       "returnType": {
// CHECK-NEXT:    "type": "TypeAnnotation",
// CHECK-NEXT:    "typeAnnotation": {
// CHECK-NEXT:      "type": "StringTypeAnnotation"

// CHECK:       "type": "FunctionExpression",
// CHECK:       "returnType": {
// CHECK-NEXT:    "type": "TypeAnnotation",
// CHECK-NEXT:    "typeAnnotation": {
// CHECK-NEXT:      "type": "StringTypeAnnotation"
