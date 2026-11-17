/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/IR/TypeContext.h"
#include "hermes/Support/HermesSafeMath.h"

#include "hermes/IR/IR.h"

#include "llvh/Support/MathExtras.h"
#include "llvh/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>

namespace hermes {

using namespace type_internal;

namespace {

/// Number-family join (union / least-upper-bound), indexed [a][b].
constexpr uint8_t kNumberJoin[6][6] = {
    /*None  */ {0, 1, 2, 3, 4, 5},
    /*UInt31*/ {1, 1, 2, 3, 4, 5},
    /*Int32 */ {2, 2, 2, 4, 4, 5},
    /*Uint32*/ {3, 3, 4, 3, 4, 5},
    /*I∪U   */ {4, 4, 4, 4, 4, 5},
    /*Number*/ {5, 5, 5, 5, 5, 5},
};

/// Number-family meet (intersection / greatest-lower-bound), indexed [a][b].
constexpr uint8_t kNumberMeet[6][6] = {
    /*None  */ {0, 0, 0, 0, 0, 0},
    /*UInt31*/ {0, 1, 1, 1, 1, 1},
    /*Int32 */ {0, 1, 2, 1, 2, 2},
    /*Uint32*/ {0, 1, 1, 3, 3, 3},
    /*I∪U   */ {0, 1, 2, 3, 4, 4},
    /*Number*/ {0, 1, 2, 3, 4, 5},
};

/// Number-family subtract (a − b, tightest representable superset),
/// indexed [a][b].
constexpr uint8_t kNumberSub[6][6] = {
    /*None  */ {0, 0, 0, 0, 0, 0},
    /*UInt31*/ {1, 0, 0, 0, 0, 0},
    /*Int32 */ {2, 2, 0, 2, 0, 0},
    /*Uint32*/ {3, 3, 3, 0, 0, 0},
    /*I∪U   */ {4, 4, 3, 2, 0, 0},
    /*Number*/ {5, 5, 5, 5, 5, 0},
};

/// \return the primMask for a leaf \p k, or kNotMaskable if \p k is a
/// refined/compiler-internal kind that never participates in inference algebra
/// (Environment, PrivateName, FunctionCode, Bits32, refined object kinds,
/// Union).
constexpr uint16_t leafKindToMask(TypeKind k) {
  switch (k) {
    case TypeKind::NoType:
      return 0;
    case TypeKind::Undefined:
      return MB_Undefined;
    case TypeKind::Null:
      return MB_Null;
    case TypeKind::Boolean:
      return MB_Boolean;
    case TypeKind::BigInt:
      return MB_BigInt;
    case TypeKind::String:
      return MB_String;
    case TypeKind::Symbol:
      return MB_Symbol;
    case TypeKind::Object:
      return MB_Object;
    case TypeKind::Empty:
      return MB_Empty;
    case TypeKind::Uninit:
      return MB_Uninit;
    case TypeKind::Number:
      return NC_Number;
    case TypeKind::Int32:
      return NC_Int32;
    case TypeKind::Uint32:
      return NC_Uint32;
    case TypeKind::UInt31:
      return NC_UInt31;
    default:
      // Environment, PrivateName, FunctionCode, Bits32, refined kinds, Union.
      return kNotMaskable;
  }
}

/// \return true if \p k is a number kind (including subtypes).
bool isNumberKind(TypeKind k) {
  return k == TypeKind::Number || k == TypeKind::Int32 ||
      k == TypeKind::Uint32 || k == TypeKind::UInt31;
}

/// \return true if \p k is a refined object kind that carries payload data.
bool isRefinedKind(TypeKind k) {
  return k >= TypeKind::_RefinedFirst && k <= TypeKind::_RefinedLast;
}

/// \return true if \p k is an object kind (including refinements).
bool isObjectKind(TypeKind k) {
  switch (k) {
    case TypeKind::Object:
    case TypeKind::ClassInstance:
    case TypeKind::Array:
    case TypeKind::Tuple:
    case TypeKind::Function:
    case TypeKind::ExactObject:
      return true;
    default:
      return false;
  }
}

/// \return true if \p k is a primitive kind (including number subtypes).
bool isPrimitiveKind(TypeKind k) {
  switch (k) {
    case TypeKind::Number:
    case TypeKind::Int32:
    case TypeKind::Uint32:
    case TypeKind::UInt31:
    case TypeKind::String:
    case TypeKind::BigInt:
    case TypeKind::Null:
    case TypeKind::Undefined:
    case TypeKind::Boolean:
    case TypeKind::Symbol:
      return true;
    default:
      return false;
  }
}

/// \return true if \p k is a non-pointer kind (including number subtypes).
bool isNonPtrKind(TypeKind k) {
  switch (k) {
    case TypeKind::Number:
    case TypeKind::Int32:
    case TypeKind::Uint32:
    case TypeKind::UInt31:
    case TypeKind::Boolean:
    case TypeKind::Null:
    case TypeKind::Undefined:
      return true;
    default:
      return false;
  }
}

/// \return true if non-refined leaf kind \p a is a subtype of \p b.
/// Refined kinds are handled by TypeContext::isLeafSubtypeById.
bool isLeafSubtype(TypeKind a, TypeKind b) {
  assert(
      !isRefinedKind(a) && !isRefinedKind(b) &&
      "refined kinds are handled by TypeContext::isLeafSubtypeById");
  if (a == b)
    return true;
  // Int32, Uint32, UInt31 are subtypes of Number.
  if (b == TypeKind::Number &&
      (a == TypeKind::Int32 || a == TypeKind::Uint32 || a == TypeKind::UInt31))
    return true;
  // UInt31 is a subtype of both Int32 and Uint32.
  if (a == TypeKind::UInt31 && (b == TypeKind::Int32 || b == TypeKind::Uint32))
    return true;
  return false;
}

/// \return true if non-refined leaf kinds \p a and \p b have no values in
/// common. Refined kinds are handled by TypeContext::areLeafTypesDisjointById.
bool areLeafKindsDisjoint(TypeKind a, TypeKind b) {
  // If one is a subtype of the other, not disjoint.
  if (isLeafSubtype(a, b) || isLeafSubtype(b, a))
    return false;
  // Number family (Number, Int32, Uint32, UInt31) overlap pairwise.
  if (isNumberKind(a) && isNumberKind(b))
    return false;
  // All other pairs of different kinds are disjoint.
  return true;
}

/// \return the display name of a leaf kind. Matches the strings used by
/// the old Type::print().
llvh::StringRef kindName(TypeKind k) {
  switch (k) {
    case TypeKind::NoType:
      return "notype";
    case TypeKind::Empty:
      return "empty";
    case TypeKind::Uninit:
      return "uninit";
    case TypeKind::Undefined:
      return "undefined";
    case TypeKind::Null:
      return "null";
    case TypeKind::Boolean:
      return "boolean";
    case TypeKind::Number:
      return "number";
    case TypeKind::BigInt:
      return "bigint";
    case TypeKind::String:
      return "string";
    case TypeKind::Symbol:
      return "symbol";
    case TypeKind::Environment:
      return "environment";
    case TypeKind::PrivateName:
      return "privateName";
    case TypeKind::FunctionCode:
      return "functionCode";
    case TypeKind::Object:
      return "object";
    case TypeKind::Bits32:
      return "bits32";
    case TypeKind::Int32:
      return "int32";
    case TypeKind::Uint32:
      return "uint32";
    case TypeKind::UInt31:
      return "uint31";
    case TypeKind::ClassInstance:
      return "classInstance";
    case TypeKind::Array:
      return "array";
    case TypeKind::Tuple:
      return "tuple";
    case TypeKind::Function:
      return "function";
    case TypeKind::ExactObject:
      return "exactObject";
    case TypeKind::Union:
      return "union";
  }
  llvm_unreachable("unknown TypeKind");
}

} // anonymous namespace

TypeEntry TypeEntry::createLeaf(TypeKind k) {
  TypeEntry e{};
  e.kind = k;
  e.primMask = leafKindToMask(k);
  return e;
}

TypeKind TypeContext::getKind(Type t) const {
  assert(t.id_ < entries_.size() && "Type ID out of range");
  return entries_[t.id_].kind;
}

llvh::ArrayRef<Type> TypeContext::getUnionArms(Type t) const {
  assert(t.id_ < entries_.size() && "Type ID out of range");
  const auto &entry = entries_[t.id_];
  assert(entry.kind == TypeKind::Union && "Not a union type");
  return llvh::ArrayRef<Type>(
      typeArrays_.data() + entry.union_.armOffset, entry.union_.armCount);
}

Type TypeContext::reserveClassInstance(Type superClass) {
  uint32_t id = entries_.size();
  TypeEntry e{}; // primMask defaults to kNotMaskable
  e.kind = TypeKind::ClassInstance;
  e.classInstance.superClassTypeId = superClass.id_;
  e.classInstance.className = Identifier{};
  e.classInstance.fieldOffset = 0;
  e.classInstance.fieldCount = 0;
  entries_.push_back(e);
  return Type{id};
}

void TypeContext::fillClassInstance(
    Type t,
    Identifier className,
    llvh::ArrayRef<ExactObjectField> fields) {
  assert(t.id_ < entries_.size() && "Type ID out of range");
  TypeEntry &e = entries_[t.id_];
  assert(e.kind == TypeKind::ClassInstance && "Not a ClassInstance entry");
  // Copy first: fields may point into fieldArrays_, which the append below
  // can reallocate.
  llvh::SmallVector<ExactObjectField, 8> copy(fields.begin(), fields.end());
  e.classInstance.className = className;
  e.classInstance.fieldOffset = safePossiblyNarrowingCast<uint32_t>(
      fieldArrays_.size(), "field array offset overflow");
  e.classInstance.fieldCount =
      safePossiblyNarrowingCast<uint16_t>(copy.size(), "too many class fields");
  fieldArrays_.insert(fieldArrays_.end(), copy.begin(), copy.end());
}

Type TypeContext::createClassInstance(
    Type superClass,
    Identifier className,
    llvh::ArrayRef<ExactObjectField> fields) {
  Type t = reserveClassInstance(superClass);
  fillClassInstance(t, className, fields);
  return t;
}

Type TypeContext::getClassSuperType(Type t) const {
  assert(getKind(t) == TypeKind::ClassInstance && "Not a ClassInstance");
  return Type{entries_[t.id_].classInstance.superClassTypeId};
}

Identifier TypeContext::getClassName(Type t) const {
  assert(getKind(t) == TypeKind::ClassInstance && "Not a ClassInstance");
  return entries_[t.id_].classInstance.className;
}

llvh::ArrayRef<ExactObjectField> TypeContext::getClassFields(Type t) const {
  assert(getKind(t) == TypeKind::ClassInstance && "Not a ClassInstance");
  const auto &ci = entries_[t.id_].classInstance;
  return llvh::ArrayRef<ExactObjectField>(
      fieldArrays_.data() + ci.fieldOffset, ci.fieldCount);
}

bool TypeContext::isSubclass(Type a, Type b) const {
  assert(getKind(a) == TypeKind::ClassInstance && "a is not a class");
  assert(getKind(b) == TypeKind::ClassInstance && "b is not a class");
  uint32_t cur = a.id_;
  while (cur != kNoTypeId) {
    if (cur == b.id_)
      return true;
    if (entries_[cur].kind != TypeKind::ClassInstance)
      break;
    cur = entries_[cur].classInstance.superClassTypeId;
  }
  return false;
}

bool TypeContext::isLeafSubtypeById(uint32_t a, uint32_t b) const {
  if (a == b)
    return true;
  TypeKind ak = entries_[a].kind;
  TypeKind bk = entries_[b].kind;
  if (isRefinedKind(ak) || isRefinedKind(bk)) {
    // Every refinement is a subtype of unrefined Object.
    if (isRefinedKind(ak) && bk == TypeKind::Object)
      return true;
    // Nominal class subtyping via the superclass chain.
    if (ak == TypeKind::ClassInstance && bk == TypeKind::ClassInstance)
      return isSubclass(Type{a}, Type{b});
    // Shallow model (D4): all other refined relationships -- different
    // categories, distinct structural types, refined vs primitive -- are not
    // subtypes. Factored here so deep structural subtyping can be added later
    // without touching call sites.
    return false;
  }
  return isLeafSubtype(ak, bk);
}

bool TypeContext::areLeafTypesDisjointById(uint32_t a, uint32_t b) const {
  TypeKind ak = entries_[a].kind;
  TypeKind bk = entries_[b].kind;
  if (isRefinedKind(ak) || isRefinedKind(bk)) {
    // Subtype either way => overlap.
    if (isLeafSubtypeById(a, b) || isLeafSubtypeById(b, a))
      return false;
    // Two classes with no inheritance relation are disjoint.
    if (ak == TypeKind::ClassInstance && bk == TypeKind::ClassInstance)
      return true;
    // Exactly one is refined: disjoint unless the other is Object (which the
    // subtype check above already resolved as overlap).
    if (isRefinedKind(ak) != isRefinedKind(bk)) {
      TypeKind other = isRefinedKind(ak) ? bk : ak;
      return other != TypeKind::Object;
    }
    // Two distinct structural refined types (or class vs structural):
    // conservatively NOT disjoint (D5).
    return false;
  }
  return areLeafKindsDisjoint(ak, bk);
}

bool TypeContext::isNoType(Type t) const {
  return entries_[t.id_].kind == TypeKind::NoType;
}

template <typename Pred>
bool TypeContext::containsMatchingKind(uint32_t id, Pred pred) const {
  assert(id < entries_.size() && "Type ID out of range");
  const auto &entry = entries_[id];
  if (entry.kind != TypeKind::Union)
    return pred(entry.kind);
  auto arms = getUnionArms(Type{id});
  for (Type arm : arms) {
    assert(entries_[arm.id_].kind != TypeKind::Union && "Nested unions");
    if (pred(entries_[arm.id_].kind))
      return true;
  }
  return false;
}

template <typename Pred>
bool TypeContext::allMatchKind(uint32_t id, Pred pred) const {
  assert(id < entries_.size() && "Type ID out of range");
  const auto &entry = entries_[id];
  if (entry.kind == TypeKind::NoType)
    return false;
  if (entry.kind != TypeKind::Union)
    return pred(entry.kind);
  auto arms = getUnionArms(Type{id});
  for (Type arm : arms) {
    if (!pred(entries_[arm.id_].kind))
      return false;
  }
  return true;
}

unsigned TypeContext::countKinds(Type t) const {
  assert(t.id_ < entries_.size() && "Type ID out of range");
  const auto &entry = entries_[t.id_];
  if (entry.kind == TypeKind::NoType)
    return 0;
  if (entry.kind == TypeKind::Union)
    return entry.union_.armCount;
  return 1;
}

TypeKind TypeContext::getFirstKind(Type t) const {
  assert(t.id_ < entries_.size() && "Type ID out of range");
  const auto &entry = entries_[t.id_];
  if (entry.kind != TypeKind::Union)
    return entry.kind;
  // For unions, return the kind of the first arm.
  auto arms = getUnionArms(t);
  assert(!arms.empty() && "Union with no arms");
  return entries_[arms[0].id_].kind;
}

void TypeContext::printLeaf(llvh::raw_ostream &OS, uint32_t id) const {
  TypeKind k = entries_[id].kind;
  if (k == TypeKind::ClassInstance) {
    OS << "classInstance(";
    Identifier name = entries_[id].classInstance.className;
    if (name.isValid())
      OS << name.str();
    OS << ')';
    return;
  }
  OS << kindName(k);
}

void TypeContext::print(llvh::raw_ostream &OS, Type t) const {
  assert(t.id_ < entries_.size() && "Type ID out of range");
  const auto &entry = entries_[t.id_];

  if (entry.kind == TypeKind::NoType) {
    OS << "notype";
    return;
  }

  if (entry.kind != TypeKind::Union) {
    printLeaf(OS, t.id_);
    return;
  }

  // Union: check for the "any" shorthand. If this union is a superset of
  // AnyType, print "any" plus any extra arms (empty, uninit).
  if (isSubsetOf(Type{kAnyTypeId}, t)) {
    OS << "any";
    auto arms = getUnionArms(t);
    for (Type arm : arms) {
      // Skip kinds that are part of AnyType.
      if (isSubsetOf(arm, Type{kAnyTypeId}))
        continue;
      OS << '|';
      printLeaf(OS, arm.id_);
    }
    return;
  }

  // General union: pipe-separated arms.
  auto arms = getUnionArms(t);
  for (size_t i = 0; i < arms.size(); ++i) {
    if (i != 0)
      OS << '|';
    printLeaf(OS, arms[i].id_);
  }
}

bool TypeContext::isKnownPrimitiveType(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  // Every JavaScript primitive -- including each number subtype -- is maskable,
  // so a non-maskable type necessarily contains a non-primitive component and
  // cannot be a single primitive.
  if (m == kNotMaskable)
    return false;
  // Object/Empty/Uninit are not JavaScript primitives.
  if (m & (MB_Object | MB_Empty | MB_Uninit))
    return false;
  // Count distinct JavaScript primitive types. The number family
  // (Number/Int32/Uint32/UInt31, and any union of them) is the single
  // primitive "number", so a non-empty number code contributes exactly one
  // regardless of which subtypes it covers.
  unsigned disCount = llvh::countPopulation((uint16_t)(m & ~kNumCodeMask));
  unsigned numCount = (m & kNumCodeMask) != NC_None ? 1 : 0;
  return disCount + numCount == 1;
}

bool TypeContext::canBeAny(Type t) const {
  return isSubsetOf(Type{kAnyTypeId}, t);
}

bool TypeContext::canBeType(Type a, Type b) const {
  return isSubsetOf(b, a);
}

bool TypeContext::isProperSubsetOf(Type a, Type b) const {
  return a != b && isSubsetOf(a, b);
}

llvh::iterator_range<Type::iterator> TypeContext::arms(Type t) const {
  return {t.begin(*this), t.end(*this)};
}

bool TypeContext::canBeNumber(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & kNumCodeMask) != NC_None;
  return containsMatchingKind(t.id_, isNumberKind);
}

