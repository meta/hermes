/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES5.1 15.9 Initialize the Date constructor.
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/Support/OSCompat.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/JSLib/DateUtil.h"
#include "hermes/VM/JSLib/JSLibStorage.h"
#include "hermes/VM/Operations.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
/// Date.

namespace {
struct ToStringOptions {
  /// Converts a time \p t that has been adjusted by \p tza from UTC,
  /// and appends the resultant string into \p buf.
  /// \param t the local time since Jan 1 1970 UTC.
  /// \param tza the offset from UTC that \p t has been adjusted by.
  /// \param buf[out] the buffer into which to output the result string.
  void (*toStringFn)(double t, double tza, llvh::SmallVectorImpl<char> &buf);
  bool isUTC;
  /// Throw if the internal value of this Date is not finite.
  bool throwOnError;
};

struct ToLocaleStringOptions {
  /// Converts a time \p t that has been adjusted by \p tza from UTC,
  /// and appends the resultant string into \p buf.
  /// \param t the local time since Jan 1 1970 UTC.
  /// \param locale the locale to convert in (the current locale).
  /// \param buf[out] the buffer into which to output the result string.
  void (*toStringFn)(double t, llvh::SmallVectorImpl<char16_t> &buf);
};

struct GetterOptions {
  enum Field {
    FULL_YEAR,
    YEAR,
    MONTH,
    DATE,
    DAY,
    HOURS,
    MINUTES,
    SECONDS,
    MILLISECONDS,
    TIMEZONE_OFFSET,
  };
  Field field;
  bool isUTC;
};
} // namespace

enum class ToStringKind {
  DatetimeToString,
  DateToString,
  TimeToString,
  ISOToString,
  UTCToString,
  NumKinds
};

enum class ToLocaleStringKind {
  DatetimeToLocaleString,
  DateToLocaleString,
  TimeToLocaleString,
  NumKinds
};

enum class GetterKind {
  GetFullYear,
  GetYear,
  GetMonth,
  GetDate,
  GetDay,
  GetHours,
  GetMinutes,
  GetSeconds,
  GetMilliseconds,
  GetUTCFullYear,
  GetUTCMonth,
  GetUTCDate,
  GetUTCDay,
  GetUTCHours,
  GetUTCMinutes,
  GetUTCSeconds,
  GetUTCMilliseconds,
  GetTimezoneOffset,
  NumKinds
};

