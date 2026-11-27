// The events bar, fixed at the bottom of every tab: the ground's log,
// newest first, appended row by row as it arrives, with a filter by kind.
// Its size button is wired in main.js.

import { $, esc, fmtTime, fmtNum, fmtAge } from './util.js';
import { t } from './i18n.js';

const ROWS_MAX = 400;
// [key, kinds, frames]: a line shows under a filter by its kind or by the
// frame it reports (gs_web.py's `ev`).
const FILTERS = [
  ['all', null, null], ['cmd', ['cmd'], null], ['bad', ['bad'], null], ['mode', ['mode'], null],
  ['adsb', null, ['tracks', 'stat', 'roster']], ['tm', ['tm', 'info', 'ok'], null],
];
let filter = 'all';
let shownN = 0;
// Entries that arrived while the bar was hidden, and whether one failed:
// the folded bar shows them so nothing goes unseen.
let unseen = 0;
let unseenBad = false;

function paintUnseen() {
  const b = $('ev-unseen');
  if (!b) return;
  b.hidden = !unseen;
  b.className = 'ev-unseen' + (unseenBad ? ' bad' : '');
  b.textContent = unseen + ' ' + t('ev_new');
}

export function clearUnseen() { unseen = 0; unseenBad = false; paintUnseen(); }

// ---- what a frame carried, in the reader's language ----------------------
// gs_web.py sends tracks, stat, roster and hk lines with the frame decoded
// (`data`): a short line here, the whole content on a click. Any other line
// is its English text.

const dash = '—';
const val = (v, unit, digits = 0) => (v == null ? dash : fmtNum(v, digits) + (unit ? ' ' + unit : ''));
const yes = (b) => t(b ? 'ev_yes' : 'ev_no');
const kv = (rows) => `<dl class="ev-kv">${rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`).join('')}</dl>`;
const aircraftN = (n) => `${n} ${t(n === 1 ? 'ev_ac1' : 'ev_acn')}`;

function meta(d) {
  const parts = [];
  if (d.seq != null) parts.push(`${t('ev_frame')} #${d.seq}`);
  if (d.rssi != null) parts.push(`${d.rssi} dBm`);
  if (d.snr != null) parts.push(`SNR ${d.snr} dB`);
  if (d.toa_ms != null) parts.push(`${d.toa_ms} ms ${t('ev_on_air')}`);
  return parts.length ? `<p class="ev-meta">${esc(parts.join(' · '))}</p>` : '';
}

function trackWords(r) {
  const alt = r.alt_ft == null ? t('ev_no_alt') : `${fmtNum(r.alt_ft)} ft`;
  return `${r.icao} ${alt}${r.lat == null ? ', ' + t('ev_no_pos') : ''}`;
}

const DETAIL = {
  tracks: {
    line(d) {
      const rs = d.records || [];
      const more = rs.length > 3 ? ` +${rs.length - 3}` : '';
      return aircraftN(d.n ?? rs.length) + (rs.length ? ' · ' + rs.slice(0, 3).map(trackWords).join('; ') + more : '');
    },
    body(d) {
      const head = ['ICAO', t('ev_class'), t('ev_pos'), t('ev_alt'), t('ev_gs'), t('ev_trk'), t('ev_vr'), t('ev_ground'), t('ev_age')];
      const rows = (d.records || []).map((r) => [
        r.icao, t('ev_cls_' + r.cls),
        r.lat == null ? dash : `${fmtNum(r.lat, 4)}, ${fmtNum(r.lon, 4)}`,
        val(r.alt_ft, 'ft'), val(r.gs_kt, 'kt'), r.trk == null ? dash : fmtNum(r.trk) + '°',
        r.vr_fpm == null ? dash : (r.vr_fpm > 0 ? '+' : '') + fmtNum(r.vr_fpm) + ' ft/min',
        yes(r.ground), r.age_s == null ? dash : fmtNum(r.age_s, 1) + ' s']);
      return `<p class="ev-what">${esc(t('ev_x_tracks'))}</p>` +
        `<div class="ev-scroll"><table class="ev-table"><thead><tr>${head.map((h) => `<th>${esc(h)}</th>`).join('')}</tr></thead>` +
        `<tbody>${rows.map((r) => `<tr>${r.map((c) => `<td>${esc(c)}</td>`).join('')}</tr>`).join('')}</tbody></table></div>` + meta(d);
    },
  },
  stat: {
    line(d) {
      return `${val(d.msgs_received)} ${t('ev_msgs_rx')}, ${val(d.msgs_decoded)} ${t('ev_msgs_dec')}, ${aircraftN(d.aircraft ?? 0)}`;
    },
    body(d) {
      const p95 = d.latency_p95_ms == null || d.latency_p95_ms === 65535 ? t('nodata') : fmtNum(d.latency_p95_ms / 1000, 1) + ' s';
      return `<p class="ev-what">${esc(t('ev_x_stat'))}</p>` + kv([
        [t('ev_st_rx'), val(d.msgs_received)], [t('ev_st_dec'), val(d.msgs_decoded)],
        [t('ev_st_ac'), val(d.aircraft)], [t('ev_st_p95'), p95],
        [t('ev_st_tm'), val(d.tm_frames_sent)], [t('ev_st_tc'), val(d.tc_frames_rx)],
        [t('ev_st_tcbad'), val(d.tc_frames_bad)], [t('ev_st_restart'), val(d.dump1090_restarts)],
      ]) + meta(d);
    },
  },
  roster: {
    line(d) {
      const ac = d.aircraft || [];
      return aircraftN(ac.length) + (ac.length ? ' · ' + ac.slice(0, 4).map((a) => a.icao + (a.callsign ? ` (${a.callsign})` : '')).join(', ') : '');
    },
    body(d) {
      return `<p class="ev-what">${esc(t('ev_x_roster'))}</p>` +
        kv((d.aircraft || []).map((a) => [a.icao, a.callsign || dash])) + meta(d);
    },
  },
  hk: {
    line(d) {
      const temp = d.temp_soc_ccel == null ? '' : ` · ${fmtNum(d.temp_soc_ccel / 100, 1)} °C SoC`;
      return `${d.obc_mode || dash} · ${d.link || dash}${temp}`;
    },
    body(d) {
      const c = (v) => (v == null ? dash : fmtNum(v / 100, 1) + ' °C');
      return `<p class="ev-what">${esc(t('ev_x_hk'))}</p>` + kv([
        [t('ev_mode'), d.obc_mode || dash], [t('ev_link'), d.link || dash],
        [t('battery'), d.battery_mv == null ? dash : fmtNum(d.battery_mv / 1000, 2) + ' V'],
        [t('current'), val(d.current_ma, 'mA')], [t('tbat'), c(d.temp_bat_ccel)],
        [t('text'), c(d.temp_ext_ccel)], [t('tsoc'), c(d.temp_soc_ccel)],
        [t('uptime'), d.uptime_s == null ? dash : fmtAge(d.uptime_s)], [t('adsb_msgs'), val(d.adsb_msgs)],
        [t('ev_hf_obc'), yes(d.obc_linked)], [t('ev_hf_adsb'), yes(d.adsb_linked)],
        [t('ev_hf_time'), yes(d.time_synced)], [t('ev_hf_dump'), yes(d.dump1090_up)],
      ]) + meta(d);
    },
  },
};