bool TypeContext::canBeString(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & MB_String) != 0;
  return containsMatchingKind(
      t.id_, [](TypeKind k) { return k == TypeKind::String; });
}

bool TypeContext::canBeObject(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & MB_Object) != 0;
  return containsMatchingKind(t.id_, isObjectKind);
}

bool TypeContext::canBeNull(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & MB_Null) != 0;
  return containsMatchingKind(
      t.id_, [](TypeKind k) { return k == TypeKind::Null; });
}

bool TypeContext::canBeUndefined(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & MB_Undefined) != 0;
  return containsMatchingKind(
      t.id_, [](TypeKind k) { return k == TypeKind::Undefined; });
}

bool TypeContext::canBeEmpty(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & MB_Empty) != 0;
  return containsMatchingKind(
      t.id_, [](TypeKind k) { return k == TypeKind::Empty; });
}

bool TypeContext::canBeUninit(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & MB_Uninit) != 0;
  return containsMatchingKind(
      t.id_, [](TypeKind k) { return k == TypeKind::Uninit; });
}

bool TypeContext::canBeBigInt(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & MB_BigInt) != 0;
  return containsMatchingKind(
      t.id_, [](TypeKind k) { return k == TypeKind::BigInt; });
}

bool TypeContext::canBeBoolean(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & MB_Boolean) != 0;
  return containsMatchingKind(
      t.id_, [](TypeKind k) { return k == TypeKind::Boolean; });
}

