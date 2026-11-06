/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JIT_X86_64_RUNTIMEOFFSETS_H
#define HERMES_VM_JIT_X86_64_RUNTIMEOFFSETS_H

#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/RuntimeModule.h"
#include "hermes/VM/sh_runtime.h"
#include "hermes/VM/sh_small_hermes_value.h"

namespace hermes {
namespace vm {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

struct RuntimeOffsets {
  static constexpr uint32_t stackPointer = offsetof(Runtime, stackPointer);
  static constexpr uint32_t registerStackEnd =
      offsetof(SHRuntime, registerStackEnd);
  static constexpr uint32_t currentFrame = offsetof(SHRuntime, currentFrame);
  static constexpr uint32_t currentIP = offsetof(Runtime, currentIP_);
  static constexpr uint32_t globalObject = offsetof(Runtime, global_);
  static constexpr uint32_t thrownValue = offsetof(Runtime, thrownValue_);
  static constexpr uint32_t identifierTable =
      offsetof(Runtime, identifierTable_);
  static constexpr uint32_t shLocals = offsetof(Runtime, shLocals);

  using BuiltinsType = decltype(Runtime::builtins_);
  static constexpr uint32_t builtins = offsetof(Runtime, builtins_);

  static constexpr uint32_t nativeStackHigh =
      offsetof(Runtime, overflowGuard_) +
      offsetof(StackOverflowGuard, nativeStackHigh);
  static constexpr uint32_t nativeStackSize =
      offsetof(Runtime, overflowGuard_) +
      offsetof(StackOverflowGuard, nativeStackSize);

  static constexpr uint32_t codeBlockJitPtr = offsetof(CodeBlock, JITCompiled_);
  static constexpr uint32_t jsFunctionCodeBlock =
      offsetof(JSFunction, codeBlock_);

  static constexpr uint32_t runtimeModuleModuleCache =
      offsetof(RuntimeModule, moduleExports_);
  using RuntimeModuleObjectLiteralHiddenClassesType =
      decltype(RuntimeModule::objectLiteralHiddenClasses_);
  static constexpr uint32_t runtimeModuleObjectLiteralHiddenClasses =
      offsetof(RuntimeModule, objectLiteralHiddenClasses_);

  /// Can't use offsetof here because KindAndSize uses bitfields.
  static constexpr uint32_t kindAndSizeKind = KindAndSize::kNumSizeBits / 8;

  static constexpr uint32_t boxedDoubleValue = offsetof(BoxedDouble, value_);

  static constexpr uint32_t stringPrimitiveLengthAndFlags =
      offsetof(StringPrimitive, lengthAndFlags_);
  static constexpr uint32_t stringPrimitiveLengthMask =
      StringPrimitive::LENGTH_MASK;

  static constexpr uint32_t hiddenClassLazyJITId =
      offsetof(HiddenClass, lazyJITId_);
  static constexpr uint32_t hiddenClassObjectParent =
      offsetof(HiddenClass, objectParent_);

  /// \name Heap value slot geometry and inline SmallHermesValue encoding.
  ///
  /// What an inline store site has to know about the SmallHermesValue a heap
  /// slot actually holds: how wide it is, and -- under boxed doubles -- how
  /// the 64-bit HermesValue the emitters carry in a GP register is encoded
  /// into one. Each constant is either derived from the runtime's own
  /// definition or pinned by a static_assert here, so a change on the
  /// runtime side breaks the build rather than the emitted code. See
  /// Emitter::emitSafeStoreOrSlow() and emit_shv_encode_or_slow().
  /// @{

  /// Size in bytes of one heap value slot: a direct property slot, an
  /// ArrayStorageSmall element, an environment slot. This is what every slot
  /// index has to be scaled by; it is 4 under compressed pointers and 8
  /// otherwise, so a hardcoded 8 is a mode bug.
  static constexpr uint32_t kSmallHermesValueSize =
      sizeof(SHGCSmallHermesValue);
  static_assert(
      kSmallHermesValueSize == sizeof(SmallHermesValue),
      "the C mirror and the C++ type must agree on the slot width");

