/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_X86_64
#include "JitEmitter-internal.h"
#include "JitEmitter.h"
#include "JitImpl.h"

#include "../JitHandlers.h"

#include "../RuntimeOffsets.h"
#include "hermes/Support/ErrorHandling.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VMLayouts/StackFrameLayout.h"

#include <cstdio>
#include <limits>

#if defined(HERMESVM_COMPRESSED_POINTERS) && !defined(HERMESVM_CONTIGUOUS_HEAP)
#error JIT does not support non-contiguous heap with compressed pointers
#endif

namespace hermes::vm::x86_64 {

// Disable warnings about missing designated initializers, which the
// roDataDesc_ entries below rely on.
#ifdef __clang__
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#endif

llvh::raw_ostream &operator<<(
    llvh::raw_ostream &os,
    const hermes::vm::x86_64::HWReg &hwReg) {
  if (hwReg.isValidGpX()) {
    os << "r" << (int)hwReg.indexInClass();
  } else if (hwReg.isValidVecD()) {
    os << "xmm" << (int)hwReg.indexInClass();
  } else {
    assert(!hwReg.isValid());
    os << "<invalid>";
  }
  return os;
}

Emitter::Emitter(
    Runtime &runtime,
    JITContext::Impl &jitImpl,
    unsigned dumpJitCode,
    bool emitAsserts,
    bool emitTypeAsserts,
    bool emitCounters,
    PerfJitDump *perfJitDump,
    CodeBlock *codeBlock,
    const std::function<void(std::string &&message)> &longjmpError)
    : runtime_(runtime),
      jitImpl_(jitImpl),
      dumpJitCode_(dumpJitCode),
      emitAsserts_(emitAsserts),
      emitTypeAsserts_(emitTypeAsserts),
      emitCounters_(emitCounters),
      frameRegs_(codeBlock->getFrameSize()),
      codeBlock_(codeBlock) {
  errorHandler_ = std::unique_ptr<asmjit::ErrorHandler>(
      new OurErrorHandler(expectedError_, longjmpError));

  code.init(jitImpl.jr.environment(), jitImpl.jr.cpuFeatures());
  code.setErrorHandler(errorHandler_.get());

#ifndef ASMJIT_NO_LOGGING
  if ((dumpJitCode_ & DumpJitCode::Code) || perfJitDump) {
    logger_ = std::unique_ptr<asmjit::Logger>(
        new OurLogger(a, perfJitDump, dumpJitCode_));
    logger_->setIndentation(asmjit::FormatIndentationGroup::kCode, 4);
    logger_->addFlags(asmjit::FormatFlags::kHexImms);
    code.setLogger(logger_.get());
  }
#endif

  code.attach(&a);

  roDataLabel_ = a.newNamedLabel("RO_DATA");
  returnLabel_ = a.newNamedLabel("leave");

  // Save read/write property cache addresses.
  roOfsReadPropertyCachePtr_ = uint64Const(
      (uint64_t)codeBlock->readPropertyCache(), "readPropertyCache");
  roOfsWritePropertyCachePtr_ = uint64Const(
      (uint64_t)codeBlock->writePropertyCache(), "writePropertyCache");
  roOfsPrivateNameCachePtr_ =
      uint64Const((uint64_t)codeBlock->privateNameCache(), "privateNameCache");
}

void Emitter::commentV(const char *fmt, va_list args) {
  char buf[80];
  vsnprintf(buf, sizeof(buf), fmt, args);
  a.comment(buf);
}

JITCompiledFunctionPtr Emitter::addToRuntime(asmjit::JitRuntime &jr) {
  code.detach(&a);
  JITCompiledFunctionPtr fn;
  asmjit::Error err = jr.add(&fn, &code);
  if (err) {
    llvh::errs() << "AsmJit failed: " << asmjit::DebugUtils::errorAsString(err)
                 << "\n";
    hermes::hermes_fatal("AsmJit failed");
  }
  return fn;
}

#ifndef NDEBUG
void Emitter::assertPostInstructionInvariants() {
  // x86-64: no instruction may leave rsp transiently lowered across its own
  // boundary -- see the rspDelta_ contract on callImpl().
  assert(rspDelta_ == 0 && "rsp delta not restored at instruction boundary");

  for (const auto &frState : frameRegs_)
    assert(!frState.regIsDirty && "Frame register is dirty");

  // Check that any temps have an associated FR.
  for (unsigned i = kGPTemp1.first; i <= kGPTemp2.second; ++i) {
    if (i > kGPTemp1.second && i < kGPTemp2.first)
      continue;
    HWReg hwReg(i, HWReg::GpX{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid()) {
      assert(gpTemp_.isAllocated(i) && "Temp register is not freed");
    }
  }

  for (unsigned i = kVecTemp.first; i <= kVecTemp.second; ++i) {
    HWReg hwReg(i, HWReg::VecD{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid()) {
      assert(vecTemp_.isAllocated(i) && "Temp register is not freed");
    }
  }
}
#endif

void Emitter::enter(uint32_t numCount, uint32_t npCount) {
  // x86-64: SysV has no callee-saved vector registers, so there is a single
  // pool of global registers, kGPSavedList, which both number FRs and
  // non-pointer FRs draw from, numbers first. arm64's second (vector) loop
  // has no analogue here.
  unsigned nextGp = 0;

  // Number registers.
  for (unsigned frIndex = 0; frIndex < numCount; ++frIndex) {
    if (nextGp >= std::size(kGPSavedList))
      break;
    unsigned regIndex = kGPSavedList[nextGp];
    comment("    ; alloc: r%u <= r%u", regIndex, frIndex);
    ++nextGp;

    frameRegs_[frIndex].globalReg = HWReg::gpX(regIndex);
    frameRegs_[frIndex].globalType = FRType::Number;
  }
  // Non-pointer regs.
  for (unsigned frIndex = numCount; frIndex < npCount + numCount; ++frIndex) {
    if (nextGp >= std::size(kGPSavedList))
      break;
    unsigned regIndex = kGPSavedList[nextGp];
    comment("    ; alloc: r%u <= r%u", regIndex, frIndex);
    ++nextGp;

    frameRegs_[frIndex].globalReg = HWReg::gpX(regIndex);
    frameRegs_[frIndex].globalType = FRType::UnknownNonPtr;
  }

  bool hasExceptionTable = !codeBlock_->getRuntimeModule()
                                ->getBytecode()
                                ->getExceptionTable(codeBlock_->getFunctionID())
                                .empty();

  // A function with exception handlers must end up with no global registers.
  // longjmp restores callee-saved registers to their values at the setjmp,
  // so a global register would present a stale value to a catch handler;
  // correctness depends on every FR's canonical location being the memory
  // frame, which is also what makes the isInTry() sync in the throwing
  // emitters sufficient.
  //
  // Nothing here enforces that: it holds because
  // RegisterAllocator::getRegClass (lib/BCGen/RegAlloc.cpp) forces
  // RegClass::Other for any function containing a try, which leaves both
  // counts at zero. That is a contract with another module, so check it.
  assert(
      (!hasExceptionTable || (numCount == 0 && npCount == 0)) &&
      "function with exception handlers must have no global registers");

  if (hasExceptionTable)
    catchTableLabel_ = a.newNamedLabel("CATCH_TABLE");

  frameSetup(frameRegs_.size(), nextGp);
}

int32_t Emitter::getDebugFunctionName() {
  if (roOfsDebugFunctionName_ < 0) {
    std::string str;
    llvh::raw_string_ostream ss(str);
    ss << codeBlock_->getFunctionID() << "(" << codeBlock_->getNameString()
       << ")";
    ss.flush();
    int32_t size = str.size() + 1;
    roOfsDebugFunctionName_ = reserveData(size, 1, asmjit::TypeId::kInt8, size);
    memcpy(roData_.data() + roOfsDebugFunctionName_, str.data(), size);
  }
  return roOfsDebugFunctionName_;
}

void Emitter::frameSetup(unsigned numFrameRegs, unsigned gpSaveCount) {
  assert(
      gpSaveCount <= std::size(kGPSavedList) &&
      "Too many callee saved GP regs");

  static_assert(
      kGPSavedList[0] == kGPReturnStash,
      "Callee saved GP regs must start at rbx");
  // Always save rbx even if it is not needed for an FR, because we use it for
  // the return value.
  if (gpSaveCount == 0)
    gpSaveCount = 1;

  gpSaveCount_ = gpSaveCount;

  // Higher addresses are at the top.
  // +-----------------------------+ <-- caller rsp before the call
  // |      return address         |
  // +-----------------------------+
  // |         saved rbp           | <-- rbp
  // +-----------------------------+
  // |   saved r15, r14, then      |
  // |   rbx/r12/r13 as used       |
  // +-----------------------------+
  // |   alignment padding 0-8     |
  // +-----------------------------+
  // |  Saved SHLocals* (optional) |
  // +-----------------------------+
  // |     SHJmpBuf (optional)     | <-- rsp (rsp % 16 == 0 here)
  // +-----------------------------+
  //
  // Stack alignment. SysV requires rsp % 16 == 0 immediately before every
  // `call`, i.e. rsp % 16 == 8 on entry to the callee. On entry here the
  // caller's `call` has pushed a return address, so rsp % 16 == 8;
  // `push rbp` brings it to 0, and each of the (2 + gpSaveCount) further
  // pushes flips it by 8:
  //   after the pushes: rsp % 16 == 8 * ((2 + gpSaveCount) % 2)
  //                              == 8 * (gpSaveCount % 2)
  // getSavedRegsPadding() is exactly that value, and the optional SHJmpBuf +
  // SHLocals area is rounded up to a multiple of 16, so the single
  // `sub rsp, getStackSize()` below lands on rsp % 16 == 0 and every call
  // emitted afterwards is aligned. Putting the padding above the exception
  // area (rather than between it and rsp) means the SHJmpBuf itself always
  // starts at rsp + 0, so it is 16-byte aligned regardless of the padding's
  // size -- i.e. regardless of push parity.
  a.push(x86::rbp);
  a.mov(x86::rbp, x86::rsp);
  a.push(xRuntime);
  a.push(xFrame);
  for (unsigned i = 0; i < gpSaveCount; ++i)
    a.push(x86::gpq(kGPSavedList[i]));
  if (uint32_t stackSize = getStackSize())
    a.sub(x86::rsp, stackSize);

  comment("// xRuntime");
  a.mov(xRuntime, x86::rdi);

  // Save the SHLocals pointer because we don't allocate and push a new
  // SHLocals in the JIT.
  // Used in CatchInst to restore state.
  if (catchTableLabel_.isValid()) {
    comment("// saved SHLocals *");
    a.mov(x86::rax, x86::qword_ptr(xRuntime, RuntimeOffsets::shLocals));
    a.mov(
        x86::qword_ptr(x86::rsp, (int32_t)getSavedSHLocalsOffset()), x86::rax);
  }

#ifndef HERMES_CHECK_NATIVE_STACK
#error Only native stack checking is supported in the JIT
#endif

  comment("// _sh_check_native_stack_overflow");
  asmjit::Label nativeOverflowLab = newSlowPathLabel();
  asmjit::Label nativeOverflowContLab = newContLabel();
  // Subtract the frame pointer from nativeStackHigh and compare it against
  // the size. If the difference is less than the stack size, then we are
  // still within the current stack bounds.
  // x86-64: rbp is the frame pointer (arm64 used x29), and the second load
  // folds into the cmp as a memory operand, so only xScratch is needed.
  a.mov(xScratch, x86::qword_ptr(xRuntime, RuntimeOffsets::nativeStackHigh));
  a.sub(xScratch, x86::rbp);
  a.cmp(xScratch, x86::qword_ptr(xRuntime, RuntimeOffsets::nativeStackSize));
  // If the frame pointer is within bounds, we are done. Otherwise, we need to
  // check if the bounds have changed.
  a.ja(nativeOverflowLab);
  a.bind(nativeOverflowContLab);
  slowPaths_.emplace_back(
      nativeOverflowLab,
      nativeOverflowContLab,
      /* emittingIP */ nullptr,
      [](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: _sh_check_native_stack_overflow");
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        // Do not save the IP because we have not yet set up the stack frame
        // for this function. If this throws, the exception should appear in
        // the caller.
        EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
            em, void (*)(SHRuntime *), _sh_check_native_stack_overflow);
        em.a.jmp(sp.contLab);
      });

  comment("// xFrame");
  a.mov(xFrame, x86::qword_ptr(xRuntime, RuntimeOffsets::stackPointer));

  // If the function has a prohibitInvoke flag, we need to check if it has been
  // called correctly.
  auto prohibitInvoke = codeBlock_->getHeaderFlags().getProhibitInvoke();
  if (prohibitInvoke != ProhibitInvoke::None) {
    // Load new.target.
    a.mov(
        x86::rax,
        x86::qword_ptr(
            xFrame, StackFrameLayout::NewTarget * (int)sizeof(SHLegacyValue)));
    // Compare new.target against undefined.
    emit_sh_ljs_is_undefined(a, xScratch, x86::rax);

    void (*slowCall)(SHRuntime *);
    const char *slowCallName;
    asmjit::Label throwInvalidInvokeLab;
    if (prohibitInvoke == ProhibitInvoke::Call) {
      // If regular calls are prohibited, then we jump to throwInvalidInvoke if
      // new.target is undefined.
      throwInvalidInvokeLab = a.newNamedLabel("throwInvalidCall");
      a.je(throwInvalidInvokeLab);

      slowCall = _sh_throw_invalid_call;
      slowCallName = "_sh_throw_invalid_call";
    } else {
      assert(
          prohibitInvoke == ProhibitInvoke::Construct &&
          "Unknown prohibitInvoke");
      // If construct calls are prohibited, then we jump to throwInvalidInvoke
      // if new.target is not undefined.
      throwInvalidInvokeLab = a.newNamedLabel("throwInvalidConstruct");
      a.jne(throwInvalidInvokeLab);

      slowCall = _sh_throw_invalid_construct;
      slowCallName = "_sh_throw_invalid_construct";
    }

    slowPaths_.emplace_back(
        throwInvalidInvokeLab,
        /* emittingIP */ nullptr,
        [slowCall, slowCallName](Emitter &em, SlowPath &sp) {
          em.comment("// Slow path: %s", slowCallName);
          em.a.bind(sp.slowPathLab);
          em.a.mov(x86::rdi, xRuntime);
          // We don't save the IP, because this is being thrown in the
          // caller's context.
          em.callRuntime((void *)slowCall, slowCallName);
          // Function does not return.
        });
  }

  // NOTE: Unlike _sh_enter, we do not push an SHLocals object.
  //  SHLegacyValue *frame = _sh_enter(shr, &locals.head, 13);
  comment("// _sh_enter");
  asmjit::Label registerOverflowLab = newSlowPathLabel();

  // Compute the remaining available stack space:
  // runtime.registerStackEnd - runtime.stackPointer
  a.mov(x86::rax, x86::qword_ptr(xRuntime, RuntimeOffsets::registerStackEnd));
  a.sub(x86::rax, xFrame);
  // Check if we need more registers than remain.
  size_t totalRegsToAlloc = numFrameRegs + hbc::StackFrameLayout::FirstLocal;
  size_t regAllocSize = totalRegsToAlloc * sizeof(SHLegacyValue);
  // x86-64: cmp and lea take a 32-bit immediate directly, so arm64's
  // isAddSubImm special casing has no analogue. A frame this large cannot be
  // described by the bytecode, but the encoding depends on it, so check.
  assert(
      regAllocSize <= (size_t)std::numeric_limits<int32_t>::max() &&
      "frame size must fit in an imm32");
  a.cmp(x86::rax, (int32_t)regAllocSize);
  a.jb(registerOverflowLab);
  a.lea(x86::rax, x86::ptr(xFrame, (int32_t)regAllocSize));

  // Advance the register stack.
  a.mov(x86::qword_ptr(xRuntime, RuntimeOffsets::stackPointer), x86::rax);
  a.mov(x86::qword_ptr(xRuntime, RuntimeOffsets::currentFrame), xFrame);

  static_assert(
      HERMESVALUE_VERSION == 2, "Raw zero value must be ignored by GC");
  // Initialize the registers with zero, in groups of 4, then 2, then 1.
  // x86-64: there is no zero register, so xmm0 is zeroed once and stored
  // with unaligned 16-byte stores (the register stack is only 8-byte
  // aligned). arm64's post-indexed stores become explicit displacements off
  // the fill base, which stays xFrame unless the loop below advances it.
  size_t regsToFill = totalRegsToAlloc;
  assert(regsToFill > 0 && "there is always at least one register");
  a.vpxor(x86::xmm0, x86::xmm0, x86::xmm0);
  x86::Gp fillPtr = xFrame;
  int32_t fillOfs = 0;
  // If there are more than 32 registers, start with a loop.
  if (regsToFill > 32) {
    a.mov(x86::rax, xFrame);
    fillPtr = x86::rax;
    // We will fill 4 registers on each iteration.
    unsigned loopBytes = llvh::alignDown(regsToFill, 4) * sizeof(SHLegacyValue);
    // Initialize the loop limit in xScratch.
    a.lea(xScratch, x86::ptr(x86::rax, (int32_t)loopBytes));
    asmjit::Label loop = a.newLabel();
    a.bind(loop);
    // Loop until we reach the limit.
    a.vmovups(x86::xmmword_ptr(x86::rax), x86::xmm0);
    a.vmovups(x86::xmmword_ptr(x86::rax, 16), x86::xmm0);
    a.add(x86::rax, 32);
    a.cmp(x86::rax, xScratch);
    a.jb(loop);

    regsToFill %= 4;
  } else {
    // If the number of registers is small, just fill them directly.
    while (regsToFill >= 4) {
      a.vmovups(x86::xmmword_ptr(fillPtr, fillOfs), x86::xmm0);
      a.vmovups(x86::xmmword_ptr(fillPtr, fillOfs + 16), x86::xmm0);
      fillOfs += 32;
      regsToFill -= 4;
    }
  }
  // Fill any excess registers.
  if (regsToFill >= 2) {
    a.vmovups(x86::xmmword_ptr(fillPtr, fillOfs), x86::xmm0);
    fillOfs += 16;
    regsToFill -= 2;
  }
  if (regsToFill > 0) {
    assert(regsToFill == 1 && "All regs must be filled");
    a.vmovsd(x86::qword_ptr(fillPtr, fillOfs), x86::xmm0);
  }

  // Create the slow path for throwing a register stack overflow.
  slowPaths_.emplace_back(
      registerOverflowLab,
      /* emittingIP */ nullptr,
      [](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: _sh_throw_register_stack_overflow");
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        // Do not save the IP because we have not yet set up the stack frame
        // for this function. The exception should appear in the caller.
        EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
            em, void (*)(SHRuntime *), _sh_throw_register_stack_overflow);
      });

  if (catchTableLabel_.isValid()) {
    comment("// _sh_try");
    int32_t jmpBufOffset = (int32_t)getJmpBufOffset();
    // buf->prev = shr->shCurJmpBuf;
    a.mov(x86::rax, x86::qword_ptr(xRuntime, offsetof(SHRuntime, shCurJmpBuf)));
    a.mov(
        x86::qword_ptr(x86::rsp, jmpBufOffset + offsetof(SHJmpBuf, prev)),
        x86::rax);

    // shr->shCurJmpBuf = buf;
    a.lea(x86::rax, x86::ptr(x86::rsp, jmpBufOffset));
    a.mov(x86::qword_ptr(xRuntime, offsetof(SHRuntime, shCurJmpBuf)), x86::rax);

    // _setjmp(buf->buf);
    a.lea(
        x86::rdi,
        x86::ptr(x86::rsp, jmpBufOffset + (int32_t)offsetof(SHJmpBuf, buf)));
    // setjmp can't throw, so the IP does not need to be saved.
    EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(*this, int (*)(jmp_buf), _sh_setjmp);
    // If this a catch, go to the catch table to jump to either a handler BB or
    // rethrow.
    a.test(x86::eax, x86::eax);
    a.jnz(catchTableLabel_);
  }

  if (dumpJitCode_ & DumpJitCode::EntryExit) {
    comment("// print entry");
    a.mov(x86::edi, 1);
    a.lea(x86::rsi, x86::ptr(roDataLabel_, getDebugFunctionName()));
    EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
        *this, void (*)(bool, const char *), _sh_print_function_entry_exit);
  }
}

