/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_HIDDENCLASS_H
#define HERMES_VM_HIDDENCLASS_H

#include "hermes/Support/OptValue.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/DictPropertyMap.h"
#include "hermes/VM/JIT/Config.h"
#include "hermes/VM/PropertyDescriptor.h"
#include "hermes/VM/WeakValueMap.h"
#include "hermes/VM/WeakValueObjectMap.h"
#include "hermes/VM/sh_mirror.h"

#include <functional>
#include <memory>
#include "llvh/ADT/ArrayRef.h"

namespace hermes {
namespace vm {

/// The storage type used for properties.
using PropStorage = ArrayStorageSmall;

/// Flags associated with a hidden class.
struct ClassFlags {
  /// Size of parentChangeCounter below in the bitfield.
  static constexpr size_t kParentChangeCounterSize = 2;

  union {
    struct {
      /// This class is in dictionary mode, meaning that adding and removing
      /// fields doesn't cause transitions but simply updates the property map.
      /// (We may still change hidden classes; see dictionaryNoCacheMode,
      /// below).
      uint16_t dictionaryMode : 1;

      /// If dictionaryMode is set, this indicates whether the hidden class can
      /// be used as the key in property caches.  If we delete properties, or
      /// update properties, we create a new hidden class for the owning object
      /// (to invalidate any property caches referencing the old hidden
      /// class).  We may decide to limit the number of hidden classes
      /// created this way (currently we allow just one).  To do this, we
      /// set this property of the hidden class property, so that the new
      /// hidden class is never added to an property cache.
      uint16_t dictionaryNoCacheMode : 1;

      /// Set when we have index-like named properties (e.g. "0", "1", etc)
      /// defined using defineOwnProperty. Array accesses will have to check the
      /// named properties first. The absence of this flag is important as it
      /// indicates that named properties whose name is an integer index don't
      /// need to be searched for - they don't exist.
      uint16_t hasIndexLikeProperties : 1;

      /// There may be a accessor property somewhere in the entire chain of
      /// leading up to this HiddenClass. Set when a property is an accessor,
      /// and can never be unset. That means this is a pessimistic flag: if a
      /// getter/setter property is set and then deleted, this will still be set
      /// to true.
      uint16_t mayHaveAccessor : 1;

      /// This is not in dictionary mode, but it is a class allocated
      /// for a typed object, which means it doesn't have any transitions and
      /// the property map must never be null.
      uint16_t typed : 1;

      /// New properties cannot be added.
      uint16_t noExtend : 1;

      /// \c Object.seal() has been invoked on this object, marking all
      /// properties as non-configurable. When \c Sealed is set, \c NoExtend is
      /// always set too.
      ///
      /// NOTE: It is possible for this flag to be 'false' but for the
      /// JSObject::isSealed() function to return true, because this flag is set
      /// lazily in cases like preventExtensions. If JSObject::isSealed() is
      /// called and returns true, this flag will be set by that call.
      uint16_t sealed : 1;

      /// \c Object.freeze() has been invoked on this object, marking all
      /// properties as non-configurable and non-writable. When \c Frozen is
      /// set, \c Sealed and must \c NoExtend are always set too.
      ///
      /// NOTE: It is possible for this flag to be 'false' but for the
      /// JSObject::isFrozen() function to return true, because this flag is set
      /// lazily in cases like preventExtensions. If JSObject::isFrozen() is
      /// called and returns true, this flag will be set by that call.
      uint16_t frozen : 1;

      /// This flag indicates this is a special object whose properties are
      /// managed by C++ code, and not via the standard property storage
      /// mechanisms.
      uint16_t hostObject : 1;

      /// This is lazily created object that must be initialized before it can
      /// be used. Note that lazy objects must have no properties defined on
      /// them.
      uint16_t lazyObject : 1;

      /// This flag indicates this is a proxy exotic Object
      uint16_t proxyObject : 1;

      /// This object has indexed storage. This flag will not change at runtime,
      /// it is set at construction and its value never changes. It is not a
      /// state.
      uint16_t indexedStorage : 1;

      /// This flag is set when any object using this HiddenClass (or any object
      /// in its parent chain) is cached in the AddPropertyCache.
      /// If the flag is set, then changing the parent or HiddenClass of objects
      /// with this class in their parent chain will increment the
      /// parentCacheEpoch in Runtime.
      uint16_t isCachedUsingEpoch : 1;

      /// The number of times the parent of the object has been changed.
      /// If this counter maxes out, the HiddenClass changes to dictionary mode
      /// to avoid an infinite chain.
      uint16_t parentChangeCounter : kParentChangeCounterSize;

      /// Unused bits, tracked explicitly for convenience.
      uint16_t unusedPadding : 1;
    };

    uint16_t _flags;
  };

  ClassFlags() {
    _flags = 0;
  }

