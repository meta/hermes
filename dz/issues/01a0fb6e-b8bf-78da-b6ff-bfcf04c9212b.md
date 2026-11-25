---
id: 01a0fb6e-b8bf-78da-b6ff-bfcf04c9212b
title: "Symbol.species not implemented: subclass slice/map/filter return base class"
type: bug
status: open
resolution: null
component: jslib
assignee: null
created: 2026-10-02T07:05:32.607Z
creator: Tzvetan Mikov <tmikov@fb.com>
---

`Symbol.species` is not implemented. For Array, %TypedArray% and
ArrayBuffer, methods that should construct their result via the species
constructor return the base class for subclass instances. For Promise,
Map, Set and RegExp, only `C[Symbol.species]` is undefined (Promise
`then` already constructs through `this.constructor`):

```js
class S extends Uint8Array {}
const s = new S([1, 2, 3]);
s.slice(1) instanceof S;          // false, spec: true
s.map(x => x) instanceof S;       // false, spec: true
s.filter(x => true) instanceof S; // false, spec: true
S[Symbol.species] === S;          // false, spec: true
class A extends Array {}
new A(1, 2, 3).slice(1) instanceof A; // false, spec: true
```

Found while migrating the typed array allocate family. A build from
before that change gives the same result, so this is existing
behavior, not a regression. `Symbol.species` is not a predefined
well-known symbol (PredefinedSymbols.def). `speciesConstructor`
(Operations.cpp) reads `constructor` but never `@@species`, so
`JSTypedArrayBase::allocateSpecies` always gets the default constructor.
Array methods (slice/map/filter/flat/flatMap/splice/concat) and
`ArrayBuffer.prototype.slice` do not perform
ArraySpeciesCreate/SpeciesConstructor at all. RegExp species is already
tracked by TODOs T69340954 and T35212035 in JSLib/RegExp.cpp.

Not listed in doc/SpecIncompat.md. Either add the symbol, the
`get [Symbol.species]` accessors, the @@species lookup in
`speciesConstructor` and ArraySpeciesCreate in the Array/ArrayBuffer
methods, or document the incompatibility.

## Log

- 2026-10-02T07:05:32.607Z  Tzvetan Mikov <tmikov@fb.com>  created
- 2026-10-02T07:05:37.509Z  Tzvetan Mikov <tmikov@fb.com>  component: gc -> jslib
