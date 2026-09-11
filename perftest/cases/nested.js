/* deep expressions: long operator chains whose intermediate results are not
   stored anywhere, so they should never reach memory. */
function deep(n) {
    var a = 1.5, b = 2.5, c = 3.5, d = 4.5, e = 5.5;
    var s = 0.0;
    for (var i = 0; i < n; i++) {
        s = (((a + b) * c - d) / (e + a) + (b * c - d * e) - a) * b + c - d + e;
    }
    return s;
}

deep(1000); /* warm-up */

var t0 = Date.now();
var check = deep(20000000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH nested ms=" + ms + " check=" + check);
