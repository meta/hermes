/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"

#if HERMESVM_JIT

#include "../../lib/VM/JIT/JitHandlers.h"
#include "VMRuntimeTestHelpers.h"

#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/JSObject.h"

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

using JitStringPrepTest = RuntimeTestFixture;

/// \return the symbol for \p s. Its handle stays in the current GCScope,
/// which keeps the symbol alive.
SymbolID sym(Runtime &runtime, const char *s) {
  return **runtime.getIdentifierTable().getSymbolHandle(
      runtime, createASCIIRef(s));
}

/// \return String.prototype's current hidden class. A raw pointer, valid
/// until the next allocation.
HiddenClass *protoClass(Runtime &runtime) {
  return vmcast<JSObject>(runtime.stringPrototype.getHermesValue())
      ->getClass(runtime);
}

/// \return the slot of \p name, which must be an own data property of
/// String.prototype, as the object's own lookup reports it.
SlotIndex ownSlot(Runtime &runtime, SymbolID name) {
  NamedPropertyDescriptor desc;
  bool found = JSObject::getOwnNamedDescriptor(
      Handle<JSObject>::vmcast(&runtime.stringPrototype), runtime, name, desc);
  EXPECT_TRUE(found);
  EXPECT_FALSE(desc.flags.accessor);
  return desc.slot;
}

/// Run \p code as a script; it must not throw.
void run(Runtime &runtime, const char *code) {
  GCScopeMarkerRAII marker{runtime};
  hermes::hbc::CompileFlags flags;
  ASSERT_EQ(
      ExecutionStatus::RETURNED, runtime.run(code, "", flags).getStatus());
}

/// A lazyId callback that assigns IDs from 100 up, as
/// Emitter::initHCLazyIDMayAlloc would but without allocating, and counts
/// its calls.
struct FakeIds {
  /// The next ID to assign.
  uint16_t next = 100;
  /// How many times the callback ran.
  unsigned calls = 0;
  /// Assign \p hc an ID if it has none. \return its ID.
  uint16_t operator()(HiddenClass *hc) {
    ++calls;
    if (!hc->getLazyJITId())
      hc->setLazyJITId(next++);
    return hc->getLazyJITId();
  }
};

// `length` is decided by name alone: Length, no lazy ID requested, the
// slot untouched. Fails if the name check is missing: String.prototype
// owns a data property `length` (PrimitiveBox.cpp:85-101), so the lookup
// would give Method.
TEST_F(JitStringPrepTest, Length) {
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 12345;
  EXPECT_EQ(
      JitByIdStringTier::Length,
      prepareByIdString(
          runtime,
          site,
          Predefined::getSymbolID(Predefined::length),
          ids,
          slot));
  EXPECT_EQ(JitByIdStringTier::Length, site.stringTier);
  EXPECT_EQ(0u, ids.calls);
  EXPECT_EQ(0u, (unsigned)site.stringProtoId);
  EXPECT_EQ(12345u, slot);
}

// The initial String.prototype: not a dictionary, with room for the 10
// properties the lit tests add at most; toString in a direct slot and
// indexOf out of line (review focus 2 relies on both); charCodeAt gives
// Method at the slot the object's own lookup reports, with the class's
// new ID. Fails if found data properties are not Method or the slot is
// not the class's.
TEST_F(JitStringPrepTest, MethodCharCodeAt) {
  ASSERT_FALSE(protoClass(runtime)->isDictionary());
  EXPECT_LE(
      protoClass(runtime)->getNumProperties() + 10,
      HiddenClass::kDictionaryThreshold);
  EXPECT_LT(
      ownSlot(runtime, Predefined::getSymbolID(Predefined::toString)),
      (SlotIndex)HERMESVM_DIRECT_PROPERTY_SLOTS);
  EXPECT_GE(
      ownSlot(runtime, Predefined::getSymbolID(Predefined::indexOf)),
      (SlotIndex)HERMESVM_DIRECT_PROPERTY_SLOTS);
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 0;
  SymbolID cc = Predefined::getSymbolID(Predefined::charCodeAt);
  EXPECT_EQ(
      JitByIdStringTier::Method,
      prepareByIdString(runtime, site, cc, ids, slot));
  EXPECT_EQ(JitByIdStringTier::Method, site.stringTier);
  EXPECT_EQ(ownSlot(runtime, cc), slot);
  EXPECT_EQ(1u, ids.calls);
  EXPECT_EQ(100u, (unsigned)site.stringProtoId);
  EXPECT_EQ(100u, (unsigned)protoClass(runtime)->getLazyJITId());
}

