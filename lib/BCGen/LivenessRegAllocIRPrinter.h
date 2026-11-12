/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_BCGEN_LIVENESSREGALLOCIRPRINTER_H
#define HERMES_BCGEN_LIVENESSREGALLOCIRPRINTER_H

#include "hermes/BCGen/RegAlloc.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Utils/Dumper.h"

#include "llvh/Support/FormatAdapters.h"
#include "llvh/Support/FormatVariadic.h"
#include "llvh/Support/raw_ostream.h"

namespace hermes {

#if !defined(NDEBUG) || !defined(HERMES_IS_MOBILE_BUILD)

template <class RegisterAllocator, typename RegClass>
struct LivenessRegAllocIRPrinter : irdumper::IRPrinter {
  RegisterAllocator &allocator;

  explicit LivenessRegAllocIRPrinter(
      RegisterAllocator &RA,
      llvh::raw_ostream &ost,
      bool escape = false)
      : IRPrinter(*RA.getModule(), ost, escape), allocator(RA) {}

  bool printInstructionDestination(Instruction *I) override {
    const auto &codeGenOpts = I->getContext().getCodeGenerationSettings();

    auto optReg = allocator.getOptionalRegister(I);

    if (optReg && optReg->getClass() != RegClass::NoOutput) {
      setColor(Color::Register);
      os_ << llvh::formatv("{0,-8} ", llvh::formatv("{{{0}}", *optReg));
      resetColor();
    } else {
      os_ << llvh::formatv("{0,-8} ", "");
    }

    if (codeGenOpts.dumpRegisterInterval) {
      bool hasInstNumber = allocator.hasInstructionNumber(I);
      auto idx = hasInstNumber ? allocator.getInstructionNumber(I) : 0;

      if (optReg && optReg->getClass() != RegClass::NoOutput && hasInstNumber) {
        setColor(Color::Name);
        os_ << llvh::formatv("{0,+3}", llvh::formatv("%{0}", idx));
        os_ << ' '
            << llvh::formatv("{0,-10}", allocator.getInstructionInterval(I));
        resetColor();
        return true;
      } else if (hasInstNumber) {
        setColor(Color::Name);
        os_ << llvh::formatv("{0,+3}", llvh::formatv("%{0}", idx));
        resetColor();
        os_ << ' ' << llvh::formatv("{0,-10}", "");
        return true;
      } else {
        os_ << llvh::formatv("{0,+3}", "");
        os_ << ' ' << llvh::formatv("{0,-10}", "");
        return false;
      }
    }

    if (optReg && optReg->getClass() != RegClass::NoOutput) {
      setColor(Color::Name);
      os_ << llvh::formatv(
          "{0,3}", llvh::formatv("%{0}", namer_.getInstNumber(I)));
      resetColor();
      return true;
    } else {
      os_ << llvh::formatv("{0,3}", "");
      return false;
    }
  }

  void printValueLabel(Instruction *I, Value *V, unsigned opIndex) override {
    const auto &codeGenOpts = I->getContext().getCodeGenerationSettings();
    if (codeGenOpts.dumpRegisterInterval) {
      if (auto *opInst = llvh::dyn_cast<Instruction>(V)) {
        setColor(Color::Name);
        if (allocator.hasInstructionNumber(opInst))
          os_ << '%' << allocator.getInstructionNumber(opInst);
        else
          os_ << "%dead";
        resetColor();
        printTypeLabel(opInst);
        return;
      }
    }
    if (allocator.isAllocated(V)) {
      setColor(Color::Register);
      os_ << '{' << allocator.getRegister(V) << "} ";
      resetColor();
    }
    IRPrinter::printValueLabel(I, V, opIndex);
  }
};

#else

template <class RegisterAllocator, typename RegClass>
struct LivenessRegAllocIRPrinter : irdumper::IRPrinter {
  explicit LivenessRegAllocIRPrinter(
      RegisterAllocator &RA,
      llvh::raw_ostream &ost,
      bool escape = false)
      : IRPrinter(*RA.getModule(), ost, escape) {}
};

#endif // !defined(NDEBUG) || !defined(HERMES_IS_MOBILE_BUILD)

} // namespace hermes

#endif
