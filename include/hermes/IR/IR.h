/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_IR_IR_H
#define HERMES_IR_IR_H

#include "hermes/ADT/OwningFoldingSet.h"
#include "hermes/ADT/WordBitSet.h"
#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/AST/NativeContext.h"
#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/FrontEndDefs/Typeof.h"
#include "hermes/IR/TypeContext.h"
#include "hermes/Sema/SemContext.h"
#include "hermes/Support/Conversions.h"
#include "hermes/Support/ScopeChain.h"

#include "llvh/ADT/Hashing.h"
#include "llvh/ADT/SmallPtrSet.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/ADT/StringRef.h"
#include "llvh/ADT/Twine.h"
#include "llvh/ADT/ilist_node.h"
#include "llvh/ADT/iterator_range.h"
#include "llvh/IR/SymbolTableListTraits.h"
#include "llvh/Support/Casting.h"
#include "llvh/Support/MathExtras.h"
#include "llvh/Support/raw_ostream.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace hermes {

namespace sema {
class FunctionInfo;
} // namespace sema

class Module;
class VariableScope;
class Function;
class BasicBlock;
class JSDynamicParam;
class Instruction;
class Context;
class TerminatorInst;
class LazyCompilationDataInst;
class EvalCompilationDataInst;

/// Describes the potential side effects of an instruction. The side effects are
/// described by a series of bits, each of which specifies a particular way in
/// which this instruction may observe/modify the state of the world, or
/// otherwise restrict its movement/deletion.
class SideEffect {
 public:
  SideEffect() {
    memset(this, 0, sizeof(SideEffect));
  }

  /// Define each of the fields in a SideEffect.
  /// \c ReadStack and \c WriteStack imply that an instruction may read/write to
  /// a stack location. Such an instruction must take an \c AllocStackInst
  /// operand which it can read/write to.
  /// \c ReadFrame and \c WriteFrame imply that an instruction may read/write
  /// variables in the frame.
  /// \c ReadHeap and \c WriteHeap imply that an instruction may read/write
  /// objects and other values on the heap.
  /// \c Throw implies that an instruction may throw an exception.
  /// \c ExecuteJS implies that an instruction may execute arbitrary user
  /// provided JS. An instruction with \c ExecuteJS set must also have bits set
  /// for throwing and accessing the frame and heap.
  /// \c FirstInBlock implies that an instruction must be at the start of a
  /// basic block. An instruction with \c FirstInBlock set may only be preceded
  /// by other instances of the same instruction.
  /// \c Idempotent implies that repeated execution of an instruction will not
  /// affect the state of the world or the result of the instruction. For
  /// example, two identical instructions that have \p Idempotent set may be
  /// merged if they are only separated by instructions that have no side
  /// effects.
  /// \c Unhoistable implies that an instruction must not be hoisted out of its
  /// basic block (e.g. by CodeMotion) because it relies on preconditions which
  /// aren't fully expressed only by its operands.

#define SIDE_EFFECT_FIELDS \
  FIELD(ReadStack)         \
  FIELD(WriteStack)        \
  FIELD(ReadFrame)         \
  FIELD(WriteFrame)        \
  FIELD(ReadHeap)          \
  FIELD(WriteHeap)         \
  FIELD(Throw)             \
  FIELD(ExecuteJS)         \
  FIELD(FirstInBlock)      \
  FIELD(Idempotent)        \
  FIELD(Unhoistable)

#define FIELD(field)        \
  bool get##field() const { \
    return f##field##_;     \
  }
  SIDE_EFFECT_FIELDS
#undef FIELD

#define FIELD(field)         \
  SideEffect &set##field() { \
    f##field##_ = true;      \
    return *this;            \
  }
  SIDE_EFFECT_FIELDS
#undef FIELD

  /// Create side effects for an instruction that may execute user JS. Executing
  /// JS implies several other side-effects, so this helps populate them.
  static SideEffect createExecute() {
    return SideEffect{}
        .setReadFrame()
        .setReadHeap()
        .setWriteFrame()
        .setWriteHeap()
        .setExecuteJS()
        .setThrow();
  }

  /// Create side effects for an instruction that may have arbitrary side
  /// effects, and should not be deleted or reordered. For now this is just an
  /// alias for createExecute.
  static SideEffect createUnknown() {
    return createExecute();
  }

  /// Determine whether the given instruction modifies the state of the world in
  /// any an observable way.
  bool hasSideEffect() const {
    // Note that we do not check ExecuteJS here because any observable
    // side-effect of a call requires one of these other bits to be set.
    return getWriteStack() || getWriteHeap() || getWriteFrame() || getThrow();
  }

  /// Determine if the instruction is pure. That is, whether separate
  /// invocations of the instruction with the same operands will always yield
  /// the same result, and will not modify the state of the world in any
  /// observable way. Note that this excludes instructions that allocate (e.g.
  /// AllocObject, CreateFunction) since they produce a different reference each
  /// time.
  bool isPure() const {
    return !hasSideEffect() && !getReadStack() && !getReadHeap() &&
        !getReadFrame() && getIdempotent();
  }

  /// Helper functions to expose the SideEffectKind interface.
  /// TODO: Delete these once all instructions are migrated off SideEffectKind.
  bool mayExecute() const {
    return getThrow() || getExecuteJS();
  }
  bool mayWriteOrWorse() const {
    return getWriteStack() || getWriteFrame() || getWriteHeap() || mayExecute();
  }
  bool mayReadOrWorse() const {
    return getReadStack() || getReadFrame() || getReadHeap() ||
        mayWriteOrWorse();
  }

  /// Check if these side effects are well formed. For now, this just checks
  /// that we correctly set/unset bits when an instruction is marked as being
  /// able to execute.
  bool isWellFormed() const {
    if (getExecuteJS()) {
      // If an instruction may execute, it cannot be idempotent, and could throw
      // or access the frame/heap.
      if (getIdempotent() || !getReadFrame() || !getReadHeap() ||
          !getWriteFrame() || !getWriteHeap() || !getThrow())
        return false;
    }
    return true;
  }

 private:
#define FIELD(field) bool f##field##_ : 1;
  SIDE_EFFECT_FIELDS
#undef FIELD
#undef SIDE_EFFECT_FIELDS
};

enum class ValueKind : uint8_t {
  First_ValueKind,
#define DEF_VALUE(CLASS, PARENT) CLASS##Kind,
#define BEGIN_VALUE(CLASS, PARENT) First_##CLASS##Kind,
#define DEF_TAG(NAME, PARENT) NAME##Kind,
#define END_VALUE(CLASS) Last_##CLASS##Kind,
#define MARK_VALUE(CLASS) CLASS##Kind,
#define MARK_FIRST(CLASS, PARENT) First_##CLASS##Kind,
#define MARK_LAST(CLASS) Last_##CLASS##Kind,
#include "hermes/IR/ValueKinds.def"
  Last_ValueKind,
};

static inline bool kindInRange(ValueKind kind, ValueKind from, ValueKind to) {
  return kind > from && kind < to;
}

