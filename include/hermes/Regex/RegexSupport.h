/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_REGEX_REGEXSUPPORT_H
#define HERMES_REGEX_REGEXSUPPORT_H

#include "llvh/ADT/ArrayRef.h"
#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/SmallVector.h"

namespace hermes {
namespace regex {
using GroupName = llvh::SmallVector<char16_t, 5>;
// The match array indices of every capture group using one name. A name is
// used by more than one group only with ES2025 duplicate named capture groups.
using GroupNums = llvh::SmallVector<uint32_t, 1>;
// A map from identifier -> match array indices
// /(?<id_foo>a)(b)(?<id_bar>c)/
// would be represented as { id_foo -> [1], id_bar -> [3] }
// /(?<id_foo>a)|(?<id_foo>b)(?<id_bar>c)/
// would be represented as { id_foo -> [1, 2], id_bar -> [3] }
using ParsedGroupNamesMapping =
    llvh::DenseMap<llvh::ArrayRef<char16_t>, GroupNums>;
} // namespace regex
} // namespace hermes

#endif // HERMES_REGEX_REGEXSUPPORT_H