void Emitter::leave(llvh::ArrayRef<const asmjit::Label *> exceptionHandlers) {
  comment("// leaveFrame");
  a.bind(returnLabel_);
  if (dumpJitCode_ & DumpJitCode::EntryExit) {
    comment("// print exit");
    a.mov(x86::edi, 0);
    a.lea(x86::rsi, x86::ptr(roDataLabel_, getDebugFunctionName()));
    EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
        *this, void (*)(bool, const char *), _sh_print_function_entry_exit);
  }

  // x86-64: rsp must not still be transiently lowered for stack arguments,
  // either when the _sh_end_try below reads the SHJmpBuf through rsp or when
  // the epilogue starts unwinding it -- see the rspDelta_ contract on
  // callImpl().
  assert(rspDelta_ == 0 && "rsp delta not restored before epilogue");

  if (catchTableLabel_.isValid()) {
    comment("// _sh_end_try");
    // shr->shCurJmpBuf = buf->prev
    int32_t jmpBufOffset = (int32_t)getJmpBufOffset();
    a.mov(
        x86::rax,
        x86::qword_ptr(x86::rsp, jmpBufOffset + offsetof(SHJmpBuf, prev)));
    a.mov(x86::qword_ptr(xRuntime, offsetof(SHRuntime, shCurJmpBuf)), x86::rax);
  }

  // _sh_leave(shr, &locals.head, frame);
  // Restore the previous stack frame.
  a.mov(x86::qword_ptr(xRuntime, RuntimeOffsets::stackPointer), xFrame);
  a.mov(
      x86::rax,
      x86::qword_ptr(
          xFrame,
          StackFrameLayout::PreviousFrame * (int)sizeof(SHLegacyValue)));
  a.mov(x86::qword_ptr(xRuntime, RuntimeOffsets::currentFrame), x86::rax);

  // The return value has been stashed in rbx by ret(). Move it to the return
  // register.
  a.mov(x86::rax, x86::rbx);

  // Restore the stack in the exact reverse of the prologue.
  if (uint32_t stackSize = getStackSize())
    a.add(x86::rsp, stackSize);
  for (unsigned i = gpSaveCount_; i > 0; --i)
    a.pop(x86::gpq(kGPSavedList[i - 1]));
  a.pop(xFrame);
  a.pop(xRuntime);
  a.pop(x86::rbp);

  a.ret();

  emitCatchTable(exceptionHandlers);
  emitSlowPaths();
  emitTypeAssertFailTail();
  emitROData();
}

