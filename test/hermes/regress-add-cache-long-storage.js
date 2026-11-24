/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -lazy %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// JSON.parse of text with a duplicate key creates the object with property
// storage for every member, duplicates included, so the storage is longer
// than the object's class needs. A named add that hits the PutById add cache
// must store into the slot that already exists, not append past it.

'use strict';

/// \return the JSON text of an object with the members m0..m(n-1) holding
/// 0..n-1, followed by \p dups members repeating m0, m1, ... with the values
/// -1, -2, ...
function text(n, dups) {
  var parts = [];
  for (var i = 0; i < n; ++i) parts.push('"m' + i + '":' + i);
  for (var i = 0; i < dups; ++i) parts.push('"m' + i + '":' + -(i + 1));
  return '{' + parts.join(',') + '}';
}

/// \return the number of members m0..m(n-1) of \p o whose value is not the
/// one text(n, dups) gives it.
function wrongMembers(o, n, dups) {
  var bad = 0;
  for (var i = 0; i < n; ++i)
    if (o['m' + i] !== (i < dups ? -(i + 1) : i)) ++bad;
  return bad;
}

function addZz(o, v) {
  o.zz = v;
}
function addYy(o, v) {
  o.yy = v;
}
function addXx(o, v) {
  o.xx = v;
}
function addWw(o, v) {
  o.ww = v;
}

// One duplicate: the storage is one slot longer than the class needs and
// exactly full. The add of zz lands in that slot; appending instead writes
// past the end of the storage, into the array allocated after it.
print('one duplicate');
// CHECK-LABEL: one duplicate
(function () {
  var bad = 0;
  var last;
  for (var n = 7; n <= 40; ++n) {
    var s = text(n, 1);
    for (var r = 0; r < 20; ++r) {
      var o = JSON.parse(s);
      var after = [r, r + 1, r + 2];
      addZz(o, r);
      if (o.zz !== r) ++bad;
      if (after.length !== 3 || after[0] !== r || after[2] !== r + 2) ++bad;
      bad += wrongMembers(o, n, 1);
      last = o;
    }
    gc();
  }
  print(bad + ' ' + Object.keys(last).length + ' ' + last.zz);
  print(JSON.stringify(last));
})();
// CHECK-NEXT: 0 41 19
// CHECK-NEXT: {"m0":-1,"m1":1,"m2":2,"m3":3,"m4":4,"m5":5,"m6":6,"m7":7,"m8":8,"m9":9,"m10":10,"m11":11,"m12":12,"m13":13,"m14":14,"m15":15,"m16":16,"m17":17,"m18":18,"m19":19,"m20":20,"m21":21,"m22":22,"m23":23,"m24":24,"m25":25,"m26":26,"m27":27,"m28":28,"m29":29,"m30":30,"m31":31,"m32":32,"m33":33,"m34":34,"m35":35,"m36":36,"m37":37,"m38":38,"m39":39,"zz":19}

// Three duplicates: three adds land in the existing slots and the fourth
// grows the storage.
print('three duplicates');
// CHECK-LABEL: three duplicates
(function () {
  var bad = 0;
  var last;
  for (var n = 4; n <= 12; ++n) {
    var s = text(n, 3);
    for (var r = 0; r < 20; ++r) {
      var o = JSON.parse(s);
      addZz(o, r);
      addYy(o, r + 1);
      var after = [r, r + 1, r + 2];
      addXx(o, r + 2);
      addWw(o, r + 3);
      if (o.zz !== r || o.yy !== r + 1 || o.xx !== r + 2 || o.ww !== r + 3)
        ++bad;
      if (after.length !== 3 || after[0] !== r || after[2] !== r + 2) ++bad;
      bad += wrongMembers(o, n, 3);
      last = o;
    }
    gc();
  }
  print(bad + ' ' + Object.keys(last).join());
  print(JSON.stringify(last));
})();
// CHECK-NEXT: 0 m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11,zz,yy,xx,ww
// CHECK-NEXT: {"m0":-1,"m1":-2,"m2":-3,"m3":3,"m4":4,"m5":5,"m6":6,"m7":7,"m8":8,"m9":9,"m10":10,"m11":11,"zz":19,"yy":20,"xx":21,"ww":22}
