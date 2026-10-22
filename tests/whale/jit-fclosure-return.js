// fclosure in value position: the JIT used to elide the closure and leave
// garbage in the result slot. Every shape the report lists as broken.
'use strict';

let failures = 0;
function check(name, ok, extra) {
  if (!ok) {
    failures++;
    console.log('FAIL', name, extra === undefined ? '' : String(extra));
  }
}

// The one the report leads with: closure in return position.
function mk1(a) { return function (b) { return a + b; }; }
for (let i = 0; i < 300; i++) {
  const f = mk1(i);
  if (typeof f !== 'function' || f(1) !== i + 1) {
    check('return closure', false, 'at ' + i + ': ' + typeof f);
    break;
  }
}
check('return closure', true);

// Parenthesised.
function mk2(a) { return (function (b) { return a + b; }); }
for (let i = 0; i < 300; i++) {
  const f = mk2(i);
  if (typeof f !== 'function' || f(1) !== i + 1) { check('paren closure', false, i); break; }
}
check('paren closure', true);

// Arrow.
function mk3(a) { return (b) => a + b; }
for (let i = 0; i < 300; i++) {
  const f = mk3(i);
  if (typeof f !== 'function' || f(1) !== i + 1) { check('arrow closure', false, i); break; }
}
check('arrow closure', true);

// No capture.
function mk4() { return function (b) { return b; }; }
for (let i = 0; i < 300; i++) {
  const f = mk4();
  if (typeof f !== 'function' || f(7) !== 7) { check('no-capture closure', false, i); break; }
}
check('no-capture closure', true);

// Assigned to a property.
const holder = {};
function mk5(a) { holder.m = function (b) { return a + b; }; }
for (let i = 0; i < 300; i++) {
  mk5(i);
  if (typeof holder.m !== 'function' || holder.m(1) !== i + 1) {
    check('property closure', false, i);
    break;
  }
}
check('property closure', true);

// Class method returning a closure.
class K { constructor(a) { this.a = a; } make() { return (b) => this.a + b; } }
for (let i = 0; i < 300; i++) {
  const f = new K(i).make();
  if (typeof f !== 'function' || f(1) !== i + 1) { check('method closure', false, i); break; }
}
check('method closure', true);

// Shapes the report says were already fine - keep them that way.
function mk6(a) { var f = function (b) { return a + b; }; return f; }
for (let i = 0; i < 300; i++) {
  const f = mk6(i);
  if (typeof f !== 'function' || f(1) !== i + 1) { check('local-then-return', false, i); break; }
}
check('local-then-return', true);

function mk7(a) { return function (b) { return a + b; }.bind(null, 1); }
for (let i = 0; i < 300; i++) {
  const f = mk7(i);
  if (typeof f !== 'function' || f() !== i + 1) { check('bind closure', false, i); break; }
}
check('bind closure', true);

function mk8(a) { return a === 1 ? function () { return 1; } : function () { return 2; }; }
for (let i = 0; i < 300; i++) {
  const f = mk8(i % 2);
  const want = (i % 2) === 1 ? 1 : 2;
  if (typeof f !== 'function' || f() !== want) { check('ternary closure', false, i); break; }
}
check('ternary closure', true);

function mk9(a) { return { f: function (b) { return a + b; } }; }
for (let i = 0; i < 300; i++) {
  const o = mk9(i);
  if (typeof o.f !== 'function' || o.f(1) !== i + 1) { check('object-field closure', false, i); break; }
}
check('object-field closure', true);

function mk10(a) { return class { constructor() { this.a = a; } }; }
for (let i = 0; i < 300; i++) {
  const C = mk10(i);
  if (typeof C !== 'function' || new C().a !== i) { check('class return', false, i); break; }
}
check('class return', true);

// A closure returned from inside a loop, and one passed straight to a call.
function mk11(a) {
  const out = [];
  for (let i = 0; i < 3; i++) out.push(() => a + i);
  return out;
}
for (let i = 0; i < 300; i++) {
  const fs = mk11(i);
  if (fs.length !== 3 || fs[0]() !== i || fs[2]() !== i + 2) { check('loop closures', false, i); break; }
}
check('loop closures', true);

function callIt(fn) { return fn(10); }
function mk12(a) { return callIt(function (b) { return a + b; }); }
for (let i = 0; i < 300; i++) {
  const v = mk12(i);
  if (v !== i + 10) { check('closure as argument', false, i + ' -> ' + v); break; }
}
check('closure as argument', true);

console.log(failures === 0 ? 'fclosure test: ok' : 'fclosure test: ' + failures + ' FAILED');
if (failures) process.exitCode = 1;
