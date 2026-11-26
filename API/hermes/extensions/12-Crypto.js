/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Crypto extension setup function.
// Receives a native helper that fills a TypedArray with cryptographically
// strong random bytes, and installs crypto.getRandomValues globally per the
// W3C Web Crypto API.
  extensions.Crypto = function(nativeRandomFill) {
    // Capture intrinsics before any user code runs, so getRandomValues keeps
    // working even if user code later replaces the globals or prototype
    // accessors.
    var CapturedTypeError = TypeError;
    var CapturedRangeError = RangeError;

    // %TypedArray%.prototype, the common prototype of all concrete TypedArray
    // prototypes. Its Symbol.toStringTag getter returns the constructor name
    // for genuine TypedArrays and undefined for everything else, making it a
    // tamper-proof brand check. The byteLength getter throws for anything
    // without the TypedArray internal slots.
    var typedArrayProto = Object.getPrototypeOf(Int8Array.prototype);
    var getTag = Function.prototype.call.bind(
        Object.getOwnPropertyDescriptor(typedArrayProto, Symbol.toStringTag)
            .get);
    var getByteLength = Function.prototype.call.bind(
        Object.getOwnPropertyDescriptor(typedArrayProto, 'byteLength').get);

    // Integer TypedArray kinds accepted by the spec. Null prototype so a
    // polluted Object.prototype cannot make the lookup succeed.
    var integerTags = {
      __proto__: null,
      Int8Array: true,
      Uint8Array: true,
      Uint8ClampedArray: true,
      Int16Array: true,
      Uint16Array: true,
      Int32Array: true,
      Uint32Array: true,
      BigInt64Array: true,
      BigUint64Array: true,
    };

    function getRandomValues(array) {
      var tag = getTag(array);
      if (integerTags[tag] !== true) {
        // The spec calls for a TypeMismatchError DOMException; Hermes has no
        // DOMException, so throw the closest ECMAScript equivalent.
        throw new CapturedTypeError(
            'crypto.getRandomValues: argument must be an integer TypedArray');
      }
      if (getByteLength(array) > 65536) {
        // The spec calls for a QuotaExceededError DOMException.
        throw new CapturedRangeError(
            'crypto.getRandomValues: byte length exceeds the quota (65536)');
      }
      nativeRandomFill(array);
      return array;
    }

    var crypto = {};
    crypto.getRandomValues = getRandomValues;

    Object.defineProperty(crypto, Symbol.toStringTag, {
      value: 'Crypto',
      writable: false,
      enumerable: false,
      configurable: true
    });

    globalThis.crypto = crypto;
  };
