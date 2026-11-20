/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

import static android.icu.text.NumberFormat.ACCOUNTINGCURRENCYSTYLE;
import static android.icu.text.NumberFormat.CURRENCYSTYLE;
import static android.icu.text.NumberFormat.NUMBERSTYLE;
import static android.icu.text.NumberFormat.PERCENTSTYLE;
import static android.icu.text.NumberFormat.SCIENTIFICSTYLE;

import android.icu.text.MeasureFormat;
import android.icu.util.Currency;
import android.os.Build;
import androidx.annotation.RequiresApi;
import java.text.AttributedCharacterIterator;

public interface IPlatformNumberFormatter {

  // [[style]]
  // The formatting style to use , the default is "decimal".
  // "decimal" for plain number formatting.
  // "currency" for currency formatting.
  // "percent" for percent formatting
  // "unit" for unit formatting
  enum Style {
    DECIMAL,
    PERCENT,
    CURRENCY,
    UNIT;

    @Override
    public String toString() {
      switch (this) {
        case DECIMAL:
          return "decimal";
        case PERCENT:
          return "percent";
        case CURRENCY:
          return "currency";
        case UNIT:
          return "unit";
        default:
          throw new IllegalArgumentException();
      }
    }

    public int getInitialNumberFormatStyle(Notation notation, CurrencySign currencySign)
        throws JSRangeErrorException {
      int numberFormatStyle;
      switch (this) {
        case CURRENCY:
          if (currencySign == CurrencySign.ACCOUNTING) numberFormatStyle = ACCOUNTINGCURRENCYSTYLE;
          else if (currencySign == CurrencySign.STANDARD) numberFormatStyle = CURRENCYSTYLE;
          else throw new JSRangeErrorException("Unrecognized formatting style requested.");
          break;

        case PERCENT:
          numberFormatStyle = PERCENTSTYLE;
          break;

        case UNIT:
        case DECIMAL:
        default:
          if (notation == Notation.SCIENTIFIC || notation == Notation.ENGINEERING) {
            numberFormatStyle = SCIENTIFICSTYLE;
          } else {
            numberFormatStyle = NUMBERSTYLE;
          }
      }
      return numberFormatStyle;
    }
  }

  // [[Notation ]]
  // The formatting that should be displayed for the number, the defaults is "standard"
  //
  //    "standard" plain number formatting
  //    "scientific" return the order-of-magnitude for formatted number.
  //    "engineering" return the exponent of ten when divisible by three
  //    "compact" string representing exponent, defaults is using the "short" form.
  enum Notation {
    STANDARD,
    SCIENTIFIC,
    ENGINEERING,
    COMPACT;

    @Override
    public String toString() {
      switch (this) {
        case STANDARD:
          return "standard";
        case SCIENTIFIC:
          return "scientific";
        case ENGINEERING:
          return "engineering";
        case COMPACT:
          return "compact";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  // [[compactDisplay]]
  // Only used when notation is "compact". Takes either "short" (default) or "long".
  enum CompactDisplay {
    SHORT,
    LONG;

    @Override
    public String toString() {
      switch (this) {
        case SHORT:
          return "short";
        case LONG:
          return "long";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum SignDisplay {
    AUTO,
    ALWAYS,
    NEVER,
    EXCEPTZERO;

    @Override
    public String toString() {
      switch (this) {
        case AUTO:
          return "auto";
        case ALWAYS:
          return "always";
        case NEVER:
          return "never";
        case EXCEPTZERO:
          return "exceptZero";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum UnitDisplay {
    SHORT,
    NARROW,
    LONG;

    @Override
    public String toString() {
      switch (this) {
        case SHORT:
          return "short";
        case NARROW:
          return "narrow";
        case LONG:
          return "long";
        default:
          throw new IllegalArgumentException();
      }
    }

    @RequiresApi(api = Build.VERSION_CODES.N)
    public MeasureFormat.FormatWidth getFormatWidth() {
      MeasureFormat.FormatWidth formatWidth;
      switch (this) {
        case LONG:
          return MeasureFormat.FormatWidth.WIDE;
        case NARROW:
          return MeasureFormat.FormatWidth.NARROW;
        case SHORT:
        default:
          return MeasureFormat.FormatWidth.SHORT;
      }
    }
  }

  // How to display the currency in currency formatting. Possible values are:
  // "symbol" to use a localized currency symbol such as €, this is the default value,
  // "narrowSymbol" to use a narrow format symbol ("$100" rather than "US$100"),
  // "code" to use the ISO currency code,
  // "name" to use a localized currency name such as "dollar",
  enum CurrencyDisplay {
    SYMBOL,
    NARROWSYMBOL,
    CODE,
    NAME;

    @Override
    public String toString() {
      switch (this) {
        case SYMBOL:
          return "symbol";
        case NARROWSYMBOL:
          return "narrowSymbol";
        case CODE:
          return "code";
        case NAME:
          return "name";
        default:
          throw new IllegalArgumentException();
      }
    }

    public int getNameStyle() {
      switch (this) {
        case NAME:
          return Currency.LONG_NAME;
        case NARROWSYMBOL:
          // Currency.NARROW_SYMBOL_NAME is only available from API 30.
          if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return Currency.NARROW_SYMBOL_NAME;
          return Currency.SYMBOL_NAME;
        case SYMBOL:
        default:
          return Currency.SYMBOL_NAME;
      }
    }
  }

  // In many locales, accounting format means to wrap the number with parentheses instead of
  // appending a minus sign.
  // You can enable this formatting by setting the currencySign option to "accounting". The default
  // value is "standard".
  enum CurrencySign {
    STANDARD,
    ACCOUNTING;

    @Override
    public String toString() {
      switch (this) {
        case ACCOUNTING:
          return "accounting";
        case STANDARD:
          return "standard";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum RoundingType {
    SIGNIFICANT_DIGITS,
    FRACTION_DIGITS,
    COMPACT_ROUNDING
  }

  IPlatformNumberFormatter configure(
      ILocaleObject<?> localeObject,
      String numberingSystem,
      IPlatformNumberFormatter.Style style,
      IPlatformNumberFormatter.CurrencySign currencySign,
      IPlatformNumberFormatter.Notation notation,
      IPlatformNumberFormatter.CompactDisplay compactDisplay)
      throws JSRangeErrorException;

  String getDefaultNumberingSystem(ILocaleObject<?> localeObject) throws JSRangeErrorException;

  IPlatformNumberFormatter setCurrency(String currencyCode, CurrencyDisplay currencyDisplay)
      throws JSRangeErrorException;

  IPlatformNumberFormatter setGrouping(boolean mGroupingUsed);

  IPlatformNumberFormatter setMinIntergerDigits(int minimumIntegerDigits);

  IPlatformNumberFormatter setSignificantDigits(
      IPlatformNumberFormatter.RoundingType roundingType,
      int minimumSignificantDigits,
      int maximumSignificantDigits)
      throws JSRangeErrorException;

  IPlatformNumberFormatter setFractionDigits(
      IPlatformNumberFormatter.RoundingType roundingType,
      int minimumFractionDigits,
      int maximumFractionDigits);

  IPlatformNumberFormatter setSignDisplay(IPlatformNumberFormatter.SignDisplay signDisplay);

  IPlatformNumberFormatter setUnits(String unit, IPlatformNumberFormatter.UnitDisplay unitDisplay)
      throws JSRangeErrorException;

  String format(double n) throws JSRangeErrorException;

  String fieldToString(AttributedCharacterIterator.Attribute attribute, double x);

  AttributedCharacterIterator formatToParts(double n) throws JSRangeErrorException;

  String[] getAvailableLocales();
}
