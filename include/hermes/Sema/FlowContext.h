/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SEMA_FLOWCONTEXT_H
#define HERMES_SEMA_FLOWCONTEXT_H

#include "hermes/AST/NativeContext.h"
#include "hermes/Sema/SemContext.h"

#include "llvh/ADT/DenseSet.h"
#include "llvh/ADT/MapVector.h"
#include "llvh/ADT/SetVector.h"

namespace hermes {

namespace ESTree {
class MethodDefinitionNode;
}

namespace flow {

#define _HERMES_SEMA_FLOW_SINGLETONS              \
  _HERMES_SEMA_FLOW_DEFKIND(Void)                 \
  _HERMES_SEMA_FLOW_DEFKIND(Null)                 \
  _HERMES_SEMA_FLOW_DEFKIND(Boolean)              \
  _HERMES_SEMA_FLOW_DEFKIND(String)               \
  _HERMES_SEMA_FLOW_DEFKIND(CPtr)                 \
  _HERMES_SEMA_FLOW_DEFKIND(Generic)              \
  _HERMES_SEMA_FLOW_DEFKIND(InferencePlaceholder) \
  _HERMES_SEMA_FLOW_DEFKIND(Number)               \
  _HERMES_SEMA_FLOW_DEFKIND(Symbol)               \
  _HERMES_SEMA_FLOW_DEFKIND(BigInt)               \
  _HERMES_SEMA_FLOW_DEFKIND(Any)                  \
  _HERMES_SEMA_FLOW_DEFKIND(Empty)                \
  _HERMES_SEMA_FLOW_DEFKIND(Mixed)

/// The literal kinds must be listed last here, see
/// _HERMES_SEMA_FLOW_LITERAL_BASES.
#define _HERMES_SEMA_FLOW_COMPLEX_TYPES                \
  _HERMES_SEMA_FLOW_DEFKIND(Union)                     \
  _HERMES_SEMA_FLOW_DEFKIND(Tuple)                     \
  _HERMES_SEMA_FLOW_DEFKIND(ExactObject)               \
  _HERMES_SEMA_FLOW_DEFKIND(UntypedFunction)           \
  _HERMES_SEMA_FLOW_DEFKIND(TypedFunction)             \
  _HERMES_SEMA_FLOW_DEFKIND(NativeFunction)            \
  _HERMES_SEMA_FLOW_DEFKIND(Class)                     \
  _HERMES_SEMA_FLOW_DEFKIND(ClassConstructor)          \
  _HERMES_SEMA_FLOW_DEFKIND(InferencePlaceholderArray) \
  _HERMES_SEMA_FLOW_DEFKIND(StringLiteral)             \
  _HERMES_SEMA_FLOW_DEFKIND(NumberLiteral)             \
  _HERMES_SEMA_FLOW_DEFKIND(BooleanLiteral)

/// Each literal TypeKind and the primary TypeKind it widens into. Every kind
/// in [_FirstLiteral, _LastLiteral] must appear here, and they must be the
/// last kinds in \c TypeKind.
#define _HERMES_SEMA_FLOW_LITERAL_BASES               \
  _HERMES_SEMA_FLOW_DEFLITERAL(StringLiteral, String) \
  _HERMES_SEMA_FLOW_DEFLITERAL(NumberLiteral, Number) \
  _HERMES_SEMA_FLOW_DEFLITERAL(BooleanLiteral, Boolean)

enum class TypeKind : uint8_t {
#define _HERMES_SEMA_FLOW_DEFKIND(name) name,
  _HERMES_SEMA_FLOW_SINGLETONS _HERMES_SEMA_FLOW_COMPLEX_TYPES
#undef _HERMES_SEMA_FLOW_DEFKIND

      _FirstPrimary = Void,
  _LastPrimary = BigInt,
  _FirstSingleton = Void,
  _LastSingleton = Mixed,
  _FirstId = Class,
  _LastId = ClassConstructor,
  _FirstFunction = UntypedFunction,
  _LastFunction = NativeFunction,
  _FirstLiteral = StringLiteral,
  _LastLiteral = BooleanLiteral,
};

/// The number of distinct TypeKind values.
constexpr unsigned kNumTypeKinds = 0
#define _HERMES_SEMA_FLOW_DEFKIND(name) +1
    _HERMES_SEMA_FLOW_SINGLETONS _HERMES_SEMA_FLOW_COMPLEX_TYPES
#undef _HERMES_SEMA_FLOW_DEFKIND
    ;

static_assert(
    (unsigned)TypeKind::_LastLiteral + 1 == kNumTypeKinds,
    "literal kinds must sort last; union literal absorption relies on it");

static_assert(
    0
#define _HERMES_SEMA_FLOW_DEFLITERAL(name, base) +1
        _HERMES_SEMA_FLOW_LITERAL_BASES
#undef _HERMES_SEMA_FLOW_DEFLITERAL
        == (int)TypeKind::_LastLiteral - (int)TypeKind::_FirstLiteral + 1,
    "every literal TypeKind needs an entry in "
    "_HERMES_SEMA_FLOW_LITERAL_BASES");

/// \return \p kind widened to the primary kind it represents, if it is a
/// literal kind. Otherwise \p kind unchanged.
TypeKind widenLiteralKind(TypeKind kind);

/// The backing storage for Types.
/// Indicates the TypeKind and can be compared for equality, etc.
class TypeInfo {
  TypeKind const kind_;
  /// Set to true once the type has been calculated.
  mutable bool hashed_ = false;
  /// Valid only if \c hashed_ is set to true, this is the calculated hash value
  /// of this type.
  mutable unsigned hashValue_;

 public:
  explicit TypeInfo(TypeKind kind) : kind_(kind) {}

  static bool classof(const TypeInfo *) {
    return true;
  }

  bool isSingleton() const {
    return kind_ <= TypeKind::_LastSingleton;
  }

  /// \return true if this is a literal type (e.g. a string literal type).
  bool isLiteral() const {
    return kind_ >= TypeKind::_FirstLiteral && kind_ <= TypeKind::_LastLiteral;
  }

  TypeKind getKind() const {
    return kind_;
  }

  /// Return the TypeKind as a string.
  llvh::StringRef getKindName() const;

  /// Comparison stores visited set for a pair of (this, other).
  /// If the comparison diverges or terminates prior to seeing a visited pair,
  /// it operates as if there was no visited set in the usual fashion.
  /// However, if we see a visited pair, that must mean we haven't diverged
  /// prior to it, and we can know that the Types must be equal.
  /// The kinds must have been checked somewhere else, or are being checked up
  /// the callstack.
  /// If we see the same pair again, we can't perform any new work because we
  /// have no new information at that point.
  class CompareState {
   public:
    /// Pairs of (this, other) that we have visited.
    llvh::SetVector<std::pair<const TypeInfo *, const TypeInfo *>> visited{};

    /// Cache for memoizing computation that could otherwise be slow.
    /// Pairs of (this, other) and the result of the comparison/equality.
    /// Note that for equals(), the value is a bool stored as int, so it can be
    /// cast directly to bool for returning.
    /// equals() only caches false: a true result may depend on a pair still
    /// in \c visited being assumed equal, and that assumption can later fail.
    /// Never gets elements popped during comparison, unlike the visited set.
    llvh::SmallDenseMap<std::pair<const TypeInfo *, const TypeInfo *>, int>
        cache{};
  };