void Emitter::newBasicBlock(const asmjit::Label &label) {
  assert(
      typeAssertPendingWrites_.empty() &&
      "pending type asserts must be drained at each instruction");
  syncAllFRTempExcept({});
  freeAllFRTempExcept({});

  // Clear all local types and regs when starting a new basic block.
  // TODO: there must be a faster way to do this when there are many regs.
  for (FRState &frState : frameRegs_) {
    frState.localType = frState.globalType;
    assert(!frState.localGpX);
    assert(!frState.localVecD);
    if (frState.globalReg)
      frState.frameUpToDate = false;
  }

  a.bind(label);
}

void Emitter::getBytecodeIP(const x86::Gp &out) {
  // x86-64: the IP is a single 64-bit immediate, so there is no analogue of
  // arm64's "materialize the function start, then add the offset" chain, and
  // no immediate-range special case for offsets above 16MB.
  loadBits64InGp(
      out,
      (uint64_t)codeBlock_->begin() + codeBlock_->getOffsetOf(emittingIP),
      "Bytecode IP");
}

void Emitter::emitIncrementCounter(JitCounter counter) {
  if (!emitCounters_)
    return;
  // x86-64: arm64 has to load, add and store through two registers, which it
  // makes room for by pushing them; x86 increments memory in place, so the
  // only register needed is the one holding the counter array pointer, and
  // xScratch is reserved for exactly this kind of transient use. The counter
  // array is non-null whenever emitCounters_ is set (JITContext::setEmit-
  // Counters allocates it), so there is no null check here, just as on arm64.
  //
  // Unlike arm64's `add`, `inc` writes EFLAGS. Both call sites -- the top of
  // callImpl and the head of its slow path -- have no live flags.
  a.mov(xScratch, x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeJitCounters));
  a.inc(
      x86::qword_ptr(
          xScratch, (int32_t)((unsigned)counter * sizeof(uint64_t))));
}

