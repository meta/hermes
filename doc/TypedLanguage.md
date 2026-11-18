# Typed Language

The typed language implementation is in progress and considered unstable. This is a usage guide that explains how to use it and what features currently work.

## Type System

The Flow type syntax is supported (use `-typed`).

The TypeScript type syntax is partially supported (use `-parse-ts` to convert TS to Flow before compilation).

### Primitive Types

* `void` (`undefined`)
* `null` (`null`)
* `boolean` (`true`, `false`)
* `string` (`"abc"`)
* `number` (`1`, `3.5`, `Infinity`, `NaN`)
* `bigint` (`1n`)

### Native Types

* `c_ptr` (raw C pointer)

### Any/Mixed

* `any` is a supertype of all types, casts implicitly to other types with a runtime type check.
* `mixed` is a supertype of all types, but does not cast implicitly.

### Empty

* `empty` has no values. It is the subtype of all other types.

### Literal Types

* String literal types: `"left"`, `"right"`
* Number literal types: `1`, `-3`
* Boolean literal types: `true`, `false`

`"abc"` is typed as `"abc"`. Literal types flows into their general (widened) types, and into another literal only when the value is identical. Operators treat a literal as the widened type. This can be used for tagged unions and string enums.

#### Widening

A fresh literal is widened to its general type (`"abc"` -> `string`) when it becomes the inferred type of a mutable binding, for example:

* `let`/`var`: `let x = 'a'` infers `string`.
* Array literals: `['a','b']` infers `Array<string>`.
* Object fields, indexer keys, and indexer values.

The literal is kept (no widening) for `const`, or any annotated context (e.g. `Array<"a">`).

#### Absorption in unions

A literal arm is dropped from a union that already contains a type the literal flows into: its general type, `any`, or `mixed`. So `string | "abc"` is just `string`, while `"a" | "b"` stays a union.

### Unions

`A | B` indicates a union between two types `A` and `B`. Values that are either `A` or `B` are valid values of `A | B`. Unions may be cyclic, but cycles must include another type. For example: `type A = A` is invalid, but `type A = [A, B] | null` is valid.

### Arrays

The typed language implements Array as a generic: `Array<T>` or `T[]` both work, and mean the same thing.

These soundly typed arrays are incompatible with untyped JS arrays and are more restrictive. They do not allow holes or empty spaces: every element of an array `a` in the range `[0, a.length)` _must_ be of type `T`. Indexed access to arrays is bounds-checked and throws on out-of-bounds access.

### Tuple

Tuples are shown with brackets: `[A, B, C]`.

When instantiating a tuple, it is typically necessary to specify the type to avoid confusion with arrays:
```
var x: [number, string] = [1, "abc"];
```

Tuples are plain objects with properties `"0"`, `"1"`, ..., not arrays, so
untyped code that expects an iterable (e.g. `new Map(Object.entries(o))`)
does not accept them.

### Objects

All object types in the typed language are _exact_ objects.
```
var obj: {a: number, b: string} = {a: 1, b: "xyz"};
```

They must have exactly the set of properties listed in their type (no extra properties allowed). Properties must be in the same order as the type requires. This allows the compiler to make access extremely efficient.

#### Variance

Object properties (and indexers, below) may carry a variance sigil:
```
var obj: {+ro: number, -wo: string} = {ro: 1, wo: "x"};
```
`+` or `readonly` makes a property read-only (covariant); `-` or `writeonly` makes it write-only (contravariant). The default (no sigil) is invariant: readable and writable, and subtyping requires the property type to match exactly.

#### Indexers

An object type may declare a single _indexer_ (index signature) instead of named properties, describing a dictionary with a uniform value type:
```
var dict: {[string]: number} = {};
var n: number = dict["x"];   // read
dict["y"] = 3;               // write
var m: number = dict.x;      // dot access works too
dict.y = 3;
```

An object type has at most one indexer, and an indexer cannot be combined with named properties. The key type is checked against the index expression.

Reads yield `T` but are checked by a checked cast at runtime (so a read of a missing key throws if `undefined` is not in `T`). Writes require the value type `T`.

Non-computed access (`dict.x`) routes through the indexer using a `string` key, so it requires a `string`-keyed indexer.

An object with named properties flows into a `string`-keyed indexer type only if the indexer is read-only, since `delete` through the indexer would break the properties' types. An object literal constrained to an indexer type is itself an indexer object.

Spreading an object that has an indexer produces an object with an indexer of the same key type. If the result also has named properties, they fold into the indexer and its value type becomes the union of all the value types:
```
var d: {[string]: number} = {};
var a = {...d};            // {[string]: number}
var b = {...d, x: "s"};    // {[string]: number | string}
```

A computed property (`{[k]: v}`) in an object literal also produces an indexer object: the key type is the union of all computed key types (plus `string` if named fields are present, plus any spread indexer key type), and the value type is the union of all value types. Mixing computed and named properties results in an indexer. If the literal is constrained to an indexer type, its key and value types are used.

```
var k: string;
var x = {[k]: 1};          // {[string]: number}
var n: number;
var y = {[n]: 1};          // {[number]: number}
```

