# The fallback crash: reproducible recipe, and where the fault is not

Follow-on to `notes/whale-step-investigation.md`. That file has the history and
the reasoning; this one has the exact reproducer and the boundary, so the next
attempt starts from the same point instead of rediscovering it.

## Getting back to the reproducer

1. Restore the work diff. `temp/whale_step_work.patch` is UTF-16 (written with
   `git diff > file` under PowerShell); convert it **and its line endings** and
   apply it with rejects:

   ```
   $p = "temp/whale_step_work.patch"
   $t = [IO.File]::ReadAllText($p, [Text.Encoding]::Unicode) -replace "`r`n", "`n"
   [IO.File]::WriteAllText("temp/whale_step_lf.patch", $t, (New-Object Text.UTF8Encoding($false)))
   git apply --reject --whitespace=nowarn temp/whale_step_lf.patch
   ```

   Two rejects are expected and both are ignorable: `JS_WhaleArrNew` (the patch
   carries an older diagnostic block) and `loop_begin` (the patch carries the
   pre-fix body; the committed fix is better).

2. **The patch does not include `src/whale_jit.h`.** It was saved with
   `git diff src/quickjs.c src/whale_jit.c`, so the eight `JS_WhaleStep*`
   declarations have to be re-added by hand or the build fails with
   `'JS_WhaleStepPushSlot' undeclared`.

3. `xmake build whalejs`, then run the reproducer.

## The reproducer

    function h(n) {
        var out = [];
        var t = { v: 1 };
        for (var i = 0; i < n; i++) out[i & 7] = t;
        return out[0].v;
    }
    h(300);
    var c = 0; for (var r = 0; r < 5; r++) c += h(3000);
    console.log("h check=" + c);

Baseline: `h check=5`, matches node. With the wiring: TinyCC refuses the
function, the interpreter runs it, and the answer is still `h check=5` - the
reproducer shows the **bad code generation**, not the wrong answer.

A two-array variant crashes instead (`A check` never prints, exit 0xC0000005);
in that one TinyCC accepts the code and the damage happens at run time.

## What the diagnostics say

With `WHALE_STEP_DBG=1`, the label sequence emitted for the reproducer is

    [label] pos=14
    [label] pos=31
    [emit] called op=72 pos=36 sp=3      <- put_array_el, handed over
    [label] pos=41
    [emit] called op=64 pos=44 sp=1      <- get_field, handed over
    whale_jit.c:1: error: pointer expected

and the same sequence repeats for the second compilation, ending in
`duplicate label 'L14'`.

So:

- **The generator emits every label exactly once.** `L14` is not emitted twice
  by us - `14`, `31` and `41` each appear once per compilation.
- The first, real error is **`pointer expected`**, raised after the `get_field`
  fallback at pos=44. In TinyCC that message means `decl()` met an identifier
  followed by `(` where it expected a type name: some emitted construct parses
  as a declaration.
- `duplicate label 'L14'` is downstream of that: with the declaration error, the
  function body is not delimited the way TinyCC expects, so the second
  compilation's `L14` collides with the first's.
- The `WHALE_JIT_SRC` dump is **not** usable to find it. It renders the fallback
  calls as well-formed `if (!JS_WhaleStepPushNum(ctx, i, _ws.v[1])) { *deopt = 1;
  ...; return 0; }` blocks, and it drops the operators the parser actually
  receives - which is exactly the information needed here.

## The next tool to build

The dump is the wrong instrument; the token stream is the right one.
`JS_WhaleTokDump()` in `src/tcc/whale_tcc_iface.c` already exists and reconstructs
the stream with `get_tok_str()`, but it is **known broken for numeric literals**
(`TOK_CINT` / `TOK_CDOUBLE` are looked up in the identifier token table and read
out of bounds - it is why an earlier investigation concluded "the stream stops
after `v0 = `" when it did not). Fix that first: it is a small change to how those
two cases are printed, and it turns this from guesswork into a diff.

Then compare, at pos=44, the token sequence the generator emits against the
sequence for the surrounding instructions. The suspicion worth testing first is
that `whale_step_emit` calls `fold_materialize()` - which emits a **statement** -
from inside a position where the surrounding generator code still has an
expression open, so the statement lands mid-expression. The `pointer expected`
message is what that looks like from TinyCC's side.