HermesValue createDateConstructor(Runtime &runtime) {
  auto datePrototype = Handle<JSObject>::vmcast(&runtime.datePrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Date),
      dateConstructor_RJS,
      datePrototype,
      7,
      lv.cons);

  // Date.prototype.xxx() methods.
  defineMethod(
      runtime,
      datePrototype,
      Predefined::getSymbolID(Predefined::valueOf),
      nullptr,
      datePrototypeGetTime,
      0);
  defineMethod(
      runtime,
      datePrototype,
      Predefined::getSymbolID(Predefined::getTime),
      nullptr,
      datePrototypeGetTime,
      0);

  auto defineToStringMethod = [&](SymbolID name, ToStringKind kind) {
    defineMethod(
        runtime,
        datePrototype,
        name,
        (void *)kind,
        datePrototypeToStringHelper,
        0);
  };

  defineToStringMethod(
      Predefined::getSymbolID(Predefined::toString),
      ToStringKind::DatetimeToString);
  defineToStringMethod(
      Predefined::getSymbolID(Predefined::toDateString),
      ToStringKind::DateToString);
  defineToStringMethod(
      Predefined::getSymbolID(Predefined::toTimeString),
      ToStringKind::TimeToString);
  defineToStringMethod(
      Predefined::getSymbolID(Predefined::toISOString),
      ToStringKind::ISOToString);
  defineToStringMethod(
      Predefined::getSymbolID(Predefined::toUTCString),
      ToStringKind::UTCToString);
  defineToStringMethod(
      Predefined::getSymbolID(Predefined::toGMTString),
      ToStringKind::UTCToString);

  auto defineToLocaleStringMethod = [&](SymbolID name,
                                        ToLocaleStringKind kind) {
    defineMethod(
        runtime,
        datePrototype,
        name,
        (void *)kind,
        datePrototypeToLocaleStringHelper,
        0);
  };

  defineToLocaleStringMethod(
      Predefined::getSymbolID(Predefined::toLocaleString),
      ToLocaleStringKind::DatetimeToLocaleString);
  defineToLocaleStringMethod(
      Predefined::getSymbolID(Predefined::toLocaleDateString),
      ToLocaleStringKind::DateToLocaleString);
  defineToLocaleStringMethod(
      Predefined::getSymbolID(Predefined::toLocaleTimeString),
      ToLocaleStringKind::TimeToLocaleString);

  auto defineGetterMethod = [&](SymbolID name, GetterKind kind) {
    defineMethod(
        runtime,
        datePrototype,
        name,
        (void *)kind,
        datePrototypeGetterHelper,
        0);
  };

  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getFullYear),
      GetterKind::GetFullYear);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getYear), GetterKind::GetYear);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getMonth), GetterKind::GetMonth);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getDate), GetterKind::GetDate);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getDay), GetterKind::GetDay);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getHours), GetterKind::GetHours);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getMinutes), GetterKind::GetMinutes);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getSeconds), GetterKind::GetSeconds);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getMilliseconds),
      GetterKind::GetMilliseconds);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getUTCFullYear),
      GetterKind::GetUTCFullYear);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getUTCMonth),
      GetterKind::GetUTCMonth);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getUTCDate), GetterKind::GetUTCDate);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getUTCDay), GetterKind::GetUTCDay);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getUTCHours),
      GetterKind::GetUTCHours);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getUTCMinutes),
      GetterKind::GetUTCMinutes);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getUTCSeconds),
      GetterKind::GetUTCSeconds);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getUTCMilliseconds),
      GetterKind::GetUTCMilliseconds);
  defineGetterMethod(
      Predefined::getSymbolID(Predefined::getTimezoneOffset),
      GetterKind::GetTimezoneOffset);

  defineMethod(
      runtime,
      datePrototype,
      Predefined::getSymbolID(Predefined::setTime),
      nullptr,
      datePrototypeSetTime_RJS,
      1);

  auto defineSetterMethod =
      [&](SymbolID name, uint32_t length, bool isUTC, NativeFunctionPtr func) {
        defineMethod(
            runtime,
            datePrototype,
            name,
            reinterpret_cast<void *>(isUTC),
            func,
            length);
      };

  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setMilliseconds),
      1,
      false,
      datePrototypeSetMilliseconds_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setUTCMilliseconds),
      1,
      true,
      datePrototypeSetMilliseconds_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setSeconds),
      2,
      false,
      datePrototypeSetSeconds_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setUTCSeconds),
      2,
      true,
      datePrototypeSetSeconds_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setMinutes),
      3,
      false,
      datePrototypeSetMinutes_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setUTCMinutes),
      3,
      true,
      datePrototypeSetMinutes_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setHours),
      4,
      false,
      datePrototypeSetHours_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setUTCHours),
      4,
      true,
      datePrototypeSetHours_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setDate),
      1,
      false,
      datePrototypeSetDate_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setUTCDate),
      1,
      true,
      datePrototypeSetDate_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setMonth),
      2,
      false,
      datePrototypeSetMonth_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setUTCMonth),
      2,
      true,
      datePrototypeSetMonth_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setFullYear),
      3,
      false,
      datePrototypeSetFullYear_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setUTCFullYear),
      3,
      true,
      datePrototypeSetFullYear_RJS);
  defineSetterMethod(
      Predefined::getSymbolID(Predefined::setYear),
      1,
      false,
      datePrototypeSetYear_RJS);

  defineMethod(
      runtime,
      datePrototype,
      Predefined::getSymbolID(Predefined::toJSON),
      nullptr,
      datePrototypeToJSON_RJS,
      1);

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  (void)defineMethod(
      runtime,
      datePrototype,
      Predefined::getSymbolID(Predefined::SymbolToPrimitive),
      Predefined::getSymbolID(Predefined::squareSymbolToPrimitive),
      nullptr,
      datePrototypeSymbolToPrimitive,
      1,
      dpf);

  // Date.xxx() methods.
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::parse),
      nullptr,
      dateParse_RJS,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::UTC),
      nullptr,
      dateUTC_RJS,
      7);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::now),
      nullptr,
      dateNow,
      0);

  return lv.cons.getHermesValue();
}

