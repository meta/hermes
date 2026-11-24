/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// Computed property gets, puts and hasOwnProperty with string keys, in loops
// long enough for the computed property cache to be filled and then hit, in
// the interpreter and under the JIT. The output is that of the generic path.
//
// The cache is used only for uniqued strings, and a string built at run time
// is not uniqued by a property access. So every section takes its keys from
// Object.keys() of a seed object kept alive for the whole run, or from
// for-in, and reuses them across fresh receivers. Built strings appear only
// where a section says so. Values read back through the cache are checked
// exactly, so that a hit on the wrong slot or class shows in the output.

'use strict';

var N = 300;
var hasOwnProperty = Object.prototype.hasOwnProperty;

/// The seed objects whose keys the sections use, kept alive for the whole run.
var seeds = [];

/// \return Object.keys(\p seed), and keep \p seed alive.
function seedKeys(seed) {
  seeds.push(seed);
  return Object.keys(seed);
}

/// Call \p f and return 'ok', or the constructor name of what it threw.
function errName(f) {
  try {
    f();
    return 'ok';
  } catch (e) {
    return e.constructor.name;
  }
}

/// Count the occurrences of each string in \p arr; return "s:count ..." in
/// order of first occurrence. A Map, so that no property access is involved.
function tally(arr) {
  var counts = new Map();
  for (var i = 0; i < arr.length; ++i)
    counts.set(arr[i], (counts.get(arr[i]) || 0) + 1);
  var parts = [];
  counts.forEach(function (n, s) {
    parts.push(s + ':' + n);
  });
  return parts.join(' ');
}

// 1. The prop-copy loop of React's jsxProd.

print('jsxProd');
// CHECK-LABEL: jsxProd

function jsxProd(config) {
  var props = {};
  var propName;
  for (propName in config) {
    if (
      hasOwnProperty.call(config, propName) &&
      propName !== 'key' &&
      propName !== 'ref'
    ) {
      props[propName] = config[propName];
    }
  }
  return props;
}

(function () {
  var inherited = {inheritedProp: 'no'};
  var count = 0;
  var sum = 0;
  var last1, last2, last3;
  for (var i = 0; i < N; ++i) {
    var c1 = {className: 'c' + i, style: i, children: i * 2};
    var c2 = {key: 'k' + i, id: i, ref: null, title: 't'};
    var c3 = Object.create(inherited);
    c3.onClick = i;
    c3.disabled = i % 2 === 0;
    last1 = jsxProd(c1);
    last2 = jsxProd(c2);
    last3 = jsxProd(c3);
    count +=
      Object.keys(last1).length +
      Object.keys(last2).length +
      Object.keys(last3).length;
    sum += last1.style + last1.children + last2.id + last3.onClick;
  }
  print(count + ' ' + sum);
  print(JSON.stringify(last1));
  print(JSON.stringify(last2));
  print(JSON.stringify(last3));
})();
// CHECK-NEXT: 2100 224250
// CHECK-NEXT: {"className":"c299","style":299,"children":598}
// CHECK-NEXT: {"id":299,"title":"t"}
// CHECK-NEXT: {"onClick":299,"disabled":false}

// 2. Get, put, hasOwnProperty and Object.hasOwn on own data properties, with
// uniqued keys and then with the same names built at run time.

print('own data properties');
// CHECK-LABEL: own data properties

(function () {
  var K = seedKeys({alpha: 0, beta: 0, gamma: 0, delta: 0, absent: 0});
  var absent = K.pop();
  // Not uniqued: the generic path.
  var B = K.map(function (k) {
    return k.slice(0, 1) + k.slice(1);
  });
  var absentB = absent.slice(0, 1) + absent.slice(1);

  function run(label, keys, absentKey) {
    var sum = 0;
    var has = 0;
    var hasOwn = 0;
    var missing = 0;
    var last;
    for (var i = 0; i < N; ++i) {
      var o = {alpha: i, beta: i + 1, gamma: i + 2, delta: i + 3};
      for (var j = 0; j < keys.length; ++j) {
        var k = keys[j];
        sum += o[k];
        o[k] = o[k] * 2 + j;
        if (hasOwnProperty.call(o, k)) ++has;
        if (Object.hasOwn(o, k)) ++hasOwn;
      }
      sum += o.alpha + o.beta + o.gamma + o.delta;
      if (o[absentKey] === undefined) ++missing;
      if (!hasOwnProperty.call(o, absentKey)) ++missing;
      if (!Object.hasOwn(o, absentKey)) ++missing;
      last = o;
    }
    print(
      label + ': ' + sum + ' ' + has + ' ' + hasOwn + ' ' + missing + ' ' +
        JSON.stringify(last),
    );
  }

  run('uniqued', K, absent);
  run('built', B, absentB);
})();
// CHECK-NEXT: uniqued: 545400 1200 1200 900 {"alpha":598,"beta":601,"gamma":604,"delta":607}
// CHECK-NEXT: built: 545400 1200 1200 900 {"alpha":598,"beta":601,"gamma":604,"delta":607}

// 3. Puts that add 70 properties to fresh objects, crossing the dictionary
// threshold. Values are read back after each add, while the class is not yet
// a dictionary, and all of them at the end.

