/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_BCGEN_HBC_BYTECODE_H
#define HERMES_BCGEN_HBC_BYTECODE_H

#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/BCGen/HBC/BytecodeInstructionGenerator.h"
#include "hermes/BCGen/HBC/BytecodeStream.h"
#include "hermes/BCGen/HBC/DebugInfo.h"
#include "hermes/BCGen/HBC/StringKind.h"
#include "hermes/BCGen/HBC/UniquingStringLiteralTable.h"
#include "hermes/BCGen/ShapeTableEntry.h"
#include "hermes/Regex/RegexSerialization.h"
#include "hermes/Support/BigIntSupport.h"
#include "hermes/Support/StringTableEntry.h"
#include "hermes/Utils/Options.h"

#include "llvh/ADT/ArrayRef.h"

#include <memory>

namespace hermes {
class SourceMapGenerator;

namespace sema {
class SemContext;
}

namespace hbc {
class BCProviderFromSrc;

// This class represents the in-memory representation of the bytecode function.
class BytecodeFunction {
  /// The bytecode executable. If this function contains switch statements, then
  /// the jump tables are also inlined in a 4-byte aligned block at the end of
  /// this vector.
  /// During execution, this vector can be executed directly because the vector
  /// storage will be 4-byte aligned.
  /// During serialisation, the jump tables and opcodes have to be separated so
  /// that the jump tables can be realigned based on the overall bytecode
  /// layout.
  const std::vector<opcode_atom_t> opcodesAndJumpTables_;

  /// Header that stores all the metadata of this function.
  FunctionHeader header_;

  /// Offsets of this function in debug info.
  DebugOffsets debugOffsets_{};

  /// Offset of the function info entry for this function, or 0 if none exists.
  uint32_t infoOffset = 0;

  /// List of exception handlers.
  std::vector<HBCExceptionHandlerInfo> exceptions_;

  /// Data to compile more code within this BytecodeFunction, if applicable.
  /// If this function is NOT lazy, the IR function is used for compiling new
  /// 'eval' code as a child.
  /// This effectively owns the IR function, and will destroy it when it is
  /// destroyed.
  /// If the IR function is destroyed prior to the BytecodeFunction,
  /// this must be reset to nullptr.
  Function *functionIR_ = nullptr;

  /// Error message if this was a lazy function which failed to compile.
  /// Stored here to avoid rerunning compilation.
  /// Compilation can fail even in bytecode generation, and running IRGen again
  /// for new bytecode and/or having to deal with half-emitted child functions
  /// would be inconvenient.
  /// This is an early error, which would have been caught as a SyntaxError in
  /// eager mode.
  llvh::Optional<std::string> lazyCompileError_{};

 public:
  std::vector<DebugScopingInfo> scopingInfo;

  /// Used during serialization. \p opcodes will be swapped after this call.
  explicit BytecodeFunction(
      std::vector<opcode_atom_t> &&opcodesAndJumpTables,
      FunctionHeader &&header,
      std::vector<HBCExceptionHandlerInfo> &&exceptionHandlers)
      : opcodesAndJumpTables_(std::move(opcodesAndJumpTables)),
        header_(std::move(header)),
        exceptions_(std::move(exceptionHandlers)) {}

  /// Destroys the IR Function if it's not null.
  ~BytecodeFunction();

  const FunctionHeader &getHeader() const {
    return header_;
  }

  FunctionHeaderFlag &mutableFlags() {
    return header_.flags;
  }

  void setOffset(uint32_t offset) {
    header_.setOffset(offset);
  }
  uint32_t getOffset() const {
    return header_.getOffset();
  }

  void setInfoOffset(uint32_t offset) {
    infoOffset = offset;
  }
  uint32_t getInfoOffset() const {
    return infoOffset;
  }

  bool isStrictMode() const {
    return header_.flags.getStrictMode();
  }

  /// Return the entire opcode array for execution, including the inlined jump
  /// tables.
  llvh::ArrayRef<opcode_atom_t> getOpcodeArray() const {
    return opcodesAndJumpTables_;
  }

  /// Return only the opcodes for serialisation. The jump tables need to be
  /// accessed separately so they can be correctly inlined.
  llvh::ArrayRef<opcode_atom_t> getOpcodesOnly() const {
    return {opcodesAndJumpTables_.data(), header_.getBytecodeSizeInBytes()};
  }

  /// Return the jump table portion of the opcode array. This is useful for the
  /// bytecode serialisation code when it is aligning the jump tables.
  llvh::ArrayRef<uint32_t> getJumpTablesOnly() const;

  uint32_t getExceptionHandlerCount() const {
    return exceptions_.size();
  }

