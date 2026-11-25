/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_HANDLE_H
#define HERMES_VM_HANDLE_H

#include "hermes/VM/Casting.h"
#include "hermes/VM/GCDecl.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/HermesValueTraits.h"
#include "llvh/Support/type_traits.h"

#include <algorithm>

namespace hermes {
namespace vm {

// Forward declarations.
template <typename T = HermesValue>
class Handle;
template <typename T = HermesValue>
class MutableHandle;
template <typename T>
class Handle;
template <typename T>
class MutableHandle;
template <typename T>
class PinnedValue;

/// A helper type, specialized on whether we are casting to a cell or a
/// non-cell.
template <typename T, bool isCell = HermesValueTraits<T>::is_cell>
struct HermesValueCast {
  /// In debug mode, check that the value is of the correct type and assert
  /// if it isn't.
  static void assertValid(HermesValue x) {
#ifndef NDEBUG
    (void)vmcast<T>(x);
#endif
  }
};

template <typename T>
struct HermesValueCast<T, false> {
  /// In debug mode, check that the value is of the correct type and assert
  /// if it isn't.
  static void assertValid(HermesValue x) {
#ifndef NDEBUG
    (void)HermesValueTraits<T>::decode(x);
#endif
  }
};

/// This class is used in performance-sensitive context in situations where we
/// want to encode in the function signature that allocations may be performed,
/// potentially moving the object, but we don't want to incur the cost of
/// always rooting the value. The callee will pin it internally if it needs to.
///
/// For example:
/// \code
///   bool checkFlag(PseudoHandle<Foo> foo, Runtime &runtime) {
///     if (foo->cheapCheck())
///       return true;
///     struct : public Locals {
///       PinnedValue<Foo> foo;
///     } lv;
///     LocalsRAII lraii(runtime, &lv);
///     lv.foo = std::move(foo);
///     return expensiveCheck(lv.foo, runtime);
///   }
/// \endcode
template <typename T = HermesValue>
class PseudoHandle {
  using traits_type = HermesValueTraits<T>;

 public:
  using value_type = typename traits_type::value_type;
  using arrow_type = typename traits_type::arrow_type;

 private:
  template <class U>
  friend class PseudoHandle;

  value_type value_;
#ifndef NDEBUG
  bool valid_{true};
#endif

  explicit PseudoHandle(value_type value) : value_(value) {}

 public:
  PseudoHandle(const PseudoHandle &) = delete;
  PseudoHandle &operator=(const PseudoHandle &) = delete;

#ifndef NDEBUG
  PseudoHandle(PseudoHandle &&hnd) : value_(hnd.value_), valid_(hnd.valid_) {
    hnd.valid_ = false;
  }
  PseudoHandle &operator=(PseudoHandle &&hnd) {
    value_ = std::move(hnd.value_);
    valid_ = hnd.valid_;
    hnd.valid_ = false;
    return *this;
  }
#else
  PseudoHandle(PseudoHandle &&) = default;
  PseudoHandle &operator=(PseudoHandle &&) = default;
#endif

  ~PseudoHandle() = default;

  constexpr PseudoHandle() : value_(traits_type::defaultValue()) {}
  PseudoHandle(Handle<T> handle) : value_(*handle) {}

  /// Implicitly convert from a PinnedValue of compatible type.
  template <
      typename U,
      typename std::enable_if<IsHermesValueConvertible<U, T>::value>::type>
  /* implicit */ PseudoHandle(const PinnedValue<U> &pv) : value_(*pv) {}

  /// Conveniently construct PseudoHandle<HermesValue> from a PinnedHermesValue
  /// pointer.
  explicit PseudoHandle(const PinnedHermesValue *pvalue) : value_(*pvalue) {
    static_assert(
        std::is_same<value_type, HermesValue>::value,
        "This constructor can only be used for PseudoHandle<HermesValue>");
  }

  PseudoHandle &operator=(Handle<T> handle) {
    value_ = *handle;
#ifndef NDEBUG
    valid_ = true;
#endif
    return *this;
  }

