/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s


print("Map prototype and constructor");
//CHECK-LABEL: Map prototype and constructor

print(Map.prototype);
//CHECK-NEXT: [object Map]
print(Map.length);
//CHECK-NEXT: 0

print(Object.getOwnPropertyNames(Map.prototype));
//CHECK-NEXT: clear,delete,entries,forEach,get,has,keys,set,size,values,constructor

print(new Map());
//CHECK-NEXT: [object Map]
var x = new Map(new Set([[1,2], [3,4]]));
print(x.get(1), x.get(3));
// CHECK-NEXT: 2 4

try {
  Map();
} catch (e) {
  print(e);
}
//CHECK-NEXT: TypeError: Constructor Map requires 'new'

try{ Map.prototype.clear(); } catch (e) { print(e); }
//CHECK-NEXT: TypeError: Non-Map object called on Map.prototype.clear

var m = new Map([]);
print(m.size);
// CHECK-NEXT: 0
var m = new Map([[1,2]]);
print(m.size, m.get(1));
// CHECK-NEXT: 1 2
var m = new Map([[1,2], [3,4]]);
print(m.size, m.get(1), m.get(3));
// CHECK-NEXT: 2 2 4
try { new Map([null]); } catch (e) { print('caught', e.name) }
// CHECK-NEXT: caught TypeError

var oldSet = Map.prototype.set;
Map.prototype.set = 123;
try { new Map([[1,2]]); } catch (e) { print('caught', e.name) }
// CHECK-NEXT: caught TypeError
var arr = [[1,2], [3,4]];
Map.prototype.set = function(k, v) {
  oldSet.call(this, k, v);
  arr.length = 1;
}
print(new Map(arr).size);
// CHECK-NEXT: 1
Map.prototype.set = function(k, v) {
  oldSet.call(this, k, v);
  // Changing the set function mid-construction shouldn't affect the call.
  Map.prototype.set = 10;
};
var m = new Map([[1,2], [3,4]]);
print(m.size, m.get(1), m.get(3));
// CHECK-NEXT: 2 2 4
Map.prototype.set = oldSet;

// Make sure we can insert internal object ID into a frozen object.
var fo = {};
Object.freeze(fo);
var fs = new Map();
fs.set(fo, fo);

function testSet(o1, o2, o3) {
  var s = new Map();
  print("testSet");
//CHECK-LABEL: testSet
  print(s.size);
//CHECK-NEXT: 0
  s.set(undefined, 0);
  s.set("abc", 1);
  s.set("def", 2);
  s.set("abc", 3);
  s.set("def", 4);
  s.set(1/-Infinity, 5);
  s.set(o1, 6);
  s.set(o2, 7);
  s.set(o3, 8);
  var o11 = o1, o22 = o2, o33 = o3;
  s.set(o11, 9);
  s.set(o22, 10);
  s.set(o33, 11);
  print(s.size);
  //CHECK-NEXT: 7
  return s;
}

function testHas(s, o1, o2, o3) {
  print("testHas");
//CHECK-LABEL: testHas
  print(s.has(0));
//CHECK-NEXT: true
  print(s.has(1));
//CHECK-NEXT: false
  print(s.has("abc"));
//CHECK-NEXT: true
  print(s.has("a"));
//CHECK-NEXT: false
  print(s.has("def"));
//CHECK-NEXT: true
  print(s.has());
//CHECK-NEXT: true
  print(s.has(null));
//CHECK-NEXT: false
  print(s.has(o1));
//CHECK-NEXT: true
  print(s.has(o2));
//CHECK-NEXT: true
  print(s.has(o3));
//CHECK-NEXT: true
  print(s.has({}));
//CHECK-NEXT: false
  var o4 = o2;
  print(s.has(o4));
//CHECK-NEXT: true
}

