# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(OptimizerSources
  Scalar/InstructionEscapeAnalysisTest.cpp
  Scalar/LowerBuiltinCallsTest.cpp
  PassManager/PassManagerTest.cpp
  )

add_hermes_unittest(HermesOptimizerTests
  ${OptimizerSources}
  )

target_link_libraries(HermesOptimizerTests
 hermesOptimizer
 hermesFrontend
 )
