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
// RUN: %hermes -fno-inline %s > %t.int && %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error %s > %t.warm && diff %t.int %t.warm
// RUN: %hermes -fno-inline -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xdump-jitcode=1 %s 2>&1 | %FileCheck --check-prefix=COLD %s
// REQUIRES: jit

// Exceptions: Catch, Throw, ThrowIfEmpty and ThrowIfThisInitialized, plus
// the per-function catch table that leave() now emits and the SHJmpBuf /
// setjmp prologue that has been in place since the first milestone but was
// unreachable until this file.
//
// The first four RUN lines are the real check -- interpreter and JIT must
// print the same thing, at -O and -O0, with and without the type asserts --
// and they carry -Xjit-crash-on-error because every function here compiles
// in all four. The two -Xdump-jitcode=2 lines pin the compile status, so
// the differential cannot degrade into comparing the interpreter with
// itself.
//
// -fno-inline is essential, not cosmetic. Without it the optimizer inlines
// the throwers into their callers, and a throw that never crosses a JS frame
// boundary never exercises the interesting half of _jit_find_catch_target:
// its "the exception came from a callee, so read the IP out of the register
// stack's SavedIP slot" branch. With it, `boom` is its own compiled frame
// and every catch below is a cross-frame catch.
//
// WHAT THE CATCH TABLE HAS TO GET RIGHT. A JIT-compiled function with an
// exception table pushes an SHJmpBuf and calls _sh_setjmp in its prologue.
// A throw anywhere below it longjmps back to that setjmp, which returns
// non-zero, and control falls into the catch table. The table calls
// _jit_find_catch_target, which maps the throwing IP to an index in the
// bytecode exception table and returns the address of the matching handler
// basic block -- or, if no entry covers the IP, ends the try and rethrows to
// the next frame out. Three separate things must line up for that to work:
//   - the handler ORDER: the table of label deltas emitted after the
//     indirect jump is indexed by the bytecode exception table's index, so
//     the i'th delta must be the i'th entry's handler. `nested` and
//     `twoTries` below each have several entries in one function, so an
//     off-by-one or a reversal shows up as the wrong catch running.
//   - the SHJmpBuf ADDRESS: it lives at rsp + 0, which longjmp restores to
//     its setjmp-time value no matter what the throwing code was doing with
//     rsp. That placement also makes it 16-byte aligned unconditionally
//     (see frameSetup()'s layout comment), which is what jmp_buf requires on
//     x86-64 and which arm64 never had to arrange.
//   - the saved SHLocals: the JIT does not push an SHLocals of its own, so
//     the prologue stashes the runtime's on entry and the catch table hands
//     it to _sh_catch_no_pop, which is what unwinds the handle scopes and
//     the register stack. A wrong value here corrupts the GC roots, which
//     is why every RUN line in this suite runs under ASan and why the
//     `churn` case below allocates hard inside a catch.
//
// NO GLOBAL REGISTERS IN A TRY FUNCTION. longjmp restores callee-saved
// registers to their setjmp-time values, so a frame register living in one
// would present a stale value to a handler. RegisterAllocator::getRegClass
// forces RegClass::Other for any function containing a try, so no such
// function ever gets a global register, and enter() asserts it. Every
// function below with a try is therefore running entirely out of the memory
// frame -- which is also what makes the isInTry() syncs in throwInst,
// throwIfEmptyUndefinedImpl, throwIfThisInitialized and fastArrayLoad
// sufficient.
//
// THE THROWER IS NOT ALWAYS COMPILED. Under -Xjit=force every function is
// compiled, so all six differential RUN lines above catch a throw raised by
// another compiled frame. The fifth RUN line covers the other direction:
// in threshold mode `hotCatcher` is called forty-one times and compiles,
// while `coldThrower` is called exactly once and never does, so a throw
// raised by the INTERPRETER unwinds into a compiled frame's setjmp. The
// COLD RUN line pins that asymmetry -- if coldThrower ever started
// compiling there, the case would silently stop testing anything.
//
// ThrowIfUndefined IS NOT COVERED, and cannot be from this file. It shares
// throwIfEmptyUndefinedImpl with ThrowIfEmpty and differs only in which tag
// helper the fast path uses. ISel emits it for a ThrowIfInst whose invalid
// type is Uninit, which IRGen produces only for the initialization-dead-zone
// check on a typed class field (ESTreeIRGen-expr.cpp's PrLoad path) -- i.e.
// only under -typed, which this file is not. Said here rather than left
// looking covered.

