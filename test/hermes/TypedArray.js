/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xhermes-internal-test-methods -Xes6-proxy -O -gc-sanitize-handles=0 %s | %FileCheck %s
// RUN: %shermes -exec %s -Wx,-Xhermes-internal-test-methods,-gc-sanitize-handles=0 | %FileCheck %s

'use strict';

print("START");
//CHECK:START

function isLittleEndian() {
  var uint16 = new Uint16Array(1);
  uint16[0] = 1;
  var uint8 = new Uint8Array(uint16.buffer);
  return uint8[0] === 1;
}

function getByteWidth(cons) {
  switch (cons) {
    case Int8Array:
    case Uint8Array:
    case Uint8ClampedArray:
      return 1;
    case Int16Array:
    case Uint16Array:
      return 2;
    case Int32Array:
    case Uint32Array:
    case Float32Array:
      return 4;
    case Float64Array:
      return 8;
  }
}

// This has the same API as the Node.js assert object.
var assert = {
  _isEqual: function(a, b) {
    // Remember to check for NaN which does not compare as equal with itself.
    return a === b || (Number.isNaN(a) && Number.isNaN(b));
  },
  equal: function(actual, expected, msg) {
    if (!assert._isEqual(actual, expected)) {
      assert.fail(
        (msg ? msg + ' -- ' : '') +
          'Not equal: actual <' +
          actual +
          '>, and expected <' +
          expected +
          '>',
      );
    }
  },
  _isArrayEqual: function(a, b) {
    if (a.length !== b.length)
      return false;
    for (var i = 0; i < a.length; i++) {
      if (!assert._isEqual(a[i], b[i]))
        return false;
    }
    return true;
  },
  arrayEqual: function(actual, expected, msg) {
    if (!assert._isArrayEqual(actual, expected)) {
      assert.fail(
        (msg ? msg + ' -- ' : '') +
          'Array not equal: actual <' +
          actual +
          '>, and expected <' +
          expected +
          '>',
      );
    }
  },
  notEqual: function(actual, expected, msg) {
    if (assert._isEqual(actual, expected)) {
      assert.fail(
        (msg ? msg + ' -- ' : '') +
          'Equal: actual <' +
          actual +
          '>, and expected <' +
          expected +
          '>',
      );
    }
  },
  ok: function(value, msg) {
    assert.equal(!!value, true, msg);
  },
  throws: function(block, error, msg) {
    try {
      block();
    } catch (e) {
      assert.equal(e.constructor, error, msg);
      return;
    }
    // Can't put fail inside the try because it will catch the AssertionError.
    assert.fail((msg ? msg + ' -- ' : '') + 'Failed to throw');
  },
  fail: function(msg) {
    throw new Error('AssertionError: ' + (msg ? msg : 'Failed'));
  },
};

var cons = [
  Int8Array,
  Int16Array,
  Int32Array,
  Uint8Array,
  Uint8ClampedArray,
  Uint16Array,
  Uint32Array,
  Float32Array,
  Float64Array,
];
var LE = isLittleEndian();

// Check length
cons.forEach(function(TypedArray) {
  var buf = new ArrayBuffer(32);
  var view = new TypedArray(buf);
  assert.equal(view.length, buf.byteLength / getByteWidth(TypedArray));
});

// Check byteLength
cons.forEach(function(TypedArray) {
  var buf = new ArrayBuffer(32);
  var view = new TypedArray(buf);
  assert.equal(view.byteLength, buf.byteLength);
});

// Check byteOffset
cons.forEach(function(TypedArray) {
  assert.equal(new TypedArray(new ArrayBuffer(32), 8, 1).byteOffset, 8);
});

// Check BYTES_PER_ELEMENT
cons.forEach(function(TypedArray) {
  assert.equal(TypedArray.BYTES_PER_ELEMENT, getByteWidth(TypedArray));
  assert.equal(new TypedArray().BYTES_PER_ELEMENT, getByteWidth(TypedArray));
  // Check flags of the field.
  var desc = Object.getOwnPropertyDescriptor(TypedArray, 'BYTES_PER_ELEMENT');
  assert.ok(!desc.writable);
  assert.ok(!desc.enumerable);
  assert.ok(!desc.configurable);

  desc = Object.getOwnPropertyDescriptor(
    TypedArray.prototype,
    'BYTES_PER_ELEMENT',
  );
  assert.ok(!desc.writable);
  assert.ok(!desc.enumerable);
  assert.ok(!desc.configurable);
});

// Check the flags for various fields.
cons.forEach(function(TypedArray) {
  var proto = Object.getPrototypeOf(TypedArray);
  assert.ok(
    !Object.getOwnPropertyDescriptor(proto.prototype, 'buffer').enumerable,
  );
  assert.ok(
    !Object.getOwnPropertyDescriptor(proto.prototype, 'byteLength').enumerable,
  );
  assert.ok(
    !Object.getOwnPropertyDescriptor(proto.prototype, 'byteOffset').enumerable,
  );

  assert.ok(
    Object.getOwnPropertyDescriptor(proto.prototype, 'buffer').configurable,
  );
  assert.ok(
    Object.getOwnPropertyDescriptor(proto.prototype, 'byteLength').configurable,
  );
  assert.ok(
    Object.getOwnPropertyDescriptor(proto.prototype, 'byteOffset').configurable,
  );
});

// Check length of the constructors.
cons.forEach(function(TypedArray) {
  assert.equal(TypedArray.length, 3);
});

// Check initialized to zero
cons.forEach(function(TypedArray) {
  var buf = new ArrayBuffer(32);
  var view = new TypedArray(buf);
  for (var i = 0; i < view.length; i++) {
    assert.equal(view[i], 0);
  }
});

(function checkBufferShared() {
  var buf = new ArrayBuffer(32);
  var int8view = new Int8Array(buf);
  var uint8view = new Uint8Array(buf);

  // Check that the buffer is shared
  // Ensure they're both zero'd out before writing (in case previous tests left
  // data in them)
  int8view[0] = 0;
  uint8view[0] = 0;
  int8view[0] = 5;
  assert.equal(uint8view[0], 5);
})();

/// @name Constructors
/// @{

// Check constructor from Array with ints
cons.forEach(function(TypedArray) {
  var view = new TypedArray([1, 2, 3]);
  assert.equal(view.length, 3);
  assert.equal(view[0], 1);
  assert.equal(view[1], 2);
  assert.equal(view[2], 3);
});

