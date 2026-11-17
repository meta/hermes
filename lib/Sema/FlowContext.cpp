/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Sema/FlowContext.h"

#include "hermes/Support/Conversions.h"

#include "llvh/ADT/Hashing.h"
#include "llvh/ADT/STLExtras.h"
#include "llvh/ADT/ScopeExit.h"
#include "llvh/ADT/SmallPtrSet.h"
#include "llvh/Support/MathExtras.h"

#include <cmath>
#include <limits>

namespace hermes {
namespace flow {

namespace {
/// Compare two sequences of types lexicographically.
template <typename It, typename Comp>
int lexicographicalComparison(
    It first1,
    It last1,
    It first2,
    It last2,
    Comp compare) {
  for (; first1 != last1; ++first1, ++first2) {
    // If sequence 2 is a prefix of sequence 1, sequence 1 is greater.
    if (first2 == last2)
      return 1;
    if (auto tmpRes = compare(*first1, *first2))
      return tmpRes;
  }

  return first2 == last2 ? 0 : -1;
}

template <typename It, typename F>
llvh::hash_code hashTypes(It first, It last, F hash) {
  llvh::hash_code hashCode = 0;
  for (; first != last; ++first) {
    hashCode = llvh::hash_combine(hashCode, hash(*first));
  }
  return hashCode;
}

/// A helper for comparing bools, returning -1, 0, +1.
inline int cmpHelperBool(bool a, bool b) {
  return (int)a - (int)b;
}

/// A helper for comparing nullable Type pointers, returning -1, 0, +1.
/// \p state must be threaded through, otherwise recursive types (e.g. a
/// function whose 'this' or return type refers back to itself) recurse forever.
int cmpHelper(Type *a, Type *b, TypeInfo::CompareState &state) {
  return a ? b ? a->info->compare(b->info, state) : 1 : b ? -1 : 0;
}

/// A helper for testing nullable Type pointers for equality.
/// Uses equals() rather than compare(), because \c CompareState::cache is
/// shared between the two and stores a bool for equals() but -1/0/1 for
/// compare().
bool eqHelper(Type *a, Type *b, TypeInfo::CompareState &state) {
  if (!a || !b)
    return a == b;
  return a->info->equals(b->info, state);
}

/// Compare recursive lists of types on the left and right.
/// Runs in quadratic time, checks for each pair of (left, right) types.
/// \return true if every element of \p left has a corresponding entry on \p
///   right and vice versa.
static bool listsMatchSlow(
    llvh::ArrayRef<Type *> left,
    llvh::ArrayRef<Type *> right,
    TypeInfo::CompareState &state) {
  // matched[i] indicates whether right[i] has a match on the left.
  llvh::BitVector matched{};
  matched.resize(right.size(), false);

  // First check all left entries for matches on the right, marking the
  // matched bit.
  for (Type *l : left) {
    bool found = false;
    for (size_t j = 0, e = right.size(); j < e; ++j) {
      Type *r = right[j];
      if (l->info->equals(r->info, state)) {
        matched.set(j);
        found = true;
        // Don't break here, we want to check all entries on the right.
      }
    }
    // Missed one entry on the left, no point continuing.
    if (!found)
      return false;
  }

  // Only return true if all the right entries have been matched.
  return matched.all();
}

/// \param active the Types currently being printed, to stop at recursive
///   types that have no alias name.
static void messageStringImpl(
    const Type *type,
    llvh::raw_ostream &os,
    llvh::SmallPtrSetImpl<const Type *> &active) {
  if (!active.insert(type).second) {
    os << "...";
    return;
  }
  auto popActive =
      llvh::make_scope_exit([type, &active]() { active.erase(type); });

#if HERMES_PARSE_FLOW
  if (type->node && llvh::isa<ESTree::TypeAliasNode>(type->node)) {
    // Return the name along with the kind of the type.
    if (type->info)
      os << type->info->getKindName() << " ";
    os << llvh::cast<ESTree::IdentifierNode>(
              llvh::cast<ESTree::TypeAliasNode>(type->node)->_id)
              ->_name->str();
    return;
  }
#endif

  if (!type->info) {
    os << "unknown";
    return;
  }

  if (auto *classType = llvh::dyn_cast<ClassType>(type->info)) {
    // If the class node has a name use that instead.
    if (type->node && llvh::isa<ESTree::ClassDeclarationNode>(type->node)) {
      os << "class "
         << llvh::cast<ESTree::IdentifierNode>(
                llvh::cast<ESTree::ClassDeclarationNode>(type->node)->_id)
                ->_name->str();
    } else if (classType->getClassName().isValid()) {
      // Types with no declaration node, e.g. the home object of `C.prototype`.
      os << "class " << classType->getClassName().str();
    } else {
      os << "class";
    }
    if (!classType->getTypeArgs().empty()) {
      os << '<';
      bool first = true;
      for (const Type *arg : classType->getTypeArgs()) {
        if (!first)
          os << ", ";
        messageStringImpl(arg, os, active);
        first = false;
      }
      os << '>';
    }
    return;
  }

  if (auto *unionType = llvh::dyn_cast<UnionType>(type->info)) {
    bool first = true;
    os << '(';
    for (const Type *t : unionType->getTypes()) {
      if (!first) {
        os << " | ";
      }
      messageStringImpl(t, os, active);
      first = false;
    }
    os << ')';
    return;
  }

  if (auto *tupleType = llvh::dyn_cast<TupleType>(type->info)) {
    bool first = true;
    os << '[';
    for (const Type *t : tupleType->getTypes()) {
      if (!first) {
        os << ", ";
      }
      messageStringImpl(t, os, active);
      first = false;
    }
    os << ']';
    return;
  }

  if (auto *strLitType = llvh::dyn_cast<StringLiteralType>(type->info)) {
    os << '"' << strLitType->getValue()->str() << '"';
    return;
  }

  if (auto *numLitType = llvh::dyn_cast<NumberLiteralType>(type->info)) {
    char buf[NUMBER_TO_STRING_BUF_SIZE];
    os << numberToString(numLitType->getValue(), buf, sizeof(buf));
    return;
  }

  if (auto *boolLitType = llvh::dyn_cast<BooleanLiteralType>(type->info)) {
    os << (boolLitType->getValue() ? "true" : "false");
    return;
  }

  os << type->info->getKindName();
}

} // anonymous namespace

std::string Type::messageString() const {
  std::string result;
  llvh::raw_string_ostream os(result);
  llvh::SmallPtrSet<const Type *, 8> active{};
  messageStringImpl(this, os, active);
  return os.str();
}

TypeKind widenLiteralKind(TypeKind kind) {
  switch (kind) {
#define _HERMES_SEMA_FLOW_DEFLITERAL(name, base) \
  case TypeKind::name:                           \
    return TypeKind::base;
    _HERMES_SEMA_FLOW_LITERAL_BASES
#undef _HERMES_SEMA_FLOW_DEFLITERAL
    default:
      return kind;
  }
}

llvh::StringRef TypeInfo::getKindName() const {
  switch (kind_) {
    case TypeKind::Void:
      return "void";
    case TypeKind::Null:
      return "null";
    case TypeKind::Boolean:
      return "boolean";
    case TypeKind::String:
      return "string";
    case TypeKind::StringLiteral:
      return "string literal";
    case TypeKind::NumberLiteral:
      return "number literal";
    case TypeKind::BooleanLiteral:
      return "boolean literal";
    case TypeKind::CPtr:
      return "c_ptr";
    case TypeKind::Number:
      return "number";
    case TypeKind::Symbol:
      return "symbol";
    case TypeKind::BigInt:
      return "bigint";
    case TypeKind::Any:
      return "any";
    case TypeKind::Empty:
      return "empty";
    case TypeKind::Mixed:
      return "mixed";
    case TypeKind::Union:
      return "union";
    case TypeKind::TypedFunction:
      return "function";
    case TypeKind::NativeFunction:
      return "native function";
    case TypeKind::UntypedFunction:
      return "untyped function";
    case TypeKind::Class:
      return "class";
    case TypeKind::ClassConstructor:
      return "class constructor";
    case TypeKind::Tuple:
      return "tuple";
    case TypeKind::ExactObject:
      return "object";
    case TypeKind::Generic:
      return "generic";
    case TypeKind::InferencePlaceholder:
      return "inference placeholder";
    case TypeKind::InferencePlaceholderArray:
      return "inference placeholder array";
  }
  llvm_unreachable("invalid TypeKind");
}

int TypeInfo::compare(const TypeInfo *other) const {
  CompareState state{};
  return compare(other, state);
}
int TypeInfo::compare(const TypeInfo *other, CompareState &state) const {
  if (other == this)
    return 0;
  if (kind_ < other->kind_)
    return -1;
  else if (kind_ > other->kind_)
    return 1;

  auto cached = state.cache.find({this, other});
  if (cached != state.cache.end()) {
    return cached->second;
  }

  if (!state.visited.insert({this, other})) {
    // Failed to insert, already visited this pair.
    // Because we haven't diverged yet, they must be equal.
    return 0;
  }

  // Remove this entry in the visited list after we finish this call.
  auto popOnExit =
      llvh::make_scope_exit([&state]() { state.visited.pop_back(); });

  int result;
  switch (kind_) {
#define _HERMES_SEMA_FLOW_DEFKIND(name)                           \
  case TypeKind::name:                                            \
    result = static_cast<const name##Type *>(this)->_compareImpl( \
        static_cast<const name##Type *>(other), state);           \
    break;
    _HERMES_SEMA_FLOW_SINGLETONS
    _HERMES_SEMA_FLOW_COMPLEX_TYPES
#undef _HERMES_SEMA_FLOW_DEFKIND
    default:
      hermes_fatal("invalid TypeKind");
  }
  state.cache[{this, other}] = result;
  return result;
}

bool TypeInfo::equals(const TypeInfo *other) const {
  CompareState state{};
  return equals(other, state);
}
bool TypeInfo::equals(const TypeInfo *other, CompareState &state) const {
  if (other == this)
    return true;

  auto cached = state.cache.find({this, other});
  if (cached != state.cache.end()) {
    return cached->second;
  }

  if (!state.visited.insert({this, other})) {
    // Failed to insert, already visited this pair.
    // Because we haven't diverged yet, they must be equal.
    return true;
  }

  // Remove this entry in the visited list after we finish this call.
  auto popOnExit =
      llvh::make_scope_exit([&state]() { state.visited.pop_back(); });

  // Need to account for either left or right not being completed unions yet.
  bool thisIncomplete = false;
  if (auto *thisUnion = llvh::dyn_cast<UnionType>(this)) {
    if (thisUnion->getNumNonLoopingTypes() == -1) {
      thisIncomplete = true;
    }
  }
  bool otherIncomplete = false;
  if (auto *otherUnion = llvh::dyn_cast<UnionType>(other)) {
    if (otherUnion->getNumNonLoopingTypes() == -1) {
      otherIncomplete = true;
    }
  }

  // If the kinds are different, the types can still be the same if one of them
  // is an incomplete union type. This is because union arms can be
  // deduplicated, resulting in any other type.
  if (kind_ != other->kind_ && !thisIncomplete && !otherIncomplete) {
    state.cache[{this, other}] = false;
    return false;
  }

  // Handle two unions when at least one of them is incomplete.
  if ((thisIncomplete || otherIncomplete) && llvh::isa<UnionType>(this) &&
      llvh::isa<UnionType>(other)) {
    bool result = listsMatchSlow(
        llvh::cast<UnionType>(this)->getTypes(),
        llvh::cast<UnionType>(other)->getTypes(),
        state);
    if (!result)
      state.cache[{this, other}] = false;
    return result;
  }
  // Handle the cases of one incomplete union and a non-union.
  if (thisIncomplete) {
    bool result = llvh::all_of(
        llvh::cast<UnionType>(this)->getTypes(),
        [&state, other](Type *t) { return t->info->equals(other, state); });
    if (!result)
      state.cache[{this, other}] = false;
    return result;
  }
  if (otherIncomplete) {
    bool result = llvh::all_of(
        llvh::cast<UnionType>(other)->getTypes(),
        [&state, this](Type *t) { return t->info->equals(this, state); });
    if (!result)
      state.cache[{this, other}] = false;
    return result;
  }

  bool result;
  switch (kind_) {
#define _HERMES_SEMA_FLOW_DEFKIND(name)                          \
  case TypeKind::name:                                           \
    result = static_cast<const name##Type *>(this)->_equalsImpl( \
        static_cast<const name##Type *>(other), state);          \
    break;
    _HERMES_SEMA_FLOW_SINGLETONS
    _HERMES_SEMA_FLOW_COMPLEX_TYPES
#undef _HERMES_SEMA_FLOW_DEFKIND
    default:
      hermes_fatal("invalid TypeKind");
  }
  if (!result)
    state.cache[{this, other}] = false;
  return result;
}

unsigned TypeInfo::hash() const {
  if (LLVM_LIKELY(hashed_))
    return hashValue_;

  unsigned hv;
  switch (kind_) {
#define _HERMES_SEMA_FLOW_DEFKIND(name)                      \
  case TypeKind::name:                                       \
    hv = static_cast<const name##Type *>(this)->_hashImpl(); \
    break;

    _HERMES_SEMA_FLOW_SINGLETONS _HERMES_SEMA_FLOW_COMPLEX_TYPES

#undef _HERMES_SEMA_FLOW_DEFKIND
        ;
    default:
      llvm_unreachable("invalid TypeKind");
  }
  hashed_ = true;
  return hashValue_ = hv;
}

UnionType::UnionType(llvh::SmallVector<Type *, 4> &&uncanonicalizedTypes)
    : types_(std::move(uncanonicalizedTypes)) {
  setHasAnyMixed();
}

void UnionType::setCanonicalTypes(
    llvh::SmallVector<Type *, 4> &&canonicalTypes,
    llvh::SmallVector<Type *, 4> &&recursiveTypes) {
  numNonLoopingTypes_ = canonicalTypes.size();
  types_ = std::move(canonicalTypes);
  types_.append(recursiveTypes.begin(), recursiveTypes.end());
  recursiveTypes.clear();
  assert(types_.size() > 1 && " Single element unions are unsupported");
  setHasAnyMixed();
}

void UnionType::canonicalizeTypes(
    llvh::ArrayRef<Type *> types,
    llvh::SmallVectorImpl<Type *> &canonicalTypes,
    llvh::SmallVectorImpl<Type *> &loopingTypes,
    Type *booleanType) {
  // Copy the union types to canonicalized, but flatten nested unions. Note that
  // there can't be more than one level.
  // The types won't cycle back to the parent because that's checked in
  // DeclareScopeTypes and this function is intended to be used after union-only
  // cycles have been detected (and errored on) in FlowChecker.
  for (Type *elemType : types) {
    assert(elemType->info && "cannot canonicalize unknown type");
    if (UnionType *unionType = llvh::dyn_cast<UnionType>(elemType->info)) {
      assert(
          unionType->getNumNonLoopingTypes() >= 0 &&
          "canonicalizeTypes children must have nested unions collapsed "
          "prior to call");
      for (Type *nestedElem : unionType->getNonLoopingTypes()) {
        canonicalTypes.push_back(nestedElem);
      }
      for (Type *nestedElem : unionType->getLoopingTypes()) {
        loopingTypes.push_back(nestedElem);
      }
    } else if (elemType->isLooping) {
      loopingTypes.push_back(elemType);
    } else {
      canonicalTypes.push_back(elemType);
    }
  }

  sortAndUniqueNonLoopingTypes(canonicalTypes, booleanType);
  uniqueLoopingTypesSlow(loopingTypes);
}

void UnionType::sortAndUniqueNonLoopingTypes(
    llvh::SmallVectorImpl<Type *> &types,
    Type *booleanType) {
  auto less = [](Type *a, Type *b) { return a->info->compare(b->info) < 0; };
  // Sort for predictable order. Literal kinds are the last TypeKinds, so a
  // literal arm is always visited after everything that can absorb it.
  std::sort(types.begin(), types.end(), less);

  // Mark for removal the identical arms (which are adjacent after sorting)
  // and the literal arms absorbed by an earlier arm.
  bool seenKind[kNumTypeKinds] = {};
  Type *prev = nullptr;
  for (size_t i = 0, e = types.size(); i < e; ++i) {
    Type *t = types[i];
    TypeKind kind = t->info->getKind();
    if (prev && prev->info->equals(t->info)) {
      types[i] = nullptr;
      continue;
    }
    // A literal is absorbed by its base type, by "any" and by "mixed".
    if (t->info->isLiteral() &&
        (seenKind[(size_t)widenLiteralKind(kind)] ||
         seenKind[(size_t)TypeKind::Any] ||
         seenKind[(size_t)TypeKind::Mixed])) {
      types[i] = nullptr;
      continue;
    }
    seenKind[(size_t)kind] = true;
    prev = t;
  }

  // Remove the marked arms.
  llvh::erase_if(types, [](Type *type) { return type == nullptr; });

  // `true | false` is `boolean`. Both remain only if nothing absorbed them.
  auto isBoolLiteral = [](Type *t) {
    return llvh::isa<BooleanLiteralType>(t->info);
  };
  if (llvh::count_if(types, isBoolLiteral) == 2) {
    llvh::erase_if(types, isBoolLiteral);
    types.insert(llvh::lower_bound(types, booleanType, less), booleanType);
  }
}

void UnionType::uniqueLoopingTypesSlow(llvh::SmallVectorImpl<Type *> &types) {
  // If any pair of types is equal, set the first to nullptr for easy
  // deletion.
  for (size_t i = 0, e = types.size(); i < e; ++i) {
    for (size_t j = i + 1; j < e; ++j) {
      if (types[i]->info->equals(types[j]->info)) {
        types[i] = nullptr;
        break;
      }
    }
  }

  // Remove the marked duplicates.
  llvh::erase_if(types, [](Type *type) { return type == nullptr; });
}

// Iterate types_ and set the hasAny_ and hasMixed_ flags.
void UnionType::setHasAnyMixed() {
  hasAny_ = false;
  hasMixed_ = false;
  for (Type *t : types_) {
    if (!t->info)
      continue;
    if (llvh::isa<AnyType>(t->info))
      hasAny_ = true;
    if (llvh::isa<MixedType>(t->info))
      hasMixed_ = true;
  }
}

int UnionType::_compareImpl(const UnionType *other, CompareState &state) const {
  assert(numNonLoopingTypes_ >= 0 && "uninitialized union");
  assert(other->numNonLoopingTypes_ >= 0 && "uninitialized union");
  assert(getLoopingTypes().empty() && "recursive unions are not comparable");
  assert(
      other->getLoopingTypes().empty() &&
      "recursive unions are not comparable");
  return lexicographicalComparison(
      getNonLoopingTypes().begin(),
      getNonLoopingTypes().end(),
      other->getNonLoopingTypes().begin(),
      other->getNonLoopingTypes().end(),
      [&state](Type *a, Type *b) { return a->info->compare(b->info, state); });
}

bool UnionType::_equalsImpl(const UnionType *other, CompareState &state) const {
  assert(
      numNonLoopingTypes_ >= 0 && other->numNonLoopingTypes_ >= 0 &&
      "uninitialized unions handled in TypeInfo::equals");

  if (numNonLoopingTypes_ != other->numNonLoopingTypes_)
    return false;

  for (size_t i = 0, e = getNonLoopingTypes().size(); i < e; ++i) {
    if (!getNonLoopingTypes()[i]->info->equals(
            other->getNonLoopingTypes()[i]->info, state))
      return false;
  }

  return listsMatchSlow(getLoopingTypes(), other->getLoopingTypes(), state);
}

unsigned UnionType::_hashImpl() const {
  assert(numNonLoopingTypes_ >= 0 && "types haven't been canonicalized yet");
  return (unsigned)llvh::hash_combine(
      (unsigned)TypeKind::Union, numNonLoopingTypes_);
}

int InferencePlaceholderArrayType::_compareImpl(
    const InferencePlaceholderArrayType *other,
    CompareState &state) const {
  return element_->info->compare(other->element_->info, state);
}

bool InferencePlaceholderArrayType::_equalsImpl(
    const InferencePlaceholderArrayType *other,
    CompareState &state) const {
  return element_->info->equals(other->element_->info, state);
}

unsigned InferencePlaceholderArrayType::_hashImpl() const {
  return (unsigned)llvh::hash_combine(
      (unsigned)TypeKind::InferencePlaceholderArray);
}

int TupleType::_compareImpl(const TupleType *other, CompareState &state) const {
  return lexicographicalComparison(
      types_.begin(),
      types_.end(),
      other->types_.begin(),
      other->types_.end(),
      [&state](const Type *ta, const Type *tb) {
        return ta->info->compare(tb->info, state);
      });
}

bool TupleType::_equalsImpl(const TupleType *other, CompareState &state) const {
  if (types_.size() != other->types_.size())
    return false;
  for (size_t i = 0, e = types_.size(); i < e; ++i) {
    if (!types_[i]->info->equals(other->types_[i]->info, state))
      return false;
  }
  return true;
}

unsigned TupleType::_hashImpl() const {
  return (unsigned)llvh::hash_combine((unsigned)TypeKind::Tuple, types_.size());
}

int StringLiteralType::_compareImpl(
    const StringLiteralType *other,
    CompareState &state) const {
  return value_->str().compare(other->value_->str());
}

bool StringLiteralType::_equalsImpl(
    const StringLiteralType *other,
    CompareState &state) const {
  return value_ == other->value_;
}

unsigned StringLiteralType::_hashImpl() const {
  return (unsigned)llvh::hash_combine(
      (unsigned)TypeKind::StringLiteral, (uintptr_t)value_);
}

int NumberLiteralType::_compareImpl(
    const NumberLiteralType *other,
    CompareState &state) const {
  // Must be a total order. Folding (e.g. 0/0) can produce a NaN literal, and
  // the naive comparison returns 0 when either side is NaN (both '<' and '>'
  // are false), so handle NaN explicitly to keep the order total.
  double a = value_, b = other->value_;
  bool aNaN = std::isnan(a), bNaN = std::isnan(b);
  if (aNaN || bNaN)
    return (int)bNaN - (int)aNaN; // NaN sorts first; NaN vs NaN -> 0.
  return a < b ? -1 : a > b ? 1 : 0; // +0/-0 -> 0, matching equals.
}

bool NumberLiteralType::_equalsImpl(
    const NumberLiteralType *other,
    CompareState &state) const {
  if (std::isnan(value_) && std::isnan(other->value_))
    return true;
  return value_ == other->value_;
}

unsigned NumberLiteralType::_hashImpl() const {
  uint64_t bits;
  if (std::isnan(value_)) {
    bits = llvh::DoubleToBits(std::numeric_limits<double>::quiet_NaN());
  } else if (value_ == 0) {
    bits = 0;
  } else {
    bits = llvh::DoubleToBits(value_);
  }
  return (unsigned)llvh::hash_combine((uint64_t)TypeKind::NumberLiteral, bits);
}

int BooleanLiteralType::_compareImpl(
    const BooleanLiteralType *other,
    CompareState &state) const {
  return (int)value_ - (int)other->value_;
}

bool BooleanLiteralType::_equalsImpl(
    const BooleanLiteralType *other,
    CompareState &state) const {
  return value_ == other->value_;
}

unsigned BooleanLiteralType::_hashImpl() const {
  return (unsigned)llvh::hash_combine(
      (unsigned)TypeKind::BooleanLiteral, (unsigned)value_);
}

hermes::OptValue<size_t> ExactObjectType::findField(Identifier id) const {
  auto it = fieldNameMap_.find(id);
  if (it == fieldNameMap_.end())
    return llvh::None;
  return it->second;
}

int ExactObjectType::_compareImpl(
    const ExactObjectType *other,
    CompareState &state) const {
  if (int tmp = lexicographicalComparison(
          fields_.begin(),
          fields_.end(),
          other->fields_.begin(),
          other->fields_.end(),
          [&state](const Field &ta, const Field &tb) {
            if (int tmp = ta.name.str().compare(tb.name.str()))
              return tmp;
            if (ta.variance != tb.variance)
              return ta.variance < tb.variance ? -1 : 1;
            if (int tmp = ta.type->info->compare(tb.type->info, state))
              return tmp;
            return 0;
          }))
    return tmp;
  // Compare the optional indexers. Absence sorts before presence.
  if (indexer_.hasValue() != other->indexer_.hasValue())
    return indexer_.hasValue() ? 1 : -1;
  if (indexer_.hasValue()) {
    const Indexer &a = *indexer_;
    const Indexer &b = *other->indexer_;
    if (int tmp = a.keyType->info->compare(b.keyType->info, state))
      return tmp;
    if (a.variance != b.variance)
      return a.variance < b.variance ? -1 : 1;
    if (int tmp = a.valueType->info->compare(b.valueType->info, state))
      return tmp;
  }
  return 0;
}

bool ExactObjectType::_equalsImpl(
    const ExactObjectType *other,
    CompareState &state) const {
  if (fields_.size() != other->fields_.size())
    return false;
  for (size_t i = 0, e = fields_.size(); i < e; ++i) {
    if (fields_[i].name != other->fields_[i].name)
      return {};
    if (fields_[i].variance != other->fields_[i].variance)
      return false;
    if (!fields_[i].type->info->equals(other->fields_[i].type->info, state))
      return false;
  }
  if (indexer_.hasValue() != other->indexer_.hasValue())
    return false;
  if (indexer_.hasValue()) {
    const Indexer &a = *indexer_;
    const Indexer &b = *other->indexer_;
    if (a.variance != b.variance)
      return false;
    if (!a.keyType->info->equals(b.keyType->info, state))
      return false;
    if (!a.valueType->info->equals(b.valueType->info, state))
      return false;
  }
  return true;
}

unsigned ExactObjectType::_hashImpl() const {
  return (unsigned)llvh::hash_combine(
      (unsigned)TypeKind::ExactObject,
      fields_.size(),
      indexer_.hasValue(),
      indexer_.hasValue() ? (unsigned)indexer_->variance : 0u);
}

/// Compare two instances of the same TypeKind.
int TypedFunctionType::_compareImpl(
    const TypedFunctionType *other,
    CompareState &state) const {
  if (auto tmp = cmpHelperBool(isAsync(), other->isAsync()))
    return tmp;
  if (auto tmp = cmpHelperBool(isGenerator(), other->isGenerator()))
    return tmp;
  if (auto tmp = cmpHelper(thisParam_, other->thisParam_, state))
    return tmp;
  if (auto tmp = lexicographicalComparison(
          params_.begin(),
          params_.end(),
          other->params_.begin(),
          other->params_.end(),
          [&state](const Param &pa, const Param &pb) {
            if (auto t = cmpHelperBool(pa.optional, pb.optional))
              return t;
            if (auto t = cmpHelperBool(pa.rest, pb.rest))
              return t;
            return pa.type->info->compare(pb.type->info, state);
          })) {
    return tmp;
  }
  return cmpHelper(return_, other->return_, state);
}

/// Compare two instances of the same TypeKind.
bool TypedFunctionType::_equalsImpl(
    const TypedFunctionType *other,
    CompareState &state) const {
  if (cmpHelperBool(isAsync(), other->isAsync()))
    return false;
  if (cmpHelperBool(isGenerator(), other->isGenerator()))
    return false;
  if (!eqHelper(thisParam_, other->thisParam_, state))
    return false;
  if (params_.size() != other->params_.size())
    return false;
  for (size_t i = 0, e = params_.size(); i < e; ++i) {
    if (params_[i].optional != other->params_[i].optional)
      return false;
    if (params_[i].rest != other->params_[i].rest)
      return false;
    if (!params_[i].type->info->equals(other->params_[i].type->info, state))
      return false;
  }
  if (!return_->info->equals(other->return_->info, state))
    return false;
  return true;
}

/// Calculate the type-specific hash.
unsigned TypedFunctionType::_hashImpl() const {
  unsigned h = (unsigned)llvh::hash_combine(
      (unsigned)TypeKind::TypedFunction,
      isAsync(),
      isGenerator(),
      thisParam_ != nullptr,
      params_.size());
  for (const auto &p : params_) {
    h = llvh::hash_combine(h, p.optional, p.rest);
  }
  return h;
}

int NativeFunctionType::_compareImpl(
    const NativeFunctionType *other,
    CompareState &) const {
  // If the native signatures are different, there is no point comparing the
  // JS signatures. If the native signatures are the same, then by definition
  // the JS ones must also be the same.
  return signature_->compare(other->signature_);
}

bool NativeFunctionType::_equalsImpl(
    const NativeFunctionType *other,
    CompareState &) const {
  // If the native signatures are different, there is no point comparing the
  // JS signatures. If the native signatures are the same, then by definition
  // the JS ones must also be the same.
  return signature_ == other->signature_;
}

unsigned NativeFunctionType::_hashImpl() const {
  return (unsigned)llvh::hash_combine(
      (unsigned)TypeKind::NativeFunction, signature_->hash());
}

/// Compare two instances of the same TypeKind.
int UntypedFunctionType::_compareImpl(
    const UntypedFunctionType *other,
    CompareState &state) const {
  if (auto tmp = cmpHelperBool(isAsync(), other->isAsync()))
    return tmp;
  if (auto tmp = cmpHelperBool(isGenerator(), other->isGenerator()))
    return tmp;
  return 0;
}

/// Compare two instances of the same TypeKind.
bool UntypedFunctionType::_equalsImpl(
    const UntypedFunctionType *other,
    CompareState &state) const {
  if (cmpHelperBool(isAsync(), other->isAsync()))
    return false;
  if (cmpHelperBool(isGenerator(), other->isGenerator()))
    return false;
  return true;
}

/// Calculate the type-specific hash.
unsigned UntypedFunctionType::_hashImpl() const {
  return (unsigned)llvh::hash_combine(
      (unsigned)TypeKind::UntypedFunction, isAsync(), isGenerator());
}

unsigned TypeWithId::_hashImpl() const {
  return (unsigned)llvh::hash_combine((unsigned)getKind(), getId());
}

ClassType::ClassType(
    size_t id,
    Identifier className,
    llvh::ArrayRef<Type *> typeArgs,
    sema::Decl *genericClassDecl)
    : TypeWithId(TypeKind::Class, id),
      className_(className),
      genericClassDecl_(genericClassDecl) {
  typeArgs_.append(typeArgs.begin(), typeArgs.end());
}

void ClassType::init(
    llvh::ArrayRef<Field> fields,
    Type *constructorType,
    Type *homeObjectType,
    Type *staticObjectType,
    Type *superClass,
    size_t numLayoutSlots,
    Type *classConsType) {
  fields_.reserve(fields.size());
  fields_.append(fields.begin(), fields.end());

  constructorType_ = constructorType;
  homeObjectType_ = homeObjectType;
  staticObjectType_ = staticObjectType;
  superClass_ = superClass;
  numLayoutSlots_ = numLayoutSlots;
  classConsType_ = classConsType;

  if (superClass) {
    auto *superClassInfo = llvh::cast_or_null<ClassType>(superClass_->info);
    // Copy the lookup table down from the superClass to avoid having to climb
    // the whole chain every time we want to typecheck a property access.
    fieldNameMap_.reserve(
        fields.size() + superClassInfo->getFieldNameMap().size());
    for (const auto &it : superClassInfo->getFieldNameMap()) {
      fieldNameMap_[it.first] = it.second;
    }
  } else {
    fieldNameMap_.reserve(fields.size());
  }

  // Override the fields which have been overridden.
  size_t index = 0;
  for (const auto &f : this->fields_) {
    if (f.isPrivate) {
      privateFieldNameMap_[f.name] = index++;
    } else {
      fieldNameMap_[f.name] = FieldLookupEntry{this, index++};
    }
  }

  markAsInitialized();
}

hermes::OptValue<ClassType::FieldLookupEntry> ClassType::findPublicField(
    Identifier id) const {
  auto it = fieldNameMap_.find(id);
  if (it == fieldNameMap_.end())
    return llvh::None;
  return it->second;
}

hermes::OptValue<ClassType::FieldLookupEntry> ClassType::findPrivateField(
    Identifier id) {
  auto it = privateFieldNameMap_.find(id);
  if (it == privateFieldNameMap_.end())
    return llvh::None;
  return FieldLookupEntry{this, it->second};
}

FlowContext::FlowContext() = default;
FlowContext::~FlowContext() = default;

Type *FlowContext::findDeclType(const sema::Decl *decl) const {
  assert(decl && "nullptr sema::Decl");
  auto it = declTypes_.find(decl);
  return it != declTypes_.end() ? it->second : nullptr;
}

Type *FlowContext::getDeclType(const sema::Decl *decl) const {
  Type *res = findDeclType(decl);
  assert(res && "unresolved declaration");
  return res;
}

void FlowContext::setNodeType(const ESTree::Node *node, Type *type) {
  if (type)
    nodeTypes_[node] = type;
}

Type *FlowContext::findNodeType(const ESTree::Node *node) const {
  auto it = nodeTypes_.find(node);
  return it != nodeTypes_.end() ? it->second : nullptr;
}

Type *FlowContext::getNodeTypeOrAny(const ESTree::Node *node) const {
  auto it = nodeTypes_.find(node);
  return it != nodeTypes_.end() ? it->second : getAny();
}

Type *FlowContext::getSingletonType(TypeKind kind) const {
  switch (kind) {
#define _HERMES_SEMA_FLOW_DEFKIND(name) \
  case TypeKind::name:                  \
    return &name##Instance_;
    _HERMES_SEMA_FLOW_SINGLETONS
#undef _HERMES_SEMA_FLOW_DEFKIND
    default:
      llvm_unreachable("invalid singleton TypeKind");
  }
}

TypeInfo *FlowContext::maybeCreateUnion(
    llvh::ArrayRef<Type *> types,
    UnionExcludes excludes) {
  assert(!types.empty() && "types must not be empty");
  llvh::SmallVector<Type *, 4> canonicalTypes{};
  llvh::SmallVector<Type *, 4> recursiveTypes{};

  UnionType::canonicalizeTypes(
      types, canonicalTypes, recursiveTypes, getBoolean());

  if (excludes.excludeVoid || excludes.excludeNull) {
    // Only look at the first two elements and get the new ending.
    auto searchEnd = std::min(canonicalTypes.end(), canonicalTypes.begin() + 2);
    auto removedEnd =
        std::remove_if(canonicalTypes.begin(), searchEnd, [&excludes](Type *t) {
          return (llvh::isa<VoidType>(t->info) && excludes.excludeVoid) ||
              (llvh::isa<NullType>(t->info) && excludes.excludeNull);
        });
    // Erase the removed elements from the container.
    canonicalTypes.erase(removedEnd, searchEnd);
  }

  // The types collapsed to a single type, so return that.
  if (canonicalTypes.size() == 1 && recursiveTypes.empty())
    return canonicalTypes.front()->info;
  if (canonicalTypes.empty() && recursiveTypes.size() == 1)
    return recursiveTypes.front()->info;

  return createCanonicalizedUnion(
      std::move(canonicalTypes), std::move(recursiveTypes));
}

UnionType *FlowContext::createPopulatedNullable(Type *type) {
  llvh::SmallVector<Type *, 4> canonicalTypes{};
  llvh::SmallVector<Type *, 4> recursiveTypes{};
  UnionType::canonicalizeTypes(
      {getVoid(), getNull(), type},
      canonicalTypes,
      recursiveTypes,
      getBoolean());

  // We know that there are at least 2 types, so it will be a union.
  return createCanonicalizedUnion(
      std::move(canonicalTypes), std::move(recursiveTypes));
}

} // namespace flow
} // namespace hermes
