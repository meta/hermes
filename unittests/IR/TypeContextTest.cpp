/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/IR/TypeContext.h"

#include "hermes/IR/IR.h"

#include "llvh/Support/raw_ostream.h"

#include "gtest/gtest.h"

namespace hermes {

/// Test fixture for TypeContext unit tests. Provides an asType()
/// helper that constructs a Type from a raw well-known ID. This is
/// needed for tests of refined kinds (UInt31, Int32, Uint32, Bits32)
/// that have no public Type::createXxx() factory in Phase 1. The
/// fixture is declared friend of Type in IR.h, so the call to the
/// private Type(uint32_t) constructor is legal here.
class TypeContextTest : public ::testing::Test {
 protected:
  static Type asType(uint32_t id) {
    return Type(id);
  }
};

} // namespace hermes

using namespace hermes;
using namespace hermes::type_internal;

namespace {

TEST_F(TypeContextTest, LeafKinds) {
  TypeContext ctx;

  EXPECT_EQ(ctx.getKind(Type::createNoType()), TypeKind::NoType);
  EXPECT_EQ(ctx.getKind(Type::createEmpty()), TypeKind::Empty);
  EXPECT_EQ(ctx.getKind(Type::createUninit()), TypeKind::Uninit);
  EXPECT_EQ(ctx.getKind(Type::createUndefined()), TypeKind::Undefined);
  EXPECT_EQ(ctx.getKind(Type::createNull()), TypeKind::Null);
  EXPECT_EQ(ctx.getKind(Type::createBoolean()), TypeKind::Boolean);
  EXPECT_EQ(ctx.getKind(Type::createString()), TypeKind::String);
  EXPECT_EQ(ctx.getKind(Type::createNumber()), TypeKind::Number);
  EXPECT_EQ(ctx.getKind(Type::createBigInt()), TypeKind::BigInt);
  EXPECT_EQ(ctx.getKind(Type::createSymbol()), TypeKind::Symbol);
  EXPECT_EQ(ctx.getKind(Type::createEnvironment()), TypeKind::Environment);
  EXPECT_EQ(ctx.getKind(Type::createPrivateName()), TypeKind::PrivateName);
  EXPECT_EQ(ctx.getKind(Type::createFunctionCode()), TypeKind::FunctionCode);
  EXPECT_EQ(ctx.getKind(Type::createObject()), TypeKind::Object);
  EXPECT_EQ(ctx.getKind(asType(kBits32Id)), TypeKind::Bits32);
}

TEST_F(TypeContextTest, AnyTypeIsUnion) {
  TypeContext ctx;

  EXPECT_EQ(ctx.getKind(Type::createAnyType()), TypeKind::Union);
  auto arms = ctx.getUnionArms(Type::createAnyType());
  // AnyType = Undefined | Null | Boolean | String | Number | BigInt | Symbol |
  // Object.
  EXPECT_EQ(arms.size(), 8u);

  // Verify all expected arms are present.
  auto contains = [&](Type t) {
    for (auto arm : arms)
      if (arm == t)
        return true;
    return false;
  };
  EXPECT_TRUE(contains(Type::createUndefined()));
  EXPECT_TRUE(contains(Type::createNull()));
  EXPECT_TRUE(contains(Type::createBoolean()));
  EXPECT_TRUE(contains(Type::createString()));
  EXPECT_TRUE(contains(Type::createNumber()));
  EXPECT_TRUE(contains(Type::createBigInt()));
  EXPECT_TRUE(contains(Type::createSymbol()));
  EXPECT_TRUE(contains(Type::createObject()));

  // Should NOT contain internal types.
  EXPECT_FALSE(contains(Type::createEmpty()));
  EXPECT_FALSE(contains(Type::createUninit()));
  EXPECT_FALSE(contains(Type::createEnvironment()));
  EXPECT_FALSE(contains(Type::createPrivateName()));
  EXPECT_FALSE(contains(Type::createFunctionCode()));
}

TEST_F(TypeContextTest, NumericIsUnion) {
  TypeContext ctx;

  EXPECT_EQ(ctx.getKind(Type::createNumeric()), TypeKind::Union);
  auto arms = ctx.getUnionArms(Type::createNumeric());
  EXPECT_EQ(arms.size(), 2u);

  bool hasNumber = false, hasBigInt = false;
  for (auto arm : arms) {
    if (arm == Type::createNumber())
      hasNumber = true;
    if (arm == Type::createBigInt())
      hasBigInt = true;
  }
  EXPECT_TRUE(hasNumber);
  EXPECT_TRUE(hasBigInt);
}

TEST_F(TypeContextTest, AnyEmptyUninitIsUnion) {
  TypeContext ctx;

  EXPECT_EQ(ctx.getKind(Type::createAnyEmptyUninit()), TypeKind::Union);
  auto arms = ctx.getUnionArms(Type::createAnyEmptyUninit());
  // All AnyType arms + Empty + Uninit = 10.
  EXPECT_EQ(arms.size(), 10u);

  auto contains = [&](Type t) {
    for (auto arm : arms)
      if (arm == t)
        return true;
    return false;
  };
  EXPECT_TRUE(contains(Type::createEmpty()));
  EXPECT_TRUE(contains(Type::createUninit()));
  EXPECT_TRUE(contains(Type::createUndefined()));
  EXPECT_TRUE(contains(Type::createNull()));
  EXPECT_TRUE(contains(Type::createBoolean()));
  EXPECT_TRUE(contains(Type::createString()));
  EXPECT_TRUE(contains(Type::createNumber()));
  EXPECT_TRUE(contains(Type::createBigInt()));
  EXPECT_TRUE(contains(Type::createSymbol()));
  EXPECT_TRUE(contains(Type::createObject()));
}

TEST_F(TypeContextTest, NullOrUndefIsUnion) {
  TypeContext ctx;

  EXPECT_EQ(ctx.getKind(Type::createNullOrUndef()), TypeKind::Union);
  auto arms = ctx.getUnionArms(Type::createNullOrUndef());
  EXPECT_EQ(arms.size(), 2u);

  bool hasUndefined = false, hasNull = false;
  for (auto arm : arms) {
    if (arm == Type::createUndefined())
      hasUndefined = true;
    if (arm == Type::createNull())
      hasNull = true;
  }
  EXPECT_TRUE(hasUndefined);
  EXPECT_TRUE(hasNull);
}

TEST_F(TypeContextTest, ReservedSlots) {
  TypeContext ctx;
  // Padding slots between the last well-known union and kFirstDynamicId
  // should be NoType placeholders.
  for (uint32_t i = _kLastUnionId + 1; i < kFirstDynamicId; ++i) {
    EXPECT_EQ(ctx.getKind(asType(i)), TypeKind::NoType)
        << "Reserved slot " << i << " should be NoType";
  }
}

TEST_F(TypeContextTest, CanBeNumberLeaf) {
  TypeContext ctx;

  EXPECT_TRUE(ctx.canBeNumber(Type::createNumber()));
  EXPECT_FALSE(ctx.canBeNumber(Type::createString()));
  EXPECT_FALSE(ctx.canBeNumber(Type::createObject()));
  EXPECT_FALSE(ctx.canBeNumber(Type::createNoType()));
  EXPECT_FALSE(ctx.canBeNumber(Type::createBoolean()));
}

TEST_F(TypeContextTest, CanBeNumberUnion) {
  TypeContext ctx;

  // AnyType is a union containing Number.
  EXPECT_TRUE(ctx.canBeNumber(Type::createAnyType()));
  // Numeric = Number | BigInt.
  EXPECT_TRUE(ctx.canBeNumber(Type::createNumeric()));
  // AnyEmptyUninit contains Number.
  EXPECT_TRUE(ctx.canBeNumber(Type::createAnyEmptyUninit()));
  // NullOrUndef does not contain Number.
  EXPECT_FALSE(ctx.canBeNumber(Type::createNullOrUndef()));
}

TEST_F(TypeContextTest, CanBeOtherKinds) {
  TypeContext ctx;

  EXPECT_TRUE(ctx.canBeString(Type::createString()));
  EXPECT_FALSE(ctx.canBeString(Type::createNumber()));
  EXPECT_TRUE(ctx.canBeString(Type::createAnyType()));

  EXPECT_TRUE(ctx.canBeObject(Type::createObject()));
  EXPECT_FALSE(ctx.canBeObject(Type::createNumber()));
  EXPECT_TRUE(ctx.canBeObject(Type::createAnyType()));

  EXPECT_TRUE(ctx.canBeNull(Type::createNull()));
  EXPECT_FALSE(ctx.canBeNull(Type::createNumber()));
  EXPECT_TRUE(ctx.canBeNull(Type::createAnyType()));
  EXPECT_TRUE(ctx.canBeNull(Type::createNullOrUndef()));

  EXPECT_TRUE(ctx.canBeUndefined(Type::createUndefined()));
  EXPECT_FALSE(ctx.canBeUndefined(Type::createNumber()));
  EXPECT_TRUE(ctx.canBeUndefined(Type::createAnyType()));
  EXPECT_TRUE(ctx.canBeUndefined(Type::createNullOrUndef()));

  EXPECT_TRUE(ctx.canBeEmpty(Type::createEmpty()));
  EXPECT_FALSE(ctx.canBeEmpty(Type::createNumber()));
  EXPECT_FALSE(ctx.canBeEmpty(Type::createAnyType()));
  EXPECT_TRUE(ctx.canBeEmpty(Type::createAnyEmptyUninit()));

  EXPECT_TRUE(ctx.canBeUninit(Type::createUninit()));
  EXPECT_FALSE(ctx.canBeUninit(Type::createNumber()));
  EXPECT_FALSE(ctx.canBeUninit(Type::createAnyType()));
  EXPECT_TRUE(ctx.canBeUninit(Type::createAnyEmptyUninit()));

  EXPECT_TRUE(ctx.canBeBigInt(Type::createBigInt()));
  EXPECT_FALSE(ctx.canBeBigInt(Type::createString()));
  EXPECT_TRUE(ctx.canBeBigInt(Type::createNumeric()));
  EXPECT_TRUE(ctx.canBeBigInt(Type::createAnyType()));

  EXPECT_TRUE(ctx.canBeBoolean(Type::createBoolean()));
  EXPECT_FALSE(ctx.canBeBoolean(Type::createString()));
  EXPECT_TRUE(ctx.canBeBoolean(Type::createAnyType()));

  EXPECT_TRUE(ctx.canBeSymbol(Type::createSymbol()));
  EXPECT_FALSE(ctx.canBeSymbol(Type::createString()));
  EXPECT_TRUE(ctx.canBeSymbol(Type::createAnyType()));
}

TEST_F(TypeContextTest, IsNoType) {
  TypeContext ctx;

  EXPECT_TRUE(ctx.isNoType(Type::createNoType()));
  EXPECT_FALSE(ctx.isNoType(Type::createNumber()));
  EXPECT_FALSE(ctx.isNoType(Type::createAnyType()));
}

TEST_F(TypeContextTest, IsPrimitive) {
  TypeContext ctx;

  // Primitive kinds: Number, String, BigInt, Null, Undefined, Boolean, Symbol.
  EXPECT_TRUE(ctx.isPrimitive(Type::createNumber()));
  EXPECT_TRUE(ctx.isPrimitive(Type::createString()));
  EXPECT_TRUE(ctx.isPrimitive(Type::createBigInt()));
  EXPECT_TRUE(ctx.isPrimitive(Type::createNull()));
  EXPECT_TRUE(ctx.isPrimitive(Type::createUndefined()));
  EXPECT_TRUE(ctx.isPrimitive(Type::createBoolean()));
  EXPECT_TRUE(ctx.isPrimitive(Type::createSymbol()));

  // Object is NOT primitive.
  EXPECT_FALSE(ctx.isPrimitive(Type::createObject()));
  // Internal types are not primitive.
  EXPECT_FALSE(ctx.isPrimitive(Type::createEmpty()));
  EXPECT_FALSE(ctx.isPrimitive(Type::createUninit()));
  EXPECT_FALSE(ctx.isPrimitive(Type::createEnvironment()));
  EXPECT_FALSE(ctx.isPrimitive(asType(kBits32Id)));
  // NoType is not primitive.
  EXPECT_FALSE(ctx.isPrimitive(Type::createNoType()));

  // AnyType contains Object, so it's not all-primitive.
  EXPECT_FALSE(ctx.isPrimitive(Type::createAnyType()));
  // Numeric = Number | BigInt — both primitive.
  EXPECT_TRUE(ctx.isPrimitive(Type::createNumeric()));
  // NullOrUndef = Null | Undefined — both primitive.
  EXPECT_TRUE(ctx.isPrimitive(Type::createNullOrUndef()));
}

TEST_F(TypeContextTest, CanBePrimitive) {
  TypeContext ctx;

  EXPECT_TRUE(ctx.canBePrimitive(Type::createNumber()));
  EXPECT_FALSE(ctx.canBePrimitive(Type::createObject()));
  EXPECT_FALSE(ctx.canBePrimitive(Type::createNoType()));
  // AnyType contains primitives (and Object).
  EXPECT_TRUE(ctx.canBePrimitive(Type::createAnyType()));
  // AnyEmptyUninit contains primitives.
  EXPECT_TRUE(ctx.canBePrimitive(Type::createAnyEmptyUninit()));
}

TEST_F(TypeContextTest, IsNonPtr) {
  TypeContext ctx;

  // NonPtr kinds: Number, Boolean, Null, Undefined.
  EXPECT_TRUE(ctx.isNonPtr(Type::createNumber()));
  EXPECT_TRUE(ctx.isNonPtr(Type::createBoolean()));
  EXPECT_TRUE(ctx.isNonPtr(Type::createNull()));
  EXPECT_TRUE(ctx.isNonPtr(Type::createUndefined()));

  // String is a pointer type.
  EXPECT_FALSE(ctx.isNonPtr(Type::createString()));
  // Object is a pointer type.
  EXPECT_FALSE(ctx.isNonPtr(Type::createObject()));
  // NoType is not nonPtr.
  EXPECT_FALSE(ctx.isNonPtr(Type::createNoType()));

  // NullOrUndef = Null | Undefined — both non-ptr.
  EXPECT_TRUE(ctx.isNonPtr(Type::createNullOrUndef()));
  // AnyType contains String/Object, so not all non-ptr.
  EXPECT_FALSE(ctx.isNonPtr(Type::createAnyType()));
  // Numeric contains BigInt, which is a pointer type.
  EXPECT_FALSE(ctx.isNonPtr(Type::createNumeric()));
}

TEST_F(TypeContextTest, IsSubsetOf) {
  TypeContext ctx;

  // Reflexive.
  EXPECT_TRUE(ctx.isSubsetOf(Type::createNumber(), Type::createNumber()));
  // NoType is subset of everything.
  EXPECT_TRUE(ctx.isSubsetOf(Type::createNoType(), Type::createNumber()));
  EXPECT_TRUE(ctx.isSubsetOf(Type::createNoType(), Type::createAnyType()));
  EXPECT_TRUE(ctx.isSubsetOf(Type::createNoType(), Type::createNoType()));
  // Nothing (except NoType) is subset of NoType.
  EXPECT_FALSE(ctx.isSubsetOf(Type::createNumber(), Type::createNoType()));
  // Leaf is subset of union containing it.
  EXPECT_TRUE(ctx.isSubsetOf(Type::createNumber(), Type::createAnyType()));
  EXPECT_TRUE(ctx.isSubsetOf(Type::createNumber(), Type::createNumeric()));
  EXPECT_TRUE(ctx.isSubsetOf(Type::createBigInt(), Type::createNumeric()));
  // Union is not subset of its member.
  EXPECT_FALSE(ctx.isSubsetOf(Type::createAnyType(), Type::createNumber()));
  EXPECT_FALSE(ctx.isSubsetOf(Type::createNumeric(), Type::createNumber()));
  // Sub-union is subset of super-union.
  EXPECT_TRUE(ctx.isSubsetOf(Type::createNumeric(), Type::createAnyType()));
  EXPECT_TRUE(ctx.isSubsetOf(Type::createNullOrUndef(), Type::createAnyType()));
  // Disjoint types.
  EXPECT_FALSE(ctx.isSubsetOf(Type::createNumber(), Type::createString()));
  EXPECT_FALSE(ctx.isSubsetOf(Type::createString(), Type::createNumber()));
}

TEST_F(TypeContextTest, AreDisjoint) {
  TypeContext ctx;

  // Same type is not disjoint with itself.
  EXPECT_FALSE(ctx.areDisjoint(Type::createNumber(), Type::createNumber()));
  // NoType is disjoint from everything (including itself).
  EXPECT_TRUE(ctx.areDisjoint(Type::createNoType(), Type::createNumber()));
  EXPECT_TRUE(ctx.areDisjoint(Type::createNumber(), Type::createNoType()));
  EXPECT_TRUE(ctx.areDisjoint(Type::createNoType(), Type::createNoType()));
  // Different leaf types are disjoint.
  EXPECT_TRUE(ctx.areDisjoint(Type::createNumber(), Type::createString()));
  EXPECT_TRUE(ctx.areDisjoint(Type::createBoolean(), Type::createObject()));
  // Union overlaps with its members.
  EXPECT_FALSE(ctx.areDisjoint(Type::createNumber(), Type::createNumeric()));
  EXPECT_FALSE(ctx.areDisjoint(Type::createNumber(), Type::createAnyType()));
  // Disjoint unions.
  EXPECT_TRUE(
      ctx.areDisjoint(Type::createNullOrUndef(), Type::createNumeric()));
  // Overlapping unions.
  EXPECT_FALSE(ctx.areDisjoint(Type::createAnyType(), Type::createNumeric()));
}

TEST_F(TypeContextTest, UnionTyIdentity) {
  TypeContext ctx;

  EXPECT_EQ(
      ctx.unionTy(Type::createNumber(), Type::createNumber()),
      Type::createNumber());
  EXPECT_EQ(
      ctx.unionTy(Type::createAnyType(), Type::createAnyType()),
      Type::createAnyType());
}

TEST_F(TypeContextTest, UnionTyNoType) {
  TypeContext ctx;

  EXPECT_EQ(
      ctx.unionTy(Type::createNoType(), Type::createString()),
      Type::createString());
  EXPECT_EQ(
      ctx.unionTy(Type::createString(), Type::createNoType()),
      Type::createString());
  EXPECT_EQ(
      ctx.unionTy(Type::createNoType(), Type::createNoType()),
      Type::createNoType());
}

TEST_F(TypeContextTest, UnionTySubset) {
  TypeContext ctx;

  // Number is subset of AnyType.
  EXPECT_EQ(
      ctx.unionTy(Type::createNumber(), Type::createAnyType()),
      Type::createAnyType());
  EXPECT_EQ(
      ctx.unionTy(Type::createAnyType(), Type::createNumber()),
      Type::createAnyType());
  // Numeric is subset of AnyType.
  EXPECT_EQ(
      ctx.unionTy(Type::createNumeric(), Type::createAnyType()),
      Type::createAnyType());
}

TEST_F(TypeContextTest, UnionTyCreatesDynamic) {
  TypeContext ctx;

  Type numStr = ctx.unionTy(Type::createNumber(), Type::createString());
  EXPECT_EQ(ctx.getKind(numStr), TypeKind::Union);
  auto arms = ctx.getUnionArms(numStr);
  EXPECT_EQ(arms.size(), 2u);
  // Arms sorted by ID: String(6), Number(7).
  EXPECT_EQ(arms[0], Type::createString());
  EXPECT_EQ(arms[1], Type::createNumber());
}

TEST_F(TypeContextTest, UnionTyInterning) {
  TypeContext ctx;

  Type a = ctx.unionTy(Type::createNumber(), Type::createString());
  Type b = ctx.unionTy(Type::createNumber(), Type::createString());
  EXPECT_EQ(a, b);

  // Reverse order produces same result.
  Type c = ctx.unionTy(Type::createString(), Type::createNumber());
  EXPECT_EQ(a, c);
}

TEST_F(TypeContextTest, UnionTyReturnsWellKnown) {
  TypeContext ctx;

  // unionTy(Number, BigInt) should return the well-known Numeric.
  EXPECT_EQ(
      ctx.unionTy(Type::createNumber(), Type::createBigInt()),
      Type::createNumeric());
  // unionTy(Null, Undefined) should return well-known NullOrUndef.
  EXPECT_EQ(
      ctx.unionTy(Type::createNull(), Type::createUndefined()),
      Type::createNullOrUndef());
}

TEST_F(TypeContextTest, IntersectTy) {
  TypeContext ctx;

  // Disjoint types.
  EXPECT_EQ(
      ctx.intersectTy(Type::createNumber(), Type::createString()),
      Type::createNoType());
  // Subset: Number intersect AnyType = Number.
  EXPECT_EQ(
      ctx.intersectTy(Type::createNumber(), Type::createAnyType()),
      Type::createNumber());
  EXPECT_EQ(
      ctx.intersectTy(Type::createAnyType(), Type::createNumber()),
      Type::createNumber());
  // Same type.
  EXPECT_EQ(
      ctx.intersectTy(Type::createNumber(), Type::createNumber()),
      Type::createNumber());
  // NoType.
  EXPECT_EQ(
      ctx.intersectTy(Type::createNoType(), Type::createNumber()),
      Type::createNoType());
  EXPECT_EQ(
      ctx.intersectTy(Type::createNumber(), Type::createNoType()),
      Type::createNoType());
  // Union intersect Union: Numeric subset of AnyType.
  EXPECT_EQ(
      ctx.intersectTy(Type::createNumeric(), Type::createAnyType()),
      Type::createNumeric());
  // NullOrUndef intersect Numeric: disjoint → NoType.
  EXPECT_EQ(
      ctx.intersectTy(Type::createNullOrUndef(), Type::createNumeric()),
      Type::createNoType());
}

TEST_F(TypeContextTest, SubtractTy) {
  TypeContext ctx;

  // Subtract member from union: AnyType - Number.
  Type result = ctx.subtractTy(Type::createAnyType(), Type::createNumber());
  EXPECT_EQ(ctx.getKind(result), TypeKind::Union);
  auto arms = ctx.getUnionArms(result);
  EXPECT_EQ(arms.size(), 7u);
  EXPECT_FALSE(ctx.canBeNumber(result));
  EXPECT_TRUE(ctx.canBeString(result));
  EXPECT_TRUE(ctx.canBeObject(result));
  EXPECT_TRUE(ctx.canBeBigInt(result));

  // Subset subtraction → NoType.
  EXPECT_EQ(
      ctx.subtractTy(Type::createNumber(), Type::createAnyType()),
      Type::createNoType());
  // Disjoint subtraction → unchanged.
  EXPECT_EQ(
      ctx.subtractTy(Type::createNumber(), Type::createString()),
      Type::createNumber());
  // NoType cases.
  EXPECT_EQ(
      ctx.subtractTy(Type::createNoType(), Type::createNumber()),
      Type::createNoType());
  EXPECT_EQ(
      ctx.subtractTy(Type::createNumber(), Type::createNoType()),
      Type::createNumber());
  // Subtract union from union: AnyType - NullOrUndef.
  Type r2 = ctx.subtractTy(Type::createAnyType(), Type::createNullOrUndef());
  EXPECT_FALSE(ctx.canBeNull(r2));
  EXPECT_FALSE(ctx.canBeUndefined(r2));
  EXPECT_TRUE(ctx.canBeNumber(r2));
  EXPECT_TRUE(ctx.canBeString(r2));
}

TEST_F(TypeContextTest, UInt31WellKnownId) {
  TypeContext ctx;

  // UInt31 is pre-allocated with its own well-known ID.
  EXPECT_EQ(ctx.getKind(asType(kUInt31Id)), TypeKind::UInt31);
  EXPECT_EQ(ctx.getKind(asType(kInt32Id)), TypeKind::Int32);
  EXPECT_EQ(ctx.getKind(asType(kUint32Id)), TypeKind::Uint32);
}

TEST_F(TypeContextTest, UInt31SubtypeRelationships) {
  TypeContext ctx;

  // UInt31 <: Int32, Uint32, Number.
  EXPECT_TRUE(ctx.isSubsetOf(asType(kUInt31Id), asType(kInt32Id)));
  EXPECT_TRUE(ctx.isSubsetOf(asType(kUInt31Id), asType(kUint32Id)));
  EXPECT_TRUE(ctx.isSubsetOf(asType(kUInt31Id), Type::createNumber()));
  // Not the reverse.
  EXPECT_FALSE(ctx.isSubsetOf(asType(kInt32Id), asType(kUInt31Id)));
  EXPECT_FALSE(ctx.isSubsetOf(asType(kUint32Id), asType(kUInt31Id)));
  EXPECT_FALSE(ctx.isSubsetOf(Type::createNumber(), asType(kUInt31Id)));
  // Int32/Uint32 are not subsets of each other.
  EXPECT_FALSE(ctx.isSubsetOf(asType(kInt32Id), asType(kUint32Id)));
  EXPECT_FALSE(ctx.isSubsetOf(asType(kUint32Id), asType(kInt32Id)));
  // But both are subsets of Number.
  EXPECT_TRUE(ctx.isSubsetOf(asType(kInt32Id), Type::createNumber()));
  EXPECT_TRUE(ctx.isSubsetOf(asType(kUint32Id), Type::createNumber()));
}

TEST_F(TypeContextTest, UInt31Disjointness) {
  TypeContext ctx;

  // UInt31 is not disjoint from its supertypes.
  EXPECT_FALSE(ctx.areDisjoint(asType(kUInt31Id), asType(kInt32Id)));
  EXPECT_FALSE(ctx.areDisjoint(asType(kUInt31Id), asType(kUint32Id)));
  EXPECT_FALSE(ctx.areDisjoint(asType(kUInt31Id), Type::createNumber()));
  // Int32 and Uint32 overlap (via UInt31).
  EXPECT_FALSE(ctx.areDisjoint(asType(kInt32Id), asType(kUint32Id)));
  // UInt31 is disjoint from non-number types.
  EXPECT_TRUE(ctx.areDisjoint(asType(kUInt31Id), Type::createString()));
  EXPECT_TRUE(ctx.areDisjoint(asType(kUInt31Id), Type::createObject()));
  EXPECT_TRUE(ctx.areDisjoint(asType(kUInt31Id), Type::createBoolean()));
}

TEST_F(TypeContextTest, IntersectInt32Uint32) {
  TypeContext ctx;

  // Int32 ∩ Uint32 = UInt31.
  EXPECT_EQ(
      ctx.intersectTy(asType(kInt32Id), asType(kUint32Id)), asType(kUInt31Id));
  EXPECT_EQ(
      ctx.intersectTy(asType(kUint32Id), asType(kInt32Id)), asType(kUInt31Id));
  // Number ∩ Int32 = Int32 (subset).
  EXPECT_EQ(
      ctx.intersectTy(Type::createNumber(), asType(kInt32Id)),
      asType(kInt32Id));
  // Number ∩ Uint32 = Uint32 (subset).
  EXPECT_EQ(
      ctx.intersectTy(Type::createNumber(), asType(kUint32Id)),
      asType(kUint32Id));
  // UInt31 ∩ Int32 = UInt31 (subset).
  EXPECT_EQ(
      ctx.intersectTy(asType(kUInt31Id), asType(kInt32Id)), asType(kUInt31Id));
  // UInt31 ∩ String = NoType (disjoint).
  EXPECT_EQ(
      ctx.intersectTy(asType(kUInt31Id), Type::createString()),
      Type::createNoType());
}

TEST_F(TypeContextTest, IntersectUnionWithLeaf) {
  TypeContext ctx;

  Type numberOrString = ctx.unionTy(Type::createNumber(), Type::createString());
  EXPECT_EQ(
      ctx.intersectTy(numberOrString, asType(kInt32Id)), asType(kInt32Id));
  EXPECT_EQ(
      ctx.intersectTy(asType(kInt32Id), numberOrString), asType(kInt32Id));
}

TEST_F(TypeContextTest, IntersectUnionWithUnionDeduplicatesResults) {
  TypeContext ctx;

  Type intOrUint = ctx.unionTy(asType(kInt32Id), asType(kUint32Id));
  Type uint31OrString = ctx.unionTy(asType(kUInt31Id), Type::createString());
  EXPECT_EQ(ctx.intersectTy(intOrUint, uint31OrString), asType(kUInt31Id));
  EXPECT_EQ(ctx.intersectTy(uint31OrString, intOrUint), asType(kUInt31Id));
}

TEST_F(TypeContextTest, UInt31Predicates) {
  TypeContext ctx;

  EXPECT_TRUE(ctx.canBeNumber(asType(kUInt31Id)));
  EXPECT_TRUE(ctx.isPrimitive(asType(kUInt31Id)));
  EXPECT_TRUE(ctx.isNonPtr(asType(kUInt31Id)));
  EXPECT_FALSE(ctx.canBeString(asType(kUInt31Id)));
  EXPECT_FALSE(ctx.canBeObject(asType(kUInt31Id)));
}

TEST_F(TypeContextTest, CountKinds) {
  TypeContext ctx;

  EXPECT_EQ(ctx.countKinds(Type::createNoType()), 0u);
  EXPECT_EQ(ctx.countKinds(Type::createNumber()), 1u);
  EXPECT_EQ(ctx.countKinds(Type::createString()), 1u);
  EXPECT_EQ(ctx.countKinds(Type::createObject()), 1u);
  // AnyType has 8 arms.
  EXPECT_EQ(ctx.countKinds(Type::createAnyType()), 8u);
  // Numeric = Number | BigInt.
  EXPECT_EQ(ctx.countKinds(Type::createNumeric()), 2u);
  // NullOrUndef = Null | Undefined.
  EXPECT_EQ(ctx.countKinds(Type::createNullOrUndef()), 2u);
  // AnyEmptyUninit = 10 arms.
  EXPECT_EQ(ctx.countKinds(Type::createAnyEmptyUninit()), 10u);
  // Dynamic union.
  Type numStr = ctx.unionTy(Type::createNumber(), Type::createString());
  EXPECT_EQ(ctx.countKinds(numStr), 2u);
}

TEST_F(TypeContextTest, GetFirstKind) {
  TypeContext ctx;

  EXPECT_EQ(ctx.getFirstKind(Type::createNoType()), TypeKind::NoType);
  EXPECT_EQ(ctx.getFirstKind(Type::createNumber()), TypeKind::Number);
  EXPECT_EQ(ctx.getFirstKind(Type::createString()), TypeKind::String);
  EXPECT_EQ(ctx.getFirstKind(Type::createObject()), TypeKind::Object);
  EXPECT_EQ(ctx.getFirstKind(Type::createEmpty()), TypeKind::Empty);
  // AnyType first arm is Undefined (lowest ID arm).
  EXPECT_EQ(ctx.getFirstKind(Type::createAnyType()), TypeKind::Undefined);
  // Numeric first arm is Number (ID 7 < ID 8).
  EXPECT_EQ(ctx.getFirstKind(Type::createNumeric()), TypeKind::Number);
  // NullOrUndef first arm is Undefined (ID 3 < ID 4).
  EXPECT_EQ(ctx.getFirstKind(Type::createNullOrUndef()), TypeKind::Undefined);
}

TEST_F(TypeContextTest, PrintLeafTypes) {
  TypeContext ctx;
  auto fmt = [&](Type t) -> std::string {
    std::string s;
    llvh::raw_string_ostream os(s);
    ctx.print(os, t);
    return s;
  };

  EXPECT_EQ(fmt(Type::createNoType()), "notype");
  EXPECT_EQ(fmt(Type::createNumber()), "number");
  EXPECT_EQ(fmt(Type::createString()), "string");
  EXPECT_EQ(fmt(Type::createObject()), "object");
  EXPECT_EQ(fmt(Type::createBoolean()), "boolean");
  EXPECT_EQ(fmt(Type::createNull()), "null");
  EXPECT_EQ(fmt(Type::createUndefined()), "undefined");
  EXPECT_EQ(fmt(Type::createBigInt()), "bigint");
  EXPECT_EQ(fmt(Type::createSymbol()), "symbol");
  EXPECT_EQ(fmt(Type::createEmpty()), "empty");
  EXPECT_EQ(fmt(Type::createUninit()), "uninit");
  EXPECT_EQ(fmt(Type::createEnvironment()), "environment");
  EXPECT_EQ(fmt(Type::createPrivateName()), "privateName");
  EXPECT_EQ(fmt(Type::createFunctionCode()), "functionCode");
  EXPECT_EQ(fmt(asType(kBits32Id)), "bits32");
  EXPECT_EQ(fmt(asType(kInt32Id)), "int32");
  EXPECT_EQ(fmt(asType(kUint32Id)), "uint32");
  EXPECT_EQ(fmt(asType(kUInt31Id)), "uint31");
}

TEST_F(TypeContextTest, PrintWellKnownUnions) {
  TypeContext ctx;
  auto fmt = [&](Type t) -> std::string {
    std::string s;
    llvh::raw_string_ostream os(s);
    ctx.print(os, t);
    return s;
  };

  // AnyType prints as "any".
  EXPECT_EQ(fmt(Type::createAnyType()), "any");
  // AnyEmptyUninit prints as "any|empty|uninit".
  EXPECT_EQ(fmt(Type::createAnyEmptyUninit()), "any|empty|uninit");
  // Numeric = Number | BigInt.
  EXPECT_EQ(fmt(Type::createNumeric()), "number|bigint");
  // NullOrUndef = Undefined | Null (sorted by ID).
  EXPECT_EQ(fmt(Type::createNullOrUndef()), "undefined|null");
}

TEST_F(TypeContextTest, PrintDynamicUnion) {
  TypeContext ctx;
  auto fmt = [&](Type t) -> std::string {
    std::string s;
    llvh::raw_string_ostream os(s);
    ctx.print(os, t);
    return s;
  };

  Type numStr = ctx.unionTy(Type::createNumber(), Type::createString());
  // Arms sorted by ID: String(6), Number(7).
  EXPECT_EQ(fmt(numStr), "string|number");
}

TEST_F(TypeContextTest, PrintAnySupersetShowsExtraArms) {
  TypeContext ctx;
  auto fmt = [&](Type t) -> std::string {
    std::string s;
    llvh::raw_string_ostream os(s);
    ctx.print(os, t);
    return s;
  };

  // any | environment should print "any|environment", not collapse to "any".
  Type anyEnv = ctx.unionTy(Type::createAnyType(), Type::createEnvironment());
  EXPECT_EQ(fmt(anyEnv), "any|environment");

  // AnyEmptyUninit is any | empty | uninit.
  EXPECT_EQ(fmt(asType(kAnyEmptyUninitId)), "any|empty|uninit");
}

TEST_F(TypeContextTest, ArmsLeaf) {
  TypeContext ctx;
  llvh::SmallVector<Type, 4> v;
  for (Type t : ctx.arms(Type::createNumber()))
    v.push_back(t);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0], Type::createNumber());
}

