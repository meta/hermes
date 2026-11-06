/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/BCGen/SH/SH.h"

#include "LineDirectiveEmitter.h"
#include "LoweringPasses.h"
#include "SHRegAlloc.h"
#include "hermes/AST/NativeContext.h"
#include "hermes/BCGen/FunctionInfo.h"
#include "hermes/BCGen/HBC/Passes.h"
#include "hermes/BCGen/LiteralBufferBuilder.h"
#include "hermes/BCGen/LowerScopes.h"
#include "hermes/BCGen/LowerStoreInstrs.h"
#include "hermes/BCGen/Lowering.h"
#include "hermes/BCGen/MovElimination.h"
#include "hermes/BCGen/RemoveMovs.h"
#include "hermes/BCGen/SerializedLiteralGenerator.h"
#include "hermes/BCGen/ShapeTableEntry.h"
#include "hermes/IR/Analysis.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRVerifier.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Regex/RegexSerialization.h"
#include "hermes/Support/BigIntSupport.h"
#include "hermes/Support/DenseMapInfoSpecializations.h"
#include "hermes/Support/HashString.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VMLayouts/StackFrameLayout.h"
#include "llvh/ADT/MapVector.h"

#include "llvh/ADT/BitVector.h"
#include "llvh/ADT/SetVector.h"

using namespace hermes;

namespace {
/// Generates the correct label for BasicBlock \p B based on \p bbMap and
/// outputs it through \p OS.
void generateBasicBlockLabel(
    BasicBlock *B,
    llvh::raw_ostream &OS,
    const llvh::DenseMap<BasicBlock *, unsigned> &bbMap) {
  OS << "L" << bbMap.find(B)->second;
}

const char *boolStr(bool b) {
  return b ? "true" : "false";
}

/// Helper to unique and store the contents of strings.
class SHStringTable {
  StringSetVector strings_;

  /// Append a printable representation of the character \p c, that when
  /// enclosed in single quotes in a C file, will produce the same character.
  /// NOTE: We deliberately print each character separately, since this emits a
  /// mix of printable characters and hexadecimal escapes, which consume all
  /// subsequent hexadecimal characters. For example, consider emitting the
  /// escape '\x0' followed by the character '1', if we emitted them in a single
  /// string literal, the compiler would parse them as the single escape '\x01'.
  static void appendEscaped(std::string &out, char16_t c) {
    // To improve the readability of the generated source, print certain
    // characters directly. We consider the character printable if it is ASCII,
    // returns true for isprint, and is not a ' or a \.
    bool canPrint = c <= 127 && std::isprint(c) && c != u'\'' && c != u'\\';
    if (canPrint) {
      out += c;
    } else {
      // If the character is not printable, emit a hexadecimal escape sequence
      // for it.
      out += "\\x";
      const char *hexdig = "0123456789ABCDEF";
      out += hexdig[c >> 12];
      out += hexdig[(c >> 8) & 0xF];
      out += hexdig[(c >> 4) & 0xF];
      out += hexdig[c & 0xF];
    }
  }

 public:
  /// \return the current number of strings in the table.
  uint32_t size() const {
    return strings_.size();
  }

  /// Add the string \p str to the table if it is not already in it.
  /// \return the index associated with str.
  uint32_t add(llvh::StringRef str) {
    return strings_.insert(str);
  }

  /// Turn the table of strings into the SH C data structures that are necessary
  /// to provide strings for the module.
  void generate(llvh::raw_ostream &os) const {
    struct StringEntry {
      uint32_t offset, length, hash;
    };
    std::vector<StringEntry> stringEntries;
    stringEntries.reserve(strings_.size());

    // Buffers containing the escaped strings, which can be emitted directly
    // into the C code.
    std::string asciiStr, u16Str;
    // The number of characters emitted so far in each of the buffers.
    uint32_t asciiOffset = 0, u16Offset = 0;

    for (const auto &str : strings_) {
      StringEntry entry;
      if (isAllASCII(str)) {
        asciiStr += "  ";
        // The given string is entirely ASCII. Emit the sequence of ASCII
        // characters to asciiStr.
        for (char c : str) {
          asciiStr += '\'';
          appendEscaped(asciiStr, c);
          asciiStr += "', ";
        }
        // Strings are null terminated to make them easier to debug.
        asciiStr += "'\\0',\n";
        entry = {
            asciiOffset,
            (uint32_t)str.size(),
            hashString(llvh::ArrayRef<char>{str.data(), str.size()})};
        asciiOffset += str.size() + 1;
      } else {
        u16Str += "  ";
        // If the string is non-ASCII, convert it to UTF-16, and then perform
        // the same steps as the ASCII string above.
        std::u16string strStorage;
        convertUTF8WithSurrogatesToUTF16(
            std::back_inserter(strStorage),
            str.data(),
            str.data() + str.size());
        for (char16_t c : strStorage) {
          u16Str += "u'";
          appendEscaped(u16Str, c);
          u16Str += "', ";
        }
        u16Str += "u'\\0',\n";
        entry = {
            u16Offset | (1 << 31),
            (uint32_t)strStorage.size(),
            hashString(
                llvh::ArrayRef<char16_t>{
                    strStorage.data(), strStorage.size()})};
        u16Offset += strStorage.size() + 1;
      }
      stringEntries.push_back(entry);
    }

    // Example:
    // static const char s_ascii_pool[] = {
    //   'H', 'e', 'l', 'l', 'o', '\0',
    //   'W', 'o', 'r', 'l', 'd', '\0',
    // };
    // static const char16_t s_u16_pool[] = {
    // };
    // static const uint32_t s_strings[] = {
    //     0, 5, 0, 6, 5, 0,
    // };

    os << "static const char s_ascii_pool[] = {\n"
       << asciiStr << "};\n"
       << "static const char16_t s_u16_pool[] = {\n"
       << u16Str << "};\n"
       << "static const uint32_t s_strings[] = {";
    for (const auto &entry : stringEntries)
      os << entry.offset << "," << entry.length << "," << entry.hash << ",";
    os << "};\n";
  }
};

/// Emit \p buf as a C array of unsigned char named \p name.
static void generateBuffer(
    llvh::raw_ostream &os,
    llvh::StringRef name,
    llvh::ArrayRef<unsigned char> buf) {
  os << "static unsigned char " << name << "[" << buf.size() << "] = {";
  for (unsigned char c : buf) {
    os << (int)c << ",";
  }
  os << "};\n";
}

/// Emit the regexp bytecode storage and the table that locates each
/// regexp in it.
static void generateRegExpTable(
    llvh::raw_ostream &os,
    const UniquingRegExpTable &table) {
  generateBuffer(os, "s_regexp_storage", table.getBytecodeBuffer());
  os << "static const SHRegExpTableEntry s_regexp_table[] = {\n";
  for (const RegExpTableEntry &entry : table.getEntryList()) {
    os.indent(2);
    os << "{ .offset = " << entry.offset << ", .length = " << entry.length
       << " },\n";
  }
  os << "};\n";
}

class SHLiteralBuffers {
 public:
  /// Table of constants used to initialize constant arrays/object values.
  /// They are stored as chars in order to shorten bytecode size.
  std::vector<unsigned char> literalValueBuffer{};

  /// Table of constants used to initialize object keys.
  /// They are stored as chars in order to shorten bytecode size
  std::vector<unsigned char> objKeyBuffer{};

  /// Table of object literal shapes.
  std::vector<ShapeTableEntry> objShapeTable{};

  /// A map from instruction to literal offset in the corresponding buffers.
  /// \c arrayBuffer, \c objKeyBuffer, \c objliteralValBuffer.
  LiteralBufferBuilder::LiteralOffsetMapTy literalOffsetMap{};

  explicit SHLiteralBuffers(
      Module *M,
      SHStringTable &table,
      bool optimizationEnabled) {
    LiteralBufferBuilder::Result bufs = LiteralBufferBuilder::generate(
        M,
        [](const Function *) { return true; },
        [&table](llvh::StringRef str) { return table.add(str); },
        [&table](llvh::StringRef str) { return table.add(str); },
        optimizationEnabled);
    literalValueBuffer = std::move(bufs.literalValBuffer);
    objKeyBuffer = std::move(bufs.keyBuffer);
    objShapeTable = std::move(bufs.shapeTable);
    literalOffsetMap = std::move(bufs.offsetMap);
  }

  /// For a given instruction \p inst that has an associated serialized literal,
  /// obtain the offset of the literal in the associated buffer. In case of
  /// an object literal, it is a pair of offsets (key and value). In case of
  /// array literal, only the first offset is used.
  LiteralBufferBuilder::LiteralOffset serializedLiteralOffsetFor(
      const Instruction *inst) const {
    assert(
        literalOffsetMap.count(inst) &&
        "instruction has no serialized literal");
    return literalOffsetMap.find(inst)->second;
  }

  void generate(llvh::raw_ostream &os) const {
    generateBuffer(os, "s_literal_val_buffer", literalValueBuffer);
    generateBuffer(os, "s_obj_key_buffer", objKeyBuffer);
    // Generate the shape table.
    os << "static const SHShapeTableEntry s_obj_shape_table[] = {\n";
    for (auto &entry : objShapeTable) {
      os.indent(2);
      os << "{ .key_buffer_offset = " << entry.keyBufferOffset
         << ", .num_props = " << entry.numProps << " },\n";
    }
    os << "};\n";
  }
};

/// For each unique SourceCoords, maintain a corresponding index, which can be
/// used to retrieve that source location information at runtime. Invalid
/// coordinates have an index of 0.
class SHSrcLocationTable {
 public:
  /// The type of the index.
  using IdxTy = uint32_t;
  /// Index representing an invalid or unknown location.
  static constexpr IdxTy kInvalidLocIdx = 0;

  SHSrcLocationTable(SHStringTable &stringTable) : stringTable_(stringTable) {
    // This first element in the table is reserved for invalid locations.
    insert({stringTable_.add(""), 0, 0});
  }

  /// Get the source location index for the given \p coords, creating one if it
  /// does not exist.
  IdxTy getIndex(
      const SourceErrorManager &sm,
      const hermes::SourceErrorManager::SourceCoords &coords) {
    if (!coords.isValid())
      return kInvalidLocIdx;
    // If this source location has not been seen before, use the next
    // index. Otherwise, use the existing entry.
    uint32_t fileIdx = getFileIdx(sm, coords.bufId);
    return insert({fileIdx, coords.line, coords.col});
  }

  /// \return how many unique source locations exist in the table.
  uint32_t size() const {
    return indicesMap_.size();
  }

  /// Turn the table of source locations into the corresponding SH C data
  /// structures.
  void generate(llvh::raw_ostream &OS, const SourceErrorManager &sm) const {
    OS << "\nstatic const SHSrcLoc s_source_locations[] = {\n";
    for (const auto &loc : locations_) {
      auto [fileIdx, line, col] = loc;
      OS.indent(2);
      OS << "{ .filename_idx = " << fileIdx << ", .line = " << line
         << ", .column = " << col << " },\n";
    }
    OS << "};\n";
  }

 private:
  /// Tuple type of <filenameStringID, line, column>. The filenameStringID is
  /// the index into the global string table to find the name of the file of a
  /// location.
  using LocationTy = std::tuple<uint32_t, uint32_t, uint32_t>;
  /// A type for a cache that maps from source coordinate buffer id to index in
  /// the global string table.
  using FileIdxCacheTy = llvh::DenseMap<uint32_t, uint32_t>;
  /// Map a tuple type to a unique index into the source location table.
  llvh::DenseMap<LocationTy, IdxTy> indicesMap_;
  /// This contains all the keys of indicesMap_, in insertion order.
  std::vector<LocationTy> locations_;
  /// A cache mapping buffer id to filename index into the global string table.
  FileIdxCacheTy fileIdxCache_{};
  /// To avoid performing a hash lookup in most cases, cache the last found
  /// entry in the file index cache.
  FileIdxCacheTy::value_type *lastFileIdxEntry_ = nullptr;

  /// A reference to the global string table. Used for filenames.
  SHStringTable &stringTable_;

  /// Insert a source location and \return its index into this location table.
  IdxTy insert(LocationTy loc) {
    uint32_t nextID = indicesMap_.size();
    const auto [iter, success] = indicesMap_.insert({loc, nextID});
    // If we successfully inserted, that means the element hasn't been seen
    // before. We should also append to the sorted vector.
    if (success) {
      locations_.push_back(loc);
    }
    return iter->second;
  }

  /// \return the index into the global string table for the corresponding name
  /// of \p bufId. The \p bufId *must* come from a valid source coordinate
  /// location.
  uint32_t getFileIdx(const SourceErrorManager &sm, uint32_t bufId) {
    // Fast path- we are probably comparing the same bufId repeatedly.
    if (LLVM_LIKELY(lastFileIdxEntry_ && lastFileIdxEntry_->first == bufId)) {
      return lastFileIdxEntry_->second;
    }

    // Slower path- check our cache to see if we already have this bufId's
    // string table index.
    auto it = fileIdxCache_.find(bufId);
    if (LLVM_LIKELY(it != fileIdxCache_.end())) {
      lastFileIdxEntry_ = &*it;
      return it->second;
    }

    // Slowest path- ask the global string table for the ID of the buffer
    // filename. This is potentially a very expensive operation since looking up
    // the filename in the string table potentially requires scanning it three
    // times.
    llvh::StringRef filename = sm.getSourceUrl(bufId);
    auto stringTableIdx = stringTable_.add(filename);
    it = fileIdxCache_.try_emplace(bufId, stringTableIdx).first;
    lastFileIdxEntry_ = &*it;
    return it->second;
  }
};

class SHNativeJSFunctionTable {
  /// A map of function pointer -> function id
  llvh::DenseMap<Function *, unsigned> funcMap_{};
  /// A reference to the global string table. Used for function names.
  SHStringTable &stringTable_;

 public:
  explicit SHNativeJSFunctionTable(Module *M, SHStringTable &stringTable)
      : stringTable_(stringTable) {
    // Ensure that the top level function has an id of 0.
    auto topLevelFunc = M->getTopLevelFunction();
    funcMap_[topLevelFunc] = 0;
    unsigned funcCounter = 1;
    // Iterate through all the functions in the module, assigning them unique
    // incrementing IDs.
    for (auto &F : *M) {
      if (&F != topLevelFunc) {
        funcMap_[&F] = funcCounter++;
      }
    }
  }

  /// \return the unique index for the given \p F.
  uint32_t getIndex(Function *F) const {
    return funcMap_.find(F)->second;
  }

  /// \return size of the function table.
  size_t size() const {
    return funcMap_.size();
  }

  /// Generates the correct label for Function \p F, and outputs it to \p OS. If
  /// the JS function name contains characters that aren't allowed in C
  /// identifiers, they will be replaced by '_'.
  void generateFunctionLabel(Function *F, llvh::raw_ostream &OS) const {
    OS << '_' << getIndex(F) << '_';

    auto name = F->getInternalNameStr();
    for (auto c : name) {
      if (('0' <= c && c <= '9') || ('a' <= c && c <= 'z') ||
          ('A' <= c && c <= 'Z')) {
        OS << c;
      } else {
        // Replace illegal identifier characters with '_'.
        OS << '_';
      }
    }
  }