function row(e) {
  const d = e.data && DETAIL[e.ev];
  let text = esc(e.text), more = '';
  if (d) {
    try {
      text = `<b class="ev-tag">${esc(t('ev_tag_' + e.ev))}</b> ${esc(d.line(e.data))}`;
      more = `<div class="ev-detail" hidden>${d.body(e.data)}</div>`;
    } catch (err) { console.error(err); }
  }
  return `<div class="ev ${esc(e.kind)}${more ? ' more' : ''}" data-k="${esc(e.kind)}" data-ev="${esc(e.ev || '')}">` +
    `<time>${esc(fmtTime(e.t))}</time><span>${text}</span>${more}</div>`;
}

function visible(kind, ev) {
  const [, kinds, frames] = FILTERS.find(([k]) => k === filter);
  if (!kinds && !frames) return true;
  return (kinds && kinds.includes(kind)) || (frames && frames.includes(ev));
}

function applyFilter() {
  for (const el of $('events').querySelectorAll('.ev')) el.hidden = !visible(el.dataset.k, el.dataset.ev);
  for (const b of $('events').querySelectorAll('.filters button')) b.setAttribute('aria-pressed', String(b.dataset.f === filter));
}

export function renderEventsFrame() {
  $('events').innerHTML = `<div class="dock-head"><h2>${esc(t('events'))}</h2>
    <div class="filters">${FILTERS.map(([k]) => `<button type="button" data-f="${k}">${esc(t('f_' + k))}</button>`).join('')}</div>
    <span id="ev-unseen" hidden></span>
    <div class="dock-tools"><button class="tool" type="button" id="ev-size"></button>
    <button class="tool" type="button" id="ev-hide"></button></div></div>
    <div class="list" id="evlist"></div>`;
  $('events').querySelector('.filters').addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (b) { filter = b.dataset.f; applyFilter(); }
  });
  // A line with a frame behind it opens on a click; selecting text inside
  // the open detail does not close it.
  $('evlist').addEventListener('click', (e) => {
    const r = e.target.closest('.ev.more');
    if (!r || e.target.closest('.ev-detail')) return;
    const d = r.querySelector('.ev-detail');
    d.hidden = !d.hidden;
    r.classList.toggle('open', !d.hidden);
  });
  shownN = 0;
}

export function renderEvents(state) {
  const list = $('evlist');
  if (!list) return;
  const fresh = state.log.filter((e) => e.n > shownN);
  if (!fresh.length) return;
  // A reconnect brings the whole recent log again: start over then.
  if (shownN && fresh[0].n !== shownN + 1 && state.st && state.st.full) list.innerHTML = '';
  list.insertAdjacentHTML('afterbegin', fresh.slice().reverse().map(row).join(''));
  shownN = fresh[fresh.length - 1].n;
  if (document.body.classList.contains('dock-hidden')) {
    unseen += fresh.filter((e) => visible(e.kind, e.ev)).length;
    unseenBad = unseenBad || fresh.some((e) => e.kind === 'bad');
    paintUnseen();
  }
  while (list.childElementCount > ROWS_MAX) list.lastElementChild.remove();
  applyFilter();
}
