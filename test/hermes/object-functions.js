/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -non-strict -O -target=HBC %s | %FileCheck --match-full-lines %s
// RUN: %hermes -non-strict -O -target=HBC -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s

print('is');
// CHECK-LABEL: is
print(Object.is.length);
// CHECK-NEXT: 2
print(Object.is({}, {}));
// CHECK-NEXT: false
print(Object.is(NaN, NaN));
// CHECK-NEXT: true
print(Object.is(-0, +0));
// CHECK-NEXT: false
var obj = {};
print(Object.is(obj, obj));
// CHECK-NEXT: true

print('seal');
// CHECK-LABEL: seal
var obj = Object();
obj.x = 1;
print(Object.isSealed(obj));
// CHECK: false
var sealed = Object.seal(obj);
obj.x = 2;
obj.y = 2;
print(sealed === obj, Object.isFrozen(obj), Object.isSealed(obj));
// CHECK: true false true
print(obj.x, obj.y);
// CHECK: 2 undefined

print('freeze');
// CHECK-LABEL: freeze
var obj = Object();
obj.x = 1;
print(Object.isFrozen(obj));
// CHECK: false
var frozen = Object.freeze(obj);
obj.x = 2;
obj.y = 2;
print(frozen === obj, Object.isFrozen(obj), Object.isSealed(obj));
// CHECK: true true true
print(obj.x, obj.y);
// CHECK: 1 undefined

print('preventExtensions');
// CHECK-LABEL: preventExtensions
var obj = Object();
obj.x = 1;
print(Object.isExtensible(obj));
// CHECK: true
var prevented = Object.preventExtensions(obj);
obj.x = 2;
obj.y = 2;
print(prevented === obj, Object.isExtensible(obj));
// CHECK: true false
print(obj.x, obj.y);
// CHECK: 2 undefined

print('defineProperty');
var obj = {};
var getFunc = function () { return 1001; }
Object.defineProperty(obj, "prop", {
  get: getFunc,
  configurable: true,
});
print(obj.prop);
//CHECK: 1001
Object.defineProperty(obj, "prop", {
  get: undefined,
});
print(obj.prop);
//CHECK: undefined
obj = { get x() { return 87; } };
Object.defineProperty(obj, "x", { writable: false });
print(obj.x);
//CHECK: undefined

print('getOwnPropertyDescriptor');
// CHECK-LABEL: getOwnPropertyDescriptor
var obj = Object();
obj.x = 'asdf';
var desc = Object.getOwnPropertyDescriptor(obj, 'x');
print(desc.value, desc.writable, desc.enumerable, desc.configurable);
// CHECK: asdf true true true

print('getOwnPropertyDescriptors');
// CHECK-LABEL: getOwnPropertyDescriptors
var obj = { x: 'a' };
obj[2] = 'b';
Object.defineProperty(obj, 'z', {
  value: 'c',
  enumerable: false,
});
var sym = Symbol();
Object.defineProperty(obj, sym, {
  value: 'd',
  enumerable: true,
});
var descs = Object.getOwnPropertyDescriptors(obj);
print(descs.x.value, descs.x.enumerable);
print(descs[2].value, descs[2].enumerable);
print(descs.z.value, descs.z.enumerable);
print(descs[sym].value, descs[sym].enumerable);
// CHECK-NEXT: a true
// CHECK-NEXT: b true
// CHECK-NEXT: c false
// CHECK-NEXT: d true

var acc = Object();
acc.get = function() {
  print('getter called');
};
acc.set = function() {
  print('setter called');
};
Object.defineProperty(obj, 'x', acc);
var desc = Object.getOwnPropertyDescriptor(obj, 'x');
print(typeof desc.get, typeof desc.set, desc.enumerable, desc.configurable);
// CHECK: function function true true
desc.get();
// CHECK: getter called
desc.set();
// CHECK: setter called

print('hasOwnProperty');
// CHECK-LABEL: hasOwnProperty
var obj = new Object();
obj.prop = 'exists';

