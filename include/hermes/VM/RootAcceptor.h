/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_SLOTACCEPTOR_H
#define HERMES_VM_SLOTACCEPTOR_H

#include "hermes/VM/Handle.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/SmallHermesValue.h"
#include "hermes/VM/SymbolID.h"

namespace hermes {
namespace vm {

class GCPointerBase;
class HeapSnapshot;
class WeakRootBase;
class WeakRootSymbolID;
class GCCell;
class WeakSmallHermesValue;

struct RootSectionAcceptor {
  virtual ~RootSectionAcceptor() = default;

  enum class Section {
#define ROOT_SECTION(name) name,
#include "hermes/VM/RootSections.def"
    NumSections,
    // Sentinel value to be used to represent an invalid section.
    InvalidSection,
  };

  virtual void beginRootSection(Section section) {}
  virtual void endRootSection() {}
};

struct RootAcceptor : public RootSectionAcceptor {
  virtual void accept(GCCell *&ptr) = 0;
  virtual void accept(PinnedHermesValue &hv) = 0;
  /// Same as the above, but allows the HermesValue to store a nullptr value.
  virtual void acceptNullable(PinnedHermesValue &hv) = 0;
  /// Accept a PinnedSmallHermesValue root. SmallHermesValue cannot encode a
  /// null pointer, so there is no nullable variant.
  virtual void accept(PinnedSmallHermesValue &shv) = 0;
  virtual void accept(const RootSymbolID &sym) = 0;
  template <typename T>
  void acceptNullablePV(PinnedValue<T> &pv) {
    // If \p pv actually holds an encoded SmallHermesValue, we need to decode it
    // first, then write it back after visiting.
    if constexpr (std::is_same_v<T, SmallHermesValue>) {
      PinnedSmallHermesValue pshv = pv.getSmallHermesValue();
      assert(
          (!pshv.isPointer() || pshv.getPointer()) &&
          "The held SHV can't be null pointer");
      // The held SHV can't be null pointer.
      accept(pshv);
      pv = pshv;
    } else {
      acceptNullable(pv);
    }
  }

  /// When we want to call an acceptor on "raw" root pointers of
  /// some JSObject subtype T, this method does the necessary
  /// reinterpret_cast to allow us to call the "GCCell *&" accept
  /// method above.
  template <typename T>
  void acceptPtr(T *&ptr) {
    accept(reinterpret_cast<GCCell *&>(ptr));
  }
};

struct RootAcceptorWithNames : public RootAcceptor {
  void accept(GCCell *&ptr) final {
    accept(ptr, nullptr);
  }
  virtual void accept(GCCell *&ptr, const char *name) = 0;

  void accept(PinnedHermesValue &hv) final {
    accept(hv, nullptr);
  }
  void acceptNullable(PinnedHermesValue &hv) final {
    acceptNullable(hv, nullptr);
  }
  void accept(PinnedSmallHermesValue &shv) final {
    accept(shv, nullptr);
  }
  virtual void accept(PinnedHermesValue &hv, const char *name) = 0;
  virtual void acceptNullable(PinnedHermesValue &hv, const char *name) = 0;
  virtual void accept(PinnedSmallHermesValue &shv, const char *name) = 0;

  void accept(const RootSymbolID &sym) final {
    accept(sym, nullptr);
  }
  virtual void accept(const RootSymbolID &sym, const char *name) = 0;

  using RootAcceptor::acceptPtr;
  template <typename T>
  void acceptPtr(T *&ptr, const char *name) {
    accept(reinterpret_cast<GCCell *&>(ptr), name);
  }

  /// Initiate the callback if this acceptor is part of heap snapshots.
  virtual void provideSnapshot(
      const std::function<void(HeapSnapshot &)> &func) {}
};

/// Acceptor for weak roots. Implementations must do nothing for a null
/// WeakRootBase or an invalid WeakRootSymbolID, so callers may skip such roots
/// instead of making a virtual call for each of them.
struct WeakRootAcceptor : RootSectionAcceptor {
  ~WeakRootAcceptor() override = default;

  /// NOTE: This is called acceptWeak in order to avoid clashing with accept
  /// from SlotAcceptor, for classes that inherit from both.
  virtual void acceptWeak(WeakRootBase &ptr) = 0;

  virtual void acceptWeakSym(WeakRootSymbolID &sym) = 0;

  virtual void acceptWeak(WeakSmallHermesValue &wshv) = 0;
};

template <typename Acceptor>
struct DroppingAcceptor final : public RootAcceptorWithNames {
  static_assert(
      std::is_base_of<RootAcceptor, Acceptor>::value,
      "Can only use this with a subclass of RootAcceptor");
  Acceptor &acceptor;

  explicit DroppingAcceptor(Acceptor &acceptor) : acceptor(acceptor) {}

  using RootAcceptorWithNames::accept;

  void accept(GCCell *&ptr, const char *) override {
    acceptor.accept(ptr);
  }

  void accept(PinnedHermesValue &hv, const char *) override {
    acceptor.accept(hv);
  }
  void acceptNullable(PinnedHermesValue &hv, const char *) override {
    acceptor.acceptNullable(hv);
  }
  void accept(PinnedSmallHermesValue &shv, const char *) override {
    acceptor.accept(shv);
  }

  void accept(const RootSymbolID &sym, const char *) override {
    acceptor.accept(sym);
  }
};

} // namespace vm
} // namespace hermes

#endif
