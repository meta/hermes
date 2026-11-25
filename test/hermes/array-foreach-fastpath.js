/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermesc -O -fstatic-builtins -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// Array.prototype.forEach reads the elements of a JSArray directly from its
// indexed storage, and uses the generic HasProperty/Get lookup for everything
// else. Check both paths, and the callback changing the array in between.

print("forEach");
// CHECK-LABEL: forEach

function show(name, v) {
  print(name + ": " + JSON.stringify(v));
}
var out, a, so;

// Value, index and array, and the return value.
out = [];
var ret = [10, 20, 30].forEach(function (v, i, o) {
  out.push(v + "@" + i + " " + typeof i + " " + o.length);
});
show("basic", out);
// CHECK-NEXT: basic: ["10@0 number 3","20@1 number 3","30@2 number 3"]
show("return", ret === undefined);
// CHECK-NEXT: return: true

out = [];
[1, 2].forEach(function (v) {
  out.push(this.x + v);
}, {x: 100});
[1].forEach(function () {
  "use strict";
  out.push(this === undefined);
});
show("thisArg", out);
// CHECK-NEXT: thisArg: [101,102,true]

// Holes are skipped, unless the prototype chain has the index.
out = [];
[1, , 3].forEach(function (v, i) {
  out.push(v + "@" + i);
});
show("holes", out);
// CHECK-NEXT: holes: ["1@0","3@2"]

Array.prototype[1] = "proto";
out = [];
[1, , 3].forEach(function (v, i) {
  out.push(v + "@" + i);
});
show("protoHole", out);
// CHECK-NEXT: protoHole: ["1@0","proto@1","3@2"]
delete Array.prototype[1];

// The tests with accessors on Array.prototype collect their results in
// strings, because the accessors also apply to the result arrays.
a = [0, , 2];
Object.defineProperty(Array.prototype, 1, {
  get: function () {
    return this === a ? "getter" : "bad receiver";
  },
  configurable: true,
});
so = "";
a.forEach(function (v) {
  so += v + ",";
});
print("protoGetter: " + so);
// CHECK-NEXT: protoGetter: 0,getter,2,
delete Array.prototype[1];

// An own element hides a prototype getter with the same index.
Object.defineProperty(Array.prototype, 0, {
  get: function () {
    throw new Error("prototype getter called");
  },
  configurable: true,
});
so = "";
[5, 6].forEach(function (v, i) {
  so += v + "@" + i + ",";
});
print("protoShadowed: " + so);
// CHECK-NEXT: protoShadowed: 5@0,6@1,
delete Array.prototype[0];

// A prototype getter that shrinks and rewrites the array.
a = [1, , 3, 4, 5];
Object.defineProperty(Array.prototype, 1, {
  get: function () {
    a.length = 3;
    a[2] = "patched";
    return "g";
  },
  configurable: true,
});
so = "";
a.forEach(function (v, i) {
  so += "[" + v + "," + i + "]";
});
print("protoGetterMutate: " + so);
// CHECK-NEXT: protoGetterMutate: [1,0][g,1][patched,2]
delete Array.prototype[1];

Object.defineProperty(Array.prototype, 0, {
  get: function () {
    throw new RangeError("from getter");
  },
  configurable: true,
});
so = "";
try {
  [, 2].forEach(function (v) {
    so += v;
  });
} catch (e) {
  so += e.name + " " + e.message;
}
print("protoThrow: " + so);
// CHECK-NEXT: protoThrow: RangeError from getter
delete Array.prototype[0];

// A Proxy on the prototype chain sees "has" for a hole, and "get" only if
// the index is present.
var log = [];
var handler = {
  has: function (t, k) {
    log.push("has " + String(k));
    return k in t;
  },
  get: function (t, k) {
    log.push("get " + String(k));
    return t[k];
  },
};
a = [1, , 3, , 5];
Object.setPrototypeOf(a, new Proxy({3: "t3"}, handler));
Array.prototype.forEach.call(a, function (v) {
  log.push(v);
});
show("protoProxy", log);
// CHECK-NEXT: protoProxy: [1,"has 1",3,"has 3","get 3","t3",5]

// The callback changes the array.
out = [];
a = [1, 2, 3, 4, 5];
a.forEach(function (v, i) {
  out.push(v);
  if (i == 1)
    a.length = 3;
});
show("shrink", out);
// CHECK-NEXT: shrink: [1,2,3]

Array.prototype[3] = "P3";
out = [];
a = [0, 1, 2, 3, 4];
a.forEach(function (v, i) {
  out.push(v);
  if (i == 1)
    a.length = 2;
});
show("shrinkProto", out);
// CHECK-NEXT: shrinkProto: [0,1,"P3"]
delete Array.prototype[3];

