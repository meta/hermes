/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

import android.icu.util.ULocale;
import android.os.Build;
import com.facebook.proguard.annotations.DoNotStrip;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * This class represents the Java part of the Android Intl.Collator implementation. The interaction
 * with the Hermes JaveScript internals are implemented in C++ and should not generally need to be
 * changed. Implementers' notes here will describe what parts of the ECMA 402 spec remain to be
 * implemented.
 *
 * <p>Also see the implementer' notes on DateTimeFormat.java.
 */
@DoNotStrip
public class Collator {

  // [[RelevantExtensionKeys]]
  // Ref: https://tc39.es/ecma402/#sec-intl.locale-internal-slots
  // According to ecma401, for locale objects, The value of the [[RelevantExtensionKeys]] internal
  // slot is « "ca", "co", "hc", "kf", "kn", "nu" ».
  // https://tc39.es/ecma402/#sec-intl-collator-internal-slots
  // And, for collator object, The value of the [[RelevantExtensionKeys]] internal slot is a List
  // that must include the element "co", may include any or all of the elements "kf" and "kn",
  // There doesn't seem to exist a clean way to map [[RelevantExtensionKeys]] with ICU.
  // ICU collator's support keywords ("android.icu.text.RuleBasedCollator.getKeywords()) which
  // currently support (only) one "collation", which maps cleanly to the unicode extension "co"
  // ICU collator currently doesn't support the other two ("kf", "kn") as keywords/extensions .. but
  // there exists methods "setNumericCollation", "setUpperCaseFirst" etc. which has same
  // functionality.
  //
  // Hence, in effect the value of [[RelevantExtensionKeys]] is ["co", "kf", "kn"] with our current
  // implementation.
  // As this is a static fact, our code assumes the above instead of dynamic validation as
  // prescribed in the pseudo code in ecma402

  // Internal slots as defined by https://tc39.es/ecma402/#sec-intl.collator
  // Let internalSlotsList be « [[InitializedCollator]], [[Locale]], [[Usage]], [[Sensitivity]],
  // [[IgnorePunctuation]], [[Collation]], [[BoundCompare]] ».
  // Note: [[BoundCompare]] is likely implemented in native layers ..
  // And we add [[Numeric]] and [[CaseFirst]] slots unconditionally as described above.

  // 11.1.2 2-4
  private IPlatformCollator.Usage mResolvedUsage;
  private IPlatformCollator.Sensitivity mResolvedSensitivity;
  private boolean mResolvedIgnorePunctuation;
  private String mResolvedCollation = Constants.COLLATION_DEFAULT;

  // These two slots needs to differentiate between "unset" and "default" .. These shouldn't be part
  // of resolvedOptions if "unset"
  // And these can be set through options as well as extensions ..
  private boolean mResolvedNumeric;

  private IPlatformCollator.CaseFirst mResolvedCaseFirst;

  // [[Locale]]
  private ILocaleObject<?> mResolvedLocaleObject;

  // This is a hacky way to avoid the "search" collation value from being shown in "resolvedOptions"
  // ..
  // ICU Collation object doesn't expose any API to specify the "search" collation .. hence we have
  // to resort to adding "-co-search" extension to locale id, which is prohibited by the ECMAScript
  // spec.
  private ILocaleObject<?> mResolvedLocaleObjectForResolvedOptions;

  // [[InitializedCollator]]
  private IPlatformCollator mPlatformCollatorObject;

  // The default locale that locale resolution fell back to, or null if a requested locale was used.
  private Object mDefaultLocale;

