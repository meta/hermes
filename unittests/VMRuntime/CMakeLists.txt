# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Some sources here have conflicting names for free functions, so disable unity
# builds if they have been globally enabled.
set(CMAKE_UNITY_BUILD OFF)

# Some tests are disabled due to needing HBC to run in Static Hermes.
set(RTSources
  AlignedHeapSegmentTest.cpp
  ArrayTest.cpp
  ArrayStorageTest.cpp
  Base64UtilTest.cpp
  BigIntPrimitiveTest.cpp
  BCProviderTest.cpp
  CallResultTest.cpp
  CardObjectBoundaryNCTest.cpp
  CardTableNCTest.cpp
  CastingTest.cpp
  CodeCoverageProfilerTest.cpp
  CopyableVectorTest.cpp
  CrashManagerTest.cpp
  DateUtilTest.cpp
  DecoratedObjectTest.cpp
  DictPropertyMapTest.cpp
  FPContractTest.cpp
  GCBasicsTest.cpp
  GCFinalizerTest.cpp
  GCFragmentationTest.cpp
  GCInitTest.cpp
  GCLazySegmentNCTest.cpp
  GCObjectIterationTest.cpp
  GCOOMTest.cpp
  GCReturnUnusedMemoryTest.cpp
  GCSanitizeHandlesTest.cpp
  HeapSnapshotTest.cpp
  HermesValueTest.cpp
  HiddenClassTest.cpp
  IdentifierTableTest.cpp
  InstrumentationAPITest.cpp
  InternalPropertiesTest.cpp
  InterpreterTest.cpp
  JitChainWalkTest.cpp
  JSLibTest.cpp
  JSONParseTest.cpp
  JSTypedArrayTest.cpp
  NativeFrameTest.cpp
  NativeFunctionTest.cpp
  NativeStateTest.cpp
  MarkBitArrayNCTest.cpp
  ObjectModelTest.cpp
  OperationsTest.cpp
  OrderedHashMapTest.cpp
  PredefinedStringsTest.cpp
  PropertyCacheTest.cpp
  ProfileGeneratorTest.cpp
  HandleTest.cpp
  RegExpBytecodeTest.cpp
  RuntimeConfigTest.cpp
  SamplingHeapProfilerTest.cpp
  SamplingProfilerTest.cpp
  SmallHermesValueTest.cpp
  SmallXStringTest.cpp
  StaticBuiltinsTest.cpp
  StackTracesTreeTest.cpp
  StorageProviderTest.cpp
  StringBuilderTest.cpp
  StringPrimitiveTest.cpp
  StringViewTest.cpp
  SymbolIDTest.cpp
  VMRuntimeTestHelpers.cpp VMRuntimeTestHelpers.h
  TestHelpers1.cpp TestHelpers1.h
  TwineChar16Test.cpp
  WeakValueMapTest.cpp
  WeakValueObjectMapTest.cpp
  MetadataTest.cpp
  FinalizationRegistryTest.cpp
  )

add_hermes_unittest(HermesVMRuntimeTests
  ${RTSources}
  LINK_LIBS hermesvm_a
  )

add_subdirectory(Instrumentation)

if(HERMESVM_ALLOW_JIT)
  add_subdirectory(JIT)
endif()
