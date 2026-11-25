// Operations, the page the operator keeps open: where each test stands
// (one tile per test tab), the OBC's modes (CDR Figure 44) as a diagram that
// sends the mode command when a node is clicked, the link and telemetry
// commands, and every run so far.

import { esc, fmtAge, fmtTime, setHTML } from '../util.js';
import { t } from '../i18n.js';
import { since, serverNow } from '../state.js';
import { armBox, commandsCard, isArmed, onArm, send } from '../commands.js';
import { latestRun, runDot } from '../runs.js';
import { pingSummary } from './link.js';

// The tests, each with its tab and the mode its run asks for.
const TESTS = [
  { tab: 'adsb', label: 'tab_adsb', target: 'MISSION_ADSB' },
  { tab: 'deploy', label: 'tab_deploy', target: 'BASIC_INTERMEDIATE' },
  { tab: 'aocs', label: 'tab_aocs', target: 'ADVANCED_AOCS' },
  { tab: 'surv', label: 'tab_surv', target: 'ENV_SURVIVAL' },
];

// One tile: the test's state from its last run, a link to its tab.
export function testTile(test, st) {
  const run = latestRun(st, test.target);
  const dot = runDot(run);
  let state = t('t_never'), detail = '';
  if (run && run.failed) { state = t('t_failed'); detail = run.failed; }
  else if (run && run.left != null) {
    state = t('t_done');
    detail = fmtTime(run.sent) + (run.inferred ? '' : ' · ' + t('r_dur') + ' ' + fmtAge(run.left - run.entered));
  } else if (run && run.entered != null) { state = t('t_running'); detail = fmtAge(serverNow() - run.entered); }
  else if (run) { state = t('t_sent'); detail = fmtTime(run.sent); }
  return `<a class="test ${dot}" href="#${test.tab}"><span class="dot ${dot}"></span>` +
    `<span class="name">${esc(t(test.label))}</span><b>${esc(state)}</b><small>${esc(detail)}</small></a>`;
}

const MODES = [
  { name: 'BASIC_INTERMEDIATE', cmd: 'mode basic_inter', arm: true, sub: 'm_basic', y: 30 },
  { name: 'ADVANCED_AOCS', cmd: 'mode aocs', arm: true, sub: 'm_aocs', y: 90 },
  { name: 'MISSION_ADSB', cmd: 'mode mission_adsb', sub: 'm_adsb', y: 150 },
  { name: 'MISSION_DOWNLINK', cmd: 'mode downlink', sub: 'm_down', y: 210 },
  { name: 'ENV_SURVIVAL', cmd: 'mode survival', sub: 'm_surv', y: 270 },
];

function node(name, cmd, arm, sub, x, y, w, h, cur, armed, hub) {
  const cls = 'node' + (cur ? ' cur' : '') + (arm ? ' arm' : '') + (arm && armed ? ' on' : '');
  return `<g class="${cls}" data-mode="${esc(cmd)}" tabindex="0" role="button"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="10"/>` +
    `<text x="${x + 14}" y="${y + (hub ? 26 : 19)}">${esc(name)}${arm ? ' ▲' : ''}</text>` +
    `<text class="sub" x="${x + 14}" y="${y + (hub ? 46 : 36)}">${esc(t(sub))}</text><title>${esc(cmd)}</title></g>`;
}

export const tab = {
  id: 'ops', label: 'tab_ops',
  mount(panel) {
    panel.innerHTML = `
      <section class="card"><h2>${esc(t('tests'))}</h2><div class="tests" id="ops-tests"></div></section>
      <section class="card span-7 modes"><h2>${esc(t('modes'))}</h2>
        ${armBox()}
        <svg viewBox="0 0 640 330" id="modesvg" role="group"></svg>
        <div class="muted" id="since"></div></section>
      <section class="card span-5">${commandsCard()}
        <div class="grp"><span class="lbl">${esc(t('link_now'))}</span><div class="kv" id="ops-link"></div></div></section>
      <section class="card"><h2>${esc(t('runs'))}</h2><div class="scroll" id="runs"></div></section>`;
    panel.querySelector('#modesvg').addEventListener('click', (e) => {
      const g = e.target.closest('[data-mode]');
      if (g) send(g.dataset.mode);
    });
    onArm(() => { this.drawn = null; });
  },
  update(state) {
    const st = state.st;
    setHTML(document.getElementById('ops-tests'), TESTS.map((x) => testTile(x, st)).join(''));
    const a = (st && st.acks) || {};
    const pings = state.log.filter((e) => e.text.startsWith('ping_summary'));
    setHTML(document.getElementById('ops-link'), [
      [t('acks'), esc(`${a.ok ?? 0} / ${a.bad ?? 0}`)],
      [t('failed'), esc(a.tc_failed ?? 0)],
      [t('latency'), esc(a.latency_ms == null ? '—' : a.latency_ms + ' ms')],
      [t('ping_res'), pings.length ? pingSummary(pings[pings.length - 1].text) : '—'],
    ].map(([k, v]) => `<span>${esc(k)}</span><span>${v}</span>`).join(''));
    const cur = st && st.mode;
    const key = cur + '|' + isArmed() + '|' + document.documentElement.lang;
    if (this.drawn !== key) {
      let h = '';
      for (const m of MODES) h += `<path class="edge" d="M210 165 C 270 165, 270 ${m.y + 22}, 330 ${m.y + 22}"/>`;
      h += '<path class="edge" d="M560 172 C 600 172, 600 232, 560 232" stroke-dasharray="4 3"/>';
      h += node('PRE_TEST', 'mode task_done', false, 'm_pre', 20, 135, 190, 60, cur === 'PRE_TEST', isArmed(), true);
      for (const m of MODES) h += node(m.name, m.cmd, m.arm, m.sub, 330, m.y, 230, 44, cur === m.name, isArmed(), false);
      document.getElementById('modesvg').innerHTML = h;
      this.drawn = key;
    }
    document.getElementById('since').textContent = st && st.mode
      ? t('in_mode') + ' ' + fmtAge(since(st.mode_age)) : t('never');
    const runs = ((st && st.runs) || []).slice().reverse();
    setHTML(document.getElementById('runs'), runs.length ? `<table class="tbl"><thead><tr>
        <th>${esc(t('r_cmd'))}</th><th>${esc(t('r_sent'))}</th><th>${esc(t('r_ack'))}</th>
        <th>${esc(t('r_in'))}</th><th title="PRE_TEST">${esc(t('r_out'))}</th><th>${esc(t('r_dur'))}</th></tr></thead><tbody>${runs.map((r) => {
        const a = r.acks.find((x) => x.cmd === 'SET_MODE');
        return `<tr><td>${esc(r.cmd.replace('mode ', ''))}</td><td>${esc(fmtTime(r.sent))}</td>
          <td>${a ? esc(a.status + ' ' + a.latency_ms + ' ms') : esc(r.failed || '…')}</td>
          <td>${r.inferred ? `<span title="${esc(t('s_inferred'))}">&lt; 10 s</span>` : r.entered != null ? '+' + esc(fmtAge(r.entered - r.sent)) : '—'}</td>
          <td>${r.left != null ? esc(r.left_to) + ' +' + esc(fmtAge(r.left - r.sent)) : '—'}</td>
          <td>${r.left != null && r.entered != null && !r.inferred ? esc(fmtAge(r.left - r.entered)) : '—'}</td></tr>`;
      }).join('')}</tbody></table>` : `<p class="muted" style="padding:10px">${esc(t('no_runs'))}</p>`);
  },
};
