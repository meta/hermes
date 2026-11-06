/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ISel.h"

#include "BytecodeGenerator.h"
#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/BCGen/HBC/DebugInfo.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/BCGen/HBC/HVMRegisterAllocator.h"
#include "hermes/BCGen/MovElimination.h"
#include "hermes/SourceMap/SourceMapGenerator.h"
#include "hermes/Support/BigIntSupport.h"
#include "hermes/Support/Statistic.h"

#include "llvh/ADT/Optional.h"

#define DEBUG_TYPE "hbc-backend-isel"

namespace hermes {
namespace hbc {

void HVMRegisterAllocator::handleInstruction(Instruction *I) {
  if (auto *CI = llvh::dyn_cast<BaseCallInst>(I)) {
    allocateCallInst(CI);
  }
}

bool HVMRegisterAllocator::hasTargetSpecificLowering(Instruction *I) {
  return llvh::isa<BaseCallInst>(I);
}

void HVMRegisterAllocator::allocateCallInst(BaseCallInst *I) {
  allocateParameterCount(I->getNumArguments() + CALL_EXTRA_REGISTERS);
}

namespace {

STATISTIC(NumJumpPass, "Number of passes to resolve all jump targets");
STATISTIC(
    NumUncachedNodes,
    "Number of put/get property instructions with property caching disabled");
STATISTIC(
    NumCachedNodes,
    "Number of put/get property instructions with property caching enabled");
STATISTIC(
    NumCacheSlots,
    "Number of cache slots allocated for all put/get property instructions");
STATISTIC(
    NumPutCacheSlots,
    "Number of cache slots allocated for all put property instructions");
STATISTIC(
    NumFunctionsWithWriteCache,
    "Number of functions with at least one put property instruction");
STATISTIC(
    NumFunctionsWithReadCache,
    "Number of functions with at least one get property instruction");

/// Given a list of basic blocks \p blocks linearized into the order they will
/// be generated, \return the set of those basic blocks containing backwards
/// successors. Note a jump from a block to itself is necessarily backwards.
static DenseSet<const BasicBlock *> basicBlocksWithBackwardSuccessors(
    ArrayRef<BasicBlock *> blocks) {
  llvh::SmallDenseSet<const BasicBlock *, 16> seen;
  DenseSet<const BasicBlock *> result;
  for (const BasicBlock *BB : blocks) {
    seen.insert(BB);
    // Add all successors that are in our seen set.
    for (const auto *successor : successors(BB)) {
      if (seen.count(successor))
        result.insert(successor);
    }
  }
  return result;
}

class HBCISel {
  struct Relocation {
    enum RelocationType {
      // A short jump instruction
      JumpType = 0,
      // A long jump instruction
      LongJumpType,
      // A basic block
      BasicBlockType,
      // A catch instruction
      CatchType,
      // Debug info
      DebugInfo,
      // Jump table dispatch
      JumpTableDispatch,
      // String switch table dispatch
      StringSwitchDispatch,
    };

    /// The current location of this relocation.
    offset_t loc;
    /// Type of the relocation.
    RelocationType type;
    /// We multiplex pointer for different things under different types:
    /// If the type is jump or long jump, pointer is the target basic block;
    /// if the type is basic block, pointer is the pointer to it.
    /// if the type is catch instruction, pointer is the pointer to it.
    Value *pointer;
  };

  /// Info about a UIntSwitchImm (jump table) instruction, for use during jump
  /// relocation.
  struct UIntSwitchImmInfo {
    /// Offset of the instruction
    uint32_t offset;

    /// Block to jump to when no matching case is found.
    BasicBlock *defaultTarget;

    /// The actual jump table table.
    /// The i'th index indicates which basic block should be jumped to for value
    /// i
    std::vector<BasicBlock *> table;
  };

  /// Info for a case of an all-string-case switch statement.
  struct StringSwitchCase {
    StringID label;
    BasicBlock *target;
  };

  /// Info about a StringSwitchImm instruction, for use during jump relocation.
  struct StringSwitchImmInfo {
    /// Offset of the instruction
    uint32_t offset;

    /// Block to jump to when no matching case is found.
    BasicBlock *defaultTarget;

    /// The actual string switch table.
    /// The i'th index is a pair of the string label and basic block should be
    /// jumped to for the ith case.
    std::vector<StringSwitchCase> table;
  };

  /// The function that we are compiling.
  Function *F_;

  /// The bytecode function that we are constructing.
  BytecodeFunctionGenerator *BCFGen_;

  /// The register allocator.
  HVMRegisterAllocator &RA_;

  /// For each Basic Block, we map to its beginning instruction location
  /// and the next basic block. We need this information to resolve jump
  /// targets and exception handler table.
  DenseMap<BasicBlock *, std::pair<offset_t, BasicBlock *>> basicBlockMap_{};

  /// The set of BasicBlocks that require an async break check prefix.
  DenseSet<const BasicBlock *> asyncBreakChecks_{};

  /// The list of all jump instructions and jump targets that require
  /// relocation and address resolution.
  llvh::SmallVector<Relocation, 8> relocations_{};

  /// A map of instructions to bytecode locations for debug info.
  DenseMap<Instruction *, offset_t> debugInstructionOffset_{};

  /// Mapping from CatchInst to the catch coverage information.
  CatchInfoMap catchInfoMap_{};

  /// Bytecode generation options.
  const BytecodeGenerationOptions &bytecodeGenerationOptions_;

  /// Map from UIntSwitchImm -> (inst offset, default block, jump table).
  llvh::DenseMap<UIntSwitchImmInst *, UIntSwitchImmInfo> uintSwitchImmInfo_{};
  using uintSwitchInfoEntry = llvh::
      DenseMap<UIntSwitchImmInst *, UIntSwitchImmInfo>::iterator::value_type;

  /// Map from StringSwitchImm -> (inst offset, default block, string switch
  /// table).
  llvh::DenseMap<StringSwitchImmInst *, StringSwitchImmInfo>
      stringSwitchImmInfo_{};
  using stringSwitchInfoEntry =
      llvh::DenseMap<StringSwitchImmInst *, StringSwitchImmInfo>::iterator::
          value_type;

  /// Saved identifier of "__proto__" for fast comparisons.
  Identifier protoIdent_{};
  /// Saved identifier of "prototype" for fast lookup.
  Identifier prototypeIdent_{};

  /// Encode a value into a param_t type.
  unsigned encodeValue(Value *);

  /// Resolve the offset of every relocation.
  void resolveRelocations();

  /// Add long jump instruction to the relocation list.
  void registerLongJump(offset_t loc, BasicBlock *target);

  /// Add a UIntSwitchImm (jump table) switch to relocation list.
  void registerUIntSwitchImm(offset_t loc, UIntSwitchImmInst *target);

  /// Add a StringSwitchImm (string switch table) switch to relocation list.
  void registerStringSwitchImm(offset_t loc, StringSwitchImmInst *target);

  /// Resolve all exception handlers.
  void resolveExceptionHandlers();

  /// Generate the jump table into the final representation.
  void generateJumpTable();

  /// Generate the string switch table into the final representation.
  void generateStringSwitchTable();

  /// Conveniently extract file/line/column from a SMLoc.
  /// Associate the source map script ID with the filename ID in the Module.
  bool getDebugSourceLocation(
      SourceErrorManager &manager,
      SMLoc loc,
      DebugSourceLocation *out);

  /// Given a bufferID, find or add the corresponding filename and source map
  /// IDs in BytecodeFunctionGenerator and return them.
  FileAndSourceMapId obtainFileAndSourceMapId(
      SourceErrorManager &sm,
      unsigned bufId);

  /// Add applicable debug info.
  void addDebugSourceLocationInfo();
  void addDebugLexicalInfo();

  /// Populate Property caching metadata to the function.
  void populatePropertyCachingInfo();

  /// Emit a mov, or none if it would be a no-op.
  void emitMovIfNeeded(param_t dest, param_t src);

  /// In debug mode, assert that parameters have been correctly allocated.
  void verifyCall(BaseCallInst *Inst);

  /// The last emitted property cache index.
  uint8_t nextPropertyReadCacheIndex_{0};
  uint8_t nextPropertyWriteCacheIndex_{0};
  uint8_t nextPrivateNameCacheIndex_{0};

  /// This enum is used to provide extra, distinguishing information to the
  /// property reads that would otherwise share of cache index.
  enum class PropCacheKind : uint8_t { NormalIdentifier = 0, WithReceiver };

  /// Map from property name & kind to the read cache index for that name.
  llvh::DenseMap<std::pair<Identifier, int>, uint8_t>
      propertyReadCacheIndexForId_;
  llvh::DenseMap<Identifier, uint8_t> propertyWriteCacheIndexForId_;

  /// This enum is used to provide extra, distinguishing information to the
  /// private name operations that would otherwise share a cache index.
  enum class PrivateNameOperationKind : uint8_t { ReadWrite = 0, In };
  /// Map from property name & kind to the private name cache index for that
  /// name.
  llvh::DenseMap<std::pair<Value *, int>, uint8_t> privateNameCacheIdx_;

  /// Compute and return the index to use for caching the read/write of a
  /// property with the given identifier name.
  /// \param k is the kind of property read cache being acquired.
  uint8_t acquirePropertyReadCacheIndex(
      Identifier prop,
      PropCacheKind k = PropCacheKind::NormalIdentifier);
  uint8_t acquirePropertyWriteCacheIndex(Identifier prop);

  /// Compute and return the index to use for caching some operation involving a
  /// private name.
  /// \param privateName is the symbol value of the private name.
  /// \k is the kind of operation being performed.
  uint8_t acquirePrivateNameCacheIndex(
      Value *privateName,
      PrivateNameOperationKind k);

  /// A cache mapping from buffer ID to filelname+source map.
  FileAndSourceMapIdCache &fileAndSourceMapIdCache_;
  /// To avoid performing a hash lookup in most cases, cache the last found
  /// buffer ID and file and source map IDs.
  FileAndSourceMapIdCache::value_type *lastFoundFileSourceMapId_ = nullptr;

  /// Generate bytecode for the basic block \p BB with the knowledge that the
  /// next basic block that we'll generate after this block is \p next. If \p BB
  /// is the last basic block then \p next is null.
  void generateBB(BasicBlock *BB, BasicBlock *next);

  /// Generate bytecode for the instruction \p II.
  void generateInst(Instruction *ii, BasicBlock *next);

/// This is the header declaration for all of the methods that emit opcodes
/// for specific high-level IR instructions.
#define DEF_VALUE(CLASS, PARENT) \
  void generate##CLASS(CLASS *Inst, BasicBlock *next);
#define BEGIN_VALUE(CLASS, PARENT) DEF_VALUE(CLASS, PARENT)
#include "hermes/IR/ValueKinds.def"
#undef DEF_VALUE
#undef MARK_VALUE

 public:
  /// C'tor.
  /// \p F is the function that we are constructing.
  /// \p OS is the output stream.
  HBCISel(
      Function *F,
      BytecodeFunctionGenerator *BCFGen,
      HVMRegisterAllocator &RA,
      const BytecodeGenerationOptions &options,
      FileAndSourceMapIdCache &debugIdCache)
      : F_(F),
        BCFGen_(BCFGen),
        RA_(RA),
        bytecodeGenerationOptions_(options),
        fileAndSourceMapIdCache_(debugIdCache) {
    protoIdent_ = F->getContext().getIdentifier("__proto__");
    prototypeIdent_ = F->getContext().getIdentifier("prototype");
  }