### Functions

Function types can be quite complex:
```
type F = (this: string, x: number, y?: boolean) => void;
```

The type can contain the type of `this` inside the function, as well as optional arguments.

If a function has NO type annotations, it is considered to be an "untyped" function, and calls to it _will not be typechecked_. This allows for convenient calling to untyped JS from typed JS without extra effort or modification.

### Classes

Classes are declared as in JS:
```
class C {
  x: number;
  y: number;

  constructor(x: number, y: number) {
    this.x = x;
    this.y = y;
  }
}
```

Classes in the typed language are _nominally typed_. This means that two classes that happen to have the same field names cannot be used interchangeably.

Class methods can be decorated as final, meaning they cannot be overridden by a subclass:
```
class A {
  @Hermes.final
  foo(): number { return 1; }
}
class B extends A {
  foo(): number { return 2; } // ERROR
}
```

Methods and getter-only accessors are read-only; setter-only accessors are write-only.

`C.prototype` is the object holding the class's methods. It is read-only, and only methods that live on it are reachable: `@Hermes.final`, private and accessor members are stored in variables instead and cannot be read through it.
```
class A { foo(): number { return 1; } }
const f = A.prototype.foo; // OK
A.prototype.foo = f; // ERROR
```

A method read through `.prototype` is selected statically, not dispatched on the receiver.

Static members are stored in variables and resolved at compile time, including through `this` (the class) and `super` (the superclass) inside static members. To keep that consistent with JS:
* A subclass cannot redeclare an inherited static field or method.
* A static field can only be written through the name of the class that declares it, not through `this`, `super` or a subclass name, because JS would create a separate property on the receiver.
* A class declaration's name cannot be reassigned, so it always refers to exactly that class.
* A private static can only be accessed through the name of the class that declares it, because JS would throw for a subclass receiver.
* Static members cannot reference the class type parameters.
* Inside a generic class, a member access on the class name (`G.x`, `G.#x`) refers to the current specialization, which has its own static storage.
* `new this()` in a static method constructs the declaring class.
```
class A {
  static x: number = 1;
  static set(): void { A.x = 2; } // OK
  static bad(): void { this.x = 2; } // ERROR
}
class B extends A {
  static x: number = 3; // ERROR
}
B.x = 4; // ERROR
```

### Optional Types

The `?` indicates that a type may also be either `null` or `void`:
```
type A = ?number;
```

### Type Aliases

Types can be aliased using the `type` keyword:
```
type A = number;
```

### Generics

Classes and type aliases can both be generic.

```
class Wrapper<T> { x: T }
type NumberTuple<T> = [T, number];
```

#### Bounds

A type parameter may have a bound (`<T extends Bound>`). Each type argument must flow into the bound, otherwise it's an error. Bounds are checked for each specialization of a generic. A bound may reference any of the type parameters, including its own (`<T extends C<T>>`). Example:

```
function g<T extends Base, U extends T>(x: T, y: U): void {}
```

## Typechecking

### Compile-time Typechecking

The compiler will emit errors for any type errors which can be detected ahead of time.

### Runtime Typechecking

Certain operations will result in checked casts which must happen at runtime.

```
function foo(a: any) {
  var b: number = a; // Checked cast inserted, will throw if 'a' is not a number.
}
```

### Type Inference

#### Variable Declarations

If the declarator has an annotation, it wins.

Otherwise the initializer is visited and its type becomes the declaration's type. Globals and `catch` variables are always `any`.

Reassignments to a `let`/`var` variable must obey the type inferred/declared at the variable's declaration.

#### Array and Tuple Literals

An array literal becomes a tuple or an array based on the constraint:

* Tuple constraint: each element is checked against the matching slot.
* Array constraint: each element is checked against `T`, with checked casts inserted as needed.
* No constraint specified: element types are unioned and the literal becomes `Array<union>`.
* Empty literal, no constraint: Warns and assumes 'any'. Annotate the declaration for a proper type.

Tuples are never inferred without context — `[1,"x"]` alone is `Array<number|string>`, not a tuple.

#### Generic Calls

When a generic call omits type arguments, each type parameter is attempted to be inferred. Arguments are processed in two passes: first those whose parameter type is a bare type variable, then those with complex types like `(T) => U` from left to right. Inference succeeds only if every placeholder is resolved.

Type arguments may also be supplied partially, in which case only the unspecified trailing type parameters are inferred:

```
function makePair<T, U>(a: T, b: U): [T, U] { return [a, b]; }
makePair<number>(1, 'a');  // T = number (explicit), U = string (inferred)
```

Matching is structural: same-kind containers (`Array`, `Tuple`, `ExactObject`, function types) recurse into their components, a union constraint tries each arm against a non-union concrete, etc.

Limitations:

* First-write wins: `[T, T]` against `[number, string]` fixes `T = number`, and the second slot then fails the regular flow check.
* Single forward pass: fixpoint iteration.

### Type Refinement

Typed language doesn't have full proper type refinement support yet but it supports a partial implementation that allows for certain use cases by implicitly checking `null` or `void` at usage time.

