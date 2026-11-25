/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"

#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/Parser/JSParser.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/Support/ScopeChain.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"
#include "llvh/Support/ConvertUTF.h"
#include "llvh/Support/raw_ostream.h"

namespace hermes {
namespace vm {

CallResult<HermesValue> evalInEnvironment(
    Runtime &runtime,
    llvh::StringRef utf8code,
    bool strictCaller,
    Handle<Environment> environment,
    const CodeBlock *codeBlock,
    Handle<> thisArg,
    Handle<> newTarget,
    bool singleFunction,
    OptValue<uint32_t> lexicalScopeIdxInParentFunction) {
  if (!runtime.enableEval) {
    return runtime.raiseEvalUnsupported(utf8code);
  }

  hbc::CompileFlags compileFlags;
  compileFlags.strict = strictCaller;
  compileFlags.includeLibHermes = false;
  compileFlags.verifyIR = runtime.verifyEvalIR;
  compileFlags.emitAsyncBreakCheck = runtime.asyncBreakCheckInEval;
  compileFlags.lazy =
      utf8code.size() >= compileFlags.preemptiveFileCompilationThreshold;
  compileFlags.enableES6BlockScoping = runtime.hasES6BlockScoping();
  compileFlags.enableAsyncGenerators = runtime.hasAsyncGenerators();
  compileFlags.test262 = runtime.test262;
  compileFlags.requireSingleFunction = singleFunction;
#ifdef HERMES_ENABLE_DEBUGGER
  // Required to allow stepping and examining local variables in eval'd code
  compileFlags.debug = true;
#endif

  std::function<void(Module &)> runOptimizationPasses;
#ifdef HERMESVM_ENABLE_OPTIMIZATION_AT_RUNTIME
  if (runtime.optimizedEval)
    runOptimizationPasses = hbc::fullOptimizationPipeline;
#endif

  std::unique_ptr<hbc::BCProvider> bytecode;
  {
    std::unique_ptr<hermes::Buffer> buffer =
        makeCompilationSourceBuffer(utf8code, compileFlags);

    if (codeBlock) {
      assert(
          lexicalScopeIdxInParentFunction &&
          "lexical scope required for non-global eval");
      assert(
          !singleFunction && "Function constructor must always be global eval");
      // Local eval.
      std::unique_ptr<hbc::BCProvider> newBCProvider;
      std::string error;
      executeInStack(
          runtime.getStackExecutor(),
          [&newBCProvider,
           &error,
           &buffer,
           codeBlock,
           compileFlags,
           lexicalScopeIdxInParentFunction]() {
            std::tie(newBCProvider, error) = hbc::compileEvalModule(
                std::move(buffer),
                codeBlock->getRuntimeModule()->getBytecode(),
                codeBlock->getFunctionID(),
                compileFlags,
                *lexicalScopeIdxInParentFunction);
          });
      if (!newBCProvider) {
        return runtime.raiseSyntaxError(llvh::StringRef(error));
      }
      bytecode = std::move(newBCProvider);
    } else {
      // Global eval.
      // Creates a new AST Context and compiles everything independently:
      // new SemContext, new IR Module, everything.
      std::pair<std::unique_ptr<hbc::BCProvider>, std::string> bytecode_err;
      executeInStack(
          runtime.getStackExecutor(),
          [&bytecode_err, &buffer, compileFlags, &runOptimizationPasses]() {
            bytecode_err = hbc::createBCProviderFromSrc(
                std::move(buffer),
                "",
                {},
                compileFlags,
                "eval",
                {},
                nullptr,
                runOptimizationPasses);
          });
      if (!bytecode_err.first) {
        return runtime.raiseSyntaxError(TwineChar16(bytecode_err.second));
      }
      bytecode = std::move(bytecode_err.first);
    }
  }

  // TODO: pass a sourceURL derived from a '//# sourceURL' comment.
  llvh::StringRef sourceURL{};
  return runtime.runBytecode(
      std::move(bytecode),
      RuntimeModuleFlags{},
      sourceURL,
      environment,
      thisArg,
      newTarget);
}

CallResult<HermesValue> directEval(
    Runtime &runtime,
    Handle<StringPrimitive> str,
    bool strictCaller,
    const CodeBlock *codeBlock,
    bool singleFunction) {
  // Convert the code into UTF8.
  std::string code;
  auto view = StringPrimitive::createStringView(runtime, str);
  if (view.isASCII()) {
    code = std::string(view.begin(), view.end());
  } else {
    SmallU16String<4> allocator;
    convertUTF16ToUTF8WithReplacements(code, view.getUTF16Ref(allocator));
  }

  return evalInEnvironment(
      runtime,
      code,
      strictCaller,
      Runtime::makeNullHandle<Environment>(),
      codeBlock,
      runtime.getGlobal(),
      Runtime::getUndefinedValue(),
      singleFunction,
      /* lexScope */ llvh::None);
}

CallResult<HermesValue> eval(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  if (!args.getArg(0).isString()) {
    return args.getArg(0);
  }

  return directEval(
      runtime, args.dyncastArg<StringPrimitive>(0), false, nullptr, false);
}

} // namespace vm
} // namespace hermes
