/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SHUnitExt.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PropertyCache.h"
#include "hermes/VM/StaticHUtils.h"

#include <cstdarg>

using namespace hermes;
using namespace hermes::vm;

static void sh_unit_init_symbols(Runtime &runtime, SHUnit *unit);
static SHLegacyValue sh_unit_run(SHRuntime *shr, SHUnit *unit);

extern "C" bool _sh_initialize_units(SHRuntime *shr, uint32_t count, ...) {
  Runtime &runtime = getRuntime(shr);
  bool success = true;

  va_list ap;
  va_start(ap, count);

  for (uint32_t i = 0; i < count; ++i) {
    SHUnitCreator unitCreator = va_arg(ap, SHUnitCreator);
    SHLegacyValue val;
    if (!_sh_unit_init_guarded(shr, unitCreator, &val)) {
      GCScope gcScope{runtime};
      // Make sure stdout catches up to stderr.
      llvh::outs().flush();
      runtime.printException(
          llvh::errs(), runtime.makeHandle(HermesValue::fromRaw(val.raw)));
      success = false;
      break;
    }
  }

  va_end(ap);
  return success;
}

extern "C" bool _sh_unit_init_guarded(
    SHRuntime *shr,
    SHUnitCreator unitCreator,
    SHLegacyValue *resOrExc) {
  Runtime &runtime = getRuntime(shr);
  GCScope gcScope{runtime};

  SHLocals locals;
  SHLegacyValue *frame = runtime.getCurrentFrame().ptr();
  SHLegacyValue *savedSP = _sh_push_locals(shr, &locals, 0);
  locals.count = 0;
  SHJmpBuf jbuf;
  bool success = true;

  if (_sh_try(shr, &jbuf) == 0) {
    *resOrExc = _sh_unit_init(shr, unitCreator);
    _sh_end_try(shr, &jbuf);
  } else {
    *resOrExc = _sh_catch(shr, &locals, frame, savedSP - frame);
    success = false;
  }

  _sh_pop_locals(shr, &locals, savedSP);
  return success;
}

extern "C" SHLegacyValue _sh_unit_init(
    SHRuntime *shr,
    SHUnitCreator unitCreator) {
  Runtime &runtime = getRuntime(shr);
  auto *unit = unitCreator();

  {
    // Counter to assign a globally unique index to each unit.
    static uint32_t nextIndex = 1;

    // Lock protecting next index, and preventing a race on the unit indices.
    // The latter works based on the following observations:
    //   1. Only this function can write to the index of a unit.
    //   2. No code outside this function can read the index of a unit before it
    //   has been written by this function.
    // This means that by holding this mutex when checking and initializing the
    // index below, we know that no other thread may be reading or writing the
    // index at the same time.
    static std::mutex idxMtx;
    std::lock_guard<std::mutex> lock(idxMtx);
    // If this unit does not have an index yet, assign one.
    if (!*unit->index) {
      if (nextIndex == std::size(runtime.units)) {
        fprintf(stderr, "Too many SH units registered\n");
        abort();
      }
      *unit->index = nextIndex++;
    }
  }

  // If the unit has already been initialized, discard the new copy.
  if (auto *existingUnit = runtime.units[*unit->index]) {
    free(unit);
    return sh_unit_run(shr, existingUnit);
  }

  unit->script_id = runtime.allocateScriptId();

  // We need to clean the symbol table in case the GC runs while we are
  // populating it.
  std::fill_n(unit->symbols, unit->num_symbols, SymbolID::EMPTY_ID);

  unit->runtime_ext = new SHUnitExt{unit->num_regexps};

  // Register the unit with the runtime before it is initialized, because the
  // GC could run in the middle of initialization and the already defined
  // symbols should be roots.
  runtime.units[*unit->index] = unit;

  sh_unit_init_symbols(runtime, unit);
  return sh_unit_run(shr, unit);
}

