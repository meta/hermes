/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// Array.prototype.slice copies the storage of arrays that pass
// arrayFastPathCheck() directly. Check that the result is the same as the
// generic path, including when the arguments run user code that changes the
// array, and that everything else still takes the generic path.
// Index-like properties on Array.prototype or Object.prototype disable the
// fast path, possibly for the rest of the run, so they are tested last.
// array-slice-fastpath-large.js tests a result with separate storage.

// Describe array a: its length, its elements (holes as '<hole>', objects as
// 'obj') and whether its prototype is Array.prototype. This does not create
// arrays, so it works with any index-like properties on the prototypes.
function d(a) {
  if (!Array.isArray(a)) return 'notarray:' + String(a);
  var s = '';
  for (var i = 0; i < a.length; i++) {
    if (i) s += ',';
    if (!(i in a)) s += '<hole>';
    else if (typeof a[i] === 'object' && a[i] !== null) s += 'obj';
    else s += String(a[i]);
  }
  var isArr = Object.getPrototypeOf(a) === Array.prototype;
  return 'len=' + a.length + ' [' + s + '] proto=' + isArr;
}

// Print name and the description of the result of f(), or the exception
// that f() throws.
function t(name, f) {
  try {
    print(name + ': ' + d(f()));
  } catch (e) {
    print(name + ': EXC ' + e.name + ': ' + e.message);
  }
}

print('ranges');
// CHECK-LABEL: ranges
var base = [1, 2.5, 'x', null, undefined, {}, true, -0, 1e300, NaN];
t('all', () => base.slice());
// CHECK-NEXT: all: len=10 [1,2.5,x,null,undefined,obj,true,0,1e+300,NaN] proto=true
t('start', () => base.slice(3));
// CHECK-NEXT: start: len=7 [null,undefined,obj,true,0,1e+300,NaN] proto=true
t('neg', () => base.slice(-3));
// CHECK-NEXT: neg: len=3 [0,1e+300,NaN] proto=true
t('negneg', () => base.slice(-5, -2));
// CHECK-NEXT: negneg: len=3 [obj,true,0] proto=true
t('range', () => base.slice(2, 5));
// CHECK-NEXT: range: len=3 [x,null,undefined] proto=true
t('reversed', () => base.slice(5, 2));
// CHECK-NEXT: reversed: len=0 [] proto=true
t('past end', () => base.slice(100));
// CHECK-NEXT: past end: len=0 [] proto=true
t('inf', () => base.slice(-Infinity, Infinity));
// CHECK-NEXT: inf: len=10 [1,2.5,x,null,undefined,obj,true,0,1e+300,NaN] proto=true
t('huge', () => base.slice(-1e300, 1e300));
// CHECK-NEXT: huge: len=10 [1,2.5,x,null,undefined,obj,true,0,1e+300,NaN] proto=true
t('frac', () => base.slice(1.7, 4.2));
// CHECK-NEXT: frac: len=3 [2.5,x,null] proto=true
t('strings', () => base.slice('2', '4'));
// CHECK-NEXT: strings: len=2 [x,null] proto=true
t('nan', () => base.slice(NaN, NaN));
// CHECK-NEXT: nan: len=0 [] proto=true
t('negzero', () => base.slice(-0, -0));
// CHECK-NEXT: negzero: len=0 [] proto=true
t('undef end', () => base.slice(2, undefined));
// CHECK-NEXT: undef end: len=8 [x,null,undefined,obj,true,0,1e+300,NaN] proto=true
t('empty', () => [].slice());
// CHECK-NEXT: empty: len=0 [] proto=true
t('one', () => [{}].slice());
// CHECK-NEXT: one: len=1 [obj] proto=true
t('bigint arg', () => base.slice(1n));
// CHECK-NEXT: bigint arg: EXC TypeError: Cannot convert BigInt to number
t('symbol arg', () => base.slice(Symbol()));
// CHECK-NEXT: symbol arg: EXC TypeError: Cannot convert Symbol to number

print('values');
// CHECK-LABEL: values
// Values must be copied exactly, including the ones that are boxed in some
// configurations.
var vals = [-0, NaN, 0.1, 2 ** 31, -(2 ** 40), 5e-324, 'str', null, true];
var vc = vals.slice();
var same = vc.length === vals.length;
for (var i = 0; i < vals.length; i++) same = same && Object.is(vc[i], vals[i]);
print(same);
// CHECK-NEXT: true
var o1 = {}, o2 = [], sym = Symbol('s');
var oc = [o1, o2, sym, 10n].slice();
print(oc[0] === o1, oc[1] === o2, oc[2] === sym, oc[3] === 10n);
// CHECK-NEXT: true true true true
// Searches must compare boxed numbers by value.
var dc = [1.5, NaN, 2 ** 40, 0.1, 2 ** 53 - 1].slice();
print(dc.includes(NaN), dc.indexOf(NaN), dc.indexOf(2 ** 40), dc.indexOf(1.5));
// CHECK-NEXT: true -1 2 0
print(dc.indexOf(0.1), dc.includes(2 ** 53 - 1), dc.lastIndexOf(2 ** 53 - 1));
// CHECK-NEXT: 3 true 4

