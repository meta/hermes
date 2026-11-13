/* @nolint */
/**
 * MIT License
 * Portions Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 ECMAScript Shims
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// ==========================
// Iterator Helper Dependencies
// Dependencies required by the iterator polyfill in https://github.com/es-shims/iterator-helpers/tree/main
// ==========================

// === Basic Utility Functions ===

var $isNaN = Number.isNaN;

// From math-intrinsics/abs.js
var abs = Math.abs;

// From math-intrinsics/floor.js
var floor = Math.floor;

// From math-intrinsics/max.js
var max = Math.max;

// From math-intrinsics/min.js
var min = Math.min;

// From math-intrinsics/pow.js
var pow = Math.pow;

// From math-intrinsics/round.js
var round = Math.round;

// From math-intrinsics/sign.js
var sign = Math.sign;

// === Object Operations ===

// From es-object-atoms/index.js
var $Object = Object;

var gopd = Object.getOwnPropertyDescriptor;

var $defineProperty = Object.defineProperty;

// From has-property-descriptors/index.js
var hasPropertyDescriptors = function hasPropertyDescriptors() {
	return !!$defineProperty;
};

hasPropertyDescriptors.hasArrayLengthDefineBug = function hasArrayLengthDefineBug() {
	return false;
}

// === Function Binding ===

// From call-bind-apply-helpers/functionCall.js
var $call = Function.prototype.call;

// From call-bind-apply-helpers/functionApply.js
var $apply = Function.prototype.apply;

// From call-bind-apply-helpers/reflectApply.js
var $reflectApply = typeof Reflect !== 'undefined' && Reflect && Reflect.apply;

// From function-bind/index.js
var bind = Function.prototype.bind

// From call-bind-apply-helpers/actualApply.js
var $actualApply = $reflectApply || bind.call($call, $apply);

// From call-bind-apply-helpers/index.js
var callBindBasic = function callBindBasic(args) {
	if (args.length < 1 || typeof args[0] !== 'function') {
				throw new TypeError('a function is required');
	}
	return $actualApply(bind, $call, args);
};

// From hasown/index.js
var hasown = (function() {
    var call = Function.prototype.call;
    var $hasOwn = Object.prototype.hasOwnProperty;
    return bind.call(call, $hasOwn);
})();

// === Symbol Support ===

// Hermes supports Symbols, but to make the polyfill easy to port, keep this variable
var hasSymbols = function hasSymbols() {
	return true;
}

// === Object Keys Implementation ===
var objectKeys = Object.keys;

// === Prototype Utilities ===

// From get-proto/Reflect.getPrototypeOf.js
var $ReflectGPO = (typeof Reflect !== 'undefined' && Reflect.getPrototypeOf) || null;

// From get-proto/Object.getPrototypeOf.js
var $ObjectGPO = $Object.getPrototypeOf;

// From dunder-proto/get.js
var getDunderProto = (function() {
	var hasProtoAccessor;
	try {
		// eslint-disable-next-line no-extra-parens, no-proto
		hasProtoAccessor = [].__proto__ === Array.prototype;
	} catch (e) {
		if (!e || typeof e !== 'object' || !('code' in e) || e.code !== 'ERR_PROTO_ACCESS') {
			throw e;
		}
	}

	// eslint-disable-next-line no-extra-parens
	var desc = !!hasProtoAccessor && gopd && gopd(Object.prototype, '__proto__');

	var $getPrototypeOf = $Object.getPrototypeOf;

	return desc && typeof desc.get === 'function'
		? callBindBasic([desc.get])
		: typeof $getPrototypeOf === 'function'
			? function getDunder(value) {
				// eslint-disable-next-line eqeqeq
				return $getPrototypeOf(value == null ? value : $Object(value));
			}
			: false;
})();

// From get-proto/index.js
var getProto = $ReflectGPO
	? function getProto(O) {
		return $ReflectGPO(O);
	}
	: $ObjectGPO
		? function getProto(O) {
			if (!O || (typeof O !== 'object' && typeof O !== 'function')) {
				throw new TypeError('getProto: not an object');
			}
			return $ObjectGPO(O);
		}
		: getDunderProto
			? function getProto(O) {
				return getDunderProto(O);
			}
			: null;

// === Property Definition ===

// From functions-have-names/index.js
var functionsHaveNames = function functionsHaveNames() {
	return typeof function f() {}.name === 'string';
};

functionsHaveNames.functionsHaveConfigurableNames = function functionsHaveConfigurableNames() {
	if (!functionsHaveNames() || !gopd) {
		return false;
	}
	var desc = gopd(function () {}, 'name');
	return !!desc && !!desc.configurable;
};

var $bind = Function.prototype.bind;

functionsHaveNames.boundFunctionsHaveNames = function boundFunctionsHaveNames() {
	return functionsHaveNames() && typeof $bind === 'function' && function f() {}.bind().name !== '';
};

// From define-data-property/index.js
var defineDataProperty = function defineDataProperty(
	obj,
	property,
	value
) {
	if (!obj || (typeof obj !== 'object' && typeof obj !== 'function')) {
		throw new TypeError('`obj` must be an object or a function`');
	}
	if (typeof property !== 'string' && typeof property !== 'symbol') {
		throw new TypeError('`property` must be a string or a symbol`');
	}
	if (arguments.length > 3 && typeof arguments[3] !== 'boolean' && arguments[3] !== null) {
		throw new TypeError('`nonEnumerable`, if provided, must be a boolean or null');
	}
	if (arguments.length > 4 && typeof arguments[4] !== 'boolean' && arguments[4] !== null) {
		throw new TypeError('`nonWritable`, if provided, must be a boolean or null');
	}
	if (arguments.length > 5 && typeof arguments[5] !== 'boolean' && arguments[5] !== null) {
		throw new TypeError('`nonConfigurable`, if provided, must be a boolean or null');
	}
	if (arguments.length > 6 && typeof arguments[6] !== 'boolean') {
		throw new TypeError('`loose`, if provided, must be a boolean');
	}

	var nonEnumerable = arguments.length > 3 ? arguments[3] : null;
	var nonWritable = arguments.length > 4 ? arguments[4] : null;
	var nonConfigurable = arguments.length > 5 ? arguments[5] : null;
	var loose = arguments.length > 6 ? arguments[6] : false;

	/* @type {false | TypedPropertyDescriptor<unknown>} */
	var desc = !!gopd && gopd(obj, property);

	if ($defineProperty) {
		$defineProperty(obj, property, {
			configurable: nonConfigurable === null && desc ? desc.configurable : !nonConfigurable,
			enumerable: nonEnumerable === null && desc ? desc.enumerable : !nonEnumerable,
			value: value,
			writable: nonWritable === null && desc ? desc.writable : !nonWritable
		});
	} else if (loose || (!nonEnumerable && !nonWritable && !nonConfigurable)) {
		// must fall back to [[Set]], and was not explicitly asked to make non-enumerable, non-writable, or non-configurable
		obj[property] = value; // eslint-disable-line no-param-reassign
	} else {
		throw new SyntaxError('This environment does not support defining a property as non-configurable, non-writable, or non-enumerable.');
	}
};

// From define-properties/index.js
var defineProperties = function(object, map) {
	var toStr = Object.prototype.toString;
	var concat = Array.prototype.concat;
	var supportsDescriptors = hasPropertyDescriptors();

	var isFunction = function (fn) {
		return typeof fn === 'function' && toStr.call(fn) === '[object Function]';
	};

	var defineProperty = function (object, name, value, predicate) {
		if (name in object) {
			if (predicate === true) {
				if (object[name] === value) {
					return;
				}
			} else if (!isFunction(predicate) || !predicate()) {
				return;
			}
		}

		if (supportsDescriptors) {
			defineDataProperty(object, name, value, true);
		} else {
			defineDataProperty(object, name, value);
		}
	};

	var predicates = arguments.length > 2 ? arguments[2] : {};
	var props = objectKeys(map);
	if (hasSymbols()) {
		props = concat.call(props, Object.getOwnPropertySymbols(map));
	}
	for (var i = 0; i < props.length; i += 1) {
		defineProperty(object, props[i], map[props[i]], predicates[props[i]]);
	}
};

defineProperties.supportsDescriptors = !!hasPropertyDescriptors();

// From set-function-name/index.js
var setFunctionName = function setFunctionName(fn, name) {
	if (typeof fn !== 'function') {
		throw new TypeError('`fn` is not a function');
	}
	var loose = arguments.length > 2 && !!arguments[2];
	if (!loose || functionsHaveNames.functionsHaveConfigurableNames()) {
		if (hasPropertyDescriptors()) {
			defineDataProperty(fn, 'name', name, true, true);
		} else {
			defineDataProperty(fn, 'name', name);
		}
	}
	return fn;
};

// === Get Intrinsics ===

// From call-bound/index.js
var callBound = function callBoundIntrinsic(name, allowMissing) {
	/* eslint no-extra-parens: 0 */

	var intrinsic = GetIntrinsic(name, !!allowMissing);
	var $indexOf = callBindBasic([Function.prototype.indexOf || String.prototype.indexOf]);
	if (typeof intrinsic === 'function' && $indexOf(name, '.prototype.') > -1) {
		return callBindBasic([intrinsic]);
	}
	return intrinsic;
};