  /// Generate the bytecode stream for the function.
  void run();
};

unsigned HBCISel::encodeValue(Value *value) {
  if (auto *I = llvh::dyn_cast<Instruction>(value)) {
    assert(I->hasOutput() && "Instruction has no output");
    return RA_.getHVMRegisterIndex(RA_.getRegister(I));
  } else if (auto *var = llvh::dyn_cast<Variable>(value)) {
    return var->getIndexInVariableList();
  } else {
    llvm_unreachable("Do not support other value types");
  }
}

void HBCISel::registerLongJump(offset_t loc, BasicBlock *target) {
  relocations_.push_back(
      {loc, Relocation::RelocationType::LongJumpType, target});
}

void HBCISel::registerUIntSwitchImm(offset_t loc, UIntSwitchImmInst *inst) {
  relocations_.push_back(
      {loc, Relocation::RelocationType::JumpTableDispatch, inst});
}

void HBCISel::registerStringSwitchImm(offset_t loc, StringSwitchImmInst *inst) {
  relocations_.push_back(
      {loc, Relocation::RelocationType::StringSwitchDispatch, inst});
}

void HBCISel::resolveRelocations() {
  bool changed;
  do {
    int totalShift = 0;
    changed = false;
    for (auto &relocation : relocations_) {
      auto loc = relocation.loc;
      auto *pointer = relocation.pointer;
      auto type = relocation.type;
      loc -= totalShift;
      relocation.loc = loc;
      switch (type) {
        case Relocation::LongJumpType: {
          int targetLoc = basicBlockMap_[cast<BasicBlock>(pointer)].first;
          int jumpOffset = targetLoc - loc;
          if (-128 <= jumpOffset && jumpOffset < 128 &&
              BCFGen_->hasShortJumpVariant(loc)) {
            // The jump offset can fit into one byte, and there's a shorter
            // variant of the jump.
            totalShift += 3;
            BCFGen_->shrinkJump(loc + 1);
            BCFGen_->updateJumpTarget(loc + 1, jumpOffset, 1);
            relocation.type = Relocation::JumpType;
            changed = true;
          } else {
            BCFGen_->updateJumpTarget(loc + 1, jumpOffset, 4);
          }
          break;
        }
        case Relocation::BasicBlockType:
          basicBlockMap_[cast<BasicBlock>(pointer)].first = loc;
          break;
        case Relocation::JumpType: {
          int targetLoc = basicBlockMap_[cast<BasicBlock>(pointer)].first;
          int jumpOffset = targetLoc - loc;
          BCFGen_->updateJumpTarget(loc + 1, jumpOffset, 1);
          break;
        }
        case Relocation::CatchType:
          catchInfoMap_[cast<CatchInst>(pointer)].catchLocation = loc;
          break;
        case Relocation::DebugInfo:
          // Nothing, just keep track of the location.
          break;
        case Relocation::JumpTableDispatch: {
          auto &uintSwitchImmInfo =
              uintSwitchImmInfo_[cast<UIntSwitchImmInst>(pointer)];
          // update default target jmp
          BasicBlock *defaultBlock = uintSwitchImmInfo.defaultTarget;
          int defaultOffset = basicBlockMap_[defaultBlock].first - loc;
          BCFGen_->updateJumpTarget(loc + 1 + 1 + 4, defaultOffset, 4);
          uintSwitchImmInfo_[cast<UIntSwitchImmInst>(pointer)].offset = loc;
          break;
        }
        case Relocation::StringSwitchDispatch: {
          auto &stringSwitchImmInfo =
              stringSwitchImmInfo_[cast<StringSwitchImmInst>(pointer)];
          // update default target jmp
          BasicBlock *defaultBlock = stringSwitchImmInfo.defaultTarget;
          int defaultOffset = basicBlockMap_[defaultBlock].first - loc;
          BCFGen_->updateJumpTarget(loc + 1 + 1 + 4 + 4, defaultOffset, 4);
          stringSwitchImmInfo_[cast<StringSwitchImmInst>(pointer)].offset = loc;
          break;
        }
      }

      NumJumpPass++;
    }
  } while (changed);
}

void HBCISel::resolveExceptionHandlers() {
  if (catchInfoMap_.empty()) {
    // No exception handling, no need to do anything.
    return;
  }

  BasicBlockInfoMap bbMap;
  for (auto it : basicBlockMap_) {
    bbMap[it.first] =
        std::make_pair(it.second.first, basicBlockMap_[it.second.second].first);
  }
  auto exceptionEntries = generateExceptionHandlers(catchInfoMap_, bbMap, F_);
  for (auto entry : exceptionEntries) {
    BCFGen_->addExceptionHandler(
        HBCExceptionHandlerInfo{entry.start, entry.end, entry.target});
  }
}

void HBCISel::generateJumpTable() {
  using UIntSwitchInfoEntry = llvh::
      DenseMap<UIntSwitchImmInst *, UIntSwitchImmInfo>::iterator::value_type;

  if (uintSwitchImmInfo_.empty())
    return;

  std::vector<uint32_t> res{};

  // Sort the jump table entries so iteration order is deterministic.
  llvh::SmallVector<UIntSwitchInfoEntry, 1> infoVector{
      uintSwitchImmInfo_.begin(), uintSwitchImmInfo_.end()};
  std::sort(
      infoVector.begin(),
      infoVector.end(),
      [](UIntSwitchInfoEntry &a, UIntSwitchInfoEntry &b) {
        return a.second.offset < b.second.offset;
      });

  // Fix up all UIntSwitchImm instructions with correct offset.
  for (auto &tuple : infoVector) {
    auto entry = tuple.second;
    uint32_t startOfTable = res.size();
    for (uint32_t jmpIdx = 0; jmpIdx < entry.table.size(); jmpIdx++) {
      res.push_back(basicBlockMap_[entry.table[jmpIdx]].first - entry.offset);
    }

    BCFGen_->updateTableOffset(
        // Offset is located two bytes from begining of instruction.
        entry.offset + 1 + 1,
        startOfTable * sizeof(uint32_t),
        entry.offset);
  }

  BCFGen_->setJumpTable(std::move(res));
}

void HBCISel::generateStringSwitchTable() {
  using StringSwitchInfoEntry =
      llvh::DenseMap<StringSwitchImmInst *, StringSwitchImmInfo>::iterator::
          value_type;

  if (stringSwitchImmInfo_.empty())
    return;

  std::vector<StringSwitchTableCase> res{};

  // Sort the string switch table entries so iteration order is deterministic.
  llvh::SmallVector<StringSwitchInfoEntry, 1> infoVector{
      stringSwitchImmInfo_.begin(), stringSwitchImmInfo_.end()};
  std::sort(
      infoVector.begin(),
      infoVector.end(),
      [](StringSwitchInfoEntry &a, StringSwitchInfoEntry &b) {
        return a.second.offset < b.second.offset;
      });

  // Fix up all StringSwitchImm instructions with correct offset.
  for (auto &tuple : infoVector) {
    auto entry = tuple.second;
    uint32_t startOfTable = res.size();
    for (const StringSwitchCase &stringSwitchCase : entry.table) {
      res.emplace_back(
          stringSwitchCase.label,
          basicBlockMap_[stringSwitchCase.target].first - entry.offset);
    }
    BCFGen_->updateTableOffset(
        // Offset is located 6 bytes from begining of instruction.
        // (Opcode, value register, global index).
        entry.offset + 1 + 1 + 4,
        // baseOffset is the byte size of the jump table.
        BCFGen_->jumpTableSize() * sizeof(uint32_t) +
            startOfTable * sizeof(StringSwitchTableCase),
        entry.offset);
  }

  BCFGen_->setStringSwitchTable(std::move(res));
}

bool HBCISel::getDebugSourceLocation(
    SourceErrorManager &manager,
    SMLoc loc,
    DebugSourceLocation *out) {
  SourceErrorManager::SourceCoords coords{};
  if (!manager.findBufferLineAndLoc(loc, coords)) {
    return false;
  }

  auto ids = obtainFileAndSourceMapId(manager, coords.bufId);

  out->line = coords.line;
  out->column = coords.col;
  out->filenameId = ids.filenameId;
  out->sourceMappingUrlId = ids.sourceMappingUrlId;

  return true;
}

inline FileAndSourceMapId HBCISel::obtainFileAndSourceMapId(
    SourceErrorManager &sm,
    unsigned bufId) {
  if (LLVM_LIKELY(
          lastFoundFileSourceMapId_ &&
          lastFoundFileSourceMapId_->first == bufId)) {
    return lastFoundFileSourceMapId_->second;
  }

  auto it = fileAndSourceMapIdCache_.find(bufId);
  if (LLVM_LIKELY(it != fileAndSourceMapIdCache_.end())) {
    lastFoundFileSourceMapId_ = &*it;
    return it->second;
  }

  llvh::StringRef filename = sm.getSourceUrl(bufId);

  uint32_t currentFilenameId = BCFGen_->addFilename(filename);
  uint32_t currentSourceMappingUrlId;

  llvh::StringRef sourceMappingUrl{};
  // Only fetch the source map URL if we are not stripping it and if we are
  // generating full debug info.
  if (!bytecodeGenerationOptions_.stripSourceMappingURL &&
      F_->getContext().getDebugInfoSetting() >= DebugInfoSetting::ALL) {
    sourceMappingUrl = sm.getSourceMappingUrl(bufId);
  }

  if (sourceMappingUrl.empty()) {
    currentSourceMappingUrlId = kInvalidSourceMappingUrlId;
  } else {
    // NOTE: this is potentially a very expensive operation, since the source
    // mapping URL could be many megabytes (when it is a data URL). Looking it
    // up in the hash table potentially requires scanning it three times.
    currentSourceMappingUrlId = BCFGen_->addFilename(
        F_->getParent()->getContext().getIdentifier(sourceMappingUrl).str());
  }

  it = fileAndSourceMapIdCache_
           .try_emplace(
               bufId,
               FileAndSourceMapId{currentFilenameId, currentSourceMappingUrlId})
           .first;
  lastFoundFileSourceMapId_ = &*it;
  return it->second;
}

void HBCISel::addDebugSourceLocationInfo() {
  bool needDebugStatementNo =
      F_->getContext().getDebugInfoSetting() == DebugInfoSetting::ALL ||
      F_->getContext().getDebugInfoSetting() == DebugInfoSetting::SOURCE_MAP;
  bool needsScopingInfo =
      F_->getContext().getDebugInfoSetting() == DebugInfoSetting::ALL;
  auto &manager = F_->getContext().getSourceErrorManager();
  IRBuilder builder(F_);

  DebugSourceLocation info{};

  bool hasDebugInfo = false;
  for (auto &reloc : relocations_) {
    if (reloc.type != Relocation::DebugInfo)
      continue;

    auto *inst = cast<Instruction>(reloc.pointer);

    // Skip instructions that don't have a location if we haven't emitted
    // anything yet, because no location is the default.
    if (!inst->hasLocation() && !hasDebugInfo)
      continue;

    hasDebugInfo = true;

    if (inst->hasLocation()) {
      auto location = inst->getLocation();

      if (LLVM_UNLIKELY(!getDebugSourceLocation(manager, location, &info))) {
        hermes_fatal("Unable to get source location");
      }
      info.statement = needDebugStatementNo ? inst->getStatementIndex() : 0;
    } else {
      info.line = 0;
      info.column = 0;
      info.statement = 0;
    }
    info.address = reloc.loc;

    // Set the scoping state for this debug source location.
    auto envID = inst->getEnvironmentID();
    auto *lexScope = inst->getLexicalScope();
    if (!needsScopingInfo || envID == 0 || lexScope == nullptr) {
      // If we aren't generating scoping info, or the envID/lexical scope wasn't
      // tracked properly, then there is no scoping information we can generate.
      // An index of 0 is reserved to indicate no scoping information.
      info.envIdx = 0;
    } else {
      assert(
          envID >= Instruction::kFirstScopeCreationIdIndex &&
          "expected valid environment ID");
      auto *implicitEnvOp = inst->getImplicitEnvOperand();
      // If the implicit environment operand is a Variable, this means it
      // was spilled to the parent environment because of the generator
      // lowering pass.
      // If the implicit environment operand is an Instruction, it is still
      // reachable and reserved in a register.
      DebugScopingInfo debInfo = llvh::isa<Variable>(implicitEnvOp)
          ? DebugScopingInfo::forSpilledSlot(
                llvh::cast<Variable>(implicitEnvOp)->getIndexInVariableList(),
                lexScope->idxInParentFunction)
          : DebugScopingInfo::forRegister(
                encodeValue(llvh::cast<Instruction>(implicitEnvOp)),
                lexScope->idxInParentFunction);
      info.envIdx = BCFGen_->addScopingInfo(debInfo);
    }

    BCFGen_->addDebugSourceLocation(info);
  }

  // If there's no debug info, don't set the function location.
  // This avoids polluting the string table with source file names.
  if (hasDebugInfo) {
    if (getDebugSourceLocation(manager, F_->getSourceRange().Start, &info)) {
      info.address = 0;
      info.statement = 0;
    } else {
      // If there is debug info but the function doesn't have source location
      // (e.g., it's synthetic function), reset info to default, which indicates
      // invalid source location (i.e., with 0 line/column value). We will check
      // for this when deserializing function debug info.
      info = {};
    }
    BCFGen_->setSourceLocation(info);
  }
}

void HBCISel::addDebugLexicalInfo() {
  // Only emit if debug info is enabled.
  if (F_->getContext().getDebugInfoSetting() != DebugInfoSetting::ALL)
    return;

  // Set the lexical parent by finding the function in which a closure is
  // created with F_, if any.
  for (auto *U : F_->getUsers()) {
    if (llvh::isa<BaseCreateLexicalChildInst>(U)) {
      BCFGen_->setLexicalParentID(BCFGen_->getFunctionID(U->getFunction()));
      break;
    }
  }
}

void HBCISel::populatePropertyCachingInfo() {
  BCFGen_->setReadCacheSize(nextPropertyReadCacheIndex_);
  BCFGen_->setWriteCacheSize(nextPropertyWriteCacheIndex_);
  BCFGen_->setPrivateNameCacheSize(nextPrivateNameCacheIndex_);
  if (nextPropertyReadCacheIndex_ > 0) {
    NumFunctionsWithReadCache++;
  }
  if (nextPropertyWriteCacheIndex_ > 0) {
    NumFunctionsWithWriteCache++;
  }
}

void HBCISel::generateDirectEvalInst(DirectEvalInst *Inst, BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto evalText = encodeValue(Inst->getEvalText());
  BCFGen_->emitDirectEval(dst, evalText, Inst->getStrictCaller());
}

void HBCISel::generateDeclareGlobalVarInst(
    DeclareGlobalVarInst *inst,
    BasicBlock *next) {
  BCFGen_->emitDeclareGlobalVar(BCFGen_->getIdentifierID(inst->getName()));
}

void HBCISel::generateAddEmptyStringInst(
    AddEmptyStringInst *Inst,
    BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  BCFGen_->emitAddEmptyString(dst, src);
}

void HBCISel::generateToPropertyKeyInst(
    ToPropertyKeyInst *Inst,
    BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  BCFGen_->emitToPropertyKey(dst, src);
}

void HBCISel::generateCreatePrivateNameInst(
    CreatePrivateNameInst *Inst,
    BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto *descStr = llvh::cast<LiteralString>(Inst->getSingleOperand());
  auto id = BCFGen_->getIdentifierID(descStr);
  BCFGen_->emitCreatePrivateName(dst, id);
}

void HBCISel::generateAsNumberInst(AsNumberInst *Inst, BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  BCFGen_->emitToNumber(dst, src);
}

void HBCISel::generateAsNumericInst(AsNumericInst *Inst, BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  BCFGen_->emitToNumeric(dst, src);
}

void HBCISel::generateAsInt32Inst(AsInt32Inst *Inst, BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  BCFGen_->emitToInt32(dst, src);
}

void HBCISel::generateAsUint32Inst(AsUint32Inst *Inst, BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  BCFGen_->emitToUint32(dst, src);
}

void HBCISel::emitMovIfNeeded(param_t dest, param_t src) {
  if (dest == src)
    return;
  if (dest <= UINT8_MAX && src <= UINT8_MAX) {
    BCFGen_->emitMov(dest, src);
  } else {
    BCFGen_->emitMovLong(dest, src);
  }
}

void HBCISel::verifyCall(BaseCallInst *Inst) {
#ifndef NDEBUG
  const auto lastArgReg = RA_.lastCallArgRegister();

  const bool isBuiltin = llvh::isa<CallBuiltinInst>(Inst);
  const bool isCallN = llvh::isa<HBCCallNInst>(Inst);

  for (unsigned i = 0, max = Inst->getNumArguments(); i < max; i++) {
    Value *argument = Inst->getArgument(i);
    // The first argument (thisArg) of CallBuiltin LiteralUndefined.
    if (isBuiltin && i == 0) {
      assert(
          llvh::isa<LiteralUndefined>(argument) && !RA_.isAllocated(argument) &&
          "Register for 'this' argument is misallocated");
    } else if (isCallN) {
      // CallN may take arguments from anywhere except for the last N registers
      // of the frame, because the bytecode instruction overwrites those
      // registers. Note that <= is correct because lastArgReg is the index of
      // the last register, not the count of registers.
      assert(
          llvh::isa<Instruction>(argument) &&
          RA_.getHVMRegisterIndex(RA_.getRegister(argument)) <=
              RA_.getHVMRegisterIndex(lastArgReg) - max &&
          "Register is misallocated");
    } else {
      // Calls require that the arguments be at the end of the frame, in reverse
      // order.
      assert(
          llvh::isa<Instruction>(argument) &&
          RA_.getHVMRegisterIndex(RA_.getRegister(argument)) <=
              RA_.getHVMRegisterIndex(lastArgReg) - i &&
          "Register is misallocated");
    }
  }
#endif
}

void HBCISel::generateLoadStackInst(LoadStackInst *Inst, BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  emitMovIfNeeded(dst, src);
}
void HBCISel::generateMovInst(MovInst *Inst, BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  emitMovIfNeeded(dst, src);
}

void HBCISel::generateImplicitMovInst(ImplicitMovInst *Inst, BasicBlock *next) {
  // ImplicitMovs produce no bytecode, they only express that a subsequent
  // instruction will perform the equivalent of a 'Mov'.
}

void HBCISel::generateTypeOfInst(TypeOfInst *Inst, hermes::BasicBlock *) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getArgument());
  BCFGen_->emitTypeOf(dst, src);
}

void HBCISel::generateTypeOfIsInst(TypeOfIsInst *Inst, hermes::BasicBlock *) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getArgument());
  TypeOfIsTypes types = Inst->getTypes()->getData();
  BCFGen_->emitTypeOfIs(dst, src, types.getRaw());
}

