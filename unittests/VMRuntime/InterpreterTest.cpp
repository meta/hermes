/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"

#include "hermes/BCGen/HBC/SimpleBytecodeBuilder.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/SmallXString.h"
#include "hermes/VM/StringView.h"

#include "gtest/gtest.h"

#include "llvh/Support/raw_ostream.h"

using namespace hermes::vm;
using namespace hermes::hbc;

namespace vmdetail = hermes::vm::detail;

/// Associate a label with an instruction. Use it like this:
/// \begincode
///   LABEL(L(1), builder.emitMov(1, 2));
/// \endcode
#define LABEL(L, emit) \
  do {                 \
    auto ofs = emit;   \
    if (pass == 0)     \
      labels[L] = ofs; \
  } while (0)

/// Emit a conditional jump to label. Use it like this:
/// \begincode
///   JCOND(builder.emitJLessEqualN, L(2), 0, 2);
/// \endcode
#define JCOND(name, L, op2, op3)              \
  do {                                        \
    int line = __LINE__;                      \
    if (pass == 0)                            \
      jmps[line] = name(0, op2, op3);         \
    else                                      \
      name(labels[L] - jmps[line], op2, op3); \
  } while (0);

/// Emit an unconditional jump to label. Use it like this:
/// \begincode
///   JMP(builder.emitJmp, L(2));
/// \endcode
#define JMP(name, L)                \
  do {                              \
    int line = __LINE__;            \
    if (pass == 0)                  \
      jmps[line] = name(0);         \
    else                            \
      name(labels[L] - jmps[line]); \
  } while (0);

/// Emit an jump builtin to label. Use it like this:
/// \begincode
///   JMP(builder.emitJmpBuiltinIs, L(2), builtinIdx, reg);
/// \endcode
#define JMP_BUILTINIS(name, L, idx, reg)      \
  do {                                        \
    int line = __LINE__;                      \
    if (pass == 0)                            \
      jmps[line] = name(0, idx, reg);         \
    else                                      \
      name(labels[L] - jmps[line], idx, reg); \
  } while (0);

/// Make labels more noticeable in the source by always using this macro to
/// refer to them.
#define L(x) x

namespace {

/// Convert all arguments to string and print them followed by new line.
static CallResult<HermesValue> print(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope scope(runtime);
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  bool first = true;

  for (Handle<> arg : args.handles()) {
    auto res = toString_RJS(runtime, arg);
    if (res != ExecutionStatus::RETURNED)
      return ExecutionStatus::EXCEPTION;
    lv.str = std::move(*res);

    if (!first)
      llvh::outs() << " ";
    SmallU16String<32> tmp;
    llvh::outs()
        << StringPrimitive::createStringView(runtime, lv.str).getUTF16Ref(tmp);
    first = false;
  }

  llvh::outs() << "\n";
  return HermesValue::encodeUndefinedValue();
}

class InterpreterFunctionTest : public RuntimeTestFixture {
 private:
  struct ResultLocals : public Locals {
    PinnedValue<StringPrimitive> str;
  } resultLv_;
  LocalsRAII resultLraii_;
  RuntimeModule *runtimeModule;
  SimpleBytecodeBuilder builder;
  bool hasRun = false;
  CallResult<HermesValue> result{ExecutionStatus::EXCEPTION};

 protected:
  BytecodeInstructionGenerator instGen;

  InterpreterFunctionTest()
      : RuntimeTestFixture(),
        resultLraii_(runtime, &resultLv_),
        runtimeModule(RuntimeModule::createUninitialized(runtime, domain)) {}

  CallResult<HermesValue> run() {
    assert(!hasRun);
    builder.addFunction(1, 1, instGen.acquireBytecode());
    auto *codeBlock = createSimpleCodeBlock(runtimeModule, runtime, builder);
    ScopedNativeCallFrame frame(
        runtime,
        0,
        HermesValue::encodeNativePointer(codeBlock),
        HermesValue::encodeUndefinedValue(),
        HermesValue::encodeUndefinedValue());
    assert(!frame.overflowed());
    result = runtime.interpretFunction(codeBlock);

    hasRun = true;
    return result;
  }

