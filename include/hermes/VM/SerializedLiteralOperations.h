/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_SERIALIZEDLITERALOPERATIONS_H
#define HERMES_VM_SERIALIZEDLITERALOPERATIONS_H

#include "hermes/BCGen/SerializedLiteralParser.h"
#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Runtime.h"

#include "llvh/ADT/ArrayRef.h"

namespace hermes {
namespace vm {

/// Add the keys in \p buffer to the hidden class \p clazz, and return the
/// resulting hidden class.
/// \param buffer the key buffer starting at the first key to add.
/// \param numProps the number of keys to add.
/// \param rootClazz the root hidden class to add the keys to. It must not
///        already contain any of the keys in \p buffer.
/// \param getSym a function to convert a StringID to a SymbolID.
template <typename F>
HiddenClass *addBufferPropertiesToHiddenClass(
    Runtime &runtime,
    llvh::ArrayRef<uint8_t> buffer,
    uint32_t numProps,
    HiddenClass *rootClazz,
    F getSym) {
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<> tmpKey;
    PinnedValue<SymbolID> tmpSym;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.clazz = rootClazz;

  // Set up the visitor to populate keys in the hidden class.
  struct {
    void visitStringID(StringID id) {
      SymbolID sym = getSym(id);
      auto addResult = HiddenClass::addProperty(
          clazz,
          runtime,
          sym,
          PropertyFlags::defaultNewNamedPropertyFlags(),
          MutableHandle<HiddenClass>(clazz));
      runtime.ignoreAllocationFailure(addResult);
    }
    void visitPrivateName() {
      hermes_fatal("private string IDs unimplemented");
    }
    void visitNumber(double d) {
      tmpHandleKey = HermesValue::encodeTrustedNumberValue(d);
      // valueToSymbolID cannot fail because the key is known to be uint32.
      runtime.ignoreAllocationFailure(
          valueToSymbolID(runtime, tmpHandleKey, tmpSym));
      auto addResult = HiddenClass::addProperty(
          clazz,
          runtime,
          *tmpSym,
          PropertyFlags::defaultNewNamedPropertyFlags(),
          MutableHandle<HiddenClass>(clazz));
      runtime.ignoreAllocationFailure(addResult);
    }

    PinnedValue<HiddenClass> &clazz;
    PinnedValue<> &tmpHandleKey;
    PinnedValue<SymbolID> &tmpSym;
    Runtime &runtime;
    F getSym;
  } v{lv.clazz, lv.tmpKey, lv.tmpSym, runtime, std::move(getSym)};

  // Visit each literal in the buffer and add it as a property.
  SerializedLiteralParser::parseKeyBuffer(buffer, numProps, v);

  return *lv.clazz;
}

/// Add the keys in \p buffer to the hidden class \p clazz.
/// \param buffer the key buffer starting at the first key to add.
/// \param numProps the number of keys to add.
/// \param clazz the typed HiddenClass to add the keys to. It must
///   be for a typed object.
/// \param propertiesEnumerable if false, properties will not be enumerable.
/// \param getSym a function to convert a StringID to a SymbolID.
template <typename F>
void addTypedBufferPropertiesToHiddenClass(
    Runtime &runtime,
    llvh::ArrayRef<uint8_t> buffer,
    uint32_t numProps,
    Handle<HiddenClass> clazz,
    bool propertiesEnumerable,
    F getSym) {
  struct : public Locals {
    PinnedValue<> tmpKey;
    PinnedValue<SymbolID> tmpSym;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Set up the visitor to populate keys in the hidden class.
  struct {
    void visitStringID(StringID id) {
      auto sym = getSym(id);
      HiddenClass::addNewTypedPublicProperty(
          clazz, runtime, sym, propertiesEnumerable);
    }
    void visitPrivateName() {
      HiddenClass::addNewTypedPrivateProperty(clazz, runtime);
    }
    void visitNumber(double d) {
      tmpHandleKey = HermesValue::encodeTrustedNumberValue(d);
      // valueToSymbolID cannot fail because the key is known to be uint32.
      runtime.ignoreAllocationFailure(
          valueToSymbolID(runtime, tmpHandleKey, tmpSym));
      HiddenClass::addNewTypedPublicProperty(
          clazz, runtime, *tmpSym, propertiesEnumerable);
    }

    Handle<HiddenClass> clazz;
    PinnedValue<> &tmpHandleKey;
    PinnedValue<SymbolID> &tmpSym;
    Runtime &runtime;
    bool propertiesEnumerable;
    F getSym;
  } v{clazz,
      lv.tmpKey,
      lv.tmpSym,
      runtime,
      propertiesEnumerable,
      std::move(getSym)};

  assert(clazz->isTyped() && "typed objects require a typed HiddenClass");

  SerializedLiteralParser::parseKeyBuffer(buffer, numProps, v);
}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_SERIALIZEDLITERALOPERATIONS_H
