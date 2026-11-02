/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Support/OSCompat.h"
#include "hermes/TimerStats.h"
#include "hermes/hermes.h" // @manual=fbsource//xplat/hermes/API:HermesAPI

#include "llvh/Support/CommandLine.h"
#include "llvh/Support/FileSystem.h"
#include "llvh/Support/InitLLVM.h"
#include "llvh/Support/MemoryBuffer.h"
#include "llvh/Support/PrettyStackTrace.h"
#include "llvh/Support/Program.h"
#include "llvh/Support/Signals.h"

using namespace facebook;

static llvh::cl::opt<bool> CheckSyntax(
    "check",
    llvh::cl::desc("check script syntax without executing"),
    llvh::cl::init(false));

static llvh::cl::opt<std::string> EvalScript(
    "eval",
    llvh::cl::desc("evaluate script"),
    llvh::cl::value_desc("script"));

static llvh::cl::opt<bool> PrintResult(
    "print",
    llvh::cl::desc("print result of evaluated script"),
    llvh::cl::init(false));

static llvh::cl::opt<bool> CollectTiming(
    "collect-timing",
    llvh::cl::desc("enable timing stats collection"),
    llvh::cl::init(false));

static llvh::cl::opt<std::string> InputFilename(
    llvh::cl::Positional,
    llvh::cl::desc("<file>"),
    llvh::cl::init("-"));

class FileBuffer : public jsi::Buffer {
 public:
  static std::shared_ptr<jsi::Buffer> bufferFromFile(llvh::StringRef path) {
    auto fileBuffer = llvh::MemoryBuffer::getFileOrSTDIN(path);
    if (!fileBuffer)
      return nullptr;
    return std::make_shared<FileBuffer>(std::move(*fileBuffer));
  }

  FileBuffer(std::unique_ptr<llvh::MemoryBuffer> buffer)
      : buffer_(std::move(buffer)) {};
  size_t size() const override {
    return buffer_->getBufferSize();
  }
  const uint8_t *data() const override {
    return reinterpret_cast<const uint8_t *>(buffer_->getBufferStart());
  }

 private:
  std::unique_ptr<llvh::MemoryBuffer> buffer_;
};

int main(int argc, char **argv) {
  // Normalize the arg vector.
  llvh::InitLLVM initLLVM(argc, argv);
  // Print a stack trace if we signal out.
  llvh::sys::PrintStackTraceOnErrorSignal("Hermes JSI");
  llvh::PrettyStackTraceProgram X(argc, argv);
  // Call llvm_shutdown() on exit to print stats and free memory.
  llvh::llvm_shutdown_obj Y;

  llvh::cl::ParseCommandLineOptions(argc, argv, "Hermes JSI\n");

  // Suppress any ASAN leak complaints for the alt signal stack on exit.
  ::hermes::oscompat::SigAltStackLeakSuppressor sigAltLeakSuppressor;

  if (!EvalScript.empty() && InputFilename != "-") {
    llvh::errs() << "Cannot use both --eval <script> and <file>" << '\n';
    return EXIT_FAILURE;
  }

  auto jsiBuffer = !EvalScript.empty()
      ? std::make_shared<jsi::StringBuffer>(EvalScript)
      : FileBuffer::bufferFromFile(InputFilename);

  if (!jsiBuffer) {
    llvh::errs() << "Error! Failed to open file: " << InputFilename << '\n';
    return EXIT_FAILURE;
  }

  auto srcPath = !EvalScript.empty() ? "<eval>"
      : InputFilename == "-"         ? "<stdin>"
                                     : std::string(InputFilename);

  std::unique_ptr<jsi::Runtime> runtime(facebook::hermes::makeHermesRuntime());

  if (CollectTiming) {
    runtime = facebook::hermes::makeTimedRuntime(std::move(runtime));
  }

  try {
    auto js = runtime->prepareJavaScript(jsiBuffer, srcPath);

    if (!CheckSyntax) {
      auto result = runtime->evaluatePreparedJavaScript(js);

      if (PrintResult) {
        llvh::outs() << result.toString(*runtime).utf8(*runtime) << '\n';
      }
    }
  } catch (const jsi::JSIException &e) {
    llvh::errs() << "JavaScript terminated via uncaught exception: " << e.what()
                 << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