  /// Zero-cost conversion between compatible types.
  template <
      typename U,
      typename = typename std::enable_if<std::is_convertible<
          typename PseudoHandle<U>::value_type,
          typename PseudoHandle<T>::value_type>::value>::type>
  /* implicit */ PseudoHandle(PseudoHandle<U> &&other)
      : value_(static_cast<value_type>(other.get())) {
    other.invalidate();
  }

  void invalidate() {
#ifndef NDEBUG
    valid_ = false;
#endif
  }

  value_type get() const {
    assert(valid_ && "Pseudo handle has been invalidated");
    return value_;
  }

  arrow_type operator->() const {
    assert(valid_ && "Pseudo handle has been invalidated");
    return traits_type::arrow(value_);
  }

  explicit operator bool() const {
    assert(valid_ && "Pseudo handle has been invalidated");
    return value_ != nullptr;
  }

  /// \return the value encoded as HermesValue
  HermesValue getHermesValue() const {
    assert(valid_ && "Pseudo handle has been invalidated");
    return traits_type::encode(value_);
  }

  /// \return value_ directly without going through HermesValue.
  /// Used if the raw value_ was directly specified.
  /// In particular, used by CallResult to check if a value is exceptional.
  value_type unsafeGetValue() const {
    return value_;
  }

  /// Create a \c PseudoHandle from a value.
  static PseudoHandle<T> create(value_type value) {
    return PseudoHandle<T>(value);
  }

  template <typename U>
  static inline PseudoHandle vmcast(PseudoHandle<U> &&other);

  template <typename U>
  static inline PseudoHandle dyn_vmcast(PseudoHandle<U> &&other);
};

/// A simple typed wrapper around a PinnedHermesValue, providing convenient
/// conversion to/from a Handle<T>.
template <typename T = HermesValue>
class PinnedValue : private PinnedHermesValue {
  template <class U>
  friend class Handle;
  friend struct RootAcceptor;

  using traits_type = HermesValueTraits<T>;
  using value_type = typename traits_type::value_type;
  using arrow_type = typename traits_type::arrow_type;

 public:
  constexpr PinnedValue() : PinnedValue(traits_type::defaultValue()) {}

  /// Copy and move constructors are disabled, to avoid accidentally passing
  /// PinnedValue by value, since its location must always be known to the GC.
  PinnedValue(const PinnedValue &) = delete;
  PinnedValue(PinnedValue &&) = delete;

  constexpr PinnedValue(value_type val)
      : PinnedHermesValue(traits_type::encode(val)) {}

  PinnedValue &operator=(PinnedValue &&) = default;
  PinnedValue &operator=(const PinnedValue &other) = default;

  PinnedValue &operator=(const Handle<T> &handle) {
    setNoBarrier(handle.getHermesValue());
    return *this;
  }
  PinnedValue &operator=(value_type val) {
    setNoBarrier(traits_type::encode(val));
    return *this;
  }

  template <
      typename U,
      typename =
          typename std::enable_if<IsHermesValueConvertible<U, T>::value>::type>
  PinnedValue &operator=(PseudoHandle<U> &&other) {
    setNoBarrier(other.getHermesValue());
    other.invalidate();
    return *this;
  }

  /// Assign the HermesValue \p val to this PinnedValue, asserting that it
  /// has type U which is compatible with T. This allows assigning a HermesValue
  /// with a known type to the PinnedValue without having to extract a
  /// pointer/symbol from it only to then re-encode it.
  template <
      typename U,
      typename =
          typename std::enable_if<IsHermesValueConvertible<U, T>::value>::type>
  void castAndSetHermesValue(HermesValue val) {
    HermesValueCast<U>::assertValid(val);
    setNoBarrier(val);
  }

  value_type get() const {
    return traits_type::decode(*this);
  }
  value_type operator*() const {
    return get();
  }
  arrow_type operator->() const {
    return traits_type::arrow(*this);
  }

  HermesValue getHermesValue() const {
    return *this;
  }

  /// A method to obtaim a reference to the underlying PinnedHermesValue, but
  /// only if T is actually HermesValue.
  PinnedHermesValue *getPinnedHermesValue() {
    static_assert(
        std::is_same<T, HermesValue>::value,
        "This method can only be used for PinnedValue<HermesValue>");
    return this;
  }

