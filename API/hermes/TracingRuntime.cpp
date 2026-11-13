/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/TracingRuntime.h>

#include <hermes/BCGen/HBC/BCProvider.h>
#include <hermes/Platform/Logging.h>
#include <hermes/Support/Algorithms.h>
#include <hermes/Support/JSONEmitter.h>
#include <hermes/VM/Callable.h>
#include <hermes/VM/JSObject.h>
#include <hermes/VM/Operations.h>
#include <hermes/VM/Predefined.h>
#include <hermes/VM/Runtime-inline.h>
#include <hermes/VM/Runtime.h>
#include <hermes/VM/SerializedValue.h>

#include <llvh/ADT/ArrayRef.h>
#include <llvh/Support/raw_ostream.h>
#include "llvh/Support/FileSystem.h"
#include "llvh/Support/SHA1.h"

#include <math.h>
#include <cmath>
#include <cstdint>

namespace facebook {
namespace hermes {
namespace tracing {

namespace {

using namespace ::hermes;
using namespace ::hermes::vm;

using Math1ArgFuncPtr = double (*)(double);
using Math2ArgFuncPtr = double (*)(double, double);

struct TraceableMathBuiltin {
  Predefined::Str name;
  BuiltinMethod::Enum builtinIndex;
  unsigned paramCount;
  Math1ArgFuncPtr function1Arg;
  Math2ArgFuncPtr function2Arg;
};

/// Stabilize the result of a math function across platforms by dropping the
/// last bit.
HermesValue stabilizeMathResult(HermesValue result) {
  double number = result.getNumber();
  if (LLVM_UNLIKELY(!std::isfinite(number))) {
    return result;
  }

  uint64_t bits = llvh::DoubleToBits(number);
  bits &= ~uint64_t{1};
  return HermesValue::encodeTrustedNumberValue(llvh::BitsToDouble(bits));
}

CallResult<HermesValue> traceableMathFunction1Arg(void *ctx, Runtime &runtime) {
  auto args = runtime.getCurrentFrame().getNativeArgs();
  auto function = reinterpret_cast<Math1ArgFuncPtr>(ctx);

  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double result = function(res->getNumber());
  return stabilizeMathResult(HermesValue::encodeTrustedNumberValue(result));
}

CallResult<HermesValue> traceableMathFunction2Arg(void *ctx, Runtime &runtime) {
  auto args = runtime.getCurrentFrame().getNativeArgs();
  auto function = reinterpret_cast<Math2ArgFuncPtr>(ctx);

  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double arg0 = res->getNumber();

  res = toNumber_RJS(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double result = function(arg0, res->getNumber());
  return stabilizeMathResult(HermesValue::encodeTrustedNumberValue(result));
}

} // namespace

TracingRuntime::TracingRuntime(
    std::shared_ptr<jsi::Runtime> runtime,
    const ::hermes::vm::RuntimeConfig &conf,
    std::unique_ptr<llvh::raw_ostream> traceStream)
    : RuntimeDecorator<jsi::Runtime>(*runtime),
      runtime_(std::move(runtime)),
      trace_(conf, std::move(traceStream)),
      numPreambleRecords_(0) {}

SynthTrace::ObjectID TracingRuntime::useObjectID(const jsi::Pointer &p) const {
  const jsi::Runtime::PointerValue *pv = getPointerValue(p);
  auto it = uniqueIDs_.find(pv);
  assert(it != uniqueIDs_.end() && "p has no def");
  return it->second;
}

SynthTrace::ObjectID TracingRuntime::defObjectID(const jsi::Pointer &p) {
  uniqueIDs_[getPointerValue(p)] = currentUniqueID_;
  return currentUniqueID_++;
}

void installTraceableBuiltinWrappers(jsi::Runtime &jsRuntime) {
  // jsRuntime could be TracingRuntime if run `synth` with `--trace` flag.
  if (auto *tracingRuntime = dynamic_cast<TracingRuntime *>(&jsRuntime)) {
    installTraceableBuiltinWrappers(tracingRuntime->plain());
    return;
  }

  // These static Math builtins are not required by IEEE 754 to be correctly
  // rounded, and may differ across libm implementations. Do not include
  // operations like sqrt or fma, which IEEE 754 does require to be correctly
  // rounded.
  // NOTE: This mapping needs to be synced with MathStdFunctions.def (we can't
  // access it here) and Builtins.def.
  static const TraceableMathBuiltin traceableMathBuiltins[] = {
      {Predefined::acos, BuiltinMethod::Math_acos, 1, std::acos, nullptr},
      {Predefined::acosh, BuiltinMethod::Math_acosh, 1, ::acosh, nullptr},
      {Predefined::asin, BuiltinMethod::Math_asin, 1, std::asin, nullptr},
      {Predefined::asinh, BuiltinMethod::Math_asinh, 1, ::asinh, nullptr},
      {Predefined::atan, BuiltinMethod::Math_atan, 1, std::atan, nullptr},
      {Predefined::atanh, BuiltinMethod::Math_atanh, 1, ::atanh, nullptr},
      {Predefined::atan2, BuiltinMethod::Math_atan2, 2, nullptr, std::atan2},
      {Predefined::cbrt, BuiltinMethod::Math_cbrt, 1, ::cbrt, nullptr},
      {Predefined::cos, BuiltinMethod::Math_cos, 1, std::cos, nullptr},
      {Predefined::cosh, BuiltinMethod::Math_cosh, 1, ::cosh, nullptr},
      {Predefined::exp, BuiltinMethod::Math_exp, 1, std::exp, nullptr},
      {Predefined::expm1, BuiltinMethod::Math_expm1, 1, ::expm1, nullptr},
      {Predefined::log, BuiltinMethod::Math_log, 1, std::log, nullptr},
      {Predefined::log10, BuiltinMethod::Math_log10, 1, std::log10, nullptr},
      {Predefined::log1p, BuiltinMethod::Math_log1p, 1, ::log1p, nullptr},
      {Predefined::log2, BuiltinMethod::Math_log2, 1, std::log2, nullptr},
      {Predefined::pow, BuiltinMethod::Math_pow, 2, nullptr, expOp},
      {Predefined::sin, BuiltinMethod::Math_sin, 1, std::sin, nullptr},
      {Predefined::sinh, BuiltinMethod::Math_sinh, 1, ::sinh, nullptr},
      {Predefined::tan, BuiltinMethod::Math_tan, 1, std::tan, nullptr},
      {Predefined::tanh, BuiltinMethod::Math_tanh, 1, ::tanh, nullptr},
  };

  auto *hermes = jsi::castInterface<IHermes>(&jsRuntime);
  if (!hermes) {
    return;
  }

  auto *runtime = static_cast<Runtime *>(hermes->getVMRuntimeUnsafe());
  GCScope gcScope(*runtime);
  struct : public Locals {
    PinnedValue<JSObject> math;
    PinnedValue<NativeFunction> replacementMathFunction;
  } lv;
  LocalsRAII lraii{*runtime, &lv};

  auto mathRes = JSObject::getNamed_RJS(
      runtime->getGlobal(),
      *runtime,
      Predefined::getSymbolID(Predefined::Math));
  if (LLVM_UNLIKELY(mathRes == ExecutionStatus::EXCEPTION)) {
    runtime->clearThrownValue();
    throw jsi::JSINativeException(
        "Failed to get Math while installing trace wrappers");
  }
  lv.math.castAndSetHermesValue<JSObject>(mathRes->getHermesValue());

  DefinePropertyFlags mathWrapperDPF =
      DefinePropertyFlags::getNewNonEnumerableFlags();
  for (const TraceableMathBuiltin &traceableMathBuiltin :
       traceableMathBuiltins) {
    GCScopeMarkerRAII marker{gcScope};
    SymbolID functionID = Predefined::getSymbolID(traceableMathBuiltin.name);
    void *functionContext;
    NativeFunctionPtr nativeFunctionPtr;
    if (traceableMathBuiltin.paramCount == 1) {
      functionContext =
          reinterpret_cast<void *>(traceableMathBuiltin.function1Arg);
      nativeFunctionPtr = traceableMathFunction1Arg;
    } else if (traceableMathBuiltin.paramCount == 2) {
      functionContext =
          reinterpret_cast<void *>(traceableMathBuiltin.function2Arg);
      nativeFunctionPtr = traceableMathFunction2Arg;
    } else {
      hermes_fatal("unsupported traceable Math function parameter count");
    }

    lv.replacementMathFunction = *NativeFunction::create(
        *runtime,
        functionContext,
        nativeFunctionPtr,
        functionID,
        traceableMathBuiltin.paramCount);

    auto defineRes = JSObject::defineOwnProperty(
        lv.math,
        *runtime,
        functionID,
        mathWrapperDPF,
        lv.replacementMathFunction);
    if (LLVM_UNLIKELY(defineRes == ExecutionStatus::EXCEPTION || !*defineRes)) {
      runtime->clearThrownValue();
      throw jsi::JSINativeException(
          "Failed to define Math function while installing wrappers");
    }

    runtime->overwriteBuiltinUnsafe(
        traceableMathBuiltin.builtinIndex, *lv.replacementMathFunction);
  }
}

void TracingRuntime::replaceNondeterministicFuncs() {
  // We trace non-deterministic functions by replacing them to call through a
  // HostFunction instead. The call from the HostFunction to the original
  // non-deterministic function is intentionally not traced. So the call and
  // response get recorded as just a call to a HostFunction.
  // This is a helper function to implement the above, it calls a given function
  // without tracing, and returns the result.
  jsi::Function callUntraced = jsi::Function::createFromHostFunction(
      *this,
      jsi::PropNameID::forAscii(*this, "callUntraced"),
      1,
      [this](
          Runtime &rt,
          const jsi::Value &thisVal,
          const jsi::Value *args,
          size_t count) {
        auto fun = args[0].getObject(*runtime_).getFunction(*runtime_);
        jsi::Value result;
        if (count > 1 && args[1].isObject()) {
          result = fun.callWithThis(
              *runtime_, args[1].asObject(*runtime_), &args[2], count - 2);
        } else {
          result = fun.call(*runtime_);
        }

        if (result.isString()) {
          // Recreate the result string via the TracingRuntime, so the string
          // appears in the resulting trace.
          const std::string resultStr =
              result.getString(*runtime_).utf8(*runtime_);
          jsi::String tracedResult = jsi::String::createFromUtf8(rt, resultStr);
          return jsi::Value(std::move(tracedResult));
        } else {
          // Other values must be primitives that will be included directly in
          // the trace.
          assert(
              (result.isUndefined() || result.isNull() || result.isNumber() ||
               result.isBool()) &&
              "Result is a pointer");
          return result;
        }
      });

  installTraceableBuiltinWrappers(*runtime_);

  // Below two host functions are for WeakRef hook.
  // We assign a new ObjectID for PointerValuea*, but for the case of Object
  // referenced by WeakRef, we need to override the PointerValue* for the
  // ObjectID of the same Object when the WeakRef's deref is called. That's
  // because we will get different PointerValue* when deref is called and a new
  // jsi::Object is created, but still need to associate the new PointerValue*
  // to the original ObjectID that was assigned when the WeakRef was created, so
  // that at replay time, TraceInterpreter can associate the def and use of the
  // same Object referred by the WeakRef.

  /// Returns a newly assigned ObjectID for a given object as an argument.
  jsi::Function recordWeakRefCreation = jsi::Function::createFromHostFunction(
      *this,
      jsi::PropNameID::forAscii(*this, "recordWeakRefCreation"),
      1,
      [this](
          Runtime &, const jsi::Value &, const jsi::Value *args, size_t count) {
        assert(count == 4);
        // Call these without tracing
        Runtime &noTracingRt = *runtime_;

        // Get the ObjectID for the given object.
        const auto obj = args[0].getObject(noTracingRt);
        SynthTrace::ObjectID id = useObjectID(obj);

        // Record it to refMap with the WeakRef object as the key.
        const auto refMap = args[1].getObject(noTracingRt);
        const auto refMapSet =
            args[2].getObject(noTracingRt).getFunction(noTracingRt);
        const auto weakRefObj = args[3].getObject(noTracingRt);
        // refMap.set(this, id);
        refMapSet.callWithThis(noTracingRt, refMap, weakRefObj, (double)id);

        return jsi::Value::undefined();
      });

  /// Override the PointerValue of given jsi::Object with the given ObjectID.
  jsi::Function recordWeakRefDeref = jsi::Function::createFromHostFunction(
      *this,
      jsi::PropNameID::forAscii(*this, "recordWeakRefDeref"),
      1,
      [this](
          Runtime &, const jsi::Value &, const jsi::Value *args, size_t count) {
        assert(count == 4);

        // Call these without tracing
        Runtime &noTracingRt = *runtime_;
        const auto weakRefObj = args[0].getObject(noTracingRt);
        const auto refMap = args[1].getObject(noTracingRt);
        const auto refMapGet =
            args[2].getObject(noTracingRt).getFunction(noTracingRt);
        const auto refMapDeref =
            args[3].getObject(noTracingRt).getFunction(noTracingRt);

        // this.deref()
        jsi::Value derefRet = refMapDeref.callWithThis(noTracingRt, weakRefObj);
        if (derefRet.isObject()) {
          jsi::Object obj = derefRet.getObject(noTracingRt);
          // oid = refMap.get(this)
          SynthTrace::ObjectID oid =
              refMapGet.callWithThis(noTracingRt, refMap, weakRefObj)
                  .getNumber();
          uniqueIDs_[getPointerValue(obj)] = oid;
        }
        return derefRet;
      });

  auto code = R"(
(function(callUntraced, recordWeakRefCreation, recordWeakRefDeref){
  var mathRandomReal = Math.random;
  Math.random = function random() { return callUntraced(mathRandomReal); };

  if(globalThis.WeakRef){
    var refMap = new WeakMap();
    // Cache these in case user code tampers with the prototype.
    var refMapGet = refMap.get;
    var refMapSet = refMap.set;

    // Hook for constructor
    var WeakRefReal = globalThis.WeakRef;
    function WeakRef(arg){
      if (new.target){
        var ref = new WeakRefReal(arg);
        recordWeakRefCreation(arg, refMap, refMapSet, ref);
        return ref;
      }
      return WeakRefReal(arg);
    }

    // Hook for deref
    WeakRef.prototype = WeakRefReal.prototype;
    var derefReal = WeakRefReal.prototype.deref;
    WeakRef.prototype.deref = function deref() {
      return recordWeakRefDeref(this, refMap, refMapGet, derefReal);
    };
    globalThis.WeakRef = WeakRef;
  }

  // Trace date getters and conversion functions as they can produce values
  // that depend on the execution environment's timezone. Setters can set
  // values that use the timezone, but the effect will not be observable
  // because of the overridden getters.
  var DateReal = globalThis.Date;
  [
    "getDate",
    "getDay",
    "getFullYear",
    "getHours",
    "getMilliseconds",
    "getMinutes",
    "getMonth",
    "getSeconds",
    "getTime",
    "getTimezoneOffset",
    "getUTCDate",
    "getUTCDay",
    "getUTCFullYear",
    "getUTCHours",
    "getUTCMilliseconds",
    "getUTCMinutes",
    "getUTCMonth",
    "getUTCSeconds",
    "getYear",
    "toDateString",
    "toGMTString",
    "toISOString",
    "toJSON",
    "toLocaleDateString",
    "toLocaleString",
    "toLocaleTimeString",
    "toString",
    "toTimeString",
    "toUTCString",
    "valueOf",
    Symbol.toPrimitive
  ].forEach((property) => {
    const original = DateReal.prototype[property];
    const replacement = function(...args) {
      return callUntraced(original, this, ...args);
    };
    Object.defineProperty(replacement, "name", {value: original.name});
    Object.defineProperty(DateReal.prototype, property, {
      value: replacement,
      writable: true,
      configurable: true
    });
  });

  function Date(...args){
    // Trace calls to `Date()`.
    if(!new.target){
      return callUntraced(DateReal);
    }
    // While `new Date()` technically records the current time, there is no way
    // to observe it since we override all the getters.
    return new DateReal(...args);
  }
  // Cannot use Object.assign because Date methods are not enumerable
  for (p of Object.getOwnPropertyNames(DateReal)){
    Date[p] = DateReal[p];
  }
  var dateNowReal = DateReal.now;
  Date.now = function now() { return callUntraced(dateNowReal); };
  globalThis.Date = Date;

  const defineProperty = Object.defineProperty;
  const realStackPropertyGetter = Object.getOwnPropertyDescriptor(Error.prototype, 'stack').get;
  defineProperty(Error.prototype, 'stack', {
    get: function() {
      var stack = callUntraced(realStackPropertyGetter, this);
      // The real getter stores the stack on the error object, meaning that
      // the real getter (and this wrapper) will not be invoked again if the
      // stack is accessed again during recording. Mimic that behavior here,
      // so the getter is also not invoked again during replay.
      defineProperty(this, 'stack', {
        value: stack,
        writable: true,
        configurable: true
      });
      return stack;
    },
    configurable: true
  });
});
)";
  global()
      .getPropertyAsFunction(*this, "eval")
      .call(*this, code)
      .asObject(*this)
      .asFunction(*this)
      .call(
          *this,
          {{std::move(callUntraced)},
           {std::move(recordWeakRefCreation)},
           {std::move(recordWeakRefDeref)}});

