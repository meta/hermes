# Phase 2 inventory: handle-returning APIs

Read-only survey of the working copy on top of `d5aaad69765b::06559e05c568`
(the Phase 1 stack, `cce78a6e216f::.`). Nothing was built or run. Counts
come from `grep` of the function name; "prod" = `lib/ include/ API/`
(excluding `API/hermes_sandbox/external`), "unit" = `unittests/`.
**Counts are grep hits and include the declaration and definition lines
(normally 2)**, so subtract 2 for the number of real callers. Names that
are overloaded or common (e.g. `allocate`) are approximate.

Legend for the recommendation column:

- **A** = `CallResult<PseudoHandle<T>>` / `PseudoHandle<T>`; callers pin
  with `lv.x = std::move(*res)`.
- **B** = `MutableHandle<T>` out-parameter (callers pass a view of their
  own `PinnedValue`), return `ExecutionStatus` or `CallResult<bool>`.
- **H** = `CallResult<HermesValue>` / `HermesValue`; callers pin with
  `castAndSetHermesValue`. Used where the result may alias a parameter
  or is untyped (value semantics make the aliasing harmless).
- **keep** = not an allocation (aliases an existing root/parameter).
- **del** = delete the API; callers use an existing non-handle API.

## 1. Table

### 1.1 IdentifierTable / symbols / StringView

