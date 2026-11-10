/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_BCGEN_HBC_BYTECODEGENERATOR_H
#define HERMES_BCGEN_HBC_BYTECODEGENERATOR_H

#include "ISel.h"
#include "hermes/BCGen/Exceptions.h"
#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/BCGen/HBC/Bytecode.h"
#include "hermes/BCGen/HBC/BytecodeInstructionGenerator.h"
#include "hermes/BCGen/HBC/ConsecutiveStringStorage.h"
#include "hermes/BCGen/HBC/DebugInfo.h"
#include "hermes/BCGen/HBC/UniquingFilenameTable.h"
#include "hermes/BCGen/HBC/UniquingStringLiteralTable.h"
#include "hermes/BCGen/LiteralBufferBuilder.h"
#include "hermes/BCGen/SerializedLiteralGenerator.h"
#include "hermes/IR/IR.h"
#include "hermes/Regex/RegexSerialization.h"
#include "hermes/Support/BigIntSupport.h"
#include "hermes/Support/Conversions.h"
#include "hermes/Support/OptValue.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/MapVector.h"
#include "llvh/ADT/StringRef.h"

namespace hermes {
namespace hbc {

const char *const kStrippedFunctionName = "function-name-stripped";

class BytecodeFunctionGenerator;
class HVMRegisterAllocator;

/// This class is used by the hermes backend.
/// It wraps all data required to generate the module.
/// To use it, construct a BytecodeModuleGenerator and then add any functions to
/// generate using \c addFunction.
/// Then, \c generate will populate and return a BytecodeModule containing only
/// the functions which were added.
class BytecodeModuleGenerator {
  /// The bytecode module being generated.
  /// Never nullptr while this BytecodeModuleGenerator is valid_.
  BytecodeModule &bm_;

  /// The IR Module for which we're generating bytecode.
  Module *M_;

  /// Mapping from Function * to a sequential ID.
  llvh::MapVector<Function *, unsigned> functionIDMap_{};

  /// Generates debug information.
  /// Stores the filename table.
  DebugInfoGenerator debugInfoGenerator_;

  /// A map from instruction to literal offset in the corresponding buffers.
  /// \c arrayBuffer_, \c objKeyBuffer_, \c objliteralValBuffer_.
  /// This map is populated before instruction selection.
  LiteralBufferBuilder::LiteralOffsetMapTy literalOffsetMap_{};

  /// The debug ID cache for the module.
  FileAndSourceMapIdCache &debugIdCache_;

  /// Options controlling bytecode generation.
  BytecodeGenerationOptions options_;

  /// Base bytecode used in delta optimizing mode.
  /// When it is not null and optimization is turned on, we optimize for the
  /// delta.
  std::unique_ptr<BCProviderBase> baseBCProvider_;

  /// Mapping of the source text UTF-8 to the modified UTF-16-like
  /// representation used by string literal encoding.
  /// See appendUnicodeToStorage.
  /// If a function source isn't in this map, then it's entirely ASCII and can
  /// be added to the string table unmodified.
  /// This allows us to add strings to the StringLiteralTable,
  /// which will convert actual UTF-8 to UTF-16 automatically if it's detected,
  /// meaning we'd not be able to directly look up the original function source
  /// in the table.
  llvh::DenseMap<llvh::StringRef, llvh::SmallVector<char, 32>>
      unicodeFunctionSources_{};

  /// Indicate whether this generator is still valid.
  /// We need this because one can only call the generate() function
  /// once, and after that, this generator is no longer valid because
  /// the content has been modified during generation.
  bool valid_{true};

 public:
  /// Constructor which enables optimizations if \p options.optimizationEnabled
  /// is set.
  BytecodeModuleGenerator(
      BytecodeModule &bcModule,
      Module *M,
      FileAndSourceMapIdCache &debugIdCache,
      BytecodeGenerationOptions options = BytecodeGenerationOptions::defaults(),
      std::unique_ptr<BCProviderBase> baseBCProvider = nullptr)
      : bm_(bcModule),
        M_(M),
        debugInfoGenerator_(bm_.getDebugInfo()),
        debugIdCache_(debugIdCache),
        options_(options),
        baseBCProvider_(std::move(baseBCProvider)) {
    bm_.getBytecodeOptionsMut().setStaticBuiltins(
        options_.staticBuiltinsEnabled);
  }