  void clear() {
    setNoBarrier(HermesValue::encodeRawZeroValueUnsafe());
  }
  bool isCleared() const {
    return raw == 0;
  }
};

/// A specialization of PinnedValue that is used to hold special HermesValue:
/// those directly encode a SmallHermesValue. This has very limited uses so it
/// does not provide the same set of methods as the templated PinnedValue.
template <>
class PinnedValue<SmallHermesValue> : private PinnedHermesValue {
 public:
  PinnedValue() : PinnedValue(SmallHermesValue::encodeUndefinedValue()) {}

  /// Copy and move constructors are disabled, to avoid accidentally passing
  /// PinnedValue by value, since its location must always be known to the GC.
  PinnedValue(const PinnedValue &) = delete;
  PinnedValue(PinnedValue &&) = delete;

  /// Encode \p val directly into the underlying PinnedHermesValue.
  /// Require explicit construction to avoid accidentally passing a
  /// SmallHermesValue to a function takes PinnedValue<SmallHermesValue>.
  explicit PinnedValue(SmallHermesValue val)
      : PinnedHermesValue(val.encodeAsHermesValue()) {}

  PinnedValue &operator=(PinnedValue &&) = default;
  PinnedValue &operator=(const PinnedValue &other) = default;
  ~PinnedValue() = default;

  /// Encode \p val directly into the underlying PinnedHermesValue.
  PinnedValue &operator=(SmallHermesValue val) {
    setNoBarrier(val.encodeAsHermesValue());
    return *this;
  }

  /// \return The SmallHermesValue held by this PinnedValue.
  SmallHermesValue getSmallHermesValue() const {
    return SmallHermesValue::decodeFromHermesValue(*this);
  }
};

/// A view of a PinnedHermesValue, which is trackable by the GC and will be
/// correctly marked and updated if objects are moved. The value is valid while
/// the viewed location is alive.
/// This is a very lightweight class: copies are very cheap (just copying a
/// register).
/// This is the base object factoring out common code from the type-specific
/// versions.
class HandleBase {
  template <typename U>
  friend class MutableHandle;
  /// Pointer to the location where the actual value is stored. That location
  /// is automatically updated by the garbage collector if necessary.
  PinnedHermesValue *handle_;

 protected:
  HandleBase &operator=(const HandleBase &other) = default;

  PinnedHermesValue *&handleRef() {
#ifdef HERMES_SLOW_DEBUG
    assert(
        !handle_->isInvalid() &&
        "Reading an invalid HermesValue through a handle");
#endif
    return handle_;
  }
  const PinnedHermesValue *handleRef() const {
#ifdef HERMES_SLOW_DEBUG
    assert(
        !handle_->isInvalid() &&
        "Reading an invalid HermesValue through a handle");
#endif
    return handle_;
  }

 public:
  /// Create a Handle aliasing a non-movable HermesValue.
  explicit HandleBase(const PinnedHermesValue *valueAddr)
      : handle_(const_cast<PinnedHermesValue *>(valueAddr)) {}

  /// Copy constructor.
  HandleBase(const HandleBase &sc) = default;

  ~HandleBase() = default;

  const PinnedHermesValue *operator->() {
    return handleRef();
  }
  HermesValue operator*() {
    return *handleRef();
  }

  HermesValue getHermesValue() const {
    return *handleRef();
  }

  /// \return the underlying pointer to the PinnedHermesValue.
  /// Note that this gives up safety guarantees Handle provides,
  /// and should only be used if the Handle will be kept alive
  /// for the entire lifetime of the returned value.
  const PinnedHermesValue *unsafeGetPinnedHermesValue() const {
    // Don't call handleRef() because it creates HermesValues in debug mode.
    return handle_;
  }
} HERMES_ATTRIBUTE_WARN_UNUSED_VARIABLES;

static_assert(
    std::is_trivially_copyable<HandleBase>::value &&
        sizeof(HandleBase) == sizeof(void *),
    "Handle must fit in a register and be trivially copyable");

/// A view of a PinnedHermesValue, which is trackable by the GC and will be
/// correctly marked and updated if objects are moved. The value is valid while
/// the viewed location is alive.
/// This is a very lightweight class: copies are very cheap (just copying a
/// register).
template <typename T>
class Handle : public HandleBase {
 protected:
  /// Create a Handle aliasing a non-movable HermesValue.
  explicit Handle(const PinnedHermesValue *valueAddr, bool)
      : HandleBase(const_cast<PinnedHermesValue *>(valueAddr)) {}

