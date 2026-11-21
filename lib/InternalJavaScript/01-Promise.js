/* @nolint */var initPromise = (function () {
  'use strict';
  var useEngineQueue = HermesInternal.useEngineQueue();
  // Captured at startup so per-element checks in Promise.all stay
  // cheap. Spec compliance (§27.2.4.1.2 step 8.e: always Invoke
  // `.then`) is gated on `--test262`; otherwise we keep the
  // fast path that synchronously invokes the resolve element when
  // the input is an already-fulfilled core Promise.
  var test262Enabled = HermesInternal.test262Enabled();
  function noop() {}

  // States:
  //
  // 0 - pending
  // 1 - fulfilled with _value
  // 2 - rejected with _value
  // 3 - adopted the state of another promise, _value
  //
  // once the state is no longer pending (0) it is immutable

  // All `_` prefixed properties will be reduced to `_{random number}`
  // at build time to obfuscate them and discourage their use.
  // We don't use symbols or Object.defineProperty to fully hide them
  // because the performance isn't good enough.


  // to avoid using try/catch inside critical functions, we
  // extract them to here.
  var LAST_ERROR = null;
  var IS_ERROR = {};
  function getThen(obj) {
    try {
      return obj.then;
    } catch (ex) {
      LAST_ERROR = ex;
      return IS_ERROR;
    }
  }

  function tryCallOne(fn, a) {
    try {
      return fn(a);
    } catch (ex) {
      LAST_ERROR = ex;
      return IS_ERROR;
    }
  }
  function tryCallTwo(fn, a, b) {
    try {
      fn(a, b);
    } catch (ex) {
      LAST_ERROR = ex;
      return IS_ERROR;
    }
  }

  function isObject(x) {
    return !!x && (typeof x === 'function' || typeof x === 'object');
  }

  function isEnumerable(O, P) {
    return Object.prototype.propertyIsEnumerable.call(O, P);
  }

  function entriesToValues(entries) {
    var values = Object.create(null);
    for (var i = 0; i < entries.length; i += 1) {
      Object.defineProperty(values, entries[i].key, {
        value: entries[i].value, writable: true,
        enumerable: true, configurable: true,
      });
    }
    return values;
  }

  // Per spec §27.2 / §15.7: declaring Promise as a class gives all the
  // built-in spec attributes for free:
  // - calling without `new` throws TypeError (per [[Call]] of class
  //   constructors).
  // - Promise.prototype is { writable: false, enumerable: false,
  //   configurable: false } per §15.7.10 step 26.
  // - every prototype and static method is non-enumerable, configurable,
  //   writable, with NamedEvaluation-derived `.name`, and has no
  //   [[Construct]] (so `new Promise.prototype.then()` throws).
  class Promise {
    static _B = null;
    static _C = null;
    static _D = noop;

    constructor(fn) {
      if (typeof fn !== 'function') {
        throw new TypeError('Promise constructor\'s argument is not a function');
      }
      this._x = 0;
      this._y = 0;
      this._z = null;
      this._A = null;
      if (fn === noop) return;
      doResolve(fn, this);
    }

    /* Static Methods */

    static resolve(value) {
      // Per spec §27.2.4.5 step 2: If Type(this) is not Object, throw
      // TypeError. Includes null (typeof null === 'object', but null
      // is the Null type, not Object).
      var C = this;
      if (C === null || (typeof C !== 'object' && typeof C !== 'function')) {
        throw new TypeError('Promise.resolve called on non-object');
      }
      // Per spec §27.2.4.5 step 3: if IsPromise(value) and value's
      // constructor is C, return value. Only read .constructor on
      // actual promises — reading it on arbitrary thenables/objects
      // would be user-observable and break the thenable resolution
      // path.
      if (value instanceof core && value.constructor === C) {
        return value;
      }

      if (C === core) {
        // Fast path for core Promise.
        if (value === null) return NULL;
        if (value === undefined) return UNDEFINED;
        if (value === true) return TRUE;
        if (value === false) return FALSE;
        if (value === 0) return ZERO;
        if (value === '') return EMPTYSTRING;

        if (typeof value === 'object' || typeof value === 'function') {
          try {
            var then = value.then;
            if (typeof then === 'function') {
              return new core(then.bind(value));
            }
          } catch (ex) {
            return new core(function (resolve, reject) {
              reject(ex);
            });
          }
        }
        return valuePromise(value);
      }

      // Subclass path: NewPromiseCapability(C) + capability.[[Resolve]](value)
      // per spec §27.2.4.5 steps 4-6. Extract resolve as a local so
      // the call form passes `this = undefined` (matches spec's
      // Call(F, undefined, args)).
      var capability = newPromiseCapability(C);
      var capResolve = capability.resolve;
      capResolve(value);
      return capability.promise;
    }

    static allKeyed(promisesObject) {
      var C = this;
      var capability = newPromiseCapability(C);
      var resultPromise = capability.promise;
      var capResolve = capability.resolve;
      var capReject = capability.reject;
      var promiseResolve;
      try {
        promiseResolve = C.resolve;
      } catch (e) {
        capReject(e);
        return resultPromise;
      }
      if (typeof promiseResolve !== 'function') {
        capReject(new TypeError('Promise resolve is not a function'));
        return resultPromise;
      }
      if (!isObject(promisesObject)) {
        capReject(new TypeError('Argument of Promise.allKeyed is not a non-null object'));
        return resultPromise;
      }
      var remainingElementsCount = 1;
      var entries = [];
      var index = 0;
      function decrementAndResolve(){
        if (--remainingElementsCount === 0) {
          return capResolve(entriesToValues(entries));
        }
        return undefined;
      }
      try {
        var keys = Reflect.ownKeys(promisesObject);
        for (var i = 0; i < keys.length; i += 1) {
          var key = keys[i];
          if(!isEnumerable(promisesObject, key)) {
            continue;
          }
          remainingElementsCount++;
          // Self calling function to scope the alreadyCalled, key variables for each iteration of the loop
          ((alreadyCalled, itemKey, itemIndex) => {
            promiseResolve.call(C, promisesObject[itemKey]).then(
              value => {
                if (alreadyCalled.value) return;
                alreadyCalled.value = true;
                Object.defineProperty(entries, itemIndex, {
                  value: { key: itemKey, value: value }, writable: true,
                  enumerable: true, configurable: true,
                });
                return decrementAndResolve();
              },
              capReject
            );
          })({ value: false }, key, index);

          index++;
        }
        decrementAndResolve();
      } catch (e) {
        capReject(e);
      }
      return resultPromise;
    }

    static all(arr) {
      var C = this;
      // Step 2: NewPromiseCapability(C). Throws synchronously if C is
      // not a valid constructor or doesn't capture resolve/reject.
      var capability = newPromiseCapability(C);
      var resultPromise = capability.promise;
      var capResolve = capability.resolve;
      var capReject = capability.reject;
      // Step 3-4: GetPromiseResolve(C), IfAbruptRejectPromise.
      var promiseResolve;
      try {
        promiseResolve = C.resolve;
      } catch (e) {
        capReject(e);
        return resultPromise;
      }
      if (typeof promiseResolve !== 'function') {
        capReject(new TypeError('Promise resolve is not a function'));
        return resultPromise;
      }
      var remainingElementsCount = 1;
      var index = 0;
      var values = [];
      // Per spec §27.2.4.1.3: each Promise.all Resolve Element Function
      // is anonymous (name === ""), captures its own [[AlreadyCalled]],
      // and shares the values array, remainingElementsCount, and
      // capability with its siblings. Returning the closure (rather
      // than naming it via assignment) avoids triggering
      // NamedEvaluation per §8.5.5. Use an arrow function so the
      // returned closure is non-constructable per spec.
      function makeResolveElement(idx) {
        var alreadyCalled = false;
        return (value) => {
          if (alreadyCalled) return;
          alreadyCalled = true;
          Object.defineProperty(values, idx, {
            value: value, writable: true,
            enumerable: true, configurable: true,
          });
          if (--remainingElementsCount === 0) {
            capResolve(values);
          }
        };
      }
      // Step 7-8: PerformPromiseAll, IfAbruptRejectPromise. Catches
      // abrupt completions from the iterator protocol, promiseResolve,
      // nextPromise.then, the fast-path resolveElement call, and the
      // final capResolve (subclass capabilities can throw).
      try {
        for (var item of arr) {
          var nextPromise = promiseResolve.call(C, item);
          var resolveElement = makeResolveElement(index);
          remainingElementsCount++;
          // Per spec §27.2.4.1.2 step 8.e, .then must be invoked on
          // nextPromise so the reaction is enqueued as a
          // PromiseReactionJob. The fast path below synchronously calls
          // resolveElement when nextPromise is an already-fulfilled core
          // Promise, collapsing the spec's 1-microtask hop. We retain it
          // by default for performance and only take the spec path under
          // --test262.
          if (test262Enabled) {
            nextPromise.then(resolveElement, capReject);
          } else {
            nextPromise._y === 1
              ? resolveElement(nextPromise._z)
              : nextPromise.then(resolveElement, capReject);
          }
          index++;
        }
        if (--remainingElementsCount === 0) {
          capResolve(values);
        }
      } catch (e) {
        capReject(e);
      }
      return resultPromise;
    }

    static allSettledKeyed(promisesObject) {
      var C = this;
      var capability = newPromiseCapability(C);
      var resultPromise = capability.promise;
      var capResolve = capability.resolve;
      var capReject = capability.reject;
      var promiseResolve;
      try {
        promiseResolve = C.resolve;
      } catch (e) {
        capReject(e);
        return resultPromise;
      }
      if (typeof promiseResolve !== 'function') {
        capReject(new TypeError('Promise resolve is not a function'));
        return resultPromise;
      }
      if (!isObject(promisesObject)) {
        capReject(new TypeError('Argument of Promise.allSettledKeyed is not a non-null object'));
        return resultPromise;
      }
      var remainingElementsCount = 1;
      var entries = [];
      var index = 0;
      function decrementAndResolve(){
        if (--remainingElementsCount === 0) {
          return capResolve(entriesToValues(entries));
        }
        return undefined;
      }
      try {
        var keys = Reflect.ownKeys(promisesObject);
        for (var i = 0; i < keys.length; i += 1) {
          var key = keys[i];
          if(!isEnumerable(promisesObject, key)) {
            continue;
          }
          remainingElementsCount++;
          // Self calling function to scope the alreadyCalled, key variables for each iteration of the loop
          ((alreadyCalled, itemKey, itemIndex) => {
            promiseResolve.call(C, promisesObject[itemKey]).then(
              (value) => {
                if (alreadyCalled.value) return;
                alreadyCalled.value = true;
                Object.defineProperty(entries, itemIndex, {
                  value: { key: itemKey, value: { status: 'fulfilled', value: value } },
                  writable: true, enumerable: true, configurable: true,
                });
                return decrementAndResolve();
              },
              (error) => {
                if (alreadyCalled.value) return;
                alreadyCalled.value = true;
                Object.defineProperty(entries, itemIndex, {
                  value: { key: itemKey, value: { status: 'rejected', reason: error } },
                  writable: true, enumerable: true, configurable: true,
                });
                return decrementAndResolve();
              }
            );
          })({value: false}, key, index);
          index++;
        }
        decrementAndResolve();
      } catch (e) {
        capReject(e);
      }
      return resultPromise;
    }

    static allSettled(iterable) {
      var C = this;
      // Step 2: NewPromiseCapability(C).
      var capability = newPromiseCapability(C);
      var resultPromise = capability.promise;
      var capResolve = capability.resolve;
      var capReject = capability.reject;
      // Step 3-4: GetPromiseResolve(C), IfAbruptRejectPromise.
      var promiseResolve;
      try {
        promiseResolve = C.resolve;
      } catch (e) {
        capReject(e);
        return resultPromise;
      }
      if (typeof promiseResolve !== 'function') {
        capReject(new TypeError('Promise resolve is not a function'));
        return resultPromise;
      }
      var values = [];
      var remainingElementsCount = 1;
      var index = 0;
      // Per spec §27.2.4.2.1 step q: onRejected and onFulfilled share
      // the same [[AlreadyCalled]] record, so once either fires the
      // other is blocked for this index. Use an array literal (not an
      // object literal) so the inner functions are anonymous (name "")
      // per spec — object property assignment would trigger
      // NamedEvaluation and infer the names from the keys. Per spec
      // §27.2.4.2.2 / §27.2.4.2.3, the per-element resolve/reject
      // functions are built-ins without [[Construct]] — use arrow
      // functions.
      function makeSettledPair(idx) {
        var alreadyCalled = false;
        return [
          (value) => {
            if (alreadyCalled) return;
            alreadyCalled = true;
            Object.defineProperty(values, idx, {
              value: { status: 'fulfilled', value: value },
              writable: true, enumerable: true, configurable: true,
            });
            if (--remainingElementsCount === 0) {
              capResolve(values);
            }
          },
          (reason) => {
            if (alreadyCalled) return;
            alreadyCalled = true;
            Object.defineProperty(values, idx, {
              value: { status: 'rejected', reason: reason },
              writable: true, enumerable: true, configurable: true,
            });
            if (--remainingElementsCount === 0) {
              capResolve(values);
            }
          },
        ];
      }
      // Step 7-8: PerformPromiseAllSettled, IfAbruptRejectPromise.
      try {
        for (var item of iterable) {
          var nextPromise = promiseResolve.call(C, item);
          var pair = makeSettledPair(index);
          remainingElementsCount++;
          nextPromise.then(pair[0], pair[1]);
          index++;
        }
        if (--remainingElementsCount === 0) {
          capResolve(values);
        }
      } catch (e) {
        capReject(e);
      }
      return resultPromise;
    }

    static reject(value) {
      // Per spec §27.2.4.4 steps 1-5: NewPromiseCapability(C) +
      // capability.[[Reject]](value).
      var C = this;
      if (C === null || (typeof C !== 'object' && typeof C !== 'function')) {
        throw new TypeError('Promise.reject called on non-object');
      }
      var capability = newPromiseCapability(C);
      var capReject = capability.reject;
      capReject(value);
      return capability.promise;
    }

    static race(values) {
      var C = this;
      // Step 2: NewPromiseCapability(C).
      var capability = newPromiseCapability(C);
      var resultPromise = capability.promise;
      var capResolve = capability.resolve;
      var capReject = capability.reject;
      // Step 3-4: GetPromiseResolve(C), IfAbruptRejectPromise.
      var promiseResolve;
      try {
        promiseResolve = C.resolve;
      } catch (e) {
        capReject(e);
        return resultPromise;
      }
      if (typeof promiseResolve !== 'function') {
        capReject(new TypeError('Promise resolve is not a function'));
        return resultPromise;
      }
      // Step 7-8: PerformPromiseRace, IfAbruptRejectPromise.
      try {
        for (var value of values) {
          promiseResolve.call(C, value).then(capResolve, capReject);
        }
      } catch (e) {
        capReject(e);
      }
      return resultPromise;
    }

    static any(values) {
      var C = this;
      // Step 2: NewPromiseCapability(C).
      var capability = newPromiseCapability(C);
      var resultPromise = capability.promise;
      var capResolve = capability.resolve;
      var capReject = capability.reject;
      // Step 3-4: GetPromiseResolve(C), IfAbruptRejectPromise.
      var promiseResolve;
      try {
        promiseResolve = C.resolve;
      } catch (e) {
        capReject(e);
        return resultPromise;
      }
      if (typeof promiseResolve !== 'function') {
        capReject(new TypeError('Promise resolve is not a function'));
        return resultPromise;
      }
      var errors = [];
      var remainingElementsCount = 1;
      var index = 0;
      // Per spec §27.2.4.3.1 step r: onFulfilled is
      // capability.[[Resolve]] directly. Only onRejected is wrapped
      // per element with its own [[AlreadyCalled]] flag. Per spec
      // §27.2.4.3.1, the Promise.any Reject Element Function is a
      // built-in without [[Construct]] — use arrow function.
      function makeRejectElement(idx) {
        var alreadyCalled = false;
        return (reason) => {
          if (alreadyCalled) return;
          alreadyCalled = true;
          Object.defineProperty(errors, idx, {
            value: reason, writable: true,
            enumerable: true, configurable: true,
          });
          if (--remainingElementsCount === 0) {
            capReject(getAggregateError(errors));
          }
        };
      }
      // Step 7-8: PerformPromiseAny, IfAbruptRejectPromise.
      try {
        for (var value of values) {
          var rejectElement = makeRejectElement(index);
          remainingElementsCount++;
          promiseResolve.call(C, value).then(capResolve, rejectElement);
          index++;
        }
        if (--remainingElementsCount === 0) {
          capReject(getAggregateError(errors));
        }
      } catch (e) {
        capReject(e);
      }
      return resultPromise;
    }

    // Per spec §27.2.4.9 Promise.withResolvers:
    //   1. Let promiseCapability be ? NewPromiseCapability(this).
    //   2. Return { promise, resolve, reject } from the capability.
    // Delegate to newPromiseCapability so that IsConstructor(C),
    // double-call detection (GetCapabilitiesExecutor steps 3-4), and
    // the final IsCallable(resolve/reject) check (§27.2.1.5 step 5)
    // are all enforced. Its return shape already matches what spec
    // requires here.
    static withResolvers() {
      return newPromiseCapability(this);
    }

    // Per spec §27.2.4.9 Promise.try ( callback, ...args )
    static try(callbackfn) {
      var C = this;
      // Step 2-3: NewPromiseCapability(C).
      var capability = newPromiseCapability(C);
      var capResolve = capability.resolve;
      var capReject = capability.reject;
      // Step 4-6: Call the callback. If it throws (abrupt), reject;
      // otherwise resolve. Throws from resolve/reject themselves
      // propagate per spec §27.2.4.9 steps 5.a / 6.a.
      var ret;
      try {
        if (arguments.length <= 1) {
          // Fast path: no extra args. Per spec step 4 args would be
          // an empty List; Call(callbackfn, undefined, «») is
          // equivalent to callbackfn(). Skips the args-array
          // allocation and .apply dispatch.
          ret = callbackfn();
        } else {
          // Collect extra arguments beyond the first.
          var args = [];
          for (var i = 1; i < arguments.length; i++) {
            args.push(arguments[i]);
          }
          ret = callbackfn.apply(undefined, args);
        }
      } catch (e) {
        capReject(e);
        return capability.promise;
      }
      capResolve(ret);
      return capability.promise;
    }

    /* Prototype Methods */

    then(onFulfilled, onRejected) {
      // Per spec §27.2.5.4 step 2: If IsPromise(this) is false, throw
      // TypeError. We approximate IsPromise via instanceof Promise (no
      // real [[PromiseState]] internal slot in pure JS). The check
      // must happen BEFORE reading this.constructor — otherwise a
      // hostile `constructor` getter would observe the call before
      // validation.
      if (!(this instanceof Promise)) {
        throw new TypeError('Promise.prototype.then called on non-Promise');
      }
      // Per spec §27.2.5.4 step 3 + §7.3.22 SpeciesConstructor: when
      // `constructor` is undefined, fall back to %Promise% — i.e. take
      // the fast path. Without this, an `undefined` constructor would
      // hit safeThen → newPromiseCapability(undefined) → spurious
      // TypeError where the spec returns a normal Promise.
      var C = this.constructor;
      if (C !== undefined && C !== Promise) {
        return safeThen(this, onFulfilled, onRejected);
      }
      var res = new Promise(noop);
      handle(this, new Handler(onFulfilled, onRejected, res));
      return res;
    }

    catch(onRejected) {
      // Per spec §27.2.5.1 step 2: Invoke(this, "then", « undefined,
      // onRejected »).
      return this.then(undefined, onRejected);
    }

    // Per spec §27.2.5.3: Promise.prototype.finally ( onFinally )
    finally(onFinally) {
      // Per spec §27.2.5.3 step 2: If Type(this) is not Object, throw
    // TypeError. Includes null (typeof null === 'object', but null
    // is the Null type, not Object).
    if (this === null ||
        (typeof this !== 'object' && typeof this !== 'function')) {
      throw new TypeError('Promise.prototype.finally called on non-object');
    }
    // Spec step 3 calls SpeciesConstructor(this, %Promise%); we
      // don't support @@species (perf), so fall back to
      // this.constructor (gives subclass dispatch for non-species-
      // overriding subclasses).
      var C = this.constructor;
      var thenFinally, catchFinally;
      if (typeof onFinally !== 'function') {
        thenFinally = onFinally;
        catchFinally = onFinally;
      } else {
        // Then Finally Function and Catch Finally Function per spec
        // are anonymous built-in functions (not constructors,
        // length=1). Use arrow functions so they are not
        // constructors and are anonymous (array-literal element
        // position avoids NamedEvaluation per §8.5.5). Per spec
        // §27.2.5.3.1 step 4, the result of onFinally() is wrapped
        // via PromiseResolve(C, result) — delegating to C.resolve
        // covers that (it handles same-Promise short-circuit,
        // thenable assimilation, and value wrapping).
        var fns = [(value) => {
          return C.resolve(onFinally()).then(() => value);
        }, (reason) => {
          return C.resolve(onFinally()).then(() => { throw reason; });
        }];
        thenFinally = fns[0];
        catchFinally = fns[1];
      }
      return this.then(thenFinally, catchFinally);
    }
  }
  var core = Promise;

  function safeThen(self, onFulfilled, onRejected) {
    // Per spec §27.2.5.4: NewPromiseCapability runs BEFORE
    // PerformPromiseThen attaches the reaction.
    var capability = newPromiseCapability(self.constructor);
    // Intermediate core Promise that holds the result of running
    // onFulfilled/onRejected when self settles.
    var res = new Promise(noop);
    handle(self, new Handler(onFulfilled, onRejected, res));
    // Chain res's settlement to the C-typed capability.
    handle(res, new Handler(capability.resolve, capability.reject,
      new Promise(noop)));
    return capability.promise;
  }

  // Per spec §27.2.1.5 NewPromiseCapability(C). Returns a record with
  // `promise`, `resolve`, and `reject`. Throws TypeError if C is not a
  // constructor, if the executor closure is called when [[Resolve]] or
  // [[Reject]] is already non-undefined (per spec's
  // GetCapabilitiesExecutor §27.2.1.5.1 steps 3-4), or if resolve/reject
  // are not callable after C's constructor returns.
  function newPromiseCapability(C) {
    if (C === null || (typeof C !== 'object' && typeof C !== 'function')) {
      throw new TypeError('Promise constructor is not an object');
    }
    var resolve, reject;
    // Per spec §27.2.1.5.1 GetCapabilitiesExecutor: the executor is a
    // built-in function without [[Construct]]. Arrow function provides
    // that and avoids any [[Construct]] / [[HomeObject]] surface.
    var promise = new C((resolveArg, rejectArg) => {
      // Per spec §27.2.1.5.1 steps 3-4: only throw if the corresponding
      // slot has already been set to a non-undefined value. Calls with
      // undefined args from a constructor that calls executor multiple
      // times are tolerated as long as no slot was non-undefined yet.
      if (resolve !== undefined) {
        throw new TypeError('Promise capability resolve already captured');
      }
      if (reject !== undefined) {
        throw new TypeError('Promise capability reject already captured');
      }
      resolve = resolveArg;
      reject = rejectArg;
    });
    if (typeof resolve !== 'function' || typeof reject !== 'function') {
      throw new TypeError('Promise capability resolve/reject is not a function');
    }
    return { promise: promise, resolve: resolve, reject: reject };
  }
  function handle(self, deferred) {
    while (self._y === 3) {
      self = self._z;
    }
    if (Promise._B) {
      Promise._B(self);
    }
    if (self._y === 0) {
      if (self._x === 0) {
        self._x = 1;
        self._A = deferred;
        return;
      }
      if (self._x === 1) {
        self._x = 2;
        self._A = [self._A, deferred];
        return;
      }
      self._A.push(deferred);
      return;
    }
    handleResolved(self, deferred);
  }

  function handleResolved(self, deferred) {
    (useEngineQueue ? HermesInternal.enqueueJob : setImmediate)(function() {
      var cb = self._y === 1 ? deferred.onFulfilled : deferred.onRejected;
      var promise = deferred.promise;
      if (promise === null) {
        // Internal reaction: its result is discarded, so only a failure of
        // the callback itself needs reporting.
        if (cb !== null && tryCallOne(cb, self._z) === IS_ERROR) {
          rejectCore(deferred.errorPromise, LAST_ERROR);
        }
        return;
      }
      if (cb === null) {
        if (self._y === 1) {
          resolve(promise, self._z);
        } else {
          reject(promise, self._z);
        }
        return;
      }
      var ret = tryCallOne(cb, self._z);
      if (ret === IS_ERROR) {
        reject(promise, LAST_ERROR);
      } else {
        resolve(promise, ret);
      }
    });
  }
  function resolve(self, newValue) {
    // Promise Resolution Procedure: https://github.com/promises-aplus/promises-spec#the-promise-resolution-procedure
    if (newValue === self) {
      return reject(
        self,
        new TypeError('A promise cannot be resolved with itself.')
      );
    }
    if (
      newValue &&
      (typeof newValue === 'object' || typeof newValue === 'function')
    ) {
      var then = getThen(newValue);
      if (then === IS_ERROR) {
        return reject(self, LAST_ERROR);
      }
      if (
        then === self.then &&
        newValue instanceof Promise &&
        newValue.constructor === Promise
      ) {
        self._y = 3;
        self._z = newValue;
        finale(self);
        return;
      } else if (typeof then === 'function') {
        doResolve(then.bind(newValue), self);
        return;
      }
    }
    self._y = 1;
    self._z = newValue;
    finale(self);
  }

  function reject(self, newValue) {
    self._y = 2;
    self._z = newValue;
    if (Promise._C) {
      Promise._C(self, newValue);
    }
    finale(self);
  }
  // The async-function driver settles its result Promise directly instead of
  // through a pair of resolving functions, so these stand in for the
  // [[AlreadyResolved]] guard that doResolve() would otherwise provide: a
  // throw escaping resolve()/reject() (a rejection-tracking hook, say) must
  // not let the caller's error path overwrite an already recorded state.
  function resolveCore(self, value) {
    if (self._y !== 0) return;
    resolve(self, value);
  }

  function rejectCore(self, value) {
    if (self._y !== 0) return;
    reject(self, value);
  }

  function finale(self) {
    if (self._x === 1) {
      handle(self, self._A);
      self._A = null;
    }
    if (self._x === 2) {
      for (var i = 0; i < self._A.length; i++) {
        handle(self, self._A[i]);
      }
      self._A = null;
    }
  }

  // Every reaction uses this one shape, so the property reads in
  // handleResolved stay monomorphic for Hermes' single-entry inline cache.
  class Handler {
    constructor(onFulfilled, onRejected, promise) {
      this.onFulfilled = typeof onFulfilled === 'function' ? onFulfilled : null;
      this.onRejected = typeof onRejected === 'function' ? onRejected : null;
      // A null `promise` marks an internal reaction, which has no result
      // Promise to settle and instead reports a callback failure by
      // rejecting `errorPromise`.
      this.promise = promise;
      this.errorPromise = null;
    }
  }

  /**
   * Take a potentially misbehaving resolver function and make sure
   * onFulfilled and onRejected are only called once.
   *
   * Makes no guarantees about asynchrony.
   */
  function doResolve(fn, promise) {
    var done = false;
    // Per spec §27.2.1.3 / §27.2.1.4: Promise Resolve/Reject Functions
    // are built-in functions without [[Construct]]. Use arrow functions
    // so `new resolve()` / `new reject()` correctly throw TypeError.
    var res = tryCallTwo(fn, (value) => {
      if (done) return;
      done = true;
      resolve(promise, value);
    }, (reason) => {
      if (done) return;
      done = true;
      reject(promise, reason);
    });
    if (!done && res === IS_ERROR) {
      done = true;
      reject(promise, LAST_ERROR);
    }
  }

  //This file contains the ES6 extensions to the core Promises/A+ API
  var es6Extensions = core;

  // Per spec §27.2.5.5: Promise.prototype[@@toStringTag] is "Promise".
  // Spec mandates a *data* property — class `get [Symbol.toStringTag]()`
  // would create an accessor instead, so we still set it explicitly.
  Object.defineProperty(core.prototype, Symbol.toStringTag, {
    value: 'Promise',
    writable: false,
    enumerable: false,
    configurable: true
  });

  var TRUE = valuePromise(true);
  var FALSE = valuePromise(false);
  var NULL = valuePromise(null);
  var UNDEFINED = valuePromise(undefined);
  var ZERO = valuePromise(0);
  var EMPTYSTRING = valuePromise('');

  function valuePromise(value) {
    var p = new core(core._D);
    p._y = 1;
    p._z = value;
    return p;
  }

  function getAggregateError(errors){
    if(typeof AggregateError === 'function'){
      return new AggregateError(errors,'All promises were rejected');
    }

    var error = new Error('All promises were rejected');

    error.name = 'AggregateError';
    error.errors = errors;

    return error;
  }

  var DEFAULT_WHITELIST = [
    ReferenceError,
    TypeError,
    RangeError
  ];

  var enabled = false;
  // The tracker of the last enable(), or null after disable(). A promise
  // can still hold a record of an earlier tracker, and the timer of that
  // record can still fire.
  var currentTracker = null;
  var disable_1 = disable;
  function disable() {
    enabled = false;
    currentTracker = null;
    core._B = null;
    core._C = null;
  }

  var enable_1 = enable;
  function enable(options) {
    options = options || {};
    if (enabled) disable();
    enabled = true;
    var tracker = {};
    currentTracker = tracker;
    var displayId = 0;
    core._B = function (promise) {
      if (promise._y !== 2) return; // NOT REJECTED
      var rejection = promise._E;
      if (rejection && rejection.tracker === tracker) {
        if (rejection.logged) {
          onHandled(rejection);
        } else {
          clearTimeout(rejection.timeout);
        }
        promise._E = null;
      }
    };
    core._C = function (promise, err) {
      if (promise._x === 0) { // not yet handled
        // Keep the record on the promise, not in a table of the tracker, so
        // that the record goes away with the promise, also after a report.
        var rejection = {
          tracker: tracker,
          displayId: null,
          error: err,
          promise: promise,
          timeout: null,
          logged: false
        };
        rejection.timeout = setTimeout(
          onUnhandled.bind(null, rejection),
          // For reference errors and type errors, this almost always
          // means the programmer made a mistake, so log them after just
          // 100ms
          // otherwise, wait 2 seconds to see if they get handled
          matchWhitelist(err, DEFAULT_WHITELIST)
            ? 100
            : 2000
        );
        promise._E = rejection;
      }
    };
    function onUnhandled(rejection) {
      if (currentTracker !== tracker) return;
      if (
        options.allRejections ||
        matchWhitelist(
          rejection.error,
          options.whitelist || DEFAULT_WHITELIST
        )
      ) {
        rejection.displayId = displayId++;
        if (options.onUnhandled) {
          rejection.logged = true;
          options.onUnhandled(
            rejection.displayId,
            rejection.error,
            rejection.promise
          );
        } else {
          rejection.logged = true;
          logError(
            rejection.displayId,
            rejection.error
          );
        }
      }
    }
    function onHandled(rejection) {
      if (rejection.logged) {
        if (options.onHandled) {
          options.onHandled(
            rejection.displayId,
            rejection.error,
            rejection.promise
          );
        } else if (!rejection.onUnhandled) {
          console.warn(
            'Promise Rejection Handled (id: ' + rejection.displayId + '):'
          );
          console.warn(
            '  This means you can ignore any previous messages of the form "Possible Unhandled Promise Rejection" with id ' +
            rejection.displayId + '.'
          );
        }
      }
    }
  }

  function logError(id, error) {
    console.warn('Possible Unhandled Promise Rejection (id: ' + id + '):');
    var errStr = (error && (error.stack || error)) + '';
    errStr.split('\n').forEach(function (line) {
      console.warn('  ' + line);
    });
  }

  function matchWhitelist(error, list) {
    return list.some(function (cls) {
      return error instanceof cls;
    });
  }

  var rejectionTracking = {
  	disable: disable_1,
  	enable: enable_1
  };

  // @nolint
  // This file is used to generate InternalBytecode/Promise.js
  // See InternalBytecode/README.md for more details.




  // Expose Promise to global, with the spec attributes for
  // built-in global properties (§19.4):
  // { writable: true, enumerable: false, configurable: true }.
  // Plain `globalThis.Promise = es6Extensions` would create an
  // enumerable property, failing test built-ins/Promise/promise.js.
  Object.defineProperty(globalThis, 'Promise', {
    value: es6Extensions, writable: true,
    enumerable: false, configurable: true,
  });

  // Expose an internal `then`-equivalent for cross-polyfill use (e.g.
  // the await polyfill in 02-AsyncFn.js). Unlike Promise.prototype.then,
  // this attaches the handlers via the polyfill's internal `handle()`
  // and skips the IsPromise / SpeciesConstructor checks (which read
  // `.constructor` and `@@species`). This matches the semantics of the
  // spec's PerformPromiseThen abstract operation (§27.2.5.4.1) — used
  // by Await — and avoids a redundant user-observable `.constructor`
  // read when the caller has already done its own check.
  // Note: keys not declared in Builtins.def are ignored by the
  // JS-builtins registration loop, so this is safe to attach here.
  internalBytecodeResult.performInternalThen = handle;
  // A reaction with no result Promise, reusable for every await of one
  // async function invocation. `errorPromise` is rejected if a callback
  // throws, and is required: nothing else can report that failure.
  internalBytecodeResult.createInternalHandler = function (
    onFulfilled,
    onRejected,
    errorPromise
  ) {
    var handler = new Handler(onFulfilled, onRejected, null);
    handler.errorPromise = errorPromise;
    return handler;
  };
  internalBytecodeResult.createPendingPromiseCore = function () {
    return new Promise(noop);
  };
  internalBytecodeResult.resolvePromiseCore = resolveCore;
  internalBytecodeResult.rejectPromiseCore = rejectCore;
  // PromiseResolve(%Promise%, value) for a caller that has already ruled out
  // the exact-Promise short-circuit of §27.2.1.6 step 1. Shares
  // Promise.resolve's cached value promises, so the common `await
  // <primitive>` shapes allocate nothing.
  internalBytecodeResult.promiseResolveCoreNonExact = function (value) {
    if (value === null) return NULL;
    if (value === undefined) return UNDEFINED;
    if (value === true) return TRUE;
    if (value === false) return FALSE;
    if (value === 0) {
      // ZERO is fulfilled with +0, so -0 has to be wrapped separately to
      // survive the await.
      return 1 / value === Infinity ? ZERO : valuePromise(value);
    }
    if (value === '') return EMPTYSTRING;
    if (typeof value !== 'object' && typeof value !== 'function') {
      // A primitive can never be a thenable, so skip the Promise Resolution
      // Procedure and fulfill directly.
      return valuePromise(value);
    }
    var promise = new Promise(noop);
    resolve(promise, value);
    return promise;
  };

  // register the JavaScript implemented `enable` function into
  // the Hermes' internal promise rejection tracker.
  var enableHook = rejectionTracking.enable;
  HermesInternal?.setPromiseRejectionTrackingHook?.(enableHook);

  var promise = {

  };

  return promise;

});
initPromise();