void HBCISel::generateUnaryOperatorInst(
    UnaryOperatorInst *Inst,
    BasicBlock *next) {
  auto opReg = encodeValue(Inst->getSingleOperand());
  auto resReg = encodeValue(Inst);

  switch (Inst->getKind()) {
    case ValueKind::UnaryMinusInstKind: { // -
      BCFGen_->emitNegate(resReg, opReg);
      break;
    }
    case ValueKind::UnaryTildeInstKind: { // ~
      BCFGen_->emitBitNot(resReg, opReg);
      break;
    }
    case ValueKind::UnaryBangInstKind: { // !
      BCFGen_->emitNot(resReg, opReg);
      break;
    }
    case ValueKind::UnaryVoidInstKind: { // Void operator.
      BCFGen_->emitLoadConstUndefined(resReg);
      break;
    }
    case ValueKind::UnaryIncInstKind: { // ++
      BCFGen_->emitInc(resReg, opReg);
      break;
    }
    case ValueKind::UnaryDecInstKind: { // --
      BCFGen_->emitDec(resReg, opReg);
      break;
    }
    default:
      llvm_unreachable("Can't handle this operation");
  }
}
void HBCISel::generateLoadFrameInst(
    hermes::LoadFrameInst *Inst,
    hermes::BasicBlock *next) {
  auto dstReg = encodeValue(Inst);
  Variable *var = Inst->getLoadVariable();
  auto envReg = encodeValue(Inst->getScope());
  auto varIdx = encodeValue(var);
  if (varIdx <= UINT8_MAX) {
    BCFGen_->emitLoadFromEnvironment(dstReg, envReg, varIdx);
  } else {
    BCFGen_->emitLoadFromEnvironmentL(dstReg, envReg, varIdx);
  }
}
void HBCISel::generatePhiInst(PhiInst *Inst, BasicBlock *next) {
  // PhiInst has been translated into a sequence of MOVs in RegAlloc
  // Nothing to do here.
}
void HBCISel::generateBinaryOperatorInst(
    BinaryOperatorInst *Inst,
    BasicBlock *next) {
  auto left = encodeValue(Inst->getLeftHandSide());
  auto right = encodeValue(Inst->getRightHandSide());
  auto res = encodeValue(Inst);

  bool isBothNumber = Inst->getLeftHandSide()->getType().isNumberType() &&
      Inst->getRightHandSide()->getType().isNumberType();

  switch (Inst->getKind()) {
    case ValueKind::BinaryEqualInstKind: // ==
      // TODO: optimize the case for null check.
      BCFGen_->emitEq(res, left, right);
      break;
    case ValueKind::BinaryNotEqualInstKind: // !=
      // TODO: optimize the case for null check.
      BCFGen_->emitNeq(res, left, right);
      break;
    case ValueKind::BinaryStrictlyEqualInstKind: // ===
      BCFGen_->emitStrictEq(res, left, right);
      break;
    case ValueKind::BinaryStrictlyNotEqualInstKind: // !===
      BCFGen_->emitStrictNeq(res, left, right);
      break;
    case ValueKind::BinaryLessThanInstKind: // <
      BCFGen_->emitLess(res, left, right);
      break;
    case ValueKind::BinaryLessThanOrEqualInstKind: // <=
      BCFGen_->emitLessEq(res, left, right);
      break;
    case ValueKind::BinaryGreaterThanInstKind: // >
      BCFGen_->emitGreater(res, left, right);
      break;
    case ValueKind::BinaryGreaterThanOrEqualInstKind: // >=
      BCFGen_->emitGreaterEq(res, left, right);
      break;
    case ValueKind::BinaryLeftShiftInstKind: // <<  (<<=)
      BCFGen_->emitLShift(res, left, right);
      break;
    case ValueKind::BinaryRightShiftInstKind: // >>  (>>=)
      BCFGen_->emitRShift(res, left, right);
      break;
    case ValueKind::BinaryUnsignedRightShiftInstKind: // >>> (>>>=)
      BCFGen_->emitURshift(res, left, right);
      break;
    case ValueKind::BinaryAddInstKind: // +   (+=)
      if (isBothNumber) {
        BCFGen_->emitAddN(res, left, right);
      } else {
        BCFGen_->emitAdd(res, left, right);
      }
      break;
    case ValueKind::BinarySubtractInstKind: // -   (-=)
      if (isBothNumber) {
        BCFGen_->emitSubN(res, left, right);
      } else {
        BCFGen_->emitSub(res, left, right);
      }
      break;
    case ValueKind::BinaryMultiplyInstKind: // *   (*=)
      if (isBothNumber) {
        BCFGen_->emitMulN(res, left, right);
      } else {
        BCFGen_->emitMul(res, left, right);
      }
      break;
    case ValueKind::BinaryDivideInstKind: // /   (/=)
      if (isBothNumber) {
        BCFGen_->emitDivN(res, left, right);
      } else {
        BCFGen_->emitDiv(res, left, right);
      }
      break;
    case ValueKind::BinaryExponentiationInstKind: // ** (**=)
      llvm_unreachable("ExponentiationKind emits a HermesInternal call");
    case ValueKind::BinaryModuloInstKind: // %   (%=)
      BCFGen_->emitMod(res, left, right);
      break;
    case ValueKind::BinaryOrInstKind: // |   (|=)
      BCFGen_->emitBitOr(res, left, right);
      break;
    case ValueKind::BinaryXorInstKind: // ^   (^=)
      BCFGen_->emitBitXor(res, left, right);
      break;
    case ValueKind::BinaryAndInstKind: // &   (^=)
      BCFGen_->emitBitAnd(res, left, right);
      break;
    case ValueKind::BinaryInInstKind: // "in"
      BCFGen_->emitIsIn(res, left, right);
      break;
    case ValueKind::BinaryPrivateInInstKind: // #privateName "in"
      BCFGen_->emitPrivateIsIn(
          res,
          left,
          right,
          acquirePrivateNameCacheIndex(
              Inst->getRightHandSide(), PrivateNameOperationKind::In));
      break;
    case ValueKind::BinaryInstanceOfInstKind: // instanceof
      BCFGen_->emitInstanceOf(res, left, right);
      break;

    default:
      break;
  }
}

void HBCISel::generateImulInst(ImulInst *Inst, BasicBlock *next) {
  auto left = encodeValue(Inst->getLeft());
  auto right = encodeValue(Inst->getRight());
  auto res = encodeValue(Inst);
  BCFGen_->emitImul(res, left, right);
}

void HBCISel::generateStorePropertyWithReceiverInst(
    StorePropertyWithReceiverInst *Inst,
    BasicBlock *next) {
  auto valueReg = encodeValue(Inst->getStoredValue());
  auto objReg = encodeValue(Inst->getObject());
  auto propReg = encodeValue(Inst->getProperty());
  auto receiverReg = encodeValue(Inst->getReceiver());
  BCFGen_->emitPutByValWithReceiver(
      objReg, propReg, valueReg, receiverReg, Inst->getIsStrict());
}

void HBCISel::generateStorePropertyLooseInst(
    StorePropertyLooseInst *Inst,
    BasicBlock *next) {
  auto valueReg = encodeValue(Inst->getStoredValue());
  auto objReg = encodeValue(Inst->getObject());
  auto prop = Inst->getProperty();

  if (auto *Lit = llvh::dyn_cast<LiteralString>(prop)) {
    // Property is a string
    auto id = BCFGen_->getIdentifierID(Lit);
    if (id <= UINT16_MAX)
      BCFGen_->emitPutByIdLoose(
          objReg,
          valueReg,
          acquirePropertyWriteCacheIndex(Lit->getValue()),
          id);
    else
      BCFGen_->emitPutByIdLooseLong(
          objReg,
          valueReg,
          acquirePropertyWriteCacheIndex(Lit->getValue()),
          id);
    return;
  }

  auto propReg = encodeValue(prop);
  BCFGen_->emitPutByValLoose(objReg, propReg, valueReg);
}

void HBCISel::generateStorePropertyStrictInst(
    StorePropertyStrictInst *Inst,
    BasicBlock *next) {
  auto valueReg = encodeValue(Inst->getStoredValue());
  auto objReg = encodeValue(Inst->getObject());
  auto prop = Inst->getProperty();

  if (auto *Lit = llvh::dyn_cast<LiteralString>(prop)) {
    // Property is a string
    auto id = BCFGen_->getIdentifierID(Lit);
    if (id <= UINT16_MAX)
      BCFGen_->emitPutByIdStrict(
          objReg,
          valueReg,
          acquirePropertyWriteCacheIndex(Lit->getValue()),
          id);
    else
      BCFGen_->emitPutByIdStrictLong(
          objReg,
          valueReg,
          acquirePropertyWriteCacheIndex(Lit->getValue()),
          id);
    return;
  }

  auto propReg = encodeValue(prop);
  BCFGen_->emitPutByValStrict(objReg, propReg, valueReg);
}

void HBCISel::generateTryStoreGlobalPropertyLooseInst(
    TryStoreGlobalPropertyLooseInst *Inst,
    BasicBlock *next) {
  auto valueReg = encodeValue(Inst->getStoredValue());
  auto objReg = encodeValue(Inst->getObject());
  auto prop = Inst->getProperty();

  auto *Lit = cast<LiteralString>(prop);

  auto id = BCFGen_->getIdentifierID(Lit);
  if (id <= UINT16_MAX)
    BCFGen_->emitTryPutByIdLoose(
        objReg, valueReg, acquirePropertyWriteCacheIndex(Lit->getValue()), id);
  else
    BCFGen_->emitTryPutByIdLooseLong(
        objReg, valueReg, acquirePropertyWriteCacheIndex(Lit->getValue()), id);
}

void HBCISel::generateTryStoreGlobalPropertyStrictInst(
    TryStoreGlobalPropertyStrictInst *Inst,
    BasicBlock *next) {
  auto valueReg = encodeValue(Inst->getStoredValue());
  auto objReg = encodeValue(Inst->getObject());
  auto prop = Inst->getProperty();

  auto *Lit = cast<LiteralString>(prop);

  auto id = BCFGen_->getIdentifierID(Lit);
  if (id <= UINT16_MAX)
    BCFGen_->emitTryPutByIdStrict(
        objReg, valueReg, acquirePropertyWriteCacheIndex(Lit->getValue()), id);
  else
    BCFGen_->emitTryPutByIdStrictLong(
        objReg, valueReg, acquirePropertyWriteCacheIndex(Lit->getValue()), id);
}

