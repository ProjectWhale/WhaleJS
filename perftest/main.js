#!/usr/bin/env node
/*
 * perftest - run the scenarios in cases/ under every engine found on PATH.
 *
 *   node perftest/main.js                     all cases, all engines
 *   node perftest/main.js fib arith locals    only these cases
 *   node perftest/main.js --engines=node,qjs  only these engines
 *   node perftest/main.js --repeat=3          best of 3 runs per cell
 *   node perftest/main.js --list              list the cases
 *   node perftest/main.js --json              machine-readable output
 *
 * Engines are plain command names resolved through PATH, so nothing here
 * depends on where a binary happens to live. An engine that is not installed
 * is reported as missing and skipped rather than failing the run.
 *
 * Each case must print one line:
 *
 *   WHALE_BENCH <name> ms=<milliseconds> check=<value>
 *
 * `check` is the correctness gate: it has to come out identical under every
 * engine that runs the case.
 */
'use strict';

const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const CASES_DIR = path.join(__dirname, 'cases');
const ALL_ENGINES = ['node', 'qjs', 'whalejs'];
const DEFAULT_TIMEOUT = 300; /* seconds */

function parseArgs(argv) {
    const opts = { cases: [], engines: ALL_ENGINES, repeat: 1, timeout: DEFAULT_TIMEOUT, json: false, list: false };

    for (const a of argv) {
        if (a === '--list') opts.list = true;
        else if (a === '--json') opts.json = true;
        else if (a.startsWith('--engines=')) opts.engines = a.slice(10).split(',').map(s => s.trim()).filter(Boolean);
        else if (a.startsWith('--repeat=')) opts.repeat = Math.max(1, parseInt(a.slice(9), 10) || 1);
        else if (a.startsWith('--timeout=')) opts.timeout = Math.max(1, parseFloat(a.slice(10)) || DEFAULT_TIMEOUT);
        else if (a.startsWith('-')) throw new Error(`unknown option: ${a}`);
        else opts.cases.push(a);
    }
    return opts;
}

function listCases() {
    return fs.readdirSync(CASES_DIR)
        .filter(f => f.endsWith('.js'))
        .map(f => f.slice(0, -3))
        .sort();
}

/* Is this command runnable from PATH? */
function probe(cmd) {
    const r = spawnSync(cmd, ['-e', '0'], { stdio: 'ignore', windowsHide: true, timeout: 30000 });
    return !r.error && r.status === 0;
}

/* Run one case under one engine; returns {ms, check} or {error}. */
function run(cmd, file, timeoutSeconds) {
    const r = spawnSync(cmd, [file], {
        encoding: 'utf8',
        windowsHide: true,
        timeout: timeoutSeconds * 1000,
        maxBuffer: 16 * 1024 * 1024,
    });
    if (r.error) return { error: r.error.code === 'ETIMEDOUT' ? 'timeout' : String(r.error.message || r.error) };
    const out = `${r.stdout || ''}${r.stderr || ''}`;
    const m = out.match(/^WHALE_BENCH\s+(\S+)\s+ms=(\d+)\s+check=(\S+)/m);
    if (!m) return { error: `no result line (exit ${r.status})` };
    return { ms: parseInt(m[2], 10), check: m[3] };
}

function pad(s, n) {
    s = String(s);
    return s.length >= n ? s : s + ' '.repeat(n - s.length);
}

function main() {
    const opts = parseArgs(process.argv.slice(2));
    const available = listCases();

    if (opts.list) {
        process.stdout.write(available.join('\n') + '\n');
        return;
    }

    const wanted = opts.cases.length ? opts.cases : available;
    const unknown = wanted.filter(c => !available.includes(c));
    if (unknown.length) {
        process.stderr.write(`unknown case(s): ${unknown.join(', ')}\n`);
        process.stderr.write(`available: ${available.join(' ')}\n`);
        process.exitCode = 2;
        return;
    }

    const engines = opts.engines.filter(e => {
        if (probe(e)) return true;
        process.stderr.write(`engine '${e}' not found on PATH - skipped\n`);
        return false;
    });
    if (!engines.length) {
        process.stderr.write('no engines available\n');
        process.exitCode = 3;
        return;
    }

    const results = {}; /* results[case][engine] = {ms, check} | {error} */
    for (const c of wanted) {
        results[c] = {};
        for (const e of engines) {
            const file = path.join(CASES_DIR, `${c}.js`);
            let best = null;
            for (let i = 0; i < opts.repeat; i++) {
                const r = run(e, file, opts.timeout);
                if (r.error) { best = r; break; }
                if (!best || r.ms < best.ms) best = r;
            }
            results[c][e] = best;
        }
    }

    if (opts.json) {
        process.stdout.write(JSON.stringify({ engines, cases: wanted, results }, null, 2) + '\n');
    } else {
        const w = Math.max(12, ...engines.map(e => e.length + 2));
        process.stdout.write(pad('case', w) + engines.map(e => pad(e, 12)).join('') + '\n');
        process.stdout.write('-'.repeat(w + 12 * engines.length) + '\n');
        for (const c of wanted) {
            const cells = engines.map(e => {
                const r = results[c][e];
                return pad(r.error ? `-- ${r.error}` : `${r.ms} ms`, 12);
            });
            process.stdout.write(pad(c, w) + cells.join('') + '\n');
        }
    }

    /* correctness gate: the checksum must agree across engines */
    let mismatch = false;
    for (const c of wanted) {
        const vals = engines.map(e => results[c][e].check).filter(v => v !== undefined);
        if (vals.length > 1 && new Set(vals).size > 1) {
            process.stderr.write(`CHECK MISMATCH ${c}: ` + engines.map(e => `${e}=${results[c][e].check ?? '-'}`).join(' ') + '\n');
            mismatch = true;
        }
    }
    if (mismatch) process.exitCode = 1;
}

try {
    main();
} catch (e) {
    process.stderr.write(`perftest: ${e.message}\n`);
    process.exitCode = 1;
}
