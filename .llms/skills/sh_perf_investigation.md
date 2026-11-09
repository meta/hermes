# Static Hermes Performance Investigation Workflow

Structured workflow for AI agents investigating performance optimization opportunities in Static Hermes.

## Prerequisites

- A built shermes binary (cmake)
- Linux with `perf` installed (for instruction counts and profiling)
- Python 3 (for analysis tools)
- The `agent-perf/` directory at the Static Hermes root

## Investigation Workflow

### Step 1: Profile — Identify Hot Functions

Collect a function-level profile of the native binary running a benchmark:

```bash
# Compile benchmark to native binary
shermes -o /tmp/bench benchmarks/octane/richards.js

# Profile with perf
perf record -g /tmp/bench
perf report --stdio > /tmp/perf_report.txt

# Convert to structured JSON
agent-perf/tools/profile_to_json.py /tmp/perf_report.txt --top 20 --json
```

**What to look for:** Functions consuming >2% of total samples. Focus on `_sh_*` runtime functions and generated code functions (`_N_*`).

### Step 2: Map — Trace Hot Code Back to Source

For SH, map native code through the compilation chain:

```bash
# Map profile to generated C and original JS
agent-perf/tools/sh_source_mapper.py /tmp/generated.c --profile /tmp/perf_report.txt --json

# Or generate C with line directives for manual inspection
shermes -emit-c -g -o /tmp/generated.c benchmarks/octane/richards.js
```

**What to look for:** Which JS source patterns produce the hottest native code.

### Step 3: Categorize — Where Is the Bottleneck?

Determine which layer the bottleneck is in:

| Hot function pattern | Layer | Investigation path |
|---------------------|-------|-------------------|
| `_sh_ljs_*` functions | Runtime library | Step 6 |
| `_N_*` generated functions | Generated code quality | Step 5 |
| `DictPropertyMap::*` | VM internals | Runtime optimization |
| `HadesGC::*` | Garbage collector | GC tuning |
| C library functions | C compiler output | Step 5 |

### Step 4: PMU — Check Hardware Bottlenecks

Collect hardware performance counters:

```bash
# Collect PMU data
agent-perf/tools/collect_pmu.sh shermes benchmarks/octane/richards.js --json > /tmp/pmu.json

# Check for anomalies
agent-perf/tools/pmu_anomalies.py /tmp/pmu.json --json
```

**Anomaly interpretation:**
- **IPC < 1.0**: Memory-bound — look for cache misses, data layout issues
- **Branch miss > 10%**: Branch prediction — look for unpredictable branches, add LLVM_LIKELY hints
- **L1-icache miss > 5%**: Code too large — look for excessive inlining, function size
- **L1-dcache miss > 10%**: Data locality — look for pointer chasing, object layout

### Step 5: Generated Code — Analyze C Code Quality

Analyze the generated C code for optimization opportunities:

```bash
# Generate C code
shermes -emit-c -o /tmp/generated.c benchmarks/octane/richards.js

# Analyze patterns
agent-perf/tools/generated_code_analyzer.py /tmp/generated.c --json

# Check for anti-patterns
agent-perf/tools/generated_code_antipatterns.py /tmp/generated.c --json

# Get detailed report
agent-perf/tools/generated_code_report.sh shermes benchmarks/octane/richards.js --json
```

**What to look for:**
- Excessive runtime calls that could be specialized
- Redundant type checks on values with known types
- Unnecessary GC scope management (`_sh_enter`/`_sh_leave`)
- Functions that are too large (hurting C compiler optimization)
- Patterns that prevent C compiler optimization (aliasing, volatile)

### Step 6: Runtime — Profile Runtime Library Usage

Identify which runtime functions are hottest:

```bash
# Profile runtime function usage
agent-perf/tools/runtime_call_profiler.py /tmp/perf_report.txt --json

# Check GC overhead
agent-perf/tools/gc_stats.sh shermes benchmarks/octane/richards.js --json
```