| # | Declaration | Return type | Rec. | prod / unit | Callers (prod) | Notes |
|---|---|---|---|---|---|---|
| 1 | `include/hermes/VM/IdentifierTable.h:85,87` `getSymbolHandle(Runtime&, UTF16Ref[, hash])`, def `lib/VM/IdentifierTable.cpp:41` | `CallResult<Handle<SymbolID>>` | **del** -> use existing `getSymbolID(...)` (`IdentifierTable.h:101`, returns `CallResult<SymbolID>`); add the no-hash overloads | 32 / 62 | ConsoleHost.cpp(9), hermes_napi_string.cpp(5), hermes_napi_error.cpp(2), hermes_napi_promise.cpp(2), HermesInternal.cpp (via #8), JSRegExp.cpp:222, RuntimeModule.cpp:319, Callable.cpp:765 | Result is a SymbolID for a *transient* char range, so nothing else roots it. Every caller that holds the SymbolID across a safepoint must pin it in a `PinnedValue<SymbolID>`; "use immediately as a property key" is not enough because `defineOwnProperty` etc. allocate before storing it. Unit tests: ObjectModelTest(21), HiddenClassTest(16), NapiExceptionTest(10), JSLibTest(5), IdentifierTableTest(5), others(5). |
| 2 | `IdentifierTable.h:93,95` `getSymbolHandle(..., ASCIIRef[, hash])`, def `IdentifierTable.cpp:52` | same | **del** (same as #1) | (counted in #1) | | |
| 3 | `IdentifierTable.h:134` `getSymbolHandleFromPrimitive(Runtime&, PseudoHandle<StringPrimitive>)`, def `IdentifierTable.cpp:101` | `CallResult<Handle<SymbolID>>` | **B** `ExecutionStatus(..., MutableHandle<SymbolID> out)` | 11 / 3 | hermes_napi_function.cpp(3), hermes_napi_object.cpp(1), hermes_napi_internal.h(1), JSLibInternal.cpp(2), JSObject.cpp:3472, Operations.h:219 | The non-uniqued path pins the string, interns, then returns `makeHandle(*cr)`: the handle is the *only* root of the new symbol (see risky case R1). With B the callee writes straight into the caller's pinned slot, so the rooting is explicit. Napi callers do `**symRes` immediately; they need a `PinnedValue<SymbolID>`. |
| 4 | `include/hermes/VM/Operations.h:215` `stringToSymbolID` (inline wrapper of #3) | `CallResult<Handle<SymbolID>>` | **B** (same shape as #3) | 9 / 5 | hermes.cpp(4), SerializedValue.cpp:1, require.cpp:1, hermes_vtable.cpp:1, Operations.cpp(valueToSymbolID) | Unit: HeapSnapshotTest(4), StackTracesTreeTest(1). Convert together with #3. |
| 5 | `Operations.h:236` `valueToSymbolID`, def `lib/VM/Operations.cpp:43` | `CallResult<Handle<SymbolID>>` | **B** | 9 / 6 | SerializedLiteralOperations.h(2), PrimitiveBox.cpp:1, JSObject.cpp:1, JSArray.cpp:1, Interpreter-slowpaths.cpp:1, JSLib/Array.cpp:1 | Symbol input case returns `Handle<SymbolID>::vmcast(nameValHnd)` (aliases the parameter) -> B must copy the SymbolID into the out slot. `SerializedLiteralOperations.h:117` derefs it right away (`*symHandle`) and flushes a marker, so it needs a Locals slot in the visitor. Unit: JSLibTest(6). |
| 6 | `API/napi/hermes_napi_internal.h:30` `internUtf8AsSymbol(env, utf8, CallResult<Handle<SymbolID>> &symRes)` | out-param `CallResult<Handle<SymbolID>>&` | **B** `MutableHandle<SymbolID>` out | 6 / 0 | hermes_napi_object.cpp(4: lines 166, 202, 239, 784), hermes_napi_function.cpp:672 | Calls #3. Doc says "requires an active GCScope". |
| 7 | `IdentifierTable.h:~140` `getStringView(Runtime&, SymbolID) const`, def `IdentifierTable.cpp:133`; also `getStringViewForDev` (`IdentifierTable.cpp:150`) | `StringView` (embeds `Handle<StringPrimitive>` for the non-lazy case via `makeHandle` at :138) | **B-style**: new overload taking a caller-supplied `MutableHandle<StringPrimitive> storage` (see open question Q2) | 31 + 12 (ForDev) / 5 | JSObject.cpp(6+6), Operations.cpp(5), HiddenClass.cpp(4), GlobalObject.cpp(4), hermes_vtable.cpp(2), hermes.cpp(1+4), Runtime.cpp:2212, JSLibInternal.cpp:160, JSArray.cpp:789, Interpreter-slowpaths.cpp:1665, JSObject.h:1111, Debugger.cpp:48, IdentifierTable.cpp:164 | **High fan-out**. Lazy (bytecode) identifiers need no handle (`StringView(ref)`), only entries that already hold a `StringPrimitive` allocate. `Debugger.cpp:48` `getFunctionName` *returns* the StringView by value (risky R4). Several uses are temporaries inside error-message expressions (`... + getStringView(...) + "'"`), where the caller needs a Locals slot just for the temporary. `createStringView`/`createStringViewMustBeFlat` take a caller handle -> not allocations (keep). |
| 8 | `lib/VM/JSLib/HermesInternal.cpp:31` `symbolForCStr` (static helper over #1) | `CallResult<Handle<SymbolID>>` | **B** or `CallResult<SymbolID>` + caller pin (follows #1) | 3 / 0 | HermesInternal.cpp(3) | Dies with #1. |

### 1.2 Operations.h

| # | Declaration | Return type | Rec. | prod / unit | Callers (prod) | Notes |
|---|---|---|---|---|---|---|
| 9 | `Operations.h:114` `toPropertyKey` (inline; contains 2 `makeHandle`s at :122, :131) | `CallResult<Handle<>>` | **H** (`CallResult<HermesValue>`; result is Symbol or String) | 17 hits, ~13 real (3 JIT `Emitter::toPropertyKey` hits are an unrelated member) / 1 | JSObject.cpp(5), Operations.cpp(2: :2751, :2899 already `.toCallResultHermesValue()`), JSLib/Object.cpp(2), JSLib/Array.cpp(2: sort comparator), Interpreter.cpp:2318 | `Interpreter.cpp:2318` is inside the main loop (ToPropertyKey opcode). `JSObject.cpp` call sites use the key after more allocations -> pin into Locals. Array.cpp sites are in a sort-comparator class with `lv_`. |
| 10 | `Operations.h:140` `toPropertyKeyIfObject` | `CallResult<Handle<>>` | **H** | 10 / 1 | JSObject.cpp(8), JSLib/Object.cpp:584 | Returns the *input handle* when not an object (aliasing) -> H is the clean fix. Calls #9. |
| 11 | `Operations.h:150` `getPrimitivePrototype`, def `Operations.cpp:721` | `CallResult<Handle<JSObject>>` | **keep** | 3 / 2 | Interpreter-slowpaths.cpp:1, OperationsTest(2) | Returns `Handle<JSObject>::vmcast(&runtime.xxxPrototype)`: a pointer to a permanent root, no allocation. Could be simplified to a raw `JSObject*` later; out of scope. |
| 12 | `Operations.h:423` `iteratorStep`, def `Operations.cpp:1612` | `CallResult<Handle<JSObject>>` (null handle = done) | **A** `CallResult<PseudoHandle<JSObject>>` (null PseudoHandle = done) | 11 / 0 | Operations.cpp(iteratorStepValue, iterableToArray), WeakSet.cpp, WeakMap.cpp, TypedArray.cpp:82, Set.cpp:358, HermesBuiltin.cpp:663, Array.cpp:5355, JSLibInternal.h:450 | Body has `makeHandle(std::move(*resultRes))` and passes it to `getNamed_RJS` (:1618) -> callee must pin internally (own Locals) before the `done` lookup. Callers use `*nextRes` as `Handle<JSObject>` for `iteratorValue` -> pin. |
| 13 | `Operations.h:453` `iterableToArray`, def `Operations.cpp:1746` | `CallResult<Handle<JSArray>>` | **A** `CallResult<PseudoHandle<JSArray>>` | 3 / 0 | JSLib/Error.cpp(1) | Array is already pinned in callee; return `std::move`d pseudo. Has `GCScopeMarkerRAII` (leave). |
| 14 | `Operations.h:469` `speciesConstructor`, def `Operations.cpp:1835` | `CallResult<Handle<Callable>>` | **keep** (returns its `defaultConstructor` parameter) or **H** | 3 / 0 | JSTypedArray.cpp:1 (allocateSpecies) | Never allocates. Leave unless it simplifies #58. |
| 15 | `Operations.h:545` `symbolDescriptiveString`, def `Operations.cpp:2097` | `CallResult<Handle<StringPrimitive>>` | **A** | 6 / 0 | Symbol.cpp, String.cpp, Interpreter-slowpaths.cpp, hermes.cpp | Returns `builder->getStringPrimitive()` -> depends on #28 (StringBuilder). Also has `desc = makeHandle(getStringPrimFromSymbolID(*sym))` inside. |
| 16 | `Operations.h:362` `numberToStringWithRadix`, def `Operations.cpp:1369` | `Handle<StringPrimitive>` | **A** `CallResult`-less `PseudoHandle<StringPrimitive>`/H (check body: it uses `ignoreAllocationFailure`) | 3 / 0 | JSLib/Number.cpp:1 | |
| 17 | `Operations.h:465` `createIterResultObject`, def `Operations.cpp:1810` | `Handle<JSObject>` | **H** (`HermesValue`) or A | 15 / 0 | JSArray.cpp(5), JSRegExpStringIterator.cpp(4), PrimitiveBox.cpp(3), JSMapImpl.h:1 | All callers immediately return `.getHermesValue()` -> **H**. Body has `objHandle` that becomes a Locals field. |

### 1.3 JSObject / JSProxy / for-in

| # | Declaration | Return type | Rec. | prod / unit | Callers (prod) | Notes |
|---|---|---|---|---|---|---|
| 18 | `JSObject.h:561` `getOwnPropertyKeys`, def `JSObject.cpp:421` | `CallResult<Handle<JSArray>>` | **A** `CallResult<PseudoHandle<JSArray>>` | 10 / 0 | JSLib/Object.cpp(4), JSProxy.cpp(2: ~:1454 `targetKeysRes`), JSObject.cpp(2), HermesBuiltin.cpp:1, hermes_napi_object.cpp:470 | Body: `array = makeHandle(...)`, `Optional<Handle<JSArray>> hostObjectSymbols` (:458); needs Locals for both. Proxy branch returns `makeHandle(std::move(*proxyRes))` -> just forward the pseudo. |
| 19 | `JSObject.h:573` `getOwnPropertyNames` (inline over #18) | same | **A** | 3 / 1 | RuntimeJSONStringify.cpp:1, RuntimeJSONParse.cpp:1, JSLib/Array.cpp:1; unit InternalPropertiesTest | Also JSI `getOwnPropertyNames` hits in APITest/SynthTraceTest are the *JSI* method, unrelated. |
| 20 | `JSObject.h:587` `getOwnPropertySymbols` | same | **A** | 1 / 1 | JSLib/Object.cpp:1 | |
| 21 | `JSObject.h:1672` `getForInPropertyNames(Runtime&, Handle<JSObject>, uint32_t&, uint32_t&)`, def `JSObject.cpp:3643` | `CallResult<Handle<ArrayStorageSmall>>` | **A** `CallResult<PseudoHandle<ArrayStorageSmall>>` | 6 / 0 | Interpreter-slowpaths.cpp:495 (GetPNameList), StaticH.cpp:2311, API/hermes/hermes.cpp:3031 (JSI getPropertyNames), hermes_vtable.cpp:654 | Callee: cache hit returns `arr` (a `MutableHandle`), uses `clazz = makeHandle(...)` and a `MutableHandle arr(runtime)`: convert in the same commit (Locals). `StaticH.cpp:2311` is in a lambda where only `.getHermesValue()` is used. Both API callers read `arr->at(i)` across allocations (hermes.cpp:3038..) -> they need a PinnedValue. |
| 38 | `include/hermes/VM/JSProxy.h:33` `detail::findTrap`, def `lib/VM/JSProxy.cpp:35` | `CallResult<Handle<Callable>>` (null handle = trap undefined) | **A** `CallResult<PseudoHandle<Callable>>` | 18 / 0 | JSProxy.cpp(16: lines 260, 323, 384, 430, 484, 654, 827, 860, 951, 987, 1086, 1126, 1214, 1247, 1361, ...), JSCallableProxy.cpp:1 | Body has an inner `GCScope` + `makeHandle(handlerPtr)` and `raiseTypeErrorForValue(makeHandle(...))`. Callers check `!*trapRes` (null = no trap). Note JSProxy.cpp is a Phase 3 file; this commit only touches the 16 call sites plus the needed Locals, it does not migrate the rest of the file. |

### 1.4 Errors / strings / builders

| # | Declaration | Return type | Rec. | prod / unit | Callers (prod) | Notes |
|---|---|---|---|---|---|---|
| 22 | `JSError.h:93` `JSError::toString`, def `JSError.cpp:239` | `CallResult<Handle<StringPrimitive>>` | **A** | 5 / 0 | JSError.cpp(2 internal), JSLib/Error.cpp:1, hermes_napi_error.cpp:1 | Returns `msgStr`/`nameStr` `MutableHandle`s (callee-local) and `builder->getStringPrimitive()` (#28). Body allocs: `name`, `msg`, `nameStr`, `msgStr`. |
| 23 | `JSError.h:135` `JSError::getFunctionNameAtIndex`, def `JSError.cpp:560` | `Handle<StringPrimitive>` | **A**/raw `StringPrimitive*` (null = none) | 4 / 0 | JSError.cpp(2), JSCallSite.cpp:1 | Returns `makeNullHandle` when empty. |
| 24 | `JSError.cpp:354` `getCallStackFunctionNames` (static) | `Handle<PropStorage>` | **A** (null on OOM) | 2 / 0 | JSError.cpp | Swallows OOM (`clearThrownValue`) and returns a null handle. Has its own `GCScope`. |
| 25 | `JSError.cpp:59` `getErrorFromStackTarget` (static) | `llvh::Optional<Handle<JSError>>` | **A** `PseudoHandle<JSError>` + bool, or B | 2 / 0 | JSError.cpp(2) | `Handle<JSError>::vmcast(targetHandle)` branch aliases the parameter, `makeHandle` branch allocates. Loop uses `mutHnd = makeMutableHandle` (Locals). |
| 26 | `PrimitiveBox.h:36,41` `JSString::create` x2, def `PrimitiveBox.cpp:40` | `CallResult<Handle<JSString>>` | **A** | 6 / 0 | JSLib/String.cpp(2), SerializedValue.cpp:1, Operations.cpp(toObject):1, GlobalObject.cpp:1 | Body: `selfHandle = initToHandle`, `Handle<>{&lengthValue}` is a stack PinnedHermesValue (leave). The 2-arg overload calls `getPredefinedStringHandle` (#32). |
| 27 | `PrimitiveBox.h:194,199` `JSBigInt::create` x2, def `PrimitiveBox.cpp:298` | `Handle<JSBigInt>` / `CallResult<Handle<JSBigInt>>` | **A** | 3 / 0 | SerializedValue.cpp:1, Operations.cpp(toObject):1 | Inline overload at :208 has a `makeHandle(bigIntRes->getBigInt())`. |
| 28 | `StringBuilder.h:32` `MutableHandle<StringPrimitive> strPrim_` member; `createStringBuilder` (:43), `getStringPrimitive` (:161), private ctor (:170) | `CallResult<StringBuilder>`; `Handle<StringPrimitive>` | see Q1 | 21 / 9 | JSLib/String.cpp(5), TypedArray.cpp(3), StringPrimitive.cpp(2), Base64.cpp(2), Array.cpp(2), StaticH.cpp:1, Operations.cpp:1, Object.cpp:1, JSLibInternal.cpp:1, JSError.cpp:1, HermesBuiltin.cpp:1 | **Class member that allocates a handle in its constructor** (`strPrim_(runtime, strPrim)`), plus `appendUTF16Ref` reassigns it. Not caught by "returns Handle<T>" greps. Unit: StringBuilderTest(5), Base64UtilTest(4). See R5. |
| 29 | `StringPrimitive.h:233,240` `createNoThrow` x2 (defs :934,:944) | `Handle<StringPrimitive>` | **A**/H | 6 / **105** | Interpreter-slowpaths.cpp:1, hermes.cpp:1 | Only 2 production callers, but 105 unit hits (OperationsTest 37, StringPrimitiveTest 16, StringViewTest 11, HandleTest 7, ...). See Q4. |
| 30 | `BigIntPrimitive.h:66` `fromSignedNoThrow`, `:105` `fromBytesNoThrow` | `Handle<BigIntPrimitive>` | **del** from prod headers / move to test helper | 1 / 31 + 21 | none in prod | Zero production callers: used by unittests only. |
| 31 | `StringPrimitive.h:314` `ensureFlat`, `:331` `createStringView`, `:403` `createStringViewMustBeFlat` | `Handle<StringPrimitive>` / `StringView` | **keep** | n/a | | Take a caller handle; no allocation (`ensureFlat` returns its argument). |
| 32 | `Runtime.h:432,433` `getPredefinedStringHandle(Str|Sym)`, inline defs `Runtime.h:2317,2322` | `Handle<StringPrimitive>` | **del** -> callers use `lv.x = runtime.getPredefinedString(...)` | 39 / 0 | JSLib: Intl.cpp(4), Set.cpp(2), Map.cpp(2), WeakSet, WeakRef, WeakMap, TypedArray, Symbol, StringIterator, RuntimeJSONParse, RegExpStringIterator, Reflect, Math, JSON, GlobalObject, GeneratorPrototype, GeneratorFunction, FinalizationRegistry, Error.cpp(macro `NATIVE_ERROR_TYPE`), DataView, CallSite, BigInt, AsyncFunction, ArrayIterator, ArrayBuffer, Array(1) ; Callable.cpp:577, Runtime.cpp:1240, Domain.cpp:159, PrimitiveBox.h:46 | **It allocates**: the definition is `makeHandle(getPredefinedString(predefined))`. There is no permanent `PinnedHermesValue` root for predefined strings (they live in the identifier table and can be lazily materialized/moved), so it is *not* an accessor to a root. Callers are mostly `defineNameLengthAndPrototype`-style args in JSLib creators -> each needs one Locals slot. Error.cpp:157 is inside a macro (`NATIVE_ERROR_TYPE`). |
| 33 | `Runtime.h:531` `getCharacterString`, def `Runtime.cpp:1798` | `Handle<StringPrimitive>` | **H** | 15 / 0 | JSLib/String.cpp(7), PrimitiveBox.cpp(3), StringPrimitive.cpp(2), Interpreter-slowpaths.cpp:715 | Mixed: ch < 256 returns `vmcast(&charStrings_[ch])` (a root), else `makeHandle(StringPrimitive::create)`. All callers already do `.getHermesValue()` / `.get()` -> H. |
| 34 | `Runtime.h:1231` `allocateCharacterString` (private), def `Runtime.cpp:1780` | `Handle<StringPrimitive>` | H | 3 / 0 | Runtime.cpp(init loop) | Init-time only. |

### 1.5 Functions, classes, creators

| # | Declaration | Return type | Rec. | prod / unit | Callers (prod) | Notes |
|---|---|---|---|---|---|---|
| 35 | `Callable.h:777` `NativeFunction::create`, def `Callable.cpp:1171` | `Handle<NativeFunction>` | **A** `PseudoHandle<NativeFunction>` (precedent: `createNapiNativeFunction` out `PseudoHandle<NativeFunction>`) | 21 / 6 | GlobalObject.cpp(4), JSLibInternal.cpp(3, incl. `defineMethod`), Intl.cpp(3), Error.cpp(2), ConsoleHost.cpp(2), TracingRuntime.cpp, Proxy.cpp, IteratorPrototype.cpp, HermesBuiltin.cpp, hermes_napi_promise.cpp, Domain.cpp:146, Callable.cpp | Body: `selfHandle = initToHandle` and later `defineNameLengthAndPrototype`/lazy props with it -> internal Locals. `defineMethod` (JSLibInternal.cpp:~86) still has `GCScope` + this handle. Unit: InterpreterTest(2), StackTracesTreeTest, NativeFunctionTest, NapiValueTest, FinalizationRegistryTest. |
| 36 | `Callable.h:565` `NativeJSFunction::create`, `:579` `createWithInferredParent`, `:656` `NativeJSClass::create`; defs `Callable.cpp:998,1018,1105` | `Handle<...>` | **A** | 3 + 7 + 2 | StaticH.cpp(642, 696, 714), Callable.cpp(2) | `StaticH.cpp:642` `create_generator_object` was explicitly left by Phase 1. Native backend: rule 10 (no `_sh_throw_current` inside the Locals scope). Note `JSFunction::createWithInferredParent` (`Callable.cpp:1420`) is already `PseudoHandle`; the 7 hits include it. |
| 37 | `JSObject.h:1688` `JSObjectInit::initToHandle` | `Handle<JSObjectType>` | **del**: callers switch to existing `initToPointer` + `PinnedValue` | 8 / 0 | Callable.cpp(3), PrimitiveBox.cpp(2), HostModel.cpp:79, Domain.cpp:350 | It is the `makeHandle` inside every creator above. Delete after #26, #27, #35, #36, #40 land. |
| 39 | `Interpreter.h:46` `reifyArgumentsSlowPath`, def `Interpreter.cpp:159` | `CallResult<Handle<Arguments>>` | **A** | 5 / 0 | Interpreter.cpp(3: ReifyArguments opcodes; `resArgs` local at :643 is declared in the main loop), StaticH.cpp:1 | Main-loop locals: `CallResult<Handle<Arguments>> resArgs` becomes `CallResult<PseudoHandle<Arguments>>`; pin into an existing/new interpreter-loop `lv` slot (hot path, see R3). |
| 40 | `Domain.h:261` `RequireContext::create`, def `Domain.cpp:340` | `Handle<RequireContext>` | **A** | 4 / 0 | Domain.cpp:156, Runtime.cpp:1239, JSLib/require.cpp:1 | Calls #32 at two of the call sites. |
| 41 | `RuntimeModule.h:317` `RuntimeModule::getDomain`, inline def `RuntimeModule-inline.h:18` | `Handle<Domain>` | **A**/raw `Domain*` (`getDomainUnsafe` already exists at :22) | 11 / 4 | Interpreter.cpp(2: :133, :1680 CreateClosure), JIT/arm64/JitHandlers.cpp:36, Interpreter-slowpaths.cpp:115, RuntimeModule.cpp:257, Runtime.cpp, JSLib/require.cpp:153,218 | `domain_` is a `WeakRoot<Domain>`: a pointer needs pinning. Interpreter.cpp:1680 and JitHandlers.cpp:36 are hot closure-creation paths that currently `makeHandle` + `flushToSmallCount`. See R3. Unit: JSLibTest(2), ObjectModelTest, InterpreterTest. |
| 42 | `JSDataView.h:41` `JSDataView::getBuffer(Runtime&)` | `Handle<JSArrayBuffer>` (`makeHandle(buffer_)`) | **A**/raw `JSArrayBuffer*` (like `JSTypedArrayBase::getBuffer`, JSTypedArray.h:84, which already returns a raw pointer) | ~4 / 0 | JSLib/DataView.cpp:29, SerializedValue.cpp:1232,1309, hermes_napi_dataview.cpp:98 | napi caller has an explicit comment about the GCScope (:91). `hermes.cpp:3484` is the TypedArray `getBuffer` (raw pointer), unrelated. |
| 43 | `JSArray.h:157` `handleAt` | `Handle<>` | **del** | 1 / 0 | none (dead) | |
| 44 | `JSRegExp.h:106` `getGroupNameMappings`, def `JSRegExp.cpp:242` | `Handle<JSObject>` (null if unset) | **A**/raw `JSObject*` (null = none) | 3 / 0 | JSLib/RegExp.cpp:1 | Already carries a TODO in the body. |
| 45 | `JSRegExp.h:49` `createMatchClass`, def `JSRegExp.cpp:74` | `Handle<HiddenClass>` | **A** | 3 / 0 | GlobalObject.cpp:486 | Chains 3x `addProperty` (#47). |
| 46 | `JSArray.h:327` `JSArray::createClass`, `FastArray.h:171` `FastArray::createClass` | `Handle<HiddenClass>` | **A** | ~4 / 0 | GlobalObject.cpp(470, 475, 482: result assigned to `runtime.xxxClass`), JSArray.h:385, FastArray.cpp:125 | Each calls `getHiddenClassForPrototype` (root view, keep) and `addProperty` (#47) and returns `added->first`. |

### 1.6 HiddenClass (all in `include/hermes/VM/HiddenClass.h`, defs in `lib/VM/HiddenClass.cpp`)

| # | Declaration | Return type | Rec. | prod / unit | Callers (prod) | Notes |
|---|---|---|---|---|---|---|
| 47 | `:513` `addProperty` (def :405) | `CallResult<std::pair<Handle<HiddenClass>, SlotIndex>>` | **B** `CallResult<SlotIndex>(..., MutableHandle<HiddenClass> out)` (or A with `pair<PseudoHandle<HiddenClass>, SlotIndex>`) | 8 / 24 | SerializedLiteralOperations.h(2), JSRegExp.cpp, JSObject.cpp, JSArray.cpp, FastArray.cpp, HiddenClass.cpp(reserveSlot); unit HiddenClassTest(24) | Dictionary-mode branch returns `selfHandle` itself (aliases the parameter); the transition branch returns `makeHandle(existingChild)` (:453). Typical callers do `cls = added->first`, which fits B directly. Two internal `getStringView` calls (#7). Calls `copyToNewDictionary`. See R2. |
| 48 | `:583` `reserveSlot` (def :823) | same | **B** (follows #47) | 3 / 0 | Runtime.cpp(2), HiddenClass.cpp | Thin wrapper over #47. |
| 49 | `:506` `deleteProperty` (def :382) | `Handle<HiddenClass>` | **A** | 3 / 3 | JSObject.cpp(2); unit HiddenClassTest(3) | Returns `selfHandle` (aliasing) or the result of `copyToNewDictionary`. |
| 50 | `:521` `updateProperty` (def :541) | `Handle<HiddenClass>` | **A** | 2 / 2 | JSObject.cpp:1 | Has `makeHandle(existingChild)` at :606. |
| 51 | `:549` `makeAllNonConfigurable` (def :677) | `Handle<HiddenClass>` | **A** | 3 / 0 | JSObject.cpp:1 | Locals for `mapHandle`, `curHandle` (:689-691) in same commit. |
| 52 | `:555` `makeAllReadOnly` (def :716) | `Handle<HiddenClass>` | **A** | 3 / 3 | JSObject.cpp:1 | Same; allocs at :726, :739, :741. |
| 53 | `:571` `updatePropertyFlagsWithoutTransitions` (def :780) | `Handle<HiddenClass>` | **A** | 7 / 5 | Runtime.cpp(2), JSObject.cpp(2); unit HiddenClassTest(3), ObjectModelTest(2) | `classHandle` is a `MutableHandle` (:787). |
| 54 | `:615` `copyToNewDictionary` (def :233) | `Handle<HiddenClass>` | **A** | 6 / 0 | all inside HiddenClass.cpp (5) | Internal only; callees of #47, #49, #50, #53. |

### 1.7 Typed arrays / buffers

| # | Declaration | Return type | Rec. | prod / unit | Callers (prod) | Notes |
|---|---|---|---|---|---|---|
| 55 | `JSTypedArray.h:29` `JSTypedArrayBase::allocate` (non-static, by vtable kind) and `:453` `JSTypedArray<T,C>::allocate`, defs `JSTypedArray.cpp:143,360` | `CallResult<Handle<JSTypedArrayBase>>` | **A** | ~7 / 1 | JSLib/TypedArray.cpp(4), HermesInternal.cpp:1, ConsoleHost.cpp:1; unit JSTypedArrayTest | Dispatched through a function-pointer table typed `CallResult<Handle<JSTypedArrayBase>>(Runtime&, size_type)` (`JSTypedArray.cpp:146`); change the typedef. |
| 56 | `:39` `allocateToSameBuffer` (def :158), `:45`/`:460` `allocateSpecies` (defs :184, :374) | same | **A** | 3 + 7 / 0 | JSLib/TypedArray.cpp(1+2), JSTypedArray.cpp(3) | `allocateSpecies` has `makeHandle(HermesValue::encodeTrustedNumberValue(length))` and `makeHandle<JSObject>(callRes->get())` (:386, :392). Also function-pointer table. Calls #14 and #55. |
| 57 | `JSArrayBuffer.h:52` `clone`, def `JSArrayBuffer.cpp:77` | `CallResult<Handle<JSArrayBuffer>>` | **A** | 2 / 0 | JSTypedArray.cpp:1 | `arr = makeHandle(JSArrayBuffer::create(...))` is the only handle. |

### 1.8 Runtime / ConsoleHost / misc

| # | Declaration | Return type | Rec. | prod / unit | Callers | Notes |
|---|---|---|---|---|---|---|
| 58 | `Runtime.h:347` `runInternalJavaScript`, def `Runtime.cpp:1301` | `Handle<JSObject>` | **A**/H (or B into a Locals in the ctor) | 3 / 0 | Runtime.cpp:493 | Two build modes (`HERMESVM_INTERNAL_JAVASCRIPT_NATIVE`): the native one returns `makeHandle<JSObject>(HermesValue::fromRaw(...))`. |
| 59 | `lib/ConsoleHost/ConsoleHost.cpp:468` lambda `tryGetDefinedProperty` | `llvh::Optional<vm::Handle<>>` (`makeHandle` at :488) | **B** (`PinnedValue<>` out slot + bool) | local, 3 calls | ConsoleHost.cpp | Phase 1 commit `b37367acad36` explicitly kept it. Not an API, just a lambda; trivial. |
| 60 | `include/hermes/ConsoleHost/ConsoleHost.h:106,113` | `makeHandle(runtime.getThrownValue())` in header | n/a | | | Plain allocation sites, *not* blocked by any API; Phase 1 leftover (see section 3). |

### 1.9 Reviewed and **kept** (not allocations)

| Declaration | Why |
|---|---|
| `NativeArgs::getArgHandle/getThisHandle/getNewTargetHandle/dyncastArg/dyncastThis/vmcastThis` (`NativeArgs.h`) | Views into the register stack. |
| `Runtime::getGlobal` (`Runtime.cpp:1444`), `Runtime::getHiddenClassForPrototype` (`Runtime-inline.h:50`, ~40 callers), `HandleRootOwner::getUndefinedValue/getNullValue/getEmptyValue/getBoolValue/getZeroValue/getOneValue/getNegOneValue/makeNullHandle` | `Handle::vmcast` of a permanent `PinnedHermesValue` member of the runtime. |
| `JSTypedArray<T,C>::getPrototype/getConstructor(const Runtime&)` (`JSTypedArray.h:447`) | `const Runtime&`, view of a runtime root. |
| `StackFrame::getCalleeClosureHandleUnsafe`, `getDebugEnvironmentHandle` (`StackFrame.h:154,187`) | View into the call frame. |
| `Callable.cpp:118` `inferredParent` (static) | Returns `runtime.xxxFunctionPrototype` root view. |
| `SerializedValue.cpp:413` `getErrorPrototype`, `DebuggerInternal.cpp:17`, `defineSystemConstructor` (`JSLibInternal.cpp:22,98`; already takes a `MutableHandle` out slot and returns it) | Root views / out-param already. |
| `JSONLexer.h:73,78` `getStrAsPrim/getStrAsSymbol` | Return the lexer's own `lv_` pinned fields. |
| `JSLib/TypedArray.cpp:2455` `validateUint8Array`, `JSLib/GeneratorPrototype.cpp:70` `generatorValidate` | `CallResult<Handle<T>>` that just `vmcast`s the `Handle<>` parameter; no allocation. Could be simplified to a bool/`ExecutionStatus`, optional follow-up. |
| `API/hermes_abi/hermes_vtable.cpp:146-171` `toHandle(...)`, `:308` `makeVMHandle`; `API/hermes/hermes.cpp:652-667,703` `stringHandle/handle/arrayBufferHandle/typedArrayHandle/vmHandleFromValue` | Views into `ManagedValue`/`HermesPointerValue` PinnedHermesValue storage (the API's own lifetime mechanism) or a caller-supplied `numberStorage`. |
| `ComputedPropertyDescWithSymStorage` (`PropertyDescriptor.h:214`), `IteratorRecord` (`Operations.h:372`), `Handle<> clazz` members in lambdas | Store handles passed *in* by the caller. |
| `StringPrimitive::createStringView(runtime, Handle)`, `createStringViewMustBeFlat` | StringView copies the caller's handle. |

Total: 59 numbered rows (#60 is a note on plain allocation sites). Of
these 3 are "keep" (#11, #14, #31), leaving 56 rows that change, which
cover roughly 70 distinct declarations (several rows group overloads or
a family). Section 1.9 lists further handle-returning functions that were
reviewed and kept.

## 2. Dependency order and proposed commits

Edges (callee -> must change before caller). Only edges that matter:

- `getSymbolID` (exists) <- #1/#2 callers; `getSymbolHandleFromPrimitive`
  (#3) <- #4 `stringToSymbolID` <- #5 `valueToSymbolID`; #6
  `internUtf8AsSymbol` <- #3.
- `StringBuilder` (#28) <- #15 `symbolDescriptiveString`, #22
  `JSError::toString`.
- #9 `toPropertyKey` <- #10 `toPropertyKeyIfObject`.
- #12 `iteratorStep` <- #13 `iterableToArray`.
- #7 `getStringView` <- #47 `addProperty` (and JSObject.cpp sites).
- #54 `copyToNewDictionary` <- #47, #49, #50, #53; #47 <- #48 `reserveSlot`
  <- #45, #46 (class builders).
- #32 `getPredefinedStringHandle` <- #26 (2-arg `JSString::create`), #40.
- #14 `speciesConstructor` (keep) and #55 `allocate` <- #56
  `allocateSpecies` / `allocateToSameBuffer`.
- Creators #26, #27, #35, #36, #40 <- #37 `initToHandle` (delete last).
- #41 `getDomain` <- callers of `JSFunction::createWithInferredParent`.

Everything else is a leaf. Convention for high-fan-out APIs: the task doc
says "declaration, definition and all call sites in one commit". Where an
API has more than ~30 hits I propose a **temporary coexisting overload**
(marked "T") so the migration can be done per directory and the old API
deleted in the last commit. That deviates from the rule; see Q3.

Proposed commits (24). Files are production files only; unit tests listed
where they must be touched.

| C# | Content | Files touched | Hits | Notes |
|---|---|---|---|---|
| C1 | #9 `toPropertyKey` + #10 `toPropertyKeyIfObject` -> H | Operations.h, JSObject.cpp, Operations.cpp, JSLib/Object.cpp, JSLib/Array.cpp, Interpreter.cpp (main loop), OperationsTest? (no), (unit: 1 hit) | ~23 | Leaf, no semantic risk (H). Good first commit. |
| C2 | #12 `iteratorStep` + #13 `iterableToArray` -> A | Operations.h/.cpp, JSLib/{WeakSet,WeakMap,Set,TypedArray,HermesBuiltin,Array,Error,JSLibInternal.h} | ~14 | Leaf. |
| C3 | #1/#2/#8 `getSymbolHandle(str)` -> `getSymbolID` + pin (T: step a lib/VM, b ConsoleHost+napi, c unittests + delete) | IdentifierTable.h/.cpp, JSRegExp.cpp, RuntimeModule.cpp, Callable.cpp, HermesInternal.cpp, ConsoleHost.cpp(9), napi string/error/promise; unit ObjectModelTest, HiddenClassTest, NapiExceptionTest, JSLibTest, IdentifierTableTest, FinalizationRegistryTest, NapiModuleLoadTest, JSONParseTest, DiscoverBBTest | 32 / 62 | **High fan-out in unit tests** (62). Production part is ~25 sites. |
| C4 | #3 + #4 + #6 (`getSymbolHandleFromPrimitive`, `stringToSymbolID`, `internUtf8AsSymbol`) -> B | IdentifierTable.h/.cpp, Operations.h, hermes.cpp(4), SerializedValue.cpp, require.cpp, hermes_vtable.cpp, JSLibInternal.cpp, JSObject.cpp:3472, napi function/object/internal.h; unit HeapSnapshotTest, StackTracesTreeTest, IdentifierTableTest, ObjectModelTest | ~26 / 11 | Risky R1. |
| C5 | #5 `valueToSymbolID` -> B | Operations.h/.cpp, SerializedLiteralOperations.h, PrimitiveBox.cpp, JSObject.cpp, JSArray.cpp, Interpreter-slowpaths.cpp, JSLib/Array.cpp; unit JSLibTest | 9 / 6 | Depends on C4. |
| C6 | #32 `getPredefinedStringHandle` removal, split: C6a JSLib A-M, C6b JSLib N-Z + lib/VM (Callable, Runtime, Domain, PrimitiveBox.h), then delete decl | ~30 files | 39 | **Flag: >30 call sites.** Mechanical; split by directory. |
| C7 | #28 `StringBuilder` (design per Q1) | StringBuilder.h + 11 callers; unit StringBuilderTest, Base64UtilTest | 21 / 9 | Medium; touches `JSLib/String.cpp`(5), `TypedArray.cpp`(3). |
| C8 | #15 `symbolDescriptiveString` -> A | Operations.h/.cpp, Symbol.cpp, String.cpp, Interpreter-slowpaths.cpp, hermes.cpp | 6 | After C7. |
| C9 | #16 `numberToStringWithRadix`, #17 `createIterResultObject` | Operations.h/.cpp, Number.cpp, JSArray.cpp, JSRegExpStringIterator.cpp, PrimitiveBox.cpp, JSMapImpl.h | 18 | Two sub-commits OK. |
| C10 | #22-#25 JSError family (`toString`, `getFunctionNameAtIndex`, statics) | JSError.h/.cpp, JSCallSite.cpp, JSLib/Error.cpp, hermes_napi_error.cpp | ~13 | After C7. |
| C11 | #18-#20 `getOwnPropertyKeys/Names/Symbols` -> A | JSObject.h/.cpp, JSLib/Object.cpp, JSProxy.cpp(2), HermesBuiltin.cpp, RuntimeJSONStringify/Parse.cpp, JSLib/Array.cpp, hermes_napi_object.cpp; unit InternalPropertiesTest | 16 / 5 | |
| C12 | #21 `getForInPropertyNames` -> A | JSObject.h/.cpp, Interpreter-slowpaths.cpp, StaticH.cpp, API/hermes/hermes.cpp, hermes_abi/hermes_vtable.cpp | 6 | Native backend rule 10 at StaticH.cpp:2311. |
| C13 | #7 `getStringView(ForDev)` -> overload with caller storage (T; 3 sub-commits: JSObject/Operations/HiddenClass/JSArray/JSObject.h; GlobalObject/JSLibInternal/Interpreter-slowpaths/Runtime/Debugger; API/hermes + hermes_abi + delete old) | IdentifierTable.h/.cpp + ~14 files; unit IdentifierTableTest(5) | 43 / 5 | **Flag: >30 call sites.** See Q2, R4. |
| C14 | #54 `copyToNewDictionary` + #49 `deleteProperty` + #50 `updateProperty` + #51/#52 `makeAll*` + #53 `updatePropertyFlagsWithoutTransitions` -> A | HiddenClass.h/.cpp, JSObject.cpp, Runtime.cpp; unit HiddenClassTest, ObjectModelTest | ~25 / 14 | |
| C15 | #47 `addProperty` + #48 `reserveSlot` -> B; #45, #46 class builders | HiddenClass.h/.cpp, JSObject.cpp, JSArray.h/.cpp, FastArray.h/.cpp, JSRegExp.h/.cpp, SerializedLiteralOperations.h, Runtime.cpp, GlobalObject.cpp; unit HiddenClassTest(24) | ~22 / 24 | After C13 and C14. See R2. |
| C16 | #38 `findTrap` -> A | JSProxy.h/.cpp (16 sites), JSCallableProxy.cpp | 18 | Touches Phase 3 file; keep change to the call sites. |
| C17 | #55-#57 typed array allocate family + `JSArrayBuffer::clone` | JSTypedArray.h/.cpp, JSArrayBuffer.h/.cpp, JSLib/TypedArray.cpp, HermesInternal.cpp, ConsoleHost.cpp; unit JSTypedArrayTest | ~19 / 1 | |
| C18 | #26 `JSString::create`, #27 `JSBigInt::create` -> A | PrimitiveBox.h/.cpp, JSLib/String.cpp, SerializedValue.cpp, Operations.cpp(toObject), GlobalObject.cpp | 9 | After C6 (empty-string handle) and before C20 (initToHandle). |
| C19 | #35 `NativeFunction::create` -> A | Callable.h/.cpp + 11 JSLib/API files; unit 5 files | 21 / 6 | Medium fan-out; may split JSLib vs rest. |
| C20 | #36 `NativeJSFunction::create/createWithInferredParent`, `NativeJSClass::create`; #40 `RequireContext::create`; #37 delete `initToHandle` (also HostModel.cpp) | Callable.h/.cpp, StaticH.cpp, Domain.h/.cpp, Runtime.cpp, require.cpp, HostModel.cpp, JSObject.h | ~16 | Rule 10 in StaticH. |
| C21 | #41 `RuntimeModule::getDomain` | RuntimeModule.h/-inline.h/.cpp, Interpreter.cpp, JitHandlers.cpp, Interpreter-slowpaths.cpp, Runtime.cpp, require.cpp; unit 4 | 11 / 4 | R3 (hot paths). |
| C22 | #39 `reifyArgumentsSlowPath` | Interpreter.h/.cpp, StaticH.cpp | 5 | R3. |
| C23 | Small APIs: #42 `JSDataView::getBuffer`, #43 `handleAt` (delete), #44 `getGroupNameMappings`, #33/#34 `getCharacterString`/`allocateCharacterString`, #58 `runInternalJavaScript`, #59 ConsoleHost lambda | JSDataView.h, JSArray.h, JSRegExp.h/.cpp, RegExp.cpp, Runtime.h/.cpp, String.cpp, PrimitiveBox.cpp, StringPrimitive.cpp, Interpreter-slowpaths.cpp, DataView.cpp, SerializedValue.cpp, hermes_napi_dataview.cpp, ConsoleHost.cpp | ~45 | Split in 2-3 commits. |
| C24 | #29 `createNoThrow` x2, #30 `fromSignedNoThrow/fromBytesNoThrow` | StringPrimitive.h, BigIntPrimitive.h, Interpreter-slowpaths.cpp, hermes.cpp + 120+ unit sites | 8 / ~160 | See Q4. Do last. |

High fan-out (>30 hits): #1/#2 `getSymbolHandle` (32 / 62 unit), #7
`getStringView` (31 + 12), #32 `getPredefinedStringHandle` (39),
#29 `createNoThrow` (6 / 105 unit), #30 (1 / 52 unit). The first three
need splitting (C3, C13, C6). C24 is only about tests.

## 3. Phase 1 leftovers blocked on Phase 2

From the commit messages of the Phase 1 stack:

| Phase 1 commit | What was left | Unblocked by |
|---|---|---|
| `d5aaad69765b` JSError | `toString`, `getCallStackFunctionNames`, `getFunctionNameAtIndex`, `getErrorFromStackTarget` (7 allocs remain in JSError.cpp: :75, :252, :256, :276, :280, :363, :366) | C10 (needs C7 for the `StringBuilder` part of `toString`) |
| `f639b8a61ad6` JSCallableProxy | `findTrap` call in `_proxyNativeCall` | C16 |
| `901e6f22ebb5` JSTypedArray | `allocate`/`allocateSpecies` (JSTypedArray.cpp:363, :386) | C17 |
| `168d294c4059` HiddenClass | `copyToNewDictionary`, `deleteProperty`, `updateProperty`, `makeAll*`, `updatePropertyFlagsWithoutTransitions`, `addProperty`, `reserveSlot` (8 allocs: :453, :606, :689, :691, :726, :739, :741, :787) | C14, C15 |
| `c491f043e8e4` Callable | `getPredefinedStringHandle` (:577), `getSymbolHandle` (:765), `getHiddenClassForPrototype` (keep; not an allocation, so that part of the leftover can be ignored), `initToHandle` in the 3 creators | C6, C3, C20 |
| `246948f36a37` IdentifierTable | `getSymbolHandle*`, `getSymbolHandleFromPrimitive`, `getStringView` (:49, :60, :108, :110, :119, :138) | C3, C4, C13 |
| `5b475edc6afe` JSRegExp | `getGroupNameMappings` (:246), `getSymbolHandle` (:222), `createMatchClass` | C23, C3, C15 |
| `8e86385a0d69` PrimitiveBox | (none listed; `JSString/JSBigInt::create` still `initToHandle`) | C18 |
| `af94587dc6e2` Domain | `NativeFunction::create` (:146), `RequireContext::create` (:156, :340, `getPredefinedStringHandle` :159) | C19, C20, C6 |
| `95d951a25d9b` Runtime | `getCharacterString`, `runInternalJavaScript`, `getPredefinedStringHandle` (:1240) + `RequireContext::create` caller | C23, C6, C20 |
| `fed02b1fd1d0` StaticH | `NativeJSFunction::create` in `create_generator_object` (:642); `getForInPropertyNames` call; `reifyArgumentsSlowPath` | C20, C12, C22 |
| `c1bf46107bd1` Interpreter | `reifyArgumentsSlowPath` (:170) | C22 |
| `ee41a4a197ed` Interpreter-slowpaths | `symbolDescriptiveString`, `StringPrimitive::createNoThrow` | C8, C24 |
| `b37367acad36` ConsoleHost.cpp | `tryGetDefinedProperty` returns `Optional<Handle<>>` (:488); `getSymbolHandle` x9 | C23, C3 |
| `39649fb0b373` napi_function | symbol lookups returning `Handle<SymbolID>` (:215, :606, :672, :686) | C4 |
| `06559e05c568` napi_object | `getOwnPropertyKeys` (:470), `internUtf8AsSymbol` (:166, :202, :239, :784), `getSymbolHandleFromPrimitive` (:799) | C11, C4 |
| (napi_string, napi_error, napi_promise; no Phase 1 commit) | `getSymbolHandle` x9 | C3 |

Allocation sites still present but **not blocked on an API change** (so
they are plain Phase 1 leftovers, flagged here so they are not lost):
`include/hermes/ConsoleHost/ConsoleHost.h:106,113`, `JSObject.h:1743,
1884, 1914-1917, 1974-1976, 2018`, `Callable.h:1019, 1162`,
`HiddenClass.h:685, 697`, `JSArray.h:414`, `ArrayLike.h:29`,
`JSMapImpl.h:206`, `StaticH.cpp:789`. `Operations.h:122,131` and
`PrimitiveBox.h:208` are inside APIs of this phase (C1, C18).

## 4. Risky cases

**R1. `Handle<SymbolID>` whose callers extract the bare `SymbolID`
(#1-#6).** `**symRes` appears in napi (function/object/string) and
`SerializedLiteralOperations.h`. For a symbol interned from a transient
string, the `makeHandle(*cr)` inside `getSymbolHandle*` is the *only*
root; after conversion nothing roots it, and `freeUnmarkedSymbols` can
reclaim the slot on sweep (the `gc-safe-coding` skill calls this out).
Recommendation: never return a bare `SymbolID` from the converted API
without an obligation to pin. Use out-param **B** with a
`PinnedValue<SymbolID>` slot (callee writes into the caller's slot, so
there is no unrooted window) for #3-#6, and for #1/#2 use the existing
`getSymbolID` and require `lv.sym = *res` in the caller whenever the id
lives past the next safepoint (in practice always).
`valueToSymbolID` has an aliasing branch (symbol input) that must copy.

**R2. `HiddenClass::addProperty` returning `pair<Handle<HiddenClass>,
SlotIndex>` (#47).** Dictionary mode returns `selfHandle` (aliases
the parameter and is mutated in place), class mode returns a handle to an
existing/new child class. A `PseudoHandle` pair is safe but all 24 unit
call sites and 8 prod sites use `added->first`; B (out
`MutableHandle<HiddenClass>` + `CallResult<SlotIndex>`) matches the usual
`classHandle = added->first;` idiom and `reserveSlot` simply forwards.
Careful: when `out` aliases `selfHandle` (typical in-place update
`addProperty(cls, ..., cls)`), the callee must read everything it needs
from `selfHandle` before writing `out`. Recommend B and an assert for the
aliasing case.

**R3. Interpreter / JIT hot paths (#39, #41, #9 at `Interpreter.cpp:2318`).**
`CreateClosure` does `makeHandle(domain)` per closure then
`gcScope.flushToSmallCount`; `resArgs` is a main-loop local; the JIT
handler (`JitHandlers.cpp:36`) does the same. Replacing these needs a
pinned slot. Do **not** add a new `Locals` per call in the hot loop;
reuse the loop's existing `lv.tmp`-style slot, and measure (the Phase 1
commits say "no Locals was added to a hot path"). `getDomainUnsafe` +
immediate use is *not* safe because `createWithInferredParent` allocates
the closure before reading the domain handle... verify per call site.

**R4. `StringView` embedding a `Handle` (#7).** `getStringView` for a
materialized identifier does `makeHandle` and stores it inside the
returned view; the view is often a temporary in an error-message
expression, is returned by value from `Debugger.cpp:getFunctionName`
(:48), and is stored by `StringView::isHandle_`. Converting needs the
caller to supply the storage slot, so every use site needs a Locals field
even when the view is a temporary. Alternatives (e.g. a raw pointer in
the view) are unsafe across GC, since strings in the identifier table can
move. Recommendation: add `getStringView(runtime, id, MutableHandle<StringPrimitive>
storage)`; for `Debugger.cpp:48`, change `getFunctionName` to take the
storage too or return `std::string`. Lazy identifiers keep working
without storage.

**R5. `StringBuilder` allocates a handle in its (private) constructor and
keeps it as a member (#28).** Not visible to "returns Handle" greps. It
is returned by value inside `CallResult<StringBuilder>`; `getStringPrimitive()`
returns a `Handle` pointing at the builder's own slot, so
`symbolDescriptiveString` and `JSError::toString` return handles whose
lifetime is the builder's `GCScope` slot. Recommendation: see Q1.

**R6. `getPredefinedStringHandle` (#32) is *not* a root accessor.** It is
`makeHandle(getPredefinedString(...))`. Predefined strings have no
`PinnedHermesValue` root. Treat it as an allocation and delete it.
(`getCharacterString` is the mixed case: root for `ch < 256`, allocation
otherwise; H handles both.)

**R7. API surfaces (JSI / napi / hermes_abi).** These use `GCScope` as
their value-lifetime mechanism (Phase 4), but call APIs from this phase:
`hermes.cpp` (`getForInPropertyNames`, `stringToSymbolID` x4, `getStringView`,
`symbolDescriptiveString`, `StringPrimitive::createNoThrow`, `getStringViewForDev` x4),
`hermes_vtable.cpp` (`getForInPropertyNames`, `stringToSymbolID`,
`getStringView` with `*toHandle(name)` x2). Their results are consumed
within the same function, so the conversion is local: pin into a Locals
field or use an out-param. Do not change JSI/ABI signatures. Napi code
holds `napi_value` (pinned storage in the env's scope stack) and is fine,
but napi helpers `internUtf8AsSymbol` and `createNapiNativeFunction`
document "requires an active GCScope"; update those comments.

**R8. Native backend rule 10.** `StaticH.cpp` call sites of
`getForInPropertyNames` (:2311), `reifyArgumentsSlowPath`,
`NativeJSFunction::create` (:642, :696, :714) sit in the lambda +
`_sh_throw_current` structure. The new `Locals` must be scoped inside the
lambda, as the Phase 1 StaticH commits already do.

**R9. Result aliasing a parameter.** `toPropertyKeyIfObject`,
`valueToSymbolID` (symbol), `speciesConstructor`, `generatorValidate`,
`validateUint8Array`, `deleteProperty`, `addProperty` (dictionary mode)
return a handle that *is* the caller's input. With `PseudoHandle`/H the
aliasing is harmless as long as the caller pins the result before the
next safepoint; with B the callee must tolerate `out` aliasing an input.

## 5. Open questions

- **Q1 (StringBuilder).** Options: (a) `createStringBuilder(runtime,
  length, MutableHandle<StringPrimitive> storage, isASCII)`; the builder
  keeps a view of the caller's `PinnedValue`, and `getStringPrimitive()`
  returns a `Handle` to that slot (callers already pin or return it);
  (b) make the builder a stack object holding a `PinnedValue` and install
  its own `LocalsRAII` (non-movable, so no `CallResult<StringBuilder>`).
  (a) is the smaller change (21 call sites + 9 unit sites). OK to go
  with (a)?
- **Q2 (getStringView).** Is it acceptable to add a storage parameter
  (43 sites, many in error paths), or do you prefer removing
  `StringView`'s `Handle` altogether (e.g. a dedicated `IdentifierStringView`
  that is only used inside one non-allocating expression)? The latter is
  a larger redesign.
- **Q3 (rule bending).** For >30-hit APIs (#1/#2, #7, #32) may I use a
  temporary coexisting overload so the work can be split per directory,
  with the old API deleted in the last commit? The task doc requires one
  commit per API.
- **Q4 (unit tests).** `getSymbolHandle` (62), `createNoThrow` (105),
  `fromSignedNoThrow`/`fromBytesNoThrow` (52) are heavily used by
  unittests. "Update minimally" would still mean ~220 mechanical edits.
  Alternative: keep Handle-returning *test-only* helpers in
  `VMRuntimeTestHelpers.h` (tests run under a `GCScope` anyway) and
  remove the production APIs. Which?
- **Q5 (HiddenClass::addProperty).** B (out-param `MutableHandle`) vs A
  (`pair<PseudoHandle, SlotIndex>`)? B is my recommendation (R2).
- **Q6 (return type for untyped/aliasing results).** OK to use
  `CallResult<HermesValue>` (H) rather than `CallResult<PseudoHandle<>>`
  for `toPropertyKey*` and `createIterResultObject`/`getCharacterString`?
  The task doc lists H only for GlobalObject creators.