print('add 70');
// CHECK-LABEL: add 70

(function () {
  var seed = {};
  for (var i = 0; i < 70; ++i) seed['p' + i] = i;
  var K = seedKeys(seed);
  var badEarly = 0;
  var badLate = 0;
  var sum = 0;
  var last;
  for (var i = 0; i < N; ++i) {
    var r = {};
    for (var j = 0; j < K.length; ++j) {
      r[K[j]] = i * 100 + j;
      if (r[K[j]] !== i * 100 + j) ++badEarly;
      if (r[K[j >> 1]] !== i * 100 + (j >> 1)) ++badEarly;
    }
    for (var j = 0; j < K.length; ++j) {
      var v = r[K[j]];
      if (v !== i * 100 + j) ++badLate;
      if (!hasOwnProperty.call(r, K[j])) ++badLate;
      sum += v;
    }
    last = r;
  }
  var lastKeys = Object.keys(last);
  print(K.length + ' ' + badEarly + ' ' + badLate + ' ' + sum + ' ' +
    lastKeys.length);
  print(lastKeys[0] + ' ' + lastKeys[63] + ' ' + lastKeys[64] + ' ' +
    lastKeys[69]);
  print(last.p0 + ' ' + last.p5 + ' ' + last.p6 + ' ' + last.p63 + ' ' +
    last.p64 + ' ' + last.p69);
})();
// CHECK-NEXT: 70 0 0 314674500 70
// CHECK-NEXT: p0 p63 p64 p69
// CHECK-NEXT: 29900 29905 29906 29963 29964 29969

// 4. After hits, each change below alters the outcome and must be seen.

print('changes after hits');
// CHECK-LABEL: changes after hits

(function () {
  var K = seedKeys({sa: 0, x: 0, y: 0, z: 0});
  var k = K[0];
  var kx = K[1];
  var ky = K[2];
  var kz = K[3];

  // A setter defined on the prototype.
  var proto = {};
  var added = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(proto);
    o[k] = i;
    if (Object.hasOwn(o, k) && o[k] === i) ++added;
  }
  var calls = 0;
  var own = 0;
  Object.defineProperty(proto, k, {
    set: function (v) {
      calls += v;
    },
    configurable: true,
  });
  for (var i = 0; i < N; ++i) {
    var o = Object.create(proto);
    o[k] = 1;
    if (Object.hasOwn(o, k)) ++own;
  }
  print('setter on proto: ' + added + ' ' + calls + ' ' + own);

  // A read-only property defined on the prototype.
  var proto2 = {};
  added = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(proto2);
    o[k] = i;
    if (Object.hasOwn(o, k) && o[k] === i) ++added;
  }
  Object.defineProperty(proto2, k, {
    value: 'ro',
    writable: false,
    configurable: true,
  });
  var errs = [];
  own = 0;
  var reads = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(proto2);
    errs.push(
      errName(function () {
        o[k] = 1;
      }),
    );
    if (Object.hasOwn(o, k)) ++own;
    if (o[k] === 'ro') ++reads;
  }
  print('read-only on proto: ' + added + ' ' + tally(errs) + ' ' + own +
    ' ' + reads);

  // The prototype's prototype replaced by one with a setter.
  var base = {};
  var mid = Object.create(base);
  added = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(mid);
    o[k] = i;
    if (Object.hasOwn(o, k) && o[k] === i) ++added;
  }
  var withSetter = {};
  calls = 0;
  Object.defineProperty(withSetter, k, {
    set: function (v) {
      calls += v;
    },
  });
  Object.setPrototypeOf(mid, withSetter);
  own = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(mid);
    o[k] = 2;
    if (Object.hasOwn(o, k)) ++own;
  }
  print('setter on proto proto: ' + added + ' ' + calls + ' ' + own);

  // Warm up the class {x}: a store to x and an add of y, read back.
  function warmX() {
    var bad = 0;
    for (var i = 0; i < N; ++i) {
      var o = {x: i};
      o[kx] = i + 1;
      o[ky] = -i;
      if (o[kx] !== i + 1 || o[ky] !== -i || o.y !== -i) ++bad;
    }
    return bad;
  }

  // Object.preventExtensions on the receiver.
  var warmBad = warmX();
  errs = [];
  var stored = 0;
  own = 0;
  for (var i = 0; i < N; ++i) {
    var o = {x: i};
    Object.preventExtensions(o);
    o[kx] = i + 1;
    if (o[kx] === i + 1) ++stored;
    errs.push(
      errName(function () {
        o[ky] = 1;
      }),
    );
    if (Object.hasOwn(o, ky)) ++own;
  }
  print('preventExtensions: ' + warmBad + ' ' + stored + ' ' + tally(errs) +
    ' ' + own);

  // Object.freeze on the receiver.
  warmBad = warmX();
  errs = [];
  var unchanged = 0;
  own = 0;
  for (var i = 0; i < N; ++i) {
    var o = {x: i};
    Object.freeze(o);
    errs.push(
      errName(function () {
        o[kx] = -1;
      }),
    );
    errs.push(
      errName(function () {
        o[ky] = -1;
      }),
    );
    if (o[kx] === i) ++unchanged;
    if (Object.hasOwn(o, ky)) ++own;
  }
  print('freeze: ' + warmBad + ' ' + tally(errs) + ' ' + unchanged + ' ' +
    own);

  // Object.defineProperty making an own property read-only, and turning
  // one into an accessor.
  errs = [];
  unchanged = 0;
  var bad = 0;
  var gets = 0;
  var getterValues = 0;
  for (var i = 0; i < N; ++i) {
    var o = {x: i, y: -i};
    o[kx] = i + 1;
    o[ky] = o[kx] * 2 + o[ky];
    if (o.y !== i + 2 || o.x !== i + 1) ++bad;
    Object.defineProperty(o, kx, {writable: false});
    errs.push(
      errName(function () {
        o[kx] = -1;
      }),
    );
    if (o[kx] === i + 1) ++unchanged;
    Object.defineProperty(o, ky, {
      get: function () {
        ++gets;
        return 42;
      },
      configurable: true,
    });
    getterValues += o[ky];
    errs.push(
      errName(function () {
        o[ky] = -1;
      }),
    );
  }
  print('defineProperty: ' + bad + ' ' + tally(errs) + ' ' + unchanged + ' ' +
    gets + ' ' + getterValues);

  // delete, then add again.
  var sum = 0;
  var orders = [];
  for (var i = 0; i < N; ++i) {
    var o = {x: i, y: i + 1, z: i + 2};
    sum += o[kx] + o[ky] + o[kz];
    delete o[ky];
    if (o[ky] !== undefined || Object.hasOwn(o, ky)) sum = NaN;
    o[ky] = i + 10;
    sum += o[kx] + o[ky] + o[kz];
    orders.push(Object.keys(o).join());
  }
  print('delete and add: ' + sum + ' ' + tally(orders));
})();
// CHECK-NEXT: setter on proto: 300 300 0
// CHECK-NEXT: read-only on proto: 300 TypeError:300 0 300
// CHECK-NEXT: setter on proto proto: 300 600 0
// CHECK-NEXT: preventExtensions: 0 300 TypeError:300 0
// CHECK-NEXT: freeze: 0 TypeError:600 300 0
// CHECK-NEXT: defineProperty: 0 TypeError:600 300 300 12600
// CHECK-NEXT: delete and add: 273600 x,z,y:300

