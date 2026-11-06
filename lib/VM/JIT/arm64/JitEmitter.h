/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "asmjit/a64.h"

#include "../JitHandlers.h"

#include "hermes/ADT/DenseUInt64.h"
#include "hermes/ADT/SimpleLRU.h"
#include "hermes/Support/OptValue.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/JIT/JIT.h"
#include "hermes/VM/JIT/PerfJitDump.h"
#include "hermes/VM/JIT/arm64/JIT.h"
#include "hermes/VM/RuntimeModule.h"
#include "hermes/VM/static_h.h"
#include "hermes/VMLayouts/StackFrameLayout.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/SmallVector.h"

#include <cstdarg>
#include <deque>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace hermes::vm::arm64 {

namespace a64 = asmjit::a64;

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
  // 0..31: GpX. 32..63: VecD. 128: invalid.
  uint8_t index_;

  explicit constexpr HWReg(uint8_t index) : index_(index) {}

 public:
  struct GpX {};
  struct VecD {};

  constexpr HWReg() : index_(0xFF) {}
  explicit constexpr HWReg(uint8_t index, GpX) : index_(index) {}
  explicit constexpr HWReg(uint8_t index, VecD) : index_(index + 32) {}
  explicit constexpr HWReg(const a64::GpX &gpx) : HWReg(gpx.id(), GpX{}) {}
  explicit constexpr HWReg(const a64::VecD &vecd) : HWReg(vecd.id(), VecD{}) {}

  static constexpr HWReg gpX(uint8_t index) {
    assert(index < 31 && "invalid GpX");
    return HWReg(index, GpX{});
  }
  static constexpr HWReg vecD(uint8_t index) {
    assert(index < 32 && "invalid VecD");
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

  a64::GpX a64GpX() const {
    assert(isGpX());
    return a64::GpX(indexInClass());
  }
  a64::VecD a64VecD() const {
    assert(isVecD());
    return a64::VecD(indexInClass());
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
    const hermes::vm::arm64::HWReg &hwReg);

/// A frame register can reside simultaneously in one or more of the following
/// locations:
/// - The stack frame
/// - A global callee-save register (which can be either GpX or VecD)
/// - A local GpX register
/// - A local VecD register.
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

// x19 is runtime
static constexpr auto xRuntime = a64::x19;
// x20 is frame
static constexpr auto xFrame = a64::x20;

/// Scratch registers. x16/x17 sit outside the register allocator and are
/// used as scratch (call targets, IP materialization, type assert check
/// sequences); nothing holds a value in them across an emitter call.
static constexpr auto xScratch = a64::x16;
static constexpr auto xScratch2 = a64::x17;

/// GP arg registers (inclusive).
// static constexpr std::pair<uint8_t, uint8_t> kGPArgs(0, 7);
/// Temporary GP registers (inclusive).
static constexpr std::pair<uint8_t, uint8_t> kGPTemp(0, 15);
/// Callee-saved GP registers (inclusive).
static constexpr std::pair<uint8_t, uint8_t> kGPSaved(21, 28);

/// Vec arg registers (inclusive).
// static constexpr std::pair<uint8_t, uint8_t> kVecArgs(0, 7);
/// Temporary vec registers (inclusive).
static constexpr std::pair<uint8_t, uint8_t> kVecTemp1(0, 7);
static constexpr std::pair<uint8_t, uint8_t> kVecTemp2(16, 31);
/// Callee-saved vec registers (inclusive).
static constexpr std::pair<uint8_t, uint8_t> kVecSaved(8, 15);

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
  TempRegAlloc gpTemp_{kGPTemp};
  /// VecD temp registers.
  TempRegAlloc vecTemp_{kVecTemp1, kVecTemp2};

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
    /// Inline storage for the lambda's captures. Sized to the largest current
    /// slow path, jCond, whose captures include an asmjit::Label (16 bytes, an
    /// Operand) plus three pointers, two FRs and two bools. Raising this is
    /// fine; the static_assert above is what keeps a too-large capture from
    /// becoming a silent heap allocation.
    alignas(void *) char storage_[56];
  };
  /// Queue of slow paths.
  std::deque<SlowPath> slowPaths_{};

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

  /// Label to branch to when returning from a function. Return value will be
  /// in x22.
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

  /// The version record of the body being emitted, owned by the compile
  /// driver until install. Its address is embedded as the identity
  /// argument of the recording runtime helpers, so the body they are
  /// called from is the body they report about.
  JitVersionData *const versionData_;

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

  unsigned gpSaveCount_ = 0;
  unsigned vecSaveCount_ = 0;

 public:
  asmjit::CodeHolder code{};
  a64::Assembler a{};
  /// The IP of the instruction being emitted.
  const inst::Inst *emittingIP{nullptr};
  /// Write-cache indices of PutById sites whose specialization was
  /// skipped in this compile because the cache had no class yet. Moved
  /// into the candidate JitVersionData::coldWriteCacheIdxs by the
  /// compile driver. The arm64 emitter does not yet report cold sites,
  /// so this stays empty and the recompilation mechanism is dormant on
  /// arm64; its cold-site reporting arrives with the arm64 port.
  llvh::SmallVector<uint8_t, 4> coldWriteCacheIdxs_;
  /// Read-cache indices of GetById sites whose specialization was
  /// skipped in this compile because the cache had no class yet. Same
  /// role as coldWriteCacheIdxs_, for GetById sites; also never appended
  /// to on arm64 today.
  llvh::SmallVector<uint8_t, 4> coldReadCacheIdxs_;

  /// \return the candidate version record's ByVal site entry for
  /// \p siteId, creating the records and/or entry if absent. The
  /// entry's address is stable (deque) and may be embedded in code.
  JitByValSiteRecord &byValSiteRecord(uint32_t siteId) {
    if (!versionData_->consumerRecords)
      versionData_->consumerRecords = std::make_unique<JitConsumerRecords>();
    return versionData_->consumerRecords->findOrCreateByValSite(siteId);
  }

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
      JitVersionData *versionData,
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

  /// Save the return value in x22.
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

