/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_napi_internal.h"

#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/Support/Buffer.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/RootAcceptor.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/TwineChar16.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <cstring>

//===========================================================================
// Error messages for napi_status codes.
//===========================================================================

/// Human-readable messages for each napi_status enum value.
/// Must be kept in sync with the napi_status enum in
/// js_native_api_types.h.
static const char *errorMessages[] = {
    nullptr, // napi_ok
    "Invalid argument",
    "An object was expected",
    "A string was expected",
    "A string or symbol was expected",
    "A function was expected",
    "A number was expected",
    "A boolean was expected",
    "An array was expected",
    "Unknown failure",
    "An exception is pending",
    "The async work item was cancelled",
    "napi_escape_handle already called on scope",
    "Invalid handle scope usage",
    "Invalid callback scope usage",
    "Thread-safe function queue is full",
    "Thread-safe function handle is closing",
    "A bigint was expected",
    "A date was expected",
    "An arraybuffer was expected",
    "A detachable arraybuffer was expected",
    "Main thread would deadlock",
    "External buffers are not allowed",
    "Cannot run JavaScript",
};

static_assert(
    sizeof(errorMessages) / sizeof(errorMessages[0]) == napi_cannot_run_js + 1,
    "errorMessages array must match napi_status enum");

//===========================================================================
// napi_env__ implementation
//===========================================================================

napi_env__::napi_env__(hermes::vm::Runtime &runtime, hermes_napi_host *host)
    : runtime(runtime), host_(host) {
  napi_clear_last_error(this);

  // Allocate the first handle block so we always have somewhere to
  // store handles when a scope is opened.
  blocks_.push_back(std::make_unique<HandleBlock>());

  // Register a custom root marking function so the GC knows about
  // values held by this environment (pending exception, handle scope
  // slots, persistent references, and active deferreds).
  runtime.addCustomRootsFunction(
      [this](hermes::vm::GC *, hermes::vm::RootAcceptor &acceptor) {
        // Mark the pending exception if one is set.
        if (hasPendingException) {
          acceptor.accept(pendingException);
        }
        // Mark all live handle scope slots.
        markHandleScopes(acceptor);
        // Mark all strong persistent references.
        markReferences(acceptor);
        // Mark all active deferreds.
        markDeferreds(acceptor);
        // Mark all active thread-safe functions.
        markTsfns(acceptor);
      });

  // Register a callback so that pending finalizers are drained
  // whenever Runtime::drainJobs() is called (e.g., between promise
  // microtask batches).
  runtime.addDrainJobsCallback([this]() { drainPendingFinalizers(); });

  // Drive substantive env teardown from ~Runtime BEFORE finalizeAll,
  // while the heap is still functional. Cleanup hooks and ref
  // finalizers may allocate and call into JS.
  runtime.addShutdownCallback([this]() { shutdown(); });

  // After finalizeAll runs, drain wrap callbacks queued by
  // finalizeNapiObjectData when getHeap().finalizeAll() finalized
  // wrapped objects still reachable from JS roots at teardown
  // (callbacks for objects collected earlier — during normal
  // operation or during shutdown()'s user-code phases — have
  // already been drained with full env access).
  //
  // These run on the mutator with a non-null env, so spec-
  // conformant finalizers (which only free native resources)
  // work normally.
  //
  // The GC heap is now in a post-mortem state — every cell has
  // had its finalizer called, and allocating new JS values or
  // calling back into JS would trigger a collection that
  // re-finalizes those cells (undefined behavior). We set
  // inPostShutdownDrain_ before the drain; CHECK_ENV checks the
  // flag and returns napi_cannot_run_js for every NAPI function.
  // Spec-conformant finalizers ignore the return; non-conformant
  // ones get a defined error instead of an undefined crash. Then
  // delete the env.
  runtime.addPostShutdownDeleter([this]() {
    inPostShutdownDrain_ = true;
    drainPendingFinalizers();
    delete this;
  });
}

void napi_env__::queuePendingFinalizer(
    napi_finalize cb,
    void *data,
    void *hint) {
  std::lock_guard<std::mutex> lock(pendingFinalizersMutex_);
  pendingFinalizers_.push_back({cb, data, hint});
}