void Emitter::callRuntimeWithSavedIP(void *fn, const char *name) {
  // Save the current IP in the runtime.
  getBytecodeIP(xScratch);
  a.mov(x86::qword_ptr(xRuntime, RuntimeOffsets::currentIP), xScratch);

  // Call the passed function.
  callRuntime(fn, name);

  if (emitAsserts_) {
    // Invalidate the current IP to make sure it is set before the next call.
    a.mov(xScratch, asmjit::Imm(Runtime::kInvalidCurrentIP));
    a.mov(x86::qword_ptr(xRuntime, RuntimeOffsets::currentIP), xScratch);
  }
}

void Emitter::callRuntime(void *fn, const char *name) {
  // x86-64: every runtime call, whether reached directly or through
  // callRuntimeWithSavedIP(), funnels through here, so a single check
  // covers the "rspDelta_ contract" for both -- see callImpl().
  assert(rspDelta_ % 16 == 0 && "rsp not 16-byte aligned at call emission");
  comment("// call %s", name);
  loadBits64InGp(xScratch, (uint64_t)fn, name);
  a.call(xScratch);
}

uint16_t Emitter::initHCLazyIDMayAlloc(HiddenClass *hc) {
  // Callers pass the result of WeakRoot::get(), which is null if the GC has
  // cleared the root. Since 0 already means "no id" and every caller checks
  // for it, tolerating null here keeps all present and future call sites safe
  // without each of them having to re-validate across safepoints.
  if (!hc)
    return 0;

  uint16_t id = hc->getLazyJITId();
  // Assign a new ID to the HC if we have to.
  if (id != 0)
    return id;

  // Too many IDs. Fail.
  if (jitImpl_.prevHCId >= jitImpl_.hcIdLimit)
    return 0;

  struct : Locals {
    PinnedValue<HiddenClass> hc;
  } lv;
  LocalsRAII lraii{runtime_, &lv};
  lv.hc = hc;

  if (jitImpl_.usedHCs.isUndefined()) {
    CallResult<HermesValue> cr = ArrayStorageSmall::create(runtime_, 8);
    if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
      // Failing to pin is not fatal: report "no id" and let the caller fall
      // back to a non-specialized path. Swallow the pending OOM, since we are
      // in the compiler and there is nobody to propagate it to.
      runtime_.clearThrownValue();
      return 0;
    }
    // We would like to use a long-lived object, but we can't, because
    // ArrayStorage cannot be long-lived: it can be allocated that way
    // initially, but when it grows, it is allocated "normally".
    jitImpl_.usedHCs = *cr;
  }

  // Pin the class before assigning the id, so that the invariant
  // "id != 0 implies the class is in usedHCs" cannot be broken by a failed
  // allocation. usedHCs is the only strong root for these classes, and the id
  // is baked into immutable machine code, so an id on an unpinned class would
  // be a dangling reference.
  auto mh = MutableHandle<ArrayStorageSmall>::vmcast(&jitImpl_.usedHCs);
  if (LLVM_UNLIKELY(
          ArrayStorageSmall::push_back(mh, runtime_, lv.hc) ==
          ExecutionStatus::EXCEPTION)) {
    // Failing to pin is not fatal: report "no id" and let the caller fall
    // back to a non-specialized path. Swallow the pending OOM, since we are
    // in the compiler and there is nobody to propagate it to.
    runtime_.clearThrownValue();
    return 0;
  }

  id = ++jitImpl_.prevHCId;
  // Note: use the pinned handle rather than the raw argument, which the
  // allocation above may have invalidated by moving the object.
  lv.hc->setLazyJITId(id);

  return id;
}