  llvh::ArrayRef<HBCExceptionHandlerInfo> getExceptionHandlers() const {
    return exceptions_;
  }

  const DebugOffsets *getDebugOffsets() const {
    return &debugOffsets_;
  }

  void setDebugOffsets(DebugOffsets offsets) {
    debugOffsets_ = offsets;
    header_.flags.setHasDebugInfo(
        debugOffsets_.sourceLocations != DebugOffsets::NO_OFFSET);
  }

  void setFunctionIR(Function *functionIR) {
    functionIR_ = functionIR;
  }
  Function *getFunctionIR() {
    return functionIR_;
  }

  /// \return true if the function should be compiled lazily.
  bool isLazy() const {
    return functionIR_ && functionIR_->isLazy();
  }

  /// Set the lazy compile error.
  /// May only be called once.
  void setLazyCompileError(std::string &&error) {
    assert(
        !lazyCompileError_.hasValue() && "Cannot set lazy compile error twice");
    lazyCompileError_ = std::move(error);
  }
  /// \return the lazy compile error if this function has already failed to
  /// compile once.
  llvh::Optional<llvh::StringRef> getLazyCompileError() const {
    if (lazyCompileError_) {
      return llvh::StringRef{*lazyCompileError_};
    }
    return llvh::None;
  }
};

/// This class represents the in-memory representation of the bytecode module.
/// It contains pointers into Context, so it must not outlive the associated
/// Context.
class BytecodeModule {
  using FunctionList = std::vector<std::unique_ptr<BytecodeFunction>>;
  using SerializableBufferTy = std::vector<unsigned char>;

  /// Pointer to owning BCProviderFromSrc if this BytecodeModule is owned
  /// by a BCProviderFromSrc, nullptr otherwise.
  /// Allows updating the table references in the BCProvider when a new
  /// lazy function is compiled.
  /// If running from source, there's always a BCProviderFromSrc, so this will
  /// always be set to non-null.
  BCProviderFromSrc *bcProviderFromSrc_ = nullptr;

  /// A list of bytecode functions.
  FunctionList functions_{};

  /// Index of the top level function, code that gets executed in the global
  /// scope.
  uint32_t globalFunctionIndex_{};

  /// Run-length encoding representing the kinds of strings in the table.
  std::vector<StringKind::Entry> stringKinds_;

  /// The list of identifier hashes, corresponding to the string entries
  /// marked as identifiers, in order.
  std::vector<uint32_t> identifierHashes_;

  /// The global string table to represent each string in the string storage.
  StringLiteralTable stringTable_{};

  /// The global bigint table for assigning IDs to bigints.
  bigint::UniquingBigIntTable bigIntTable_;

  /// The list of unique RegExp objects.
  UniquingRegExpTable regExpTable_;

  /// A table containing debug info (if compiled with -g).
  DebugInfo debugInfo_;

  /// Object/Array Literal Value Buffer table.
  SerializableBufferTy literalValueBuffer_;

  /// Object Key Buffer table.
  SerializableBufferTy objKeyBuffer_;

  /// Object shape table.
  std::vector<ShapeTableEntry> objShapeTable_;

  /// The segment ID corresponding to this BytecodeModule.
  /// This uniquely identifies this BytecodeModule within a set of modules
  /// which were compiled at the same time (and will correspond to a set of
  /// RuntimeModules in a Domain).
  uint32_t segmentID_;

  /// The number of StringSwitchImm instructions in the BytecodeModule.
  uint32_t numStringSwitchImm_{0};

  /// Table which indicates where to find the different CommonJS modules.
  /// Mapping from {filename ID => function index}.
  std::vector<std::pair<uint32_t, uint32_t>> cjsModuleTable_{};

  /// Table which indicates where to find the different CommonJS modules.
  /// Used if the modules have been statically resolved.
  /// Mapping from {global module ID => function index}.
  std::vector<std::pair<uint32_t, uint32_t>> cjsModuleTableStatic_{};

  /// Mapping function ids to the string table offsets that store their
  /// non-default source code representation that would be used by `toString`.
  /// These are only available when functions are declared with source
  /// visibility directives such as 'show source', 'hide source', etc.
  std::vector<std::pair<uint32_t, uint32_t>> functionSourceTable_{};

  /// Storing information about the bytecode, needed when it is loaded by the
  /// runtime.
  BytecodeOptions options_{};

 public:
  /// Create a simple BytecodeModule with only functionCount set.
  explicit BytecodeModule(uint32_t functionCount) {
    functions_.resize(functionCount);
  }

  /// Create an empty BytecodeModule with no functions.
  explicit BytecodeModule() = default;

  /// Destructor cleans up any Function IR that's been kept around for Eval
  /// data.
  ~BytecodeModule();