  /// Compare this type and other type lexicographically and return -1, 0, 1
  /// correspondingly.
  /// The less than and greater than comparisons are, in some sense, arbitrary,
  /// they only need to be consistent, so we can sort types, but don't have an
  /// inherent "meaning".
  /// Equality however is well-defined: it compares structural types "deeply"
  /// and nominal types (subclasses of \c TypeWithId) "shallowly".
  ///
  /// The types must be known to be non-looping; looping unions
  /// are not comparable because we have no good way to order cycles.
  /// This restriction isn't enforced - the caller must know the types are
  /// non-looping and breaking this precondition will result in unpredictable
  /// behavior.
  int compare(const TypeInfo *other) const;

  /// \param state the state for tracking cycles through the comparison.
  int compare(const TypeInfo *other, CompareState &state) const;

  /// Check whether types are equal.
  /// May be used on looping types (with known looping union arms).
  bool equals(const TypeInfo *other) const;

  /// \param state the state for tracking cycles through the comparison.
  ///   The state is restored to its pre-call state when the function returns.
  bool equals(const TypeInfo *other, CompareState &state) const;

  /// Calculate a shallow hash of this type for hash tables, etc.
  /// Types that compare equal using \c compare() must have the same hash.
  /// However, the hash of each type is cached after it is calculated the first
  /// time, so the amortized cost should be O(1).
  /// TODO: The cache mechanism is kept around for faster lookups and to reduce
  /// calls to llvh::hash_value, along with the ability to make hashing slightly
  /// more expensive and deeper even if we aren't able to hash the entire
  /// structure always.
  unsigned hash() const;
};

/// Used when TypeInfo is the key in, e.g. std::map.
struct TypeInfoLessThan {
  bool operator()(const TypeInfo *lhs, const TypeInfo *rhs) const {
    return lhs->compare(rhs) < 0;
  }
};

/// An instance of a creation of a Type.
/// Associates the backing storage TypeInfo with the AST node that created it.
class Type {
 public:
  /// Backing storage indicating the kind of the type.
  /// nullptr before it's initialized.
  TypeInfo *info = nullptr;

  /// If non-null, the AST node used for the source location for this type.
  /// If null, this type was created for some reason that was unassociated with
  /// JS source.
  ESTree::Node *node = nullptr;

  /// Whether this type is a "looping" type,
  /// and should be treated specially when used in a union.
  bool isLooping = false;

  explicit Type(TypeInfo *info = nullptr, ESTree::Node *node = nullptr)
      : info(info), node(node) {}

  /// \return a string representation of the type suitable for error messages.
  std::string messageString() const;
};

class SingletonType : public TypeInfo {
 public:
  explicit SingletonType(TypeKind kind) : TypeInfo(kind) {
    assert(classof(this) && "Invalid SingletonType kind");
  }
  static bool classof(const TypeInfo *t) {
    return t->getKind() >= TypeKind::_FirstSingleton &&
        t->getKind() <= TypeKind::_LastSingleton;
  }

  /// Compare two instances of the same TypeKind.
  int _compareImpl(const SingletonType *other, CompareState &state) const {
    assert(
        this->getKind() == other->getKind() &&
        "only the same TypeKind can be compared");
    return 0;
  }
  /// Compare two instances of the same TypeKind.
  bool _equalsImpl(const SingletonType *other, CompareState &state) const {
    return _compareImpl(other, state) == 0;
  }
  /// Calculate the type-specific hash.
  unsigned _hashImpl() const {
    assert(getKind() != TypeKind::Generic && "generic type is not hashable");
    return (unsigned)getKind();
  }
};

class PrimaryType : public SingletonType {
 public:
  explicit PrimaryType(TypeKind kind) : SingletonType(kind) {
    assert(classof(this) && "Invalid PrimaryType kind");
  }

  static bool classof(const TypeInfo *t) {
    return t->getKind() >= TypeKind::_FirstPrimary &&
        t->getKind() <= TypeKind::_LastPrimary;
  }
};

template <TypeKind KIND, class BASE>
class SingleType : public BASE {
 public:
  explicit SingleType() : BASE(KIND) {}
  static bool classof(const TypeInfo *t) {
    return t->getKind() == KIND;
  }
};

using VoidType = SingleType<TypeKind::Void, PrimaryType>;
using NullType = SingleType<TypeKind::Null, PrimaryType>;
using BooleanType = SingleType<TypeKind::Boolean, PrimaryType>;
using StringType = SingleType<TypeKind::String, PrimaryType>;
using CPtrType = SingleType<TypeKind::CPtr, PrimaryType>;
/// Placeholder for unspecialized generic class or type alias.
/// Must not escape the FlowChecker, and will be replaced by a concrete type.
using GenericType = SingleType<TypeKind::Generic, PrimaryType>;
using NumberType = SingleType<TypeKind::Number, PrimaryType>;
using SymbolType = SingleType<TypeKind::Symbol, PrimaryType>;
using BigIntType = SingleType<TypeKind::BigInt, PrimaryType>;

using InferencePlaceholderType =
    SingleType<TypeKind::InferencePlaceholder, PrimaryType>;

using AnyType = SingleType<TypeKind::Any, SingletonType>;
using MixedType = SingleType<TypeKind::Mixed, SingletonType>;
using EmptyType = SingleType<TypeKind::Empty, SingletonType>;

/// Unions are handled specially because their existence and layout is dependent
/// on their arms.
///
/// Some of the union arms may be "looping":
/// they may point to types which eventually point back to themselves,
/// meaning there's no end point to the recursive comparison.
/// Comparable union arms are non-looping.
///
/// For unions which may be looping (e.g. via type aliases but not via
/// nominal types), they are first populated with unordered arms.
/// The arms are sorted and uniqued during type alias resolution,
/// with the looping types split from canonicalized types (sorted, uniqued).
/// Looping types can be uniqued slowly using \c TypeInfo::equals.
/// Once canonicalization happens, they are registered as canonicalized in
/// UnionType.
///
/// Unions can also be created with known non-looping/looping types.
///
/// In either case, UnionType will not be used when it is known to only have one
/// type following canonicalization.
/// It will be replaced with the single type.
class UnionType : public SingleType<TypeKind::Union, TypeInfo> {
  /// When numNonLoopingTypes_ is non-negative:
  ///   The first numNonLoopingTypes_ are canonical (sorted, uniqued).
  ///   The other types are recursive.
  /// When numNonLoopingTypes_ is -1:
  ///   No types have been checked for canonicalization.
  llvh::SmallVector<Type *, 4> types_{};

  /// Number of types at the start of types_ that are known to be non-recursive,
  /// sorted, and uniqued.
  /// -1 prior to any processing (if the types have no known information yet).
  int32_t numNonLoopingTypes_ = -1;

  bool hasAny_ = false;
  bool hasMixed_ = false;

 public:
  /// Create a UnionType with an unknown list of types to be canonicalized
  /// later.
  explicit UnionType(llvh::SmallVector<Type *, 4> &&types);
  /// Create a UnionType already knowing the canonical and recursive types.
  explicit UnionType(
      llvh::SmallVector<Type *, 4> &&nonLoopingTypes,
      llvh::SmallVector<Type *, 4> &&loopingTypes) {
    setCanonicalTypes(std::move(nonLoopingTypes), std::move(loopingTypes));
  }

  /// Set the type arms for the union after the canonical representation is
  /// known.
  void setCanonicalTypes(
      llvh::SmallVector<Type *, 4> &&nonLoopingTypes,
      llvh::SmallVector<Type *, 4> &&loopingTypes);