**Optimization opportunities:**
- Hot `_sh_*` functions → add fast paths, branch hints, or inline
- High GC overhead → reduce allocation rate, optimize SHLocals management
- Hot property access → optimize hash table, cache-friendly layout

### Step 7: Compiler — Check Pass Effectiveness

Analyze which optimization passes help most:

```bash
# Compiler statistics
agent-perf/tools/compiler_stats.sh shermes benchmarks/octane/richards.js --json

# Experiment: what if we disable a specific pass?
agent-perf/tools/pass_experiment.sh shermes benchmarks/octane/richards.js --disable typeinference --json

# Register allocation quality
agent-perf/tools/regalloc_report.py < <(shermes -dump-ra benchmarks/octane/richards.js 2>&1)
```

### Step 8: Type Analysis — Check Typed Mode Potential

If working with typed code, or evaluating typed mode potential:

```bash
# Compare typed vs untyped
agent-perf/tools/type_coverage_analyzer.py \
  --untyped /tmp/untyped.c \
  --typed /tmp/typed.c \
  --json

# Quick proxy comparison
agent-perf/tools/perf_proxies.sh shermes benchmarks/octane/richards.js --json
agent-perf/tools/perf_proxies.sh shermes benchmarks/octane/richards.js --typed --json
```

### Step 9: Anti-Patterns — Scan for Known Issues

```bash
# C++ runtime anti-patterns
agent-perf/tools/antipattern_scan.py lib/VM/StaticH.cpp --profile /tmp/profile.json --json

# JS benchmark anti-patterns
agent-perf/tools/js_antipattern_scan.py benchmarks/octane/richards.js --json
```

### Step 10: Fix — Implement the Optimization

Based on findings, implement the fix in the appropriate layer:

| Finding | Where to fix |
|---------|-------------|
| Hot runtime function | `lib/VM/StaticH.cpp` — add fast path, branch hints |
| Redundant generated code | `lib/BCGen/SH/SH.cpp` — modify InstrGen |
| Missing optimization pass | `lib/Optimizer/` — new pass or improve existing |
| Type-related inefficiency | `lib/Sema/FlowChecker*.cpp` or `lib/Optimizer/Scalar/TypeInference.cpp` |
| Register allocation issue | `lib/BCGen/SH/SHRegAlloc.cpp` |

### Step 11: Validate — Measure the Impact

Run benchmarks to validate the optimization:

```bash
# Quick check: instruction count on one benchmark
agent-perf/tools/count_instructions.sh benchmarks/octane/richards.js \
  --compare ./shermes-baseline ./shermes-optimized --json

# Full suite comparison
agent-perf/tools/benchmark_compare.sh ./shermes-baseline ./shermes-optimized \
  --suite all --iterations 21 --json

# Quick proxies check (no regression in code size, compile time)
agent-perf/tools/perf_proxies.sh ./shermes-optimized benchmarks/octane/ --json
```

**Validation criteria:**
- Geometric mean instruction count improvement (even -0.5% is meaningful)
- Zero regressions across the benchmark suite
- p < 0.05 on Mann-Whitney U test
- No increase in native binary size beyond 1%
- No increase in compilation time beyond 5%

### Step 12: Submit — Create the Diff

Create a diff with benchmark results as the test plan:

```bash
# Run tests
cmake --build cmake-build-debug --target check-shermes

# Submit
jf s
```

**Diff structure:**
- Title: `[SH] <what the optimization does>`
- Summary: explain the bottleneck found, the optimization, and why it works
- Test Plan: include benchmark results table (baseline vs optimized, delta %)

## Tool Catalog

### Measurement & Benchmarking
| Tool | Purpose |
|------|---------|
| `count_instructions.sh` | Measure CPU instruction counts (deterministic) |
| `perf_proxies.sh` | Quick multi-dimensional performance proxies |
| `run_benchmarks.sh` | Run benchmark suite with median/stddev |
| `benchmark_compare.sh` | Statistical A/B comparison (Mann-Whitney U) |
| `compilation_profiler.sh` | Profile compilation pipeline timing |

