/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_STATIC_H_H
#define HERMES_STATIC_H_H

#include "hermes/FrontEndDefs/TypeOfIsTypes.h"
#include "hermes/Support/sh_tryfast_fp_cvt.h"
#include "hermes/VM/sh_legacy_value.h"
#include "hermes/VM/sh_mirror.h"
#include "hermes/VM/sh_runtime.h"
#include "hermes/VM/sh_small_hermes_value.h"
#include "hermes/VM/sh_stack_frame.h"
#include "hermes/VMLayouts/sh_stack_frame_layout.h"

#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SHUnitExt SHUnitExt;
typedef struct SHUnit SHUnit;

/// Encodes the set of keys to be used to construct an object literal.
typedef struct SHShapeTableEntry {
  /// The number of bytes into the key buffer this shape begins.
  uint32_t key_buffer_offset;
  /// The number of properties in this shape.
  uint32_t num_props;
} SHShapeTableEntry;

/// Locates the bytecode of one regexp literal in SHUnit::regexp_storage.
typedef struct SHRegExpTableEntry {
  /// Offset of the bytecode in regexp_storage, in bytes.
  uint32_t offset;
  /// Length of the bytecode, in bytes.
  uint32_t length;
} SHRegExpTableEntry;

/// This represents a source JS location. This is only valid in a particular
/// SHUnit, since the filename is stored as an index into the SHUnit's global
/// string table.
typedef struct SHSrcLoc {
  /// Index into the global string table to get the filename for this location.
  uint32_t filename_idx;
  /// Line in the source file. 1-based.
  uint32_t line;
  /// Column in the source file. 1-based.
  uint32_t column;
} SHSrcLoc;

/// Encodes some basic information about a native function.
typedef struct SHNativeFuncInfo {
  /// The index in the global string table to get the function name.
  uint32_t name_index;
  /// The number of arguments this function takes.
  uint32_t arg_count;
  /// Which kind of function, constructed from enum FuncKind.
  uint8_t kind : 2;
  /// Which kinds of calls are prohibited, constructed from enum ProhibitInvoke.
  uint8_t prohibit_invoke : 2;
} SHNativeFuncInfo;

/// Type of a function that allocates and returns a new SHUnit ready to be used
/// with a runtime.
typedef SHUnit *(*SHUnitCreator)(void);

/// SHUnit describes a compilation unit.
///
/// <h2>Restrictions</h2>
/// A SHUnit can only be associated with one SHRuntime at a time. Once
/// associated with a runtime, a unit cannot be unloaded and persists until
/// the runtime is destroyed. At that point the unit can be reinitialized and
/// used again by a different runtime.
///
/// <br>One implication is that all units are treated as "persistent" modules,
/// and strings don't need to be copied to RAM until needed.
typedef struct SHUnit {
  /// Pointer to the static index variable that uniquely identifies the unit
  /// (rather than this particular instance). The index is zero until the first
  /// instance is initialized.
  uint32_t *index;
  /// The ScriptID allocated from the Runtime when the unit is in use.
  uint32_t script_id;

  /// Number of symbols (strings).
  uint32_t num_symbols;
  /// Sizes of the property caches.
  uint32_t num_read_prop_cache_entries;
  uint32_t num_write_prop_cache_entries;
  uint32_t num_private_name_cache_entries;

  /// Pool of ASCII strings.
  const char *ascii_pool;
  /// Pool of 16-bit strings.
  const char16_t *u16_pool;

  /// Triples of (offset, length, hash).
  /// The high bit of offset indicates whether the string is 7-bit ASCII in
  /// ascii_pool (value 0), or 16-bit (value 1) in u16_pool. The offset is in
  /// the corresponding units (char or char16_t).
  ///
  /// The strings are not 0-terminated (and naturally the length doesn't include
  /// a trailing 0). However, for easier debugging the pools *can* contain zero
  /// terminated strings, so a pointer into the pool can be interpreted as a
  /// regular C string. This decrease the chances for string packing.
  const uint32_t *strings;
  /// Symbols populated by the init code from strings above. This pointer is
  /// initialized by the owner of the struct and typically points into BSS.
  SHSymbolID *symbols;
  /// Write Property cache. Points to an array with
  /// `num_write_prop_cache_entries` elements.  Must be zeroed
  /// initially.
  SHWritePropertyCacheEntry *write_prop_cache;
  /// Read Property cache. Points to an array with
  /// `num_read_prop_cache_entries` elements.  Must be zeroed
  /// initially.
  SHReadPropertyCacheEntry *read_prop_cache;
  /// Private name cache. Points to an array with
  /// `num_private_name_cache_entries` elements.  Must be zeroed
  /// initially.
  SHPrivateNameCacheEntry *private_name_cache;

  /// Object key buffer.
  const unsigned char *obj_key_buffer;
  /// Size of object key buffer.
  uint32_t obj_key_buffer_size;
  /// Object/Array value buffer.
  const unsigned char *literal_val_buffer;
  /// Size of value buffer.
  uint32_t literal_val_buffer_size;

  /// Object shape table.
  const SHShapeTableEntry *obj_shape_table;
  /// Size of object shape table.
  uint32_t obj_shape_table_count;
  /// Cached object literal hidden classes. Points to an array of
  /// `obj_shape_table_count` WeakRoots, which point to the
  /// cached hidden class for that entry.
  /// NOTE: These should always be treated as WeakRoots, which means a read
  /// barrier is needed to safely read out the value.
  SHCompressedPointer *object_literal_class_cache;

  /// Concatenated bytecode of the unit's regexp literals, compiled ahead of
  /// time.
  const unsigned char *regexp_storage;
  /// One entry per regexp literal, indexed by the id passed to
  /// _sh_ljs_create_regexp.
  const SHRegExpTableEntry *regexp_table;
  /// Number of entries in regexp_table.
  uint32_t num_regexps;

  /// The module exports cache: a map from non-negative integer module indexes
  /// to the export of the module.  An empty value indicates that the module has
  /// not yet been initialized.
  SHArrayStorage *moduleExports;

  /// List of source locations.
  const SHSrcLoc *source_locations;
  /// Size of source locations array.
  uint32_t source_locations_size;

  /// Unit main function.
  SHLegacyValue (*unit_main)(SHRuntime *shr);
  /// Unit main function information.
  const SHNativeFuncInfo *unit_main_info;
  /// Unit name.
  const char *unit_name;

  /// Data managed by the runtime. Field populated by the runtime.
  SHUnitExt *runtime_ext;
} SHUnit;

typedef struct SHLocals {
  struct SHLocals *prev;
  unsigned count;
  /// The SHUnit associated with this locals.
  SHUnit *unit;
  /// The current index into the SHUnit's source location table.
  uint32_t src_location_idx;
  SHLegacyValue locals[0];
} SHLocals;

/// Utility to concatenate a prefix with HERMESVM_MODEL.
#define _HERMESVM_JOIN_TOKENS(x, y) _HERMESVM_JOIN_HELPER(x, y)
/// This helper is needed due to how the preprocessor works.
#define _HERMESVM_JOIN_HELPER(x, y) x##y

/// The name of the model-specific exported symbol
#define _SH_MODEL _HERMESVM_JOIN_TOKENS(_sh_model, HERMESVM_MODEL)

/// A dummy export to ensure correct library is linked.
SHERMES_EXPORT void _SH_MODEL(void);

#ifndef HERMES_IS_MOBILE_BUILD
/// Parse the command line flags, if provided. If not provided, the VM command
/// line options receive their default values specified in cli::RuntimeFlags.
/// Returns a new runtime on success, terminate the process on error. Errors
/// are printed to stderr.
///
/// \param argc number of command line arguments. If 0, no parsing is performed.
/// \param argv the command line arguments, where argv[0] is the application
///     path. Ignored if \c argc is 0.
/// \return the created runtime.
SHERMES_EXPORT SHRuntime *_sh_init(int argc, char **argv);

/// Parse the command line flags, if provided. If not provided, the VM command
/// line options receive their default values specified in cli::RuntimeFlags.
/// Returns a new runtime on success, or nullptr on error.
///
/// \param argc number of command line arguments. If 0, no parsing is performed.
/// \param argv the command line arguments, where argv[0] is the application
///     path. Ignored if \c argc is 0.
/// \param errorMessage if non-null, it will be cleared on success, or
///     initialized with a heap-allocated error message string, which needs to
///     be freed with \c free().
/// \return the created runtime or nullptr
SHERMES_EXPORT SHRuntime *
_sh_init_with_error(int argc, char **argv, char **errorMessage);
#endif // HERMES_IS_MOBILE_BUILD

/// Destroy a runtime instance created by \c _sh_init();
SHERMES_EXPORT void _sh_done(SHRuntime *shr);

/// Check on the native stack and throw a stack overflow error if it overflows.
/// When compiled without HERMES_CHECK_NATIVE_STACK, does nothing.
SHERMES_EXPORT void _sh_check_native_stack_overflow(SHRuntime *shr);

/// Same as _sh_check_native_stack_overflow(), with the in-bounds check inline.
/// \p frameLow is an address near the bottom of the caller's frame, such as
/// its locals. Using it instead of the frame address avoids forcing a frame
/// pointer and accounts for the caller's locals.
static inline void _sh_check_native_stack_overflow_inline(
    SHRuntime *shr,
    void *frameLow) {
  if (SH_LIKELY(
          !sh_overflow_guard_out_of_bounds(&shr->overflowGuard, frameLow)))
    return;
  _sh_check_native_stack_overflow(shr);
}

