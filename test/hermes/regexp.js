/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: LC_ALL=en_US.UTF-8 %hermes -non-strict -O -target=HBC %s | %FileCheck --match-full-lines %s

print('RegExp');
// CHECK-LABEL: RegExp
print(RegExp.prototype.constructor === RegExp);
// CHECK-NEXT: true
print(new RegExp(new RegExp(), "m"));
// CHECK-NEXT: /(?:)/m
print(new RegExp(new RegExp("hello", "g")));
// CHECK-NEXT: /hello/g
print(new RegExp(/abc/g, "m"));
// CHECK-NEXT: /abc/m
print((new RegExp("abc")).toString());
// CHECK-NEXT: /abc/
print((new RegExp("abc", "g")).toString());
// CHECK-NEXT: /abc/g
print((new RegExp("abc", "mg")).toString());
// CHECK-NEXT: /abc/gm
print((new RegExp("abc", "mig")).toString());
// CHECK-NEXT: /abc/gim
print((new RegExp("abc", "yi")).toString());
// CHECK-NEXT: /abc/iy
try { RegExp.prototype.toString.call(undefined); } catch (err) { print(err.name); }
// CHECK-NEXT: TypeError
print(RegExp.prototype.toString.call({source: "lol"}));
// CHECK-NEXT: /lol/undefined
print(RegExp.prototype.toString.call({source: "definitely / not", flags:"a regexp"}));
// CHECK-NEXT: /definitely / not/a regexp

var objWithRegExpCons = {constructor: RegExp};
objWithRegExpCons[Symbol.match] = true;
print(objWithRegExpCons == RegExp(objWithRegExpCons));
// CHECK-NEXT: true
print(objWithRegExpCons == new RegExp(objWithRegExpCons));
// CHECK-NEXT: false
print(objWithRegExpCons == RegExp(objWithRegExpCons, 'g'));
// CHECK-NEXT: false

function createFromNotRegExp(matchValue) {
  var notRegExp = {};
  notRegExp[Symbol.match] = matchValue;
  print(new RegExp(notRegExp));
}
createFromNotRegExp(false);
// CHECK-NEXT: /[object Object]/
createFromNotRegExp(undefined);
// CHECK-NEXT: /[object Object]/
createFromNotRegExp(null);
// CHECK-NEXT: /[object Object]/
print(new RegExp(true));
// CHECK-NEXT: /true/
print(new RegExp(undefined));
// CHECK-NEXT: /(?:)/

function createFromRegExpLike(s, f, flags) {
  var regExpLike = {source: s, flags: f};
  regExpLike[Symbol.match] = true;
  print(new RegExp(regExpLike, flags));
}
createFromRegExpLike("hello", "g", undefined);
// CHECK-NEXT: /hello/g
createFromRegExpLike("hello", undefined, "g");
// CHECK-NEXT: /hello/g
createFromRegExpLike("hello", "m", "g");
// CHECK-NEXT: /hello/g
createFromRegExpLike(undefined, "m", "g");
// CHECK-NEXT: /(?:)/g

try { new RegExp("", "gmig"); } catch (err) { print(err.name); }
// CHECK-NEXT: SyntaxError
try { new RegExp("", "gxi"); } catch (err) { print(err.name); }
// CHECK-NEXT: SyntaxError

// Ensure we catch excessive capture groups.
var longPattern = '()';
for (var i=0; i < 16; i++) longPattern += longPattern;
try { new RegExp(longPattern, ""); } catch (err) { print(err.name); }
// CHECK-NEXT: SyntaxError

// Ensure we catch excessive loops groups.
var longPattern = '.*';
for (var i=0; i < 16; i++) longPattern += longPattern;
try { new RegExp(longPattern, ""); } catch (err) { print(err.name); }
// CHECK-NEXT: SyntaxError

// Ensure very deep nesting does not produce an error.
print("dcba".search(RegExp("(".repeat(50000) + "a" + ")".repeat(50000))));
// CHECK-NEXT: 3

// Ensure a large number of alternations does not produce an error
try { print(RegExp("a" + "|a".repeat(50000)).source.length); } catch (err) { print(err.name); }
// CHECK-NEXT: 100001

