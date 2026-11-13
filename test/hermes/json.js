/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -non-strict -O -target=HBC -gc-sanitize-handles=0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -non-strict -O -target=HBC -emit-binary -out %t.hbc %s && %hermes -gc-sanitize-handles=0 %t.hbc | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s -Wx,-gc-sanitize-handles=0 | %FileCheck --match-full-lines %s

// This test was one of HandleSan's slowest at 30 seconds, so
// -gc-sanitize-handles=0 is passed to reduce the risk of a timeout.

function add(a, b) { return a + b; }

print(JSON.toString());
//CHECK-LABEL: [object JSON]

// JSON.parse()

print(JSON.parse("5.6") === 5.6);
//CHECK-NEXT: true

print(JSON.parse("true") === true);
//CHECK-NEXT: true

print(JSON.parse("false") === false);
//CHECK-NEXT: true

print(JSON.parse("null") === null);
//CHECK-NEXT: true

print(JSON.parse("\"str\"") === "str");
//CHECK-NEXT: true

var obj = JSON.parse("{\"p1\": \"v1\", \"p2\": 5}");
for (var p in obj) {
  print(typeof obj[p], obj[p]);
}
//CHECK-NEXT: string v1
//CHECK-NEXT: number 5

var arr = JSON.parse("[1, 2, \"a\", {}]");
print(arr.length);
//CHECK-NEXT: 4
for (var i = 0; i < arr.length; ++i) {
  print(typeof arr[i], arr[i]);
}
//CHECK-NEXT: number 1
//CHECK-NEXT: number 2
//CHECK-NEXT: string a
//CHECK-NEXT: object [object Object]

