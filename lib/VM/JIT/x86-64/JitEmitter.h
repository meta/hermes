/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "asmjit/x86.h"

#include "../JitHandlers.h"

#include "hermes/ADT/DenseUInt64.h"
#include "hermes/ADT/SimpleLRU.h"
#include "hermes/Support/OptValue.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/JIT/JIT.h"
#include "hermes/VM/JIT/PerfJitDump.h"
#include "hermes/VM/JIT/x86-64/JIT.h"
#include "hermes/VM/RuntimeModule.h"
#include "hermes/VM/static_h.h"
#include "hermes/VMLayouts/StackFrameLayout.h"

#include <cstdarg>
#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/SmallVector.h"

#include <deque>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace hermes::vm::x86_64 {

namespace x86 = asmjit::x86;

/// A HermesVM frame register
class FR {
  uint32_t index_;

 public:
  static constexpr uint32_t kInvalid = UINT32_MAX;

  FR() : index_(kInvalid) {}
  constexpr explicit FR(uint32_t index) : index_(index) {}

  constexpr bool isValid() const {
    return index_ != kInvalid;
  }

  constexpr uint32_t index() const {
    return index_;
  }
  bool operator==(const FR &fr) const {
    return fr.index_ == index_;
  }
  bool operator!=(const FR &fr) const {
    return fr.index_ != index_;
  }
};

enum class FRType : uint8_t {
  Number = 1,
  Bool = 2,
  /// Any other non-pointer type.
  OtherNonPtr = 4,
  Pointer = 8,
  UnknownNonPtr = Number | Bool | OtherNonPtr,
  UnknownPtr = UnknownNonPtr | Pointer,
};

class HWReg {
  // 0..31: Gp. 32..63: Vec. 128: invalid.
  uint8_t index_;

  explicit constexpr HWReg(uint8_t index) : index_(index) {}

 public:
  struct GpX {};
  struct VecD {};

  constexpr HWReg() : index_(0xFF) {}
  explicit constexpr HWReg(uint8_t index, GpX) : index_(index) {}
  explicit constexpr HWReg(uint8_t index, VecD) : index_(index + 32) {}
  explicit constexpr HWReg(const x86::Gp &gp) : HWReg(gp.id(), GpX{}) {}
  explicit constexpr HWReg(const x86::Xmm &xmm) : HWReg(xmm.id(), VecD{}) {}

  static constexpr HWReg gpX(uint8_t index) {
    assert(index < 16 && "invalid Gp");
    return HWReg(index, GpX{});
  }
  static constexpr HWReg vecD(uint8_t index) {
    assert(index < 16 && "invalid Xmm");
    return HWReg(index, VecD{});
  }

  operator bool() const {
    return isValid();
  }
  bool isValid() const {
    return index_ != 0xFF;
  }
  bool isValidGpX() const {
    return index_ < 32;
  }
  bool isValidVecD() const {
    return index_ >= 32 && index_ < 64;
  }
  bool isGpX() const {
    assert(isValid());
    return index_ < 32;
  }
  bool isVecD() const {
    assert(isValid());
    return index_ >= 32 && index_ < 64;
  }

  x86::Gp gpq() const {
    assert(isGpX());
    return x86::gpq(indexInClass());
  }
  x86::Xmm xmm() const {
    assert(isVecD());
    return x86::xmm(indexInClass());
  }

  uint8_t combinedIndex() const {
    assert(isValid());
    return index_ & 63;
  }
  uint8_t indexInClass() const {
    assert(isValid());
    return index_ & 31;
  }

  bool operator==(const HWReg &other) const {
    return index_ == other.index_;
  }
  bool operator!=(const HWReg &other) const {
    return index_ != other.index_;
  }
};

llvh::raw_ostream &operator<<(
    llvh::raw_ostream &os,
    const hermes::vm::x86_64::HWReg &hwReg);

/// A frame register can reside simultaneously in one or more of the following
/// locations:
/// - The stack frame
/// - A global callee-save register (which can be either Gp or Vec)
/// - A local Gp register
/// - A local Vec register.
/// A frame register always has an allocated slot in the frame, even if it never
/// uses it.
/// Additionally, it may have an associated global reg, and two local regs.
/// Having them associated with the frame reg does not necessarily mean that the
/// hardware registers contain the most up-to-date value. The following
/// invariants apply:
/// - If there are local registers, they always contain the latest value.
/// - If there is more than one local register, they all contain the same bit
/// pattern.
/// - if there is a global register, it contains the latest value, unless
/// globalRegUpToDate is not set, in which case the latest value *must* be in
/// local registers. The state where there is a global reg, but the latest value
/// is only in the frame is not valid, as it is not useful.
/// - if frameUpToDate is set, then the frame contains the latest value.
struct FRState {
  /// Type that applies for the entire function.
  FRType globalType = FRType::UnknownPtr;
  /// Type in the current basic block, could be narrower. This applies, until
  /// it is reset, to the up-to-date value, local or not.
  FRType localType = FRType::UnknownPtr;

  /// Pre-allocated global register.
  HWReg globalReg{};
  /// Register in the current basic block.
  HWReg localGpX{};
  HWReg localVecD{};

  /// Whether the latest value has been written to the frame.
  bool frameUpToDate = false;
  /// Whether the global register exists and contains an up-to-date value. If
  /// false, either there is no globalReg, or there must be a local register
  /// allocated.
  bool globalRegUpToDate = false;

#ifndef NDEBUG
  /// Whether the currently associated register is dirty and about to be
  /// overwritten. This FR should not be read when in this state.
  bool regIsDirty = false;
#endif
};

struct HWRegState {
  FR contains{};
};

// x86-64: SysV register convention (see the design spec).
static constexpr auto xRuntime = x86::r15;
static constexpr auto xFrame = x86::r14;
// Reserved scratch, never allocated by TempRegAlloc.
static constexpr auto xScratch = x86::r11;
// GPR temps: {rax,rcx,rdx} and {rsi,rdi,r8,r9,r10} (asmjit ids 0-2,6-10);
// rbx/rsp/rbp (3-5) sit between the ranges and are never temps.
static constexpr std::pair<uint8_t, uint8_t> kGPTemp1(0, 2);
static constexpr std::pair<uint8_t, uint8_t> kGPTemp2(6, 10);
// Callee-saved global pool: rbx (also the return-value stash, the x21
// analogue), r12, r13. Not contiguous, so a list, not a range.
static constexpr uint8_t kGPSavedList[] = {3, 12, 13};
// rbx: return-value stash (arm64 x21 analogue); must equal kGPSavedList[0].
static constexpr uint8_t kGPReturnStash = 3;
// Vector temps: all of xmm0-xmm15; no callee-saved vector registers
// exist in SysV, so there are no vector globals.
static constexpr std::pair<uint8_t, uint8_t> kVecTemp(0, 15);

/// \return true if \p id falls within [range.first, range.second].
static constexpr bool idInRange(
    uint32_t id,
    std::pair<uint8_t, uint8_t> range) {
  return id >= range.first && id <= range.second;
}
/// \return true if \p id appears in kGPSavedList.
static constexpr bool idInGPSavedList(uint32_t id) {
  for (uint8_t saved : kGPSavedList)
    if (saved == id)
      return true;
  return false;
}

// xScratch/xFrame/xRuntime are dedicated registers, never handed out by
// TempRegAlloc. Slow paths rely on this: they pass xScratch as a tag temp
// alongside live allocatable registers, so if xScratch (or xFrame/xRuntime)
// aliased a temp or saved-global id, that code would silently clobber a
// live value.
static_assert(
    !idInRange(xScratch.id(), kGPTemp1) &&
        !idInRange(xScratch.id(), kGPTemp2) && !idInGPSavedList(xScratch.id()),
    "xScratch must not overlap the GP temp ranges or saved-global pool");
static_assert(
    !idInRange(xFrame.id(), kGPTemp1) && !idInRange(xFrame.id(), kGPTemp2) &&
        !idInGPSavedList(xFrame.id()),
    "xFrame must not overlap the GP temp ranges or saved-global pool");
static_assert(
    !idInRange(xRuntime.id(), kGPTemp1) &&
        !idInRange(xRuntime.id(), kGPTemp2) && !idInGPSavedList(xRuntime.id()),
    "xRuntime must not overlap the GP temp ranges or saved-global pool");

static constexpr uint32_t bitMask32(unsigned first, unsigned last) {
  return ((1u << (last - first + 1)) - 1u) << first;
}
template <typename T>
static constexpr uint32_t bitMask32(std::pair<T, T> range) {
  return bitMask32(range.first, range.second);
}

class TempRegAlloc {
  unsigned first_;
  SimpleLRU<unsigned> lru_{};
  std::vector<unsigned *> map_{};
  uint32_t availBits_;

 public:
  explicit TempRegAlloc(std::pair<uint8_t, uint8_t> range)
      : first_(range.first), lru_(range.second - range.first + 1) {
    map_.resize(range.second - range.first + 1);
    availBits_ = bitMask32(range);
  }
  explicit TempRegAlloc(
      std::pair<uint8_t, uint8_t> range1,
      std::pair<uint8_t, uint8_t> range2)
      : first_(range1.first),
        lru_(
            range1.second - range1.first + 1 + range2.second - range2.first +
            1) {
    map_.resize(range2.second - range1.first + 1);
    availBits_ = bitMask32(range1) | bitMask32(range2);
  }