function say(x) {
  print(x);
}

// The thrower. Its own compiled frame, so every catch below is a cross-frame
// catch and _jit_find_catch_target has to find the IP in the callee's
// SavedIP slot rather than in Runtime::currentIP.
function boom(msg) {
  throw new Error(msg);
}
// Plain try/catch/finally. `finallyRuns` counts the finally blocks so that a
// finally skipped or run twice changes the output.
var finallyRuns = 0;
function classify(x) {
  try {
    if (x < 0) boom("neg " + x);
    return "ok:" + x;
  } catch (e) {
    return "caught:" + e.message;
  } finally {
    finallyRuns++;
  }
}
say(classify(1) + "|" + classify(-2) + "|" + finallyRuns);
// Functions are compiled on first call, so the pins below follow call order,
// not source order: `classify` is entered before it calls `boom`, and `say`
// is not called until both have returned.
// CHECK: JIT successfully compiled FunctionID 3, 'classify'
// CHECK: JIT successfully compiled FunctionID 2, 'boom'
// CHECK: JIT successfully compiled FunctionID 1, 'say'
// CHECK-NEXT: ok:1|caught:neg -2|2
// CHECK0: JIT successfully compiled FunctionID 3, 'classify'
// CHECK0: JIT successfully compiled FunctionID 2, 'boom'
// CHECK0: JIT successfully compiled FunctionID 1, 'say'
// CHECK0-NEXT: ok:1|caught:neg -2|2

// A finally that runs while the exception is still in flight. `inner` has
// one exception table entry, covering the try body; the handler runs the
// finally block and then rethrows with a Throw whose own IP is PAST the end
// of that entry. So the rethrow re-enters inner's catch table, finds no
// entry covering it, and takes _jit_find_catch_target's other exit: end the
// try, pop the SHJmpBuf and rethrow to the next frame out, which is `outer`.
// That is the only path in this file that leaves a catch table without
// jumping into a handler.
function inner(x) {
  try {
    boom("inner " + x);
  } finally {
    finallyRuns++;
  }
}
function outer(x) {
  try {
    inner(x);
    return "no throw";
  } catch (e) {
    return "outer got:" + e.message;
  }
}
say(outer(7) + "|" + finallyRuns);
// CHECK: JIT successfully compiled FunctionID 5, 'outer'
// CHECK: JIT successfully compiled FunctionID 4, 'inner'
// CHECK-NEXT: outer got:inner 7|3
// CHECK0: JIT successfully compiled FunctionID 5, 'outer'
// CHECK0: JIT successfully compiled FunctionID 4, 'inner'
// CHECK0-NEXT: outer got:inner 7|3

// Catch inside a loop: the handler block is a loop body, so control returns
// from the catch table into the middle of the function and keeps going. The
// accumulator lives in a frame register that the handler must see the
// current value of, which is exactly what the no-global-registers contract
// buys.
function loopCatch(n) {
  var caught = 0;
  var sum = 0;
  for (var i = 0; i < n; ++i) {
    try {
      if (i % 3 === 0) boom("i=" + i);
      sum += i;
    } catch (e) {
      caught += e.message.length;
    }
  }
  return sum + "/" + caught;
}
say(loopCatch(10));
// CHECK: JIT successfully compiled FunctionID 6, 'loopCatch'
// CHECK-NEXT: 27/12
// CHECK0: JIT successfully compiled FunctionID 6, 'loopCatch'
// CHECK0-NEXT: 27/12

// Rethrow: catch, then throw the caught value again from inside the handler.
// The Throw is itself inside the try region of the OUTER try, so throwInst's
// isInTry() sync path is the one that runs.
function rethrower(x) {
  try {
    try {
      boom("deep " + x);
    } catch (e) {
      throw e;
    }
    return "unreachable";
  } catch (e2) {
    return "rethrown:" + e2.message;
  }
}
say(rethrower(4));
// CHECK: JIT successfully compiled FunctionID 7, 'rethrower'
// CHECK-NEXT: rethrown:deep 4
// CHECK0: JIT successfully compiled FunctionID 7, 'rethrower'
// CHECK0-NEXT: rethrown:deep 4

