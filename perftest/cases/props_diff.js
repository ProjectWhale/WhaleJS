/* React.memo / Vue patch shape: shallow-compare two props objects field by field
   and count mismatches. All comparison, no allocation. */
function makeProps(v) { return { a: v, b: v + 1, c: v + 2, d: v + 3, e: v + 4, f: v + 5 }; }
function shallowEq(x, y) {
    return x.a === y.a && x.b === y.b && x.c === y.c &&
           x.d === y.d && x.e === y.e && x.f === y.f;
}
var oldP = makeProps(0);
var t0 = Date.now();
var check = 0;
for (var r = 0; r < 20000; r++) {
    var np = makeProps(r % 97);
    if (!shallowEq(oldP, np)) check++;
    oldP = np;
}
var ms = Date.now() - t0;
console.log("WHALE_BENCH props_diff ms=" + ms + " check=" + check);