print('independence');
// CHECK-LABEL: independence
var src = [1, 2, 3];
var cp = src.slice();
cp[0] = 9;
src[1] = 8;
print(src.join(), cp.join());
// CHECK-NEXT: 1,8,3 9,2,3
cp.push(4);
print(cp.join(), cp.length);
// CHECK-NEXT: 9,2,3,4 4
cp.length = 1;
print(cp.join(), cp.length);
// CHECK-NEXT: 9 1
// These doubles are boxed in some configurations.
var dbl = [1.1, 2.2, 3.3];
var dcp = dbl.slice();
dbl[0] += 1;
dcp[1] += 10;
print(dbl.join(), dcp.join());
// CHECK-NEXT: 2.1,2.2,3.3 1.1,12.2,3.3
var fr = Object.freeze([1, 2]);
var fcp = fr.slice();
fcp.push(3);
fcp[0] = 9;
print(fcp.join(), Object.isFrozen(fcp), Object.isExtensible(fcp));
// CHECK-NEXT: 9,2,3 false true

print('holes');
// CHECK-LABEL: holes
var holes = [1, , 3, , 5];
t('holes', () => holes.slice());
// CHECK-NEXT: holes: len=5 [1,<hole>,3,<hole>,5] proto=true
t('holes range', () => holes.slice(1, 4));
// CHECK-NEXT: holes range: len=3 [<hole>,3,<hole>] proto=true
var tail = [1, 2, 3];
tail.length = 6;
t('length raised', () => tail.slice(2));
// CHECK-NEXT: length raised: len=4 [3,<hole>,<hole>,<hole>] proto=true
t('Array(3)', () => new Array(3).slice());
// CHECK-NEXT: Array(3): len=3 [<hole>,<hole>,<hole>] proto=true

print('array properties');
// CHECK-LABEL: array properties
var named = [1, 2, 3];
named.foo = 'bar';
t('named property', () => named.slice(1));
// CHECK-NEXT: named property: len=2 [2,3] proto=true
var roLength = [1, , 3];
Object.defineProperty(roLength, 'length', {writable: false});
t('read-only length', () => roLength.slice(0, 2));
// CHECK-NEXT: read-only length: len=2 [1,<hole>] proto=true
var getter = [1, 2, 3];
Object.defineProperty(getter, 1, {get: () => 'G'});
t('getter', () => getter.slice());
// CHECK-NEXT: getter: len=3 [1,G,3] proto=true
var setter = [1, 2, 3];
Object.defineProperty(setter, 1, {set: v => {}});
t('setter only', () => setter.slice());
// CHECK-NEXT: setter only: len=3 [1,undefined,3] proto=true
var readOnly = [1, 2, 3];
Object.defineProperty(readOnly, 1, {value: 'RO', writable: false});
t('read-only element', () => readOnly.slice());
// CHECK-NEXT: read-only element: len=3 [1,RO,3] proto=true
t('frozen', () => Object.freeze([1, 2, 3]).slice(1));
// CHECK-NEXT: frozen: len=2 [2,3] proto=true
t('sealed', () => Object.seal([1, , 3]).slice());
// CHECK-NEXT: sealed: len=3 [1,<hole>,3] proto=true
t('non-extensible', () => Object.preventExtensions([1, , 3]).slice());
// CHECK-NEXT: non-extensible: len=3 [1,<hole>,3] proto=true
var shifted = [1, 2, 3, 4];
shifted.shift();
t('shift', () => shifted.slice());
// CHECK-NEXT: shift: len=3 [2,3,4] proto=true
var unshifted = [3, 4];
unshifted.unshift(1, 2);
t('unshift', () => unshifted.slice(1));
// CHECK-NEXT: unshift: len=3 [2,3,4] proto=true
var sparse = [];
sparse[100000] = 1;
sparse[5] = 2;
t('sparse', () => sparse.slice(4, 7));
// CHECK-NEXT: sparse: len=3 [<hole>,2,<hole>] proto=true
var withProto = [1, , 3];
Object.setPrototypeOf(withProto, {1: 'NP', __proto__: Array.prototype});
t('own prototype', () => withProto.slice());
// CHECK-NEXT: own prototype: len=3 [1,NP,3] proto=true
var nullProto = [1, , 3];
Object.setPrototypeOf(nullProto, null);
t('null prototype', () => Array.prototype.slice.call(nullProto));
// CHECK-NEXT: null prototype: len=3 [1,<hole>,3] proto=true
// Being the prototype of another object does not disqualify an array, so
// this case takes the fast path.
var protoOfObject = [1, , 3];
var child = Object.create(protoOfObject);
child.x = 1;
t('prototype of an object', () => protoOfObject.slice());
// CHECK-NEXT: prototype of an object: len=3 [1,<hole>,3] proto=true
t('Array.prototype', () => Array.prototype.slice());
// CHECK-NEXT: Array.prototype: len=0 [] proto=true

