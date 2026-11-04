/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int && %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline %s > %t.int && %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -fno-inline -O0 %s > %t.int0 && %hermes -fno-inline -O0 -Xjit=force -Xjit-crash-on-error %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -fno-inline -O0 %s > %t.int0 && %hermes -fno-inline -O0 -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit3 && diff %t.int0 %t.jit3
// RUN: %hermes -fno-inline -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// REQUIRES: jit

// `arguments`: GetArgumentsLength, GetArgumentsPropByValLoose/Strict and
// ReifyArgumentsLoose/Strict.
//
// The four differential RUN lines are the real check, at -O and -O0, with and
// without type asserts, all under -Xjit-crash-on-error because every function
// here compiles in all four. The two -Xdump-jitcode=2 lines pin the compile
// status.
//
// THE LAZY REGISTER IS THE WHOLE STORY. `arguments` is not materialized on
// entry. A frame register -- the "lazy register" -- starts out undefined and
// holds the reified Arguments OBJECT once anything forces it into existence.
// All five opcodes branch on exactly that: is the lazy register an object?
//   - GetArgumentsLength: if not, the count is read straight out of the
//     frame's ArgCount slot, which stores a NativeUint32 in the low 32 bits
//     of the HermesValue and is converted to a double in the emitted code.
//     If it is an object, the runtime reads .length off the object.
//   - GetArgumentsPropByVal*: if not -- and the index is an exact integer in
//     [0, argCount) -- the value is loaded from the caller's outgoing
//     argument slot at framePtr[FirstArg - index]. Everything else goes to
//     the runtime.
//   - ReifyArguments*: if it IS already an object, do nothing; otherwise
//     call the runtime to build one. The fast path is literally a compare
//     and a not-taken branch.
// Every function below is called at least once on each side of that branch,
// so no emitter here is only ever seen on one path. `bothWays` in particular
// reads an argument, forces reification, and reads again -- one compiled
// function, both paths through the same GetArgumentsPropByVal.
//
// THE INDEX CHECK IS x86-SPECIFIC IN ONE PLACE. The fast path admits an
// index only if emit_double_is_int() accepts it AND it is unsigned-below the
// argument count. arm64 gets the NaN case for free (its fcmp reports
// unordered as NE); x86's vucomisd reports unordered as EQUAL, so
// getArgumentsPropByValImpl branches on jp as well as jne. `idx` is called
// with a NaN index below. That particular value would be rejected by the
// bounds check anyway -- vcvttsd2si turns a NaN into INT64_MIN, which is
// enormous unsigned -- so this is a belt-and-braces case, and it is called
// out here rather than left looking load-bearing.
//
// NEGATIVE AND OUT-OF-RANGE INDICES share one branch. The conversion
// sign-extends, so a negative index becomes a huge unsigned value and the
// single unsigned compare against ArgCount rejects both directions at once.
// Both are exercised.
//
// THE ADDRESSING IS NOT arm64's. Arguments live BELOW the frame pointer, at
// framePtr[FirstArg - index]. arm64 computes FirstArg - index into a
// register; x86 has no negative index scale, so getArgumentsPropByValImpl
// negates the index and folds FirstArg's byte offset into the memory
// operand's displacement. `idx` is called with the first, the last and an
// interior index of a five-argument call, so an off-by-one or a sign error
// in that displacement shows up as a wrong argument rather than a crash.

function say(x) {
  print(x);
}

// Loose mode. Both are read on the fast path here -- nothing in either
// function reifies.
function looseLen() {
  return arguments.length;
}
function looseIdx(i) {
  return arguments[i];
}

// Strict mode: a different pair of opcodes with the same fast paths.
function strictLen() {
  "use strict";
  return arguments.length;
}
function strictIdx(i) {
  "use strict";
  return arguments[i];
}

// Declared parameters do not change the picture: the count and the slots
// come from the CALL, not from the signature. Called below with more and
// with fewer arguments than it declares.
function withParams(a, b, c) {
  return arguments.length + ":" + a + "," + b + "," + c;
}

// Every index shape, in one compiled function, on the fast path: in range,
// at both ends, out of range in both directions, fractional, NaN, -0, and a
// string that is not an index at all. Always called with five extra
// arguments, so the valid indices are 0 (the index itself) through 5.
function idx(i) {
  return String(arguments[i]);
}