void HBCISel::generateDefineOwnPropertyInst(
    DefineOwnPropertyInst *Inst,
    BasicBlock *next) {
  auto valueReg = encodeValue(Inst->getStoredValue());
  auto objReg = encodeValue(Inst->getObject());
  Value *prop = Inst->getProperty();

  // If the property is a LiteralNumber, the property is enumerable, and it is a
  // valid array index, it is coming from an array initialization and we will
  // emit it as DefineOwnByIndex.
  auto *numProp = llvh::dyn_cast<LiteralNumber>(prop);
  if (numProp) {
    assert(
        Inst->getIsEnumerable() &&
        "Non-enumerable properties with literal keys should be handled by LoadConstants");
    uint32_t index = numProp->convertToArrayIndex().getValue();
    if (index <= UINT8_MAX) {
      BCFGen_->emitDefineOwnByIndex(objReg, valueReg, index);
    } else {
      BCFGen_->emitDefineOwnByIndexL(objReg, valueReg, index);
    }
    return;
  }

  if (auto *Lit = llvh::dyn_cast<LiteralString>(prop)) {
    assert(
        Inst->getIsEnumerable() &&
        "Non-enumerable properties with literal keys should be handled by LoadConstants");
    auto id = BCFGen_->getIdentifierID(Lit);
    if (id <= UINT16_MAX) {
      BCFGen_->emitDefineOwnById(
          objReg,
          valueReg,
          acquirePropertyWriteCacheIndex(Lit->getValue()),
          id);
    } else {
      BCFGen_->emitDefineOwnByIdLong(
          objReg,
          valueReg,
          acquirePropertyWriteCacheIndex(Lit->getValue()),
          id);
    }
    return;
  }

  // It is a register operand.
  auto propReg = encodeValue(Inst->getProperty());
  BCFGen_->emitDefineOwnByVal(
      objReg, valueReg, propReg, Inst->getIsEnumerable());
}

void HBCISel::generateDefineOwnInDenseArrayInst(
    DefineOwnInDenseArrayInst *Inst,
    BasicBlock *next) {
  auto valueReg = encodeValue(Inst->getStoredValue());
  auto objReg = encodeValue(Inst->getArray());
  LiteralNumber *arrayIndex = Inst->getArrayIndex();
  uint32_t index = arrayIndex->asUInt32();
  if (index <= UINT8_MAX) {
    BCFGen_->emitDefineOwnInDenseArray(objReg, valueReg, index);
  } else if (index < UINT16_MAX) {
    // NewArray caps the storage at UINT16_MAX elements (indices
    // 0..UINT16_MAX-1), so index UINT16_MAX must take the growing
    // DefineOwnByIndex path.
    BCFGen_->emitDefineOwnInDenseArrayL(objReg, valueReg, index);
  } else {
    // Fallback: use DefineOwnByIndex.
    BCFGen_->emitDefineOwnByIndexL(objReg, valueReg, index);
  }
}

void HBCISel::generateStoreOwnPrivateFieldInst(
    StoreOwnPrivateFieldInst *Inst,
    BasicBlock *next) {
  auto objReg = encodeValue(Inst->getObject());
  auto valueReg = encodeValue(Inst->getStoredValue());
  auto prop = Inst->getProperty();
  auto propReg = encodeValue(prop);
  uint32_t privateNameCacheIdx = acquirePrivateNameCacheIndex(
      Inst->getProperty(), PrivateNameOperationKind::ReadWrite);
  BCFGen_->emitPutOwnPrivateBySym(
      objReg, valueReg, privateNameCacheIdx, propReg);
}

void HBCISel::generateAddOwnPrivateFieldInst(
    AddOwnPrivateFieldInst *Inst,
    BasicBlock *next) {
  auto objReg = encodeValue(Inst->getObject());
  auto valueReg = encodeValue(Inst->getStoredValue());
  auto symReg = encodeValue(Inst->getProperty());
  BCFGen_->emitAddOwnPrivateBySym(objReg, valueReg, symReg);
}

void HBCISel::generatePrivateBrandCheckInst(
    PrivateBrandCheckInst *Inst,
    BasicBlock *next) {
  hermes_fatal("PrivateBrandCheckInst should have been lowered.");
}

void HBCISel::generateDefineOwnGetterSetterInst(
    DefineOwnGetterSetterInst *Inst,
    BasicBlock *next) {
  auto objReg = encodeValue(Inst->getObject());
  auto ident = encodeValue(Inst->getProperty());
  BCFGen_->emitDefineOwnGetterSetterByVal(
      objReg,
      ident,
      encodeValue(Inst->getStoredGetter()),
      encodeValue(Inst->getStoredSetter()),
      Inst->getIsEnumerable());
}
void HBCISel::generateDeletePropertyLooseInst(
    DeletePropertyLooseInst *Inst,
    BasicBlock *next) {
  auto objReg = encodeValue(Inst->getObject());
  auto resultReg = encodeValue(Inst);
  auto prop = Inst->getProperty();

  auto propReg = encodeValue(prop);
  BCFGen_->emitDelByVal(resultReg, objReg, propReg, /* strict */ 0);
}
void HBCISel::generateDeletePropertyStrictInst(
    DeletePropertyStrictInst *Inst,
    BasicBlock *next) {
  auto objReg = encodeValue(Inst->getObject());
  auto resultReg = encodeValue(Inst);
  auto prop = Inst->getProperty();

  auto propReg = encodeValue(prop);
  BCFGen_->emitDelByVal(resultReg, objReg, propReg, /* strict */ 1);
}
void HBCISel::generateLoadPropertyInst(
    LoadPropertyInst *Inst,
    BasicBlock *next) {
  auto resultReg = encodeValue(Inst);
  auto objReg = encodeValue(Inst->getObject());
  auto prop = Inst->getProperty();

  if (auto *Lit = llvh::dyn_cast<LiteralString>(prop)) {
    auto id = BCFGen_->getIdentifierID(Lit);
    if (id > UINT16_MAX) {
      BCFGen_->emitGetByIdLong(
          resultReg,
          objReg,
          acquirePropertyReadCacheIndex(Lit->getValue()),
          id);
    } else if (id > UINT8_MAX) {
      BCFGen_->emitGetById(
          resultReg,
          objReg,
          acquirePropertyReadCacheIndex(Lit->getValue()),
          id);
    } else {
      BCFGen_->emitGetByIdShort(
          resultReg,
          objReg,
          acquirePropertyReadCacheIndex(Lit->getValue()),
          id);
    }
    return;
  }

  // If the prop is a uint8_t constant, generate the special bytecode.
  if (auto *litNum = llvh::dyn_cast<LiteralNumber>(prop);
      litNum && litNum->isUInt8Representible()) {
    BCFGen_->emitGetByIndex(resultReg, objReg, litNum->asUInt8());
    return;
  }

  auto propReg = encodeValue(prop);
  BCFGen_->emitGetByVal(resultReg, objReg, propReg);
}
void HBCISel::generateLoadOwnPrivateFieldInst(
    LoadOwnPrivateFieldInst *Inst,
    BasicBlock *next) {
  auto resultReg = encodeValue(Inst);
  auto objReg = encodeValue(Inst->getObject());
  auto prop = Inst->getProperty();
  auto propReg = encodeValue(prop);
  uint32_t privateNameCacheIdx = acquirePrivateNameCacheIndex(
      Inst->getProperty(), PrivateNameOperationKind::ReadWrite);
  BCFGen_->emitGetOwnPrivateBySym(
      resultReg, objReg, privateNameCacheIdx, propReg);
}

void HBCISel::generateLoadPropertyWithReceiverInst(
    LoadPropertyWithReceiverInst *Inst,
    BasicBlock *next) {
  auto resultReg = encodeValue(Inst);
  auto objReg = encodeValue(Inst->getObject());
  auto receiverReg = encodeValue(Inst->getReceiver());
  auto prop = Inst->getProperty();

  if (auto *Lit = llvh::dyn_cast<LiteralString>(prop)) {
    auto id = BCFGen_->getIdentifierID(Lit);
    BCFGen_->emitGetByIdWithReceiverLong(
        resultReg,
        objReg,
        acquirePropertyReadCacheIndex(
            Lit->getValue(), PropCacheKind::WithReceiver),
        receiverReg,
        id);
    return;
  }

  auto propReg = encodeValue(prop);
  BCFGen_->emitGetByValWithReceiver(resultReg, objReg, propReg, receiverReg);
}

void HBCISel::generateTryLoadGlobalPropertyInst(
    TryLoadGlobalPropertyInst *Inst,
    BasicBlock *next) {
  auto resultReg = encodeValue(Inst);
  auto objReg = encodeValue(Inst->getObject());
  auto prop = Inst->getProperty();

  auto *Lit = cast<LiteralString>(prop);

  auto id = BCFGen_->getIdentifierID(Lit);
  if (id > UINT16_MAX) {
    BCFGen_->emitTryGetByIdLong(
        resultReg, objReg, acquirePropertyReadCacheIndex(Lit->getValue()), id);
  } else {
    BCFGen_->emitTryGetById(
        resultReg, objReg, acquirePropertyReadCacheIndex(Lit->getValue()), id);
  }
}

void HBCISel::generateStoreStackInst(StoreStackInst *Inst, BasicBlock *next) {
  llvm_unreachable("StoreStackInst should have been lowered.");
}
void HBCISel::generateStoreFrameInst(StoreFrameInst *Inst, BasicBlock *next) {
  Variable *var = Inst->getVariable();
  auto valueReg = encodeValue(Inst->getValue());
  auto envReg = encodeValue(Inst->getScope());
  auto varIdx = encodeValue(var);
  if (Inst->getModule()->getTypeContext().isNonPtr(
          Inst->getValue()->getType())) {
    if (varIdx <= UINT8_MAX) {
      BCFGen_->emitStoreNPToEnvironment(envReg, varIdx, valueReg);
    } else {
      BCFGen_->emitStoreNPToEnvironmentL(envReg, varIdx, valueReg);
    }
  } else {
    if (varIdx <= UINT8_MAX) {
      BCFGen_->emitStoreToEnvironment(envReg, varIdx, valueReg);
    } else {
      BCFGen_->emitStoreToEnvironmentL(envReg, varIdx, valueReg);
    }
  }
}
void HBCISel::generateAllocStackInst(AllocStackInst *Inst, BasicBlock *next) {
  // This is a no-op.
}
void HBCISel::generateAllocObjectLiteralInst(
    AllocObjectLiteralInst *Inst,
    BasicBlock *) {
  auto result = encodeValue(Inst);
  assert(
      Inst->getKeyValuePairCount() == 0 &&
      "AllocObjectLiteralInst with properties should be lowered to LIRAllocObjectFromBufferInst");
  assert(llvh::isa<EmptySentinel>(Inst->getParentObject()));
  BCFGen_->emitNewObject(result);
}
void HBCISel::generateAllocTypedObjectInst(
    AllocTypedObjectInst *Inst,
    BasicBlock *) {
  auto result = encodeValue(Inst);
  assert(
      Inst->getKeyValuePairCount() == 0 &&
      "AllocTypedObjectInst with properties should be lowered to LIRAllocObjectFromBufferInst");
  assert(llvh::isa<EmptySentinel>(Inst->getParentObject()));
  BCFGen_->emitNewObject(result);
}
void HBCISel::generateAllocTypedNonEnumObjectInst(
    AllocTypedNonEnumObjectInst *Inst,
    BasicBlock *) {
  auto result = encodeValue(Inst);
  assert(
      Inst->getKeyValuePairCount() == 0 &&
      "AllocTypedNonEnumObjectInst with properties should be lowered to LIRAllocTypedNonEnumObjectFromBufferInst");
  assert(llvh::isa<EmptySentinel>(Inst->getParentObject()));
  BCFGen_->emitNewObject(result);
}
void HBCISel::generateAllocArrayInst(AllocArrayInst *Inst, BasicBlock *next) {
  auto dstReg = encodeValue(Inst);
  auto elementCount = Inst->getElementCount();
  uint32_t sizeHint =
      std::min((uint32_t)UINT16_MAX, Inst->getSizeHint()->asUInt32());

  if (elementCount == 0) {
    BCFGen_->emitNewArray(dstReg, sizeHint);
  } else {
    auto bufIndex =
        BCFGen_->getBytecodeModuleGenerator().serializedLiteralOffsetFor(Inst);
    if (bufIndex.valueBufferOffset <= UINT16_MAX) {
      BCFGen_->emitNewArrayWithBuffer(
          encodeValue(Inst),
          sizeHint,
          elementCount,
          bufIndex.valueBufferOffset);
    } else {
      BCFGen_->emitNewArrayWithBufferLong(
          encodeValue(Inst),
          sizeHint,
          elementCount,
          bufIndex.valueBufferOffset);
    }
  }
}
void HBCISel::generateGetTemplateObjectInst(
    GetTemplateObjectInst *inst,
    BasicBlock *next) {
  hermes_fatal("GetTemplateObjectInst unsupported in HBC");
}
void HBCISel::generateCreateFunctionInst(
    CreateFunctionInst *Inst,
    BasicBlock *) {
  auto env = encodeValue(Inst->getScope());
  auto output = encodeValue(Inst);
  auto code = BCFGen_->getFunctionID(Inst->getFunctionCode());
  if (LLVM_LIKELY(code <= UINT16_MAX)) {
    // Most of the cases, function index will be less than 2^16.
    BCFGen_->emitCreateClosure(output, env, code);
  } else {
    BCFGen_->emitCreateClosureLongIndex(output, env, code);
  }
}

