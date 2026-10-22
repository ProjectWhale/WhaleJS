/* WhaleJS regression: a function that captures more than four enclosing
   variables. The generator keeps four closure-slot entries, and it used to
   bound the get_var_ref index by var_ref_count - which counts the function's
   OWN locals that its children captured, not the enclosing variables it reads.
   A function with var_ref_count 0 and five captured variables therefore passed
   the gate, indexed past the four-entry tables, and read a garbage bytecode
   pointer from the stack (`cpc = 0x2200000021`) - a segfault on the second
   Obfuscator.obfuscate() call in WhaleNode.
   Expected: "many-closure: ok". */

'use strict';

var checks = 0;
function eq(actual, expected, what) {
    if (actual !== expected) {
        throw new Error(what + ': got ' + actual + ', expected ' + expected);
    }
    checks++;
}

/* Six enclosing variables: the index space is 0..5, well past the four slots
   the generator can name. The function must be refused by the generator and
   interpreted, and the answers must still be right. */
function make6() {
    var a = 1, b = 2, c = 3, d = 4, e = 5, f = 6;
    return function (x) { return a + b + c + d + e + f + x; };
}
var six = make6();
var acc = 0;
for (var i = 0; i < 500; i++) {
    acc += six(i);
}
/* sum_{i=0..499} (21 + i) = 500*21 + 124750 */
eq(acc, 500 * 21 + 124750, 'six captured variables');

/* The same shape with writes to the captured cells. */
function makeCounter6() {
    var a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
    return function () { a++; b++; c++; d++; e++; f++; return a + b + c + d + e + f; };
}
var cnt = makeCounter6();
var last = 0;
for (var j = 0; j < 400; j++) {
    last = cnt();
}
eq(last, 400 * 6, 'six captured counters');

/* Four captured variables stay on the fast path; the answers must match. */
function make4() {
    var a = 10, b = 20, c = 30, d = 40;
    return function (x) { return a + b + c + d + x; };
}
var four = make4();
var acc4 = 0;
for (var k = 0; k < 500; k++) {
    acc4 += four(k);
}
eq(acc4, 500 * 100 + 124750, 'four captured variables');

/* Mixed: a closure over five variables used as a call target (the VRFN path
   reads the same tables). */
function makeCallee5() {
    var p = 1, q = 2, r = 3, s = 4, t = 5;
    return function (x) { return p + q + r + s + t + x; };
}
function callIt(fn) {
    var total = 0;
    for (var i = 0; i < 300; i++) {
        total += fn(i);
    }
    return total;
}
eq(callIt(makeCallee5()), 300 * 15 + 44850, 'five captured variables via a call target');

console.log('many-closure: ok (' + checks + ' checks)');
