/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -gc-sanitize-handles=1 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// Method blocks for builtins and for data properties of every value
// kind. Each function's version 1 is compiled before any string arrives
// (NotPrepared): calls 1-16 decline, call 16 recompiles, and version 2's
// Method block serves calls 17-20. 13 sites x 4 = 52 hits; 13
// recompiles.
// - cc calls the loaded charCodeAt; io (indexOf, out of line: its load
//   goes through propStorage) and sub (substring) are called too.
// - ts loads toString, a direct slot (JitStringPrepTest pins both slots;
//   review focus 2): its load has no propStorage indirection.
// - gInt ... gBig read data properties def() adds to String.prototype
//   (out of line), holding a small integer, 1.1 (a heap BoxedDouble on
//   HV32 and BOXED: its low bits do not fit the inline encoding,
//   SmallHermesValue.h:318-336), a symbol, a heap string, an object,
//   undefined, null, true and a BigInt: on HV32 and BOXED every decoder
//   branch runs, and the pointer branch with each of its three tags
//   (string, BigInt, object).
// - Review focus 5: ts, holding its Method block, gets null and
//   undefined. Neither takes the block (their tag is not the string
//   tag); both reach the slow call, which throws a TypeError and
//   longjmps past the decline count. Then 4 more strings hit: 4 hits.
// - len's version 2 holds a Length block (call 16 recompiles, calls
//   17-20 hit: 4 hits). String.prototype's class already has a lazy ID
//   then (cc's preparation assigned it, after def()'s additions), while
//   a Length site's stringProtoId stays 0. 16 number receivers then
//   decline and cross the threshold once: the Length tier never
//   progresses (byIdStringCanProgress), so nothing recompiles, and one
//   unchanged crossing does not demote. Without the Length early return
//   the ID compare would recompile len (NumRecompiles 15).
// NumByIdStringHits = 52 + 4 + 4 = 60; NumRecompiles = 13 + 1 = 14.
// main() runs three recording reads (String.prototype, and SP.toString
// twice; its global reads are TryGetById, which does not record) and
// def() nine keyed stores, both under 16.
// Fails before the feature (no Method blocks; NumByIdStringHits 0), and
// on the HV32 and BOXED trees without the block's decoder.
function main() {
  var SP = String.prototype;
  function def(o, k, v) {
    o[k] = v;
  }
  function cat(a, b) {
    return a + b;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  // "TypeError" if f(x) throws one.
  function tryGet(f, x) {
    try {
      f(x);
      return 'no throw';
    } catch (e) {
      return e instanceof TypeError ? 'TypeError' : 'other';
    }
  }
  function cc(s) {
    return s.charCodeAt(1);
  }
  function io(s) {
    return s.indexOf('c');
  }
  function sub(s) {
    return s.substring(1, 3);
  }
  function ts(s) {
    return s.toString;
  }
  function gInt(s) {
    return s.pInt;
  }
  function gDbl(s) {
    return s.pDbl;
  }
  function gSym(s) {
    return s.pSym;
  }
  function gStr(s) {
    return s.pStr;
  }
  function gObj(s) {
    return s.pObj;
  }
  function gUndef(s) {
    return s.pUndef;
  }
  function gNull(s) {
    return s.pNull;
  }
  function gTrue(s) {
    return s.pTrue;
  }
  function gBig(s) {
    return s.pBig;
  }
  function len(s) {
    return s.length;
  }
  var sym = Symbol('ps');
  var obj = {k: 9};
  var big = 12345678901234567890n;
  def(SP, 'pInt', 42);
  def(SP, 'pDbl', 1.1);
  def(SP, 'pSym', sym);
  def(SP, 'pStr', cat('str', 'ing'));
  def(SP, 'pObj', obj);
  def(SP, 'pUndef', undefined);
  def(SP, 'pNull', null);
  def(SP, 'pTrue', true);
  def(SP, 'pBig', big);
  print(
      count(cc, 'abc', 20, 98),
      count(io, 'abc', 20, 2),
      count(sub, 'abcd', 20, 'bc'),
      count(ts, 'abc', 20, SP.toString));
  print(
      count(gInt, 'abc', 20, 42),
      count(gDbl, 'abc', 20, 1.1),
      count(gSym, 'abc', 20, sym),
      count(gStr, 'abc', 20, 'string'),
      count(gObj, 'abc', 20, obj),
      count(gUndef, 'abc', 20, undefined),
      count(gNull, 'abc', 20, null),
      count(gTrue, 'abc', 20, true),
      count(gBig, 'abc', 20, big));
  print(
      tryGet(ts, null),
      tryGet(ts, undefined),
      count(ts, 'abc', 4, SP.toString));
  print(count(len, 'abc', 20, 3), count(len, 5, 16, undefined));
}
main();

// OUT: 20 20 20 20
// OUT-NEXT: 20 20 20 20 20 20 20 20 20
// OUT-NEXT: TypeError TypeError 4
// OUT-NEXT: 20 16
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'cc' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'cc' (version 2)
// DUMP-NOT: JIT compilation of
// DUMP: JIT ById string sites: 0 length, 1 method
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'io' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{([6-9]|[1-9][0-9]+)}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'sub' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'ts' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-5]}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gInt' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gDbl' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gSym' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gStr' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gObj' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gUndef' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gNull' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gTrue' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'gTrue' (version 2)
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gBig' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'gBig' (version 2)
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'len' (version 2)
// DUMP: // Get string length (site {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'len' (version 2)
// DUMP-NOT: 'len' (version 3)
// CNT: NumRecompiles: 14{{$}}
// CNT: NumByIdStringHits: 60{{$}}
