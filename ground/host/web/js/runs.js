// A test, step by step, from the runs gs_web.py follows (GsState.runs):
// every `mode` command from its echo to the OBC leaving the mode it asked
// for. Mode changes are seen in the housekeeping, every 10 s: the times
// of "entered" and "left" are that coarse.

import { esc, fmtAge } from './util.js';
import { t } from './i18n.js';
import { serverNow } from './state.js';

export function latestRun(st, target) {
  const runs = (st && st.runs) || [];
  for (let i = runs.length - 1; i >= 0; i--) if (runs[i].target === target) return runs[i];
  return null;
}

export function ack(run, cmd) {
  return run ? run.acks.find((a) => a.cmd === cmd) || null : null;
}

// One <li> of a step list. state: 'done' | 'now' | 'fail' | ''.
export function step(n, label, cls, value) {
  return `<li class="${cls}"><span class="s">${cls === 'done' ? '✓' : cls === 'fail' ? '✗' : n}</span>` +
    `<span>${esc(label)}</span><span class="v">${esc(value ?? '')}</span></li>`;
}

// "+2.4 s" after the command was sent.
export function after(run, t1) {
  return t1 == null ? '' : '+' + fmtAge(t1 - run.sent);
}

// The steps every mode test shares: sent, accepted, in the mode, back.
export function genericSteps(run, extra = []) {
  if (!run) return '';
  const a = ack(run, 'SET_MODE');
  const failed = run.failed;
  const items = [];
  let n = 1;
  items.push(step(n++, t('s_sent'), 'done', fmtTimeOf(run.sent)));
  items.push(step(n++, t('s_mode'), a ? (a.status === 'OK' ? 'done' : 'fail') : failed ? 'fail' : 'now',
    a ? `${a.status} · ${a.latency_ms} ms` : failed || t('waiting')));
  const reached = run.entered != null;
  // A mode shorter than the HK period (the burn-wire's pulse) is never seen
  // in the housekeeping: gs_web.py infers it from the ACK and the OBC back
  // in PRE_TEST, and its duration is not known.
  items.push(step(n++, t('s_entered') + ' ' + (run.target || ''), reached ? 'done' : (a && !failed ? 'now' : ''),
    run.inferred ? t('s_inferred') : reached ? after(run, run.entered) : ''));
  for (const [label, at, value] of extra) {
    items.push(step(n++, label, at != null ? 'done' : reached && run.left == null ? 'now' : '', value ?? (at != null ? after(run, at) : '')));
  }
  const back = run.left != null;
  items.push(step(n++, t('s_back') + (run.left_to && run.left_to !== 'PRE_TEST' ? ' (' + run.left_to + ')' : ''),
    back ? 'done' : reached ? 'now' : '',
    back ? (run.inferred ? after(run, run.left) : `${after(run, run.left)} · ${t('r_dur')} ${fmtAge(run.left - run.entered)}`)
      : reached ? fmtAge(serverNow() - run.entered) : ''));
  return `<ol class="steps">${items.join('')}</ol>`;
}

function fmtTimeOf(t1) {
  return new Date(t1 * 1000).toLocaleTimeString([], { hour12: false });
}

// The tab's dot: running, done or failed.
export function runDot(run) {
  if (!run) return '';
  if (run.failed) return 'bad';
  if (run.entered != null && run.left == null) return 'run';
  if (run.left != null) return 'ok';
  return 'run';
}