(function checkArrayCopyWithDeletedElement() {
  var source = [1, 2, 3];
  delete source[1];

  // The deleted element reads as undefined, which converts to NaN for a float
  // type and to 0 for an integer type.
  var view = new Float64Array(source);
  assert.arrayEqual([view[0], view[1], view[2]], [1, NaN, 3]);

  view = Uint8Array.from(source);
  assert.arrayEqual([view[0], view[1], view[2]], [1, 0, 3]);

  // The mapper visits every index once and gets undefined for the deleted
  // element, and the source keeps its hole.
  var visited = [];
  view = Float64Array.from(source, function(value, index) {
    visited.push(index);
    if (index === 1) {
      assert.equal(value, undefined);
      return 7;
    }
    return value;
  });
  assert.arrayEqual([view[0], view[1], view[2]], [1, 7, 3]);
  assert.arrayEqual(visited, [0, 1, 2]);
  assert.equal(source.length, 3);
  assert.equal(1 in source, false);

  // The holes are read while the source is iterated, before the mapper runs,
  // so the mapper must not observe the elements it adds to Array.prototype.
  source = [1, 2, 3, 4];
  delete source[1];
  delete source[3];
  try {
    view = Float64Array.from(source, function(value, index) {
      if (index === 0) {
        Array.prototype[1] = 41;
        Array.prototype[3] = 43;
      }
      return value;
    });
  } finally {
    // Truncate rather than delete: deleting would leave holes in
    // Array.prototype, which disables the array fast path for later tests.
    Array.prototype.length = 0;
  }
  assert.equal(Array.prototype.length, 0);
  assert.arrayEqual([view[0], view[1], view[2], view[3]], [1, NaN, 3, NaN]);
})();

// Check constructor from dense Array
(function() {
  var dense = [1, 2, 3];
  var view = new Float64Array(dense);
  assert.arrayEqual([view[0], view[1], view[2]], dense);
})();

// Check constructor from Array with a hole, which reads as undefined
(function() {
  var sparse = [1, , 3];
  var view = new Float64Array(sparse);
  assert.equal(view[0], 1);
  assert.equal(view[1], NaN);
  assert.equal(view[2], 3);
})();

// Check constructor from Array with a hole, which reads through the
// prototype chain
(function() {
  var sparse = [1, , 3];
  Array.prototype[1] = 7;
  try {
    var view = new Float64Array(sparse);
    assert.equal(view[1], 7);
  } finally {
    // Truncate rather than delete: deleting would leave holes in
    // Array.prototype, which disables the array fast path for later tests.
    Array.prototype.length = 0;
  }
  assert.equal(Array.prototype.length, 0);
})();

// Check constructor from Array that a getter grows during iteration
(function() {
  var growing = [1, 2];
  Object.defineProperty(growing, 0, {
    get: function() {
      growing.push(3);
      return 1;
    },
  });
  var view = new Float64Array(growing);
  assert.equal(view.length, 3);
  assert.equal(view[2], 3);
})();

// Check constructor from Array that a getter shrinks during iteration
(function() {
  var shrinking = [];
  shrinking.length = 0xffffffff;
  Object.defineProperty(shrinking, 0, {
    get: function() {
      shrinking.length = 1;
      return 7;
    },
  });
  var view = new Float64Array(shrinking);
  assert.equal(view.length, 1);
  assert.equal(view[0], 7);
})();

// Check that constructor reads every element of an Array before converting
// any of them
(function() {
  var source = [
    {
      valueOf: function() {
        source[1] = 9;
        return 1;
      },
    },
    2,
  ];
  var view = new Float64Array(source);
  assert.equal(view[0], 1);
  assert.equal(view[1], 2);
})();

// Check constructor from Array with its own Symbol.iterator
(function() {
  var source = [1, 2];
  source[Symbol.iterator] = function*() {
    yield 7;
    yield 8;
  };
  var view = new Float64Array(source);
  assert.equal(view[0], 7);
  assert.equal(view[1], 8);
})();

// Check constructor from Array when %ArrayIteratorPrototype%.next is replaced
(function() {
  var iteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
  var originalNext = iteratorPrototype.next;
  iteratorPrototype.next = function() {
    return {value: 11, done: true};
  };
  try {
    var view = new Float64Array([1, 2]);
    assert.equal(view.length, 0);
  } finally {
    iteratorPrototype.next = originalNext;
  }
})();

// Check constructor from Array with arbitrary types
cons.forEach(function(TypedArray) {
  var view = new TypedArray(['1', {}, false]);
  assert.equal(view.length, 3);
  assert.equal(view[0], 1);
  if (TypedArray == Float32Array || TypedArray == Float64Array) {
    assert.equal(view[1], NaN);
  } else {
    assert.equal(view[1], 0);
  }
  assert.equal(view[2], 0);
});

// Check constructor from empty Array
cons.forEach(function(TypedArray) {
  assert.equal(new TypedArray([]).length, 0);
});

// Constructor from undefined should give an empty TypedArray
cons.forEach(function(TypedArray) {
  assert.equal(new TypedArray(undefined).length, 0);
});

// Check constructor from Array-esque object
cons.forEach(function(TypedArray) {
  var arrayEsque = {length: 1};
  arrayEsque[0] = 1;
  var view = new TypedArray(arrayEsque);
  assert.equal(view.length, 1);
  assert.equal(view[0], 1);
});

// Check constructor from iterable (Set)
cons.forEach(function(TypedArray) {
  var set = new Set([1, 2, 3]);
  var view = new TypedArray(set);
  assert.equal(view.length, 3);
  assert.equal(view[0], 1);
  assert.equal(view[1], 2);
  assert.equal(view[2], 3);
});

// Check constructor from iterable (Map.values())
cons.forEach(function(TypedArray) {
  var map = new Map([[0, 10], [1, 20], [2, 30]]);
  var view = new TypedArray(map.values());
  assert.equal(view.length, 3);
  assert.equal(view[0], 10);
  assert.equal(view[1], 20);
  assert.equal(view[2], 30);
});

// Check constructor from iterable (generator)
cons.forEach(function(TypedArray) {
  function* gen() { yield 4; yield 5; yield 6; }
  var view = new TypedArray(gen());
  assert.equal(view.length, 3);
  assert.equal(view[0], 4);
  assert.equal(view[1], 5);
  assert.equal(view[2], 6);
});

// Check that create(iterable) propagates the error thrown by the value getter
// of an iterator result without calling the iterator's return method, as
// IteratorStepValue specifies.
function ValueError() {}
function checkValueErrorDoesNotClose(create) {
  var returnCalls = 0;
  var iterable = {};
  iterable[Symbol.iterator] = function() {
    return {
      next: function() {
        return {
          done: false,
          get value() {
            throw new ValueError();
          },
        };
      },
      return: function() {
        ++returnCalls;
        return {};
      },
    };
  };
  assert.throws(function() {
    create(iterable);
  }, ValueError);
  assert.equal(returnCalls, 0);
}

// Check constructor from iterable whose value getter throws
cons.forEach(function(TypedArray) {
  checkValueErrorDoesNotClose(function(iterable) {
    return new TypedArray(iterable);
  });
});

// Empty constructor
cons.forEach(function(TypedArray) {
  assert.equal(new TypedArray().length, 0);
});

// Constructor with length
cons.forEach(function(TypedArray) {
  var view = new TypedArray(16);
  assert.equal(view.length, 16);
  assert.equal(view[0], 0);
});

// Constructor from buffer but with zero size
cons.forEach(function(TypedArray) {
  var buf = new ArrayBuffer(32);
  var emptyArrayFromOtherBuffer = new TypedArray(buf, 16, 0);
  assert.equal(emptyArrayFromOtherBuffer.length, 0);
});

