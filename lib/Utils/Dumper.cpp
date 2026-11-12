/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cctype>
#include <string>

#include "llvh/ADT/DenseSet.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/Support/Casting.h"
#include "llvh/Support/ErrorHandling.h"
#include "llvh/Support/GraphWriter.h"
#include "llvh/Support/raw_ostream.h"

#include "hermes/AST/Context.h"
#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRVisitor.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Support/Statistic.h"
#include "hermes/Utils/Dumper.h"

using llvh::dyn_cast;
using llvh::isa;

namespace hermes::irdumper {

#if !defined(NDEBUG) || !defined(HERMES_IS_MOBILE_BUILD)

IRPrinter::IRPrinter(
    Context &ctx,
    bool usePersistent,
    llvh::raw_ostream &ost,
    bool escape,
    bool labelAllInsts)
    : indent_(0),
      ctx_(ctx),
      sm_(ctx.getSourceErrorManager()),
      os_(ost),
      colors_(ctx.getCodeGenerationSettings().colors && os_.has_colors()),
      needEscape_(escape),
      tempNamer_(
          usePersistent && ctx.getPersistentIRNamer()
              ? nullptr
              : new Namer(usePersistent)),
      namer_(tempNamer_ ? *tempNamer_ : *ctx.getPersistentIRNamer()),
      labelAllInsts_(labelAllInsts) {}

std::string IRPrinter::escapeStr(llvh::StringRef name) {
  std::string s = name.str();
  std::string out;
  out += getQuoteSign();
  for (std::string::const_iterator i = s.begin(); i != s.end(); ++i) {
    unsigned char c = *i;
    if (std::isprint(c) && c != '\\' && c != '"') {
      out += c;
    } else {
      out += "\\\\";
      switch (c) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\t':
          out += 't';
          break;
        case '\r':
          out += 'r';
          break;
        case '\n':
          out += 'n';
          break;
        default:
          char const *const hexdig = "0123456789ABCDEF";
          out += 'x';
          out += hexdig[c >> 4];
          out += hexdig[c & 0xF];
      }
    }
  }
  out += getQuoteSign();
  return out;
}

std::string IRPrinter::quoteStr(llvh::StringRef name) {
  if (name.count(" ") || name.empty()) {
    return getQuoteSign() + name.str() + getQuoteSign();
  }
  return name.str();
}

#endif // !defined(NDEBUG) || !defined(HERMES_IS_MOBILE_BUILD)

void ValueNamer::clear() {
  currentGen_ = 0;
  counter_ = 0;
  map_.clear();
}

void ValueNamer::nextGeneration() {
  for (auto it = map_.begin(), end = map_.end(); it != end;) {
    auto cur = it++;
    if (cur->second.visitedGen != currentGen_) {
      map_.erase(cur);
    }
  }

  currentGen_ ^= 1;
}

unsigned ValueNamer::getNumber(const Value *v) {
  ValueKind kind = v->getKind();
  auto [it, inserted] =
      map_.try_emplace(v, ValueT(kind, currentGen_, counter_));
  if (inserted) {
    ++counter_;
  } else {
    if (it->second.visitedGen != currentGen_)
      it->second.visitedGen = currentGen_;
    if (it->second.kind != kind) {
      // If the kind of the value has changed, reset the counter.
      it->second.kind = kind;
      it->second.number = counter_++;
    }
  }
  return it->second.number;
}

VariableNamer::Name VariableNamer::getName(Variable *var) {
  auto [varIt, newVar] = varMap_.try_emplace(var, 0);
  if (newVar) {
    varIt->second =
        ++namesCounts_[std::make_pair(var->getParent(), var->getName())];
  }

  return Name{var->getName(), varIt->second - 1};
}

llvh::raw_ostream &operator<<(
    llvh::raw_ostream &os,
    const VariableNamer::Name &n) {
  // If the name is empty or contains spaces, quote it.
  if (n.name.str().count(' ') || n.name.str().empty())
    os << '"' << n.name << '"';
  else
    os << n.name;
  if (n.suffix)
    os << "#" << n.suffix;
  return os;
}

#if !defined(NDEBUG) || !defined(HERMES_IS_MOBILE_BUILD)

