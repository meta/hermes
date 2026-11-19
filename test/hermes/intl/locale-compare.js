/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s
// REQUIRES: intl

// String.prototype.localeCompare reuses its collator while the locales and
// options stay the same. A call with other arguments must not get the
// previous call's collator.

function assertEq(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(what + ': expected ' + expected + ', got ' + actual);
  }
}

function sorted(locales, options) {
  return ['Z', 'a', 'z', 'ä']
    .sort((x, y) => x.localeCompare(y, locales, options))
    .join();
}

for (var i = 0; i < 2; i++) {
  assertEq(sorted('de'), 'a,ä,z,Z', 'de');
  assertEq(sorted('sv'), 'a,z,Z,ä', 'sv');
  assertEq(sorted('sv', {caseFirst: 'upper'}), 'a,Z,z,ä', 'sv, upper first');
  assertEq(sorted('de', {caseFirst: 'upper'}), 'a,ä,Z,z', 'de, upper first');
  assertEq(
    'test10'.localeCompare('test2', 'en', {numeric: true}),
    1,
    'numeric',
  );
  assertEq('test10'.localeCompare('test2', 'en'), -1, 'not numeric');
}

// The options are still read on every call, even when the collator is reused.
var reads = 0;
var counted = {
  get sensitivity() {
    reads++;
    return 'base';
  },
};
for (var i = 0; i < 3; i++) {
  assertEq('a'.localeCompare('A', 'en', counted), 0, 'counted getter');
}
assertEq(reads, 3, 'sensitivity reads');

// A throwing getter throws with a warm cache, and the next call still works.
var threw = false;
try {
  'a'.localeCompare('A', 'en', {
    get sensitivity() {
      throw new Error('boom');
    },
  });
} catch (e) {
  threw = e.message === 'boom';
}
assertEq(threw, true, 'throwing getter');
assertEq('a'.localeCompare('A', 'en', counted), 0, 'after throwing getter');

// A getter that calls localeCompare with other arguments does not disturb the
// outer call.
var reentrant = {
  get sensitivity() {
    assertEq(sorted('sv'), 'a,z,Z,ä', 'inner sv');
    return 'base';
  },
};
assertEq('a'.localeCompare('A', 'de', reentrant), 0, 'outer de');
assertEq('ä'.localeCompare('z', 'de', reentrant), -1, 'outer de after inner');

// An invalid option throws and leaves later calls correct.
threw = false;
try {
  'a'.localeCompare('b', 'en', {usage: 'order'});
} catch (e) {
  threw = e instanceof RangeError;
}
assertEq(threw, true, 'invalid usage');
assertEq(sorted('de'), 'a,ä,z,Z', 'de after invalid usage');