/// Takes \p args in UTC time of the form:
/// (year, month, [, date [, hours [, minutes [, seconds [, ms]]]]])
/// and returns the unclipped time in milliseconds since Jan 1 1970 UTC.
static CallResult<double> makeTimeFromArgs_RJS(
    Runtime &runtime,
    NativeArgs args) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto argCount = args.getArgCount();

  // General case: read all fields in and compute timestamp.
  enum fieldname { y, m, dt, h, min, s, milli, yr, FIELD_COUNT };

  // Initialize to default fields and update them in loop if necessary.
  double fields[FIELD_COUNT] = {nan, nan, 1, 0, 0, 0, 0};

  for (size_t i = 0; i < std::min(7u, argCount); ++i) {
    GCScopeMarkerRAII marker{runtime};
    auto res = toNumber_RJS(runtime, args.getArgHandle(i));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    fields[i] = res->getNumber();
  }

  // Years between 0 and 99 are treated as offsets from 1900.
  double yint = std::trunc(fields[y]);
  if (!std::isnan(fields[y]) && 0 <= yint && yint <= 99) {
    fields[yr] = 1900 + yint;
  } else {
    fields[yr] = fields[y];
  }
  return makeDate(
      makeDay(fields[yr], fields[m], fields[dt]),
      makeTime(fields[h], fields[min], fields[s], fields[milli]));
}

CallResult<HermesValue> dateConstructor_RJS(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (!args.isConstructorCall()) {
    llvh::SmallString<32> str{};
    double t = curTime();
    double local =
        localTime(t, runtime.getJSLibStorage()->localTimeOffsetCache);
    dateTimeString(local, local - t, str);
    return runtime.ignoreAllocationFailure(
        StringPrimitive::create(runtime, str));
  }

  struct : public Locals {
    PinnedValue<> primitiveValue;
    PinnedValue<JSObject> selfParent;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  uint32_t argCount = args.getArgCount();
  double finalDate;
  if (argCount == 0) {
    // No arguments, just set it to the current time.
    finalDate = curTime();
  } else if (argCount == 1) {
    if (auto *dateArg = dyn_vmcast<JSDate>(args.getArg(0))) {
      // No handle needed here because we just retrieve a double.
      NoAllocScope noAlloc(runtime);
      finalDate = dateArg->getPrimitiveValue();
    } else {
      // Parse the argument if it's a string, else just convert to number.
      auto res =
          toPrimitive_RJS(runtime, args.getArgHandle(0), PreferredType::NONE);
      if (res == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.primitiveValue = res.getValue();

      if (lv.primitiveValue->isString()) {
        // Call the String -> Date parsing function.
        finalDate = timeClip(parseDate(
            StringPrimitive::createStringView(
                runtime, Handle<StringPrimitive>::vmcast(&lv.primitiveValue)),
            runtime.getJSLibStorage()->localTimeOffsetCache));
      } else {
        auto numRes = toNumber_RJS(runtime, lv.primitiveValue);
        if (numRes == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
        finalDate = timeClip(numRes->getNumber());
      }
    }
  } else {
    // General case: read all fields in and compute timestamp.
    CallResult<double> cr{0};
    cr = makeTimeFromArgs_RJS(runtime, args);
    if (cr == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // makeTimeFromArgs interprets arguments as UTC.
    // We want them as local time, so pretend that they are,
    // and call utcTime to get the final UTC value we want to store.
    finalDate =
        timeClip(utcTime(*cr, runtime.getJSLibStorage()->localTimeOffsetCache));
  }

  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          runtime.dateConstructor.getHermesValue().getRaw())) {
    return JSDate::create(runtime, finalDate).getHermesValue();
  }
  CallResult<PseudoHandle<JSObject>> thisParentRes =
      NativeConstructor::parentForNewThis_RJS(
          runtime,
          Handle<Callable>::vmcast(&args.getNewTarget()),
          runtime.datePrototype);
  if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.selfParent = std::move(*thisParentRes);
  return JSDate::create(runtime, finalDate, lv.selfParent).getHermesValue();
}

CallResult<HermesValue> dateParse_RJS(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> stringValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto res = toString_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.stringValue = std::move(*res);
  return HermesValue::encodeTrustedNumberValue(timeClip(parseDate(
      StringPrimitive::createStringView(runtime, lv.stringValue),
      runtime.getJSLibStorage()->localTimeOffsetCache)));
}

CallResult<HermesValue> dateUTC_RJS(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // With less than 2 arguments, this is implementation-dependent behavior.
  // We define the behavior that test262 expects here.
  if (args.getArgCount() == 0) {
    return HermesValue::encodeNaNValue();
  }
  if (args.getArgCount() == 1) {
    auto res = toNumber_RJS(runtime, args.getArgHandle(0));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    double y = res->getNumber();
    return HermesValue::encodeTrustedNumberValue(
        timeClip(makeDate(makeDay(y, 0, 1), makeTime(0, 0, 0, 0))));
  }
  CallResult<double> cr{0};
  cr = makeTimeFromArgs_RJS(runtime, args);
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeTrustedNumberValue(timeClip(*cr));
}

CallResult<HermesValue> dateNow(void *, Runtime &runtime) {
  return HermesValue::encodeTrustedNumberValue(curTime());
}

CallResult<HermesValue> datePrototypeToStringHelper(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  static ToStringOptions toStringOptions[] = {
      {dateTimeString, false, false},
      {dateString, false, false},
      {timeTZString, false, false},
      {datetimeToISOString, true, true},
      {dateTimeUTCString, true, false},
  };
  assert(
      (uint64_t)ctx < (uint64_t)ToStringKind::NumKinds &&
      "dataPrototypeToString with wrong kind as context");
  ToStringOptions *opts = &toStringOptions[(uint64_t)ctx];
  auto *date = dyn_vmcast<JSDate>(args.getThisArg());
  if (!date) {
    return runtime.raiseTypeError(
        "Date.prototype.toString() called on non-Date object");
  }
  double t = date->getPrimitiveValue();
  if (!std::isfinite(t)) {
    if (opts->throwOnError) {
      return runtime.raiseRangeError("Date value out of bounds");
    }
    // "Invalid Date" in non-finite or NaN cases.
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::InvalidDate));
  }
  llvh::SmallString<32> str{};
  if (!opts->isUTC) {
    double local =
        localTime(t, runtime.getJSLibStorage()->localTimeOffsetCache);
    opts->toStringFn(local, local - t, str);
  } else {
    opts->toStringFn(t, 0, str);
  }
  return runtime.ignoreAllocationFailure(StringPrimitive::create(runtime, str));
}