  bool operator==(ClassFlags f) const {
    return _flags == f._flags;
  }
  bool operator!=(ClassFlags f) const {
    return _flags != f._flags;
  }
};

/// A "hidden class" describes a fixed set of properties, their property flags
/// and the order that they were created in. It is logically immutable (unless
/// it is in "dictionary mode", which is described below).
///
/// Overview
/// ========
/// Adding, deleting or updating a property of a "hidden class" is represented
/// as a transition to a new "hidden class", which encodes the new state of the
/// property set. We call the old class a "parent" and the new class a
/// "child". Starting from a given parent class, its children and their
/// children (etc...) form a tree.
///
/// Each class contains a transition table from itself to its children, keyed on
/// the new/updated property name (SymbolID) and the new/updated property flags
/// that caused each child to be created.
///
/// When a new empty JavaScript object is created, it is assigned an empty
/// "root" hidden class. Adding a new property causes a transition from the
/// root class to a new child class and the transition is recorded in the root
/// class transition table. Adding a second property causes another class to
/// be allocated and a transition to be recorded in its parent, and so on.
/// When a second empty JavaScript object is created and the same properties
/// are added in the same order, the existing classes will be found by looking
/// up in each transition table.
///
/// In this way, JavaScript objects which have the same properties added in the
/// same order will end up having the same hidden class identifying their set
/// of properties. That can decreases the memory dramatically (because we have
/// only one set description per class instead of one per object) and can be
/// used for caching property offsets and other attributes.
///
/// Dictionary Mode
/// ===============
/// When more than a predefined number of properties are added (\c
/// kDictionaryThreshold) or if a property is deleted, a new class is created
/// without a parent and placed in "dictionary mode". In that mode the class
/// is not shared - it belongs to exactly one object - and updates are done "in
/// place" instead of creating new child classes.
///
/// Property Maps
/// =============
/// Conceptually every hidden class has a property map - a table mapping from
/// a property name (SymbolID) to a property descriptor (slot + flags).
///
/// In order to conserve memory, we create the property map associated with a
/// class the first time it is needed. To delay creation further, if we are
/// looking for a property for a "put-by-name" operation, we can avoid needing
/// the map by looking for the property in the transition table first. Lastly,
/// when we transition from a parent class to a child class, we "steal" the
/// parent's property map and assign it to the child.
///
/// The desired effect is that only "leaf" classes have property maps and normal
/// property assignment doesn't create a map at all in the intermediate states
/// (except the first time).
///
/// Typed Private Properties
/// ========================
///
/// Typed objects are always created as orphans, not in dictionary mode,
/// and are never transitioned to from any other HiddenClass.
/// Properties are added during allocation and then never again.
///
/// Typed objects may have private properties.
/// These properties have no names, because they can't be accessed by name,
/// only by slot index which is known at compile time. They are still
/// represented here in the HiddenClass by incrementing numProperties_,
/// but they have no entry in the DictPropertyMap.
namespace detail {
/// Encode a transition from a hidden class to a child, keyed on the
/// name of the property and its property flags.
/// This is an internal type but has to be made public so we can define
/// a llvh::DenseMapInfo<> trait for it.
class Transition {
 public:
  /// The name for the property being modified.
  /// Empty and Deleted are reserved for use by the DenseMapInfo.
  SymbolID symbolID;
  PropertyFlags propertyFlags;
  ClassFlags classFlags;

  /// Construct a transition with default property flags. Used for DenseMap
  /// sentinel values and internal-property transitions.
  explicit Transition(SymbolID symbolID, ClassFlags classFlags)
      : symbolID(symbolID), propertyFlags(), classFlags(classFlags) {}
  Transition(SymbolID symbolID, PropertyFlags flags)
      : symbolID(symbolID), propertyFlags(flags) {}

  bool operator==(const Transition &a) const {
    return symbolID == a.symbolID && propertyFlags == a.propertyFlags &&
        classFlags == a.classFlags;
  }
};

/// A front for WeakValueMap<Transition, HiddenClass> that is space-optimized
/// for the common case of 0 or 1 entry. See WeakValueMap for more details.
class TransitionMap {
 public:
  ~TransitionMap() {
    if (isLarge())
      delete large();
    else if (!isClean())
      smallValue().releaseSlot();
  }

  /// Return true if there is an entry with the given key and a valid value.
  bool containsKey(const Transition &key, GC &gc) {
    return (smallKey_ == key && smallValue().isValid()) ||
        (isLarge() && large()->containsKey(key));
  }

  /// Look for a \p key and return the corresponding HiddenClass, or nullptr if
  /// it is not found.
  HiddenClass *lookup(Runtime &runtime, const Transition &key) {
    if (smallKey_ == key) {
      return smallValue().get(runtime);
    } else if (isLarge()) {
      return large()->lookup(runtime, key);
    } else {
      return nullptr;
    }
  }

