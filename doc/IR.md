---
id: ir
title: Design of the IR
---

### Introduction

This document is a reference manual for the Hermes High-level IR. The IR is a
Static Single Assignment (SSA) based representation that captures the JavaScript
language semantics. It features optional types (values may be annotated with
types).

The IR representation is designed to be used as an in-memory form. The IR can be
dumped to human readable assembly-like format.

### Well-Formedness

This section describes the rules that define a valid IR.

  - Instructions must be dominated by their operands.

  - Basic block must end with a terminator, which is a branch instruction or a
    return instruction.

  - There has to be exactly one terminator for each basic block.

  - AllocStack instructions must appear at the 'entry' basic block (the first basic
    block in the function).

  - PHI Nodes must appear at the beginning of the basic block and must contain
    an entry for each predecessor.

### Frames:

Every JavaScript function is a closure that can capture variables from its
declaration scope. The declaration scope may be the global scope or another
function. A closure is a pair of function code, which contains the code of the
function, and a context which saves the environment that the closure may access.

Function variables that are captured by a closure are allocated in a scope on
the heap and the scope is passed as part of the context to the closure.
Closures may access variables at all nest-levels, which means that they can
access multiple scopes. We implement this feature by nesting scopes and linking
them together. Each scope (the set of captured variables) also has a reference
to the scope of the caller function. Closures can access variables at different
nesting levels by loading the parent scope for each scope.

At the IR Level we define a few instructions that can load and store frame
variables.  Functions own variables, that are storage units that represent
variables at the source level.  These variables can be optimized away or
promoted to the stack, etc.  We define the AllocStack instruction for allocating
variable storage. The only instructions that can access the storage values
(Variable and AllocStack) are the Load and Store instructions. It is not
possible to save the address of the allocation itself. Depending on the
implementation of the virtual machine, the heap allocations may be packed into a
single frame.


### Types:

The Hermes high-level IR is optionally typed. Values may be annotated with the
expected type of the value. The types are optional and untyped programs are
correct. The types represent primitive JavaScript types, or refinement of these
types. All values can be annotated with types: Functions, Instructions and
Parameters. Annotations of functions represent the type of the returned value.
The type annotations are not a suggestion or a hint. They must be correct and
consistent or else the program may fail at run time. Programs are expected to
run and have the same semantics when the type annotations are stripped.

Some instructions in the IR expect operands of specific types, or produce
specific types.  For example, the CondBranch instruction expects the condition
operand to be annotated as a boolean type. The ToBool instruction produces
values that are annotated as boolean type.

The optimizer may optimize the IR based on type annotations. For example, if the
operand of the instruction ToBool is annotated with the boolean type then the
optimizer is free to remove the instruction and replace all uses with the
operand of the conversion instruction.

### Example:

This is a short example of a valid program in textual IR. The function contains
four basic blocks with a few instructions. The code below follow the
requirements of a well-formed function defined in the previous section.

      function forEach(cond : boolean, value : number)
        %BB0:
          %0 = BranchInst %BB1
        %BB1:
          %2 = CondBranchInst %cond, %BB2, %BB3
        %BB2:
          %3 = ReturnInst %cond
        %BB3:
          %4 = ReturnInst %value


### Side Effects:

The instructions in the IR are annotated with side effect labels. The
labels describe the kind of effect the instruction has on memory, IO
state (for example, hardware IO such as drawing to a screen or sending
a network packet), and whether they can throw. The two kind of memory
side effects are 'read' and 'write'.  ('write' actually indicates
"read and/or write".)  These side effect specifications indicate that
the instruction does not have IO effects or throw.  The 'unknown' side
effect is maximally conservative: it indicates that the instruction
may read, write, have IO effects, and/or throw.  These side effect
annotations allow the optimizer to decide which optimizations are
legal. For example, it is not legal to sink instructions that can
write to memory past each other. It is not legal to hoist instructions
that read to memory outside of a loop if there is another instruction
that writes to memory in that loop, because the writer instruction may
clobber memory that the reader may read.

Some instructions that allocate memory are marked as not having any side
effects.  This is because the instructions only touch the newly allocated memory
and do not influence the rest of the heap. Notice that we don't model the effect
of instructions on the garbage collector or anything like that, just the content
of the allocated memory.

## Register allocation

In Hermes, register allocation is performed on the IR. An infinite number of
virtual registers are allocated while preserving the constraints of the IR and
of the low-level target. Registers are allocated in an attempt to reduce the
number of registers, values across PhiNodes are coalesced, AllocStack
instructions are assigned with a dedicated register and arguments to call
instructions are placed in consecutive registers.

While in SSA form, the IR does not support all of the things that can be done
with registers. This is why the Register Allocator introduces MOV instructions
that represent a copy of one register to another. We lower some of the
load/store instructions into MOVs, and spill registers with MOV instructions.

## Generator Overview

When generating the IR for a generator function, we make two functions:
an outer GeneratorFunction and an inner function.
The outer function calls CreateGenerator on the inner function,
and returns the resultant generator.

An async function is the outer function of its own generator: it calls
CreateGenerator on the inner function and passes the resultant generator to
the `spawnAsync` builtin, which drives it.

The inner function assumes that it can store and retrieve state from
its own closure. As such, it contains instructions to start, save/yield,
and resume generators.

CreateGenerator:
An instruction to create a generator given a `Function`.
First, it creates an inner GeneratorInnerFunction, then it wraps it in a Generator object.
Used by the GeneratorFunction to create the generator which is returned upon calling it,
and by the AsyncFunction to create the generator it passes to `spawnAsync`.

StartGenerator:
Always the first instruction executed when an inner function is called.
Restores values for all local variables in the generator,
and jumps to the resume point of the generator if it's suspended.
If the generator hasn't been started yet, simply continues execution.

SaveAndYield:
Saves necessary state to the closure and yields execution.
In practice, this will save state and use the return opcode to
allow the caller to get the yield result.
Emitting a SaveAndYield also emits its corresponding ResumeGenerator.

ResumeGenerator:
Placed at the start of the block following the SaveAndYield to which it
corresponds. Resumes execution by loading context from the closure,
and then using state stored in the closure, does one of 3 things:
- Continues execution with the result of the instruction being the
  argument to `.next()`.
- Throws a value immediately
- Sets an `%isReturn` flag to true, which later instructions may branch on to execute the `finally`
  if necessary, and then return.

## Instruction semantics

This section describes the semantic of the instruction that are defined in the
IR. Please make sure to update this section as new instructions are added to the
compiler.


### BranchInst

BranchInst | _
--- | --- |
Description  | Jumps to a different basic block.
Example |  %0 = BranchInst %BB1
Arguments | A single operand which is the target basic block.
Semantics | Terminates a basic block and 'jumps' to a different basic block.
Effects | Does not read or write from memory.

### ReturnInst

ReturnInst | _
--- | --- |
Description | Leaves the function and returns a value.
Example |  %0 = ReturnInst %17
Arguments | A single operand which is the returned value. Notice the functions that return without an explicit value return the 'undefined' value.
Semantics | Terminates a basic block and transfer the control to the caller of the current function.
Effects | Does not read or write from memory.

### AllocStackInst

AllocStackInst | _
--- | --- |
Description | Allocates a variable on the stack.
Example |  %0 = AllocStackInst $name
Arguments | $name is the textual representation of the variable at the sourcecode level.
Semantics | AllocStack allocates a variable on the stack. Depending on the implementation of the VM, the variables may be packed into a single frame. AllocStack values may be used by instructions in different functions that represent closures created by the current functions. AllocStack values are used to represent local and captured variables. The AllocStack itself needs to be used directly. It is not possible to save a reference to the reference. The lifetime of the AllocStack may not exceed the lifetime of the allocating function.
Effects | Does not read or write from memory.

