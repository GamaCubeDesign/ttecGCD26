// The tests that are one OBC mode each: deployment (burn-wire), the AOCS
// task and survival. Each shows what it does, its button, its steps from
// the last run, and what the housekeeping says about it.

import { esc, fmtNum, setHTML } from '../util.js';
import { t } from '../i18n.js';
import { latestRun, genericSteps, runDot, step } from '../runs.js';
import { isArmed, setArmed, toast } from '../commands.js';

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

// REG-07's fallback, when the radio cannot reach the satellite: the same
// pulse fired from the Pi itself over ssh (gs_web.py --pi-ssh, gama-deploy
// on the Pi). Not a radio command; it needs arming like basic_inter.
let sshResult = null;
let sshBusy = false;

function sshSteps(r) {
  if (!r) return '';
  const s = r.steps || {};
  const st = (ok) => ok ? 'done' : r.result ? 'fail' : '';
  return `<ol class="steps">
    ${step(1, t('ssh_s_signal'), st(s.signal), '')}
    ${step(2, t('ssh_s_entered'), st(s.entered), '')}
    ${step(3, t('ssh_s_burn'), st(s.burn_on && s.driven), r.burn_ms != null ? fmtNum(r.burn_ms, 0) + ' ms' : '')}
    ${step(4, t('s_back'), st(s.back), '')}</ol>
    <p class="${r.ok ? 'muted' : 'toast bad'}">${esc(r.why || '')}</p>`;
}

function renderSsh(box) {
  const host = box.dataset.host;
  setHTML(box, `<h2>${esc(t('ssh_title'))}</h2>
    <p class="lead">${esc(t('ssh_lead'))}</p>
    ${host ? `<p class="muted">${esc(t('ssh_host'))}: <b>${esc(host)}</b></p>
      <div class="row" style="margin-bottom:12px">
        <button class="btn danger" id="ssh-deploy"${sshBusy ? ' disabled' : ''}>${esc(t(sshBusy ? 'ssh_busy' : 'ssh_start'))} ▲</button></div>
      <div id="ssh-steps">${sshSteps(sshResult)}</div>`
    : `<p class="muted">${esc(t('ssh_off'))}</p>`}`);
}

async function deploySsh(box) {
  if (!isArmed()) { toast(t('ssh_arm'), 'bad'); return; }
  sshBusy = true;
  sshResult = null;
  renderSsh(box);
  try {
    const r = await fetch('/api/deploy-ssh', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ armed: true }),
    });
    sshResult = await r.json();
    setArmed(false);
    toast(sshResult.why, sshResult.ok ? 'ok' : 'bad');
  } catch (e) {
    sshResult = { ok: false, why: t('no_server') };
  }
  sshBusy = false;
  renderSsh(box);
}

export const deploy = modeTab({
  id: 'deploy', label: 'tab_deploy', target: 'BASIC_INTERMEDIATE', lead: 'deploy_lead',
  start: { cmd: 'mode basic_inter', label: 'start_deploy', arm: true },
  extra(box) {
    if (box.dataset.ready) return;
    box.dataset.ready = '1';
    box.addEventListener('click', (e) => { if (e.target.closest('#ssh-deploy')) deploySsh(box); });
    fetch('/api/deploy-ssh').then((r) => r.json()).then((j) => {
      box.dataset.host = j.host || '';
      renderSsh(box);
    }).catch(() => renderSsh(box));
  },
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
