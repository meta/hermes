/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_IR_TYPECONTEXT_H
#define HERMES_IR_TYPECONTEXT_H

#include "hermes/Support/StringTable.h"

#include "llvh/ADT/ArrayRef.h"
#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/FoldingSet.h"
#include "llvh/ADT/Hashing.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/ADT/iterator_range.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace llvh {
class raw_ostream;
} // namespace llvh

namespace hermes {

/// Kinds of types in the IR type system.
enum class TypeKind : uint8_t {
  // --- Leaf kinds (no sub-type data) ---
  NoType, ///< Empty set, bottom.
  Undefined,
  Null,
  Boolean,
  Number, ///< Unrefined fp64.
  BigInt,
  String,
  Symbol,
  Empty, ///< TDZ.
  Uninit, ///< Declared, not initialized.
  Environment,
  PrivateName,
  FunctionCode,
  Object, ///< Unrefined JS object.
  Bits32, ///< Physical 32-bit integer, signless.

  // --- Number subtypes (still fp64, value constrained) ---
  Int32, ///< Integer in [-2^31, 2^31 - 1].
  Uint32, ///< Integer in [0, 2^32 - 1].
  UInt31, ///< Integer in [0, 2^31 - 1] (Int32 ∩ Uint32).

  // --- Refined object types (payload-bearing, not yet supported in Phase 1)
  _RefinedFirst,
  ClassInstance = _RefinedFirst, ///< Nominal class instance.
  Array, ///< Array with known element type.
  Tuple, ///< Fixed-length positional types.
  Function, ///< Callable with known signature.
  ExactObject, ///< Known exact field set.
  _RefinedLast = ExactObject,

  // --- Composite ---
  Union, ///< Union of two or more types.
};

/// Primitive-mask fast path.
///
/// In legacy untyped JavaScript -- which is what this fast path targets --
/// most types are built only from JavaScript primitives (number, string,
/// boolean, bigint, symbol, null, undefined) plus the unrefined Object and the
/// internal Empty/Uninit markers. (Typed programs lean on refined object types,
/// which are not maskable and take the general path.) For such a type,
/// TypeContext caches a compact bitmask -- its "primMask" -- in the type table
/// entry, encoding the whole type as a set of bits. When both operands of a
/// type operation (unionTy/intersectTy/subtractTy, isSubsetOf/areDisjoint, the
/// canBe* queries, the isPrimitive/isNonPtr predicates, ...) are maskable, the
/// operation is computed with a handful of bit operations plus a small
/// number-family lookup table, instead of walking and re-interning union arms;
/// the result is then mapped back to its canonical interned Type through a
/// cache. Refined object types and compiler-internal kinds (Environment,
/// FunctionCode, ...) cannot be encoded this way: their entries are tagged
/// kNotMaskable and always take the general, slower path. The mask is a pure
/// accelerator -- it never changes which Type an operation yields, only how
/// quickly it is found.
///
/// A primMask is kPrimMaskBits wide, in two parts:
///   - the low kNumCodeBits bits hold one code from the number-family lattice
///     (None < UInt31 < {Int32, Uint32} < Int32|Uint32 < Number), because the
///     numeric subtypes overlap and so cannot be independent bits; and
///   - the upper kKindBits bits are independent one-per-kind flags, one for
///     each non-numeric primitive plus Object/Empty/Uninit.
///
/// The encoding constants below live in a nested namespace so they do not leak
/// into the public `hermes` namespace; they sit in the header only because
/// TypeEntry and the white-box unit tests reference them. The lattice tables
/// and the leaf->mask mapping that operate on the encoding are private to
/// TypeContext.cpp.
namespace type_internal {

/// Width of the number-family lattice code, in the low bits of a primMask.
static constexpr unsigned kNumCodeBits = 3;
/// Number of independent one-per-kind bits above the number code.
static constexpr unsigned kKindBits = 9;
/// Total width of a primMask.
static constexpr unsigned kPrimMaskBits = kNumCodeBits + kKindBits;

/// Sentinel primMask for a type that cannot be represented as a mask: the
/// first value past the valid kPrimMaskBits-wide range, so it never collides
/// with a real mask.
static constexpr uint16_t kNotMaskable = 1u << kPrimMaskBits;

/// Selects the number-family lattice code (the low kNumCodeBits bits).
static constexpr uint16_t kNumCodeMask = (1u << kNumCodeBits) - 1;

/// Number-family lattice element codes (subsets of Number).
enum NumCode : uint16_t {
  NC_None = 0, ///< no number
  NC_UInt31 = 1, ///< [0, 2^31)        = Int32 ∩ Uint32
  NC_Int32 = 2, ///< [-2^31, 2^31)
  NC_Uint32 = 3, ///< [0, 2^32)
  NC_Int32OrUint32 = 4, ///< [-2^31, 2^32) = Int32 ∪ Uint32
  NC_Number = 5, ///< all fp64
};

/// Disjoint per-kind mask bits (MB), one per kind, occupying the kKindBits
/// bits above the number code.
enum MaskBit : uint16_t {
  MB_Undefined = 1u << (kNumCodeBits + 0),
  MB_Null = 1u << (kNumCodeBits + 1),
  MB_Boolean = 1u << (kNumCodeBits + 2),
  MB_BigInt = 1u << (kNumCodeBits + 3),
  MB_String = 1u << (kNumCodeBits + 4),
  MB_Symbol = 1u << (kNumCodeBits + 5),
  MB_Object = 1u << (kNumCodeBits + 6),
  MB_Empty = 1u << (kNumCodeBits + 7),
  MB_Uninit = 1u << (kNumCodeBits + 8),
};

static_assert(
    kPrimMaskBits < 16,
    "a primMask and its kNotMaskable sentinel must fit in uint16_t");
static_assert(
    NC_Number <= kNumCodeMask,
    "number-family codes must fit in kNumCodeBits");
static_assert(
    MB_Uninit == (1u << (kPrimMaskBits - 1)),
    "the disjoint kind bits must exactly fill kKindBits above the number code");

} // namespace type_internal

/// An entry in the TypeContext type table.
struct TypeEntry {
  TypeKind kind;
  /// Compact primitive-mask encoding of this type, or kNotMaskable if the type
  /// cannot be represented as a mask. See the type_internal namespace above for
  /// the layout and how the fast path uses it.
  uint16_t primMask{type_internal::kNotMaskable};

