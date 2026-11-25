/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_abi/hermes_vtable.h"

#include "hermes_abi/hermes_abi.h"

#include "hermes/ADT/ManagedChunkedList.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/Runtime.h"
#include "hermes_abi/HermesABIHelpers.h"

#include "llvh/Support/ConvertUTF.h"

using namespace hermes;
using namespace facebook::hermes;

namespace {

/// An implementation of hermes::Buffer that wraps a HermesABIBuffer.
class BufferWrapper : public hermes::Buffer {
  HermesABIBuffer *buffer_;

 public:
  explicit BufferWrapper(HermesABIBuffer *buffer)
      : hermes::Buffer(buffer->data, buffer->size), buffer_(buffer) {}

  ~BufferWrapper() override {
    buffer_->vtable->release(buffer_);
  }
};

/// A ManagedChunkedList element that indicates whether it's occupied based on
/// a refcount.
template <typename T>
class ManagedValue : public HermesABIManagedPointer {
  static void invalidate(HermesABIManagedPointer *ptr) {
    static_cast<ManagedValue<T> *>(ptr)->dec();
  }
  static constexpr HermesABIManagedPointerVTable vt{invalidate};

 public:
  ManagedValue() : HermesABIManagedPointer{&vt}, refCount_(0) {}

  /// Determine whether the element is occupied by inspecting the refcount.
  bool isFree() const {
    // In general, it is safe to use relaxed operations here because there
    // will be a control dependency between this load and any store that needs
    // to be ordered. However, TSAN does not analyse control dependencies,
    // since they are technically not part of C++, so use acquire under TSAN
    // to enforce the ordering of subsequent writes resulting from deleting a
    // freed element. (see the comment on dec() for more information)

#if LLVM_THREAD_SANITIZER_BUILD
    return refCount_.load(std::memory_order_acquire) == 0;
#else
    return refCount_.load(std::memory_order_relaxed) == 0;
#endif
  }

  /// Store a value and start the refcount at 1. After invocation, this
  /// instance is occupied with a value, and the "nextFree" methods should
  /// not be used until the value is released.
  template <typename... Args>
  void emplace(Args &&...args) {
    assert(isFree() && "Emplacing already occupied value");
    refCount_.store(1, std::memory_order_relaxed);
    new (&value_) T(std::forward<Args>(args)...);
  }

  /// Get the next free element. Must not be called when this instance is
  /// occupied with a value.
  ManagedValue<T> *getNextFree() {
    assert(isFree() && "Free pointer unusable while occupied");
    return nextFree_;
  }

  /// Set the next free element. Must not be called when this instance is
  /// occupied with a value.
  void setNextFree(ManagedValue<T> *nextFree) {
    assert(isFree() && "Free pointer unusable while occupied");
    nextFree_ = nextFree;
  }

  T &value() {
    assert(!isFree() && "Value not present");
    return value_;
  }

  const T &value() const {
    assert(!isFree() && "Value not present");
    return value_;
  }

  void inc() {
    // It is always safe to use relaxed operations for incrementing the
    // reference count, because the only operation that may occur concurrently
    // with it is decrementing the reference count, and we do not need to
    // enforce any ordering between the two.
    auto oldCount = refCount_.fetch_add(1, std::memory_order_relaxed);
    assert(oldCount && "Cannot resurrect a pointer");
    assert(oldCount + 1 != 0 && "Ref count overflow");
    (void)oldCount;
  }

  void dec() {
    // It is safe to use relaxed operations here because decrementing the
    // reference count is the only access that may be performed without proper
    // synchronisation. As a result, the only ordering we need to enforce when
    // decrementing is that the vtable pointer used to call \c invalidate is
    // loaded from before the decrement, in case the decrement ends up causing
    // this value to be freed. We get this ordering from the fact that the
    // vtable read and the reference count update form a load-store control
    // dependency, which preserves their ordering on any reasonable hardware.
    // TSAN does not analyse control dependencies, so we use release under
    // TSAN to enforce the ordering of the preceding vtable load.

#if LLVM_THREAD_SANITIZER_BUILD
    auto oldCount = refCount_.fetch_sub(1, std::memory_order_release);
#else
    auto oldCount = refCount_.fetch_sub(1, std::memory_order_relaxed);
#endif
    assert(oldCount > 0 && "Ref count underflow");
    (void)oldCount;
  }

 private:
  std::atomic<uint32_t> refCount_;
  union {
    T value_;
    ManagedValue<T> *nextFree_;
  };
};

/// Helper functions to create a handle from a HermesABI reference.
template <typename T = vm::HermesValue>
vm::Handle<T> toHandle(HermesABIManagedPointer *value) {
  return vm::Handle<T>::vmcast(
      &static_cast<ManagedValue<vm::PinnedHermesValue> *>(value)->value());
}
vm::Handle<vm::JSObject> toHandle(HermesABIObject obj) {
  return toHandle<vm::JSObject>(obj.pointer);
}
vm::Handle<vm::StringPrimitive> toHandle(HermesABIString str) {
  return toHandle<vm::StringPrimitive>(str.pointer);
}
vm::Handle<vm::SymbolID> toHandle(HermesABISymbol sym) {
  return toHandle<vm::SymbolID>(sym.pointer);
}
vm::Handle<vm::SymbolID> toHandle(HermesABIPropNameID sym) {
  return toHandle<vm::SymbolID>(sym.pointer);
}
vm::Handle<vm::BigIntPrimitive> toHandle(HermesABIBigInt bi) {
  return toHandle<vm::BigIntPrimitive>(bi.pointer);
}
vm::Handle<vm::Callable> toHandle(HermesABIFunction fn) {
  return toHandle<vm::Callable>(fn.pointer);
}
vm::Handle<vm::JSArrayBuffer> toHandle(HermesABIArrayBuffer ab) {
  return toHandle<vm::JSArrayBuffer>(ab.pointer);
}

/// Helper function to convert a HermesABIValue to a HermesValue.
vm::HermesValue toHermesValue(const HermesABIValue &val) {
  switch (abi::getValueKind(val)) {
    case HermesABIValueKindUndefined:
      return vm::HermesValue::encodeUndefinedValue();
    case HermesABIValueKindNull:
      return vm::HermesValue::encodeNullValue();
    case HermesABIValueKindBoolean:
      return vm::HermesValue::encodeBoolValue(abi::getBoolValue(val));
    case HermesABIValueKindNumber:
      return vm::HermesValue::encodeUntrustedNumberValue(
          abi::getNumberValue(val));
    case HermesABIValueKindString:
    case HermesABIValueKindObject:
    case HermesABIValueKindSymbol:
    case HermesABIValueKindBigInt:
      return *toHandle<>(val.data.pointer);
    default:
      // This incoming value is either an error, or from a newer version of the
      // ABI, which violates our expectations.
      hermes_fatal("Value has an unexpected tag.");
  }
}

/// A thin wrapper around vm::Runtime to provide additional state for things
/// like pointer management. It is intended to provide a small number of helper
/// functions, with the core logic being kept in the actual API functions below,
/// which can directly manipulate the vm::Runtime.
class HermesABIRuntimeImpl : public HermesABIRuntime {
  static const HermesABIRuntimeVTable vtable;

 public:
  std::shared_ptr<::hermes::vm::Runtime> rt;
  ManagedChunkedList<ManagedValue<vm::PinnedHermesValue>> hermesValues;
  ManagedChunkedList<ManagedValue<vm::WeakRoot<vm::JSObject>>> weakHermesValues;

