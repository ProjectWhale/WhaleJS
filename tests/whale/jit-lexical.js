/* The `let`/`const` scopes, function values, and the shapes WhaleNode's
   report (doc-internal/whalejs-jit-lexical-and-comma.md) reproduced.

   Every case here used to keep the whole function in the interpreter. The
   failure mode that matters for the first group is not a wrong number - it is
   a loop that never terminates, which is what a mis-typed `let` counter
   produced while it was being fixed. So each case has a trip count the answer
   proves ran, not just an equality that a constant could satisfy. */

function eq(what, got, want)
{
    if (got !== want) {
        console.log("FAIL: " + what + ": got " + got + ", want " + want);
        throw new Error("mismatch: " + what);
    }
}

/* ---- `let` / `const` must compile, and the counter must advance ---- */

function letCounter(n) {          /* `for (let i ...)` - put_loc_check + post_inc */
    var s = 0;
    for (let i = 0; i < n; i++)
        s += i;
    return s;
}

function letAccum(n) {            /* function-level `let`, var counter */
    let s = 0;
    for (var i = 0; i < n; i++)
        s += i;
    return s;
}

function letBlock(n) {            /* block-scoped `let` inside the body */
    var s = 0;
    for (var i = 0; i < n; i++) {
        { let t = i; s += t; }
    }
    return s;
}

function constMul(n) {
    const k = 2;
    var s = 0;
    for (var i = 0; i < n; i++)
        s += i * k;
    return s;
}

function letInIf(n) {
    var s = 0;
    for (var i = 0; i < n; i++) {
        if (i > 2) { let t = 1; s += t; }
        else { s += 2; }
    }
    return s;
}

function letUnused(n) {           /* declared, never read: still must not bail */
    let unused = 1;
    var s = 0;
    for (var i = 0; i < n; i++)
        s += i;
    return s;
}

/* A large n on purpose: a counter that never advances would hang instead of
   answering, and 1e6 trips is the cheapest way to make that visible. */
var N = 1000000;

eq("letCounter", letCounter(100), 4950);
eq("letAccum", letAccum(100), 4950);
eq("letBlock", letBlock(100), 4950);
eq("constMul", constMul(100), 9900);
eq("letInIf", letInIf(100), 3 * 2 + 97);
eq("letUnused", letUnused(100), 4950);
eq("letCounter big", letCounter(N), N * (N - 1) / 2);

/* ---- a function value / closure built inside a hot loop ---- */

function fnExpr(n) {              /* `var g = function (v) {}` per iteration */
    var s = 0;
    for (var i = 0; i < n; i++) {
        var g = function (v) { return v + 1; };
        s += g(i);
    }
    return s;
}

function tryCatch(n) {            /* try/catch inside a loop */
    var s = 0;
    for (var i = 0; i < n; i++) {
        try { s += i; } catch (e) { s -= 1; }
    }
    return s;
}

function objCtor(n) {             /* `new` on an object with a method call */
    var s = 0;
    for (var i = 0; i < n; i++) {
        var d = new Date(0);
        s += d.getTime() === 0 ? 1 : 0;
    }
    return s;
}

eq("fnExpr", fnExpr(100), 5050);
eq("tryCatch", tryCatch(100), 4950);
eq("tryCatch throws", (function () {
        var s = 0;
        for (var i = 0; i < 10; i++) {
            try { if (i === 5) throw new Error("x"); s += i; } catch (e) { s += 100; }
        }
        return s;
    })(), 40 + 100);   /* 0..9 without 5, plus the catch */
eq("objCtor", objCtor(100), 100);

/* A closure that captures and mutates: the cells are shared, so the increment
   has to be visible outside the loop as well as inside it. */
function captured(n) {
    var total = 0;
    for (var i = 0; i < n; i++) {
        var add = function (v) { total += v; };
        add(i);
    }
    return total;
}

eq("captured", captured(100), 4950);

/* ---- five captured names: the CommonJS module wrapper shape ---- */

var wrap = eval("(function (exports, require, module, __filename, __dirname) {" +
                "  var n = 0;" +
                "  for (var i = 0; i < 30; i++) n += i * 2;" +
                "  exports.n = n;" +
                "})");
{
    var out = {};

    for (var k = 0; k < 40; k++) {
        out = {};
        wrap(out, null, {}, 'x.js', '.');
    }
    eq("cjs wrapper", out.n, 870);
}

console.log("lexical test: ok");