TEST_F(TypeContextTest, ArmsNoType) {
  TypeContext ctx;
  llvh::SmallVector<Type, 4> v;
  for (Type t : ctx.arms(Type::createNoType()))
    v.push_back(t);
  EXPECT_EQ(v.size(), 0u);
}

TEST_F(TypeContextTest, ArmsUnion) {
  TypeContext ctx;
  Type numStr = ctx.unionTy(Type::createNumber(), Type::createString());
  llvh::SmallVector<Type, 4> v;
  for (Type t : ctx.arms(numStr))
    v.push_back(t);
  ASSERT_EQ(v.size(), 2u);
  // Sorted by ID: String (6), Number (7).
  EXPECT_EQ(v[0], Type::createString());
  EXPECT_EQ(v[1], Type::createNumber());
}

TEST_F(TypeContextTest, CanBeAnyAndConveniences) {
  TypeContext ctx;
  EXPECT_TRUE(ctx.canBeAny(Type::createAnyType()));
  EXPECT_FALSE(ctx.canBeAny(Type::createNumber()));
  EXPECT_TRUE(ctx.canBeType(Type::createAnyType(), Type::createNumber()));
  EXPECT_FALSE(ctx.canBeType(Type::createNumber(), Type::createString()));
  EXPECT_TRUE(
      ctx.isProperSubsetOf(Type::createNumber(), Type::createAnyType()));
  EXPECT_FALSE(
      ctx.isProperSubsetOf(Type::createNumber(), Type::createNumber()));
  EXPECT_TRUE(ctx.isKnownPrimitiveType(Type::createNumber()));
  EXPECT_FALSE(ctx.isKnownPrimitiveType(Type::createAnyType()));
  EXPECT_FALSE(ctx.isKnownPrimitiveType(Type::createObject()));
}

