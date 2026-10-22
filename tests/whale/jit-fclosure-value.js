// Second instance of the same class: a local that the pre-scan recorded as
// holding a function is read with "nothing to emit" and marked WHALE_T_FN.
// That is only safe where a call site expands it. Everywhere else it is a
// value, and the value is garbage.
'use strict';

let failures = 0;
function check(name, ok, extra) {
  if (!ok) {
    failures++;
    console.log('FAIL', name, extra === undefined ? '' : String(extra));
  }
}

// Declaration read back as a value (the report says this shape was "ok",
// but it was ok only because the JIT declined the whole function).
function f1(a) {
  function g(b) { return a + b; }
  return g;
}
for (let i = 0; i < 300; i++) {
  const g = f1(i);
  if (typeof g !== 'function' || g(1) !== i + 1) { check('declaration as value', false, i + ' ' + typeof g); break; }
}
check('declaration as value', true);

// var f = function(){}; return f
function f2(a) {
  var g = function (b) { return a + b; };
  return g;
}
for (let i = 0; i < 300; i++) {
  const g = f2(i);
  if (typeof g !== 'function' || g(1) !== i + 1) { check('var-fn as value', false, i + ' ' + typeof g); break; }
}
check('var-fn as value', true);

// The function value handed to an opaque call.
function opaque(x) { return x; }
function f3(a) {
  function g(b) { return a + b; }
  return opaque(g);
}
for (let i = 0; i < 300; i++) {
  const g = f3(i);
  if (typeof g !== 'function' || g(1) !== i + 1) { check('declaration to call', false, i + ' ' + typeof g); break; }
}
check('declaration to call', true);

// Stored into an object field.
function f4(a) {
  function g(b) { return a + b; }
  return { m: g };
}
for (let i = 0; i < 300; i++) {
  const o = f4(i);
  if (typeof o.m !== 'function' || o.m(1) !== i + 1) { check('declaration to field', false, i); break; }
}
check('declaration to field', true);

// Called locally AND returned - both uses of the same local.
function f5(a) {
  function g(b) { return a + b; }
  const first = g(10);
  return [first, g];
}
for (let i = 0; i < 300; i++) {
  const r = f5(i);
  if (r[0] !== i + 10 || typeof r[1] !== 'function' || r[1](1) !== i + 1) {
    check('called and returned', false, i);
    break;
  }
}
check('called and returned', true);

// A named function expression (self-reference through the local).
function f6(a) {
  const g = function h(b) { return b < 1 ? a : h(b - 1); };
  return g;
}
for (let i = 0; i < 300; i++) {
  const g = f6(i);
  if (typeof g !== 'function' || g(3) !== i) { check('named fn expr', false, i); break; }
}
check('named fn expr', true);

console.log(failures === 0 ? 'fclosure-value test: ok' : 'fclosure-value test: ' + failures + ' FAILED');