  //
  // Wrapper for HermesInternal.getInstrumentedStats (or any other
  // non-deterministic functions that return JSObject)
  //
  jsi::Function callUntracedSimpleObjects =
      jsi::Function::createFromHostFunction(
          *this,
          jsi::PropNameID::forAscii(*this, "callUntracedSimpleObjects"),
          3,
          [this](
              Runtime &rt,
              const jsi::Value &, // thisVal
              const jsi::Value *args,
              size_t count) {
            assert(count == 3);
            // Use non-tracing runtime to call the original function and
            // stringify operation.
            Runtime &noTracingRt = *runtime_;
            const auto nativeFunc =
                args[0].getObject(noTracingRt).getFunction(noTracingRt);
            const auto jsonStringify =
                args[1].getObject(noTracingRt).getFunction(noTracingRt);
            const auto jsonParse =
                args[2].getObject(noTracingRt).getFunction(noTracingRt);

            // Call the original native function without tracing.
            const jsi::Value funcResult = nativeFunc.call(noTracingRt);

            // Stringify the result and convert it to UTF8 string;
            const std::string utf8 = jsonStringify.call(noTracingRt, funcResult)
                                         .asString(noTracingRt)
                                         .utf8(noTracingRt);

            // Recreate the result object from the string with TracingRuntime
            // (rt) so that we record this object creation in trace record.
            jsi::String str = jsi::String::createFromUtf8(rt, utf8);
            // Finally, parse the stringified result back to JS object.
            return jsonParse.call(rt, std::move(str));
          });