/// Return true if the specified kind falls in the range of the specified class.
#define HERMES_IR_KIND_IN_CLASS(kind, CLASS) \
  kindInRange(                               \
      kind, ValueKind::First_##CLASS##Kind, ValueKind::Last_##CLASS##Kind)

/// Return the numeric offset of the specified kind from the first kind in the
/// class.
#define HERMES_IR_KIND_TO_OFFSET(CLASS, kind) \
  ((int)(kind) - (int)ValueKind::First_##CLASS##Kind - 1)

/// Convert from an offset to a ValueKind inside a class.
#define HERMES_IR_OFFSET_TO_KIND(CLASS, offset) \
  ((ValueKind)((int)ValueKind::First_##CLASS##Kind + (offset) + 1))

/// Return number of values in an IR class.
#define HERMES_IR_CLASS_LENGTH(CLASS) \
  ((int)ValueKind::Last_##CLASS##Kind - (int)ValueKind::First_##CLASS##Kind - 1)

/// A set of attributes to be associated with Values.
union Attributes {
  struct {
#define ATTRIBUTE(_valueKind, name, _string) uint16_t name : 1;
#include "hermes/IR/Attributes.def"
  };

  uint16_t flags_;

  /// No attributes on construction.
  explicit Attributes() : flags_(0) {
    // Make sure the bits can fit into flags_.
    static_assert(sizeof(Attributes) == sizeof(flags_), "too many attributes");
  }

  /// \return true if there are no attributes on the function.
  bool isEmpty() const {
    return flags_ == 0;
  }

  /// Clear all attributes on the function.
  void clear() {
    flags_ = 0;
  }

  llvh::hash_code hash() {
    return flags_;
  }

  /// \return a string describing the attributes if there are any.
  /// If there are no attributes, returns "".
  /// If there are attributes returns, e.g. "[allCallsitesKnownInStrictMode]".
  std::string getDescriptionStr() const;
};

class Value {
 public:
  using UseListTy = llvh::SmallVector<Instruction *, 2>;
  using Use = std::pair<Value *, unsigned>;

 private:
  // We declare operator delete as a private member below. Classes declared
  // as friend below invokes constructor of Value subtypes directly.
  // C++ requires that if an exception is raised during construction of
  // a new object, the object is deleted.
  // As a result, delete must be accessible to callers of constructors.
  // On some (but not all) compilers, the -fno-exceptions compiler flag
  // removes this requirement.
  friend class Module;
  friend class IRBuilder;

  ValueKind Kind;

  /// Bitset of the attributes associated with this value.
  /// If this exceeds 2 bytes, the actual storage should be moved to Module,
  /// and this field replaced with a bit which indicates whether the storage
  /// exists.
  /// ValueKind is 1 byte and Type is 4 bytes, so we can store one uint16_t here
  /// without increasing the size of Value.
  Attributes attributes_;

  /// The JavaScript type of the value.
  Type valueType = Type::createAnyType();

  /// A list of users of this instruction.
  UseListTy Users;

  // Instances of Value are not supposed to be deleted directly because we
  // want to avoid defining a virtual destructor. The private operator
  // delete can only be invoked by the \c destroy() method.

  // Use sized deallocation to speed things up, if it is available
#ifdef __cpp_sized_deallocation
  void operator delete(void *p, size_t s) {
    ::operator delete(p, s);
  }
#else
  void operator delete(void *p) {
    ::operator delete(p);
  }
#endif

 protected:
  explicit Value(ValueKind k) {
    static_assert(sizeof(Kind) == 1, "ValueKind too big");
    static_assert(sizeof(attributes_) == 2, "attributes_ increases Value size");
    static_assert(
        sizeof(valueType) <= 4,
        "Type aligning to 4 bytes allows attributes_ to not increase Value size");
    Kind = k;
  }

 public:
  Value(const Value &) = delete;
  void operator=(const Value &) = delete;

  /// Run a Value's destructor and deallocate its memory.
  static void destroy(Value *V);

  /// \return true if this instruction tracks its users.
  inline bool tracksUsers() const;

  /// \return the users of the value.
  const UseListTy &getUsers() const;

  /// \returns the number of users the value has.
  unsigned getNumUsers() const;

  /// \returns true if the value has some users.
  bool hasUsers() const;

  /// \returns true if the value has only one user.
  bool hasOneUser() const;

  /// Remove the first occurrence of \p Inst from the Users list.
  void removeUse(Use U);

  /// Add the instruction \p Inst to the Users list.
  /// \returns the use handle.
  Value::Use addUser(Instruction *Inst);

  /// Replaces all uses of the current value with \p Other.
  void replaceAllUsesWith(Value *Other);

  /// \returns the kind of the value.
  ValueKind getKind() const {
    return Kind;
  }

  /// \returns the string representation of the Value kind.
  llvh::StringRef getKindStr() const;

  /// Sets a new type \p type to the value.
  ///
  /// Types for Instructions should only be set in the constructors
  /// or during the TypeInference pass.
  /// The only instructions which set the type in the constructor are
  /// those with inherent types (see \c Instruction::getInherentType),
  /// and those which are simply copying the types from a single operand
  /// (e.g. MovInst, LIRLoadConstInst).
  /// All other Instruction types will be set during inference in TypeInference.
  void setType(Type type) {
#ifndef NDEBUG
    if (llvh::isa<Function>(this)) {
      assert(
          type.isFunctionCodeType() &&
          "Functions cannot have non-functionCode types");
    }
#endif
    valueType = type;
  }

  /// \returns the JavaScript type of the value.
  Type getType() const {
    return valueType;
  }

  /// Takes a Module parameter to allow easily moving the attributes_ storage
  /// into Module if necessary in the future.
  /// Will create default Attributes if they don't exist already.
  /// \return the Attributes of this value.
  Attributes &getAttributesRef(const Module *) {
    return attributes_;
  }
  /// Takes a Module parameter to allow easily moving the attributes_ storage
  /// into Module if necessary in the future.
  /// \return the Attributes by value, because nonexistent attributes might not
  ///   be stored anywhere if/when we use a side table.
  Attributes getAttributes(const Module *) const {
    return attributes_;
  }

  static bool classof(const Value *) {
    return true;
  }

  using iterator = UseListTy::iterator;
  using const_iterator = UseListTy::const_iterator;

  inline const_iterator users_begin() const {
    return Users.begin();
  }
  inline const_iterator users_end() const {
    return Users.end();
  }
  inline iterator users_begin() {
    return Users.begin();
  }
  inline iterator users_end() {
    return Users.end();
  }
};

/// Deleter for Values.
struct ValueDeleter {
  void operator()(Value *V) {
    Value::destroy(V);
  }
};

/// \return the customData field of \p lexScope, casted to VariableScope. Can
/// return null.
VariableScope *getLexicalScopeCustomData(sema::LexicalScope *lexScope);

/// \return the customData field of \p lexScope, casted to Value. Can return
/// null.
Value *getDeclCustomData(sema::Decl *decl);

/// This represents a function parameter.
class Parameter : public Value {
  friend class Function;
  Parameter(const Parameter &) = delete;
  void operator=(const Parameter &) = delete;

  /// The function that contains this paramter.
  Function *parent_;

  /// The formal name of the parameter
  Identifier name_;

  explicit Parameter(Function *parent, Identifier name)
      : Value(ValueKind::ParameterKind), parent_(parent), name_(name) {
    assert(parent_ && "Invalid parent");
  }

 public:
  Function *getParent() const {
    return parent_;
  }
  /// \brief Return a constant reference to the value's name.
  Identifier getName() const {
    return name_;
  }
  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::ParameterKind;
  }
};

/// Supertype of the types below, to factor out common code.
/// Note that ctor is protected; can't create one of these.
class JSParam : public Value {
  JSParam(const JSParam &) = delete;
  void operator=(const JSParam &) = delete;

  /// The formal name of the parameter
  Identifier name_;

 protected:
  JSParam(ValueKind kind, Function *parent, Identifier name)
      : Value(kind), name_(name), parent_(parent) {
    assert(parent_ && "Invalid parent");
  }

  /// The function that contains this paramter.
  Function *parent_;

 public:
  Context &getContext() const;

  Function *getParent() const {
    return parent_;
  }

  /// \brief Return a constant reference to the value's name.
  Identifier getName() const {
    return name_;
  }
};

/// This represents a JS function parameter, all of which are optional.
class JSDynamicParam : public JSParam {
  friend class Function;
  friend class IRBuilder;
  JSDynamicParam(const JSDynamicParam &) = delete;
  void operator=(const JSDynamicParam &) = delete;

  JSDynamicParam(Function *parent, Identifier name)
      : JSParam(ValueKind::JSDynamicParamKind, parent, name) {}

 public:
  /// Return the index of this parameter in the function's parameter list.
  /// "this" parameter is excluded from the list.
  uint32_t getIndexInParamList() const;

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::JSDynamicParamKind;
  }
};

/// This represents one of the "special" JS function parameters: newTarget,
/// or parentScope.
class JSSpecialParam : public JSParam {
  friend class Function;
  friend class IRBuilder;
  JSSpecialParam(const JSSpecialParam &) = delete;
  void operator=(const JSSpecialParam &) = delete;

  JSSpecialParam(Function *parent, Identifier name)
      : JSParam(ValueKind::JSSpecialParamKind, parent, name) {}

 public:
  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::JSSpecialParamKind;
  }
};

/// This represent an empty or missing value.
class EmptySentinel : public Value {
  EmptySentinel(const EmptySentinel &) = delete;
  void operator=(const EmptySentinel &) = delete;

 public:
  explicit EmptySentinel() : Value(ValueKind::EmptySentinelKind) {}

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::EmptySentinelKind;
  }
};

/// This represents a label, which is a thin wrapper around an identifier.
class Label : public Value {
  Label(const Label &) = delete;
  void operator=(const Label &) = delete;

  // Label is "special" it is never created separately - it is only embedded
  // in existing instructions. To prevent accidents we list them here as
  // friends.

  void *operator new(size_t) {
    llvm_unreachable("Labels cannot be allocated separately");
  }

  /// The formal name of the parameter
  Identifier text;

 public:
  explicit Label(Identifier txt) : Value(ValueKind::LabelKind), text(txt) {}

  Identifier get() const {
    return text;
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LabelKind;
  }
};

class Literal : public Value {
  Literal(const Literal &) = delete;
  void operator=(const Literal &) = delete;

 public:
  explicit Literal(ValueKind k) : Value(k) {}

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), Literal);
  }
};

class LiteralEmpty : public Literal {
  LiteralEmpty(const LiteralEmpty &) = delete;
  void operator=(const LiteralEmpty &) = delete;

 public:
  explicit LiteralEmpty() : Literal(ValueKind::LiteralEmptyKind) {
    setType(Type::createEmpty());
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LiteralEmptyKind;
  }
};

class LiteralUninit : public Literal {
  LiteralUninit(const LiteralUninit &) = delete;
  void operator=(const LiteralUninit &) = delete;

 public:
  explicit LiteralUninit() : Literal(ValueKind::LiteralUninitKind) {
    setType(Type::createUninit());
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LiteralUninitKind;
  }
};

class LiteralNull : public Literal {
  LiteralNull(const LiteralNull &) = delete;
  void operator=(const LiteralNull &) = delete;

 public:
  explicit LiteralNull() : Literal(ValueKind::LiteralNullKind) {
    setType(Type::createNull());
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LiteralNullKind;
  }
};

class LiteralUndefined : public Literal {
  LiteralUndefined(const LiteralUndefined &) = delete;
  void operator=(const LiteralUndefined &) = delete;

 public:
  explicit LiteralUndefined() : Literal(ValueKind::LiteralUndefinedKind) {
    setType(Type::createUndefined());
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LiteralUndefinedKind;
  }
};

class LiteralBigInt : public Literal, public llvh::FoldingSetNode {
  LiteralBigInt(const LiteralBigInt &) = delete;
  LiteralBigInt &operator=(const LiteralBigInt &) = delete;

  // value holds the BigInt literal string as parsed by the front-end.
  UniqueString *value;

 public:
  explicit LiteralBigInt(UniqueString *v)
      : Literal(ValueKind::LiteralBigIntKind), value(v) {
    setType(Type::createBigInt());
  }

  UniqueString *getValue() const {
    return value;
  }

  static void Profile(llvh::FoldingSetNodeID &ID, UniqueString *value) {
    ID.AddPointer(value);
  }

  void Profile(llvh::FoldingSetNodeID &ID) const {
    LiteralBigInt::Profile(ID, value);
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LiteralBigIntKind;
  }
};

class LiteralNumber : public Literal, public llvh::FoldingSetNode {
  LiteralNumber(const LiteralNumber &) = delete;
  void operator=(const LiteralNumber &) = delete;
  double value;

 public:
  double getValue() const {
    return value;
  }

  /// Check whether the number can be represented in integer
  /// of type T without losing any precision or information.
  /// If not representible, \returns llvh::None.
  template <typename T>
  llvh::Optional<T> isIntTypeRepresentible() const {
    // Check the value is within the bounds of T.
    // Converting double to int when it's out of bound is undefined behavior.
    // Although it is harmless in this scenario, we should still avoid it.
    if (value > std::numeric_limits<T>::max() ||
        value < std::numeric_limits<T>::min()) {
      return llvh::None;
    }
    if (std::isnan(value)) {
      return llvh::None;
    }
    // Check the value is integer and is not -0.
    T valAsInt = static_cast<T>(value);
    if (valAsInt == value && (valAsInt || !std::signbit(value))) {
      return valAsInt;
    }
    return llvh::None;
  }

