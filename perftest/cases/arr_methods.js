/* front-end shape: build a list and repeatedly reduce it - push, map-like loops */
function build(n) {
    var a = [];
    for (var i = 0; i < n; i++) a.push(i % 251);
    return a;
}
function total(a) {
    var s = 0;
    for (var i = 0; i < a.length; i++) s += a[i];
    return s;
}
build(1000);
var t0 = Date.now();
var check = 0;
for (var r = 0; r < 40; r++) check = total(build(2000));
var ms = Date.now() - t0;
console.log("WHALE_BENCH arr_methods ms=" + ms + " check=" + check);