void Emitter::emitSlowPaths() {
  while (!slowPaths_.empty()) {
    SlowPath &sp = slowPaths_.front();
    emittingIP = sp.emittingIP;
    sp.emit(*this);
    slowPaths_.pop_front();
  }
  emittingIP = nullptr;
}

void Emitter::emitTypeAssert(FR fr, HWReg hwVal, TypePred pred) {
  if (LLVM_LIKELY(!emitTypeAsserts_))
    return;
  comment("// type assert r%u is %s", fr.index(), typePredName(pred));
  if (hwVal.isVecD()) {
    // x86-64: arm64's fmov bit-reinterprets a vector register into a GpX
    // in place; the x86 analogue crossing register files is vmovq.
    a.vmovq(xScratch, hwVal.xmm());
    emitTypeAssertGpX(fr, xScratch, pred);
  } else {
    emitTypeAssertGpX(fr, hwVal.gpq(), pred);
  }
}

void Emitter::emitTypeAssertFR(FR fr, TypePred pred) {
  if (LLVM_LIKELY(!emitTypeAsserts_))
    return;
  comment("// type assert r%u is %s", fr.index(), typePredName(pred));
  readFRForAssert(fr);
  emitTypeAssertGpX(fr, xScratch, pred);
}

void Emitter::emitPendingTypeAssertsSlow() {
  assert(!typeAssertPendingWrites_.empty() && "nothing to emit");
  for (FR fr : typeAssertPendingWrites_) {
    FRState &frState = frameRegs_[fr.index()];
    TypePred pred = frState.globalType == FRType::Number ? TypePred::IsNumber
                                                         : TypePred::NotPointer;
    emitTypeAssertFR(fr, pred);
  }
  typeAssertPendingWrites_.clear();
}

