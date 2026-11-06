/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSLIB_RUNTIMEJSONPARSE_H
#define HERMES_VM_JSLIB_RUNTIMEJSONPARSE_H

#include "hermes/VM/Runtime.h"

namespace hermes {

class UTF16Stream;

namespace vm {

/// Parse JSON string \p jsonString according to ES5.1 15.12.1 and 15.12.2.
CallResult<HermesValue> runtimeJSONParse(
    Runtime &runtime,
    Handle<StringPrimitive> jsonString,
    Handle<Callable> reviver);

/// Parse bounded JSON outside the JS heap with the raw ASCII lexer, without a
/// reviver. The input must contain only ASCII bytes and remain readable and
/// unchanged across allocations until this call returns. No input is retained.
CallResult<HermesValue> runtimeJSONParseRef(Runtime &runtime, ASCIIRef json);

/// Parse bounded JSON outside the JS heap with the raw UTF-16 lexer, without a
/// reviver. The input must remain readable and unchanged across allocations
/// until this call returns. Lone surrogates in strings are preserved. No input
/// is retained.
CallResult<HermesValue> runtimeJSONParseRef(Runtime &runtime, UTF16Ref json);

/// Alternative interface to runtimeJSONParse using the streaming UTF-8 lexer.
CallResult<HermesValue> runtimeJSONParseRef(Runtime &runtime, UTF16Stream &&s);

} // namespace vm
} // namespace hermes

#endif
