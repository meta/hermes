/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: TZ=GMT %hermes -O -target=HBC %s
// RUN: TZ=GMT %shermes -O -exec %s
// REQUIRES: intl && apple

var date = new Date(Date.UTC(2020, 0, 2, 3, 45));

function hourPart(locale, options) {
  return new Intl.DateTimeFormat(locale, {
    hour: options.hour,
    hourCycle: options.hourCycle,
    timeZone: 'UTC',
  }).formatToParts(date).find(part => part.type === 'hour').value;
}

if (hourPart('en-US', {hour: '2-digit', hourCycle: 'h12'}) !== '03') {
  throw new Error('12-hour clock lost its requested hour width');
}
if (hourPart('en-US', {hour: '2-digit', hourCycle: 'h23'}) !== '03') {
  throw new Error('24-hour clock lost its requested hour width');
}
if (!new Intl.DateTimeFormat('ja-JP', {
  hour: '2-digit',
  hourCycle: 'h23',
  timeZone: 'UTC',
}).format(date).startsWith('03')) {
  throw new Error('localized hour pattern lost its requested width');
}
if (hourPart('en-US', {hour: 'numeric', hourCycle: 'h12'}) !== '3') {
  throw new Error('numeric hour unexpectedly gained a leading zero');
}

if (new Intl.DateTimeFormat('de-DE', {
  hour: 'numeric',
  minute: 'numeric',
  timeZone: 'UTC',
}).format(date) !== '03:45') {
  throw new Error('numeric hour lost its localized width');
}

['toLocaleString', 'toLocaleTimeString'].forEach(method => {
  if (date[method]('de-DE', {
    hour: 'numeric',
    minute: 'numeric',
    timeZone: 'UTC',
  }) !== '03:45') {
    throw new Error(method + ' changed numeric hour formatting');
  }
  if (date[method]('ja-JP', {
    hour: '2-digit',
    minute: '2-digit',
    hourCycle: 'h23',
    timeZone: 'UTC',
  }) !== '03:45') {
    throw new Error(method + ' lost the requested two-digit hour');
  }
});

if (date.toLocaleTimeString('de-DE', {timeZone: 'UTC'}) !== '03:45:00') {
  throw new Error('toLocaleTimeString changed default hour formatting');
}
if (date.toLocaleString('de-DE', {timeZone: 'UTC'}) !==
    '2.1.2020, 03:45:00') {
  throw new Error('toLocaleString changed default hour formatting');
}