### LoadFrameInst

LoadFrameInst | _
--- | --- |
Description | Loads a value from a variable.
Example |  %1 = LoadFrameInst %scope, %variable
Arguments | %variable is a location in %scope to load from.
Semantics | The the instruction reads from a variable. The variable must be valid and found in the given scope.
Effects | Reads from memory.


### LoadStackInst

LoadStackInst | _
--- | --- |
Description | Loads a value from a stack allocated memory pointed by a reference.
Example |  %1 = LoadInst %0
Arguments | The address from which the instruction loads.
Semantics | The the instruction reads from memory. The address must be a valid stack address.
Effects | Reads from memory.

### StoreFrameInst

StoreFrameInst | _
--- | --- |
Description | Stores a value to a frame variable.
Example |  %1 = StoreFrameInst %scope, %value, %variable
Arguments | %value is the value to be stored. %variable is a variable in %scope where where the value will be stored.
Semantics | The the instruction saves a value to memory. The variable must be a real variable found in the scope.
Effects | Writes to memory.

### StoreStackInst

StoreStackInst | _
--- | --- |
Description | Stores a value to a stack allocated memory.
Example |  %1 = StoreStackInst %value, %stack_allocated
Arguments | %value is the value to be stored. %address is the reference to stack allocation.
Semantics | The the instruction saves a value to memory. The address must be a valid stack allocation.
Effects | Writes to memory.

### ToPropertyKeyInst

ToPropertyKeyInst | _
--- | --- |
Description | Converts a JavaScript value into a property key.
Example |  %1 = ToPropertyKeyInst %input
Arguments | The value to cast.
Semantics | Implements ES15 7.1.19 ToPropertyKey ( argument ).
Effects | May execute.

### AsNumberInst

AsNumberInst | _
--- | --- |
Description | Casts a JavaScript value into a number value.
Example |  %1 = AsNumberInst %input
Arguments | The value to cast.
Semantics | The instruction follows the JavaScript rules for converting types into numbers.
Effects | May read or write to memory.

### AsInt32Inst

AsInt32Inst | _
--- | --- |
Description | Casts a JavaScript value into a signed 32-bit integer value.
Example |  %1 = AsInt32Inst %input
Arguments | The value to cast.
Semantics | The instruction follows the JavaScript rules for converting types into 32-bit signed integers.
Effects | May read or write to memory.

### AsUint32Inst

AsUint32Inst | _
--- | --- |
Description | Casts a JavaScript value into an unsigned 32-bit integer value.
Example |  %1 = AsUint32Inst %input
Arguments | The value to cast.
Semantics | The instruction follows the JavaScript rules for converting types into 32-bit unsigned integers (equivalent to `>>> 0`).
Effects | May read or write to memory.

### AddEmptyStringInst

AddEmptyStringInst | _
--- | --- |
Description | Convert a value to string as if evaluating `value + ''`
Example |  %1 = AddEmptyStringInst %input
Arguments | The value to cast.
Semantics | The instruction follows the JavaScript rules for adding an empty string to a value (ES5.1 11.6.1).
Effects | May read or write to memory or throw.

### CreatePrivateNameInst

CreatePrivateNameInst | _
--- | --- |
Description | Create a globally unique private value, settings its internal [[Description]] to the given string literal. Practically speakig, this will result in a symbol value created at runtime with its [[Description]] set.
Example |  %1 = CreatePrivateNameInst %descString
Arguments | %descString is the string description of the name. It must be a string literal.
Semantics | Allocate a new globally unique private name.
Effects | Does not read or write to memory.

### CondBranchInst

CondBranchInst | _
--- | --- |
Description | Jumps to one of two blocks depending on a condition value.
Example | %1 = CondBranchInst %cond, %BB1, %BB2
Arguments | %cond is the condition variable, %BB1 is the 'True' block, %BB2 is the 'False' block.
Semantics | The instruction observes the value of a typed value and jumps to one of two basic blocks. If the condition is evaluated as 'True' the program jumps to the 'True' block. Otherwise the program jumps to the 'False' block.
Effects | Does not read or write from memory.

### HBCCompareBranchInst

HBCCompareBranchInst | _
--- | --- |
Description | Performs  a binary comparison of the two operands and a conditional branch depending on the result.
Example |  %0 = CompareBranch %x, %y, %BB1, %BB2
Arguments | %x and %y are the operands of the binary operation, %BB1 is the 'True' block, %BB2 is the 'False' block.
Semantics | The instruction follows the rules of JavaScript for each one of the binary operators defined in the instruction. If the condition is evaluated as 'True' the program jumps to the 'True' block. Otherwise the program jumps to the 'False' block.
Effects | May read and write memory.

### HBCFCompareBranchInst

HBCFCompareBranchInst | _
--- | --- |
Description | Performs a binary comparison of the two numbers and a conditional branch depending on the result.
Example |  %0 = HBCFCompareBranch %x, %y, %BB1, %BB2
Arguments | %x and %y are the operands of the binary operation, %BB1 is the 'True' block, %BB2 is the 'False' block.
Semantics | Performs a numeric comparison on the two doubles. If the condition is evaluated as 'True' the program jumps to the 'True' block. Otherwise the program jumps to the 'False' block.
Effects | Does not read or write to memory.

### HBCCmpBrTypeOfIsInst

HBCCmpBrTypeOfIsInst | _
--- | --- |
Description | Compare the type of a value against known types and branch.
Example |  %0 = HBCCmpBrTypeOfIsInst %val, %types, %trueBlock, %falseBlock
Arguments | %val is the argument to the typeof comparison, %types is a TypeOfIsTypes (see Typeof.h), %trueBlock and %falseBlock are the targets
Semantics | Branch to true if the type of %val matches the bit in %types, false otherwise.
Effects | Does not read or write memory.

### GetParentScopeInst

GetParentScopeInst | _
--- | --- |
Description | Retrieve the parent scope of the current function.
Example | %0 = GetParentScopeInst %varScope, %parentScopeParam
Arguments | %varScope is the VariableScope that describes the parent environment. %parentScopeParam is dummy parameter used to model usage of the parent environment.
Semantics | The instruction returns the enclosing environment of the currently executing function.
Effects | Does not read or write to memory.

### CreateScopeInst

CreateScopeInst | _
--- | --- |
Description | Creates a new scope which can be used to store variables.
Example | %0 = CreateScopeInst %variablescope, %parentScope
Arguments | %variablescope is a VariableScope describing the variables stored in the produced scope. %parentScope is the scope to use as the parent of the new scope, or EmptySentinel if the scope does not have a parent.
Semantics | The instruction creates a new scope which can be used to store/retrieve variables, and allow inner functions to access variables in an enclosing scope.
Effects | Does not read or write to memory.

### ResolveScopeInst

ResolveScopeInst | _
--- | --- |
Description | Traverse the scope chain to retrieve an enclosing scope.
Example | %0 = ResolveScopeInst %variablescope, %startVarScope, %startScope
Arguments | %variablescope is the VariableScope corresponding to the enclosing scope to retrieve. %startScope is the scope from which to start traversing. %startVarScope is the VariableScope associated with %startScope.
Semantics | The instruction retrieves the requested scope, which must be reachable from the starting scope.
Effects | Does not read or write to memory.

### LIRResolveScopeInst

LIRResolveScopeInst | _
--- | --- |
Description | Traverse the scope chain to retrieve an enclosing scope.
Example | %0 = LIRResolveScopeInst %variablescope, %startScope, %numLevels
Arguments | %variablescope is the VariableScope corresponding to the enclosing scope to retrieve. %startScope is the scope from which to start traversing. %numLevels is the number of levels to walk up the chain.
Semantics | The instruction retrieves the requested scope, which must be %numLevels up from the starting scope.
Effects | Does not read or write to memory.

