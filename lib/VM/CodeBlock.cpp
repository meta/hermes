/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "codeblock"

#include "hermes/VM/CodeBlock.h"

#include "hermes/BCGen/HBC/Bytecode.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/Support/Conversions.h"
#include "hermes/Support/PerfSection.h"
#include "hermes/Support/SimpleDiagHandler.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/JIT/JitFunctionData.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/RuntimeModule.h"

#include "llvh/Support/Debug.h"
#include "llvh/Support/ErrorHandling.h"

namespace hermes {
namespace vm {

using namespace hermes::inst;

CodeBlock::CodeBlock(
    RuntimeModule *runtimeModule,
    hbc::RuntimeFunctionHeader header,
    const uint8_t *bytecode,
    uint32_t functionID,
    uint32_t readCacheSize,
    uint32_t writeCacheSize,
    uint32_t privateNameCacheSize)
    : runtimeModule_(runtimeModule),
      functionHeader_(header),
      bytecode_(bytecode),
      functionID_(functionID),
      readPropertyCacheSize_(readCacheSize),
      writePropertyCacheSize_(writeCacheSize),
      privateNameCacheSize_(privateNameCacheSize) {
  std::uninitialized_fill_n(
      readPropertyCache(), readCacheSize, ReadPropertyCacheEntry{});
  std::uninitialized_fill_n(
      writePropertyCache(), writeCacheSize, WritePropertyCacheEntry{});
  std::uninitialized_fill_n(
      privateNameCache(), privateNameCacheSize, PrivateNameCacheEntry{});
}

CodeBlock::~CodeBlock() = default;

#if HERMESVM_JIT
JitFunctionData *CodeBlock::ensureJitData(uint8_t budget) {
  if (!jitData_)
    jitData_.reset(new JitFunctionData(budget));
  return jitData_.get();
}
#endif

#ifdef HERMES_SLOW_DEBUG

static void validateInstructions(ArrayRef<uint8_t> list, unsigned frameSize) {
  const OperandAddr32 listSize = (OperandAddr32)list.size();
  assert((size_t)listSize == list.size() && "more than 2GB instructions!");

  auto validateUInt8 = [](...) {};
  auto validateUInt16 = [](...) {};
  auto validateUInt32 = [](...) {};
  auto validateImm32 = [](...) {};
  auto validateDouble = [](...) {};
  auto validateReg8 = [&](OperandAddr32, OperandReg8 reg8) {
    assert(reg8 < frameSize && "invalid register index");
  };
  auto validateReg32 = [&](OperandAddr32, OperandReg32 reg32) {
    assert(reg32 < frameSize && "invalid register index");
  };
  auto validateAddr32 = [&](OperandAddr32 ip, OperandAddr32 offset) {
    // Check the offset while avoiding overflow.
    assert(
        (offset < 0 ? ip + offset >= 0 : offset < listSize - ip) &&
        "invalid jmp offset");
  };
  auto validateAddr8 = [&](OperandAddr32 ip, OperandAddr8 offset) {
    validateAddr32(ip, offset);
  };

  for (OperandAddr32 ip = 0; ip != listSize;) {
    assert(ip < listSize);
    auto *inst = reinterpret_cast<const Inst *>(&list[ip]);
    switch (inst->opCode) {
#define DEFINE_OPCODE_0(name)    \
  case OpCode::name:             \
    ip += sizeof(inst->i##name); \
    break;
#define DEFINE_OPCODE_1(name, op1type)        \
  case OpCode::name:                          \
    validate##op1type(ip, inst->i##name.op1); \
    ip += sizeof(inst->i##name);              \
    break;
#define DEFINE_OPCODE_2(name, op1type, op2type) \
  case OpCode::name:                            \
    validate##op1type(ip, inst->i##name.op1);   \
    validate##op2type(ip, inst->i##name.op2);   \
    ip += sizeof(inst->i##name);                \
    break;
#define DEFINE_OPCODE_3(name, op1type, op2type, op3type) \
  case OpCode::name:                                     \
    validate##op1type(ip, inst->i##name.op1);            \
    validate##op2type(ip, inst->i##name.op2);            \
    validate##op3type(ip, inst->i##name.op3);            \
    ip += sizeof(inst->i##name);                         \
    break;
#define DEFINE_OPCODE_4(name, op1type, op2type, op3type, op4type) \
  case OpCode::name:                                              \
    validate##op1type(ip, inst->i##name.op1);                     \
    validate##op2type(ip, inst->i##name.op2);                     \
    validate##op3type(ip, inst->i##name.op3);                     \
    validate##op4type(ip, inst->i##name.op4);                     \
    ip += sizeof(inst->i##name);                                  \
    break;
#define DEFINE_OPCODE_5(name, op1type, op2type, op3type, op4type, op5type) \
  case OpCode::name:                                                       \
    validate##op1type(ip, inst->i##name.op1);                              \
    validate##op2type(ip, inst->i##name.op2);                              \
    validate##op3type(ip, inst->i##name.op3);                              \
    validate##op4type(ip, inst->i##name.op4);                              \
    validate##op5type(ip, inst->i##name.op5);                              \
    ip += sizeof(inst->i##name);                                           \
    break;
#define DEFINE_OPCODE_6(                                        \
    name, op1type, op2type, op3type, op4type, op5type, op6type) \
  case OpCode::name:                                            \
    validate##op1type(ip, inst->i##name.op1);                   \
    validate##op2type(ip, inst->i##name.op2);                   \
    validate##op3type(ip, inst->i##name.op3);                   \
    validate##op4type(ip, inst->i##name.op4);                   \
    validate##op5type(ip, inst->i##name.op5);                   \
    validate##op6type(ip, inst->i##name.op6);                   \
    ip += sizeof(inst->i##name);                                \
    break;

#include "hermes/BCGen/HBC/BytecodeList.def"
      default:
        llvm_unreachable("invalid opcode");
    }
  }
}

#endif

std::unique_ptr<CodeBlock> CodeBlock::createCodeBlock(
    RuntimeModule *runtimeModule,
    hbc::RuntimeFunctionHeader header,
    const uint8_t *bytecode,
    uint32_t functionID) {
#ifdef HERMES_SLOW_DEBUG
  if (bytecode)
    validateInstructions(
        {bytecode, header.getBytecodeSizeInBytes()}, header.getFrameSize());
#endif

  // Compute size needed for caches.  The bytecode instructions have
  // one byte for cache indices.  If the number of cache entries
  // needed in a function reaches 256, we reserve index 255 to mean
  // "overflow", don't cache -- the cache size won't grow beyond 256.
  // But we only have one byte in the function header for the size.
  // So we encode 256 as 255: a size of 255 could mean that there are
  // actually 255 elements, or that there are 256.  Here we make sure
  // that the cache is big enough in that case, by conservatively
  // adding one element when the size is 255.
  auto sizeComputer = [](uint8_t size) -> uint32_t {
    static_assert(hbc::PROPERTY_CACHING_DISABLED == 255);
    return size == hbc::PROPERTY_CACHING_DISABLED ? 256 : size;
  };

  uint32_t readCacheSize = sizeComputer(header.getReadCacheSize());
  uint32_t writeCacheSize = sizeComputer(header.getWriteCacheSize());
  uint32_t privateNameCacheSize =
      sizeComputer(header.getPrivateNameCacheSize());

  bool isCodeBlockLazy = !bytecode;
  if (isCodeBlockLazy) {
    readCacheSize = sizeComputer(std::numeric_limits<uint8_t>::max());
    writeCacheSize = sizeComputer(std::numeric_limits<uint8_t>::max());
    privateNameCacheSize = sizeComputer(std::numeric_limits<uint8_t>::max());
  }

  auto allocSize = totalSizeToAlloc<
      ReadPropertyCacheEntry,
      WritePropertyCacheEntry,
      PrivateNameCacheEntry>(
      readCacheSize, writeCacheSize, privateNameCacheSize);
  void *mem = checkedMalloc(allocSize);
  return std::unique_ptr<CodeBlock>(new (mem) CodeBlock(
      runtimeModule,
      header,
      bytecode,
      functionID,
      readCacheSize,
      writeCacheSize,
      privateNameCacheSize));
}

int32_t CodeBlock::findCatchTargetOffset(uint32_t exceptionOffset) {
  return runtimeModule_->getBytecode()->findCatchTargetOffset(
      functionID_, exceptionOffset);
}

SymbolID CodeBlock::getNameMayAllocate() const {
  return runtimeModule_->getSymbolIDFromStringIDMayAllocate(
      functionHeader_.getFunctionName());
}

std::string CodeBlock::getNameString() const {
  return runtimeModule_->getStringFromStringID(
      functionHeader_.getFunctionName());
}

OptValue<uint32_t> CodeBlock::getDebugSourceLocationsOffset() const {
  auto *debugOffsets =
      runtimeModule_->getBytecode()->getDebugOffsets(functionID_);
  if (!debugOffsets)
    return llvh::None;
  uint32_t ret = debugOffsets->sourceLocations;
  if (ret == hbc::DebugOffsets::NO_OFFSET)
    return llvh::None;
  return ret;
}

OptValue<hbc::DebugSourceLocation> CodeBlock::getSourceLocation(
    uint32_t offset) const {
  auto debugLocsOffset = getDebugSourceLocationsOffset();
  if (!debugLocsOffset) {
    return llvh::None;
  }

  return getRuntimeModule()
      ->getBytecode()
      ->getDebugInfo()
      ->getLocationForAddress(*debugLocsOffset, offset);
}

OptValue<hbc::DebugSourceLocation> CodeBlock::getSourceLocationForFunction()
    const {
  auto debugLocsOffset = getDebugSourceLocationsOffset();
  if (!debugLocsOffset) {
    return llvh::None;
  }

  return getRuntimeModule()
      ->getBytecode()
      ->getDebugInfo()
      ->getLocationForFunction(*debugLocsOffset);
}

ExecutionStatus CodeBlock::compileLazyFunction(Runtime &runtime) {
  assert(isLazy() && "Laziness has not been checked");
  auto *provider = runtimeModule_->getBytecode();

  bool success;
  llvh::StringRef errMsg;
  executeInStack(
      runtime.getStackExecutor(), [&success, &errMsg, &provider, this]() {
        std::tie(success, errMsg) =
            hbc::compileLazyFunction(provider, functionID_);
      });
  if (!success) {
    // Raise a SyntaxError to be consistent with eval().
    return runtime.raiseSyntaxError(llvh::StringRef{errMsg});
  }

  functionHeader_ =
      runtimeModule_->getBytecode()->getFunctionHeader(functionID_);
  bytecode_ = runtimeModule_->getBytecode()->getBytecode(functionID_);
  runtimeModule_->initAfterLazyCompilation();

  return ExecutionStatus::RETURNED;
}

bool CodeBlock::coordsInLazyFunction(SMLoc loc, OptValue<SMLoc> end) const {
  assert(isLazy() && "function is not lazy");

  return hbc::coordsInLazyFunction(
      runtimeModule_->getBytecode(), functionID_, loc, end);
}

std::vector<uint32_t> CodeBlock::getVariableCounts(
    uint32_t lexicalScopeIdxInParentFunction) const {
  return hbc::getVariableCounts(
      runtimeModule_->getBytecode(),
      functionID_,
      lexicalScopeIdxInParentFunction);
}

hbc::VariableInfoAtDepth CodeBlock::getVariableInfoAtDepth(
    uint32_t depth,
    uint32_t variableIndex,
    uint32_t lexicalScopeIdxInParentFunction) const {
  return hbc::getVariableInfoAtDepth(
      runtimeModule_->getBytecode(),
      functionID_,
      depth,
      variableIndex,
      lexicalScopeIdxInParentFunction);
}

OptValue<uint32_t> CodeBlock::getFunctionSourceID() const {
  // Note that for the case of lazy compilation, the function sources had been
  // reserved into the function source table of the root bytecode module.
  // For non-lazy module, the lazy root module is itself.
  llvh::ArrayRef<std::pair<uint32_t, uint32_t>> table =
      runtimeModule_->getBytecode()->getFunctionSourceTable();

  // Performs a binary search since the function source table is sorted by the
  // 1st value. We could further optimize the lookup by loading it as a map in
  // the RuntimeModule, but the table is expected to be small.
  auto it = std::lower_bound(
      table.begin(),
      table.end(),
      functionID_,
      [](std::pair<uint32_t, uint32_t> entry, uint32_t id) {
        return entry.first < id;
      });
  if (it == table.end() || it->first != functionID_) {
    return llvh::None;
  } else {
    return it->second;
  }
}

void CodeBlock::markWeakElementsInCaches(
    Runtime &runtime,
    WeakRootAcceptor &acceptor) {
  for (auto &prop :
       llvh::makeMutableArrayRef(readPropertyCache(), readPropertyCacheSize_)) {
    acceptor.acceptWeak(prop.clazz);
    acceptor.acceptWeak(prop.negMatchClazz);
  }
  for (auto &prop : llvh::makeMutableArrayRef(
           writePropertyCache(), writePropertyCacheSize_)) {
    acceptor.acceptWeak(prop.clazz);
  }
  for (auto &prop :
       llvh::makeMutableArrayRef(privateNameCache(), privateNameCacheSize_)) {
    acceptor.acceptWeak(prop.clazz);
    acceptor.acceptWeakSym(prop.nameVal);
  }
}

uint32_t CodeBlock::getVirtualOffset() const {
  return getRuntimeModule()->getBytecode()->getVirtualOffsetForFunction(
      functionID_);
}

#ifdef HERMES_ENABLE_DEBUGGER

/// Makes the page that \p address is in writable.
/// If it fails, aborts execution.
static void makeWritable(void *address, size_t length) {
  void *endAddress = static_cast<void *>(static_cast<char *>(address) + length);

  // Align the address to page size before setting the pagesize.
  void *alignedAddress = reinterpret_cast<void *>(llvh::alignDown(
      reinterpret_cast<uintptr_t>(address), hermes::oscompat::page_size()));

  size_t totalLength =
      static_cast<char *>(endAddress) - static_cast<char *>(alignedAddress);

  bool success = oscompat::vm_protect(
      alignedAddress, totalLength, oscompat::ProtectMode::ReadWrite);
  if (!success) {
    hermes_fatal("mprotect failed before modifying breakpoint");
  }
}

void CodeBlock::installBreakpointAtOffset(uint32_t offset) {
  auto opcodes = getOpcodeArray();
  assert(offset < opcodes.size() && "patch offset out of bounds");
  hbc::opcode_atom_t *address =
      const_cast<hbc::opcode_atom_t *>(opcodes.begin() + offset);
  hbc::opcode_atom_t debuggerOpcode =
      static_cast<hbc::opcode_atom_t>(OpCode::Debugger);

  static_assert(
      sizeof(inst::DebuggerInst) == 1,
      "debugger instruction can only be a single opcode atom");

  makeWritable(address, sizeof(inst::DebuggerInst));
  *address = debuggerOpcode;
  ++numInstalledBreakpoints_;
}

void CodeBlock::uninstallBreakpointAtOffset(
    uint32_t offset,
    hbc::opcode_atom_t opCode) {
  auto opcodes = getOpcodeArray();
  assert(offset < opcodes.size() && "unpatch offset out of bounds");
  hbc::opcode_atom_t *address =
      const_cast<hbc::opcode_atom_t *>(opcodes.begin() + offset);
  assert(
      *address == static_cast<hbc::opcode_atom_t>(OpCode::Debugger) &&
      "can't uninstall a non-debugger instruction");

  // This is valid because we can only uninstall breakpoints that we installed.
  // Therefore, the page here must be writable.
  *address = opCode;
  --numInstalledBreakpoints_;
}

#endif

} // namespace vm
} // namespace hermes

#undef DEBUG_TYPE
