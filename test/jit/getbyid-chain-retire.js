/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// A retired body still on the stack. rec() reads o.m once before and
// once after recursing (one site, executed twice per frame).
// Phase 1: 8 calls rec(o, 0, noop), 16 declines in version 1 (call 1
// records the chain entry); the 16th recompiles: version 2 emits it.
// Phase 2: rec(o, 4, hook) runs version 2 in frames d = 4..0, each
// reading before recursing: 5 hits. At d = 0, hook(0) drives rec with
// three own-property shapes: each call's first read declines and
// records an own entry, its second hits the generic tier; the 16th
// crosses, the own entries are progress, and version 3 is installed,
// retiring version 2 (its slot goes to the plain helper; its entries
// keep their epochs). Unwinding, frames d = 0 and 1 read after
// recursing: the retired body's chain entry still hits (2 hits). hook(2)
// adds m to mid, a flagged object: a bump and a shadowing property.
// Frames d = 2, 3, 4 then miss and read 100 through the plain helper.
// rec(o, 4, hook) = 1+(1+(1+(1+(1+1)+1)+100)+100)+100 = 307. The final
// rec(o, 0, noop) runs version 3, whose copied entry is stale: 200.
// NumByIdChainHits = 5 + 2 = 7; NumRecompiles = 2.
// Fails if retired chain checks miss (5 hits), and if the epoch compare
// is missing (frames d = 2..4 read 1, not 100).
// hook's own declines are harmless: `own[i % 3]` is an in-bounds array
// read and `mid.m = 100` a single PutById miss, so hook never reaches
// 16 declines, and it has no GetById site to demote or record.
function main() {
  var base = {m: 1};
  var mid = Object.create(base);
  var o = Object.create(mid);
  var own = [{m: 1000}, {a: 0, m: 1000}, {b: 0, m: 1000}];
  function noop(d) {}
  function rec(o, d, hook) {
    var s = 0;
    for (var k = 0; k < 2; ++k) {
      s += o.m;
      if (k === 0) {
        if (d > 0)
          s += rec(o, d - 1, hook);
        hook(d);
      }
    }
    return s;
  }
  function hook(d) {
    if (d === 0) {
      for (var i = 0; i < 16; ++i)
        rec(own[i % 3], 0, noop);
    } else if (d === 2) {
      mid.m = 100;
    }
  }
  for (var i = 0; i < 8; ++i)
    rec(o, 0, noop);
  var r = rec(o, 4, hook);
  print(r, rec(o, 0, noop));
}
main();

// OUT: 307 200
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'rec' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec' (version 3)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'rec' (version 3)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdChainHits: 7{{$}}