bool TypeContext::canBeSymbol(Type t) const {
  uint16_t m = entries_[t.id_].primMask;
  if (m != kNotMaskable)
    return (m & MB_Symbol) != 0;
  return containsMatchingKind(
      t.id_, [](TypeKind k) { return k == TypeKind::Symbol; });
}

bool TypeContext::isPrimitive(Type t) const {
  {
    uint16_t m = entries_[t.id_].primMask;
    if (m != kNotMaskable)
      return m != 0 && (m & (MB_Object | MB_Empty | MB_Uninit)) == 0;
  }
  return allMatchKind(t.id_, isPrimitiveKind);
}

bool TypeContext::canBePrimitive(Type t) const {
  {
    uint16_t m = entries_[t.id_].primMask;
    if (m != kNotMaskable)
      return (m & kNumCodeMask) != NC_None ||
          (m &
           (MB_Undefined | MB_Null | MB_Boolean | MB_BigInt | MB_String |
            MB_Symbol)) != 0;
  }
  if (t.id_ == kNoTypeId)
    return false;
  return containsMatchingKind(t.id_, isPrimitiveKind);
}

bool TypeContext::isIDZType(Type t) const {
  // A field needs IDZ unless it can hold a non-symbol primitive, which provides
  // a representable literal default (0/""/false/0n/undefined/null). Object
  // types and symbol have no such default and must be IDZ-checked.
  {
    uint16_t m = entries_[t.id_].primMask;
    if (m != kNotMaskable)
      return (m & kNumCodeMask) == NC_None &&
          (m & (MB_Undefined | MB_Null | MB_Boolean | MB_BigInt | MB_String)) ==
          0;
  }
  if (t.id_ == kNoTypeId)
    return false;
  return !containsMatchingKind(t.id_, [](TypeKind k) {
    return isPrimitiveKind(k) && k != TypeKind::Symbol;
  });
}

