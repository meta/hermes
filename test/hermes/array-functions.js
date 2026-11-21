/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -target=HBC -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -target=HBC -O -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s
"use strict";

// Performs a nested array comparison between the two arrays.
function arrayEquals(a, b) {
  if (a.length !== b.length) {
    return false;
  }
  for (var i = 0; i < a.length; ++i) {
    if (Array.isArray(a[i]) && Array.isArray(b[i])) {
      if (!arrayEquals(a[i], b[i])) {
        return false;
      }
    } else {
      if (a[i] !== b[i]) {
        return false;
      }
    }
  }
  return true;
}

print('toString');
// CHECK-LABEL: toString
print(Array(1,2,3).toString());
// CHECK-NEXT: 1,2,3
print('empty', Array().toString());
// CHECK-NEXT: empty

// Overwrite join and make sure it fails over to Object.prototype.toString().
var a = Array(1,2,3);
a.join = null;
print(a.toString());
// CHECK-NEXT: [object Array]
var ots = Object.prototype.toString;
Object.prototype.toString = null;
print(a.toString());
// CHECK-NEXT: [object Array]
Object.prototype.toString = ots;
var a = [1,2];
a.push(a);
print(a.toString());
// CHECK-NEXT: 1,2,
var a = [1,2];
var b = [3,4,a];
a.push(b);
print(a.toString());
// CHECK-NEXT: 1,2,3,4,
var a = [1,2];
var b = [3,4];
var c = [5,6,b];
b.push(c);
a.push(b);
print(a.toString());
// CHECK-NEXT: 1,2,3,4,5,6,

print('of');
// CHECK-LABEL: of
print(Array.of(1,2,3));
// CHECK-NEXT: 1,2,3
print(Array.of('asdf', {}));
// CHECK-NEXT: asdf,[object Object]
print(Array.of.call(Math.sin, 1, 2, 3));
// CHECK-NEXT: 1,2,3
print(Array.of.call(Math.sin.bind(), 1, 2, 3));
// CHECK-NEXT: 1,2,3
print(Array.of.call(Math.sin.bind().bind(), 1, 2, 3));
// CHECK-NEXT: 1,2,3
print(Array.of(), "EMPTY");
// CHECK-NEXT: EMPTY
var res = Array.of.call(Object, 1, 2, 3);
print(Array.isArray(res), res[0], res[1], res[2]);
// CHECK-NEXT: false 1 2 3
var f = function() {}
var res = Array.of.call(f, 1, 2, 3);
print(res instanceof f, res[0], res[1], res[2]);
// CHECK-NEXT: true 1 2 3
var res = Array.of.call(f.bind(), 1, 2, 3);
print(res instanceof f, res[0], res[1], res[2]);
// CHECK-NEXT: true 1 2 3

print('from');
// CHECK-LABEL: from
var emptyRes = Array.from([]);
print(emptyRes.length);
// CHECK-NEXT: 0
emptyRes = Array.from("");
print(emptyRes.length);
// CHECK-NEXT: 0
emptyRes = Array.from({});
print(emptyRes.length);
// CHECK-NEXT: 0
var fromRes = Array.from('foo');
print(fromRes);
// CHECK-NEXT: f,o,o
print(fromRes.length);
// CHECK-NEXT: 3
print(Array.from(new Set(['foo', 1])));
// CHECK-NEXT: foo,1
print(Array.from([1, 2, 3], function(x) { return x + 1; }));
// CHECK-NEXT: 2,3,4
print(Array.from({length: 5}, function(v, i) { return i; }));
// CHECK-NEXT: 0,1,2,3,4
var thisObj = {a : "hello"};
Array.from([0, 1], function() { print(this.a); return 0;}, thisObj);
// CHECK-NEXT: hello
// CHECK-NEXT: hello

// user-defined iterator
var iterable = {};
iterable[Symbol.iterator] = function () {
    var step = 0;
    var iterator = {
        next: function() {
            if (step == 5) {
              return {value: undefined, done: true};
            } else {
        			return {value: step++, done: false};
            }
        }
    };
	return iterator;
}
var iterableArr = Array.from(iterable);
print(iterableArr);
// CHECK-NEXT: 0,1,2,3,4
print(iterableArr.length);
// CHECK-NEXT: 5
iterableArr = Array.from(iterable, function(x, i) { return x + i; });
print(iterableArr);
// CHECK-NEXT: 0,2,4,6,8
print(iterableArr.length);
// CHECK-NEXT: 5
Array.from(iterable, function() { print(this.a); return 0;}, thisObj);
// CHECK-NEXT: hello
// CHECK-NEXT: hello
// CHECK-NEXT: hello
// CHECK-NEXT: hello
// CHECK-NEXT: hello

function testThisAsConstructor(items) {
  var ctor = function() {return {hello : "hello"};};
  var res = Array.from.call(ctor, items);
  print(res.length);
  print(res.hello);
}
testThisAsConstructor([1, 2]);
// CHECK-NEXT: 2
// CHECK-NEXT: hello
testThisAsConstructor({1: 1, 2: 2, length: 2});
// CHECK-NEXT: 2
// CHECK-NEXT: hello
testThisAsConstructor(iterable);
// CHECK-NEXT: 5
// CHECK-NEXT: hello

print('toLocaleString');
// CHECK-LABEL: toLocaleString
print([1,2,3].toLocaleString());
// CHECK-NEXT: 1,2,3
var n = 0;
var obj = {toLocaleString: function() {++n; return 1337;}};
print([1,2,obj,3,obj].toLocaleString());
print(n);
// CHECK-NEXT: 1,2,1337,3,1337
// CHECK-NEXT: 2
var a = [1,2];
a.push(a);
print(a.toLocaleString());
// CHECK-NEXT: 1,2,
var a = [1,2];
var b = [3,4,a];
a.push(b);
print(a.toLocaleString());
// CHECK-NEXT: 1,2,3,4,
var a = [1,2];
var b = [3,4];
var c = [5,6,b];
b.push(c);
a.push(b);
print(a.toLocaleString());
// CHECK-NEXT: 1,2,3,4,5,6,
try {[{toLocaleString:1}].toLocaleString()}
catch(e) {print('caught', e.name, e.message);}
// CHECK-NEXT: caught TypeError {{.*}}