TEST_F(TypeContextTest, PrimMaskValues) {
  TypeContext tc;
  // Leaves.
  EXPECT_EQ(0u, tc.testPrimMask(Type::createNoType()));
  EXPECT_EQ(MB_String, tc.testPrimMask(Type::createString()));
  EXPECT_EQ((uint16_t)NC_Number, tc.testPrimMask(Type::createNumber()));
  EXPECT_EQ(kNotMaskable, tc.testPrimMask(Type::createEnvironment()));
  EXPECT_EQ(kNotMaskable, tc.testPrimMask(Type::createFunctionCode()));
  // Well-known unions.
  EXPECT_EQ(
      (uint16_t)(MB_String | MB_Symbol),
      tc.testPrimMask(Type::createStringOrSymbol()));
  EXPECT_EQ(
      (uint16_t)(MB_Null | MB_Undefined),
      tc.testPrimMask(Type::createNullOrUndef()));
  // AnyType = the 7 primitives + Object (no Empty/Uninit/internal).
  EXPECT_EQ(
      (uint16_t)(MB_Undefined | MB_Null | MB_Boolean | MB_BigInt | MB_String |
                 MB_Symbol | MB_Object | NC_Number),
      tc.testPrimMask(Type::createAnyType()));
  // Well-known ids map to the matching leaf factories.
  EXPECT_EQ(Type::createString(), asType(kStringId));
  EXPECT_EQ(Type::createNumber(), asType(kNumberId));
}

TEST_F(TypeContextTest, LookupPrimMaskRoundTrips) {
  TypeContext tc;
  // Single bits map back to leaves.
  EXPECT_EQ(Type::createString(), tc.testLookupPrimMask(MB_String));
  EXPECT_EQ(Type::createNumber(), tc.testLookupPrimMask(NC_Number));
  EXPECT_EQ(Type::createNoType(), tc.testLookupPrimMask(0));
  // Combinations equal the slow-path union, and are stable (interned).
  Type ns = tc.unionTy(Type::createNumber(), Type::createString());
  EXPECT_EQ(ns, tc.testLookupPrimMask((uint16_t)(MB_String | NC_Number)));
  EXPECT_EQ(
      tc.testLookupPrimMask((uint16_t)(MB_String | NC_Number)),
      tc.testLookupPrimMask((uint16_t)(MB_String | NC_Number)));
  // Int32 ∪ Uint32 materializes to the 2-arm union.
  Type iu = tc.testLookupPrimMask(NC_Int32OrUint32);
  EXPECT_EQ(2u, tc.countKinds(iu));
  // Multi-arm round-trip: Number | Boolean | String.
  Type nbs = tc.unionTy(
      Type::createNumber(),
      tc.unionTy(Type::createBoolean(), Type::createString()));
  EXPECT_EQ(
      nbs,
      tc.testLookupPrimMask((uint16_t)(NC_Number | MB_Boolean | MB_String)));
}

TEST_F(TypeContextTest, FastPathConstructors) {
  TypeContext tc;
  Type num = Type::createNumber(), str = Type::createString();
  Type i32 = asType(kInt32Id);
  Type u32 = asType(kUint32Id);
  Type u31 = asType(kUInt31Id);

  // union
  Type ns = tc.unionTy(num, str);
  EXPECT_TRUE(tc.canBeNumber(ns) && tc.canBeString(ns));
  EXPECT_EQ(num, tc.unionTy(num, i32)); // Int32 <: Number
  EXPECT_EQ(2u, tc.countKinds(tc.unionTy(i32, u32))); // Int32 ∪ Uint32

  // intersect
  EXPECT_EQ(u31, tc.intersectTy(i32, u32)); // Int32 ∩ Uint32 = UInt31
  EXPECT_EQ(i32, tc.intersectTy(num, i32));
  EXPECT_EQ(Type::createNoType(), tc.intersectTy(num, str));

  // subtract
  EXPECT_EQ(str, tc.subtractTy(tc.unionTy(num, str), num));
  EXPECT_EQ(num, tc.subtractTy(num, i32)); // conservative: Number - Int32
}

TEST_F(TypeContextTest, FastPathQueries) {
  TypeContext tc;
  Type num = Type::createNumber(), str = Type::createString();
  Type obj = Type::createObject(), i32 = asType(kInt32Id);
  Type ns = tc.unionTy(num, str);

  EXPECT_TRUE(tc.isSubsetOf(num, ns));
  EXPECT_TRUE(tc.isSubsetOf(i32, num));
  EXPECT_FALSE(tc.isSubsetOf(ns, num));
  EXPECT_TRUE(tc.areDisjoint(num, str));
  EXPECT_FALSE(tc.areDisjoint(num, i32));
  EXPECT_TRUE(tc.areDisjoint(num, obj));

  EXPECT_TRUE(tc.canBeNumber(ns));
  EXPECT_TRUE(tc.canBeString(ns));
  EXPECT_FALSE(tc.canBeObject(ns));
  EXPECT_TRUE(tc.canBeNumber(i32));
}

TEST_F(TypeContextTest, FastPathPredicates) {
  TypeContext tc;
  Type num = Type::createNumber(), str = Type::createString();
  Type obj = Type::createObject(), i32 = asType(kInt32Id);
  Type u32 = asType(kUint32Id);
  Type ns = tc.unionTy(num, str);
  Type no = Type::createNoType();

  EXPECT_TRUE(tc.isPrimitive(num));
  EXPECT_TRUE(tc.isPrimitive(ns));
  EXPECT_FALSE(tc.isPrimitive(obj));
  EXPECT_FALSE(tc.isPrimitive(no));

  EXPECT_TRUE(tc.canBePrimitive(tc.unionTy(num, obj)));
  EXPECT_FALSE(tc.canBePrimitive(obj));

  EXPECT_TRUE(tc.isNonPtr(num));
  EXPECT_FALSE(tc.isNonPtr(str));

  EXPECT_TRUE(tc.isKnownPrimitiveType(num));
  EXPECT_TRUE(tc.isKnownPrimitiveType(i32));
  EXPECT_TRUE(tc.isKnownPrimitiveType(asType(kUInt31Id)));
  EXPECT_FALSE(tc.isKnownPrimitiveType(ns));
  EXPECT_FALSE(tc.isKnownPrimitiveType(no));
  EXPECT_FALSE(tc.isKnownPrimitiveType(obj));

  // A union of number subtypes is still the single JS primitive "number".
  Type i32u32 = tc.unionTy(i32, u32);
  EXPECT_TRUE(tc.isKnownPrimitiveType(i32u32));
  // ...but mixing the number family with another primitive is two primitives.
  EXPECT_FALSE(tc.isKnownPrimitiveType(tc.unionTy(i32u32, str)));
}

