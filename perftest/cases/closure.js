/* front-end shape: a component closure capturing state, updated in a loop */
function makeCounter() {
    var n = 0;
    return function (steps) {
        for (var i = 0; i < steps; i++) n = n + 1;
        return n;
    };
}
var c = makeCounter();
c(1000);
var t0 = Date.now();
var check = 0;
for (var r = 0; r < 20000; r++) check = c(50);
var ms = Date.now() - t0;
console.log("WHALE_BENCH closure ms=" + ms + " check=" + check);