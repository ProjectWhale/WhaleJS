/* Complex object shapes: the cases that are not a single fixed record.
 *
 * The point of this one is that a single measured shape is not the only thing a real
 * program does. Four phases, each a different pattern:
 *
 *   1. two literals with different fields, updated in the same loop - the union case;
 *      the layout has to span both, which is what the reservation rule is for
 *   2. a property added after the literal - the shape grows once, early
 *   3. a record whose fields are written in a different order than declared
 *   4. one object per iteration, escaped into an array - the case where unboxing has
 *      to give up or convert, and must stay correct either way
 *
 * `check` is the gate, so every phase has to come out identical under every engine
 * whatever the JIT decides to do with it.
 *
 * Every call takes a varying argument: a pure function called repeatedly with the
 * same one is answered by the engine's memo table and measures nothing at all. That
 * mistake has been made three times in this work - see
 * notes/correction-memo-and-giveup.md.
 */
function twoShapes(n, seed) {
    var a = { x: 0, y: 0, w: 0 };
    var b = { x: 0, z: 0 };
    var acc = 0;

    for (var i = 0; i < n; i++) {
        a.x += i % 3;
        a.y += i % 5;
        a.w = a.x - a.y;
        b.x += i % 7;
        b.z = b.x * 2;
        acc += a.w + b.z;
    }
    return acc + seed;
}

function growsShape(n, seed) {
    var o = { p: 0, q: 0 };
    var acc = 0;

    o.r = 0;                 /* added after the literal: the shape changes once */
    for (var i = 0; i < n; i++) {
        o.p = i % 11;
        o.q += o.p;
        o.r = o.q - o.p;
        acc += o.r;
    }
    return acc + seed;
}

function writeOrder(n, seed) {
    var o = { m: 0, n: 0, k: 0 };
    var acc = 0;

    for (var i = 0; i < n; i++) {
        o.k = i % 13;        /* declared order is m, n, k - written in reverse */
        o.n = o.k + 1;
        o.m = o.n + o.k;
        acc += o.m;
    }
    return acc + seed;
}

function escaped(n, seed) {
    var out = [];
    var acc = 0;

    for (var i = 0; i < n; i++) {
        var t = { v: i % 17, u: 0 };
        t.u = t.v * 3;
        out[i & 7] = t;      /* one object per iteration, kept in an array */
        acc += t.u;
    }
    return acc + seed;
}

var PHASES = [twoShapes, growsShape, writeOrder, escaped];

twoShapes(200, 0);
growsShape(200, 0);
writeOrder(200, 0);
escaped(200, 0);

var t0 = Date.now();
var check = 0;
var N = 120000;

for (var r = 0; r < 12; r++) {
    /* the argument varies so the memo table cannot answer for the function */
    for (var p = 0; p < PHASES.length; p++)
        check += PHASES[p](N + p, r);
}

var ms = Date.now() - t0;
console.log("WHALE_BENCH objshape ms=" + ms + " check=" + check);