TEST_F(TypeContextTest, WellKnownUnionsAreInterned) {
  TypeContext tc;
  // Dynamically building a well-known union must return its canonical
  // well-known id. Regression: the last well-known union (ObjectOrUndef)
  // was skipped by the intern-table pre-population loop, so building it
  // dynamically produced a fresh duplicate id != createObjectOrUndef().
  EXPECT_EQ(
      Type::createObjectOrUndef(),
      tc.unionTy(Type::createObject(), Type::createUndefined()));
  EXPECT_EQ(
      Type::createObjectOrNull(),
      tc.unionTy(Type::createObject(), Type::createNull()));
  EXPECT_EQ(
      Type::createStringOrSymbol(),
      tc.unionTy(Type::createString(), Type::createSymbol()));
}

TEST(TypeContextNoGuardTest, PrintFallback) {
  // Without a TypeContext, Type::print emits a "type#<id>" placeholder.
  std::string s;
  llvh::raw_string_ostream os(s);
  Type::createNumber().print(os);
  EXPECT_EQ(os.str(), "type#7");
}

TEST_F(TypeContextTest, ClassInstanceConstruction) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  Identifier animalName = strTab.getIdentifier("Animal");
  Identifier dogName = strTab.getIdentifier("Dog");

  // class Animal { legs: number }
  ExactObjectField animalFields[] = {
      {strTab.getIdentifier("legs"), Type::createNumber()}};
  Type animal =
      ctx.createClassInstance(Type::createNoType(), animalName, animalFields);

  // class Dog extends Animal { name: string }
  ExactObjectField dogFields[] = {
      {strTab.getIdentifier("name"), Type::createString()}};
  Type dog = ctx.createClassInstance(animal, dogName, dogFields);

  EXPECT_EQ(ctx.getKind(animal), TypeKind::ClassInstance);
  EXPECT_EQ(ctx.getKind(dog), TypeKind::ClassInstance);

  // Nominal: each declaration is a distinct entry, even structurally identical.
  Type animal2 =
      ctx.createClassInstance(Type::createNoType(), animalName, animalFields);
  EXPECT_NE(animal, animal2);

  // Accessors.
  EXPECT_EQ(ctx.getClassName(dog), dogName);
  EXPECT_EQ(ctx.getClassSuperType(dog), animal);
  EXPECT_TRUE(ctx.getClassSuperType(animal).isNoType());

  auto fields = ctx.getClassFields(dog);
  ASSERT_EQ(fields.size(), 1u);
  EXPECT_EQ(fields[0].name, strTab.getIdentifier("name"));
  EXPECT_EQ(fields[0].type, Type::createString());
}

TEST_F(TypeContextTest, ClassInstanceReserveFillSelfReference) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  // class Node { next: Node } -- reserve first so the self-referential field
  // can point at the class being built.
  Type node = ctx.reserveClassInstance(Type::createNoType());
  ExactObjectField fields[] = {{strTab.getIdentifier("next"), node}};
  ctx.fillClassInstance(node, strTab.getIdentifier("Node"), fields);

  EXPECT_EQ(ctx.getKind(node), TypeKind::ClassInstance);
  EXPECT_EQ(ctx.getClassName(node), strTab.getIdentifier("Node"));
  EXPECT_TRUE(ctx.getClassSuperType(node).isNoType());

  auto f = ctx.getClassFields(node);
  ASSERT_EQ(f.size(), 1u);
  EXPECT_EQ(f[0].name, strTab.getIdentifier("next"));
  EXPECT_EQ(f[0].type, node); // self-reference resolved to the same type
}

/// A class is reserved with its superclass, so the algebra treats the
/// reserved, unfilled entry as a subclass while its fields are being built:
/// the field `f: D | Base` of `class D extends Base` normalizes to Base.
TEST_F(TypeContextTest, ClassInstanceReservedWithSuperclass) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  Type base = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("Base"), {});
  Type d = ctx.reserveClassInstance(base);

  // Before fill.
  EXPECT_EQ(ctx.getClassSuperType(d), base);
  EXPECT_TRUE(ctx.isSubclass(d, base));
  EXPECT_TRUE(ctx.isSubsetOf(d, base));
  EXPECT_FALSE(ctx.areDisjoint(d, base));
  EXPECT_EQ(ctx.intersectTy(d, base), d);
  Type fieldTy = ctx.unionTy(d, base);
  EXPECT_EQ(fieldTy, base);

  ExactObjectField fields[] = {{strTab.getIdentifier("f"), fieldTy}};
  ctx.fillClassInstance(d, strTab.getIdentifier("D"), fields);

  // Fill sets the name and fields and keeps the superclass.
  EXPECT_EQ(ctx.getClassSuperType(d), base);
  EXPECT_EQ(ctx.getClassName(d), strTab.getIdentifier("D"));
  ASSERT_EQ(ctx.getClassFields(d).size(), 1u);
  EXPECT_EQ(ctx.getClassFields(d)[0].type, base);
}

TEST_F(TypeContextTest, ClassInstanceFillFromOwnFields) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  ExactObjectField baseFields[] = {
      {strTab.getIdentifier("a"), Type::createNumber()},
      {strTab.getIdentifier("b"), Type::createString()}};
  Type prev = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("C0"), baseFields);

  // Build a chain of classes, each filled with the fields of the previous one
  // read straight out of the context. The fields being appended point into
  // the side array they are appended to, which grows and reallocates many
  // times along the chain.
  for (unsigned i = 0; i < 64; ++i) {
    Type cur = ctx.reserveClassInstance(prev);
    ctx.fillClassInstance(
        cur, strTab.getIdentifier("C"), ctx.getClassFields(prev));
    llvh::ArrayRef<ExactObjectField> fields = ctx.getClassFields(cur);
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields[0].name, strTab.getIdentifier("a"));
    EXPECT_EQ(fields[0].type, Type::createNumber());
    EXPECT_EQ(fields[1].name, strTab.getIdentifier("b"));
    EXPECT_EQ(fields[1].type, Type::createString());
    prev = cur;
  }
}

TEST_F(TypeContextTest, ClassInstanceSubtyping) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  Type animal = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("Animal"), {});
  Type dog = ctx.createClassInstance(animal, strTab.getIdentifier("Dog"), {});
  Type cat = ctx.createClassInstance(animal, strTab.getIdentifier("Cat"), {});

  // Reflexive + nominal chain.
  EXPECT_TRUE(ctx.isSubsetOf(dog, dog));
  EXPECT_TRUE(ctx.isSubsetOf(dog, animal));
  EXPECT_FALSE(ctx.isSubsetOf(animal, dog));
  EXPECT_FALSE(ctx.isSubsetOf(dog, cat));
  EXPECT_FALSE(ctx.isSubsetOf(cat, dog));

  // Every refinement <: Object.
  EXPECT_TRUE(ctx.isSubsetOf(dog, Type::createObject()));
  EXPECT_FALSE(ctx.isSubsetOf(Type::createObject(), dog));

  // Class vs primitive: never a subtype.
  EXPECT_FALSE(ctx.isSubsetOf(dog, Type::createNumber()));
  EXPECT_FALSE(ctx.isSubsetOf(Type::createNumber(), dog));

  // Class is a subset of a union containing its ancestor.
  Type animalOrNull = ctx.unionTy(animal, Type::createNull());
  EXPECT_TRUE(ctx.isSubsetOf(dog, animalOrNull));

  // Public isSubclass.
  EXPECT_TRUE(ctx.isSubclass(dog, animal));
  EXPECT_FALSE(ctx.isSubclass(animal, dog));
  EXPECT_TRUE(ctx.isSubclass(dog, dog));
}

/// Subclassing is transitive: a grandchild is a subclass of the root, which
/// a walk that stops at the immediate superclass does not find.
TEST_F(TypeContextTest, ClassInstanceTransitiveSubtyping) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  // Puppy extends Dog extends Animal; Cat extends Animal.
  Type animal = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("Animal"), {});
  Type dog = ctx.createClassInstance(animal, strTab.getIdentifier("Dog"), {});
  Type puppy = ctx.createClassInstance(dog, strTab.getIdentifier("Puppy"), {});
  Type cat = ctx.createClassInstance(animal, strTab.getIdentifier("Cat"), {});

  EXPECT_TRUE(ctx.isSubclass(puppy, animal));
  EXPECT_FALSE(ctx.isSubclass(animal, puppy));
  EXPECT_FALSE(ctx.isSubclass(puppy, cat));

  EXPECT_TRUE(ctx.isSubsetOf(puppy, animal));
  EXPECT_FALSE(ctx.isSubsetOf(animal, puppy));
  EXPECT_FALSE(ctx.isSubsetOf(puppy, cat));

  // A union holding the root contains the grandchild; one holding only an
  // unrelated class does not.
  Type animalOrNull = ctx.unionTy(animal, Type::createNull());
  EXPECT_TRUE(ctx.isSubsetOf(puppy, animalOrNull));
  EXPECT_FALSE(ctx.isSubsetOf(puppy, ctx.unionTy(cat, Type::createNull())));
  EXPECT_EQ(ctx.unionTy(puppy, animalOrNull), animalOrNull);
}

TEST_F(TypeContextTest, ClassInstanceAlgebra) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  Type animal = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("Animal"), {});
  Type dog = ctx.createClassInstance(animal, strTab.getIdentifier("Dog"), {});
  Type cat = ctx.createClassInstance(animal, strTab.getIdentifier("Cat"), {});

  // Disjointness.
  EXPECT_TRUE(ctx.areDisjoint(dog, cat)); // unrelated classes
  EXPECT_FALSE(ctx.areDisjoint(dog, animal)); // related
  EXPECT_TRUE(ctx.areDisjoint(dog, Type::createNumber())); // class vs primitive
  EXPECT_FALSE(ctx.areDisjoint(dog, Type::createObject())); // dog <: Object

  // Intersection.
  EXPECT_EQ(ctx.intersectTy(animal, dog), dog); // narrower wins
  EXPECT_EQ(ctx.intersectTy(dog, animal), dog);
  EXPECT_TRUE(ctx.intersectTy(dog, cat).isNoType()); // unrelated -> empty
  EXPECT_EQ(ctx.intersectTy(dog, Type::createObject()), dog);

  // Union subsumption / non-subsumption.
  EXPECT_EQ(ctx.unionTy(dog, animal), animal); // dog <: animal -> animal
  EXPECT_EQ(
      ctx.unionTy(dog, Type::createObject()),
      Type::createObject()); // refinement -> Object
  Type dogOrCat = ctx.unionTy(dog, cat);
  EXPECT_EQ(ctx.getKind(dogOrCat), TypeKind::Union); // unrelated -> union
  EXPECT_EQ(ctx.countKinds(dogOrCat), 2u);

  // The arm-by-arm intersection of a union with two unrelated class arms
  // keeps both arms: the classes share a kind, but neither subsumes the
  // other when the result union is normalized.
  EXPECT_EQ(ctx.intersectTy(dogOrCat, Type::createObject()), dogOrCat);
  EXPECT_EQ(ctx.intersectTy(dogOrCat, animal), dogOrCat);

  // Subtract stays conservative but exact where representable.
  EXPECT_TRUE(ctx.subtractTy(dog, animal).isNoType()); // dog <: animal
  EXPECT_EQ(ctx.subtractTy(dog, cat), dog); // disjoint
}

TEST_F(TypeContextTest, ClassInstancePrint) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;

  Type dog = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("Dog"), {});

  std::string s;
  llvh::raw_string_ostream os{s};
  ctx.print(os, dog);
  os.flush();
  EXPECT_EQ(s, "classInstance(Dog)");

  // A union containing a class prints the class arm via the same path.
  std::string s2;
  llvh::raw_string_ostream os2{s2};
  ctx.print(os2, ctx.unionTy(dog, Type::createNull()));
  os2.flush();
  EXPECT_NE(s2.find("classInstance(Dog)"), std::string::npos);
  EXPECT_NE(s2.find("null"), std::string::npos);
}

TEST_F(TypeContextTest, ArrayConstructionAndInterning) {
  TypeContext ctx;

  // Array<number> and Array<string>.
  Type arrNum = ctx.createArray(Type::createNumber());
  Type arrStr = ctx.createArray(Type::createString());

  EXPECT_EQ(ctx.getKind(arrNum), TypeKind::Array);
  EXPECT_EQ(ctx.getArrayElementType(arrNum), Type::createNumber());
  EXPECT_EQ(ctx.getArrayElementType(arrStr), Type::createString());

  // Interned: same element type => same id.
  Type arrNum2 = ctx.createArray(Type::createNumber());
  EXPECT_EQ(arrNum, arrNum2);
  // Different element type => different id.
  EXPECT_NE(arrNum, arrStr);
}

/// Each interning factory allocates a type table entry only on a cache miss:
/// after repeated hits, the next allocated entry immediately follows the
/// interned one.
TEST_F(TypeContextTest, FactoriesAllocateOnlyOnCacheMiss) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;
  Identifier x = strTab.getIdentifier("x");
  Type num = Type::createNumber();
  Type str = Type::createString();
  FunctionParam params[] = {{num.getRawId(), /*isOptional*/ false}};

  Type arr = ctx.createArray(num);
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(ctx.createArray(num), arr);
  EXPECT_EQ(ctx.reserveArray().getRawId(), arr.getRawId() + 1);

  Type tup = ctx.createTuple({num, str});
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(ctx.createTuple({num, str}), tup);
  EXPECT_EQ(ctx.reserveTuple().getRawId(), tup.getRawId() + 1);

  Type obj = ctx.createExactObject({{x, num}});
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(ctx.createExactObject({{x, num}}), obj);
  EXPECT_EQ(ctx.reserveExactObject().getRawId(), obj.getRawId() + 1);

  Type fn = ctx.createFunction(
      str, Type::createNoType(), Type::createNoType(), params, false, false);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(
        ctx.createFunction(
            str,
            Type::createNoType(),
            Type::createNoType(),
            params,
            false,
            false),
        fn);
  }
  EXPECT_EQ(ctx.reserveFunction().getRawId(), fn.getRawId() + 1);
}

TEST_F(TypeContextTest, ArrayReserveFillSelfReference) {
  TypeContext ctx;
  // Simulate a recursive array whose element is the array itself:
  //   type A = Array<A>
  Type a = ctx.reserveArray();
  ctx.fillArray(a, a);
  EXPECT_EQ(ctx.getKind(a), TypeKind::Array);
  EXPECT_EQ(ctx.getArrayElementType(a), a);
}