function testGet(s, o1, o2, o3) {
  print("testGet");
//CHECK-LABEL: testGet
  print(s.get(0));
//CHECK-NEXT: 5
  print(s.get(1));
//CHECK-NEXT: undefined
  print(s.get("abc"));
//CHECK-NEXT: 3
  print(s.get("a"));
//CHECK-NEXT: undefined
  print(s.get("def"));
//CHECK-NEXT: 4
  print(s.get());
//CHECK-NEXT: 0
  print(s.get(null));
//CHECK-NEXT: undefined
  print(s.get(o1));
//CHECK-NEXT: 9
  print(s.get(o2));
//CHECK-NEXT: 10
  print(s.get(o3));
//CHECK-NEXT: 11
  print(s.get({}));
//CHECK-NEXT: undefined
  var o4 = o2;
  print(s.get(o4));
//CHECK-NEXT: 10
}



function testDelete(s, o1, o2, o3) {
  print("testDelete");
//CHECK-LABEL: testDelete
  print(s.delete(0));
//CHECK-NEXT: true
  print(s.size);
//CHECK-NEXT: 6
  print(s.delete(1));
//CHECK-NEXT: false
  print(s.delete("abc"));
//CHECK-NEXT: true
  print(s.size);
//CHECK-NEXT: 5
  print(s.delete("a"));
//CHECK-NEXT: false
  print(s.delete("def"));
//CHECK-NEXT: true
  print(s.size);
//CHECK-NEXT: 4
  print(s.delete());
//CHECK-NEXT: true
  print(s.size);
//CHECK-NEXT: 3
  print(s.delete(null));
//CHECK-NEXT: false
  print(s.delete(o1));
//CHECK-NEXT: true
  print(s.size);
//CHECK-NEXT: 2
  var o4 = o2;
  print(s.delete(o4));
//CHECK-NEXT: true
  print(s.size);
//CHECK-NEXT: 1
  print(s.delete(o3));
//CHECK-NEXT: true
  print(s.size);
//CHECK-NEXT: 0
  print(s.delete({}));
//CHECK-NEXT: false

  print(s.has(0));
//CHECK-NEXT: false
  print(s.has("abc"));
//CHECK-NEXT: false
  print(s.has("def"));
//CHECK-NEXT: false
  print(s.has());
//CHECK-NEXT: false
  print(s.has(o1));
//CHECK-NEXT: false
  print(s.has(o2));
//CHECK-NEXT: false
  print(s.has(o3));
//CHECK-NEXT: false
}

function testClear(o1, o2, o3) {
  var s = testSet(o1, o2, o3);

  print("testClear");
//CHECK-LABEL: testClear
  print(s.size);
//CHECK-NEXT: 7
  s.clear();
  print(s.size);
//CHECK-NEXT: 0
}

function testIteration() {
  s = new Map();
  s.set(1, {});
  s.set({}, 1);
  s.set(undefined, null);
  s.set("abc", "def");

  print("testIteration");
//CHECK-LABEL: testIteration
  var keys = s.keys();
  print(keys.__proto__);
//CHECK-NEXT: [object Map Iterator]
  print(keys[Symbol.iterator]());
//CHECK-NEXT: [object Map Iterator]
  print(Object.getOwnPropertyNames(keys.__proto__));
//CHECK-NEXT: next
  while (true) {
    var e = keys.next();
    print(e.value);
    if (e.done) break;
  }
//CHECK-NEXT: 1
//CHECK-NEXT: [object Object]
//CHECK-NEXT: undefined
//CHECK-NEXT: abc
//CHECK-NEXT: undefined

  var values = s.values();
  while (true) {
    var e = values.next();
    print(e.value);
    if (e.done) break;
  }
//CHECK-NEXT: [object Object]
//CHECK-NEXT: 1
//CHECK-NEXT: null
//CHECK-NEXT: def
//CHECK-NEXT: undefined

  var entries = s.entries();
  while (true) {
    var e = entries.next();
    print(e.value);
    if (e.done) break;
  }
//CHECK-NEXT: 1,[object Object]
//CHECK-NEXT: [object Object],1
//CHECK-NEXT: ,
//CHECK-NEXT: abc,def
//CHECK-NEXT: undefined
}