  bool getResultAsBool() const {
    assert(hasRun);
    assert(result != ExecutionStatus::EXCEPTION);
    return result->getBool();
  }

  Handle<StringPrimitive> getResultAsString() {
    assert(hasRun);
    assert(result != ExecutionStatus::EXCEPTION);
    resultLv_.str = result->getString();
    return resultLv_.str;
  }
};

using InterpreterTest = RuntimeTestFixture;

TEST_F(InterpreterTest, SimpleSmokeTest) {
  auto *runtimeModule = RuntimeModule::createUninitialized(runtime, domain);

  /*
   ; calculate 10 - 2 and print "result =" the value.
   load_imm     reg0, 10
   load_imm     reg1, 2
   subn         reg2, reg0, reg1
   get_global   reg0
   get_named    reg1, reg0, "print"
   load_imm     reg3, #undefined
   push         reg3
   load_string  reg3, "result="
   push         reg3
   push         reg2
   call         reg3, reg1, 3
   ret          reg2
   */

  StringID printID = 1;
  StringID resultID = 2;

  const int FRAME_SIZE = 16;
  SimpleBytecodeBuilder builder;
  BytecodeInstructionGenerator instGen;

  instGen.emitLoadConstDoubleDirect(0, 10);
  instGen.emitLoadConstDoubleDirect(1, 2);
  instGen.emitSubN(2, 0, 1);
  instGen.emitGetGlobalObject(0);
  instGen.emitGetById(1, 0, 0, printID);
  instGen.emitLoadConstUndefined(3);
  instGen.emitMov(
      static_cast<unsigned>(FRAME_SIZE + StackFrameLayout::ThisArg), 3);
  instGen.emitLoadConstString(
      static_cast<unsigned>(FRAME_SIZE + StackFrameLayout::FirstArg), resultID);
  instGen.emitMov(
      static_cast<unsigned>(FRAME_SIZE + StackFrameLayout::FirstArg - 1), 2);
  instGen.emitCall(3, 1, 3);
  instGen.emitRet(2);

  builder.addFunction(1, FRAME_SIZE, instGen.acquireBytecode(), 1, 0);
  auto *codeBlock = createSimpleCodeBlock(runtimeModule, runtime, builder);

  ASSERT_EQ(vmdetail::mapStringMayAllocate(*runtimeModule, "print"), printID);
  ASSERT_EQ(
      vmdetail::mapStringMayAllocate(*runtimeModule, "result="), resultID);

  struct : public Locals {
    PinnedValue<NativeFunction> printFn;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.printFn = NativeFunction::create(
      runtime,
      nullptr,
      print,
      Predefined::getSymbolID(Predefined::emptyString),
      0);

  // Define the 'print' function.
  (void)JSObject::putNamed_RJS(
      runtime.getGlobal(),
      runtime,
      runtimeModule->getSymbolIDFromStringIDMayAllocate(printID),
      lv.printFn);

  CallResult<HermesValue> status{ExecutionStatus::EXCEPTION};
  {
    ScopedNativeCallFrame frame(
        runtime,
        0,
        HermesValue::encodeNativePointer(codeBlock),
        HermesValue::encodeUndefinedValue(),
        HermesValue::encodeUndefinedValue());
    ASSERT_FALSE(frame.overflowed());
    status = runtime.interpretFunction(codeBlock);
  }

  auto frames = runtime.getStackFrames();
  ASSERT_TRUE(frames.begin() == frames.end());
  ASSERT_EQ(
      StackFrameLayout::CalleeExtraRegistersAtStart, runtime.getStackLevel());
  ASSERT_EQ(ExecutionStatus::RETURNED, status.getStatus());
  ASSERT_EQ(8.0, status.getValue().getDouble());
}

TEST_F(InterpreterTest, IterativeFactorialTest) {
  auto runtimeModule = RuntimeModule::createUninitialized(runtime, domain);

  /*
   get_arg    reg0, 1           ; load n
   mov        reg1, reg0        ; res = n
   load_imm   reg2, 1           ; constant for reuse
   to_number  reg0, reg0        ; --n
L1:
   subn       reg0, reg0, reg2
   jlen       L2, reg0, reg2    ; if n <= 1 goto L2
   mul        reg1, reg1, reg0  ; res *= n
   jmp        L1
L2:
   ret        reg1              ; return res
   */

  std::map<int, int> labels{};
  std::map<int, int> jmps{};

  auto emit = [&](BytecodeInstructionGenerator &builder, int pass) {
    builder.emitLoadParam(0, 1);
    builder.emitMov(1, 0);
    builder.emitLoadConstDoubleDirect(2, 1);
    builder.emitToNumber(0, 0);
    LABEL(L(1), builder.emitSubN(0, 0, 2));
    JCOND(builder.emitJLessEqualN, L(2), 0, 2);
    builder.emitMul(1, 1, 0);
    JMP(builder.emitJmp, L(1));
    LABEL(L(2), builder.emitRet(1));
  };

  // Pass 0 - resolve labels.
  {
    BytecodeInstructionGenerator instGen;
    emit(instGen, 0);
  }

  // Pass 1 - build the actual code.
  SimpleBytecodeBuilder builder;
  BytecodeInstructionGenerator instGen;
  emit(instGen, 1);
  builder.addFunction(1, 3, instGen.acquireBytecode());
  auto *codeBlock = createSimpleCodeBlock(runtimeModule, runtime, builder);

  CallResult<HermesValue> status{ExecutionStatus::EXCEPTION};
  {
    ScopedNativeCallFrame newFrame(
        runtime,
        1,
        HermesValue::encodeUndefinedValue(),
        HermesValue::encodeUndefinedValue(),
        HermesValue::encodeUndefinedValue());
    ASSERT_FALSE(newFrame.overflowed());
    newFrame->getArgRef(0) = HermesValue::encodeTrustedNumberValue(5);
    status = runtime.interpretFunction(codeBlock);
  }
  auto frames = runtime.getStackFrames();
  ASSERT_TRUE(frames.begin() == frames.end());
  ASSERT_EQ(
      StackFrameLayout::CalleeExtraRegistersAtStart, runtime.getStackLevel());
  ASSERT_EQ(ExecutionStatus::RETURNED, status.getStatus());
  ASSERT_EQ(120.0, status.getValue().getDouble());
}

TEST_F(InterpreterTest, RecursiveFactorialTest) {
  struct : public Locals {
    PinnedValue<Domain> domainH;
    PinnedValue<JSFunction> factFn;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto runtimeModule = RuntimeModule::createUninitialized(runtime, domain);

  auto factID = vmdetail::mapStringMayAllocate(*runtimeModule, "fact");

  /*
   get_arg    reg0, 1           ; load n
   load_imm   reg1, 2           ; load constant 2
   jg         L1, reg0, reg1    ; if n > 2 goto L1
   ret        reg0              ; return n
L1:
   load_imm   reg1, 1           ; load constant 1
   sub        reg2, reg0, reg1  ; reg2 = n-1
   load_imm   reg1, #undefined  ; fact(n-1)
   push       reg1
   push       reg2
   get_global reg1
   get_named  reg1, reg1, "fact"
   call       reg1, reg1, 2
   mul        reg0, reg0, reg1 ; return n*fact(n-1)
   ret        reg0
   */

  std::map<int, int> labels{};
  std::map<int, int> jmps{};
  const unsigned FRAME_SIZE = 16;

  auto emit = [&](BytecodeInstructionGenerator &builder, int pass) {
    builder.emitLoadParam(0, 1);
    builder.emitLoadConstDoubleDirect(1, 2);
    JCOND(builder.emitJGreater, L(1), 0, 1);
    builder.emitRet(0);
    LABEL(L(1), builder.emitLoadConstDoubleDirect(1, 1));
    builder.emitLoadConstUndefined(FRAME_SIZE + StackFrameLayout::ThisArg);
    builder.emitSub(FRAME_SIZE + StackFrameLayout::FirstArg, 0, 1);
    builder.emitGetGlobalObject(1);
    builder.emitGetById(1, 1, 0, factID);
    builder.emitCall(1, 1, 2);
    builder.emitMul(0, 0, 1);
    builder.emitRet(0);
  };

  // Pass 0 - resolve labels.
  {
    BytecodeInstructionGenerator instGen;
    emit(instGen, 0);
  }

  // Pass 1 - build the actual code.
  SimpleBytecodeBuilder BMG;
  BytecodeInstructionGenerator instGen;
  emit(instGen, 1);
  BMG.addFunction(1, FRAME_SIZE, instGen.acquireBytecode(), 1, 0);
  auto *codeBlock = createSimpleCodeBlock(runtimeModule, runtime, BMG);

  lv.domainH = runtimeModule->getDomainUnsafe(runtime);
  lv.factFn = JSFunction::create(
      runtime,
      lv.domainH,
      runtime.makeNullHandle<JSObject>(),
      runtime.makeNullHandle<Environment>(),
      codeBlock);

  // Define the 'fact' function.
  (void)JSObject::putNamed_RJS(
      runtime.getGlobal(),
      runtime,
      runtimeModule->getSymbolIDFromStringIDMayAllocate(factID),
      lv.factFn);

  {
    CallResult<HermesValue> status{ExecutionStatus::EXCEPTION};
    {
      ScopedNativeCallFrame newFrame(
          runtime,
          1,
          HermesValue::encodeUndefinedValue(),
          HermesValue::encodeUndefinedValue(),
          HermesValue::encodeUndefinedValue());
      ASSERT_FALSE(newFrame.overflowed());
      newFrame->getArgRef(0) = HermesValue::encodeTrustedNumberValue(2);
      status = runtime.interpretFunction(codeBlock);
    }
    auto frames = runtime.getStackFrames();
    ASSERT_TRUE(frames.begin() == frames.end());
    ASSERT_EQ(
        StackFrameLayout::CalleeExtraRegistersAtStart, runtime.getStackLevel());
    ASSERT_EQ(ExecutionStatus::RETURNED, status.getStatus());
    ASSERT_EQ(2.0, status.getValue().getDouble());
  }

  {
    CallResult<HermesValue> status{ExecutionStatus::EXCEPTION};
    {
      ScopedNativeCallFrame newFrame(
          runtime,
          1,
          HermesValue::encodeNativePointer(codeBlock),
          HermesValue::encodeUndefinedValue(),
          HermesValue::encodeUndefinedValue());
      ASSERT_FALSE(newFrame.overflowed());
      newFrame->getArgRef(0) = HermesValue::encodeTrustedNumberValue(5);
      status = runtime.interpretFunction(codeBlock);
    }
    auto frames = runtime.getStackFrames();
    ASSERT_TRUE(frames.begin() == frames.end());
    ASSERT_EQ(
        StackFrameLayout::CalleeExtraRegistersAtStart, runtime.getStackLevel());
    ASSERT_EQ(ExecutionStatus::RETURNED, status.getStatus());
    ASSERT_EQ(120.0, status.getValue().getDouble());
  }
}

TEST_F(InterpreterTest, TestJmpBuiltinIs) {
  auto *runtimeModule = RuntimeModule::createUninitialized(runtime, domain);

  StringID functionID = 1;
  StringID callID = 2;

  std::map<int, int> labels{};
  std::map<int, int> jmps{};
  const unsigned FRAME_SIZE = 16;
  SimpleBytecodeBuilder builder;

  auto emit = [&](BytecodeInstructionGenerator &builder, int pass) {
    builder.emitGetGlobalObject(0);
    builder.emitGetById(0, 0, 0, functionID);
    builder.emitGetById(0, 0, 0, callID);
    JMP_BUILTINIS(
        builder.emitJmpBuiltinIs,
        L(1),
        (uint8_t)hermes::BuiltinMethod::HermesBuiltin_functionPrototypeCall,
        0);
    builder.emitUnreachable();
    // Use LoadConstUndefined for convenience because JMP_BUILTINIS isn't an
    // expression so it's hard to put it directly into LABEL.
    LABEL(L(1), builder.emitLoadConstUndefined(1));
    JMP_BUILTINIS(
        builder.emitJmpBuiltinIsNot,
        L(2),
        (uint8_t)hermes::BuiltinMethod::HermesBuiltin_functionPrototypeApply,
        0);
    builder.emitUnreachable();
    LABEL(L(2), builder.emitLoadConstUndefined(1));
    JMP_BUILTINIS(
        builder.emitJmpBuiltinIsLong,
        L(3),
        (uint8_t)hermes::BuiltinMethod::HermesBuiltin_functionPrototypeCall,
        0);
    builder.emitUnreachable();
    LABEL(L(3), builder.emitLoadConstUndefined(1));
    JMP_BUILTINIS(
        builder.emitJmpBuiltinIsNotLong,
        L(4),
        (uint8_t)hermes::BuiltinMethod::HermesBuiltin_functionPrototypeApply,
        0);
    builder.emitUnreachable();
    LABEL(L(4), builder.emitLoadConstUndefined(0));
    builder.emitRet(1);
  };
  // Pass 0 - resolve labels.
  {
    BytecodeInstructionGenerator instGen;
    emit(instGen, 0);
  }

  // Pass 1 - build the actual code.
  SimpleBytecodeBuilder BMG;
  BytecodeInstructionGenerator instGen;
  emit(instGen, 1);
  BMG.addFunction(1, FRAME_SIZE, instGen.acquireBytecode(), 1, 0);
  auto *codeBlock = createSimpleCodeBlock(runtimeModule, runtime, BMG);

  ASSERT_EQ(
      vmdetail::mapStringMayAllocate(*runtimeModule, "Function"), functionID);
  ASSERT_EQ(vmdetail::mapStringMayAllocate(*runtimeModule, "call"), callID);

  CallResult<HermesValue> status{ExecutionStatus::EXCEPTION};
  {
    ScopedNativeCallFrame frame(
        runtime,
        0,
        HermesValue::encodeNativePointer(codeBlock),
        HermesValue::encodeUndefinedValue(),
        HermesValue::encodeUndefinedValue());
    ASSERT_FALSE(frame.overflowed());
    status = runtime.interpretFunction(codeBlock);
  }

  ASSERT_TRUE(status.getValue().isUndefined());
}

TEST_F(InterpreterFunctionTest, GetByIdSlowPathChecksForExceptions) {
  instGen.emitLoadConstUndefined(0);
  instGen.emitGetById(0, 0, 0, 0);
  instGen.emitRet(0);
  ASSERT_EQ(ExecutionStatus::EXCEPTION, run());
}

TEST_F(InterpreterFunctionTest, PutByIdSlowPathChecksForExceptions) {
  instGen.emitLoadConstUndefined(0);
  instGen.emitPutByIdStrict(0, 0, 0, 0);
  instGen.emitRet(0);
  ASSERT_EQ(ExecutionStatus::EXCEPTION, run());
}

TEST_F(InterpreterFunctionTest, TestNot) {
  instGen.emitLoadConstFalse(0);
  instGen.emitNot(0, 0);
  instGen.emitRet(0);
  ASSERT_EQ(ExecutionStatus::RETURNED, run());
  EXPECT_TRUE(getResultAsBool());
}

TEST_F(InterpreterFunctionTest, TestToString) {
  instGen.emitLoadConstFalse(0);
  instGen.emitAddEmptyString(0, 0);
  instGen.emitRet(0);
  ASSERT_EQ(ExecutionStatus::RETURNED, run());
  SmallU16String<8> tmp;
  getResultAsString()->appendUTF16String(tmp);
  ASSERT_EQ(createUTF16Ref(u"false"), tmp.arrayRef());
}

#if defined(NDEBUG) && !defined(HERMES_UBSAN) && \
    !LLVM_THREAD_SANITIZER_BUILD && !LLVM_ADDRESS_SANITIZER_BUILD
// Returns the native stack pointer of the callee frame.
static CallResult<HermesValue> getSP(void *, Runtime &runtime) {
  int dummy = 0;
  return HermesValue::encodeNativePointer(&dummy);
}

// Use a non-inline function to perform the stack size measurement so that it
// takes place in a new stack frame. This ensures that location of dummy on the
// stack really is right before the stack frame for interpretFunction.
LLVM_ATTRIBUTE_NOINLINE static void testInterpreterStackSize(
    Runtime &runtime,
    CodeBlock *codeBlock) {
  // Check that inner and outer stack pointer differ by at most a set threshold.
  int dummy;
  const auto outerStackPointer = reinterpret_cast<uintptr_t>(&dummy);
  auto status = runtime.interpretFunction(codeBlock);
  ASSERT_EQ(ExecutionStatus::RETURNED, status.getStatus());
  const auto innerStackPointer =
      reinterpret_cast<uintptr_t>(status.getValue().getNativePointer<void>());
  // Increase this only if you have a reason to grow the interpreter's frame.
#ifdef _MSC_VER
  // TODO(T42117517) Understand why stack frame size is large on Windows
  uintptr_t kStackFrameSizeLimit = 3700;
#else
  uintptr_t kStackFrameSizeLimit = 1500;
#endif
  ASSERT_LE(outerStackPointer - innerStackPointer, kStackFrameSizeLimit);
}

// In release mode, we test the size of the interpreter's stack frame.
// "getSP" is installed as a native function and called from JS. The
// distance from its "inner" frame to the "outer" frame that invoked
// the interpreter is our approximate measure of the stack size used
// by the interpreter. We set a limit that will catch severe
// regressions, e.g., due to compiler quirks.
TEST_F(InterpreterTest, FrameSizeTest) {
  auto runtimeModule = RuntimeModule::createUninitialized(runtime, domain);

  /*
   get_global   reg0
   get_named    reg1, reg0, "getSP"
   call         reg0, reg1, 0
   ret          reg0
   */

  StringID getSPID = 1;

  const unsigned FRAME_SIZE = 16;
  SimpleBytecodeBuilder builder;
  BytecodeInstructionGenerator instGen;

  instGen.emitGetGlobalObject(0);
  instGen.emitGetById(1, 0, 0, getSPID);
  instGen.emitCall(0, 1, 0);
  instGen.emitRet(0);

  builder.addFunction(1, FRAME_SIZE, instGen.acquireBytecode(), 1, 0);
  auto *codeBlock = createSimpleCodeBlock(runtimeModule, runtime, builder);

  ASSERT_EQ(vmdetail::mapStringMayAllocate(*runtimeModule, "getSP"), getSPID);

  auto getSPFn = runtime.makeHandle<NativeFunction>(NativeFunction::create(
      runtime,
      nullptr,
      getSP,
      Predefined::getSymbolID(Predefined::emptyString),
      0));

  // Define the 'getSP' function.
  (void)JSObject::putNamed_RJS(
      runtime.getGlobal(),
      runtime,
      runtimeModule->getSymbolIDFromStringIDMayAllocate(getSPID),
      getSPFn);

  ScopedNativeCallFrame frame(
      runtime, 0, nullptr, false, HermesValue::encodeUndefinedValue());
  ASSERT_FALSE(frame.overflowed());

  testInterpreterStackSize(runtime, codeBlock);
}
#endif // NDEBUG

} // anonymous namespace
