/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

import android.icu.util.ULocale;
import android.os.Build;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

public class UnicodeExtensionKeys {
  public static String CALENDAR = "calendar";
  public static String CALENDAR_CANON = "ca";

  public static String NUMERINGSYSTEM = "numbers";
  public static String NUMERINGSYSTEM_CANON = "nu";

  public static String HOURCYCLE = "hours";
  public static String HOURCYCLE_CANON = "hc";

  public static String COLLATION = "collation";
  public static String COLLATION_CANON = "co";

  public static String COLLATION_NUMERIC = "colnumeric";
  public static String COLLATION_NUMERIC_CANON = "kn";

  public static String COLLATION_CASEFIRST = "colcasefirst"; // TODO:: double check this
  public static String COLLATION_CASEFIRST_CANON = "kf";

  // TODO :: Build/Look-for a BiMap datastructure.
  private static HashMap<String, String> s_canonicalkey_icukey_map;
  private static HashMap<String, String> s_icukey_canonicalkey_map;

  static {
    s_canonicalkey_icukey_map =
        new HashMap<String, String>() {
          {
            put(CALENDAR_CANON, CALENDAR);
            put(NUMERINGSYSTEM_CANON, NUMERINGSYSTEM);
            put(HOURCYCLE_CANON, HOURCYCLE);
            put(COLLATION_CANON, COLLATION);
            put(COLLATION_NUMERIC_CANON, COLLATION_NUMERIC);
            put(COLLATION_CASEFIRST_CANON, COLLATION_CASEFIRST);
          }
        };

    s_icukey_canonicalkey_map =
        new HashMap<String, String>() {
          {
            put(CALENDAR, CALENDAR_CANON);
            put(NUMERINGSYSTEM, NUMERINGSYSTEM_CANON);
            put(HOURCYCLE, HOURCYCLE_CANON);
            put(COLLATION, COLLATION_CANON);
            put(COLLATION_NUMERIC, COLLATION_NUMERIC_CANON);
            put(COLLATION_CASEFIRST, COLLATION_CASEFIRST_CANON);
          }
        };
  }

  public static String CanonicalKeyToICUKey(String key) {
    if (s_canonicalkey_icukey_map.containsKey(key)) return s_canonicalkey_icukey_map.get(key);
    else return key; // TODO:: Make it tighter
  }

  public static String ICUKeyToCanonicalKey(String key) {
    if (s_icukey_canonicalkey_map.containsKey(key)) return s_icukey_canonicalkey_map.get(key);
    else return key; // // TODO:: Make it tighter
  }

  // Ref: https://github.com/unicode-org/cldr/blob/release-37/common/bcp47/collation.xml#L12
  private static final Map<String, String> s_collationAliasMappings =
      new HashMap<String, String>() {
        {
          put("dictionary", "dict");
          put("phonebook", "phonebk");
          put("traditional", "trad");
          put("gb2312han", "gb2312");
        }
      };

  public static String resolveCollationAlias(String value) {
    if (!s_collationAliasMappings.containsKey(value)) {
      return value;
    } else {
      return s_collationAliasMappings.get(value);
    }
  }

  private static Map<String, String> s_calendarAliasMappings =
      new HashMap<String, String>() {
        {
          // https://github.com/unicode-org/cldr/blob/master/common/bcp47/calendar.xml
          put("gregorian", "gregory");
        }
      };

  private static Map<String, String> s_numberSystemAliasMappings =
      new HashMap<String, String>() {
        {
          // https://github.com/unicode-org/cldr/blob/master/common/bcp47/number.xml
          put("traditional", "traditio");
        }
      };

  public static String resolveCalendarAlias(String value) {
    if (!s_calendarAliasMappings.containsKey(value)) {
      return value;
    } else {
      return s_calendarAliasMappings.get(value);
    }
  }

  public static String resolveNumberSystemAlias(String value) {
    if (!s_numberSystemAliasMappings.containsKey(value)) {
      return value;
    } else {
      return s_numberSystemAliasMappings.get(value);
    }
  }