void IRPrinter::printTypeLabel(Value *v) {
  // Don't print the type of basic blocks or certain literals.
  if (llvh::isa<BasicBlock>(v) || llvh::isa<LiteralIRType>(v) ||
      llvh::isa<LiteralTypeOfIsTypes>(v))
    return;
  setColor(Color::Type);
  os_ << ": " << v->getType();
  resetColor();
}

void IRPrinter::printValueLabel(Instruction *I, Value *V, unsigned opIndex) {
  auto &ctx = I->getContext();
  if (auto *builtinIdx = dyn_cast<LiteralBuiltinIdx>(V)) {
    os_ << "[" << getBuiltinMethodName(builtinIdx->getData()) << "]";
  } else if (auto LBI = dyn_cast<LiteralBigInt>(V)) {
    os_ << LBI->getValue()->str();
  } else if (auto LS = dyn_cast<LiteralString>(V)) {
    os_ << escapeStr(ctx.toString(LS->getValue()));
  } else if (auto LS = dyn_cast<LiteralPrivateName>(V)) {
    os_ << "private " << escapeStr(ctx.toString(LS->getValue()));
  } else if (auto LB = dyn_cast<LiteralBool>(V)) {
    os_ << (LB->getValue() ? "true" : "false");
  } else if (auto LN = dyn_cast<LiteralNumber>(V)) {
    const auto Num = LN->getValue();
    if (Num == 0 && std::signbit(Num)) {
      // Ensure we output -0 correctly
      os_ << "-0";
    } else {
      char buf[NUMBER_TO_STRING_BUF_SIZE];
      numberToString(LN->getValue(), buf, sizeof(buf));
      os_ << buf;
    }
  } else if (isa<LiteralEmpty>(V)) {
    os_ << "empty";
  } else if (isa<LiteralUninit>(V)) {
    os_ << "uninit";
  } else if (isa<LiteralNull>(V)) {
    os_ << "null";
  } else if (isa<LiteralUndefined>(V)) {
    os_ << "undefined";
  } else if (isa<GlobalObject>(V)) {
    os_ << "globalObject";
  } else if (isa<EmptySentinel>(V)) {
    os_ << "empty";
  } else if (isa<Instruction>(V)) {
    setColor(Color::Name);
    os_ << "%" << namer_.getInstNumber(llvh::cast<Instruction>(V));
    resetColor();
  } else if (isa<BasicBlock>(V)) {
    setColor(Color::Name);
    os_ << "%BB" << namer_.getBBNumber(llvh::cast<BasicBlock>(V));
    resetColor();
  } else if (auto L = dyn_cast<Label>(V)) {
    auto Name = L->get();
    os_ << "$" << quoteStr(ctx.toString(Name));
  } else if (auto P = dyn_cast<JSDynamicParam>(V)) {
    auto Name = P->getName();
    os_ << "%" << ctx.toString(Name);
  } else if (auto P = dyn_cast<JSSpecialParam>(V)) {
    auto Name = P->getName();
    os_ << "%" << ctx.toString(Name);
  } else if (auto F = dyn_cast<Function>(V)) {
    os_ << "%" << quoteStr(ctx.toString(F->getInternalName())) << "()";
  } else if (auto VS = dyn_cast<VariableScope>(V)) {
    os_ << "%VS" << namer_.getScopeNumber(VS);
  } else if (auto VR = dyn_cast<Variable>(V)) {
    os_ << "[%VS" << namer_.getScopeNumber(VR->getParent()) << "."
        << namer_.getVarName(VR) << "]";
  } else if (auto *lt = llvh::dyn_cast<LiteralIRType>(V)) {
    os_ << "type(" << lt->getData() << ")";
  } else if (auto *lt = llvh::dyn_cast<LiteralTypeOfIsTypes>(V)) {
    os_ << "typeOfIs(" << lt->getData() << ")";
  } else if (auto *NS = llvh::dyn_cast<LiteralNativeSignature>(V)) {
    os_ << '"';
    NS->getData()->format(os_);
    os_ << '"';
  } else if (auto *NE = llvh::dyn_cast<LiteralNativeExtern>(V)) {
    NativeExtern *ne = NE->getData();
    os_ << "extern_c(";
    ne->signature()->format(os_, ne->name()->c_str());
    if (ne->declared())
      os_ << " /*declared*/";
    os_ << ")";
  } else {
    llvm_unreachable("Invalid value");
  }

  printTypeLabel(V);
}

