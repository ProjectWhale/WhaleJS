/* Redux / React state shape: replace one field by copying the rest */
function setField(o, v) { return { a: o.a, b: o.b, c: o.c, d: o.d, e: v }; }
function step(n) {
    var st = { a: 0, b: 0, c: 0, d: 0, e: 0 };
    for (var i = 0; i < n; i++) {
        st = setField(st, i % 11);
        st.a = st.a + st.e;
    }
    return st.a;
}
step(1000);
var t0 = Date.now();
var check = 0;
for (var r = 0; r < 20; r++) check = step(50000);
var ms = Date.now() - t0;
console.log("WHALE_BENCH immutable_update ms=" + ms + " check=" + check);