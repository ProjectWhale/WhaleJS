/* floating-point expression chain: `s = s * c + k` every iteration.
   Four source-level operations per pass, so this is where folding a whole
   expression into one C statement shows up. */
function chain(n, c, k) {
    var s = 0.0;
    for (var i = 0; i < n; i++) {
        s = s * c + k;
    }
    return s;
}

chain(1000, 1.000001, 1.5); /* warm-up */

var t0 = Date.now();
var check = chain(20000000, 1.000001, 1.5);
var ms = Date.now() - t0;

console.log("WHALE_BENCH arith ms=" + ms + " check=" + check);