  /// \return the number of known non-looping types if that has been determined,
  /// -1 if it hasn't.
  /// The number will have been calculated during FlowChecker,
  /// so callers after the FlowChecker has run generally don't have to worry
  /// about it being -1.
  int32_t getNumNonLoopingTypes() const {
    return numNonLoopingTypes_;
  }

  /// \return the members of the union.
  llvh::ArrayRef<Type *> getTypes() const {
    return types_;
  }
  /// \return the non-looping members of the union.
  llvh::ArrayRef<Type *> getNonLoopingTypes() const {
    assert(numNonLoopingTypes_ >= 0 && "types haven't been canonicalized yet");
    return getTypes().take_front(numNonLoopingTypes_);
  }
  /// \return the looping members of the union.
  llvh::ArrayRef<Type *> getLoopingTypes() const {
    assert(numNonLoopingTypes_ >= 0 && "types haven't been canonicalized yet");
    return getTypes().drop_front(numNonLoopingTypes_);
  }

  bool hasAny() const {
    assert(numNonLoopingTypes_ >= 0 && "types haven't been canonicalized yet");
    return hasAny_;
  }
  bool hasMixed() const {
    assert(numNonLoopingTypes_ >= 0 && "types haven't been canonicalized yet");
    return hasMixed_;
  }
  /// \return true if the union contains a "void" arm.
  bool hasVoid() const {
    assert(numNonLoopingTypes_ >= 0 && "types haven't been canonicalized yet");
    return llvh::isa<VoidType>(types_[0]->info);
  }
  /// \return true if the union contains a "null" arm.
  bool hasNull() const {
    assert(numNonLoopingTypes_ >= 0 && "types haven't been canonicalized yet");
    static_assert(
        (int)TypeKind::Null == (int)TypeKind::Void + 1,
        "null and void aren't adjacent");
    // Null must be either in the first or second slot.
    if (llvh::isa<NullType>(types_[0]->info))
      return true;
    return types_.size() > 1 && llvh::isa<NullType>(types_[1]->info);
  }

  /// Compare two instances of the same TypeKind.
  int _compareImpl(const UnionType *other, CompareState &state) const;
  /// Compare two instances of the same TypeKind.
  bool _equalsImpl(const UnionType *other, CompareState &state) const;
  /// Calculate the type-specific hash.
  unsigned _hashImpl() const;

  /// Collapse nested unions, remove duplicates,
  /// and deterministically sort the types.
  /// Categorize the resulting types into non-looping and looping.
  /// NOTE: This function isn't to be used on type aliases, which can have
  /// union-only cycles, multiple layers of nested unions, etc. Those are to be
  /// resolved in the FlowChecker's DeclareScopeTypes call.
  /// This function only works when every element of \p types has itself already
  /// been correctly resolved, with nested unions collapsed and no duplicates.
  /// \param booleanType the boolean singleton, which replaces `true | false`.
  static void canonicalizeTypes(
      llvh::ArrayRef<Type *> types,
      llvh::SmallVectorImpl<Type *> &nonLoopingTypes,
      llvh::SmallVectorImpl<Type *> &loopingTypes,
      Type *booleanType);

  /// Unique types and sort them according to \c TypeInfo::compare. Also
  /// remove the literal types absorbed by another arm: a literal is absorbed
  /// by its base type (`string | "abc"` is `string`), by `any` and by
  /// `mixed`. `true | false` is replaced with \p booleanType.
  /// \param types the types that are known to be non-recursive.
  static void sortAndUniqueNonLoopingTypes(
      llvh::SmallVectorImpl<Type *> &types,
      Type *booleanType);

  /// Remove all the duplicates in the list of looping \p types.
  /// Runs in quadratic time on the length of the types because they can't be
  /// sorted.
  static void uniqueLoopingTypesSlow(llvh::SmallVectorImpl<Type *> &types);

 private:
  // Iterate types_ and set the hasAny_ and hasMixed_ flags.
  void setHasAnyMixed();
};

/// Array type used as a constraint during type argument inference.
/// Wraps an element type that is an InferencePlaceholderType, allowing
/// matchConstraintToType to recurse into the element to resolve the
/// placeholder against an actual Array class type.
class InferencePlaceholderArrayType
    : public SingleType<TypeKind::InferencePlaceholderArray, TypeInfo> {
  Type *element_ = nullptr;

 public:
  /// Initialize a new instance.
  explicit InferencePlaceholderArrayType(Type *element) : element_(element) {}

  Type *getElement() const {
    return element_;
  }

  /// Compare two instances of the same TypeKind.
  int _compareImpl(
      const InferencePlaceholderArrayType *other,
      CompareState &state) const;
  /// Compare two instances of the same TypeKind.
  bool _equalsImpl(
      const InferencePlaceholderArrayType *other,
      CompareState &state) const;
  /// Calculate the type-specific hash.
  unsigned _hashImpl() const;
};

class TupleType : public TypeInfo {
  llvh::SmallVector<Type *, 4> types_{};

 public:
  /// Initialize a new instance.
  explicit TupleType(llvh::ArrayRef<Type *> types) : TypeInfo(TypeKind::Tuple) {
    types_.append(types.begin(), types.end());
  }

  llvh::ArrayRef<Type *> getTypes() const {
    return types_;
  }

  /// Compare two instances of the same TypeKind.
  int _compareImpl(const TupleType *other, CompareState &state) const;
  /// Compare two instances of the same TypeKind.
  bool _equalsImpl(const TupleType *other, CompareState &state) const;
  /// Calculate the type-specific hash.
  unsigned _hashImpl() const;

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::Tuple;
  }
};

/// The type of a single string literal, e.g. the type "foo". Carries the
/// interned literal value. A StringLiteralType flows into String, and into
/// another StringLiteralType only when the value is identical.
class StringLiteralType : public TypeInfo {
  /// The interned literal value.
  UniqueString *value_;

 public:
  explicit StringLiteralType(UniqueString *value)
      : TypeInfo(TypeKind::StringLiteral), value_(value) {
    assert(value && "value must be non-null");
  }

  /// \return the interned literal value.
  UniqueString *getValue() const {
    return value_;
  }

  int _compareImpl(const StringLiteralType *other, CompareState &state) const;
  bool _equalsImpl(const StringLiteralType *other, CompareState &state) const;
  unsigned _hashImpl() const;

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::StringLiteral;
  }
};

/// The type of a single numeric literal, e.g. the type 1. Carries the literal
/// value. A NumberLiteralType flows into Number, and into another
/// NumberLiteralType only when the value is identical.
class NumberLiteralType : public TypeInfo {
  /// The literal value.
  double value_;

 public:
  explicit NumberLiteralType(double value)
      : TypeInfo(TypeKind::NumberLiteral), value_(value) {}

  /// \return the literal value.
  double getValue() const {
    return value_;
  }

  int _compareImpl(const NumberLiteralType *other, CompareState &state) const;
  bool _equalsImpl(const NumberLiteralType *other, CompareState &state) const;
  unsigned _hashImpl() const;

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::NumberLiteral;
  }
};

/// The type of a single boolean literal, i.e. the type true or the type false.
/// Carries the literal value. A BooleanLiteralType flows into Boolean, and into
/// another BooleanLiteralType only when the value is identical.
class BooleanLiteralType : public TypeInfo {
  /// The literal value.
  bool value_;

 public:
  explicit BooleanLiteralType(bool value)
      : TypeInfo(TypeKind::BooleanLiteral), value_(value) {}

  /// \return the literal value.
  bool getValue() const {
    return value_;
  }

