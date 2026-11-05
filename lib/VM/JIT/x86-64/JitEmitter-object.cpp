/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_X86_64
#include "../JitHandlers.h"
#include "JitEmitter-internal.h"
#include "JitEmitter.h"

#include "hermes/BCGen/SerializedLiteralParser.h"
#include "hermes/VM/JSObject-inline.h"

namespace hermes::vm::x86_64 {

void Emitter::newObject(FR frRes) {
  comment("// NewObject r%u", frRes.index());
  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});

  // Allocate the result register.
  HWReg hwRes = getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes, FRType::Pointer);
  auto res = hwRes.gpq();

  // Allocate temporary registers, note that these must be different from the
  // result as we will use all of them together.
  HWReg hwTemp1 = allocTempGpX();
  HWReg hwTemp2 = allocTempGpX();
  auto temp1 = hwTemp1.gpq();
  auto temp2 = hwTemp2.gpq();
  freeReg(hwTemp1);
  freeReg(hwTemp2);

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  // Allocate the object.
  // The allocation-helper precondition (JitEmitter.h) holds: no vector temp
  // is live here (none was allocated), and none of res/temp1/temp2 can be
  // xScratch -- the two temps come from the GP temp ranges and the result
  // from those or the saved-global pool, and xScratch is in neither (see the
  // static_asserts next to kGPTemp1).
  allocInYoung(
      CellKind::JSObjectKind,
      cellSize<JSObject>(),
      res,
      temp1,
      temp2,
      slowPathLab);

  emit_jsobject_init(a, res, /* tempOrPropStorageOpt */ temp2, false);

  // Add the object tag to the result.
  emit_sh_ljs_object(a, res);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, hwRes](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: NewObject r%u", frRes.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        EMIT_RUNTIME_CALL(
            em, SHLegacyValue (*)(SHRuntime *), _sh_ljs_new_object);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
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
  auto parent = hwParent.gpq();
  auto newObjPtr = hwNewObjPtr.gpq();
  auto clazz = hwClazz.gpq();
  auto temp1 = hwTemp1.gpq();
  auto temp2 = hwTemp2.gpq();
  auto temp3 = hwTemp3.gpq();
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
  // to the slow path instead. See newObjectWithBuffer() for why this is a
  // compare against memory.
  a.cmp(
      x86::byte_ptr(xRuntime, RuntimeOffsets::runtimeHadesOGMarkingBarriers),
      asmjit::Imm(0));
  a.jne(slowPathLab);