  explicit Handle(const HandleBase &hb, bool) : HandleBase(hb) {}

 public:
  using value_type = typename HermesValueTraits<T>::value_type;

  /// Create a Handle aliasing a non-movable HermesValue.
  explicit Handle(const PinnedHermesValue *valueAddr)
      : HandleBase(const_cast<PinnedHermesValue *>(valueAddr)) {
    static_assert(
        std::is_same<value_type, HermesValue>::value,
        "This constructor can only be used for Handle<HermesValue>");
  }

  /// Implicitly convert from a PinnedValue of a compatible type.
  template <
      typename U,
      typename =
          typename std::enable_if<IsHermesValueConvertible<U, T>::value>::type>
  /* implicit */ Handle(const PinnedValue<U> &pv) : HandleBase(&pv) {}

  /// Convert between compatible types.
  template <
      typename U,
      typename =
          typename std::enable_if<IsHermesValueConvertible<U, T>::value>::type>
  Handle(const Handle<U> &other) : HandleBase(other) {}

  Handle(const Handle<T> &) = default;
  Handle(Handle<T> &&) = default;

  template <
      typename U,
      typename =
          typename std::enable_if<IsHermesValueConvertible<U, T>::value>::type>
  Handle<T> &operator=(const Handle<U> &other) {
    HandleBase::operator=(other);
    return *this;
  }

  Handle<T> &operator=(const Handle<T> &other) = default;

  value_type get() const {
    return HermesValueTraits<T>::decode(getHermesValue());
  }

  /// \return true if it contains a non-null pointer.
  template <
      typename U = T,
      typename = typename std::enable_if<HermesValueTraits<U>::is_cell>::type>
  explicit operator bool() const {
    return handleRef()->getPointer();
  }

  typename HermesValueTraits<T>::arrow_type operator->() const {
    return HermesValueTraits<T>::arrow(*handleRef());
  }

  value_type operator*() const {
    return get();
  }

  bool operator==(const Handle &value) const {
    return get() == value.get();
  }

  /// Create a Handle from aliasing a pinned HermesValue and assert that the
  /// value has the correct type.
  static Handle<T> vmcast(const PinnedHermesValue *valueAddr) {
    HermesValueCast<T>::assertValid(*valueAddr);
    return Handle<T>(valueAddr, true);
  }

  /// Create a Handle from aliasing a PinnedValue and assert that the
  /// value has the correct type.
  template <typename U>
  static Handle<T> vmcast(const PinnedValue<U> *pv) {
    HermesValueCast<T>::assertValid(*pv);
    return Handle<T>(pv, true);
  }

  /// Create a Handle from aliasing a pinned HermesValue and assert that the
  /// value has the correct type.
  static Handle<T> vmcast_or_null(const PinnedHermesValue *valueAddr) {
    (void)hermes::vm::vmcast_or_null<T>(*valueAddr);
    return Handle<T>(valueAddr, true);
  }

  /// Cast the argument to the desired type and assert if it doesn't have the
  /// correct type.
  template <typename U>
  static Handle<T> vmcast(const Handle<U> &other) {
    HermesValueCast<T>::assertValid(other.getHermesValue());
    return Handle<T>(other, true);
  }

  /// Cast the argument to the desired type and assert if it doesn't have the
  /// correct type.
  static Handle<T> vmcast_or_null(const HandleBase &other) {
    (void)hermes::vm::vmcast_or_null<T>(other.getHermesValue());
    return Handle<T>(other, true);
  }

  template <typename U>
  static Handle<T> dyn_vmcast(const Handle<U> &other);