// A name String.prototype's class does not own: Absent, with the class's
// ID and property count. Fails if the count is not recorded.
TEST_F(JitStringPrepTest, Absent) {
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 0;
  EXPECT_EQ(
      JitByIdStringTier::Absent,
      prepareByIdString(
          runtime, site, sym(runtime, "noSuchStringProp"), ids, slot));
  EXPECT_NE(0u, site.stringProtoCount);
  EXPECT_EQ(protoClass(runtime)->getNumProperties(), site.stringProtoCount);
  EXPECT_EQ(100u, (unsigned)site.stringProtoId);
}

// An accessor of String.prototype: Ineligible, with the class's ID.
// Fails if accessors are taken as Method.
TEST_F(JitStringPrepTest, AccessorIsIneligible) {
  run(runtime,
      "Object.defineProperty(String.prototype, 'acc', "
      "{get: function () { return 1; }, configurable: true});");
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 0;
  EXPECT_EQ(
      JitByIdStringTier::Ineligible,
      prepareByIdString(runtime, site, sym(runtime, "acc"), ids, slot));
  EXPECT_EQ(100u, (unsigned)site.stringProtoId);
}

// No lazy ID available: Ineligible with ID 0, even for a data property.
// Fails if a zero ID is not checked.
TEST_F(JitStringPrepTest, NoIdIsIneligible) {
  auto noId = [](HiddenClass *) -> uint16_t { return 0; };
  JitByIdSiteRecord site;
  SlotIndex slot = 0;
  EXPECT_EQ(
      JitByIdStringTier::Ineligible,
      prepareByIdString(
          runtime,
          site,
          Predefined::getSymbolID(Predefined::charCodeAt),
          noId,
          slot));
  EXPECT_EQ(0u, (unsigned)site.stringProtoId);
}

// A no-cache dictionary String.prototype (a second deletion,
// HiddenClass.cpp:425-434) is not eligible: Ineligible, with the class's
// ID. Fails if eligibility is not checked.
TEST_F(JitStringPrepTest, NoCacheDictionaryIsIneligible) {
  run(runtime,
      "String.prototype.t1 = 1; delete String.prototype.t1; "
      "String.prototype.t2 = 1; delete String.prototype.t2;");
  ASSERT_TRUE(protoClass(runtime)->isDictionaryNoCache());
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 0;
  EXPECT_EQ(
      JitByIdStringTier::Ineligible,
      prepareByIdString(
          runtime,
          site,
          Predefined::getSymbolID(Predefined::charCodeAt),
          ids,
          slot));
  EXPECT_EQ(100u, (unsigned)site.stringProtoId);
}

// The lazyId callback is preparation's only GC point (spec: "GC during
// preparation"). This one collects before assigning. On the hsan tree
// (MallocGC + handle sanitization) the collection moves String.prototype's
// class and frees the old copy (MallocGC.cpp:65-102), so a class pointer
// held across the callback is a use after free that ASan reports. The
// callback roots its argument as initHCLazyIDMayAlloc does.
TEST_F(JitStringPrepTest, CollectingLazyId) {
  unsigned calls = 0;
  auto collecting = [this, &calls](HiddenClass *hc) -> uint16_t {
    ++calls;
    struct : public Locals {
      PinnedValue<HiddenClass> hc;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    lv.hc = hc;
    runtime.collect("JitStringPrepTest");
    lv.hc->setLazyJITId(77);
    return 77;
  };
  JitByIdSiteRecord site;
  SlotIndex slot = 0;
  SymbolID cc = Predefined::getSymbolID(Predefined::charCodeAt);
  EXPECT_EQ(
      JitByIdStringTier::Method,
      prepareByIdString(runtime, site, cc, collecting, slot));
  EXPECT_EQ(1u, calls);
  EXPECT_EQ(77u, (unsigned)site.stringProtoId);
  EXPECT_EQ(77u, (unsigned)protoClass(runtime)->getLazyJITId());
  EXPECT_EQ(ownSlot(runtime, cc), slot);
}

} // namespace

#endif // HERMESVM_JIT
