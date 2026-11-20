/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict
 * @format
 */

type AstType = 'Node' | 'NodeList';
const NODE = 'Node';
const NODE_LIST = 'NodeList';

/**
 * Custom AST definitions.
 *
 * Hermes parser generates its AST defintions based on `ESTree.def` but currently this codebase
 * uses slightly different AST format. This file is used to support additional AST nodes that
 * are not contained in `ESTree.def` and is only intented as a short term bridge until
 * internals can be cleaned up.
 *
 * Format:
 *   - 'builder'. The builder strings are used to codegen the builder API's.
 *     - e.g. `File: {builder: ['program']}` -> `function File(program) { return {type: 'File', program: program}; }`
 *   - 'visitor'. The visitor defs are used to codegen the AST visitor information.
 *     - `NODE` denotes the key contains a single AST node.
 *     - `NODE_LIST` denotes the key contains a list of AST nodes.
 *
 * TODO: Change babel to use the Hermes AST format so this is no longer needed.
 */
const customASTDefs: Readonly<{
  [key: string]: Readonly<{[key: string]: AstType}>,
}> = {
  File: {
    program: NODE,
  },
  ObjectProperty: {
    key: NODE,
    value: NODE,
  },
  ObjectMethod: {
    key: NODE,
    params: NODE_LIST,
    body: NODE,
    returnType: NODE,
    typeParameters: NODE_LIST,
  },
  ClassMethod: {
    key: NODE,
    params: NODE_LIST,
    body: NODE,
    returnType: NODE,
    typeParameters: NODE_LIST,
  },
  Import: {},

  // to help with the conversion from Hermes -> ESTree
  ClassProperty: {
    key: NODE,
    value: NODE,
    decorators: NODE_LIST,
    variance: NODE,
    typeAnnotation: NODE,
  },
  ClassPrivateProperty: {
    key: NODE,
    value: NODE,
    decorators: NODE_LIST,
    variance: NODE,
    typeAnnotation: NODE,
  },
  PrivateName: {
    id: NODE,
  },
  OptionalCallExpression: {
    callee: NODE,
    typeArguments: NODE,
    arguments: NODE_LIST,
  },
  OptionalMemberExpression: {
    object: NODE,
    property: NODE,
  },
  // We delete this from the ESTree AST but need it to exist for the parser visitor to correctly translate to the ESTree AST.
  ExportNamespaceSpecifier: {
    exported: NODE,
  },
};
export default customASTDefs;
