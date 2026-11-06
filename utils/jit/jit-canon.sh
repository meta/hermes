#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Canonicalize a raw `-Xdump-jitcode` dump on stdin, writing the
# canonicalized text to stdout.
#
# This is the single source of truth for JIT dump canonicalization. It is
# consumed by utils/jit/jit-dump.sh (its canonicalize() calls this script)
# and by test/jit/recompile-deterministic.js (whose RUN lines pipe raw
# -Xdump-jitcode output through it directly, in place of a plain
# `sed 's/0x[0-9A-Fa-f]*//g'`).
#
# The canonicalization is not optional for comparison purposes; see
# utils/jit/README.md for why two runs of an *unchanged* binary produce
# different raw dumps.
#
#  - Collapse constant materialization to a CONST token. isCheapConst() picks
#    between mov/movk and an RO-data load based on the *value*, and JIT code
#    bakes in runtime pointers that ASLR moves every run.
#  - Drop the RO_DATA contents, whose layout shifts with the above.
#  - Normalize any remaining wide hex literal to ADDR.
#
# x86-64 needs less of this than arm64, not more. loadBits64InGp() there is
# an unconditional `mov reg, imm64` (see JitEmitter.h) -- there is no
# isCheapConst() split and no RO-data fallback for GP constants, so the
# mechanism that makes two arm64 runs of the *same* binary diverge is simply
# absent for those loads. What still varies run to run is only the immediate
# itself (runtime/heap pointers moved by ASLR: call targets, `runtimeModule`,
# `codeBlock_`, property-cache addresses, ...), and every one of those prints
# as >= 8 hex digits (confirmed against a real x86 dump: two-run self-diffs
# over test/jit/*.js and test/jit/x86-64/*.js are 100% lines of the form
# `mov reg, 0x<8+ hex digits>`, nothing shorter). The existing trailing
# `s/0x[0-9A-Fa-f]{8,}/ADDR/g` rule already collapses all of them; small hex
# immediates that use the same `mov reg, 0x..` shape (type tags, packed
# kind/size headers, bytecode-IP deltas) stay verbatim on purpose because
# they are not ASLR-dependent and a real change to them should show up as a
# diff. No x86-specific "mov reg, 0x... -> CONST reg" rule is added: it would
# be redundant with the trailing rule for everything that actually varies.
#
# Caveat: the trailing ADDR rule matches on digit width, not on origin, so
# it also collapses any mov reg, imm64 whose immediate happens to print at
# 8+ hex digits for a non-ASLR reason -- a 64-bit tag mask, an IEEE 754
# double's bit pattern -- exactly like a real pointer. A changed constant
# of that kind is invisible on the instruction line; it surfaces only as a
# comment-line diff, which --comments-ok suppresses. See
# utils/jit/README.md ("Limitations") for the full explanation.
#
# x86 does still spill values (doubles, property-cache pointers) to a
# RO_DATA section, addressed as `[RO_DATA]` / `[RO_DATA+N]` with asmjit's own
# `.dq 0x...` listing at the end of the function -- the syntactic analogue of
# arm64's `.xword`. The reference sites carry no embedded hex (the label and
# offset are stable), so only the listing needs dropping.

set -u
set -o pipefail

sed -E \
    -e 's/^( *)mov (x[0-9]+), 0x[0-9A-Fa-f]+$/\1CONST \2/' \
    -e 's/^( *)ldr (x[0-9]+), \[RO_DATA(, [0-9]+)?\]$/\1CONST \2/' \
    -e 's/^( *)ldr (d[0-9]+), \[RO_DATA(, [0-9]+)?\]$/\1CONST \2/' \
    -e '/JIT total memory usage/d' \
    -e '/^\.xword /d' \
    -e '/^\.dq /d' \
    -e '/^\/\/ Bytecode start$/d' \
    -e '/^\/\/ RuntimeModule$/d' \
    -e 's/0x[0-9A-Fa-f]{8,}/ADDR/g' \
| awk '/^RO_DATA:$/ {inro=1; next} inro && /^\/\// {next} inro {inro=0} {print}'
