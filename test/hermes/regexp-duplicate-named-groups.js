/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O -target=HBC %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -target=HBC %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -target=HBC -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -lazy -target=HBC %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// ES2025 duplicate named capturing groups: the same name may be used by
// several capture groups as long as they are in different alternatives, so
// that they can never both participate in a single match.

print('duplicate named capture groups');
// CHECK-LABEL: duplicate named capture groups

/// Print whether \p source is accepted as a RegExp, and the error if not.
function checkSyntax(source) {
  try {
    new RegExp(source);
    print(source, '-> ok');
  } catch (e) {
    print(source, '->', e.message);
  }
}

// The syntax rule itself is covered exhaustively by unittests/Support/RegexTest;
// this only checks that it surfaces through the RegExp constructor.
checkSyntax('(?<a>x)|(?<a>y)');
// CHECK-NEXT: (?<a>x)|(?<a>y) -> ok
checkSyntax('(?:(?<a>1)|(?<a>2))+');
// CHECK-NEXT: (?:(?<a>1)|(?<a>2))+ -> ok
checkSyntax('(?<a>x)(?<a>y)');
// CHECK-NEXT: (?<a>x)(?<a>y) -> Invalid RegExp: Duplicate capture group name
checkSyntax('(?=(?<a>x))(?<a>y)');
// CHECK-NEXT: (?=(?<a>x))(?<a>y) -> Invalid RegExp: Duplicate capture group name
// A name that is never defined is still an error.
checkSyntax('(?:(?<a>x)|(?<a>y))\\k<zz>');
// CHECK-NEXT: (?:(?<a>x)|(?<a>y))\k<zz> -> Invalid RegExp: Nonexistent named capture reference

print('groups object');
// CHECK-LABEL: groups object

// The groups object has one property per distinct name, holding the value of
// whichever group participated.
var date = /(?<year>\d{4})-(?<month>\d{2})|(?<month>\d{2})-(?<year>\d{4})/;
print(JSON.stringify(date.exec('2024-12').groups));
// CHECK-NEXT: {"year":"2024","month":"12"}
print(JSON.stringify(date.exec('12-2024').groups));
// CHECK-NEXT: {"year":"2024","month":"12"}

// Property order is first appearance in the pattern, whichever branch matched.
print(Object.keys(/(?:(?<b>1)|(?<b>2))(?<a>x)/.exec('2x').groups).join(','));
// CHECK-NEXT: b,a

// The numbered captures are unaffected: each group keeps its own slot.
var m = /(?<a>x)|(?<a>y)/.exec('y');
print(m.length, m[1], m[2]);
// CHECK-NEXT: 3 undefined y

// The groups object still has a null prototype.
print(Object.getPrototypeOf(m.groups));
// CHECK-NEXT: null

// If no group with the name participated, the property exists and is undefined.
var none = /(?:(?<a>x)|(?<a>y))?z/.exec('z');
print(Object.keys(none.groups).join(','), none.groups.a, 'a' in none.groups);
// CHECK-NEXT: a undefined true

// Only the last iteration of a quantified group is visible.
print(JSON.stringify(/(?:(?<a>1)|(?<a>2))+/.exec('12').groups));
// CHECK-NEXT: {"a":"2"}

print('indices');
// CHECK-LABEL: indices

var d = /(?:(?<a>x)|(?<a>y))/d.exec('__y');
print(JSON.stringify(d.indices.groups));
// CHECK-NEXT: {"a":[2,3]}
var dNone = /(?:(?<a>x)|(?<a>y))?z/d.exec('z');
print(Object.keys(dNone.indices.groups).join(','), dNone.indices.groups.a);
// CHECK-NEXT: a undefined

print('backreferences');
// CHECK-LABEL: backreferences

// \k<a> refers to whichever group named "a" participated.
var back = /^(?:(?<a>x)|(?<a>y))\k<a>$/;
print(back.test('xx'), back.test('yy'), back.test('xy'));
// CHECK-NEXT: true true false

// A backreference resolves against every group with the name, including ones
// defined later in the pattern.
var fwd = /^(?:(?<a>x)\k<a>|q(?<a>y)\k<a>)$/;
print(fwd.test('xx'), fwd.test('qyy'), fwd.test('qyx'));
// CHECK-NEXT: true true false

// If no group named "a" participated, the backreference matches empty.
print(/^(?:z|(?<a>x)|(?<a>y))\k<a>$/.test('z'));
// CHECK-NEXT: true

// Case-insensitive multi-group backreferences.
print(/^(?:(?<a>x)|(?<a>y))\k<a>$/i.test('xX'));
// CHECK-NEXT: true

// Without the unicode flag, \k is only a backreference if the pattern has named
// groups, which requires reparsing the whole pattern once they are found.
print(/\k<a>(?:(?<a>x)|(?<a>y))\k<a>$/.test('yy'));
// CHECK-NEXT: true

print('substitution');
// CHECK-LABEL: substitution

print('12-2024'.replace(/(?<y>\d{4})-\d{2}|\d{2}-(?<y>\d{4})/, '$<y>'));
// CHECK-NEXT: 2024
print('a-2b'.replace(/(?:(?<v>1)|(?<v>2))/, '[$<v>]'));
// CHECK-NEXT: a-[2]b
// The functional replacer receives the groups object as its last argument.
print('y'.replace(/(?<a>x)|(?<a>y)/, function () {
  return JSON.stringify(arguments[arguments.length - 1]);
}));
// CHECK-NEXT: {"a":"y"}

print('literals');
// CHECK-LABEL: literals

// Regexp literals carry their group name mapping separately from the pattern,
// so exercise a literal with both plain and duplicated names.
var lit = /(?<pre>a)(?<mid>-)(?:(?<v>1)|(?<v>2))(?<post>b)/.exec('a-2b');
print(Object.keys(lit.groups).join(','), JSON.stringify(lit.groups));
// CHECK-NEXT: pre,mid,v,post {"pre":"a","mid":"-","v":"2","post":"b"}

print('new RegExp');
// CHECK-LABEL: new RegExp

// new RegExp builds the group name mapping at runtime rather than taking it
// from the compiled literal, so read a shared name back through every consumer.
var dyn = new RegExp('(?<y>\\d{4})-(?<m>\\d{2})|(?<m>\\d{2})-(?<y>\\d{4})', 'd');
var dynMatch = dyn.exec('12-2024');
print(JSON.stringify(dynMatch.groups));
// CHECK-NEXT: {"y":"2024","m":"12"}
print(JSON.stringify(dynMatch.indices.groups));
// CHECK-NEXT: {"y":[3,7],"m":[0,2]}
print('12-2024'.replace(dyn, '$<y>/$<m>'));
// CHECK-NEXT: 2024/12
