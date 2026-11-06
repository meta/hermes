/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_ARM64
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

namespace hermes::vm::arm64 {

// Disable warnings about missing designated initializers, which the
// roDataDesc_ entries below rely on.
#ifdef __clang__
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#endif

llvh::raw_ostream &operator<<(
    llvh::raw_ostream &os,
    const hermes::vm::arm64::HWReg &hwReg) {
  if (hwReg.isValidGpX()) {
    os << "x" << (int)hwReg.indexInClass();
  } else if (hwReg.isValidVecD()) {
    os << "d" << (int)hwReg.indexInClass();
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
    JitVersionData *versionData,
    const std::function<void(std::string &&message)> &longjmpError)
    : runtime_(runtime),
      jitImpl_(jitImpl),
      dumpJitCode_(dumpJitCode),
      emitAsserts_(emitAsserts),
      emitTypeAsserts_(emitTypeAsserts),
      emitCounters_(emitCounters),
      frameRegs_(codeBlock->getFrameSize()),
      codeBlock_(codeBlock),
      versionData_(versionData) {
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

void Emitter::enter(uint32_t numCount, uint32_t npCount) {
  unsigned nextVec = kVecSaved.first;
  unsigned nextGp = kGPSaved.first;

  // Number registers: allocate in vector hw regs first.
  for (unsigned frIndex = 0; frIndex < numCount; ++frIndex) {
    HWReg hwReg;
    if (nextVec <= kVecSaved.second) {
      hwReg = HWReg::vecD(nextVec);
      comment("    ; alloc: d%u <= r%u", nextVec, frIndex);
      ++nextVec;
    } else if (nextGp <= kGPSaved.second) {
      hwReg = HWReg::gpX(nextGp);
      comment("    ; alloc: x%u <= r%u", nextGp, frIndex);
      ++nextGp;
    } else
      break;

    frameRegs_[frIndex].globalReg = hwReg;
    frameRegs_[frIndex].globalType = FRType::Number;
  }
  // Non-pointer regs: allocate in gp regs first.
  for (unsigned frIndex = numCount; frIndex < npCount + numCount; ++frIndex) {
    HWReg hwReg;
    if (nextGp <= kGPSaved.second) {
      hwReg = HWReg::gpX(nextGp);
      comment("    ; alloc: x%u <= r%u", nextGp, frIndex);
      ++nextGp;
    } else if (nextVec <= kVecSaved.second) {
      hwReg = HWReg::vecD(nextVec);
      comment("    ; alloc: d%u <= r%u", nextVec, frIndex);
      ++nextVec;
    } else
      break;

    frameRegs_[frIndex].globalReg = hwReg;
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

  frameSetup(
      frameRegs_.size(), nextGp - kGPSaved.first, nextVec - kVecSaved.first);
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
  for (const auto &frState : frameRegs_)
    assert(!frState.regIsDirty && "Frame register is dirty");

  // Check that any temps have an associated FR.
  for (unsigned i = kGPTemp.first; i <= kGPTemp.second; ++i) {
    HWReg hwReg(i, HWReg::GpX{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid()) {
      assert(gpTemp_.isAllocated(i) && "Temp register is not freed");
    }
  }

  for (unsigned i = kVecTemp1.first; i <= kVecTemp2.second; ++i) {
    if (i > kVecTemp1.second && i < kVecTemp2.first)
      continue;
    HWReg hwReg(i, HWReg::VecD{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid()) {
      assert(vecTemp_.isAllocated(i) && "Temp register is not freed");
    }
  }
}
#endif

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

void Emitter::frameSetup(
    unsigned numFrameRegs,
    unsigned gpSaveCount,
    unsigned vecSaveCount) {
  assert(
      gpSaveCount <= kGPSaved.second - kGPSaved.first + 1 &&
      "Too many callee saved GP regs");
  assert(
      vecSaveCount <= kVecSaved.second - kVecSaved.first + 1 &&
      "Too many callee saved Vec regs");

  static_assert(
      kGPSaved.first == 21, "Callee saved GP regs must start from x21");
  // Always save x21 even if it is not needed for an FR because we use it for
  // the return value.
  if (gpSaveCount == 0)
    gpSaveCount = 1;
  // We always save x19 and x20 since they are used for xRuntime and xFrame.
  gpSaveCount += 2;

  gpSaveCount_ = gpSaveCount;
  vecSaveCount_ = vecSaveCount;

  // Higher addresses are at the top.
  // +-----------------------------+<---- old sp
  // |             x30             |
  // +-----------------------------+
  // |             x29             |
  // +-----------------------------+<---- new x29
  // |             ...             |
  // +-----------------------------+
  // |             x21             |
  // +-----------------------------+
  // |             x20             |
  // +-----------------------------+
  // |             x19             |
  // +-----------------------------+
  // |  Saved SHLocals* (optional) |
  // +-----------------------------+
  // |      SHJmpBuf (optional)    |
  // +-----------------------------+<--- new sp
  a.sub(a64::sp, a64::sp, getStackSize());

  unsigned stackOfs = getSavedRegsOffset();
  for (unsigned i = 0; i < gpSaveCount; i += 2, stackOfs += 16) {
    if (i + 1 < gpSaveCount)
      a.stp(a64::GpX(19 + i), a64::GpX(20 + i), a64::Mem(a64::sp, stackOfs));
    else
      a.str(a64::GpX(19 + i), a64::Mem(a64::sp, stackOfs));
  }
  for (unsigned i = 0; i < vecSaveCount; i += 2, stackOfs += 16) {
    if (i + 1 < vecSaveCount)
      a.stp(
          a64::VecD(kVecSaved.first + i),
          a64::VecD(kVecSaved.first + 1 + i),
          a64::Mem(a64::sp, stackOfs));
    else
      a.str(a64::VecD(kVecSaved.first + i), a64::Mem(a64::sp, stackOfs));
  }
  a.stp(a64::x29, a64::x30, a64::Mem(a64::sp, stackOfs));
  a.add(a64::x29, a64::sp, stackOfs);

  comment("// xRuntime");
  a.mov(xRuntime, a64::x0);

  // Save the SHLocals pointer because we don't allocate and push a new
  // SHLocals in the JIT.
  // Used in CatchInst to restore state.
  if (catchTableLabel_.isValid()) {
    comment("// saved SHLocals *");
    a.ldr(a64::x0, a64::Mem(xRuntime, RuntimeOffsets::shLocals));
    a.str(a64::x0, a64::Mem(a64::sp, getSavedSHLocalsOffset()));
  }

#ifndef HERMES_CHECK_NATIVE_STACK
#error Only native stack checking is supported in the JIT
#endif

  comment("// _sh_check_native_stack_overflow");
  asmjit::Label nativeOverflowLab = newSlowPathLabel();
  asmjit::Label nativeOverflowContLab = newContLabel();
  // Get the stack bounds from the runtime.
  a.ldr(a64::x0, a64::Mem(xRuntime, RuntimeOffsets::nativeStackHigh));
  a.ldr(a64::x1, a64::Mem(xRuntime, RuntimeOffsets::nativeStackSize));
  // Subtract the frame pointer from nativeStackHigh and compare it against the
  // size. If the difference is less than the stack size, then we are still
  // within the current stack bounds.
  a.sub(a64::x0, a64::x0, a64::x29);
  a.cmp(a64::x0, a64::x1);
  // If the frame pointer is within bounds, we are done. Otherwise, we need to
  // check if the bounds have changed.
  a.b_hi(nativeOverflowLab);
  a.bind(nativeOverflowContLab);
  slowPaths_.emplace_back(
      nativeOverflowLab,
      nativeOverflowContLab,
      /* emittingIP */ nullptr,
      [](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: _sh_check_native_stack_overflow");
        em.a.bind(sp.slowPathLab);
        em.a.mov(a64::x0, xRuntime);
        // Do not save the IP because we have not yet set up the stack frame
        // for this function. If this throws, the exception should appear in
        // the caller.
        EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
            em, void (*)(SHRuntime *), _sh_check_native_stack_overflow);
        em.a.b(sp.contLab);
      });

  comment("// xFrame");
  a.ldr(xFrame, a64::Mem(xRuntime, RuntimeOffsets::stackPointer));

  // If the function has a prohibitInvoke flag, we need to check if it has been
  // called correctly.
  auto prohibitInvoke = codeBlock_->getHeaderFlags().getProhibitInvoke();
  if (prohibitInvoke != ProhibitInvoke::None) {
    // Load new.target.
    a.ldur(
        a64::x0,
        a64::Mem(
            xFrame, StackFrameLayout::NewTarget * (int)sizeof(SHLegacyValue)));
    // Compare new.target against undefined.
    emit_sh_ljs_is_undefined(a, a64::x0, a64::x0);

    void (*slowCall)(SHRuntime *);
    const char *slowCallName;
    asmjit::Label throwInvalidInvokeLab;
    if (prohibitInvoke == ProhibitInvoke::Call) {
      // If regular calls are prohibited, then we jump to throwInvalidInvoke if
      // new.target is undefined.
      throwInvalidInvokeLab = a.newNamedLabel("throwInvalidCall");
      a.b_eq(throwInvalidInvokeLab);

      slowCall = _sh_throw_invalid_call;
      slowCallName = "_sh_throw_invalid_call";
    } else {
      assert(
          prohibitInvoke == ProhibitInvoke::Construct &&
          "Unknown prohibitInvoke");
      // If construct calls are prohibited, then we jump to throwInvalidInvoke
      // if new.target is not undefined.
      throwInvalidInvokeLab = a.newNamedLabel("throwInvalidConstruct");
      a.b_ne(throwInvalidInvokeLab);

      slowCall = _sh_throw_invalid_construct;
      slowCallName = "_sh_throw_invalid_construct";
    }

    slowPaths_.emplace_back(
        throwInvalidInvokeLab,
        /* emittingIP */ nullptr,
        [slowCall, slowCallName](Emitter &em, SlowPath &sp) {
          em.comment("// Slow path: %s", slowCallName);
          em.a.bind(sp.slowPathLab);
          em.a.mov(a64::x0, xRuntime);
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
  a.ldr(a64::x0, a64::Mem(xRuntime, RuntimeOffsets::registerStackEnd));
  a.sub(a64::x0, a64::x0, xFrame);
  // Check if we need more registers than remain.
  size_t totalRegsToAlloc = numFrameRegs + hbc::StackFrameLayout::FirstLocal;
  size_t regAllocSize = totalRegsToAlloc * sizeof(SHLegacyValue);
  // NOTE: cmp has the same immediate field type as add/sub, so we can use the
  // same utility function.
  if (a64::Utils::isAddSubImm(regAllocSize)) {
    a.cmp(a64::x0, regAllocSize);
    a.b_lo(registerOverflowLab);
    a.add(a64::x0, xFrame, regAllocSize);
  } else {
    a.mov(a64::x1, regAllocSize);
    a.cmp(a64::x0, a64::x1);
    a.b_lo(registerOverflowLab);
    a.add(a64::x0, xFrame, a64::x1);
  }

  // Advance the register stack.
  a.str(a64::x0, a64::Mem(xRuntime, RuntimeOffsets::stackPointer));
  a.str(xFrame, a64::Mem(xRuntime, RuntimeOffsets::currentFrame));

  static_assert(
      HERMESVALUE_VERSION == 2, "Raw zero value must be ignored by GC");
  // Initialize the pointer to the current set of registers.
  a.mov(a64::x0, xFrame);
  size_t regsToFill = totalRegsToAlloc;
  // Fill the registers with zero in groups of 4, then 2, then 1.
  // If there are more than 32 registers, start with a loop.
  if (regsToFill > 32) {
    a.movi(a64::v0.d2(), 0);
    // We will fill 4 registers on each iteration.
    unsigned loopBytes = llvh::alignDown(regsToFill, 4) * sizeof(SHLegacyValue);
    // Initialize the loop limit in x1.
    if (a64::Utils::isAddSubImm(loopBytes)) {
      a.add(a64::x1, a64::x0, loopBytes);
    } else {
      a.mov(a64::x1, loopBytes);
      a.add(a64::x1, a64::x0, a64::x1);
    }
    asmjit::Label loop = a.newLabel();
    a.bind(loop);
    // Loop until we reach the limit.
    a.stp(a64::v0, a64::v0, a64::Mem(a64::x0).post(32));
    a.cmp(a64::x0, a64::x1);
    a.b_lo(loop);

    regsToFill %= 4;
  } else if (regsToFill >= 4) {
    a.movi(a64::v0.d2(), 0);
    // If the number of registers is small, just fill them directly.
    while (regsToFill >= 4) {
      a.stp(a64::v0, a64::v0, a64::Mem(a64::x0).post(32));
      regsToFill -= 4;
    }
  }
  // Fill any excess registers.
  if (regsToFill >= 2) {
    a.stp(a64::xzr, a64::xzr, a64::Mem(a64::x0).post(16));
    regsToFill -= 2;
  }
  if (regsToFill > 0) {
    assert(regsToFill == 1 && "All regs must be filled");
    a.str(a64::xzr, a64::Mem(a64::x0));
  }

  // Create the slow path for throwing a register stack overflow.
  slowPaths_.emplace_back(
      registerOverflowLab,
      /* emittingIP */ nullptr,
      [](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: _sh_throw_register_stack_overflow");
        em.a.bind(sp.slowPathLab);
        em.a.mov(a64::x0, xRuntime);
        // Do not save the IP because we have not yet set up the stack frame
        // for this function. The exception should appear in the caller.
        EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
            em, void (*)(SHRuntime *), _sh_throw_register_stack_overflow);
      });

  if (catchTableLabel_.isValid()) {
    comment("// _sh_try");
    uint32_t jmpBufOffset = getJmpBufOffset();
    // buf->prev = shr->shCurJmpBuf;
    a.ldr(a64::x0, a64::Mem(xRuntime, offsetof(SHRuntime, shCurJmpBuf)));
    a.str(a64::x0, a64::Mem(a64::sp, jmpBufOffset + offsetof(SHJmpBuf, prev)));

    // shr->shCurJmpBuf = buf;
    a.add(a64::x0, a64::sp, jmpBufOffset);
    a.str(a64::x0, a64::Mem(xRuntime, offsetof(SHRuntime, shCurJmpBuf)));

    // _setjmp(buf->buf);
    a.add(a64::x0, a64::sp, jmpBufOffset + offsetof(SHJmpBuf, buf));
    // setjmp can't throw, so the IP does not need to be saved.
    EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(*this, int (*)(jmp_buf), _sh_setjmp);
    // If this a catch, go to the catch table to jump to either a handler BB or
    // rethrow.
    a.cbnz(a64::x0, catchTableLabel_);
  }

  if (dumpJitCode_ & DumpJitCode::EntryExit) {
    comment("// print entry");
    a.mov(a64::w0, 1);
    a.adr(a64::x1, roDataLabel_);
    a.add(a64::x1, a64::x1, getDebugFunctionName());
    EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
        *this, void (*)(bool, const char *), _sh_print_function_entry_exit);
  }
}

void Emitter::leave(llvh::ArrayRef<const asmjit::Label *> exceptionHandlers) {
  comment("// leaveFrame");
  a.bind(returnLabel_);
  if (dumpJitCode_ & DumpJitCode::EntryExit) {
    comment("// print exit");
    a.mov(a64::w0, 0);
    a.adr(a64::x1, roDataLabel_);
    a.add(a64::x1, a64::x1, getDebugFunctionName());
    EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
        *this, void (*)(bool, const char *), _sh_print_function_entry_exit);
  }

  if (catchTableLabel_.isValid()) {
    comment("// _sh_end_try");
    // shr->shCurJmpBuf = buf->prev
    uint32_t jmpBufOffset = getJmpBufOffset();
    a.ldr(a64::x0, a64::Mem(a64::sp, jmpBufOffset + offsetof(SHJmpBuf, prev)));
    a.str(a64::x0, a64::Mem(xRuntime, offsetof(SHRuntime, shCurJmpBuf)));
  }

  // _sh_leave(shr, &locals.head, frame);
  // Restore the previous stack frame.
  a.str(xFrame, a64::Mem(xRuntime, RuntimeOffsets::stackPointer));
  a.ldr(
      a64::x0,
      a64::Mem(
          xFrame,
          StackFrameLayout::PreviousFrame * (int)sizeof(SHLegacyValue)));
  a.str(a64::x0, a64::Mem(xRuntime, RuntimeOffsets::currentFrame));

  // The return value has been stashed in x21 by ret(). Move it to the return
  // register.
  a.mov(a64::x0, a64::x21);

  unsigned stackOfs = getSavedRegsOffset();
  for (unsigned i = 0; i < gpSaveCount_; i += 2, stackOfs += 16) {
    if (i + 1 < gpSaveCount_)
      a.ldp(a64::GpX(19 + i), a64::GpX(20 + i), a64::Mem(a64::sp, stackOfs));
    else
      a.ldr(a64::GpX(19 + i), a64::Mem(a64::sp, stackOfs));
  }
  for (unsigned i = 0; i < vecSaveCount_; i += 2, stackOfs += 16) {
    if (i + 1 < vecSaveCount_)
      a.ldp(
          a64::VecD(kVecSaved.first + i),
          a64::VecD(kVecSaved.first + 1 + i),
          a64::Mem(a64::sp, stackOfs));
    else
      a.ldr(a64::VecD(kVecSaved.first + i), a64::Mem(a64::sp, stackOfs));
  }
  a.ldp(a64::x29, a64::x30, a64::Mem(a64::sp, stackOfs));

  a.add(a64::sp, a64::sp, getStackSize());

  a.ret(a64::x30);

  emitCatchTable(exceptionHandlers);
  emitSlowPaths();
  emitTypeAssertFailTail();
  emitROData();
}

void Emitter::callRuntimeWithSavedIP(void *fn, const char *name) {
  // Save the current IP in the runtime.
  getBytecodeIP(xScratch);
  a.str(xScratch, a64::Mem(xRuntime, RuntimeOffsets::currentIP));

  // Call the passed function.
  callRuntime(fn, name);

  if (emitAsserts_) {
    // Invalidate the current IP to make sure it is set before the next call.
    a.mov(xScratch, Runtime::kInvalidCurrentIP);
    a.str(xScratch, a64::Mem(xRuntime, RuntimeOffsets::currentIP));
  }
}

void Emitter::callRuntime(void *fn, const char *name) {
  comment("// call %s", name);
  loadBits64InGp(xScratch, (uint64_t)fn, name);
  a.blr(xScratch);
}

void Emitter::callRuntimeWithSavedIPIndirect(
    uint64_t slotAddr,
    const char *name) {
  // Save the current IP in the runtime.
  getBytecodeIP(xScratch);
  a.str(xScratch, a64::Mem(xRuntime, RuntimeOffsets::currentIP));

  // Call through the slot.
  callRuntimeIndirect(slotAddr, name);

  if (emitAsserts_) {
    // Invalidate the current IP to make sure it is set before the next call.
    a.mov(xScratch, Runtime::kInvalidCurrentIP);
    a.str(xScratch, a64::Mem(xRuntime, RuntimeOffsets::currentIP));
  }
}

void Emitter::callRuntimeIndirect(uint64_t slotAddr, const char *name) {
  comment("// call %s", name);
  // The slot's ADDRESS is the constant baked into the code; the callee is
  // whatever the slot holds when the call executes. xScratch carries first
  // the one and then the other, which is all the direct form clobbers too.
  loadBits64InGp(xScratch, slotAddr, "helper slot address");
  a.ldr(xScratch, a64::Mem(xScratch));
  a.blr(xScratch);
}

void Emitter::emitIncrementCounter(JitCounter counter) {
  if (!emitCounters_)
    return;
  // Push some registers onto the stack so we can use them.
  a.stp(a64::x0, a64::x1, a64::Mem(a64::sp).pre(-16));

  // Increment the counter.
  a.ldr(a64::x0, a64::Mem(xRuntime, RuntimeOffsets::runtimeJitCounters));
  a.ldr(a64::x1, a64::Mem(a64::x0, (unsigned)counter * sizeof(uint64_t)));
  a.add(a64::x1, a64::x1, 1);
  a.str(a64::x1, a64::Mem(a64::x0, (unsigned)counter * sizeof(uint64_t)));

  // Pop the saved values back off the stack.
  a.ldp(a64::x0, a64::x1, a64::Mem(a64::sp).post(16));
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

void Emitter::loadFrameAddr(a64::GpX dst, FR frameReg) {
  auto ofs =
      (frameReg.index() + StackFrameLayout::FirstLocal) * sizeof(SHLegacyValue);
  // If the offset fits as an immediate, just emit an add.
  if (a64::Utils::isAddSubImm(ofs)) {
    a.add(dst, xFrame, ofs);
    return;
  }
  // We cannot add the offset as an immediate, so move it in first.
  a.mov(dst, ofs);
  a.add(dst, dst, xFrame);
}

void Emitter::getBytecodeIP(const a64::GpX &xOut) {
  uint32_t ofs = codeBlock_->getOffsetOf(emittingIP);
  // ADD's immediate is 12 bits, optionally shifted left by 12, so an offset
  // of 16MB or above cannot be reached by adding to the base at all.
  // Materialize the whole address instead. That costs a constant pool entry
  // per call site rather than one shared for the function, which is why it is
  // not the general case.
  if (LLVM_UNLIKELY(ofs > 0xFFFFFF)) {
    loadBits64InGp(xOut, (uint64_t)codeBlock_->begin() + ofs, "Bytecode IP");
    return;
  }
  loadBits64InGp(xOut, (uint64_t)codeBlock_->begin(), "Bytecode start");
  // The first instruction of a function is at offset zero, which needs no
  // add at all.
  if (ofs)
    emit_add_imm_u24(a, xOut, ofs);
}

void Emitter::unreachable() {
  EMIT_RUNTIME_CALL(*this, void (*)(), _sh_unreachable);
}

void Emitter::profilePoint(uint16_t pointIndex) {
  comment("// ProfilePoint %u", pointIndex);
#ifdef HERMESVM_PROFILER_BB
  syncAllFRTempExcept({});
  freeAllFRTempExcept({});
  a.mov(a64::x0, xRuntime);
  a.mov(a64::w1, pointIndex);
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

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frText);
  a.mov(a64::w2, strictCaller);
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

  asmjit::Error err;
  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  HWReg hwTmp = allocAndLogTempGpX();
  a64::GpW wTmp(hwTmp.indexInClass());

  a.ldur(
      wTmp,
      a64::Mem(
          xFrame,
          (int)StackFrameLayout::ArgCount * (int)sizeof(SHLegacyValue)));

  // Does paramIndex fit in the 12-bit unsigned immediate?
  if (a64::Utils::isAddSubImm(paramIndex)) {
    a.cmp(wTmp, paramIndex);
  } else {
    HWReg hwTmp2 = allocAndLogTempGpX();
    a64::GpW wTmp2(hwTmp2.indexInClass());
    loadBits64InGp(wTmp2, paramIndex, "paramIndex");
    a.cmp(wTmp, wTmp2);
    freeReg(hwTmp2);
  }
  a.b_lo(slowPathLab);

  freeReg(hwTmp);

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
    a.b(slowPathLab);
  } else {
    int32_t ofs = (int32_t)ofs64;
    EXPECT_ERROR(
        asmjit::kErrorInvalidDisplacement,
        err = a.ldur(hwRes.a64GpX(), a64::Mem(xFrame, ofs)));
    // Does the offset fit in the 9-bit signed offset?
    if (err) {
      ofs = -ofs;
      a64::GpX xRes = hwRes.a64GpX();
      if (ofs <= 4095) {
        a.sub(xRes, xFrame, ofs);
      } else {
        loadBits64InGp(xRes, ofs, nullptr);
        a.sub(xRes, xFrame, xRes);
      }
      a.ldr(xRes, a64::Mem(xRes));
    }
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
        em.loadBits64InGp(hwRes.a64GpX(), _sh_ljs_undefined().raw, "undefined");
        em.a.b(sp.contLab);
      });
}

void Emitter::getGlobalObject(FR frRes) {
  comment("// GetGlobalObject r%u", frRes.index());
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWFromMem(hwRes, a64::Mem(xRuntime, RuntimeOffsets::globalObject));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::declareGlobalVar(SHSymbolID symID) {
  comment("// DeclareGlobalVar %u", symID);

  syncAllFRTempExcept({});
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  a.mov(a64::w1, symID);
  EMIT_RUNTIME_CALL(
      *this, void (*)(SHRuntime *, SHSymbolID), _sh_ljs_declare_global_var);
}

void Emitter::debugger() {
  comment("// Debugger");
  if (dumpJitCode_ & DumpJitCode::BRK)
    a.brk(0);
}

void Emitter::createRegExp(
    FR frRes,
    SHSymbolID patternID,
    SHSymbolID flagsID,
    uint32_t regexpID) {
  comment("// CreateRegExp r%u, %u, %u", frRes.index(), patternID, flagsID);

  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadBits64InGp(a64::x1, (uint64_t)codeBlock_, "CodeBlock");
  a.mov(a64::w2, patternID);
  a.mov(a64::w3, flagsID);
  a.mov(a64::w4, regexpID);
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

  // Find the catch target for the exception.
  a.mov(a64::x0, xRuntime);
  loadBits64InGp(a64::x1, (uint64_t)codeBlock_, "CodeBlock");
  a.mov(a64::x2, xFrame);
  a.add(a64::x3, a64::sp, getJmpBufOffset());
  a.ldr(a64::x4, a64::Mem(a64::sp, getSavedSHLocalsOffset()));
  a.adr(a64::x5, addressTableLab);
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
  a.br(a64::x0);

  // Table of offsets from addressTableLab to jump to.
  a.bind(addressTableLab);
  for (const asmjit::Label *handler : exceptionHandlers) {
    a.embedLabelDelta(*handler, addressTableLab, /* size */ 4);
  }
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
    a.fmov(xScratch, hwVal.a64VecD());
    emitTypeAssertGpX(fr, xScratch, pred);
  } else {
    emitTypeAssertGpX(fr, hwVal.a64GpX(), pred);
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

void Emitter::emitTypeAssertGpX(FR fr, const a64::GpX &xVal, TypePred pred) {
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

  // The helpers below tolerate xTemp == xVal; every such use is the last
  // read of xVal in the sequence.
  switch (pred) {
    case TypePred::IsNumber:
      emit_sh_ljs_is_double(a, xVal, xScratch2);
      a.b_hs(failLab);
      break;
    case TypePred::IsBool:
      emit_sh_ljs_is_bool(a, xScratch, xVal);
      a.b_ne(failLab);
      break;
    case TypePred::NotPointer:
      emit_sh_ljs_get_tag(a, xScratch, xVal);
      emit_sh_ljs_tag_is_pointer(a, xScratch);
      a.b_hs(failLab);
      break;
    case TypePred::BitComparable:
      emit_sh_ljs_is_double(a, xVal, xScratch2);
      a.b_lo(failLab);
      emit_sh_ljs_get_tag(a, xScratch, xVal);
      emit_sh_ljs_tag_is_pointer(a, xScratch);
      a.b_hs(failLab);
      break;
    case TypePred::IsObject:
      emit_sh_ljs_is_object(a, xScratch, xVal);
      a.b_ne(failLab);
      break;
  }

  slowPaths_.emplace_back(
      failLab, emittingIP, [idx](Emitter &em, SlowPath &sp) {
        em.comment("// Type assert failure %u", idx);
        em.a.bind(sp.slowPathLab);
        em.a.mov(a64::w0, idx);
        em.a.b(em.typeAssertFailLab_);
      });
}

void Emitter::readFRForAssert(FR fr) {
  assert(emitTypeAsserts_ && "caller must check emitTypeAsserts_");
  FRState &frState = frameRegs_[fr.index()];
  assert(!frState.regIsDirty && "reading an FR that is about to be written");

  // Locals always hold the latest value; a global reg holds it only if
  // globalRegUpToDate; otherwise the frame must be up to date.
  if (frState.localGpX) {
    a.mov(xScratch, frState.localGpX.a64GpX());
  } else if (frState.localVecD) {
    a.fmov(xScratch, frState.localVecD.a64VecD());
  } else if (frState.globalReg && frState.globalRegUpToDate) {
    if (frState.globalReg.isGpX())
      a.mov(xScratch, frState.globalReg.a64GpX());
    else
      a.fmov(xScratch, frState.globalReg.a64VecD());
  } else {
    assert(frState.frameUpToDate && "FR has no up-to-date location");
    // _loadFrame's large-offset encoding fallback also uses xScratch, but
    // only as the address index in its mov/ldr pair, which is read before
    // the ldr writes the loaded value into it, so passing xScratch as the
    // destination here is safe.
    _loadFrame(HWReg(xScratch), fr);
  }
}

void Emitter::emitTypeAssertFailTail() {
  if (!typeAssertSites_)
    return;
  comment("// Type assert failure tail");
  a.bind(typeAssertFailLab_);
  // w0 already holds the site index, set by the per-site stub.
  a.ldr(
      a64::x1,
      a64::Mem(
          roDataLabel_,
          uint64Const((uint64_t)typeAssertSites_, "type assert site table")));
  // Not EMIT_RUNTIME_CALL: that saves the current IP, and emitSlowPaths()
  // has already cleared emittingIP by the time this runs. The handler does
  // not need the IP anyway - the site record carries the bytecode offset.
  EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
      *this,
      void (*)(uint32_t, const std::vector<TypeAssertSite> *),
      _jit_type_assert_failed);
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

} // namespace hermes::vm::arm64
#endif // HERMESVM_JIT_ARM64
