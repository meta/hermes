/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -target=HBC -dump-bytecode -O -fstatic-builtins %s | %FileCheckOrRegen --match-full-lines %s
// RUN: %hermes -target=HBC -dump-bytecode -O0 -fstatic-builtins %s | %FileCheck --check-prefix=O0 --implicit-check-not=CallBuiltin %s

// The Imul opcode and its encoding, pinned the way the rest of the
// three-register arithmetic family is pinned by binary.js in this directory.
// That file cannot host this one: Math.imul has no source operator, so it
// only becomes an instruction under -fstatic-builtins, which binary.js does
// not pass.
//
// The second function is the one-argument call, which reaches the same
// opcode through a padded operand. The extra constant register in its body
// is that padding: the missing argument becomes a literal 0, not a
// CallBuiltin to Math.imul.
//
// The lowering does not ride on the optimizer. LowerBuiltinCalls runs in the
// backend pipeline at every optimization level, and the peephole runs with
// it, so -O0 reaches the opcode too. That is what the second RUN line pins,
// and it is not redundant: the -O pipeline rewrites these calls earlier, in
// LowerBuiltinCallsOptimized, so every check below would still pass if the
// unoptimized pipeline alone stopped lowering them. One Imul per function,
// and no CallBuiltin anywhere in the dump.
// O0: Imul
// O0: Imul

function imul() {
  var x = foo(), y = foo();
  return Math.imul(x, y);
}

function oneArg() {
  var x = foo();
  return Math.imul(x);
}

function foo() { return; }

// Auto-generated content below. Please do not modify manually.

// CHECK:Bytecode File Information:
// CHECK-NEXT:  Bytecode version number: {{.*}}
// CHECK-NEXT:  Source hash: {{.*}}
// CHECK-NEXT:  Function count: 4
// CHECK-NEXT:  String count: 4
// CHECK-NEXT:  BigInt count: 0
// CHECK-NEXT:  String Kind Entry count: 2
// CHECK-NEXT:  RegExp count: 0
// CHECK-NEXT:  StringSwitchImm count: 0
// CHECK-NEXT:  Key buffer size (bytes): 0
// CHECK-NEXT:  Value buffer size (bytes): 0
// CHECK-NEXT:  Shape table count: 1
// CHECK-NEXT:  Segment ID: 0
// CHECK-NEXT:  CommonJS module count: 0
// CHECK-NEXT:  CommonJS module count (static): 0
// CHECK-NEXT:  Function source count: 0
// CHECK-NEXT:  Bytecode options:
// CHECK-NEXT:    staticBuiltins: 1
// CHECK-NEXT:    cjsModulesStaticallyResolved: 0

// CHECK:Global String Table:
// CHECK-NEXT:s0[ASCII, 7..12]: global
// CHECK-NEXT:i1[ASCII, 0..2] #9290584E: foo
// CHECK-NEXT:i2[ASCII, 2..7] #D84C6FDB: oneArg
// CHECK-NEXT:i3[ASCII, 13..16] #17510F77: imul

// CHECK:Object Shape Table:
// CHECK-NEXT:0[0, 0]

// CHECK:Function<global>(1 params, 3 registers, 0 numbers, 1 non-pointers):
// CHECK-NEXT:Offset in debug table: source 0x0000
// CHECK-NEXT:    DeclareGlobalVar  "imul"
// CHECK-NEXT:    DeclareGlobalVar  "oneArg"
// CHECK-NEXT:    DeclareGlobalVar  "foo"
// CHECK-NEXT:    GetGlobalObject   r2
// CHECK-NEXT:    LoadConstUndefined r0
// CHECK-NEXT:    CreateClosure     r1, r0, Function<imul>
// CHECK-NEXT:    PutByIdLoose      r2, r1, 0, "imul"
// CHECK-NEXT:    CreateClosure     r1, r0, Function<oneArg>
// CHECK-NEXT:    PutByIdLoose      r2, r1, 1, "oneArg"
// CHECK-NEXT:    CreateClosure     r1, r0, Function<foo>
// CHECK-NEXT:    PutByIdLoose      r2, r1, 2, "foo"
// CHECK-NEXT:    Ret               r0

// CHECK:Function<imul>(1 params, 12 registers, 1 numbers, 1 non-pointers):
// CHECK-NEXT:Offset in debug table: source 0x0017
// CHECK-NEXT:    GetGlobalObject   r2
// CHECK-NEXT:    GetByIdShort      r3, r2, 0, "foo"
// CHECK-NEXT:    LoadConstUndefined r1
// CHECK-NEXT:    Call1             r3, r3, r1
// CHECK-NEXT:    GetByIdShort      r2, r2, 0, "foo"
// CHECK-NEXT:    Call1             r2, r2, r1
// CHECK-NEXT:    Imul              r0, r3, r2
// CHECK-NEXT:    Ret               r0

// CHECK:Function<oneArg>(1 params, 11 registers, 1 numbers, 1 non-pointers):
// CHECK-NEXT:Offset in debug table: source 0x002b
// CHECK-NEXT:    GetGlobalObject   r2
// CHECK-NEXT:    GetByIdShort      r2, r2, 0, "foo"
// CHECK-NEXT:    LoadConstUndefined r1
// CHECK-NEXT:    Call1             r2, r2, r1
// CHECK-NEXT:    LoadConstZero     r0
// CHECK-NEXT:    Imul              r0, r2, r0
// CHECK-NEXT:    Ret               r0

// CHECK:Function<foo>(1 params, 1 registers, 0 numbers, 1 non-pointers):
// CHECK-NEXT:    LoadConstUndefined r0
// CHECK-NEXT:    Ret               r0

// CHECK:Debug filename table:
// CHECK-NEXT:  0: {{.*}}imul.js

// CHECK:Debug file table:
// CHECK-NEXT:  source table offset 0x0000: filename id 0

// CHECK:Debug source table:
// CHECK-NEXT:  0x0000  function idx 0, starts at line 32 col 1
// CHECK-NEXT:    bc 0: line 32 col 1
// CHECK-NEXT:    bc 5: line 32 col 1
// CHECK-NEXT:    bc 10: line 32 col 1
// CHECK-NEXT:    bc 24: line 32 col 1
// CHECK-NEXT:    bc 35: line 32 col 1
// CHECK-NEXT:    bc 46: line 32 col 1
// CHECK-NEXT:  0x0017  function idx 1, starts at line 32 col 1
// CHECK-NEXT:    bc 2: line 33 col 11
// CHECK-NEXT:    bc 9: line 33 col 14
// CHECK-NEXT:    bc 13: line 33 col 22
// CHECK-NEXT:    bc 18: line 33 col 25
// CHECK-NEXT:    bc 22: line 34 col 19
// CHECK-NEXT:  0x002b  function idx 2, starts at line 37 col 1
// CHECK-NEXT:    bc 2: line 38 col 11
// CHECK-NEXT:    bc 9: line 38 col 14
// CHECK-NEXT:    bc 15: line 39 col 19
// CHECK-NEXT:  0x0039  end of debug source table
