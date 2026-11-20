/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_impl.h"

#include "hermes/Support/UTF8.h"
#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/StringPrimitive.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/Support/ConvertUTF.h"

#include <cstring>
#include <string>

//===========================================================================
// String creation
//===========================================================================

napi_status NAPI_CDECL napi_create_string_utf8(
    napi_env env,
    const char *str,
    size_t length,
    napi_value *result) {
  CHECK_ENV(env);
  if (length > 0) {
    CHECK_ARG(env, str);
  }
  CHECK_ARG(env, result);
  RETURN_STATUS_IF_FALSE(
      env, (length == NAPI_AUTO_LENGTH) || length <= INT_MAX, napi_invalid_arg);

  if (length == NAPI_AUTO_LENGTH) {
    length = std::strlen(str);
  }

  const auto *utf8 = reinterpret_cast<const uint8_t *>(str);

  // Fast path: if all bytes are ASCII, create directly.
  if (hermes::isAllASCII(utf8, utf8 + length)) {
    hermes::vm::GCScope gcScope(env->runtime);
    auto strRes = hermes::vm::StringPrimitive::createEfficient(
        env->runtime, hermes::vm::ASCIIRef(str, length));
    if (strRes == hermes::vm::ExecutionStatus::EXCEPTION) {
      return napi_set_last_error(env, napi_generic_failure);
    }
    *result = env->addToCurrentScope(*strRes);
    return napi_clear_last_error(env);
  }

  // Non-ASCII path: decode UTF-8 to UTF-16, replacing invalid
  // sequences with U+FFFD to match V8/Node.js behavior.
  // The UTF-16 length is at most the UTF-8 byte length (each byte
  // produces at most one code unit), plus one extra for surrogate
  // pairs from 4-byte sequences (but those consume 4 input bytes
  // and produce 2 output units, so the bound still holds).
  llvh::SmallVector<char16_t, 128> buf;
  buf.reserve(length);
  // decodeUTF8 reads the continuation bytes of a sequence without checking
  // for the end, and relies on a terminator to stop at the end of the input.
  // str is only valid for length bytes, so decode a null-terminated copy.
  std::string terminated(str, length);
  const char *cur = terminated.c_str();
  const char *end = cur + length;
  while (cur < end) {
    uint32_t cp = hermes::decodeUTF8<false>(cur, [](const llvh::Twine &) {});
    auto inserter = std::back_inserter(buf);
    hermes::encodeUTF16(inserter, cp);
  }

  hermes::vm::GCScope gcScope(env->runtime);
  auto strRes = hermes::vm::StringPrimitive::createEfficient(
      env->runtime, hermes::vm::UTF16Ref(buf.data(), buf.size()));
  if (strRes == hermes::vm::ExecutionStatus::EXCEPTION) {
    return napi_set_last_error(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(*strRes);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_string_latin1(
    napi_env env,
    const char *str,
    size_t length,
    napi_value *result) {
  CHECK_ENV(env);
  if (length > 0) {
    CHECK_ARG(env, str);
  }
  CHECK_ARG(env, result);
  RETURN_STATUS_IF_FALSE(
      env, (length == NAPI_AUTO_LENGTH) || length <= INT_MAX, napi_invalid_arg);

  if (length == NAPI_AUTO_LENGTH) {
    length = std::strlen(str);
  }

  // Latin-1 (ISO 8859-1) code points 0x00-0xFF map directly to
  // Unicode code points U+0000-U+00FF. Widen each byte to char16_t
  // and create via UTF16Ref. StringPrimitive::createEfficient will
  // detect if the content is all-ASCII and store it compactly.
  llvh::SmallVector<char16_t, 128> buf(length);
  const auto *src = reinterpret_cast<const uint8_t *>(str);
  for (size_t i = 0; i < length; ++i) {
    buf[i] = static_cast<char16_t>(src[i]);
  }

  hermes::vm::GCScope gcScope(env->runtime);
  auto strRes = hermes::vm::StringPrimitive::createEfficient(
      env->runtime, hermes::vm::UTF16Ref(buf.data(), length));
  if (strRes == hermes::vm::ExecutionStatus::EXCEPTION) {
    return napi_set_last_error(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(*strRes);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_string_utf16(
    napi_env env,
    const char16_t *str,
    size_t length,
    napi_value *result) {
  CHECK_ENV(env);
  if (length > 0) {
    CHECK_ARG(env, str);
  }
  CHECK_ARG(env, result);
  RETURN_STATUS_IF_FALSE(
      env, (length == NAPI_AUTO_LENGTH) || length <= INT_MAX, napi_invalid_arg);

  if (length == NAPI_AUTO_LENGTH) {
    length = std::char_traits<char16_t>::length(str);
  }

  // A GCScope is needed because createEfficient may internally
  // allocate temporary Handle objects.
  hermes::vm::GCScope gcScope(env->runtime);
  auto strRes = hermes::vm::StringPrimitive::createEfficient(
      env->runtime, hermes::vm::UTF16Ref(str, length));
  if (strRes == hermes::vm::ExecutionStatus::EXCEPTION) {
    return napi_set_last_error(env, napi_generic_failure);
  }

  *result = env->addToCurrentScope(*strRes);
  return napi_clear_last_error(env);
}

//===========================================================================
// External string creation
//===========================================================================

napi_status NAPI_CDECL node_api_create_external_string_latin1(
    napi_env env,
    char *str,
    size_t length,
    node_api_basic_finalize finalize_callback,
    void *finalize_hint,
    napi_value *result,
    bool *copied) {
  // Hermes does not support external string storage. We copy the data
  // and set *copied = true, then call the finalizer so the caller can
  // free the original buffer.
  napi_status status = napi_create_string_latin1(env, str, length, result);
  if (status == napi_ok) {
    if (copied != nullptr) {
      *copied = true;
    }
    if (finalize_callback) {
      finalize_callback(env, str, finalize_hint);
    }
  }
  return status;
}

napi_status NAPI_CDECL node_api_create_external_string_utf16(
    napi_env env,
    char16_t *str,
    size_t length,
    node_api_basic_finalize finalize_callback,
    void *finalize_hint,
    napi_value *result,
    bool *copied) {
  // Hermes does not support external string storage. We copy the data
  // and set *copied = true, then call the finalizer so the caller can
  // free the original buffer.
  napi_status status = napi_create_string_utf16(env, str, length, result);
  if (status == napi_ok) {
    if (copied != nullptr) {
      *copied = true;
    }
    if (finalize_callback) {
      finalize_callback(env, str, finalize_hint);
    }
  }
  return status;
}

//===========================================================================
// Property key creation
//===========================================================================

napi_status NAPI_CDECL node_api_create_property_key_latin1(
    napi_env env,
    const char *str,
    size_t length,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  if (length > 0) {
    CHECK_ARG(env, str);
  }
  CHECK_ARG(env, result);
  RETURN_STATUS_IF_FALSE(
      env, (length == NAPI_AUTO_LENGTH) || length <= INT_MAX, napi_invalid_arg);

  if (length == NAPI_AUTO_LENGTH) {
    length = std::strlen(str);
  }

  auto &runtime = env->runtime;

  // Latin-1 code points 0x00-0xFF map directly to Unicode U+0000-U+00FF.
  // Check if all bytes are ASCII — if so, we can use ASCIIRef directly.
  const auto *src = reinterpret_cast<const uint8_t *>(str);
  if (hermes::isAllASCII(src, src + length)) {
    hermes::vm::GCScope gcScope(runtime);
    auto symRes = runtime.getIdentifierTable().getSymbolHandle(
        runtime, hermes::vm::ASCIIRef(str, length));
    if (LLVM_UNLIKELY(symRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
      return napi_set_last_error(env, napi_generic_failure);
    }
    auto *strPrim = runtime.getStringPrimFromSymbolID(**symRes);
    *result = env->addToCurrentScope(
        hermes::vm::HermesValue::encodeStringValue(strPrim));
    return napi_clear_last_error(env);
  }

  // Non-ASCII Latin-1: widen each byte to char16_t.
  llvh::SmallVector<char16_t, 128> buf(length);
  for (size_t i = 0; i < length; ++i) {
    buf[i] = static_cast<char16_t>(src[i]);
  }

  hermes::vm::GCScope gcScope(runtime);
  auto symRes = runtime.getIdentifierTable().getSymbolHandle(
      runtime, hermes::vm::UTF16Ref(buf.data(), length));
  if (LLVM_UNLIKELY(symRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return napi_set_last_error(env, napi_generic_failure);
  }
  auto *strPrim = runtime.getStringPrimFromSymbolID(**symRes);
  *result = env->addToCurrentScope(
      hermes::vm::HermesValue::encodeStringValue(strPrim));
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_create_property_key_utf8(
    napi_env env,
    const char *str,
    size_t length,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  if (length > 0) {
    CHECK_ARG(env, str);
  }
  CHECK_ARG(env, result);
  RETURN_STATUS_IF_FALSE(
      env, (length == NAPI_AUTO_LENGTH) || length <= INT_MAX, napi_invalid_arg);

  if (length == NAPI_AUTO_LENGTH) {
    length = std::strlen(str);
  }

  auto &runtime = env->runtime;
  const auto *utf8 = reinterpret_cast<const uint8_t *>(str);

  // If all bytes are ASCII, intern directly via ASCIIRef.
  if (hermes::isAllASCII(utf8, utf8 + length)) {
    hermes::vm::GCScope gcScope(runtime);
    auto symRes = runtime.getIdentifierTable().getSymbolHandle(
        runtime, hermes::vm::ASCIIRef(str, length));
    if (LLVM_UNLIKELY(symRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
      return napi_set_last_error(env, napi_generic_failure);
    }
    auto *strPrim = runtime.getStringPrimFromSymbolID(**symRes);
    *result = env->addToCurrentScope(
        hermes::vm::HermesValue::encodeStringValue(strPrim));
    return napi_clear_last_error(env);
  }

  // Non-ASCII UTF-8: convert to UTF-16 first, then intern.
  // UTF-16 length is at most the UTF-8 byte length.
  llvh::SmallVector<char16_t, 128> buf(length);
  const llvh::UTF8 *sourceStart = utf8;
  const llvh::UTF8 *sourceEnd = utf8 + length;
  llvh::UTF16 *targetStart = reinterpret_cast<llvh::UTF16 *>(buf.data());
  llvh::UTF16 *targetEnd = targetStart + length;
  llvh::ConversionResult cRes = llvh::ConvertUTF8toUTF16(
      &sourceStart,
      sourceEnd,
      &targetStart,
      targetEnd,
      llvh::lenientConversion);
  if (cRes != llvh::ConversionResult::conversionOK) {
    return napi_set_last_error(env, napi_generic_failure);
  }
  size_t utf16Len = targetStart - reinterpret_cast<llvh::UTF16 *>(buf.data());

  hermes::vm::GCScope gcScope(runtime);
  auto symRes = runtime.getIdentifierTable().getSymbolHandle(
      runtime, hermes::vm::UTF16Ref(buf.data(), utf16Len));
  if (LLVM_UNLIKELY(symRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return napi_set_last_error(env, napi_generic_failure);
  }
  auto *strPrim = runtime.getStringPrimFromSymbolID(**symRes);
  *result = env->addToCurrentScope(
      hermes::vm::HermesValue::encodeStringValue(strPrim));
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_create_property_key_utf16(
    napi_env env,
    const char16_t *str,
    size_t length,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  if (length > 0) {
    CHECK_ARG(env, str);
  }
  CHECK_ARG(env, result);
  RETURN_STATUS_IF_FALSE(
      env, (length == NAPI_AUTO_LENGTH) || length <= INT_MAX, napi_invalid_arg);

  if (length == NAPI_AUTO_LENGTH) {
    length = std::char_traits<char16_t>::length(str);
  }

  auto &runtime = env->runtime;
  hermes::vm::GCScope gcScope(runtime);
  auto symRes = runtime.getIdentifierTable().getSymbolHandle(
      runtime, hermes::vm::UTF16Ref(str, length));
  if (LLVM_UNLIKELY(symRes == hermes::vm::ExecutionStatus::EXCEPTION)) {
    return napi_set_last_error(env, napi_generic_failure);
  }
  auto *strPrim = runtime.getStringPrimFromSymbolID(**symRes);
  *result = env->addToCurrentScope(
      hermes::vm::HermesValue::encodeStringValue(strPrim));
  return napi_clear_last_error(env);
}

//===========================================================================
// String extraction
//===========================================================================

napi_status NAPI_CDECL napi_get_value_string_utf8(
    napi_env env,
    napi_value value,
    char *buf,
    size_t bufsize,
    size_t *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isString(), napi_string_expected);

  auto *str = hermes::vm::vmcast<hermes::vm::StringPrimitive>(*phv);

  if (!buf) {
    // Size query mode: return the required buffer size in bytes
    // (excluding null terminator). result must be non-null.
    CHECK_ARG(env, result);
    if (str->isASCII()) {
      // ASCII bytes are valid UTF-8 — length in bytes equals string
      // length.
      *result = str->getStringLength();
    } else {
      // Convert the whole string to compute the UTF-8 byte length.
      auto ref = str->getStringRef<char16_t>();
      std::string utf8;
      hermes::convertUTF16ToUTF8WithReplacements(utf8, ref);
      *result = utf8.size();
    }
  } else if (bufsize != 0) {
    // Copy mode: write up to bufsize-1 bytes and null-terminate.
    if (str->isASCII()) {
      auto ref = str->getStringRef<char>();
      size_t copyLen = std::min(ref.size(), bufsize - 1);
      std::memcpy(buf, ref.data(), copyLen);
      buf[copyLen] = '\0';
      if (result != nullptr) {
        *result = copyLen;
      }
    } else {
      auto ref = str->getStringRef<char16_t>();
      auto [numRead, numWritten] =
          hermes::convertUTF16ToUTF8BufferWithReplacements(
              llvh::MutableArrayRef<uint8_t>(
                  reinterpret_cast<uint8_t *>(buf), bufsize - 1),
              ref);
      buf[numWritten] = '\0';
      if (result != nullptr) {
        *result = numWritten;
      }
    }
  } else if (result != nullptr) {
    // bufsize is 0: nothing to copy, just report 0 written.
    *result = 0;
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_string_utf16(
    napi_env env,
    napi_value value,
    char16_t *buf,
    size_t bufsize,
    size_t *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isString(), napi_string_expected);

  auto *str = hermes::vm::vmcast<hermes::vm::StringPrimitive>(*phv);

  if (!buf) {
    // Size query mode: return the string length in UTF-16 code units
    // (excluding null terminator). result must be non-null.
    CHECK_ARG(env, result);
    *result = str->getStringLength();
  } else if (bufsize != 0) {
    // Copy mode: write up to bufsize-1 code units and
    // null-terminate.
    size_t strLen = str->getStringLength();
    size_t copyLen = std::min(strLen, bufsize - 1);

    if (str->isASCII()) {
      // Widen ASCII bytes to char16_t.
      auto ref = str->getStringRef<char>();
      for (size_t i = 0; i < copyLen; ++i) {
        buf[i] = static_cast<char16_t>(static_cast<unsigned char>(ref[i]));
      }
    } else {
      // Direct copy from UTF-16 storage.
      auto ref = str->getStringRef<char16_t>();
      std::memcpy(buf, ref.data(), copyLen * sizeof(char16_t));
    }
    buf[copyLen] = u'\0';

    if (result != nullptr) {
      *result = copyLen;
    }
  } else if (result != nullptr) {
    // bufsize is 0: nothing to copy, just report 0 written.
    *result = 0;
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_string_latin1(
    napi_env env,
    napi_value value,
    char *buf,
    size_t bufsize,
    size_t *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, value);

  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(value);
  RETURN_STATUS_IF_FALSE(env, phv->isString(), napi_string_expected);

  auto *str = hermes::vm::vmcast<hermes::vm::StringPrimitive>(*phv);

  if (!buf) {
    // Size query mode: return the string length in characters
    // (excluding null terminator). Each UTF-16 code unit maps to one
    // Latin-1 byte, so the length equals getStringLength().
    CHECK_ARG(env, result);
    *result = str->getStringLength();
  } else if (bufsize != 0) {
    // Copy mode: write up to bufsize-1 bytes and null-terminate.
    // Each UTF-16 code unit is truncated to its low byte, following
    // V8 behavior.
    size_t strLen = str->getStringLength();
    size_t copyLen = std::min(strLen, bufsize - 1);

    if (str->isASCII()) {
      // ASCII bytes are valid Latin-1 — direct copy.
      auto ref = str->getStringRef<char>();
      std::memcpy(buf, ref.data(), copyLen);
    } else {
      // Truncate each UTF-16 code unit to its low byte.
      auto ref = str->getStringRef<char16_t>();
      for (size_t i = 0; i < copyLen; ++i) {
        buf[i] = static_cast<char>(ref[i]);
      }
    }
    buf[copyLen] = '\0';

    if (result != nullptr) {
      *result = copyLen;
    }
  } else if (result != nullptr) {
    // bufsize is 0: nothing to copy, just report 0 written.
    *result = 0;
  }

  return napi_clear_last_error(env);
}