  code = R"(
(function(callUntracedSimpleObjects){
  // Capture the original JSON.stringify and JSON.parse functions in case they are overridden.
  var realJSONStringify = JSON.stringify;
  var realJSONParse = JSON.parse;
  var hermesInternalGetInstrumentedStatsReal = HermesInternal.getInstrumentedStats;
  HermesInternal.getInstrumentedStats = function getInstrumentedStats() {
    return callUntracedSimpleObjects(hermesInternalGetInstrumentedStatsReal,
      realJSONStringify, realJSONParse);
  };
  Object.freeze(HermesInternal);
});
)";
  global()
      .getPropertyAsFunction(*this, "eval")
      .call(*this, code)
      .asObject(*this)
      .asFunction(*this)
      .call(*this, {std::move(callUntracedSimpleObjects)});

  numPreambleRecords_ = trace_.records().size();
}

jsi::Value TracingRuntime::evaluateJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  ::hermes::SHA1 sourceHash{};
  bool sourceIsBytecode = false;
  auto *api = jsi::castInterface<IHermesRootAPI>(makeHermesRootAPI());
  if (api->isHermesBytecode(buffer->data(), buffer->size())) {
    sourceHash = ::hermes::hbc::BCProviderFromBuffer::getSourceHashFromBytecode(
        llvh::makeArrayRef(buffer->data(), buffer->size()));
    sourceIsBytecode = true;
  } else {
    sourceHash =
        llvh::SHA1::hash(llvh::makeArrayRef(buffer->data(), buffer->size()));
  }
  trace_.emplace_back<SynthTrace::BeginExecJSRecord>(
      getTimeSinceStart(), sourceURL, sourceHash, sourceIsBytecode);
  auto res = RD::evaluateJavaScript(buffer, sourceURL);
  trace_.emplace_back<SynthTrace::EndExecJSRecord>(
      getTimeSinceStart(), defTraceValue(res));
  return res;
}

void TracingRuntime::queueMicrotask(const jsi::Function &callback) {
  trace_.emplace_back<SynthTrace::QueueMicrotaskRecord>(
      getTimeSinceStart(), useObjectID(callback));
  RD::queueMicrotask(callback);
}

bool TracingRuntime::drainMicrotasks(int maxMicrotasksHint) {
  trace_.emplace_back<SynthTrace::DrainMicrotasksRecord>(
      getTimeSinceStart(), maxMicrotasksHint);
  return RD::drainMicrotasks(maxMicrotasksHint);
};

jsi::Object TracingRuntime::global() {
  auto obj = RD::global();
  trace_.emplace_back<SynthTrace::GlobalRecord>(
      getTimeSinceStart(), defObjectID(obj));
  return obj;
}

jsi::Object TracingRuntime::createObject() {
  auto obj = RD::createObject();
  trace_.emplace_back<SynthTrace::CreateObjectRecord>(
      getTimeSinceStart(), defObjectID(obj));
  return obj;
}

jsi::Object TracingRuntime::createObjectWithPrototype(
    const jsi::Value &prototype) {
  auto obj = RD::createObjectWithPrototype(prototype);
  trace_.emplace_back<SynthTrace::CreateObjectWithPrototypeRecord>(
      getTimeSinceStart(), defObjectID(obj), useTraceValue(prototype));
  return obj;
}

jsi::Object TracingRuntime::createObject(std::shared_ptr<jsi::HostObject> ho) {
  class TracingHostObject : public jsi::DecoratedHostObject {
   public:
    using jsi::DecoratedHostObject::DecoratedHostObject;

    jsi::Value get(jsi::Runtime &rt, const jsi::PropNameID &name) override {
      TracingRuntime &trt = tracingRuntime();
      trt.trace_.emplace_back<SynthTrace::GetPropertyNativeRecord>(
          trt.getTimeSinceStart(),
          objID_,
          trt.defObjectID(name),
          name.utf8(rt));

      try {
        // Note that this ignores the "rt" argument, passing the
        // DHO's cached decoratedRuntime() to the underlying HostObject's get.
        // In this case, that will be a TracingRuntime.
        auto ret = DecoratedHostObject::get(rt, name);

        trt.trace_.emplace_back<SynthTrace::GetPropertyNativeReturnRecord>(
            trt.getTimeSinceStart(), trt.useTraceValue(ret));

        return ret;
      } catch (...) {
        // TODO(T28293178): The trace currently has no way to model
        // exceptions thrown from C++ code.
        ::hermes::hermes_fatal(
            "Exception happened in native code during trace");
      }
    }

    void set(
        jsi::Runtime &rt,
        const jsi::PropNameID &name,
        const jsi::Value &value) override {
      TracingRuntime &trt = tracingRuntime();
      trt.trace_.emplace_back<SynthTrace::SetPropertyNativeRecord>(
          trt.getTimeSinceStart(),
          objID_,
          trt.defObjectID(name),
          name.utf8(rt),
          trt.defTraceValue(value));

      try {
        // Note that this ignores the "rt" argument, passing the
        // DHO's cached decoratedRuntime() to the underlying HostObject's set.
        // In this case, that will be a TracingRuntime.
        DecoratedHostObject::set(rt, name, value);
      } catch (...) {
        // TODO(T28293178): The trace currently has no way to model
        // exceptions thrown from C++ code.
        ::hermes::hermes_fatal(
            "Exception happened in native code during trace");
      }

      trt.trace_.emplace_back<SynthTrace::SetPropertyNativeReturnRecord>(
          trt.getTimeSinceStart());
    }

    std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime &rt) override {
      TracingRuntime &trt = tracingRuntime();
      trt.trace_.emplace_back<SynthTrace::GetNativePropertyNamesRecord>(
          trt.getTimeSinceStart(), objID_);
      std::vector<jsi::PropNameID> props;
      try {
        // Note that this ignores the "rt" argument, passing the
        // DHO's cached decoratedRuntime() to the underlying HostObject's
        // getPropertyNames.  In this case, that will be a TracingRuntime.
        props = DecoratedHostObject::getPropertyNames(rt);
      } catch (...) {
        // TODO(T28293178): The trace currently has no way to model
        // exceptions thrown from C++ code.
        ::hermes::hermes_fatal(
            "Exception happened in native code during trace");
      }

      std::vector<SynthTrace::TraceValue> propNameIDs;
      propNameIDs.reserve(props.size());
      for (const jsi::PropNameID &prop : props) {
        propNameIDs.emplace_back(
            SynthTrace::encodePropNameID(trt.useObjectID(prop)));
      }

      trt.trace_.emplace_back<SynthTrace::GetNativePropertyNamesReturnRecord>(
          trt.getTimeSinceStart(), propNameIDs);
      return props;
    }

    void setObjectID(SynthTrace::ObjectID id) {
      objID_ = id;
    }

   private:
    /// The TracingRuntime used when the TracingHostObject was created.
    TracingRuntime &tracingRuntime() {
      // A TracingHostObject is always created with a TracingRuntime,
      // so this cast is safe.
      return static_cast<TracingRuntime &>(decoratedRuntime());
    }
    /// The object id of the host object that this is attached to.
    SynthTrace::ObjectID objID_;
  };