  // Must read the same locale as LocaleObject.createDefault(), or defaultLocaleChanged() compares
  // the wrong value.
  private static Object currentDefaultLocale() {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
      return ULocale.getDefault(ULocale.Category.FORMAT);
    } else {
      return Locale.getDefault();
    }
  }

  // https://tc39.es/ecma402/#sec-initializecollator
  private void initializeCollator(List<String> locales, Map<String, Object> options)
      throws JSRangeErrorException {

    // 1.
    // Canonicalize Locale List
    // We don't explicitlULy canonicalize but it is implicitly happening while creating Locale
    // objects from the tag.

    // 4 & 5
    Object usage =
        OptionHelpers.GetOption(
            options,
            "usage",
            OptionHelpers.OptionType.STRING,
            Constants.COLLATOR_USAGE_POSSIBLE_VALUES,
            Constants.SORT);
    mResolvedUsage =
        OptionHelpers.searchEnum(IPlatformCollator.Usage.class, JSObjects.getJavaString(usage));

    // We don't have 6 & 7
    // Note: We don't know a way to map the 'usage' option to "LocaleData" parameter to be used
    // while resolving locales, with ICU.
    // With ICU, the only way to specify 'search' usage is through 'co=search' unicode extension ..
    // (which is explicitly deprecated in ecma402, but internally we use this ! )

    Object opt = JSObjects.newObject();

    Object matcher =
        OptionHelpers.GetOption(
            options,
            Constants.LOCALEMATCHER,
            OptionHelpers.OptionType.STRING,
            Constants.LOCALEMATCHER_POSSIBLE_VALUES,
            Constants.LOCALEMATCHER_BESTFIT);
    JSObjects.Put(opt, "localeMatcher", matcher);

    Object numeric =
        OptionHelpers.GetOption(
            options,
            "numeric",
            OptionHelpers.OptionType.BOOLEAN,
            JSObjects.Undefined(),
            JSObjects.Undefined());
    if (!JSObjects.isUndefined(numeric))
      numeric = JSObjects.newString(String.valueOf(JSObjects.getJavaBoolean(numeric)));
    JSObjects.Put(opt, "kn", numeric);

    Object caseFirst =
        OptionHelpers.GetOption(
            options,
            "caseFirst",
            OptionHelpers.OptionType.STRING,
            Constants.CASEFIRST_POSSIBLE_VALUES,
            JSObjects.Undefined());
    JSObjects.Put(opt, "kf", caseFirst);

    // https://tc39.es/ecma402/#sec-intl-collator-internal-slots
    List<String> relevantExtensionKeys = Arrays.asList("co", "kf", "kn");

    HashMap<String, Object> r = LocaleResolver.resolveLocale(locales, opt, relevantExtensionKeys);

    mResolvedLocaleObject = (ILocaleObject<?>) JSObjects.getJavaMap(r).get("locale");
    mDefaultLocale = JSObjects.getJavaMap(r).get("defaultLocale");
    mResolvedLocaleObjectForResolvedOptions = mResolvedLocaleObject.cloneObject();

    Object collation = JSObjects.Get(r, "co");
    if (JSObjects.isNull(collation)) collation = JSObjects.newString(Constants.COLLATION_DEFAULT);
    mResolvedCollation = JSObjects.getJavaString(collation);

    Object numericCollation = JSObjects.Get(r, "kn");
    if (JSObjects.isNull(numericCollation)) mResolvedNumeric = false;
    else {
      String numericCollationValue = JSObjects.getJavaString(numericCollation);
      mResolvedNumeric = Boolean.parseBoolean(numericCollationValue);
    }

    Object caseFirstCollation = JSObjects.Get(r, "kf");
    if (JSObjects.isNull(caseFirstCollation)) caseFirstCollation = JSObjects.newString("false");

    mResolvedCaseFirst =
        OptionHelpers.searchEnum(
            IPlatformCollator.CaseFirst.class, JSObjects.getJavaString(caseFirstCollation));

    // Note:: We can't find any other way to force the collator to use search rules .
    if (mResolvedUsage == IPlatformCollator.Usage.SEARCH) {
      ArrayList<String> currentCollationExtensions =
          mResolvedLocaleObject.getUnicodeExtensions(Constants.COLLATION_EXTENSION_KEY_LONG);

      ArrayList<String> currentResolvedCollationExtensions = new ArrayList<>();
      for (String currentCollationExtension : currentCollationExtensions) {
        currentResolvedCollationExtensions.add(
            UnicodeExtensionKeys.resolveCollationAlias(currentCollationExtension));
      }

      currentResolvedCollationExtensions.add(
          UnicodeExtensionKeys.resolveCollationAlias(Constants.SEARCH));
      mResolvedLocaleObject.setUnicodeExtensions(
          Constants.COLLATION_EXTENSION_KEY_SHORT, currentResolvedCollationExtensions);
    }

    Object sensitivity =
        OptionHelpers.GetOption(
            options,
            Constants.COLLATION_OPTION_SENSITIVITY,
            OptionHelpers.OptionType.STRING,
            Constants.SENSITIVITY_POSSIBLE_VALUES,
            JSObjects.Undefined());
    if (!JSObjects.isUndefined(sensitivity)) {
      mResolvedSensitivity =
          OptionHelpers.searchEnum(
              IPlatformCollator.Sensitivity.class, JSObjects.getJavaString(sensitivity));
    } else {

      if (mResolvedUsage == IPlatformCollator.Usage.SORT)
        mResolvedSensitivity = IPlatformCollator.Sensitivity.VARIANT;
      else mResolvedSensitivity = IPlatformCollator.Sensitivity.LOCALE;
    }

    Object ignorePunctuation =
        OptionHelpers.GetOption(
            options,
            Constants.COLLATION_OPTION_IGNOREPUNCTUATION,
            OptionHelpers.OptionType.BOOLEAN,
            JSObjects.Undefined(),
            false);
    mResolvedIgnorePunctuation = JSObjects.getJavaBoolean(ignorePunctuation);
  }

  // options are usage:string, localeMatcher:string, numeric:boolean, caseFirst:string,
  // sensitivity:string, ignorePunctuation:boolean
  //
  // Implementer note: The ctor corresponds roughly to
  // https://tc39.es/ecma402/#sec-initializecollator
  // Also see the implementer notes on DateTimeFormat#DateTimeFormat()
  @DoNotStrip
  public Collator(List<String> locales, Map<String, Object> options) throws JSRangeErrorException {

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
      mPlatformCollatorObject = new PlatformCollatorICU();
    } else {
      mPlatformCollatorObject = new PlatformCollatorAndroid();
    }

    initializeCollator(locales, options);

    mPlatformCollatorObject
        .configure(mResolvedLocaleObject)
        .setNumericAttribute(mResolvedNumeric)
        .setCaseFirstAttribute(mResolvedCaseFirst)
        .setSensitivity(mResolvedSensitivity)
        .setIgnorePunctuation(mResolvedIgnorePunctuation);
  }

  // options are localeMatcher:string
  //
  // Implementer note: This method corresponds roughly to
  // https://tc39.es/ecma402/#sec-intl.collator.supportedlocalesof
  //
  // The notes on DateTimeFormat#DateTimeFormat() for Locales and
  // Options also apply here.
  @DoNotStrip
  public static List<String> supportedLocalesOf(List<String> locales, Map<String, Object> options)
      throws JSRangeErrorException {
    String matcher =
        JSObjects.getJavaString(
            OptionHelpers.GetOption(
                options,
                Constants.LOCALEMATCHER,
                OptionHelpers.OptionType.STRING,
                Constants.LOCALEMATCHER_POSSIBLE_VALUES,
                Constants.LOCALEMATCHER_BESTFIT));
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && matcher.equals("best fit")) {
      return Arrays.asList(
          LocaleMatcher.bestFitSupportedLocales(locales.toArray(new String[locales.size()])));
    } else {
      return Arrays.asList(
          LocaleMatcher.lookupSupportedLocales(locales.toArray(new String[locales.size()])));
    }
  }

  // Implementer note: This method corresponds roughly to
  // https://tc39.es/ecma402/#sec-intl.collator.prototype.resolvedoptions
  @DoNotStrip
  public Map<String, Object> resolvedOptions() throws JSRangeErrorException {
    HashMap<String, Object> finalResolvedOptions = new LinkedHashMap<>();
    String finalResolvedLocaleId = mResolvedLocaleObjectForResolvedOptions.toCanonicalTag();
    // An example of going extra mile to adhere to spec !! .. It looks wierd though, but i believe
    // it is the right thing to do ..
    finalResolvedLocaleId = finalResolvedLocaleId.replace("-kn-true", "-kn");
    finalResolvedOptions.put(Constants.LOCALE, finalResolvedLocaleId);

    finalResolvedOptions.put(Constants.COLLATION_OPTION_USAGE, mResolvedUsage.toString());

    if (mResolvedSensitivity == IPlatformCollator.Sensitivity.LOCALE) {
      IPlatformCollator.Sensitivity defaultSensitivity = mPlatformCollatorObject.getSensitivity();
      finalResolvedOptions.put(
          Constants.COLLATION_OPTION_SENSITIVITY, defaultSensitivity.toString());
    } else {
      finalResolvedOptions.put(
          Constants.COLLATION_OPTION_SENSITIVITY, mResolvedSensitivity.toString());
    }

    finalResolvedOptions.put(
        Constants.COLLATION_OPTION_IGNOREPUNCTUATION, mResolvedIgnorePunctuation);
    finalResolvedOptions.put(Constants.COLLATION, mResolvedCollation);

    finalResolvedOptions.put(Constants.COLLATION_OPTION_NUMERIC, mResolvedNumeric);

    finalResolvedOptions.put(Constants.COLLATION_OPTION_CASEFIRST, mResolvedCaseFirst.toString());

    return finalResolvedOptions;
  }

  // Implementer note: This method corresponds roughly to
  // https://tc39.es/ecma402/#sec-collator-comparestrings
  @DoNotStrip
  public double compare(String source, String target) {
    return mPlatformCollatorObject.compare(source, target);
  }

  // Whether this collator uses a default locale that is no longer the default, in which case a
  // collator created now from the same locales and options would resolve differently.
  @DoNotStrip
  public boolean defaultLocaleChanged() {
    return mDefaultLocale != null && !mDefaultLocale.equals(currentDefaultLocale());
  }
}
