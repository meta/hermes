/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BytecodeGenerator.h"

#include "LoweringPipelines.h"
#include "hermes/ADT/ScopedHashTable.h"
#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/BCGen/HBC/BCProviderFromSrc.h"
#include "hermes/BCGen/HBC/HVMRegisterAllocator.h"
#include "hermes/BCGen/HBC/Passes.h"
#include "hermes/BCGen/HBC/Passes/InsertProfilePoint.h"
#include "hermes/BCGen/LowerStoreInstrs.h"
#include "hermes/BCGen/Lowering.h"
#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/IR/Analysis.h"

#include <variant>

#define DEBUG_TYPE "hbc-bytecode-generator"

namespace hermes {
namespace hbc {

// If we have less than this number of instructions in a Function, and we're
// not compiling in optimized mode, take shortcuts during register allocation.
// 250 was chosen so that registers will fit in a single byte even after some
// have been reserved for function parameters.
const unsigned kFastRegisterAllocationThreshold = 250;

// If register allocation is expected to take more than this number of bytes of
// RAM, and we're not trying to optimize, use a simpler pass to reduce compile
// time memory usage.
const uint64_t kRegisterAllocationMemoryLimit = 10L * 1024 * 1024;

/// Used in delta optimizing mode.
/// \return a UniquingStringLiteralAccumulator seeded with strings  from a
/// bytecode provider \p bcProvider.
static StringLiteralTable stringAccumulatorFromBCProvider(
    const BCProviderBase &bcProvider) {
  uint32_t count = bcProvider.getStringCount();

  std::vector<StringTableEntry> entries;
  llvh::BitVector isIdentifier;

  entries.reserve(count);
  isIdentifier.reserve(count);

  {
    unsigned i = 0;
    for (auto kindEntry : bcProvider.getStringKinds()) {
      bool isIdentRun = kindEntry.kind() != StringKind::String;
      for (unsigned j = 0; j < kindEntry.count(); ++j, ++i) {
        entries.push_back(bcProvider.getStringTableEntry(i));
        isIdentifier.push_back(isIdentRun);
      }
    }

    assert(i == count && "Did not initialise every string");
  }

  auto strStorage = bcProvider.getStringStorage();
  ConsecutiveStringStorage css{std::move(entries), strStorage.vec()};

  return StringLiteralTable{std::move(css), std::move(isIdentifier)};
}

void BytecodeFunctionGenerator::addDebugSourceLocation(
    const DebugSourceLocation &info) {
  assert(
      !complete_ &&
      "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
  // If an address is repeated, it means no actual bytecode was emitted for the
  // previous source location.
  if (!debugLocations_.empty() &&
      debugLocations_.back().address == info.address) {
    debugLocations_.back() = info;
  } else {
    debugLocations_.push_back(info);
  }
}

std::unique_ptr<BytecodeFunction>
BytecodeFunctionGenerator::generateBytecodeFunction(
    Function *F,
    uint32_t functionID,
    uint32_t nameID,
    BytecodeModuleGenerator &BMGen,
    HVMRegisterAllocator &RA,
    BytecodeGenerationOptions options,
    FileAndSourceMapIdCache &debugCache,
    DebugInfoGenerator &debugInfoGenerator) {
  BytecodeFunctionGenerator funcGen{BMGen, RA.getMaxHVMRegisterUsage()};

  if (F->isLazy()) {
    // Move the Function out of the main function list.
    F->moveToCompiledFunctionList();
    funcGen.bytecodeGenerationComplete();
  } else {
    runHBCISel(F, &funcGen, RA, options, debugCache);
    if (funcGen.hasEncodingError()) {
      F->getParent()->getContext().getSourceErrorManager().error(
          F->getSourceRange().Start, "Error encoding bytecode");
      return nullptr;
    }
  }
  assert(
      funcGen.complete_ && "ISel did not complete BytecodeFunctionGenerator");

  // Get the number of bits needed to encode the loop depth.
  // Avoid overflowing the FunctionHeader just for loop depth.
#define DECLARE_BITFIELD(_, storage, api_type, name, bits) \
  [[maybe_unused]] constexpr uint32_t f##name##Width = bits;
  FUNC_HEADER_FIELDS(DECLARE_BITFIELD, DECLARE_BITFIELD)
#undef DECLARE_BITFIELD

  FunctionHeader header{
      funcGen.bytecodeSize_,
      F->getExpectedParamCountIncludingThis(),
      std::min(
          RA.getMaxLoopDepth(),
          llvh::maskTrailingOnes<uint32_t>(fLoopDepthWidth)),
      funcGen.frameSize_,
      RA.getMaxRegisterUsage(RegClass::Number),
      RA.getMaxRegisterUsage(RegClass::NonPtr),
      nameID,
      funcGen.readCacheSize_,
      funcGen.writeCacheSize_,
      funcGen.privateNameCacheSize_};

  header.flags.setProhibitInvoke(computeProhibitInvoke(F->getProhibitInvoke()));
  header.flags.setKind(computeFuncKind(F->getKind()));
  header.flags.setStrictMode(F->isStrictMode());
  header.flags.setHasExceptionHandler(funcGen.exceptionHandlers_.size());

  auto bcFunc = std::make_unique<BytecodeFunction>(
      std::move(funcGen.opcodes_),
      std::move(header),
      std::move(funcGen.exceptionHandlers_));

  if (funcGen.hasDebugInfo()) {
    uint32_t sourceLocOffset = debugInfoGenerator.appendSourceLocations(
        funcGen.getSourceLocation(), functionID, funcGen.getDebugLocations());
    // TODO: Delete lexical offset, there's nothing there.
    bcFunc->setDebugOffsets({sourceLocOffset});
  }

  // For lazy functions, set the function here to be used by the
  // compilation pipeline when they are called at runtime.
  if (auto *lazyInst = F->getLazyCompilationDataInst()) {
    bcFunc->setFunctionIR(F);
    lazyInst->getData().bcFunctionID = functionID;
  } else if (auto *evalInst = F->getEvalCompilationDataInst()) {
    bcFunc->setFunctionIR(F);
    evalInst->getData().bcFunctionID = functionID;
  }

  return bcFunc;
}

void BytecodeFunctionGenerator::shrinkJump(offset_t loc) {
  // We are shrinking a long jump into a short jump.
  // The size of operand reduces from 4 bytes to 1 byte, a delta of 3.
  opcodes_.erase(opcodes_.begin() + loc, opcodes_.begin() + loc + 3);

  // Change this instruction from long jump to short jump.
  longToShortJump(loc - 1);
}

void BytecodeFunctionGenerator::updateJumpTarget(
    offset_t loc,
    int newVal,
    int bytes) {
  // The jump target is encoded in little-endian. Update it correctly
  // regardless of host byte order.
  for (; bytes; --bytes, ++loc) {
    opcodes_[loc] = (opcode_atom_t)(newVal);
    newVal >>= 8;
  }
}

void BytecodeFunctionGenerator::updateTableOffset(
    offset_t loc,
    uint32_t tableOffset,
    uint32_t instLoc) {
  assert(opcodes_.size() > instLoc && "invalid switchimm offset");

  // The offset is not aligned, but will be aligned when read in the
  // interpreter.
  updateJumpTarget(
      loc, opcodes_.size() + tableOffset - instLoc, sizeof(uint32_t));
}

void BytecodeFunctionGenerator::bytecodeGenerationComplete() {
  assert(!complete_ && "Can only call bytecodeGenerationComplete once");
  complete_ = true;
  bytecodeSize_ = opcodes_.size();
  // Add the jump tables inline with the opcodes, as a 4-byte aligned section at
  // the end of the opcode array.
  uint32_t alignedOpcodes = llvh::alignTo<sizeof(uint32_t)>(bytecodeSize_);
  // The alignment constraints of jump tables and string switch tables are the
  // same:
  static_assert(alignof(StringSwitchTableCase) == sizeof(uint32_t));
  uint32_t stringSwitchTableStart = alignedOpcodes;
  if (!jumpTable_.empty()) {
    uint32_t jumpTableBytes = jumpTable_.size() * sizeof(uint32_t);
    stringSwitchTableStart += jumpTableBytes;
    opcodes_.reserve(alignedOpcodes + jumpTableBytes);
    opcodes_.resize(alignedOpcodes, 0);
    const opcode_atom_t *jumpTableStart =
        reinterpret_cast<opcode_atom_t *>(jumpTable_.data());
    opcodes_.insert(
        opcodes_.end(), jumpTableStart, jumpTableStart + jumpTableBytes);
  }
  if (!stringSwitchTable_.empty()) {
    uint32_t stringSwitchTableBytes =
        stringSwitchTable_.size() * sizeof(StringSwitchTableCase);
    opcodes_.reserve(stringSwitchTableStart + stringSwitchTableBytes);
    opcodes_.resize(stringSwitchTableStart, 0);
    const opcode_atom_t *stringSwitchTableStart =
        reinterpret_cast<opcode_atom_t *>(stringSwitchTable_.data());
    opcodes_.insert(
        opcodes_.end(),
        stringSwitchTableStart,
        stringSwitchTableStart + stringSwitchTableBytes);
  }
}

unsigned BytecodeModuleGenerator::addFunction(Function *F) {
  auto [it, inserted] = functionIDMap_.insert({F, bm_.getNumFunctions()});
  if (inserted) {
    bm_.addFunction();
  }
  return it->second;
}

void BytecodeModuleGenerator::initializeSerializedLiterals(
    LiteralBufferBuilder::Result &&bufs) {
  assert(
      bm_.getLiteralValueBuffer().empty() && bm_.getObjectKeyBuffer().empty() &&
      literalOffsetMap_.empty() && "serialized literals already initialized");
  bm_.initializeSerializedLiterals(
      std::move(bufs.literalValBuffer),
      std::move(bufs.keyBuffer),
      std::move(bufs.shapeTable));
  literalOffsetMap_ = std::move(bufs.offsetMap);
}

void BytecodeModuleGenerator::initializeSerializedLiteralsLazy(
    LiteralBufferBuilder::Result &&bufs) {
  uint32_t literalValBufferOffset = bm_.getLiteralValueBufferSize();
  uint32_t objKeyBufferOffset = bm_.getObjectKeyBufferSize();
  uint32_t objShapeTableOffset = bm_.getObjectShapeTableSize();

  // Fix up the shape table to point to the potentially offset key buffer.
  for (auto &entry : bufs.shapeTable) {
    entry.keyBufferOffset += objKeyBufferOffset;
  }

  // Offset all the offsets by the existing offsets.
  for (auto &[it, entry] : bufs.offsetMap) {
    if (entry.shapeTableIdx == UINT32_MAX) {
      entry.valueBufferOffset += literalValBufferOffset;
    } else {
      entry.shapeTableIdx += objShapeTableOffset;
      entry.valueBufferOffset += literalValBufferOffset;
    }
  }

  // FIXME: Avoid copying the buffers by reusing the existing std::vectors.
  bm_.appendSerializedLiterals(
      std::move(bufs.literalValBuffer),
      std::move(bufs.keyBuffer),
      std::move(bufs.shapeTable));

  /// literalOffsetMap_ isn't persisted because it's only used for ISel,
  /// and once that's done it can be discarded because any relevant information
  /// has been stored in the bytecode instructions themselves.
  literalOffsetMap_ = std::move(bufs.offsetMap);
}

/// \return true if and only if a literal string operand at index \p idx of
/// instruction \p I is to be treated as an Identifier during Hermes Bytecode
/// generation.
static bool isIdOperand(const Instruction *I, unsigned idx) {
#define CASE_WITH_PROP_IDX(INSN) \
  case ValueKind::INSN##Kind:    \
    return idx == INSN::PropertyIdx

  switch (I->getKind()) {
    CASE_WITH_PROP_IDX(DeletePropertyLooseInst);
    CASE_WITH_PROP_IDX(DeletePropertyStrictInst);
    CASE_WITH_PROP_IDX(LoadPropertyInst);
    CASE_WITH_PROP_IDX(LoadPropertyWithReceiverInst);
    CASE_WITH_PROP_IDX(DefineOwnPropertyInst);
    CASE_WITH_PROP_IDX(StorePropertyLooseInst);
    CASE_WITH_PROP_IDX(StorePropertyStrictInst);
    CASE_WITH_PROP_IDX(TryLoadGlobalPropertyInst);
    CASE_WITH_PROP_IDX(TryStoreGlobalPropertyLooseInst);
    CASE_WITH_PROP_IDX(TryStoreGlobalPropertyStrictInst);
    CASE_WITH_PROP_IDX(CreatePrivateNameInst);

    case ValueKind::DeclareGlobalVarInstKind:
      return idx == DeclareGlobalVarInst::NameIdx;

    case ValueKind::LIRAllocObjectFromBufferInstKind:
      // AllocObjectFromBuffer stores the keys and values as alternating
      // operands, with keys starting first.
      return (idx - LIRAllocObjectFromBufferInst::FirstKeyIdx) % 2 == 0;
    case ValueKind::LIRAllocTypedObjectFromBufferInstKind:
      return llvh::cast<LIRAllocTypedObjectFromBufferInst>(I)->isKeyIndex(idx);
    case ValueKind::LIRAllocTypedNonEnumObjectFromBufferInstKind:
      return llvh::cast<LIRAllocTypedNonEnumObjectFromBufferInst>(I)
          ->isKeyIndex(idx);

    default:
      return false;
  }
#undef CASE
}

/// Encode a Unicode codepoint into a UTF8 sequence and append it to \p
/// storage. Code points above 0xFFFF are encoded into UTF16, and the
/// resulting surrogate pair values are encoded individually into UTF8.
static inline void appendUnicodeToStorage(
    uint32_t cp,
    llvh::SmallVectorImpl<char> &storage) {
  // Sized to allow for two 16-bit values to be encoded.
  // A 16-bit value takes up to three bytes encoded in UTF-8.
  char buf[8];
  char *d = buf;
  // We need to normalize code points which would be encoded with a surrogate
  // pair. Note that this produces technically invalid UTF-8.
  if (LLVM_LIKELY(cp < 0x10000)) {
    hermes::encodeUTF8(d, cp);
  } else {
    assert(cp <= UNICODE_MAX_VALUE && "invalid Unicode value");
    cp -= 0x10000;
    hermes::encodeUTF8(d, UTF16_HIGH_SURROGATE + ((cp >> 10) & 0x3FF));
    hermes::encodeUTF8(d, UTF16_LOW_SURROGATE + (cp & 0x3FF));
  }
  storage.append(buf, d);
}

void BytecodeModuleGenerator::collectStrings() {
  StringLiteralTable &strings = bm_.getStringLiteralTableMut();
  if (baseBCProvider_) {
    // If we are in delta optimizing mode, start with the string storage from
    // our base bytecode provider.
    assert(strings.empty() && "delta optimizing mode can't be lazy");
    strings = stringAccumulatorFromBCProvider(*baseBCProvider_);
  }

  for (auto [F, functionID] : functionIDMap_) {
    // Walk functions.
    for (auto &BB : *F) {
      // Walk instruction operands.
      for (auto &I : BB) {
        for (int i = 0, e = I.getNumOperands(); i < e; i++) {
          auto *op = I.getOperand(i);
          if (auto *str = llvh::dyn_cast<LiteralString>(op)) {
            strings.addString(str->getValue().str(), isIdOperand(&I, i));
          }
        }
      }
    }
  }

  /// Add the original function source \p str to the \c strings table.
  /// If it's not ASCII, re-encode it using the string table's string literal
  /// encoding and map from the original source to the newly encoded source in
  /// unicodeFunctionSources,so it can be reused below.
  auto addFunctionSource = [&strings, this](llvh::StringRef str) {
    if (hermes::isAllASCII(str.begin(), str.end())) {
      // Fast path, no re-encoding needed.
      strings.addString(str, /* isIdentifier */ false);
    } else {
      auto &storage = unicodeFunctionSources_[str];
      if (!storage.empty())
        return;
      for (const char *cur = str.begin(), *e = str.end(); cur != e;
           /* increment in body */) {
        if (LLVM_UNLIKELY(isUTF8Start(*cur))) {
          // Decode and re-encode the character and append it to the string
          // storage
          appendUnicodeToStorage(
              hermes::_decodeUTF8SlowPath<false>(
                  cur, [](const llvh::Twine &) {}),
              storage);
        } else {
          storage.push_back(*cur);
          ++cur;
        }
      }
      strings.addString(
          llvh::StringRef{storage.begin(), storage.size()},
          /* isIdentifier */ false);
    }
  };

  // Populate strings table and if the source of a function contains unicode,
  // add an entry to the unicodeFunctionSources.
  for (auto [F, functionID] : functionIDMap_) {
    if (!options_.stripFunctionNames) {
      strings.addString(
          F->getOriginalOrInferredName().str(), /* isIdentifier */ false);
    }
    // The source visibility of the global function indicate the presence of
    // top-level source visibility directives, but we should not preserve the
    // source code of the global function.
    if (!F->isGlobalScope()) {
      // Only add non-default source representation to the string table.
      if (auto source = F->getSourceRepresentationStr()) {
        addFunctionSource(*source);
      }
    }
  }

  if (!M_->getCJSModulesResolved()) {
    for (auto [F, functionID] : functionIDMap_) {
      if (auto *cjsModule = M_->findCJSModule(F)) {
        strings.addString(cjsModule->filename.str(), /* isIdentifier */ false);
      }
    }
  }

  if (options_.stripFunctionNames) {
    strings.addString(kStrippedFunctionName, /* isIdentifier */ true);
    // Force the string table to treat the kStrippedFunctionName as very
    // important, so that it gets a low string ID (under 256).
    // Setting the number of identifier references to the maximum makes the
    // string table sort it into the earliest frequency bucket it can.
    // Do this _after_ all other strings have been collected to ensure nothing
    // else increments/changes the number of identifier references.
    strings.tryEnsure8BitStringIDForIdentifier(kStrippedFunctionName);
  }

  if (M_->getContext().isLazyCompilation()) {
    uint32_t startIdx = strings.getStringTableView().size();
    strings.populateStorage(StringLiteralTable::OptimizeMode::None);
    bm_.appendStringMetadataFromStringTable(startIdx);
  } else {
    strings.populateStorage(
        options_.optimizationEnabled
            ? StringLiteralTable::OptimizeMode::ReorderAndPack
            : StringLiteralTable::OptimizeMode::Reorder);
    bm_.populateStringMetadataFromStringTable();
  }
}

namespace {

class FixupEnvIDVisitor;

/// A "stack node" used by the dominator tree DFS visitor to keep track where
/// it is in the tree. This specialization pushes and pops a
/// ScopedHashTableScope, to pop all dominating environments when we go back
/// up the tree.
class FixupEnvIDStackNode : public DomTreeDFS::StackNode {
  ScopedHashTableScope<Value *, std::monostate> scope_;

 public:
  explicit inline FixupEnvIDStackNode(
      FixupEnvIDVisitor *ctx,
      const DominanceInfoNode *n);
};

/// A utility class to clear the environment ID from instructions that are not
/// dominated by the instruction creating the environment. This happens in
/// generators, albeit rarely, making the instruction non-inspectable. In those
/// rare cases we need to ensure we never produce invalid debugging information.
/// The "correct" solution would be for this never to happen in the first place,
/// but we haven't figured out how yet.
///
/// In practice this also clears up the environment ID of the instructions
/// creating each environment, because they are tagged with their own
/// environment when they arrive at this stage.
///
/// Implemented as a DFS dominator tree visitor, keeping the currently
/// dominating environments in a ScopedHashTable.
class FixupEnvIDVisitor
    : public DomTreeDFS::Visitor<FixupEnvIDVisitor, FixupEnvIDStackNode> {
  /// The function we are fixing up.
  Function *F_;

  using EnvSet = llvh::SmallPtrSet<Value *, 2>;
  /// Map recording all environments created in a basic block. This is
  /// pre-populated before we run.
  llvh::DenseMap<BasicBlock *, EnvSet> bbToEnvs_{};

  /// Environments dominating the current instruction at any point. As we walk
  /// the dominator tree in DFS, environments are pushed as we encounter
  /// instructions that created them (pre-recorded in \c envBBs) and popped
  /// when we go back up the tree.
  ScopedHashTable<Value *, std::monostate> envs_{};

#ifndef NDEBUG
  /// Number of instructions with scope checked.
  int count_ = 0;
  /// Number of instructions with scope cleared.
  int cleared_ = 0;
#endif

  friend class FixupEnvIDStackNode;
  friend class DomTreeDFS::Visitor<FixupEnvIDVisitor, FixupEnvIDStackNode>;

 public:
  explicit FixupEnvIDVisitor(Function *F, DominanceInfo &DI)
      : DomTreeDFS::Visitor<FixupEnvIDVisitor, FixupEnvIDStackNode>(DI),
        F_(F) {}
  // clang-format off
#ifndef NDEBUG
  void incCount() { ++count_; }
  int getCount() const { return count_; }
  void incCleared() { ++cleared_; }
  int getCleared() const { return cleared_; }
#else
  void incCount() {}
  int getCount() const { return 0; }
  void incCleared() {}
  int getCleared() const { return 0; }
#endif
  // clang-format on

  /// Perform the fixup.
  void run() {
    // Collect all basic block environments.
    for (Value *env : F_->environments())
      if (auto *I = dyn_cast<Instruction>(env))
        bbToEnvs_[I->getParent()].insert(I);

    DFS();
  }

 private:
  /// Called by the visitor when we visit each dominator tree node in DFS order.
  /// All environments dominating this basic block are "active" in \c bbToEnvs_.
  bool processNode(FixupEnvIDStackNode *SN) {
    BasicBlock *BB = SN->node()->getBlock();

    // An environment already known to dominate instructions in this BB.
    // This is a fast 1-entry cache while scanning the BB.
    Instruction *knownDominateEnv = nullptr;

    // Run two versions of the loop, depending on whether environments are
    // created by this BB.
    if (auto it = bbToEnvs_.find(BB); it != bbToEnvs_.end()) {
      // Some environments created in this BB. We need to record when exactly,
      // as that environment will dominate the remaining instructions in the
      // block.
      EnvSet *const envSet = &it->second;

      for (auto &I : *BB) {
        auto *implicitEnvOperand =
            llvh::dyn_cast_or_null<Instruction>(I.getImplicitEnvOperand());
        if (!implicitEnvOperand)
          continue;
        incCount();
        if (implicitEnvOperand == knownDominateEnv)
          continue;

        // Does the environment dominate this inst? If not, clear it.
        if (envs_.find(implicitEnvOperand)) {
          // Cache the last known dominator.
          knownDominateEnv = implicitEnvOperand;
        } else {
          incCleared();
          I.clearEnvironmentID();
        }

        // If this instruction creates an environment, record it as dominating
        // the rest of the instructions in this BB (and lower dominated BBs).
        if (envSet->count(&I))
          envs_.try_emplace(&I, {});
      }
    } else {
      // No environments created in this BB.
      for (auto &I : *BB) {
        auto *implicitEnvOperand =
            llvh::dyn_cast_or_null<Instruction>(I.getImplicitEnvOperand());
        if (!implicitEnvOperand)
          continue;
        incCount();
        if (implicitEnvOperand == knownDominateEnv)
          continue;

        // Does the environment dominate this inst? If not, clear it.
        if (envs_.find(implicitEnvOperand)) {
          // Cache the last known dominator.
          knownDominateEnv = implicitEnvOperand;
        } else {
          incCleared();
          I.clearEnvironmentID();
        }
      }
    }

    return true;
  }
};

FixupEnvIDStackNode::FixupEnvIDStackNode(
    FixupEnvIDVisitor *ctx,
    const DominanceInfoNode *n)
    : DomTreeDFS::StackNode(n), scope_(ctx->envs_) {}

} // anonymous namespace

/// Iterate over all the instructions in \p F and make sure the environment ID
/// is valid.
static void fixupEnvironmentIDs(Function *F) {
#if 0
  auto start = std::chrono::steady_clock::now();
#endif

  DominanceInfo D(F);
  FixupEnvIDVisitor v(F, D);
  v.run();

  LLVM_DEBUG(
      llvh::dbgs() << v.getCount() << " checks, " << v.getCleared()
                   << " cleared\n");
#if 0
  auto end = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  llvh::outs() << "fixupEnvironmentIDs took " << duration.count() << " ms\n";
#endif
}

bool BytecodeModuleGenerator::generateAddedFunctions(
    OptValue<uint32_t> replacedFunctionID) {
  BytecodeOptions &bytecodeOptions = bm_.getBytecodeOptionsMut();
  bytecodeOptions.setCjsModulesStaticallyResolved(M_->getCJSModulesResolved());

  M_->assignIndexToVariables();

  const uint32_t strippedFunctionNameId =
      options_.stripFunctionNames ? bm_.getStringID(kStrippedFunctionName) : 0;
  // Generated function with ID replacedFunctionID, installed last.
  std::unique_ptr<BytecodeFunction> replacement{};
  for (auto [F, functionID] : functionIDMap_) {
    auto *cjsModule = M_->findCJSModule(F);
    if (cjsModule) {
      if (M_->getCJSModulesResolved()) {
        addCJSModuleStatic(cjsModule->id, functionID);
      } else {
        addCJSModule(functionID, getStringID(cjsModule->filename.str()));
      }
    }

    // Add entries to function source table for non-default source.
    if (!F->isGlobalScope()) {
      if (auto source = F->getSourceRepresentationStr()) {
        auto it = unicodeFunctionSources_.find(*source);
        // If the original source was mapped to a re-encoded one in
        // unicodeFunctionSources, then use the re-encoded source to lookup the
        // string ID. Otherwise it's ASCII and can be used directly.
        if (it != unicodeFunctionSources_.end()) {
          addFunctionSource(
              functionID,
              getStringID(
                  llvh::StringRef{it->second.begin(), it->second.size()}));
        } else {
          addFunctionSource(functionID, getStringID(*source));
        }
      }
    }

    // Under full debug info, ensure the environment IDs are all valid. Without
    // debug info, there are not environment IDs set at all.
    if (M_->getContext().getDebugInfoSetting() == DebugInfoSetting::ALL) {
      fixupEnvironmentIDs(F);
    }

    // Run register allocation.
    HVMRegisterAllocator RA(F);
    if (!options_.optimizationEnabled) {
      RA.setFastPassThreshold(kFastRegisterAllocationThreshold);
      RA.setMemoryLimit(kRegisterAllocationMemoryLimit);
    }
    auto PO = postOrderAnalysis(F);
    /// The order of the blocks is reverse-post-order, which is a simply
    /// topological sort.
    llvh::SmallVector<BasicBlock *, 16> order(PO.rbegin(), PO.rend());
    RA.allocate(order);

    if (options_.format == DumpRA)
      RA.dump();

    lowerAllocatedFunctionIR(F, RA, options_);

    if (options_.format == DumpLRA)
      RA.dump();

    uint32_t functionNameId = options_.stripFunctionNames
        ? strippedFunctionNameId
        : bm_.getStringID(F->getOriginalOrInferredName().str());

    // Use the register allocated IR to make a BytecodeFunctionGenerator and
    // run ISel.
    std::unique_ptr<BytecodeFunction> func =
        BytecodeFunctionGenerator::generateBytecodeFunction(
            F,
            functionID,
            functionNameId,
            *this,
            RA,
            options_,
            debugIdCache_,
            debugInfoGenerator_);
    if (!func) {
      // Leave the IR of the replacement in the Module, as when its own
      // generation fails: destroying the BytecodeFunction would remove the IR
      // from the compiled function list, which does not contain it.
      if (replacement)
        replacement->setFunctionIR(nullptr);
      return false;
    }

    if (replacedFunctionID && functionID == *replacedFunctionID)
      replacement = std::move(func);
    else
      bm_.setFunction(functionID, std::move(func));
  }

  if (replacement)
    bm_.setFunction(*replacedFunctionID, std::move(replacement));

  std::move(debugInfoGenerator_).generate();
  return true;
}

bool BytecodeModuleGenerator::generate(
    Function *entryPoint,
    hermes::OptValue<uint32_t> segment) && {
  assert(valid_ && "cannot generate more than once with a generator");
  valid_ = false;

  lowerModuleIR(M_, options_);
  if (M_->getContext().getSourceErrorManager().getErrorCount() != 0)
    return false;

  if (options_.format == DumpLIR)
    M_->dump();

  if (segment) {
    setSegmentID(*segment);
  }
  // Empty if all functions should be generated (i.e. bundle splitting was not
  // requested).
  llvh::DenseSet<Function *> functionsToGenerate = segment
      ? M_->getFunctionsInSegment(*segment)
      : llvh::DenseSet<Function *>{};

  /// \return true if we should generate function \p f.
  std::function<bool(Function *)> shouldGenerate;
  if (segment) {
    shouldGenerate = [entryPoint, &functionsToGenerate](const Function *f) {
      return f == entryPoint || functionsToGenerate.count(f) > 0;
    };
  } else {
    shouldGenerate = [](const Function *) { return true; };
  }

  // Add each function to BMGen so that each function has a unique ID.
  for (auto &F : *M_) {
    if (!shouldGenerate(&F)) {
      continue;
    }
    auto index = addFunction(&F);
    if (&F == entryPoint) {
      setEntryPointIndex(index);
    }
  }
  assert(getEntryPointIndex() != -1 && "Entry point not added");

  collectStrings();

  // TODO: Avoid iterating the entire Module here.
  // Possibilities include passing the list of Functions to
  // LiteralBufferBuilder or letting LiteralBufferBuilder operate in an
  // incremental manner on single Functions.
  initializeSerializedLiterals(
      LiteralBufferBuilder::generate(
          M_,
          shouldGenerate,
          [this](llvh::StringRef str) { return getIdentifierID(str); },
          [this](llvh::StringRef str) { return getStringID(str); },
          options_.optimizationEnabled,
          baseBCProvider_.get()));

  if (!generateAddedFunctions())
    return false;

  M_->resetForMoreCompilation();

  return true;
}

bool BytecodeModuleGenerator::generateLazyFunctions(
    Function *lazyFunc,
    uint32_t lazyFuncID) && {
  assert(valid_ && "cannot generate more than once with a generator");
  assert(
      bm_.getBCProviderFromSrc() && "BytecodeModule doesn't have a provider");
  valid_ = false;

  lowerModuleIR(M_, options_);

  // Add each function to BMGen so that each function has a unique ID.
  // Start by giving the real Function the ID of the lazy Function.
  // generateAddedFunctions() replaces the lazy BytecodeFunction only if all
  // the functions are generated, because the runtime still references it.
  // NOTE: The old lazy BytecodeFunction's destructor will then run,
  // which will remove the lazy IR Function from the Module.
  functionIDMap_.insert({lazyFunc, lazyFuncID});

  /// \return true if we should generate function \p f.
  std::function<bool(Function *)> shouldGenerate = [this](Function *f) -> bool {
    // Do not generate the top-level function because it's retained
    // throughout lazy compilation (due to IRGen requirements) but is only
    // an UnreachableInst stub after it's generated.
    if (f == M_->getTopLevelFunction())
      return false;

    // Skip functions that already have BytecodeFunctions and IDs.
    if (auto *lazyInst = f->getLazyCompilationDataInst();
        lazyInst && lazyInst->getData().bcFunctionID.hasValue()) {
      return false;
    } else if (auto *evalInst = f->getEvalCompilationDataInst();
               evalInst && evalInst->getData().bcFunctionID.hasValue()) {
      return false;
    }

    return true;
  };

  for (Function &F : *M_) {
    if (!shouldGenerate(&F)) {
      continue;
    }
    addFunction(&F);
  }

  collectStrings();

  // TODO: Avoid creating a new vector and copying the data here.
  // We should just be able to give references to LiteralBufferBuilder and have
  // it append directly.
  initializeSerializedLiteralsLazy(
      LiteralBufferBuilder::generate(
          M_,
          shouldGenerate,
          [this](llvh::StringRef str) { return getIdentifierID(str); },
          [this](llvh::StringRef str) { return getStringID(str); },
          options_.optimizationEnabled));

  bool result = generateAddedFunctions(lazyFuncID);

  // Any of the previous steps may have modified existing tables,
  // so make sure we update the provider and clean up even if errors occurred
  // when generateAddedFunctions was called.
  bm_.getBCProviderFromSrc()->setBytecodeModuleRefs();
  M_->resetForMoreCompilation();

  return result;
}

bool BytecodeModuleGenerator::generateForEval(Function *entryPoint) && {
  assert(valid_ && "cannot generate more than once with a generator");
  valid_ = false;

  lowerModuleIR(M_, options_);

  if (options_.format == DumpLIR)
    M_->dump();

  llvh::DenseSet<Function *> functionsToGenerate{};

  /// \return true if we should generate function \p f.
  auto shouldGenerate = [this](Function *f) {
    // Do not generate the top-level function because it's retained
    // throughout multiple compilations (due to IRGen requirements) but is only
    // an UnreachableInst stub after it's generated.
    if (f == M_->getTopLevelFunction())
      return false;

    // Skip functions that already have BytecodeFunctions and IDs.
    if (auto *lazyInst = f->getLazyCompilationDataInst();
        lazyInst && lazyInst->getData().bcFunctionID.hasValue()) {
      return false;
    } else if (auto *evalInst = f->getEvalCompilationDataInst();
               evalInst && evalInst->getData().bcFunctionID.hasValue()) {
      return false;
    }

    return true;
  };

  // Add each function to BMGen so that each function has a unique ID.
  for (auto &F : *M_) {
    if (!shouldGenerate(&F)) {
      continue;
    }
    auto index = addFunction(&F);
    if (&F == entryPoint) {
      setEntryPointIndex(index);
    }
  }
  assert(getEntryPointIndex() != -1 && "Entry point not added");

  collectStrings();

  // TODO: Avoid iterating the entire Module here.
  // Possibilities include passing the list of Functions to
  // LiteralBufferBuilder or letting LiteralBufferBuilder operate in an
  // incremental manner on single Functions.
  initializeSerializedLiterals(
      LiteralBufferBuilder::generate(
          M_,
          shouldGenerate,
          [this](llvh::StringRef str) { return getIdentifierID(str); },
          [this](llvh::StringRef str) { return getStringID(str); },
          options_.optimizationEnabled));

  if (!generateAddedFunctions())
    return false;

  M_->resetForMoreCompilation();

  return true;
}

} // namespace hbc
} // namespace hermes
