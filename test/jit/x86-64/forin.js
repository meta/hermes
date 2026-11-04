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

// Iteration: for-in (GetPNameList, GetNextPName) and the iteration protocol
// (IteratorBegin, IteratorNext, IteratorClose). Both live here because they
// are the same subject -- a loop driven by a runtime handler that updates
// registers in place -- and because for-of is what gives IteratorClose its
// only coverage in this suite.
//
// The four differential RUN lines are the real check, at -O and -O0, with and
// without type asserts, all under -Xjit-crash-on-error since every function
// here compiles in all four. The two -Xdump-jitcode=2 lines pin the compile
// status.
//
// JmpUndefined FINALLY RUNS HERE. It was ported in milestone 2 and had no
// runtime coverage at all until this file: nothing else the backend compiles
// emits it. for-in emits TWO, and both are load-bearing:
//   - after GetPNameList, which returns undefined when the operand has no
//     properties to enumerate -- i.e. when it is null or undefined. If that
//     branch did not jump, the loop body would run with the undefined
//     "property list" and GetNextPName would be handed a non-array. `nullIn`
//     and `undefIn` below are exactly that case, and they are the reason
//     this file cannot pass with a broken JmpUndefined.
//   - at the top of each iteration, on GetNextPName's result, which is
//     undefined when the enumeration is exhausted. This is the loop's ONLY
//     exit: invert it and every for-in below runs zero times; drop it and
//     they never terminate. `keysOf` returns the joined keys, so a loop that
//     ran the wrong number of times prints a different string rather than
//     merely a different count.
// Both are the untyped path through jmpUndefined (emit_sh_ljs_is_undefined
// plus je), not the isFRKnownType shortcut: GetPNameList and GetNextPName
// both produce values of unknown type.
//
// WHAT GetNextPName HAS TO GET RIGHT. It takes four registers -- the
// property list, the object, the index and the size -- and updates the index
// IN PLACE, in the frame, which is why the emitter syncs all four to the
// frame before the call and calls syncFrameOutParam on the index afterwards.
// A missed sync shows up as an infinite loop or a repeated key. The
// mid-iteration mutation cases below are the ones that keep the runtime
// handler on its slow path, where the object's hidden class has changed
// since the property list was captured.
//
// WHAT IteratorBegin/Next HAVE TO GET RIGHT. Both have in/out register
// parameters too: IteratorBegin overwrites its source register with either
// the iterator object or, for the fast-array path, the numeric index 0, and
// IteratorNext bumps that index in place. `ofArray` takes the array path and
// `ofIterable` takes the object path, so both halves of that register's
// meaning are exercised.

function say(x) {
  print(x);
}

// The workhorse: collects keys in visit order, so both the SET of keys and
// their ORDER are pinned, and a loop that exits early or late is visible.
function keysOf(o) {
  var s = "";
  for (var k in o) s += k + ",";
  return s;
}

// The GetPNameList JmpUndefined: null and undefined enumerate nothing, and
// the loop body must not run once.
function nullIn() {
  var n = 0;
  for (var k in null) n++;
  return n;
}
function undefIn(x) {
  var n = 0;
  for (var k in x) n++;
  return n;
}

// The GetNextPName JmpUndefined on an empty enumeration: the property list
// exists but is empty, so the very first GetNextPName returns undefined.
function emptyIn() {
  var n = 0;
  for (var k in {}) n++;
  return n;
}

// Values in visit order, which requires the key to be usable as a property
// name -- for an array, the string form of the index.
function valsOf(o) {
  var s = "";
  for (var k in o) s += o[k] + ",";
  return s;
}

// Delete a property that has not been visited yet. The spec says a property
// deleted before it is visited must not be visited, so the runtime handler
// has to re-check each captured name against the object.
function deleteDuring(o, victim) {
  var s = "";
  for (var k in o) {
    s += k + ",";
    delete o[victim];
  }
  return s;
}

// Add a property mid-loop. Whether it is visited is implementation-defined,
// but it must be the SAME answer in the interpreter and the JIT, and adding
// a property changes the object's hidden class, which is what pushes
// GetNextPName off its fast path.
function addDuring(o) {
  var s = "";
  var n = 0;
  for (var k in o) {
    s += k + ",";
    if (++n === 1) o.zAdded = 1;
    if (n > 20) break;
  }
  return s;
}

// Break out of a for-in: the loop is left without ever seeing the undefined.
function firstKey(o) {
  for (var k in o) return k;
  return "<none>";
}

// Nested for-in over the same object: two independent enumerations, each
// with its own property list, index and size registers live at once.
function crossKeys(o) {
  var s = "";
  for (var a in o)
    for (var b in o) s += a + b + " ";
  return s;
}

// for-of over an array. IteratorBegin recognizes the array and leaves the
// index 0 in the source register instead of an iterator object;
// IteratorNext bumps that index in place.
function ofArray(a) {
  var s = 0;
  for (var v of a) s += v;
  return s;
}