  /// This holds the message for cases where we throw a native exception.
  std::string nativeExceptionMessage{};

  /// Compiler flags to use when evaluating source.
  hbc::CompileFlags compileFlags{};

  explicit HermesABIRuntimeImpl(const hermes::vm::RuntimeConfig &runtimeConfig)
      : HermesABIRuntime{&vtable},
        rt(hermes::vm::Runtime::create(runtimeConfig)),
        hermesValues(runtimeConfig.getGCConfig().getOccupancyTarget(), 0.5),
        weakHermesValues(
            runtimeConfig.getGCConfig().getOccupancyTarget(),
            0.5) {
    compileFlags.emitAsyncBreakCheck = runtimeConfig.getAsyncBreakCheckInEval();
    // Add custom roots functions to the runtime to expose references retained
    // through the API as roots.
    rt->addCustomRootsFunction([this](vm::GC *, vm::RootAcceptor &acceptor) {
      hermesValues.forEach(
          [&acceptor](auto &element) { acceptor.accept(element.value()); });
    });
    rt->addCustomWeakRootsFunction(
        [this](vm::GC *, vm::WeakRootAcceptor &acceptor) {
          weakHermesValues.forEach([&acceptor](auto &element) {
            acceptor.acceptWeak(element.value());
          });
        });
  }

  ~HermesABIRuntimeImpl() {
    // Release the runtime to make sure that any remaining references held by
    // things like HostObject are freed.
    rt.reset();
    assert(hermesValues.sizeForTests() == 0 && "Dangling references.");
    assert(weakHermesValues.sizeForTests() == 0 && "Dangling references.");
  }

  /// Create a HermesABIValue from a HermesValue, including creating root
  /// entries for pointers if necessary.
  HermesABIValue createValue(vm::HermesValue hv) {
    switch (hv.getETag()) {
      case vm::HermesValue::ETag::Undefined:
        return abi::createUndefinedValue();
      case vm::HermesValue::ETag::Null:
        return abi::createNullValue();
      case vm::HermesValue::ETag::Bool:
        return abi::createBoolValue(hv.getBool());
      case vm::HermesValue::ETag::Symbol:
        return abi::createSymbolValue(&hermesValues.add(hv));
      case vm::HermesValue::ETag::Str1:
      case vm::HermesValue::ETag::Str2:
        return abi::createStringValue(&hermesValues.add(hv));
      case vm::HermesValue::ETag::BigInt1:
      case vm::HermesValue::ETag::BigInt2:
        return abi::createBigIntValue(&hermesValues.add(hv));
      case vm::HermesValue::ETag::Object1:
      case vm::HermesValue::ETag::Object2:
        return abi::createObjectValue(&hermesValues.add(hv));
      default:
        assert(hv.isNumber() && "No other types are permitted in the API.");
        return abi::createNumberValue(hv.getNumber());
    }
  }

  /// Convenience function to create a HermesABIValueOrError directly from a
  /// HermesValue.
  HermesABIValueOrError createValueOrError(vm::HermesValue hv) {
    return abi::createValueOrError(createValue(hv));
  }

  /// Create a HermesABIManagedPointer entry using the given HermesValue. This
  /// template should never be used directly, and is only useful so that we can
  /// define the pointer creation functions using a macro below.
  template <typename T>
  HermesABIManagedPointer *createPointerImpl(vm::HermesValue hv) {
    if constexpr (!std::is_same_v<T, HermesABIWeakObject>)
      return &hermesValues.add(hv);

    return &weakHermesValues.add(
        vm::WeakRoot<vm::JSObject>(vm::vmcast<vm::JSObject>(hv), *rt));
  }

  /// Define functions to create each of the Hermes ABI pointer types from a
  /// HermesValue.

#define DECLARE_HERMES_ABI_POINTER_HELPERS(name)                               \
  HermesABI##name create##name(vm::HermesValue hv) {                           \
    return abi::create##name(createPointerImpl<HermesABI##name>(hv));          \
  }                                                                            \
  HermesABI##name##OrError create##name##OrError(vm::HermesValue hv) {         \
    return abi::create##name##OrError(createPointerImpl<HermesABI##name>(hv)); \
  }
  HERMES_ABI_POINTER_TYPES(DECLARE_HERMES_ABI_POINTER_HELPERS)
#undef DECLARE_HERMES_ABI_POINTER_HELPERS

  /// Create a Handle<HermesValue> from a HermesABIValue. Note that the Handle
  /// will be cheaply constructed from the existing PinnedHermesValue, so the
  /// HermesABIValue must outlive the Handle.
  vm::Handle<> makeVMHandle(
      const HermesABIValue &val,
      vm::PinnedHermesValue *numberStorage) {
    switch (abi::getValueKind(val)) {
      case HermesABIValueKindUndefined:
        return vm::Runtime::getUndefinedValue();
      case HermesABIValueKindNull:
        return vm::Runtime::getNullValue();
      case HermesABIValueKindBoolean:
        return vm::Runtime::getBoolValue(abi::getBoolValue(val));
      case HermesABIValueKindNumber:
        *numberStorage = vm::HermesValue::encodeUntrustedNumberValue(
            abi::getNumberValue(val));
        return vm::Handle<>(numberStorage);
      case HermesABIValueKindString:
      case HermesABIValueKindObject:
      case HermesABIValueKindSymbol:
      case HermesABIValueKindBigInt:
        return toHandle<>(val.data.pointer);
      default:
        // This incoming value is either an error, or from a newer version of
        // the ABI, which violates our expectations.
        hermes_fatal("Value has an unexpected tag.");
    }
  }

  /// Convert the error associated with the given error code \p err into a VM
  /// exception.
  vm::ExecutionStatus raiseError(HermesABIErrorCode err) {
    if (err == HermesABIErrorCodeJSError)
      return vm::ExecutionStatus::EXCEPTION;

    if (err == HermesABIErrorCodeNativeException) {
      auto msg = std::exchange(nativeExceptionMessage, {});

      // Treat the error message as UTF-8 and convert it to UTF-16 before
      // passing it into the VM.
      llvh::SmallVector<llvh::UTF16, 8> u16msg;
      if (!llvh::convertUTF8ToUTF16String(msg, u16msg))
        return rt->raiseError("<invalid utf-8 exception message>");

      static_assert(
          sizeof(llvh::UTF16) == sizeof(char16_t),
          "Cannot safely cast UTF16 to char16_t.");
      return rt->raiseError(
          vm::UTF16Ref{(char16_t *)u16msg.data(), u16msg.size()});
    }

    return rt->raiseError("<unknown native exception>");
  }
};

/// Convenience function to cast the given HermesABIRuntime to a
/// HermesABIRuntimeImpl.
HermesABIRuntimeImpl *impl(HermesABIRuntime *abiRt) {
  return static_cast<HermesABIRuntimeImpl *>(abiRt);
}

/// Helper function to write the given StringRef \p ref to the buffer \p buf.
/// Terminates if the size of the buffer cannot be grown sufficiently to hold
/// the resulting string.
/// TODO: Revisit the termination behavior. It may be preferable to report an
/// error or truncate the string depending on the usage.
void writeToBuf(HermesABIGrowableBuffer *buf, llvh::StringRef ref) {
  // Grow the buffer if necessary.
  if (buf->size < ref.size())
    buf->vtable->try_grow_to(buf, ref.size());

  // In the unlikely case that we failed to allocate enough space for the
  // message, fatal.
  if (buf->size < ref.size())
    hermes_fatal("Failed to allocate buffer to return string");

  // Copy the message into the buffer and adjust the buffer's available space.
  memcpy(buf->data, ref.data(), ref.size());
  buf->used = ref.size();
}