try { new RegExp("*"); } catch (err) { print(err.name); }
// CHECK-NEXT: SyntaxError
try { new RegExp("[z-a]"); } catch (err) { print(err.name); }
// CHECK-NEXT: SyntaxError
print(new RegExp("[\\s-\\S]"));
// CHECK-NEXT: /[\s-\S]/
try { new RegExp("[1-0]"); } catch (err) { print(err.name); }
// CHECK-NEXT: SyntaxError
print(new RegExp("[1-1]"));
// CHECK-NEXT: /[1-1]/
print(/[]/.exec("abc"));
// CHECK-NEXT: null
print(/[^]/.exec("abc"));
// CHECK-NEXT: a
print(/[a-zA-Z]*/.exec("AbCd-ef!"));
// CHECK-NEXT: AbCd
print(/[-a-zA-Z]*/.exec("AbCd-ef!"));
// CHECK-NEXT: AbCd-ef
print(/(abc|)+/.exec("abc"));
// CHECK-NEXT: abc,abc
print(/(abc|)*/.exec("abc"));
// CHECK-NEXT: abc,abc
print(/(abc|)+/.exec("xyz      "));
// CHECK-NEXT: ,

var m = RegExp("\\w (\\d+)").exec("abc 1234");
print(m.input + ", " + m.index + ", " + m.length + ", " + m[1]);
// CHECK-NEXT: abc 1234, 2, 2, 1234
print(RegExp("\\w (\\d+)").exec("abc def"));
// CHECK-NEXT: null
print(RegExp("\\w (\\d+)").exec("abc 1234"));
// CHECK-NEXT: c 1234,1234
print(RegExp(123).exec('789123456'));
// CHECK-NEXT: 123
print(JSON.stringify(/[\3]/.exec("abc\3def")));
// CHECK-NEXT: ["\u0003"]

// Check lastIndex handling
var re = RegExp("a", "g");
print(re.lastIndex);
// CHECK-NEXT: 0
print(re.exec("aab"));
// CHECK-NEXT: a
print(re.lastIndex);
// CHECK-NEXT: 1
print(re.exec("aab"));
// CHECK-NEXT: a
print(re.lastIndex);
// CHECK-NEXT: 2
print(re.exec("aab"));
// CHECK-NEXT: null
print(re.lastIndex);
// CHECK-NEXT: 0
re.lastIndex = -1000;
re.exec("aab");
print(re.lastIndex);
// CHECK-NEXT: 1
re.lastIndex = 1;
re.exec("aab");
print(re.lastIndex);
// CHECK-NEXT: 2

// Non-global regexps should ignore lastIndex
re = RegExp("a", "");
print(re.lastIndex);
// CHECK-NEXT: 0
re.exec("aab");
print(re.lastIndex);
// CHECK-NEXT: 0
re.lastIndex = 100;
print(re.exec("aab"));
// CHECK-NEXT: a
print(re.lastIndex);
// CHECK-NEXT: 100

// Check lastIndex handling with non-standard class
re = RegExp("a", "g");
re.myDummyProp = 42;
print(re.lastIndex);
// CHECK-NEXT: 0
print(re.exec("aab"));
// CHECK-NEXT: a
print(re.lastIndex);
// CHECK-NEXT: 1