CallResult<HermesValue> datePrototypeToLocaleStringHelper(
    void *ctx,
    Runtime &runtime) {
  assert(
      (uint64_t)ctx < (uint64_t)ToLocaleStringKind::NumKinds &&
      "dataPrototypeToLocaleString with wrong kind as context");
#ifdef HERMES_ENABLE_INTL
  static NativeFunctionPtr toLocaleStringFunctions[] = {
      intlDatePrototypeToLocaleString,
      intlDatePrototypeToLocaleDateString,
      intlDatePrototypeToLocaleTimeString,
  };
  assert(
      sizeof(toLocaleStringFunctions) / sizeof(toLocaleStringFunctions[0]) ==
          (size_t)ToLocaleStringKind::NumKinds &&
      "toLocaleStringFunctions has wrong number of elements");
  return toLocaleStringFunctions[(uint64_t)ctx](
      /* unused */ ctx, runtime);
#else
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  static ToLocaleStringOptions toLocaleStringOptions[] = {
      {datetimeToLocaleString},
      {dateToLocaleString},
      {timeToLocaleString},
  };
  assert(
      sizeof(toLocaleStringOptions) / sizeof(toLocaleStringOptions[0]) ==
          (size_t)ToLocaleStringKind::NumKinds &&
      "toLocaleStringOptions has wrong number of elements");
  ToLocaleStringOptions *opts = &toLocaleStringOptions[(uint64_t)ctx];
  auto *date = dyn_vmcast<JSDate>(args.getThisArg());
  if (!date) {
    return runtime.raiseTypeError(
        "Date.prototype.toLocaleString() called on non-Date object");
  }
  double t = date->getPrimitiveValue();
  if (!std::isfinite(t)) {
    // "Invalid Date" in non-finite or NaN cases.
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::InvalidDate));
  }
  SmallU16String<128> str{};

  opts->toStringFn(t, str);
  return StringPrimitive::create(runtime, str);
#endif
}

CallResult<HermesValue> datePrototypeGetTime(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto *date = dyn_vmcast<JSDate>(args.getThisArg());
  if (!date) {
    return runtime.raiseTypeError(
        "Date.prototype.getTime() called on non-Date object");
  }

  return HermesValue::encodeTrustedNumberValue(date->getPrimitiveValue());
}

