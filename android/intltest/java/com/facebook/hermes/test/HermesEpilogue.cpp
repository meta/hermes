/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HermesEpilogue.h"

#include <hermes/hermes.h> // @manual=fbsource//xplat/hermes/API:HermesAPI

#include <MappedFileBuffer.h>

namespace facebook {
namespace jsi {
namespace jni {

static jni::local_ref<jbyteArray> metadataParserImpl(
    const uint8_t *data,
    size_t size) {
  auto *hermesAPI =
      castInterface<hermes::IHermesRootAPI>(hermes::makeHermesRootAPI());
  std::pair<const uint8_t *, size_t> epiPair =
      hermesAPI->getBytecodeEpilogue(data, size);
  size_t epilogueSize = epiPair.second;
  const uint8_t *epilogue = epiPair.first;
  if (epilogueSize == 0) {
    // No data after HBC content.
    return jni::JArrayByte::newArray(0);
  } else {
    jni::local_ref<jbyteArray> res = jni::JArrayByte::newArray(epilogueSize);
    auto resPtr = res->pin();

    // copy epilogue into java land
    memcpy(reinterpret_cast<uint8_t *>(resPtr.get()), epilogue, epilogueSize);
    return res;
  }
}

jni::local_ref<jbyteArray> HermesEpilogue::getHermesBytecodeMetadata(
    jni::alias_ref<jclass>,
    jni::alias_ref<jbyteArray> jByteArray) {
  auto bytesPrimArr = jByteArray->pin();
  size_t fileSize = bytesPrimArr.size();
  const auto bytes = reinterpret_cast<const uint8_t *>(bytesPrimArr.get());
  return metadataParserImpl(bytes, fileSize);
}

jni::local_ref<jbyteArray> HermesEpilogue::getHermesBCFileMetadata(
    jni::alias_ref<jclass>,
    std::string filename) {
  MappedFileBuffer buf{filename};
  return metadataParserImpl(buf.data(), buf.size());
}

void HermesEpilogue::registerNatives() {
  javaClassLocal()->registerNatives({
      makeNativeMethod(
          "getHermesBytecodeMetadata",
          HermesEpilogue::getHermesBytecodeMetadata),
      makeNativeMethod(
          "getHermesBCFileMetadata", HermesEpilogue::getHermesBCFileMetadata),
  });
}

} // namespace jni
} // namespace jsi
} // namespace facebook