void Emitter::emitTypeAssertGpX(FR fr, const x86::Gp &val, TypePred pred) {
  assert(emitTypeAsserts_ && "caller must check emitTypeAsserts_");
  if (!typeAssertSites_) {
    typeAssertSites_ = &jitImpl_.typeAssertSites.emplace_back();
    typeAssertFailLab_ = newPrefLabel("TYPEASSERT_FAIL", 0);
  }

  uint32_t idx = (uint32_t)typeAssertSites_->size();
  typeAssertSites_->push_back(
      TypeAssertSite{
          codeBlock_,
          codeBlock_->getOffsetOf(emittingIP),
          (uint16_t)fr.index(),
          pred});

  asmjit::Label failLab = newPrefLabel("TYPEASSERT_", idx);

  // x86-64: arm64 tests IsNumber/BitComparable's "is this a double" step by
  // comparing the raw 64-bit value against the shifted tag boundary
  // (emit_sh_ljs_is_double), which needs a second register to hold that
  // 64-bit constant, distinct from val -- arm64 has one (xScratch2), this
  // backend does not. Rather than borrow one from the temp allocator (which
  // would be unsafe here: several call sites, e.g. jStrictEqual's raw-bit
  // path, free their operand registers back to the allocator before their
  // own final use of them, relying on no intervening allocation stealing
  // that exact register), the check below extracts the tag into xScratch
  // with the same non-destructive get_tag used by NotPointer -- val is
  // never written -- and compares *that* against HVTag_First as a small
  // sign-extended immediate. This is exactly equivalent to the raw-value
  // form: is_double's threshold has zero low 48 bits, so unsigned
  // `val < (HVTag_First << 48)` holds iff unsigned `(val >> 48) <
  // HVTag_First` (sar's sign extension commutes with the shift on both
  // sides of the comparison). The whole predicate table below therefore
  // uses only xScratch, exactly as arm64 uses only its two dedicated
  // scratch registers -- a pure insertion that never touches the
  // allocator.
  switch (pred) {
    case TypePred::IsNumber:
      emit_sh_ljs_get_tag(a, xScratch, val);
      a.cmp(xScratch, asmjit::Imm(HVTag_First));
      a.jae(failLab);
      break;
    case TypePred::IsBool:
      emit_sh_ljs_is_bool(a, xScratch, val);
      a.jne(failLab);
      break;
    case TypePred::NotPointer:
      emit_sh_ljs_get_tag(a, xScratch, val);
      emit_sh_ljs_tag_is_pointer(a, xScratch);
      a.jae(failLab);
      break;
    case TypePred::BitComparable:
      emit_sh_ljs_get_tag(a, xScratch, val);
      a.cmp(xScratch, asmjit::Imm(HVTag_First));
      a.jb(failLab);
      emit_sh_ljs_tag_is_pointer(a, xScratch);
      a.jae(failLab);
      break;
    case TypePred::IsObject:
      emit_sh_ljs_is_object(a, xScratch, val);
      a.jne(failLab);
      break;
  }

  slowPaths_.emplace_back(
      failLab, emittingIP, [idx](Emitter &em, SlowPath &sp) {
        em.comment("// Type assert failure %u", idx);
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::edi, asmjit::Imm(idx));
        em.a.jmp(em.typeAssertFailLab_);
      });
}

void Emitter::readFRForAssert(FR fr) {
  assert(emitTypeAsserts_ && "caller must check emitTypeAsserts_");
  FRState &frState = frameRegs_[fr.index()];
  assert(!frState.regIsDirty && "reading an FR that is about to be written");

  // Locals always hold the latest value; a global reg holds it only if
  // globalRegUpToDate; otherwise the frame must be up to date.
  if (frState.localGpX) {
    a.mov(xScratch, frState.localGpX.gpq());
  } else if (frState.localVecD) {
    a.vmovq(xScratch, frState.localVecD.xmm());
  } else if (frState.globalReg && frState.globalRegUpToDate) {
    // x86-64: globals are GP-only here (see kGPSavedList; there are no
    // callee-saved XMM registers in SysV), so the isGpX() split's else arm
    // is unreachable on this backend. Kept for textual parity with arm64,
    // which does have vector globals.
    if (frState.globalReg.isGpX())
      a.mov(xScratch, frState.globalReg.gpq());
    else
      a.vmovq(xScratch, frState.globalReg.xmm());
  } else {
    assert(frState.frameUpToDate && "FR has no up-to-date location");
    _loadFrame(HWReg(xScratch), fr);
  }
}

void Emitter::emitTypeAssertFailTail() {
  if (!typeAssertSites_)
    return;
  comment("// Type assert failure tail");
  a.bind(typeAssertFailLab_);
  // edi already holds the site index, set by the per-site stub. SysV's
  // second integer argument register is rsi.
  a.mov(
      x86::rsi,
      roConst64((uint64_t)typeAssertSites_, "type assert site table"));
  // Not EMIT_RUNTIME_CALL: that saves the current IP, and emitSlowPaths()
  // has already cleared emittingIP by the time this runs. The handler does
  // not need the IP anyway - the site record carries the bytecode offset.
  EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
      *this,
      void (*)(uint32_t, const std::vector<TypeAssertSite> *),
      _jit_type_assert_failed);
}

