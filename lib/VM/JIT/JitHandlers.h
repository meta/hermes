/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "hermes/VM/HermesValue.h"
#include "hermes/VM/RuntimeModule.h"
#include "hermes/VM/sh_legacy_value.h"
#include "hermes/VM/static_h.h"

#include <vector>

typedef struct SHRuntime SHRuntime;
typedef struct SHRuntimeModule SHRuntimeModule;
typedef struct SHCodeBlock SHCodeBlock;
typedef struct SHJitVersionData SHJitVersionData;

namespace hermes::vm {

class Runtime;
class CodeBlock;
class JSObject;
class PinnedHermesValue;

/// Create a JSFunction closure. The properties of the function (name, etc.)
/// will be populated lazily.
SHLegacyValue _sh_ljs_create_bytecode_closure(
    SHRuntime *shr,
    const SHLegacyValue *env,
    SHRuntimeModule *shRuntimeModule,
    uint32_t functionID);

/// Create a generator from bytecode.
SHLegacyValue _interpreter_create_generator(
    SHRuntime *shr,
    SHLegacyValue *frame,
    const SHLegacyValue *env,
    SHRuntimeModule *shRuntimeModule,
    uint32_t functionID);

/// Get the string associated with the given RuntimeModule-specific string ID.
/// This may lazily allocate a SymbolID and the string itself.
/// This is used when executing with a bytecode CodeBlock.
SHLegacyValue _sh_ljs_get_bytecode_string(
    SHRuntime *shr,
    SHRuntimeModule *runtimeModule,
    uint32_t stringID);

/// Get the BigInt associated with the given RuntimeModule-specific BigInt ID.
SHLegacyValue _sh_ljs_get_bytecode_bigint(
    SHRuntime *shr,
    SHRuntimeModule *runtimeModule,
    uint32_t bigintID);

/// Wrapper around Interpreter::createObjectFromBuffer.
SHLegacyValue _interpreter_create_object_from_buffer(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset);
SHLegacyValue _interpreter_create_object_from_buffer_with_parent(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset);
SHLegacyValue _interpreter_create_object_with_parent(
    Runtime &runtime,
    CodeBlock *codeBlock,
    PinnedHermesValue *parent,
    uint32_t shapeTableIndex);

/// Wrapper around Interpreter::createObjectFromBuffer.
SHLegacyValue _interpreter_create_typed_object_from_buffer(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset);

/// Wrapper around Interpreter::createObjectFromBuffer for non-enumerable.
SHLegacyValue _interpreter_create_typed_non_enum_object_from_buffer(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset);

/// Wrapper around Interpreter::createArrayFromBuffer.
SHLegacyValue _interpreter_create_array_from_buffer(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    unsigned numElements,
    unsigned numLiterals,
    unsigned bufferIndex);

/// Counterpart of _sh_ljs_create_regexp that takes the precompiled regexp
/// bytecode from the RuntimeModule cache of \p codeBlock.
SHLegacyValue _interpreter_create_regexp(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    uint32_t pattern,
    uint32_t flags,
    uint32_t regexpID);

/// Wrapper around Interpreter::caseCreateClass
void _interpreter_create_class(SHRuntime *shr, SHLegacyValue *frameRegs);

/// Implementation of createFunctionEnvironment that takes the closure to get
/// the parentEnvironment from.
/// The native backend doesn't use createFunctionEnvironment.
SHLegacyValue _sh_ljs_create_function_environment(
    SHRuntime *shr,
    SHLegacyValue *frame,
    uint32_t size);

/// Print a message about entering or exiting the specified string, with
/// nesting tracking. Used for debugging.
void _sh_print_function_entry_exit(bool enter, const char *msg);

/// Concat two values that are known to be strings.
SHLegacyValue
_sh_ljs_string_add(SHRuntime *shr, SHLegacyValue *left, SHLegacyValue *right);

/// Create a new empty object for the given shape table index.
/// The properties will have to be populated by the caller afterwards.
/// \param tmp a FR pointer on the JS stack used to avoid having to allocate
///   Locals internally. May be overwritten.
JSObject *_jit_new_empty_object_for_buffer(
    Runtime &runtime,
    CodeBlock *codeBlock,
    uint32_t shapeTableIndex,
    PinnedHermesValue *tmp);

#ifdef HERMESVM_PROFILER_BB
/// Register BB execution for BB profiler based on the current callee CodeBlock.
void _interpreter_register_bb_execution(SHRuntime *shr, uint16_t pointIndex);
#endif

/// Run the direct eval on \p text and return the result.
HermesValue
_jit_direct_eval(Runtime &runtime, PinnedHermesValue *text, bool strictCaller);

/// Only valid to call from the longjmp catch handler in the JIT,
/// prior to running the Catch instruction itself.
/// At this point the C++ stack has been restored, but the Runtime register
/// stack has not.
///
/// \return a pointer to the instruction to branch to.
void *_jit_find_catch_target(
    SHRuntime *shr,
    SHCodeBlock *codeBlock,
    SHLegacyValue *frame,
    SHJmpBuf *jmpBuf,
    SHLocals *savedLocals,
    int32_t *addressTable);

/// Throw an exception that the current function was incorrectly called as a
/// constructor/non-constructor.
[[noreturn]] void _sh_throw_invalid_construct(SHRuntime *shr);
[[noreturn]] void _sh_throw_invalid_call(SHRuntime *shr);

/// Raise an exception that the target of a call is not callable. The outgoing
/// registers must already have been set up.
[[noreturn]] void _jit_throw_non_object_call(SHRuntime *shr);

/// Calls the builtin with index \p builtinMethodID. The new frame is at the top
/// of the stack. The arguments (excluding 'this') must be populated.
SHLegacyValue _jit_call_builtin(
    SHRuntime *shr,
    SHLegacyValue *frame,
    uint32_t argCount,
    uint32_t builtinMethodID);

/// Slow path of PutById, and the recording site for the PutById tier's
/// declines.
/// \param versionData the version record of the CALLING body; the
///   CodeBlock is reached through it.
void _jit_put_by_id(
    SHRuntime *shr,
    SHJitVersionData *versionData,
    SHLegacyValue *base,
    SHLegacyValue *value,
    uint8_t cacheIdx,
    SHSymbolID symID,
    bool strictMode,
    bool tryProp);

/// Slow path of PutByVal (loose), and the recording site for ByVal
/// tier declines: records the observed target shape into
/// \p versionData's entry for \p siteId, counts the decline (possibly
/// triggering a recompile), then forwards to the plain SH helper.
/// \param siteId the PutByVal instruction's bytecode offset.
void _jit_put_by_val_loose(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    SHJitVersionData *versionData,
    uint32_t siteId);
/// Strict-mode variant of _jit_put_by_val_loose.
void _jit_put_by_val_strict(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    SHJitVersionData *versionData,
    uint32_t siteId);

/// Slow path of GetByVal, and the recording site for the ByVal load
/// tier's declines: records the observed source shape into
/// \p versionData's entry for \p siteId, counts the decline (possibly
/// triggering a recompile), then forwards to the plain SH helper and
/// returns its value.
/// \param siteId the GetByVal instruction's bytecode offset.
SHLegacyValue _jit_get_by_val(
    SHRuntime *shr,
    SHLegacyValue *source,
    SHLegacyValue *key,
    SHJitVersionData *versionData,
    uint32_t siteId);

/// Slow path of GetByIndex, and the recording site for the ByIndex load
/// tier's declines: records the observed source shape into
/// \p versionData's entry for \p siteId, counts the decline (possibly
/// triggering a recompile), then forwards to the plain SH helper and
/// returns its value.
/// \param siteId the GetByIndex instruction's bytecode offset.
SHLegacyValue _jit_get_by_index(
    SHRuntime *shr,
    SHLegacyValue *source,
    uint32_t key,
    SHJitVersionData *versionData,
    uint32_t siteId);

/// Assumes that table  at index \p tableIndex is an initialized string switch
/// runtime table. Pass the runtimeModule+index in order to avoid relying on
/// potentially reallocating storage in RuntimeModule.
///
/// If \p switchValue is found as a case in that table, returns that case's
/// index, which is dense in [0, table size). Otherwise -- \p switchValue is
/// not a string, or is a string no case matches -- returns -1, meaning the
/// default case.
///
/// The result deliberately identifies the case rather than a code address:
/// the table is shared by every compiled version of the function, while the
/// address of a case's code is specific to one body. The caller turns the
/// index into an address using the jump table embedded in its own body.
int64_t _jit_string_switch_imm_table_lookup(
    RuntimeModule *runtimeModule,
    uint32_t tableIndex,
    SHLegacyValue *switchValue);

/// A property of an FR's value that a fast path relies on. These are the
/// predicates the emitters actually exploit, which is narrower and more
/// useful than the declared FRType.
enum class TypePred : uint8_t {
  /// Unsigned-below (HVTag_First << kHV_NumDataBits).
  IsNumber,
  /// ETag == HVETag_Bool.
  IsBool,
  /// Tag unsigned-below the pointer range. This is the GC-safety predicate.
  NotPointer,
  /// NotPointer && !IsNumber: the raw bits are the value's identity under
  /// strict equality, so `===` is a bit compare. Doubles are excluded
  /// because NaN is not equal to itself while its bits are, and -0 and +0
  /// are equal while their bits are not; pointers because strings compare
  /// by content rather than address.
  BitComparable,
  /// Tag == HVTag_Object.
  IsObject,
};

/// \return a human-readable name for \p pred, for diagnostics.
const char *typePredName(TypePred pred);

/// One emitted type check, recorded so the failure handler can name it.
struct TypeAssertSite {
  CodeBlock *codeBlock;
  uint32_t bytecodeOfs;
  uint16_t frIndex;
  TypePred pred;
};

/// Report a failed JIT type assertion and abort. Called only from JIT'ed
/// code, and never returns, so it needs no register or frame preservation.
/// \p sites is the failing function's site table and is never null; \p
/// siteIdx indexes it.
[[noreturn]] void _jit_type_assert_failed(
    uint32_t siteIdx,
    const std::vector<TypeAssertSite> *sites);

} // namespace hermes::vm
