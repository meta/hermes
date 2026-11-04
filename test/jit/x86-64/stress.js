/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-crash-on-error %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit3 && diff %t.int0 %t.jit3
// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// REQUIRES: jit

// The milestone-5 gate. The body below is a byte-for-byte copy of
// aarch64/jit-stress.js -- the differential soak test that the plan
// requires the x86-64 backend to run under -Xjit=force -Xjit-crash-on-error
// on HV64, HV32 and BOXED, matching the interpreter exactly, before the
// backend is called opcode-complete. That script is a plain .js file run
// manually against each build (it is not itself a lit test, and it stays
// where it is so the arm64 side keeps using it unmodified); this file wraps
// the same body as a standing lit gate so the property is checked on every
// build from here on rather than only by hand. Do not let the two drift --
// if you change one, change the other the same way, or note here why they
// diverge. Three body lines exceed 80 columns; that is inherited from
// aarch64/jit-stress.js, which already exceeds 80 columns there, not
// introduced here -- leave them as they are rather than "tidying" them
// to fit this repo's usual line limit, which would silently break the
// byte-for-byte property above.
//
// All four differential RUN lines carry -Xjit-crash-on-error: every
// function in this file compiles, at both -O and -O0, with and without the
// type asserts (measured; this is also the state manually verified on all
// three heap-value-mode builds per the milestone-5 report). The two
// -Xdump-jitcode=2 RUN lines pin exactly which functions compile and in
// what order, so the differential can never quietly degrade into comparing
// the interpreter against itself, and so a future decline anywhere in this
// file's opcode mix is caught here even before -Xjit-crash-on-error would
// abort the differential itself.
//
// FunctionIDs were read out of a real -Xdump-jitcode=2 dump, matched to
// source names using the dump's own "JIT successfully compiled FunctionID
// N, 'name'" lines -- not by grepping bytecode disassembly headers, where
// an anonymous function's block starts with NCFunction<...> rather than
// Function<...> and a naive /^Function</ pattern silently misattributes it
// to the previous named function (a mistake made and caught in an earlier
// milestone-5 task; see the exceptions/objects test headers).
//
// -O and -O0 compile the same 31 functions except one: makeCounter's
// `inner` closure is inlined into its caller at -O and never gets its own
// compile event, but survives as a real call at -O0 (FunctionID 32). Two
// functions are named 'gen' (15 and 31) because a generator function
// compiles as two bodies -- the outer function that constructs the
// generator object and the inner body that runs on each .next() -- and two
// are named 'describe' (21 and 23) because Base and Derived each define
// their own. Seven are anonymous (17, 24, 25, 26, 27, 29, 30): 17 is
// Point.prototype.norm and 29 is builtinThis's Ctor.method, both function
// expressions assigned through a member expression rather than bound by a
// `var`/object-literal-shorthand name, which is what makes Hermes not
// infer a name for them; 30 is makeCounter's own returned closure
// (`return function () {...}`); 24-27 are the map/filter/sort/reduce
// callbacks near the end. FunctionID 28 -- builtinThis's `Ctor` itself --
// is declared but never invoked (`Ctor.method()` and `r instanceof Ctor`
// both read it without calling it), so it never compiles under
// -Xjit=force and is absent from both pin lists by design, not omission
// (confirmed with hbcdump's function-info: index 28 is named "Ctor").

var out = [];
function log(x) {
  out.push(String(x));
}

// Arithmetic, bit ops, NaN/-0 corners, the *N fast paths.
function arith(n) {
  var s = 0, f = 1.5;
  for (var i = 0; i < n; ++i) {
    s += i * 3 - (i >> 2) + (i | 1) + (i ^ 7) + (i & 3);
    s -= (i << 1) >>> 3;
    f = f * 1.0000001 + i / 7 - (i % 5);
  }
  return [s, f.toFixed(6)];
}
log(arith(200));
log([0 / 0, 1 / 0, -1 / 0, -0, (0 === -0), Object.is(-0, 0 * -1)]);
log([(2147483647 | 0) + 1, -2147483648 >> 1, -1 >>> 28, 5 % -3, -5 % 3]);
log([Math.pow(2, 53) + 1, 0.1 + 0.2, (1e308 * 10)]);

// Comparisons incl. NaN, and the inverted branch forms.
function cmp(a, b) {
  var r = [];
  r.push(a < b, a <= b, a > b, a >= b, a == b, a === b, a != b, a !== b);
  if (!(a < b)) r.push("!lt");
  if (!(a >= b)) r.push("!ge");
  return r;
}
log(cmp(1, 2));
log(cmp(NaN, 1));
log(cmp(1, NaN));
log(cmp("a", "b"));
log(cmp(null, undefined));

