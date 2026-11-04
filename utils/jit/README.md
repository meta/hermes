# JIT dump tools

Tools for checking that a change to the arm64 or x86-64 JIT emitter did not
change the code it emits.

- `jit-dump.sh` — capture a canonicalized dump of all JIT-emitted code from
  one `hermes` binary.
- `jit-diff.sh` — capture from two binaries and compare, reporting comment
  changes separately from instruction changes.

Both tools work unmodified on either architecture: they take a `hermes`
binary, not a target triple, and the canonicalizer's rules are keyed on the
disassembly syntax each backend actually emits, so the arm64 and x86-64
rules coexist without a flag. See "x86-64" below for what differs.

## Why not just diff `-Xdump-jitcode` output?

Because two runs of an **unchanged** binary produce different dumps.

On arm64, `Emitter::loadBits64InGp` asks `isCheapConst()` whether a 64-bit
value has at most two non-zero 16-bit halfwords. If it does, the value is
materialized inline with `mov`/`movk`; if not, it is spilled to the
read-only data section and loaded with `ldr`. JIT-emitted code bakes in
runtime pointers, and ASLR moves those every run. So the emitter genuinely
selects *different instructions* from one run to the next, and every
subsequent RO-data offset shifts along with them.

You can see this for yourself:

```sh
utils/jit/jit-dump.sh --raw -o /tmp/a.txt build/bin/hermes
utils/jit/jit-dump.sh --raw -o /tmp/b.txt build/bin/hermes
diff -u /tmp/a.txt /tmp/b.txt | grep -cE '^[-+]'
```

That reports roughly 3,500 changed lines out of about 90,000 — from the same
binary, twice, with no code change at all. Disabling ASLR (running under lldb)
cuts it to a few dozen but does not eliminate it, because the contiguous
heap's base address comes from a separate randomized mmap.

`jit-dump.sh` therefore collapses constant materialization to a `CONST` token,
drops the RO-data contents, and normalizes any remaining wide hex literal to
`ADDR`. What survives is compared verbatim: instructions, registers, branches,
labels, ordering, and the emitter's own comments. In practice that is about
92% of the dump, and the same command without `--raw` reports zero
differences.

## x86-64

x86-64 needs *less* canonicalization than arm64, not more. Its
`loadBits64InGp` (`lib/VM/JIT/x86-64/JitEmitter.h`) is an unconditional `mov
reg, imm64` — there is no `isCheapConst()` split and no RO-data fallback for
GP constants, so the mechanism that makes two arm64 runs of the *same*
binary diverge is simply absent for those loads. What still varies run to
run is only the immediate itself: runtime/heap pointers ASLR moves (call
targets, `runtimeModule`, `codeBlock_`, property-cache addresses, ...).

Checked against a real x86-64 dump (two-run self-diff over `test/jit/*.js`
and `test/jit/x86-64/*.js`): every line that differs between two runs has
the shape `mov reg, 0x<8+ hex digits>`, and the pre-existing trailing rule
(`s/0x[0-9A-Fa-f]{8,}/ADDR/g`) already collapses all of them. No dedicated
"mov reg, imm -> CONST reg" rule was added for x86-64: it would be
redundant with that trailing rule for everything that actually varies, and
adding one risked *hiding* real diffs behind a hex constant a future
refactor makes short. Small hex immediates that use the same `mov reg,
0x..` shape — type tags, packed kind/size headers, bytecode-IP deltas — are
deliberately left verbatim: they are not ASLR-dependent, so a real change to
them should show up as a diff.

That only holds under 8 hex digits, though. The trailing `ADDR` rule
matches on width, not on origin, so a `mov reg, imm64` whose immediate
happens to print at 8+ hex digits — a 64-bit NaN-boxing tag mask, or an
IEEE 754 double's bit pattern loaded via `loadBits64InGp` — is collapsed
to `ADDR` exactly like a real ASLR-moved pointer. A changed tag mask or
double constant is therefore invisible in the instruction line; it
surfaces only as a diff in that line's comment (if the emitter comments
the constant), which `--comments-ok` explicitly suppresses. There is no
rule that distinguishes "wide because ASLR" from "wide because it is a
genuine 64-bit constant" — see "Limitations" below.

x86-64 does still spill values (doubles, property-cache pointers) to a
RO_DATA section, addressed from code as `[RO_DATA]` / `[RO_DATA+N]` (a
stable label + offset, no embedded hex) with asmjit's own `.dq 0x...`
listing at the end of the function — the syntactic analogue of arm64's
`.xword`. `jit-dump.sh` drops those listing lines (`/^\.dq /d`) the same way
it drops `.xword`.