  union {
    /// ClassInstance payload. Classes are nominal: each class declaration
    /// produces a unique type entry, not interned by structure.
    struct {
      /// Type ID of parent ClassInstance, or kNoTypeId if no parent.
      uint32_t superClassTypeId;
      /// Class name for runtime type descriptors and error messages.
      Identifier className;
      /// Offset into fieldArrays_.
      uint32_t fieldOffset;
      /// Number of fields.
      uint16_t fieldCount;
    } classInstance;

    /// Array payload.
    struct {
      uint32_t elemTypeId;
    } array;

    /// Tuple payload.
    struct {
      uint32_t elemOffset; ///< into typeArrays_
      uint16_t elemCount;
    } tuple;

    /// Function payload.
    struct {
      uint32_t returnTypeId;
      uint32_t thisTypeId;
      uint32_t restTypeId;
      uint32_t paramOffset; ///< into paramArrays_
      uint16_t paramCount;
      bool isAsync;
      bool isGenerator;
    } function;

    /// ExactObject payload.
    struct {
      uint32_t fieldOffset; ///< into fieldArrays_
      uint16_t fieldCount;
    } exactObject;

    /// Union payload.
    struct {
      uint32_t armOffset; ///< into typeArrays_
      uint16_t armCount; ///< >= 2
    } union_;
  };

  /// Construct a leaf type entry with no payload.
  static TypeEntry createLeaf(TypeKind k);

  /// Construct a union type entry.
  static TypeEntry createUnion(uint32_t armOffset, uint16_t armCount) {
    assert(armCount >= 2 && "Union must have at least 2 arms");
    TypeEntry e{};
    e.kind = TypeKind::Union;
    e.union_.armOffset = armOffset;
    e.union_.armCount = armCount;
    return e;
  }
};

/// Side array element for function parameters.
struct FunctionParam {
  uint32_t typeId;
  bool isOptional;
};

/// Pre-allocated type IDs (assigned by TypeContext constructor).
/// These are constants, valid for any TypeContext instance.
/// The well-known ID assignment is frozen: IDs are never removed or reordered.
/// New well-known IDs are only appended before kFirstDynamicId.
enum : uint32_t {
  kNoTypeId,
  kEmptyId,
  kUninitId,
  kUndefinedId,
  kNullId,
  kBooleanId,
  kStringId,
  kNumberId,
  kBigIntId,
  kSymbolId,
  kEnvironmentId,
  kPrivateNameId,
  kFunctionCodeId,
  kObjectId,
  kBits32Id,
  kInt32Id,
  kUint32Id,
  /// Int32 ∩ Uint32: integer in [0, 2^31 - 1].
  kUInt31Id,
  _kFirstUnionId,
  /// Union of all JS-observable types.
  kAnyTypeId = _kFirstUnionId,
  /// Number | BigInt.
  kNumericId,
  /// any | Empty | Uninit.
  kAnyEmptyUninitId,
  /// Null | Undefined.
  kNullOrUndefId,
  /// String | Symbol (property keys after ToPropertyKey).
  kStringOrSymbolId,
  /// Empty | Uninit (TDZ check input set).
  kEmptyOrUninitId,
  /// Object | Null (LoadParent result).
  kObjectOrNullId,
  /// Object | Undefined (ES5 new.target).
  kObjectOrUndefId,
  _kLastUnionId = kObjectOrUndefId,

  /// IDs below this are reserved for well-known types.
  kFirstDynamicId = 32,
};

/// Hashable key for interning union types. Contains sorted arm IDs.
/// TODO: This is inefficient. DenseMap stores a full SmallVector in every
/// bucket, including empty and tombstone slots (~56 bytes each), and the arm
/// data duplicates what is already in typeArrays_.
struct UnionInternKey {
  llvh::SmallVector<uint32_t, 8> arms;

