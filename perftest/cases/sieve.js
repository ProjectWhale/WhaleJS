/* sieve of Eratosthenes over a typed array: integer-heavy inner loops with an
   array in the mix. */
function sieve(n) {
    var flags = new Uint8Array(n + 1);
    var count = 0;
    for (var i = 2; i <= n; i++) flags[i] = 1;
    for (var p = 2; p * p <= n; p++) {
        if (flags[p]) {
            for (var m = p * p; m <= n; m += p) flags[m] = 0;
        }
    }
    for (var k = 2; k <= n; k++) if (flags[k]) count++;
    return count;
}

sieve(1000); /* warm-up */

var t0 = Date.now();
var check = 0;
for (var r = 0; r < 20; r++) check = sieve(300000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH sieve ms=" + ms + " check=" + check);