print('concat');
// CHECK-LABEL: concat
print([1,2].concat([3,4]));
// CHECK-NEXT: 1,2,3,4
print([].concat([3,4]));
// CHECK-NEXT: 3,4
print([1,2].concat([]));
// CHECK-NEXT: 1,2
print('empty', [].concat([]));
// CHECK-NEXT: empty
print(['a','b'].concat(['c','d'], 'efg', 123, {}));
// CHECK-NEXT: a,b,c,d,efg,123,[object Object]
var a = [1,2].concat([[3,4],[5,6]]);
print(a[0], a[1], a[2][0], a[2][1], a[3][0], a[3][1]);
// CHECK-NEXT: 1 2 3 4 5 6
print([1,2].concat(3, {0: 4, 1: 5, length: 2}));
// CHECK-NEXT: 1,2,3,[object Object]
var a = [1,,];
a.__proto__[1] = 2;
print(a.concat(3));
// CHECK-NEXT: 1,2,3
delete a.__proto__[1];
var a = [1,2,3]
a = [];
print(typeof a.concat(100)[0]);
// CHECK-NEXT: number
Array.prototype[1] = 1;
var x = [];
x.length = 2;
var a = x.concat();
print(a.hasOwnProperty('1'));
// CHECK-NEXT: true
delete Array.prototype[1];
var args = [];
for (var i = 0; i < 1000; ++i) args.push([]);
Array.prototype.concat.apply([], args);
var obj = {
  length: Number.MAX_SAFE_INTEGER,
  get '0'() {
    throw 'in_getter';
  },
};
obj[Symbol.isConcatSpreadable] = true;
try {
  [].concat(obj);
} catch (e) {
  print(e);
}
// CHECK-NEXT: in_getter

print('copyWithin');
// CHECK-LABEL: copyWithin
print([1,2,3,4,5].copyWithin(0,3,4));
// CHECK-NEXT: 4,2,3,4,5
print([4,2,3,4,5].copyWithin(1,3));
// CHECK-NEXT: 4,4,5,4,5
print([1,2,3,4,5].copyWithin(-2));
// CHECK-NEXT: 1,2,3,1,2
print([1,2,3,4,5].copyWithin(1));
// CHECK-NEXT: 1,1,2,3,4
print([1,2,3,4,5].copyWithin(1));
// CHECK-NEXT: 1,1,2,3,4
print([1,2,,,5].copyWithin(0,2,4));
// CHECK-NEXT: ,,,,5
print([1,2,3,4,5].copyWithin(-2, -3, -1));
// CHECK-NEXT: 1,2,3,3,4
print([1,2,3,4,5].copyWithin(-100, -1, -3));
// CHECK-NEXT: 1,2,3,4,5
print([1,2,3,4,5].copyWithin(100, -1, -3));
// CHECK-NEXT: 1,2,3,4,5
print([1,2,3,4,5].copyWithin(-2, -1, -3));
// CHECK-NEXT: 1,2,3,4,5
print([].copyWithin(-2, -1, -3), "EMPTY");
// CHECK-NEXT: EMPTY
var res = [].copyWithin.call({length: 5, 3: 1}, 0, 3);
print(res[0], res[1], res[2], res[3], res[4], res.length);
// CHECK-NEXT: 1 undefined undefined 1 undefined 5

print('join');
// CHECK-LABEL: join
print(Array(1,2,3).join());
// CHECK-NEXT: 1,2,3
print(Array(1,2,3).join(','));
// CHECK-NEXT: 1,2,3
print(Array(1,2,3).join(':'));
// CHECK-NEXT: 1:2:3
print(Array(1,2,Array(3,4),5).join(':'));
// CHECK-NEXT: 1:2:3,4:5
print(Array(1,2,3).join(' SEPARATOR '));
// CHECK-NEXT: 1 SEPARATOR 2 SEPARATOR 3
print('empty', Array().join());
// CHECK-NEXT: empty
print('empty', Array().join('::::'));
// CHECK-NEXT: empty
print(Array(null,2,undefined,3,null).join('-'));
// CHECK-NEXT: -2--3-

print('pop');
// CHECK-LABEL: pop
var a = Array(1,2,3);
print(a.pop(), a.length, a[0], a[1], a[2]);
// CHECK-NEXT: 3 2 1 2 undefined
print(a.pop(), a.length, a[0], a[1], a[2]);
// CHECK-NEXT: 2 1 1 undefined undefined
print(a.pop(), a.length, a[0], a[1], a[2]);
// CHECK-NEXT: 1 0 undefined undefined undefined
print(a.pop(), a.length, a[0], a[1], a[2]);
// CHECK-NEXT: undefined 0 undefined undefined undefined
print(a.pop(), a.length, a[0], a[1], a[2]);
// CHECK-NEXT: undefined 0 undefined undefined undefined
// Test recursion of pop re-entering itself.
var a = [];
Object.defineProperty(a, 9, {
  get: Array.prototype.pop,
});
try {
  print(a.pop());
} catch (e) {
  // Infinite recursion, should throw call stack exceeded.
  print(e.name);
}
// CHECK-NEXT: RangeError
var a = [];
a[0xFFFFFFFE] = 1;
print(a.length);
// CHECK-NEXT: 4294967295
print(a.pop());
// CHECK-NEXT: 1
print(a.length);
// CHECK-NEXT: 4294967294

