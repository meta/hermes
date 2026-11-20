/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict
 * @format
 */

'use strict';

import {
  EXCLUDE_PROPERTIES_FROM_NODE,
  GetHermesESTreeJSON,
  formatAndWriteSrcArtifact,
} from './utils/scriptUtils';
import tempCustomASTDefs from './utils/tempCustomASTDefs';

const visitorKeys: {
  [key: string]: Readonly<{[key: string]: 'Node' | 'NodeList'}>,
} =
  // $FlowExpectedError[incompatible-type]
  Object.create(null);
for (const node of GetHermesESTreeJSON()) {
  const nodeVisitorKeys: {[key: string]: 'Node' | 'NodeList'} = {};
  for (const arg of node.arguments) {
    if (EXCLUDE_PROPERTIES_FROM_NODE.get(node.name)?.has(arg.name)) {
      continue;
    }
    switch (arg.type) {
      case 'NodeList':
        nodeVisitorKeys[arg.name] = 'NodeList';
        break;

      case 'NodePtr':
        nodeVisitorKeys[arg.name] = 'Node';
        break;

      default:
        break;
    }
  }
  visitorKeys[node.name] = nodeVisitorKeys;
}

// custom temp defs
for (let typeName of Object.keys(tempCustomASTDefs)) {
  visitorKeys[typeName] = tempCustomASTDefs[typeName];
}

const visitorKeysFileContents = `\
export const NODE_CHILD = 'Node';
export const NODE_LIST_CHILD = 'NodeList';
export const HERMES_AST_VISITOR_KEYS = ${JSON.stringify(visitorKeys, null, 2)};
`;

formatAndWriteSrcArtifact({
  code: visitorKeysFileContents,
  package: 'hermes-parser',
  file: 'generated/ParserVisitorKeys.js',
  // This file is shadowed by a manual `.js.flow` file
  flow: false,
});
