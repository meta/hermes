/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "class"
#include "hermes/VM/HiddenClass.h"

#include "hermes/Support/Statistic.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/HiddenClass-inline.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/StringView.h"

#include "llvh/Support/Debug.h"

using llvh::dbgs;

HERMES_SLOW_STATISTIC(
    NumHCInitPropMap,
    "NumHCInitPropMap: Number of HiddenClass map initializations.");
HERMES_SLOW_STATISTIC(
    NumHCMapSteal,
    "NumHCMapSteal: Number of map steals from parent HiddenClass.");
HERMES_SLOW_STATISTIC(
    NumHCObjectParentChanges,
    "NumHCObjectParentChanges: Number of HiddenClass ObjectParent change slow "
    "path calls.");

namespace hermes {
namespace vm {

namespace detail {

#ifdef HERMES_MEMORY_INSTRUMENTATION
void TransitionMap::snapshotAddNodes(GC &gc, HeapSnapshot &snap) {
  if (!isLarge()) {
    return;
  }
  // Make one node that is the sum of the sizes of the WeakValueMap and the
  // llvh::DenseMap to which it points.
  // This is based on the assumption that the WeakValueMap uniquely owns that
  // DenseMap.
  snap.beginNode();
  snap.endNode(
      HeapSnapshot::NodeType::Native,
      "WeakValueMap",
      gc.getNativeID(large()),
      getMemorySize(),
      0);
}

void TransitionMap::snapshotAddEdges(GC &gc, HeapSnapshot &snap) {
  uint32_t edge_index = 0;
  forEachEntry([&snap, &gc, &edge_index](
                   const Transition &, const WeakRef<HiddenClass> &value) {
    // Filter out empty refs from adding edges.
    if (value.isValid()) {
      snap.addNamedEdge(
          HeapSnapshot::EdgeType::Weak,
          std::to_string(edge_index++),
          gc.getObjectID(value.getNoBarrierUnsafe(gc.getPointerBase())));
    }
  });

  if (!isLarge()) {
    return;
  }
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Internal,
      "transitionMap",
      gc.getNativeID(large()));
}

void TransitionMap::snapshotUntrackMemory(GC &gc) {
  // Untrack the memory ID in case one was created.
  if (isLarge()) {
    gc.getIDTracker().untrackNative(large());
  }
}
#endif

size_t TransitionMap::getMemorySize() const {
  // Inline slot is not counted here (it counts as part of the HiddenClass).
  return isLarge() ? sizeof(*large()) + large()->getMemorySize() : 0;
}

void TransitionMap::uncleanMakeLarge(Runtime &runtime) {
  assert(!isClean() && "must not still be clean");
  assert(!isLarge() && "must not yet be large");
  auto large = new WeakValueMap<Transition, HiddenClass>();
  // Move any valid entry into the allocated map.
  if (auto value = smallValue().get(runtime))
    large->insertNew(runtime, smallKey_, runtime.makeHandle(value));
  smallValue().releaseSlot();
  u.large_ = large;
  smallKey_.symbolID = SymbolID::deleted();
  assert(isLarge());
}

} // namespace detail

const VTable HiddenClass::vt{
    CellKind::HiddenClassKind,
    cellSize<HiddenClass>(),
    /* allowLargeAlloc */ false,
    _finalizeImpl,
    _mallocSizeImpl,
    nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
    ,
    VTable::HeapSnapshotMetadata{
        HeapSnapshot::NodeType::Object,
        HiddenClass::_snapshotNameImpl,
        HiddenClass::_snapshotAddEdgesImpl,
        HiddenClass::_snapshotAddNodesImpl,
        nullptr}
#endif
};

void HiddenClassBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const HiddenClass *>(cell);
  mb.setVTable(&HiddenClass::vt);
  mb.addField("objectParent", &self->objectParent_);
  mb.addField("symbol", &self->symbolID_);
  mb.addField("parent", &self->parent_);
  mb.addField("propertyMap", &self->propertyMap_);
  mb.addField("forInCache", &self->forInCache_);
}

void HiddenClass::_finalizeImpl(GCCell *cell, GC &gc) {
  auto *self = vmcast<HiddenClass>(cell);
#ifdef HERMES_MEMORY_INSTRUMENTATION
  self->transitionMap_.snapshotUntrackMemory(gc);
#endif
  self->~HiddenClass();
}