void napi_env__::drainPendingFinalizers() {
  std::vector<PendingFinalizer> batch;
  {
    std::lock_guard<std::mutex> lock(pendingFinalizersMutex_);
    if (drainingFinalizers_ || pendingFinalizers_.empty())
      return;
    drainingFinalizers_ = true;
    batch.swap(pendingFinalizers_);
  }

  // Process in batches: swap out the current queue so callbacks that
  // trigger further GC (adding new entries) don't invalidate our
  // iteration. Loop until the queue is fully drained.
  while (true) {
    napi_handle_scope scope = nullptr;
    napi_open_handle_scope(this, &scope);
    for (auto &pf : batch) {
      pf.cb(this, pf.data, pf.hint);
    }
    napi_close_handle_scope(this, scope);

    std::lock_guard<std::mutex> lock(pendingFinalizersMutex_);
    if (pendingFinalizers_.empty()) {
      drainingFinalizers_ = false;
      break;
    }
    batch.clear();
    batch.swap(pendingFinalizers_);
  }
}

void napi_env__::shutdown() {
  // Drain anything queued during normal operation.
  drainPendingFinalizers();

  // Run cleanup hooks in LIFO (reverse registration) order.
  // Hooks may remove themselves during execution, so iterate a copy.
  while (!cleanupHooks_.empty()) {
    auto hook = cleanupHooks_.back();
    cleanupHooks_.pop_back();
    hook.first(hook.second);
  }

  // Run async cleanup hooks in LIFO order. Since Hermes has no event
  // loop, these are called synchronously. The hook receives the handle
  // (so it can call napi_remove_async_cleanup_hook to signal
  // completion). We also provide a no-op "done" callback.
  while (!asyncCleanupHooks_.empty()) {
    auto *handle = asyncCleanupHooks_.back();
    asyncCleanupHooks_.pop_back();
    handle->hook(handle, handle->arg);
    delete handle;
  }

  // Clean up all remaining persistent references.
  // This must happen before instance data finalization because wrap
  // finalizers may call napi_get_instance_data().
  // Open a handle scope because finalizer callbacks may call NAPI
  // functions that create values.
  napi_handle_scope teardownScope = nullptr;
  napi_open_handle_scope(this, &teardownScope);
  while (refListHead_) {
    napi_ref__ *ref = refListHead_;
    refListHead_ = ref->next_;
    // Detach from the list so removeReference is safe if the
    // finalizer calls napi_delete_reference on other refs.
    if (ref->next_) {
      ref->next_->prev_ = nullptr;
    }
    ref->prev_ = nullptr;
    ref->next_ = nullptr;
    // Mark as pending so napi_delete_reference on THIS ref is a no-op.
    ref->deletionPending_ = true;
    if (ref->finalize_cb) {
      ref->finalize_cb(this, ref->finalize_data, ref->finalize_hint);
    }
    ref->releaseWeakSlot();
    delete ref;
  }
  napi_close_handle_scope(this, teardownScope);

  // Call the instance data finalizer, if set. This runs after
  // persistent reference finalizers because wrap finalizers may
  // depend on instance data.
  if (instanceDataFinalizeCb_) {
    instanceDataFinalizeCb_(this, instanceData_, instanceDataFinalizeHint_);
    instanceDataFinalizeCb_ = nullptr;
    instanceData_ = nullptr;
    instanceDataFinalizeHint_ = nullptr;
  }
  // Clean up any remaining active deferreds.
  while (deferredListHead_) {
    napi_deferred__ *def = deferredListHead_;
    deferredListHead_ = def->next_;
    delete def;
  }
  // Clean up any remaining thread-safe functions. Each refed tsfn
  // decrements activeTsfnLoopRefs_ via releaseTsfnLoopRef in cleanup,
  // which fires host->unref_loop on the 1→0 transition — so the host's
  // loop ref is released naturally as part of tsfn cleanup.
  // hermes_napi_cleanup_tsfns is defined in hermes_napi_tsfn.cpp.
  extern void hermes_napi_cleanup_tsfns(napi_env env);
  hermes_napi_cleanup_tsfns(this);
  assert(
      activeTsfnLoopRefs_ == 0 && "tsfn loop ref count nonzero after cleanup");

  // Final drain. User code in the cleanup hooks, ref finalizers,
  // instance data finalizer, and TSFN cleanup above may have
  // transitively triggered GC, queueing wrap callbacks. Drain
  // them here so they run with full env access while the heap is
  // still functional. drainPendingFinalizers loops internally to
  // handle cascading queue additions. Anything queued after this
  // point comes from getHeap().finalizeAll() and is handled by
  // the post-shutdown deleter under inPostShutdownDrain_.
  drainPendingFinalizers();
}