// Strings: concat, AddS, comparison, interning behaviour.
function strs(n) {
  var s = "";
  for (var i = 0; i < n; ++i) s += (i % 10);
  return [s.length, s.charCodeAt(5), s.slice(3, 9), s === s, s > "0"];
}
log(strs(50));
log(["x" + 1, 1 + "x", "a" + null, "" + undefined, "" + {}, "" + [1, 2]]);

// Property access: inline caches, prototype chains, accessors, shape changes.
function Point(x, y) {
  this.x = x;
  this.y = y;
}
Point.prototype.norm = function () {
  return this.x * this.x + this.y * this.y;
};
Object.defineProperty(Point.prototype, "tag", {
  get: function () {
    return "P" + this.x;
  },
});
function props(n) {
  var acc = 0, tags = "";
  for (var i = 0; i < n; ++i) {
    var p = new Point(i, i + 1);
    acc += p.norm();
    if (i % 20 === 0) tags += p.tag;
    if (i === 30) p.extra = 1; // shape transition mid-loop
  }
  return [acc, tags];
}
log(props(100));

// Objects/arrays from literal buffers, then mutation.
log([{ a: 1, b: "two", c: null, d: true, e: 2.5 }, [1, 2, 3, "four", null]]);
var big = {};
for (var i = 0; i < 40; ++i) big["k" + i] = i * i;
log([Object.keys(big).length, big.k39, big.k0]);

// Arrays: dense/sparse, push/pop, holes, iteration.
function arrays(n) {
  var a = [];
  for (var i = 0; i < n; ++i) a.push(i);
  a[n + 5] = "sparse";
  var sum = 0;
  for (var j = 0; j < a.length; ++j) if (a[j] !== undefined) sum += typeof a[j] === "number" ? a[j] : 0;
  return [a.length, sum, a.indexOf(7), a.pop(), a.slice(-3).join(",")];
}
log(arrays(60));

// Builtins that read `this` -- the CallBuiltin ThisArg regression.
function builtinThis() {
  function Ctor() {}
  Ctor.method = function () {
    return 1;
  };
  Ctor.method(); // leaves Ctor in the shared outgoing ThisArg slot
  var r = Array.from([10, 20, 30]);
  return [Array.isArray(r), r instanceof Ctor, r.join("|")];
}
log(builtinThis());

// Closures, environments (incl. deeper nesting), recursion.
function makeCounter(start) {
  var n = start;
  return function () {
    var inner = function () {
      return ++n;
    };
    return inner();
  };
}
var c = makeCounter(10);
log([c(), c(), c()]);
function fib(n) {
  return n < 2 ? n : fib(n - 1) + fib(n - 2);
}
log(fib(20));

// Exceptions: try/catch/finally, throw across frames, catch in a loop.
function thrower(i) {
  if (i % 3 === 0) throw new Error("e" + i);
  return i;
}
function exceptions(n) {
  var ok = 0, bad = 0, fin = 0;
  for (var i = 0; i < n; ++i) {
    try {
      ok += thrower(i);
    } catch (e) {
      bad += e.message.length;
    } finally {
      fin++;
    }
  }
  return [ok, bad, fin];
}
log(exceptions(30));
try {
  null.x;
} catch (e) {
  log(e instanceof TypeError);
}

// Switches (dense uint and string), typeof, instanceof, in.
function sw(x) {
  switch (x) {
    case 0: return "zero";
    case 1: return "one";
    case 2: return "two";
    case 3: return "three";
    default: return "many";
  }
}
log([sw(0), sw(2), sw(9)]);
function ssw(s) {
  switch (s) {
    case "alpha": return 1;
    case "beta": return 2;
    case "gamma": return 3;
    default: return 0;
  }
}
log([ssw("alpha"), ssw("gamma"), ssw("zzz")]);
log([typeof 1, typeof "s", typeof undefined, typeof null, typeof {}, typeof log, typeof true]);
log([[] instanceof Array, "x" in { x: 1 }, 0 in [1], "length" in []]);

// Classes, super, getters, static, private-ish patterns.
class Base {
  constructor(v) {
    this.v = v;
  }
  get double() {
    return this.v * 2;
  }
  describe() {
    return "Base(" + this.v + ")";
  }
}
class Derived extends Base {
  constructor(v) {
    super(v + 1);
  }
  describe() {
    return "Derived>" + super.describe();
  }
}
var d = new Derived(4);
log([d.v, d.double, d.describe(), d instanceof Base]);

// Iterators, spread, destructuring, for-of, generators.
function* gen(n) {
  for (var i = 0; i < n; ++i) yield i * i;
}
log([...gen(5)]);
var [p, q, ...rest] = [1, 2, 3, 4, 5];
log([p, q, rest]);
var { a: aa = 9, z: zz = 42 } = { a: 1 };
log([aa, zz]);
var forOf = 0;
for (var v of [1, 2, 3, 4]) forOf += v;
log(forOf);