  /// Set the bcProviderFromSrc_ field to an owning BCProviderFromSrc.
  void setBCProviderFromSrc(BCProviderFromSrc *bcProviderFromSrc) {
    bcProviderFromSrc_ = bcProviderFromSrc;
  }
  /// Set the bcProviderFromSrc_ field to an owning BCProviderFromSrc.
  BCProviderFromSrc *getBCProviderFromSrc() {
    return bcProviderFromSrc_;
  }

  /// Add an uncompiled BytecodeFunction to the table, to track that we will
  /// eventually compile it.
  void addFunction() {
    functions_.push_back(nullptr);
  }
  /// Remove the uncompiled BytecodeFunctions at the end of the table, which
  /// were added but not generated because bytecode generation failed.
  void popUncompiledFunctions();
  const FunctionList &getFunctionTable() const {
    return functions_;
  }
  uint32_t getNumFunctions() const {
    return functions_.size();
  }

  void setGlobalFunctionIndex(uint32_t global) {
    globalFunctionIndex_ = global;
  }
  uint32_t getGlobalFunctionIndex() const {
    return globalFunctionIndex_;
  }

  /// Add a new bytecode function to the module at \p index.
  void setFunction(uint32_t index, std::unique_ptr<BytecodeFunction> F);

  BytecodeFunction &getFunction(unsigned index);

  BytecodeFunction &getGlobalCode() {
    return getFunction(globalFunctionIndex_);
  }

  /// Create the stringKinds_ and identifierHashes_ fields based on the
  /// stringTable.
  void populateStringMetadataFromStringTable() {
    assert(stringKinds_.empty() && "String table must be empty");
    stringKinds_ = stringTable_.getStringKinds();
    identifierHashes_ = stringTable_.getIdentifierHashes();
  }

  /// Create the stringKinds_ and identifierHashes_ fields based on the existing
  /// stringTable_, only copying the data for strings added since the last
  /// initialization of the fields.
  /// Used for lazy compilation, where the BytecodeModule is appended to as we
  /// compile new lazy functions.
  /// \param startIdx the index of stringTable_'s strings_ from which to start
  /// generating the kinds and hashes.
  void appendStringMetadataFromStringTable(uint32_t startIdx) {
    auto newKinds = stringTable_.getStringKinds(startIdx);
    stringKinds_.insert(stringKinds_.end(), newKinds.begin(), newKinds.end());
    auto newHashes = stringTable_.getIdentifierHashes(startIdx);
    identifierHashes_.insert(
        identifierHashes_.end(), newHashes.begin(), newHashes.end());
  }

  StringLiteralTable &getStringLiteralTableMut() {
    return stringTable_;
  }

  llvh::ArrayRef<StringKind::Entry> getStringKinds() const {
    return stringKinds_;
  }

  /// \return the number of identifiers.
  uint32_t getIdentifierCount() const {
    return identifierHashes_.size();
  }

  llvh::ArrayRef<uint32_t> getIdentifierHashes() const {
    return identifierHashes_;
  }

  unsigned getIdentifierID(llvh::StringRef str) const {
    return stringTable_.getIdentifierID(str);
  }
  unsigned getStringID(llvh::StringRef str) const {
    return stringTable_.getStringID(str);
  }

  uint32_t getStringTableSize() const {
    return stringTable_.count();
  }

  StringTableEntry::StringTableRefTy getStringTable() const {
    return stringTable_.getStringTableView();
  }

  uint32_t getStringStorageSize() const {
    return stringTable_.getStringStorageView().size();
  }

  StringTableEntry::StringStorageRefTy getStringStorage() const {
    return stringTable_.getStringStorageView();
  }

  uint32_t addBigInt(bigint::ParsedBigInt &&bigint) {
    return bigIntTable_.addBigInt(std::move(bigint));
  }

  llvh::ArrayRef<bigint::BigIntTableEntry> getBigIntTable() const {
    return bigIntTable_.getEntryList();
  }

  llvh::ArrayRef<uint8_t> getBigIntStorage() const {
    return bigIntTable_.getDigitsBuffer();
  }

  /// Add a new RegExp to the module.
  /// \param regExp the RegExp object to add, stored in the Context.
  uint32_t addRegExp(CompiledRegExp *regExp) {
    return regExpTable_.addRegExp(regExp);
  }

  llvh::ArrayRef<RegExpTableEntry> getRegExpTable() const {
    return regExpTable_.getEntryList();
  }

  llvh::ArrayRef<uint8_t> getRegExpStorage() const {
    return regExpTable_.getBytecodeBuffer();
  }

  void setSegmentID(uint32_t segmentID) {
    segmentID_ = segmentID;
  }
  uint32_t getSegmentID() const {
    return segmentID_;
  }