Net effect: run `jit-dump.sh` / `jit-diff.sh` on an x86-64 binary exactly as
you would on arm64 — same commands, same `test/jit` corpus, same zero-diff
expectation for an unchanged binary. `cmake-build-x86jit/jit-baseline.dump`
is the x86-64 analogue of arm64's `cmake-build-arm64/jit-baseline.dump`.

## Usage

Both tools take **binaries**, not a build directory, so they work regardless
of how you built.

```sh
# Build the "before" binary and stash it somewhere.
cmake --build build --target hermes
cp build/bin/hermes /tmp/hermes-before

# Make your change, rebuild, and compare.
utils/jit/jit-diff.sh /tmp/hermes-before build/bin/hermes
```

Typical output for a refactor that should be behaviour-preserving:

```
jit-diff: dumps are identical
```

and for one that touches only `Emitter::comment()` strings:

```
jit-diff: 20 changed lines (+10 / -10)
  comment lines:     20
  instruction lines: 0
RESULT: comments only, no instruction changed
```

`--comments-ok` makes that second case exit 0, which is useful when a change
deliberately edits a comment string.

If the "before" binary is gone but you kept its capture, compare the dumps
directly:

```sh
utils/jit/jit-diff.sh --dumps /tmp/before.txt /tmp/after.txt
```

To capture a single dump, for example to read one function by hand:

```sh
utils/jit/jit-dump.sh --raw -c test/jit/binops.js build/bin/hermes | less
```

`--raw` skips canonicalization. Use it for reading, never for comparing.

## Corpus

By default the corpus is every `test/jit/*.js` plus three typed tests that
exercise paths the JIT tests do not:

- `test/shermes/array-typed.js`
- `test/hermes/flow/array-for-of.js`
- `test/hermes/flow/nbody.js`

Each `test/jit` file is run typed if and only if its own lit `RUN:` line
passes `-typed`. That matters more than it sounds: running a typed test
untyped does not just lose coverage, it fails to compile, so the file
contributes a syntax error to the dump instead of any JIT code.

Override with `-c FILE` (untyped) and `-t FILE` (compiled with `-typed`),
both repeatable. Supplying either replaces the whole default corpus.

A corpus file that exits nonzero aborts the capture. Its diagnostics would
otherwise land in the dump, where they are indistinguishable from emitted
code and will happily compare equal between two runs.

## Limitations

**Canonicalized lines are not compared.** A change that alters *which*
instruction form is chosen for a constant is invisible to these tools, because
that is exactly what gets collapsed to `CONST`/`ADDR` or dropped. On arm64,
if your change touches `isCheapConst`, `loadBits64InGp`, `uint64Const`, or
RO-data layout, these tools cannot verify it. On x86-64 the exposure is
narrower — `loadBits64InGp` has no split to hide — but the `.dq` RO-data
listing is dropped outright, so a change to an RO-data constant
(`uint64Const`/`roConst64`: the 0.0, ±1.0 and -0.0 doubles as well as the
cache pointers) or to the RO-data layout is invisible on x86-64 too.
The trailing `ADDR` rule is also width-only, not origin-aware: any x86-64
`mov reg, imm64` whose immediate is 8+ hex digits — a tag mask, a double's
bit pattern, any other genuine 64-bit constant, not just an ASLR-moved
pointer — is collapsed the same as a real pointer, so a change to one of
those is invisible on the instruction line and shows up only as a
comment-line diff (which `--comments-ok` suppresses).

**Coverage is only as good as the corpus.** An emitter path no test reaches
will not appear in the dump. Notably, every function in the default corpus has
well under 4 KB of bytecode, so paths gated on large bytecode offsets — such as
the two-instruction `add` in `Emitter::getBytecodeIP` — are never exercised.
Add a targeted file with `-c` when working on one of those.

**A clean diff is not a correctness proof.** It shows the emitted code did not
change, which is the right check for a refactor. It says nothing about whether
the code was correct to begin with, and nothing about the emitter's own runtime
behaviour. Run the lit suite too.

**The JIT must be enabled** in the binaries under test
(`-DHERMESVM_ALLOW_JIT=1`); the tools force it on per-run with `-Xjit=force
-Xjit-threshold=1`, but cannot enable a compile-time-disabled JIT. If a binary
was built without it, `jit-dump.sh` fails with a message saying so rather than
silently producing an empty dump.
