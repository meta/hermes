/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_ARM64
#include "../JitHandlers.h"
#include "JitEmitter-internal.h"
#include "JitEmitter.h"

#include "hermes/BCGen/SerializedLiteralParser.h"
#include "hermes/VM/JSObject-inline.h"

namespace hermes::vm::arm64 {

void Emitter::newObject(FR frRes) {
  comment("// NewObject r%u", frRes.index());
  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});

  // Allocate the result register.
  HWReg hwRes = getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes, FRType::Pointer);
  auto xRes = hwRes.a64GpX();

  // Allocate temporary registers, note that these must be different from the
  // result as we will use all of them together.
  HWReg hwTemp1 = allocTempGpX();
  HWReg hwTemp2 = allocTempGpX();
  auto xTemp1 = hwTemp1.a64GpX();
  auto xTemp2 = hwTemp2.a64GpX();
  freeReg(hwTemp1);
  freeReg(hwTemp2);

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  // Allocate the object.
  allocInYoung(
      CellKind::JSObjectKind,
      cellSize<JSObject>(),
      xRes,
      xTemp1,
      xTemp2,
      slowPathLab);

  emit_jsobject_init(a, xRes, /* xTempOrPropStorageOpt */ xTemp2, false);

  // Add the object tag to the result.
  emit_sh_ljs_object(a, xRes);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, hwRes](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: NewObject r%u", frRes.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(a64::x0, xRuntime);
        EMIT_RUNTIME_CALL(
            em, SHLegacyValue (*)(SHRuntime *), _sh_ljs_new_object);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.b(sp.contLab);
      });
}