  /// Insert a key/value into the map if the key is not already there.
  /// \return true if it was inserted, false if the key was already there.
  bool insertNew(
      Runtime &runtime,
      const Transition &key,
      Handle<HiddenClass> value) {
    assert(
        key.symbolID != SymbolID::empty() &&
        "Should never insert an empty key");
    if (smallKey_ == key && smallValue().isValid()) {
      return false;
    }
    if (isClean()) {
      smallKey_ = key;
      smallValue() = WeakRef<HiddenClass>(runtime, value);
      return true;
    }
    if (!isLarge())
      uncleanMakeLarge(runtime);
    return large()->insertNew(runtime, key, value);
  }

  /// \return estimated dynamically allocated memory owned by this map.
  size_t getMemorySize() const;

  /// \return true if the map is known to be empty. May have false negatives.
  bool isKnownEmpty() const {
    return isClean() || (isLarge() && large()->isKnownEmpty());
  }

  /// Invoke \p callback on each (const) key and value. Values may be invalid.
  template <typename CallbackFunction>
  void forEachEntry(const CallbackFunction &callback) const {
    if (isLarge()) {
      large()->forEachEntry(callback);
    } else if (!isClean()) {
      callback(smallKey_, smallValue());
    }
  }

#ifdef HERMES_MEMORY_INSTRUMENTATION
  void snapshotAddNodes(GC &gc, HeapSnapshot &snap);
  void snapshotAddEdges(GC &gc, HeapSnapshot &snap);
  void snapshotUntrackMemory(GC &gc);
#endif

 private:
  /// Clean = no transition has been inserted since construction.
  bool isClean() const {
    return smallKey_.symbolID == SymbolID::empty();
  }

  /// Large = allocated WeakValueMap contains any/all entries.
  bool isLarge() const {
    return smallKey_.symbolID == SymbolID::deleted();
  }

  /// Expand to large mode, assuming already unclean.
  void uncleanMakeLarge(Runtime &runtime);

  /// Accessors for each union member after asserting it's active.
  WeakRef<HiddenClass> &smallValue() {
    assert(!isLarge());
    return u.smallValue_;
  }
  const WeakRef<HiddenClass> &smallValue() const {
    assert(!isLarge());
    return u.smallValue_;
  }
  WeakValueMap<Transition, HiddenClass> *large() const {
    assert(isLarge());
    return u.large_;
  }

  Transition smallKey_{SymbolID::empty(), ClassFlags{}};
  union U {
    U() : smallValue_((WeakRefSlot *)nullptr) {}
    WeakRef<HiddenClass> smallValue_;
    WeakValueMap<Transition, HiddenClass> *large_;
  } u;
};

} // namespace detail

class HiddenClass final : public GCCell {
  friend void HiddenClassBuildMeta(const GCCell *cell, Metadata::Builder &mb);
  friend struct RuntimeOffsets;

  /// Optional property map of all properties defined by this hidden class.
  /// This includes \c symbolID_, \c parent_->symbolID_, \c
  /// parent_->parent_->symbolID_ and so on (in reverse order).
  /// It is constructed lazily when needed, or is "stolen" from the parent class
  /// when a transition is performed from the parent class to this one.
  ///
  /// NOTE: May be cleared by the GC for any HiddenClass not in a Handle.
  GCPointer<DictPropertyMap> propertyMap_{};

  /// The parent hidden class which contains a transition from itself to this
  /// one keyed on \c symbolID_+propertyFlags_. It can be null if there is no
  /// parent.
  GCPointer<HiddenClass> parent_;

  /// The JSObject which is the parent of the object.
  GCPointer<JSObject> objectParent_{nullptr};

  /// Cache that contains for-in property names for objects of this class.
  /// Never used in dictionary mode.
  GCPointer<ArrayStorageSmall> forInCache_{};

  /// The symbol that was added when transitioning to this hidden class.
  const GCSymbolID symbolID_;
  /// The flags of the added symbol.
  const PropertyFlags propertyFlags_;

  /// Total number of properties encoded in the entire chain from this class
  /// to the root. Note that some transitions do not introduce a new property,
  /// so this is not the same as the length of the transition chain.
  /// Before we enter "dictionary mode", this determines the offset of a new
  /// property.
  unsigned numProperties_;

#if HERMESVM_JIT
  /// A unique identifier for this hidden class, lazily assigned by the JIT,
  /// when it needs to keep track of the hidden class cheaply.
  /// 0 means not assigned yet.
  uint16_t lazyJITId_ = 0;
#endif

  /// Flags associated with this hidden class.
  ClassFlags flags_{};

  /// This hash table encodes the transitions from this class to child classes
  /// keyed on the property being added (or updated) and its flags.
  detail::TransitionMap transitionMap_;

