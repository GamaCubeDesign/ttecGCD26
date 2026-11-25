// OBC: the battery's health and the OBC's state, from the housekeeping
// (every 10 s): the figures now, and the last 30 min that gs_web.py keeps
// (GsState.hk_hist: [t, battery_mv, current_ma, temp_bat, temp_ext, temp_soc,
// soc_pct]); the charge estimated at the ground (GsState.battery, ADR-0024).

import { esc, fmtAge, fmtNum, setHTML } from '../util.js';
import { t } from '../i18n.js';
import { since } from '../state.js';

function fig(label, v, digits, unit) {
  const nd = v == null || !isFinite(v);
  return `<div class="fig"><span class="lbl">${esc(label)}</span>` +
    `<b>${nd ? '—' : esc(fmtNum(v, digits)) + `<small>${esc(unit)}</small>`}</b></div>`;
}

// Each series on its own scale, its range written at its left; time along x.
// series: [{ idx, div, unit, digits, color }]
function chart(canvas, hist, series) {
  const g = canvas.getContext('2d');
  const ratio = window.devicePixelRatio || 1;
  const w = canvas.clientWidth * ratio, h = canvas.clientHeight * ratio;
  if (!w || !h) return;
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  const css = getComputedStyle(document.documentElement);
  g.clearRect(0, 0, w, h);
  g.strokeStyle = css.getPropertyValue('--line');
  g.lineWidth = 1;
  for (let i = 1; i < 4; i++) { g.beginPath(); g.moveTo(0, h * i / 4); g.lineTo(w, h * i / 4); g.stroke(); }
  if (!hist || hist.length < 2) return;
  const t0 = hist[0][0], t1 = hist[hist.length - 1][0];
  const span = Math.max(t1 - t0, 1);
  const pad = 14 * ratio;
  g.font = `${11 * ratio}px ${css.getPropertyValue('--sans')}`;
  series.forEach((s, k) => {
    const vals = hist.map((r) => r[s.idx]).filter((v) => v != null);
    if (!vals.length) return;
    let lo = Math.min(...vals), hi = Math.max(...vals);
    if (hi - lo < s.minSpan) { const m = (hi + lo) / 2; lo = m - s.minSpan / 2; hi = m + s.minSpan / 2; }
    const color = css.getPropertyValue(s.color);
    g.strokeStyle = color; g.lineWidth = 2 * ratio; g.beginPath();
    let pen = false;
    for (const r of hist) {
      const v = r[s.idx];
      if (v == null) { pen = false; continue; }
      const x = (r[0] - t0) / span * w;
      const y = h - pad - (v - lo) / (hi - lo) * (h - 2 * pad);
      if (pen) g.lineTo(x, y); else g.moveTo(x, y);
      pen = true;
    }
    g.stroke();
    g.fillStyle = color;
    const fmt = (v) => fmtNum(v / s.div, s.digits) + ' ' + s.unit;
    g.fillText(fmt(hi), 4 * ratio + k * 90 * ratio, 11 * ratio);
    g.fillText(fmt(lo), 4 * ratio + k * 90 * ratio, h - 3 * ratio);
  });
}

function legend(items) {
  return `<div class="leg">${items.map(([c, l]) => `<span style="color:var(${c})">● ${esc(l)}</span>`).join('')}` +
    `<span class="hint">${esc(t('last_30'))}</span></div>`;
}

