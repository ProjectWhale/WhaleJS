/* float literals in the hot path.
   A literal that reaches the emitter as an integer kind is silently truncated
   (1.5 -> 1), which changes the result without any error - so this case keeps
   literals of both kinds inside the loop where the JIT can see them. */
function frac(n) {
    var s = 0.0;
    for (var i = 0; i < n; i++) {
        s = s * 0.5 + 1.5;
    }
    return s;
}

frac(1000); /* warm-up */

var t0 = Date.now();
var check = frac(20000000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH floatlit ms=" + ms + " check=" + check);
