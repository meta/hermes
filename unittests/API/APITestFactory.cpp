/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/hermes.h> // @manual=fbsource//xplat/hermes/API:HermesAPI
#include <hermes_abi/HermesABIRuntimeWrapper.h>
#include <hermes_abi/hermes_vtable.h>
#include <hermes_sandbox/HermesSandboxRuntime.h>
#include <jsi/test/testlib.h>
#include <jsi/threadsafe.h>

using namespace facebook::hermes;

namespace facebook {
namespace jsi {

std::vector<RuntimeFactory> runtimeGenerators() {
  return {
      [] {
        return makeHermesRuntime(
            ::hermes::vm::RuntimeConfig::Builder()
                .withMicrotaskQueue(true)
                .build());
      },
      [] { return makeThreadSafeHermesRuntime(); },
      [] { return makeHermesABIRuntimeWrapper(get_hermes_abi_vtable()); },
      [] { return makeHermesSandboxRuntime(); }};
}

} // namespace jsi
} // namespace facebook
