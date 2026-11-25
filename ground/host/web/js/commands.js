// Sending commands to the ground, the arm switch (on every tab that has a
// command needing it: operations, deployment, AOCS; 15 s, counted down) and
// the operations tab's command card.
// gs_web.py checks every command again (check_command): the page can only
// make sending easier, never let more through.

import { $, esc } from './util.js';
import { t } from './i18n.js';

const ARM_S = 15;
let armed = false;
let armTimer = null;
let armTick = null;
let armUntil = 0;
const armListeners = new Set();
let toastTimer = null;

export function isArmed() { return armed; }
export function onArm(fn) { armListeners.add(fn); }

export function setArmed(on) {
  armed = on;
  clearTimeout(armTimer);
  clearInterval(armTick);
  if (on) {
    armUntil = Date.now() + ARM_S * 1000;
    armTimer = setTimeout(() => setArmed(false), ARM_S * 1000);
    armTick = setInterval(paintArm, 250);
  }
  paintArm();
  for (const fn of armListeners) fn(armed);
}

// The switch, as HTML for a tab to place; every copy follows the one state.
export function armBox() {
  return `<label class="arm"><input type="checkbox" data-arm><span>${esc(t('arm'))}</span>` +
    `<b class="arm-left" data-arm-left></b></label>`;
}

function paintArm() {
  const left = Math.max(0, Math.ceil((armUntil - Date.now()) / 1000));
  for (const box of document.querySelectorAll('.arm')) {
    box.classList.toggle('on', armed);
    const cb = box.querySelector('[data-arm]');
    if (cb) cb.checked = armed;
    const l = box.querySelector('[data-arm-left]');
    if (l) l.textContent = armed ? t('armed') + ' · ' + left + ' s' : '';
  }
  // The buttons that need it wait for it, so arming is the first step.
  for (const b of document.querySelectorAll('[data-needs-arm]')) b.disabled = !armed;
}

document.addEventListener('change', (e) => {
  if (e.target.matches && e.target.matches('[data-arm]')) setArmed(e.target.checked);
});
// A tab drawn after arming shows the switch as it is.
export function refreshArm() { paintArm(); }

export function toast(msg, kind) {
  const el = $('toast');
  if (!el) return;
  el.textContent = msg;
  el.className = 'toast show ' + (kind || '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = 'toast'; }, 5000);
}

// POSTs a command; resolves to {ok, why}. Commands that need arming disarm
// once sent.
export async function send(cmd) {
  try {
    const r = await fetch('/api/cmd', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ cmd, armed }),
    });
    const j = await r.json();
    toast(j.ok ? '› ' + cmd + ' · ' + t('sent') : cmd + ': ' + j.why, j.ok ? 'ok' : 'bad');
    if (j.ok && /basic_inter|aocs/.test(cmd)) setArmed(false);
    return j;
  } catch (e) {
    toast(t('no_server'), 'bad');
    return { ok: false, why: 'no connection' };
  }
}

// Any element with data-cmd sends its command when clicked.
export function wire(root) {
  root.addEventListener('click', (e) => {
    const b = e.target.closest('[data-cmd]');
    if (b && root.contains(b) && !b.disabled) send(b.dataset.cmd);
  });
}

// The operations tab's command card: link, rate and snapshots.
export function commandsCard() {
  return `<h2>${esc(t('commands'))}</h2>
    <div class="grp"><span class="lbl">${esc(t('g_link'))}</span><div class="row">
      <button class="btn" data-cmd="ping 3">ping ×3</button>
      <button class="btn" data-cmd="hk">hk</button>
      <button class="btn" data-cmd="stat">stat</button>
      <button class="btn" data-cmd="roster">roster</button>
      <button class="btn" data-cmd="time">${esc(t('b_time'))}</button>
    </div></div>
    <div class="grp"><span class="lbl">${esc(t('g_rate'))}</span><div class="row">
      <button class="btn" data-cmd="rate safe">safe</button>
      <button class="btn" data-cmd="rate nominal">nominal</button>
      <button class="btn" data-cmd="rate fast">fast</button>
    </div></div>
    <div class="grp"><span class="lbl">${esc(t('g_stream'))}</span><div class="row">
      <button class="btn" data-cmd="stream 5">${esc(t('b_stream'))}</button>
      <button class="btn" data-cmd="stop">${esc(t('b_stop'))}</button>
    </div></div>`;
}
