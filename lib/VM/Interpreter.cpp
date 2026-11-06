/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "vm"
#include "hermes/VM/Interpreter.h"
#include "hermes/VM/Runtime.h"

#include "hermes/Inst/InstDecode.h"
#include "hermes/Support/Conversions.h"
#include "hermes/Support/SlowAssert.h"
#include "hermes/Support/Statistic.h"
#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/FastArray.h"
#include "hermes/VM/HandleRootOwner-inline.h"
#include "hermes/VM/JIT/JIT.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSGeneratorObject.h"
#include "hermes/VM/JSObject-inline.h"
#include "hermes/VM/JSProxy.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Profiler.h"
#include "hermes/VM/Profiler/CodeCoverageProfiler.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringPrimitiveValueDenseMapInfo-inline.h"
#include "hermes/VM/StringView.h"
#include "hermes/VM/WeakRoot-inline.h"

#include "llvh/ADT/ScopeExit.h"
#include "llvh/Support/Debug.h"
#include "llvh/Support/Format.h"
#include "llvh/Support/raw_ostream.h"

#include "Interpreter-internal.h"

using llvh::dbgs;
using namespace hermes::inst;

HERMES_SLOW_STATISTIC(
    NumGetById,
    "NumGetById: Number of property 'read by id' accesses");
HERMES_SLOW_STATISTIC(
    NumGetByIdCacheHits,
    "NumGetByIdCacheHits: Number of property 'read by id' cache hits");
HERMES_SLOW_STATISTIC(
    NumGetByIdProtoHits,
    "NumGetByIdProtoHits: Number of property 'read by id' cache hits for the prototype");
HERMES_SLOW_STATISTIC(
    NumGetByIdDict,
    "NumGetByIdDict: Number of property 'read by id' of dictionaries");

HERMES_SLOW_STATISTIC(NumGetByIndex, "NumGetByIndex: Number of GetByIndex");

HERMES_SLOW_STATISTIC(NumGetByVal, "NumGetByVal: Number of GetByVal");
HERMES_SLOW_STATISTIC(
    NumGetByValObjInd,
    "NumGetByValObjInd: Number of GetByVal _,Obj,Index");
HERMES_SLOW_STATISTIC(
    NumGetByValArrInd,
    "NumGetByValArrInd: Number of GetByVal _,JSArray,Index");
HERMES_SLOW_STATISTIC(
    NumGetByValTAInd,
    "NumGetByValTAInd: Number of GetByVal _,TypedArray,Index");
HERMES_SLOW_STATISTIC(
    NumGetByValObjIndFast,
    "NumGetByValObjIndFast: Number of GetByVal _,Obj,Index good fast paths");

HERMES_SLOW_STATISTIC(
    NumPutById,
    "NumPutById: Number of property 'write by id' accesses");
HERMES_SLOW_STATISTIC(
    NumPutByIdSlowPaths,
    "NumPutByIdSlowPaths: Number of property 'write by id' slow paths");
HERMES_SLOW_STATISTIC(
    NumPutByIdCacheHits,
    "NumPutByIdCacheHits: Number of property 'write by id' cache hits");
HERMES_SLOW_STATISTIC(
    NumPutByIdTransitionHits,
    "NumPutByIdTransitionHits: Number of property 'write by id' transition hits");

HERMES_SLOW_STATISTIC(
    NumNativeFunctionCalls,
    "NumNativeFunctionCalls: Number of native function calls");
HERMES_SLOW_STATISTIC(
    NumBoundFunctionCalls,
    "NumBoundCalls: Number of bound function calls");

// Ensure that instructions declared as having matching layouts actually do.
#include "InstLayout.inc"