// Constructor from buffer with length smaller than offset
cons.forEach(function(TypedArray) {
  var buf = new ArrayBuffer(32);
  var arrayFromBufferWithSmallLength = new TypedArray(buf, 8, 2);
  assert.equal(arrayFromBufferWithSmallLength.length, 2);
  assert.equal(arrayFromBufferWithSmallLength[0], 0);
  assert.equal(arrayFromBufferWithSmallLength[1], 0);
});

// Constructor from another TypedArray of the same type
cons.forEach(function(TypedArray) {
  var original = new TypedArray(32);
  var sameTypeArray = new TypedArray(original);
  assert.equal(sameTypeArray.length, original.length);
  // The bytes should have been copied, and reside in a different buffer
  assert.notEqual(sameTypeArray.buffer, original.buffer);
});

(function checkLargeValuesInConstructor() {
  // Check constructor from Array with huge values
  // 256 should wrap around to be 0
  var consFromArray = new Uint8Array([256]);
  assert.equal(consFromArray.length, 1);
  assert.equal(consFromArray[0], 0);
})();

/// @}

// Check for..of loop
cons.forEach(function(TypedArray) {
  var arr = [10, 20, 30]
  var view = new TypedArray(arr);
  var result = [];
  for (var i of view)
    result.push(i);
  assert.arrayEqual(result, arr);
});

// Check for..in loop
cons.forEach(function(TypedArray) {
  var view = new TypedArray(5);
  var result = [];
  for(var i in view)
    result.push(i);
  assert.arrayEqual(result, ["0", "1", "2", "3", "4"]);
  // Detach the buffer. No keys should be enumerable.
  HermesInternal.detachArrayBuffer(view.buffer);
  result = [];
  for(var i in view)
    result.push(i);
  assert.arrayEqual(result, []);
});

// Check basic writability properties
cons.forEach(function(TypedArray) {
  var view = new TypedArray(32);
  for (var i = 0; i < view.length; i++) {
    view[i] = i;
    assert.equal(view[i], i);
  }
});

// Check uint8clamped is clamped
(function checkClamped() {
  var view = new Uint8ClampedArray(1);
  // Writing a value > 255 into uint8clampedview should give 255
  view[0] = 300;
  assert.equal(view[0], 255);
  // Also check for round-to-even behavior
  view[0] = 1.5;
  assert.equal(view[0], 2);
  view[0] = 2.5;
  assert.equal(view[0], 2);
})();

(function checkFloatMaxValue() {
  var view = new Float32Array(1);
  view[0] = Number.MAX_VALUE;
  assert.equal(view[0], Infinity);
})();

(function checkFloatSanitization() {
  // Check float is sanitized
  // Basis of this test is that in Hermes, a NaN can have a "payload" which
  // could represent a pointer. We must ensure that if such a value is read from,
  // the payload is sanitized out
  var buf = new ArrayBuffer(8);
  var uint8view = new Uint8Array(buf);
  var uint32view = new Uint32Array(buf);
  var float32view = new Float32Array(buf);
  var float64view = new Float64Array(buf);
  // all 1's
  uint32view[0] = 0xffffffff;
  assert.equal(float32view[0], NaN);
  // 64-bit version, need to use a uint8 array,
  // 0xff ff 00 01 00 e0 00 58
  // is an example tagged NaN value to sanitize
  if (LE) {
    // little-endian order of bytes
    uint8view[0] = 0x58;
    uint8view[1] = 0x00;
    uint8view[2] = 0xe0;
    uint8view[3] = 0x00;
    uint8view[4] = 0x01;
    uint8view[5] = 0x00;
    uint8view[6] = 0xff;
    uint8view[7] = 0xff;
  } else {
    // big-endian order of bytes
    uint8view[7] = 0xff;
    uint8view[6] = 0xff;
    uint8view[5] = 0x00;
    uint8view[4] = 0x01;
    uint8view[3] = 0x00;
    uint8view[2] = 0xe0;
    uint8view[1] = 0x00;
    uint8view[0] = 0x58;
  }
  assert.equal(float64view[0], NaN);
})();

(function checkConstructorFromDifferentType() {
  // Constructor from another TypedArray of a different type
  var int32view = new Int32Array([0, 1, 2, 3]);
  // This constructor copies the values
  var differentTypeArray = new Int16Array(int32view);
  assert.equal(differentTypeArray.length, int32view.length);
  // The bytes should have been copied
  for (var i = 0; i < differentTypeArray.length; i++) {
    assert.equal(differentTypeArray[i], i);
  }
})();

(function checkSignsInFloat() {
  // Signs in float
  var a = new Float64Array(4);
  a[0] = -0;
  assert.equal(1 / a[0], -Infinity);
})();

/// @name %TypedArray% in and delete
/// @{
cons.forEach(function(TA) {
  var ta = new TA([1,2,3,4,5]);
  assert.equal(false, -1 in ta);
  assert.equal(true, 0 in ta);
  assert.equal(true, 4 in ta);
  assert.equal(false, 5 in ta);

  assert.equal(true, delete ta[-1]);
  assert.throws(_ => delete ta[0], TypeError);
  assert.throws(_ => delete ta[4], TypeError);
  assert.equal(true, delete ta[5]);

  assert.equal(true, Reflect.deleteProperty(ta, -1));
  assert.equal(false, Reflect.deleteProperty(ta, 0));
  assert.equal(false, Reflect.deleteProperty(ta, 4));
  assert.equal(true, Reflect.deleteProperty(ta, 5));
});
/// @}

/// @name %TypedArray%.prototype.buffer
/// @{
cons.forEach(function(TypedArray) {
  assert.throws(function() {
    TypedArray.prototype.buffer;
  }, TypeError);
});
/// @}

/// @name %TypedArray%.prototype.at
/// @{
cons.forEach(function(TA) {
  var ta = new TA([1, 2, 3, 4, 5]);
  assert.equal(ta.at(0).toString(), '1');
  assert.equal(ta.at(-1).toString(), '5');
  assert.equal(ta.at(10), undefined);
  assert.throws(function(){TA.prototype.at.call(ta, 1n)}, TypeError);
  assert.throws(function(){TA.prototype.at.call("hi", 1)}, TypeError);
  assert.throws(function(){TA.prototype.at.call([1, "f", 3], 1)}, TypeError);
});
// @}

/// @name %TypedArray%.prototype.copyWithin
/// @{

cons.forEach(function(TA) {
  assert.equal(new TA([1, 2, 3, 4, 5]).copyWithin(-2).toString(), '1,2,3,1,2');
  assert.equal(new TA([1, 2, 3, 4, 5]).copyWithin(0).toString(), '1,2,3,4,5');
  assert.equal(
    new TA([1, 2, 3, 4, 5]).copyWithin(0, 3, 4).toString(),
    '4,2,3,4,5',
  );
  assert.equal(
    new TA([1, 2, 3, 4, 5]).copyWithin(-2, -3, -1).toString(),
    '1,2,3,3,4',
  );
  if (HermesInternal.detachArrayBuffer) {
    var ta = new TA(1000);
    assert.throws(function() {
      ta.copyWithin(
        {
          valueOf: function() {
            HermesInternal.detachArrayBuffer(ta.buffer);
            return 1;
          },
        },
        4,
        256,
      );
    }, TypeError);
  }
});

