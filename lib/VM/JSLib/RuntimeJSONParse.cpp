/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSLib/RuntimeJSONParse.h"

#include "Object.h"

#include "hermes/Support/UTF16Stream.h"
#include "hermes/VM/ArrayLike.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/JSArray.h"

#include "JSONLexer.h"

#include "llvh/Support/SaveAndRestore.h"

#include <variant>

namespace hermes {
namespace vm {

namespace {

static constexpr int32_t MAX_JSON_RECURSION_DEPTH =
#ifndef HERMES_LIMIT_STACK_DEPTH
    512
#else
    100
#endif
    ;

/// This class wraps the functionality required to parse a JSON string into
/// a VM runtime value. It supports ASCII/UTF16/UTF8 input, and returns a
/// HermesValue when parse is called.
template <EncodingKind Kind>
class RuntimeJSONParser {
 public:
 private:
  using Traits = EncodingTraits<Kind>;
  using CharT = typename Traits::CharT;
  /// The VM runtime.
  Runtime &runtime_;

  /// The lexer.
  JSONLexer<Kind> lexer_;

  /// Stores the optional reviver parameter.
  /// https://es5.github.io/#x15.12.2
  Handle<Callable> reviver_;

  /// How many more nesting levels we allow before error.
  /// Decremented every time a nested level is started,
  /// and incremented again when leaving the nest.
  /// If it drops below 0 while parsing, raise a stack overflow.
  int32_t remainingDepth_{MAX_JSON_RECURSION_DEPTH};

 public:
  template <typename U = Traits, typename = std::enable_if_t<U::UsesRawPtr>>
  explicit RuntimeJSONParser(
      Runtime &runtime,
      llvh::ArrayRef<CharT> str,
      Handle<Callable> reviver)
      : runtime_(runtime), lexer_(runtime, str), reviver_(reviver) {}

  template <typename U = Traits, typename = std::enable_if_t<!U::UsesRawPtr>>
  explicit RuntimeJSONParser(Runtime &runtime, UTF16Stream &&jsonString)
      : runtime_(runtime),
        lexer_(runtime, std::move(jsonString)),
        reviver_(Runtime::makeNullHandle<Callable>()) {}

  /// Parse JSON string through lexer_, create objects using runtime_.
  /// If errors occur, this function will return undefined, and the error
  /// should be kept inside SourceErrorManager.
  CallResult<HermesValue> parse();

 private:
  /// Parse the top-level JSON value. When this function is finished, the lexer
  /// should be at the end of the file.
  CallResult<HermesValue> parseValue();

  /// Use reviver to filter the result.
  CallResult<HermesValue> revive(Handle<> value);

  /// The abstract operation Walk is a recursive abstract operation that takes
  /// two parameters: a holder object and the String name of a property in
  CallResult<HermesValue> operationWalk(
      Handle<JSObject> holder,
      Handle<> property);