  /// Check whether the number is positive zero.
  bool isPositiveZero() const {
    return value == 0.0 && !std::signbit(value);
  }

  /// Check whether the number is negative zero.
  bool isNegativeZero() const {
    return value == 0.0 && std::signbit(value);
  }

  /// Check whether the number can be represented in unsigned 8-bit integer
  /// without losing any precision or information.
  bool isUInt8Representible() const {
    return isIntTypeRepresentible<uint8_t>().hasValue();
  }

  /// Check whether the number can be represented in 32-bit integer
  /// without losing any precision or information.
  bool isInt32Representible() const {
    return isIntTypeRepresentible<int32_t>().hasValue();
  }

  /// Check whether the number can be represented in unsigned 32-bit integer
  /// without losing any precision or information.
  bool isUInt32Representible() const {
    return isIntTypeRepresentible<uint32_t>().hasValue();
  }

  /// Convert the number to uint8_t without losing precision. If not doable,
  /// assertion will fail.
  uint32_t asUInt8() const {
    auto tmp = isIntTypeRepresentible<uint8_t>();
    assert(tmp && "Cannot convert to uint8_t");
    return tmp.getValue();
  }

  /// Convert the number to int32_t without losing precision. If not doable,
  /// assertion will fail.
  int32_t asInt32() const {
    auto tmp = isIntTypeRepresentible<int32_t>();
    assert(tmp && "Cannot convert to int32_t");
    return tmp.getValue();
  }

  /// Convert the number to uint32_t without losing precision. If not doable,
  /// assertion will fail.
  uint32_t asUInt32() const {
    auto tmp = isIntTypeRepresentible<uint32_t>();
    assert(tmp && "Cannot convert to uint32_t");
    return tmp.getValue();
  }

  /// Convert the number to an 32-bit integer according to the rules specified
  /// in ES5.1 section 9.5. This is notably different from what a C/C++ cast
  /// to integer would return for numbers larger than 2*32.
  int32_t truncateToInt32() const {
    return hermes::truncateToInt32(value);
  }

  uint32_t truncateToUInt32() const {
    return hermes::truncateToUInt32(value);
  }

  /// Attempt to convert to an array index.
  OptValue<uint32_t> convertToArrayIndex() const {
    return doubleToArrayIndex(value);
  }

  explicit LiteralNumber(double val)
      : Literal(ValueKind::LiteralNumberKind), value(val) {
    if (isInt32Representible()) {
      setType(Type::createInt32());
    } else {
      setType(Type::createNumber());
    }
  }

  static void Profile(llvh::FoldingSetNodeID &ID, double value) {
    ID.AddInteger(llvh::DoubleToBits(value));
  }

  void Profile(llvh::FoldingSetNodeID &ID) const {
    LiteralNumber::Profile(ID, value);
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LiteralNumberKind;
  }
};

class LiteralString : public Literal, public llvh::FoldingSetNode {
  LiteralString(const LiteralString &) = delete;
  void operator=(const LiteralString &) = delete;
  Identifier value;

 public:
  Identifier getValue() const {
    return value;
  }

  explicit LiteralString(Identifier val)
      : Literal(ValueKind::LiteralStringKind), value(val) {
    setType(Type::createString());
  }

  static void Profile(llvh::FoldingSetNodeID &ID, Identifier value) {
    ID.AddPointer(value.getUnderlyingPointer());
  }

  void Profile(llvh::FoldingSetNodeID &ID) const {
    LiteralString::Profile(ID, value);
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LiteralStringKind;
  }
};

/// Typed private property name.
class LiteralPrivateName : public Literal, public llvh::FoldingSetNode {
  LiteralPrivateName(const LiteralPrivateName &) = delete;
  void operator=(const LiteralPrivateName &) = delete;
  Identifier value;

 public:
  Identifier getValue() const {
    return value;
  }

  explicit LiteralPrivateName(Identifier val)
      : Literal(ValueKind::LiteralPrivateNameKind), value(val) {
    // Use 'any' because there's no runtime representation.
    setType(Type::createAnyType());
  }

  static void Profile(llvh::FoldingSetNodeID &ID, Identifier value) {
    ID.AddPointer(value.getUnderlyingPointer());
  }

  void Profile(llvh::FoldingSetNodeID &ID) const {
    LiteralPrivateName::Profile(ID, value);
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LiteralPrivateNameKind;
  }
};

class LiteralBool : public Literal {
  LiteralBool(const LiteralBool &) = delete;
  void operator=(const LiteralBool &) = delete;
  bool value;

 public:
  bool getValue() const {
    return value;
  }

  explicit LiteralBool(bool val)
      : Literal(ValueKind::LiteralBoolKind), value(val) {
    setType(Type::createBoolean());
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::LiteralBoolKind;
  }
};

/// This represents the JavaScript global object. Only one instance of it
/// can exist in a module.
class GlobalObject : public Literal {
  GlobalObject(const GlobalObject &) = delete;
  void operator=(const GlobalObject &) = delete;

 public:
  explicit GlobalObject() : Literal(ValueKind::GlobalObjectKind) {
    setType(Type::createObject());
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::GlobalObjectKind;
  }
};

template <class T, ValueKind K, Type typer() = Type::createAnyType>
class LiteralWrapper : public Literal, public llvh::FoldingSetNode {
 protected:
  T data_;

 public:
  explicit LiteralWrapper(T data) : Literal(K), data_(data) {
    setType(typer());
  }

  T getData() const {
    return data_;
  }

  static bool classof(const Value *V) {
    return V->getKind() == K;
  }

  static void Profile(llvh::FoldingSetNodeID &ID, T data) {
    if constexpr (std::is_integral_v<T>)
      ID.AddInteger(data);
    else if constexpr (std::is_enum_v<T>)
      ID.AddInteger(static_cast<std::underlying_type_t<T>>(data));
    else if constexpr (std::is_pointer_v<T>)
      ID.AddPointer(data);
    else
      ID.Add(data);
  }

  void Profile(llvh::FoldingSetNodeID &ID) const {
    Profile(ID, data_);
  }
};

using LiteralBuiltinIdx = LiteralWrapper<
    BuiltinMethod::Enum,
    ValueKind::LiteralBuiltinIdxKind,
    Type::createNumber>;
using LiteralIRType =
    LiteralWrapper<Type, ValueKind::LiteralIRTypeKind, Type::createNull>;
using LiteralTypeOfIsTypes = LiteralWrapper<
    TypeOfIsTypes,
    ValueKind::LiteralTypeOfIsTypesKind,
    Type::createNull>;
using LiteralNativeSignature =
    LiteralWrapper<NativeSignature *, ValueKind::LiteralNativeSignatureKind>;
using LiteralNativeExtern = LiteralWrapper<
    NativeExtern *,
    ValueKind::LiteralNativeExternKind,
    Type::createNumber>;

/// This represents a JavaScript variable, that's allocated in the function.
class Variable : public Value {
 public:
 private:
  Variable(const Variable &) = delete;
  void operator=(const Variable &) = delete;

  /// The textual representation of the variable in the JavaScript program.
  Identifier text;

  /// The scope that owns the variable.
  VariableScope *parent;

  /// Index used for a Variable that hasn't had its index assigned yet.
  static constexpr uint32_t kUnresolvedIndex = UINT32_MAX;

  /// The index of the variable in the environment.
  /// Initially set to kUnresolvedIndex, then set after register allocation.
  /// Used for emitting loads/stores from environments using the variable.
  /// Reused for variables that are captured in nested lazy functions, when they
  /// are compiled.
  uint32_t index_ = kUnresolvedIndex;

  /// If true, this variable obeys the TDZ rules.
  bool obeysTDZ_ = false;

  /// The constness of this variable.
  sema::Decl::Constness constness_ = sema::Decl::Constness::Never;

  /// If true, this variable is hidden from e.g. the debugger.
  /// Used for synthetic variables that don't correspond to user-defined
  /// variables.
  bool hidden_ = false;

 public:
  explicit Variable(VariableScope *scope, Identifier txt, bool hidden);

  ~Variable();

  Identifier getName() const {
    return text;
  }
  VariableScope *getParent() const {
    return parent;
  }

  /// Set the index of the variable in the environment.
  /// \pre the index has not yet been set.
  void setIndexInVariableList(uint32_t index) {
    assert(!hasIndexInVariableList() && "Index can only be set once");
    index_ = index;
  }
  /// \return whether the index of the variable has been set.
  bool hasIndexInVariableList() const {
    return index_ != kUnresolvedIndex;
  }
  /// \pre the index has been set.
  /// \return the index of the variable in the environment.
  uint32_t getIndexInVariableList() const {
    assert(hasIndexInVariableList() && "Index not set yet");
    return index_;
  }

  bool getObeysTDZ() const {
    return obeysTDZ_;
  }
  void setObeysTDZ(bool value) {
    obeysTDZ_ = value;
  }

  sema::Decl::Constness getConstness() const {
    return constness_;
  }
  void setConstness(sema::Decl::Constness constness) {
    constness_ = constness;
  }

  bool getHidden() const {
    return hidden_;
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::VariableKind;
  }
};

/// A property of the global object.
class GlobalObjectProperty : public Value, public llvh::FoldingSetNode {
  /// The variable name.
  LiteralString *name_;

  /// Was it explicitly declared (which means that it is non-configurable).
  bool declared_ = false;

 public:
  GlobalObjectProperty(LiteralString *name)
      : Value(ValueKind::GlobalObjectPropertyKind), name_(name) {}

  static bool classof(const Value *v) {
    return v->getKind() == ValueKind::GlobalObjectPropertyKind;
  }

  LiteralString *getName() const {
    return name_;
  }

  bool isDeclared() const {
    return declared_;
  }

  void orDeclared(bool declared) {
    declared_ |= declared;
  }