(function() {
  // Ensure bit-level preservation.
  var A = new Float32Array(4);
  A[0] = 0 / 0;
  A[1] = -(0 / 0);
  var bytes1 = new Uint8Array(A.buffer, 0, 8);
  A.copyWithin(2, 0); // Copy bytes to the second half of A.
  var bytes2 = new Uint8Array(A.buffer, 8, 8);
  assert.equal(bytes1.length, bytes2.length);
  for (var i = 0; i < bytes1.length; ++i) {
    assert.equal(bytes1[i], bytes2[i]);
  }
})();

/// @}

/// @name %TypedArray%.from
/// @{

cons.forEach(function(c, i) {
  // Function exists.
  assert.ok('from' in c);
  assert.ok(c.from);

  // Works on arrays.
  var ta = c.from([1, 2, 3]);
  assert.equal(ta.length, 3);
  assert.equal(ta[0], 1);
  assert.equal(ta[1], 2);
  assert.equal(ta[2], 3);

  // Works on a different typed array.
  var otherTA = new cons[i == 0 ? cons.length - 1 : i - 1]([1, 2, 3]);
  ta = c.from(otherTA);
  assert.equal(ta.length, 3);
  assert.equal(ta[0], 1);
  assert.equal(ta[1], 2);
  assert.equal(ta[2], 3);

  // "array-like" object.
  var arrayLike = {length: 3};
  arrayLike[0] = 1;
  arrayLike[1] = 2;
  arrayLike[2] = 3;
  ta = c.from(arrayLike);
  assert.equal(ta.length, 3);
  assert.equal(ta[0], 1);
  assert.equal(ta[1], 2);
  assert.equal(ta[2], 3);

  // Array with holes.
  ta = c.from([1, 2, , , 3]);
  assert.equal(ta.length, 5);
  assert.equal(ta[0], 1);
  assert.equal(ta[1], 2);
  // undefined is coerced to zero, except for floats which are NaN.
  if (c === Float32Array || c === Float64Array) {
    assert.equal(ta[2], NaN);
    assert.equal(ta[3], NaN);
  } else {
    assert.equal(ta[2], 0);
    assert.equal(ta[3], 0);
  }
  assert.equal(ta[4], 3);

  // Test calling without the this argument.
  var from = c.from;
  assert.throws(function() {
    from([]);
  }, TypeError);

  // Test an array-like (non-iterable) object whose length is greater than
  // 2 ^ 32 - 1. NOTE: This behavior differs from v8 and JSC, because we use
  // uint32_t as our indexing type. They also disagree with each other.
  // NOTE: TypedArrays have @@iterator, so they use iteration (not length).
  // We use a plain object without @@iterator to test the length limit.
  assert.throws(function() {
    var arrayLike = {
      length: 4294967297, // 2 ^ 32 + 1
      0: 1
    };
    return c.from(arrayLike);
  }, RangeError);

  // Works on iterables (Set).
  var set = new Set([1, 2, 3]);
  ta = c.from(set);
  assert.equal(ta.length, 3);
  assert.equal(ta[0], 1);
  assert.equal(ta[1], 2);
  assert.equal(ta[2], 3);

  // Works on iterables (Map.values()).
  var map = new Map([[0, 10], [1, 20], [2, 30]]);
  ta = c.from(map.values());
  assert.equal(ta.length, 3);
  assert.equal(ta[0], 10);
  assert.equal(ta[1], 20);
  assert.equal(ta[2], 30);

  // Works on iterables (generator).
  function* gen() { yield 4; yield 5; yield 6; }
  ta = c.from(gen());
  assert.equal(ta.length, 3);
  assert.equal(ta[0], 4);
  assert.equal(ta[1], 5);
  assert.equal(ta[2], 6);

  // Works on iterables with mapfn.
  ta = c.from(new Set([1, 2, 3]), function(x) { return x * 2; });
  assert.equal(ta.length, 3);
  assert.equal(ta[0], 2);
  assert.equal(ta[1], 4);
  assert.equal(ta[2], 6);

  // Doesn't close the iterator when a value getter throws.
  checkValueErrorDoesNotClose(function(iterable) {
    return c.from(iterable);
  });
});

(function checkFromArrayIteratorFastPathSemantics() {
  // All the values are read before any of them is converted, so the valueOf
  // of the first element does not change the second value.
  var source = [
    {
      valueOf: function() {
        source[1] = 9;
        return 1;
      },
    },
    2,
  ];
  var ta = Float64Array.from(source);
  assert.equal(ta[0], 1);
  assert.equal(ta[1], 2);

  // Likewise, all the values are read before the mapper runs.
  source = [1, 2];
  ta = Float64Array.from(source, function(value, index) {
    if (index === 0) {
      source[1] = 9;
    }
    return value;
  });
  assert.equal(ta[0], 1);
  assert.equal(ta[1], 2);
})();

/// @}

/// @name %TypedArray%.of
/// @{

cons.forEach(function(TypedArray) {
  // Function exists.
  assert.ok('of' in TypedArray);
  assert.ok(TypedArray.of);
  assert.equal(TypedArray.of.length, 0);

  var ta = TypedArray.of(1, 2, 3);
  assert.equal(ta.length, 3);
  assert.equal(ta[0], 1);
  assert.equal(ta[1], 2);
  assert.equal(ta[2], 3);

  // Works on no arguments.
  ta = TypedArray.of();
  assert.equal(ta.length, 0);

  // Test calling without the this argument.
  var of = TypedArray.of;
  assert.throws(function() {
    of();
  }, TypeError);
});

// Returning a smaller array than requested.
cons.forEach(function(TypedArray) {
  var thisArg = function() {
    return new TypedArray(1);
  };

  assert.throws(function() {
    TypedArray.of.call(thisArg, 1, 2, 3, 4);
  }, TypeError);
});

/// @}

/// @name %TypedArray%.prototype.every && .some
/// @{

cons.forEach(function(TypedArray) {
  var view = new TypedArray([0, 1, 2, 3, 4]);
  // Check that callback function is passed parameters correctly
  assert.ok(
    view.every(function(elem, i, v) {
      return elem === i && v == view;
    }),
  );

  // Check basic properties
  assert.ok(
    view.every(function() {
      return true;
    }),
  );
  assert.ok(
    !view.every(function() {
      return false;
    }),
  );

  assert.ok(
    !view.every(function(elem, i) {
      return i !== 0;
    }),
  );

  // Check that callback function is passed parameters correctly
  assert.ok(
    !view.some(function(elem, i, v) {
      return !(elem === i && v == view);
    }),
  );
  // Check basic properties
  assert.ok(
    !view.some(function() {
      return false;
    }),
  );
  assert.ok(
    view.some(function() {
      return true;
    }),
  );
  assert.ok(
    view.some(function(elem, i) {
      if (i == 0) {
        return true;
      }
    }),
  );
});

/// @}

/// @name %TypedArray%.prototype.fill
/// @{