  UnionInternKey() = default;
  explicit UnionInternKey(llvh::ArrayRef<uint32_t> a)
      : arms(a.begin(), a.end()) {}
};

/// DenseMap info for UnionInternKey.
struct UnionInternKeyInfo {
  static UnionInternKey getEmptyKey() {
    UnionInternKey k;
    k.arms.push_back(UINT32_MAX);
    return k;
  }
  static UnionInternKey getTombstoneKey() {
    UnionInternKey k;
    k.arms.push_back(UINT32_MAX - 1);
    return k;
  }
  static unsigned getHashValue(const UnionInternKey &k) {
    return llvh::hash_combine_range(k.arms.begin(), k.arms.end());
  }
  static bool isEqual(const UnionInternKey &a, const UnionInternKey &b) {
    return a.arms == b.arms;
  }
};

/// Hashable key for interning tuple types: the element type ids, in order.
/// Unlike UnionInternKey, the ids are neither sorted nor deduplicated, since
/// the position and the count of the elements are part of a tuple type.
struct TupleInternKey {
  llvh::SmallVector<uint32_t, 4> elems;
};

/// DenseMap info for TupleInternKey.
struct TupleInternKeyInfo {
  static TupleInternKey getEmptyKey() {
    TupleInternKey k;
    k.elems.push_back(UINT32_MAX);
    return k;
  }
  static TupleInternKey getTombstoneKey() {
    TupleInternKey k;
    k.elems.push_back(UINT32_MAX - 1);
    return k;
  }
  static unsigned getHashValue(const TupleInternKey &k) {
    return llvh::hash_combine_range(k.elems.begin(), k.elems.end());
  }
  static bool isEqual(const TupleInternKey &a, const TupleInternKey &b) {
    return a.elems == b.elems;
  }
};

/// Hashable key for interning exact object types: the name and the type id of
/// every field, in order. Field order is significant, so {x, y} and {y, x}
/// have different keys. Field variance is not part of the key (D8, see
/// TypeContext::createExactObject).
struct ExactObjectInternKey {
  /// One field of the key.
  struct Field {
    Identifier name;
    uint32_t typeId;
  };
  llvh::SmallVector<Field, 4> fields;
};

/// DenseMap info for ExactObjectInternKey.
struct ExactObjectInternKeyInfo {
  static ExactObjectInternKey getEmptyKey() {
    ExactObjectInternKey k;
    k.fields.push_back({Identifier{}, UINT32_MAX});
    return k;
  }
  static ExactObjectInternKey getTombstoneKey() {
    ExactObjectInternKey k;
    k.fields.push_back({Identifier{}, UINT32_MAX - 1});
    return k;
  }
  static unsigned getHashValue(const ExactObjectInternKey &k) {
    llvh::hash_code h = llvh::hash_value(k.fields.size());
    for (const ExactObjectInternKey::Field &f : k.fields)
      h = llvh::hash_combine(h, f.name.getUnderlyingPointer(), f.typeId);
    return (unsigned)h;
  }
  static bool isEqual(
      const ExactObjectInternKey &a,
      const ExactObjectInternKey &b) {
    if (a.fields.size() != b.fields.size())
      return false;
    for (size_t i = 0, e = a.fields.size(); i < e; ++i) {
      if (a.fields[i].name != b.fields[i].name ||
          a.fields[i].typeId != b.fields[i].typeId)
        return false;
    }
    return true;
  }
};

/// Hashable key for interning function types. Captures the whole shallow
/// signature: return/this/rest ids, the ordered params (id + optional), and
/// the async/generator flags.
struct FunctionInternKey {
  uint32_t returnTypeId;
  uint32_t thisTypeId;
  uint32_t restTypeId;
  bool isAsync;
  bool isGenerator;
  llvh::SmallVector<FunctionParam, 4> params;
};

/// DenseMap info for FunctionInternKey.
struct FunctionInternKeyInfo {
  static FunctionInternKey getEmptyKey() {
    FunctionInternKey k;
    k.returnTypeId = UINT32_MAX;
    k.thisTypeId = 0;
    k.restTypeId = 0;
    k.isAsync = false;
    k.isGenerator = false;
    return k;
  }
  static FunctionInternKey getTombstoneKey() {
    FunctionInternKey k;
    k.returnTypeId = UINT32_MAX - 1;
    k.thisTypeId = 0;
    k.restTypeId = 0;
    k.isAsync = false;
    k.isGenerator = false;
    return k;
  }
  static unsigned getHashValue(const FunctionInternKey &k) {
    llvh::hash_code h = llvh::hash_combine(
        k.returnTypeId, k.thisTypeId, k.restTypeId, k.isAsync, k.isGenerator);
    for (const FunctionParam &p : k.params)
      h = llvh::hash_combine(h, p.typeId, p.isOptional);
    return (unsigned)h;
  }
  static bool isEqual(const FunctionInternKey &a, const FunctionInternKey &b) {
    if (a.returnTypeId != b.returnTypeId || a.thisTypeId != b.thisTypeId ||
        a.restTypeId != b.restTypeId || a.isAsync != b.isAsync ||
        a.isGenerator != b.isGenerator || a.params.size() != b.params.size())
      return false;
    for (size_t i = 0, e = a.params.size(); i < e; ++i) {
      if (a.params[i].typeId != b.params[i].typeId ||
          a.params[i].isOptional != b.params[i].isOptional)
        return false;
    }
    return true;
  }
};

class TypeContext;
class TypeContextTest;

/// Representation of a type in the IR. A Type is an opaque 4-byte handle
/// (index) into the type table owned by a TypeContext. Only trivial,
/// constexpr identity operations live on Type itself (well-known ID
/// factories, id-equality checks, hashing); every non-trivial query or
/// operation requires an explicit TypeContext (typically reached via the
/// owning Module, e.g. `inst->getModule()->getTypeContext()`).
class Type {
  friend class TypeContext;
  /// Test fixture is a friend so it can construct Type from a raw
  /// well-known ID for refined-kind tests that have no public factory.
  friend class TypeContextTest;

  /// Index into TypeContext's type table.
  uint32_t id_{kNoTypeId};

  /// Private constructor from a type ID.
  constexpr explicit Type(uint32_t id) : id_(id) {}