// Reification, loose: assigning through `arguments` forces the object into
// existence, and everything after that point is on the slow path.
function reifyLoose(a) {
  var before = arguments[0];
  arguments[0] = "written";
  return before + "/" + arguments[0] + "/" + arguments.length + "/" + a;
}

// Reification, strict: returning the object is what forces it. In strict
// mode the object is a snapshot -- writing to it does not write the
// parameter, and writing the parameter does not change it.
function reifyStrict(a) {
  "use strict";
  var o = arguments;
  a = "param-changed";
  return o.length + "/" + o[0] + "/" + a;
}

// Both paths through ONE GetArgumentsPropByVal and ONE GetArgumentsLength:
// the loop reads arguments[k] and arguments.length on every iteration, and
// the middle iteration reifies. Only the emitter's own branch decides which
// path each iteration takes.
function bothWays(n) {
  var s = "";
  for (var k = 0; k < n; ++k) {
    s += arguments[k] + "@" + arguments.length + " ";
    if (k === 1) arguments[0] = "R";
  }
  // Read slot 0 once more, now that the object exists. Its frame slot still
  // holds the original value, so this prints "R" only if the read went
  // through the reified object -- i.e. only if the is-object branch works.
  return s + "|" + arguments[0] + "@" + arguments.length;
}

// The lazy register when there is nothing to be lazy about: zero arguments.
function noArgs() {
  return arguments.length + "/" + String(arguments[0]);
}

// Reify without ever reading an element, so the ReifyArguments fast path
// (already an object) is reached on the second call within one frame.
function reifyTwice() {
  var a = arguments;
  var b = arguments;
  return (a === b) + "/" + a.length;
}

// ReifyArgumentsStrict survives optimization only when the object ESCAPES.
// Any use that stays inside the function -- reifyStrict's o.length and o[0]
// above -- is rewritten back into GetArgumentsLength and
// GetArgumentsPropByValStrict at -O, so without this function the strict
// reify emitter would be covered at -O0 only.
function escapeStrict() {
  "use strict";
  return arguments;
}

// Non-integer values living in argument slots, so the raw 8-byte load has to
// be the HermesValue and not a re-encoded one.
function echoAll() {
  var s = "";
  for (var k = 0; k < arguments.length; ++k) s += typeof arguments[k] + ",";
  return s;
}

// Lengths: no arguments, exactly the declared count, and well past it.
// The pins follow CALL order, not source order.
say(looseLen() + " " + looseLen(1) + " " + looseLen(1, 2, 3, 4, 5, 6, 7));
// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK: JIT successfully compiled FunctionID 2, 'looseLen'
// CHECK: JIT successfully compiled FunctionID 1, 'say'
// CHECK-NEXT: 0 1 7
// CHECK0: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 2, 'looseLen'
// CHECK0: JIT successfully compiled FunctionID 1, 'say'
// CHECK0-NEXT: 0 1 7
say(strictLen() + " " + strictLen(1) + " " + strictLen(1, 2, 3, 4, 5, 6, 7));
// CHECK: JIT successfully compiled FunctionID 4, 'strictLen'
// CHECK-NEXT: 0 1 7
// CHECK0: JIT successfully compiled FunctionID 4, 'strictLen'
// CHECK0-NEXT: 0 1 7

// Indices, loose and strict, in range and out. Slot 0 is the index argument
// itself, so looseIdx(0, ...) answers 0.
say(looseIdx(0, "zero") + " " + looseIdx(1, "zero", "one") + " " +
    String(looseIdx(9, "zero")));
// CHECK: JIT successfully compiled FunctionID 3, 'looseIdx'
// CHECK-NEXT: 0 zero undefined
// CHECK0: JIT successfully compiled FunctionID 3, 'looseIdx'
// CHECK0-NEXT: 0 zero undefined
say(strictIdx(0, "zero") + " " + strictIdx(1, "zero", "one") + " " +
    String(strictIdx(9, "zero")));
// CHECK: JIT successfully compiled FunctionID 5, 'strictIdx'
// CHECK-NEXT: 0 zero undefined
// CHECK0: JIT successfully compiled FunctionID 5, 'strictIdx'
// CHECK0-NEXT: 0 zero undefined