cons.forEach(function(TypedArray) {
  var view = new TypedArray(4);
  view.fill(1);
  for (var i = 0; i < view.length; i++) {
    assert.equal(view[i], 1);
  }

  view.fill(2, 3);
  for (var i = 0; i < view.length; i++) {
    assert.equal(view[i], i < 3 ? 1 : 2);
  }

  view.fill(0);
  view.fill(1, 4, 6);
  for (var i = 0; i < view.length; i++) {
    assert.equal(view[i], i < 4 || i >= 6 ? 0 : 1);
  }

  view.fill(0);
  view.fill(1, 4, 100);
  for (var i = 0; i < view.length; i++) {
    assert.equal(view[i], i < 4 ? 0 : 1);
  }

  view.fill(0);
  view.fill(1, -100, 4);
  for (var i = 0; i < view.length; i++) {
    assert.equal(view[i], i < 4 ? 1 : 0);
  }

  view.fill(0);
  view.fill(1, -2, -1);
  for (var i = 0; i < view.length; i++) {
    assert.equal(view[i], i < view.length - 2 || i >= view.length - 1 ? 0 : 1);
  }

  view.fill(1);
  // This should not fill any elements.
  view.fill(0, Infinity);
  for (var i = 0; i < view.length; i++) {
    assert.equal(view[i], 1);
  }
  // This should fill all elements, Infinity is past the end.
  view.fill(2, 0, Infinity);
  for (var i = 0; i < view.length; i++) {
    assert.equal(view[i], 2);
  }

  // Calling fill on a detached TypedArray should throw TypeError.
  HermesInternal.detachArrayBuffer(view.buffer);
  assert.throws(function() {
    view.fill(0);
  }, TypeError);

  // Converting the first argument to a number can detach the TypedArray.
  view = new TypedArray(4);
  assert.throws(function() {
    view.fill({
      valueOf: function() {
        HermesInternal.detachArrayBuffer(view.buffer);
        return 1;
      },
    });
  }, TypeError);
});

cons.forEach(function(TypedArray) {
  // Fill with start >= end should remain the same.
  var arr = new TypedArray([0, 0, 0, 0]);
  arr.fill(8, 3, 1);
  assert.equal(arr.length, 4);
  assert.equal(arr[0], 0);
  assert.equal(arr[1], 0);
  assert.equal(arr[2], 0);
  assert.equal(arr[3], 0);

  arr.fill(8, -1, -3);
  assert.equal(arr.length, 4);
  assert.equal(arr[0], 0);
  assert.equal(arr[1], 0);
  assert.equal(arr[2], 0);
  assert.equal(arr[3], 0);
});

/// @}

/// @name TypedArray.prototype.filter
/// @{
cons.forEach(function(TypedArray) {
  var arr = new TypedArray([0, 1, 2, 3]);

  // Result should be the same length and contain the same elements
  var filtered = arr.filter(function(elem, i, view) {
    return elem === i && view === arr;
  });
  // It should be a copy, not the same
  assert.notEqual(filtered, arr);
  assert.equal(filtered.length, arr.length);
  for (var i = 0; i < filtered.length; i++) {
    assert.equal(filtered[i], i);
  }

  filtered = arr.filter(function(_, i) {
    return i >= arr.length - 2 && i < arr.length;
  });
  assert.equal(filtered.length, 2);
  assert.equal(filtered[0], arr.length - 2);
  assert.equal(filtered[1], arr.length - 1);

  assert.throws(function() {
    arr.filter(function() {
      throw new RangeError();
    });
  }, RangeError);
});
/// @}

/// @name TypedArray.prototype.findLast/.findLastIndex
/// @{
cons.forEach(function(TypedArray) {
  var arr = new TypedArray([0, 1, 2, 3]);

  var cb = function(elem, i, view) {
    assert.equal(view, arr);
    assert.equal(elem, i);
    return i === arr.length - 1;
  };
  assert.equal(arr.findLast(cb), arr.length - 1);
  assert.equal(arr.findLastIndex(cb), arr.length - 1);
  var numcalls = 0;
  cb = function() {
    ++numcalls;
    return true;
  };
  assert.equal(arr.findLast(cb), 3);
  assert.equal(arr.findLastIndex(cb), 3);
  assert.equal(numcalls, 2);

  cb = function() {
    return false;
  };
  assert.equal(arr.findLast(cb), undefined);
  assert.equal(arr.findLastIndex(cb), -1);

  var state = [];
  cb = function(elem, i, view) {
    state.push(elem);
    return elem === 2;
  };
  assert.equal(arr.findLast(cb), 2);
  assert.arrayEqual(state, [3, 2]);
});
/// @}

/// @name TypedArray.prototype.find/.findIndex
/// @{
cons.forEach(function(TypedArray) {
  var arr = new TypedArray([0, 1, 2, 3]);

  var cb = function(elem, i, view) {
    assert.equal(view, arr);
    assert.equal(elem, i);
    return i === arr.length - 1;
  };
  assert.equal(arr.find(cb), arr.length - 1);
  assert.equal(arr.findIndex(cb), arr.length - 1);
  var numcalls = 0;
  cb = function() {
    ++numcalls;
    return true;
  };
  assert.equal(arr.find(cb), 0);
  assert.equal(arr.findIndex(cb), 0);
  assert.equal(numcalls, 2);

  cb = function() {
    return false;
  };
  assert.equal(arr.find(cb), undefined);
  assert.equal(arr.findIndex(cb), -1);
  var state = [];
  cb = function(elem, i, view) {
    state.push(elem);
    return elem === 2;
  };
  assert.equal(arr.find(cb), 2);
  assert.arrayEqual(state, [0, 1, 2]);
});
/// @}

/// @name TypedArray.prototype.forEach
/// @{
cons.forEach(function(TypedArray) {
  var arr = new TypedArray([0, 1, 2, 3]);

  assert.equal(
    arr.forEach(function(elem, i, view) {
      assert.equal(elem, i);
      assert.equal(view, arr);
    }),
    undefined,
  );

  var numcalls = 0;
  arr.forEach(function() {
    numcalls++;
  });
  assert.equal(numcalls, arr.length);
});
/// @}

