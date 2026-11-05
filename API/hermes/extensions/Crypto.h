/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <jsi/jsi.h>

namespace facebook {
namespace hermes {

/// Install the crypto object on the global object, with:
///   - getRandomValues(typedArray) filling an integer TypedArray with
///     cryptographically strong random bytes and returning it, per the
///     W3C Web Crypto API.
///
/// Entropy is drawn directly from the operating system's CSPRNG
/// (CCRandomGenerateBytes on Apple, BCryptGenRandom on Windows,
/// arc4random_buf on Android, getrandom(2) on Linux, getentropy()
/// elsewhere). If entropy cannot be obtained,
/// getRandomValues throws; there is no fallback to a weaker source.
///
/// Deviations from the spec, which requires DOMException types Hermes does
/// not have: a non-integer TypedArray argument throws TypeError (instead of
/// TypeMismatchError) and a view longer than 65536 bytes throws RangeError
/// (instead of QuotaExceededError).
///
/// \param runtime The JSI runtime to install into.
/// \param extensions The precompiled extensions object containing setup
///   functions.
void installCrypto(jsi::Runtime &runtime, jsi::Object &extensions);

} // namespace hermes
} // namespace facebook