### Profile & Diagnostic
| Tool | Purpose |
|------|---------|
| `profile_to_json.py` | Convert perf profiles to JSON |
| `annotate_source.py` | Line-level profile annotation |
| `annotate_disasm.py` | Instruction-level disassembly annotation |
| `sh_source_mapper.py` | Map native→C→JS source |

### PMU
| Tool | Purpose |
|------|---------|
| `collect_pmu.sh` | Collect hardware performance counters |
| `pmu_anomalies.py` | Detect PMU anomalies with investigation hints |

### Runtime Analysis
| Tool | Purpose |
|------|---------|
| `runtime_call_profiler.py` | Profile `_sh_*` function usage |
| `gc_stats.sh` | GC statistics with derived metrics |

### Compiler Pipeline
| Tool | Purpose |
|------|---------|
| `compiler_stats.sh` | Compiler pass statistics and timing |
| `pass_experiment.sh` | Experiment with optimization passes |
| `regalloc_report.py` | Register allocation quality |
| `generated_code_report.sh` | Generated C code analysis |

### SH-Specific Analysis
| Tool | Purpose |
|------|---------|
| `generated_code_analyzer.py` | Analyze C code patterns and call categories |
| `type_coverage_analyzer.py` | Typed mode effectiveness |
| `generated_code_antipatterns.py` | Generated code anti-pattern detection |

### Anti-Pattern Scanners
| Tool | Purpose |
|------|---------|
| `antipattern_scan.py` | C++ source anti-patterns |
| `js_antipattern_scan.py` | JS benchmark anti-patterns |

## Running Agent-Perf Tests

All Python tools have unit tests in `agent-perf/tests/` using Python's built-in
`unittest` module. Tests use synthetic input data — no shermes binary, perf
counters, or build system required.

```bash
# Run ALL tests (from xplat/static_h/)
python3 -m unittest discover -s agent-perf/tests -p 'test_*.py' -v

# Run a single test file
python3 -m unittest agent-perf/tests/test_generated_code_analyzer.py -v

# Run a specific test class
python3 -m unittest agent-perf.tests.test_generated_code_analyzer.TestAnalyzeGeneratedCode -v

# Run a specific test method
python3 -m unittest agent-perf.tests.test_generated_code_analyzer.TestAnalyzeGeneratedCode.test_basic_analysis -v
```

**Always run from `xplat/static_h/`.** Tests import tools via relative path.

Validate shell scripts (syntax check only):
```bash
for f in agent-perf/tools/*.sh; do bash -n "$f"; done
```

**When to run tests:**
- After modifying any Python tool in `agent-perf/tools/`
- After modifying any test in `agent-perf/tests/`
- Before submitting diffs that touch agent-perf code

**Adding tests for a new tool** `agent-perf/tools/my_tool.py`:
1. Create `agent-perf/tests/test_my_tool.py`
2. Add the path preamble:
   ```python
   import sys
   from pathlib import Path
   TOOLS_DIR = Path(__file__).parent.parent / "tools"
   sys.path.insert(0, str(TOOLS_DIR))
   from my_tool import my_function
   ```
3. Write `unittest.TestCase` classes with synthetic inputs
4. Verify: `python3 -m unittest agent-perf/tests/test_my_tool.py -v`

## Key Runtime Files for Optimization

| File | Lines | What's hot |
|------|-------|-----------|
| `lib/VM/StaticH.cpp` | ~2530 | Property access, type coercion, object creation |
| `lib/VM/JSObject.cpp` | — | Property lookup, hidden class transitions |
| `lib/VM/Operations.cpp` | — | Equality, comparison, typeof |
| `lib/VM/DictPropertyMap.cpp` | — | Hash table lookup for properties |
| `lib/VM/Callable.cpp` | — | Function call dispatch |
| `lib/BCGen/SH/SH.cpp` | ~3125 | C code generation patterns |
