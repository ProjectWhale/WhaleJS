/* recursion: the classic self-recursive call shape.
   The callee is the function itself, so this is the one call form the JIT
   compiles: no boxing on the recursive path. */
function fib(n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

fib(18); /* warm-up: the JIT installs once the function is hot */

var t0 = Date.now();
var check = 0;
for (var i = 0; i < 20; i++) check = fib(26);
var ms = Date.now() - t0;

console.log("WHALE_BENCH fib ms=" + ms + " check=" + check);
