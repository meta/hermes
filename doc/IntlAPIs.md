---
id: intl
title: Internationalization APIs
---

This document describes the current state of the Android and iOS implementation of the [ECMAScript Internationalization API Specification](https://tc39.es/ecma402/) (ECMA-402, or `Intl`). ECMA-402 is still evolving and the latest iteration that was taken into account is [7th edition](https://402.ecma-international.org/7.0/) which was published in June 2020. Each new edition is built on top of the last one and adds new capabilities typically as:
- New `Intl` service constructors (e.g. `Intl.Collator`, `Intl.NumberFormat` etc.) or extending existing ones by accepting more parameters
- New functions or properties in `Intl` objects (e.g. `Intl.Collator.prototype.compare`)
- New locale aware functions in standard Javascript object prototypes (e.g. `String.prototype.localeCompare`)

One popular implementation strategy followed by other engines, is to bundle an internationalization framework (typically [ICU](http://site.icu-project.org/)) along with the application package. This guarantees deterministic behaviours at the cost of applications package bloat. We decided to consume the Android and iOS platform provided facilities for space efficiency, but at the cost of some variance in behaviours across Android and iOS platforms. This also includes behavioural variations between different versions of Android.

# ECMA-402 Compliance

## Supported on both platforms

- `Intl.Collator`
  - `Intl.Collator.supportedLocalesOf`
  - `Intl.Collator.prototype.compare`
  - `Intl.Collator.prototype.resolvedOptions`

- `Intl.NumberFormat`*
  - `Intl.NumberFormat.supportedLocalesOf`
  - `Intl.NumberFormat.prototype.format`
  - `Intl.NumberFormat.prototype.resolvedOptions`

- `Intl.DateTimeFormat`*
  - `Intl.DateTimeFormat.supportedLocalesOf`
  - `Intl.DateTimeFormat.prototype.format`
  - `Intl.DateTimeFormat.prototype.resolvedOptions`

- `Intl.getCanonicalLocales`

- `String.prototype`
  - `localeCompare`
  - `toLocaleLowerCase`
  - `toLocaleUpperCase`

- `Array.prototype`
  - `toLocaleString`

- `Number.prototype`
  - `toLocaleString`

- `Date.prototype`
  - `toLocaleString`
  - `toLocaleDateString`
  - `toLocaleTimeString`

## Supported on Android only
- `Intl.NumberFormat`
  - `Intl.NumberFormat.prototype.formatToParts`

## * Limitations on property support

### Limited iOS property support
- `Intl.NumberFormat` implementation does not support the following properties,
  - `notation`: 'compact'
  - `notation`: 'engineering'
  - `compactDisplay`
  - `signDisplay`

- `Intl.DateTimeFormat` implementation does not support the following properties,
  - `numberingSystem`
  - [`formatMatcher`](https://tc39.es/ecma402/#sec-basicformatmatcher)

### Limited Android property support

- `Intl.DateTimeFormat` implementation does not support the following properties,
   - [`dayPeriod`](https://github.com/tc39/ecma402/issues/29)
   - [`fractionalSecondDigits`](https://github.com/tc39/ecma402/pull/347)
   - [`formatMatcher`](https://tc39.es/ecma402/#sec-basicformatmatcher) The property enables the implementation to pick the best display format when it supports only a subset of all possible formats. ICU library in Android platform and hence our implementation allows all subsets and formats which makes this `formatMatcher` property unnecessary.

## Limitations across Android SDKs

### Android 11

- The keys of the object returned by `resolvedOptions` function in all `Intl` services are not deterministically ordered as prescribed by spec.
- DateFormat: ECMAScript [beginning of time](https://www.ecma-international.org/ecma-262/11.0/index.html#sec-time-values-and-time-range) (-8,640,000,000,000,000), is formatted as `November 271817`, instead of expected `April 271822`.
- `Intl.NumberFormat` implementation has some rough edges in supporting the following properties,
  - `style`: 'unit'
  - `notation`: 'compact'
  - `signDisplay`
  - `currencyFormat`

### Android 10 and older (SDK < 30)

- `Intl.NumberFormat`: Scientific notation formatting has issues on some cases. e.g. `-Infinity` may get formatted as '-∞E0' instead of expected '-∞'. Another manifestation of the issues is that the formatToParts may return 4 parts instead of 2.
- `Intl.NumberFormat`: Compact notation `formatToParts` doesn't identify unit, hence we report unit as 'literal'. For e.g. the second part of "100ac" gets reported as "literal" instead of "compact"

### Android 9 and older (SDK < 29)

- There are some failures likely due to older Unicode and CLDR version, which are hard to generalize. Some examples are,
  - `Intl.NumberFormat`: 'Percent' is not accepted as a unit.
  - `Intl.NumberFormat`: unit symbols difference, kph vs km/h
  - Some issue in significant digit precision, which is not yet looked into the details.

### Android 8.0 – 8.1 and older (SDK < 28)

- `Intl.getCanonicalLocales`: Unicode/CLDR version differences results in some variances. e.g. und-u-tz-utc vs. und-u-tz-gmt.
- `Intl.NumberFormat`: CompactFormatter doesn't respect the precision inputs.

### Android 7.0 - 7.1 and older (SDK < 26)

- `Intl.getCanonicalLocales`: Unicode/CLDR version differences results in some variances. e.g. und-u-ms-imperial vs. und-u-ms-uksystem.

### Android 7.0 - 7.1 and older (SDK < 24)

- `Intl.Collator`: Doesn't canonically decompose the input strings. Canonically equivalent string with non-identical code points may not match.
- `Intl.getCanonicalLocales`: Unicode/CLDR version differences results in some variances. e.g. und-u-ca-ethiopic-amete-alem vs. und-u-ca-ethioaa, und-u-ks-primary vs. und-u-ks-level1.
- `Intl.NumberFormat`: Unit style does not work.
- `Intl.NumberFormat`: There are issues in the precision configuration due to lack of APIs.
- `Intl.DateFormat`: There are issues with the calendar configuration which needs to be dug into.
   - [`dateStyle/timeStyle`](https://tc39.es/proposal-intl-datetime-style/) is not implemented.

### SDK < 21 and older

On platforms before 21, `Locale.forLanguageTag()` is not available, hence we can't construct `java.util.Locale` object from locale tag. Hence, we fallback to English for any locale input.

# Internationalization framework in Android Platform

Our implementation is essentially projecting the Android platform provided internationalization facilities through the ECMA-402 specified services. It implies that the results of running the same code may vary between devices running different versions of Android.

Android platform internationalization libraries have been based on [ICU4j project](https://unicode-org.github.io/icu-docs/#/icu4j). Version of ICU4j and the backing [CLDR data](http://cldr.unicode.org/) varies across Android platform versions. Also, the ICU APIs were never exposed directly, but only through wrappers and aliases. This results in significant variance in internationalization API surface and data across platform versions.

The following table summarizes ICU, CLDR and Unicode versions available on the Android platforms.

### Platform 24+ where ICU4j APIs are available.

| Android Platform Version | ICU | Unicode | CLDR
| --- | --- | --- | --- |
| Android 11 (API level 30) | ICU4J 66.1 ([ref](https://android.googlesource.com/platform/external/icu/+/refs/heads/android11-mainline-release/icu4j/readme.html)) | Unicode 13 beta | CLDR 36.1 |
| Android 10 (API level 29) | ICU4j 63.2 ([ref](https://developer.android.com/guide/topics/resources/internationalization)) | CLDR 34 | Unicode 11.0 |
| Android 9 (API level 28) | ICU4j 60.2 ([ref](https://developer.android.com/guide/topics/resources/internationalization)) | CLDR 32.0.1 | Unicode 10.0 |
| Android 8.0 - 8.1 (API levels 26 - 27) | ICU4j 58.2( [ref](https://developer.android.com/guide/topics/resources/internationalization)) | CLDR 30.0.3 | Unicode 9.0 |
| Android 7.0 - 7.1 (API levels 24 - 25) | ICU4j 56 ([ref](https://developer.android.com/guide/topics/resources/internationalization))| CLDR 28 | Unicode 8.0 |


### Pre-24 platforms

| Android Platform Version | ICU | Unicode | CLDR
| --- | --- | --- | --- |
| Android 6.0 (API level 23) | ICU4j 55.1 ([ref](https://developer.android.com/guide/topics/resources/internationalization)) | CLDR 27.0.1 | Unicode 7.0 |
| Android 5.0 (API levels 21–22) | ICU4j 53 ([ref](https://developer.android.com/guide/topics/resources/internationalization)) | CLDR 25 | Unicode 6.3 |
| Android 4.4 (API levels 19–20) | ICU4j 51 ([ref](https://developer.android.com/guide/topics/resources/internationalization)) | CLDR 23 | Unicode 6.2 |
| Android 4.3 (API level 18) | ICU4j 50 ([ref](https://developer.android.com/guide/topics/resources/internationalization)) | CLDR 22.1 | Unicode 6.2 |
| Android 4.1 (API levels 16–17) | ICU4j 4.8 ([ref](https://developer.android.com/guide/topics/resources/internationalization)) | CLDR 2.0 | Unicode 6.0 |

<br />

In summary,

1. Platforms >= 24 have much better internationalization support than earlier, as many ICU classes are available as is.

2. Platforms 21-24 still has reasonable internationalization support, by allowing creation of Locale objects and enabling selected ICU services through java.text namespace.

3. Platforms < 21 doesn't allow creation of Locale objects from tags, severely limiting general purpose international code.

4. Platform 30 has introduced classes under [`android.icu.number`](https://developer.android.com/reference/android/icu/util/package-summary) namespace which will majorly improve our `Intl.NumberFormat` implementation

# Impact on Android Application Size

The following numbers are measured using a test application which takes dependency on the Hermes library to evaluate a JavaScript snippet. Essentially, enabling Intl APIs adds 57-62K per ABI.

| **Product APK Size** | **NOINTL** | **INTL** | **DIFF** | **PERC** |
| --- | --- | --- | --- | --- |
| ARM64 | 1,672,235 | 1,729,579 | 57,344 | 3.43% |
| ARM | 1,471,539 | 1,528,883 | 57,344 | 3.90% |
| X86\_64 | 1,844,255 | 1,901,599 | 57,344 | 3.11% |
| X86 | 1,950,739 | 2,012,179 | 61,440 | 3.15% |

The overhead is contributed by both compiled native C++ and Java bits

The uncompressed size of the Hermes shared library got bigger as follows,

| **libhermes.so Size** | **NOINTL** | **INTL** | **DIFF** | **PERC** |
| --- | --- | --- | --- | --- |
| ARM64 | 2,473,760 | 2,551,592 | 77,832 | 3.15% |
| ARM | 1,696,672 | 1,754,016 | 57,344 | 3.38% |
| X86\_64 | 2,633,528 | 2,711,368 | 77,840 | 2.96% |
| X86 | 2,859,916 | 2,945,936 | 86,020 | 3.01% |

And the Java bits got bigger as well,

| **Java Size** | **NOINTL** | **INTL** | **DIFF** | **PERC** |
| --- | --- | --- | --- | --- |
| classes.jar<br />(in hermes.aar) | 559 | 120975 | 120,416 | 21541.32% |
| classes.dex<br />(replapp.apk) | 160708 | 234808 | 74,100 | 46.11% |

_Please note that the application dex file contains non-hermes class files too._

And finally, this is the increase in the final npm package,

| **NPM Package** | **NOINTL** | **INTL** | **DIFF** | **PERC** |
| --- | --- | --- | --- | --- |
| hermes | 214447973 | 219291220 | 4,843,247 | 2.26% |