HermesABIRuntime *make_hermes_runtime(const HermesABIRuntimeConfig *config) {
  return new HermesABIRuntimeImpl({});
}

bool is_hermes_bytecode(const uint8_t *data, size_t len) {
  return hbc::BCProviderFromBuffer::isBytecodeStream(
      llvh::ArrayRef<uint8_t>(data, len));
}

void release_hermes_runtime(HermesABIRuntime *abiRt) {
  delete impl(abiRt);
}

HermesABIValue get_and_clear_js_error_value(HermesABIRuntime *abiRt) {
  auto *hart = impl(abiRt);
  auto thrownValue = hart->rt->getThrownValue();

  // In debug builds, assert if there is no currently thrown value. In release
  // builds, return undefined so the behaviour is reasonable.
  assert(!thrownValue.isEmpty() && "Retrieving a non-existent error.");

  auto ret = thrownValue.isEmpty() ? abi::createUndefinedValue()
                                   : hart->createValue(thrownValue);
  hart->rt->clearThrownValue();
  return ret;
}

void get_and_clear_native_exception_message(
    HermesABIRuntime *abiRt,
    HermesABIGrowableBuffer *buf) {
  auto *hart = impl(abiRt);
  writeToBuf(buf, hart->nativeExceptionMessage);
  hart->nativeExceptionMessage.clear();
  hart->nativeExceptionMessage.shrink_to_fit();
}

void set_js_error_value(HermesABIRuntime *abiRt, const HermesABIValue *val) {
  impl(abiRt)->rt->setThrownValue(toHermesValue(*val));
}
void set_native_exception_message(
    HermesABIRuntime *abiRt,
    const uint8_t *message,
    size_t length) {
  impl(abiRt)->nativeExceptionMessage.assign((const char *)message, length);
}

HermesABIPropNameID clone_propnameid(
    HermesABIRuntime *,
    HermesABIPropNameID name) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(name.pointer)->inc();
  return name;
}
HermesABIString clone_string(HermesABIRuntime *, HermesABIString str) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(str.pointer)->inc();
  return str;
}
HermesABISymbol clone_symbol(HermesABIRuntime *, HermesABISymbol sym) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(sym.pointer)->inc();
  return sym;
}
HermesABIObject clone_object(HermesABIRuntime *, HermesABIObject obj) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(obj.pointer)->inc();
  return obj;
}
HermesABIBigInt clone_bigint(HermesABIRuntime *, HermesABIBigInt bi) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(bi.pointer)->inc();
  return bi;
}

HermesABIValueOrError runBCProvider(
    HermesABIRuntimeImpl *hart,
    std::unique_ptr<hbc::BCProvider> provider,
    llvh::StringRef sourceURL) {
  auto &runtime = *hart->rt;
  vm::RuntimeModuleFlags runtimeFlags{};
  auto res = runtime.runBytecode(
      std::move(provider),
      runtimeFlags,
      sourceURL,
      vm::Runtime::makeNullHandle<vm::Environment>());
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);

  return hart->createValueOrError(*res);
}

HermesABIValueOrError evaluate_javascript_source(
    HermesABIRuntime *abiRt,
    HermesABIBuffer *source,
    const char *sourceURL,
    size_t sourceURLLength) {
  auto *hart = impl(abiRt);
  llvh::StringRef sourceURLRef(sourceURL, sourceURLLength);
  auto bcErr = hbc::createBCProviderFromSrc(
      std::make_unique<BufferWrapper>(source),
      sourceURLRef,
      /* sourceMap */ {},
      /* compileFlags */ hart->compileFlags);
  if (!bcErr.first) {
    hart->nativeExceptionMessage = std::move(bcErr.second);
    return abi::createValueOrError(HermesABIErrorCodeNativeException);
  }

  return runBCProvider(hart, std::move(bcErr.first), sourceURLRef);
}

HermesABIValueOrError evaluate_hermes_bytecode(
    HermesABIRuntime *abiRt,
    HermesABIBuffer *bytecode,
    const char *sourceURL,
    size_t sourceURLLength) {
  auto *hart = impl(abiRt);
  assert(is_hermes_bytecode(bytecode->data, bytecode->size));
  auto bcErr = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
      std::make_unique<BufferWrapper>(bytecode));
  if (!bcErr.first) {
    hart->nativeExceptionMessage = std::move(bcErr.second);
    return abi::createValueOrError(HermesABIErrorCodeNativeException);
  }

  return runBCProvider(
      hart, std::move(bcErr.first), {sourceURL, sourceURLLength});
}

HermesABIObject get_global_object(HermesABIRuntime *abiRt) {
  auto *hart = impl(abiRt);
  return hart->createObject(hart->rt->getGlobal().getHermesValue());
}

HermesABIStringOrError create_string_from_utf8(
    HermesABIRuntime *abiRt,
    const uint8_t *utf8,
    size_t length) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto strRes = vm::StringPrimitive::createEfficient(
      runtime, llvh::makeArrayRef(utf8, length), /* IgnoreInputErrors */ true);
  if (strRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createStringOrError(HermesABIErrorCodeJSError);
  return hart->createStringOrError(*strRes);
}

HermesABIObjectOrError create_object(HermesABIRuntime *abiRt) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  return hart->createObjectOrError(
      vm::JSObject::create(runtime).getHermesValue());
}

HermesABIBoolOrError has_object_property_from_value(
    HermesABIRuntime *abiRt,
    HermesABIObject obj,
    const HermesABIValue *key) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;

  vm::PinnedHermesValue numberStorage;
  auto res = vm::JSObject::hasComputed(
      toHandle(obj), runtime, hart->makeVMHandle(*key, &numberStorage));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createBoolOrError(HermesABIErrorCodeJSError);
  return abi::createBoolOrError(*res);
}

HermesABIBoolOrError has_object_property_from_propnameid(
    HermesABIRuntime *abiRt,
    HermesABIObject obj,
    HermesABIPropNameID name) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto res =
      vm::JSObject::hasNamedOrIndexed(toHandle(obj), runtime, *toHandle(name));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createBoolOrError(HermesABIErrorCodeJSError);
  return abi::createBoolOrError(*res);
}

HermesABIValueOrError get_object_property_from_value(
    HermesABIRuntime *abiRt,
    HermesABIObject object,
    const HermesABIValue *key) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;

  vm::PinnedHermesValue numberStorage;
  auto res = vm::JSObject::getComputed_RJS(
      toHandle(object), runtime, hart->makeVMHandle(*key, &numberStorage));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  return hart->createValueOrError(res->get());
}

HermesABIValueOrError get_object_property_from_propnameid(
    HermesABIRuntime *abiRt,
    HermesABIObject object,
    HermesABIPropNameID sym) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto res = vm::JSObject::getNamedOrIndexed(
      toHandle(object), runtime, *toHandle(sym));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  return hart->createValueOrError(res->get());
}