// for-in over a mutated object (GetPNameList / GetNextPName).
var fi = { one: 1, two: 2, three: 3 };
var keys = [];
for (var k in fi) keys.push(k);
log(keys.sort().join(","));

// arguments object, reified and not.
function argSum() {
  var s = 0;
  for (var i = 0; i < arguments.length; ++i) s += arguments[i];
  return [s, arguments.length];
}
log(argSum(1, 2, 3, 4));

// Higher-order builtins that call back into JIT'd code.
log([1, 2, 3, 4, 5].map(function (x) { return x * x; }).filter(function (x) { return x % 2; }));
log([3, 1, 2].sort(function (a, b) { return a - b; }).join("-"));
log([1, 2, 3].reduce(function (a, b) { return a + b; }, 0));

// JSON round-trip (exercises the runtime broadly).
log(JSON.parse(JSON.stringify({ n: 1, s: "x", b: [1, null, true] })).b[2]);

print(out.join("\n"));

// Compile-status pins, in the order each function is first entered (which
// interleaves with the program's own output on stdout; see the header).
// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK: JIT successfully compiled FunctionID 2, 'arith'
// CHECK: JIT successfully compiled FunctionID 1, 'log'
// CHECK: JIT successfully compiled FunctionID 3, 'cmp'
// CHECK: JIT successfully compiled FunctionID 4, 'strs'
// CHECK: JIT successfully compiled FunctionID 6, 'props'
// CHECK: JIT successfully compiled FunctionID 5, 'Point'
// CHECK: JIT successfully compiled FunctionID 17, ''
// CHECK: JIT successfully compiled FunctionID 18, 'get'
// CHECK: JIT successfully compiled FunctionID 7, 'arrays'
// CHECK: JIT successfully compiled FunctionID 8, 'builtinThis'
// CHECK: JIT successfully compiled FunctionID 29, ''
// CHECK: JIT successfully compiled FunctionID 9, 'makeCounter'
// CHECK: JIT successfully compiled FunctionID 30, ''
// CHECK: JIT successfully compiled FunctionID 10, 'fib'
// CHECK: JIT successfully compiled FunctionID 12, 'exceptions'
// CHECK: JIT successfully compiled FunctionID 11, 'thrower'
// CHECK: JIT successfully compiled FunctionID 13, 'sw'
// CHECK: JIT successfully compiled FunctionID 14, 'ssw'
// CHECK: JIT successfully compiled FunctionID 22, 'Derived'
// CHECK: JIT successfully compiled FunctionID 19, 'Base'
// CHECK: JIT successfully compiled FunctionID 20, 'get double'
// CHECK: JIT successfully compiled FunctionID 23, 'describe'
// CHECK: JIT successfully compiled FunctionID 21, 'describe'
// CHECK: JIT successfully compiled FunctionID 15, 'gen'
// CHECK: JIT successfully compiled FunctionID 31, 'gen'
// CHECK: JIT successfully compiled FunctionID 16, 'argSum'
// CHECK: JIT successfully compiled FunctionID 24, ''
// CHECK: JIT successfully compiled FunctionID 25, ''
// CHECK: JIT successfully compiled FunctionID 26, ''
// CHECK: JIT successfully compiled FunctionID 27, ''
// CHECK-NEXT: 90100,2444.371996
// CHECK-NEXT: NaN,Infinity,-Infinity,0,true,true
// CHECK-NEXT: 2147483648,-1073741824,15,2,-2
// CHECK-NEXT: 9007199254740992,0.30000000000000004,Infinity
// CHECK-NEXT: true,true,false,false,false,false,true,true,!ge
// CHECK-NEXT: false,false,false,false,false,false,true,true,!lt,!ge
// CHECK-NEXT: false,false,false,false,false,false,true,true,!lt,!ge
// CHECK-NEXT: true,true,false,false,false,false,true,true,!ge
// CHECK-NEXT: false,false,false,false,true,false,false,true,!lt,!ge
// CHECK-NEXT: 50,53,345678,true,true
// CHECK-NEXT: x1,1x,anull,undefined,[object Object],1,2
// CHECK-NEXT: 666700,P0P20P40P60P80
// CHECK-NEXT: [object Object],1,2,3,four,
// CHECK-NEXT: 40,1521,0
// CHECK-NEXT: 66,1770,7,sparse,,,
// CHECK-NEXT: true,false,10|20|30
// CHECK-NEXT: 11,12,13
// CHECK-NEXT: 6765
// CHECK-NEXT: 300,26,30
// CHECK-NEXT: true
// CHECK-NEXT: zero,two,many
// CHECK-NEXT: 1,3,0
// CHECK-NEXT: number,string,undefined,object,object,function,boolean
// CHECK-NEXT: true,true,true,true
// CHECK-NEXT: 5,10,Derived>Base(5),true
// CHECK-NEXT: 0,1,4,9,16
// CHECK-NEXT: 1,2,3,4,5
// CHECK-NEXT: 1,42
// CHECK-NEXT: 10
// CHECK-NEXT: one,three,two
// CHECK-NEXT: 10,4
// CHECK-NEXT: 1,9,25
// CHECK-NEXT: 1-2-3
// CHECK-NEXT: 6
// CHECK-NEXT: true
//
// CHECK0: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 2, 'arith'
// CHECK0: JIT successfully compiled FunctionID 1, 'log'
// CHECK0: JIT successfully compiled FunctionID 3, 'cmp'
// CHECK0: JIT successfully compiled FunctionID 4, 'strs'
// CHECK0: JIT successfully compiled FunctionID 6, 'props'
// CHECK0: JIT successfully compiled FunctionID 5, 'Point'
// CHECK0: JIT successfully compiled FunctionID 17, ''
// CHECK0: JIT successfully compiled FunctionID 18, 'get'
// CHECK0: JIT successfully compiled FunctionID 7, 'arrays'
// CHECK0: JIT successfully compiled FunctionID 8, 'builtinThis'
// CHECK0: JIT successfully compiled FunctionID 29, ''
// CHECK0: JIT successfully compiled FunctionID 9, 'makeCounter'
// CHECK0: JIT successfully compiled FunctionID 30, ''
// CHECK0: JIT successfully compiled FunctionID 32, 'inner'
// CHECK0: JIT successfully compiled FunctionID 10, 'fib'
// CHECK0: JIT successfully compiled FunctionID 12, 'exceptions'
// CHECK0: JIT successfully compiled FunctionID 11, 'thrower'
// CHECK0: JIT successfully compiled FunctionID 13, 'sw'
// CHECK0: JIT successfully compiled FunctionID 14, 'ssw'
// CHECK0: JIT successfully compiled FunctionID 22, 'Derived'
// CHECK0: JIT successfully compiled FunctionID 19, 'Base'
// CHECK0: JIT successfully compiled FunctionID 20, 'get double'
// CHECK0: JIT successfully compiled FunctionID 23, 'describe'
// CHECK0: JIT successfully compiled FunctionID 21, 'describe'
// CHECK0: JIT successfully compiled FunctionID 15, 'gen'
// CHECK0: JIT successfully compiled FunctionID 31, 'gen'
// CHECK0: JIT successfully compiled FunctionID 16, 'argSum'
// CHECK0: JIT successfully compiled FunctionID 24, ''
// CHECK0: JIT successfully compiled FunctionID 25, ''
// CHECK0: JIT successfully compiled FunctionID 26, ''
// CHECK0: JIT successfully compiled FunctionID 27, ''
// CHECK0-NEXT: 90100,2444.371996
// CHECK0-NEXT: NaN,Infinity,-Infinity,0,true,true
// CHECK0-NEXT: 2147483648,-1073741824,15,2,-2
// CHECK0-NEXT: 9007199254740992,0.30000000000000004,Infinity
// CHECK0-NEXT: true,true,false,false,false,false,true,true,!ge
// CHECK0-NEXT: false,false,false,false,false,false,true,true,!lt,!ge
// CHECK0-NEXT: false,false,false,false,false,false,true,true,!lt,!ge
// CHECK0-NEXT: true,true,false,false,false,false,true,true,!ge
// CHECK0-NEXT: false,false,false,false,true,false,false,true,!lt,!ge
// CHECK0-NEXT: 50,53,345678,true,true
// CHECK0-NEXT: x1,1x,anull,undefined,[object Object],1,2
// CHECK0-NEXT: 666700,P0P20P40P60P80
// CHECK0-NEXT: [object Object],1,2,3,four,
// CHECK0-NEXT: 40,1521,0
// CHECK0-NEXT: 66,1770,7,sparse,,,
// CHECK0-NEXT: true,false,10|20|30
// CHECK0-NEXT: 11,12,13
// CHECK0-NEXT: 6765
// CHECK0-NEXT: 300,26,30
// CHECK0-NEXT: true
// CHECK0-NEXT: zero,two,many
// CHECK0-NEXT: 1,3,0
// CHECK0-NEXT: number,string,undefined,object,object,function,boolean
// CHECK0-NEXT: true,true,true,true
// CHECK0-NEXT: 5,10,Derived>Base(5),true
// CHECK0-NEXT: 0,1,4,9,16
// CHECK0-NEXT: 1,2,3,4,5
// CHECK0-NEXT: 1,42
// CHECK0-NEXT: 10
// CHECK0-NEXT: one,three,two
// CHECK0-NEXT: 10,4
// CHECK0-NEXT: 1,9,25
// CHECK0-NEXT: 1-2-3
// CHECK0-NEXT: 6
// CHECK0-NEXT: true