size_t HiddenClass::_mallocSizeImpl(GCCell *cell) {
  auto *self = vmcast<HiddenClass>(cell);
  size_t size = self->transitionMap_.getMemorySize();
  if (self->parentTransitionMap_)
    size += sizeof(WeakValueObjectMap<HiddenClass>) +
        self->parentTransitionMap_->getMemorySize();
  return size;
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
std::string HiddenClass::_snapshotNameImpl(GCCell *cell, GC &gc) {
  auto *const self = vmcast<HiddenClass>(cell);
  std::string name{cell->getVT()->snapshotMetaData.defaultNameForNode(self)};
  if (self->isDictionary()) {
    return name + "(Dictionary)";
  }
  return name;
}

void HiddenClass::_snapshotAddEdgesImpl(
    GCCell *cell,
    GC &gc,
    HeapSnapshot &snap) {
  auto *const self = vmcast<HiddenClass>(cell);
  self->transitionMap_.snapshotAddEdges(gc, snap);
  if (self->parentTransitionMap_)
    self->parentTransitionMap_->snapshotAddEdges(gc, snap, "parentTransition");
}

void HiddenClass::_snapshotAddNodesImpl(
    GCCell *cell,
    GC &gc,
    HeapSnapshot &snap) {
  auto *const self = vmcast<HiddenClass>(cell);
  self->transitionMap_.snapshotAddNodes(gc, snap);
}
#endif

HiddenClass *HiddenClass::createRoot(
    Runtime &runtime,
    Handle<JSObject> objectParent,
    ClassFlags flags) {
  return create(
      runtime,
      flags,
      Runtime::makeNullHandle<HiddenClass>(),
      objectParent,
      SymbolID{},
      PropertyFlags{},
      0);
}

HiddenClass *HiddenClass::createForTypedObject(
    Runtime &runtime,
    Handle<JSObject> objectParent,
    uint32_t capacity) {
  assert(capacity <= maxNumProperties() && "too many properties");

  struct : public Locals {
    PinnedValue<HiddenClass> self;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Typed objects are frozen from the perspective of untyped code. Adding the
  // buffer properties afterwards is a class-level operation that ignores
  // noExtend.
  ClassFlags flags{};
  flags.typed = 1;
  flags.noExtend = 1;
  flags.sealed = 1;
  flags.frozen = 1;

  /// Allocate a new class without a parent.
  /// Start with 0 properties, they'll be added later by the caller of
  /// createForTypedObject.
  lv.self = HiddenClass::create(
      runtime,
      flags,
      Runtime::makeNullHandle<HiddenClass>(),
      objectParent,
      SymbolID{},
      PropertyFlags{},
      0);

  PseudoHandle<DictPropertyMap> dictRes = runtime.ignoreAllocationFailure(
      DictPropertyMap::create(runtime, capacity));

  lv.self->propertyMap_.set(runtime, dictRes.get(), runtime.getHeap());
  return lv.self.get();
}

HiddenClass *HiddenClass::create(
    Runtime &runtime,
    ClassFlags flags,
    Handle<HiddenClass> parent,
    Handle<JSObject> objectParent,
    SymbolID symbolID,
    PropertyFlags propertyFlags,
    unsigned numProperties) {
  assert(
      (flags.dictionaryMode || numProperties == 0 || *parent) &&
      "non-empty non-dictionary orphan");
  auto *obj =
      runtime.makeAFixed<HiddenClass, HasFinalizer::Yes, LongLived::Yes>(
          runtime,
          flags,
          parent,
          objectParent,
          symbolID,
          propertyFlags,
          numProperties);
  return obj;
}

Handle<HiddenClass> HiddenClass::copyToNewDictionary(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    bool noCache) {
  assert(
      !selfHandle->isDictionaryNoCache() && "class already in no-cache mode");

  struct : public Locals {
    PinnedValue<JSObject> objectParent;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto newFlags = selfHandle->flags_;
  newFlags.dictionaryMode = true;
  // If the requested, transition to no-cache mode.
  if (noCache) {
    newFlags.dictionaryNoCacheMode = true;
  }

  /// Allocate a new class without a parent.
  lv.objectParent = selfHandle->objectParent_.get(runtime);
  auto newClassHandle = runtime.makeHandle<HiddenClass>(HiddenClass::create(
      runtime,
      newFlags,
      Runtime::makeNullHandle<HiddenClass>(),
      lv.objectParent,
      SymbolID{},
      PropertyFlags{},
      selfHandle->numProperties_));

  // Optionally allocate the property map and move it to the new class.
  if (LLVM_UNLIKELY(!selfHandle->propertyMap_))
    initializeMissingPropertyMap(selfHandle, runtime);

  newClassHandle->propertyMap_.set(
      runtime, selfHandle->propertyMap_, runtime.getHeap());
  selfHandle->propertyMap_.setNull(runtime.getHeap());

  LLVM_DEBUG(
      dbgs() << "Converted Class:" << selfHandle->getDebugAllocationId()
             << " to dictionary Class:"
             << newClassHandle->getDebugAllocationId() << "\n");

  return newClassHandle;
}

void HiddenClass::forEachPropertyNoAlloc(
    HiddenClass *self,
    PointerBase &base,
    std::function<void(SymbolID, NamedPropertyDescriptor)> callback) {
  std::vector<std::pair<SymbolID, NamedPropertyDescriptor>> properties;
  HiddenClass *curr = self;
  while (curr && !curr->propertyMap_) {
    // Skip invalid symbols stored in the hidden class chain, as well as
    // flag-only transitions.
    if (curr->symbolID_.isValid() && !curr->propertyFlags_.flagsTransition) {
      properties.emplace_back(
          curr->symbolID_,
          NamedPropertyDescriptor{
              curr->propertyFlags_, curr->numProperties_ - 1});
    }
    curr = curr->parent_.get(base);
  }

  // Either we reached the root hidden class and never found a property
  // map, or we found a property map somewhere in the HiddenClass chain.
  if (curr) {
    assert(
        curr->propertyMap_ &&
        "If it's not the root class, it must have a property map");
    // The DPM exists, and it can be iterated over to find some properties.
    DictPropertyMap::forEachPropertyNoAlloc(
        curr->propertyMap_.getNonNull(base), callback);
  }
  // Add any iterated properties at the end.
  // Since we moved backwards through HiddenClasses, the properties are in
  // reverse order. Iterate backwards through properties to get the original
  // order.
  for (auto it = properties.rbegin(); it != properties.rend(); ++it) {
    callback(it->first, it->second);
  }
}

OptValue<HiddenClass::PropertyPos> HiddenClass::findPropertyNoMap(
    PseudoHandle<HiddenClass> self,
    Runtime &runtime,
    SymbolID name,
    NamedPropertyDescriptor &desc) {
  assert(!self->propertyMap_ && "property map must not exist");
  auto selfHandle = runtime.makeHandle(std::move(self));
  initializeMissingPropertyMap(selfHandle, runtime);

  auto *propMap = selfHandle->propertyMap_.getNonNull(runtime);
  {
    // propMap is a raw pointer.  We assume that find does no allocation.
    NoAllocScope noAlloc(runtime);
    auto found = DictPropertyMap::find(propMap, name);
    if (!found)
      return llvh::None;
    // Technically, the last use of propMap occurs before the call here, so
    // it would be legal for the call to allocate.  If that were ever the case,
    // we would move "found" out of scope, and terminate the NoAllocScope here.
    desc = DictPropertyMap::getDescriptorPair(propMap, *found)->second;
    return *found;
  }
}

llvh::Optional<NamedPropertyDescriptor> HiddenClass::findPropertyNoAlloc(
    HiddenClass *self,
    PointerBase &base,
    SymbolID name) {
  // The flags of the newest flags transition of \p name seen so far. A flags
  // transition keeps the property's slot, so the slot still has to come from
  // the class that added the property, or from a map.
  llvh::Optional<PropertyFlags> newestFlags;
  for (HiddenClass *curr = self; curr; curr = curr->parent_.get(base)) {
    if (curr->propertyMap_) {
      // If a property map exists, just search this hidden class
      auto found =
          DictPropertyMap::find(curr->propertyMap_.getNonNull(base), name);
      if (found) {
        NamedPropertyDescriptor desc =
            DictPropertyMap::getDescriptorPair(
                curr->propertyMap_.getNonNull(base), *found)
                ->second;
        if (newestFlags)
          desc.flags = *newestFlags;
        return desc;
      }
    }
    // Else, no property map exists. Check the current hidden class before
    // moving up.
    if (curr->symbolID_ == name) {
      if (curr->propertyFlags_.flagsTransition) {
        if (!newestFlags) {
          newestFlags = curr->propertyFlags_;
          newestFlags->flagsTransition = 0;
        }
        continue;
      }
      return NamedPropertyDescriptor{
          newestFlags ? *newestFlags : curr->propertyFlags_,
          curr->numProperties_ - 1};
    }
  }
  // Reached the root hidden class without finding a property map or the
  // matching symbol, this property doesn't exist.
  return llvh::None;
}

bool HiddenClass::debugIsPropertyDefined(
    HiddenClass *self,
    PointerBase &base,
    SymbolID name) {
  do {
    // If we happen to have a property map, use it.
    if (self->propertyMap_)
      return DictPropertyMap::find(self->propertyMap_.getNonNull(base), name)
          .hasValue();
    // Is the property defined in this class?
    if (self->symbolID_ == name)
      return true;
    self = self->parent_.get(base);
  } while (self);
  return false;
}

Handle<HiddenClass> HiddenClass::deleteProperty(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    PropertyPos pos) {
  // We convert to dictionary if we're not yet a dictionary
  // (transition to a cacheable dictionary), or if we are, but not yet
  // in no-cache mode (transition to no-cache mode).
  auto newHandle = LLVM_UNLIKELY(!selfHandle->isDictionaryNoCache())
      ? copyToNewDictionary(selfHandle, runtime, selfHandle->isDictionary())
      : selfHandle;

  --newHandle->numProperties_;

  DictPropertyMap::erase(newHandle->propertyMap_.get(runtime), runtime, pos);

  LLVM_DEBUG(
      dbgs() << "Deleting from Class:" << selfHandle->getDebugAllocationId()
             << " produces Class:" << newHandle->getDebugAllocationId()
             << "\n");

  return newHandle;
}

CallResult<std::pair<Handle<HiddenClass>, SlotIndex>> HiddenClass::addProperty(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    SymbolID name,
    PropertyFlags propertyFlags) {
  assert(propertyFlags.isValid() && "propertyFlags must be valid");

  if (LLVM_UNLIKELY(selfHandle->isDictionary())) {
    auto isIndexLike =
        toArrayIndex(runtime.getIdentifierTable().getStringView(runtime, name))
            .hasValue();
    selfHandle->flags_ =
        computeFlags(selfHandle->flags_, propertyFlags, isIndexLike);

    // Allocate a new slot.
    // TODO: this changes the property map, so if we want to support OOM
    // handling in the future, and the following operation fails, we would have
    // to somehow be able to undo it, or use an approach where we peek the slot
    // but not consume until we are sure (which is less efficient, but more
    // robust). T31555339.
    SlotIndex newSlot = DictPropertyMap::allocatePropertySlot(
        selfHandle->propertyMap_.getNonNull(runtime), runtime);

    if (LLVM_UNLIKELY(
            addToPropertyMap(
                selfHandle,
                runtime,
                name,
                NamedPropertyDescriptor(propertyFlags, newSlot)) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    ++selfHandle->numProperties_;
    return std::make_pair(selfHandle, newSlot);
  }

  // Do we already have a transition for that property+flags pair?
  auto existingChild =
      selfHandle->transitionMap_.lookup(runtime, {name, propertyFlags});
  if (LLVM_LIKELY(existingChild)) {
    LLVM_DEBUG(
        dbgs() << "Adding property " << runtime.formatSymbolID(name)
               << " to Class:" << selfHandle->getDebugAllocationId()
               << " transitions to existing Class:"
               << existingChild->getDebugAllocationId() << "\n");

    return std::make_pair(
        runtime.makeHandle(existingChild), selfHandle->numProperties_);
  }

  // Do we need to convert to dictionary?
  if (LLVM_UNLIKELY(selfHandle->numProperties_ == kDictionaryThreshold)) {
    // Do it.
    auto childHandle = copyToNewDictionary(selfHandle, runtime);

    auto isIndexLike =
        toArrayIndex(runtime.getIdentifierTable().getStringView(runtime, name))
            .hasValue();
    childHandle->flags_ =
        computeFlags(childHandle->flags_, propertyFlags, isIndexLike);

    // Add the property to the child.
    if (LLVM_UNLIKELY(
            addToPropertyMap(
                childHandle,
                runtime,
                name,
                NamedPropertyDescriptor(
                    propertyFlags, childHandle->numProperties_)) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return std::make_pair(childHandle, childHandle->numProperties_++);
  }

  auto isIndexLike =
      toArrayIndex(runtime.getIdentifierTable().getStringView(runtime, name))
          .hasValue();
  auto newFlags = computeFlags(selfHandle->flags_, propertyFlags, isIndexLike);

  struct : public Locals {
    PinnedValue<JSObject> objectParent;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Allocate the child.
  lv.objectParent = selfHandle->objectParent_.get(runtime);
  auto childHandle = runtime.makeHandle<HiddenClass>(HiddenClass::create(
      runtime,
      newFlags,
      selfHandle,
      lv.objectParent,
      name,
      propertyFlags,
      selfHandle->numProperties_ + 1));

  // Add it to the transition table.
  auto inserted = selfHandle->transitionMap_.insertNew(
      runtime, Transition(name, propertyFlags), childHandle);
  (void)inserted;
  assert(
      inserted &&
      "transition already exists when adding a new property to hidden class");

  if (selfHandle->propertyMap_) {
    assert(
        !DictPropertyMap::find(
            selfHandle->propertyMap_.getNonNull(runtime), name) &&
        "Adding an existing property to hidden class");

    LLVM_DEBUG(
        dbgs() << "Adding property " << runtime.formatSymbolID(name)
               << " to Class:" << selfHandle->getDebugAllocationId()
               << " transitions Map to new Class:"
               << childHandle->getDebugAllocationId() << "\n");

    // Move the map to the child class.
    childHandle->propertyMap_.set(
        runtime, selfHandle->propertyMap_, runtime.getHeap());
    selfHandle->propertyMap_.setNull(runtime.getHeap());

    if (LLVM_UNLIKELY(
            addToPropertyMap(
                childHandle,
                runtime,
                name,
                NamedPropertyDescriptor(
                    propertyFlags, selfHandle->numProperties_)) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    LLVM_DEBUG(
        dbgs() << "Adding property " << runtime.formatSymbolID(name)
               << " to Class:" << selfHandle->getDebugAllocationId()
               << " transitions to new Class:"
               << childHandle->getDebugAllocationId() << "\n");
  }

  return std::make_pair(childHandle, selfHandle->numProperties_);
}

Handle<HiddenClass> HiddenClass::updateProperty(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    PropertyPos pos,
    PropertyFlags newFlags) {
  assert(newFlags.isValid() && "newFlags must be valid");

  // In dictionary mode we simply update our map (which must exist).
  if (LLVM_UNLIKELY(selfHandle->isDictionary())) {
    assert(
        selfHandle->propertyMap_ &&
        "propertyMap must exist in dictionary mode");
    selfHandle->flags_ = computeFlags(selfHandle->flags_, newFlags, false);
    DictPropertyMap::getDescriptorPair(
        selfHandle->propertyMap_.getNonNull(runtime), pos)
        ->second.flags = newFlags;
    // If it's still cacheable, make it non-cacheable.
    if (!selfHandle->isDictionaryNoCache()) {
      selfHandle = copyToNewDictionary(selfHandle, runtime, /*noCache*/ true);
    }
    return selfHandle;
  }

  assert(
      selfHandle->propertyMap_ && "propertyMap must exist in updateProperty()");

  auto *descPair = DictPropertyMap::getDescriptorPair(
      selfHandle->propertyMap_.getNonNull(runtime), pos);
  // If the property flags didn't change, do nothing.
  if (descPair->second.flags == newFlags)
    return selfHandle;

  auto name = descPair->first;

  // The transition flags must indicate that it is a "flags transition".
  PropertyFlags transitionFlags = newFlags;
  transitionFlags.flagsTransition = 1;

  // Do we already have a transition for that property+flags pair?
  auto existingChild =
      selfHandle->transitionMap_.lookup(runtime, {name, transitionFlags});
  if (LLVM_LIKELY(existingChild)) {
    // If the child doesn't have a property map, but we do, update our map and
    // move it to the child.
    if (!existingChild->propertyMap_) {
      LLVM_DEBUG(
          dbgs() << "Updating property " << runtime.formatSymbolID(name)
                 << " in Class:" << selfHandle->getDebugAllocationId()
                 << " transitions Map to existing Class:"
                 << existingChild->getDebugAllocationId() << "\n");

      descPair->second.flags = newFlags;
      existingChild->propertyMap_.set(
          runtime, selfHandle->propertyMap_, runtime.getHeap());
    } else {
      LLVM_DEBUG(
          dbgs() << "Updating property " << runtime.formatSymbolID(name)
                 << " in Class:" << selfHandle->getDebugAllocationId()
                 << " transitions to existing Class:"
                 << existingChild->getDebugAllocationId() << "\n");
    }

    // In any case, clear our own map.
    selfHandle->propertyMap_.setNull(runtime.getHeap());

    return runtime.makeHandle(existingChild);
  }

  struct : public Locals {
    PinnedValue<JSObject> objectParent;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // We are updating the existing property and adding a transition to a new
  // hidden class.
  descPair->second.flags = newFlags;

  // Allocate the child.
  lv.objectParent = selfHandle->objectParent_.get(runtime);
  auto childHandle = runtime.makeHandle<HiddenClass>(HiddenClass::create(
      runtime,
      computeFlags(selfHandle->flags_, newFlags, false),
      selfHandle,
      lv.objectParent,
      name,
      transitionFlags,
      selfHandle->numProperties_));

  // Add it to the transition table.
  auto inserted = selfHandle->transitionMap_.insertNew(
      runtime, Transition(name, transitionFlags), childHandle);
  (void)inserted;
  assert(
      inserted &&
      "transition already exists when updating a property in hidden class");

  LLVM_DEBUG(
      dbgs() << "Updating property " << runtime.formatSymbolID(name)
             << " in Class:" << selfHandle->getDebugAllocationId()
             << " transitions Map to new Class:"
             << childHandle->getDebugAllocationId() << "\n");

  // Move the updated map to the child class.
  childHandle->propertyMap_.set(
      runtime, selfHandle->propertyMap_, runtime.getHeap());
  selfHandle->propertyMap_.setNull(runtime.getHeap());

  return childHandle;
}

SlotIndex HiddenClass::addNewTypedPublicProperty(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    SymbolID name,
    bool propertiesEnumerable) {
  assert(!selfHandle->parent_ && "must be root hidden class");
  assert(selfHandle->flags_.typed && "must be typed");
  assert(selfHandle->propertyMap_ && "must have property map allocated");

  struct : public Locals {
    PinnedValue<DictPropertyMap> propertyMap;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Typed properties are not configurable or writable from untyped code.
  PropertyFlags propFlags = PropertyFlags::defaultNewNamedPropertyFlags();
  propFlags.writable = false;
  propFlags.configurable = false;
  if (!propertiesEnumerable)
    propFlags.enumerable = false;

  auto isIndexLike =
      toArrayIndex(runtime.getIdentifierTable().getStringView(runtime, name))
          .hasValue();
  selfHandle->flags_ = computeFlags(selfHandle->flags_, propFlags, isIndexLike);

  SlotIndex newSlot = selfHandle->numProperties_++;
  addToPropertyMap(
      selfHandle, runtime, name, NamedPropertyDescriptor(propFlags, newSlot));

  return newSlot;
}

HiddenClass *HiddenClass::updateClassFlags(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    ClassFlags newFlags) {
  if (selfHandle->flags_ == newFlags) {
    // Nothing to do, return immediately.
    return *selfHandle;
  }

  // A typed class is a shared orphan whose property map cannot be rebuilt from
  // a transition chain, so a child of it would steal the map and leave every
  // sibling object broken. Typed objects are marked non-extensible and frozen
  // at construction, so every caller short-circuits above.
  assert(!selfHandle->isTyped() && "typed classes cannot change flags");
  // Only the state flags may be updated here. The dictionary flags describe the
  // structure of the class, and changing them would produce a class that claims
  // to be a dictionary without a property map (or vice versa).
  assert(
      newFlags.dictionaryMode == selfHandle->flags_.dictionaryMode &&
      newFlags.dictionaryNoCacheMode ==
          selfHandle->flags_.dictionaryNoCacheMode &&
      "updateClassFlags cannot change dictionary mode");

  // In dictionary mode we simply update our map (which must exist).
  if (LLVM_UNLIKELY(selfHandle->isDictionary())) {
    assert(
        selfHandle->propertyMap_ &&
        "propertyMap must exist in dictionary mode");
    selfHandle->flags_ = newFlags;
    // If it's still cacheable, make it non-cacheable.
    if (!selfHandle->isDictionaryNoCache()) {
      selfHandle = copyToNewDictionary(selfHandle, runtime, /*noCache*/ true);
    }
    return *selfHandle;
  }

  // Check whether we have already cached the final state.
  assert(!selfHandle->isDictionary());
  if (auto *hc = selfHandle->transitionMap_.lookup(
          runtime,
          Transition(
              getSymbolID(Predefined::InternalPropertyFlagsTransition),
              newFlags))) {
    return hc;
  }

  // Otherwise, update ClassFlags by making a new HiddenClass.

  struct : public Locals {
    PinnedValue<JSObject> objectParent;
    PinnedValue<HiddenClass> newClazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.objectParent = selfHandle->getObjectParent(runtime);

  lv.newClazz = HiddenClass::create(
      runtime,
      newFlags,
      selfHandle,
      lv.objectParent,
      SymbolID::empty(),
      PropertyFlags{},
      selfHandle->numProperties_);

  // Cache the transition to the final class.
  selfHandle->transitionMap_.insertNew(
      runtime,
      Transition(
          getSymbolID(Predefined::InternalPropertyFlagsTransition), newFlags),
      lv.newClazz);

  return *lv.newClazz;
}

JSObject *WeakValueObjectMapKey<HiddenClass>::get(
    HiddenClass *clazz,
    Runtime &runtime) {
  assert(!clazz->isDictionary() && "dictionaries are never transition targets");
  return clazz->getObjectParent(runtime);
}

HiddenClass *HiddenClass::updateObjectParentSlowPath(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    PseudoHandle<JSObject> newParentPH) {
  ++NumHCObjectParentChanges;
  assert(
      newParentPH.get() != selfHandle->objectParent_.get(runtime) &&
      "don't call slow path if the parent isn't changing");
  // A typed class is a shared orphan whose property map cannot be rebuilt from
  // a transition chain, so a child of it would steal the map and leave every
  // sibling object broken. Typed objects are non-extensible, so setParent
  // rejects them before we get here.
  assert(!selfHandle->isTyped() && "typed classes cannot change parent");

  // In dictionary mode, update the objectParent directly without making a new
  // HiddenClass.
  if (LLVM_UNLIKELY(selfHandle->isDictionary())) {
    selfHandle->objectParent_.set(
        runtime, newParentPH.get(), runtime.getHeap());
    return *selfHandle;
  }

  struct : public Locals {
    PinnedValue<HiddenClass> child;
    PinnedValue<JSObject> newParent;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.newParent = std::move(newParentPH);

  // Check if incrementing parentChangeCounter would exceed the maximum.
  constexpr uint8_t maxCounter =
      (1 << ClassFlags::kParentChangeCounterSize) - 1;
  if (LLVM_UNLIKELY(selfHandle->flags_.parentChangeCounter >= maxCounter)) {
    // Convert to dictionary mode and update the objectParent directly.
    Handle<HiddenClass> dictionaryHandle =
        copyToNewDictionary(selfHandle, runtime);
    dictionaryHandle->objectParent_.set(
        runtime, *lv.newParent, runtime.getHeap());
    return *dictionaryHandle;
  }

  // Check for existing transition in parentTransitionMap_.
  if (!selfHandle->parentTransitionMap_) {
    selfHandle->parentTransitionMap_ =
        std::make_unique<WeakValueObjectMap<HiddenClass>>();
  } else if (
      HiddenClass *existingChild =
          selfHandle->parentTransitionMap_->lookup(runtime, *lv.newParent)) {
    return existingChild;
  }

  // Create new flags with incremented parentChangeCounter
  ClassFlags newClassFlags = selfHandle->flags_;
  ++newClassFlags.parentChangeCounter;

  // Create a new HiddenClass with the new objectParent.
  lv.child = HiddenClass::create(
      runtime,
      newClassFlags,
      selfHandle,
      lv.newParent,
      SymbolID::empty(),
      PropertyFlags{},
      selfHandle->numProperties_);

  selfHandle->parentTransitionMap_->insert(runtime, *lv.newParent, *lv.child);

  return *lv.child;
}

HiddenClass *HiddenClass::seal(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime) {
  if (!selfHandle->propertyMap_)
    initializeMissingPropertyMap(selfHandle, runtime);

  LLVM_DEBUG(
      dbgs() << "Class:" << selfHandle->getDebugAllocationId()
             << " making all non-configurable\n");

  struct : public Locals {
    PinnedValue<HiddenClass> cur;
    PinnedValue<DictPropertyMap> map;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Keep a handle to our initial map. The order of properties in it will
  // remain the same as long as we are only doing property updates.
  lv.map = selfHandle->propertyMap_.getNonNull(runtime);

  lv.cur = *selfHandle;

  // TODO: this can be made much more efficient at the expense of moving some
  // logic from updateOwnProperty() here.
  DictPropertyMap::forEachProperty(
      lv.map,
      runtime,
      [&runtime, &lv](SymbolID id, NamedPropertyDescriptor desc) {
        if (!desc.flags.configurable)
          return;
        PropertyFlags newFlags = desc.flags;
        newFlags.configurable = 0;

        assert(
            lv.cur->propertyMap_ &&
            "propertyMap must exist after updateOwnProperty()");

        auto found =
            DictPropertyMap::find(lv.cur->propertyMap_.getNonNull(runtime), id);
        assert(found && "property not found during enumeration");
        lv.cur = *updateProperty(lv.cur, runtime, *found, newFlags);
      });

  ClassFlags sealedFlags = lv.cur->flags_;
  sealedFlags.noExtend = 1;
  sealedFlags.sealed = 1;
  lv.cur = HiddenClass::updateClassFlags(lv.cur, runtime, sealedFlags);

  return *lv.cur;
}

HiddenClass *HiddenClass::freeze(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime) {
  // Check whether we have already cached the final state of freezing this
  // class.
  if (LLVM_LIKELY(!selfHandle->isDictionary())) {
    ClassFlags flags = selfHandle->flags_;
    flags.noExtend = 1;
    flags.sealed = 1;
    flags.frozen = 1;
    if (auto *hc = selfHandle->transitionMap_.lookup(
            runtime,
            Transition(
                getSymbolID(Predefined::InternalPropertyFreezeTransition),
                flags))) {
      return hc;
    }
  }

  if (!selfHandle->propertyMap_)
    initializeMissingPropertyMap(selfHandle, runtime);

  LLVM_DEBUG(
      dbgs() << "Class:" << selfHandle->getDebugAllocationId()
             << " making all read-only\n");

  // Keep a handle to our initial map. The order of properties in it will
  // remain the same as long as we are only doing property updates.
  auto mapHandle = runtime.makeHandle(selfHandle->propertyMap_);

  struct : public Locals {
    PinnedValue<HiddenClass> cur;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.cur = *selfHandle;

  // TODO: this can be made much more efficient at the expense of moving some
  // logic from updateOwnProperty() here.
  DictPropertyMap::forEachProperty(
      mapHandle,
      runtime,
      [&runtime, &lv](SymbolID id, NamedPropertyDescriptor desc) {
        PropertyFlags newFlags = desc.flags;
        if (!newFlags.accessor) {
          newFlags.writable = 0;
          newFlags.configurable = 0;
        } else {
          newFlags.configurable = 0;
        }
        if (desc.flags == newFlags)
          return;

        assert(
            lv.cur->propertyMap_ &&
            "propertyMap must exist after updateOwnProperty()");

        auto found =
            DictPropertyMap::find(lv.cur->propertyMap_.get(runtime), id);
        assert(found && "property not found during enumeration");
        lv.cur = *updateProperty(lv.cur, runtime, *found, newFlags);
      });

  ClassFlags frozenFlags = lv.cur->flags_;
  frozenFlags.sealed = 1;
  frozenFlags.noExtend = 1;
  frozenFlags.frozen = 1;
  lv.cur = HiddenClass::updateClassFlags(lv.cur, runtime, frozenFlags);

  if (LLVM_LIKELY(!selfHandle->isDictionary())) {
    // Cache the transition to the final class.
    selfHandle->transitionMap_.insertNew(
        runtime,
        Transition(
            getSymbolID(Predefined::InternalPropertyFreezeTransition),
            frozenFlags),
        lv.cur);
  }

  return *lv.cur;
}

Handle<HiddenClass> HiddenClass::updatePropertyFlagsWithoutTransitions(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    PropertyFlags flagsToClear,
    PropertyFlags flagsToSet,
    OptValue<llvh::ArrayRef<SymbolID>> props) {
  // Result must be in dictionary mode, since it's a non-empty orphan.
  MutableHandle<HiddenClass> classHandle{runtime};
  if (selfHandle->isDictionary()) {
    classHandle = *selfHandle;
  } else {
    classHandle = *copyToNewDictionary(selfHandle, runtime);
  }

  auto mapHandle =
      runtime.makeHandle<DictPropertyMap>(classHandle->propertyMap_);

  auto changeFlags = [&flagsToClear,
                      &flagsToSet](NamedPropertyDescriptor &desc) {
    desc.flags.changeFlags(flagsToClear, flagsToSet);
  };

  // If we have the subset of properties to update, only update them; otherwise,
  // update all properties.
  if (props) {
    // Iterate over the properties that exist on the property map.
    for (auto id : *props) {
      auto pos = DictPropertyMap::find(*mapHandle, id);
      if (!pos) {
        continue;
      }
      auto descPair = DictPropertyMap::getDescriptorPair(*mapHandle, *pos);
      changeFlags(descPair->second);
    }
  } else {
    DictPropertyMap::forEachMutablePropertyDescriptor(
        mapHandle, runtime, changeFlags);
  }
  classHandle->flags_ = computeFlags(classHandle->flags_, flagsToSet, false);

  return std::move(classHandle);
}

CallResult<std::pair<Handle<HiddenClass>, SlotIndex>> HiddenClass::reserveSlot(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime) {
  assert(
      !selfHandle->isDictionary() &&
      "Reserved slots can only be added in class mode");
  SlotIndex index = selfHandle->numProperties_;
  assert(
      index < InternalProperty::NumAnonymousInternalProperties &&
      "Reserved slot index is too large");

  return HiddenClass::addProperty(
      selfHandle,
      runtime,
      InternalProperty::getSymbolID(index),
      PropertyFlags{});
}

bool HiddenClass::areAllNonConfigurable(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime) {
  return forEachPropertyWhile(
      selfHandle,
      runtime,
      [](Runtime &, SymbolID, NamedPropertyDescriptor desc) {
        return !desc.flags.configurable;
      });
}

bool HiddenClass::areAllReadOnly(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime) {
  return forEachPropertyWhile(
      selfHandle,
      runtime,
      [](Runtime &, SymbolID, NamedPropertyDescriptor desc) {
        if (!desc.flags.accessor && desc.flags.writable)
          return false;
        return !desc.flags.configurable;
      });
}

ExecutionStatus HiddenClass::addToPropertyMap(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    SymbolID name,
    NamedPropertyDescriptor desc) {
  assert(selfHandle->propertyMap_ && "the property map must be initialized");

  // Add the new field to the property map.
  MutableHandle<DictPropertyMap> updatedMap{
      runtime, selfHandle->propertyMap_.getNonNull(runtime)};

  if (LLVM_UNLIKELY(
          DictPropertyMap::add(updatedMap, runtime, name, desc) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  selfHandle->propertyMap_.setNonNull(runtime, *updatedMap, runtime.getHeap());
  return ExecutionStatus::RETURNED;
}

void HiddenClass::initializeMissingPropertyMap(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime) {
  assert(!selfHandle->propertyMap_ && "property map is already initialized");
  ++NumHCInitPropMap;

  // Check whether we can steal our parent's map. If we can, we only need
  // to add or update a single property.
  if (selfHandle->parent_ &&
      selfHandle->parent_.getNonNull(runtime)->propertyMap_)
    return stealPropertyMapFromParent(selfHandle, runtime);

  LLVM_DEBUG(
      dbgs() << "Class:" << selfHandle->getDebugAllocationId()
             << " allocating new map\n");

  // First collect all entries in reverse order. This avoids recursion.
  using MapEntry = std::pair<SymbolID, PropertyFlags>;
  llvh::SmallVector<MapEntry, 4> entries;
  entries.reserve(selfHandle->numProperties_);
  {
    // Walk chain of parents using raw pointers.
    NoAllocScope _(runtime);
    for (auto *cur = *selfHandle; cur->numProperties_ > 0;
         cur = cur->parent_.getNonNull(runtime)) {
      // Only visit the HiddenClasses which involve properties.
      if (LLVM_UNLIKELY(cur->symbolID_.isInvalid()))
        continue;
      auto tmpFlags = cur->propertyFlags_;
      tmpFlags.flagsTransition = 0;
      entries.emplace_back(cur->symbolID_, tmpFlags);
    }
  }

  assert(
      entries.size() <= DictPropertyMap::getMaxCapacity() &&
      "There shouldn't ever be this many properties");
  // Allocate the map with the correct size.
  auto res = DictPropertyMap::create(
      runtime,
      std::max(
          (DictPropertyMap::size_type)entries.size(),
          toRValue(DictPropertyMap::DEFAULT_CAPACITY)));
  assert(
      res != ExecutionStatus::EXCEPTION &&
      "Since the entries would fit, there shouldn't be an exception");
  MutableHandle<DictPropertyMap> mapHandle{runtime, res->get()};

  // Add the collected entries in reverse order. Note that there could be
  // duplicates.
  SlotIndex slotIndex = 0;
  for (auto it = entries.rbegin(), e = entries.rend(); it != e; ++it) {
    auto inserted = DictPropertyMap::findOrAdd(mapHandle, runtime, it->first);
    assert(
        inserted != ExecutionStatus::EXCEPTION &&
        "Space was already reserved, this couldn't have grown");

    inserted->first->flags = it->second;
    // If it is a new property, allocate the next slot.
    if (LLVM_LIKELY(inserted->second))
      inserted->first->slot = slotIndex++;
  }

  selfHandle->propertyMap_.setNonNull(runtime, *mapHandle, runtime.getHeap());
}

void HiddenClass::stealPropertyMapFromParent(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime) {
  ++NumHCMapSteal;
  // Most of this method uses raw pointers.
  NoAllocScope noAlloc(runtime);
  auto *self = *selfHandle;
  assert(
      self->parent_ && self->parent_.getNonNull(runtime)->propertyMap_ &&
      !self->propertyMap_ &&
      "stealPropertyMapFromParent() must be called with a valid parent with a property map");

  LLVM_DEBUG(
      dbgs() << "Class:" << self->getDebugAllocationId()
             << " stealing map from parent Class:"
             << self->parent_.getNonNull(runtime)->getDebugAllocationId()
             << "\n");

  // Success! Just steal our parent's map and add our own property.
  self->propertyMap_.set(
      runtime,
      self->parent_.getNonNull(runtime)->propertyMap_,
      runtime.getHeap());
  self->parent_.getNonNull(runtime)->propertyMap_.setNull(runtime.getHeap());

  if (LLVM_UNLIKELY(self->symbolID_.isInvalid())) {
    // No changes to make to the property map, because only the object's parent
    // was updated.
    return;
  }

  // Does our class add a new property?
  if (LLVM_LIKELY(!self->propertyFlags_.flagsTransition)) {
    // This is a new property that we must now add.
    assert(
        self->numProperties_ - 1 ==
            self->propertyMap_.getNonNull(runtime)->size() &&
        "propertyMap->size() must match HiddenClass::numProperties-1 in "
        "new prop transition");

    // Create a descriptor for our property.
    NamedPropertyDescriptor desc{
        self->propertyFlags_, self->numProperties_ - 1};
    // Return to handle mode to add the property.
    noAlloc.release();
    addToPropertyMap(selfHandle, runtime, selfHandle->symbolID_, desc);
    return;
  }
  // Our class is updating the flags of an existing property. So we need
  // to find it and update it.

  auto *propMap = self->propertyMap_.getNonNull(runtime);
  assert(
      self->numProperties_ == propMap->size() &&
      "propertyMap->size() must match HiddenClass::numProperties in "
      "flag update transition");

  auto pos = DictPropertyMap::find(propMap, self->symbolID_);
  assert(pos && "property must exist in flag update transition");
  auto tmpFlags = self->propertyFlags_;
  tmpFlags.flagsTransition = 0;
  DictPropertyMap::getDescriptorPair(propMap, *pos)->second.flags = tmpFlags;
}

} // namespace vm
} // namespace hermes

#undef DEBUG_TYPE