HermesABIVoidOrError set_object_property_from_value(
    HermesABIRuntime *abiRt,
    HermesABIObject obj,
    const HermesABIValue *key,
    const HermesABIValue *val) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;

  vm::PinnedHermesValue numberStorageKey;
  vm::PinnedHermesValue numberStorageVal;
  auto res = vm::JSObject::putComputed_RJS(
                 toHandle(obj),
                 runtime,
                 hart->makeVMHandle(*key, &numberStorageKey),
                 hart->makeVMHandle(*val, &numberStorageVal),
                 vm::PropOpFlags().plusThrowOnError())
                 .getStatus();
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createVoidOrError(HermesABIErrorCodeJSError);
  return abi::createVoidOrError();
}

HermesABIVoidOrError set_object_property_from_propnameid(
    HermesABIRuntime *abiRt,
    HermesABIObject obj,
    HermesABIPropNameID name,
    const HermesABIValue *val) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;

  vm::PinnedHermesValue numberStorage;
  auto res = vm::JSObject::putNamedOrIndexed(
                 toHandle(obj),
                 runtime,
                 *toHandle(name),
                 hart->makeVMHandle(*val, &numberStorage),
                 vm::PropOpFlags().plusThrowOnError())
                 .getStatus();
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createVoidOrError(HermesABIErrorCodeJSError);
  return abi::createVoidOrError();
}

HermesABIArrayOrError get_object_property_names(
    HermesABIRuntime *abiRt,
    HermesABIObject obj) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  uint32_t beginIndex;
  uint32_t endIndex;
  auto objHandle = toHandle(obj);

  struct : public vm::Locals {
    vm::PinnedValue<vm::ArrayStorageSmall> props;
    vm::PinnedValue<vm::JSArray> ret;
    vm::PinnedValue<> nameHnd;
  } lv;
  vm::LocalsRAII lraii(runtime, &lv);

  auto propsRes =
      vm::getForInPropertyNames(runtime, objHandle, beginIndex, endIndex);
  if (propsRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createArrayOrError(HermesABIErrorCodeJSError);

  lv.props = std::move(*propsRes);
  size_t length = endIndex - beginIndex;

  auto retRes = vm::JSArray::create(runtime, length, length);
  if (retRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createArrayOrError(HermesABIErrorCodeJSError);

  lv.ret.castAndSetHermesValue<vm::JSArray>(retRes->getHermesValue());
  vm::JSArray::setStorageEndIndex(lv.ret, runtime, length);

  // Convert each property name to a string and store it in the result array.
  for (size_t i = 0; i < length; ++i) {
    vm::SmallHermesValue name = lv.props->at(beginIndex + i);
    if (name.isString()) {
      vm::JSArray::unsafeSetExistingElementAt(*lv.ret, runtime, i, name);
    } else if (name.isSymbol()) {
      // May allocate. 'name' must not be used afterwards.
      vm::StringPrimitive *asString =
          runtime.getStringPrimFromSymbolID(name.getSymbol());
      auto strName = vm::SmallHermesValue::encodeStringValue(asString, runtime);
      vm::JSArray::unsafeSetExistingElementAt(*lv.ret, runtime, i, strName);
    } else {
      assert(name.isNumber());
      lv.nameHnd =
          vm::HermesValue::encodeTrustedNumberValue(name.getNumber(runtime));
      auto asStrRes = vm::toString_RJS(runtime, lv.nameHnd);
      if (asStrRes == vm::ExecutionStatus::EXCEPTION)
        return abi::createArrayOrError(HermesABIErrorCodeJSError);
      auto strName =
          vm::SmallHermesValue::encodeStringValue(asStrRes->get(), runtime);
      vm::JSArray::unsafeSetExistingElementAt(*lv.ret, runtime, i, strName);
    }
  }

  return hart->createArrayOrError(lv.ret.getHermesValue());
}

HermesABIVoidOrError set_object_external_memory_pressure(
    HermesABIRuntime *abiRt,
    HermesABIObject obj,
    size_t amt) {
  auto *hart = impl(abiRt);
  auto &rt = *hart->rt;
  auto objHandle = toHandle(obj);
  if (objHandle->isProxyObject(rt)) {
    hart->nativeExceptionMessage = "Cannot set external memory on Proxy";
    return abi::createVoidOrError(HermesABIErrorCodeNativeException);
  }

  // Check if the internal property is already set. If so, we can update the
  // associated external memory in place.
  vm::NamedPropertyDescriptor desc;
  bool exists = vm::JSObject::getOwnNamedDescriptor(
      objHandle,
      rt,
      vm::Predefined::getSymbolID(
          vm::Predefined::InternalPropertyExternalMemoryPressure),
      desc);

  struct : public vm::Locals {
    vm::PinnedValue<vm::NativeState> nsHnd;
  } lv;
  vm::LocalsRAII lraii(rt, &lv);

  vm::NativeState *ns;
  if (exists) {
    ns = vm::vmcast<vm::NativeState>(
        vm::JSObject::getNamedSlotValueUnsafe(*objHandle, rt, desc)
            .getObject(rt));
  } else {
    // An object without the property implicitly has no external memory, so
    // setting it to 0 is a no-op.
    if (amt == 0)
      return abi::createVoidOrError();

    auto debitMem = [](vm::GC &gc, vm::NativeState *ns) {
      auto amt = reinterpret_cast<uintptr_t>(ns->context());
      gc.debitExternalMemory(ns, amt);
    };

    // This is the first time adding external memory to this object. Create a
    // new NativeState. We use the context pointer to store the external memory
    // amount.
    lv.nsHnd =
        vm::NativeState::create(rt, reinterpret_cast<void *>(0), debitMem);

    // Use defineNewOwnProperty to create the new property since we know it
    // doesn't exist. Note that this also bypasses the extensibility check on
    // the object.
    auto res = vm::JSObject::defineNewOwnProperty(
        objHandle,
        rt,
        vm::Predefined::getSymbolID(
            vm::Predefined::InternalPropertyExternalMemoryPressure),
        vm::PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.nsHnd);
    if (LLVM_UNLIKELY(res == vm::ExecutionStatus::EXCEPTION))
      return abi::createVoidOrError(HermesABIErrorCodeJSError);
    ns = *lv.nsHnd;
  }

  // Ensure that the raw pointer `ns` remains valid.
  vm::NoAllocScope noAllocs{rt};

  auto curAmt = reinterpret_cast<uintptr_t>(ns->context());
  assert(llvh::isUInt<32>(curAmt) && "Amount is too large.");

  // Nothing to do if the amount is unchanged.
  if (amt == curAmt)
    return abi::createVoidOrError();

  // The GC does not support adding more than a 32 bit amount.
  if (!llvh::isUInt<32>(amt)) {
    hart->nativeExceptionMessage = "Amount is too large";
    return abi::createVoidOrError(HermesABIErrorCodeNativeException);
  }

  // Try to credit or debit the delta depending on whether the new amount is
  // larger.
  if (amt > curAmt) {
    auto delta = amt - curAmt;
    if (!rt.getHeap().canAllocExternalMemory(delta)) {
      hart->nativeExceptionMessage = "External memory is too high";
      return abi::createVoidOrError(HermesABIErrorCodeNativeException);
    }
    rt.getHeap().creditExternalMemory(ns, delta);
  } else {
    rt.getHeap().debitExternalMemory(ns, curAmt - amt);
  }

  ns->setContext(reinterpret_cast<void *>(amt));
  return abi::createVoidOrError();
}

HermesABIArrayOrError create_array(HermesABIRuntime *abiRt, size_t length) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto result = vm::JSArray::create(runtime, length, length);
  if (result == vm::ExecutionStatus::EXCEPTION)
    return abi::createArrayOrError(HermesABIErrorCodeJSError);
  return hart->createArrayOrError(result->getHermesValue());
}

