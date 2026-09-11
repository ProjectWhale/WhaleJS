/* nested loops with a min/max reduction. */
function minmax(n) {
    var lo = 1e30, hi = -1e30;
    for (var i = 0; i < n; i++) {
        for (var j = 0; j < 8; j++) {
            var v = i * 3 + j * 7 - 11;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    return hi - lo;
}

minmax(1000); /* warm-up */

var t0 = Date.now();
var check = minmax(2000000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH minmax ms=" + ms + " check=" + check);