bool TypeContext::isNonPtr(Type t) const {
  {
    uint16_t m = entries_[t.id_].primMask;
    if (m != kNotMaskable)
      return m != 0 &&
          (m &
           (MB_BigInt | MB_String | MB_Symbol | MB_Object | MB_Empty |
            MB_Uninit)) == 0;
  }
  return allMatchKind(t.id_, isNonPtrKind);
}

bool TypeContext::isSubsetOf(Type a, Type b) const {
  if (a.id_ == b.id_)
    return true;
  if (a.id_ == kNoTypeId)
    return true;
  if (b.id_ == kNoTypeId)
    return false;

  uint16_t ma = entries_[a.id_].primMask;
  uint16_t mb = entries_[b.id_].primMask;
  if (ma != kNotMaskable && mb != kNotMaskable) {
    uint16_t aDis = (uint16_t)(ma & ~kNumCodeMask);
    uint16_t bDis = (uint16_t)(mb & ~kNumCodeMask);
    if ((aDis & bDis) != aDis)
      return false;
    // a_num <: b_num  iff  meet(a_num,b_num) == a_num
    uint16_t na = ma & kNumCodeMask, nb = mb & kNumCodeMask;
    return kNumberMeet[na][nb] == na;
  }

  TypeKind aKind = entries_[a.id_].kind;
  TypeKind bKind = entries_[b.id_].kind;

  // Union on left: all arms must be subsets of b.
  if (aKind == TypeKind::Union) {
    for (Type arm : getUnionArms(a)) {
      if (!isSubsetOf(arm, b))
        return false;
    }
    return true;
  }

  // Leaf on left, union on right: a must be subset of some arm.
  if (bKind == TypeKind::Union) {
    for (Type arm : getUnionArms(b)) {
      if (isSubsetOf(a, arm))
        return true;
    }
    return false;
  }

  // Both leaf types (id-aware: distinguishes nominal classes).
  return isLeafSubtypeById(a.id_, b.id_);
}

