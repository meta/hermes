/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// This benchmark is intended to measure the pure instruction dispatch
/// performance of the interpreter.
///
/// The benchmark consists of hand-optimized bytecode instructions for
/// calculating factorial multiple times in a loop.  It uses only simple
/// comparison and arithmetic instructions - there are no object conversions,
/// property access, or anything at all that would cause a slow path call
/// outside of the interpreter.
///
/// It eliminates the effects of the compiler (since it is hand optimized),
/// expensive instruction implementation (no calls, property access, etc),
/// d-cache size (it only uses a few registers), exceptions, heap allocation and
/// garbage collector, practically anything that doesn't have directly to do
/// with instruction dispatch.
///
/// So, it is not representative *at all* of the application performance of the
/// VM, but is very useful to derive insights about the one aspect it covers.
///
/// If it is slower than other interpreters on the same JavaScript code, under
/// these ideal circumstances and given the "competition" didn't hand optimize
/// their bytecodes, then we clearly have important low level performance work
/// to do in the interpreter.
///
/// If, on the other hand, it is faster, then we can focus on higher level
/// optimizations.
//===----------------------------------------------------------------------===//
#include "hermes/BCGen/HBC/BCProviderFromSrc.h"
#include "hermes/BCGen/HBC/SimpleBytecodeBuilder.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/SmallXString.h"
#include "hermes/VM/StringView.h"

#include "llvh/Support/CommandLine.h"
#include "llvh/Support/ManagedStatic.h"
#include "llvh/Support/PrettyStackTrace.h"
#include "llvh/Support/Signals.h"
#include "llvh/Support/raw_ostream.h"

#include <map>

using namespace hermes::vm;
using namespace hermes::hbc;