  /// log2 of kSmallHermesValueSize: the scale factor of an x86 indexed
  /// addressing mode over an array of slots.
  static constexpr uint32_t kLogSmallHermesValueSize =
      kSmallHermesValueSize == 4 ? 2 : 3;
  static_assert(
      ((uint32_t)1 << kLogSmallHermesValueSize) == kSmallHermesValueSize,
      "a heap value slot must be 4 or 8 bytes wide");

#ifdef HERMESVM_BOXED_DOUBLES
  /// Width in bits of a whole SmallHermesValue, and of the tag that occupies
  /// its low bits. The C macros are what the C helpers in
  /// sh_small_hermes_value.h are written against, and the emitted encode has
  /// to agree with both them and the C++ class.
  static constexpr uint32_t kShvRawTypeBits = SH_SHV_RAW_TYPE_BITS;
  static constexpr uint32_t kShvTagBits = SH_SHV_TAG_BITS;
  static_assert(
      kShvRawTypeBits == HermesValue32::kNumRawTypeBits &&
          kShvRawTypeBits == kSmallHermesValueSize * 8,
      "SH_SHV_RAW_TYPE_BITS must be the full width of a slot");
  static_assert(
      kShvTagBits == HermesValue32::kNumTagBits,
      "SH_SHV_TAG_BITS must be the class's tag width");

  /// Width in bits of the payload of a non-pointer SmallHermesValue. A
  /// HermesValue is storable inline exactly when its low 64 - kShvValueBits
  /// bits are zero, which is the test the emitted encode makes.
  static constexpr uint32_t kShvValueBits = kShvRawTypeBits - kShvTagBits;
  static_assert(
      kShvValueBits == HermesValue32::kNumValueBits,
      "the value payload is everything above the tag");
  static_assert(
      kShvValueBits != 0 && kShvValueBits < 64,
      "the emitted shift that tests for inline representability must have a "
      "non-zero count, so that it sets the flags it is branched on");

  /// A SymbolID fits the payload with room to spare, which is what lets the
  /// emitted symbol encode be a plain shift with no range check of its own.
  /// HermesValue32::fromTagAndValue() asserts exactly this.
  static_assert(
      (uint64_t)SymbolID::LAST_INVALID_ID < ((uint64_t)1 << kShvValueBits),
      "a SymbolID must fit a SmallHermesValue's payload");

  /// The tag of an inline ("compressed HV64") value is zero, which is what
  /// makes the encode of one a plain right shift with nothing to or in --
  /// and what makes the "does it fit" test above a test of the low bits
  /// alone. HermesValue32::bitsToCompressedHV64() relies on the same fact.
  static_assert(
      (uint32_t)HermesValue32::Tag::CompressedHV64 == 0,
      "the compressed-HV64 tag must be zero");

  /// The constant offset between a HermesValue's pointer tag and the
  /// SmallHermesValue tag it encodes to. This is
  /// HermesValue32::encodeHermesValue()'s toHV32Tag(), written as an
  /// addition rather than a subtraction of its negation.
  static constexpr int32_t kShvPointerTagBias =
      (int32_t)HV32Tag_String - (int32_t)HVTag_Str;
  static_assert(
      (int32_t)HVTag_Str + kShvPointerTagBias == (int32_t)HV32Tag_String &&
          (int32_t)HVTag_BigInt + kShvPointerTagBias ==
              (int32_t)HV32Tag_BigInt &&
          (int32_t)HVTag_Object + kShvPointerTagBias == (int32_t)HV32Tag_Object,
      "one bias must map all three pointer tags");
#endif
  /// @}

#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
  static constexpr uint32_t runtimeHadesYGLevel =
      offsetof(Runtime, heap_.youngGen_.level_);
  static constexpr uint32_t runtimeHadesYGEnd =
      offsetof(Runtime, heap_.youngGen_.effectiveEnd_);
  static constexpr uint32_t runtimeHadesOGMarkingBarriers =
      offsetof(Runtime, heap_.ogMarkingBarriers_);

