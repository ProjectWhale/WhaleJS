/* branch-heavy: short loop body, several comparisons per iteration, so the
   comparison-to-branch path dominates. */
function branchy(n) {
    var s = 0;
    for (var i = 0; i < n; i++) {
        var v = i & 15;
        if (v === 0) s += 1;
        else if (v < 4) s += 2;
        else if (v < 8) s += 3;
        else if (v < 12) s += 4;
        else s += 5;
    }
    return s;
}

branchy(1000); /* warm-up */

var t0 = Date.now();
var check = branchy(10000000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH branch ms=" + ms + " check=" + check);
