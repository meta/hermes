# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(ADTSources
  BitArrayTest.cpp
  CompactArrayTest.cpp
  ConsumableRangeTest.cpp
  ManagedChunkedListTest.cpp
  SafeIntTest.cpp
  ScopedHashTable.cpp
  PersistentScopedMapTest.cpp
  WordBitSet.cpp
  OwningFoldingSet.cpp
  TransparentConservativeVectorTest.cpp
  )

add_hermes_unittest(HermesADTTests
  ${ADTSources}
  )

target_link_libraries(HermesADTTests
  hermesADT
  )
