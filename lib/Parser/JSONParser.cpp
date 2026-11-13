/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Parser/JSONParser.h"
#include "hermes/ADT/HalfPairIterator.h"

#include "llvh/ADT/STLExtras.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/Support/Casting.h"

namespace hermes {
namespace parser {

JSONNull JSONNull::instance_{};
JSONBoolean JSONBoolean::true_{true};
JSONBoolean JSONBoolean::false_{false};

const char *JSONKindToString(JSONKind kind) {
  switch (kind) {
    case JSONKind::Object:
      return "Object";
    case JSONKind::Array:
      return "Array";
    case JSONKind::String:
      return "String";
    case JSONKind::Number:
      return "Number";
    case JSONKind::Boolean:
      return "Boolean";
    case JSONKind::Null:
      return "Null";
  }
  llvm_unreachable("Illegal JSONKind");
}

void JSONValue::emitInto(JSONEmitter &emitter) const {
  using llvh::cast;

  switch (this->getKind()) {
    case JSONKind::Object:
      emitter.openDict();
      for (auto pair : *cast<JSONObject>(this)) {
        emitter.emitKey(pair.first->str());
        pair.second->emitInto(emitter);
      }
      emitter.closeDict();
      break;
    case JSONKind::Array:
      emitter.openArray();
      for (auto *val : *cast<JSONArray>(this)) {
        val->emitInto(emitter);
      }
      emitter.closeArray();
      break;
    case JSONKind::String:
      emitter.emitValue(cast<JSONString>(this)->str());
      break;
    case JSONKind::Number: {
      emitter.emitValue(cast<JSONNumber>(this)->getValue());
      break;
    }
    case JSONKind::Boolean:
      emitter.emitValue(cast<JSONBoolean>(this)->getValue());
      break;
    case JSONKind::Null:
      emitter.emitNullValue();
      break;
  }
}

JSONFactory::JSONFactory(Allocator &allocator, StringTable *strTab)
    : allocator_(allocator),
      ownStrTab_(strTab ? nullptr : new StringTable(allocator_)),
      strTab_(strTab ? *strTab : *ownStrTab_) {}

JSONString *JSONFactory::getString(UniqueString *lit) {
  llvh::FoldingSetNodeID id;
  JSONString::Profile(id, lit);

  void *insertPos;
  if (auto *found = strings_.FindNodeOrInsertPos(id, insertPos))
    return found;

  auto *res = new (allocator_.Allocate<JSONString>()) JSONString(lit);
  strings_.InsertNode(res, insertPos);
  return res;
}

JSONString *JSONFactory::getString(llvh::StringRef str) {
  return getString(strTab_.getString(str));
}

JSONNumber *JSONFactory::getNumber(double value) {
  llvh::FoldingSetNodeID id;
  JSONNumber::Profile(id, value);

  void *insertPos;
  if (auto *found = numbers_.FindNodeOrInsertPos(id, insertPos))
    return found;

  auto *res = new (allocator_.Allocate<JSONNumber>()) JSONNumber(value);
  numbers_.InsertNode(res, insertPos);
  return res;
}

JSONHiddenClass *JSONFactory::getHiddenClass(const HiddenClassKey &key) {
  auto it = hiddenClasses_.find(key);
  if (it != hiddenClasses_.end())
    return it->second;

  auto *cls = new (allocator_, key.first)
      JSONHiddenClass(key.first, key.second, key.second + key.first);
  hiddenClasses_.insert({std::make_pair(cls->size(), cls->begin()), cls});
  return cls;
}

JSONString *JSONFactory::sortProps(Prop *from, Prop *to) {
  // Sort the keys by sorting the indexes first.
  llvh::array_pod_sort(from, to, [](const Prop *a, const Prop *b) {
    return a->first->str().compare(b->first->str());
  });

  // Check for duplicate keys
  JSONString *lastKey = nullptr;
  for (auto *p = from; p != to; ++p) {
    // Duplicate?
    if (p->first == lastKey)
      return lastKey;
    lastKey = p->first;
  }

  return nullptr;
}

JSONObject *JSONFactory::newObject(Prop *from, Prop *to, bool propsAreSorted) {
  if (!propsAreSorted) {
    // Sort the properties. If non-null was returned, there was a duplicate.
    if (sortProps(from, to))
      return nullptr;
  }

  // Look for an existing hidden class.
  llvh::SmallVector<JSONString *, 10> keys(
      makePairFirstIterator(from), makePairFirstIterator(to));

  auto *klazz = getHiddenClass({keys.size(), keys.begin()});

  // Create the object with the class.
  return newObject(
      klazz, makePairSecondIterator(from), makePairSecondIterator(to));
}

bool JSONFactory::LessHiddenClassKey::operator()(
    const HiddenClassKey &a,
    const HiddenClassKey &b) const {
  if (a.first < b.first)
    return true;
  if (a.first > b.first)
    return false;

  // Same length - need to compare the contents.
  for (size_t i = 0, e = a.first; i != e; ++i) {
    JSONString *s1 = a.second[i];
    JSONString *s2 = b.second[i];
    if (s1 < s2)
      return true;
    if (s1 > s2)
      return false;
  }

  return false;
}

JSONParser::JSONParser(
    JSONFactory &factory,
    std::unique_ptr<llvh::MemoryBuffer> input,
    SourceErrorManager &sm,
    bool convertSurrogates)
    : factory_(factory),
      lexer_(
          std::move(input),
          sm,
          factory_.getAllocator(),
          &factory_.getStringTable(),
          true,
          convertSurrogates),
      sm_(sm) {}

llvh::Optional<JSONValue *> JSONParser::parse() {
  lexer_.advance();
  auto res = parseValue();
  if (!res)
    return llvh::None;
  if (lexer_.getSourceMgr().getErrorCount() != 0)
    return llvh::None;
  return res.getValue();
}

llvh::Optional<JSONValue *> JSONParser::parseValue() {
  if (LLVM_UNLIKELY(recursionDepth_ >= MAX_RECURSION_DEPTH)) {
    error("Too many nested JSON values");
    return llvh::None;
  }
  ++recursionDepth_;
  auto res = parseValueImpl();
  --recursionDepth_;
  return res;
}

llvh::Optional<JSONValue *> JSONParser::parseValueImpl() {
  bool needsNegation = false;
  switch (lexer_.getCurToken()->getKind()) {
    case TokenKind::string_literal: {
      auto res = factory_.getString(lexer_.getCurToken()->getStringLiteral());
      lexer_.advance();
      return res;
    }
    case TokenKind::minus:
      needsNegation = true;
      lexer_.advance();
      if (lexer_.getCurToken()->getKind() != TokenKind::numeric_literal) {
        error("No numeric literal following minus (-) token in value");
        return llvh::None;
      }
      [[fallthrough]];
    case TokenKind::numeric_literal: {
      auto numericValue = lexer_.getCurToken()->getNumericLiteral();
      auto res =
          factory_.getNumber(needsNegation ? -numericValue : numericValue);
      lexer_.advance();
      return res;
    }

    case TokenKind::l_brace:
      lexer_.advance();
      return parseObject();
    case TokenKind::l_square:
      lexer_.advance();
      return parseArray();

    case TokenKind::rw_true:
      lexer_.advance();
      return factory_.getBoolean(true);
    case TokenKind::rw_false:
      lexer_.advance();
      return factory_.getBoolean(false);
    case TokenKind::rw_null:
      lexer_.advance();
      return factory_.getNull();

    default:
      error("JSON object or array expected");
      return llvh::None;
  }
}

llvh::Optional<JSONValue *> JSONParser::parseArray() {
  llvh::SmallVector<JSONValue *, 10> storage;

  if (lexer_.getCurToken()->getKind() != TokenKind::r_square) {
    for (;;) {
      auto val = parseValue();
      if (!val)
        return llvh::None;
      storage.push_back(val.getValue());

      if (lexer_.getCurToken()->getKind() == TokenKind::comma) {
        lexer_.advance();
        if (lexer_.getCurToken()->getKind() == TokenKind::r_square)
          break;
      } else {
        break;
      }
    }
    if (lexer_.getCurToken()->getKind() != TokenKind::r_square) {
      error("expected ']'");
      return llvh::None;
    }
  }

  lexer_.advance(); // consume the ']'

  return factory_.newArray(storage.size(), storage.begin(), storage.end());
}

llvh::Optional<JSONValue *> JSONParser::parseObject() {
  llvh::SmallVector<JSONFactory::Prop, 10> pairs;

  if (lexer_.getCurToken()->getKind() != TokenKind::r_brace) {
    for (;;) {
      if (lexer_.getCurToken()->getKind() != TokenKind::string_literal) {
        error("expected a string");
        return llvh::None;
      }
      JSONString *key =
          factory_.getString(lexer_.getCurToken()->getStringLiteral());

      if (lexer_.advance()->getKind() != TokenKind::colon) {
        error("expected ':'");
        return llvh::None;
      }
      lexer_.advance();

      if (auto val = parseValue()) {
        pairs.push_back({key, val.getValue()});
      } else
        return llvh::None;

      if (lexer_.getCurToken()->getKind() == TokenKind::comma) {
        lexer_.advance();
        if (lexer_.getCurToken()->getKind() == TokenKind::r_brace)
          break;
      } else {
        break;
      }
    }
    if (lexer_.getCurToken()->getKind() != TokenKind::r_brace) {
      error("expected '}'");
      return llvh::None;
    }
  }

  lexer_.advance(); // consume the '}'

  if (auto *duplicate = factory_.sortProps(pairs.begin(), pairs.end())) {
    error("key '" + duplicate->str() + "' is already present");
    return llvh::None;
  }

  return factory_.newObject(pairs.begin(), pairs.end(), true);
}

} // namespace parser
} // namespace hermes