/// \return the element types of Tuple \p t in \p ctx, copied into a vector.
std::vector<Type> tupleElements(const TypeContext &ctx, Type t) {
  llvh::ArrayRef<Type> elems = ctx.getTupleElements(t);
  return std::vector<Type>(elems.begin(), elems.end());
}

TEST_F(TypeContextTest, TupleConstructionAndInterning) {
  TypeContext ctx;
  Type num = Type::createNumber();
  Type str = Type::createString();

  // [number, string]
  Type numStr = ctx.createTuple({num, str});
  EXPECT_EQ(ctx.getKind(numStr), TypeKind::Tuple);
  EXPECT_EQ(tupleElements(ctx, numStr), (std::vector<Type>{num, str}));

  // Interned: same element types in the same order => same id.
  EXPECT_EQ(ctx.createTuple({num, str}), numStr);

  // Order matters: [string, number] is a different tuple.
  Type strNum = ctx.createTuple({str, num});
  EXPECT_NE(strNum, numStr);
  EXPECT_EQ(tupleElements(ctx, strNum), (std::vector<Type>{str, num}));

  // Repeated elements are kept: [number] and [number, number] differ.
  Type one = ctx.createTuple({num});
  Type two = ctx.createTuple({num, num});
  EXPECT_NE(one, two);
  EXPECT_EQ(tupleElements(ctx, two), (std::vector<Type>{num, num}));

  // The empty tuple [] is interned too.
  Type empty = ctx.createTuple({});
  EXPECT_EQ(ctx.getKind(empty), TypeKind::Tuple);
  EXPECT_TRUE(ctx.getTupleElements(empty).empty());
  EXPECT_EQ(ctx.createTuple({}), empty);
  EXPECT_NE(empty, one);
}

TEST_F(TypeContextTest, TupleReserveFillSelfReference) {
  TypeContext ctx;
  Type num = Type::createNumber();
  // Simulate a recursive tuple whose second element is the tuple itself:
  //   type T = [number, T]
  Type t = ctx.reserveTuple();
  ctx.fillTuple(t, {num, t});
  EXPECT_EQ(ctx.getKind(t), TypeKind::Tuple);
  EXPECT_EQ(tupleElements(ctx, t), (std::vector<Type>{num, t}));

  // A reserved entry is not interned: building the same element list again
  // creates a separate tuple, whose second element is the reserved one.
  Type u = ctx.createTuple({num, t});
  EXPECT_NE(u, t);
  EXPECT_EQ(tupleElements(ctx, u), (std::vector<Type>{num, t}));
}

/// A field of an ExactObject as a comparable (name, type) pair.
using FieldPair = std::pair<Identifier, Type>;

/// \return the fields of ExactObject \p t in \p ctx, as (name, type) pairs.
std::vector<FieldPair> objectFields(const TypeContext &ctx, Type t) {
  std::vector<FieldPair> res;
  for (const ExactObjectField &f : ctx.getExactObjectFields(t))
    res.emplace_back(f.name, f.type);
  return res;
}

TEST_F(TypeContextTest, ExactObjectConstructionAndInterning) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;
  Identifier x = strTab.getIdentifier("x");
  Identifier y = strTab.getIdentifier("y");
  Type num = Type::createNumber();
  Type str = Type::createString();

  // {x: number, y: string}
  Type xy = ctx.createExactObject({{x, num}, {y, str}});
  EXPECT_EQ(ctx.getKind(xy), TypeKind::ExactObject);
  EXPECT_EQ(
      objectFields(ctx, xy), (std::vector<FieldPair>{{x, num}, {y, str}}));

  // Interned: same names and types in the same order => same id.
  EXPECT_EQ(ctx.createExactObject({{x, num}, {y, str}}), xy);

  // Field order matters: {y: string, x: number} is a different type.
  Type yx = ctx.createExactObject({{y, str}, {x, num}});
  EXPECT_NE(yx, xy);
  EXPECT_EQ(
      objectFields(ctx, yx), (std::vector<FieldPair>{{y, str}, {x, num}}));

  // Same names, different types: {x: string, y: number} differs.
  EXPECT_NE(ctx.createExactObject({{x, str}, {y, num}}), xy);

  // Same types, different names: {x: number} and {y: number} differ.
  Type xNum = ctx.createExactObject({{x, num}});
  Type yNum = ctx.createExactObject({{y, num}});
  EXPECT_NE(xNum, yNum);

  // A prefix is a different type: {x: number} and {x: number, y: string}.
  EXPECT_NE(xNum, xy);

  // The empty object {} is interned too.
  Type empty = ctx.createExactObject({});
  EXPECT_EQ(ctx.getKind(empty), TypeKind::ExactObject);
  EXPECT_TRUE(ctx.getExactObjectFields(empty).empty());
  EXPECT_EQ(ctx.createExactObject({}), empty);
  EXPECT_NE(empty, xNum);

  // An exact object with the same fields as a class is not that class.
  ExactObjectField classFields[] = {{x, num}};
  Type cls = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("C"), classFields);
  EXPECT_NE(ctx.createExactObject(classFields), cls);
  EXPECT_EQ(ctx.createExactObject(classFields), xNum);
}

TEST_F(TypeContextTest, ExactObjectReserveFillSelfReference) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;
  Identifier left = strTab.getIdentifier("left");
  Identifier right = strTab.getIdentifier("right");

  // type Tree = null | {left: Tree, right: Tree}
  // The object is reserved first so that the union, and through it the
  // object's own fields, can refer to it.
  Type node = ctx.reserveExactObject();
  Type tree = ctx.unionTy(Type::createNull(), node);
  ctx.fillExactObject(node, {{left, tree}, {right, tree}});

  EXPECT_EQ(ctx.getKind(node), TypeKind::ExactObject);
  EXPECT_EQ(
      objectFields(ctx, node),
      (std::vector<FieldPair>{{left, tree}, {right, tree}}));
  EXPECT_EQ(ctx.getKind(tree), TypeKind::Union);
  EXPECT_TRUE(ctx.isSubsetOf(node, tree));
  EXPECT_EQ(ctx.unionTy(Type::createNull(), node), tree);

  // A reserved entry is not interned: building the same field list again
  // creates a separate object, whose fields refer to the reserved one.
  Type other = ctx.createExactObject({{left, tree}, {right, tree}});
  EXPECT_NE(other, node);
  EXPECT_EQ(
      objectFields(ctx, other),
      (std::vector<FieldPair>{{left, tree}, {right, tree}}));
}

TEST_F(TypeContextTest, ExactObjectFillFromOwnFields) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;
  Identifier a = strTab.getIdentifier("a");
  Identifier b = strTab.getIdentifier("b");
  Type num = Type::createNumber();
  Type str = Type::createString();

  Type prev = ctx.createExactObject({{a, num}, {b, str}});
  // Build a chain of objects, each filled with the fields of the previous one
  // read straight out of the context. The fields being appended point into
  // the side array they are appended to, which grows and reallocates many
  // times along the chain.
  for (unsigned i = 0; i < 64; ++i) {
    Type cur = ctx.reserveExactObject();
    ctx.fillExactObject(cur, ctx.getExactObjectFields(prev));
    EXPECT_EQ(
        objectFields(ctx, cur), (std::vector<FieldPair>{{a, num}, {b, str}}));
    prev = cur;
  }
}

TEST_F(TypeContextTest, FunctionConstructionAndInterning) {
  TypeContext ctx;

  FunctionParam params[] = {
      {Type::createNumber().getRawId(), /*isOptional*/ false},
      {Type::createString().getRawId(), /*isOptional*/ true}};

  // (a: number, b?: string) => boolean, this: object, no rest.
  Type fn = ctx.createFunction(
      Type::createBoolean(),
      Type::createObject(),
      Type::createNoType(),
      params,
      /*isAsync*/ false,
      /*isGenerator*/ false);

  EXPECT_EQ(ctx.getKind(fn), TypeKind::Function);
  EXPECT_EQ(ctx.getFunctionReturn(fn), Type::createBoolean());
  EXPECT_EQ(ctx.getFunctionThis(fn), Type::createObject());
  EXPECT_TRUE(ctx.getFunctionRest(fn).isNoType());
  EXPECT_FALSE(ctx.getFunctionIsAsync(fn));
  EXPECT_FALSE(ctx.getFunctionIsGenerator(fn));
  auto gotParams = ctx.getFunctionParams(fn);
  ASSERT_EQ(gotParams.size(), 2u);
  EXPECT_EQ(gotParams[0].typeId, Type::createNumber().getRawId());
  EXPECT_FALSE(gotParams[0].isOptional);
  EXPECT_EQ(gotParams[1].typeId, Type::createString().getRawId());
  EXPECT_TRUE(gotParams[1].isOptional);

  // Interned: identical signature => same id.
  Type fn2 = ctx.createFunction(
      Type::createBoolean(),
      Type::createObject(),
      Type::createNoType(),
      params,
      false,
      false);
  EXPECT_EQ(fn, fn2);
}

/// A function type is interned by its whole signature: changing any single
/// component gives a distinct type, and every accessor returns the stored
/// component, including a present rest param and true async/generator flags.
TEST_F(TypeContextTest, FunctionInterningBySignatureComponent) {
  TypeContext ctx;
  Type num = Type::createNumber();
  Type str = Type::createString();
  Type boolean = Type::createBoolean();
  Type obj = Type::createObject();
  Type none = Type::createNoType();
  uint32_t numId = num.getRawId();
  uint32_t strId = str.getRawId();

  // async function* (this: object, a: number, b?: string, ...rest: string):
  // boolean
  FunctionParam params[] = {{numId, false}, {strId, /*isOptional*/ true}};
  Type base = ctx.createFunction(
      boolean, obj, str, params, /*isAsync*/ true, /*isGenerator*/ true);
  EXPECT_EQ(ctx.getFunctionReturn(base), boolean);
  EXPECT_EQ(ctx.getFunctionThis(base), obj);
  EXPECT_EQ(ctx.getFunctionRest(base), str);
  EXPECT_TRUE(ctx.getFunctionIsAsync(base));
  EXPECT_TRUE(ctx.getFunctionIsGenerator(base));
  llvh::ArrayRef<FunctionParam> got = ctx.getFunctionParams(base);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].typeId, numId);
  EXPECT_FALSE(got[0].isOptional);
  EXPECT_EQ(got[1].typeId, strId);
  EXPECT_TRUE(got[1].isOptional);

  // Each variant differs from base in exactly one component.
  FunctionParam otherType[] = {{numId, false}, {numId, true}};
  FunctionParam otherOptional[] = {{numId, false}, {strId, false}};
  FunctionParam swapped[] = {{strId, true}, {numId, false}};
  FunctionParam fewer[] = {{numId, false}};
  FunctionParam more[] = {{numId, false}, {strId, true}, {strId, true}};
  struct Variant {
    const char *what;
    Type type;
  };
  Variant variants[] = {
      {"return", ctx.createFunction(num, obj, str, params, true, true)},
      {"this", ctx.createFunction(boolean, none, str, params, true, true)},
      {"rest", ctx.createFunction(boolean, obj, none, params, true, true)},
      {"param type",
       ctx.createFunction(boolean, obj, str, otherType, true, true)},
      {"param optional",
       ctx.createFunction(boolean, obj, str, otherOptional, true, true)},
      {"param order",
       ctx.createFunction(boolean, obj, str, swapped, true, true)},
      {"fewer params",
       ctx.createFunction(boolean, obj, str, fewer, true, true)},
      {"more params", ctx.createFunction(boolean, obj, str, more, true, true)},
      {"isAsync", ctx.createFunction(boolean, obj, str, params, false, true)},
      {"isGenerator",
       ctx.createFunction(boolean, obj, str, params, true, false)},
  };
  for (const Variant &v : variants)
    EXPECT_NE(v.type, base) << v.what;

  // Only the identical signature hits the cache.
  EXPECT_EQ(ctx.createFunction(boolean, obj, str, params, true, true), base);
}

TEST_F(TypeContextTest, FunctionReserveFillSelfReference) {
  TypeContext ctx;
  // type F = (cb: F) => void  — a function whose parameter is itself.
  Type f = ctx.reserveFunction();
  FunctionParam params[] = {{f.getRawId(), false}};
  ctx.fillFunction(
      f,
      Type::createUndefined(),
      Type::createNoType(),
      Type::createNoType(),
      params,
      false,
      false);
  EXPECT_EQ(ctx.getKind(f), TypeKind::Function);
  ASSERT_EQ(ctx.getFunctionParams(f).size(), 1u);
  EXPECT_EQ(ctx.getFunctionParams(f)[0].typeId, f.getRawId());
}

TEST_F(TypeContextTest, FunctionFillFromOwnParams) {
  TypeContext ctx;
  FunctionParam baseParams[] = {
      {Type::createNumber().getRawId(), false},
      {Type::createString().getRawId(), true}};
  Type prev = ctx.createFunction(
      Type::createUndefined(),
      Type::createNoType(),
      Type::createNoType(),
      baseParams,
      false,
      false);

  // Build a chain of functions, each returning the previous one and filled
  // with its params read straight out of the context. The params being
  // appended point into the side array they are appended to, which grows and
  // reallocates many times along the chain.
  for (unsigned i = 0; i < 64; ++i) {
    Type cur = ctx.reserveFunction();
    ctx.fillFunction(
        cur,
        prev,
        Type::createNoType(),
        Type::createNoType(),
        ctx.getFunctionParams(prev),
        false,
        false);
    llvh::ArrayRef<FunctionParam> params = ctx.getFunctionParams(cur);
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(params[0].typeId, Type::createNumber().getRawId());
    EXPECT_FALSE(params[0].isOptional);
    EXPECT_EQ(params[1].typeId, Type::createString().getRawId());
    EXPECT_TRUE(params[1].isOptional);
    prev = cur;
  }
}