 public:
  /// \name Static well-known constructors (constexpr, no context needed).
  /// @{
  static constexpr Type createNoType() {
    return Type(kNoTypeId);
  }
  static constexpr Type createAnyEmptyUninit() {
    return Type(kAnyEmptyUninitId);
  }
  static constexpr Type createAnyType() {
    return Type(kAnyTypeId);
  }
  /// Create an uninitialized TDZ type.
  static constexpr Type createEmpty() {
    return Type(kEmptyId);
  }
  static constexpr Type createUninit() {
    return Type(kUninitId);
  }
  static constexpr Type createUndefined() {
    return Type(kUndefinedId);
  }
  static constexpr Type createNull() {
    return Type(kNullId);
  }
  static constexpr Type createBoolean() {
    return Type(kBooleanId);
  }
  static constexpr Type createString() {
    return Type(kStringId);
  }
  static constexpr Type createSymbol() {
    return Type(kSymbolId);
  }
  static constexpr Type createObject() {
    return Type(kObjectId);
  }
  static constexpr Type createNumber() {
    return Type(kNumberId);
  }
  /// Alias for createNumber().
  static constexpr Type createInt32() {
    return createNumber();
  }
  /// Alias for createNumber().
  static constexpr Type createUint32() {
    return createNumber();
  }
  static constexpr Type createBigInt() {
    return Type(kBigIntId);
  }
  static constexpr Type createNumeric() {
    return Type(kNumericId);
  }
  static constexpr Type createEnvironment() {
    return Type(kEnvironmentId);
  }
  static constexpr Type createPrivateName() {
    return Type(kPrivateNameId);
  }
  static constexpr Type createFunctionCode() {
    return Type(kFunctionCodeId);
  }
  static constexpr Type createNullOrUndef() {
    return Type(kNullOrUndefId);
  }
  static constexpr Type createStringOrSymbol() {
    return Type(kStringOrSymbolId);
  }
  static constexpr Type createEmptyOrUninit() {
    return Type(kEmptyOrUninitId);
  }
  static constexpr Type createObjectOrNull() {
    return Type(kObjectOrNullId);
  }
  static constexpr Type createObjectOrUndef() {
    return Type(kObjectOrUndefId);
  }
  /// @}

  /// \name Instance type checks (compare against well-known IDs, no context).
  /// @{
  constexpr bool isNoType() const {
    return id_ == kNoTypeId;
  }
  constexpr bool isAnyEmptyUninitType() const {
    return id_ == kAnyEmptyUninitId;
  }
  constexpr bool isAnyType() const {
    return id_ == kAnyTypeId;
  }
  constexpr bool isEmptyType() const {
    return id_ == kEmptyId;
  }
  constexpr bool isUninitType() const {
    return id_ == kUninitId;
  }
  constexpr bool isUndefinedType() const {
    return id_ == kUndefinedId;
  }
  constexpr bool isNullType() const {
    return id_ == kNullId;
  }
  constexpr bool isBooleanType() const {
    return id_ == kBooleanId;
  }
  constexpr bool isStringType() const {
    return id_ == kStringId;
  }
  constexpr bool isObjectType() const {
    return id_ == kObjectId;
  }
  constexpr bool isNumberType() const {
    return id_ == kNumberId;
  }
  constexpr bool isBigIntType() const {
    return id_ == kBigIntId;
  }
  constexpr bool isSymbolType() const {
    return id_ == kSymbolId;
  }
  constexpr bool isEnvironmentType() const {
    return id_ == kEnvironmentId;
  }
  constexpr bool isPrivateNameType() const {
    return id_ == kPrivateNameId;
  }
  constexpr bool isFunctionCodeType() const {
    return id_ == kFunctionCodeId;
  }
  /// @}

  /// \return the raw type id. Used by lowering to populate side-array entries
  /// that store raw ids (e.g. FunctionParam.typeId).
  uint32_t getRawId() const {
    return id_;
  }

  /// Diagnostic placeholder used by streams that have no TypeContext
  /// available; prints `type#<id>`. For pretty names use
  /// `TypeContext::print(OS, t)` or `tc.fmt(t)` in a chained stream.
  void print(llvh::raw_ostream &OS) const;

  /// The hash of a Type is the hash of its opaque value.
  llvh::hash_code hash() const {
    return llvh::hash_value(id_);
  }

  constexpr bool operator==(Type RHS) const {
    return id_ == RHS.id_;
  }
  constexpr bool operator!=(Type RHS) const {
    return !(*this == RHS);
  }

  class iterator;

  /// Return an iterator over the types in this Type, using \p ctx for
  /// arm lookup.
  iterator begin(const TypeContext &ctx) const;
  /// Return an "end" iterator using \p ctx for arm lookup.
  iterator end(const TypeContext &ctx) const;

  /// Allow Type to be used as a llvh::FoldingSet.
  void Profile(llvh::FoldingSetNodeID &ID) const {
    ID.AddInteger(id_);
  }
};

static_assert(sizeof(Type) == 4, "Type must be 4 bytes");

/// Side array element for exact object fields (and class fields).
struct ExactObjectField {
  Identifier name;
  Type type;
};

/// An iterator over the types in a Type. For non-union types, yields
/// the type itself as the single element. For unions, fetches arms fresh
/// from a TypeContext on each dereference. Safe under TypeContext
/// mutation because typeArrays_ is append-only and a union's
/// armOffset/armCount are immutable once the union entry is created.
class Type::iterator {
  friend class Type;

  /// TypeContext for arm lookup. Always non-null.
  const TypeContext *ctx_;
  /// The type being iterated.
  Type type_;
  /// For non-union types: 0 = "this element", 1 = end.
  /// For union types: index into the arm array.
  unsigned index_;

  iterator(const TypeContext &ctx, Type type, unsigned index)
      : ctx_(&ctx), type_(type), index_(index) {}

 public:
  bool operator==(const iterator &RHS) const {
    return type_ == RHS.type_ && index_ == RHS.index_;
  }
  bool operator!=(const iterator &RHS) const {
    return !(*this == RHS);
  }

  iterator &operator++() {
    ++index_;
    return *this;
  }
  iterator operator++(int) {
    auto copy = *this;
    ++*this;
    return copy;
  }

  Type operator*() const;
};

/// A streamable wrapper around a Type that pretty-prints through a
/// TypeContext. Produced by `TypeContext::fmt(t)`. Lets chained `<<`
/// expressions format a Type by pretty name without splitting the
/// expression. Holds the context by pointer and the Type by value;
/// the referenced TypeContext must outlive the wrapper (which is
/// normally just a temporary in a single streaming expression).
struct PrintedType {
  const TypeContext *tc;
  Type t;
};

/// Owns the type table for a Module and provides type operations.
///
/// The constructor pre-allocates entries for all well-known types (primitives
/// and common unions). Well-known IDs have fixed values that are the same
/// across all TypeContext instances.
class TypeContext {
 public:
  TypeContext();

