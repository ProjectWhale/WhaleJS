/* two mutually dependent accumulators, both live across the loop body. */
function mixed(n) {
    var x = 1.0, y = 2.0;
    for (var i = 0; i < n; i++) {
        x = x + y * 2;
        y = x - y;
    }
    return x + y;
}

mixed(1000); /* warm-up */

var t0 = Date.now();
var check = mixed(20000000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH mixed ms=" + ms + " check=" + check);