void Emitter::newObjectWithParent(
    FR frRes,
    FR frParent,
    uint32_t shapeTableIndex) {
  comment("// NewObjectWithParent r%u, r%u", frRes.index(), frParent.index());
  syncAllFRTempExcept(frRes != frParent ? frRes : FR());
  syncToFrame(frParent);
  auto hwParent = getOrAllocFRInGpX(frParent, true);
  auto hwNewObjPtr = allocTempGpX();
  auto hwClazz = allocTempGpX();
  auto hwTemp1 = allocTempGpX();
  auto hwTemp2 = allocTempGpX();
  auto hwTemp3 = allocTempGpX();
  auto xParent = hwParent.a64GpX();
  auto xNewObjPtr = hwNewObjPtr.a64GpX();
  auto xClazz = hwClazz.a64GpX();
  auto xTemp1 = hwTemp1.a64GpX();
  auto xTemp2 = hwTemp2.a64GpX();
  auto xTemp3 = hwTemp3.a64GpX();
  freeAllFRTempExcept({});
  freeReg(hwNewObjPtr);
  freeReg(hwClazz);
  freeReg(hwTemp1);
  freeReg(hwTemp2);
  freeReg(hwTemp3);

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
  // Reading a WeakRoot while the OG is marking needs the read barrier, so bail
  // to the slow path instead.
  emit_load_from_base_offset<1, true>(
      a,
      xTemp1,
      xRuntime,
      xTemp2,
      RuntimeOffsets::runtimeHadesOGMarkingBarriers);
  a.cbnz(xTemp1.w(), slowPathLab);
#endif
  // Load the HC from the cache.
  static_assert(
      std::is_same_v<
          TransparentConservativeVector<WeakRoot<HiddenClass>>,
          RuntimeOffsets::RuntimeModuleObjectLiteralHiddenClassesType>,
      "objectLiteralHiddenClasses_ must be transparent");
  loadBits64InGp(
      xTemp1, (uint64_t)codeBlock_->getRuntimeModule(), "RuntimeModule");
  a.ldr(
      xTemp1,
      a64::Mem(
          xTemp1, RuntimeOffsets::runtimeModuleObjectLiteralHiddenClasses));
  emit_load_from_base_offset<sizeof(WeakRoot<HiddenClass>)>(
      a,
      xClazz,
      xTemp1,
      xTemp2,
      shapeTableIndex * sizeof(WeakRoot<HiddenClass>));
  // If the HC isn't cached, slow path.
  a.cbz(xClazz, slowPathLab);

  // Compute what the parent should be into xTemp2. We must not overwrite
  // xParent, because it may be a register that the allocator still believes
  // holds the live frParent value. Normalize to a CP-encoded raw pointer
  // (or 0 for null), so we can compare against the HC's objectParent.
  auto decodeObjParentLab = a.newLabel();
  auto parentDoneLab = a.newLabel();

  // Use xTemp2 for parent calculations below.
  a.mov(xTemp2, xParent);

  // Check if the parent is an object.
  emit_sh_ljs_is_object(a, xTemp1, xParent);
  a.b_eq(decodeObjParentLab);

  // Check if the parent is null.
  emit_sh_ljs_is_null(a, xTemp1, xParent);
  // Set it to zero so we store nullptr if the parent is JS null.
  a.mov(xTemp2, 0);
  a.b_eq(parentDoneLab);

  // The parent is not an object or null, so get Object.prototype.
  a.ldr(xTemp2, a64::Mem(xRuntime, offsetof(Runtime, objectPrototype)));

  // Extract the parent object pointer from the HermesValue in xTemp2.
  a.bind(decodeObjParentLab);
  emit_sh_ljs_get_pointer(a, xTemp2, xTemp2);
  emit_sh_cp_encode_non_null(a, xTemp2);

  a.bind(parentDoneLab);

  // If the parent isn't correct, slow path.
  auto &xClazzDecoded =
      emit_sh_cp_decode_non_null_preserve_input(a, xTemp1, xClazz);
  emit_load_cp(
      a,
      xTemp1,
      a64::Mem(xClazzDecoded, RuntimeOffsets::hiddenClassObjectParent));
  a.cmp(xTemp1, xTemp2);
  a.b_ne(slowPathLab);

  allocInYoung(
      CellKind::JSObjectKind,
      cellSize<JSObject>(),
      xNewObjPtr,
      xTemp1,
      xTemp3,
      slowPathLab);

  // Initialize the object.
  emit_jsobject_init(a, xNewObjPtr, xTemp1, false, xClazz);

  auto hwRes = getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes, FRType::Pointer);

  // Move the object into the result register as a HermesValue.
  emit_sh_ljs_object2(a, hwRes.a64GpX(), xNewObjPtr);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, frParent, hwRes, shapeTableIndex](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: NewObjectWithParent r%u, r%u",
            frRes.index(),
            frParent.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(a64::x0, xRuntime);
        em.loadBits64InGp(a64::x1, (uint64_t)em.codeBlock_, "CodeBlock");
        em.loadFrameAddr(a64::x2, frParent);
        em.a.mov(a64::w3, shapeTableIndex);

        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(
                Runtime &, CodeBlock *, PinnedHermesValue *, uint32_t),
            _interpreter_create_object_with_parent);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.b(sp.contLab);
      });
}

void Emitter::newObjectWithBuffer(
    FR frRes,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  ShapeTableEntry shapeInfo = codeBlock_->getRuntimeModule()
                                  ->getBytecode()
                                  ->getObjectShapeTable()[shapeTableIndex];

  // If the object and the property storage can't be guaranteed to be in the
  // young gen, bail.
  // TODO: apply the inline write barrier (emitSafeStoreOrSlow) here.
  if (shapeInfo.numProps > JSObject::maxYoungGenAllocationPropCount()) {
    newObjectWithBufferSlow(frRes, shapeTableIndex, valBufferOffset);
    return;
  }

  // Simple visitor to check if the fast path is possible.
  struct {
    void visitStringID(StringID) {}
    void visitNumber(double d) {
      // TODO: Implement fast loading for boxed double values.
      if (!SmallHermesValue::canInlineDouble(d))
        fast = false;
    }
    void visitNull() {}
    void visitUndefined() {}
    void visitBool(bool) {}
    bool fast = true;
  } fastPathCheckVisitor{};

  SerializedLiteralParser::parseValueBuffer(
      codeBlock_->getRuntimeModule()
          ->getBytecode()
          ->getLiteralValueBuffer()
          .slice(valBufferOffset),
      shapeInfo.numProps,
      fastPathCheckVisitor);

  // If we can't use the fast path, fall back to the slow path.
  if (!fastPathCheckVisitor.fast) {
    newObjectWithBufferSlow(frRes, shapeTableIndex, valBufferOffset);
    return;
  }

  // Fast path: we can create the object directly.
  comment(
      "// NewObjectWithBuffer r%u, %u, %u",
      frRes.index(),
      shapeTableIndex,
      valBufferOffset);

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});

  HWReg hwRes = getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);

  HWReg hwObj = allocTempGpX();
  HWReg hwClazz = allocTempGpX();
  HWReg hwTmp = allocTempGpX();
  HWReg hwTmp2 = allocTempGpX();
  a64::GpX xObj = hwObj.a64GpX();
  a64::GpX xClazz = hwClazz.a64GpX();
  a64::GpX xTmp = hwTmp.a64GpX();
  a64::GpX xTmp2 = hwTmp2.a64GpX();
  freeReg(hwObj);
  freeReg(hwClazz);
  freeReg(hwTmp);
  freeReg(hwTmp2);

  // Load the HiddenClass from the cache.