// 5. Unusual prototypes.

print('prototypes');
// CHECK-LABEL: prototypes

(function () {
  var K = seedKeys({extra: 0, name: 0, length: 0, prototype: 0});
  var kExtra = K[0];
  var kName = K[1];
  var kLength = K[2];
  var kPrototype = K[3];

  /// \return a proxy whose traps count into \p log; nothing is defined.
  function countingProxy(log) {
    return new Proxy(
      {},
      {
        set: function (t, key, v, r) {
          ++log.sets;
          return true;
        },
        get: function (t, key, r) {
          ++log.gets;
          return 'g';
        },
      },
    );
  }

  // A proxy as prototype from the start: its traps are called.
  var log = {sets: 0, gets: 0};
  var p = countingProxy(log);
  var own = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(p);
    o[kExtra] = i;
    if (Object.hasOwn(o, kExtra)) ++own;
  }
  print('proxy: ' + log.sets + ' ' + own);

  // A plain prototype, filled and hit through adds and stores, then a proxy
  // put above it: the set trap is called for every add after the change.
  var pp = {};
  var bad = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(pp);
    o[kExtra] = i;
    o[kExtra] = o[kExtra] + 1;
    if (!Object.hasOwn(o, kExtra) || o[kExtra] !== i + 1) ++bad;
  }
  log = {sets: 0, gets: 0};
  Object.setPrototypeOf(pp, countingProxy(log));
  own = 0;
  var reads = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(pp);
    o[kExtra] = i;
    if (Object.hasOwn(o, kExtra)) ++own;
    if (o[kExtra] === 'g') ++reads;
  }
  print('proxy above prototype: ' + bad + ' ' + log.sets + ' ' + log.gets +
    ' ' + own + ' ' + reads);

  // An array as prototype, with a plain and an index-like name. extra and
  // length are added and read back while the receiver's class is cacheable;
  // '0' then gives it an index-like property.
  var arr = [10, 20, 30];
  arr.extra = 7;
  var AK = Object.keys(arr);
  var sum = 0;
  bad = 0;
  var last;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(arr);
    sum += o[AK[0]] + o[AK[3]] + o[kLength];
    o[AK[3]] = i;
    o[kLength] = i + 2;
    if (o[AK[3]] !== i || o[kLength] !== i + 2 || o.extra !== i) ++bad;
    o[AK[0]] = i + 1;
    sum += o[AK[0]] + o[AK[3]] + o[kLength] + o[AK[1]];
    last = o;
  }
  print('array: ' + AK.join() + ' ' + bad + ' ' + sum + ' ' +
    JSON.stringify(last) + ' ' + JSON.stringify(arr) + ' ' + arr.extra);

  // Functions as prototypes, whose name, length and prototype are created
  // lazily. A put of name or length is a TypeError (read-only on the
  // function); a put of prototype adds an own property (writable on it).
  function run(label, getFn) {
    var errs = [];
    var sum = 0;
    var names = [];
    var bad = 0;
    for (var i = 0; i < N; ++i) {
      var fn = getFn();
      var o = Object.create(fn);
      o[kPrototype] = i;
      if (o[kPrototype] !== i || !Object.hasOwn(o, kPrototype)) ++bad;
      if (typeof fn.prototype !== 'object' || fn.prototype === i) ++bad;
      o[kExtra] = i + 1;
      if (o[kExtra] !== i + 1 || o[kPrototype] !== i) ++bad;
      sum += o[kLength];
      names.push(o[kName]);
      errs.push(
        errName(function () {
          o[kName] = 'x';
        }),
      );
      errs.push(
        errName(function () {
          o[kLength] = 5;
        }),
      );
      if (Object.hasOwn(o, kName) || Object.hasOwn(o, kLength)) ++bad;
    }
    print(label + ': ' + bad + ' ' + sum + ' ' + tally(names) + ' ' +
      tally(errs));
  }
  // One function, not used as an object before, for all the receivers.
  function sharedFn(a, b) {}
  run('shared function', function () {
    return sharedFn;
  });
  // A fresh function for each receiver.
  run('fresh function', function () {
    return function freshFn(a, b, c) {};
  });

  // A function not used as an object before, below a prototype that has its
  // own writable name: adds of name to the receivers fill and hit the
  // cache. Deleting the prototype's name exposes the function's read-only
  // one, which the next put must find.
  function lazyFn(a) {}
  var midProto = Object.create(lazyFn);
  Object.defineProperty(midProto, kName, {
    value: 'mid',
    writable: true,
    enumerable: true,
    configurable: true,
  });
  bad = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(midProto);
    o[kName] = 'own' + i;
    if (o[kName] !== 'own' + i || !Object.hasOwn(o, kName)) ++bad;
  }
  delete midProto[kName];
  var errs = [];
  var names = [];
  own = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(midProto);
    errs.push(
      errName(function () {
        o[kName] = 'own' + i;
      }),
    );
    if (Object.hasOwn(o, kName)) ++own;
    names.push(o[kName]);
  }
  print('name exposed on function: ' + bad + ' ' + tally(errs) + ' ' + own +
    ' ' + tally(names));

  // An object with a null prototype.
  sum = 0;
  bad = 0;
  var has = 0;
  for (var i = 0; i < N; ++i) {
    var o = Object.create(null);
    o[kExtra] = i;
    o[kName] = i + 1;
    o[kExtra] = o[kExtra] + o[kName];
    if (o[kExtra] !== 2 * i + 1 || o[kName] !== i + 1) ++bad;
    sum += o[kExtra] + o[kName];
    if (hasOwnProperty.call(o, kExtra)) ++has;
    if (Object.hasOwn(o, kName)) ++has;
    if (!Object.hasOwn(o, kLength)) ++has;
    last = o;
  }
  print('null prototype: ' + bad + ' ' + sum + ' ' + has + ' ' +
    JSON.stringify(last));
})();
// CHECK-NEXT: proxy: 300 0
// CHECK-NEXT: proxy above prototype: 0 300 300 0 300
// CHECK-NEXT: array: 0,1,2,extra 0 147450 {"0":300,"extra":299,"length":301} [10,20,30] 7
// CHECK-NEXT: shared function: 0 600 sharedFn:300 TypeError:600
// CHECK-NEXT: fresh function: 0 900 freshFn:300 TypeError:600
// CHECK-NEXT: name exposed on function: 0 TypeError:300 0 lazyFn:300
// CHECK-NEXT: null prototype: 0 135150 900 {"extra":599,"name":300}

