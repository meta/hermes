/* @nolint */var initPromise = (function () {
  'use strict';
  var useEngineQueue = HermesInternal.useEngineQueue();
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

  var core = Promise;

  function Promise(fn) {
    if (typeof this !== 'object') {
      throw new TypeError('Promises must be constructed via new');
    }
    if (typeof fn !== 'function') {
      throw new TypeError('Promise constructor\'s argument is not a function');
    }
    this._h = 0;
    this._i = 0;
    this._j = null;
    this._k = null;
    if (fn === noop) return;
    doResolve(fn, this);
  }
  Promise._l = null;
  Promise._m = null;
  Promise._n = noop;

  Promise.prototype.then = function(onFulfilled, onRejected) {
    if (this.constructor !== Promise) {
      return safeThen(this, onFulfilled, onRejected);
    }
    var res = new Promise(noop);
    handle(this, new Handler(onFulfilled, onRejected, res));
    return res;
  };

  function safeThen(self, onFulfilled, onRejected) {
    return new self.constructor(function (resolve, reject) {
      var res = new Promise(noop);
      res.then(resolve, reject);
      handle(self, new Handler(onFulfilled, onRejected, res));
    });
  }
  function handle(self, deferred) {
    while (self._i === 3) {
      self = self._j;
    }
    if (Promise._l) {
      Promise._l(self);
    }
    if (self._i === 0) {
      if (self._h === 0) {
        self._h = 1;
        self._k = deferred;
        return;
      }
      if (self._h === 1) {
        self._h = 2;
        self._k = [self._k, deferred];
        return;
      }
      self._k.push(deferred);
      return;
    }
    handleResolved(self, deferred);
  }

  function handleResolved(self, deferred) {
    (useEngineQueue ? HermesInternal.enqueueJob : setImmediate)(function() {
      var cb = self._i === 1 ? deferred.onFulfilled : deferred.onRejected;
      if (cb === null) {
        if (self._i === 1) {
          resolve(deferred.promise, self._j);
        } else {
          reject(deferred.promise, self._j);
        }
        return;
      }
      var ret = tryCallOne(cb, self._j);
      if (ret === IS_ERROR) {
        reject(deferred.promise, LAST_ERROR);
      } else {
        resolve(deferred.promise, ret);
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
        newValue instanceof Promise
      ) {
        self._i = 3;
        self._j = newValue;
        finale(self);
        return;
      } else if (typeof then === 'function') {
        doResolve(then.bind(newValue), self);
        return;
      }
    }
    self._i = 1;
    self._j = newValue;
    finale(self);
  }

  function reject(self, newValue) {
    self._i = 2;
    self._j = newValue;
    if (Promise._m) {
      Promise._m(self, newValue);
    }
    finale(self);
  }
  function finale(self) {
    if (self._h === 1) {
      handle(self, self._k);
      self._k = null;
    }
    if (self._h === 2) {
      for (var i = 0; i < self._k.length; i++) {
        handle(self, self._k[i]);
      }
      self._k = null;
    }
  }

  function Handler(onFulfilled, onRejected, promise){
    this.onFulfilled = typeof onFulfilled === 'function' ? onFulfilled : null;
    this.onRejected = typeof onRejected === 'function' ? onRejected : null;
    this.promise = promise;
  }

  /**
   * Take a potentially misbehaving resolver function and make sure
   * onFulfilled and onRejected are only called once.
   *
   * Makes no guarantees about asynchrony.
   */
  function doResolve(fn, promise) {
    var done = false;
    var res = tryCallTwo(fn, function (value) {
      if (done) return;
      done = true;
      resolve(promise, value);
    }, function (reason) {
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

  /* Static Functions */

  var TRUE = valuePromise(true);
  var FALSE = valuePromise(false);
  var NULL = valuePromise(null);
  var UNDEFINED = valuePromise(undefined);
  var ZERO = valuePromise(0);
  var EMPTYSTRING = valuePromise('');

  function valuePromise(value) {
    var p = new core(core._n);
    p._i = 1;
    p._j = value;
    return p;
  }
  core.resolve = function (value) {
    if (value instanceof core) return value;

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
  };

  var iterableToArray = function (iterable) {
    if (typeof Array.from === 'function') {
      // ES2015+, iterables exist
      iterableToArray = Array.from;
      return Array.from(iterable);
    }

    // ES5, only arrays and array-likes exist
    iterableToArray = function (x) { return Array.prototype.slice.call(x); };
    return Array.prototype.slice.call(iterable);
  };

  core.all = function (arr) {
    var args = iterableToArray(arr);

    return new core(function (resolve, reject) {
      if (args.length === 0) return resolve([]);
      var remaining = args.length;
      function res(i, val) {
        if (val && (typeof val === 'object' || typeof val === 'function')) {
          if (val instanceof core && val.then === core.prototype.then) {
            while (val._i === 3) {
              val = val._j;
            }
            if (val._i === 1) return res(i, val._j);
            if (val._i === 2) reject(val._j);
            val.then(function (val) {
              res(i, val);
            }, reject);
            return;
          } else {
            var then = val.then;
            if (typeof then === 'function') {
              var p = new core(then.bind(val));
              p.then(function (val) {
                res(i, val);
              }, reject);
              return;
            }
          }
        }
        args[i] = val;
        if (--remaining === 0) {
          resolve(args);
        }
      }
      for (var i = 0; i < args.length; i++) {
        res(i, args[i]);
      }
    });
  };

  function onSettledFulfill(value) {
    return { status: 'fulfilled', value: value };
  }
  function onSettledReject(reason) {
    return { status: 'rejected', reason: reason };
  }
  function mapAllSettled(item) {
    if(item && (typeof item === 'object' || typeof item === 'function')){
      if(item instanceof core && item.then === core.prototype.then){
        return item.then(onSettledFulfill, onSettledReject);
      }
      var then = item.then;
      if (typeof then === 'function') {
        return new core(then.bind(item)).then(onSettledFulfill, onSettledReject)
      }
    }

    return onSettledFulfill(item);
  }
  core.allSettled = function (iterable) {
    return core.all(iterableToArray(iterable).map(mapAllSettled));
  };

  core.reject = function (value) {
    return new core(function (resolve, reject) {
      reject(value);
    });
  };

  core.race = function (values) {
    return new core(function (resolve, reject) {
      iterableToArray(values).forEach(function(value){
        core.resolve(value).then(resolve, reject);
      });
    });
  };

  /* Prototype Methods */

  core.prototype['catch'] = function (onRejected) {
    return this.then(null, onRejected);
  };

  function getAggregateError(errors){
    if(typeof AggregateError === 'function'){
      return new AggregateError(errors,'All promises were rejected');
    }

    var error = new Error('All promises were rejected');

    error.name = 'AggregateError';
    error.errors = errors;

    return error;
  }

  function withResolvers() {
    var resolve, reject;
    var promise = new this(function(doResolve, doReject) {
      resolve = doResolve;
      reject = doReject;
    });
    return { resolve, reject, promise };
  }

  core.withResolvers = withResolvers;

  core.any = function promiseAny(values) {
    return new core(function(resolve, reject) {
      var promises = iterableToArray(values);
      var hasResolved = false;
      var rejectionReasons = [];

      function resolveOnce(value) {
        if (!hasResolved) {
          hasResolved = true;
          resolve(value);
        }
      }

      function rejectionCheck(reason) {
        rejectionReasons.push(reason);

        if (rejectionReasons.length === promises.length) {
          reject(getAggregateError(rejectionReasons));
        }
      }

      if(promises.length === 0){
        reject(getAggregateError(rejectionReasons));
      } else {
        promises.forEach(function(value){
          core.resolve(value).then(resolveOnce, rejectionCheck);
        });
      }
    });
  };

  core.prototype.finally = function (f) {
    return this.then(function (value) {
      return core.resolve(f()).then(function () {
        return value;
      });
    }, function (err) {
      return core.resolve(f()).then(function () {
        throw err;
      });
    });
  };

  var DEFAULT_WHITELIST = [
    ReferenceError,
    TypeError,
    RangeError
  ];

  var enabled = false;
  var disable_1 = disable;
  function disable() {
    enabled = false;
    core._l = null;
    core._m = null;
  }

  var enable_1 = enable;
  function enable(options) {
    options = options || {};
    if (enabled) disable();
    enabled = true;
    var id = 0;
    var displayId = 0;
    var rejections = {};
    core._l = function (promise) {
      if (
        promise._i === 2 && // IS REJECTED
        rejections[promise._o]
      ) {
        if (rejections[promise._o].logged) {
          onHandled(promise._o);
        } else {
          clearTimeout(rejections[promise._o].timeout);
        }
        delete rejections[promise._o];
      }
    };
    core._m = function (promise, err) {
      if (promise._h === 0) { // not yet handled
        promise._o = id++;
        rejections[promise._o] = {
          displayId: null,
          error: err,
          promise: promise,
          timeout: setTimeout(
            onUnhandled.bind(null, promise._o),
            // For reference errors and type errors, this almost always
            // means the programmer made a mistake, so log them after just
            // 100ms
            // otherwise, wait 2 seconds to see if they get handled
            matchWhitelist(err, DEFAULT_WHITELIST)
              ? 100
              : 2000
          ),
          logged: false
        };
      }
    };
    function onUnhandled(id) {
      if (
        options.allRejections ||
        matchWhitelist(
          rejections[id].error,
          options.whitelist || DEFAULT_WHITELIST
        )
      ) {
        rejections[id].displayId = displayId++;
        if (options.onUnhandled) {
          rejections[id].logged = true;
          options.onUnhandled(
            rejections[id].displayId,
            rejections[id].error,
            rejections[id].promise
          );
        } else {
          rejections[id].logged = true;
          logError(
            rejections[id].displayId,
            rejections[id].error
          );
        }
      }
    }
    function onHandled(id) {
      if (rejections[id].logged) {
        if (options.onHandled) {
          options.onHandled(
            rejections[id].displayId,
            rejections[id].error,
            rejections[id].promise
          );
        } else if (!rejections[id].onUnhandled) {
          console.warn(
            'Promise Rejection Handled (id: ' + rejections[id].displayId + '):'
          );
          console.warn(
            '  This means you can ignore any previous messages of the form "Possible Unhandled Promise Rejection" with id ' +
            rejections[id].displayId + '.'
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





  // expose Promise to global.
  globalThis.Promise = es6Extensions;

  // register the JavaScript implemented `enable` function into
  // the Hermes' internal promise rejection tracker.
  var enableHook = rejectionTracking.enable;
  HermesInternal?.setPromiseRejectionTrackingHook?.(enableHook);

  var promise = {

  };

  return promise;

});
if (HermesInternal?.hasPromise?.()) {
  initPromise();
}