// for-of over a non-array iterable, which takes the generic path: the source
// register holds a real iterator object and IteratorNext calls its next().
function ofIterable(it) {
  var s = "";
  for (var v of it) s += v + ",";
  return s;
}

// Break out of a for-of: IteratorClose with ignoreExceptions == 0, which
// calls the iterator's return() and propagates anything it throws.
function ofBreak(it, stop) {
  var s = "";
  for (var v of it) {
    if (v === stop) break;
    s += v + ",";
  }
  return s;
}

// Throw out of a for-of: IteratorClose with ignoreExceptions == 1, since a
// failure to close must not displace the exception being propagated.
function ofThrow(it, bad) {
  var s = "";
  try {
    for (var v of it) {
      if (v === bad) throw new Error("stop at " + v);
      s += v + ",";
    }
  } catch (e) {
    s += "[" + e.message + "]";
  }
  return s;
}

// Array destructuring, which is the iteration protocol without a loop:
// IteratorBegin, two IteratorNexts, then IteratorClose because the pattern
// stops before the iterator is exhausted.
function destructure(a) {
  var [x, y] = a;
  return x + "/" + y;
}

// An iterable that is not an array and not a built-in collection, so every
// step goes through user JS.
function makeCounter(n) {
  var obj = {};
  obj[Symbol.iterator] = function () {
    var i = 0;
    var closed = 0;
    return {
      next: function () {
        if (i < n) return {value: i++, done: false};
        return {value: undefined, done: true};
      },
      return: function () {
        closed++;
        return {done: true};
      },
    };
  };
  return obj;
}

var plain = {a: 1, b: 2, c: 3};
say(keysOf(plain) + "|" + valsOf(plain));
// The pins follow CALL order, not source order.
// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK: JIT successfully compiled FunctionID 2, 'keysOf'
// CHECK: JIT successfully compiled FunctionID 6, 'valsOf'
// CHECK: JIT successfully compiled FunctionID 1, 'say'
// CHECK-NEXT: a,b,c,|1,2,3,
// CHECK0: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 2, 'keysOf'
// CHECK0: JIT successfully compiled FunctionID 6, 'valsOf'
// CHECK0: JIT successfully compiled FunctionID 1, 'say'
// CHECK0-NEXT: a,b,c,|1,2,3,

// The two JmpUndefined-after-GetPNameList cases, plus the empty-enumeration
// case that exits on the FIRST GetNextPName.
say(nullIn() + " " + undefIn(undefined) + " " + emptyIn());
// CHECK: JIT successfully compiled FunctionID 3, 'nullIn'
// CHECK: JIT successfully compiled FunctionID 4, 'undefIn'
// CHECK: JIT successfully compiled FunctionID 5, 'emptyIn'
// CHECK-NEXT: 0 0 0
// CHECK0: JIT successfully compiled FunctionID 3, 'nullIn'
// CHECK0: JIT successfully compiled FunctionID 4, 'undefIn'
// CHECK0: JIT successfully compiled FunctionID 5, 'emptyIn'
// CHECK0-NEXT: 0 0 0
// An object is not undefined, so the same branch must NOT be taken here --
// `undefIn` is one compiled function seeing both polarities.
say(undefIn(plain));
// CHECK-NEXT: 3
// CHECK0-NEXT: 3

// Inherited enumerable properties are visited after the own ones; an own
// property shadowing an inherited one is visited exactly once.
var proto = {p1: 10, p2: 20, a: 99};
var child = Object.create(proto);
child.a = 1;
child.b = 2;
say(keysOf(child));
// CHECK-NEXT: a,b,p1,p2,
// CHECK0-NEXT: a,b,p1,p2,

// Non-enumerable properties are skipped.
var hidden = {v: 1};
Object.defineProperty(hidden, "secret", {value: 2, enumerable: false});
Object.defineProperty(hidden, "shown", {value: 3, enumerable: true});
say(keysOf(hidden));
// CHECK-NEXT: v,shown,
// CHECK0-NEXT: v,shown,

// Array indices enumerate as strings, in index order, and named properties
// after them.
var arr = [10, 20, 30];
arr.named = 40;
say(keysOf(arr) + "|" + valsOf(arr));
// CHECK-NEXT: 0,1,2,named,|10,20,30,40,
// CHECK0-NEXT: 0,1,2,named,|10,20,30,40,

// A string: indices with no own named properties.
say(keysOf("abc"));
// CHECK-NEXT: 0,1,2,
// CHECK0-NEXT: 0,1,2,