// Declared parameters, over- and under-supplied.
say(withParams(1, 2, 3));
// CHECK: JIT successfully compiled FunctionID 6, 'withParams'
// CHECK-NEXT: 3:1,2,3
// CHECK0: JIT successfully compiled FunctionID 6, 'withParams'
// CHECK0-NEXT: 3:1,2,3
say(withParams(1));
// CHECK-NEXT: 1:1,undefined,undefined
// CHECK0-NEXT: 1:1,undefined,undefined
say(withParams(1, 2, 3, 4, 5));
// CHECK-NEXT: 5:1,2,3
// CHECK0-NEXT: 5:1,2,3

// The index shapes. arguments[0] is the index itself, so the interesting
// slots are 1..5: first, last and interior.
say(idx(1, "a", "b", "c", "d", "e") + " " + idx(5, "a", "b", "c", "d", "e") +
    " " + idx(3, "a", "b", "c", "d", "e"));
// CHECK: JIT successfully compiled FunctionID 7, 'idx'
// CHECK-NEXT: a e c
// CHECK0: JIT successfully compiled FunctionID 7, 'idx'
// CHECK0-NEXT: a e c
// One past the end, negative, and fractional: all three take the slow path,
// and all three answer undefined.
say(idx(6, "a", "b", "c", "d", "e") + " " + idx(-1, "a", "b", "c", "d", "e") +
    " " + idx(1.5, "a", "b", "c", "d", "e"));
// CHECK-NEXT: undefined undefined undefined
// CHECK0-NEXT: undefined undefined undefined
// NaN, -0 (which IS the index 0, and index 0 is the index argument itself),
// a numeric string, and an object.
say(idx(0 / 0, "a", "b", "c", "d", "e") + " " +
    idx(-0, "a", "b", "c", "d", "e") + " " +
    idx("2", "a", "b", "c", "d", "e") + " " +
    idx({}, "a", "b", "c", "d", "e"));
// CHECK-NEXT: undefined 0 b undefined
// CHECK0-NEXT: undefined 0 b undefined

// Reification.
say(reifyLoose("orig"));
// CHECK: JIT successfully compiled FunctionID 8, 'reifyLoose'
// CHECK-NEXT: orig/written/1/orig
// CHECK0: JIT successfully compiled FunctionID 8, 'reifyLoose'
// CHECK0-NEXT: orig/written/1/orig
say(reifyStrict("orig"));
// CHECK: JIT successfully compiled FunctionID 9, 'reifyStrict'
// CHECK-NEXT: 1/orig/param-changed
// CHECK0: JIT successfully compiled FunctionID 9, 'reifyStrict'
// CHECK0-NEXT: 1/orig/param-changed
say(bothWays(4, "a", "b", "c", "d"));
// The trailing "|R@5" is the reified read; the frame slot still holds 4.
// CHECK: JIT successfully compiled FunctionID 10, 'bothWays'
// CHECK-NEXT: 4@5 a@5 b@5 c@5 |R@5
// CHECK0: JIT successfully compiled FunctionID 10, 'bothWays'
// CHECK0-NEXT: 4@5 a@5 b@5 c@5 |R@5
say(noArgs());
// CHECK: JIT successfully compiled FunctionID 11, 'noArgs'
// CHECK-NEXT: 0/undefined
// CHECK0: JIT successfully compiled FunctionID 11, 'noArgs'
// CHECK0-NEXT: 0/undefined
say(reifyTwice(1, 2));
// CHECK: JIT successfully compiled FunctionID 12, 'reifyTwice'
// CHECK-NEXT: true/2
// CHECK0: JIT successfully compiled FunctionID 12, 'reifyTwice'
// CHECK0-NEXT: true/2
say(escapeStrict(1, 2, 3).length + "/" + escapeStrict().length + "/" +
    escapeStrict("x")[0]);
// CHECK: JIT successfully compiled FunctionID 13, 'escapeStrict'
// CHECK-NEXT: 3/0/x
// CHECK0: JIT successfully compiled FunctionID 13, 'escapeStrict'
// CHECK0-NEXT: 3/0/x
say(echoAll(1, "s", null, undefined, true, {}, 1.5));
// CHECK: JIT successfully compiled FunctionID 14, 'echoAll'
// CHECK-NEXT: number,string,object,undefined,boolean,object,number,
// CHECK0: JIT successfully compiled FunctionID 14, 'echoAll'
// CHECK0-NEXT: number,string,object,undefined,boolean,object,number,