CallResult<HermesValue> datePrototypeGetterHelper(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  static GetterOptions getterOptions[] = {
      {GetterOptions::Field::FULL_YEAR, false},
      {GetterOptions::Field::YEAR, false},
      {GetterOptions::Field::MONTH, false},
      {GetterOptions::Field::DATE, false},
      {GetterOptions::Field::DAY, false},
      {GetterOptions::Field::HOURS, false},
      {GetterOptions::Field::MINUTES, false},
      {GetterOptions::Field::SECONDS, false},
      {GetterOptions::Field::MILLISECONDS, false},
      {GetterOptions::Field::FULL_YEAR, true},
      {GetterOptions::Field::MONTH, true},
      {GetterOptions::Field::DATE, true},
      {GetterOptions::Field::DAY, true},
      {GetterOptions::Field::HOURS, true},
      {GetterOptions::Field::MINUTES, true},
      {GetterOptions::Field::SECONDS, true},
      {GetterOptions::Field::MILLISECONDS, true},
      {GetterOptions::Field::TIMEZONE_OFFSET, false},
  };
  assert(
      (uint64_t)ctx < (uint64_t)GetterKind::NumKinds &&
      "datePropertyGetterHelper with wrong kind as context");
  GetterOptions *opts = &getterOptions[(uint64_t)ctx];
  auto *date = dyn_vmcast<JSDate>(args.getThisArg());
  if (!date) {
    return runtime.raiseTypeError(
        "Date.prototype.toString() called on non-Date object");
  }
  double t = date->getPrimitiveValue();
  if (std::isnan(t)) {
    return HermesValue::encodeNaNValue();
  }

  // Store the original value of t to be used in offset calculations.
  double utc = t;
  if (!opts->isUTC) {
    t = localTime(t, runtime.getJSLibStorage()->localTimeOffsetCache);
  }

  double result{std::numeric_limits<double>::quiet_NaN()};
  switch (opts->field) {
    case GetterOptions::Field::FULL_YEAR:
      result = yearFromTime(t);
      break;
    case GetterOptions::Field::YEAR:
      result = yearFromTime(t) - 1900;
      break;
    case GetterOptions::Field::MONTH:
      result = monthFromTime(t);
      break;
    case GetterOptions::Field::DATE:
      result = dateFromTime(t);
      break;
    case GetterOptions::Field::DAY:
      result = weekDay(t);
      break;
    case GetterOptions::Field::HOURS:
      result = hourFromTime(t);
      break;
    case GetterOptions::Field::MINUTES:
      result = minFromTime(t);
      break;
    case GetterOptions::Field::SECONDS:
      result = secFromTime(t);
      break;
    case GetterOptions::Field::MILLISECONDS:
      result = msFromTime(t);
      break;
    case GetterOptions::Field::TIMEZONE_OFFSET:
      result = (utc - t) / MS_PER_MINUTE;
      break;
  }
  return HermesValue::encodeTrustedNumberValue(result);
}

/// Set the [[PrimitiveValue]] to the given time.
CallResult<HermesValue> datePrototypeSetTime_RJS(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSDate>();
  if (!self) {
    return runtime.raiseTypeError(
        "Date.prototype.setTime() called on non-Date object");
  }
  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double t = timeClip(res->getNumber());
  self->setPrimitiveValue(t);
  return HermesValue::encodeTrustedNumberValue(t);
}

/// Set the milliseconds as provided and return the new time value.
CallResult<HermesValue> datePrototypeSetMilliseconds_RJS(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  bool isUTC = static_cast<bool>(ctx);
  auto self = args.dyncastThis<JSDate>();
  if (!self) {
    return runtime.raiseTypeError(
        "Date.prototype.setMilliseconds() called on non-Date object");
  }
  double t = self->getPrimitiveValue();
  auto &localTimeOffsetCache = runtime.getJSLibStorage()->localTimeOffsetCache;
  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (std::isnan(t)) {
    return HermesValue::encodeTrustedNumberValue(
        std::numeric_limits<double>::quiet_NaN());
  }
  if (!isUTC) {
    t = localTime(t, localTimeOffsetCache);
  }
  double ms = res->getNumber();
  double date = makeDate(
      day(t), makeTime(hourFromTime(t), minFromTime(t), secFromTime(t), ms));
  double utcT =
      !isUTC ? timeClip(utcTime(date, localTimeOffsetCache)) : timeClip(date);
  self->setPrimitiveValue(utcT);
  return HermesValue::encodeTrustedNumberValue(utcT);
}