  int _compareImpl(const BooleanLiteralType *other, CompareState &state) const;
  bool _equalsImpl(const BooleanLiteralType *other, CompareState &state) const;
  unsigned _hashImpl() const;

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::BooleanLiteral;
  }
};

/// Variance of a field in a structural type. Controls read/write permissions
/// and how the field participates in subtyping.
enum class FieldVariance : uint8_t {
  /// Invariant: both read and write allowed; subtyping requires type equality.
  None,
  /// Covariant (Flow's `+`): read-only; subtyping is covariant in field type.
  ReadOnly,
  /// Contravariant (Flow's `-`): write-only; subtyping is contravariant in
  /// field type.
  WriteOnly,
};

/// A type representing an ordered, named list of fields in an object.
/// ExactObject types with different field orders can't flow into each other.
class ExactObjectType : public TypeInfo {
 public:
  /// Represents a field in an object.
  struct Field {
    /// The name of the field.
    Identifier name;
    /// The type of the field.
    Type *type;
    /// Variance of this field (None == invariant).
    FieldVariance variance;

    Field(
        Identifier name,
        Type *type,
        FieldVariance variance = FieldVariance::None)
        : name(name), type(type), variance(variance) {}
  };

  /// An index signature, e.g. `[k: string]: number`. An exact object has at
  /// most one indexer, and an object with an indexer has no named fields.
  struct Indexer {
    /// The type of the key (e.g. string or number).
    Type *keyType;
    /// The type of the value.
    Type *valueType;
    /// Variance of the indexer (None == invariant).
    FieldVariance variance;
  };

 private:
  /// The list of fields on the object, in order.
  llvh::SmallVector<Field, 4> fields_{};

  /// Map from field name to field index.
  /// Contains all fields in this exact object.
  llvh::SmallDenseMap<Identifier, size_t, 4> fieldNameMap_{};

  /// The optional index signature. When set, fields_ is empty.
  OptValue<Indexer> indexer_;

 public:
  /// Initialize a new instance.
  explicit ExactObjectType(
      llvh::ArrayRef<Field> fields,
      OptValue<Indexer> indexer = llvh::None)
      : TypeInfo(TypeKind::ExactObject), indexer_(indexer) {
    // Populate the fields_.
    fields_.append(fields.begin(), fields.end());

    // Populate the fieldNameMap_.
    fieldNameMap_.reserve(fields.size());
    size_t index = 0;
    for (const auto &f : fields_) {
      fieldNameMap_[f.name] = index++;
    }
  }

  /// \return the fields in order.
  llvh::ArrayRef<Field> getFields() const {
    return fields_;
  }

  /// \return the index signature, None if there isn't one.
  const OptValue<Indexer> &getIndexer() const {
    return indexer_;
  }

  /// \return true if this object has an index signature.
  bool hasIndexer() const {
    return indexer_.hasValue();
  }

  /// \return the field index for the given identifier, None if not found.
  hermes::OptValue<size_t> findField(Identifier id) const;

  /// Compare two instances of the same TypeKind.
  int _compareImpl(const ExactObjectType *other, CompareState &state) const;
  /// Compare two instances of the same TypeKind.
  bool _equalsImpl(const ExactObjectType *other, CompareState &state) const;
  /// Calculate the type-specific hash.
  unsigned _hashImpl() const;

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::ExactObject;
  }
};

class BaseFunctionType : public TypeInfo {
 private:
  bool isAsync_ = false;
  bool isGenerator_ = false;

 public:
  /// Initialize a new typed function instance.
  explicit BaseFunctionType(TypeKind kind, bool isAsync, bool isGenerator)
      : TypeInfo(kind), isAsync_(isAsync), isGenerator_(isGenerator) {}

  bool isAsync() const {
    return isAsync_;
  }
  bool isGenerator() const {
    return isGenerator_;
  }

  static bool classof(const TypeInfo *t) {
    return t->getKind() >= TypeKind::_FirstFunction &&
        t->getKind() <= TypeKind::_LastFunction;
  }
};

/// The type of a typed function: its parameters, optional "this" parameter,
/// return type and async/generator flags.
///
/// The return type is the annotation. For a plain function it is also the
/// type of a call. For an async function it is the type of the promise's
/// value, and for a generator the type of its return value, so it is not the
/// type of a call, which is a promise or a generator object. Typed mode has
/// no promise or generator types yet, so FlowContext::getCallResultType
/// types such a call 'mixed', and IRGen types it 'object' in the IR. Function
/// assignability (canAFlowIntoB) compares the annotations and requires equal
/// async and generator flags.
class TypedFunctionType : public BaseFunctionType {
 public:
  struct Param {
    /// Name of the parameter.
    Identifier name;
    /// Type of the parameter.
    Type *type;
    /// Whether the parameter was declared as optional.
    bool optional;
    /// Whether this is a rest parameter (...args).
    /// Rest parameter MUST be the last parameter in params_.
    /// There is at most one rest parameter.
    bool rest = false;
  };

 private:
  /// Return type annotation: for an async function or a generator, not the
  /// type of a call (see the class comment).
  Type *return_ = nullptr;
  /// Optional "this" parameter type.
  Type *thisParam_ = nullptr;
  /// Parameter types.
  llvh::SmallVector<Param, 2> params_{};

 public:
  /// Initialize a new typed function instance.
  explicit TypedFunctionType(
      Type *returnType,
      Type *thisParam,
      llvh::ArrayRef<Param> params,
      bool isAsync,
      bool isGenerator)
      : BaseFunctionType(TypeKind::TypedFunction, isAsync, isGenerator),
        return_(returnType),
        thisParam_(thisParam) {
    params_.append(params.begin(), params.end());
  }

  Type *getReturnType() const {
    return return_;
  }
  /// \return true if a call to this function does not return a value of the
  /// return type: an async function returns a promise and a generator
  /// returns a generator object.
  bool callWrapsReturnType() const {
    return isAsync() || isGenerator();
  }
  Type *getThisParam() const {
    return thisParam_;
  }
  const llvh::ArrayRef<Param> getParams() const {
    return params_;
  }

  /// Compare two instances of the same TypeKind.
  int _compareImpl(const TypedFunctionType *other, CompareState &state) const;
  /// Compare two instances of the same TypeKind.
  bool _equalsImpl(const TypedFunctionType *other, CompareState &state) const;
  /// Calculate the type-specific hash.
  unsigned _hashImpl() const;

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::TypedFunction;
  }
};

class NativeFunctionType : public BaseFunctionType {
 public:
  using Param = TypedFunctionType::Param;

 private:
  /// Result type.
  Type *return_ = nullptr;
  /// Parameter types.
  llvh::SmallVector<Param, 2> params_{};
  /// The signature of the native function.
  NativeSignature *signature_;

 public:
  /// Initialize a new native function instance.
  explicit NativeFunctionType(
      Type *returnType,
      llvh::ArrayRef<TypedFunctionType::Param> params,
      NativeSignature *signature)
      : BaseFunctionType(TypeKind::NativeFunction, false, false),
        return_(returnType),
        signature_(signature) {
    assert(signature && "signature must be non-null");
    params_.append(params.begin(), params.end());
  }

  Type *getReturnType() const {
    return return_;
  }
  const llvh::ArrayRef<Param> getParams() const {
    return params_;
  }
  NativeSignature *getSignature() const {
    return signature_;
  }

