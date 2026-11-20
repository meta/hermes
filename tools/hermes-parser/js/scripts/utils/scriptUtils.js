/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict
 * @format
 */

// @licenselint-loose-mode

'use strict';

import fs from 'fs';
import mkdirp from 'mkdirp';
import path from 'path';
import * as prettier from 'prettier';
import aliasDefs from './aliases';
// $FlowExpectedError[cannot-resolve-module]
import prettierConfig from '../../.prettierrc.json';

export type ESTreeJSON = ReadonlyArray<
  Readonly<{
    name: string,
    base: string,
    arguments: ReadonlyArray<
      Readonly<{
        type:
          | 'NodeLabel'
          | 'NodeString'
          | 'NodeBoolean'
          | 'NodeNumber'
          | 'NodePtr'
          | 'NodeList',
        name: string,
        optional: boolean,
      }>,
    >,
  }>,
>;

export const HermesESTreeJSONFile: string = path.resolve(
  __dirname,
  '../dist/HermesESTreeJSON.json',
);
// $FlowExpectedError[unsupported-syntax]
export const GetHermesESTreeJSON: () => ESTreeJSON = () =>
  // $FlowExpectedError[unsupported-syntax]
  require(HermesESTreeJSONFile);

const isFlowTransform = process.env.HERMES_TRANSFORM_TARGET === 'flow';

export const TransformESTreePackage: 'hermes-estree' | 'flow-estree' =
  isFlowTransform ? 'flow-estree' : 'hermes-estree';
export const TransformPackage: 'hermes-transform' | 'flow-transform' =
  isFlowTransform ? 'flow-transform' : 'hermes-transform';
export const TransformReadonly: 'readonly ' | '+' = isFlowTransform
  ? '+'
  : 'readonly ';

export const GetTransformESTreeJSON: () => ESTreeJSON = () => {
  const nodes = GetHermesESTreeJSON();
  if (!isFlowTransform) {
    return nodes;
  }

  return nodes.map(node =>
    node.name === 'DeclareVariable'
      ? {
          ...node,
          arguments: [
            {type: 'NodeList', name: 'declarations', optional: false},
            {type: 'NodeString', name: 'kind', optional: false},
            {type: 'NodeBoolean', name: 'implicitDeclare', optional: false},
          ],
        }
      : node,
  );
};

type FlowStyle = false | 'loose' | 'strict' | 'strict-local';
function HEADER(flow: FlowStyle, skipFormat: boolean): string {
  let flowDirective = ``;
  if (flow === false) {
    flowDirective += `${'@'}noflow`;
  } else {
    flowDirective += `${'@'}flow`;
    if (flow !== 'loose') {
      flowDirective += ` ${flow}`;
    }
  }
  const formatDirective = skipFormat ? '' : '\n * @format';

  return `\
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * ${flowDirective}${formatDirective}
 * ${'@'}generated
 */

/*
 * !!! GENERATED FILE !!!
 *
 * Any manual changes to this file will be overwritten. To regenerate run \`yarn build\`.
 */

// lint directives to let us do some basic validation of generated files
/* eslint no-undef: 'error', no-unused-vars: ['error', {vars: "local"}], no-redeclare: 'error' */
/* global NonNullable, Partial, Readonly, ReadonlyArray, $FlowFixMe */

'use strict';

`;
}

type Package =
  | 'hermes-eslint'
  | 'hermes-estree'
  | 'hermes-parser'
  | 'hermes-transform'
  | 'flow-transform'
  | 'prettier-plugin-hermes-parser'
  | 'babel-plugin-syntax-hermes-parser';

type ArtifactOptions = Readonly<{
  code: string,
  flow?: FlowStyle,
  // Writes to ../<package>/<file> unless a transform package override is set.
  package: Package,
  file: string,
  skipFormat?: boolean,
}>;

export async function formatAndWriteDistArtifact(
  opts: ArtifactOptions,
): Promise<void> {
  await formatAndWriteArtifact({
    ...opts,
    file: path.join('dist', opts.file),
  });
}
export async function formatAndWriteSrcArtifact(
  opts: ArtifactOptions,
): Promise<void> {
  await formatAndWriteArtifact({
    ...opts,
    file: path.join('src', opts.file),
  });
}

async function formatAndWriteArtifact({
  code: code_,
  flow = 'loose',
  package: pkg,
  file,
  skipFormat = false,
}: ArtifactOptions): Promise<void> {
  // make sure the code has a header
  const code =
    code_.slice(0, 3) === '/**' ? code_ : HEADER(flow, skipFormat) + code_;

  // Format the file
  const formattedContents = skipFormat
    ? code
    : await prettier.format(code, {
        ...prettierConfig,
        parser: 'flow',
      });

  // make sure the folder exists first
  const packageRoot =
    pkg === TransformPackage && process.env.HERMES_TRANSFORM_PACKAGE_DIR != null
      ? process.env.HERMES_TRANSFORM_PACKAGE_DIR
      : path.resolve(__dirname, '..', '..', pkg);
  const folder = path.resolve(packageRoot, path.dirname(file));
  mkdirp.sync(folder);
  // write to disk
  const artifactPath = path.resolve(folder, path.basename(file));
  fs.writeFileSync(artifactPath, formattedContents);
}

export const LITERAL_TYPES: ReadonlySet<string> = new Set([
  'BigIntLiteral',
  'BooleanLiteral',
  'NullLiteral',
  'NumericLiteral',
  'RegExpLiteral',
  'StringLiteral',
]);

export const FLIPPED_ALIAS_KEYS: Readonly<{
  [key: string]: ReadonlySet<string>,
}> = (() => {
  // $FlowExpectedError[incompatible-type]
  const flippedAliasKeys: {[key: string]: Set<string>} = Object.create(null);

  for (let typeName of Object.keys(aliasDefs)) {
    for (let aliasName of aliasDefs[typeName]) {
      if (flippedAliasKeys[aliasName]) {
        flippedAliasKeys[aliasName].add(typeName);
      } else {
        flippedAliasKeys[aliasName] = new Set([typeName]);
      }
    }
  }

  return flippedAliasKeys;
})();

export const EXCLUDE_PROPERTIES_FROM_NODE: ReadonlyMap<
  string,
  ReadonlySet<string>,
> = new Map([
  // This property is only needed for TS
  ['PropertyDefinition', new Set(['tsModifiers'])],
]);