/// Takes 2 arguments: seconds, milliseconds.
/// Set the seconds, optionally milliseconds, and return the new time.
CallResult<HermesValue> datePrototypeSetSeconds_RJS(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  bool isUTC = static_cast<bool>(ctx);
  auto self = args.dyncastThis<JSDate>();
  if (!self) {
    return runtime.raiseTypeError(
        "Date.prototype.setSeconds() called on non-Date object");
  }
  double t = self->getPrimitiveValue();
  auto &localTimeOffsetCache = runtime.getJSLibStorage()->localTimeOffsetCache;
  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double s = res->getNumber();
  double milli;
  if (args.getArgCount() >= 2) {
    res = toNumber_RJS(runtime, args.getArgHandle(1));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    milli = res->getNumber();
  }
  if (std::isnan(t)) {
    return HermesValue::encodeTrustedNumberValue(
        std::numeric_limits<double>::quiet_NaN());
  }
  if (!isUTC) {
    t = localTime(t, localTimeOffsetCache);
  }
  if (args.getArgCount() < 2) {
    milli = msFromTime(t);
  }

  double date =
      makeDate(day(t), makeTime(hourFromTime(t), minFromTime(t), s, milli));
  double utcT =
      !isUTC ? timeClip(utcTime(date, localTimeOffsetCache)) : timeClip(date);
  self->setPrimitiveValue(utcT);
  return HermesValue::encodeTrustedNumberValue(utcT);
}

/// Takes 3 arguments: minutes, seconds, milliseconds.
/// Set the minutes, optionally seconds and milliseconds, return time.
CallResult<HermesValue> datePrototypeSetMinutes_RJS(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  bool isUTC = static_cast<bool>(ctx);
  auto self = args.dyncastThis<JSDate>();
  if (!self) {
    return runtime.raiseTypeError(
        "Date.prototype.setMinutes() called on non-Date object");
  }
  double t = self->getPrimitiveValue();
  auto &localTimeOffsetCache = runtime.getJSLibStorage()->localTimeOffsetCache;
  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double m = res->getNumber();
  double s;
  if (args.getArgCount() >= 2) {
    res = toNumber_RJS(runtime, args.getArgHandle(1));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    s = res->getNumber();
  }
  double milli;
  if (args.getArgCount() >= 3) {
    res = toNumber_RJS(runtime, args.getArgHandle(2));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    milli = res->getNumber();
  }
  if (std::isnan(t)) {
    return HermesValue::encodeTrustedNumberValue(
        std::numeric_limits<double>::quiet_NaN());
  }
  if (!isUTC) {
    t = localTime(t, localTimeOffsetCache);
  }
  if (args.getArgCount() < 2) {
    s = secFromTime(t);
  }
  if (args.getArgCount() < 3) {
    milli = msFromTime(t);
  }

  double date = makeDate(day(t), makeTime(hourFromTime(t), m, s, milli));
  double utcT =
      !isUTC ? timeClip(utcTime(date, localTimeOffsetCache)) : timeClip(date);
  self->setPrimitiveValue(utcT);
  return HermesValue::encodeTrustedNumberValue(utcT);
}

