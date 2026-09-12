/* Vue list diff shape: walk the old and new children arrays with a key check */
function build(n, salt) {
    var a = [];
    for (var i = 0; i < n; i++) a[i] = (i + salt) % 251;
    return a;
}
function reconcile(oldA, newA, n) {
    var moved = 0;
    for (var i = 0; i < n; i++) {
        if (oldA[i] !== newA[i]) moved++;
        if (i + 1 < n && newA[i] > newA[i + 1]) moved += 2;
    }
    return moved;
}
var oldA = build(200, 0);
reconcile(oldA, build(200, 1), 200);
var t0 = Date.now();
var check = 0;
for (var r = 0; r < 3000; r++) check = reconcile(oldA, build(200, r % 17), 200);
var ms = Date.now() - t0;
console.log("WHALE_BENCH list_reconcile ms=" + ms + " check=" + check);