/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

import android.os.Build;

public class LocaleObject {
  @SuppressWarnings("rawtypes")
  public static ILocaleObject createDefault() {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) return LocaleObjectICU.createDefault();
    else return LocaleObjectAndroid.createDefault();
  }

  @SuppressWarnings("rawtypes")
  public static ILocaleObject createFromLocaleId(String localeId) throws JSRangeErrorException {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N)
      return LocaleObjectICU.createFromLocaleId(localeId);
    else return LocaleObjectAndroid.createFromLocaleId(localeId);
  }
}