print('push');
// CHECK-LABEL: push
var a = [1,2,3];
print(a.push(1834), a.toString());
// CHECK-NEXT: 4 1,2,3,1834
print(a.push('abcd', 746, 133), a.toString());
// CHECK-NEXT: 7 1,2,3,1834,abcd,746,133
print(a.push(), a.toString());
// CHECK-NEXT: 7 1,2,3,1834,abcd,746,133
print(a.pop(), a.toString());
// CHECK-NEXT: 133 1,2,3,1834,abcd,746
print(a.push('last'), a.toString());
// CHECK-NEXT: 7 1,2,3,1834,abcd,746,last
var a = [,,,'x','y','z'];
print(a)
// CHECK-NEXT: ,,,x,y,z
print(a.push(1,2,3), a);
// CHECK-NEXT: 9 ,,,x,y,z,1,2,3
var a = {3: 'x', 4: 'y', length: 5};
Array.prototype.push.call(a, 'z', 'last');
print(a.length, a[5], a[6]);
// CHECK-NEXT: 7 z last
var a = Array(0xfffffffe);
print(a.length);
// CHECK-NEXT: 4294967294
a.push('arrEnd');
print(a.length, a[4294967294]);
// CHECK-NEXT: 4294967295 arrEnd
try { a.push('a','b','c'); } catch (e) { print('caught', e.name) }
// CHECK-NEXT: caught RangeError
print(a[4294967295], a[4294967296], a[4294967297]);
// CHECK-NEXT: a b c
Array.prototype[1] = 'asdf';
Object.defineProperty(Array.prototype, '1', {
  value: 'asdf',
  writable: false,
  configurable: true,
});
var a = [];
try { a.push('x','y','z'); } catch (e) { print('caught', e.name) }
// CHECK-NEXT: caught TypeError
delete Array.prototype[1];
Object.defineProperty(Array.prototype, '1', {
  get: function() { return 'asdf'; },
  set: function() { print('setter'); },
  configurable: true,
});
var a = [];
print(a.push(1,2), a);
// CHECK-NEXT: setter
// CHECK-NEXT: 2 1,asdf
delete Array.prototype[1];
var a = [];
a[0] = 'x1';
a[10000] = 'x2';
print(a.push('last'), a[0], a[10000], a[10001]);
// CHECK-NEXT: 10002 x1 x2 last
a = null;
var a = {length: 2**53 - 1};
try { a.push(1); } catch (e) { print('caught', e.name) }
// CHECK-NEXT: caught TypeError

print('reverse');
// CHECK-LABEL: reverse
var a = [1,2,3];
print(a.reverse(), a);
// CHECK-NEXT: 3,2,1 3,2,1
print([1,2].reverse());
// CHECK-NEXT: 2,1
print('empty', [].reverse());
// CHECK-NEXT: empty
print([12,13,14,15,16,17,18].reverse());
// CHECK-NEXT: 18,17,16,15,14,13,12
print([,,,1].reverse());
// CHECK-NEXT: 1,,,
print([,,1,,5,7,,].reverse());
// CHECK-NEXT: ,7,5,,1,,
var a = {};
a[0] = 'a';
a[1] = 'b';
a[5] = 'c';
a.length = 6;
Array.prototype.reverse.call(a);
print(a[0], a[4], a[5]);
// CHECK-NEXT: c b a
var a = [0, 1, 2, 3];
Object.defineProperty(a, 3, {
  get: function() {
    delete a[1];
    return -1;
  },
  set: function() { print('setter'); }
});
a.reverse();
print(a);
// CHECK-NEXT: setter
// CHECK-NEXT: -1,2,,-1
var a = [0, 1];
Object.defineProperty(a, 0, {
  get: function() {
    a.pop();
    return -1;
  }
});
a.reverse();
print(a);
// CHECK-NEXT: ,-1
var a = [];
Object.defineProperties(a, {
  '0': {
    get: function() {
      print('getter 0');
      return a.val_0;
    },
    set: function(v) { a.val_0 = v; }
  },
  '1': {
    get: function() {
      print('getter 1');
      return a.val_1;
    },
    set: function(v) { a.val_1 = v; }
  },
});
a[0] = 0;
a[1] = 1;
a.reverse();
print(a);
// CHECK-NEXT: getter 0
// CHECK-NEXT: getter 1
// CHECK-NEXT: getter 0
// CHECK-NEXT: getter 1
// CHECK-NEXT: 1,0
var a = [0, 1, 2, 3, 4, 5, 6];
Object.defineProperties(a, {
  '0': {
    get: function() {
      a.pop();
      return -1;
    }
  },
  '1': {
    set: function() { a.push('a'); }
  },
  '2': {
    get: function() {
      a.push('b');
      return -2;
    },
    set: function() { a.pop(); }
  }
});
a.reverse();
print(a);
// CHECK-NEXT: ,,-2,3,-2,,-1,a

print('shift');
// CHECK-LABEL: shift
var a = [1,2,3];
print(a.shift(), a, a.length);
// CHECK-NEXT: 1 2,3 2
print(a.shift(), a, a.length);
// CHECK-NEXT: 2 3 1
print(a.shift(), a, a.length);
// CHECK-NEXT: 3  0
print(a.shift(), a, a.length);
// CHECK-NEXT: undefined  0
print(a.shift(), a, a.length);
// CHECK-NEXT: undefined  0

print('slice');
// CHECK-LABEL: slice
var a = ['a','b','c','d','e'];
print(a.slice(1, 3), a.slice(1,3).length, a);
// CHECK-NEXT: b,c 2 a,b,c,d,e
print(a.slice(1));
// CHECK-NEXT: b,c,d,e
print(a.slice(1, 1000));
// CHECK-NEXT: b,c,d,e
print(a.slice());
// CHECK-NEXT: a,b,c,d,e
print(a.slice(-1000));
// CHECK-NEXT: a,b,c,d,e
print(a.slice(-1000, 1000));
// CHECK-NEXT: a,b,c,d,e
print(a.slice(4, 1000));
// CHECK-NEXT: e
print('empty', a.slice(4, -1000));
// CHECK-NEXT: empty
var a = [];
print('empty', a.slice(4, 1000));
// CHECK-NEXT: empty
print('empty', a.slice(-10, 10));
// CHECK-NEXT: empty
print('empty', a.slice());
// CHECK-NEXT: empty

