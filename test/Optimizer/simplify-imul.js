/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -hermes-parser -dump-ir -O -fstatic-builtins %s | %FileCheckOrRegen --match-full-lines %s
// RUN: %hermesc -hermes-parser -dump-ir -O -fstatic-builtins %s | %FileCheck --check-prefix=FOLDED %s

// InstSimplify folds ImulInst when it can evaluate ToInt32 on both operands,
// which is what a Math.imul call on constants becomes. The values here are
// the ones that distinguish a 32-bit wrapping multiply from a double
// multiply, and test/hermes/math-imul-instruction.js runs the same values
// three ways: through this folder at -O, through the instruction at -O0, and
// through the untouched builtin under -fno-static-builtins. Agreement among
// the three is what says the folder computes Math.imul rather than something
// that merely resembles it.
//
// The padded arities fold as well, because the padding is a literal 0: a
// call with fewer than two arguments leaves no operand the folder cannot
// see.

function ordinary() {
  return Math.imul(3, 4);
}

// The 2^31 boundaries, where truncating to int32 is the whole difference
// between Math.imul and a double multiply.
function wrap() {
  return Math.imul(0x7fffffff, 2);
}

function minInt32() {
  return Math.imul(-0x80000000, -1);
}

function bothHighBits() {
  return Math.imul(0x80000000, 0x80000000);
}

// The exact product here exceeds 2^53, so no double multiply can produce
// this answer; only a real 32-bit multiply can.
function beyondDoublePrecision() {
  return Math.imul(123456789, 987654321);
}

// ToInt32 truncates toward zero, then keeps only the low 32 bits.
function fraction() {
  return Math.imul(2.9, 3);
}

function outOfRange() {
  return Math.imul(1e21, 7);
}

// ToInt32 of NaN and of the infinities is +0.
function notANumber() {
  return Math.imul(NaN, 5);
}

function infinite() {
  return Math.imul(Infinity, 5);
}

// Literals that are not numbers still convert without running anything:
// booleans, null and undefined all have a ToInt32 the folder can evaluate.
function booleans() {
  return Math.imul(true, 7);
}

function nullOperand() {
  return Math.imul(null, 5);
}

function undefinedOperand() {
  return Math.imul(undefined, 5);
}

// A missing argument is padded with the literal 0, so these fold too.
function noArgs() {
  return Math.imul();
}

function oneArg() {
  return Math.imul(7);
}

// Below here the folder must refuse, and the instruction must survive.

// A BigInt operand must not fold. Math.imul is not a bitwise operator: it
// coerces with ToInt32, which throws a TypeError on a BigInt instead of
// computing in BigInt, so the instruction has to survive in order to throw
// at run time. Folding this to a number would quietly turn a throwing
// program into one that returns a value. evalToNumber refusing BigInt is
// what makes that structural rather than a case the folder remembers to
// check.
function bigintThrows() {
  return Math.imul(1n, 2n);
}

// A string operand could in principle fold -- ToNumber of a string literal
// runs no user code -- but evalToNumber does not convert strings, so it does
// not. Pinned because it is a conservative miss and not a correctness
// requirement: if evalToNumber ever learns strings, this starts folding and
// the expectation below is what says so.
function strings() {
  return Math.imul("5", "3");
}

// Operands that are not literals at all plainly cannot fold.
function unknown(a, b) {
  return Math.imul(a, b);
}

print(ordinary, wrap, minInt32, bothHighBits, beyondDoublePrecision, fraction,
      outOfRange, notANumber, infinite, booleans, nullOperand,
      undefinedOperand, noArgs, oneArg, bigintThrows, strings, unknown);

// Exactly three instructions survive, and they are the three at the bottom.
// The leading directive is the one that matters: it fails the moment any
// call above stops folding.
// FOLDED-NOT: ImulInst
// FOLDED: function bigintThrows
// FOLDED: ImulInst
// FOLDED: function strings
// FOLDED: ImulInst
// FOLDED: function unknown
// FOLDED: ImulInst
// FOLDED-NOT: ImulInst

// Auto-generated content below. Please do not modify manually.