  static void Profile(llvh::FoldingSetNodeID &ID, LiteralString *name) {
    ID.AddPointer(name);
  }
  void Profile(llvh::FoldingSetNodeID &ID) const {
    Profile(ID, name_);
  }
};

/// This is the base class for all instructions in the high-level IR.
class Instruction
    : public llvh::ilist_node_with_parent<Instruction, BasicBlock>,
      public Value {
  friend class Value;
  Instruction(const Instruction &) = delete;
  void operator=(const Instruction &) = delete;

  /// The basic block containing this instruction.
  BasicBlock *Parent;
  /// Saves the instruction operands.
  llvh::SmallVector<Value::Use, 2> Operands;

  SMLoc location_{};
  /// The statement of which this Instruction is a part.
  /// If 0, then there is no corresponding source statement.
  uint32_t statementIndex_{0};

  /// The nearest enclosing environment ID associated with this instruction.
  /// 0 means no valid ID.
  /// >= 1 refers to some Value* which can be found in the Function*.
  uint32_t environmentId_{0};

  /// The enclosing lexical scope that existed when this instruction was
  /// created. This is set by the IRBuilder.
  sema::LexicalScope *lexicalScope_{nullptr};

 protected:
  explicit Instruction(ValueKind kind) : Value(kind), Parent(nullptr) {}

  /// A constructor used for cloning instructions. It copies the \c kind and all
  /// other non-operand data from the original instruction. The operands are
  /// taken from \p operands (because the original operands are likely not
  /// useful).
  /// It deliberately doesn't take a reference as a first parameter since we
  /// don't want it to resemble a copy constructor (to avoid confusion).
  explicit Instruction(
      const Instruction *src,
      llvh::ArrayRef<Value *> operands);

  /// Add an operand to the operand list.
  void pushOperand(Value *Val);

  /// Implementation of \c getInherentType, default implementation returns
  /// None.
  ///
  /// Subclasses should override this and call it in the constructor if the
  /// instructions themselves have an inherent type.
  ///
  /// \return a Type if the instruction has an inherent type, None otherwise.
  static llvh::Optional<Type> getInherentTypeImpl() {
    return llvh::None;
  }

 public:
  /// All scope creation IDs of this value or higher represent an index into a
  /// list of environments maintained in the Function this instruction belongs
  /// to.
  static constexpr uint32_t kFirstScopeCreationIdIndex = 1;

  void setOperand(Value *Val, unsigned Index);
  Value *getOperand(unsigned Index) const;
  unsigned getNumOperands() const;
  void removeOperand(unsigned index);

  /// Returns a vector of flags indicating which operands the instruction writes
  /// to.
  WordBitSet<> getChangedOperands();

  /// An "inherent type" is a type that the instruction will _always_ have,
  /// regardless of the operands that it is provided with.
  /// The inherent type MUST be the tightest type bound that can be placed
  /// on the result of the instruction - it must not be possible to narrow
  /// the inherent type through any kind of type inference.
  ///
  /// If an Instruction has an inherent type, the constructor should set the
  /// type of the Instruction to the inherent type by calling
  /// \c getInherentTypeImpl.
  ///
  /// \return a Type if the instruction has an inherent type, None otherwise.
  llvh::Optional<Type> getInherentType();

  /// \return whether this instruction has an output value.
  bool hasOutput() const;

  /// \return whether this instruction is a typed instruction and should not,
  /// e.g. have its type inferred by TypeInference.
  bool isTyped() const;

  /// Returns true if any of the operands can have an "empty" or "uninit" type.
  bool acceptsEmptyType() const;

  /// Returns true if any of the operands can have an "empty" or "uninit" type.
  /// The default implementation returns false. This method has to be overridden
  /// by a few instructions that can handle an empty type (ThrowIf, Mov, etc).
  bool acceptsEmptyTypeImpl() const {
    return false;
  }

  void setLocation(SMLoc loc) {
    location_ = loc;
  }
  SMLoc getLocation() const {
    return location_;
  }
  bool hasLocation() const {
    return location_.isValid();
  }

  /// Update the statement that this Instruction belongs to.
  /// Set to 0 if the Instruction isn't part of a source statement.
  void setStatementIndex(uint32_t statementIndex) {
    statementIndex_ = statementIndex;
  }

  uint32_t getStatementIndex() const {
    return statementIndex_;
  }

  void setEnvironmentID(uint32_t enclosingScopeCreationID) {
    environmentId_ = enclosingScopeCreationID;
  }
  /// Set the environment ID of the instruction to a sentinel value indicating
  /// there's no valid environment ID information.
  void clearEnvironmentID() {
    environmentId_ = 0;
  }
  uint32_t getEnvironmentID() const {
    return environmentId_;
  }
  /// \return an index into the IR `Function`'s environment list.
  uint32_t getEnvironmentIDAsIndex() const {
    assert(environmentId_ >= kFirstScopeCreationIdIndex);
    return environmentId_ - kFirstScopeCreationIdIndex;
  }
  /// \return the implicit environment operand associated with this instruction.
  Value *getImplicitEnvOperand() const;

  void setLexicalScope(sema::LexicalScope *lexScope) {
    lexicalScope_ = lexScope;
  }
  sema::LexicalScope *getLexicalScope() {
    return lexicalScope_;
  }

  /// A debug utility that dumps the textual representation of the IR to the
  /// given ostream, defaults to stdout.
  void dump(llvh::raw_ostream &os = llvh::outs()) const;

  /// Replace the first operand from \p From to \p To. The value \p From must
  /// be an operand of the instruction. The method only replaces the first
  /// occurrence of \p From.
  void replaceFirstOperandWith(Value *OldValue, Value *NewValue);

  /// Move or insert an instruction at a given position. We deliberately do not
  /// have iterator versions of these methods because these functions rely on
  /// the position being a valid instruction (which is not the case with end
  /// iterators, for instance).
  void insertBefore(Instruction *InsertPos);
  void insertAfter(Instruction *InsertPos);
  void moveBefore(Instruction *Later);

  void removeFromParent();
  void eraseFromParent();

  /// Return the name of the instruction.
  llvh::StringRef getName();

  /// \returns the side effect of the instruction.
  SideEffect getSideEffect() const;

  Context &getContext() const;
  BasicBlock *getParent() const {
    return Parent;
  }
  void setParent(BasicBlock *parent) {
    Parent = parent;
  }

  /// \return the Function to which this Instruction belongs.
  inline Function *getFunction() const;

  /// \return the Module to which this Instruction belongs.
  inline Module *getModule() const;

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), Instruction);
  }

  /// Return the hash code the this instruction.
  /// (The instruction cannot be a Phi, since that might have loops.)
  llvh::hash_code getSimpleHashCode() const;

  /// Return true if \p RHS is equal to this instruction.
  bool isIdenticalTo(const Instruction *RHS) const;
};

} // namespace hermes

//===----------------------------------------------------------------------===//
// ilist_traits for Instruction
//===----------------------------------------------------------------------===//

namespace llvh {

template <>
struct ilist_alloc_traits<::hermes::Instruction> {
  static void deleteNode(::hermes::Instruction *V) {
    ::hermes::Value::destroy(V);
  }
};

} // namespace llvh

namespace hermes {

class BasicBlock : public llvh::ilist_node_with_parent<BasicBlock, Function>,
                   public Value {
  BasicBlock(const BasicBlock &) = delete;
  void operator=(const BasicBlock &) = delete;

 public:
  using InstListType = llvh::iplist<Instruction>;

 private:
  InstListType InstList{};
  Function *Parent;

 public:
  explicit BasicBlock(Function *parent);

  /// A debug utility that dumps the textual representation of the IR to \p os,
  /// defaults to stdout.
  void dump(llvh::raw_ostream &os = llvh::outs()) const;

  /// Used by LLVM's graph trait.
  void printAsOperand(llvh::raw_ostream &OS, bool) const;

  /// \brief Returns the terminator instruction if the block is well formed or
  /// null if the block is not well formed.
  TerminatorInst *getTerminator();
  const TerminatorInst *getTerminator() const;

  InstListType &getInstList() {
    return InstList;
  }

  void push_back(Instruction *I);
  void removeFromParent();
  void eraseFromParent();
  void remove(Instruction *I);
  void erase(Instruction *I);

  Context &getContext() const;
  Function *getParent() const {
    return Parent;
  }
  void setParent(Function *parent) {
    Parent = parent;
  }

  /// Needed by llvh::ilist_node_with_parent. Returns the offset of the
  /// aproperiate list based on the type of the argument.
  static InstListType BasicBlock::*getSublistAccess(Instruction *) {
    return &BasicBlock::InstList;
  }

  using iterator = InstListType::iterator;
  using const_iterator = InstListType::const_iterator;
  using reverse_iterator = InstListType::reverse_iterator;
  using const_reverse_iterator = InstListType::const_reverse_iterator;

  using range = llvh::iterator_range<iterator>;
  using const_range = llvh::iterator_range<const_iterator>;
  using reverse_range = llvh::iterator_range<reverse_iterator>;
  using const_reverse_range = llvh::iterator_range<const_reverse_iterator>;

  inline iterator begin() {
    return InstList.begin();
  }
  inline iterator end() {
    return InstList.end();
  }
  inline reverse_iterator rbegin() {
    return InstList.rbegin();
  }
  inline reverse_iterator rend() {
    return InstList.rend();
  }
  inline const_iterator begin() const {
    return InstList.begin();
  }
  inline const_iterator end() const {
    return InstList.end();
  }
  inline const_reverse_iterator rbegin() const {
    return InstList.rbegin();
  }
  inline const_reverse_iterator rend() const {
    return InstList.rend();
  }
  inline size_t size() const {
    return InstList.size();
  }
  inline bool empty() const {
    return InstList.empty();
  }
  inline Instruction &front() {
    return InstList.front();
  }
  inline const Instruction &front() const {
    return InstList.front();
  }
  inline Instruction &back() {
    return InstList.back();
  }
  inline const Instruction &back() const {
    return InstList.back();
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::BasicBlockKind;
  }
};

} // namespace hermes

//===----------------------------------------------------------------------===//
// ilist_traits for BasicBlock
//===----------------------------------------------------------------------===//