bool TypeContext::areDisjoint(Type a, Type b) const {
  if (a.id_ == kNoTypeId || b.id_ == kNoTypeId)
    return true;
  if (a.id_ == b.id_)
    return false;

  uint16_t ma = entries_[a.id_].primMask;
  uint16_t mb = entries_[b.id_].primMask;
  if (ma != kNotMaskable && mb != kNotMaskable) {
    if ((ma & ~kNumCodeMask) & (mb & ~kNumCodeMask))
      return false; // shared disjoint kind
    return kNumberMeet[ma & kNumCodeMask][mb & kNumCodeMask] == NC_None;
  }

  TypeKind aKind = entries_[a.id_].kind;
  TypeKind bKind = entries_[b.id_].kind;

  // Union on left: disjoint iff all arms are disjoint from b.
  if (aKind == TypeKind::Union) {
    for (Type arm : getUnionArms(a)) {
      if (!areDisjoint(arm, b))
        return false;
    }
    return true;
  }

  // Union on right: disjoint iff a is disjoint from all arms.
  if (bKind == TypeKind::Union) {
    for (Type arm : getUnionArms(b)) {
      if (!areDisjoint(a, arm))
        return false;
    }
    return true;
  }

  // Both leaf types (id-aware, conservative for refined kinds).
  return areLeafTypesDisjointById(a.id_, b.id_);
}

uint32_t TypeContext::createUnionImpl(uint32_t a, uint32_t b) {
  // Collect sorted arm ID lists from both operands. Leaf types produce
  // single-element sequences; union arms are already sorted.
  auto collectArms = [this](uint32_t id, llvh::SmallVectorImpl<uint32_t> &out) {
    if (entries_[id].kind == TypeKind::Union) {
      for (Type arm : getUnionArms(Type{id}))
        out.push_back(arm.id_);
    } else {
      out.push_back(id);
    }
  };
  llvh::SmallVector<uint32_t, 8> aArms, bArms;
  collectArms(a, aArms);
  collectArms(b, bArms);

  // Remove cross-subsumed arms. Arms within each input are already
  // canonical (no internal subsumption), so we only check across inputs.
  auto isSubsumedBy = [this](uint32_t id, llvh::ArrayRef<uint32_t> others) {
    for (uint32_t o : others) {
      if (o != id && isLeafSubtypeById(id, o))
        return true;
    }
    return false;
  };

  llvh::SmallVector<uint32_t, 8> aFiltered, bFiltered;
  for (uint32_t id : aArms) {
    if (!isSubsumedBy(id, bArms))
      aFiltered.push_back(id);
  }
  for (uint32_t id : bArms) {
    if (!isSubsumedBy(id, aArms))
      bFiltered.push_back(id);
  }

  // Merge both sorted sequences, skipping duplicates.
  llvh::SmallVector<uint32_t, 16> arms;
  size_t ai = 0, bi = 0;
  while (ai < aFiltered.size() && bi < bFiltered.size()) {
    if (aFiltered[ai] < bFiltered[bi]) {
      arms.push_back(aFiltered[ai++]);
    } else if (aFiltered[ai] > bFiltered[bi]) {
      arms.push_back(bFiltered[bi++]);
    } else {
      arms.push_back(aFiltered[ai]);
      ++ai;
      ++bi;
    }
  }
  while (ai < aFiltered.size())
    arms.push_back(aFiltered[ai++]);
  while (bi < bFiltered.size())
    arms.push_back(bFiltered[bi++]);

  if (arms.empty())
    return kNoTypeId;
  if (arms.size() == 1)
    return arms[0];

  // Intern: check if this union already exists.
  UnionInternKey key(arms);
  auto it = internTable_.find(key);
  if (it != internTable_.end())
    return it->second;

  // Create new union entry.
  uint32_t id = addUnionEntry(arms);
  internTable_[std::move(key)] = id;
  return id;
}