print('user code');
// CHECK-LABEL: user code
// ToIntegerOrInfinity() of the arguments may run user code that changes the
// array after its length was read.
var a1 = [1, 2, 3, 4, 5];
t('shrink', () => a1.slice({valueOf() { a1.length = 2; return 0; }}));
// CHECK-NEXT: shrink: len=5 [1,2,<hole>,<hole>,<hole>] proto=true
var a2 = [1, 2, 3, 4, 5];
t('shrink in end', () =>
  a2.slice(0, {valueOf() { a2.length = 1; return 5; }}));
// CHECK-NEXT: shrink in end: len=5 [1,<hole>,<hole>,<hole>,<hole>] proto=true
var a3 = [1, 2, 3];
t('grow', () => a3.slice({valueOf() { a3.push(4, 5, 6); return 0; }}));
// CHECK-NEXT: grow: len=3 [1,2,3] proto=true
var a4 = [1, 2, 3];
t('replace', () =>
  a4.slice({valueOf() { a4[0] = 'r'; a4[2] = 'z'; return 0; }}));
// CHECK-NEXT: replace: len=3 [r,2,z] proto=true
var a5 = [1, 2, 3];
t('make hole', () => a5.slice({valueOf() { delete a5[1]; return 0; }}));
// CHECK-NEXT: make hole: len=3 [1,<hole>,3] proto=true
var a6 = [1, 2, 3];
t('raise length', () =>
  a6.slice(0, {valueOf() { a6.length = 10; return 3; }}));
// CHECK-NEXT: raise length: len=3 [1,2,3] proto=true
var a7 = [1, 2, 3, 4];
t('shrink and regrow', () =>
  a7.slice({
    valueOf() {
      a7.length = 0;
      a7.push('a', 'b', 'c', 'd');
      return 1;
    },
  }));
// CHECK-NEXT: shrink and regrow: len=3 [b,c,d] proto=true
var a8 = [1, 2, 3, 4];
t('shrink and raise length', () =>
  a8.slice({valueOf() { a8.length = 1; a8.length = 4; return 0; }}));
// CHECK-NEXT: shrink and raise length: len=4 [1,<hole>,<hole>,<hole>] proto=true
var a9 = [1, 2, 3];
t('add getter', () =>
  a9.slice({
    valueOf() {
      Object.defineProperty(a9, 2, {get: () => 'g'});
      return 0;
    },
  }));
// CHECK-NEXT: add getter: len=3 [1,2,g] proto=true
var a10 = [1, , 3];
t('set prototype', () =>
  a10.slice({
    valueOf() {
      Object.setPrototypeOf(a10, {1: 'np', __proto__: Array.prototype});
      return 0;
    },
  }));
// CHECK-NEXT: set prototype: len=3 [1,np,3] proto=true
var a11 = [1, , 3];
t('null prototype', () =>
  a11.slice({valueOf() { Object.setPrototypeOf(a11, null); return 0; }}));
// CHECK-NEXT: null prototype: len=3 [1,<hole>,3] proto=true
var a12 = [1, , 3];
var protoProxy = new Proxy(Array.prototype, {
  get: (t, k, r) => (k === '1' ? 'PX' : Reflect.get(t, k, r)),
  has: (t, k) => k === '1' || Reflect.has(t, k),
});
t('proxy prototype', () =>
  a12.slice({
    valueOf() {
      Object.setPrototypeOf(a12, protoProxy);
      return 0;
    },
  }));
// CHECK-NEXT: proxy prototype: len=3 [1,PX,3] proto=true
var a13 = [1, 2, 3];
t('freeze', () => a13.slice({valueOf() { Object.freeze(a13); return 1; }}));
// CHECK-NEXT: freeze: len=2 [2,3] proto=true
var a14 = [1, 2, 3];
t('sparse', () =>
  a14.slice({valueOf() { a14[100000] = 9; return 0; }}, 3));
// CHECK-NEXT: sparse: len=3 [1,2,3] proto=true
t('throw', () =>
  [1, 2].slice({valueOf() { throw new TypeError('boom'); }}));
// CHECK-NEXT: throw: EXC TypeError: boom