/// Register, initialize and execute a main function of the unit returned by the
/// given creator function. The unit is freed when the runtime is destroyed.
/// Execution of the unit initialization code might throw a JS exception.
SHERMES_EXPORT SHLegacyValue
_sh_unit_init(SHRuntime *shr, SHUnitCreator unitCreator);

/// Execute \c _sh_unit_init and catch JS exceptions. \p resultOrExc is
/// initialized to either the returned value or the thrown exception.
///
/// \return false if an exception was thrown.
SHERMES_EXPORT bool _sh_unit_init_guarded(
    SHRuntime *shr,
    SHUnitCreator unitCreator,
    SHLegacyValue *resultOrExc);

/// Initialize all units passed as arguments in order. If a unit throws a
/// JS exception during initialization, print the exception and stop.
///
/// \param count number of units
/// \return false if a unit threw an exception.
SHERMES_EXPORT bool _sh_initialize_units(SHRuntime *shr, uint32_t count, ...);

/// ES6.0 12.2.9.3 Runtime Semantics: GetTemplateObject ( templateLiteral )
///
/// Given a template literal, return a template object that looks like this:
/// [cookedString0, cookedString1, ..., raw: [rawString0, rawString1]].
/// This object is frozen, as well as the 'raw' object nested inside.
/// We only pass the parts from the template literal that are needed to
/// construct this object. That is, the raw strings and cooked strings.
/// \param templateObjID is the unique id associated with the template object.
/// \param dup when true, cooked strings are the same as raw strings.
/// \param argCount the number of varargs.
/// \param ... (const SHLegacyValue *)
///   First raw strings are passed.
///   Then cooked strings are optionally passed if \p dup is true.
SHERMES_EXPORT SHLegacyValue _sh_get_template_object(
    SHRuntime *shr,
    SHUnit *unit,
    uint32_t templateObjID,
    bool dup,
    uint32_t argCount,
    ...);

/// Given \p templateObjectID, retrieve the cached template object.
/// if it doesn't exist, return a nullptr.
SHERMES_EXPORT void *_sh_find_cached_template_object(
    const SHUnit *unit,
    uint32_t templateObjID);

/// Cache a template object in the template map using a template object ID as
/// key.
/// \p templateObjID is the template object ID, and it should not already
/// exist in the map.
/// \p templateObj is the template object that we are caching.
SHERMES_EXPORT void _sh_cache_template_object(
    SHUnit *unit,
    uint32_t templateObjID,
    SHLegacyValue templateObj);

/// Throw a register stack overflow exception.
SHERMES_EXPORT void _sh_throw_register_stack_overflow(SHRuntime *shr);

/// Add the locals to the gc list, allocate register stack, return a pointer to
/// the frame.
static inline SHLegacyValue *
_sh_enter(SHRuntime *shr, SHLocals *locals, uint32_t stackSize) {
  SHLegacyValue *frame = shr->stackPointer;

  if (SH_UNLIKELY(!_sh_check_and_alloc_stack(shr, stackSize))) {
    _sh_throw_register_stack_overflow(shr);
    SH_BUILTIN_UNREACHABLE;
  }

  shr->currentFrame = frame;
  locals->prev = shr->shLocals;
  shr->shLocals = locals;
  return frame;
}

/// Pop the locals from the root stack and restore the stack and frame pointers.
/// \param shr the SHRuntime
/// \param locals the locals to pop from the root stack
/// \param frame the value that should be restored to the stack pointer
static inline void
_sh_leave(SHRuntime *shr, SHLocals *locals, SHLegacyValue *frame) {
  assert(shr->shLocals == locals && "Only the current locals can be popped");
  shr->shLocals = locals->prev;
  shr->stackPointer = frame;
  shr->currentFrame = _sh_stackframe_get_previous_frame_pointer(frame);
}

/// Add a locals struct to the root stack, allocate the requested number of
/// registers and return the previous register stack pointer.
static inline SHLegacyValue *
_sh_push_locals(SHRuntime *shr, SHLocals *locals, uint32_t stackSize) {
  SHLegacyValue *savedSP = shr->stackPointer;

  if (stackSize && SH_UNLIKELY(!_sh_check_and_alloc_stack(shr, stackSize))) {
    _sh_throw_register_stack_overflow(shr);
  }

  locals->prev = shr->shLocals;
  shr->shLocals = locals;
  return savedSP;
}

/// Pop the locals (which must be current), restore the register stack.
static inline void
_sh_pop_locals(SHRuntime *shr, SHLocals *locals, SHLegacyValue *savedSP) {
  assert(shr->shLocals == locals && "Only the current locals can be popped");
  shr->shLocals = locals->prev;
  assert(
      savedSP <= shr->stackPointer &&
      "attempting to pop the stack to a higher level");
  shr->stackPointer = savedSP;
}

/// Index 0 loads "this", 1 the first param, etc.
static inline SHLegacyValue _sh_ljs_param(
    SHLegacyValue *frame,
    uint32_t index) {
  uint32_t argCount = _sh_stackframe_get_argcount(frame);
  if (SH_LIKELY(index <= argCount)) {
    // Subtract 1 (just like the interpreter) because -1 is used for "this" in
    // _sh_stackframe_get_arg_ptr.
    return *_sh_stackframe_get_arg_ptr(frame, (int32_t)index - 1);
  } else {
    return _sh_ljs_undefined();
  }
}

/// Coerce a value assumed to contain 'this' to an object using non-strict
/// mode rules. Primitives are boxed, \c null or \c undefed produce the global
/// object.
SHERMES_EXPORT SHLegacyValue
_sh_ljs_coerce_this_ns(SHRuntime *shr, SHLegacyValue value);

/// `arguments` operations all work with a lazy "register" that contains either
/// undefined or a reified array. The first call to _sh_reify_arguments_* will
/// populate the reified array. This is an optimization to allow arguments[i] to
/// just load an argument instead of doing a full array allocation and property
/// lookup.

/// Get a property of the 'arguments' array by value.
SHERMES_EXPORT SHLegacyValue _sh_ljs_get_arguments_prop_by_val_loose(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *idx,
    SHLegacyValue *lazyReg);
SHERMES_EXPORT SHLegacyValue _sh_ljs_get_arguments_prop_by_val_strict(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *idx,
    SHLegacyValue *lazyReg);

/// Get the length of the 'arguments' array.
SHERMES_EXPORT SHLegacyValue _sh_ljs_get_arguments_length(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *lazyReg);

/// Ensure that the reified array has been created, updating \p lazyReg in place
/// if it has not.
SHERMES_EXPORT void _sh_ljs_reify_arguments_loose(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *lazyReg);
SHERMES_EXPORT void _sh_ljs_reify_arguments_strict(
    SHRuntime *shr,
    SHLegacyValue *frame,
    SHLegacyValue *lazyReg);

/// Allocate an empty, uninitialized object (immediately before a constructor).
/// \param classCacheEntry caches the HiddenClass of the new object, may be
///   null.
SHERMES_EXPORT SHLegacyValue _sh_ljs_create_this(
    SHRuntime *shr,
    SHLegacyValue *callee,
    SHLegacyValue *newTarget,
    SHReadPropertyCacheEntry *propCacheEntry,
    SHCompressedPointer *classCacheEntry);

#if defined(HERMES_SANDBOX_WASM)
// The sandbox (API/hermes_sandbox) builds the VM as a standalone wasm module
// that is translated to C and runs without a JS host. By default (which uses
// -sSUPPORT_LONGJMP=emscripten), Emscripten implements setjmp/longjmp with JS
// imports, which the sandbox cannot provide. Only code compiled by shermes and
// the JIT use _sh_try and _sh_throw, and neither can run in the sandbox, so
// setjmp always returns 0 and longjmp traps.
// Note: -sSUPPORT_LONGJMP=wasm turns on WASM exception handling. We may use
// this if we want to support native backend or JIT in sandbox in the future.
#define _sh_setjmp(buf) ((void)(buf), 0)
#define _sh_longjmp(buf, val) ((void)(buf), (void)(val), __builtin_trap())
#elif !defined(_WINDOWS)
// Use _setjmp and _longjmp outside Windows, to avoid saving and restoring the
// signal mask, which is costly and may interfere with signal mask manipulation
// by non-Hermes code.
#define _sh_setjmp _setjmp
#define _sh_longjmp _longjmp
#else
// Windows does not provide _setjmp and _longjmp, so use setjmp and longjmp
// instead.
#define _sh_setjmp setjmp
#define _sh_longjmp longjmp
#endif

#define _sh_try(shr, jbuf) (_sh_push_try(shr, jbuf), _sh_setjmp((jbuf)->buf))

/// Push the given \p buf onto the exception handler stack.
static inline void _sh_push_try(SHRuntime *shr, SHJmpBuf *buf) {
  buf->prev = shr->shCurJmpBuf;
  shr->shCurJmpBuf = buf;
}

/// Pop the current top of the exception handler stack, which is known to be
/// \p buf.
static inline void _sh_end_try(SHRuntime *shr, SHJmpBuf *buf) {
  assert(shr->shCurJmpBuf == buf && "must pop top buffer");
  shr->shCurJmpBuf = buf->prev;
}

