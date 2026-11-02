/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

import java.text.AttributedCharacterIterator;

public interface IPlatformDateTimeFormatter {
  enum FormatMatcher {
    BESTFIT,
    BASIC;

    @Override
    public String toString() {
      switch (this) {
        case BESTFIT:
          return "best fit";
        case BASIC:
          return "basic";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum HourCycle {
    H11,
    H12,
    H23,
    H24,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case H11:
          return "h11";
        case H12:
          return "h12";
        case H23:
          return "h23";
        case H24:
          return "h24";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum WeekDay {
    LONG,
    SHORT,
    NARROW,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case LONG:
          return "long";
        case SHORT:
          return "short";
        case NARROW:
          return "narrow";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol() {
      switch (this) {
        case LONG:
          return "EEEE";
        case SHORT:
          return "EEE";
        case NARROW:
          return "EEEEE";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum Era {
    LONG,
    SHORT,
    NARROW,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case LONG:
          return "long";
        case SHORT:
          return "short";
        case NARROW:
          return "narrow";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol() {
      switch (this) {
        case LONG:
          return "GGGG";
        case SHORT:
          return "GGG";
        case NARROW:
          return "G5";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum Year {
    NUMERIC,
    DIGIT2,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case NUMERIC:
          return "numeric";
        case DIGIT2:
          return "2-digit";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol() {
      switch (this) {
        case NUMERIC:
          return "yyyy";
        case DIGIT2:
          return "yy";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum Month {
    NUMERIC,
    DIGIT2,
    LONG,
    SHORT,
    NARROW,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case NUMERIC:
          return "numeric";
        case DIGIT2:
          return "2-digit";
        case LONG:
          return "long";
        case SHORT:
          return "short";
        case NARROW:
          return "narrow";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol() {
      switch (this) {
        case NUMERIC:
          return "M";
        case DIGIT2:
          return "MM";
        case LONG:
          return "MMMM";
        case SHORT:
          return "MMM";
        case NARROW:
          return "MMMMM";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum Day {
    NUMERIC,
    DIGIT2,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case NUMERIC:
          return "numeric";
        case DIGIT2:
          return "2-digit";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol() {
      switch (this) {
        case NUMERIC:
          return "d";
        case DIGIT2:
          return "dd";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum Hour {
    NUMERIC,
    DIGIT2,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case NUMERIC:
          return "numeric";
        case DIGIT2:
          return "2-digit";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol12() {
      switch (this) {
        case NUMERIC:
          return "h";
        case DIGIT2:
          return "hh";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol24() {
      switch (this) {
        case NUMERIC:
          return "k";
        case DIGIT2:
          return "kk";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum Minute {
    NUMERIC,
    DIGIT2,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case NUMERIC:
          return "numeric";
        case DIGIT2:
          return "2-digit";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol() {
      switch (this) {
        case NUMERIC:
          return "m";
        case DIGIT2:
          return "mm";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum Second {
    NUMERIC,
    DIGIT2,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case NUMERIC:
          return "numeric";
        case DIGIT2:
          return "2-digit";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol() {
      switch (this) {
        case NUMERIC:
          return "s";
        case DIGIT2:
          return "ss";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum TimeZoneName {
    LONG,
    LONGOFFSET,
    LONGGENERIC,
    SHORT,
    SHORTOFFSET,
    SHORTGENERIC,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case LONG:
          return "long";
        case LONGOFFSET:
          return "longOffset";
        case LONGGENERIC:
          return "longGeneric";
        case SHORT:
          return "short";
        case SHORTOFFSET:
          return "shortOffset";
        case SHORTGENERIC:
          return "shortGeneric";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }

    public String getSkeleonSymbol() {
      switch (this) {
        case LONG:
          return "zzzz";
        case LONGOFFSET:
          return "OOOO";
        case LONGGENERIC:
          return "vvvv";
        case SHORT:
          return "z";
        case SHORTOFFSET:
          return "O";
        case SHORTGENERIC:
          return "v";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum DateStyle {
    FULL,
    LONG,
    MEDIUM,
    SHORT,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case FULL:
          return "full";
        case LONG:
          return "long";
        case MEDIUM:
          return "medium";
        case SHORT:
          return "short";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  enum TimeStyle {
    FULL,
    LONG,
    MEDIUM,
    SHORT,
    UNDEFINED;

    @Override
    public String toString() {
      switch (this) {
        case FULL:
          return "full";
        case LONG:
          return "long";
        case MEDIUM:
          return "medium";
        case SHORT:
          return "short";
        case UNDEFINED:
          return "";
        default:
          throw new IllegalArgumentException();
      }
    }
  }

  void configure(
      ILocaleObject<?> resolvedLocaleObject,
      String calendar,
      String numberingSystem,
      FormatMatcher formatMatcher,
      WeekDay weekDay,
      Era era,
      Year year,
      Month month,
      Day day,
      Hour hour,
      Minute minute,
      Second second,
      TimeZoneName timeZoneName,
      HourCycle hourCycle,
      Object timeZone,
      DateStyle dateStyle,
      TimeStyle timeStyle,
      Object hour12)
      throws JSRangeErrorException;

  String format(double n) throws JSRangeErrorException;

  String fieldToString(AttributedCharacterIterator.Attribute attribute, String fieldValue);

  AttributedCharacterIterator formatToParts(double n) throws JSRangeErrorException;

  String getDefaultCalendarName(ILocaleObject<?> mResolvedLocaleObject)
      throws JSRangeErrorException;

  HourCycle getDefaultHourCycle(ILocaleObject<?> localeObject) throws JSRangeErrorException;

  HourCycle getPreferredHourCycle(ILocaleObject<?> localeObject, boolean hour12)
      throws JSRangeErrorException;

  String getDefaultTimeZone(ILocaleObject<?> localeObject) throws JSRangeErrorException;

  String getDefaultNumberingSystem(ILocaleObject<?> localeObject) throws JSRangeErrorException;

  String[] getAvailableLocales();
}
