#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Capture a canonicalized dump of all JIT-emitted code over a corpus of JS
# files, suitable for byte-for-byte comparison between two builds.
#
# The canonicalization is not optional for comparison purposes; see
# utils/jit/README.md for why two runs of an *unchanged* binary produce
# different raw dumps.

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

usage() {
  cat <<'EOF'
Usage: jit-dump.sh [options] <hermes-binary>

Runs <hermes-binary> over a corpus of JS files with the JIT forced on,
dumping every emitted function, and canonicalizes the output so that two
captures can be compared byte for byte.

Options:
  -o FILE        Write the dump to FILE (default: stdout).
  -c FILE        Add FILE to the corpus, run untyped. Repeatable.
  -t FILE        Add FILE to the corpus, run with -typed. Repeatable.
                 If neither -c nor -t is given, the default corpus is used:
                 test/jit/*.js plus a few typed tests.
  --raw          Skip canonicalization. Useful for reading a single dump,
                 useless for comparing two.
  -q             Do not print the summary line to stderr.
  -h, --help     Show this help.

Examples:
  # Capture from a CMake build.
  utils/jit/jit-dump.sh -o /tmp/before.txt ~/build/bin/hermes

  # Compare two builds (see also jit-diff.sh).
  utils/jit/jit-dump.sh -o /tmp/after.txt ./bin/hermes
  diff -u /tmp/before.txt /tmp/after.txt

  # Just one file, raw, to read by hand.
  utils/jit/jit-dump.sh --raw -c test/jit/binops.js ./bin/hermes
EOF
}

OUT=""
RAW=0
QUIET=0
UNTYPED=()
TYPED=()

# Abort rather than loop forever when an option is given without its operand.
need_arg() {
  if [ "$2" -lt 2 ]; then
    echo "jit-dump: option $1 requires an argument" >&2
    exit 2
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    -o) need_arg -o $#; OUT="$2"; shift 2 ;;
    -c) need_arg -c $#; UNTYPED+=("$2"); shift 2 ;;
    -t) need_arg -t $#; TYPED+=("$2"); shift 2 ;;
    --raw) RAW=1; shift ;;
    -q) QUIET=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "jit-dump: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *) break ;;
  esac
done

if [ $# -ne 1 ]; then
  echo "jit-dump: expected exactly one hermes binary" >&2
  usage >&2
  exit 2
fi
HERMES="$1"

if [ ! -x "$HERMES" ]; then
  echo "jit-dump: not an executable: $HERMES" >&2
  exit 2
fi

# Default corpus: every JIT test, plus a few typed tests that exercise paths
# the JIT tests do not.
#
# A JIT test is run typed iff its own lit RUN line passes -typed. Running a
# typed test untyped does not merely reduce coverage: it fails to compile, and
# the corpus then contributes a syntax error instead of any JIT code.
if [ ${#UNTYPED[@]} -eq 0 ] && [ ${#TYPED[@]} -eq 0 ]; then
  while IFS= read -r f; do
    if grep -qE '^// *RUN:.*[[:space:]]-typed([[:space:]]|$)' "$f"; then
      TYPED+=("$f")
    else
      UNTYPED+=("$f")
    fi
  done < <(ls "$ROOT"/test/jit/*.js 2>/dev/null)
  TYPED+=("$ROOT/test/shermes/array-typed.js")
  TYPED+=("$ROOT/test/hermes/flow/array-for-of.js")
  TYPED+=("$ROOT/test/hermes/flow/nbody.js")
  if [ ${#UNTYPED[@]} -eq 0 ] && [ ${#TYPED[@]} -eq 3 ]; then
    echo "jit-dump: default corpus is empty; no $ROOT/test/jit/*.js found" >&2
    exit 2
  fi
fi

for f in ${UNTYPED[@]+"${UNTYPED[@]}"} ${TYPED[@]+"${TYPED[@]}"}; do
  [ -f "$f" ] || { echo "jit-dump: no such corpus file: $f" >&2; exit 2; }
done

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

# Canonicalize the dump so that two captures can be compared byte for byte.
# The actual sed+awk pipeline lives in jit-canon.sh -- see that script for
# the full rationale (constant materialization, RO_DATA, ASLR addresses). It
# is the single source of truth, also consumed directly by
# test/jit/recompile-deterministic.js.
canonicalize() {
  if [ "$RAW" -eq 1 ]; then
    cat
    return
  fi
  "$SCRIPT_DIR/jit-canon.sh"
}

run_one() {
  local label="$1" file="$2"; shift 2
  local before after
  # Snapshot the whole pipeline status: hermes failing and canonicalization
  # failing both yield a dump that is wrong but looks plausible.
  local -a pstatus
  echo "===== $label =====" >> "$TMP"
  before=$(wc -l < "$TMP")
  # Recompilation would put multiple bodies per function in the dump;
  # baselines are defined as first-compile output.
  "$HERMES" "$@" -Xjit=force -Xjit-threshold=1 -Xjit-max-recompiles=0 \
    -Xdump-jitcode=3 "$file" 2>&1 \
    | canonicalize >> "$TMP"
  pstatus=("${PIPESTATUS[@]}")
  after=$(wc -l < "$TMP")
  # A failing run still writes its diagnostics into the dump, where they are
  # indistinguishable from emitted code. Never let that pass silently.
  if [ "${pstatus[0]}" -ne 0 ]; then
    echo "jit-dump: '$label' exited with status ${pstatus[0]}" >&2
    echo "  Its diagnostics, not JIT output, would have gone into the dump." >&2
    echo "  Last lines captured:" >&2
    tail -n 3 "$TMP" | sed 's/^/    /' >&2
    exit 1
  fi
  # Canonicalization is sed|awk writing to $TMP, so this catches a write
  # failure (full disk, unwritable temp) that would silently truncate.
  if [ "${pstatus[1]}" -ne 0 ]; then
    echo "jit-dump: canonicalizing '$label' failed with status ${pstatus[1]}" >&2
    echo "  The dump is likely truncated; refusing to continue." >&2
    exit 1
  fi
  if [ "$after" -le "$((before + 2))" ]; then
    echo "jit-dump: '$label' emitted almost nothing." >&2
    echo "  Is $HERMES built with the JIT enabled (HERMESVM_ALLOW_JIT)?" >&2
    exit 1
  fi
}

for f in ${UNTYPED[@]+"${UNTYPED[@]}"}; do run_one "$(basename "$f")" "$f"; done
for f in ${TYPED[@]+"${TYPED[@]}"}; do run_one "typed $(basename "$f")" "$f" -typed; done

if [ "$QUIET" -eq 0 ]; then
  total=$(wc -l < "$TMP" | tr -d ' ')
  const=$(grep -c 'CONST ' "$TMP" || true)
  pct=$(( total > 0 ? (total - const) * 100 / total : 0 ))
  echo "jit-dump: $total lines, $const canonicalized (${pct}% compared verbatim)" >&2
fi

if [ -n "$OUT" ]; then
  cp "$TMP" "$OUT"
else
  cat "$TMP"
fi
