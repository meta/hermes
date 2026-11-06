/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Walks that must not record (spec "The walk").
// get(): p two prototypes up from each of a receiver whose prototype is
// a dictionary, one whose prototype is a Proxy, and a dictionary
// receiver. None records a chain entry. The fourth receiver has p as an
// own property; its own entry is what makes version 2 compile at all,
// so its dump line can show `0 chain`.
// getName(): `name` two prototypes up from Object.create(mid) gives a
// chain entry (version 2 emits it; calls 17-20 of the first count()
// hit: the only 4 chain hits). Then fresh functions whose prototype is
// mid, so `name` is also two up (Function.prototype's own `name` is not
// on their chain), each still lazy when read: their own `name` wins.
// Then a Proxy receiver and a null-prototype receiver, whose classes
// match no entry, read through the helper.
// Fails if the walk admits a dictionary on the path or as the receiver
// (get's dump line would read 1 chain).
function main() {
  function get(o) {
    return o.p;
  }
  function getName(o) {
    return o.name;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % arr.length]);
    return s;
  }
  function count(f, arr, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(arr[i % arr.length]) === want)
        ++c;
    return c;
  }
  var dictMid = Object.create({p: 1});
  dictMid.t = 0;
  delete dictMid.t;
  var viaDict = Object.create(dictMid);
  var viaProxy = Object.create(new Proxy(Object.create({p: 2}), {}));
  var dictRecv = Object.create(Object.create({p: 3}));
  dictRecv.t = 0;
  delete dictRecv.t;
  print(drive(get, [viaDict, viaProxy, dictRecv, {p: 4}], 100));

  var mid = Object.create({name: 'top'});
  print(count(getName, [Object.create(mid)], 20, 'top'));
  var fns = 0;
  for (var k = 0; k < 20; ++k) {
    var f = function () {};
    Object.setPrototypeOf(f, mid);
    if (getName(f) === 'f')
      ++fns;
  }
  print(fns);
  print(getName(new Proxy({}, {get: function () { return 'trap'; }})));
  print(getName(Object.create(null)));
}
main();

// OUT: 250
// OUT-NEXT: 20
// OUT-NEXT: 20
// OUT-NEXT: trap
// OUT-NEXT: undefined
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getName' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getName' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// CNT: NumByIdChainHits: 4{{$}}