static SHLegacyValue sh_unit_run(SHRuntime *shr, SHUnit *unit) {
  Runtime &runtime = getRuntime(shr);
  struct {
    SHLocals head;
  } locals;
  // NOTE: sh_unit_run() is not a function call, it executes in an existing
  // frame.
  SHLegacyValue *savedSP = _sh_push_locals(
      shr, &locals.head, hbc::StackFrameLayout::callerOutgoingRegisters(0));
  locals.head.count = 0;

  SHLegacyValue closure = _sh_ljs_create_closure(
      shr, nullptr, unit->unit_main, unit->unit_main_info, unit);

  auto frame = StackFramePtr::initFrame(
      runtime.getStackPointer(),
      runtime.getCurrentFrame(),
      runtime.getCurrentIP(),
      nullptr,
      nullptr,
      0,
      HermesValue::fromRaw(closure.raw),
      HermesValue::encodeUndefinedValue());
  frame.getThisArgRef() = runtime.getGlobal().getHermesValue();

  SHLegacyValue res = NativeJSFunction::_legacyCall(
      shr, vmcast<NativeJSFunction>(HermesValue::fromRaw(closure.raw)));

  _sh_pop_locals(shr, &locals.head, savedSP);
  return res;
}

static void sh_unit_init_symbols(Runtime &runtime, SHUnit *unit) {
  // Iterate over all strings and add them to the identifier table.

  // NOTE: since SH units cannot be destroyed/unloaded before the runtime itself
  // is destroyed, we treat units as "persistent", which allows us to register
  // all strings as "lazy". A lazy string reserves an entry in the identifier
  // table with a pointer to the string source, but does not allocate a copy in
  // the GC heap.
  // This speeds up the init path by eliminating almost all GC operations and
  // copies.
  //
  // If units could be unloaded, we would have to adopt the more complicated
  // scheme used by the bytecode. Identifier strings are always registered, even
  // if it requires a copy in the GC heap, while string literals are registered
  // lazily when needed. This ensures that operations using ids remain fast.

  const uint32_t *stringData = unit->strings;
  for (uint32_t symIndex = 0, nSyms = unit->num_symbols; symIndex != nSyms;
       ++symIndex, stringData += 3) {
    SHSymbolID id;
    // UTF-16 string?
    if (stringData[0] & 0x80000000) {
      auto str = UTF16Ref{
          unit->u16_pool + (stringData[0] & 0x7FFFFFFF), stringData[1]};
      // TODO: HACK HACK: if the hash is 0, calculate it.
      uint32_t hash = stringData[2] ? stringData[2] : hashString(str);
      id = runtime.getIdentifierTable()
               .registerLazyIdentifier(runtime, str, hash)
               .unsafeGetRaw();
    } else {
      auto str = ASCIIRef{unit->ascii_pool + stringData[0], stringData[1]};
      // TODO: HACK HACK: if the hash is 0, calculate it.
      uint32_t hash = stringData[2] ? stringData[2] : hashString(str);
      id = runtime.getIdentifierTable()
               .registerLazyIdentifier(runtime, str, hash)
               .unsafeGetRaw();
    }
    unit->symbols[symIndex] = id;
  }
}

void hermes::vm::sh_unit_done(Runtime &runtime, SHUnit *unit) {
  delete unit->runtime_ext;
  free(unit);
}

size_t hermes::vm::sh_unit_additional_memory_size(const SHUnit *unit) {
  const SHUnitExt *ext = unit->runtime_ext;
  size_t regExpSize = ext->regExpCache.capacity() * sizeof(ext->regExpCache[0]);
  for (const auto &entry : ext->regExpCache) {
    if (entry)
      regExpSize += entry->mallocSize();
  }
  return sizeof(*ext) + ext->templateMap.getMemorySize() +
      ext->addCacheEntries.capacity_in_bytes() + regExpSize;
}

