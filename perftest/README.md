# perftest

Scenario benchmarks that run unchanged under several JS engines, so the same
file measures Node (V8), QuickJS and WhaleJS side by side.

## Usage

```
node perftest/main.js                     # every case, every engine on PATH
node perftest/main.js fib arith locals    # only these cases
node perftest/main.js --engines=node,qjs  # only these engines
node perftest/main.js --repeat=3          # best of 3 runs per cell
node perftest/main.js --list              # list the cases
node perftest/main.js --json              # machine-readable output
node perftest/main.js --timeout=120       # per-run limit in seconds
```

Exit code is 1 when two engines disagree on a case's `check` value, 2 for an
unknown case name, 3 when no engine is available.

## Engines

Engines are plain command names resolved through `PATH` - nothing here depends
on where a binary lives, and an engine that is not installed is reported as
missing and skipped:

| name      | what it should be                             |
|-----------|-----------------------------------------------|
| `node`    | Node.js                                       |
| `qjs`     | stock QuickJS interpreter                     |
| `whalejs` | the WhaleJS build of `qjs` (with the JIT)     |

To make a build available under those names, put the binary (or a copy/symlink
of it) in a directory that is on `PATH`, e.g. `whalejs` -> the `qjs` built by
this repository.

## Cases

Each file in `cases/` is self-contained and prints exactly one line:

```
WHALE_BENCH <name> ms=<milliseconds> check=<value>
```

`check` is the correctness gate: it must be identical under every engine that
runs the case. Each case warms up first, because WhaleJS installs its JIT
lazily once a function is hot (`WHALE_JIT_HOT_THRESHOLD`).

| case       | what it exercises                                        |
|------------|----------------------------------------------------------|
| `fib`      | self-recursive calls, no boxing on the recursive path    |
| `arith`    | `s = s * c + k` chain, one expression per iteration      |
| `locals`   | rotating locals, no expression chain                     |
| `mixed`    | two accumulators live across the loop body               |
| `nested`   | deep operator chain, intermediates never stored          |
| `loop_sum` | integer loop with `%` and comparisons                    |
| `branch`   | short body, many comparisons per iteration               |
| `minmax`   | nested loops feeding a min/max reduction                 |
| `floatlit` | float literals inside the loop body (truncation trap)    |
| `bitops`   | `& \| ^ << >> %`, and `~x` vs `!x` (ToInt32 semantics)   |
| `array`    | array build + scan (outside the numeric JIT shape)       |
| `sieve`    | Uint8Array sieve, integer inner loops                    |
| `object`   | plain-object property loads and stores                   |
| `string`   | string building and `join` (allocation bound)            |

`floatlit` and `bitops` are regression guards: both used to be lowered wrongly
(float literals truncated to int, bitwise results off) without any error, and
only a case that keeps them inside the hot loop catches that.

The last four are deliberately outside what the numeric JIT compiles; they
show its coverage boundary and how much is left on the interpreter.