/// \param frame the value that should be set to the current frame
///     (Runtime::currentFrame_).
/// \param stackSize <tt>frame + stackSize</tt> will be the new value of the
///     register stack pointer.
SHERMES_EXPORT SHLegacyValue _sh_catch(
    SHRuntime *shr,
    SHLocals *locals,
    SHLegacyValue *frame,
    uint32_t stackSize);
/// Like _sh_catch, but doesn't update shCurJmpBuf because that's handled at the
/// function-level by running _sh_end_try before returning or throwing out of
/// the function, and does not clear/return the thrown value.
///
/// \param frame the value that should be set to the current frame
///     (Runtime::currentFrame_).
/// \param stackSize <tt>frame + stackSize</tt> will be the new value of the
///     register stack pointer.
SHERMES_EXPORT void _sh_catch_no_pop(
    SHRuntime *shr,
    SHLocals *locals,
    SHLegacyValue *frame,
    uint32_t stackSize);
/// Clear the thrown value and return it.
SHERMES_EXPORT SHLegacyValue _sh_get_clear_thrown_value(SHRuntime *shr);
SHERMES_EXPORT SH_ATTRIBUTE_NORETURN void _sh_throw_current(SHRuntime *shr);
SHERMES_EXPORT SH_ATTRIBUTE_NORETURN void _sh_throw(
    SHRuntime *shr,
    SHLegacyValue value);

/// Throw a TypeError with the given message.
/// \param message will be converted to a string and used as the error message.
SHERMES_EXPORT SH_ATTRIBUTE_NORETURN void _sh_throw_type_error(
    SHRuntime *shr,
    SHLegacyValue *message);
/// Throw a TypeError with the given ASCII message.
SHERMES_EXPORT SH_ATTRIBUTE_NORETURN void _sh_throw_type_error_ascii(
    SHRuntime *shr,
    const char *message);
SHERMES_EXPORT SH_ATTRIBUTE_NORETURN void _sh_throw_reference_error_ascii(
    SHRuntime *shr,
    const char *message);

/// Throw a ReferenceError for accessing uninitialized variable.
SHERMES_EXPORT SH_ATTRIBUTE_NORETURN void _sh_throw_empty(SHRuntime *shr);

/// Throws a ReferenceError for double-initialization of `this`.
static inline SH_ATTRIBUTE_NORETURN void _sh_throw_this_already_initialized(
    SHRuntime *shr) {
  _sh_throw_reference_error_ascii(shr, "Cannot call super constructor twice");
}

/// Throw a ReferenceError if the given derived class constructor's checked this
/// is already initialized.
static inline void _sh_ljs_throw_if_this_initialized(
    SHRuntime *shr,
    SHLegacyValue checkedThis) {
  if (!_sh_ljs_is_empty(checkedThis))
    _sh_throw_this_already_initialized(shr);
}

/// Performs a function call. The new frame is at the top of the stack.
/// Arguments, this, and callee must be populated.
SHERMES_EXPORT SHLegacyValue
_sh_ljs_call(SHRuntime *shr, SHLegacyValue *frame, uint32_t argCount);

/// Performs a function call. The new frame is at the top of the stack.
/// The arguments (excluding 'this') must be populated.
SHERMES_EXPORT SHLegacyValue _sh_ljs_call_builtin(
    SHRuntime *shr,
    SHLegacyValue *frame,
    uint32_t argCount,
    uint32_t builtinMethodID);

SHERMES_EXPORT SHLegacyValue
_sh_ljs_get_builtin_closure(SHRuntime *shr, uint32_t builtinMethodID);

/// Semantically equivalent to calling \p requireFunc with the argument
/// \p modIndex.  Assumes that \p requireFunc is the metro require function,
/// and may cache the result for a given \p modIndex.
/// The \p cacheData argument is currently unused, but will be used in a
/// later diff to provide caching.
SHERMES_EXPORT SHLegacyValue _sh_ljs_callRequire(
    SHRuntime *shr,
    SHArrayStorage **exportCache,
    SHLegacyValue *requireFunc,
    uint32_t modIndex);

/// Create a new environment with the specified \p size and \p parentEnv (which
/// may be null if this is a top level environment).
SHERMES_EXPORT SHLegacyValue _sh_ljs_create_environment(
    SHRuntime *shr,
    const SHLegacyValue *parentEnv,
    uint32_t size);

/// Get the environment from the given \p closure. Note that the result should
/// be handled with care, since it may be nullptr for the top level function,
/// which cannot be stored in SHLocals.
static inline SHLegacyValue _sh_ljs_get_env_from_closure(
    SHRuntime *shr,
    SHLegacyValue closure) {
  SHCompressedPointer scp =
      ((SHCallable *)_sh_ljs_get_pointer(closure))->environment;
  return _sh_ljs_object(_sh_cp_decode(shr, scp));
}

/// Get the enclosing LJS environment, \p level levels up from \p startEnv,
/// where 0 represents the environment of the current function.
/// TODO: implement a "raw" version of this function returning a raw pointer,
///       as well as raw version of the other env-related functions, as soon
///       as we have the ability to mark unencoded pointers.
SHERMES_EXPORT SHLegacyValue
_sh_ljs_get_env(SHRuntime *shr, SHLegacyValue startEnv, uint32_t level);

/// Load the value at slot \p index from the given Environment \p env.
static inline SHLegacyValue _sh_ljs_load_from_env(
    SHLegacyValue env,
    uint32_t index) {
  return ((SHEnvironment *)_sh_ljs_get_pointer(env))->slots[index];
}

SHERMES_EXPORT void _sh_ljs_store_to_env(
    SHRuntime *shr,
    SHLegacyValue env,
    SHLegacyValue val,
    uint32_t index);

/// Same as _sh_ljs_store_to_env(), but \p value is known to not be a pointer
/// (though the old value being overwritten still might be).
SHERMES_EXPORT void _sh_ljs_store_np_to_env(
    SHRuntime *shr,
    SHLegacyValue env,
    SHLegacyValue val,
    uint32_t index);

/// Create a closure. The properties of the function (name, etc.) will be
/// populated lazily.
/// \param env NULL if there is no environment.
/// \param funcInfo Must not be NULL.
SHERMES_EXPORT SHLegacyValue _sh_ljs_create_closure(
    SHRuntime *shr,
    const SHLegacyValue *env,
    SHLegacyValue (*func)(SHRuntime *),
    const SHNativeFuncInfo *funcInfo,
    const SHUnit *unit);

/// Create a generator object.
/// \param env NULL if there is no environment.
/// \param funcInfo Should not be null.
SHERMES_EXPORT SHLegacyValue _sh_ljs_create_generator_object(
    SHRuntime *shr,
    const SHLegacyValue *env,
    SHLegacyValue (*func)(SHRuntime *),
    const SHNativeFuncInfo *funcInfo,
    const SHUnit *unit);

/// Create a class.
/// \param env Should not be null.
/// \param funcInfo Should not be null.
SHERMES_EXPORT SHLegacyValue _sh_ljs_create_class(
    SHRuntime *shr,
    const SHLegacyValue *env,
    SHLegacyValue (*func)(SHRuntime *),
    const SHNativeFuncInfo *funcInfo,
    const SHUnit *unit,
    SHLegacyValue *homeObjectOut,
    SHLegacyValue *superClass);

static inline SHLegacyValue _sh_ljs_get_global_object(SHRuntime *shr) {
  return shr->global_;
}

SHERMES_EXPORT void _sh_ljs_declare_global_var(SHRuntime *shr, SHSymbolID name);

SHERMES_EXPORT void _sh_ljs_put_by_id_loose_rjs(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *target,
    SHSymbolID symID,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *propCacheEntry);
SHERMES_EXPORT void _sh_ljs_put_by_id_strict_rjs(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *target,
    SHSymbolID symID,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *propCacheEntry);
SHERMES_EXPORT void _sh_ljs_try_put_by_id_loose_rjs(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *target,
    SHSymbolID symID,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *propCacheEntry);
SHERMES_EXPORT void _sh_ljs_try_put_by_id_strict_rjs(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *target,
    SHSymbolID symID,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *propCacheEntry);
SHERMES_EXPORT void _sh_ljs_put_by_val_loose_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value);
SHERMES_EXPORT void _sh_ljs_put_by_val_strict_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value);
SHERMES_EXPORT void _sh_ljs_put_by_val_with_receiver_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value,
    SHLegacyValue *receiver,
    bool isStrict);

/// Put a property given a valid array index \p key.
SHERMES_EXPORT void _sh_ljs_put_by_index_loose_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t key,
    SHLegacyValue *value);
SHERMES_EXPORT void _sh_ljs_put_by_index_strict_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t key,
    SHLegacyValue *value);