// Massive get-intrinsic implementation
// From get-intrinsic/index.js
var GetIntrinsic = (function() {
	var undefined;

	var throwTypeError = function () {
		throw new TypeError();
	};
	var ThrowTypeError = gopd
		? (function () {
			try {
				// eslint-disable-next-line no-unused-expressions, no-caller, no-restricted-properties
				arguments.callee; // IE 8 does not throw here
				return throwTypeError;
			} catch (calleeThrows) {
				try {
					// IE 8 throws on Object.getOwnPropertyDescriptor(arguments, '')
					return gopd(arguments, 'callee').get;
				} catch (gOPDthrows) {
					return throwTypeError;
				}
			}
		}())
		: throwTypeError;

	var TypedArray = typeof Uint8Array === 'undefined' || !getProto ? undefined : getProto(Uint8Array);

	var INTRINSICS = {
		__proto__: null,
		'%AggregateError%': typeof AggregateError === 'undefined' ? undefined : AggregateError,
		'%Array%': Array,
		'%ArrayBuffer%': typeof ArrayBuffer === 'undefined' ? undefined : ArrayBuffer,
		'%ArrayIteratorPrototype%': hasSymbols() && getProto ? getProto([][Symbol.iterator]()) : undefined,
		'%AsyncFromSyncIteratorPrototype%': undefined,
		'%Atomics%': typeof Atomics === 'undefined' ? undefined : Atomics,
		'%BigInt%': typeof BigInt === 'undefined' ? undefined : BigInt,
		'%BigInt64Array%': typeof BigInt64Array === 'undefined' ? undefined : BigInt64Array,
		'%BigUint64Array%': typeof BigUint64Array === 'undefined' ? undefined : BigUint64Array,
		'%Boolean%': Boolean,
		'%DataView%': typeof DataView === 'undefined' ? undefined : DataView,
		'%Date%': Date,
		'%decodeURI%': decodeURI,
		'%decodeURIComponent%': decodeURIComponent,
		'%encodeURI%': encodeURI,
		'%encodeURIComponent%': encodeURIComponent,
		'%Error%': Error,
		'%eval%': eval, // eslint-disable-line no-eval
		'%EvalError%': EvalError,
		'%Float16Array%': typeof Float16Array === 'undefined' ? undefined : Float16Array,
		'%Float32Array%': typeof Float32Array === 'undefined' ? undefined : Float32Array,
		'%Float64Array%': typeof Float64Array === 'undefined' ? undefined : Float64Array,
		'%FinalizationRegistry%': typeof FinalizationRegistry === 'undefined' ? undefined : FinalizationRegistry,
		'%Function%': Function,
		'%Int8Array%': typeof Int8Array === 'undefined' ? undefined : Int8Array,
		'%Int16Array%': typeof Int16Array === 'undefined' ? undefined : Int16Array,
		'%Int32Array%': typeof Int32Array === 'undefined' ? undefined : Int32Array,
		'%isFinite%': isFinite,
		'%isNaN%': isNaN,
		'%IteratorPrototype%': hasSymbols() && getProto ? getProto(getProto([][Symbol.iterator]())) : undefined,
		'%JSON%': typeof JSON === 'object' ? JSON : undefined,
		'%Map%': typeof Map === 'undefined' ? undefined : Map,
		'%MapIteratorPrototype%': typeof Map === 'undefined' || !hasSymbols() || !getProto ? undefined : getProto(new Map()[Symbol.iterator]()),
		'%Math%': Math,
		'%Number%': Number,
		'%Object%': $Object,
		'%Object.getOwnPropertyDescriptor%': gopd,
		'%parseFloat%': parseFloat,
		'%parseInt%': parseInt,
		'%Promise%': typeof Promise === 'undefined' ? undefined : Promise,
		'%Proxy%': typeof Proxy === 'undefined' ? undefined : Proxy,
		'%RangeError%': RangeError,
		'%ReferenceError%': ReferenceError,
		'%Reflect%': typeof Reflect === 'undefined' ? undefined : Reflect,
		'%RegExp%': RegExp,
		'%Set%': typeof Set === 'undefined' ? undefined : Set,
		'%SetIteratorPrototype%': typeof Set === 'undefined' || !hasSymbols() || !getProto ? undefined : getProto(new Set()[Symbol.iterator]()),
		'%SharedArrayBuffer%': typeof SharedArrayBuffer === 'undefined' ? undefined : SharedArrayBuffer,
		'%String%': String,
		'%StringIteratorPrototype%': hasSymbols() && getProto ? getProto(''[Symbol.iterator]()) : undefined,
		'%Symbol%': hasSymbols() ? Symbol : undefined,
		'%SyntaxError%': SyntaxError,
		'%ThrowTypeError%': ThrowTypeError,
		'%TypedArray%': TypedArray,
		'%TypeError%': TypeError,
		'%Uint8Array%': typeof Uint8Array === 'undefined' ? undefined : Uint8Array,
		'%Uint8ClampedArray%': typeof Uint8ClampedArray === 'undefined' ? undefined : Uint8ClampedArray,
		'%Uint16Array%': typeof Uint16Array === 'undefined' ? undefined : Uint16Array,
		'%Uint32Array%': typeof Uint32Array === 'undefined' ? undefined : Uint32Array,
		'%URIError%': URIError,
		'%WeakMap%': typeof WeakMap === 'undefined' ? undefined : WeakMap,
		'%WeakRef%': typeof WeakRef === 'undefined' ? undefined : WeakRef,
		'%WeakSet%': typeof WeakSet === 'undefined' ? undefined : WeakSet,

		'%Function.prototype.call%': $call,
		'%Function.prototype.apply%': $apply,
		'%Object.defineProperty%': $defineProperty,
		'%Object.getPrototypeOf%': $ObjectGPO,
		'%Math.abs%': abs,
		'%Math.floor%': floor,
		'%Math.max%': max,
		'%Math.min%': min,
		'%Math.pow%': pow,
		'%Math.round%': round,
		'%Math.sign%': sign,
		'%Reflect.getPrototypeOf%': $ReflectGPO
	};

	if (getProto) {
		try {
			null.error; // eslint-disable-line no-unused-expressions
		} catch (e) {
			// https://github.com/tc39/proposal-shadowrealm/pull/384#issuecomment-1364264229
			var errorProto = getProto(getProto(e));
			INTRINSICS['%Error.prototype%'] = errorProto;
		}
	}

	var LEGACY_ALIASES = {
		__proto__: null,
		'%ArrayBufferPrototype%': ['ArrayBuffer', 'prototype'],
		'%ArrayPrototype%': ['Array', 'prototype'],
		'%ArrayProto_entries%': ['Array', 'prototype', 'entries'],
		'%ArrayProto_forEach%': ['Array', 'prototype', 'forEach'],
		'%ArrayProto_keys%': ['Array', 'prototype', 'keys'],
		'%ArrayProto_values%': ['Array', 'prototype', 'values'],
		'%AsyncFunctionPrototype%': ['AsyncFunction', 'prototype'],
		'%AsyncGenerator%': ['AsyncGeneratorFunction', 'prototype'],
		'%AsyncGeneratorPrototype%': ['AsyncGeneratorFunction', 'prototype', 'prototype'],
		'%BooleanPrototype%': ['Boolean', 'prototype'],
		'%DataViewPrototype%': ['DataView', 'prototype'],
		'%DatePrototype%': ['Date', 'prototype'],
		'%ErrorPrototype%': ['Error', 'prototype'],
		'%EvalErrorPrototype%': ['EvalError', 'prototype'],
		'%Float32ArrayPrototype%': ['Float32Array', 'prototype'],
		'%Float64ArrayPrototype%': ['Float64Array', 'prototype'],
		'%FunctionPrototype%': ['Function', 'prototype'],
		'%Generator%': ['GeneratorFunction', 'prototype'],
		'%GeneratorPrototype%': ['GeneratorFunction', 'prototype', 'prototype'],
		'%Int8ArrayPrototype%': ['Int8Array', 'prototype'],
		'%Int16ArrayPrototype%': ['Int16Array', 'prototype'],
		'%Int32ArrayPrototype%': ['Int32Array', 'prototype'],
		'%JSONParse%': ['JSON', 'parse'],
		'%JSONStringify%': ['JSON', 'stringify'],
		'%MapPrototype%': ['Map', 'prototype'],
		'%NumberPrototype%': ['Number', 'prototype'],
		'%ObjectPrototype%': ['Object', 'prototype'],
		'%ObjProto_toString%': ['Object', 'prototype', 'toString'],
		'%ObjProto_valueOf%': ['Object', 'prototype', 'valueOf'],
		'%PromisePrototype%': ['Promise', 'prototype'],
		'%PromiseProto_then%': ['Promise', 'prototype', 'then'],
		'%Promise_all%': ['Promise', 'all'],
		'%Promise_reject%': ['Promise', 'reject'],
		'%Promise_resolve%': ['Promise', 'resolve'],
		'%RangeErrorPrototype%': ['RangeError', 'prototype'],
		'%ReferenceErrorPrototype%': ['ReferenceError', 'prototype'],
		'%RegExpPrototype%': ['RegExp', 'prototype'],
		'%SetPrototype%': ['Set', 'prototype'],
		'%SharedArrayBufferPrototype%': ['SharedArrayBuffer', 'prototype'],
		'%StringPrototype%': ['String', 'prototype'],
		'%SymbolPrototype%': ['Symbol', 'prototype'],
		'%SyntaxErrorPrototype%': ['SyntaxError', 'prototype'],
		'%TypedArrayPrototype%': ['TypedArray', 'prototype'],
		'%TypeErrorPrototype%': ['TypeError', 'prototype'],
		'%Uint8ArrayPrototype%': ['Uint8Array', 'prototype'],
		'%Uint8ClampedArrayPrototype%': ['Uint8ClampedArray', 'prototype'],
		'%Uint16ArrayPrototype%': ['Uint16Array', 'prototype'],
		'%Uint32ArrayPrototype%': ['Uint32Array', 'prototype'],
		'%URIErrorPrototype%': ['URIError', 'prototype'],
		'%WeakMapPrototype%': ['WeakMap', 'prototype'],
		'%WeakSetPrototype%': ['WeakSet', 'prototype']
	};

	var $concat = bind.call($call, Array.prototype.concat);
	var $spliceApply = bind.call($apply, Array.prototype.splice);
	var $replace = bind.call($call, String.prototype.replace);
	var $strSlice = bind.call($call, String.prototype.slice);
	var $exec = bind.call($call, RegExp.prototype.exec);

	/* adapted from https://github.com/lodash/lodash/blob/4.17.15/dist/lodash.js#L6735-L6744 */
	var rePropName = /[^%.[\]]+|\[(?:(-?\d+(?:\.\d+)?)|(["'])((?:(?!\2)[^\\]|\\.)*?)\2)\]|(?=(?:\.|\[\])(?:\.|\[\]|%$))/g;
	var reEscapeChar = /\\(\\)?/g; /** Used to match backslashes in property paths. */
	var stringToPath = function stringToPath(string) {
		var first = $strSlice(string, 0, 1);
		var last = $strSlice(string, -1);
		if (first === '%' && last !== '%') {
			throw new SyntaxError('invalid intrinsic syntax, expected closing `%`');
		} else if (last === '%' && first !== '%') {
			throw new SyntaxError('invalid intrinsic syntax, expected opening `%`');
		}
		var result = [];
		$replace(string, rePropName, function (match, number, quote, subString) {
			result[result.length] = quote ? $replace(subString, reEscapeChar, '$1') : number || match;
		});
		return result;
	};
	/* end adaptation */

	var getBaseIntrinsic = function getBaseIntrinsic(name, allowMissing) {
		var intrinsicName = name;
		var alias;
		if (hasown(LEGACY_ALIASES, intrinsicName)) {
			alias = LEGACY_ALIASES[intrinsicName];
			intrinsicName = '%' + alias[0] + '%';
		}

		if (hasown(INTRINSICS, intrinsicName)) {
			var value = INTRINSICS[intrinsicName];
			if (typeof value === 'undefined' && !allowMissing) {
				throw new TypeError('intrinsic ' + name + ' exists, but is not available. Please file an issue!');
			}

			return {
				alias: alias,
				name: intrinsicName,
				value: value
			};
		}

		throw new SyntaxError('intrinsic ' + name + ' does not exist!');
	};

	var reIntrinsicName = /^%?[^%]*%?$/;

	return function GetIntrinsic(name, allowMissing) {
		if (typeof name !== 'string' || name.length === 0) {
			throw new TypeError('intrinsic name must be a non-empty string');
		}
		if (arguments.length > 1 && typeof allowMissing !== 'boolean') {
			throw new TypeError('"allowMissing" argument must be a boolean');
		}

		if ($exec(reIntrinsicName, name) === null) {
			throw new SyntaxError('`%` may not be present anywhere but at the beginning and end of the intrinsic name');
		}
		var parts = stringToPath(name);
		var intrinsicBaseName = parts.length > 0 ? parts[0] : '';

		var intrinsic = getBaseIntrinsic('%' + intrinsicBaseName + '%', allowMissing);
		var intrinsicRealName = intrinsic.name;
		var value = intrinsic.value;
		var skipFurtherCaching = false;

		var alias = intrinsic.alias;
		if (alias) {
			intrinsicBaseName = alias[0];
			$spliceApply(parts, $concat([0, 1], alias));
		}

		for (var i = 1, isOwn = true; i < parts.length; i += 1) {
			var part = parts[i];
			var first = $strSlice(part, 0, 1);
			var last = $strSlice(part, -1);
			if (
				(
					(first === '"' || first === "'" || first === '`')
					|| (last === '"' || last === "'" || last === '`')
				)
				&& first !== last
			) {
				throw new SyntaxError('property names with quotes must have matching quotes');
			}
			if (part === 'constructor' || !isOwn) {
				skipFurtherCaching = true;
			}

			intrinsicBaseName += '.' + part;
			intrinsicRealName = '%' + intrinsicBaseName + '%';

			if (hasown(INTRINSICS, intrinsicRealName)) {
				value = INTRINSICS[intrinsicRealName];
			} else if (value != null) {
				if (!(part in value)) {
					if (!allowMissing) {
						throw new TypeError('base intrinsic for ' + name + ' exists, but the property is not available.');
					}
					return void undefined;
				}
				if (gopd && (i + 1) >= parts.length) {
					var desc = gopd(value, part);
					isOwn = !!desc;

					// By convention, when a data property is converted to an accessor
					// property to emulate a data property that does not suffer from
					// the override mistake, that accessor's getter is marked with
					// an `originalValue` property. Here, when we detect this, we
					// uphold the illusion by pretending to see that original data
					// property, i.e., returning the value rather than the getter
					// itself.
					if (isOwn && 'get' in desc && !('originalValue' in desc.get)) {
						value = desc.get;
					} else {
						value = value[part];
					}
				} else {
					isOwn = hasown(value, part);
					value = value[part];
				}

				if (isOwn && !skipFurtherCaching) {
					INTRINSICS[intrinsicRealName] = value;
				}
			}
		}
		return value;
	};
})();

// === ES-Abstract Dependencies ===

// From es-object-atoms/isObject.js
var isObject = function isObject(x) {
	return !!x && (typeof x === 'function' || typeof x === 'object');
};

// From es-abstract/helpers/isPropertyKey.js
var isPropertyKey = function isPropertyKey(argument) {
	return typeof argument === 'string' || typeof argument === 'symbol';
};

// From es-abstract/helpers/isPrimitive.js
var isPrimitive = function isPrimitive(value) {
	return value === null || (typeof value !== 'function' && typeof value !== 'object');
};

// From es-abstract/2025/SameValue.js
var SameValue = function SameValue(x, y) {
	if (x === y) { // 0 === -0, but they are not identical.
		if (x === 0) { return 1 / x === 1 / y; }
		return true;
	}
	return $isNaN(x) && $isNaN(y);
};

// From es-abstract/2025/ToBoolean.js
var ToBoolean = function ToBoolean(value) { return !!value; };

// From is-callable/index.js
var isCallable = (function() {
	var fnToStr = Function.prototype.toString;
	var reflectApply = typeof Reflect === 'object' && Reflect !== null && Reflect.apply;
	var badArrayLike;
	var isCallableMarker;
	if (typeof reflectApply === 'function' && typeof Object.defineProperty === 'function') {
		try {
			badArrayLike = Object.defineProperty({}, 'length', {
				get: function () {
					throw isCallableMarker;
				}
			});
			isCallableMarker = {};
			// eslint-disable-next-line no-throw-literal
			reflectApply(function () { throw 42; }, null, badArrayLike);
		} catch (_) {
			if (_ !== isCallableMarker) {
				reflectApply = null;
			}
		}
	} else {
		reflectApply = null;
	}

	var constructorRegex = /^\s*class\b/;
	var isES6ClassFn = function isES6ClassFunction(value) {
		try {
			var fnStr = fnToStr.call(value);
			return constructorRegex.test(fnStr);
		} catch (e) {
			return false; // not a function
		}
	};

	var tryFunctionObject = function tryFunctionToStr(value) {
		try {
			if (isES6ClassFn(value)) { return false; }
			fnToStr.call(value);
			return true;
		} catch (e) {
			return false;
		}
	};
	var toStr = Object.prototype.toString;
	var objectClass = '[object Object]';
	var fnClass = '[object Function]';
	var genClass = '[object GeneratorFunction]';
	var ddaClass = '[object HTMLAllCollection]'; // IE 11
	var ddaClass2 = '[object HTML document.all class]';
	var ddaClass3 = '[object HTMLCollection]'; // IE 9-10
	var hasToStringTag = typeof Symbol === 'function' && !!Symbol.toStringTag; // better: use `has-tostringtag`

	var isIE68 = !(0 in [,]); // eslint-disable-line no-sparse-arrays, comma-spacing

	var isDDA = function isDocumentDotAll() { return false; };
	if (typeof document === 'object') {
		// Firefox 3 canonicalizes DDA to undefined when it's not accessed directly
		var all = document.all;
		if (toStr.call(all) === toStr.call(document.all)) {
			isDDA = function isDocumentDotAll(value) {
				/* globals document: false */
				// in IE 6-8, typeof document.all is "object" and it's truthy
				if ((isIE68 || !value) && (typeof value === 'undefined' || typeof value === 'object')) {
					try {
						var str = toStr.call(value);
						return (
							str === ddaClass
							|| str === ddaClass2
							|| str === ddaClass3 // opera 12.16
							|| str === objectClass // IE 6-8
						) && value('') == null; // eslint-disable-line eqeqeq
					} catch (e) { /**/ }
				}
				return false;
			};
		}
	}

	return reflectApply
		? function isCallable(value) {
			if (isDDA(value)) { return true; }
			if (!value) { return false; }
			if (typeof value !== 'function' && typeof value !== 'object') { return false; }
			try {
				reflectApply(value, null, badArrayLike);
			} catch (e) {
				if (e !== isCallableMarker) { return false; }
			}
			return !isES6ClassFn(value) && tryFunctionObject(value);
		}
		: function isCallable(value) {
			if (isDDA(value)) { return true; }
			if (!value) { return false; }
			if (typeof value !== 'function' && typeof value !== 'object') { return false; }
			if (hasToStringTag) { return tryFunctionObject(value); }
			if (isES6ClassFn(value)) { return false; }
			var strClass = toStr.call(value);
			if (strClass !== fnClass && strClass !== genClass && !(/^\[object HTML/).test(strClass)) { return false; }
			return tryFunctionObject(value);
		};
})();

// From es-abstract/helpers/records/property-descriptor.js
var isPropertyDescriptor = function isPropertyDescriptor(Desc) {
	if (!Desc || typeof Desc !== 'object') {
		return false;
	}

	var allowed = {
		__proto__: null,
		'[[Configurable]]': true,
		'[[Enumerable]]': true,
		'[[Get]]': true,
		'[[Set]]': true,
		'[[Value]]': true,
		'[[Writable]]': true
	};

	for (var key in Desc) { // eslint-disable-line
		if (hasown(Desc, key) && !allowed[key]) {
			return false;
		}
	}

	var isData = hasown(Desc, '[[Value]]') || hasown(Desc, '[[Writable]]');
	var IsAccessor = hasown(Desc, '[[Get]]') || hasown(Desc, '[[Set]]');
	if (isData && IsAccessor) {
		throw new TypeError('Property Descriptors may not be both accessor and data descriptors');
	}
	return true;
};

// From es-abstract/2025/IsAccessorDescriptor.js
var IsAccessorDescriptor = function IsAccessorDescriptor(Desc) {
	if (typeof Desc === 'undefined') {
		return false;
	}

	if (!hasown(Desc, '[[Get]]') && !hasown(Desc, '[[Set]]')) {
		return false;
	}

	return true;
};

// From es-abstract/2025/IsDataDescriptor.js
var IsDataDescriptor = function IsDataDescriptor(Desc) {
	if (typeof Desc === 'undefined') {
		return false;
	}

	if (!hasown(Desc, '[[Value]]') && !hasown(Desc, '[[Writable]]')) {
		return false;
	}

	return true;
};

// From es-abstract/2025/IsGenericDescriptor.js
var IsGenericDescriptor = function IsGenericDescriptor(Desc) {
	if (typeof Desc === 'undefined') {
		return false;
	}

	if (!IsAccessorDescriptor(Desc) && !IsDataDescriptor(Desc)) {
		return true;
	}

	return false;
};

// From es-abstract/2025/IsExtensible.js
var IsExtensible = (function() {
	var $preventExtensions = GetIntrinsic('%Object.preventExtensions%', true);
	var $isExtensible = GetIntrinsic('%Object.isExtensible%', true);

	return $preventExtensions
		? function IsExtensible(obj) {
			return !isPrimitive(obj) && $isExtensible(obj);
		}
		: function IsExtensible(obj) {
			return !isPrimitive(obj);
		};
})();

var IsArray = Array.isArray;

// From es-abstract/helpers/fromPropertyDescriptor.js
var fromPropertyDescriptor = function fromPropertyDescriptor(Desc) {
	if (typeof Desc === 'undefined') {
		return Desc;
	}
	var obj = {};
	if ('[[Value]]' in Desc) {
		obj.value = Desc['[[Value]]'];
	}
	if ('[[Writable]]' in Desc) {
		obj.writable = !!Desc['[[Writable]]'];
	}
	if ('[[Get]]' in Desc) {
		obj.get = Desc['[[Get]]'];
	}
	if ('[[Set]]' in Desc) {
		obj.set = Desc['[[Set]]'];
	}
	if ('[[Enumerable]]' in Desc) {
		obj.enumerable = !!Desc['[[Enumerable]]'];
	}
	if ('[[Configurable]]' in Desc) {
		obj.configurable = !!Desc['[[Configurable]]'];
	}
	return obj;
};

// From es-abstract/2025/FromPropertyDescriptor.js
var FromPropertyDescriptor = function FromPropertyDescriptor(Desc) {
	return fromPropertyDescriptor(Desc);
};

// From es-abstract/2025/ToPropertyDescriptor.js
var ToPropertyDescriptor = function ToPropertyDescriptor(Obj) {
	if (!isObject(Obj)) {
		throw new TypeError('ToPropertyDescriptor requires an object');
	}

	var desc = {};
	if (hasown(Obj, 'enumerable')) {
		desc['[[Enumerable]]'] = ToBoolean(Obj.enumerable);
	}
	if (hasown(Obj, 'configurable')) {
		desc['[[Configurable]]'] = ToBoolean(Obj.configurable);
	}
	if (hasown(Obj, 'value')) {
		desc['[[Value]]'] = Obj.value;
	}
	if (hasown(Obj, 'writable')) {
		desc['[[Writable]]'] = ToBoolean(Obj.writable);
	}
	if (hasown(Obj, 'get')) {
		var getter = Obj.get;
		if (typeof getter !== 'undefined' && !isCallable(getter)) {
			throw new TypeError('getter must be a function');
		}
		desc['[[Get]]'] = getter;
	}
	if (hasown(Obj, 'set')) {
		var setter = Obj.set;
		if (typeof setter !== 'undefined' && !isCallable(setter)) {
			throw new TypeError('setter must be a function');
		}
		desc['[[Set]]'] = setter;
	}

	if ((hasown(desc, '[[Get]]') || hasown(desc, '[[Set]]')) && (hasown(desc, '[[Value]]') || hasown(desc, '[[Writable]]'))) {
		throw new TypeError('Invalid property descriptor. Cannot both specify accessors and a value or writable attribute');
	}
	return desc;
};

// From es-abstract/helpers/isFullyPopulatedPropertyDescriptor.js
var isFullyPopulatedPropertyDescriptor = function isFullyPopulatedPropertyDescriptor(ES, Desc) {
	return isPropertyDescriptor(Desc)
		&& '[[Enumerable]]' in Desc
		&& '[[Configurable]]' in Desc
		&& (ES.IsAccessorDescriptor(Desc) || ES.IsDataDescriptor(Desc));
};

// From es-abstract/helpers/DefineOwnProperty.js
var DefineOwnProperty = function DefineOwnProperty(IsDataDescriptor, SameValue, FromPropertyDescriptor, O, P, desc) {
	var hasArrayLengthDefineBug = hasPropertyDescriptors.hasArrayLengthDefineBug();
	var $isEnumerable = callBound('Object.prototype.propertyIsEnumerable');

	if (!$defineProperty) {
		if (!IsDataDescriptor(desc)) {
			// ES3 does not support getters/setters
			return false;
		}
		if (!desc['[[Configurable]]'] || !desc['[[Writable]]']) {
			return false;
		}

		// fallback for ES3
		if (P in O && $isEnumerable(O, P) !== !!desc['[[Enumerable]]']) {
			// a non-enumerable existing property
			return false;
		}

		// property does not exist at all, or exists but is enumerable
		var V = desc['[[Value]]'];
		// eslint-disable-next-line no-param-reassign
		O[P] = V; // will use [[Define]]
		return SameValue(O[P], V);
	}
	if (
		hasArrayLengthDefineBug
		&& P === 'length'
		&& '[[Value]]' in desc
		&& IsArray(O)
		&& O.length !== desc['[[Value]]']
	) {
		// eslint-disable-next-line no-param-reassign
		O.length = desc['[[Value]]'];
		return O.length === desc['[[Value]]'];
	}

	$defineProperty(O, P, FromPropertyDescriptor(desc));
	return true;
};

// From es-abstract/2025/ValidateAndApplyPropertyDescriptor.js
var ValidateAndApplyPropertyDescriptor = function ValidateAndApplyPropertyDescriptor(O, P, extensible, Desc, current) {
	if (typeof current === 'undefined') { // step 2
		if (!extensible) {
			return false; // step 2.a
		}
		if (typeof O === 'undefined') {
			return true; // step 2.b
		}
		if (IsAccessorDescriptor(Desc)) { // step 2.c
			return DefineOwnProperty(
				IsDataDescriptor,
				SameValue,
				FromPropertyDescriptor,
				O,
				P,
				Desc
			);
		}
		// step 2.d
		return DefineOwnProperty(
			IsDataDescriptor,
			SameValue,
			FromPropertyDescriptor,
			O,
			P,
			{
				'[[Configurable]]': !!Desc['[[Configurable]]'],
				'[[Enumerable]]': !!Desc['[[Enumerable]]'],
				'[[Value]]': Desc['[[Value]]'],
				'[[Writable]]': !!Desc['[[Writable]]']
			}
		);
	}

	// 3. Assert: current is a fully populated Property Descriptor.
	if (
		!isFullyPopulatedPropertyDescriptor(
			{
				IsAccessorDescriptor: IsAccessorDescriptor,
				IsDataDescriptor: IsDataDescriptor
			},
			current
		)
	) {
		throw new TypeError('`current`, when present, must be a fully populated and valid Property Descriptor');
	}

	// 4. If every field in Desc is absent, return true.
	// this can't really match the assertion that it's a Property Descriptor in our JS implementation

	// 5. If current.[[Configurable]] is false, then
	if (!current['[[Configurable]]']) {
		if ('[[Configurable]]' in Desc && Desc['[[Configurable]]']) {
			// step 5.a
			return false;
		}
		if ('[[Enumerable]]' in Desc && !SameValue(Desc['[[Enumerable]]'], current['[[Enumerable]]'])) {
			// step 5.b
			return false;
		}
		if (!IsGenericDescriptor(Desc) && !SameValue(IsAccessorDescriptor(Desc), IsAccessorDescriptor(current))) {
			// step 5.c
			return false;
		}
		if (IsAccessorDescriptor(current)) { // step 5.d
			if ('[[Get]]' in Desc && !SameValue(Desc['[[Get]]'], current['[[Get]]'])) {
				return false;
			}
			if ('[[Set]]' in Desc && !SameValue(Desc['[[Set]]'], current['[[Set]]'])) {
				return false;
			}
		} else if (!current['[[Writable]]']) { // step 5.e
			if ('[[Writable]]' in Desc && Desc['[[Writable]]']) {
				return false;
			}
			if ('[[Value]]' in Desc && !SameValue(Desc['[[Value]]'], current['[[Value]]'])) {
				return false;
			}
		}
	}

	// 6. If O is not undefined, then
	if (typeof O !== 'undefined') {
		var configurable;
		var enumerable;
		if (IsDataDescriptor(current) && IsAccessorDescriptor(Desc)) { // step 6.a
			configurable = ('[[Configurable]]' in Desc ? Desc : current)['[[Configurable]]'];
			enumerable = ('[[Enumerable]]' in Desc ? Desc : current)['[[Enumerable]]'];
			// Replace the property named P of object O with an accessor property having [[Configurable]] and [[Enumerable]] attributes as described by current and each other attribute set to its default value.
			return DefineOwnProperty(
				IsDataDescriptor,
				SameValue,
				FromPropertyDescriptor,
				O,
				P,
				{
					'[[Configurable]]': !!configurable,
					'[[Enumerable]]': !!enumerable,
					'[[Get]]': ('[[Get]]' in Desc ? Desc : current)['[[Get]]'],
					'[[Set]]': ('[[Set]]' in Desc ? Desc : current)['[[Set]]']
				}
			);
		} else if (IsAccessorDescriptor(current) && IsDataDescriptor(Desc)) {
			configurable = ('[[Configurable]]' in Desc ? Desc : current)['[[Configurable]]'];
			enumerable = ('[[Enumerable]]' in Desc ? Desc : current)['[[Enumerable]]'];
			// i. Replace the property named P of object O with a data property having [[Configurable]] and [[Enumerable]] attributes as described by current and each other attribute set to its default value.
			return DefineOwnProperty(
				IsDataDescriptor,
				SameValue,
				FromPropertyDescriptor,
				O,
				P,
				{
					'[[Configurable]]': !!configurable,
					'[[Enumerable]]': !!enumerable,
					'[[Value]]': ('[[Value]]' in Desc ? Desc : current)['[[Value]]'],
					'[[Writable]]': !!('[[Writable]]' in Desc ? Desc : current)['[[Writable]]']
				}
			);
		}

		// For each field of Desc that is present, set the corresponding attribute of the property named P of object O to the value of the field.
		return DefineOwnProperty(
			IsDataDescriptor,
			SameValue,
			FromPropertyDescriptor,
			O,
			P,
			Desc
		);
	}

	return true; // step 7
};

// From es-abstract/2025/OrdinaryDefineOwnProperty.js
var OrdinaryDefineOwnProperty = function OrdinaryDefineOwnProperty(O, P, Desc) {
	if (!gopd) {
		// ES3/IE 8 fallback
		if (IsAccessorDescriptor(Desc)) {
			throw new SyntaxError('This environment does not support accessor property descriptors.');
		}
		var creatingNormalDataProperty = !(P in O)
			&& Desc['[[Writable]]']
			&& Desc['[[Enumerable]]']
			&& Desc['[[Configurable]]']
			&& '[[Value]]' in Desc;
		var settingExistingDataProperty = (P in O)
			&& (!('[[Configurable]]' in Desc) || Desc['[[Configurable]]'])
			&& (!('[[Enumerable]]' in Desc) || Desc['[[Enumerable]]'])
			&& (!('[[Writable]]' in Desc) || Desc['[[Writable]]'])
			&& '[[Value]]' in Desc;
		if (creatingNormalDataProperty || settingExistingDataProperty) {
			O[P] = Desc['[[Value]]']; // eslint-disable-line no-param-reassign
			return SameValue(O[P], Desc['[[Value]]']);
		}
		throw new SyntaxError('This environment does not support defining non-writable, non-enumerable, or non-configurable properties');
	}
	var desc = gopd(O, P);
	var current = desc && ToPropertyDescriptor(desc);
	var extensible = IsExtensible(O);
	return ValidateAndApplyPropertyDescriptor(O, P, extensible, Desc, current);
};

// From es-abstract/2025/CreateDataProperty.js
var CreateDataProperty = function CreateDataProperty(O, P, V) {
	var newDesc = {
		'[[Configurable]]': true,
		'[[Enumerable]]': true,
		'[[Value]]': V,
		'[[Writable]]': true
	};
	return OrdinaryDefineOwnProperty(O, P, newDesc);
};

// From es-abstract/2025/CreateDataPropertyOrThrow.js
var CreateDataPropertyOrThrow = function CreateDataPropertyOrThrow(O, P, V) {
	var success = CreateDataProperty(O, P, V);
	if (!success) {
		throw new TypeError('unable to create data property');
	}
};

// From es-abstract/2025/Set.js
var Set = function Set(O, P, V, Throw) {
	// IE 9 does not throw in strict mode when writability/configurability/extensibility is violated
	var noThrowOnStrictViolation = (function () {
		try {
			delete [].length;
			return true;
		} catch (e) {
			return false;
		}
	}());

	if (Throw) {
		O[P] = V; // eslint-disable-line no-param-reassign
		if (noThrowOnStrictViolation && !SameValue(O[P], V)) {
			throw new TypeError('Attempted to assign to readonly property.');
		}
		return true;
	}
	try {
		O[P] = V; // eslint-disable-line no-param-reassign
		return noThrowOnStrictViolation ? SameValue(O[P], V) : true;
	} catch (e) {
		return false;
	}
};

// From es-abstract/2025/SetterThatIgnoresPrototypeProperties.js
var SetterThatIgnoresPrototypeProperties = function SetterThatIgnoresPrototypeProperties(thisValue, home, p, v) {
	if (SameValue(thisValue, home)) { // step 2
		throw new TypeError('Throwing here emulates assignment to a non-writable data property on the `home` object in strict mode code'); // step 2.b
	}

	var desc = gopd(thisValue, p); // step 3

	if (typeof desc === 'undefined') { // step 4
		CreateDataPropertyOrThrow(thisValue, p, v); // step 4.a
	} else { // step 5
		Set(thisValue, p, v, true); // step 5.a
	}
};

// === Iterator Helper Prototype Dependencies ===

// Hermes supports proto, so set to true by default
var hasProto = function() {
	return true;
}

// From es-abstract/2025/CreateIterResultObject.js
var CreateIterResultObject = function CreateIterResultObject(value, done) {
	return {
		value: value,
		done: done
	};
};

// === Side Channel Implementation ===

// Simplified object-inspect for our needs
var objectInspect = function inspect(obj) {
	if (typeof obj === 'string') {
		return '"' + obj + '"';
	}
	if (typeof obj === 'number' || typeof obj === 'boolean') {
		return String(obj);
	}
	if (obj === null) {
		return 'null';
	}
	if (typeof obj === 'undefined') {
		return 'undefined';
	}
	return Object.prototype.toString.call(obj);
};

// From side-channel-list/index.js
var getSideChannelList = function getSideChannelList() {
	var listGetNode = function (list, key, isDelete) {
		var prev = list;
		var curr;
		for (; (curr = prev.next) != null; prev = curr) {
			if (curr.key === key) {
				prev.next = curr.next;
				if (!isDelete) {
					curr.next = list.next;
					list.next = curr;
				}
				return curr;
			}
		}
	};

	var listGet = function (objects, key) {
		if (!objects) {
			return void undefined;
		}
		var node = listGetNode(objects, key);
		return node && node.value;
	};

	var listSet = function (objects, key, value) {
		var node = listGetNode(objects, key);
		if (node) {
			node.value = value;
		} else {
			objects.next = {
				key: key,
				next: objects.next,
				value: value
			};
		}
	};

	var listHas = function (objects, key) {
		if (!objects) {
			return false;
		}
		return !!listGetNode(objects, key);
	};

	var listDelete = function (objects, key) {
		if (objects) {
			return listGetNode(objects, key, true);
		}
	};

	var $o;

	var channel = {
		assert: function (key) {
			if (!channel.has(key)) {
				throw new TypeError('Side channel does not contain ' + objectInspect(key));
			}
		},
		'delete': function (key) {
			var root = $o && $o.next;
			var deletedNode = listDelete($o, key);
			if (deletedNode && root && root === deletedNode) {
				$o = void undefined;
			}
			return !!deletedNode;
		},
		get: function (key) {
			return listGet($o, key);
		},
		has: function (key) {
			return listHas($o, key);
		},
		set: function (key, value) {
			if (!$o) {
				$o = {
					next: void undefined
				};
			}
			listSet($o, key, value);
		}
	};
	return channel;
};

// From side-channel-map/index.js
var getSideChannelMap = (function() {
	var $Map = GetIntrinsic('%Map%', true);

	if (!$Map) {
		return false;
	}

	var $mapGet = callBound('Map.prototype.get', true);
	var $mapSet = callBound('Map.prototype.set', true);
	var $mapHas = callBound('Map.prototype.has', true);
	var $mapDelete = callBound('Map.prototype.delete', true);
	var $mapSize = callBound('Map.prototype.size', true);

	return function getSideChannelMap() {
		var $m;

		var channel = {
			assert: function (key) {
				if (!channel.has(key)) {
					throw new TypeError('Side channel does not contain ' + objectInspect(key));
				}
			},
			'delete': function (key) {
				if ($m) {
					var result = $mapDelete($m, key);
					if ($mapSize($m) === 0) {
						$m = void undefined;
					}
					return result;
				}
				return false;
			},
			get: function (key) {
				if ($m) {
					return $mapGet($m, key);
				}
			},
			has: function (key) {
				if ($m) {
					return $mapHas($m, key);
				}
				return false;
			},
			set: function (key, value) {
				if (!$m) {
					$m = new $Map();
				}
				$mapSet($m, key, value);
			}
		};

		return channel;
	};
})();

// From side-channel-weakmap/index.js
var getSideChannelWeakMap = (function() {
	var $WeakMap = GetIntrinsic('%WeakMap%', true);

	if (!$WeakMap) {
		return false;
	}

	var $weakMapGet = callBound('WeakMap.prototype.get', true);
	var $weakMapSet = callBound('WeakMap.prototype.set', true);
	var $weakMapHas = callBound('WeakMap.prototype.has', true);
	var $weakMapDelete = callBound('WeakMap.prototype.delete', true);

	return function getSideChannelWeakMap() {
		var $wm;
		var $m;

		var channel = {
			assert: function (key) {
				if (!channel.has(key)) {
					throw new TypeError('Side channel does not contain ' + objectInspect(key));
				}
			},
			'delete': function (key) {
				if ($WeakMap && key && (typeof key === 'object' || typeof key === 'function')) {
					if ($wm) {
						return $weakMapDelete($wm, key);
					}
				} else if (getSideChannelMap) {
					if ($m) {
						return $m['delete'](key);
					}
				}
				return false;
			},
			get: function (key) {
				if ($WeakMap && key && (typeof key === 'object' || typeof key === 'function')) {
					if ($wm) {
						return $weakMapGet($wm, key);
					}
				}
				return $m && $m.get(key);
			},
			has: function (key) {
				if ($WeakMap && key && (typeof key === 'object' || typeof key === 'function')) {
					if ($wm) {
						return $weakMapHas($wm, key);
					}
				}
				return !!$m && $m.has(key);
			},
			set: function (key, value) {
				if ($WeakMap && key && (typeof key === 'object' || typeof key === 'function')) {
					if (!$wm) {
						$wm = new $WeakMap();
					}
					$weakMapSet($wm, key, value);
				} else if (getSideChannelMap) {
					if (!$m) {
						$m = getSideChannelMap();
					}
					$m.set(key, value);
				}
			}
		};

		return channel;
	};
})();

// From side-channel/index.js
var sideChannel = function getSideChannel() {
	var makeChannel = getSideChannelWeakMap || getSideChannelMap || getSideChannelList;
	var $channelData;

	var channel = {
		assert: function (key) {
			if (!channel.has(key)) {
				throw new TypeError('Side channel does not contain ' + objectInspect(key));
			}
		},
		'delete': function (key) {
			return !!$channelData && $channelData['delete'](key);
		},
		get: function (key) {
			return $channelData && $channelData.get(key);
		},
		has: function (key) {
			return !!$channelData && $channelData.has(key);
		},
		set: function (key, value) {
			if (!$channelData) {
				$channelData = makeChannel();
			}

			$channelData.set(key, value);
		}
	};
	return channel;
};

// From internal-slot/index.js
var SLOT = (function() {
	var channel = sideChannel();
	return {
		assert: function (O, slot) {
			if (!O || (typeof O !== 'object' && typeof O !== 'function')) {
				throw new TypeError('`O` is not an object');
			}
			if (typeof slot !== 'string') {
				throw new TypeError('`slot` must be a string');
			}
			if (!SLOT.has(O, slot)) {
				throw new TypeError('`' + slot + '` is not present on `O`');
			}
		},
		get: function (O, slot) {
			if (!O || (typeof O !== 'object' && typeof O !== 'function')) {
				throw new TypeError('`O` is not an object');
			}
			if (typeof slot !== 'string') {
				throw new TypeError('`slot` must be a string');
			}
			var slots = channel.get(O);
			return slots && slots['$' + slot];
		},
		has: function (O, slot) {
			if (!O || (typeof O !== 'object' && typeof O !== 'function')) {
				throw new TypeError('`O` is not an object');
			}
			if (typeof slot !== 'string') {
				throw new TypeError('`slot` must be a string');
			}
			var slots = channel.get(O);
			return !!slots && hasown(slots, '$' + slot);
		},
		set: function (O, slot, V) {
			if (!O || (typeof O !== 'object' && typeof O !== 'function')) {
				throw new TypeError('`O` is not an object');
			}
			if (typeof slot !== 'string') {
				throw new TypeError('`slot` must be a string');
			}
			var slots = channel.get(O);
			if (!slots) {
				slots = {};
				channel.set(O, slots);
			}
			slots['$' + slot] = V;
		}
	};
})();

// From es-abstract/2025/CompletionRecord.js
var CompletionRecord = function CompletionRecord(type, value) {
	if (!(this instanceof CompletionRecord)) {
		return new CompletionRecord(type, value);
	}
	SLOT.set(this, '[[Type]]', type);
	SLOT.set(this, '[[Value]]', value);
	// [[Target]] slot?
};

CompletionRecord.prototype.type = function Type() {
	return SLOT.get(this, '[[Type]]');
};

CompletionRecord.prototype.value = function Value() {
	return SLOT.get(this, '[[Value]]');
};

CompletionRecord.prototype['?'] = function ReturnIfAbrupt() {
	var type = SLOT.get(this, '[[Type]]');
	var value = SLOT.get(this, '[[Value]]');

	if (type === 'throw') {
		throw value;
	}
	return value;
};

CompletionRecord.prototype['!'] = function assert() {
	var type = SLOT.get(this, '[[Type]]');
	return SLOT.get(this, '[[Value]]');
};

// From es-abstract/2025/NormalCompletion.js
var NormalCompletion = function NormalCompletion(value) {
	return new CompletionRecord('normal', value);
};

// From es-abstract/2025/ThrowCompletion.js
var ThrowCompletion = function ThrowCompletion(argument) {
	return new CompletionRecord('throw', argument);
};

// From es-abstract/helpers/every.js
var every = function every(array, predicate) {
	for (var i = 0; i < array.length; i += 1) {
		if (!predicate(array[i], i, array)) {
			return false;
		}
	}
	return true;
};

// From es-abstract/helpers/records/iterator-record.js
var isIteratorRecord = function isIteratorRecord(value) {
	return !!value
		&& typeof value === 'object'
		&& hasown(value, '[[Iterator]]')
		&& hasown(value, '[[NextMethod]]')
		&& hasown(value, '[[Done]]')
		&& typeof value['[[Done]]'] === 'boolean';
};

// From es-abstract/2025/IteratorClose.js
var IteratorClose = function IteratorClose(iteratorRecord, completion) {
	var completionThunk = completion instanceof CompletionRecord ? function () { return completion['?'](); } : completion;

	var iterator = iteratorRecord['[[Iterator]]']; // step 3

	var iteratorReturn;
	try {
		iteratorReturn = GetMethod(iterator, 'return'); // step 4
	} catch (e) {
		completionThunk(); // throws if `completion` is a throw completion // step 6
		completionThunk = null; // ensure it's not called twice.
		throw e; // step 7
	}
	if (typeof iteratorReturn === 'undefined') {
		return completionThunk(); // step 5.a - 5.b
	}

	var innerResult;
	try {
		innerResult = Call(iteratorReturn, iterator, []);
	} catch (e) {
		// if we hit here, then "e" is the innerResult completion that needs re-throwing

		completionThunk(); // throws if `completion` is a throw completion // step 6
		completionThunk = null; // ensure it's not called twice.

		// if not, then return the innerResult completion
		throw e; // step 7
	}
	var completionRecord = completionThunk(); // if innerResult worked, then throw if the completion does
	completionThunk = null; // ensure it's not called twice.

	if (!isObject(innerResult)) {
		throw new TypeError('iterator .return must return an object');
	}

	return completionRecord;
};

// From es-iterator-helpers/aos/ReturnCompletion.js
var ReturnCompletion = function ReturnCompletion(value) {
	return new CompletionRecord('return', value);
};

// From es-iterator-helpers/aos/GeneratorValidate.js
var GeneratorValidate = function GeneratorValidate(generator, generatorBrand) {
	SLOT.assert(generator, '[[GeneratorState]]'); // step 1
	SLOT.assert(generator, '[[GeneratorBrand]]'); // step 2

	var state = SLOT.get(generator, '[[GeneratorState]]'); // step 5
	if (state === 'EXECUTING') {
		throw new TypeError('generator is executing');
	}

	return state; // step 7
};

// From es-iterator-helpers/aos/GeneratorResume.js
var GeneratorResume = function GeneratorResume(generator, value, generatorBrand) {
	var state = GeneratorValidate(generator, generatorBrand); // step 1
	if (state === 'COMPLETED') {
		return CreateIterResultObject(void undefined, true); // step 2
	}

	var genContext = SLOT.get(generator, '[[GeneratorContext]]');

	SLOT.set(generator, '[[GeneratorState]]', 'EXECUTING'); // step 7

	var result = genContext(value); // steps 5-6, 8-10

	return result;
};

// From es-iterator-helpers/aos/GeneratorResumeAbrupt.js
var GeneratorResumeAbrupt = function GeneratorResumeAbrupt(generator, abruptCompletion, generatorBrand) {
	var state = GeneratorValidate(generator, generatorBrand); // step 1

	if (state === 'SUSPENDED-START') { // step 2
		SLOT.set(generator, '[[GeneratorState]]', 'COMPLETED'); // step 2.a
		SLOT.set(generator, '[[GeneratorContext]]', null); // step 2.b
		state = 'COMPLETED'; // step 2.c
	}

	var value = abruptCompletion.value();

	if (state === 'COMPLETED') { // step 3
		if (abruptCompletion.type() === 'return') { // step 3.a
			return CreateIterResultObject(value, true); // step 3.a.i
		}
		return abruptCompletion['?'](); // step 3.b - throw
	}

	var genContext = SLOT.get(generator, '[[GeneratorContext]]'); // step 5

	SLOT.set(generator, '[[GeneratorState]]', 'EXECUTING'); // step 8

	if (abruptCompletion.type() === 'return') {
		// due to representing `GeneratorContext` as a function, we can't safely re-invoke it, so we can't support sending it a return completion
		var closeResult = SLOT.get(generator, '[[CloseIfAbrupt]]')(NormalCompletion(value));
		SLOT.set(generator, '[[GeneratorState]]', 'COMPLETED'); // step 10
		return CreateIterResultObject(closeResult, true);
	}

	var result = genContext(value); // steps 6-7, 9-11

	return result; // step 12
};

// From es-iterator-helpers/aos/IteratorCloseAll.js
var IteratorCloseAll = function IteratorCloseAll(iters, completion) {
	for (var i = iters.length - 1; i >= 0; i -= 1) { // step 1
		try {
			IteratorClose(iters[i], completion); // step 1.a
		} catch (e) {
			// eslint-disable-next-line no-param-reassign
			completion = ThrowCompletion(e); // step 1.a
		}
	}

	return completion['?'](); // step 2
};

// === Iterator.prototype.drop Dependencies ===

// From isarray/index.js
var isarray = Array.isArray || function (arr) {
	return {}.toString.call(arr) == '[object Array]';
};

// From es-abstract/2025/floor.js
var floor = function floor(x) {
	if (typeof x === 'bigint') {
		return x;
	}
	return Math.floor(x);
};

// From es-abstract/2025/truncate.js
var truncate = function truncate(x) {
	if (typeof x !== 'number' && typeof x !== 'bigint') {
		throw new TypeError('argument must be a Number or a BigInt');
	}
	var result = x < 0 ? -floor(-x) : floor(x);
	return result === 0 ? 0 : result; // filter out -0
};

// Simplified ToPrimitive for common cases
var ToPrimitive = function ToPrimitive(input) {
	if (typeof input === 'object' && input !== null) {
		if (typeof Symbol !== 'undefined' && Symbol.toPrimitive) {
			var exoticToPrim = input[Symbol.toPrimitive];
			if (typeof exoticToPrim === 'function') {
				var result = exoticToPrim.call(input, 'number');
				if (isPrimitive(result)) {
					return result;
				}
				throw new TypeError('Cannot convert object to primitive value');
			}
		}
		if (typeof input.valueOf === 'function') {
			var valueOf = input.valueOf();
			if (typeof valueOf !== 'object') return valueOf;
		}
		if (typeof input.toString === 'function') {
			var toString = input.toString();
			if (typeof toString !== 'object') return toString;
		}
		throw new TypeError('Cannot convert object to primitive value');
	}
	return input;
};

// Simplified StringToNumber for common cases
var StringToNumber = function StringToNumber(argument) {
	var trimmed = argument.replace(/^\s+|\s+$/g, ''); // basic trim
	if (trimmed === '') return 0;
	if (trimmed === 'Infinity') return Infinity;
	if (trimmed === '-Infinity') return -Infinity;
	var num = +trimmed;
	return num;
};

// From es-abstract/2025/ToNumber.js
var ToNumber = function ToNumber(argument) {
	var value = isPrimitive(argument) ? argument : ToPrimitive(argument);
	if (typeof value === 'symbol') {
		throw new TypeError('Cannot convert a Symbol value to a number');
	}
	if (typeof value === 'bigint') {
		throw new TypeError('Conversion from \'BigInt\' to \'number\' is not allowed.');
	}
	if (typeof value === 'string') {
		return StringToNumber(value);
	}
	return +value;
};

// From es-abstract/2025/ToIntegerOrInfinity.js
var ToIntegerOrInfinity = function ToIntegerOrInfinity(value) {
	var number = ToNumber(value);
	if ($isNaN(number) || number === 0) { return 0; }
	if (!$isFinite(number)) { return number; }
	return truncate(number);
};

// From es-abstract/helpers/isNaN.js
var isNaN = Number.isNaN || function isNaN(a) {
	return a !== a;
};

// From es-abstract/helpers/isFinite.js
var $isFinite = Number.isFinite || function isFinite(a) {
	return typeof a === 'number' && isFinite(a);
};

// From es-abstract/helpers/forEach.js
var forEach = function forEach(array, callback) {
	for (var i = 0; i < array.length; i += 1) {
		callback(array[i], i, array);
	}
};

// From es-abstract/2025/IteratorComplete.js
var IteratorComplete = function IteratorComplete(iterResult) {
	return ToBoolean(Get(iterResult, 'done'));
};

// From es-abstract/2025/IteratorNext.js
var IteratorNext = function IteratorNext(iteratorRecord) {
	var result;
	try {
		if (arguments.length < 2) { // step 1
			result = Call(iteratorRecord['[[NextMethod]]'], iteratorRecord['[[Iterator]]']); // step 1.a
		} else { // step 2
			result = Call(iteratorRecord['[[NextMethod]]'], iteratorRecord['[[Iterator]]'], [arguments[1]]); // step 2.a
		}
	} catch (e) { // step 3
		iteratorRecord['[[Done]]'] = true; // step 3.a
		throw e; // step 3.b
	}

	if (!isObject(result)) { // step 5
		iteratorRecord['[[Done]]'] = true; // step 5.a
		throw new TypeError('iterator next must return an object'); // step 5.b
	}
	return result; // step 6
};

// From es-abstract/2025/IteratorStep.js
var IteratorStep = function IteratorStep(iteratorRecord) {
	var result = IteratorNext(iteratorRecord); // step 1
	var done;
	try {
		done = IteratorComplete(result); // step 2
	} catch (e) { // step 3
		iteratorRecord['[[Done]]'] = true; // step 3.a
		throw e; // step 3.b
	}
	if (done) { // step 5
		iteratorRecord['[[Done]]'] = true; // step 5.a
		return false; // step 5.b
	}
	return result; // step 6
};

// From es-abstract/2025/IteratorStepValue.js
var IteratorStepValue = function IteratorStepValue(iteratorRecord) {
	/* eslint no-param-reassign: 0 */

	var result = IteratorStep(iteratorRecord); // step 1
	if (!result || result === 'DONE') { // step 2
		return result; // step 2.a
	}

	var value;
	try {
		value = Get(result, 'value'); // step 3
	} catch (e) { // step 4
		iteratorRecord['[[Done]]'] = true; // step 4.a
		throw e; // step 5
	}

	return value; // step 5
};

// From es-iterator-helpers/aos/GeneratorStart.js
var GeneratorStart = function GeneratorStart(generator, closure) {
	var sentinel = SLOT.get(closure, '[[Sentinel]]');
	SLOT.set(generator, '[[GeneratorContext]]', function () { // steps 2-5
		try {
			var result = closure();
			if (result === sentinel) {
				SLOT.set(generator, '[[GeneratorState]]', 'COMPLETED');
				SLOT.set(generator, '[[GeneratorContext]]', null);
				return CreateIterResultObject(void undefined, true);
			}
			SLOT.set(generator, '[[GeneratorState]]', 'SUSPENDED-YIELD');
			return CreateIterResultObject(result, false);
		} catch (e) {
			SLOT.set(generator, '[[GeneratorState]]', 'COMPLETED');
			SLOT.set(generator, '[[GeneratorContext]]', null);
			throw e;
		}
	});

	SLOT.set(generator, '[[GeneratorState]]', 'SUSPENDED-START'); // step 6
};

// From es-iterator-helpers/aos/CreateIteratorFromClosure.js
var CreateIteratorFromClosure = function CreateIteratorFromClosure(closure, generatorBrand, proto) {
	var extraSlots = arguments.length > 3 ? arguments[3] : [];
	var internalSlotsList = extraSlots.concat(['[[GeneratorContext]]', '[[GeneratorBrand]]', '[[GeneratorState]]']); // step 3
	var generator = OrdinaryObjectCreate(proto, internalSlotsList); // steps 4, 6
	SLOT.set(generator, '[[GeneratorBrand]]', generatorBrand); // step 5

	SLOT.set(generator, '[[Sentinel]]', SLOT.get(closure, '[[Sentinel]]')); // our userland slot
	SLOT.set(generator, '[[CloseIfAbrupt]]', SLOT.get(closure, '[[CloseIfAbrupt]]')); // our second userland slot

	GeneratorStart(generator, closure); // step 13

	return generator; // step 15
};

// === Type System ===

// From es-abstract/5/Type.js
var ES5Type = function Type(x) {
	if (x === null) {
		return 'Null';
	}
	if (typeof x === 'undefined') {
		return 'Undefined';
	}
	if (isObject(x)) {
		return 'Object';
	}
	if (typeof x === 'number') {
		return 'Number';
	}
	if (typeof x === 'boolean') {
		return 'Boolean';
	}
	if (typeof x === 'string') {
		return 'String';
	}
};

// From es-abstract/2025/Type.js
var Type = function Type(x) {
	if (typeof x === 'symbol') {
		return 'Symbol';
	}
	if (typeof x === 'bigint') {
		return 'BigInt';
	}
	return ES5Type(x);
};

// === Iterator Helper Operations ===

// From es-abstract/2025/Get.js
var Get = function Get(O, P) {
	return O[P];
};

// From es-abstract/2025/GetV.js
var GetV = function GetV(V, P) {
	return V[P];
};

// From es-abstract/2025/GetMethod.js
var GetMethod = function GetMethod(O, P) {
	var func = GetV(O, P);

	if (func == null) {
		return void 0;
	}

	if (!isCallable(func)) {
		throw new TypeError(P + ' is not a function: ' + func);
	}

	return func;
};

// From es-abstract/2025/Call.js
var Call = function Call(F, V) {
	var argumentsList = arguments.length > 2 ? arguments[2] : [];
	return $actualApply(F, V, argumentsList);
};

// From es-abstract/2025/OrdinaryObjectCreate.js
var $ObjectCreate = $Object.create;
var OrdinaryObjectCreate = function OrdinaryObjectCreate(proto) {
	var additionalInternalSlotsList = arguments.length < 2 ? [] : arguments[1];
	var O;
	if (hasProto()) {
		O = { __proto__: proto };
	} else if ($ObjectCreate) {
		O = $ObjectCreate(proto);
	} else {
		if (proto === null) {
			throw new SyntaxError('native Object.create support is required to create null objects');
		}
		var T = function T() {};
		T.prototype = proto;
		O = new T();
	}

	if (additionalInternalSlotsList.length > 0) {
		forEach(additionalInternalSlotsList, function (slot) {
			SLOT.set(O, slot, void undefined);
		});
	}

	return O;
};

// From es-iterator-helpers/aos/GetIteratorDirect.js
var GetIteratorDirect = function GetIteratorDirect(obj) {
	var nextMethod = Get(obj, 'next'); // step 2

	var iteratorRecord = { '[[Iterator]]': obj, '[[NextMethod]]': nextMethod, '[[Done]]': false }; // step 3

	return iteratorRecord; // step 4
};

// === Additional Dependencies for FlatMap ===

// From math-intrinsics/isInteger.js (simplified)
var isInteger = Number.isInteger || function isInteger(value) {
	return typeof value === 'number' && isFinite(value) && Math.floor(value) === value;
};

// From math-intrinsics/constants/maxSafeInteger.js
var MAX_SAFE_INTEGER = Number.MAX_SAFE_INTEGER || 9007199254740991;

// From es-abstract/helpers/isLeadingSurrogate.js
var isLeadingSurrogate = function isLeadingSurrogate(charCode) {
	return charCode >= 0xD800 && charCode <= 0xDBFF;
};

// From es-abstract/helpers/isTrailingSurrogate.js
var isTrailingSurrogate = function isTrailingSurrogate(charCode) {
	return charCode >= 0xDC00 && charCode <= 0xDFFF;
};

// From es-abstract/2025/UTF16SurrogatePairToCodePoint.js
var UTF16SurrogatePairToCodePoint = function UTF16SurrogatePairToCodePoint(lead, trail) {
	if (!isLeadingSurrogate(lead) || !isTrailingSurrogate(trail)) {
		throw new TypeError('Assertion failed: `lead` must be a leading surrogate, and `trail` must be a trailing surrogate');
	}
	// https://tc39.es/ecma262/#sec-utf16decodesurrogatepair
	return (lead - 0xD800) * 0x400 + (trail - 0xDC00) + 0x10000;
};

// From es-abstract/2025/CodePointAt.js
var CodePointAt = function CodePointAt(string, position) {
	if (typeof string !== 'string') {
		throw new TypeError('Assertion failed: `string` must be a String');
	}
	var size = string.length;
	if (position < 0 || position >= size) {
		throw new TypeError('Assertion failed: `position` must be >= 0, and < the length of `string`');
	}
	var $charAt = callBound('String.prototype.charAt');
	var $charCodeAt = callBound('String.prototype.charCodeAt');

	var first = $charCodeAt(string, position);
	var cp = $charAt(string, position);
	var firstIsLeading = isLeadingSurrogate(first);
	var firstIsTrailing = isTrailingSurrogate(first);
	if (!firstIsLeading && !firstIsTrailing) {
		return {
			'[[CodePoint]]': cp,
			'[[CodeUnitCount]]': 1,
			'[[IsUnpairedSurrogate]]': false
		};
	}
	if (firstIsTrailing || (position + 1 === size)) {
		return {
			'[[CodePoint]]': cp,
			'[[CodeUnitCount]]': 1,
			'[[IsUnpairedSurrogate]]': true
		};
	}
	var second = $charCodeAt(string, position + 1);
	if (!isTrailingSurrogate(second)) {
		return {
			'[[CodePoint]]': cp,
			'[[CodeUnitCount]]': 1,
			'[[IsUnpairedSurrogate]]': true
		};
	}

	return {
		'[[CodePoint]]': UTF16SurrogatePairToCodePoint(first, second),
		'[[CodeUnitCount]]': 2,
		'[[IsUnpairedSurrogate]]': false
	};
};

// From es-abstract/2025/AdvanceStringIndex.js
var AdvanceStringIndex = function AdvanceStringIndex(S, index, unicode) {
	if (typeof S !== 'string') {
		throw new TypeError('Assertion failed: `S` must be a String');
	}
	if (!isInteger(index) || index < 0 || index > MAX_SAFE_INTEGER) {
		throw new TypeError('Assertion failed: `length` must be an integer >= 0 and <= 2**53');
	}
	if (typeof unicode !== 'boolean') {
		throw new TypeError('Assertion failed: `unicode` must be a Boolean');
	}
	if (!unicode) {
		return index + 1;
	}
	var length = S.length;
	if ((index + 1) >= length) {
		return index + 1;
	}
	var cp = CodePointAt(S, index);
	return index + cp['[[CodeUnitCount]]'];
};

// From is-string/index.js (simplified)
var isString = function isString(value) {
	return typeof value === 'string' || (typeof value === 'object' && value !== null && Object.prototype.toString.call(value) === '[object String]');
};

// From es-abstract/helpers/getIteratorMethod.js
var getIteratorMethod = function getIteratorMethod(ES, iterable) {
	var usingIterator;
	if (hasSymbols() && typeof Symbol !== 'undefined' && Symbol.iterator) {
		usingIterator = ES.GetMethod(iterable, Symbol.iterator);
	} else if (IsArray(iterable)) {
		usingIterator = function () {
			var i = -1;
			var arr = this; // eslint-disable-line no-invalid-this
			return {
				next: function () {
					i += 1;
					return {
						done: i >= arr.length,
						value: arr[i]
					};
				}
			};
		};
	} else if (isString(iterable)) {
		var $stringSlice = callBound('String.prototype.slice');
		var $String = GetIntrinsic('%String%');
		usingIterator = function () {
			var i = 0;
			return {
				next: function () {
					var nextIndex = ES.AdvanceStringIndex($String(iterable), i, true);
					var value = $stringSlice(iterable, i, nextIndex);
					i = nextIndex;
					var done = nextIndex > iterable.length;
					return {
						done: done,
						value: done ? void undefined : value
					};
				}
			};
		};
	}
	return usingIterator;
};

// From es-iterator-helpers/aos/GetIteratorFlattenable.js
var GetIteratorFlattenable = function GetIteratorFlattenable(obj, stringHandling) {
	if (stringHandling !== 'REJECT-PRIMITIVES' && stringHandling !== 'ITERATE-STRING-PRIMITIVES') {
		throw new TypeError('Assertion failed: `stringHandling` must be "REJECT-PRIMITIVES" or "ITERATE-STRING-PRIMITIVES"');
	}

	if (!isObject(obj)) {
		if (stringHandling === 'REJECT-PRIMITIVES' || typeof obj !== 'string') {
			throw new TypeError('obj must be an Object'); // step 1.a
		}
	}

	var method = void undefined; // step 2

	// method = GetMethod(obj, Symbol.iterator); // step 5.a
	method = getIteratorMethod(
		{
			AdvanceStringIndex: AdvanceStringIndex,
			GetMethod: GetMethod,
			IsArray: IsArray
		},
		obj
	);

	var iterator;
	if (typeof method === 'undefined') { // step 3
		iterator = obj; // step 3.a
	} else { // step 4
		iterator = Call(method, obj); // step 4.a
	}

	if (!isObject(iterator)) {
		throw new TypeError('iterator must be an Object'); // step 5
	}
	return GetIteratorDirect(iterator); // step 6
};

// === Iterator.from Dependencies ===

// From es-abstract/2025/OrdinaryHasInstance.js
var OrdinaryHasInstance = function OrdinaryHasInstance(C, O) {
	if (!isCallable(C)) {
		return false;
	}
	if (!isObject(O)) {
		return false;
	}
	var P = Get(C, 'prototype');
	if (!isObject(P)) {
		throw new TypeError('OrdinaryHasInstance called on an object with an invalid prototype property.');
	}
	return O instanceof C;
};