  /// Consume the generator and write the generated code to the BytecodeModule.
  /// \return true on success, false on failure (will report errors).
  bool generate(Function *entryPoint, hermes::OptValue<uint32_t> segment) &&;

  /// Generates new functions created by lazy compilation.
  /// Generates all the functions in the Module except the top-level function.
  /// All the functions generated will have \p lazyFunc as a lexical ancestor,
  /// and \p lazyFunc will be the only Function that had a bytecode function ID
  /// prior to this call.
  /// After this succeeds, the lazy BytecodeFunction at \p lazyFuncID has been
  /// replaced, which cleans up its data, and the lazy children of \p lazyFunc
  /// have their IDs assigned. If it fails, the lazy BytecodeFunction stays in
  /// place.
  /// \param lazyFunc a new function replacing an existing lazy function.
  /// \param lazyFuncID the existing ID of the lazy function.
  /// \return true on success, false on failure (will report errors).
  bool generateLazyFunctions(Function *lazyFunc, uint32_t lazyFuncID) &&;

  /// Generates new functions created by 'eval'.
  /// Skips functions that already have bytecode function IDs.
  /// Consumes the generator.
  /// \param entryPoint the entry point function.
  /// \return true on success, false on failure (will report errors).
  bool generateForEval(Function *entryPoint) &&;

  /// Add a function to request generating bytecode for it if it doesn't
  /// already exist.
  /// The associated BytecodeFunction will be nullptr until it's generated.
  /// \return the function ID.
  unsigned addFunction(Function *F);

  /// Gets the index of the entry point function (global function).
  int getEntryPointIndex() const {
    return bm_.getGlobalFunctionIndex();
  }

  /// Sets the index of the entry point function (global function).
  void setEntryPointIndex(int index) {
    bm_.setGlobalFunctionIndex(index);
  }

  /// Notes that a new StringSwitchImm instruction has been created.
  /// Counts the number created, returning a unique 0-based index for
  /// each such instruction.
  uint32_t addStringSwitchImmInstr() {
    return bm_.addStringSwitchImmInstr();
  }

  /// \returns the index of the string in this module's string table if it
  /// exists.  If the string does not exist will trigger an assertion failure
  /// if assertions are enabled.
  unsigned getStringID(llvh::StringRef str) const {
    return bm_.getStringID(str);
  }

  /// \returns the index of the string in this module's string table, assuming
  /// it exists and is an identifier.  If the string does not exist in the
  /// table, or it is not marked as an identifier, an assertion failure will be
  /// triggered, if assertions are enabled.
  unsigned getIdentifierID(llvh::StringRef str) const {
    return bm_.getIdentifierID(str);
  }

  /// Adds a parsed bigint to the module table.
  /// \return the index of the bigint in the table.
  uint32_t addBigInt(bigint::ParsedBigInt bigint) {
    return bm_.addBigInt(std::move(bigint));
  }

  /// Set the serialized literal tables that this generator will use.
  /// Only to be called once, when the module is created and the buffers are
  /// empty.
  /// \param bufs containing the serialized literals.
  void initializeSerializedLiterals(LiteralBufferBuilder::Result &&bufs);

  /// Set the serialized literal tables that this generator will use,
  /// given that the module already exists with the buffers already set.
  /// Appends new data and populates literalOffsetMap_.
  /// \param bufs containing the serialized literals.
  void initializeSerializedLiteralsLazy(LiteralBufferBuilder::Result &&bufs);

  /// Adds a compiled regexp to the module table.
  /// \return the index of the regexp in the table.
  uint32_t addRegExp(CompiledRegExp *regexp) {
    return bm_.addRegExp(regexp);
  }

  /// Add filename to the filename table.
  /// \return the index of the string.
  uint32_t addFilename(llvh::StringRef filename) {
    return debugInfoGenerator_.addFilename(filename);
  }

  /// Add scoping info to the scoping info table.
  /// \return the index of the scoping info.
  uint32_t addScopingInfo(const DebugScopingInfo &scopingInfo) {
    return debugInfoGenerator_.addScopingInfo(scopingInfo);
  }