// Check RegExp flags
// We need strict mode here.
(function() {
"use strict";
re = RegExp("abc", "")
print(re.global, re.ignoreCase, re.multiline, re.sticky, re.hasIndices, re.lastIndex);
// CHECK-NEXT: false false false false false 0
re = RegExp("abc", "igymd")
print(re.global, re.ignoreCase, re.multiline, re.sticky, re.hasIndices, re.lastIndex);
// CHECK-NEXT: true true true true true 0
re = RegExp("abc", "gi")
print(re.global, re.ignoreCase, re.multiline, re.sticky, re.hasIndices, re.lastIndex);
// CHECK-NEXT: true true false false false 0
try { re.global = false; } catch (err) { print(err.name); } // not writable
// CHECK-NEXT: TypeError
try { re.ignoreCase = false; } catch (err) { print(err.name); } // not writable
// CHECK-NEXT: TypeError
try { re.multiline = false; } catch (err) { print(err.name); } // not writable
// CHECK-NEXT: TypeError
try { re.sticky = false; } catch (err) { print(err.name); } // not writable
// CHECK-NEXT: TypeError
try { re.hasIndices = false; } catch (err) { print(err.name); } // not writable
// CHECK-NEXT: TypeError
re.lastIndex = 42; // yes writable
print(re.global, re.ignoreCase, re.multiline, re.lastIndex);
// CHECK-NEXT: true true false 42
try { eval("b+/v/a"); } catch (err) { print(err.name); };
// CHECK-NEXT: SyntaxError

// Flag property getter tests.
var globalGetter = Object.getOwnPropertyDescriptor(RegExp.prototype, 'global').get;
var ignoreCaseGetter = Object.getOwnPropertyDescriptor(RegExp.prototype, 'ignoreCase').get;
var multilineGetter = Object.getOwnPropertyDescriptor(RegExp.prototype, 'multiline').get;
var stickyGetter = Object.getOwnPropertyDescriptor(RegExp.prototype, 'sticky').get;
var dotAllGetter = Object.getOwnPropertyDescriptor(RegExp.prototype, 'dotAll').get;
var hasIndicesGetter = Object.getOwnPropertyDescriptor(RegExp.prototype, 'hasIndices').get;
print(globalGetter.call(/abc/g), globalGetter.call(/abc/), globalGetter.call(RegExp.prototype));
// CHECK-NEXT: true false undefined
print(ignoreCaseGetter.call(/abc/i), ignoreCaseGetter.call(/abc/), ignoreCaseGetter.call(RegExp.prototype));
// CHECK-NEXT: true false undefined
print(multilineGetter.call(/abc/m), multilineGetter.call(/abc/), multilineGetter.call(RegExp.prototype));
// CHECK-NEXT: true false undefined
print(stickyGetter.call(/abc/y), stickyGetter.call(/abc/), stickyGetter.call(RegExp.prototype));
// CHECK-NEXT: true false undefined
print(dotAllGetter.call(/abc/s), dotAllGetter.call(/abc/), dotAllGetter.call(RegExp.prototype));
// CHECK-NEXT: true false undefined
print(hasIndicesGetter.call(/abc/d), hasIndicesGetter.call(/abc/), hasIndicesGetter.call(RegExp.prototype));
// CHECK-NEXT: true false undefined
try { multilineGetter.call({}); } catch (err) { print(err.name); }
// CHECK-NEXT: TypeError
try { multilineGetter.call(undefined); } catch (err) { print(err.name); }
// CHECK-NEXT: TypeError


// Flags accessor support
print(/aaa/.flags.length);
// CHECK-NEXT: 0
print(/aaa/mi.flags, /aaa/im.flags, /aaa/ig.flags, /aaa/gi.flags, /aaa/gim.flags, /aaa/mgi.flags, /aaa/m.flags, /aaa/g.flags, /aaa/i.flags, /aaa/y.flags, /aaa/s.flags);
// CHECK-NEXT: im im gi gi gim gim m g i y s
print(/aaa/igsdmyu.flags);
// CHECK-NEXT: dgimsuy

var flagsGetter = Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get;
print(flagsGetter.call({multiline: 1, global: 0, ignoreCase: "yep"}));
// CHECK-NEXT: im

try { flagsGetter.call(undefined); } catch (err) { print(err.name); }
// CHECK-NEXT: TypeError

})();

// Test capture groups inside assertions
print(/(a+)(?=\1)/.exec("aaaaaa"));
// CHECK-NEXT: aaa,aaa
print(/(a+)(?=(\1)\1)/.exec("aaaaaa"));
// CHECK-NEXT: aa,aa,aa
print(/(a+)(?!(?=\1))/.exec("aaaaaa"));
// CHECK-NEXT: aaaaaa,aaaaaa
print(/(a+)((?=\1))/.exec("aaaaaa"));
// CHECK-NEXT: aaa,aaa,
print(/(a+)(?!\1)/.exec("aaaaaa"));
// CHECK-NEXT: aaaaaa,aaaaaa
print(JSON.stringify(/(a+)(?!(\1))/.exec("aaaaaa")));
// CHECK-NEXT: ["aaaaaa","aaaaaa",null]
print(/(xy\1)/.exec("xy"));
// CHECK-NEXT: xy,xy

