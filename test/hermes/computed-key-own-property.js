/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// obj[key], hasOwnProperty and Object.hasOwn with a key that is a uniqued
// string (a key from Object.keys or for-in) look the name up in the
// object's hidden class before anything else. Every case below must give
// what the slow path gives; the expected output is that of the slow path.
// Each case runs with uniqued keys and again with the same names built at
// run time, which are not uniqued and always take the slow path.

'use strict';

var hasOwnProperty = Object.prototype.hasOwnProperty;

/// \return the keys of \p seed, which are uniqued strings.
function uniquedKeys(seed) {
  return Object.keys(seed);
}

/// \return the same names as strings built at run time, which are not
/// uniqued.
function builtKeys(keys) {
  var res = [];
  for (var i = 0; i < keys.length; ++i)
    res.push(keys[i].slice(0, 1) + keys[i].slice(1));
  return res;
}

/// \return a short description of the value \p v.
function describe(v) {
  if (typeof v === 'function')
    return 'fn';
  if (typeof v === 'object' && v !== null)
    return 'obj';
  return String(v);
}

/// Describe the result of get, hasOwnProperty and Object.hasOwn of every
/// key in \p keys on \p obj.
function probe(obj, keys) {
  var res = [];
  for (var i = 0; i < keys.length; ++i) {
    var k = keys[i];
    var v = obj[k];
    res.push(
        k + ':' + describe(v) + ':' +
        hasOwnProperty.call(obj, k) + ':' + Object.hasOwn(obj, k));
  }
  return res.join(' ');
}

/// Print \p label and the probes of \p make() with uniqued and with built
/// keys, repeated so that a compiled function runs them too.
function check(label, make, keys) {
  var built = builtKeys(keys);
  var u, b;
  for (var i = 0; i < 50; ++i) {
    u = probe(make(), keys);
    b = probe(make(), built);
  }
  print(label);
  print(' ', u);
  print(' ', u === b ? 'built: same' : 'built: ' + b);
}

var K = uniquedKeys({a: 0, b: 0, zz: 0, length: 0, name: 0, prototype: 0,
                     0: 0, 1: 0, 7: 0, toString: 0})
            .concat(uniquedKeys(JSON.parse('{"__proto__": 0}')));
print(K.join(','));
var kA = K[K.indexOf('a')];
var kZz = K[K.indexOf('zz')];
var kLength = K[K.indexOf('length')];

check('plain object', function() {
  return {a: 1, b: 2};
}, K);

check('accessor', function() {
  var o = {b: 2};
  Object.defineProperty(o, 'a', {get: function() { return 'got'; },
                                 enumerable: true});
  return o;
}, K);

check('property on the prototype', function() {
  return Object.create({a: 'proto', zz: 'protozz'}, {b: {value: 2}});
}, K);

check('null prototype', function() {
  var o = Object.create(null);
  o.a = 1;
  o['__proto__'] = 'own';
  return o;
}, K);

check('after delete (dictionary)', function() {
  var o = {a: 1, b: 2, zz: 3};
  delete o.b;
  return o;
}, K);

check('more than 64 properties (dictionary)', function() {
  var o = {};
  for (var i = 0; i < 70; ++i)
    o['p' + i] = i;
  o.zz = 'last';
  return o;
}, K);

check('index-like named property', function() {
  return {a: 1, 7: 'seven', 0: 'zero'};
}, K);

check('array', function() {
  var arr = ['e0', 'e1'];
  arr.a = 'named';
  return arr;
}, K);

check('frozen array', function() {
  return Object.freeze(['f0']);
}, K);

check('String object', function() {
  var s = new String('xy');
  s.b = 'named';
  return s;
}, K);

check('arguments', function() {
  return (function() { return arguments; })('arg0', 'arg1');
}, K);

check('typed array', function() {
  return new Uint8Array([5, 6]);
}, K);

check('function not used as an object before', function() {
  return function fresh(p, q) {};
}, K);

check('bound function', function() {
  return (function target(p) {}).bind(null);
}, K);

check('frozen object', function() {
  return Object.freeze({a: 1, zz: 2});
}, K);