napi_env__::~napi_env__() {
  // Substantive teardown ran via shutdown() and the post-shutdown
  // deleter — both have already executed by the time we get here.
  // Member destructors release the remaining heap-allocated
  // structures.
}

napi_value napi_env__::addToCurrentScope(hermes::vm::HermesValue val) {
  assert(
      !scopeStack_.empty() &&
      "addToCurrentScope called with no open handle scope");

  // If the current block is full, move to the next one (allocating if
  // necessary).
  if (currentSlotIndex_ >= HandleBlock::kSize) {
    ++currentBlockIndex_;
    currentSlotIndex_ = 0;
    if (currentBlockIndex_ >= blocks_.size()) {
      blocks_.push_back(std::make_unique<HandleBlock>());
    }
  }

  // Store the value in the current slot and return a pointer to it.
  // PinnedHermesValue lives outside the GC heap, so we use operator=
  // which calls setNoBarrier() internally — no write barrier needed.
  hermes::vm::PinnedHermesValue *slot =
      &blocks_[currentBlockIndex_]->slots[currentSlotIndex_];
  *slot = val;
  ++currentSlotIndex_;
  return reinterpret_cast<napi_value>(slot);
}

void napi_env__::acquireTsfnLoopRef() {
  ++activeTsfnLoopRefs_;
  // Call host->ref_loop only on the 0 → 1 transition: tsfns coalesce
  // into a single env-level loop ref so the host sees one ref per env
  // regardless of how many tsfns are alive. This is the contract
  // documented on hermes_napi_host::ref_loop.
  if (activeTsfnLoopRefs_ == 1 && host_->ref_loop != nullptr) {
    host_->ref_loop(host_->data);
  }
}

void napi_env__::releaseTsfnLoopRef() {
  assert(activeTsfnLoopRefs_ > 0 && "underflow in tsfn loop ref count");
  --activeTsfnLoopRefs_;
  if (activeTsfnLoopRefs_ == 0 && host_->unref_loop != nullptr) {
    host_->unref_loop(host_->data);
  }
}

void napi_env__::markHandleScopes(hermes::vm::RootAcceptor &acceptor) {
  if (scopeStack_.empty()) {
    return;
  }

  // Mark all slots from block 0, slot 0 up to (but not including)
  // currentBlockIndex_/currentSlotIndex_.
  for (size_t bi = 0; bi < currentBlockIndex_; ++bi) {
    HandleBlock &block = *blocks_[bi];
    for (size_t si = 0; si < HandleBlock::kSize; ++si) {
      acceptor.accept(block.slots[si]);
    }
  }
  // Mark used slots in the current (partially filled) block.
  HandleBlock &curBlock = *blocks_[currentBlockIndex_];
  for (size_t si = 0; si < currentSlotIndex_; ++si) {
    acceptor.accept(curBlock.slots[si]);
  }
}

//===========================================================================
// Environment lifecycle
//===========================================================================

napi_env hermes_napi_create_env(void *hermes_runtime, hermes_napi_host *host) {
  // The env registers shutdown and post-shutdown deleter callbacks
  // on the Runtime in its constructor, so the Runtime takes
  // ownership and will tear it down and free it at the right phase
  // of ~Runtime. The returned pointer is borrowed by the caller.
  return new napi_env__(
      *static_cast<hermes::vm::Runtime *>(hermes_runtime), host);
}

//===========================================================================
// Error handling
//===========================================================================