// 6. A proxy as receiver: the traps are called every time.

print('proxy receiver');
// CHECK-LABEL: proxy receiver

(function () {
  var K = seedKeys({a: 0, b: 0});
  var log = {get: 0, set: 0, gopd: 0};
  var handler = {
    get: function (t, key, r) {
      ++log.get;
      return t[key];
    },
    set: function (t, key, v, r) {
      ++log.set;
      t[key] = v;
      return true;
    },
    getOwnPropertyDescriptor: function (t, key) {
      ++log.gopd;
      return Object.getOwnPropertyDescriptor(t, key);
    },
  };
  var sum = 0;
  var has = 0;
  var last;
  for (var i = 0; i < N; ++i) {
    // Values far apart, so that reading one property for the other shows.
    var t = {a: i, b: i + 1000};
    var p = new Proxy(t, handler);
    for (var j = 0; j < K.length; ++j) {
      var k = K[j];
      sum += p[k];
      p[k] = p[k] + 1;
      if (hasOwnProperty.call(p, k)) ++has;
      if (Object.hasOwn(p, k)) ++has;
    }
    last = t;
  }
  print(sum + ' ' + has + ' ' + JSON.stringify(log) + ' ' +
    JSON.stringify(last));
})();
// CHECK-NEXT: 389700 1200 {"get":1200,"set":600,"gopd":1200} {"a":300,"b":1300}