Type TypeContext::unionTy(Type a, Type b) {
  if (a.id_ == b.id_)
    return a;
  if (a.id_ == kNoTypeId)
    return b;
  if (b.id_ == kNoTypeId)
    return a;
  uint16_t ma = entries_[a.id_].primMask;
  uint16_t mb = entries_[b.id_].primMask;
  if (ma != kNotMaskable && mb != kNotMaskable) {
    uint16_t dis = (uint16_t)((ma | mb) & ~kNumCodeMask);
    uint16_t nc = kNumberJoin[ma & kNumCodeMask][mb & kNumCodeMask];
    return lookupPrimMask((uint16_t)(dis | nc));
  }
  if (isSubsetOf(a, b))
    return b;
  if (isSubsetOf(b, a))
    return a;
  return Type{createUnionImpl(a.id_, b.id_)};
}

uint32_t TypeContext::intersectLeafTy(uint32_t a, uint32_t b) const {
  assert(a != kNoTypeId && b != kNoTypeId && "NoType must be handled first");
  TypeKind aKind = entries_[a].kind;
  TypeKind bKind = entries_[b].kind;
  assert(aKind != TypeKind::Union && bKind != TypeKind::Union);

  if (isLeafSubtypeById(a, b))
    return a;
  if (isLeafSubtypeById(b, a))
    return b;

  // The only non-subset overlap in the number family is Int32 ∩ Uint32 =
  // UInt31. All other number-family pairs are handled by the subtype checks
  // above.
  if (isNumberKind(aKind) && isNumberKind(bKind))
    return kUInt31Id;
  return kNoTypeId;
}

uint32_t TypeContext::createUnionFromLeafArms(llvh::ArrayRef<uint32_t> arms) {
  llvh::SmallVector<uint32_t, 16> normalized;
  normalized.reserve(arms.size());
  for (uint32_t arm : arms) {
    if (arm != kNoTypeId)
      normalized.push_back(arm);
  }

  if (normalized.empty())
    return kNoTypeId;

  std::sort(normalized.begin(), normalized.end());
  normalized.erase(
      std::unique(normalized.begin(), normalized.end()), normalized.end());

  llvh::SmallVector<uint32_t, 16> filtered;
  filtered.reserve(normalized.size());
  for (size_t i = 0; i < normalized.size(); ++i) {
    bool subsumed = false;
    for (size_t j = 0; j < normalized.size(); ++j) {
      if (i == j)
        continue;
      if (isLeafSubtypeById(normalized[i], normalized[j])) {
        subsumed = true;
        break;
      }
    }
    if (!subsumed)
      filtered.push_back(normalized[i]);
  }

  if (filtered.empty())
    return kNoTypeId;
  if (filtered.size() == 1)
    return filtered[0];

  UnionInternKey key(filtered);
  auto it = internTable_.find(key);
  if (it != internTable_.end())
    return it->second;

  uint32_t id = addUnionEntry(filtered);
  internTable_[std::move(key)] = id;
  return id;
}

Type TypeContext::materializePrimMask(uint16_t m) {
  assert(m != kNotMaskable && "cannot materialize a non-maskable mask");
  llvh::SmallVector<uint32_t, 12> arms;
  if (m & MB_Undefined)
    arms.push_back(kUndefinedId);
  if (m & MB_Null)
    arms.push_back(kNullId);
  if (m & MB_Boolean)
    arms.push_back(kBooleanId);
  if (m & MB_BigInt)
    arms.push_back(kBigIntId);
  if (m & MB_String)
    arms.push_back(kStringId);
  if (m & MB_Symbol)
    arms.push_back(kSymbolId);
  if (m & MB_Object)
    arms.push_back(kObjectId);
  if (m & MB_Empty)
    arms.push_back(kEmptyId);
  if (m & MB_Uninit)
    arms.push_back(kUninitId);
  switch (m & kNumCodeMask) {
    case NC_None:
      break;
    case NC_UInt31:
      arms.push_back(kUInt31Id);
      break;
    case NC_Int32:
      arms.push_back(kInt32Id);
      break;
    case NC_Uint32:
      arms.push_back(kUint32Id);
      break;
    case NC_Int32OrUint32:
      arms.push_back(kInt32Id);
      arms.push_back(kUint32Id);
      break;
    case NC_Number:
      arms.push_back(kNumberId);
      break;
    default:
      assert(false && "invalid number code in primMask");
      break;
  }
  // createUnionFromLeafArms returns NoType for empty, the leaf id for one arm,
  // or an interned union otherwise -- the same canonical id the slow path uses.
  return Type{createUnionFromLeafArms(arms)};
}

Type TypeContext::lookupPrimMask(uint16_t m) {
  assert(m < kNumPrimMasks && "mask out of range (kNotMaskable passed?)");
  auto it = primCache_.find(m);
  if (it != primCache_.end())
    return it->second;
  Type t = materializePrimMask(m);
  primCache_.insert({m, t});
  return t;
}

