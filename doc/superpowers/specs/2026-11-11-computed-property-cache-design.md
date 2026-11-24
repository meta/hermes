# VM: string-keyed computed property access

dz 01a1cdf5-7332.

This describes the code on branch `x86-jit` as of 2026-11-19. The
design was first written, reviewed and built as one table that served
gets, puts and `hasOwnProperty`; "History" at the end says what changed
and why.

## Problem

`obj[key]` with a string key had no fast path. `GetByVal` and
`PutByVal*` carry no cache operand, so every such get, put and
`hasOwnProperty.call(obj, key)` ran the generic computed-property path:
convert the key to a SymbolID, look the name up in the hidden class,
walk the prototype chain, and for a store that adds a property, look up
the class transition.

On the React benchmarks (`~/work/react-benchmarks`) this is the prop-copy
loop of `jsxProd`:

```js
for (propName in config) {
  if (hasOwnProperty.call(config, propName) &&
      propName !== 'key' && propName !== 'ref')
    props[propName] = config[propName];
}
```

Share of run time before the change (perf, x86-64 JIT, HV64, x86-jit at
eeacdf640):

| benchmark        | put  | hasOwnProperty | get  | total |
|------------------|------|----------------|------|-------|
| pixel-grid-small | 9.9% | 5.3%           | 4.2% | 19.4% |
| draw             | 2.4% | 1.8%           | 3.5% | 7.7%  |
| relay-feed       | 1.4% | 0.1%           | 1.6% | ~3%   |

## What was built

Both parts apply when the key is a uniqued string, which already holds
its SymbolID. A key from `for-in` or `Object.keys` is one.

- Reads. A get, `Object.prototype.hasOwnProperty` and `Object.hasOwn`
  look the name up in the map of the object's own hidden class before
  anything else. No new data structure.
- Puts. A runtime-wide table keyed on (hidden class, property name)
  answers a put with one probe: store to this slot, or add the property,
  which gives this class and this slot.

The fast paths are C++ and sit in the functions every engine already
calls, so the interpreter, the JIT and natively compiled code all use
them. No bytecode, JIT emitter or compiler change.