void HBCISel::generateLIRAllocObjectFromBufferInst(
    LIRAllocObjectFromBufferInst *Inst,
    BasicBlock *next) {
  auto result = encodeValue(Inst);
  auto buffIdxs =
      BCFGen_->getBytecodeModuleGenerator().serializedLiteralOffsetFor(Inst);
  if (Inst->getKeyValuePairCount() == 0) {
    assert(
        !llvh::isa<EmptySentinel>(Inst->getParentObject()) &&
        "must have a parent, otherwise we should use AllocObjectLiteral");
    BCFGen_->emitNewObjectWithParent(
        result, encodeValue(Inst->getParentObject()), buffIdxs.shapeTableIdx);
  } else if (!llvh::isa<EmptySentinel>(Inst->getParentObject())) {
    BCFGen_->emitNewObjectWithBufferAndParent(
        result,
        encodeValue(Inst->getParentObject()),
        buffIdxs.shapeTableIdx,
        buffIdxs.valueBufferOffset);
  } else if (
      buffIdxs.shapeTableIdx <= UINT16_MAX &&
      buffIdxs.valueBufferOffset <= UINT16_MAX) {
    BCFGen_->emitNewObjectWithBuffer(
        result, buffIdxs.shapeTableIdx, buffIdxs.valueBufferOffset);
  } else {
    BCFGen_->emitNewObjectWithBufferLong(
        result, buffIdxs.shapeTableIdx, buffIdxs.valueBufferOffset);
  }
}

void HBCISel::generateLIRAllocTypedObjectFromBufferInst(
    LIRAllocTypedObjectFromBufferInst *Inst,
    BasicBlock *next) {
  auto result = encodeValue(Inst);
  auto parent = encodeValue(Inst->getParentObject());
  auto buffIdxs =
      BCFGen_->getBytecodeModuleGenerator().serializedLiteralOffsetFor(Inst);
  if (Inst->getKeyValuePairCount() == 0) {
    BCFGen_->emitNewObjectWithParent(result, parent, buffIdxs.shapeTableIdx);
  } else {
    BCFGen_->emitNewTypedObjectWithBuffer(
        result,
        parent,
        buffIdxs.shapeTableIdx,
        buffIdxs.valueBufferOffset,
        /* nonEnumerable */ 0);
  }
}

void HBCISel::generateLIRAllocTypedNonEnumObjectFromBufferInst(
    LIRAllocTypedNonEnumObjectFromBufferInst *Inst,
    BasicBlock *next) {
  auto result = encodeValue(Inst);
  auto parent = encodeValue(Inst->getParentObject());
  auto buffIdxs =
      BCFGen_->getBytecodeModuleGenerator().serializedLiteralOffsetFor(Inst);
  if (Inst->getKeyValuePairCount() == 0) {
    BCFGen_->emitNewObjectWithParent(result, parent, buffIdxs.shapeTableIdx);
  } else {
    BCFGen_->emitNewTypedObjectWithBuffer(
        result,
        parent,
        buffIdxs.shapeTableIdx,
        buffIdxs.valueBufferOffset,
        /* nonEnumerable */ 1);
  }
}

void HBCISel::generateCatchInst(CatchInst *Inst, BasicBlock *next) {
  auto loc = BCFGen_->emitCatch(encodeValue(Inst));
  relocations_.push_back({loc, Relocation::CatchType, Inst});
  catchInfoMap_[Inst] = CatchCoverageInfo(loc);
}
void HBCISel::generateDebuggerInst(DebuggerInst *Inst, BasicBlock *next) {
  BCFGen_->emitDebugger();
}
void HBCISel::generateCreateRegExpInst(
    CreateRegExpInst *Inst,
    BasicBlock *next) {
  const auto &pattern = Inst->getPattern()->getValue();
  const auto &flags = Inst->getFlags()->getValue();
  auto &ctx = F_->getParent()->getContext();
  auto &regexp = ctx.getCompiledRegExp(
      pattern.getUnderlyingPointer(), flags.getUnderlyingPointer());
  uint32_t reBytecodeID = BCFGen_->addRegExp(&regexp);
  auto patternStrID = BCFGen_->getStringID(Inst->getPattern());
  auto flagsStrID = BCFGen_->getStringID(Inst->getFlags());
  BCFGen_->emitCreateRegExp(
      encodeValue(Inst), patternStrID, flagsStrID, reBytecodeID);
}
void HBCISel::generateTryEndInst(TryEndInst *Inst, BasicBlock *next) {
  auto *dst = Inst->getBranchDest();
  if (dst == next)
    return;

  auto loc = BCFGen_->emitJmpLong(0);
  registerLongJump(loc, dst);
}
void HBCISel::generateBranchIfBuiltinInst(
    BranchIfBuiltinInst *Inst,
    BasicBlock *next) {
  uint8_t builtinIndex = Inst->getBuiltinIndex();
  auto argument = encodeValue(Inst->getArgument());

  BasicBlock *trueBlock = Inst->getTrueBlock();
  BasicBlock *falseBlock = Inst->getFalseBlock();

  offset_t loc;
  if (next == trueBlock) {
    loc = BCFGen_->emitJmpBuiltinIsNotLong(0, builtinIndex, argument);
    registerLongJump(loc, falseBlock);
    return;
  }

  loc = BCFGen_->emitJmpBuiltinIsLong(0, builtinIndex, argument);
  registerLongJump(loc, trueBlock);

  if (next == falseBlock)
    return;

  loc = BCFGen_->emitJmpLong(0);
  registerLongJump(loc, falseBlock);
}
void HBCISel::generateBranchInst(BranchInst *Inst, BasicBlock *next) {
  auto *dst = Inst->getBranchDest();
  if (dst == next)
    return;

  auto loc = BCFGen_->emitJmpLong(0);
  registerLongJump(loc, dst);
}
void HBCISel::generateReturnInst(ReturnInst *Inst, BasicBlock *next) {
  auto value = encodeValue(Inst->getValue());
  BCFGen_->emitRet(value);
}
void HBCISel::generateThrowInst(ThrowInst *Inst, BasicBlock *next) {
  BCFGen_->emitThrow(encodeValue(Inst->getThrownValue()));
}
void HBCISel::generateThrowIfInst(
    hermes::ThrowIfInst *Inst,
    hermes::BasicBlock *next) {
  assert(
      (Inst->getInvalidTypes()->getData().isEmptyType() ||
       Inst->getInvalidTypes()->getData().isUninitType()) &&
      "Only Empty/Undefined supported");
  if (Inst->getInvalidTypes()->getData().isEmptyType()) {
    BCFGen_->emitThrowIfEmpty(
        encodeValue(Inst), encodeValue(Inst->getCheckedValue()));
  } else {
    BCFGen_->emitThrowIfUndefined(
        encodeValue(Inst), encodeValue(Inst->getCheckedValue()));
  }
}
void HBCISel::generateThrowIfThisInitializedInst(
    hermes::ThrowIfThisInitializedInst *Inst,
    hermes::BasicBlock *next) {
  BCFGen_->emitThrowIfThisInitialized(
      encodeValue(Inst->getDerivedClassCheckedThis()));
}
void HBCISel::generateSwitchInst(SwitchInst *Inst, BasicBlock *next) {
  llvm_unreachable("SwitchInst should have been lowered");
}
void HBCISel::generateBaseSwitchImmInst(
    BaseSwitchImmInst *Inst,
    BasicBlock *next) {
  llvm_unreachable("Not a concrete instruction");
}
void HBCISel::generateSaveAndYieldInst(
    SaveAndYieldInst *Inst,
    BasicBlock *next) {
  llvm_unreachable("SaveAndYieldInst should have been lowered");
}
void HBCISel::generateCreateGeneratorInst(
    CreateGeneratorInst *Inst,
    BasicBlock *next) {
  auto env = encodeValue(Inst->getScope());
  auto output = encodeValue(Inst);
  auto code = BCFGen_->getFunctionID(Inst->getFunctionCode());
  if (LLVM_LIKELY(code <= UINT16_MAX)) {
    // Most of the cases, function index will be less than 2^16.
    BCFGen_->emitCreateGenerator(output, env, code);
  } else {
    BCFGen_->emitCreateGeneratorLongIndex(output, env, code);
  }
}
void HBCISel::generateResumeGeneratorInst(
    ResumeGeneratorInst *Inst,
    BasicBlock *next) {
  llvm_unreachable("ResumeGeneratorInst should have been lowered");
}

void HBCISel::generateCondBranchInst(CondBranchInst *Inst, BasicBlock *next) {
  auto condReg = encodeValue(Inst->getCondition());

  BasicBlock *trueBlock = Inst->getTrueDest();
  BasicBlock *falseBlock = Inst->getFalseDest();

  // Emit a conditional jump to the 'False' destination and a fall-through to
  // the 'True' side.
  if (next == trueBlock) {
    auto loc = BCFGen_->emitJmpFalseLong(0, condReg);
    registerLongJump(loc, falseBlock);
    return;
  }

  // Emit a conditional jump to the 'True' destination and a fall-through to the
  // 'False' side.
  auto loc = BCFGen_->emitJmpTrueLong(0, condReg);
  registerLongJump(loc, trueBlock);

  // Try to eliminate the branch by using a fall-through. If the destination
  // is also the basic block we'll generate next then no need to emit the
  // jmp.
  if (next == falseBlock) {
    return;
  }

  loc = BCFGen_->emitJmpLong(0);
  registerLongJump(loc, falseBlock);
}

void HBCISel::generateHBCCompareBranchInst(
    HBCCompareBranchInst *Inst,
    BasicBlock *next) {
  auto left = encodeValue(Inst->getLeftHandSide());
  auto right = encodeValue(Inst->getRightHandSide());

  bool isBothNumber = Inst->getLeftHandSide()->getType().isNumberType() &&
      Inst->getRightHandSide()->getType().isNumberType();

  BasicBlock *trueBlock = Inst->getTrueDest();
  BasicBlock *falseBlock = Inst->getFalseDest();

  bool invert = false;

  // If we need to fall-through to the "true" case, invert the condition and
  // the jump targets.
  if (next == trueBlock) {
    invert = true;
    std::swap(trueBlock, falseBlock);
  }

  offset_t loc;
  switch (Inst->getKind()) {
    case ValueKind::CmpBrLessThanInstKind: // <
      loc = invert ? (isBothNumber ? BCFGen_->emitJNotLessNLong(0, left, right)
                                   : BCFGen_->emitJNotLessLong(0, left, right))
                   : (isBothNumber ? BCFGen_->emitJLessNLong(0, left, right)
                                   : BCFGen_->emitJLessLong(0, left, right));
      break;
    case ValueKind::CmpBrLessThanOrEqualInstKind: // <=
      loc = invert
          ? (isBothNumber ? BCFGen_->emitJNotLessEqualNLong(0, left, right)
                          : BCFGen_->emitJNotLessEqualLong(0, left, right))
          : (isBothNumber ? BCFGen_->emitJLessEqualNLong(0, left, right)
                          : BCFGen_->emitJLessEqualLong(0, left, right));
      break;
    case ValueKind::CmpBrGreaterThanInstKind: // >
      loc = invert
          ? (isBothNumber ? BCFGen_->emitJNotLessNLong(0, right, left)
                          : BCFGen_->emitJNotGreaterLong(0, left, right))
          : (isBothNumber ? BCFGen_->emitJLessNLong(0, right, left)
                          : BCFGen_->emitJGreaterLong(0, left, right));
      break;
    case ValueKind::CmpBrGreaterThanOrEqualInstKind: // >=
      loc = invert
          ? (isBothNumber ? BCFGen_->emitJNotLessEqualNLong(0, right, left)
                          : BCFGen_->emitJNotGreaterEqualLong(0, left, right))
          : (isBothNumber ? BCFGen_->emitJLessEqualNLong(0, right, left)
                          : BCFGen_->emitJGreaterEqualLong(0, left, right));
      break;

    case ValueKind::CmpBrEqualInstKind:
      loc = invert ? BCFGen_->emitJNotEqualLong(0, left, right)
                   : BCFGen_->emitJEqualLong(0, left, right);
      break;

    case ValueKind::CmpBrNotEqualInstKind:
      loc = invert ? BCFGen_->emitJEqualLong(0, left, right)
                   : BCFGen_->emitJNotEqualLong(0, left, right);
      break;

    case ValueKind::CmpBrStrictlyEqualInstKind:
      loc = invert ? BCFGen_->emitJStrictNotEqualLong(0, left, right)
                   : BCFGen_->emitJStrictEqualLong(0, left, right);
      break;

    case ValueKind::CmpBrStrictlyNotEqualInstKind:
      loc = invert ? BCFGen_->emitJStrictEqualLong(0, left, right)
                   : BCFGen_->emitJStrictNotEqualLong(0, left, right);
      break;

    default:
      llvm_unreachable("invalid compare+branch operator");
  }

  registerLongJump(loc, trueBlock);

  // Try to eliminate the branch by using a fall-through. If the destination
  // is also the basic block we'll generate next then no need to emit the
  // jmp.
  if (next == falseBlock) {
    return;
  }

  loc = BCFGen_->emitJmpLong(0);
  registerLongJump(loc, falseBlock);
}
void HBCISel::generateGetPNamesInst(GetPNamesInst *Inst, BasicBlock *next) {
  auto itrReg = encodeValue(Inst->getIterator());
  BCFGen_->emitGetPNameList(
      itrReg,
      encodeValue(Inst->getBase()),
      encodeValue(Inst->getIndex()),
      encodeValue(Inst->getSize()));
  // If the returned iterator is empty value, quit the iteration directly.
  registerLongJump(
      BCFGen_->emitJmpUndefinedLong(0, itrReg), Inst->getOnEmptyDest());

  auto *onSomeBlock = Inst->getOnSomeDest();
  if (next != onSomeBlock) {
    auto loc = BCFGen_->emitJmpLong(0);
    registerLongJump(loc, onSomeBlock);
  }
}
void HBCISel::generateHBCCmpBrTypeOfIsInst(
    HBCCmpBrTypeOfIsInst *Inst,
    BasicBlock *next) {
  auto arg = encodeValue(Inst->getArgument());
  TypeOfIsTypes types = Inst->getTypes()->getData();
  BasicBlock *trueBlock = Inst->getTrueDest();
  BasicBlock *falseBlock = Inst->getFalseDest();
  if (next == trueBlock) {
    // If the next block is the true block, invert the condition so we can
    // fall through (only a single instruction should be necessary).
    types = types.invert();
    std::swap(trueBlock, falseBlock);
  }
  registerLongJump(BCFGen_->emitJmpTypeOfIs(0, arg, types.getRaw()), trueBlock);
  if (next != falseBlock) {
    registerLongJump(BCFGen_->emitJmpLong(0), falseBlock);
  }
}
void HBCISel::generateGetNextPNameInst(
    GetNextPNameInst *Inst,
    BasicBlock *next) {
  auto indexReg = encodeValue(Inst->getIndexAddr());
  auto sizeReg = encodeValue(Inst->getSizeAddr());
  auto propReg = encodeValue(Inst->getPropertyAddr());

  BCFGen_->emitGetNextPName(
      propReg,
      encodeValue(Inst->getIteratorAddr()),
      encodeValue(Inst->getBaseAddr()),
      indexReg,
      sizeReg);

  // If the returned property is empty, terminate the iteration.
  registerLongJump(
      BCFGen_->emitJmpUndefinedLong(0, propReg), Inst->getOnLastDest());

  auto *onSomeBlock = Inst->getOnSomeDest();
  if (next != onSomeBlock) {
    auto loc = BCFGen_->emitJmpLong(0);
    registerLongJump(loc, onSomeBlock);
  }
}
void HBCISel::generateTryStartInst(TryStartInst *Inst, BasicBlock *next) {
  // TryStartInst is the same as BranchInst in bytecode gen.
  BasicBlock *destination = Inst->getTryBody();

  assert(
      Inst->getParent()->getTerminator() == Inst &&
      "BI is not the terminator!");

  // Try to eliminate the branch by using a fall-through. If the destination
  // is also the basic block we'll generate next then no need to emit the
  // jmp.
  if (next == destination) {
    return;
  }

  // Emit the jump and remember to update the relocation location.
  auto loc = BCFGen_->emitJmpLong(0);
  registerLongJump(loc, destination);
}
void HBCISel::generateCallInst(CallInst *Inst, BasicBlock *next) {
  // Handle Metro require calls specially.
  if (Inst->getAttributes(Inst->getModule()).isMetroRequire) {
    auto *litNum = llvh::cast<LiteralNumber>(Inst->getArgument(1));
    assert(
        litNum->isUInt32Representible() &&
        "Or should not have been optimized.");
    BCFGen_->emitCallRequire(
        encodeValue(Inst), encodeValue(Inst->getCallee()), litNum->asUInt32());
    return;
  }

  auto output = encodeValue(Inst);
  auto function = encodeValue(Inst->getCallee());
  bool newTargetIsUndefined = llvh::isa<LiteralUndefined>(Inst->getNewTarget());
  assert(
      Inst->getNumArguments() <= UINT8_MAX &&
      "CallInst doesn't support large number of args");
  verifyCall(Inst);

  if (newTargetIsUndefined) {
    BCFGen_->emitCall(output, function, Inst->getNumArguments());
  } else {
    auto newTarget = encodeValue(Inst->getNewTarget());
    if (function == newTarget) {
      BCFGen_->emitConstruct(output, function, Inst->getNumArguments());
    } else {
      BCFGen_->emitCallWithNewTarget(
          output, function, newTarget, Inst->getNumArguments());
    }
  }
}

