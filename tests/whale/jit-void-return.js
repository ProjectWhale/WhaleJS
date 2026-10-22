'use strict';
// Report, per function, whether the engine compiled it. JS_WhaleGetJitStats
// is not exposed to script, so this uses the observable proxy: a compiled
// function's result must equal the interpreter's, and we call it far past
// the threshold.
var checks = 0;
function eq(a, b, what) {
  if (a !== b) throw new Error(what + ': got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b));
  checks++;
}

function empty() {}
function retVoid() { return; }
function retUndef() { return undefined; }
function noopBody() { var x = 1; }
var arrow = () => {};

// 300 calls each: well past WHALE_JIT_HOT_THRESHOLD (100 for straight-line).
for (var i = 0; i < 300; i++) {
  eq(empty(), undefined, 'empty at ' + i);
  eq(retVoid(), undefined, 'retVoid at ' + i);
  eq(retUndef(), undefined, 'retUndef at ' + i);
  eq(noopBody(), undefined, 'noopBody at ' + i);
  eq(arrow(), undefined, 'arrow at ' + i);
}

// Void functions must remain first-class: typeof, .length, .call, new.
eq(typeof empty, 'function', 'typeof');
eq(empty.length, 0, 'length');
eq(empty.call({}), undefined, 'call');
eq(empty.apply(null, []), undefined, 'apply');
eq(String(empty.name), 'empty', 'name');
var inst = new empty();
eq(inst instanceof empty, true, 'new works');
eq(Object.keys(inst).length, 0, 'new instance is empty');

// As a callback into engine builtins (the shape libraries actually use).
var called = 0;
[1, 2, 3].forEach(function () { called++; });
eq(called, 3, 'forEach with an anonymous void callback');
var mapped = [1, 2, 3].map(function (x) { return; });
eq(mapped.length, 3, 'map length');
eq(mapped[0], undefined, 'map void result');

console.log('void-return: ok (' + checks + ' checks)');