out = [];
a = [1, 2, 3];
a.forEach(function (v) {
  out.push(v);
  a.push(v * 10);
});
show("grow", out);
// CHECK-NEXT: grow: [1,2,3]
show("grown", a);
// CHECK-NEXT: grown: [1,2,3,10,20,30]

out = [];
a = [1, 2, 3, 4];
a.forEach(function (v, i) {
  out.push(v);
  if (i + 1 < a.length)
    a[i + 1] = v * 10;
});
show("writeAhead", out);
// CHECK-NEXT: writeAhead: [1,10,100,1000]

out = [];
a = [1, 2, 3, 4];
a.forEach(function (v, i) {
  out.push(v);
  delete a[i + 1];
});
show("deleteAhead", out);
// CHECK-NEXT: deleteAhead: [1,3]

out = [];
a = [1, 2, 3, 4, 5];
a.forEach(function (v) {
  out.push(v);
  a.shift();
});
show("shift", out);
// CHECK-NEXT: shift: [1,3,5]

out = [];
a = [1, 2, 3];
a.forEach(function (v, i) {
  out.push(v);
  if (i == 0) {
    a.length = 0;
    a.push("n1", "n2", "n3");
  }
});
show("refill", out);
// CHECK-NEXT: refill: [1,"n2","n3"]

out = [];
a = [1, 2, 3, 4];
a.forEach(function (v, i) {
  out.push(v);
  if (i == 0) {
    Object.defineProperty(a, 2, {
      get: function () {
        return "getter";
      },
    });
  }
});
show("accessorMid", out);
// CHECK-NEXT: accessorMid: [1,2,"getter",4]

out = [];
a = [1, 2, 3];
a.forEach(function (v, i) {
  out.push(v);
  if (i == 0)
    Object.defineProperty(a, 1, {value: "ro", writable: false});
});
show("readOnlyMid", out);
// CHECK-NEXT: readOnlyMid: [1,"ro",3]

a = [1, 2, 3, 4];
Object.defineProperty(a, 3, {
  get: function () {
    return "acc";
  },
  enumerable: true,
  configurable: true,
});
out = [];
a.forEach(function (v) {
  out.push(v);
});
show("accessorBefore", out);
// CHECK-NEXT: accessorBefore: [1,2,3,"acc"]

a = [1, , 3];
Object.setPrototypeOf(a, {__proto__: Array.prototype, 1: "x"});
out = [];
a.forEach(function (v) {
  out.push(v);
});
show("setProto", out);
// CHECK-NEXT: setProto: [1,"x",3]

a = [1, , 3];
out = [];
a.forEach(function (v, i) {
  out.push(v);
  if (i == 0)
    Object.setPrototypeOf(a, {__proto__: Array.prototype, 1: "y"});
});
show("setProtoMid", out);
// CHECK-NEXT: setProtoMid: [1,"y",3]

out = [];
a = [1, 2, 3, 4];
a.forEach(function (v, i) {
  out.push(v);
  if (i == 1) {
    Object.freeze(a);
    out.push(Reflect.set(a, 2, "ignored"));
  }
});
show("freezeMid", {out: out, frozen: Object.isFrozen(a)});
// CHECK-NEXT: freezeMid: {"out":[1,2,false,3,4],"frozen":true}

out = [];
a = [1, 2, 3, 4];
a.forEach(function (v, i) {
  out.push(v);
  if (i == 1) {
    Object.seal(a);
    a[2] = "written";
  }
});
show("sealMid", {out: out, sealed: Object.isSealed(a)});
// CHECK-NEXT: sealMid: {"out":[1,2,"written",4],"sealed":true}

out = [];
Object.freeze([7, 8, 9]).forEach(function (v, i) {
  out.push(v + i);
});
show("frozen", out);
// CHECK-NEXT: frozen: [7,9,11]

out = [];
a = [1, 2, 3, 4];
a.forEach(function (v, i) {
  out.push(v);
  if (i == 0) {
    a.length = 2;
    Object.defineProperty(a, "length", {writable: false});
  }
});
show("lengthReadOnly", {out: out, length: a.length});
// CHECK-NEXT: lengthReadOnly: {"out":[1,2],"length":2}

// The indexed storage starts at index 2.
a = [];
a[3] = "x";
a[4] = "y";
a[2] = "w";
out = [];
a.forEach(function (v, i) {
  out.push(v + "@" + i);
});
show("beginIndex", out);
// CHECK-NEXT: beginIndex: ["w@2","x@3","y@4"]

a = [];
a[0] = "first";
a[1000] = "far";
out = [];
a.forEach(function (v, i) {
  out.push(i);
});
show("sparse", out);
// CHECK-NEXT: sparse: [0,1000]

