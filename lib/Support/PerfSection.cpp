/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Support/PerfSection.h" // @manual

#if defined(HERMES_USE_FBSYSTRACE) || defined(HERMESVM_PLATFORM_LOGGING)

#ifdef HERMES_USE_FBSYSTRACE
#include "fbsystrace.h"
#include "fbsystrace_tags.h"
#endif

#include "hermes/Platform/Logging.h"
#include "hermes/Support/SNPrintfBuf.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

namespace hermes {

using KVPair = std::pair<std::string, std::string>;

PerfSection::PerfSection(const char *name, const char *category)
    :
#ifdef HERMESVM_PLATFORM_LOGGING
      name_(name),
      category_(category),
#endif
      enabled_(false),
      argValues_(0) {
#ifdef HERMESVM_PLATFORM_LOGGING
  enabled_ = true;
  if (category_) {
    hermesLog("Hermes", "%s[%s] BEGIN.", name_, category_);
  } else {
    hermesLog("Hermes", "%s BEGIN.", name_);
  }
#endif
#ifdef HERMES_USE_FBSYSTRACE
  if (fbsystrace_is_tracing(TRACE_TAG_JS_VM)) {
    enabled_ = true;
    fbsystrace_begin_section(TRACE_TAG_JS_VM, name);
  }
#endif
}

void PerfSection::addArg(const char *argName, size_t value) {
  if (!enabled_)
    return;
  // Note that if the argument has already been set, we overwrite it.
  freeDataIfExists(argName);
  auto &val = argValues_[argName];
  val.type = ArgType::SIZE_T;
  val.value.sz_t = value;
}

void PerfSection::addArgD(const char *argName, double d) {
  if (!enabled_)
    return;
  // Note that if the argument has already been set, we overwrite it.
  freeDataIfExists(argName);
  auto &val = argValues_[argName];
  val.type = ArgType::DOUBLE;
  val.value.d = d;
}

void PerfSection::addArg(
    const char *argName,
    const llvh::StringRef value,
    bool doCopy) {
  if (!enabled_)
    return;
  freeDataIfExists(argName);
  auto &val = argValues_[argName];
  val.type = ArgType::STRINGREF;

  const char *data;
  if (doCopy) {
    char *copy = new char[value.size()];
    std::memcpy(copy, value.data(), value.size());
    data = copy;
  } else {
    data = value.data();
  }
  val.value.stringref = {data, value.size(), doCopy};
}

PerfSection::~PerfSection() {
  if (!enabled_)
    return;
  if (argValues_.empty()) {
#ifdef HERMES_USE_FBSYSTRACE
    fbsystrace_end_section(TRACE_TAG_JS_VM);
#endif
#ifdef HERMESVM_PLATFORM_LOGGING
    if (category_) {
      hermesLog("Hermes", "%s[%s] END.", name_, category_);
    } else {
      hermesLog("Hermes", "%s END.", name_);
    }
#endif
    return;
  }

  // First, transform the ArgValue parts of argValues_ into strings.
  std::vector<KVPair> kvPairs(argValues_.size());
  std::transform(
      argValues_.begin(),
      argValues_.end(),
      kvPairs.begin(),
      [](const std::pair<std::string, ArgValue> &pair) -> KVPair {
        // If more elements are added to ArgType, more cases must be added to
        // this switch.
        switch (pair.second.type) {
          case ArgType::SIZE_T: {
            SNPrintfBuf buf(20);
            buf.printf("%zu", pair.second.value.sz_t);
            // Note that KVPair has std::string elements, so the
            // c_str() is copied.
            return KVPair{pair.first, buf.c_str()};
          }
          case ArgType::DOUBLE: {
            SNPrintfBuf buf(20);
            buf.printf("%f", pair.second.value.d);
            // Note that KVPair has std::string elements, so the
            // c_str() is copied.
            return KVPair{pair.first, buf.c_str()};
          }
          case ArgType::STRINGREF:
            return KVPair{
                pair.first,
                std::string(
                    pair.second.value.stringref.data,
                    pair.second.value.stringref.size)};
          default:
            llvm_unreachable("Unhandled argtype");
        }
      });
#ifdef HERMES_USE_FBSYSTRACE
  // To output via fbsystrace, we must further translate into
  // FBSystraceSectionArg format.
  auto argArr = std::unique_ptr<FbSystraceSectionArg[]>{
      new FbSystraceSectionArg[argValues_.size()]};
  std::transform(
      kvPairs.begin(), kvPairs.end(), argArr.get(), [](const KVPair &kvPair) {
        return FbSystraceSectionArg{
            kvPair.first.c_str(),
            static_cast<int>(kvPair.first.size()),
            kvPair.second.c_str(),
            static_cast<int>(kvPair.second.size()),
        };
      });
  fbsystrace_end_section_with_args(
      TRACE_TAG_JS_VM, argValues_.size(), argArr.get());
#endif
#ifdef HERMESVM_PLATFORM_LOGGING
  SNPrintfBuf buf(1000);
  bool first = true;
  for (const KVPair &kvPair : kvPairs) {
    if (first) {
      first = false;
    } else {
      buf.printf(", ");
    }
    buf.printf("%s: %s", kvPair.first.c_str(), kvPair.second.c_str());
  }

  if (category_) {
    hermesLog("Hermes", "%s[%s] END: %s.", name_, category_, buf.c_str());
  } else {
    hermesLog("Hermes", "%s END: %s.", name_, buf.c_str());
  }
#endif
  for (auto &arg : argValues_) {
    arg.second.freeDependencies();
  }
}

} // namespace hermes

#endif // defined(HERMES_USE_FBSYSTRACE) ||
       // defined(HERMESVM_PLATFORM_LOGGING)