function changeO() {
  obj.newprop = obj.prop;
  delete obj.prop;
}

print(obj.hasOwnProperty('prop'));
//CHECK: true
changeO();
print(obj.hasOwnProperty('prop'));
//CHECK: false
print(obj.hasOwnProperty('newprop'));
//CHECK: true
print(obj.hasOwnProperty('new' + 'prop'));
//CHECK: true
obj[5] = 'exists';
print(obj.hasOwnProperty(5));
//CHECK: true
print([1, 2].hasOwnProperty(0));
//CHECK: true
print([1, 2].hasOwnProperty(2));
//CHECK: false

var child = Object.create(obj);
print(child.hasOwnProperty('newprop'));
//CHECK: false
print(child.hasOwnProperty(5));
//CHECK: false

var key = {
  [Symbol.toPrimitive]() {
    throw 'to_prim';
  }
};
try {
  Object.prototype.hasOwnProperty.call(undefined, key);
} catch (e) {
  print(e);
}
//CHECK: to_prim

print('hasOwn');
// CHECK-LABEL: hasOwn
var obj = new Object();
obj.prop = 'exists';

function changeO() {
  obj.newprop = obj.prop;
  delete obj.prop;
}

print(Object.hasOwn(obj, 'prop'));
//CHECK: true
changeO();
print(Object.hasOwn(obj, 'prop'));
//CHECK: false
print(Object.hasOwn(obj, 'newprop'));
//CHECK: true
print(Object.hasOwn(obj, 'new' + 'prop'));
//CHECK: true
obj[5] = 'exists';
print(Object.hasOwn(obj, 5));
//CHECK: true
print(Object.hasOwn([1, 2], 0));
//CHECK: true
print(Object.hasOwn([1, 2], 2));
//CHECK: false

var child = Object.create(obj);
print(Object.hasOwn(child, 'newprop'));
//CHECK: false
print(Object.hasOwn(child, 5));
//CHECK: false

print('defineProperties');
// CHECK-LABEL: defineProperties
var a = [];
Object.defineProperties(a, {"1": {value: 42}});
print(a[1]);
// CHECK-NEXT: 42
Object.preventExtensions(a);
try {Object.defineProperties(a, {"0": {value: 1}});}
catch(e) {print('caught', e.name, e.message);}
// CHECK-NEXT: caught TypeError {{.*}}

print('setPrototypeOf');
// CHECK-LABEL: setPrototypeOf
var obj = {};
var proto = {};
print(Object.getPrototypeOf(Object.setPrototypeOf(obj, proto)) === proto);
// CHECK-NEXT: true
print(Object.getPrototypeOf(Object.setPrototypeOf(obj, null)));
// CHECK-NEXT: null
print(Object.setPrototypeOf(true, {}));
// CHECK-NEXT: true
print(Object.setPrototypeOf(1, {}));
// CHECK-NEXT: 1
try { Object.setPrototypeOf(undefined, 1); } catch (e) { print(e.name) }
// CHECK-NEXT: TypeError
try { Object.setPrototypeOf({}, 1); } catch (e) { print(e.name) }
// CHECK-NEXT: TypeError

print('create');
// CHECK-LABEL: create
var x = Object.create(null);
print(Object.getPrototypeOf(x));
//CHECK-NEXT: null

print('values');
// CHECK-LABEL: values
print(Object.values({a: 1, b: 2}));
// CHECK-NEXT: 1,2
print(Object.values({}).length);
// CHECK-NEXT: 0
var x = {a: 1, c: 3};
Object.defineProperty(x, 'b', {enumerable: false, value: 2});
print(Object.values(x));
// CHECK-NEXT: 1,3

print('entries');
// CHECK-LABEL: entries
print(Object.entries({}).length);
// CHECK-NEXT: 0
var e = Object.entries({a: 1, b: 2});
print(e.length, e[0].length, e[1].length);
// CHECK-NEXT: 2 2 2
print(e[0], e[1])
// CHECK-NEXT: a,1 b,2
var x = {a: 1, c: 3};
Object.defineProperty(x, 'b', {enumerable: false, value: 2});
var e = Object.entries(x);
print(e.length)
// CHECK-NEXT: 2
print(e[0], e[1]);
// CHECK-NEXT: a,1 c,3

