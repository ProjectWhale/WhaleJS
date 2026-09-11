/* array access and growth: outside the numeric JIT shape on purpose, so this
   one stays on the interpreter and shows the JIT's coverage boundary. */
function build(n) {
    var a = [];
    for (var i = 0; i < n; i++) a[i] = i % 97;
    return a;
}

function scan(a) {
    var s = 0;
    for (var i = 0; i < a.length; i++) s += a[i];
    return s;
}

scan(build(1000)); /* warm-up */

var t0 = Date.now();
var check = 0;
for (var r = 0; r < 20; r++) check = scan(build(50000));
var ms = Date.now() - t0;

console.log("WHALE_BENCH array ms=" + ms + " check=" + check);