  /// Return the kind of the type entry for \p t.
  TypeKind getKind(Type t) const;

  /// Return the arm types of a union type entry. Asserts that \p t is a
  /// Union.
  /// \warning The returned ArrayRef points into an internal vector. It is
  /// invalidated by any operation that creates a new union type (e.g.
  /// unionTy, intersectTy, subtractTy) and by any later tuple creation or
  /// fill, since they may reallocate the underlying typeArrays_. Copy the arms
  /// into a local container before calling such operations during iteration.
  llvh::ArrayRef<Type> getUnionArms(Type t) const;

  /// Create a nominal class instance type. NOT interned: each call returns a
  /// fresh entry, so two structurally identical class declarations are
  /// different types. \p superClass is the parent ClassInstance, or NoType for
  /// a root class.
  Type createClassInstance(
      Type superClass,
      Identifier className,
      llvh::ArrayRef<ExactObjectField> fields);

  /// Reserve a ClassInstance entry whose superclass is \p superClass (the IR
  /// type of the parent class, or NoType for a root class) and return its
  /// Type. Used together with fillClassInstance to break self/mutual
  /// recursion when lowering a class whose fields reference the class itself.
  /// Between reserve and fill, only the kind, the id and the superclass of
  /// the entry are valid. They are all the type algebra reads from a class,
  /// so subtyping, union, intersection and disjointness involving the entry
  /// are already exact. Its name and fields (getClassName, getClassFields,
  /// printing) are valid only after fill.
  Type reserveClassInstance(Type superClass);

  /// Fill a previously reserved ClassInstance entry with its name and fields.
  /// \p t must come from reserveClassInstance. \p fields may point into this
  /// context (e.g. come from getClassFields).
  void fillClassInstance(
      Type t,
      Identifier className,
      llvh::ArrayRef<ExactObjectField> fields);

  /// \return the superclass of class \p t, or NoType if it is a root class.
  Type getClassSuperType(Type t) const;
  /// \return the name of class \p t.
  Identifier getClassName(Type t) const;
  /// \return the fields of class \p t.
  /// \warning The returned ArrayRef points into an internal vector. It is
  /// invalidated by any later class or exact object creation or fill, since
  /// that may reallocate the underlying fieldArrays_. Copy the fields into a
  /// local container before creating more such types during iteration.
  llvh::ArrayRef<ExactObjectField> getClassFields(Type t) const;

  /// \return true if class \p a is the same as or a subclass of class \p b.
  /// Both must be ClassInstance types.
  bool isSubclass(Type a, Type b) const;

  /// Create an Array type with element type \p elem. Interned: two calls with
  /// the same element type return the same Type. Allocates an entry only on a
  /// cache miss.
  Type createArray(Type elem);

  /// Reserve an empty Array entry and return its Type. Used with fillArray to
  /// break cycles when lowering a recursive array type. The entry is NOT
  /// interned. Its kind and id are valid immediately, so it may be passed to
  /// the type algebra (unionTy, isSubsetOf, areDisjoint, ...) before it is
  /// filled: the algebra reads only the kind and the id of an Array entry. Its
  /// payload (the element type) is valid only after fillArray, so accessors
  /// and printing must not be applied to it before then.
  Type reserveArray();

  /// Fill a previously reserved Array entry. \p t must come from reserveArray.
  void fillArray(Type t, Type elem);

  /// \return the element type of Array \p t.
  Type getArrayElementType(Type t) const;

  /// Create a Tuple type with element types \p elems, in order. Interned: two
  /// calls with the same element types in the same order return the same
  /// Type. Allocates an entry only on a cache miss.
  Type createTuple(llvh::ArrayRef<Type> elems);

  /// Reserve an empty Tuple entry and return its Type. Used with fillTuple to
  /// break cycles when lowering a recursive tuple type. The entry is NOT
  /// interned. Its kind and id are valid immediately, so it may be passed to
  /// the type algebra (unionTy, isSubsetOf, areDisjoint, ...) before it is
  /// filled: the algebra reads only the kind and the id of a Tuple entry. Its
  /// payload (the element types) is valid only after fillTuple, so accessors
  /// and printing must not be applied to it before then.
  Type reserveTuple();

  /// Fill a previously reserved Tuple entry with element types \p elems, in
  /// order. \p t must come from reserveTuple. \p elems may point into this
  /// context (e.g. come from getTupleElements).
  void fillTuple(Type t, llvh::ArrayRef<Type> elems);

  /// \return the element types of Tuple \p t, in order.
  /// \warning The returned ArrayRef points into an internal vector. It is
  /// invalidated by any later tuple creation or fill and by any operation that
  /// creates a new union type (e.g. unionTy, intersectTy, subtractTy), since
  /// that may reallocate the underlying typeArrays_. Copy the elements into a
  /// local container before calling such operations during iteration.
  llvh::ArrayRef<Type> getTupleElements(Type t) const;

  /// Create an ExactObject type with fields \p fields, in order. Interned by
  /// the ordered list of (name, type id) pairs: two calls with the same field
  /// names and types in the same order return the same Type. Field order is
  /// significant, so {x: number, y: string} and {y: string, x: number} are
  /// different types. Allocates an entry only on a cache miss. \p fields may
  /// point into this context.
  ///
  /// An ExactObject is a closed field list; a Flow exact object with an
  /// indexer is not represented by one (D7).
  ///
  /// Field variance is not part of the type (D8, phase3-5-design.md).
  /// ExactObjectField has no variance and the intern key has none, so the
  /// Flow types {+x: number}, {-x: number} and {x: number} all map to the same
  /// Type. The IR has no use for it: ExactObject types are compared by id
  /// only (D4), the IR never checks a write against a field type, and IRGen
  /// types a field read from the Flow field type. Because of this, the field
  /// types here are not an upper bound on the stored values (Flow lets
  /// {x: number | string} flow into {-x: number}), so do not type a loaded
  /// value from them. Adding deep structural subtyping, or any consumer of
  /// these field types, requires adding variance to the entry and the key.
  Type createExactObject(llvh::ArrayRef<ExactObjectField> fields);