### GetClosureScopeInst

GetClosureScopeInst | _
--- | --- |
Description | Retrieve the scope from the given closure.
Example | %0 = GetClosureScopeInst %varScope, %function, %closure
Arguments | %varScope is the VariableScope that describes the resulting scope. %function is the IR function that the closure operand is known to refer to. %closure is the closure from which to read the scope.
Semantics | The instruction returns the scope stored in the given closure.
Effects | Does not read or write to memory.

### CreateFunctionInst

CreateFunctionInst | _
--- | --- |
Description | Constructs a new function into the current scope from its code representation.
Example | %0 = CreateFunction %scope, %varScope, %function
Arguments | %function is the function that represents the code of the generated closure. %scope is the surrounding environment. %varScope is the VariableScope that describes %scope.
Semantics | The instruction creates a new closure that may access the lexical scope of the current function
Effects | Does not read or write to memory.

### CreateClassInst

CreateClassInst | _
--- | --- |
Description | Constructs a new class into the current scope from its constructor code representation.
Example | %0 = CreateClassInst %scope, %varScope, %function, %superClass, %homeObjectOutput
Arguments | %function is the function that represents the code of the constructor. %scope is the surrounding environment. %varScope is the VariableScope that describes %scope. %superClass is the class to inherit from; in base classes this is an empty sentinel value. %homeObjectOutput is an out parameter which will contain the home object (.prototype) of the class.
Semantics | The instruction creates a new class that may access the lexical scope of the current function, and an inherit from a given super class. (ES2023 15.7.14) This results in the creation of 2 objects: the class function object itself, and the "home" object. The home object is where methods are put. The home object can be found on the .prototype of the class, and the class can be found on the .constructor of the home object.
Effects | Writes to stack memory. May execute JS if it's a derived class.

### BinaryOperatorInst

BinaryOperatorInst | _
--- | --- |
Description | Performs the binary operation on the two operands.
Example |  %0 = BinaryOperatorInst %x, %y
Arguments | %x and %y are the operands of the binary operation.
Semantics | The instruction follows the rules of JavaScript for each one of the binary operators defined in the instruction.
Effects | May read and write memory.

### ImulInst

ImulInst | _
--- | --- |
Description | Computes `Math.imul(left, right)`: the signed 32-bit-wrapped product of `ToInt32(left)` and `ToInt32(right)`.
Example | %0 = ImulInst %left, %right
Arguments | %left and %right may be values of any type.
Semantics | Evaluates `ToInt32(%left)`, then `ToInt32(%right)`, in that order, exactly as `Math.imul` does. Each `ToInt32` may run arbitrary JS (e.g. a `valueOf`/`toString` method) and throws a TypeError if the operand is a BigInt or a Symbol. The result is the mathematical product of the two resulting int32 values, taken modulo 2^32 and reinterpreted as a signed 32-bit number.
Effects | If both operands are statically known to be of Number type, the instruction is idempotent and does not read or write memory. Otherwise it may execute arbitrary JS and throw.

### DirectEvalInst

DirectEvalInst | _
--- | --- |
Description | Implement direct eval.
Example |  `%0 = DirectEvalInst %%evalText, %strictCaller`
Arguments | %evalArg is the value which will be evaluated.
Semantics | Implement the semantics of ES6 `PerformEval(%evalText, evalRealm, strictCaller, direct=true)` (ES6 18.2.1.1).
Effects | Unknown

### CreateThisInst

CreateThisInst | _
--- | --- |
Description | Creates the object to be used as the `this` parameter of a construct call.
Example | %0 = CreateThisInst %closure, %newtarget, %functionCode
Arguments | %closure is the closure that will be invoked as a constructor, it may be any value, and the instruction will throw if it is not a valid callable. %newtarget is the new.target value to use for the call, it must either be the same value as %closure, or a valid callable. %functionCode is the IR function that is known to be associated with the closure operand, or EmptySentinel.
Semantics | The instruction is responsible for preparing the `this` parameter of a construct call. If %closure is a legacy class constructor, the instruction does not do anything and immediately produces undefined, since those constructors handle creating `this` internally. Otherwise, this the instruction determines the `new.target` to use for retrieving the `.prototype` (which may involve iterating over bound functions), reads the `.prototype` from it, and creates an object with that value as its parent.
Effects | May execute JS if %closure is not a legacy class constructor.

### CallInst

CallInst | _
--- | --- |
Description | Calls another function with some arguments.
Example | %0 = CallInst %callee, %target, %calleeIsAlwaysClosure, %env, %newtarget %this,  %arg0, %arg1, %arg2, ...
Arguments | %callee is the closure to execute. %target is either EmptySentinel or the only possible Function this call can invoke. %calleeIsAlwaysClosure indicates whether the callee needs to be checked for being a function. %env is the environment for the function or EmptySentinel. %newtarget is the new.target value to use for the call. %this is a reference to the 'this' value. Arguments %arg0 ... %argN are the arguments passed to the function.
Semantics | The instruction passes the control to the callee, that must be of closure type. The arguments are mapped to the parameters. Unmapped parameters are initialized to 'undefined'.
Effects | May read and write memory.

### CallBuiltinInst

CallBuiltinInst | _
--- | --- |
Description | Calls a builtin function passing "undefined" for this
Example | %0 = CallBuiltinInst %builtinNumber, %undefined, %undefined, %arg0, %arg1, %arg2, ...
Arguments | %builtinNumber is the builtin to execute. Arguments %arg0 ... %argN are the arguments passed to the function.
Semantics | The instruction passes the control to the builtin in a VM-specific way. The arguments are mapped to the parameters. Unmapped parameters are initialized to 'undefined'.
Effects | May read and write memory.

### CallIntrinsicInst

CallIntrinsicInst | _
--- | --- |
Description | Calls an unsafe compiler intrinsic, passing "undefined" for this
Example | %0 = CallIntrinsicInst %intrinsicsIndex, %undefined, %arg0, %arg1, %arg2, ...
Arguments | %intrinsicsIndex is the intrinsic to execute. Arguments %arg0 ... %argN are the arguments passed to the function.
Semantics | The instruction passes the control to the intrinsics in a VM-specific way. The arguments are mapped to the parameters.
Effects | May read and write memory.

### GetBuiltinClosureInst

GetBuiltinClosureInst | _
--- | --- |
Description | Get a closure of a builtin function
Example | %0 = GetBuiltinClosureInst %builtinNumber
Arguments | %builtinNumber is the builtin to return the closure of.
Semantics |
Effects | Reads from memory.

### LoadPropertyInst

LoadPropertyInst | _
--- | --- |
Description | Loads the value of a field from a JavaScript object.
Example |  %0 = LoadPropertyInst %object, %property
Arguments | %object is the object to load from. %property is the name of the field.
Semantics | The instruction follows the rules of JavaScript property access in ES5.1 sec 11.2.1. The operation GetValue (ES5.1. sec 8.7.1) is then applied to the returned Reference.
Effects | May read and write memory or throw.

### LoadOwnPrivateFieldInst

LoadOwnPrivateFieldInst | _
--- | --- |
Description | Loads the value of a private name from a JavaScript object.
Example |  %0 = LoadOwnPrivateFieldInst %object, %property : symbol
Arguments | %object is the object to load from. %property is the private name symbol.
Semantics | The instruction honors ES2024 7.3.30 PrivateGet. It looks up a private name on the object and throws if it does not exist. However, this does not have to deal with accessors or methods because those are stored outside of the instance.
Effects | May execute JS.

