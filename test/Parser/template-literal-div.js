/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -dump-ast -pretty-json %s | %FileCheckOrRegen %s --match-full-lines

// A '/' after a template literal is division, not a regexp.
`4` / 2;
`4${0}` / 2 / 1;
tag`4` / 2;

// Auto-generated content below. Please do not modify manually.

// CHECK:{
// CHECK-NEXT:  "type": "Program",
// CHECK-NEXT:  "body": [
// CHECK-NEXT:    {
// CHECK-NEXT:      "type": "ExpressionStatement",
// CHECK-NEXT:      "expression": {
// CHECK-NEXT:        "type": "BinaryExpression",
// CHECK-NEXT:        "left": {
// CHECK-NEXT:          "type": "TemplateLiteral",
// CHECK-NEXT:          "quasis": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "type": "TemplateElement",
// CHECK-NEXT:              "tail": true,
// CHECK-NEXT:              "cooked": "4",
// CHECK-NEXT:              "raw": "4"
// CHECK-NEXT:            }
// CHECK-NEXT:          ],
// CHECK-NEXT:          "expressions": []
// CHECK-NEXT:        },
// CHECK-NEXT:        "right": {
// CHECK-NEXT:          "type": "NumericLiteral",
// CHECK-NEXT:          "value": 2,
// CHECK-NEXT:          "raw": "2"
// CHECK-NEXT:        },
// CHECK-NEXT:        "operator": "\/"
// CHECK-NEXT:      },
// CHECK-NEXT:      "directive": null
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "type": "ExpressionStatement",
// CHECK-NEXT:      "expression": {
// CHECK-NEXT:        "type": "BinaryExpression",
// CHECK-NEXT:        "left": {
// CHECK-NEXT:          "type": "BinaryExpression",
// CHECK-NEXT:          "left": {
// CHECK-NEXT:            "type": "TemplateLiteral",
// CHECK-NEXT:            "quasis": [
// CHECK-NEXT:              {
// CHECK-NEXT:                "type": "TemplateElement",
// CHECK-NEXT:                "tail": false,
// CHECK-NEXT:                "cooked": "4",
// CHECK-NEXT:                "raw": "4"
// CHECK-NEXT:              },
// CHECK-NEXT:              {
// CHECK-NEXT:                "type": "TemplateElement",
// CHECK-NEXT:                "tail": true,
// CHECK-NEXT:                "cooked": "",
// CHECK-NEXT:                "raw": ""
// CHECK-NEXT:              }
// CHECK-NEXT:            ],
// CHECK-NEXT:            "expressions": [
// CHECK-NEXT:              {
// CHECK-NEXT:                "type": "NumericLiteral",
// CHECK-NEXT:                "value": 0,
// CHECK-NEXT:                "raw": "0"
// CHECK-NEXT:              }
// CHECK-NEXT:            ]
// CHECK-NEXT:          },
// CHECK-NEXT:          "right": {
// CHECK-NEXT:            "type": "NumericLiteral",
// CHECK-NEXT:            "value": 2,
// CHECK-NEXT:            "raw": "2"
// CHECK-NEXT:          },
// CHECK-NEXT:          "operator": "\/"
// CHECK-NEXT:        },
// CHECK-NEXT:        "right": {
// CHECK-NEXT:          "type": "NumericLiteral",
// CHECK-NEXT:          "value": 1,
// CHECK-NEXT:          "raw": "1"
// CHECK-NEXT:        },
// CHECK-NEXT:        "operator": "\/"
// CHECK-NEXT:      },
// CHECK-NEXT:      "directive": null
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "type": "ExpressionStatement",
// CHECK-NEXT:      "expression": {
// CHECK-NEXT:        "type": "BinaryExpression",
// CHECK-NEXT:        "left": {
// CHECK-NEXT:          "type": "TaggedTemplateExpression",
// CHECK-NEXT:          "tag": {
// CHECK-NEXT:            "type": "Identifier",
// CHECK-NEXT:            "name": "tag"
// CHECK-NEXT:          },
// CHECK-NEXT:          "quasi": {
// CHECK-NEXT:            "type": "TemplateLiteral",
// CHECK-NEXT:            "quasis": [
// CHECK-NEXT:              {
// CHECK-NEXT:                "type": "TemplateElement",
// CHECK-NEXT:                "tail": true,
// CHECK-NEXT:                "cooked": "4",
// CHECK-NEXT:                "raw": "4"
// CHECK-NEXT:              }
// CHECK-NEXT:            ],
// CHECK-NEXT:            "expressions": []
// CHECK-NEXT:          }
// CHECK-NEXT:        },
// CHECK-NEXT:        "right": {
// CHECK-NEXT:          "type": "NumericLiteral",
// CHECK-NEXT:          "value": 2,
// CHECK-NEXT:          "raw": "2"
// CHECK-NEXT:        },
// CHECK-NEXT:        "operator": "\/"
// CHECK-NEXT:      },
// CHECK-NEXT:      "directive": null
// CHECK-NEXT:    }
// CHECK-NEXT:  ]
// CHECK-NEXT:}
