// WhaleJS 性能/语义基准：qjs tests/whale/fib.js
// JIT 上线后用于对照解释器与机器码的结果和耗时。
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

var N = 30;
var t = Date.now();
var r = fib(N);
var ms = Date.now() - t;

if (r !== 832040) {
  console.error("fib(" + N + ") = " + r + "，期望 832040");
  throw new Error("bad fib");
}
console.log("fib(" + N + ")=" + r + " " + ms + "ms");