  /// Create a handle based on a potentially invalid address.
  /// For example, this is used in CallResult for exception values.
  static Handle<T> unsafeCreate(const PinnedHermesValue *valueAddr) {
    return Handle<T>(valueAddr, true);
  }
} HERMES_ATTRIBUTE_WARN_UNUSED_VARIABLES;

static_assert(
    std::is_trivially_copyable<Handle<>>::value &&
        sizeof(Handle<>) == sizeof(void *),
    "Handle must fit in a register and be trivially copyable");

/// Extend Handle to allow mutability. The vast majority of users must not
/// be able to modify the handle, but for those few that do, we need to be as
/// explicit as possible by using this class.
template <typename T>
class MutableHandle : public Handle<T> {
  /// Alias a MutableHandle with a non-movable HermesValue without any checks.
  explicit MutableHandle(PinnedHermesValue *valueAddr, bool dummy)
      : Handle<T>(valueAddr, dummy) {}

 public:
  using value_type = typename Handle<T>::value_type;

  /// Construct a new Handle by aliasing an existing PinnedValue of the same
  /// type.
  /* implicit */ MutableHandle(const PinnedValue<T> &pv) : Handle<T>(pv) {}

  MutableHandle(const MutableHandle &sc) = default;
  MutableHandle(MutableHandle &&sc) = default;

  /// Copy constructor from MutableHandle<U> where U is compatible with T.
  template <
      typename U,
      typename =
          typename std::enable_if<IsHermesValueConvertible<T, U>::value>::type>
  MutableHandle(const MutableHandle<U> &sc) : Handle<T>(sc) {}

  MutableHandle &operator=(MutableHandle &&sc) {
    *HandleBase::handleRef() = *sc.handleRef();
    return *this;
  }

  MutableHandle &operator=(PseudoHandle<T> &&other) {
    set(other.get());
    other.invalidate();
    return *this;
  }

  MutableHandle &operator=(value_type value) {
    set(value);
    return *this;
  }
  /// Clear the value of the handle so it doesn't prevent an object from GC.
  void clear() {
    set(HermesValueTraits<T>::defaultValue());
  }

  void set(PseudoHandle<T> &&other) {
    set(other.get());
    other.invalidate();
  }

  void set(value_type value) {
    *HandleBase::handleRef() = HermesValueTraits<T>::encode(value);
  }

  /// Assign the HermesValue \p val to this MutableHandle, asserting that it
  /// has type U which is compatible with T. This allows assigning a HermesValue
  /// with a known type to the PinnedValue without having to extract a
  /// pointer/symbol from it only to then re-encode it.
  template <
      typename U,
      typename =
          typename std::enable_if<IsHermesValueConvertible<U, T>::value>::type>
  void castAndSetHermesValue(HermesValue val) {
    HermesValueCast<U>::assertValid(val);
    *HandleBase::handleRef() = val;
  }

  /// Create a MutableHandle from a pinned HermesValue and assert that the value
  /// has the correct type.
  static MutableHandle<T> vmcast(PinnedHermesValue *valueAddr) {
    HermesValueCast<T>::assertValid(*valueAddr);
    return MutableHandle<T>(valueAddr, true);
  }

  /// Alias a MutableHandle with a pinned HermesValue. This is only for cases
  /// where the MutableHandle<> is an output-only value, so we don't want to
  /// unnecessarily initialize it to a value compatible with its type.
  static MutableHandle<T> aliasForOutput(PinnedHermesValue *valueAddr) {
    // It may seem like a good idea to set valueAddr to an invalid value in
    // debug builds, in an effort to catch reads on this MutableHandle before a
    // write has been issued. But, this would be incorect for a couple of
    // reasons. First, writing to a MutableHandle triggers a check to make sure
    // the handle doesn't currently contain invalid. Second, this would prevent
    // this method from being useful in certain situations in the interpreter.
    // An output-only register may be aliased to an input register, so we could
    // inadvertently clobber an input register before we've read it if we used
    // this method on it.
    return MutableHandle<T>(valueAddr, true);
  }

  /// A version of \c aliasForOutput() that takes a \c MutableHandle<U>.
  template <
      typename U,
      typename =
          typename std::enable_if<IsHermesValueConvertible<T, U>::value>::type>
  static MutableHandle aliasForOutput(const MutableHandle<U> &other) {
    return MutableHandle(
        const_cast<PinnedHermesValue *>(other.handleRef()), true);
  }