TryLoadGlobalPropertyInst | _
--- | --- |
Description | Loads the value of an existing field from the global object or throw if it doesn't exist.
Example |  %0 = TryLoadGlobalPropertyInst %object, %property
Arguments | %object is the global object. %property is the name of the field, which must be a string literal.
Semantics | Similar to LoadPropertyInst, but throw if the field doesn't exist.
Effects | May read and write memory or throw.

### LoadPropertyWithReceiverInst

LoadPropertyWithReceiverInst | _
--- | --- |
Description | Loads the value of a field from a JavaScript object.
Example |  %0 = LoadPropertyWithReceiverInst %object, %property, %receiver
Arguments | %object is the object to load from. %property is the name of the field. %receiver is receiver of the operation.
Semantics | The instruction implements ES2024 6.2.5.5 GetValue. This is used for `super` references.
Effects | May read and write memory or throw.

### DeletePropertyInst

DeletePropertyInst | _
--- | --- |
Description | Deletes the value of a field from a JavaScript object.
Example |  %0 = DeletePropertyInst %object, %property
Arguments | %object is the object to modify. %property is the name of the field.
Semantics | The instruction follows the rules of JavaScript property access.
Effects | May read and write memory.

### StorePropertyInst

StorePropertyInst | _
--- | --- |
Description | Stores a value to field in a JavaScript object.
Example |   %4 = StorePropertyInst %value, %object, %property
Arguments | %value is the value to be stored. %object is the object where the field %property will be created or modified.
Semantics | The instruction follows the rules of JavaScript property access in ES5.1 sec 11.2.1. The operation PutValue (ES5.1. sec 8.7.2) is then applied to the returned Reference.
Effects | May read and write memory or throw.


### StorePropertyWithReceiverInst

StorePropertyWithReceiverInst | _
--- | --- |
Description | Stores a value to field in a JavaScript object, with a specified receiver object.
Example |   %4 = StorePropertyWithReceiverInst %value, %object, %property, %receiver, %isStrict
Arguments | %value is the value to be stored. %object is the object where the field %property will be created or modified. %receiver will be the receiver operand when invoking the `[[Set]] ` internal method. If %isStrict is true, the operation will fail if there is an incompatible property descriptor found during the attempted store..
Semantics | This follows the same semantics as StorePropertyInst, except the receiver is explicitly specified.
Effects | May read and write memory or throw.

### TryStoreGlobalPropertyInst

TryStoreGlobalPropertyInst | _
--- | --- |
Description | Attempt to store a value into an existing field of the global object and throw if it doesn't exist.
Example |   %4 = TryStoreGlobalPropertyInst %value, %object, %property
Arguments | %value is the value to be stored. %object is the global object, where the field %property will be stored. %property must be a string literal.
Semantics | Similar to StorePropertyInst, but throw if the field doesn't exist.
Effects | May read and write memory or throw.

### DefineOwnPropertyInst

DefineOwnPropertyInst | _
--- | --- |
Description | Define an *own property* of JavaScript object. Will throw if the property write was not successful (e.g. trying to store to a non-writable property.)
Example |   %4 = DefineOwnPropertyInst %value, %object : object, %property, %enumerable : boolean
Arguments | %value is the value to be stored. %object *must* be of an object type; it's the object where the field with name %property will be created or modified. %enumerable determines whether a new property will be created as enumerable or not.
Semantics | Implements ES15 7.3.8 DefinePropertyOrThrow. The instruction follows the rules of JavaScript *own* property access. The property is created or updated in the instance of the object, regardless of whether the same property already exists earlier in the prototype chain.
Effects | May read and write memory.

### DefineOwnInDenseArrayInst

DefineOwnInDenseArrayInst | _
--- | --- |
Description | Define an *own property* in a dense JavaScript array at a specific index. This is an optimized version for dense array property assignment. Used only when populating newly created array literals.
Example |   %0 = DefineOwnInDenseArrayInst %value, %array : object, %arrayIndex : LiteralNumber
Arguments | %value is the value to be stored. %array is the dense array object where the property will be defined. %arrayIndex is a literal number representing the array index where the property will be stored.
Semantics | This instruction is used for efficient property definition. It requires that the array is dense and that the ArrayStorage underlying it has a size which is greater than the arrayIndex operand. The property is defined directly at the specified index without prototype chain traversal.
Effects | Writes to heap.


### StoreOwnPrivateFieldInst

StoreOwnPrivateFieldInst | _
--- | --- |
Description | Store a private *own property* to an object.
Example |   `%4 = StoreOwnPrivateFieldInst %value, %object, %property : symbol, %enumerable : boolean`
Arguments | *%value* is the value to be stored. *%object* should be an object, if not this throws. *%property* must be the value of a symbol. *%enumerable* must always be false.
Semantics | This instruction implements ES2024 7.3.31 PrivateSet. This instruction will throw if the private property does not already exist on %object.
Effects | May read and write memory.

### AddOwnPrivateFieldInst

AddOwnPrivateFieldInst | _
--- | --- |
Description | Add a new private own field.
Example |   `%4 = AddOwnPrivateFieldInst %value, %object, %property : symbol, %enumerable : boolean`
Arguments | *%value* is the value to be stored. *%object* must be an object. *%property* must be the value of a symbol. *%enumerable* should always be false.
Semantics | This instruction is used to initialize private fields on objects. As such it implements ES2024 7.3.27 PrivateFieldAdd- except this instruction will assume that the property being added does not already exist.
Effects | May read and write memory.

### PrivateBrandCheckInst

PrivateBrandCheckInst | _
--- | --- |
Description | Check that an object has a private brand installed.
Example |   `PrivateBrandCheckInst %object, %brand : privateName`
Arguments | *%object* is the object to check. *%brand* must be a private name representing the brand to look for.
Semantics | This instruction implements the brand check for private methods and accessors. It throws a TypeError if the brand is not present on the object.
Effects | May read memory and throw.

### DefineOwnGetterSetterInst

DefineOwnGetterSetterInst | _
--- | --- |
Description | Associates a pair of getter and setter with an *own* field in a JavaScript object, replacing the previous value.
Example |   %4 = DefineOwnGetterSetterInst %getter, %setter, %object, %property, %enumerable
Arguments | %getter is a getter accessor, or undefined. %setter is a setter accessor, or undefined. %object is the object where the field %property will be created or modified. %enumerable determines whether a new property will be created as enumerable or not.
Semantics | The instruction follows the rules of JavaScript property access. The property is created or updated in the instance of the object, regardless of whether the same property already exists earlier in the prototype chain. It replaces both accessors even if one or both of the parameters are undefined.
Effects | May read and write memory.

### AllocObjectInst

AllocObjectInst | _
--- | --- |
Description | Allocates a new JavaScript object on the heap.
Example |  `%0 = AllocObjectInst %sizeHint : LiteralNumber, %parent : EmptySentinel or null or Value`
Arguments | *%sizeHint% indicates that the object will need at least that many property slots. *%parent* is the optional parent to create the object with: *EmptySentinel* means use *Object.prototype*, *null* means no parent, or otherwise use the specified value.
Semantics | The instruction creates a new JavaScript object on the heap. If the parent is invalid (not EmptySenyinel, null or object), it is silently ignored.
Effects | Does not read or write to memory.

### AllocObjectLiteralInst