llvh::IntrusiveRefCntPtr<RegExpBytecode>
hermes::vm::sh_unit_get_regexp_bytecode(SHUnit *unit, uint32_t regExpID) {
  auto &cache = unit->runtime_ext->regExpCache;
  assert(regExpID < cache.size() && "Invalid regexp id");
  llvh::IntrusiveRefCntPtr<RegExpBytecode> &entry = cache[regExpID];
  if (LLVM_UNLIKELY(!entry)) {
    const SHRegExpTableEntry &te = unit->regexp_table[regExpID];
    entry = RegExpBytecode::create(
        {unit->regexp_storage + te.offset, te.length},
        RegExpBytecode::Ownership::Cached);
  }
  return entry;
}

void hermes::vm::sh_unit_mark_roots(
    SHUnit *unit,
    RootAcceptorWithNames &acceptor,
    bool markLongLived) {
  for (auto &it : unit->runtime_ext->templateMap) {
    acceptor.acceptPtr(it.second);
  }

  if (markLongLived) {
    for (const SHSymbolID *p = unit->symbols, *e = p + unit->num_symbols;
         p != e;
         ++p) {
      const RootSymbolID sym(SymbolID::unsafeCreate(*p));
      if (sym.isValid())
        acceptor.accept(sym);
    }
  }
}

void hermes::vm::sh_unit_mark_weak_roots(
    SHUnit *unit,
    WeakRootAcceptor &acceptor,
    bool markLongLived) {
  // Many of these weak roots are empty. Acceptors ignore empty weak roots, so
  // skip them here instead of making a virtual call for each of them.
  if (markLongLived) {
    for (auto &prop : llvh::makeMutableArrayRef(
             reinterpret_cast<ReadPropertyCacheEntry *>(unit->read_prop_cache),
             unit->num_read_prop_cache_entries)) {
      if (prop.clazz)
        acceptor.acceptWeak(prop.clazz);
      if (prop.negMatchClazz)
        acceptor.acceptWeak(prop.negMatchClazz);
    }
    for (auto &prop : llvh::makeMutableArrayRef(
             reinterpret_cast<WritePropertyCacheEntry *>(
                 unit->write_prop_cache),
             unit->num_write_prop_cache_entries)) {
      if (prop.clazz)
        acceptor.acceptWeak(prop.clazz);
    }
    for (auto &prop : llvh::makeMutableArrayRef(
             reinterpret_cast<PrivateNameCacheEntry *>(
                 unit->private_name_cache),
             unit->num_private_name_cache_entries)) {
      if (prop.clazz)
        acceptor.acceptWeak(prop.clazz);
      if (!prop.nameVal.isInvalid())
        acceptor.acceptWeakSym(prop.nameVal);
    }

    for (auto &entry : llvh::makeMutableArrayRef(
             reinterpret_cast<WeakRootBase *>(unit->object_literal_class_cache),
             unit->obj_shape_table_count)) {
      if (entry)
        acceptor.acceptWeak(entry);
    }
  }

  if (markLongLived) {
    for (auto &entry : unit->runtime_ext->addCacheEntries) {
      if (entry.startClazz)
        acceptor.acceptWeak(entry.startClazz);
      if (entry.resultClazz)
        acceptor.acceptWeak(entry.resultClazz);
      if (entry.parent)
        acceptor.acceptWeak(entry.parent);
    }
  } else {
    for (auto &entry : unit->runtime_ext->addCacheEntries) {
      if (entry.parent)
        acceptor.acceptWeak(entry.parent);
    }
  }
}

OptValue<uint32_t> hermes::vm::sh_unit_allocate_add_cache_entry(SHUnit *unit) {
  auto &entries = unit->runtime_ext->addCacheEntries;
  if (entries.size() < WritePropertyCacheEntry::kMaxAddCacheIndex) {
    uint32_t index = entries.size();
    entries.emplace_back();
    return index;
  }
  return llvh::None;
}