/// Takes 4 arguments: hours, minutes, seconds, milliseconds.
/// Set the hours, optionally minutes, seconds, and milliseconds, return time.
CallResult<HermesValue> datePrototypeSetHours_RJS(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  bool isUTC = static_cast<bool>(ctx);
  auto self = args.dyncastThis<JSDate>();
  if (!self) {
    return runtime.raiseTypeError(
        "Date.prototype.setHours() called on non-Date object");
  }
  double t = self->getPrimitiveValue();
  auto &localTimeOffsetCache = runtime.getJSLibStorage()->localTimeOffsetCache;
  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double h = res->getNumber();
  double m;
  if (args.getArgCount() >= 2) {
    res = toNumber_RJS(runtime, args.getArgHandle(1));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    m = res->getNumber();
  }
  double s;
  if (args.getArgCount() >= 3) {
    res = toNumber_RJS(runtime, args.getArgHandle(2));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    s = res->getNumber();
  }
  double milli;
  if (args.getArgCount() >= 4) {
    res = toNumber_RJS(runtime, args.getArgHandle(3));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    milli = res->getNumber();
  }
  if (std::isnan(t)) {
    return HermesValue::encodeTrustedNumberValue(
        std::numeric_limits<double>::quiet_NaN());
  }
  if (!isUTC) {
    t = localTime(t, localTimeOffsetCache);
  }
  if (args.getArgCount() < 2) {
    m = minFromTime(t);
  }
  if (args.getArgCount() < 3) {
    s = secFromTime(t);
  }
  if (args.getArgCount() < 4) {
    milli = msFromTime(t);
  }

  double date = makeDate(day(t), makeTime(h, m, s, milli));
  double utcT =
      !isUTC ? timeClip(utcTime(date, localTimeOffsetCache)) : timeClip(date);
  self->setPrimitiveValue(utcT);
  return HermesValue::encodeTrustedNumberValue(utcT);
}

/// Set the date of the month and return the new time.
CallResult<HermesValue> datePrototypeSetDate_RJS(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  bool isUTC = static_cast<bool>(ctx);
  auto self = args.dyncastThis<JSDate>();
  if (!self) {
    return runtime.raiseTypeError(
        "Date.prototype.setDate() called on non-Date object");
  }
  double t = self->getPrimitiveValue();
  auto &localTimeOffsetCache = runtime.getJSLibStorage()->localTimeOffsetCache;
  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (std::isnan(t)) {
    return HermesValue::encodeTrustedNumberValue(
        std::numeric_limits<double>::quiet_NaN());
  }
  if (!isUTC) {
    t = localTime(t, localTimeOffsetCache);
  }
  double dt = res->getNumber();
  double newDate = makeDate(
      makeDay(yearFromTime(t), monthFromTime(t), dt), timeWithinDay(t));
  double utcT = !isUTC ? timeClip(utcTime(newDate, localTimeOffsetCache))
                       : timeClip(newDate);
  self->setPrimitiveValue(utcT);
  return HermesValue::encodeTrustedNumberValue(utcT);
}

/// Takes 2 arguments: month and date.
/// Set the month, optionally the date of the month, return the time.
CallResult<HermesValue> datePrototypeSetMonth_RJS(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  bool isUTC = static_cast<bool>(ctx);
  auto self = args.dyncastThis<JSDate>();
  if (!self) {
    return runtime.raiseTypeError(
        "Date.prototype.setMonth() called on non-Date object");
  }
  double t = self->getPrimitiveValue();
  auto &localTimeOffsetCache = runtime.getJSLibStorage()->localTimeOffsetCache;
  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double m = res->getNumber();
  double dt;
  if (args.getArgCount() >= 2) {
    res = toNumber_RJS(runtime, args.getArgHandle(1));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    dt = res->getNumber();
  }
  if (std::isnan(t)) {
    return HermesValue::encodeTrustedNumberValue(
        std::numeric_limits<double>::quiet_NaN());
  }
  if (!isUTC) {
    t = localTime(t, localTimeOffsetCache);
  }
  if (args.getArgCount() < 2) {
    dt = dateFromTime(t);
  }
  double newDate = makeDate(makeDay(yearFromTime(t), m, dt), timeWithinDay(t));
  double utcT = !isUTC ? timeClip(utcTime(newDate, localTimeOffsetCache))
                       : timeClip(newDate);
  self->setPrimitiveValue(utcT);
  return HermesValue::encodeTrustedNumberValue(utcT);
}

