/* local-variable pressure: a small rotating set of locals, no expression chain.
   Measures how well the backend keeps locals out of memory. */
function rotate(n) {
    var a = 1, b = 2, c = 3;
    for (var i = 0; i < n; i++) {
        var t = a + b;
        a = b + 1;
        b = c + 1;
        c = t;
    }
    return a + b + c;
}

rotate(1000); /* warm-up */

var t0 = Date.now();
var check = rotate(20000000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH locals ms=" + ms + " check=" + check);