// Nested try in one function: several entries in one bytecode exception
// table, so the handler-index-to-label mapping has more than one candidate
// and a mis-indexed table runs the wrong handler.
function nested(a, b) {
  var out = "";
  try {
    out += "A";
    try {
      out += "B";
      if (a) boom("inner");
      out += "C";
    } catch (e) {
      out += "[i:" + e.message + "]";
      if (b) boom("outer");
      out += "D";
    }
    out += "E";
  } catch (e) {
    out += "[o:" + e.message + "]";
  }
  return out;
}
say(
  nested(false, false) + "|" + nested(true, false) + "|" +
  nested(true, true));
// CHECK: JIT successfully compiled FunctionID 8, 'nested'
// CHECK-NEXT: ABCE|AB[i:inner]DE|AB[i:inner][o:outer]
// CHECK0: JIT successfully compiled FunctionID 8, 'nested'
// CHECK0-NEXT: ABCE|AB[i:inner]DE|AB[i:inner][o:outer]

// Two sibling try regions in one function, each with its own handler. Same
// point as `nested`, but with the regions disjoint rather than enclosed, so
// the two entries cannot be told apart by their extents.
function twoTries(which) {
  var out = "";
  try {
    if (which === 1) boom("one");
    out += "1ok";
  } catch (e) {
    out += "1c:" + e.message;
  }
  try {
    if (which === 2) boom("two");
    out += "|2ok";
  } catch (e) {
    out += "|2c:" + e.message;
  }
  return out;
}
say(twoTries(0) + " / " + twoTries(1) + " / " + twoTries(2));
// CHECK: JIT successfully compiled FunctionID 9, 'twoTries'
// CHECK-NEXT: 1ok|2ok / 1c:one|2ok / 1ok|2c:two
// CHECK0: JIT successfully compiled FunctionID 9, 'twoTries'
// CHECK0-NEXT: 1ok|2ok / 1c:one|2ok / 1ok|2c:two

// A throw that crosses several compiled frames before it finds a handler:
// every frame between the thrower and the catcher has no exception table at
// all, so _sh_throw's longjmp target is the catcher's setjmp, four frames
// up, and the intervening frames' epilogues never run.
function level3(n) {
  return boom("depth " + n);
}
function level2(n) {
  return level3(n) + 1;
}
function level1(n) {
  return level2(n) + 1;
}
function deepCatch(n) {
  try {
    return level1(n);
  } catch (e) {
    return "deep:" + e.message;
  }
}
say(deepCatch(3));
// CHECK: JIT successfully compiled FunctionID 13, 'deepCatch'
// CHECK: JIT successfully compiled FunctionID 12, 'level1'
// CHECK: JIT successfully compiled FunctionID 11, 'level2'
// CHECK: JIT successfully compiled FunctionID 10, 'level3'
// CHECK-NEXT: deep:depth 3
// CHECK0: JIT successfully compiled FunctionID 13, 'deepCatch'
// CHECK0: JIT successfully compiled FunctionID 12, 'level1'
// CHECK0: JIT successfully compiled FunctionID 11, 'level2'
// CHECK0: JIT successfully compiled FunctionID 10, 'level3'
// CHECK0-NEXT: deep:depth 3

// A NATIVE frame in the middle. Array.prototype.map is C++, so the longjmp
// unwinds compiled JS -> native -> compiled JS. The native frame's own
// cleanup is what _sh_throw's unwinding has to respect.
function throughNative(arr) {
  try {
    return arr.map(function (x) {
      if (x === 3) boom("map " + x);
      return x * 2;
    }).join(",");
  } catch (e) {
    return "native:" + e.message;
  }
}
say(throughNative([1, 2]) + "|" + throughNative([1, 3]));
// FunctionID 22 is the anonymous map callback.
// CHECK: JIT successfully compiled FunctionID 14, 'throughNative'
// CHECK: JIT successfully compiled FunctionID 22, ''
// CHECK-NEXT: 2,4|native:map 3
// CHECK0: JIT successfully compiled FunctionID 14, 'throughNative'
// CHECK0: JIT successfully compiled FunctionID 22, ''
// CHECK0-NEXT: 2,4|native:map 3