Type TypeContext::intersectTy(Type a, Type b) {
  if (a.id_ == b.id_)
    return a;
  if (a.id_ == kNoTypeId || b.id_ == kNoTypeId)
    return Type{kNoTypeId};
  uint16_t ma = entries_[a.id_].primMask;
  uint16_t mb = entries_[b.id_].primMask;
  if (ma != kNotMaskable && mb != kNotMaskable) {
    uint16_t dis = (uint16_t)((ma & mb) & ~kNumCodeMask);
    uint16_t nc = kNumberMeet[ma & kNumCodeMask][mb & kNumCodeMask];
    return lookupPrimMask((uint16_t)(dis | nc));
  }
  TypeKind aKind = entries_[a.id_].kind;
  TypeKind bKind = entries_[b.id_].kind;

  if (aKind != TypeKind::Union && bKind != TypeKind::Union)
    return Type{intersectLeafTy(a.id_, b.id_)};

  llvh::SmallVector<uint32_t, 16> intersections;

  if (aKind == TypeKind::Union && bKind == TypeKind::Union) {
    auto aArms = getUnionArms(a);
    auto bArms = getUnionArms(b);
    intersections.reserve(aArms.size() * bArms.size());
    for (Type aArm : aArms) {
      for (Type bArm : bArms) {
        uint32_t intersection = intersectLeafTy(aArm.id_, bArm.id_);
        if (intersection != kNoTypeId)
          intersections.push_back(intersection);
      }
    }
    return Type{createUnionFromLeafArms(intersections)};
  }

  auto unionArms = aKind == TypeKind::Union ? getUnionArms(a) : getUnionArms(b);
  uint32_t leaf = aKind == TypeKind::Union ? b.id_ : a.id_;
  intersections.reserve(unionArms.size());
  for (Type arm : unionArms) {
    uint32_t intersection = intersectLeafTy(arm.id_, leaf);
    if (intersection != kNoTypeId)
      intersections.push_back(intersection);
  }
  return Type{createUnionFromLeafArms(intersections)};
}

Type TypeContext::subtractTy(Type a, Type b) {
  if (a.id_ == kNoTypeId)
    return Type{kNoTypeId};
  if (b.id_ == kNoTypeId)
    return a;
  uint16_t ma = entries_[a.id_].primMask;
  uint16_t mb = entries_[b.id_].primMask;
  if (ma != kNotMaskable && mb != kNotMaskable) {
    uint16_t aDis = (uint16_t)(ma & ~kNumCodeMask);
    uint16_t bDis = (uint16_t)(mb & ~kNumCodeMask);
    uint16_t dis = (uint16_t)(aDis & ~bDis);
    uint16_t nc = kNumberSub[ma & kNumCodeMask][mb & kNumCodeMask];
    return lookupPrimMask((uint16_t)(dis | nc));
  }
  if (isSubsetOf(a, b))
    return Type{kNoTypeId};
  if (areDisjoint(a, b))
    return a;

  // Distribute over unions in a. Copy arms first because unionTy may
  // reallocate typeArrays_, invalidating the ArrayRef from getUnionArms.
  if (entries_[a.id_].kind == TypeKind::Union) {
    llvh::SmallVector<Type, 16> arms;
    for (Type arm : getUnionArms(a))
      arms.push_back(arm);
    Type result{kNoTypeId};
    for (Type arm : arms)
      result = unionTy(result, subtractTy(arm, b));
    return result;
  }

  // a is a leaf, not subset of b, not disjoint from b.
  // Conservative: return a.
  return a;
}

uint32_t TypeContext::addUnionEntry(llvh::ArrayRef<uint32_t> arms) {
  assert(arms.size() >= 2 && "Union must have at least 2 arms");
  // Fold arm masks: OR the disjoint bits, join the number codes; any
  // non-maskable arm makes the union non-maskable.
  uint16_t disjoint = 0;
  uint16_t numCode = NC_None;
  bool maskable = true;
  for (uint32_t armId : arms) {
    uint16_t m = entries_[armId].primMask;
    if (m == kNotMaskable) {
      maskable = false;
      break;
    }
    disjoint |= (m & ~kNumCodeMask);
    numCode = kNumberJoin[numCode][m & kNumCodeMask];
  }
  uint32_t offset = safePossiblyNarrowingCast<uint32_t>(
      typeArrays_.size(), "type array offset overflow");
  for (uint32_t armId : arms)
    typeArrays_.push_back(Type(armId));
  uint32_t id = entries_.size();
  entries_.push_back(
      TypeEntry::createUnion(
          offset,
          safePossiblyNarrowingCast<uint16_t>(
              arms.size(), "too many union arms")));
  entries_[id].primMask =
      maskable ? (uint16_t)(disjoint | numCode) : kNotMaskable;
  return id;
}