  llvh::Optional<unsigned> alloc(
      llvh::Optional<unsigned> preferred = llvh::None) {
    if (availBits_ == 0)
      return llvh::None;

    unsigned index;
    if (preferred && (availBits_ & (1u << *preferred)))
      index = *preferred;
    else
      index = llvh::findFirstSet(availBits_);
    availBits_ &= ~(1u << index);
    assert(index >= first_ && "Invalid tmpreg index");
    assert(!map_[index - first_] && "map shows the index as occupied");
    map_[index - first_] = lru_.add(index);

    return index;
  }

  void use(unsigned index) {
    assert(index >= first_ && "Invalid tmpreg index");
    if (!(availBits_ & (1u << index)))
      lru_.use(map_[index - first_]);
  }

  void free(unsigned index) {
    assert(index >= first_ && "Invalid tmpreg index");
    assert(map_[index - first_] && "map shows the tmpreg is freed");
    assert(!(availBits_ & (1u << index)) && "bitmask shows tmpreg is freed");

    availBits_ |= (1u << index);
    lru_.remove(map_[index - first_]);
    map_[index - first_] = nullptr;
  }

  bool isAllocated(unsigned index) {
    assert(index >= first_ && "Invalid tmpreg index");
    return availBits_ & (1u << index);
  }

  unsigned leastRecentlyUsed() {
    return *lru_.leastRecent();
  }

 private:
};

class Emitter {
  Runtime &runtime_;
  // Used by emitTypeAssertGpX() to claim this function's entry in
  // jitImpl_.typeAssertSites (JitEmitter.cpp).
  JITContext::Impl &jitImpl_;

  /// Level of dumping JIT code. Bit 0 indicates code printing on or off.
  unsigned const dumpJitCode_;
  /// Whether to emit asserts in the JIT'ed code.
  bool const emitAsserts_;
  /// Whether to verify FR type assumptions in the JIT'ed code.
  bool const emitTypeAsserts_;
  /// Whether to emit counters in the JIT'ed code.
  bool const emitCounters_;

#ifndef ASMJIT_NO_LOGGING
  std::unique_ptr<asmjit::Logger> logger_{};
#endif

  std::unique_ptr<asmjit::ErrorHandler> errorHandler_;
  asmjit::Error expectedError_ = asmjit::kErrorOk;

  std::vector<FRState> frameRegs_;
  std::array<HWRegState, 64> hwRegs_;

  /// GP temp registers.
  TempRegAlloc gpTemp_{kGPTemp1, kGPTemp2};
  /// Vec temp registers.
  TempRegAlloc vecTemp_{kVecTemp};

  /// A deferred slow path, emitted at the end of the function.
  ///
  /// Everything the slow path needs beyond the common fields below is held in
  /// the lambda passed to the constructor, stored inline in \c storage_. This
  /// keeps each slow path's state private to the one place that produces and
  /// consumes it, instead of a shared set of fields that any slow path could
  /// read whether or not its producer set them.
  class SlowPath {
   public:
    /// Label of the slow path.
    asmjit::Label slowPathLab;
    /// Label to jump to after the slow path.
    asmjit::Label contLab;
    /// Bytecode IP of the instruction that this is a slow path for.
    const inst::Inst *emittingIP;

    /// \param l is invoked as l(Emitter &, SlowPath &) to emit the slow path.
    /// Its captures are copied into \c storage_ and never destroyed, so they
    /// must be trivially destructible and must fit.
    template <typename L>
    SlowPath(
        asmjit::Label slowPathLab,
        asmjit::Label contLab,
        const inst::Inst *emittingIP,
        L &&l)
        : slowPathLab(slowPathLab),
          contLab(contLab),
          emittingIP(emittingIP),
          emit_([](Emitter &em, SlowPath &sp) {
            (*reinterpret_cast<std::decay_t<L> *>(sp.storage_))(em, sp);
          }) {
      using Lambda = std::decay_t<L>;
      static_assert(
          sizeof(Lambda) <= sizeof(storage_),
          "slow path captures too much; enlarge storage_ or capture less");
      static_assert(
          alignof(Lambda) <= alignof(void *),
          "slow path captures are over-aligned");
      static_assert(
          std::is_trivially_destructible_v<Lambda>,
          "slow path captures must be trivially destructible");
      ::new (storage_) Lambda(std::forward<L>(l));
    }

    /// Overload for slow paths that do not branch back to a continuation.
    template <typename L>
    SlowPath(asmjit::Label slowPathLab, const inst::Inst *emittingIP, L &&l)
        : SlowPath(
              slowPathLab,
              asmjit::Label(),
              emittingIP,
              std::forward<L>(l)) {}

    /// Non-copyable and non-movable: \c storage_ holds a type-erased lambda
    /// that cannot be relocated by the implicit memberwise copy. std::deque
    /// never relocates existing elements, so emplace_back and pop_front are
    /// all that is needed.
    SlowPath(const SlowPath &) = delete;
    SlowPath &operator=(const SlowPath &) = delete;

    /// Emit this slow path.
    void emit(Emitter &em) {
      emit_(em, *this);
    }

   private:
    void (*emit_)(Emitter &em, SlowPath &sp);
    /// Inline storage for the lambda's captures. Sized to match arm64, whose
    /// largest slow path captures an asmjit::Label (16 bytes, an Operand)
    /// plus three pointers, two FRs and two bools. Raising this is fine; the
    /// static_assert above is what keeps a too-large capture from becoming a
    /// silent heap allocation.
    alignas(void *) char storage_[56];
  };
  /// Queue of slow paths.
  std::deque<SlowPath> slowPaths_{};
  /// x86-64: monotonically increasing counter used to name SLOW_/CONT_
  /// labels. Unlike arm64 -- which still names labels from
  /// slowPaths_.size() -- this backend does not reuse an index once it is
  /// handed out. See newSlowPathLabel()/newContLabel().
  unsigned slowPathLabelCounter_{0};

  /// Records for every emitted type check, in site-index order. Owned by
  /// JITContext::Impl, which outlives the emitted code that refers to it;
  /// this is only a pointer to that entry, claimed on first use.
  std::vector<TypeAssertSite> *typeAssertSites_ = nullptr;
  /// The shared failure tail, bound only if there is at least one site.
  asmjit::Label typeAssertFailLab_{};

  /// FRs written by the bytecode instruction currently being emitted whose
  /// global register class requires a check. Drained at each instruction
  /// boundary by emitPendingTypeAsserts().
  llvh::SmallVector<FR, 4> typeAssertPendingWrites_{};

  /// Descriptor for a single RO data entry.
  struct DataDesc {
    /// Size in bytes.
    int32_t size;
    asmjit::TypeId typeId;
    int32_t itemCount;
    /// Optional comment.
    const char *comment;
  };
  /// Used for pretty printing when logging data.
  std::vector<DataDesc> roDataDesc_{};
  std::vector<uint8_t> roData_{};
  asmjit::Label roDataLabel_{};

  /// Map from the bit pattern of a double value to offset in constant pool.
  llvh::DenseMap<hermes::DenseUInt64, int32_t> fp64ConstMap_{};

  /// Label to branch to when returning from a function. The return value
  /// will be in rbx.
  asmjit::Label returnLabel_{};

  /// Label to branch to when catching an exception with setjmp.
  /// Invalid if there's no try/catch in the function.
  asmjit::Label catchTableLabel_{};

  /// Label to branch to when attempting to call a non-object. The callee and
  /// saved IP must already be in the right position on the stack. This is
  /// initialized lazily by the first call, and shared across all calls.
  asmjit::Label nonObjCallLabel_{};

  /// The bytecode codeblock.
  CodeBlock *const codeBlock_;

  /// Optionally, the offset of the string name, used for debug printing.
  int32_t roOfsDebugFunctionName_ = -1;

  /// Offset in RODATA of the pointer to the start of the read property
  /// cache.
  int32_t roOfsReadPropertyCachePtr_;
  /// Offset in RODATA of the pointer to the start of the write property
  /// cache.
  int32_t roOfsWritePropertyCachePtr_;
  /// Offset in RODATA of the pointer to the start of the private name
  /// cache.
  int32_t roOfsPrivateNameCachePtr_;

  /// Number of entries of kGPSavedList pushed by the prologue. Always at
  /// least one, since rbx is saved even when no FR uses it.
  unsigned gpSaveCount_ = 0;

#ifndef NDEBUG
  /// x86-64: running total of EVERY transient rsp adjustment the emitter
  /// makes below the post-prologue rsp (see the callImpl() contract
  /// comment). Two sites move rsp: putByIdImpl, whose two stack-argument
  /// pushes each add 8 and whose matching `add rsp, 16` subtracts 16, and
  /// bumpAllocAndUnpoison, whose ASan-build register save/restore adds and
  /// then removes 8 per GP temp. callRuntime()/callRuntimeWithSavedIP()
  /// assert it is a multiple of 16 at every call, and it must be exactly 0
  /// at every instruction boundary and at leave(). Pure bookkeeping: no
  /// code is emitted for it.
  ///
  /// Nothing tracked here survives an instruction, so it says nothing about
  /// where rsp is when an exception is thrown -- and it does not have to:
  /// longjmp restores rsp to its value at the setjmp, which is where the
  /// catch table then reads the SHJmpBuf and the saved SHLocals from. See
  /// emitCatchTable().
  int32_t rspDelta_{0};
#endif