HermesABISizeTOrError get_array_length(
    HermesABIRuntime *abiRt,
    HermesABIArray arr) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto objHandle = toHandle<vm::JSObject>(arr.pointer);
  if (LLVM_LIKELY(vm::vmisa<vm::JSArray>(*objHandle))) {
    return abi::createSizeTOrError(
        vm::JSArray::getLength(vm::vmcast<vm::JSArray>(*objHandle), runtime));
  }

  // The array may be a Proxy, so read and convert its length property.
  struct : public vm::Locals {
    vm::PinnedValue<> lenProp;
  } lv;
  vm::LocalsRAII lraii(runtime, &lv);
  auto getRes = vm::JSObject::getNamed_RJS(
      objHandle, runtime, vm::Predefined::getSymbolID(vm::Predefined::length));
  if (LLVM_UNLIKELY(getRes == vm::ExecutionStatus::EXCEPTION))
    return abi::createSizeTOrError(HermesABIErrorCodeJSError);
  lv.lenProp = std::move(*getRes);
  auto lenRes = vm::toLength(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(lenRes == vm::ExecutionStatus::EXCEPTION))
    return abi::createSizeTOrError(HermesABIErrorCodeJSError);

  // The length may be up to 2^53 - 1, which does not fit in a 32-bit size_t.
  double len = lenRes->getNumber();
  if (LLVM_UNLIKELY(len > (double)std::numeric_limits<size_t>::max())) {
    hart->nativeExceptionMessage = "Array length does not fit in size_t.";
    return abi::createSizeTOrError(HermesABIErrorCodeNativeException);
  }
  return abi::createSizeTOrError(static_cast<size_t>(len));
}

HermesABIArrayBufferOrError create_arraybuffer_from_external_data(
    HermesABIRuntime *abiRt,
    HermesABIMutableBuffer *buf) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;

  struct : public vm::Locals {
    vm::PinnedValue<vm::JSArrayBuffer> arrayBuffer;
  } lv;
  vm::LocalsRAII lraii(runtime, &lv);

  lv.arrayBuffer = vm::JSArrayBuffer::create(
      runtime, vm::Handle<vm::JSObject>::vmcast(&runtime.arrayBufferPrototype));

  // Set up the ArrayBuffer such that the data refers to the provided buffer,
  // and the finalizer will invoke the release method.
  auto size = buf->size;
  auto *data = buf->data;
  vm::JSArrayBuffer::setExternalDataBlock(
      runtime,
      lv.arrayBuffer,
      data,
      size,
      std::shared_ptr<void>(buf, [](HermesABIMutableBuffer *buffer) {
        buffer->vtable->release(buffer);
      }));
  return hart->createArrayBufferOrError(lv.arrayBuffer.getHermesValue());
}

HermesABIUint8PtrOrError get_arraybuffer_data(
    HermesABIRuntime *abiRt,
    HermesABIArrayBuffer buf) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto ab = toHandle(buf);
  if (!ab->attached()) {
    hart->nativeExceptionMessage =
        "Cannot get data block of detached ArrayBuffer.";
    return abi::createUint8PtrOrError(HermesABIErrorCodeNativeException);
  }
  return abi::createUint8PtrOrError(ab->getDataBlock());
}

HermesABISizeTOrError get_arraybuffer_size(
    HermesABIRuntime *abiRt,
    HermesABIArrayBuffer buf) {
  auto *hart = impl(abiRt);
  auto ab = toHandle(buf);
  if (!ab->attached()) {
    hart->nativeExceptionMessage = "Cannot get size of detached ArrayBuffer.";
    return abi::createSizeTOrError(HermesABIErrorCodeNativeException);
  }
  return abi::createSizeTOrError(ab->size());
}

HermesABIPropNameIDOrError create_propnameid_from_string(
    HermesABIRuntime *abiRt,
    HermesABIString str) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  struct : public vm::Locals {
    vm::PinnedValue<vm::SymbolID> sym;
  } lv;
  vm::LocalsRAII lraii(runtime, &lv);
  if (vm::stringToSymbolID(
          runtime, vm::createPseudoHandle(*toHandle(str)), lv.sym) ==
      vm::ExecutionStatus::EXCEPTION)
    return abi::createPropNameIDOrError(HermesABIErrorCodeJSError);
  return hart->createPropNameIDOrError(lv.sym.getHermesValue());
}

HermesABIPropNameIDOrError create_propnameid_from_symbol(
    HermesABIRuntime *abiRt,
    HermesABISymbol sym) {
  return impl(abiRt)->createPropNameIDOrError(toHandle(sym).getHermesValue());
}

bool prop_name_id_equals(
    HermesABIRuntime *,
    HermesABIPropNameID a,
    HermesABIPropNameID b) {
  return *toHandle(a) == *toHandle(b);
}

HermesABIValueOrError call(
    HermesABIRuntime *abiRt,
    HermesABIFunction func,
    const HermesABIValue *jsThis,
    const HermesABIValue *args,
    size_t count) {
  auto *hart = impl(abiRt);

  // We have to cast down the count to uint32 before calling VM APIs.
  if (count > std::numeric_limits<uint32_t>::max()) {
    hart->nativeExceptionMessage = "Too many arguments to call";
    return abi::createValueOrError(HermesABIErrorCodeNativeException);
  }

  auto &runtime = *hart->rt;
  vm::Handle<vm::Callable> funcHandle = toHandle(func);

  // Set up the call frame and create space for the arguments.
  vm::ScopedNativeCallFrame newFrame{
      runtime,
      static_cast<uint32_t>(count),
      funcHandle.getHermesValue(),
      vm::HermesValue::encodeUndefinedValue(),
      toHermesValue(*jsThis)};
  if (LLVM_UNLIKELY(newFrame.overflowed())) {
    (void)runtime.raiseStackOverflow(
        ::hermes::vm::Runtime::StackOverflowKind::NativeStack);
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  }

  // Convert each argument from a HermesABIValue to a HermesValue and store it
  // in the frame.
  for (uint32_t i = 0; i != count; ++i)
    newFrame->getArgRef(i) = toHermesValue(args[i]);

  auto callRes = vm::Callable::call(funcHandle, runtime);
  if (callRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);

  return hart->createValueOrError(callRes->get());
}

HermesABIValueOrError call_as_constructor(
    HermesABIRuntime *abiRt,
    HermesABIFunction fn,
    const HermesABIValue *args,
    size_t count) {
  auto *hart = impl(abiRt);
  if (count > std::numeric_limits<uint32_t>::max()) {
    hart->nativeExceptionMessage = "Too many arguments to call";
    return abi::createValueOrError(HermesABIErrorCodeNativeException);
  }

  auto &runtime = *hart->rt;

  struct : public vm::Locals {
    vm::PinnedValue<> thisArg;
  } lv;
  vm::LocalsRAII lraii(runtime, &lv);

  vm::Handle<vm::Callable> funcHandle = toHandle(fn);

  // Create the new object for the constructor call and save it in case the
  // function does not return an object. This is undefined for functions that
  // create their own this, like native constructors.
  auto thisRes =
      vm::Callable::createThisForConstruct_RJS(funcHandle, runtime, funcHandle);
  if (LLVM_UNLIKELY(thisRes == vm::ExecutionStatus::EXCEPTION))
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  lv.thisArg = std::move(*thisRes);

  // Set up the call frame and create space for the arguments.
  vm::ScopedNativeCallFrame newFrame{
      runtime,
      static_cast<uint32_t>(count),
      funcHandle.getHermesValue(),
      funcHandle.getHermesValue(),
      lv.thisArg.getHermesValue()};
  if (LLVM_UNLIKELY(newFrame.overflowed())) {
    (void)runtime.raiseStackOverflow(
        ::hermes::vm::Runtime::StackOverflowKind::NativeStack);
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  }

  // Convert each argument from a HermesABIValue to a HermesValue and store it
  // in the frame.
  for (uint32_t i = 0; i != count; ++i)
    newFrame->getArgRef(i) = toHermesValue(args[i]);

  auto callRes = vm::Callable::call(funcHandle, runtime);
  if (callRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);

  // If the result is not an object, return the this parameter.
  auto res = callRes->get();
  return hart->createValueOrError(
      res.isObject() ? res : lv.thisArg.getHermesValue());
}

