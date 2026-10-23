'use strict';
// The cold field-read fallback must not destroy the field it failed on.
//
// `function f(x) { return x.kind === 'a'; }` compiles to a cached field read.
// The first trip takes the cold path: resolve the field pointer, then dispatch
// on the value's tag. A string is not a number, so the helper sets *deopt and
// answers 0 - and the code that followed the dispatch used to write that 0 back
// into the object's real field and flip its tag to float64. Every later call
// then read 0 where 'a' had been.
//
// Measured before the fix: `first true, bad 180` - the first ~20 calls were
// right and every one after that was wrong. The JIT had also corrupted the
// object itself, so interpreting it afterwards was wrong too.
//
// The same mechanism broke svelte's own compiler under WhaleNode: esrap reads
// `node.type` (a string) through exactly this path, and once it had been
// overwritten with 0, `node.type.includes(...)` threw "not a function".

var checks = 0;
function eq(a, b, what) {
  if (a !== b) throw new Error(what + ': got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b));
  checks++;
}

// 1. String-valued field read through the cold path, many times.
function kindIs(x) { return x.kind === 'a'; }
var obj = { kind: 'a' };
for (var i = 0; i < 400; i++) {
  eq(kindIs(obj), true, 'kindIs at ' + i);
}
// the object itself must be untouched
eq(obj.kind, 'a', 'object field not overwritten');

// 2. The plain read, so the corruption cannot hide behind the comparison.
function readKind(x) { return x.kind; }
for (var j = 0; j < 400; j++) {
  eq(readKind(obj), 'a', 'readKind at ' + j);
}

// 3. A boolean-valued field (same cold path, different non-number tag).
function flagOf(x) { return x.flag; }
var fo = { flag: true };
for (var k = 0; k < 400; k++) {
  eq(flagOf(fo), true, 'flagOf at ' + k);
}
eq(fo.flag, true, 'flag field not overwritten');

// 4. A number-valued field must still be fast-pathed and correct.
function numOf(x) { return x.n; }
var no = { n: 7 };
for (var m = 0; m < 400; m++) {
  eq(numOf(no), 7, 'numOf at ' + m);
}
eq(no.n, 7, 'number field unchanged');

// 5. The esrap shape: a table lookup keyed by a string field.
var table = { a: 1, b: 2, c: 3 };
function dispatch(node) { return table[node.type]; }
var nodes = [{ type: 'a' }, { type: 'b' }, { type: 'c' }];
for (var r = 0; r < 400; r++) {
  eq(dispatch(nodes[0]), 1, 'dispatch a at ' + r);
  eq(dispatch(nodes[1]), 2, 'dispatch b at ' + r);
  eq(dispatch(nodes[2]), 3, 'dispatch c at ' + r);
}

// 6. And a `.includes` on such a field, which is what actually threw.
function hasTS(node) { return node.type.includes('TS'); }
for (var q = 0; q < 400; q++) {
  eq(hasTS({ type: 'Identifier' }), false, 'hasTS false at ' + q);
  eq(hasTS({ type: 'TSFoo' }), true, 'hasTS true at ' + q);
}

console.log('field-deopt: ok (' + checks + ' checks)');