namespace llvh {

template <>
struct ilist_alloc_traits<::hermes::BasicBlock> {
  static void deleteNode(::hermes::BasicBlock *V) {
    ::hermes::Value::destroy(V);
  }
};

} // namespace llvh

namespace hermes {

/// VariableScope is a lexical scope. This inherits from ilist_node twice, once
/// for its owning list in the module, and again for the child list that each
/// scope is part of. The ilist_tags used are just templated on type of the
/// owner of the specific list, but they could be any type in principle.
class VariableScope
    : public Value,
      public llvh::ilist_node<VariableScope, llvh::ilist_tag<Module>>,
      public llvh::ilist_node<VariableScope, llvh::ilist_tag<VariableScope>> {
  using VariableListType = llvh::SmallVector<Variable *, 8>;
  using ChildListType =
      llvh::simple_ilist<VariableScope, llvh::ilist_tag<VariableScope>>;

  /// The variables associated with this scope.
  VariableListType variables_;

  /// The VariableScope representing the parent of this scope, or null if no
  /// such scope exists.
  VariableScope *parentScope_;

  /// The list of children of this VariableScope. This is necessary so we can
  /// update their parents if this scope is eliminated.
  ChildListType children_;

  /// The number of variables in this scope that are visible to the debugger.
  /// Defaulted to UINT32_MAX to indicate that it hasn't been assigned yet.
  uint32_t numVisibleVariables_ = UINT32_MAX;

 public:
  VariableScope(VariableScope *parentScope);

  /// \returns a list of variables.
  VariableListType &getVariables() {
    return variables_;
  }
  /// \returns a list of variables.
  llvh::ArrayRef<Variable *> getVariables() const {
    return variables_;
  }

  /// \returns a list of children.
  ChildListType &getChildren() {
    return children_;
  }

  /// \return whether assignIndexToVariables() has been called already.
  bool hasAssignedIndices() const {
    return numVisibleVariables_ != UINT32_MAX;
  }

  /// \return the number of variables that are visible in this scope.
  /// The first numVisibleVariables in this VariableScope are visible to the
  /// debugger.
  uint32_t getNumVisibleVariables() const {
    assert(numVisibleVariables_ != UINT32_MAX && "not assigned yet");
    return numVisibleVariables_;
  }

  /// Add a variable \p V to the variable list.
  void addVariable(Variable *V) {
    variables_.push_back(V);
  }

  /// Get the parent scope, which may be null if there isn't one.
  VariableScope *getParentScope() const {
    return parentScope_;
  }

  /// Update the parent scope of this scope.
  void setParentScope(VariableScope *parentScope);

  /// Remove this scope from the scope chain, by moving all of its children to
  /// instead be children of its parent.
  void removeFromScopeChain();

  /// Store the location of each Variable in the scope into the Variable itself.
  /// This is done after functions using the VariableScope have been register
  /// allocated, so the layout of environments corresponding to this
  /// VariableScope must no longer change (no Variables added or removed).
  void assignIndexToVariables();

  ~VariableScope() {
    // Free all variables.
    for (auto *v : variables_) {
      Value::destroy(v);
    }
  }

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::VariableScopeKind;
  }
};

