/* bitwise operators, remainder and logical not.
   These are int32 operators living in the numeric path, so they have to come
   back through ToInt32 - a plain C cast gives different results for the
   wrap-around and non-finite cases. `~x` (bitwise) and `!x` (logical) are
   distinct opcodes and are easy to confuse. */
function bits(n) {
    var acc = 0;
    for (var i = 0; i < n; i++) {
        var v = i & 255;
        acc = acc ^ (v << 3);
        acc = acc | (v >> 1);
        acc = (acc + (v % 7)) & 0xffff;
        if (!(v & 1)) acc = acc - (~v & 15);
    }
    return acc;
}

bits(1000); /* warm-up */

var t0 = Date.now();
var check = bits(5000000);
var ms = Date.now() - t0;

console.log("WHALE_BENCH bitops ms=" + ms + " check=" + check);