function parse(json) {
  try {
    JSON.parse(json);
  } catch (e) {
    print(e);
  }
}
parse("015");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character in number: 1
parse("-015");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character in number: 1
parse("-00");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character in number: 0
parse("[-01.5]");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character in number: 1
print(1 / JSON.parse("-0"), JSON.parse("-0.5"), JSON.parse("[-10]")[0]);
//CHECK-NEXT: -Infinity -0.5 -10
parse("+5");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character: +
parse("-");
//CHECK-NEXT: SyntaxError: JSON Parse error: No digits after minus sign in JSON number
parse("[-]");
//CHECK-NEXT: SyntaxError: JSON Parse error: No digits after minus sign in JSON number
parse("[5,]");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character: ]
parse("[0x5]");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character: x
parse("[\"\\v\"]");
//CHECK-NEXT: SyntaxError: JSON Parse error: Invalid escape sequence: v
parse("nulltrue");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character: t
parse("nul");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected end of input
parse("{5: 5}");
//CHECK-NEXT: SyntaxError: JSON Parse error: Expect a string key in JSON object
parse("{\"5\": true");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected end of input
parse("{\"ab\"}");
//CHECK-NEXT: SyntaxError: JSON Parse error: Expect ':' after the key in JSON object
parse("\"\\u");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected end of input
parse("\"\\uabcg\"");
//CHECK-NEXT: SyntaxError: JSON Parse error: Invalid unicode point character: g
parse("ef");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character: e
parse("\"\\\"");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected end of input
parse("\"\\a\"");
//CHECK-NEXT: SyntaxError: JSON Parse error: Invalid escape sequence: a
parse("{}[]");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character: [
parse("");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected end of input
parse("abc");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected character: a
parse("[true");
//CHECK-NEXT: SyntaxError: JSON Parse error: Unexpected end of input

// reviver
function reviver(key, value) {
  print(key);
  if (key === "old" || key === "") {
    return value;
  } else if (key === "new") {
    return [7, 8];
  } else if (key === '0') {
    return "a";
  } else if (key === '1') {
    return "b";
  } else {
    return undefined;
  }
}
print(JSON.stringify(JSON.parse(add('{"discard": [1, 2], "n', 'ew": [3, 4], "old": [5, 6]}'), reviver)));
//CHECK-NEXT: 0
//CHECK-NEXT: 1
//CHECK-NEXT: discard
//CHECK-NEXT: 0
//CHECK-NEXT: 1
//CHECK-NEXT: new
//CHECK-NEXT: 0
//CHECK-NEXT: 1
//CHECK-NEXT: old
//CHECK: {"new":[7,8],"old":["a","b"]}

// Non-callable reviver.
print(JSON.stringify(JSON.parse(add('{"discard": [1, 2], "n', 'ew": [3, 4], "old": [5, 6]}'), 10)));
//CHECK-NEXT: {"discard":[1,2],"new":[3,4],"old":[5,6]}


// JSON.stringify()

// Simple cases
print(JSON.stringify(5.5));
//CHECK-NEXT: 5.5
print(JSON.stringify(null));
//CHECK-NEXT: null
print(JSON.stringify(true));
//CHECK-NEXT: true
print(JSON.stringify(false));
//CHECK-NEXT: false
print(JSON.stringify("abc"));
//CHECK-NEXT: "abc"
print(typeof JSON.stringify(undefined));
//CHECK-NEXT: undefined
print(JSON.stringify([]));
//CHECK-NEXT: []
print(JSON.stringify({}));
//CHECK-NEXT: {}

var obj = new Number(5);
obj.valueOf = () => 10;
print(JSON.stringify(obj));
//CHECK-NEXT: 10

var obj = new String('asdf');
obj.toString = () => 'qwerty';
print(JSON.stringify(obj));
//CHECK-NEXT: "qwerty"

var obj = new Boolean(false);
print(JSON.stringify(obj));
//CHECK-NEXT: false

var obj = {
  "a": [1, true, false, "a", undefined, null, [], {}],
  "b": {
    "c": "d"
  }
};

// Complicated object
print(JSON.stringify(obj));
//CHECK-NEXT: {"a":[1,true,false,"a",null,null,[],{}],"b":{"c":"d"}}

// Complicated object with space
print(JSON.stringify(obj, undefined, 4));
//CHECK-NEXT: {
//CHECK-NEXT:     "a": [
//CHECK-NEXT:         1,
//CHECK-NEXT:         true,
//CHECK-NEXT:         false,
//CHECK-NEXT:         "a",
//CHECK-NEXT:         null,
//CHECK-NEXT:         null,
//CHECK-NEXT:         [],
//CHECK-NEXT:         {}
//CHECK-NEXT:     ],
//CHECK-NEXT:     "b": {
//CHECK-NEXT:         "c": "d"
//CHECK-NEXT:     }
//CHECK-NEXT: }

var space = new String("x")
space.toString = () => "y";
print(JSON.stringify(obj, undefined, space));
//CHECK-NEXT: {
//CHECK-NEXT: y"a": [
//CHECK-NEXT: yy1,
//CHECK-NEXT: yytrue,
//CHECK-NEXT: yyfalse,
//CHECK-NEXT: yy"a",
//CHECK-NEXT: yynull,
//CHECK-NEXT: yynull,
//CHECK-NEXT: yy[],
//CHECK-NEXT: yy{}
//CHECK-NEXT: y],
//CHECK-NEXT: y"b": {
//CHECK-NEXT: yy"c": "d"
//CHECK-NEXT: y}
//CHECK-NEXT: }

// Replacer
print(JSON.stringify(obj, ["a", "a", "a"]));
//CHECK-NEXT: {"a":[1,true,false,"a",null,null,[],{}]}
print(JSON.stringify(obj, ["b"]));
//CHECK-NEXT: {"b":{}}
print(JSON.stringify(obj, ["b", "c"]));
//CHECK-NEXT: {"b":{"c":"d"}}
print(JSON.stringify(obj, function(key, value) { print(key); return value; }));
//CHECK: a
//CHECK-NEXT: 0
//CHECK-NEXT: 1
//CHECK-NEXT: 2
//CHECK-NEXT: 3
//CHECK-NEXT: 4
//CHECK-NEXT: 5
//CHECK-NEXT: 6
//CHECK-NEXT: 7
//CHECK-NEXT: b
//CHECK-NEXT: c
//CHECK-NEXT: {"a":[1,true,false,"a",null,null,[],{}],"b":{"c":"d"}}

var obj = {
  "a": [1, true, false, "a", undefined, null, [], {}],
  "b": {
    "c": "d"
  },
  "c": {
    "a": [1, true, false, "a", undefined, null, [], {}],
    "b": {
      "c": {
        "a": [1, true, false, "a", undefined, null, [], {}],
        "b": {
          "c": "d",
          "d": "d",
          "e": "d",
          "f": "d",
          "g": "d"
        }
      },
      "d": "d",
      "e": "d",
      "f": "d",
      "g": "d"
    }
  }
};

// More complicated object
print(JSON.stringify(obj));
//CHECK-NEXT: {"a":[1,true,false,"a",null,null,[],{}],"b":{"c":"d"},"c":{"a":[1,true,false,"a",null,null,[],{}],"b":{"c":{"a":[1,true,false,"a",null,null,[],{}],"b":{"c":"d","d":"d","e":"d","f":"d","g":"d"}},"d":"d","e":"d","f":"d","g":"d"}}}

var obj = {
  "a": [1, true, false, "a", undefined, null, [], {}],
  "b": {
    "c": "d"
  }
};

var obj2 = {
  "a": [1, true, false, "a", undefined, null, [], {}, obj],
  "b": {
    "c": obj
  },
  "c": obj
};

var obj3 = {
  "a": [1, true, false, "a", undefined, null, [], {}, obj2],
  "b": {
    "c": obj2
  },
  "c": obj2
};

var obj4 = {
  "a": [1, true, false, "a", undefined, null, [], {}, obj3],
  "b": {
    "c": obj3
  },
  "c": obj3
};

var obj5 = {
  "a": [1, true, false, "a", undefined, null, [], {}, obj4],
  "b": {
    "c": obj4
  },
  "c": obj4
};

// Obnoxiously complicated object
print(JSON.stringify(obj5));
//CHECK-NEXT: {{.+}}

function createNested(n) {
  var obj = {
    "a": [1, true, false, "a", undefined, null, [], {}],
    "b": {
      "c": "d"
    }
  };
  for (i = 0; i < n; i++) {
   obj = [{"a": obj}];
  }
  return obj;
}

// Deeply nested object
print(JSON.stringify(createNested(5)));
//CHECK-NEXT: {{.+}}

function createWide(n) {
  var obj = {}
  for (i = 0; i < n; i++) {
    obj['#' + i] = [1, 2, 3];
  }
  return obj;
}

// Very wide object
print(JSON.stringify(createWide(10000)));
//CHECK-NEXT: {{.+}}

// Customized space
print(JSON.stringify(["a", "b", "c"], undefined, "abcdefghijklmnopqrstuvwxzyz"));
//CHECK-NEXT: [
//CHECK-NEXT: abcdefghij"a",
//CHECK-NEXT: abcdefghij"b",
//CHECK-NEXT: abcdefghij"c"
//CHECK-NEXT: ]

// Empty object/array with space
print(JSON.stringify([{}, []], undefined, 4));
//CHECK-NEXT: [
//CHECK-NEXT:     {},
//CHECK-NEXT:     []
//CHECK-NEXT: ]

// Escape
print(JSON.stringify("\b\f\r\n\t\0"));
//CHECK-NEXT: "\b\f\r\n\t\u0000"

// Cyclical structure
var obj = {};
var arr = [];
obj["a"] = arr;
arr[0] = obj;
try {
  JSON.stringify(obj);
} catch (e) {
  print(e);
}
//CHECK-NEXT: TypeError: cyclical structure in JSON object

var obj = {};
var arr = [];
arr[0] = obj;
obj["a"] = arr;
try {
  JSON.stringify(arr);
} catch (e) {
  print(e);
}
//CHECK-NEXT: TypeError: cyclical structure in JSON object

// Check nested objects.
var o = JSON.parse('{"a":{"c":1},"b":{"d":2}}' );
var tmp = "";
for(var i in o) {
    if (tmp)
        tmp += ",";
    tmp += i;
}
print(tmp);
//CHECK-NEXT: a,b

// Construct the source string dynamically.
var s = '{"a":{"c": 1},';
var t = ' "b":{"d":2}}';
var o = JSON.parse(add(s, t));
print(JSON.stringify(o));
//CHECK-NEXT: {"a":{"c":1},"b":{"d":2}}

var obj = { toJSON: function() { return "foo"; } };
print(JSON.stringify([obj]));
// CHECK-NEXT: ["foo"]

// Verify that deeply nested structures can be parsed.
var a = '';
var b = '';
for (var i = 0; i < 1000; ++i) {
  // 50 brackets.
  a += '[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[';
  b += ']]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]';
}
var c = a + b;
JSON.parse(c);
print("parse completed");
// CHECK-NEXT: parse completed

try {
  JSON.parse(
  '{"p": 5, "x" : []}',
  function(key, value) {
    if(this.x) {
      this.x = this;
    }
    return value;
  });
} catch (e) {
  print('caught', e.name);
}
// CHECK-NEXT: caught RangeError

// Pathological cases

var n = new Number(1);
print(typeof n);
// CHECK-NEXT: object
n.toString = function() {
  throw new Error('asdf');
}
try { JSON.stringify([1,2,3], [n]); } catch (e) { print('caught', e.message); }
// CHECK-NEXT: caught asdf

var replacer = [];
Object.defineProperty(replacer, '0', {
  get: function() {
    throw new Error('asdf');
  }
});
try {
  JSON.stringify([1,2,3], replacer);
} catch (e) {
  print('caught', e.message);
}
// CHECK-NEXT: caught asdf

var reviver = function(name, val) {
  // `this` is the holder object.
  Object.defineProperty(this, 'b', {
    get: function() {
      throw new Error('asdf');
    }
  });
}
var str = '{"a": 1, "b": 2}';
try { JSON.parse(str, reviver); } catch (e) { print('caught', e.message); }
// CHECK-NEXT: caught asdf

var reviver = function(name, val) {
  // `this` is the holder object.
  Object.defineProperty(this, 'b', {
    value: this.b,
    configurable: false,
  });
}
var str = '{"a": 1, "b": 2}';
JSON.parse(str, reviver);

var reviver = function(name, val) {
  // `this` is the holder object.
  Object.defineProperty(this, 'b', {
    value: this.b,
    configurable: false,
    writable: false,
  });
  return 'b';
}
var str = '{"a": 1, "b": 2}';
JSON.parse(str, reviver);

globalFirst = true;
var reviver = function () {
  if (globalFirst) {
    globalArray = Array(3000);
    this[2] = globalArray;
  }
  globalFirst = false;
}
print(JSON.parse('[1, 2, []]', reviver));
// CHECK-NEXT: undefined

print(JSON.parse("1.2e2"));
// CHECK-NEXT: 120
print(JSON.parse("2.3E3"));
// CHECK-NEXT: 2300

function tryPrintParse(str) {
  try {
    print(JSON.parse(str));
  } catch (e) {
    print(e.constructor.name);
  }
}

tryPrintParse("55e555555555555555555555555555555555551e-1000");
// CHECK-NEXT: SyntaxError
tryPrintParse("55e55555555555555555555555555555555555");
// CHECK-NEXT: Infinity
tryPrintParse("1e-1000");
// CHECK-NEXT: 0
tryPrintParse("abc");
// CHECK-NEXT: SyntaxError
tryPrintParse("123error");
// CHECK-NEXT: SyntaxError
tryPrintParse("123.56.78");
// CHECK-NEXT: SyntaxError
tryPrintParse("+52");
// CHECK-NEXT: SyntaxError
tryPrintParse("123.123.123.123");
// CHECK-NEXT: SyntaxError

// Verify that deeply nested objects can be parsed without crashing.
// Previously, parsing deeply nested objects caused a GCScope handle assertion
// failure because handles allocated in the inner parsing loop were not flushed.
var deepObj = '{"a":'.repeat(1000) + '1' + '}'.repeat(1000);
var parsedObj = JSON.parse(deepObj);
var cur = parsedObj;
for (var i = 0; i < 1000; ++i) cur = cur.a;
print("deep object", cur);
// CHECK-NEXT: deep object 1

// Same test but with deeply nested arrays.
var deepArr = '['.repeat(1000) + '1' + ']'.repeat(1000);
var parsedArr = JSON.parse(deepArr);
cur = parsedArr;
for (var i = 0; i < 1000; ++i) cur = cur[0];
print("deep array", cur);
// CHECK-NEXT: deep array 1