TEST_F(TypeContextTest, ReservedUnfilledEntriesInAlgebra) {
  TypeContext ctx;
  // Lowering a recursive type such as
  //   type Json = number | string | Json[]
  // builds the union number|string|A while the Array entry A is reserved but
  // not yet filled. The algebra must work from the kind and the id alone.
  Type numStr = ctx.unionTy(Type::createNumber(), Type::createString());
  Type arrNum = ctx.createArray(Type::createNumber());
  Type fnVoid = ctx.createFunction(
      Type::createUndefined(),
      Type::createNoType(),
      Type::createNoType(),
      {},
      false,
      false);

  Type reservedArr = ctx.reserveArray();
  Type reservedFn = ctx.reserveFunction();
  Type reservedTup = ctx.reserveTuple();
  Type reservedObj = ctx.reserveExactObject();
  // Only the kind of an unfilled entry may be read; its payload is valid
  // only after the fill.
  ASSERT_EQ(ctx.getKind(reservedArr), TypeKind::Array);
  ASSERT_EQ(ctx.getKind(reservedFn), TypeKind::Function);
  ASSERT_EQ(ctx.getKind(reservedTup), TypeKind::Tuple);
  ASSERT_EQ(ctx.getKind(reservedObj), TypeKind::ExactObject);

  for (Type r : {reservedArr, reservedFn, reservedTup, reservedObj}) {
    Type u = ctx.unionTy(numStr, r);
    EXPECT_EQ(ctx.countKinds(u), 3u);
    EXPECT_TRUE(ctx.isSubsetOf(r, u));
    EXPECT_TRUE(ctx.isSubsetOf(numStr, u));
    EXPECT_FALSE(ctx.isSubsetOf(u, numStr));

    EXPECT_TRUE(ctx.isSubsetOf(r, Type::createObject()));
    EXPECT_FALSE(ctx.isSubsetOf(Type::createObject(), r));
    EXPECT_FALSE(ctx.isSubsetOf(r, arrNum));
    EXPECT_FALSE(ctx.isSubsetOf(r, fnVoid));

    EXPECT_TRUE(ctx.areDisjoint(r, numStr));
    EXPECT_FALSE(ctx.areDisjoint(r, Type::createObject()));
    EXPECT_FALSE(ctx.areDisjoint(r, arrNum));
    EXPECT_FALSE(ctx.areDisjoint(r, fnVoid));
    EXPECT_FALSE(ctx.areDisjoint(r, u));
  }

  // Filling changes only the payload: the union built before the fill is
  // still the canonical union of the same arms.
  Type uArr = ctx.unionTy(numStr, reservedArr);
  ctx.fillArray(reservedArr, uArr);
  EXPECT_EQ(ctx.getArrayElementType(reservedArr), uArr);
  EXPECT_EQ(ctx.unionTy(numStr, reservedArr), uArr);

  Type uTup = ctx.unionTy(numStr, reservedTup);
  ctx.fillTuple(reservedTup, {uTup, uTup});
  EXPECT_EQ(tupleElements(ctx, reservedTup), (std::vector<Type>{uTup, uTup}));
  EXPECT_EQ(ctx.unionTy(numStr, reservedTup), uTup);

  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  Identifier x = strTab.getIdentifier("x");
  Type uObj = ctx.unionTy(numStr, reservedObj);
  ctx.fillExactObject(reservedObj, {{x, uObj}});
  EXPECT_EQ(
      objectFields(ctx, reservedObj), (std::vector<FieldPair>{{x, uObj}}));
  EXPECT_EQ(ctx.unionTy(numStr, reservedObj), uObj);
}

TEST_F(TypeContextTest, RecursiveTypesAreObjects) {
  TypeContext ctx;
  // type Json = number | string | Json[]
  Type arr = ctx.reserveArray();
  Type json =
      ctx.unionTy(ctx.unionTy(Type::createNumber(), Type::createString()), arr);
  ctx.fillArray(arr, json);
  // type F = (x: number) => F
  Type fn = ctx.reserveFunction();
  FunctionParam params[] = {{Type::createNumber().getRawId(), false}};
  ctx.fillFunction(
      fn, fn, Type::createNoType(), Type::createNoType(), params, false, false);

  EXPECT_TRUE(ctx.isSubsetOf(arr, Type::createObject()));
  EXPECT_TRUE(ctx.isSubsetOf(fn, Type::createObject()));
  EXPECT_FALSE(ctx.isSubsetOf(json, Type::createObject()));
  EXPECT_TRUE(ctx.isSubsetOf(json, json));
  EXPECT_TRUE(ctx.isSubsetOf(fn, fn));
  EXPECT_FALSE(ctx.areDisjoint(json, arr));
  EXPECT_FALSE(ctx.areDisjoint(fn, arr));
  EXPECT_EQ(ctx.intersectTy(json, Type::createObject()), arr);
  EXPECT_EQ(ctx.getFunctionReturn(fn), fn);
}

TEST_F(TypeContextTest, ArrayFunctionAlgebra) {
  TypeContext ctx;

  Type arrNum = ctx.createArray(Type::createNumber());
  Type arrStr = ctx.createArray(Type::createString());
  Type fnVoid = ctx.createFunction(
      Type::createUndefined(),
      Type::createNoType(),
      Type::createNoType(),
      {},
      false,
      false);
  Type obj = Type::createObject();

  // Every refinement is a subtype of Object.
  EXPECT_TRUE(ctx.isSubsetOf(arrNum, obj));
  EXPECT_TRUE(ctx.isSubsetOf(fnVoid, obj));
  EXPECT_FALSE(ctx.isSubsetOf(obj, arrNum));

  // Invariant / id-equality within kind: Array<number> is only a subtype of
  // itself, not of Array<string>.
  EXPECT_TRUE(ctx.isSubsetOf(arrNum, arrNum));
  EXPECT_FALSE(ctx.isSubsetOf(arrNum, arrStr));

  // Invariance, not covariance: Array<number> and Array<number|string> are
  // not subtypes of each other although number <: number|string, and
  // likewise for function types whose return types are comparable.
  Type numOrStr = ctx.unionTy(Type::createNumber(), Type::createString());
  Type arrNumOrStr = ctx.createArray(numOrStr);
  EXPECT_FALSE(ctx.isSubsetOf(arrNum, arrNumOrStr));
  EXPECT_FALSE(ctx.isSubsetOf(arrNumOrStr, arrNum));
  Type fnNum = ctx.createFunction(
      Type::createNumber(),
      Type::createNoType(),
      Type::createNoType(),
      {},
      false,
      false);
  Type fnNumOrStr = ctx.createFunction(
      numOrStr, Type::createNoType(), Type::createNoType(), {}, false, false);
  EXPECT_FALSE(ctx.isSubsetOf(fnNum, fnNumOrStr));
  EXPECT_FALSE(ctx.isSubsetOf(fnNumOrStr, fnNum));

  // Conservative disjointness (D5): distinct structural refined types are NOT
  // declared disjoint; refined-vs-primitive IS disjoint.
  EXPECT_FALSE(ctx.areDisjoint(arrNum, arrStr));
  EXPECT_FALSE(ctx.areDisjoint(arrNum, fnVoid));
  EXPECT_TRUE(ctx.areDisjoint(arrNum, Type::createNumber()));

  // intersect: refined ∩ Object = refined; refined ∩ same = same; refined ∩
  // distinct-primitive = NoType.
  EXPECT_EQ(ctx.intersectTy(arrNum, obj), arrNum);
  EXPECT_EQ(ctx.intersectTy(arrNum, arrNum), arrNum);
  EXPECT_TRUE(ctx.intersectTy(arrNum, Type::createNumber()).isNoType());

  // union: refined | Object collapses to Object (subsumption); refined |
  // distinct-refined keeps both arms.
  EXPECT_EQ(ctx.unionTy(arrNum, obj), obj);
  Type u = ctx.unionTy(arrNum, arrStr);
  EXPECT_EQ(ctx.countKinds(u), 2u);
  EXPECT_TRUE(ctx.isSubsetOf(arrNum, u));
  EXPECT_TRUE(ctx.isSubsetOf(arrStr, u));
}

TEST_F(TypeContextTest, ArrayFunctionPrint) {
  TypeContext ctx;

  Type arrNum = ctx.createArray(Type::createNumber());
  FunctionParam params[] = {{Type::createNumber().getRawId(), false}};
  Type fn = ctx.createFunction(
      Type::createString(),
      Type::createNoType(),
      Type::createNoType(),
      params,
      false,
      false);

  auto str = [&](Type t) {
    std::string s;
    llvh::raw_string_ostream os{s};
    ctx.print(os, t);
    return os.str();
  };

  EXPECT_EQ(str(arrNum), "array(number)");
  EXPECT_EQ(str(fn), "function(string)");

  // A union element or return type prints its arms, using the same "any"
  // shorthand as a top-level union.
  Type numStr = ctx.unionTy(Type::createNumber(), Type::createString());
  EXPECT_EQ(str(ctx.createArray(numStr)), "array(string|number)");
  EXPECT_EQ(
      str(ctx.createArray(
          ctx.unionTy(Type::createAnyType(), Type::createEmpty()))),
      "array(any|empty)");
  Type fnNumStr = ctx.createFunction(
      numStr, Type::createNoType(), Type::createNoType(), {}, false, false);
  EXPECT_EQ(str(fnNumStr), "function(string|number)");
  // Parentheses keep a union payload apart from an enclosing union.
  EXPECT_EQ(
      str(ctx.unionTy(fnNumStr, Type::createNull())),
      "null|function(string|number)");
}

TEST_F(TypeContextTest, RecursiveTypePrint) {
  TypeContext ctx;

  auto str = [&](Type t) {
    std::string s;
    llvh::raw_string_ostream os{s};
    ctx.print(os, t);
    return os.str();
  };
  // Fill the reserved function \p f with return type \p ret and no params.
  auto fillFn = [&](Type f, Type ret) {
    ctx.fillFunction(
        f, ret, Type::createNoType(), Type::createNoType(), {}, false, false);
  };

  // type A = A[]
  Type a = ctx.reserveArray();
  ctx.fillArray(a, a);
  EXPECT_EQ(str(a), "array(^1)");

  // type F = () => F
  Type f = ctx.reserveFunction();
  fillFn(f, f);
  EXPECT_EQ(str(f), "function(^1)");

  // type G = () => G | null
  Type g = ctx.reserveFunction();
  fillFn(g, ctx.unionTy(g, Type::createNull()));
  EXPECT_EQ(str(g), "function(null|^1)");

  // type Json = number | string | Json[]. The element union refers to the
  // array, so the array prints its own arms with a back-reference.
  Type jsonArr = ctx.reserveArray();
  Type json = ctx.unionTy(
      ctx.unionTy(Type::createNumber(), Type::createString()), jsonArr);
  ctx.fillArray(jsonArr, json);
  EXPECT_EQ(str(json), "string|number|array(string|number|^1)");

  // type M = M[][]: the inner array refers back to the outer one, two levels
  // up. It must print differently from N[] where type N = N[], whose inner
  // array refers to itself.
  Type mOuter = ctx.reserveArray();
  Type mInner = ctx.createArray(mOuter);
  ctx.fillArray(mOuter, mInner);
  Type n = ctx.reserveArray();
  ctx.fillArray(n, n);
  Type arrN = ctx.createArray(n);
  EXPECT_EQ(str(mOuter), "array(array(^2))");
  EXPECT_EQ(str(arrN), "array(array(^1))");
  // Printing starts from the given type, so the inner entry of M prints
  // with its own back-reference depth.
  EXPECT_EQ(str(mInner), "array(array(^2))");

  // Mutual recursion through an array and a function:
  //   type P = Q[]; type Q = () => P
  Type p = ctx.reserveArray();
  Type q = ctx.reserveFunction();
  fillFn(q, p);
  ctx.fillArray(p, q);
  EXPECT_EQ(str(p), "array(function(^2))");
  EXPECT_EQ(str(q), "function(array(^2))");

  // A type that repeats a non-recursive entry on two arms is not a cycle and
  // prints it in full both times.
  Type arrNum = ctx.createArray(Type::createNumber());
  Type twice = ctx.createArray(
      ctx.unionTy(arrNum, ctx.createArray(ctx.createArray(arrNum))));
  EXPECT_EQ(str(twice), "array(array(number)|array(array(array(number))))");
}