class Function : public llvh::ilist_node_with_parent<Function, Module>,
                 public Value {
  Function(const Function &) = delete;
  void operator=(const Function &) = delete;

 public:
  using BasicBlockListType = llvh::iplist<BasicBlock>;

  enum class DefinitionKind {
    ES5Function,
    ES6BaseConstructor,
    ES6DerivedConstructor,
    ES6Arrow,
    ES6Method,
    // This corresponds to the synthetic function we create in IRGen, which is
    // the inner generator function passed as the argument to CreateGenerator.
    GeneratorInner,
    // This is a GeneratorInner function which is created as part of an async
    // arrow.
    GeneratorInnerArrow,
  };

  /// Enum describing restrictions on how this function may be invoked.
  enum class ProhibitInvoke {
    ProhibitNone,
    ProhibitConstruct,
    ProhibitCall,
  };

  /// \return true when every callsite of this Function is absolutely known
  /// and analyzable, even in the presence of Error structured stack trace.
  bool allCallsitesKnown() const {
    return getAttributes(parent_)._allCallsitesKnownInStrictMode && strictMode_;
  }

  /// \return true when every callsite of this Function is absolutely known
  /// and analyzable except for perhaps the usage of Error structured stack
  /// trace in loose mode.
  bool allCallsitesKnownExceptErrorStructuredStackTrace() const {
    return getAttributes(parent_)._allCallsitesKnownInStrictMode;
  }

  /// \return a ProhibitInvoke value representing restrictions on how this
  /// function may be invoked.
  ProhibitInvoke getProhibitInvoke() const;

 private:
  /// The Module owning this function.
  Module *parent_;

  /// The basic blocks in this function.
  BasicBlockListType BasicBlockList{};
  /// JS parameters, which are all technically optional. "this" is at index 0.
  llvh::SmallVector<JSDynamicParam *, 4> jsDynamicParams_{};
  /// Flag indicating whether the JS "this" dynamic param has been added to
  /// params.
  bool jsThisAdded_ = false;
  /// Parameter used as an operand in GetNewTarget to easily find all users.
  JSSpecialParam newTargetParam_;
  /// Parameter used as an operand in GetParentScope to easily find all users.
  JSSpecialParam parentScopeParam_;
  /// The user-specified original name of the function,
  /// or if not specified (e.g. anonymous), the inferred name.
  /// If there was no inference, an empty string.
  Identifier originalOrInferredName_;
  /// What kind of function is this - es5, constructor, etc.
  DefinitionKind const definitionKind_;
  /// Whether the function is in strict mode.
  const bool strictMode_{};
  /// The source location of the function.
  SMRange SourceRange{};
  /// Information on custom directives found in this function.
  CustomDirectives customDirectives_{};

  Type returnType_ = Type::createAnyType();

  /// The IR type of this function as a callable value (a TypeKind::Function
  /// type: interned, unless the signature is recursive), or the generic object
  /// type if the function is untyped. Set during IRGen for typed functions:
  /// for a typed generator or async function it is set on the outer function,
  /// whose closure user code receives, and not on the inner function, which
  /// CreateGeneratorInst creates and which is never a closure. Used as the
  /// type of CreateFunctionInst closures so they carry their real signature.
  Type functionType_{Type::createObject()};

  /// A name derived from \c originalOrInferredName_, but unique in the Module.
  /// Used only for printing and diagnostic.
  Identifier internalName_;

  /// The number of expected arguments, derived from the formal parameters given
  /// in the function signature.
  /// Only parameters up to the first parameter with an initializer are counted.
  /// See ES14.1.7 for details.
  uint32_t expectedParamCountIncludingThis_{0};

  /// The current statement count in this function.
  /// If statementCount_ > 0, it represents the current statement being
  /// generated.
  /// If statementCount == 0, then we haven't started generating statements.
  /// If statementCount_ is None we've completed generating statements
  /// and have cleared it in preparation for lowering steps.
  OptValue<uint32_t> statementCount_{0};

  /// The current sema::LexicalScope for the IR being generated in this
  /// function.
  /// lexicalScope_ can be null, indicating no lexical scope info is
  /// available.
  /// If lexicalScope_ is None we've completed IRGen and have cleared
  /// it in preparation for lowering steps.
  OptValue<sema::LexicalScope *> lexicalScope_{nullptr};

  /// The current environment ID.
  /// If environmentID_ == 0, then there's no information being recorded.
  /// If environmentID_ >= 1, it represents the most recent environment created
  /// in the function.
  /// If environmentID_ is None we've completed IRGen and have cleared it in
  /// preparation for lowering steps.
  OptValue<uint32_t> environmentID_{0};

  /// The two fields below are used only in "opt-to-fixed-point" compilation
  /// mode.

  /// The set of functions that have been inlined into this one.
  llvh::DenseSet<Function *> inlinedInto_;
  /// The set of functions that this function has been inlined into.
  llvh::DenseSet<Function *> inlinedBy_;

  /// Whether this inner generator function was created for an async function
  /// (as opposed to a user generator or transformed async generator).
  /// When true, the driver is spawnAsync which handles raw yields.
  /// When false and isInnerGenerator(), the driver may be AsyncGenerator
  /// which needs OverloadYield wrapping for internal awaits.
  bool isFromAsyncFunction_ = false;

  /// List of environments made by this function. Out of IRGen, this list is
  /// filled only with `CreateScopeInst`s. However, after generators have been
  /// lowered, this may contain `Variable`s for environments that have been
  /// spilled.
  llvh::SmallVector<Value *, 2> environments_;

 protected:
  explicit Function(
      ValueKind kind,
      Module *parent,
      Identifier originalName,
      DefinitionKind definitionKind,
      bool strictMode,
      CustomDirectives customDirectives,
      SMRange sourceRange,
      Function *insertBefore = nullptr);

 public:
  ~Function();

  Module *getParent() const {
    return parent_;
  }

  /// \returns whether this is the top level function (i.e. global scope).
  bool isGlobalScope() const;

  /// \return whether this is an anonymous function.
  bool isAnonymous() const {
    return originalOrInferredName_.str().empty();
  }

  /// \param isDescriptive whether in a more descriptive manner, e.g.
  /// "arrow function" instead of "arrow".
  /// \return the string representation of enum \c DefinitionKind.
  std::string getDefinitionKindStr(bool isDescriptive) const;

  /// \return the string representation in a more human readable manner that
  /// should be used in error messages. Currently it only prefixes a "anonymous"
  /// when it's needed, e.g. "anonymous function" instead of "function".
  std::string getDescriptiveDefinitionKindStr() const;

  /// \return the source code representation string of the function if its
  /// source visibility is non-default, or llvh::None if it's the default.
  /// Specifically, it returns the real source code for function declared with
  /// 'show source' and an empty string for function declared with 'hide source'
  /// or 'sensitive'. See comments in the implementation for details.
  llvh::Optional<llvh::StringRef> getSourceRepresentationStr() const;

  /// \returns the internal name of the function.
  const Identifier getInternalName() const {
    return internalName_;
  }

  /// \returns the string representation of internal name.
  llvh::StringRef getInternalNameStr() const {
    return internalName_.str();
  }

  /// \returns the original function name specified by the user,
  /// or if not specified, the inferred name.
  const Identifier getOriginalOrInferredName() const {
    return originalOrInferredName_;
  }

  /// \return the Context of the parent module.
  Context &getContext() const;

  void addBlock(BasicBlock *BB);

  /// Set \p newEntry as the entry block to this function.
  /// \p newEntry must already exist in this function's BasicBlockList.
  void moveBlockToEntry(BasicBlock *newEntry);

  /// Add a new JS parameter.
  void addJSDynamicParam(JSDynamicParam *param);
  /// Add the "this" JS parameter.
  void addJSThisParam(JSDynamicParam *param);
  /// \return true if JS "this" parameter was added.
  bool jsThisAdded() const {
    return jsThisAdded_;
  }

  Type getReturnType() const {
    return returnType_;
  }
  void setReturnType(Type returnType) {
    returnType_ = returnType;
  }

  /// \return the IR type of this function as a callable value: a Function
  /// type (interned, unless the signature is recursive), or object if the
  /// function is untyped. This is distinct
  /// from getType(), which is always functionCode.
  Type getFunctionType() const {
    return functionType_;
  }
  /// Set the IR type of this function as a callable value. Must be called
  /// before any CreateFunctionInst of this function is created, because its
  /// constructor copies the type into the closure.
  void setFunctionType(Type functionType) {
    functionType_ = functionType;
  }

  /// \return the new.target parameter.
  JSSpecialParam *getNewTargetParam() {
    return &newTargetParam_;
  }
  /// \return the new.target parameter.
  const JSSpecialParam *getNewTargetParam() const {
    return &newTargetParam_;
  }

  /// \return the parent scope parameter.
  JSSpecialParam *getParentScopeParam() {
    return &parentScopeParam_;
  }

  const BasicBlockListType &getBasicBlockList() const {
    return BasicBlockList;
  }
  BasicBlockListType &getBasicBlockList() {
    return BasicBlockList;
  }

  /// Remove the function from the module's main function list.
  /// Move the function to the compiled function list.
  void moveToCompiledFunctionList();

  /// Erase all the basic blocks and instructions in this function.
  /// Then remove the function from the module, remove all references.
  /// However this does not deallocate (destroy) the memory of this function.
  void eraseFromParentNoDestroy();

  /// Erase all the basic blocks and instructions in this function.
  /// Then remove the function from the compiled functions list,
  /// remove all references.
  /// However this does not deallocate (destroy) the memory of this function.
  void eraseFromCompiledFunctionsNoDestroy();

  /// A debug utility that dumps the textual representation of the IR to \p os,
  /// defaults to stdout.
  void dump(llvh::raw_ostream &os = llvh::outs()) const;

  /// Return the kind of function: constructor, arrow, etc.
  DefinitionKind getDefinitionKind() const {
    return definitionKind_;
  }

  /// Return whether the function is in strict mode.
  bool isStrictMode() const {
    return strictMode_;
  }

  /// Return the source range covered by the function.
  SMRange getSourceRange() const {
    return SourceRange;
  }

  /// Return the source visibility of the function.
  SourceVisibility getSourceVisibility() const {
    return customDirectives_.sourceVisibility;
  }

  /// \return whether the function should always be attempted to be inlined,
  /// bypassing the heuristics that normally determine that.
  bool getAlwaysInline() const {
    return customDirectives_.alwaysInline;
  }

  /// \return whether the function never be attempted to be inlined,
  /// bypassing the heuristics that normally determine that.
  bool getNoInline() const {
    return customDirectives_.noInline;
  }

  /// Set the function to be noInline.  Useful for certain cases, to
  /// preserve information useful for optimizations that would be erased
  /// by inlining.
  void setNoInline() {
    customDirectives_.noInline = true;
  }

  /// The four inlining-related functions below are used only in
  /// "opt-to-fixed-point" compilation mode.

  /// The set of functions that have been inlined into this one.
  llvh::DenseSet<Function *> &inlinedInto() {
    return inlinedInto_;
  };
  const llvh::DenseSet<Function *> &inlinedInto() const {
    return inlinedInto_;
  };

  /// The set of functions that this function has been inlined into.
  llvh::DenseSet<Function *> &inlinedBy() {
    return inlinedBy_;
  };
  const llvh::DenseSet<Function *> &inlinedBy() const {
    return inlinedBy_;
  };

  llvh::SmallVectorImpl<Value *> &environments() {
    return environments_;
  };

  OptValue<uint32_t> getStatementCount() const {
    return statementCount_;
  }

  void setStatementCount(uint32_t count) {
    statementCount_ = count;
  }

  /// Increments the statement count of the function.
  /// Requires that the statement count has never been cleared.
  void incrementStatementCount() {
    assert(
        statementCount_.hasValue() &&
        "cannot incrementStatementCount after clearing it");
    statementCount_ = *statementCount_ + 1;
  }

  void clearStatementCount() {
    statementCount_ = llvh::None;
  }

  OptValue<sema::LexicalScope *> getLexicalScope() const {
    return lexicalScope_;
  }
  void setLexicalScope(sema::LexicalScope *lexScope) {
    lexicalScope_ = lexScope;
  }
  void clearLexicalScope() {
    lexicalScope_ = llvh::None;
  }

  /// \return the current environmentID of the Function.
  OptValue<uint32_t> getEnvironmentID() const {
    return environmentID_;
  }

  /// Set the current environmentID of the Function to \p envID.
  void setEnvironmentID(uint32_t envID) {
    environmentID_ = envID;
  }

  /// Clear the environmentID, signaling the end of IRGen for this Function.
  void clearEnvironmentID() {
    environmentID_ = llvh::None;
  }

  auto &getJSDynamicParams() {
    return jsDynamicParams_;
  }

  const auto &getJSDynamicParams() const {
    return jsDynamicParams_;
  }
  /// \return a JS parameter by index. Index 0 is "this", 1 the first declared
  /// parameter, etc.
  JSDynamicParam *getJSDynamicParam(uint32_t index) const {
    return jsDynamicParams_[index];
  }

  void setExpectedParamCountIncludingThis(uint32_t count) {
    expectedParamCountIncludingThis_ = count;
  }

  /// \return the number of params this function takes, including 'this'. Should
  /// always return a number greater than 0.
  uint32_t getExpectedParamCountIncludingThis() const {
    assert(
        expectedParamCountIncludingThis_ > 0 &&
        "there should always be at least the 'this' parameter");
    return expectedParamCountIncludingThis_;
  }

  /// \return the LazyCompilationDataInst at the start of the function,
  /// or nullptr if there isn't one.
  const LazyCompilationDataInst *getLazyCompilationDataInst() const;
  LazyCompilationDataInst *getLazyCompilationDataInst();

  /// \return the EvalCompilationDataInst at the start of the function,
  /// or nullptr if there isn't one.
  EvalCompilationDataInst *getEvalCompilationDataInst();

  /// \return true if the function should be compiled lazily.
  bool isLazy() const {
    return getLazyCompilationDataInst() != nullptr;
  }

  using iterator = BasicBlockListType::iterator;
  using const_iterator = BasicBlockListType::const_iterator;
  using reverse_iterator = BasicBlockListType::reverse_iterator;
  using const_reverse_iterator = BasicBlockListType::const_reverse_iterator;

  inline iterator begin() {
    return BasicBlockList.begin();
  }
  inline iterator end() {
    return BasicBlockList.end();
  }
  inline reverse_iterator rbegin() {
    return BasicBlockList.rbegin();
  }
  inline reverse_iterator rend() {
    return BasicBlockList.rend();
  }
  inline const_iterator begin() const {
    return BasicBlockList.begin();
  }
  inline const_iterator end() const {
    return BasicBlockList.end();
  }
  inline const_reverse_iterator rbegin() const {
    return BasicBlockList.rbegin();
  }
  inline const_reverse_iterator rend() const {
    return BasicBlockList.rend();
  }
  inline size_t size() const {
    return BasicBlockList.size();
  }
  inline bool empty() const {
    return BasicBlockList.empty();
  }
  inline BasicBlock &front() {
    return BasicBlockList.front();
  }
  inline BasicBlock &back() {
    return BasicBlockList.back();
  }

  void viewGraph();

  /// \return true if this is a GeneratorInner or GeneratorInnerArrow function.
  bool isInnerGenerator() {
    auto defKind = getDefinitionKind();
    return defKind == DefinitionKind::GeneratorInner ||
        defKind == DefinitionKind::GeneratorInnerArrow;
  }

  /// \return true if this inner generator was created for an async function.
  bool isFromAsyncFunction() const {
    return isFromAsyncFunction_;
  }
  void setIsFromAsyncFunction(bool v) {
    isFromAsyncFunction_ = v;
  }

  static bool classof(const Value *V) {
    return HERMES_IR_KIND_IN_CLASS(V->getKind(), Function);
  }

  /// This is an RAII object that saves and restores the sema::LexicalScope of
  /// the Function. This can only be used during IRGen, since this expects that
  /// the lexical scope of the function is never `None` on construction.
  class ScopedLexicalScopeChange {
    ScopedLexicalScopeChange(const ScopedLexicalScopeChange &) = delete;
    void operator=(const ScopedLexicalScopeChange &) = delete;

    Function *F_;
    sema::LexicalScope *oldLexicalScope_;

   public:
    explicit ScopedLexicalScopeChange(Function *F, sema::LexicalScope *newScope)
        : F_(F), oldLexicalScope_(*F->getLexicalScope()) {
      if (newScope)
        F->setLexicalScope(newScope);
    }

    ~ScopedLexicalScopeChange() {
      // If since the constructor has run, the lexical scope has been cleared
      // out, we shouldn't attempt to set anything. The lexical scope being set
      // to `None` indicates the end of IRGen for the function, so we shouldn't
      // take it out of that state.
      if (F_->getLexicalScope().hasValue()) {
        F_->setLexicalScope(oldLexicalScope_);
      }
    }
  };
};

