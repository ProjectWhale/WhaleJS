/* object property access: property loads/stores on a plain object. */
function tally(n) {
    var o = { a: 0, b: 0, c: 0 };
    for (var i = 0; i < n; i++) {
        o.a += i % 3;
        o.b += i % 5;
        o.c += i % 7;
    }
    return o.a + o.b * 2 + o.c * 3;
}

tally(1000); /* warm-up */

var t0 = Date.now();
var check = 0;
for (var r = 0; r < 20; r++) check = tally(200000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH object ms=" + ms + " check=" + check);