  /// Given value and key, recursively call operationWalk to generate a new
  /// filtered value. It's a helper function for operationWalk, and does not
  /// need to return a value
  ExecutionStatus filter(Handle<JSObject> val, Handle<> key);
};

} // namespace

template <EncodingKind Kind>
CallResult<HermesValue> RuntimeJSONParser<Kind>::parse() {
  struct : public Locals {
    PinnedValue<> parResult;
  } lv;
  LocalsRAII lraii(runtime_, &lv);

  // parseValue() requires one token to start with.
  if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto parRes = parseValue();
  if (LLVM_UNLIKELY(parRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Make sure the next token must be EOF.
  if (LLVM_UNLIKELY(lexer_.getCurToken()->getKind() != JSONTokenKind::Eof)) {
    return lexer_.errorUnexpectedChar();
  }

  if (reviver_.get()) {
    lv.parResult = *parRes;
    if ((parRes = revive(lv.parResult)) == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
  }
  return parRes;
}

/// Check if the hidden class found in \p existingEntry matches the SymbolIDs
/// found in \p properties, starting from \p beginIdx.
/// \param existingCacheEntry is the entry in the hidden class cache to compare.
///   It might not be a HiddenClass if this cache entry hasn't been written to
///   yet.
/// \param properties ArrayStorage which must contain only SymbolIDs.
/// \param beginIdx starting place in \p properties to begin comparison.
/// \param sz the amount of SymbolIDs in \p properties to check.
/// \param cacheEntry[out] will be set if \p existingEntry was a HiddenClass,
///   regardless of if there was a match or not.
/// \return true if \p existingCacheEntry is a HiddenClass whose properties all
/// match exactly to the relevant contents of \p properties.
static inline bool matchesHiddenClass(
    Runtime &runtime,
    HermesValue existingCacheEntry,
    Handle<ArrayStorage> properties,
    size_t beginIdx,
    size_t sz,
    PinnedValue<HiddenClass> &cacheEntry) {
  if (!vmisa<HiddenClass>(existingCacheEntry)) {
    return false;
  }
  cacheEntry.template castAndSetHermesValue<HiddenClass>(existingCacheEntry);
  if (sz != cacheEntry->getNumProperties()) {
    return false;
  }
  return HiddenClass::forEachPropertyWhile(
      cacheEntry,
      runtime,
      [properties, &beginIdx](
          Runtime &, SymbolID expected, NamedPropertyDescriptor desc) {
        SymbolID actual = properties->at(beginIdx++).getSymbol();
        return actual == expected;
      });
}

template <EncodingKind Kind>
CallResult<HermesValue> RuntimeJSONParser<Kind>::parseValue() {
  struct : public Locals {
    /// List of property SymbolIDs for all unclosed objects being parsed.
    PinnedValue<ArrayStorage> properties;
    /// List of property values for all unclosed objects/arrays being parsed.
    PinnedValue<ArrayStorageSmall> values;
    /// The most recently finished parsed value.
    PinnedValue<> curVal;
    /// Used to be able to give a handle to defineOwnComputedPrimitive when
    /// making objects.
    PinnedValue<SymbolID> key;
    /// Used to hold recently created objects/arrays.
    PinnedValue<JSObject> newObject;
    /// Cache of hidden classes, keyed by depth. The cache is grown every time
    /// an object at a new depth is parsed. The depth is defined as the sum of
    /// all open objects and arrays. Because of this, it's possible for the
    /// cache to contain invalid entries of the value empty.
    PinnedValue<ArrayStorage> hcCache;
    /// Current cache entry being tested.
    PinnedValue<HiddenClass> cacheEntry;
  } lv;
  LocalsRAII lraii(runtime_, &lv);

  auto propsRes = ArrayStorage::create(runtime_, 4, 0);
  if (LLVM_UNLIKELY(propsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.properties = vmcast<ArrayStorage>(*propsRes);

  auto valuesRes = ArrayStorageSmall::create(runtime_, 4, 0);
  if (LLVM_UNLIKELY(valuesRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.values = vmcast<ArrayStorageSmall>(*valuesRes);

  auto cacheRes = ArrayStorage::create(runtime_, 4, 0);
  if (LLVM_UNLIKELY(cacheRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // The hidden class cache.
  lv.hcCache = vmcast<ArrayStorage>(*cacheRes);

  // These different contexts here refer to the unfinished elements that are
  // being parsed. Each context is pushed onto a stack when beginning to parse
  // an element of that type.

  /// The RootCtx indicates the beginning of the parse. If a value is finished
  /// parsing and the current context is the RootCtx, then that means this
  /// parsed value *is* the top-level result.
  struct RootCtx {};
  /// When an array is first encountered, record the starting index into the
  /// pending values ArrayStorage. This is so that when the array is finished
  /// parsing, we know how large it is and what elements in the values
  /// ArrayStorage pertain to this parsed array.
  struct ArrayCtx {
    size_t valuesIdx;
  };
  /// When an object is first encountered, record the starting index into the
  /// pending values ArrayStorage. This is so that when the object is finished
  /// parsing, we know how large it is and what elements in the values
  /// ArrayStorage and properties ArrayStorage pertain to this parsed object.
  struct ObjectCtx {
    size_t valuesIdx;
  };
  using CtxTy = std::variant<RootCtx, ArrayCtx, ObjectCtx>;
  // This holds the list of all started but not yet finished arrays/objects.
  std::vector<CtxTy> contexts;
  // The current context is initialized to root, which signals the top-level
  // JSON value.
  CtxTy curContext = RootCtx{};
  auto popContext = [&contexts]() -> CtxTy {
    auto res = contexts.back();
    contexts.pop_back();
    return res;
  };
  auto pushContext = [&contexts](CtxTy ctx) { contexts.push_back(ctx); };

  GCScope gcScope{runtime_};
  auto marker = gcScope.createMarker();
  // This outer loop iterates after a value has been added to an object/array,
  // and we are starting to parsing the next element of it. Take the following
  // JSON value: [1, 2, 3]. We produce the value of 1 from the first inner loop,
  // we consume that 1 in the second inner loop (along with the comma) then we
  // come back around on the outer loop to get to the first inner loop again to
  // produce the 2.
  while (true) {
    gcScope.flushToMarker(marker);
    // 'break' will jump to the bottom of the loop which will consume the
    // produced values; 'continue' will restart the loop.
    while (true) {
      // Keep iterating until we reach a simple 'terminal value'. A terminal
      // value is a string/number/true/false/null/empty object/empty array.
      // Note the string of an object property does not count towards this
      // definition of terminal value.
      switch (lexer_.getCurToken()->getKind()) {
        case JSONTokenKind::String:
          lv.curVal = lexer_.getCurToken()->getStrAsPrim().getHermesValue();
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          break;
        case JSONTokenKind::Number:
          lv.curVal = HermesValue::encodeTrustedNumberValue(
              lexer_.getCurToken()->getNumber());
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          break;
        case JSONTokenKind::True:
          lv.curVal = HermesValue::encodeBoolValue(true);
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          break;
        case JSONTokenKind::False:
          lv.curVal = HermesValue::encodeBoolValue(false);
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          break;
        case JSONTokenKind::Null:
          lv.curVal = HermesValue::encodeNullValue();
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          break;
        case JSONTokenKind::LBrace: {
          if (LLVM_UNLIKELY(
                  lexer_.advanceStrAsSymbol() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          if (lexer_.getCurToken()->getKind() == JSONTokenKind::RBrace) {
            // we have an empty object.
            lv.curVal = JSObject::create(runtime_).getHermesValue();
            if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
              return ExecutionStatus::EXCEPTION;
            }
            break;
          }
          if (lexer_.getCurToken()->getKind() != JSONTokenKind::String) {
            return lexer_.error("Expect a string key in JSON object");
          }
          pushContext(curContext);
          curContext = ObjectCtx{lv.values->size()};
          if (LLVM_UNLIKELY(
                  ArrayStorage::push_back(
                      lv.properties,
                      runtime_,
                      lexer_.getCurToken()->getStrAsSymbol()) ==
                  ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          };
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          if (LLVM_UNLIKELY(
                  lexer_.getCurToken()->getKind() != JSONTokenKind::Colon)) {
            return lexer_.error("Expect ':' after the key in JSON object");
          }
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          continue;
        }
        case JSONTokenKind::LSquare: {
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          if (lexer_.getCurToken()->getKind() == JSONTokenKind::RSquare) {
            // parsed an empty array
            auto arrRes = JSArray::create(runtime_, 0, 0);
            if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
              return ExecutionStatus::EXCEPTION;
            }
            lv.curVal = arrRes->getHermesValue();
            if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
              return ExecutionStatus::EXCEPTION;
            }
            break;
          }
          pushContext(curContext);
          curContext = ArrayCtx{lv.values->size()};
          continue;
        }
        case JSONTokenKind::Eof:
          return lexer_.error("Unexpected end of input");
        default:
          // Closing arrays or objects is not handled this loop (unless they are
          // empty). ] and } should always be parsed and handled in the bottom
          // loop responsible for building objects and arrays.
          return lexer_.errorUnexpectedChar();
      }
      break;
    }

    // We will keep iterating in this loop as long as we can close pending
    // arrays or objects. If we see that there are more elements or properties
    // left to parse, we break out of this loop, and continue in the top loop.
    while (true) {
      gcScope.flushToMarker(marker);
      if (std::holds_alternative<RootCtx>(curContext)) {
        return lv.curVal.getHermesValue();
      } else if (auto *objCtx = std::get_if<ObjectCtx>(&curContext)) {
        if (LLVM_UNLIKELY(
                ArrayStorageSmall::push_back(lv.values, runtime_, lv.curVal) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        if (LLVM_LIKELY(
                lexer_.getCurToken()->getKind() == JSONTokenKind::Comma)) {
          // A comma means there is another property for this object. Let's
          // parse the property and leave the parser ready to continue
          // parsing the property value.
          if (LLVM_UNLIKELY(
                  lexer_.advanceStrAsSymbol() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          if (LLVM_UNLIKELY(
                  lexer_.getCurToken()->getKind() != JSONTokenKind::String)) {
            return lexer_.error("Expect a string key in JSON object");
          }
          if (LLVM_UNLIKELY(
                  ArrayStorage::push_back(
                      lv.properties,
                      runtime_,
                      lexer_.getCurToken()->getStrAsSymbol()) ==
                  ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          if (LLVM_UNLIKELY(
                  lexer_.getCurToken()->getKind() != JSONTokenKind::Colon)) {
            return lexer_.error("Expect ':' after the key in JSON object");
          }
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          break;
        }
        // If there isn't a comma for another property, then we must be at the
        // end of the object.
        if (LLVM_UNLIKELY(
                lexer_.getCurToken()->getKind() != JSONTokenKind::RBrace)) {
          return lexer_.errorUnexpectedChar();
        }

        size_t beginValIdx = objCtx->valuesIdx;
        size_t numElements = lv.values->size() - beginValIdx;
        size_t beginPropIdx = lv.properties->size() - numElements;
        auto depth = contexts.size();
        if (lv.hcCache->size() > depth &&
            matchesHiddenClass(
                runtime_,
                lv.hcCache->at(depth),
                lv.properties,
                beginPropIdx,
                numElements,
                lv.cacheEntry)) {
          // Fast path, we know the end HiddenClass and it's been written to
          // lv.cacheEntry.
          lv.newObject = JSObject::create(
              runtime_,
              Handle<JSObject>::vmcast(&runtime_.objectPrototype),
              lv.cacheEntry);
          for (size_t i = 0; i < numElements; i++) {
            size_t valIdx = beginValIdx + i;
            auto shv = lv.values->at(valIdx);
            JSObject::setNamedSlotValueUnsafe(*lv.newObject, runtime_, i, shv);
          }
        } else {
          // Slowpath
          lv.newObject = JSObject::create(
              runtime_,
              runtime_.objectPrototype,
              runtime_.classJSObject,
              numElements);
          for (size_t i = 0; i < numElements; i++) {
            gcScope.flushToMarker(marker);
            size_t propIdx = beginPropIdx + i;
            size_t valIdx = beginValIdx + i;
            lv.key = lv.properties->at(propIdx).getSymbol();
            lv.curVal = lv.values->at(valIdx).unboxToHV(runtime_);
            if (LLVM_UNLIKELY(
                    JSObject::defineOwnComputedPrimitive(
                        lv.newObject,
                        runtime_,
                        lv.key,
                        DefinePropertyFlags::getDefaultNewPropertyFlags(),
                        lv.curVal) == ExecutionStatus::EXCEPTION))
              return ExecutionStatus::EXCEPTION;
          }
          // Populate cache entry. Dictionary-mode HiddenClasses are owned by
          // a single object and mutated in place, so caching one would let a
          // mutation on a sibling object grow the shared class past this
          // object's propStorage (out-of-bounds access).
          if (LLVM_LIKELY(!lv.newObject->getClass(runtime_)->isDictionary())) {
            if (lv.hcCache->size() < depth + 1) {
              if (LLVM_UNLIKELY(
                      ArrayStorage::resize(lv.hcCache, runtime_, depth + 1) ==
                      ExecutionStatus::EXCEPTION)) {
                return ExecutionStatus::EXCEPTION;
              }
            }
            lv.hcCache->set(
                depth,
                HermesValue::encodeObjectValue(
                    lv.newObject->getClass(runtime_), runtime_),
                runtime_.getHeap());
          }
        }

        ArrayStorageSmall::resizeWithinCapacity(
            *lv.values, runtime_.getHeap(), beginValIdx);
        ArrayStorage::resizeWithinCapacity(
            *lv.properties, runtime_.getHeap(), beginPropIdx);

        lv.curVal = lv.newObject.getHermesValue();
        curContext = popContext();
        if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        continue;
      } else if (auto *arrCtx = std::get_if<ArrayCtx>(&curContext)) {
        if (LLVM_UNLIKELY(
                ArrayStorageSmall::push_back(lv.values, runtime_, lv.curVal) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        if (LLVM_LIKELY(
                lexer_.getCurToken()->getKind() == JSONTokenKind::Comma)) {
          if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          break;
        }
        if (LLVM_UNLIKELY(
                lexer_.getCurToken()->getKind() != JSONTokenKind::RSquare)) {
          return lexer_.errorUnexpectedChar();
        }
        size_t beginIdx = arrCtx->valuesIdx;
        size_t numElements = lv.values->size() - beginIdx;

        auto arrRes = JSArray::create(runtime_, numElements, numElements);
        if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.newObject = vmcast<JSObject>(arrRes->getHermesValue());
        if (LLVM_UNLIKELY(
                JSArray::setStorageEndIndex(
                    Handle<JSArray>::vmcast(&lv.newObject),
                    runtime_,
                    numElements) == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        };
        // Transfer the elements from the temporary storage in lv.values, to the
        // indexed storage of the JSArray just created.
        ArrayStorageSmall *srcStorage = *lv.values;
        ArrayStorageSmall *destStorage =
            vmcast<JSArray>(*lv.newObject)->getIndexedStorageUnsafe(runtime_);
        GCSmallHermesValueInLargeObj::uninitialized_copy(
            srcStorage->data() + beginIdx,
            srcStorage->data() + beginIdx + numElements,
            destStorage->data(),
            destStorage,
            runtime_.getHeap());
        ArrayStorageSmall::resizeWithinCapacity(
            *lv.values, runtime_.getHeap(), beginIdx);
        lv.curVal = lv.newObject.getHermesValue();
        curContext = popContext();
        if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        continue;
      }
      assert(false && "control flow should be handled inside of cases above");
    }
  }

  // Advance one more time to reach what should be the end of the file.
  if (LLVM_UNLIKELY(lexer_.advance() == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

template <EncodingKind Kind>
CallResult<HermesValue> RuntimeJSONParser<Kind>::revive(Handle<> value) {
  struct : public Locals {
    PinnedValue<JSObject> root;
  } lv;
  LocalsRAII lraii(runtime_, &lv);

  lv.root.template castAndSetHermesValue<JSObject>(
      JSObject::create(runtime_).getHermesValue());
  auto status = JSObject::defineOwnProperty(
      lv.root,
      runtime_,
      Predefined::getSymbolID(Predefined::emptyString),
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      value);
  (void)status;
  assert(
      status != ExecutionStatus::EXCEPTION && *status &&
      "defineOwnProperty on new object cannot fail");
  return operationWalk(
      lv.root, runtime_.getPredefinedStringHandle(Predefined::emptyString));
}

template <EncodingKind Kind>
CallResult<HermesValue> RuntimeJSONParser<Kind>::operationWalk(
    Handle<JSObject> holder,
    Handle<> property) {
  // The operation is recursive so it needs a GCScope.
  GCScope gcScope(runtime_);

  llvh::SaveAndRestore<decltype(remainingDepth_)> oldDepth{
      remainingDepth_, remainingDepth_ - 1};
  if (remainingDepth_ <= 0) {
    return runtime_.raiseStackOverflow(Runtime::StackOverflowKind::JSONParser);
  }
  // The reviver is only called after the children have been walked, so check
  // the native stack here, at every level of the walk.
  if (LLVM_UNLIKELY(runtime_.isStackOverflowing())) {
    return runtime_.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }

  struct : public Locals {
    PinnedValue<> tmpHandle;
    PinnedValue<> valHandle;
  } lv;
  LocalsRAII lraii(runtime_, &lv);

  auto propRes = JSObject::getComputed_RJS(holder, runtime_, property);
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CallResult<bool> isArrayRes =
      isArray(runtime_, dyn_vmcast<JSObject>(propRes->get()));
  if (LLVM_UNLIKELY(isArrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.valHandle = std::move(*propRes);
  if (*isArrayRes) {
    Handle<JSObject> objHandle = Handle<JSObject>::vmcast(&lv.valHandle);
    CallResult<uint64_t> lenRes = getArrayLikeLength_RJS(objHandle, runtime_);
    if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    GCScopeMarkerRAII marker(runtime_);
    for (uint64_t index = 0, e = *lenRes; index < e; ++index) {
      lv.tmpHandle = HermesValue::encodeTrustedNumberValue(index);
      // Note that deleting elements doesn't affect array length.
      if (LLVM_UNLIKELY(
              filter(objHandle, lv.tmpHandle) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      marker.flush();
    }
  } else if (vmisa<JSObject>(lv.valHandle.getHermesValue())) {
    auto scopedObject = Handle<JSObject>::vmcast(&lv.valHandle);
    auto cr = JSObject::getOwnPropertyNames(scopedObject, runtime_, true);
    if (cr == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    auto keys = *cr;
    GCScopeMarkerRAII marker(runtime_);
    for (uint32_t index = 0, e = keys->getEndIndex(); index < e; ++index) {
      lv.tmpHandle = keys->at(runtime_, index).unboxToHV(runtime_);
      if (LLVM_UNLIKELY(
              filter(scopedObject, lv.tmpHandle) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      marker.flush();
    }
  }
  // We have delayed converting the property to a string if it was index.
  // Now we have to do it because we are passing it to the reviver.
  lv.tmpHandle = *property;
  auto strRes = toString_RJS(runtime_, lv.tmpHandle);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.tmpHandle = strRes->getHermesValue();

  return Callable::executeCall2(
             reviver_,
             runtime_,
             holder,
             lv.tmpHandle.getHermesValue(),
             lv.valHandle.getHermesValue())
      .toCallResultHermesValue();
}

template <EncodingKind Kind>
ExecutionStatus RuntimeJSONParser<Kind>::filter(
    Handle<JSObject> val,
    Handle<> key) {
  struct : public Locals {
    PinnedValue<> newElement;
  } lv;
  LocalsRAII lraii(runtime_, &lv);

  auto jsonRes = operationWalk(val, key);
  if (LLVM_UNLIKELY(jsonRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.newElement = *jsonRes;
  if (lv.newElement->isUndefined()) {
    if (LLVM_UNLIKELY(
            JSObject::deleteComputed(val, runtime_, key) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    if (LLVM_UNLIKELY(
            JSObject::defineOwnComputed(
                val,
                runtime_,
                key,
                DefinePropertyFlags::getDefaultNewPropertyFlags(),
                lv.newElement) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

CallResult<HermesValue> runtimeJSONParse(
    Runtime &runtime,
    Handle<StringPrimitive> jsonString,
    Handle<Callable> reviver) {
  // Our parser requires strings that do not move during GCs, so
  // in most cases we'll need to copy, except for external strings.
  if (jsonString->isASCII()) {
    ASCIIRef ref;
    llvh::SmallVector<char, 32> storage;
    if (LLVM_UNLIKELY(jsonString->isExternal())) {
      ref = jsonString->getStringRef<char>();
    } else {
      auto view = StringPrimitive::createStringView(runtime, jsonString);
      storage.append(
          view.castToCharPtr(), view.castToCharPtr() + view.length());
      ref = storage;
    }
    RuntimeJSONParser<EncodingKind::ASCII> parser{runtime, ref, reviver};
    return parser.parse();
  }
  UTF16Ref ref;
  SmallU16String<32> storage;
  if (LLVM_UNLIKELY(jsonString->isExternal())) {
    ref = jsonString->getStringRef<char16_t>();
  } else {
    StringPrimitive::createStringView(runtime, jsonString)
        .appendUTF16String(storage);
    ref = storage;
  }
  RuntimeJSONParser<EncodingKind::UTF16> parser{runtime, ref, reviver};
  return parser.parse();
}

CallResult<HermesValue> runtimeJSONParseRef(Runtime &runtime, ASCIIRef json) {
#ifndef NDEBUG
  for (unsigned char ch : json)
    assert(ch < 128 && "non-ASCII character in JSON input");
#endif
  RuntimeJSONParser<EncodingKind::ASCII> parser{
      runtime, json, Runtime::makeNullHandle<Callable>()};
  return parser.parse();
}

CallResult<HermesValue> runtimeJSONParseRef(Runtime &runtime, UTF16Ref json) {
  RuntimeJSONParser<EncodingKind::UTF16> parser{
      runtime, json, Runtime::makeNullHandle<Callable>()};
  return parser.parse();
}

CallResult<HermesValue> runtimeJSONParseRef(
    Runtime &runtime,
    UTF16Stream &&stream) {
  RuntimeJSONParser<EncodingKind::UTF8> parser{runtime, std::move(stream)};
  return parser.parse();
}

} // namespace vm
} // namespace hermes