void Emitter::unreachable() {
  EMIT_RUNTIME_CALL(*this, void (*)(), _sh_unreachable);
}

void Emitter::profilePoint(uint16_t pointIndex) {
  comment("// ProfilePoint %u", pointIndex);
#ifdef HERMESVM_PROFILER_BB
  syncAllFRTempExcept({});
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  a.mov(x86::esi, asmjit::Imm(pointIndex));
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(SHRuntime *, uint16_t),
      _interpreter_register_bb_execution);
#else
  // No-op if profiling is not enabled.
#endif
}

void Emitter::directEval(FR frRes, FR frText, bool strictCaller) {
  comment("// DirectEval r%u, r%u", frRes.index(), frText.index());
  syncAllFRTempExcept({});
  syncToFrame(frText);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frText);
  a.mov(x86::edx, asmjit::Imm(strictCaller));
  EMIT_RUNTIME_CALL(
      *this,
      HermesValue (*)(Runtime &, PinnedHermesValue *, bool),
      _jit_direct_eval);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<true>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::mov(FR frRes, FR frInput, bool logComment) {
  // Sometimes mov() is used by other instructions, so logging is optional.
  if (logComment)
    comment("// %s r%u, r%u", "mov", frRes.index(), frInput.index());
  if (frRes == frInput)
    return;

  HWReg hwInput = getOrAllocFRInAnyReg(frInput, true);
  HWReg hwDest = getOrAllocFRInAnyReg(frRes, false);
  movHWFromHW<false>(hwDest, hwInput);
  frUpdatedWithHW(frRes, hwDest, frameRegs_[frInput.index()].localType);
}

void Emitter::loadParam(FR frRes, uint32_t paramIndex) {
  comment("// LoadParam r%u, %u", frRes.index(), paramIndex);

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  HWReg hwTmp = allocAndLogTempGpX();
  x86::Gp wTmp = hwTmp.gpq().r32();

  a.mov(
      wTmp,
      x86::dword_ptr(
          xFrame,
          (int)StackFrameLayout::ArgCount * (int)sizeof(SHLegacyValue)));

  // x86-64: cmp takes the immediate directly, so arm64's isAddSubImm
  // fallback through a second temp has no analogue.
  a.cmp(wTmp, paramIndex);
  a.jb(slowPathLab);

  freeReg(hwTmp);

  // freeReg(hwTmp) above guarantees a free GP temp, so this allocation
  // cannot spill. That matters here specifically: a spill store would be
  // emitted after the `jb` to the slow path above, so it would be skipped
  // whenever the slow path is taken, desynchronizing the allocator's view
  // of the spilled FR from the value actually left on the stack.
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);

  // Compute the frame offset in 64 bits. paramIndex is a UInt32 operand of
  // LoadParamLong, and (ThisArg - paramIndex) * sizeof(SHLegacyValue)
  // overflows int32 from paramIndex 2^28 upwards, long before that range is
  // exhausted.
  int64_t ofs64 = ((int64_t)StackFrameLayout::ThisArg - (int64_t)paramIndex) *
      (int64_t)sizeof(SHLegacyValue);
  assert(ofs64 < 0 && "frame offset of a parameter must be negative");

  if (LLVM_UNLIKELY(ofs64 <= std::numeric_limits<int32_t>::min())) {
    // The parameter is so far away that no argument count can reach it, so
    // the comparison above always branches. Emit the branch unconditionally
    // rather than a fast path that cannot be reached and whose offset would
    // not encode; the slow path yields undefined, which is the right answer.
    a.jmp(slowPathLab);
  } else {
    // x86-64: any int32 displacement encodes, so unlike arm64 there is no
    // narrower-immediate form to try first and no fallback.
    a.mov(hwRes.gpq(), x86::qword_ptr(xFrame, (int32_t)ofs64));
  }

  a.bind(contLab);
  frUpdatedWithHW(frRes, hwRes);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, hwRes](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: LoadParam r%u", frRes.index());
        em.a.bind(sp.slowPathLab);
        em.loadBits64InGp(hwRes.gpq(), _sh_ljs_undefined().raw, "undefined");
        em.a.jmp(sp.contLab);
      });
}

void Emitter::getGlobalObject(FR frRes) {
  comment("// GetGlobalObject r%u", frRes.index());
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWFromMem(hwRes, x86::ptr(xRuntime, RuntimeOffsets::globalObject));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::declareGlobalVar(SHSymbolID symID) {
  comment("// DeclareGlobalVar %u", symID);

  syncAllFRTempExcept({});
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  a.mov(x86::esi, asmjit::Imm(symID));
  EMIT_RUNTIME_CALL(
      *this, void (*)(SHRuntime *, SHSymbolID), _sh_ljs_declare_global_var);
}

void Emitter::debugger() {
  comment("// Debugger");
  // x86-64: int3 is the breakpoint trap, standing in for arm64's brk #0.
  if (dumpJitCode_ & DumpJitCode::BRK)
    a.int3();
}

void Emitter::createRegExp(
    FR frRes,
    SHSymbolID patternID,
    SHSymbolID flagsID,
    uint32_t regexpID) {
  comment("// CreateRegExp r%u, %u, %u", frRes.index(), patternID, flagsID);

  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadBits64InGp(x86::rsi, (uint64_t)codeBlock_, "CodeBlock");
  a.mov(x86::edx, asmjit::Imm(patternID));
  a.mov(x86::ecx, asmjit::Imm(flagsID));
  a.mov(x86::r8d, asmjit::Imm(regexpID));
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *, SHCodeBlock *, uint32_t, uint32_t, uint32_t),
      _interpreter_create_regexp);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