void IRPrinter::printFunctionHeader(Function *F) {
  auto &Ctx = F->getContext();
  std::string defKindStr = F->getDefinitionKindStr(false);

  os_ << defKindStr << " " << quoteStr(Ctx.toString(F->getInternalName()))
      << "(";
  uint32_t idx = 0u - 1;
  for (auto *P : F->getJSDynamicParams()) {
    ++idx;
    // Skip "this".
    if (idx == 0)
      continue;
    // Comma before the second param
    if (idx > 1) {
      os_ << ", ";
    }
    os_ << P->getName().str();
    printTypeLabel(P);
  }
  os_ << ")";
  setColor(Color::Type);
  os_ << ": " << F->getReturnType();
  resetColor();
  os_ << " " << F->getAttributes(F->getParent()).getDescriptionStr();
}

void IRPrinter::visitVariableScope(const hermes::VariableScope &VS) {
  bool first = true;
  os_.indent(indent_);
  os_ << "scope %VS" << namer_.getScopeNumber(&VS) << " [";
  for (auto *V : VS.getVariables()) {
    if (!first) {
      os_ << ", ";
    }
    os_ << namer_.getVarName(V);
    printTypeLabel(V);
    first = false;
  }
  os_ << "]\n\n";
}

bool IRPrinter::printInstructionDestination(Instruction *I) {
  unsigned number = namer_.getInstNumber(I);
  if (labelAllInsts_ || I->hasOutput()) {
    setColor(Color::Name);
    os_ << "%" << number;
    resetColor();
    return true;
  } else {
    // Calculate the width of the printed number.
    unsigned width = 1;
    while (number > 9) {
      number /= 10;
      ++width;
    }
    os_ << ' ';
    os_.indent(width);
    return false;
  }
}

void IRPrinter::printInstruction(Instruction *I) {
  if (printInstructionDestination(I))
    os_ << " = ";
  else
    os_ << "   ";
  setColor(Color::Inst);
  os_ << I->getName();
  resetColor();

  if (!I->getAttributes(I->getModule()).isEmpty()) {
    os_ << " " << I->getAttributes(I->getModule()).getDescriptionStr();
  }

  // Don't print the type of instructions without output.
  if (I->hasOutput()) {
    os_ << " (";
    setColor(Color::Type);
    os_ << ':' << I->getType();
    resetColor();
    os_ << ")";
  }

  bool first = true;

  for (int i = 0, e = I->getNumOperands(); i < e; i++) {
    os_ << (first ? " " : ", ");
    printValueLabel(I, I->getOperand(i), i);
    first = false;
  }

  const auto &codeGenOpts = I->getContext().getCodeGenerationSettings();
  // Print the use list if there is any user for the instruction.
  if (!codeGenOpts.dumpUseList || I->getUsers().empty())
    return;

  llvh::DenseSet<Instruction *> Visited;
  os_ << " // users:";
  for (auto &U : I->getUsers()) {
    auto *II = cast<Instruction>(U);
    assert(II && "Expecting user to be an Instruction");
    if (Visited.find(II) != Visited.end())
      continue;
    Visited.insert(II);
    os_ << " %" << namer_.getInstNumber(II);
  }
}

void IRPrinter::printSourceLocation(SMLoc loc) {
  SourceErrorManager::SourceCoords coords;
  if (!sm_.findBufferLineAndLoc(loc, coords))
    return;

  os_ << sm_.getSourceUrl(coords.bufId) << ":" << coords.line << ":"
      << coords.col;
}

void IRPrinter::printSourceLocation(SMRange rng) {
  SourceErrorManager::SourceCoords start, end;
  if (!sm_.findBufferLineAndLoc(rng.Start, start) ||
      !sm_.findBufferLineAndLoc(rng.End, end))
    return;

  os_ << "[" << sm_.getSourceUrl(start.bufId) << ":" << start.line << ":"
      << start.col << " ... " << sm_.getSourceUrl(end.bufId) << ":" << end.line
      << ":" << end.col << ")";
}

void IRPrinter::visitModule(const Module &M) {
  // Use IRVisitor dispatch to visit each individual function.
  for (auto &F : M)
    visit(F);
}