TypeContext::TypeContext() {
  // Reserve space for well-known entries.
  entries_.reserve(kFirstDynamicId);

  // Pre-allocate leaf type entries. The order must match the well-known ID
  // constants exactly.

  // 0: NoType
  entries_.push_back(TypeEntry::createLeaf(TypeKind::NoType));
  assert(entries_.size() - 1 == kNoTypeId);

  // 1: Empty
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Empty));
  assert(entries_.size() - 1 == kEmptyId);

  // 2: Uninit
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Uninit));
  assert(entries_.size() - 1 == kUninitId);

  // 3: Undefined
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Undefined));
  assert(entries_.size() - 1 == kUndefinedId);

  // 4: Null
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Null));
  assert(entries_.size() - 1 == kNullId);

  // 5: Boolean
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Boolean));
  assert(entries_.size() - 1 == kBooleanId);

  // 6: String
  entries_.push_back(TypeEntry::createLeaf(TypeKind::String));
  assert(entries_.size() - 1 == kStringId);

  // 7: Number
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Number));
  assert(entries_.size() - 1 == kNumberId);

  // 8: BigInt
  entries_.push_back(TypeEntry::createLeaf(TypeKind::BigInt));
  assert(entries_.size() - 1 == kBigIntId);

  // 9: Symbol
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Symbol));
  assert(entries_.size() - 1 == kSymbolId);

  // 10: Environment
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Environment));
  assert(entries_.size() - 1 == kEnvironmentId);

  // 11: PrivateName
  entries_.push_back(TypeEntry::createLeaf(TypeKind::PrivateName));
  assert(entries_.size() - 1 == kPrivateNameId);

  // 12: FunctionCode
  entries_.push_back(TypeEntry::createLeaf(TypeKind::FunctionCode));
  assert(entries_.size() - 1 == kFunctionCodeId);

  // 13: Object
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Object));
  assert(entries_.size() - 1 == kObjectId);

  // 14: Bits32
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Bits32));
  assert(entries_.size() - 1 == kBits32Id);

  // 15: Int32
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Int32));
  assert(entries_.size() - 1 == kInt32Id);

  // 16: Uint32
  entries_.push_back(TypeEntry::createLeaf(TypeKind::Uint32));
  assert(entries_.size() - 1 == kUint32Id);

  // 17: UInt31 (Int32 ∩ Uint32)
  entries_.push_back(TypeEntry::createLeaf(TypeKind::UInt31));
  assert(entries_.size() - 1 == kUInt31Id);

  // 18: AnyType — union of all JS-observable types (matching TYPE_ANY_MASK).
  // Primitives + Object, excludes Empty, Uninit, Environment, PrivateName,
  // FunctionCode, Bits32.
  {
    uint32_t anyArms[] = {
        kUndefinedId,
        kNullId,
        kBooleanId,
        kStringId,
        kNumberId,
        kBigIntId,
        kSymbolId,
        kObjectId};
    uint32_t id = addUnionEntry(anyArms);
    (void)id;
    assert(id == kAnyTypeId);
  }

  // 19: Numeric — Number | BigInt.
  {
    uint32_t numericArms[] = {kNumberId, kBigIntId};
    uint32_t id = addUnionEntry(numericArms);
    (void)id;
    assert(id == kNumericId);
  }

  // 20: AnyEmptyUninit — any | Empty | Uninit.
  // This is a union of all the AnyType arms plus Empty and Uninit.
  {
    uint32_t aeuArms[] = {
        kEmptyId,
        kUninitId,
        kUndefinedId,
        kNullId,
        kBooleanId,
        kStringId,
        kNumberId,
        kBigIntId,
        kSymbolId,
        kObjectId};
    uint32_t id = addUnionEntry(aeuArms);
    (void)id;
    assert(id == kAnyEmptyUninitId);
  }

  // 21: NullOrUndef — Null | Undefined.
  {
    uint32_t nuArms[] = {kUndefinedId, kNullId};
    uint32_t id = addUnionEntry(nuArms);
    (void)id;
    assert(id == kNullOrUndefId);
  }

  // 22: StringOrSymbol — String | Symbol.
  {
    uint32_t arms[] = {kStringId, kSymbolId};
    uint32_t id = addUnionEntry(arms);
    (void)id;
    assert(id == kStringOrSymbolId);
  }

  // 23: EmptyOrUninit — Empty | Uninit.
  {
    uint32_t arms[] = {kEmptyId, kUninitId};
    uint32_t id = addUnionEntry(arms);
    (void)id;
    assert(id == kEmptyOrUninitId);
  }

  // 24: ObjectOrNull — Null | Object (sorted by ID).
  {
    uint32_t arms[] = {kNullId, kObjectId};
    uint32_t id = addUnionEntry(arms);
    (void)id;
    assert(id == kObjectOrNullId);
  }

  // 25: ObjectOrUndef — Undefined | Object (sorted by ID).
  {
    uint32_t arms[] = {kUndefinedId, kObjectId};
    uint32_t id = addUnionEntry(arms);
    (void)id;
    assert(id == kObjectOrUndefId);
  }

  // Pre-populate intern table with well-known unions.
  // TypeContext is a friend of Type, so we can access Type::id_.
  for (uint32_t id = _kFirstUnionId; id <= _kLastUnionId; ++id) {
    auto arms = getUnionArms(Type{id});
    UnionInternKey key;
    key.arms.reserve(arms.size());
    for (Type t : arms)
      key.arms.push_back(t.id_);
    internTable_[std::move(key)] = id;
  }

  // Pad remaining entries up to kFirstDynamicId with NoType placeholders.
  while (entries_.size() < kFirstDynamicId) {
    entries_.push_back(TypeEntry::createLeaf(TypeKind::NoType));
  }
  assert(entries_.size() == kFirstDynamicId);
}

// All Type operations now go through TypeContext explicitly. The only
// out-of-line Type method that remains here is the trivial print fallback,
// which is used by the operator<<(raw_ostream &, Type) overload in IR.cpp
// for diagnostic streams that do not have a TypeContext available.

void Type::print(llvh::raw_ostream &OS) const {
  OS << "type#" << id_;
}

Type Type::iterator::operator*() const {
  if (ctx_->entries_[type_.id_].kind != TypeKind::Union) {
    // Non-union: yield the type itself.
    return type_;
  }
  return ctx_->getUnionArms(type_)[index_];
}

Type::iterator Type::begin(const TypeContext &ctx) const {
  if (isNoType())
    return end(ctx);
  return iterator(ctx, *this, 0);
}

Type::iterator Type::end(const TypeContext &ctx) const {
  if (ctx.entries_[id_].kind == TypeKind::Union)
    return iterator(ctx, *this, ctx.getUnionArms(*this).size());
  return iterator(ctx, *this, isNoType() ? 0 : 1);
}

} // namespace hermes