  /// Turn the table of function information into the corresponding SH C data
  /// structures.
  void generate(llvh::raw_ostream &OS) const {
    // Sort the keys by function index.
    std::vector<const Function *> sortedKeys{funcMap_.size()};
    for (auto &entry : funcMap_)
      sortedKeys[entry.second] = entry.first;

    OS << "\nstatic SHNativeFuncInfo s_function_info_table[] = {\n";
    for (const Function *F : sortedKeys) {
      uint32_t nameIdx = stringTable_.add(F->getOriginalOrInferredName().str());
      uint32_t argCount = F->getExpectedParamCountIncludingThis() - 1;
      auto kindVal = F->getKind();
      OS.indent(2);
      OS << "{ .name_index = " << nameIdx << ", .arg_count = " << argCount
         << ", .prohibit_invoke = "
         << computeProhibitInvoke(F->getProhibitInvoke())
         << ", .kind = " << computeFuncKind(kindVal) << " },\n";
    }
    OS << "};\n";
  }
};

struct ModuleGen {
  /// Table containing uniqued strings for the current module.
  SHStringTable stringTable{};

  /// Literal buffers for objects and arrays.
  SHLiteralBuffers literalBuffers;

  /// Maintain a table of all unique source file locations used by throwing
  /// instructions.
  SHSrcLocationTable srcLocationTable;

  /// Table of JS native functions
  SHNativeJSFunctionTable nativeFunctionTable;

  /// Uniqued table of the module's regexp literals and their bytecode.
  UniquingRegExpTable regExpTable{};

  explicit ModuleGen(Module *M, bool optimizationEnabled)
      : literalBuffers{M, stringTable, optimizationEnabled},
        srcLocationTable{stringTable},
        nativeFunctionTable{M, stringTable} {}
};

/// \return true if the SHLegacyValue representations of values \p a and \p b
/// can be compared directly (bitwise).
static bool canCompareStrictEqualityRaw(TypeContext &tc, Value *a, Value *b) {
  Type aType = a->getType();
  Type bType = b->getType();

  // If both can be numbers, then we can't compare because `-0` and `0` have
  // different bitwise representations.
  if (tc.canBeNumber(aType) && tc.canBeNumber(bType))
    return false;

  // If both can be bigint, then we can't compare because BigInts are compared
  // by values which are stored on the heap.
  if (tc.canBeBigInt(aType) && tc.canBeBigInt(bType))
    return false;

  // If both can be strings, then we can't compare because strings are compared
  // by their contents which are stored on the heap.
  if (tc.canBeString(aType) && tc.canBeString(bType))
    return false;

  // Otherwise, we can compare.
  return true;
}

class InstrGen {
 public:
  /// \p os is the output stream
  /// \p ra is the pre-ran register allocator for the current function
  /// \p tryIDs is a map from TryStartInst to a unique ID for the try/catch.
  InstrGen(
      llvh::raw_ostream &os,
      sh::SHRegisterAllocator &ra,
      const BytecodeGenerationOptions &options,
      const llvh::DenseMap<BasicBlock *, unsigned> &bbMap,
      Function &F,
      ModuleGen &moduleGen,
      uint32_t &nextWriteCacheIdx,
      uint32_t &nextReadCacheIdx,
      uint32_t &nextPrivateNameCacheIdx,
      const llvh::MapVector<TryStartInst *, uint32_t> &tryIDs)
      : os_(os),
        ra_(ra),
        options_(options),
        bbMap_(bbMap),
        F_(F),
        nativeContext_(F.getContext().getNativeContext()),
        moduleGen_(moduleGen),
        nextWriteCacheIdx_(nextWriteCacheIdx),
        nextReadCacheIdx_(nextReadCacheIdx),
        nextPrivateNameCacheIdx_(nextPrivateNameCacheIdx),
        tryIDs_(tryIDs) {
    (void)options_;
    if (!tryIDs_.empty())
      enclosingTrys_ = *findEnclosingTrysPerBlock(&F_);
  }

  /// Converts Instruction \p I into valid C code and outputs it through the
  /// ostream.
  void generate(Instruction &I) {
    switch (I.getKind()) {
#define DEF_VALUE(CLASS, PARENT) \
  case ValueKind::CLASS##Kind:   \
    return generate##CLASS(*cast<CLASS>(&I));
#define DEF_TAG(NAME, PARENT) \
  case ValueKind::NAME##Kind: \
    return generate##PARENT(*cast<PARENT>(&I));
#include "hermes/IR/Instrs.def"
      default:
        llvm_unreachable("Invalid kind");
    }
  }

 private:
  /// The ostream used to output C code
  llvh::raw_ostream &os_;

  /// The register allocator that was created for the current function
  sh::SHRegisterAllocator &ra_;

  /// Options for generation.
  const BytecodeGenerationOptions &options_;

  /// A map from basic blocks to unique numbers for identification
  const llvh::DenseMap<BasicBlock *, unsigned> &bbMap_;

  // The function being compiled.
  Function &F_;

  /// IR type context for the module being compiled.
  TypeContext &typeCtx_{F_.getParent()->getTypeContext()};

  // Info related to native compilation.
  NativeContext &nativeContext_;

  /// The state for the module currently being emitted.
  ModuleGen &moduleGen_;

  /// These starts at 0 and increments every time a cache index is used
  uint32_t &nextWriteCacheIdx_;
  uint32_t &nextReadCacheIdx_;
  uint32_t &nextPrivateNameCacheIdx_;

  /// Map from TryStart to an ID for the try/catch.
  /// Set the tryState to the ID when entering the try, restore it when leaving.
  const llvh::MapVector<TryStartInst *, uint32_t> &tryIDs_;

  /// Map from BasicBlock to the TryStartInst that encloses it, nullptr if none.
  /// If empty, there's no try in the entire function.
  /// If there is any try in the function, this will have an entry for every BB.
  llvh::DenseMap<BasicBlock *, TryStartInst *> enclosingTrys_{};

  void unimplemented(Instruction &inst) {
    std::string err{"Unimplemented "};
    err += inst.getName();
    F_.getParent()->getContext().getSourceErrorManager().error(
        inst.getLocation(), err);
    // This can optionally be disabled.
    hermes_fatal(err);
  }

  /// Generate a comment containing the string \p str escaped and truncated
  /// to 20 characters.
  llvh::raw_ostream &genStringComment(llvh::StringRef str) {
    os_ << " /*";

    // Escape */ specially, since we are in a comment.
    auto escape = [this](llvh::StringRef str) {
      size_t from = 0;
      size_t pos;
      while ((pos = str.find("*/", from)) != llvh::StringRef::npos) {
        // Write the part of the string including the "*" followed by "\/".
        os_.write_escaped(str.slice(from, pos + 1)) << "\\057";
        from = pos + 2;
      }
      os_.write_escaped(str.substr(from));
    };
    if (str.size() > 20) {
      escape(str.take_front(20));
      os_ << "...";
    } else {
      escape(str);
    }
    return os_ << "*/";
  }

  /// Generate a string constant by referencing the global string table.
  llvh::raw_ostream &genStringConst(LiteralString *LS) {
    auto str = LS->getValue().str();
    os_ << "get_symbols(shUnit)[" << moduleGen_.stringTable.add(str) << ']';
    return genStringComment(str);
  }

  /// Generate a string constant, followed by an optional value (if non-null),
  /// and a cache index. This must be used when
  /// passing parameters to API functions that will use the cache index.
  /// First function is common part, second and third are specializations for
  /// read and write cache entries.
  void genStringConstICCommon(LiteralString *LS, Value *optValue) {
    genStringConst(LS);
    if (optValue) {
      os_ << ", ";
      generateRegisterPtr(*optValue);
    }
    os_ << ", ";
  }
  llvh::raw_ostream &genStringConstWriteIC(
      LiteralString *LS,
      Value *optValue = nullptr) {
    genStringConstICCommon(LS, optValue);
    return genWriteIC(LS);
  }
  llvh::raw_ostream &genStringConstReadIC(
      LiteralString *LS,
      Value *optValue = nullptr) {
    genStringConstICCommon(LS, optValue);
    return genReadIC(LS);
  }

  /// Generate a cache index. This must be used when passing parameters to API
  /// functions that will use the cache index.
  /// \p LS is currently unused. In the future, it might be used in the
  /// optimization controlled by OptimizationSettings::reusePropCache: that
  /// different instructions accessing the same property probably are for
  /// objects of the same hidden class, and should get the same offset.
  llvh::raw_ostream &genWriteIC(LiteralString *LS) {
    return os_ << "get_write_prop_cache(shUnit) + " << nextWriteCacheIdx_++;
  }
  llvh::raw_ostream &genReadIC(LiteralString *LS) {
    return os_ << "get_read_prop_cache(shUnit) + " << nextReadCacheIdx_++;
  }
  llvh::raw_ostream &genPrivateNameIC(Value *privateName) {
    return os_ << "get_private_name_cache(shUnit) + "
               << nextPrivateNameCacheIdx_++;
  }

  /// Helper to generate a value in a register,
  void generateRegister(sh::Register reg) {
    switch (reg.getClass()) {
      case sh::RegClass::LocalPtr:
        os_ << "locals.t" << reg.getIndex();
        break;

      case sh::RegClass::LocalNonPtr:
        os_ << "np" << reg.getIndex();
        break;

      case sh::RegClass::RegStack:
        os_ << "frame[" << (hbc::StackFrameLayout::FirstLocal + reg.getIndex())
            << ']';
        break;

      default:
        hermes_fatal("unimplemented reg class");
    }
  }

  /// Helper to generate a value that must always have an allocated register,
  /// for instance because we need to assign to it or take its address.
  void generateRegister(Value &val) {
    generateRegister(ra_.getRegister(&val));
  }

  /// Helper to generate a pointer to a value that must always have an allocated
  /// register, for instance because we need to assign to it or take its
  /// address.
  /// The pointer can be used to pass to API functions that will use it
  /// as a PinnedHermesValue.
  void generateRegisterPtr(Value &val) {
    os_ << "&";
    generateRegister(val);
  }

  /// Helper to generate an SHValue from a Value.
  void generateValue(Value &val) {
    if (llvh::isa<LiteralUndefined>(&val) || llvh::isa<LiteralUninit>(&val)) {
      os_ << "_sh_ljs_undefined()";
    } else if (llvh::isa<LiteralNull>(&val)) {
      os_ << "_sh_ljs_null()";
    } else if (llvh::isa<LiteralEmpty>(&val)) {
      os_ << "_sh_ljs_empty()";
    } else if (auto B = llvh::dyn_cast<LiteralBool>(&val)) {
      os_ << "_sh_ljs_bool(" << boolStr(B->getValue()) << ")";
    } else if (auto LN = llvh::dyn_cast<LiteralNumber>(&val)) {
      os_ << "_sh_ljs_double(";
      int32_t intval;
      if (!LN->isNegativeZero() &&
          sh_tryfast_f64_to_i32(LN->getValue(), intval)) {
        os_ << intval;
      } else {
        os_ << "((struct HermesValueBase){.raw = "
            << llvh::DoubleToBits(LN->getValue()) << "u}).f64";
      }
      os_ << ")";
    } else if (auto S = llvh::dyn_cast<LiteralString>(&val)) {
      os_ << "_sh_ljs_get_string(shr, ";
      genStringConst(S) << ")";
    } else if (auto *LBI = llvh::dyn_cast<LiteralBigInt>(&val)) {
      auto parsedBigInt = bigint::ParsedBigInt::parsedBigIntFromNumericValue(
          LBI->getValue()->str());
      llvh::ArrayRef<uint8_t> bytes = parsedBigInt->getBytes();
      assert(parsedBigInt && "should be valid");
      os_ << "_sh_ljs_create_bigint(shr, ";
      // Print a string literal using hex escapes for each byte.
      // Cast it to `uint8_t*` for use with the API signature.
      os_ << "(const uint8_t *)\"";
      for (const uint8_t c : bytes) {
        os_ << llvh::format("\\x%x", c);
      }
      os_ << "\", ";
      os_ << bytes.size();
      os_ << ")";
    } else if (llvh::isa<Instruction>(&val)) {
      generateRegister(val);
    } else if (auto *NE = llvh::dyn_cast<LiteralNativeExtern>(&val)) {
      os_ << "_sh_ljs_native_pointer(" << NE->getData()->name()->str() << ")";
    } else {
      hermes_fatal("Unknown value");
    }
  }

