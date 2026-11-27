/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

import android.icu.util.ULocale;
import android.os.Build;
import androidx.annotation.RequiresApi;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;

public class LocaleMatcher {

  public static class LocaleMatchResult {
    public ILocaleObject<?> matchedLocale;
    public HashMap<String, String> extensions = new HashMap<>();
    // The default locale the match fell back to because no requested locale is available, or null.
    public Object defaultLocale;
  }

  // https://tc39.es/ecma402/#sec-bestavailablelocale
  public static String BestAvailableLocale(String[] availableLocales, String locale) {
    String candidate = locale;
    while (true) {
      // TODO:: Method take String list as argument to avoid conversions between array and list?
      // TODO:: AvailableLocales list always seems to be sorted. Is it guaranteed so that we can
      // binary search int it ? Can we cheaply ensure that it is sorted ? Do we do multiple searches
      // to justify sorting ?
      if (Arrays.asList(availableLocales).indexOf(candidate) > -1) return candidate;

      int pos = candidate.lastIndexOf("-");
      if (pos < 0) return ""; // We treat empty string as "undefined"

      if (pos >= 2
          && candidate.charAt(pos - 2)
              == '-') // This is very likely unnecessary as this function is called after removing
        // extensions.
        pos -= 2;

      candidate = candidate.substring(0, pos);
    }
  }

  // https://tc39.es/ecma402/#sec-lookupmatcher
  public static LocaleMatchResult lookupMatch(String[] requestedLocales, String[] availableLocales)
      throws JSRangeErrorException {

    LocaleMatchResult result = new LocaleMatchResult();
    for (String locale : requestedLocales) {
      ILocaleObject<?> requestedLocaleObject = LocaleObject.createFromLocaleId(locale);
      String noExtensionLocale = requestedLocaleObject.toCanonicalTagWithoutExtensions();

      String availableLocale = BestAvailableLocale(availableLocales, noExtensionLocale);
      if (!availableLocale.isEmpty()) {
        result.matchedLocale = LocaleObject.createFromLocaleId(availableLocale);
        result.extensions = requestedLocaleObject.getUnicodeExtensions();
        return result;
      }
    }

    result.matchedLocale = LocaleObject.createDefault();
    result.defaultLocale = result.matchedLocale.getLocale();
    return result;
  }

  public static String[] getAvailableLocales() {

    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) {
      // Before L, Locale.toLanguageTag isn't available. Need to figure out how to get a locale id
      // from locale object ... Currently resoring to support only en
      return new String[] {"en"};
    }

    ArrayList<String> availableLocaleIds = new ArrayList<>();
    java.util.Locale[] availableLocales = java.util.Locale.getAvailableLocales();
    for (java.util.Locale locale : availableLocales) {
      availableLocaleIds.add(locale.toLanguageTag()); // TODO:: Not available on platforms <= 20
    }

    String[] availableLocaleIdsArray = new String[availableLocaleIds.size()];
    return availableLocaleIds.toArray(availableLocaleIdsArray);
  }

  // https://tc39.es/ecma402/#sec-lookupmatcher
  public static LocaleMatchResult lookupMatch(String[] requestedLocales)
      throws JSRangeErrorException {

    String[] availableLocales = getAvailableLocales();
    return lookupMatch(requestedLocales, availableLocales);
  }

  // https://tc39.es/ecma402/#sec-lookupsupportedlocales
  public static String[] lookupSupportedLocales(String[] requestedLocales)
      throws JSRangeErrorException {
    ArrayList<String> subset = new ArrayList<>();
    String[] availableLocales = getAvailableLocales();

    for (String requestedLocale : requestedLocales) {
      String noExtensionLocale =
          LocaleObject.createFromLocaleId(requestedLocale).toCanonicalTagWithoutExtensions();
      String availableLocale = BestAvailableLocale(availableLocales, noExtensionLocale);
      if (availableLocale != null && !availableLocale.isEmpty()) subset.add(requestedLocale);
    }

    String[] subsetArray = new String[subset.size()];
    return subset.toArray(subsetArray);
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  public static ULocale bestFitBestAvailableLocale(ILocaleObject<?> requestedLocaleObject)
      throws JSRangeErrorException {
    ULocale[] availableLocales = ULocale.getAvailableLocales();

    android.icu.util.ULocale requestedULocaleWithoutExtensions =
        (android.icu.util.ULocale) requestedLocaleObject.getLocaleWithoutExtensions();
    android.icu.util.ULocale[] requestedLocalesArray =
        new android.icu.util.ULocale[] {requestedULocaleWithoutExtensions};
    boolean[] fallback = new boolean[1];

    android.icu.util.ULocale acceptedLocale =
        android.icu.util.ULocale.acceptLanguage(requestedLocalesArray, availableLocales, fallback);

    // Process if there is a match without fallback to ROOT
    if (!fallback[0] && acceptedLocale != null) {
      return acceptedLocale;
    }

    return null;
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  public static LocaleMatchResult bestFitMatch(String[] requestedLocales)
      throws JSRangeErrorException {
    LocaleMatchResult result = new LocaleMatchResult();
    for (String requestedLocale : requestedLocales) {
      ILocaleObject<?> requestedLocaleObject = LocaleObject.createFromLocaleId(requestedLocale);
      ULocale availableLocale = bestFitBestAvailableLocale(requestedLocaleObject);
      if (availableLocale != null) {
        result.matchedLocale = LocaleObjectICU.createFromULocale(availableLocale);
        result.extensions = requestedLocaleObject.getUnicodeExtensions();
        return result;
      }
    }

    result.matchedLocale = LocaleObjectICU.createDefault();
    result.defaultLocale = result.matchedLocale.getLocale();
    return result;
  }

  @RequiresApi(api = Build.VERSION_CODES.N)
  public static String[] bestFitSupportedLocales(String[] requestedLocales)
      throws JSRangeErrorException {
    ArrayList<String> subset = new ArrayList<>();
    for (String requestedLocale : requestedLocales) {
      ULocale uLocale =
          bestFitBestAvailableLocale(LocaleObject.createFromLocaleId(requestedLocale));
      if (uLocale != null) subset.add(requestedLocale);
    }

    String[] subsetArray = new String[subset.size()];
    return subset.toArray(subsetArray);
  }
}