/// Takes 3 arguments: full year, month and date.
/// Set the full year, optionally the month and date, return the time.
CallResult<HermesValue> datePrototypeSetFullYear_RJS(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  bool isUTC = static_cast<bool>(ctx);
  auto self = args.dyncastThis<JSDate>();
  if (!self) {
    return runtime.raiseTypeError(
        "Date.prototype.setFullYear() called on non-Date object");
  }
  double t = self->getPrimitiveValue();
  auto &localTimeOffsetCache = runtime.getJSLibStorage()->localTimeOffsetCache;
  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double y = res->getNumber();
  double m;
  if (args.getArgCount() >= 2) {
    res = toNumber_RJS(runtime, args.getArgHandle(1));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    m = res->getNumber();
  }
  double dt;
  if (args.getArgCount() >= 3) {
    res = toNumber_RJS(runtime, args.getArgHandle(2));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    dt = res->getNumber();
  }
  if (std::isnan(t)) {
    t = 0;
  } else if (!isUTC) {
    t = localTime(t, localTimeOffsetCache);
  }
  if (args.getArgCount() < 2) {
    m = monthFromTime(t);
  }
  if (args.getArgCount() < 3) {
    dt = dateFromTime(t);
  }
  double newDate = makeDate(makeDay(y, m, dt), timeWithinDay(t));
  double utcT = !isUTC ? timeClip(utcTime(newDate, localTimeOffsetCache))
                       : timeClip(newDate);
  self->setPrimitiveValue(utcT);
  return HermesValue::encodeTrustedNumberValue(utcT);
}

/// Takes one argument: the partial (or full) year.
/// Per spec, adds 1900 if the year is between 0 and 99.
/// Sets the year to the new year and returns the time.
CallResult<HermesValue> datePrototypeSetYear_RJS(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSDate>();
  if (!self) {
    return runtime.raiseTypeError(
        "Date.prototype.setYear() called on non-Date object");
  }
  double t = self->getPrimitiveValue();
  auto &localTimeOffsetCache = runtime.getJSLibStorage()->localTimeOffsetCache;
  auto res = toNumber_RJS(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double y = res->getNumber();
  if (std::isnan(y)) {
    self->setPrimitiveValue(std::numeric_limits<double>::quiet_NaN());
    return HermesValue::encodeNaNValue();
  }
  if (std::isnan(t)) {
    t = 0;
  } else {
    t = localTime(t, localTimeOffsetCache);
  }
  double yint = std::trunc(y);
  double yr = 0 <= yint && yint <= 99 ? yint + 1900 : y;
  double date = utcTime(
      makeDate(
          makeDay(yr, monthFromTime(t), dateFromTime(t)), timeWithinDay(t)),
      localTimeOffsetCache);
  double d = timeClip(date);
  self->setPrimitiveValue(d);
  return HermesValue::encodeTrustedNumberValue(d);
}

CallResult<HermesValue> datePrototypeToJSON_RJS(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> propValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto selfHandle = args.getThisHandle();
  auto objRes = toObject(runtime, selfHandle);
  if (objRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O = vmcast<JSObject>(objRes.getValue());
  auto tvRes = toPrimitive_RJS(runtime, lv.O, PreferredType::NUMBER);
  if (tvRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto tv = *tvRes;
  if (tv.isNumber() && !std::isfinite(tv.getNumber())) {
    return HermesValue::encodeNullValue();
  }
  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::toISOString));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propValue = std::move(*propRes);
  auto toISO = Handle<Callable>::dyn_vmcast(Handle<>{lv.propValue});
  if (!toISO.get()) {
    return runtime.raiseTypeError(
        "toISOString is not callable in Date.prototype.toJSON()");
  }
  return Callable::executeCall0(toISO, runtime, lv.O).toCallResultHermesValue();
}

CallResult<HermesValue> datePrototypeSymbolToPrimitive(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto O = args.dyncastThis<JSObject>();
  if (LLVM_UNLIKELY(!O)) {
    return runtime.raiseTypeError(
        "Date[Symbol.toPrimitive]() must be called on an object");
  }

  auto hint = args.getArgHandle(0);
  if (LLVM_UNLIKELY(!hint->isString())) {
    return runtime.raiseTypeError(
        "Date[Symbol.toPrimitive]() argument must be a string");
  }

  PreferredType tryFirst;

  if (runtime.symbolEqualsToStringPrim(
          Predefined::getSymbolID(Predefined::string), hint->getString()) ||
      runtime.symbolEqualsToStringPrim(
          Predefined::getSymbolID(Predefined::defaultStr), hint->getString())) {
    tryFirst = PreferredType::STRING;
  } else if (runtime.symbolEqualsToStringPrim(
                 Predefined::getSymbolID(Predefined::number),
                 hint->getString())) {
    tryFirst = PreferredType::NUMBER;
  } else {
    return runtime.raiseTypeError(
        "Type hint to Date[Symbol.primitive] must be "
        "'number', 'string', or 'default'");
  }

  return ordinaryToPrimitive(O, runtime, tryFirst);
}

} // namespace vm
} // namespace hermes