  void generateInstruction(Instruction &inst) {
    hermes_fatal("Unimplemented instruction Instruction");
  }
  void generateCreatePrivateNameInst(CreatePrivateNameInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_ << "_sh_ljs_create_private_name(shr, ";
    genStringConst(cast<LiteralString>(inst.getSingleOperand())) << ");\n";
  }
  void generateAddEmptyStringInst(AddEmptyStringInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_ << "_sh_ljs_add_empty_string_rjs(shr, ";
    generateRegisterPtr(*inst.getSingleOperand());
    os_ << ");\n";
  }
  void generateToPropertyKeyInst(ToPropertyKeyInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_ << "_sh_ljs_to_property_key(shr, ";
    generateRegisterPtr(*inst.getSingleOperand());
    os_ << ");\n";
  }
  void generateAsNumberInst(AsNumberInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_ << "_sh_ljs_double(_sh_ljs_to_double_rjs(shr, ";
    generateRegisterPtr(*inst.getSingleOperand());
    os_ << "));\n";
  }
  void generateAsNumericInst(AsNumericInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_ << "_sh_ljs_to_numeric_rjs(shr, ";
    generateRegisterPtr(*inst.getSingleOperand());
    os_ << ");\n";
  }
  void generateAsInt32Inst(AsInt32Inst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    if (inst.getSingleOperand()->getType().isNumberType()) {
      os_ << "_sh_ljs_double((double)_sh_to_int32_double(_sh_ljs_get_double(";
      generateRegister(*inst.getSingleOperand());
      os_ << ")));\n";
    } else {
      os_
          << (options_.smallC
                  ? "_sh_ljs_double(_sh_ljs_to_int32_rjs(shr, "
                  : "_sh_ljs_double(_sh_ljs_to_int32_rjs_inline(shr, ");
      generateRegisterPtr(*inst.getSingleOperand());
      os_ << "));\n";
    }
  }
  void generateAsUint32Inst(AsUint32Inst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    if (inst.getSingleOperand()->getType().isNumberType()) {
      os_ << "_sh_ljs_double((double)_sh_to_uint32_double(_sh_ljs_get_double(";
      generateRegister(*inst.getSingleOperand());
      os_ << ")));\n";
    } else {
      os_
          << (options_.smallC
                  ? "_sh_ljs_double(_sh_ljs_to_uint32_rjs(shr, "
                  : "_sh_ljs_double(_sh_ljs_to_uint32_rjs_inline(shr, ");
      generateRegisterPtr(*inst.getSingleOperand());
      os_ << "));\n";
    }
  }
  void generateLoadStackInst(LoadStackInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    generateValue(*inst.getSingleOperand());
    os_ << ";\n";
  }
  void generateMovInst(MovInst &inst) {
    sh::Register dstReg = ra_.getRegister(&inst);
    os_.indent(2);
    generateRegister(dstReg);
    os_ << " = ";
    generateValue(*inst.getSingleOperand());
    os_ << ";\n";
  }
  void generateImplicitMovInst(ImplicitMovInst &inst) {
    // ImplicitMovs produce no bytecode, they only express that a subsequent
    // instruction will perform the equivalent of a 'Mov'.
    os_ << "  // ImplicitMovInst\n";
  }
  void generateTypeOfInst(TypeOfInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_typeof(shr, ";
    generateRegisterPtr(*inst.getArgument());
    os_ << ");\n";
  }
  void generateTypeOfIsInst(TypeOfIsInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    // Every value has exactly one type, so test whichever of the set and its
    // complement has fewer types: the inline check grows with each type.
    TypeOfIsTypes types = inst.getTypes()->getData();
    bool negate = types.count() > types.invert().count();
    if (negate)
      types = types.invert();
    os_ << " = _sh_ljs_bool(" << (negate ? "!" : "")
        << "_sh_ljs_typeof_is_inline(";
    generateValue(*inst.getArgument());
    os_ << ", " << types.getRaw() << "));\n";
  }
  void generateUnaryOperatorInst(UnaryOperatorInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    bool isDouble = inst.getSingleOperand()->getType().isNumberType();
    switch (inst.getKind()) {
      case (ValueKind::UnaryIncInstKind):
        if (isDouble) {
          os_ << "_sh_ljs_double(_sh_ljs_get_double(";
          generateRegister(*inst.getSingleOperand());
          os_ << ") + 1);\n";
        } else {
          os_ << (options_.smallC ? "_sh_ljs_inc_rjs"
                                  : "_sh_ljs_inc_rjs_inline")
              << "(shr, &";
          generateRegister(*inst.getSingleOperand());
          os_ << ");\n";
        }
        break;
      case (ValueKind::UnaryDecInstKind):
        if (isDouble) {
          os_ << "_sh_ljs_double(_sh_ljs_get_double(";
          generateRegister(*inst.getSingleOperand());
          os_ << ") - 1);\n";
        } else {
          os_ << (options_.smallC ? "_sh_ljs_dec_rjs"
                                  : "_sh_ljs_dec_rjs_inline")
              << "(shr, &";
          generateRegister(*inst.getSingleOperand());
          os_ << ");\n";
        }
        break;
      case (ValueKind::UnaryTildeInstKind):
        os_ << (options_.smallC ? "_sh_ljs_bit_not_rjs"
                                : "_sh_ljs_bit_not_rjs_inline")
            << "(shr, &";
        generateRegister(*inst.getSingleOperand());
        os_ << ");\n";
        break;
      case ValueKind::UnaryMinusInstKind: { // -
        os_ << "_sh_ljs_minus_rjs(shr, &";
        generateRegister(*inst.getSingleOperand());
        os_ << ");\n";
        break;
      }
      case ValueKind::UnaryBangInstKind: { // !
        os_ << "_sh_ljs_bool(!_sh_ljs_to_boolean(";
        generateRegister(*inst.getSingleOperand());
        os_ << "));\n";
        break;
      }
      case ValueKind::UnaryVoidInstKind: { // Void operator.
        os_ << "_sh_ljs_undefined();\n";
        break;
      }
      default:
        unimplemented(inst);
        break;
    }
  }
  void generateDirectEvalInst(DirectEvalInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_direct_eval(shr, ";
    generateRegisterPtr(*inst.getEvalText());
    os_ << ", ";
    os_ << boolStr(inst.getStrictCaller());
    os_ << ");\n";
  }
  void generateDeclareGlobalVarInst(DeclareGlobalVarInst &inst) {
    os_ << "  _sh_ljs_declare_global_var(shr, ";
    genStringConst(inst.getName()) << ");\n";
  }
  void generateLoadFrameInst(LoadFrameInst &inst) {
    os_.indent(2);
    generateValue(inst);
    os_ << " = _sh_ljs_load_from_env(";
    generateValue(*inst.getScope());
    os_ << ", " << inst.getLoadVariable()->getIndexInVariableList() << ");\n";
  }
  void generateLIRLoadConstInst(LIRLoadConstInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    generateValue(*inst.getConst());
    os_ << ";\n";
  }
  void generateLoadParamInst(LoadParamInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    uint32_t index = inst.getParam()->getIndexInParamList();
    // Special case "this": it is always available, so we don't need a call.
    if (index == 0) {
      os_ << " = frame[" << hbc::StackFrameLayout::ThisArg << "];\n";
    } else {
      os_ << " = _sh_ljs_param(frame, " << index << ");\n";
    }
  }
  void generateGetParentScopeInst(GetParentScopeInst &inst) {
    os_.indent(2);
    generateRegister(inst);

    os_ << " = _sh_ljs_get_env_from_closure(shr, frame["
        << hbc::StackFrameLayout::CalleeClosureOrCB << "]);";
  }
  void generateResolveScopeInst(ResolveScopeInst &inst) {
    hermes_fatal("ResolveScopeInst should have been lowered.");
  }
  void generateLIRResolveScopeInst(LIRResolveScopeInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_get_env(shr, ";
    generateRegister(*inst.getStartScope());
    os_ << ", " << inst.getNumLevels()->asUInt32() << ");\n";
  }
  void generateHBCResolveParentEnvironmentInst(
      HBCResolveParentEnvironmentInst &inst) {
    hermes_fatal("HBCResolveEnvironment is not used by SH.");
  }
  void generateGetClosureScopeInst(GetClosureScopeInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_get_env_from_closure(shr, ";
    generateRegister(*inst.getClosure());
    os_ << ");";
  }
  void generateLIRGetArgumentsLengthInst(LIRGetArgumentsLengthInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_get_arguments_length(shr, frame, ";
    generateRegisterPtr(*inst.getLazyRegister());
    os_ << ");\n";
  }
  void generateLIRReifyArgumentsLooseInst(LIRReifyArgumentsLooseInst &inst) {
    os_ << "  _sh_ljs_reify_arguments_loose(shr, frame, ";
    generateRegisterPtr(*inst.getLazyRegister());
    os_ << ");\n";
  }
  void generateLIRReifyArgumentsStrictInst(LIRReifyArgumentsStrictInst &inst) {
    os_ << "  _sh_ljs_reify_arguments_strict(shr, frame, ";
    generateRegisterPtr(*inst.getLazyRegister());
    os_ << ");\n";
  }
  void generateLIRSpillMovInst(LIRSpillMovInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    generateValue(*inst.getSingleOperand());
    os_ << ";\n";
  }
  void generatePhiInst(PhiInst &inst) {
    // PhiInst has been translated into a sequence of MOVs in RegAlloc
    // Nothing to do here.
    os_ << "  // PhiInst\n";
  }
  void generateBinaryOperatorInst(BinaryOperatorInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";

    // If the final result is a bool instead of a double.
    bool boolConv = false;

    // Whether to pass the arguments to a function by value.
    bool passByValue = false;

    // Function to use if we can't specialize the types.
    const char *funcUntypedOp = nullptr;

    // Infix operator for doubles.
    const char *infixDoubleOp = nullptr;
    // Infix operator for raw bitwise comparison.
    const char *infixRawOp = nullptr;
    // Function call for operator for doubles.
    const char *funcDoubleOp = nullptr;
    // Function call for operator for int32.
    const char *funcInt32Op = nullptr;

    bool bothDouble = inst.getLeftHandSide()->getType().isNumberType() &&
        inst.getRightHandSide()->getType().isNumberType();

    // NOTE: this used to check whether we know that both operands are numbers
    // in the int32 range. We no longer track that information, so for now this
    // is hardcoded to false.
    const bool bothInt32 = false;

    switch (inst.getKind()) {
      case ValueKind::BinaryAddInstKind: // +   (+=)
        if (bothDouble) {
          infixDoubleOp = "+";
        } else {
          funcUntypedOp =
              options_.smallC ? "_sh_ljs_add_rjs" : "_sh_ljs_add_rjs_inline";
        }
        break;
      case ValueKind::BinarySubtractInstKind: // -   (-=)
        if (bothDouble) {
          infixDoubleOp = "-";
        } else {
          funcUntypedOp =
              options_.smallC ? "_sh_ljs_sub_rjs" : "_sh_ljs_sub_rjs_inline";
        }
        break;
      case ValueKind::BinaryMultiplyInstKind: // *   (*=)
        if (bothDouble) {
          infixDoubleOp = "*";
        } else {
          funcUntypedOp =
              options_.smallC ? "_sh_ljs_mul_rjs" : "_sh_ljs_mul_rjs_inline";
        }
        break;
      case ValueKind::BinaryDivideInstKind: // /   (/=)
        if (bothDouble)
          infixDoubleOp = "/";
        else
          funcUntypedOp =
              options_.smallC ? "_sh_ljs_div_rjs" : "_sh_ljs_div_rjs_inline";
        break;
      case ValueKind::BinaryModuloInstKind: // %   (%=)
        if (bothInt32)
          funcInt32Op = "_sh_mod_int32";
        else if (bothDouble)
          funcDoubleOp = "_sh_mod_double";
        else
          funcUntypedOp =
              options_.smallC ? "_sh_ljs_mod_rjs" : "_sh_ljs_mod_rjs_inline";
        break;
      case ValueKind::BinaryOrInstKind: // |   (|=)
        funcUntypedOp = options_.smallC ? "_sh_ljs_bit_or_rjs"
                                        : "_sh_ljs_bit_or_rjs_inline";
        break;
      case ValueKind::BinaryAndInstKind: // &   (&=)
        funcUntypedOp = options_.smallC ? "_sh_ljs_bit_and_rjs"
                                        : "_sh_ljs_bit_and_rjs_inline";
        break;
      case ValueKind::BinaryXorInstKind: // ^   (^=)
        funcUntypedOp = options_.smallC ? "_sh_ljs_bit_xor_rjs"
                                        : "_sh_ljs_bit_xor_rjs_inline";
        break;
      case ValueKind::BinaryRightShiftInstKind: // >>  (>>=)
        funcUntypedOp = options_.smallC ? "_sh_ljs_right_shift_rjs"
                                        : "_sh_ljs_right_shift_rjs_inline";
        break;
      case ValueKind::BinaryUnsignedRightShiftInstKind: // >>> (>>>=)
        funcUntypedOp = options_.smallC
            ? "_sh_ljs_unsigned_right_shift_rjs"
            : "_sh_ljs_unsigned_right_shift_rjs_inline";
        break;
      case ValueKind::BinaryLeftShiftInstKind: // <<  (<<=)
        funcUntypedOp = options_.smallC ? "_sh_ljs_left_shift_rjs"
                                        : "_sh_ljs_left_shift_rjs_inline";
        break;
      case ValueKind::BinaryNotEqualInstKind: // !=
        funcUntypedOp = options_.smallC ? "!_sh_ljs_equal_rjs"
                                        : "!_sh_ljs_equal_rjs_inline";
        boolConv = true;
        break;
      case ValueKind::BinaryEqualInstKind: // ==
        if (bothDouble) {
          infixDoubleOp = "==";
        } else {
          funcUntypedOp = options_.smallC ? "_sh_ljs_equal_rjs"
                                          : "_sh_ljs_equal_rjs_inline";
        }
        boolConv = true;
        break;
      case ValueKind::BinaryStrictlyNotEqualInstKind: // !==
        if (bothDouble) {
          infixDoubleOp = "!=";
        } else if (canCompareStrictEqualityRaw(
                       typeCtx_,
                       inst.getLeftHandSide(),
                       inst.getRightHandSide())) {
          infixRawOp = "!=";
        } else {
          funcUntypedOp = options_.smallC ? "!_sh_ljs_strict_equal"
                                          : "!_sh_ljs_strict_equal_inline";
          passByValue = true;
        }
        boolConv = true;
        break;
      case ValueKind::BinaryStrictlyEqualInstKind: // ===
        if (bothDouble) {
          infixDoubleOp = "==";
        } else if (canCompareStrictEqualityRaw(
                       typeCtx_,
                       inst.getLeftHandSide(),
                       inst.getRightHandSide())) {
          infixRawOp = "==";
        } else {
          funcUntypedOp = options_.smallC ? "_sh_ljs_strict_equal"
                                          : "_sh_ljs_strict_equal_inline";
          passByValue = true;
        }
        boolConv = true;
        break;
      case ValueKind::BinaryInInstKind: // in
        funcUntypedOp = "_sh_ljs_is_in_rjs";
        break;
      case ValueKind::BinaryPrivateInInstKind: // in
        os_ << "_sh_ljs_private_is_in_rjs(shr, ";
        generateRegisterPtr(*inst.getLeftHandSide());
        os_ << ", ";
        generateRegisterPtr(*inst.getRightHandSide());
        os_ << ", ";
        genPrivateNameIC(inst.getLeftHandSide());
        os_ << ");\n";
        return;
      case ValueKind::BinaryInstanceOfInstKind:
        funcUntypedOp = "_sh_ljs_instance_of_rjs";
        break;
      case ValueKind::BinaryLessThanInstKind:
        if (bothDouble) {
          infixDoubleOp = "<";
        } else {
          funcUntypedOp =
              options_.smallC ? "_sh_ljs_less_rjs" : "_sh_ljs_less_rjs_inline";
        }
        boolConv = true;
        break;
      case ValueKind::BinaryLessThanOrEqualInstKind:
        if (bothDouble) {
          infixDoubleOp = "<=";
        } else {
          funcUntypedOp = options_.smallC ? "_sh_ljs_less_equal_rjs"
                                          : "_sh_ljs_less_equal_rjs_inline";
        }
        boolConv = true;
        break;
      case ValueKind::BinaryGreaterThanInstKind:
        if (bothDouble) {
          infixDoubleOp = ">";
        } else {
          funcUntypedOp = options_.smallC ? "_sh_ljs_greater_rjs"
                                          : "_sh_ljs_greater_rjs_inline";
        }
        boolConv = true;
        break;
      case ValueKind::BinaryGreaterThanOrEqualInstKind:
        if (bothDouble) {
          infixDoubleOp = ">=";
        } else {
          funcUntypedOp = options_.smallC ? "_sh_ljs_greater_equal_rjs"
                                          : "_sh_ljs_greater_equal_rjs_inline";
        }
        boolConv = true;
        break;
      case ValueKind::BinaryExponentiationInstKind:
      default:
        unimplemented(inst);
        return;
    }
    if (boolConv)
      os_ << "_sh_ljs_bool(";
    else if (infixDoubleOp || funcDoubleOp || funcInt32Op)
      os_ << "_sh_ljs_double(";
    if (passByValue) {
      assert(funcUntypedOp);
      os_ << funcUntypedOp << "(";
      generateRegister(*inst.getLeftHandSide());
      os_ << ", ";
      generateRegister(*inst.getRightHandSide());
      os_ << ")";
    } else if (infixDoubleOp) {
      assert(bothDouble);
      os_ << "_sh_ljs_get_double(";
      generateRegister(*inst.getLeftHandSide());
      os_ << ") " << infixDoubleOp << " _sh_ljs_get_double(";
      generateRegister(*inst.getRightHandSide());
      os_ << ")";
    } else if (funcInt32Op) {
      assert(bothInt32);
      os_ << funcInt32Op << "(";
      os_ << "_sh_ljs_get_double(";
      generateRegister(*inst.getLeftHandSide());
      os_ << "), _sh_ljs_get_double(";
      generateRegister(*inst.getRightHandSide());
      os_ << "))";
    } else if (funcDoubleOp) {
      assert(bothDouble);
      os_ << funcDoubleOp << "(";
      os_ << "_sh_ljs_get_double(";
      generateRegister(*inst.getLeftHandSide());
      os_ << "), _sh_ljs_get_double(";
      generateRegister(*inst.getRightHandSide());
      os_ << "))";
    } else if (infixRawOp) {
      os_ << "";
      generateRegister(*inst.getLeftHandSide());
      os_ << ".raw " << infixRawOp << " ";
      generateRegister(*inst.getRightHandSide());
      os_ << ".raw";
    } else {
      assert(funcUntypedOp);
      os_ << funcUntypedOp << "(shr, &";
      generateRegister(*inst.getLeftHandSide());
      os_ << ", &";
      generateRegister(*inst.getRightHandSide());
      os_ << ")";
    }
    if (boolConv)
      os_ << ")";
    else if (infixDoubleOp || funcDoubleOp || funcInt32Op)
      os_ << ")";
    os_ << ";\n";
  }

  void generateImulInst(ImulInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_
        << (options_.smallC ? "_sh_ljs_imul_rjs(shr, "
                            : "_sh_ljs_imul_rjs_inline(shr, ");
    generateRegisterPtr(*inst.getLeft());
    os_ << ", ";
    generateRegisterPtr(*inst.getRight());
    os_ << ");\n";
  }

  void generateStorePropertyWithReceiverInst(
      StorePropertyWithReceiverInst &inst) {
    os_ << "_sh_ljs_put_by_val_with_receiver_rjs(shr,&";
    generateRegister(*inst.getObject());
    os_ << ", &";
    generateRegister(*inst.getProperty());
    os_ << ", &";
    generateRegister(*inst.getStoredValue());
    os_ << ", &";
    generateRegister(*inst.getReceiver());
    os_ << ", " << inst.getIsStrict();
    os_ << ");\n";
  }
  void generateStorePropertyInstImpl(StorePropertyInst &inst, bool strictMode) {
    os_.indent(2);
    if (auto *LS = llvh::dyn_cast<LiteralString>(inst.getProperty())) {
      if (strictMode)
        os_ << "_sh_ljs_put_by_id_strict_rjs";
      else
        os_ << "_sh_ljs_put_by_id_loose_rjs";
      os_ << "(shr, shUnit, &";
      generateRegister(*inst.getObject());
      os_ << ", ";
      genStringConstWriteIC(LS, inst.getStoredValue()) << ");\n";
      return;
    }
    // If the prop is an index-like constant, use put_by_index directly.
    if (auto *litNum = llvh::dyn_cast<LiteralNumber>(inst.getProperty())) {
      if (auto idxOpt = doubleToArrayIndex(litNum->getValue())) {
        if (strictMode)
          os_ << "_sh_ljs_put_by_index_strict_rjs";
        else
          os_ << "_sh_ljs_put_by_index_loose_rjs";
        os_ << "(shr,&";
        generateRegister(*inst.getObject());
        os_ << ", " << *idxOpt << "u, &";
        generateRegister(*inst.getStoredValue());
        os_ << ");\n";
        return;
      }
    }
    // If the key is known to be a number, use the optimized numeric path
    // which tries sh_tryfast_f64_to_u32 inline before falling back.
    else if (inst.getProperty()->getType().isNumberType()) {
      if (strictMode)
        os_ << "_sh_ljs_put_by_val_numeric_strict_rjs";
      else
        os_ << "_sh_ljs_put_by_val_numeric_loose_rjs";
      os_ << "(shr,&";
      generateRegister(*inst.getObject());
      os_ << ", ";
      generateRegister(*inst.getProperty());
      os_ << ", &";
      generateRegister(*inst.getStoredValue());
      os_ << ");\n";
      return;
    }

    if (strictMode)
      os_ << "_sh_ljs_put_by_val_strict_rjs";
    else
      os_ << "_sh_ljs_put_by_val_loose_rjs";
    os_ << "(shr,&";
    generateRegister(*inst.getObject());
    os_ << ", &";
    generateRegister(*inst.getProperty());
    os_ << ", &";
    generateRegister(*inst.getStoredValue());
    os_ << ");\n";
  }
  void generateStorePropertyLooseInst(StorePropertyLooseInst &inst) {
    generateStorePropertyInstImpl(inst, false);
  }
  void generateStorePropertyStrictInst(StorePropertyStrictInst &inst) {
    generateStorePropertyInstImpl(inst, true);
  }

  void generateTryStoreGlobalPropertyInstImpl(
      TryStoreGlobalPropertyInst &inst,
      bool strictMode) {
    os_.indent(2);
    if (strictMode)
      os_ << "_sh_ljs_try_put_by_id_strict_rjs(";
    else
      os_ << "_sh_ljs_try_put_by_id_loose_rjs(";

    os_ << "shr, shUnit, ";
    generateRegisterPtr(*inst.getObject());
    os_ << ", ";
    auto prop = inst.getProperty();
    auto *propStr = cast<LiteralString>(prop);
    genStringConstWriteIC(propStr, inst.getStoredValue()) << ");\n";
  }
  void generateTryStoreGlobalPropertyLooseInst(
      TryStoreGlobalPropertyLooseInst &inst) {
    generateTryStoreGlobalPropertyInstImpl(inst, false);
  }
  void generateTryStoreGlobalPropertyStrictInst(
      TryStoreGlobalPropertyStrictInst &inst) {
    generateTryStoreGlobalPropertyInstImpl(inst, true);
  }

  void generateDefineOwnPropertyInst(DefineOwnPropertyInst &inst) {
    os_.indent(2);
    auto prop = inst.getProperty();
    bool isEnumerable = inst.getIsEnumerable();

    // If the property is a LiteralNumber, the property is enumerable, and it is
    // a valid array index, it is coming from an array initialization and we
    // will emit it as DefineOwnByIndex.
    if (auto *numProp = llvh::dyn_cast<LiteralNumber>(prop)) {
      assert(
          inst.getIsEnumerable() &&
          "Non-enumerable properties with literal keys should be handled by LoadConstants");
      auto arrayIndex = numProp->convertToArrayIndex();
      uint32_t index = arrayIndex.getValue();
      os_ << "_sh_ljs_define_own_by_index(";
      os_ << "shr, ";
      generateRegisterPtr(*inst.getObject());
      os_ << ", ";
      os_ << index << ", ";
      generateRegisterPtr(*inst.getStoredValue());
      os_ << ");\n";
      return;
    }

    if (auto *LS = llvh::dyn_cast<LiteralString>(prop)) {
      assert(
          inst.getIsEnumerable() &&
          "Non-enumerable properties with literal keys should be handled by LoadConstants");
      os_ << "_sh_ljs_define_own_by_id(shr,&";
      generateRegister(*inst.getObject());
      os_ << ", ";
      genStringConstWriteIC(LS, inst.getStoredValue()) << ");\n";
      return;
    }

    if (isEnumerable)
      os_ << "_sh_ljs_define_own_by_val(";
    else
      os_ << "_sh_ljs_define_own_ne_by_val(";

    os_ << "shr, ";
    generateRegisterPtr(*inst.getObject());
    os_ << ", ";
    generateRegisterPtr(*prop);
    os_ << ", ";
    generateRegisterPtr(*inst.getStoredValue());
    os_ << ");\n";
  }
  void generateDefineOwnInDenseArrayInst(DefineOwnInDenseArrayInst &inst) {
    os_.indent(2);
    os_ << "_sh_ljs_define_own_in_dense_array(shr, ";
    generateRegisterPtr(*inst.getArray());
    os_ << ", ";
    generateRegisterPtr(*inst.getStoredValue());
    os_ << ", ";
    auto *arrayIndex = inst.getArrayIndex();
    uint32_t index = arrayIndex->asUInt32();
    os_ << index << ");\n";
  }
  void generateStoreOwnPrivateFieldInst(StoreOwnPrivateFieldInst &inst) {
    os_.indent(2);
    os_ << "_sh_ljs_put_own_private_by_sym(shr, ";
    generateRegisterPtr(*inst.getObject());
    os_ << ", ";
    generateRegisterPtr(*inst.getProperty());
    os_ << ", ";
    generateRegisterPtr(*inst.getStoredValue());
    os_ << ", ";
    genPrivateNameIC(inst.getProperty());
    os_ << ");\n";
  }
  void generateAddOwnPrivateFieldInst(AddOwnPrivateFieldInst &inst) {
    os_.indent(2);
    os_ << "_sh_ljs_add_own_private_by_sym(shr, ";
    generateRegisterPtr(*inst.getObject());
    os_ << ", ";
    generateRegisterPtr(*inst.getProperty());
    os_ << ", ";
    generateRegisterPtr(*inst.getStoredValue());
    os_ << ");\n";
  }
  void generatePrivateBrandCheckInst(PrivateBrandCheckInst &inst) {
    hermes_fatal("PrivateBrandCheckInst should have been lowered");
  }
  void generateDefineOwnGetterSetterInst(DefineOwnGetterSetterInst &inst) {
    os_.indent(2);
    os_ << "_sh_ljs_define_own_getter_setter_by_val(";
    os_ << "shr, ";
    generateRegisterPtr(*inst.getObject());
    os_ << ", ";
    generateRegisterPtr(*inst.getProperty());
    os_ << ", ";
    generateRegisterPtr(*inst.getStoredGetter());
    os_ << ", ";
    generateRegisterPtr(*inst.getStoredSetter());
    os_ << ", " << inst.getIsEnumerable();
    os_ << ");\n";
  }
  void generateDeletePropertyInstImpl(
      DeletePropertyInst &inst,
      bool strictMode) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    auto prop = inst.getProperty();

    if (strictMode)
      os_ << "_sh_ljs_del_by_val_strict(";
    else
      os_ << "_sh_ljs_del_by_val_loose(";

    os_ << "shr, ";
    generateRegisterPtr(*inst.getObject());
    os_ << ", ";
    generateRegisterPtr(*prop);
    os_ << ");\n";
  }
  void generateDeletePropertyLooseInst(DeletePropertyLooseInst &inst) {
    generateDeletePropertyInstImpl(inst, false);
  }
  void generateDeletePropertyStrictInst(DeletePropertyStrictInst &inst) {
    generateDeletePropertyInstImpl(inst, true);
  }
  void generateLoadPropertyInst(LoadPropertyInst &inst) {
    os_.indent(2);
    generateValue(inst);
    os_ << " = ";
    if (auto *LS = llvh::dyn_cast<LiteralString>(inst.getProperty())) {
      os_ << (options_.smallC ? "_sh_ljs_get_by_id_rjs"
                              : "_sh_ljs_get_by_id_rjs_inline")
          << "(shr,&";
      generateRegister(*inst.getObject());
      os_ << ",";
      genStringConstReadIC(LS) << ");\n";
      return;
    }
    // If the prop is an index-like constant, generate the special bytecode.
    if (auto *litNum = llvh::dyn_cast<LiteralNumber>(inst.getProperty())) {
      if (auto idxOpt = doubleToArrayIndex(litNum->getValue())) {
        os_ << "_sh_ljs_get_by_index_rjs(shr,&";
        generateRegister(*inst.getObject());
        os_ << ", ";
        os_ << *idxOpt << ");\n";
        return;
      }
    }
    os_ << "_sh_ljs_get_by_val_rjs(shr,&";
    generateRegister(*inst.getObject());
    os_ << ", &";
    generateRegister(*inst.getProperty());
    os_ << ");\n";
  }
  void generateLoadPropertyWithReceiverInst(
      LoadPropertyWithReceiverInst &inst) {
    os_.indent(2);
    generateValue(inst);
    os_ << " = ";
    auto prop = inst.getProperty();
    if (auto *LS = llvh::dyn_cast<LiteralString>(prop)) {
      os_ << "_sh_ljs_get_by_id_with_receiver_rjs(shr, &";
      generateRegister(*inst.getObject());
      os_ << ", &";
      generateRegister(*inst.getReceiver());
      os_ << ", ";
      genStringConstReadIC(LS) << ");\n";
      return;
    }
    os_ << "_sh_ljs_get_by_val_with_receiver_rjs(shr, &";
    generateRegister(*inst.getObject());
    os_ << ", &";
    generateRegister(*inst.getProperty());
    os_ << ", &";
    generateRegister(*inst.getReceiver());
    os_ << ");\n";
  }
  void generateLoadOwnPrivateFieldInst(LoadOwnPrivateFieldInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_ << "_sh_ljs_get_own_private_by_sym(shr, ";
    generateRegisterPtr(*inst.getObject());
    os_ << ", ";
    generateRegisterPtr(*inst.getProperty());
    os_ << ", ";
    genPrivateNameIC(inst.getProperty());
    os_ << ");\n";
  }
  void generateTryLoadGlobalPropertyInst(TryLoadGlobalPropertyInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    LiteralString *LS = inst.getProperty();
    os_ << "_sh_ljs_try_get_by_id_rjs(shr,&";
    generateRegister(*inst.getObject());
    os_ << ", ";
    genStringConstReadIC(LS) << ");\n";
  }
  void generateLoadParentNoTrapsInst(LoadParentNoTrapsInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_load_parent_no_traps(shr, ";
    generateRegister(*inst.getObject());
    os_ << ");\n";
  }
  void generateTypedLoadParentInst(TypedLoadParentInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_ << "_sh_typed_load_parent(shr, ";
    generateRegisterPtr(*inst.getObject());
    os_ << ");\n";
  }
  void generateFUnaryMathInst(FUnaryMathInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_double(";
    switch (inst.getKind()) {
      case ValueKind::FNegateKind:
        os_ << "-_sh_ljs_get_double(";
        generateValue(*inst.getArg());
        os_ << ")";
        break;
      default:
        llvm_unreachable("invalid FUnaryMath");
    }
    os_ << ");\n";
  }
  void generateFBinaryMathInst(FBinaryMathInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";

    // Handle FModuloInst separately because it needs a function call
    // and doesn't fit neatly into a C binary operator.
    if (inst.getKind() == ValueKind::FModuloInstKind) {
      os_ << "_sh_ljs_double(_sh_mod_double(_sh_ljs_get_double(";
      generateValue(*inst.getLeft());
      os_ << "), _sh_ljs_get_double(";
      generateValue(*inst.getRight());
      os_ << ")));\n";
      return;
    }

    os_ << "_sh_ljs_double(_sh_ljs_get_double(";
    generateValue(*inst.getLeft());
    os_ << ") ";
    switch (inst.getKind()) {
      case ValueKind::FAddInstKind:
        os_ << "+";
        break;
      case ValueKind::FSubtractInstKind:
        os_ << "-";
        break;
      case ValueKind::FMultiplyInstKind:
        os_ << "*";
        break;
      case ValueKind::FDivideInstKind:
        os_ << "/";
        break;
      default:
        llvm_unreachable("invalid FBinaryMath");
    }
    os_ << " _sh_ljs_get_double(";
    generateValue(*inst.getRight());
    os_ << "));\n";
  }
  void generateFCompareInst(FCompareInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";

    os_ << "_sh_ljs_bool(_sh_ljs_get_double(";
    generateValue(*inst.getLeft());
    os_ << ") ";
    switch (inst.getKind()) {
      case ValueKind::FEqualInstKind:
        os_ << "==";
        break;
      case ValueKind::FNotEqualInstKind:
        os_ << "!=";
        break;
      case ValueKind::FLessThanInstKind:
        os_ << "<";
        break;
      case ValueKind::FLessThanOrEqualInstKind:
        os_ << "<=";
        break;
      case ValueKind::FGreaterThanInstKind:
        os_ << ">";
        break;
      case ValueKind::FGreaterThanOrEqualInstKind:
        os_ << ">=";
        break;
      default:
        llvm_unreachable("invalid FBinaryMath");
    }
    os_ << " _sh_ljs_get_double(";
    generateValue(*inst.getRight());
    os_ << "));\n";
  }
  void generateHBCFCompareBranchInst(HBCFCompareBranchInst &inst) {
    hermes_fatal("HBCFCompareBranchInst not generated in native backend");
  }
  void generateStringConcatInst(StringConcatInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_ << "_sh_string_concat(shr, " << inst.getNumOperands() << ", ";
    for (int i = 0, e = inst.getNumOperands(); i < e; ++i) {
      if (i != 0)
        os_ << ", ";
      generateRegisterPtr(*inst.getOperand(i));
    }
    os_ << ");\n";
  }
  void generateHBCStringConcatInst(HBCStringConcatInst &inst) {
    hermes_fatal("HBCStringConcatInst not generated in native backend");
  }
  void generateStoreStackInst(StoreStackInst &inst) {
    hermes_fatal("StoreStackInst should have been lowered.");
  }
  void generateStoreFrameInst(StoreFrameInst &inst) {
    os_ << "  _sh_ljs_store_to_env(shr, ";
    generateValue(*inst.getScope());
    os_ << ",";
    generateValue(*inst.getValue());
    os_ << ", " << inst.getVariable()->getIndexInVariableList() << ");\n";
  }
  void generateAllocStackInst(AllocStackInst &inst) {
    // This is a no-op.
    os_ << "  // AllocStackInst\n";
  }
  void generateAllocArrayInst(AllocArrayInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    auto elementCount = inst.getElementCount();
    uint32_t sizeHint = inst.getSizeHint()->asUInt32();

    if (elementCount == 0) {
      os_ << "_sh_ljs_new_array(shr, " << sizeHint << ")";
    } else {
      auto bufIndex =
          moduleGen_.literalBuffers.serializedLiteralOffsetFor(&inst);

      os_ << "_sh_ljs_new_array_with_buffer(shr, shUnit, ";
      os_ << sizeHint << ", ";
      os_ << elementCount << ", ";
      os_ << bufIndex.valueBufferOffset << ")";
    }
    os_ << ";\n";
  }
  void generateAllocFastArrayInst(AllocFastArrayInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_new_fastarray_with_proto(shr, &";
    generateRegister(*inst.getPrototype());
    os_ << ", " << inst.getCapacity()->asUInt32() << ");\n";
  }
  void generateGetTemplateObjectInst(GetTemplateObjectInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    // If not dup, argCount also includes cooked strings.
    uint32_t numStrings = inst.getNumStrings();
    uint32_t argCount = inst.isDup() ? numStrings : numStrings * 2;
    // Can't lower to calling the HermesBuiltin because that depends on
    // RuntimeModule, so we have a _sh_get_template_object function instead,
    // which can read from the SHUnit templateMap.
    os_ << "_sh_get_template_object(shr, shUnit, " << inst.getTemplateObjID()
        << ", " << boolStr(inst.isDup()) << ", " << argCount;
    for (unsigned i = 0; i < numStrings; ++i) {
      os_ << ", ";
      generateRegisterPtr(*inst.getRawString(i));
    }
    if (!inst.isDup()) {
      for (unsigned i = 0; i < numStrings; ++i) {
        os_ << ", ";
        generateRegisterPtr(*inst.getCookedString(i));
      }
    }
    os_ << ");\n";
  }
  void generateAllocObjectLiteralInst(AllocObjectLiteralInst &inst) {
    assert(
        inst.getKeyValuePairCount() == 0 &&
        "AllocObjectLiteralInst with properties should be lowered to LIRAllocObjectFromBufferInst");
    assert(llvh::isa<EmptySentinel>(inst.getParentObject()));
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    // TODO: Utilize sizeHint.
    os_ << "_sh_ljs_new_object(shr)";
    os_ << ";\n";
  }
  void generateAllocTypedObjectInst(AllocTypedObjectInst &inst) {
    assert(
        inst.getKeyValuePairCount() == 0 &&
        "AllocTypedObjectInst with properties should be lowered to LIRAllocObjectFromBufferInst");
    assert(llvh::isa<EmptySentinel>(inst.getParentObject()));
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    // TODO: Utilize sizeHint.
    os_ << "_sh_ljs_new_object(shr)";
    os_ << ";\n";
  }
  void generateAllocTypedNonEnumObjectInst(AllocTypedNonEnumObjectInst &inst) {
    assert(
        inst.getKeyValuePairCount() == 0 &&
        "AllocTypedNonEnumObjectInst with properties should be lowered to LIRAllocTypedNonEnumObjectFromBufferInst");
    assert(llvh::isa<EmptySentinel>(inst.getParentObject()));
    os_.indent(2);
    generateRegister(inst);
    os_ << " = ";
    os_ << "_sh_ljs_new_object(shr)";
    os_ << ";\n";
  }
  void generateCreateArgumentsLooseInst(CreateArgumentsLooseInst &inst) {
    hermes_fatal("CreateArgumentsLooseInst should have been lowered.");
  }
  void generateCreateArgumentsStrictInst(CreateArgumentsStrictInst &inst) {
    hermes_fatal("CreateArgumentsStrictInst should have been lowered.");
  }
  void generateCatchInst(CatchInst &inst) {
    // Restore the tryState to the state of the Try enclosing this block.
    TryStartInst *enclosing = enclosingTrys_.at(inst.getParent());
    uint32_t stateToRestore = enclosing ? tryIDs_.at(enclosing) : 0;
    os_ << "  tryState = " << stateToRestore << ";\n";
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_get_clear_thrown_value(shr);\n";
  }
  void generateDebuggerInst(DebuggerInst &inst) {
    hermes_fatal("DebuggerInst should have been deleted.");
  }
  void generateCreateRegExpInst(CreateRegExpInst &inst) {
    os_.indent(2);

    uint32_t patternStrID =
        moduleGen_.stringTable.add(inst.getPattern()->getValue().str());
    uint32_t flagsStrID =
        moduleGen_.stringTable.add(inst.getFlags()->getValue().str());

    // Semantic resolution compiled every regexp literal into the Context.
    CompiledRegExp &regexp = F_.getContext().getCompiledRegExp(
        inst.getPattern()->getValue().getUnderlyingPointer(),
        inst.getFlags()->getValue().getUnderlyingPointer());
    uint32_t regExpID = moduleGen_.regExpTable.addRegExp(&regexp);

    generateValue(inst);
    os_ << " = ";
    os_ << "_sh_ljs_create_regexp(shr, shUnit, ";
    os_ << llvh::format("get_symbols(shUnit)[%u]", patternStrID);
    os_ << ", ";
    os_ << llvh::format("get_symbols(shUnit)[%u]", flagsStrID);
    os_ << ", " << regExpID << ");\n";
  }
  void generateTryEndInst(TryEndInst &inst) {
    // Restore the tryState to the state of the Try enclosing the catch.
    TryStartInst *enclosing = enclosingTrys_.at(inst.getBranchDest());
    uint32_t stateToRestore = enclosing ? tryIDs_.at(enclosing) : 0;
    os_ << "  tryState = " << stateToRestore << ";\n";
    os_ << "  goto ";
    generateBasicBlockLabel(inst.getBranchDest(), os_, bbMap_);
    os_ << ";\n";
  }
  void generateBranchIfBuiltinInst(BranchIfBuiltinInst &inst) {
    os_ << "  if (_sh_ljs_get_builtin_closure(shr, "
        << (uint32_t)inst.getBuiltinIndex() << ").raw == ";
    generateValue(*inst.getArgument());
    os_ << ".raw) goto ";
    generateBasicBlockLabel(inst.getTrueBlock(), os_, bbMap_);
    os_ << ";\n";
    os_ << "else goto ";
    generateBasicBlockLabel(inst.getFalseBlock(), os_, bbMap_);
    os_ << ";\n";
  }
  void generateGetNewTargetInst(GetNewTargetInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = frame[" << hbc::StackFrameLayout::NewTarget << "];\n";
  }
  void generateIteratorBeginInst(IteratorBeginInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_iterator_begin_rjs(shr, ";
    generateRegisterPtr(*inst.getSourceOrNext());
    os_ << ");\n";
  }
  void generateIteratorNextInst(IteratorNextInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_iterator_next_rjs(shr, ";
    generateRegisterPtr(*inst.getIterator());
    os_ << ", ";
    generateRegisterPtr(*inst.getSourceOrNext());
    os_ << ");\n";
  }
  void generateIteratorCloseInst(IteratorCloseInst &inst) {
    os_ << "  _sh_ljs_iterator_close_rjs(shr, ";
    generateRegisterPtr(*inst.getIterator());
    os_ << ", " << boolStr(inst.getIgnoreInnerException());
    os_ << ");\n";
  }
  void generateUnreachableInst(UnreachableInst &inst) {
    os_.indent(2);
    os_ << "_sh_unreachable();\n";
  }
  void generateCreateFunctionInst(CreateFunctionInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_create_closure" << "(shr, ";
    if (llvh::isa<EmptySentinel>(inst.getScope()))
      os_ << "NULL";
    else
      generateRegisterPtr(*inst.getScope());
    os_ << ", ";
    moduleGen_.nativeFunctionTable.generateFunctionLabel(
        inst.getFunctionCode(), os_);
    os_ << ", ";
    os_ << "&s_function_info_table["
        << moduleGen_.nativeFunctionTable.getIndex(inst.getFunctionCode())
        << "]" << ", shUnit);\n";
  }
  void generateCreateGeneratorInst(CreateGeneratorInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_create_generator_object" << "(shr, ";
    if (llvh::isa<EmptySentinel>(inst.getScope()))
      os_ << "NULL";
    else
      generateRegisterPtr(*inst.getScope());
    os_ << ", ";
    moduleGen_.nativeFunctionTable.generateFunctionLabel(
        inst.getFunctionCode(), os_);
    os_ << ", ";
    os_ << "&s_function_info_table["
        << moduleGen_.nativeFunctionTable.getIndex(inst.getFunctionCode())
        << "]" << ", shUnit);\n";
  }
  void generateCreateClassInst(CreateClassInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    bool isBaseClass = llvh::isa<EmptySentinel>(inst.getSuperClass());
    os_ << " = _sh_ljs_create_class(shr,";
    if (llvh::isa<EmptySentinel>(inst.getScope()))
      os_ << "NULL";
    else
      generateRegisterPtr(*inst.getScope());
    os_ << ", ";
    moduleGen_.nativeFunctionTable.generateFunctionLabel(
        inst.getFunctionCode(), os_);
    os_ << ", ";
    os_ << "&s_function_info_table["
        << moduleGen_.nativeFunctionTable.getIndex(inst.getFunctionCode())
        << "]";
    os_ << ", (SHUnit *)shUnit, ";
    generateRegisterPtr(*inst.getHomeObjectOutput());
    os_ << ", ";
    if (isBaseClass) {
      os_ << "NULL";
    } else {
      generateRegisterPtr(*inst.getSuperClass());
    }
    os_ << ");\n";
  }
  void generateBranchInst(BranchInst &inst) {
    os_ << "  goto ";
    generateBasicBlockLabel(inst.getBranchDest(), os_, bbMap_);
    os_ << ";";
  }
  void generateReturnInst(ReturnInst &inst) {
    if (!tryIDs_.empty()) {
      os_ << "  _sh_end_try(shr, &jmpBuf);\n";
    }
    os_ << "  _sh_leave(shr, &locals.head, frame);\n  return ";
    generateValue(*inst.getValue());
    os_ << ";\n";
  }
  void generateThrowInst(ThrowInst &inst) {
    os_ << "  _sh_throw(shr, ";
    generateValue(*inst.getThrownValue());
    os_ << ");\n";
  }
  void generateThrowTypeErrorInst(ThrowTypeErrorInst &inst) {
    os_ << "  _sh_throw_type_error(shr, ";
    generateRegisterPtr(*inst.getMessage());
    os_ << ");\n";
  }
  void generateSwitchInst(SwitchInst &inst) {
    hermes_fatal("SwitchInst should have been lowered");
  }
  void generateCondBranchInst(CondBranchInst &inst) {
    os_.indent(2);
    os_ << "if(";
    if (inst.getCondition()->getType().isBooleanType()) {
      os_ << "_sh_ljs_get_bool(";
    } else {
      os_ << "_sh_ljs_to_boolean(";
    }
    generateRegister(*inst.getCondition());
    os_ << ")) ";
    os_ << "goto ";
    generateBasicBlockLabel(inst.getTrueDest(), os_, bbMap_);
    os_ << ";\n  goto ";
    generateBasicBlockLabel(inst.getFalseDest(), os_, bbMap_);
    os_ << ";\n";
  }
  void generateGetPNamesInst(GetPNamesInst &inst) {
    os_.indent(2);
    generateValue(*inst.getIterator());
    os_ << " = _sh_ljs_get_pname_list_rjs(shr, ";
    generateRegisterPtr(*inst.getBase());
    os_ << ", ";
    generateRegisterPtr(*inst.getIndex());
    os_ << ", ";
    generateRegisterPtr(*inst.getSize());
    os_ << ");\n";

    os_.indent(2) << "if (_sh_ljs_is_undefined(";
    generateValue(*inst.getIterator());
    os_ << ")) goto ";
    generateBasicBlockLabel(inst.getOnEmptyDest(), os_, bbMap_);
    os_ << ";\n";

    os_.indent(2) << "goto ";
    generateBasicBlockLabel(inst.getOnSomeDest(), os_, bbMap_);
    os_ << ";\n";
  }
  void generateGetNextPNameInst(GetNextPNameInst &inst) {
    os_.indent(2);
    generateValue(*inst.getPropertyAddr());
    os_ << " = _sh_ljs_get_next_pname_rjs(shr, ";
    generateRegisterPtr(*inst.getIteratorAddr());
    os_ << ", ";
    generateRegisterPtr(*inst.getBaseAddr());
    os_ << ", ";
    generateRegisterPtr(*inst.getIndexAddr());
    os_ << ", ";
    generateRegisterPtr(*inst.getSizeAddr());
    os_ << ");\n";

    os_.indent(2) << "if (_sh_ljs_is_undefined(";
    generateValue(*inst.getPropertyAddr());
    os_ << ")) goto ";
    generateBasicBlockLabel(inst.getOnLastDest(), os_, bbMap_);
    os_ << ";\n";

    os_.indent(2) << "goto ";
    generateBasicBlockLabel(inst.getOnSomeDest(), os_, bbMap_);
    os_ << ";\n";
  }
  void generateTryStartInst(TryStartInst &inst) {
    uint32_t stateToEnter = tryIDs_.at(&inst);
    os_.indent(2) << "tryState = " << stateToEnter << ";\n";
    os_ << "  goto ";
    generateBasicBlockLabel(inst.getTryBody(), os_, bbMap_);
    os_ << ";\n";
  }
  void generateHBCCompareBranchInst(HBCCompareBranchInst &inst) {
    unimplemented(inst);
  }
  void generateHBCCmpBrTypeOfIsInst(HBCCmpBrTypeOfIsInst &inst) {
    unimplemented(inst);
  }
  void generateUIntSwitchImmInst(UIntSwitchImmInst &inst) {
    unimplemented(inst);
  }
  void generateStringSwitchImmInst(StringSwitchImmInst &inst) {
    unimplemented(inst);
  }
  void generateSaveAndYieldInst(SaveAndYieldInst &inst) {
    unimplemented(inst);
  }
  /// Populate the outgoing registers which are constants.
  /// These aren't added by LowerCalls, they're not representable in lowered IR.
  /// We want to set them up to be able to do the call without going through
  /// doCall.
  void setupCallInline(BaseCallInst &inst) {
    auto stackReg = [maxArgsRegs = ra_.getMaxArgumentRegisters()](
                        int32_t index) -> sh::Register {
      return sh::Register(
          sh::RegClass::RegStack, (sh::RegIndex)((int32_t)maxArgsRegs + index));
    };

    os_.indent(2);
    generateRegister(stackReg(hbc::StackFrameLayout::PreviousFrame));
    os_ << " = _sh_ljs_native_pointer(frame);\n";

    os_.indent(2);
    generateRegister(stackReg(hbc::StackFrameLayout::SavedIP));
    os_ << " = _sh_ljs_native_pointer((void*)0);\n";

    os_.indent(2);
    generateRegister(stackReg(hbc::StackFrameLayout::SavedCodeBlock));
    os_ << " = _sh_ljs_native_pointer((void*)0);\n";

    os_.indent(2);
    generateRegister(stackReg(hbc::StackFrameLayout::SHLocals));
    os_ << " = _sh_ljs_native_pointer((void*)0);\n";

    os_.indent(2);
    generateRegister(stackReg(hbc::StackFrameLayout::ArgCount));
    os_ << " = _sh_ljs_native_uint32(" << (inst.getNumArguments() - 1)
        << ");\n";

    // new.target is set up in LowerCalls.
  }
  void generateCallInst(CallInst &inst) {
    if (inst.getAttributes(inst.getModule()).isNativeJSFunction) {
      // Fast paths for calling NativeJSFunction.
      setupCallInline(inst);
      os_.indent(2);
      generateRegister(inst);
      os_ << " = ";
      if (auto *targetFunc = llvh::dyn_cast<Function>(inst.getTarget())) {
        // Fast path, avoid all indirection and just call the function.
        moduleGen_.nativeFunctionTable.generateFunctionLabel(targetFunc, os_);
        os_ << "(shr);\n";
      } else {
        // Avoid doCall and perform a legacy call on the pointer.
        os_ << "((SHNativeJSFunction *)_sh_ljs_get_pointer(";
        generateValue(*inst.getCallee());
        os_ << "))->functionPtr(shr);\n";
      }
    } else if (inst.getAttributes(inst.getModule()).isMetroRequire) {
      auto *litNum = llvh::cast<LiteralNumber>(inst.getArgument(1));
      assert(
          litNum->isUInt32Representible() &&
          "Or should not have been optimized.");
      uint32_t modId = litNum->asUInt32();

      os_.indent(2);
      generateRegister(inst);
      os_ << " = _sh_ljs_callRequire(shr, &shUnit->moduleExports, ";
      generateRegisterPtr(*inst.getCallee());
      os_ << ", " << modId << ");\n";
    } else {
      os_.indent(2);
      generateRegister(inst);
      os_ << " = _sh_ljs_call(shr, frame, " << inst.getNumArguments() - 1
          << ");\n";
    }
  }

  void generateGetBuiltinClosureInst(GetBuiltinClosureInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_get_builtin_closure(shr, "
        << (uint32_t)inst.getBuiltinIndex() << ");\n";
  }
  void generateCallBuiltinInst(CallBuiltinInst &inst) {
    // These builtins only exist to be called from typed code, so call their
    // implementations directly instead of going through the builtin table.
    const char *directCall = nullptr;
    switch (inst.getBuiltinIndex()) {
      case BuiltinMethod::HermesBuiltin_objectKeys:
        directCall = "_sh_object_keys";
        break;
      case BuiltinMethod::HermesBuiltin_objectValues:
        directCall = "_sh_object_values";
        break;
      case BuiltinMethod::HermesBuiltin_objectEntries:
        directCall = "_sh_object_entries";
        break;
      default:
        break;
    }
    if (directCall) {
      assert(
          inst.getNumArguments() == 3 &&
          "object props builtins take 'this' plus the object and prototype");
      os_.indent(2);
      generateRegister(inst);
      os_ << " = " << directCall << "(shr, ";
      generateValue(*inst.getArgument(1));
      os_ << ", ";
      generateValue(*inst.getArgument(2));
      os_ << ");\n";
      return;
    }
    if (inst.getBuiltinIndex() == BuiltinMethod::Math_sqrt) {
      if (inst.getNumArguments() == 2 &&
          inst.getArgument(1)->getType().isNumberType()) {
        os_.indent(2);
        generateRegister(inst);
        os_ << " = _sh_ljs_double(sqrt(_sh_ljs_get_double(";
        generateValue(*inst.getArgument(1));
        os_ << ")));\n";
        return;
      }
    }
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_call_builtin(shr, frame, " << inst.getNumArguments() - 1
        << ", " << (uint32_t)inst.getBuiltinIndex() << ");\n";
  }
  void generateHBCCallWithArgCountInst(HBCCallWithArgCountInst &inst) {
    unimplemented(inst);
  }
  void generateHBCCallNInst(HBCCallNInst &inst) {
    unimplemented(inst);
  }
  void generateResumeGeneratorInst(ResumeGeneratorInst &inst) {
    unimplemented(inst);
  }
  void generateLIRGetGlobalObjectInst(LIRGetGlobalObjectInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_get_global_object(shr);\n";
  }
  void generateCreateScopeInst(CreateScopeInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_create_environment(shr, ";
    if (llvh::isa<EmptySentinel>(inst.getParentScope()))
      os_ << "NULL";
    else
      generateRegisterPtr(*inst.getParentScope());
    os_ << ", " << inst.getVariableScope()->getVariables().size() << ");\n";
  }
  void generateHBCCreateFunctionEnvironmentInst(
      HBCCreateFunctionEnvironmentInst &inst) {
    hermes_fatal("HBCCreateEnvironmentInst is not used by SH.");
  }
  void generateCoerceThisNSInst(CoerceThisNSInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_coerce_this_ns(shr, ";
    generateRegister(*inst.getSingleOperand());
    os_ << ");\n";
  }
  void generateLIRGetThisNSInst(LIRGetThisNSInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_coerce_this_ns(shr, frame["
        << hbc::StackFrameLayout::ThisArg << "]);\n";
  }
  void generateCreateThisInst(CreateThisInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_create_this(shr, &";
    generateRegister(*inst.getClosure());
    os_ << ", &";
    generateRegister(*inst.getNewTarget());
    os_ << ", ";
    Module *M = F_.getParent();
    auto *protoStr =
        M->getLiteralString(M->getContext().getIdentifier("prototype"));
    genReadIC(protoStr);
    os_ << ", &shUnit->object_literal_class_cache["
        << moduleGen_.literalBuffers.serializedLiteralOffsetFor(&inst)
               .shapeTableIdx
        << "]);\n";
  }
  void generateLIRGetArgumentsPropByValLooseInst(
      LIRGetArgumentsPropByValLooseInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_get_arguments_prop_by_val_loose(shr, frame, ";
    generateRegisterPtr(*inst.getIndex());
    os_ << ", ";
    generateRegisterPtr(*inst.getLazyRegister());
    os_ << ");\n";
  }
  void generateLIRGetArgumentsPropByValStrictInst(
      LIRGetArgumentsPropByValStrictInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_get_arguments_prop_by_val_strict(shr, frame, ";
    generateRegisterPtr(*inst.getIndex());
    os_ << ", ";
    generateRegisterPtr(*inst.getLazyRegister());
    os_ << ");\n";
  }
  void generateGetConstructedObjectInst(GetConstructedObjectInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_is_object(";
    generateRegister(*inst.getConstructorReturnValue());
    os_ << ") ? ";
    generateRegister(*inst.getConstructorReturnValue());
    os_ << " : ";
    generateRegister(*inst.getThisValue());
    os_ << ";\n";
  }
  void generateLIRAllocObjectFromBufferInst(
      LIRAllocObjectFromBufferInst &inst) {
    os_.indent(2);
    generateRegister(inst);

    auto [shapeIdx, valIdx] =
        moduleGen_.literalBuffers.serializedLiteralOffsetFor(&inst);

    os_ << " = ";
    if (inst.getKeyValuePairCount() == 0) {
      assert(
          !llvh::isa<EmptySentinel>(inst.getParentObject()) &&
          "must have a parent, otherwise we should use AllocObjectLiteral");
      os_ << "_sh_ljs_new_object_with_parent(shr, shUnit, ";
      generateRegisterPtr(*inst.getParentObject());
      os_ << ", " << shapeIdx << ")";
    } else if (llvh::isa<EmptySentinel>(inst.getParentObject())) {
      os_ << "_sh_ljs_new_object_with_buffer(shr, shUnit, ";
    } else {
      os_ << "_sh_ljs_new_object_with_buffer_and_parent(shr, shUnit, ";
      generateRegisterPtr(*inst.getParentObject());
      os_ << ", ";
    }
    if (inst.getKeyValuePairCount() > 0) {
      os_ << shapeIdx << ", ";
      os_ << valIdx << ")";
    }
    os_ << ";\n";
  }
  void generateLIRAllocTypedObjectFromBufferInst(
      LIRAllocTypedObjectFromBufferInst &inst) {
    os_.indent(2);
    generateRegister(inst);

    auto [shapeIdx, valIdx] =
        moduleGen_.literalBuffers.serializedLiteralOffsetFor(&inst);

    os_ << " = ";
    if (inst.getKeyValuePairCount() == 0) {
      os_ << "_sh_ljs_new_object_with_parent(shr, shUnit, ";
      generateRegisterPtr(*inst.getParentObject());
      os_ << ", " << shapeIdx << ")";
    } else {
      os_ << "_sh_new_typed_object_with_buffer(shr, shUnit, ";
      generateRegisterPtr(*inst.getParentObject());
      os_ << ", ";
      os_ << shapeIdx << ", ";
      os_ << valIdx;
      os_ << ")";
    }
    os_ << ";\n";
  }
  void generateLIRAllocTypedNonEnumObjectFromBufferInst(
      LIRAllocTypedNonEnumObjectFromBufferInst &inst) {
    os_.indent(2);
    generateRegister(inst);

    auto [shapeIdx, valIdx] =
        moduleGen_.literalBuffers.serializedLiteralOffsetFor(&inst);

    os_ << " = ";
    if (inst.getKeyValuePairCount() == 0) {
      os_ << "_sh_ljs_new_object_with_parent(shr, shUnit, ";
      generateRegisterPtr(*inst.getParentObject());
      os_ << ", " << shapeIdx << ")";
    } else {
      os_ << "_sh_new_typed_non_enum_object_with_buffer(shr, shUnit, ";
      generateRegisterPtr(*inst.getParentObject());
      os_ << ", ";
      os_ << shapeIdx << ", ";
      os_ << valIdx;
      os_ << ")";
    }
    os_ << ";\n";
  }
  void generateHBCProfilePointInst(HBCProfilePointInst &inst) {
    unimplemented(inst);
  }
  void generatePrLoadInst(PrLoadInst &inst) {
    os_.indent(2);
    generateValue(inst);
    os_ << " = ";
    os_ << "_sh_prload(shr, ";
    generateRegister(*inst.getObject());
    os_ << ", " << inst.getPropIndex() << ");\n";
  }
  void generatePrStoreInst(PrStoreInst &inst) {
    os_.indent(2);
    const char *suffix = "";
    Type propType = inst.getStoredValue()->getType();
    if (propType.isNumberType()) {
      suffix = "_number";
    } else if (propType.isBooleanType()) {
      suffix = "_bool";
    } else if (propType.isObjectType()) {
      suffix = "_object";
    } else if (propType.isStringType()) {
      suffix = "_string";
    }
    os_ << "_sh_prstore" << suffix << "(shr, ";
    generateRegisterPtr(*inst.getObject());
    os_ << ", " << inst.getPropIndex() << ", ";
    generateRegisterPtr(*inst.getStoredValue());
    os_ << ");\n";
  }
  void generateUnionNarrowTrustedInst(UnionNarrowTrustedInst &inst) {
    // Since all values are currently NaN-boxed, narrowing is just a move.
    // TODO(T155912625): Revisit this once union narrow lowering is fixed.
    sh::Register dstReg = ra_.getRegister(&inst);
    if (ra_.isAllocated(inst.getSingleOperand()) &&
        dstReg == ra_.getRegister(inst.getSingleOperand())) {
      return;
    }

    os_.indent(2);
    generateRegister(dstReg);
    os_ << " = ";
    generateValue(*inst.getSingleOperand());
    os_ << ";\n";
  }

  /// \return the name of the SH function for checking whether a value is of
  /// a specific type.
  static llvh::StringLiteral nameOfFunctionCheckingForType(
      TypeContext &tc,
      Type type) {
    assert(!type.isNoType() && "type must be non-zero");
    switch (tc.getFirstKind(type)) {
      case TypeKind::Empty:
        return "_sh_ljs_is_empty";
      case TypeKind::Uninit:
      case TypeKind::Undefined:
        return "_sh_ljs_is_undefined";
      case TypeKind::Null:
        return "_sh_ljs_is_null";
      case TypeKind::Boolean:
        return "_sh_ljs_is_bool";
      case TypeKind::String:
        return "_sh_ljs_is_string";
      case TypeKind::Number:
      // The number subtypes are all represented as doubles at runtime.
      case TypeKind::Int32:
      case TypeKind::Uint32:
      case TypeKind::UInt31:
        return "_sh_ljs_is_double";
      case TypeKind::BigInt:
        return "_sh_ljs_is_bigint";
      case TypeKind::Symbol:
        return "_sh_ljs_is_symbol";
      case TypeKind::Environment:
        hermes_fatal("cannot check for environment type");
      case TypeKind::PrivateName:
        hermes_fatal("cannot check for PrivateName type");
      case TypeKind::FunctionCode:
        hermes_fatal("cannot check for functionCode type");
      case TypeKind::Object:
        return "_sh_ljs_is_object";
      default:
        break;
    }
    hermes_fatal("invalid type for checking");
  }

  /// \param badTypes the types which cause an exception if they are the input.
  void _typeCastHelper(
      Type resultType,
      Type badTypes,
      sh::Register dstReg,
      sh::Register srcReg,
      const char *errorExpr) {
    os_.indent(2);
    os_ << "if (";

    // Are there fewer "bad" types than "good" types? That determines which we
    // check.
    auto [checkTypes, negativeCheck] =
        typeCtx_.countKinds(badTypes) < typeCtx_.countKinds(resultType)
        ? std::make_pair(badTypes, true)
        : std::make_pair(resultType, false);

    if (!negativeCheck)
      os_ << "!(";
    {
      bool first = true;
      for (Type t : typeCtx_.arms(checkTypes)) {
        if (!first)
          os_ << " || ";
        os_ << nameOfFunctionCheckingForType(typeCtx_, t) << '(';
        generateRegister(srcReg);
        os_ << ')';
        first = false;
      }
    }
    if (!negativeCheck)
      os_ << ')';

    os_ << ") " << errorExpr << ";\n";

    // Set the result, but do nothing if the registers are the same.
    if (dstReg != srcReg) {
      os_.indent(2);
      generateRegister(dstReg);
      os_ << " = ";
      generateRegister(srcReg);
      os_ << ";\n";
    }
  }
  void generateCheckedTypeCastInst(CheckedTypeCastInst &inst) {
    const Type resultType = inst.getType();
    const Type inputType = inst.getCheckedValue()->getType();

    assert(
        ra_.isAllocated(inst.getCheckedValue()) &&
        "operand of CheckedCastInst must have an allocated register");
    sh::Register srcReg = ra_.getRegister(inst.getCheckedValue());
    sh::Register dstReg = ra_.getRegister(&inst);

    // Are the input and output type the same?
    if (typeCtx_.isSubsetOf(inputType, resultType)) {
      // If so, just move the value, but do nothing if the registers are the
      // same.
      if (dstReg != srcReg) {
        os_.indent(2);
        generateRegister(dstReg);
        os_ << " = ";
        generateValue(*inst.getCheckedValue());
        os_ << ";\n";
      }
      return;
    }

    // TODO: generate a type-specific error.
    _typeCastHelper(
        resultType,
        typeCtx_.subtractTy(inputType, resultType),
        dstReg,
        srcReg,
        "_sh_throw_type_error_ascii(shr, \"Checked cast failed\")");
  }
  void generateThrowIfInst(ThrowIfInst &inst) {
    assert(
        ra_.isAllocated(inst.getCheckedValue()) &&
        "operand of ThrowIf must have an allocated register");

    Type badTypes = inst.getInvalidTypes()->getData();
    assert(
        !badTypes.isNoType() &&
        typeCtx_.isSubsetOf(badTypes, Type::createEmptyOrUninit()) &&
        "invalidTypes set can only contain Empty or Uninit");

    _typeCastHelper(
        inst.getType(),
        badTypes,
        ra_.getRegister(&inst),
        ra_.getRegister(inst.getCheckedValue()),
        "_sh_throw_empty(shr)");
  }
  void generateThrowIfThisInitializedInst(ThrowIfThisInitializedInst &inst) {
    os_.indent(2);
    os_ << "_sh_ljs_throw_if_this_initialized(shr, ";
    generateRegister(*inst.getDerivedClassCheckedThis());
    os_ << ");\n";
  }
  void generateLIRDeadValueInst(LIRDeadValueInst &inst) {
    os_.indent(2);
    os_ << "__builtin_unreachable();\n";
    os_.indent(2);
    // Initialize the register with whatever.
    generateRegister(inst);
    os_ << " = _sh_ljs_double(0);\n";
  }
  void generateFastArrayLoadInst(FastArrayLoadInst &inst) {
    os_.indent(2);
    generateValue(inst);
    os_ << " = ";
    os_ << "_sh_fastarray_load(shr, ";
    generateRegisterPtr(*inst.getArray());
    os_ << ", _sh_ljs_get_double(";
    generateRegister(*inst.getIndex());
    os_ << "));\n";
  }
  void generateFastArrayStoreInst(FastArrayStoreInst &inst) {
    os_.indent(2);
    os_ << "_sh_fastarray_store(shr, ";
    generateRegisterPtr(*inst.getStoredValue());
    os_ << ", ";
    generateRegisterPtr(*inst.getArray());
    os_ << ", _sh_ljs_get_double(";
    generateRegister(*inst.getIndex());
    os_ << "));\n";
  }
  void generateFastArrayPushInst(FastArrayPushInst &inst) {
    os_.indent(2);
    os_ << "_sh_fastarray_push(shr, ";
    generateRegisterPtr(*inst.getPushedValue());
    os_ << ", ";
    generateRegisterPtr(*inst.getArray());
    os_ << ");\n";
  }
  void generateFastArrayAppendInst(FastArrayAppendInst &inst) {
    os_.indent(2);
    os_ << "_sh_fastarray_append(shr, ";
    generateRegisterPtr(*inst.getOther());
    os_ << ", ";
    generateRegisterPtr(*inst.getArray());
    os_ << ");\n";
  }
  void generateFastArrayLengthInst(FastArrayLengthInst &inst) {
    os_.indent(2);
    generateValue(inst);
    os_ << " = ";
    os_ << "_sh_fastarray_length(shr, ";
    generateRegisterPtr(*inst.getArray());
    os_ << ");\n";
  }
  void generateGetNativeRuntimeInst(GetNativeRuntimeInst &inst) {
    os_.indent(2);
    generateRegister(inst);
    os_ << " = _sh_ljs_native_pointer(shr);\n";
  }
  void generateNativeCallInst(NativeCallInst &inst) {
    NativeSignature *sig = inst.getSignature()->getData();
    unsigned parens = 0;

    os_.indent(2);
    if (sig->result() != NativeCType::c_void) {
      generateValue(inst);
      os_ << " = ";
      convertFromNativeResult(sig->result(), parens);
    }

    // If the callee is a literal, generate simpler code.
    if (auto *ne = llvh::dyn_cast<LiteralNativeExtern>(inst.getCallee())) {
      os_ << ne->getData()->name()->str();
    } else {
      os_ << "((";
      sig->format(os_);
      os_ << ")_sh_ljs_get_native_pointer(";
      generateValue(*inst.getCallee());
      os_ << "))";
    }
    os_ << '(';
    for (unsigned i = 0, e = inst.getNumArgs(); i != e; ++i) {
      if (i)
        os_ << ", ";
      convertToNativeArg(sig->params()[i], inst.getArg(i));
    }
    // End of argument list.
    os_ << ')';
    while (parens--)
      os_ << ')';

    os_ << ";\n";

    // If the function returns void, we must synthesize an 'undefined'.
    if (sig->result() == NativeCType::c_void) {
      os_.indent(2);
      generateValue(inst);
      os_ << " = _sh_ljs_undefined();\n";
    }
  }
  void generateLazyCompilationDataInst(LazyCompilationDataInst &inst) {
    hermes_fatal("lazy compilation unsupported in native backend");
  }
  void generateEvalCompilationDataInst(EvalCompilationDataInst &inst) {
    hermes_fatal("eval compilation unsupported in native backend");
  }
  /// Print code to convert the result of a native call to its corresponding
  /// JS type.
  ///
  /// \param ctype the native type of the result.
  /// \param parens the number of closing parentheses to print after. This
  ///     value is in-out.
  void convertFromNativeResult(NativeCType ctype, unsigned &parens) {
    if (ctype == NativeCType::c_void || ctype == NativeCType::c_hermes_value)
      return;

    MachineType mt = nativeContext_.md.mapCType(ctype);
    const MachineDesc::MTD &mtd = nativeContext_.md.getMTD(mt);

    ++parens;
    switch (mtd.cat) {
      case MachineDesc::Category::Int:
        // Integer types with sizes of 6 bytes or fewer can always fit in a
        // double.
        if (mtd.size <= 6) {
          os_ << "_sh_ljs_double((double)(" << ctype << ')';
        } else {
          ++parens;
          if (mtd.sign)
            os_ << "_sh_ljs_double(_sh_to_double_int64_or_throw(shr, ";
          else
            os_ << "_sh_ljs_double(_sh_to_double_uint64_or_throw(shr, ";
        }
        break;
      case MachineDesc::Category::FP:
        // Note that we treat floats as untrusted, because it is not defined
        // how the bit pattern of a float is transferred to a double.
        os_ << "_sh_ljs_untrusted_double(";
        break;
      case MachineDesc::Category::Ptr:
        os_ << "_sh_ljs_native_pointer_or_throw(shr, ";
        break;
    }
  }
  /// Convert a JS value to its corresponding native argument type.
  void convertToNativeArg(NativeCType ctype, Value *arg) {
    MachineType mt = nativeContext_.md.mapCType(ctype);
    const MachineDesc::MTD &mtd = nativeContext_.md.getMTD(mt);
    int parens = 1;

    switch (mtd.cat) {
      case MachineDesc::Category::Int:
        // If the integer value fits in int32, use the normal truncating
        // machinery.
        if (mtd.size <= 4) {
          os_ << '(' << ctype << ")_sh_to_int32_double(_sh_ljs_get_double(";
          ++parens;
        } else {
          // If the integer value is larger than 32 bits, we must check whether
          // it is within the integer range that can be safely represented in
          // a double, and throw otherwise.
          assert(mtd.size == 8 && "Invalid integer size");
          if (mtd.sign)
            os_ << "_sh_to_int64_double_or_throw(shr, _sh_ljs_get_double(";
          else
            os_ << "_sh_to_uint64_double_or_throw(shr, _sh_ljs_get_double(";
          ++parens;
        }
        break;
      case MachineDesc::Category::FP:
        assert(
            (mt == MachineType::f32 || mt == MachineType::f64) &&
            "invalid FP type");
        if (mt == MachineType::f32)
          os_ << "(float)";
        os_ << "_sh_ljs_get_double(";
        break;
      case MachineDesc::Category::Ptr:
        os_ << "_sh_ljs_get_native_pointer(";
        break;
    }

    generateValue(*arg);
    while (parens--)
      os_ << ')';
  }
};

/// Lower module IR to LIR, so it is suitable for register allocation.
/// \return true if lowering completed successfully.
bool lowerModuleIR(Module *M, bool optimize) {
  PassManager PM("SH Lower");
  PM.addLowerGeneratorFunction();
  // LowerPrivateBrandCheck produces ThrowTypeErrorInst, which is lowered by
  // PeepholeLowering. Therefore, LowerPrivateBrandCheck must run before.
  PM.addPass(createLowerPrivateBrandCheck());
  // Lowering ExponentiationOperator and ThrowTypeError (in PeepholeLowering)
  // needs to run before LowerBuiltinCalls because it introduces calls to
  // HermesInternal.
  PM.addPass(sh::createPeepholeLowering(optimize));
  // LowerBuiltinCalls needs to run before the rest of the lowering.
  PM.addPass(createLowerBuiltinCalls());
  PM.addPass(new LowerNumericProperties());
  PM.addPass(new LowerAllocObjectLiteral());
  PM.addPass(new hbc::LowerArgumentsArray());
  PM.addPass(new LimitAllocArray(UINT16_MAX));
  PM.addPass(new hbc::DedupReifyArguments());
  // TODO Consider supporting LowerSwitchIntoJumpTables for optimization
  PM.addPass(new SwitchLowering());
  if (optimize) {
    // TODO(T204084366): TypeInference must run before OptEnvironmentInit,
    // because the latter will remove stores that may affect the inferred type.
    PM.addTypeInference();
    // OptEnvironmentInit checks for LiteralUndefined, so it needs to run before
    // LoadConstants.
    PM.addPass(createOptEnvironmentInit());
  }
  PM.addPass(sh::createLoadConstants());
  PM.addPass(createLowerScopes());
  if (optimize) {
    // Move loads to child blocks if possible.
    PM.addCodeMotion();
    // Eliminate common LIRLoadConstInsts.
    // TODO(T140823187): Run before CodeMotion too.
    // Avoid pushing LIRLoadConstInsts down into individual blocks,
    // preventing their elimination.
    PM.addCSE();
    // Drop unused LoadParamInsts.
    PM.addDCE();
  }
  return PM.run(M);
}

/// Perform final lowering of a register-allocated function's IR.
void lowerAllocatedFunctionIR(
    Function *F,
    sh::SHRegisterAllocator &RA,
    bool optimize) {
  PassManager PM("SH LowerAllocatedFunctionIR");
  PM.addPass(new LowerStoreInstrs(RA));
  PM.addPass(sh::createLowerCalls(RA));
  if (optimize) {
    PM.addPass(new MovElimination(RA));
    PM.addPass(sh::createRecreateCheapValues(RA));
  }
  PM.addPass(new RemoveMovs(RA));
  PM.run(F);
}

/// Converts Function \p F into valid C code and outputs it through \p OS.
void generateFunction(
    Function &F,
    hermes::sh::LineDirectiveEmitter &OS,
    ModuleGen &moduleGen,
    uint32_t &nextWriteCacheIdx,
    uint32_t &nextReadCacheIdx,
    uint32_t &nextPrivateNameCacheIdx,
    BytecodeGenerationOptions options) {
  auto PO = hermes::postOrderAnalysis(&F);

  llvh::SmallVector<BasicBlock *, 16> order(PO.rbegin(), PO.rend());

  sh::SHRegisterAllocator RA(&F);
  RA.allocate(order);

  if (options.format == DumpRA) {
    RA.dump(order);
    return;
  }

  lowerAllocatedFunctionIR(&F, RA, options.optimizationEnabled);

  if (options.format == DumpLRA) {
    RA.dump(order);
    return;
  }

  assert(
      (options.format == DumpBytecode || options.format == EmitBundle) &&
      "All other dump formats must be handled before this.");

  // Set up the entry to the function to look like:
  // static SHLegacyValue _1_myFunc(SHRuntime *shr) {
  //   struct {
  //     SHLocals head;
  //     SHLegacyValue t0;
  //     SHLegacyValue t1;
  //     SHLegacyValue t2;
  //   } locals;
  //   SHLegacyValue *frame = _sh_enter(shr, &locals.head, 3);
  //   locals.head.count = 3;
  //   locals.t0 = _sh_ljs_undefined();
  //   locals.t1 = _sh_ljs_undefined();
  //   locals.t2 = _sh_ljs_undefined();

  unsigned bbCounter = 0;
  llvh::DenseMap<BasicBlock *, unsigned> bbMap;
  // Map from TryStart to an ID for that try/catch.
  llvh::MapVector<TryStartInst *, uint32_t> tryIDs{};
  // Start counting at 1 because 0 is reserved for blocks without a try.
  uint32_t tryIDCounter = 1;

  for (auto &B : order) {
    bbMap[B] = bbCounter;
    bbCounter++;
    if (auto *tryStart = llvh::dyn_cast<TryStartInst>(B->getTerminator())) {
      tryIDs[tryStart] = tryIDCounter;
      ++tryIDCounter;
    }
  }

  auto &srcMgr = F.getContext().getSourceErrorManager();
  OS << "// ";
  srcMgr.dumpCoords(OS, F.getSourceRange().Start);
  OS << '\n';

  InstrGen instrGen(
      OS,
      RA,
      options,
      bbMap,
      F,
      moduleGen,
      nextWriteCacheIdx,
      nextReadCacheIdx,
      nextPrivateNameCacheIdx,
      tryIDs);

  // Number of registers stored in the `locals` struct below.
  uint32_t localsSize = RA.getMaxRegisterUsage(sh::RegClass::LocalPtr);

  OS << "static SHLegacyValue ";
  moduleGen.nativeFunctionTable.generateFunctionLabel(&F, OS);
  OS << "(SHRuntime *shr) {\n";

  bool emitLineDirectives = options.emitLineDirectives;
  // Emit source location comments if requested, and we are *not* emitting line
  // directives. Otherwise, the resulting C is too noisy and unreadable.
  bool emitSrcLocComments = options.emitSourceLocations && !emitLineDirectives;
  if (emitLineDirectives) {
    OS.enableLineDirectives();
    // Initialize the location of the directive statement to source location of
    // the current function. This way, if the first BB initially doesn't have
    // instructions with source information, this value will provide a good
    // fallback.
    OS.setDirectiveInfo(F.getSourceRange().Start, srcMgr);
  }

  // In the global function, ensure that we are linking to the correct
  // library.
  if (F.isGlobalScope())
    OS << "  _SH_MODEL();\n";

  OS << "  struct {\n    SHLocals head;\n";

  for (size_t i = 0; i < localsSize; ++i) {
    OS << "    SHLegacyValue t" << i << ";\n";
  }

  OS << "  } locals;\n";

  // Emit the stack check if necessary.
  if (F.getParent()
          ->getContext()
          .getNativeContext()
          .settings.emitCheckNativeStack) {
    OS
        << (options.smallC
                ? "  _sh_check_native_stack_overflow(shr);\n"
                : "  _sh_check_native_stack_overflow_inline(shr, &locals);\n");
  }

  // Emit instructions for native stack traces if the debug info level is set to
  // at least throwing.
  bool emitNativeTraces =
      F.getContext().getDebugInfoSetting() >= DebugInfoSetting::THROWING;

  OS << "  SHLegacyValue *frame = _sh_enter(shr, &locals.head, "
     << (RA.getMaxArgumentRegisters() + hbc::StackFrameLayout::FirstLocal)
     << ");\n"
     << "  locals.head.count =" << localsSize << ";\n"
     << "  SHUnit *shUnit = shr->units[unit_index];\n";

  if (emitNativeTraces) {
    // Initialize the current SHUnit.
    OS << "  locals.head.unit = shUnit;\n";
    // Initialize the current source location to invalid.
    OS << "  locals.head.src_location_idx = "
       << SHSrcLocationTable::kInvalidLocIdx << ";\n";
  }

  // Initialize all registers to undefined.
  for (size_t i = 0; i < localsSize; ++i) {
    OS << "  locals.t" << i << " = _sh_ljs_undefined();\n";
  }
  for (size_t i = 0, e = RA.getMaxRegisterUsage(sh::RegClass::LocalNonPtr);
       i < e;
       ++i) {
    OS << "  SHLegacyValue np" << i << " = _sh_ljs_undefined();\n";
  }

  switch (F.getProhibitInvoke()) {
    case Function::ProhibitInvoke::ProhibitNone:
      break;
    case Function::ProhibitInvoke::ProhibitConstruct:
      OS << "  if (!_sh_ljs_is_undefined(frame["
         << hbc::StackFrameLayout::NewTarget << "]))\n"
         << "    _sh_throw_type_error_ascii(shr, \"Function is not a "
            "constructor\");\n";
      break;
    case Function::ProhibitInvoke::ProhibitCall:
      OS << "  if (_sh_ljs_is_undefined(frame["
         << hbc::StackFrameLayout::NewTarget << "]))\n"
         << "    _sh_throw_type_error_ascii(shr, \"Class constructor invoked "
            "without new\");\n";
      break;
  }

  // Initialize SHJmpBuf and emit the setjmp for the function-level try.
  // Every caught exception will jump to this _sh_try and be handled in the
  // switch statement under L_catch.
  // NOTE: We don't mark SHJmpBuf as volatile here because the jmp_buf will not
  // be stored in registers in practice, and setjmp doesn't take a volatile
  // parameter.
  // Set up tryState to 0 to indicate that we are not in a try block.
  // It's updated to the enclosing try's ID when we enter a try block,
  // and restored in Catch or TryEnd.
  if (!tryIDs.empty()) {
    OS << R"(
  SHJmpBuf jmpBuf;
  volatile uint32_t tryState = 0;
  if (__builtin_expect(_sh_try(shr, &jmpBuf), 0) != 0) goto L_catch;
)";
  }

  if (emitNativeTraces) {
    // Setup the frame to reference this SHLocals.
    OS << "  frame[" << hbc::StackFrameLayout::SHLocals << "]"
       << " = _sh_ljs_native_pointer(&locals.head);\n";
  }

  // The most recent index into the source location table that was used by a
  // throwing instruction.
  SHSrcLocationTable::IdxTy prevSrcLocationIdx =
      SHSrcLocationTable::kInvalidLocIdx;
  for (auto &B : order) {
    // Line directives of the BB label will match those of the first
    // instruction in this basic block.
    OS.setDirectiveInfo(B->front().getLocation(), srcMgr);
    OS << "\n";
    generateBasicBlockLabel(B, OS, bbMap);
    OS << ":\n"
       // Print a no-op expression, since the line after a label must be an
       // expression (as opposed to a declaration).
       << "  ;\n";

    // The most recent location of an instruction that was printed as line
    // comments in the C file.
    SMLoc prevCommentLoc{};
    // The first instruction in the function should always have its line info
    // printed, since it has no previous instruction that it could be a
    // duplicate of.
    bool firstInst = true;
    for (auto &I : *B) {
      OS.setDirectiveInfo(I.getLocation(), srcMgr);
      if (emitSrcLocComments) {
        SMLoc curLoc = I.getLocation();
        // We only print out a line comment if the source location has changed
        // from the previous line comment printed.
        if (curLoc != prevCommentLoc || firstInst) {
          OS << "  // ";
          if (curLoc.isValid()) {
            srcMgr.dumpCoords(OS, curLoc);
          } else {
            // If we don't find any source location info, explicitly state that.
            OS << "no-src-info";
          }
          OS << '\n';
        }
        prevCommentLoc = curLoc;
        firstInst = false;
      }
      if (emitNativeTraces) {
        // If the instruction we are about to generate can throw, then we must
        // update the current source line we are executing in preparation.
        auto sideEffect = I.getSideEffect();
        if (sideEffect.getThrow()) {
          // If the instruction has no source location information, then just
          // use the invalid location.
          SHSrcLocationTable::IdxTy idx = SHSrcLocationTable::kInvalidLocIdx;
          SMLoc curLoc = I.getLocation();
          if (curLoc.isValid()) {
            hermes::SourceErrorManager::SourceCoords coords;
            if (srcMgr.findBufferLineAndLoc(curLoc, coords)) {
              idx = moduleGen.srcLocationTable.getIndex(srcMgr, coords);
            }
          }
          if (idx != prevSrcLocationIdx) {
            // We update the source location if the source location index of the
            // current instruction has changed from the previous update.
            OS << "  locals.head.src_location_idx = " << idx << ";\n";
          }
          prevSrcLocationIdx = idx;
        }
      }
      instrGen.generate(I);
    }
  }

  if (!tryIDs.empty()) {
    OS << R"(
L_catch:
  if (tryState == 0) {
    _sh_end_try(shr, &jmpBuf);
    _sh_throw_current(shr);
  }
  )";
    OS << "_sh_catch_no_pop(shr, (SHLocals*)&locals, frame, "
       << (RA.getMaxArgumentRegisters() + hbc::StackFrameLayout::FirstLocal)
       << ");\n";

    OS << R"(
  switch (tryState) {
    default:
      abort();
)";

    // For each of the try blocks, generate a case statement that will jump to
    // the catch block for that try.
    for (const auto &[tryStart, id] : tryIDs) {
      OS << "    case " << id << ":\n";
      OS << "      goto ";
      generateBasicBlockLabel(tryStart->getCatchTarget(), OS, bbMap);
      OS << ";\n";
    }

    OS << "  }\n";
  }

  OS.disableLineDirectives();
  OS << "}\n";
}

/// Collect all the native externs used in the module.
std::unique_ptr<llvh::DenseSet<NativeExtern *>> collectUsedExterns(Module *M) {
  auto externs = llvh::make_unique<llvh::DenseSet<NativeExtern *>>();
  // Iterate all functions, basic blocks and operands, checking for
  // LiteralNativeExtern.
  for (auto &F : *M) {
    for (auto &BB : F) {
      for (auto &I : BB) {
        for (unsigned i = 0, count = I.getNumOperands(); i != count; ++i) {
          if (auto *ne = llvh::dyn_cast<LiteralNativeExtern>(I.getOperand(i))) {
            externs->insert(ne->getData());
          }
        }
      }
    }
  }
  return externs;
}

/// Generate the include statements requested by native externs.
void generateExternCIncludes(
    Module *M,
    llvh::raw_ostream &OS,
    const llvh::DenseSet<NativeExtern *> &usedExterns) {
  llvh::SmallSetVector<UniqueString *, 4> includes;
  for (NativeExtern *ne : M->getContext().getNativeContext().getAllExterns()) {
    if (usedExterns.count(ne))
      if (auto *inc = ne->include())
        includes.insert(inc);
  }
  for (auto *inc : includes) {
    OS << "#include <";
    OS.write_escaped(inc->c_str());
    OS << ">\n";
  }
  if (!includes.empty())
    OS << "\n";
}

/// Generate the external C declarations for native externs.
void generateExternC(
    Module *M,
    llvh::raw_ostream &OS,
    const llvh::DenseSet<NativeExtern *> &usedExterns) {
  size_t count = 0;
  for (NativeExtern *ne : M->getContext().getNativeContext().getAllExterns()) {
    if (usedExterns.count(ne) && !ne->declared() && !ne->include()) {
      ++count;
      if (count == 1)
        OS << '\n';
      ne->signature()->format(OS, ne->name()->c_str());
      OS << ";\n";
    }
  }
  if (count != 0)
    OS << '\n';
}

/// Converts Module \p M into valid C code and outputs it through \p OS.
/// Returns the cache size necessary to store all the cache indexes used.
void generateModule(
    Module *M,
    hermes::sh::LineDirectiveEmitter &OS,
    const BytecodeGenerationOptions &options) {
  if (!lowerModuleIR(M, options.optimizationEnabled)) {
    return;
  }

  if (options.verifyIR) {
    if (!verifyModule(*M, &llvh::errs(), VerificationMode::IR_LOWERED)) {
      M->getContext().getSourceErrorManager().error(
          SMLoc{}, "Lowered IR verification failed");
      return;
    }
  }

  if (options.format == DumpLIR) {
    M->dump();
    return;
  }

  // TODO: Share cache indices where the property name is the same and
  // -reuse-prop-cache is passed in.
  uint32_t nextWriteCacheIdx = 0;
  uint32_t nextReadCacheIdx = 0;
  uint32_t nextPrivateNameCacheIdx = 0;
  ModuleGen moduleGen{M, options.optimizationEnabled};

  if (options.format == DumpBytecode || options.format == EmitBundle) {
    if (!isValidSHUnitName(options.unitName))
      hermes_fatal("Invalid unit name passed to SH backend.");
    OS << R"(
#include "hermes/VM/static_h.h"

#include <stdlib.h>

)";

    auto usedExterns = collectUsedExterns(M);
    generateExternCIncludes(M, OS, *usedExterns);

    OS << R"(
static uint32_t unit_index;
static inline SHSymbolID* get_symbols(SHUnit *);
static inline SHWritePropertyCacheEntry* get_write_prop_cache(SHUnit *);
static inline SHReadPropertyCacheEntry* get_read_prop_cache(SHUnit *);
static inline SHPrivateNameCacheEntry* get_private_name_cache(SHUnit *);
static const SHSrcLoc s_source_locations[];
static SHNativeFuncInfo s_function_info_table[];
)";

    // Declare extern functions.
    generateExternC(M, OS, *usedExterns);
    // Free the used externs, we no longer need them.
    usedExterns.reset();

    // Forward declare every JS function.
    for (auto &F : *M) {
      OS << "static SHLegacyValue ";
      moduleGen.nativeFunctionTable.generateFunctionLabel(&F, OS);
      OS << "(SHRuntime *shr);\n";
    }
  }

  M->assignIndexToVariables();

  for (auto &F : *M) {
    generateFunction(
        F,
        OS,
        moduleGen,
        nextWriteCacheIdx,
        nextReadCacheIdx,
        nextPrivateNameCacheIdx,
        options);
  }

  if (options.format == DumpBytecode || options.format == EmitBundle) {
    moduleGen.literalBuffers.generate(OS);
    generateRegExpTable(OS, moduleGen.regExpTable);
    moduleGen.srcLocationTable.generate(
        OS, M->getContext().getSourceErrorManager());
    moduleGen.nativeFunctionTable.generate(OS);
    // String table should be generated last, because the generate calls to
    // other module components may add new entries to the string table.
    moduleGen.stringTable.generate(OS);

    // Note that we prefix the unit name with sh_export_ to avoid potential
    // conflicts.
    OS << "#define CREATE_THIS_UNIT sh_export_" << options.unitName << "\n";

    OS << "struct UnitData {\n"
       << "  SHUnit unit;\n"
       << "  SHSymbolID symbol_data[" << moduleGen.stringTable.size() << "];\n"
       << "  SHWritePropertyCacheEntry write_prop_cache_data["
       << nextWriteCacheIdx << "];\n"
       << "  SHReadPropertyCacheEntry read_prop_cache_data[" << nextReadCacheIdx
       << "];\n"
       << "  SHPrivateNameCacheEntry private_name_cache_data["
       << nextPrivateNameCacheIdx << "];\n"
       << "  SHCompressedPointer object_literal_class_cache["
       << moduleGen.literalBuffers.objShapeTable.size() << "];\n};\n"
       << "SHUnit *CREATE_THIS_UNIT(void) {\n"
       << "  struct UnitData *unit_data = calloc(sizeof(struct UnitData), 1);\n"
       << "  *unit_data = (struct UnitData){.unit = {.index = &unit_index,"
       << ".num_symbols =" << moduleGen.stringTable.size()
       << ", .num_write_prop_cache_entries = " << nextWriteCacheIdx
       << ", .num_read_prop_cache_entries = " << nextReadCacheIdx
       << ", .ascii_pool = s_ascii_pool, .u16_pool = s_u16_pool,"
       << ".strings = s_strings, .symbols = unit_data->symbol_data,"
       << ".write_prop_cache = unit_data->write_prop_cache_data,"
       << ".read_prop_cache = unit_data->read_prop_cache_data, "
       << ".private_name_cache = unit_data->private_name_cache_data, "
       << ".obj_key_buffer = s_obj_key_buffer, .obj_key_buffer_size = "
       << moduleGen.literalBuffers.objKeyBuffer.size() << ", "
       << ".literal_val_buffer = s_literal_val_buffer, .literal_val_buffer_size = "
       << moduleGen.literalBuffers.literalValueBuffer.size() << ", "
       << ".obj_shape_table = s_obj_shape_table, "
       << ".obj_shape_table_count = "
       << moduleGen.literalBuffers.objShapeTable.size() << ", "
       << ".object_literal_class_cache = unit_data->object_literal_class_cache, "
       << ".regexp_storage = s_regexp_storage, "
       << ".regexp_table = s_regexp_table, "
       << ".num_regexps = " << moduleGen.regExpTable.getEntryList().size()
       << ", "
       << ".source_locations = s_source_locations, "
       << ".source_locations_size = " << moduleGen.srcLocationTable.size()
       << ", " << ".unit_main = _0_global, "
       << ".unit_main_info = &s_function_info_table[0], "
       << ".unit_name = \"sh_compiled\" }};\n"
       << "  return (SHUnit *)unit_data;\n}\n"
       << R"(
SHSymbolID *get_symbols(SHUnit *unit) {
  return ((struct UnitData *)unit)->symbol_data;
}

SHWritePropertyCacheEntry *get_write_prop_cache(SHUnit *unit) {
  return ((struct UnitData *)unit)->write_prop_cache_data;
}
SHReadPropertyCacheEntry *get_read_prop_cache(SHUnit *unit) {
  return ((struct UnitData *)unit)->read_prop_cache_data;
}
SHPrivateNameCacheEntry *get_private_name_cache(SHUnit *unit) {
  return ((struct UnitData *)unit)->private_name_cache_data;
}
)";
    if (options.emitMain) {
      OS << R"(
typedef struct SHConsoleContext SHConsoleContext;

SHConsoleContext *init_console_bindings(
    SHRuntime *shr, int scriptArgc, const char *const *scriptArgv);

void release_console_bindings(SHConsoleContext *consoleContext);

void free_console_context(SHConsoleContext *consoleContext);

bool run_event_loop(
    SHRuntime *shr,
    SHConsoleContext *consoleContext);

int main(int argc, char **argv) {
  int scriptArgc = 0;
  char **scriptArgv = NULL;
  int clArgc = argc;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] == '\0') {
      scriptArgc = argc - i - 1;
      scriptArgv = argv + i + 1;
      clArgc = i;
      break;
    }
  }
  SHRuntime *shr = _sh_init(clArgc, argv);
  SHConsoleContext *consoleContext =
      init_console_bindings(shr, scriptArgc, (const char *const *)scriptArgv);
  bool success =
    _sh_initialize_units(shr, 1, CREATE_THIS_UNIT) &&
    run_event_loop(shr, consoleContext);
  release_console_bindings(consoleContext);
  _sh_done(shr);
  free_console_context(consoleContext);
  return success ? 0 : 1;
}
)";
    }
  }
}
} // namespace

/// Converts Module \p M into valid C code and outputs it through \p OS
void sh::generateSH(
    Module *M,
    llvh::raw_ostream &OS,
    const BytecodeGenerationOptions &options) {
  LineDirectiveEmitter emitter{OS};
  generateModule(M, emitter, options);
}
