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

import {SimpleTraverser} from '../../src/traverse/SimpleTraverser';

describe('SimpleTraverser', () => {
  it('passes the parent key and array index to the callbacks', () => {
    const traverser = new SimpleTraverser();

    const fakeAst: $FlowFixMe = {
      type: 'Program',
      body: [
        {
          type: 'ExpressionStatement',
          expression: {type: 'Identifier', name: 'a'},
        },
      ],
    };

    const entered: Array<string> = [];
    traverser.traverse(fakeAst, {
      enter(node, _parent, parentKey, parentIndex) {
        entered.push(`${node.type}:${String(parentKey)}:${String(parentIndex)}`);
      },
      leave() {},
    });

    expect(entered).toEqual([
      'Program:undefined:undefined',
      // array child: both the key and the index are known
      'ExpressionStatement:body:0',
      // single child: the key is known, there is no index
      'Identifier:expression:null',
    ]);
  });

  it("traverses all keys except 'parent', 'loc', 'range', 'leadingComments', and 'trailingComments'", () => {
    const traverser = new SimpleTraverser();

    // intentionally weird AST for testing
    const fakeAst: $FlowFixMe = {
      type: 'Program',
      body: [
        {
          type: 'ExpressionStatement',
          leadingComments: {
            // should not get traversed
            type: 'Line',
          },
          trailingComments: {
            // should not get traversed
            type: 'Block',
          },
        },
        {
          type: 'Identifier',
          foo: {
            // should not get traversed
            type: 'BarStatement',
          },
          name: 'test',
        },
      ],
    };
    fakeAst.body[0].parent = fakeAst;

    const enteredNodes = [];
    const exitedNodes = [];

    traverser.traverse(fakeAst, {
      enter: node => {
        enteredNodes.push(node);
      },
      leave: node => {
        exitedNodes.push(node);
      },
    });

    expect(enteredNodes).toEqual([fakeAst, fakeAst.body[0], fakeAst.body[1]]);
    expect(exitedNodes).toEqual([fakeAst.body[0], fakeAst.body[1], fakeAst]);
  });
});
