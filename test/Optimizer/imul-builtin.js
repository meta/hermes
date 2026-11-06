/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -O -fstatic-builtins -dump-ir %s | %FileCheckOrRegen --match-full-lines %s
// RUN: %hermesc -O -fstatic-builtins -dump-ir %s | %FileCheck --check-prefix=NOCB %s
// RUN: %hermesc -O -fno-static-builtins -dump-ir %s | %FileCheck --check-prefix=NOSB %s

// LowerBuiltinCalls rewrites a proven Math.imul call into a CallBuiltinInst
// like any other builtin, and then peepholes that CallBuiltinInst into
// ImulInst. The peephole runs on every CallBuiltinInst the pass walks over,
// not only on the one it just created, so a Math.imul builtin call reaching
// the pass from any other producer becomes the instruction too.
//
// Math.imul reads exactly two arguments. A missing one is undefined and
// ToInt32(undefined) is 0, so the peephole pads with the literal 0 instead:
// same value, but a literal number leaves the padded operand statically
// numeric, which an undefined never would. Arguments past the second are
// dropped, having already been evaluated by their own instructions.
//
// The receiver of an ordinary Math.imul(a, b) call is the Math object, not
// undefined; the lowering discards it exactly as the CallBuiltin rewrite
// discards it, so there is no this-value precondition to check.
//
// No arity survives as a builtin call. This is the check that makes the
// peephole's coverage -- and not merely its result on two arguments -- a
// property of the output, so padding or dropping an argument cannot be
// quietly replaced by giving up and leaving the call alone.
// NOCB-NOT: [Math.imul]
//
// The lowering rides the existing static-builtins proof and adds no
// condition of its own, so the last RUN line is what pins that: with
// -fno-static-builtins spelled explicitly, no call here resolves to a
// builtin at all and no ImulInst is produced.
// NOSB-NOT: ImulInst
// NOSB: CallInst
// NOSB-NOT: ImulInst

function twoArgs(a, b) {
  return Math.imul(a, b);
}

function oneArg(a) {
  return Math.imul(a);
}

// Both operands end up literal 0, so InstSimplify folds this one away
// entirely and no instruction is left to see. The padding itself is visible
// in oneArg above, whose supplied operand is a parameter.
function noArgs() {
  return Math.imul();
}

function threeArgs(a, b, c) {
  return Math.imul(a, b, c);
}

// Dropping the extra arguments drops operands, not evaluation. The call
// computing the third argument stays exactly where it was; only the
// reference to its result goes away.
function extraArgEffect(a, b) {
  return Math.imul(a, b, sideEffect());
}

// The result is discarded, but the operands are not statically numbers, so
// either of them may run a valueOf or throw. The instruction has to survive
// DCE.
function unusedResult(a, b) {
  Math.imul(a, b);
  return 1;
}

// A shadowed Math is not the builtin object, so the proof fails and the
// call stays an ordinary CallInst.
function shadowed(a, b) {
  var Math = {imul: function (x, y) { return x; }};
  return Math.imul(a, b);
}

print(twoArgs, oneArg, noArgs, threeArgs, extraArgEffect, unusedResult,
      shadowed);

// Auto-generated content below. Please do not modify manually.

