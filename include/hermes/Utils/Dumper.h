/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_UTILS_DUMPER_H
#define HERMES_UTILS_DUMPER_H

#include <map>

#include "hermes/IR/IRVisitor.h"
#include "llvh/ADT/StringRef.h"

namespace llvh {
class raw_ostream;
} // namespace llvh

namespace hermes {
class Value;
class Argument;
class Instruction;
class BasicBlock;
class Function;
class Module;

class CondBranchInst;
class AllocaInst;
class ReturnInst;
class JSDynamicParam;
class BranchInst;

namespace irdumper {

/// Display a nice dotty graph that depicts the function.
void viewGraph(Function *F);

/// A utility class for naming IR values. This should only be used for
/// pretty-printing instructions and basic blocks.
class ValueNamer {
  /// The kind of the value associated with this number, and the actual number.
  struct ValueT {
    /// The kind of the value for which this number was allocated. If the kind
    /// changes, a new number will be allocated. A kind can change when an
    /// instruction is freed and a new one is allocated at the same address.
    ValueKind kind;

    /// Generation of the namer when this number was visited. Unvisited values
    /// are "garbage collected".
    uint8_t visitedGen;

    /// The unique (within a function) number label associated with this value.
    unsigned number;

    ValueT(ValueKind kind, uint8_t visitedGen, unsigned int number)
        : kind(kind), visitedGen(visitedGen), number(number) {}
  };

  /// Map from an IR Value pointer, to a number associated with it, plus some
  /// extra data.
  llvh::DenseMap<const Value *, ValueT> map_{};

  /// The current generation. This is stamped in every value when we visit it.
  /// When the generation changes, all values that were not visited in the last
  /// generation are removed from the map.
  uint8_t currentGen_ = 0;

  /// Next number to allocate.
  unsigned counter_ = 0;

 public:
  ValueNamer() = default;

  /// Clear the map and reset the counter.
  void clear();

  /// Advance the generation. This invalidates all numbers that were not visited
  /// in the previous generation.
  void nextGeneration();

  /// Return the number associated with \p v. If \p v is not in the map, or is
  /// in the map bit associated with a different kind, a new number is
  /// allocated.
  unsigned getNumber(const Value *v);
};

/// Utility class to print unique variable name within a function.
class VariableNamer {
  /// Map from a scope+name to number of occurrences.
  llvh::DenseMap<std::pair<VariableScope *, Identifier>, unsigned>
      namesCounts_{};
  /// Map from variable to suffix.
  llvh::DenseMap<Variable *, unsigned> varMap_{};

 public:
  struct Name {
    Identifier name;
    unsigned suffix;
  };

  Name getName(Variable *var);
};

/// This class holds all state necessary for naming things in IR dumps.
class Namer {
  /// State needed per IR Function.
  struct PerFunction {
    ValueNamer instNamer;
    ValueNamer bbNamer;

    void nextGeneration() {
      instNamer.nextGeneration();
      bbNamer.nextGeneration();
    }

    void clear() {
      instNamer.clear();
      bbNamer.clear();
    }
  };
  VariableNamer varNamer{};

  /// Namer for scope metadata.
  ValueNamer scopeNamer{};

  /// Whether the state should persist across functions.
  bool const persistent_;

  /// Associates per-function state with an IR Function. Only in "persistent"
  /// mode.
  llvh::DenseMap<const Function *, std::unique_ptr<PerFunction>> functionMap_{};

  /// State that is used in non-persistent mode.
  std::unique_ptr<PerFunction> nonPersistentState_{};

  /// The current state.
  PerFunction *curFunctionState_ = nullptr;

 public:
  explicit Namer(bool persistent) : persistent_(persistent) {
    if (!persistent_) {
      nonPersistentState_ = std::make_unique<PerFunction>();
      curFunctionState_ = nonPersistentState_.get();
    }
  }

  /// Restore the function state for the given \p F. In contrast to newFunction,
  /// this will not invoke nextGeneration on the function state. This function
  /// is meant to prepare the dumper to retrieve BB/Inst labels. As such, this
  /// should only be used when in persistent mode, and when the function has
  /// already been visited.
  void restoreFunctionState(const Function *F) {
    assert(persistent_ && "Not in persistent mode");
    assert(functionMap_.count(F) && "Function hasn't been visited before");
    curFunctionState_ = functionMap_[F].get();
  }

  void newFunction(const Function *F) {
    if (persistent_) {
      auto [it, inserted] =
          functionMap_.try_emplace(F, std::unique_ptr<PerFunction>());
      if (inserted)
        it->second = std::make_unique<PerFunction>();
      curFunctionState_ = it->second.get();
      curFunctionState_->nextGeneration();
    } else {
      curFunctionState_->clear();
    }
  }