#define DECL_BINOP(methodName, forceNum, commentStr, slowCall, a64body) \
  void methodName(FR rRes, FR rLeft, FR rRight) {                       \
    arithBinOp(                                                         \
        forceNum,                                                       \
        rRes,                                                           \
        rLeft,                                                          \
        rRight,                                                         \
        commentStr,                                                     \
        [](a64::Assembler & as,                                         \
           const a64::VecD &res,                                        \
           const a64::VecD &dl,                                         \
           const a64::VecD &dr) a64body,                                \
        (void *)slowCall,                                               \
        #slowCall);                                                     \
  }

  DECL_BINOP(mul, false, "mul", _sh_ljs_mul_rjs, { as.fmul(res, dl, dr); })
  DECL_BINOP(add, false, "add", _sh_ljs_add_rjs, { as.fadd(res, dl, dr); })
  DECL_BINOP(sub, false, "sub", _sh_ljs_sub_rjs, { as.fsub(res, dl, dr); })
  DECL_BINOP(div, false, "div", _sh_ljs_div_rjs, { as.fdiv(res, dl, dr); })
  DECL_BINOP(mulN, true, "mulN", _sh_ljs_mul_rjs, { as.fmul(res, dl, dr); })
  DECL_BINOP(addN, true, "addN", _sh_ljs_add_rjs, { as.fadd(res, dl, dr); })
  DECL_BINOP(subN, true, "subN", _sh_ljs_sub_rjs, { as.fsub(res, dl, dr); })
  DECL_BINOP(divN, true, "divN", _sh_ljs_div_rjs, { as.fdiv(res, dl, dr); })
#undef DECL_BINOP

#define DECL_BIT_BINOP(methodName, unsignedRes, commentStr, slowCall, a64body) \
  void methodName(FR rRes, FR rLeft, FR rRight) {                              \
    bitBinOp(                                                                  \
        rRes,                                                                  \
        rLeft,                                                                 \
        rRight,                                                                \
        unsignedRes,                                                           \
        commentStr,                                                            \
        slowCall,                                                              \
        #slowCall,                                                             \
        [](a64::Assembler & a,                                                 \
           const a64::GpX &res,                                                \
           const a64::GpX &dl,                                                 \
           const a64::GpX &dr) a64body);                                       \
  }

  DECL_BIT_BINOP(bitAnd, false, "bit_and", _sh_ljs_bit_and_rjs, {
    a.and_(res, dl, dr);
  })
  DECL_BIT_BINOP(bitOr, false, "bit_or", _sh_ljs_bit_or_rjs, {
    a.orr(res, dl, dr);
  })
  DECL_BIT_BINOP(bitXor, false, "bit_xor", _sh_ljs_bit_xor_rjs, {
    a.eor(res, dl, dr);
  })
  DECL_BIT_BINOP(lShift, false, "lshift", _sh_ljs_left_shift_rjs, {
    a.lsl(res.w(), dl.w(), dr.w());
  })
  DECL_BIT_BINOP(rShift, false, "rshift", _sh_ljs_right_shift_rjs, {
    a.asr(res.w(), dl.w(), dr.w());
  })
  DECL_BIT_BINOP(urShift, true, "rshiftu", _sh_ljs_unsigned_right_shift_rjs, {
    a.lsr(res.w(), dl.w(), dr.w());
  })
  DECL_BIT_BINOP(imul, false, "imul", _sh_ljs_imul_rjs, {
    a.mul(res.w(), dl.w(), dr.w());
  })

#undef DECL_BIT_BINOP

#define DECL_UNOP(methodName, forceNum, commentStr, slowCall, a64body) \
  void methodName(FR rRes, FR rInput) {                                \
    arithUnop(                                                         \
        forceNum,                                                      \
        rRes,                                                          \
        rInput,                                                        \
        commentStr,                                                    \
        [](a64::Assembler & as,                                        \
           const a64::VecD &d,                                         \
           const a64::VecD &s,                                         \
           const a64::VecD &tmp) a64body,                              \
        (void *)slowCall,                                              \
        #slowCall);                                                    \
  }

  DECL_UNOP(dec, false, "dec", _sh_ljs_dec_rjs, {
    as.fmov(tmp, -1.0);
    as.fadd(d, s, tmp);
  })
  DECL_UNOP(inc, false, "inc", _sh_ljs_inc_rjs, {
    as.fmov(tmp, 1.0);
    as.fadd(d, s, tmp);
  })
  DECL_UNOP(negate, false, "neg", _sh_ljs_minus_rjs, { as.fneg(d, s); });

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

#define DECL_COMPARE(                                                   \
    methodName, commentStr, slowCall, condCode, invSlow, passArgsByVal) \
  void methodName(FR rRes, FR rLeft, FR rRight) {                       \
    compareImpl(                                                        \
        rRes,                                                           \
        rLeft,                                                          \
        rRight,                                                         \
        commentStr,                                                     \
        a64::CondCode::condCode,                                        \
        (void *)slowCall,                                               \
        #slowCall,                                                      \
        invSlow,                                                        \
        passArgsByVal);                                                 \
  }
  DECL_COMPARE(greater, "greater", _sh_ljs_greater_rjs, kGT, false, false)
  DECL_COMPARE(
      greaterEqual,
      "greater_equal",
      _sh_ljs_greater_equal_rjs,
      kGE,
      false,
      false)
  DECL_COMPARE(less, "less", _sh_ljs_less_rjs, kMI, false, false)
  DECL_COMPARE(
      lessEqual,
      "less_equal",
      _sh_ljs_less_equal_rjs,
      kLS,
      false,
      false)
  DECL_COMPARE(equal, "Eq", _sh_ljs_equal_rjs, kEQ, false, false)
  DECL_COMPARE(notEqual, "Neq", _sh_ljs_equal_rjs, kNE, true, false)
#undef DECL_COMPARE

  void strictEqual(FR frRes, FR frLeft, FR frRight) {
    strictEqualImpl(false, frRes, frLeft, frRight);
  }
  void strictNotEqual(FR frRes, FR frLeft, FR frRight) {
    strictEqualImpl(true, frRes, frLeft, frRight);
  }

#define DECL_JCOND(                                                      \
    methodName, forceNum, passArgsByVal, commentStr, slowCall, condCode) \
  void methodName(                                                       \
      bool invert, const asmjit::Label &target, FR rLeft, FR rRight) {   \
    jCond(                                                               \
        forceNum,                                                        \
        invert,                                                          \
        passArgsByVal,                                                   \
        target,                                                          \
        rLeft,                                                           \
        rRight,                                                          \
        commentStr,                                                      \
        a64::CondCode::condCode,                                         \
        (void *)slowCall,                                                \
        #slowCall);                                                      \
  }
  DECL_JCOND(jGreater, false, false, "greater", _sh_ljs_greater_rjs, kGT)
  DECL_JCOND(
      jGreaterEqual,
      false,
      false,
      "greater_equal",
      _sh_ljs_greater_equal_rjs,
      kGE)
  DECL_JCOND(jLess, false, false, "less", _sh_ljs_less_rjs, kMI)
  DECL_JCOND(
      jLessEqual,
      false,
      false,
      "less_equal",
      _sh_ljs_less_equal_rjs,
      kLS)
  DECL_JCOND(jLessN, true, false, "less_n", _sh_ljs_less_rjs, kMI)
  DECL_JCOND(
      jLessEqualN,
      true,
      false,
      "less_equal_n",
      _sh_ljs_less_equal_rjs,
      kLS)
  DECL_JCOND(jEqual, false, false, "eq", _sh_ljs_equal_rjs, kEQ)
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

  /// Emit a string switch. The lookup table is identified at runtime by
  /// (\p runtimeModule, \p tableIndex) rather than by baking its address into
  /// the code, since the module's table vector may be reallocated after this
  /// code is compiled (e.g. by lazy compilation).
  ///
  /// \p caseLabels is indexed by the case index the lookup helper returns --
  /// so it has one entry per distinct case string, in the order the runtime
  /// table numbered them, not in bytecode order. Any index with no case in
  /// this body must be filled with \p defaultLabel by the caller.
  void stringSwitchImm(
      FR frInput,
      RuntimeModule *runtimeModule,
      uint32_t tableIndex,
      const asmjit::Label &defaultLabel,
      llvh::ArrayRef<const asmjit::Label *> caseLabels);

  void getByVal(FR frRes, FR frSource, FR frKey);
  void getByIndex(FR frRes, FR frSource, uint32_t key);

#define DECL_PUT_BY_VAL(methodName, commentStr, strict)         \
  void methodName(FR frTarget, FR frKey, FR frValue) {          \
    putByValImpl(frTarget, frKey, frValue, commentStr, strict); \
  }

  DECL_PUT_BY_VAL(putByValLoose, "putByValLoose", false);
  DECL_PUT_BY_VAL(putByValStrict, "putByValStrict", true);

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

#define DECL_GET_ARGUMENTS_PROP_BY_VAL(methodName, commentStr, shFn) \
  void methodName(FR frRes, FR frIndex, FR frLazyReg) {              \
    getArgumentsPropByValImpl(                                       \
        frRes, frIndex, frLazyReg, commentStr, shFn, #shFn);         \
  }

  DECL_GET_ARGUMENTS_PROP_BY_VAL(
      getArgumentsPropByValLoose,
      "GetArgumentsPropByValLoose",
      _sh_ljs_get_arguments_prop_by_val_loose);
  DECL_GET_ARGUMENTS_PROP_BY_VAL(
      getArgumentsPropByValStrict,
      "GetArgumentsPropByValStrict",
      _sh_ljs_get_arguments_prop_by_val_strict);

  void reifyArgumentsLoose(FR frLazyReg) {
    reifyArgumentsImpl(frLazyReg, false, "ReifyArgumentsLoose");
  }
  void reifyArgumentsStrict(FR frLazyReg) {
    reifyArgumentsImpl(frLazyReg, true, "ReifyArgumentsStrict");
  }

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
  /// Uses only xScratch/xScratch2 and never touches the register
  /// allocator, so it is a pure insertion. It clobbers NZCV, so the caller
  /// must have verified that flags are dead at the insertion point. That
  /// is an obligation, not a property emitters have in general: see
  /// selectObject, which holds flags across getOrAllocFRInGpX.
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

  /// \return true if \p ofs encodes as the scaled immediate of an LDR/STR
  /// of a 64-bit register. The immediate is 12 bits scaled by 8, and frame
  /// offsets are always multiples of 8, so only the upper bound can fail.
  static constexpr inline bool isFrameImmOffset(uint32_t ofs) {
    return ofs <= 4095 * 8;
  }

  /// Create an a64::Mem to a specifc frame register.
  static constexpr inline a64::Mem frA64Mem(FR fr) {
    return a64::Mem(xFrame, frByteOffset(fr));
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
  template <typename REG>
  void loadBits64InGp(const REG &dest, uint64_t bits, const char *constName);

  /// Load a SmallHermesValue into a Gp.
  void loadSmallHermesValueInGpX(
      a64::GpX &dest,
      SmallHermesValue shv,
      const char *constName);

  /// Load the StringPrimitive for \p id as a pointer into \p xOut.
  /// The StringPrimitive must already be known to be allocated in the
  /// IdentifierTable at JIT time.
  /// \param xTemp is a temporary register, must not be the same as \p xOut.
  ///   xTemp may not be modified in practice, based on the value of \p id.
  void loadConstStringInGpX(
      SymbolID id,
      const a64::GpX &xOut,
      const a64::GpX &xTemp);

  void _loadFrame(HWReg dest, FR rFrom) {
    uint32_t ofs = frByteOffset(rFrom);
    if (LLVM_LIKELY(isFrameImmOffset(ofs))) {
      if (dest.isGpX())
        a.ldr(dest.a64GpX(), a64::Mem(xFrame, ofs));
      else
        a.ldr(dest.a64VecD(), a64::Mem(xFrame, ofs));
      return;
    }
    a.mov(xScratch, ofs);
    if (dest.isGpX())
      a.ldr(dest.a64GpX(), a64::Mem(xFrame, xScratch));
    else
      a.ldr(dest.a64VecD(), a64::Mem(xFrame, xScratch));
  }
  void _storeFrame(HWReg src, FR rFrom) {
    uint32_t ofs = frByteOffset(rFrom);
    if (LLVM_LIKELY(isFrameImmOffset(ofs))) {
      if (src.isGpX())
        a.str(src.a64GpX(), a64::Mem(xFrame, ofs));
      else
        a.str(src.a64VecD(), a64::Mem(xFrame, ofs));
      return;
    }
    a.mov(xScratch, ofs);
    if (src.isGpX())
      a.str(src.a64GpX(), a64::Mem(xFrame, xScratch));
    else
      a.str(src.a64VecD(), a64::Mem(xFrame, xScratch));
  }

  bool isTempGpX(HWReg hwReg) const {
    assert(hwReg.isGpX());
    unsigned index = hwReg.indexInClass();
    return index >= kGPTemp.first && index <= kGPTemp.second;
  }

  bool isTempVecD(HWReg hwReg) const {
    assert(hwReg.isVecD());
    unsigned index = hwReg.indexInClass();
    return (index >= kVecTemp1.first && index <= kVecTemp1.second) ||
        (index >= kVecTemp2.first && index <= kVecTemp2.second);
  }

  bool isTemp(HWReg hwReg) const {
    return hwReg.isGpX() ? isTempGpX(hwReg) : isTempVecD(hwReg);
  }

  void loadFrameAddr(a64::GpX dst, FR frameReg);
  template <bool use>
  void movHWFromHW(HWReg dst, HWReg src);
  void _storeHWToFrame(FR fr, HWReg src);
  void movHWFromFR(HWReg hwRes, FR src);
  void movHWFromMem(HWReg hwRes, a64::Mem src);

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
    comment("    ; alloc: x%u (temp)", res.indexInClass());
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

  /// Get the current bytecode IP in \p xOut.
  void getBytecodeIP(const a64::GpX &xOut);

 private:
  /// Allocate or return the offset in RO DATA of the current function's debug
  /// name, in the format ID(name).
  int32_t getDebugFunctionName();

  /// \return the stack size subtracted/added to sp when entering/leaving the
  /// function. Includes the two words at the top of the stack for x29 and x30.
  uint32_t getStackSize() const {
    return ((((gpSaveCount_ + 1) & ~1) + ((vecSaveCount_ + 1) & ~1) + 2) * 8) +
        getSavedRegsOffset();
  }

  /// \return the offset of the saved registers in the stack frame.
  uint32_t getSavedRegsOffset() const {
    // If there's exceptions, allocate space for SHJmpBuf and saved SHLocals in
    // the stack.
    return catchTableLabel_.isValid() ? llvh::alignTo(sizeof(SHJmpBuf) + 8, 16)
                                      : 0;
  }

  /// \return the offset of the SHJmpBuf in the stack frame.
  uint32_t getJmpBufOffset() const {
    assert(catchTableLabel_.isValid() && "no SHJmpBuf on stack");
    return 0;
  }

  /// \return the offset of the saved SHLocals pointer in the stack frame.
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

  void frameSetup(
      unsigned numFrameRegs,
      unsigned gpSaveCount,
      unsigned vecSaveCount);

  asmjit::Label newSlowPathLabel() {
    return newPrefLabel("SLOW_", slowPaths_.size());
  }
  asmjit::Label newContLabel() {
    return newPrefLabel("CONT_", slowPaths_.size());
  }

  int32_t reserveData(
      int32_t dsize,
      size_t align,
      asmjit::TypeId typeId,
      int32_t itemCount,
      const char *comment = nullptr);
  /// Register a 64-bit constant in RO DATA and return its offset.
  int32_t uint64Const(uint64_t bits, const char *comment);

  /// Emit a call to \p fn, saving the bytecode IP to Runtime::currentIP
  /// before making the call. This should be used for all calls that may
  /// observe the IP, such as calls that may throw exceptions, or perform
  /// allocations.
  void callRuntimeWithSavedIP(void *fn, const char *name);

  /// Emit a call to \p fn without saving the IP. This should be used only
  /// where saving the IP is unnecessary or incorrect.
  void callRuntime(void *fn, const char *name);

  /// Indirect sibling of callRuntimeWithSavedIP: saves the bytecode IP,
  /// then calls through the mutable function pointer slot at
  /// \p slotAddr rather than a compile-time-constant address. Used for
  /// per-site helper slots (e.g. JitByValSiteRecord::helper) that the
  /// runtime may flip, after emission, from a recording helper to a
  /// plain one; the callee currently stored in the slot is called, and
  /// both possible callees follow the ordinary saved-IP call protocol.
  /// \p name is used only for the disassembly comment.
  void callRuntimeWithSavedIPIndirect(uint64_t slotAddr, const char *name);

  /// Indirect sibling of callRuntime: emits
  /// `mov xScratch, slotAddr; ldr xScratch, [xScratch]; blr xScratch`,
  /// calling whatever function pointer currently lives at \p slotAddr
  /// instead of a compile-time-constant address. Clobbers only
  /// xScratch, exactly as the direct form does. A callee reached this
  /// way may take fewer than the six argument registers the caller sets
  /// up: under AAPCS64, extra argument registers (e.g. x4/x5 for a
  /// four-argument callee) are simply unread, so one call sequence
  /// serves every callee the slot may hold.
  void callRuntimeIndirect(uint64_t slotAddr, const char *name);

  /// Emit the code that runs when this function is longjmped to.
  /// Performs the catch table lookup and jumps to the appropriate catch block,
  /// and if no catch block is found, pops the SHJmpBuf and rethrows the
  /// exception.
  void emitCatchTable(llvh::ArrayRef<const asmjit::Label *> exceptionHandlers);
  void emitSlowPaths();
  void emitROData();

  /// Emit \c emitTypeAssert's check sequence for \p pred against \p xVal,
  /// which holds the current value of \p fr, recording a TypeAssertSite.
  /// The caller emits the dump comment, so that it precedes any load it
  /// had to emit to produce \p xVal.
  void emitTypeAssertGpX(FR fr, const a64::GpX &xVal, TypePred pred);

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

  /// Record that \p fr was written, so that the instruction boundary can
  /// check the value against its global register class. Records nothing
  /// unless the FR owns a global register. Callers must check
  /// emitTypeAsserts_ themselves; this does not.
  void recordFRWriteForAssert(FR fr);

 private:
  /// Set up the call frame and perform the call. The caller should have already
  /// populated the arg count and new target registers.
  /// \param frRes is the frame register that will contain the result.
  /// \param frCallee is a frame register containing the callee.
  void callImpl(FR frRes, FR frCallee);

  void arithUnop(
      bool forceNumber,
      FR frRes,
      FR frInput,
      const char *name,
      void (*fast)(
          a64::Assembler &a,
          const a64::VecD &dst,
          const a64::VecD &src,
          const a64::VecD &tmp),
      void *slowCall,
      const char *slowCallName);

  void arithBinOp(
      bool forceNumber,
      FR frRes,
      FR frLeft,
      FR frRight,
      const char *name,
      void (*fast)(
          a64::Assembler &a,
          const a64::VecD &res,
          const a64::VecD &dl,
          const a64::VecD &dr),
      void *slowCall,
      const char *slowCallName);

  void bitBinOp(
      FR frRes,
      FR frLeft,
      FR frRight,
      bool unsignedRes,
      const char *name,
      SHLegacyValue (*slowCall)(
          SHRuntime *shr,
          const SHLegacyValue *a,
          const SHLegacyValue *b),
      const char *slowCallName,
      void (*fast)(
          a64::Assembler &a,
          const a64::GpX &res,
          const a64::GpX &dl,
          const a64::GpX &dr));

  void compareImpl(
      FR frRes,
      FR frLeft,
      FR frRight,
      const char *name,
      a64::CondCode condCode,
      void *slowCall,
      const char *slowCallName,
      bool invSlow,
      bool passArgsByVal);

  void strictEqualImpl(bool invert, FR frRes, FR frLeft, FR frRight);

  void jCond(
      bool forceNumber,
      bool invert,
      bool passArgsByVal,
      const asmjit::Label &target,
      FR frLeft,
      FR frRight,
      const char *name,
      a64::CondCode condCode,
      void *slowCall,
      const char *slowCallName);

  void putByValImpl(
      FR frTarget,
      FR frKey,
      FR frValue,
      const char *name,
      bool strict);

  /// Split out of getByVal(): the full tier-selection sequence, mirroring
  /// putByValImpl() -- the evidence-driven typed-array tier first (kind
  /// miss chains into the next tier), then the unconditional JSArray
  /// tier, then the indirect recording helper call as the shared slow
  /// path. The result lands in the same register on every path.
  void getByValImpl(FR frRes, FR frSource, FR frKey);

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

#if HERMES_JIT_INLINE_SAFE_STORE
  /// Emit an inline store of the already-encoded slot value in \p shv to the
  /// heap slot whose address is in \p loc, performed only when the Hades
  /// write barrier for that store is provably either a no-op or a single
  /// card-dirty. In every other case -- concurrent marking active, a
  /// compaction in progress, or a segment whose card array is not the inline
  /// one -- nothing is stored and control jumps to \p slowLab, whose code is
  /// expected to perform the store through the runtime, barrier included.
  ///
  /// A heap slot holds a SmallHermesValue, not a HermesValue, and under
  /// HERMESVM_BOXED_DOUBLES the two differ. Callers produce \p shv with
  /// emit_shv_encode_for_slot_or_slow(), which is where the one value this
  /// path cannot store is declined -- a double whose bits do not fit inline
  /// needs a heap-allocated BoxedDouble. That happens BEFORE the caller's
  /// guards, not here, because a value that cannot be encoded is the cheapest
  /// thing to reject and rejecting it early skips the whole guard chain; on
  /// Box2D two thirds of the stores reaching these tiers are exactly that.
  /// In the default heap-value mode the encode emits nothing and \p shv is
  /// \p value.
  ///
  /// Everything here, the card decision included, is phrased in terms of the
  /// ORIGINAL 64-bit \p value rather than \p shv: the two agree on which
  /// values are pointers (the BoxedDouble that would not is already gone),
  /// and a HermesValue carries its pointer uncompressed, which is what the
  /// segment compare needs.
  ///
  /// The emitted predicate mirrors HadesGC::writeBarrier() and
  /// HadesGC::relocationWriteBarrier() exactly:
  ///
  ///     segLoc = loc & ~(kSegmentUnitSize-1)
  ///     if (segLoc == runtime.heap_.youngGen_.lowLim_)  // young target
  ///       *loc = shv; done                              //   no barrier
  ///     if (runtime.heap_.ogMarkingBarriers_)  goto slow  // snapshot barrier
  ///     if (compactee active)                  goto slow  // relocation into
  ///     if (segLoc's size != 1 unit)           goto slow  //   the compactee
  ///     *loc = shv
  ///     if (value is a pointer &&
  ///         (value.ptr & ~(kSegmentUnitSize-1)) == youngGen lowLim)
  ///       segLoc[(loc - segLoc) >> kLogCardSize] = CardStatus::Dirty
  ///
  /// The order matters: the snapshot barrier reads the OLD contents of the
  /// slot, so the marking test has to precede the store. It does, and the
  /// path that stores is precisely the path on which marking is off, so the
  /// requirement holds by construction rather than by convention.
  ///
  /// PRECONDITION on \p loc: it must lie within the first kSegmentUnitSize
  /// bytes of its segment. Everything here derives the segment start as
  /// `loc & ~(kSegmentUnitSize-1)`, which is the true start only under that
  /// bound: a JumboHeapSegment is aligned to kSegmentUnitSize but is N units
  /// long, so for a \p loc further in, that mask yields the start of a later
  /// unit, and the segment-size test below then reads object payload instead
  /// of SHSegmentInfo. Should those bytes happen to hold a 1, the guard
  /// passes and the card-dirty store writes a byte into the cell's own data
  /// while the real, out-of-line card stays clean -- a missed old-to-young
  /// root. Callers are responsible for the bound:
  ///  - PutById: WritePropertyCacheEntry::kMaxSlot is 0xff, which puts \p loc
  ///    at most ~2KB past a cell head, and every cell head lives in the first
  ///    unit of its segment (see the AlignedHeapSegment class comment).
  ///  - Array element stores must gate on the storage cell's size; a large
  ///    array's indexed storage genuinely is a jumbo cell.
  ///
  /// Note the asymmetry that makes this bound load-bearing: the runtime's
  /// large-object barrier derives the segment start from the OWNING CELL
  /// (AlignedHeapSegment::dirtyCardForAddressInLargeObj takes owningObj),
  /// whereas this code derives it from \p loc. The two coincide only while
  /// \p loc is in the same unit as the cell head.
  ///
  /// Given the bound, the segment-size test is what makes the card math
  /// valid: only a segment exactly one unit long keeps its card status array
  /// inline at offset 0, which is what the card-dirty store assumes. A jumbo
  /// segment holds that array out of line, so its stores go to the helper.
  ///
  /// \param loc address of the slot; preserved.
  /// \param shv the encoded slot value to store; read only up to the store.
  ///   It MAY be \p t2 -- and under boxed doubles it is, because the caller
  ///   encoded into t2 before its guards -- which is sound precisely because
  ///   t2 is not touched here until after the store.
  /// \param value the original 64-bit HermesValue, used for the card
  ///   decision; preserved.
  /// \param t1 scratch, clobbered. Must differ from \p loc and \p value.
  /// \param t2 scratch, clobbered after the store. Must differ from \p loc
  ///   and \p value.
  /// \param slowLab where to jump when the store was NOT performed.
  ///
  /// NZCV is clobbered, and so is xScratch: unlike x86-64, which compares
  /// against memory directly, arm64 has to load each of the runtime words it
  /// tests into a register, and neither temporary is free to be it -- \p t1
  /// holds the segment start the card store indexes off, and \p t2 may be
  /// carrying the encoded value. Nothing else is touched: in particular this
  /// emits no call, so no register needs to be synced or freed around it.
  void emitSafeStoreOrSlow(
      const a64::GpX &loc,
      const a64::GpX &shv,
      const a64::GpX &value,
      const a64::GpX &t1,
      const a64::GpX &t2,
      const asmjit::Label &slowLab);

  /// Emit the PutById inline tier: a guard that the target is an object of
  /// the hidden class the write cache recorded at compile time, followed by
  /// an inline store into the cached slot through emitSafeStoreOrSlow().
  /// Falling through means the store is done; every guard that fails jumps
  /// to \p helperLab.
  ///
  /// On entry \p frTarget and \p frValue must already be synced to the frame,
  /// because \p helperLab reads them from there. On return every temp this
  /// used is free again and no FR is registered in one, so the helper call
  /// that follows is safe (see the free-after-call invariant in doc/JIT.md).
  ///
  /// \param clazzID the lazy JIT id of the cached hidden class, non-zero.
  /// \param slot the cached slot index.
  void emitPutByIdInlineTier(
      FR frTarget,
      FR frValue,
      uint16_t clazzID,
      SlotIndex slot,
      const asmjit::Label &helperLab);

  /// Emit the PutByVal inline fast array store: a chain of guards that the
  /// target is a fast JSArray and the key an existing element of it,
  /// followed by an inline store into that element through
  /// emitSafeStoreOrSlow(). Falling through means the store is done; every
  /// guard that fails jumps to \p helperLab.
  ///
  /// The guards replicate, one for one, the fast path
  /// putByValWithReceiver_RJS() takes in StaticH.cpp -- which is itself
  /// JSObject::putComputedWithReceiver_RJS()'s first branch. Anything the
  /// runtime would not have handled there is declined:
  ///   - the target is an object;
  ///   - of CellKind JSArray exactly, which is what pins haveOwnIndexed()
  ///     and setOwnIndexed() to ArrayImpl's implementations (Arguments,
  ///     FastArray, JSTypedArray and friends have their own);
  ///   - hasFastIndexProperties() holds and the class is not frozen, the
  ///     latter because ArrayImpl::_setOwnIndexedImpl() refuses a frozen
  ///     array and freezing does not make the index properties slow;
  ///   - the key is a double that converts to a uint32 and back unchanged
  ///     and is not 0xFFFFFFFF -- exactly toArrayIndexFastPath();
  ///   - `index - beginIndex_ < elemCount_` unsigned, the storage range test
  ///     of _haveOwnIndexedImpl() and of _setOwnIndexedImpl()'s in-range
  ///     branch;
  ///   - the element is not a hole. _haveOwnIndexedImpl() reports false for
  ///     an `empty` element, so a write to a hole is NOT a fast-path write:
  ///     the runtime resolves the property normally and may find a setter on
  ///     the prototype chain.
  /// The receiver-equals-target test of putByValWithReceiver_RJS() needs no
  /// code: PutByVal passes the target as the receiver.
  ///
  /// One further guard has no counterpart in the runtime and exists only for
  /// emitSafeStoreOrSlow()'s precondition: the indexed storage cell's
  /// allocated size must be at most RuntimeOffsets::kMaxInlineStorage, which
  /// is what proves the cell -- and therefore the element address -- lies in
  /// a one-unit FixedSizeHeapSegment. A large array's indexed storage
  /// genuinely is a multi-unit jumbo cell whose card status array is out of
  /// line, and for an element more than one unit into it the predicate's own
  /// segment-size test would read object payload instead of SHSegmentInfo.
  ///
  /// On entry all three operands must already be synced to the frame,
  /// because \p helperLab reads them from there. On return every temp this
  /// used is free again and no FR is registered in one, so the helper call
  /// that follows is safe (see the free-after-call invariant in doc/JIT.md).
  ///
  /// \param targetKnownObject true when an earlier tier at this site has
  ///   already proved the target an object, in which case the object check
  ///   below is skipped. Everything else is still emitted: this tier derives
  ///   its own object pointer and re-reads the cell kind.
  void emitPutByValFastArrayTier(
      FR frTarget,
      FR frKey,
      FR frValue,
      const asmjit::Label &helperLab,
      bool targetKnownObject);
#endif // HERMES_JIT_INLINE_SAFE_STORE

  /// Emit the inline typed-array store tier specialized for exactly \p kind.
  /// A kind mismatch on an object target branches to \p kindMissLab (the
  /// JSArray tier at duo sites, else the helper); every other guard declines
  /// to \p helperLab, whose helper call preserves exact JS semantics and
  /// keeps recording. Emits no write barrier: typed-array storage holds no
  /// GC pointers, so unlike the fast array tier this one is not gated on
  /// HERMES_JIT_INLINE_SAFE_STORE and exists under every GC.
  ///
  /// The guards, in emission order (which is not the spec's listing order --
  /// the object and kind checks come first so that a duo site's non-number
  /// stores reach the JSArray tier rather than the helper):
  ///   - the target is an object of CellKind \p kind exactly;
  ///   - hasFastIndexProperties() holds and the class is not frozen, the
  ///     same masked compare the fast array tier emits: an out-of-range
  ///     defineProperty gives the class index-like properties, and a frozen
  ///     typed array must throw on a strict store rather than be written;
  ///   - the value is a number that the element type can hold without a
  ///     helper -- see the conversion comments at the emission site;
  ///   - the key is a double that converts to a uint32 and back unchanged,
  ///     exactly emit_double_is_uint32() as in the fast array tier;
  ///   - the index is below length_;
  ///   - the buffer is attached, i.e. its data_ is non-null.
  ///
  /// On entry all three operands must already be synced to the frame,
  /// because \p helperLab and \p kindMissLab read them from there. On return
  /// every temp this used is free again and no FR is registered in one, so
  /// the helper call that follows is safe (see the free-after-call invariant
  /// in doc/JIT.md).
  void emitPutByValTypedArrayTier(
      FR frTarget,
      FR frKey,
      FR frValue,
      CellKind kind,
      const asmjit::Label &kindMissLab,
      const asmjit::Label &helperLab);

  /// The register assignment the inline GetByVal tiers run on. It is made
  /// ONCE, by getByValImpl(), and handed to every tier, for two reasons:
  /// the tiers must agree on which register the result lands in (they are
  /// alternatives reached by a chain of kind guards, and the shared slow
  /// path and the frame update that follow them name one register), and
  /// the source and the key are loaded once for the whole chain rather
  /// than reloaded per tier. Contrast the PutByVal tiers, which each own
  /// their prologue: a store has no result register to agree on.
  ///
  /// Every field is a temp the caller has ALREADY released back to the
  /// allocator (see getByValImpl()), so a tier may clobber any of them
  /// freely -- except that \c source and \c key must survive a tier that
  /// declines, because the next tier in the chain reads them, and \c res
  /// must be written LAST, since it may alias \c loc or \c idx.
  struct GetByValRegs {
    /// The source operand, as a raw HermesValue.
    a64::GpX source;
    /// The key operand, as the raw 64 bits of the double it has to be.
    a64::VecD key;
    /// Scratch: the object pointer, then the storage or buffer, then the
    /// element base address.
    a64::GpX loc;
    /// Scratch: the key converted to a uint32 element index, zero-extended
    /// so its W view is directly usable as a UXTW-scaled index.
    a64::GpX idx;
    /// General scratch.
    a64::GpX temp1;
    /// Scratch for the key conversion's round trip.
    a64::VecD keyTmp;
    /// Scratch holding the typed-array element as a double. Invalid unless
    /// a typed-array tier is emitted at this site.
    a64::VecD valTmp;
    /// Where the result HermesValue must land, on every path.
    a64::GpX res;
  };

  /// Emit the GetByVal inline fast array load: a chain of guards that the
  /// source is a fast JSArray and the key an existing, non-hole element of
  /// it, followed by an inline unbox of that element's SmallHermesValue
  /// into \c regs.res. Falling through means the load is done and
  /// \c regs.res holds it; every guard that fails jumps to \p helperLab,
  /// whose helper call resolves the read exactly (the prototype chain may
  /// carry the answer).
  ///
  /// Unconditional: this tier is emitted at every GetByVal site, in every
  /// version, on every build configuration and every GC/heap-value mode --
  /// unlike emitPutByValFastArrayTier() it needs no
  /// HERMES_JIT_INLINE_SAFE_STORE gate, because a load takes no write
  /// barrier. Nothing observes its hits, so it is a static prior rather
  /// than an evidence-driven decision.
  ///
  /// The guards mirror ArrayImpl::at() (JSArray.h) and
  /// tryFastGetComputedNoAlloc()'s JSArray branch (JSObject-inline.h),
  /// reusing emitPutByValFastArrayTier()'s object/kind/key-conversion
  /// sequence VERBATIM but dropping everything that exists only for the
  /// store side:
  ///   - the source is an object (unless \p sourceKnownObject);
  ///   - of CellKind JSArray exactly -- the same narrower-than-ArrayImpl
  ///     guard the put tier uses, which is why an Arguments object (a
  ///     different CellKind that shares ArrayImpl's storage layout)
  ///     declines here instead of being read inline;
  ///   - NO flags_.fastIndexProperties/frozen check: unlike the store side,
  ///     the read side needs none. tryFastGetComputedNoAlloc()'s own
  ///     comment explains why -- "We don't need to check for fast index
  ///     properties here, because if there are non-fast ones, the
  ///     corresponding slot will be empty" -- so the empty check below
  ///     already subsumes it;
  ///   - the key is a double that converts to a uint32 and back unchanged
  ///     and is not 0xFFFFFFFF -- exactly toArrayIndexFastPath(), the put
  ///     tier's own key-conversion sequence, unchanged;
  ///   - `index - beginIndex_ < elemCount_` unsigned, ArrayImpl::at()'s
  ///     range test. Out of range is a DECLINE to \p helperLab, not an
  ///     inline `undefined`: the prototype chain may carry indexed
  ///     properties at that index, so only the full path can answer;
  ///   - the loaded SmallHermesValue element is not `empty` (a hole):
  ///     ArrayImpl::at() returns `empty` for one, and a hole read must
  ///     resolve through the prototype chain (a data property or an
  ///     accessor may live there), so it is also a decline, not
  ///     `undefined`.
  ///
  /// The one guard emitPutByValFastArrayTier() has that this omits besides
  /// the flags check: the "not a jumbo cell" size gate. That guard exists
  /// solely to satisfy emitSafeStoreOrSlow()'s precondition on the write
  /// barrier's card math; a load touches no card and needs no such bound.
  ///
  /// \param sourceKnownObject true when an earlier tier at this site has
  ///   already proved the source an object, in which case the object check
  ///   is skipped. Everything else is still emitted: this tier derives its
  ///   own object pointer and re-reads the cell kind.
  ///
  /// On entry the source and the key must already be synced to the frame,
  /// because \p helperLab reads them from there, and they must be live in
  /// \c regs.source and \c regs.key. This emits no register-allocator
  /// bookkeeping at all -- getByValImpl() has already left the allocator
  /// in the state the shared helper call needs.
  void emitGetByValFastArrayTier(
      const GetByValRegs &regs,
      const asmjit::Label &helperLab,
      bool sourceKnownObject);

  /// Emit the inline typed-array load tier specialized for exactly \p kind,
  /// the load-side sibling of emitPutByValTypedArrayTier(). A kind mismatch
  /// on an object source branches to \p kindMissLab (the JSArray tier,
  /// which is always emitted after this one); a non-object source or a key
  /// that is not an exact uint32 declines to \p helperLab.
  ///
  /// Semantics authority: tryFastGetComputedNoAlloc()'s typed-array branch
  /// (JSObject-inline.h). Two differences from the store tier follow from
  /// it, and both are load-side ONLY:
  ///   - NO object-flags check. The read path tests neither
  ///     fastIndexProperties nor frozen for a typed array: an element read
  ///     is answered by length and attachedness alone, so a tier that
  ///     checked the flags would decline where the interpreter does not.
  ///   - An out-of-bounds index, and a detached buffer, are NOT declines.
  ///     Both read as `undefined`, so both branch to this tier's own inline
  ///     `undefined`, never to the helper -- which also means they record
  ///     nothing, and rightly so: the site's shape at that point IS the
  ///     specialized kind, and there is nothing left to learn.
  ///
  /// The element is loaded, widened to a double per \p kind, and encoded as
  /// a number HermesValue. For the two float kinds the widened value is
  /// NaN-canonicalized first: a raw f32/f64 element may hold ANY NaN bit
  /// pattern, including one that aliases the NaN-box tag space, and
  /// encoding that verbatim would forge a non-number HermesValue. The
  /// integer kinds always produce a finite double and skip it.
  ///
  /// On entry the source and the key must already be synced to the frame,
  /// because \p helperLab reads them from there, and they must be live in
  /// \c regs.source and \c regs.key. \c regs.valTmp must be valid. Like the
  /// fast array tier, this emits no register-allocator bookkeeping.
  void emitGetByValTypedArrayTier(
      const GetByValRegs &regs,
      CellKind kind,
      const asmjit::Label &kindMissLab,
      const asmjit::Label &helperLab);

  /// Emit the per-kind element access of an inline typed-array load: the
  /// sized load of one element of \p kind, its widening to a double, the
  /// NaN canonicalization the two float kinds need, and the encode of the
  /// result as a number HermesValue in \p xRes. Every path ends in a jump
  /// to \p doneLab, so nothing falls out of this.
  ///
  /// \p elemBase is the address of the element to load, in one of the two
  /// natural AArch64 addressing forms: a base register plus a W index
  /// register extended with `uxtw #logWidth` (an ELEMENT index, which the
  /// Mem itself scales -- there is no separate scaled add here), or a base
  /// register plus a scaled unsigned immediate. The caller builds it,
  /// because only the caller knows which form its index is; this asserts
  /// that whichever form arrived is the one \p kind's width can encode.
  ///
  /// This is the tail shared by the typed-array load tiers; everything
  /// before it -- the guards, the bounds check, and the materialization of
  /// \p elemBase -- belongs to the individual tier, as does the
  /// `undefined` block \p doneLab's binding follows.
  ///
  /// Allocates nothing: \p xRes, \p xTemp1 and \p dValTmp are all chosen by
  /// the tier's caller, which must have finished its register-allocator
  /// bookkeeping before any of the chain was emitted.
  void emitTypedArrayElementLoad(
      CellKind kind,
      const a64::Mem &elemBase,
      const a64::GpX &xRes,
      const a64::GpX &xTemp1,
      const a64::VecD &dValTmp,
      const asmjit::Label &doneLab);

  /// The register assignment the inline GetByIndex tiers run on -- the
  /// GetByValRegs of the constant-key opcode. It is made ONCE, by
  /// getByIndexImpl(), and handed to every tier, for the same two reasons
  /// GetByValRegs exists: the tiers must agree on which register the
  /// result lands in, and the source is loaded once for the whole chain.
  /// There is no \c key/\c keyTmp pair: the key is an emit-time uint8
  /// constant, never a frame value, so there is nothing to load or
  /// convert. The typed-array tier reuses \c loc/\c temp1/\c res exactly
  /// as GetByValRegs's typed-array tier does, plus its own \c valTmp; it
  /// has no use for \c idx at all, since the constant element index is
  /// folded into the element access's displacement.
  ///
  /// Every field is a temp the caller has ALREADY released back to the
  /// allocator (see getByIndexImpl()), so a tier may clobber any of them
  /// freely -- except that \c source must survive a tier that declines,
  /// because the next tier in the chain reads it, and \c res must be
  /// written LAST, since it may alias \c loc or \c idx.
  struct GetByIndexRegs {
    /// The source operand, as a raw HermesValue.
    a64::GpX source;
    /// Scratch: the object pointer, then the storage or buffer, then the
    /// element base address.
    a64::GpX loc;
    /// Scratch: the constant key K, then K's storage-relative index
    /// (K - beginIndex_), zero-extended so its X view is directly usable
    /// as a scaled 64-bit index.
    a64::GpX idx;
    /// General scratch.
    a64::GpX temp1;
    /// Scratch holding the typed-array element as a double. Invalid unless
    /// a typed-array tier is emitted at this site.
    a64::VecD valTmp;
    /// Where the result HermesValue must land, on every path.
    a64::GpX res;
  };

  /// Emit the GetByIndex inline fast array load: the constant-key twin of
  /// emitGetByValFastArrayTier(), reusing its object/kind guard and its
  /// hole-check/unbox tail verbatim but replacing the key-conversion block
  /// with nothing (K is an emit-time constant) and its range test with the
  /// constant-K begin-relative form. Falling through means the load is
  /// done and \c regs.res holds it; every guard that fails jumps to
  /// \p helperLab, whose helper call resolves the read exactly (the
  /// prototype chain may carry the answer).
  ///
  /// Unconditional, for the same reason as emitGetByValFastArrayTier(): a
  /// load takes no write barrier, so this needs no
  /// HERMES_JIT_INLINE_SAFE_STORE gate and is emitted at every GetByIndex
  /// site, in every version, under every GC/heap-value mode.
  ///
  /// The guards mirror ArrayImpl::at() (JSArray.h) and
  /// tryFastGetComputedNoAlloc()'s JSArray branch (JSObject-inline.h),
  /// exactly as emitGetByValFastArrayTier()'s doc comment describes, with
  /// one storage-layout correction the constant key does NOT remove: K
  /// being constant does not make the storage offset constant, because the
  /// stored fields are \c beginIndex_ and \c elemCount_ (JSArray.h) and a
  /// first indexed write can set \c beginIndex_ to any index
  /// (JSArray.cpp). The range test is therefore still
  /// `K - beginIndex_ < elemCount_` unsigned -- one comparison that covers
  /// both `K < beginIndex_` (via unsigned wrap) and
  /// `K >= beginIndex_ + elemCount_` -- emitted as
  /// `mov wIdx, #K`, `ldr wTemp1, [loc, #beginIndex_]`,
  /// `sub wIdx, wIdx, wTemp1`, `ldr wTemp1, [loc, #elemCount_]`,
  /// `cmp wIdx, wTemp1`, `b.hs helperLab`. Out of range is a DECLINE, not
  /// `undefined`, and so is a hole (an `empty` slot), exactly as on the
  /// ByVal side.
  ///
  /// \param sourceKnownObject true when an earlier tier at this site -- a
  ///   typed-array tier, at a site that earned one -- has already proved
  ///   the source an object, in which case the object check is skipped.
  ///
  /// On entry the source must already be synced to the frame, because
  /// \p helperLab reads it from there, and it must be live in
  /// \c regs.source. This emits no register-allocator bookkeeping at all
  /// -- getByIndexImpl() has already left the allocator in the state the
  /// shared helper call needs.
  void emitGetByIndexFastArrayTier(
      const GetByIndexRegs &regs,
      uint32_t key,
      const asmjit::Label &helperLab,
      bool sourceKnownObject);

  /// Emit the inline typed-array load tier of GetByIndex, specialized for
  /// exactly \p kind at the emit-time constant index \p key -- the
  /// constant-key twin of emitGetByValTypedArrayTier(). A kind mismatch on
  /// an object source branches to \p kindMissLab (the JSArray tier, which
  /// is always emitted after this one); a non-object source declines to
  /// \p helperLab. There is nothing else left to decline: with the key a
  /// constant there is no key conversion and so no parity exit.
  ///
  /// Semantics authority, and the two load-side-only differences from the
  /// store tier (no object-flags check; an out-of-bounds index and a
  /// detached buffer are inline `undefined` rather than declines, and
  /// therefore record nothing), are exactly as
  /// emitGetByValTypedArrayTier()'s doc comment describes.
  ///
  /// Two things differ from that tier, both consequences of the constant
  /// key:
  ///   - the bounds test compares the length field against Imm(K)
  ///     DIRECTLY -- `ldr wTemp1, [loc, #length_]`, `cmp wTemp1, #K`,
  ///     `b.ls undefined` -- the reverse operand order of the ByVal tier's
  ///     `cmp idx, length_` / `b.hs`, and therefore the reverse condition.
  ///     Both accept STRICTLY: `b.ls` is taken when `length_ <= K`, i.e.
  ///     exactly when K is NOT a valid index. K is a uint8, so it is
  ///     always an `cmp` immediate;
  ///   - the shared element-load tail is handed the IMMEDIATE addressing
  ///     form -- the `data_ + offset_` base register plus a
  ///     `K * elementWidth` scaled unsigned displacement, and NO index
  ///     register -- rather than the UXTW base+index form. The product is
  ///     at most 255 * 8 = 2040, which is inside every width's scaled
  ///     12-bit range.
  ///
  /// On entry the source must already be synced to the frame, because
  /// \p helperLab reads it from there, and it must be live in
  /// \c regs.source. \c regs.valTmp must be valid. Like the fast array
  /// tier, this emits no register-allocator bookkeeping.
  void emitGetByIndexTypedArrayTier(
      const GetByIndexRegs &regs,
      uint32_t key,
      CellKind kind,
      const asmjit::Label &kindMissLab,
      const asmjit::Label &helperLab);

  /// Split out of getByIndex(): the tier-selection sequence for the
  /// constant-key twin of getByValImpl(), and the same shape -- the
  /// evidence-driven typed-array tier first (its kind miss chaining into
  /// the JSArray tier, never into the helper), the unconditional JSArray
  /// tier second, and the per-site INDIRECT recording call to
  /// _jit_get_by_index as the shared slow path. Registers, including the
  /// result register, are allocated ONCE here and handed to the tiers via
  /// GetByIndexRegs, never allocated by a tier itself.
  ///
  /// Tier selection reads the PRIOR version's record for this site, with
  /// the LOAD predicate, exactly as getByValImpl() does; GetByIndex sites
  /// join the same JitConsumerRecords::byValSites deque (siteId is the
  /// bytecode offset, and GetByIndex is a distinct opcode, so no
  /// collision with a ByVal site is possible).
  void getByIndexImpl(FR frRes, FR frSource, uint32_t key);

  void getArgumentsPropByValImpl(
      FR frRes,
      FR frIndex,
      FR frLazyReg,
      const char *name,
      SHLegacyValue (*shImpl)(
          SHRuntime *shr,
          SHLegacyValue *frame,
          SHLegacyValue *idx,
          SHLegacyValue *lazyReg),
      const char *shImplName);

  void reifyArgumentsImpl(FR frLazyReg, bool strict, const char *name);

  void throwIfEmptyUndefinedImpl(FR frRes, FR frInput, bool empty);

  /// Bump allocate \p sz bytes on the GC heap and store the result in \p xOut.
  /// If not possible, jump to the \p slowPathLab.
  /// \param sz is the aligned number of bytes to bump the pointer by.
  /// \param xOut is the register to store the address of the new object.
  /// \param xTemp1 is a temporary register.
  /// \param xTemp2 is a temporary register.
  /// \param slowPathLab is the label to jump to if the allocation fails.
  void bumpAllocAndUnpoison(
      uint32_t sz,
      const a64::GpX &xOut,
      const a64::GpX &xTemp1,
      const a64::GpX &xTemp2,
      const asmjit::Label &slowPathLab);

  /// Initialize a GCCell at the pointer given.
  /// \param kind the CellKind to populate.
  /// \param sz the aligned total size of the cell.
  /// \param xCell pointer to the start of the cell.
  /// \param xTemp1 is a temporary, must not be the same as xCell.
  void initGCCell(
      CellKind kind,
      uint32_t sz,
      const a64::GpX &xCell,
      const a64::GpX &xTemp1);

  /// Emit the code to perform an allocation in the young generation, populating
  /// the fields of the new GCCell.
  /// \param kind is the CellKind of object to allocate.
  /// \param sz is the size of the object to allocate.
  /// \param xOut is the register to store the address of the new object.
  /// \param xTemp1 is a temporary register.
  /// \param xTemp2 is a temporary register.
  /// \param slowPathLab is the label to jump to if the allocation fails.
  void allocInYoung(
      CellKind kind,
      uint32_t sz,
      const a64::GpX &xOut,
      const a64::GpX &xTemp1,
      const a64::GpX &xTemp2,
      const asmjit::Label &slowPathLab);

  /// Emit the code to perform an allocation in the young generation, populating
  /// the fields of the new GCCell.
  /// \param kind1 is the CellKind of object 1 to allocate.
  /// \param sz1 is the size of the object 1 to allocate.
  /// \param kind2 is the CellKind of object 2 to allocate.
  /// \param sz2 is the size of the object 2 to allocate.
  /// \param xOut1 is the register to store the address of the first object.
  /// \param xOut2 is the register to store the address of the second object.
  /// \param xTemp is a temporary register.
  /// \param slowPathLab is the label to jump to if the allocation fails.
  void alloc2InYoung(
      CellKind kind1,
      uint32_t sz1,
      CellKind kind2,
      uint32_t sz2,
      const a64::GpX &xOut1,
      const a64::GpX &xOut2,
      const a64::GpX &xTemp,
      const asmjit::Label &slowPathLab);

  /// Slow version of newObjectWithBuffer.
  /// Used temporarily while full functionality is being added to
  /// newObjectWithBuffer.
  void newObjectWithBufferSlow(
      FR frRes,
      uint32_t shapeTableIndex,
      uint32_t valBufferOffset);

  /// If counters are enabled, emit code to increment \p counter.
  void emitIncrementCounter(JitCounter counter);

  /// Initialize or obtain an existing lazy JIT ID for the given hidden class.
  /// NOTE: this call performs GC allocations, to the HC might move. The raw
  /// pointer MUST NOT be used after this call.
  /// \return 0 if too many IDs have been assigned.
  uint16_t initHCLazyIDMayAlloc(HiddenClass *hc);
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

/// Return true if the specified 64-bit value can be efficiently loaded on
/// Arm64 with up to two integer instructions. In other words, it has at most
/// two non-zero 16-bit words.
inline bool isCheapConst(uint64_t k) {
  unsigned count = 0;
  for (uint64_t mask = 0xFFFF; mask != 0; mask <<= 16) {
    if (k & mask)
      ++count;
  }
  return count <= 2;
}

template <bool use>
void Emitter::movHWFromHW(HWReg dst, HWReg src) {
  if (dst != src) {
    if (dst.isVecD() && src.isVecD())
      a.fmov(dst.a64VecD(), src.a64VecD());
    else if (dst.isVecD())
      a.fmov(dst.a64VecD(), src.a64GpX());
    else if (src.isVecD())
      a.fmov(dst.a64GpX(), src.a64VecD());
    else
      a.mov(dst.a64GpX(), src.a64GpX());
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

template <typename REG>
void Emitter::loadBits64InGp(
    const REG &dest,
    uint64_t bits,
    const char *constName) {
  if (isCheapConst(bits)) {
    a.mov(dest, bits);
  } else {
    a.ldr(dest, a64::Mem(roDataLabel_, uint64Const(bits, constName)));
  }
}

} // namespace hermes::vm::arm64