  // These next two lines are very similar to the body of
  // RD::createObject:
  // return plain_.createObject(
  //     std::make_shared<DecoratedHostObject>(*this, std::move(ho)));
  // but (a) using the TracingHostObject subtype of
  // DecoratedHostObject, and (b) using createFromHostObject, because
  // createObject is protected, and is thus inaccessible here.
  auto tracer = std::make_shared<TracingHostObject>(*this, ho);
  auto obj = jsi::Object::createFromHostObject(plain(), tracer);

  tracer->setObjectID(defObjectID(obj));
  trace_.emplace_back<SynthTrace::CreateHostObjectRecord>(
      getTimeSinceStart(), useObjectID(obj));
  return obj;
}

jsi::BigInt TracingRuntime::createBigIntFromInt64(int64_t value) {
  jsi::BigInt res = RD::createBigIntFromInt64(value);
  trace_.emplace_back<SynthTrace::CreateBigIntRecord>(
      getTimeSinceStart(),
      defObjectID(res),
      SynthTrace::CreateBigIntRecord::Method::FromInt64,
      value);
  return res;
}

jsi::BigInt TracingRuntime::createBigIntFromUint64(uint64_t value) {
  jsi::BigInt res = RD::createBigIntFromUint64(value);
  trace_.emplace_back<SynthTrace::CreateBigIntRecord>(
      getTimeSinceStart(),
      defObjectID(res),
      SynthTrace::CreateBigIntRecord::Method::FromUint64,
      value);
  return res;
}

jsi::String TracingRuntime::bigintToString(
    const jsi::BigInt &bigint,
    int radix) {
  jsi::String res = RD::bigintToString(bigint, radix);
  trace_.emplace_back<SynthTrace::BigIntToStringRecord>(
      getTimeSinceStart(), defObjectID(res), useObjectID(bigint), radix);
  return res;
}

jsi::String TracingRuntime::createStringFromAscii(
    const char *str,
    size_t length) {
  jsi::String res = RD::createStringFromAscii(str, length);
  trace_.emplace_back<SynthTrace::CreateStringRecord>(
      getTimeSinceStart(), defObjectID(res), str, length);
  return res;
};

jsi::String TracingRuntime::createStringFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  jsi::String res = RD::createStringFromUtf8(utf8, length);
  trace_.emplace_back<SynthTrace::CreateStringRecord>(
      getTimeSinceStart(), defObjectID(res), utf8, length);
  return res;
};

jsi::String TracingRuntime::createStringFromUtf16(
    const char16_t *utf16,
    size_t length) {
  jsi::String res = RD::createStringFromUtf16(utf16, length);
  trace_.emplace_back<SynthTrace::CreateStringRecord>(
      getTimeSinceStart(), defObjectID(res), utf16, length);
  return res;
}

jsi::Value TracingRuntime::createValueFromJsonUtf8(
    const uint8_t *json,
    size_t length) {
  // Deliberately not RD::createValueFromJsonUtf8, which would parse in the
  // plain runtime and return a value that was never recorded. See the header.
  return jsi::Runtime::createValueFromJsonUtf8(json, length);
}

jsi::PropNameID TracingRuntime::createPropNameIDFromAscii(
    const char *str,
    size_t length) {
  jsi::PropNameID res = RD::createPropNameIDFromAscii(str, length);
  trace_.emplace_back<SynthTrace::CreatePropNameIDRecord>(
      getTimeSinceStart(), defObjectID(res), str, length);
  return res;
}

jsi::PropNameID TracingRuntime::createPropNameIDFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  jsi::PropNameID res = RD::createPropNameIDFromUtf8(utf8, length);
  trace_.emplace_back<SynthTrace::CreatePropNameIDRecord>(
      getTimeSinceStart(), defObjectID(res), utf8, length);
  return res;
}

jsi::PropNameID TracingRuntime::createPropNameIDFromUtf16(
    const char16_t *utf16,
    size_t length) {
  jsi::PropNameID res = RD::createPropNameIDFromUtf16(utf16, length);
  trace_.emplace_back<SynthTrace::CreatePropNameIDRecord>(
      getTimeSinceStart(), defObjectID(res), utf16, length);
  return res;
}

std::string TracingRuntime::utf8(const jsi::PropNameID &name) {
  std::string res = RD::utf8(name);
  trace_.emplace_back<SynthTrace::Utf8Record>(
      getTimeSinceStart(),
      SynthTrace::encodePropNameID(useObjectID(name)),
      res);
  return res;
}

std::string TracingRuntime::utf8(const jsi::String &str) {
  std::string res = RD::utf8(str);
  trace_.emplace_back<SynthTrace::Utf8Record>(
      getTimeSinceStart(), SynthTrace::encodeString(useObjectID(str)), res);
  return res;
}

std::string TracingRuntime::symbolToString(const jsi::Symbol &sym) {
  std::string res = RD::symbolToString(sym);
  trace_.emplace_back<SynthTrace::Utf8Record>(
      getTimeSinceStart(), SynthTrace::encodeSymbol(useObjectID(sym)), res);
  return res;
}

std::u16string TracingRuntime::utf16(const jsi::String &str) {
  std::u16string res = RD::utf16(str);
  trace_.emplace_back<SynthTrace::Utf16Record>(
      getTimeSinceStart(), SynthTrace::encodeString(useObjectID(str)), res);
  return res;
}

std::u16string TracingRuntime::utf16(const jsi::PropNameID &name) {
  std::u16string res = RD::utf16(name);
  trace_.emplace_back<SynthTrace::Utf16Record>(
      getTimeSinceStart(),
      SynthTrace::encodePropNameID(useObjectID(name)),
      res);
  return res;
}

