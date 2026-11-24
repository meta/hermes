/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s

// Array.prototype.sort on objects that do not take the array fast path also
// reads the elements into a list, sorts the list and writes it back
// (ES2025 23.1.3.30).

print("sort-generic");
// CHECK-LABEL: sort-generic

// A comparator that throws leaves a subclass instance unchanged.
class Sub extends Array {}
var sub = Sub.of(3, 1, 2, 5, 4);
var n = 0;
try {
  sub.sort(function (a, b) {
    if (++n === 3) throw new Error("boom");
    return a - b;
  });
} catch (e) {
  print(e.message + " " + Array.from(sub));
}
// CHECK-NEXT: boom 3,1,2,5,4

// Holes in a subclass instance move to the end and stay holes.
var holey = new Sub(5);
holey[0] = 3;
holey[3] = 1;
holey.sort();
print(holey[0] + "," + holey[1] + " " + (2 in holey) + " " + (3 in holey));
// CHECK-NEXT: 1,3 false false

// The write-back to a frozen array throws, even if it is already sorted
// and even for a single element.
[[1, 2], [1]].forEach(function (arr) {
  try {
    Object.freeze(arr).sort();
    print("no throw");
  } catch (e) {
    print(e.name);
  }
});
// CHECK-NEXT: TypeError
// CHECK-NEXT: TypeError

// The comparator sees an array-like object unchanged while it sorts.
var arrayLike = {0: "b", 1: "a", length: 2};
var seen;
Array.prototype.sort.call(arrayLike, function (a, b) {
  seen = (0 in arrayLike) + "/" + (1 in arrayLike);
  return a < b ? -1 : 1;
});
print(seen + " " + arrayLike[0] + arrayLike[1]);
// CHECK-NEXT: true/true ab

// An inherited element is read and written back as an own property.
var inherits = Object.create({1: "a"});
inherits[0] = "b";
inherits.length = 2;
Array.prototype.sort.call(inherits);
print(inherits[0] + inherits[1] + " " + inherits.hasOwnProperty(1));
// CHECK-NEXT: ab true

// A getter that adds and deletes elements changes which indices are read.
var setValues = [];
var mutating = {2: "c", length: 10};
Object.defineProperty(mutating, "0", {
  get: function () {
    mutating[5] = "a";
    delete mutating[2];
    return "b";
  },
  set: function (v) {
    setValues.push(v);
  },
  configurable: true,
});
Array.prototype.sort.call(mutating);
print(setValues + " " + mutating[1] + " " + (2 in mutating) + " " + (5 in mutating));
// CHECK-NEXT: a b false false

// A proxy sees HasProperty and Get for every index, then Set for the sorted
// elements and DeletePropertyOrThrow for the rest.
var log = [];
var proxy = new Proxy([2, , 1], {
  has: function (t, k) { log.push("has" + String(k)); return k in t; },
  get: function (t, k) {
    if (k !== "length") log.push("get" + String(k));
    return t[k];
  },
  set: function (t, k, v) { log.push("set" + String(k)); t[k] = v; return true; },
  deleteProperty: function (t, k) { log.push("del" + String(k)); return delete t[k]; },
});
Array.prototype.sort.call(proxy);
print(log.join(" "));
// CHECK-NEXT: has0 get0 has1 has2 get2 set0 set1 del2

// A proxy on the prototype chain sees HasProperty for each index the object
// lacks, and Get for those it has, so every index is visited.
function tracer(log) {
  return {
    has: function (t, k) { log.push("has" + String(k)); return k in t; },
    get: function (t, k, r) {
      log.push("get" + String(k));
      return Reflect.get(t, k, r);
    },
    set: function (t, k, v, r) {
      log.push("set" + String(k));
      return Reflect.set(t, k, v, r);
    },
  };
}
var protoLog = [];
var onProto = Object.create(new Proxy({2: "a"}, tracer(protoLog)));
onProto[0] = "c";
onProto[3] = "b";
onProto.length = 5;
protoLog.length = 0;
Array.prototype.sort.call(onProto);
print(protoLog.join(" ") + " | " + onProto[0] + onProto[1] + onProto[2] + " " +
    onProto.hasOwnProperty(2) + " " + onProto.hasOwnProperty(3));
// CHECK-NEXT: has1 has2 get2 has4 set1 set2 | abc true false

// A getter that puts a proxy on the prototype chain makes the rest of the
// read visit every index.
var laterLog = [];
var later = {3: "a", length: 4};
Object.defineProperty(later, "0", {
  get: function () {
    Object.setPrototypeOf(later, new Proxy({}, tracer(laterLog)));
    return "b";
  },
  set: function (v) {
    laterLog.push("setter " + v);
  },
  configurable: true,
});
Array.prototype.sort.call(later);
print(laterLog.join(" "));
// CHECK-NEXT: has1 has2 setter a set1