#endif
  // Load the HC from the cache.
  static_assert(
      std::is_same_v<
          TransparentConservativeVector<WeakRoot<HiddenClass>>,
          RuntimeOffsets::RuntimeModuleObjectLiteralHiddenClassesType>,
      "objectLiteralHiddenClasses_ must be transparent");
  loadBits64InGp(
      temp1, (uint64_t)codeBlock_->getRuntimeModule(), "RuntimeModule");
  a.mov(
      temp1,
      x86::qword_ptr(
          temp1, RuntimeOffsets::runtimeModuleObjectLiteralHiddenClasses));
  size_t shapeOfs = (size_t)shapeTableIndex * sizeof(WeakRoot<HiddenClass>);
  assert(
      shapeOfs <= (size_t)INT32_MAX && "shape table offset must fit a disp32");
  emit_load_cp(a, clazz, x86::ptr(temp1, (int32_t)shapeOfs));
  // If the HC isn't cached, slow path.
  a.test(clazz, clazz);
  a.jz(slowPathLab);

  // Compute what the parent should be into temp2. We must not overwrite
  // parent, because it may be a register that the allocator still believes
  // holds the live frParent value. Normalize to a CP-encoded raw pointer
  // (or 0 for null), so we can compare against the HC's objectParent.
  auto decodeObjParentLab = a.newLabel();
  auto parentDoneLab = a.newLabel();

  // Use temp2 for parent calculations below.
  a.mov(temp2, parent);

  // Check if the parent is an object.
  emit_sh_ljs_is_object(a, temp1, parent);
  a.je(decodeObjParentLab);

  // Check if the parent is null.
  emit_sh_ljs_is_null(a, temp1, parent);
  // Set it to zero so we store nullptr if the parent is JS null.
  // x86-64: this must be `mov reg, 0` and not the shorter `xor`, which would
  // destroy the flags the branch below reads. arm64's `mov` is flag-clean for
  // the same reason.
  a.mov(temp2, asmjit::Imm(0));
  a.je(parentDoneLab);

  // The parent is not an object or null, so get Object.prototype.
  a.mov(temp2, x86::qword_ptr(xRuntime, offsetof(Runtime, objectPrototype)));

  // Extract the parent object pointer from the HermesValue in temp2.
  a.bind(decodeObjParentLab);
  emit_sh_ljs_get_pointer(a, temp2, temp2);
  emit_sh_cp_encode_non_null(a, temp2);

  a.bind(parentDoneLab);

  // If the parent isn't correct, slow path.
  a.mov(temp1, clazz);
  emit_sh_cp_decode_non_null(a, temp1);
  emit_load_cp(
      a, temp1, x86::ptr(temp1, RuntimeOffsets::hiddenClassObjectParent));
  a.cmp(temp1, temp2);
  a.jne(slowPathLab);

  // See newObject() for why the allocation-helper precondition holds. The
  // extra live values here, `parent` and `clazz`, are each either a GP temp
  // (which bumpAllocAndUnpoison pushes and pops around its ASan call) or a
  // callee-saved global, so they survive either way.
  allocInYoung(
      CellKind::JSObjectKind,
      cellSize<JSObject>(),
      newObjPtr,
      temp1,
      temp3,
      slowPathLab);

  // Initialize the object.
  emit_jsobject_init(
      a, newObjPtr, /* tempOrPropStorageOpt */ temp1, false, clazz);

  auto hwRes = getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes, FRType::Pointer);

  // Move the object into the result register as a HermesValue.
  emit_sh_ljs_object2(a, hwRes.gpq(), newObjPtr);

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
        em.a.mov(x86::rdi, xRuntime);
        em.loadBits64InGp(x86::rsi, (uint64_t)em.codeBlock_, "CodeBlock");
        em.loadFrameAddr(x86::rdx, frParent);
        em.a.mov(x86::ecx, shapeTableIndex);

        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(
                Runtime &, CodeBlock *, PinnedHermesValue *, uint32_t),
            _interpreter_create_object_with_parent);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
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
  x86::Gp obj = hwObj.gpq();
  x86::Gp clazz = hwClazz.gpq();
  x86::Gp tmp = hwTmp.gpq();
  x86::Gp tmp2 = hwTmp2.gpq();
  freeReg(hwObj);
  freeReg(hwClazz);
  freeReg(hwTmp);
  freeReg(hwTmp2);

  // Load the HiddenClass from the cache.
#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
  // First check the read barrier.
  // x86-64: arm64 has to load the flag into a register (through
  // emit_load_from_base_offset, since its byte load has a limited immediate
  // range) before it can test it. x86 compares against memory directly, so
  // neither tmp nor tmp2 is touched here.
  a.cmp(
      x86::byte_ptr(xRuntime, RuntimeOffsets::runtimeHadesOGMarkingBarriers),
      asmjit::Imm(0));
  a.jne(slowPathLab);
