/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O --target=HBC -dump-ra %s | %FileCheckOrRegen %s --check-prefix=CHKRA --match-full-lines
// Ensure that phi node does not update deadcode liveness interval
function b(d=([[[[{z:[{}]}]]]]=arguments)) {}

// Auto-generated content below. Please do not modify manually.

// CHKRA:function global(): undefined
// CHKRA-NEXT:%BB0:
// CHKRA-NEXT:                 DeclareGlobalVarInst "b": string
// CHKRA-NEXT:  {r1}      %1 = LIRGetGlobalObjectInst (:object)
// CHKRA-NEXT:  {np0}     %2 = LIRLoadConstInst (:undefined) undefined: undefined
// CHKRA-NEXT:  {r0}      %3 = CreateFunctionInst (:object) {np0} %2: undefined, empty: any, %b(): functionCode
// CHKRA-NEXT:                 StorePropertyLooseInst {r0} %3: object, {r1} %1: object, "b": string
// CHKRA-NEXT:                 ReturnInst {np0} %2: undefined
// CHKRA-NEXT:function_end

// CHKRA:function b(d: any): undefined
// CHKRA-NEXT:%BB0:
// CHKRA-NEXT:  {r3}      %0 = AllocStackInst (:undefined|object) $arguments: any
// CHKRA-NEXT:  {r0}      %1 = LIRLoadConstInst (:undefined) undefined: undefined
// CHKRA-NEXT:                 StoreStackInst {r0} %1: undefined, {r3} %0: undefined|object
// CHKRA-NEXT:  {r1}      %3 = LoadParamInst (:any) %d: any
// CHKRA-NEXT:                 CmpBrStrictlyNotEqualInst {r1} %3: any, {r0} %1: undefined, %BB2, %BB1
// CHKRA-NEXT:%BB1:
// CHKRA-NEXT:  {r2}      %5 = AllocStackInst (:any) $?anon_0_iter: any
// CHKRA-NEXT:  {r1}      %6 = AllocStackInst (:any) $?anon_1_sourceOrNext: any
// CHKRA-NEXT:                 LIRReifyArgumentsLooseInst {r3} %0: undefined|object
// CHKRA-NEXT:  {r3}      %8 = LoadStackInst (:undefined|object) {r3} %0: undefined|object
// CHKRA-NEXT:  {r3}      %9 = UnionNarrowTrustedInst (:object) {r3} %8: undefined|object
// CHKRA-NEXT:                 StoreStackInst {r3} %9: object, {r1} %6: any
// CHKRA-NEXT:  {r3}     %11 = IteratorBeginInst (:any) {r1} %6: any
// CHKRA-NEXT:                 StoreStackInst {r3} %11: any, {r2} %5: any
// CHKRA-NEXT:                 TryStartInst %BB4, %BB6
// CHKRA-NEXT:%BB2:
// CHKRA-NEXT:                 ReturnInst {r0} %1: undefined
// CHKRA-NEXT:%BB3:
// CHKRA-NEXT:  {r1}     %15 = PhiInst (:any) {r1} %21: any, %BB4, {r1} %28: any, %BB7
// CHKRA-NEXT:  {r2}     %16 = LoadStackInst (:any) {r2} %5: any
// CHKRA-NEXT:  {r2}     %17 = IteratorCloseInst (:any) {r2} %16: any, true: boolean
// CHKRA-NEXT:  {r1}     %18 = MovInst (:any) {r1} %15: any
// CHKRA-NEXT:                 ThrowInst {r1} %18: any
// CHKRA-NEXT:%BB4:
// CHKRA-NEXT:  {r3}     %20 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %21 = MovInst (:any) {r3} %20: any
// CHKRA-NEXT:                 BranchInst %BB3
// CHKRA-NEXT:%BB5:
// CHKRA-NEXT:  {r1}     %23 = LoadStackInst (:any) {r1} %6: any
// CHKRA-NEXT:  {r4}     %24 = IteratorNextInst (:any) {r2} %5: any, {r1} %23: any
// CHKRA-NEXT:                 TryStartInst %BB7, %BB9
// CHKRA-NEXT:%BB6:
// CHKRA-NEXT:                 TryEndInst %BB4, %BB5
// CHKRA-NEXT:%BB7:
// CHKRA-NEXT:  {r1}     %27 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %28 = MovInst (:any) {r1} %27: any
// CHKRA-NEXT:                 BranchInst %BB3
// CHKRA-NEXT:%BB8:
// CHKRA-NEXT:  {r1}     %30 = LoadStackInst (:any) {r2} %5: any
// CHKRA-NEXT:  {r1}     %31 = IteratorCloseInst (:any) {r1} %30: any, false: boolean
// CHKRA-NEXT:                 BranchInst %BB2
// CHKRA-NEXT:%BB9:
// CHKRA-NEXT:  {r3}     %33 = AllocStackInst (:any) $?anon_4_iter: any
// CHKRA-NEXT:  {r1}     %34 = AllocStackInst (:any) $?anon_5_sourceOrNext: any
// CHKRA-NEXT:                 StoreStackInst {r4} %24: any, {r1} %34: any
// CHKRA-NEXT:  {r4}     %36 = IteratorBeginInst (:any) {r1} %34: any
// CHKRA-NEXT:                 StoreStackInst {r4} %36: any, {r3} %33: any
// CHKRA-NEXT:                 TryStartInst %BB11, %BB13
// CHKRA-NEXT:%BB10:
// CHKRA-NEXT:  {r1}     %39 = PhiInst (:any) {r1} %45: any, %BB11, {r1} %52: any, %BB14
// CHKRA-NEXT:  {r3}     %40 = LoadStackInst (:any) {r3} %33: any
// CHKRA-NEXT:  {r3}     %41 = IteratorCloseInst (:any) {r3} %40: any, true: boolean
// CHKRA-NEXT:  {r1}     %42 = MovInst (:any) {r1} %39: any
// CHKRA-NEXT:                 ThrowInst {r1} %42: any, %BB7
// CHKRA-NEXT:%BB11:
// CHKRA-NEXT:  {r4}     %44 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %45 = MovInst (:any) {r4} %44: any
// CHKRA-NEXT:                 BranchInst %BB10
// CHKRA-NEXT:%BB12:
// CHKRA-NEXT:  {r1}     %47 = LoadStackInst (:any) {r1} %34: any
// CHKRA-NEXT:  {r5}     %48 = IteratorNextInst (:any) {r3} %33: any, {r1} %47: any
// CHKRA-NEXT:                 TryStartInst %BB14, %BB16
// CHKRA-NEXT:%BB13:
// CHKRA-NEXT:                 TryEndInst %BB11, %BB12
// CHKRA-NEXT:%BB14:
// CHKRA-NEXT:  {r1}     %51 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %52 = MovInst (:any) {r1} %51: any
// CHKRA-NEXT:                 BranchInst %BB10
// CHKRA-NEXT:%BB15:
// CHKRA-NEXT:  {r1}     %54 = LoadStackInst (:any) {r3} %33: any
// CHKRA-NEXT:  {r1}     %55 = IteratorCloseInst (:any) {r1} %54: any, false: boolean
// CHKRA-NEXT:                 TryEndInst %BB7, %BB8
// CHKRA-NEXT:%BB16:
// CHKRA-NEXT:  {r4}     %57 = AllocStackInst (:any) $?anon_8_iter: any
// CHKRA-NEXT:  {r1}     %58 = AllocStackInst (:any) $?anon_9_sourceOrNext: any
// CHKRA-NEXT:                 StoreStackInst {r5} %48: any, {r1} %58: any
// CHKRA-NEXT:  {r5}     %60 = IteratorBeginInst (:any) {r1} %58: any
// CHKRA-NEXT:                 StoreStackInst {r5} %60: any, {r4} %57: any
// CHKRA-NEXT:                 TryStartInst %BB18, %BB20
// CHKRA-NEXT:%BB17:
// CHKRA-NEXT:  {r1}     %63 = PhiInst (:any) {r1} %69: any, %BB18, {r1} %76: any, %BB21
// CHKRA-NEXT:  {r4}     %64 = LoadStackInst (:any) {r4} %57: any
// CHKRA-NEXT:  {r4}     %65 = IteratorCloseInst (:any) {r4} %64: any, true: boolean
// CHKRA-NEXT:  {r1}     %66 = MovInst (:any) {r1} %63: any
// CHKRA-NEXT:                 ThrowInst {r1} %66: any, %BB14
// CHKRA-NEXT:%BB18:
// CHKRA-NEXT:  {r5}     %68 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %69 = MovInst (:any) {r5} %68: any
// CHKRA-NEXT:                 BranchInst %BB17
// CHKRA-NEXT:%BB19:
// CHKRA-NEXT:  {r1}     %71 = LoadStackInst (:any) {r1} %58: any
// CHKRA-NEXT:  {r6}     %72 = IteratorNextInst (:any) {r4} %57: any, {r1} %71: any
// CHKRA-NEXT:                 TryStartInst %BB21, %BB23
// CHKRA-NEXT:%BB20:
// CHKRA-NEXT:                 TryEndInst %BB18, %BB19
// CHKRA-NEXT:%BB21:
// CHKRA-NEXT:  {r1}     %75 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %76 = MovInst (:any) {r1} %75: any
// CHKRA-NEXT:                 BranchInst %BB17
// CHKRA-NEXT:%BB22:
// CHKRA-NEXT:  {r1}     %78 = LoadStackInst (:any) {r4} %57: any
// CHKRA-NEXT:  {r1}     %79 = IteratorCloseInst (:any) {r1} %78: any, false: boolean
// CHKRA-NEXT:                 TryEndInst %BB14, %BB15
// CHKRA-NEXT:%BB23:
// CHKRA-NEXT:  {r5}     %81 = AllocStackInst (:any) $?anon_12_iter: any
// CHKRA-NEXT:  {r1}     %82 = AllocStackInst (:any) $?anon_13_sourceOrNext: any
// CHKRA-NEXT:                 StoreStackInst {r6} %72: any, {r1} %82: any
// CHKRA-NEXT:  {r6}     %84 = IteratorBeginInst (:any) {r1} %82: any
// CHKRA-NEXT:                 StoreStackInst {r6} %84: any, {r5} %81: any
// CHKRA-NEXT:                 TryStartInst %BB25, %BB27
// CHKRA-NEXT:%BB24:
// CHKRA-NEXT:  {r1}     %87 = PhiInst (:any) {r1} %93: any, %BB25, {r1} %100: any, %BB28
// CHKRA-NEXT:  {r5}     %88 = LoadStackInst (:any) {r5} %81: any
// CHKRA-NEXT:  {r5}     %89 = IteratorCloseInst (:any) {r5} %88: any, true: boolean
// CHKRA-NEXT:  {r1}     %90 = MovInst (:any) {r1} %87: any
// CHKRA-NEXT:                 ThrowInst {r1} %90: any, %BB21
// CHKRA-NEXT:%BB25:
// CHKRA-NEXT:  {r6}     %92 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %93 = MovInst (:any) {r6} %92: any
// CHKRA-NEXT:                 BranchInst %BB24
// CHKRA-NEXT:%BB26:
// CHKRA-NEXT:  {r1}     %95 = LoadStackInst (:any) {r1} %82: any
// CHKRA-NEXT:  {r1}     %96 = IteratorNextInst (:any) {r5} %81: any, {r1} %95: any
// CHKRA-NEXT:                 TryStartInst %BB28, %BB30
// CHKRA-NEXT:%BB27:
// CHKRA-NEXT:                 TryEndInst %BB25, %BB26
// CHKRA-NEXT:%BB28:
// CHKRA-NEXT:  {r1}     %99 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %100 = MovInst (:any) {r1} %99: any
// CHKRA-NEXT:                 BranchInst %BB24
// CHKRA-NEXT:%BB29:
// CHKRA-NEXT:  {r1}     %102 = LoadStackInst (:any) {r5} %81: any
// CHKRA-NEXT:  {r1}     %103 = IteratorCloseInst (:any) {r1} %102: any, false: boolean
// CHKRA-NEXT:                 TryEndInst %BB21, %BB22
// CHKRA-NEXT:%BB30:
// CHKRA-NEXT:  {r7}     %105 = LoadPropertyInst (:any) {r1} %96: any, "z": string
// CHKRA-NEXT:  {r6}     %106 = AllocStackInst (:any) $?anon_16_iter: any
// CHKRA-NEXT:  {r1}     %107 = AllocStackInst (:any) $?anon_17_sourceOrNext: any
// CHKRA-NEXT:                 StoreStackInst {r7} %105: any, {r1} %107: any
// CHKRA-NEXT:  {r7}     %109 = IteratorBeginInst (:any) {r1} %107: any
// CHKRA-NEXT:                 StoreStackInst {r7} %109: any, {r6} %106: any
// CHKRA-NEXT:                 TryStartInst %BB32, %BB34
// CHKRA-NEXT:%BB31:
// CHKRA-NEXT:  {r1}     %112 = PhiInst (:any) {r1} %118: any, %BB32, {r1} %125: any, %BB35
// CHKRA-NEXT:  {r6}     %113 = LoadStackInst (:any) {r6} %106: any
// CHKRA-NEXT:  {r6}     %114 = IteratorCloseInst (:any) {r6} %113: any, true: boolean
// CHKRA-NEXT:  {r1}     %115 = MovInst (:any) {r1} %112: any
// CHKRA-NEXT:                 ThrowInst {r1} %115: any, %BB28
// CHKRA-NEXT:%BB32:
// CHKRA-NEXT:  {r7}     %117 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %118 = MovInst (:any) {r7} %117: any
// CHKRA-NEXT:                 BranchInst %BB31
// CHKRA-NEXT:%BB33:
// CHKRA-NEXT:  {r1}     %120 = LoadStackInst (:any) {r1} %107: any
// CHKRA-NEXT:  {r1}     %121 = IteratorNextInst (:any) {r6} %106: any, {r1} %120: any
// CHKRA-NEXT:                 TryStartInst %BB35, %BB37
// CHKRA-NEXT:%BB34:
// CHKRA-NEXT:                 TryEndInst %BB32, %BB33
// CHKRA-NEXT:%BB35:
// CHKRA-NEXT:  {r1}     %124 = CatchInst (:any)
// CHKRA-NEXT:  {r1}     %125 = MovInst (:any) {r1} %124: any
// CHKRA-NEXT:                 BranchInst %BB31
// CHKRA-NEXT:%BB36:
// CHKRA-NEXT:  {r1}     %127 = LoadStackInst (:any) {r6} %106: any
// CHKRA-NEXT:  {r1}     %128 = IteratorCloseInst (:any) {r1} %127: any, false: boolean
// CHKRA-NEXT:                 TryEndInst %BB28, %BB29
// CHKRA-NEXT:%BB37:
// CHKRA-NEXT:                 HBCCmpBrTypeOfIsInst {r1} %121: any, typeOfIs(Undefined,Null), %BB38, %BB39
// CHKRA-NEXT:%BB38:
// CHKRA-NEXT:  {r1}     %131 = LIRLoadConstInst (:string) "Cannot destructure 'undefined' or 'null'.": string
// CHKRA-NEXT:  {r1}     %132 = CallBuiltinInst (:any) [HermesBuiltin.throwTypeError]: number, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, {r1} %131: string
// CHKRA-NEXT:                 UnreachableInst
// CHKRA-NEXT:%BB39:
// CHKRA-NEXT:                 TryEndInst %BB35, %BB36
// CHKRA-NEXT:function_end