check('redefined as accessor after use', function() {
  var o = {a: 1, b: 2};
  var before = o[kA] + ':' + hasOwnProperty.call(o, kA);
  Object.defineProperty(o, 'a', {get: function() { return before; }});
  return o;
}, K);

check('number receiver', function() { return 5; }, K);
check('string receiver', function() { return 'xyz'; }, K);
check('boolean receiver', function() { return true; }, K);

// A proxy: every operation must reach the traps, every time.
(function() {
  var log = {get: 0, has: 0, desc: 0};
  var target = {a: 1};
  var p = new Proxy(target, {
    get: function(t, k, r) { ++log.get; return Reflect.get(t, k, r); },
    has: function(t, k) { ++log.has; return Reflect.has(t, k); },
    getOwnPropertyDescriptor: function(t, k) {
      ++log.desc;
      return Reflect.getOwnPropertyDescriptor(t, k);
    },
  });
  var res;
  for (var i = 0; i < 50; ++i)
    res = probe(p, K);
  print('proxy');
  print(' ', res);
  print(' ', JSON.stringify(log));
  // A proxy on the prototype chain is reached only for names the object
  // lacks.
  log = {get: 0, has: 0, desc: 0};
  var child = Object.create(p);
  child.b = 2;
  for (var i = 0; i < 50; ++i)
    res = probe(child, K);
  print('proxy as prototype');
  print(' ', res);
  print(' ', JSON.stringify(log));
})();

// Keys that are not strings.
(function() {
  var sym = Symbol('s');
  var o = {a: 1, 1: 'one'};
  o[sym] = 'symval';
  var keyObj = {toString: function() { return 'a'; }};
  var res = [];
  for (var i = 0; i < 50; ++i) {
    res = [
      o[1], hasOwnProperty.call(o, 1), Object.hasOwn(o, 1),
      o[1.5], hasOwnProperty.call(o, 1.5),
      o[sym], hasOwnProperty.call(o, sym), Object.hasOwn(o, sym),
      o[keyObj], hasOwnProperty.call(o, keyObj), Object.hasOwn(o, keyObj),
      o[new String('a')], hasOwnProperty.call(o, new String('a')),
      o[null], hasOwnProperty.call(o, undefined),
    ];
  }
  print('non-string keys');
  print(' ', res.join(' '));
})();

// A class that changes under the same key: the answer follows the object.
(function() {
  var k = kA;
  var res = [];
  for (var i = 0; i < 50; ++i) {
    var o = {};
    res = [hasOwnProperty.call(o, k), o[k]];
    o.a = i;
    res.push(hasOwnProperty.call(o, k), o[k]);
    delete o.a;
    res.push(hasOwnProperty.call(o, k), o[k]);
    o.a = 'again';
    res.push(hasOwnProperty.call(o, k), o[k]);
    Object.setPrototypeOf(o, {zz: 'inherited'});
    res.push(hasOwnProperty.call(o, kZz), o[kZz]);
  }
  print('changing object');
  print(' ', res.join(' '));
})();

// hasOwnProperty with a receiver that is not an object.
(function() {
  var res = [];
  res.push(hasOwnProperty.call('xyz', kLength), hasOwnProperty.call(5, kA));
  try {
    hasOwnProperty.call(undefined, kA);
    res.push('no throw');
  } catch (e) {
    res.push(e.constructor.name);
  }
  try {
    Object.hasOwn(null, kA);
    res.push('no throw');
  } catch (e) {
    res.push(e.constructor.name);
  }
  print('receivers that are not objects');
  print(' ', res.join(' '));
})();

// A function whose properties have not been created yet: the first
// question asked of it is hasOwnProperty, with no get before it.
(function() {
  var kName = K[K.indexOf('name')];
  var kPrototype = K[K.indexOf('prototype')];
  var res = [];
  for (var i = 0; i < 50; ++i) {
    res = [
      hasOwnProperty.call(function f1(p) {}, kName),
      hasOwnProperty.call(function f2(p) {}, kLength),
      hasOwnProperty.call(function f3(p) {}, kPrototype),
      hasOwnProperty.call(function f4(p) {}, kZz),
      Object.hasOwn(function f5(p) {}, kName),
      Object.hasOwn(function f6(p) {}, kZz),
    ];
  }
  print('fresh functions');
  print(' ', res.join(' '));
})();