  /// Compare two instances of the same TypeKind.
  int _compareImpl(const NativeFunctionType *other, CompareState &state) const;
  /// Compare two instances of the same TypeKind.
  bool _equalsImpl(const NativeFunctionType *other, CompareState &state) const;
  /// Calculate the type-specific hash.
  unsigned _hashImpl() const;

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::NativeFunction;
  }
};

class UntypedFunctionType : public BaseFunctionType {
 public:
  explicit UntypedFunctionType(bool isAsync, bool isGenerator)
      : BaseFunctionType(TypeKind::UntypedFunction, isAsync, isGenerator) {}

  /// Compare two instances of the same TypeKind.
  int _compareImpl(const UntypedFunctionType *other, CompareState &state) const;
  /// Compare two instances of the same TypeKind.
  bool _equalsImpl(const UntypedFunctionType *other, CompareState &state) const;
  /// Calculate the type-specific hash.
  unsigned _hashImpl() const;

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::UntypedFunction;
  }
};

/// A complex type annotated with a unique id (for that specific kind) to allow
/// predictable sorting without examining recursive attributes.
class TypeWithId : public TypeInfo {
  /// A unique global id used for predictable sorting.
  size_t const id_;
  /// Has this type been initialized.
  bool initialized_ = false;

 public:
  TypeWithId(TypeKind kind, const size_t id) : TypeInfo(kind), id_(id) {}

  static bool classof(const TypeInfo *t) {
    return t->getKind() >= TypeKind::_FirstId &&
        t->getKind() <= TypeKind::_LastId;
  }

  /// \return the per-type unique id.
  size_t getId() const {
    return id_;
  }

  /// \return false if this type has only been forward decclared but not
  ///     initialized yet.
  bool isInitialized() const {
    return initialized_;
  }

  /// Compare two instances of the same TypeKind.
  int _compareImpl(const TypeWithId *other, CompareState &state) const {
    return id_ < other->id_ ? -1 : id_ == other->id_ ? 0 : 1;
  }
  /// Compare two instances of the same TypeKind.
  int _equalsImpl(const TypeWithId *other, CompareState &state) const {
    return _compareImpl(other, state) == 0;
  }
  /// Calculate the type-specific hash.
  unsigned _hashImpl() const;

 protected:
  /// Mark the type as initialized.
  void markAsInitialized() {
    initialized_ = true;
  }
};

class ClassType : public TypeWithId {
 public:
  /// Represents either a class field or a method.
  /// Non-const members are mutated after construction as the class and its
  /// subclasses are processed: the getter/setter merge, the overload merge and
  /// the subclass override marking all update an existing Field in place.
  struct Field {
    const Identifier name;
    /// The type of the field. For accessor fields, this is the getter's
    /// return type (or Void for setter-only).
    /// For overloaded methods this is null; per-overload types are stored in
    /// \c overloads. See \c isOverloaded().
    Type *type;
    /// The slot for PrLoad and PrStore, used during IRGen.
    /// This ideally should be computed during conversion to IR Type,
    /// but we don't have that yet.
    /// None for fields that don't have a layout slot (e.g. final methods,
    /// private methods).
    const OptValue<size_t> layoutSlotIR;
    /// If the field is a method, AST for the method (getter for accessors).
    /// For overloaded methods this is null; per-overload method nodes are
    /// stored in \c overloads. See \c isOverloaded().
    ESTree::MethodDefinitionNode *method;
    /// The key AST node for static fields/methods that use variables instead
    /// of layout slots. Used to propagate the Decl to member access sites.
    /// For methods this is redundant with method->_key but is set for static
    /// properties (which have no MethodDefinitionNode) and works uniformly
    /// for both public and private statics.
    ESTree::IdentifierNode *const staticKeyNode;
    /// Whether the field is a final method (non-virtual, cannot be overridden).
    /// Required for generic methods on either generic or non-generic classes.
    /// Final methods will not be stored in the home object, they will only be
    /// stored in variables.
    const bool finalMethod;

    /// Function type of the getter accessor, or nullptr.
    Type *getterType = nullptr;
    /// Function type of the setter accessor, or nullptr.
    Type *setterType = nullptr;
    /// AST for the setter method. The getter uses the existing \c method field.
    ESTree::MethodDefinitionNode *setterMethod = nullptr;

    /// Whether this field is a method that has been overridden by a subclass.
    /// Set as subclasses are processed, after the field is constructed.
    bool overridden{false};

    /// Whether this is a private field for this class.
    bool isPrivate;

    /// Variance of this field (None == invariant), mirroring
    /// ExactObjectType::Field. Methods and getter-only accessors are ReadOnly,
    /// setter-only accessors are WriteOnly. Reset to None when a getter and a
    /// setter are merged into a single field.
    FieldVariance variance = FieldVariance::None;

    /// All overloads for overloaded methods, mapping method AST node to its
    /// type. For non-overloaded methods, this is empty and the existing
    /// \c type and \c method fields are used. For overloaded methods, ALL
    /// overload entries are stored here and \c type and \c method are null.
    llvh::MapVector<ESTree::MethodDefinitionNode *, Type *> overloads;

    /// Whether this field is an overloaded method.
    bool isOverloaded() const {
      return !overloads.empty();
    }

    Field(
        Identifier name,
        Type *type,
        OptValue<size_t> layoutSlotIR,
        bool isPrivate,
        ESTree::MethodDefinitionNode *method = nullptr,
        bool finalMethod = false,
        ESTree::IdentifierNode *staticKeyNode = nullptr,
        Type *getterType = nullptr,
        Type *setterType = nullptr,
        ESTree::MethodDefinitionNode *setterMethod = nullptr)
        : name(name),
          type(type),
          layoutSlotIR(layoutSlotIR),
          method(method),
          staticKeyNode(staticKeyNode),
          finalMethod(finalMethod),
          getterType(getterType),
          setterType(setterType),
          setterMethod(setterMethod),
          isPrivate(isPrivate) {
      if (isMethod() && !isAccessor())
        variance = FieldVariance::ReadOnly;
      else if (isAccessor() && !hasSetter())
        variance = FieldVariance::ReadOnly;
      else if (isAccessor() && !hasGetter())
        variance = FieldVariance::WriteOnly;
    }

    bool isMethod() const {
      return method != nullptr || setterMethod != nullptr || isOverloaded();
    }

    /// Whether the field's value lives in a slot on the object itself, i.e. it
    /// can be read by loading the field from the object. False for final and
    /// private methods and for accessors, which live in Variables.
    bool isStoredOnObject() const {
      // TODO: This is a temporary implementation: once layoutSlotIR goes
      // away this must be implemented without it.
      return layoutSlotIR.hasValue();
    }

    /// Whether this field is an accessor (getter and/or setter).
    bool isAccessor() const {
      return getterType || setterType;
    }
    /// Whether this accessor has a getter.
    bool hasGetter() const {
      return getterType != nullptr;
    }
    /// Whether this accessor has a setter.
    bool hasSetter() const {
      return setterType != nullptr;
    }
  };

  /// Lookup entry in the table, used to quickly find fields.
  struct FieldLookupEntry {
    /// The class on which this field is defined.
    /// Fields may occur anywhere in the inheritance chain.
    ClassType *classType;

    /// Slot index in the fields_ list in classType.
    size_t fieldsIndex;

    explicit FieldLookupEntry() = default;
    explicit FieldLookupEntry(ClassType *classType, size_t fieldsIndex)
        : classType(classType), fieldsIndex(fieldsIndex) {}

    /// Helper functions to retrieve the field given the lookup entry.
    const Field *getField() const {
      return &classType->fields_[fieldsIndex];
    }
    Field *getFieldMut() const {
      return &classType->fields_[fieldsIndex];
    }
  };