  /// Returns the total number of StringSwitchImm instructions in the module.
  uint32_t getNumStringSwitchImmInstrs() const {
    return numStringSwitchImm_;
  }
  /// Notes that a new StringSwitchImm instruction has been created.
  /// Counts the number created, returning a unique 0-based index for
  /// each such instruction.
  uint32_t addStringSwitchImmInstr() {
    return numStringSwitchImm_++;
  }

  void addCJSModule(uint32_t functionID, uint32_t nameID) {
    assert(
        cjsModuleTableStatic_.empty() &&
        "Statically resolved modules must be in cjsModulesStatic_");
    cjsModuleTable_.push_back({nameID, functionID});
  }

  llvh::ArrayRef<std::pair<uint32_t, uint32_t>> getCJSModuleTable() const {
    return cjsModuleTable_;
  }

  void addCJSModuleStatic(uint32_t moduleID, uint32_t functionID) {
    assert(
        cjsModuleTableStatic_.empty() &&
        "Statically resolved modules must be in cjsModulesStatic_");
    cjsModuleTableStatic_.push_back({moduleID, functionID});
  }

  llvh::ArrayRef<std::pair<uint32_t, uint32_t>> getCJSModuleTableStatic()
      const {
    return cjsModuleTableStatic_;
  }

  void addFunctionSource(uint32_t functionID, uint32_t stringID) {
    functionSourceTable_.push_back({functionID, stringID});
  }

  llvh::ArrayRef<std::pair<uint32_t, uint32_t>> getFunctionSourceTable() const {
    return functionSourceTable_;
  }

  DebugInfo &getDebugInfo() {
    return debugInfo_;
  }

  /// Initialize the literal buffers.
  void initializeSerializedLiterals(
      std::vector<unsigned char> &&literalValueBuffer,
      std::vector<unsigned char> &&objKeyBuffer,
      std::vector<ShapeTableEntry> &&objShapeTable) {
    literalValueBuffer_ = std::move(literalValueBuffer);
    objKeyBuffer_ = std::move(objKeyBuffer);
    objShapeTable_ = std::move(objShapeTable);
  }

  /// Append to the buffers based on the buffers passed in.
  void appendSerializedLiterals(
      std::vector<unsigned char> &&literalValueBuffer,
      std::vector<unsigned char> &&objKeyBuffer,
      std::vector<ShapeTableEntry> &&objShapeTable) {
    literalValueBuffer_.insert(
        literalValueBuffer_.end(),
        literalValueBuffer.begin(),
        literalValueBuffer.end());
    objKeyBuffer_.insert(
        objKeyBuffer_.end(), objKeyBuffer.begin(), objKeyBuffer.end());
    objShapeTable_.insert(
        objShapeTable_.end(), objShapeTable.begin(), objShapeTable.end());
  }

  /// Returns the amount of bytes in the object key buffer
  uint32_t getObjectKeyBufferSize() const {
    return objKeyBuffer_.size();
  }

  /// Returns the amount of bytes in the literal value buffer.
  uint32_t getLiteralValueBufferSize() const {
    return literalValueBuffer_.size();
  }

  /// Returns the amount of bytes in the literal value buffer.
  uint32_t getObjectShapeTableSize() const {
    return objShapeTable_.size();
  }

  /// Returns a reference to the literal value buffer.
  llvh::ArrayRef<unsigned char> getLiteralValueBuffer() const {
    return literalValueBuffer_;
  }

  /// Returns a reference to the object keys.
  llvh::ArrayRef<unsigned char> getObjectKeyBuffer() const {
    return llvh::ArrayRef<unsigned char>(objKeyBuffer_);
  }

  /// Returns a reference to the object shapes.
  llvh::ArrayRef<ShapeTableEntry> getObjectShapeTable() const {
    return objShapeTable_;
  }

  /// Returns a pair, where the first element is the key buffer starting at
  /// keyIdx and the second element is the value buffer starting at valIdx
  std::pair<llvh::ArrayRef<unsigned char>, llvh::ArrayRef<unsigned char>>
  getObjectBufferAtOffset(unsigned keyIdx, unsigned valIdx) const {
    return {
        llvh::ArrayRef<unsigned char>(objKeyBuffer_).slice(keyIdx),
        llvh::ArrayRef<unsigned char>(literalValueBuffer_).slice(valIdx)};
  }

  /// Populate the source map \p sourceMap with the debug information.
  void populateSourceMap(SourceMapGenerator *sourceMap) const;

  BytecodeOptions &getBytecodeOptionsMut() {
    return options_;
  }
  BytecodeOptions getBytecodeOptions() const {
    return options_;
  }
};

} // namespace hbc
} // namespace hermes

#endif