// Non-matching capture groups should be undefined, not empty
print(/aa(b)?aa/.exec("aaaa")[1]);
// CHECK-NEXT: undefined
print(/(a+)(?!(\1))/.exec("aaaaaa"));
// CHECK-NEXT: aaaaaa,aaaaaa,
print(/\1(a)/.exec("aa")); // see 15.10.2.11_A1_T5 from test262
// CHECK-NEXT: a,a
print(/\2(a)/.exec("aa"));
// CHECK-NEXT: null
print(/((a)|(b))*?c/.exec("abc"))
// CHECK-NEXT: abc,b,,b

// Tests from the spec itself.
print(/(?=(a+))/.exec("baaabac"));
// CHECK-NEXT: ,aaa
print(/(?=(a+))a*b\1/.exec("baaabac"));
// CHECK-NEXT: aba,a
print(JSON.stringify(/(.*?)a(?!(a+)b\2c)\2(.*)/.exec("baaabaac")));
// CHECK-NEXT: ["baaabaac","ba",null,"abaac"]
print(JSON.stringify(/\0/.exec("abc\0def")));
// CHECK-NEXT: ["\u0000"]

// Multiline support
print(/^abc/.exec("abc"));
// CHECK-NEXT: abc
print(/^def/.exec("abc\ndef"));
// CHECK-NEXT: null
print(/^def/m.exec("abc\ndef"));
// CHECK-NEXT: def
print(/^def/m.exec("abc\n\rdef"));
// CHECK-NEXT: def
print(/(a*)^(a*)$/.exec("aa\raaa"));
// CHECK-NEXT: null
print(/(a*)^(a*)$/m.exec("aa\raaa"));
// CHECK-NEXT: aa,,aa
print(/[ab]$/.exec("a\rb"));
// CHECK-NEXT: b
print(/[ab]$/m.exec("a\rb"));
// CHECK-NEXT: a
print('aaa\n789\r\nccc\r\n345'.match(/^\d/gm));
// CHECK-NEXT: 7,3
print('aaa789\n789\r\nccc10\r\n345'.match(/\d$/gm));
// CHECK-NEXT: 9,9,0,5
print('hello world'.replace(/\b/g, '!'));
// CHECK-NEXT: !hello! !world!
print('hello world'.replace(/\B/g, '!'));
// CHECK-NEXT: h!e!l!l!o w!o!r!l!d
print('hello world'.replace(/\B|\b/g, '!'));
// CHECK-NEXT: !h!e!l!l!o! !w!o!r!l!d!

// Lookbehind assertions.
print(/(?<=efg)../.exec("abcdefghijk123456"))
// CHECK-NEXT: hi
print(/(?<=\d{3}).*/.exec("abcdefghijk123456"))
// CHECK-NEXT: 456
print(!! /(?<=\d{3}.*)/.exec("abcdefghijk123456"))
// CHECK-NEXT: true
print(/(?<![a-z])../.exec("abcdefghijk123456"))
// CHECK-NEXT: ab
print(/(?<![a-z])\d{2}/.exec("abcdefghijk123456"))
// CHECK-NEXT: 23
print(/(?<=x{3,4})\d/.exec("1yxx2xxx3xxxx4xxxxx5xxxxxx6xxxxxxx7xxxxxxxx8"));
// CHECK-NEXT: 3
print(/(?<=(?:xx){3})\d/.exec("1yxx2xxx3xxxx4xxxxx5xxxxxx6xxxxxxx7xxxxxxxx8"));
// CHECK-NEXT: 6
print(/(?<=(x*))\1$/.exec("xxxxxxxx"));
// CHECK-NEXT: xxxx,xxxx
print(/(?<!(x*))\1$/.exec("xxxxxxxx"));
// CHECK-NEXT: null
print(/(?<!$ab)\d/.exec("ab1ab2"));
// CHECK-NEXT: 1
print(/(?<!^ab)\d/.exec("ab1ab2"));
// CHECK-NEXT: 2
print(/a(?<=(\1))/.exec("a"));
// CHECK-NEXT: a,
print(/a(?<=((?!\1)))/.exec("a"));
// CHECK-NEXT: null
// A ^ inside a lookbehind's alternation or loop is checked against the
// position where the lookbehind's match ends, not where it starts.
print(/a(?<=x|^a)/.exec("a"));
// CHECK-NEXT: a
print(/ba(?<=x|^a)/.exec("ba"));
// CHECK-NEXT: null
print(/b(?<=^a.|^b)/.exec("ab"));
// CHECK-NEXT: b
print(/a(?<=(x|^a))/.exec("a"));
// CHECK-NEXT: a,a
print(/a(?<=(?:^a)+)/.exec("a"));
// CHECK-NEXT: a
print(/a(?<=(?:^a){1,2})/.exec("a"));
// CHECK-NEXT: a
print(/a(?<=(?:x|^a)+)/.exec("a"));
// CHECK-NEXT: a
print(/ba(?<=(?:x|^a)+)/.exec("ba"));
// CHECK-NEXT: null