// CHECK:function global(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       DeclareGlobalVarInst "twoArgs": string
// CHECK-NEXT:       DeclareGlobalVarInst "oneArg": string
// CHECK-NEXT:       DeclareGlobalVarInst "noArgs": string
// CHECK-NEXT:       DeclareGlobalVarInst "threeArgs": string
// CHECK-NEXT:       DeclareGlobalVarInst "extraArgEffect": string
// CHECK-NEXT:       DeclareGlobalVarInst "unusedResult": string
// CHECK-NEXT:       DeclareGlobalVarInst "shadowed": string
// CHECK-NEXT:  %7 = CreateFunctionInst (:object) empty: any, empty: any, %twoArgs(): functionCode
// CHECK-NEXT:       StorePropertyLooseInst %7: object, globalObject: object, "twoArgs": string
// CHECK-NEXT:  %9 = CreateFunctionInst (:object) empty: any, empty: any, %oneArg(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %9: object, globalObject: object, "oneArg": string
// CHECK-NEXT:  %11 = CreateFunctionInst (:object) empty: any, empty: any, %noArgs(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %11: object, globalObject: object, "noArgs": string
// CHECK-NEXT:  %13 = CreateFunctionInst (:object) empty: any, empty: any, %threeArgs(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %13: object, globalObject: object, "threeArgs": string
// CHECK-NEXT:  %15 = CreateFunctionInst (:object) empty: any, empty: any, %extraArgEffect(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %15: object, globalObject: object, "extraArgEffect": string
// CHECK-NEXT:  %17 = CreateFunctionInst (:object) empty: any, empty: any, %unusedResult(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %17: object, globalObject: object, "unusedResult": string
// CHECK-NEXT:  %19 = CreateFunctionInst (:object) empty: any, empty: any, %shadowed(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %19: object, globalObject: object, "shadowed": string
// CHECK-NEXT:  %21 = TryLoadGlobalPropertyInst (:any) globalObject: object, "print": string
// CHECK-NEXT:  %22 = LoadPropertyInst (:any) globalObject: object, "twoArgs": string
// CHECK-NEXT:  %23 = LoadPropertyInst (:any) globalObject: object, "oneArg": string
// CHECK-NEXT:  %24 = LoadPropertyInst (:any) globalObject: object, "noArgs": string
// CHECK-NEXT:  %25 = LoadPropertyInst (:any) globalObject: object, "threeArgs": string
// CHECK-NEXT:  %26 = LoadPropertyInst (:any) globalObject: object, "extraArgEffect": string
// CHECK-NEXT:  %27 = LoadPropertyInst (:any) globalObject: object, "unusedResult": string
// CHECK-NEXT:  %28 = LoadPropertyInst (:any) globalObject: object, "shadowed": string
// CHECK-NEXT:  %29 = CallInst (:any) %21: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %22: any, %23: any, %24: any, %25: any, %26: any, %27: any, %28: any
// CHECK-NEXT:        ReturnInst %29: any
// CHECK-NEXT:function_end

// CHECK:function twoArgs(a: any, b: any): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %a: any
// CHECK-NEXT:  %1 = LoadParamInst (:any) %b: any
// CHECK-NEXT:  %2 = ImulInst (:number) %0: any, %1: any
// CHECK-NEXT:       ReturnInst %2: number
// CHECK-NEXT:function_end

// CHECK:function oneArg(a: any): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %a: any
// CHECK-NEXT:  %1 = ImulInst (:number) %0: any, 0: number
// CHECK-NEXT:       ReturnInst %1: number
// CHECK-NEXT:function_end

// CHECK:function noArgs(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst 0: number
// CHECK-NEXT:function_end

// CHECK:function threeArgs(a: any, b: any, c: any): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %a: any
// CHECK-NEXT:  %1 = LoadParamInst (:any) %b: any
// CHECK-NEXT:  %2 = ImulInst (:number) %0: any, %1: any
// CHECK-NEXT:       ReturnInst %2: number
// CHECK-NEXT:function_end

// CHECK:function extraArgEffect(a: any, b: any): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %a: any
// CHECK-NEXT:  %1 = LoadParamInst (:any) %b: any
// CHECK-NEXT:  %2 = TryLoadGlobalPropertyInst (:any) globalObject: object, "sideEffect": string
// CHECK-NEXT:  %3 = CallInst (:any) %2: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined
// CHECK-NEXT:  %4 = ImulInst (:number) %0: any, %1: any
// CHECK-NEXT:       ReturnInst %4: number
// CHECK-NEXT:function_end

// CHECK:function unusedResult(a: any, b: any): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %a: any
// CHECK-NEXT:  %1 = LoadParamInst (:any) %b: any
// CHECK-NEXT:  %2 = ImulInst (:number) %0: any, %1: any
// CHECK-NEXT:       ReturnInst 1: number
// CHECK-NEXT:function_end

// CHECK:function shadowed(a: any, b: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %a: any
// CHECK-NEXT:  %1 = LoadParamInst (:any) %b: any
// CHECK-NEXT:  %2 = AllocObjectLiteralInst (:object) empty: any, "imul": string, null: null
// CHECK-NEXT:  %3 = CreateFunctionInst (:object) empty: any, empty: any, %imul(): functionCode
// CHECK-NEXT:       PrStoreInst %3: object, %2: object, 0: number, "imul": string, false: boolean
// CHECK-NEXT:  %5 = LoadPropertyInst (:any) %2: object, "imul": string
// CHECK-NEXT:  %6 = CallInst (:any) %5: any, empty: any, false: boolean, empty: any, undefined: undefined, %2: object, %0: any, %1: any
// CHECK-NEXT:       ReturnInst %6: any
// CHECK-NEXT:function_end

// CHECK:function imul(x: any, y: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %x: any
// CHECK-NEXT:       ReturnInst %0: any
// CHECK-NEXT:function_end