print('sort');
// CHECK-LABEL: sort
print('empty', [].sort());
// CHECK-NEXT: empty
print([1,2].sort());
// CHECK-NEXT: 1,2
print([2,1].sort());
// CHECK-NEXT: 1,2
print([2,13].sort());
// CHECK-NEXT: 13,2
print([2,13].sort(function(x,y) {return x - y}));
// CHECK-NEXT: 2,13
print(['a', 'aaa', 'aa'].sort(function(x,y) {return x.length - y.length}));
// CHECK-NEXT: a,aa,aaa
print([4,3,5,1,8,6,7,2].sort());
// CHECK-NEXT: 1,2,3,4,5,6,7,8
print([8,7,6,5,4,3,2,1].sort());
// CHECK-NEXT: 1,2,3,4,5,6,7,8
print([,undefined,,8,,,7,6,5,4,3,2,1].sort());
// CHECK-NEXT: 1,2,3,4,5,6,7,8,,,,,
print([8,32,86,25,71,92,66,68,97,52].sort(function(x,y) {return y - x;}));
// CHECK-NEXT: 97,92,86,71,68,66,52,32,25,8
print([
  93,62,92,70,2,64,61,16,85,18,3,18,59,35,54,9,31,50,67,37,82,6,36
].sort(function(x,y) {return x - y;}));
// CHECK-NEXT: 2,3,6,9,16,18,18,31,35,36,37,50,54,59,61,62,64,67,70,82,85,92,93
var a = {0: 'c', 1: 'b', 2: 'a', 3: 'f', 4: 'd', 5: 'g', 6: 'e'};
a.length = 7;
print(Array.prototype.join.call(Array.prototype.sort.call(a), ''));
// CHECK-NEXT: abcdefg
var a = {0: 2, 1: 13};
a.length = 2;
print(Array.prototype.join.call(Array.prototype.sort.call(a)));
// CHECK-NEXT: 13,2
var a = {0: 2, 1: 13};
a.length = 2;
print(Array.prototype.join.call(Array.prototype.sort.call(a, function(x,y) {
  return x - y;
})));
// CHECK-NEXT: 2,13
var a = [
    { "idx": 0, "key": 1 },
    { "idx": 1, "key": 2 },
    { "idx": 2, "key": 1 },
    { "idx": 3, "key": 2 },
    { "idx": 4, "key": 1 },
    { "idx": 5, "key": 2 },
    { "idx": 6, "key": 1 },
    { "idx": 7, "key": 2 }];
a.sort((item1, item2) => item1.key - item2.key);
print(JSON.stringify(a));
// CHECK-NEXT: [{"idx":0,"key":1},{"idx":2,"key":1},{"idx":4,"key":1},{"idx":6,"key":1},{"idx":1,"key":2},{"idx":3,"key":2},{"idx":5,"key":2},{"idx":7,"key":2}]
a.sort((item1, item2) => item1.key - item2.key);
print(JSON.stringify(a));
// CHECK-NEXT: [{"idx":0,"key":1},{"idx":2,"key":1},{"idx":4,"key":1},{"idx":6,"key":1},{"idx":1,"key":2},{"idx":3,"key":2},{"idx":5,"key":2},{"idx":7,"key":2}]
a.sort((item1, item2) => item2.key - item1.key);
print(JSON.stringify(a));
// CHECK-NEXT: [{"idx":1,"key":2},{"idx":3,"key":2},{"idx":5,"key":2},{"idx":7,"key":2},{"idx":0,"key":1},{"idx":2,"key":1},{"idx":4,"key":1},{"idx":6,"key":1}]
print(['1', '111', '11', '222', '22', '2', '33', '3', '333'].sort(
  function(x,y) {return x.length - y.length})
);
// CHECK-NEXT: 1,2,3,11,22,33,111,222,333
try { [1,2,3].sort(null); } catch(e) { print('caught', e.name); }
// CHECK-NEXT: caught TypeError
var badobj = {
  get length() { throw RangeError("Don't get the length"); }
}
try { [].sort.call(badobj, null); } catch(e) { print('caught', e.name); }
// CHECK-NEXT: caught TypeError
try { [1].sort(12); } catch(e) { print('caught', e.name); }
// CHECK-NEXT: caught TypeError
try { [].sort(true); } catch(e) { print('caught', e.name); }
// CHECK-NEXT: caught TypeError
try { [1,2,3].sort({}); } catch(e) { print('caught', e.name); }
// CHECK-NEXT: caught TypeError
var a = [{}, {}];
a.__defineGetter__(1, function() { a.length = 0; return 0; });
a.sort();
print('sorting', a, 'did not crash');
// CHECK-NEXT: sorting 0,[object Object] did not crash
var a = new Array(2);
// hole at 0
a[1] = 1;
a.__proto__ = new Proxy([],{});
a.sort();
print(a);
// CHECK-NEXT: 1,

print('splice');
// CHECK-LABEL: splice
var a = ['a','b','c','d'];
print(a.splice(), a);
// CHECK-NEXT:  a,b,c,d
var a = ['a','b','c','d'];
print(a.splice(1), a);
// CHECK-NEXT: b,c,d a
var a = ['a','b','c','d'];
print(a.splice(1, 2), a);
// CHECK-NEXT: b,c a,d
var a = ['a','b','c','d'];
print(a.splice(1, 2, 'x'), a);
// CHECK-NEXT: b,c a,x,d
var a = ['a','b','c','d'];
print(a.splice(1, 2, 'x', 'y', 'z'), a);
// CHECK-NEXT: b,c a,x,y,z,d
var a = ['a','b','c','d'];
print(a.splice(1, 1000, 'x', 'y', 'z'), a);
// CHECK-NEXT: b,c,d a,x,y,z
var a = ['a','b','c','d'];
print(a.splice(-1, 1, 'x', 'y', 'z'), a);
// CHECK-NEXT: d a,b,c,x,y,z
var a = ['a','b','c','d'];
print(a.splice(-1, 1, 'x', 'y', 'z'), a);
// CHECK-NEXT: d a,b,c,x,y,z
var a = ['a','b','c','d'];
print(a.splice(2, 0, 'x', 'y', 'z'), a);
// CHECK-NEXT:  a,b,x,y,z,c,d
var a = ['a','b','c','d'];
print(a.splice(2, -100, 'x', 'y', 'z'), a);
// CHECK-NEXT:  a,b,x,y,z,c,d
var a = ['a',,'b',,,'c'];
print(a.splice(0, 3, 'x', 'y'), a);
// CHECK-NEXT: a,,b x,y,,,c

