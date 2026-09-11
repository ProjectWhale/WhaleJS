/* algebraic identities that must fold away without changing the value:
   x*1, 1*x, x/1, x-0. Each removed operation is one statement less, which is
   what matters here because TinyCC spills at every statement boundary.

   Note the deliberate omissions - x+0 and x*0 are NOT identities in JS
   (-0 + 0 is +0, and NaN * 0 is NaN), so a case like this must not start
   relying on them. */
function identities(n) {
    var s = 1.0;
    for (var i = 0; i < n; i++) {
        s = s * 1.0;
        s = 1.0 * s;
        s = s / 1.0;
        s = s - 0.0;
        s = s + (i & 3);
    }
    return s;
}

identities(1000); /* warm-up */

var t0 = Date.now();
var check = identities(5000000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH ident ms=" + ms + " check=" + check);