extern "C" SHLegacyValue _sh_get_template_object(
    SHRuntime *shr,
    SHUnit *unit,
    uint32_t templateObjID,
    bool dup,
    uint32_t argCount,
    ...) {
  Runtime &runtime = getRuntime(shr);

  va_list args;
  va_start(args, argCount);

  CallResult<HermesValue> result = [&]() -> CallResult<HermesValue> {
    GCScope gcScope{runtime};

    // Try finding the template object in the template object cache.
    // _sh_find_cached_template_object returns nullptr if cache entry hasn't
    // been populated yet, so we use cast_or_null.
    JSObject *cachedTemplateObj = llvh::cast_or_null<JSObject>(
        (GCCell *)_sh_find_cached_template_object(unit, templateObjID));
    if (cachedTemplateObj) {
      return HermesValue::encodeObjectValue(cachedTemplateObj);
    }

    if (argCount % 2 == 1) {
      assert(dup && "There must be the same number of raw and cooked strings.");
    }
    uint32_t count = dup ? argCount : argCount / 2;

    // Create template object and raw object.
    auto arrRes = JSArray::create(runtime, count, 0);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    Handle<JSObject> rawObj = runtime.makeHandle(std::move(*arrRes));
    auto arrRes2 = JSArray::create(runtime, count, 0);
    if (LLVM_UNLIKELY(arrRes2 == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    Handle<JSObject> templateObj = runtime.makeHandle(std::move(*arrRes2));

    // Set cooked and raw strings as elements in template object and raw object,
    // respectively.
    DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
    dpf.writable = 0;
    dpf.configurable = 0;
    MutableHandle<> idx{runtime};
    auto marker = gcScope.createMarker();
    for (uint32_t i = 0; i < count; ++i) {
      gcScope.flushToMarker(marker);
      idx = HermesValue::encodeTrustedNumberValue(i);

      const SHLegacyValue *rawLegacyValue = va_arg(args, const SHLegacyValue *);
      Handle<> value{toPHV(rawLegacyValue)};

      auto putRes = JSObject::defineOwnComputedPrimitive(
          rawObj, runtime, idx, dpf, value);
      assert(
          putRes != ExecutionStatus::EXCEPTION && *putRes &&
          "Failed to set raw value to raw object.");

      if (dup) {
        // Cooked value is the same as the raw value.
        putRes = JSObject::defineOwnComputedPrimitive(
            templateObj, runtime, idx, dpf, value);
        assert(
            putRes != ExecutionStatus::EXCEPTION && *putRes &&
            "Failed to set cooked value to template object.");
      }
    }
    if (!dup) {
      for (uint32_t i = 0; i < count; ++i) {
        gcScope.flushToMarker(marker);
        idx = HermesValue::encodeTrustedNumberValue(i);

        const SHLegacyValue *cookedLegacyValue =
            va_arg(args, const SHLegacyValue *);
        Handle<> value{toPHV(cookedLegacyValue)};
        auto putRes = JSObject::defineOwnComputedPrimitive(
            templateObj, runtime, idx, dpf, value);
        (void)putRes;
        assert(
            putRes != ExecutionStatus::EXCEPTION && *putRes &&
            "Failed to set cooked value to template object.");
      }
    }

    if (LLVM_UNLIKELY(
            setTemplateObjectProps(runtime, templateObj, rawObj) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    // Cache the template object.
    _sh_cache_template_object(
        unit, templateObjID, templateObj.getHermesValue());

    return templateObj.getHermesValue();
  }();

  va_end(args);

  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *result;
}

extern "C" void *_sh_find_cached_template_object(
    const SHUnit *unit,
    uint32_t templateObjID) {
  return unit->runtime_ext->templateMap.lookup(templateObjID);
}

extern "C" void _sh_cache_template_object(
    SHUnit *unit,
    uint32_t templateObjID,
    SHLegacyValue templateObj) {
  assert(
      unit->runtime_ext->templateMap.count(templateObjID) == 0 &&
      "The template object already exists.");
  unit->runtime_ext->templateMap[templateObjID] =
      vmcast<JSObject>(HermesValue::fromRaw(templateObj.raw));
}