  /// \name Inline write barrier ("safe store") geometry.
  ///
  /// These describe the GC state the JIT reads inline in order to decide
  /// whether a heap store needs no barrier at all, needs only a card to be
  /// dirtied, or must go to the runtime helper. Together they are a
  /// maintenance contract with HadesGC: each is either derived with offsetof()
  /// below or pinned by a static_assert, so a change on the GC side breaks the
  /// build rather than the emitted code. See Emitter::emitSafeStoreOrSlow().
  /// @{

  /// The start of the young generation segment. Unlike the two YG fields
  /// above, this one can change identity and not just value:
  /// HadesGC::setYoungGen swaps in a whole different segment, so the emitted
  /// code loads it rather than baking it in.
  static constexpr uint32_t runtimeHadesYGStart =
      offsetof(Runtime, heap_.youngGen_.lowLim_);

  /// The start address of the segment currently being compacted, or
  /// kHadesNoCompactee when there is none.
  /// HadesGC::CompacteeState::contains() compares against exactly this field.
  static constexpr uint32_t runtimeHadesCompacteeStart =
      offsetof(Runtime, heap_.compactee_.start);

  /// The value runtimeHadesCompacteeStart holds when no compaction is in
  /// progress. Deliberately a non-null value that cannot be a segment start,
  /// which is what makes the inline "no compactee" test a plain compare.
  static constexpr uintptr_t kHadesNoCompactee =
      HadesGC::CompacteeState::kInvalidCompacteeStart;

  using SegmentContents = AlignedHeapSegment::Contents;

  /// Size and alignment of a heap segment unit. A pointer into a unit-sized
  /// segment is turned into its segment start by masking off the low
  /// kSegmentUnitSize-1 bits.
  static constexpr size_t kSegmentUnitSize =
      AlignedHeapSegment::kSegmentUnitSize;
  static_assert(
      kSegmentUnitSize && (kSegmentUnitSize & (kSegmentUnitSize - 1)) == 0,
      "segment unit size must be a power of two");

  /// Offset within a segment of the inline card status array. The emitted
  /// card-dirty store indexes off the segment start directly, so this must
  /// be 0.
  static constexpr uint32_t segmentInlineCards =
      offsetof(SegmentContents, inlineCardsArray_);
  static_assert(
      segmentInlineCards == 0,
      "the inline cards array must start at segment offset 0");

  /// log2 of the number of heap bytes covered by one card.
  static constexpr uint32_t kLogCardSize = SegmentContents::kLogCardSize;
  static_assert(kLogCardSize == 9, "512-byte cards");

  /// The card status byte value meaning "dirty".
  static constexpr uint8_t kCardDirty =
      (uint8_t)SegmentContents::CardStatus::Dirty;
  static_assert(kCardDirty == 1, "CardStatus::Dirty must be 1");

  /// Offset within a segment of its size, expressed as a multiple of
  /// kSegmentUnitSize. The inline card-dirty store is only valid in a segment
  /// whose size is exactly one unit, because only then does
  /// Contents::prefixHeader_.cards_ point at inlineCardsArray_ (see the
  /// Contents constructor); a larger, "jumbo" segment holds its card array
  /// out of line and its cells are barriered through
  /// dirtyCardForAddressInLargeObj() instead.
  static constexpr uint32_t segmentShiftedSize =
      offsetof(SegmentContents, prefixHeader_.segmentInfo_.shiftedSegmentSize);
  static_assert(
      sizeof(SHSegmentInfo::shiftedSegmentSize) == 2,
      "shiftedSegmentSize is compared as a 16-bit value");

