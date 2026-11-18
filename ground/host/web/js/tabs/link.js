// Link: signal over time, the protocol's counters, and the rate.

import { esc, fmtAge, setHTML } from '../util.js';
import { t } from '../i18n.js';
import { since } from '../state.js';

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

export const tab = {
  id: 'link', label: 'tab_link',
  mount(panel) {
    panel.innerHTML = `
      <section class="card span-7"><h2>${esc(t('link'))}</h2>
        <canvas class="spark" id="link-spark"></canvas>
        <div class="leg"><span style="color:var(--forest)">● RSSI (dBm)</span><span style="color:var(--yellow)">● SNR (dB)</span></div></section>
      <section class="card span-5"><h2>${esc(t('stats'))}</h2><div class="kv" id="link-kv"></div>
        <div class="grp" style="margin-top:14px"><span class="lbl">${esc(t('rate'))}</span><div class="row">
          <button class="btn" data-cmd="rate safe">safe</button>
          <button class="btn" data-cmd="rate nominal">nominal</button>
          <button class="btn" data-cmd="rate fast">fast</button>
          <button class="btn" data-cmd="ping 10">ping ×10</button></div></div></section>`;
  },
  update(state, fresh) {
    const st = state.st;
    const a = (st && st.acks) || {};
    const rssi = st && st.rssi.length ? st.rssi[st.rssi.length - 1] : null;
    const snr = st && st.snr.length ? st.snr[st.snr.length - 1] : null;
    const pings = state.log.filter((e) => e.text.startsWith('ping_summary'));
    const last = pings.length ? pings[pings.length - 1].text.replace('ping_summary ', '') : '—';
    setHTML(document.getElementById('link-kv'), [
      [t('last_frame'), st && st.frame_age != null ? fmtAge(since(st.frame_age)) : '—'],
      [t('rssi') + ' / ' + t('snr'), rssi == null ? '—' : `${rssi} dBm / ${snr} dB`],
      [t('acks'), `${a.ok ?? 0} / ${a.bad ?? 0}`],
      [t('latency'), a.latency_ms == null ? '—' : a.latency_ms + ' ms'],
      [t('failed'), a.tc_failed ?? 0], [t('badcrc'), a.rx_bad ?? 0],
      [t('ground_radio'), st && st.ground && st.ground.radio ? `${st.ground.radio} · ${st.ground.profile || ''}` : '—'],
      [t('ping_res'), last],
    ].map(([k, v]) => `<span>${esc(k)}</span><span>${esc(v)}</span>`).join(''));
    if (fresh) spark(document.getElementById('link-spark'), st);
  },
};