print('fromEntries');
// CHECK-LABEL: fromEntries
try {
  Object.fromEntries(null);
} catch(e) {
  print('caught', e.name);
}
// CHECK-NEXT: caught TypeError
var obj = Object.fromEntries([['a', 1], ['b', 2]]);
print(Object.entries(obj));
// CHECK-NEXT: a,1,b,2
var desc = Object.getOwnPropertyDescriptor(obj, 'a');
print(desc.enumerable, desc.configurable, desc.writable);
// CHECK-NEXT: true true true
function* gen(x) {
  yield ['a', x];
  yield {0: 'b', 1: x+10};
}
var obj = Object.fromEntries(gen(4));
print(Object.entries(obj));
// CHECK-NEXT: a,4,b,14
print(Object.entries(Object.fromEntries([])).length);
// CHECK-NEXT: 0

function testGetOwnPropertyNames() {
  var obj = {a: 0, b: 1};
  Object.defineProperty(obj, 'c', {value: 2, enumerable: false});
  obj[0] = 3;
  var set = new Set();
  set.add(obj);
  var names = Object.getOwnPropertyNames(obj)
  for (var p in names) {
    print(typeof names[p], names[p]);
  }
}
testGetOwnPropertyNames();
//CHECK-NEXT: string 0
//CHECK-NEXT: string a
//CHECK-NEXT: string b
//CHECK-NEXT: string c

function testKeys() {
  var obj = {a: 0};
  Object.defineProperty(obj, 'b', {value: 1, enumerable: false});
  obj.c = 2;
  var names = Object.keys(obj);
  for (var p in names) {
    print(names[p]);
  }
}
testKeys();
//CHECK-NEXT: a
//CHECK-NEXT: c

function testPropertyIsEnumerable() {
  var obj = {a: 0};
  Object.defineProperty(obj, 'b', {value: 1, enumerable: false});
  obj.c = 2;
  var names = Object.getOwnPropertyNames(obj);
  print(names.length);
  for (var p in names) {
    print(names[p], obj.propertyIsEnumerable(names[p]));
  }
}
testPropertyIsEnumerable();
//CHECK-NEXT: 3
//CHECK-NEXT: a true
//CHECK-NEXT: b false
//CHECK-NEXT: c true

function testIsPrototypeOf() {
  function Foo() {}
  function Bar() {}

  Bar.prototype = Object.create(Foo.prototype);
  var bar = new Bar();
  var obj = new Object();
  print(Bar.prototype.isPrototypeOf(bar));
  print(Foo.prototype.isPrototypeOf(bar));
  print(Object.prototype.isPrototypeOf(bar));
  print(Bar.prototype.isPrototypeOf(obj));
  print(Foo.prototype.isPrototypeOf(obj));
  print(Object.prototype.isPrototypeOf(obj));
}
testIsPrototypeOf();
//CHECK-NEXT: true
//CHECK-NEXT: true
//CHECK-NEXT: true
//CHECK-NEXT: false
//CHECK-NEXT: false
//CHECK-NEXT: true

print("Object.prototype.toLocaleString");
//CHECK-LABEL: Object.prototype.toLocaleString
print(Object.prototype.toLocaleString() === Object.prototype.toString());
//CHECK-NEXT: true
var obj = {};
Object.defineProperty(obj, "toString", {get: Object.prototype.toLocaleString});
try {
  print(obj);
} catch (e) {
  // Should be a stack overflow from infinite recursion.
  print(e.name);
}
//CHECK-NEXT: RangeError

var obj = {};
var proto = {};
obj.__proto__ = proto;