namespace {

/// Run the benchmark and store the resulting string in \p result.
void benchmark(
    Runtime &runtime,
    double loopc,
    double factc,
    MutableHandle<StringPrimitive> result) {
  struct : public Locals {
    PinnedValue<Domain> domain;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.domain = Domain::create(runtime);
  auto *runtimeModule = RuntimeModule::createUninitialized(runtime, lv.domain);

  std::map<int, int> labels{};
  std::map<int, int> jmps{};

#define LABEL(L, emit) \
  do {                 \
    auto ofs = emit;   \
    if (pass == 0)     \
      labels[L] = ofs; \
  } while (0)

#define JCOND(name, L, op2, op3)              \
  do {                                        \
    int line = __LINE__;                      \
    if (pass == 0)                            \
      jmps[line] = name(0, op2, op3);         \
    else                                      \
      name(labels[L] - jmps[line], op2, op3); \
  } while (0);

#define L(x) x

  /*
     LoadParam        r0, 1           ; load loopc
     LoadConstDouble  r1, 0           ; res = 0
     LoadConstDouble  r4, 1           ; constant 1 for reuse
     LoadConstDouble  r5, 0           ; constant 0 for reuse

     ToNumber         r0, r0
     SubN             r0, r0, r4      ; --loopc
     JLessN           L1, r0, r5      ; if loopc < 0 goto L1
L2:
     LoadParam        r2, 2           ; n = factc
     Mov              r3, r2          ; fact = n
     ToNumber         r2, r2
     SubN             r2, r2, r4      ; --n
     JLessEqualN      L3, r2, r4      ; if n <= 1 goto L3
L4:
     Mul              r3, r3, r2      ; fact *= n
     SubN             r2, r2, r4      ; --n
     JGreaterN        L4, r4, r2, r4  ; if n > 1 goto L4
L3:
     AddN             r1, r1, r3      ; res += fact
     SubN             r0, r0, r4      ; --loopc
     JGreaterEqualN   L2, r0, r5      ; if loopc >= 0 goto L2
L1:
     ToString         r1, r1
     Ret              r1
     */

  const unsigned FRAME_SIZE = 9;

  auto emit = [&](BytecodeInstructionGenerator &builder, int pass) {
    builder.emitLoadParam(0, 1);
    builder.emitLoadConstDoubleDirect(1, 0);
    builder.emitLoadConstDoubleDirect(4, 1);
    builder.emitLoadConstDoubleDirect(5, 0);
    builder.emitToNumber(0, 0);
    builder.emitSubN(0, 0, 4);
    JCOND(builder.emitJLessN, L(1), 0, 5);
    LABEL(2, builder.emitLoadParam(2, 2));
    builder.emitMov(3, 2);
    builder.emitToNumber(2, 2);
    builder.emitSubN(2, 2, 4);
    JCOND(builder.emitJLessEqualN, L(3), 2, 4);
    LABEL(4, builder.emitMul(3, 3, 2));
    builder.emitSubN(2, 2, 4);
    JCOND(builder.emitJLessN, L(4), 4, 2);
    LABEL(3, builder.emitAddN(1, 1, 3));
    builder.emitSubN(0, 0, 4);
    JCOND(builder.emitJLessEqualN, L(2), 5, 0);
    LABEL(1, builder.emitAddEmptyString(1, 1));
    builder.emitRet(1);
  };

  // Pass 0 - resolve labels.
  {
    BytecodeInstructionGenerator BFG;
    emit(BFG, 0);
  }

  // Pass 1 - build the actual code.

  SimpleBytecodeBuilder BMG;
  BytecodeInstructionGenerator BFG;
  emit(BFG, 1);
  BMG.addFunction(2, FRAME_SIZE, BFG.acquireBytecode());
  auto buffer = BMG.generateBytecodeBuffer();

  runtimeModule->initializeWithoutCJSModulesMayAllocate(
      BCProviderFromBuffer::createBCProviderFromBuffer(std::move(buffer))
          .first);
  auto *codeBlock = runtimeModule->getCodeBlockMayAllocate(0);

  ScopedNativeCallFrame newFrame{
      runtime, 2, nullptr, false, HermesValue::encodeUndefinedValue()};
  assert(!newFrame.overflowed() && "Frame allocation should not have failed");
  newFrame->getArgRef(0) = HermesValue::encodeTrustedNumberValue(loopc);
  newFrame->getArgRef(1) = HermesValue::encodeTrustedNumberValue(factc);

  auto status = runtime.interpretFunction(codeBlock);
  assert(status == ExecutionStatus::RETURNED);
  result.set(vmcast<StringPrimitive>(*status));
}
} // namespace

static llvh::cl::opt<double> LoopCount{
    llvh::cl::Positional,
    llvh::cl::init(4e6),
    llvh::cl::desc("(loop count)")};
static llvh::cl::opt<int> FactValue{
    llvh::cl::Positional,
    llvh::cl::init(100),
    llvh::cl::desc("(factorial value)")};

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  llvh::sys::PrintStackTraceOnErrorSignal("Hermes driver");
  llvh::PrettyStackTraceProgram X(argc, argv);
  // Call llvm_shutdown() on exit to print stats and free memory.
  llvh::llvm_shutdown_obj Y;
  llvh::cl::ParseCommandLineOptions(argc, argv, "Hermes vm driver\n");

  llvh::outs() << "Running " << (uint64_t)LoopCount << " loops of factorial("
               << FactValue << ")\n";

  auto runtime = Runtime::create(
      RuntimeConfig::Builder()
          .withGCConfig(
              GCConfig::Builder()
                  .withInitHeapSize(1 << 16)
                  .withMaxHeapSize(1 << 19)
                  .build())
          .build());

  GCScope scope(*runtime);
  struct : public Locals {
    PinnedValue<StringPrimitive> res;
  } lv;
  LocalsRAII lraii(*runtime, &lv);
  benchmark(
      *runtime, LoopCount, FactValue, MutableHandle<StringPrimitive>{lv.res});
  SmallU16String<32> tmp;
  llvh::outs()
      << StringPrimitive::createStringView(*runtime, lv.res).getUTF16Ref(tmp)
      << "\n";
#ifdef HERMESVM_OPCODE_STATS
  Runtime::dumpOpcodeStats(llvh::outs());
#endif
  return 0;
}