class NormalFunction final : public Function {
 public:
  /// \param parent Module this function will belong to.
  /// \param originalName User-specified function name, or an empty string.
  /// \param strictMode Whether this function uses strict mode.
  /// \param isGlobal Whether this is the global (top-level) function.
  /// \param sourceRange Range of source code this function comes from.
  /// \param insertBefore Another function in \p parent where this function
  ///   should be inserted before. If null, appends to the end of the module.
  explicit NormalFunction(
      Module *parent,
      Identifier originalName,
      DefinitionKind definitionKind,
      bool strictMode,
      CustomDirectives customDirectives,
      SMRange sourceRange,
      Function *insertBefore = nullptr)
      : Function(
            ValueKind::NormalFunctionKind,
            parent,
            originalName,
            definitionKind,
            strictMode,
            customDirectives,
            sourceRange,
            insertBefore) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::NormalFunctionKind;
  }
};

/// The "outer" generator function, used with CreateFunctionInst and invoked as
/// a normal function.
class GeneratorFunction final : public Function {
 public:
  explicit GeneratorFunction(
      Module *parent,
      Identifier originalName,
      DefinitionKind definitionKind,
      bool strictMode,
      CustomDirectives customDirectives,
      SMRange sourceRange,
      Function *insertBefore)
      : Function(
            ValueKind::GeneratorFunctionKind,
            parent,
            originalName,
            definitionKind,
            strictMode,
            customDirectives,
            sourceRange,
            insertBefore) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::GeneratorFunctionKind;
  }
};

class AsyncFunction final : public Function {
 public:
  explicit AsyncFunction(
      Module *parent,
      Identifier originalName,
      DefinitionKind definitionKind,
      bool strictMode,
      CustomDirectives customDirectives,
      SMRange sourceRange,
      Function *insertBefore)
      : Function(
            ValueKind::AsyncFunctionKind,
            parent,
            originalName,
            definitionKind,
            strictMode,
            customDirectives,
            sourceRange,
            insertBefore) {}

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::AsyncFunctionKind;
  }
};

} // namespace hermes

//===----------------------------------------------------------------------===//
// ilist_traits for Function
//===----------------------------------------------------------------------===//

namespace llvh {

template <>
struct ilist_alloc_traits<::hermes::Function> {
  static void deleteNode(::hermes::Function *V) {
    ::hermes::Value::destroy(V);
  }
};

template <>
struct ilist_alloc_traits<::hermes::VariableScope> {
  static void deleteNode(::hermes::VariableScope *V) {
    ::hermes::Value::destroy(V);
  }
};

} // namespace llvh

namespace hermes {

class LowerBuiltinCallsContext;

/// Shared state between optimization passes.
class OptimizationContext {
 public:
  /// Used by LowerBuiltinCalls to cache information about identifiers and
  /// builtin IDs.
  std::shared_ptr<LowerBuiltinCallsContext> lowerBuiltinCallsContext = nullptr;
};

class Module : public Value {
  Module(const Module &) = delete;
  void operator=(const Module &) = delete;

 public:
  using FunctionListType = llvh::iplist<Function>;
  using VariableScopeListType =
      llvh::iplist<VariableScope, llvh::ilist_tag<Module>>;

  using RawStringList = std::vector<LiteralString *>;

  struct CJSModule {
    /// ID of the module, allocated using a counter.
    uint32_t id;
    /// Filename for the module.
    Identifier filename;
    /// Pointer to the wrapper function for the module.
    Function *function;
  };

 private:
  std::shared_ptr<Context> Ctx;
  /// Type context for the IR type system. Marked mutable because type
  /// operations may intern derived union entries (e.g. unionTy,
  /// intersectTy) and must remain callable through a const Module.
  mutable TypeContext typeContext_;
  /// Optionally specify the top level function, if it isn't the first one.
  Function *topLevelFunction_{};

  FunctionListType FunctionList{};

  /// List of all the functions that have already been compiled,
  /// which are retained to ensure their instructions stay alive,
  /// importantly EvalCompilationDataInst and LazyCompilationDataInst.
  /// Kept separate to avoid iterating over these because they've already been
  /// compiled. We don't want to visit them during lowering, optimization, etc.
  FunctionListType compiledFunctions_{};

  /// List of all the VariableScopes owned by this module.
  ///
  /// Invariant: VariableScopes at the start of the list have Variables with
  /// assigned indices, while VariableScopes at the end of the list have
  /// Variables with unassigned indices. New VariableScopes are never added to
  /// the middle of the list, but they may be deleted from the middle of the
  /// list without violating the invariant.
  VariableScopeListType variableScopes_{};

  /// List of VariableScopes that may have no users, letting lazy
  /// compilation reclaim dead scopes without rescanning all of variableScopes_.
  ///
  /// Invariant: every user-less scope is a member, so no dead scope is missed.
  /// Membership does not imply user-less, so entries are re-checked before
  /// deletion.
  /// Maintained by addVariableScope() (new scopes) and
  /// markVariableScopeAsMaybeDead() (last user removed).
  /// Once it's iterated, this list is cleared but a VariableScope may be
  /// re-added by one of the above mechanisms.
  llvh::SmallPtrSet<VariableScope *, 16> maybeDeadVariableScopes_{};

  GlobalObject globalObject_{};
  LiteralEmpty literalEmpty{};
  LiteralUninit literalUninit_{};
  LiteralUndefined literalUndefined{};
  LiteralNull literalNull{};
  LiteralBool literalFalse{false};
  LiteralBool literalTrue{true};
  EmptySentinel emptySentinel_{};

  // Uniqued values.

  /// Utility class to create elements that are subclasses of Value in the
  /// folding set. This is necessary because calling new on a type requires
  /// access to its deleter when exceptions are enabled, but the deleter of
  /// Value is private.
  template <typename T>
  struct ValueCreator {
    template <typename... Args>
    static T *create(Args &&...args) {
      return new T(std::forward<Args>(args)...);
    }
  };

  template <typename T>
  using ValueOFS = OwningFoldingSet<T, ValueCreator<T>, ValueDeleter>;

  ValueOFS<LiteralNumber> literalNumbers_{};
  ValueOFS<LiteralBigInt> literalBigInts_{};
  ValueOFS<LiteralString> literalStrings_{};
  ValueOFS<LiteralPrivateName> literalPrivateNames_{};
  ValueOFS<GlobalObjectProperty> globalProperties_{};
  ValueOFS<LiteralBuiltinIdx> literalBuiltinIdxs_{};
  ValueOFS<LiteralIRType> literalIRTypes_{};
  ValueOFS<LiteralTypeOfIsTypes> literalTypeOfIsTypes_{};
  ValueOFS<LiteralNativeSignature> nativeSignatures_{};
  ValueOFS<LiteralNativeExtern> nativeExterns_{};

  /// Map from an identifier to a number indicating how many times it has been
  /// used. This allows to construct unique internal names derived from regular
  /// identifiers.
  llvh::DenseMap<Identifier, unsigned> internalNamesMap_;

  /// Storage for the CJS modules.
  /// Use a deque to allow for non-copying push_back, to avoid invalidating the
  /// pointers in cjsModuleFunctionMap_ below.
  std::deque<CJSModule> cjsModules_{};

  using CJSModuleIterator = std::deque<CJSModule>::iterator;

  /// Map from functions to members of the CJS module storage.
  llvh::DenseMap<Function *, CJSModule *> cjsModuleFunctionMap_{};
  /// Map from file names to CJS module IDs.
  llvh::DenseMap<Identifier, uint32_t> cjsModuleFilenameMap_{};
  /// Map from segment IDs to CJS module wrapper functions.
  /// The vectors in the map have no duplicates; this is enforced using
  /// cjsModuleFunctionMap_.
  llvh::DenseMap<uint32_t, std::vector<Function *>> cjsModuleSegmentMap_{};

  /// true if all CJS modules have been resolved.
  bool cjsModulesResolved_{false};

  using CJSModuleUseGraph =
      std::unordered_map<Function *, llvh::SmallPtrSet<Function *, 2>>;

  /// Graph which maps from a function F to a set of functions which it uses.
  /// Used to determine what functions must be generated for a given segment.
  /// Lazily generated the first time bytecode is generated for a split bundle,
  /// and used when generating bytecode for a given segment.
  /// If empty, this has not been generated yet.
  CJSModuleUseGraph cjsModuleUseGraph_{};

  /// The (JS) module factory function for the (hermes) module.
  llvh::DenseSet<Function *> jsModuleFactoryFunctions_;

  struct HashRawStrings {
    std::size_t operator()(const RawStringList &rawStrings) const {
      return llvh::hash_combine_range(rawStrings.begin(), rawStrings.end());
    }
  };

  /// A map from a list of raw strings from a template literal to its unique id.
  std::unordered_map<RawStringList, uint32_t, HashRawStrings>
      templateObjectIDMap_;

  /// Set to true when lowerIR has been called on this Module.
  bool isLowered_{false};

  /// Set to true when generator functions have been lowered.
  bool areGeneratorsLowered_{false};

  /// Extra data to be shared between optimization passes.
  OptimizationContext optContext_{};

 public:
  explicit Module(std::shared_ptr<Context> ctx)
      : Value(ValueKind::ModuleKind), Ctx(std::move(ctx)) {}

  ~Module();

  Context &getContext() const {
    return *Ctx;
  }

  /// Return the IR type context for this module. Returns a mutable reference
  /// even from a const Module because type operations may intern derived
  /// union entries.
  TypeContext &getTypeContext() const {
    return typeContext_;
  }

  std::shared_ptr<Context> shareContext() const {
    return Ctx;
  }

  /// Derive a unique internal name from a specified identifier.
  Identifier deriveUniqueInternalName(Identifier originalName);

  using iterator = FunctionListType::iterator;
  using const_iterator = FunctionListType::const_iterator;
  using reverse_iterator = FunctionListType::reverse_iterator;
  using const_reverse_iterator = FunctionListType::const_reverse_iterator;

  /// Add a new function to the module.
  void push_back(Function *F);
  /// Insert a new function at a specific position in the module.
  void insert(iterator position, Function *F);

  const FunctionListType &getFunctionList() const {
    return FunctionList;
  }
  FunctionListType &getFunctionList() {
    return FunctionList;
  }