namespace hermes {
namespace vm {

/// Initialize the state of some internal variables based on the current
/// code block.
#define INIT_STATE_FOR_CODEBLOCK(codeBlock)                      \
  do {                                                           \
    if (EnableCrashTrace) {                                      \
      auto *bc = (codeBlock)->getRuntimeModule()->getBytecode(); \
      bytecodeFileStart = bc->getRawBuffer().data();             \
      auto hash = bc->getSourceHash();                           \
      runtime.crashTrace_.recordModule(                          \
          bc->getSegmentID(),                                    \
          (codeBlock)->getRuntimeModule()->getSourceURL(),       \
          llvh::StringRef((const char *)&hash, sizeof(hash)));   \
    }                                                            \
  } while (0)

CallResult<PseudoHandle<JSGeneratorObject>> Interpreter::createGenerator_RJS(
    Runtime &runtime,
    RuntimeModule *runtimeModule,
    unsigned funcIndex,
    Handle<Environment> envHandle,
    NativeArgs args) {
  auto innerFuncPH = JSFunction::create(
      runtime,
      runtimeModule->getDomain(runtime),
      Handle<JSObject>::vmcast(&runtime.functionPrototype),
      envHandle,
      runtimeModule->getCodeBlockMayAllocate(funcIndex));
  auto innerFunc = runtime.makeHandle(std::move(innerFuncPH));

  auto generatorFunction = Handle<JSFunction>::vmcast(
      runtime.getCurrentFrame().getCalleeClosureHandleUnsafe());

  // The generator created by an async function is internal to it, and the
  // async function's "prototype" property, if any, belongs to user code.
  if (generatorFunction->getCodeBlock()->getHeaderFlags().getKind() ==
      FuncKind::Async) {
    return JSGeneratorObject::create(
        runtime,
        innerFunc,
        Handle<JSObject>::vmcast(&runtime.generatorPrototype));
  }

  auto prototypeProp = JSObject::getNamed_RJS(
      generatorFunction,
      runtime,
      Predefined::getSymbolID(Predefined::prototype));
  if (LLVM_UNLIKELY(prototypeProp == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSObject> prototype = vmisa<JSObject>(prototypeProp->get())
      ? runtime.makeHandle<JSObject>(prototypeProp->get())
      : Handle<JSObject>::vmcast(&runtime.generatorPrototype);

  return JSGeneratorObject::create(runtime, innerFunc, prototype);
}

CallResult<Handle<Arguments>> Interpreter::reifyArgumentsSlowPath(
    Runtime &runtime,
    Handle<Callable> curFunction,
    bool strictMode) {
  auto frame = runtime.getCurrentFrame();
  uint32_t argCount = frame.getArgCount();
  // Define each JavaScript argument.
  auto argRes = Arguments::create(runtime, argCount, curFunction, strictMode);
  if (LLVM_UNLIKELY(argRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<Arguments> args = runtime.makeHandle(std::move(*argRes));

  for (uint32_t argIndex = 0; argIndex < argCount; ++argIndex) {
    SmallHermesValue shv =
        SmallHermesValue::encodeHermesValue(frame.getArgRef(argIndex), runtime);
    Arguments::unsafeSetExistingElementAt(*args, runtime, argIndex, shv);
  }

  // The returned value should already be set from the create call.
  return args;
}

CallResult<PseudoHandle<>> Interpreter::getArgumentsPropByValSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *lazyReg,
    PinnedHermesValue *valueReg,
    Handle<Callable> curFunction,
    bool strictMode) {
  auto frame = runtime.getCurrentFrame();

  // If the arguments object has already been created.
  if (!lazyReg->isUndefined()) {
    // The arguments object has been created, so this is a regular property
    // get.
    assert(lazyReg->isObject() && "arguments lazy register is not an object");

    return JSObject::getComputed_RJS(
        Handle<JSObject>::vmcast(lazyReg), runtime, Handle<>(valueReg));
  }

  if (!valueReg->isSymbol()) {
    // Attempt a fast path in the case that the key is not a symbol.
    // If it is a symbol, force reification for now.
    // Convert the value to a string.
    auto strRes = toString_RJS(runtime, Handle<>(valueReg));
    if (strRes == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    auto strPrim = runtime.makeHandle(std::move(*strRes));

    // Check if the string is a valid argument index.
    if (auto index = toArrayIndex(*strPrim)) {
      if (*index < frame.getArgCount()) {
        return createPseudoHandle(frame.getArgRef(*index));
      }

      auto objectPrototype = Handle<JSObject>::vmcast(&runtime.objectPrototype);

      // OK, they are requesting an index that either doesn't exist or is
      // somewhere up in the prototype chain. Since we want to avoid reifying,
      // check which it is:
      MutableHandle<JSObject> inObject{runtime};
      ComputedPropertyDescWithSymStorage desc{MutableHandle<SymbolID>{runtime}};
      JSObject::getComputedPrimitiveDescriptor(
          objectPrototype, runtime, strPrim, inObject, desc);

      // If we couldn't find the property, just return 'undefined'.
      if (!inObject)
        return createPseudoHandle(HermesValue::encodeUndefinedValue());

      // If the property isn't an accessor, we can just return it without
      // reifying.
      if (!desc.flags.accessor) {
        return JSObject::getComputedSlotValue(
            createPseudoHandle(inObject.get()), runtime, desc);
      }
    }

    // Are they requesting "arguments.length"?
    if (runtime.symbolEqualsToStringPrim(
            Predefined::getSymbolID(Predefined::length), *strPrim)) {
      return createPseudoHandle(
          HermesValue::encodeTrustedNumberValue(frame.getArgCount()));
    }
  }

  // Looking for an accessor or a property that needs reification.
  auto argRes = reifyArgumentsSlowPath(runtime, curFunction, strictMode);
  if (argRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // Update the register with the reified value.
  *lazyReg = argRes->getHermesValue();

  // For simplicity, call ourselves again.
  return getArgumentsPropByValSlowPath_RJS(
      runtime, lazyReg, valueReg, curFunction, strictMode);
}

CallResult<PseudoHandle<>> Interpreter::handleCallSlowPath(
    Runtime &runtime,
    PinnedHermesValue *callTarget) {
  if (LLVM_UNLIKELY(!vmisa<Callable>(*callTarget))) {
    return runtime.raiseTypeErrorForValue(
        Handle<>(callTarget), " is not a function");
  }
#if HERMES_SLOW_STATISTIC_ENABLED
  if (vmisa<NativeJSFunction>(*callTarget)) {
    ++NumNativeFunctionCalls;
  } else if (vmisa<NativeFunction>(*callTarget)) {
    ++NumNativeFunctionCalls;
  } else if (vmisa<BoundFunction>(*callTarget)) {
    ++NumBoundFunctionCalls;
  }
#else
  (void)NumNativeFunctionCalls;
  (void)NumBoundFunctionCalls;
#endif
  return Callable::call(Handle<Callable>::vmcast(callTarget), runtime);
}

#ifndef NDEBUG

llvh::raw_ostream &operator<<(llvh::raw_ostream &OS, DumpHermesValue dhv) {
  OS << dhv.hv;
  // If it is a string, dump the contents, truncated to 8 characters.
  if (dhv.hv.isString()) {
    SmallU16String<32> str;
    dhv.hv.getString()->appendUTF16String(str);
    UTF16Ref ref = str.arrayRef();
    if (str.size() <= 8) {
      OS << ":'" << ref << "'";
    } else {
      OS << ":'" << ref.slice(0, 8) << "'";
      OS << "...[" << str.size() << "]";
    }
  }
  return OS;
}

void dumpCallArguments(
    llvh::raw_ostream &OS,
    Runtime &runtime,
    StackFramePtr calleeFrame) {
  OS << "arguments:\n";
  OS << "  " << 0 << " " << DumpHermesValue(calleeFrame.getThisArgRef())
     << "\n";
  for (unsigned i = 0; i < calleeFrame.getArgCount(); ++i) {
    OS << "  " << (i + 1) << " " << DumpHermesValue(calleeFrame.getArgRef(i))
       << "\n";
  }
}

LLVM_ATTRIBUTE_UNUSED
static void printDebugInfo(
    CodeBlock *curCodeBlock,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  // Check if LLVm debugging is enabled for us.
  bool debug = false;
  SLOW_DEBUG(debug = true);
  if (!debug)
    return;

  DecodedInstruction decoded = decodeInstruction(ip);

  dbgs() << llvh::format_decimal((const uint8_t *)ip - curCodeBlock->begin(), 4)
         << " OpCode::" << getOpCodeString(decoded.meta.opCode);

  for (unsigned i = 0; i < decoded.meta.numOperands; ++i) {
    auto operandType = decoded.meta.operandType[i];
    auto value = decoded.operandValue[i];

    dbgs() << (i == 0 ? " " : ", ");
    dumpOperand(dbgs(), operandType, value);

    if (operandType == OperandType::Reg8 || operandType == OperandType::Reg32) {
      // Print the register value, if source.
      if (i != 0 || decoded.meta.numOperands == 1)
        dbgs() << "="
               << DumpHermesValue(REG(static_cast<uint32_t>(value.integer)));
    }
  }

  dbgs() << "\n";
}

#endif

/// \return the address of the next instruction after \p ip, which must be a
/// call-type instruction.
LLVM_ATTRIBUTE_ALWAYS_INLINE
static inline const Inst *nextInstCall(const Inst *ip) {
  HERMES_SLOW_ASSERT(isCallType(ip->opCode) && "ip is not of call type");

  // To avoid needing a large lookup table or switchcase, the following packs
  // information about the size of each call opcode into a uint32_t. Each call
  // type is represented with two bits, representing how much larger it is than
  // the smallest call instruction.
  // If we used 64 bits, we could fit the actual size of each call, without
  // needing the offset, and this may be necessary if new call instructions are
  // added in the future. For now however, due to limitations on loading large
  // immediates in ARM, it is significantly more efficient to use a uint32_t
  // than a uint64_t.
  constexpr auto firstCall = std::min({
#define DEFINE_RET_TARGET(name) static_cast<uint8_t>(OpCode::name),
#include "hermes/BCGen/HBC/BytecodeList.def"
  });
  constexpr auto lastCall = std::max({
#define DEFINE_RET_TARGET(name) static_cast<uint8_t>(OpCode::name),
#include "hermes/BCGen/HBC/BytecodeList.def"
  });
  constexpr auto minSize = std::min({
#define DEFINE_RET_TARGET(name) sizeof(inst::name##Inst),
#include "hermes/BCGen/HBC/BytecodeList.def"
  });
  constexpr auto maxSize = std::max({
#define DEFINE_RET_TARGET(name) sizeof(inst::name##Inst),
#include "hermes/BCGen/HBC/BytecodeList.def"
  });

  constexpr uint32_t W = 2;
  constexpr uint32_t mask = (1 << W) - 1;

  static_assert(llvh::isUInt<W>(maxSize - minSize), "Size range too large.");
  static_assert((lastCall - firstCall + 1) * W <= 32, "Too many call opcodes.");

  constexpr uint32_t callSizes = 0
#define DEFINE_RET_TARGET(name)             \
  |                                         \
      ((sizeof(inst::name##Inst) - minSize) \
       << (((uint8_t)OpCode::name - firstCall) * W))
#include "hermes/BCGen/HBC/BytecodeList.def"

      ;
#undef DEFINE_RET_TARGET

  const uint8_t offset = static_cast<uint8_t>(ip->opCode) - firstCall;
  return IPADD(((callSizes >> (offset * W)) & mask) + minSize);
}

CallResult<HermesValue> Runtime::interpretFunctionImpl(
    CodeBlock *newCodeBlock) {
  if (LLVM_UNLIKELY(newCodeBlock->isLazy())) {
    if (LLVM_UNLIKELY(
            newCodeBlock->compileLazyFunction(*this) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }

#if defined(HERMES_MEMORY_INSTRUMENTATION) || !defined(NDEBUG)
  // We always call getCurrentIP() in a debug build as this has the effect
  // of asserting the IP is correctly set (not invalidated) at this point.
  // This allows us to leverage our whole test-suite to find missing cases
  // of CAPTURE_IP* macros in the interpreter loop.
  const inst::Inst *ip = getCurrentIP();
  (void)ip;
#endif
#ifdef HERMES_MEMORY_INSTRUMENTATION
  if (ip) {
    const CodeBlock *codeBlock;
    std::tie(codeBlock, ip) = getCurrentInterpreterLocation(ip);
    // All functions end in a Ret so we must match this with a pushCallStack()
    // before executing.
    if (codeBlock) {
      // Push a call entry at the last location we were executing bytecode.
      // This will correctly attribute things like eval().
      pushCallStack(codeBlock, ip);
    } else {
      // Push a call entry at the entry at the top of interpreted code.
      pushCallStack(newCodeBlock, (const Inst *)newCodeBlock->begin());
    }
  } else {
    // Push a call entry at the entry at the top of interpreted code.
    pushCallStack(newCodeBlock, (const Inst *)newCodeBlock->begin());
  }
#endif

  InterpreterState state{newCodeBlock, 0};
  CallResult<HermesValue> res{ExecutionStatus::EXCEPTION};
  if (HERMESVM_CRASH_TRACE &&
      (getVMExperimentFlags() & experiments::CrashTrace)) {
    res = Interpreter::interpretFunction<false, true>(*this, state);
  } else {
    res = Interpreter::interpretFunction<false, false>(*this, state);
  }

  // Interpreter::interpretFunction saves/restores its IP via the runtime
  // whenever a call out is made (see the CAPTURE_IP_* macros). Failing to
  // preserve the IP across calls to this function disrupts interpreter calls
  // further up the C++ callstack. Restore the IP in the runtime to the saved IP
  // of the caller before returning.
  // Note that we deliberately do not use a scope_exit for this, because we do
  // not want to attempt to retrieve the IP from the register stack during
  // unwinding of a C++ exception (if EXCEPTION_ON_OOM is enabled). Doing so
  // would be incorrect, because the register stack will not have been unwound.
  setCurrentIP(StackFramePtr(getStackPointer()).getSavedIP());
  return res;
}

CallResult<HermesValue> Runtime::interpretFunction(CodeBlock *newCodeBlock) {
  // Make sure we are not re-entering JS execution from a context that doesn't
  // allow reentrancy
  assert(this->noRJSLevel_ == 0 && "No JS execution allowed right now.");
  return interpretFunctionImpl(newCodeBlock);
}

#ifdef HERMES_ENABLE_DEBUGGER
ExecutionStatus Runtime::stepFunction(InterpreterState &state) {
  if (HERMESVM_CRASH_TRACE &&
      (getVMExperimentFlags() & experiments::CrashTrace))
    return Interpreter::interpretFunction<true, true>(*this, state).getStatus();
  else
    return Interpreter::interpretFunction<true, false>(*this, state)
        .getStatus();
}
#endif

template <bool SingleStep, bool EnableCrashTrace>
CallResult<HermesValue> Interpreter::interpretFunction(
    Runtime &runtime,
    InterpreterState &state) {
#ifndef HERMES_ENABLE_DEBUGGER
  static_assert(!SingleStep, "can't use single-step mode without the debugger");
#endif
  CodeBlock *curCodeBlock = state.codeBlock;
  const Inst *ip = nullptr;
  // Points to the first local register in the current frame.
  // This eliminates the indirect load from Runtime and the -1 offset.
  PinnedHermesValue *frameRegs;

// These CAPTURE_IP* macros should wrap around any major calls out of the
// interpreter loop. They stash and retrieve the IP via the current Runtime
// allowing the IP to be externally observed and even altered to change the flow
// of execution. Explicitly saving AND restoring the IP from the Runtime in this
// way means the C++ compiler will keep IP in a register within the rest of the
// interpreter loop.
//
// When assertions are enabled we take the extra step of "invalidating" the IP
// between captures so we can detect if it's erroneously accessed.
//
#ifdef NDEBUG

#define CAPTURE_IP(expr)    \
  runtime.setCurrentIP(ip); \
  (void)(expr);             \
  ip = runtime.getCurrentIP();

// Used when we want to declare a new variable and assign the expression to it.
#define CAPTURE_IP_ASSIGN(decl, expr) \
  runtime.setCurrentIP(ip);           \
  decl = (expr);                      \
  ip = runtime.getCurrentIP();

#else // !NDEBUG

#define CAPTURE_IP(expr)       \
  runtime.setCurrentIP(ip);    \
  (void)(expr);                \
  ip = runtime.getCurrentIP(); \
  runtime.invalidateCurrentIP();

// Used when we want to declare a new variable and assign the expression to it.
#define CAPTURE_IP_ASSIGN(decl, expr) \
  runtime.setCurrentIP(ip);           \
  decl = (expr);                      \
  ip = runtime.getCurrentIP();        \
  runtime.invalidateCurrentIP();

#endif // NDEBUG

/// The ENCODE_HV_AS_SHV macro is intended to be used when we need to convert
/// a HermesValue to a SmallHermesValue in interpreter code. It takes the
/// HermesValue \p src and declares a new SmallHermesValue with name \p dst that
/// contains the converted value. If boxed doubles are enabled, the conversion
/// may allocate, and this will perform a CAPTURE_IP accordingly.
#ifdef HERMESVM_BOXED_DOUBLES
// When boxed doubles are enabled, encodeHermesValue may allocate when a double
// is boxed, so we have to use CAPTURE_IP.
#define ENCODE_HV_AS_SHV(dst, src) \
  CAPTURE_IP_ASSIGN(               \
      SmallHermesValue dst, SmallHermesValue::encodeHermesValue(src, runtime))
#else
// When boxed doubles are disabled, SmallHermesValue is identical to
// HermesValue, but it is still technically a distinct type. So
// encodeHermesValue is effectively just a cast.
#define ENCODE_HV_AS_SHV(dst, src) \
  SmallHermesValue dst = SmallHermesValue::encodeHermesValue(src, runtime);
#endif

/// \def DONT_CAPTURE_IP(expr)
/// \param expr A call expression to a function external to the interpreter. The
///   expression should not make any allocations and the IP should be set
///   immediately following this macro.
#define DONT_CAPTURE_IP(expr)      \
  do {                             \
    NoAllocScope noAlloc(runtime); \
    (void)expr;                    \
  } while (false)

  LLVM_DEBUG(dbgs() << "interpretFunction() called\n");

  ScopedNativeDepthTracker depthTracker{runtime};
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }

  if (!SingleStep) {
#ifdef HERMESVM_JIT
    {
      JITCompiledFunctionPtr jitPtr = curCodeBlock->getJITCompiled();
      if (!jitPtr && runtime.jitContext_.shouldCompile(curCodeBlock)) {
        // JIT compilation may allocate on the GC heap, e.g. to populate
        // StringPrimitives in the IdentifierTable.
        auto *oldCurrentIP = runtime.getCurrentIP();
        // If there's a frame currently on the stack with a CodeBlock,
        // then use its first opcode as the IP.
        // Otherwise, use nullptr.
        if (runtime.getCurrentFrame() != *runtime.getStackFrames().end()) {
          runtime.setCurrentIP(
              runtime.getCurrentFrame().getCalleeCodeBlock()
                  ? (Inst const *)runtime.getCurrentFrame()
                        .getCalleeCodeBlock()
                        ->begin()
                  : nullptr);
        } else {
          runtime.setCurrentIP(nullptr);
        }
        jitPtr = runtime.jitContext_.compile(runtime, curCodeBlock);
        // Restore the currentIP field so nothing else is affected.
        runtime.setCurrentIP(oldCurrentIP);
      }
      if (jitPtr) {
        return JSFunction::_jittedCall(jitPtr, runtime);
      }
    }
#endif

    // Ensure the IP was correctly saved before we create the new frame.
    runtime.validateSavedIPBeforeCall();

    // Check for invalid invocation. This is done before setting up the stack so
    // the exception appears to come from the call site.
    auto newFrame = StackFramePtr(runtime.getStackPointer());
    bool isCtorCall = newFrame.isConstructorCall();
    if (LLVM_UNLIKELY(
            curCodeBlock->getHeaderFlags().isCallProhibited(isCtorCall))) {
      return runtime.raiseTypeError(
          isCtorCall ? "Function is not a constructor"
                     : "Class constructor invoked without new");
    }

    // Allocate the registers for the new frame. As with ProhibitInvoke, if this
    // fails, we want the exception to come from the call site.
    if (LLVM_UNLIKELY(!runtime.checkAndAllocStack(
            curCodeBlock->getFrameSize() +
            StackFrameLayout::CalleeExtraRegistersAtStart))) {
      return runtime.raiseStackOverflow(
          Runtime::StackOverflowKind::JSRegisterStack);
    }

    // Advance the frame pointer.
    runtime.setCurrentFrame(newFrame);
    // Point frameRegs to the first register in the new frame.
    frameRegs = &newFrame.getFirstLocalRef();
    ip = (Inst const *)curCodeBlock->begin();
  }

  GCScope gcScope(runtime);
  struct : public Locals {
    PinnedValue<> tmp;
  } lv;
  lv.tmp.clear();
  LocalsRAII lvraii(runtime, &lv);
  CallResult<HermesValue> res{ExecutionStatus::EXCEPTION};
  CallResult<PseudoHandle<>> resPH{ExecutionStatus::EXCEPTION};
  CallResult<Handle<Arguments>> resArgs{ExecutionStatus::EXCEPTION};
  CallResult<bool> boolRes{ExecutionStatus::EXCEPTION};
  // Start of the bytecode file, used to calculate IP offset in crash traces.
  const uint8_t *bytecodeFileStart;

  // Mark the gcScope so we can clear all allocated handles.
  // Remember how many handles the scope has so we can clear them in the loop.
  static constexpr unsigned KEEP_HANDLES = 0;
  assert(
      gcScope.getHandleCountDbg() == KEEP_HANDLES &&
      "scope has unexpected number of handles");

  INIT_OPCODE_PROFILER;

tailCall:
  PROFILER_ENTER_FUNCTION(curCodeBlock);

#ifdef HERMES_ENABLE_DEBUGGER
  runtime.getDebugger().willEnterCodeBlock(curCodeBlock);
#endif

  runtime.getCodeCoverageProfiler().markExecuted(curCodeBlock);

  // Update function executionCount_ count
  curCodeBlock->incrementExecutionCount();

#ifndef NDEBUG
  runtime.invalidateCurrentIP();
#endif

  if (!SingleStep) {
#ifndef NDEBUG
    LLVM_DEBUG(
        dbgs() << "function entry: stackLevel=" << runtime.getStackLevel()
               << ", argCount=" << runtime.getCurrentFrame().getArgCount()
               << ", frameSize=" << curCodeBlock->getFrameSize() << "\n");

    LLVM_DEBUG(
        dbgs() << " callee "
               << DumpHermesValue(
                      runtime.getCurrentFrame().getCalleeClosureOrCBRef())
               << "\n");
    LLVM_DEBUG(
        dbgs() << "   this "
               << DumpHermesValue(runtime.getCurrentFrame().getThisArgRef())
               << "\n");
    for (uint32_t i = 0; i != runtime.getCurrentFrame()->getArgCount(); ++i) {
      LLVM_DEBUG(
          dbgs() << "   " << llvh::format_decimal(i, 4) << " "
                 << DumpHermesValue(runtime.getCurrentFrame().getArgRef(i))
                 << "\n");
    }
#endif

  } else {
    // Point frameRegs to the first register in the frame.
    frameRegs = &runtime.getCurrentFrame().getFirstLocalRef();
    ip = (Inst const *)(curCodeBlock->begin() + state.offset);
  }

  assert((const uint8_t *)ip < curCodeBlock->end() && "CodeBlock is empty");

  INIT_STATE_FOR_CODEBLOCK(curCodeBlock);

#define BEFORE_OP_CODE                                                       \
  {                                                                          \
    UPDATE_OPCODE_TIME_SPENT;                                                \
    HERMES_SLOW_ASSERT(                                                      \
        curCodeBlock->contains(ip) && "curCodeBlock must contain ip");       \
    HERMES_SLOW_ASSERT((printDebugInfo(curCodeBlock, frameRegs, ip), true)); \
    HERMES_SLOW_ASSERT(                                                      \
        gcScope.getHandleCountDbg() == KEEP_HANDLES &&                       \
        "unaccounted handles were created");                                 \
    HERMES_SLOW_ASSERT(lv.tmp.isCleared() && "lv.tmp not cleared");          \
    RECORD_OPCODE_START_TIME;                                                \
    INC_OPCODE_COUNT;                                                        \
    if (EnableCrashTrace) {                                                  \
      runtime.crashTrace_.recordInst(                                        \
          (uint32_t)((const uint8_t *)ip - bytecodeFileStart), ip->opCode);  \
    }                                                                        \
  }

#ifdef HERMESVM_INDIRECT_THREADING
  static void *opcodeDispatch[] = {
#define DEFINE_OPCODE(name) &&case_##name,
#include "hermes/BCGen/HBC/BytecodeList.def"
      &&case__last};

#define CASE(name) case_##name:
// For indirect threading, there is no way to specify a default, leave it as
// an empty label.
#define DEFAULT_CASE
#define DISPATCH                                \
  BEFORE_OP_CODE;                               \
  if (SingleStep) {                             \
    runtime.setCurrentIP(ip);                   \
    state.codeBlock = curCodeBlock;             \
    state.offset = CUROFFSET;                   \
    return HermesValue::encodeUndefinedValue(); \
  }                                             \
  goto *opcodeDispatch[(unsigned)ip->opCode]

#else // HERMESVM_INDIRECT_THREADING

#define CASE(name) case OpCode::name:
#define DEFAULT_CASE default:
#define DISPATCH                                \
  if (SingleStep) {                             \
    runtime.setCurrentIP(ip);                   \
    state.codeBlock = curCodeBlock;             \
    state.offset = CUROFFSET;                   \
    return HermesValue::encodeUndefinedValue(); \
  }                                             \
  continue

#endif // HERMESVM_INDIRECT_THREADING

// This macro is used when we detect that either the Implicit or Explicit
// AsyncBreak flags have been set. It checks to see which one was requested and
// propagate the corresponding RunReason. If both Implicit and Explicit have
// been requested, then we'll propagate the RunReasons for both. Once for
// Implicit and once for Explicit.
#define RUN_DEBUGGER_ASYNC_BREAK(flags)                         \
  bool requestedImplicit = (uint8_t)(flags) &                   \
      (uint8_t)Runtime::AsyncBreakReasonBits::DebuggerImplicit; \
  bool requestedExplicit = (uint8_t)(flags) &                   \
      (uint8_t)Runtime::AsyncBreakReasonBits::DebuggerExplicit; \
  do {                                                          \
    if (requestedImplicit) {                                    \
      CAPTURE_IP_ASSIGN(                                        \
          auto dRes,                                            \
          runDebuggerUpdatingState(                             \
              Debugger::RunReason::AsyncBreakImplicit,          \
              runtime,                                          \
              curCodeBlock,                                     \
              ip,                                               \
              frameRegs));                                      \
      if (dRes == ExecutionStatus::EXCEPTION)                   \
        goto exception;                                         \
    }                                                           \
    if (requestedExplicit) {                                    \
      CAPTURE_IP_ASSIGN(                                        \
          auto dRes,                                            \
          runDebuggerUpdatingState(                             \
              Debugger::RunReason::AsyncBreakExplicit,          \
              runtime,                                          \
              curCodeBlock,                                     \
              ip,                                               \
              frameRegs));                                      \
      if (dRes == ExecutionStatus::EXCEPTION)                   \
        goto exception;                                         \
    }                                                           \
  } while (0)

  for (;;) {
    BEFORE_OP_CODE;

#ifdef HERMESVM_INDIRECT_THREADING
    goto *opcodeDispatch[(unsigned)ip->opCode];
#else
    switch (ip->opCode)
#endif
    {
      const Inst *nextIP;
      uint32_t idVal;
      bool tryProp;
      bool strictMode;
      uint32_t callArgCount;
      // This is HermesValue::getRaw(), since HermesValue cannot be assigned
      // to. It is meant to be used only for very short durations, in the
      // dispatch of call instructions, when there is definitely no possibility
      // of a GC.
      HermesValue::RawType callNewTarget;

/// Handle an opcode \p name with an out-of-line implementation in a function
///   ExecutionStatus caseName(
///       Runtime &,
///       PinnedHermesValue *frameRegs,
///       Inst *ip)
#define CASE_OUTOFLINE(name)                                         \
  CASE(name) {                                                       \
    CAPTURE_IP_ASSIGN(auto res, case##name(runtime, frameRegs, ip)); \
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {          \
      goto exception;                                                \
    }                                                                \
    gcScope.flushToSmallCount(KEEP_HANDLES);                         \
    ip = NEXTINST(name);                                             \
    DISPATCH;                                                        \
  }

/// Implement a binary arithmetic instruction with a fast path where both
/// operands are numbers.
/// \param name the name of the instruction. The fast path case will have a
///     "n" appended to the name.
#define BINOP(name)                                                        \
  CASE(name) {                                                             \
    if (LLVM_LIKELY(                                                       \
            _sh_ljs_are_both_non_nan_numbers(O2REG(name), O3REG(name)))) { \
      /* Fast-path. */                                                     \
      O1REG(name) = HermesValue::encodeTrustedNumberValue(                 \
          do##name(O2REG(name).getNumber(), O3REG(name).getNumber()));     \
      ip = NEXTINST(name);                                                 \
      DISPATCH;                                                            \
    }                                                                      \
    CAPTURE_IP_ASSIGN(                                                     \
        ExecutionStatus status,                                            \
        doOperSlowPath_RJS<do##name>(runtime, frameRegs, &ip->i##name));   \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION))               \
      goto exception;                                                      \
    gcScope.flushToSmallCount(KEEP_HANDLES);                               \
    ip = NEXTINST(name);                                                   \
    DISPATCH;                                                              \
  }                                                                        \
  CASE(name##N) {                                                          \
    O1REG(name) = HermesValue::encodeTrustedNumberValue(                   \
        do##name(O2REG(name).getNumber(), O3REG(name).getNumber()));       \
    ip = NEXTINST(name);                                                   \
    DISPATCH;                                                              \
  }

#define INCDECOP(name)                                                         \
  CASE(name) {                                                                 \
    if (LLVM_LIKELY(_sh_ljs_is_non_nan_number(O2REG(name)))) {                 \
      O1REG(name) = HermesValue::encodeTrustedNumberValue(                     \
          do##name(O2REG(name).getNumber()));                                  \
      ip = NEXTINST(name);                                                     \
      DISPATCH;                                                                \
    }                                                                          \
    CAPTURE_IP_ASSIGN(                                                         \
        ExecutionStatus status,                                                \
        doIncDecOperSlowPath_RJS<do##name>(runtime, frameRegs, &ip->i##name)); \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {                 \
      goto exception;                                                          \
    }                                                                          \
    gcScope.flushToSmallCount(KEEP_HANDLES);                                   \
    ip = NEXTINST(name);                                                       \
    DISPATCH;                                                                  \
  }

/// Implement a shift instruction with a fast path where both
/// operands are numbers.
/// \param name the name of the instruction.
#define SHIFTOP(name)                                                         \
  CASE(name) {                                                                \
    int32_t lhsInt, rhsInt;                                                   \
    if (LLVM_LIKELY(                                                          \
            _sh_ljs_tryfast_truncate_to_int32(O2REG(name), &lhsInt) &&        \
            _sh_ljs_tryfast_truncate_to_int32(O3REG(name), &rhsInt))) {       \
      /* Fast - path. */                                                      \
      uint32_t shiftAmt = rhsInt & 0x1f;                                      \
      O1REG(name) =                                                           \
          HermesValue::encodeTrustedNumberValue(do##name(lhsInt, shiftAmt));  \
      ip = NEXTINST(name);                                                    \
      DISPATCH;                                                               \
    }                                                                         \
    CAPTURE_IP_ASSIGN(                                                        \
        ExecutionStatus status,                                               \
        doShiftOperSlowPath_RJS<do##name>(runtime, frameRegs, &ip->i##name)); \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {                \
      goto exception;                                                         \
    }                                                                         \
    gcScope.flushToSmallCount(KEEP_HANDLES);                                  \
    ip = NEXTINST(name);                                                      \
    DISPATCH;                                                                 \
  }

/// Implement a binary bitwise instruction with a fast path where both
/// operands are numbers.
/// \param name the name of the instruction.
#define BITWISEBINOP(name)                                                  \
  CASE(name) {                                                              \
    int32_t lhsInt, rhsInt;                                                 \
    if (LLVM_LIKELY(                                                        \
            _sh_ljs_tryfast_truncate_to_int32(O2REG(name), &lhsInt) &&      \
            _sh_ljs_tryfast_truncate_to_int32(O3REG(name), &rhsInt))) {     \
      /* Fast-path. */                                                      \
      O1REG(name) =                                                         \
          HermesValue::encodeTrustedNumberValue(do##name(lhsInt, rhsInt));  \
      ip = NEXTINST(name);                                                  \
      DISPATCH;                                                             \
    }                                                                       \
    CAPTURE_IP_ASSIGN(                                                      \
        ExecutionStatus status,                                             \
        doBitOperSlowPath_RJS<do##name>(runtime, frameRegs, &ip->i##name)); \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {              \
      goto exception;                                                       \
    }                                                                       \
    gcScope.flushToSmallCount(KEEP_HANDLES);                                \
    ip = NEXTINST(name);                                                    \
    DISPATCH;                                                               \
  }

/// Implement a comparison instruction.
/// \param name the name of the instruction.
/// \param oper the C++ operator to use to actually perform the fast arithmetic
///     comparison.
/// \param operFuncName  function to call for the slow-path comparison.
#define CONDOP(name, oper, operFuncName)                                   \
  CASE(name) {                                                             \
    if (LLVM_LIKELY(                                                       \
            _sh_ljs_are_both_non_nan_numbers(O2REG(name), O3REG(name)))) { \
      /* Fast-path. */                                                     \
      O1REG(name) = HermesValue::encodeBoolValue(                          \
          O2REG(name).getNumber() oper O3REG(name).getNumber());           \
      ip = NEXTINST(name);                                                 \
      DISPATCH;                                                            \
    }                                                                      \
    CAPTURE_IP(                                                            \
        boolRes = operFuncName(                                            \
            runtime, Handle<>(&O2REG(name)), Handle<>(&O3REG(name))));     \
    if (boolRes == ExecutionStatus::EXCEPTION)                             \
      goto exception;                                                      \
    gcScope.flushToSmallCount(KEEP_HANDLES);                               \
    O1REG(name) = HermesValue::encodeBoolValue(boolRes.getValue());        \
    ip = NEXTINST(name);                                                   \
    DISPATCH;                                                              \
  }

/// Implement a comparison conditional jump with a fast path where both
/// operands are numbers.
/// \param name the name of the instruction.
/// \param suffix  Optional suffix to be added to the end (e.g. Long)
/// \param oper the C++ operator to use to actually perform the fast arithmetic
///     comparison.
/// \param operFuncName  function to call for the slow-path comparison.
/// \param trueDest  ip value if the conditional evaluates to true
/// \param falseDest  ip value if the conditional evaluates to false
#define JCOND_IMPL(name, suffix, oper, operFuncName, trueDest, falseDest) \
  CASE(name##suffix) {                                                    \
    if (LLVM_LIKELY(_sh_ljs_are_both_non_nan_numbers(                     \
            O2REG(name##suffix), O3REG(name##suffix)))) {                 \
      /* Fast-path. */                                                    \
      if (O2REG(name##suffix)                                             \
              .getNumber() oper O3REG(name##suffix)                       \
              .getNumber()) {                                             \
        ip = trueDest;                                                    \
        DISPATCH;                                                         \
      }                                                                   \
      ip = falseDest;                                                     \
      DISPATCH;                                                           \
    }                                                                     \
    CAPTURE_IP(                                                           \
        boolRes = operFuncName(                                           \
            runtime,                                                      \
            Handle<>(&O2REG(name##suffix)),                               \
            Handle<>(&O3REG(name##suffix))));                             \
    if (boolRes == ExecutionStatus::EXCEPTION)                            \
      goto exception;                                                     \
    gcScope.flushToSmallCount(KEEP_HANDLES);                              \
    if (boolRes.getValue()) {                                             \
      ip = trueDest;                                                      \
      DISPATCH;                                                           \
    }                                                                     \
    ip = falseDest;                                                       \
    DISPATCH;                                                             \
  }

/// Like JCOND_IMPL, but for cases where the operands are known to be numbers.
#define JCOND_N_IMPL(name, suffix, oper, operFuncName, trueDest, falseDest) \
  CASE(name##N##suffix) {                                                   \
    if (O2REG(name##suffix)                                                 \
            .getNumber() oper O3REG(name##suffix)                           \
            .getNumber()) {                                                 \
      ip = trueDest;                                                        \
      DISPATCH;                                                             \
    }                                                                       \
    ip = falseDest;                                                         \
    DISPATCH;                                                               \
  }

/// Implement a strict equality conditional jump
/// \param name the name of the instruction.
/// \param suffix  Optional suffix to be added to the end (e.g. Long)
/// \param trueDest  ip value if the conditional evaluates to true
/// \param falseDest  ip value if the conditional evaluates to false
#define JCOND_STRICT_EQ_IMPL(name, suffix, trueDest, falseDest)         \
  CASE(name##suffix) {                                                  \
    if (strictEqualityTest(O2REG(name##suffix), O3REG(name##suffix))) { \
      ip = trueDest;                                                    \
      DISPATCH;                                                         \
    }                                                                   \
    ip = falseDest;                                                     \
    DISPATCH;                                                           \
  }

/// Implement an equality conditional jump
/// \param name the name of the instruction.
/// \param suffix  Optional suffix to be added to the end (e.g. Long)
/// \param trueDest  ip value if the conditional evaluates to true
/// \param falseDest  ip value if the conditional evaluates to false
#define JCOND_EQ_IMPL(name, suffix, trueDest, falseDest) \
  CASE(name##suffix) {                                   \
    CAPTURE_IP_ASSIGN(                                   \
        auto eqRes,                                      \
        abstractEqualityTest_RJS(                        \
            runtime,                                     \
            Handle<>(&O2REG(name##suffix)),              \
            Handle<>(&O3REG(name##suffix))));            \
    if (eqRes == ExecutionStatus::EXCEPTION) {           \
      goto exception;                                    \
    }                                                    \
    gcScope.flushToSmallCount(KEEP_HANDLES);             \
    if (*eqRes) {                                        \
      ip = trueDest;                                     \
      DISPATCH;                                          \
    }                                                    \
    ip = falseDest;                                      \
    DISPATCH;                                            \
  }

/// Implement the long and short forms of a conditional jump, and its negation.
#define JCOND(name, oper, operFuncName, IMPL) \
  IMPL(                                       \
      J##name,                                \
      ,                                       \
      oper,                                   \
      operFuncName,                           \
      IPADD(ip->iJ##name.op1),                \
      NEXTINST(J##name));                     \
  IMPL(                                       \
      J##name,                                \
      Long,                                   \
      oper,                                   \
      operFuncName,                           \
      IPADD(ip->iJ##name##Long.op1),          \
      NEXTINST(J##name##Long));               \
  IMPL(                                       \
      JNot##name,                             \
      ,                                       \
      oper,                                   \
      operFuncName,                           \
      NEXTINST(JNot##name),                   \
      IPADD(ip->iJNot##name.op1));            \
  IMPL(                                       \
      JNot##name,                             \
      Long,                                   \
      oper,                                   \
      operFuncName,                           \
      NEXTINST(JNot##name##Long),             \
      IPADD(ip->iJNot##name##Long.op1));

/// Load a constant.
/// \param value is the value to store in the output register.
#define LOAD_CONST(name, value) \
  CASE(name) {                  \
    O1REG(name) = value;        \
    ip = NEXTINST(name);        \
    DISPATCH;                   \
  }

#define LOAD_CONST_CAPTURE_IP(name, value) \
  CASE(name) {                             \
    CAPTURE_IP(O1REG(name) = value);       \
    ip = NEXTINST(name);                   \
    DISPATCH;                              \
  }

      CASE(Mov) {
        O1REG(Mov) = O2REG(Mov);
        ip = NEXTINST(Mov);
        DISPATCH;
      }

      CASE(MovLong) {
        O1REG(MovLong) = O2REG(MovLong);
        ip = NEXTINST(MovLong);
        DISPATCH;
      }

      CASE(LoadParam) {
        if (LLVM_LIKELY(ip->iLoadParam.op2 <= FRAME.getArgCount())) {
          // index 0 must load 'this'. Index 1 the first argument, etc.
          O1REG(LoadParam) = FRAME.getArgRef((int32_t)ip->iLoadParam.op2 - 1);
          ip = NEXTINST(LoadParam);
          DISPATCH;
        }
        O1REG(LoadParam) = HermesValue::encodeUndefinedValue();
        ip = NEXTINST(LoadParam);
        DISPATCH;
      }

      CASE(LoadParamLong) {
        if (LLVM_LIKELY(ip->iLoadParamLong.op2 <= FRAME.getArgCount())) {
          // index 0 must load 'this'. Index 1 the first argument, etc.
          O1REG(LoadParamLong) =
              FRAME.getArgRef((int32_t)ip->iLoadParamLong.op2 - 1);
          ip = NEXTINST(LoadParamLong);
          DISPATCH;
        }
        O1REG(LoadParamLong) = HermesValue::encodeUndefinedValue();
        ip = NEXTINST(LoadParamLong);
        DISPATCH;
      }

      CASE(CoerceThisNS) {
        if (LLVM_LIKELY(O2REG(CoerceThisNS).isObject())) {
          O1REG(CoerceThisNS) = O2REG(CoerceThisNS);
        } else if (
            O2REG(CoerceThisNS).isNull() || O2REG(CoerceThisNS).isUndefined()) {
          O1REG(CoerceThisNS) = runtime.getGlobal().getHermesValue();
        } else {
          lv.tmp = O2REG(CoerceThisNS);
          nextIP = NEXTINST(CoerceThisNS);
          goto coerceThisSlowPath;
        }
        ip = NEXTINST(CoerceThisNS);
        DISPATCH;
      }
      CASE(LoadThisNS) {
        if (LLVM_LIKELY(FRAME.getThisArgRef().isObject())) {
          O1REG(LoadThisNS) = FRAME.getThisArgRef();
        } else if (
            FRAME.getThisArgRef().isNull() ||
            FRAME.getThisArgRef().isUndefined()) {
          O1REG(LoadThisNS) = runtime.getGlobal().getHermesValue();
        } else {
          lv.tmp = FRAME.getThisArgRef();
          nextIP = NEXTINST(LoadThisNS);
          goto coerceThisSlowPath;
        }
        ip = NEXTINST(LoadThisNS);
        DISPATCH;
      }
    coerceThisSlowPath: {
      CAPTURE_IP(res = toObject(runtime, lv.tmp));
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        goto exception;
      }
      O1REG(CoerceThisNS) = res.getValue();
      lv.tmp.clear();
      gcScope.flushToSmallCount(KEEP_HANDLES);
      ip = nextIP;
      DISPATCH;
    }

      CASE(CallWithNewTarget) {
        callArgCount = (uint32_t)ip->iCallWithNewTarget.op4;
        nextIP = NEXTINST(CallWithNewTarget);
        callNewTarget = O3REG(CallWithNewTarget).getRaw();
        goto doCall;
      }
      CASE(CallWithNewTargetLong) {
        callArgCount = O4REG(CallWithNewTarget).getNumber();
        nextIP = NEXTINST(CallWithNewTargetLong);
        callNewTarget = O3REG(CallWithNewTargetLong).getRaw();
        goto doCall;
      }

      // Note in Call1 through Call4, the first argument is 'this' which has
      // argument index -1.
      // Also note that we are writing to callNewTarget last, to avoid the
      // possibility of it being aliased by the arg writes.
      CASE(Call1) {
        callArgCount = 1;
        nextIP = NEXTINST(Call1);
        StackFramePtr fr{runtime.getStackPointer()};
        fr.getArgRefUnsafe(-1) = O3REG(Call1);
        callNewTarget = HermesValue::encodeUndefinedValue().getRaw();
        goto doCall;
      }

      CASE(Call2) {
        callArgCount = 2;
        nextIP = NEXTINST(Call2);
        StackFramePtr fr{runtime.getStackPointer()};
        fr.getArgRefUnsafe(-1) = O3REG(Call2);
        fr.getArgRefUnsafe(0) = O4REG(Call2);
        callNewTarget = HermesValue::encodeUndefinedValue().getRaw();
        goto doCall;
      }

      CASE(Call3) {
        callArgCount = 3;
        nextIP = NEXTINST(Call3);
        StackFramePtr fr{runtime.getStackPointer()};
        fr.getArgRefUnsafe(-1) = O3REG(Call3);
        fr.getArgRefUnsafe(0) = O4REG(Call3);
        fr.getArgRefUnsafe(1) = O5REG(Call3);
        callNewTarget = HermesValue::encodeUndefinedValue().getRaw();
        goto doCall;
      }

      CASE(Call4) {
        callArgCount = 4;
        nextIP = NEXTINST(Call4);
        StackFramePtr fr{runtime.getStackPointer()};
        fr.getArgRefUnsafe(-1) = O3REG(Call4);
        fr.getArgRefUnsafe(0) = O4REG(Call4);
        fr.getArgRefUnsafe(1) = O5REG(Call4);
        fr.getArgRefUnsafe(2) = O6REG(Call4);
        callNewTarget = HermesValue::encodeUndefinedValue().getRaw();
        goto doCall;
      }

      CASE(Construct) {
        callArgCount = (uint32_t)ip->iConstruct.op3;
        nextIP = NEXTINST(Construct);
        callNewTarget = O2REG(Construct).getRaw();
        goto doCall;
      }
      CASE(Call) {
        callArgCount = (uint32_t)ip->iCall.op3;
        nextIP = NEXTINST(Call);
        callNewTarget = HermesValue::encodeUndefinedValue().getRaw();
        goto doCall;
      }

    doCall: {
#ifdef HERMES_ENABLE_DEBUGGER
      // Check for an async debugger request.
      if (uint8_t asyncFlags =
              runtime.testAndClearDebuggerAsyncBreakRequest()) {
        RUN_DEBUGGER_ASYNC_BREAK(asyncFlags);
        gcScope.flushToSmallCount(KEEP_HANDLES);
        DISPATCH;
      }
#endif

      // Subtract 1 from callArgCount as 'this' is considered an argument in the
      // instruction, but not in the frame.
      auto newFrame = StackFramePtr::initFrame(
          runtime.getStackPointer(),
          FRAME,
          ip,
          curCodeBlock,
          /* SHLocals */ nullptr,
          callArgCount - 1,
          O2REG(Call),
          HermesValue::fromRaw(callNewTarget));
      (void)newFrame;

      SLOW_DEBUG(dumpCallArguments(dbgs(), runtime, newFrame));

      if (auto *func = dyn_vmcast<JSFunction>(O2REG(Call))) {
        assert(!SingleStep && "can't single-step a call");

#ifdef HERMES_MEMORY_INSTRUMENTATION
        runtime.pushCallStack(curCodeBlock, ip);
#endif

        CodeBlock *calleeBlock = func->getCodeBlock();
        if (LLVM_UNLIKELY(calleeBlock->isLazy())) {
          CAPTURE_IP_ASSIGN(
              auto res, calleeBlock->compileLazyFunction(runtime));
          if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
            goto exception;
          }
        }

#ifdef HERMESVM_JIT
        JITCompiledFunctionPtr jitPtr = calleeBlock->getJITCompiled();
        if (!jitPtr && runtime.jitContext_.shouldCompile(calleeBlock)) {
          // JIT compilation may allocate on the GC heap, e.g. to populate
          // StringPrimitives in the IdentifierTable.
          CAPTURE_IP_ASSIGN(
              jitPtr, runtime.jitContext_.compile(runtime, calleeBlock));
        }
        if (jitPtr) {
          CAPTURE_IP_ASSIGN(
              auto rres, JSFunction::_jittedCall(jitPtr, runtime));
          if (LLVM_UNLIKELY(rres == ExecutionStatus::EXCEPTION))
            goto exception;
          O1REG(Call) = *rres;
          SLOW_DEBUG(
              dbgs() << "JIT return value r" << (unsigned)ip->iCall.op1 << "="
                     << DumpHermesValue(O1REG(Call)) << "\n");
          ip = nextIP;
          DISPATCH;
        }
#endif

        // Check for invalid invocation.
        bool isCtorCall = !HermesValue::fromRaw(callNewTarget).isUndefined();
        if (LLVM_UNLIKELY(
                calleeBlock->getHeaderFlags().isCallProhibited(isCtorCall))) {
          CAPTURE_IP(runtime.raiseTypeError(
              isCtorCall ? "Function is not a constructor"
                         : "Class constructor invoked without new"));
          goto exception;
        }

        // Allocate the registers for the new frame.
        if (LLVM_UNLIKELY(!runtime.checkAndAllocStack(
                calleeBlock->getFrameSize() +
                StackFrameLayout::CalleeExtraRegistersAtStart))) {
          CAPTURE_IP(runtime.raiseStackOverflow(
              Runtime::StackOverflowKind::JSRegisterStack));
          goto exception;
        }

        // Advance the frame pointer.
        runtime.setCurrentFrame(newFrame);

        // Update the executing CodeBlock to the callee.
        curCodeBlock = calleeBlock;
        // Point frameRegs to the first register in the new frame.
        frameRegs = &newFrame.getFirstLocalRef();
        // Update the IP to the start of the callee.
        ip = (Inst const *)curCodeBlock->begin();

        goto tailCall;
      }
      CAPTURE_IP(
          resPH = Interpreter::handleCallSlowPath(runtime, &O2REG(Call)));
      if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
        goto exception;
      }
      O1REG(Call) = std::move(resPH->get());
      SLOW_DEBUG(
          dbgs() << "native return value r" << (unsigned)ip->iCall.op1 << "="
                 << DumpHermesValue(O1REG(Call)) << "\n");
      gcScope.flushToSmallCount(KEEP_HANDLES);
      ip = nextIP;
      DISPATCH;
    }

      CASE(CallRequire) {
        uint32_t modIndex = ip->iCallRequire.op3;
        RuntimeModule *runtimeModule = curCodeBlock->getRuntimeModule();
        HermesValue modExport = runtimeModule->getModuleExport(modIndex);
        if (LLVM_LIKELY(!modExport.isEmpty())) {
          // This is a cache hit.  Return the result obtained from the
          // cache.
          O1REG(CallRequire) = modExport;
        } else {
          CAPTURE_IP_ASSIGN(
              ExecutionStatus res,
              doCallRequireSlowPath_RJS(runtime, frameRegs, ip, runtimeModule));
          if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
            goto exception;
          gcScope.flushToSmallCount(KEEP_HANDLES);
        }
        ip = NEXTINST(CallRequire);
        DISPATCH;
      }

      CASE(GetBuiltinClosure) {
        uint8_t methodIndex = ip->iCallBuiltin.op2;
        Callable *closure = runtime.getBuiltinCallable(methodIndex);
        O1REG(GetBuiltinClosure) = HermesValue::encodeObjectValue(closure);
        ip = NEXTINST(GetBuiltinClosure);
        DISPATCH;
      }

      CASE(CallBuiltin) {
        CAPTURE_IP_ASSIGN(
            auto cres,
            implCallBuiltin(
                runtime, frameRegs, curCodeBlock, ip->iCallBuiltin.op3));
        if (LLVM_UNLIKELY(cres == ExecutionStatus::EXCEPTION))
          goto exception;
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(CallBuiltin);
        DISPATCH;
      }
      CASE(CallBuiltinLong) {
        CAPTURE_IP_ASSIGN(
            auto cres,
            implCallBuiltin(
                runtime, frameRegs, curCodeBlock, ip->iCallBuiltinLong.op3));
        if (LLVM_UNLIKELY(cres == ExecutionStatus::EXCEPTION))
          goto exception;
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(CallBuiltinLong);
        DISPATCH;
      }

      CASE(Ret) {
#ifdef HERMES_ENABLE_DEBUGGER
        // Check for an async debugger request, but skip it if we're single
        // stepping. The only case where we'd be single stepping a Ret is if it
        // was replaced with Debugger OpCode and we're coming here from
        // stepFunction(). This does take away a chance to handle AsyncBreak. An
        // AsyncBreak request could be either Explicit or Implicit. The Explicit
        // case is to have the program being executed to pause. There isn't a
        // need to pause at a particular location. Also, since we just came from
        // a breakpoint, handling Explicit AsyncBreak for single step isn't so
        // important. The other possible kind is an Implicit AsyncBreak, which
        // is used for debug clients to interrupt the runtime to execute their
        // own code. Not processing AsyncBreak just means that the Implicit
        // AsyncBreak needs to wait for the next opportunity to interrupt the
        // runtime, which should be fine. There is no contract for when the
        // interrupt should happen.
        if (!SingleStep) {
          if (uint8_t asyncFlags =
                  runtime.testAndClearDebuggerAsyncBreakRequest()) {
            RUN_DEBUGGER_ASYNC_BREAK(asyncFlags);
            gcScope.flushToSmallCount(KEEP_HANDLES);
            DISPATCH;
          }
        }
#endif

        PROFILER_EXIT_FUNCTION(curCodeBlock);

#ifdef HERMES_MEMORY_INSTRUMENTATION
        runtime.popCallStack();
#endif

        // Store the return value.
        res = O1REG(Ret);

        ip = FRAME.getSavedIP();
        curCodeBlock = FRAME.getSavedCodeBlock();

        frameRegs =
            &runtime.restoreStackAndPreviousFrame(FRAME).getFirstLocalRef();

        SLOW_DEBUG(
            dbgs() << "function exit: restored stackLevel="
                   << runtime.getStackLevel() << "\n");

        // Are we returning to native code?
        if (!curCodeBlock) {
          SLOW_DEBUG(dbgs() << "function exit: returning to native code\n");
          return res;
        }

        INIT_STATE_FOR_CODEBLOCK(curCodeBlock);
        O1REG(Call) = res.getValue();

#ifdef HERMES_ENABLE_DEBUGGER
        // Only do the more expensive check for breakpoint location (in
        // getRealOpCode) if there are breakpoints installed in the function
        // we're returning into.
        if (LLVM_UNLIKELY(curCodeBlock->getNumInstalledBreakpoints() > 0)) {
          ip = IPADD(
              inst::getInstSize(
                  runtime.debugger_.getRealOpCode(curCodeBlock, CUROFFSET)));
        } else {
          // No breakpoints in the function being returned to, just use
          // nextInstCall().
          ip = nextInstCall(ip);
        }
#else
        ip = nextInstCall(ip);
#endif

        DISPATCH;
      }

      CASE(Catch) {
        assert(!runtime.thrownValue_->isEmpty() && "Invalid thrown value");
        assert(
            !isUncatchableError(*runtime.thrownValue_) &&
            "Uncatchable thrown value was caught");
        O1REG(Catch) = *runtime.thrownValue_;
        runtime.clearThrownValue();
#ifdef HERMES_ENABLE_DEBUGGER
        // Signal to the debugger that we're done unwinding an exception,
        // and we can resume normal debugging flow.
        runtime.debugger_.finishedUnwindingException();
#endif
        ip = NEXTINST(Catch);
        DISPATCH;
      }

      CASE(Throw) {
        runtime.thrownValue_ = O1REG(Throw);
        SLOW_DEBUG(
            dbgs() << "Exception thrown: "
                   << DumpHermesValue(*runtime.thrownValue_) << "\n");
        goto exception;
      }

      CASE(ThrowIfEmpty) {
        if (LLVM_UNLIKELY(O2REG(ThrowIfEmpty).isEmpty())) {
          SLOW_DEBUG(dbgs() << "Throwing ReferenceError for empty variable");
          CAPTURE_IP(runtime.raiseReferenceError(
              "accessing an uninitialized variable"));
          goto exception;
        }
        O1REG(ThrowIfEmpty) = O2REG(ThrowIfEmpty);
        ip = NEXTINST(ThrowIfEmpty);
        DISPATCH;
      }

      CASE(ThrowIfUndefined) {
        if (LLVM_UNLIKELY(O2REG(ThrowIfUndefined).isUndefined())) {
          SLOW_DEBUG(
              dbgs() << "Throwing ReferenceError for undefined variable");
          CAPTURE_IP(runtime.raiseReferenceError(
              "accessing an uninitialized variable"));
          goto exception;
        }
        O1REG(ThrowIfUndefined) = O2REG(ThrowIfUndefined);
        ip = NEXTINST(ThrowIfUndefined);
        DISPATCH;
      }

      CASE(ThrowIfThisInitialized) {
        if (LLVM_UNLIKELY(!O1REG(ThrowIfThisInitialized).isEmpty())) {
          SLOW_DEBUG(
              dbgs() << "Throwing Error for already initialized subclass this");
          CAPTURE_IP(runtime.raiseReferenceError(
              "Cannot call super constructor twice"));
          goto exception;
        }
        ip = NEXTINST(ThrowIfThisInitialized);
        DISPATCH;
      }

      CASE(Debugger) {
        SLOW_DEBUG(dbgs() << "debugger statement executed\n");
#ifdef HERMES_ENABLE_DEBUGGER
        {
          if (!runtime.debugger_.isDebugging()) {
            // Only run the debugger if we're not already debugging.
            // Don't want to call it again and mess with its state.
            CAPTURE_IP_ASSIGN(
                auto res,
                runDebuggerUpdatingState(
                    Debugger::RunReason::Opcode,
                    runtime,
                    curCodeBlock,
                    ip,
                    frameRegs));
            if (res == ExecutionStatus::EXCEPTION) {
              // If one of the internal steps threw,
              // then handle that here by jumping to where we're supposed to go.
              // If we're in mid-step, the breakpoint at the catch point
              // will have been set by the debugger.
              // We don't want to execute this instruction because it's already
              // thrown.
              goto exception;
            }
          }
          InterpreterState newState{curCodeBlock, (uint32_t)CUROFFSET};
          ExecutionStatus status =
              runtime.debugger_.processInstUnderDebuggerOpCode(newState);
          if (status == ExecutionStatus::EXCEPTION) {
            goto exception;
          }

          if (newState.codeBlock != curCodeBlock ||
              newState.offset != (uint32_t)CUROFFSET) {
            ip = newState.codeBlock->getOffsetPtr(newState.offset);

            if (newState.codeBlock != curCodeBlock) {
              curCodeBlock = newState.codeBlock;
              INIT_STATE_FOR_CODEBLOCK(curCodeBlock);
              // Single-stepping should handle call stack management for us.
              frameRegs = &runtime.getCurrentFrame().getFirstLocalRef();
            }
          }
          gcScope.flushToSmallCount(KEEP_HANDLES);
        }
        DISPATCH;
#else
        ip = NEXTINST(Debugger);
        DISPATCH;
#endif
      }

      CASE(AsyncBreakCheck) {
        if (LLVM_UNLIKELY(runtime.hasAsyncBreak())) {
#ifdef HERMES_ENABLE_DEBUGGER
          if (uint8_t asyncFlags =
                  runtime.testAndClearDebuggerAsyncBreakRequest()) {
            RUN_DEBUGGER_ASYNC_BREAK(asyncFlags);
          }
#endif
          if (runtime.testAndClearTimeoutAsyncBreakRequest()) {
            CAPTURE_IP_ASSIGN(auto nRes, runtime.notifyTimeout());
            if (nRes == ExecutionStatus::EXCEPTION) {
              goto exception;
            }
          }
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);

        ip = NEXTINST(AsyncBreakCheck);
        DISPATCH;
      }

      CASE(ProfilePoint) {
#ifdef HERMESVM_PROFILER_BB
        auto pointIndex = ip->iProfilePoint.op1;
        SLOW_DEBUG(llvh::dbgs() << "ProfilePoint: " << pointIndex << "\n");
        CAPTURE_IP(runtime.getBasicBlockExecutionInfo().executeBlock(
            curCodeBlock, pointIndex));
#endif
        ip = NEXTINST(ProfilePoint);
        DISPATCH;
      }

      // Use a macro here to avoid clang-format issues with a literal default:
      // label.
      DEFAULT_CASE
      CASE(Unreachable) {
        hermes_fatal("Unreachable instruction encountered");
        // The fatal call doesn't return, no need to set the IP differently and
        // dispatch.
      }

      CASE(CreateBaseClass) {
        nextIP = NEXTINST(CreateBaseClass);
        goto createClass;
      }
      CASE(CreateBaseClassLongIndex) {
        nextIP = NEXTINST(CreateBaseClassLongIndex);
        goto createClass;
      }
      CASE(CreateDerivedClassLongIndex) {
        nextIP = NEXTINST(CreateDerivedClassLongIndex);
        goto createClass;
      }
      CASE(CreateDerivedClass) {
        nextIP = NEXTINST(CreateDerivedClass);
        goto createClass;
      }
    createClass: {
      CAPTURE_IP_ASSIGN(auto res, caseCreateClass(runtime, frameRegs));
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        goto exception;
      }
      gcScope.flushToSmallCount(KEEP_HANDLES);
      ip = nextIP;
      DISPATCH;
    }

      CASE(CreateClosure) {
        idVal = ip->iCreateClosure.op3;
        nextIP = NEXTINST(CreateClosure);
        goto createClosure;
      }
      CASE(CreateClosureLongIndex) {
        idVal = ip->iCreateClosureLongIndex.op3;
        nextIP = NEXTINST(CreateClosureLongIndex);
        goto createClosure;
      }
    createClosure: {
      auto *runtimeModule = curCodeBlock->getRuntimeModule();
      CAPTURE_IP(
          O1REG(CreateClosure) =
              JSFunction::createWithInferredParent(
                  runtime,
                  runtimeModule->getDomain(runtime),
                  O2REG(CreateClosure).isUndefined()
                      ? Runtime::makeNullHandle<Environment>()
                      : Handle<Environment>::vmcast(&O2REG(CreateClosure)),
                  runtimeModule->getCodeBlockMayAllocate(idVal))
                  .getHermesValue());
      gcScope.flushToSmallCount(KEEP_HANDLES);
      ip = nextIP;
      DISPATCH;
    }

      CASE(CreateGenerator) {
        CAPTURE_IP_ASSIGN(
            auto res,
            createGenerator_RJS(
                runtime,
                curCodeBlock->getRuntimeModule(),
                ip->iCreateGenerator.op3,
                O2REG(CreateGenerator).isUndefined()
                    ? Runtime::makeNullHandle<Environment>()
                    : Handle<Environment>::vmcast(&O2REG(CreateGenerator)),
                FRAME.getNativeArgs()));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(CreateGenerator) = res->getHermesValue();
        res->invalidate();
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(CreateGenerator);
        DISPATCH;
      }
      CASE(CreateGeneratorLongIndex) {
        CAPTURE_IP_ASSIGN(
            auto res,
            createGenerator_RJS(
                runtime,
                curCodeBlock->getRuntimeModule(),
                ip->iCreateGeneratorLongIndex.op3,
                Handle<Environment>::vmcast(&O2REG(CreateGeneratorLongIndex)),
                FRAME.getNativeArgs()));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(CreateGeneratorLongIndex) = res->getHermesValue();
        res->invalidate();
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(CreateGeneratorLongIndex);
        DISPATCH;
      }

      CASE(GetEnvironment) {
        Environment *curEnv = vmcast<Environment>(O2REG(GetEnvironment));
        for (unsigned level = ip->iGetEnvironment.op3; level; --level) {
          assert(curEnv && "invalid environment relative level");
          curEnv = curEnv->getParentEnvironment(runtime);
        }
        O1REG(GetEnvironment) = HermesValue::encodeObjectValue(curEnv);
        ip = NEXTINST(GetEnvironment);
        DISPATCH;
      }

      CASE(GetParentEnvironment) {
        // The currently executing function must exist, so get the environment.
        Environment *curEnv =
            FRAME.getCalleeClosureUnsafe()->getEnvironment(runtime);
        for (unsigned level = ip->iGetParentEnvironment.op2; level; --level) {
          assert(curEnv && "invalid environment relative level");
          curEnv = curEnv->getParentEnvironment(runtime);
        }
        O1REG(GetParentEnvironment) = HermesValue::encodeObjectValue(curEnv);
        ip = NEXTINST(GetParentEnvironment);
        DISPATCH;
      }
      CASE(GetClosureEnvironment) {
        O1REG(GetClosureEnvironment) = HermesValue::encodeObjectValue(
            vmcast<Callable>(O2REG(GetClosureEnvironment))
                ->getEnvironment(runtime));
        ip = NEXTINST(GetClosureEnvironment);
        DISPATCH;
      }

      CASE(CreateEnvironment) {
        CAPTURE_IP_ASSIGN(
            HermesValue envHV,
            Environment::create(
                runtime,
                Handle<Environment>::vmcast(&O2REG(CreateEnvironment)),
                ip->iCreateEnvironment.op3));

        O1REG(CreateEnvironment) = envHV;
#ifdef HERMES_ENABLE_DEBUGGER
        FRAME.getDebugEnvironmentRef() = envHV;
#endif
        ip = NEXTINST(CreateEnvironment);
        DISPATCH;
      }

      CASE(CreateFunctionEnvironment) {
        CAPTURE_IP_ASSIGN(
            Environment * env,
            Environment::create(
                runtime,
                FRAME.getCalleeClosureHandleUnsafe(),
                ip->iCreateFunctionEnvironment.op2));

        O1REG(CreateFunctionEnvironment) = HermesValue::encodeObjectValue(env);
#ifdef HERMES_ENABLE_DEBUGGER
        FRAME.getDebugEnvironmentRef() = O1REG(CreateFunctionEnvironment);
#endif
        lv.tmp.clear();
        ip = NEXTINST(CreateFunctionEnvironment);
        DISPATCH;
      }

      CASE(CreateTopLevelEnvironment) {
        CAPTURE_IP_ASSIGN(
            HermesValue envHV,
            Environment::create(
                runtime,
                Runtime::makeNullHandle<Environment>(),
                ip->iCreateTopLevelEnvironment.op2));

        O1REG(CreateTopLevelEnvironment) = envHV;
#ifdef HERMES_ENABLE_DEBUGGER
        FRAME.getDebugEnvironmentRef() = envHV;
#endif
        ip = NEXTINST(CreateTopLevelEnvironment);
        DISPATCH;
      }

      CASE(StoreToEnvironment) {
        vmcast<Environment>(O1REG(StoreToEnvironment))
            ->slot(ip->iStoreToEnvironment.op2)
            .set(O3REG(StoreToEnvironment), runtime.getHeap());
        ip = NEXTINST(StoreToEnvironment);
        DISPATCH;
      }
      CASE(StoreToEnvironmentL) {
        vmcast<Environment>(O1REG(StoreToEnvironmentL))
            ->slot(ip->iStoreToEnvironmentL.op2)
            .set(O3REG(StoreToEnvironmentL), runtime.getHeap());
        ip = NEXTINST(StoreToEnvironmentL);
        DISPATCH;
      }

      CASE(StoreNPToEnvironment) {
        vmcast<Environment>(O1REG(StoreNPToEnvironment))
            ->slot(ip->iStoreNPToEnvironment.op2)
            .setNonPtr(O3REG(StoreNPToEnvironment), runtime.getHeap());
        ip = NEXTINST(StoreNPToEnvironment);
        DISPATCH;
      }
      CASE(StoreNPToEnvironmentL) {
        vmcast<Environment>(O1REG(StoreNPToEnvironmentL))
            ->slot(ip->iStoreNPToEnvironmentL.op2)
            .setNonPtr(O3REG(StoreNPToEnvironmentL), runtime.getHeap());
        ip = NEXTINST(StoreNPToEnvironmentL);
        DISPATCH;
      }

      CASE(LoadFromEnvironment) {
        O1REG(LoadFromEnvironment) =
            vmcast<Environment>(O2REG(LoadFromEnvironment))
                ->slot(ip->iLoadFromEnvironment.op3);
        ip = NEXTINST(LoadFromEnvironment);
        DISPATCH;
      }

      CASE(LoadFromEnvironmentL) {
        O1REG(LoadFromEnvironmentL) =
            vmcast<Environment>(O2REG(LoadFromEnvironmentL))
                ->slot(ip->iLoadFromEnvironmentL.op3);
        ip = NEXTINST(LoadFromEnvironmentL);
        DISPATCH;
      }

      CASE(GetGlobalObject) {
        O1REG(GetGlobalObject) = runtime.getGlobal().getHermesValue();
        ip = NEXTINST(GetGlobalObject);
        DISPATCH;
      }

      CASE(GetNewTarget) {
        O1REG(GetNewTarget) = FRAME.getNewTargetRef();
        ip = NEXTINST(GetNewTarget);
        DISPATCH;
      }

      CASE(LoadParentNoTraps) {
        assert(
            !vmcast<JSObject>(O2REG(LoadParentNoTraps))
                 ->isProxyObject(runtime) &&
            "proxy is not supported");
        auto *parent =
            vmcast<JSObject>(O2REG(LoadParentNoTraps))->getParent(runtime);
        O1REG(LoadParentNoTraps) = parent
            ? HermesValue::encodeObjectValue(parent)
            : HermesValue::encodeNullValue();
        ip = NEXTINST(LoadParentNoTraps);
        DISPATCH;
      }

      CASE(DeclareGlobalVar) {
        CAPTURE_IP_ASSIGN(
            auto res, declareGlobalVarImpl(runtime, curCodeBlock, ip));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        ip = NEXTINST(DeclareGlobalVar);
        DISPATCH;
      }

#ifndef NDEBUG
#define INC_NUMGETBYIDDICT                                               \
  if (vmcast<HiddenClass>(clazzPtr.getNonNull(runtime))->isDictionary()) \
    ++NumGetByIdDict;
#else
#define INC_NUMGETBYIDDICT (void)NumGetByIdDict;
#endif

#define GET_BY_ID_IMPL(name)                                                  \
  ++NumGetById;                                                               \
  if (LLVM_LIKELY(O2REG(name).isObject())) {                                  \
    auto *obj = vmcast<JSObject>(O2REG(name));                                \
    auto cacheIdx = ip->i##name.op3;                                          \
    auto *cacheEntry = curCodeBlock->getReadCacheEntry(cacheIdx);             \
    CompressedPointer clazzPtr{obj->getClassGCPtr()};                         \
    INC_NUMGETBYIDDICT;                                                       \
    /* If we have a cache hit, reuse the cached offset and immediately return \
     * the property. */                                                       \
    if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {                         \
      ++NumGetByIdCacheHits;                                                  \
      O1REG(name) = JSObject::getNamedSlotValueUnsafe(                        \
                        obj, runtime, cacheEntry->getSlot())                  \
                        .unboxToHV(runtime);                                  \
      ip = NEXTINST(name);                                                    \
      DISPATCH;                                                               \
    }                                                                         \
    if (LLVM_LIKELY(cacheEntry->negMatchClazz == clazzPtr)) {                 \
      /* Proxy, HostObject and lazy objects have special hidden classes, so   \
       * they should never match the cached class. */                         \
      assert(!obj->isProxyObject(runtime));                                   \
      assert(!obj->isHostObject(runtime));                                    \
      assert(!obj->isLazy(runtime));                                          \
      const GCPointer<JSObject> &parentGCPtr = obj->getParentGCPtr(runtime);  \
      if (LLVM_LIKELY(parentGCPtr)) {                                         \
        JSObject *parent = parentGCPtr.getNonNull(runtime);                   \
        if (LLVM_LIKELY(cacheEntry->clazz == parent->getClassGCPtr())) {      \
          ++NumGetByIdProtoHits;                                              \
          /* We've already checked that this isn't a Proxy. */                \
          O1REG(name) = JSObject::getNamedSlotValueUnsafe(                    \
                            parent, runtime, cacheEntry->getSlot())           \
                            .unboxToHV(runtime);                              \
          ip = NEXTINST(name);                                                \
          DISPATCH;                                                           \
        }                                                                     \
      }                                                                       \
    }                                                                         \
  }                                                                           \
  CAPTURE_IP_ASSIGN(                                                          \
      ExecutionStatus res,                                                    \
      doGetByIdSlowPath_RJS(                                                  \
          runtime, frameRegs, ip, curCodeBlock, idVal, tryProp));             \
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))                       \
    goto exception;                                                           \
  gcScope.flushToSmallCount(KEEP_HANDLES);                                    \
  ip = NEXTINST(name);                                                        \
  DISPATCH;

      CASE(TryGetByIdLong) {
        tryProp = true;
        idVal = ip->iTryGetByIdLong.op4;
        GET_BY_ID_IMPL(TryGetByIdLong);
      }
      CASE(GetByIdLong) {
        tryProp = false;
        idVal = ip->iGetByIdLong.op4;
        GET_BY_ID_IMPL(GetByIdLong);
      }
      CASE(GetByIdShort) {
        tryProp = false;
        idVal = ip->iGetByIdShort.op4;
        GET_BY_ID_IMPL(GetByIdShort);
      }
      CASE(TryGetById) {
        tryProp = true;
        idVal = ip->iTryGetById.op4;
        GET_BY_ID_IMPL(TryGetById);
      }
      CASE(GetById) {
        tryProp = false;
        idVal = ip->iGetById.op4;
        GET_BY_ID_IMPL(GetById);
      }

#undef INC_NUMGETBYIDDICT
#undef GET_BY_ID_IMPL

      CASE(GetByIdWithReceiverLong) {
        if (LLVM_LIKELY(O2REG(GetByIdWithReceiverLong).isObject())) {
          auto *obj = vmcast<JSObject>(O2REG(GetByIdWithReceiverLong));
          auto cacheIdx = ip->iGetByIdWithReceiverLong.op3;
          auto *cacheEntry = curCodeBlock->getReadCacheEntry(cacheIdx);
          CompressedPointer clazzPtr{obj->getClassGCPtr()};
          // If we have a cache hit, reuse the cached offset and immediately
          // return the property.
          if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {
            ++NumGetByIdCacheHits;
            O1REG(GetByIdWithReceiverLong) =
                JSObject::getNamedSlotValueUnsafe(
                    obj, runtime, cacheEntry->getSlot())
                    .unboxToHV(runtime);
            ip = NEXTINST(GetByIdWithReceiverLong);
            DISPATCH;
          }
        }
        CAPTURE_IP_ASSIGN(
            ExecutionStatus res,
            doGetByIdWithReceiverSlowPath_RJS(
                runtime, frameRegs, ip, curCodeBlock));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
          goto exception;
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(GetByIdWithReceiverLong);
        DISPATCH;
      }

      CASE(TryPutByIdLooseLong) {
        tryProp = true;
        strictMode = false;
        idVal = ip->iTryPutByIdLooseLong.op4;
        nextIP = NEXTINST(TryPutByIdLooseLong);
        goto putById;
      }
      CASE(TryPutByIdStrictLong) {
        tryProp = true;
        strictMode = true;
        idVal = ip->iTryPutByIdStrictLong.op4;
        nextIP = NEXTINST(TryPutByIdStrictLong);
        goto putById;
      }
      CASE(PutByIdLooseLong) {
        tryProp = false;
        strictMode = false;
        idVal = ip->iPutByIdLooseLong.op4;
        nextIP = NEXTINST(PutByIdLooseLong);
        goto putById;
      }
      CASE(PutByIdStrictLong) {
        tryProp = false;
        strictMode = true;
        idVal = ip->iPutByIdStrictLong.op4;
        nextIP = NEXTINST(PutByIdStrictLong);
        goto putById;
      }
      CASE(TryPutByIdLoose) {
        tryProp = true;
        strictMode = false;
        idVal = ip->iTryPutByIdLoose.op4;
        nextIP = NEXTINST(TryPutByIdLoose);
        goto putById;
      }
      CASE(TryPutByIdStrict) {
        tryProp = true;
        strictMode = true;
        idVal = ip->iTryPutByIdStrict.op4;
        nextIP = NEXTINST(TryPutByIdStrict);
        goto putById;
      }
      CASE(PutByIdLoose) {
        tryProp = false;
        strictMode = false;
        idVal = ip->iPutByIdLoose.op4;
        nextIP = NEXTINST(PutByIdLoose);
        goto putById;
      }
      CASE(PutByIdStrict) {
        tryProp = false;
        strictMode = true;
        idVal = ip->iPutByIdStrict.op4;
        nextIP = NEXTINST(PutByIdStrict);
      }
    putById: {
      ++NumPutById;
      if (LLVM_LIKELY(O1REG(PutByIdLoose).isObject())) {
        ENCODE_HV_AS_SHV(shv, O2REG(PutByIdLoose));
        auto *obj = vmcast<JSObject>(O1REG(PutByIdLoose));
        auto cacheIdx = ip->iPutByIdLoose.op3;
        auto *cacheEntry = curCodeBlock->getWriteCacheEntry(cacheIdx);

        CompressedPointer clazzPtr{obj->getClassGCPtr()};
        // If we have a cache hit, reuse the cached offset and immediately
        // return the property.
        if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {
          ++NumPutByIdCacheHits;
          JSObject::setNamedSlotValueUnsafe(
              obj, runtime, cacheEntry->getSlot(), shv);
          ip = nextIP;
          DISPATCH;
        }

        // Now check against the AddPropertyCacheEntry to ensure we can still
        // use the cached information.
        // NOTE: Need to check resultClazz in all cases because it's a
        // weak reference that may have been freed even if the add cache is
        // valid.
        const auto &addCacheEntry =
            curCodeBlock->getRuntimeModule()->getAddCacheEntry(
                cacheEntry->getAddCacheIndex());
        if (LLVM_LIKELY(addCacheEntry.startClazz == clazzPtr) &&
            LLVM_LIKELY(addCacheEntry.resultClazz) &&
            LLVM_LIKELY(
                addCacheEntry.getParentEpoch() ==
                runtime.getParentCacheEpoch()) &&
            LLVM_LIKELY(addCacheEntry.parent == obj->getParentGCPtr(runtime))) {
          HiddenClass *resultClazz =
              addCacheEntry.resultClazz.getNonNull(runtime, runtime.getHeap());
          ++NumPutByIdTransitionHits;
          CAPTURE_IP(
              JSObject::addNewOwnPropertyInSlot(
                  obj, runtime, resultClazz, addCacheEntry.getSlot(), shv));
          ip = nextIP;
          DISPATCH;
        }
      }
      ++NumPutByIdSlowPaths;
      CAPTURE_IP_ASSIGN(
          ExecutionStatus res,
          doPutByIdSlowPath_RJS(
              runtime,
              frameRegs,
              ip,
              curCodeBlock,
              idVal,
              strictMode,
              tryProp));
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
        goto exception;
      gcScope.flushToSmallCount(KEEP_HANDLES);
      ip = nextIP;
      DISPATCH;
    }

      CASE(GetByIndex) {
        ++NumGetByIndex;
        if (LLVM_LIKELY(O2REG(GetByIndex).isObject())) {
          if (auto optRes = tryFastGetComputedNoAlloc(
                  runtime,
                  vmcast<JSObject>(O2REG(GetByIndex)),
                  ip->iGetByIndex.op3)) {
            O1REG(GetByIndex) = *optRes;
            ip = NEXTINST(GetByIndex);
            DISPATCH;
          }
        }
        // Otherwise...
        // This is the "slow path".
        lv.tmp = HermesValue::encodeTrustedNumberValue(ip->iGetByIndex.op3);
        CAPTURE_IP(
            resPH = Interpreter::getByValTransient_RJS(
                runtime, Handle<>(&O2REG(GetByIndex)), lv.tmp));
        lv.tmp.clear();
        if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        O1REG(GetByIndex) = resPH->get();
        ip = NEXTINST(GetByIndex);
        DISPATCH;
      }

      CASE(GetByVal) {
        ++NumGetByVal;

        // An efficient check whether the value is both a number and a uint32.
        static_assert(
            HERMESVALUE_VERSION == 2,
            "HermesValue must use NaN-encoding for non-numbers");
        uint32_t index;
        if (sh_tryfast_f64_to_u32(O3REG(GetByVal).f64, index) &&
            LLVM_LIKELY(O2REG(GetByVal).isObject())) {
          ++NumGetByValObjInd;
          if (HERMES_SLOW_STATISTIC_ENABLED) {
            if (vmisa<ArrayImpl>(O2REG(GetByVal)))
              ++NumGetByValArrInd;
            else if (vmisa<JSTypedArrayBase>(O2REG(GetByVal)))
              ++NumGetByValTAInd;
          }

          if (auto optRes = tryFastGetComputedNoAlloc(
                  runtime, vmcast<JSObject>(O2REG(GetByVal)), index)) {
            ++NumGetByValObjIndFast;
            O1REG(GetByVal) = *optRes;
            ip = NEXTINST(GetByVal);
            DISPATCH;
          }
        }

        CAPTURE_IP_ASSIGN(
            ExecutionStatus res, caseGetByVal(runtime, frameRegs, ip));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
          goto exception;
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(GetByVal);
        DISPATCH;
      }

      CASE_OUTOFLINE(GetByValWithReceiver);

      CASE(DefineOwnById) {
        assert(
            O1REG(DefineOwnById).isObject() &&
            "PutOwn requires object operand");
        ENCODE_HV_AS_SHV(shv, O2REG(DefineOwnById));
        auto *obj = vmcast<JSObject>(O1REG(DefineOwnById));
        auto cacheIdx = ip->iDefineOwnById.op3;
        auto *cacheEntry = curCodeBlock->getWriteCacheEntry(cacheIdx);
        CompressedPointer clazzPtr{obj->getClassGCPtr()};
        // If we have a cache hit, reuse the cached offset and immediately
        // write the property.
        if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {
          JSObject::setNamedSlotValueUnsafe(
              obj, runtime, cacheEntry->getSlot(), shv);
          ip = NEXTINST(DefineOwnById);
          DISPATCH;
        }
        CAPTURE_IP_ASSIGN(
            ExecutionStatus status,
            defineOwnByIdSlowPath(runtime, shv, curCodeBlock, frameRegs, ip));
        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(DefineOwnById);
        DISPATCH;
      }
      CASE(DefineOwnByIdLong) {
        assert(
            O1REG(DefineOwnByIdLong).isObject() &&
            "PutOwn requires object operand");
        CAPTURE_IP_ASSIGN(
            SmallHermesValue shv,
            SmallHermesValue::encodeHermesValue(
                O2REG(DefineOwnByIdLong), runtime));
        auto *obj = vmcast<JSObject>(O1REG(DefineOwnByIdLong));
        auto cacheIdx = ip->iDefineOwnByIdLong.op3;
        auto *cacheEntry = curCodeBlock->getWriteCacheEntry(cacheIdx);
        CompressedPointer clazzPtr{obj->getClassGCPtr()};
        // If we have a cache hit, reuse the cached offset and immediately
        // write the property.
        if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {
          JSObject::setNamedSlotValueUnsafe(
              obj, runtime, cacheEntry->getSlot(), shv);
          ip = NEXTINST(DefineOwnByIdLong);
          DISPATCH;
        }
        CAPTURE_IP_ASSIGN(
            ExecutionStatus status,
            defineOwnByIdSlowPath(runtime, shv, curCodeBlock, frameRegs, ip));
        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(DefineOwnByIdLong);
        DISPATCH;
      }

      CASE(PutByValLoose)
      CASE(PutByValStrict) {
        CAPTURE_IP_ASSIGN(
            ExecutionStatus status, casePutByVal(runtime, frameRegs, ip));
        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION))
          goto exception;
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(PutByValLoose);
        DISPATCH;
      }

      CASE_OUTOFLINE(PutByValWithReceiver);

      CASE(DefineOwnByIndexL) {
        nextIP = NEXTINST(DefineOwnByIndexL);
        idVal = ip->iDefineOwnByIndexL.op3;
        goto DefineOwnByIndex;
      }
      CASE(DefineOwnByIndex) {
        nextIP = NEXTINST(DefineOwnByIndex);
        idVal = ip->iDefineOwnByIndex.op3;
      }
    DefineOwnByIndex: {
      lv.tmp = HermesValue::encodeTrustedNumberValue(idVal);
      CAPTURE_IP(
          JSObject::defineOwnComputedPrimitive(
              Handle<JSObject>::vmcast(&O1REG(DefineOwnByIndex)),
              runtime,
              lv.tmp,
              DefinePropertyFlags::getDefaultNewPropertyFlags(),
              Handle<>(&O2REG(DefineOwnByIndex)),
              PropOpFlags().plusThrowOnError()));
      gcScope.flushToSmallCount(KEEP_HANDLES);
      lv.tmp.clear();
      ip = nextIP;
      DISPATCH;
    }

      CASE(DefineOwnInDenseArray) {
        // This instruction only makes sense if we know that the ArrayStorage is
        // the same size as it was when it was created.
        assert(JSArray::vt.trimSize_ == nullptr && "trimming must be disabled");
        ENCODE_HV_AS_SHV(shv, O2REG(DefineOwnInDenseArray));
        JSArray::unsafeSetExistingElementAt(
            vmcast<JSArray>(O1REG(DefineOwnInDenseArray)),
            runtime,
            ip->iDefineOwnInDenseArray.op3,
            shv);
        ip = NEXTINST(DefineOwnInDenseArray);
        DISPATCH;
      }
      CASE(DefineOwnInDenseArrayL) {
        // This instruction only makes sense if we know that the ArrayStorage is
        // the same size as it was when it was created.
        assert(JSArray::vt.trimSize_ == nullptr && "trimming must be disabled");
        ENCODE_HV_AS_SHV(shv, O2REG(DefineOwnInDenseArrayL));
        JSArray::unsafeSetExistingElementAt(
            vmcast<JSArray>(O1REG(DefineOwnInDenseArrayL)),
            runtime,
            ip->iDefineOwnInDenseArrayL.op3,
            shv);
        ip = NEXTINST(DefineOwnInDenseArrayL);
        DISPATCH;
      }

      CASE_OUTOFLINE(GetPNameList);
      CASE_OUTOFLINE(GetNextPName);

      CASE(ToPropertyKey) {
        {
          CAPTURE_IP_ASSIGN(
              auto res,
              toPropertyKey(runtime, Handle<>(&O2REG(ToPropertyKey))));
          if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
            goto exception;
          }
          O1REG(ToPropertyKey) = res->getHermesValue();
          gcScope.flushToSmallCount(KEEP_HANDLES);
        }
        ip = NEXTINST(ToPropertyKey);
        DISPATCH;
      }

      CASE(ToNumber) {
        if (LLVM_LIKELY(O2REG(ToNumber).isNumber())) {
          O1REG(ToNumber) = O2REG(ToNumber);
          ip = NEXTINST(ToNumber);
        } else {
          CAPTURE_IP(res = toNumber_RJS(runtime, Handle<>(&O2REG(ToNumber))));
          if (res == ExecutionStatus::EXCEPTION)
            goto exception;
          gcScope.flushToSmallCount(KEEP_HANDLES);
          O1REG(ToNumber) = res.getValue();
          ip = NEXTINST(ToNumber);
        }
        DISPATCH;
      }

      CASE(ToNumeric) {
        if (LLVM_LIKELY(O2REG(ToNumeric).isNumber())) {
          O1REG(ToNumeric) = O2REG(ToNumeric);
          ip = NEXTINST(ToNumeric);
        } else {
          CAPTURE_IP(res = toNumeric_RJS(runtime, Handle<>(&O2REG(ToNumeric))));
          if (res == ExecutionStatus::EXCEPTION)
            goto exception;
          gcScope.flushToSmallCount(KEEP_HANDLES);
          O1REG(ToNumeric) = res.getValue();
          ip = NEXTINST(ToNumeric);
        }
        DISPATCH;
      }

      CASE(ToInt32) {
        int32_t argInt;
        if (LLVM_LIKELY(
                _sh_ljs_tryfast_truncate_to_int32(O2REG(ToInt32), &argInt))) {
          /* Fast-path. */
          O1REG(ToInt32) = HermesValue::encodeTrustedNumberValue(argInt);
        } else {
          CAPTURE_IP(res = toInt32_RJS(runtime, Handle<>(&O2REG(ToInt32))));
          if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
            goto exception;
          gcScope.flushToSmallCount(KEEP_HANDLES);
          O1REG(ToInt32) = res.getValue();
        }
        ip = NEXTINST(ToInt32);
        DISPATCH;
      }

      CASE(ToUint32) {
        uint32_t argInt;
        if (LLVM_LIKELY(
                _sh_ljs_tryfast_truncate_to_uint32(O2REG(ToUint32), &argInt))) {
          /* Fast-path. */
          O1REG(ToUint32) = HermesValue::encodeTrustedNumberValue(argInt);
        } else {
          CAPTURE_IP(res = toUInt32_RJS(runtime, Handle<>(&O2REG(ToUint32))));
          if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
            goto exception;
          gcScope.flushToSmallCount(KEEP_HANDLES);
          O1REG(ToUint32) = res.getValue();
        }
        ip = NEXTINST(ToUint32);
        DISPATCH;
      }

      CASE(AddEmptyString) {
        if (LLVM_LIKELY(O2REG(AddEmptyString).isString())) {
          O1REG(AddEmptyString) = O2REG(AddEmptyString);
          ip = NEXTINST(AddEmptyString);
        } else {
          CAPTURE_IP(
              res = toPrimitive_RJS(
                  runtime,
                  Handle<>(&O2REG(AddEmptyString)),
                  PreferredType::NONE));
          if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
            goto exception;
          lv.tmp = res.getValue();
          CAPTURE_IP_ASSIGN(auto strRes, toString_RJS(runtime, lv.tmp));
          if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION))
            goto exception;
          lv.tmp.clear();
          gcScope.flushToSmallCount(KEEP_HANDLES);
          O1REG(AddEmptyString) = strRes->getHermesValue();
          ip = NEXTINST(AddEmptyString);
        }
        DISPATCH;
      }

      CASE_OUTOFLINE(CreatePrivateName);

      CASE(Jmp) {
        ip = IPADD(ip->iJmp.op1);
        DISPATCH;
      }
      CASE(JmpLong) {
        ip = IPADD(ip->iJmpLong.op1);
        DISPATCH;
      }
      CASE(JmpTrue) {
        if (toBoolean(O2REG(JmpTrue)))
          ip = IPADD(ip->iJmpTrue.op1);
        else
          ip = NEXTINST(JmpTrue);
        DISPATCH;
      }
      CASE(JmpTrueLong) {
        if (toBoolean(O2REG(JmpTrueLong)))
          ip = IPADD(ip->iJmpTrueLong.op1);
        else
          ip = NEXTINST(JmpTrueLong);
        DISPATCH;
      }
      CASE(JmpFalse) {
        if (!toBoolean(O2REG(JmpFalse)))
          ip = IPADD(ip->iJmpFalse.op1);
        else
          ip = NEXTINST(JmpFalse);
        DISPATCH;
      }
      CASE(JmpFalseLong) {
        if (!toBoolean(O2REG(JmpFalseLong)))
          ip = IPADD(ip->iJmpFalseLong.op1);
        else
          ip = NEXTINST(JmpFalseLong);
        DISPATCH;
      }
      CASE(JmpUndefined) {
        if (O2REG(JmpUndefined).isUndefined())
          ip = IPADD(ip->iJmpUndefined.op1);
        else
          ip = NEXTINST(JmpUndefined);
        DISPATCH;
      }
      CASE(JmpUndefinedLong) {
        if (O2REG(JmpUndefinedLong).isUndefined())
          ip = IPADD(ip->iJmpUndefinedLong.op1);
        else
          ip = NEXTINST(JmpUndefinedLong);
        DISPATCH;
      }
      CASE(JmpBuiltinIs) {
        if (O3REG(JmpBuiltinIs).getRaw() ==
            HermesValue::encodeObjectValue(
                runtime.getBuiltinCallable(ip->iJmpBuiltinIs.op2))
                .getRaw()) {
          ip = IPADD(ip->iJmpBuiltinIs.op1);
        } else {
          ip = NEXTINST(JmpBuiltinIs);
        }
        DISPATCH;
      }
      CASE(JmpBuiltinIsLong) {
        if (O3REG(JmpBuiltinIsLong).getRaw() ==
            HermesValue::encodeObjectValue(
                runtime.getBuiltinCallable(ip->iJmpBuiltinIsLong.op2))
                .getRaw()) {
          ip = IPADD(ip->iJmpBuiltinIsLong.op1);
        } else {
          ip = NEXTINST(JmpBuiltinIsLong);
        }
        DISPATCH;
      }
      CASE(JmpBuiltinIsNot) {
        if (O3REG(JmpBuiltinIsNot).getRaw() !=
            HermesValue::encodeObjectValue(
                runtime.getBuiltinCallable(ip->iJmpBuiltinIsNot.op2))
                .getRaw()) {
          ip = IPADD(ip->iJmpBuiltinIsNot.op1);
        } else {
          ip = NEXTINST(JmpBuiltinIsNot);
        }
        DISPATCH;
      }
      CASE(JmpBuiltinIsNotLong) {
        if (O3REG(JmpBuiltinIsNotLong).getRaw() !=
            HermesValue::encodeObjectValue(
                runtime.getBuiltinCallable(ip->iJmpBuiltinIsNotLong.op2))
                .getRaw()) {
          ip = IPADD(ip->iJmpBuiltinIsNotLong.op1);
        } else {
          ip = NEXTINST(JmpBuiltinIsNotLong);
        }
        DISPATCH;
      }
      INCDECOP(Inc)
      INCDECOP(Dec)
      CASE(AddN) {
        O1REG(Add) = HermesValue::encodeTrustedNumberValue(
            O2REG(Add).getNumber() + O3REG(Add).getNumber());
        ip = NEXTINST(Add);
        DISPATCH;
      }
      CASE(Add) {
        if (LLVM_LIKELY(_sh_ljs_are_both_non_nan_numbers(
                O2REG(Add), O3REG(Add)))) { /* Fast-path. */
          O1REG(Add) = HermesValue::encodeTrustedNumberValue(
              O2REG(Add).getNumber() + O3REG(Add).getNumber());
          ip = NEXTINST(Add);
          DISPATCH;
        }
        CAPTURE_IP(
            res = addOp_RJS(
                runtime, Handle<>(&O2REG(Add)), Handle<>(&O3REG(Add))));
        if (res == ExecutionStatus::EXCEPTION) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        O1REG(Add) = res.getValue();
        ip = NEXTINST(Add);
        DISPATCH;
      }
      CASE(AddS) {
        CAPTURE_IP(
            res = StringPrimitive::concat(
                runtime,
                Handle<StringPrimitive>::vmcast(&O2REG(AddS)),
                Handle<StringPrimitive>::vmcast(&O3REG(AddS))));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        O1REG(AddS) = res.getValue();
        ip = NEXTINST(AddS);
        DISPATCH;
      }

      CASE(BitNot) {
        int32_t argInt;
        if (LLVM_LIKELY(
                _sh_ljs_tryfast_truncate_to_int32(O2REG(BitNot), &argInt))) {
          /* Fast-path. */
          O1REG(BitNot) = HermesValue::encodeTrustedNumberValue(~argInt);
          ip = NEXTINST(BitNot);
          DISPATCH;
        }
        CAPTURE_IP_ASSIGN(
            ExecutionStatus status,
            doBitNotSlowPath_RJS(runtime, frameRegs, ip));
        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(BitNot);
        DISPATCH;
      }

      CASE(GetArgumentsLength) {
        // If the arguments object hasn't been created yet.
        if (O2REG(GetArgumentsLength).isUndefined()) {
          O1REG(GetArgumentsLength) =
              HermesValue::encodeTrustedNumberValue(FRAME.getArgCount());
          ip = NEXTINST(GetArgumentsLength);
          DISPATCH;
        }
        // The arguments object has been created, so this is a regular property
        // get.
        assert(
            O2REG(GetArgumentsLength).isObject() &&
            "arguments lazy register is not an object");
        CAPTURE_IP(
            resPH = JSObject::getNamed_RJS(
                Handle<JSObject>::vmcast(&O2REG(GetArgumentsLength)),
                runtime,
                Predefined::getSymbolID(Predefined::length)));
        if (resPH == ExecutionStatus::EXCEPTION) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        O1REG(GetArgumentsLength) = resPH->get();
        ip = NEXTINST(GetArgumentsLength);
        DISPATCH;
      }

      CASE(GetArgumentsPropByValLoose)
      CASE(GetArgumentsPropByValStrict) {
        // If the arguments object hasn't been created yet and we have a
        // valid integer index, we use the fast path.
        if (O3REG(GetArgumentsPropByValLoose).isUndefined()) {
          // If this is an integer index.
          if (auto index =
                  toArrayIndexFastPath(O2REG(GetArgumentsPropByValLoose))) {
            // Is this an existing argument?
            if (*index < FRAME.getArgCount()) {
              O1REG(GetArgumentsPropByValLoose) = FRAME.getArgRef(*index);
              ip = NEXTINST(GetArgumentsPropByValLoose);
              DISPATCH;
            }
          }
        }
        // Slow path.
        CAPTURE_IP_ASSIGN(
            auto res,
            getArgumentsPropByValSlowPath_RJS(
                runtime,
                &O3REG(GetArgumentsPropByValLoose),
                &O2REG(GetArgumentsPropByValLoose),
                FRAME.getCalleeClosureHandleUnsafe(),
                ip->opCode == OpCode::GetArgumentsPropByValStrict));
        if (res == ExecutionStatus::EXCEPTION) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        O1REG(GetArgumentsPropByValLoose) = res->getHermesValue();
        ip = NEXTINST(GetArgumentsPropByValLoose);
        DISPATCH;
      }
      CASE(ReifyArgumentsLoose)
      CASE(ReifyArgumentsStrict) {
        // If the arguments object was already created, do nothing.
        if (!O1REG(ReifyArgumentsLoose).isUndefined()) {
          assert(
              O1REG(ReifyArgumentsLoose).isObject() &&
              "arguments lazy register is not an object");
          ip = NEXTINST(ReifyArgumentsLoose);
          DISPATCH;
        }
        CAPTURE_IP(
            resArgs = reifyArgumentsSlowPath(
                runtime,
                FRAME.getCalleeClosureHandleUnsafe(),
                ip->opCode == OpCode::ReifyArgumentsStrict));
        if (LLVM_UNLIKELY(resArgs == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(ReifyArgumentsLoose) = resArgs->getHermesValue();
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(ReifyArgumentsLoose);
        DISPATCH;
      }

      CASE(NewObject) {
        // Create a new object using the built-in constructor. Note that the
        // built-in constructor is empty, so we don't actually need to call
        // it.
        CAPTURE_IP(
            O1REG(NewObject) = JSObject::create(runtime).getHermesValue());
        assert(
            gcScope.getHandleCountDbg() == KEEP_HANDLES &&
            "Should not create handles.");
        ip = NEXTINST(NewObject);
        DISPATCH;
      }
      CASE(NewObjectWithParent) {
        CAPTURE_IP_ASSIGN(
            O1REG(NewObjectWithParent),
            createObjectWithParent(
                runtime,
                curCodeBlock,
                O2REG(NewObjectWithParent).isObject()
                    ? Handle<JSObject>::vmcast(&O2REG(NewObjectWithParent))
                    : O2REG(NewObjectWithParent).isNull()
                    ? Runtime::makeNullHandle<JSObject>()
                    : Handle<JSObject>::vmcast(&runtime.objectPrototype),
                ip->iNewObjectWithParent.op3));
        assert(
            gcScope.getHandleCountDbg() == KEEP_HANDLES &&
            "Should not create handles.");
        ip = NEXTINST(NewObjectWithParent);
        DISPATCH;
      }

      CASE(NewObjectWithBuffer) {
        CAPTURE_IP(
            resPH = Interpreter::createObjectFromBuffer(
                runtime,
                curCodeBlock,
                Handle<JSObject>::vmcast(&runtime.objectPrototype),
                ip->iNewObjectWithBuffer.op2,
                ip->iNewObjectWithBuffer.op3,
                /* isTyped */ ObjectAllocKind::Untyped));
        if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(NewObjectWithBuffer) = resPH->get();
        ip = NEXTINST(NewObjectWithBuffer);
        DISPATCH;
      }

      CASE(NewObjectWithBufferLong) {
        CAPTURE_IP(
            resPH = Interpreter::createObjectFromBuffer(
                runtime,
                curCodeBlock,
                Handle<JSObject>::vmcast(&runtime.objectPrototype),
                ip->iNewObjectWithBufferLong.op2,
                ip->iNewObjectWithBufferLong.op3,
                /* isTyped */ ObjectAllocKind::Untyped));
        if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(NewObjectWithBufferLong) = resPH->get();
        ip = NEXTINST(NewObjectWithBufferLong);
        DISPATCH;
      }

      CASE(NewObjectWithBufferAndParent) {
        CAPTURE_IP_ASSIGN(
            ExecutionStatus status,
            caseNewObjectWithBufferAndParent(
                runtime, frameRegs, curCodeBlock, ip));
        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION))
          goto exception;
        ip = NEXTINST(NewObjectWithBufferAndParent);
        DISPATCH;
      }

      CASE(NewTypedObjectWithBuffer) {
        auto nonEnumerable = ip->iNewTypedObjectWithBuffer.op5;
        CAPTURE_IP(
            resPH = Interpreter::createObjectFromBuffer(
                runtime,
                curCodeBlock,
                Handle<JSObject>::dyn_vmcast(
                    Handle<>(&O2REG(NewTypedObjectWithBuffer))),
                ip->iNewTypedObjectWithBuffer.op3,
                ip->iNewTypedObjectWithBuffer.op4,
                nonEnumerable ? ObjectAllocKind::TypedNonEnumerable
                              : ObjectAllocKind::TypedEnumerable));
        if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(NewTypedObjectWithBuffer) = resPH->get();
        ip = NEXTINST(NewTypedObjectWithBuffer);
        DISPATCH;
      }

      CASE(NewArray) {
        // Create a new array using the built-in constructor. Note that the
        // built-in constructor is empty, so we don't actually need to call
        // it.
        {
          CAPTURE_IP_ASSIGN(
              auto createRes,
              JSArray::create(runtime, ip->iNewArray.op2, ip->iNewArray.op2));
          if (createRes == ExecutionStatus::EXCEPTION) {
            goto exception;
          }
          O1REG(NewArray) = createRes->getHermesValue();
        }
        ip = NEXTINST(NewArray);
        DISPATCH;
      }

      CASE(NewFastArray) {
        {
          GCScopeMarkerRAII marker{runtime};
          auto protoHandle =
              Handle<JSObject>::vmcast(runtime.makeHandle(O2REG(NewFastArray)));
          CAPTURE_IP_ASSIGN(
              auto createRes,
              FastArray::create(runtime, protoHandle, ip->iNewFastArray.op3));
          if (createRes == ExecutionStatus::EXCEPTION)
            goto exception;
          O1REG(NewFastArray) = *createRes;
        }
        ip = NEXTINST(NewFastArray);
        DISPATCH;
      }
      CASE(FastArrayLength) {
        O1REG(FastArrayLength) = HermesValue::encodeTrustedNumberValue(
            vmcast<FastArray>(O2REG(FastArrayLength))
                ->getLengthAsDouble(runtime));
        ip = NEXTINST(FastArrayLength);
        DISPATCH;
      }
      CASE(FastArrayLoad) {
        double idx = O3REG(FastArrayLoad).getNumber();
        uint32_t intIndex = _sh_tryfast_f64_to_u32_cvt(idx);
        auto *storage = vmcast<FastArray>(O2REG(FastArrayLoad))
                            ->unsafeGetIndexedStorage(runtime);

        if (LLVM_UNLIKELY(intIndex >= storage->size() || intIndex != idx)) {
          CAPTURE_IP(runtime.raiseRangeError("array load index out of range"));
          goto exception;
        }

        O1REG(FastArrayLoad) = storage->at(intIndex).unboxToHV(runtime);
        ip = NEXTINST(FastArrayLoad);
        DISPATCH;
      }
      CASE(FastArrayStore) {
        double idx = O2REG(FastArrayStore).getNumber();
        uint32_t intIndex = _sh_tryfast_f64_to_u32_cvt(idx);
        ENCODE_HV_AS_SHV(shv, O3REG(FastArrayStore));
        auto *storage = vmcast<FastArray>(O1REG(FastArrayStore))
                            ->unsafeGetIndexedStorage(runtime);

        if (LLVM_UNLIKELY(intIndex >= storage->size() || intIndex != idx)) {
          CAPTURE_IP(runtime.raiseRangeError("array store index out of range"));
          goto exception;
        }

        storage->set(intIndex, shv, runtime.getHeap());
        ip = NEXTINST(FastArrayStore);
        DISPATCH;
      }
      CASE(FastArrayPush) {
        CAPTURE_IP_ASSIGN(
            auto res,
            FastArray::push(
                Handle<FastArray>::vmcast(&O1REG(FastArrayPush)),
                runtime,
                Handle<>(&O2REG(FastArrayPush))));
        if (res == ExecutionStatus::EXCEPTION)
          goto exception;
        ip = NEXTINST(FastArrayPush);
        DISPATCH;
      }
      CASE(FastArrayAppend) {
        CAPTURE_IP_ASSIGN(
            auto res,
            FastArray::append(
                Handle<FastArray>::vmcast(&O1REG(FastArrayAppend)),
                runtime,
                Handle<FastArray>::vmcast(&O2REG(FastArrayAppend))));
        if (res == ExecutionStatus::EXCEPTION)
          goto exception;
        ip = NEXTINST(FastArrayAppend);
        DISPATCH;
      }

      CASE(NewArrayWithBuffer) {
        CAPTURE_IP(
            resPH = Interpreter::createArrayFromBuffer(
                runtime,
                curCodeBlock,
                ip->iNewArrayWithBuffer.op2,
                ip->iNewArrayWithBuffer.op3,
                ip->iNewArrayWithBuffer.op4));
        if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(NewArrayWithBuffer) = resPH->get();
        gcScope.flushToSmallCount(KEEP_HANDLES);
        lv.tmp.clear();
        ip = NEXTINST(NewArrayWithBuffer);
        DISPATCH;
      }

      CASE(NewArrayWithBufferLong) {
        CAPTURE_IP(
            resPH = Interpreter::createArrayFromBuffer(
                runtime,
                curCodeBlock,
                ip->iNewArrayWithBufferLong.op2,
                ip->iNewArrayWithBufferLong.op3,
                ip->iNewArrayWithBufferLong.op4));
        if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(NewArrayWithBufferLong) = resPH->get();
        gcScope.flushToSmallCount(KEEP_HANDLES);
        lv.tmp.clear();
        ip = NEXTINST(NewArrayWithBufferLong);
        DISPATCH;
      }

      CASE(CreateThisForSuper) {
        CAPTURE_IP_ASSIGN(
            res,
            createThisImpl(
                runtime,
                &O2REG(CreateThisForSuper),
                &O3REG(CreateThisForSuper),
                ip->iCreateThisForSuper.op4,
                ip->iCreateThisForSuper.op5,
                curCodeBlock));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(CreateThisForSuper) = *res;
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(CreateThisForSuper);
        DISPATCH;
      }

      CASE(CreateThisForNew) {
        CAPTURE_IP(
            res = createThisImpl(
                runtime,
                &O2REG(CreateThisForNew),
                &O2REG(CreateThisForNew),
                ip->iCreateThisForNew.op3,
                ip->iCreateThisForNew.op4,
                curCodeBlock));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(CreateThisForNew) = *res;
        ip = NEXTINST(CreateThisForNew);
        DISPATCH;
      }

      CASE(SelectObject) {
        // Registers: output, thisObject, constructorReturnValue.
        O1REG(SelectObject) = O3REG(SelectObject).isObject()
            ? O3REG(SelectObject)
            : O2REG(SelectObject);
        assert(
            O1REG(SelectObject).isObject() &&
            "SelectObject should always produce an object");
        ip = NEXTINST(SelectObject);
        DISPATCH;
      }

      CASE(Eq)
      CASE(Neq) {
        CAPTURE_IP_ASSIGN(
            auto eqRes,
            abstractEqualityTest_RJS(
                runtime, Handle<>(&O2REG(Eq)), Handle<>(&O3REG(Eq))));
        if (eqRes == ExecutionStatus::EXCEPTION) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        O1REG(Eq) = HermesValue::encodeBoolValue(
            ip->opCode == OpCode::Eq ? *eqRes : !*eqRes);
        ip = NEXTINST(Eq);
        DISPATCH;
      }
      CASE(StrictEq) {
        O1REG(StrictEq) = HermesValue::encodeBoolValue(
            strictEqualityTest(O2REG(StrictEq), O3REG(StrictEq)));
        ip = NEXTINST(StrictEq);
        DISPATCH;
      }
      CASE(StrictNeq) {
        O1REG(StrictNeq) = HermesValue::encodeBoolValue(
            !strictEqualityTest(O2REG(StrictNeq), O3REG(StrictNeq)));
        ip = NEXTINST(StrictNeq);
        DISPATCH;
      }
      CASE(Not) {
        O1REG(Not) = HermesValue::encodeBoolValue(!toBoolean(O2REG(Not)));
        ip = NEXTINST(Not);
        DISPATCH;
      }
      CASE(Negate) {
        if (LLVM_LIKELY(_sh_ljs_is_non_nan_number(O2REG(Negate)))) {
          O1REG(Negate) =
              HermesValue::encodeTrustedNumberValue(-O2REG(Negate).getNumber());
          ip = NEXTINST(Negate);
          DISPATCH;
        }
        CAPTURE_IP_ASSIGN(
            ExecutionStatus status,
            doNegateSlowPath_RJS(runtime, frameRegs, ip));
        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION))
          goto exception;
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(Negate);
        DISPATCH;
      }
      CASE(TypeOf) {
        CAPTURE_IP(O1REG(TypeOf) = typeOf(runtime, Handle<>(&O2REG(TypeOf))));
        ip = NEXTINST(TypeOf);
        DISPATCH;
      }
      CASE(Mod) {
        if (LLVM_LIKELY(
                _sh_ljs_are_both_non_nan_numbers(O2REG(Mod), O3REG(Mod)))) {
          /* Fast-path. */
          O1REG(Mod) = HermesValue::encodeTrustedNumberValue(
              doMod(O2REG(Mod).getNumber(), O3REG(Mod).getNumber()));
          ip = NEXTINST(Mod);
          DISPATCH;
        }
        CAPTURE_IP_ASSIGN(
            ExecutionStatus status,
            doOperSlowPath_RJS<doMod>(runtime, frameRegs, &ip->iMod));
        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(Mod);
        DISPATCH;
      }
      CASE(InstanceOf) {
        CAPTURE_IP_ASSIGN(
            auto result,
            instanceOfOperator_RJS(
                runtime,
                Handle<>(&O2REG(InstanceOf)),
                Handle<>(&O3REG(InstanceOf))));
        if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        O1REG(InstanceOf) = HermesValue::encodeBoolValue(*result);
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(InstanceOf);
        DISPATCH;
      }
      CASE(IsIn) {
        {
          if (LLVM_UNLIKELY(!O3REG(IsIn).isObject())) {
            CAPTURE_IP(runtime.raiseTypeError(
                "right operand of 'in' is not an object"));
            goto exception;
          }
          CAPTURE_IP_ASSIGN(
              auto cr,
              JSObject::hasComputed(
                  Handle<JSObject>::vmcast(&O3REG(IsIn)),
                  runtime,
                  Handle<>(&O2REG(IsIn))));
          if (cr == ExecutionStatus::EXCEPTION) {
            goto exception;
          }
          O1REG(IsIn) = HermesValue::encodeBoolValue(*cr);
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(IsIn);
        DISPATCH;
      }

      CASE(PrivateIsIn) {
        if (LLVM_LIKELY(O3REG(PrivateIsIn).isObject())) {
          auto *obj = vmcast<JSObject>(O3REG(PrivateIsIn));
          auto cacheIdx = ip->iPrivateIsIn.op4;
          auto *cacheEntry = curCodeBlock->getPrivateNameCacheEntry(cacheIdx);
          CompressedPointer clazzPtr{obj->getClassGCPtr()};
          SymbolID nameSym = O2REG(PrivateIsIn).getSymbol();
          // If we have a cache hit, reuse the cached result.
          if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr) &&
              LLVM_LIKELY(cacheEntry->nameVal == nameSym)) {
            O1REG(PrivateIsIn) = HermesValue::encodeBoolValue(cacheEntry->slot);
            ip = NEXTINST(PrivateIsIn);
            DISPATCH;
          }
        }
        CAPTURE_IP_ASSIGN(
            auto res, casePrivateIsIn(runtime, frameRegs, curCodeBlock, ip));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(PrivateIsIn);
        DISPATCH;
      }

      CASE(TypeOfIs) {
        TypeOfIsTypes types(ip->iTypeOfIs.op3);
        O1REG(TypeOfIs) =
            HermesValue::encodeBoolValue(matchTypeOfIs(O2REG(TypeOfIs), types));
        ip = NEXTINST(TypeOfIs);
        DISPATCH;
      }

      CASE(PutOwnBySlotIdxLong) {
        assert(
            O1REG(PutOwnBySlotIdxLong).isObject() &&
            "Object argument of PutOwnBySlotIdx must be an object");
        ENCODE_HV_AS_SHV(shv, O2REG(PutOwnBySlotIdxLong));
        JSObject::setNamedSlotValueUnsafe(
            vmcast<JSObject>(O1REG(PutOwnBySlotIdxLong)),
            runtime,
            ip->iPutOwnBySlotIdxLong.op3,
            shv);
        ip = NEXTINST(PutOwnBySlotIdxLong);
        DISPATCH;
      }
      CASE(PutOwnBySlotIdx) {
        assert(
            O1REG(PutOwnBySlotIdx).isObject() &&
            "Object argument of PutOwnBySlotIdx must be an object");
        ENCODE_HV_AS_SHV(shv, O2REG(PutOwnBySlotIdx));
        JSObject::setNamedSlotValueUnsafe(
            vmcast<JSObject>(O1REG(PutOwnBySlotIdx)),
            runtime,
            ip->iPutOwnBySlotIdx.op3,
            shv);
        ip = NEXTINST(PutOwnBySlotIdx);
        DISPATCH;
      }

      CASE(GetOwnBySlotIdxLong) {
        O1REG(GetOwnBySlotIdxLong) =
            JSObject::getNamedSlotValueUnsafe(
                vmcast<JSObject>(O2REG(GetOwnBySlotIdxLong)),
                runtime,
                ip->iGetOwnBySlotIdxLong.op3)
                .unboxToHV(runtime);
        ip = NEXTINST(GetOwnBySlotIdxLong);
        DISPATCH;
      }
      CASE(GetOwnBySlotIdx) {
        O1REG(GetOwnBySlotIdx) = JSObject::getNamedSlotValueUnsafe(
                                     vmcast<JSObject>(O2REG(GetOwnBySlotIdx)),
                                     runtime,
                                     ip->iGetOwnBySlotIdx.op3)
                                     .unboxToHV(runtime);
        ip = NEXTINST(GetOwnBySlotIdx);
        DISPATCH;
      }

      CASE_OUTOFLINE(DelByVal);

      CASE(CreateRegExp) {
        CAPTURE_IP(caseCreateRegExp(runtime, frameRegs, curCodeBlock, ip));
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(CreateRegExp);
        DISPATCH;
      }

      CASE(UIntSwitchImm) {
        if (LLVM_LIKELY(O1REG(UIntSwitchImm).isNumber())) {
          double numVal = O1REG(UIntSwitchImm).getNumber();
          uint32_t uintVal = (uint32_t)numVal;
          if (LLVM_LIKELY(numVal == uintVal) && // Only integers.
              LLVM_LIKELY(
                  uintVal >= ip->iUIntSwitchImm.op4) && // Bounds checking.
              LLVM_LIKELY(
                  uintVal <= ip->iUIntSwitchImm.op5)) // Bounds checking.
          {
            // Calculate the offset into the bytecode where the jump table for
            // this SwitchImm starts.
            const uint8_t *tablestart = (const uint8_t *)llvh::alignAddr(
                (const uint8_t *)ip + ip->iUIntSwitchImm.op2, sizeof(uint32_t));

            // Read the offset from the table.
            // Must be signed to account for backwards branching.
            const int32_t *loc =
                (const int32_t *)tablestart + uintVal - ip->iUIntSwitchImm.op4;

            ip = IPADD(*loc);
            DISPATCH;
          }
        }
        // Wrong type or out of range, jump to default.
        ip = IPADD(ip->iUIntSwitchImm.op3);
        DISPATCH;
      }
      CASE(StringSwitchImm) {
        // While it can't invoke any JS code, doStringSwitchImm (when
        // it calls initializeStringSwitchImmTable) invokes operations
        // that check that the current IP is set.  Thus, we must
        // capture it here.  Note that we can't assign to ip directly
        // within CAPTURE_IP_ASSIGN, because that assigns the original
        // IP back to ip after evaluating the expression, so it would
        // overwrite the new value.
        CAPTURE_IP_ASSIGN(
            const Inst *newIp, doStringSwitchImm(curCodeBlock, frameRegs, ip));
        ip = newIp;
        DISPATCH;
      }
      LOAD_CONST(
          LoadConstUInt8,
          HermesValue::encodeTrustedNumberValue(ip->iLoadConstUInt8.op2));
      LOAD_CONST(
          LoadConstInt,
          HermesValue::encodeTrustedNumberValue(ip->iLoadConstInt.op2));
      LOAD_CONST(
          LoadConstDouble,
          HermesValue::encodeTrustedNumberValue(ip->iLoadConstDouble.op2));
      LOAD_CONST_CAPTURE_IP(
          LoadConstString,
          HermesValue::encodeStringValue(
              curCodeBlock->getRuntimeModule()
                  ->getStringPrimFromStringIDMayAllocate(
                      ip->iLoadConstString.op2)));
      LOAD_CONST_CAPTURE_IP(
          LoadConstStringLongIndex,
          HermesValue::encodeStringValue(
              curCodeBlock->getRuntimeModule()
                  ->getStringPrimFromStringIDMayAllocate(
                      ip->iLoadConstStringLongIndex.op2)));
      LOAD_CONST(LoadConstEmpty, HermesValue::encodeEmptyValue());
      LOAD_CONST(LoadConstUndefined, HermesValue::encodeUndefinedValue());
      LOAD_CONST(LoadConstNull, HermesValue::encodeNullValue());
      LOAD_CONST(LoadConstTrue, HermesValue::encodeBoolValue(true));
      LOAD_CONST(LoadConstFalse, HermesValue::encodeBoolValue(false));
      LOAD_CONST(LoadConstZero, HermesValue::encodeTrustedNumberValue(0));
      CASE(LoadConstBigInt) {
        idVal = ip->iLoadConstBigInt.op2;
        nextIP = NEXTINST(LoadConstBigInt);
        goto doLoadConstBigInt;
      }
      CASE(LoadConstBigIntLongIndex) {
        idVal = ip->iLoadConstBigIntLongIndex.op2;
        nextIP = NEXTINST(LoadConstBigIntLongIndex);
        goto doLoadConstBigInt;
      }
    doLoadConstBigInt: {
      CAPTURE_IP_ASSIGN(
          auto res,
          BigIntPrimitive::fromBytes(
              runtime,
              curCodeBlock->getRuntimeModule()->getBigIntBytesFromBigIntId(
                  idVal)));
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        goto exception;
      }
      // It is safe to access O1REG(LoadConstBigInt) or
      // O1REG(LoadConstBigIntLongIndex) here as both instructions' O1 operands
      // are the same size and live in the same offset w.r.t. the start of the
      // instruction.
      O1REG(LoadConstBigInt) = std::move(*res);
      ip = nextIP;
      DISPATCH;
    }
      BINOP(Sub);
      BINOP(Mul);
      BINOP(Div);
      // Can't do BINOP(Mod) as there's no ModN opcode.
      BITWISEBINOP(BitAnd);
      BITWISEBINOP(BitOr);
      BITWISEBINOP(BitXor);
      CASE(Imul) {
        // Math.imul is not BigInt-aware: unlike the other bitwise binops,
        // it always converts both operands with ToInt32 and throws on a
        // BigInt operand instead of producing a BigInt result. So it cannot
        // use BITWISEBINOP/doBitOperSlowPath_RJS, which are BigInt-aware.
        int32_t lhsInt, rhsInt;
        if (LLVM_LIKELY(
                _sh_ljs_tryfast_truncate_to_int32(O2REG(Imul), &lhsInt) &&
                _sh_ljs_tryfast_truncate_to_int32(O3REG(Imul), &rhsInt))) {
          /* Fast-path. */
          O1REG(Imul) =
              HermesValue::encodeTrustedNumberValue(doImul(lhsInt, rhsInt));
          ip = NEXTINST(Imul);
          DISPATCH;
        }
        CAPTURE_IP_ASSIGN(
            ExecutionStatus status, doImulSlowPath_RJS(runtime, frameRegs, ip));
        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(Imul);
        DISPATCH;
      }
      SHIFTOP(LShift);
      SHIFTOP(RShift);
      SHIFTOP(URshift);
      CONDOP(Less, <, lessOp_RJS);
      CONDOP(LessEq, <=, lessEqualOp_RJS);
      CONDOP(Greater, >, greaterOp_RJS);
      CONDOP(GreaterEq, >=, greaterEqualOp_RJS);
      JCOND(Less, <, lessOp_RJS, JCOND_IMPL);
      JCOND(Less, <, lessOp_RJS, JCOND_N_IMPL);
      JCOND(LessEqual, <=, lessEqualOp_RJS, JCOND_IMPL);
      JCOND(LessEqual, <=, lessEqualOp_RJS, JCOND_N_IMPL);

      JCOND(Greater, >, greaterOp_RJS, JCOND_IMPL);
      JCOND(GreaterEqual, >=, greaterEqualOp_RJS, JCOND_IMPL);

      JCOND_STRICT_EQ_IMPL(
          JStrictEqual, , IPADD(ip->iJStrictEqual.op1), NEXTINST(JStrictEqual));
      JCOND_STRICT_EQ_IMPL(
          JStrictEqual,
          Long,
          IPADD(ip->iJStrictEqualLong.op1),
          NEXTINST(JStrictEqualLong));
      JCOND_STRICT_EQ_IMPL(
          JStrictNotEqual,
          ,
          NEXTINST(JStrictNotEqual),
          IPADD(ip->iJStrictNotEqual.op1));
      JCOND_STRICT_EQ_IMPL(
          JStrictNotEqual,
          Long,
          NEXTINST(JStrictNotEqualLong),
          IPADD(ip->iJStrictNotEqualLong.op1));

      JCOND_EQ_IMPL(JEqual, , IPADD(ip->iJEqual.op1), NEXTINST(JEqual));
      JCOND_EQ_IMPL(
          JEqual, Long, IPADD(ip->iJEqualLong.op1), NEXTINST(JEqualLong));
      JCOND_EQ_IMPL(
          JNotEqual, , NEXTINST(JNotEqual), IPADD(ip->iJNotEqual.op1));
      JCOND_EQ_IMPL(
          JNotEqual,
          Long,
          NEXTINST(JNotEqualLong),
          IPADD(ip->iJNotEqualLong.op1));

      CASE(JmpTypeOfIs) {
        TypeOfIsTypes types(ip->iJmpTypeOfIs.op3);
        if (matchTypeOfIs(O2REG(JmpTypeOfIs), types)) {
          ip = IPADD(ip->iJmpTypeOfIs.op1);
        } else {
          ip = NEXTINST(JmpTypeOfIs);
        }
        DISPATCH;
      }

      CASE(AddOwnPrivateBySym) {
        CAPTURE_IP_ASSIGN(
            auto res,
            JSObject::defineNewOwnProperty(
                Handle<JSObject>::vmcast(&O1REG(AddOwnPrivateBySym)),
                runtime,
                O3REG(AddOwnPrivateBySym).getSymbol(),
                PropertyFlags::privateFieldPropertyFlags(),
                Handle<>(&O2REG(AddOwnPrivateBySym))));
        if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
          goto exception;
        }
        gcScope.flushToSmallCount(KEEP_HANDLES);
        ip = NEXTINST(AddOwnPrivateBySym);
        DISPATCH;
      }

      CASE(GetOwnPrivateBySym) {
        if (LLVM_LIKELY(O2REG(GetOwnPrivateBySym).isObject())) {
          auto *obj = vmcast<JSObject>(O2REG(GetOwnPrivateBySym));
          auto cacheIdx = ip->iGetOwnPrivateBySym.op3;
          auto *cacheEntry = curCodeBlock->getPrivateNameCacheEntry(cacheIdx);
          CompressedPointer clazzPtr{obj->getClassGCPtr()};
          SymbolID nameSym = O4REG(GetOwnPrivateBySym).getSymbol();
          // If we have a cache hit, reuse the cached result.
          if (LLVM_LIKELY(
                  cacheEntry->clazz == clazzPtr &&
                  cacheEntry->nameVal == nameSym)) {
            O1REG(GetOwnPrivateBySym) = JSObject::getNamedSlotValueUnsafe(
                                            obj, runtime, cacheEntry->slot)
                                            .unboxToHV(runtime);
            ip = NEXTINST(GetOwnPrivateBySym);
            DISPATCH;
          }
          CAPTURE_IP_ASSIGN(
              auto res,
              JSObject::getPrivateField(
                  Handle<JSObject>::vmcast(&O2REG(GetOwnPrivateBySym)),
                  runtime,
                  Handle<SymbolID>::vmcast(&O4REG(GetOwnPrivateBySym)),
                  cacheIdx == hbc::PROPERTY_CACHING_DISABLED ? nullptr
                                                             : cacheEntry));
          if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
            goto exception;
          }
          O1REG(GetOwnPrivateBySym) = *res;
          gcScope.flushToSmallCount(KEEP_HANDLES);
          ip = NEXTINST(GetOwnPrivateBySym);
          DISPATCH;
        } else {
          CAPTURE_IP(runtime.raiseTypeError(
              "cannot read private property of a non-object"));
          goto exception;
        }
      }

      CASE(PutOwnPrivateBySym) {
        if (LLVM_LIKELY(O1REG(PutOwnPrivateBySym).isObject())) {
          ENCODE_HV_AS_SHV(shv, O2REG(PutOwnPrivateBySym));
          auto *obj = vmcast<JSObject>(O1REG(PutOwnPrivateBySym));
          auto cacheIdx = ip->iPutOwnPrivateBySym.op3;
          auto *cacheEntry = curCodeBlock->getPrivateNameCacheEntry(cacheIdx);
          CompressedPointer clazzPtr{obj->getClassGCPtr()};
          SymbolID nameSym = O4REG(PutOwnPrivateBySym).getSymbol();
          if (LLVM_LIKELY(
                  cacheEntry->clazz == clazzPtr &&
                  cacheEntry->nameVal == nameSym)) {
            JSObject::setNamedSlotValueUnsafe(
                obj, runtime, cacheEntry->slot, shv);
            ip = NEXTINST(PutOwnPrivateBySym);
            DISPATCH;
          }
          // Slow path
          CAPTURE_IP_ASSIGN(
              auto res,
              JSObject::setPrivateField(
                  Handle<JSObject>::vmcast(&O1REG(PutOwnPrivateBySym)),
                  runtime,
                  Handle<SymbolID>::vmcast(&O4REG(PutOwnPrivateBySym)),
                  Handle<>(&O2REG(PutOwnPrivateBySym)),
                  cacheIdx == hbc::PROPERTY_CACHING_DISABLED
                      ? nullptr
                      : curCodeBlock->getPrivateNameCacheEntry(cacheIdx)));
          if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
            goto exception;
          }
          gcScope.flushToSmallCount(KEEP_HANDLES);
          ip = NEXTINST(PutOwnPrivateBySym);
          DISPATCH;
        } else {
          CAPTURE_IP(runtime.raiseTypeError(
              "cannot set private property of a non-object"));
          goto exception;
        }
      }

      CASE_OUTOFLINE(DefineOwnByVal);
      CASE_OUTOFLINE(DefineOwnGetterSetterByVal);
      CASE_OUTOFLINE(DirectEval);

      CASE_OUTOFLINE(IteratorBegin);
      CASE_OUTOFLINE(IteratorNext);
      CASE(IteratorClose) {
        if (LLVM_UNLIKELY(O1REG(IteratorClose).isObject())) {
          // The iterator must be closed if it's still an object.
          // That means it was never an index and is not done iterating (a state
          // which is indicated by `undefined`).
          CAPTURE_IP_ASSIGN(
              auto res,
              iteratorClose(
                  runtime,
                  Handle<JSObject>::vmcast(&O1REG(IteratorClose)),
                  Runtime::getEmptyValue()));
          if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
            if (ip->iIteratorClose.op2 &&
                !isUncatchableError(*runtime.thrownValue_)) {
              // Ignore inner exception.
              runtime.clearThrownValue();
            } else {
              goto exception;
            }
          }
          gcScope.flushToSmallCount(KEEP_HANDLES);
        }
        ip = NEXTINST(IteratorClose);
        DISPATCH;
      }

      CASE(TypedLoadParent) {
        JSObject *parent =
            vmcast<JSObject>(O2REG(TypedLoadParent))->getParent(runtime);
        O1REG(TypedLoadParent) = HermesValue::encodeObjectValue(parent);
        ip = NEXTINST(TypedLoadParent);
        DISPATCH;
      }

      CASE(_last) {
        hermes_fatal("Invalid opcode _last");
      }
    }

    hermes_fatal(
        "All opcodes should dispatch to the next and not fallthrough "
        "to here");

  exception:
    UPDATE_OPCODE_TIME_SPENT;
    assert(
        !runtime.thrownValue_->isEmpty() &&
        "thrownValue unavailable at exception");

    bool catchable = true;
    if (auto *jsError = dyn_vmcast<JSError>(*runtime.thrownValue_)) {
      catchable = jsError->catchable();
    }

    gcScope.flushToSmallCount(KEEP_HANDLES);
    lv.tmp.clear();

#ifdef HERMES_ENABLE_DEBUGGER
    if (SingleStep) {
      // If we're single stepping, don't bother with any more checks,
      // and simply signal that we should continue execution with an exception.
      runtime.setCurrentIP(ip);
      state.codeBlock = curCodeBlock;
      state.offset = CUROFFSET;
      return ExecutionStatus::EXCEPTION;
    }

    using PauseOnThrowMode = facebook::hermes::debugger::PauseOnThrowMode;
    auto mode = runtime.debugger_.getPauseOnThrowMode();
    if (mode != PauseOnThrowMode::None) {
      if (!runtime.debugger_.isDebugging()) {
        // Determine whether the PauseOnThrowMode requires us to stop here.
        bool caught =
            runtime.debugger_
                .findCatchTarget(InterpreterState(curCodeBlock, CUROFFSET))
                .hasValue();
        bool shouldStop = mode == PauseOnThrowMode::All ||
            (mode == PauseOnThrowMode::Uncaught && !caught);
        if (shouldStop) {
          // When runDebugger is invoked after an exception,
          // stepping should never happen internally.
          // Any step is a step to an exception handler, which we do
          // directly here in the interpreter.
          // Thus, the result state should be the same as the input state.
          InterpreterState tmpState{curCodeBlock, (uint32_t)CUROFFSET};
          CAPTURE_IP_ASSIGN(
              ExecutionStatus resultStatus,
              runtime.debugger_.runDebugger(
                  Debugger::RunReason::Exception, tmpState));
          (void)resultStatus;
          assert(
              tmpState == InterpreterState(curCodeBlock, CUROFFSET) &&
              "not allowed to step internally in a pauseOnThrow");
          gcScope.flushToSmallCount(KEEP_HANDLES);
        }
      }
    }
#endif

    int32_t handlerOffset = 0;

    // If the exception is not catchable, skip found catch blocks.
    while (((handlerOffset = curCodeBlock->findCatchTargetOffset(CUROFFSET)) ==
            -1) ||
           !catchable) {
      PROFILER_EXIT_FUNCTION(curCodeBlock);

#ifdef HERMES_MEMORY_INSTRUMENTATION
      runtime.popCallStack();
#endif

      // Restore the code block and IP.
      curCodeBlock = FRAME.getSavedCodeBlock();
      ip = FRAME.getSavedIP();

      // Pop a stack frame.
      frameRegs =
          &runtime.restoreStackAndPreviousFrame(FRAME).getFirstLocalRef();

      SLOW_DEBUG(
          dbgs() << "function exit with exception: restored stackLevel="
                 << runtime.getStackLevel() << "\n");

      // Are we returning to native code?
      if (!curCodeBlock) {
        SLOW_DEBUG(
            dbgs()
            << "function exit with exception: returning to native code\n");
        return ExecutionStatus::EXCEPTION;
      }

      assert(
          isCallType(ip->opCode) &&
          "return address is not Call-type instruction");
    }

    INIT_STATE_FOR_CODEBLOCK(curCodeBlock);

    ip = IPADD(handlerOffset - CUROFFSET);
  }
}

} // namespace vm
} // namespace hermes

#undef DEBUG_TYPE