// Element values of every kind, including doubles that are boxed in HV32.
var obj = {};
var values = [Symbol("s"), 10n, 1.5, -0, 2 ** 40 + 0.5, NaN, 1e-300, null];
values.push(undefined, true, "s", obj);
values.forEach(function (v, i) {
  var str = v === obj ? "is obj true" : String(v);
  if (Object.is(v, -0))
    str += " (-0)";
  print("value " + i + ": " + typeof v + " " + str);
});
// CHECK-NEXT: value 0: symbol Symbol(s)
// CHECK-NEXT: value 1: bigint 10
// CHECK-NEXT: value 2: number 1.5
// CHECK-NEXT: value 3: number 0 (-0)
// CHECK-NEXT: value 4: number 1099511627776.5
// CHECK-NEXT: value 5: number NaN
// CHECK-NEXT: value 6: number 1e-300
// CHECK-NEXT: value 7: object null
// CHECK-NEXT: value 8: undefined undefined
// CHECK-NEXT: value 9: boolean true
// CHECK-NEXT: value 10: string s
// CHECK-NEXT: value 11: object is obj true

// Re-entrant forEach on the same array.
a = [1, 2, 3];
out = [];
a.forEach(function (v, i) {
  a.forEach(function (w, j) {
    out.push(v * 10 + w);
    if (i == 0 && j == 0)
      a.pop();
  });
});
show("nested", out);
// CHECK-NEXT: nested: [11,12,21,22]

// Callback kinds.
out = [];
[1, 2].forEach(
  function (v) {
    out.push(this.k + v);
  }.bind({k: 100}),
);
[3].forEach(
  new Proxy(function (v) {
    out.push(v * 3);
  }, {}),
);
[4].forEach(
  new Proxy(function () {}, {
    apply: function (target, thisArg, args) {
      out.push("apply " + args[0] + " " + args[1]);
    },
  }),
);
show("callbacks", out);
// CHECK-NEXT: callbacks: [101,102,9,"apply 4 0"]

["a", "b"].forEach(print);
// CHECK-NEXT: a 0 a,b
// CHECK-NEXT: b 1 a,b

var pushed = [];
[7, 8].forEach(Array.prototype.push, pushed);
show("nativePush", pushed);
// CHECK-NEXT: nativePush: [7,0,[7,8],8,1,[7,8]]

out = [];
try {
  [1, 2, 3].forEach(function (v) {
    out.push(v);
    if (v == 2)
      throw new Error("boom");
  });
} catch (e) {
  out.push(e.message);
}
show("throw", out);
// CHECK-NEXT: throw: [1,2,"boom"]

// Receivers other than arrays.
out = [];
Array.prototype.forEach.call({length: 3, 0: "a", 2: "c"}, function (v, i) {
  out.push(v + "@" + i);
});
show("arrayLike", out);
// CHECK-NEXT: arrayLike: ["a@0","c@2"]

var lenReads = 0;
out = [];
Array.prototype.forEach.call(
  {
    get length() {
      lenReads++;
      return 2;
    },
    0: "a",
    1: "b",
  },
  function (v) {
    out.push(v);
  },
);
show("lengthGetter", {out: out, lenReads: lenReads});
// CHECK-NEXT: lengthGetter: {"out":["a","b"],"lenReads":1}

try {
  Array.prototype.forEach.call(
    {length: 2 ** 53 + 10, 0: "a", 1: "b"},
    function (v, i) {
      if (i == 1)
        throw "stopped at " + v;
    },
  );
} catch (e) {
  print("hugeLength: " + e);
}
// CHECK-NEXT: hugeLength: stopped at b

out = [];
Array.prototype.forEach.call("xyz", function (v, i, o) {
  out.push(v + i + typeof o);
});
show("string", out);
// CHECK-NEXT: string: ["x0object","y1object","z2object"]

Number.prototype.length = 2;
Number.prototype[1] = "n1";
out = [];
Array.prototype.forEach.call(5, function (v, i, o) {
  out.push(v + "@" + i + " " + typeof o);
});
Array.prototype.forEach.call(true, function () {
  out.push("bool");
});
delete Number.prototype.length;
delete Number.prototype[1];
show("primitive", out);
// CHECK-NEXT: primitive: ["n1@1 object"]

out = [];
Array.prototype.forEach.call(new Int16Array([5, 6, 7]), function (v, i) {
  out.push(v + i);
});
show("typedArray", out);
// CHECK-NEXT: typedArray: [5,7,9]

(function () {
  out = [];
  Array.prototype.forEach.call(arguments, function (v) {
    out.push(v);
  });
  show("arguments", out);
})(9, 8, 7);
// CHECK-NEXT: arguments: [9,8,7]