AllocObjectLiteralInst | _
--- | --- |
Description | Allocates a new JavaScript object on the heap. During lowering pass it will be lowered to either an AllocObjectInst or a LIRAllocObjectFromBufferInst.
Example |  %0 = AllocObjectLiteralInst %parent, "prop1" : string, 10 : number
Arguments | %parent is the parent of the new object. %prop_map is a vector of (Literal*, value*) pairs which represents the properties and their keys in the object literal.
Semantics | The instruction creates a new JavaScript object on the heap with an initial list of properties.
Effects | Does not read or write to memory.

### AllocTypedObjectInst

AllocTypedObjectInst | _
--- | --- |
Description | Allocates a new typed object on the heap. During lowering pass it will be lowered to either an AllocObjectInst or a LIRAllocObjectFromBufferInst.
Example |  %0 = AllocTypedObjectInst %parent, "prop1" : string, 10 : number
Arguments | %parent is the parent of the new object, and the other operands are alternating (Literal*, value*) pairs which represent the properties and their keys in the typed class.
Semantics | The instruction creates a new JavaScript object on the heap with an initial list of properties, which may include 'uninit' values. Properties are enumerable.
Effects | Does not read or write to memory.

### AllocTypedNonEnumObjectInst

AllocTypedNonEnumObjectInst | _
--- | --- |
Description | Allocates a new typed object on the heap with non-enumerable properties. Used for home objects (class prototypes with methods). During lowering pass it will be lowered to a LIRAllocTypedNonEnumObjectFromBufferInst.
Example |  %0 = AllocTypedNonEnumObjectInst %parent, "prop1" : string, 10 : number
Arguments | %parent is the parent of the new object, and the other operands are alternating (Literal*, value*) pairs which represent the properties and their keys in the typed class.
Semantics | The instruction creates a new JavaScript object on the heap with an initial list of non-enumerable properties, which may include 'uninit' values.
Effects | Does not read or write to memory.

### AllocArrayInst

AllocArrayInst | _
--- | --- |
Description | Allocates a new JavaScript array on the heap.
Example |  %0 = AllocArrayInst %sizeHint, %value0, %value1, ...
Arguments | sizeHint tells the size of the array that the VM should allocate. It must be equal or larger than the initial list of elements in this instruction. The rest of the values are all literal values as the initial elements of the array. Non-literal values or values after elision will be inserted into the array separately.
Semantics | The instruction creates a new JavaScript array on the heap with a hinted size and initial list of elements.
Effects | Does not read or write to memory.

### AllocFastArrayInst

AllocFastArrayInst | _
--- | --- |
Description | Allocates a new FastArray on the heap.
Example |  %0 = AllocFastArrayInst %capacity
Arguments | %capacity is a hint to the VM for how large the initial capacity should be.
Semantics | The instruction creates a new empty FastArray on the heap.
Effects | Does not read or write to memory.

### GetTemplateObjectInst

GetTemplateObjectInst | _
--- | --- |
Description | Gets the object to pass to the tagged template function.
Example |  %0 = GetTemplateObjectInst %templateObjID, %dup, %string1, ...
Arguments | %templateObjID is the cache key for the template. %dup indicates whether the raw strings are duplicates of the cooked strings. If %dup is true, then all %string operands are raw. Otherwise, some number of raw %string operands are followed by the _same_ number of cooked %string operands.
Semantics | If cached, retrieves from the template cache. Otherwise, updates the cache with a new template object with the cooked strings, which also contains an object containing the raw strings. Returns the resultant object.
Effects | Does not read or write to program memory. Updates template cache.

### TypeOfInst

TypeOfInst | _
--- | --- |
Description | The JS `typeof` operator
Example |  %0 = TypeOfInst %val
Arguments | %val is the value whose type we want to obtain.
Semantics | Obtains a string representing the type of the operand.
Effects | Does not read or write to memory.

### TypeOfIsInst

TypeOfIsInst | _
--- | --- |
Description | Compare the type of a value against known types
Example |  %0 = TypeOfIsInst %val, %types
Arguments | %val is the value whose type we want to compare, %types is the TypeOfIsTypes (see Typeof.h) to compare against.
Semantics | Returns true if the type of %val matches the bit in the TypeOfIsTypes, false otherwise.
Effects | Does not read or write to memory.

### CreateArgumentsInst