  /// Set the segment ID for this module.
  void setSegmentID(uint32_t id) {
    bm_.setSegmentID(id);
  }

  /// Adds a CJS module entry to the table.
  void addCJSModule(uint32_t functionID, uint32_t nameID) {
    bm_.addCJSModule(functionID, nameID);
  }

  /// Adds a statically-resolved CJS module entry to the table.
  /// \param moduleID the index of the CJS module (incremented each call).
  void addCJSModuleStatic(uint32_t moduleID, uint32_t functionID) {
    bm_.addCJSModuleStatic(moduleID, functionID);
  }

  /// Adds a function source entry to the table.
  /// \param functionID the index of the function.
  /// \param stringID the index of the corresponding source in the string table.
  void addFunctionSource(uint32_t functionID, uint32_t stringID) {
    bm_.addFunctionSource(functionID, stringID);
  }

  /// For a given instruction \p inst that has an associated serialized literal,
  /// obtain the offset of the literal in the associated buffer. In case of
  /// an object literal, it is a pair of offsets (key and value). In case of
  /// array literal, only the first offset is used.
  LiteralBufferBuilder::LiteralOffset serializedLiteralOffsetFor(
      const Instruction *inst) const {
    assert(
        literalOffsetMap_.count(inst) &&
        "instruction has no serialized literal");
    return literalOffsetMap_.find(inst)->second;
  }

 private:
  /// Collects all strings in the functions which have been added to the
  /// functionIDMap_ and populates the string table in the BytecodeModule.
  /// Populates unicodeFunctionSources_ if reencoding of function sources was
  /// required.
  /// Must be called exactly once per generation.
  void collectStrings();

  /// Generate all functions added to the functionIDMap_.
  /// \pre all strings and literals have been collected.
  /// \param replacedFunctionID the ID of an existing BytecodeFunction, which
  ///   the function added with that ID replaces only if all the functions
  ///   are generated.
  /// \return true on success, false otherwise.
  bool generateAddedFunctions(
      OptValue<uint32_t> replacedFunctionID = llvh::None);
};

/// This class is used by the hermes backend.
/// It wraps all data required to generate the bytecode for a function.
class BytecodeFunctionGenerator : public BytecodeInstructionGenerator {
  // The bytecode module generator.
  BytecodeModuleGenerator &BMGen_;

  /// Exception handler table.
  std::vector<HBCExceptionHandlerInfo> exceptionHandlers_{};

  /// Size of the frame on stack (i.e. number of virtual registers used).
  uint32_t frameSize_{0};

  DebugSourceLocation sourceLocation_;
  std::vector<DebugSourceLocation> debugLocations_{};

  /// Table mapping variable names to frame locations.
  std::vector<Identifier> debugVariableNames_;

  /// Lexical parent function ID, i.e. the lexically containing function.
  OptValue<uint32_t> lexicalParentID_{};

  /// The size (in bytes) of the bytecode array in this function.
  /// Set by calling bytecodeGenerationComplete once all the opcodes are
  /// generated.
  uint32_t bytecodeSize_{0};

  /// Whether we are done generating this bytecode.
  /// Set to true by calling bytecodeGenerationComplete.
  bool complete_{false};

  /// Sizes of the read and write property caches in this function.
  /// Note that the size is reported as 255 when it is 255, or when it is 256
  /// (i.e., when we're using index 255, the "sticky" overflow index).  It's not
  /// worth expanding the width of these fields to 2 bytes for this case;
  /// instead, we will just conservatively allocate a 256-element cache when the
  /// size is 255.  This wastes at most one cache slot in a rare case.
  uint8_t readCacheSize_{0};
  uint8_t writeCacheSize_{0};
  /// The size of the private name cache index in this function.  (Same comment
  /// as above applies when the size is 255.
  uint8_t privateNameCacheSize_{0};

  /// The jump table for this function (if any)
  /// this vector consists of jump table for each UIntSwitchImm instruction,
  /// laid out sequentially. Each entry is a relative jump.
  std::vector<uint32_t> jumpTable_{};

