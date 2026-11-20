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
  GetHermesESTreeJSON,
  formatAndWriteSrcArtifact,
  LITERAL_TYPES,
  EXCLUDE_PROPERTIES_FROM_NODE,
} from './utils/scriptUtils';

const ALLOWED_ARG_TYPES = new Set(['NodePtr', 'NodeList']);

// $FlowExpectedError[incompatible-type]
const visitorKeys: {[key: string]: Array<string>} = Object.create(null);
for (const node of GetHermesESTreeJSON()) {
  visitorKeys[node.name] = node.arguments
    .filter(
      arg =>
        ALLOWED_ARG_TYPES.has(arg.type) &&
        EXCLUDE_PROPERTIES_FROM_NODE.get(node.name)?.has(arg.name) !== true,
    )
    .map(arg => arg.name);
}

// Override select visitor keys to set correct traversal order
visitorKeys['IfStatement'] = ['test', 'consequent', 'alternate'];
visitorKeys['ConditionalExpression'] = ['test', 'consequent', 'alternate'];
visitorKeys['WhileStatement'] = ['test', 'body'];
visitorKeys['VariableDeclarator'] = ['id', 'init'];

// these remain as a work around for prettier requiring us to use the old, deprecated AST for formatting
visitorKeys['OptionalMemberExpression'] = visitorKeys['MemberExpression'];
visitorKeys['OptionalCallExpression'] = visitorKeys['CallExpression'];

// correct the "literal" types
// the base defs declare each literal as a separate node, but ESTree treats them as a single node
visitorKeys['Literal'] = [];
for (const type of LITERAL_TYPES) {
  delete visitorKeys[type];
}

const visitorKeysFileContents = `module.exports = ${JSON.stringify(
  visitorKeys,
  null,
  2,
)};`;

formatAndWriteSrcArtifact({
  code: visitorKeysFileContents,
  package: 'hermes-parser',
  file: 'generated/ESTreeVisitorKeys.js',
  // This file is shadowed by a manual `.js.flow` file
  flow: false,
});