void HBCISel::generateHBCCallWithArgCountInst(
    HBCCallWithArgCountInst *Inst,
    BasicBlock *next) {
  auto output = encodeValue(Inst);
  auto function = encodeValue(Inst->getCallee());
  auto newTarget = encodeValue(Inst->getNewTarget());
  auto argCount = encodeValue(Inst->getNumArgumentsLiteral());
  BCFGen_->emitCallWithNewTargetLong(output, function, newTarget, argCount);
}

void HBCISel::generateHBCCallNInst(HBCCallNInst *Inst, BasicBlock *next) {
  auto output = encodeValue(Inst);
  auto function = encodeValue(Inst->getCallee());
  verifyCall(Inst);

  static_assert(
      HBCCallNInst::kMinArgs == 1 && HBCCallNInst::kMaxArgs == 4,
      "Update generateHBCCallNInst to reflect min/max arg range");

  switch (Inst->getNumArguments()) {
    case 1:
      BCFGen_->emitCall1(output, function, encodeValue(Inst->getArgument(0)));
      break;
    case 2:
      BCFGen_->emitCall2(
          output,
          function,
          encodeValue(Inst->getArgument(0)),
          encodeValue(Inst->getArgument(1)));
      break;
    case 3:
      BCFGen_->emitCall3(
          output,
          function,
          encodeValue(Inst->getArgument(0)),
          encodeValue(Inst->getArgument(1)),
          encodeValue(Inst->getArgument(2)));
      break;
    case 4:
      BCFGen_->emitCall4(
          output,
          function,
          encodeValue(Inst->getArgument(0)),
          encodeValue(Inst->getArgument(1)),
          encodeValue(Inst->getArgument(2)),
          encodeValue(Inst->getArgument(3)));
      break;
    default:
      llvm_unreachable("Unexpected argument count");
  }
}

void HBCISel::generateCallBuiltinInst(CallBuiltinInst *Inst, BasicBlock *next) {
  auto output = encodeValue(Inst);
  verifyCall(Inst);

  if (Inst->getNumArguments() <= UINT8_MAX) {
    BCFGen_->emitCallBuiltin(
        output, Inst->getBuiltinIndex(), Inst->getNumArguments());
  } else {
    BCFGen_->emitCallBuiltinLong(
        output, Inst->getBuiltinIndex(), Inst->getNumArguments());
  }
}

void HBCISel::generateGetBuiltinClosureInst(
    GetBuiltinClosureInst *Inst,
    BasicBlock *next) {
  auto output = encodeValue(Inst);
  BCFGen_->emitGetBuiltinClosure(output, Inst->getBuiltinIndex());
}

void HBCISel::generateGetParentScopeInst(
    GetParentScopeInst *Inst,
    hermes::BasicBlock *next) {
  BCFGen_->emitGetParentEnvironment(encodeValue(Inst), 0);
}

void HBCISel::generateResolveScopeInst(
    ResolveScopeInst *Inst,
    BasicBlock *next) {
  llvm_unreachable("ResolveScopeInst should have been lowered.");
}

void HBCISel::generateLIRResolveScopeInst(
    hermes::LIRResolveScopeInst *Inst,
    hermes::BasicBlock *next) {
  auto delta = Inst->getNumLevels()->asUInt32();
  if (std::numeric_limits<uint8_t>::max() < delta) {
    F_->getContext().getSourceErrorManager().error(
        Inst->getLocation(), "Variable environment is out-of-reach");
  }
  BCFGen_->emitGetEnvironment(
      encodeValue(Inst), encodeValue(Inst->getStartScope()), delta);
}

void HBCISel::generateHBCResolveParentEnvironmentInst(
    HBCResolveParentEnvironmentInst *Inst,
    BasicBlock *next) {
  auto delta = Inst->getNumLevels()->asUInt32();
  if (std::numeric_limits<uint8_t>::max() < delta) {
    F_->getContext().getSourceErrorManager().error(
        Inst->getLocation(), "Variable environment is out-of-reach");
  }
  BCFGen_->emitGetParentEnvironment(encodeValue(Inst), delta);
}

void HBCISel::generateGetClosureScopeInst(
    hermes::GetClosureScopeInst *Inst,
    hermes::BasicBlock *next) {
  auto output = encodeValue(Inst);
  auto closure = encodeValue(Inst->getClosure());
  BCFGen_->emitGetClosureEnvironment(output, closure);
}

void HBCISel::generateLIRLoadConstInst(
    hermes::LIRLoadConstInst *Inst,
    hermes::BasicBlock *next) {
  auto output = encodeValue(Inst);
  Literal *literal = Inst->getConst();
  switch (literal->getKind()) {
    case ValueKind::LiteralEmptyKind:
      BCFGen_->emitLoadConstEmpty(output);
      break;
    // Uninit shares undefined's runtime representation.
    case ValueKind::LiteralUninitKind:
    case ValueKind::LiteralUndefinedKind:
      BCFGen_->emitLoadConstUndefined(output);
      break;
    case ValueKind::LiteralNullKind:
      BCFGen_->emitLoadConstNull(output);
      break;
    case ValueKind::LiteralBoolKind: {
      auto *litBool = cast<LiteralBool>(literal);
      if (litBool->getValue()) {
        BCFGen_->emitLoadConstTrue(output);
      } else {
        BCFGen_->emitLoadConstFalse(output);
      }
      break;
    }
    case ValueKind::LiteralNumberKind: {
      auto *litNum = cast<LiteralNumber>(literal);
      // Check for +0.0.
      if (litNum->isPositiveZero()) {
        BCFGen_->emitLoadConstZero(output);
      } else {
        if (litNum->isUInt8Representible()) {
          BCFGen_->emitLoadConstUInt8(output, litNum->asUInt8());
        } else if (litNum->isInt32Representible()) {
          BCFGen_->emitLoadConstInt(output, litNum->asInt32());
        } else {
          // param_t is int64_t, we cannot directly convert a double into that.
          // Instead we are going to copy it as if it is binary.
          BCFGen_->emitLoadConstDoubleDirect(output, litNum->getValue());
        }
      }
      break;
    }
    case ValueKind::LiteralBigIntKind: {
      auto parsedBigInt = bigint::ParsedBigInt::parsedBigIntFromNumericValue(
          cast<LiteralBigInt>(literal)->getValue()->str());
      assert(parsedBigInt && "should be valid");
      if (bigint::tooManyBytes(parsedBigInt->getBytes().size())) {
        // TODO: move this to the semantic analysis so we can get a proper
        // warning (i.e., with the correct location for the literal).
        std::string sizeStr;
        {
          llvh::raw_string_ostream OS(sizeStr);
          OS << parsedBigInt->getBytes().size();
        }
        F_->getContext().getSourceErrorManager().warning(
            Inst->getLocation(),
            Twine("BigInt literal has too many bytes (") + sizeStr +
                ") and a RangeError will be raised at runtime time if it "
                "is referenced.");
      }
      auto idx = BCFGen_->addBigInt(std::move(*parsedBigInt));
      if (idx <= UINT16_MAX) {
        BCFGen_->emitLoadConstBigInt(output, idx);
      } else {
        BCFGen_->emitLoadConstBigIntLongIndex(output, idx);
      }
      break;
    }
    case ValueKind::LiteralStringKind: {
      auto idx = BCFGen_->getStringID(cast<LiteralString>(literal));
      if (idx <= UINT16_MAX) {
        BCFGen_->emitLoadConstString(output, idx);
      } else {
        BCFGen_->emitLoadConstStringLongIndex(output, idx);
      }
      break;
    }
    default:
      llvm_unreachable("Invalid literal type");
  }
}
void HBCISel::generateLoadParamInst(
    hermes::LoadParamInst *Inst,
    hermes::BasicBlock *next) {
  auto output = encodeValue(Inst);
  uint32_t value = Inst->getParam()->getIndexInParamList();
  if (value <= UINT8_MAX) {
    BCFGen_->emitLoadParam(output, value);
  } else {
    BCFGen_->emitLoadParamLong(output, value);
  }
}

void HBCISel::generateCreateScopeInst(
    hermes::CreateScopeInst *Inst,
    hermes::BasicBlock *next) {
  auto dstReg = encodeValue(Inst);
  Value *parent = Inst->getParentScope();
  size_t scopeSize = Inst->getVariableScope()->getVariables().size();
  // Check if this is the top level scope.
  if (llvh::isa<EmptySentinel>(parent)) {
    BCFGen_->emitCreateTopLevelEnvironment(dstReg, scopeSize);
  } else {
    BCFGen_->emitCreateEnvironment(
        dstReg,
        encodeValue(parent),
        Inst->getVariableScope()->getVariables().size());
  }
}

void HBCISel::generateHBCCreateFunctionEnvironmentInst(
    hermes::HBCCreateFunctionEnvironmentInst *Inst,
    hermes::BasicBlock *next) {
  auto dstReg = encodeValue(Inst);
  static_assert(
      HBCCreateFunctionEnvironmentInst::kMaxScopeSize == UINT8_MAX,
      "IR max size does not match bytecode");
  BCFGen_->emitCreateFunctionEnvironment(
      dstReg, Inst->getVariableScope()->getVariables().size());
}

void HBCISel::generateHBCProfilePointInst(
    hermes::HBCProfilePointInst *Inst,
    hermes::BasicBlock *next) {
  BCFGen_->emitProfilePoint(Inst->getPointIndex());
}

void HBCISel::generateLIRGetGlobalObjectInst(
    hermes::LIRGetGlobalObjectInst *Inst,
    hermes::BasicBlock *next) {
  auto dstReg = encodeValue(Inst);
  BCFGen_->emitGetGlobalObject(dstReg);
}

