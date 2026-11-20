/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSRegExp.h"

#include "hermes/Regex/Executor.h"
#include "hermes/Regex/Regex.h"
#include "hermes/Regex/RegexTraits.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/RegExpMatch.h"
#include "hermes/VM/Runtime-inline.h"
#include "hermes/VM/StringView.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
// class JSRegExp

const ObjectVTable JSRegExp::vt{
    VTable(
        CellKind::JSRegExpKind,
        cellSize<JSRegExp>(),
        /* allowLargeAlloc */ false,
        JSRegExp::_finalizeImpl,
        JSRegExp::_mallocSizeImpl,
        nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
        ,
        VTable::HeapSnapshotMetadata{
            HeapSnapshot::NodeType::Regexp,
            JSRegExp::_snapshotNameImpl,
            JSRegExp::_snapshotAddEdgesImpl,
            JSRegExp::_snapshotAddNodesImpl,
            nullptr}
#endif

        ),
    JSRegExp::_getOwnIndexedRangeImpl,
    JSRegExp::_haveOwnIndexedImpl,
    JSRegExp::_getOwnIndexedPropertyFlagsImpl,
    JSRegExp::_getOwnIndexedImpl,
    JSRegExp::_setOwnIndexedImpl,
    JSRegExp::_deleteOwnIndexedImpl,
    JSRegExp::_checkAllOwnIndexedImpl,
};

void JSRegExpBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSRegExp>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const JSRegExp *>(cell);
  mb.setVTable(&JSRegExp::vt);
  mb.addField("pattern", &self->pattern_);
  mb.addField("groupNameMappings", &self->groupNameMappings_);
}

PseudoHandle<JSRegExp> JSRegExp::create(Runtime &runtime) {
  auto *cell = runtime.makeAFixed<JSRegExp, HasFinalizer::Yes>(
      runtime, runtime.regExpPrototype, runtime.classJSRegExp);
  return JSObjectInit::initToPseudoHandle(runtime, cell);
}

PseudoHandle<JSRegExp> JSRegExp::create(
    Runtime &runtime,
    Handle<JSObject> parentHandle) {
  auto *cell = runtime.makeAFixed<JSRegExp, HasFinalizer::Yes>(
      runtime,
      parentHandle,
      runtime.getHiddenClassForPrototype(*parentHandle, runtime.classJSRegExp));
  return JSObjectInit::initToPseudoHandle(runtime, cell);
}

Handle<HiddenClass> JSRegExp::createMatchClass(
    Runtime &runtime,
    Handle<HiddenClass> arrayClass) {
  // Adds the property \p name to matchClass which, upon return, will point to
  // the newly created hidden class.
  auto addProperty = [&](Handle<HiddenClass> clazz, Predefined::Str name) {
    auto added = HiddenClass::addProperty(
        clazz,
        runtime,
        Predefined::getSymbolID(name),
        PropertyFlags::defaultNewNamedPropertyFlags());
    assert(
        added != ExecutionStatus::EXCEPTION &&
        "Adding the first properties shouldn't cause overflow");
    return added->first;
  };

  Handle<HiddenClass> addIndex = addProperty(arrayClass, Predefined::index);
  Handle<HiddenClass> addInput = addProperty(addIndex, Predefined::input);
  return addProperty(addInput, Predefined::groups);
}

void JSRegExp::initialize(
    Handle<JSRegExp> selfHandle,
    Runtime &runtime,
    Handle<StringPrimitive> pattern,
    Handle<StringPrimitive> flags,
    llvh::IntrusiveRefCntPtr<RegExpBytecode> bytecode) {
  assert(
      pattern && flags &&
      "Null pattern and/or flags passed to JSRegExp::initialize");
  selfHandle->pattern_.set(runtime, *pattern, runtime.getHeap());

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.enumerable = 0;
  dpf.configurable = 0;

  auto res = JSObject::defineOwnProperty(
      selfHandle,
      runtime,
      Predefined::getSymbolID(Predefined::lastIndex),
      dpf,
      HandleRootOwner::getZeroValue());
  (void)res;
  assert(
      res != ExecutionStatus::EXCEPTION && *res &&
      "defineOwnProperty() failed");

  selfHandle->setBytecode(std::move(bytecode));
}