print('generic');
// CHECK-LABEL: generic
var slice = Array.prototype.slice;
t('array-like', () => slice.call({length: 3, 0: 'a', 2: 'c'}));
// CHECK-NEXT: array-like: len=3 [a,<hole>,c] proto=true
t('arguments', () =>
  (function () {
    return slice.call(arguments, 1);
  })(1, 2, 3));
// CHECK-NEXT: arguments: len=2 [2,3] proto=true
t('string', () => slice.call('abc', 1));
// CHECK-NEXT: string: len=2 [b,c] proto=true
t('typed array', () => slice.call(new Int8Array([1, 2, 3])));
// CHECK-NEXT: typed array: len=3 [1,2,3] proto=true
t('primitive', () => slice.call(5));
// CHECK-NEXT: primitive: len=0 [] proto=true
t('undefined', () => slice.call(undefined));
// CHECK-NEXT: undefined: EXC TypeError: Cannot convert undefined value to object
var proxy = new Proxy([1, 2, 3], {
  get: (t, k) => (k === 'length' ? 3 : 'p' + String(k)),
});
t('proxy', () => slice.call(proxy));
// CHECK-NEXT: proxy: len=3 [p0,p1,p2] proto=true
class MyArray extends Array {}
var mine = MyArray.from([1, 2, 3]);
t('subclass', () => mine.slice(1));
// CHECK-NEXT: subclass: len=2 [2,3] proto=true
// ArraySpeciesCreate is not implemented, so the result is a plain Array. If it
// is implemented, the fast path must check the species too.
var species = [1, 2, 3];
species.constructor = {[Symbol.species]: function () { return {}; }};
t('species', () => species.slice(1));
// CHECK-NEXT: species: len=2 [2,3] proto=true
t('2^53-1 length', () =>
  slice.call({length: 2 ** 53 - 1, [2 ** 53 - 2]: 'last'}, 2 ** 53 - 2));
// CHECK-NEXT: 2^53-1 length: len=1 [last] proto=true
t('range error', () => slice.call({length: 2 ** 40}));
// CHECK-NEXT: range error: EXC RangeError: invalid array length

print('gc stress');
// CHECK-LABEL: gc stress
var mixed = [];
for (var i = 0; i < 1000; i++) {
  mixed.push(i % 3 === 0 ? {v: i} : i % 3 === 1 ? i + 0.5 : 's' + i);
}
var total = 0;
for (var j = 0; j < 1000; j++) {
  var c = mixed.slice(j % 500, 500 + (j % 500));
  var junk = {a: j, b: [j, j + 0.5]};
  total += c.length + (typeof c[0] === 'object' ? c[0].v : 0);
}
print(total);
// CHECK-NEXT: 583166

print('prototype chain');
// CHECK-LABEL: prototype chain
// Holes must expose inherited index-like properties. Setting the length of
// Array.prototype to 0 removes its indexed storage again.
Array.prototype[1] = 'AP';
t('Array.prototype index', () => holes.slice());
// CHECK-NEXT: Array.prototype index: len=5 [1,AP,3,<hole>,5] proto=true
delete Array.prototype[1];
Array.prototype.length = 0;
t('deleted', () => holes.slice());
// CHECK-NEXT: deleted: len=5 [1,<hole>,3,<hole>,5] proto=true
var h1 = [1, , 3];
t('Array.prototype index in valueOf', () =>
  h1.slice({valueOf() { Array.prototype[1] = 'AP'; return 0; }}));
// CHECK-NEXT: Array.prototype index in valueOf: len=3 [1,AP,3] proto=true
delete Array.prototype[1];
Array.prototype.length = 0;
t('deleted', () => h1.slice());
// CHECK-NEXT: deleted: len=3 [1,<hole>,3] proto=true
// An index-like property on Object.prototype or an accessor on
// Array.prototype may disable the fast path for the rest of the run.
var h2 = [1, , 3];
t('Object.prototype index in valueOf', () =>
  h2.slice({valueOf() { Object.prototype[1] = 'OP'; return 0; }}));
// CHECK-NEXT: Object.prototype index in valueOf: len=3 [1,OP,3] proto=true
delete Object.prototype[1];
t('deleted', () => h2.slice());
// CHECK-NEXT: deleted: len=3 [1,<hole>,3] proto=true
Object.prototype[3] = 'OP';
t('Object.prototype index', () => holes.slice());
// CHECK-NEXT: Object.prototype index: len=5 [1,<hole>,3,OP,5] proto=true
delete Object.prototype[3];
var getterCalls = 0;
Object.defineProperty(Array.prototype, 1, {
  get: function () {
    ++getterCalls;
    return 'G' + this.length;
  },
  configurable: true,
});
t('Array.prototype getter', () => holes.slice());
// CHECK-NEXT: Array.prototype getter: len=5 [1,G5,3,<hole>,5] proto=true
print('getter calls', getterCalls);
// CHECK-NEXT: getter calls 1