/// A wrapper that holds the HermesABIRuntime and HostFunction, to allow the
/// HostFunction to be conveniently exposed to the VM and destroyed when it is
/// no longer needed.
class HostFunctionWrapper {
  /// The HermesABIRuntime that created this HostFunction.
  HermesABIRuntimeImpl *hart_;

  /// The HostFunction that this wrapper manages.
  HermesABIHostFunction *func_;

 public:
  HostFunctionWrapper(HermesABIRuntimeImpl *hart, HermesABIHostFunction *func)
      : hart_(hart), func_(func) {}

  ~HostFunctionWrapper() {
    func_->vtable->release(func_);
  }

  HermesABIHostFunction *getFunc() {
    return func_;
  }

  static vm::CallResult<vm::HermesValue> call(
      void *hfCtx,
      vm::Runtime &runtime) {
    vm::NativeArgs hvArgs = runtime.getCurrentFrame().getNativeArgs();
    auto *self = static_cast<HostFunctionWrapper *>(hfCtx);
    auto *hart = self->hart_;
    assert(&runtime == hart->rt.get());

    // Convert the HermesValue arguments to HermesABIValues.
    llvh::SmallVector<HermesABIValue, 8> apiArgs;
    for (vm::HermesValue hv : hvArgs)
      apiArgs.push_back(hart->createValue(hv));

    HermesABIValue thisArg = hart->createValue(hvArgs.getThisArg());

    auto retOrError = (self->func_->vtable->call)(
        self->func_, hart, &thisArg, apiArgs.data(), apiArgs.size());

    // Release all of the values allocated for the arguments.
    for (const auto &arg : apiArgs)
      abi::releaseValue(arg);
    abi::releaseValue(thisArg);

    // Error values do not need to be "released" so we can return early.
    if (abi::isError(retOrError))
      return hart->raiseError(abi::getError(retOrError));

    // Convert the returned HermesABIValue to a HermesValue and release it.
    auto ret = abi::getValue(retOrError);
    auto retHV = toHermesValue(ret);
    abi::releaseValue(ret);
    return retHV;
  }
  static void release(void *data) {
    delete static_cast<HostFunctionWrapper *>(data);
  }
};

HermesABIFunctionOrError create_function_from_host_function(
    HermesABIRuntime *abiRt,
    HermesABIPropNameID name,
    unsigned int paramCount,
    HermesABIHostFunction *func) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto *hfw = new HostFunctionWrapper(hart, func);

  // Create a FinalizableNativeFunction that uses the HostFunctionWrapper as its
  // context for calling and finalization.
  auto funcRes = vm::FinalizableNativeFunction::createWithoutPrototype(
      runtime,
      hfw,
      HostFunctionWrapper::call,
      HostFunctionWrapper::release,
      *toHandle(name),
      paramCount);
  assert(
      funcRes != vm::ExecutionStatus::EXCEPTION &&
      "Failed to create HostFunction");
  return hart->createFunctionOrError(*funcRes);
}

HermesABIHostFunction *get_host_function(
    HermesABIRuntime *,
    HermesABIFunction fn) {
  if (auto h =
          vm::Handle<vm::FinalizableNativeFunction>::dyn_vmcast(toHandle(fn)))
    return static_cast<HostFunctionWrapper *>(h->getContext())->getFunc();
  return nullptr;
}

/// A wrapper around the ABI HostObject implementation that implements
/// vm::HostObjectProxy. This allows us to expose an ABI HostObject to the VM.
class HostObjectWrapper : public vm::HostObjectProxy {
  /// The HermesABIRuntime that created this HostObject. We save this so it can
  /// be passed into calls.
  HermesABIRuntimeImpl *hart_;

  /// The ABI HostObject that this wrapper manages.
  HermesABIHostObject *ho_;

 public:
  HostObjectWrapper(HermesABIRuntimeImpl *hart, HermesABIHostObject *ho)
      : hart_(hart), ho_(ho) {}

  ~HostObjectWrapper() override {
    ho_->vtable->release(ho_);
  }

  /// Get the managed ABI HostObject.
  HermesABIHostObject *getHostObject() {
    return ho_;
  }

  /// Get a property value with key \p sym from the VM by calling into the user
  /// implemented ABI HostObject.
  vm::CallResult<vm::HermesValue> get(vm::SymbolID sym) override {
    HermesABIPropNameID name =
        hart_->createPropNameID(vm::HermesValue::encodeSymbolValue(sym));
    auto retOrErr = ho_->vtable->get(ho_, hart_, name);
    abi::releasePointer(name.pointer);

    if (abi::isError(retOrErr))
      return hart_->raiseError(abi::getError(retOrErr));

    auto ret = abi::getValue(retOrErr);
    auto retHV = toHermesValue(ret);
    abi::releaseValue(ret);
    return retHV;
  }

  /// Set a property with key \p sym to \p value from the VM by calling into the
  /// user implemented ABI HostObject.
  vm::CallResult<bool> set(vm::SymbolID sym, vm::HermesValue value) override {
    HermesABIPropNameID name =
        hart_->createPropNameID(vm::HermesValue::encodeSymbolValue(sym));
    auto abiVal = hart_->createValue(value);
    auto ret = ho_->vtable->set(ho_, hart_, name, &abiVal);
    abi::releasePointer(name.pointer);
    abi::releaseValue(abiVal);
    if (abi::isError(ret))
      return hart_->raiseError(abi::getError(ret));
    return true;
  }

  /// Query the names of properties by calling out through the ABI to a user
  /// provided HostObject.
  /// \return an array of Symbols representing the properties.
  /// TODO(T172475694): The ABI formally allows the HostObject implementation to
  /// return symbols, but the VM implementation cannot handle them yet.
  vm::ExecutionStatus getHostPropertyNames(
      vm::MutableHandle<vm::JSArray> result) override {
    auto ret = ho_->vtable->get_own_keys(ho_, hart_);
    if (abi::isError(ret))
      return hart_->raiseError(abi::getError(ret));

    auto *abiNames = abi::getPropNameIDListPtr(ret);
    const HermesABIPropNameID *names = abiNames->props;
    size_t size = abiNames->size;
    auto &runtime = *hart_->rt;
    auto arrayRes = vm::JSArray::create(runtime, size, size);
    if (arrayRes == vm::ExecutionStatus::EXCEPTION) {
      abiNames->vtable->release(abiNames);
      return vm::ExecutionStatus::EXCEPTION;
    }

    // Store the array in a PinnedValue first
    result = std::move(*arrayRes);
    vm::JSArray::setStorageEndIndex(result, runtime, size);
    for (size_t i = 0; i < size; ++i) {
      auto shv = vm::SmallHermesValue::encodeSymbolValue(*toHandle(names[i]));
      vm::JSArray::unsafeSetExistingElementAt(*result, runtime, i, shv);
    }
    abiNames->vtable->release(abiNames);
    return vm::ExecutionStatus::RETURNED;
  }
};

