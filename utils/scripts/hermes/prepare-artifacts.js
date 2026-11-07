/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow
 * @format
 */

'use strict';

const fs = require('fs');
const path = require('path');
const {chmod, cp, echo, exit, ls, mkdir, test} = require('shelljs');

function copyHermesBinaries() {
  const HERMES_WS_DIR = process.env.HERMES_WS_DIR || '/tmp/hermes';

  echo('Copying Hermes binaries...');

  // Create directories for npm/hermes-compiler/hermesc binaries
  mkdir('-p', './npm/hermes-compiler/hermesc/osx-bin');
  mkdir('-p', './npm/hermes-compiler/hermesc/linux64-bin');
  mkdir('-p', './npm/hermes-compiler/hermesc/linuxarm64-bin');
  mkdir('-p', './npm/hermes-compiler/hermesc/win64-bin');

  const osxBinDir = path.join(HERMES_WS_DIR, 'osx-bin');
  const osxReleaseDir = path.join(osxBinDir, 'Release');
  const osxDebugDir = path.join(osxBinDir, 'Debug');

  if (test('-d', osxReleaseDir)) {
    echo('Copying macOS Release hermesc binary...');
    cp(
      '-r',
      path.join(osxReleaseDir, 'hermesc'),
      './npm/hermes-compiler/hermesc/osx-bin/hermesc',
    );
  } else if (test('-d', osxDebugDir)) {
    echo('Copying macOS Debug hermesc binary...');
    cp(
      '-r',
      path.join(osxDebugDir, 'hermesc'),
      './npm/hermes-compiler/hermesc/osx-bin/hermesc',
    );
  } else {
    if (test('-d', osxBinDir)) {
      ls(osxBinDir);
    } else {
      echo('hermesc macOS artifacts directory missing.');
    }
    echo('Could not locate macOS hermesc binary.');
    exit(1);
  }

  // Sometimes, GHA creates artifacts with lowercase Debug/Release. Make sure that if it happen, we uppercase them.
  const darwinDir = path.join(HERMES_WS_DIR, 'hermes-runtime-darwin');
  const debugTarLower = path.join(darwinDir, 'hermes-ios-debug.tar.gz');
  const debugTarUpper = path.join(darwinDir, 'hermes-ios-Debug.tar.gz');
  const releaseTarLower = path.join(darwinDir, 'hermes-ios-release.tar.gz');
  const releaseTarUpper = path.join(darwinDir, 'hermes-ios-Release.tar.gz');

  if (test('-f', debugTarLower)) {
    echo('Renaming debug tar to uppercase...');
    fs.renameSync(debugTarLower, debugTarUpper);
  }

  if (test('-f', releaseTarLower)) {
    echo('Renaming release tar to uppercase...');
    fs.renameSync(releaseTarLower, releaseTarUpper);
  }

  echo('Copying Windows binaries...');
  cp(
    '-r',
    path.join(HERMES_WS_DIR, 'win64-bin/*'),
    './npm/hermes-compiler/hermesc/win64-bin/.',
  );

  echo('Copying Linux binaries...');
  cp(
    '-r',
    path.join(HERMES_WS_DIR, 'linux64-bin/*'),
    './npm/hermes-compiler/hermesc/linux64-bin/.',
  );
  echo('Copying Linux ARM64 binary...');
  const linuxArm64Hermesc = path.join(HERMES_WS_DIR, 'linuxarm64-bin', 'hermesc');
  if (!test('-f', linuxArm64Hermesc)) {
    echo('Could not locate Linux ARM64 hermesc binary.');
    exit(1);
  }
  cp(
    linuxArm64Hermesc,
    './npm/hermes-compiler/hermesc/linuxarm64-bin/.',
  );

  echo('Making hermesc files executable...');
  chmod('-R', '+x', 'npm/hermes-compiler/hermesc/*');

  const iosArtifactsDir = path.join('android', 'ios-artifacts', 'artifacts');
  mkdir('-p', iosArtifactsDir);

  echo('Copying iOS artifacts...');
  cp(debugTarUpper, path.join(iosArtifactsDir, 'hermes-ios-debug.tar.gz'));
  cp(releaseTarUpper, path.join(iosArtifactsDir, 'hermes-ios-release.tar.gz'));
  cp(
    path.join(HERMES_WS_DIR, 'dSYM', 'Debug', 'hermesvm.framework.dSYM'),
    path.join(iosArtifactsDir, 'hermes-framework-dSYM-debug.tar.gz'),
  );
  cp(
    path.join(HERMES_WS_DIR, 'dSYM', 'Release', 'hermesvm.framework.dSYM'),
    path.join(iosArtifactsDir, 'hermes-framework-dSYM-release.tar.gz'),
  );

  echo('Hermes binaries copied successfully!');
}

async function main() {
  copyHermesBinaries();
}

if (require.main === module) {
  void main();
}
