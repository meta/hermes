# Principles for specs and plans

Distilled from review findings on the polymorphic GetById spec and plan
(doc/superpowers/specs/2026-10-14-jit-getbyid-polymorphic-design.md).

1. **No claim about existing code without reading it.** Cite file:line
   for every statement of how the code behaves. Check every plan
   snippet against the real declarations: copy semantics, access,
   signatures.
2. **List safepoints and reentry after anything held across a call.**
   Root values that cross a GC point; never compare raw pointers across
   one. A helper that can run JS or compile revalidates all its state
   afterwards. For each weak reference, ask what the record means if
   only it is cleared; encode kinds explicitly, not by nullness.
   SH-ABI code throws by longjmp (`_sh_throw_current`), which skips C++
   destructors: never let an RAII registration (`LocalsRAII`,
   `GCScope`) span a call that can longjmp; close the scope first, as
   the slow paths in `lib/VM/StaticH.cpp` do. VM code that returns
   `ExecutionStatus`/`CallResult` does not longjmp.
3. **Before calling a check unnecessary or unobservable, try to build
   the counterexample.**
4. **Extending a state machine: keep its invariants and trace every
   input class through it** (cacheable, uncacheable, throwing, overflow,
   exactly at the boundary). Flags mark transitions, not repeats.
5. **Inserting code before existing code: write down the register and
   label contract at each join point**, and check for emit-once helpers.
6. **Every test states what it fails on, and that failure is checked.**
   - It fails before the feature.
   - Its mutation is not masked by another guard.
   - Its observable distinguishes the claim (emission vs. value).
   - Nothing unrelated feeds its counters. Global reads inside functions
     are GetById sites; wrap the test in `main()` and pass functions as
     parameters.
   - Expected numbers are derived in writing; environmental assumptions
     (shared hidden classes, allocations happening) are verified.
7. **Self-review adversarially before external review:** for each
   claim, where in the code does it hold, and what input breaks it?