/// Optimized put-by-val when the key is known to be a number at compile time.
/// The key is passed by value (not pointer) because numbers are immediate
/// values that do not require GC rooting. Tries sh_tryfast_f64_to_u32 inline,
/// then calls _sh_ljs_put_by_index on success, or falls back to the generic
/// put-by-val path.
static inline void _sh_ljs_put_by_val_numeric_loose_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue key,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_double(key) && "key must be a number");
  uint32_t index;
  // Reject 0xFFFFFFFF: valid uint32 but not a valid array index.
  if (SH_LIKELY(
          sh_tryfast_f64_to_u32(key.f64, index) && index != 0xFFFFFFFFu)) {
    _sh_ljs_put_by_index_loose_rjs(shr, target, index, value);
    return;
  }
  _sh_ljs_put_by_val_loose_rjs(shr, target, &key, value);
}
static inline void _sh_ljs_put_by_val_numeric_strict_rjs(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue key,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_double(key) && "key must be a number");
  uint32_t index;
  // Reject 0xFFFFFFFF: valid uint32 but not a valid array index.
  if (SH_LIKELY(
          sh_tryfast_f64_to_u32(key.f64, index) && index != 0xFFFFFFFFu)) {
    _sh_ljs_put_by_index_strict_rjs(shr, target, index, value);
    return;
  }
  _sh_ljs_put_by_val_strict_rjs(shr, target, &key, value);
}

/// Load a property from direct storage.
SHERMES_EXPORT SHLegacyValue
_sh_prload_direct(SHRuntime *shr, SHLegacyValue source, uint32_t propIndex);

/// Load a property from indirect storage. Note that propIndex is relative to
/// the indirect storage.
SHERMES_EXPORT SHLegacyValue
_sh_prload_indirect(SHRuntime *shr, SHLegacyValue source, uint32_t propIndex);

static inline SHLegacyValue
_sh_prload(SHRuntime *shr, SHLegacyValue source, uint32_t propIndex) {
  if (propIndex < HERMESVM_DIRECT_PROPERTY_SLOTS) {
#ifndef HERMESVM_BOXED_DOUBLES
    return ((SHJSObjectAndDirectProps *)_sh_ljs_get_pointer(source))
        ->directProps[propIndex];
#else
    return _sh_prload_direct(shr, source, propIndex);
#endif
  } else {
    return _sh_prload_indirect(
        shr, source, propIndex - HERMESVM_DIRECT_PROPERTY_SLOTS);
  }
}

static inline SHLegacyValue
_sh_prload_inline(SHRuntime *shr, SHLegacyValue source, uint32_t propIndex) {
  if (propIndex < HERMESVM_DIRECT_PROPERTY_SLOTS) {
    SHGCSmallHermesValue shv =
        ((SHJSObjectAndDirectProps *)_sh_ljs_get_pointer(source))
            ->directProps[propIndex];
    return _sh_shv_unbox_inline(shr, shv);
  } else {
    // Inline indirect property access
    SHCompressedPointer propStoragePtr = {
        .raw = ((SHJSObject *)_sh_ljs_get_pointer(source))->propStorage};
    SHArrayStorageSmall *propStorage =
        (SHArrayStorageSmall *)_sh_cp_decode_non_null(shr, propStoragePtr);
    SHGCSmallHermesValue shv =
        propStorage->storage[propIndex - HERMESVM_DIRECT_PROPERTY_SLOTS];
    return _sh_shv_unbox_inline(shr, shv);
  }
}

SHERMES_EXPORT SHLegacyValue _sh_ljs_try_get_by_id_rjs(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry);
SHERMES_EXPORT SHLegacyValue _sh_ljs_get_by_id_rjs(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry);

/// Inline fast path for property access by ID with cache checking.
/// If cache hits, directly loads the property. Otherwise falls back to the
/// out-of-line implementation.
/// \pre propCacheEntry is not null.
static inline SHLegacyValue _sh_ljs_get_by_id_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry) {
  assert(propCacheEntry && "propCacheEntry must not be null");
  if (SH_LIKELY(_sh_ljs_is_object(*source))) {
    // Fast path: check cache hit
    SHJSObject *sourceCell = (SHJSObject *)_sh_ljs_get_pointer(*source);

    // Check if the object is a JSObject
    SHCompressedPointer clazzPtr = {.raw = sourceCell->clazz};

    // Compare cached class with object's current class
    if (SH_LIKELY(propCacheEntry->clazz == clazzPtr.raw)) {
      // Cache hit! Extract slot with proper masking (like
      // ReadPropertyCacheEntry::getSlot())
      uint16_t slot = propCacheEntry->_slot16 & 0xffff;

      // Load property directly using the cached slot
      return _sh_prload_inline(shr, *source, slot);
    }
  }

  // Cache miss - fall back to out-of-line implementation
  return _sh_ljs_get_by_id_rjs(shr, source, symID, propCacheEntry);
}

SHERMES_EXPORT SHLegacyValue _sh_ljs_get_by_id_with_receiver_rjs(
    SHRuntime *shr,
    const SHLegacyValue *source,
    const SHLegacyValue *receiver,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry);
SHERMES_EXPORT SHLegacyValue _sh_ljs_get_by_val_with_receiver_rjs(
    SHRuntime *shr,
    SHLegacyValue *source,
    SHLegacyValue *key,
    SHLegacyValue *receiver);
static inline SHLegacyValue _sh_ljs_get_by_val_rjs(
    SHRuntime *shr,
    SHLegacyValue *source,
    SHLegacyValue *key) {
  return _sh_ljs_get_by_val_with_receiver_rjs(shr, source, key, source);
}

SHERMES_EXPORT SHLegacyValue _sh_ljs_get_own_private_by_sym(
    SHRuntime *shr,
    const SHLegacyValue *source,
    const SHLegacyValue *privateNameKey,
    SHPrivateNameCacheEntry *privateNameCacheEntry);

/// Get a property from the given object \p source given a valid array index
/// \p key.
SHERMES_EXPORT SHLegacyValue
_sh_ljs_get_by_index_rjs(SHRuntime *shr, SHLegacyValue *source, uint32_t key);

/// Put an enumerable property by string id.
SHERMES_EXPORT void _sh_ljs_define_own_by_id(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHSymbolID key,
    SHLegacyValue *value,
    SHWritePropertyCacheEntry *cacheEntry);
/// Put an enumerable property.
SHERMES_EXPORT void _sh_ljs_define_own_by_val(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value);
/// Put a non-enumerable property.
SHERMES_EXPORT void _sh_ljs_define_own_ne_by_val(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value);

SHERMES_EXPORT void _sh_ljs_define_own_by_index(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t key,
    SHLegacyValue *value);

SHERMES_EXPORT void _sh_ljs_define_own_getter_setter_by_val(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *getter,
    SHLegacyValue *setter,
    bool isEnumerable);

/// Add a new private field. This must be a new property.
SHERMES_EXPORT void _sh_ljs_add_own_private_by_sym(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key,
    SHLegacyValue *value);

/// Store to a private field.
SHERMES_EXPORT void _sh_ljs_put_own_private_by_sym(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *privateNameKey,
    SHLegacyValue *value,
    SHPrivateNameCacheEntry *privateNameCacheEntry);

SHERMES_EXPORT SHLegacyValue
_sh_ljs_del_by_id_strict(SHRuntime *shr, SHLegacyValue *target, SHSymbolID key);
SHERMES_EXPORT SHLegacyValue
_sh_ljs_del_by_id_loose(SHRuntime *shr, SHLegacyValue *target, SHSymbolID key);
SHERMES_EXPORT SHLegacyValue _sh_ljs_del_by_val_strict(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key);
SHERMES_EXPORT SHLegacyValue _sh_ljs_del_by_val_loose(
    SHRuntime *shr,
    SHLegacyValue *target,
    SHLegacyValue *key);

/// Get the string associated with the given SHSymbolID. The caller is
/// responsible for ensuring that \p symID is not garbage collected.
SHERMES_EXPORT SHLegacyValue
_sh_ljs_get_string(SHRuntime *shr, SHSymbolID symID);

/// Create a regexp from literal \p regexp_id of \p unit, whose pattern and
/// flags are the symbols \p pattern and \p flags. The bytecode was compiled
/// ahead of time and is shared by every evaluation of the literal.
SHERMES_EXPORT SHLegacyValue _sh_ljs_create_regexp(
    SHRuntime *shr,
    SHUnit *unit,
    SHSymbolID pattern,
    SHSymbolID flags,
    uint32_t regexp_id);

/// \param value the string of the BigInt.
/// \param size  the size of the string \c value.
SHERMES_EXPORT SHLegacyValue
_sh_ljs_create_bigint(SHRuntime *shr, const uint8_t *value, uint32_t size);

SHERMES_EXPORT SHLegacyValue
_sh_ljs_to_property_key(SHRuntime *shr, const SHLegacyValue *val);

SHERMES_EXPORT double _sh_ljs_to_double_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n);
SHERMES_EXPORT bool _sh_ljs_to_boolean(SHLegacyValue b);
SHERMES_EXPORT SHLegacyValue
_sh_ljs_to_numeric_rjs(SHRuntime *shr, const SHLegacyValue *n);
SHERMES_EXPORT double _sh_ljs_to_int32_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n);
static inline double _sh_ljs_to_int32_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  int32_t res;
  if (SH_LIKELY(_sh_ljs_tryfast_truncate_to_int32(*n, &res))) {
    return res;
  }
  return _sh_ljs_to_int32_rjs(shr, n);
}
SHERMES_EXPORT double _sh_ljs_to_uint32_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n);
static inline double _sh_ljs_to_uint32_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  uint32_t res;
  if (SH_LIKELY(_sh_ljs_tryfast_truncate_to_uint32(*n, &res))) {
    return res;
  }
  return _sh_ljs_to_uint32_rjs(shr, n);
}

SHERMES_EXPORT bool _sh_ljs_less_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);