  private static Map<String, String[]> s_validKeywords =
      new HashMap<String, String[]>() {
        {
          // Ref:: https://tc39.es/ecma402/#table-numbering-system-digits
          // It is a subset of
          // https://github.com/unicode-org/cldr/blob/master/common/bcp47/number.xml
          put(
              "nu",
              new String[] {
                "adlm",
                "ahom",
                "arab",
                "arabext",
                "bali",
                "beng",
                "bhks",
                "brah",
                "cakm",
                "cham",
                "deva",
                "diak",
                "fullwide",
                "gong",
                "gonm",
                "gujr",
                "guru",
                "hanidec",
                "hmng",
                "hmnp",
                "java",
                "kali",
                "khmr",
                "knda",
                "lana",
                "lanatham",
                "laoo",
                "latn",
                "lepc",
                "limb",
                "mathbold",
                "mathdbl",
                "mathmono",
                "mathsanb",
                "mathsans",
                "mlym",
                "modi",
                "mong",
                "mroo",
                "mtei",
                "mymr",
                "mymrshan",
                "mymrtlng",
                "newa",
                "nkoo",
                "olck",
                "orya",
                "osma",
                "rohg",
                "saur",
                "segment",
                "shrd",
                "sind",
                "sinh",
                "sora",
                "sund",
                "takr",
                "talu",
                "tamldec",
                "telu",
                "thai",
                "tibt",
                "tirh",
                "vaii",
                "wara",
                "wcho"
              });

          // Ref:: https://github.com/unicode-org/cldr/blob/release-37/common/bcp47/collation.xml
          // -- Minus "standard" & "search" which are not allowed as per spec:
          // https://tc39.es/ecma402/#sec-intl-collator-internal-slots
          // This list can be replaced by
          // https://developer.android.com/reference/android/icu/text/Collator#getKeywordValuesForLocale(java.lang.String,%20android.icu.util.ULocale,%20boolean)
          put(
              "co",
              new String[] {
                "big5han",
                "compat",
                "dict",
                "direct",
                "ducet",
                "emoji",
                "eor",
                "gb2312",
                "phonebk",
                "phonetic",
                "pinyin",
                "reformed",
                "searchjl",
                "stroke",
                "trad",
                "unihan",
                "zhuyin"
              });

          put(
              "ca",
              new String[] {
                "buddhist",
                "chinese",
                "coptic",
                "dangi",
                "ethioaa",
                "ethiopic",
                "gregory",
                "hebrew",
                "indian",
                "islamic",
                "islamic-umalqura",
                "islamic-tbla",
                "islamic-civil",
                "islamic-rgsa",
                "iso8601",
                "japanese",
                "persian",
                "roc"
              });
        }
      };

  public static boolean isValidKeyword(String key, String value, ILocaleObject<?> localeObject)
      throws JSRangeErrorException {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {

      android.icu.util.ULocale locale = (ULocale) localeObject.getLocale();
      String[] availableValues = new String[] {};
      if (key.equals("co")) {
        // https://tc39.es/ecma402/#sec-intl-collator-internal-slots
        if (value.equals("standard") || value.equals("search")) return false;
        availableValues =
            android.icu.text.RuleBasedCollator.getKeywordValuesForLocale("co", locale, false);
      } else if (key.equals("ca")) {
        availableValues = android.icu.util.Calendar.getKeywordValuesForLocale("ca", locale, false);
      } else if (key.equals("nu")) {
        availableValues = android.icu.text.NumberingSystem.getAvailableNames();
      }

      if (availableValues.length == 0) {
        return true; // When we don't have the list of valid values, assume everything is valid.
      }

      return Arrays.asList(availableValues).contains(value);

    } else {
      if (s_validKeywords.containsKey(key)) {
        return Arrays.asList(s_validKeywords.get(key)).contains(value);
      }

      return true;
    }
  }

  public static Object resolveKnownAliases(String key, Object value) {
    if (key.equals("ca") && JSObjects.isString(value)) {
      return UnicodeExtensionKeys.resolveCalendarAlias((String) value);
    }

    if (key.equals("nu") && JSObjects.isString(value)) {
      return UnicodeExtensionKeys.resolveNumberSystemAlias((String) value);
    }

    if (key.equals("co") && JSObjects.isString(value)) {
      return UnicodeExtensionKeys.resolveCollationAlias((String) value);
    }

    if (key.equals("kn") && JSObjects.isString(value) && value.equals("yes"))
      return JSObjects.newString("true");

    if ((key.equals("kn") || key.equals("kf")) && JSObjects.isString(value) && value.equals("no"))
      return JSObjects.newString("false");

    return value;
  }
}