  /// This hash table encodes the transitions from this class to child classes
  /// keyed on the new object parent. Null until the first parent change,
  /// because the vast majority of classes never see one.
  std::unique_ptr<WeakValueObjectMap<HiddenClass>> parentTransitionMap_;

 public:
  using Transition = detail::Transition;
  /// Adding more than this number of properties will switch to "dictionary
  /// mode".
  static constexpr unsigned kDictionaryThreshold = 64;

  static const VTable vt;

  inline HiddenClass(
      Runtime &runtime,
      ClassFlags flags,
      Handle<HiddenClass> parent,
      Handle<JSObject> objectParent,
      SymbolID symbolID,
      PropertyFlags propertyFlags,
      unsigned numProperties);

  static constexpr CellKind getCellKind() {
    return CellKind::HiddenClassKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::HiddenClassKind;
  }

  /// \return the maximum number of properties that a hidden class can have.
  static constexpr auto maxNumProperties() {
    return DictPropertyMap::getMaxCapacity();
  }

  /// Create a "root" hidden class - one that doesn't define any properties, but
  /// is a starting point for a hierarchy.
  static HiddenClass *
  createRoot(Runtime &runtime, Handle<JSObject> objectParent, ClassFlags flags);

  /// Create a "root" hidden class for a typed object - one that doesn't define
  /// any properties yet but reserves space for a property map. The class is
  /// created frozen, sealed and noExtend.
  /// \param capacity the number of properties in the typed object,
  ///  used to reserve space in the property map.
  /// \pre capacity < maxNumProperties().
  static HiddenClass *createForTypedObject(
      Runtime &runtime,
      Handle<JSObject> objectParent,
      uint32_t capacity);

  /// \return true if this hidden class is guaranteed to be a leaf.
  /// It can return false negatives, so it should only be used for stats
  /// reporting and such.
  bool isKnownLeaf() const {
    return transitionMap_.isKnownEmpty();
  }

#if HERMESVM_JIT
  uint16_t getLazyJITId() const {
    return lazyJITId_;
  }
  void setLazyJITId(uint16_t id) {
    lazyJITId_ = id;
  }
#endif

  /// \return the object's parent. May be nullptr.
  const GCPointer<JSObject> &getObjectParentGCPtr() const {
    return objectParent_;
  }

  /// \return the object's parent. May be nullptr.
  inline JSObject *getObjectParent(PointerBase &pb) const;

  /// \return the number of own properties described by this hidden class.
  /// This corresponds to the size of the property map, if it is initialized.
  unsigned getNumProperties() const {
    return numProperties_;
  }

  ClassFlags getFlags() const {
    return flags_;
  }

  /// \return true if this class is in "dictionary mode" - i.e. changes to it
  /// don't (normally) result in creation of new classes.
  bool isDictionary() const {
    return flags_.dictionaryMode;
  }

  /// \return true if new properties can be added to the class.
  bool isExtensible() const {
    return !flags_.noExtend;
  }

  /// \return true if the class is sealed.
  bool isSealed() const {
    return flags_.sealed;
  }

  /// \return true if the class is frozen.
  bool isFrozen() const {
    return flags_.frozen;
  }

  /// \return true if the class is for a lazy object.
  bool isLazyObject() const {
    return flags_.lazyObject;
  }

  /// \return true if the class is for a HostObject.
  bool isHostObject() const {
    return flags_.hostObject;
  }

  /// \return true if the class is for a JS Proxy.
  bool isProxyObject() const {
    return flags_.proxyObject;
  }

  /// \return true if the class is frozen.
  bool hasIndexedStorage() const {
    return flags_.indexedStorage;
  }

  /// \return true if this class is in "non-cacheable dictionary mode"
  /// - it is a dictionary, and the owning object has been modified in
  /// a way since becoming a dictionary that precludes inline caching
  /// (for example, a property has been deleted or updated).
  bool isDictionaryNoCache() const {
    assert(
        (!flags_.dictionaryNoCacheMode || flags_.dictionaryMode) &&
        "dictionaryNoCacheMode should only be set if dictionaryMode is set.");
    return flags_.dictionaryNoCacheMode;
  }

  bool getHasIndexLikeProperties() const {
    return flags_.hasIndexLikeProperties;
  }

  bool getMayHaveAccessor() const {
    return flags_.mayHaveAccessor;
  }

  bool isTyped() const {
    return flags_.typed;
  }

  /// \return true if this HiddenClass is used by objects cached using the
  /// parent cache epoch mechanism.
  bool getIsCachedUsingEpoch() const {
    return flags_.isCachedUsingEpoch;
  }

