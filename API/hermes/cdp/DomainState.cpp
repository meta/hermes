/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string>
#include <functional>
#include <iostream>
#include "DomainState.h"

#include <cassert>

namespace facebook {
namespace hermes {
namespace cdp {

std::unique_ptr<StateValue> DictionaryStateValue::copy() const {
  auto dict = std::make_unique<DictionaryStateValue>();
  for (auto &[key, value] : values) {
    assert(value != nullptr && "Shouldn't have stored any nullptr");
    dict->values[key] = value->copy();
  }
  return dict;
}

std::unique_ptr<StateValue> BooleanStateValue::copy() const {
  auto stateValue = std::make_unique<BooleanStateValue>();
  stateValue->value = value;

  return stateValue;
}

DomainState::DomainState() : dict_(std::make_unique<DictionaryStateValue>()) {}

DomainState::DomainState(std::unique_ptr<DictionaryStateValue> dict)
    : dict_(std::move(dict)) {}

std::unique_ptr<DomainState> DomainState::copy() {
  std::unique_ptr<StateValue> copiedStateValue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    copiedStateValue = dict_->copy();
  }

  // Convert unique_ptr<StateValue> to unique_ptr<DictionaryStateValue>
  auto dictPtr =
      dynamic_cast<DictionaryStateValue *>(copiedStateValue.release());
  auto dictCopy = std::unique_ptr<DictionaryStateValue>(dictPtr);

  return std::make_unique<DomainState>(std::move(dictCopy));
}

std::unique_ptr<StateValue> DomainState::getCopy(
    std::vector<std::string> paths) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(!paths.empty() && "Must have path to copy");

  std::string key = paths.back();
  assert(!key.empty() && "Key cannot be empty");
  paths.pop_back();

  DictionaryStateValue *dict = getDict(paths, false);
  if (dict && dict->values.find(key) != dict->values.end()) {
    return dict->values[key]->copy();
  } else {
    return nullptr;
  }
}

DomainState::Transaction DomainState::transaction() {
  return Transaction(*this);
}

DictionaryStateValue *DomainState::getDict(
    const std::vector<std::string> &paths,
    bool createMissingDict) {
  DictionaryStateValue *dict = dict_.get();
  assert(
      dict != nullptr &&
      "DomainState should always have a DictionaryStateValue");
  for (auto &path : paths) {
    assert(!path.empty() && "Strings in paths should not be empty strings");
    if (dict->values.find(path) == dict->values.end()) {
      if (createMissingDict) {
        auto newDict = std::make_unique<DictionaryStateValue>();
        dict->values[path] = std::move(newDict);
      } else {
        return nullptr;
      }
    }

    StateValue *sv = dict->values[path].get();
    dict = dynamic_cast<DictionaryStateValue *>(sv);
    assert(dict != nullptr && "Expected DictionaryStateValue");
  }
  return dict;
}

void DomainState::commitTransaction(Transaction &transaction) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &modification : transaction.modifications_) {
    std::vector<std::string> &paths = modification.first;
    std::unique_ptr<StateValue> &value = modification.second;

    assert(!paths.empty() && "Must have path to manipulate");

    std::string key = paths.back();
    assert(!key.empty() && "Key cannot be empty");
    paths.pop_back();

    // nullptr means deletion
    if (value == nullptr) {
      DictionaryStateValue *dictPtr = getDict(paths, false);
      if (dictPtr) {
        dictPtr->values.erase(key);
      }
    } else {
      getDict(paths, true)->values[key] = std::move(value);
    }
  }
}

DomainState::Transaction::Transaction(DomainState &state) : state_(state) {}

DomainState::Transaction::~Transaction() {
  state_.commitTransaction(*this);
}

void DomainState::Transaction::add(
    std::vector<std::string> paths,
    const StateValue &value) {
  modifications_.emplace_back(paths, value.copy());
}

void DomainState::Transaction::remove(std::vector<std::string> paths) {
  modifications_.emplace_back(paths, nullptr);
}

} // namespace cdp
} // namespace hermes
} // namespace facebook
