/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/ConsoleHost/ConsoleHost.h"

#include "hermes/CompilerDriver/CompilerDriver.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/Support/OutputStream.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/NativeArgs.h"
#include "hermes/VM/Profiler/SamplingProfiler.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"
#include "hermes/VM/TimeLimitMonitor.h"
#include "hermes/VM/instrumentation/PerfEvents.h"
#include "hermes/hermes.h"
#ifdef HERMES_ENABLE_NAPI
#include "hermes/Support/SerialExecutor.h"
#include "hermes_napi_impl.h"
#endif

#include "jsi/jsi.h"

#include <queue>

namespace hermes {

ConsoleHostContext::ConsoleHostContext(vm::Runtime &runtime) {
  runtime.addCustomRootsFunction([this](vm::GC *, vm::RootAcceptor &acceptor) {
    for (auto &entry : taskQueue_) {
      acceptor.acceptPtr(entry.second);
    }
  });
}

/// Raises an uncatchable quit exception.
static vm::CallResult<vm::HermesValue> quit(void *, vm::Runtime &runtime) {
  return runtime.raiseQuitError();
}

#ifdef HERMES_ENABLE_NAPI
/// Trigger a full garbage collection. Exposed as gc_() for NAPI tests.
static vm::CallResult<vm::HermesValue> triggerGC(void *, vm::Runtime &runtime) {
  runtime.collect("gc_");
  return vm::HermesValue::encodeUndefinedValue();
}
#endif

static void printStats(
    vm::Runtime &runtime,
    llvh::raw_ostream &os,
    const std::vector<vm::GCAnalyticsEvent> *gcAnalyticsEvents) {
  std::string stats;
  {
    llvh::raw_string_ostream tmp{stats};
    runtime.printHeapStats(tmp);
  }
  vm::instrumentation::PerfEvents::endAndInsertStats(stats);

  if (gcAnalyticsEvents) {
    llvh::raw_string_ostream tmp{stats};
    tmp << "Collections:\n";
    ::hermes::JSONEmitter json{tmp, /*pretty*/ true};
    json.openArray();
    for (const auto &event : *gcAnalyticsEvents) {
      json.openDict();
      json.emitKeyValue("runtimeDescription", event.runtimeDescription);
      json.emitKeyValue("gcKind", event.gcKind);
      json.emitKeyValue("collectionType", event.collectionType);
      json.emitKeyValue("cause", event.cause);
      json.emitKeyValue("duration", event.duration.count());
      json.emitKeyValue("cpuDuration", event.cpuDuration.count());
      json.emitKeyValue("preAllocated", event.allocated.before);
      json.emitKeyValue("postAllocated", event.allocated.after);
      json.emitKeyValue("preSize", event.size.before);
      json.emitKeyValue("postSize", event.size.after);
      json.emitKeyValue("preExternal", event.external.before);
      json.emitKeyValue("postExternal", event.external.after);
      json.emitKeyValue("survivalRatio", event.survivalRatio);
      json.emitKey("tags");
      json.openArray();
      for (const auto &tag : event.tags) {
        json.emitValue(tag);
      }
      json.closeArray();
      json.closeDict();
    }
    json.closeArray();
    tmp << "\n";
  }

  os << stats;
}

static vm::CallResult<vm::HermesValue> createHeapSnapshot(
    void *,
    vm::Runtime &runtime) {
#ifdef HERMES_MEMORY_INSTRUMENTATION
  vm::NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  using namespace vm;
  std::string fileName;
  if (args.getArgCount() >= 1 && !args.getArg(0).isUndefined()) {
    if (!args.getArg(0).isString()) {
      return runtime.raiseTypeError("Filename argument must be a string");
    }
    auto str = Handle<StringPrimitive>::vmcast(args.getArgHandle(0));
    auto jsFileName = StringPrimitive::createStringView(runtime, str);
    llvh::SmallVector<char16_t, 16> buf;
    convertUTF16ToUTF8WithReplacements(fileName, jsFileName.getUTF16Ref(buf));
  }

  if (fileName.empty()) {
    // "-" is recognized as stdout.
    fileName = "-";
  } else if (
      !llvh::StringRef{fileName}.endswith(".heapsnapshot") &&
      !llvh::StringRef{fileName}.endswith(".heaptimeline")) {
    return runtime.raiseTypeError(
        "Filename must end in .heapsnapshot or .heaptimeline");
  }
  std::error_code err;
  llvh::raw_fd_ostream os(fileName, err, llvh::sys::fs::FileAccess::FA_Write);
  if (err) {
    // This isn't a TypeError, but no other built-in can express file errors,
    // so this will have to do.
    return runtime.raiseTypeError(
        TwineChar16("Could not write out to the file located at \"") +
        llvh::StringRef(fileName) +
        "\". System error: " + llvh::StringRef(err.message()));
  }
  // Taking a snapshot always starts with garbage collection.
  runtime.collect("snapshot");
  runtime.getHeap().createSnapshot(os, true);
  return HermesValue::encodeUndefinedValue();
#else // !defined(HERMES_MEMORY_INSTRUMENTATION)
  return runtime.raiseTypeError(
      "Heap snapshotting requires a build with memory instrumentation");
#endif // !defined(HERMES_MEMORY_INSTRUMENTATION)
}

static vm::CallResult<vm::HermesValue> loadSegment(
    void *ctx,
    vm::Runtime &runtime) {
  vm::NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  using namespace hermes::vm;
  const auto *baseFilename = reinterpret_cast<std::string *>(ctx);

  auto requireContext = args.dyncastArg<RequireContext>(0);
  if (!requireContext) {
    return runtime.raiseTypeError(
        "First argument to loadSegment must be context");
  }

  auto segmentRes = toUInt32_RJS(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(segmentRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint32_t segment = segmentRes->getNumberAs<uint32_t>();

  auto fileBufRes =
      llvh::MemoryBuffer::getFile(Twine(*baseFilename) + "." + Twine(segment));
  if (!fileBufRes) {
    return runtime.raiseTypeError(
        TwineChar16("Failed to open segment: ") + segment);
  }

  auto ret = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
      std::make_unique<OwnedMemoryBuffer>(std::move(*fileBufRes)));
  if (!ret.first) {
    return runtime.raiseTypeError("Error deserializing bytecode");
  }

  if (LLVM_UNLIKELY(
          runtime.loadSegment(std::move(ret.first), requireContext) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return HermesValue::encodeUndefinedValue();
}

#ifdef HERMES_ENABLE_NAPI
/// Host integration for envs created by loadNativeModule. Set by
/// executeHBCBytecodeImpl via CurrentNapiHostScope for the duration of
/// script execution; nullptr otherwise (in which case async-work and
/// tsfn APIs return napi_generic_failure).
static hermes_napi_host *currentNapiHost = nullptr;

struct CurrentNapiHostScope {
  explicit CurrentNapiHostScope(hermes_napi_host *host) {
    assert(currentNapiHost == nullptr && "nested CurrentNapiHostScope");
    currentNapiHost = host;
  }
  ~CurrentNapiHostScope() {
    currentNapiHost = nullptr;
  }
  CurrentNapiHostScope(const CurrentNapiHostScope &) = delete;
  CurrentNapiHostScope &operator=(const CurrentNapiHostScope &) = delete;
};

/// Load a NAPI native module from a shared library.
/// Usage: var exports = loadNativeModule("/path/to/addon.node");
static vm::CallResult<vm::HermesValue> loadNativeModule(
    void *,
    vm::Runtime &runtime) {
  vm::NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  using namespace hermes::vm;

  if (args.getArgCount() < 1 || !args.getArg(0).isString()) {
    return runtime.raiseTypeError(
        "loadNativeModule requires a string argument");
  }

  // Extract the path string from the argument.
  auto str = Handle<StringPrimitive>::vmcast(args.getArgHandle(0));
  auto jsPath = StringPrimitive::createStringView(runtime, str);
  std::string path;
  llvh::SmallVector<char16_t, 64> buf;
  convertUTF16ToUTF8WithReplacements(path, jsPath.getUTF16Ref(buf));

  // Create a napi_env for loading the module. The env must outlive the
  // loaded module because NativeFunctions hold pointers to
  // CallbackBundles owned by the env.
  //
  // The host pointer (currentNapiHost) is set by executeHBCBytecodeImpl
  // for the duration of script execution and provides the worker thread
  // + event-loop integration needed by napi_create_async_work and
  // threadsafe-function APIs. When null (no script context), async-work
  // and tsfn APIs return napi_generic_failure.
  napi_env env = hermes_napi_create_env(&runtime, currentNapiHost);

  // Open a handle scope for the module init call.
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value napiResult;
  napi_status status = hermes_napi_load_module(env, path.c_str(), &napiResult);

  if (status != napi_ok) {
    // The load failed — capture the pending exception from the env
    // and re-throw it on the runtime.
    HermesValue exceptionVal = HermesValue::encodeUndefinedValue();
    if (env->hasPendingException) {
      std::memcpy(&exceptionVal, &env->pendingException, sizeof(HermesValue));
      env->pendingException = HermesValue::encodeUndefinedValue();
      env->hasPendingException = false;
    }

    napi_close_handle_scope(env, scope);
    // env is owned by the Runtime; it stays alive for the Runtime's
    // lifetime so the module's NativeFunctions remain valid.

    runtime.setThrownValue(exceptionVal);
    return ExecutionStatus::EXCEPTION;
  }

  // Copy the result HermesValue before closing the handle scope.
  HermesValue resultVal;
  auto *phv = reinterpret_cast<PinnedHermesValue *>(napiResult);
  std::memcpy(&resultVal, phv, sizeof(HermesValue));

  napi_close_handle_scope(env, scope);
  // env is owned by the Runtime; it stays alive for the Runtime's
  // lifetime so the module's NativeFunctions remain valid.

  return resultVal;
}
#endif // HERMES_ENABLE_NAPI

static vm::CallResult<vm::HermesValue> setTimeout(
    void *ctx,
    vm::Runtime &runtime) {
  vm::NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  ConsoleHostContext *consoleHost = (ConsoleHostContext *)ctx;
  using namespace hermes::vm;
  Handle<Callable> callable = args.dyncastArg<Callable>(0);
  if (!callable) {
    return runtime.raiseTypeError("Argument to setTimeout must be a function");
  }
  CallResult<HermesValue> boundFunction = BoundFunction::create(
      runtime, callable, args.getArgCount() - 1, args.begin() + 1);
  if (boundFunction == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  uint32_t taskId = consoleHost->queueTask(
      PseudoHandle<Callable>::vmcast(createPseudoHandle(*boundFunction)));
  return HermesValue::encodeTrustedNumberValue(taskId);
}

static vm::CallResult<vm::HermesValue> clearTimeout(
    void *ctx,
    vm::Runtime &runtime) {
  vm::NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  ConsoleHostContext *consoleHost = (ConsoleHostContext *)ctx;
  using namespace hermes::vm;
  if (!args.getArg(0).isNumber()) {
    return runtime.raiseTypeError("Argument to clearTimeout must be a number");
  }
  consoleHost->clearTask(args.getArg(0).getNumberAs<uint32_t>());
  return HermesValue::encodeUndefinedValue();
}

/// Synchronously read a file and return its contents as a Uint8Array.
static vm::CallResult<vm::HermesValue> hermescliLoadFile(
    void *,
    vm::Runtime &runtime) {
  vm::NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  using namespace vm;

  if (args.getArgCount() < 1 || !args.getArg(0).isString()) {
    return runtime.raiseTypeError("loadFile requires a string path argument");
  }

  auto str = Handle<StringPrimitive>::vmcast(args.getArgHandle(0));
  auto jsPath = StringPrimitive::createStringView(runtime, str);
  std::string path;
  llvh::SmallVector<char16_t, 64> buf;
  convertUTF16ToUTF8WithReplacements(path, jsPath.getUTF16Ref(buf));

  auto fileBufRes = llvh::MemoryBuffer::getFile(path);
  if (!fileBufRes) {
    return runtime.raiseTypeError(
        TwineChar16("Failed to open file: ") + llvh::StringRef(path));
  }

  size_t len = (*fileBufRes)->getBufferSize();
  auto result = Uint8Array::allocate(runtime, len);
  if (result == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto ta = std::move(*result);
  std::memcpy(ta->data(runtime), (*fileBufRes)->getBufferStart(), len);

  return HermesValue::encodeObjectValue(ta.get());
}

/// Load and execute HBC bytecode from a Uint8Array or ArrayBuffer.
/// Returns the result of the top-level function.
static vm::CallResult<vm::HermesValue> hermescliLoadHBC(
    void *,
    vm::Runtime &runtime) {
  vm::NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  using namespace vm;

  uint8_t *data = nullptr;
  size_t len = 0;

  if (auto *ta = dyn_vmcast<JSTypedArrayBase>(args.getArg(0))) {
    data = ta->data(runtime);
    len = ta->getByteLength();
  } else if (auto *ab = dyn_vmcast<JSArrayBuffer>(args.getArg(0))) {
    data = ab->getDataBlock();
    len = ab->size();
  } else {
    return runtime.raiseTypeError(
        "loadHBC requires a Uint8Array or ArrayBuffer argument");
  }

  // Copy the data into an OwnedMemoryBuffer.
  auto ownedBuf =
      std::make_unique<OwnedMemoryBuffer>(llvh::MemoryBuffer::getMemBufferCopy(
          llvh::StringRef(reinterpret_cast<const char *>(data), len)));

  auto ret = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
      std::move(ownedBuf));
  if (!ret.first) {
    return runtime.raiseTypeError(
        TwineChar16("Error deserializing bytecode: ") +
        llvh::StringRef(ret.second));
  }

  struct : public Locals {
    PinnedValue<Domain> domain;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.domain = Domain::create(runtime);

  RuntimeModuleFlags flags;
  flags.persistent = true;

  return runtime.runBytecode(
      std::move(ret.first),
      flags,
      /*sourceURL*/ "",
      Runtime::makeNullHandle<Environment>(),
      lv.domain);
}

/// Return an array of extra CLI arguments passed after the script filename.
static vm::CallResult<vm::HermesValue> hermescliGetScriptArgs(
    void *ctx,
    vm::Runtime &runtime) {
  using namespace vm;
  auto *consoleHost = reinterpret_cast<ConsoleHostContext *>(ctx);
  const auto &scriptArgs = consoleHost->scriptArgs_;
  auto len = static_cast<JSArray::size_type>(scriptArgs.size());

  auto arrayRes = JSArray::create(runtime, len, len);
  if (arrayRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<JSArray> arr;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.arr = std::move(*arrayRes);

  if (lv.arr->setStorageEndIndex(lv.arr, runtime, len) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  for (JSArray::size_type i = 0; i < len; ++i) {
    GCScopeMarkerRAII marker{runtime};
    auto strRes = StringPrimitive::createEfficient(
        runtime, llvh::createASCIIRef(scriptArgs[i].c_str()));
    if (strRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    auto shv = SmallHermesValue::encodeStringValue(
        vmcast<StringPrimitive>(*strRes), runtime);
    JSArray::unsafeSetExistingElementAt(*lv.arr, runtime, i, shv);
  }

  return HermesValue::encodeObjectValue(*lv.arr);
}

/// The test262 testsuite requires below harness symbols:
/// - $262, with properties:
///     - global, alias to globalThis
///     - evalScript, alias to eval
///     - detachArrayBuffer, alias to HermesInternal.detachArrayBuffer (if it
///       exists)
/// - alert, alias to print
static void initTest262Harness(vm::Runtime &runtime) {
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSObject> test262Obj;
    vm::PinnedValue<> prop;
    vm::PinnedValue<> hermesInternal;
  } lv;
  vm::LocalsRAII lraii{runtime, &lv};
  lv.test262Obj = vm::JSObject::create(runtime);

  vm::DefinePropertyFlags nonEnumerableDPF =
      vm::DefinePropertyFlags::getNewNonEnumerableFlags();

  // Define $262.global
  auto global = runtime.getGlobal();
  runtime.ignoreAllocationFailure(
      vm::JSObject::defineOwnProperty(
          lv.test262Obj,
          runtime,
          vm::Predefined::getSymbolID(vm::Predefined::global),
          nonEnumerableDPF,
          global));

  /// Try to get defined property on given JSObject \p selfHandle into \p out.
  /// If it does not exist, return false.
  auto tryGetDefinedProperty = [&runtime](
                                   vm::Handle<vm::JSObject> selfHandle,
                                   vm::SymbolID name,
                                   vm::PinnedValue<> &out) -> bool {
    auto propRes = vm::JSObject::getNamed_RJS(selfHandle, runtime, name);
    if (propRes == vm::ExecutionStatus::EXCEPTION) {
      struct : public vm::Locals {
        vm::PinnedValue<> thrown;
      } lv;
      vm::LocalsRAII lraii{runtime, &lv};
      lv.thrown = runtime.getThrownValue();
      runtime.printException(llvh::outs(), lv.thrown);
      return false;
    }

    // It may not really exist, e.g., HermesInternal.detachArrayBuffer.
    if (propRes.getValue()->isUndefined()) {
      return false;
    }

    out = std::move(*propRes);
    return true;
  };

  /// Try to copy the defined property \p srcName on \p srcSelfHandle to
  /// \p tgtSelfHandle with given name \p tgtName. If \p srcName does not exist,
  /// do nothing.
  auto tryCopyProperty =
      [&runtime, &nonEnumerableDPF, &tryGetDefinedProperty, &lv](
          vm::Handle<vm::JSObject> srcSelfHandle,
          vm::SymbolID srcName,
          vm::Handle<vm::JSObject> tgtSelfHandle,
          vm::SymbolID tgtName) {
        if (!tryGetDefinedProperty(srcSelfHandle, srcName, lv.prop))
          return;

        runtime.ignoreAllocationFailure(
            vm::JSObject::defineOwnProperty(
                tgtSelfHandle, runtime, tgtName, nonEnumerableDPF, lv.prop));
      };

  // Define $262.evalScript
  tryCopyProperty(
      global,
      vm::Predefined::getSymbolID(vm::Predefined::eval),
      lv.test262Obj,
      vm::Predefined::getSymbolID(vm::Predefined::evalScript));

  // Define $262.detachArrayBuffer
  if (tryGetDefinedProperty(
          global,
          vm::Predefined::getSymbolID(vm::Predefined::HermesInternal),
          lv.hermesInternal)) {
    tryCopyProperty(
        vm::Handle<vm::JSObject>::vmcast(&lv.hermesInternal),
        vm::Predefined::getSymbolID(vm::Predefined::detachArrayBuffer),
        lv.test262Obj,
        vm::Predefined::getSymbolID(vm::Predefined::detachArrayBuffer));
  }

  // Define global object $262
  runtime.ignoreAllocationFailure(
      vm::JSObject::defineOwnProperty(
          global,
          runtime,
          vm::Predefined::getSymbolID(vm::Predefined::test262),
          nonEnumerableDPF,
          lv.test262Obj));

  // Define global function alert()
  tryCopyProperty(
      global,
      vm::Predefined::getSymbolID(vm::Predefined::print),
      global,
      vm::Predefined::getSymbolID(vm::Predefined::alert));
}

void installConsoleBindings(
    vm::Runtime &runtime,
    ConsoleHostContext &ctx,
    vm::StatSamplingThread *statSampler,
    const std::string *filename) {
  vm::GCScopeMarkerRAII marker{runtime};
  vm::DefinePropertyFlags normalDPF =
      vm::DefinePropertyFlags::getNewNonEnumerableFlags();

  struct : public vm::Locals {
    vm::PinnedValue<vm::JSObject> console;
    vm::PinnedValue<> print;
    vm::PinnedValue<vm::JSObject> hermescliObj;
    vm::PinnedValue<vm::SymbolID> sym;
    vm::PinnedValue<vm::NativeFunction> func;
  } lv;
  vm::LocalsRAII lraii{runtime, &lv};

  // Intern \p name and pin the SymbolID in lv.sym, which keeps it alive until
  // the next call.
  auto internSym = [&](const char *name) -> vm::SymbolID {
    lv.sym = runtime.ignoreAllocationFailure(
        runtime.getIdentifierTable().getSymbolID(
            runtime, llvh::createASCIIRef(name)));
    return *lv.sym;
  };

  auto defineGlobalFunc = [&](vm::SymbolID name,
                              vm::NativeFunctionPtr functionPtr,
                              void *context,
                              unsigned paramCount) -> void {
    vm::GCScopeMarkerRAII marker{runtime};
    lv.func = vm::NativeFunction::create(
        runtime, context, functionPtr, name, paramCount);
    auto res = vm::JSObject::defineOwnProperty(
        runtime.getGlobal(), runtime, name, normalDPF, lv.func);
    (void)res;
    assert(
        res != vm::ExecutionStatus::EXCEPTION && *res &&
        "global.defineOwnProperty() failed");
  };

  // Define the 'quit' function.
  defineGlobalFunc(
      vm::Predefined::getSymbolID(vm::Predefined::quit), quit, nullptr, 0);

#ifdef HERMES_ENABLE_NAPI
  // Define 'gc_()' — triggers a full garbage collection.
  // Used by NAPI tests (common.js gcUntil helper).
  defineGlobalFunc(internSym("gc_"), triggerGC, nullptr, 0);
#endif
  defineGlobalFunc(
      vm::Predefined::getSymbolID(vm::Predefined::createHeapSnapshot),
      createHeapSnapshot,
      nullptr,
      1);

  // Define the 'loadSegment' function.
  defineGlobalFunc(
      internSym("loadSegment"),
      loadSegment,
      reinterpret_cast<void *>(const_cast<std::string *>(filename)),
      2);

#ifdef HERMES_ENABLE_NAPI
  // Define the 'loadNativeModule' function for loading NAPI addons.
  defineGlobalFunc(internSym("loadNativeModule"), loadNativeModule, nullptr, 1);
#endif

  defineGlobalFunc(internSym("setTimeout"), setTimeout, &ctx, 2);
  defineGlobalFunc(internSym("clearTimeout"), clearTimeout, &ctx, 1);

  // Define `setImmediate` to be the same as `setTimeout` here.
  // `setTimeout` doesn't use the time provided to it, and due to this
  // being CLI code, we don't have an event loop.
  // This allows the Promise polyfill to work enough for testing in the
  // terminal, though other hosts should provide their own implementation of the
  // event loop.
  defineGlobalFunc(internSym("setImmediate"), setTimeout, &ctx, 1);

  lv.console = vm::JSObject::create(runtime);
  runtime.ignoreAllocationFailure(
      vm::JSObject::defineOwnProperty(
          runtime.getGlobal(),
          runtime,
          internSym("console"),
          normalDPF,
          lv.console));
  lv.print = runtime.ignoreAllocationFailure(
      vm::JSObject::getNamed_RJS(
          runtime.getGlobal(),
          runtime,
          vm::Predefined::getSymbolID(vm::Predefined::print)));
  runtime.ignoreAllocationFailure(
      vm::JSObject::defineOwnProperty(
          lv.console,
          runtime,
          vm::Predefined::getSymbolID(vm::Predefined::log),
          normalDPF,
          lv.print));

  initTest262Harness(runtime);

  // Register hermescli object with test utility functions, gated on the
  // -Xhermes-internal-test-methods flag.
  if (ctx.enableTestMethods_) {
    lv.hermescliObj = vm::JSObject::create(runtime);

    auto defineMethod = [&](const char *name,
                            vm::NativeFunctionPtr functionPtr,
                            void *context,
                            unsigned paramCount) {
      vm::GCScopeMarkerRAII marker{runtime};
      auto sym = internSym(name);
      lv.func = vm::NativeFunction::create(
          runtime, context, functionPtr, sym, paramCount);
      auto res = vm::JSObject::defineOwnProperty(
          lv.hermescliObj, runtime, sym, normalDPF, lv.func);
      (void)res;
      assert(
          res != vm::ExecutionStatus::EXCEPTION && *res &&
          "hermescli.defineOwnProperty() failed");
    };

    defineMethod("loadFile", hermescliLoadFile, nullptr, 1);
    defineMethod("loadHBC", hermescliLoadHBC, nullptr, 1);
    defineMethod("getScriptArgs", hermescliGetScriptArgs, &ctx, 0);

    runtime.ignoreAllocationFailure(
        vm::JSObject::defineOwnProperty(
            runtime.getGlobal(),
            runtime,
            internSym("hermescli"),
            normalDPF,
            lv.hermescliObj));
  }
}

// If a function body might throw C++ exceptions other than
// jsi::JSError from Hermes, it should be wrapped in this form:
//
//   return maybeCatchException([&] { body })
//
// This will execute body; if exceptions are enabled, this execution
// will be wrapped in a try/catch that catches those exceptions, report it then
// exit.
namespace {

template <typename F>
auto maybeCatchException(const F &f) -> decltype(f()) {
#if defined(HERMESVM_EXCEPTION_ON_OOM)
  try {
    return f();
  } catch (const std::exception &ex) {
    // Report thrown exception and exit the process with failure code.
    llvh::errs() << ex.what();
    exit(1);
  }
#else // HERMESVM_EXCEPTION_ON_OOM
  return f();
#endif
}

// Simple event-loop queue: tasks scheduled via `scheduleTask` are drained
// by `drainTasks` on the JS thread. `registerTaskQueueSource` /
// `unregisterTaskQueueSource` track external sources (workers, async work)
// that may post tasks; `drainTasks` does not exit until the queue, the
// source set, and the anonymous source counter are all empty. All
// methods are thread-safe.
class EventLoopControl final : public facebook::hermes::IEventLoopControl {
 public:
  ~EventLoopControl() = default;

  void scheduleTask(const std::function<void()> &callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push(callback);
    cv_.notify_all();
  }

  uint64_t registerTaskQueueSource() override {
    std::lock_guard<std::mutex> lock(mutex_);
    sourceCount_++;
    sources_.insert(sourceCount_);
    return sourceCount_;
  }

  void unregisterTaskQueueSource(uint64_t sourceId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.erase(sourceId);
    if (sources_.empty() && anonSources_ == 0) {
      cv_.notify_all();
    }
  }

  /// Add an anonymous (unidentified) loop reference. Used by clients
  /// that don't care about a returned id — they just want to hold the
  /// loop alive. Each call must be paired with a matching
  /// removeAnonSource. Thread-safe.
  void addAnonSource() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++anonSources_;
  }

  void removeAnonSource() {
    std::lock_guard<std::mutex> lock(mutex_);
    assert(anonSources_ > 0 && "anon source underflow");
    --anonSources_;
    if (sources_.empty() && anonSources_ == 0) {
      cv_.notify_all();
    }
  }

  /// Drain all tasks from the queue, blocking until there are no more tasks
  /// and no more active sources that could schedule new tasks.
  void drainTasks() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      if (!tasks_.empty()) {
        // There are tasks to run. Grab the next one and run it.
        auto task = tasks_.front();
        tasks_.pop();
        lock.unlock();
        // Run the task outside the lock. Otherwise, the running task may
        // also try to queue more tasks, causing a deadlock.
        task();
        lock.lock();
      } else if (!sources_.empty() || anonSources_ > 0) {
        // No tasks to run, but there are still active sources that may
        // schedule tasks. Sleep until woken by `scheduleTask`,
        // `unregisterTaskQueueSource`, or `removeAnonSource`.
        cv_.wait(lock);
      } else {
        // No tasks and no sources at this point, exit the event loop.
        break;
      }
    }
  }

 private:
  // Callbacks may be added from any thread and removed on the JS thread.
  std::mutex mutex_{};
  // Used to wake up the event loop if it is waiting for a new task or if
  // all task sources have disappeared.
  std::condition_variable cv_;
  // All scheduled tasks.
  std::queue<std::function<void()>> tasks_{};
  // Counter of total number of sources ever registered (monotonic).
  uint64_t sourceCount_{0};
  // The set of active sources (from the id-based register API).
  llvh::DenseSet<uint64_t> sources_{};
  // Count of active anonymous sources (from the addAnonSource API).
  int anonSources_{0};
};

#if defined(HERMES_ENABLE_NAPI) && defined(JSI_UNSTABLE)
// Bridges a hermes_napi_host onto EventLoopControl plus a worker thread,
// so NAPI async work and threadsafe-function dispatch can be wired into
// ConsoleHost's event loop. The post_work / post_task trampolines
// register a task queue source for the duration of each in-flight item,
// so `EventLoopControl::drainTasks` does not return until every queued
// item has completed.
//
// cancel_work always returns false (SerialExecutor exposes no per-task
// cancel; cancelation is reported as napi_generic_failure to the addon).
class NapiHostAdapter final {
 public:
  explicit NapiHostAdapter(EventLoopControl *jsLoop) : jsLoop_(jsLoop) {
    host.data = this;
    host.post_work = &postWorkTrampoline;
    host.cancel_work = &cancelWorkTrampoline;
    host.post_task = &postTaskTrampoline;
    host.uv_loop = nullptr;
    host.fatal_exception = nullptr;
    host.ref_loop = &refLoopTrampoline;
    host.unref_loop = &unrefLoopTrampoline;
  }

  // The struct passed to hermes_napi_create_env. Field destructor is
  // trivial; lifetime of this struct is tied to the adapter.
  hermes_napi_host host{};

 private:
  EventLoopControl *jsLoop_;
  // Single worker thread used for napi_create_async_work `execute`
  // callbacks. NAPI does not promise parallel execution, so one thread
  // is semantically sufficient.
  ::hermes::SerialExecutor worker_{};

  static void postWorkTrampoline(
      void *loop_data,
      void *work_data,
      void (*execute)(void *work_data),
      void (*complete)(void *work_data, napi_status status)) {
    auto *self = static_cast<NapiHostAdapter *>(loop_data);
    // Keep the JS event loop alive until `complete` finishes — without
    // this, drainTasks could exit before the worker has a chance to
    // schedule the completion callback.
    uint64_t srcId = self->jsLoop_->registerTaskQueueSource();
    self->worker_.add([self, work_data, execute, complete, srcId]() {
      execute(work_data);
      self->jsLoop_->scheduleTask([self, work_data, complete, srcId]() {
        complete(work_data, napi_ok);
        self->jsLoop_->unregisterTaskQueueSource(srcId);
      });
    });
  }

  static bool cancelWorkTrampoline(void * /*loop_data*/, void * /*work*/) {
    // SerialExecutor has no per-task cancel. Report failure; the NAPI
    // layer surfaces napi_generic_failure to the addon. Acceptable per
    // the hermes_napi_host contract (hermes_napi.h cancel_work doc).
    return false;
  }

  static void postTaskTrampoline(
      void *loop_data,
      void *task_data,
      void (*callback)(void *task_data)) {
    auto *self = static_cast<NapiHostAdapter *>(loop_data);
    uint64_t srcId = self->jsLoop_->registerTaskQueueSource();
    self->jsLoop_->scheduleTask([self, task_data, callback, srcId]() {
      callback(task_data);
      self->jsLoop_->unregisterTaskQueueSource(srcId);
    });
  }

  // ref_loop / unref_loop forward to EventLoopControl's anonymous
  // source counter. Per the hermes_napi_host contract these are paired
  // and non-nested within a single env; with multiple envs sharing this
  // adapter, several refs may be in flight concurrently — the counter
  // handles that naturally with no per-call state in the adapter.
  static void refLoopTrampoline(void *loop_data) {
    static_cast<NapiHostAdapter *>(loop_data)->jsLoop_->addAnonSource();
  }

  static void unrefLoopTrampoline(void *loop_data) {
    static_cast<NapiHostAdapter *>(loop_data)->jsLoop_->removeAnonSource();
  }
};
#endif

bool executeHBCBytecodeImpl(
    std::shared_ptr<hbc::BCProvider> &&bytecode,
    const ExecuteOptions &options,
    const std::string *filename) {
  bool shouldRecordGCStats =
      options.runtimeConfig.getGCConfig().getShouldRecordStats();
  if (shouldRecordGCStats) {
    vm::instrumentation::PerfEvents::begin();
  }

  std::unique_ptr<vm::StatSamplingThread> statSampler;

  // Declared before hermesRuntime so it outlives the runtime. The runtime's
  // finalizerExecutor_ may call into the EventLoopControl during destruction.
  EventLoopControl eventLoopControl{};

#if defined(HERMES_ENABLE_NAPI) && defined(JSI_UNSTABLE)
  // NAPI host adapter — must outlive any napi_env created via
  // loadNativeModule. Declared after eventLoopControl so the adapter's
  // worker thread is joined first (during the adapter's destructor),
  // before eventLoopControl tears down. NAPI envs are owned by the
  // Runtime and torn down by ~Runtime before the adapter goes out of
  // scope, so adapter teardown does not race with env teardown.
  NapiHostAdapter napiHostAdapter{&eventLoopControl};
  CurrentNapiHostScope napiHostScope{&napiHostAdapter.host};
#endif

  // Create HermesRuntime (JSI wrapper) - this installs JSI extensions like
  // TextEncoder. We then extract the underlying vm::Runtime for low-level
  // operations.
  auto hermesRuntime =
      facebook::hermes::makeHermesRuntime(options.runtimeConfig);
  auto *runtime = static_cast<vm::Runtime *>(
      facebook::jsi::castInterface<facebook::hermes::IHermes>(
          hermesRuntime.get())
          ->getVMRuntimeUnsafe());

  // Other JIT settings (forceJIT, jitThreshold, jitMemoryLimit) are now
  // configured via RuntimeConfig in the Runtime constructor.
  runtime->getJITContext().setDumpJITCode(options.dumpJITCode);
  runtime->getJITContext().setCrashOnError(options.jitCrashOnError);
  runtime->getJITContext().setEmitAsserts(options.jitEmitAsserts);
  runtime->getJITContext().setEmitTypeAsserts(options.jitEmitTypeAsserts);
  runtime->getJITContext().setEmitCounters(options.jitEmitCounters);
  runtime->getJITContext().setHCIdLimit(options.jitHCIdLimit);

  if (options.perfProfJitDumpFd != -1) {
    runtime->getJITContext().initPerfProfData(
        options.perfProfJitDumpFd,
        options.perfProfDebugInfoFd,
        options.perfProfDebugInfoFile);
  }

  if (options.timeLimit > 0) {
    runtime->timeLimitMonitor = vm::TimeLimitMonitor::getOrCreate();
    runtime->timeLimitMonitor->watchRuntime(
        *runtime, std::chrono::milliseconds(options.timeLimit));
  }

  if (shouldRecordGCStats) {
    statSampler = std::make_unique<vm::StatSamplingThread>(
        std::chrono::milliseconds(100));
  }

  if (options.heapTimeline) {
#ifdef HERMES_MEMORY_INSTRUMENTATION
    runtime->enableAllocationLocationTracker();
#else
    llvh::errs() << "Failed to track allocation locations; build does not"
                    "include memory instrumentation\n";
#endif
  }

  auto *setEventLoopInterface =
      facebook::jsi::castInterface<facebook::hermes::ISetEventLoopControl>(
          hermesRuntime.get());
  setEventLoopInterface->setEventLoopControl(&eventLoopControl);

  vm::GCScope scope(*runtime);
  struct : public vm::Locals {
    vm::PinnedValue<vm::Domain> domain;
    vm::PinnedValue<vm::Callable> task;
  } lv;
  vm::LocalsRAII lraii{*runtime, &lv};
  ConsoleHostContext ctx{*runtime};
  ctx.enableTestMethods_ =
      options.runtimeConfig.getEnableHermesInternalTestMethods();
  ctx.scriptArgs_ = options.scriptArgs;

  installConsoleBindings(*runtime, ctx, statSampler.get(), filename);

  vm::RuntimeModuleFlags flags;
  if (auto *bcProviderFromSrc =
          llvh::dyn_cast<hbc::BCProviderFromSrc>(bytecode.get())) {
    flags.persistent = bcProviderFromSrc->allowPersistent();
  } else {
    flags.persistent = true;
  }

  if (options.stopAfterInit) {
    lv.domain = vm::Domain::create(*runtime);
    if (LLVM_UNLIKELY(
            vm::RuntimeModule::create(
                *runtime,
                lv.domain,
                facebook::hermes::debugger::kInvalidLocation,
                std::move(bytecode),
                flags) == vm::ExecutionStatus::EXCEPTION)) {
      llvh::errs() << "Failed to initialize main RuntimeModule\n";
      return false;
    }

    return true;
  }

#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  if (options.sampleProfiling != ExecuteOptions::SampleProfilingMode::None) {
    if (options.profilingOutFile.empty()) {
      llvh::errs()
          << "Please specify a profiling output file with -profiling-out\n";
      return false;
    }
    vm::SamplingProfiler::enable(options.sampleProfilingFreq);
  }
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE

  llvh::StringRef sourceURL{};
  if (filename)
    sourceURL = *filename;
  vm::CallResult<vm::HermesValue> status = runtime->runBytecode(
      std::move(bytecode),
      flags,
      sourceURL,
      vm::Runtime::makeNullHandle<vm::Environment>());

  bool threwException = status == vm::ExecutionStatus::EXCEPTION;

  if (threwException) {
    // Make sure stdout catches up to stderr.
    llvh::outs().flush();
    microtask::printThrownValue(*runtime);
  }

  // Perform a microtask checkpoint after running script.
  microtask::performCheckpoint(*runtime);

  if (!ctx.tasksEmpty()) {
    vm::GCScopeMarkerRAII marker{scope};
    // Run the tasks until there are no more.
    while (auto optTask = ctx.dequeueTask()) {
      lv.task = std::move(*optTask);
      auto callRes = vm::Callable::executeCall0(
          lv.task, *runtime, vm::Runtime::getUndefinedValue(), false);
      if (LLVM_UNLIKELY(callRes == vm::ExecutionStatus::EXCEPTION)) {
        threwException = true;
        llvh::outs().flush();
        microtask::printThrownValue(*runtime);
        break;
      }

      // Perform a microtask checkpoint at the end of every task tick.
      microtask::performCheckpoint(*runtime);
    }
  }

  // Run all tasks queued in the event loop. Drains JSI-scheduled tasks
  // and any pending NAPI async-work completions / tsfn dispatches.
  eventLoopControl.drainTasks();

#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  if (options.sampleProfiling != ExecuteOptions::SampleProfilingMode::None) {
    assert(!options.profilingOutFile.empty() && "Must not be empty");
    OutputStream fileOS;
    if (!fileOS.open(options.profilingOutFile, llvh::sys::fs::F_Text))
      return false;
    switch (options.sampleProfiling) {
      case ExecuteOptions::SampleProfilingMode::None:
        llvm_unreachable("Cannot be none");
      case ExecuteOptions::SampleProfilingMode::Chrome:
        vm::SamplingProfiler::disable();
        runtime->samplingProfiler->dumpChromeTrace(fileOS.os());
        break;
      case ExecuteOptions::SampleProfilingMode::Tracery:
        vm::SamplingProfiler::disable();
        runtime->samplingProfiler->dumpTraceryTrace(fileOS.os());
        break;
    }
    if (!fileOS.close())
      return false;
  }
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE

#ifdef HERMESVM_PROFILER_OPCODE
  runtime->dumpOpcodeStats(llvh::outs());
#endif

#ifdef HERMESVM_PROFILER_JSFUNCTION
  runtime->dumpJSFunctionStats();
#endif

#ifdef HERMESVM_PROFILER_NATIVECALL
  runtime->dumpNativeCallStats(llvh::outs());
#endif

  if (shouldRecordGCStats) {
    llvh::errs() << "Process stats:\n";
    statSampler->stop().printJSON(llvh::errs());

    if (options.forceGCBeforeStats) {
      runtime->collect("forced for stats");
    }
    printStats(*runtime, llvh::errs(), options.gcAnalyticsEvents);
  }

  if (options.runtimeConfig.getTrackIO()) {
    runtime->getIOTrackingInfoJSON(llvh::errs());
  }

  if (options.jitEmitCounters) {
    llvh::errs() << "JIT counters:\n";
    runtime->getJITContext().dumpCounters(llvh::errs());
  }

#ifdef HERMESVM_PROFILER_BB
  if (options.basicBlockProfiling) {
    OutputStream profilingFileOS(llvh::errs());
    if (!options.profilingOutFile.empty()) {
      if (!profilingFileOS.open(
              options.profilingOutFile, llvh::sys::fs::F_Text)) {
        llvh::errs() << "Failed to open file '" << options.profilingOutFile
                     << "'; writing to stderr instead.\n";
      }
    }
    runtime->getBasicBlockExecutionInfo().dump(profilingFileOS.os());
    if (!profilingFileOS.close()) {
      llvh::errs() << "Failed to close profiling file '"
                   << options.profilingOutFile << "'.\n";
    }
  }
#endif

  // NAPI envs are owned by the Runtime and torn down as part of
  // ~Runtime — no explicit cleanup is required here.

  return !threwException;
}

} // namespace

/// Executes the HBC bytecode provided in HermesVM.
/// \return true on success, false on error.
bool executeHBCBytecode(
    std::shared_ptr<hbc::BCProvider> &&bytecode,
    const ExecuteOptions &options,
    const std::string *filename) {
  return maybeCatchException([&] {
    return executeHBCBytecodeImpl(std::move(bytecode), options, filename);
  });
}

} // namespace hermes
