/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_INTERPRETER_INTERNAL_H
#define HERMES_VM_INTERPRETER_INTERNAL_H

#include "hermes/VM/Interpreter.h"

// Convenient aliases for operand registers.
#if !defined(__arm__) || !defined(__clang__) || \
    !defined(HERMESVM_ALLOW_INLINE_ASM)
#define REG(index) frameRegs[index]
#else
// Work around bad codegen from clang on armv7 that causes it to emit two
// separate loads instead of a single ldrd.
#define REG(index)                                                           \
  (*({                                                                       \
    PinnedHermesValue *addr;                                                 \
    static_assert(sizeof(PinnedHermesValue) == 8, "asm depends on HV size"); \
    static_assert(sizeof(index) <= 4, "index must fit in a register");       \
    asm("add.w   %0, %1, %2, lsl #3"                                         \
        : "=r"(addr)                                                         \
        : "r"(frameRegs), "r"(index));                                       \
    addr;                                                                    \
  }))
#endif
#define O1REG(name) REG(ip->i##name.op1)
#define O2REG(name) REG(ip->i##name.op2)
#define O3REG(name) REG(ip->i##name.op3)
#define O4REG(name) REG(ip->i##name.op4)
#define O5REG(name) REG(ip->i##name.op5)
#define O6REG(name) REG(ip->i##name.op6)

/// Get a StackFramePtr from the current frameRegs.
#define FRAME StackFramePtr(frameRegs - StackFrameLayout::FirstLocal)

/// Calculate the default property access flags depending on the mode of the
/// \c CodeBlock.
#define DEFAULT_PROP_OP_FLAGS(strictMode) \
  (strictMode ? PropOpFlags().plusThrowOnError() : PropOpFlags())

/// Map from a string ID encoded in the operand to a SymbolID.
/// This string ID must be used explicitly as identifier.
#define ID(stringID) \
  (curCodeBlock->getRuntimeModule()->getSymbolIDMustExist(stringID))

// Add an arbitrary byte offset to ip.
#define IPADD(val) ((const Inst *)((const uint8_t *)ip + (val)))

// Get the current bytecode offset.
#define CUROFFSET ((const uint8_t *)ip - (const uint8_t *)curCodeBlock->begin())

// Calculate the address of the next instruction given the name of the current
// one.
#define NEXTINST(name) ((const Inst *)(&ip->i##name + 1))

namespace hermes {
namespace vm {

/// \return the quotient of x divided by y.
double doDiv(double x, double y) LLVM_NO_SANITIZE("float-divide-by-zero");
inline double doDiv(double x, double y) {
  // UBSan will complain about float divide by zero as our implementation
  // of OpCode::Div depends on IEEE 754 float divide by zero. All modern
  // compilers implement this and there is no trivial work-around without
  // sacrificing performance and readability.
  return x / y;
}

inline double doMod(double x, double y) {
  // We use fmod here for simplicity. Theoretically fmod behaves slightly
  // differently than the ECMAScript Spec. fmod applies round-towards-zero for
  // the remainder when it's not representable by a double; while the spec
  // requires round-to-nearest. As an example, 5 % 0.7 will give
  // 0.10000000000000031 using fmod, but using the rounding style described by
  // the spec, the output should really be 0.10000000000000053. Such difference
  // can be ignored in practice.
  return std::fmod(x, y);
}

/// \return the product of x multiplied by y.
inline double doMul(double x, double y) {
  return x * y;
}

/// \return the difference of y subtracted from x.
inline double doSub(double x, double y) {
  return x - y;
}

inline int32_t doBitAnd(int32_t x, int32_t y) {
  return x & y;
}

inline int32_t doBitOr(int32_t x, int32_t y) {
  return x | y;
}

inline int32_t doBitXor(int32_t x, int32_t y) {
  return x ^ y;
}

/// \return Math.imul(x, y): the 32-bit integer product of x and y, computed
/// by multiplying the operands as unsigned 32-bit values (to avoid signed
/// overflow UB) and reinterpreting the result as signed.
inline int32_t doImul(int32_t x, int32_t y) {
  uint32_t product = static_cast<uint32_t>(x) * static_cast<uint32_t>(y);
  return static_cast<int32_t>(product);
}

inline int32_t doLShift(uint32_t x, uint32_t y) {
  return x << y;
}

inline int32_t doRShift(int32_t x, uint32_t y) {
  return x >> y;
}

inline uint32_t doURshift(uint32_t x, uint32_t y) {
  return x >> y;
}

inline double doInc(double d) {
  return d + 1;
}

inline double doDec(double d) {
  return d - 1;
}

template <auto Oper, typename InstType>
ExecutionStatus doOperSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const InstType *inst);

template <auto Oper, typename InstType>
ExecutionStatus doBitOperSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const InstType *inst);

template <auto Oper, typename InstType>
ExecutionStatus doShiftOperSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const InstType *inst);

template <auto Oper, typename InstType>
ExecutionStatus doIncDecOperSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const InstType *inst);

ExecutionStatus doBitNotSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip);

ExecutionStatus doNegateSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip);

/// Slow path for Imul: converts both operands with ToInt32 (raising a
/// TypeError if either is a BigInt, since Math.imul is not BigInt-aware)
/// and multiplies them with doImul.
ExecutionStatus doImulSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip);

ExecutionStatus doCallRequireSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip,
    RuntimeModule *runtimeModule);

ExecutionStatus doGetByIdSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip,
    CodeBlock *curCodeBlock,
    uint32_t idVal,
    bool tryProp);

ExecutionStatus doGetByIdWithReceiverSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip,
    CodeBlock *curCodeBlock);

ExecutionStatus doPutByIdSlowPath_RJS(
    Runtime &runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip,
    CodeBlock *curCodeBlock,
    uint32_t idVal,
    bool strictMode,
    bool tryProp);

/// Handle a StringSwitchImm instruction (which must be the kind of \p ip).
/// Expects the Interpreter's current CodeBlock and frameRegisters in the first
/// two arguments.  Returns the address of the next instruction to execute.
const Inst *doStringSwitchImm(
    CodeBlock *curCodeBlock,
    PinnedHermesValue *frameRegs,
    const Inst *ip);

} // namespace vm
} // namespace hermes
#endif // HERMES_VM_INTERPRETER_INTERNAL_H