TEST_F(TypeContextTest, TupleExactObjectAlgebra) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;
  Identifier x = strTab.getIdentifier("x");
  Identifier y = strTab.getIdentifier("y");
  Type num = Type::createNumber();
  Type str = Type::createString();
  Type obj = Type::createObject();

  Type tupNumStr = ctx.createTuple({num, str});
  Type tupStrNum = ctx.createTuple({str, num});
  Type objXY = ctx.createExactObject({{x, num}, {y, str}});
  Type objYX = ctx.createExactObject({{y, str}, {x, num}});
  // The other structural kinds and a class, for cross-kind checks.
  Type arrNum = ctx.createArray(num);
  Type fnVoid = ctx.createFunction(
      Type::createUndefined(),
      Type::createNoType(),
      Type::createNoType(),
      {},
      false,
      false);
  Type cls = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("C"), {});

  for (Type t : {tupNumStr, objXY}) {
    // Every refinement is a subtype of Object, and only of itself.
    EXPECT_TRUE(ctx.isSubsetOf(t, obj));
    EXPECT_FALSE(ctx.isSubsetOf(obj, t));
    EXPECT_TRUE(ctx.isSubsetOf(t, t));

    // Disjoint from every primitive, and from the primitive part of AnyType.
    for (Type prim :
         {num,
          str,
          Type::createBoolean(),
          Type::createNull(),
          Type::createUndefined(),
          Type::createBigInt(),
          Type::createSymbol(),
          Type::createInt32()}) {
      EXPECT_TRUE(ctx.areDisjoint(t, prim));
      EXPECT_TRUE(ctx.areDisjoint(prim, t));
      EXPECT_TRUE(ctx.intersectTy(t, prim).isNoType());
      EXPECT_EQ(ctx.subtractTy(t, prim), t);
    }
    EXPECT_TRUE(ctx.areDisjoint(t, ctx.unionTy(num, str)));
    EXPECT_FALSE(ctx.areDisjoint(t, obj));
    EXPECT_FALSE(ctx.areDisjoint(t, Type::createAnyType()));

    // Not a subtype of, and not disjoint from (D5), any other structural kind
    // or a class.
    for (Type other : {arrNum, fnVoid, cls}) {
      EXPECT_FALSE(ctx.isSubsetOf(t, other));
      EXPECT_FALSE(ctx.isSubsetOf(other, t));
      EXPECT_FALSE(ctx.areDisjoint(t, other));
      EXPECT_FALSE(ctx.areDisjoint(other, t));
    }

    // Union subsumption with Object.
    EXPECT_EQ(ctx.unionTy(t, obj), obj);
    EXPECT_EQ(ctx.unionTy(obj, t), obj);
    Type numOrObj = ctx.unionTy(num, obj);
    EXPECT_EQ(ctx.unionTy(t, numOrObj), numOrObj);
    EXPECT_TRUE(ctx.isSubsetOf(t, numOrObj));
    Type numOrT = ctx.unionTy(num, t);
    EXPECT_EQ(ctx.countKinds(numOrT), 2u);
    EXPECT_EQ(ctx.unionTy(numOrT, obj), numOrObj);

    // Intersection and subtraction with Object and with a union holding it.
    EXPECT_EQ(ctx.intersectTy(t, obj), t);
    EXPECT_EQ(ctx.intersectTy(t, t), t);
    EXPECT_EQ(ctx.intersectTy(numOrT, obj), t);
    EXPECT_EQ(ctx.intersectTy(numOrT, num), num);
    EXPECT_TRUE(ctx.subtractTy(t, obj).isNoType());
    EXPECT_EQ(ctx.subtractTy(numOrT, obj), num);
    EXPECT_EQ(ctx.subtractTy(numOrT, num), t);
  }

  // Shallow model (D4): a tuple or exact object is a subtype only of itself,
  // even when another one has the same element or field types.
  EXPECT_FALSE(ctx.isSubsetOf(tupNumStr, tupStrNum));
  EXPECT_FALSE(ctx.isSubsetOf(tupNumStr, ctx.createTuple({num})));
  EXPECT_FALSE(ctx.isSubsetOf(objXY, objYX));
  EXPECT_FALSE(ctx.isSubsetOf(objXY, ctx.createExactObject({{x, num}})));
  EXPECT_FALSE(ctx.isSubsetOf(tupNumStr, objXY));
  EXPECT_FALSE(ctx.isSubsetOf(objXY, tupNumStr));

  // Invariance, not covariance: comparable element or field types do not
  // make the tuples or exact objects comparable.
  Type numOrStr = ctx.unionTy(num, str);
  Type tupNum = ctx.createTuple({num});
  Type tupNumOrStr = ctx.createTuple({numOrStr});
  EXPECT_FALSE(ctx.isSubsetOf(tupNum, tupNumOrStr));
  EXPECT_FALSE(ctx.isSubsetOf(tupNumOrStr, tupNum));
  Type objXNum = ctx.createExactObject({{x, num}});
  Type objXNumOrStr = ctx.createExactObject({{x, numOrStr}});
  EXPECT_FALSE(ctx.isSubsetOf(objXNum, objXNumOrStr));
  EXPECT_FALSE(ctx.isSubsetOf(objXNumOrStr, objXNum));

  // Conservative disjointness (D5): two distinct tuples or exact objects, or
  // a tuple and an exact object, are not disjoint.
  EXPECT_FALSE(ctx.areDisjoint(tupNumStr, tupStrNum));
  EXPECT_FALSE(ctx.areDisjoint(objXY, objYX));
  EXPECT_FALSE(ctx.areDisjoint(tupNumStr, objXY));

  // A union of two distinct ones keeps both arms; Object subsumes both.
  Type tupOrObj = ctx.unionTy(tupNumStr, objXY);
  EXPECT_EQ(ctx.countKinds(tupOrObj), 2u);
  EXPECT_TRUE(ctx.isSubsetOf(tupNumStr, tupOrObj));
  EXPECT_TRUE(ctx.isSubsetOf(objXY, tupOrObj));
  EXPECT_TRUE(ctx.isSubsetOf(tupOrObj, obj));
  EXPECT_EQ(ctx.unionTy(tupOrObj, obj), obj);
}

/// D9: intersecting two leaves that overlap without a subtype relation
/// returns the first operand, so the result is a subset of the first operand
/// and the argument order matters. Exact cases are unaffected.
TEST_F(TypeContextTest, IntersectOverlappingUnrelatedReturnsFirst) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;
  Type num = Type::createNumber();
  Type str = Type::createString();
  Type null = Type::createNull();
  Type obj = Type::createObject();

  // (x: number|string) => number and (x: number) => number|string: the
  // function types of a value that Flow lets flow into the other.
  FunctionParam numOrStrParam[] = {
      {ctx.unionTy(num, str).getRawId(), /*isOptional*/ false}};
  FunctionParam numParam[] = {{num.getRawId(), /*isOptional*/ false}};
  Type f1 = ctx.createFunction(
      num,
      Type::createNoType(),
      Type::createNoType(),
      numOrStrParam,
      false,
      false);
  Type f2 = ctx.createFunction(
      ctx.unionTy(num, str),
      Type::createNoType(),
      Type::createNoType(),
      numParam,
      false,
      false);
  Type arr = ctx.createArray(num);
  Type tup = ctx.createTuple({num, str});
  Type animal = ctx.createClassInstance(
      Type::createNoType(), strTab.getIdentifier("Animal"), {});
  Type dog = ctx.createClassInstance(animal, strTab.getIdentifier("Dog"), {});
  Type cat = ctx.createClassInstance(animal, strTab.getIdentifier("Cat"), {});

  // Overlapping, unrelated leaves: the first operand wins, in either order.
  // Two functions, an array and a tuple, a class and a function.
  Type pairs[][2] = {{f1, f2}, {arr, tup}, {dog, f1}};
  for (auto [a, b] : pairs) {
    ASSERT_FALSE(ctx.areDisjoint(a, b));
    ASSERT_FALSE(ctx.isSubsetOf(a, b));
    ASSERT_FALSE(ctx.isSubsetOf(b, a));
    EXPECT_EQ(ctx.intersectTy(a, b), a);
    EXPECT_EQ(ctx.intersectTy(b, a), b);
  }

  // With unions, each arm of the first operand is kept, whichever operand is
  // the union.
  Type f1OrNull = ctx.unionTy(f1, null);
  Type f2OrNull = ctx.unionTy(f2, null);
  EXPECT_EQ(ctx.intersectTy(f2OrNull, f1OrNull), f2OrNull);
  EXPECT_EQ(ctx.intersectTy(f1OrNull, f2OrNull), f1OrNull);
  EXPECT_EQ(ctx.intersectTy(f2OrNull, f1), f2);
  EXPECT_EQ(ctx.intersectTy(f1, f2OrNull), f1);
  EXPECT_EQ(ctx.intersectTy(f2, ctx.unionTy(f1, arr)), f2);

  // When one operand is a subset of the other, the result is the smaller one,
  // even if an arm of the union operand overlaps the other operand without a
  // subtype relation.
  Type f1OrArr = ctx.unionTy(f1, arr);
  EXPECT_EQ(ctx.intersectTy(f1OrArr, arr), arr);
  EXPECT_EQ(ctx.intersectTy(arr, f1OrArr), arr);
  EXPECT_EQ(ctx.intersectTy(ctx.unionTy(dog, f1), dog), dog);

  // Exact cases are unchanged and symmetric: a subtype relation returns the
  // smaller type, Int32 and Uint32 meet at UInt31, disjoint pairs are empty.
  EXPECT_EQ(ctx.intersectTy(f1, obj), f1);
  EXPECT_EQ(ctx.intersectTy(obj, f1), f1);
  EXPECT_EQ(ctx.intersectTy(f1, f1), f1);
  EXPECT_EQ(ctx.intersectTy(animal, dog), dog);
  EXPECT_EQ(ctx.intersectTy(dog, animal), dog);
  EXPECT_EQ(
      ctx.intersectTy(asType(kInt32Id), asType(kUint32Id)), asType(kUInt31Id));
  EXPECT_EQ(
      ctx.intersectTy(asType(kUint32Id), asType(kInt32Id)), asType(kUInt31Id));
  EXPECT_TRUE(ctx.intersectTy(dog, cat).isNoType());
  EXPECT_TRUE(ctx.intersectTy(cat, dog).isNoType());
  EXPECT_TRUE(ctx.intersectTy(f1, num).isNoType());
  EXPECT_TRUE(ctx.intersectTy(num, f1).isNoType());
  EXPECT_EQ(ctx.intersectTy(f1OrNull, ctx.unionTy(num, null)), null);
  EXPECT_EQ(ctx.intersectTy(ctx.unionTy(num, null), f1OrNull), null);
}

TEST_F(TypeContextTest, TupleExactObjectPrint) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;
  Identifier x = strTab.getIdentifier("x");
  Identifier y = strTab.getIdentifier("y");
  Type num = Type::createNumber();
  Type str = Type::createString();
  Type numStr = ctx.unionTy(num, str);

  auto print = [&](Type t) {
    std::string s;
    llvh::raw_string_ostream os{s};
    ctx.print(os, t);
    return os.str();
  };

  EXPECT_EQ(print(ctx.createTuple({num, str})), "tuple(number, string)");
  EXPECT_EQ(print(ctx.createTuple({num})), "tuple(number)");
  EXPECT_EQ(print(ctx.createTuple({})), "tuple()");
  EXPECT_EQ(
      print(ctx.createExactObject({{x, num}, {y, str}})),
      "exactObject(x: number, y: string)");
  // Fields print in field order.
  EXPECT_EQ(
      print(ctx.createExactObject({{y, str}, {x, num}})),
      "exactObject(y: string, x: number)");
  EXPECT_EQ(print(ctx.createExactObject({})), "exactObject()");

  // Elements and field types print their union arms, with the same "any"
  // shorthand as a top-level union.
  EXPECT_EQ(
      print(ctx.createTuple({numStr, Type::createNull()})),
      "tuple(string|number, null)");
  EXPECT_EQ(
      print(ctx.createExactObject(
          {{x, numStr},
           {y, ctx.unionTy(Type::createAnyType(), Type::createEmpty())}})),
      "exactObject(x: string|number, y: any|empty)");

  // Nested structural payloads, and a structural type as a union arm.
  Type fnStr = ctx.createFunction(
      str, Type::createNoType(), Type::createNoType(), {}, false, false);
  Type nested = ctx.createTuple(
      {ctx.createArray(num), ctx.createExactObject({{x, fnStr}})});
  EXPECT_EQ(
      print(nested), "tuple(array(number), exactObject(x: function(string)))");
  EXPECT_EQ(
      print(ctx.unionTy(nested, Type::createNull())),
      "null|tuple(array(number), exactObject(x: function(string)))");
}

TEST_F(TypeContextTest, ExactObjectPrintQuotesNonIdentifierNames) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;
  Type num = Type::createNumber();
  auto print = [&](Type t) {
    std::string s;
    llvh::raw_string_ostream os{s};
    ctx.print(os, t);
    return os.str();
  };
  auto field = [&](const char *name) {
    return ExactObjectField{strTab.getIdentifier(name), num};
  };

  // ASCII identifiers print as they are.
  EXPECT_EQ(
      print(ctx.createExactObject({field("$_a1"), field("Z")})),
      "exactObject($_a1: number, Z: number)");
  // Other keys print quoted, with backslash escapes.
  EXPECT_EQ(
      print(ctx.createExactObject({field("a-b"), field("1x"), field("")})),
      "exactObject(\"a-b\": number, \"1x\": number, \"\": number)");
  EXPECT_EQ(
      print(ctx.createExactObject({field("q\"\\\n")})),
      "exactObject(\"q\\\"\\\\\\n\": number)");
}

TEST_F(TypeContextTest, RecursiveTupleExactObjectPrint) {
  hermes::BumpPtrAllocator alloc;
  hermes::StringTable strTab{alloc};
  TypeContext ctx;
  Identifier left = strTab.getIdentifier("left");
  Identifier right = strTab.getIdentifier("right");
  Identifier kids = strTab.getIdentifier("kids");
  Identifier f = strTab.getIdentifier("f");
  Type num = Type::createNumber();

  auto print = [&](Type t) {
    std::string s;
    llvh::raw_string_ostream os{s};
    ctx.print(os, t);
    return os.str();
  };

  // type T = [number, T]
  Type t = ctx.reserveTuple();
  ctx.fillTuple(t, {num, t});
  EXPECT_EQ(print(t), "tuple(number, ^1)");

  // type Tree = null | {left: Tree, right: Tree}
  Type node = ctx.reserveExactObject();
  Type tree = ctx.unionTy(Type::createNull(), node);
  ctx.fillExactObject(node, {{left, tree}, {right, tree}});
  EXPECT_EQ(print(node), "exactObject(left: null|^1, right: null|^1)");
  EXPECT_EQ(print(tree), "null|exactObject(left: null|^1, right: null|^1)");

  // type U = [number, U[]]: the back-reference skips the enclosing array.
  Type u = ctx.reserveTuple();
  ctx.fillTuple(u, {num, ctx.createArray(u)});
  EXPECT_EQ(print(u), "tuple(number, array(^2))");

  // type O = {kids: O[]}
  Type o = ctx.reserveExactObject();
  ctx.fillExactObject(o, {{kids, ctx.createArray(o)}});
  EXPECT_EQ(print(o), "exactObject(kids: array(^2))");

  // A cycle through all four structural kinds: N counts each of them.
  //   type A = [{f: () => A}][]
  Type fn = ctx.reserveFunction();
  Type arr =
      ctx.createArray(ctx.createTuple({ctx.createExactObject({{f, fn}})}));
  ctx.fillFunction(
      fn, arr, Type::createNoType(), Type::createNoType(), {}, false, false);
  EXPECT_EQ(print(arr), "array(tuple(exactObject(f: function(^4))))");
  EXPECT_EQ(print(fn), "function(array(tuple(exactObject(f: ^4))))");

  // A non-recursive tuple repeated on two arms is not a cycle and prints in
  // full both times.
  Type tupNum = ctx.createTuple({num});
  EXPECT_EQ(
      print(ctx.createTuple({tupNum, tupNum})),
      "tuple(tuple(number), tuple(number))");
}

/// Fixture for the totality tests of the type algebra. It builds a sample of
/// types that covers every leaf kind, every refined kind (including a
/// self-referential entry of each), and unions that mix them.
/// The tests check algebraic laws over all ordered pairs of the sample instead
/// of hand-written result tables, so a new kind or rule is checked against
/// every other one.
class TypeAlgebraTotalityTest : public TypeContextTest {
 protected:
  /// A sample type and the label that names it in failure messages.
  struct Sample {
    const char *name;
    Type type;
  };

  /// Build the sample types in ctx_ and list them in samples_.
  TypeAlgebraTotalityTest();

  /// \return "(a, b)" with the labels of \p a and \p b, for failure messages.
  static std::string pairName(const Sample &a, const Sample &b);

  /// \return \p t printed by the context.
  std::string print(Type t) const;

  /// \return true if \p t is a leaf of a refined kind.
  bool isRefinedLeaf(Type t) const;

  /// \return true if \p t is or has an arm of a refined kind.
  bool hasRefinedArm(Type t) const;