void IRPrinter::visitFunction(const Function &F) {
  auto *UF = const_cast<Function *>(&F);
  llvh::SmallVector<BasicBlock *, 8> order{};
  for (auto &BB : *UF)
    order.push_back(&BB);
  visitFunction(F, order);
}

void IRPrinter::visitFunction(
    const Function &F,
    llvh::ArrayRef<BasicBlock *> order) {
  if (!ctx_.getCodeGenerationSettings().dumpFunctions.empty()) {
    if (!ctx_.getCodeGenerationSettings().dumpFunctions.count(
            F.getInternalNameStr()))
      return;
  }
  if (ctx_.getCodeGenerationSettings().noDumpFunctions.count(
          F.getInternalNameStr())) {
    return;
  }

  auto *UF = const_cast<Function *>(&F);
  namer_.newFunction(&F);
  for (auto *BB : order) {
    // Number all basic blocks sequentially.
    namer_.getBBNumber(BB);
    // Number all instructions sequentially.
    for (auto &I : *BB) {
      namer_.getInstNumber(&I);

      // Dump the scopes that are being encountered for the first time.
      for (size_t i = 0; i < I.getNumOperands(); i++)
        if (auto *VS = dyn_cast<VariableScope>(I.getOperand(i)))
          if (dumpedScopes_.insert(VS).second)
            visit(*VS);
    }
  }

  os_.indent(indent_);
  printFunctionHeader(UF);
  os_ << "\n";

  const auto &codeGenOpts = F.getContext().getCodeGenerationSettings();
  if (codeGenOpts.dumpSourceLocation) {
    os_ << "source location: ";
    printSourceLocation(F.getSourceRange());
    os_ << "\n";
  }

  // Use IRVisitor dispatch to visit the basic blocks.
  for (auto *BB : order) {
    visit(*BB);
  }

  os_.indent(indent_);
  os_ << "function_end" << "\n";
  os_ << "\n";
}

void IRPrinter::visitBasicBlock(const BasicBlock &BB) {
  auto *UBB = const_cast<BasicBlock *>(&BB);
  os_.indent(indent_);
  setColor(Color::Name);
  os_ << "%BB" << namer_.getBBNumber(UBB) << ":\n";
  resetColor();
  indent_ += 2;

  // Use IRVisitor dispatch to visit the instructions.
  for (auto &I : BB)
    visit(I);

  indent_ -= 2;
}

void IRPrinter::visitInstruction(const Instruction &I) {
  auto *UII = const_cast<Instruction *>(&I);
  const auto &codeGenOpts = I.getContext().getCodeGenerationSettings();
  if (codeGenOpts.dumpSourceLocation) {
    os_ << "; ";
    printSourceLocation(UII->getLocation());
    os_ << "\n";
  }
  os_.indent(indent_);
  printInstruction(UII);
  os_ << "\n";
}

/// A simple palette for IR dumping, superficially inspired by Godbolt's LLVM IR
/// palette.
static struct {
  llvh::raw_ostream::Colors color;
  // -1 means select default color, false means not bold, true means bold.
  int8_t bold;
} s_palette[] = {
    // None
    {llvh::raw_ostream::Colors::BLACK, -1},
    // Inst
    {llvh::raw_ostream::Colors::BLACK, -1},
    // Type like :number.
    {llvh::raw_ostream::Colors::GREEN, false},
    // Name like %15 or %BB3
    /* BLUE looks better, but sometimes fades in dark mode. */
    /* {raw_ostream::Colors::BLUE, false}, */
    {llvh::raw_ostream::Colors::RED, false},
    // Register like {loc1}
    {llvh::raw_ostream::Colors::MAGENTA, false},
};
static_assert(
    sizeof(s_palette) / sizeof(s_palette[0]) == (size_t)IRPrinter::Color::_last,
    "palette size mismatch");

void IRPrinter::setColor(IRPrinter::Color color) {
  assert(color < Color::_last && "invalid color");
  unsigned ind = (unsigned)color;
  if (s_palette[ind].bold < 0)
    resetColor();
  else
    _changeColor(s_palette[ind].color, s_palette[ind].bold);
}

void IRPrinter::resetColor() {
  if (colors_)
    os_.resetColor();
}

void IRPrinter::_changeColor(
    llvh::raw_ostream::Colors Color,
    bool Bold,
    bool BG) {
  if (colors_)
    os_.changeColor(Color, Bold, BG);
}

