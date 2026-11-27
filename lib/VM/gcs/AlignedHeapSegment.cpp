/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/AlignedHeapSegment.h"

#include "hermes/Support/OSCompat.h"
#include "hermes/VM/GCBase-inline.h"
#include "hermes/VM/GCBase.h"
#include "hermes/VM/GCCell.h"
#include "hermes/VM/HiddenClass.h"

#include "llvh/Support/MathExtras.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace hermes {
namespace vm {

#ifndef NDEBUG
/// Set the given range [start, end) to a dead value.
static void clearRange(char *start, char *end) {
#if LLVM_ADDRESS_SANITIZER_BUILD
  __asan_poison_memory_region(start, end - start);
#else
  std::memset(start, kInvalidHeapValue, end - start);
#endif
}
#endif

AlignedHeapSegment::AlignedHeapSegment(
    StorageProvider *provider,
    void *lowLim,
    size_t segmentSize)
    : provider_(provider), lowLim_(reinterpret_cast<char *>(lowLim)) {
  // Storage end must be page-aligned so that markUnused below stays in
  // segment.
  assert(
      ((reinterpret_cast<uintptr_t>(lowLim) + segmentSize) %
       oscompat::page_size()) == 0 &&
      "The higher limit must be page aligned");
  new (contents()) Contents(segmentSize);

  // Poison the padding array to detect any unintentional read/write.
  __asan_poison_memory_region(contents()->padding_, Contents::kPaddingSize);

#ifndef NDEBUG
  clearRange(start(), static_cast<char *>(lowLim_) + segmentSize);
#endif
}

void swap(AlignedHeapSegment &a, AlignedHeapSegment &b) {
  // Field lowLim_ and provider_ need to be swapped to make sure the storage of
  // a is not deleted when b is destroyed.
  std::swap(a.lowLim_, b.lowLim_);
  std::swap(a.provider_, b.provider_);
  std::swap(a.level_, b.level_);
}

AlignedHeapSegment::AlignedHeapSegment(AlignedHeapSegment &&other)
    : AlignedHeapSegment() {
  swap(*this, other);
}

AlignedHeapSegment &AlignedHeapSegment::operator=(AlignedHeapSegment &&other) {
  swap(*this, other);
  return *this;
}

AlignedHeapSegment::~AlignedHeapSegment() {
  if (lowLim() == nullptr) {
    return;
  }
  size_t segmentSize = contents()->getSegmentSize();
  contents()->~Contents();
  __asan_unpoison_memory_region(contents()->padding_, Contents::kPaddingSize);
  __asan_unpoison_memory_region(start(), segmentSize - kOffsetOfAllocRegion);

  if (provider_) {
    provider_->deleteStorage(lowLim_, segmentSize);
  }
}

/* static */
llvh::ErrorOr<JumboHeapSegment> JumboHeapSegment::create(
    StorageProvider *provider,
    const char *name,
    size_t segmentSize) {
  assert(
      segmentSize > kSegmentUnitSize &&
      "JumboHeapSegment size must be larger than kSegmentUnitSize");
  auto result = provider->newStorage(segmentSize, name);
  if (!result) {
    return result.getError();
  }
  assert(*result && "Heap segment storage allocation failure");
  return JumboHeapSegment{provider, *result, segmentSize};
}

llvh::ErrorOr<FixedSizeHeapSegment> FixedSizeHeapSegment::create(
    StorageProvider *provider,
    const char *name) {
  auto result = provider->newStorage(storageSize(), name);
  if (!result) {
    return result.getError();
  }
  assert(*result && "Heap segment storage allocation failure");
  return FixedSizeHeapSegment{provider, *result};
}

FixedSizeHeapSegment::FixedSizeHeapSegment(
    StorageProvider *provider,
    void *lowLim)
    : AlignedHeapSegment(provider, lowLim, kSize) {}

void FixedSizeHeapSegment::markUnused(char *start, char *end) {
  assert(
      !llvh::alignmentAdjustment(start, oscompat::page_size()) &&
      !llvh::alignmentAdjustment(end, oscompat::page_size()));
  assert(start <= end && "Unused region boundaries inverted");
  assert(lowLim() <= start && end <= hiLim() && "Unused region out-of-bounds");
#ifndef HERMES_ALLOW_HUGE_PAGES
  oscompat::vm_unused(start, end - start);
#endif
}

template <AdviseUnused MU>
void FixedSizeHeapSegment::setLevel(char *lvl) {
  assert(dbgContainsLevel(lvl));
  if (lvl < level_) {
#ifndef NDEBUG
    clearRange(lvl, level_);
#else
    if (MU == AdviseUnused::Yes) {
      const size_t PS = oscompat::page_size();
      auto nextPageAfter = reinterpret_cast<char *>(
          llvh::alignTo(reinterpret_cast<uintptr_t>(lvl), PS));
      auto nextPageBefore = reinterpret_cast<char *>(
          llvh::alignTo(reinterpret_cast<uintptr_t>(level_), PS));

      markUnused(nextPageAfter, nextPageBefore);
    }
#endif
  }
  level_ = lvl;
}

/// Explicit template instantiations for setLevel
template void FixedSizeHeapSegment::setLevel<AdviseUnused::Yes>(char *lvl);
template void FixedSizeHeapSegment::setLevel<AdviseUnused::No>(char *lvl);

template <AdviseUnused MU>
void FixedSizeHeapSegment::resetLevel() {
  setLevel<MU>(start());
}

/// Explicit template instantiations for resetLevel
template void FixedSizeHeapSegment::resetLevel<AdviseUnused::Yes>();
template void FixedSizeHeapSegment::resetLevel<AdviseUnused::No>();

void FixedSizeHeapSegment::setEffectiveEnd(char *effectiveEnd) {
  assert(
      start() <= effectiveEnd && effectiveEnd <= end() &&
      "Must be valid end for segment.");
  assert(level() <= effectiveEnd && "Must not set effective end below level");
  effectiveEnd_ = effectiveEnd;
  externalMemoryCharge_ = 0;
}

void FixedSizeHeapSegment::resetEffectiveEnd() {
  setEffectiveEnd(end());
}

void FixedSizeHeapSegment::chargeExternalMemory(size_t size) {
  const size_t charge = std::min(size, available());
  effectiveEnd_ -= charge;
  externalMemoryCharge_ += charge;
  assert(level() <= effectiveEnd_ && "Charged below the level of the segment");
}

void FixedSizeHeapSegment::capExternalMemoryCharge(size_t remainingSize) {
  // A credit that found less room than it asked for decreased the effective
  // end by less than its size, so the charge can be smaller than the external
  // memory still credited; it is never allowed to be larger.
  const size_t charge = std::min(externalMemoryCharge_, remainingSize);
  effectiveEnd_ += externalMemoryCharge_ - charge;
  externalMemoryCharge_ = charge;
  assert(effectiveEnd_ <= end() && "Gave back more room than was charged");
}

#ifndef NDEBUG
bool FixedSizeHeapSegment::dbgContainsLevel(const void *lvl) const {
  return contains(lvl) || lvl == hiLim();
}

bool FixedSizeHeapSegment::validPointer(const void *p) const {
  return start() <= p && p < level() &&
      static_cast<const GCCell *>(p)->isValid();
}

/* static */ void FixedSizeHeapSegment::checkUnwritten(char *start, char *end) {
#if !LLVM_ADDRESS_SANITIZER_BUILD && defined(HERMES_SLOW_DEBUG)
  // Check that the space was not written into.
  std::for_each(
      start, end, [](char value) { assert(value == kInvalidHeapValue); });
#endif
}
#endif // !NDEBUG

} // namespace vm
} // namespace hermes