 private:
  /// Class name.
  Identifier const className_;
  /// The non-inherited fields of the class, which are pointed to from
  /// the fieldNameMap_ for lookup from either this class or subclasses.
  llvh::SmallVector<Field, 4> fields_{};
  /// The constructor function (FunctionType).
  Type *constructorType_ = nullptr;
  /// The .prototype property ([[HomeObject]] in the spec) contains methods.
  /// This class encodes those methods.
  /// Null for the ClassTypes that represent home objects and static objects.
  /// Home objects are reachable as expression types (via `C.prototype`), so
  /// you must check this before assuming it exists.
  Type *homeObjectType_ = nullptr;
  /// The type encoding static properties and methods of the class.
  /// Null if the class has no static elements.
  Type *staticObjectType_ = nullptr;
  /// Map from field name to field lookup entry.
  /// Contains all fields in this class and all superClasses.
  /// This allows us to quickly check how many fields to allocate for the class,
  /// as well as quick lookup to see if a field exists.
  /// This also means we can query which class the field exists on easily.
  /// Use a MapVector to make sure it's deterministic to iterate.
  llvh::SmallMapVector<Identifier, FieldLookupEntry, 4> fieldNameMap_{};
  /// Map from private field name to index in fields_.
  /// Contains all private fields in this class ONLY (never inherited).
  /// To know how many fields to allocate for private properties, you must
  /// also account for all ancestor classes in IRGen (requires iterating through
  /// superclasses and counting all private fields).
  /// Use a MapVector to make sure it's deterministic to iterate.
  llvh::SmallMapVector<Identifier, size_t, 4> privateFieldNameMap_{};
  /// Map from specialized MethodDefinitionNode to its Decl.
  /// Used by IRGen to emit specialized methods and store Function* in the Decl.
  llvh::SmallMapVector<ESTree::MethodDefinitionNode *, sema::Decl *, 4>
      specializedMethodDecls_{};

  /// Type arguments for this specialization of a generic class.
  /// Empty if the class is not a generic specialization.
  /// Used mostly for error reporting.
  llvh::SmallVector<Type *, 2> typeArgs_{};

  /// The generic class Decl this class was specialized from, or nullptr if this
  /// class is not a generic specialization. Identifies builtin generics (e.g.
  /// Array<T>) without a name-based or side-table lookup.
  sema::Decl *genericClassDecl_ = nullptr;

  /// Super class, nullptr if this class doesn't extend anything.
  Type *superClass_ = nullptr;

  /// The ClassConstructorType wrapping this class, i.e. the type of the class
  /// object itself; distinct from \c constructorType_, which is the function
  /// type of the constructor's signature. Null for the synthetic home object
  /// and static object classes, which have no constructor of their own.
  Type *classConsType_ = nullptr;

  /// Number of fields that have IR layout slots.
  /// Final methods don't get layout slots, so this may be less than
  /// fieldNameMap_.size().
  size_t numLayoutSlots_ = 0;

 public:
  explicit ClassType(
      size_t id,
      Identifier className,
      llvh::ArrayRef<Type *> typeArgs,
      sema::Decl *genericClassDecl = nullptr);

  /// Initialize an empty (freshly created) instance. Note that fields are
  /// immutable after this.
  /// \p numLayoutSlots is the number of fields that have layout slots in IR,
  /// which may be less than the number of fields in the field name map
  /// (e.g. final methods don't get layout slots).
  /// \p classConsType is the ClassConstructorType wrapping this class, null
  /// for the synthetic home object and static object classes.
  void init(
      llvh::ArrayRef<Field> fields,
      Type *constructorType,
      Type *homeObjectType,
      Type *staticObjectType,
      Type *superClass,
      size_t numLayoutSlots,
      Type *classConsType);

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::Class;
  }

  Identifier getClassName() const {
    return className_;
  }
  llvh::StringRef getClassNameOrDefault() const {
    return className_.isValid() ? className_.str()
                                : llvh::StringLiteral("<anonymous>");
  }
  const llvh::ArrayRef<Field> getFields() const {
    assert(isInitialized());
    return fields_;
  }
  Type *getConstructorType() const {
    assert(isInitialized());
    return constructorType_;
  }
  TypedFunctionType *getConstructorTypeInfo() const {
    assert(isInitialized());
    return llvh::cast_or_null<TypedFunctionType>(constructorType_->info);
  }
  Type *getHomeObjectType() const {
    assert(isInitialized());
    return homeObjectType_;
  }
  ClassType *getHomeObjectTypeInfo() const {
    assert(isInitialized());
    return llvh::cast_or_null<ClassType>(
        homeObjectType_ ? homeObjectType_->info : nullptr);
  }
  /// A ClassType which has no home object of its own is itself a home object.
  bool isHomeObject() const {
    assert(isInitialized());
    return !homeObjectType_;
  }
  Type *getStaticObjectType() const {
    assert(isInitialized());
    return staticObjectType_;
  }
  ClassType *getStaticObjectTypeInfo() const {
    assert(isInitialized());
    return llvh::cast_or_null<ClassType>(
        staticObjectType_ ? staticObjectType_->info : nullptr);
  }
  /// \return the public field name lookup table.
  const llvh::SmallMapVector<Identifier, FieldLookupEntry, 4> &getFieldNameMap()
      const {
    assert(isInitialized());
    return fieldNameMap_;
  }
  /// \return the private field name lookup table.
  const llvh::SmallMapVector<Identifier, size_t, 4> &getPrivateFieldNameMap()
      const {
    assert(isInitialized());
    return privateFieldNameMap_;
  }
  /// Lookup a field on this class (includes ancestors).
  /// \return the lookup entry of a field, None if it doesn't exist.
  hermes::OptValue<FieldLookupEntry> findPublicField(Identifier id) const;

  /// Lookup an own private field on this class.
  /// \return the lookup entry of a field, None if it doesn't exist.
  hermes::OptValue<FieldLookupEntry> findPrivateField(Identifier id);

  /// Return the number of fields that have IR layout slots.
  size_t getNumLayoutSlots() const {
    assert(isInitialized());
    return numLayoutSlots_;
  }

  /// \return the type arguments, empty if not a generic specialization.
  llvh::ArrayRef<Type *> getTypeArgs() const {
    return typeArgs_;
  }

  /// \return the generic class Decl this was specialized from, or nullptr.
  sema::Decl *getGenericClassDecl() const {
    return genericClassDecl_;
  }

  Type *getSuperClass() const {
    assert(isInitialized());
    return superClass_;
  }
  ClassType *getSuperClassInfo() const {
    assert(isInitialized());
    return llvh::cast_or_null<ClassType>(
        superClass_ ? superClass_->info : nullptr);
  }

  /// \return \c classConsType_, the type of the class object itself. Not to be
  ///   confused with \c getConstructorType(), which returns the function type
  ///   of the constructor's signature.
  Type *getClassConsType() const {
    assert(isInitialized());
    return classConsType_;
  }

  /// Add a specialized method with its Decl.
  /// \param specializedMethod the specialized MethodDefinitionNode.
  /// \param decl the Decl created for this specialization.
  void addSpecializedMethodDecl(
      ESTree::MethodDefinitionNode *specializedMethod,
      sema::Decl *decl) {
    [[maybe_unused]] auto [it, inserted] =
        specializedMethodDecls_.insert({specializedMethod, decl});
    assert(inserted && "specialized method already exists");
  }

  /// Find the Decl for a specialized method.
  /// \return the Decl, or nullptr if not found.
  sema::Decl *findSpecializedMethodDecl(
      ESTree::MethodDefinitionNode *specializedMethod) const {
    auto it = specializedMethodDecls_.find(specializedMethod);
    if (it == specializedMethodDecls_.end())
      return nullptr;
    return it->second;
  }

  /// \return the map of specialized method decls for iteration.
  const llvh::SmallMapVector<ESTree::MethodDefinitionNode *, sema::Decl *, 4> &
  getSpecializedMethodDecls() const {
    return specializedMethodDecls_;
  }
};

