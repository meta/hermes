/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/BCGen/HBC/UniquingStringLiteralTable.h"

#include "BytecodeGenerator.h"

#include "llvh/ADT/STLExtras.h"

#include <cassert>

namespace hermes {
namespace hbc {

StringLiteralTable::StringLiteralTable(
    ConsecutiveStringStorage &&storage,
    llvh::BitVector &&isIdentifier)
    : storage_(std::move(storage)), isIdentifier_(std::move(isIdentifier)) {
  populateStringsTableFromStorage();
}

std::vector<uint32_t> StringLiteralTable::getIdentifierHashes(
    uint32_t start) const {
  std::vector<uint32_t> result;
  assert(stringsKeys_.size() == isIdentifier_.size());
  for (size_t i = start; i < stringsKeys_.size(); ++i) {
    if (!isIdentifier_[i]) {
      continue;
    }
    result.push_back(storage_.getEntryHash(i));
  }

  return result;
}

std::vector<StringKind::Entry> StringLiteralTable::getStringKinds(
    uint32_t start) const {
  StringKind::Accumulator acc;

  assert(stringsKeys_.size() == isIdentifier_.size());
  for (size_t i = start, e = isIdentifier_.size(); i < e; ++i) {
    acc.push_back(
        isIdentifier_[i] ? StringKind::Identifier : StringKind::String);
  }

  return std::move(acc).entries();
}

void StringLiteralTable::addString(llvh::StringRef str, bool isIdentifier) {
  assert(stringsKeys_.size() == isIdentifier_.size());

  // Need to do a lookup and then an insertion, because the StringRef will point
  // to the string in stringsKeys_, like StringSetVector does.
  auto it = strings_.find(str);
  if (it == strings_.end()) {
    // Brand new string, add it to the deque and update isIdentifier_.
    size_t newIdx = stringsKeys_.size();
    stringsKeys_.emplace_back(str);
    strings_.try_emplace(stringsKeys_.back(), newIdx);
    isIdentifier_.push_back(isIdentifier);
    numIdentifierRefs_.push_back(1);
    assert(isIdentifier_[newIdx] == isIdentifier && "isIdentifier_ wrong");
    return;
  }

  // String already exists.

  if (!isIdentifier) {
    // If we're not inserting an identifier, no extra information to record.
    // Done.
    return;
  }

  uint32_t id = it->second;

  // DO NOT update isIdentifier for existing strings.
  // In lazy compilation, we must add a new string to the mapping and update the
  // strings_ table to point to the new string, which will be used for any
  // future references to it.
  // In this way, the stringsKeys_ deque may contain the same string at most
  // twice: once as a non-identifier and then later as an identifier.
  const size_t existingStrings = storage_.count();

  if (isIdentifier_[id]) {
    // We're inserting an identifier, but the string is already marked as an
    // identifier.
    // Update the numIdentifierRefs_ and that's it.
    if (id >= existingStrings) {
      // We only track the frequency of new strings, so the ID needs to be
      // translated.
      if (numIdentifierRefs_[id - existingStrings] != UINT32_MAX) {
        ++numIdentifierRefs_[id - existingStrings];
      }
    }
    return;
  }

  // Now, we're inserting an identifier, and the string is not marked as an
  // identifier already, so we need to either update the existing string or
  // add a new string to stringsKeys if it's not already committed to the
  // ConsecutiveStringStorage.
  assert(isIdentifier && !isIdentifier_[id] && "already checked above");

  if (id < existingStrings) {
    // The string is already in the storage, so we can't update
    // the storage.  Instead, we need to add a new string to the
    // stringsKeys_ deque and update strings_.
    size_t newIdx = stringsKeys_.size();
    stringsKeys_.emplace_back(str);
    isIdentifier_.push_back(true);
    numIdentifierRefs_.push_back(1);
    // Update strings_ to have the new ID.
    it->second = newIdx;
  } else {
    // The string is not in the storage, so we can just update isIdentifier_.
    isIdentifier_[id] = true;
    if (id >= existingStrings) {
      // We only track the frequency of new strings, so the ID needs to be
      // translated.
      if (numIdentifierRefs_[id - existingStrings] != UINT32_MAX) {
        ++numIdentifierRefs_[id - existingStrings];
      }
    }
  }
}

void StringLiteralTable::tryEnsure8BitStringIDForIdentifier(
    llvh::StringRef str) {
  auto it = strings_.find(str);
  if (it == strings_.end())
    return;
  uint32_t id = it->second;
  if (!isIdentifier_[id])
    return;
  const size_t existingStrings = storage_.count();

  // We only track the frequency of new strings, so the ID needs to be
  // translated.
  // If this is NOT a new string (i.e. id < existingStrings), then we can't
  // do anything about its ID so we dont do anything.
  if (id >= existingStrings)
    numIdentifierRefs_[id - existingStrings] = UINT32_MAX;
}

void StringLiteralTable::populateStorage(
    StringLiteralTable::OptimizeMode mode) {
  switch (mode) {
    case OptimizeMode::None:
      return appendStorageLazy();
    case OptimizeMode::Reorder:
      return sortAndRemap(false);
    case OptimizeMode::ReorderAndPack:
      return sortAndRemap(true);
  }
}

void StringLiteralTable::populateStringsTableFromStorage() {
  // Initialize our tables by decoding our storage's string table.
  stringsKeys_.clear();
  strings_.clear();
  std::string utf8Storage;
  uint32_t count = storage_.count();
  assert(isIdentifier_.size() == count);
  for (uint32_t i = 0; i < count; i++) {
    stringsKeys_.emplace_back(storage_.getUTF8StringAtIndex(i, utf8Storage));
    auto [it, inserted] =
        strings_.try_emplace(stringsKeys_.back(), stringsKeys_.size() - 1);
    if (!inserted) {
      // Overwrite the existing entry with the new index.
      // A duplicate entry in stringsKeys_ will only happen if the same string
      // is used first as a non-identifier and then as an identifier.
      assert(isIdentifier_[stringsKeys_.size() - 1] && "must be an identifier");
      it->second = stringsKeys_.size() - 1;
    }
  }
}

void StringLiteralTable::sortAndRemap(bool optimize) {
  const size_t existingStrings = storage_.count();
  const size_t allStrings = stringsKeys_.size();
  const size_t newStrings = allStrings - existingStrings;
  assert(
      existingStrings <= allStrings &&
      "Cannot have more written strings than strings");

  /// Associate a string with its original index in the table.
  struct Index {
    size_t origIndex;
    llvh::StringRef str;
    StringKind::Kind kind;
    /// The number of identifier references to the string.
    size_t numIdentifierRefs;

    Index(
        size_t origIndex,
        llvh::StringRef str,
        StringKind::Kind kind,
        size_t numIdentifierRefs)
        : origIndex(origIndex),
          str(str),
          kind(kind),
          numIdentifierRefs(numIdentifierRefs) {}

    using Key = std::tuple<StringKind::Kind, llvh::StringRef>;

    inline Key key() const {
      return std::make_tuple(kind, str);
    }

    inline bool operator<(const Index &that) const {
      return this->key() < that.key();
    }
  };

  /// Associates a StringTableEntry with its String Kind.
  struct KindedEntry {
    StringKind::Kind kind;
    StringTableEntry entry;

    KindedEntry(StringKind::Kind kind, StringTableEntry entry)
        : kind(kind), entry(entry) {}

   private:
    // Key for performing comparisons with.  Ordering on this key is used to
    // optimise string index layout to compress better.
    using Key = std::tuple<StringKind::Kind, uint32_t, uint32_t>;
    inline Key key() const {
      return std::make_tuple(kind, entry.getOffset(), entry.getLength());
    }

   public:
    inline bool operator<(const KindedEntry &that) const {
      return this->key() < that.key();
    }
  };

  std::vector<Index> indices;
  indices.reserve(newStrings);

  for (size_t i = existingStrings; i < allStrings; ++i) {
    indices.emplace_back(
        i,
        stringsKeys_[i],
        isIdentifier_[i] ? StringKind::Identifier : StringKind::String,
        numIdentifierRefs_[i - existingStrings]);
  }

  // Sort indices of new strings by frequency of identifier references.
  // Strings with equal frequency keep their original order.
  llvh::array_pod_sort(
      indices.begin(), indices.end(), [](const Index *a, const Index *b) {
        if (a->numIdentifierRefs != b->numIdentifierRefs)
          return a->numIdentifierRefs > b->numIdentifierRefs ? -1 : 1;
        if (a->origIndex != b->origIndex)
          return a->origIndex < b->origIndex ? -1 : 1;
        return 0;
      });

  // Take an index into all the strings, and map it to an index in to a data
  // structure containing only the new strings.
  const auto remap = [allStrings, existingStrings](size_t ix) {
    // Skip the existing strings.
    ix = std::max(ix, existingStrings);

    // Avoid a buffer overrun;
    ix = std::min(ix, allStrings);

    // Translate by the number of strings we skipped over.
    ix -= existingStrings;

    return ix;
  };

  // Sort indices within each frequency class by kind, and alphabetically.
  const auto indicesFrom = [&remap, &indices](size_t ix) {
    return indices.begin() + remap(ix);
  };

  llvh::array_pod_sort(indicesFrom(0), indicesFrom(UINT8_MAX));
  llvh::array_pod_sort(indicesFrom(UINT8_MAX), indicesFrom(UINT16_MAX));
  llvh::array_pod_sort(indicesFrom(UINT16_MAX), indicesFrom(SIZE_MAX));

  { // Add the new strings to the storage.
    std::vector<llvh::StringRef> refs;
    refs.reserve(newStrings);
    for (auto &i : indices) {
      refs.emplace_back(i.str);
    }
    ConsecutiveStringStorage newStrings(refs, optimize);
    storage_.appendStorage(std::move(newStrings));
  }

  // Associate the new string table index entries with their string kind.
  auto tableView = storage_.getStringTableView();
  std::vector<KindedEntry> kindedEntries;
  kindedEntries.reserve(newStrings);

  for (size_t i = 0, j = existingStrings; i < newStrings; ++i, ++j) {
    kindedEntries.emplace_back(indices[i].kind, tableView[j]);
  }

  // Sort index entries within each frequency and kind bucket by their offset
  // in the storage and length.
  const auto entriesFrom = [&remap, &kindedEntries](size_t ix) {
    return kindedEntries.begin() + remap(ix);
  };

  llvh::array_pod_sort(entriesFrom(0), entriesFrom(UINT8_MAX));
  llvh::array_pod_sort(entriesFrom(UINT8_MAX), entriesFrom(UINT16_MAX));
  llvh::array_pod_sort(entriesFrom(UINT16_MAX), entriesFrom(SIZE_MAX));

  // Write the re-ordered entries back into the table.
  for (size_t i = 0, j = existingStrings; i < newStrings; ++i, ++j) {
    tableView[j] = kindedEntries[i].entry;
    isIdentifier_[j] = kindedEntries[i].kind != StringKind::String;
  }

  // Update the strings table with the new storage.
  populateStringsTableFromStorage();
  numIdentifierRefs_.clear();
}

void StringLiteralTable::appendStorageLazy() {
  const size_t existingStrings = storage_.count();
  const size_t allStrings = stringsKeys_.size();

  {
    std::vector<llvh::StringRef> refs;
    refs.reserve(allStrings - existingStrings);
    for (size_t i = existingStrings; i < allStrings; ++i) {
      refs.emplace_back(stringsKeys_[i]);
    }
    storage_.appendStorage(ConsecutiveStringStorage{refs});
  }

  assert(storage_.count() == stringsKeys_.size() && "must map all strings");
}

} // namespace hbc
} // namespace hermes
