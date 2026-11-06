/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- a | %FileCheck --check-prefix=OUTA %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- b | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- c | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -gc-sanitize-handles=1 %s -- c | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- d | %FileCheck --check-prefix=OUTD %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- a 2>&1 | %FileCheck --check-prefix=CNTA %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- b 2>&1 | %FileCheck --check-prefix=CNTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- c 2>&1 | %FileCheck --check-prefix=CNTC %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- d 2>&1 | %FileCheck --check-prefix=CNTD %s
// REQUIRES: jit

// Demoted sites and string receivers (spec: "Progress", after the table),
// one scenario per process. f(o, q) holds two sites: S (`o.foo`, run when
// q is null; String.prototype.foo is 'F') and T (`q.bar` on accT, an own
// accessor: never recorded, every call declines). T never runs before
// phase 3, so it is still recording then (an unseen site is never
// demoted: the !s.seen test in considerRecompile), and its first
// crossing is a crossing of the same body. acc is an own accessor `foo`
// ('G'): S declines on it forever. Phases: 1 demotes S; 2 reads 8 strings at S; 3
// runs T 16 times (one crossing; T's own first crossing changes, nothing
// of T's progresses); 4 reads 8 strings at S.
// (a) Phase 1: 64 acc calls. Crossings at calls 16 (S's first seen:
//     changed), 32, 48 and 64; nothing progresses; the fourth demotes S
//     to the plain helper, which records nothing, so strings never set
//     sawString: phase 3's crossing has nothing to do. Hits 0,
//     NumRecompiles 0, NumByIdDemotions 1. A regression pin: it passes
//     before the feature too, and no mutation of this plan's code reaches
//     it.
// (b) Phase 1: 112 calls alternating `chained` (foo two prototypes up:
//     a chain entry) and acc. Call 16 recompiles (the unemitted chain
//     entry); version 2's chain entry hits on the 48 chained calls 17-111
//     (odd calls, i even); its 48 acc calls cross at calls 48, 80 and 112
//     with nothing to learn, and the third demotes S to
//     _jit_get_by_id_refresh (an emitted chain entry), which records
//     nothing. Hits 0, NumRecompiles 1, NumByIdDemotions 1,
//     NumByIdChainHits 48. Fails if the refresh helper recorded strings
//     (phase 3's crossing would recompile on NotPrepared; phase 4 would
//     hit 8).
// (c) Phase 1: 112 calls alternating 'abc' and acc. Call 16 recompiles
//     (NotPrepared); version 2's Method block hits the 48 strings of
//     calls 17-112, its 48 acc calls demote S at call 112 (crossings at
//     48, 80, 112). An unrelated property then changes String.prototype's
//     class: phase 2's strings miss into the plain helper (no declines).
//     Phase 3's crossing finds S's class-ID term (0 against the stored ID)
//     and recompiles; version 3 prepares S again (its slot stays
//     demoted), so phase 4's 8 strings hit. Hits 56, NumRecompiles 2,
//     NumByIdDemotions 1. Fails without the class-ID term (48 hits).
// (d) As (c) without phase 3: no crossing, so no recompile; phases 2 and
//     4 read through the plain helper. Hits 48, NumRecompiles 1. Fails if
//     the guard is not checked (64 hits: foo keeps its slot, so the
//     values alone cannot tell).
function main(scenario) {
  var SP = String.prototype;
  SP.foo = 'F';
  function f(o, q) {
    return q ? q.bar : o.foo;
  }
  // n calls of f(x, q); how many returned want.
  function count(f, x, q, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x, q) === want)
        ++c;
    return c;
  }
  // n calls alternating f(a, null) (i even) and f(b, null) (i odd);
  // "<matches of wa> <matches of wb>".
  function alt(f, a, wa, b, wb, n) {
    var ca = 0;
    var cb = 0;
    for (var i = 0; i < n; ++i) {
      if (i & 1) {
        if (f(b, null) === wb)
          ++cb;
      } else if (f(a, null) === wa) {
        ++ca;
      }
    }
    return ca + ' ' + cb;
  }
  var acc = {
    get foo() {
      return 'G';
    },
  };
  var accT = {
    get bar() {
      return 'B';
    },
  };
  if (scenario === 'a') {
    print(count(f, acc, null, 64, 'G'));
  } else if (scenario === 'b') {
    var chained = Object.create(Object.create({foo: 'C'}));
    print(alt(f, chained, 'C', acc, 'G', 112));
  } else {
    print(alt(f, 'abc', 'F', acc, 'G', 112));
  }
  if (scenario === 'c' || scenario === 'd')
    SP.unrelated = 1;
  print(count(f, 'abc', null, 8, 'F'));
  if (scenario !== 'd')
    print(count(f, 'abc', accT, 16, 'B'));
  print(count(f, 'abc', null, 8, 'F'));
}
main(hermescli.getScriptArgs()[0]);

// OUTA: 64
// OUTA-NEXT: 8
// OUTA-NEXT: 16
// OUTA-NEXT: 8
// OUTB: 56 56
// OUTB-NEXT: 8
// OUTB-NEXT: 16
// OUTB-NEXT: 8
// OUTD: 56 56
// OUTD-NEXT: 8
// OUTD-NEXT: 8
// CNTA: NumRecompiles: 0{{$}}
// CNTA: NumByIdDemotions: 1{{$}}
// CNTA: NumByIdStringHits: 0{{$}}
// CNTB: NumRecompiles: 1{{$}}
// CNTB: NumByIdDemotions: 1{{$}}
// CNTB: NumByIdChainHits: 48{{$}}
// CNTB: NumByIdStringHits: 0{{$}}
// CNTC: NumRecompiles: 2{{$}}
// CNTC: NumByIdDemotions: 1{{$}}
// CNTC: NumByIdStringHits: 56{{$}}
// CNTD: NumRecompiles: 1{{$}}
// CNTD: NumByIdDemotions: 1{{$}}
// CNTD: NumByIdStringHits: 48{{$}}