  PinnedHermesValue *unsafeGetPinnedHermesValue() {
    return HandleBase::handleRef();
  }
} HERMES_ATTRIBUTE_WARN_UNUSED_VARIABLES;

static_assert(
    sizeof(MutableHandle<>) == sizeof(HandleBase),
    "MutableHandle must be a thin wrapper on top of Handle");

/// An RAII object to manage the use of a pre-allocated PinnedValue as a handle.
/// It must not be recursively reused, and it clears the value on destruction.
template <typename T>
class UsePinnedValueRAII {
  PinnedHermesValue &phv_;

 public:
  using value_type = typename HermesValueTraits<T>::value_type;

  explicit UsePinnedValueRAII(PinnedHermesValue &phv) : phv_(phv) {
    assert(
        phv.getRaw() == 0 &&
        "PinnedHermesValue is not cleared - possible recursive reuse?");
  }
  ~UsePinnedValueRAII() {
    phv_ = HermesValue::encodeRawZeroValueUnsafe();
  }

  // Disallow copying and moving.
  UsePinnedValueRAII(const UsePinnedValueRAII &) = delete;
  void operator=(const UsePinnedValueRAII &) = delete;

  Handle<T> hnd() const {
    return Handle<T>(&phv_);
  }
  MutableHandle<T> mut() {
    return MutableHandle<T>::aliasForOutput(&phv_);
  }

  operator Handle<T>() const {
    return Handle<T>(&phv_);
  }
  operator MutableHandle<T>() {
    return MutableHandle<T>::aliasForOutput(&phv_);
  }

  UsePinnedValueRAII &operator=(value_type value) {
    phv_ = HermesValueTraits<T>::encode(value);
    return *this;
  }
  void set(value_type value) {
    phv_ = HermesValueTraits<T>::encode(value);
  }

  value_type get() const {
    return HermesValueTraits<T>::decode(phv_);
  }

  /// Assign the HermesValue \p val to this PinnedValue, asserting that it
  /// has type U which is compatible with T. This allows assigning a HermesValue
  /// with a known type to the PinnedValue without having to extract a
  /// pointer/symbol from it only to then re-encode it.
  template <
      typename U,
      typename =
          typename std::enable_if<IsHermesValueConvertible<U, T>::value>::type>
  void castAndSetHermesValue(HermesValue val) {
    HermesValueCast<U>::assertValid(val);
    phv_ = val;
  }

  HermesValue getHermesValue() const {
    return phv_;
  }

  PinnedHermesValue *getPinnedHermesValue() {
    return &phv_;
  }
};

/// Create a \c PseudoHandle from a pointer.
template <typename T>
inline PseudoHandle<T> createPseudoHandle(T *ptr) {
  return PseudoHandle<T>::create(ptr);
}

/// Create a \c PseudoHandle from a HermesValue.
inline PseudoHandle<> createPseudoHandle(HermesValue value) {
  return PseudoHandle<>::create(value);
}

} // namespace vm
} // namespace hermes

namespace std {
/// std::swap<MutableHandle<T>> specialization. This is needed as the default
/// implementation is not correct due MutableHandle<>::operator= definition.
template <typename T>
void swap(hermes::vm::MutableHandle<T> &a, hermes::vm::MutableHandle<T> &b) {
  typename hermes::vm::MutableHandle<T>::value_type tmp = a.get();
  a.set(b.get());
  b.set(tmp);
}
} // namespace std
/// When we updated LLVM most recently (2/2019), we had build failures.
/// Apparently, llvh::Optional<T> asks whether T "isPodLike", to
/// determine whether it can use an instantiation that invokes a copy
/// ctor of T.  isPodLike is implemented as isTriviallyCopyable.  The
/// inference by llvh::Optional is wrong, since isTriviallyCopyable<T> does
/// not imply that T has a copy ctor.  We get around this by explicitly
/// declaring that the particular instantiation that causes the failures
/// is *not* PodLike.
/// TODO(T40600161): when we next change LLVM version, see if this is
/// still necessary.
namespace llvh {

// Instantiating Optional with a T "isPodLike" will result in a specialized
// OptionalStorage class without move ctor and would only copy T. Since the
// PseudoHandle is not copyable we specialized the trait to be always false.
template <typename T>
struct isPodLike<hermes::vm::PseudoHandle<T>> {
  static const bool value = false;
};
} // namespace llvh

#endif // HERMES_VM_HANDLE_H