  /// Return the number associated with \p inst.
  unsigned getInstNumber(const Instruction *inst) {
    return curFunctionState_->instNamer.getNumber(inst);
  }
  /// Return the number associated with \p bb.
  unsigned getBBNumber(const BasicBlock *bb) {
    return curFunctionState_->bbNamer.getNumber(bb);
  }
  /// Return the unique printable name associated with \p var.
  VariableNamer::Name getVarName(Variable *var) {
    return varNamer.getName(var);
  }
  /// Return the number associated with \p VS.
  unsigned getScopeNumber(const VariableScope *VS) {
    return scopeNamer.getNumber(VS);
  }
};

llvh::raw_ostream &operator<<(
    llvh::raw_ostream &os,
    const VariableNamer::Name &n);

#if !defined(NDEBUG) || !defined(HERMES_IS_MOBILE_BUILD)

class IRPrinter : public IRVisitor<IRPrinter, void> {
 protected:
  /// Indentation level.
  unsigned indent_;

  Context &ctx_;
  SourceErrorManager &sm_;
  /// Output stream.
  llvh::raw_ostream &os_;
  /// Whether to show colors.
  bool colors_;
  /// If set to true then we need to escape the quote mark because the output of
  /// this printer may be printed as a quoted label.
  bool needEscape_;

  /// A non-peristent namer used when one isn't provided by Context.
  std::unique_ptr<Namer> tempNamer_;

  /// The set of scopes that have been dumped. This is used to ensure that
  /// scopes are only dumped once right before the first function that uses
  /// them.
  llvh::DenseSet<VariableScope *> dumpedScopes_;

 public:
  /// Indexes in a pallette of colors for IR dumps.
  enum class Color : uint8_t {
    // Default color.
    None,
    // Color of an instruction.
    Inst,
    // Color of type annotation like :number.
    Type,
    // Color of a name like %10.
    Name,
    // Color of a register name.
    Register,
    _last
  };

  /// State for naming values and variables.
  Namer &namer_;

  /// If set to true then we should output a label for all instructions. If not,
  /// then instructions that don't have an output will not have a label printed.
  bool labelAllInsts_;

  /// \param ctx  the Context
  /// \param usePersistent whether to use the persistent namer from Context
  /// \param ost  output stream
  /// \param escape whether to escape the quote mark.
  /// \param labelAllInsts whether to include labels on all instructions
  explicit IRPrinter(
      Context &ctx,
      bool usePersistent,
      llvh::raw_ostream &ost,
      bool escape,
      bool labelAllInsts);

  explicit IRPrinter(
      Context &ctx,
      llvh::raw_ostream &ost,
      bool escape = false,
      bool labelAllInsts = false)
      : IRPrinter(ctx, true, ost, escape, labelAllInsts) {}

  /// Force colors to off.
  void disableColors() {
    colors_ = false;
  }

  virtual ~IRPrinter() = default;

  virtual void printFunctionHeader(Function *F);
  virtual void printValueLabel(Instruction *I, Value *V, unsigned opIndex);
  virtual void printTypeLabel(Value *v);
  virtual void printInstruction(Instruction *I);
  /// Return true if the destination is non-empty.
  virtual bool printInstructionDestination(Instruction *I);
  virtual void printSourceLocation(SMLoc loc);
  virtual void printSourceLocation(SMRange rng);

  std::string getQuoteSign() {
    return needEscape_ ? R"(\")" : R"(")";
  }

  /// Quote the string if it has spaces.
  std::string quoteStr(llvh::StringRef name);

  /// Escapes the string if it has non-printable characters.
  std::string escapeStr(llvh::StringRef name);

  /// Declare the functions we are going to reimplement.
  void visitInstruction(const Instruction &I);
  void visitBasicBlock(const BasicBlock &BB);
  void visitFunction(const Function &F);
  void visitVariableScope(const VariableScope &VS);
  void visitFunction(const Function &F, llvh::ArrayRef<BasicBlock *> order);
  void visitModule(const Module &M);

  /// Set the output color to \p Color. Do nothing if colors are disabled.
  void setColor(Color color);
  /// Set the output color to the default color. Do nothing if colors are
  /// disabled.
  void resetColor();

  /// Invoke llvh::raw_ostream::changeColor() if colors are enabled, otherwise
  /// do nothing.
  void _changeColor(
      llvh::raw_ostream::Colors Color,
      bool Bold = false,
      bool BG = false);
};

#else

/// An IRPrinter that prints nothing.
class IRPrinter {
 public:
  explicit IRPrinter(
      Context &,
      llvh::raw_ostream &,
      bool /* escape */ = false,
      bool /* labelAllInsts */ = false) {}

  void visit(const Value &) {}
  void visitFunction(const Function &) {}
  void visitFunction(const Function &, llvh::ArrayRef<BasicBlock *>) {}
};

#endif // !defined(NDEBUG) || !defined(HERMES_IS_MOBILE_BUILD)

} // namespace irdumper

} // namespace hermes

#endif