// CHECK:function global(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       DeclareGlobalVarInst "ordinary": string
// CHECK-NEXT:       DeclareGlobalVarInst "wrap": string
// CHECK-NEXT:       DeclareGlobalVarInst "minInt32": string
// CHECK-NEXT:       DeclareGlobalVarInst "bothHighBits": string
// CHECK-NEXT:       DeclareGlobalVarInst "beyondDoublePrecision": string
// CHECK-NEXT:       DeclareGlobalVarInst "fraction": string
// CHECK-NEXT:       DeclareGlobalVarInst "outOfRange": string
// CHECK-NEXT:       DeclareGlobalVarInst "notANumber": string
// CHECK-NEXT:       DeclareGlobalVarInst "infinite": string
// CHECK-NEXT:       DeclareGlobalVarInst "booleans": string
// CHECK-NEXT:        DeclareGlobalVarInst "nullOperand": string
// CHECK-NEXT:        DeclareGlobalVarInst "undefinedOperand": string
// CHECK-NEXT:        DeclareGlobalVarInst "noArgs": string
// CHECK-NEXT:        DeclareGlobalVarInst "oneArg": string
// CHECK-NEXT:        DeclareGlobalVarInst "bigintThrows": string
// CHECK-NEXT:        DeclareGlobalVarInst "strings": string
// CHECK-NEXT:        DeclareGlobalVarInst "unknown": string
// CHECK-NEXT:  %17 = CreateFunctionInst (:object) empty: any, empty: any, %ordinary(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %17: object, globalObject: object, "ordinary": string
// CHECK-NEXT:  %19 = CreateFunctionInst (:object) empty: any, empty: any, %wrap(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %19: object, globalObject: object, "wrap": string
// CHECK-NEXT:  %21 = CreateFunctionInst (:object) empty: any, empty: any, %minInt32(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %21: object, globalObject: object, "minInt32": string
// CHECK-NEXT:  %23 = CreateFunctionInst (:object) empty: any, empty: any, %bothHighBits(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %23: object, globalObject: object, "bothHighBits": string
// CHECK-NEXT:  %25 = CreateFunctionInst (:object) empty: any, empty: any, %beyondDoublePrecision(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %25: object, globalObject: object, "beyondDoublePrecision": string
// CHECK-NEXT:  %27 = CreateFunctionInst (:object) empty: any, empty: any, %fraction(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %27: object, globalObject: object, "fraction": string
// CHECK-NEXT:  %29 = CreateFunctionInst (:object) empty: any, empty: any, %outOfRange(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %29: object, globalObject: object, "outOfRange": string
// CHECK-NEXT:  %31 = CreateFunctionInst (:object) empty: any, empty: any, %notANumber(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %31: object, globalObject: object, "notANumber": string
// CHECK-NEXT:  %33 = CreateFunctionInst (:object) empty: any, empty: any, %infinite(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %33: object, globalObject: object, "infinite": string
// CHECK-NEXT:  %35 = CreateFunctionInst (:object) empty: any, empty: any, %booleans(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %35: object, globalObject: object, "booleans": string
// CHECK-NEXT:  %37 = CreateFunctionInst (:object) empty: any, empty: any, %nullOperand(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %37: object, globalObject: object, "nullOperand": string
// CHECK-NEXT:  %39 = CreateFunctionInst (:object) empty: any, empty: any, %undefinedOperand(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %39: object, globalObject: object, "undefinedOperand": string
// CHECK-NEXT:  %41 = CreateFunctionInst (:object) empty: any, empty: any, %noArgs(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %41: object, globalObject: object, "noArgs": string
// CHECK-NEXT:  %43 = CreateFunctionInst (:object) empty: any, empty: any, %oneArg(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %43: object, globalObject: object, "oneArg": string
// CHECK-NEXT:  %45 = CreateFunctionInst (:object) empty: any, empty: any, %bigintThrows(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %45: object, globalObject: object, "bigintThrows": string
// CHECK-NEXT:  %47 = CreateFunctionInst (:object) empty: any, empty: any, %strings(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %47: object, globalObject: object, "strings": string
// CHECK-NEXT:  %49 = CreateFunctionInst (:object) empty: any, empty: any, %unknown(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %49: object, globalObject: object, "unknown": string
// CHECK-NEXT:  %51 = TryLoadGlobalPropertyInst (:any) globalObject: object, "print": string
// CHECK-NEXT:  %52 = LoadPropertyInst (:any) globalObject: object, "ordinary": string
// CHECK-NEXT:  %53 = LoadPropertyInst (:any) globalObject: object, "wrap": string
// CHECK-NEXT:  %54 = LoadPropertyInst (:any) globalObject: object, "minInt32": string
// CHECK-NEXT:  %55 = LoadPropertyInst (:any) globalObject: object, "bothHighBits": string
// CHECK-NEXT:  %56 = LoadPropertyInst (:any) globalObject: object, "beyondDoublePrecision": string
// CHECK-NEXT:  %57 = LoadPropertyInst (:any) globalObject: object, "fraction": string
// CHECK-NEXT:  %58 = LoadPropertyInst (:any) globalObject: object, "outOfRange": string
// CHECK-NEXT:  %59 = LoadPropertyInst (:any) globalObject: object, "notANumber": string
// CHECK-NEXT:  %60 = LoadPropertyInst (:any) globalObject: object, "infinite": string
// CHECK-NEXT:  %61 = LoadPropertyInst (:any) globalObject: object, "booleans": string
// CHECK-NEXT:  %62 = LoadPropertyInst (:any) globalObject: object, "nullOperand": string
// CHECK-NEXT:  %63 = LoadPropertyInst (:any) globalObject: object, "undefinedOperand": string
// CHECK-NEXT:  %64 = LoadPropertyInst (:any) globalObject: object, "noArgs": string
// CHECK-NEXT:  %65 = LoadPropertyInst (:any) globalObject: object, "oneArg": string
// CHECK-NEXT:  %66 = LoadPropertyInst (:any) globalObject: object, "bigintThrows": string
// CHECK-NEXT:  %67 = LoadPropertyInst (:any) globalObject: object, "strings": string
// CHECK-NEXT:  %68 = LoadPropertyInst (:any) globalObject: object, "unknown": string
// CHECK-NEXT:  %69 = CallInst (:any) %51: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %52: any, %53: any, %54: any, %55: any, %56: any, %57: any, %58: any, %59: any, %60: any, %61: any, %62: any, %63: any, %64: any, %65: any, %66: any, %67: any, %68: any
// CHECK-NEXT:        ReturnInst %69: any
// CHECK-NEXT:function_end