  /// \return true if leaves \p x and \p y are a pair that D9 covers: two
  /// distinct structural refined types, or a class and a structural refined
  /// type, that overlap without either being a subtype of the other.
  bool isD9LeafPair(Type x, Type y) const;

  /// \return true if some arm of \p a and some arm of \p b form a D9 pair.
  bool hasD9ArmPair(Type a, Type b) const;

  /// \return true if D9 exempts the pair (\p a, \p b) from the law
  /// `isSubsetOf(intersectTy(a, b), b)`. intersectTy returns b when b is a
  /// subset of a. Otherwise it intersects arm by arm, and each arm-by-arm
  /// result is the exact meet (a subset of b) except for a D9 arm pair (x, y),
  /// which yields the arm x of a. So the result escapes b exactly when b is
  /// not a subset of a and such an x is itself not a subset of b. Such an x
  /// survives the subsumption in the result union: anything that could
  /// subsume it is either a subset of b or another arm of a, and the arms of
  /// a canonical union do not subsume each other.
  bool isD9Exempt(Type a, Type b) const;

  /// The allocator behind strTab_.
  hermes::BumpPtrAllocator alloc_;
  /// Names of the classes and fields in the sample.
  hermes::StringTable strTab_{alloc_};
  /// The context that owns every sample type.
  TypeContext ctx_;
  /// The sample, in construction order.
  std::vector<Sample> samples_;
};

TypeAlgebraTotalityTest::TypeAlgebraTotalityTest() {
  Type none = Type::createNoType();
  Type num = Type::createNumber();
  Type str = Type::createString();
  Type null = Type::createNull();
  // Type::createInt32() and Type::createUint32() return Number.
  Type int32 = asType(kInt32Id);
  Type uint32 = asType(kUint32Id);
  Identifier x = strTab_.getIdentifier("x");
  Identifier y = strTab_.getIdentifier("y");
  Identifier next = strTab_.getIdentifier("next");
  Identifier left = strTab_.getIdentifier("left");
  Identifier right = strTab_.getIdentifier("right");

  // Classes: Dog extends Animal; Plant is unrelated to both.
  //   class Node { next: Node | null }
  Type animal =
      ctx_.createClassInstance(none, strTab_.getIdentifier("Animal"), {});
  Type dog = ctx_.createClassInstance(animal, strTab_.getIdentifier("Dog"), {});
  Type plant =
      ctx_.createClassInstance(none, strTab_.getIdentifier("Plant"), {});
  Type node = ctx_.reserveClassInstance(none);
  ctx_.fillClassInstance(
      node, strTab_.getIdentifier("Node"), {{next, ctx_.unionTy(node, null)}});

  FunctionParam numParam[] = {{num.getRawId(), /*isOptional*/ false}};
  Type arrNum = ctx_.createArray(num);
  Type arrStr = ctx_.createArray(str);
  Type fnNum = ctx_.createFunction(num, none, none, {}, false, false);
  Type fnStr = ctx_.createFunction(str, none, none, numParam, false, false);
  Type tupNumStr = ctx_.createTuple({num, str});
  Type tupStrNum = ctx_.createTuple({str, num});
  Type objXY = ctx_.createExactObject({{x, num}, {y, str}});
  Type objYX = ctx_.createExactObject({{y, str}, {x, num}});

  // Self-referential structural entries, built by reserve and fill:
  //   type A = A[]
  //   type F = (x: number) => F | null
  //   type T = [number, T]
  //   type Tree = null | {left: Tree, right: Tree}
  Type selfArr = ctx_.reserveArray();
  ctx_.fillArray(selfArr, selfArr);
  Type selfFn = ctx_.reserveFunction();
  ctx_.fillFunction(
      selfFn, ctx_.unionTy(selfFn, null), none, none, numParam, false, false);
  Type selfTup = ctx_.reserveTuple();
  ctx_.fillTuple(selfTup, {num, selfTup});
  Type treeNode = ctx_.reserveExactObject();
  Type tree = ctx_.unionTy(null, treeNode);
  ctx_.fillExactObject(treeNode, {{left, tree}, {right, tree}});

  samples_ = {
      {"notype", none},
      {"empty", Type::createEmpty()},
      {"uninit", Type::createUninit()},
      {"undefined", Type::createUndefined()},
      {"null", null},
      {"boolean", Type::createBoolean()},
      {"string", str},
      {"bigint", Type::createBigInt()},
      {"symbol", Type::createSymbol()},
      {"number", num},
      {"int32", int32},
      {"uint32", uint32},
      {"uint31", asType(kUInt31Id)},
      {"environment", Type::createEnvironment()},
      {"privateName", Type::createPrivateName()},
      {"functionCode", Type::createFunctionCode()},
      {"bits32", asType(kBits32Id)},
      {"object", Type::createObject()},
      {"Animal", animal},
      {"Dog", dog},
      {"Plant", plant},
      {"Node (self-referential)", node},
      {"array(number)", arrNum},
      {"array(string)", arrStr},
      {"function(number)", fnNum},
      {"function(string)", fnStr},
      {"tuple(number, string)", tupNumStr},
      {"tuple(string, number)", tupStrNum},
      {"exactObject(x, y)", objXY},
      {"exactObject(y, x)", objYX},
      {"A = A[]", selfArr},
      {"F = () => F|null", selfFn},
      {"T = [number, T]", selfTup},
      {"Tree node", treeNode},
      {"Tree = null|node", tree},
      {"array(number)|function(number)", ctx_.unionTy(arrNum, fnNum)},
      {"tuple(number, string)|null", ctx_.unionTy(tupNumStr, null)},
      {"Dog|function(string)", ctx_.unionTy(dog, fnStr)},
      {"number|string", ctx_.unionTy(num, str)},
      {"int32|uint32", ctx_.unionTy(int32, uint32)},
      {"bits32|environment",
       ctx_.unionTy(asType(kBits32Id), Type::createEnvironment())},
      {"any", Type::createAnyType()},
  };
}

std::string TypeAlgebraTotalityTest::pairName(
    const Sample &a,
    const Sample &b) {
  return std::string("(") + a.name + ", " + b.name + ")";
}

std::string TypeAlgebraTotalityTest::print(Type t) const {
  std::string s;
  llvh::raw_string_ostream os{s};
  ctx_.print(os, t);
  return os.str();
}

bool TypeAlgebraTotalityTest::isRefinedLeaf(Type t) const {
  TypeKind k = ctx_.getKind(t);
  return k >= TypeKind::_RefinedFirst && k <= TypeKind::_RefinedLast;
}

bool TypeAlgebraTotalityTest::hasRefinedArm(Type t) const {
  for (Type arm : ctx_.arms(t)) {
    if (isRefinedLeaf(arm))
      return true;
  }
  return false;
}

bool TypeAlgebraTotalityTest::isD9LeafPair(Type x, Type y) const {
  if (!isRefinedLeaf(x) || !isRefinedLeaf(y))
    return false;
  // Two classes are related or disjoint (D5), never a D9 pair.
  if (ctx_.getKind(x) == TypeKind::ClassInstance &&
      ctx_.getKind(y) == TypeKind::ClassInstance)
    return false;
  return !ctx_.areDisjoint(x, y) && !ctx_.isSubsetOf(x, y) &&
      !ctx_.isSubsetOf(y, x);
}

bool TypeAlgebraTotalityTest::hasD9ArmPair(Type a, Type b) const {
  for (Type x : ctx_.arms(a)) {
    for (Type y : ctx_.arms(b)) {
      if (isD9LeafPair(x, y))
        return true;
    }
  }
  return false;
}

bool TypeAlgebraTotalityTest::isD9Exempt(Type a, Type b) const {
  if (ctx_.isSubsetOf(b, a))
    return false;
  for (Type x : ctx_.arms(a)) {
    if (ctx_.isSubsetOf(x, b))
      continue;
    for (Type y : ctx_.arms(b)) {
      if (isD9LeafPair(x, y))
        return true;
    }
  }
  return false;
}

TEST_F(TypeAlgebraTotalityTest, UnionContainsOperandsAndCommutes) {
  for (const Sample &a : samples_) {
    for (const Sample &b : samples_) {
      Type u = ctx_.unionTy(a.type, b.type);
      EXPECT_TRUE(ctx_.isSubsetOf(a.type, u)) << pairName(a, b);
      EXPECT_TRUE(ctx_.isSubsetOf(b.type, u)) << pairName(a, b);
      EXPECT_EQ(u, ctx_.unionTy(b.type, a.type)) << pairName(a, b);
    }
  }
}

TEST_F(TypeAlgebraTotalityTest, IntersectionIsSubsetOfFirstOperand) {
  for (const Sample &a : samples_) {
    for (const Sample &b : samples_) {
      EXPECT_TRUE(ctx_.isSubsetOf(ctx_.intersectTy(a.type, b.type), a.type))
          << pairName(a, b);
    }
  }
}

/// `isSubsetOf(intersectTy(a, b), b)` holds for every pair except exactly the
/// ones D9 exempts (see isD9Exempt), and in particular whenever one operand
/// is a subset of the other or neither has a refined arm.
TEST_F(TypeAlgebraTotalityTest, IntersectionIsSubsetOfSecondOperandExceptD9) {
  unsigned exempt = 0;
  for (const Sample &a : samples_) {
    for (const Sample &b : samples_) {
      bool holds = ctx_.isSubsetOf(ctx_.intersectTy(a.type, b.type), b.type);
      bool isExempt = isD9Exempt(a.type, b.type);
      exempt += isExempt;
      EXPECT_EQ(holds, !isExempt) << pairName(a, b);
      if (ctx_.isSubsetOf(a.type, b.type) || ctx_.isSubsetOf(b.type, a.type) ||
          (!hasRefinedArm(a.type) && !hasRefinedArm(b.type))) {
        EXPECT_TRUE(holds) << pairName(a, b);
      }
    }
  }
  // The exemption is not vacuous: the sample has D9 pairs.
  EXPECT_GT(exempt, 0u);
}

/// D9: two leaves that overlap without a subtype relation are either a D9
/// pair, whose intersection is the first operand, or Int32 and Uint32, which
/// meet at UInt31. Every other leaf pair intersects exactly and symmetrically,
/// and so does every pair of types that has no D9 arm pair or where one type
/// is a subset of the other. When one type is a subset of the other, the
/// intersection is exactly the smaller type.
TEST_F(TypeAlgebraTotalityTest, IntersectionIsExactExceptD9) {
  Type int32 = asType(kInt32Id);
  Type uint32 = asType(kUint32Id);
  for (const Sample &a : samples_) {
    for (const Sample &b : samples_) {
      Type ab = ctx_.intersectTy(a.type, b.type);
      if (!hasD9ArmPair(a.type, b.type) || ctx_.isSubsetOf(a.type, b.type) ||
          ctx_.isSubsetOf(b.type, a.type)) {
        EXPECT_TRUE(ctx_.isSubsetOf(ab, b.type)) << pairName(a, b);
        EXPECT_EQ(ab, ctx_.intersectTy(b.type, a.type)) << pairName(a, b);
      }
      if (ctx_.isSubsetOf(a.type, b.type)) {
        EXPECT_EQ(ab, a.type) << pairName(a, b);
      } else if (ctx_.isSubsetOf(b.type, a.type)) {
        EXPECT_EQ(ab, b.type) << pairName(a, b);
      }
      if (ctx_.countKinds(a.type) != 1 || ctx_.countKinds(b.type) != 1 ||
          ctx_.areDisjoint(a.type, b.type) || ctx_.isSubsetOf(a.type, b.type) ||
          ctx_.isSubsetOf(b.type, a.type))
        continue;
      if (isD9LeafPair(a.type, b.type)) {
        EXPECT_EQ(ab, a.type) << pairName(a, b);
      } else {
        EXPECT_TRUE(
            (a.type == int32 && b.type == uint32) ||
            (a.type == uint32 && b.type == int32))
            << pairName(a, b);
        EXPECT_EQ(ab, asType(kUInt31Id)) << pairName(a, b);
      }
    }
  }
}

TEST_F(TypeAlgebraTotalityTest, DisjointIffIntersectionIsEmpty) {
  for (const Sample &a : samples_) {
    for (const Sample &b : samples_) {
      bool disjoint = ctx_.areDisjoint(a.type, b.type);
      EXPECT_EQ(disjoint, ctx_.areDisjoint(b.type, a.type)) << pairName(a, b);
      EXPECT_EQ(disjoint, ctx_.intersectTy(a.type, b.type).isNoType())
          << pairName(a, b);
    }
  }
}

TEST_F(TypeAlgebraTotalityTest, SubsetIsPartialOrder) {
  size_t n = samples_.size();
  std::vector<std::vector<bool>> sub(n, std::vector<bool>(n));
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j)
      sub[i][j] = ctx_.isSubsetOf(samples_[i].type, samples_[j].type);
  }
  for (size_t i = 0; i < n; ++i) {
    EXPECT_TRUE(sub[i][i]) << samples_[i].name;
    for (size_t j = 0; j < n; ++j) {
      // Antisymmetric: mutual subsets are the same canonical type.
      if (sub[i][j] && sub[j][i]) {
        EXPECT_EQ(samples_[i].type, samples_[j].type)
            << pairName(samples_[i], samples_[j]);
      }
      if (!sub[i][j])
        continue;
      for (size_t k = 0; k < n; ++k) {
        if (sub[j][k]) {
          EXPECT_TRUE(sub[i][k]) << pairName(samples_[i], samples_[j])
                                 << " <: " << samples_[k].name;
        }
      }
    }
  }
}

TEST_F(TypeAlgebraTotalityTest, SubtractionIsSubsetOfMinuend) {
  for (const Sample &a : samples_) {
    EXPECT_TRUE(ctx_.subtractTy(a.type, a.type).isNoType()) << a.name;
    for (const Sample &b : samples_) {
      EXPECT_TRUE(ctx_.isSubsetOf(ctx_.subtractTy(a.type, b.type), a.type))
          << pairName(a, b);
    }
  }
}

/// Printing terminates on every sample type, including the self-referential
/// ones, and on every result the binary operations produce from the sample.
TEST_F(TypeAlgebraTotalityTest, PrintingTerminates) {
  for (const Sample &a : samples_) {
    EXPECT_FALSE(print(a.type).empty()) << a.name;
    for (const Sample &b : samples_) {
      for (Type r :
           {ctx_.unionTy(a.type, b.type),
            ctx_.intersectTy(a.type, b.type),
            ctx_.subtractTy(a.type, b.type)}) {
        EXPECT_FALSE(print(r).empty()) << pairName(a, b);
      }
    }
  }
}

} // anonymous namespace