// The output of the slow path.
// CHECK: 0,1,7,a,b,zz,length,name,prototype,toString,__proto__
// CHECK-NEXT: plain object
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:1:true:true b:2:true:true zz:undefined:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: accessor
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:got:true:true b:2:true:true zz:undefined:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: property on the prototype
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:proto:false:false b:2:true:true zz:protozz:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: null prototype
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:1:true:true b:undefined:false:false zz:undefined:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:undefined:false:false __proto__:own:true:true
// CHECK-NEXT: built: same
// CHECK-NEXT: after delete (dictionary)
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:1:true:true b:undefined:false:false zz:3:true:true length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: more than 64 properties (dictionary)
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:undefined:false:false b:undefined:false:false zz:last:true:true length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: index-like named property
// CHECK-NEXT: 0:zero:true:true 1:undefined:false:false 7:seven:true:true a:1:true:true b:undefined:false:false zz:undefined:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: array
// CHECK-NEXT: 0:e0:true:true 1:e1:true:true 7:undefined:false:false a:named:true:true b:undefined:false:false zz:undefined:false:false length:2:true:true name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: frozen array
// CHECK-NEXT: 0:f0:true:true 1:undefined:false:false 7:undefined:false:false a:undefined:false:false b:undefined:false:false zz:undefined:false:false length:1:true:true name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: String object
// CHECK-NEXT: 0:x:true:true 1:y:true:true 7:undefined:false:false a:undefined:false:false b:named:true:true zz:undefined:false:false length:2:true:true name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: arguments
// CHECK-NEXT: 0:arg0:true:true 1:arg1:true:true 7:undefined:false:false a:undefined:false:false b:undefined:false:false zz:undefined:false:false length:2:true:true name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: typed array
// CHECK-NEXT: 0:5:true:true 1:6:true:true 7:undefined:false:false a:undefined:false:false b:undefined:false:false zz:undefined:false:false length:2:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: function not used as an object before
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:undefined:false:false b:undefined:false:false zz:undefined:false:false length:2:true:true name:fresh:true:true prototype:obj:true:true toString:fn:false:false __proto__:fn:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: bound function
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:undefined:false:false b:undefined:false:false zz:undefined:false:false length:1:true:true name:bound target:true:true prototype:undefined:false:false toString:fn:false:false __proto__:fn:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: frozen object
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:1:true:true b:undefined:false:false zz:2:true:true length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: redefined as accessor after use
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:1:true:true:true b:2:true:true zz:undefined:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: number receiver
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:undefined:false:false b:undefined:false:false zz:undefined:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: string receiver
// CHECK-NEXT: 0:x:true:true 1:y:true:true 7:undefined:false:false a:undefined:false:false b:undefined:false:false zz:undefined:false:false length:3:true:true name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: boolean receiver
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:undefined:false:false b:undefined:false:false zz:undefined:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: built: same
// CHECK-NEXT: proxy
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:1:true:true b:undefined:false:false zz:undefined:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: {"get":550,"has":0,"desc":1100}
// CHECK-NEXT: proxy as prototype
// CHECK-NEXT: 0:undefined:false:false 1:undefined:false:false 7:undefined:false:false a:1:false:false b:2:true:true zz:undefined:false:false length:undefined:false:false name:undefined:false:false prototype:undefined:false:false toString:fn:false:false __proto__:obj:false:false
// CHECK-NEXT: {"get":500,"has":0,"desc":0}
// CHECK-NEXT: non-string keys
// CHECK-NEXT: one true true  false symval true true 1 true true 1 true  false
// CHECK-NEXT: changing object
// CHECK-NEXT: false  true 49 false  true again false inherited
// CHECK-NEXT: receivers that are not objects
// CHECK-NEXT: true false TypeError TypeError
// CHECK-NEXT: fresh functions
// CHECK-NEXT: true true true false true false