// CHECK:function ordinary(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 12: number
// CHECK-NEXT:function_end

// CHECK:function wrap(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst -2: number
// CHECK-NEXT:function_end

// CHECK:function minInt32(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst -2147483648: number
// CHECK-NEXT:function_end

// CHECK:function bothHighBits(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 0: number
// CHECK-NEXT:function_end

// CHECK:function beyondDoublePrecision(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst -67153019: number
// CHECK-NEXT:function_end

// CHECK:function fraction(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 6: number
// CHECK-NEXT:function_end

// CHECK:function outOfRange(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 375390208: number
// CHECK-NEXT:function_end

// CHECK:function notANumber(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 0: number
// CHECK-NEXT:function_end

// CHECK:function infinite(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 0: number
// CHECK-NEXT:function_end

// CHECK:function booleans(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 7: number
// CHECK-NEXT:function_end

// CHECK:function nullOperand(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 0: number
// CHECK-NEXT:function_end

// CHECK:function undefinedOperand(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 0: number
// CHECK-NEXT:function_end

// CHECK:function noArgs(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 0: number
// CHECK-NEXT:function_end

// CHECK:function oneArg(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 0: number
// CHECK-NEXT:function_end

// CHECK:function bigintThrows(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = ImulInst (:number) 1: bigint, 2: bigint
// CHECK-NEXT:       ReturnInst %0: number
// CHECK-NEXT:function_end

// CHECK:function strings(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = ImulInst (:number) "5": string, "3": string
// CHECK-NEXT:       ReturnInst %0: number
// CHECK-NEXT:function_end

// CHECK:function unknown(a: any, b: any): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %a: any
// CHECK-NEXT:  %1 = LoadParamInst (:any) %b: any
// CHECK-NEXT:  %2 = ImulInst (:number) %0: any, %1: any
// CHECK-NEXT:       ReturnInst %2: number
// CHECK-NEXT:function_end
