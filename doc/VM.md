---
id: vm
title: VM Overview
---

## Value Representation

### HermesValue

The VM uses a class called `HermesValue` to encapsulate JS values efficiently,
preserving their type while still allowing them to fit in a register.
NaN-tagging is used to store different types of values;
we store values in the lower bits of a `uint64_t`.
Thus, when the `uint64_t` is interpreted as a `double`,
tagged `NaN` values can hold non-`double` types.

After we reserve the canonical quiet NaN to be used as the NaN representation
in the VM, we have 51 remaining bits we can set. Even on 64-bit systems, we are
able to accommodate a full pointer and type tag, since pointers are never more
than 48 bits in practice.

A `HermesValue` can take on any of the following types of values, which are
distinguished by their type tag:
1. Empty
2. Undefined
3. Null
4. Boolean
5. Symbol
6. Native value (used to store an int or pointer for bookkeeping)
7. String pointer
8. Object pointer

`HermesValue` is used across the VM to store and pass JS values.
`PinnedHermesValue` is used in non-moveable memory, primarily for the register
stack and other GC roots in known locations in memory. `GCHermesValue`s are
used on the GC managed heap.

### HermesValue32

When compressed pointers are enabled, we also encode some values in a compact
32-bit representation called `HermesValue32`. Because compressing pointers to
store in this format requires additional work, we also avoid using
`HermesValue32` for frequently accessed values (like the register stack).
Instead we selectively use `HermesValue32` for objects that are known to
consume a large percentage of heap memory, but that are unlikely to affect
performance.

`HermesValue32` supports storing all of the types that `HermesValue` does,
except for native values, which were dropped because they are relatively rare
and because we do not have a mechanism for compressing native pointers outside
the GC managed heap.

Instead of using NaN boxing, it takes advantage of the 8-byte alignment of the
Hermes heap to store tags, since the lowest 3 bits of a pointer are always
guaranteed to be zero (even after compression).

`HermesValue32` also requires special handling for doubles. It is able to store
small integers up to 29 bits inline, but anything that cannot be represented in
that form must be stored as a separate double object on the heap. Based on the
workloads we have looked at, actual doubles are very rarely used, so the
overhead of this approach is small.

### Strings

`StringPrimitive` is used to store immutable UTF16 encoded strings,
and `StringPrimitive *` can be stored in `HermesValue` to make JS String values.

Internally, `StringPrimitive` can be
- `DynamicStringPrimitive` (stored in the GC heap)
- `ExternalStringPrimitive` (stored as a pointer outside the VM, such as into a bytecode file)

## Runtime

The `Runtime` class is the primary driver of the VM.
It contains the current environment and heap, as well as the code to execute.
`Runtime` is used to execute `RuntimeModule`s,
which are constructed from `BytecodeModule`s using `Runtime::runModule()`.

### Runtime Module

A `RuntimeModule` is the VM representation into a bytecode file.
`RuntimeModule`s are stored outside the GC heap and are constructed via `new`.

To allow for segmentation of bytecode files and `require`ing modules between
separate segments, we collect `RuntimeModule`s in a class called `Domain`.
You may think of the `Domain` as the collection of bytecode files which were
all compiled in the same invocation of the compiler.

Every `JSFunction` shares ownership of a `Domain`, and the `Domain` owns
the `RuntimeModule`s which provide those functions. In this way, when all
`JSFunction`s which require the files in a `Domain` are collected,
the `Domain` and the `RuntimeModule`s are also collected.

### Runtime Identifiers

The `Runtime` contains an `IdentifierTable`,
which is used for getting unique IDs for strings.
The table is used to go from `StringPrimitive` to `SymbolID` and back.
It's prepopulated with some "predefined strings",
the set of strings that are required by built in functions,
which can be seen in `PredefinedStrings.def`.

### Garbage Collection

Currently, the VM uses `HadesGC` by default, a concurrent garbage collector aimed at dramatically lowering pause times over our previous collector [GenGC](./GenGC.md). The heap in Hermes is non-contiguous, which allows us to avoid reserving large regions of address space upfront, and allows us to return memory to the OS at a finer granularity. Garbage collection in Hermes is precise, which means that the GC always knows which values contain valid pointers.

See the documentation for [Hades](./Hades.md) for details
on how it works.

The garbage collector moves objects to different place on the heap,
invalidating `HermesValue`s,
so there are a couple classes which allow updating them automatically.
`Handle<>` and `Handle<T>` are garbage collector-aware handles;
they are moved if a collection occurs in between two successive accesses.
So, to ensure correctness in the VM,
use the handles instead of passing raw `HermesValue` between functions.

Values are rooted by storing them in `PinnedValue<T>` fields of a stack
allocated `Locals` struct, which the garbage collector marks as roots.
A `Handle<T>` is a view of such a `PinnedHermesValue`.

We also provide `PseudoHandle<T>` classes which are explicitly *not* handles.
These are used to be explicit about storage of raw pointers and `HermesValue`.
`PseudoHandle` should be used as an argument in place of a raw pointer to
functions which may want to turn that argument into a `Handle`,
but in which it's not necessary to *always* incur the cost of pinning the value.
`PseudoHandle` also does not have a copy constructor,
and moving out of one invalidates it.
This prevents the reuse of `PseudoHandle` after an allocating function call.

#### Rules for GC-safe code

See [GCSafeCoding.md](GCSafeCoding.md) for the complete guide to writing correct
and performant GC-safe C++ code, including the preferred `Locals` + `PinnedValue`
API, handle usage, error handling patterns, and common mistakes.

## Object Model

Currently the object model is a VTable-based scheme,
in which all possible JS values inherit from a base garbage collector VTable.
These are called "cells", and all the cells are defined in `CellKinds.def`.
Objects have a special `ObjectVTable`, Callables have a `CallableVTable`, etc.

### Objects

Each JS object is represented by `Object` (or a class derived from `Object`).
JS objects have a set of name/value pairs, and some optional "indexed storage".
Read more about how `Object` works in `ObjectModel.h`.
The Runtime contains a global object which is used to store in global scope.

### Arrays

Arrays, the `arguments` object, etc. inherit from Object directly,
but simply provide their own implementations of `*OwnIndexed` using the VTable.

### Functions

Functions and native functions inherit from `Callable`.
This allows them to call `executeCall*` to run functions using the internal API.

### Boxed Primitives

The VM has classes used to contain Booleans, Strings, and Numbers,
when they are constructed using their respective JS constructors.
`JSString` is a boxed `String` object, etc.

## REPL

The HermesVM provides a REPL in `bin/hermes`,
which calls through to the `eval()` global function in the `Runtime`.