void TracingRuntime::getStringData(
    const jsi::String &str,
    void *ctx,
    void (*cb)(void *ctx, bool ascii, const void *data, size_t num)) {
  struct Context {
    void *ctx;
    void (*cb)(void *ctx, bool ascii, const void *data, size_t num);
    std::u16string buffer;
  } context = {ctx, cb, u""};

  RD::getStringData(
      str, &context, [](void *ctx, bool ascii, const void *data, size_t num) {
        // Save the string content passed into the callback
        auto *context = (Context *)ctx;
        if (ascii) {
          context->buffer.append((const char *)data, (const char *)data + num);
        } else {
          context->buffer.append((const char16_t *)data, num);
        }

        // Invoke the original callback provided by the user
        context->cb(context->ctx, ascii, data, num);
      });

  trace_.emplace_back<SynthTrace::GetStringDataRecord>(
      getTimeSinceStart(),
      SynthTrace::encodeString(useObjectID(str)),
      std::move(context.buffer));
}

void TracingRuntime::getPropNameIdData(
    const jsi::PropNameID &sym,
    void *ctx,
    void (*cb)(void *ctx, bool ascii, const void *data, size_t num)) {
  struct Context {
    void *ctx;
    void (*cb)(void *ctx, bool ascii, const void *data, size_t num);
    std::u16string buffer;
  } context = {ctx, cb, u""};

  RD::getPropNameIdData(
      sym, &context, [](void *ctx, bool ascii, const void *data, size_t num) {
        // Save the string content passed into the callback
        auto *context = (Context *)ctx;
        if (ascii) {
          context->buffer.append((const char *)data, (const char *)data + num);
        } else {
          context->buffer.append((const char16_t *)data, num);
        }

        // Invoke the original callback provided by the user
        context->cb(context->ctx, ascii, data, num);
      });

  trace_.emplace_back<SynthTrace::GetStringDataRecord>(
      getTimeSinceStart(),
      SynthTrace::encodePropNameID(useObjectID(sym)),
      std::move(context.buffer));
}

jsi::PropNameID TracingRuntime::createPropNameIDFromString(
    const jsi::String &str) {
  jsi::PropNameID res = RD::createPropNameIDFromString(str);
  trace_.emplace_back<SynthTrace::CreatePropNameIDWithValueRecord>(
      getTimeSinceStart(),
      defObjectID(res),
      SynthTrace::encodeString(useObjectID(str)));
  return res;
}

jsi::PropNameID TracingRuntime::createPropNameIDFromSymbol(
    const jsi::Symbol &sym) {
  jsi::PropNameID res = RD::createPropNameIDFromSymbol(sym);
  trace_.emplace_back<SynthTrace::CreatePropNameIDWithValueRecord>(
      getTimeSinceStart(),
      defObjectID(res),
      SynthTrace::encodeSymbol(useObjectID(sym)));
  return res;
}

jsi::Value TracingRuntime::getProperty(
    const jsi::Object &obj,
    const jsi::String &name) {
  trace_.emplace_back<SynthTrace::GetPropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      SynthTrace::encodeString(useObjectID(name))
#ifdef HERMESVM_API_TRACE_DEBUG
          ,
      name.utf8(*this)
#endif
  );
  auto value = RD::getProperty(obj, name);
  trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
      getTimeSinceStart(), defTraceValue(value));
  return value;
}

jsi::Value TracingRuntime::getProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  trace_.emplace_back<SynthTrace::GetPropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      SynthTrace::encodePropNameID(useObjectID(name))
#ifdef HERMESVM_API_TRACE_DEBUG
          ,
      name.utf8(*this)
#endif
  );
  auto value = RD::getProperty(obj, name);
  trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
      getTimeSinceStart(), defTraceValue(value));
  return value;
}

jsi::Value TracingRuntime::getProperty(
    const jsi::Object &obj,
    const jsi::Value &name) {
  trace_.emplace_back<SynthTrace::GetPropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      useTraceValue(name)
#ifdef HERMESVM_API_TRACE_DEBUG
          ,
      SynthTrace::getDescriptiveString(*this, name)
#endif
  );
  auto value = RD::getProperty(obj, name);
  trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
      getTimeSinceStart(), defTraceValue(value));
  return value;
}

jsi::Value TracingRuntime::getPrototypeOf(const jsi::Object &object) {
  trace_.emplace_back<SynthTrace::GetPrototypeRecord>(
      getTimeSinceStart(), useObjectID(object));

  auto prototype = RD::getPrototypeOf(object);
  trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
      getTimeSinceStart(), defTraceValue(prototype));
  return prototype;
}

bool TracingRuntime::hasProperty(
    const jsi::Object &obj,
    const jsi::String &name) {
  trace_.emplace_back<SynthTrace::HasPropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      SynthTrace::encodeString(useObjectID(name))
#ifdef HERMESVM_API_TRACE_DEBUG
          ,
      name.utf8(*this)
#endif
  );
  return RD::hasProperty(obj, name);
}

bool TracingRuntime::hasProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  trace_.emplace_back<SynthTrace::HasPropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      SynthTrace::encodePropNameID(useObjectID(name))
#ifdef HERMESVM_API_TRACE_DEBUG
          ,
      name.utf8(*this)
#endif
  );
  return RD::hasProperty(obj, name);
}

bool TracingRuntime::hasProperty(
    const jsi::Object &obj,
    const jsi::Value &name) {
  trace_.emplace_back<SynthTrace::HasPropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      useTraceValue(name)
#ifdef HERMESVM_API_TRACE_DEBUG
          ,
      SynthTrace::getDescriptiveString(*this, name)
#endif
  );
  return RD::hasProperty(obj, name);
}

void TracingRuntime::setPropertyValue(
    const jsi::Object &obj,
    const jsi::String &name,
    const jsi::Value &value) {
  trace_.emplace_back<SynthTrace::SetPropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      SynthTrace::encodeString(useObjectID(name)),
#ifdef HERMESVM_API_TRACE_DEBUG
      name.utf8(*this),
#endif
      useTraceValue(value));
  RD::setPropertyValue(obj, name, value);
}

void TracingRuntime::setPropertyValue(
    const jsi::Object &obj,
    const jsi::PropNameID &name,
    const jsi::Value &value) {
  trace_.emplace_back<SynthTrace::SetPropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      SynthTrace::encodePropNameID(useObjectID(name)),
#ifdef HERMESVM_API_TRACE_DEBUG
      name.utf8(*this),
#endif
      useTraceValue(value));
  RD::setPropertyValue(obj, name, value);
}

void TracingRuntime::setPropertyValue(
    const jsi::Object &obj,
    const jsi::Value &name,
    const jsi::Value &value) {
  trace_.emplace_back<SynthTrace::SetPropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      useTraceValue(name),
#ifdef HERMESVM_API_TRACE_DEBUG
      SynthTrace::getDescriptiveString(*this, name),
#endif
      useTraceValue(value));
  RD::setPropertyValue(obj, name, value);
}

void TracingRuntime::deleteProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  trace_.emplace_back<SynthTrace::DeletePropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      SynthTrace::encodePropNameID(useObjectID(name)));
  RD::deleteProperty(obj, name);
}

void TracingRuntime::deleteProperty(
    const jsi::Object &obj,
    const jsi::String &name) {
  trace_.emplace_back<SynthTrace::DeletePropertyRecord>(
      getTimeSinceStart(),
      useObjectID(obj),
      SynthTrace::encodeString(useObjectID(name)));
  RD::deleteProperty(obj, name);
}

void TracingRuntime::deleteProperty(
    const jsi::Object &obj,
    const jsi::Value &name) {
  trace_.emplace_back<SynthTrace::DeletePropertyRecord>(
      getTimeSinceStart(), useObjectID(obj), useTraceValue(name));
  RD::deleteProperty(obj, name);
}

void TracingRuntime::setPrototypeOf(
    const jsi::Object &object,
    const jsi::Value &prototype) {
  trace_.emplace_back<SynthTrace::SetPrototypeRecord>(
      getTimeSinceStart(), useObjectID(object), useTraceValue(prototype));
  RD::setPrototypeOf(object, prototype);
}

jsi::Array TracingRuntime::getPropertyNames(const jsi::Object &o) {
  trace_.emplace_back<SynthTrace::GetPropertyNamesRecord>(
      getTimeSinceStart(), useObjectID(o));
  jsi::Array arr = RD::getPropertyNames(o);
  trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
      getTimeSinceStart(), SynthTrace::encodeObject(defObjectID(arr)));
  return arr;
}