namespace {

/// This class prints Functions into dotty graphs. This struct inherits the
/// IRVisitor and reimplement the visitFunction and visitBasicBlock function.
struct DottyPrinter : public IRVisitor<DottyPrinter, void> {
  llvh::raw_ostream &os;
  llvh::SmallVector<std::pair<std::string, std::string>, 4> Edges;
  IRPrinter Printer;

  explicit DottyPrinter(
      Context &ctx,
      llvh::raw_ostream &ost,
      llvh::StringRef Title)
      : os(ost), Printer(ctx, false, ost, /* escape output */ true, false) {
    Printer.disableColors();
    os << " digraph g {\n graph [ rankdir = \"TD\" ];\n";
    os << "labelloc=\"t\"; ";
    os << " node [ fontsize = \"16\" shape = \"record\" ]; edge [ ];\n";
  }

  ~DottyPrinter() {
    os << "\n";

    // Print the edges between the blocks.
    for (auto T : Edges) {
      os << "" << T.first << " ->";
      os << "" << T.second << ";\n";
    }
    os << "\n}\n";
  }

  /// Convert pointers into unique textual ids.
  static std::string toString(BasicBlock *ptr) {
    auto Num = (size_t)ptr;
    return std::to_string(Num);
  }

  /// Reimplement the visitFunction in IRVisitor.
  void visitFunction(const Function &F) {
    os << "label=\"";
    Printer.printFunctionHeader(const_cast<Function *>(&F));
    os << "\";\n";

    // Pre-assign every instruction a number, by doing this we avoid
    // assigning non-consecutive numbers to consecutive instructions if we
    // are dumping user list. Its also not a bad practice to initialize the
    // InstNamer table before we dump all the instructions.
    for (auto &BB : F) {
      for (auto &II : BB) {
        (void)Printer.namer_.getInstNumber(const_cast<Instruction *>(&II));
      }
    }

    // Use IRVisitor dispatch to visit the basic blocks.
    for (auto &BB : F) {
      visit(BB);
    }
  }

  /// Reimplement the visitBasicBlock in the IRVisitor.
  /// Visit all of the basic blocks in the program and render them into dotty
  /// records. Save edges between basic blocks and print them when we finish
  /// visiting all of the blocks in the program.
  void visitBasicBlock(const BasicBlock &V) {
    auto *BB = const_cast<BasicBlock *>(&V);
    os << "\"" << toString(BB) << "\"";
    os << "[ label = \"{ ";

    os << " { <self> | <head> \\<\\<%BB" << Printer.namer_.getBBNumber(BB)
       << "\\>\\> | } ";

    int counter = 0;

    // For each instruction in the basic block:
    for (auto &I : *BB) {
      counter++;
      os << " | ";

      // Print the instruction.
      os << "<L" << counter << ">";
      Printer.printInstruction(&I);
    }

    for (auto I = succ_begin(BB), E = succ_end(BB); I != E; ++I) {
      auto From = toString(BB) + ":L" + std::to_string(counter);
      Edges.push_back({From, toString(*I) + (*I == BB ? ":self" : ":head")});
    }

    // Emit the record:
    os << "}\" shape = \"record\" ]; \n";
  }
};

} // anonymous namespace

#endif // !defined(NDEBUG) || !defined(HERMES_IS_MOBILE_BUILD)

void viewGraph(Function *F) {
#ifndef NDEBUG
  auto &Ctx = F->getContext();
  llvh::StringRef Name = Ctx.toString(F->getInternalName());
  int FD;
  // Windows can't always handle long paths, so limit the length of the name.
  std::string N = Name.str();
  N = N.substr(0, std::min<std::size_t>(N.size(), 140));
  std::string Filename = llvh::createGraphFilename(N, FD);
  {
    llvh::raw_fd_ostream O(FD, /*shouldClose=*/true);

    if (FD == -1) {
      llvh::errs() << "error opening file '" << Filename << "' for writing!\n";
      return;
    }

    DottyPrinter D(F->getContext(), O, Name);
    D.visitFunction(*F);
  }

  llvh::DisplayGraph(Filename);
  llvh::errs() << " done. \n";
#endif
}

} // namespace hermes::irdumper