HermesABIObjectOrError create_object_from_host_object(
    HermesABIRuntime *abiRt,
    HermesABIHostObject *ho) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto objRes = vm::HostObject::createWithoutPrototype(
      runtime, std::make_unique<HostObjectWrapper>(hart, ho));
  assert(
      objRes != vm::ExecutionStatus::EXCEPTION &&
      "Failed to create HostObject");
  return hart->createObjectOrError(*objRes);
}

HermesABIHostObject *get_host_object(HermesABIRuntime *, HermesABIObject obj) {
  if (auto h = vm::Handle<vm::HostObject>::dyn_vmcast(toHandle(obj)))
    return static_cast<HostObjectWrapper *>(h->getProxy())->getHostObject();
  return nullptr;
}

HermesABINativeState *get_native_state(
    HermesABIRuntime *abiRt,
    HermesABIObject obj) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto h = toHandle(obj);

  // Proxy and HostObject cannot have native state.
  if (h->isProxyObject(runtime) || h->isHostObject(runtime))
    return nullptr;

  // Check if the internal property exists, and if so retrieve its descriptor.
  vm::NamedPropertyDescriptor desc;
  bool exists = vm::JSObject::getOwnNamedDescriptor(
      h,
      runtime,
      vm::Predefined::getSymbolID(vm::Predefined::InternalPropertyNativeState),
      desc);

  if (!exists)
    return nullptr;

  // There is a NativeState instance, return it.
  vm::NoAllocScope scope(runtime);
  vm::NativeState *ns = vm::vmcast<vm::NativeState>(
      vm::JSObject::getNamedSlotValueUnsafe(*h, runtime, desc)
          .getObject(runtime));
  assert(ns->context() && "State cannot be null.");
  return static_cast<HermesABINativeState *>(ns->context());
}

HermesABIVoidOrError set_native_state(
    HermesABIRuntime *abiRt,
    HermesABIObject obj,
    HermesABINativeState *abiState) {
  assert(abiState && "Cannot set null native state.");

  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;

  struct : public vm::Locals {
    vm::PinnedValue<vm::NativeState> ns;
  } lv;
  vm::LocalsRAII lraii(runtime, &lv);

  auto finalize = [](vm::GC &, vm::NativeState *ns) {
    auto *self = static_cast<HermesABINativeState *>(ns->context());
    self->vtable->release(self);
  };
  // Note that creating the vm::NativeState here takes ownership of abiState, so
  // if the below steps fail, abiState will simply be freed when the
  // vm::NativeState is garbage collected.
  lv.ns = vm::NativeState::create(runtime, abiState, finalize);

  auto h = toHandle(obj);
  if (h->isProxyObject(runtime)) {
    hart->nativeExceptionMessage = "Native state is unsupported on Proxy";
    return abi::createVoidOrError(HermesABIErrorCodeNativeException);
  } else if (h->isHostObject(runtime)) {
    hart->nativeExceptionMessage = "Native state is unsupported on HostObject";
    return abi::createVoidOrError(HermesABIErrorCodeNativeException);
  }

  // Store the vm::NativeState as an internal property on the object.
  auto res = vm::JSObject::defineOwnProperty(
      h,
      runtime,
      vm::Predefined::getSymbolID(vm::Predefined::InternalPropertyNativeState),
      vm::DefinePropertyFlags::getDefaultNewPropertyFlags(),
      lv.ns);
  if (res == vm::ExecutionStatus::EXCEPTION) {
    return abi::createVoidOrError(HermesABIErrorCodeJSError);
  }
  if (!*res) {
    hart->nativeExceptionMessage = "Failed to set native state.";
    return abi::createVoidOrError(HermesABIErrorCodeNativeException);
  }
  return abi::createVoidOrError();
}

HermesABIBoolOrError object_is_array(
    HermesABIRuntime *abiRt,
    HermesABIObject object) {
  auto objHandle = toHandle(object);
  if (LLVM_LIKELY(vm::vmisa<vm::JSArray>(*objHandle)))
    return abi::createBoolOrError(true);
  // Look through Proxy objects, like Array.isArray.
  auto res = vm::isArray(*impl(abiRt)->rt, *objHandle);
  if (LLVM_UNLIKELY(res == vm::ExecutionStatus::EXCEPTION))
    return abi::createBoolOrError(HermesABIErrorCodeJSError);
  return abi::createBoolOrError(*res);
}
bool object_is_arraybuffer(HermesABIRuntime *, HermesABIObject object) {
  return vm::vmisa<vm::JSArrayBuffer>(*toHandle(object));
}
bool object_is_function(HermesABIRuntime *, HermesABIObject object) {
  return vm::vmisa<vm::Callable>(*toHandle(object));
}

HermesABIWeakObjectOrError create_weak_object(
    HermesABIRuntime *abiRt,
    HermesABIObject obj) {
  return impl(abiRt)->createWeakObjectOrError(toHandle(obj).getHermesValue());
}
HermesABIValue lock_weak_object(
    HermesABIRuntime *abiRt,
    HermesABIWeakObject obj) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  const auto &wr =
      static_cast<ManagedValue<vm::WeakRoot<vm::JSObject>> *>(obj.pointer)
          ->value();
  if (const auto ptr = wr.get(runtime, runtime.getHeap()))
    return hart->createValue(vm::HermesValue::encodeObjectValue(ptr));
  return abi::createUndefinedValue();
}

void get_utf8_from_string(
    HermesABIRuntime *abiRt,
    HermesABIString str,
    HermesABIGrowableBuffer *buf) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto view = vm::StringPrimitive::createStringView(runtime, toHandle(str));

  // If the string is already ASCII, we can write it directly into the buffer.
  if (LLVM_LIKELY(view.isASCII())) {
    writeToBuf(buf, {view.castToCharPtr(), view.length()});
    return;
  }

  // For non-ASCII strings, convert them to UTF-8 and then copy the result into
  // the buffer. If necessary, we could eliminate the copy in the future by
  // writing directly into the buffer.
  std::string convertBuf;
  convertUTF16ToUTF8WithReplacements(
      convertBuf, {view.castToChar16Ptr(), view.length()});
  writeToBuf(buf, convertBuf);
}

void get_utf8_from_propnameid(
    HermesABIRuntime *abiRt,
    HermesABIPropNameID name,
    HermesABIGrowableBuffer *buf) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  struct : public vm::Locals {
    vm::PinnedValue<vm::StringPrimitive> nameStr;
  } lv;
  vm::LocalsRAII lraii(runtime, &lv);
  auto view =
      runtime.getIdentifierTable().getStringView(*toHandle(name), lv.nameStr);
  // TODO: Consider aligning this with the behaviour for symbols below, such
  // that PropNameIDs backed by Symbols also get wrapped in "Symbol()".
  if (LLVM_LIKELY(view.isASCII())) {
    writeToBuf(buf, {view.castToCharPtr(), view.length()});
    return;
  }

  std::string convertBuf;
  convertUTF16ToUTF8WithReplacements(
      convertBuf, {view.castToChar16Ptr(), view.length()});
  writeToBuf(buf, convertBuf);
}

