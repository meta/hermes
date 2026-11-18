/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -typed -dump-bytecode -fno-std-globals %s | %FileCheckOrRegen --match-full-lines %s

class A {
  x: number;
}

// Observe that the shape table has two entries.
// Typed and untyped objects can't share shape table (and cache) entries.
globalThis.a = new A();
globalThis.b = ({x: 1}: {[string]: number});

// Auto-generated content below. Please do not modify manually.

// CHECK:Bytecode File Information:
// CHECK-NEXT:  Bytecode version number: {{.*}}
// CHECK-NEXT:  Source hash: {{.*}}
// CHECK-NEXT:  Function count: 2
// CHECK-NEXT:  String count: 7
// CHECK-NEXT:  BigInt count: 0
// CHECK-NEXT:  String Kind Entry count: 2
// CHECK-NEXT:  RegExp count: 0
// CHECK-NEXT:  StringSwitchImm count: 0
// CHECK-NEXT:  Key buffer size (bytes): 3
// CHECK-NEXT:  Value buffer size (bytes): 10
// CHECK-NEXT:  Shape table count: 4
// CHECK-NEXT:  Segment ID: 0
// CHECK-NEXT:  CommonJS module count: 0
// CHECK-NEXT:  CommonJS module count (static): 0
// CHECK-NEXT:  Function source count: 0
// CHECK-NEXT:  Bytecode options:
// CHECK-NEXT:    staticBuiltins: 1
// CHECK-NEXT:    cjsModulesStaticallyResolved: 0

// CHECK:Global String Table:
// CHECK-NEXT:s0[ASCII, 0..0]: A
// CHECK-NEXT:s1[ASCII, 1..6]: global
// CHECK-NEXT:i2[ASCII, 1..10] #F3091991: globalThis
// CHECK-NEXT:i3[ASCII, 5..5] #00018270: a
// CHECK-NEXT:i4[ASCII, 11..11] #00018E43: b
// CHECK-NEXT:i5[ASCII, 12..20] #807C5F3D: prototype
// CHECK-NEXT:i6[ASCII, 21..21] #0001E7F9: x

// CHECK:Literal Value Buffer:
// CHECK-NEXT:[int 0]
// CHECK-NEXT:[int 1]

// CHECK:Object Key Buffer:
// CHECK-NEXT:[String 6]

// CHECK:Object Shape Table:
// CHECK-NEXT:0[0, 0]
// CHECK-NEXT:1[0, 0]
// CHECK-NEXT:2[0, 1]
// CHECK-NEXT:3[0, 1]

// CHECK:Function<global>(1 params, 4 registers, 0 numbers, 1 non-pointers):
// CHECK-NEXT:Offset in debug table: source 0x0000
// CHECK-NEXT:    LoadConstNull     r0
// CHECK-NEXT:    NewObjectWithParent r1, r0, 1
// CHECK-NEXT:    Mov               r2, r1
// CHECK-NEXT:    LoadConstUndefined r0
// CHECK-NEXT:    CreateClosure     r1, r0, Function<A>
// CHECK-NEXT:    PutByIdStrict     r1, r2, 0, "prototype"
// CHECK-NEXT:    GetGlobalObject   r1
// CHECK-NEXT:    TryGetById        r3, r1, 0, "globalThis"
// CHECK-NEXT:    NewTypedObjectWithBuffer r2, r2, 2, 0, 0
// CHECK-NEXT:    PutByIdStrict     r3, r2, 1, "a"
// CHECK-NEXT:    TryGetById        r2, r1, 0, "globalThis"
// CHECK-NEXT:    NewObjectWithBuffer r1, 3, 5
// CHECK-NEXT:    PutByIdStrict     r2, r1, 2, "b"
// CHECK-NEXT:    Ret               r0

// CHECK:Function<A>(1 params, 1 registers, 0 numbers, 1 non-pointers):
// CHECK-NEXT:    LoadConstUndefined r0
// CHECK-NEXT:    Ret               r0

// CHECK:Debug filename table:
// CHECK-NEXT:  0: {{.*}}class-shape-table.js

// CHECK:Debug file table:
// CHECK-NEXT:  source table offset 0x0000: filename id 0

// CHECK:Debug source table:
// CHECK-NEXT:  0x0000  function idx 0, starts at line 10 col 1
// CHECK-NEXT:    bc 19: line 10 col 1
// CHECK-NEXT:    bc 27: line 16 col 1
// CHECK-NEXT:    bc 45: line 16 col 14
// CHECK-NEXT:    bc 51: line 17 col 1
// CHECK-NEXT:    bc 63: line 17 col 14
// CHECK-NEXT:  0x0014  end of debug source table
