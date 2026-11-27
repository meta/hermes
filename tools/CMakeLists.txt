# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

hermes_enable_werror_undef()

add_subdirectory(hermes)
add_subdirectory(hermesc)
add_subdirectory(hdb)
add_subdirectory(hbcdump)
add_subdirectory(hvm)
add_subdirectory(hvm-bench)
add_subdirectory(hbc-diff)
add_subdirectory(hbc-deltaprep)
add_subdirectory(hbc-attribute)
add_subdirectory(jsi)
if(HERMES_ENABLE_NAPI)
  add_subdirectory(napi-runner)
endif()
add_subdirectory(emhermesc)
add_subdirectory(fuzzers)
add_subdirectory(dependency-extractor)
add_subdirectory(hermes-parser)
add_subdirectory(shermes)
add_subdirectory(synth)
add_subdirectory(hcdp)

add_subdirectory(test-runner)