/// The type of the constructor of the class. This is what a class expression
/// returns. Similarly, this is the type of the class variable in class
/// statements.
class ClassConstructorType : public TypeWithId {
  /// The class this constructor is for.
  Type *const classType_;

 public:
  explicit ClassConstructorType(size_t id, Type *classType)
      : TypeWithId(TypeKind::ClassConstructor, id), classType_(classType) {
    markAsInitialized();
  }

  static bool classof(const TypeInfo *t) {
    return t->getKind() == TypeKind::ClassConstructor;
  }

  Type *getClassType() const {
    return classType_;
  }
  ClassType *getClassTypeInfo() const {
    return llvh::cast<ClassType>(classType_->info);
  }
};

/// Discriminator for the receiver of a builtin method (TypedLib functions
/// declared with a "builtin" directive). populateBuiltinMethods registers
/// methods whose 'this' is annotated T[], string or number under Array, String
/// or Number. Only String and Number receivers are looked up when member
/// accesses are checked (visitMemberString, visitMemberNumber); a method
/// registered under Array is never found.
enum class BuiltinReceiverKind : uint8_t { Array, String, Number };

/// Key for looking up builtin methods.
///
/// Methods with "builtin" directive are stored here, so they can be looked up
/// when typechecking builtin method calls.
struct BuiltinMethodKey {
  /// The kind of the receiver ('this' parameter) of the builtin method.
  /// Note that we don't store a real Type here because it's easier to match
  /// against builtin types with just the receiver kind, and we're only using
  /// this key to lookup a small number of registered "builtin" methods.
  BuiltinReceiverKind receiverKind;
  /// The method name.
  UniqueString *name;
  /// Whether this is a static method (vs prototype/instance method).
  bool isStatic;

  /// \return a hash value for the key.
  unsigned getHashValue() const {
    return llvh::hash_combine(
        (uint8_t)receiverKind,
        llvh::hash_value(name),
        llvh::hash_value(isStatic));
  }

  bool operator==(const BuiltinMethodKey &other) const {
    return receiverKind == other.receiverKind && name == other.name &&
        isStatic == other.isStatic;
  }
};

/// DenseMapInfo specialization for BuiltinMethodKey.
struct BuiltinMethodKeyInfo {
  static inline BuiltinMethodKey getEmptyKey() {
    return {
        BuiltinReceiverKind::Array,
        llvh::DenseMapInfo<UniqueString *>::getEmptyKey(),
        false};
  }
  static inline BuiltinMethodKey getTombstoneKey() {
    return {
        BuiltinReceiverKind::Array,
        llvh::DenseMapInfo<UniqueString *>::getTombstoneKey(),
        false};
  }
  static unsigned getHashValue(const BuiltinMethodKey &key) {
    return key.getHashValue();
  }
  static bool isEqual(
      const BuiltinMethodKey &lhs,
      const BuiltinMethodKey &rhs) {
    return lhs == rhs;
  }
};

class FlowContext {
  friend class FlowTypesDumper;

 public:
  /// A dummy type to prevent accidentally invoking mutating accessors.
  struct ForUpdate {};

  FlowContext(const FlowContext &) = delete;
  void operator=(const FlowContext &) = delete;

  FlowContext();
  ~FlowContext();

  const auto &declTypeMap() const {
    return declTypes_;
  }
  const auto &nodeTypeMap() const {
    return nodeTypes_;
  }
  auto &declTypeMap(ForUpdate) {
    return declTypes_;
  }
  auto &nodeTypeMap(ForUpdate) {
    return nodeTypes_;
  }

  /// Return the type associated with the specified decl, or nullptr.
  Type *findDeclType(const sema::Decl *decl) const;

  /// Return the type, which must exist, associated with the specified
  /// declaration.
  Type *getDeclType(const sema::Decl *decl) const;

  /// Associate a type with an expression node. Do nothing if the specified
  /// type is nullptr.
  void setNodeType(const ESTree::Node *node, Type *type);

  /// Return the type associated with the specified AST node, or nullptr.
  Type *findNodeType(const ESTree::Node *node) const;

  /// Return the optional type associated with a node or "any" if no type.
  Type *getNodeTypeOrAny(const ESTree::Node *node) const;

  // Singleton accessors
#define _HERMES_SEMA_FLOW_DEFKIND(name) \
  TypeInfo *get##name##Info() const {   \
    return &name##Info_;                \
  }                                     \
  Type *get##name() const {             \
    return &name##Instance_;            \
  }
  _HERMES_SEMA_FLOW_SINGLETONS
