// Sending commands to the ground, the arm switch and the utilities card
// (folded away until opened: the tests' tabs hold the commands of a run).
// gs_web.py checks every command again (check_command): the page can only
// make sending easier, never let more through.

import { $, esc, setHTML } from './util.js';
import { t } from './i18n.js';

let armed = false;
let armTimer = null;
const armListeners = new Set();
let toastTimer = null;

export function isArmed() { return armed; }
export function onArm(fn) { armListeners.add(fn); }

export function setArmed(on) {
  armed = on;
  clearTimeout(armTimer);
  if (on) armTimer = setTimeout(() => setArmed(false), 15000);
  for (const fn of armListeners) fn(armed);
}

export function toast(msg, kind) {
  const el = $('toast');
  if (!el) return;
  el.textContent = msg;
  el.className = 'toast ' + (kind || '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.textContent = ''; }, 5000);
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

export function renderUtilities() {
  const box = $('utilities');
  setHTML(box, `
    <summary><h2>${esc(t('utilities'))}</h2><span class="muted">${esc(t('util_hint'))}</span></summary>
    <label class="arm" id="armbox"><input type="checkbox" id="arm"><span>${esc(t('arm'))}</span></label>
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
    </div></div>
    <div class="toast" id="toast"></div>`);
  const arm = $('arm');
  arm.checked = armed;
  $('armbox').classList.toggle('on', armed);
  arm.addEventListener('change', () => setArmed(arm.checked));
}

onArm((on) => {
  const box = $('armbox');
  if (box) { box.classList.toggle('on', on); $('arm').checked = on; }
});