print('unshift');
// CHECK-LABEL: unshift
var a = [1,2,3];
print(a.unshift('a', 'b'), a);
// CHECK-NEXT: 5 a,b,1,2,3
print(a.unshift(), a);
// CHECK-NEXT: 5 a,b,1,2,3
print(a.unshift('c'), a);
// CHECK-NEXT: 6 c,a,b,1,2,3
var a = [];
print(a.unshift(1,2,3), a);
// CHECK-NEXT: 3 1,2,3
var a = [];
print(a.unshift(0,0,0,0,0,0,0,0,0,0), a);
// CHECK-NEXT 10 0,0,0,0,0,0,0,0,0,0

print('indexOf');
// CHECK-LABEL: indexOf
var a = ['a','b','c','b','x'];
print(a.indexOf('b'));
// CHECK-NEXT: 1
print(a.indexOf('b', 0));
// CHECK-NEXT: 1
print(a.indexOf('b', -2));
// CHECK-NEXT: 3
print(a.indexOf('b', 2));
// CHECK-NEXT: 3
print(a.indexOf('b', -1));
// CHECK-NEXT: -1
print(a.indexOf('b', 4));
// CHECK-NEXT: -1
print(a.indexOf('d'));
// CHECK-NEXT: -1
var a = [];
print(a.indexOf('a', -1));
// CHECK-NEXT: -1
print(a.indexOf('a', 4));
// CHECK-NEXT: -1
print(a.indexOf('a'));
// CHECK-NEXT: -1
print(1 / [true].indexOf(true, -0));
//CHECK-NEXT: Infinity

print('lastIndexOf');
// CHECK-LABEL: lastIndexOf
var a = ['a','b','c','b','x'];
print(a.lastIndexOf('b'));
// CHECK-NEXT: 3
print(a.lastIndexOf('b', 0));
// CHECK-NEXT: -1
print(a.lastIndexOf('b', -2));
// CHECK-NEXT: 3
print(a.lastIndexOf('b', 2));
// CHECK-NEXT: 1
print(a.lastIndexOf('b', -1));
// CHECK-NEXT: 3
print(a.lastIndexOf('b', 4));
// CHECK-NEXT: 3
print(a.lastIndexOf('d'));
// CHECK-NEXT: -1
var a = [];
print(a.lastIndexOf('a', -1));
// CHECK-NEXT: -1
print(a.lastIndexOf('a', 4));
// CHECK-NEXT: -1
print(a.lastIndexOf('a'));
// CHECK-NEXT: -1

print('every');
// CHECK-LABEL: every
print([1,2,3].every(function(x) {return x < 4}));
// CHECK-NEXT: true
print([1,2,3].every(function(x) {return x < 2}));
// CHECK-NEXT: false
print([].every(function(x) {return false}));
// CHECK-NEXT: true
print(['!', '@', '#'].every(function(x, i, arr) {
  print(this, x, i, arr);
  return x === '!';
}, 'thisval'));
// CHECK-NEXT: thisval ! 0 !,@,#
// CHECK-NEXT: thisval @ 1 !,@,#
// CHECK-NEXT: false
try {[1].every(1)} catch(e) {print('caught', e.name, e.message);}
// CHECK-NEXT: caught TypeError {{.*}}

print('some');
// CHECK-LABEL: some
print([1,2,3].some(function(x) {return x < 4}));
// CHECK-NEXT: true
print([1,2,3].some(function(x) {return x < 2}));
// CHECK-NEXT: true
print([1,2,3].some(function(x) {return x < 0}));
// CHECK-NEXT: false
print([].some(function(x) {return false}));
// CHECK-NEXT: false
print(['!', '@', '#'].some(function(x, i, arr) {
  print(this, x, i, arr);
  return x === '!';
}, 'thisval'));
// CHECK-NEXT: thisval ! 0 !,@,#
// CHECK-NEXT: true

print('foreach');
// CHECK-LABEL: foreach
print(['!', '@', '#'].forEach(function(x, i, arr) {
  print(this, x, i, arr);
  return x === '!';
}));
// CHECK-NEXT: undefined ! 0 !,@,#
// CHECK-NEXT: undefined @ 1 !,@,#
// CHECK-NEXT: undefined # 2 !,@,#
// CHECK-NEXT: undefined
print(['!', '@', '#'].forEach(function(x, i, arr) {
  print(this, x, i, arr);
  return x === '!';
}, 'thisval'));
// CHECK-NEXT: thisval ! 0 !,@,#
// CHECK-NEXT: thisval @ 1 !,@,#
// CHECK-NEXT: thisval # 2 !,@,#
// CHECK-NEXT: undefined
try {[1].forEach(1)} catch(e) {print('caught', e.name, e.message);}
// CHECK-NEXT: caught TypeError {{.*}}

print('map');
// CHECK-LABEL: map
print(['!', '@', '#'].map(function(x, i, arr) {
  print(this, x, i, arr);
  return x === '!';
}));
// CHECK-NEXT: undefined ! 0 !,@,#
// CHECK-NEXT: undefined @ 1 !,@,#
// CHECK-NEXT: undefined # 2 !,@,#
// CHECK-NEXT: true,false,false
print(['!', '@', '#'].map(function(x, i, arr) {
  print(this, x, i, arr);
  return x === '!';
}, 'thisval'));
// CHECK-NEXT: thisval ! 0 !,@,#
// CHECK-NEXT: thisval @ 1 !,@,#
// CHECK-NEXT: thisval # 2 !,@,#
// CHECK-NEXT: true,false,false
try {[1].map(1)} catch(e) {print('caught', e.name, e.message);}
// CHECK-NEXT: caught TypeError {{.*}}

