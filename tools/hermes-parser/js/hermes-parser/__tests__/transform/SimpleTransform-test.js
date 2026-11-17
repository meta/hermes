/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict-local
 * @format
 */

'use strict';

import type {ESNode} from 'hermes-estree';

import {cleanASTForSnapshot, parse} from '../../__test_utils__/parse';
import {SimpleTransform} from '../../src/transform/SimpleTransform';

function expectTransformToEqual({
  code,
  result,
  transform,
}: Readonly<{
  code: string,
  result: string,
  transform: ESNode => ESNode | null,
}>): void {
  const codeAST = parse(code);
  const resultAST = parse(result);

  const transformedAST = SimpleTransform.transform(codeAST, {transform});
  expect(
    transformedAST == null ? null : cleanASTForSnapshot(transformedAST),
  ).toEqual(cleanASTForSnapshot(resultAST));
}

describe('SimpleTransform', () => {
  describe('Remove', () => {
    it('Statement', () => {
      expectTransformToEqual({
        code: `a; function b() {}`,
        result: `function b() {}`,
        transform(node) {
          if (node.type === 'ExpressionStatement') {
            return null;
          }
          return node;
        },
      });
    });
    it('TypeAnnotation', () => {
      expectTransformToEqual({
        code: `function b(): void {}`,
        result: `function b() {}`,
        transform(node) {
          if (node.type === 'TypeAnnotation') {
            return null;
          }
          return node;
        },
      });
    });
  });
  describe('Replace', () => {
    it('Statement', () => {
      let retraversedReplacedNode = false;
      expectTransformToEqual({
        code: `a; function b() {}`,
        result: `b; function b() {}`,
        transform(node) {
          if (node.type === 'ExpressionStatement') {
            if (
              node.expression.type === 'Identifier' &&
              node.expression.name === 'b'
            ) {
              retraversedReplacedNode = true;
              return node;
            }
            // $FlowFixMe[incompatible-type]
            return {
              type: 'ExpressionStatement',
              expression: {
                type: 'Identifier',
                name: 'b',
                typeAnnotation: null,
                optional: false,
              },
              directive: null,
            };
          }
          return node;
        },
      });
      expect(retraversedReplacedNode).toBeTruthy();
    });
    it('TypeAnnotation', () => {
      expectTransformToEqual({
        code: `function b(): void {}`,
        result: `function b(): string {}`,
        transform(node) {
          if (node.type === 'VoidTypeAnnotation') {
            // $FlowFixMe[incompatible-type]
            return {
              type: 'StringTypeAnnotation',
            };
          }
          return node;
        },
      });
    });
    it('Nested expressions', () => {
      expectTransformToEqual({
        code: `a as number as string;`,
        result: `a;`,
        transform(node) {
          if (node.type === 'AsExpression') {
            return node.expression;
          }
          return node;
        },
      });
    });
    it('Array element at each position', () => {
      // A replacement must land correctly at the start, middle and end of the
      // sibling array.
      for (const target of [1, 2, 3]) {
        expectTransformToEqual({
          code: `[1, 2, 3];`,
          result: `[${[1, 2, 3].map(v => (v === target ? 9 : v)).join(', ')}];`,
          transform(node) {
            if (node.type === 'Literal' && node.value === target) {
              // $FlowFixMe[incompatible-type]
              return {
                type: 'Literal',
                value: 9,
                raw: '9',
                literalType: 'numeric',
              };
            }
            return node;
          },
        });
      }
    });
    it('Nested array element', () => {
      expectTransformToEqual({
        code: `[[1, 2], [3]];`,
        result: `[[1, 9], [3]];`,
        transform(node) {
          if (node.type === 'Literal' && node.value === 2) {
            // $FlowFixMe[incompatible-type]
            return {
              type: 'Literal',
              value: 9,
              raw: '9',
              literalType: 'numeric',
            };
          }
          return node;
        },
      });
    });
    it('One node with many nodes inside an array', () => {
      // Array-valued replacements change the sibling count, so they must keep
      // going through `replaceInArray` instead of any in-place assignment.
      expectTransformToEqual({
        code: `[1, 2, 3];`,
        result: `[1, 9, 8, 3];`,
        transform(node) {
          if (node.type === 'Literal' && node.value === 2) {
            // $FlowFixMe[incompatible-type]
            return [
              {type: 'Literal', value: 9, raw: '9', literalType: 'numeric'},
              {type: 'Literal', value: 8, raw: '8', literalType: 'numeric'},
            ];
          }
          return node;
        },
      });
    });
    it('Keeps the sibling array identity for a single-node replacement', () => {
      // Regression guard for the O(1) replacement path: swapping one node for
      // one node must write into the existing array rather than rebuild it.
      // Rebuilding is O(siblings) per replacement, which made the `babel: true`
      // AST conversion quadratic in sibling count.
      const ast: $FlowFixMe = parse(`[1, 2, 3];`);
      const elementsBefore = ast.body[0].expression.elements;

      const result = SimpleTransform.transform(ast, {
        transform(node) {
          if (node.type === 'Literal' && node.value === 2) {
            // $FlowFixMe[incompatible-type]
            return {
              type: 'Literal',
              value: 9,
              raw: '9',
              literalType: 'numeric',
            };
          }
          return node;
        },
      });

      const resultAST: $FlowFixMe = result;
      const elementsAfter = resultAST.body[0].expression.elements;
      expect(elementsAfter).toBe(elementsBefore);
      expect(elementsAfter.map(element => element.value)).toEqual([1, 9, 3]);
    });
  });
});
