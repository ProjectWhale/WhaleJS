/* string building: allocation-bound and interpreter-only, included as the
   lower bound that no numeric JIT can help with. */
function build(n) {
    var parts = [];
    for (var i = 0; i < n; i++) parts.push("v" + i);
    return parts.join(",");
}

build(10); /* warm-up */

var t0 = Date.now();
var check = 0;
for (var r = 0; r < 30; r++) check = build(2000).length;
var ms = Date.now() - t0;

console.log("WHALE_BENCH string ms=" + ms + " check=" + check);
