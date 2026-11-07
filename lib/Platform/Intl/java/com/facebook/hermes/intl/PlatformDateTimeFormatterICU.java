/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

import static com.facebook.hermes.intl.IPlatformDateTimeFormatter.DateStyle.UNDEFINED;

import android.icu.text.DateFormat;
import android.icu.text.DateTimePatternGenerator;
import android.icu.text.NumberingSystem;
import android.icu.text.SimpleDateFormat;
import android.icu.util.Calendar;
import android.icu.util.TimeZone;
import android.icu.util.ULocale;
import android.os.Build;
import androidx.annotation.RequiresApi;
import java.text.AttributedCharacterIterator;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;

public class PlatformDateTimeFormatterICU implements IPlatformDateTimeFormatter {
  private DateFormat mDateFormat = null;

  @RequiresApi(api = Build.VERSION_CODES.N)
  @Override
  public String format(double n) {
    return mDateFormat.format(new Date((long) n));
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  @Override
  public String fieldToString(AttributedCharacterIterator.Attribute field, String fieldValue) {
    if (field == DateFormat.Field.DAY_OF_WEEK) {
      return "weekday";
    }
    if (field == DateFormat.Field.ERA) {
      return "era";
    }
    if (field == DateFormat.Field.YEAR) {
      // TODO:: I can't find the right rules to mark the type of the an year part as "yearName".
      // Likely, the presense of another part with "relatedYear" is the decider ?
      // Currently, i'm differentiating based on whether the value is numeric or not.
      try {
        Double.parseDouble(fieldValue);
        return "year";
      } catch (NumberFormatException nfe) {
        return "yearName";
      }
    }
    if (field == DateFormat.Field.MONTH) {
      return "month";
    }
    if (field == DateFormat.Field.DAY_OF_MONTH) {
      return "day";
    }
    if (field == DateFormat.Field.HOUR0) {
      return "hour";
    }
    if (field == DateFormat.Field.HOUR1) {
      return "hour";
    }
    if (field == DateFormat.Field.HOUR_OF_DAY0) {
      return "hour";
    }
    if (field == DateFormat.Field.HOUR_OF_DAY1) {
      return "hour";
    }
    if (field == DateFormat.Field.MINUTE) {
      return "minute";
    }
    if (field == DateFormat.Field.SECOND) {
      return "second";
    }
    if (field == DateFormat.Field.TIME_ZONE) {
      return "timeZoneName";
    }
    if (field == DateFormat.Field.AM_PM) {
      return "dayPeriod";
    }
    // TODO:: There must be a better way to do this.
    if (field.toString().equals("android.icu.text.DateFormat$Field(related year)"))
      return "relatedYear";

    // Report unsupported/unexpected date fields as literals.
    return "literal";
  }

  @Override
  public AttributedCharacterIterator formatToParts(double n) {
    return mDateFormat.formatToCharacterIterator(n);
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  @Override
  public String getDefaultCalendarName(ILocaleObject<?> mResolvedLocaleObject)
      throws JSRangeErrorException {
    String defaultCalendar =
        DateFormat.getDateInstance(DateFormat.SHORT, (ULocale) mResolvedLocaleObject.getLocale())
            .getCalendar()
            .getType();
    defaultCalendar = UnicodeExtensionKeys.resolveCalendarAlias(defaultCalendar);
    return defaultCalendar;
  }

  private static class PatternUtils {

    public static String getPatternWithoutLiterals(String pattern) {

      StringBuilder segment = new StringBuilder();
      boolean literalSegmentRunning = false;
      for (int idx = 0; idx < pattern.length(); idx++) {
        char c = pattern.charAt(idx);
        if (c == '\'') {
          literalSegmentRunning = !literalSegmentRunning;
          continue;
        }

        if (literalSegmentRunning) continue;

        // ['a'..'z'] and ['A'..'Z']
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) segment.append(pattern.charAt(idx));
      }

      return segment.toString();
    }

    public static String withHourCycle(String pattern, HourCycle hourCycle) {
      char hourSymbol;
      switch (hourCycle) {
        case H11:
          hourSymbol = 'K';
          break;
        case H12:
          hourSymbol = 'h';
          break;
        case H23:
          hourSymbol = 'H';
          break;
        case H24:
          hourSymbol = 'k';
          break;
        default:
          return pattern;
      }

      StringBuilder result = new StringBuilder(pattern.length());
      boolean inLiteral = false;
      for (int i = 0; i < pattern.length(); i++) {
        char c = pattern.charAt(i);
        if (c == '\'') {
          result.append(c);
          if (i + 1 < pattern.length() && pattern.charAt(i + 1) == '\'') {
            result.append(pattern.charAt(++i));
          } else {
            inLiteral = !inLiteral;
          }
        } else if (!inLiteral && (c == 'h' || c == 'H' || c == 'K' || c == 'k')) {
          result.append(hourSymbol);
        } else {
          result.append(c);
        }
      }
      return result.toString();
    }

    public static String withoutDayPeriod(String pattern) {
      StringBuilder result = new StringBuilder(pattern.length());
      boolean inLiteral = false;
      for (int i = 0; i < pattern.length(); i++) {
        char c = pattern.charAt(i);
        if (c == '\'') {
          result.append(c);
          if (i + 1 < pattern.length() && pattern.charAt(i + 1) == '\'') {
            result.append(pattern.charAt(++i));
          } else {
            inLiteral = !inLiteral;
          }
        } else if (inLiteral || (c != 'a' && c != 'b' && c != 'B')) {
          result.append(c);
        }
      }
      return result.toString();
    }
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  @Override
  public HourCycle getDefaultHourCycle(ILocaleObject<?> localeObject) throws JSRangeErrorException {
    HourCycle hourCycle;
    try {
      String dateFormatPattern =
          ((android.icu.text.SimpleDateFormat)
                  DateFormat.getTimeInstance(DateFormat.FULL, (ULocale) localeObject.getLocale()))
              .toPattern();
      String dateFormatPatternWithoutLiterals =
          PatternUtils.getPatternWithoutLiterals(dateFormatPattern);
      if (dateFormatPatternWithoutLiterals.contains(String.valueOf('h'))) hourCycle = HourCycle.H12;
      else if (dateFormatPatternWithoutLiterals.contains(String.valueOf('K')))
        hourCycle = HourCycle.H11;
      else if (dateFormatPatternWithoutLiterals.contains(String.valueOf('H')))
        hourCycle = HourCycle.H23;
      else // TODO :: Make it more tight.
      hourCycle = HourCycle.H24;
    } catch (ClassCastException ex) {
      hourCycle = HourCycle.H24;
    }

    return hourCycle;
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  @Override
  public HourCycle getPreferredHourCycle(ILocaleObject<?> localeObject, boolean hour12)
      throws JSRangeErrorException {
    ULocale locale = (ULocale) localeObject.getLocaleWithoutExtensions();
    String pattern =
        DateTimePatternGenerator.getInstance(locale).getBestPattern(hour12 ? "hm" : "Hm");
    String fields = PatternUtils.getPatternWithoutLiterals(pattern);
    if (fields.indexOf('K') >= 0) return HourCycle.H11;
    if (fields.indexOf('h') >= 0) return HourCycle.H12;
    if (fields.indexOf('H') >= 0) return HourCycle.H23;
    if (fields.indexOf('k') >= 0) return HourCycle.H24;
    throw new JSRangeErrorException("No hour field in locale time pattern");
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  @Override
  public String getDefaultTimeZone(ILocaleObject<?> localeObject) throws JSRangeErrorException {
    return Calendar.getInstance((ULocale) localeObject.getLocale()).getTimeZone().getID();
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  @Override
  public String getDefaultNumberingSystem(ILocaleObject<?> localeObject)
      throws JSRangeErrorException {
    return NumberingSystem.getInstance((ULocale) localeObject.getLocale()).getName();
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  static int toICUDateStyle(DateStyle dateStyle) throws JSRangeErrorException {
    switch (dateStyle) {
      case FULL:
        return DateFormat.FULL;
      case LONG:
        return DateFormat.LONG;
      case MEDIUM:
        return DateFormat.MEDIUM;
      case SHORT:
        return DateFormat.SHORT;
      case UNDEFINED:
      default:
        throw new JSRangeErrorException("Invalid DateStyle: " + dateStyle.toString());
    }
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  static int toICUTimeStyle(TimeStyle timeStyle) throws JSRangeErrorException {
    switch (timeStyle) {
      case FULL:
        return DateFormat.FULL;
      case LONG:
        return DateFormat.LONG;
      case MEDIUM:
        return DateFormat.MEDIUM;
      case SHORT:
        return DateFormat.SHORT;
      case UNDEFINED:
      default:
        throw new JSRangeErrorException("Invalid DateStyle: " + timeStyle.toString());
    }
  }

  private static void replaceChars(StringBuilder builder, String from, String to) {
    int index = builder.indexOf(from);
    if (index != -1) {
      builder.replace(index, index + from.length(), to);
    }
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  private static String getPatternForStyle(
      ILocaleObject<?> resolvedLocaleObject, DateStyle dateStyle, TimeStyle timeStyle)
      throws JSRangeErrorException {
    if (dateStyle == UNDEFINED) {
      return ((SimpleDateFormat)
              DateFormat.getTimeInstance(
                  toICUTimeStyle(timeStyle), (ULocale) resolvedLocaleObject.getLocale()))
          .toLocalizedPattern();
    } else if (timeStyle == TimeStyle.UNDEFINED) {
      return ((SimpleDateFormat)
              DateFormat.getDateInstance(
                  toICUDateStyle(dateStyle), (ULocale) resolvedLocaleObject.getLocale()))
          .toLocalizedPattern();
    } else {
      return ((SimpleDateFormat)
              DateFormat.getDateTimeInstance(
                  toICUDateStyle(dateStyle),
                  toICUTimeStyle(timeStyle),
                  (ULocale) resolvedLocaleObject.getLocale()))
          .toLocalizedPattern();
    }
  }

  private static void replacePatternChars(StringBuilder skeletonBuffer, char[] fromSet, char to) {
    for (int idx = 0; idx < skeletonBuffer.length(); idx++) {
      for (char fromChar : fromSet) {
        if (skeletonBuffer.charAt(idx) == fromChar) {
          skeletonBuffer.setCharAt(idx, to);
          break; // we don't expect more than once fromChar to be present in the pattern.
        }
      }
    }
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  private static String getSkeleton(
      ILocaleObject<?> resolvedLocaleObject,
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
      DateStyle dateStyle,
      TimeStyle timeStyle,
      Object hour12)
      throws JSRangeErrorException {

    StringBuilder skeletonBuffer = new StringBuilder();

    // For reference on patterns/skleleton:
    // https://docs.oracle.com/javase/8/docs/api/java/text/SimpleDateFormat.html

    if (dateStyle != UNDEFINED || timeStyle != TimeStyle.UNDEFINED) {
      skeletonBuffer.append(getPatternForStyle(resolvedLocaleObject, dateStyle, timeStyle));

      // Apply customizations based on additional options.
      HashMap<String, String> exts = resolvedLocaleObject.getUnicodeExtensions();
      if (exts.containsKey("hc")) {
        String hourCycleExt = exts.get("hc");
        if (hourCycleExt == "h11" || hourCycleExt == "h12") {
          replacePatternChars(skeletonBuffer, new char[] {'H', 'K', 'k'}, 'h');
        } else if (hourCycleExt == "h23" || hourCycleExt == "h24") {
          replacePatternChars(skeletonBuffer, new char[] {'h', 'H', 'K'}, 'k');
        }
      }

      if (hourCycle == HourCycle.H11 || hourCycle == HourCycle.H12) {
        replacePatternChars(skeletonBuffer, new char[] {'H', 'K', 'k'}, 'h');
      } else if (hourCycle == HourCycle.H23 || hourCycle == HourCycle.H24) {
        replacePatternChars(skeletonBuffer, new char[] {'h', 'H', 'K'}, 'k');
      }

      if (!JSObjects.isUndefined(hour12) && !JSObjects.isNull(hour12)) {
        if (JSObjects.getJavaBoolean(hour12)) { // true
          replacePatternChars(skeletonBuffer, new char[] {'H', 'K', 'k'}, 'h');
        } else {
          replacePatternChars(skeletonBuffer, new char[] {'h', 'H', 'K'}, 'k');
        }
      }

      if (hourCycle == HourCycle.H23 || hourCycle == HourCycle.H24) {
        skeletonBuffer = new StringBuilder(PatternUtils.withoutDayPeriod(skeletonBuffer.toString()));
      }
    } else {
      skeletonBuffer.append(weekDay.getSkeleonSymbol());
      skeletonBuffer.append(era.getSkeleonSymbol());
      skeletonBuffer.append(year.getSkeleonSymbol());
      skeletonBuffer.append(month.getSkeleonSymbol());
      skeletonBuffer.append(day.getSkeleonSymbol());

      if (hourCycle == HourCycle.H11 || hourCycle == HourCycle.H12)
        skeletonBuffer.append(hour.getSkeleonSymbol12());
      else skeletonBuffer.append(hour.getSkeleonSymbol24());

      skeletonBuffer.append(minute.getSkeleonSymbol());
      skeletonBuffer.append(second.getSkeleonSymbol());
      skeletonBuffer.append(timeZoneName.getSkeleonSymbol());
    }

    return skeletonBuffer.toString();
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  public void configure(
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
      throws JSRangeErrorException {
    String skeleton =
        getSkeleton(
            resolvedLocaleObject,
            weekDay,
            era,
            year,
            month,
            day,
            hour,
            minute,
            second,
            timeZoneName,
            hourCycle,
            dateStyle,
            timeStyle,
            hour12);

    Calendar calendarInstance = null;
    if (!calendar.isEmpty()) {
      ArrayList<String> calendarList = new ArrayList<>();
      calendarList.add(JSObjects.getJavaString(calendar));

      ILocaleObject<?> modifiedLocaleObject = resolvedLocaleObject.cloneObject();
      modifiedLocaleObject.setUnicodeExtensions("ca", calendarList);

      calendarInstance = Calendar.getInstance((ULocale) modifiedLocaleObject.getLocale());
    }

    if (!numberingSystem.isEmpty()) {
      NumberingSystem numberingSystemObject;
      try {
        numberingSystemObject =
            NumberingSystem.getInstanceByName(JSObjects.getJavaString(numberingSystem));
      } catch (RuntimeException ex) {
        throw new JSRangeErrorException("Invalid numbering system: " + numberingSystem);
      }

      if (numberingSystemObject == null)
        throw new JSRangeErrorException("Invalid numbering system: " + numberingSystem);

      ArrayList<String> numberingSystemList = new ArrayList<>();
      numberingSystemList.add(JSObjects.getJavaString(numberingSystem));

      // TODO : Make sure that this is not shown through the resolvedOptions
      resolvedLocaleObject.setUnicodeExtensions("nu", numberingSystemList);
    }

    if (calendarInstance != null)
      mDateFormat =
          DateFormat.getPatternInstance(
              calendarInstance, skeleton, (ULocale) resolvedLocaleObject.getLocale());
    else
      mDateFormat =
          DateFormat.getPatternInstance(skeleton, (ULocale) resolvedLocaleObject.getLocale());

    if (hourCycle != HourCycle.UNDEFINED) {
      SimpleDateFormat formatter = (SimpleDateFormat) mDateFormat;
      formatter.applyPattern(PatternUtils.withHourCycle(formatter.toPattern(), hourCycle));
    }

    if (!JSObjects.isUndefined(timeZone) && !JSObjects.isNull(timeZone)) {
      TimeZone timeZoneObject = TimeZone.getTimeZone(JSObjects.getJavaString(timeZone));
      mDateFormat.setTimeZone(timeZoneObject);
    }
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  @Override
  public String[] getAvailableLocales() {
    ArrayList<String> availableLocaleIds = new ArrayList<>();

    // [[Comment copied here from NumberFormat]]
    // NumberFormat.getAvailableLocales() returns a shorter list compared to
    // ULocale.getAvailableLocales.
    // For e.g. "zh-TW" is missing in the list returned by NumberFormat.getAvailableLocales() in my
    // emulator.
    // But, NumberFormatter is able to format specific to "zh-TW" .. for instance "NaN" is expected
    // to be formatted as "非數值" in "zh-TW" by as "NaN" in "zh"
    // In short, NumberFormat.getAvailableLocales() doesn't contain all the locales as the
    // NumberFormat can format. Hence, using ULocale.getAvailableLocales()
    //

    // java.util.Locale[] availableLocales = android.icu.text.DateFormat.getAvailableLocales();
    android.icu.util.ULocale[] availableLocales = ULocale.getAvailableLocales();
    for (android.icu.util.ULocale locale : availableLocales) {
      availableLocaleIds.add(locale.toLanguageTag());
    }

    String[] availableLocaleIdsArray = new String[availableLocaleIds.size()];
    return availableLocaleIds.toArray(availableLocaleIdsArray);
  }

  PlatformDateTimeFormatterICU() {}
}