static inline bool _sh_ljs_less_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: check if both values are numbers and compare.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_get_double(*a) < _sh_ljs_get_double(*b);
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_less_rjs(shr, a, b);
}

SHERMES_EXPORT bool _sh_ljs_greater_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);

static inline bool _sh_ljs_greater_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: check if both values are numbers and compare.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_get_double(*a) > _sh_ljs_get_double(*b);
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_greater_rjs(shr, a, b);
}

SHERMES_EXPORT bool _sh_ljs_less_equal_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);

static inline bool _sh_ljs_less_equal_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: check if both values are numbers and compare.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_get_double(*a) <= _sh_ljs_get_double(*b);
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_less_equal_rjs(shr, a, b);
}

SHERMES_EXPORT bool _sh_ljs_greater_equal_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);

static inline bool _sh_ljs_greater_equal_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: check if both values are numbers and compare.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_get_double(*a) >= _sh_ljs_get_double(*b);
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_greater_equal_rjs(shr, a, b);
}
SHERMES_EXPORT bool _sh_ljs_equal_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);

static inline bool _sh_ljs_equal_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: both are non-NaN numbers, compare as doubles.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_get_double(*a) == _sh_ljs_get_double(*b);
  }
  // Fast path: identical raw bits for non-numbers are always equal
  // (same object, same string pointer, null==null, undefined==undefined,
  // same bool). For NaN, raw equality is possible but NaN != NaN.
  if (a->raw == b->raw) {
    return !_sh_ljs_is_double(*a);
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_equal_rjs(shr, a, b);
}

SHERMES_EXPORT bool _sh_ljs_strict_equal(SHLegacyValue a, SHLegacyValue b);

/// Inline fast path for strict equality (===).
/// Handles the common cases inline: same raw bits (identity check for objects,
/// bools, null, undefined, symbols) and both-number comparison. Falls back to
/// out-of-line for string/bigint deep compare, different types, and mixed NaN
/// patterns.
static inline bool _sh_ljs_strict_equal_inline(
    SHLegacyValue a,
    SHLegacyValue b) {
  // Fast path: raw bits match → equal iff not a NaN.
  // Covers: same object pointer, same bool, null===null, undefined===undefined,
  // same symbol, same non-NaN number (same bit pattern).
  // For NaN, a.f64 == a.f64 is false per IEEE 754.
  if (a.raw == b.raw)
    return !_sh_ljs_is_double(a) || a.f64 == a.f64;
  // Fast path: both are non-NaN numbers → compare as doubles.
  // Handles +0 === -0 (true) and different numeric values.
  if (_sh_ljs_are_both_non_nan_numbers(a, b))
    return a.f64 == b.f64;
  // Out-of-line: string/bigint deep compare, different types, mixed NaN.
  return _sh_ljs_strict_equal(a, b);
}

SHERMES_EXPORT SHLegacyValue
_sh_ljs_add_rjs(SHRuntime *shr, const SHLegacyValue *a, const SHLegacyValue *b);

static inline SHLegacyValue _sh_ljs_add_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: check if both values are numbers and add.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_double(_sh_ljs_get_double(*a) + _sh_ljs_get_double(*b));
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_add_rjs(shr, a, b);
}

SHERMES_EXPORT SHLegacyValue
_sh_ljs_sub_rjs(SHRuntime *shr, const SHLegacyValue *a, const SHLegacyValue *b);

static inline SHLegacyValue _sh_ljs_sub_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: check if both values are numbers and subtract.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_double(_sh_ljs_get_double(*a) - _sh_ljs_get_double(*b));
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_sub_rjs(shr, a, b);
}

SHERMES_EXPORT SHLegacyValue
_sh_ljs_mul_rjs(SHRuntime *shr, const SHLegacyValue *a, const SHLegacyValue *b);

static inline SHLegacyValue _sh_ljs_mul_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: check if both values are numbers and multiply.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_double(_sh_ljs_get_double(*a) * _sh_ljs_get_double(*b));
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_mul_rjs(shr, a, b);
}

SHERMES_EXPORT SHLegacyValue
_sh_ljs_div_rjs(SHRuntime *shr, const SHLegacyValue *a, const SHLegacyValue *b);

static inline SHLegacyValue _sh_ljs_div_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: check if both values are numbers and divide.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_double(_sh_ljs_get_double(*a) / _sh_ljs_get_double(*b));
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_div_rjs(shr, a, b);
}

/// Run a % b if b != 0, otherwise use `fmod` so the operation can't fail. Note
/// that we only do this for unsigned integers, because signed mod can produce
/// -0, which requires special handling.
/// \return the double representing the result of the JS mod operation on the
///   two integers.
static inline double _sh_mod_uint32(uint32_t a, uint32_t b) {
  if (b == 0) {
    // Avoid the divide-by-zero and return NaN directly.
    return nan("");
  }
  return (double)(a % b);
}

static inline SH_READNONE double _sh_mod_double(double a, double b) {
  uint32_t aUint, bUint;
  bool aIsUint = sh_tryfast_f64_to_u32(a, aUint);
  bool bIsUint = sh_tryfast_f64_to_u32(b, bUint);

  // If both numbers are unsigned integers, use the fast path.
  // `-0` must be handled specially, so just check `a != 0` for simplicity.
  if (a != 0 && aIsUint && bIsUint) {
    return _sh_mod_uint32(aUint, bUint);
  }

  // Actually have to do the double operation.
  return fmod(a, b);
}

SHERMES_EXPORT SHLegacyValue
_sh_ljs_mod_rjs(SHRuntime *shr, const SHLegacyValue *a, const SHLegacyValue *b);

static inline SHLegacyValue _sh_ljs_mod_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  // Fast path: check if both values are numbers and perform modulo.
  if (SH_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    return _sh_ljs_double(
        _sh_mod_double(_sh_ljs_get_double(*a), _sh_ljs_get_double(*b)));
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_mod_rjs(shr, a, b);
}

SHERMES_EXPORT SHLegacyValue
_sh_ljs_is_in_rjs(SHRuntime *shr, SHLegacyValue *name, SHLegacyValue *obj);
SHERMES_EXPORT SHLegacyValue _sh_ljs_private_is_in_rjs(
    SHRuntime *shr,
    SHLegacyValue *privateName,
    SHLegacyValue *target,
    SHPrivateNameCacheEntry *cacheEntry);
SHERMES_EXPORT SHLegacyValue _sh_ljs_instance_of_rjs(
    SHRuntime *shr,
    SHLegacyValue *object,
    SHLegacyValue *constructor);

SHERMES_EXPORT SHLegacyValue
_sh_ljs_inc_rjs(SHRuntime *shr, const SHLegacyValue *n);

static inline SHLegacyValue _sh_ljs_inc_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  // Fast path: check if value is a number and increment.
  if (SH_LIKELY(_sh_ljs_is_non_nan_number(*n))) {
    return _sh_ljs_double(_sh_ljs_get_double(*n) + 1.0);
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_inc_rjs(shr, n);
}

SHERMES_EXPORT SHLegacyValue
_sh_ljs_dec_rjs(SHRuntime *shr, const SHLegacyValue *n);

static inline SHLegacyValue _sh_ljs_dec_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  // Fast path: check if value is a number and decrement.
  if (SH_LIKELY(_sh_ljs_is_non_nan_number(*n))) {
    return _sh_ljs_double(_sh_ljs_get_double(*n) - 1.0);
  }

  // Otherwise: go to out-of-line slow path.
  return _sh_ljs_dec_rjs(shr, n);
}

SHERMES_EXPORT SHLegacyValue _sh_ljs_bit_or_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);

static inline SHLegacyValue _sh_ljs_bit_or_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  int32_t aInt, bInt;
  if (SH_LIKELY(
          _sh_ljs_tryfast_truncate_to_int32(*a, &aInt) &&
          _sh_ljs_tryfast_truncate_to_int32(*b, &bInt))) {
    return _sh_ljs_double((double)(aInt | bInt));
  }
  return _sh_ljs_bit_or_rjs(shr, a, b);
}