jsi::WeakObject TracingRuntime::createWeakObject(const jsi::Object &o) {
  jsi::WeakObject w = RD::createWeakObject(o);
  // Make a relationship between the WeakObject's PointerValue and the ObjectID
  // of given jsi::Object. We use this ObjectID in lockWeakObject().
  weakRefIDs_[getPointerValue(w)] = useObjectID(o);
  return w;
}

jsi::Value TracingRuntime::lockWeakObject(const jsi::WeakObject &wo) {
  jsi::Value res = RD::lockWeakObject(wo);
  if (res.isUndefined()) {
    return res;
  }
  // If the JSObject is alive, find the original ObjectID that was assigned to
  // the jsi::Object passed to createWeakObject(), and use it as the ObjectID
  // for this newly returned jsi::Object. This is so that the replay can
  // associate the def of the ObjectID and use of the same ObjectID even when
  // the jsi::Object's PointerValues are different betweeen createWeakObject and
  // lockWeakObject.
  SynthTrace::ObjectID id = weakRefIDs_[getPointerValue(wo)];
  uniqueIDs_[getPointerValue(res)] = id;
  return res;
}

jsi::Array TracingRuntime::createArray(size_t length) {
  auto arr = RD::createArray(length);
  trace_.emplace_back<SynthTrace::CreateArrayRecord>(
      getTimeSinceStart(), defObjectID(arr), length);
  return arr;
}

jsi::ArrayBuffer TracingRuntime::createArrayBuffer(
    std::shared_ptr<jsi::MutableBuffer> buffer) {
  throw std::logic_error("Cannot create external ArrayBuffers in trace mode.");
}

jsi::Uint8Array TracingRuntime::createUint8Array(size_t length) {
  auto arr = RD::createUint8Array(length);
  trace_.emplace_back<SynthTrace::CreateUInt8ArrayRecord>(
      getTimeSinceStart(), defObjectID(arr), length);
  return arr;
}

jsi::Uint8Array TracingRuntime::createUint8Array(
    const jsi::ArrayBuffer &buffer,
    size_t offset,
    size_t length) {
  auto arr = RD::createUint8Array(buffer, offset, length);
  trace_.emplace_back<SynthTrace::CreateUInt8ArrayFromArrayBufferRecord>(
      getTimeSinceStart(),
      defObjectID(arr),
      useObjectID(buffer),
      offset,
      length);
  return arr;
}

jsi::ArrayBuffer TracingRuntime::buffer(const jsi::TypedArray &typedArray) {
  auto buf = RD::buffer(typedArray);
  trace_.emplace_back<SynthTrace::GetBufferFromTypedArrayRecord>(
      getTimeSinceStart(), defObjectID(buf), useObjectID(typedArray));
  return buf;
}

jsi::Value TracingRuntime::createError(const jsi::String &msg) {
  auto error = RD::createError(msg);
  trace_.emplace_back<SynthTrace::CreateJSErrorRecord>(
      getTimeSinceStart(),
      defObjectID(error.getObject(*this)),
      SynthTrace::JSErrorType::Error,
      useObjectID(msg));
  return error;
}

jsi::Value TracingRuntime::createEvalError(const jsi::String &msg) {
  auto error = RD::createEvalError(msg);
  trace_.emplace_back<SynthTrace::CreateJSErrorRecord>(
      getTimeSinceStart(),
      defObjectID(error.getObject(*this)),
      SynthTrace::JSErrorType::EvalError,
      useObjectID(msg));
  return error;
}

jsi::Value TracingRuntime::createRangeError(const jsi::String &msg) {
  auto error = RD::createRangeError(msg);
  trace_.emplace_back<SynthTrace::CreateJSErrorRecord>(
      getTimeSinceStart(),
      defObjectID(error.getObject(*this)),
      SynthTrace::JSErrorType::RangeError,
      useObjectID(msg));
  return error;
}

jsi::Value TracingRuntime::createReferenceError(const jsi::String &msg) {
  auto error = RD::createReferenceError(msg);
  trace_.emplace_back<SynthTrace::CreateJSErrorRecord>(
      getTimeSinceStart(),
      defObjectID(error.getObject(*this)),
      SynthTrace::JSErrorType::ReferenceError,
      useObjectID(msg));
  return error;
}

jsi::Value TracingRuntime::createSyntaxError(const jsi::String &msg) {
  auto error = RD::createSyntaxError(msg);
  trace_.emplace_back<SynthTrace::CreateJSErrorRecord>(
      getTimeSinceStart(),
      defObjectID(error.getObject(*this)),
      SynthTrace::JSErrorType::SyntaxError,
      useObjectID(msg));
  return error;
}

jsi::Value TracingRuntime::createTypeError(const jsi::String &msg) {
  auto error = RD::createTypeError(msg);
  trace_.emplace_back<SynthTrace::CreateJSErrorRecord>(
      getTimeSinceStart(),
      defObjectID(error.getObject(*this)),
      SynthTrace::JSErrorType::TypeError,
      useObjectID(msg));
  return error;
}

jsi::Value TracingRuntime::createURIError(const jsi::String &msg) {
  auto error = RD::createURIError(msg);
  trace_.emplace_back<SynthTrace::CreateJSErrorRecord>(
      getTimeSinceStart(),
      defObjectID(error.getObject(*this)),
      SynthTrace::JSErrorType::URIError,
      useObjectID(msg));
  return error;
}

size_t TracingRuntime::size(const jsi::Array &arr) {
  // Array size inquiries read from the length property, which is
  // non-configurable and thus cannot have side effects.
  return RD::size(arr);
}

size_t TracingRuntime::size(const jsi::ArrayBuffer &buf) {
  // ArrayBuffer size inquiries read from the byteLength property, which is
  // non-configurable and thus cannot have side effects.
  return RD::size(buf);
}

uint8_t *TracingRuntime::data(const jsi::ArrayBuffer &buf) {
  throw std::logic_error(
      "Cannot write raw bytes into an ArrayBuffer in trace mode");
}

jsi::Value TracingRuntime::getValueAtIndex(const jsi::Array &arr, size_t i) {
  trace_.emplace_back<SynthTrace::ArrayReadRecord>(
      getTimeSinceStart(), useObjectID(arr), i);
  auto value = RD::getValueAtIndex(arr, i);
  trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
      getTimeSinceStart(), defTraceValue(value));
  return value;
}

void TracingRuntime::setValueAtIndexImpl(
    const jsi::Array &arr,
    size_t i,
    const jsi::Value &value) {
  trace_.emplace_back<SynthTrace::ArrayWriteRecord>(
      getTimeSinceStart(), useObjectID(arr), i, useTraceValue(value));
  return RD::setValueAtIndexImpl(arr, i, value);
}

size_t TracingRuntime::push(
    const jsi::Array &arr,
    const jsi::Value *elements,
    size_t count) {
  size_t retVal = RD::push(arr, elements, count);
  trace_.emplace_back<SynthTrace::ArrayPushRecord>(
      getTimeSinceStart(),
      useObjectID(arr),
      argStringifyer(elements, count),
      retVal);
  return retVal;
}

