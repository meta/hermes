/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/IdentifierTable.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/StringView.h"

#include "VMRuntimeTestHelpers.h"

#include "gtest/gtest.h"

#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace hermes;
using namespace hermes::vm;

namespace {

using IdentifierTableLargeHeapTest = LargeHeapRuntimeTestFixture;

// When handlesan is ON, there could be symbols freed when creating new symbols
// in the test, so we can't reliably assert about the create symbol index.
#if HERMESVM_SANITIZE_HANDLES == 0
TEST_F(IdentifierTableLargeHeapTest, LookupTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> storageSlot;
    PinnedValue<StringPrimitive> d;
    PinnedValue<SymbolID> tmp;
    PinnedValue<StringPrimitive> e;
    PinnedValue<SymbolID> tmp2;
    PinnedValue<SymbolID> sa;
    PinnedValue<SymbolID> sb;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  IdentifierTable &table = runtime.getIdentifierTable();

  uint32_t predefinedCount = table.getSymbolsEnd();

  UTF16Ref a{u"foo", 3};
  UTF16Ref b{u"ab", 2};
  UTF16Ref c{u"foo", 3};

  lv.sa = table.getSymbolID(runtime, a).getValue();
  SymbolID sa = *lv.sa;
  ASSERT_GE(sa.unsafeGetIndex(), predefinedCount);
  lv.sb = table.getSymbolID(runtime, b).getValue();
  SymbolID sb = *lv.sb;
  ASSERT_GE(sb.unsafeGetIndex(), predefinedCount);

  EXPECT_EQ(0u, sa.unsafeGetIndex() - predefinedCount);
  EXPECT_EQ(1u, sb.unsafeGetIndex() - predefinedCount);
  EXPECT_EQ(
      0u,
      table.getSymbolID(runtime, a).getValue().unsafeGetIndex() -
          predefinedCount);
  EXPECT_EQ(
      0u,
      table.getSymbolID(runtime, c).getValue().unsafeGetIndex() -
          predefinedCount);

  auto d = createNoThrowForTest(runtime, llvh::StringRef("foo"), lv.d);
  EXPECT_EQ(
      0u,
      stringToSymbolIDForTest(runtime, d, lv.tmp)
              .getValue()
              .get()
              .unsafeGetIndex() -
          predefinedCount);

  auto e = createNoThrowForTest(runtime, llvh::StringRef("ab"), lv.e);
  EXPECT_EQ(
      1u,
      stringToSymbolIDForTest(runtime, e, lv.tmp2)
              .getValue()
              .get()
              .unsafeGetIndex() -
          predefinedCount);

  MutableHandle<StringPrimitive> storage{lv.storageSlot};
  EXPECT_TRUE(table.getStringView(sa, storage).equals(a));
  EXPECT_TRUE(table.getStringView(sb, storage).equals(b));
  SmallU16String<8> tmp;
  table.getStringView(sa, storage).appendUTF16String(tmp);
  EXPECT_EQ(a, tmp.arrayRef());
  tmp.clear();
  table.getStringView(sb, storage).appendUTF16String(tmp);
  EXPECT_EQ(b, tmp.arrayRef());

  // Ensure allocations are aligned.
  EXPECT_EQ(
      0u,
      (uint64_t)runtime.getStringPrimFromSymbolID(sa) % (uint64_t)HeapAlign);
  EXPECT_EQ(
      0u,
      (uint64_t)runtime.getStringPrimFromSymbolID(sb) % (uint64_t)HeapAlign);
}
#endif

// The identifier hash table must keep its memory bounded when a bounded set
// of live entries is churned repeatedly. Freed entries leave "deleted"
// tombstones that count towards the load factor which triggers a rehash, so
// the table must reclaim them in place rather than growing capacity forever.
//
// This interns ~500K unique symbols, so it is gated off under handle
// sanitization, where it would be prohibitively slow.
#if HERMESVM_SANITIZE_HANDLES == 0
TEST_F(IdentifierTableLargeHeapTest, HashTableBoundedWithDeletedEntries) {
  IdentifierTable &table = runtime.getIdentifierTable();

  constexpr size_t kBatch = 1000;
  constexpr size_t kRounds = 500;

  size_t counter = 0;
  auto internAndDropBatch = [&]() {
    for (size_t i = 0; i < kBatch; ++i) {
      // Flush handles each iteration so the interned symbols are not kept
      // alive by the handle scope and can be freed by the collection below.
      GCScopeMarkerRAII marker{runtime};
      std::string s = "unique_ident_" + std::to_string(counter++);
      auto symRes = table.getSymbolID(runtime, ASCIIRef{s.data(), s.size()});
      ASSERT_FALSE(isException(symRes));
    }
    // With no remaining references, the just-interned symbols are freed,
    // turning their hash table slots into tombstones.
    runtime.collect("test");
  };

  // Warm up so the table reaches its steady-state capacity for kBatch entries.
  internAndDropBatch();
  const size_t baseline = table.additionalMemorySize();

  for (size_t r = 0; r < kRounds; ++r) {
    internAndDropBatch();
  }

  // The live entry count per round is constant, so the footprint must stay
  // bounded. Without in-place rehashing it grows with the total number of
  // interned strings (tens of x over this loop).
  const size_t finalSize = table.additionalMemorySize();
  EXPECT_LE(finalSize, baseline * 2)
      << "IdentifierHashTable grew unboundedly with deleted entries: baseline="
      << baseline << " final=" << finalSize;
}
#endif

using IdentifierTableTest = RuntimeTestFixture;

TEST_F(IdentifierTableTest, DeletedEntryReuseDoesNotGrowHashTable) {
  constexpr uint32_t kCapacity = 16;

  IdentifierTable identifierTable;
  hermes::vm::detail::IdentifierHashTable hashTable{kCapacity};
  hashTable.setIdentifierTable(&identifierTable);

  ASCIIRef refs[] = {
      createASCIIRef("reuse"),
      createASCIIRef("live-1"),
      createASCIIRef("live-2"),
      createASCIIRef("live-3"),
  };
  SymbolID reusedID;
  for (size_t i = 0; i < std::size(refs); ++i) {
    SymbolID id = identifierTable.registerLazyIdentifier(runtime, refs[i]);
    uint32_t idx = hashTable.lookupString(refs[i]);
    hashTable.insert(idx, id);
    if (i == 0) {
      reusedID = id;
    }
  }

  // Repeated insertion and deletion at the same index should not grow the
  // capacity.
  for (size_t i = 0; i < kCapacity; ++i) {
    uint32_t idx = hashTable.lookupString(refs[0]);
    // remove() won't rehash the table.
    hashTable.remove(refs[0]);
    hashTable.insert(idx, reusedID);
  }

  EXPECT_EQ(kCapacity, hashTable.capacity());
}

TEST_F(IdentifierTableTest, NotUniquedSymbol) {
  struct : public Locals {
    PinnedValue<StringPrimitive> storageSlot;
    PinnedValue<StringPrimitive> id1;
    PinnedValue<SymbolID> sym;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto &idTable = runtime.getIdentifierTable();

  {
    ASCIIRef asdf{"asdf", 4};
    lv.id1.castAndSetHermesValue<StringPrimitive>(
        *StringPrimitive::create(runtime, asdf));
    lv.sym = *idTable.createNotUniquedSymbol(runtime, lv.id1);
    Handle<SymbolID> sym = lv.sym;
    EXPECT_TRUE((*sym).isNotUniqued());
    EXPECT_FALSE((*sym).isUniqued());
    MutableHandle<StringPrimitive> storage{lv.storageSlot};
    EXPECT_TRUE(idTable.getStringView(*sym, storage).equals(asdf));
  }
}

TEST(IdentifierTableDeathTest, LazyExternalSymbolTooBig) {
  auto fn = [] {
    auto rt = Runtime::create(
        RuntimeConfig::Builder().withGCConfig(kTestGCConfig).build());
    auto &runtime = *rt;
    GCScope gcScope{runtime};
    auto &idTable = runtime.getIdentifierTable();

    const size_t extSize =
        (1 << 24) +
        std::max<uint64_t>(
            kTestGCConfig.getMaxHeapSize(),
            toRValue(StringPrimitive::EXTERNAL_STRING_THRESHOLD));

    // A string of this size is definitely too big to be allocated.
    ASSERT_FALSE(runtime.getHeap().canAllocExternalMemory(extSize));

    std::string buf(extSize, '\0');
    ASCIIRef ref{buf.data(), extSize};

    SymbolID symbol = idTable.registerLazyIdentifier(runtime, ref);
    idTable.getStringPrim(runtime, symbol);
  };
  EXPECT_DEATH_IF_SUPPORTED(fn(), "Unhandled out of memory exception");
}

// Verifies that SymbolIDs are allocated consecutively, increasing from zero, as
// long as none have been freed.
TEST_F(IdentifierTableTest, ConsecutiveIncreasingSymbolIDAlloc) {
  IdentifierTable idTable;

  // Backing store for StringRefs
  std::vector<std::string> ascii;
  std::vector<std::u16string> utf16;

  for (size_t i = 0; i < 100; ++i) {
    std::stringstream ssa;
    ssa << "ascii-" << i;
    ascii.emplace_back(ssa.str());

    std::stringstream ssu;
    ssu << "utf16-" << i;
    auto abuf = ssu.str();
    std::u16string buf;
    convertUTF8WithSurrogatesToUTF16(
        std::back_inserter(buf), abuf.data(), abuf.data() + abuf.size());

    utf16.emplace_back(std::move(buf));
  }

  { // Add refs to the ID Table.  First time round, allocate all new IDs.
    size_t idx = 0;
    for (auto &s : ascii) {
      auto r = createASCIIRef(s.c_str());
      EXPECT_EQ(
          idTable.registerLazyIdentifier(runtime, r).unsafeGetIndex(), idx++)
          << "Uniqued ASCII First Round";
    }

    for (auto &s : utf16) {
      auto r = createUTF16Ref(s.c_str());
      EXPECT_EQ(
          idTable.registerLazyIdentifier(runtime, r).unsafeGetIndex(), idx++)
          << "Uniqued UTF16 First Round";
    }

    for (auto &s : ascii) {
      auto r = createASCIIRef(s.c_str());
      EXPECT_EQ(idTable.createNotUniquedLazySymbol(r).unsafeGetIndex(), idx++)
          << "Not Uniqued ASCII First Round";
    }
  }

  { // Next time around: The IDs should be the same as before for the uniqued
    // SymbolIDs.
    size_t idx = 0;
    for (auto &s : ascii) {
      auto r = createASCIIRef(s.c_str());
      EXPECT_EQ(
          idTable.registerLazyIdentifier(runtime, r).unsafeGetIndex(), idx++)
          << "Uniqued ASCII Second Round";
    }

    for (auto &s : utf16) {
      auto r = createUTF16Ref(s.c_str());
      EXPECT_EQ(
          idTable.registerLazyIdentifier(runtime, r).unsafeGetIndex(), idx++)
          << "Uniqued UTF16 Second Round";
    }

    // These symbols are not uniqued so they are going to get re-allocated every
    // time, although still linearly.
    idx = idTable.getSymbolsEnd();
    for (auto &s : ascii) {
      auto r = createASCIIRef(s.c_str());
      EXPECT_EQ(idTable.createNotUniquedLazySymbol(r).unsafeGetIndex(), idx++)
          << "Not Uniqued ASCII Second Round";
    }
  }
}

} // namespace