print('filter');
// CHECK-LABEL: filter
var a = [1,2,3];
print(a.filter(function(x) {return x === 2}));
// CHECK-NEXT: 2
print(a.filter(function(x) {return x !== 2}));
// CHECK-NEXT: 1,3
print(a.filter(function() {return true}));
// CHECK-NEXT: 1,2,3
print('empty', a.filter(function() {return false}));
// CHECK-NEXT: empty
var a = [];
print('empty', a.filter(function() {return true}));
// CHECK-NEXT: empty
print(['!', '@', '#'].filter(function(x, i, arr) {
  print(this, x, i, arr);
  return x === '!';
}, 'thisval'));
// CHECK-NEXT: thisval ! 0 !,@,#
// CHECK-NEXT: thisval @ 1 !,@,#
// CHECK-NEXT: thisval # 2 !,@,#
// CHECK-NEXT: !
var a = [null,2,3];
var acc = {get: function() {return 1000}};
Object.defineProperty(a, 0, acc);
print(a.filter(function(x) {return x === 1000}));
// CHECK-NEXT: 1000
var a = {};
a[0] = 1239;
a[3] = 18583;
a.length = 6;
print(Array.prototype.filter.call(a, function(x) {return x === 1239}));
// CHECK-NEXT: 1239
try {[1].filter(1)} catch(e) {print('caught', e.name, e.message);}
// CHECK-NEXT: caught TypeError {{.*}}

print('fill');
// CHECK-LABEL: fill
var a = Array(3);
a.fill();
print(a[0], a[1], a[2]);
// CHECK-NEXT: undefined undefined undefined
a.fill(1);
print(a[0], a[1], a[2]);
// CHECK-NEXT: 1 1 1
a.fill(2, 0, a.length);
print(a[0], a[1], a[2]);
// CHECK-NEXT: 2 2 2
a.fill(3, -1);
print(a[0], a[1], a[2]);
// CHECK-NEXT: 2 2 3
a.fill(4, 1, -1);
print(a[0], a[1], a[2]);
// CHECK-NEXT: 2 4 3
var a = {};
a[0] = 1;
a[3] = 2;
a.length = 6;
Array.prototype.fill.call(a, 0);
print(a[0], a[1], a[2], a[3], a[4], a[5]);
// CHECK-NEXT: 0 0 0 0 0 0

print('Pop from large array');
// CHECK-LABEL: Pop from large array
// This caused a crash with SegmentedArray used as the backing storage.
// It was due to an off-by-one error in SegmentedArray::decreaseSize.
var a = Array(4097).fill();
// This pop causes the backing storage to shrink to only be inline storage.
a.pop();
print(a[a.length - 1]);
// CHECK-NEXT: undefined
a = Array(5121).fill();
// This pop causes the backing storage to shrink to one segment instead of 2.
a.pop();
print(a[a.length - 1]);
// CHECK-NEXT: undefined

print('find');
// CHECK-LABEL: find
print(Array.prototype.find.length);
// CHECK-NEXT: 1
print([].find(function(){}));
// CHECK-NEXT: undefined
print([1,2,3].find(function(v){ return v === 4 }));
// CHECK-NEXT: undefined
print([0,1,2].find(function(v){ return v }));
// CHECK-NEXT: 1
print(['a','b','c'].find(function(v, k, obj) {
  print(this, v, k, obj);
  return v === 'b';
}, 'thisarg'));
// CHECK-NEXT: thisarg a 0 a,b,c
// CHECK-NEXT: thisarg b 1 a,b,c
// CHECK-NEXT: b
print(Array.prototype.find.call(
  {0:'a',1:'b',length:3},
  function(v) { return v === 'b'; }
));
// CHECK-NEXT: b

print('findIndex');
// CHECK-LABEL: findIndex
print(Array.prototype.findIndex.length);
// CHECK-NEXT: 1
print([].findIndex(function(){}));
// CHECK-NEXT: -1
print([1,2,3].findIndex(function(v){ return v === 4 }));
// CHECK-NEXT: -1
print([0,1,2].findIndex(function(v){ return v }));
// CHECK-NEXT: 1
print(['a','b','c'].findIndex(function(v, k, obj) {
  print(this, v, k, obj);
  return v === 'b';
}, 'thisarg'));
// CHECK-NEXT: thisarg a 0 a,b,c
// CHECK-NEXT: thisarg b 1 a,b,c
// CHECK-NEXT: 1
print(Array.prototype.findIndex.call(
  {0:'a',1:'b',length:3},
  function(v) { return v === 'b'; }
));
// CHECK-NEXT: 1

print('findLast');
// CHECK-LABEL: findLast
print(Array.prototype.findLast.length);
// CHECK-NEXT: 1
print([].findLast(function(){}));
// CHECK-NEXT: undefined
print([1,2,3].findLast(function(v){ return v === 4 }));
// CHECK-NEXT: undefined
print([0,1,2].findLast(function(v){ return v }));
// CHECK-NEXT: 2
print(['a','b','c'].findLast(function(v, k, obj) {
  print(this, v, k, obj);
  return v === 'b';
}, 'thisarg'));
// CHECK-NEXT: thisarg c 2 a,b,c
// CHECK-NEXT: thisarg b 1 a,b,c
// CHECK-NEXT: b
print(Array.prototype.findLast.call(
  {0:'a',1:'b',length:3},
  function(v) { return v === 'b'; }
));
// CHECK-NEXT: b
print([3,3,3].findLast(function(v){ return v === 3 }));
// CHECK-NEXT: 3

print('findLastIndex');
// CHECK-LABEL: findLastIndex
print(Array.prototype.findLastIndex.length);
// CHECK-NEXT: 1
print([].findLastIndex(function(){}));
// CHECK-NEXT: -1
print([1,2,3].findLastIndex(function(v){ return v === 4 }));
// CHECK-NEXT: -1
print([0,1,2].findLastIndex(function(v){ return v }));
// CHECK-NEXT: 2
print(['a','b','c'].findLastIndex(function(v, k, obj) {
  print(this, v, k, obj);
  return v === 'b';
}, 'thisarg'));
// CHECK-NEXT: thisarg c 2 a,b,c
// CHECK-NEXT: thisarg b 1 a,b,c
// CHECK-NEXT: 1
print(Array.prototype.findLastIndex.call(
  {0:'a',1:'b',length:3},
  function(v) { return v === 'b'; }
));
// CHECK-NEXT: 1
print([3,3,3].findLastIndex(function(v){ return v === 3 }));
// CHECK-NEXT: 2

