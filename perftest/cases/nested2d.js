/* front-end shape: row-major walk of a 2D grid (table / virtual scroll block) */
function grid(rows, cols) {
    var g = [];
    for (var r = 0; r < rows; r++) {
        var row = [];
        for (var c = 0; c < cols; c++) row[c] = (r * 3 + c * 7) % 101;
        g[r] = row;
    }
    return g;
}
function walk(g, rows, cols) {
    var s = 0;
    for (var r = 0; r < rows; r++)
        for (var c = 0; c < cols; c++)
            s += g[r][c];
    return s;
}
walk(grid(20, 20), 20, 20);
var g = grid(60, 60);
var t0 = Date.now();
var check = 0;
for (var k = 0; k < 40; k++) check = walk(g, 60, 60);
var ms = Date.now() - t0;
console.log("WHALE_BENCH nested2d ms=" + ms + " check=" + check);