/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "identtable"
#include "hermes/VM/IdentifierTable.h"

#include "hermes/Support/Conversions.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/CallResult.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"

#include "llvh/Support/Debug.h"

namespace hermes {
namespace vm {

IdentifierTable::LookupEntry::LookupEntry(
    StringPrimitive *str,
    bool isNotUniqued)
    : strPrim_(str),
      isUTF16_(false),
      isNotUniqued_(isNotUniqued),
      num_(NON_LAZY_STRING_PRIM_TAG) {
  assert(str && "Invalid string primitive pointer");
  if (str->isASCII()) {
    hash_ = hermes::hashString(str->castToASCIIRef());
  } else {
    hash_ = hermes::hashString(str->castToUTF16Ref());
  }
}

IdentifierTable::IdentifierTable() {
  hashTable_.setIdentifierTable(this);
}

CallResult<SymbolID>
IdentifierTable::getSymbolID(Runtime &runtime, UTF16Ref str, uint32_t hash) {
  return getOrCreateIdentifier(
      runtime, str, Runtime::makeNullHandle<StringPrimitive>(), hash);
}

CallResult<SymbolID>
IdentifierTable::getSymbolID(Runtime &runtime, ASCIIRef str, uint32_t hash) {
  return getOrCreateIdentifier(
      runtime, str, Runtime::makeNullHandle<StringPrimitive>(), hash);
}

SymbolID IdentifierTable::registerLazyIdentifier(
    Runtime &runtime,
    ASCIIRef str) {
  return registerLazyIdentifierImpl(runtime, str, hermes::hashString(str));
}

SymbolID IdentifierTable::registerLazyIdentifier(
    Runtime &runtime,
    ASCIIRef str,
    uint32_t hash) {
  return registerLazyIdentifierImpl(runtime, str, hash);
}

SymbolID IdentifierTable::registerLazyIdentifier(
    Runtime &runtime,
    UTF16Ref str) {
  return registerLazyIdentifierImpl(runtime, str, hermes::hashString(str));
}

SymbolID IdentifierTable::registerLazyIdentifier(
    Runtime &runtime,
    UTF16Ref str,
    uint32_t hash) {
  return registerLazyIdentifierImpl(runtime, str, hash);
}

ExecutionStatus IdentifierTable::getSymbolFromPrimitive(
    Runtime &runtime,
    PseudoHandle<StringPrimitive> str,
    MutableHandle<SymbolID> out) {
  assert(str && "null string primitive");
  if (str->isUniqued()) {
    // If the string was already uniqued, we can return directly.
    out = str->getUniqueID(runtime);
    return ExecutionStatus::RETURNED;
  }
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.str = std::move(str);
  // Force the string primitive to flatten if it's a rope.
  lv.str = StringPrimitive::ensureFlat(runtime, lv.str);
  uint32_t hash = lv.str->getOrComputeHash();
  auto cr = lv.str->isASCII()
      ? getOrCreateIdentifier(runtime, lv.str->castToASCIIRef(), lv.str, hash)
      : getOrCreateIdentifier(runtime, lv.str->castToUTF16Ref(), lv.str, hash);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  out = *cr;
  return ExecutionStatus::RETURNED;
}

StringPrimitive *IdentifierTable::getStringPrim(Runtime &runtime, SymbolID id) {
  auto &entry = getLookupTableEntry(id);
  if (entry.isStringPrim()) {
    return entry.getStringPrimRef();
  }

  // This is a lazy identifier, since a string primitive is requested, we must
  // materialize it.
  return materializeLazyIdentifier(runtime, id);
}

StringView IdentifierTable::getStringView(
    SymbolID id,
    MutableHandle<StringPrimitive> storage) const {
  auto &entry = getLookupTableEntry(id);
  if (entry.isStringPrim()) {
    // The const_cast is a mechanical requirement as it's not worth it to
    // add const version constructors for Handle.
    storage.set(const_cast<StringPrimitive *>(entry.getStringPrim()));
    // We know that this string already exists in the identifier table,
    // and hence it's safe to call the const version of getStringView.
    return StringPrimitive::createStringViewMustBeFlat(storage);
  }
  if (entry.isLazyASCII()) {
    return StringView(entry.getLazyASCIIRef());
  }
  return StringView(entry.getLazyUTF16Ref());
}

OptValue<uint32_t> IdentifierTable::symbolToArrayIndex(SymbolID id) const {
  auto &entry = getLookupTableEntry(id);
  if (entry.isStringPrim()) {
    // Identifier strings are always flat.
    return toArrayIndex(const_cast<StringPrimitive *>(entry.getStringPrim()));
  }
  if (entry.isLazyASCII()) {
    auto ref = entry.getLazyASCIIRef();
    return hermes::toArrayIndex(ref.data(), ref.data() + ref.size());
  }
  auto ref = entry.getLazyUTF16Ref();
  return hermes::toArrayIndex(ref.data(), ref.data() + ref.size());
}

bool IdentifierTable::symbolEqualsToStringPrim(
    SymbolID id,
    const StringPrimitive *strPrim) const {
  auto &entry = getLookupTableEntry(id);
  if (entry.isStringPrim())
    return strPrim->equals(entry.getStringPrim());
  if (entry.isLazyASCII())
    return strPrim->equals(StringView(entry.getLazyASCIIRef()));
  return strPrim->equals(StringView(entry.getLazyUTF16Ref()));
}

StringView IdentifierTable::getStringViewForDev(
    SymbolID id,
    MutableHandle<StringPrimitive> storage) const {
  if (id == SymbolID::empty()) {
    assert(false && "getStringViewForDev on empty SymbolID");
    return "<<empty>>";
  }
  if (id == SymbolID::deleted()) {
    assert(false && "getStringViewForDev on deleted SymbolID");
    return "<<deleted>>";
  }
  if (id.isInvalid()) {
    assert(false && "getStringViewForDev on invalid SymbolID");
    return "<<invalid>>";
  }
  return getStringView(id, storage);
}

std::string IdentifierTable::convertSymbolToUTF8(SymbolID id) {
  auto &vectorEntry = getLookupTableEntry(id);
  if (vectorEntry.isStringPrim()) {
    const StringPrimitive *stringPrim = vectorEntry.getStringPrim();
    llvh::SmallVector<char16_t, 16> tmp;
    stringPrim->appendUTF16String(tmp);
    std::string out;
    convertUTF16ToUTF8WithReplacements(out, UTF16Ref{tmp});
    return out;
  } else if (vectorEntry.isLazyASCII()) {
    auto asciiRef = vectorEntry.getLazyASCIIRef();
    return std::string{asciiRef.begin(), asciiRef.end()};
  } else if (vectorEntry.isLazyUTF16()) {
    auto ref = vectorEntry.getLazyUTF16Ref();
    std::string out;
    convertUTF16ToUTF8WithReplacements(out, ref);
    return out;
  } else {
    llvm_unreachable("Invalid symbol given");
  }
}

void IdentifierTable::markIdentifiers(RootAcceptor &acceptor, GC &gc) {
  for (auto &vectorEntry : lookupVector_) {
    if (!vectorEntry.isFreeSlot() && vectorEntry.isStringPrim()) {
      assert(
          !gc.inYoungGen(vectorEntry.getStringPrimRef()) &&
          "Identifiers must be allocated in the old gen");
      acceptor.acceptPtr(vectorEntry.getStringPrimRef());
    }
  }
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
void IdentifierTable::snapshotAddNodes(HeapSnapshot &snap) {
  snap.beginNode();
  snap.endNode(
      HeapSnapshot::NodeType::Native,
      "std::vector<LookupEntry>",
      GCBase::IDTracker::reserved(
          GCBase::IDTracker::ReservedObjectID::IdentifierTableLookupVector),
      lookupVector_.capacity() * sizeof(LookupEntry),
      0);

  snap.beginNode();
  snap.endNode(
      HeapSnapshot::NodeType::Native,
      "CompactArray",
      GCBase::IDTracker::reserved(
          GCBase::IDTracker::ReservedObjectID::IdentifierTableHashTable),
      hashTable_.additionalMemorySize(),
      0);
}

void IdentifierTable::snapshotAddEdges(HeapSnapshot &snap) {
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Internal,
      "lookupVector",
      GCBase::IDTracker::reserved(
          GCBase::IDTracker::ReservedObjectID::IdentifierTableLookupVector));
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Internal,
      "hashTable",
      GCBase::IDTracker::reserved(
          GCBase::IDTracker::ReservedObjectID::IdentifierTableHashTable));
}
#endif // HERMES_MEMORY_INSTRUMENTATION

void IdentifierTable::visitIdentifiers(
    const std::function<void(SymbolID, const StringPrimitive *)> &acceptor) {
  for (uint32_t i = 0; i < getSymbolsEnd(); ++i) {
    auto &vectorEntry = getLookupTableEntry(i);
    if (!vectorEntry.isFreeSlot()) {
      const StringPrimitive *str =
          vectorEntry.isStringPrim() ? vectorEntry.getStringPrim() : nullptr;
      acceptor(SymbolID::unsafeCreate(i), str);
    }
  }
}

template <typename T, bool Unique>
CallResult<PseudoHandle<StringPrimitive>>
IdentifierTable::allocateDynamicString(
    Runtime &runtime,
    llvh::ArrayRef<T> str,
    Handle<StringPrimitive> primHandle) {
  size_t length = str.size();

  assert(
      (!primHandle || primHandle->isFlat()) && "StringPrimitive must be flat");

  GCScope gcScope(runtime);

  PseudoHandle<StringPrimitive> result;
  if (StringPrimitive::isExternalLength(length)) {
    if (LLVM_UNLIKELY(length > StringPrimitive::MAX_STRING_LENGTH)) {
      return runtime.raiseRangeError("String length exceeds limit");
    }
    std::basic_string<T> stdString(str.begin(), str.end());
    auto cr = ExternalStringPrimitive<T>::createLongLived(
        runtime, std::move(stdString));
    if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    result = createPseudoHandle(vmcast<StringPrimitive>(*cr));
  } else {
    auto *tmp = runtime.makeAVariable<
        DynamicStringPrimitive<T, Unique>,
        HasFinalizer::No,
        LongLived::Yes>(
        DynamicStringPrimitive<T, Unique>::allocationSize((uint32_t)length),
        length);
    // Since we keep a raw pointer to mem, no more JS heap allocations after
    // this point.
    NoAllocScope _(runtime);
    if (primHandle) {
      str = primHandle->getStringRef<T>();
    }
    std::copy(str.begin(), str.end(), tmp->getRawPointerForWrite());
    result = createPseudoHandle<StringPrimitive>(tmp);
  }

  return result;
}

uint32_t IdentifierTable::allocIDAndInsert(
    Runtime &runtime,
    uint32_t hashTableIndex,
    StringPrimitive *strPrim) {
  uint32_t nextId = allocNextID();
  SymbolID symbolId = SymbolID::unsafeCreate(nextId);
  runtime.getHeap().symbolAllocationBarrier(symbolId);
  assert(lookupVector_[nextId].isFreeSlot() && "Allocated a non-free slot");
  strPrim->convertToUniqued(symbolId);

  // We must assign strPrim to the lookupVector before inserting to
  // hashTable_, because inserting to hashTable_ could trigger a grow/rehash,
  // which requires accessing the newly inserted string primitive.
  new (&lookupVector_[nextId]) LookupEntry(strPrim);

  hashTable_.insert(hashTableIndex, symbolId);

  LLVM_DEBUG(
      llvh::dbgs() << "allocated symbol " << nextId << " '" << strPrim
                   << "'\n");

  return nextId;
}

template <typename T>
CallResult<SymbolID> IdentifierTable::getOrCreateIdentifier(
    Runtime &runtime,
    llvh::ArrayRef<T> str,
    Handle<StringPrimitive> maybeIncomingPrimHandle,
    uint32_t hash) {
  assert(
      !(maybeIncomingPrimHandle && maybeIncomingPrimHandle->isUniqued()) &&
      "Should not call getOrCreateIdentifier with a uniqued StrPrim");
  assert(
      (!maybeIncomingPrimHandle || maybeIncomingPrimHandle->isFlat()) &&
      "StringPrimitive must be flat");

  auto idx = hashTable_.lookupString(str, hash);
  if (hashTable_.isValid(idx)) {
    NoAllocScope scope{runtime};
    const auto id = hashTable_.get(idx);
    auto sym = SymbolID::unsafeCreate(id);
    // Read barrier here because a symbol value is getting read out of the hash
    // map.
    runtime.getHeap().weakRefReadBarrier(sym);
    return sym;
  }

  // It is tempting here to check whether the incoming StringPrimitive can be
  // uniqued, and use it instead of allocating a new one. The problem is that
  // identifiers must always be allocated in "long-lived" memory, but we don't
  // (yet) have a way of checking whether that is the case. If in the future we
  // did get that GC API, the check would look something like this:
  // \code
  // if (maybeIncomingPrimHandle &&
  //     LLVM_UNLIKELY(maybeIncomingPrimHandle->canBeUniqued()) &&
  //     runtime.getHeap().isLongLived(*maybeIncomingPrimHandle)) {
  //   return SymbolID::unsafeCreate(
  //       allocIDAndInsert(idx, maybeIncomingPrimHandle.get()));
  // }
  // \endcode

  CallResult<PseudoHandle<StringPrimitive>> cr =
      allocateDynamicString(runtime, str, maybeIncomingPrimHandle);
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  (*cr)->setHash(hash);

  // Allocate the id after we have performed memory allocations because a GC
  // would have freed id.
  return SymbolID::unsafeCreate(allocIDAndInsert(runtime, idx, cr->get()));
}

StringPrimitive *IdentifierTable::getExistingStringPrimitiveOrNull(
    Runtime &runtime,
    llvh::ArrayRef<char16_t> str) {
  return getExistingStringPrimitiveOrNullWithHash(
      runtime, str, hashString(str));
}

StringPrimitive *IdentifierTable::getExistingStringPrimitiveOrNullWithHash(
    Runtime &runtime,
    llvh::ArrayRef<char16_t> str,
    uint32_t hash) {
  assert(hash == hashString(str) && "hash is not correct");
  auto idx = hashTable_.lookupString(str, hash);
  if (!hashTable_.isValid(idx)) {
    return nullptr;
  }
  struct : public Locals {
    PinnedValue<SymbolID> sym;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // Use a handle since getStringPrim may need to materialize the string.
  const auto id = hashTable_.get(idx);
  auto symID = SymbolID::unsafeCreate(id);
  // Read barrier here because a symbol value is getting read out of the hash
  // map.
  runtime.getHeap().weakRefReadBarrier(symID);
  lv.sym = symID;
  return getStringPrim(runtime, *lv.sym);
}

template <typename T>
SymbolID IdentifierTable::registerLazyIdentifierImpl(
    Runtime &runtime,
    llvh::ArrayRef<T> str,
    uint32_t hash) {
  auto idx = hashTable_.lookupString(str, hash);
  if (hashTable_.isValid(idx)) {
    // If the string is already in the table, return it.
    const auto id = hashTable_.get(idx);
    auto sym = SymbolID::unsafeCreate(id);
    // Read barrier here because a symbol value is getting read out of the hash
    // map.
    runtime.getHeap().weakRefReadBarrier(sym);
    return sym;
  }
  uint32_t nextId = allocNextID();
  // No allocation barrier is needed because lazy symbols are not garbage
  // collected.
  SymbolID symbolId = SymbolID::unsafeCreate(nextId);
  assert(lookupVector_[nextId].isFreeSlot() && "Allocated a non-free slot");
  new (&lookupVector_[nextId]) LookupEntry(str, hash);
  hashTable_.insert(idx, symbolId);
  LLVM_DEBUG(
      llvh::dbgs() << "Allocated lazy identifier: " << nextId << " " << str
                   << "\n");
  return symbolId;
}

StringPrimitive *IdentifierTable::materializeLazyIdentifier(
    Runtime &runtime,
    SymbolID id) {
  auto &entry = getLookupTableEntry(id);
  assert(
      (entry.isLazyASCII() || entry.isLazyUTF16()) && "identifier is not lazy");
  // This in theory can throw if running out of memory. However there are only
  // finite number of persistent identifiers, and we should always have enough
  // memory to hold them.
  PseudoHandle<StringPrimitive> strPrim = runtime.ignoreAllocationFailure(
      entry.isLazyASCII() ? allocateDynamicString(
                                runtime,
                                entry.getLazyASCIIRef(),
                                Runtime::makeNullHandle<StringPrimitive>())
                          : allocateDynamicString(
                                runtime,
                                entry.getLazyUTF16Ref(),
                                Runtime::makeNullHandle<StringPrimitive>()));
  if (id.isUniqued())
    strPrim->convertToUniqued(id);
  LLVM_DEBUG(llvh::dbgs() << "Materializing lazy identifier " << id << "\n");
  entry.materialize(strPrim.get());
  return strPrim.get();
}

void IdentifierTable::freeSymbol(uint32_t index) {
  assert(index < lookupVector_.size() && "Invalid symbol index to free");
  assert(
      lookupVector_[index].isNonLazyStringPrim() &&
      "The specified symbol cannot be freed");

  LLVM_DEBUG(
      llvh::dbgs() << "Freeing symbol index " << index << " '"
                   << lookupVector_[index].getStringPrim() << "'\n");

  if (LLVM_LIKELY(!lookupVector_[index].isNotUniqued())) {
    auto *strPrim =
        const_cast<StringPrimitive *>(lookupVector_[index].getStringPrim());
    strPrim->clearUniquedBit();
    hashTable_.remove(strPrim);
  }
  freeID(index);
}

uint32_t IdentifierTable::allocNextID() {
  // If the free list is empty, grow the array.
  if (firstFreeID_ == LookupEntry::FREE_LIST_END) {
    uint32_t newID = lookupVector_.size();
    if (LLVM_UNLIKELY(newID > LookupEntry::MAX_IDENTIFIER)) {
      hermes_fatal("Failed to allocate Identifier: IdentifierTable is full");
    }
    lookupVector_.emplace_back();
    LLVM_DEBUG(
        llvh::dbgs() << "Allocated new symbol id at end " << newID << "\n");
    // Don't need to tell the GC about this ID, it will assume any growth is
    // live.
    return newID;
  }

  // Allocate from the free list.
  uint32_t nextId = firstFreeID_;
  auto &entry = getLookupTableEntry(nextId);
  assert(entry.isFreeSlot() && "firstFreeID_ is not a free slot");
  firstFreeID_ = entry.getNextFreeSlot();
  LLVM_DEBUG(llvh::dbgs() << "Allocated freed symbol id " << nextId << "\n");
  return nextId;
}

void IdentifierTable::freeID(uint32_t id) {
  assert(
      lookupVector_[id].isStringPrim() &&
      "The specified symbol cannot be freed");

  // Add the freed index to the free list.
  lookupVector_[id].free(firstFreeID_);
  firstFreeID_ = id;
  LLVM_DEBUG(llvh::dbgs() << "Freeing ID " << id << "\n");
}

void IdentifierTable::freeUnmarkedSymbols(
    llvh::BitVector &markedSymbols,
    GC::IDTracker &tracker) {
  assert(
      markedSymbols.size() <= lookupVector_.size() &&
      "Size of markedSymbols must be less than the current lookupVector");
  const bool hasTrackedObjectIDs = tracker.hasTrackedObjectIDs();
  for (int i = markedSymbols.find_first_unset(); i >= 0;
       i = markedSymbols.find_next_unset(i)) {
    assert((unsigned)i < markedSymbols.size() && "New symbol is unmarked");
    // We never free StringPrimitives that are materialized from a lazy
    // identifier.
    if (lookupVector_[i].isNonLazyStringPrim()) {
      if (hasTrackedObjectIDs) {
        tracker.untrackSymbol(i);
      }
      freeSymbol(i);
    } else {
      // This symbol should not be freed, update the marked symbols so the GC
      // knows that it is retained.
      markedSymbols.set(i);
    }
  }
}

#ifdef HERMES_SLOW_DEBUG
bool IdentifierTable::isSymbolLive(SymbolID id) const {
  auto &entry = getLookupTableEntry(id);
  // If the entry is not a free slot, then it is live.
  return !entry.isFreeSlot();
}

const StringPrimitive *IdentifierTable::getStringForSymbol(SymbolID id) const {
  auto &entry = getLookupTableEntry(id);
  if (entry.isStringPrim()) {
    return entry.getStringPrim();
  }
  return nullptr;
}
#endif

SymbolID IdentifierTable::createNotUniquedLazySymbol(ASCIIRef desc) {
  uint32_t nextID = allocNextID();
  // No allocation barrier is needed because lazy symbols are not garbage
  // collected.
  new (&lookupVector_[nextID]) LookupEntry(desc, 0, true);
  return SymbolID::unsafeCreateNotUniqued(nextID);
}

CallResult<SymbolID> IdentifierTable::createNotUniquedSymbol(
    Runtime &runtime,
    Handle<StringPrimitive> desc) {
  StringPrimitive *str;
  if (runtime.getHeap().inYoungGen(desc.get())) {
    // Need to reallocate in the old gen if the description is in the young gen.
    CallResult<PseudoHandle<StringPrimitive>> longLivedStr = desc->isASCII()
        ? allocateDynamicString<char, /* Unique */ false>(
              runtime, desc->castToASCIIRef(), desc)
        : allocateDynamicString<char16_t, /* Unique */ false>(
              runtime, desc->castToUTF16Ref(), desc);
    if (LLVM_UNLIKELY(longLivedStr == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    str = longLivedStr->get();
  } else {
    // Description is already in the old gen, just point to it.
    str = *desc;
  }

  // Since we keep a raw pointer to mem, no more JS heap allocations after this
  // point.
  NoAllocScope _(runtime);
  // Allocate the id after we have performed memory allocations because a GC
  // would have freed the newly allocated ID.
  uint32_t nextID = allocNextID();
  SymbolID symbolId = SymbolID::unsafeCreateNotUniqued(nextID);
  runtime.getHeap().symbolAllocationBarrier(SymbolID::unsafeCreate(nextID));
  new (&lookupVector_[nextID]) LookupEntry(str, true);
  return symbolId;
}

llvh::raw_ostream &operator<<(llvh::raw_ostream &OS, SymbolID symbolID) {
  if (symbolID.isInvalid())
    return OS << "SymbolID(INVALID)";
  else if (symbolID == SymbolID::deleted())
    return OS << "SymbolID(DELETED)";
  else
    return OS << "SymbolID("
              << (symbolID.isUniqued() ? "(Uniqued)" : "(Not Uniqued)")
              << symbolID.unsafeGetIndex() << ")";
}

} // namespace vm
} // namespace hermes

#undef DEBUG_TYPE