void HBCISel::generateGetNewTargetInst(
    hermes::GetNewTargetInst *Inst,
    hermes::BasicBlock *next) {
  auto dstReg = encodeValue(Inst);
  BCFGen_->emitGetNewTarget(dstReg);
}

void HBCISel::generateLIRGetThisNSInst(
    hermes::LIRGetThisNSInst *Inst,
    hermes::BasicBlock *next) {
  auto dstReg = encodeValue(Inst);
  BCFGen_->emitLoadThisNS(dstReg);
}
void HBCISel::generateCoerceThisNSInst(
    CoerceThisNSInst *Inst,
    BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  BCFGen_->emitCoerceThisNS(dst, src);
}
void HBCISel::generateLIRGetArgumentsLengthInst(
    hermes::LIRGetArgumentsLengthInst *Inst,
    hermes::BasicBlock *next) {
  auto output = encodeValue(Inst);
  auto reg = encodeValue(Inst->getLazyRegister());
  BCFGen_->emitGetArgumentsLength(output, reg);
}
void HBCISel::generateLIRGetArgumentsPropByValLooseInst(
    hermes::LIRGetArgumentsPropByValLooseInst *Inst,
    hermes::BasicBlock *next) {
  auto output = encodeValue(Inst);
  auto index = encodeValue(Inst->getIndex());
  auto reg = encodeValue(Inst->getLazyRegister());
  BCFGen_->emitGetArgumentsPropByValLoose(output, index, reg);
}
void HBCISel::generateLIRGetArgumentsPropByValStrictInst(
    hermes::LIRGetArgumentsPropByValStrictInst *Inst,
    hermes::BasicBlock *next) {
  auto output = encodeValue(Inst);
  auto index = encodeValue(Inst->getIndex());
  auto reg = encodeValue(Inst->getLazyRegister());
  BCFGen_->emitGetArgumentsPropByValStrict(output, index, reg);
}
void HBCISel::generateLIRReifyArgumentsLooseInst(
    hermes::LIRReifyArgumentsLooseInst *Inst,
    hermes::BasicBlock *next) {
  auto reg = encodeValue(Inst->getLazyRegister());
  BCFGen_->emitReifyArgumentsLoose(reg);
}
void HBCISel::generateLIRReifyArgumentsStrictInst(
    hermes::LIRReifyArgumentsStrictInst *Inst,
    hermes::BasicBlock *next) {
  auto reg = encodeValue(Inst->getLazyRegister());
  BCFGen_->emitReifyArgumentsStrict(reg);
}
void HBCISel::generateCreateThisInst(CreateThisInst *Inst, BasicBlock *next) {
  auto output = encodeValue(Inst);
  auto closure = encodeValue(Inst->getClosure());
  LiteralBufferBuilder::LiteralOffset literalOffset =
      BCFGen_->getBytecodeModuleGenerator().serializedLiteralOffsetFor(Inst);
  uint32_t shapeTableIdx = literalOffset.shapeTableIdx <= UINT16_MAX
      ? literalOffset.shapeTableIdx
      : hbc::SHAPE_TABLE_CACHING_DISABLED;
  if (Inst->getNewTarget() == Inst->getClosure()) {
    BCFGen_->emitCreateThisForNew(
        output,
        closure,
        acquirePropertyReadCacheIndex(prototypeIdent_),
        shapeTableIdx);
  } else {
    auto newTarget = encodeValue(Inst->getNewTarget());
    BCFGen_->emitCreateThisForSuper(
        output,
        closure,
        newTarget,
        acquirePropertyReadCacheIndex(prototypeIdent_),
        shapeTableIdx);
  }
}
void HBCISel::generateGetConstructedObjectInst(
    GetConstructedObjectInst *Inst,
    BasicBlock *next) {
  auto output = encodeValue(Inst);
  auto thisValue = encodeValue(Inst->getThisValue());
  auto constructed = encodeValue(Inst->getConstructorReturnValue());
  BCFGen_->emitSelectObject(output, thisValue, constructed);
}
void HBCISel::generateLIRSpillMovInst(LIRSpillMovInst *Inst, BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getValue());
  emitMovIfNeeded(dst, src);
}
void HBCISel::generateUnreachableInst(
    hermes::UnreachableInst *Inst,
    hermes::BasicBlock *next) {
  if (bytecodeGenerationOptions_.emitAsserts) {
    BCFGen_->emitUnreachable();
  }
}

void HBCISel::generateIteratorBeginInst(
    hermes::IteratorBeginInst *Inst,
    hermes::BasicBlock *next) {
  auto iter = encodeValue(Inst);
  auto src = encodeValue(Inst->getSourceOrNext());
  BCFGen_->emitIteratorBegin(iter, src);
}

void HBCISel::generateIteratorNextInst(
    hermes::IteratorNextInst *Inst,
    hermes::BasicBlock *next) {
  auto dst = encodeValue(Inst);
  auto iter = encodeValue(Inst->getIterator());
  auto src = encodeValue(Inst->getSourceOrNext());
  BCFGen_->emitIteratorNext(dst, iter, src);
}

void HBCISel::generateIteratorCloseInst(
    hermes::IteratorCloseInst *Inst,
    hermes::BasicBlock *next) {
  auto iter = encodeValue(Inst->getIterator());
  bool ignoreInnerException = Inst->getIgnoreInnerException();
  BCFGen_->emitIteratorClose(iter, ignoreInnerException);
}

void HBCISel::generateCreateClassInst(CreateClassInst *Inst, BasicBlock *next) {
  auto classOut = encodeValue(Inst);
  auto homeObjOut = encodeValue(Inst->getHomeObjectOutput());
  auto env = encodeValue(Inst->getScope());
  auto code = BCFGen_->getFunctionID(Inst->getFunctionCode());
  bool isBaseClass = llvh::isa<EmptySentinel>(Inst->getSuperClass());
  if (isBaseClass) {
    if (LLVM_LIKELY(code <= UINT16_MAX)) {
      BCFGen_->emitCreateBaseClass(classOut, homeObjOut, env, code);
    } else {
      BCFGen_->emitCreateBaseClassLongIndex(classOut, homeObjOut, env, code);
    }
  } else {
    auto super = encodeValue(Inst->getSuperClass());
    if (LLVM_LIKELY(code <= UINT16_MAX)) {
      BCFGen_->emitCreateDerivedClass(classOut, homeObjOut, env, super, code);
    } else {
      BCFGen_->emitCreateDerivedClassLongIndex(
          classOut, homeObjOut, env, super, code);
    }
  }
}

void HBCISel::generateUIntSwitchImmInst(
    hermes::UIntSwitchImmInst *Inst,
    hermes::BasicBlock *next) {
  uint32_t min = Inst->getMinValue();
  uint32_t size = Inst->getSize();

  uint32_t max = min + size - 1;

  std::vector<BasicBlock *> jmpTable;
  jmpTable.resize(size);

  // fill jump table entries in use.
  for (uint32_t caseIdx = 0; caseIdx < Inst->getNumCasePair(); caseIdx++) {
    auto casePair = Inst->getCasePair(caseIdx);
    uint32_t val = casePair.first->asUInt32();
    BasicBlock *target = casePair.second;
    jmpTable[val - min] = target;
  }

  for (uint32_t idx = 0; idx < size; idx++) {
    if (jmpTable[idx] == nullptr)
      jmpTable[idx] = Inst->getDefaultDestination();
  }

  registerUIntSwitchImm(
      BCFGen_->emitUIntSwitchImm(
          encodeValue(Inst->getInputValue()), 0, 0, min, max),
      Inst);
  uintSwitchImmInfo_[Inst] = {0, Inst->getDefaultDestination(), jmpTable};
}

void HBCISel::generateStringSwitchImmInst(
    hermes::StringSwitchImmInst *Inst,
    hermes::BasicBlock *next) {
  uint32_t size = Inst->getSize();

  std::vector<StringSwitchCase> stringSwitchTable;
  stringSwitchTable.resize(size);

  // fill string switch table entries.
  for (uint32_t caseIdx = 0; caseIdx < Inst->getNumCasePair(); caseIdx++) {
    auto casePair = Inst->getCasePair(caseIdx);
    auto &caseInTable = stringSwitchTable[caseIdx];
    caseInTable.label = BCFGen_->getStringID(casePair.first);
    caseInTable.target = casePair.second;
  }

  registerStringSwitchImm(
      BCFGen_->emitStringSwitchImm(
          encodeValue(Inst->getInputValue()),
          BCFGen_->getBytecodeModuleGenerator().addStringSwitchImmInstr(),
          0,
          0,
          stringSwitchTable.size()),
      Inst);
  stringSwitchImmInfo_[Inst] = {
      0, Inst->getDefaultDestination(), stringSwitchTable};
}

void HBCISel::generateLoadParentNoTrapsInst(
    LoadParentNoTrapsInst *Inst,
    BasicBlock *) {
  auto resultReg = encodeValue(Inst);
  auto objReg = encodeValue(Inst->getObject());
  BCFGen_->emitLoadParentNoTraps(resultReg, objReg);
}

void HBCISel::generatePrLoadInst(PrLoadInst *inst, BasicBlock *) {
  auto objReg = encodeValue(inst->getObject());
  auto resReg = encodeValue(inst);
  if (inst->getPropIndex() <= UINT8_MAX) {
    BCFGen_->emitGetOwnBySlotIdx(resReg, objReg, inst->getPropIndex());
  } else {
    BCFGen_->emitGetOwnBySlotIdxLong(resReg, objReg, inst->getPropIndex());
  }
}
void HBCISel::generatePrStoreInst(PrStoreInst *Inst, BasicBlock *) {
  auto valueReg = encodeValue(Inst->getOperand(PrStoreInst::StoredValueIdx));
  auto objReg = encodeValue(Inst->getObject());
  if (Inst->getPropIndex() <= UINT8_MAX) {
    BCFGen_->emitPutOwnBySlotIdx(objReg, valueReg, Inst->getPropIndex());
  } else {
    BCFGen_->emitPutOwnBySlotIdxLong(objReg, valueReg, Inst->getPropIndex());
  }
}