print('__defineGetter__');
// CHECK-LABEL: __defineGetter__
obj.__defineGetter__('a', function() {return 10;});
print(obj.a);
// CHECK-NEXT: 10
proto.__defineGetter__('b', function() {return 12;});
print(obj.b);
// CHECK-NEXT: 12
proto.__defineSetter__('d', function() {});

print('__lookupGetter__');
// CHECK-LABEL: __lookupGetter__
var getter = obj.__lookupGetter__('a');
print(getter());
// CHECK-NEXT: 10
var getter = obj.__lookupGetter__('b');
print(getter());
// CHECK-NEXT: 12
var getter = obj.__lookupGetter__('c');
print(getter);
// CHECK-NEXT: undefined
print(obj.__lookupGetter__('d'));
// CHECK-NEXT: undefined

var obj = {};
var proto = {};
obj.__proto__ = proto;

print('__defineSetter__');
// CHECK-LABEL: __defineSetter__
obj.__defineSetter__('a', function() {print(10);});
obj.a = 3;
// CHECK-NEXT: 10
proto.__defineSetter__('b', function() {print(12);});
obj.b = 4;
// CHECK-NEXT: 12
proto.__defineGetter__('d', function() {});

print('__lookupSetter__');
// CHECK-LABEL: __lookupSetter__
var setter = obj.__lookupSetter__('a');
setter();
// CHECK-NEXT: 10
var setter = obj.__lookupSetter__('b');
setter();
// CHECK-NEXT: 12
var setter = obj.__lookupSetter__('c');
print(setter);
// CHECK-NEXT: undefined
print(obj.__lookupSetter__('d'));
// CHECK-NEXT: undefined

print('accessors in prototypes');
// CHECK-LABEL: accessors in prototypes
var get = {get: function(){}};
var set = {set: function(){}};

var obj = {};
var proto = {};
Object.defineProperty(proto, 'a', get);
var obj = Object.create(proto, {a: set});
print(Object.create(obj).__lookupGetter__('a'));
// CHECK-NEXT: undefined

var obj = {};
var proto = {};
Object.defineProperty(proto, 'a', set);
var obj = Object.create(proto, {a: get});
print(Object.create(obj).__lookupSetter__('a'));
// CHECK-NEXT: undefined

print('Object.assign')
//CHECK-LABEL: Object.assign

function testObjectAssignSimple() {
  var obj = {a:0}
   // should just return obj
  print ("" + (Object.assign(obj) === obj));

  Object.assign(obj, {a:1, b:0}, {a:2, b:1});
  print (JSON.stringify(obj))
}

testObjectAssignSimple()
//CHECK-NEXT: true
//CHECK-NEXT: {"a":2,"b":1}

function testObjectAssign() {
    var obj1 = { a: 0 , b: { c: 0}};
    var obj2 = Object.assign({}, obj1);
    print(JSON.stringify(obj2)); // { a: 0, b: { c: 0}}

    obj1.a = 1;
    print(JSON.stringify(obj1)); // { a: 1, b: { c: 0}}
    print(JSON.stringify(obj2)); // { a: 0, b: { c: 0}}

    obj2.a = 2;
    print(JSON.stringify(obj1)); // { a: 1, b: { c: 0}}
    print(JSON.stringify(obj2)); // { a: 2, b: { c: 0}}

    obj2.b.c = 3;
    print(JSON.stringify(obj1)); // { a: 1, b: { c: 3}}
    print(JSON.stringify(obj2)); // { a: 2, b: { c: 3}}

    // Deep Clone
    obj1 = { a: 0 , b: { c: 0}};
    var obj3 = JSON.parse(JSON.stringify(obj1));
    obj1.a = 4;
    obj1.b.c = 4;
    print(JSON.stringify(obj3)); // { a: 0, b: { c: 0}}
}

testObjectAssign();
//CHECK-NEXT: {"a":0,"b":{"c":0}}
//CHECK-NEXT: {"a":1,"b":{"c":0}}
//CHECK-NEXT: {"a":0,"b":{"c":0}}
//CHECK-NEXT: {"a":1,"b":{"c":0}}
//CHECK-NEXT: {"a":2,"b":{"c":0}}
//CHECK-NEXT: {"a":1,"b":{"c":3}}
//CHECK-NEXT: {"a":2,"b":{"c":3}}
//CHECK-NEXT: {"a":0,"b":{"c":0}}

