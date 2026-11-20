/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Crypto.h"

#include "JSIUtils.h"

#if defined(__APPLE__)
#include <CommonCrypto/CommonRandom.h>
#elif defined(_WIN32)
#include <windows.h>

#include <bcrypt.h>
#elif defined(__ANDROID__)
#include <stdlib.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#else
// Everything else (Emscripten, the BSDs) is expected to provide getentropy().
// A platform with no known OS CSPRNG interface fails to compile rather than
// silently falling back to a weaker source.
#include <unistd.h>

#include <algorithm>
#endif

namespace facebook {
namespace hermes {
namespace {

/// Throw a JSError reporting that the OS entropy source failed.
[[noreturn]] void throwEntropyError(jsi::Runtime &rt) {
  throw jsi::JSError(
      rt, "crypto.getRandomValues: failed to obtain entropy from the OS");
}

/// Fill \p buf with \p len cryptographically strong random bytes obtained
/// directly from the operating system's CSPRNG, or throw a JSError if entropy
/// cannot be obtained. There is deliberately no fallback to a weaker source
/// (userspace PRNGs, time seeds, or the CPU's RDRAND alone): callers such as
/// key generation must get kernel-pool entropy or an error.
/// std::random_device is unsuitable here: the C++ standard permits it to be a
/// deterministic PRNG (and older MinGW-w64 shipped exactly that), and
/// libstdc++ on x86-64 services it from RDSEED/RDRAND alone, bypassing the
/// kernel pool.
void secureRandomBytes(jsi::Runtime &rt, uint8_t *buf, size_t len) {
  if (len == 0) {
    return;
  }
#if defined(__APPLE__)
  if (CCRandomGenerateBytes(buf, len) != kCCSuccess) {
    throwEntropyError(rt);
  }
#elif defined(_WIN32)
  if (!BCRYPT_SUCCESS(BCryptGenRandom(
          nullptr,
          buf,
          static_cast<ULONG>(len),
          BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    throwEntropyError(rt);
  }
#elif defined(__ANDROID__)
  // Bionic's recommended CSPRNG interface, available at every API level.
  // The getrandom(2) syscall cannot be used unconditionally here: Hermes
  // supports minSdk 24 (Android 7.0), and supported Nougat devices shipped
  // on kernel 3.10 (e.g. Nexus 6/9), which predates the syscall (added in
  // 3.17), so it would always fail with ENOSYS there. arc4random_buf is
  // OpenBSD's ChaCha20 generator keyed from kernel entropy (via getrandom
  // on kernels that have it), and it aborts rather than returning weak
  // output if it cannot be seeded, so this is not a fallback to a weaker
  // source.
  arc4random_buf(buf, len);
#elif defined(__linux__)
  // Use the getrandom(2) syscall directly: the libc wrapper is missing on
  // older glibc (< 2.25), but the syscall exists in every kernel >= 3.17.
  // Unlike reading /dev/urandom, it also blocks until the kernel entropy
  // pool has been initialized. On kernels without the syscall (ENOSYS) this
  // throws instead of falling back.
  while (len > 0) {
    long n = syscall(SYS_getrandom, buf, len, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throwEntropyError(rt);
    }
    buf += n;
    len -= static_cast<size_t>(n);
  }
#else
  // getentropy() serves at most 256 bytes per call.
  while (len > 0) {
    size_t chunk = std::min<size_t>(len, 256);
    if (getentropy(buf, chunk) != 0) {
      throwEntropyError(rt);
    }
    buf += chunk;
    len -= chunk;
  }
#endif
}

/// Native helper backing crypto.getRandomValues(). The JS wrapper has already
/// validated that args[0] is an integer TypedArray of at most 65536 bytes;
/// this fills its underlying bytes with cryptographically strong random data.
jsi::Value cryptoRandomFill(
    jsi::Runtime &rt,
    const jsi::Value &,
    const jsi::Value *args,
    size_t count) {
  // Defensive check: we expect exactly 1 argument from our JS wrapper.
  if (count != 1) {
    throw jsi::JSError(rt, "cryptoRandomFill requires exactly 1 argument");
  }

  TypedArrayBufferInfo info = getTypedArrayBuffer(
      rt,
      args[0],
      "crypto.getRandomValues: argument must be an integer TypedArray",
      "crypto.getRandomValues called on a detached ArrayBuffer");

  secureRandomBytes(rt, info.data(), info.byteLength());

  return jsi::Value::undefined();
}

} // namespace

void installCrypto(jsi::Runtime &rt, jsi::Object &extensions) {
  // Get the setup function from the precompiled extensions object
  jsi::Function setup = extensions.getPropertyAsFunction(rt, "Crypto");

  // Create the native helper function
  jsi::Function nativeRandomFill = jsi::Function::createFromHostFunction(
      rt,
      jsi::PropNameID::forAscii(rt, "cryptoRandomFill"),
      1,
      cryptoRandomFill);

  // Call the setup function with our native helper
  setup.call(rt, nativeRandomFill);
}

} // namespace hermes
} // namespace facebook