SHERMES_EXPORT SHLegacyValue _sh_ljs_bit_and_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);
static inline SHLegacyValue _sh_ljs_bit_and_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  int32_t aInt, bInt;
  if (SH_LIKELY(
          _sh_ljs_tryfast_truncate_to_int32(*a, &aInt) &&
          _sh_ljs_tryfast_truncate_to_int32(*b, &bInt))) {
    return _sh_ljs_double((double)(aInt & bInt));
  }
  return _sh_ljs_bit_and_rjs(shr, a, b);
}
SHERMES_EXPORT SHLegacyValue _sh_ljs_bit_xor_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);
static inline SHLegacyValue _sh_ljs_bit_xor_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  int32_t aInt, bInt;
  if (SH_LIKELY(
          _sh_ljs_tryfast_truncate_to_int32(*a, &aInt) &&
          _sh_ljs_tryfast_truncate_to_int32(*b, &bInt))) {
    return _sh_ljs_double((double)(aInt ^ bInt));
  }
  return _sh_ljs_bit_xor_rjs(shr, a, b);
}
/// Math.imul(a, b): unlike the other bitwise binops above, it is not
/// BigInt-aware -- it always converts both operands with ToInt32 (which
/// throws a TypeError for a BigInt operand) and never falls back to a
/// BigInt result.
SHERMES_EXPORT SHLegacyValue _sh_ljs_imul_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);
static inline SHLegacyValue _sh_ljs_imul_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  int32_t aInt, bInt;
  if (SH_LIKELY(
          _sh_ljs_tryfast_truncate_to_int32(*a, &aInt) &&
          _sh_ljs_tryfast_truncate_to_int32(*b, &bInt))) {
    // Multiply as unsigned 32-bit values (to avoid signed overflow UB) and
    // reinterpret the result as signed.
    return _sh_ljs_double((double)(int32_t)((uint32_t)aInt * (uint32_t)bInt));
  }
  return _sh_ljs_imul_rjs(shr, a, b);
}
SHERMES_EXPORT SHLegacyValue _sh_ljs_right_shift_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);
static inline SHLegacyValue _sh_ljs_right_shift_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  int32_t aInt, bInt;
  if (SH_LIKELY(
          _sh_ljs_tryfast_truncate_to_int32(*a, &aInt) &&
          _sh_ljs_tryfast_truncate_to_int32(*b, &bInt))) {
    uint32_t bUInt = bInt & 0x1f;
    return _sh_ljs_double((double)(aInt >> bUInt));
  }
  return _sh_ljs_right_shift_rjs(shr, a, b);
}
SHERMES_EXPORT SHLegacyValue _sh_ljs_unsigned_right_shift_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);
static inline SHLegacyValue _sh_ljs_unsigned_right_shift_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  uint32_t aUint;
  int32_t bInt;
  if (SH_LIKELY(
          _sh_ljs_tryfast_truncate_to_uint32(*a, &aUint) &&
          _sh_ljs_tryfast_truncate_to_int32(*b, &bInt))) {
    uint32_t bUInt = bInt & 0x1f;
    return _sh_ljs_double((double)(aUint >> bUInt));
  }
  return _sh_ljs_unsigned_right_shift_rjs(shr, a, b);
}
SHERMES_EXPORT SHLegacyValue _sh_ljs_left_shift_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b);
static inline SHLegacyValue _sh_ljs_left_shift_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  int32_t aInt, bInt;
  if (SH_LIKELY(
          _sh_ljs_tryfast_truncate_to_int32(*a, &aInt) &&
          _sh_ljs_tryfast_truncate_to_int32(*b, &bInt))) {
    uint32_t bUInt = bInt & 0x1f;
    return _sh_ljs_double((double)(aInt << bUInt));
  }
  return _sh_ljs_left_shift_rjs(shr, a, b);
}
SHERMES_EXPORT SHLegacyValue
_sh_ljs_bit_not_rjs(SHRuntime *shr, const SHLegacyValue *a);
static inline SHLegacyValue _sh_ljs_bit_not_rjs_inline(
    SHRuntime *shr,
    const SHLegacyValue *a) {
  int32_t aInt;
  if (SH_LIKELY(_sh_ljs_tryfast_truncate_to_int32(*a, &aInt))) {
    return _sh_ljs_double((double)(~aInt));
  }
  return _sh_ljs_bit_not_rjs(shr, a);
}
SHERMES_EXPORT SHLegacyValue
_sh_ljs_minus_rjs(SHRuntime *shr, const SHLegacyValue *n);

SHERMES_EXPORT SHLegacyValue
_sh_ljs_add_empty_string_rjs(SHRuntime *shr, const SHLegacyValue *a);

SHERMES_EXPORT SHLegacyValue
_sh_ljs_create_private_name(SHRuntime *shr, SHSymbolID descStrID);

/// Concatenate \p argCount strings together into a new StringPrimitive.
/// \param argCount the number of varargs that follow.
/// \param ... the (const SHLegacyValue *) string arguments to concatenate.
SHERMES_EXPORT SHLegacyValue
_sh_string_concat(SHRuntime *shr, uint32_t argCount, ...);

SHERMES_EXPORT SHLegacyValue _sh_ljs_typeof(SHRuntime *shr, SHLegacyValue *v);
SHERMES_EXPORT bool _sh_ljs_typeof_is(SHLegacyValue val, uint16_t types);

/// Bit positions of the TypeOfIsTypes passed to _sh_ljs_typeof_is().
enum {
#define HERMES_TYPEOF_IS_TYPES_TYPE(name) SH_TYPEOF_IS_##name##_INDEX,
  HERMES_TYPEOF_IS_TYPES
#undef HERMES_TYPEOF_IS_TYPES_TYPE
};

/// Bits of the TypeOfIsTypes passed to _sh_ljs_typeof_is().
enum {
#define HERMES_TYPEOF_IS_TYPES_TYPE(name) \
  SH_TYPEOF_IS_##name = 1 << SH_TYPEOF_IS_##name##_INDEX,
  HERMES_TYPEOF_IS_TYPES
#undef HERMES_TYPEOF_IS_TYPES_TYPE
};

/// Inline version of _sh_ljs_typeof_is(). Meant to be called with constant
/// \p types, so that it folds to the few checks that matter.
static inline bool _sh_ljs_typeof_is_inline(SHLegacyValue val, uint16_t types) {
  if (_sh_ljs_is_object(val)) {
    const uint16_t objOrFunc = SH_TYPEOF_IS_Object | SH_TYPEOF_IS_Function;
    if ((types & objOrFunc) == 0)
      return false;
    if ((types & objOrFunc) == objOrFunc)
      return true;
    unsigned kind = ((SHGCCell *)_sh_ljs_get_pointer(val))->kindAndSize.kind;
    bool isCallable =
        kind >= SHCellKind_Callable_first && kind <= SHCellKind_Callable_last;
    return (types & SH_TYPEOF_IS_Function) ? isCallable : !isCallable;
  }
  return ((types & SH_TYPEOF_IS_Undefined) && _sh_ljs_is_undefined(val)) ||
      ((types & SH_TYPEOF_IS_Null) && _sh_ljs_is_null(val)) ||
      ((types & SH_TYPEOF_IS_String) && _sh_ljs_is_string(val)) ||
      ((types & SH_TYPEOF_IS_Symbol) && _sh_ljs_is_symbol(val)) ||
      ((types & SH_TYPEOF_IS_Boolean) && _sh_ljs_is_bool(val)) ||
      ((types & SH_TYPEOF_IS_Number) && _sh_ljs_is_double(val)) ||
      ((types & SH_TYPEOF_IS_Bigint) && _sh_ljs_is_bigint(val));
}

SHERMES_EXPORT SHLegacyValue _sh_ljs_new_object(SHRuntime *shr);
SHERMES_EXPORT SHLegacyValue _sh_ljs_new_object_with_parent(
    SHRuntime *shr,
    SHUnit *unit,
    const SHLegacyValue *parent,
    uint32_t shapeTableIndex);

/// \p shapeTableIndex the entry index in the literal shape table.
/// \p valBufferOffset the beginning offset in the literal value buffer.
SHERMES_EXPORT SHLegacyValue _sh_ljs_new_object_with_buffer(
    SHRuntime *shr,
    SHUnit *unit,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset);

/// Like _sh_ljs_new_object_with_buffer, but also takes a \p parent.
SHERMES_EXPORT SHLegacyValue _sh_ljs_new_object_with_buffer_and_parent(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset);

SHERMES_EXPORT SHLegacyValue _sh_new_typed_object_with_buffer(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset);

SHERMES_EXPORT SHLegacyValue _sh_new_typed_non_enum_object_with_buffer(
    SHRuntime *shr,
    SHUnit *unit,
    SHLegacyValue *parent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset);

/// \p size the size of the resultant array.
SHERMES_EXPORT SHLegacyValue _sh_ljs_new_array(SHRuntime *shr, uint32_t size);

/// \p numElements the size of the resultant array.
/// \p numLiterals the number of literals to read off the buffer
///   to populate the start of the array.
SHERMES_EXPORT SHLegacyValue _sh_ljs_new_array_with_buffer(
    SHRuntime *shr,
    SHUnit *unit,
    uint32_t numElements,
    uint32_t numLiterals,
    uint32_t bufferIndex);

/// Define an own property in a dense JavaScript array at a specific index.
/// Requires that the array is dense and that the ArrayStorage
/// underlying it has a size which is greater than the arrayIndex operand.
SHERMES_EXPORT void _sh_ljs_define_own_in_dense_array(
    SHRuntime *shr,
    SHLegacyValue *array,
    SHLegacyValue *prop,
    uint32_t idx);

/// \return a newly created fast array with the given \p capacity.
SHERMES_EXPORT SHLegacyValue
_sh_new_fastarray(SHRuntime *shr, uint32_t capacity);

/// \return a newly created fast array with the given \p prototype and
/// \p capacity.
SHERMES_EXPORT SHLegacyValue _sh_new_fastarray_with_proto(
    SHRuntime *shr,
    SHLegacyValue *prototype,
    uint32_t capacity);

/// Typed Object.keys/values/entries. \return a new fast array with the given
/// \p prototype containing the enumerable own string-keyed property names of
/// \p obj, their values, or [key, value] tuple objects.
/// @{
SHERMES_EXPORT SHLegacyValue
_sh_object_keys(SHRuntime *shr, SHLegacyValue obj, SHLegacyValue prototype);
SHERMES_EXPORT SHLegacyValue
_sh_object_values(SHRuntime *shr, SHLegacyValue obj, SHLegacyValue prototype);
SHERMES_EXPORT SHLegacyValue
_sh_object_entries(SHRuntime *shr, SHLegacyValue obj, SHLegacyValue prototype);
/// @}