  /// Reserve an empty ExactObject entry and return its Type. Used with
  /// fillExactObject to break cycles when lowering a recursive object type.
  /// The entry is NOT interned. Its kind and id are valid immediately, so it
  /// may be passed to the type algebra (unionTy, isSubsetOf, areDisjoint, ...)
  /// before it is filled: the algebra reads only the kind and the id of an
  /// ExactObject entry. Its payload (the fields) is valid only after
  /// fillExactObject, so accessors and printing must not be applied to it
  /// before then.
  Type reserveExactObject();

  /// Fill a previously reserved ExactObject entry with fields \p fields, in
  /// order. \p t must come from reserveExactObject. \p fields may point into
  /// this context (e.g. come from getExactObjectFields or getClassFields).
  void fillExactObject(Type t, llvh::ArrayRef<ExactObjectField> fields);

  /// \return the fields of ExactObject \p t, in order.
  /// \warning The returned ArrayRef points into an internal vector. It is
  /// invalidated by any later class or exact object creation or fill, since
  /// that may reallocate the underlying fieldArrays_. Copy the fields into a
  /// local container before creating more such types during iteration.
  llvh::ArrayRef<ExactObjectField> getExactObjectFields(Type t) const;

  /// Create a Function type. Interned: two calls with an identical signature
  /// return the same Type. Allocates an entry only on a cache miss. \p thisTy
  /// and \p restTy may be NoType when absent.
  Type createFunction(
      Type ret,
      Type thisTy,
      Type restTy,
      llvh::ArrayRef<FunctionParam> params,
      bool isAsync,
      bool isGenerator);

  /// Reserve an empty Function entry. Used with fillFunction to break cycles
  /// when lowering a recursive function type. The entry is NOT interned. Its
  /// kind and id are valid immediately, so it may be passed to the type
  /// algebra (unionTy, isSubsetOf, areDisjoint, ...) before it is filled: the
  /// algebra reads only the kind and the id of a Function entry. Its payload
  /// (the signature) is valid only after fillFunction, so accessors and
  /// printing must not be applied to it before then.
  Type reserveFunction();

  /// Fill a previously reserved Function entry. \p t must come from
  /// reserveFunction. \p params may point into this context (e.g. come from
  /// getFunctionParams).
  void fillFunction(
      Type t,
      Type ret,
      Type thisTy,
      Type restTy,
      llvh::ArrayRef<FunctionParam> params,
      bool isAsync,
      bool isGenerator);

  /// \return the return type of Function \p t.
  Type getFunctionReturn(Type t) const;
  /// \return the "this" param type of Function \p t (NoType if absent).
  Type getFunctionThis(Type t) const;
  /// \return the rest param type of Function \p t (NoType if absent).
  Type getFunctionRest(Type t) const;
  /// \return the params of Function \p t, in order.
  /// \warning The returned ArrayRef points into an internal vector. It is
  /// invalidated by any later function creation or fill, since that may
  /// reallocate the underlying paramArrays_. Copy the params into a local
  /// container before creating more function types during iteration.
  llvh::ArrayRef<FunctionParam> getFunctionParams(Type t) const;
  /// \return whether Function \p t is async.
  bool getFunctionIsAsync(Type t) const;
  /// \return whether Function \p t is a generator.
  bool getFunctionIsGenerator(Type t) const;

  /// \return true if \p t is NoType (empty set).
  bool isNoType(Type t) const;

  /// \return true if \p t can represent a Number value.
  bool canBeNumber(Type t) const;
  /// \return true if \p t can represent a String value.
  bool canBeString(Type t) const;
  /// \return true if \p t can represent an Object value.
  bool canBeObject(Type t) const;
  /// \return true if \p t can represent a Null value.
  bool canBeNull(Type t) const;
  /// \return true if \p t can represent an Undefined value.
  bool canBeUndefined(Type t) const;
  /// \return true if \p t can represent an Empty value.
  bool canBeEmpty(Type t) const;
  /// \return true if \p t can represent an Uninit value.
  bool canBeUninit(Type t) const;
  /// \return true if \p t can represent a BigInt value.
  bool canBeBigInt(Type t) const;
  /// \return true if \p t can represent a Boolean value.
  bool canBeBoolean(Type t) const;
  /// \return true if \p t can represent a Symbol value.
  bool canBeSymbol(Type t) const;

  /// \return true if \p t represents only primitive types
  /// (Number, String, BigInt, Null, Undefined, Boolean, Symbol).
  /// Returns false for NoType.
  bool isPrimitive(Type t) const;

  /// \return true if any of the types in \p t are primitive.
  bool canBePrimitive(Type t) const;

  /// \return true if a class field of type \p t must be stored uninitialized
  /// and IDZ-checked on load because it has no representable literal default.
  /// True for object types and symbol; false for non-symbol primitives (which
  /// default to 0/""/false/0n/undefined/null) and for any (defaults to
  /// undefined).
  bool isIDZType(Type t) const;

  /// \return true if \p t is not referenced by a pointer
  /// (Number, Boolean, Null, Undefined only). Returns false for NoType.
  bool isNonPtr(Type t) const;

  /// \return true if \p t is statically known to be exactly one JavaScript
  /// primitive type: number, string, boolean, bigint, symbol, null, or
  /// undefined. "Primitive" is meant in the JavaScript sense, not the
  /// representation sense: the number subtypes Int32/Uint32/UInt31 -- and any
  /// union of them, e.g. Int32|Uint32 -- all count as the single primitive
  /// "number". Returns false for NoType, for Object, and for any type that
  /// mixes more than one primitive or contains a non-primitive component.
  bool isKnownPrimitiveType(Type t) const;