  /// The string switch table for this function (if any).
  /// this vector consists of the string switch tables for each StringSwitchImm
  /// instruction, laid out sequentially.
  std::vector<StringSwitchTableCase> stringSwitchTable_{};

  explicit BytecodeFunctionGenerator(
      BytecodeModuleGenerator &BMGen,
      uint32_t frameSize)
      : BytecodeInstructionGenerator(), BMGen_(BMGen), frameSize_(frameSize) {}

 public:
  /// Runs ISel and generates a BytecodeFunction for the given
  /// register-allocated function.
  ///
  /// \param F the function for which we're generating bytecode.
  /// \param functionID the ID of the function in BMGen.
  /// \param BMGen the BytecodeModuleGenerator owning the BytecodeModule to
  /// generate into.
  /// \param RA the register allocator for the function.
  /// \param options the bytecode generation options.
  /// \param debugCache the cache of debug information, populated by ISel.
  /// \param sourceMapGen the source map generator, or nullptr if not needed.
  /// \param debugInfoGenerator the debug info generator to populate with
  /// information from the BytecodeFunction.
  ///
  /// \return the generated BytecodeFunction. On error, report an error and
  /// return nullptr.
  static std::unique_ptr<BytecodeFunction> generateBytecodeFunction(
      Function *F,
      uint32_t functionID,
      uint32_t nameID,
      BytecodeModuleGenerator &BMGen,
      HVMRegisterAllocator &RA,
      BytecodeGenerationOptions options,
      FileAndSourceMapIdCache &debugCache,
      DebugInfoGenerator &debugInfoGenerator);

  /// \return the generator for the current BytecodeModule.
  BytecodeModuleGenerator &getBytecodeModuleGenerator() {
    return BMGen_;
  }

  /// \return the generator for the current BytecodeModule.
  const BytecodeModuleGenerator &getBytecodeModuleGenerator() const {
    return BMGen_;
  }

  /// Create a bytecode function.
  /// \p nameID is an index to the string table representing the name
  /// of this function. If unspecified, the creator basically don't
  /// care about the name and we default it to the first entry in the
  /// string table. We guarantee that the string table at runtime will
  /// have at least one entry for this purpose.
  std::unique_ptr<BytecodeFunction> generateBytecodeFunction(
      Function::ProhibitInvoke prohibitInvoke,
      ValueKind valueKind,
      bool strictMode,
      uint32_t paramCount,
      uint32_t nameID = 0);

  unsigned getFunctionID(Function *F) {
    return BMGen_.addFunction(F);
  }

  /// \return the ID in the bytecode's string table for a given literal string
  /// \p value.
  unsigned getStringID(LiteralString *value) const {
    return BMGen_.getStringID(value->getValue().str());
  }

  /// \return the ID in the bytecode's string table for a given literal string
  /// \p value, assuming it has been registered for us as an identifier.
  unsigned getIdentifierID(LiteralString *value) const {
    return BMGen_.getIdentifierID(value->getValue().str());
  }

  /// Adds a parsed bigint to the module table.
  /// \return the index of the bigint in the table.
  uint32_t addBigInt(bigint::ParsedBigInt bigint) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    return BMGen_.addBigInt(std::move(bigint));
  }

  /// Adds a compiled regexp to the module table.
  /// \return the index of the regexp in the table.
  uint32_t addRegExp(CompiledRegExp *regexp) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    return BMGen_.addRegExp(regexp);
  }

