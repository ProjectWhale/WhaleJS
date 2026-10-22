'use strict';
// A branch that is itself a jump target must still get its label emitted.
//
// `if (a) b = 1; else if (c) ...` compiles to a branch whose position is also
// the destination of an earlier branch. The emitter fuses `<cmp>; if` into one
// `if (..) goto`, which consumes the branch instruction - so unless the fuse
// checks that nothing jumps to it, the label is never defined and TinyCC
// rejects the whole unit with "label used but not defined". The function then
// silently ran on the interpreter (measured: 94,973 stderr lines on a Vite dev
// server before the fix).
//
// The shape below is the smallest one that produces it: an equality test whose
// fall-through is the position of a later branch.

var checks = 0;
function eq(a, b, what) {
  if (a !== b) throw new Error(what + ': got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b));
  checks++;
}

function isWhiteSpace(c) {
  if (c === 32) return true;
  if (c >= 9 && c <= 13) return true;
  if (c < 160) return false;
  if (c === 160) return true;
  if (c === 5760) return true;
  if (c >= 8192 && c <= 8202) return true;
  if (c === 8232) return true;
  if (c === 8233) return true;
  if (c === 8239) return true;
  if (c === 8287) return true;
  if (c === 12288) return true;
  if (c === 65279) return true;
  return false;
}

var expect = [];
for (var i = 0; i < 70000; i++) expect.push(isWhiteSpace(i) ? 1 : 0);

// warm it well past the hot threshold
for (var r = 0; r < 400; r++) {
  for (var k = 0; k < 300; k++) {
    var got = isWhiteSpace(k % 70000);
    if ((got ? 1 : 0) !== expect[k % 70000]) {
      throw new Error('isWhiteSpace(' + (k % 70000) + ') = ' + got);
    }
  }
}
checks++;

// spot-check the boundaries
eq(isWhiteSpace(32), true, 'space');
eq(isWhiteSpace(9), true, 'tab');
eq(isWhiteSpace(13), true, 'CR');
eq(isWhiteSpace(14), false, '14');
eq(isWhiteSpace(159), false, '159');
eq(isWhiteSpace(160), true, 'nbsp');
eq(isWhiteSpace(5760), true, 'ogham');
eq(isWhiteSpace(8192), true, 'en quad');
eq(isWhiteSpace(8202), true, 'hair space');
eq(isWhiteSpace(8203), false, 'zwsp');
eq(isWhiteSpace(8232), true, 'line sep');
eq(isWhiteSpace(65279), true, 'bom');
eq(isWhiteSpace(65280), false, 'after bom');

// A second shape: chained else-if where every condition is a branch target.
function classify(n) {
  if (n === 1) return 'a';
  else if (n === 2) return 'b';
  else if (n === 3) return 'c';
  else if (n === 4) return 'd';
  else if (n === 5) return 'e';
  else if (n >= 6 && n <= 9) return 'f';
  else if (n === 10) return 'g';
  else if (n < 0) return 'neg';
  return 'other';
}
var cls = [];
for (var m = 0; m < 300; m++) cls.push(classify(m % 14));
var want = ['other','a','b','c','d','e','f','f','f','f','g','other','other','other'];
for (var q = 0; q < 400; q++) {
  for (var j = 0; j < 14; j++) {
    var got2 = classify(j);
    if (got2 !== want[j]) throw new Error('classify(' + j + ') = ' + got2 + ', want ' + want[j]);
  }
}
checks++;
eq(classify(-5), 'neg', 'negative');
eq(classify(11), 'other', 'other');

console.log('fuse-label: ok (' + checks + ' checks)');