function testObjectAssignPrimitives() {
  var str = "grendel"; //should be exploded
  var bool = true; //wrapped with no enum props
  var num = 42; // same ^

  var obj = Object.assign({},str, undefined, bool, num, null);
  print (JSON.stringify(obj));
}

testObjectAssignPrimitives()
//CHECK-NEXT: {"0":"g","1":"r","2":"e","3":"n","4":"d","5":"e","6":"l"}

function testObjectSymbolKeys() {
  var obj = Object.assign({}, {[Symbol.for('akey')]:'avalue'});
  // Define a props object with a single non-enumerable property whose
  // key is a symbol and value is not a valid properties object.
  var props = Object.create({}, {
    [Symbol.for('bkey')]: {
      value: "bvalue",
      writable: false,
      enumerable: false,
      configurable: true,
    },
  });
  // Should include non-enumerable symbol key
  print (Object.getOwnPropertySymbols(props).length)
  // Now, assign those properties to obj.  This should be a noop, as there
  // are no enumerable properties in props.
  Object.defineProperties(obj, props);
  print (Object.getOwnPropertySymbols(obj).length);
}
testObjectSymbolKeys()
//CHECK-NEXT: 1
//CHECK-NEXT: 1

function testObjectAssignExceptions() {
    var obj = Object.defineProperty({}, 'whytho', {
        get: function() { return 23; },
        set: function(newValue) { throw "nope" },
    });

    // trying to assign to whytho should throw and never get to last object
    try {
        Object.assign(obj, {iamsafe: 10}, {whytho: 34}, {never: 100});
    } catch (e) {
        print ("EX: " + e)
    }

    // should only contain iamsafe
    print (JSON.stringify(obj))
}

testObjectAssignExceptions()
//CHECK-NEXT: EX: nope
//CHECK-NEXT: {"iamsafe":10}

function testObjectAssignModifications() {
    // corner case where getter modifies object.
    var obj1 = Object.defineProperty({}, 'a', {
        get: function() {delete obj1.b; return 10},
        set: function(newValue) {},
        enumerable: true
    });

    obj1.b = 23;
    obj1.c = 32;

    print (JSON.stringify(Object.assign({}, obj1)))
}
testObjectAssignModifications()
//CHECK-NEXT: {"a":10,"c":32}

print('groupBy');
// CHECK-LABEL: groupBy

var obj = Object.groupBy([1, 2, 3, 4, 5, 6, 7], function(key) { return key % 2; });
print(JSON.stringify(obj));
//CHECK-NEXT: {"0":[2,4,6],"1":[1,3,5,7]}

var obj = Object.groupBy([1, 2, 3, 4, 5, 6, 7], function(key) { return key % 3; });
print(JSON.stringify(obj));
//CHECK-NEXT: {"0":[3,6],"1":[1,4,7],"2":[2,5]}

var obj = Object.groupBy([1, 2, 3, 4, 5, 6, 7], function(key) { return key % 1; });
print(JSON.stringify(obj));
//CHECK-NEXT: {"0":[1,2,3,4,5,6,7]}

var obj = Object.groupBy([1, 2, 3, 4], function(key) { return key; });
print(JSON.stringify(obj));
//CHECK-NEXT: {"1":[1],"2":[2],"3":[3],"4":[4]}

var team = [
    {name: 'Peter', age: 15},
    {name: 'Mike', age: 20},
    {name: 'John', age: 22},
];

var obj = Object.groupBy(team, function(p) { return p.age < 18 ? 'underage' : 'adult'; });
print(JSON.stringify(obj));
//CHECK-NEXT: {"underage":[{"name":"Peter","age":15}],"adult":[{"name":"Mike","age":20},{"name":"John","age":22}]}