#undef _HERMES_SEMA_FLOW_DEFKIND

  /// Get a singleton type by index.
  Type *getSingletonType(TypeKind kind) const;

  /// \return the type of a call to a function of type \p ft: its return type,
  /// or 'mixed' for an async function or a generator, whose call returns a
  /// promise or a generator object. Flow has no general object type (exact
  /// objects are fixed shapes and classes are nominal, so either would be
  /// false). 'any' would flow implicitly into any concrete type through a
  /// checked cast, and a cast to a refined object kind checks only that the
  /// value is an object, so a promise could be used as a class instance.
  /// 'mixed' flows only into 'any' and 'mixed' (or through an explicit
  /// cast), and still allows typeof and await, until promise and generator
  /// types exist.
  Type *getCallResultType(const TypedFunctionType *ft) const {
    return ft->callWrapsReturnType() ? getMixed() : ft->getReturnType();
  }

  /// Options on which arms to exclude from a created union.
  struct UnionExcludes {
    /// Whether to remove 'void' from the result.
    bool excludeVoid = false;
    /// Whether to remove 'null' from the result.
    bool excludeNull = false;

    UnionExcludes() {}
  };

  /// Allocate a union from the given \p types.
  /// The types may contain duplicates or other unions.
  UnionType *createNonCanonicalizedUnion(llvh::SmallVector<Type *, 4> &&types) {
    return &allocUnion_.emplace_back(std::move(types));
  }
  /// Canonicalize the given \p types, and create a union from them if more than
  /// one type remains. Otherwise, just return the single type.
  /// \param excludes a set of TypeKinds to erase from the final type.
  /// \pre the unified list of types must include at least one element not in \p
  ///   excludes.
  TypeInfo *maybeCreateUnion(
      llvh::ArrayRef<Type *> types,
      UnionExcludes excludes = {});
  /// Create an initialized "maybe" type (void | null | type).
  UnionType *createPopulatedNullable(Type *type);

  InferencePlaceholderArrayType *createInferencePlaceholderArray(
      Type *element) {
    assert(element);
    return &allocInferencePlaceholderArray_.emplace_back(element);
  }
  TupleType *createTuple(llvh::ArrayRef<Type *> types) {
    return &allocTuple_.emplace_back(types);
  }
  StringLiteralType *createStringLiteral(UniqueString *value) {
    assert(value);
    return &allocStringLiteral_.emplace_back(value);
  }
  NumberLiteralType *createNumberLiteral(double value) {
    // -0 and 0 are the same literal type, so store +0 for both: the value is
    // emitted as the default of literal-typed fields.
    return &allocNumberLiteral_.emplace_back(value == 0 ? 0.0 : value);
  }
  BooleanLiteralType *createBooleanLiteral(bool value) {
    return &allocBooleanLiteral_.emplace_back(value);
  }
  ExactObjectType *createExactObject(
      llvh::ArrayRef<ExactObjectType::Field> fields,
      OptValue<ExactObjectType::Indexer> indexer = llvh::None) {
    return &allocExactObject_.emplace_back(fields, indexer);
  }
  TypedFunctionType *createFunction(
      Type *returnType,
      Type *thisParam,
      llvh::ArrayRef<TypedFunctionType::Param> params,
      bool isAsync,
      bool isGenerator) {
    return &allocTypedFunction_.emplace_back(
        returnType, thisParam, params, isAsync, isGenerator);
  }
  NativeFunctionType *createNativeFunction(
      Type *returnType,
      llvh::ArrayRef<TypedFunctionType::Param> params,
      NativeSignature *signature) {
    return &allocNativeFunction_.emplace_back(returnType, params, signature);
  }
  UntypedFunctionType *createUntypedFunction(bool isAsync, bool isGenerator) {
    return &allocUntypedFunction_.emplace_back(isAsync, isGenerator);
  }
  ClassType *createClass(
      Identifier name,
      llvh::ArrayRef<Type *> typeArgs = {},
      sema::Decl *genericClassDecl = nullptr) {
    return &allocClass_.emplace_back(
        allocClass_.size(), name, typeArgs, genericClassDecl);
  }
  ClassConstructorType *createClassConstructor(Type *classType) {
    return &allocClassConstructor_.emplace_back(
        allocClassConstructor_.size(), classType);
  }
  Type *createType(ESTree::Node *node = nullptr) {
    return &allocTypes_.emplace_back(nullptr, node);
  }
  Type *createType(TypeInfo *type, ESTree::Node *node = nullptr) {
    return &allocTypes_.emplace_back(type, node);
  }

  /// Register a builtin method in the table.
  void registerBuiltinMethod(
      const BuiltinMethodKey &key,
      ESTree::FunctionDeclarationNode *funcDecl) {
    builtinMethods_[key] = funcDecl;
  }

  /// Look up a builtin method. Returns nullptr if not found.
  ESTree::FunctionDeclarationNode *findBuiltinMethod(
      const BuiltinMethodKey &key) const {
    auto it = builtinMethods_.find(key);
    return it != builtinMethods_.end() ? it->second : nullptr;
  }

  /// Register a builtin call target for IRGen.
  void setBuiltinCallTarget(
      ESTree::CallExpressionNode *call,
      ESTree::FunctionDeclarationNode *funcDecl) {
    builtinCallTargets_[call] = funcDecl;
  }

  /// Get the builtin function for a call, or nullptr if not a builtin call.
  ESTree::FunctionDeclarationNode *getBuiltinCallTarget(
      ESTree::CallExpressionNode *call) const {
    auto it = builtinCallTargets_.find(call);
    return it != builtinCallTargets_.end() ? it->second : nullptr;
  }

  /// Record the generic Decl of the builtin Array<T> class (@Hermes.array).
  void setArrayGenericClassDecl(sema::Decl *decl) {
    arrayGenericClassDecl_ = decl;
  }

  /// \return the generic Decl of the builtin Array<T> class (@Hermes.array),
  /// or nullptr if it has not been declared.
  sema::Decl *getArrayGenericClassDecl() const {
    return arrayGenericClassDecl_;
  }

  /// \return true if \p info is a specialization of the builtin Array<T>
  /// class.
  bool isArrayClassType(const TypeInfo *info) const {
    auto *ct = llvh::dyn_cast_or_null<ClassType>(info);
    return ct && arrayGenericClassDecl_ &&
        ct->getGenericClassDecl() == arrayGenericClassDecl_;
  }

  /// \return true if \p t is a specialization of the builtin Array<T> class.
  bool isArrayClassType(const Type *t) const {
    return t && isArrayClassType(t->info);
  }

  /// \return true if \p classType is a specialization of the builtin Array<T>
  /// class or derives from one. Instances of such classes are fast arrays.
  bool derivesFromArrayClass(const ClassType *classType) const {
    for (const ClassType *cur = classType; cur; cur = cur->getSuperClassInfo())
      if (isArrayClassType(cur))
        return true;
    return false;
  }

  /// \return the element type of an Array<T> specialization.
  /// \pre isArrayClassType(info).
  Type *getArrayElementType(const TypeInfo *info) const {
    assert(isArrayClassType(info) && "not an Array ClassType");
    return llvh::cast<ClassType>(info)->getTypeArgs()[0];
  }

  /// \return the element type of an Array<T> specialization.
  /// \pre isArrayClassType(t).
  Type *getArrayElementType(const Type *t) const {
    return getArrayElementType(t->info);
  }

 private:
  /// Allocate a union from the given \p types. The types may not contain
  /// duplicates or other unions.
  UnionType *createCanonicalizedUnion(
      llvh::SmallVector<Type *, 4> &&nonLoopingTypes,
      llvh::SmallVector<Type *, 4> &&loopingTypes) {
    return &allocUnion_.emplace_back(
        std::move(nonLoopingTypes), std::move(loopingTypes));
  }

  /// Types associated with declarations.
  llvh::DenseMap<const sema::Decl *, Type *> declTypes_{};

  /// Types associated with expression nodes.
  llvh::DenseMap<const ESTree::Node *, Type *> nodeTypes_{};

  /// Map from a callee node (typically MemberExpressionNode) to the
  /// specialized method's Decl for generic method calls.
  /// This allows IRGen to look up the specialized function for a call.
  llvh::DenseMap<const ESTree::Node *, sema::Decl *> specializedMethodCalls_{};

  /// Table mapping builtin method keys to their function declarations.
  /// Populated by FlowChecker for functions with "builtin" directive.
  llvh::DenseMap<
      BuiltinMethodKey,
      ESTree::FunctionDeclarationNode *,
      BuiltinMethodKeyInfo>
      builtinMethods_{};

  /// Maps CallExpressionNode to the builtin FunctionDeclarationNode.
  /// Needed for IRGen to generate direct calls to the builtins.
  llvh::
      DenseMap<ESTree::CallExpressionNode *, ESTree::FunctionDeclarationNode *>
          builtinCallTargets_{};

  /// The generic Decl of the builtin Array<T> class (@Hermes.array), or nullptr
  /// before it is discovered. Used to identify Array specializations.
  sema::Decl *arrayGenericClassDecl_ = nullptr;

  std::deque<Type> allocTypes_{};

  // Declare allocators for complex types.
#define _HERMES_SEMA_FLOW_DEFKIND(name) std::deque<name##Type> alloc##name##_{};
  _HERMES_SEMA_FLOW_COMPLEX_TYPES
#undef _HERMES_SEMA_FLOW_DEFKIND

  // Define the singletons
#define _HERMES_SEMA_FLOW_DEFKIND(name) \
  mutable name##Type name##Info_{};     \
  mutable Type name##Instance_{&name##Info_};
  _HERMES_SEMA_FLOW_SINGLETONS
#undef _HERMES_SEMA_FLOW_DEFKIND
};

} // namespace flow
} // namespace hermes

#endif // HERMES_SEMA_FLOWCONTEXT_H
