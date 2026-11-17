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

TEST_F(TypeContextTest, ArrayReserveFillSelfReference) {
  TypeContext ctx;
  // Simulate a recursive array whose element is the array itself:
  //   type A = Array<A>
  Type a = ctx.reserveArray();
  ctx.fillArray(a, a);
  EXPECT_EQ(ctx.getKind(a), TypeKind::Array);
  EXPECT_EQ(ctx.getArrayElementType(a), a);
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

  // Differ by isAsync => different id.
  Type fnAsync = ctx.createFunction(
      Type::createBoolean(),
      Type::createObject(),
      Type::createNoType(),
      params,
      /*isAsync*/ true,
      false);
  EXPECT_NE(fn, fnAsync);

  // Differ by a param optional flag => different id.
  FunctionParam params2[] = {
      {Type::createNumber().getRawId(), false},
      {Type::createString().getRawId(), /*isOptional*/ false}};
  Type fn3 = ctx.createFunction(
      Type::createBoolean(),
      Type::createObject(),
      Type::createNoType(),
      params2,
      false,
      false);
  EXPECT_NE(fn, fn3);
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
  // Unfilled: the payloads still hold NoType.
  ASSERT_TRUE(ctx.getArrayElementType(reservedArr).isNoType());
  ASSERT_TRUE(ctx.getFunctionReturn(reservedFn).isNoType());

  for (Type r : {reservedArr, reservedFn}) {
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

} // anonymous namespace