```
function foo(a: number | void): number {
  var b: number = a; // Typechecks at compile time, but will throw if 'a' is not a number.
  return b;
}
```

### Decorations on Function Declarations

JS decorator syntax (`@Hermes.foo`) does not apply to function declarations, so the typed language uses a `Hermes.decorate(...)` call to attach decorations:

```
function charAt(this: string, pos: number): string { return this[pos]; }
Hermes.decorate(charAt, Hermes.builtin);
```

* The call must appear as a statement immediately following the `FunctionDeclaration` it decorates.
* The first argument must be an identifier matching the preceding function's name.
* The second argument must be the decorator.
* Exactly two arguments are required.

## JS Language Features

The typed language is a subset of JavaScript. Most common ES2015+ constructs work, with the notable exceptions listed under "Not yet supported" below.

### Function Parameters

Function parameters can be typed, optional, defaulted, or destructured.

```
// Typed parameter.
function f(x: number): void {}

// Optional parameter (must come last). Inside the body, type is `T | void`.
function g(x: number, y?: string): void {}

// Default parameter.
function h(x: number = 1): number { return x; }

// Object destructuring parameter, with optional default.
function i({x, y}: {x: number, y: string} = {x: 0, y: ""}): void {}

// Array (tuple) destructuring parameter, including nested patterns.
function j([a, b]: [number, string]): void {}
function k({a: {b}}: {a: {b: number}}): void {}
```

### Destructuring

Destructuring is supported in variable declarations and parameters, including nested patterns and default values. Type annotations inside destructuring patterns are not allowed; annotate the whole pattern instead.

```
const [x, y]: [number, string] = [1, "abc"];
const {a, b}: {a: number, b: number} = obj;
```

Destructuring names in object patterns can have defaults, but typed tuple and `Array<T>` patterns don't support defaults yet.

An unannotated `let`/`const` loop variable is typed from the loop: the element type for `for-of` over a typed array, `string` for `for-in`, and `any` otherwise. Loop targets are checked against that type. An annotated destructuring pattern must match it exactly (no implicit casts), since its leaves are read without a runtime check.

### Spread in Array Literals

Spread of an array into an array literal is supported.

```
const xs: number[] = [1, 2, ...other];
```

### Spread in Calls

Only a tuple can be spread into a call, because only a tuple has a width that
is known at compile time. The elements are read out individually and passed as
ordinary arguments, so the call is a direct call, and arity and element types
are checked as if the elements had been written out by hand.

```
function f(a: number, b: string): void {}
const t: [number, string] = [1, "abc"];
f(...t);          // same as f(t[0], t[1])
f(...t, ...([]: []));
```

This works in calls, method calls, `new`, `super()`, `fn.call()` and
`$SHBuiltin.call()`. In the last two, the `this` argument is consumed
positionally and cannot itself be a spread.

`Array.push()` is the exception: it is special-cased rather than treated as a
real function, so it takes an array spread and rejects a tuple one.

An array cannot be spread into a call: its length is not known at compile time,
so neither is the call's arity. An untyped callee accepts an array spread, but
not alongside a tuple spread, because tuples are not iterable.

An element whose type needs a checked cast to reach the parameter type is
rejected, because there is no argument expression to attach the cast to.

### Overloading

Class methods can be overloaded using the `@Hermes.overload` decoration. Overloaded methods must be final.
```
class C {
  @Hermes.final
  @Hermes.overload
  foo(x: number): string { return 'a'; }
  @Hermes.final
  @Hermes.overload
  foo(x: string): void {}
}
```

Overloads are resolved at compile time, and are not stored on the home object.

Both public and private methods may be overloaded, and an overload set may mix non-generic and generic overloads.

Restrictions:
* No overloading constructors/accessors
* Overloaded methods must be called (`var x = obj.overloadedMethod` is an error)

### Comparisons

Strict equality (`===` and `!==`) is type-checked. An expression `a === b` (or `a !== b`) is legal only when one operand's type flows into the other's without a checked cast.

Loose equality (`==` and `!=`) is generally disallowed. To compare two values of the same type, use `===` / `!==`. It exists for the `x == null` / `x == undefined` idiom used to test nullability. Specifically, `a == b` is allowed only when either operand has type `any`, or one operand has type `null` or `void` and `null` or `void` flows into the other type.

### Other Features

* Arrow functions (without type parameters).
* `for-of` loops over arrays.
* `try`/`catch`/`throw` (the catch binding is typed `any`).
* Template literals.
* Classes with static members, private fields and methods (`#name`),
  inheritance, `super` calls, and `super.method()` access.
* Getters and setters on classes (must be marked `@Hermes.final`).
* Rest parameters in function declarations and function types
  (`function f(...args)`).

### Not supported (yet)

* Spreading an array into a call (`f(...args)` where `args` is an array).
* Rest elements in tuple destructuring (`[a, ...rest]` on a tuple).
* Spread elements in tuple literals.
* `async` methods and generator methods.
* Computed property names in class definitions.
* Optional call expressions (`f?.()`).
* Type parameters on function expressions and arrow functions.