export const tab = {
  id: 'obc', label: 'tab_obc',
  mount(panel) {
    panel.innerHTML = `
      <section class="card span-7"><h2>${esc(t('obc_bat'))}</h2>
        <p class="lead">${esc(t('obc_lead'))}</p>
        <div class="figs big" id="obc-bat"></div>
        <canvas class="spark tall" id="obc-bat-chart"></canvas>
        ${legend([['--forest', t('battery') + ' (V)'], ['--yellow', t('current') + ' (mA)']])}</section>
      <section class="card span-5"><h2>${esc(t('obc_temps'))}</h2>
        <div class="figs" id="obc-temps"></div>
        <canvas class="spark tall" id="obc-temp-chart"></canvas>
        ${legend([['--forest', t('tbat')], ['--yellow', t('tsoc')]])}</section>
      <section class="card span-7"><h2>${esc(t('obc_charge'))}</h2>
        <p class="lead">${esc(t('charge_lead'))}</p>
        <div class="figs big" id="obc-charge"></div>
        <p class="small" style="margin:10px 0 0"><button class="btn" type="button" id="obc-fasthk"></button>
          <span class="muted" id="obc-fasthk-note"></span></p>
        <canvas class="spark tall" id="obc-soc-chart"></canvas>
        ${legend([['--forest', t('soc') + ' (%)']])}</section>
      <section class="card span-5"><h2>${esc(t('obc_state'))}</h2><div class="kv" id="obc-kv"></div></section>`;
    // The fast HK (gs_web.py FastHk): the battery's state in ~6 s, for the EPS test.
    this.fastOn = false;
    document.getElementById('obc-fasthk').addEventListener('click', async () => {
      const note = document.getElementById('obc-fasthk-note');
      try {
        const r = await fetch('/api/fast-hk', {
          method: 'POST', headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ on: !this.fastOn }),
        });
        const j = await r.json();
        if (!j.ok) note.textContent = j.why;
      } catch (e) {
        note.textContent = t('no_server');
      }
    });
    this.drawnAt = null;
  },
  show() { this.drawnAt = null; },
  update(state) {
    const st = state.st;
    const hk = st && st.hk;
    const v = hk && hk.battery_mv != null ? hk.battery_mv / 1000 : null;
    const i = hk && hk.current_ma != null ? hk.current_ma : null;
    setHTML(document.getElementById('obc-bat'),
      fig(t('battery'), v, 2, 'V') + fig(t('current'), i, 0, 'mA') +
      fig(t('power_w'), v != null && i != null ? v * i / 1000 : null, 2, 'W') +
      fig(t('tbat'), hk && hk.temp_bat_ccel != null ? hk.temp_bat_ccel / 100 : null, 1, '°C'));
    setHTML(document.getElementById('obc-temps'),
      fig(t('tbat'), hk && hk.temp_bat_ccel != null ? hk.temp_bat_ccel / 100 : null, 1, '°C') +
      fig(t('text'), hk && hk.temp_ext_ccel != null ? hk.temp_ext_ccel / 100 : null, 1, '°C') +
      fig(t('tsoc'), hk && hk.temp_soc_ccel != null ? hk.temp_soc_ccel / 100 : null, 1, '°C'));
    const b = st && st.battery;
    const dur = (s) => (s == null ? null : fmtAge(s));
    const figText = (label, text) =>
      `<div class="fig txt"><span class="lbl">${esc(label)}</span><b>${text == null ? '—' : esc(text)}</b></div>`;
    const charging = b && b.state === 'charging';
    setHTML(document.getElementById('obc-charge'), b ? (
      fig(t('soc'), b.soc_pct, 0, '%') +
      (charging ? figText(t('full_in'), dur(b.full_in_s)) : figText(t('autonomy'), dur(b.autonomy_s))) +
      fig(t('trend'), b.trend_pct_h, 1, '%/h') +
      (charging ? fig(t('charge_in'), b.charge_ma, 0, 'mA') : fig(t('used'), b.used_mah, 0, 'mAh')) +
      `<div class="fig txt"><span class="lbl">${esc(t('b_state'))}</span><b>${esc(t('st_' + b.state))}` +
      (b.state_s != null ? `<br><small>${esc(t('for'))} ${esc(fmtAge(b.state_s))}</small>` : '') + '</b></div>') : '');
    const fh = st && st.fast_hk;
    this.fastOn = !!(fh && fh.on);
    const fb = document.getElementById('obc-fasthk');
    fb.textContent = this.fastOn ? t('fast_hk_stop') : t('fast_hk');
    fb.className = 'btn' + (this.fastOn ? ' primary' : '');
    setHTML(document.getElementById('obc-fasthk-note'), esc(this.fastOn
      ? `${t('fast_hk_on')} ${fmtAge(fh.left_s)}` : t('fast_hk_note')));
    const yn = (b) => (b == null ? '—' : b ? '✓ ' + t('yes') : '✗ ' + t('no'));
    setHTML(document.getElementById('obc-kv'), hk ? [
      [t('obc_mode'), st.mode ? `${st.mode} · ${t('for')} ${fmtAge(since(st.mode_age))}` : '—'],
      [t('hk_age'), fmtAge(since(st.hk_age))],
      [t('uptime'), hk.uptime_s != null ? fmtAge(hk.uptime_s) : '—'],
      [t('time_sync'), yn(hk.time_synced)],
      [t('obc_linked'), yn(hk.obc_linked)],
      [t('adsb_linked'), yn(hk.adsb_linked)],
      [t('dump1090_up'), yn(hk.dump1090_up)],
      [t('soc_v'), b && b.soc_v_pct != null ? fmtNum(b.soc_v_pct, 0) + ' %' : '—'],
      [t('d1_drop'), b && b.diode_v != null ? fmtNum(b.diode_v, 2) + ' V' : '—'],
      [t('r_model'), b ? `${b.r_measured_ohm != null ? fmtNum(b.r_measured_ohm, 2) : '—'} / ${fmtNum(b.r_ohm, 2)} Ω` : '—'],
      [t('noise_mv'), b && b.noise_mv != null ? fmtNum(b.noise_mv, 1) + ' mV' : '—'],
    ].map(([k, x]) => `<span>${esc(k)}</span><span>${esc(x)}</span>`).join('')
      : `<span class="muted">${esc(t('no_hk'))}</span><span></span>`);
    // The charts change only with a new HK.
    const hist = (st && st.hk_hist) || [];
    const last = hist.length ? hist[hist.length - 1][0] : null;
    if (last === this.drawnAt) return;
    this.drawnAt = last;
    chart(document.getElementById('obc-bat-chart'), hist, [
      { idx: 1, div: 1000, unit: 'V', digits: 2, color: '--forest', minSpan: 100 },
      { idx: 2, div: 1, unit: 'mA', digits: 0, color: '--yellow', minSpan: 20 },
    ]);
    chart(document.getElementById('obc-soc-chart'), hist, [
      { idx: 6, div: 1, unit: '%', digits: 0, color: '--forest', minSpan: 5 },
    ]);
    chart(document.getElementById('obc-temp-chart'), hist, [
      { idx: 3, div: 100, unit: '°C', digits: 1, color: '--forest', minSpan: 200 },
      { idx: 5, div: 100, unit: '°C', digits: 1, color: '--yellow', minSpan: 200 },
    ]);
  },
};