 public:
  asmjit::CodeHolder code{};
  x86::Assembler a{};
  /// The IP of the instruction being emitted.
  const inst::Inst *emittingIP{nullptr};

  /// Create an Emitter, but do not emit any actual code.
  /// Use \c enter to set up the stack frame before emitting the actual code.
  explicit Emitter(
      Runtime &runtime,
      JITContext::Impl &jitImpl,
      unsigned dumpJitCode,
      bool emitAsserts,
      bool emitTypeAsserts,
      bool emitCounters,
      PerfJitDump *perfJitDump,
      CodeBlock *codeBlock,
      const std::function<void(std::string &&message)> &longjmpError);

  /// Add the jitted function to the JIT runtime and return a pointer to it.
  JITCompiledFunctionPtr addToRuntime(asmjit::JitRuntime &jr);

#ifdef NDEBUG
  void assertPostInstructionInvariants() {}
#else
  void assertPostInstructionInvariants();
#endif

  /// Allocate global registers and set up the stack frame.
  /// Must be called before emitting any real code.
  /// \param numCount the first numCount registers are "number" registers.
  /// \param npCount the first npCount registers after the number registers are
  ///   non-pointer registers.
  void enter(uint32_t numCount, uint32_t npCount);

  /// Log a comment.
  /// Annotated with printf-style format.
  /// Defined inline below the class so the logger check is visible in every
  /// translation unit; the formatting itself is out of line in commentV().
  void comment(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

  /// Format \p fmt with \p args and pass the result to the assembler. Out of
  /// line so that vsnprintf is not duplicated into every caller.
  void commentV(const char *fmt, va_list args);

  /// Emit the catch table, slow paths and RO data,
  /// then reset the stack, end any try, and return.
  /// \param exceptionHandlers the labels for the exception handler table.
  void leave(llvh::ArrayRef<const asmjit::Label *> exceptionHandlers);
  void newBasicBlock(const asmjit::Label &label);

  /// Abort execution.
  void unreachable();

  /// Emit profiling information if profiling is enabled.
  void profilePoint(uint16_t point);

  void directEval(FR frRes, FR frText, bool strictCaller);

  /// Call a JS function.
  void call(FR frRes, FR frCallee, uint32_t argc);
  void callN(FR frRes, FR frCallee, llvh::ArrayRef<FR> args);
  void callBuiltin(FR frRes, uint32_t builtinIndex, uint32_t argc);
  void callWithNewTarget(FR frRes, FR frCallee, FR frNewTarget, uint32_t argc);
  /// Note that this technically allows different arguments at runtime because
  /// argc is a register.
  void callWithNewTargetLong(FR frRes, FR frCallee, FR frNewTarget, FR frArgc);

  /// Special bytecode for calling Metro require.
  void callRequire(FR frRes, FR frRequireFunc, uint32_t modIndex);

  /// Get a builtin closure.
  void getBuiltinClosure(FR frRes, uint32_t builtinIndex);

  void catchInst(FR frRes);

  /// Save the return value.
  void ret(FR frValue);
  void mov(FR frRes, FR frInput, bool logComment = true);
  void loadParam(FR frRes, uint32_t paramIndex);
  void loadConstDouble(FR frRes, double val, const char *name);
  void loadConstBits64(FR frRes, uint64_t val, FRType type, const char *name);
  void
  loadConstString(FR frRes, RuntimeModule *runtimeModule, uint32_t stringID);
  void
  loadConstBigInt(FR frRes, RuntimeModule *runtimeModule, uint32_t bigIntID);
  void toNumber(FR frRes, FR frInput);
  void toNumeric(FR frRes, FR frInput);
  void toInt32(FR frRes, FR frInput, bool isSigned);
  void addEmptyString(FR frRes, FR frInput);

  void addS(FR frRes, FR frLeft, FR frRight);
  void mod(bool forceNumber, FR frRes, FR frLeft, FR frRight);

  // x86-64: the arm64 body is a three-operand `fadd res, dl, dr`; the VEX
  // form is likewise three-operand, so res may alias either source without
  // an extra move.
#define DECL_BINOP(methodName, forceNum, commentStr, slowCall, x86body) \
  void methodName(FR rRes, FR rLeft, FR rRight) {                       \
    arithBinOp(                                                         \
        forceNum,                                                       \
        rRes,                                                           \
        rLeft,                                                          \
        rRight,                                                         \
        commentStr,                                                     \
        [](x86::Assembler & as,                                         \
           const x86::Xmm &res,                                         \
           const x86::Xmm &dl,                                          \
           const x86::Xmm &dr) x86body,                                 \
        (void *)slowCall,                                               \
        #slowCall);                                                     \
  }

  DECL_BINOP(mul, false, "mul", _sh_ljs_mul_rjs, { as.vmulsd(res, dl, dr); })
  DECL_BINOP(add, false, "add", _sh_ljs_add_rjs, { as.vaddsd(res, dl, dr); })
  DECL_BINOP(sub, false, "sub", _sh_ljs_sub_rjs, { as.vsubsd(res, dl, dr); })
  DECL_BINOP(div, false, "div", _sh_ljs_div_rjs, { as.vdivsd(res, dl, dr); })
  DECL_BINOP(mulN, true, "mulN", _sh_ljs_mul_rjs, { as.vmulsd(res, dl, dr); })
  DECL_BINOP(addN, true, "addN", _sh_ljs_add_rjs, { as.vaddsd(res, dl, dr); })
  DECL_BINOP(subN, true, "subN", _sh_ljs_sub_rjs, { as.vsubsd(res, dl, dr); })
  DECL_BINOP(divN, true, "divN", _sh_ljs_div_rjs, { as.vdivsd(res, dl, dr); })
#undef DECL_BINOP

  // x86-64: two differences from the arm64 table. The fast body takes one
  // source register instead of two, because x86's integer ALU is
  // two-operand and arm64 already passes the same register as destination
  // and first source. And there is an extra rightInCl column: a variable
  // shift on x86 reads its count from cl and nowhere else, so those three
  // rows ask bitBinOp to allocate the right-hand temp into rcx (see there).
  // The shifts operate on the 32-bit halves, as arm64's do on W registers,
  // which also makes the CPU's count masking (count & 31) the same as the
  // masking JS specifies.
  //
  // bitBinOp verifies the rcx placement at emission time and declines the
  // function if it ever fails, so the asserts in the shift bodies below are
  // redundant for that. They are kept because they catch the one mistake
  // that check cannot see -- a body that uses cl while its rightInCl column
  // says false, which asks bitBinOp for no placement at all -- and the
  // (void)right casts go with them, since a release build then has no other
  // use for the parameter.
#define DECL_BIT_BINOP(                                                  \
    methodName, unsignedRes, rightInCl, commentStr, slowCall, x86body)   \
  void methodName(FR rRes, FR rLeft, FR rRight) {                        \
    bitBinOp(                                                            \
        rRes,                                                            \
        rLeft,                                                           \
        rRight,                                                          \
        unsignedRes,                                                     \
        rightInCl,                                                       \
        commentStr,                                                      \
        slowCall,                                                        \
        #slowCall,                                                       \
        [](x86::Assembler & a, const x86::Gp &res, const x86::Gp &right) \
            x86body);                                                    \
  }

  DECL_BIT_BINOP(bitAnd, false, false, "bit_and", _sh_ljs_bit_and_rjs, {
    a.and_(res, right);
  })
  DECL_BIT_BINOP(bitOr, false, false, "bit_or", _sh_ljs_bit_or_rjs, {
    a.or_(res, right);
  })
  DECL_BIT_BINOP(bitXor, false, false, "bit_xor", _sh_ljs_bit_xor_rjs, {
    a.xor_(res, right);
  })
  DECL_BIT_BINOP(lShift, false, true, "lshift", _sh_ljs_left_shift_rjs, {
    assert(right.id() == x86::rcx.id() && "shift count must be in cl");
    (void)right;
    a.shl(res.r32(), x86::cl);
  })
  DECL_BIT_BINOP(rShift, false, true, "rshift", _sh_ljs_right_shift_rjs, {
    assert(right.id() == x86::rcx.id() && "shift count must be in cl");
    (void)right;
    a.sar(res.r32(), x86::cl);
  })
  DECL_BIT_BINOP(
      urShift,
      true,
      true,
      "rshiftu",
      _sh_ljs_unsigned_right_shift_rjs,
      {
        assert(right.id() == x86::rcx.id() && "shift count must be in cl");
        (void)right;
        a.shr(res.r32(), x86::cl);
      })
#undef DECL_BIT_BINOP

  // x86-64: the fast body receives the Emitter rather than the assembler,
  // because x86 has no floating-point immediate: arm64's `fmov tmp, 1.0`
  // becomes a load of an interned RO-data constant, which only the Emitter
  // can hand out (roConst64()).
#define DECL_UNOP(methodName, forceNum, commentStr, slowCall, x86body) \
  void methodName(FR rRes, FR rInput) {                                \
    arithUnop(                                                         \
        forceNum,                                                      \
        rRes,                                                          \
        rInput,                                                        \
        commentStr,                                                    \
        [](Emitter & em,                                               \
           const x86::Xmm &d,                                          \
           const x86::Xmm &s,                                          \
           const x86::Xmm &tmp) x86body,                               \
        (void *)slowCall,                                              \
        #slowCall);                                                    \
  }

  // The addend is taken straight from memory: `vaddsd xmm, xmm, m64` reads
  // 8 bytes with no alignment requirement, so no temp is needed and the
  // `tmp` operand goes unused (unlike arm64, which needs it for `fmov`).
  DECL_UNOP(dec, false, "dec", _sh_ljs_dec_rjs, {
    (void)tmp;
    em.a.vaddsd(d, s, em.roConst64(llvh::DoubleToBits(-1.0), "-1.0"));
  })
  DECL_UNOP(inc, false, "inc", _sh_ljs_inc_rjs, {
    (void)tmp;
    em.a.vaddsd(d, s, em.roConst64(llvh::DoubleToBits(1.0), "1.0"));
  })
  // x86-64: arm64's `fneg` becomes a sign-bit flip. The mask is loaded into
  // the vector temp first rather than used as a `vxorpd` memory operand,
  // because that operand is 128 bits wide and would read past an 8-byte
  // RO-data entry. `tmp` is `d` whenever `d != s`, so the load never
  // clobbers the source.
  DECL_UNOP(negate, false, "neg", _sh_ljs_minus_rjs, {
    em.a.vmovsd(tmp, em.roConst64(UINT64_C(1) << 63, "-0.0"));
    em.a.vxorpd(d, s, tmp);
  });
#undef DECL_UNOP

  void jmpTrueFalse(bool onTrue, const asmjit::Label &target, FR frInput);
  void jmpUndefined(const asmjit::Label &target, FR frInput);
  void jmp(const asmjit::Label &target);
  void jmpBuiltinIs(
      bool invert,
      const asmjit::Label &target,
      uint8_t builtinIndex,
      FR frInput);

  void booleanNot(FR frRes, FR frInput);
  void bitNot(FR frRes, FR frInput);
  void typeOf(FR frRes, FR frInput);

  void getPNameList(FR frRes, FR frObj, FR frIdx, FR frSize);
  void getNextPName(FR frRes, FR frProps, FR frObj, FR frIdx, FR frSize);

  void toPropertyKey(FR frRes, FR frVal);

  void privateIsIn(FR frRes, FR frPrivateName, FR frTarget, uint8_t cacheIdx);

  void createPrivateName(FR frRes, SHSymbolID symID);

  // x86-64: where arm64 supplies a single a64::CondCode, the x86 tables
  // supply an x86::CondCode *plus* a swapOperands flag. Only the
  // above-family conditions (kA, kAE) are false on unordered, which is the
  // property arm64's kGT/kGE/kMI/kLS all have and which the emitters rely
  // on; "less" and "less or equal" therefore become "above" and "above or
  // equal" on reversed operands rather than the NaN-unsafe kB/kBE.
#define DECL_COMPARE(                             \
    methodName,                                   \
    commentStr,                                   \
    slowCall,                                     \
    condCode,                                     \
    swapOperands,                                 \
    invSlow,                                      \
    passArgsByVal)                                \
  void methodName(FR rRes, FR rLeft, FR rRight) { \
    compareImpl(                                  \
        rRes,                                     \
        rLeft,                                    \
        rRight,                                   \
        commentStr,                               \
        x86::CondCode::condCode,                  \
        swapOperands,                             \
        (void *)slowCall,                         \
        #slowCall,                                \
        invSlow,                                  \
        passArgsByVal);                           \
  }
  DECL_COMPARE(greater, "greater", _sh_ljs_greater_rjs, kA, false, false, false)
  DECL_COMPARE(
      greaterEqual,
      "greater_equal",
      _sh_ljs_greater_equal_rjs,
      kAE,
      false,
      false,
      false)
  DECL_COMPARE(less, "less", _sh_ljs_less_rjs, kA, true, false, false)
  DECL_COMPARE(
      lessEqual,
      "less_equal",
      _sh_ljs_less_equal_rjs,
      kAE,
      true,
      false,
      false)
  DECL_COMPARE(equal, "Eq", _sh_ljs_equal_rjs, kE, false, false, false)
  DECL_COMPARE(notEqual, "Neq", _sh_ljs_equal_rjs, kNE, false, true, false)
#undef DECL_COMPARE

  void strictEqual(FR frRes, FR frLeft, FR frRight) {
    strictEqualImpl(false, frRes, frLeft, frRight);
  }
  void strictNotEqual(FR frRes, FR frLeft, FR frRight) {
    strictEqualImpl(true, frRes, frLeft, frRight);
  }

  // x86-64: see DECL_COMPARE above for the extra swapOperands column.
#define DECL_JCOND(                                                    \
    methodName,                                                        \
    forceNum,                                                          \
    passArgsByVal,                                                     \
    commentStr,                                                        \
    slowCall,                                                          \
    condCode,                                                          \
    swapOperands)                                                      \
  void methodName(                                                     \
      bool invert, const asmjit::Label &target, FR rLeft, FR rRight) { \
    jCond(                                                             \
        forceNum,                                                      \
        invert,                                                        \
        passArgsByVal,                                                 \
        target,                                                        \
        rLeft,                                                         \
        rRight,                                                        \
        commentStr,                                                    \
        x86::CondCode::condCode,                                       \
        swapOperands,                                                  \
        (void *)slowCall,                                              \
        #slowCall);                                                    \
  }
  DECL_JCOND(jGreater, false, false, "greater", _sh_ljs_greater_rjs, kA, false)
  DECL_JCOND(
      jGreaterEqual,
      false,
      false,
      "greater_equal",
      _sh_ljs_greater_equal_rjs,
      kAE,
      false)
  DECL_JCOND(jLess, false, false, "less", _sh_ljs_less_rjs, kA, true)
  DECL_JCOND(
      jLessEqual,
      false,
      false,
      "less_equal",
      _sh_ljs_less_equal_rjs,
      kAE,
      true)
  DECL_JCOND(jLessN, true, false, "less_n", _sh_ljs_less_rjs, kA, true)
  DECL_JCOND(
      jLessEqualN,
      true,
      false,
      "less_equal_n",
      _sh_ljs_less_equal_rjs,
      kAE,
      true)
  DECL_JCOND(jEqual, false, false, "eq", _sh_ljs_equal_rjs, kE, false)
#undef DECL_JCOND

  void
  jmpTypeOfIs(const asmjit::Label &target, FR frInput, TypeOfIsTypes types);

  void typeOfIs(FR frRes, FR frInput, TypeOfIsTypes types);

  void
  jStrictEqual(bool invert, const asmjit::Label &target, FR frLeft, FR frRight);

  void uintSwitchImm(
      FR frInput,
      const asmjit::Label &defaultLabel,
      llvh::ArrayRef<const asmjit::Label *> labels,
      uint32_t minVal,
      uint32_t maxVal);

  /// Information for a case of a StringSwitchImm instruction.
  struct StringSwitchCase {
    // The string id of the case label.
    uint32_t caseLabelStringId;
    // A JIT label for the start of JITted code for the the basic block
    // corresponding to the case.
    const asmjit::Label *target;

    StringSwitchCase(uint32_t caseLabelStringId, const asmjit::Label *target)
        : caseLabelStringId(caseLabelStringId), target(target) {}
  };

  /// Emit a string switch. The lookup table is identified at runtime by
  /// (\p runtimeModule, \p tableIndex) rather than by baking its address into
  /// the code, since the module's table vector may be reallocated after this
  /// code is compiled (e.g. by lazy compilation).
  void stringSwitchImm(
      FR frInput,
      RuntimeModule *runtimeModule,
      uint32_t tableIndex,
      const asmjit::Label &defaultLabel,
      llvh::ArrayRef<StringSwitchCase> cases);

  void getByVal(FR frRes, FR frSource, FR frKey);
  void getByIndex(FR frRes, FR frSource, uint32_t key);

#define DECL_PUT_BY_VAL(methodName, commentStr, shFn)                \
  void methodName(FR frTarget, FR frKey, FR frValue) {               \
    putByValImpl(frTarget, frKey, frValue, commentStr, shFn, #shFn); \
  }

  DECL_PUT_BY_VAL(putByValLoose, "putByValLoose", _sh_ljs_put_by_val_loose_rjs);
  DECL_PUT_BY_VAL(
      putByValStrict,
      "putByValStrict",
      _sh_ljs_put_by_val_strict_rjs);

  void putByValWithReceiver(
      FR frTarget,
      FR frKey,
      FR frValue,
      FR frReceiver,
      bool isStrict);

#define DECL_GET_BY_ID(methodName, commentStr, shFn)                           \
  void methodName(FR frRes, SHSymbolID symID, FR frSource, uint8_t cacheIdx) { \
    getByIdImpl(frRes, symID, frSource, cacheIdx, commentStr, shFn, #shFn);    \
  }

  void getByIdWithReceiver(
      FR frRes,
      SHSymbolID symID,
      FR frSource,
      FR frReceiver,
      uint8_t cacheIdx);
  void getByValWithReceiver(FR frRes, FR frSource, FR frKey, FR frReceiver);

  DECL_GET_BY_ID(getById, "getById", _sh_ljs_get_by_id_rjs)
  DECL_GET_BY_ID(tryGetById, "tryGetById", _sh_ljs_try_get_by_id_rjs)

#define DECL_PUT_BY_ID(methodName, strictMode, tryProp)                   \
  void methodName(                                                        \
      FR frTarget, SHSymbolID symID, FR frValue, uint8_t cacheIdx) {      \
    putByIdImpl(frTarget, symID, frValue, cacheIdx, strictMode, tryProp); \
  }

  DECL_PUT_BY_ID(putByIdLoose, false, false);
  DECL_PUT_BY_ID(putByIdStrict, true, false);
  DECL_PUT_BY_ID(tryPutByIdLoose, false, true);
  DECL_PUT_BY_ID(tryPutByIdStrict, true, true);

  void defineOwnInDenseArray(FR frArray, FR frProp, uint32_t idx);

  void
  defineOwnById(FR frTarget, SHSymbolID symID, FR frValue, uint8_t cacheIdx);
  void defineOwnByIndex(FR frTarget, FR frValue, uint32_t key);
  void defineOwnByVal(FR frTarget, FR frValue, FR frKey, bool enumerable);
  void defineOwnGetterSetterByVal(
      FR frTarget,
      FR frKey,
      FR frGetter,
      FR frSetter,
      bool enumerable);

  void getOwnBySlotIdx(FR frRes, FR frTarget, uint32_t slotIdx);
  void putOwnBySlotIdx(FR frTarget, FR frValue, uint32_t slotIdx);

  void delByVal(FR frRes, FR frTarget, FR frKey, bool strict);

  void addOwnPrivateBySym(FR frTarget, FR frKey, FR frValue);

  void getOwnPrivateBySym(FR frRes, FR frTarget, FR frKey, uint8_t cacheIdx);

  void putOwnPrivateBySym(FR frTarget, FR frKey, FR frValue, uint8_t cacheIdx);

  void instanceOf(FR frRes, FR frLeft, FR frRight);
  void isIn(FR frRes, FR frLeft, FR frRight);

  asmjit::Label newPrefLabel(const char *pref, size_t index);

  void newObject(FR frRes);
  void newObjectWithParent(FR frRes, FR frParent, uint32_t shapeTableIndex);
  void newObjectWithBuffer(
      FR frRes,
      uint32_t shapeTableIndex,
      uint32_t valBufferOffset);
  void newObjectWithBufferAndParent(
      FR frRes,
      FR frParent,
      uint32_t shapeTableIndex,
      uint32_t valBufferOffset);

  void newTypedObjectWithBuffer(
      FR frRes,
      FR frParent,
      uint32_t shapeTableIndex,
      uint32_t valBufferOffset,
      uint8_t nonEnumerable);

  void newArray(FR frRes, uint32_t size);
  void newArrayWithBuffer(
      FR frRes,
      uint32_t numElements,
      uint32_t numLiterals,
      uint32_t bufferIndex);

  void newFastArray(FR frRes, FR frProto, uint32_t size);
  void fastArrayLength(FR frRes, FR arr);
  void fastArrayLoad(FR frRes, FR arr, FR idx);
  void fastArrayStore(FR arr, FR idx, FR val);
  void fastArrayPush(FR arr, FR val);
  void fastArrayAppend(FR arr, FR other);

  void getGlobalObject(FR frRes);
  void declareGlobalVar(SHSymbolID symID);
  void createTopLevelEnvironment(FR frRes, uint32_t size);
  void createFunctionEnvironment(FR frRes, uint32_t size);
  void createEnvironment(FR frRes, FR frParent, uint32_t size);
  void getParentEnvironment(FR frRes, uint32_t level);
  void getEnvironment(FR frRes, FR frSource, uint32_t level);
  void getClosureEnvironment(FR frRes, FR frClosure);
  void loadFromEnvironment(FR frRes, FR frEnv, uint32_t slot);
  void storeToEnvironment(bool np, FR frEnv, uint32_t slot, FR frValue);
  void createClosure(
      FR frRes,
      FR frEnv,
      RuntimeModule *runtimeModule,
      uint32_t functionID);
  void createBaseClass(FR frRes, FR frPrototypeOut, FR frEnv);
  void
  createDerivedClass(FR frRes, FR frPrototypeOut, FR frEnv, FR frSuperClass);
  void createGenerator(
      FR frRes,
      FR frEnv,
      RuntimeModule *runtimeModule,
      uint32_t functionID);

  void getArgumentsPropByValLoose(FR frRes, FR frIndex, FR frLazyReg);
  void getArgumentsPropByValStrict(FR frRes, FR frIndex, FR frLazyReg);

  void reifyArgumentsLoose(FR frLazyReg);
  void reifyArgumentsStrict(FR frLazyReg);

  void getArgumentsLength(FR frRes, FR frLazyReg);

  void createThis(
      FR frRes,
      FR frCallee,
      FR frNewTarget,
      uint8_t cacheIdx,
      uint32_t shapeTableIndex);
  void selectObject(FR frRes, FR frThis, FR frConstructed);

  void loadThisNS(FR frRes);
  void coerceThisNS(FR frRes, FR frThis);
  void getNewTarget(FR frRes);

  void iteratorBegin(FR frRes, FR frSource);
  void iteratorNext(FR frRes, FR frIteratorOrIdx, FR frSourceOrNext);
  void iteratorClose(FR frIteratorOrIdx, bool ignoreExceptions);

  void debugger();
  void throwInst(FR frInput);
  void throwIfEmpty(FR frRes, FR frInput) {
    throwIfEmptyUndefinedImpl(frRes, frInput, true);
  }
  void throwIfUndefined(FR frRes, FR frInput) {
    throwIfEmptyUndefinedImpl(frRes, frInput, false);
  }
  void throwIfThisInitialized(FR frInput);

  void createRegExp(
      FR frRes,
      SHSymbolID patternID,
      SHSymbolID flagsID,
      uint32_t regexpID);

  void loadParentNoTraps(FR frRes, FR frObj);
  void typedLoadParent(FR frRes, FR frObj);

  /// Emit, only when emitTypeAsserts_ is set, a trap-on-violation check
  /// that the value of \p fr, currently held in \p hwVal, satisfies
  /// \p pred.
  ///
  /// Uses only xScratch (r11) and never touches the register allocator, so
  /// it is a pure insertion -- unlike arm64, which reserves a second
  /// dedicated scratch register (xScratch2) for predicates that would
  /// otherwise need two registers, this backend has none to spare and
  /// instead formulates every check as a single non-destructive tag
  /// extraction into xScratch (see emitTypeAssertGpX). It clobbers EFLAGS,
  /// so the caller must have verified that flags are dead at the
  /// insertion point. That is an obligation, not a property emitters have
  /// in general: selectObject (on both backends) holds flags across
  /// getOrAllocFRInGpX.
  ///
  /// Where an emitter knows a type fact per operand, guard each check on
  /// that operand's own fact, never on the emitter's combined fast-path
  /// condition: the point is to assert every fact the JIT holds, not only
  /// the ones the chosen code shape happens to rely on.
  void emitTypeAssert(FR fr, HWReg hwVal, TypePred pred);

  /// Emit, at a bytecode instruction boundary, the global-register-class
  /// checks for every FR recorded by recordFRWriteForAssert() since the
  /// last call, then clear the recorded set. Called from compileBB as a
  /// sibling of assertPostInstructionInvariants(), never from inside it:
  /// that function's body is compiled out under NDEBUG, and emitting
  /// checks from within it would silently disable Class C in
  /// release-with-flag builds.
  ///
  /// This checks the value each FR holds at the boundary, not at the
  /// instruction's write to it. An instruction that writes a
  /// non-conforming value, calls the runtime (a GC safepoint), and then
  /// overwrites it with a conforming one is not caught; only the value
  /// that survives the instruction is.
  void emitPendingTypeAsserts() {
    if (LLVM_LIKELY(typeAssertPendingWrites_.empty()))
      return;
    emitPendingTypeAssertsSlow();
  }

 private:
  /// \return the byte offset of \p fr's slot from xFrame.
  static constexpr inline uint32_t frByteOffset(FR fr) {
    return (fr.index() + hbc::StackFrameLayout::FirstLocal) *
        sizeof(SHLegacyValue);
  }

  /// Create an x86::Mem addressing a specific frame register.
  /// x86-64: every memory operand takes a signed 32-bit displacement, which
  /// spans any frame the bytecode can describe, so unlike arm64 there is no
  /// immediate-range fallback anywhere in the frame accessors.
  static inline x86::Mem frMem(FR fr) {
    return x86::qword_ptr(xFrame, (int32_t)frByteOffset(fr));
  }

  /// Return true if we are logging, false otherwise.
  bool hasLogger() {
#ifndef ASMJIT_NO_LOGGING
    return logger_ != nullptr;
#else
    return false;
#endif
  }

  /// Load an arbitrary bit pattern into a Gp.
  /// x86-64: any 64-bit constant is a single mov, so there is neither a
  /// cheap/expensive split nor an RO-data fallback. \p constName is kept
  /// for signature parity with arm64.
  template <typename REG>
  void loadBits64InGp(const REG &dest, uint64_t bits, const char *constName) {
    (void)constName;
    a.mov(dest, asmjit::Imm(bits));
  }

  /// Load the bit pattern of the SmallHermesValue \p shv into \p dest.
  void loadSmallHermesValueInGpX(
      const x86::Gp &dest,
      SmallHermesValue shv,
      const char *constName);

  /// Load the StringPrimitive for \p id as a pointer into \p out.
  /// The StringPrimitive must already be known to be allocated in the
  /// IdentifierTable at JIT time.
  ///
  /// x86-64: arm64 takes an extra temporary, because the lookup entry's
  /// offset can outgrow its scaled load immediate. Every x86 displacement is
  /// a signed 32-bit one, which covers any entry the identifier table can
  /// hold, so there is no fallback and no temporary.
  void loadConstStringInGpX(SymbolID id, const x86::Gp &out);

  void _loadFrame(HWReg dest, FR rFrom) {
    if (dest.isGpX())
      a.mov(dest.gpq(), frMem(rFrom));
    else
      a.vmovsd(dest.xmm(), frMem(rFrom));
  }
  void _storeFrame(HWReg src, FR rFrom) {
    if (src.isGpX())
      a.mov(frMem(rFrom), src.gpq());
    else
      a.vmovsd(frMem(rFrom), src.xmm());
  }

  bool isTempGpX(HWReg hwReg) const {
    assert(hwReg.isGpX());
    unsigned index = hwReg.indexInClass();
    return (index >= kGPTemp1.first && index <= kGPTemp1.second) ||
        (index >= kGPTemp2.first && index <= kGPTemp2.second);
  }

  bool isTempVecD(HWReg hwReg) const {
    assert(hwReg.isVecD());
    unsigned index = hwReg.indexInClass();
    return index >= kVecTemp.first && index <= kVecTemp.second;
  }

  bool isTemp(HWReg hwReg) const {
    return hwReg.isGpX() ? isTempGpX(hwReg) : isTempVecD(hwReg);
  }

  template <bool use>
  void movHWFromHW(HWReg dst, HWReg src);
  void _storeHWToFrame(FR fr, HWReg src);
  void movHWFromFR(HWReg hwRes, FR src);
  void movHWFromMem(HWReg hwRes, x86::Mem src);

  /// Move a value from a hardware register \p src to the frame register \p
  /// frDest.
  void movFRFromHW(FR frDest, HWReg src, FRType type);

  /// In rare cases, such as when we have in/out parameters to operations, the
  /// frame may get updated with a new value. This will ensure that the frame is
  /// marked up-to-date, and that any associated global register holds the same
  /// value.
  void syncFrameOutParam(FR fr, FRType type = FRType::UnknownPtr);

  template <class TAG>
  HWReg _allocTemp(TempRegAlloc &ra, llvh::Optional<HWReg> preferred);
  HWReg allocTempGpX(llvh::Optional<HWReg> preferred = llvh::None) {
    assert((!preferred || preferred->isGpX()) && "invalid preferred register");
    return _allocTemp<HWReg::GpX>(gpTemp_, preferred);
  }
  HWReg allocTempVecD(llvh::Optional<HWReg> preferred = llvh::None) {
    assert((!preferred || preferred->isVecD()) && "invalid preferred register");
    return _allocTemp<HWReg::VecD>(vecTemp_, preferred);
  }
  HWReg allocAndLogTempGpX() {
    HWReg res = allocTempGpX();
    comment("    ; alloc: r%u (temp)", res.indexInClass());
    return res;
  }
  /// Free \p hwReg, which may be any HWReg.
  void freeReg(HWReg hwReg);
  /// If \p hwReg is a valid temp associated with an FR, sync it to the global
  /// register if the FR has one, else store it in the frame. Then free the
  /// temp, making it available to be used again.
  /// Else, do nothing.
  void syncAndFreeTempReg(HWReg hwReg);
  HWReg useReg(HWReg hwReg);

  /// Ensure that an HWReg currently containing an FR is available to be used
  /// again by "spilling" its value to its canonical location (either the frame
  /// or a global reg). Conceptually it then frees the HWReg and immediately
  /// allocates it again, so now it is as if it was just allocated.
  /// \pre \p toSpill must have a corresponding FR.
  void _spillTempForFR(HWReg toSpill);

  /// Ensure that \p fr is stored in the frame so that we can take its address
  /// (e.g. when passing the address of \p fr as a param to a function).
  ///
  /// Store any temporary or global register associated with \p fr to the frame
  /// in memory.
  void syncToFrame(FR fr);

  /// Ensure all FRs have their values stored in either global registers or the
  /// frame, not just temporary registers.
  /// Must run before calls because temporary registers will be clobbered by the
  /// call.
  ///
  /// Sync all temporary registers associated with FRs to either the global
  /// register or the frame.
  /// \param exceptFR If specified, do not sync this FR (used for output FRs
  /// that we aren't going to load from before storing to them anyway).
  void syncAllFRTempExcept(FR exceptFR);

  /// Free all temporary registers associated with FRs except \p exceptFR.
  void freeAllFRTempExcept(FR exceptFR);

  /// Free any temporary register associated with \p FR.
  void freeFRTemp(FR fr);

  void _assignAllocatedLocalHWReg(FR fr, HWReg hwReg);

  /// \return a valid register if the FR is in a hw register, otherwise invalid.
  HWReg _isFRInRegister(FR fr);
  HWReg getOrAllocFRInVecD(
      FR fr,
      bool load,
      llvh::Optional<HWReg> preferred = llvh::None);
  HWReg getOrAllocFRInGpX(
      FR fr,
      bool load,
      llvh::Optional<HWReg> preferred = llvh::None);
  HWReg getOrAllocFRInAnyReg(
      FR fr,
      bool load,
      llvh::Optional<HWReg> preferred = llvh::None);

  void
  frUpdatedWithHW(FR fr, HWReg hwReg, FRType localType = FRType::UnknownPtr);
  void frUpdateType(FR fr, FRType type);

  /// \return true if the FR is currently known to contain the specified type.
  bool isFRKnownType(FR fr, FRType frType) const {
    auto &frState = frameRegs_[fr.index()];
    return frState.globalType == frType || frState.localType == frType;
  }
  /// \return true if the FR is currently known to contain a number.
  bool isFRKnownNumber(FR fr) const {
    return isFRKnownType(fr, FRType::Number);
  }
  /// \return true if the FR is currently known to contain a number.
  bool isFRKnownBool(FR fr) const {
    return isFRKnownType(fr, FRType::Bool);
  }
  /// \return true if the FR is currently known to contain an OtherNonPtr.
  bool isFRKnownOtherNonPtr(FR fr) const {
    return isFRKnownType(fr, FRType::OtherNonPtr);
  }

  /// Record that \p fr was written, so that the instruction boundary can
  /// check the value against its global register class. Records nothing
  /// unless the FR owns a global register. Callers must check
  /// emitTypeAsserts_ themselves; this does not.
  void recordFRWriteForAssert(FR fr);

  /// Get the current bytecode IP in \p out.
  /// x86-64: the whole address is a single 64-bit immediate, so unlike arm64
  /// there is no "materialize the function start, then add the offset" chain
  /// and no immediate-range special case.
  void getBytecodeIP(const x86::Gp &out);

  /// If counters are enabled, emit code to increment \p counter.
  /// Clobbers xScratch and EFLAGS; call it only where both are dead.
  void emitIncrementCounter(JitCounter counter);

  /// Set up the call frame and perform the call. The caller should have
  /// already populated the arg count and new target registers.
  /// \param frRes is the frame register that will contain the result.
  /// \param frCallee is a frame register containing the callee.
  ///
  /// The register contract of the call sequence in the definition below.
  /// Both the fast path (a direct call into another JIT compiled function)
  /// and the slow path (an indirect call through VTable::jitCallArray)
  /// arrive at contLab having prepared the same three registers, so that
  /// contLab itself is a single shared call sequence:
  ///   eax - the callee's CellKind. Produced by the fast path's type check
  ///         and consumed by the slow path, which indexes jitCallArray with
  ///         it. It is dead at contLab, and is deliberately the register
  ///         the call's return value lands in.
  ///   rsi - the Callable *, i.e. the second argument of an
  ///         ObjectJitCallPtr. A JIT compiled function ignores it; the
  ///         jitCall thunks do not.
  ///   rdx - the address to call (arm64's x2). The slow path materializes
  ///         &VTable::jitCallArray into xScratch to index it, because a
  ///         scaled-index memory operand has no absolute-address form.
  /// contLab then adds the first argument, rdi = Runtime, and calls rdx.
  /// Every temp is free at this point (everything was synced above), which
  /// is what lets the two paths agree on fixed registers at all. rsp % 16
  /// == 0 holds at every instruction an emitter can be entered at, so the
  /// call site is SysV-aligned without any adjustment here.
  ///
  /// The prologue is what establishes that alignment, and one exception to
  /// "nothing moves rsp afterwards" exists: an emitter may adjust rsp
  /// transiently to pass stack arguments, but only in multiples of 16 and
  /// only within its own emission, restoring it before it emits anything
  /// else. There are two instances today: putByIdImpl -- _jit_put_by_id
  /// takes eight arguments where SysV has six argument registers, so it
  /// pushes two and follows the call with `add rsp, 16` -- and, in ASan
  /// builds, bumpAllocAndUnpoison's save/restore of the eight GP temps
  /// around its __asan_unpoison_memory_region call. Anything emitted while
  /// rsp is lowered must therefore avoid rsp-relative frame access, which
  /// nothing on either path does (loadFrameAddr goes through xFrame, and
  /// callRuntimeWithSavedIP touches only xScratch and xRuntime).
  ///
  /// That contract is enforced in debug builds, not left as a convention:
  /// x86-64: `rspDelta_` is a running count of the current rsp delta, bumped
  /// by both of those sites and brought back down by their restores.
  /// callRuntime() asserts it is a multiple of 16 at every call emission,
  /// assertPostInstructionInvariants() and leave() assert it is exactly 0.
  /// Together that catches both an odd push count and a delta left
  /// unrestored, including if a third stack-argument call site appears.
  void callImpl(FR frRes, FR frCallee);

  /// Load the address of \p frameReg's frame slot into \p dst.
  /// x86-64: every frame slot is reachable through lea's signed 32-bit
  /// displacement, so unlike arm64 there is no immediate-range fallback.
  void loadFrameAddr(const x86::Gp &dst, FR frameReg) {
    a.lea(dst, x86::ptr(xFrame, (int32_t)frByteOffset(frameReg)));
  }

  /// \return a RIP-relative qword operand addressing the 64-bit constant
  /// \p bits, interning it in RO data on first use. x86 has no
  /// floating-point immediate, so a constant an emitter needs in a vector
  /// register has to come from memory.
  ///
  /// The RO data block is emitted wherever the code stream happens to end,
  /// so its absolute address carries no alignment beyond the code buffer's
  /// own. Entries are therefore usable only by instructions with no
  /// alignment requirement -- which is every VEX-encoded memory operand
  /// this backend emits, but not the legacy aligned-SSE forms. An emitter
  /// that needs an aligned operand must first make emitROData() align the
  /// block, with an \c a.align() before it binds roDataLabel_.
  x86::Mem roConst64(uint64_t bits, const char *comment) {
    return x86::qword_ptr(roDataLabel_, uint64Const(bits, comment));
  }

  void arithUnop(
      bool forceNumber,
      FR frRes,
      FR frInput,
      const char *name,
      void (*fast)(
          Emitter &em,
          const x86::Xmm &d,
          const x86::Xmm &s,
          const x86::Xmm &tmp),
      void *slowCall,
      const char *slowCallName);

  void arithBinOp(
      bool forceNumber,
      FR frRes,
      FR frLeft,
      FR frRight,
      const char *name,
      void (*fast)(
          x86::Assembler &a,
          const x86::Xmm &res,
          const x86::Xmm &dl,
          const x86::Xmm &dr),
      void *slowCall,
      const char *slowCallName);

  // x86-64: \p fast takes one source instead of arm64's two, and \p
  // rightInCl has no arm64 counterpart; see DECL_BIT_BINOP.
  void bitBinOp(
      FR frRes,
      FR frLeft,
      FR frRight,
      bool unsignedRes,
      bool rightInCl,
      const char *name,
      SHLegacyValue (*slowCall)(
          SHRuntime *shr,
          const SHLegacyValue *a,
          const SHLegacyValue *b),
      const char *slowCallName,
      void (
          *fast)(x86::Assembler &a, const x86::Gp &res, const x86::Gp &right));

  /// x86-64 helper with no arm64 counterpart: materialize the boolean result
  /// of a preceding \c vucomisd into \p res as 0 or 1.
  /// \p cc must be one of kA/kAE/kE/kNE. kA and kAE are false when the
  /// compare was unordered, exactly like the arm64 condition codes the
  /// emitters were written against, so \c setcc alone is correct for them.
  /// kE and kNE read ZF, which \c vucomisd *sets* on unordered, so where
  /// unordered can still reach here (\p unorderedPossible) the result is
  /// patched to what an unordered compare must produce: not equal.
  void setBoolFromCompare(
      const x86::Gp &res,
      x86::CondCode cc,
      bool unorderedPossible);

  // x86-64: \p condCode and \p swapOperands together are arm64's single
  // condCode; see DECL_COMPARE.
  void compareImpl(
      FR frRes,
      FR frLeft,
      FR frRight,
      const char *name,
      x86::CondCode condCode,
      bool swapOperands,
      void *slowCall,
      const char *slowCallName,
      bool invSlow,
      bool passArgsByVal);

  void strictEqualImpl(bool invert, FR frRes, FR frLeft, FR frRight);

  void putByValImpl(
      FR frTarget,
      FR frKey,
      FR frValue,
      const char *name,
      void (*shImpl)(
          SHRuntime *shr,
          SHLegacyValue *target,
          SHLegacyValue *key,
          SHLegacyValue *value),
      const char *shImplName);

  class GetByIdImpl;
  void getByIdImpl(
      FR frRes,
      SHSymbolID symID,
      FR frSource,
      uint8_t cacheIdx,
      const char *name,
      SHLegacyValue (*shImpl)(
          SHRuntime *shr,
          const SHLegacyValue *source,
          SHSymbolID symID,
          SHReadPropertyCacheEntry *propCacheEntry),
      const char *shImplName);

  void putByIdImpl(
      FR frTarget,
      SHSymbolID symID,
      FR frValue,
      uint8_t cacheIdx,
      bool strictMode,
      bool tryProp);

  void throwIfEmptyUndefinedImpl(FR frRes, FR frInput, bool empty);

  // x86-64: \p condCode and \p swapOperands together are arm64's single
  // condCode; see DECL_JCOND.
  void jCond(
      bool forceNumber,
      bool invert,
      bool passArgsByVal,
      const asmjit::Label &target,
      FR frLeft,
      FR frRight,
      const char *name,
      x86::CondCode condCode,
      bool swapOperands,
      void *slowCall,
      const char *slowCallName);

  /// Bump allocate \p sz bytes on the GC heap and store the result in \p out.
  /// If not possible, jump to the \p slowPathLab.
  /// \param sz is the aligned number of bytes to bump the pointer by.
  /// \param out is the register to store the address of the new object.
  /// \param temp1 is a temporary register.
  /// \param temp2 is a temporary register.
  /// \param slowPathLab is the label to jump to if the allocation fails.
  ///
  /// Precondition (ASan builds only): no live value may be held in any
  /// vector temp, nor in xScratch (r11), across this call -- the ASan
  /// save loop pushes only the 8 GP temps (kGPTemp1/kGPTemp2), since SysV
  /// has no callee-saved xmm registers. \p out, \p temp1, and \p temp2
  /// must not be xScratch either, since it is not part of that saved set.
  void bumpAllocAndUnpoison(
      uint32_t sz,
      const x86::Gp &out,
      const x86::Gp &temp1,
      const x86::Gp &temp2,
      const asmjit::Label &slowPathLab);

  /// Initialize a GCCell at the pointer given.
  /// \param kind the CellKind to populate.
  /// \param sz the aligned total size of the cell.
  /// \param cell pointer to the start of the cell.
  /// \param temp1 is a temporary, must not be the same as cell.
  void initGCCell(
      CellKind kind,
      uint32_t sz,
      const x86::Gp &cell,
      const x86::Gp &temp1);

  /// Emit the code to perform an allocation in the young generation, populating
  /// the fields of the new GCCell.
  /// \param kind is the CellKind of object to allocate.
  /// \param sz is the size of the object to allocate.
  /// \param out is the register to store the address of the new object.
  /// \param temp1 is a temporary register.
  /// \param temp2 is a temporary register.
  /// \param slowPathLab is the label to jump to if the allocation fails.
  ///
  /// Precondition (ASan builds only): same as bumpAllocAndUnpoison() --
  /// no live value in any vector temp or xScratch across the call, and
  /// \p out, \p temp1, \p temp2 must not be xScratch.
  void allocInYoung(
      CellKind kind,
      uint32_t sz,
      const x86::Gp &out,
      const x86::Gp &temp1,
      const x86::Gp &temp2,
      const asmjit::Label &slowPathLab);

  /// Emit the code to perform an allocation in the young generation, populating
  /// the fields of the new GCCell.
  /// \param kind1 is the CellKind of object 1 to allocate.
  /// \param sz1 is the size of the object 1 to allocate.
  /// \param kind2 is the CellKind of object 2 to allocate.
  /// \param sz2 is the size of the object 2 to allocate.
  /// \param out1 is the register to store the address of the first object.
  /// \param out2 is the register to store the address of the second object.
  /// \param temp is a temporary register.
  /// \param slowPathLab is the label to jump to if the allocation fails.
  ///
  /// Precondition (ASan builds only): same as bumpAllocAndUnpoison() --
  /// no live value in any vector temp or xScratch across the call, and
  /// \p out1, \p out2, \p temp must not be xScratch.
  void alloc2InYoung(
      CellKind kind1,
      uint32_t sz1,
      CellKind kind2,
      uint32_t sz2,
      const x86::Gp &out1,
      const x86::Gp &out2,
      const x86::Gp &temp,
      const asmjit::Label &slowPathLab);

 private:
  /// Allocate or return the offset in RO DATA of the current function's debug
  /// name, in the format ID(name).
  int32_t getDebugFunctionName();

  /// \return the number of bytes subtracted from/added to rsp by the single
  /// sub/add that follows the pushes. Covers the optional SHJmpBuf and saved
  /// SHLocals, plus the padding that restores 16-byte alignment.
  uint32_t getStackSize() const {
    return getSavedRegsPadding() + getExceptionAreaSize();
  }

  /// \return the size of the optional SHJmpBuf + saved SHLocals area, which
  /// is a multiple of 16 so that it cannot perturb the stack alignment.
  uint32_t getExceptionAreaSize() const {
    return catchTableLabel_.isValid() ? llvh::alignTo(sizeof(SHJmpBuf) + 8, 16)
                                      : 0;
  }

  /// \return the alignment padding between the saved registers and the
  /// exception area (SHLocals*/SHJmpBuf). See the arithmetic in
  /// frameSetup().
  uint32_t getSavedRegsPadding() const {
    return 8 * (gpSaveCount_ % 2);
  }

  /// \return the offset of the SHJmpBuf from rsp. The SHJmpBuf sits directly
  /// above rsp (below the padding), so it is 16-byte aligned regardless of
  /// the parity of gpSaveCount_ -- see the frame-layout comment in
  /// frameSetup().
  uint32_t getJmpBufOffset() const {
    assert(catchTableLabel_.isValid() && "no SHJmpBuf on stack");
    return 0;
  }

  /// \return the offset of the saved SHLocals pointer from rsp.
  uint32_t getSavedSHLocalsOffset() const {
    assert(catchTableLabel_.isValid() && "no saved SHLocals * on stack");
    return sizeof(SHJmpBuf);
  }

  /// \return true if \c emittingIP is in a try (i.e. exceptions can be observed
  ///   in this function).
  bool isInTry() const {
    return codeBlock_->findCatchTargetOffset(
               codeBlock_->getOffsetOf(emittingIP)) != -1;
  }

  /// Emit the function prologue.
  /// \param numFrameRegs the number of JS frame registers to reserve.
  /// \param gpSaveCount the number of kGPSavedList entries used by globals.
  void frameSetup(unsigned numFrameRegs, unsigned gpSaveCount);

  // x86-64: unlike arm64, label names here are drawn from a dedicated
  // monotonically-increasing counter rather than slowPaths_.size(). The
  // latter is non-monotonic -- emitSlowPaths() pops from the front as it
  // emits -- and two label pairs created before either is emplaced would
  // sample the same size() and collide. newSlowPathLabel() advances the
  // counter and newContLabel() reads it back, so a SLOW/CONT pair always
  // shares one index and no two pairs ever collide.
  asmjit::Label newSlowPathLabel() {
    return newPrefLabel("SLOW_", ++slowPathLabelCounter_);
  }
  asmjit::Label newContLabel() {
    return newPrefLabel("CONT_", slowPathLabelCounter_);
  }

  /// Emit a call to \p fn, saving the bytecode IP to Runtime::currentIP
  /// before making the call. This should be used for all calls that may
  /// observe the IP, such as calls that may throw exceptions, or perform
  /// allocations.
  void callRuntimeWithSavedIP(void *fn, const char *name);

  /// Emit a call to \p fn without saving the IP. This should be used only
  /// where saving the IP is unnecessary or incorrect.
  void callRuntime(void *fn, const char *name);

  /// Emit the code that runs when this function is longjmped to.
  /// Performs the catch table lookup and jumps to the appropriate catch block,
  /// and if no catch block is found, pops the SHJmpBuf and rethrows the
  /// exception.
  void emitCatchTable(llvh::ArrayRef<const asmjit::Label *> exceptionHandlers);

  void emitSlowPaths();

  int32_t reserveData(
      int32_t dsize,
      size_t align,
      asmjit::TypeId typeId,
      int32_t itemCount,
      const char *comment = nullptr);
  /// Register a 64-bit constant in RO DATA and return its offset.
  int32_t uint64Const(uint64_t bits, const char *comment);

  void emitROData();

  /// Emit \c emitTypeAssert's check sequence for \p pred against \p val,
  /// which holds the current value of \p fr, recording a TypeAssertSite.
  /// The caller emits the dump comment, so that it precedes any load it
  /// had to emit to produce \p val.
  void emitTypeAssertGpX(FR fr, const x86::Gp &val, TypePred pred);

  /// Like \c emitTypeAssert, but for an \p fr that the fast path never
  /// materializes into a register: reads it with \c readFRForAssert first.
  /// Like \c emitTypeAssert, it does nothing unless emitTypeAsserts_ is
  /// set, so callers need not check it themselves.
  void emitTypeAssertFR(FR fr, TypePred pred);

  /// Read the current value of \p fr into xScratch, for use immediately
  /// before an \c emitTypeAssertGpX call, without allocating or perturbing
  /// any FRState. Honors the FRState up-to-date invariants rather than
  /// merely the location priority: the local register if any (locals are
  /// always current), else the global register only if
  /// globalRegUpToDate, else the frame slot (asserting frameUpToDate).
  /// \pre \p fr is not dirty (regIsDirty).
  void readFRForAssert(FR fr);

  /// The out-of-line body of \c emitPendingTypeAsserts.
  /// \pre the pending set is not empty.
  void emitPendingTypeAssertsSlow();

  /// Emit the shared out-of-line tail that all type assert failure stubs
  /// jump to, if any type assert was emitted for this function.
  void emitTypeAssertFailTail();

  /// Slow version of newObjectWithBuffer.
  /// Used temporarily while full functionality is being added to
  /// newObjectWithBuffer.
  void newObjectWithBufferSlow(
      FR frRes,
      uint32_t shapeTableIndex,
      uint32_t valBufferOffset);

  /// Initialize or obtain an existing lazy JIT ID for the given hidden class.
  /// NOTE: this call performs GC allocations, to the HC might move. The raw
  /// pointer MUST NOT be used after this call.
  /// \return 0 if too many IDs have been assigned.
  uint16_t initHCLazyIDMayAlloc(HiddenClass *hc);

  /// Report that the emitter does not yet support \p what, aborting
  /// compilation of the current function cleanly (the JITContext::Compiler
  /// driver catches this and falls back to the interpreter). Routes through
  /// the same expected-error / longjmp mechanism as a genuine AsmJit error.
  [[noreturn]] void unsupported(const char *what);
}; // class Emitter

/// Only the logger check lives here; the formatting is out of line in
/// commentV(). The check has to be visible to every emitter translation unit:
/// when ASMJIT_NO_LOGGING is defined hasLogger() folds to a constant false, so
/// the compiler can drop the call and dead-strip the format string. With the
/// whole body in JitEmitter.cpp, callers in other translation units had to
/// materialise and pass every string, which cost ~4KB of .cstring. Keeping
/// vsnprintf out of line means enabling logging does not duplicate the
/// formatting code into each caller.
inline void Emitter::comment(const char *fmt, ...) {
  if (!hasLogger())
    return;
  va_list args;
  va_start(args, fmt);
  commentV(fmt, args);
  va_end(args);
}

template <bool use>
void Emitter::movHWFromHW(HWReg dst, HWReg src) {
  if (dst != src) {
    // x86-64: vmovaps is the reg-to-reg vector move (no partial-register
    // merge, unlike the three-operand vmovsd), and vmovq moves the whole
    // 64-bit pattern between a Gp and the low half of an Xmm.
    if (dst.isVecD() && src.isVecD())
      a.vmovaps(dst.xmm(), src.xmm());
    else if (dst.isVecD())
      a.vmovq(dst.xmm(), src.gpq());
    else if (src.isVecD())
      a.vmovq(dst.gpq(), src.xmm());
    else
      a.mov(dst.gpq(), src.gpq());
  }
  if constexpr (use) {
    useReg(src);
    useReg(dst);
  }
}

template <class TAG>
HWReg Emitter::_allocTemp(TempRegAlloc &ra, llvh::Optional<HWReg> preferred) {
  llvh::Optional<unsigned> pr{};
  if (preferred)
    pr = preferred->indexInClass();
  if (auto optReg = ra.alloc(pr); optReg)
    return HWReg(*optReg, TAG{});
  // Spill one register.
  unsigned index = pr ? *pr : ra.leastRecentlyUsed();
  _spillTempForFR(HWReg(index, TAG{}));
  ra.free(index);
  // Allocate again. This must succeed.
  return HWReg(*ra.alloc(), TAG{});
}

} // namespace hermes::vm::x86_64