void get_utf8_from_symbol(
    HermesABIRuntime *abiRt,
    HermesABISymbol name,
    HermesABIGrowableBuffer *buf) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  struct : public vm::Locals {
    vm::PinnedValue<vm::StringPrimitive> nameStr;
  } lv;
  vm::LocalsRAII lraii(runtime, &lv);
  auto view =
      runtime.getIdentifierTable().getStringView(*toHandle(name), lv.nameStr);

  // Wrap the resulting string in "Symbol()" to match the behaviour of
  // symbolDescriptiveString. We don't directly use symbolDescriptiveString here
  // to avoid extra allocations and error conditions.
  // For simplicity, we first construct the result string in this buffer before
  // copying it over. This is inefficient since we force a copy, but should be
  // fairly uncommon and the strings should be small.
  std::string res = "Symbol(";

  if (LLVM_LIKELY(view.isASCII())) {
    res.append(view.castToCharPtr(), view.length());
  } else {
    std::string cvtBuf;
    convertUTF16ToUTF8WithReplacements(
        cvtBuf, {view.castToChar16Ptr(), view.length()});
    res.append(cvtBuf);
  }
  res.push_back(')');
  writeToBuf(buf, res);
}

HermesABIBoolOrError
instance_of(HermesABIRuntime *abiRt, HermesABIObject o, HermesABIFunction f) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto result = vm::instanceOfOperator_RJS(runtime, toHandle(o), toHandle(f));
  if (result == vm::ExecutionStatus::EXCEPTION)
    return abi::createBoolOrError(HermesABIErrorCodeJSError);
  return abi::createBoolOrError(*result);
}

bool strict_equals_symbol(
    HermesABIRuntime *abiRt,
    HermesABISymbol a,
    HermesABISymbol b) {
  return toHandle(a) == toHandle(b);
}
bool strict_equals_bigint(
    HermesABIRuntime *abiRt,
    HermesABIBigInt a,
    HermesABIBigInt b) {
  return toHandle(a)->compare(*toHandle(b)) == 0;
}
bool strict_equals_string(
    HermesABIRuntime *abiRt,
    HermesABIString a,
    HermesABIString b) {
  return toHandle(a)->equals(*toHandle(b));
}
bool strict_equals_object(
    HermesABIRuntime *abiRt,
    HermesABIObject a,
    HermesABIObject b) {
  return toHandle(a) == toHandle(b);
}

HermesABIBoolOrError drain_microtasks(HermesABIRuntime *abiRt, int) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  if (runtime.hasMicrotaskQueue()) {
    auto drainRes = runtime.drainJobs();
    if (drainRes == vm::ExecutionStatus::EXCEPTION)
      return abi::createBoolOrError(HermesABIErrorCodeJSError);
  }

  // Clear strong references to objects retained by WeakRef accesses.
  runtime.clearKeptObjects();

  // Run the callback for dead registered targets.
  auto cleanupRes = runtime.cleanUpFinalizationCallbacks();
  if (cleanupRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createBoolOrError(HermesABIErrorCodeJSError);

  // drainJobs currently drains the entire queue, unless there is an exception,
  // so always return true.
  // TODO(T89426441): Support max_hint.
  return abi::createBoolOrError(true);
}

HermesABIBigIntOrError create_bigint_from_int64(
    HermesABIRuntime *abiRt,
    int64_t value) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto res = vm::BigIntPrimitive::fromSigned(runtime, value);
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createBigIntOrError(HermesABIErrorCodeJSError);
  return hart->createBigIntOrError(*res);
}
HermesABIBigIntOrError create_bigint_from_uint64(
    HermesABIRuntime *abiRt,
    uint64_t value) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  auto res = vm::BigIntPrimitive::fromUnsigned(runtime, value);
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createBigIntOrError(HermesABIErrorCodeJSError);
  return hart->createBigIntOrError(*res);
}
bool bigint_is_int64(HermesABIRuntime *, HermesABIBigInt bigint) {
  return toHandle(bigint)->isTruncationToSingleDigitLossless(
      /* signedTruncation */ true);
}
bool bigint_is_uint64(HermesABIRuntime *, HermesABIBigInt bigint) {
  return toHandle(bigint)->isTruncationToSingleDigitLossless(
      /* signedTruncation */ false);
}
uint64_t bigint_truncate_to_uint64(HermesABIRuntime *, HermesABIBigInt bigint) {
  auto digit = toHandle(bigint)->truncateToSingleDigit();
  static_assert(
      sizeof(digit) == sizeof(uint64_t),
      "BigInt digit is no longer sizeof(uint64_t) bytes.");
  return digit;
}
HermesABIStringOrError bigint_to_string(
    HermesABIRuntime *abiRt,
    HermesABIBigInt bigint,
    unsigned radix) {
  auto *hart = impl(abiRt);
  auto &runtime = *hart->rt;
  if (radix < 2 || radix > 36) {
    hart->nativeExceptionMessage = "Radix must be between 2 and 36";
    return abi::createStringOrError(HermesABIErrorCodeNativeException);
  }

  auto toStringRes = vm::BigIntPrimitive::toString(
      runtime, vm::createPseudoHandle(*toHandle(bigint)), radix);

  if (toStringRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createStringOrError(HermesABIErrorCodeJSError);
  return hart->createStringOrError(*toStringRes);
}

constexpr HermesABIRuntimeVTable HermesABIRuntimeImpl::vtable = {
    release_hermes_runtime,
    get_and_clear_js_error_value,
    get_and_clear_native_exception_message,
    set_js_error_value,
    set_native_exception_message,
    clone_propnameid,
    clone_string,
    clone_symbol,
    clone_object,
    clone_bigint,
    evaluate_javascript_source,
    evaluate_hermes_bytecode,
    get_global_object,
    create_string_from_utf8,
    create_object,
    has_object_property_from_value,
    has_object_property_from_propnameid,
    get_object_property_from_value,
    get_object_property_from_propnameid,
    set_object_property_from_value,
    set_object_property_from_propnameid,
    get_object_property_names,
    set_object_external_memory_pressure,
    create_array,
    get_array_length,
    create_arraybuffer_from_external_data,
    get_arraybuffer_data,
    get_arraybuffer_size,
    create_propnameid_from_string,
    create_propnameid_from_symbol,
    prop_name_id_equals,
    call,
    call_as_constructor,
    create_function_from_host_function,
    get_host_function,
    create_object_from_host_object,
    get_host_object,
    get_native_state,
    set_native_state,
    object_is_array,
    object_is_arraybuffer,
    object_is_function,
    create_weak_object,
    lock_weak_object,
    get_utf8_from_string,
    get_utf8_from_propnameid,
    get_utf8_from_symbol,
    instance_of,
    strict_equals_symbol,
    strict_equals_bigint,
    strict_equals_string,
    strict_equals_object,
    drain_microtasks,
    create_bigint_from_int64,
    create_bigint_from_uint64,
    bigint_is_int64,
    bigint_is_uint64,
    bigint_truncate_to_uint64,
    bigint_to_string,
};

} // namespace

extern "C" {
const HermesABIVTable *get_hermes_abi_vtable() {
  static constexpr HermesABIVTable abiVtable = {
      make_hermes_runtime,
      is_hermes_bytecode,
  };
  return &abiVtable;
}
} // extern "C"