#endif
  // Load the HC from the cache.
  static_assert(
      std::is_same_v<
          TransparentConservativeVector<WeakRoot<HiddenClass>>,
          RuntimeOffsets::RuntimeModuleObjectLiteralHiddenClassesType>,
      "objectLiteralHiddenClasses_ must be transparent");
  loadBits64InGp(
      clazz, (uint64_t)codeBlock_->getRuntimeModule(), "RuntimeModule");
  a.mov(
      clazz,
      x86::qword_ptr(
          clazz, RuntimeOffsets::runtimeModuleObjectLiteralHiddenClasses));
  size_t shapeOfs = (size_t)shapeTableIndex * sizeof(WeakRoot<HiddenClass>);
  assert(
      shapeOfs <= (size_t)INT32_MAX && "shape table offset must fit a disp32");
  emit_load_cp(a, clazz, x86::ptr(clazz, (int32_t)shapeOfs));
  // If the HC isn't cached, slow path.
  a.test(clazz, clazz);
  a.jz(slowPathLab);

  // Create the object.
  const unsigned numIndirectSlots =
      shapeInfo.numProps <= JSObject::DIRECT_PROPERTY_SLOTS
      ? 0
      : shapeInfo.numProps - JSObject::DIRECT_PROPERTY_SLOTS;
  if (numIndirectSlots > 0) {
    // Need indirect property storage, so allocate 2 cells.
    // We know that both together should be able to fit in young gen if there's
    // space due to the above checks.
    // PropStorage will be in tmp2.
    //
    // The alloc2InYoung precondition (JitEmitter.h) holds: no vector temp is
    // live -- this emitter allocates none -- and obj/tmp2/tmp all come from
    // the GP temp ranges, which cannot be xScratch. `clazz`, loaded above and
    // still live across the call, is likewise a GP temp, so the ASan
    // save/restore loop inside bumpAllocAndUnpoison preserves it.
    alloc2InYoung(
        CellKind::JSObjectKind,
        cellSize<JSObject>(),
        PropStorage::getCellKind(),
        PropStorage::allocationSize(numIndirectSlots),
        /* out1 */ obj,
        /* out2 */ tmp2,
        tmp,
        slowPathLab);
  } else {
    // Same precondition argument as the alloc2InYoung call above.
    allocInYoung(
        CellKind::JSObjectKind,
        cellSize<JSObject>(),
        obj,
        tmp,
        tmp2,
        slowPathLab);
  }

  // Initialize the JSObject to have the correct parent/HC.
  emit_jsobject_init(
      a,
      obj,
      /* tempOrPropStorageOpt */ tmp2,
      numIndirectSlots > 0,
      /* clazz */ clazz);

  if (numIndirectSlots > 0) {
    // Populate the size.
    // x86-64: a 32-bit store of an immediate needs no register, so arm64's
    // mov-then-str pair is a single instruction and tmp stays untouched.
    a.mov(
        x86::dword_ptr(
            obj,
            (int32_t)(heapAlignSize(cellSize<JSObject>()) +
                      offsetof(SHArrayStorageSmall, size))),
        asmjit::Imm(numIndirectSlots));
  }

  // Come back from the slow path with obj having the JSObject pointer.
  a.bind(contLab);

  // Store the HermesValue encoded result and never update it again.
  // From here on we'll just populate the values via a raw pointer to the
  // JSObject in obj.
  emit_sh_ljs_object2(a, hwRes.gpq(), obj);

  // Store each of the values to the object at obj.
  // No write barrier required because the object and property storage were
  // allocated in the young gen.
  struct {
    Emitter &em;
    x86::Gp &obj;
    x86::Gp &tmp;
    x86::Gp &tmp2;

    /// Iteration counter.
    /// Index of the next value to be inserted into the object.
    size_t i = 0;

    // \return the offset from obj where we'll place the property.
    //   obj either points to the JSObject or to indirect property storage
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

    /// Store whatever is already in tmp into the current offset in the
    /// object.
    ///
    /// x86-64: arm64 needs a register-offset retry when the scaled store
    /// immediate runs out of range; a disp32 covers every slot a young-gen
    /// literal can have, so the store always encodes and tmp2 is not needed.
    void storeTmpAtCurrentOffset() {
      size_t ofs = currentOffset();
      assert(ofs <= (size_t)INT32_MAX && "slot offset must fit a disp32");
      emit_store_shv(em.a, tmp, x86::ptr(obj, (int32_t)ofs));
    }

    /// Store a simple SmallHermesValue into the current offset in the object.
    void storeVal(SmallHermesValue val) {
      em.loadSmallHermesValueInGpX(tmp, val, "object literal buffer val");
      storeTmpAtCurrentOffset();
    }

    /// Advance internal state to the next slot, updating the counter and any
    /// registers so that they have the correct state for the next element.
    void advance() {
      ++i;

      // obj was set to the JSObject itself initially.
      // If we are transitioning into indirect storage, switch obj to point to
      // the indirect property storage.
      if (i == JSObject::DIRECT_PROPERTY_SLOTS) {
        // We know alloc2InYoung succeded so we can just advance the pointer.
#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
        em.a.add(obj, asmjit::Imm(heapAlignSize(cellSize<JSObject>())));
#else
        emit_load_cp(
            em.a, obj, x86::ptr(obj, offsetof(SHJSObject, propStorage)));
        emit_sh_cp_decode_non_null(em.a, obj);
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

      // tmp = identifierTable_.lookupVector_.ptr
      em.loadConstStringInGpX(symID, tmp);
      // Encode compressed pointer and wrap with StringTag.
      // We know it's not null because we allocated it at JIT compile time.
      emit_sh_cp_encode_non_null(em.a, tmp);
      emit_shv_string(em.a, tmp);
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
  } emittingVisitor{*this, obj, tmp, tmp2};

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
        em.a.mov(x86::rdi, xRuntime);
        em.loadBits64InGp(x86::rsi, (uint64_t)em.codeBlock_, "CodeBlock");
        em.a.mov(x86::edx, asmjit::Imm(shapeTableIndex));
        em.loadFrameAddr(x86::rcx, frRes);
        EMIT_RUNTIME_CALL(
            em,
            JSObject *
                (*)(Runtime &, CodeBlock *, uint32_t, PinnedHermesValue *),
            _jit_new_empty_object_for_buffer);
        // Move into obj.
        em.movHWFromHW<false>(hwObj, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
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
  a.mov(x86::rdi, xRuntime);
  loadBits64InGp(x86::rsi, (uint64_t)codeBlock_, "CodeBlock");
  a.mov(x86::edx, asmjit::Imm(shapeTableIndex));
  a.mov(x86::ecx, asmjit::Imm(valBufferOffset));
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
  a.mov(x86::rdi, xRuntime);
  loadBits64InGp(x86::rsi, (uint64_t)codeBlock_, "CodeBlock");
  loadFrameAddr(x86::rdx, frParent);
  a.mov(x86::ecx, asmjit::Imm(shapeTableIndex));
  a.mov(x86::r8d, asmjit::Imm(valBufferOffset));
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
  a.mov(x86::rdi, xRuntime);
  loadBits64InGp(x86::rsi, (uint64_t)codeBlock_, "CodeBlock");
  loadFrameAddr(x86::rdx, frParent);
  a.mov(x86::ecx, asmjit::Imm(shapeTableIndex));
  a.mov(x86::r8d, asmjit::Imm(valBufferOffset));
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
  HWReg hwTmp2 = allocTempGpX();
  freeReg(hwTmp);
  freeReg(hwTmp2);
  x86::Gp tmp = hwTmp.gpq();
  x86::Gp tmp2 = hwTmp2.gpq();
  x86::Gp res = hwRes.gpq();
  emit_sh_ljs_get_pointer(a, tmp, hwObj.gpq());
  // tmp contains the unencoded pointer value.
  emit_load_cp(a, tmp, x86::ptr(tmp, offsetof(SHJSObject, clazz)));
  emit_sh_cp_decode_non_null(a, tmp);
  emit_load_cp(a, tmp, x86::ptr(tmp, RuntimeOffsets::hiddenClassObjectParent));
  // x86-64: the nullable decode needs a zero register, which arm64 gets for
  // free as xzr; hence the extra temp, which is unused in the HV64 build.
  // See emit_sh_cp_decode().
  emit_sh_cp_decode(a, tmp, /* zeroTemp */ tmp2);
  // x86-64: arm64 sets the flags here and keeps them across the encode,
  // because its movk-based tagging writes no flags. Every instruction in
  // emit_sh_ljs_object2() does (see the comment on emit_sh_ljs_object), so
  // the null test moves after the encode instead. It reads tmp, which the
  // encode leaves untouched.
  // res contains the encoded pointer.
  emit_sh_ljs_object2(a, res, tmp);
  // Check whether the pointer was nullptr and set flags.
  a.test(tmp, tmp);
  // tmp contains encoded null. `mov reg, imm` writes no flags, so it can sit
  // between the test and the cmov.
  loadBits64InGp(tmp, _sh_ljs_null().raw, "null");
  // If the pointer was nullptr, use encoded null, otherwise encoded ptr.
  a.cmovz(res, tmp);

  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::typedLoadParent(FR frRes, FR frObj) {
  comment("// TypedLoadParent r%u, r%u", frRes.index(), frObj.index());

  HWReg hwObj = getOrAllocFRInGpX(frObj, true);
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  x86::Gp res = hwRes.gpq();
  emit_sh_ljs_get_pointer(a, res, hwObj.gpq());
  emit_load_cp(a, res, x86::ptr(res, offsetof(SHJSObject, clazz)));
  emit_sh_cp_decode_non_null(a, res);
  emit_load_cp(a, res, x86::ptr(res, RuntimeOffsets::hiddenClassObjectParent));
  emit_sh_cp_decode_non_null(a, res);
  emit_sh_ljs_object(a, res);

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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frLeft);
  loadFrameAddr(x86::rdx, frRight);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
      _sh_ljs_instance_of_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

} // namespace hermes::vm::x86_64

#endif // HERMESVM_JIT_X86_64