ExecutionStatus JSRegExp::initialize(
    Handle<JSRegExp> selfHandle,
    Runtime &runtime,
    Handle<JSRegExp> otherHandle,
    Handle<StringPrimitive> flags) {
  struct : public Locals {
    PinnedValue<StringPrimitive> pattern;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  llvh::SmallVector<char16_t, 16> flagsText16;
  flags->appendUTF16String(flagsText16);

  auto sflags = regex::SyntaxFlags::fromString(flagsText16);
  if (!sflags) {
    return runtime.raiseSyntaxError("Invalid RegExp: Invalid flags");
  }

  lv.pattern = getPattern(otherHandle.get(), runtime);

  // Fast path to avoid recompiling the RegExp if the flags match
  if (LLVM_LIKELY(
          sflags->toByte() == getSyntaxFlags(otherHandle.get()).toByte())) {
    // A Cached entry is shared. An Exclusive entry has exactly one holder,
    // which reports its memory, so it is copied.
    const RegExpBytecode *other = otherHandle->bytecode_.get();
    assert(other && "fast copy from an uninitialized RegExp");
    llvh::IntrusiveRefCntPtr<RegExpBytecode> bytecode =
        other->ownership() == RegExpBytecode::Ownership::Cached
        ? otherHandle->bytecode_
        : RegExpBytecode::create(
              other->bytecode(), RegExpBytecode::Ownership::Exclusive);
    initialize(selfHandle, runtime, lv.pattern, flags, std::move(bytecode));
    // The group name mapping depends only on the pattern, and it is never
    // modified after initialization, so share it rather than rebuilding it.
    selfHandle->groupNameMappings_.set(
        runtime, otherHandle->groupNameMappings_, runtime.getHeap());
    return ExecutionStatus::RETURNED;
  }
  return initialize(selfHandle, runtime, lv.pattern, flags);
}

/// ES11 21.2.3.2.2 RegExpInitialize ( obj, pattern, flags )
ExecutionStatus JSRegExp::initialize(
    Handle<JSRegExp> selfHandle,
    Runtime &runtime,
    Handle<StringPrimitive> pattern,
    Handle<StringPrimitive> flags) {
  assert(
      pattern && flags &&
      "Null pattern and/or flags passed to JSRegExp::initialize");
  llvh::SmallVector<char16_t, 6> flagsText16;
  flags->appendUTF16String(flagsText16);

  llvh::SmallVector<char16_t, 16> patternText16;
  pattern->appendUTF16String(patternText16);

  // Build the regex.
  regex::Regex<regex::UTF16RegexTraits> regex(patternText16, flagsText16);

  if (!regex.valid()) {
    return runtime.raiseSyntaxError(
        TwineChar16("Invalid RegExp: ") +
        regex::constants::messageForError(regex.getError()));
  }
  // The regex is valid. Compile and store its bytecode.
  auto bytecode = regex.compile();
  // Also store the name mappings.
  if (LLVM_UNLIKELY(
          initializeGroupNameMappingObj(
              runtime,
              selfHandle,
              regex.getOrderedNamedGroups(),
              regex.getGroupNamesMapping()) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  initialize(
      selfHandle,
      runtime,
      pattern,
      flags,
      RegExpBytecode::create(bytecode, RegExpBytecode::Ownership::Exclusive));
  return ExecutionStatus::RETURNED;
}

ExecutionStatus JSRegExp::initializeGroupNameMappingObj(
    Runtime &runtime,
    Handle<JSRegExp> selfHandle,
    std::deque<llvh::SmallVector<char16_t, 5>> &orderedNamedGroups,
    regex::ParsedGroupNamesMapping &parsedMappings) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  GCScope gcScope(runtime);
  if (parsedMappings.size() == 0)
    return ExecutionStatus::RETURNED;

  auto objRes = JSObject::create(
      runtime,
      Runtime::makeNullHandle<JSObject>(),
      runtime.getHiddenClassForPrototype(nullptr, runtime.classJSObject),
      parsedMappings.size());
  lv.obj = objRes.get();

  for (const auto &identifier : orderedNamedGroups) {
    GCScopeMarkerRAII marker{gcScope};

    auto mappingIt = parsedMappings.find(identifier);
    assert(
        mappingIt != parsedMappings.end() &&
        "identifier not found in named groups");
    const regex::GroupNums &matchIndices = mappingIt->second;
    if (LLVM_LIKELY(matchIndices.size() == 1)) {
      lv.value = HermesValue::encodeTrustedNumberValue(matchIndices.front());
    } else {
      // Several mutually exclusive capture groups share this name, so it maps
      // to an array of their match array indices rather than to a single one.
      uint32_t count = matchIndices.size();
      auto arrRes = JSArray::create(runtime, count, count);
      if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      lv.value = arrRes->getHermesValue();
      for (uint32_t i = 0; i < count; ++i) {
        // Encoding a number may allocate, so do it before touching the array.
        SmallHermesValue shv =
            SmallHermesValue::encodeNumberValue(matchIndices[i], runtime);
        JSArray::unsafeSetExistingElementAt(
            vmcast<JSArray>(*lv.value), runtime, i, shv);
      }
    }

    auto symbolRes =
        runtime.getIdentifierTable().getSymbolHandle(runtime, identifier);
    if (LLVM_UNLIKELY(symbolRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto res = JSObject::defineNewOwnProperty(
        lv.obj,
        runtime,
        symbolRes->get(),
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.value);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  selfHandle->groupNameMappings_.set(runtime, lv.obj.get(), runtime.getHeap());
  return ExecutionStatus::RETURNED;
}

Handle<JSObject> JSRegExp::getGroupNameMappings(Runtime &runtime) {
  // TODO: This function returns Handle and needs refactoring to avoid
  // makeHandle
  if (auto *ptr = vmcast_or_null<JSObject>(groupNameMappings_.get(runtime)))
    return runtime.makeHandle(ptr);
  return Runtime::makeNullHandle<JSObject>();
}

void JSRegExp::setGroupNameMappings(Runtime &runtime, JSObject *groupObj) {
  groupNameMappings_.set(runtime, groupObj, runtime.getHeap());
}

void JSRegExp::setBytecode(llvh::IntrusiveRefCntPtr<RegExpBytecode> bytecode) {
  assert(!bytecode_ && "a JSRegExp's bytecode is set only once");
  syntaxFlags_ = regex::SyntaxFlags::fromByte(bytecode->header().syntaxFlags);
  bytecode_ = std::move(bytecode);
}

const RegExpBytecode *JSRegExp::exclusiveBytecode() const {
  const RegExpBytecode *bytecode = bytecode_.get();
  return bytecode &&
          bytecode->ownership() == RegExpBytecode::Ownership::Exclusive
      ? bytecode
      : nullptr;
}

PseudoHandle<StringPrimitive> JSRegExp::getPattern(
    JSRegExp *self,
    PointerBase &base) {
  return createPseudoHandle(self->pattern_.get(base));
}

template <typename CharT, typename Traits>
CallResult<RegExpMatch> performSearch(
    Runtime &runtime,
    llvh::ArrayRef<uint8_t> bytecode,
    const CharT *start,
    uint32_t stringLength,
    uint32_t searchStartOffset,
    regex::constants::MatchFlagType matchFlags) {
  std::vector<regex::CapturedRange> &nativeMatchRanges =
      runtime.regExpSearchCaptures;
  auto matchResult = regex::searchWithBytecode(
      bytecode,
      start,
      searchStartOffset,
      stringLength,
      &nativeMatchRanges,
      matchFlags,
      runtime.getOverflowGuardForRegex());
  if (matchResult == regex::MatchRuntimeResult::StackOverflow) {
    return runtime.raiseRangeError("Maximum regex stack depth reached");
  } else if (matchResult == regex::MatchRuntimeResult::NoMatch) {
    return RegExpMatch{}; // not found.
  }
  size_t matchRangeCount = nativeMatchRanges.size();
  assert(matchRangeCount > 0);
  RegExpMatch match;
  match.reserve(matchRangeCount);
  for (size_t i = 0; i < matchRangeCount; i++) {
    const auto &submatch = nativeMatchRanges[i];
    if (!submatch.matched()) {
      assert(i > 0 && "match_result[0] should always match");
      match.push_back(llvh::None);
    } else {
      uint32_t pos = submatch.start;
      uint32_t length = submatch.end - submatch.start;
      match.push_back(RegExpMatchRange{pos, length});
    }
  }
  assert(!match.empty() && "Unexpected empty match");
  return match;
}

CallResult<RegExpMatch> JSRegExp::search(
    Handle<JSRegExp> selfHandle,
    Runtime &runtime,
    Handle<StringPrimitive> strHandle,
    uint32_t searchStartOffset) {
  assert(selfHandle->bytecode_ && "Missing bytecode");
  auto input = StringPrimitive::createStringView(runtime, strHandle);

  // Note we may still have a match if searchStartOffset == str.size(),
  // if the regexp can match an empty string
  if (searchStartOffset > input.length()) {
    return RegExpMatch{}; // no match possible
  }

  auto matchFlags = regex::constants::matchDefault;

  // Respect the sticky flag, which forces us to match only at the given
  // location.
  if (selfHandle->syntaxFlags_.sticky) {
    matchFlags |= regex::constants::matchOnlyAtStart;
  }

  CallResult<RegExpMatch> matchResult = RegExpMatch{};
  if (input.isASCII()) {
    matchFlags |= regex::constants::matchInputAllAscii;
    matchResult = performSearch<char, regex::ASCIIRegexTraits>(
        runtime,
        selfHandle->getBytecode(),
        input.castToCharPtr(),
        input.length(),
        searchStartOffset,
        matchFlags);
  } else {
    matchResult = performSearch<char16_t, regex::UTF16RegexTraits>(
        runtime,
        selfHandle->getBytecode(),
        input.castToChar16Ptr(),
        input.length(),
        searchStartOffset,
        matchFlags);
  }

  // Only update on successful match.
  if (LLVM_UNLIKELY(matchResult == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  } else if (!matchResult->empty()) {
    runtime.regExpLastInput = strHandle;
    runtime.regExpLastRegExp = selfHandle;
    runtime.regExpLastMatch = *matchResult;
  }
  return matchResult;
}

JSRegExp::~JSRegExp() = default;

void JSRegExp::_finalizeImpl(GCCell *cell, GC &gc) {
  JSRegExp *self = vmcast<JSRegExp>(cell);
  // A Cached entry's native ID belongs to its cache.
  if (const RegExpBytecode *bytecode = self->exclusiveBytecode())
    gc.getIDTracker().untrackNative(bytecode);
  self->~JSRegExp();
}

size_t JSRegExp::_mallocSizeImpl(GCCell *cell) {
  // A Cached entry is reported by its cache.
  const RegExpBytecode *bytecode = vmcast<JSRegExp>(cell)->exclusiveBytecode();
  return bytecode ? bytecode->mallocSize() : 0;
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
std::string JSRegExp::_snapshotNameImpl(GCCell *cell, GC &gc) {
  auto *const self = vmcast<JSRegExp>(cell);
  return converter(getPattern(self, gc.getPointerBase()).get());
}

void JSRegExp::_snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap) {
  auto *const self = vmcast<JSRegExp>(cell);
  // Call the super type to add any other custom edges.
  JSObject::_snapshotAddEdgesImpl(self, gc, snap);
  if (const RegExpBytecode *bytecode = self->exclusiveBytecode()) {
    snap.addNamedEdge(
        HeapSnapshot::EdgeType::Internal, "bytecode", gc.getNativeID(bytecode));
  }
}

void JSRegExp::_snapshotAddNodesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap) {
  auto *const self = vmcast<JSRegExp>(cell);
  if (const RegExpBytecode *bytecode = self->exclusiveBytecode()) {
    // Add a native node for regex bytecode, to account for native size
    // directly owned by the regex.
    snap.beginNode();
    snap.endNode(
        HeapSnapshot::NodeType::Native,
        "RegExpBytecode",
        gc.getNativeID(bytecode),
        bytecode->mallocSize(),
        0);
  }
}
#endif

/// \return an escaped string equivalent to \p pattern.
/// This is used to construct the 'source' property of RegExp. This requires
/// us to return a string from which the regexp may be reconstructed as if
/// from a /foo/ style literal. Note this is different from the RegExp
/// constructor that takes a string, e.g. new RegExp("/") returns a regexp
/// that matches /, but
/// /// does not (it's a comment!). So we may have to perform surgery on the
/// pattern.
CallResult<HermesValue> JSRegExp::escapePattern(
    Handle<StringPrimitive> pattern,
    Runtime &runtime) {
  SmallU16String<32> result;
  result.reserve(pattern->getStringLength());
  auto patternView = StringPrimitive::createStringView(runtime, pattern);
  bool isBackslashed = false;
  for (char16_t c : patternView) {
    switch (c) {
      case u'/':
        // Avoid premature end of regex.
        // TODO nice to have: don't do this if we are in square brackets.
        // /[/]/ is valid and the middle / does not need to be escaped.
        // However /[\/]/ is also valid and means the same thing
        // (CharacterEscape production from regexp grammar). Still it would be
        // nice to not unnecessarily mangle the user's supplied pattern.
        result.append(isBackslashed ? "/" : "\\/");
        break;

        // Escape line terminators. See ES5.1 7.3.
      case u'\n':
        result.append(isBackslashed ? "n" : "\\n");
        break;

      case u'\r':
        result.append(isBackslashed ? "r" : "\\r");
        break;

      case 0x2028:
        result.append(isBackslashed ? "u2028" : "\\u2028");
        break;

      case 0x2029:
        result.append(isBackslashed ? "u2029" : "\\u2029");
        break;

      default:
        result.append(c);
        break;
    }
    isBackslashed = (c == u'\\') && !isBackslashed;
  }
  // "If P is the empty String, this specification can be met by letting S be
  // '(?:)'."
  if (result.empty()) {
    result = u"(?:)";
  }

  // Avoid unnecessary allocation in the likely event the source and pattern
  // match.
  if (patternView.equals(result.arrayRef())) {
    return pattern.getHermesValue();
  }
  return StringPrimitive::create(runtime, result);
}

} // namespace vm
} // namespace hermes
