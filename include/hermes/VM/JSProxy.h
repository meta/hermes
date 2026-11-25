/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSPROXY_H
#define HERMES_VM_JSPROXY_H

#include "hermes/VM/JSObject.h"

namespace hermes {
namespace vm {

namespace detail {

/// A Proxy is, fundamentally, a target (the thing the Proxy wraps),
/// and the traps (which are stored as properties on the handler).
/// These are taken directly from the Proxy constructor or
/// Proxy.revocable factory function.  This is a separate class
/// because it is a member of JSProxy and JSCallableProxy.
struct ProxySlots {
  GCPointer<JSObject> target;
  GCPointer<JSObject> handler;
};

// These methods are not part of the public API, but are used by
// JSCallableProxy.

detail::ProxySlots &slots(JSObject *selfHandle);

/// \return the trap \p name of the proxy \p selfHandle, or a null
/// PseudoHandle if the handler does not define it.
CallResult<PseudoHandle<Callable>>
findTrap(Handle<JSObject> selfHandle, Runtime &runtime, Predefined::Str name);

} // namespace detail

/// JSProxy implements all the behavior for proxies except for [[Call]]
/// and [[Construct]], which are implemented by JSCallableProxy.
/// JSCallableProxy acts like a JSProxy and a NativeFunction, but we
/// don't want to use multiple inheritance.  So, JSCallableProxy
/// extends NativeFunction (because the VM expects objects which can be
/// called to impelement Callable), not JSProxy, and instead JSProxy
/// and JSCallableProxy both include ProxySlots as a member.  In order
/// to provide the functionality shared by JSProxy and JSCallableProxy,
/// the ES9 9.5.x methods here take a Handle<JSObject> instead of a
/// Handle<JSProxy>, and dynamically check if it's a JSProxy or
/// JSCallableProxy.  The caller uses a flag which is set on both,
/// instead of calling isa twice.
class JSProxy : public JSObject {
 public:
  friend void JSProxyBuildMeta(const GCCell *cell, Metadata::Builder &mb);
  friend detail::ProxySlots &detail::slots(JSObject *selfHandle);

  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSProxyKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSProxyKind;
  }

  static PseudoHandle<JSProxy> create(Runtime &runtime);

  static PseudoHandle<JSProxy> create(
      Runtime &runtime,
      Handle<JSObject> /* prototype */) {
    return create(runtime);
  }

  static void setTargetAndHandler(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      Handle<JSObject> target,
      Handle<JSObject> handler);

  static PseudoHandle<JSObject> getTarget(JSObject *proxy, PointerBase &base) {
    return createPseudoHandle(detail::slots(proxy).target.get(base));
  }

  static PseudoHandle<JSObject> getHandler(JSObject *proxy, PointerBase &base) {
    return createPseudoHandle(detail::slots(proxy).handler.get(base));
  }

  /// Proxy.create checks if the proxy is revoked, which is defined by
  /// the spec as having a null handler.
  static bool isRevoked(JSObject *proxy, PointerBase &base) {
    return !getHandler(proxy, base);
  }

  // ES9 9.5 Proxy internal methods

  static CallResult<PseudoHandle<JSObject>> getPrototypeOf(
      Handle<JSObject> selfHandle,
      Runtime &runtime);

  static CallResult<bool> setPrototypeOf(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      Handle<JSObject> parent);

  static CallResult<bool> isExtensible(
      Handle<JSObject> selfHandle,
      Runtime &runtime);

  static CallResult<bool> preventExtensions(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      PropOpFlags opFlags = PropOpFlags());

  static CallResult<bool> getOwnProperty(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      Handle<> nameValHandle,
      ComputedPropertyDescriptor &desc,
      MutableHandle<> *valueOrAccessor);

  static CallResult<bool> defineOwnProperty(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      Handle<> nameValHandle,
      DefinePropertyFlags dpFlags,
      Handle<> valueOrAccessor,
      PropOpFlags opFlags);

  static CallResult<bool>
  hasNamed(Handle<JSObject> selfHandle, Runtime &runtime, SymbolID name);

  static CallResult<bool> hasComputed(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      Handle<> nameValHandle);

  static CallResult<PseudoHandle<>> getNamed(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      SymbolID name,
      Handle<> receiver);

  static CallResult<PseudoHandle<>> getComputed(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      Handle<> nameValHandle,
      Handle<> receiver);

  static CallResult<bool> setNamed(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      SymbolID name,
      Handle<> valueHandle,
      Handle<> receiver);

  static CallResult<bool> setComputed(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      Handle<> nameValHandle,
      Handle<> valueHandle,
      Handle<> receiver);

  static CallResult<bool>
  deleteNamed(Handle<JSObject> selfHandle, Runtime &runtime, SymbolID name);

  static CallResult<bool> deleteComputed(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      Handle<> nameValHandle);

  // If okflags.getIncludeNonEnumerable() is not true, this call needs to
  // call getOwnProperty traps in order to discover if each property is
  // enumerable.  Thus, unlike most JSProxy functions, this function can
  // call multiple traps.
  static CallResult<PseudoHandle<JSArray>> ownPropertyKeys(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      OwnKeysFlags okFlags);

 public:
  JSProxy(Runtime &runtime, Handle<JSObject> parent, Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz) {}

 private:
  detail::ProxySlots slots_;
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_JSPROXY_H
