/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Reproduction of the OldGen::search dead-zone walk (dz 01a040a3-b290,
// fixed by "[SH] GC: Skip the HV64 dead-zone freelist bucket in
// OldGen::search").
// Timing-only, so it is a benchmark, not a lit test. On HV64 (8-byte
// GCCell header, minAllocationSize() == 16) phase 2 took 3.4-3.5 s before
// the fix and 1.85 s after; on HV32 (4-byte header, minAllocationSize()
// == 8 == the bucket step) there is no dead zone and the fix is a no-op.
// Run precompiled: hermes -emit-binary -out x.hbc <this> && hermes x.hbc
// This version avoids two interferences found with freelist
// instrumentation:
// pre-sized arrays (a growing array's storage request replaces
// allocChunk_ with a huge segment-tail remainder that then serves every
// promotion without a search), and precomputed pads (per-call concat
// temporaries were being promoted and later swept into the freelist).
var L = 40, D = 40000, F = 40000, N = 150000;
var pads = {};
function pad(n) { if (!pads[n]) { var p = ""; while (p.length < n) p += "x"; pads[n] = p; } return pads[n]; }
function mk(len, i) { var s = String(i); while (s.length < 6) s = "0" + s; return pad(len - 6) + s; }
pad(L - 6); pad(L + 2); pad(L + 34);
var all = new Array(2 * (D + F));
var live = new Array(N);
var j = 0;
for (var i = 0; i < D; ++i) { all[j++] = mk(L + 8, i); all[j++] = {k: i}; }
for (var i = 0; i < F; ++i) { all[j++] = mk(L + 40, i); all[j++] = {k: i}; }
gc();                                     // promote everything
for (var i = 0; i < all.length; i += 2) all[i] = null;
gc();                                     // sweep -> dead + feeder cells
var t0 = Date.now();
for (var i = 0; i < N; ++i) {
  live[i] = mk(L, i);
  if ((i & 1023) === 1023) { for (var g = 0; g < 60000; ++g) { var tmp = pad(L + 20) + g; } }  // garbage -> natural YG GCs promote the batch
}
var t1 = Date.now();
print("phase2 ms: " + (t1 - t0));
