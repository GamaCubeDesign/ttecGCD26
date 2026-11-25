// The page's modules that need no browser, under Node: the state merge
// (whole log, then deltas), a test's steps, and the formats.
//
//     node ground/host/web/test_web.mjs

import assert from 'node:assert/strict';
import { state, merge } from './js/state.js';
import { latestRun, genericSteps, runDot, ack } from './js/runs.js';
import { fmtAge, fmtClock } from './js/util.js';
import { t } from './js/i18n.js';

const entry = (n) => ({ n, t: 1794863800 + n, kind: 'tm', text: 'line ' + n });

// A full view, then deltas; a repeated entry is not kept twice.
merge({ full: true, log: [1, 2, 3].map(entry), now: 1 });
merge({ full: false, log: [3, 4, 5].map(entry), now: 2 });
merge({ full: false, log: [], now: 3 });
assert.deepEqual(state.log.map((e) => e.n), [1, 2, 3, 4, 5]);
// A reconnect brings the whole recent log again: it replaces.
merge({ full: true, log: [4, 5, 6].map(entry), now: 4 });
assert.deepEqual(state.log.map((e) => e.n), [4, 5, 6]);
// The log is bounded.
merge({ full: true, log: Array.from({ length: 700 }, (_, i) => entry(i + 1)), now: 5 });
assert.equal(state.log.length, 500);
assert.equal(state.log[state.log.length - 1].n, 700);

// Runs: the latest of a target, its ACK, its dot and its steps.
const run = {
  n: 1, cmd: 'mode basic_inter', target: 'BASIC_INTERMEDIATE', sent: 100, failed: null,
  acks: [{ cmd: 'SET_MODE', status: 'OK', latency_ms: 289, attempts: 1, t: 100.3 }],
  entered: 104, left: null, left_to: null, dump1090: null, first_tracks: null, tracks: 0,
};
const st = { runs: [{ ...run, sent: 50, target: 'MISSION_ADSB' }, run], now: 110 };
assert.equal(latestRun(st, 'BASIC_INTERMEDIATE'), run);
assert.equal(latestRun(st, 'ENV_SURVIVAL'), null);
assert.equal(ack(run, 'SET_MODE').latency_ms, 289);
assert.equal(runDot(run), 'run');
assert.equal(runDot({ ...run, left: 109, left_to: 'PRE_TEST' }), 'ok');
assert.equal(runDot({ ...run, failed: 'SET_MODE FAILED' }), 'bad');
const html = genericSteps({ ...run, left: 109, left_to: 'PRE_TEST' });
assert.equal((html.match(/class="done"/g) || []).length, 4);
assert.match(html, /OK · 289 ms/);
// A pulse shorter than the HK period: inferred, no duration claimed.
const inferred = genericSteps({ ...run, entered: 100.3, left: 108, left_to: 'PRE_TEST', inferred: true }, [['burn', 108, null]]);
assert.equal((inferred.match(/class="done"/g) || []).length, 5);
assert.match(inferred, new RegExp(t('s_inferred')));
assert.doesNotMatch(inferred, new RegExp(t('r_dur')));
const failed = genericSteps({ ...run, acks: [{ cmd: 'SET_MODE', status: 'REJECTED_STATE', latency_ms: 280 }], entered: null, failed: 'SET_MODE REJECTED_STATE' });
assert.match(failed, /class="fail"/);

// Formats and words.
assert.equal(fmtAge(42), '42 s');
assert.equal(fmtAge(185), '3 min 05 s');
assert.equal(fmtClock(598), '09:58');
assert.equal(fmtClock(-5), '−00:05');
assert.equal(t('start_adsb'), 'Iniciar missão ADS-B');
assert.equal(t('no_such_key'), 'no_such_key');

console.log('test_web: ok');