void HBCISel::generateFCompareInst(FCompareInst *Inst, BasicBlock *) {
  auto left = encodeValue(Inst->getLeft());
  auto right = encodeValue(Inst->getRight());
  auto res = encodeValue(Inst);
  switch (Inst->getKind()) {
    case ValueKind::FEqualInstKind:
      BCFGen_->emitStrictEq(res, left, right);
      break;
    case ValueKind::FNotEqualInstKind:
      BCFGen_->emitStrictNeq(res, left, right);
      break;
    case ValueKind::FLessThanInstKind:
      BCFGen_->emitLess(res, left, right);
      break;
    case ValueKind::FLessThanOrEqualInstKind:
      BCFGen_->emitLessEq(res, left, right);
      break;
    case ValueKind::FGreaterThanInstKind:
      BCFGen_->emitGreater(res, left, right);
      break;
    case ValueKind::FGreaterThanOrEqualInstKind:
      BCFGen_->emitGreaterEq(res, left, right);
      break;
    default:
      hermes_fatal("invalid kind for FCompareInst");
  }
}
void HBCISel::generateHBCFCompareBranchInst(
    HBCFCompareBranchInst *Inst,
    BasicBlock *next) {
  auto left = encodeValue(Inst->getLeftHandSide());
  auto right = encodeValue(Inst->getRightHandSide());

  BasicBlock *trueBlock = Inst->getTrueDest();
  BasicBlock *falseBlock = Inst->getFalseDest();

  bool invert = false;

  // If we need to fall-through to the "true" case, invert the condition and
  // the jump targets.
  if (next == trueBlock) {
    invert = true;
    std::swap(trueBlock, falseBlock);
  }

  offset_t loc;
  switch (Inst->getKind()) {
    case ValueKind::HBCFCmpBrEqualInstKind:
      loc = invert ? BCFGen_->emitJStrictNotEqualLong(0, left, right)
                   : BCFGen_->emitJStrictEqualLong(0, left, right);
      break;
    case ValueKind::HBCFCmpBrNotEqualInstKind:
      loc = invert ? BCFGen_->emitJStrictEqualLong(0, left, right)
                   : BCFGen_->emitJStrictNotEqualLong(0, left, right);
      break;
    case ValueKind::HBCFCmpBrLessThanInstKind:
      loc = invert ? BCFGen_->emitJNotLessNLong(0, left, right)
                   : BCFGen_->emitJLessNLong(0, left, right);
      break;
    case ValueKind::HBCFCmpBrLessThanOrEqualInstKind:
      loc = invert ? BCFGen_->emitJNotLessEqualNLong(0, left, right)
                   : BCFGen_->emitJLessEqualNLong(0, left, right);
      break;
    case ValueKind::HBCFCmpBrGreaterThanInstKind:
      loc = invert ? BCFGen_->emitJNotLessNLong(0, right, left)
                   : BCFGen_->emitJLessNLong(0, right, left);
      break;
    case ValueKind::HBCFCmpBrGreaterThanOrEqualInstKind:
      loc = invert ? BCFGen_->emitJNotLessEqualNLong(0, right, left)
                   : BCFGen_->emitJLessEqualNLong(0, right, left);
      break;
    default:
      hermes_fatal("invalid kind for FCompareBranchInst");
  }

  registerLongJump(loc, trueBlock);

  if (next == falseBlock)
    return;

  loc = BCFGen_->emitJmpLong(0);
  registerLongJump(loc, falseBlock);
}
void HBCISel::generateFBinaryMathInst(FBinaryMathInst *Inst, BasicBlock *) {
  auto left = encodeValue(Inst->getLeft());
  auto right = encodeValue(Inst->getRight());
  auto res = encodeValue(Inst);
  switch (Inst->getKind()) {
    case ValueKind::FAddInstKind:
      BCFGen_->emitAddN(res, left, right);
      break;
    case ValueKind::FSubtractInstKind:
      BCFGen_->emitSubN(res, left, right);
      break;
    case ValueKind::FMultiplyInstKind:
      BCFGen_->emitMulN(res, left, right);
      break;
    case ValueKind::FDivideInstKind:
      BCFGen_->emitDivN(res, left, right);
      break;
    case ValueKind::FModuloInstKind:
      BCFGen_->emitMod(res, left, right);
      break;
    default:
      hermes_fatal("invalid kind for FBinaryMathInst");
  }
}
void HBCISel::generateFUnaryMathInst(FUnaryMathInst *Inst, BasicBlock *) {
  auto opReg = encodeValue(Inst->getArg());
  auto resReg = encodeValue(Inst);
  switch (Inst->getKind()) {
    case ValueKind::FNegateKind:
      BCFGen_->emitNegate(resReg, opReg);
      break;
    default:
      hermes_fatal("invalid kind for FUnaryMathInst");
  }
}
void HBCISel::generateTypedLoadParentInst(
    TypedLoadParentInst *Inst,
    BasicBlock *) {
  auto object = encodeValue(Inst->getObject());
  auto res = encodeValue(Inst);
  BCFGen_->emitTypedLoadParent(res, object);
}
void HBCISel::generateNativeCallInst(NativeCallInst *, BasicBlock *) {
  hermes_fatal("NativeCallInst not supported.");
}
void HBCISel::generateGetNativeRuntimeInst(
    hermes::GetNativeRuntimeInst *Inst,
    hermes::BasicBlock *next) {
  hermes_fatal("GetNativeRuntimeInst not supported.");
}
void HBCISel::generateLIRDeadValueInst(LIRDeadValueInst *, BasicBlock *) {
  hermes_fatal("LIRDeadValueInst not supported.");
}
void HBCISel::generateFastArrayLoadInst(FastArrayLoadInst *inst, BasicBlock *) {
  BCFGen_->emitFastArrayLoad(
      encodeValue(inst),
      encodeValue(inst->getArray()),
      encodeValue(inst->getIndex()));
}
void HBCISel::generateFastArrayPushInst(FastArrayPushInst *inst, BasicBlock *) {
  BCFGen_->emitFastArrayPush(
      encodeValue(inst->getArray()), encodeValue(inst->getPushedValue()));
}
void HBCISel::generateAllocFastArrayInst(
    AllocFastArrayInst *inst,
    BasicBlock *) {
  BCFGen_->emitNewFastArray(
      encodeValue(inst),
      encodeValue(inst->getPrototype()),
      std::min<uint32_t>(inst->getCapacity()->asUInt32(), UINT16_MAX));
}
void HBCISel::generateFastArrayStoreInst(
    FastArrayStoreInst *inst,
    BasicBlock *) {
  BCFGen_->emitFastArrayStore(
      encodeValue(inst->getArray()),
      encodeValue(inst->getIndex()),
      encodeValue(inst->getStoredValue()));
}
void HBCISel::generateThrowTypeErrorInst(ThrowTypeErrorInst *, BasicBlock *) {
  hermes_fatal("ThrowTypeErrorInst should have been lowered.");
}
void HBCISel::generateCheckedTypeCastInst(CheckedTypeCastInst *, BasicBlock *) {
  hermes_fatal("CheckedTypeCastInst should have been lowered.");
}
void HBCISel::generateFastArrayAppendInst(
    FastArrayAppendInst *inst,
    BasicBlock *) {
  BCFGen_->emitFastArrayAppend(
      encodeValue(inst->getArray()), encodeValue(inst->getOther()));
}
void HBCISel::generateFastArrayLengthInst(
    FastArrayLengthInst *inst,
    BasicBlock *) {
  BCFGen_->emitFastArrayLength(
      encodeValue(inst), encodeValue(inst->getArray()));
}
void HBCISel::generateUnionNarrowTrustedInst(
    UnionNarrowTrustedInst *Inst,
    BasicBlock *) {
  auto dst = encodeValue(Inst);
  auto src = encodeValue(Inst->getSingleOperand());
  emitMovIfNeeded(dst, src);
}
void HBCISel::generateCreateArgumentsLooseInst(
    CreateArgumentsLooseInst *,
    BasicBlock *) {
  hermes_fatal("CreateArgumentsLooseInst not supported.");
}
void HBCISel::generateCreateArgumentsStrictInst(
    CreateArgumentsStrictInst *,
    BasicBlock *) {
  hermes_fatal("CreateArgumentsStrictInst not supported.");
}
void HBCISel::generateStringConcatInst(StringConcatInst *inst, BasicBlock *) {
  hermes_fatal("StringConcatInst should be lowered.");
}
void HBCISel::generateHBCStringConcatInst(
    HBCStringConcatInst *inst,
    BasicBlock *) {
  BCFGen_->emitAddS(
      encodeValue(inst),
      encodeValue(inst->getLeft()),
      encodeValue(inst->getRight()));
}
void HBCISel::generateLazyCompilationDataInst(
    LazyCompilationDataInst *,
    BasicBlock *) {
  // Nothing to do, this is metadata.
}
void HBCISel::generateEvalCompilationDataInst(
    EvalCompilationDataInst *,
    BasicBlock *) {
  // Nothing to do, this is metadata.
}

void HBCISel::generateBB(BasicBlock *BB, BasicBlock *next) {
  // Register the address of the current basic block.
  auto begin_loc = BCFGen_->getCurrentLocation();

  // It is important to register the basic block before processing it,
  // as we require the relocations to be sorted by their offset.
  relocations_.push_back(
      {begin_loc, Relocation::RelocationType::BasicBlockType, BB});
  basicBlockMap_[BB] = std::make_pair(begin_loc, next);

  // Emit an async break check before the terminator if necessary.
  // We do this at the end of the block so that we come after any
  // CreateEnvironment instruction.
  const Instruction *asyncBreakCheckLoc =
      asyncBreakChecks_.count(BB) ? BB->getTerminator() : nullptr;
  for (auto &I : *BB) {
    if (&I == asyncBreakCheckLoc) {
      BCFGen_->emitAsyncBreakCheck();
    }
    generateInst(&I, next);
  }
  auto end_loc = BCFGen_->getCurrentLocation();
  if (!next) {
    // When next is nullptr, we are hitting the last BB.
    // We should also register that null BB with it's location.
    assert(
        basicBlockMap_.find(nullptr) == basicBlockMap_.end() &&
        "Multiple nullptr BBs encountered");
    basicBlockMap_[nullptr] = std::make_pair(end_loc, nullptr);
  }

  LLVM_DEBUG(
      llvh::dbgs() << "Generated the block " << BB << " from " << begin_loc
                   << " .. " << end_loc << "\n");
}

void HBCISel::generateInst(Instruction *ii, BasicBlock *next) {
  LLVM_DEBUG(
      llvh::dbgs() << "Generating the instruction " << ii->getName() << "\n");

  // Generate the debug info.
  switch (F_->getContext().getDebugInfoSetting()) {
    case DebugInfoSetting::NONE:
      break;
    case DebugInfoSetting::THROWING:
      if (!ii->getSideEffect().getThrow()) {
        break;
      }
      // Falls through - if ii can execute.
      [[fallthrough]];
    case DebugInfoSetting::SOURCE_MAP:
    case DebugInfoSetting::ALL:
      // Check the case where the instruction has no location, but is the
      // first instruction in the block.
      // We don't want the previous block's info to run over into this block,
      // because it could be anything.
      // When emitting the RelocationType::DebugInfo in
      // addDebugSourceLocationInfo, we'll avoid emitting no location
      // instructions if we haven't emitted any locations yet.
      if (ii->hasLocation() || &ii->getParent()->front() == ii) {
        relocations_.push_back(
            {BCFGen_->getCurrentLocation(),
             Relocation::RelocationType::DebugInfo,
             ii});
        break;
      }
  }

  switch (ii->getKind()) {
#define DEF_VALUE(CLASS, PARENT) \
  case ValueKind::CLASS##Kind:   \
    return generate##CLASS(cast<CLASS>(ii), next);
#define DEF_TAG(NAME, PARENT) \
  case ValueKind::NAME##Kind: \
    return generate##PARENT(cast<PARENT>(ii), next);
#include "hermes/IR/Instrs.def"

    default:
      llvm_unreachable("Invalid kind");
  }
}

void HBCISel::run() {
  auto PO = postOrderAnalysis(F_);

  /// The order of the blocks is reverse-post-order, which is a simply
  /// topological sort.
  llvh::SmallVector<BasicBlock *, 16> order(PO.rbegin(), PO.rend());

  // If we are compiling with debugger or otherwise need async break checks,
  // decide which blocks need runtime async break checks: blocks with backwards
  // jumps, and the first block if this is a function (i.e. not the
  // global scope).
  if (F_->getContext().getDebugInfoSetting() == DebugInfoSetting::ALL ||
      F_->getContext().getEmitAsyncBreakCheck()) {
    asyncBreakChecks_ = basicBlocksWithBackwardSuccessors(order);
    asyncBreakChecks_.insert(order.front());
  }

  for (int i = 0, e = order.size(); i < e; ++i) {
    BasicBlock *BB = order[i];
    BasicBlock *next = ((i + 1) == e) ? nullptr : order[i + 1];
    LLVM_DEBUG(
        llvh::dbgs() << "Generating bytecode for basic block " << BB << "\n");
    generateBB(BB, next);
  }

  resolveRelocations();
  resolveExceptionHandlers();
  addDebugSourceLocationInfo();
  generateJumpTable();
  generateStringSwitchTable();
  addDebugLexicalInfo();
  populatePropertyCachingInfo();
  BCFGen_->bytecodeGenerationComplete();
}

uint8_t HBCISel::acquirePrivateNameCacheIndex(
    Value *privateName,
    PrivateNameOperationKind k) {
  const bool reuse = F_->getContext().getOptimizationSettings().reusePropCache;
  Value *cacheKey = privateName;
  // Ideally we want to be returning the same cache index (when reuse is true)
  // for usages of the same private name. Most usages of private names will be
  // via a LoadFrame. Each LoadFrame is a different instruction, so we would
  // never get matches on those `Value*`s. So instead, use the underlying
  // variable as the Value* key, as that will be shared across different
  // LoadFrames.
  if (auto *LFI = llvh::dyn_cast<LoadFrameInst>(privateName)) {
    cacheKey = LFI->getLoadVariable();
  }
  if (reuse) {
    auto iter = privateNameCacheIdx_.find({cacheKey, (int)k});
    if (iter != privateNameCacheIdx_.end()) {
      ++NumCachedNodes;
      return iter->second;
    }
  }

  if (LLVM_UNLIKELY(nextPrivateNameCacheIndex_ == PROPERTY_CACHING_DISABLED)) {
    ++NumUncachedNodes;
    return PROPERTY_CACHING_DISABLED;
  }

  ++NumCachedNodes;
  ++NumCacheSlots;
  uint8_t idx = nextPrivateNameCacheIndex_++;
  if (reuse) {
    privateNameCacheIdx_[{cacheKey, (int)k}] = idx;
  }
  return idx;
}

uint8_t HBCISel::acquirePropertyReadCacheIndex(
    Identifier prop,
    PropCacheKind k) {
  const bool reuse = F_->getContext().getOptimizationSettings().reusePropCache;
  if (reuse) {
    auto iter = propertyReadCacheIndexForId_.find({prop, (int)k});
    if (iter != propertyReadCacheIndexForId_.end()) {
      ++NumCachedNodes;
      return iter->second;
    }
  }

  if (LLVM_UNLIKELY(nextPropertyReadCacheIndex_ == PROPERTY_CACHING_DISABLED)) {
    ++NumUncachedNodes;
    return PROPERTY_CACHING_DISABLED;
  }

  ++NumCachedNodes;
  ++NumCacheSlots;
  int8_t idx = nextPropertyReadCacheIndex_++;
  if (reuse) {
    propertyReadCacheIndexForId_[{prop, (int)k}] = idx;
  }
  return idx;
}

uint8_t HBCISel::acquirePropertyWriteCacheIndex(Identifier prop) {
  const bool reuse = F_->getContext().getOptimizationSettings().reusePropCache;
  if (reuse) {
    auto iter = propertyWriteCacheIndexForId_.find(prop);
    if (iter != propertyWriteCacheIndexForId_.end()) {
      ++NumCachedNodes;
      return iter->second;
    }
  }

  if (LLVM_UNLIKELY(
          nextPropertyWriteCacheIndex_ == PROPERTY_CACHING_DISABLED)) {
    ++NumUncachedNodes;
    return PROPERTY_CACHING_DISABLED;
  }

  ++NumCachedNodes;
  ++NumCacheSlots;
  ++NumPutCacheSlots;
  uint8_t idx = nextPropertyWriteCacheIndex_++;
  if (reuse) {
    propertyWriteCacheIndexForId_[prop] = idx;
  }
  return idx;
}

} // namespace

void runHBCISel(
    Function *F,
    BytecodeFunctionGenerator *BCFGen,
    HVMRegisterAllocator &RA,
    const BytecodeGenerationOptions &options,
    FileAndSourceMapIdCache &debugIdCache) {
  HBCISel{F, BCFGen, RA, options, debugIdCache}.run();
}

} // namespace hbc
} // namespace hermes

#undef DEBUG_TYPE