#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
  // First check the read barrier.
  emit_load_from_base_offset<1, true>(
      a, xTmp, xRuntime, xTmp2, RuntimeOffsets::runtimeHadesOGMarkingBarriers);
  a.cbnz(xTmp.w(), slowPathLab);
#endif
  // Load the HC from the cache.
  static_assert(
      std::is_same_v<
          TransparentConservativeVector<WeakRoot<HiddenClass>>,
          RuntimeOffsets::RuntimeModuleObjectLiteralHiddenClassesType>,
      "objectLiteralHiddenClasses_ must be transparent");
  loadBits64InGp(
      xClazz, (uint64_t)codeBlock_->getRuntimeModule(), "RuntimeModule");
  a.ldr(
      xClazz,
      a64::Mem(
          xClazz, RuntimeOffsets::runtimeModuleObjectLiteralHiddenClasses));
  emit_load_from_base_offset<sizeof(WeakRoot<HiddenClass>)>(
      a, xClazz, xClazz, xTmp, shapeTableIndex * sizeof(WeakRoot<HiddenClass>));
  // If the HC isn't cached, slow path.
  a.cbz(xClazz, slowPathLab);

  // Create the object.
  const unsigned numIndirectSlots =
      shapeInfo.numProps <= JSObject::DIRECT_PROPERTY_SLOTS
      ? 0
      : shapeInfo.numProps - JSObject::DIRECT_PROPERTY_SLOTS;
  if (numIndirectSlots > 0) {
    // Need indirect property storage, so allocate 2 cells.
    // We know that both together should be able to fit in young gen if there's
    // space due to the above checks.
    // PropStorage will be in xTmp2.
    alloc2InYoung(
        CellKind::JSObjectKind,
        cellSize<JSObject>(),
        PropStorage::getCellKind(),
        PropStorage::allocationSize(numIndirectSlots),
        /* xOut1 */ xObj,
        /* xOut2 */ xTmp2,
        xTmp,
        slowPathLab);
  } else {
    allocInYoung(
        CellKind::JSObjectKind,
        cellSize<JSObject>(),
        xObj,
        xTmp,
        xTmp2,
        slowPathLab);
  }

  // Initialize the JSObject to have the correct parent/HC.
  emit_jsobject_init(
      a,
      xObj,
      /* xTempOrPropStorageOpt */ xTmp2,
      numIndirectSlots > 0,
      /* xClazz */ xClazz);

  if (numIndirectSlots > 0) {
    // The parent in xTmp is no longer needed.
    // Populate the size.
    a.mov(xTmp, numIndirectSlots);
    a.str(
        xTmp.w(),
        a64::Mem(
            xObj,
            heapAlignSize(cellSize<JSObject>()) +
                offsetof(SHArrayStorageSmall, size)));
  }

  // Come back from the slow path with xObj having the JSObject pointer.
  a.bind(contLab);

  // Store the HermesValue encoded result and never update it again.
  // From here on we'll just populate the values via a raw pointer to the
  // JSObject in xObj.
  emit_sh_ljs_object2(a, hwRes.a64GpX(), xObj);

  // Store each of the values to the object at xObj.
  // No write barrier required because the object and property storage were
  // allocated in the young gen.
  struct {
    Emitter &em;
    a64::GpX &xObj;
    a64::GpX &xTmp;
    a64::GpX &xTmp2;
    /// Needed by the convenient EXPECT_ERROR macro, which expects to be able to
    /// use the Emitter::expectedError_ field (using implicit 'this').
    /// Reference it directly here so the macro keeps working.
    asmjit::Error &expectedError_ = em.expectedError_;

    /// Iteration counter.
    /// Index of the next value to be inserted into the object.
    size_t i = 0;

    // \return the offset from xObj where we'll place the property.
    //   xObj either points to the JSObject or to indirect property storage
    //   depending on the value of \c i.
    size_t currentOffset() const {
      if (i < JSObject::DIRECT_PROPERTY_SLOTS) {
        return offsetof(SHJSObjectAndDirectProps, directProps) +
            i * sizeof(SHGCSmallHermesValue);
      } else {
        // If we're reading from indirect storage, offset by the number of
        // direct property slots.
        auto storageSlot = i - JSObject::DIRECT_PROPERTY_SLOTS;
        return offsetof(SHArrayStorageSmall, storage) +
            storageSlot * sizeof(SHGCSmallHermesValue);
      }
    }

    /// Store whatever is already in xTmp into the current offset in the
    /// object, falling back to a register offset when the displacement is too
    /// large to encode.
    void storeTmpAtCurrentOffset() {
      asmjit::Error err;
      EXPECT_ERROR(
          asmjit::kErrorInvalidDisplacement,
          err = emit_store_shv(em.a, xTmp, a64::Mem(xObj, currentOffset())));
      if (err) {
        em.a.mov(xTmp2, currentOffset());
        emit_store_shv(em.a, xTmp, a64::Mem(xObj, xTmp2));
      }
    }

    /// Store a simple SmallHermesValue into the current offset in the object.
    void storeVal(SmallHermesValue val) {
      em.loadSmallHermesValueInGpX(xTmp, val, "object literal buffer val");
      storeTmpAtCurrentOffset();
    }

    /// Advance internal state to the next slot, updating the counter and any
    /// registers so that they have the correct state for the next element.
    void advance() {
      ++i;

      // xObj was set to the JSObject itself initially.
      // If we are transitioning into indirect storage, switch xObj to point to
      // the indirect property storage.
      if (i == JSObject::DIRECT_PROPERTY_SLOTS) {
        // We know alloc2InYoung succeded so we can just advance the pointer.
#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
        em.a.add(xObj, xObj, heapAlignSize(cellSize<JSObject>()));
#else
        emit_load_cp(
            em.a, xObj, a64::Mem(xObj, offsetof(SHJSObject, propStorage)));
        emit_sh_cp_decode_non_null(em.a, xObj);
#endif
      }
    }

    void visitStringID(StringID id) {
      em.comment("    ; string");
      RuntimeModule *runtimeModule = em.codeBlock_->getRuntimeModule();
      Runtime &runtime = runtimeModule->getRuntime();
      SymbolID symID = runtimeModule->getSymbolIDFromStringIDMayAllocate(id);

      // Force allocation of the StringPrimitive at JIT time.
      // StringPrimitive won't be freed because RuntimeModule keeps the symbols
      // in stringIDMap_ alive and StringPrimitives with live symbols aren't
      // freed.
      [[maybe_unused]] StringPrimitive *strPrim =
          runtime.getStringPrimFromSymbolID(symID);
      assert(strPrim && "must be allocated");

      // xTmp = identifierTable_.lookupVector_.ptr
      em.loadConstStringInGpX(symID, xTmp, xTmp2);
      // Encode compressed pointer and wrap with StringTag.
      // We know it's not null because we allocated it at JIT compile time.
      emit_sh_cp_encode_non_null(em.a, xTmp);
      emit_shv_string(em.a, xTmp);
      // Store to storage, falling back to a register offset when the
      // displacement does not encode, as storeVal does.
      storeTmpAtCurrentOffset();

      advance();
    }
    void visitNumber(double d) {
      em.comment("    ; number: %lf", d);
      assert(
          SmallHermesValue::canInlineDouble(d) &&
          "boxed doubles not supported in fast path");
      storeVal(SmallHermesValue::encodeInlineDoubleValueUnsafe(d));
      advance();
    }
    void visitNull() {
      em.comment("    ; null");
      storeVal(SmallHermesValue::encodeNullValue());
      advance();
    }
    void visitUndefined() {
      em.comment("    ; undefined");
      storeVal(SmallHermesValue::encodeUndefinedValue());
      advance();
    }
    void visitBool(bool b) {
      em.comment("    ; bool: %d", b);
      storeVal(SmallHermesValue::encodeBoolValue(b));
      advance();
    }
  } emittingVisitor{*this, xObj, xTmp, xTmp2};

  SerializedLiteralParser::parseValueBuffer(
      codeBlock_->getRuntimeModule()
          ->getBytecode()
          ->getLiteralValueBuffer()
          .slice(valBufferOffset),
      shapeInfo.numProps,
      emittingVisitor);

  // This slow path only allocates the new object.
  // Property population is always done in JIT-emitted code specialized for this
  // shape table entry.
  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, hwObj, shapeTableIndex, valBufferOffset](
          Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: NewObjectWithBuffer r%u, %u, %u",
            frRes.index(),
            shapeTableIndex,
            valBufferOffset);
        em.a.bind(sp.slowPathLab);
        em.a.mov(a64::x0, xRuntime);
        em.loadBits64InGp(a64::x1, (uint64_t)em.codeBlock_, "CodeBlock");
        em.a.mov(a64::w2, shapeTableIndex);
        em.loadFrameAddr(a64::x3, frRes);
        EMIT_RUNTIME_CALL(
            em,
            JSObject *
                (*)(Runtime &, CodeBlock *, uint32_t, PinnedHermesValue *),
            _jit_new_empty_object_for_buffer);
        // Move into xObj.
        em.movHWFromHW<false>(hwObj, HWReg::gpX(0));
        em.a.b(sp.contLab);
      });
}