  /// \return true if \p t is a superset of AnyType (i.e. it can hold "any"
  /// JS-observable value).
  bool canBeAny(Type t) const;

  /// \return true if all values of type \p a are also values of type \p b.
  bool isSubsetOf(Type a, Type b) const;

  /// \return true if every value of \p b is a possible value of \p a.
  /// Equivalent to `isSubsetOf(b, a)`.
  bool canBeType(Type a, Type b) const;

  /// \return true if \p a is a proper subset of \p b (subset and not equal).
  bool isProperSubsetOf(Type a, Type b) const;

  /// \return true if types \p a and \p b have no values in common.
  bool areDisjoint(Type a, Type b) const;

  /// \return the union of types \p a and \p b. May create and intern a new
  /// union type.
  Type unionTy(Type a, Type b);

  /// \return the intersection of types \p a and \p b, or, where that is not
  /// expressible, a type that contains it and is a subset of \p a.
  ///
  /// The result is exact, and symmetric, except when neither operand is a
  /// subset of the other and a leaf of \p a and a leaf of \p b overlap
  /// without either being a subtype of the other: two distinct structural
  /// refined types (e.g. two different function types), or a class and a
  /// structural refined type. Shallow subtyping (D4) cannot express their
  /// meet, so the leaf of \p a stands in for it (decision D9 in
  /// doc/plans/ir-type/phase3-5-design.md). The result is therefore always a
  /// subset of \p a, but not necessarily of \p b.
  ///
  /// Argument-order contract: pass the type whose shape the caller keeps (the
  /// target type of a cast or a narrow, a declared type that bounds an
  /// inferred one) as \p a. Whether the result is NoType does not depend on
  /// the order, so callers that only test for NoType may pass either order.
  Type intersectTy(Type a, Type b);

  /// \return type \p a minus type \p b (conservative approximation).
  Type subtractTy(Type a, Type b);

  /// \return the number of kinds in the type: 0 for NoType, arm count for
  /// unions, 1 for leaf types.
  unsigned countKinds(Type t) const;

  /// \return the TypeKind of the type. For unions, returns the kind of the
  /// first arm. For NoType, returns TypeKind::NoType.
  TypeKind getFirstKind(Type t) const;

  /// Print the human-readable type name to \p OS. Leaf kinds print their name
  /// (e.g. "number"). Unions print pipe-separated arms (e.g. "number|string").
  /// NoType prints "notype". AnyType prints "any". A class instance prints
  /// "classInstance(<name>)". The structural kinds print their payload:
  /// - an array prints "array(<element>)";
  /// - a function prints "function(<return>)" (parameters are not printed);
  /// - a tuple prints "tuple(<e1>, <e2>, ...)", and the empty tuple "tuple()";
  /// - an exact object prints "exactObject(<name1>: <t1>, <name2>: <t2>, ...)"
  ///   in field order, and the empty object "exactObject()".
  /// Each payload type is printed recursively by these same rules, so a union
  /// payload prints its arms (e.g. "array(string|number)",
  /// "tuple(string|number, null)").
  ///
  /// Printing terminates on recursive types. A structural type (array,
  /// function, tuple or exact object) that is reached again while its own
  /// payload is being printed prints as the back-reference "^N", where N
  /// counts the enclosing structural types, of all four kinds, from the
  /// innermost one (N = 1) outwards to the one being referenced.
  /// For example, `type A = A[]` prints "array(^1)",
  /// `type F = () => F | null` prints "function(null|^1)",
  /// `type M = M[][]` prints "array(array(^2))", which differs from the
  /// "array(array(^1))" of `N[]` with `type N = N[]`, and
  /// `type T = [number, T[]]` prints "tuple(number, array(^2))".
  void print(llvh::raw_ostream &OS, Type t) const;

  /// \return a range over the component types of \p t, suitable for
  /// range-for. For non-union types, yields the type itself as a single
  /// element. For unions, yields each arm. For NoType, yields nothing.
  /// The TypeContext must outlive the returned range.
  llvh::iterator_range<Type::iterator> arms(Type t) const;

  /// Wrap \p t as a streamable value that pretty-prints through this
  /// context. Lets chained `<<` expressions format Types by pretty name:
  ///   `OS << "x = " << tc.fmt(t) << "\n";`
  PrintedType fmt(Type t) const;

#ifdef UNIT_TEST
  /// Expose the stored primMask for a type.
  uint16_t testPrimMask(Type t) const {
    return entries_[t.id_].primMask;
  }
  /// Drive the mask->Type cache directly.
  Type testLookupPrimMask(uint16_t m) {
    return lookupPrimMask(m);
  }
#endif

 private:
  friend class Type;

  /// Append the printed form of the type with id \p id, in the format
  /// documented at print(), to \p OS. \p enclosing holds the ids of the
  /// structural types (arrays, functions, tuples and exact objects) whose
  /// payload is currently being printed, outermost first; it is used to print
  /// back-references and is restored on return.
  void printType(
      llvh::raw_ostream &OS,
      uint32_t id,
      llvh::SmallVectorImpl<uint32_t> &enclosing) const;

  /// Append the printed form of the single non-union, non-NoType type with id
  /// \p id to \p OS, in the format documented at print(). If \p id is a
  /// structural type already in \p enclosing (see printType()), print the
  /// back-reference "^N" instead of its payload. Otherwise a structural type
  /// is pushed onto \p enclosing while its payload is printed.
  void printLeaf(
      llvh::raw_ostream &OS,
      uint32_t id,
      llvh::SmallVectorImpl<uint32_t> &enclosing) const;