The code is four commits: a fix to `JSObject::addNewOwnPropertyInSlot`
that the put path depends on ("VM: store in place when an added
property's slot exists"), the reads ("VM: look up a uniqued string key
in the class before the slow path"), the table ("VM: add
ComputedPropertyCache, a table keyed on (class, name)") and its use
("VM: cache string-keyed computed puts"). A fifth commit, "VM: skip
marked prototypes when caching an add", makes the prototype marking
that the add fill and the PutById add cache share skip the objects
already marked.

## Not covered

- Probing the table inline in JIT code, and a JIT fast path for the
  native call to `hasOwnProperty`.
- A get of a property found on a prototype, of an accessor, or of an
  absent property. They take the generic path after the lookup.
- Puts to dictionary-mode objects, objects with indexed storage (arrays,
  typed arrays, `arguments`, String objects), proxies, host objects,
  lazy objects and objects that have an index-like named property; puts
  that reach a setter or a read-only property.
- Keys that are not uniqued strings: numbers, Symbols, objects, and
  strings built at run time, which a property access does not unique
  (`IdentifierTable.cpp:340-355`).
- `in`, `delete`, `Object.defineProperty`, `Reflect.*` with a receiver
  other than the target, `Object.assign`, spread.
- `GetNextPName` and the young-generation size (dz 01a1cc16).
- A switch to turn any of it off.

## Background

All line numbers in this section are at fc6322e0e, the commit before
the change.

### Every engine reaches the same two functions

- Interpreter `GetByVal`: `Interpreter.cpp:2147-2180` tries the numeric
  fast path and then calls `caseGetByVal`, which calls
  `JSObject::getComputed_RJS` (`Interpreter-slowpaths.cpp:734-757`).
  `PutByValLoose`/`PutByValStrict` call `casePutByVal`, which calls
  `JSObject::putComputed_RJS` (`Interpreter-slowpaths.cpp:800-830`).
- JIT and natively compiled code: `_jit_get_by_val` forwards to
  `_sh_ljs_get_by_val_rjs` (`JitHandlers.cpp:511-523`), which is
  `_sh_ljs_get_by_val_with_receiver_rjs`; after its numeric fast path it
  calls `JSObject::getComputedWithReceiver_RJS` (`StaticH.cpp:215-261`).
  The put helpers reach `putByValWithReceiver_RJS`, which calls
  `JSObject::putComputedWithReceiver_RJS` (`StaticH.cpp:961-1025`).
- `getComputed_RJS` and `putComputed_RJS` are inline wrappers that pass
  the object as its own receiver (`JSObject.h:2168-2174, 2197-2205`).

So `JSObject::getComputedWithReceiver_RJS` (`JSObject.cpp:1279`) and
`JSObject::putComputedWithReceiver_RJS` (`JSObject.cpp:1691`) are the two
places. `hasOwnProperty` and `Object.hasOwn` are
`objectPrototypeHasOwnProperty` and `objectHasOwn`
(`JSLib/Object.cpp:1781-1825`); both end in `objectHasOwnHelper`
(`:1753-1778`).

### The generic paths, for a string key

Get (`JSObject.cpp:1279-1325`): the numeric fast path at `:1286-1294`
needs a number key (`toArrayIndexFastPath`, `Operations.h:283-292`).
`toPropertyKeyIfObject` leaves a string alone (`Operations.h:140-147`).
`getComputedPrimitiveDescriptor` (`:1107-1158`) walks the chain; at each
object `getOwnComputedPrimitiveDescriptorImpl` (`:866-960`) converts the
key to a SymbolID (`:910`), looks for a named property (`:913-919`), and
only if that fails consults indexed storage, lazy initialization and the
proxy (`:921-960`). A data property found is read from its named slot
(`:1320-1325`, `JSObject.h:2043-2057`).

Put (`JSObject.cpp:1691-1986`): the same descriptor walk (`:1740`). An
own, writable, non-accessor property without an internal setter, with
the receiver equal to the object, is stored in place (`:1755-1768`).
Otherwise, if nothing on the chain blocks the store (accessor `:1771`,
proxy `:1797`, read-only `:1814`, internal setter `:1828`), the receiver
is the object (`:1845`), it is extensible (`:1928`) and the name is not
an index into indexed storage (`:1938-1973`), `addOwnProperty` adds the
property with the default flags (`:1975-1985`).

`addOwnProperty` (`:2978-3030`) calls `addOwnPropertyImpl`
(`:3116-3168`): `HiddenClass::addProperty` returns the child class and
the slot, the object's class is updated, and the value is stored. For a
class that is not a dictionary, `addProperty` returns the existing
child from the transition table if there is one, and the slot is the
parent's property count (`HiddenClass.cpp:485-497, 588`). A class with
`kDictionaryThreshold` (64) properties gets a dictionary child instead
(`:499-522`).

Has: `toPropertyKey` returns a string unchanged (`Operations.h:114-132`)
and `objectHasOwnHelper` calls `getOwnComputedDescriptor`, which is the
own-descriptor lookup above (`JSObject.cpp:982-993`).

### Uniqued strings

A StringPrimitive that is the identifier table's string for a SymbolID
has the uniqued bit and holds the ID (`StringPrimitive.h:260-262,
443-452`). The generic key conversion uses exactly that: `valueToSymbolID`
(`Operations.cpp:43-56`) ends in
`IdentifierTable::getSymbolHandleFromPrimitive`, which returns
`getUniqueID()` for a uniqued string (`IdentifierTable.cpp:101-109`).
The reference from the string to the symbol is weak: when the symbol is
freed, the bit is cleared (`IdentifierTable.cpp:446-463`,
`StringPrimitive.h:428-432`).

`for-in` produces its keys from SymbolIDs, so they are uniqued. On
pixel-grid-small 1177 of 5.2 million gets had a key that was not
(dz 01a1cdf5-7332, "Table size").

### Hidden classes

- A class that is not a dictionary is immutable: adding, updating and
  deleting a property, and changing a class flag, each produce another
  class (`HiddenClass.h:144-181`; `HiddenClass.cpp:425-446` delete,
  `448-589` add, `591-699` update, `734-807` class flags, `816-` object
  parent). The exceptions are typed classes, which append properties
  in place while they are being set up and never afterwards
  (`HiddenClass.h:199-210`, `HiddenClass.cpp:701-731`), and the mutable
  fields that describe no property (`propertyMap_`, `forInCache_`,
  `lazyJITId_`).
- The property map. A class finds a name through `propertyMap_`, a hash
  table from SymbolID to descriptor. A dictionary class always has one.
  Any other class builds it on the first lookup that needs it, and
  loses it again when a property is added: the child class takes the
  map over (`HiddenClass.cpp:566-568`). MallocGC also drops the maps of
  classes it did not mark (`MallocGC.cpp:308-313`).
  `HiddenClass::tryFindPropertyFast` looks a name up without building
  anything: it answers found or not found if the class has a map or has
  no properties, and "cannot tell" otherwise.
- The class flags record dictionary mode, index-like named properties,
  non-extensible, host object, lazy object, proxy and indexed storage
  (`HiddenClass.h:31-142`). `hasIndexLikeProperties` is set by every add
  of an index-like name (`HiddenClass.cpp:456-460, 504-508, 524-527,
  722-725`) and never cleared.
- The object's prototype is a field of its class
  (`HiddenClass.h:368-369`, `JSObject.h:485-490`). Only a dictionary
  updates it in place (`HiddenClass.h:1015-1017`).
- A class keeps the names of its properties alive: `symbolID_` and
  `parent_` are marked (`HiddenClass.cpp:126-134`), and a typed or
  dictionary class has a property map that marks its names
  (`DictPropertyMap.cpp:35-42`).
- Classes are allocated long-lived (`HiddenClass.cpp:247`).

### The PutById add cache and the parent epoch

`AddPropertyCacheEntry` (`PropertyCache.h:139-178`) caches a property
add at a PutById site: start class, result class, slot, and the parent
cache epoch at the time. A hit requires the start class, a live result
class and an equal epoch, and calls `JSObject::addNewOwnPropertyInSlot`
(`Interpreter.cpp:2082-2099`, `JitHandlers.cpp:390-403`), which updates
the class and stores the value, allocating property storage itself when
it must (`JSObject.cpp:3170-3228`).

`JSObject::tryCacheAddProperty` (`JSObject.cpp:3032-3114`) fills it. It
refuses if any object on the prototype chain is a proxy, a host object
or a dictionary (`:3064-3071`), records the epoch (`:3073`), and marks
every object on the chain with `setCachedUsingEpoch` (`:3109-3113`). A
marked object bumps `Runtime::parentCacheEpoch_` whenever its class or
its prototype changes (`JSObject.h:1447-1458`, `JSObject.cpp:309-315`).
The full epoch is 64-bit and never wraps (`Runtime.h:701-705,
1483-1494`).

So an unchanged epoch means no object on the chain has gained, lost or
changed a property, or changed its prototype, since the entry was
filled.

### Weak roots

`Runtime::markWeakRoots` (`Runtime.cpp:774-802`) marks the class
pointers of the existing caches only when `markLongLived` is set
(`:780-790`, `RuntimeModule.cpp:380-407`). The acceptor nulls a root
whose class is dead (`HadesGC.cpp:960-972`) and, when a compaction moves
the class, rewrites it to the new address (`HadesGC.cpp:572-591, 2661`).
Hades frees dead symbols and then clears weak roots in one pause
(`HadesGC.cpp:1936-1939`). A weak root read to be stored somewhere needs
the read barrier; a read for comparison does not (`WeakRoot.h:64-74`).

## Design: reads

The generic path's first lookup for a string key is the named lookup in
the object's own class. The read fast path is that same lookup, done
before the key conversion, the descriptor setup and the walk.

`JSObject::tryGetOwnNamedDescriptorOfKeyFast(self, runtime, key, desc)`:
if the key is a string with the uniqued bit, call
`HiddenClass::tryFindPropertyFast` on the object's class with the key's
`getUniqueID()`. It answers found (and fills the descriptor), not found,
or "cannot tell": the key is not a uniqued string, or the class has
properties and no map. It does not allocate. `getUniqueID` became public
for it.

Get, first thing in `getComputedWithReceiver_RJS`: if the helper answers
found and the property is not an accessor, return the slot's value
(`getNamedSlotValueUnsafe(...).unboxToHV`). The receiver is irrelevant
to a data property. Anything else continues into the existing code.

Has, first thing in `objectPrototypeHasOwnProperty` and `objectHasOwn`
(`objectHasOwnFast`): if the object argument is an object,
- found: return true, accessor or not;
- not found, and the class has none of `indexedStorage`, `lazyObject`,
  `proxyObject`, `hostObject`: return false;
- otherwise continue into the existing code.

"Cannot tell" happens once per class: the generic path that follows
builds the map, and the next lookup finds it. A class whose map was
taken by a child (an object that has since gained a property has the
child class, so this concerns the objects still at the parent) rebuilds
it the same way.

### Why this gives the generic result

Get. The numeric fast path does not apply to a string. The key converts
to the same SymbolID that the string holds. The named lookup on the
object comes before indexed storage, lazy initialization and the proxy,
so when it finds a data property the generic path returns that slot's
value.

Has, found: the own-descriptor lookup returns true at the same named
lookup. Has, not found: after a failed named lookup the generic path
returns false unless the object has indexed storage, is lazy or is a
proxy (`JSObject.cpp:921-926`); a host object is excluded as well, so
that the answer never depends on what a host object's class map holds.

Dictionary objects, objects with index-like named properties and frozen
objects need no exclusion: the lookup is the one the generic path would
do, on the same map.

Nothing allocates between reading the object's class and returning, so
no value is held across a safepoint.

## Design: puts

### Why a table here and not for reads

A put has to learn more than a read. For a name the object has, the
class's map answers as it does for a read. For a name it lacks, the
generic path walks the prototype chain to rule out a setter or a
read-only property, checks extensibility, and looks up the class
transition. The table holds the outcome of all of that per (class,
name), next to the epoch that says whether it still holds, and one probe
tells a store from an add.

For reads the table was tried and measured the same as the class's map
(see "History").

### The cache

`ComputedPropertyCache`, in `include/hermes/VM/ComputedPropertyCache.h`
and `-inline.h`, with its out-of-line code in
`lib/VM/ComputedPropertyCache.cpp`. The Runtime owns one. Its table is
1024 sets of two entries, allocated zeroed on the C++ heap when the
Runtime is constructed, so the Runtime's own layout does not grow by the
table.

An entry:

| field         | meaning                                                  |
|---------------|----------------------------------------------------------|
| `clazz`       | weak root; the class of the object. Null: the entry is empty |
| `name`        | SymbolID of the property                                 |
| `kind`        | `Slot` or `Add`                                          |
| `slot`        | slot index, 30 bits                                      |
| `writable`    | `Slot` only: a plain store to the slot is allowed        |
| `resultClazz` | `Add` only: weak root; the class after the add           |
| `parentEpoch` | `Add` only: the full parent cache epoch at fill, 64-bit  |

32 bytes with 64-bit pointers (HV64) and 24 with compressed pointers
(HV32), so the table is 64 KB or 48 KB per Runtime.

For tests the cache also exposes the set index of a key, the number of
non-empty entries, and a function that empties it.

The key is (`clazz`, `name`). The set index is a hash of the two raw
values. Lookup compares both ways of the set and returns the matching
entry or null. It writes nothing.

Insertion puts the new entry in way 0. If way 0 held a different key,
that entry moves to way 1 first, replacing what was there. If way 0 was
empty, way 1 is left alone, unless it holds the key being inserted, in
which case it is emptied. A key therefore never occupies both ways, and
two keys that share a set both stay cached.

What an entry asserts, for every object whose class is `clazz`:

- `Slot`: the class maps `name` to a data property (not an accessor) in
  `slot`. `writable` is the property's writable flag with the internal
  setter flag clear.
- `Add`: the class has no property `name`, is extensible, and
  `HiddenClass::addProperty(clazz, name, default flags)` yields
  `resultClazz` and `slot`. When `parentEpoch` was recorded, no object
  on the prototype chain was a proxy, host object or dictionary, and
  the generic put of `name` reached the add.

A class has `name` or it does not, so one key never needs both kinds.

### Which classes and names

A class is cacheable when none of these flags is set: `dictionaryMode`,
`hasIndexLikeProperties`, `hostObject`, `lazyObject`, `proxyObject`,
`indexedStorage`. For an `Add` entry both classes must be cacheable.

A key has a name when it is a string with the uniqued bit; the name is
its `getUniqueID()`.

Consequences the correctness arguments use:

- No cached name is index-like. A `Slot` name is a property of a class
  without `hasIndexLikeProperties`; an `Add` name is a property of the
  result class, which also lacks it. So indexed storage, on the object
  or on any prototype, is never consulted for a cached name.
- Every non-empty entry's name is alive, because a live class keeps its
  property names alive, and the entry is emptied in the same pause in
  which its class dies (below). A freed SymbolID can be reused only
  after that, and a string whose symbol was freed has lost its uniqued
  bit. So equal IDs mean the same name.

### The fast path

First thing in `putComputedWithReceiver_RJS` (`JSObject.cpp:1722-1777`):
if the key is a string and the receiver is the object itself, probe
(`ComputedPropertyCache-inline.h:40-53`): test that the object's class
is cacheable, test the key's uniqued bit and take its name, and look up
(class, name). The class test comes before the name, the hash and the
lookup; a class that is not cacheable has no entry, so the test only
spares it the rest.

The entry found is usable if it is a `Slot` entry with `writable`, or an
`Add` entry with `parentEpoch == getFullParentCacheEpoch()`. Without a
usable entry the put continues into the existing code unchanged, and has
encoded nothing.

With one, the value is encoded by `tryEncodeHermesValueNoAlloc`
(`SmallHermesValue-inline.h:153-167`), which does not allocate and
declines a value that `encodeHermesValue` would box: under HV32 and
BOXED a double that does not fit inline, and there any number under
Handle-SAN. Under HV64 it never declines. When it succeeds, the value is
stored through the entry found. When it declines, `encodeHermesValue`
boxes the value, which allocates, and then the probe and the usability
test are repeated (see "Safepoints"). If the entry found again is
usable, the value is stored through it; if not, the put continues into
the existing code.

The store:

- `Slot`: `setNamedSlotValueUnsafe`, return true.
- `Add`: read `resultClazz` through the read barrier and call
  `addNewOwnPropertyInSlot(obj, runtime, resultClazz, slot, shv)`,
  return true.

Both encodes feed one store: with two stores clang outlines the store
and the served put pays a call.

### Fills

Entries are recorded where the generic put has just established the
fact, with no extra lookup:

- After the in-place store to an existing own property, the inline
  `fillSlot(runtime, clazz, key, flags, slot)`. It does nothing unless
  the object's class is cacheable and the key has a name, tested in
  that order so that an excluded class pays neither a call nor the
  name; then it calls `recordSlot` with the name, which records only a
  named data property (`indexed`, `accessor`, `hostObject` and
  `proxyObject` clear) whose slot fits 30 bits. Neither allocates. The
  name is taken at the fill, not carried over from the probe:
  allocations can lie between the two, and the uniqued bit is tested
  where it is used.
- At the add (`JSObject.cpp:2060-2089`). Before the add, the put
  decides whether to record it: the key is a uniqued string and the
  receiver is the object itself. A uniqued key's SymbolID is the
  identifier for its content, so the add's SymbolID is the key's name.
  The put has already found the receiver extensible (`:2013-2019`), so
  it calls `addOwnPropertyImpl` directly with the default new-property
  flags, passing the decision, instead of going through
  `addOwnProperty`. `addOwnPropertyImpl` (`:3295-3354`) roots the start
  class in a Locals `PinnedValue` before `HiddenClass::addProperty` and
  the result class after it, and when the result is not a dictionary it
  passes both, the add's SymbolID and the slot `addProperty` returned
  to `tryCacheComputedAdd` (`:3251-3293`). That follows
  `tryCacheAddProperty` exactly: refuse if an object on the prototype
  chain is a proxy, host object or dictionary; write the entry with the
  current full epoch; mark every object on the chain with
  `setCachedUsingEpoch`. `markParentsCachedUsingEpoch` first walks the
  chain without rooting anything and returns if every object is already
  marked or typed (for those `setCachedUsingEpoch` changes nothing), and
  otherwise marks only the objects that are not; this is the fifth
  commit, and it serves the PutById fill too. The two loops of
  `tryCacheAddProperty` are the
  private helpers `parentsAllowEpochCaching` and
  `markParentsCachedUsingEpoch`, which both callers use. Nothing is
  recorded if either class is not cacheable or the start class is not
  extensible. The two classes cannot be equal, since only a dictionary
  result can be its own start and that never reaches the fill; it is
  an assertion. A PutById add cache entry and this fill are never asked
  for the same add, which `addOwnPropertyImpl` asserts.

A fill overwrites: a `Slot` or `Add` entry with the same key is
replaced, which is how a stale epoch is refreshed.

Reads record nothing. An object whose properties are only ever read
never enters the table.

### GC

`Runtime::markWeakRoots` calls the cache's `markWeakRoots(acceptor)`
under `markLongLived`. For each non-empty entry it accepts `clazz` and,
for `Add`, `resultClazz`, then empties the entry if `clazz` is now null
or has a different address, or if an `Add` entry's `resultClazz` is now
null.

So after marking, a non-empty entry has live classes, and it is in the
set its key hashes to. The kind is a field, never inferred from a null
pointer, and an `Add` entry with a dead result class does not exist.

### Why the fast path gives the generic result

Put to an existing property. The generic path finds the same own
property, takes the in-place branch because it is writable, not an
accessor and has no internal setter and the receiver is the object, and
stores the encoded value in the slot.

Put that adds. The start class has no such property and is not lazy, a
proxy or a host object, so the generic walk moves to the prototype
chain. The chain is fixed by the start class, and with an unchanged
epoch every object on it has the class and prototype it had at fill, so
the walk ends as it did then: not found, or found as a writable data
property on a prototype, both of which reach the add. The name is not
index-like, so indexed storage on the chain cannot answer differently.
The receiver is the object and the start class is extensible. Then
`addProperty` returns the result class from the transition table,
because the class is alive and was inserted there when it was created,
and the slot is the start class's property count.
`addNewOwnPropertyInSlot` performs the same class update and store as
`addOwnPropertyImpl`, as it does for the PutById add cache.

### Safepoints

- Nothing allocates between the last probe and the store. The probe and
  `tryEncodeHermesValueNoAlloc` do not allocate. `encodeHermesValue`
  allocates when it boxes (`SmallHermesValue-inline.h:131-151`), and a
  collection there can empty the entry found: its class moved or died,
  or an `Add` entry's result class died. If that result class was the
  last holder of the key's name, the collection also frees the key's
  SymbolID and clears its uniqued bit, while the string itself stays
  alive. So after that encode the entry is found again: the probe tests
  the class, tests the bit and takes the name, and looks up, and the
  entry is tested for use again. `addNewOwnPropertyInSlot` allocates
  only after it has installed the result class in the object and roots
  the value itself.
- A put that the fast path does not serve leaves an unused box behind
  only when the first probe found a usable entry, the value had to be
  boxed, and the second probe found none; the generic path then encodes
  again. Accepted.
- `recordSlot` does not allocate. The add fill allocates only in
  `setCachedUsingEpoch`, after the entry is written. The start and
  result classes are rooted inside `addOwnPropertyImpl`, across
  `HiddenClass::addProperty`, the storage allocation and the fill.
- No longjmp crosses this code: the JSObject functions return
  `CallResult`, and the SH callers throw only after they return.

## Input classes

An input a fast path does not serve is not free: it pays for the
attempt before the generic path runs. Measured costs are in "Costs".

Reads (key is a uniqued string unless the row says otherwise):

| input                                         | get                          | `hasOwnProperty`, `Object.hasOwn` |
|-----------------------------------------------|------------------------------|-----------------------------------|
| own data property                             | the slot's value             | true                              |
| own accessor                                  | lookup, then generic         | true                              |
| absent, plain or dictionary object            | lookup, then generic (the prototype walk) | false                |
| absent, object with indexed storage, lazy, proxy or host object | lookup, then generic | lookup, then generic   |
| class has properties and no map               | generic, which builds the map | the same                         |
| dictionary object, object with an index-like named property, frozen object | as for a plain object | as for a plain object |
| key not a uniqued string                      | generic after the test of the key | the same                     |
| receiver differs from the object              | as above: a data property ignores the receiver | not applicable   |

Puts, receiver is the object and the key is a uniqued string unless the
row says otherwise:

| input                                             | result                         |
|---------------------------------------------------|--------------------------------|
| own writable data property, cacheable class       | filled on the first put, then hits |
| read-only own data property                       | never filled; probe and miss, then generic, every time |
| name absent, cacheable class, nothing on the chain blocks the store | the add is filled on the first put, then every object of the class hits |
| setter or read-only property on the chain         | never filled; probe and miss, then generic, every time |
| dictionary, proxy, host, indexed storage, class with an index-like named property | the probe stops at the class flags, then generic, and a fill attempt that stops at the same test, every time |
| lazy object                                       | the probe stops at the class flags; generic, which materializes it; its new class is filled like any other |
| key not a uniqued string                          | the class test and the test of the uniqued bit, then generic, every time |
| key not a string                                  | generic after one type test    |
| add from a class with 64 properties (dictionary result) | never filled; probe and miss, then generic and the add fill's refusal |
| add from a class with 63 properties               | filled; the result is not a dictionary |
| add with a proxy, host object or dictionary on the chain | never filled; probe and miss, then generic and the add fill's walk of the chain, every time |
| add after an object on the chain changed          | epoch differs; generic, refilled |
| add to a non-extensible object                    | different class; probe and miss, then generic |
| receiver differs from the object                  | generic after the receiver test |
| class moved by compaction, or dead                | entry emptied by the GC        |
| third key in a full set                           | evicts the entry in way 1      |
| more hot (class, name) pairs than the table holds | probe, miss, generic and refill on every put that was evicted |

## Tests

Reads:

- `ObjectModelTest.TryGetOwnNamedDescriptorOfKeyFast`: a uniqued key
  found and absent; a string without the uniqued bit, a number and a
  Symbol answer "cannot tell".
- `test/hermes/computed-key-own-property.js`, run by the interpreter
  and, from `test/jit`, by the JIT. It asks get, `hasOwnProperty.call`
  and `Object.hasOwn` with uniqued keys and with the same names built at
  run time, over plain objects, accessors, prototype properties, a null
  prototype, dictionaries, index-like names, arrays, String objects,
  `arguments`, typed arrays, lazy and bound functions, frozen objects,
  primitives, proxies (with trap counts), non-string keys, objects that
  change between the questions, fresh functions asked `hasOwnProperty`
  before anything materializes them, and receivers that are not
  objects. Its expected output is that of the build before the change.
- Each of these changes makes a test fail: dropping the accessor check
  of the get; dropping the `indexedStorage`, the `lazyObject` or the
  `proxyObject` exclusion of the has; dropping the uniqued test.

The table, `unittests/VMRuntime/ComputedPropertyCacheTest.cpp`. The
fixture runs with a handle-sanitizer rate of 1.0, so that the
Handle-SAN trees move the heap on every allocation.

- `InsertLookup`, `TwoWays`, `EmptyWay0`: insertion, both ways of a
  set, eviction of the older of three keys, and the empty-way-0 rule.
- `Predicate`, `RecordSlot`: `isCacheable` per class flag; `recordSlot`
  and `fillSlot` record nothing for an accessor, an excluded class, a
  key without a name or a slot of 2^30, and record `writable` clear for
  a property with an internal setter.
- `WeakRoots`: with a test acceptor passed to `markWeakRoots`, an entry
  is emptied when its class is nulled, when its class moves, and when
  an `Add` entry's result class is nulled; untouched otherwise.
- `CollectEmpties`: entries whose classes have no object left are gone
  after `runtime.collect`, so no entry outlives its name.

The puts, same file. To prove that the fast path consults the table
they plant a false entry with the cache's own insertion functions and
check that the put follows it. Nothing allocates between a fill and the
`lookup` that observes it, or between planting an entry and the put: a
moving collector would otherwise empty the entry, correctly. A planted
entry's name is rooted by a SymbolID handle and its classes by pinned
values, because a false entry's class does not keep the name alive.

- `PutFillsAndHits`, `ReadOnlyPut`, `TypedObjectPut`: a put to an
  existing property fills a writable `Slot` entry; a planted entry is
  followed only when `writable` is set.
- `PutReceiver`, `AddReceiver`: with another receiver the put is
  generic and records nothing.
- `PutNonUniquedKey`: a string with the characters of an existing name
  but without the uniqued bit writes the property and does not follow a
  planted entry.
- `AddFillsAndHits`: a put that adds fills an `Add` entry, the next
  object of the class follows it, and a planted entry is followed.
- `AddAcrossStorage`: two objects get the same 20 names; the second
  follows the entries through the slots in the object, the first
  out-of-line slot and each growth of the storage.
- `AddLongStorage`: an `Add` hit on an object whose storage is longer
  than its class needs.
- `StaleEpochNotFollowed`, `AddEpoch`: a planted entry with a stale
  epoch is not followed; a setter or a read-only property appearing on
  the prototype, and a replaced prototype of the prototype, are seen by
  the next put.
- `AddNotRecorded`: the 65th property (a dictionary result), a
  dictionary prototype and a host-object prototype record nothing; the
  64th property is recorded.
- `SymbolDiesWhileStringLives`: a uniqued string whose SymbolID nothing
  holds loses its uniqued bit in a collection while the string lives.
- `AddNameCollected`: a put finds an `Add` entry whose result class only
  a dead object held, and that class alone holds the key's name. In the
  Handle-SAN BOXED tree, where the encode of 0.5 allocates and every
  allocation collects, the encode empties the entry and frees the
  SymbolID; the second probe must see the cleared bit and miss, and the
  generic path adds the property. There it fails if the second probe and
  its test are skipped, and, on the assertion in `getUniqueID`, if the
  second probe takes the name without testing the bit. In the other
  trees the put follows the entry.
- `CollectRefills`: after a collection has emptied the table, puts of
  the same names add and refill.

The encode, `unittests/VMRuntime/SmallHermesValueTest.cpp`:
`TryEncodeNoAllocMatchesEncode` and `TryEncodeNoAllocNoneForBoxed`.
`tryEncodeHermesValueNoAlloc` gives the bits of `encodeHermesValue` for
a value that is not boxed, and declines 0.1 under HV32 and BOXED, and
the integer 42 there under Handle-SAN.
`test/hermes/computed-property-cache-gc-encode.js` runs served puts of
boxed values with the handle sanitizer on.

`test/hermes/computed-property-cache.js`, run by the interpreter (`-O`,
`-O0`), `%shermes -exec` and, from `test/jit`, the JIT. Nine sections:
the `jsxProd` loop; gets, puts and has on own data properties; puts
that add 70 properties and cross into a dictionary; changes after hits
that must be seen (a setter and a read-only property appearing on the
prototype, freezing, preventing extensions, `delete` and re-adding);
unusual prototypes (a proxy, an array, a function not yet
materialized); a proxy as receiver; `Reflect.set` and `Reflect.get` with
another receiver; non-string keys, primitive receivers and more names
than the table holds; and receivers built by JSON.parse (with duplicate
keys, nested, with revivers), object literals with duplicate keys,
spread, `Object.fromEntries`, `Object.assign`, class instances,
`Object.create` with descriptors, regexp match results, functions and
arrays. It passes without the change too; its job is to fail if a fast
path changes behavior in any engine.

Not tested:

- The host-object exclusion of the has fast path.
- The read barrier on `resultClazz`. A missing barrier shows only when
  an old-generation mark is in progress at the hit, the receiver has
  already been scanned and the result class has no other reference.
  Arranging that needs a way to step Hades marking from a test, and no
  such test peer exists. The hit uses `WeakRoot::getNonNull(base, gc)`,
  as the three existing add-cache hits do, none of which has such a
  test.

## Measurements

2026-11-19, one session. x86-64 Release, Clang 18 `-O3`,
`HERMESVM_ALLOW_JIT=2`. Four builds:

- "before": the commit that fixes `addNewOwnPropertyInSlot`, with
  neither part;
- "reads": the commit before the table, which has the reads (and the
  two unrelated fixes);
- "puts": the table and its use;
- "top": the fifth commit, which skips marked prototypes.

Bytecode from each build's own `hermesc -O -emit-binary`, `taskset -c
2`, the configuration order rotated by the repetition, medians. React:
the four benchmarks in `~/work/react-benchmarks`, 16 configurations
(four builds, HV64 and HV32, `-Xjit` and `-Xjit=off`), 7 runs, 3 for
full-size pixel-grid. Octane without RegExp (`benchmarks/octane`):
"reads", "puts" and "top", HV64 and HV32, `-Xjit`, 5 runs. Every run
exited 0. The scripts and scores are in
`/tmp/claude-1000/react-bench/cpc/cut`. "~" marks a ratio whose two
min-max ranges overlap.

React, JIT:

| benchmark | mode | before | reads | puts | top | reads / before | puts / reads | top / puts | top / before |
|---|---|---|---|---|---|---|---|---|---|
| pixel-grid-small | HV64 | 197030 [194431-197617] | 206004 [202442-208705] | 225368 [218441-228867] | 222595 [218802-226394] | 1.046 | 1.094 | 0.988~ | 1.130 |
| pixel-grid | HV64 | 12420 [12347-12603] | 12962 [12892-12976] | 13440 [13096-13748] | 13160 [13131-13163] | 1.044 | 1.037 | 0.979~ | 1.060 |
| draw | HV64 | 41042 [40818-41497] | 42519 [42279-42762] | 43340 [42762-43508] | 43424 [43008-43592] | 1.036 | 1.019~ | 1.002~ | 1.058 |
| relay-feed | HV64 | 3584 [3559-3623] | 3597 [3521-3636] | 3610 [3597-3650] | 3584 [3546-3650] | 1.004~ | 1.004~ | 0.993~ | 1.000~ |
| pixel-grid-small | HV32 | 211815 [210248-214440] | 227039 [226265-227950] | 247052 [244772-247975] | 246440 [245831-247512] | 1.072 | 1.088 | 0.998~ | 1.163 |
| pixel-grid | HV32 | 13193 [13178-13257] | 13627 [13414-13675] | 14456 [14421-14553] | 14521 [14486-14636] | 1.033 | 1.061 | 1.004~ | 1.101 |
| draw | HV32 | 38376 [38051-38574] | 38976 [38774-39248] | 39594 [39317-39805] | 39664 [39248-39805] | 1.016 | 1.016 | 1.002~ | 1.034 |
| relay-feed | HV32 | 4149 [3717-4202] | 4184 [4167-4202] | 4167 [4149-4219] | 4184 [4032-4237] | 1.008~ | 0.996~ | 1.004~ | 1.008~ |

Geomean of the four, top / before: 1.061 under HV64 and 1.075 under
HV32.

The fifth commit reads 1 to 2% low under HV64 in this table, inside
the ranges. A second session of "puts" against "top" (15 runs of
pixel-grid-small, 9 of draw, 7 of pixel-grid) gave 0.991~, 0.998~ and
0.986~ under HV64 and 1.003~, 0.996~ and 0.999~ under HV32. The
benchmarks run a fixed script of steps, so `perf stat` compares the
work exactly: "top" runs 143 thousand fewer instructions of 25.2
billion on pixel-grid-small and 0.3% fewer cycles under HV64, and 190
million fewer of 362 billion on pixel-grid with 0.2% fewer cycles. The
React benchmarks rarely fill an add cache under a marked prototype, so
the fifth commit changes nothing measurable there.

React, interpreter (`-Xjit=off`). It reaches the same functions, so it
gains too:

| benchmark | mode | before | reads | puts | top | reads / before | puts / reads | top / puts | top / before |
|---|---|---|---|---|---|---|---|---|---|
| pixel-grid-small | HV64 | 147298 [141889-149962] | 153727 [150075-155045] | 162250 [149680-163719] | 161657 [154443-164940] | 1.044 | 1.055~ | 0.996~ | 1.097 |
| pixel-grid | HV64 | 9050 [8886-9068] | 9276 [9187-9283] | 9696 [9619-9862] | 9764 [9681-9822] | 1.025 | 1.045 | 1.007~ | 1.079 |
| draw | HV64 | 24140 [24036-24244] | 24114 [24036-24349] | 24455 [24402-24562] | 24482 [24218-24562] | 0.999~ | 1.014 | 1.001~ | 1.014~ |
| relay-feed | HV64 | 2801 [2778-2841] | 2809 [2762-2833] | 2833 [2793-2857] | 2825 [2801-2865] | 1.003~ | 1.009~ | 0.997~ | 1.009~ |
| pixel-grid-small | HV32 | 158946 [155651-160161] | 165833 [158376-167932] | 177917 [171997-178636] | 178555 [172668-180909] | 1.043~ | 1.073 | 1.004~ | 1.123 |
| pixel-grid | HV32 | 10091 [10031-10094] | 10214 [10079-10293] | 10844 [10812-10901] | 10941 [10889-10976] | 1.012~ | 1.062 | 1.009~ | 1.084 |
| draw | HV32 | 23908 [23732-24062] | 24323 [23908-24697] | 24509 [24349-24697] | 24616 [24402-24889] | 1.017~ | 1.008~ | 1.004~ | 1.030 |
| relay-feed | HV32 | 3390 [3367-3413] | 3401 [3367-3425] | 3425 [3257-3436] | 3425 [3390-3448] | 1.003~ | 1.007~ | 1.000~ | 1.010~ |

Geomean, top / before: 1.049 under HV64 and 1.061 under HV32.

Octane, JIT. The reads against "before" were measured on 2026-11-13
with the same method: geomean 1.004 under HV64 and 1.005 under HV32,
the ranges overlapping in every row.

| benchmark | HV64 reads | HV64 puts | HV64 top | puts / reads | top / puts | HV32 reads | HV32 puts | HV32 top | puts / reads | top / puts |
|---|---|---|---|---|---|---|---|---|---|---|
| richards | 7583 [7297-8187] | 7406 [7106-8123] | 7523 [7071-7809] | 0.977~ | 1.016~ | 6132 [5687-6174] | 6075 [5910-6125] | 5814 [5677-6111] | 0.991~ | 0.957~ |
| deltablue | 7432 [6843-7544] | 7485 [7253-7518] | 7577 [7498-7597] | 1.007~ | 1.012~ | 6010 [5997-6043] | 6004 [5944-6023] | 5984 [5832-5990] | 0.999~ | 0.997~ |
| crypto | 7204 [7165-7211] | 7204 [7197-7207] | 7204 [7200-7212] | 1.000~ | 1.000~ | 6539 [6529-6552] | 6542 [6529-6555] | 6536 [6529-6552] | 1.000~ | 0.999~ |
| raytrace | 13024 [12898-13098] | 12885 [12542-12998] | 13059 [12863-13146] | 0.989~ | 1.014~ | 11004 [10930-11045] | 11026 [10782-11056] | 11015 [10982-11089] | 1.002~ | 0.999~ |
| earley-boyer | 13190 [13121-13349] | 13189 [13146-13356] | 13235 [13128-13321] | 1.000~ | 1.003~ | 12993 [12914-13006] | 12997 [12776-13011] | 13001 [10980-13063] | 1.000~ | 1.000~ |
| splay | 4779 [4730-4790] | 4673 [4644-4717] | 4771 [4746-4803] | 0.978 | 1.021 | 6077 [6026-6222] | 6065 [6026-6185] | 6112 [6079-6167] | 0.998~ | 1.008~ |
| navier-stokes | 14485 [14413-14514] | 14500 [14500-14514] | 14500 [14456-14514] | 1.001~ | 1.000~ | 7780 [7764-7803] | 7787 [7780-7795] | 7749 [7741-7787] | 1.001~ | 0.995~ |
| pdfjs | 20805 [20784-20825] | 20764 [20703-20845] | 20886 [20703-20948] | 0.998~ | 1.006~ | 20030 [19931-20069] | 19951 [19833-20010] | 20010 [19911-20089] | 0.996~ | 1.003~ |
| mandreel | 4805 [4689-4846] | 4800 [4791-4807] | 4784 [4764-4814] | 0.999~ | 0.997~ | 4208 [4158-4219] | 4208 [4189-4224] | 4210 [4204-4215] | 1.000~ | 1.000~ |
| gbemu | 20926 [20864-21031] | 20618 [19401-21219] | 20864 [20679-20947] | 0.985~ | 1.012~ | 19766 [19582-19784] | 19710 [18998-19766] | 19655 [19401-19766] | 0.997~ | 0.997~ |
| box2d | 24229 [24063-24448] | 24158 [23857-24374] | 24374 [23715-24448] | 0.997~ | 1.009~ | 16994 [16825-17027] | 16775 [16758-16825] | 16862 [16675-16994] | 0.987~ | 1.005~ |
| zlib | 6421 [6408-6428] | 6411 [6397-6457] | 6448 [6423-6482] | 0.998~ | 1.006~ | 6253 [6214-6299] | 6255 [6176-6291] | 6224 [6215-6243] | 1.000~ | 0.995~ |
| typescript | 54888 [54723-55293] | 55413 [55007-55582] | 55510 [54005-55679] | 1.010~ | 1.002~ | 52235 [51832-52407] | 51937 [51643-52107] | 52064 [51853-52601] | 0.994~ | 1.002~ |
| geomean | | | | 0.995 | 1.007 | | | | 0.997 | 0.997 |

The one row whose ranges do not overlap, splay under HV64, was run
again with 9 runs: puts / reads 0.993~, top / puts 1.020~ under HV64,
1.000~ and 1.010~ under HV32. On 2026-11-14 puts / reads measured
0.999~. So Octane does not show a gain or a loss.

Not measured in this form: a profile of the three `jsxProd` sites. The
per-operation profile in dz 01a1cdf5-7332 is of the prototype.

## Costs

What the inputs the fast paths do not serve cost, and what they gain
where they do, against "before". Two measures, both 2026-11-19 with
`-Xjit=force`. (Before that date these micro-benchmarks were timed with
`-Xjit`, which compiles a function after 32 calls; they call theirs 4
to 6 times, so some of them ran in the interpreter.)

Exact instructions per operation, from `perf stat` on the case files
in `/tmp/claude-1000/react-bench/cpc/why`. The reads add nothing to
these puts; the column "puts" includes the reads.

| case | HV64 before | puts | top | HV32 before | puts | top |
|---|---|---|---|---|---|---|
| dictionary object, put | 414.5 | 436.5 (+22) | 436.5 | 477.7 | 501.7 (+24) | 501.7 |
| key built at run time, put | 656.7 | 690.7 (+34) | 690.7 | 703.7 | 740.7 (+37) | 740.7 |
| setter reached by a computed put | 651.7 | 686.7 (+35) | 686.7 | 720.4 | 757.4 (+37) | 757.4 |
| adds along class paths that never repeat (per put) | 899.5 | 968.2 | 891.1 (-8) | 929.8 | 1006.7 | 920.0 (-10) |
| computed adds to an object that is already a prototype (per iteration) | 7619 | 10637 | 9498 (+1879) | 8063 | 11209 | 9987 (+1924) |
| 4000 (class, name) pairs, round-robin put (every put misses and refills) | 384.0 | 488.4 (+104) | 488.4 | 420.7 | 528.7 (+108) | 528.7 |
| put to an existing property, served | 410.3 | 224.3 | 224.3 | 470.0 | 339.1 | 339.1 |
| add to a fresh object, served | 813.6 | 246.7 | 246.7 | 850.8 | 305.4 | 305.4 |
| PutById adds under a chain of marked prototypes, missing the add cache (top / puts) | | | 0.674 | | | 0.692 |

Time, top / before (puts / before where they differ), per case the
minimum over 4 processes, run in an order rotated by the repetition, of
each process's best of 5 runs (`bench.js` and `bench2.js` in
`/tmp/claude-1000/react-bench/cpc/final-review`, `bench4.js` in
`/tmp/claude-1000/react-bench/cpc/put`), HV64 / HV32; below 1 is
faster.

Slower:

| case | top / before |
|---|---|
| dictionary object, put (double, integer, second program) | 1.04, 1.04, 1.02 / 1.03, 1.03, 1.03 |
| key built at run time (not uniqued), put of a double | 1.03 / 1.03 |
| setter reached by a computed put | 1.04 / 1.06 |
| key built at run time, get | 1.06 / 1.01 |
| adds along class paths that never repeat: every add misses and fills | 1.01 / 1.03 (puts: 1.09 / 1.11) |
| 4000 (class, name) pairs, round-robin puts to existing properties: every put misses and refills | 1.32 / 1.30 |
| computed adds to an object that is already a prototype (each bumps the epoch), next to an ordinary constructor | 1.14 / 1.12 (puts: 1.28 / 1.25) |

The last three are built to defeat the table. The first three are what
an unserved put pays for the attempt: 2 to 6% of a put. The get with a
key built at run time is the reads commit: 10 instructions more per get
(627 to 637 under HV64, 642 to 653 under HV32), the failed test of the
uniqued bit before the generic path.

Unchanged or faster:

| case | top / before |
|---|---|
| dictionary object, get | 0.62 / 0.62 |
| dictionary object, `hasOwnProperty` | 0.51 / 0.59 |
| object after a `delete` (a dictionary), get and put | 0.78 / 0.80 |
| array, `a["length"]` | 0.48 / 0.48 |
| absent key, `hasOwnProperty` and get | 0.79 / 0.84 |
| 4000 (class, name) pairs, round-robin get | 0.55 / 0.56 |
| the same, `hasOwnProperty` | 0.51 / 0.55 |
| get, put and `hasOwnProperty` on a small object, all served | 0.47 / 0.57 |
| put to an existing property, served | 0.59 / 0.76 |
| 8 adds to a fresh object, served | 0.31 / 0.36 |

Where the remaining put-side instructions go was traced exactly
(single-stepping whole operations under gdb in Release `-O3 -g` builds,
whose code `-g` does not change; scripts in
`/tmp/claude-1000/react-bench/cpc/why`). Under HV64, a put to a
dictionary pays the string-tag and receiver tests at entry and the
class test in the probe, then the class test of the fill after the
in-place store. A round-robin miss pays the hash and the two-way
compare, then `recordSlot`, which takes the name and the hash again and
moves way 0 to way 1. An add that misses pays the probe and
`tryCacheComputedAdd`, which walks the prototype chain to check it and
writes the entry. Computed adds to an object that is already a
prototype bump the epoch on each later add to it, which also empties
the PutById add cache of the constructor next to them: that is the
cost the fifth commit does not remove.

## Verification

2026-11-19, at the fifth commit. "Unit" is
`--gtest_filter='ComputedPropertyCache*:ObjectModelTest.*:StringPrimTest.*:SmallHermesValue*'`,
64 tests in HV64 trees and 65 in HV32 and BOXED trees. "Lit" is the 8
tests that `LIT_FILTER='computed-key-own-property|computed-property-cache|regress-add-cache|regress-computed-key-symbol-rooting|add-cache'`
selects, among them `computed-property-cache-gc-encode`; on arm64 the
filter is `jit/computed-|jit/regress-add-cache|jit/.*add-cache`.

| tree | run | result |
|---|---|---|
| cmake-build-x86jit (ASan, Hades, HV64) | full check-hermes; unit | 4719 expected passes, 6 expected failures, 162 unsupported, 0 unexpected; 64 passed |
| cmake-build-x86jit-hv32 | full check-hermes; unit | 4720 / 6 / 162, 0 unexpected; 65 passed |
| cmake-build-x86jit-boxed | full check-hermes; unit | 4720 / 6 / 162, 0 unexpected; 65 passed |
| cmake-build-x86jit-malloc (MallocGC) | unit; lit | 64 passed; 8 passed |
| cmake-build-x86jit-hsan (MallocGC, Handle-SAN, HV64) | unit; lit | 64 passed; 8 passed |
| cmake-build-x86jit-hsan-boxed (MallocGC, Handle-SAN, BOXED) | unit; lit | 65 passed; 8 passed |
| cmake-build-arm64 (qemu) | unit; the JIT runners; the interpreter by hand ("" and -O0) on the three main lit tests, against the host FileCheck | 64 passed; 3 passed; FileCheck rc 0 in all six |
| cmake-build-arm64-hv32 (qemu) | same | 65 passed; 3 passed; FileCheck rc 0 in all six |

The table commit alone was built in Release with its unit tests: 34
passed.

## Alternatives considered

- A cache operand on `GetByVal`/`PutByVal`. The sites are megamorphic in
  the key: one site sees every prop name, so a per-site entry misses.
  It also changes the bytecode format.
- Having the compiler pass the `for-in` variable's SymbolID. Not needed:
  the key string already carries it.
- The table for reads as well. Built and measured first; see "History".
- The class's own structures for puts: the property map for a store and
  the transition table for an add. A store would work as the reads do.
  An add still needs the walk of the prototype chain or somewhere to
  keep an epoch per (class, name), and the root class of object
  literals has one transition per first property name. With the table
  one probe tells a store from an add.
- A one-way table. The prototype's tables were one-way, and at 256
  entries two hot keys in one slot evicted each other (dz
  01a1cdf5-7332, "Table size"). As an estimate, not a measurement: n hot
  keys in m slots give about n^2/2m colliding pairs, ten for 200 keys in
  2048 slots. Two ways remove pair collisions for one extra compare on a
  way-0 mismatch; `TwoWays` shows the mechanism, and no benchmark here
  can show the benefit.
- Separate tables for stores and adds, as in the prototype. A put then
  probes twice.

## Risks

- More objects get marked `isCachedUsingEpoch`: the prototypes of
  objects that receive computed adds, as PutById adds already mark
  theirs. A program that keeps changing such a prototype bumps the
  epoch, which empties the PutById add caches and the JIT's GetById
  chain entries as well as these.
- Every put that misses with a cacheable result writes an entry. A
  program whose objects all have distinct classes pays that write and
  gets no hits; "Costs" has the number.
- The table is fixed at 2048 entries. An application with more hot
  (class, name) pairs than that loses hits to eviction, not correctness.
- A read of a class without a property map does the lookup twice, once
  to learn that there is no map and once in the generic path. That
  happens once per class, and again for a class whose map was taken by
  a child or dropped by MallocGC.

## Files

- Reads: `include/hermes/VM/JSObject.h` (the helper),
  `lib/VM/JSObject.cpp` (the get), `lib/VM/JSLib/Object.cpp` (the has),
  `include/hermes/VM/StringPrimitive.h` (`getUniqueID` public),
  `unittests/VMRuntime/ObjectModelTest.cpp`,
  `test/hermes/computed-key-own-property.js` and its `test/jit` runner.
- The table: `include/hermes/VM/ComputedPropertyCache.h`,
  `include/hermes/VM/ComputedPropertyCache-inline.h`,
  `lib/VM/ComputedPropertyCache.cpp`; `include/hermes/VM/Runtime.h` and
  `lib/VM/Runtime.cpp` (the member, its accessor, the `markWeakRoots`
  call); `unittests/VMRuntime/ComputedPropertyCacheTest.cpp`; the two
  `CMakeLists.txt`.
- Puts: `lib/VM/JSObject.cpp`, `include/hermes/VM/JSObject.h` (the fast
  path, the two fills, the two helpers split out of
  `tryCacheAddProperty`), `test/hermes/computed-property-cache.js` and
  its `test/jit` runner, `test/hermes/computed-property-cache-gc-encode.js`;
  `include/hermes/VM/SmallHermesValue.h` and `-inline.h`
  (`tryEncodeHermesValueNoAlloc`) and
  `unittests/VMRuntime/SmallHermesValueTest.cpp`.

## History

- 2026-11-11. A throwaway prototype with a read table and an add table,
  probed in the JIT's helpers only, made pixel-grid-small 16.5% faster
  under HV64 and 17.7% under HV32 (dz 01a1cdf5-7332, which also has a
  per-operation profile and the table-size measurements).
- 2026-11-12. The first form of this design was built: one table that
  served gets, `hasOwnProperty`, puts and adds, with fills at each
  generic path. Against fc6322e0e with the JIT it measured 1.149 / 1.164
  on pixel-grid-small (HV64 / HV32), 1.075 / 1.109 on pixel-grid and
  1.050 / 1.041 on draw. The implementation plan
  (`doc/superpowers/plans/2026-11-11-computed-property-cache.md`) is the
  plan of that form.
- Its final review found that `JSObject::addNewOwnPropertyInSlot`
  appended the value, assuming the object's property storage is exactly
  as long as its class needs. `JSObject::create` with a property count
  makes it longer, and JSON.parse passes the member count, duplicate
  keys included, so a cached add to such an object wrote past the end of
  the storage. Named adds through the PutById add cache did this before
  the change. The helper now stores in place when the slot exists; that
  is the first commit, with its own tests.
- 2026-11-13. The question was why a table in front of the class's map
  should be faster than the map: the class's map answers a read of an
  own property with no miss. With the table's read paths switched off,
  and then replaced by a direct lookup in the class's map, the three
  benchmarks measured the same as with the table under HV64 and up to
  1.5% behind it under HV32. The reads were rebuilt as that lookup, in a
  commit that needs no table; it also serves dictionary objects and
  answers `hasOwnProperty` false, which the table did not. The table's
  read paths, their fills and their tests were removed.
- The table stayed for puts because one probe there tells a store from
  an add and carries what the add needs.
- Two older defects that the reviews ran into were fixed in their own
  commits: a bare SymbolID kept across a GCScope flush in
  `JSObject::getComputedPrimitiveDescriptor`, and
  `StringPrimTest.StringsAreMemcpySafe` reading an uninitialized word.
- 2026-11-18. The put-side costs were traced instruction by instruction
  ("Costs" says how) and four cuts were folded into the table and put
  commits or added after them. The probe tests the class before the
  name (a put to a dictionary: 8 fewer instructions under HV64, 11
  under HV32). The value is encoded after the probe, not before it,
  with `tryEncodeHermesValueNoAlloc` and a second probe after a boxing
  encode (10 fewer per unserved put under HV32). The add fill moved
  into `addOwnPropertyImpl`, which already roots the classes and knows
  the slot, and the put calls it directly (about 70 fewer per add that
  fills). Prototype marking skips objects already marked, the fifth
  commit (about 98 fewer per marked prototype per fill, PutById fills
  included). Together, with the JIT and against the reads commit, the
  exact instructions an unserved put adds went from 29 to 22 under HV64
  and from 58 to 24 under HV32, and a put that adds along class paths
  that never repeat went from 114 more to 19 fewer under HV64.