// Sticky support
print(/abc/y.exec("abc"));
// CHECK-NEXT: abc
print(/abc/y.exec("dabc"));
// CHECK-NEXT: null

// dotAll support
print(/.*/.exec("abc\ndef")[0], "DONE");
// CHECK-NEXT: abc DONE
print(/.*/s.exec("abc\ndef")[0], "DONE");
// CHECK-NEXT: abc
// CHECK-NEXT: def DONE
print(/.*/su.exec("abc\ndef")[0], "DONE");
// CHECK-NEXT: abc
// CHECK-NEXT: def DONE

print((new RegExp("5{").exec("abc5{,}def")));
// CHECK-NEXT: 5{
print((new RegExp("5{,").exec("abc5{,}def")));
// CHECK-NEXT: 5{,
print((new RegExp("5{,}").exec("abc5{,}def")));
// CHECK-NEXT: 5{,}
print((new RegExp("5{,10}").exec("abc5{,10}def")));
// CHECK-NEXT: 5{,10}
print((new RegExp("5{10,}").exec("abc5{,10}def")));
// CHECK-NEXT: null
print((new RegExp("{10").exec("abc5{10,10}def")));
// CHECK-NEXT: {10
print((new RegExp("{10,,100}").exec("abc5{10,,100}def")));
// CHECK-NEXT: {10,,100}
print((new RegExp("abc{5,,}").exec("123abc{5,,}def")));
// CHECK-NEXT: abc{5,,}
try { print((new RegExp("{5}").exec("abc5{10,10}def"))) } catch (err) { print(err.name); }
// CHECK-NEXT: SyntaxError
print((new RegExp("]").exec("abc]def")));
// CHECK-NEXT: ]
try { print((new RegExp(")").exec("abc)def"))); } catch (err) { print(err.name); }
// CHECK-NEXT: SyntaxError
print(JSON.stringify(/|/.exec("abc")));
// CHECK-NEXT: [""]
print("X".match(/(A{9999999999}B|X)*/ ));
// CHECK-NEXT: X,X

(function () {
  function foo() {}
  let inst = Reflect.construct(RegExp, [], foo);
  print(Object.getPrototypeOf(inst) === foo.prototype);
// CHECK-NEXT: true
})();

// hasIndices support
var indicesRegexRes1 = /(a).(c)/d.exec("abcdef")
print(indicesRegexRes1.indices);
// CHECK-NEXT: 0,3,0,1,2,3
print(indicesRegexRes1.indices.groups);
// CHECK-NEXT: undefined

var indicesRegexRes2 = /(?<a_letter>a).(?<c_letter>c)/d.exec("abcdef")
print(indicesRegexRes2.indices);
// CHECK-NEXT: 0,3,0,1,2,3
print(Object.getOwnPropertyNames(indicesRegexRes2.indices.groups));
// CHECK-NEXT: a_letter,c_letter
print(indicesRegexRes2.indices.groups.a_letter);
// CHECK-NEXT: 0,1
print(indicesRegexRes2.indices.groups.c_letter);
// CHECK-NEXT: 2,3

var indicesRegexRes3 = /(?<a_letter>a)(?<h_letter>h)*/d.exec("abcdef")
print(indicesRegexRes3.indices);
// CHECK-NEXT: 0,1,0,1,
print(Object.getOwnPropertyNames(indicesRegexRes3.indices.groups));
// CHECK-NEXT: a_letter,h_letter
print(indicesRegexRes3.indices.groups.a_letter);
// CHECK-NEXT: 0,1
print(indicesRegexRes3.indices.groups.h_letter);
// CHECK-NEXT: undefined