  /// Mark this HiddenClass as being used by objects cached using the parent
  /// cache epoch mechanism.
  /// \return the resulting class
  static HiddenClass *markAsCachedUsingEpoch(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime) {
    // A dictionary is owned by a single object and the flag doesn't affect
    // property lookup, so set it in place. Going through updateClassFlags
    // would permanently make a cacheable dictionary non-cacheable.
    if (selfHandle->isDictionary()) {
      selfHandle->flags_.isCachedUsingEpoch = 1;
      return *selfHandle;
    }
    ClassFlags newFlags = selfHandle->flags_;
    newFlags.isCachedUsingEpoch = 1;
    return HiddenClass::updateClassFlags(selfHandle, runtime, newFlags);
  }

  /// \return The for-in cache if one has been set, otherwise nullptr.
  ArrayStorageSmall *getForInCache(Runtime &runtime) const {
    return forInCache_.get(runtime);
  }

  void setForInCache(ArrayStorageSmall *arr, Runtime &runtime) {
    forInCache_.set(runtime, arr, runtime.getHeap());
  }

  void clearForInCache(Runtime &runtime) {
    forInCache_.setNull(runtime.getHeap());
  }

  /// Reset the property map, unless this class is in dictionary mode or typed.
  /// May be called by the GC for any HiddenClass not in a Handle.
  void clearPropertyMap(GC &gc) {
    if (!isDictionary() && !isTyped())
      propertyMap_.setNull(gc);
  }

  /// An opaque class representing a reference to a valid property in the
  /// property map.
  using PropertyPos = DictPropertyMap::PropertyPos;

  /// Call the supplied callback pass each property's \c SymbolID and \c
  /// NamedPropertyDescriptor as parameters.
  /// Obviously the callback shouldn't be doing naughty things like modifying
  /// the property map or creating new hidden classes (even implicitly).
  /// A marker for the current gcScope is obtained in the beginning and the
  /// scope is flushed after every callback.
  /// NOTE: Does NOT include typed private properties, which have no names.
  template <typename CallbackFunction>
  static void forEachProperty(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      const CallbackFunction &callback);

  /// Same as \p forEachProperty, but the callback cannot do any GC operations,
  /// such as allocating objects, modifying objects, or creating handles.
  /// \p forEachProperty is allowed to allocate, and thus can steal and cache
  /// the property map for the next query, so it is preferred.
  static void forEachPropertyNoAlloc(
      HiddenClass *self,
      PointerBase &base,
      std::function<void(SymbolID, NamedPropertyDescriptor)> callback);

  /// Same as forEachProperty() but the callback returns true to continue or
  /// false to stop immediately.
  /// A marker for the current gcScope is obtained in the beginning and the
  /// scope is flushed after every callback.
  /// \return false if the callback returned false, true otherwise.
  template <typename CallbackFunction>
  static bool forEachPropertyWhile(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      const CallbackFunction &callback);

  /// Look for a property in the property map. If the property is found, return
  /// a \c PropertyPos identifying it and store its descriptor in \p desc.
  /// \return the "position" of the property, if found.
  static inline OptValue<PropertyPos> findProperty(
      PseudoHandle<HiddenClass> self,
      Runtime &runtime,
      SymbolID name,
      NamedPropertyDescriptor &desc);

  /// Same operation as \p findProperty, but does not do any allocations.
  /// This is slower than \p findProperty because it needs to traverse the full
  /// hidden class chain in the worst case.
  static llvh::Optional<NamedPropertyDescriptor>
  findPropertyNoAlloc(HiddenClass *self, PointerBase &base, SymbolID name);

  /// An optimistic fast path for \c findProperty(). If there is an allocated
  /// property map, this will return an OptValue containing either true or
  /// false. If there was no allocated property map, this returns llvh::None. If
  /// this fails by returning None, the "slow path", \c findProperty() itself,
  /// must be used.
  static OptValue<bool> tryFindPropertyFast(
      const HiddenClass *self,
      PointerBase &base,
      SymbolID name,
      NamedPropertyDescriptor &desc);

  /// Performs a very slow linear search for the specified property. This should
  /// only be used for debug tests where we don't want to allocate a property
  /// map because doing so would change the behavior.
  /// \return true if the property is defined, false otherwise.
  static bool
  debugIsPropertyDefined(HiddenClass *self, PointerBase &base, SymbolID name);

  /// Delete a property which we found earlier using \c findProperty.
  /// \return the resulting new class.
  static PseudoHandle<HiddenClass> deleteProperty(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      PropertyPos pos);

  /// Add a new property. It must not already exist.
  /// \param[out] out receives the resulting class. It is only written on
  ///   success and may alias \p selfHandle.
  /// \return the index of the new property.
  static CallResult<SlotIndex> addProperty(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      SymbolID name,
      PropertyFlags propertyFlags,
      MutableHandle<HiddenClass> out);

