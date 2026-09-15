/* React shape: a render pass creating vnodes - one fresh object per element,
   with a shallow props object inside each. Object creation dominates.

   One call, not a loop of them. A loop whose argument is the same in every pass
   is folded into a single call - in whalejs and in v8 alike - so it reports the
   time of one pass while reading like thirty. render(20000) once is what both
   engines actually run. */
function createElement(type, props) { return { type: type, props: props, key: null }; }
function render(n) {
    var out = 0;
    for (var i = 0; i < n; i++) {
        var v = createElement(i % 5, { id: i, cls: "c" + (i % 3) });
        out += v.type + v.props.id;
    }
    return out;
}
render(1000);
var t0 = Date.now();
var check = render(20000);
var ms = Date.now() - t0;
console.log("WHALE_BENCH vnode_build ms=" + ms + " check=" + check);