// The tests that are one OBC mode each: deployment (burn-wire), the AOCS
// task and survival. Each shows what it does, its button, its steps from
// the last run, and what the housekeeping says about it.

import { esc, fmtNum, setHTML } from '../util.js';
import { t } from '../i18n.js';
import { latestRun, genericSteps, runDot } from '../runs.js';

function hkFig(label, v, div, unit) {
  const nd = v == null;
  return `<div class="fig"><span class="lbl">${esc(label)}</span><b>${nd ? '—' : esc(fmtNum(v / div, 1)) + `<small>${esc(unit)}</small>`}</b></div>`;
}

function modeTab({ id, label, target, lead, start, end, extra, side }) {
  return {
    id, label,
    mount(panel) {
      panel.innerHTML = `
        <section class="card span-6"><h2>${esc(t(label))}</h2>
          <p class="lead">${esc(t(lead))}</p>
          <div class="row" style="margin-bottom:12px">
            <button class="btn primary${start.arm ? ' danger' : ''}" data-cmd="${esc(start.cmd)}">${esc(t(start.label))}${start.arm ? ' ▲' : ''}</button>
            ${end ? `<button class="btn" data-cmd="${esc(end.cmd)}">${esc(t(end.label))}</button>` : ''}</div>
          <div id="${id}-steps"></div></section>
        <section class="card span-6" id="${id}-side"></section>
        ${extra ? `<section class="card" id="${id}-extra"></section>` : ''}`;
    },
    update(state) {
      const run = latestRun(state.st, target);
      setHTML(document.getElementById(id + '-steps'),
        run ? genericSteps(run, side.steps ? side.steps(run) : []) : `<p class="muted">${esc(t('no_runs'))}</p>`);
      setHTML(document.getElementById(id + '-side'), side.html(state.st));
      if (extra) extra(document.getElementById(id + '-extra'), state.st);
    },
    dot(st) { return runDot(latestRun(st, target)); },
  };
}

export const deploy = modeTab({
  id: 'deploy', label: 'tab_deploy', target: 'BASIC_INTERMEDIATE', lead: 'deploy_lead',
  start: { cmd: 'mode basic_inter', label: 'start_deploy', arm: true },
  side: {
    steps: (run) => [[t('s_burn'), run.left, null]],
    html: (st) => {
      const hk = st && st.hk;
      return `<h2>${esc(t('power'))}</h2><div class="figs">
        ${hkFig(t('battery'), hk && hk.battery_mv, 1000, 'V')}
        ${hkFig(t('current'), hk && hk.current_ma, 1, 'mA')}
        ${hkFig(t('tbat'), hk && hk.temp_bat_ccel, 100, '°C')}</div>`;
    },
  },
});

export const aocs = modeTab({
  id: 'aocs', label: 'tab_aocs', target: 'ADVANCED_AOCS', lead: 'aocs_lead',
  start: { cmd: 'mode aocs', label: 'start_aocs', arm: true },
  side: {
    html: (st) => {
      const hk = st && st.hk;
      return `<h2>${esc(t('attitude'))}</h2><div class="figs att">
        ${hkFig(t('roll'), hk && hk.roll_cdeg, 100, '°')}
        ${hkFig(t('pitch'), hk && hk.pitch_cdeg, 100, '°')}
        ${hkFig(t('yaw'), hk && hk.yaw_cdeg, 100, '°')}</div>`;
    },
  },
  // The AOCS team's page, once it is in ground/host/web/aocs/: same origin,
  // so it uses /api/state, /events and /api/cmd like this page.
  extra(box, st) {
    if (box.dataset.ready) return;
    box.dataset.ready = '1';
    fetch('/aocs/index.html', { method: 'HEAD' }).then((r) => {
      box.innerHTML = r.ok
        ? `<h2>AOCS</h2><iframe class="slot" src="/aocs/" title="AOCS"></iframe>`
        : `<h2>AOCS</h2><p class="muted">${esc(t('aocs_slot'))}</p>`;
    }).catch(() => { box.innerHTML = `<p class="muted">${esc(t('aocs_slot'))}</p>`; });
  },
});

export const survival = modeTab({
  id: 'surv', label: 'tab_surv', target: 'ENV_SURVIVAL', lead: 'surv_lead',
  start: { cmd: 'mode survival', label: 'start_surv' },
  end: { cmd: 'mode task_done', label: 'end_surv' },
  side: {
    html: (st) => {
      const hk = st && st.hk;
      return `<h2>${esc(t('temps'))}</h2><div class="figs">
        ${hkFig(t('tbat'), hk && hk.temp_bat_ccel, 100, '°C')}
        ${hkFig(t('text'), hk && hk.temp_ext_ccel, 100, '°C')}
        ${hkFig(t('tsoc'), hk && hk.temp_soc_ccel, 100, '°C')}
        ${hkFig(t('current'), hk && hk.current_ma, 1, 'mA')}</div>`;
    },
  },
});