  /// Update an existing property's flags and return the resulting class.
  /// \param pos is the position of the property into the property map.
  static PseudoHandle<HiddenClass> updateProperty(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      PropertyPos pos,
      PropertyFlags newFlags);

  /// Add a property to the HiddenClass (does not allocate a new HiddenClass).
  /// \param propertiesEnumerable if false, the property will not be enumerable.
  /// \return the index of the property.
  static SlotIndex addNewTypedPublicProperty(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      SymbolID name,
      bool propertiesEnumerable);

  /// Add an unnamed private property to the typed HiddenClass.
  /// \return the index of the property.
  static SlotIndex addNewTypedPrivateProperty(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime) {
    assert(!selfHandle->parent_ && "must be root hidden class");
    assert(selfHandle->flags_.typed && "must be typed");
    SlotIndex newSlot = selfHandle->numProperties_++;
    return newSlot;
  }

  /// Update the object's parent and return the resulting class.
  /// If the parent has changed more than parentChangeCounter can handle,
  /// the resulting HiddenClass will be in dictionary mode.
  /// \param newParent may be null if the parent is being deleted.
  static inline HiddenClass *updateObjectParent(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      PseudoHandle<JSObject> newParent);

  /// The slow path for updating the object parent.
  /// Only called if the parent actually needs to be updated.
  static HiddenClass *updateObjectParentSlowPath(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      PseudoHandle<JSObject> newParent);

  /// Update the ClassFlags and return the new HiddenClass.
  /// Updates the transition table to cache the new HiddenClass.
  static HiddenClass *updateClassFlags(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      ClassFlags newFlags);

  /// Mark all properties as non-configurable.
  /// \return the resulting class
  static HiddenClass *seal(Handle<HiddenClass> selfHandle, Runtime &runtime);

  /// Mark the HiddenClass as being sealed.
  /// \return the resulting class
  static HiddenClass *markAsSealed(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime) {
    ClassFlags sealedFlags = selfHandle->flags_;
    sealedFlags.noExtend = 1;
    sealedFlags.sealed = 1;
    return HiddenClass::updateClassFlags(selfHandle, runtime, sealedFlags);
  }

  /// Mark all properties as non-writable and non-configurable.
  /// \return the resulting class
  static HiddenClass *freeze(Handle<HiddenClass> selfHandle, Runtime &runtime);

  /// Mark the HiddenClass as being frozen (which implies sealed and
  /// non-extensible).
  /// \return the resulting class
  static HiddenClass *markAsFrozen(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime) {
    ClassFlags frozenFlags = selfHandle->flags_;
    frozenFlags.noExtend = 1;
    frozenFlags.frozen = 1;
    frozenFlags.sealed = 1;
    return HiddenClass::updateClassFlags(selfHandle, runtime, frozenFlags);
  }

  /// Mark the HiddenClass as being non-extensible.
  /// \return the resulting class
  static HiddenClass *markNoExtend(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime) {
    ClassFlags noExtendFlags = selfHandle->flags_;
    noExtendFlags.noExtend = 1;
    return HiddenClass::updateClassFlags(selfHandle, runtime, noExtendFlags);
  }

  /// Update the flags for the properties in the list \p props with \p
  /// flagsToClear and \p flagsToSet. If in dictionary mode, the properties are
  /// updated on the hidden class directly; otherwise, create a new dictionary
  /// hidden class as result. Updating the properties mutates the property map
  /// directly without creating transitions.
  /// \p flagsToClear and \p flagsToSet are masks for updating the property
  /// flags.
  /// \p props is a list of SymbolIDs for properties that need to be updated
  /// made read-only. It should contain a subset of properties in the hidden
  /// class, so the SymbolIDs won't get freed by gc. It can be llvh::None; if it
  /// is llvh::None, update every property.
  /// \return the resulting hidden class.
  static PseudoHandle<HiddenClass> updatePropertyFlagsWithoutTransitions(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      PropertyFlags flagsToClear,
      PropertyFlags flagsToSet,
      OptValue<llvh::ArrayRef<SymbolID>> props);

  /// Create a new class where the next slot is reserved, by calling addProperty
  /// with an internal property name. Only slots with index less than
  /// InternalProperty::NumAnonymousInternalProperties can be reserved.
  /// \param selfHandle must not be in dictionary mode.
  /// \param[out] out receives the resulting class; may alias \p selfHandle.
  /// \return the index of the reserved slot.
  static CallResult<SlotIndex> reserveSlot(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      MutableHandle<HiddenClass> out);

  /// \return true if all properties are non-configurable
  static bool areAllNonConfigurable(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime);

  /// \return true if all properties are non-writable and non-configurable
  static bool areAllReadOnly(Handle<HiddenClass> selfHandle, Runtime &runtime);