  /// Add filename to the filename table.
  /// \return the index of the string.
  uint32_t addFilename(llvh::StringRef filename) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    return BMGen_.addFilename(filename);
  }

  /// Add scoping info to the scoping info table.
  /// \return the index of the scoping info.
  uint32_t addScopingInfo(const DebugScopingInfo &scopingInfo) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    return BMGen_.addScopingInfo(scopingInfo);
  }

  void addExceptionHandler(HBCExceptionHandlerInfo info) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    exceptionHandlers_.push_back(info);
  }

  /// Set the source location of the function definition.
  void setSourceLocation(const DebugSourceLocation &location) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    sourceLocation_ = location;
  }

  const DebugSourceLocation &getSourceLocation() const {
    return sourceLocation_;
  }

  /// Add the location of an opcode.
  /// A line of 0 indicates that there is no source location.
  void addDebugSourceLocation(const DebugSourceLocation &info);
  const std::vector<DebugSourceLocation> &getDebugLocations() const {
    return debugLocations_;
  }

  bool hasDebugInfo() const {
    return !debugLocations_.empty() || lexicalParentID_ ||
        !debugVariableNames_.empty();
  }

  /// Add a debug variable named \name.
  void setDebugVariableNames(std::vector<Identifier> names) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    debugVariableNames_ = std::move(names);
  }

  /// \return the list of debug variable names.
  llvh::ArrayRef<Identifier> getDebugVariableNames() const {
    return debugVariableNames_;
  }

  /// Set the lexical parent ID to \p parentId.
  void setLexicalParentID(OptValue<uint32_t> parentID) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    lexicalParentID_ = parentID;
  }

  /// \return the lexical parent ID (that is, the function lexically enclosing
  /// this function) or None if there is no lexical parent ID (i.e. the function
  /// is global).
  OptValue<uint32_t> getLexicalParentID() const {
    return lexicalParentID_;
  }

  /// Shift the bytecode stream starting from \p loc left by 3 bytes.
  /// This is called when a long jump offset is found to fit into 1 byte.
  void shrinkJump(offset_t loc);

  /// Update \p bytes number of bytes in opcode stream location \loc
  /// with \p newVal.
  void updateJumpTarget(offset_t loc, int newVal, int bytes);

  /// Update the jump/switch table offset of a SwitchImm instruction during
  /// jump relocation.
  /// \param loc location of the instruction
  /// \param tableOffset offset of the location to be updated in table section.
  /// \param instLoc offset will be computed relative to this position in
  ///   bytecode vector.
  void updateTableOffset(offset_t loc, uint32_t tableOffset, uint32_t instLoc);

  /// Change the opcode of a long jump instruction into a short jump.
  inline void longToShortJump(offset_t loc) {
    switch (opcodes_[loc]) {
#define DEFINE_JUMP_LONG_VARIANT(shortName, longName) \
  case longName##Op:                                  \
    opcodes_[loc] = shortName##Op;                    \
    break;
#include "hermes/BCGen/HBC/BytecodeList.def"
      default:
        llvm_unreachable("Unknown jump opcode");
    }
  }

  /// \return true if the jump instruction at \p loc is a long jump with a
  /// corresponding short variant.
  bool hasShortJumpVariant(offset_t loc) const {
    switch (opcodes_[loc]) {
#define DEFINE_JUMP_LONG_VARIANT(shortName, longName) case longName##Op:
#include "hermes/BCGen/HBC/BytecodeList.def"
      return true;
      default:
        return false;
    }
  }

  /// \return the size of the frame.
  uint32_t getFrameSize() const {
    return frameSize_;
  }

  void setReadCacheSize(uint8_t sz) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    this->readCacheSize_ = sz;
  }
  void setWriteCacheSize(uint8_t sz) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    this->writeCacheSize_ = sz;
  }
  void setPrivateNameCacheSize(uint8_t sz) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    this->privateNameCacheSize_ = sz;
  }

  /// Set the jump table for this function, if any.
  void setJumpTable(std::vector<uint32_t> &&jumpTable) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    assert(!jumpTable.empty() && "invoked with no jump table");

    jumpTable_ = std::move(jumpTable);
  }

  /// The size of the jump table.
  size_t jumpTableSize() const {
    return jumpTable_.size();
  }

  /// Set the string switch table for this function, if any.
  void setStringSwitchTable(
      std::vector<StringSwitchTableCase> &&stringSwitchTable) {
    assert(
        !complete_ &&
        "Cannot modify BytecodeFunction after call to bytecodeGenerationComplete.");
    assert(!stringSwitchTable.empty() && "invoked with no string switch table");

    stringSwitchTable_ = std::move(stringSwitchTable);
  }

  /// Signal that bytecode generation is finalized.
  /// Sets bytecodeSize_ and adds jump tables.
  void bytecodeGenerationComplete();
};

} // namespace hbc
} // namespace hermes

#endif // HERMES_BCGEN_HBC_BYTECODEGENERATOR_H
