/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSRuntime.h"

#include <hermes/CompileJS.h>
#include <hermes/hermes.h> // @manual=fbsource//xplat/hermes/API:HermesAPI

#include <MappedFileBuffer.h>

#include <string>

namespace facebook {
namespace jsi {
namespace jni {

jni::local_ref<JSRuntime::jhybridobject> JSRuntime::makeHermesRuntime(
    jni::alias_ref<jclass>,
    bool shouldRecordGCStats) {
  return newObjectCxxArgs(
      hermes::makeHermesRuntime(
          ::hermes::vm::RuntimeConfig::Builder()
              .withIntl(true)
              .withGCConfig(
                  ::hermes::vm::GCConfig::Builder()
                      .withShouldRecordStats(shouldRecordGCStats)
                      .build())
              .build()));
}

jni::local_ref<JSRuntime::jhybridobject>
JSRuntime::makeHermesRuntimeWithHeapSpec(
    jni::alias_ref<jclass>,
    jlong initHeapSize,
    jlong maxHeapSize,
    bool shouldRecordGCStats) {
  return newObjectCxxArgs(
      hermes::makeHermesRuntime(
          ::hermes::vm::RuntimeConfig::Builder()
              .withGCConfig(
                  ::hermes::vm::GCConfig::Builder()
                      .withInitHeapSize(initHeapSize)
                      .withMaxHeapSize(maxHeapSize)
                      .withShouldRecordStats(shouldRecordGCStats)
                      .build())
              .build()));
}

jni::local_ref<jbyteArray> JSRuntime::compileJavaScript(
    jni::alias_ref<jclass>,
    jni::alias_ref<jbyteArray> bytes) {
  auto bytesPrimArr = bytes->pin();
  std::string str(
      reinterpret_cast<char *>(bytesPrimArr.get()), bytesPrimArr.size());
  bytesPrimArr.release();
  std::string bytecode;
  ::hermes::compileJS(str, bytecode);
  if (bytecode.empty()) {
    throw std::invalid_argument("Invalid source code given");
  }
  jni::local_ref<jbyteArray> result = jni::make_byte_array(bytecode.size());
  result->setRegion(
      0, bytecode.size(), reinterpret_cast<const jbyte *>(bytecode.data()));
  return result;
}

} // namespace jni
} // namespace jsi
} // namespace facebook