// Don't split surrogate pairs
re = RegExp(".", "duy");
re.lastIndex = 1;
var res = re.exec("\u{1F600}");
print(res.indices[0][0], res.indices[0][1]);
// CHECK-NEXT: 0 2
print(res[0].charCodeAt(0), res[0].charCodeAt(1));
// CHECK-NEXT: 55357 56832

// Source support
print(/abc/.source);
// CHECK-NEXT: abc
print(/\//.source);
// CHECK-NEXT: \/
print(RegExp("/").source);
// CHECK-NEXT: \/
print(/[/]/.source);
// CHECK-NEXT: [\/]
print(RegExp("").source);
// CHECK-NEXT: (?:)
print(RegExp("abc\ndef").source);
// CHECK-NEXT: abc\ndef
print(RegExp("abc\\\ndef").source);
// CHECK-NEXT: abc\ndef
print(RegExp("abc\rdef").source);
// CHECK-NEXT: abc\rdef
print(RegExp("abc\\\rdef").source);
// CHECK-NEXT: abc\rdef
print(RegExp("abc\u2028def").source);
// CHECK-NEXT: abc\u2028def
print(RegExp("abc\\\u2028def").source);
// CHECK-NEXT: abc\u2028def
print(RegExp("abc\u2029def").source);
// CHECK-NEXT: abc\u2029def
print(RegExp("abc\\\u2029def").source);
// CHECK-NEXT: abc\u2029def
var sourceGetter = Object.getOwnPropertyDescriptor(RegExp.prototype, 'source').get;
print(sourceGetter.call(/abc/));
// CHECK-NEXT: abc
print(sourceGetter.call(RegExp.prototype));
// CHECK-NEXT: (?:)
try { sourceGetter.call(undefined); } catch (err) { print(err.name); }
// CHECK-NEXT: TypeError
try { sourceGetter.call({}); } catch (err) { print(err.name); }
// CHECK-NEXT: TypeError

print(RegExp.prototype);
// CHECK-NEXT: /(?:)/
print(/abc/ instanceof RegExp, new RegExp("abc") instanceof RegExp, RegExp.prototype instanceof RegExp)
// CHECK-NEXT: true true false

print('static properties');
// CHECK-LABEL: static properties
print(/(a)sdf/.exec('asdf'), RegExp.$1);
// CHECK-NEXT: asdf,a a
print('empty', RegExp.$2);
// CHECK-NEXT: empty
print(/asdf/.exec('qwer'), RegExp.$1);
// CHECK-NEXT: null a
print(/(a)|b/.exec("b"), JSON.stringify(RegExp.$1), JSON.stringify(RegExp["$+"]));
// CHECK-NEXT: b, "" ""
/(ban)a(na)/.exec('applebananacarrot');
print(RegExp["$`"], RegExp.leftContext);
// CHECK-NEXT: apple apple
print(RegExp["$&"], RegExp.lastMatch);
// CHECK-NEXT: banana banana
print(RegExp["$'"], RegExp.rightContext);
// CHECK-NEXT: carrot carrot
print(RegExp["$_"], RegExp.input);
// CHECK-NEXT: applebananacarrot applebananacarrot
print(RegExp["$+"], RegExp.lastParen);
// CHECK-NEXT: na na
print(RegExp.$1, RegExp.$2);
// CHECK-NEXT: ban na

// Check that error messages are propagated to exceptions.
// The unit tests check the individual errors in more detail.
try { new RegExp("["); } catch (e) { print(e.message); }
// CHECK-NEXT: Invalid RegExp: Character class not closed
try { new RegExp("\\"); } catch (e) { print(e.message); }
// CHECK-NEXT: Invalid RegExp: Incomplete escape

// Check that incomplete escapes don't cause an OOB read.
try { new RegExp("                                \\"); } catch (e) { print(e.message); }
// CHECK-NEXT: Invalid RegExp: Incomplete escape

// textual 'undefined' flag is invalid.
try {
  RegExp(/1/g, 'undefined');
} catch (e) {
  print (e);
}
// CHECK-NEXT: SyntaxError: Invalid RegExp: Invalid flags
// actual undefined flag would result in empty pattern.
var obj = {
  get [Symbol.match]() { return () => 1 }
}
print(RegExp(obj))
// CHECK-NEXT: /(?:)/

print("RegExp.prototype[Symbol.match]");
// CHECK-LABEL: RegExp.prototype[Symbol.match]
print(/[0-9]+/g[Symbol.match]('2016-01-02'));
// CHECK-NEXT: 2016,01,02
print(/[0-9]+/g[Symbol.match]('hello'));
// CHECK-NEXT: null
print(RegExp.prototype[Symbol.match].name);
// CHECK-NEXT: [Symbol.match]

print("RegExp.prototype[Symbol.matchAll]");
// CHECK-LABEL: RegExp.prototype[Symbol.matchAll]
var it = /[0-9]+/g[Symbol.matchAll]('2016-01-02')
var y = it.next()
var m = it.next()
var d = it.next()
var nil = it.next()
print(y.value, y.done)
// CHECK-NEXT: 2016 false
print(m.value, m.done)
// CHECK-NEXT: 01 false
print(d.value, d.done)
// CHECK-NEXT: 02 false
print(nil.value, nil.done)
// CHECK-NEXT: undefined true
print([.../[0-9]+/g[Symbol.matchAll]('2016-01-02')])
// CHECK-NEXT: 2016,01,02
var it = /[0-9]+/g[Symbol.matchAll]('hello')
var nil = it.next()
print(nil.value, nil.done)
// CHECK-NEXT: undefined true
print(RegExp.prototype[Symbol.matchAll].name);
// CHECK-NEXT: [Symbol.matchAll]
try {
  RegExp.prototype[Symbol.matchAll].call(1);
} catch (e) {
  print(e)
}
// CHECK-NEXT: TypeError: RegExp.prototype[@@matchAll] should be called on a js object

print("RegExp.prototype[Symbol.search]");
// CHECK-LABEL: RegExp.prototype[Symbol.search]
print(/[0-9]+/g[Symbol.search]('2016-01-02'));
// CHECK-NEXT: 0
print(/-/g[Symbol.search]('2016-01-02'));
// CHECK-NEXT: 4
print(/[0-9]+/g[Symbol.search]('hello'));
// CHECK-NEXT: -1
print(RegExp.prototype[Symbol.search].name);
// CHECK-NEXT: [Symbol.search]

print("RegExp.prototype[Symbol.replace]");
// CHECK-LABEL: RegExp.prototype[Symbol.replace]
print(RegExp.prototype[Symbol.replace].name);
// CHECK-NEXT: [Symbol.replace]
print(/-/g[Symbol.replace]('2016-01-01', '.'));
// CHECK-NEXT: 2016.01.01
// Shouldn't throw if index is large.
var r = /./;
r.exec = function () {
  return { index: 1.7976931348623157e+308 }
};
r[Symbol.replace](0);

print("RegExp.prototype[Symbol.split]");
// CHECK-LABEL: RegExp.prototype[Symbol.split]
print(RegExp.prototype[Symbol.split].name);
// CHECK-NEXT: [Symbol.split]
print(/-/[Symbol.split]('a-b-c'));
// CHECK-NEXT: a,b,c

// Check UTF-16 string matching executes correctly
print(/abc/u.exec("\u20ac\u20ac\u20ac\u20ac"));
// CHECK-LABEL: null

// Check that lookbehind searches stay within bounds
print(/(?<=a)/u[Symbol.match](["\u00E9",34534502349000]))
// CHECK-LABEL: null

// Check that \B assertions are parsed as Assertions per the spec, as opposed to AtomEscapes.
try {
  new RegExp("\\B{1}")
} catch (e) {
  print(e)
}
// CHECK-LABEL: SyntaxError: Invalid RegExp: Quantifier has nothing to repeat

// Case-insensitive word boundary (\b with /iu).
// U+212A KELVIN SIGN canonicalizes to 'k', so \b should treat it as a word char.
print(/\bk\b/iu.test("\u212A"));
// CHECK-NEXT: true