// Mid-loop mutation. `deleteDuring` removes a key that has not been reached
// yet, so it must not appear in the output.
say(deleteDuring({k1: 1, k2: 2, k3: 3, k4: 4}, "k3"));
// CHECK: JIT successfully compiled FunctionID 7, 'deleteDuring'
// CHECK-NEXT: k1,k2,k4,
// CHECK0: JIT successfully compiled FunctionID 7, 'deleteDuring'
// CHECK0-NEXT: k1,k2,k4,
// Deleting the key currently being visited is a no-op for the enumeration.
say(deleteDuring({k1: 1, k2: 2, k3: 3}, "k1"));
// CHECK-NEXT: k1,k2,k3,
// CHECK0-NEXT: k1,k2,k3,
// Adding during iteration, which also invalidates the hidden class.
say(addDuring({m1: 1, m2: 2, m3: 3}));
// CHECK: JIT successfully compiled FunctionID 8, 'addDuring'
// CHECK-NEXT: m1,m2,m3,
// CHECK0: JIT successfully compiled FunctionID 8, 'addDuring'
// CHECK0-NEXT: m1,m2,m3,

// Early exit, and the nested case.
say(firstKey(plain) + " " + firstKey({}) + " " + firstKey(null));
// CHECK: JIT successfully compiled FunctionID 9, 'firstKey'
// CHECK-NEXT: a <none> <none>
// CHECK0: JIT successfully compiled FunctionID 9, 'firstKey'
// CHECK0-NEXT: a <none> <none>
say(crossKeys({x: 1, y: 2}));
// CHECK: JIT successfully compiled FunctionID 10, 'crossKeys'
// CHECK-NEXT: xx xy yx yy{{ *}}
// CHECK0: JIT successfully compiled FunctionID 10, 'crossKeys'
// CHECK0-NEXT: xx xy yx yy{{ *}}

// A for-in in a loop that runs enough times to matter, so a leaked handle or
// a missed sync in GetNextPName shows up as a crash under ASan rather than a
// wrong answer.
var total = 0;
for (var i = 0; i < 200; ++i) {
  var o = {};
  o["k" + (i % 7)] = i;
  o.fixed = 1;
  for (var k in o) total += o[k];
}
say(total);
// CHECK-NEXT: 20100
// CHECK0-NEXT: 20100

// The iteration protocol.
say(ofArray([1, 2, 3, 4]) + " " + ofArray([]) + " " + ofArray([1.5, -0.5]));
// CHECK: JIT successfully compiled FunctionID 11, 'ofArray'
// CHECK-NEXT: 10 0 1
// CHECK0: JIT successfully compiled FunctionID 11, 'ofArray'
// CHECK0-NEXT: 10 0 1
var counter = makeCounter(5);
say(ofIterable(counter));
// The iterable's closures compile on their first call, which is inside
// ofIterable's loop, so they are pinned here too: FunctionID 17 is the
// anonymous Symbol.iterator method and 18 is its next().
// CHECK: JIT successfully compiled FunctionID 16, 'makeCounter'
// CHECK: JIT successfully compiled FunctionID 12, 'ofIterable'
// CHECK: JIT successfully compiled FunctionID 17, ''
// CHECK: JIT successfully compiled FunctionID 18, 'next'
// CHECK-NEXT: 0,1,2,3,4,
// CHECK0: JIT successfully compiled FunctionID 16, 'makeCounter'
// CHECK0: JIT successfully compiled FunctionID 12, 'ofIterable'
// CHECK0: JIT successfully compiled FunctionID 17, ''
// CHECK0: JIT successfully compiled FunctionID 18, 'next'
// CHECK0-NEXT: 0,1,2,3,4,
say(ofBreak(counter, 3) + "|" + ofBreak(counter, 0) + "|" +
    ofBreak(counter, 99));
// The break is what first calls the iterator's return(), so FunctionID 19
// compiles here -- the IteratorClose(ignoreExceptions == 0) path.
// CHECK: JIT successfully compiled FunctionID 13, 'ofBreak'
// CHECK: JIT successfully compiled FunctionID 19, 'return'
// CHECK-NEXT: 0,1,2,||0,1,2,3,4,
// CHECK0: JIT successfully compiled FunctionID 13, 'ofBreak'
// CHECK0: JIT successfully compiled FunctionID 19, 'return'
// CHECK0-NEXT: 0,1,2,||0,1,2,3,4,
say(ofThrow(counter, 2) + "|" + ofThrow(counter, 99));
// CHECK: JIT successfully compiled FunctionID 14, 'ofThrow'
// CHECK-NEXT: 0,1,[stop at 2]|0,1,2,3,4,
// CHECK0: JIT successfully compiled FunctionID 14, 'ofThrow'
// CHECK0-NEXT: 0,1,[stop at 2]|0,1,2,3,4,
say(destructure([7, 8]) + " " + destructure([7]) + " " + destructure(counter));
// CHECK: JIT successfully compiled FunctionID 15, 'destructure'
// CHECK-NEXT: 7/8 7/undefined 0/1
// CHECK0: JIT successfully compiled FunctionID 15, 'destructure'
// CHECK0-NEXT: 7/8 7/undefined 0/1
// Spread, which is the same protocol driven to exhaustion.
say([...counter].join("-") + " " + [...[9, 8]].join("-"));
// CHECK-NEXT: 0-1-2-3-4 9-8
// CHECK0-NEXT: 0-1-2-3-4 9-8
