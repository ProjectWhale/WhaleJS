/* integer loop with a remainder and a comparison in the body: exercises the
   integer-ish opcodes (mod, comparisons) without leaving the number domain. */
function loop_sum(n) {
    var s = 0;
    for (var i = 0; i < n; i++) {
        if (i % 3 === 0) {
            s += i;
        } else if (i % 7 === 0) {
            s -= i;
        } else {
            s += 1;
        }
    }
    return s;
}

loop_sum(1000); /* warm-up */

var t0 = Date.now();
var check = loop_sum(5000000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH loop_sum ms=" + ms + " check=" + check);