function testForEach() {
  function callbackFn(value, key, s) {
    print(key, value, s.size);
  }
  s = new Map();
  s.set(1, []);
  s.set({}, -0);
  s.set(undefined, undefined);
  s.set("abc", "");

  print("testForEach", Map.prototype.forEach.length);
//CHECK-LABEL: testForEach 1
  s.forEach(callbackFn);
//CHECK-NEXT: 1  4
//CHECK-NEXT: [object Object] 0 4
//CHECK-NEXT: undefined undefined 4
//CHECK-NEXT: abc  4
}

function testZero() {
  print("testZero");
//CHECK-LABEL: testZero

  // -0 and +0 are normalized to +0
  var m = new Map();
  m.set(-0, 42);
  print(m.has(-0));
  print(m.has(+0));
  print(m.get(-0));
  print(m.get(+0));
  print(Object.is(-0, [...m.keys()][0]));
  print(Object.is(+0, [...m.keys()][0]));
  print(m.delete(+0))
  print(m.size === 0)
//CHECK-NEXT: true
//CHECK-NEXT: true
//CHECK-NEXT: 42
//CHECK-NEXT: 42
//CHECK-NEXT: false
//CHECK-NEXT: true
//CHECK-NEXT: true
//CHECK-NEXT: true
  m.set(+0, 43);
  print(m.has(-0));
  print(m.has(+0));
  print(m.get(-0));
  print(m.get(+0));
  print(Object.is(-0, [...m.keys()][0]));
  print(Object.is(+0, [...m.keys()][0]));
  print(m.delete(-0))
  print(m.size === 0)
//CHECK-NEXT: true
//CHECK-NEXT: true
//CHECK-NEXT: 43
//CHECK-NEXT: 43
//CHECK-NEXT: false
//CHECK-NEXT: true
//CHECK-NEXT: true
//CHECK-NEXT: true

  // Only key should be normalized.
  var m = new Map();
  m.set(-0, -0);
  var [k, v] = m.entries().next().value;
  print(Object.is(-0, k));
  print(Object.is(-0, v));
//CHECK-NEXT: false
//CHECK-NEXT: true
}

var o1 = {};
var o2 = {a: 1};
var o3 = {a: 1, b: 2};
var s = testSet(o1, o2, o3);
testHas(s, o1, o2, o3);
testGet(s, o1, o2, o3);
testDelete(s, o1, o2, o3);
testClear(o1, o2, o3);
testIteration();
testForEach();
testZero();


print("Map.groupBy");
//CHECK-LABEL: Map.groupBy

var map = Map.groupBy([1, 2, 3, 4, 5, 6, 7], function(key) { return key % 2; });
print(map.get('0'))
//CHECK-NEXT: 2,4,6
print(map.get('1'))
//CHECK-NEXT: 1,3,5,7
print(map.size);
//CHECK-NEXT: 2

var map = Map.groupBy([1, 2, 3, 4, 5, 6, 7], function(key) { return key % 3; });
print(map.get('0'))
//CHECK-NEXT: 3,6
print(map.get('1'))
//CHECK-NEXT: 1,4,7
print(map.get('2'))
//CHECK-NEXT: 2,5
print(map.size);
//CHECK-NEXT: 3

var map = Map.groupBy([1, 2, 3, 4, 5, 6, 7], function(key) { return key % 1; });
print(map.get('0'))
//CHECK-NEXT: 1,2,3,4,5,6,7
print(map.size);
//CHECK-NEXT: 1

var map = Map.groupBy([1, 2, 3, 4], function(key) { return key; });
print(map.size);
//CHECK-NEXT: 4

var team = [
  {name: 'Peter', age: 15},
  {name: 'Mike', age: 20},
  {name: 'John', age: 22},
];

var map = Map.groupBy(team, function(p) { return p.age < 18 ? 'underage' : 'adult'; });
print(map.size);
//CHECK-NEXT: 2
print(JSON.stringify(map.get('underage')))
//CHECK-NEXT: [{"name":"Peter","age":15}]
print(JSON.stringify(map.get('adult')))
//CHECK-NEXT: [{"name":"Mike","age":20},{"name":"John","age":22}]