/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

import android.os.Build;
import java.text.AttributedCharacterIterator;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

public class PlatformDateTimeFormatterAndroid implements IPlatformDateTimeFormatter {
  private DateFormat mDateFormat = null;

  @Override
  public String format(double n) {
    return mDateFormat.format(new Date((long) n));
  }

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

  @Override
  public String getDefaultCalendarName(ILocaleObject<?> mResolvedLocaleObject)
      throws JSRangeErrorException {
    return DateFormat.getDateInstance(
            java.text.DateFormat.SHORT, (Locale) mResolvedLocaleObject.getLocale())
        .getCalendar()
        .toString();
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
  }

  @Override
  public HourCycle getDefaultHourCycle(ILocaleObject<?> localeObject) throws JSRangeErrorException {
    HourCycle hourCycle;
    try {
      String dateFormatPattern =
          ((java.text.SimpleDateFormat)
                  DateFormat.getTimeInstance(DateFormat.FULL, (Locale) localeObject.getLocale()))
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

  @Override
  public HourCycle getPreferredHourCycle(ILocaleObject<?> localeObject, boolean hour12)
      throws JSRangeErrorException {
    HourCycle defaultCycle = getDefaultHourCycle(localeObject);
    if (hour12) {
      return defaultCycle == HourCycle.H11 || defaultCycle == HourCycle.H23
          ? HourCycle.H11
          : HourCycle.H12;
    }
    return defaultCycle == HourCycle.H11 || defaultCycle == HourCycle.H23
        ? HourCycle.H23
        : HourCycle.H24;
  }

  @Override
  public String getDefaultTimeZone(ILocaleObject<?> localeObject) throws JSRangeErrorException {
    return Calendar.getInstance((Locale) localeObject.getLocale()).getTimeZone().getID();
  }

  @Override
  public String getDefaultNumberingSystem(ILocaleObject<?> localeObject) {
    return "latn";
  }

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
    if (!calendar.isEmpty()) {
      ArrayList<String> calendarList = new ArrayList<>();
      calendarList.add(JSObjects.getJavaString(calendar));

      resolvedLocaleObject.setUnicodeExtensions("ca", calendarList);
    }

    if (!numberingSystem.isEmpty()) {

      ArrayList<String> numberingSystemList = new ArrayList<>();
      numberingSystemList.add(JSObjects.getJavaString(numberingSystem));

      resolvedLocaleObject.setUnicodeExtensions("nu", numberingSystemList);
    }

    boolean needDate = year != null || month != null || day != null;
    boolean needTime = hour != null || minute != null || second != null;

    if (needDate && needTime)
      mDateFormat =
          DateFormat.getDateTimeInstance(
              DateFormat.FULL, DateFormat.FULL, (Locale) resolvedLocaleObject.getLocale());
    else if (needDate)
      mDateFormat =
          DateFormat.getDateInstance(DateFormat.FULL, (Locale) resolvedLocaleObject.getLocale());
    else if (needTime)
      mDateFormat =
          DateFormat.getTimeInstance(DateFormat.FULL, (Locale) resolvedLocaleObject.getLocale());

    if (!JSObjects.isUndefined(timeZone) && !JSObjects.isNull(timeZone)) {
      TimeZone timeZoneObject = TimeZone.getTimeZone(JSObjects.getJavaString(timeZone));
      mDateFormat.setTimeZone(timeZoneObject);
    }
  }

  @Override
  public String[] getAvailableLocales() {

    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) {
      // Before L, Locale.toLanguageTag isn't available. Need to figure out how to get a locale id
      // from locale object ... Currently resoring to support only en
      return new String[] {"en"};
    }

    ArrayList<String> availableLocaleIds = new ArrayList<>();
    java.util.Locale[] availableLocales = java.text.DateFormat.getAvailableLocales();
    for (java.util.Locale locale : availableLocales) {
      availableLocaleIds.add(locale.toLanguageTag()); // TODO:: Not available on platforms <= 20
    }

    String[] availableLocaleIdsArray = new String[availableLocaleIds.size()];
    return availableLocaleIds.toArray(availableLocaleIdsArray);
  }

  PlatformDateTimeFormatterAndroid() {}
}