// 7. Reflect.set and Reflect.get with a receiver that is not the target,
// after the target's class has entries for the key.

print('reflect receiver');
// CHECK-LABEL: reflect receiver

(function () {
  var K = seedKeys({a: 0, b: 0, c: 0});
  var ka = K[0];
  var kb = K[1];
  var kc = K[2];
  var badWarm = 0;
  var okSet = 0;
  var okSetSame = 0;
  var okAdd = 0;
  var okGet = 0;
  var lastRecv;
  for (var i = 0; i < N; ++i) {
    // A store to a and an add of c on objects of the target's class, read
    // back.
    var warm = {a: 1, b: 2};
    warm[ka] = i;
    warm[kc] = i + 1;
    if (warm[ka] !== i || warm[kb] !== 2 || warm[kc] !== i + 1) ++badWarm;
    if (warm.a !== i || warm.b !== 2 || warm.c !== i + 1) ++badWarm;
    var target = {a: 1, b: 2};
    target[ka] = i;

    var receiver = {};
    if (
      Reflect.set(target, ka, -1, receiver) &&
      target.a === i &&
      receiver.a === -1 &&
      Object.hasOwn(receiver, ka)
    )
      ++okSet;

    // A receiver of the same class as the target.
    var receiver2 = {a: 100, b: 200};
    if (
      Reflect.set(target, ka, -2, receiver2) &&
      target.a === i &&
      receiver2.a === -2 &&
      receiver2.b === 200
    )
      ++okSetSame;

    // An absent key: the receiver gets it, the target does not.
    var receiver3 = {a: 100, b: 200};
    if (
      Reflect.set(target, kc, -3, receiver3) &&
      !Object.hasOwn(target, kc) &&
      receiver3.c === -3
    )
      ++okAdd;

    if (Reflect.get(target, ka, receiver2) === i) ++okGet;
    lastRecv = receiver3;
  }
  var withGetter = {
    get a() {
      return this === lastRecv ? 'receiver' : 'other';
    },
  };
  print(badWarm + ' ' + okSet + ' ' + okSetSame + ' ' + okAdd + ' ' + okGet +
    ' ' + Reflect.get(withGetter, ka, lastRecv) + ' ' +
    JSON.stringify(lastRecv));
})();
// CHECK-NEXT: 0 300 300 300 300 receiver {"a":100,"b":200,"c":-3}

// 8. Non-string keys, primitive receivers, more names than the cache holds,
// doubles, and __proto__.

print('non-string keys');
// CHECK-LABEL: non-string keys

(function () {
  var K = seedKeys({a: 0, b: 0});
  var ka = K[0];
  var kb = K[1];
  var sym = Symbol('s');
  var strObj = new String(ka);
  var toStringCalls = 0;
  var keyObj = {
    toString: function () {
      ++toStringCalls;
      return ka;
    },
  };
  var sum = 0;
  var has = 0;
  var bad = 0;
  var last;
  for (var i = 0; i < N; ++i) {
    var o = {a: i};
    // A uniqued add and gets, which use the cache, between the others.
    o[kb] = -i;
    if (o[ka] !== i || o[kb] !== -i) ++bad;
    o[sym] = 3;
    o[strObj] = o[strObj] + 1;
    o[1.5] = 2;
    if (o[ka] !== i + 1 || o[kb] !== -i || o.b !== -i) ++bad;
    o[keyObj] = o[keyObj] + 1;
    if (o[ka] !== i + 2) ++bad;
    // An index-like name last: the class is no longer cacheable after it.
    o[0] = 1;
    sum += o[0] + o[1.5] + o[sym] + o.a + o['0'];
    if (hasOwnProperty.call(o, 0)) ++has;
    if (Object.hasOwn(o, 1.5)) ++has;
    if (hasOwnProperty.call(o, sym)) ++has;
    if (Object.hasOwn(o, strObj)) ++has;
    if (hasOwnProperty.call(o, keyObj)) ++has;
    if (!Object.hasOwn(o, 2)) ++has;
    last = o;
  }
  print(bad + ' ' + sum + ' ' + has + ' ' + toStringCalls + ' ' +
    JSON.stringify(last) + ' ' + Object.getOwnPropertySymbols(last).length);
})();
// CHECK-NEXT: 0 47550 1800 900 {"0":1,"a":301,"b":-299,"1.5":2} 1

// A get with a primitive receiver boxes it into a fresh wrapper object and
// looks the key up there (Interpreter::getByValTransientWithReceiver_RJS).
// The methods are not own properties of the wrapper, so nothing is filled:
// these gets take the generic path every time.

print('primitive receivers');
// CHECK-LABEL: primitive receivers

