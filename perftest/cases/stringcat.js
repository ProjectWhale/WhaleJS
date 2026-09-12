/* front-end shape: template assembly into a string */
function join(n) {
    var s = "";
    for (var i = 0; i < n; i++) s = s + "x";
    return s.length;
}
join(1000);
var t0 = Date.now();
var check = join(20000);
var ms = Date.now() - t0;
console.log("WHALE_BENCH stringcat ms=" + ms + " check=" + check);