/// \p base[in/out] is the object to be iterated.
/// \p index[out] is the next index in the list.
/// \p size[out] is the size of the list.
/// \return the array of properties, or undefined if there is nothing to
///   iterate.
SHERMES_EXPORT SHLegacyValue _sh_ljs_get_pname_list_rjs(
    SHRuntime *shr,
    SHLegacyValue *base,
    SHLegacyValue *index,
    SHLegacyValue *size);

/// \p props is the array of properties
/// \p base is the object to be iterated.
/// \p index[in/out] is the iterating index.
/// \p size is the size of the property list.
/// \return the next property, undefined if unavailable
SHERMES_EXPORT SHLegacyValue _sh_ljs_get_next_pname_rjs(
    SHRuntime *shr,
    SHLegacyValue *props,
    SHLegacyValue *base,
    SHLegacyValue *index,
    SHLegacyValue *size);

/// If \p src is an array with an unmodified [Symbol.iterator], return 0. Else
/// return the Symbol.iterator property of \p src and update \p src in place to
/// the .next() method on the iterator.
SHERMES_EXPORT SHLegacyValue
_sh_ljs_iterator_begin_rjs(SHRuntime *shr, SHLegacyValue *src);

/// Move \p iteratorOrIdx forward, and return the value at the new iterator. \p
/// srcOrNext is the source if we are iterating over an array, or the .next()
/// method otherwise. An exhausted iterator is represented by undefined;
/// advancing it returns undefined without accessing \p srcOrNext.
SHERMES_EXPORT SHLegacyValue _sh_ljs_iterator_next_rjs(
    SHRuntime *shr,
    SHLegacyValue *iteratorOrIdx,
    const SHLegacyValue *srcOrNext);

/// If the given iterator is an object, call iterator.return(), ignoring
/// catchable exceptions if ignoreExceptions is true.
SHERMES_EXPORT void _sh_ljs_iterator_close_rjs(
    SHRuntime *shr,
    const SHLegacyValue *iteratorOrIdx,
    bool ignoreExceptions);

SHERMES_EXPORT SHLegacyValue
_sh_ljs_direct_eval(SHRuntime *shr, SHLegacyValue *evalText, bool strictCaller);

/// Call the \c hermes::truncateToInt32SlowPath function.
/// Annotated ((const)) to let the compiler know that the function will not
/// read/write global memory at all (it's just doing math), allowing for
/// optimizations around the locals struct.
SHERMES_EXPORT SH_READNONE int32_t _sh_to_int32_double_slow_path(double d);

/// C version of the hermes::truncateToInt32 function.
/// Inlines the fast path for SH to use, calls out to the slow path.
static inline int32_t _sh_to_int32_double(double d) {
  // If we are compiling with ARM v8.3 or above, there is a special instruction
  // to do the conversion.
#ifdef __ARM_FEATURE_JCVT
  return __builtin_arm_jcvt(d);
#endif

  // NOTE: this implementation should be consistent with truncateToInt32()
  // in Support/Conversions.h

  // Use __builtin_constant_p() for better perf and to avoid UB caused by
  // constant propagation.
#if defined(__GNUC__)
  if (__builtin_constant_p(d)) {
    // Be aggressive on constant path, use the maximum precision bits
    // of double type for range check.
    if (d >= (int64_t)(-1ULL << 53) && d <= (1LL << 53))
      return (int32_t)(int64_t)d;
    return _sh_to_int32_double_slow_path(d);
  }
#endif

  if (HERMES_TRYFAST_F64_TO_64_IS_FAST) {
    int64_t fast;
    if (SH_LIKELY(sh_tryfast_f64_to_i64(d, fast)))
      return (int32_t)fast;
  } else {
    int32_t fast;
    if (SH_LIKELY(sh_tryfast_f64_to_i32(d, fast)))
      return fast;
  }

  return _sh_to_int32_double_slow_path(d);
}

/// C version of the hermes::truncateToUInt32 function.
/// Inlines the fast path for SH to use, calls out to the slow path.
static inline uint32_t _sh_to_uint32_double(double d) {
  // NOTE: this implementation should be consistent with truncateToUint32()
  // in Support/Conversions.h
  // If we are compiling with ARM v8.3 or above, there is a special instruction
  // to do the conversion.
#ifdef __ARM_FEATURE_JCVT
  return __builtin_arm_jcvt(d);
#endif

  // NOTE: this implementation should be consistent with truncateToInt32()
  // in Support/Conversions.h

  // Use __builtin_constant_p() for better perf and to avoid UB caused by
  // constant propagation.
#if defined(__GNUC__)
  if (__builtin_constant_p(d)) {
    // Be aggressive on constant path, use the maximum precision bits
    // of double type for range check.
    if (d >= (int64_t)(-1ULL << 53) && d <= (1LL << 53))
      return (int32_t)(int64_t)d;
    return (uint32_t)_sh_to_int32_double_slow_path(d);
  }
#endif

  if (HERMES_TRYFAST_F64_TO_64_IS_FAST) {
    uint64_t fast;
    if (SH_LIKELY(sh_tryfast_f64_to_u64(d, fast)))
      return (uint32_t)fast;
  } else {
    uint32_t fast;
    if (SH_LIKELY(sh_tryfast_f64_to_u32(d, fast)))
      return fast;
  }

  return (uint32_t)_sh_to_int32_double_slow_path(d);
}

/// Store a property into direct storage.
SHERMES_EXPORT void _sh_prstore_direct(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value);

/// Store a property into direct storage when both the \p value being stored and
/// the destination field referred to by \p target and \p propIndex are known to
/// be of a given type.
SHERMES_EXPORT void _sh_prstore_direct_bool(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value);
SHERMES_EXPORT void _sh_prstore_direct_number(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value);
SHERMES_EXPORT void _sh_prstore_direct_object(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value);
SHERMES_EXPORT void _sh_prstore_direct_string(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value);

/// Store a property into indirect storage. Note that propIndex is relative to
/// the indirect storage.
SHERMES_EXPORT void _sh_prstore_indirect(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value);

SHERMES_EXPORT SH_ATTRIBUTE_NORETURN void _sh_unreachable();

/// Store a property into direct or indirect storage depending on its index.
static inline void _sh_prstore(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  if (propIndex < HERMESVM_DIRECT_PROPERTY_SLOTS) {
    _sh_prstore_direct(shr, target, propIndex, value);
  } else {
    _sh_prstore_indirect(
        shr, target, propIndex - HERMESVM_DIRECT_PROPERTY_SLOTS, value);
  }
}

/// Store a bool property into direct or indirect storage depending on its
/// index.
static inline void _sh_prstore_bool(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_bool(*value));
  if (propIndex < HERMESVM_DIRECT_PROPERTY_SLOTS) {
#ifndef HERMESVM_BOXED_DOUBLES
    ((SHJSObjectAndDirectProps *)_sh_ljs_get_pointer(*target))
        ->directProps[propIndex] = *value;
#else
    _sh_prstore_direct_bool(shr, target, propIndex, value);
#endif
  } else {
    _sh_prstore_indirect(
        shr, target, propIndex - HERMESVM_DIRECT_PROPERTY_SLOTS, value);
  }
}

/// Store a number property into direct or indirect storage depending on its
/// index.
static inline void _sh_prstore_number(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_double(*value));
  if (propIndex < HERMESVM_DIRECT_PROPERTY_SLOTS) {
#ifndef HERMESVM_BOXED_DOUBLES
    ((SHJSObjectAndDirectProps *)_sh_ljs_get_pointer(*target))
        ->directProps[propIndex] = *value;
#else
    _sh_prstore_direct_number(shr, target, propIndex, value);
#endif
  } else {
    _sh_prstore_indirect(
        shr, target, propIndex - HERMESVM_DIRECT_PROPERTY_SLOTS, value);
  }
}

/// Store an object property into direct or indirect storage depending on its
/// index.
static inline void _sh_prstore_object(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_object(*value));
  if (propIndex < HERMESVM_DIRECT_PROPERTY_SLOTS) {
    _sh_prstore_direct_object(shr, target, propIndex, value);
  } else {
    _sh_prstore_indirect(
        shr, target, propIndex - HERMESVM_DIRECT_PROPERTY_SLOTS, value);
  }
}

/// Store a string property into direct or indirect storage depending on its
/// index.
static inline void _sh_prstore_string(
    SHRuntime *shr,
    SHLegacyValue *target,
    uint32_t propIndex,
    SHLegacyValue *value) {
  assert(_sh_ljs_is_string(*value));
  if (propIndex < HERMESVM_DIRECT_PROPERTY_SLOTS) {
    _sh_prstore_direct_string(shr, target, propIndex, value);
  } else {
    _sh_prstore_indirect(
        shr, target, propIndex - HERMESVM_DIRECT_PROPERTY_SLOTS, value);
  }
}

/// Throw an array out-of-bounds error.
SHERMES_EXPORT SH_ATTRIBUTE_NORETURN void _sh_throw_array_oob(SHRuntime *shr);

/// Out-of-line implementation for loading the element at \p index from the
/// FastArray \p array.
SHERMES_EXPORT SHLegacyValue
_sh_fastarray_load(SHRuntime *shr, SHLegacyValue *array, double index);

/// Store \p storedValue to the FastArray \p array at \p index.
SHERMES_EXPORT void _sh_fastarray_store(
    SHRuntime *shr,
    const SHLegacyValue *storedValue,
    SHLegacyValue *array,
    double index);