print('reduce');
// CHECK-LABEL: reduce
var a = [1,2,3,4];
print(a.reduce(function(x,y) {return x + y}));
// CHECK-NEXT: 10
print(a.reduce(function(x,y) {return x + y}, 100));
// CHECK-NEXT: 110
print(a.reduce(function(x,y) {return x * y}));
// CHECK-NEXT: 24
var a = [];
print(a.reduce(function() {}, 10));
// CHECK-NEXT: 10
var a = [,,,,1,2,,,,3,,4,,,];
print(a.reduce(function(x,y) {return x + y}));
// CHECK-NEXT: 10
print(a.reduce(function(x,y) {return x + y}, 100));
// CHECK-NEXT: 110
print(['!', '@', '#'].reduce(function(x, y, i, arr) {
  print(this, x, y, i, arr);
  return x + y;
}, '**'));
// CHECK-NEXT: undefined ** ! 0 !,@,#
// CHECK-NEXT: undefined **! @ 1 !,@,#
// CHECK-NEXT: undefined **!@ # 2 !,@,#
// CHECK-NEXT: **!@#
try {[1].reduce(1)} catch(e) {print('caught', e.name, e.message);}
// CHECK-NEXT: caught TypeError {{.*}}
var a = Array(10);
try {print(a.reduce(function(){}));} catch(e) {print('caught', e.name, e.message)}
// CHECK-NEXT: caught TypeError {{.*}}

print('reduceRight');
// CHECK-LABEL: reduceRight
var a = [1,2,3,4];
print(a.reduceRight(function(x,y) {return x + y}));
// CHECK-NEXT: 10
print(a.reduceRight(function(x,y) {return x + y}, 100));
// CHECK-NEXT: 110
print(a.reduceRight(function(x,y) {return x * y}));
// CHECK-NEXT: 24
var a = [];
print(a.reduceRight(function() {}, 10));
// CHECK-NEXT: 10
var a = [,,,,1,2,,,,3,,4,,,];
print(a.reduceRight(function(x,y) {return x + y}));
// CHECK-NEXT: 10
print(a.reduceRight(function(x,y) {return x + y}, 100));
// CHECK-NEXT: 110
print(['!', '@', '#'].reduceRight(function(x, y, i, arr) {
  print(this, x, y, i, arr);
  return x + y;
}, '**'));
// CHECK-NEXT: undefined ** # 2 !,@,#
// CHECK-NEXT: undefined **# @ 1 !,@,#
// CHECK-NEXT: undefined **#@ ! 0 !,@,#
// CHECK-NEXT: **#@!
var a = Array(10);
try {print(a.reduceRight(function(){}));} catch(e) {print('caught', e.name, e.message)}
// CHECK-NEXT: caught TypeError {{.*}}
try {[1].reduceRight(1)} catch(e) {print('caught', e.name, e.message);}
// CHECK-NEXT: caught TypeError {{.*}}
try {print(a.reduce(function(){}));} catch(e) {print('caught', e.name, e.message)}
// CHECK-NEXT: caught TypeError {{.*}}

print('includes');
// CHECK-LABEL: includes
print([1,2,3,4].includes(1));
// CHECK-NEXT: true
print([1,2,3,4].includes(1, Infinity));
// CHECK-NEXT: false
print([1,2,3,4].includes(1, 1));
// CHECK-NEXT: false
print([1,2,3,4].includes(1, -2));
// CHECK-NEXT: false
print([1,2,3,4].includes(1, -4));
// CHECK-NEXT: true
print([1,2,3,4].includes(3));
// CHECK-NEXT: true
print([1,2,3,4].includes(4));
// CHECK-NEXT: true
print([1,2,3,4].includes(4, -1));
// CHECK-NEXT: true
print([1,2,3,4].includes(4, 3));
// CHECK-NEXT: true
print([1,2,3,4].includes({}));
// CHECK-NEXT: false
print([1,2,3,4].includes(10));
// CHECK-NEXT: false
print([].includes(10));
// CHECK-NEXT: false
print(Array.prototype.includes.call({length: 3, 0: 'a', 1: 'b', 2: 'c'}, 'a'));
// CHECK-NEXT: true
var o = {};
print([,,,o,,,].includes(o));
// CHECK-NEXT: true

print('flat');
// CHECK-LABEL: flat
print([].flat().length);
// CHECK-NEXT: 0
print(arrayEquals([1,2,3].flat(), [1,2,3]));
// CHECK-NEXT: true
print(arrayEquals([1,[2,3]].flat(), [1,2,3]));
// CHECK-NEXT: true
print(arrayEquals([1,[2,[3]]].flat(), [1,2,[3]]));
// CHECK-NEXT: true
print(arrayEquals([1,[2,[3,[4]]]].flat(0), [1,[2,[3,[4]]]]));
// CHECK-NEXT: true
print(arrayEquals([1,[2,[3,[4]]]].flat(), [1,2,[3,[4]]]));
// CHECK-NEXT: true
print(arrayEquals([1,[2,[3,[4]]]].flat(1), [1,2,[3,[4]]]));
// CHECK-NEXT: true
print(arrayEquals([1,[2,[3,[4]]]].flat(2), [1,2,3,[4]]));
// CHECK-NEXT: true
print(arrayEquals([1,[2,[3,[4]]]].flat(Infinity), [1,2,3,4]));
// CHECK-NEXT: true

print('flatMap');
// CHECK-LABEL: flatMap
print([].flatMap(() => { throw Error('fail') }).length);
// CHECK-NEXT: 0
print(arrayEquals([1,2,3].flatMap(x => x+1), [2,3,4]));
// CHECK-NEXT: true
print(arrayEquals([1,2,3].flatMap(function(x) {
  return x + this;
}, 100), [101, 102, 103]));
// CHECK-NEXT: true
print(arrayEquals([1,2,3].flatMap(function(x) {
  return [x, x+this];
}, 100), [1,101,2,102,3,103]));
// CHECK-NEXT: true

