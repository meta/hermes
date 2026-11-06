/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_FRONTENDDEFS_TYPEOF_H
#define HERMES_FRONTENDDEFS_TYPEOF_H

#include "hermes/FrontEndDefs/TypeOfIsTypes.h"
#include "llvh/ADT/FoldingSet.h"
#include "llvh/Support/MathExtras.h"
#include "llvh/Support/raw_ostream.h"

#include <cstdint>

namespace hermes {

/// Bitfield for the flags in the TypeOfIs IR and bytecode instructions.
/// Guaranteed to fit in a uint16_t.
///
/// NOTE: "Object" does not match "null" or functions.
///
/// WARNING: Must update the HBC version if this is changed.
struct TypeOfIsTypes {
  /// Construct an empty TypeOfIsTypes.
  explicit TypeOfIsTypes() : raw_(0) {}
  /// Construct a TypeOfIsTypes from a raw value.
  /// \pre the \p raw value must have been generated from getRaw().
  /// Will assert on invalid \p raw.
  explicit TypeOfIsTypes(uint16_t raw) : raw_(raw) {
    assert((raw >> (int)Types::_count) == 0 && "invalid raw");
  }

  /// \return a 16-bit representation that can be round-tripped to
  /// TypeOfIsTypes.
  uint16_t getRaw() const {
    return raw_;
  }

  /// \return a TypeOfIsTypes that matches the inverse of this one.
  /// NOTE: We use this instead of manually inverting the bits to futureproof
  /// against possible changes to the representation.
  TypeOfIsTypes invert() const {
    return TypeOfIsTypes(
        (~raw_) & llvh::maskTrailingOnes<uint16_t>((int)Types::_count));
  }

#define HERMES_TYPEOF_IS_TYPES_TYPE(name)                       \
  /* \return a new TypeOfIsTypes with the bit set to \p val. */ \
  TypeOfIsTypes with##name(bool val) const {                    \
    uint16_t raw = raw_;                                        \
    if (val)                                                    \
      raw |= (1 << (int)Types::name);                           \
    else                                                        \
      raw &= ~(1 << (int)Types::name);                          \
    return TypeOfIsTypes(raw);                                  \
  }                                                             \
  /* \return whether the bit is set. */                         \
  bool has##name() const {                                      \
    return (raw_ & (1 << (int)Types::name)) != 0;               \
  }
  HERMES_TYPEOF_IS_TYPES
#undef HERMES_TYPEOF_IS_TYPES_TYPE

  /// \return the number of types that this TypeOfIsTypes represents.
  size_t count() const {
    return llvh::countPopulation(raw_);
  }

  /// Allow TypeOfIsTypes to be used as a llvh::FoldingSet.
  void Profile(llvh::FoldingSetNodeID &ID) const {
    ID.AddInteger(raw_);
  }

  bool operator==(const TypeOfIsTypes &other) const {
    return raw_ == other.raw_;
  }
  bool operator!=(const TypeOfIsTypes &other) const {
    return !(*this == other);
  }

 private:
  /// The raw representation of the TypeOfIsTypes.
  /// Bit \c i represents "true" for the type at Types::i.
  uint16_t raw_;

  enum class Types {
#define HERMES_TYPEOF_IS_TYPES_TYPE(name) name,
    HERMES_TYPEOF_IS_TYPES
#undef HERMES_TYPEOF_IS_TYPES_TYPE
        _count
  };
};

static_assert(
    sizeof(TypeOfIsTypes) == sizeof(uint16_t),
    "TypeOfIsTypes should be 16 bits");

/// Simple printer for TypeOfIsTypes.
/// Placed here to avoid having a cpp file just for this function.
/// It's only used for debugging or IR dumps.
inline llvh::raw_ostream &operator<<(
    llvh::raw_ostream &os,
    const TypeOfIsTypes &types) {
  bool comma = false;

#define HERMES_TYPEOF_IS_TYPES_TYPE(name) \
  if (types.has##name()) {                \
    if (comma)                            \
      os << ",";                          \
    os << #name;                          \
    comma = true;                         \
  }
  HERMES_TYPEOF_IS_TYPES
#undef HERMES_TYPEOF_IS_TYPES_TYPE

  return os;
}

} // namespace hermes

#endif