  /// Verify that the mirror struct SHHiddenClass has the correct layout.
  /// This function is never called - it only exists to contain static asserts.
  static void staticAsserts();

 private:
  /// Allocate a new hidden class instance with the supplied parameters.
  static HiddenClass *create(
      Runtime &runtime,
      ClassFlags flags,
      Handle<HiddenClass> parent,
      Handle<JSObject> objectParent,
      SymbolID symbolID,
      PropertyFlags propertyFlags,
      unsigned numProperties);

  /// Create a copy of this \c HiddenClass and ensure that the copy is
  /// in dictionary mode.  Requires that the current \C HiddenClass
  /// does not have the dictionaryNoCacheMode flag set; such
  /// dictionaries may not be copied.  If the current class has a
  /// property map, it will be moved to the new class. Otherwise a new
  /// property map will be created for the new class. In either case,
  /// the current class will have no property map and the new class
  /// will have one.  The \p noCache argument indicates whether the
  /// new dictionary \c HiddenClass's dictionaryNoCacheMode flag will
  /// be set.  \return the new class.
  static PseudoHandle<HiddenClass> copyToNewDictionary(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      bool noCache = false);

  /// Out-of-line slow path of addProperty for a class-mode \p selfHandle with
  /// no existing transition: either converts to a dictionary or allocates a
  /// new child class. \p out may alias \p selfHandle.
  LLVM_ATTRIBUTE_NOINLINE static CallResult<SlotIndex> addPropertySlow(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      SymbolID name,
      PropertyFlags propertyFlags,
      MutableHandle<HiddenClass> out);

  /// Like findProperty, but for the case where we know that propertyMap_ is
  /// null. This serves as the out-of-line slow path for findProperty.
  static OptValue<PropertyPos> findPropertyNoMap(
      PseudoHandle<HiddenClass> self,
      Runtime &runtime,
      SymbolID name,
      NamedPropertyDescriptor &desc);

  /// Add a new property pair (\p name and \p desc) to the property map (which
  /// must have been initialized).
  static ExecutionStatus addToPropertyMap(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime,
      SymbolID name,
      NamedPropertyDescriptor desc);

  /// Construct a property map by walking back the chain of hidden classes and
  /// store it in \c propertyMap_.
  static void initializeMissingPropertyMap(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime);

  /// Initialize the property map by transferring the parent's map to ourselves
  /// and adding a our property to it. It must only be called if we don't have a
  /// property map of our own but have a valid parent with a property map.
  static void stealPropertyMapFromParent(
      Handle<HiddenClass> selfHandle,
      Runtime &runtime);

  /// Free all non-GC managed resources associated with the object.
  static void _finalizeImpl(GCCell *cell, GC &gc);

  /// \return the amount of non-GC memory being used by the given \p cell, which
  /// is assumed to be a HiddenClass.
  static size_t _mallocSizeImpl(GCCell *cell);

#ifdef HERMES_MEMORY_INSTRUMENTATION
  static std::string _snapshotNameImpl(GCCell *cell, GC &gc);
  static void _snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
  static void _snapshotAddNodesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
#endif

