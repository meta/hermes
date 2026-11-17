---
id: 01a1cea0-0d4c-7e2a-aa56-c2931ddde2bc
title: Implicit constructor forwards an inherited rest parameter as one argument
type: bug
status: closed
resolution: fixed
component: compiler
assignee: null
created: 2026-11-12T07:19:18.093Z
creator: Tzvetan Mikov <tmikov@gmail.com>
---

A typed class without an explicit constructor forwards the parameters of
the nearest ancestor's explicit constructor (`genTypedImplicitConstructor`,
`lib/IRGen/ESTreeIRGen-typed-class.cpp`). It creates one `JSDynamicParam`
per ancestor parameter, including the rest parameter, and passes each as
one argument. A rest parameter therefore receives only the first
argument:

    class A { s: number; constructor(...xs: number[]) { this.s = xs.length; } }
    class B extends A {}
    print(new B(1, 2, 3).s);   // prints 1, expected 3 (-O0 and -O)

The forwarded optional parameters are also typed without `undefined`
(the loop sets `flowTypeToIRType(param.type)`, while `emitParameters`
adds `undefined` for optional parameters), so a missing argument is
trusted as the declared type. This is observable at `-O`: the ancestor
constructor is inlined into the implicit one, its `x === undefined` check
on the parameter folds to false, and a `number` field ends up holding
`undefined`.

Found by an external (Codex) review of the typed FunctionCode design
(`doc/plans/ir-type/typed-functioncode-design.md`), whose synthesized
implicit-constructor signature must match the forwarding. Fix: forward
the rest arguments (e.g. copy them into an array and spread them into the
super call) and include `undefined` in optional parameter types, matching
`emitParameters`.

## Log

- 2026-11-12T07:19:18.093Z  Tzvetan Mikov <tmikov@gmail.com>  created
- 2026-11-12T08:00:18.521Z  Tzvetan Mikov <tmikov@gmail.com>  status: open -> closed (fixed)
    Fixed by "Fix implicit typed ctor param forwarding": with a rest parameter, genTypedImplicitConstructor copies all arguments with copyRestArgs and applies them to the super constructor with applyWithNewTarget; forwarded optional parameters include undefined. The wrong optional type was observable at -O: the inlined undefined check folded to false