  const FunctionListType &getCompiledFunctionList() const {
    return compiledFunctions_;
  }
  FunctionListType &getCompiledFunctionList() {
    return compiledFunctions_;
  }

  /// Set the top-level function of this module - the function that will be
  /// executed when the module is executed.
  void setTopLevelFunction(Function *topLevelFunction) {
    assert(
        topLevelFunction->getParent() == this &&
        "topLevelFunction from a different module");
    assert(!topLevelFunction_ && "Top level function is already set.");
    topLevelFunction_ = topLevelFunction;
  }

  /// Return the top-level function.
  Function *getTopLevelFunction() {
    assert(topLevelFunction_ && "top-level function hasn't been created yet");
    // If the top level function hasn't been overridden, return the first
    // function.
    return topLevelFunction_;
  }

  /// \return the list of variable scopes owned by this module.
  /// NOTE: DO NOT append or remove from the returned list.
  /// ONLY use addVariableScope and destroyVariableScope for that.
  VariableScopeListType &getVariableScopes() {
    return variableScopes_;
  }
  /// \return the list of variable scopes owned by this module.
  /// NOTE: DO NOT append or remove from the returned list.
  /// ONLY use addVariableScope and destroyVariableScope for that.
  const VariableScopeListType &getVariableScopes() const {
    return variableScopes_;
  }

  /// Add a VariableScope owned by this Module.
  void addVariableScope(VariableScope *varScope) {
    variableScopes_.push_back(varScope);
    maybeDeadVariableScopes_.insert(varScope);
  }

  /// Remove and destroy a VariableScope owned by this Module.
  /// \pre \p variableScope has no users.
  void destroyVariableScope(VariableScope *varScope);

  /// Remember that a VariableScope lost its last user.
  void markVariableScopeAsMaybeDead(VariableScope *varScope) {
    maybeDeadVariableScopes_.insert(varScope);
  }

  OptimizationContext &getOptimizationContext() {
    return optContext_;
  }
  const OptimizationContext &getOptimizationContext() const {
    return optContext_;
  }

  /// Assign indices to Variables in newly-created VariableScopes.
  void assignIndexToVariables() {
    // Iterate variableScopes_ backwards, and stop at the first one that has
    // already been assigned indices. This prevents rescanning VariableScopes
    // unnecessarily and causing slowdown.
    // See the invariant on variableScopes_ for why this works.
    for (auto it = variableScopes_.rbegin(); it != variableScopes_.rend();
         ++it) {
      VariableScope *varScope = &*it;
      if (varScope->hasAssignedIndices())
        break;
      varScope->assignIndexToVariables();
    }
  }

  /// Create the specified global property if it doesn't exist. If it does
  /// exist, its declared property is updated by a logical OR.
  GlobalObjectProperty *addGlobalProperty(Identifier name, bool declared);

  /// Create a new literal number of value \p value.
  LiteralNumber *getLiteralNumber(double value);

  /// Create a new literal BigInt of value \p value.
  LiteralBigInt *getLiteralBigInt(UniqueString *value);

  /// Create a new literal string of value \p value.
  LiteralString *getLiteralString(Identifier value);

  /// Create a new literal private name of value \p value.
  LiteralPrivateName *getLiteralPrivateName(Identifier value);

  /// Create a new literal bool of value \p value.
  LiteralBool *getLiteralBool(bool value);

  /// Create a new literal representing an IR type.
  LiteralBuiltinIdx *getLiteralBuiltinIdx(BuiltinMethod::Enum builtinIdx);

  /// Create a new literal representing an IR type.
  LiteralIRType *getLiteralIRType(Type value);

  /// Create a new literal representing TypeOfIsTypes.
  LiteralTypeOfIsTypes *getLiteralTypeOfIsTypes(TypeOfIsTypes value);

  /// Create a new LiteralNativeSignature.
  LiteralNativeSignature *getLiteralNativeSignature(NativeSignature *data);

  /// Create a new LiteralNativeExtern.
  LiteralNativeExtern *getLiteralNativeExtern(NativeExtern *data);

  /// Create a new literal 'empty'.
  LiteralEmpty *getLiteralEmpty() {
    return &literalEmpty;
  }

  /// Return the Uninit literal singleton.
  LiteralUninit *getLiteralUninit() {
    return &literalUninit_;
  }

  /// Create a new literal 'undefined'.
  LiteralUndefined *getLiteralUndefined() {
    return &literalUndefined;
  }

  /// Create a new literal null.
  LiteralNull *getLiteralNull() {
    return &literalNull;
  }

  /// Return the GlobalObject value.
  GlobalObject *getGlobalObject() {
    return &globalObject_;
  }

  /// Return the shared instance of EmptySentinel.
  EmptySentinel *getEmptySentinel() {
    return &emptySentinel_;
  }

  /// Add a new CJS module entry, given the function representing the module.
  void addCJSModule(
      uint32_t segmentID,
      uint32_t id,
      Identifier name,
      Function *function) {
    cjsModules_.emplace_back(CJSModule{id, name, function});
    CJSModule &module = cjsModules_.back();
    {
      auto result = cjsModuleFilenameMap_.try_emplace(name, id);
      (void)result;
      assert(
          (result.second || result.first->second == id) &&
          "Module must have the same ID for the same name");
    }
    {
      auto result = cjsModuleFunctionMap_.try_emplace(function, &module);
      (void)result;
      assert(
          result.second && "Should only insert each CJS wrapper function once");
    }
    cjsModuleSegmentMap_[segmentID].push_back(function);
  }

  /// \return the CommonJS module given the wrapping function if it is a module,
  /// else nullptr.
  const CJSModule *findCJSModule(Function *function) const {
    auto it = cjsModuleFunctionMap_.find(function);
    return it == cjsModuleFunctionMap_.end() ? nullptr : it->second;
  }

  /// \return the CommonJS module ID given the filename if it is a module,
  /// else nullptr.
  llvh::Optional<uint32_t> findCJSModuleID(Identifier filename) const {
    auto it = cjsModuleFilenameMap_.find(filename);
    if (it != cjsModuleFilenameMap_.end()) {
      return it->second;
    }
    return llvh::None;
  }

  /// \return the registered CommonJS modules.
  llvh::iterator_range<CJSModuleIterator> getCJSModules() {
    return {cjsModules_.begin(), cjsModules_.end()};
  }

  /// \return true if all CJS modules in this Module have been resolved.
  bool getCJSModulesResolved() const {
    return cjsModulesResolved_;
  }

  /// Set to true if all CJS modules in this Module have been resolved.
  void setCJSModulesResolved(bool cjsModulesResolved) {
    cjsModulesResolved_ = cjsModulesResolved;
  }

  /// The (JS) module factory function for the (hermes) module.
  llvh::DenseSet<Function *> &jsModuleFactoryFunctions() {
    return jsModuleFactoryFunctions_;
  }
  const llvh::DenseSet<Function *> &jsModuleFactoryFunctions() const {
    return jsModuleFactoryFunctions_;
  }

  /// \return the set of functions which are used by the modules in the segment
  /// specified by \p segment. Order is unspecified, so the return value
  /// should not be used for iteration, only for checking membership.
  llvh::DenseSet<Function *> getFunctionsInSegment(uint32_t segment);

  /// \param rawStrings a list of raw strings from a template literal
  /// \return (rawStrings, id) where rawStrings is the stored string list,
  ///   and the id is the newly allocated template object ID.
  std::pair<llvh::ArrayRef<LiteralString *>, uint32_t> emplaceTemplateObject(
      RawStringList &&rawStrings);

  bool isLowered() const {
    return isLowered_;
  }

  void setLowered(bool isLowered) {
    isLowered_ = isLowered;
  }

  bool areGeneratorsLowered() const {
    return areGeneratorsLowered_;
  }

  void setGeneratorsLowered(bool lowered) {
    areGeneratorsLowered_ = lowered;
  }

  inline iterator begin() {
    return FunctionList.begin();
  }
  inline iterator end() {
    return FunctionList.end();
  }
  inline reverse_iterator rbegin() {
    return FunctionList.rbegin();
  }
  inline reverse_iterator rend() {
    return FunctionList.rend();
  }
  inline const_iterator begin() const {
    return FunctionList.begin();
  }
  inline const_iterator end() const {
    return FunctionList.end();
  }
  inline const_reverse_iterator rbegin() const {
    return FunctionList.rbegin();
  }
  inline const_reverse_iterator rend() const {
    return FunctionList.rend();
  }
  inline size_t size() const {
    return FunctionList.size();
  }
  inline bool empty() const {
    return FunctionList.empty();
  }
  inline Function &front() {
    return FunctionList.front();
  }
  inline Function &back() {
    return FunctionList.back();
  }

  /// Delete all non top-level non-lazy functions and set the Module state to be
  /// ready for compiling more code.
  /// Delete unused VariableScopes to avoid unnecessary memory usage.
  /// Avoids destroying a Module completely,
  /// but allows it to be reused for another purpose.
  void resetForMoreCompilation();

  void viewGraph();
  void dump(llvh::raw_ostream &os = llvh::outs()) const;

  static bool classof(const Value *V) {
    return V->getKind() == ValueKind::ModuleKind;
  }

  /// The hash_code of the Module.  This is intended to detect changes made
  /// by optimization passes; aspects not changed by such passes may
  /// not be included in the hash value.
  llvh::hash_code hash() const;

 private:
  /// Calculate the CJS module function graph, if it hasn't been calculated yet.
  /// Caches the result in cjsModuleUseGraph_.
  void populateCJSModuleUseGraph();
};

/// The hash of a Type is the hash of its opaque value.
static inline llvh::hash_code hash_value(Type V) {
  return V.hash();
}

inline bool Value::tracksUsers() const {
  // We do not track users of literals because we never need to enumerate them
  // and they can be very costly to maintain.
  return !llvh::isa<Literal>(this);
}

inline Function *Instruction::getFunction() const {
  return Parent->getParent();
}
inline Module *Instruction::getModule() const {
  return getFunction()->getParent();
}

} // end namespace hermes

namespace llvh {

raw_ostream &operator<<(raw_ostream &OS, const hermes::Type &T);

} // namespace llvh

#endif