 private:
  /// Computes the updated class flags for a class with flags \p flags for when
  /// a property is added or updated with property flags \p pf and based on
  /// whether a new index like property has been added.
  /// This operation is idempotent, which means that multiple updates with the
  /// same \p pf and \p addedIndexLike may be collapsed into a single update.
  /// \return Updated flags that reflect the added/updated property.
  static ClassFlags
  computeFlags(ClassFlags flags, PropertyFlags pf, bool addedIndexLike);
};

//===----------------------------------------------------------------------===//
// HiddenClass inline methods.

template <typename CallbackFunction>
void HiddenClass::forEachProperty(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    const CallbackFunction &callback) {
  if (LLVM_UNLIKELY(!selfHandle->propertyMap_))
    initializeMissingPropertyMap(selfHandle, runtime);

  struct : public Locals {
    PinnedValue<DictPropertyMap> map;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.map = selfHandle->propertyMap_.getNonNull(runtime);
  return DictPropertyMap::forEachProperty(lv.map, runtime, callback);
}

template <typename CallbackFunction>
bool HiddenClass::forEachPropertyWhile(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    const CallbackFunction &callback) {
  if (LLVM_UNLIKELY(!selfHandle->propertyMap_))
    initializeMissingPropertyMap(selfHandle, runtime);

  struct : public Locals {
    PinnedValue<DictPropertyMap> map;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.map = selfHandle->propertyMap_.getNonNull(runtime);
  return DictPropertyMap::forEachPropertyWhile(lv.map, runtime, callback);
}

inline OptValue<bool> HiddenClass::tryFindPropertyFast(
    const HiddenClass *self,
    PointerBase &base,
    SymbolID name,
    NamedPropertyDescriptor &desc) {
  if (LLVM_LIKELY(self->propertyMap_)) {
    auto found =
        DictPropertyMap::find(self->propertyMap_.getNonNull(base), name);
    if (LLVM_LIKELY(found)) {
      desc = DictPropertyMap::getDescriptorPair(
                 self->propertyMap_.getNonNull(base), *found)
                 ->second;
    }
    return found.hasValue();
  } else if (self->numProperties_ == 0) {
    return false;
  }
  return llvh::None;
}
inline ClassFlags HiddenClass::computeFlags(
    ClassFlags flags,
    PropertyFlags pf,
    bool addedIndexLike) {
  flags.hasIndexLikeProperties |= addedIndexLike;
  // Carry over the the existing mayHaveAccessor flag. Once an accessor property
  // has been set, all subsequent classes must have this property marked.
  flags.mayHaveAccessor |= pf.accessor;
  return flags;
}

inline OptValue<HiddenClass::PropertyPos> HiddenClass::findProperty(
    PseudoHandle<HiddenClass> self,
    Runtime &runtime,
    SymbolID name,
    NamedPropertyDescriptor &desc) {
  if (LLVM_UNLIKELY(!self->propertyMap_)) {
    return findPropertyNoMap(std::move(self), runtime, name, desc);
  }

  auto *propMap = self->propertyMap_.getNonNull(runtime);
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

inline void HiddenClass::staticAsserts() {
  static_assert(sizeof(PropertyFlags) == 2);
  static_assert(sizeof(ClassFlags) == 2);
  static_assert(sizeof(detail::Transition) == sizeof(SHTransition));
  static_assert(sizeof(detail::Transition) == sizeof(SHTransition));
  static_assert(sizeof(detail::TransitionMap) == sizeof(SHTransitionMap));
  static_assert(sizeof(ClassFlags) == 2);

  static_assert(sizeof(HiddenClass) == sizeof(SHHiddenClass));
  static_assert(
      offsetof(HiddenClass, propertyMap_) ==
      offsetof(SHHiddenClass, propertyMap));
  static_assert(
      offsetof(HiddenClass, parent_) == offsetof(SHHiddenClass, parent));
  static_assert(
      offsetof(HiddenClass, objectParent_) ==
      offsetof(SHHiddenClass, objectParent));
  static_assert(
      offsetof(HiddenClass, forInCache_) ==
      offsetof(SHHiddenClass, forInCache));
  static_assert(
      offsetof(HiddenClass, symbolID_) == offsetof(SHHiddenClass, symbolID));
  static_assert(
      offsetof(HiddenClass, propertyFlags_) ==
      offsetof(SHHiddenClass, propertyFlags));
  static_assert(
      offsetof(HiddenClass, numProperties_) ==
      offsetof(SHHiddenClass, numProperties));
#if HERMESVM_JIT
  static_assert(
      offsetof(HiddenClass, lazyJITId_) == offsetof(SHHiddenClass, lazyJITId));
#endif
  static_assert(
      offsetof(HiddenClass, flags_) == offsetof(SHHiddenClass, flags));
  static_assert(sizeof(SHHiddenClass::flags) == sizeof(ClassFlags));
  static_assert(
      offsetof(HiddenClass, transitionMap_) ==
      offsetof(SHHiddenClass, transitionMap));
  static_assert(
      offsetof(HiddenClass, parentTransitionMap_) ==
      offsetof(SHHiddenClass, parentTransitionMap));
  llvm_unreachable("staticAsserts must never be called.");
}

/// The key of a class in its parent's parentTransitionMap_ is its object
/// parent. It never changes, because classes in the map are not dictionaries,
/// and only dictionaries update their object parent in place.
template <>
struct WeakValueObjectMapKey<HiddenClass> {
  /// \return the object parent of \p clazz, which may be null.
  static JSObject *get(HiddenClass *clazz, Runtime &runtime);
};

} // namespace vm
} // namespace hermes

// Enable using HiddenClass::Transition in DenseMap.
namespace llvh {

using namespace hermes::vm;

template <>
struct DenseMapInfo<HiddenClass::Transition> {
  static inline HiddenClass::Transition getEmptyKey() {
    return HiddenClass::Transition(SymbolID::empty(), ClassFlags{});
  }

  static inline HiddenClass::Transition getTombstoneKey() {
    return HiddenClass::Transition(SymbolID::deleted(), ClassFlags{});
  }

  static inline unsigned getHashValue(HiddenClass::Transition transition) {
    return transition.symbolID.unsafeGetRaw() ^ transition.propertyFlags._flags;
  }

  static inline bool isEqual(
      const HiddenClass::Transition &a,
      const HiddenClass::Transition &b) {
    return a == b;
  }
};

} // namespace llvh

#endif // HERMES_VM_HIDDENCLASS_H