  /// Every card index the emitted store can produce falls inside the
  /// allocation region, which starts well past the prefix of the card array
  /// that is repurposed to hold SHSegmentInfo and the out-of-line cards
  /// pointer.
  static_assert(
      (AlignedHeapSegment::kOffsetOfAllocRegion >> kLogCardSize) >=
          SegmentContents::kFirstUsedIndex,
      "a dirtied card must never land in the repurposed card table prefix");

  /// The largest allocation size, in bytes, at which a cell is guaranteed NOT
  /// to have been placed in a multi-unit JumboHeapSegment.
  ///
  /// Derivation: an ArrayStorageSmall is allocated with CanBeLarge::Yes
  /// (ArrayStorage.h), and the only two places such a cell can become large
  /// both spell the threshold the same way --
  /// HadesGC::allocSlow() sends `sz > FixedSizeHeapSegment::maxSize()` to
  /// OldGen::allocLarge(), and HadesGC::allocLongLived() allocates in a
  /// FixedSizeHeapSegment for anything `sz <= FixedSizeHeapSegment::maxSize()`.
  /// Everything at or below the bound therefore lives in a young-gen or
  /// old-gen FixedSizeHeapSegment, which is exactly one kSegmentUnitSize unit
  /// long. That is what the inline array store needs: it satisfies
  /// emitSafeStoreOrSlow()'s precondition that the slot address lies within
  /// the first unit of its segment, and it makes `loc & ~(unit-1)` the true
  /// segment start.
  ///
  /// This is a CELL size in bytes, not an element count, because the element
  /// count in ArrayStorageSmall::size does not bound the cell: capacity may
  /// be much larger than size (ArrayStorage grows geometrically and shrinking
  /// keeps the capacity), and it is capacity that the allocation size is
  /// derived from. The gate therefore reads the cell's own KindAndSize.
  static constexpr uint32_t kMaxInlineStorage = FixedSizeHeapSegment::maxSize();
  static_assert(
      kMaxInlineStorage < kSegmentUnitSize,
      "a fixed-size segment holds at most one unit");
  /// A cell larger than GCCell::maxNormalSize() stores 0 in its KindAndSize
  /// instead of its size, so the inline gate has to reject 0 as well. It can
  /// only do that soundly if every non-jumbo size is representable, which is
  /// what this pins.
  static_assert(
      kMaxInlineStorage <= GCCell::maxNormalSize(),
      "a non-jumbo cell must store its size inline");

  /// Width, in bits, of the size field of KindAndSize. The emitted gate reads
  /// it as a plain 32-bit load from offset 0 of the cell, which is only the
  /// size when this is 32.
  static constexpr uint32_t kindAndSizeNumSizeBits = KindAndSize::kNumSizeBits;
  /// @}
#endif

  /// \name Fast array element access.
  ///
  /// The ArrayImpl layout both the inline PutByVal fast array STORE tier
  /// (Emitter::emitPutByValFastArrayTier(), Hades-only: see the barrier
  /// geometry above) and the inline GetByVal fast array LOAD tier
  /// (Emitter::emitGetByValFastArrayTier()) read. Deliberately outside the
  /// Hades-only block above, unlike the store tier's other geometry: a load
  /// takes no write barrier, so its tier is emitted under every GC,
  /// MallocGC included, and needs these three fields regardless.
  /// @{
  static constexpr uint32_t arrayImplBeginIndex =
      offsetof(ArrayImpl, beginIndex_);
  static constexpr uint32_t arrayImplElemCount =
      offsetof(ArrayImpl, elemCount_);
  static constexpr uint32_t arrayImplIndexedStorage =
      offsetof(ArrayImpl, indexedStorage_);
  /// @}

  /// \name Indexed store class flags.
  ///
  /// Deliberately outside the Hades-only block above: these depend only on
  /// ClassFlags, and the inline typed-array store tier -- which needs no
  /// write barrier and is therefore emitted under every GC -- uses them too.
  /// @{

