/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O -gc-sanitize-handles=0 %s | %FileCheck --match-full-lines %s

// With HV32, a young-gen collection compares the bits of each value, with the
// tag cleared, against the start of the segments being evacuated before it
// checks whether the value is a pointer. A number stored inline is the top 32
// bits of its double, so the numbers below match the start of every possible
// segment of at least 64 KB, except a few whose bits are NaNs, whether
// pointers are compressed or 32-bit raw addresses. Check that the collection
// leaves them alone, both in the cells it evacuates and in the dirty cards it
// scans.

var dv = new DataView(new ArrayBuffer(8));
function numberWithHighBits(hi) {
  dv.setUint32(0, hi);
  dv.setUint32(4, 0);
  return dv.getFloat64(0);
}

var expected = [];
for (var k = 0; k < 0x10000; ++k) {
  var n = numberWithHighBits(k * 0x10000);
  // NaNs are canonicalized, so their bits cannot be chosen.
  if (n === n)
    expected.push(n);
}

// Each chunk holds the numbers between two placeholder slots, which are later
// set to young objects to dirty the cards of the whole chunk.
var kChunkSize = 64;
var chunks = [];
for (var i = 0; i < expected.length; i += kChunkSize) {
  var chunk = [0];
  for (var j = i; j < i + kChunkSize && j < expected.length; ++j)
    chunk.push(expected[j]);
  chunk.push(0);
  chunks.push(chunk);
}

var errors = 0;
function checkChunks() {
  for (var c = 0; c < chunks.length; ++c) {
    var chunk = chunks[c];
    for (var j = 1; j < chunk.length - 1; ++j) {
      if (!Object.is(chunk[j], expected[c * kChunkSize + j - 1])) {
        print('Bad number in chunk ' + c + ' at ' + j);
        ++errors;
      }
    }
  }
}

// The chunks are young, so the collection evacuates them and visits their
// elements.
gc();
checkChunks();

// The chunks are now old. Store young objects into them, so that the next
// collection scans their dirty cards.
for (var c = 0; c < chunks.length; ++c) {
  chunks[c][0] = {};
  chunks[c][chunks[c].length - 1] = {};
}
gc();
checkChunks();

print(errors === 0 ? 'OK' : 'FAILED');
// CHECK: OK
