---
name: add-ir-instruction
description: >
  Guide for adding a new IR instruction to the Hermes compiler. Use when the
  user asks to add, create, or define a new IR instruction (Inst/Instruction)
  in the Hermes intermediate representation. Covers all required files and
  the patterns for each.
---

# Adding a New IR Instruction to Hermes

When adding a new IR instruction, you must touch a specific set of files. This
skill describes each file, the pattern to follow, and important conventions.

## Checklist of Files to Modify

1. `doc/IR.md` — Documentation (the **only** place for doc-comments)
2. `include/hermes/IR/Instrs.def` — Instruction registration
3. `include/hermes/IR/Instrs.h` — Class definition (NO doc-comments here)
4. `include/hermes/IR/IRBuilder.h` — Builder declaration
5. `lib/IR/IRBuilder.cpp` — Builder implementation
6. `lib/IR/IRVerifier.cpp` — Verification logic
7. `lib/Optimizer/Scalar/TypeInference.cpp` — Type inference stub
8. `lib/BCGen/HBC/ISel.cpp` — HBC instruction selection (stub or implementation)
9. `lib/BCGen/SH/SH.cpp` — Static Hermes codegen (stub or implementation)
10. Tests — At minimum, update or add tests in `test/`

If the instruction needs lowering (i.e., it does not map directly to a bytecode
opcode), you also need:

11. `include/hermes/BCGen/Lowering.h` — Lowering pass declaration
12. `lib/BCGen/Lowering.cpp` — Lowering pass implementation
13. `lib/BCGen/HBC/LoweringPipelines.cpp` — Register pass in HBC pipeline
14. `lib/BCGen/SH/SH.cpp` (in `lowerModuleIR`) — Register pass in SH pipeline

## Step-by-Step Guide

### 1. Document in `doc/IR.md`

**This is the ONLY place to put documentation for the instruction.** Do NOT add
doc-comments to `Instrs.h`.

Add a markdown table entry in the appropriate section:

```markdown
### MyNewInst

MyNewInst | _
--- | --- |
Description | Brief description of what the instruction does.
Example |   `MyNewInst %arg1, %arg2 : type`
Arguments | *%arg1* is ... *%arg2* is ...
Semantics | Describe the semantics, referencing the spec where appropriate.
Effects | Describe side effects (e.g., "May read and write memory.", "May read memory and throw.", "Does not read or write memory.").
```

### 2. Register in `Instrs.def`

Add a `DEF_VALUE` entry. Place it near related instructions:

```cpp
DEF_VALUE(MyNewInst, Instruction)
```

If it's a subclass of another instruction, use the parent as the second argument.
If it's a terminator, use `TERMINATOR` instead of `DEF_VALUE`.

### 3. Define the class in `Instrs.h`

**Do NOT add doc-comments to this file.** Documentation belongs in `doc/IR.md`.

Follow this exact pattern:

```cpp
class MyNewInst : public Instruction {
  MyNewInst(const MyNewInst &) = delete;
  void operator=(const MyNewInst &) = delete;

 public:
  enum { Arg1Idx, Arg2Idx };
  explicit MyNewInst(Value *arg1, Value *arg2)
      : Instruction(ValueKind::MyNewInstKind) {
    // Optional assertions on operand types:
    // assert(arg2->getType().isSomeType() && "message");

    // Set the result type:
    setType(Type::createNoType());  // for instructions with no output
    // or: setType(Type::createFoo()); for typed instructions

    pushOperand(arg1);
    pushOperand(arg2);
  }

  explicit MyNewInst(
      const MyNewInst *src,
      llvh::ArrayRef<Value *> operands)
      : Instruction(src, operands) {}

  Value *getArg1() const {
    return getOperand(Arg1Idx);
  }
  Value *getArg2() const {
    return getOperand(Arg2Idx);
  }

  static bool hasOutput() {
    return false;  // true if the instruction produces a value
  }
  static bool isTyped() {
    return false;  // true if the output type is meaningful
  }

  SideEffect getSideEffectImpl() const {
    // Compose side effects. Common patterns:
    //   return {};                                          // pure
    //   return SideEffect{}.setReadHeap();                  // reads memory
    //   return SideEffect{}.setReadHeap().setWriteHeap();   // reads+writes
    //   return SideEffect{}.setThrow().setReadHeap();       // may throw + read
    return SideEffect{}.setThrow().setReadHeap();
  }

  static bool classof(const Value *V) {
    ValueKind kind = V->getKind();
    return kind == ValueKind::MyNewInstKind;
  }
};
```

