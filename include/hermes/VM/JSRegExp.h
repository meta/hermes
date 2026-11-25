/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSREGEXP_H
#define HERMES_VM_JSREGEXP_H

#include "hermes/Regex/Regex.h"
#include "hermes/Regex/RegexTypes.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/RegExpBytecode.h"
#include "hermes/VM/RegExpMatch.h"
#include "hermes/VM/SmallXString.h"

#include <memory>
#include "llvh/ADT/SmallString.h"

namespace hermes {
namespace vm {

class JSRegExp final : public JSObject {
 public:
  using Super = JSObject;
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSRegExpKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSRegExpKind;
  }

  /// Create a JSRegExp, with the empty string for pattern and flags.
  static PseudoHandle<JSRegExp> create(Runtime &runtime);

  /// Create a JSRegExp, with the empty string for pattern and flags.
  static PseudoHandle<JSRegExp> create(
      Runtime &runtime,
      Handle<JSObject> prototype);

  /// Creates the hidden class for Regular Expression match objects.
  /// \p arrayClass should be hidden class for JSArray objects, and it will be
  /// augmented with properties "index", "input", and "groups".
  static PseudoHandle<HiddenClass> createMatchClass(
      Runtime &runtime,
      Handle<HiddenClass> arrayClass);

  /// Initializes RegExp with existing bytecode. Populates fields for the
  /// pattern and flags, but performs no validation on them. It is assumed that
  /// the bytecode is correct and corresponds to the given pattern/flags.
  /// \p bytecode is shared, not copied. The regexp must not have been
  /// initialized before.
  static void initialize(
      Handle<JSRegExp> selfHandle,
      Runtime &runtime,
      Handle<StringPrimitive> pattern,
      Handle<StringPrimitive> flags,
      llvh::IntrusiveRefCntPtr<RegExpBytecode> bytecode);

  /// Initialize a RegExp based on another RegExp \p otherHandle. If \p flags
  /// matches the internal flags of the other RegExp, this lets us avoid
  /// recompiling by just copying the bytecode.
  static ExecutionStatus initialize(
      Handle<JSRegExp> selfHandle,
      Runtime &runtime,
      Handle<JSRegExp> otherHandle,
      Handle<StringPrimitive> flags);

  /// Perform validation of the pattern and flags and throw \c SyntaxError on
  /// error. If valid, set the source and flags to the given strings, and set
  /// the standard properties of the RegExp according to the flags. Note that
  /// RegExps are not mutable (with the exception of the lastIndex property).
  /// Compiles the \p pattern and \p flags to RegExp bytecode.
  static ExecutionStatus initialize(
      Handle<JSRegExp> selfHandle,
      Runtime &runtime,
      Handle<StringPrimitive> pattern,
      Handle<StringPrimitive> flags);

  /// \return the pattern string used to initialize this RegExp.
  /// Note this is not suitable for interpolation between //, nor for
  /// implementation of toString(). See 'escapePattern' to properly escape it.
  static PseudoHandle<StringPrimitive> getPattern(
      JSRegExp *self,
      PointerBase &base);

  /// \return An escaped version of the regexp pattern per ES6 21.2.3.2.4, or an
  /// exception if the string could not be created.
  static CallResult<HermesValue> escapePattern(
      Handle<StringPrimitive> pattern,
      Runtime &runtime);

  /// \return the flag bits used to initialize this RegExp
  static regex::SyntaxFlags getSyntaxFlags(JSRegExp *self) {
    return self->syntaxFlags_;
  }

  /// Set the flag bits for this RegExp to \p flags
  static void setSyntaxFlags(JSRegExp *self, regex::SyntaxFlags flags) {
    self->syntaxFlags_ = flags;
  }

  /// \return the bytecode. The regexp must have been initialized.
  llvh::ArrayRef<uint8_t> getBytecode() const {
    return bytecode_->bytecode();
  }

  /// \return the object holding the bytecode, or null before
  /// initialization.
  const RegExpBytecode *getBytecodeEntry() const {
    return bytecode_.get();
  }

  /// \return the group name mappings object, or null if there are none. The
  /// pointer is not rooted: pin it before the next allocation.
  JSObject *getGroupNameMappings(Runtime &runtime);

  void setGroupNameMappings(Runtime &runtime, JSObject *groupObj);

  /// Searches self for a match for \str.
  /// \p searchStartOffset is the offset from which to begin searching.
  /// If searchStartOffset exceeds the length of the string, or if no match
  /// is found at or after searchStartOffset, then the result will be empty.
  /// If a match is found, the returned value will have length equal to the
  /// number of capture groups, plus one. The first element in the result will
  /// be the full match, with subsequent elements corresponding to the capture
  /// groups in order.
  /// Note that the location of matches is given relative to the beginning of
  /// the string, not the searchStartOffset.
  static CallResult<RegExpMatch> search(
      Handle<JSRegExp> selfHandle,
      Runtime &runtime,
      Handle<StringPrimitive> strHandle,
      uint32_t searchStartOffset);

 public:
  friend void JSRegExpBuildMeta(const GCCell *, Metadata::Builder &);

  JSRegExp(Runtime &runtime, Handle<JSObject> parent, Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz),
        pattern_(
            runtime,
            runtime.getPredefinedString(Predefined::emptyString),
            runtime.getHeap()) {}

 private:
  ~JSRegExp();

  /// Take the reference \p bytecode and set syntaxFlags_ from its header.
  void setBytecode(llvh::IntrusiveRefCntPtr<RegExpBytecode> bytecode);

  /// \return the bytecode object if this regexp owns it exclusively and so
  /// reports its memory, otherwise null.
  const RegExpBytecode *exclusiveBytecode() const;

  static ExecutionStatus initializeGroupNameMappingObj(
      Runtime &runtime,
      Handle<JSRegExp> selfHandle,
      std::deque<llvh::SmallVector<char16_t, 5>> &orderedNamedGroups,
      regex::ParsedGroupNamesMapping &parsedMappings);

  /// The order of properties here is important to avoid wasting space. When
  /// compressed pointers are enabled, JSObject has an odd number of 4 byte
  /// properties. So putting this GCPointer before the native pointer guarantees
  /// that the native pointer is always 8 byte aligned without extra padding.
  GCPointer<StringPrimitive> pattern_;

  /// The bytecode, owned exclusively or shared with a literal cache. One
  /// pointer, so the GC may relocate the cell by copying it.
  llvh::IntrusiveRefCntPtr<RegExpBytecode> bytecode_{};

  regex::SyntaxFlags syntaxFlags_ = {};

  /// Maps each distinct capture group name to its match array index, or to a
  /// JSArray of indices if several mutually exclusive capture groups share the
  /// name. Never exposed to JS and never modified after initialization, so
  /// JSRegExps compiled from the same pattern may share one.
  GCPointer<JSObject> groupNameMappings_{nullptr};

  // Finalizer to clean up stored native regex
  static void _finalizeImpl(GCCell *cell, GC &gc);
  static size_t _mallocSizeImpl(GCCell *cell);

#ifdef HERMES_MEMORY_INSTRUMENTATION
  static std::string _snapshotNameImpl(GCCell *cell, GC &gc);
  static void _snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
  static void _snapshotAddNodesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
#endif
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_JSREGEXP_H