/// @name TypedArray.prototype.includes && .indexOf
/// @{
cons.forEach(function(TypedArray) {
  var arr = new TypedArray([0, 1, 2, 3]);
  assert.ok(arr.includes(1));
  assert.equal(arr.indexOf(1), 1);
  assert.equal(arr.lastIndexOf(1), 1);
  assert.ok(!arr.includes(1000));
  assert.equal(arr.indexOf(1000), -1);
  assert.equal(arr.lastIndexOf(1000), -1);

  assert.ok(arr.includes(0, 0));
  assert.ok(!arr.includes(0, 1));
  assert.ok(!arr.includes(0, 1000));
  assert.equal(arr.indexOf(0, 0), 0);
  assert.equal(arr.indexOf(0, 1), -1);
  assert.equal(arr.indexOf(0, 1000), -1);
  assert.equal(arr.lastIndexOf(0, 0), 0);
  assert.equal(arr.lastIndexOf(0, 1), 0);
  assert.equal(arr.lastIndexOf(0, 1000), 0);

  assert.ok(arr.includes(arr.length - 2, -2));
  assert.ok(!arr.includes(arr.length - 3, -1));
  assert.equal(arr.indexOf(arr.length - 2, -2), arr.length - 2);
  assert.equal(arr.indexOf(arr.length - 3, -1), -1);
  assert.equal(arr.lastIndexOf(arr.length - 2, -2), arr.length - 2);
  assert.equal(arr.lastIndexOf(arr.length - 3, -1), arr.length - 3);

  // Set up duplicates to test indexOf vs lastIndexOf
  arr[0] = 50;
  arr[1] = 50;
  assert.equal(arr.indexOf(50), 0);
  assert.equal(arr.lastIndexOf(50), 1);

  // When the buffer is detached during includes, elements become undefined.
  // Searching for undefined should return true.
  var arr2 = new TypedArray(10);
  assert.ok(arr2.includes(undefined, {
    valueOf() {
      HermesInternal.detachArrayBuffer(arr2.buffer);
      return 0;
    },
  }));

  // Searching for any other value should return false.
  var arr3 = new TypedArray(10);
  assert.ok(!arr3.includes(0, {
    valueOf() {
      HermesInternal.detachArrayBuffer(arr3.buffer);
      return 0;
    },
  }));
  // When the buffer is detached during indexOf, it should always return -1.
  var arr4 = new TypedArray(10);
  assert.equal(arr4.indexOf(undefined, {
    valueOf() {
      HermesInternal.detachArrayBuffer(arr4.buffer);
      return 0;
    },
  }), -1);

  var arr5 = new TypedArray(10);
  assert.equal(arr5.indexOf(0, {
    valueOf() {
      HermesInternal.detachArrayBuffer(arr5.buffer);
      return 0;
    },
  }), -1);

  // When the buffer is detached during lastIndexOf, it should always return -1.
  var arr6 = new TypedArray(10);
  assert.equal(arr6.lastIndexOf(undefined, {
    valueOf() {
      HermesInternal.detachArrayBuffer(arr6.buffer);
      return 0;
    },
  }), -1);

  var arr7 = new TypedArray(10);
  assert.equal(arr7.lastIndexOf(0, {
    valueOf() {
      HermesInternal.detachArrayBuffer(arr7.buffer);
      return 0;
    },
  }), -1);
});
/// @}

/// @name TypedArray.prototype.join && .toString (since toString calls join)
/// @{
cons.forEach(function(ta) {
  var arr = new ta([1, 2, 3]);
  assert.equal(arr.join(), '1,2,3');
  assert.equal(arr.join(','), '1,2,3');
  assert.equal(arr.join(''), '123');
  assert.equal(arr.join('asdf'), '1asdf2asdf3');

  // Test empty case.
  assert.equal(new ta([]).join(), '');

  // Test toString.
  assert.equal(ta.prototype.toString, Array.prototype.toString);
  assert.equal(arr.toString(), arr.join());
  // Test difference between integral and floating toString
  if (ta === Float32Array || ta === Float64Array) {
    assert.equal(new ta([{}]).toString(), 'NaN');
  } else {
    assert.equal(new ta([{}]).toString(), '0', ta.toString());
  }
});
/// @}

/// @name TypedArray.prototype.reverse
/// @{
cons.forEach(function(TypedArray) {
  var arr = new TypedArray([0, 1, 2, 3]);

  arr.reverse();
  for (var i = 0; i < arr.length; i++) {
    assert.equal(arr[i], arr.length - 1 - i);
  }
  var emptyReversed = new (Object.getPrototypeOf(arr)).constructor(
    [],
  ).reverse();
  assert.equal(emptyReversed.length, 0);
  var oddReversed = new (Object.getPrototypeOf(arr)).constructor([
    1,
    2,
    3,
  ]).reverse();
  assert.equal(oddReversed.length, 3);
  assert.equal(oddReversed[0], 3);
  assert.equal(oddReversed[1], 2);
  assert.equal(oddReversed[2], 1);
});
/// @}

/// @name TypedArray.prototype.sort
/// @{
cons.forEach(function(ta) {
  var x = new ta([3, 2, 1]);
  x.sort();
  assert.equal(x[0], 1);
  assert.equal(x[1], 2);
  assert.equal(x[2], 3);

  // Check empty doesn't crash.
  x = new ta([]);
  x.sort();
  assert.equal(x.length, 0);

  // Check with comparefn.
  x = new ta([3, 2, 1]);
  // Use the reverse sorter, normal definition is a - b.
  x.sort(function(a, b) {
    return b - a;
  });
  assert.equal(x[0], 3);
  assert.equal(x[1], 2);
  assert.equal(x[2], 1);

  // Check stable.
  x = new ta([1, 111, 11, 22, 2, 33, 3]);
  x.sort(function(a, b) {
    return (a + "").length - (b + "").length;
  });
  assert.equal(x[0], 1);
  assert.equal(x[1], 2);
  assert.equal(x[2], 3);
  assert.equal(x[3], 11);
  assert.equal(x[4], 22);
  assert.equal(x[5], 33);
  assert.equal(x[6], 111);

  assert.throws(function() {
    x.sort(null);
  }, TypeError);
  assert.throws(function() {
    x.sort(true);
  }, TypeError);
  assert.throws(function() {
    x.sort(12);
  }, TypeError);
  assert.throws(function() {
    x.sort({});
  }, TypeError);

  // Exercise the comparator path with a larger captured list.
  x = new ta(104);
  x.sort(function(unused1, unused2) {
    return true;
  });
  for (var i = 0; i < x.length; i++) {
    assert.equal(x[i], 0);
  }

  // Sort the captured values even if the comparator changes the receiver.
  x = new ta([3, 2, 1]);
  var calls = 0;
  x.sort(function(a, b) {
    if (calls++ === 0) {
      x.fill(0);
    }
    return a - b;
  });
  assert.equal(x[0], 1);
  assert.equal(x[1], 2);
  assert.equal(x[2], 3);

  // Detaching does not change the values passed to later comparator calls.
  x = new ta([3, 2, 1]);
  calls = 0;
  var result = x.sort(function(a, b) {
    assert.notEqual(a, undefined);
    assert.notEqual(b, undefined);
    if (calls++ === 0) {
      HermesInternal.detachArrayBuffer(x.buffer);
    }
    return a - b;
  });
  assert.equal(result, x);
  assert.ok(calls > 1);
});

/// Bigint are represented as a heap object when encoded to HermesValue. This
/// tests that they remain alive across GC.
(function bigintSortSnapshotSurvivesGC() {
  var x = new BigInt64Array([3n, 2n, 1n]);
  x.sort(function(a, b) {
    gc();
    return a < b ? -1 : a > b ? 1 : 0;
  });
  assert.equal(x[0], 1n);
  assert.equal(x[1], 2n);
  assert.equal(x[2], 3n);
})();