CreateArgumentsInst | _
--- | --- |
Description | Allocates the JavaScript `arguments` array-like object on the heap.
Example |  %0 = CreateArgumentsInst
Arguments | None.
Semantics | The instruction creates the `arguments` object, populates it with copies of the values of the arguments (according to "strict mode" semantics) and sets `arguments.length` to the number of arguments (`this` isn't copied or counted). There should be only one CreateArgumentsInst in a function.
Effects | Does not read or write to memory.

### CreateRegExpInst

CreateRegExpInst | _
--- | --- |
Description | Construct a RegExp object from a regexp literal.
Example |  %0 = CreateRegExpInst "pattern", "flags"
Arguments | `pattern: LiteralString` and `flags: LiteralString`
Semantics | It is equivalent to calling `RegExp(pattern, flags)`, except that it calls the built-in constructor, even if `RegExp` has been overridden.
Effects | Does not read or write to memory.

### SwitchInst

SwitchInst | _
--- | --- |
Description | The ‘switch‘ instruction is used to transfer control to one of different places.
Example |  %0 = SwitchInst %input, %default, [%val0, %block0], [%val1, %block1] ..
Arguments | The instruction accepts an input, a default block, and one or more pairs of value-destination values. The value must be a primitive JS type, and the destination must be a basic block within the current function.
Semantics | The semantic of the instruction is identical to a sequence of 'if' statements that compare the value of the input to each of the case statements. Repeating the same value is not allowed.
Effects | May read and write memory.

### GetPNamesInst

GetPNamesInt | _
--- | --- |
Description | Generates the property enumerator, which is a collection of registers that hold the state of the enumerator (iterator, object base, index, size, etc).
Example |  %0 = GetPNamesInt  %propertyAddr, %baseAddr, %indexAddr, %sizeAddr, %iteratorAddr, %onEmpty, %onLast
Arguments | The first 5 parameters are addresses (stack allocated addresses) that represent the state of the property enumerator. The last two argument are jump destination for the two cases: empty object and object with some properties.
Semantics | This instruction is a terminator instruction and prepares the enumerator for the GetNextPNameInst instruction to consume.
Effects | May read and write memory.

### GetNextPNameInst

GetNextPNameInst | _
--- | --- |
Description | Loads the next property from the object property enumerator.
Example |  %0 = GetNextPNameInst %propertyAddr, %baseAddr, %indexAddr, %sizeAddr, %iteratorAddr, %onLast, %onSome
Arguments | The first argument is the destination where the name of the property is written into. The next 4 arguments are the state of the property enumerator. The last two arguments are the destination blocks for: no next property, or some property available.
Semantics | This instruction is a terminator instruction that uses the state that was prepared by the GetPNamesInst instruction.
Effects | May read and write memory.

### CatchInst

CatchInst | _
--- | --- |
Description | This instruction catches an exception, and returns that exception.
Example | %0 = CatchInst
Arguments | This instruction does not have arguments.
Semantics | This instruction will be generated for each catch block and for each finally block. The current exception will be returned. CatchInst can only show up at the beginning of a basic block. The coverage and depth information for the CatchInst will be constructed dynamically later during bytecode generation.
Effects | May read and write memory.

### ThrowInst

ThrowInst | _
--- | --- |
Description | This instruction will throw an exception.
Example | %0 = ThrowInst %e, %catchTarget
Arguments | This instruction takes one required operand, which is the register that contains the exception value. The second operand is optional and indicates the closest surrounding catch block; it must be present if there is one, and must be missing otherwise.
Semantics | This instruction is a terminator instruction that will transition the control to the CatchInst that covers this instruction with closest scope.
Effects | May read and write memory.

### ThrowTypeErrorInst

ThrowTypeErrorInst | _
--- | --- |
Description | This instruction will create and throw a TypeError.
Example | %0 = ThrowTypeErrorInst %message
Arguments | This instruction takes one argument, %message, which will be converted to a string and used as the message for the TypeError.
Semantics | This instruction is a terminator instruction that will transition the control to the CatchInst that covers this instruction with closest scope.
Effects | Will throw.

### TryStartInst

TryStartInst | _
--- | --- |
Description | Mark the beginning of the try blocks.
Example | %0 = TryStartInst %catchTargetBlock, %tryBodyBlock
Arguments | This instruction takes 2 arguments: %tryBodyBlock is the block where the body of Try starts, %catchTargetBlock is the basic block that contains the CatchInst which covers this try. Both %tryBodyBlock and %catchTargetBlock are successors of this instruction.
Semantics | This is a nop, used only for tracking the beginning of try blocks.
Effects | Does not read or write memory.

### TryEndInst

TryEndInst | _
--- | --- |
Description | A terminator that marks the end of the try blocks.
Example | %0 = TryEndInst %CatchBlock, %BranchDest
Arguments | The active catch block and the destination block to branch to.
Semantics | This is a nop, used only for tracking the end of try blocks.
Effects | Technically this instruction itself does not touch memory, however we mark it as may write to prevent optimizations going pass this instruction.

### BranchIfBuiltinInst

BranchIfBuiltinInst | _
--- | --- |
Description | Check if a value is the same as a known builtin value.
Example | %0 = BranchIfBuiltin %builtinNumber, %arg, %trueDest, %falseDest
Arguments | The builtin number, the value to check, and the blocks to branch to.
Semantics | Branch to %trueDest if %arg is the same pointer as the builtin with %builtinNumber, else branch to %falseDest.
Effects | None

### PhiInst

PhiInst | _
--- | --- |
Description | This is a Phi node instruction.
Example |  %0 = PhiInst %value0, %block0, [%value1, %block1]
Arguments | A list of pairs of value and incoming basic block.
Semantics | The PhiNode needs to have a single entry for each incoming basic block of the block the PHI is located in. The incoming value must dominate the last instruction in the incoming block.
Effects | Does not read or write memory.

### MovInst

MovInst | _
--- | --- |
Description | The MOV inst represents a low-level operation of moving one register to another.
Example |  %0 = MovInst %value0
Arguments | Any value.
Semantics | The Mov instruction is only valid after Register Allocation in bytecode as we move away from SSA form.
Effects | Does not read or write memory.

### ImplicitMovInst

ImplicitMovInst | _
--- | --- |
Description | The ImplicitMov inst represents moving one register to another, except the mov will be performed implicitly by an immediately-subsequent instruction. This is used to express to the optimizer instructions which modify registers other than their destination.
Example |  %0 = ImplicitMovInst %value0
Arguments | Any value.
Semantics | The ImplicitMov instruction is only valid after Register Allocation in bytecode as we move away from SSA form.
Effects | Does not read or write memory.


### DebuggerInst

DebuggerInst | _
--- | --- |
Description | This instruction corresponds to the JavaScript `debugger` statement.
Example | %0 = DebuggerInst
Arguments | It takes no arguments and returns no values.
Semantics | Its behavior is implementation-dependent.
Effects | Does not read or write to memory.

### GetNewTargetInst

GetNewTargetInst | _
--- | --- |
Description | Obtains the value of `new.target` in the current function or constructor.
Example |  %0 = GetNewTargetInst, %param
Arguments | %param is a dummy JSDynamicParam used to quickly find usages of `new.target`.
Semantics | It must only be called from a ES6 class constructor or ES5 function. If the callee was invoked from `new`, it returns the function object of the direct constructor, otherwise `undefined`.
Effects | Does not read or write memory

### ThrowIfInst

ThrowIfInst | _
--- | --- |
Description | Check whether the value belongs to one of the "rejected" types, and if it is, throw ReferenceError, otherwise return it.
Example |  %_ = ThrowIfInst %value, %rejectedTypesUnion
Arguments | %value is the value to check. %rejectedTypesUnion is a union of types that should be rejected.
Semantics | It is used to implement ES6 TDZ functionality. Variables declared with `let` are *poisoned* with *empty* until they are initialized.
Effects | Potentially throws an exception. Has no other side effects.

### ThrowIfThisInitializedInst

ThrowIfThisInitializedInst | _
--- | --- |
Description | Check whether the value of `this` in a derived class constructor is already initialized. If so, throw.
Example |  %_ = ThrowIfThisInitializedInst %thisVal
Arguments | %thisVal is the value to check.
Semantics | This is used to guard against double-initialization of `this`, which happens by invoking `super()` multiple times.
Effects | Potentially throws an exception. Has no other side effects.

### CoerceThisNS

CoerceThisNS | _
--- | --- |
Description | Coerces its argument using the rules of "this" coercion to object in non-strict mode.
Example |  %0 = CoerceThisNS %value0
Arguments | Any value.
Semantics |
Effects | Does not read or write memory (it potentially creates a new object)

### CreateGenerator

CreateGenerator | _
--- | --- |
Description | Constructs a new GeneratorInnerFunction from its code representation, and wraps it in a Generator object.
Example | %0 = CreateFunction %scope, %varScope, %function
Arguments | %function is the function that represents the code of the generator's inner function. %scope is the surrounding environment. %varScope is the VariableScope that describes %scope.
Semantics | Creates a new GeneratorInnerFunction closure that may access the environment and wraps it in a generator
Effects | Does not read or write to memory (creates a new object).

### SaveAndYield

SaveAndYield | _
--- | --- |
Description | Saves information needed to resume generator execution and yield.
Example |  %0 = SaveAndYield %value, %isDelegated, %next
Arguments | %value is the value to yield, %isDelegated determines if the value to be yielded should be wrapped (true when inside a yield*), %next is the next BasicBlock to execute upon resuming, which must begin with a ResumeGeneratorInst (generated alongside SaveAndYield).
Semantics | Saves the frame variables and the next IP to the closure, and yield execution.
Effects | Reads and writes to memory, may throw or execute.

### ResumeGenerator

ResumeGenerator | _
--- | --- |
Description | Perform the user-requested action on resuming a generator.
Example |  %0 = ResumeGenerator %isReturn
Arguments | %isReturn is an output argument set to true if the user requested a return, false otherwise.
Semantics | If the user requested next(), continue on. If the user requested throw(), throw. If the user requested return(), set %isReturn to true and continue. Subsequent instructions will check %isReturn and execute any `finally` handlers, for example, before returning.
Effects | May read and write memory. (may throw)

### IteratorBegin

IteratorBegin | _
--- | --- |
Description | Begins array destructuring on a given iterable source.
Example |  %0 = IteratorBegin %sourceOrNext
Arguments | %sourceOrNext[in/out] is the stack location for source to destructure from. Is set to source if performing array iteration, else set to the `.next()` method of the iterator.
Semantics | If %sourceOrNext is an Array then it remains unmodified and the instruction returns `0`, but if it is not, it is replaced with the 'next' method so that it can be called on each step of the iteration and the instruction returns the iterator object. If the `[Symbol.iterator]` function throws, this instruction will throw.
Effects | May read and write memory, may throw or execute.

### IteratorNext

IteratorNext | _
--- | --- |
Description | Destructures the next value from a given iterator.
Example |  %0 = IteratorNext %iterator %sourceOrNext
Arguments | %iterator is the index or the iterator. %sourceOrNext is the input stack location (source to destructure from) or the next method.
Semantics | If %iterator is an index: if %iterator is less than `%sourceOrNext.length`, reads the value from %sourceOrNext and increments the index, else sets %iterator to undefined and returns undefined. If %iterator is an actual iterator, calls %sourceOrNext as a next method and evaluates to the result value. When iteration is complete, sets %iterator to undefined as a signal that we're done.
```
if (typeof %iterator === 'undefined') {
  return undefined;
}
if (typeof %iterator === 'number') {
  if (%iterator >= %sourceOrNext.length) {
    %iterator = undefined;
    return undefined;
  }
  return %sourceOrNext[%iterator];
}
var iterResult = %sourceOrNext();
if (iterResult.done) {
  %iterator = undefined;
  return undefined;
}
return iterResult.value;
```
Effects | May read and write memory, may throw or execute.

### IteratorClose

IteratorClose | _
--- | --- |
Description | Closes an iterator if it exists.
Example |  %0 = IteratorClose %iterator %ignoreInnerException
Arguments | %iterator is the index or the iterator. %ignoreInnerException is a boolean literal.
Semantics | If %iterator is an iterator, calls `.return()` on it to close it. Otherwise, this is a no-op. If `.return()` throws, the exception is ignored when %ignoreInnerException is true.
Effects | May read and write memory, may throw or execute.

### UnreachableInst

UnreachableInst | _
--- | --- |
Description | Crashes the VM (ifndef NDEBUG).
Example |  %0 = UnreachableInst
Arguments | None.
Semantics | Can be added to stubs and similar to verify that they are never executed.
Effects | Marked as reading/writing memory to avoid reordering.

## Target Instructions

Some high-level IR instructions are lowered into a sequence of low-level machine
instructions. In order to perform register allocation on these instructions we
need to perform lowering, which is a form of instruction selection. The semantic
of these instructions are identical to the semantic of the relevant target
instructions.

### LIRGetGlobalObjectInst

LIRGetGlobalObjectInst | _
--- | --- |
Description | Obtain the "global" object
Example |  %0 = LIRGetGlobalObjectInst
Arguments | None.
Semantics | The instruction returns a reference to the "global" object.
Effects | Does not read or write to memory.

### HBCCreateFunctionEnvironmentInst

HBCCreateFunctionEnvironmentInst | _
--- | --- |
Description | Create a new environment with the function's parent environment as its parent.
Example | %0 = HBCCreateFunctionEnvironmentInst %varScope, %parentScopeParam
Arguments | %varScope is the variable scope that this instruction will produce. %parentScopeParam is dummy parameter used to model usage of the parent environment.
Semantics | The instruction creates a new environment for a function.
Effects | Does not read or write to memory.

### HBCResolveParentEnvironmentInst

HBCResolveParentEnvironmentInst | _
--- | --- |
Description | Traverse the chain of environments starting at the current function's parent to find a given environment.
Example | %0 = HBCResolveParentEnvironmentInst %varScope, %numLevels, %parentScopeParam
Arguments | %varScope is the variable scope to resolve to. %numLevels is the number of scopes up from the current function's parent that the result will be found. %parentScopeParam is dummy parameter used to model usage of the parent environment.
Semantics | The instruction resolves an environment that is a parent of the current function's environment.
Effects | Does not read or write to memory.

### LIRAllocObjectFromBufferInst

LIRAllocObjectFromBufferInst | _
--- | --- |
Description | Allocates a new JavaScript object on the heap, and initializes it with values from the object buffer.
Example |  %0 = LIRAllocObjectFromBufferInst %parent, %value0, %value1, ...
Arguments | %parent is the parent of the new object. The subsequent values are all literal values, with alternating keys and values. Non-serializable values will be inserted into the array separately.
Semantics | The instruction creates a new JavaScript object on the heap with an initial list of properties.
Effects | Does not read or write to memory.

### LIRAllocTypedObjectFromBufferInst

LIRAllocTypedObjectFromBufferInst | _
--- | --- |
Description | Allocates a new typed class instance on the heap, and initializes it with values from the buffer. Properties are enumerable.
Example |  %0 = LIRAllocTypedObjectFromBufferInst %parent, %key0, %value0, ...
Arguments | The %parent is the parent of the result object. The remaining operands are all literal values, with alternating keys and values. Non-literal values will be inserted into the array separately.
Semantics | The instruction creates a new JavaScript typed object on the heap with an initial list of enumerable properties.
Effects | Does not read or write to memory.

### LIRAllocTypedNonEnumObjectFromBufferInst

LIRAllocTypedNonEnumObjectFromBufferInst | _
--- | --- |
Description | Allocates a new typed class instance on the heap with non-enumerable properties, and initializes it with values from the buffer. Used for home objects (class prototypes).
Example |  %0 = LIRAllocTypedNonEnumObjectFromBufferInst %parent, %key0, %value0, ...
Arguments | The %parent is the parent of the result object. The remaining operands are all literal values, with alternating keys and values. Non-literal values will be inserted into the array separately.
Semantics | The instruction creates a new JavaScript typed object on the heap with an initial list of non-enumerable properties.
Effects | Does not read or write to memory.

### HBCCallNInst

HBCCallNInst | _
--- | --- |
Description | Calls a function with a fixed number of arguments (from 1 to 4, inclusive).
Example | %0 = HBCCallNInst %callee, %this, %arg0, %arg1, %arg2
Arguments | %callee is the function to execute. %this is a reference to the 'this' value. Arguments %arg0 ... %argN are the arguments passed to the function.
Semantics | The instruction copies its arguments (starting from this) into the parameter-passing registers at the end of the frame, and passes the control to the callee, which must be of closure type. The arguments are mapped to the parameters. Unmapped parameters are initialized to 'undefined'.
Effects | May read and write memory.

### HBCCallWithArgCountInst

HBCCallWithArgCountInst | _
--- | --- |
Description | This instruction contains the same operands as CallInst, in addition to explicitly passing in the argument count as an operand.
Example | %0 = HBCCallWithArgCountInst %callee, %target, %calleeIsAlwaysClosure, %env, %newtarget, $argcount, %this, %arg0, %arg1, %arg2, ...
Arguments | %argcount is the number of arguments to the function, including 'this'.
Semantics | The instruction passes the control to the callee, that must be of closure type. The arguments are mapped to the parameters. Unmapped parameters are initialized to 'undefined'.
Effects | May read and write memory.

### prload

| prload      | _                                                                                                                                                                     |
|-------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Description | Load a typed object property by index                                                                                                                                 |
| Example     | `%0 = prload (:type) %object, %propIndex, %propName`                                                                                                                  |
| Arguments   | %object is the object to load from. %propIndex is the property index. %propName is the property name (which isn't actually used). `type` is the type of the property. |
| Semantics   | Load the property without any checking.                                                                                                                               |
| Effects     | May read memory.                                                                                                                                                      |

### prstore

| prstore     | _                                                                                                                                                                                                                                                                                                                         |
|-------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Description | Store a typed object property by index                                                                                                                                                                                                                                                                                    |
| Example     | `prstore %value, %object, %propIndex, %propName, %nonPointer`                                                                                                                                                                                                                                                             |
| Arguments   | `%value` is the value to store. `%object` is the object to store into. `%propIndex` is the property index. `%propName` is the property name (which isn't actually used). `%nonPointer` is a boolean indicating whether the property is a non-pointer type (meaning that both the old and the new value are not pointers). |
| Semantics   | Store the property without any checking.                                                                                                                                                                                                                                                                                  |
| Effects     | May read memory.                                                                                                                                                                                                                                                                                                          |

### FastArrayLoad

FastArrayLoad | _
--- | --- |
Description | Loads an element from a FastArray, validates the index.
Example | FastArrayLoad %array (:object), %index (:number)
Arguments | %array is the array, %index is the index to read
Semantics | Throw if the index is not an unsigned integer within range, otherwise, load the value at %index from %array.
Effects | May read memory or throw.

### FastArrayStore

FastArrayStore
--- | --- |
Description | Stores an element to an array, validates the index.
Example | FastArrayStore %storedValue, %array, %index
Arguments | %array is the array, %index is the index to write to, %storedValue is the value to write
Semantics | Throw if the index is not an unsigned integer within range, otherwise, store %storedValue to %array.
Effects | May write memory or throw.

### FastArrayLength

FastArrayLength | _
--- | --- |
Description | Read the length of a FastArray
Example | FastArrayLength (:number) %array
Arguments | %array is the array
Semantics | Read the length property of the array and return it.
Effects | May read memory.

### FastArrayPushInst

FastArrayPushInst | _
--- | --- |
Description | Pushes an element onto a FastArray.
Example | FastArrayPushInst %pushedValue, %array
Arguments | %pushedValue is the value to be pushed, %array is the array we are pushing onto.
Semantics | Push an element onto the target array, increasing its length by 1. If accommodating the additional element causes a reallocation past the maximum allowable allocation size, throw an exception.
Effects | May write memory or throw.

### FastArrayAppendInst

FastArrayAppendInst | _
--- | --- |
Description | Appends the elements of one FastArray to another.
Example | FastArrayAppendInst %other, %array
Arguments | %other is array from which elements will be copied, %array is the array onto which elements will be appended.
Semantics | Copy the elements from %other into the end of %array, increasing its length by the number of elements in %other. If accommodating the additional elements causes a reallocation past the maximum allowable allocation size, throw an exception.
Effects | May write memory or throw.

### LoadParentNoTraps

LoadParentNoTraps | _
--- | --- |
Description | Loads the parent for a legacy, ordinary untyped object. The given object cannot be proxy. Returns `null` or an object.
Example | %0 = LoadParentNoTraps %object
Arguments | %object must be of type object.
Semantics | Read the parent.
Effects | May read memory.

### TypedLoadParent

TypedLoadParent | _
--- | --- |
Description | Loads the parent (the vtable) for a typed object instance of a class
Example | %0 = TypedLoadParent %object
Arguments | %object is an instance of a typed class
Semantics | Read the parent without any checks.
Effects | May read memory.

### FUnaryMath

FUnaryMath | _
--- |-----------------------------------------------|
Description | Instruction class for a floating point unary math operation.
Example | FNegate (:number), %operand (:number)
Arguments | %operand is the input value
Semantics | Perform the specified op on a floating point number, return a number.
Effects | None

### FBinaryMath

FBinaryMath | _
--- |-----------------------------------------------|
Description | Instruction class for a floating point binary math operation.
Example | FAdd (:number), %left (:number), %right (:number)
Arguments | %left and %right are the input values
Semantics | Perform the specified op on two floating point numbers (same type), return a number.
Effects | None

### FCompare

FCompare | _
--- |-----------------------------------------------|
Description | Instruction class for comparing floating point numbers
Example | FLessThan (:bool), %left (:number), %right (:number)
Arguments | %left and %right are the input values
Semantics | Perform the specified compare on two floating point numbers (same type), return bool. Note that if any operand is NaN, comparison always returns false.
Effects | None

### StringConcat

StringConcat | _
--- |-----------------------------------------------|
Description | Concatenate `N` strings and return the resultant string.
Example | StringConcat (:string), %op1 (:string), ..., %opN (:string)
Arguments | %op1 to %opN are the input values which must be strings.
Semantics | Perform the specified concatenation on strings.
Effects | None

### HBCStringConcat

HBCStringConcat | _
--- |-----------------------------------------------|
Description | Concatenate 2 strings and return the resultant string.
Example | StringConcat (:string), %left (:string), %right (:string)
Arguments | %left and %right are the input values which must be strings.
Semantics | Perform the specified concatenation on strings.
Effects | None

### UnionNarrowTrusted

UnionNarrowTrusted | _
--- |-----------------------------------------------|
Description | Narrow a union type when the compiler has proven that the operand is more specific.
Example | UnionNarrowTrusted (:number), %operand (:number &vert; empty)
Arguments | %operand is the input value
Semantics | Narrow the type, but doesn't change the value.
Effects | None

### CheckedTypeCast

CheckedTypeCast | _
--- |-----------------------------------------------|
Description | Attempt to cast to the result type, throw if unable.
Example | CheckedTypeCast (:U), %operand (:T), %type
Arguments | %operand is the value to cast, %type is the type to cast to
Semantics | Cast from type `T` to `U`, throw when the cast is not valid. The result type may be narrower than the
specified cast type, if the compiler can prove that the input value is more specific.
Effects | May throw.

### NativeCall

NativeCall | _
--- |-----------------------------------------------|
Description | Call a native function.
Example | NativeCall (:type) %nativeFunctionPtr, %nativeSignature, %arg1, %arg2, ...
Arguments | %nativeFunctionPtr is the pointer to the native function. %nativeSignature is the signature of the native function. Arguments %arg1 ... %argN are the arguments passed to the function encoded as JS values.
Semantics | The JS arguments are converted to native types. The result is converted to the JS type.
Effects | Unknown.

### GetNativeRuntime

GetNativeRuntime | _
--- |-----------------------------------------------|
Description | Get a native pointer to the native runtime to be passed to a native function.
Example | GetNativeRuntime (:number)
Arguments | None
Semantics | Get the native runtime pointer.
Effects | None

### LIRDeadValue

LIRDeadValue | _
--- |-----------------------------------------------|
Description | Create a "dead value" of the specified type. This instruction is created during lowering in code that will never execute, but is needed to satisfy the type constraints of downstream instruction that will also never execute.
Example | LIRDeadValue (:number)
Arguments | None
Semantics | Create a value of the specified type.
Effects | None

### LazyCompilationData

NOTE: LazyCompilationData relies on the fact that we don't delete Variables during the lazy compilation pipeline. That means no stack promotion, and the existing full optimization pipeline cannot run, because we haven't yet figured out which variables are captured by child functions.

LazyCompilationData | _
--- |-----------------------------------------------|
Description | Data needed for lazy compilation, including the VariableScope chain used to access captured variables.
Example | LazyCompilationData %capturedThis, %capturedNewTarget, %capturedArguments, %capturedHomeObject, %parentVS, %parentParentVS, ...
Arguments | The captured values are used in case the lazy function may have arrow functions as children which need to capture the values as variables. In the case of %capturedHomeObject, this is needed even for non-arrow functions. %parentVS the immediately enclosing VariableScope. The remaining operands are successively the ancestors of %parentVS, which are kept as operands to ensure the VariableScopes aren't deleted across lazy compilation calls.
Semantics | Information needed for lazy compilation. Deleted after use.
Effects | None

### EvalCompilationData

NOTE: EvalCompilationData relies on the fact that we don't delete Variables during the eval compilation pipeline. That means no stack promotion, and the existing full optimization pipeline cannot run, because we haven't yet figured out which variables are captured by child functions.

EvalCompilationData | _
--- |-----------------------------------------------|
Description | Data needed for eval compilation within this function, including the VariableScope chain used to access captured variables.
Example | EvalCompilationData %capturedThis, %capturedNewTarget, %capturedArguments, %VS, %parentVS, ...
Arguments | The captured values are used in case the eval function may have arrow functions as children which need to capture the values as variables. %VS the VariableScope for the function (block scoping not supported). The remaining operands are successively the ancestors of %VS, which are kept as operands to ensure the VariableScopes aren't deleted across compilation calls.
Semantics | Information needed for eval compilation.
Effects | None
