// Link: signal over time, the protocol's counters, and the rate.

import { esc, fmtAge, setHTML } from '../util.js';
import { t } from '../i18n.js';
import { since } from '../state.js';
import { toast } from '../commands.js';

// The satellite's link, LoRa or ssh (gama-link on the Pi, then the page's
// source): two clicks, the second within CONFIRM_MS, as nothing else on the
// page restarts ttcd.
const CONFIRM_MS = 5000;
let pending = null;      // {to, until}
let switching = false;
let lastLink = null;     // the server's last {current, options, busy}

function redraw(panel) {
  const box = panel.querySelector('#link-buttons');
  if (box && lastLink) setHTML(box, linkButtons(lastLink));
}

function linkButtons(L) {
  return ['lora', 'ssh'].map((to) => {
    const cur = L.current === to;
    const ask = pending && pending.to === to && Date.now() < pending.until;
    const label = switching && pending && pending.to === to ? t('link_busy')
      : ask ? t('link_confirm') : t('link_' + to);
    return `<button class="btn${cur ? ' primary' : ask ? ' danger' : ''}" data-link="${to}"` +
      `${cur || switching || L.busy ? ' disabled' : ''}>${esc(label)}</button>`;
  }).join('');
}

async function switchLink(to, panel) {
  if (!pending || pending.to !== to || Date.now() >= pending.until) {
    pending = { to, until: Date.now() + CONFIRM_MS };
    setTimeout(() => { pending = null; redraw(panel); }, CONFIRM_MS + 50);
    redraw(panel);
    return;
  }
  switching = true;
  redraw(panel);
  try {
    const r = await fetch('/api/link', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ to }),
    });
    const j = await r.json();
    toast(j.why, j.ok ? 'ok' : 'bad');
    panel.querySelector('#link-msg').textContent = j.why;
  } catch (e) {
    toast(t('no_server'), 'bad');
  }
  switching = false;
  pending = null;
  redraw(panel);
}

function spark(canvas, st) {
  const g = canvas.getContext('2d');
  const ratio = window.devicePixelRatio || 1;
  const w = canvas.clientWidth * ratio, h = canvas.clientHeight * ratio;
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  const css = getComputedStyle(document.documentElement);
  g.clearRect(0, 0, w, h);
  g.strokeStyle = css.getPropertyValue('--line');
  g.lineWidth = 1;
  for (let i = 1; i < 4; i++) { g.beginPath(); g.moveTo(0, h * i / 4); g.lineTo(w, h * i / 4); g.stroke(); }
  const line = (arr, lo, hi, color) => {
    if (!arr || arr.length < 2) return;
    g.strokeStyle = color; g.lineWidth = 2 * ratio; g.beginPath();
    arr.forEach((v, i) => {
      const x = w * i / (arr.length - 1);
      const y = h - (Math.min(Math.max(v, lo), hi) - lo) / (hi - lo) * (h - 8) - 4;
      if (i) g.lineTo(x, y); else g.moveTo(x, y);
    });
    g.stroke();
  };
  if (st) {
    line(st.rssi, -120, 0, css.getPropertyValue('--forest'));
    line(st.snr, -20, 20, css.getPropertyValue('--yellow'));
  }
}

// The ground's ping_summary line, as figures: 3/3 acked, min / mean / max.
export function pingSummary(text) {
  let p;
  try { p = JSON.parse(text.replace(/^ping_summary /, '')); } catch (e) { return esc(text); }
  const ms = (v) => (v == null ? '—' : esc(v) + ' ms');
  return `<b>${esc(p.acked ?? '—')}/${esc(p.sent ?? '—')}</b> ${esc(t('acked'))}` +
    (p.failed ? ` · <span class="bad">${esc(p.failed)} ${esc(t('fail'))}</span>` : '') +
    ` · ${esc(t('attempts'))} ${esc(p.attempts ?? '—')}` +
    `<br><span class="muted">min / ${esc(t('mean'))} / max</span> ${ms(p.latency_min_ms)} / ${ms(p.latency_mean_ms)} / ${ms(p.latency_max_ms)}`;
}

export const tab = {
  id: 'link', label: 'tab_link',
  mount(panel) {
    panel.innerHTML = `
      <section class="card" style="grid-column: span 12" id="link-switch" hidden><h2>${esc(t('link_sat'))}</h2>
        <p class="lead">${esc(t('link_lead'))}</p>
        <div class="row" id="link-buttons"></div><p class="muted" id="link-msg"></p></section>
      <section class="card span-7"><h2>${esc(t('link'))}</h2>
        <canvas class="spark" id="link-spark"></canvas>
        <div class="leg"><span style="color:var(--forest)">● RSSI (dBm)</span><span style="color:var(--yellow)">● SNR (dB)</span></div></section>
      <section class="card span-5"><h2>${esc(t('stats'))}</h2><div class="kv" id="link-kv"></div>
        <div class="grp" style="margin-top:14px"><span class="lbl">${esc(t('ping_res'))}</span><div class="ping" id="link-ping"></div></div>
        <div class="grp"><span class="lbl">${esc(t('rate'))}</span><div class="row">
          <button class="btn" data-cmd="rate safe">safe</button>
          <button class="btn" data-cmd="rate nominal">nominal</button>
          <button class="btn" data-cmd="rate fast">fast</button>
          <button class="btn" data-cmd="ping 10">ping ×10</button></div></div></section>`;
    panel.addEventListener('click', (e) => {
      const b = e.target.closest('[data-link]');
      if (b && !b.disabled) switchLink(b.dataset.link, panel);
    });
  },
  update(state, fresh) {
    const st = state.st;
    const a = (st && st.acks) || {};
    const rssi = st && st.rssi.length ? st.rssi[st.rssi.length - 1] : null;
    const snr = st && st.snr.length ? st.snr[st.snr.length - 1] : null;
    const pings = state.log.filter((e) => e.text.startsWith('ping_summary'));
    setHTML(document.getElementById('link-ping'), pings.length ? pingSummary(pings[pings.length - 1].text) : '—');
    setHTML(document.getElementById('link-kv'), [
      [t('last_frame'), st && st.frame_age != null ? fmtAge(since(st.frame_age)) : '—'],
      [t('rssi') + ' / ' + t('snr'), rssi == null ? '—' : `${rssi} dBm / ${snr} dB`],
      [t('acks'), `${a.ok ?? 0} / ${a.bad ?? 0}`],
      [t('latency'), a.latency_ms == null ? '—' : a.latency_ms + ' ms'],
      [t('failed'), a.tc_failed ?? 0], [t('badcrc'), a.rx_bad ?? 0],
      [t('ground_radio'), st && st.ground && st.ground.radio
        ? `${st.source === 'ssh' ? 'ssh' : st.ground.radio} · ${st.ground.profile || ''}` : '—'],
    ].map(([k, v]) => `<span>${esc(k)}</span><span>${esc(v)}</span>`).join(''));
    const L = st && st.link;
    const box = document.getElementById('link-switch');
    box.hidden = !(L && L.options && L.options.length > 1);
    lastLink = L;
    if (!box.hidden) setHTML(document.getElementById('link-buttons'), linkButtons(L));
    if (fresh) spark(document.getElementById('link-spark'), st);
  },
};