void Emitter::newObjectWithBufferSlow(
    FR frRes,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  comment(
      "// NewObjectWithBuffer r%u, %u, %u",
      frRes.index(),
      shapeTableIndex,
      valBufferOffset);

  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});
  a.mov(a64::x0, xRuntime);
  loadBits64InGp(a64::x1, (uint64_t)codeBlock_, "CodeBlock");
  a.mov(a64::w2, shapeTableIndex);
  a.mov(a64::w3, valBufferOffset);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHCodeBlock *, uint32_t, uint32_t),
      _interpreter_create_object_from_buffer);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::newObjectWithBufferAndParent(
    FR frRes,
    FR frParent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset) {
  comment(
      "// NewObjectWithBufferAndParent r%u, r%u, %u, %u",
      frRes.index(),
      frParent.index(),
      shapeTableIndex,
      valBufferOffset);

  // We unconditionally skip frRes here because we handle frParent with the
  // syncToFrame below.
  syncAllFRTempExcept(frRes);
  syncToFrame(frParent);
  freeAllFRTempExcept({});
  a.mov(a64::x0, xRuntime);
  loadBits64InGp(a64::x1, (uint64_t)codeBlock_, "CodeBlock");
  loadFrameAddr(a64::x2, frParent);
  a.mov(a64::w3, shapeTableIndex);
  a.mov(a64::w4, valBufferOffset);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *, SHCodeBlock *, SHLegacyValue *, uint32_t, uint32_t),
      _interpreter_create_object_from_buffer_with_parent);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::newTypedObjectWithBuffer(
    FR frRes,
    FR frParent,
    uint32_t shapeTableIndex,
    uint32_t valBufferOffset,
    uint8_t nonEnumerable) {
  comment(
      "// NewTypedObjectWithBuffer r%u, r%u, %u, %u, %u",
      frRes.index(),
      frParent.index(),
      shapeTableIndex,
      valBufferOffset,
      nonEnumerable);

  // We unconditionally skip frRes here because we handle frParent with the
  // syncToFrame below.
  syncAllFRTempExcept(frRes);
  syncToFrame(frParent);
  freeAllFRTempExcept({});
  a.mov(a64::x0, xRuntime);
  loadBits64InGp(a64::x1, (uint64_t)codeBlock_, "CodeBlock");
  loadFrameAddr(a64::x2, frParent);
  a.mov(a64::w3, shapeTableIndex);
  a.mov(a64::w4, valBufferOffset);
  if (nonEnumerable) {
    EMIT_RUNTIME_CALL(
        *this,
        SHLegacyValue (*)(
            SHRuntime *, SHCodeBlock *, SHLegacyValue *, uint32_t, uint32_t),
        _interpreter_create_typed_non_enum_object_from_buffer);
  } else {
    EMIT_RUNTIME_CALL(
        *this,
        SHLegacyValue (*)(
            SHRuntime *, SHCodeBlock *, SHLegacyValue *, uint32_t, uint32_t),
        _interpreter_create_typed_object_from_buffer);
  }
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::loadParentNoTraps(FR frRes, FR frObj) {
  comment("// LoadParentNoTraps r%u, r%u", frRes.index(), frObj.index());

  HWReg hwObj = getOrAllocFRInGpX(frObj, true);
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  HWReg hwTmp = allocTempGpX();
  freeReg(hwTmp);
  a64::GpX xTmp = hwTmp.a64GpX();
  a64::GpX xRes = hwRes.a64GpX();
  emit_sh_ljs_get_pointer(a, xTmp, hwObj.a64GpX());
  // xTmp contains the unencoded pointer value.
  emit_load_cp(a, xTmp, a64::Mem(xTmp, offsetof(SHJSObject, clazz)));
  emit_sh_cp_decode_non_null(a, xTmp);
  emit_load_cp(
      a, xTmp, a64::Mem(xTmp, RuntimeOffsets::hiddenClassObjectParent));
  emit_sh_cp_decode(a, xTmp);
  // Check whether it is nullptr and set flags.
  // TODO: Combine this null check with the one in emit_load_and_sh_cp_decode.
  a.cmp(xTmp, 0);
  // xRes contains the encoded pointer.
  emit_sh_ljs_object2(a, xRes, xTmp);
  // xTmp contains encoded null.
  loadBits64InGp(xTmp, _sh_ljs_null().raw, "null");
  // If the pointer was nullptr, use encoded null, otherwise encoded ptr.
  a.csel(xRes, xTmp, xRes, asmjit::arm::CondCode::kEQ);

  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::typedLoadParent(FR frRes, FR frObj) {
  comment("// TypedLoadParent r%u, r%u", frRes.index(), frObj.index());

  HWReg hwObj = getOrAllocFRInGpX(frObj, true);
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  a64::GpX xRes = hwRes.a64GpX();
  emit_sh_ljs_get_pointer(a, xRes, hwObj.a64GpX());
  emit_load_cp(a, xRes, a64::Mem(xRes, offsetof(SHJSObject, clazz)));
  emit_sh_cp_decode_non_null(a, xRes);
  emit_load_cp(
      a, xRes, a64::Mem(xRes, RuntimeOffsets::hiddenClassObjectParent));
  emit_sh_cp_decode_non_null(a, xRes);
  emit_sh_ljs_object(a, xRes);

  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::instanceOf(FR frRes, FR frLeft, FR frRight) {
  comment(
      "// InstanceOf r%u, r%u, r%u",
      frRes.index(),
      frLeft.index(),
      frRight.index());

  syncAllFRTempExcept(frRes != frLeft && frRes != frRight ? frRes : FR());
  syncToFrame(frLeft);
  syncToFrame(frRight);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frLeft);
  loadFrameAddr(a64::x2, frRight);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
      _sh_ljs_instance_of_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

} // namespace hermes::vm::arm64

#endif // HERMESVM_JIT_ARM64
