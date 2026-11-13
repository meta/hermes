/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSLIB_RUNTIMECOMMONSTORAGE_H
#define HERMES_VM_JSLIB_RUNTIMECOMMONSTORAGE_H

#include "hermes/VM/JSLib/DateCache.h"
#ifdef HERMES_ENABLE_INTL
#include "hermes/Platform/Intl/PlatformIntl.h"
#endif

#include <memory>
#include <random>

namespace hermes {
namespace vm {

/// This struct provides a shared location for per-Runtime storage needs of
/// JSLib. Runtime owns and provides access to an instance of this class.
struct JSLibStorage {
  JSLibStorage();
  ~JSLibStorage();

  /// JSLibStorage is tied to a single Runtime, and should not be copied
  JSLibStorage(const JSLibStorage &) = delete;
  void operator=(const JSLibStorage &) = delete;

  /// PRNG used by Math.random()
  std::mt19937_64 randomEngine_;
  bool randomEngineSeeded_ = false;

  /// Time zone offset cache used in conversion between UTC and local time.
  LocalTimeOffsetCache localTimeOffsetCache;

#ifdef HERMES_ENABLE_INTL
  /// The collator String.prototype.localeCompare created last, with the
  /// normalized locales and options it was created from.
  struct {
    std::vector<std::u16string> locales;
    platform_intl::Options options;
    std::unique_ptr<platform_intl::Collator> collator;
  } localeCompareCollator;
#endif
};

} // namespace vm
} // namespace hermes

#endif
