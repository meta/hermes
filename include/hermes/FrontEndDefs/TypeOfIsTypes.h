/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_FRONTENDDEFS_TYPEOFISTYPES_H
#define HERMES_FRONTENDDEFS_TYPEOFISTYPES_H

/// The types matched by the TypeOfIs IR and bytecode instructions. Type \c i
/// in this list is bit \c i of the raw TypeOfIsTypes value.
/// Shared by the compiler and the C code emitted by the native backend, so it
/// must remain valid C.
///
/// WARNING: Must update the HBC version if this is changed.
#define HERMES_TYPEOF_IS_TYPES           \
  HERMES_TYPEOF_IS_TYPES_TYPE(Undefined) \
  HERMES_TYPEOF_IS_TYPES_TYPE(Object)    \
  HERMES_TYPEOF_IS_TYPES_TYPE(String)    \
  HERMES_TYPEOF_IS_TYPES_TYPE(Symbol)    \
  HERMES_TYPEOF_IS_TYPES_TYPE(Boolean)   \
  HERMES_TYPEOF_IS_TYPES_TYPE(Number)    \
  HERMES_TYPEOF_IS_TYPES_TYPE(Bigint)    \
  HERMES_TYPEOF_IS_TYPES_TYPE(Function)  \
  HERMES_TYPEOF_IS_TYPES_TYPE(Null)

#endif
