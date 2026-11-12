/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SH_CONFIG_H
#define HERMES_SH_CONFIG_H

#include "libhermesvm-config.h"

/// Number of property slots allocated directly inside the object.
enum { HERMESVM_DIRECT_PROPERTY_SLOTS = 6 };

#ifndef __cplusplus
// uchar.h is not universally available, so just define our own.
typedef uint16_t char16_t;
#endif

#ifndef SHERMES_EXPORT
#ifdef _MSC_VER
#define SHERMES_EXPORT __declspec(dllexport)
#else // _MSC_VER
#define SHERMES_EXPORT __attribute__((visibility("default")))
#endif // _MSC_VER
#endif // !defined(HERMES_EXPORT)

/// \macro SHERMES_NO_SANITIZE
/// Disable a particular sanitizer for a function.
// __has_attribute(...) needs an #if of its own: MSVC lacks it, and "&&"
// after defined(__has_attribute) skips evaluating it, not parsing it, so
// MSVC would warn C4067.
#ifdef __has_attribute
#if __has_attribute(no_sanitize)
#define SHERMES_NO_SANITIZE(KIND) __attribute__((no_sanitize(KIND)))
#endif
#endif
#ifndef SHERMES_NO_SANITIZE
#define SHERMES_NO_SANITIZE(KIND)
#endif

#ifdef __GNUC__
#define SH_LIKELY(EXPR) __builtin_expect((bool)(EXPR), true)
#define SH_UNLIKELY(EXPR) __builtin_expect((bool)(EXPR), false)
#else
#define SH_LIKELY(EXPR) (EXPR)
#define SH_UNLIKELY(EXPR) (EXPR)
#endif

/// SH_ATTRIBUTE_NOINLINE - On compilers where we have a directive to do so,
/// mark a method "not for inlining".
#ifdef __GNUC__
#define SH_ATTRIBUTE_NOINLINE __attribute__((noinline))
#else
#define SH_ATTRIBUTE_NOINLINE
#endif

/// SH_ATTRIBUTE_ALWAYS_INLINE - On compilers where we have a directive to do
/// so, mark a method "always inline" because it is performance sensitive.
#ifdef __GNUC__
#define SH_ATTRIBUTE_ALWAYS_INLINE __attribute__((always_inline))
#else
#define SH_ATTRIBUTE_ALWAYS_INLINE
#endif

#ifdef __GNUC__
#define SH_ATTRIBUTE_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define SH_ATTRIBUTE_NORETURN __declspec(noreturn)
#else
#define SH_ATTRIBUTE_NORETURN
#endif

/// SH_READNONE - Indicates that a function has no side effects and doesn't
/// read any state other than its parameters (a pure mathematical function).
#ifdef __GNUC__
#define SH_READNONE __attribute__((const))
#else
#define SH_READNONE
#endif

/// SH_BUILTIN_UNREACHABLE - On compilers which support it, expands
/// to an expression which states that it is undefined behavior for the
/// compiler to reach this point.  Otherwise is not defined.
#ifdef __GNUC__
#define SH_BUILTIN_UNREACHABLE __builtin_unreachable()
#elif defined(_MSC_VER)
#define SH_BUILTIN_UNREACHABLE __assume(false)
#else
#define SH_BUILTIN_UNREACHABLE ((void)0)
#endif

#endif // HERMES_SH_CONFIG_H