asmjit::Label Emitter::newPrefLabel(const char *pref, size_t index) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%s%lu", pref, index);
  return a.newNamedLabel(buf);
}

int32_t Emitter::reserveData(
    int32_t dsize,
    size_t align,
    asmjit::TypeId typeId,
    int32_t itemCount,
    const char *comment) {
  // Align the new data.
  size_t oldSize = roData_.size();
  size_t dataOfs = (roData_.size() + align - 1) & ~(align - 1);
  if (dataOfs >= INT32_MAX)
    hermes::hermes_fatal("JIT RO data overflow");
  // Grow to include the data.
  roData_.resize(dataOfs + dsize);

  // If logging is enabled, generate data descriptors.
  if (hasLogger()) {
    // Optional padding descriptor.
    if (dataOfs != oldSize) {
      int32_t gap = (int32_t)(dataOfs - oldSize);
      roDataDesc_.push_back(
          {.size = gap, .typeId = asmjit::TypeId::kUInt8, .itemCount = gap});
    }

    roDataDesc_.push_back(
        {.size = dsize,
         .typeId = typeId,
         .itemCount = itemCount,
         .comment = comment});
  }

  return (int32_t)dataOfs;
}

/// Register a 64-bit constant in RO DATA and return its offset.
int32_t Emitter::uint64Const(uint64_t bits, const char *comment) {
  auto [it, inserted] = fp64ConstMap_.try_emplace(bits, 0);
  if (inserted) {
    int32_t dataOfs = reserveData(
        sizeof(double), sizeof(double), asmjit::TypeId::kFloat64, 1, comment);
    memcpy(roData_.data() + dataOfs, &bits, sizeof(double));
    it->second = dataOfs;
  }
  return it->second;
}

void Emitter::emitCatchTable(
    llvh::ArrayRef<const asmjit::Label *> exceptionHandlers) {
  // No trys in the function, nothing to do here.
  if (!catchTableLabel_.isValid())
    return;

  a.bind(catchTableLabel_);

  asmjit::Label addressTableLab = a.newLabel();

  // This block is reached only by falling out of the setjmp in frameSetup()
  // with a non-zero return, i.e. by longjmp. longjmp restores rsp to its
  // value at the setjmp, which is the post-prologue rsp, so the two
  // rsp-relative accesses below are correct no matter what the throwing code
  // was doing with rsp -- including a throw from inside putByIdImpl's two
  // pushes, where the emitter's rspDelta_ was transiently non-zero. The same
  // reasoning covers the ASan pushes in bumpAllocAndUnpoison(); see the
  // rspDelta_ comment in JitEmitter.h.
  //
  // The SHJmpBuf sits at rsp + 0, and frameSetup() puts the alignment
  // padding ABOVE the exception area rather than below it, so the SHJmpBuf
  // is 16-byte aligned whatever the parity of the pushes. That is more than
  // _sh_setjmp's jmp_buf actually requires on x86-64 (alignof(jmp_buf) ==
  // 8) -- it is free headroom, not a correctness requirement -- but it is
  // also the alignment arm64 gets for free because its sp is
  // architecturally 16-byte aligned, so keeping it costs nothing and keeps
  // the two backends' invariants easy to compare.

  // Find the catch target for the exception.
  a.mov(x86::rdi, xRuntime);
  loadBits64InGp(x86::rsi, (uint64_t)codeBlock_, "CodeBlock");
  a.mov(x86::rdx, xFrame);
  a.lea(x86::rcx, x86::ptr(x86::rsp, (int32_t)getJmpBufOffset()));
  a.mov(x86::r8, x86::qword_ptr(x86::rsp, (int32_t)getSavedSHLocalsOffset()));
  // x86-64: arm64's pc-relative `adr` is a RIP-relative `lea` here.
  a.lea(x86::r9, x86::ptr(addressTableLab));
  EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
      *this,
      void *(*)(SHRuntime *,
                SHCodeBlock *,
                SHLegacyValue *,
                SHJmpBuf *,
                SHLocals *,
                int32_t *),
      _jit_find_catch_target);

  // The address to branch to was returned here.
  a.jmp(x86::rax);

  // Table of offsets from addressTableLab to jump to.
  //
  // x86-64: arm64 gets this for free because every instruction is four bytes
  // wide; here the table follows a variable-length `jmp`, so align it
  // explicitly -- _jit_find_catch_target() reads it through an int32_t *.
  // The section base is at least 4-byte aligned, so in-section alignment is
  // absolute alignment.
  a.align(asmjit::AlignMode::kData, 4);
  a.bind(addressTableLab);
  for (const asmjit::Label *handler : exceptionHandlers) {
    a.embedLabelDelta(*handler, addressTableLab, /* size */ 4);
  }
}

void Emitter::emitROData() {
  a.bind(roDataLabel_);
  if (!hasLogger()) {
    a.embed(roData_.data(), roData_.size());
  } else {
    int32_t ofs = 0;
    for (const auto &desc : roDataDesc_) {
      if (desc.comment)
        comment("// %s", desc.comment);
      a.embedDataArray(desc.typeId, roData_.data() + ofs, desc.itemCount);
      ofs += desc.size;
    }
  }
}

void Emitter::unsupported(const char *what) {
  char buf[128];
  snprintf(buf, sizeof(buf), "unsupported instruction: %s", what);
  // Route through the same expected-error / longjmp mechanism as a genuine
  // AsmJit error: reportError() calls OurErrorHandler::handleError(), which
  // (since this error code never matches expectedError_) formats the
  // message and invokes the longjmp callback passed to the constructor,
  // unwinding to JITContext::Compiler::compileCodeBlock() and abandoning
  // compilation of this function.
  a.reportError(asmjit::kErrorInvalidState, buf);
  hermes::hermes_fatal("jit: unsupported() unexpectedly returned");
}

} // namespace hermes::vm::x86_64
#endif // HERMESVM_JIT_X86_64