(function bigintElementWrites() {
  var int64MinBit = 9223372036854775808n; // 2^63
  var uint64Max = 18446744073709551615n;  // 2^64 - 1
  var uint64Mod = 18446744073709551616n;  // 2^64

  var u = new BigUint64Array(3);
  u[0] = int64MinBit;
  u[1] = uint64Max;
  u[2] = uint64Mod;
  assert.equal(u[0], int64MinBit);
  assert.equal(u[1], uint64Max);
  assert.equal(u[2], 0n);

  var s = new BigInt64Array(3);
  s[0] = int64MinBit;
  s[1] = uint64Max;
  s[2] = uint64Mod;
  assert.equal(s[0], -int64MinBit);
  assert.equal(s[1], -1n);
  assert.equal(s[2], 0n);
})();

(function negativeZeroSort() {
  var x = new Float64Array([+0, -0]);
  x.sort();
  assert.equal(1 / x[0], -Infinity);
  assert.equal(1 / x[1], +Infinity);
})();

// NaN must sort to the end.
(function nanSort() {
  var a = new Float64Array([NaN, -Infinity, 0, Infinity, 1, -1, NaN]);
  a.sort();
  assert.equal(a[0], -Infinity);
  assert.equal(a[1], -1);
  assert.equal(a[2], 0);
  assert.equal(a[3], 1);
  assert.equal(a[4], Infinity);
  assert.equal(a[5], NaN);
  assert.equal(a[6], NaN);

  var b = new Float32Array([NaN, 3, -0, 0, NaN, -3]);
  b.sort();
  assert.equal(b[0], -3);
  assert.equal(1 / b[1], -Infinity); // -0
  assert.equal(b[2], 0);
  assert.equal(b[3], 3);
  assert.equal(b[4], NaN);
  assert.equal(b[5], NaN);
})();

/// @}

/// @name TypedArray.prototype.set
/// @{
(function checkPrototypeSet() {
  cons.forEach(function(ta, i) {
    var dst = new ta([1, 2, 3]);
    // Check setting to Object.
    dst.set([5]);
    assert.equal(dst[0], 5);
    // Check setting at an offset.
    dst.set([5], 2);
    assert.equal(dst[2], 5);
    assert.notEqual(dst[1], 5);
    // Check setting to larger object.
    dst.set([3, 2, 1]);
    assert.equal(dst[0], 3);
    assert.equal(dst[1], 2);
    assert.equal(dst[2], 1);

    // Check exception case of trying to set too large of an object.
    assert.throws(function() {
      dst.set([1, 2, 3, 4]);
    }, RangeError);
    assert.throws(function() {
      dst.set([1], 10);
    }, RangeError);

    dst = new ta([4, 5, 6]);
    // Check with other TypedArray of the same type.
    var other = new ta([1, 2]);
    dst.set(other);
    assert.equal(dst[0], 1);
    assert.equal(dst[1], 2);
    assert.equal(dst[2], 6);

    // Check with TypedArray of a different type.
    var otherCons = cons[i == 0 ? cons.length - 1 : i - 1];
    other = new otherCons([1, 2]);
    dst.set(other);
    assert.equal(dst[0], 1);
    assert.equal(dst[1], 2);
    assert.equal(dst[2], 6);

    // Check with self.
    dst = new ta([0, 1, 2]);
    dst.set(dst);
    for (var j = 0; j < dst.length; j++) {
      assert.equal(dst[j], j);
    }
    // New TypedArray pointing to the same memory location, but at a different
    // starting point.
    other = new ta(dst.buffer, getByteWidth(ta));
    assert.equal(other.length, dst.length - 1);
    assert.equal(other[0], 1);
    assert.equal(other[1], 2);
    // This should move the rest of the array leftwards 1 space.
    dst.set(other);
    for (var j = 0; j < dst.length - 1; j++) {
      assert.equal(dst[j], j + 1);
    }
    assert.equal(dst[dst.length - 1], dst.length - 1);

    // Check with setting to an empty TypedArray
    dst = new ta();
    dst.set(new ta());
    // Set empty to non-empty
    dst = new ta([1, 2, 3]);
    dst.set(new ta());
    // Set non-empty to empty should throw RangeError
    dst = new ta();
    assert.throws(function() {
      dst.set(new ta([1, 2, 3]));
    }, RangeError);
  });
  // Moving from one typed array view to another of the same data.
  var dst = new Int8Array([0, 1, 2, 3, 4, 5, 6, 7]);
  var src = new Int32Array(dst.buffer);
  dst.set(src);
  assert.equal(dst[0], 0);
  assert.equal(dst[1], 4);
  for (var i = 2; i < dst.length; i++) {
    assert.equal(dst[i], i);
  }
  // Check in reverse.
  dst = new Int8Array([7, 6, 5, 4, 3, 2, 1, 0]);
  src = new Int32Array(dst.buffer);
  dst.set(src);
  assert.equal(dst[0], 7);
  assert.equal(dst[1], 3);
  for (var i = 2; i < dst.length; i++) {
    assert.equal(dst[i], dst.length - 1 - i);
  }

  // Check smaller into bigger as well.
  dst = new Int32Array([5, 5, 0]);
  src = new Int8Array(dst.buffer, 9);
  // This should move the rest of the array leftwards 1 space.
  dst.set(src);
  assert.equal(dst[0], 0);
  assert.equal(dst[1], 0);
  assert.equal(dst[2], 0);

  // Check overlapping and src is before dst
  var buf = new Int8Array([1, 2, 3, 4]).buffer;
  dst = new Int8Array(buf, 2);
  src = new Int8Array(buf, 1, 2);
  dst.set(src);
  assert.equal(dst[0], 2);
  assert.equal(dst[1], 3);

  // Check that bitwise equivalence is preserved.
  var bytes = new Uint8Array([0xff, 0xff, 0xff, 0xff]);
  var flt = new Float32Array([1]);
  flt.set(new Float32Array(bytes.buffer));
  var chk = new Uint8Array(flt.buffer);
  assert.equal(chk[0], 0xff);
  assert.equal(chk[1], 0xff);
  assert.equal(chk[2], 0xff);
  assert.equal(chk[3], 0xff);

  // Check that detach check does not raise type error and following accesses
  // return undefined.
  var typedarray = new Uint8Array(16);
  var evilNumber = {
    valueOf: function() {
      HermesInternal.detachArrayBuffer(typedarray.buffer);
      return 0;
    },
  };
  typedarray.set([evilNumber, 5, 5], 5);
  assert.equal(typedarray[0], undefined);
})();

/// @name TypedArray.prototype.slice
/// @{
(function checkPrototypeSlice() {
  cons.forEach(function(TypedArray) {
    var arr = new TypedArray([0, 1, 2, 3]);
    var x = arr.slice(0, 2);
    assert.equal(x.length, 2);
    assert.equal(x[0], arr[0]);
    assert.equal(x[1], arr[1]);

    // Check for the case where end is undefined.
    x = arr.slice(1);
    assert.equal(x.length, arr.length - 1);
    for (var i = 0; i < x.length; i++) {
      assert.equal(x[i], i + 1);
    }

    // Check negatives.
    x = arr.slice(-2, -1);
    assert.equal(x.length, 1);
    assert.equal(x[0], arr.length - 2);

    x = arr.slice(-2);
    assert.equal(x.length, 2);
    assert.equal(x[0], arr.length - 2);
    assert.equal(x[1], arr.length - 1);

    // Check empty.
    arr = new (Object.getPrototypeOf(arr)).constructor([]);
    x = arr.slice(0);
    assert.equal(x.length, 0);
  });
  // Check that byte-wise equality is maintained.
  var bytes = new Uint8Array([0xff, 0xff, 0xff, 0xff]);
  var chk = new Uint8Array(new Float32Array(bytes.buffer).slice(0).buffer);
  assert.equal(chk[0], 0xff);
  assert.equal(chk[1], 0xff);
  assert.equal(chk[2], 0xff);
  assert.equal(chk[3], 0xff);
})();
/// @}