### 4. Add IRBuilder declaration in `IRBuilder.h`

```cpp
MyNewInst *createMyNewInst(Value *arg1, Value *arg2);
```

### 5. Add IRBuilder implementation in `IRBuilder.cpp`

```cpp
MyNewInst *IRBuilder::createMyNewInst(Value *arg1, Value *arg2) {
  auto *inst = new MyNewInst(arg1, arg2);
  insert(inst);
  return inst;
}
```

### 6. Add verification in `IRVerifier.cpp`

Add a `visit` method that checks invariants:

```cpp
bool Verifier::visitMyNewInst(const MyNewInst &Inst) {
  AssertIWithMsg(
      Inst,
      Inst.getArg2()->getType().isSomeType(),
      "MyNewInst::Arg2 must be of SomeType");
  return true;
}
```

### 7. Add type inference in `TypeInference.cpp`

Add an `infer` method. For instructions without output, return `createNoType()`:

```cpp
Type inferMyNewInst(MyNewInst *inst) {
  return Type::createNoType();
}
```

### 8. Add code generation stubs

If the instruction is lowered before codegen, add fatal stubs. Otherwise,
implement the actual code generation.

**HBC ISel (`lib/BCGen/HBC/ISel.cpp`):**

```cpp
void HBCISel::generateMyNewInst(MyNewInst *Inst, BasicBlock *next) {
  hermes_fatal("MyNewInst should have been lowered.");
}
```

**SH (`lib/BCGen/SH/SH.cpp`):**

```cpp
void generateMyNewInst(MyNewInst &inst) {
  hermes_fatal("MyNewInst should have been lowered");
}
```

### 9. (If needed) Add a lowering pass

**Declare in `Lowering.h`:**

```cpp
/// Brief description of what the lowering does.
Pass *createLowerMyNewInst();
```

**Implement in `Lowering.cpp`:**

```cpp
Pass *hermes::createLowerMyNewInst() {
  class ThisPass : public FunctionPass {
   public:
    explicit ThisPass() : FunctionPass("LowerMyNewInst") {}

    bool runOnFunction(Function *F) override {
      IRBuilder builder{F};
      bool changed = false;

      // Collect instructions first to avoid iterator invalidation.
      llvh::SmallVector<MyNewInst *, 4> insts;
      for (auto &BB : *F) {
        for (auto &I : BB) {
          if (auto *MNI = llvh::dyn_cast<MyNewInst>(&I))
            insts.push_back(MNI);
        }
      }

      for (auto *MNI : insts) {
        // Replace MNI with lowered IR...
        MNI->eraseFromParent();
        changed = true;
      }

      return changed;
    }
  };
  return new ThisPass();
}
```

**Register in pipelines:**

In `lib/BCGen/HBC/LoweringPipelines.cpp` and in
`lib/BCGen/SH/SH.cpp` (`lowerModuleIR`), add `PM.addPass(createLowerMyNewInst());`
at the appropriate point (before any pass that would need to process instructions
introduced by the lowering).

### 10. Add or update tests

Add lit tests in the appropriate `test/` subdirectory. Use `%FileCheck` or
`%FileCheckOrRegen` to verify the IR output. If existing tests cover the
feature, update their expected output to reflect the new instruction.

Which subdirectory to use depends on how the instruction interacts with the
compiler pipeline — not every instruction needs tests in every directory:

- `test/IRGen/` — When the instruction is generated directly from JavaScript
  source. Tests here verify that IRGen produces the expected IR.
- `test/Optimizer/` — When the instruction affects or interacts with
  optimization passes.
- `test/BCGen/` — Not used as often, but if this IR instruction is used in
  combination with new bytecode instructions, then place bytecode gen tests here.

## Key Conventions

- **No doc-comments in `Instrs.h`.** All documentation goes in `doc/IR.md`.
  The class in `Instrs.h` should have no `///` or `/** */` comments describing
  what the instruction does. Brief inline comments explaining non-obvious
  implementation details (like side effects) are fine.
- **Placement matters.** Place the new instruction near related instructions
  in every file (e.g., private field instructions are grouped together).
- **Consistent naming.** The instruction name (e.g., `FooBarInst`) must be
  consistent across all files: `Instrs.def`, `Instrs.h`, `IRBuilder.h/cpp`,
  `IRVerifier.cpp`, `TypeInference.cpp`, and all codegen files.
- **The `ValueKind` is derived automatically.** When you add
  `DEF_VALUE(MyNewInst, Instruction)` to `Instrs.def`, the enum value
  `ValueKind::MyNewInstKind` is generated automatically.