(function () {
  var K = seedKeys({
    toFixed: 0,
    charAt: 0,
    valueOf: 0,
    toString: 0,
    length: 0,
  });
  var kToFixed = K[0];
  var kCharAt = K[1];
  var kValueOf = K[2];
  var kToString = K[3];
  var kLength = K[4];
  var same = 0;
  var results = [];
  var errs = [];
  for (var i = 0; i < N; ++i) {
    if ((5)[kToFixed] === Number.prototype.toFixed) ++same;
    if ('abc'[kCharAt] === String.prototype.charAt) ++same;
    if (true[kValueOf] === Boolean.prototype.valueOf) ++same;
    results.push(
      (i % 3)[kToFixed](1) +
        '|' +
        'abc'[kLength] +
        '|' +
        'abc'[kCharAt](i % 3) +
        '|' +
        true[kValueOf]() +
        '|' +
        true[kToString]() +
        '|' +
        (5)[kLength] +
        '|' +
        true[kLength],
    );
    errs.push(
      errName(function () {
        'abc'[kLength] = 1;
      }),
    );
  }
  print(same + ' ' + tally(results) + ' ' + tally(errs));
})();
// CHECK-NEXT: 900 0.0|3|a|true|true|undefined|undefined:100 1.0|3|b|true|true|undefined|undefined:100 2.0|3|c|true|true|undefined|undefined:100 TypeError:300

// More (class, name) pairs than the cache holds (1024 sets of 2 ways). Each
// window of 200 objects, each with its own name and v, is filled and hit in
// four rounds of fresh objects, about 800 entries; the next window evicts
// it. At the end all 3000 objects are read back.

print('3000 names');
// CHECK-LABEL: 3000 names

(function () {
  var M = 3000;
  var W = 200;
  var R = 4;
  var seed = {};
  for (var i = 0; i < M; ++i) seed['q' + i] = i;
  var Q = seedKeys(seed);
  var kv = seedKeys({v: 0})[0];
  var objs = [];
  var bad = 0;
  var sum = 0;
  for (var w = 0; w < M; w += W) {
    for (var r = 0; r < R; ++r) {
      for (var i = w; i < w + W; ++i) {
        var val = i * 10 + r;
        var o = {};
        o[Q[i]] = val;
        o[kv] = -val;
        if (o[Q[i]] !== val || o[kv] !== -val) ++bad;
        o[Q[i]] = o[Q[i]] + 1;
        o[kv] = o[kv] - 1;
        if (o[Q[i]] !== val + 1 || o[kv] !== -val - 1 || o.v !== -val - 1)
          ++bad;
        if (!Object.hasOwn(o, Q[i]) || hasOwnProperty.call(o, Q[(i + 1) % M]))
          ++bad;
        sum += o[Q[i]] - o[kv];
        objs[i] = o;
      }
    }
  }
  // All of them, in reverse order.
  for (var i = M - 1; i >= 0; --i) {
    var o = objs[i];
    var val = i * 10 + R - 1;
    if (o[Q[i]] !== val + 1 || o[kv] !== -val - 1) ++bad;
    if (!Object.hasOwn(o, Q[i]) || hasOwnProperty.call(o, kv) !== true) ++bad;
    sum += o[Q[i]];
  }
  print(Q.length + ' ' + bad + ' ' + sum + ' ' + JSON.stringify(objs[0]) +
    ' ' + JSON.stringify(objs[M - 1]));
})();
// CHECK-NEXT: 3000 0 404937000 {"q0":4,"v":-4} {"q2999":29994,"v":-29994}

print('doubles');
// CHECK-LABEL: doubles

(function () {
  var D = seedKeys({d0: 0, d1: 0, d2: 0});
  var bad = 0;
  var sum = 0;
  var last;
  for (var i = 0; i < N; ++i) {
    var o = {};
    for (var j = 0; j < D.length; ++j) o[D[j]] = i + 0.1 + j;
    for (var j = 0; j < D.length; ++j) o[D[j]] = o[D[j]] + 0.25;
    for (var j = 0; j < D.length; ++j) {
      if (o[D[j]] !== i + 0.1 + j + 0.25) ++bad;
      sum += o[D[j]];
    }
    var big = {d0: 0.5, d1: 0, d2: 0};
    big[D[0]] = i * 1e10 + 0.5;
    big[D[1]] = -i - 0.75;
    if (big[D[0]] !== i * 1e10 + 0.5 || big[D[1]] !== -i - 0.75) ++bad;
    last = o;
  }
  print(bad + ' ' + Math.round(sum * 100) + ' ' + JSON.stringify(last));
})();
// CHECK-NEXT: 0 13576500 {"d0":299.35,"d1":300.35,"d2":301.35}

print('__proto__');
// CHECK-LABEL: __proto__