  /// Offset of the 16-bit ClassFlags word in a HiddenClass.
  static constexpr uint32_t hiddenClassFlags = offsetof(HiddenClass, flags_);

  /// Bit mask, within a HiddenClass's 16-bit ClassFlags word, of the three
  /// class flags the inline fast array store depends on, and the value that
  /// word must have under that mask: indexedStorage set,
  /// hasIndexLikeProperties clear and frozen clear. The first two together
  /// are JSObject::hasFastIndexProperties().
  ///
  /// Both are derived from the bitfield declaration rather than hardcoded, so
  /// reordering ClassFlags cannot silently change the emitted immediates.
  /// They are functions rather than constexpr constants because reading a
  /// union through a member other than the one written is not a constant
  /// expression in C++17. The values they must produce are pinned by an
  /// assert at the emission site.
  ///
  /// frozen has to be tested even though the indexed properties are fast:
  /// Object.freeze() changes neither indexedStorage nor
  /// hasIndexLikeProperties, and ArrayImpl::_setOwnIndexedImpl() refuses the
  /// write when the class is frozen.
  static uint32_t classFlagsFastArrayMask() {
    ClassFlags f;
    f.indexedStorage = 1;
    f.hasIndexLikeProperties = 1;
    f.frozen = 1;
    return f._flags;
  }
  static uint32_t classFlagsFastArrayValue() {
    ClassFlags f;
    f.indexedStorage = 1;
    return f._flags;
  }
  /// @}

  /// \name Typed array element store.
  ///
  /// The layout the inline PutByVal typed-array tier reads out of a
  /// JSTypedArrayBase and its JSArrayBuffer. See
  /// Emitter::emitPutByValTypedArrayTier(). Like the class flags above,
  /// these are unconditional: the tier stores no GC pointers and so exists
  /// under every GC.
  /// @{
  static constexpr uint32_t jsTypedArrayBaseBuffer =
      offsetof(JSTypedArrayBase, buffer_);
  static constexpr uint32_t jsTypedArrayBaseLength =
      offsetof(JSTypedArrayBase, length_);
  static constexpr uint32_t jsTypedArrayBaseOffset =
      offsetof(JSTypedArrayBase, offset_);
  static constexpr uint32_t jsArrayBufferData = offsetof(JSArrayBuffer, data_);
  /// @}

#ifndef NDEBUG
  static constexpr uint32_t runtimeDebugAllocCounter =
      offsetof(Runtime, heap_.debugAllocationCounter_);
  static constexpr uint32_t gcCellDebugAllocId =
      offsetof(GCCell, _debugAllocationId_);
  static constexpr uint32_t gcCellMagicValue = GCCell::kMagic;
#endif

  using IdentifierTableLookupEntryType = IdentifierTable::LookupEntry;
  using IdentifierTableLookupVectorType =
      decltype(IdentifierTable::lookupVector_);
  static constexpr uint32_t identifierTableLookupVector =
      offsetof(IdentifierTable, lookupVector_);
  static constexpr uint32_t identifierTableLookupEntrySize =
      sizeof(IdentifierTable::LookupEntry);
  static constexpr uint32_t identifierTableLookupEntryStrPrim =
      offsetof(IdentifierTable::LookupEntry, strPrim_);

  static constexpr uint32_t runtimeJitCounters =
      offsetof(Runtime, jitContext_.counters_);

  /// The full 64-bit parent cache epoch. Emitted GetById chain entries
  /// compare it with the epoch stored in their record entry.
  static constexpr uint32_t runtimeParentCacheEpoch =
      offsetof(Runtime, parentCacheEpoch_);
  static_assert(
      sizeof(Runtime::parentCacheEpoch_) == 8,
      "chain entries compare the full 64-bit parent cache epoch");
};

#pragma GCC diagnostic pop

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_JIT_X86_64_RUNTIMEOFFSETS_H