// An error raised by the RUNTIME rather than by a Throw instruction: the
// longjmp originates inside a runtime helper called from compiled code, not
// from an emitted call to _sh_throw.
function runtimeError(o) {
  try {
    return "len:" + o.x.length;
  } catch (e) {
    return e instanceof TypeError ? "type-error" : "other";
  }
}
say(runtimeError({x: "abc"}) + "|" + runtimeError({}));
// CHECK: JIT successfully compiled FunctionID 15, 'runtimeError'
// CHECK-NEXT: len:3|type-error
// CHECK0: JIT successfully compiled FunctionID 15, 'runtimeError'
// CHECK0-NEXT: len:3|type-error

// Allocation inside a catch handler, a lot of it. The handler runs after
// _sh_catch_no_pop has restored the SHLocals the prologue stashed; if that
// pointer were wrong the handle scopes would be left in an inconsistent
// state and these allocations would trip ASan or the GC.
function churn(n) {
  var acc = 0;
  for (var i = 0; i < n; ++i) {
    try {
      if ((i & 15) === 0) boom("churn " + i);
      acc += i;
    } catch (e) {
      var o = {a: i, b: [i, i + 1, i + 2], c: "s" + i};
      acc += o.b[1] + o.c.length;
    }
  }
  return acc;
}
say(churn(2000));
// CHECK: JIT successfully compiled FunctionID 16, 'churn'
// CHECK-NEXT: 1999679
// CHECK0: JIT successfully compiled FunctionID 16, 'churn'
// CHECK0-NEXT: 1999679

// ThrowIfEmpty and ThrowIfThisInitialized. Reading `this` in a derived
// constructor before super() is a ThrowIfEmpty on the captured `this`
// binding; calling super() twice is a ThrowIfThisInitialized. Both paths of
// both checks run: `mode` selects which, and the ordinary construction takes
// the fast path through the same two instructions (the implicit `return
// this` is itself a ThrowIfEmpty).
class Base {
  constructor(tag) {
    this.tag = tag;
  }
}
class Derived extends Base {
  constructor(mode) {
    if (mode === "early") this.tag = "never";
    super(mode);
    this.seen = mode;
    if (mode === "twice") super(mode);
  }
}
function build(mode) {
  try {
    return new Derived(mode).seen;
  } catch (e) {
    return e.name;
  }
}
say(build("ok") + "|" + build("early") + "|" + build("twice"));
// CHECK: JIT successfully compiled FunctionID 17, 'build'
// CHECK: JIT successfully compiled FunctionID 21, 'Derived'
// CHECK: JIT successfully compiled FunctionID 20, 'Base'
// CHECK-NEXT: ok|ReferenceError|ReferenceError
// CHECK0: JIT successfully compiled FunctionID 17, 'build'
// CHECK0: JIT successfully compiled FunctionID 21, 'Derived'
// CHECK0: JIT successfully compiled FunctionID 20, 'Base'
// CHECK0-NEXT: ok|ReferenceError|ReferenceError

// The interpreted-thrower case, which only the threshold RUN line reaches.
// hotCatcher runs its no-call branch forty times, which is far past the
// threshold of 2, so it is compiled by the time it finally calls
// coldThrower -- and coldThrower has been called zero times up to that
// point, so it is still interpreted when it throws.
function coldThrower(n) {
  throw new Error("cold " + n);
}
function hotCatcher(n) {
  try {
    if (n === 0) return 0;
    return coldThrower(n);
  } catch (e) {
    return e.message;
  }
}
var hot = 0;
for (var i = 0; i < 40; ++i) hot += hotCatcher(0);
say(hot + "|" + hotCatcher(5));
// CHECK: JIT successfully compiled FunctionID 19, 'hotCatcher'
// CHECK: JIT successfully compiled FunctionID 18, 'coldThrower'
// CHECK-NEXT: 0|cold 5
// CHECK0: JIT successfully compiled FunctionID 19, 'hotCatcher'
// CHECK0: JIT successfully compiled FunctionID 18, 'coldThrower'
// CHECK0-NEXT: 0|cold 5
// COLD: JIT successfully compiled FunctionID 19, 'hotCatcher'
// COLD-NOT: FunctionID 18, 'coldThrower'