jsi::Function TracingRuntime::createFunctionFromHostFunction(
    const jsi::PropNameID &name,
    unsigned int paramCount,
    jsi::HostFunctionType func) {
  class TracingHostFunction : public jsi::DecoratedHostFunction {
   public:
    using jsi::DecoratedHostFunction::DecoratedHostFunction;

    jsi::Value operator()(
        jsi::Runtime &rt,
        const jsi::Value &thisVal,
        const jsi::Value *args,
        size_t count) {
      TracingRuntime &trt = static_cast<TracingRuntime &>(decoratedRuntime());

      trt.trace_.emplace_back<SynthTrace::CallToNativeRecord>(
          trt.getTimeSinceStart(),
          functionID_,
          trt.defTraceValue(thisVal),
          trt.argStringifyer(args, count, true));

      try {
        auto ret =
            jsi::DecoratedHostFunction::operator()(rt, thisVal, args, count);

        trt.trace_.emplace_back<SynthTrace::ReturnFromNativeRecord>(
            trt.getTimeSinceStart(), trt.useTraceValue(ret));
        return ret;
      } catch (...) {
        // TODO(T28293178): The trace currently has no way to model
        // exceptions thrown from C++ code.
        ::hermes::hermes_fatal(
            "Exception happened in native code during trace");
      }
    }

    void setFunctionID(SynthTrace::ObjectID functionID) {
      functionID_ = functionID;
    }

   private:
    /// The object id of the function that this is attached to.
    SynthTrace::ObjectID functionID_;
  };
  auto tracer = TracingHostFunction(*this, func);
  auto tfunc = jsi::Function::createFromHostFunction(
      plain(), name, paramCount, std::move(tracer));
  const auto funcID = defObjectID(tfunc);
  tfunc.getHostFunction(plain()).target<TracingHostFunction>()->setFunctionID(
      funcID);
  trace_.emplace_back<SynthTrace::CreateHostFunctionRecord>(
      getTimeSinceStart(),
      funcID,
      useObjectID(name),
#ifdef HERMESVM_API_TRACE_DEBUG
      name.utf8(*this),
#endif
      paramCount);
  return tfunc;
}

jsi::Value TracingRuntime::call(
    const jsi::Function &func,
    const jsi::Value &jsThis,
    const jsi::Value *args,
    size_t count) {
  trace_.emplace_back<SynthTrace::CallFromNativeRecord>(
      getTimeSinceStart(),
      useObjectID(func),
      useTraceValue(jsThis),
      argStringifyer(args, count));
  auto retval = RD::call(func, jsThis, args, count);
  trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
      getTimeSinceStart(), defTraceValue(retval));
  return retval;
}

jsi::Value TracingRuntime::callAsConstructor(
    const jsi::Function &func,
    const jsi::Value *args,
    size_t count) {
  trace_.emplace_back<SynthTrace::ConstructFromNativeRecord>(
      getTimeSinceStart(),
      useObjectID(func),
      // A construct call always has an undefined this.
      // The ReturnToNativeRecord will contain the object that was either
      // created by the new keyword, or the objec that's returned from the
      // function.
      SynthTrace::encodeUndefined(),
      argStringifyer(args, count));
  auto retval = RD::callAsConstructor(func, args, count);
  trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
      getTimeSinceStart(), defTraceValue(retval));
  return retval;
}

void TracingRuntime::setExternalMemoryPressure(
    const jsi::Object &obj,
    size_t amount) {
  trace_.emplace_back<SynthTrace::SetExternalMemoryPressureRecord>(
      getTimeSinceStart(), useObjectID(obj), amount);
  RD::setExternalMemoryPressure(obj, amount);
}

std::shared_ptr<jsi::MutableBuffer> TracingRuntime::tryGetMutableBuffer(
    const jsi::ArrayBuffer &) {
  throw std::logic_error(
      "Cannot retrieve MutableBuffer from ArrayBuffer in trace mode.");
}

jsi::ICast *TracingRuntime::castInterface(const jsi::UUID &interfaceUUID) {
  auto *interface = RD::castInterface(interfaceUUID);
  if (!interface) {
    // Underlying runtime does not support this interface, so return null
    return nullptr;
  }
// We need to make sure we return the implementation provided by
// TracingRuntime, otherwise the records will not be created.
#ifdef JSI_UNSTABLE
  if (interfaceUUID == ISerialization::uuid) {
    return static_cast<jsi::ISerialization *>(this);
  }
#endif
  return nullptr;
}

#ifdef JSI_UNSTABLE
std::shared_ptr<jsi::Serialized> TracingRuntime::serialize(
    const jsi::Value &value) {
  auto serializationInterface =
      jsi::dynamicInterfaceCast<ISerialization>(runtime_);
  assert(
      serializationInterface &&
      "Underlying runtime does not implement ISerialization interface");
  trace_.emplace_back<SynthTrace::SerializeRecord>(
      getTimeSinceStart(), useTraceValue(value));
  return serializationInterface->serialize(value);
}

jsi::Value TracingRuntime::deserialize(
    const std::shared_ptr<jsi::Serialized> &serialized) {
  auto serializationInterface =
      jsi::dynamicInterfaceCast<ISerialization>(runtime_);
  assert(
      serializationInterface &&
      "Underlying runtime does not implement ISerialization interface");
  auto retVal = serializationInterface->deserialize(serialized);

  auto tracingHelperInterface =
      jsi::dynamicInterfaceCast<IHermesTracingHelpers>(runtime_);
  assert(
      tracingHelperInterface &&
      "Underlying runtime does not implement IHermesTracingHelpers interface");
  const ::hermes::vm::SerializedValue *serializedValue =
      tracingHelperInterface->getHermesSerializedValue(*serialized);
  trace_.emplace_back<SynthTrace::DeserializeRecord>(
      getTimeSinceStart(),
      serializedValue->offsets,
      serializedValue->content,
      serializedValue->strings);
  trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
      getTimeSinceStart(), defTraceValue(retVal));
  return retVal;
}

std::unique_ptr<jsi::Serialized> TracingRuntime::serializeWithTransfer(
    const jsi::Value &value,
    const jsi::Array &transferList) {
  throw std::logic_error("Cannot transfer JS Values in tracing mode.");
}

jsi::Array TracingRuntime::deserializeWithTransfer(
    std::unique_ptr<jsi::Serialized> &serialized) {
  throw std::logic_error("Cannot transfer JS Values in tracing mode.");
}

std::shared_ptr<jsi::Serialized> TracingRuntime::serializeBound(
    const jsi::Value &value) {
  auto serializationInterface =
      jsi::dynamicInterfaceCast<ISerialization>(runtime_);
  assert(
      serializationInterface &&
      "Underlying runtime does not implement ISerialization interface");
  trace_.emplace_back<SynthTrace::SerializeRecord>(
      getTimeSinceStart(), useTraceValue(value));
  return serializationInterface->serializeBound(value);
}

std::unique_ptr<jsi::Serialized> TracingRuntime::serializeBoundWithTransfer(
    const jsi::Value &value,
    const jsi::Array &transferList) {
  throw std::logic_error("Cannot transfer JS Values in tracing mode.");
}
#endif

void TracingRuntime::addMarker(const std::string &marker) {
  trace_.emplace_back<SynthTrace::MarkerRecord>(getTimeSinceStart(), marker);
}

std::vector<SynthTrace::TraceValue> TracingRuntime::argStringifyer(
    const jsi::Value *args,
    size_t count,
    bool assignNewUID /* = false */) {
  std::vector<SynthTrace::TraceValue> stringifiedArgs;
  stringifiedArgs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    stringifiedArgs.emplace_back(toTraceValue(args[i], assignNewUID));
  }
  return stringifiedArgs;
}

SynthTrace::TraceValue TracingRuntime::toTraceValue(
    const jsi::Value &value,
    bool assignNewUID /* = false */) {
  if (value.isUndefined()) {
    return SynthTrace::encodeUndefined();
  } else if (value.isNull()) {
    return SynthTrace::encodeNull();
  } else if (value.isBool()) {
    return SynthTrace::encodeBool(value.getBool());
  } else if (value.isNumber()) {
    return SynthTrace::encodeNumber(value.getNumber());
  } else if (value.isBigInt()) {
    return SynthTrace::encodeBigInt(
        assignNewUID ? defObjectID(value.getBigInt(*this))
                     : useObjectID(value.getBigInt(*this)));
  } else if (value.isString()) {
    return SynthTrace::encodeString(
        assignNewUID ? defObjectID(value.getString(*this))
                     : useObjectID(value.getString(*this)));
  } else if (value.isObject()) {
    // Get a unique identifier from the object, and use that instead. This is
    // so that object identity is tracked.
    return SynthTrace::encodeObject(
        assignNewUID ? defObjectID(value.getObject(*this))
                     : useObjectID(value.getObject(*this)));
  } else if (value.isSymbol()) {
    return SynthTrace::encodeSymbol(
        assignNewUID ? defObjectID(value.getSymbol(*this))
                     : useObjectID(value.getSymbol(*this)));
  } else {
    throw std::logic_error("Unsupported value reached");
  }
}

