/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_PLATFORMINTL_PLATFORMINTL_H
#define HERMES_PLATFORMINTL_PLATFORMINTL_H

#ifdef HERMES_ENABLE_INTL
#include "hermes/VM/CallResult.h"
#include "hermes/VM/DecoratedObject.h"
#include "hermes/VM/Handle.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/StringPrimitive.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace hermes {
namespace platform_intl {

class Option {
 public:
  enum Kind { Bool, Number, String };

  Option(bool b) : kind_(Kind::Bool), num_(b ? 1.0 : 0.0) {}
  Option(double n) : kind_(Kind::Number), num_(n) {}
  Option(std::u16string s) : kind_(Kind::String), str_(std::move(s)) {}
  bool isBool() const {
    return kind_ == Kind::Bool;
  }
  bool isNumber() const {
    return kind_ == Kind::Number;
  }
  bool isString() const {
    return kind_ == Kind::String;
  }

  bool getBool() const {
    assert(isBool() && "Option is not a bool");
    return num_ == 1.0;
  }
  double getNumber() const {
    assert(isNumber() && "Option is not a number");
    return num_;
  }
  std::u16string &getString() {
    assert(isString() && "Option is not a string");
    return str_;
  }
  const std::u16string &getString() const {
    assert(isString() && "Option is not a string");
    return str_;
  }

  bool operator==(const Option &other) const {
    if (kind_ != other.kind_)
      return false;
    return isString() ? str_ == other.str_ : num_ == other.num_;
  }

 private:
  Kind kind_;
  // I am too lazy to make this a union.
  double num_;
  std::u16string str_;
};

using Options = std::unordered_map<std::u16string, Option>;
using Part = std::unordered_map<std::u16string, std::u16string>;

/// For functions which take locales, most of
/// https://tc39.es/ecma402/#sec-canonicalizelocalelist has been done
/// already.  All that remains is for each method to iterate the array
/// and do steps 7.c.v-vii for each element.
///
/// For functions which take options, it has been canonicalized into a
/// C++ map.
///
/// Formally, doing this canonicalization up front create an
/// observable difference between the order of operations in ECMA 402
/// and the actual operations, if the locals or options involve
/// getters or Proxies.  For now, we will ignore this inconsistency,
/// as anybody depending on it is trying too hard.

vm::CallResult<std::vector<std::u16string>> getCanonicalLocales(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales);
vm::CallResult<std::u16string> toLocaleLowerCase(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const std::u16string &str);
vm::CallResult<std::u16string> toLocaleUpperCase(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const std::u16string &str);

enum class NativeType {
  Collator,
  DateTimeFormat,
  NumberFormat,
};

class Collator : public vm::DecoratedObject::Decoration {
 protected:
  Collator();

 public:
  ~Collator() override;

  static constexpr NativeType getNativeType() {
    return NativeType::Collator;
  }

  static vm::CallResult<std::vector<std::u16string>> supportedLocalesOf(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;

  static vm::CallResult<std::unique_ptr<Collator>> create(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;
  Options resolvedOptions() noexcept;

  double compare(const std::u16string &x, const std::u16string &y) noexcept;

  /// Whether this collator uses a default locale that is no longer the
  /// platform's default. A collator created now from the same locales and
  /// options would then resolve to a different locale.
  bool defaultLocaleChanged() noexcept;
};

class DateTimeFormat : public vm::DecoratedObject::Decoration {
 protected:
  DateTimeFormat();

 public:
  ~DateTimeFormat() override;

  static constexpr NativeType getNativeType() {
    return NativeType::DateTimeFormat;
  }

  static vm::CallResult<std::vector<std::u16string>> supportedLocalesOf(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;

  static vm::CallResult<std::unique_ptr<DateTimeFormat>> create(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;
  Options resolvedOptions() noexcept;

  std::u16string format(double jsTimeValue) noexcept;
  std::vector<Part> formatToParts(double jsTimeValue) noexcept;
};

class NumberFormat : public vm::DecoratedObject::Decoration {
 protected:
  NumberFormat();

 public:
  ~NumberFormat() override;

  static constexpr NativeType getNativeType() {
    return NativeType::NumberFormat;
  }

  static vm::CallResult<std::vector<std::u16string>> supportedLocalesOf(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;

  static vm::CallResult<std::unique_ptr<NumberFormat>> create(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;
  Options resolvedOptions() noexcept;

  std::u16string format(double jsTimeValue) noexcept;
  std::vector<Part> formatToParts(double jsTimeValue) noexcept;
};

} // namespace platform_intl
} // namespace hermes
#endif

#endif