(function () {
  var PROTO = Object.getOwnPropertyNames(Object.prototype).filter(
    function (n) {
      return n === '__proto__';
    },
  )[0];
  var XY = seedKeys({x: 0, y: 0});
  var kx = XY[0];
  var ky = XY[1];

  // On an ordinary object, __proto__ reaches the accessor on
  // Object.prototype: a fresh prototype each time, and a shared one followed
  // by adds of x and y and a store to y, which use the cache.
  var setProto = 0;
  var notOwn = 0;
  var reads = 0;
  var ignored = 0;
  var bad = 0;
  var shared = {tag: 'shared'};
  for (var i = 0; i < N; ++i) {
    var o = {};
    var np = {tag: i};
    o[PROTO] = np;
    if (Object.getPrototypeOf(o) === np && o.tag === i) ++setProto;
    if (!Object.hasOwn(o, PROTO) && !hasOwnProperty.call(o, PROTO)) ++notOwn;
    if (o[PROTO] === np) ++reads;
    var o2 = {};
    o2[PROTO] = 5;
    if (
      Object.getPrototypeOf(o2) === Object.prototype &&
      !Object.hasOwn(o2, PROTO)
    )
      ++ignored;
    var o3 = {};
    o3[PROTO] = shared;
    o3[kx] = i;
    o3[ky] = -i;
    o3[ky] = o3[ky] - 1;
    if (o3[kx] !== i || o3[ky] !== -i - 1 || o3[PROTO] !== shared) ++bad;
    if (o3.x !== i || o3.y !== -i - 1) ++bad;
    if (o3.tag !== 'shared' || Object.hasOwn(o3, PROTO)) ++bad;
  }
  print('ordinary: ' + PROTO + ' ' + setProto + ' ' + notOwn + ' ' + reads +
    ' ' + ignored + ' ' + bad + ' ' + JSON.stringify(shared));

  // On an object with a null prototype, __proto__ is a plain property, here
  // in the second slot.
  var sum = 0;
  var own = 0;
  bad = 0;
  var last;
  for (var i = 0; i < N; ++i) {
    var n = Object.create(null);
    n[kx] = -i;
    n[PROTO] = i;
    if (n[PROTO] !== i || n[kx] !== -i) ++bad;
    n[PROTO] = n[PROTO] + 1;
    if (n[PROTO] !== i + 1 || n[kx] !== -i) ++bad;
    sum += n[PROTO];
    if (
      Object.hasOwn(n, PROTO) &&
      hasOwnProperty.call(n, PROTO) &&
      Object.getPrototypeOf(n) === null
    )
      ++own;
    last = n;
  }
  print('null prototype: ' + bad + ' ' + sum + ' ' + own + ' ' +
    Object.keys(last) + ' ' + last[PROTO]);
})();
// CHECK-NEXT: ordinary: __proto__ 300 300 300 300 0 {"tag":"shared"}
// CHECK-NEXT: null prototype: 0 45150 300 x,__proto__ 300

// 9. Receivers built by many code paths, not only by puts and literals.
// Each source makes a fresh receiver R times; every receiver gets the same
// gets, hasOwnProperty and Object.hasOwn calls, puts to its own properties,
// computed adds of three names and an Object.assign, so that from the
// second receiver on these hit the cache wherever the class is cacheable.
// Each line is the tally of what the receivers ended with; one string
// occurring R times means every receiver ended the same. JSON.parse of
// text with a duplicate key and an object literal with one give storage
// longer than the class needs, where the adds must land in the slots that
// exist.

print('receiver sources');
// CHECK-LABEL: receiver sources

