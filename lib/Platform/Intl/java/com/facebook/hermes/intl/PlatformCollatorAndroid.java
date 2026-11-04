/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

import static com.facebook.hermes.intl.IPlatformCollator.Sensitivity.*;
import static com.facebook.hermes.intl.IPlatformCollator.Sensitivity.VARIANT;

import android.os.Build;
import java.text.RuleBasedCollator;
import java.util.ArrayList;
import java.util.Locale;

public class PlatformCollatorAndroid implements IPlatformCollator {

  private RuleBasedCollator mCollator;
  private LocaleObjectAndroid mLocale;

  PlatformCollatorAndroid() {}

  @Override
  @SuppressWarnings("cast")
  public IPlatformCollator configure(ILocaleObject<?> localeObject) throws JSRangeErrorException {
    mLocale = (LocaleObjectAndroid) localeObject;

    assert (Build.VERSION.SDK_INT < Build.VERSION_CODES.N);
    mCollator = (RuleBasedCollator) RuleBasedCollator.getInstance((Locale) mLocale.getLocale());

    // TODO :: I can't find a way to set the decomposition mode.

    return this;
  }

  @Override
  public int compare(String source, String target) {
    return mCollator.compare(source, target);
  }

  @Override
  public Sensitivity getSensitivity() {
    if (mCollator == null) {
      return LOCALE; // TODO: Ad-hoc default
    }

    int strength = mCollator.getStrength();
    if (strength == java.text.Collator.PRIMARY) {
      return BASE;
    }

    if (strength == java.text.Collator.SECONDARY) return ACCENT;

    return VARIANT;
  }

  @Override
  public IPlatformCollator setSensitivity(Sensitivity sensitivity) {
    assert (Build.VERSION.SDK_INT < Build.VERSION_CODES.N);

    switch (sensitivity) {
      case BASE:
        mCollator.setStrength(java.text.Collator.PRIMARY);
        break;
      case ACCENT:
        mCollator.setStrength(java.text.Collator.SECONDARY);
        break;
      case VARIANT:
        mCollator.setStrength(java.text.Collator.TERTIARY);
        break;
      case CASE:
        mCollator.setStrength(android.icu.text.Collator.PRIMARY);
        // TODO: setCaseLevel method is not available. Essentially, the "CASE" sensitivity won't
        // work correctly on older platforms.
    }

    return this;
  }

  @Override
  public IPlatformCollator setIgnorePunctuation(boolean ignore) {
    return this;
  }

  @Override
  public IPlatformCollator setNumericAttribute(boolean numeric) {
    return this;
  }

  @Override
  public IPlatformCollator setCaseFirstAttribute(CaseFirst caseFirst) {
    return this;
  }

  @Override
  public String[] getAvailableLocales() {

    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) {
      // Before L, Locale.toLanguageTag isn't available. Need to figure out how to get a locale id
      // from locale object ... Currently resoring to support only en
      return new String[] {"en"};
    }

    ArrayList<String> availableLocaleIds = new ArrayList<>();
    java.util.Locale[] availableLocales = java.text.Collator.getAvailableLocales();
    for (java.util.Locale locale : availableLocales) {
      availableLocaleIds.add(locale.toLanguageTag()); // TODO:: Not available on platforms <= 20
    }

    String[] availableLocaleIdsArray = new String[availableLocaleIds.size()];
    return availableLocaleIds.toArray(availableLocaleIdsArray);
  }
}