  /// Append the payload of the array, function, tuple or exact object with id
  /// \p id to \p OS: the text between the parentheses in the format
  /// documented at print(). \p id must already be the innermost entry of
  /// \p enclosing (see printType()).
  void printStructuralPayload(
      llvh::raw_ostream &OS,
      uint32_t id,
      llvh::SmallVectorImpl<uint32_t> &enclosing) const;

  /// Id-aware leaf subtype test for two non-union types. Distinguishes nominal
  /// classes (which share the ClassInstance kind). Falls back to the
  /// kind-based isLeafSubtype for non-refined kinds.
  bool isLeafSubtypeById(uint32_t a, uint32_t b) const;

  /// Id-aware disjointness test for two non-union types. Conservative for
  /// structural refined kinds (D5): only unrelated classes and refined-vs-
  /// primitive are declared disjoint.
  bool areLeafTypesDisjointById(uint32_t a, uint32_t b) const;

  /// Type table. Index 0 = NoType. Pre-allocated entries for primitives.
  std::vector<TypeEntry> entries_;

  /// Side array storing union arm types and tuple element types, referenced by
  /// (armOffset, armCount) in Union entries and (elemOffset, elemCount) in
  /// Tuple entries. Type is layout-compatible with uint32_t (it wraps a
  /// uint32_t id).
  std::vector<Type> typeArrays_;

  /// Side array storing class and exact object field descriptors, referenced
  /// by (fieldOffset, fieldCount) in ClassInstance/ExactObject entries.
  std::vector<ExactObjectField> fieldArrays_;

  /// Side array storing function parameters, referenced by
  /// (paramOffset, paramCount) in Function entries.
  std::vector<FunctionParam> paramArrays_;

  /// Intern table mapping sorted arm sets to existing union type IDs.
  llvh::DenseMap<UnionInternKey, uint32_t, UnionInternKeyInfo> internTable_;

  /// Intern table mapping an element type id to the canonical Array type id.
  /// Non-recursive arrays only; recursive arrays are built via reserve/fill.
  llvh::DenseMap<uint32_t, uint32_t> arrayInternTable_;

  /// Intern table mapping an ordered element type id list to the canonical
  /// Tuple id. Non-recursive tuples only; recursive tuples are built via
  /// reserve/fill.
  llvh::DenseMap<TupleInternKey, uint32_t, TupleInternKeyInfo>
      tupleInternTable_;

  /// Intern table mapping an ordered (name, type id) field list to the
  /// canonical ExactObject id. Non-recursive objects only; recursive objects
  /// are built via reserve/fill.
  llvh::DenseMap<ExactObjectInternKey, uint32_t, ExactObjectInternKeyInfo>
      exactObjectInternTable_;

  /// Intern table mapping a function signature to the canonical Function id.
  /// Non-recursive functions only; recursive functions are built via
  /// reserve/fill.
  llvh::DenseMap<FunctionInternKey, uint32_t, FunctionInternKeyInfo>
      functionInternTable_;

  /// Number of distinct primitive masks (the kPrimMaskBits index space).
  static constexpr uint32_t kNumPrimMasks = 1u << type_internal::kPrimMaskBits;

  /// Cache mapping a primitive mask to its canonical Type, lazily populated.
  /// Pure accelerator in front of internTable_; see lookupPrimMask. The live
  /// working set is tiny (tens of entries on real programs), so a hash map
  /// beats a dense kNumPrimMasks-slot array on cache locality.
  llvh::DenseMap<uint32_t, Type> primCache_;

  /// Return true if any component of the type at \p id satisfies \p pred.
  /// For leaf types, tests the kind directly. For unions, tests any arm.
  template <typename Pred>
  bool containsMatchingKind(uint32_t id, Pred pred) const;

  /// Return true if all components of the type at \p id satisfy \p pred.
  /// Returns false for NoType.
  template <typename Pred>
  bool allMatchKind(uint32_t id, Pred pred) const;

  /// Helper to append arms to typeArrays_ and create a union entry.
  uint32_t addUnionEntry(llvh::ArrayRef<uint32_t> arms);

  /// Intersect two non-union, non-empty type IDs. The result is exact unless
  /// \p a and \p b overlap without either being a subtype of the other (two
  /// distinct structural refined types, or a class and a structural refined
  /// type). Their meet is not expressible, so \p a is returned (D9). The
  /// result is therefore always a subtype of \p a but not necessarily of \p b,
  /// and callers must pass the leaf whose type they keep as \p a.
  uint32_t intersectLeafTy(uint32_t a, uint32_t b) const;

  /// Create a union from two operands with full canonicalization and interning.
  /// Flattens, deduplicates, removes subsumed arms, sorts, and interns.
  uint32_t createUnionImpl(uint32_t a, uint32_t b);

  /// Canonicalize a list of non-union type IDs into an interned union.
  uint32_t createUnionFromLeafArms(llvh::ArrayRef<uint32_t> arms);

  /// Look up (or lazily create) the canonical Type for primitive mask \p m.
  Type lookupPrimMask(uint16_t m);
  /// Build the canonical Type for primitive mask \p m by decoding it to leaf
  /// ids and interning through the existing union machinery.
  Type materializePrimMask(uint16_t m);
};

inline llvh::raw_ostream &operator<<(llvh::raw_ostream &OS, PrintedType pt) {
  pt.tc->print(OS, pt.t);
  return OS;
}

inline PrintedType TypeContext::fmt(Type t) const {
  return PrintedType{this, t};
}
} // namespace hermes

namespace llvh {
template <>
struct FoldingSetTrait<hermes::Type> {
  static inline void Profile(hermes::Type t, FoldingSetNodeID &ID) {
    t.Profile(ID);
  }
};
} // namespace llvh

#endif // HERMES_IR_TYPECONTEXT_H