var proxyArray = new Proxy([1, 2, 3], {
  get: function (t, k) {
    return k === "length" ? t.length : t[k] * 2;
  },
});
out = [];
Array.prototype.forEach.call(proxyArray, function (v) {
  out.push(v);
});
show("proxyReceiver", out);
// CHECK-NEXT: proxyReceiver: [2,4,6]

// A Proxy receiver sees one "get" of length, then "has" for every index and
// "get" only for the present ones.
log = [];
var proxyHoles = new Proxy([10, , 30], handler);
Array.prototype.forEach.call(proxyHoles, function (v, i, o) {
  log.push(v + "@" + i + " " + (o === proxyHoles));
});
show("proxyReceiverTraps", log);
// CHECK-NEXT: proxyReceiverTraps: ["get length","has 0","get 0","10@0 true","has 1","has 2","get 2","30@2 true"]

class MyArray extends Array {}
var myArray = MyArray.from([4, 5]);
out = [];
myArray.forEach(function (v) {
  out.push(v);
});
show("subclass", {out: out, isMyArray: myArray instanceof MyArray});
// CHECK-NEXT: subclass: {"out":[4,5],"isMyArray":true}

// A non-callable callback throws a TypeError after ToObject and
// LengthOfArrayLike, and before any element is read.
function tryForEach(name, thisArg, callback) {
  try {
    Array.prototype.forEach.call(thisArg, callback);
    print(name + ": no error");
  } catch (e) {
    print(name + ": " + e.name + ": " + e.message);
  }
}
tryForEach("nonCallable", [1], 42);
// CHECK-NEXT: nonCallable: TypeError: Array.prototype.forEach() requires a callable argument
tryForEach("noCallback", [], undefined);
// CHECK-NEXT: noCallback: TypeError: Array.prototype.forEach() requires a callable argument
tryForEach("nonCallableProxy", [1], new Proxy({}, {}));
// CHECK-NEXT: nonCallableProxy: TypeError: Array.prototype.forEach() requires a callable argument
tryForEach("nullThis", null, 42);
// CHECK-NEXT: nullThis: TypeError: Cannot convert null value to object
tryForEach("undefinedThis", undefined, 42);
// CHECK-NEXT: undefinedThis: TypeError: Cannot convert undefined value to object
var steps = [];
tryForEach(
  "order",
  {
    get length() {
      steps.push("length");
      return {
        valueOf: function () {
          steps.push("valueOf");
          return 1;
        },
      };
    },
    get 0() {
      steps.push("element");
      return 0;
    },
  },
  42,
);
show("orderSteps", steps);
// CHECK-NEXT: order: TypeError: Array.prototype.forEach() requires a callable argument
// CHECK-NEXT: orderSteps: ["length","valueOf"]

// The callback and a getter run the GC, which can move the array and its
// elements.
a = [];
for (var i = 0; i < 50; i++)
  a.push({i: i, d: i + 0.5});
var total = 0;
a.forEach(function (v, i) {
  if (i % 10 == 0)
    gc();
  var tmp = [{j: i}, {j: i + 1}];
  if (v.i !== i || v.d !== i + 0.5)
    throw new Error("bad element " + i);
  total += v.i + tmp.length;
});
print("gc: " + total);
// CHECK-NEXT: gc: 1325

a = [{n: 0}, , {n: 2}];
Object.defineProperty(Array.prototype, 1, {
  get: function () {
    gc();
    return {n: 1};
  },
  configurable: true,
});
so = "";
a.forEach(function (v, i) {
  gc();
  so += v.n + "@" + i + ",";
});
print("gcGetter: " + so);
// CHECK-NEXT: gcGetter: 0@0,1@1,2@2,
delete Array.prototype[1];

// Re-entry through native frames only: every level is a forEach bound to a
// one-element holey array, so every level takes the generic lookup (a
// prototype getter or a Proxy prototype) while the outer levels wait.
var getterCalls = 0;
Object.defineProperty(Array.prototype, 0, {
  get: function () {
    getterCalls++;
    return 0;
  },
  configurable: true,
});
var chain = function () {};
for (var i = 0; i < 100; i++)
  chain = Array.prototype.forEach.bind([,], chain);
chain();
delete Array.prototype[0];
print("reentrantGetter: " + getterCalls);
// CHECK-NEXT: reentrantGetter: 100

var proxyGets = 0;
var proxyProto = new Proxy(
  {0: 0},
  {
    get: function (t, k) {
      proxyGets++;
      return t[k];
    },
  },
);
chain = function () {};
for (var i = 0; i < 100; i++) {
  var holey = [,];
  Object.setPrototypeOf(holey, proxyProto);
  chain = Array.prototype.forEach.bind(holey, chain);
}
chain();
print("reentrantProxy: " + proxyGets);
// CHECK-NEXT: reentrantProxy: 100