/// Push the given element \p pushedValue onto the given fast array \p array.
SHERMES_EXPORT void _sh_fastarray_push(
    SHRuntime *shr,
    SHLegacyValue *pushedValue,
    SHLegacyValue *array);

/// Append the elements from the fast array \p other onto the fast array
/// \p array.
SHERMES_EXPORT void _sh_fastarray_append(
    SHRuntime *shr,
    SHLegacyValue *other,
    SHLegacyValue *array);

static inline SHLegacyValue _sh_fastarray_length(
    SHRuntime *shr,
    SHLegacyValue *array) {
#ifdef HERMESVM_BOXED_DOUBLES
  SHFastArray *arr = (SHFastArray *)_sh_ljs_get_pointer(*array);
  SHArrayStorageSmall *storage =
      (SHArrayStorageSmall *)_sh_cp_decode_non_null(shr, arr->indexedStorage);
  return _sh_ljs_double(storage->size);
#else
  return ((SHFastArray *)_sh_ljs_get_pointer(*array))->length;
#endif
}

/// \return true if the given JSObject is a proxy.
SHERMES_EXPORT bool _sh_ljs_is_proxy(SHRuntime *shr, SHLegacyValue object);

/// \return the parent of the legacy, ordinary object \p object. It must not be
/// a proxy.
static inline SHLegacyValue _sh_ljs_load_parent_no_traps(
    SHRuntime *shr,
    SHLegacyValue object) {
  SHJSObject *objectPtr = (SHJSObject *)_sh_ljs_get_pointer(object);
  SHCompressedPointer clazzCompressed = {.raw = objectPtr->clazz};
  SHHiddenClass *clazz =
      (SHHiddenClass *)_sh_cp_decode_non_null(shr, clazzCompressed);
  assert(!_sh_ljs_is_proxy(shr, object) && "proxy is not supported");
  if (clazz->objectParent) {
    SHCompressedPointer parent = {.raw = clazz->objectParent};
    SHJSObject *parentPtr = (SHJSObject *)_sh_cp_decode_non_null(shr, parent);
    return _sh_ljs_object(parentPtr);
  }
  return _sh_ljs_null();
}

static inline SHLegacyValue _sh_typed_load_parent(
    SHRuntime *shr,
    const SHLegacyValue *object) {
  SHCompressedPointer clazzCompressed = {
      .raw = ((SHJSObject *)_sh_ljs_get_pointer(*object))->clazz};
  SHHiddenClass *clazz =
      (SHHiddenClass *)_sh_cp_decode_non_null(shr, clazzCompressed);
  SHCompressedPointer parentCompressed = {.raw = clazz->objectParent};
  return _sh_ljs_object(_sh_cp_decode_non_null(shr, parentCompressed));
}

/// If the double value is within representable integer range, convert it,
/// otherwise throw.
static inline uint64_t _sh_to_uint64_double_or_throw(SHRuntime *shr, double d) {
  if (SH_LIKELY(d >= 0 && d <= (double)(1LL << 53))) {
    return (uint64_t)d;
  }
  _sh_throw_type_error_ascii(shr, "number not representable as uint64_t");
}

/// If the double value is within representable integer range, convert it,
/// otherwise throw.
static inline int64_t _sh_to_int64_double_or_throw(SHRuntime *shr, double d) {
  if (SH_LIKELY(
          d >= (double)(int64_t)(-1ULL << 53) && d <= (double)(1LL << 53))) {
    return (int64_t)d;
  }
  _sh_throw_type_error_ascii(shr, "number not representable as int64_t");
}

/// If the int64 value is within representable integer range, convert it,
/// otherwise throw
static inline double _sh_to_double_int64_or_throw(SHRuntime *shr, int64_t i) {
  if (SH_LIKELY(i >= (int64_t)(-1ULL << 53) && i <= (1LL << 53))) {
    return (double)i;
  }
  _sh_throw_type_error_ascii(shr, "int64_t not representable as number");
}
/// If the uint64 value is within representable integer range, convert it,
/// otherwise throw
static inline double _sh_to_double_uint64_or_throw(SHRuntime *shr, uint64_t i) {
  if (SH_LIKELY(i <= 1ULL << 53)) {
    // On x86_64 there is no instruction to convert uint64_t to double, making
    // it a bit slower. Fortunately, we know that `i` is in range for int64_t,
    // so we can just pretend it is signed.
    return (double)(int64_t)i;
  }
  _sh_throw_type_error_ascii(shr, "uint64_t not representable as number");
}

/// Encode a pointer as SHLegacyValue or throw if it can't be encoded (if the
/// bit pattern would repersent a NaN).
static inline SHLegacyValue _sh_ljs_native_pointer_or_throw(
    SHRuntime *shr,
    void *p) {
#if HERMESVM_SIZEOF_VOID_P == 8
  // Check that the bit pattern is not a NaN. A NaN is encoded when the exponent
  // (bits 52-62) are all 1s and the mantissa is not 0. For this check we ignore
  // the mantissa and just look at the exponent.
  // It is faster to check if all 11 bits are 1s if we invert the number and
  // check for 0 instead.
  if (SH_LIKELY(((~(uint64_t)(uintptr_t)p >> 52) & 0x7ff) != 0)) {
    return _sh_ljs_native_pointer(p);
  }
  _sh_throw_type_error_ascii(shr, "pointer not representable as number");
#else
  (void)shr;
  return _sh_ljs_native_pointer(p);
#endif
}

/// \return the C errno value.
SHERMES_EXPORT int _sh_errno(void);

/// Convert a C string to a JS string.
/// \param str the C string to convert.
/// \param len the length of the string, or -1 if the length is unknown.
SHERMES_EXPORT SHLegacyValue
_sh_asciiz_to_string(SHRuntime *shr, const char *str, ptrdiff_t len);

static inline void _sh_ptr_write_char(char *ptr, int offset, char c) {
  ptr[offset] = c;
}
static inline unsigned char _sh_ptr_read_uchar(unsigned char *ptr, int offset) {
  return ptr[offset];
}

//===----------------------------------------------------------------------===//
/// \section Type punning
///
/// These functions access arbitrary values from arbitrary locations, with the
/// only requirement being that the location is aligned to the size of the
/// value. This should be generally safe with strict aliasing disabled, but we
/// still want to be extra safe, so we use \c __builtin_memcpy().
///
/// The main limitation of the memcpy() approach is that in the general case it
/// doesn't preserve alignment information, so it theoretically can emit
/// suboptimal instructions when we know the address is aligned. We do our best
/// to give the compiler the alignment, both by using a typed pointer, and by
/// using \c __builtin_assume_aligned().
///
/// This is actually unnecessary in most cases, because in practice all
/// architectures that we target have efficient unaligned access of most types,
/// with the only exception being ARM32 with 64-bit values.
///
/// The situation on ARM32 is interesting, because it differs between int64_t
/// and double, between armv7-a and prior architectures, as well as between
/// GCC and Clang/LLVM.
/// - armv7-a can efficiently access unaligned double values. So, on armv7-a
/// the only case where we need to be careful is int64_t.
/// - GCC utilizes the alignment information we give it, resulting in the best
/// possible code on all architectures.
/// - Clang/LLVM loses the alignment information, except in very simple cases
/// where the offset is zero.
///
/// Given that in practice we target armv7-a and later and Clang/LLVM, the net
/// result is that accesses to int64_t on Arm32 are slightly suboptimal when
/// the offset is non-zero (at compile time). This is unfortunate, but it's not
/// really a critical problem.
//===----------------------------------------------------------------------===//

/// Write the supplied \p value to the given \p dest at the given \p offset.
/// Note that the offset is in sizeof(void *) units. It is required that the
/// destination pointer is correctly aligned (which is implied by its type).
static inline void _sh_ptr_write_ptr(void **dest, int offset, void *value) {
#ifdef __GNUC__
  __builtin_memcpy(
      __builtin_assume_aligned(&dest[offset], sizeof(value)),
      &value,
      sizeof(value));
#else
  memcpy(&dest[offset], &value, sizeof(value));
#endif
}
/// Read a pointer from the given \p src at the given \p offset.
/// Note that the offset is in sizeof(void *) units. It is required that the
/// source pointer is correctly aligned (which is implied by its type).
static inline void *_sh_ptr_read_ptr(void **src, int offset) {
  void *value;
#ifdef __GNUC__
  __builtin_memcpy(
      &value,
      __builtin_assume_aligned(&src[offset], sizeof(value)),
      sizeof(value));
#else
  memcpy(&value, &src[offset], sizeof(value));
#endif
  return value;
}

#ifdef __cplusplus
}
#endif

/// A C++ SH init.
#ifdef __cplusplus
namespace hermes::vm {
class RuntimeConfig;
}

/// Create a runtime instance using the supplied configuration.
SHERMES_EXPORT SHRuntime *_sh_init(const hermes::vm::RuntimeConfig &config);

namespace facebook::hermes {
class HermesRuntime;
}

/// Get the HermesRuntime that owns the given SHRuntime. This can be used to
/// access JSI functionality from an SHRuntime.
SHERMES_EXPORT facebook::hermes::HermesRuntime *_sh_get_hermes_runtime(
    SHRuntime *shr);
#endif // __cplusplus

#endif // HERMES_STATIC_H_H
