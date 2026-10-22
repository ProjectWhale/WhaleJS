'use strict';
// Mixed-shape returns: `return;` on one path, `return <value>` on another.
// Each variant must be compiled AND answer identically to the interpreter.
var checks = 0;
function eq(a, b, what) {
  if (a !== b) throw new Error(what + ': got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b));
  checks++;
}

// number / void
function numOrVoid(x) { if (x > 0) return; return 7; }
// bool / void
function boolOrVoid(x) { if (x > 0) return; return true; }
// string / void  (a value kind that is neither number nor object)
function strOrVoid(x) { if (x > 0) return; return 'v'; }
// object / void
function objOrVoid(x) { if (x > 0) return; return { a: 1 }; }

var acc = 0;
for (var i = 0; i < 400; i++) {
  eq(numOrVoid(1), undefined, 'numOrVoid undefined');
  eq(numOrVoid(-1), 7, 'numOrVoid value');
  eq(boolOrVoid(1), undefined, 'boolOrVoid undefined');
  eq(boolOrVoid(-1), true, 'boolOrVoid value');
  eq(strOrVoid(1), undefined, 'strOrVoid undefined');
  eq(strOrVoid(-1), 'v', 'strOrVoid value');
  eq(objOrVoid(1), undefined, 'objOrVoid undefined');
  eq(objOrVoid(-1).a, 1, 'objOrVoid value');
  acc++;
}
eq(acc, 400, 'iterations');

// The err-callback shape this exists for.
function cb(err, out) { if (err) return; out.push('ok'); }
var seen = [];
for (var j = 0; j < 400; j++) {
  cb(null, seen);
  cb(new Error('x'), seen);
}
eq(seen.length, 400, 'callback ran only on the success path');
eq(seen[0], 'ok', 'callback value');

console.log('mixed-return: ok (' + checks + ' checks)');
