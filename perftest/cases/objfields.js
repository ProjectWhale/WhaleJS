/* front-end shape: a vnode-like record updated field by field in a loop */
function make() { return { tag: 0, key: 0, flags: 0, depth: 0 }; }
function run(n) {
    var o = make();
    for (var i = 0; i < n; i++) {
        o.tag = i % 7;
        o.key = i % 13;
        o.flags += o.tag;
        o.depth = o.flags - o.key;
    }
    return o.tag + o.key * 2 + o.flags * 3 + o.depth * 5;
}
run(1000);
var t0 = Date.now();
var check = 0;
for (var r = 0; r < 20; r++) check = run(200000);
var ms = Date.now() - t0;
console.log("WHALE_BENCH objfields ms=" + ms + " check=" + check);