/// @name TypedArray.prototype.subarray
/// @{
cons.forEach(function(ta) {
  var x = new ta([1, 2, 3]);
  // Check when it's the full length.
  var y = x.subarray(0, x.length);
  assert.equal(y.length, x.length);
  // They should share a buffer.
  assert.equal(y.buffer, x.buffer);
  for (var i = 0; i < x.length; i++) {
    assert.equal(y[i], x[i]);
  }
  // Check undefined second parameter.
  y = x.subarray(0);
  assert.equal(y.length, x.length);
  for (var i = 0; i < x.length; i++) {
    assert.equal(y[i], x[i]);
  }
  // Check smaller range.
  y = x.subarray(1);
  assert.equal(y.length, x.length - 1);
  for (var i = 0; i < y.length; i++) {
    assert.equal(y[i], x[i + 1]);
  }
  // Check negative start.
  y = x.subarray(-1);
  assert.equal(y.length, 1);
  assert.equal(y[0], x[x.length - 1]);

  // Check negative end.
  y = x.subarray(1, -1);
  assert.equal(y.length, x.length - 2);
  for (var i = 0; i < y.length; i++) {
    assert.equal(y[i], x[i + 1]);
  }

  // Check negative start and end.
  y = x.subarray(-2, -1);
  assert.equal(y.length, 1);
  assert.equal(y[0], x[x.length - 2]);

  // Check end < begin (zero length result).
  y = x.subarray(2, 0);
  assert.equal(y.length, 0);

  // Check that zero-sized array yields another zero size array.
  y = new ta(0).subarray();
  assert.equal(y.length, 0);

  // Check that creating a subarray from a subarray works, and doesn't read
  // off the end of the original.
  y = new ta(10);
  var z = y.subarray(5);
  z.subarray();
});

/// @name TypedArray.prototype.toLocaleString
/// @{
cons.forEach(function(TypedArray) {
  var arr = new TypedArray([1, 2, 3]);
  assert.equal(arr.toLocaleString(), '1,2,3');
  var n = 0;
  var oldToLocale = Number.prototype.toLocaleString;
  Number.prototype.toLocaleString = function() {
    ++n;
    return this;
  };
  assert.equal(arr.toLocaleString(), '1,2,3');
  assert.equal(n, 3);
  Number.prototype.toLocaleString = oldToLocale;
});

/// @name TypedArray.prototype.map
/// @{
cons.forEach(function(TypedArray) {
  var arr = new TypedArray([0, 1, 2, 3]);
  var other = arr.map(function(elem) {
    return elem + 1;
  });
  // Should be a copy, not the same.
  assert.notEqual(other, arr);
  assert.equal(other.length, arr.length);
  for (var i = 0; i < arr.length; i++) {
    assert.equal(other[i], arr[i] + 1);
  }
  // Check non-number usages.
  other = arr.map(function(elem) {
    return {
      valueOf: function() {
        return 0;
      },
    };
  });
  assert.equal(other.length, arr.length);
  for (var i = 0; i < other.length; i++) {
    assert.equal(other[i], 0);
  }
});
/// @}

/// @name TypedArray.prototype.reduce && reduceRight
/// @{
cons.forEach(function(TypedArray) {
  var arr = new TypedArray([0, 1, 2, 3]);
  var count = function(acc) {
    return acc + 1;
  };
  var sum = function(acc, elem) {
    return acc + elem;
  };
  // sum(0, 1, ..., len - 1) == (len - 1) * (len) / 2
  var totalsum = (arr.length * (arr.length - 1)) / 2;
  assert.equal(arr.reduce(count, 0), arr.length);
  assert.equal(arr.reduce(sum), totalsum);
  assert.equal(arr.reduce(sum, 0), totalsum);
  assert.equal(arr.reduceRight(count, 0), arr.length);
  assert.equal(arr.reduceRight(sum), totalsum);
  assert.equal(arr.reduceRight(sum, 0), totalsum);

  // Check that it properly uses the first element
  assert.equal(
    arr.reduce(function(acc) {
      return acc;
    }),
    0,
  );
  assert.equal(
    arr.reduce(function(acc) {
      return acc;
    }, 1),
    1,
  );
  assert.equal(
    arr.reduceRight(function(acc) {
      return acc;
    }),
    arr.length - 1,
  );
  assert.equal(
    arr.reduceRight(function(acc) {
      return acc;
    }, 1),
    1,
  );

  // Check non-objects:
  var anEmptyObject = {};
  assert.equal(
    arr.reduce(function() {
      return anEmptyObject;
    }),
    anEmptyObject,
  );
  assert.equal(
    arr.reduceRight(function() {
      return anEmptyObject;
    }),
    anEmptyObject,
  );

  // Check empty array.
  var empty = new (Object.getPrototypeOf(arr)).constructor([]);
  var numcalls = 0;
  assert.throws(function() {
    empty.reduce(function() {});
  }, TypeError);
  assert.equal(
    empty.reduce(function() {
      numcalls++;
    }, 0),
    0,
  );
  assert.equal(numcalls, 0);
  numcalls = 0;
  assert.throws(function() {
    empty.reduceRight(function() {});
  }, TypeError);
  assert.equal(
    empty.reduceRight(function() {
      numcalls++;
    }, 0),
    0,
  );
  assert.equal(numcalls, 0);

  // Check reduce and reduceRight are different
  assert.equal(
    arr.reduce(function(acc, elem) {
      return elem;
    }),
    arr.length - 1,
  );
  assert.equal(
    arr.reduceRight(function(acc, elem) {
      return elem;
    }),
    0,
  );
});
/// @}

/// @name Exception cases
/// @{

// Constructor with non-evenly-divisable length
assert.throws(function() {
  new Int32Array(new ArrayBuffer(9));
}, RangeError);

// Constructor with negative offset
assert.throws(function() {
  new Int8Array(new ArrayBuffer(8), -1);
}, RangeError);

// Constructor with offset not aligned
assert.throws(function() {
  new Int32Array(new ArrayBuffer(8), 3);
}, RangeError);

// Constructor with offset out of bounds
assert.throws(function() {
  new Int8Array(new ArrayBuffer(8), 9);
}, RangeError);

// Constructor with offset + length out of bounds
assert.throws(function() {
  new Int8Array(new ArrayBuffer(8), 5, 5);
}, RangeError);

// TypedArray shouldn't be a symbol.
assert.throws(function() {
  print(TypedArray);
}, ReferenceError);

/// @}

print("OK");
//CHECK-NEXT:OK