print([1, 2, 3, 4, 5].at(1));
// CHECK-NEXT: 2
print(Array.prototype.at.call({length: 3, 0: 'a', 1: 'b', 2: 'c'}, 1));
// CHECK-NEXT: b
print([1, 2, 3, 4, 5].at(6));
// CHECK-NEXT: undefined
print(Array.prototype.at.call({length: 0, 0: 'a', 1: 'b', 2: 'c'}, 1));
// CHECK-NEXT: undefined
try { [].at(1n); } catch(e) { print(e.name) }
// CHECK-NEXT: TypeError
print([1, 2, 3, 4, 5].at(-1));
// CHECK-NEXT: 5
print([1, 2, 3, 4, 5].at(-5));
// CHECK-NEXT: 1
print([1, 2, 3, 4, 5].at(-6));
// CHECK-NEXT: undefined
print(Array.prototype.at.call({length: 3, 0: 'a', 1: 'b', 2: 'c'}, -1));
// CHECK-NEXT: c
print(Array.prototype.at.call({length: 30}, 5));
// CHECK-NEXT: undefined

print('toReversed');
// CHECK-LABEL: toReversed
print(Array.prototype.toReversed.length);
// CHECK-NEXT: 0
var a = [1,2,3,4];
print(a.toReversed().toString())
// CHECK-NEXT: 4,3,2,1
print(a.toString())
// CHECK-NEXT: 1,2,3,4
print(arrayEquals([ 1, 2, 3 ].toReversed(), [ 3, 2, 1 ]));
// CHECK-NEXT: true
print(Array.prototype.toReversed.call({length : 3, 0 : 'a', 1 : 'b', 2 : 'c'})
          .toString())
// CHECK-NEXT: c,b,a

print('toSpliced');
// CHECK-LABEL: toSpliced
print(Array.prototype.toSpliced.length);
// CHECK-NEXT: 2
var a = [ 1, 2, 3, 4 ];
print(a.toSpliced(0, 3, 7, 6, 5).toString())
// CHECK-NEXT: 7,6,5,4
print(a.toString())
// CHECK-NEXT: 1,2,3,4
print(arrayEquals([ 1, 2, 3, 4 ].toSpliced(), [ 1, 2, 3, 4 ]));
// CHECK-NEXT: true
print(arrayEquals([ 1, 2, 3, 4 ].toSpliced(2), [ 1, 2 ]));
// CHECK-NEXT: true
print(arrayEquals([ 1, 2, 3, 4 ].toSpliced(2, 2), [ 1, 2 ]));
// CHECK-NEXT: true
print(arrayEquals([ 1, 2, 3, 4 ].toSpliced(2, 1), [ 1, 2, 4 ]));
// CHECK-NEXT: true
print(arrayEquals([ 1, 2, 3, 4 ].toSpliced(2, 1, -3), [ 1, 2, -3, 4 ]));
// CHECK-NEXT: true
print(arrayEquals(
    [ 1, 2, 3, 4 ].toSpliced(2, 1, '3a', '3b', '3c'),
    [ 1, 2, '3a', '3b', '3c', 4 ]));
// CHECK-NEXT: true
print(arrayEquals([ 1, 2, 3, 4 ].toSpliced(2, 110, 'end'), [ 1, 2, 'end' ]));
// CHECK-NEXT: true
print(arrayEquals(
    [ 1, 2, 3, 4 ].toSpliced(2, -110, 'notend'), [ 1, 2, 'notend', 3, 4 ]));
// CHECK-NEXT: true
print(Array.prototype
          .toSpliced.call({length : 3, 0 : 'a', 1 : 'b', 2 : 'c'}, 0, 0, 0)
          .toString())
// CHECK-NEXT: 0,a,b,c

print('with');
// CHECK-LABEL: with
print(Array.prototype.with.length);
// CHECK-NEXT: 2
var a = [1,2,3,4];
print(a.with(0, 0).toString())
// CHECK-NEXT: 0,2,3,4
print(a.toString())
// CHECK-NEXT: 1,2,3,4
print(arrayEquals([ 1, 2, 3 ].with(0, 0), [ 0, 2, 3 ]));
// CHECK-NEXT: true
print(arrayEquals([ 1, 2, 3 ].with(1, 0), [ 1, 0, 3 ]));
// CHECK-NEXT: true
print(arrayEquals([ 1, 2, 3 ].with(-1, 30), [ 1, 2, 30 ]));
// CHECK-NEXT: true
print(Array.prototype.with.call({length : 3, 0 : 'a', 1 : 'b', 2 : 'c'}, 1, 'B')
          .toString())
// CHECK-NEXT: a,B,c
try {
  [].with(1, 1);
} catch (e) {
  print(e.name)
}
// CHECK-NEXT: RangeError
try {
  [].with(-2, 1);
} catch (e) {
  print(e.name)
}
// CHECK-NEXT: RangeError
var obj = {length: 2 ** 32};
try { Array.prototype.with.call(obj, 0, 123) } catch (e) { print(e.name) }
// CHECK-NEXT: RangeError

print('toSorted');
// CHECK-LABEL: toSorted
print(Array.prototype.toSorted.length);
// CHECK-NEXT: 1
try {
  [].toSorted(1);
} catch (e) {
  print(e.name)
}
// CHECK-NEXT: TypeError
var a = [ 0, 2, 1, 3 ];
print(a.toSorted().toString())
// CHECK-NEXT: 0,1,2,3
print(a.toString())
// CHECK-NEXT: 0,2,1,3
print(arrayEquals([ 'aa', 'a', 'aaa', 0 ].toSorted(), [ 0, 'a', 'aa', 'aaa' ]));
// CHECK-NEXT: true
print(arrayEquals(
    [ 'aa', 'a', 'aaa' ].toSorted(function(a, b) {
      return b.length - a.length;
    }),
    [ 'aaa', 'aa', 'a' ]));
// CHECK-NEXT: true
print(Array.prototype.toSorted.call({length : 3, 0 : 'b', 1 : 'c', 2 : 'a'})
          .toString());
// CHECK-NEXT: a,b,c