(function () {
  var R = 20;
  var K = seedKeys({a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0});
  var A = seedKeys({zz: 0, yy: 0, xx: 0});
  var extra = {a: 'A', ww: 'W'};

  /// \return the own enumerable properties of \p o as "k=v,...".
  function describe(o) {
    var parts = [];
    var keys = Object.keys(o);
    for (var i = 0; i < keys.length; ++i) parts.push(keys[i] + '=' + o[keys[i]]);
    return parts.join(',');
  }

  /// Run the operations on \p o; \return what they saw and what \p o ended
  /// with.
  function exercise(o) {
    var log = [];
    for (var j = 0; j < K.length; ++j) {
      var k = K[j];
      var own = hasOwnProperty.call(o, k);
      log.push((own ? 1 : 0) + (Object.hasOwn(o, k) ? 2 : 0) + ':' + o[k]);
      if (own) o[k] = o[k] + 10;
    }
    for (var j = 0; j < A.length; ++j) o[A[j]] = 100 + j;
    Object.assign(o, extra);
    for (var j = 0; j < A.length; ++j)
      log.push(o[A[j]] + (hasOwnProperty.call(o, A[j]) ? '+' : '-'));
    for (var j = 0; j < K.length; ++j) log.push(String(o[K[j]]));
    return log.join(' ') + ' | ' + describe(o);
  }

  /// Print the tally of exercise() over \p R receivers made by \p mk.
  function run(label, mk) {
    var results = [];
    for (var r = 0; r < R; ++r) results.push(exercise(mk()));
    print(label + ': ' + tally(results));
  }

  var dup1 = '{"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"a":8}';
  var dup3 = '{"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,' +
    '"a":-1,"b":-2,"c":-3}';
  var noDup = '{"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8}';
  run('JSON.parse', function () {
    return JSON.parse(noDup);
  });
  run('JSON.parse, one duplicate', function () {
    return JSON.parse(dup1);
  });
  run('JSON.parse, three duplicates', function () {
    return JSON.parse(dup3);
  });
  run('JSON.parse, array element', function () {
    return JSON.parse('[' + dup1 + ',' + noDup + ',' + dup3 + ']')[0];
  });
  run('JSON.parse, nested', function () {
    return JSON.parse('{"x":' + dup1 + ',"y":' + dup3 + '}').y;
  });
  run('JSON.parse, reviver that replaces', function () {
    return JSON.parse(dup1, function (k, v) {
      return typeof v === 'number' ? v * 2 : v;
    });
  });
  run('JSON.parse, reviver that deletes', function () {
    return JSON.parse(noDup, function (k, v) {
      return k === 'h' ? undefined : v;
    });
  });
  var holders = [];
  run('JSON.parse, reviver holder', function () {
    holders = [];
    JSON.parse('{"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7}', function (k, v) {
      holders.push(this);
      return v;
    });
    return holders[0];
  });
  run('literal, duplicate key', function () {
    return {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, a: 8};
  });
  run('literal, computed duplicate', function () {
    var k = K[0];
    return {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, [k]: 8};
  });
  var spreadSrc = {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7};
  run('spread', function () {
    return {...spreadSrc, ...spreadSrc, a: 8};
  });
  run('Object.fromEntries', function () {
    return Object.fromEntries([
      ['a', 1], ['b', 2], ['c', 3], ['d', 4], ['e', 5], ['f', 6], ['g', 7],
      ['a', 8],
    ]);
  });
  run('Object.assign target', function () {
    return Object.assign({}, spreadSrc, spreadSrc, {a: 8});
  });
  class C {
    constructor() {
      this.a = 1;
      this.b = 2;
      this.c = 3;
      this.d = 4;
      this.e = 5;
      this.f = 6;
      this.g = 7;
    }
  }
  run('class instance', function () {
    return new C();
  });
  var descs = {};
  for (var i = 0; i < 7; ++i)
    descs[K[i]] = {value: i + 1, writable: true, enumerable: true,
      configurable: true};
  run('Object.create with descriptors', function () {
    return Object.create(null, descs);
  });
  run('Object.create with descriptors and a prototype', function () {
    return Object.create(spreadSrc, descs);
  });
  var re = /(?<a>.)(?<b>.)(?<c>.)(?<d>.)(?<e>.)(?<f>.)(?<g>.)/;
  run('regexp groups', function () {
    return re.exec('1234567').groups;
  });
  run('regexp match array', function () {
    return /(.)(.)/.exec('ab');
  });
  run('function object', function () {
    var fn = function () {};
    fn.a = 1;
    fn.b = 2;
    fn.c = 3;
    fn.d = 4;
    fn.e = 5;
    fn.f = 6;
    fn.g = 7;
    return fn;
  });
  run('array', function () {
    return [1, 2, 3];
  });
})();
// CHECK-NEXT: JSON.parse: 3:1 3:2 3:3 3:4 3:5 3:6 3:7 3:8 100+ 101+ 102+ A 12 13 14 15 16 17 18 | a=A,b=12,c=13,d=14,e=15,f=16,g=17,h=18,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: JSON.parse, one duplicate: 3:8 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: JSON.parse, three duplicates: 3:-1 3:-2 3:-3 3:4 3:5 3:6 3:7 3:8 100+ 101+ 102+ A 8 7 14 15 16 17 18 | a=A,b=8,c=7,d=14,e=15,f=16,g=17,h=18,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: JSON.parse, array element: 3:8 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: JSON.parse, nested: 3:-1 3:-2 3:-3 3:4 3:5 3:6 3:7 3:8 100+ 101+ 102+ A 8 7 14 15 16 17 18 | a=A,b=8,c=7,d=14,e=15,f=16,g=17,h=18,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: JSON.parse, reviver that replaces: 3:16 3:4 3:6 3:8 3:10 3:12 3:14 0:undefined 100+ 101+ 102+ A 14 16 18 20 22 24 undefined | a=A,b=14,c=16,d=18,e=20,f=22,g=24,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: JSON.parse, reviver that deletes: 3:1 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: JSON.parse, reviver holder: 3:1 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: literal, duplicate key: 3:8 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: literal, computed duplicate: 3:8 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: spread: 3:8 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: Object.fromEntries: 3:8 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: Object.assign target: 3:8 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: class instance: 3:1 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: Object.create with descriptors: 3:1 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: Object.create with descriptors and a prototype: 3:1 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: regexp groups: 3:1 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 210 310 410 510 610 710 undefined | a=A,b=210,c=310,d=410,e=510,f=610,g=710,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: regexp match array: 0:undefined 0:undefined 0:undefined 0:undefined 0:undefined 0:undefined 0:undefined 0:undefined 100+ 101+ 102+ A undefined undefined undefined undefined undefined undefined undefined | 0=ab,1=a,2=b,index=0,input=ab,groups=undefined,zz=100,yy=101,xx=102,a=A,ww=W:20
// CHECK-NEXT: function object: 3:1 3:2 3:3 3:4 3:5 3:6 3:7 0:undefined 100+ 101+ 102+ A 12 13 14 15 16 17 undefined | a=A,b=12,c=13,d=14,e=15,f=16,g=17,zz=100,yy=101,xx=102,ww=W:20
// CHECK-NEXT: array: 0:undefined 0:undefined 0:undefined 0:undefined 0:undefined 0:undefined 0:undefined 0:undefined 100+ 101+ 102+ A undefined undefined undefined undefined undefined undefined undefined | 0=1,1=2,2=3,zz=100,yy=101,xx=102,a=A,ww=W:20