napi_status NAPI_CDECL napi_get_last_error_info(
    node_api_basic_env env,
    const napi_extended_error_info **result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  // The value of the constant below must be updated to reference the
  // last message in the napi_status enum each time a new error message
  // is added.
  const int lastStatus = napi_cannot_run_js;

  static_assert(
      sizeof(errorMessages) / sizeof(errorMessages[0]) ==
          static_cast<size_t>(lastStatus) + 1,
      "Count of error messages must match count of error values");

  // Populate the error message string lazily, only when someone
  // requests the error info.
  if (env->last_error.error_code <= lastStatus) {
    env->last_error.error_message = errorMessages[env->last_error.error_code];
  } else {
    env->last_error.error_message = "Unknown error code";
  }

  if (env->last_error.error_code == napi_ok) {
    napi_clear_last_error(env);
  }
  *result = &(env->last_error);
  return napi_ok;
}

//===========================================================================
// Version management
//===========================================================================

napi_status NAPI_CDECL
napi_get_version(node_api_basic_env env, uint32_t *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result = NAPI_VERSION;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_node_version(
    node_api_basic_env env,
    const napi_node_version **result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  static const napi_node_version version = {
      HERMES_VERSION_MAJOR,
      HERMES_VERSION_MINOR,
      HERMES_VERSION_PATCH,
      "hermes"};
  *result = &version;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
node_api_get_module_file_name(node_api_basic_env env, const char **result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result = env->moduleFileName_.c_str();
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_adjust_external_memory(
    node_api_basic_env env,
    int64_t change_in_bytes,
    int64_t *adjusted_value) {
  CHECK_ENV(env);
  CHECK_ARG(env, adjusted_value);

  // Hermes GC external memory APIs (creditExternalMemory/
  // debitExternalMemory) require a specific GCCell, so a global hint
  // without a cell cannot be forwarded. Track the counter for
  // reporting purposes only, clamping to zero if it would go negative.
  env->externalMemory_ += change_in_bytes;
  if (env->externalMemory_ < 0) {
    env->externalMemory_ = 0;
  }
  *adjusted_value = env->externalMemory_;
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_uv_event_loop(node_api_basic_env env, struct uv_loop_s **loop) {
  CHECK_ENV(env);
  CHECK_ARG(env, loop);

  if (env->host_ && env->host_->uv_loop) {
    *loop = env->host_->uv_loop;
    return napi_clear_last_error(env);
  }

  // No host or no uv_loop provided. Return napi_generic_failure as
  // documented — many non-Node runtimes don't support this.
  return napi_set_last_error(env, napi_generic_failure);
}

//===========================================================================
// Instance data
//===========================================================================

napi_status NAPI_CDECL napi_set_instance_data(
    node_api_basic_env env,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint) {
  CHECK_ENV(env);

  // If there is existing instance data with a finalizer, call it first.
  if (env->instanceDataFinalizeCb_) {
    env->instanceDataFinalizeCb_(
        env, env->instanceData_, env->instanceDataFinalizeHint_);
  }

  env->instanceData_ = data;
  env->instanceDataFinalizeCb_ = finalize_cb;
  env->instanceDataFinalizeHint_ = finalize_hint;

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_get_instance_data(node_api_basic_env env, void **data) {
  CHECK_ENV(env);
  CHECK_ARG(env, data);

  *data = env->instanceData_;

  return napi_clear_last_error(env);
}

//===========================================================================
// Cleanup hooks
//===========================================================================

napi_status NAPI_CDECL napi_add_env_cleanup_hook(
    node_api_basic_env env,
    napi_cleanup_hook fun,
    void *arg) {
  CHECK_ENV(env);
  CHECK_ARG(env, fun);

  // Check if this {fun, arg} pair is already registered. If so, no-op.
  auto &hooks = env->cleanupHooks_;
  for (const auto &entry : hooks) {
    if (entry.first == fun && entry.second == arg) {
      return napi_clear_last_error(env);
    }
  }

  hooks.emplace_back(fun, arg);
  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_remove_env_cleanup_hook(
    node_api_basic_env env,
    napi_cleanup_hook fun,
    void *arg) {
  CHECK_ENV(env);
  CHECK_ARG(env, fun);

  // Find and remove the {fun, arg} pair. If not found, no-op.
  auto &hooks = env->cleanupHooks_;
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    if (it->first == fun && it->second == arg) {
      hooks.erase(it);
      break;
    }
  }

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_add_async_cleanup_hook(
    node_api_basic_env env,
    napi_async_cleanup_hook hook,
    void *arg,
    napi_async_cleanup_hook_handle *remove_handle) {
  CHECK_ENV(env);
  CHECK_ARG(env, hook);

  auto *handle = new napi_async_cleanup_hook_handle__{
      const_cast<napi_env>(env), hook, arg};
  env->asyncCleanupHooks_.push_back(handle);

  if (remove_handle != nullptr)
    *remove_handle = handle;

  return napi_clear_last_error(env);
}

napi_status NAPI_CDECL
napi_remove_async_cleanup_hook(napi_async_cleanup_hook_handle remove_handle) {
  if (remove_handle == nullptr)
    return napi_invalid_arg;

  // Remove from the env's list. If found, erase and delete. If not
  // found (e.g., already removed by the env destructor during
  // teardown), do nothing — the caller (env destructor) owns the
  // handle.
  auto &hooks = remove_handle->env->asyncCleanupHooks_;
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    if (*it == remove_handle) {
      hooks.erase(it);
      delete remove_handle;
      return napi_ok;
    }
  }

  // Handle was not in the list (already popped by destructor).
  // Don't delete — the destructor will delete it after the hook returns.
  return napi_ok;
}

//===========================================================================
// Script evaluation
//===========================================================================

napi_status NAPI_CDECL
napi_run_script(napi_env env, napi_value script, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, script);
  CHECK_ARG(env, result);

  using namespace hermes::vm;

  auto *phv = reinterpret_cast<PinnedHermesValue *>(script);
  RETURN_STATUS_IF_FALSE(env, phv->isString(), napi_string_expected);

  auto *str = vmcast<StringPrimitive>(*phv);

  // Extract the script source as UTF-8.
  auto *owned = new std::string();
  if (str->isASCII()) {
    auto ref = str->getStringRef<char>();
    owned->assign(ref.begin(), ref.end());
  } else {
    auto ref = str->getStringRef<char16_t>();
    hermes::convertUTF16ToUTF8WithReplacements(*owned, ref);
  }

  // Delegate to hermes_run_script. The size includes the null
  // terminator so hermes_run_script can use the buffer zero-copy.
  return hermes_run_script(
      env,
      reinterpret_cast<const uint8_t *>(owned->c_str()),
      owned->size() + 1,
      [](const uint8_t *, size_t, void *hint) {
        delete static_cast<std::string *>(hint);
      },
      owned,
      nullptr,
      nullptr,
      result);
}

/// A zero-terminated source buffer with a finalize callback.
///
/// This class exists to bridge a mismatch between two conventions:
///
/// - The caller of hermes_run_script() passes a buffer of \p size bytes
///   where the last byte is '\0'. The finalize callback expects to
///   receive the full \p size (including the null terminator).
///
/// - LLVM's Buffer convention (used by createBCProviderFromSrc) requires
///   that size() returns the length of the *content* excluding the null
///   terminator, with the invariant that data()[size()] == '\0'.
///
/// So we store size_ = allocSize - 1 (the content length) in the base
/// class, which satisfies createBCProviderFromSrc, and recover the
/// original allocation size as size_ + 1 when calling the finalize
/// callback.
namespace {
class WeirdZeroTerminatedBuffer : public hermes::Buffer {
 public:
  using FinalizeCb = void (*)(const uint8_t *data, size_t size, void *hint);

  /// \p allocSize is the full buffer size including the null terminator.
  WeirdZeroTerminatedBuffer(
      const uint8_t *data,
      size_t allocSize,
      FinalizeCb cb,
      void *hint)
      : Buffer(data, allocSize - 1), cb_(cb), hint_(hint) {
    assert(allocSize >= 1 && "allocSize must include the null terminator");
    assert(data[allocSize - 1] == '\0' && "buffer must be null-terminated");
  }

  ~WeirdZeroTerminatedBuffer() override {
    if (cb_)
      cb_(data_, size_ + 1, hint_);
  }

 private:
  FinalizeCb cb_;
  void *hint_;
};
} // namespace

napi_status hermes_run_script(
    napi_env env,
    const uint8_t *data,
    size_t size,
    void (*finalize_cb)(const uint8_t *data, size_t size, void *hint),
    void *finalize_hint,
    const char *source_url,
    const hermes_run_script_flags *flags,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, data);
  CHECK_ARG(env, result);

  using namespace hermes::vm;
  Runtime &runtime = env->runtime;

  // createBCProviderFromSrc requires buffer.data()[buffer.size()] == '\0'.
  // Build a buffer that satisfies this and carries the finalize callback.
  std::unique_ptr<hermes::Buffer> buffer;
  if (size > 0 && data[size - 1] == '\0') {
    // Null terminator included — zero-copy.
    buffer = std::make_unique<WeirdZeroTerminatedBuffer>(
        data, size, finalize_cb, finalize_hint);
  } else {
    // No null terminator — copy into a std::string (which guarantees
    // null termination), then release the original buffer.
    std::string copy(reinterpret_cast<const char *>(data), size);
    if (finalize_cb)
      finalize_cb(data, size, finalize_hint);
    buffer = std::make_unique<hermes::StdStringBuffer>(std::move(copy));
  }

  // Set up compile flags.
  hermes::hbc::CompileFlags compileFlags;
  compileFlags.format = hermes::Execute;
  compileFlags.includeLibHermes = false;
  compileFlags.enableGenerator = true;
  compileFlags.enableES6BlockScoping = true;
  compileFlags.enableAsyncGenerators = true;
#ifdef HERMES_ENABLE_DEBUGGER
  // Include debug info so the debugger can set breakpoints and pause.
  compileFlags.debug = true;
#endif
  if (flags && flags->struct_size >= sizeof(hermes_run_script_flags)) {
    compileFlags.strict = flags->strict;
    compileFlags.enableTS = flags->enable_ts;
  }

  // Compile source to bytecode.
  llvh::StringRef sourceURL = source_url ? source_url : "";
  auto bytecodeErr = hermes::hbc::createBCProviderFromSrc(
      std::move(buffer), sourceURL, {}, compileFlags);
  if (!bytecodeErr.first) {
    (void)runtime.raiseSyntaxError(TwineChar16(bytecodeErr.second));
    return captureRuntimeException(env, napi_pending_exception);
  }

  // Set up runtime module flags. persistent defaults to false so
  // compiled scripts don't leak into persistentBCProviders_.
  RuntimeModuleFlags rmflags{};
  if (flags && flags->struct_size >= sizeof(hermes_run_script_flags)) {
    rmflags.persistent = flags->persistent;
  }

  // Run the compiled bytecode.
  auto runRes = runtime.runBytecode(
      std::move(bytecodeErr.first),
      rmflags,
      sourceURL,
      Runtime::makeNullHandle<Environment>());
  if (LLVM_UNLIKELY(runRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = env->addToCurrentScope(*runRes);
  return napi_clear_last_error(env);
}

//===========================================================================
// Pre-compiled bytecode
//===========================================================================

napi_status hermes_run_bytecode(
    napi_env env,
    const uint8_t *data,
    size_t size,
    void (*finalize_cb)(const uint8_t *data, size_t size, void *hint),
    void *finalize_hint,
    const char *source_url,
    const hermes_bytecode_flags *flags,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, data);
  CHECK_ARG(env, result);

  using namespace hermes::vm;
  Runtime &runtime = env->runtime;

  // Wrap raw data in an owning Buffer.
  auto buffer = std::make_unique<hermes::CallbackBuffer>(
      data, size, finalize_cb, finalize_hint);

  // Parse bytecode.
  auto bcResult = hermes::hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
      std::move(buffer));
  if (!bcResult.first) {
    (void)runtime.raiseSyntaxError(TwineChar16(bcResult.second));
    return captureRuntimeException(env, napi_pending_exception);
  }

  // Read flags with ABI-safe struct_size handling.
  RuntimeModuleFlags rmFlags{};
  if (flags && flags->struct_size >= sizeof(hermes_bytecode_flags)) {
    rmFlags.persistent = flags->persistent;
    rmFlags.hidesEpilogue = flags->hides_epilogue;
    rmFlags.funcsAreBuiltins = flags->funcs_are_builtins;
  }

  // Run bytecode.
  auto runRes = runtime.runBytecode(
      std::move(bcResult.first),
      rmFlags,
      source_url ? source_url : "",
      Runtime::makeNullHandle<Environment>());
  if (LLVM_UNLIKELY(runRes == ExecutionStatus::EXCEPTION)) {
    return captureRuntimeException(env, napi_pending_exception);
  }

  *result = env->addToCurrentScope(*runRes);
  return napi_clear_last_error(env);
}

//===========================================================================
// Module registration
//===========================================================================

/// The last module registered via the deprecated napi_module_register().
/// This is stored for use by the module loader (step 8.2), which will
/// call the module's init function. In the symbol-based approach
/// (NAPI_MODULE_INIT), this is not used.
static const napi_module *lastRegisteredModule = nullptr;

void NAPI_CDECL napi_module_register(napi_module *mod) {
  // Store the module info for later use by the module loader.
  // The NAPI_MODULE_INIT macro-based approach bypasses this function
  // entirely, looking up napi_register_module_v1 by symbol instead.
  lastRegisteredModule = mod;
}

const napi_module *hermes_napi_get_last_registered_module() {
  return lastRegisteredModule;
}

//===========================================================================
// Module loading
//===========================================================================

napi_status
hermes_napi_load_module(napi_env env, const char *path, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, path);
  CHECK_ARG(env, result);

  using namespace hermes::vm;
  Runtime &runtime = env->runtime;

#ifdef _WIN32
  (void)runtime.raiseTypeError(
      TwineChar16("Loading native modules is not supported on Windows"));
  return captureRuntimeException(env, napi_pending_exception);
#else
  // Open the shared library. RTLD_LOCAL prevents symbol collisions
  // between different addons. RTLD_NOW ensures all symbols are resolved
  // immediately so we get a clear error if something is missing.
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char *err = dlerror();
    (void)runtime.raiseTypeError(
        TwineChar16("Failed to load module: ") + (err ? err : ""));
    return captureRuntimeException(env, napi_pending_exception);
  }

  // Look up the modern symbol-based entry point.
  using InitFunc = napi_value (*)(napi_env, napi_value);
  auto initFunc =
      reinterpret_cast<InitFunc>(dlsym(handle, "napi_register_module_v1"));

  // If the modern symbol isn't found, check the deprecated
  // napi_module_register() path. The shared library's static
  // constructors may have called napi_module_register() during
  // dlopen().
  const napi_module *mod = nullptr;
  if (!initFunc) {
    mod = hermes_napi_get_last_registered_module();
    if (mod && mod->nm_register_func) {
      initFunc = mod->nm_register_func;
    }
  }

  if (!initFunc) {
    dlclose(handle);
    (void)runtime.raiseTypeError(TwineChar16(
        "Module has no 'napi_register_module_v1' "
        "export and no registered napi_module"));
    return captureRuntimeException(env, napi_pending_exception);
  }

  // Record the module filename on the caller's env.
  env->moduleFileName_ = path;

  // Create the exports object (a plain JS object) in the caller's
  // handle scope.
  auto exports = JSObject::create(runtime);
  napi_value exportsValue = env->addToCurrentScope(exports.getHermesValue());

  // Call the module's init function with the caller's env.
  napi_value initResult = initFunc(env, exportsValue);

  // Check if the init function set a pending exception.
  if (env->hasPendingException) {
    return napi_set_last_error(env, napi_pending_exception);
  }

  // The init function may return a different object (e.g., a
  // function) as the module's exports. If it returns null, use the
  // original exports object.
  if (initResult != nullptr) {
    *result = initResult;
  } else {
    *result = exportsValue;
  }

  return napi_clear_last_error(env);
#endif
}