SynthTrace::TimeSinceStart TracingRuntime::getTimeSinceStart() const {
  return std::chrono::duration_cast<SynthTrace::TimeSinceStart>(
      std::chrono::steady_clock::now() - startTime_);
}

TracingHermesRuntime::TracingHermesRuntime(
    std::shared_ptr<HermesRuntime> runtime,
    const ::hermes::vm::RuntimeConfig &runtimeConfig,
    std::unique_ptr<llvh::raw_ostream> traceStream,
    std::function<std::string()> commitAction,
    std::function<void()> rollbackAction)
    : TracingRuntime(std::move(runtime), runtimeConfig, std::move(traceStream)),
      conf_(runtimeConfig),
      commitAction_(std::move(commitAction)),
      rollbackAction_(std::move(rollbackAction)),
      crashCallbackKey_(
          conf_.getCrashMgr()
              ? llvh::Optional<::hermes::vm::CrashManager::CallbackKey>(
                    conf_.getCrashMgr()->registerCallback(
                        [this](int fd) { crashCallback(fd); }))
              : llvh::None) {}

TracingHermesRuntime::~TracingHermesRuntime() {
  if (crashCallbackKey_) {
    conf_.getCrashMgr()->unregisterCallback(*crashCallbackKey_);
  }
  if (!flushedAndDisabled_) {
    // If the trace is not flushed and disabled, flush the trace and
    // run rollback action (e.g. delete the in-progress trace)
    flushAndDisableTrace();
    rollbackAction_();
  }
}

void TracingHermesRuntime::flushAndDisableTrace() {
  (void)flushAndDisableBridgeTrafficTrace();
}

std::string TracingHermesRuntime::flushAndDisableBridgeTrafficTrace() {
  if (flushedAndDisabled_) {
    return committedTraceFilename_;
  }
  trace().flushAndDisable(hermesRuntime().getGCExecTrace());
  flushedAndDisabled_ = true;
  committedTraceFilename_ = commitAction_();
  return committedTraceFilename_;
}

jsi::Value TracingHermesRuntime::evaluateJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  return TracingRuntime::evaluateJavaScript(buffer, sourceURL);
}

void TracingHermesRuntime::crashCallback(int fd) {
  if (flushedAndDisabled_) {
    // The trace is disabled prior to the crash.
    // As a result, this trace will likely not re-produce the crash.
    return;
  }
  llvh::raw_fd_ostream jsonStream(fd, false);
  ::hermes::JSONEmitter json(jsonStream);
  json.openDict();
  json.emitKeyValue("type", "tracing");
  std::string status = "Failed to flush";
  try {
    flushAndDisableBridgeTrafficTrace();
    status = "Completed";
  } catch (std::exception &ex) {
    ::hermes::hermesLog("Hermes", "Failed to flush trace: %s", ex.what());
    // suppress; we're in a crash handler
  } catch (...) {
    // suppress; we're in a crash handler
  }
  json.emitKeyValue("status", status);
  json.emitKeyValue("fileName", committedTraceFilename_);
  json.closeDict();
}

namespace {

void addRecordMarker(TracingRuntime &tracingRuntime) {
  const char *funcName = "__nativeRecordTraceMarker";
  const auto funcProp = jsi::PropNameID::forAscii(tracingRuntime, funcName);
  if (tracingRuntime.global().hasProperty(tracingRuntime, funcProp)) {
    // If this function is already defined, throw.
    throw jsi::JSINativeException(
        std::string("global.") + funcName +
        " already exists, won't overwrite it");
  }
  tracingRuntime.global().setProperty(
      tracingRuntime,
      funcProp,
      jsi::Function::createFromHostFunction(
          tracingRuntime,
          funcProp,
          0,
          [funcName, &tracingRuntime](
              jsi::Runtime &rt,
              const jsi::Value &,
              const jsi::Value *args,
              size_t argc) -> jsi::Value {
            if (argc < 1) {
              throw jsi::JSINativeException(
                  std::string(funcName) + " requires a single string argument");
            }
            tracingRuntime.addMarker(args[0].asString(rt).utf8(rt));
            return jsi::Value::undefined();
          }));
}

} // namespace

static std::unique_ptr<TracingHermesRuntime> makeTracingHermesRuntimeImpl(
    std::shared_ptr<HermesRuntime> hermesRuntime,
    const ::hermes::vm::RuntimeConfig &runtimeConfig,
    std::unique_ptr<llvh::raw_ostream> traceStream,
    std::function<std::string()> commitAction,
    std::function<void()> rollbackAction,
    bool forReplay) {
  ::hermes::hermesLog("Hermes", "Creating TracingHermesRuntime.");

  auto ret = std::make_unique<TracingHermesRuntime>(
      std::move(hermesRuntime),
      runtimeConfig,
      std::move(traceStream),
      commitAction,
      rollbackAction);
  if (!forReplay) {
    // In non-replay executions, add the __nativeRecordTraceMarker function.
    // In replay executions, this will be simulated from the trace.
    addRecordMarker(*ret);
    // In replay executions, the trace will contain the instructions for
    // replacing the nondeterministic functions.
    ret->replaceNondeterministicFuncs();
  }
  return ret;
}

std::unique_ptr<TracingHermesRuntime> makeTracingHermesRuntime(
    std::shared_ptr<HermesRuntime> hermesRuntime,
    const ::hermes::vm::RuntimeConfig &runtimeConfig,
    const std::string &traceScratchPath,
    const std::string &traceResultPath,
    std::function<bool()> traceCompletionCallback) {
  std::error_code ec;
  std::unique_ptr<llvh::raw_ostream> traceStream =
      std::make_unique<llvh::raw_fd_ostream>(
          traceScratchPath, ec, llvh::sys::fs::F_Text);
  if (ec) {
    ::hermes::hermesLog(
        "Hermes",
        "Failed to open file %s for tracing: %s",
        traceScratchPath.c_str(),
        ec.message().c_str());
    return makeTracingHermesRuntime(
        std::move(hermesRuntime), runtimeConfig, nullptr, true);
  }

  return makeTracingHermesRuntimeImpl(
      std::move(hermesRuntime),
      runtimeConfig,
      std::move(traceStream),
      // commitAction
      [traceCompletionCallback, traceScratchPath, traceResultPath]() {
        if (traceScratchPath != traceResultPath) {
          std::error_code ec =
              llvh::sys::fs::rename(traceScratchPath, traceResultPath);
          if (ec) {
            ::hermes::hermesLog(
                "Hermes",
                "Failed to rename tracing file from %s to %s: %s",
                traceScratchPath.c_str(),
                traceResultPath.c_str(),
                ec.message().c_str());
            return std::string();
          }
        }

        if (traceCompletionCallback) {
          bool success = traceCompletionCallback();
          if (!success) {
            ::hermes::hermesLog(
                "Hermes",
                "Failed to invoke completion callback for tracing file %s",
                traceResultPath.c_str());
            return std::string();
          }
        }
        ::hermes::hermesLog(
            "Hermes", "Completed tracing file at %s", traceResultPath.c_str());
        return traceResultPath;
      },
      // rollbackAction
      [traceScratchPath]() {
        // Delete the in-progress trace
        llvh::sys::fs::remove(traceScratchPath);
      },
      false);
}

std::unique_ptr<TracingHermesRuntime> makeTracingHermesRuntime(
    std::shared_ptr<HermesRuntime> hermesRuntime,
    const ::hermes::vm::RuntimeConfig &runtimeConfig,
    std::unique_ptr<llvh::raw_ostream> traceStream,
    bool forReplay) {
  return makeTracingHermesRuntimeImpl(
      std::move(hermesRuntime),
      runtimeConfig,
      std::move(traceStream),
      /* commitAction */ []() { return std::string(); },
      /* rollbackAction*/ []() {},
      forReplay);
}

} // namespace tracing
} // namespace hermes
} // namespace facebook
